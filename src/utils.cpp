// ── 通用工具函数实现 ──────────────────────────────────────────
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <system_error>
#include <unordered_set>

#include <mmsystem.h>
#include <shlobj.h>
#pragma comment(lib, "winmm.lib")

// ── 字符串处理 ────────────────────────────────────────────────────
std::wstring Trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

// ── 路径工具 ──────────────────────────────────────────────────────
std::wstring AppDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

std::wstring NormalizeDirPathForCompare(std::wstring path) {
    path = Trim(path);
    for (auto& ch : path) {
        if (ch == L'/') ch = L'\\';
    }
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L' '))
        path.pop_back();
    return path;
}

bool PathIsUnderRoot(const std::wstring& path, const std::wstring& root) {
    const std::wstring p = NormalizeDirPathForCompare(path);
    const std::wstring r = NormalizeDirPathForCompare(root);
    if (p.empty() || r.empty() || p.size() < r.size()) return false;
    if (_wcsnicmp(p.c_str(), r.c_str(), r.size()) != 0) return false;
    if (p.size() == r.size()) return true;
    return p[r.size()] == L'\\';
}

bool InstallDirNeedsRoamingWebView2Data(const std::wstring& exeDir,
    const std::wstring& programFiles, const std::wstring& programFilesX86) {
    return PathIsUnderRoot(exeDir, programFiles)
        || PathIsUnderRoot(exeDir, programFilesX86);
}

namespace {
constexpr wchar_t kProductRegKey[] = L"Software\\QuickScriptTool";
constexpr wchar_t kWebView2UserDataName[] = L"WebView2UserData";
constexpr wchar_t kWebView2FetchDataName[] = L"WebView2FetchData";

std::wstring ResolveRoamingOrSidecar(const std::wstring& exeDir,
    const std::wstring& programFiles, const std::wstring& programFilesX86,
    const std::wstring& localAppData, const wchar_t* leaf) {
    if (InstallDirNeedsRoamingWebView2Data(exeDir, programFiles, programFilesX86)
        && !Trim(localAppData).empty()) {
        return NormalizeDirPathForCompare(localAppData) + L"\\QuickScriptTool\\" + leaf;
    }
    return NormalizeDirPathForCompare(exeDir) + L"\\" + leaf;
}

std::wstring QuerySpecialFolder(int csidl) {
    wchar_t buf[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf)))
        return {};
    return buf;
}

std::wstring EnsureDirectoryPath(const std::wstring& dir) {
    if (dir.empty()) return dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}
}  // namespace

std::wstring ResolveWebView2UserDataDir(const std::wstring& exeDir,
    const std::wstring& programFiles, const std::wstring& programFilesX86,
    const std::wstring& localAppData) {
    return ResolveRoamingOrSidecar(exeDir, programFiles, programFilesX86,
        localAppData, kWebView2UserDataName);
}

std::wstring ResolveWebView2FetchDataDir(const std::wstring& exeDir,
    const std::wstring& programFiles, const std::wstring& programFilesX86,
    const std::wstring& localAppData) {
    return ResolveRoamingOrSidecar(exeDir, programFiles, programFilesX86,
        localAppData, kWebView2FetchDataName);
}

std::wstring WebView2UserDataDir() {
    return EnsureDirectoryPath(ResolveWebView2UserDataDir(AppDir(),
        QuerySpecialFolder(CSIDL_PROGRAM_FILES),
        QuerySpecialFolder(CSIDL_PROGRAM_FILESX86),
        QuerySpecialFolder(CSIDL_LOCAL_APPDATA)));
}

std::wstring WebView2FetchDataDir() {
    return EnsureDirectoryPath(ResolveWebView2FetchDataDir(AppDir(),
        QuerySpecialFolder(CSIDL_PROGRAM_FILES),
        QuerySpecialFolder(CSIDL_PROGRAM_FILESX86),
        QuerySpecialFolder(CSIDL_LOCAL_APPDATA)));
}

void RecordLastRunAppDir() {
    const std::wstring dir = AppDir();
    if (dir.empty()) return;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kProductRegKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS || !key) {
        return;
    }
    RegSetValueExW(key, L"LastRunDir", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(dir.c_str()),
        static_cast<DWORD>((dir.size() + 1) * sizeof(wchar_t)));
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        const std::wstring exe = exePath;
        RegSetValueExW(key, L"LastRunExe", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(exe.c_str()),
            static_cast<DWORD>((exe.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
}

std::wstring AppStartupSoundFilePath() {
    return AppDir() + L"\\startup.wav";
}

std::wstring AppFinishSoundFilePath() {
    return AppDir() + L"\\finish.wav";
}

bool IsPlayableWavFile(const std::wstring& path) {
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 12) {
        CloseHandle(h);
        return false;
    }
    unsigned char hdr[12]{};
    DWORD n = 0;
    const BOOL ok = ReadFile(h, hdr, 12, &n, nullptr);
    CloseHandle(h);
    return ok && n == 12
        && std::memcmp(hdr, "RIFF", 4) == 0
        && std::memcmp(hdr + 8, "WAVE", 4) == 0;
}

namespace {
void PlayWavOrFallbackBeep(const std::wstring& path) {
    // SND_NODEFAULT：自定义文件失败时不要再播一遍系统音，由下面统一 MessageBeep。
    if (IsPlayableWavFile(path)
        && PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
        return;
    }
    MessageBeep(MB_OK);
}
}

void PlayAppStartupSound() {
    PlayWavOrFallbackBeep(AppStartupSoundFilePath());
}

void PlayAppFinishSound() {
    PlayWavOrFallbackBeep(AppFinishSoundFilePath());
}

std::wstring ExpandEnvironmentVars(const std::wstring& text) {
    if (text.find(L'%') == std::wstring::npos) return text;
    wchar_t buf[32768]{};
    const DWORD n = ExpandEnvironmentStringsW(text.c_str(), buf, 32768);
    if (n > 0 && n <= 32768) {
        // n 含结尾 NUL；未定义的 %VAR% 会原样保留在结果中
        return std::wstring(buf, n - 1);
    }
    return text;
}

std::wstring ScriptsDir() { return AppDir() + L"\\scripts"; }
std::wstring RecordingsDir() { return AppDir() + L"\\recordings"; }

std::wstring LibraryKindDir(const std::wstring& kind) {
    if (kind == L"macro" || kind == L"scripts") return ScriptsDir();
    if (kind == L"rec" || kind == L"recordings") return RecordingsDir();
    if (kind == L"sched") return AppDir() + L"\\library\\sched";
    if (kind == L"ai") return AppDir() + L"\\library\\ai";
    return AppDir() + L"\\library\\" + kind;
}

void EnsureScriptsDir() {
    CreateDirectoryW(ScriptsDir().c_str(), nullptr);
    CreateDirectoryW(RecordingsDir().c_str(), nullptr);
}

void EnsureLibraryKindDir(const std::wstring& kind) {
    if (kind == L"macro" || kind == L"scripts" || kind == L"rec" || kind == L"recordings") {
        EnsureScriptsDir();
        return;
    }
    CreateDirectoryW((AppDir() + L"\\library").c_str(), nullptr);
    CreateDirectoryW(LibraryKindDir(kind).c_str(), nullptr);
}

std::wstring NormalizeRelativeFolder(std::wstring folder) {
    folder = Trim(folder);
    for (auto& ch : folder) {
        if (ch == L'\\') ch = L'/';
    }
    while (!folder.empty() && (folder.front() == L'/' || folder.front() == L' '))
        folder.erase(folder.begin());
    while (!folder.empty() && (folder.back() == L'/' || folder.back() == L' '))
        folder.pop_back();
    // 折叠重复斜杠
    std::wstring out;
    out.reserve(folder.size());
    for (size_t i = 0; i < folder.size(); ++i) {
        if (folder[i] == L'/' && !out.empty() && out.back() == L'/') continue;
        out.push_back(folder[i]);
    }
    return out;
}

bool IsSafeRelativeFolder(const std::wstring& folder) {
    const std::wstring f = NormalizeRelativeFolder(folder);
    if (f.empty()) return true;
    if (f.size() >= 2 && f[1] == L':') return false;
    if (f.find(L"..") != std::wstring::npos) return false;
    static const wchar_t* kSkip[] = { L"images", L"WebView2Fixed", L"WebView2UserData" };
    size_t start = 0;
    while (start <= f.size()) {
        const auto slash = f.find(L'/', start);
        const std::wstring seg = (slash == std::wstring::npos)
            ? f.substr(start) : f.substr(start, slash - start);
        if (seg.empty() || seg == L"." || seg == L"..") return false;
        for (wchar_t ch : seg) {
            if (wcschr(L"<>:\"|?*", ch)) return false;
        }
        for (auto* skip : kSkip) {
            if (_wcsicmp(seg.c_str(), skip) == 0) return false;
        }
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    return true;
}

bool EnsureRelativeFolder(const std::wstring& rootDir, const std::wstring& folder) {
    if (!IsSafeRelativeFolder(folder)) return false;
    const std::wstring norm = NormalizeRelativeFolder(folder);
    if (norm.empty()) {
        CreateDirectoryW(rootDir.c_str(), nullptr);
        return true;
    }
    std::wstring cur = rootDir;
    CreateDirectoryW(cur.c_str(), nullptr);
    size_t start = 0;
    while (start <= norm.size()) {
        const auto slash = norm.find(L'/', start);
        const std::wstring seg = (slash == std::wstring::npos)
            ? norm.substr(start) : norm.substr(start, slash - start);
        if (!seg.empty()) {
            cur += L"\\" + seg;
            if (!CreateDirectoryW(cur.c_str(), nullptr)) {
                const DWORD e = GetLastError();
                if (e != ERROR_ALREADY_EXISTS) {
                    const DWORD attr = GetFileAttributesW(cur.c_str());
                    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
                        return false;
                }
            }
        }
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    return true;
}

namespace {
bool IsReservedScriptDirName(const std::wstring& name) {
    return _wcsicmp(name.c_str(), L"images") == 0
        || _wcsicmp(name.c_str(), L".") == 0
        || _wcsicmp(name.c_str(), L"..") == 0;
}

void EnumerateScriptJsonFilesRec(const std::wstring& rootDir, const std::wstring& relFolder,
    std::vector<ScriptFileEntry>& out) {
    WIN32_FIND_DATAW fd{};
    const std::wstring abs = relFolder.empty() ? rootDir : (rootDir + L"\\" + relFolder);
    const std::wstring pattern = abs + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0
            || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        const std::wstring name = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (IsReservedScriptDirName(name)) continue;
            const std::wstring child = relFolder.empty() ? name : (relFolder + L"\\" + name);
            EnumerateScriptJsonFilesRec(rootDir, child, out);
            continue;
        }
        const size_t n = name.size();
        if (n < 5 || _wcsicmp(name.c_str() + (n - 5), L".json") != 0) continue;
        ScriptFileEntry e;
        e.path = abs + L"\\" + name;
        e.fileName = name;
        e.folder = NormalizeRelativeFolder(relFolder);
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void EnumerateRelativeFoldersRec(const std::wstring& rootDir, const std::wstring& relFolder,
    std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd{};
    const std::wstring abs = relFolder.empty() ? rootDir : (rootDir + L"\\" + relFolder);
    const std::wstring pattern = abs + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0
            || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        const std::wstring name = fd.cFileName;
        if (IsReservedScriptDirName(name)) continue;
        const std::wstring childWin = relFolder.empty() ? name : (relFolder + L"\\" + name);
        out.push_back(NormalizeRelativeFolder(childWin));
        EnumerateRelativeFoldersRec(rootDir, childWin, out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
}  // namespace

void EnumerateScriptJsonFiles(const std::wstring& rootDir, std::vector<ScriptFileEntry>& out) {
    EnumerateScriptJsonFilesRec(rootDir, L"", out);
}

std::wstring LibraryJsonFileName(std::wstring name) {
    name = Trim(std::move(name));
    if (name.empty()) return {};
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    if (name.size() < 5 || _wcsicmp(name.c_str() + name.size() - 5, L".json") != 0)
        name += L".json";
    return name;
}

bool FindScriptJsonByFileName(const std::wstring& rootDir, const std::wstring& fileName,
    std::wstring& outPath) {
    outPath.clear();
    const std::wstring want = LibraryJsonFileName(fileName);
    if (want.empty() || rootDir.empty()) return false;
    std::vector<ScriptFileEntry> files;
    EnumerateScriptJsonFiles(rootDir, files);
    for (const auto& f : files) {
        if (_wcsicmp(f.fileName.c_str(), want.c_str()) == 0) {
            outPath = f.path;
            return true;
        }
    }
    return false;
}

std::wstring StripExtendedPathPrefix(std::wstring p) {
    if (p.size() >= 4 && wcsncmp(p.c_str(), L"\\\\?\\", 4) == 0) {
        p.erase(0, 4);
        if (p.size() >= 4 && _wcsnicmp(p.c_str(), L"UNC\\", 4) == 0)
            p = L"\\\\" + p.substr(4);
    }
    for (auto& ch : p) {
        if (ch == L'/') ch = L'\\';
    }
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

std::wstring WinFullPath(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (n == 0) return StripExtendedPathPrefix(path);
    std::wstring buf(n, L'\0');
    n = GetFullPathNameW(path.c_str(), n, buf.data(), nullptr);
    if (n == 0) return StripExtendedPathPrefix(path);
    buf.resize(n);
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return StripExtendedPathPrefix(buf);
}

bool PathIsUnderDir(const std::wstring& path, const std::wstring& dir) {
    if (path.empty() || dir.empty()) return false;
    const std::wstring f = WinFullPath(path);
    const std::wstring d = WinFullPath(dir);
    if (f.empty() || d.empty() || f.size() <= d.size()) return false;
    if (_wcsnicmp(f.c_str(), d.c_str(), d.size()) != 0) return false;
    return f[d.size()] == L'\\';
}

bool LibraryFileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool ResolveLibraryScriptPath(const std::wstring& pathOrName, std::wstring& outPath) {
    outPath.clear();
    const std::wstring cand = Trim(pathOrName);
    if (cand.empty()) return false;

    const bool hasSep = cand.find(L'\\') != std::wstring::npos
        || cand.find(L'/') != std::wstring::npos;
    const bool exists = LibraryFileExists(cand);
    const bool inScripts = PathIsUnderDir(cand, ScriptsDir());
    const bool inRec = PathIsUnderDir(cand, RecordingsDir());

    if (exists && (inScripts || inRec)) {
        const std::wstring full = WinFullPath(cand);
        outPath = full.empty() ? cand : full;
        return true;
    }

    const std::wstring name = LibraryJsonFileName(cand);
    if (name.empty()) return false;
    const bool preferRec = hasSep && inRec && !inScripts;
    if (preferRec) {
        if (FindScriptJsonByFileName(RecordingsDir(), name, outPath)) return true;
        if (FindScriptJsonByFileName(ScriptsDir(), name, outPath)) return true;
    } else {
        if (FindScriptJsonByFileName(ScriptsDir(), name, outPath)) return true;
        if (FindScriptJsonByFileName(RecordingsDir(), name, outPath)) return true;
    }
    return false;
}

std::wstring NormalizeLibraryPathKey(const std::wstring& path) {
    std::error_code ec;
    const auto canon = std::filesystem::weakly_canonical(path, ec);
    std::wstring s = (!ec && !canon.empty()) ? canon.wstring() : path;
    for (auto& ch : s) {
        if (ch == L'/') ch = L'\\';
    }
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

bool LibraryPathsEqual(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return _wcsicmp(NormalizeLibraryPathKey(a).c_str(), NormalizeLibraryPathKey(b).c_str()) == 0;
}

bool RelocatePathUnderDir(std::wstring& path, const std::wstring& oldDir, const std::wstring& newDir) {
    if (path.empty() || oldDir.empty() || newDir.empty()) return false;
    const std::wstring p = NormalizeLibraryPathKey(path);
    const std::wstring o = NormalizeLibraryPathKey(oldDir);
    const std::wstring n = NormalizeLibraryPathKey(newDir);
    if (o.empty() || p.size() <= o.size()) return false;
    if (_wcsnicmp(p.c_str(), o.c_str(), o.size()) != 0) return false;
    if (p[o.size()] != L'\\') return false;
    path = n + p.substr(o.size());
    return true;
}

void EnumerateRelativeFolders(const std::wstring& rootDir, std::vector<std::wstring>& out) {
    EnumerateRelativeFoldersRec(rootDir, L"", out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

std::wstring NowText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04d/%02d/%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::wstring TimestampName() {
    using namespace std::chrono;
    const auto ts = duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();
    // 仅时间戳；业务前缀由调用方加（鼠标宏- / 键鼠录制- / 任务-），避免叠成「键鼠录制-鼠标宏-…」
    return std::to_wstring(ts);
}

std::wstring StripJsonExtension(std::wstring name) {
    if (name.size() >= 5
        && _wcsicmp(name.c_str() + name.size() - 5, L".json") == 0) {
        name.resize(name.size() - 5);
    }
    return name;
}

// ── 窗口文本操作 ──────────────────────────────────────────────────
std::wstring GetText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    // 分配 len+1 个字符以确保 GetWindowTextW 写入的空终止符有足够空间
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(len)); // 去掉尾部的空终止符
    return text;
}

void SetText(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return "";
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), out.data(), len);
    return out;
}

// ── 数值转换 ──────────────────────────────────────────────────────
int ToInt(HWND edit, int fallback) {
    try {
        const auto text = Trim(GetText(edit));
        return text.empty() ? fallback : std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

double ToDouble(HWND edit, double fallback) {
    try {
        const auto text = Trim(GetText(edit));
        return text.empty() ? fallback : std::stod(text);
    } catch (...) {
        return fallback;
    }
}

std::wstring F3(double value) {
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(3) << value;
    return ss.str();
}

// ── JSON 转义 ─────────────────────────────────────────────────────
std::wstring EscapeJson(const std::wstring& value) {
    std::wstringstream out;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': out << L"\\\\"; break;
        case L'\"': out << L"\\\""; break;
        case L'\n': out << L"\\n";  break;
        case L'\r': out << L"\\r";  break;
        case L'\t': out << L"\\t";  break;
        default:
            if (ch < 0x20) {
                // 其余控制字符必须 \uXXXX 转义，否则不是合法 JSON
                wchar_t buf[8];
                swprintf_s(buf, 8, L"\\u%04x", static_cast<unsigned>(ch));
                out << buf;
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

std::wstring UnescapeJson(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'\\' && i + 1 < value.size()) {
            switch (value[i + 1]) {
            case L'n': out.push_back(L'\n'); ++i; break;
            case L'r': out.push_back(L'\r'); ++i; break;
            case L't': out.push_back(L'\t'); ++i; break;
            case L'\\': out.push_back(L'\\'); ++i; break;
            case L'\"': out.push_back(L'\"'); ++i; break;
            default: out.push_back(value[i]); break;
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

namespace {

size_t FindJsonStringEnd(const std::wstring& src, size_t quotePos) {
    for (size_t i = quotePos + 1; i < src.size(); ++i) {
        if (src[i] == L'\\' && i + 1 < src.size()) {
            ++i;
            continue;
        }
        if (src[i] == L'"') return i;
    }
    return std::wstring::npos;
}

size_t FindJsonStringEnd(const std::string& src, size_t quotePos) {
    for (size_t i = quotePos + 1; i < src.size(); ++i) {
        if (src[i] == '\\' && i + 1 < src.size()) {
            ++i;
            continue;
        }
        if (src[i] == '"') return i;
    }
    return std::string::npos;
}

template <typename StringT, typename CharT>
size_t FindMatchingJsonDelimImpl(const StringT& src, size_t openPos, CharT openCh, CharT closeCh) {
    if (openPos >= src.size() || src[openPos] != openCh) return StringT::npos;
    int depth = 0;
    for (size_t i = openPos; i < src.size(); ++i) {
        if (src[i] == CharT('"')) {
            const size_t end = FindJsonStringEnd(src, i);
            if (end == StringT::npos) return StringT::npos;
            i = end;
            continue;
        }
        if (src[i] == openCh) ++depth;
        else if (src[i] == closeCh) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return StringT::npos;
}

size_t FindTopLevelJsonKeyColonImpl(const std::wstring& src, const std::wstring& key) {
    if (key.empty()) return std::wstring::npos;
    size_t lastColon = std::wstring::npos;
    int objDepth = 0;
    int arrDepth = 0;
    bool expectKey = false;
    bool inStr = false;
    bool esc = false;
    for (size_t i = 0; i < src.size(); ++i) {
        const wchar_t c = src[i];
        if (inStr) {
            if (esc) { esc = false; continue; }
            if (c == L'\\') { esc = true; continue; }
            if (c == L'"') inStr = false;
            continue;
        }
        if (c == L'"') {
            if (expectKey && objDepth == 1 && arrDepth == 0) {
                const size_t end = FindJsonStringEnd(src, i);
                if (end == std::wstring::npos) break;
                const std::wstring k = UnescapeJson(src.substr(i + 1, end - i - 1));
                size_t j = end + 1;
                while (j < src.size() && (src[j] == L' ' || src[j] == L'\t'
                    || src[j] == L'\n' || src[j] == L'\r')) {
                    ++j;
                }
                if (j < src.size() && src[j] == L':' && k == key)
                    lastColon = j;
                expectKey = false;
                i = end;
                continue;
            }
            inStr = true;
            continue;
        }
        if (c == L'{') { ++objDepth; expectKey = true; continue; }
        if (c == L'}') { if (objDepth > 0) --objDepth; expectKey = false; continue; }
        if (c == L'[') { ++arrDepth; expectKey = false; continue; }
        if (c == L']') { if (arrDepth > 0) --arrDepth; expectKey = false; continue; }
        if (c == L',') {
            expectKey = (objDepth >= 1 && arrDepth == 0);
            continue;
        }
    }
    return lastColon;
}

}  // namespace

size_t FindTopLevelJsonKeyColon(const std::wstring& src, const std::wstring& key) {
    return FindTopLevelJsonKeyColonImpl(src, key);
}

size_t FindMatchingJsonBrace(const std::wstring& src, size_t openPos) {
    return FindMatchingJsonDelimImpl(src, openPos, L'{', L'}');
}

size_t FindMatchingJsonBrace(const std::string& src, size_t openPos) {
    return FindMatchingJsonDelimImpl(src, openPos, '{', '}');
}

size_t FindMatchingJsonBracket(const std::string& src, size_t openPos) {
    return FindMatchingJsonDelimImpl(src, openPos, '[', ']');
}

size_t FindMatchingJsonBracket(const std::wstring& src, size_t openPos) {
    return FindMatchingJsonDelimImpl(src, openPos, L'[', L']');
}

std::vector<std::wstring> ExtractJsonStringArray(const std::wstring& src, const std::wstring& key) {
    std::vector<std::wstring> out;
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return out;
    const auto colon = src.find(L':', pos);
    if (colon == std::wstring::npos) return out;
    size_t tok = colon + 1;
    while (tok < src.size() && (src[tok] == L' ' || src[tok] == L'\t'
        || src[tok] == L'\n' || src[tok] == L'\r')) {
        ++tok;
    }
    if (tok >= src.size() || src[tok] != L'[') return out;
    const auto rb = FindMatchingJsonBracket(src, tok);
    if (rb == std::wstring::npos) return out;
    size_t i = tok + 1;
    while (i < rb) {
        while (i < rb && (src[i] == L' ' || src[i] == L'\t' || src[i] == L'\n'
            || src[i] == L'\r' || src[i] == L',')) {
            ++i;
        }
        if (i >= rb || src[i] != L'"') break;
        ++i;
        std::wstring val;
        for (; i < rb; ++i) {
            const wchar_t c = src[i];
            if (c == L'\\') {
                val.push_back(c);
                if (i + 1 < rb) val.push_back(src[++i]);
                continue;
            }
            if (c == L'"') {
                ++i;
                break;
            }
            val.push_back(c);
        }
        val = UnescapeJson(val);
        out.push_back(std::move(val));
    }
    return out;
}

std::vector<int> ExtractJsonIntArray(const std::wstring& src, const std::wstring& key) {
    std::vector<int> out;
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return out;
    const auto colon = src.find(L':', pos);
    if (colon == std::wstring::npos) return out;
    size_t tok = colon + 1;
    while (tok < src.size() && (src[tok] == L' ' || src[tok] == L'\t'
        || src[tok] == L'\n' || src[tok] == L'\r')) {
        ++tok;
    }
    if (tok >= src.size() || src[tok] != L'[') return out;
    const auto rb = FindMatchingJsonBracket(src, tok);
    if (rb == std::wstring::npos) return out;
    size_t i = tok + 1;
    while (i < rb) {
        while (i < rb && (src[i] == L' ' || src[i] == L'\t' || src[i] == L'\n'
            || src[i] == L'\r' || src[i] == L',')) {
            ++i;
        }
        if (i >= rb) break;
        if (src[i] == L'"') {
            ++i;
            bool esc = false;
            for (; i < rb; ++i) {
                if (esc) {
                    esc = false;
                    continue;
                }
                if (src[i] == L'\\') {
                    esc = true;
                    continue;
                }
                if (src[i] == L'"') {
                    ++i;
                    break;
                }
            }
            continue;
        }
        if (i + 4 <= rb && src.compare(i, 4, L"true") == 0) {
            out.push_back(1);
            i += 4;
            continue;
        }
        if (i + 5 <= rb && src.compare(i, 5, L"false") == 0) {
            out.push_back(0);
            i += 5;
            continue;
        }
        wchar_t* endPtr = nullptr;
        const long v = wcstol(src.c_str() + static_cast<ptrdiff_t>(i), &endPtr, 10);
        if (endPtr && endPtr != src.c_str() + static_cast<ptrdiff_t>(i)) {
            out.push_back(static_cast<int>(v));
            i = static_cast<size_t>(endPtr - src.c_str());
            continue;
        }
        ++i;
    }
    return out;
}

std::vector<std::wstring> ExtractJsonActionBlocks(const std::wstring& content) {
    std::vector<std::wstring> blocks;
    const auto actionsKey = content.find(L"\"actions\"");
    if (actionsKey == std::wstring::npos) return blocks;
    const auto arrayStart = content.find(L'[', actionsKey);
    if (arrayStart == std::wstring::npos) return blocks;

    size_t pos = arrayStart + 1;
    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == L' ' || content[pos] == L'\n'
            || content[pos] == L'\r' || content[pos] == L'\t' || content[pos] == L',')) {
            ++pos;
        }
        if (pos >= content.size() || content[pos] == L']') break;
        if (content[pos] != L'{') {
            if (content[pos] == L'"') {
                const size_t end = FindJsonStringEnd(content, pos);
                pos = (end == std::wstring::npos) ? content.size() : end + 1;
            } else {
                ++pos;
            }
            continue;
        }
        const auto objEnd = FindMatchingJsonBrace(content, pos);
        if (objEnd == std::wstring::npos) break;
        blocks.push_back(content.substr(pos, objEnd - pos + 1));
        pos = objEnd + 1;
    }
    return blocks;
}

// ── 文件操作 ──────────────────────────────────────────────────────
std::wstring ReadAll(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return L"";
    std::stringstream ss;
    ss << file.rdbuf();
    std::string bytes = ss.str();
    if (bytes.size() >= 3
        && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    return FromUtf8(bytes);
}

std::wstring ExtractString(const std::wstring& src,
                            const std::wstring& key) {
    const auto colon = FindTopLevelJsonKeyColon(src, key);
    if (colon == std::wstring::npos) return L"";
    const auto first = src.find(L'\"', colon + 1);
    if (first == std::wstring::npos) return L"";
    const size_t second = FindJsonStringEnd(src, first);
    if (second == std::wstring::npos) return L"";
    return UnescapeJson(src.substr(first + 1, second - first - 1));
}

double ExtractNumber(const std::wstring& src,
                      const std::wstring& key, double fallback) {
    const auto colon = FindTopLevelJsonKeyColon(src, key);
    if (colon == std::wstring::npos) return fallback;
    const auto end = src.find_first_of(L",}]\n", colon + 1);
    try {
        return std::stod(Trim(src.substr(colon + 1, end - colon - 1)));
    } catch (...) {
        return fallback;
    }
}

bool ExtractBool(const std::wstring& src, const std::wstring& key, bool fallback) {
    const auto colon = FindTopLevelJsonKeyColon(src, key);
    if (colon == std::wstring::npos) return fallback;
    size_t i = colon + 1;
    while (i < src.size() && (src[i] == L' ' || src[i] == L'\t'
        || src[i] == L'\n' || src[i] == L'\r')) {
        ++i;
    }
    if (i < src.size() && src.compare(i, 4, L"true") == 0) return true;
    if (i < src.size() && src.compare(i, 5, L"false") == 0) return false;
    const auto end = src.find_first_of(L",}]\n", colon + 1);
    try {
        return std::stod(Trim(src.substr(colon + 1, end - colon - 1))) != 0.0;
    } catch (...) {
        return fallback;
    }
}

int CountActionsInJson(const std::wstring& content) {
    // 与 ExtractJsonActionBlocks 同一套扫描，但不拷贝每个动作对象。
    // 1.5 万步录制若按 blocks.size() 会在列表刷新时分配上万个 wstring，拖死回放线程。
    const auto actionsKey = content.find(L"\"actions\"");
    if (actionsKey == std::wstring::npos) return 0;
    const auto arrayStart = content.find(L'[', actionsKey);
    if (arrayStart == std::wstring::npos) return 0;

    int n = 0;
    size_t pos = arrayStart + 1;
    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == L' ' || content[pos] == L'\n'
            || content[pos] == L'\r' || content[pos] == L'\t' || content[pos] == L',')) {
            ++pos;
        }
        if (pos >= content.size() || content[pos] == L']') break;
        if (content[pos] != L'{') {
            if (content[pos] == L'"') {
                const size_t end = FindJsonStringEnd(content, pos);
                pos = (end == std::wstring::npos) ? content.size() : end + 1;
            } else {
                ++pos;
            }
            continue;
        }
        const auto objEnd = FindMatchingJsonBrace(content, pos);
        if (objEnd == std::wstring::npos) break;
        ++n;
        pos = objEnd + 1;
    }
    return n;
}

std::wstring UpdateJsonStringField(const std::wstring& content,
                                    const std::wstring& key,
                                    const std::wstring& value) {
    const auto pos = content.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return content;
    const auto colon = content.find(L':', pos);
    if (colon == std::wstring::npos) return content;
    const auto firstQuote = content.find(L'\"', colon + 1);
    if (firstQuote == std::wstring::npos) return content;
    const size_t secondQuote = FindJsonStringEnd(content, firstQuote);
    if (secondQuote == std::wstring::npos) return content;
    std::wstring out = content;
    out.replace(firstQuote + 1, secondQuote - firstQuote - 1, EscapeJson(value));
    return out;
}

// ── 图片管理 ──────────────────────────────────────────────────────
std::wstring FindImagesDir() {
    return ScriptsDir() + L"\\images";
}

void EnsureFindImagesDir() {
    CreateDirectoryW(ScriptsDir().c_str(), nullptr);
    CreateDirectoryW(FindImagesDir().c_str(), nullptr);
}

bool IsPathInImageDir(const std::wstring& path) {
    if (path.empty()) return false;
    wchar_t fullPath[MAX_PATH]{};
    wchar_t fullDir[MAX_PATH]{};
    if (GetFullPathNameW(path.c_str(), MAX_PATH, fullPath, nullptr) == 0) return false;
    if (GetFullPathNameW(FindImagesDir().c_str(), MAX_PATH, fullDir, nullptr) == 0) return false;
    const size_t dlen = wcslen(fullDir);
    if (_wcsnicmp(fullPath, fullDir, dlen) != 0) return false;
    return fullPath[dlen] == L'\\' || fullPath[dlen] == L'\0';
}

static bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return true;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    return false;
}

std::wstring ResolveImagePath(const std::wstring& stored) {
    if (stored.empty()) return L"";
    std::wstring candidate;
    if (IsAbsolutePath(stored)) {
        candidate = stored;
    } else {
        std::wstring normalized = stored;
        for (wchar_t& ch : normalized) {
            if (ch == L'/') ch = L'\\';
        }
        // 相对路径禁止 .. 穿越
        if (normalized.find(L"..") != std::wstring::npos) return L"";
        if (normalized.rfind(L"images\\", 0) == 0) {
            candidate = ScriptsDir() + L"\\" + normalized;
        } else if (normalized.find(L'\\') == std::wstring::npos) {
            candidate = FindImagesDir() + L"\\" + normalized;
        } else {
            candidate = ScriptsDir() + L"\\" + normalized;
        }
    }
    wchar_t fullBuf[MAX_PATH]{};
    if (GetFullPathNameW(candidate.c_str(), MAX_PATH, fullBuf, nullptr) == 0) return L"";

    auto fileOk = [](const wchar_t* p) {
        return p && p[0] && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    };
    auto recoverEatenTemplateName = [](std::wstring name) -> std::wstring {
        name.erase(std::remove(name.begin(), name.end(), L'\t'), name.end());
        auto swapPrefix = [&](const wchar_t* bad, const wchar_t* good) -> std::wstring {
            const size_t n = wcslen(bad);
            if (name.size() > n && _wcsnicmp(name.c_str(), bad, n) == 0) {
                return std::wstring(good) + name.substr(n);
            }
            return L"";
        };
        std::wstring r = swapPrefix(L"imagestemplate_", L"template_");
        if (!r.empty()) return r;
        return swapPrefix(L"imagesemplate_", L"template_");
    };
    if (!fileOk(fullBuf)) {
        std::wstring cur = fullBuf;
        const auto slash = cur.find_last_of(L"\\/");
        const std::wstring dir = slash == std::wstring::npos ? L"" : cur.substr(0, slash + 1);
        const std::wstring name = slash == std::wstring::npos ? cur : cur.substr(slash + 1);
        const std::wstring recoveredName = recoverEatenTemplateName(name);
        if (!recoveredName.empty()) {
            const std::wstring recovered = dir + recoveredName;
            wchar_t recoveredFull[MAX_PATH]{};
            if (GetFullPathNameW(recovered.c_str(), MAX_PATH, recoveredFull, nullptr) != 0
                && fileOk(recoveredFull)
                && (IsAbsolutePath(stored) || IsPathInImageDir(recoveredFull))) {
                return recoveredFull;
            }
        }
    }

    if (IsAbsolutePath(stored)) {
        // 绝对路径：规范化后返回（旧脚本可能指向库外文件）；桥接读图另做 images 目录门闩
        return fullBuf;
    }
    // 相对路径必须落在 images 下
    if (!IsPathInImageDir(fullBuf)) return L"";
    return fullBuf;
}

std::wstring ImagePathForJson(const std::wstring& absolutePath) {
    if (absolutePath.empty()) return L"";
    std::wstring normalized = absolutePath;
    for (wchar_t& ch : normalized) {
        if (ch == L'/') ch = L'\\';
    }
    const std::wstring imgDir = FindImagesDir();
    if (_wcsnicmp(normalized.c_str(), imgDir.c_str(), imgDir.size()) == 0) {
        const wchar_t* rest = normalized.c_str() + imgDir.size();
        if (*rest == L'\\') ++rest;
        if (*rest == L'\0') return L"";
        std::wstring restStr(rest);
        for (wchar_t& ch : restStr) {
            if (ch == L'\\') ch = L'/';
        }
        return L"images/" + restStr;
    }
    const auto slash = normalized.find_last_of(L"\\/");
    const std::wstring fileName = slash == std::wstring::npos ? normalized : normalized.substr(slash + 1);
    return fileName.empty() ? L"" : L"images/" + fileName;
}

std::wstring EnsureImageInLibrary(const std::wstring& path) {
    if (path.empty()) return L"";
    const std::wstring resolved = ResolveImagePath(path);
    if (IsPathInImageDir(resolved) &&
        GetFileAttributesW(resolved.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return resolved;
    }
    const std::wstring source = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ? path : resolved;
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) return resolved;

    EnsureFindImagesDir();
    const auto slash = source.find_last_of(L"\\/");
    std::wstring fileName = slash == std::wstring::npos ? source : source.substr(slash + 1);
    if (fileName.empty()) return resolved;

    std::wstring dest = FindImagesDir() + L"\\" + fileName;
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const auto dot = fileName.find_last_of(L'.');
        const std::wstring stem = dot == std::wstring::npos ? fileName : fileName.substr(0, dot);
        const std::wstring ext = dot == std::wstring::npos ? L"" : fileName.substr(dot);
        dest = FindImagesDir() + L"\\" + stem + L"_" + std::to_wstring(GetTickCount()) + ext;
    }
    if (CopyFileW(source.c_str(), dest.c_str(), FALSE)) return dest;
    return resolved;
}

std::unordered_set<std::wstring> CollectImagePathsFromJson(const std::wstring& jsonContent) {
    std::unordered_set<std::wstring> paths;
    const auto blocks = ExtractJsonActionBlocks(jsonContent);
    for (const auto& block : blocks) {
        const auto type = ExtractString(block, L"type");
        const auto imgPath = ExtractString(block, L"imagePath");
        if (!imgPath.empty()) {
            if (type == L"findImage" || type == L"watchImage" || type == L"multiMatch") {
                paths.insert(ResolveImagePath(imgPath));
            } else if ((type == L"mouseDrag" || type == L"getColor"
                || type == L"colorMatch" || type == L"findColor")
                && ExtractBool(block, L"imageLocate", false)) {
                paths.insert(ResolveImagePath(imgPath));
            } else if (type == L"textRecognition"
                && ExtractNumber(block, L"ocrRegionByImage", 0) != 0) {
                paths.insert(ResolveImagePath(imgPath));
            }
        }
        if (type == L"multiMatch") {
            const auto mmPaths = ExtractJsonStringArray(block, L"imagePaths");
            const auto mmUse = ExtractJsonIntArray(block, L"imageUseVars");
            const bool fallbackUse = ExtractNumber(block, L"imageUseVar", 0) != 0;
            for (size_t i = 0; i < mmPaths.size(); ++i) {
                const bool useVar = i < mmUse.size() ? mmUse[i] != 0 : (i == 0 && fallbackUse);
                if (useVar || mmPaths[i].empty()) continue;
                paths.insert(ResolveImagePath(mmPaths[i]));
            }
        }
        const auto recordedCapturePath = ExtractString(block, L"recordedCapturePath");
        if (!recordedCapturePath.empty()) {
            paths.insert(ResolveImagePath(recordedCapturePath));
        }
        const auto aiTargetImagePath = ExtractString(block, L"aiTargetImagePath");
        if (!aiTargetImagePath.empty()
            && (type == L"aiImageAnalysis" || type == L"aiActionExecute")) {
            paths.insert(ResolveImagePath(aiTargetImagePath));
        }
    }
    return paths;
}

namespace {

bool ImportedImageFileNameMatches(const std::wstring& storedPath,
    const std::wstring& zipEntryFileName,
    const std::wstring& destFileName) {
    if (storedPath.empty()) return false;
    const auto slash = storedPath.find_last_of(L"\\/");
    const std::wstring base = (slash == std::wstring::npos)
        ? storedPath : storedPath.substr(slash + 1);
    return _wcsicmp(base.c_str(), zipEntryFileName.c_str()) == 0
        || (!destFileName.empty() && _wcsicmp(base.c_str(), destFileName.c_str()) == 0);
}

void ReplaceJsonStringFieldValue(std::wstring& content,
    const wchar_t* field,
    const std::wstring& oldValue,
    const std::wstring& newValue) {
    if (!field || oldValue.empty()) return;
    const std::wstring prefix = std::wstring(L"\"") + field + L"\": \"";
    const std::wstring key = prefix + EscapeJson(oldValue) + L"\"";
    const auto pos = content.find(key);
    if (pos == std::wstring::npos) return;
    content.replace(pos + prefix.size(), EscapeJson(oldValue).size(), EscapeJson(newValue));
}

}  // namespace

void RemapImportedImagePathInScriptJson(std::wstring& content,
    const std::wstring& zipEntryFileName,
    const std::wstring& newRelPath) {
    if (zipEntryFileName.empty() || newRelPath.empty()) return;
    const auto destSlash = newRelPath.find_last_of(L"\\/");
    const std::wstring destFileName = (destSlash == std::wstring::npos)
        ? newRelPath : newRelPath.substr(destSlash + 1);

    const auto blocks = ExtractJsonActionBlocks(content);
    for (const auto& block : blocks) {
        const auto type = ExtractString(block, L"type");
        const bool remapImagePath = type == L"findImage"
            || type == L"watchImage"
            || type == L"multiMatch"
            || (type == L"textRecognition"
                && ExtractNumber(block, L"ocrRegionByImage", 0) != 0);
        if (remapImagePath) {
            const auto oldPath = ExtractString(block, L"imagePath");
            if (ImportedImageFileNameMatches(oldPath, zipEntryFileName, destFileName)) {
                ReplaceJsonStringFieldValue(content, L"imagePath", oldPath, newRelPath);
            }
        }
        if (type == L"multiMatch") {
            for (const auto& oldPath : ExtractJsonStringArray(block, L"imagePaths")) {
                if (!ImportedImageFileNameMatches(oldPath, zipEntryFileName, destFileName)) continue;
                const std::wstring quotedOld = L"\"" + EscapeJson(oldPath) + L"\"";
                const std::wstring quotedNew = L"\"" + EscapeJson(newRelPath) + L"\"";
                const auto p = content.find(quotedOld);
                if (p != std::wstring::npos)
                    content.replace(p, quotedOld.size(), quotedNew);
            }
        }
        if (type == L"aiImageAnalysis" || type == L"aiActionExecute") {
            const auto oldPath = ExtractString(block, L"aiTargetImagePath");
            if (ImportedImageFileNameMatches(oldPath, zipEntryFileName, destFileName)) {
                ReplaceJsonStringFieldValue(content, L"aiTargetImagePath", oldPath, newRelPath);
            }
        }
        {
            const auto oldPath = ExtractString(block, L"recordedCapturePath");
            if (ImportedImageFileNameMatches(oldPath, zipEntryFileName, destFileName)) {
                ReplaceJsonStringFieldValue(content, L"recordedCapturePath", oldPath, newRelPath);
            }
        }
    }
}

std::unordered_set<std::wstring> CollectAllReferencedImages() {
    std::unordered_set<std::wstring> allPaths;
    const std::vector<std::wstring> dirs = {ScriptsDir(), RecordingsDir()};
    for (const auto& dir : dirs) {
        std::vector<ScriptFileEntry> files;
        EnumerateScriptJsonFiles(dir, files);
        for (const auto& f : files) {
            const auto content = ReadAll(f.path);
            if (content.empty()) continue;
            auto imgPaths = CollectImagePathsFromJson(content);
            allPaths.insert(imgPaths.begin(), imgPaths.end());
        }
    }
    return allPaths;
}

int CleanOrphanImages() {
    const auto imgDir = FindImagesDir();
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = imgDir + L"\\*.*";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;
    const auto referenced = CollectAllReferencedImages();
    int deleted = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring filePath = imgDir + L"\\" + fd.cFileName;
        bool isReferenced = referenced.find(filePath) != referenced.end();
        if (!isReferenced) {
            for (const auto& ref : referenced) {
                if (_wcsicmp(ref.c_str(), filePath.c_str()) == 0) {
                    isReferenced = true;
                    break;
                }
            }
        }
        if (!isReferenced && DeleteFileW(filePath.c_str())) ++deleted;
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return deleted;
}

void DeleteUnreferencedImagesOfScript(const std::wstring& scriptPath) {
    if (scriptPath.empty()) return;
    const auto content = ReadAll(scriptPath);
    if (content.empty()) return;
    const auto scriptImages = CollectImagePathsFromJson(content);
    if (scriptImages.empty()) return;
    // 检查其他脚本是否引用了这些图片
    const auto allReferenced = CollectAllReferencedImages();
    // 注意：CollectAllReferencedImages 会包含当前脚本的引用（因为文件还在），
    // 但实际上我们在删除脚本文件之后才调用此函数，所以文件已经不存在了。
    // 这里传入的是路径，但脚本可能已被删除。为了安全，我们重新收集所有引用（排除当前脚本）。
    // 由于当前脚本文件可能已被删除，CollectAllReferencedImages 已经不会包含它的引用了。
    // 所以我们直接检查 allReferenced 中是否包含这些图片即可。
    // 但如果脚本文件尚未删除，我们需要排除当前脚本的引用。
    
    // 为安全起见，手动排除当前脚本的引用后检查
    std::unordered_set<std::wstring> otherRefs;
    const std::vector<std::wstring> dirs = {ScriptsDir(), RecordingsDir()};
    for (const auto& dir : dirs) {
        std::vector<ScriptFileEntry> files;
        EnumerateScriptJsonFiles(dir, files);
        for (const auto& f : files) {
            if (_wcsicmp(f.path.c_str(), scriptPath.c_str()) == 0) continue;
            const auto c = ReadAll(f.path);
            if (!c.empty()) {
                auto imgs = CollectImagePathsFromJson(c);
                otherRefs.insert(imgs.begin(), imgs.end());
            }
        }
    }
    for (const auto& img : scriptImages) {
        if (otherRefs.find(img) == otherRefs.end()) {
            DeleteFileW(img.c_str());
        }
    }
}

// ── ZIP 压缩包操作（stored 方式）────────────────────────────────

#pragma pack(push, 1)
struct ZipLocalFileHeader {
    uint32_t signature = 0x04034b50;
    uint16_t versionNeeded = 20;
    uint16_t flags = 0;
    uint16_t compression = 0; // 0 = stored
    uint16_t modTime = 0;
    uint16_t modDate = 0;
    uint32_t crc32 = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint16_t fileNameLen = 0;
    uint16_t extraLen = 0;
};

struct ZipCentralDirHeader {
    uint32_t signature = 0x02014b50;
    uint16_t versionMadeBy = 20;
    uint16_t versionNeeded = 20;
    uint16_t flags = 0;
    uint16_t compression = 0;
    uint16_t modTime = 0;
    uint16_t modDate = 0;
    uint32_t crc32 = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint16_t fileNameLen = 0;
    uint16_t extraLen = 0;
    uint16_t commentLen = 0;
    uint16_t diskStart = 0;
    uint16_t internalAttr = 0;
    uint32_t externalAttr = 0;
    uint32_t localHeaderOffset = 0;
};

struct ZipEndOfCentralDir {
    uint32_t signature = 0x06054b50;
    uint16_t diskNum = 0;
    uint16_t centralDirDisk = 0;
    uint16_t entriesOnDisk = 0;
    uint16_t totalEntries = 0;
    uint32_t centralDirSize = 0;
    uint32_t centralDirOffset = 0;
    uint16_t commentLen = 0;
};
#pragma pack(pop)

static uint32_t ComputeCrc32(const void* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

static std::string ArchiveNameUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    const int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

static bool ReadBinaryFileW(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > 0x7FFFFFFF) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    return ok && read == out.size();
}

static bool WriteAllBytes(HANDLE h, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < size) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - written, 0x7FFFFFFF));
        DWORD n = 0;
        if (!WriteFile(h, p + written, chunk, &n, nullptr) || n == 0) return false;
        written += n;
    }
    return true;
}

static bool FindEocdInBuffer(const std::vector<uint8_t>& buf, ZipEndOfCentralDir& eocd, uint32_t& eocdOffset) {
    if (buf.size() < sizeof(ZipEndOfCentralDir)) return false;
    const size_t searchStart = (buf.size() > 65557) ? (buf.size() - 65557) : 0;
    for (size_t pos = searchStart; pos + 4 <= buf.size(); ++pos) {
        uint32_t sig = 0;
        memcpy(&sig, buf.data() + pos, 4);
        if (sig == 0x06054b50) {
            if (pos + sizeof(ZipEndOfCentralDir) > buf.size()) return false;
            memcpy(&eocd, buf.data() + pos, sizeof(eocd));
            eocdOffset = static_cast<uint32_t>(pos);
            return true;
        }
    }
    return false;
}

CreateZipResult CreateZipFile(const std::wstring& zipPath,
                              const std::vector<std::pair<std::wstring, std::wstring>>& files,
                              const std::wstring& requiredLocalPath) {
    CreateZipResult result{};
    struct FileEntry {
        std::string name;
        std::vector<uint8_t> data;
        uint32_t crc32 = 0;
    };
    std::vector<FileEntry> entries;
    entries.reserve(files.size());

    for (const auto& [archiveName, localPath] : files) {
        std::vector<uint8_t> data;
        if (!ReadBinaryFileW(localPath, data)) {
            if (!requiredLocalPath.empty() && _wcsicmp(localPath.c_str(), requiredLocalPath.c_str()) == 0) {
                return result;
            }
            result.skippedFiles.push_back(localPath);
            continue;
        }
        FileEntry entry;
        entry.name = ArchiveNameUtf8(archiveName);
        if (entry.name.empty()) {
            result.skippedFiles.push_back(localPath);
            continue;
        }
        entry.data = std::move(data);
        entry.crc32 = ComputeCrc32(entry.data.data(), entry.data.size());
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) return result;

    std::vector<uint32_t> localOffsets;
    localOffsets.reserve(entries.size());
    uint32_t offset = 0;
    for (const auto& e : entries) {
        localOffsets.push_back(offset);
        offset += static_cast<uint32_t>(sizeof(ZipLocalFileHeader) + e.name.size() + e.data.size());
    }
    const uint32_t centralDirStart = offset;

    const HANDLE h = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return result;

    auto fail = [&]() {
        CloseHandle(h);
        DeleteFileW(zipPath.c_str());
        result.success = false;
        return result;
    };

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        ZipLocalFileHeader lh{};
        lh.compSize = static_cast<uint32_t>(e.data.size());
        lh.uncompSize = static_cast<uint32_t>(e.data.size());
        lh.crc32 = e.crc32;
        lh.fileNameLen = static_cast<uint16_t>(e.name.size());
        if (!WriteAllBytes(h, &lh, sizeof(lh)) ||
            !WriteAllBytes(h, e.name.data(), e.name.size()) ||
            !WriteAllBytes(h, e.data.data(), e.data.size())) {
            return fail();
        }
    }

    uint32_t centralDirSize = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        ZipCentralDirHeader cd{};
        cd.compSize = static_cast<uint32_t>(e.data.size());
        cd.uncompSize = static_cast<uint32_t>(e.data.size());
        cd.crc32 = e.crc32;
        cd.fileNameLen = static_cast<uint16_t>(e.name.size());
        cd.localHeaderOffset = localOffsets[i];
        centralDirSize += static_cast<uint32_t>(sizeof(ZipCentralDirHeader) + e.name.size());
        if (!WriteAllBytes(h, &cd, sizeof(cd)) ||
            !WriteAllBytes(h, e.name.data(), e.name.size())) {
            return fail();
        }
    }

    ZipEndOfCentralDir eocd{};
    eocd.entriesOnDisk = static_cast<uint16_t>(entries.size());
    eocd.totalEntries = static_cast<uint16_t>(entries.size());
    eocd.centralDirSize = centralDirSize;
    eocd.centralDirOffset = centralDirStart;
    if (!WriteAllBytes(h, &eocd, sizeof(eocd))) {
        return fail();
    }

    CloseHandle(h);
    result.success = true;
    return result;
}

int ExtractZipFile(const std::wstring& zipPath, const std::wstring& destDir) {
    std::vector<uint8_t> buf;
    if (!ReadBinaryFileW(zipPath, buf)) return -1;

    // Zip Slip 防护：拒绝绝对路径/驱动符/UNC/`..` 段，并做全路径包含校验。
    auto safeJoin = [](const std::wstring& dir, const std::string& name, std::wstring& out) -> bool {
        if (name.find(':') != std::string::npos) return false;
        if (name.find("\\\\") != std::string::npos) return false;
        std::wstring rel;
        size_t i = 0;
        while (i < name.size()) {
            const char ch = name[i];
            if (ch == '/' || ch == '\\') { ++i; continue; }
            size_t j = name.find_first_of("/\\", i);
            if (j == std::string::npos) j = name.size();
            const std::string seg = name.substr(i, j - i);
            if (seg == "..") return false;
            if (seg == ".") { i = j; continue; }
            if (!rel.empty()) rel += L"\\";
            rel += std::wstring(seg.begin(), seg.end());
            i = j;
        }
        if (rel.empty()) return false;
        const std::wstring joined = dir + L"\\" + rel;
        wchar_t fullOut[MAX_PATH]{};
        wchar_t fullDir[MAX_PATH]{};
        if (GetFullPathNameW(joined.c_str(), MAX_PATH, fullOut, nullptr) == 0) return false;
        if (GetFullPathNameW(dir.c_str(), MAX_PATH, fullDir, nullptr) == 0) return false;
        const size_t dlen = wcslen(fullDir);
        if (_wcsnicmp(fullOut, fullDir, dlen) != 0) return false;
        if (fullOut[dlen] != L'\\') return false;
        out = fullOut;
        return true;
    };

    ZipEndOfCentralDir eocd{};
    uint32_t eocdOffset = 0;
    if (!FindEocdInBuffer(buf, eocd, eocdOffset)) return -1;
    if (eocd.totalEntries == 0) return 0;

    CreateDirectoryW(destDir.c_str(), nullptr);

    size_t pos = eocd.centralDirOffset;
    int extracted = 0;
    for (uint16_t i = 0; i < eocd.totalEntries; ++i) {
        if (pos + sizeof(ZipCentralDirHeader) > buf.size()) break;
        ZipCentralDirHeader cd{};
        memcpy(&cd, buf.data() + pos, sizeof(cd));
        pos += sizeof(cd);
        if (cd.signature != 0x02014b50) break;

        if (pos + cd.fileNameLen > buf.size()) break;
        std::string archiveName(reinterpret_cast<const char*>(buf.data() + pos), cd.fileNameLen);
        pos += cd.fileNameLen + cd.extraLen + cd.commentLen;

        if (!archiveName.empty() && archiveName.back() == '/') continue;

        std::wstring destPath;
        if (!safeJoin(destDir, archiveName, destPath)) continue;

        if (cd.localHeaderOffset + sizeof(ZipLocalFileHeader) > buf.size()) continue;
        ZipLocalFileHeader lh{};
        memcpy(&lh, buf.data() + cd.localHeaderOffset, sizeof(lh));
        if (lh.signature != 0x04034b50) continue;

        const size_t dataOffset = cd.localHeaderOffset + sizeof(ZipLocalFileHeader)
            + lh.fileNameLen + lh.extraLen;
        if (dataOffset + cd.compSize > buf.size()) continue;

        const HANDLE h = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        const bool ok = WriteAllBytes(h, buf.data() + dataOffset, cd.compSize);
        CloseHandle(h);
        if (ok) ++extracted;
    }
    return extracted;
}

std::string ReadTextFromZip(const std::wstring& zipPath, const std::string& archiveName) {
    std::vector<uint8_t> buf;
    if (!ReadBinaryFileW(zipPath, buf)) return "";

    ZipEndOfCentralDir eocd{};
    uint32_t eocdOffset = 0;
    if (!FindEocdInBuffer(buf, eocd, eocdOffset)) return "";

    size_t pos = eocd.centralDirOffset;
    for (uint16_t i = 0; i < eocd.totalEntries; ++i) {
        if (pos + sizeof(ZipCentralDirHeader) > buf.size()) break;
        ZipCentralDirHeader cd{};
        memcpy(&cd, buf.data() + pos, sizeof(cd));
        pos += sizeof(cd);
        if (cd.signature != 0x02014b50) break;

        if (pos + cd.fileNameLen > buf.size()) break;
        std::string name(reinterpret_cast<const char*>(buf.data() + pos), cd.fileNameLen);
        pos += cd.fileNameLen + cd.extraLen + cd.commentLen;

        if (name == archiveName && cd.compSize > 0) {
            if (cd.localHeaderOffset + sizeof(ZipLocalFileHeader) > buf.size()) return "";
            ZipLocalFileHeader lh{};
            memcpy(&lh, buf.data() + cd.localHeaderOffset, sizeof(lh));
            const size_t dataOffset = cd.localHeaderOffset + sizeof(ZipLocalFileHeader)
                + lh.fileNameLen + lh.extraLen;
            if (dataOffset + cd.compSize > buf.size()) return "";
            return std::string(reinterpret_cast<const char*>(buf.data() + dataOffset), cd.compSize);
        }
    }
    return "";
}

// ── 热键与按键名称 ────────────────────────────────────────────────
std::wstring VkName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z')
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= '0' && vk <= '9')
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk) {
    case VK_SPACE:    return L"空格键";
    case VK_RETURN:   return L"Enter";
    case VK_ESCAPE:   return L"Esc";
    case VK_TAB:      return L"Tab";
    case VK_BACK:     return L"Backspace";
    case VK_CAPITAL:  return L"CapsLock";
    case VK_NUMLOCK:  return L"NumLock";
    case VK_SCROLL:   return L"ScrollLock";
    case VK_PAUSE:    return L"Pause";
    case VK_SNAPSHOT: return L"截屏键";
    case VK_INSERT:   return L"Ins";
    case VK_DELETE:   return L"Del";
    case VK_HOME:     return L"Home";
    case VK_END:      return L"End";
    case VK_PRIOR:    return L"PgUp";
    case VK_NEXT:     return L"PgDn";
    case VK_LEFT:     return L"←";
    case VK_UP:       return L"↑";
    case VK_RIGHT:    return L"→";
    case VK_DOWN:     return L"↓";
    case VK_LWIN:     return L"LWin";
    case VK_RWIN:     return L"RWin";
    case VK_LCONTROL: return L"LCtrl";
    case VK_RCONTROL: return L"RCtrl";
    case VK_LMENU:    return L"LAlt";
    case VK_RMENU:    return L"RAlt";
    case VK_LSHIFT:   return L"LShift";
    case VK_RSHIFT:   return L"RShift";
    case VK_APPS:     return L"Apps";
    case VK_MULTIPLY: return L"Num*";
    case VK_ADD:      return L"Num+";
    case VK_SUBTRACT: return L"Num-";
    case VK_DECIMAL:  return L"Num.";
    case VK_DIVIDE:   return L"Num/";
    case VK_NUMPAD0:  return L"Num0";
    case VK_NUMPAD1:  return L"Num1";
    case VK_NUMPAD2:  return L"Num2";
    case VK_NUMPAD3:  return L"Num3";
    case VK_NUMPAD4:  return L"Num4";
    case VK_NUMPAD5:  return L"Num5";
    case VK_NUMPAD6:  return L"Num6";
    case VK_NUMPAD7:  return L"Num7";
    case VK_NUMPAD8:  return L"Num8";
    case VK_NUMPAD9:  return L"Num9";
    case VK_LBUTTON:  return L"鼠标左键";
    case VK_MBUTTON:  return L"鼠标中键";
    case VK_RBUTTON:  return L"鼠标右键";
    case VK_XBUTTON1: return L"鼠标侧键1";
    case VK_XBUTTON2: return L"鼠标侧键2";
    case VK_OEM_COMMA:  return L",";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_MINUS:  return L"-";
    case VK_OEM_PLUS:   return L"=";
    case VK_OEM_1:      return L";";
    case VK_OEM_2:      return L"/";
    case VK_OEM_3:      return L"`";
    case VK_OEM_4:      return L"[";
    case VK_OEM_5:      return L"\\";
    case VK_OEM_6:      return L"]";
    case VK_OEM_7:      return L"'";
    case VK_CLEAR:      return L"Clear";
    }
    return L"按键" + std::to_wstring(vk);
}

UINT VirtualKeyFromKeyText(const std::wstring& keyText) {
    if (keyText.empty()) return 0;
    if (keyText == L"←") return VK_LEFT;
    if (keyText == L"↑") return VK_UP;
    if (keyText == L"→") return VK_RIGHT;
    if (keyText == L"↓") return VK_DOWN;
    if (keyText == L"空格键") return VK_SPACE;
    return 0;
}

UINT NormalizeScriptKeyVk(UINT vk, const std::wstring& keyText) {
    const UINT fromText = VirtualKeyFromKeyText(keyText);
    switch (vk) {
    case 0x2190: return VK_LEFT;
    case 0x2191: return VK_UP;
    case 0x2192: return VK_RIGHT;
    case 0x2193: return VK_DOWN;
    default:
        break;
    }
    if (vk > 255) return fromText;
    if (vk == 0) return fromText;
    return vk;
}

std::wstring HotkeyText(UINT modifiers, UINT vk) {
    std::wstring text;
    if (modifiers & MOD_CONTROL) text += L"Ctrl + ";
    if (modifiers & MOD_ALT)     text += L"Alt + ";
    if (modifiers & MOD_SHIFT)   text += L"Shift + ";
    if (modifiers & MOD_WIN)     text += L"Win + ";
    if (vk == 0) {
        if (text.size() >= 3) text.erase(text.size() - 3);
        return text.empty() ? L"无" : text;
    }
    text += VkName(vk);
    return text;
}

std::wstring HotkeyText(UINT modifiers, UINT vk, bool holdMode) {
    // 显示名仅键位；「按住/按」由黄条等上下文文案表达
    (void)holdMode;
    return HotkeyText(modifiers, vk);
}

double NormalizeHoldThresholdSeconds(double seconds) {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) return 0.2;
    if (seconds > 60.0) return 60.0;
    return seconds;
}

DWORD HoldThresholdMsFromSeconds(double seconds) {
    const double sec = NormalizeHoldThresholdSeconds(seconds);
    const double ms = sec * 1000.0 + 0.5;
    if (ms < 1.0) return 1;
    if (ms > 60000.0) return 60000;
    return static_cast<DWORD>(ms);
}

std::wstring FormatHoldThresholdLabel(double seconds) {
    const double sec = NormalizeHoldThresholdSeconds(seconds);
    wchar_t buf[64]{};
    swprintf_s(buf, L"%.4g", sec);
    return buf;
}

std::wstring FormatDuration(double sec) {
    const int total = std::max(0, static_cast<int>(sec + 0.5));
    const int m = total / 60;
    const int s = total % 60;
    return std::to_wstring(m) + L"' " + std::to_wstring(s) + L"\"";
}

namespace {
constexpr wchar_t kAutoStartRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAutoStartValueName[] = L"键鼠工坊";
}

bool SetAutoStartOnBoot(bool enabled) {
    HKEY key = nullptr;
    const LONG open = RegOpenKeyExW(HKEY_CURRENT_USER, kAutoStartRunKey, 0,
        KEY_SET_VALUE | KEY_QUERY_VALUE, &key);
    if (open != ERROR_SUCCESS || !key) return false;
    bool ok = false;
    if (enabled) {
        wchar_t exePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
            const std::wstring cmd = L"\"" + std::wstring(exePath) + L"\"";
            ok = RegSetValueExW(key, kAutoStartValueName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(cmd.c_str()),
                static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }
    } else {
        const LONG del = RegDeleteValueW(key, kAutoStartValueName);
        ok = (del == ERROR_SUCCESS || del == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

bool IsAutoStartOnBootEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAutoStartRunKey, 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS || !key) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    const LONG q = RegQueryValueExW(key, kAutoStartValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return q == ERROR_SUCCESS && type == REG_SZ && size > sizeof(wchar_t);
}
