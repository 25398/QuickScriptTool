#include "ocr_engine.h"

#include "base64.h"
#include "image_match.h"
#include "opencv_runtime.h"
#include "utils.h"

#include <urlmon.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>
#include <initializer_list>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#pragma comment(lib, "urlmon.lib")

namespace {

std::wstring QuoteArg(const std::wstring& arg) {
    std::wstring out = L"\"";
    for (wchar_t ch : arg) {
        if (ch == L'"') out += L"\\\"";
        else out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
}

std::wstring OcrHelperScriptPath() {
    return AppDir() + L"\\tools\\paddle_ocr_helper.py";
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE
        || GetLastError() == ERROR_ALREADY_EXISTS;
}

constexpr wchar_t kOcrVenvDir[] = L"C:\\paddle_env\\venv";
constexpr wchar_t kOcrVenvPythonExe[] = L"C:\\paddle_env\\venv\\Scripts\\python.exe";
constexpr wchar_t kOcrBasePythonExe[] = L"C:\\paddle_env\\python312\\python.exe";
constexpr wchar_t kOcrBasePythonDir[] = L"C:\\paddle_env\\python312";
constexpr wchar_t kOcrPythonInstallerName[] = L"python-3.12.10-amd64.exe";
constexpr wchar_t kOcrPythonNugetName[] = L"python.3.12.10.nupkg";
constexpr wchar_t kOcrPythonInstallerUrl[] =
    L"https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe";

const wchar_t* kOcrPythonInstallerUrls[] = {
    L"https://registry.npmmirror.com/-/binary/python/3.12.10/python-3.12.10-amd64.exe",
    L"https://mirrors.huaweicloud.com/python/3.12.10/python-3.12.10-amd64.exe",
    kOcrPythonInstallerUrl,
};

const wchar_t* kOcrPythonNugetUrls[] = {
    L"https://globalcdn.nuget.org/packages/python.3.12.10.nupkg",
    L"https://api.nuget.org/v3-flatcontainer/python/3.12.10/python.3.12.10.nupkg",
};

const wchar_t* kOcrPipIndexes[] = {
    L"https://pypi.tuna.tsinghua.edu.cn/simple",
    L"https://mirrors.aliyun.com/pypi/simple",
    L"https://pypi.org/simple",
};

bool FileExists(const std::wstring& path) {
    return !path.empty()
        && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool FileExistsNonEmpty(const std::wstring& path, ULONGLONG minBytes = 1) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (path.empty()
        || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    ULARGE_INTEGER size{};
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    return size.QuadPart >= minBytes;
}

bool RunProcessCaptureStdout(const std::wstring& commandLine, std::string& stdoutText, DWORD& exitCode);

std::wstring DetectPythonExecutable() {
    return kOcrVenvPythonExe;
}

bool RunProcessWait(const std::wstring& commandLine, DWORD& exitCode, bool showWindow = false) {
    exitCode = 1;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = showWindow ? SW_SHOWDEFAULT : SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
    cmdLine.push_back(L'\0');

    const DWORD creationFlags = showWindow ? 0 : CREATE_NO_WINDOW;
    const BOOL ok = CreateProcessW(
        nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
        creationFlags, nullptr, nullptr, &si, &pi);
    if (!ok) return false;

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool DownloadFileUrl(const std::wstring& url, const std::wstring& destPath, ULONGLONG minBytes = 1) {
    DeleteFileW(destPath.c_str());
    const HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), destPath.c_str(), 0, nullptr);
    if (!SUCCEEDED(hr) || !FileExists(destPath)) return false;
    return FileExistsNonEmpty(destPath, minBytes);
}

bool CopyDirectoryTree(const std::wstring& srcDir, const std::wstring& destDir) {
    if (!FileExists(srcDir)) return false;
    EnsureDirectory(destDir);
    const std::wstring cmd = L"robocopy " + QuoteArg(srcDir) + L" " + QuoteArg(destDir)
        + L" /E /NFL /NDL /NJH /NJS /NC /NS /NP";
    DWORD exitCode = 8;
    if (!RunProcessWait(cmd, exitCode)) return false;
    return exitCode < 8;
}

bool RemovePathTree(const std::wstring& path) {
    if (path.empty() || !FileExists(path)) return true;
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(path), ec);
    return !std::filesystem::exists(std::filesystem::path(path), ec);
}

std::wstring TrimAscii(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r')) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) {
        --end;
    }
    return FromUtf8(value.substr(begin, end - begin));
}

std::wstring ReadVenvCfgHome() {
    const std::filesystem::path cfg(L"C:\\paddle_env\\venv\\pyvenv.cfg");
    std::ifstream in(cfg);
    if (!in) return {};
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 4) continue;
        if (line.compare(0, 4, "home") != 0) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        return TrimAscii(line.substr(eq + 1));
    }
    return {};
}

bool VenvLooksUsable() {
    if (!FileExists(kOcrVenvPythonExe)) return false;
    const std::wstring home = ReadVenvCfgHome();
    if (home.empty()) return false;
    if (home.find(L"WindowsApps") != std::wstring::npos) return false;
    return FileExistsNonEmpty(home + L"\\python.exe", 4096);
}

bool PythonExecutableWorks(const std::wstring& pythonExe) {
    if (!FileExists(pythonExe)) return false;
    DWORD exitCode = 1;
    std::string out;
    const std::wstring cmd = QuoteArg(pythonExe) + L" -c \"import sys; print(sys.version_info[0])\"";
    if (!RunProcessCaptureStdout(cmd, out, exitCode) || exitCode != 0) return false;
    return out.find('3') != std::string::npos;
}

bool WriteVenvCfgForBasePython() {
    EnsureDirectory(kOcrVenvDir);
    const std::filesystem::path cfg(L"C:\\paddle_env\\venv\\pyvenv.cfg");
    std::ofstream out(cfg, std::ios::binary);
    if (!out) return false;
    out << "home = C:\\paddle_env\\python312\n"
        << "include-system-site-packages = false\n"
        << "version = 3.12.10\n"
        << "executable = C:\\paddle_env\\python312\\python.exe\n"
        << "command = C:\\paddle_env\\python312\\python.exe -m venv C:\\paddle_env\\venv\n";
    return static_cast<bool>(out);
}

bool WriteOcrSitecustomize() {
    const std::wstring dir = L"C:\\paddle_env\\venv\\Lib\\site-packages";
    if (!EnsureDirectory(dir)) return false;
    const std::filesystem::path path(dir + L"\\sitecustomize.py");
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const char* body =
        "# Generated by QuickScriptTool OCR installer.\n"
        "import collections\n"
        "import collections.abc\n"
        "import os\n"
        "os.environ.setdefault(\"KMP_DUPLICATE_LIB_OK\", \"TRUE\")\n"
        "os.environ.setdefault(\"PADDLEOCR_HOME\", r\"C:\\paddle_env\\models\")\n"
        "for _name in (\"Mapping\", \"MutableMapping\", \"Sequence\", \"Callable\", "
        "\"Iterable\", \"MutableSequence\"):\n"
        "    if not hasattr(collections, _name) and hasattr(collections.abc, _name):\n"
        "        setattr(collections, _name, getattr(collections.abc, _name))\n";
    out.write(body, std::strlen(body));
    return static_cast<bool>(out);
}

bool InstallPython312FromInstaller(const std::wstring& installerPath, std::wstring& errorOut) {
    errorOut.clear();
    if (!FileExists(installerPath)) {
        errorOut = L"找不到 Python 安装程序";
        return false;
    }

    const std::wstring cmd = QuoteArg(installerPath)
        + L" /passive InstallAllUsers=0 InstallLauncherAllUsers=0 PrependPath=0"
        + L" Include_launcher=0 Include_test=0 Include_doc=0 Include_pip=1"
        + L" AssociateFiles=0 Shortcuts=0 TargetDir=" + QuoteArg(kOcrBasePythonDir);
    DWORD exitCode = 1;
    if (!RunProcessWait(cmd, exitCode, true) || exitCode != 0) {
        errorOut = L"Python 3.12 安装程序执行失败";
        return false;
    }
    if (!PythonExecutableWorks(kOcrBasePythonExe)) {
        errorOut = L"Python 3.12 安装完成但无法启动 python.exe";
        return false;
    }
    return true;
}

std::wstring FindExtractedPythonDir(const std::wstring& extractDir) {
    if (FileExists(extractDir + L"\\tools\\python.exe")) return extractDir + L"\\tools";
    if (FileExists(extractDir + L"\\python.exe")) return extractDir;
    return {};
}

bool DeployPythonFromNuget(std::wstring& errorOut, const OcrInstallProgressFn& onProgress) {
    auto report = [&](int percent, const wchar_t* status) {
        if (onProgress) onProgress(percent, status);
    };

    const std::wstring nupkgPath = L"C:\\paddle_env\\" + std::wstring(kOcrPythonNugetName);
    report(10, L"正在下载独立 Python 3.12...");
    bool downloaded = false;
    for (const wchar_t* url : kOcrPythonNugetUrls) {
        if (DownloadFileUrl(url, nupkgPath, 5ull * 1024ull * 1024ull)) {
            downloaded = true;
            break;
        }
    }
    if (!downloaded) {
        errorOut = L"无法下载 Python 3.12 运行时，请检查网络后重试。";
        return false;
    }

    const std::wstring extractDir = L"C:\\paddle_env\\_py_nupkg";
    RemovePathTree(extractDir);
    EnsureDirectory(extractDir);
    report(14, L"正在解压 Python 3.12...");
    const std::wstring tarCmd = QuoteArg(L"C:\\Windows\\System32\\tar.exe")
        + L" -xf " + QuoteArg(nupkgPath) + L" -C " + QuoteArg(extractDir);
    DWORD tarExit = 1;
    if (!RunProcessWait(tarCmd, tarExit) || tarExit != 0) {
        errorOut = L"解压 Python 运行时失败";
        return false;
    }

    const std::wstring srcDir = FindExtractedPythonDir(extractDir);
    if (srcDir.empty()) {
        errorOut = L"Python 运行时压缩包内容无效";
        return false;
    }

    report(16, L"正在部署 Python 3.12...");
    if (FileExists(kOcrBasePythonDir) && !PythonExecutableWorks(kOcrBasePythonExe)) {
        RemovePathTree(kOcrBasePythonDir);
    }
    if (!CopyDirectoryTree(srcDir, kOcrBasePythonDir) || !PythonExecutableWorks(kOcrBasePythonExe)) {
        errorOut = L"部署 Python 3.12 失败";
        RemovePathTree(extractDir);
        return false;
    }
    RemovePathTree(extractDir);
    return true;
}

bool EnsurePython312Base(std::wstring& errorOut, const OcrInstallProgressFn& onProgress) {
    errorOut.clear();
    if (PythonExecutableWorks(kOcrBasePythonExe)) return true;

    auto report = [&](int percent, const wchar_t* status) {
        if (onProgress) onProgress(percent, status);
    };

    if (FileExists(kOcrBasePythonDir) && !PythonExecutableWorks(kOcrBasePythonExe)) {
        RemovePathTree(kOcrBasePythonDir);
    }

    const std::wstring bundledPortable = AppDir() + L"\\tools\\python312";
    if (FileExists(bundledPortable + L"\\python.exe")) {
        report(10, L"正在部署内置 Python 3.12...");
        if (CopyDirectoryTree(bundledPortable, kOcrBasePythonDir)
            && PythonExecutableWorks(kOcrBasePythonExe)) {
            return true;
        }
    }

    if (DeployPythonFromNuget(errorOut, onProgress)) return true;
    const std::wstring nugetError = errorOut;

    std::wstring installerPath;
    const std::wstring bundledInstaller = AppDir() + L"\\tools\\" + kOcrPythonInstallerName;
    if (FileExistsNonEmpty(bundledInstaller, 10ull * 1024ull * 1024ull)) {
        installerPath = bundledInstaller;
        report(10, L"正在安装内置 Python 3.12...");
        if (InstallPython312FromInstaller(installerPath, errorOut)) return true;
    } else {
        report(10, L"正在下载 Python 3.12 安装程序...");
        installerPath = L"C:\\paddle_env\\" + std::wstring(kOcrPythonInstallerName);
        for (const wchar_t* url : kOcrPythonInstallerUrls) {
            if (DownloadFileUrl(url, installerPath, 20ull * 1024ull * 1024ull)) {
                report(18, L"正在安装 Python 3.12...");
                if (InstallPython312FromInstaller(installerPath, errorOut)) return true;
                break;
            }
        }
    }

    if (errorOut.empty()) {
        errorOut = nugetError.empty()
            ? L"无法安装 Python 3.12，请检查网络连接后重试。"
            : nugetError;
    }
    return false;
}

bool CreateOcrVirtualEnv(std::wstring& errorOut, const OcrInstallProgressFn& onProgress) {
    errorOut.clear();
    auto report = [&](int percent, const wchar_t* status) {
        if (onProgress) onProgress(percent, status);
    };

    if (!EnsurePython312Base(errorOut, onProgress)) return false;

    if (FileExists(kOcrVenvPythonExe)) {
        if (PythonExecutableWorks(kOcrVenvPythonExe)) {
            WriteOcrSitecustomize();
            return true;
        }
        report(20, L"正在修复已损坏的 Python 虚拟环境...");
        if (WriteVenvCfgForBasePython() && PythonExecutableWorks(kOcrVenvPythonExe)) {
            WriteOcrSitecustomize();
            return true;
        }
        report(22, L"正在重建 Python 虚拟环境...");
        const std::wstring backup = std::wstring(kOcrVenvDir) + L"_broken_"
            + std::to_wstring(GetTickCount64());
        std::error_code ec;
        std::filesystem::rename(std::filesystem::path(kOcrVenvDir),
            std::filesystem::path(backup), ec);
        if (ec) RemovePathTree(kOcrVenvDir);
    }

    report(22, L"正在创建 Python 虚拟环境...");
    const std::wstring venvCmd = QuoteArg(kOcrBasePythonExe)
        + L" -m venv " + QuoteArg(kOcrVenvDir);
    DWORD venvExit = 1;
    if (!RunProcessWait(venvCmd, venvExit) || venvExit != 0
        || !PythonExecutableWorks(kOcrVenvPythonExe)) {
        errorOut = L"无法创建 Python 虚拟环境，请重试。";
        return false;
    }
    WriteOcrSitecustomize();
    return true;
}

bool InferenceModelReady(const std::wstring& dir) {
    return FileExistsNonEmpty(dir + L"\\inference.pdmodel", 1024);
}

bool EnsureOcrModels(const OcrInstallProgressFn& onProgress) {
    struct Spec { const wchar_t* rel; };
    const Spec specs[] = {
        {L"det\\ch\\ch_PP-OCRv4_det_infer"},
        {L"rec\\ch\\ch_PP-OCRv4_rec_infer"},
        {L"cls\\ch_ppocr_mobile_v2.0_cls_infer"},
    };
    const std::wstring destRoot = L"C:\\paddle_env\\models\\whl";
    wchar_t profile[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    const std::wstring userProfile = (n > 0 && n < MAX_PATH) ? std::wstring(profile) : std::wstring();

    bool copied = false;
    for (const auto& spec : specs) {
        const std::wstring dest = destRoot + L"\\" + spec.rel;
        if (InferenceModelReady(dest)) continue;
        if (userProfile.empty()) continue;
        const std::wstring src = userProfile + L"\\.paddleocr\\whl\\" + spec.rel;
        if (!InferenceModelReady(src)) continue;
        if (!copied && onProgress) onProgress(28, L"正在复制本地 OCR 模型...");
        copied = true;
        CopyDirectoryTree(src, dest);
    }
    return true;
}

std::wstring TempOcrImagePath() {
    return AppDir() + L"\\temp_ocr_" + std::to_wstring(GetTickCount64()) + L".png";
}

struct RawOcrBitmap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

bool ReadOcrBitmapPixels(HBITMAP bitmap, RawOcrBitmap& out) {
    if (!bitmap) return false;
    BITMAP bm{};
    if (!GetObjectW(bitmap, sizeof(bm), &bm)) return false;
    out.width = bm.bmWidth;
    out.height = bm.bmHeight;
    if (out.width <= 0 || out.height <= 0) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = out.width;
    bi.bmiHeader.biHeight = -out.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4);

    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const int lines = GetDIBits(dc, bitmap, 0, out.height, out.pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines > 0;
}

cv::Mat OcrBitmapToBgr(HBITMAP bitmap) {
    if (!bitmap || !OpenCvAvailable()) return {};
    RawOcrBitmap raw{};
    if (!ReadOcrBitmapPixels(bitmap, raw)) return {};
    const cv::Mat bgra(raw.height, raw.width, CV_8UC4, raw.pixels.data());
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return bgr.clone();
}

bool EncodeHbitmapPng(HBITMAP bitmap, std::vector<uchar>& pngOut) {
    pngOut.clear();
    if (!OpenCvAvailable()) return false;
    const cv::Mat bgr = OcrBitmapToBgr(bitmap);
    if (bgr.empty()) return false;
    return cv::imencode(".png", bgr, pngOut);
}

bool EncodeHbitmapPngBase64(HBITMAP bitmap, std::string& b64Out) {
    b64Out.clear();
    std::vector<uchar> png;
    if (!EncodeHbitmapPng(bitmap, png) || png.empty()) return false;
    b64Out = qst::base64::Encode(png.data(), png.size());
    return !b64Out.empty();
}

bool SaveHbitmapPng(HBITMAP bitmap, const std::wstring& path) {
    std::vector<uchar> png;
    if (!EncodeHbitmapPng(bitmap, png) || png.empty()) return false;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) return false;
    const size_t wrote = fwrite(png.data(), 1, png.size(), fp);
    fclose(fp);
    return wrote == png.size();
}

std::wstring NormalizeOcrSearchText(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == 0x3000)
            continue;
        if (ch >= 0xFF10 && ch <= 0xFF19) ch = static_cast<wchar_t>(L'0' + (ch - 0xFF10));
        else if (ch >= 0xFF21 && ch <= 0xFF3A) ch = static_cast<wchar_t>(L'A' + (ch - 0xFF21));
        else if (ch >= 0xFF41 && ch <= 0xFF5A) ch = static_cast<wchar_t>(L'a' + (ch - 0xFF41));
        out.push_back(ch);
    }
    return out;
}

int LevenshteinDistance(const std::wstring& a, const std::wstring& b) {
    const size_t n = a.size();
    const size_t m = b.size();
    if (n == 0) return static_cast<int>(m);
    if (m == 0) return static_cast<int>(n);
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = (std::min)({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(cur);
    }
    return prev[m];
}

double OcrTextSimilarity(const std::wstring& a, const std::wstring& b) {
    const int denom = static_cast<int>((std::max)(a.size(), b.size()));
    if (denom <= 0) return 1.0;
    const int dist = LevenshteinDistance(a, b);
    return 1.0 - (static_cast<double>(dist) / static_cast<double>(denom));
}

OcrTextLine UnionOcrLines(const OcrTextLine& a, const OcrTextLine& b) {
    OcrTextLine out = a;
    out.text += b.text;
    out.x1 = (std::min)(a.x1, b.x1);
    out.y1 = (std::min)(a.y1, b.y1);
    out.x2 = (std::max)(a.x2, b.x2);
    out.y2 = (std::max)(a.y2, b.y2);
    out.confidence = (std::min)(a.confidence, b.confidence);
    return out;
}

bool RunProcessCaptureStdout(const std::wstring& commandLine, std::string& stdoutText, DWORD& exitCode) {
    stdoutText.clear();
    exitCode = 1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
    cmdLine.push_back(L'\0');

    const BOOL ok = CreateProcessW(
        nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return false;
    }

    char buffer[4096];
    DWORD readBytes = 0;
    for (;;) {
        const BOOL readOk = ReadFile(readPipe, buffer, sizeof(buffer), &readBytes, nullptr);
        if (!readOk || readBytes == 0) break;
        stdoutText.append(buffer, buffer + readBytes);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool AppendJsonEscapedChar(const std::string& json, size_t& pos, std::string& raw);

size_t SkipJsonWhitespace(const std::string& json, size_t pos) {
    while (pos < json.size()
        && (json[pos] == ' ' || json[pos] == '\t'
            || json[pos] == '\n' || json[pos] == '\r')) {
        ++pos;
    }
    return pos;
}

size_t FindJsonArrayAfterKey(const std::string& json, const char* key, size_t start = 0) {
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t keyPos = json.find(quoted, start);
    if (keyPos == std::string::npos) return std::string::npos;

    size_t pos = keyPos + quoted.size();
    pos = SkipJsonWhitespace(json, pos);
    if (pos >= json.size() || json[pos] != ':') return std::string::npos;
    ++pos;
    pos = SkipJsonWhitespace(json, pos);
    if (pos >= json.size() || json[pos] != '[') return std::string::npos;
    return pos + 1;
}

size_t FindJsonStringValueAfterKey(const std::string& json, const char* key, size_t start) {
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t keyPos = json.find(quoted, start);
    if (keyPos == std::string::npos || keyPos >= json.size()) return std::string::npos;

    size_t pos = keyPos + quoted.size();
    pos = SkipJsonWhitespace(json, pos);
    if (pos >= json.size() || json[pos] != ':') return std::string::npos;
    ++pos;
    pos = SkipJsonWhitespace(json, pos);
    return pos;
}

std::wstring ExtractJsonStringField(const std::string& json, const char* key) {
    const size_t pos = FindJsonStringValueAfterKey(json, key, 0);
    if (pos == std::string::npos) return L"";
    if (pos >= json.size() || json[pos] != '"') return L"";
    std::string raw;
    size_t i = pos + 1;
    while (i < json.size()) {
        if (json[i] == '"') break;
        if (!AppendJsonEscapedChar(json, i, raw)) break;
    }
    return FromUtf8(raw);
}

bool ExtractJsonBoolField(const std::string& json, const char* key, bool fallback) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    size_t valueStart = pos + needle.size();
    while (valueStart < json.size()
        && (json[valueStart] == ' ' || json[valueStart] == '\t')) {
        ++valueStart;
    }
    if (json.compare(valueStart, 4, "true") == 0) return true;
    if (json.compare(valueStart, 5, "false") == 0) return false;
    return fallback;
}

void AppendUtf8Codepoint(std::string& raw, unsigned codepoint) {
    if (codepoint <= 0x7F) {
        raw.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        raw.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        raw.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        raw.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        raw.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        raw.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

bool AppendJsonEscapedChar(const std::string& json, size_t& pos, std::string& raw) {
    if (pos >= json.size()) return false;
    const char ch = json[pos++];
    if (ch != '\\' || pos >= json.size()) {
        raw.push_back(ch);
        return true;
    }

    const char next = json[pos++];
    switch (next) {
    case 'n': raw.push_back('\n'); return true;
    case 'r': raw.push_back('\r'); return true;
    case 't': raw.push_back('\t'); return true;
    case '\\': raw.push_back('\\'); return true;
    case '"': raw.push_back('"'); return true;
    case 'u':
        if (pos + 4 <= json.size()) {
            unsigned codepoint = 0;
            for (int i = 0; i < 4; ++i) {
                const char hex = json[pos + i];
                codepoint <<= 4;
                if (hex >= '0' && hex <= '9') codepoint |= static_cast<unsigned>(hex - '0');
                else if (hex >= 'a' && hex <= 'f') codepoint |= static_cast<unsigned>(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F') codepoint |= static_cast<unsigned>(hex - 'A' + 10);
                else return false;
            }
            pos += 4;
            AppendUtf8Codepoint(raw, codepoint);
            return true;
        }
        raw.push_back(next);
        return true;
    default:
        raw.push_back(next);
        return true;
    }
}

std::string ExtractJsonPayload(const std::string& stdoutText) {
    const size_t pos = stdoutText.rfind("{\"success\"");
    if (pos == std::string::npos) {
        const size_t fallback = stdoutText.rfind('{');
        return fallback == std::string::npos ? stdoutText : stdoutText.substr(fallback);
    }
    const size_t end = stdoutText.find_last_of('}');
    if (end != std::string::npos && end >= pos) {
        return stdoutText.substr(pos, end - pos + 1);
    }
    return stdoutText.substr(pos);
}

double ExtractJsonNumberAfter(const std::string& json, size_t pos, double fallback) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
    size_t end = pos;
    while (end < json.size() && (json[end] == '-' || json[end] == '+' || json[end] == '.'
        || (json[end] >= '0' && json[end] <= '9'))) ++end;
    if (end == pos) return fallback;
    try {
        return std::stod(json.substr(pos, end - pos));
    } catch (...) {
        return fallback;
    }
}

std::wstring ExtractJsonStringAfter(const std::string& json, size_t pos) {
    while (pos < json.size() && json[pos] != '"') ++pos;
    if (pos >= json.size()) return L"";
    ++pos;
    std::string raw;
    while (pos < json.size()) {
        if (json[pos] == '"') break;
        if (!AppendJsonEscapedChar(json, pos, raw)) break;
    }
    return FromUtf8(raw);
}

OcrEngineOutput ParseOcrJson(const std::string& jsonRaw) {
    const std::string json = ExtractJsonPayload(jsonRaw);
    OcrEngineOutput output;
    output.success = ExtractJsonBoolField(json, "success", false);
    output.error = ExtractJsonStringField(json, "error");

    size_t pos = FindJsonArrayAfterKey(json, "lines");
    if (pos == std::string::npos) return output;

    while (pos < json.size()) {
        while (pos < json.size() && json[pos] != '{') {
            if (json[pos] == ']') return output;
            ++pos;
        }
        if (pos >= json.size() || json[pos] != '{') break;

        OcrTextLine line;
        const size_t textValuePos = FindJsonStringValueAfterKey(json, "text", pos);
        const size_t objEnd = FindMatchingJsonBrace(json, pos);
        if (objEnd == std::string::npos) break;
        if (textValuePos != std::string::npos && textValuePos < objEnd
            && textValuePos < json.size() && json[textValuePos] == '"') {
            std::string raw;
            size_t i = textValuePos + 1;
            while (i < json.size()) {
                if (json[i] == '"') break;
                if (!AppendJsonEscapedChar(json, i, raw)) break;
            }
            line.text = FromUtf8(raw);
        }
        const auto numAt = [&](const char* key, int& outVal) {
            const std::string quoted = std::string("\"") + key + "\"";
            const size_t keyPos = json.find(quoted, pos);
            if (keyPos == std::string::npos || keyPos > objEnd) return;
            size_t valuePos = keyPos + quoted.size();
            valuePos = SkipJsonWhitespace(json, valuePos);
            if (valuePos >= json.size() || json[valuePos] != ':') return;
            ++valuePos;
            valuePos = SkipJsonWhitespace(json, valuePos);
            outVal = static_cast<int>(ExtractJsonNumberAfter(json, valuePos, 0.0));
        };
        numAt("x1", line.x1);
        numAt("y1", line.y1);
        numAt("x2", line.x2);
        numAt("y2", line.y2);

        const std::string confKey = "\"confidence\":";
        const size_t confPos = json.find(confKey, pos);
        if (confPos != std::string::npos && confPos < objEnd) {
            line.confidence = ExtractJsonNumberAfter(json, confPos + confKey.size(), 0.0);
        }

        if (!line.text.empty()) output.lines.push_back(line);

        pos = objEnd + 1;
        if (pos < json.size() && json[pos] == ']') break;
    }
    return output;
}

HBITMAP CaptureRegionBitmap(int x1, int y1, int x2, int y2,
    HBITMAP frozenScreen, int frozenVirtX, int frozenVirtY) {
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    if (frozenScreen) {
        const int cropX = x1 - frozenVirtX;
        const int cropY = y1 - frozenVirtY;
        const int w = x2 - x1;
        const int h = y2 - y1;
        HDC screenDc = GetDC(nullptr);
        HDC memDc = CreateCompatibleDC(screenDc);
        HBITMAP cropBmp = CreateCompatibleBitmap(screenDc, w, h);
        HGDIOBJ oldCrop = SelectObject(memDc, cropBmp);

        HDC srcDc = CreateCompatibleDC(screenDc);
        HGDIOBJ oldSrc = SelectObject(srcDc, frozenScreen);
        BitBlt(memDc, 0, 0, w, h, srcDc, cropX, cropY, SRCCOPY);
        SelectObject(srcDc, oldSrc);
        DeleteDC(srcDc);

        SelectObject(memDc, oldCrop);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return cropBmp;
    }
    return CaptureScreenRegion(x1, y1, x2, y2);
}

OcrEnvStatus CheckOcrEnvironmentImpl(bool verifyImport) {
    OcrEnvStatus status;
    const std::wstring pythonExe = DetectPythonExecutable();
    if (GetFileAttributesW(pythonExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        status.state = OcrEnvState::NotInstalled;
        status.detail = L"未检测到 Python 虚拟环境";
        return status;
    }
    if (!VenvLooksUsable()) {
        status.state = OcrEnvState::NotInstalled;
        status.detail = L"Python 环境已损坏（例如依赖已卸载的微软商店 Python），请点击安装/修复";
        return status;
    }
    const std::wstring helperPath = OcrHelperScriptPath();
    if (GetFileAttributesW(helperPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        status.state = OcrEnvState::MissingHelper;
        status.detail = L"缺少 OCR 辅助脚本";
        return status;
    }
    if (!verifyImport) {
        status.state = OcrEnvState::Ready;
        return status;
    }
    if (!PythonExecutableWorks(pythonExe)) {
        status.state = OcrEnvState::NotInstalled;
        status.detail = L"Python 环境无法启动，请点击安装/修复";
        return status;
    }
    auto importOk = [&](const wchar_t* snippet) {
        std::string stdoutText;
        DWORD exitCode = 1;
        const std::wstring testCmd = QuoteArg(pythonExe) + L" -c " + QuoteArg(snippet);
        return RunProcessCaptureStdout(testCmd, stdoutText, exitCode) && exitCode == 0;
    };
    if (!importOk(L"from rapidocr import RapidOCR") && !importOk(L"import paddleocr")) {
        status.state = OcrEnvState::MissingDeps;
        status.detail = L"RapidOCR 依赖未完整安装";
        return status;
    }
    status.state = OcrEnvState::Ready;
    return status;
}

std::wstring HostFromUrl(const wchar_t* url) {
    std::wstring u(url ? url : L"");
    const auto scheme = u.find(L"://");
    if (scheme != std::wstring::npos) u = u.substr(scheme + 3);
    const auto slash = u.find(L'/');
    if (slash != std::wstring::npos) u.resize(slash);
    return u;
}

std::wstring SummarizeCmdOutput(const std::string& text, size_t maxChars = 360) {
    if (text.empty()) return {};
    std::string slice = text;
    const auto errPos = slice.find("ERROR");
    if (errPos == std::string::npos) {
        const auto err2 = slice.find("Error");
        if (err2 != std::string::npos && slice.size() > maxChars) {
            slice = slice.substr(err2);
        } else if (slice.size() > maxChars) {
            slice = slice.substr(slice.size() - maxChars);
        }
    } else if (slice.size() - errPos > maxChars) {
        slice = slice.substr(errPos, maxChars);
    } else if (errPos > 0) {
        slice = slice.substr(errPos);
    }
    for (char& ch : slice) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        else if (static_cast<unsigned char>(ch) < 32) ch = ' ';
    }
    while (!slice.empty() && slice.front() == ' ') slice.erase(slice.begin());
    while (!slice.empty() && slice.back() == ' ') slice.pop_back();
    if (slice.size() > maxChars) slice.resize(maxChars);
    return FromUtf8(slice);
}

bool PipInstallRequirements(const std::wstring& pythonExe, const std::wstring& reqPath,
    std::wstring& errorOut, const OcrInstallProgressFn& onProgress) {
    errorOut.clear();
    auto report = [&](int percent, const wchar_t* status) {
        if (onProgress) onProgress(percent, status);
    };

    report(32, L"正在准备 pip...");
    {
        DWORD pipExit = 1;
        std::string pipOut;
        const std::wstring upgradeCmd = QuoteArg(pythonExe)
            + L" -m pip install -U pip setuptools wheel --disable-pip-version-check --timeout 120";
        RunProcessCaptureStdout(upgradeCmd, pipOut, pipExit);
    }

    for (const wchar_t* indexUrl : kOcrPipIndexes) {
        report(40, L"正在下载并安装依赖，请稍候...");
        const std::wstring host = HostFromUrl(indexUrl);
        const std::wstring installCmd = QuoteArg(pythonExe)
            + L" -m pip install -r " + QuoteArg(reqPath)
            + L" --disable-pip-version-check --timeout 180"
            + L" -i " + QuoteArg(indexUrl)
            + L" --trusted-host " + QuoteArg(host);
        std::string installOut;
        DWORD installExit = 1;
        if (RunProcessCaptureStdout(installCmd, installOut, installExit) && installExit == 0) {
            return true;
        }
        errorOut = SummarizeCmdOutput(installOut);
    }
    if (errorOut.empty()) errorOut = L"依赖安装失败，请检查网络连接后重试。";
    else errorOut = L"依赖安装失败：" + errorOut;
    return false;
}

bool RunOcrInstallImpl(std::wstring& messageOut, const OcrInstallProgressFn& onProgress) {
    messageOut.clear();
    auto report = [&](int percent, const wchar_t* status) {
        if (onProgress) onProgress(percent, status);
    };
    report(5, L"正在准备安装...");
    EnsureDirectory(L"C:\\paddle_env");
    EnsureDirectory(L"C:\\paddle_env\\models");
    EnsureDirectory(L"C:\\paddle_env\\models\\whl");

    if (!CreateOcrVirtualEnv(messageOut, onProgress)) {
        return false;
    }
    WriteOcrSitecustomize();
    EnsureOcrModels(onProgress);

    const std::wstring reqPath = AppDir() + L"\\tools\\requirements-ocr.txt";
    if (GetFileAttributesW(reqPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        messageOut = L"缺少依赖清单文件 tools\\requirements-ocr.txt";
        return false;
    }

    report(30, L"正在检查已有依赖...");
    if (CheckOcrEnvironmentImpl(true).state == OcrEnvState::Ready) {
        messageOut = L"文字识别组件已就绪，可直接使用。";
        report(100, L"安装完成");
        return true;
    }

    std::wstring pipError;
    const bool pipOk = PipInstallRequirements(kOcrVenvPythonExe, reqPath, pipError, onProgress);
    WriteOcrSitecustomize();
    report(85, L"正在验证安装...");

    const OcrEnvStatus verified = CheckOcrEnvironmentImpl(true);
    if (verified.state == OcrEnvState::Ready) {
        messageOut = pipOk
            ? L"文字识别组件安装完成，可直接使用。"
            : L"文字识别组件已可用（在线更新依赖未完成，可稍后再次修复）。";
        report(100, L"安装完成");
        return true;
    }

    messageOut = pipOk
        ? (verified.detail.empty()
            ? L"安装完成但验证失败，请尝试点击「安装 / 修复」重试。"
            : verified.detail)
        : pipError;
    return false;
}

struct OcrSessionState {
    std::mutex mu;
    int refCount = 0;
    HANDLE process = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stderrNull = nullptr;
    std::string stdoutBuf;
};

OcrSessionState g_ocrSession;

bool ReadStdoutLine(OcrSessionState& session, std::string& line, DWORD timeoutMs) {
    line.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const size_t nl = session.stdoutBuf.find('\n');
        if (nl != std::string::npos) {
            line = session.stdoutBuf.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            session.stdoutBuf.erase(0, nl + 1);
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (!session.stdoutRead) return false;
        DWORD available = 0;
        if (!PeekNamedPipe(session.stdoutRead, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available > 0) {
            std::vector<char> buf(available);
            DWORD readBytes = 0;
            if (!ReadFile(session.stdoutRead, buf.data(), available, &readBytes, nullptr) || readBytes == 0) {
                return false;
            }
            session.stdoutBuf.append(buf.data(), buf.data() + readBytes);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

bool WriteStdinLine(HANDLE pipe, const std::string& text) {
    std::string payload = text;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
    size_t offset = 0;
    while (offset < payload.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(payload.size() - offset, static_cast<size_t>(32 * 1024)));
        DWORD written = 0;
        if (!WriteFile(pipe, payload.data() + offset, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

void CloseSessionHandles(OcrSessionState& session) {
    if (session.stdinWrite) {
        CloseHandle(session.stdinWrite);
        session.stdinWrite = nullptr;
    }
    if (session.stdoutRead) {
        CloseHandle(session.stdoutRead);
        session.stdoutRead = nullptr;
    }
    if (session.stderrNull) {
        CloseHandle(session.stderrNull);
        session.stderrNull = nullptr;
    }
    if (session.process) {
        CloseHandle(session.process);
        session.process = nullptr;
    }
    session.stdoutBuf.clear();
}

void StopSessionLocked(OcrSessionState& session) {
    if (!session.process) {
        CloseSessionHandles(session);
        return;
    }

    if (session.stdinWrite) {
        WriteStdinLine(session.stdinWrite, "QUIT");
        FlushFileBuffers(session.stdinWrite);
    }

    if (WaitForSingleObject(session.process, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(session.process, 1);
        WaitForSingleObject(session.process, 2000);
    }

    CloseSessionHandles(session);
}

bool StartSessionLocked(OcrSessionState& session, std::wstring& errorOut) {
    errorOut.clear();
    if (session.process) return true;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&stdinRead, &session.stdinWrite, &sa, 0)) {
        errorOut = L"无法创建 OCR 进程管道";
        return false;
    }
    if (!CreatePipe(&session.stdoutRead, &stdoutWrite, &sa, 0)) {
        CloseHandle(stdinRead);
        CloseHandle(session.stdinWrite);
        session.stdinWrite = nullptr;
        errorOut = L"无法创建 OCR 进程管道";
        return false;
    }

    SetHandleInformation(session.stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(session.stdoutRead, HANDLE_FLAG_INHERIT, 0);

    session.stderrNull = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError = session.stderrNull ? session.stderrNull : stdoutWrite;
    si.wShowWindow = SW_HIDE;

    const std::wstring pythonExe = DetectPythonExecutable();
    const std::wstring scriptPath = OcrHelperScriptPath();
    const std::wstring commandLine = QuoteArg(pythonExe) + L" " + QuoteArg(scriptPath) + L" --serve --lang ch";

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
    cmdLine.push_back(L'\0');

    const BOOL ok = CreateProcessW(
        pythonExe.c_str(), cmdLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (!ok) {
        StopSessionLocked(session);
        errorOut = L"无法启动 OCR 服务，请检查依赖是否已安装";
        return false;
    }

    session.process = pi.hProcess;
    CloseHandle(pi.hThread);

    std::string readyLine;
    if (!ReadStdoutLine(session, readyLine, 180000)) {
        StopSessionLocked(session);
        errorOut = L"OCR 服务启动超时";
        return false;
    }
    if (!ExtractJsonBoolField(readyLine, "ready", false)) {
        StopSessionLocked(session);
        errorOut = L"OCR 服务启动失败";
        return false;
    }
    if (!ExtractJsonBoolField(readyLine, "success", true)) {
        StopSessionLocked(session);
        errorOut = ExtractJsonStringField(readyLine, "error");
        if (errorOut.empty()) errorOut = L"OCR 服务启动失败";
        return false;
    }
    return true;
}

std::string EscapeJsonPathUtf8(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '"') out += "\\\"";
        else out.push_back(ch);
    }
    return out;
}

bool SessionRequestLocked(OcrSessionState& session, const std::string& imageB64,
    const std::wstring& imagePath, bool digitsOnly, std::string& jsonOut, std::wstring& errorOut) {
    jsonOut.clear();
    errorOut.clear();
    if (!session.process || !session.stdinWrite || !session.stdoutRead) {
        errorOut = L"OCR 服务未运行";
        return false;
    }

    std::string requestLine = "{\"digits_only\":";
    requestLine += digitsOnly ? "true" : "false";
    if (!imageB64.empty()) {
        requestLine += ",\"image_b64\":\"";
        requestLine += imageB64;
        requestLine += "\"}";
    } else {
        const std::string pathUtf8 = ToUtf8(imagePath);
        requestLine += ",\"image\":\"";
        requestLine += EscapeJsonPathUtf8(pathUtf8);
        requestLine += "\"}";
    }
    if (!WriteStdinLine(session.stdinWrite, requestLine)) {
        errorOut = L"无法向 OCR 服务发送请求";
        return false;
    }
    FlushFileBuffers(session.stdinWrite);

    if (!ReadStdoutLine(session, jsonOut, 120000)) {
        errorOut = L"OCR 服务响应超时";
        StopSessionLocked(session);
        return false;
    }
    return true;
}

OcrEngineOutput RunOcrOnImagePathOneShot(const std::wstring& imagePath, bool digitsOnly) {
    OcrEngineOutput output;
    const std::wstring pythonExe = DetectPythonExecutable();
    const std::wstring scriptPath = OcrHelperScriptPath();
    std::wstring commandLine = QuoteArg(pythonExe) + L" " + QuoteArg(scriptPath)
        + L" --image " + QuoteArg(imagePath) + L" --lang ch";
    if (digitsOnly) commandLine += L" --digits-only";

    std::string stdoutText;
    DWORD exitCode = 1;
    if (!RunProcessCaptureStdout(commandLine, stdoutText, exitCode)) {
        output.error = L"无法启动 OCR 服务，请检查依赖是否已安装";
        return output;
    }
    return ParseOcrJson(stdoutText);
}

OcrEngineOutput RequestOcrOnHbitmap(HBITMAP bmp, bool digitsOnly) {
    OcrEngineOutput output;
    std::string imageB64;
    const bool haveB64 = EncodeHbitmapPngBase64(bmp, imageB64);
    std::wstring imagePath;
    if (!haveB64) {
        imagePath = TempOcrImagePath();
        if (!SaveHbitmapPng(bmp, imagePath) && !SaveBitmapToFile(bmp, imagePath)) {
            output.error = L"无法编码识别截图";
            return output;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_ocrSession.mu);
        if (g_ocrSession.process) {
            std::string jsonOut;
            std::wstring sessionError;
            if (SessionRequestLocked(g_ocrSession, imageB64, imagePath, digitsOnly, jsonOut, sessionError)) {
                output = ParseOcrJson(jsonOut);
            } else {
                output.error = sessionError.empty() ? L"OCR 服务请求失败" : sessionError;
            }
        } else if (g_ocrSession.refCount > 0) {
            output.error = L"OCR 服务不可用";
        } else if (!imagePath.empty()) {
            output = RunOcrOnImagePathOneShot(imagePath, digitsOnly);
        } else {
            imagePath = TempOcrImagePath();
            if (SaveHbitmapPng(bmp, imagePath)) {
                output = RunOcrOnImagePathOneShot(imagePath, digitsOnly);
            } else {
                output.error = L"无法保存临时截图";
            }
        }
    }
    if (!imagePath.empty()) DeleteFileW(imagePath.c_str());
    return output;
}

}  // namespace

std::wstring OcrPythonPath() {
    return kOcrVenvPythonExe;
}

OcrEnvStatus CheckOcrEnvironment(bool verifyImport) {
    return CheckOcrEnvironmentImpl(verifyImport);
}

bool RunOcrInstall(std::wstring& messageOut, OcrInstallProgressFn onProgress) {
    {
        std::lock_guard<std::mutex> lock(g_ocrSession.mu);
        StopSessionLocked(g_ocrSession);
        g_ocrSession.refCount = 0;
    }
    return RunOcrInstallImpl(messageOut, onProgress);
}

void EnsureOcrSession() {
    std::lock_guard<std::mutex> lock(g_ocrSession.mu);
    ++g_ocrSession.refCount;
    if (g_ocrSession.process) return;

    std::wstring error;
    if (!StartSessionLocked(g_ocrSession, error)) {
        if (g_ocrSession.refCount > 0) --g_ocrSession.refCount;
    }
}

void ReleaseOcrSession() {
    std::lock_guard<std::mutex> lock(g_ocrSession.mu);
    if (g_ocrSession.refCount > 0) --g_ocrSession.refCount;
    if (g_ocrSession.refCount > 0) return;
    StopSessionLocked(g_ocrSession);
}

bool IsOcrSessionActive() {
    std::lock_guard<std::mutex> lock(g_ocrSession.mu);
    return g_ocrSession.process != nullptr;
}

OcrEngineOutput ParseOcrEngineJson(const std::string& jsonRaw) {
    return ParseOcrJson(jsonRaw);
}

OcrEngineOutput RunOcrOnScreenRegion(
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP frozenScreen, int frozenVirtX, int frozenVirtY, bool digitsOnly) {
    OcrEngineOutput output;
    const auto start = std::chrono::steady_clock::now();

    HBITMAP bmp = CaptureRegionBitmap(
        searchX1, searchY1, searchX2, searchY2, frozenScreen, frozenVirtX, frozenVirtY);
    if (!bmp) {
        output.error = L"无法截取识别区域";
        return output;
    }

    output = RequestOcrOnHbitmap(bmp, digitsOnly);
    DeleteBitmapHandle(bmp);

    const auto elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    if (!output.success) {
        if (output.error.empty()) output.error = L"文字识别失败";
        output.elapsedMs = elapsedMs;
        return output;
    }

    const int cropW = std::max(1, searchX2 - searchX1);
    const int cropH = std::max(1, searchY2 - searchY1);
    for (auto& line : output.lines) {
        if (digitsOnly && (line.x2 <= line.x1 || line.y2 <= line.y1)) {
            line.x1 = 0;
            line.y1 = 0;
            line.x2 = cropW;
            line.y2 = cropH;
        }
        line.x1 += searchX1;
        line.y1 += searchY1;
        line.x2 += searchX1;
        line.y2 += searchY1;
    }

    output.elapsedMs = elapsedMs;
    return output;
}

OcrEngineOutput RunOcrOnBitmap(HBITMAP bmp, int coordOffsetX, int coordOffsetY, bool digitsOnly) {
    OcrEngineOutput output;
    if (!bmp) {
        output.error = L"无法截取识别区域";
        return output;
    }

    const auto start = std::chrono::steady_clock::now();
    output = RequestOcrOnHbitmap(bmp, digitsOnly);

    const auto elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    if (!output.success) {
        if (output.error.empty()) output.error = L"文字识别失败";
        output.elapsedMs = elapsedMs;
        return output;
    }

    BITMAP bm{};
    int cropW = 1;
    int cropH = 1;
    if (GetObject(bmp, sizeof(bm), &bm)) {
        cropW = std::max(1, static_cast<int>(bm.bmWidth));
        cropH = std::max(1, static_cast<int>(bm.bmHeight));
    }
    for (auto& line : output.lines) {
        if (digitsOnly && (line.x2 <= line.x1 || line.y2 <= line.y1)) {
            line.x1 = 0;
            line.y1 = 0;
            line.x2 = cropW;
            line.y2 = cropH;
        }
        line.x1 += coordOffsetX;
        line.y1 += coordOffsetY;
        line.x2 += coordOffsetX;
        line.y2 += coordOffsetY;
    }

    output.elapsedMs = elapsedMs;
    return output;
}

std::wstring ConcatOcrLines(const OcrEngineOutput& output) {
    std::wstring text;
    for (size_t i = 0; i < output.lines.size(); ++i) {
        if (i > 0) text += L"\n";
        text += output.lines[i].text;
    }
    return text;
}

std::optional<OcrTextLine> FindTextInOcrLines(
    const OcrEngineOutput& output, const std::wstring& target) {
    if (target.empty() || output.lines.empty()) return std::nullopt;

    for (const auto& line : output.lines) {
        if (line.text.find(target) != std::wstring::npos) return line;
    }

    const std::wstring needle = NormalizeOcrSearchText(target);
    if (!needle.empty()) {
        for (const auto& line : output.lines) {
            if (NormalizeOcrSearchText(line.text).find(needle) != std::wstring::npos) return line;
        }
        const size_t n = output.lines.size();
        for (size_t i = 0; i < n; ++i) {
            OcrTextLine acc = output.lines[i];
            std::wstring norm = NormalizeOcrSearchText(acc.text);
            for (size_t j = i + 1; j < n && j < i + 4; ++j) {
                acc = UnionOcrLines(acc, output.lines[j]);
                norm = NormalizeOcrSearchText(acc.text);
                if (norm.find(needle) != std::wstring::npos) return acc;
            }
        }
    }

    if (needle.size() < 2) return std::nullopt;
    constexpr double kMinFuzzy = 0.82;
    const OcrTextLine* best = nullptr;
    double bestScore = kMinFuzzy;
    for (const auto& line : output.lines) {
        const std::wstring hay = NormalizeOcrSearchText(line.text);
        if (hay.empty()) continue;
        const double score = OcrTextSimilarity(hay, needle);
        if (score > bestScore) {
            bestScore = score;
            best = &line;
        }
        if (hay.size() > needle.size()) {
            for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                const double win = OcrTextSimilarity(hay.substr(i, needle.size()), needle);
                if (win > bestScore) {
                    bestScore = win;
                    best = &line;
                }
            }
        }
    }
    if (best) return *best;
    return std::nullopt;
}

OcrVarResult MakeOcrSearchVarResult(const OcrTextLine& line, bool found) {
    OcrVarResult result;
    result.mode = OcrVarMode::Search;
    result.found = found ? 1 : 0;
    if (found) {
        result.topLeftX = line.x1;
        result.topLeftY = line.y1;
        result.bottomRightX = line.x2;
        result.bottomRightY = line.y2;
    }
    return result;
}

OcrVarResult MakeOcrTextVarResult(const std::wstring& text) {
    OcrVarResult result;
    result.mode = OcrVarMode::Text;
    result.text = text;
    return result;
}
