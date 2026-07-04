#include "ocr_engine.h"

#include "image_match.h"
#include "utils.h"

#include <urlmon.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

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

constexpr wchar_t kOcrVenvPythonExe[] = L"C:\\paddle_env\\venv\\Scripts\\python.exe";
constexpr wchar_t kOcrBasePythonExe[] = L"C:\\paddle_env\\python312\\python.exe";
constexpr wchar_t kOcrBasePythonDir[] = L"C:\\paddle_env\\python312";
constexpr wchar_t kOcrPythonInstallerName[] = L"python-3.12.10-amd64.exe";
constexpr wchar_t kOcrPythonInstallerUrl[] =
    L"https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe";

bool FileExists(const std::wstring& path) {
    return !path.empty()
        && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring DetectPythonExecutable() {
    if (FileExists(kOcrVenvPythonExe)) return kOcrVenvPythonExe;
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

bool DownloadFileUrl(const std::wstring& url, const std::wstring& destPath) {
    DeleteFileW(destPath.c_str());
    const HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), destPath.c_str(), 0, nullptr);
    return SUCCEEDED(hr) && FileExists(destPath);
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

bool InstallPython312FromInstaller(const std::wstring& installerPath, std::wstring& errorOut) {
    errorOut.clear();
    if (!FileExists(installerPath)) {
        errorOut = L"找不到 Python 安装程序";
        return false;
    }

    const std::wstring cmd = QuoteArg(installerPath)
        + L" /passive InstallAllUsers=0 PrependPath=0 Include_launcher=0 Include_test=0 Include_doc=0"
        + L" TargetDir=" + QuoteArg(kOcrBasePythonDir);
    DWORD exitCode = 1;
    if (!RunProcessWait(cmd, exitCode, true) || exitCode != 0) {
        errorOut = L"Python 3.12 安装程序执行失败";
        return false;
    }
    if (!FileExists(kOcrBasePythonExe)) {
        errorOut = L"Python 3.12 安装完成但未找到 python.exe";
        return false;
    }
    return true;
}

bool EnsurePython312Base(std::wstring& errorOut, const OcrInstallProgressFn& onProgress) {
    errorOut.clear();
    if (FileExists(kOcrBasePythonExe)) return true;

    auto report = [&](int percent, const wchar_t* status) {
        if (onProgress) onProgress(percent, status);
    };

    const std::wstring bundledPortable = AppDir() + L"\\tools\\python312";
    if (FileExists(bundledPortable + L"\\python.exe")) {
        report(10, L"正在部署内置 Python 3.12...");
        if (CopyDirectoryTree(bundledPortable, kOcrBasePythonDir)
            && FileExists(kOcrBasePythonExe)) {
            return true;
        }
    }

    std::wstring installerPath;
    const std::wstring bundledInstaller = AppDir() + L"\\tools\\" + kOcrPythonInstallerName;
    if (FileExists(bundledInstaller)) {
        installerPath = bundledInstaller;
        report(10, L"正在安装内置 Python 3.12...");
    } else {
        report(10, L"正在下载 Python 3.12...");
        installerPath = L"C:\\paddle_env\\" + std::wstring(kOcrPythonInstallerName);
        if (!DownloadFileUrl(kOcrPythonInstallerUrl, installerPath)) {
            errorOut = L"无法下载 Python 3.12，请检查网络连接后重试。";
            return false;
        }
        report(18, L"正在安装 Python 3.12...");
    }

    return InstallPython312FromInstaller(installerPath, errorOut);
}

bool CreateOcrVirtualEnv(std::wstring& errorOut, const OcrInstallProgressFn& onProgress) {
    errorOut.clear();
    if (FileExists(kOcrVenvPythonExe)) return true;

    auto report = [&](int percent, const wchar_t* status) {
        if (onProgress) onProgress(percent, status);
    };

    if (!EnsurePython312Base(errorOut, onProgress)) return false;

    report(22, L"正在创建 Python 虚拟环境...");
    const std::wstring venvCmd = QuoteArg(kOcrBasePythonExe)
        + L" -m venv " + QuoteArg(L"C:\\paddle_env\\venv");
    DWORD venvExit = 1;
    if (!RunProcessWait(venvCmd, venvExit) || venvExit != 0
        || !FileExists(kOcrVenvPythonExe)) {
        errorOut = L"无法创建 Python 虚拟环境，请重试或联系技术支持。";
        return false;
    }
    return true;
}

std::wstring TempOcrImagePath() {
    return AppDir() + L"\\temp_ocr_" + std::to_wstring(GetTickCount64()) + L".bmp";
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
        const size_t objEnd = json.find('}', pos);
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

        if (objEnd == std::string::npos) break;
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
    const std::wstring testCmd = QuoteArg(pythonExe) + L" -c \"import paddleocr\"";
    std::string stdoutText;
    DWORD exitCode = 1;
    if (!RunProcessCaptureStdout(testCmd, stdoutText, exitCode) || exitCode != 0) {
        status.state = OcrEnvState::MissingDeps;
        status.detail = L"PaddleOCR 依赖未完整安装";
        return status;
    }
    status.state = OcrEnvState::Ready;
    return status;
}

bool RunOcrInstallImpl(std::wstring& messageOut, const OcrInstallProgressFn& onProgress) {
    messageOut.clear();
    auto report = [&](int percent, const wchar_t* status) {
        if (onProgress) onProgress(percent, status);
    };
    report(5, L"正在准备安装...");
    EnsureDirectory(L"C:\\paddle_env");
    EnsureDirectory(L"C:\\paddle_env\\models");

    if (!CreateOcrVirtualEnv(messageOut, onProgress)) {
        return false;
    }

    const std::wstring reqPath = AppDir() + L"\\tools\\requirements-ocr.txt";
    if (GetFileAttributesW(reqPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        messageOut = L"缺少依赖清单文件 tools\\requirements-ocr.txt";
        return false;
    }

    report(30, L"正在下载并安装依赖，请稍候...");
    const std::wstring pipExe = L"C:\\paddle_env\\venv\\Scripts\\pip.exe";
    const std::wstring installCmd = QuoteArg(pipExe)
        + L" install -r " + QuoteArg(reqPath)
        + L" --disable-pip-version-check";
    std::string installOut;
    DWORD installExit = 1;
    if (!RunProcessCaptureStdout(installCmd, installOut, installExit) || installExit != 0) {
        messageOut = L"依赖安装失败，请检查网络连接后重试。";
        return false;
    }
    report(85, L"正在验证安装...");

    const OcrEnvStatus verified = CheckOcrEnvironmentImpl(true);
    if (verified.state != OcrEnvState::Ready) {
        messageOut = verified.detail.empty()
            ? L"安装完成但验证失败，请尝试点击「修复/更新」重试。"
            : verified.detail;
        return false;
    }

    messageOut = L"文字识别组件安装完成，可直接使用。";
    report(100, L"安装完成");
    return true;
}

struct OcrSessionState {
    std::mutex mu;
    int refCount = 0;
    HANDLE process = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stderrNull = nullptr;
};

OcrSessionState g_ocrSession;

bool ReadStdoutLine(HANDLE pipe, std::string& line, DWORD timeoutMs) {
    line.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available > 0) {
            char ch = 0;
            DWORD readBytes = 0;
            if (!ReadFile(pipe, &ch, 1, &readBytes, nullptr) || readBytes == 0) return false;
            if (ch == '\n') return true;
            if (ch != '\r') line.push_back(ch);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    return !line.empty();
}

bool WriteStdinLine(HANDLE pipe, const std::string& text) {
    std::string payload = text;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
    DWORD written = 0;
    return WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) != FALSE;
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
    const std::wstring commandLine = pythonExe + L" " + QuoteArg(scriptPath) + L" --serve --lang ch";

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
    cmdLine.push_back(L'\0');

    const BOOL ok = CreateProcessW(
        nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (!ok) {
        StopSessionLocked(session);
        errorOut = L"无法启动 Python/PaddleOCR，请检查依赖是否已安装";
        return false;
    }

    session.process = pi.hProcess;
    CloseHandle(pi.hThread);

    std::string readyLine;
    if (!ReadStdoutLine(session.stdoutRead, readyLine, 180000)) {
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

bool SessionRequestLocked(OcrSessionState& session, const std::wstring& imagePath,
    bool digitsOnly, std::string& jsonOut, std::wstring& errorOut) {
    jsonOut.clear();
    errorOut.clear();
    if (!session.process || !session.stdinWrite || !session.stdoutRead) {
        errorOut = L"OCR 服务未运行";
        return false;
    }

    std::string requestLine;
    if (digitsOnly) {
        const std::string pathUtf8 = ToUtf8(imagePath);
        requestLine = "{\"image\":\"" + EscapeJsonPathUtf8(pathUtf8) + "\",\"digits_only\":true}";
    } else {
        requestLine = ToUtf8(imagePath);
    }
    if (!WriteStdinLine(session.stdinWrite, requestLine)) {
        errorOut = L"无法向 OCR 服务发送请求";
        return false;
    }
    FlushFileBuffers(session.stdinWrite);

    if (!ReadStdoutLine(session.stdoutRead, jsonOut, 120000)) {
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
    std::wstring commandLine = pythonExe + L" " + QuoteArg(scriptPath)
        + L" --image " + QuoteArg(imagePath) + L" --lang ch";
    if (digitsOnly) commandLine += L" --digits-only";

    std::string stdoutText;
    DWORD exitCode = 1;
    if (!RunProcessCaptureStdout(commandLine, stdoutText, exitCode)) {
        output.error = L"无法启动 Python/PaddleOCR，请检查依赖是否已安装";
        return output;
    }
    return ParseOcrJson(stdoutText);
}

}  // namespace

std::wstring OcrPythonPath() {
    return kOcrVenvPythonExe;
}

OcrEnvStatus CheckOcrEnvironment(bool verifyImport) {
    return CheckOcrEnvironmentImpl(verifyImport);
}

bool RunOcrInstall(std::wstring& messageOut, OcrInstallProgressFn onProgress) {
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

    const std::wstring imagePath = TempOcrImagePath();
    if (!SaveBitmapToFile(bmp, imagePath)) {
        DeleteBitmapHandle(bmp);
        output.error = L"无法保存临时截图";
        return output;
    }
    DeleteBitmapHandle(bmp);

    {
        std::lock_guard<std::mutex> lock(g_ocrSession.mu);
        if (g_ocrSession.process) {
            std::string jsonOut;
            std::wstring sessionError;
            if (SessionRequestLocked(g_ocrSession, imagePath, digitsOnly, jsonOut, sessionError)) {
                output = ParseOcrJson(jsonOut);
            } else {
                output.error = sessionError.empty() ? L"OCR 服务请求失败" : sessionError;
            }
        } else if (g_ocrSession.refCount > 0) {
            output.error = L"OCR 服务不可用";
        } else {
            output = RunOcrOnImagePathOneShot(imagePath, digitsOnly);
        }
    }
    DeleteFileW(imagePath.c_str());

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
    if (target.empty()) return std::nullopt;
    for (const auto& line : output.lines) {
        if (line.text.find(target) != std::wstring::npos) return line;
    }
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
