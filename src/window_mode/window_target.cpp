#include "window_target.h"

#include "macro_virtual_desktop.h"
#include "window_mode_log.h"
#include "virtual_desktop_accessor.h"
#include "window_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dwmapi.h>
#include <tlhelp32.h>

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

namespace windowmode {

namespace {

std::wstring ToLowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::wstring FileNameFromPath(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring QueryProcessPath(DWORD pid) {
    if (pid == 0) return L"";
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"";

    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameW(process, 0, path, &size)) {
        CloseHandle(process);
        return L"";
    }
    CloseHandle(process);
    return path;
}

bool IsDescendantProcessImpl(DWORD pid, DWORD ancestorPid) {
    if (pid == 0 || ancestorPid == 0) return false;
    if (pid == ancestorPid) return true;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    std::unordered_map<DWORD, DWORD> parentOf;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            parentOf[pe.th32ProcessID] = pe.th32ParentProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    DWORD cur = pid;
    for (int depth = 0; depth < 64 && cur != 0; ++depth) {
        if (cur == ancestorPid) return true;
        const auto it = parentOf.find(cur);
        if (it == parentOf.end()) break;
        cur = it->second;
    }
    return false;
}

bool ProcessMatchesQuery(DWORD pid, const WindowTargetQuery& query) {
    if (query.pid != 0) {
        return pid == query.pid || IsDescendantProcessImpl(pid, query.pid);
    }

    const std::wstring targetLower = ToLowerCopy(query.exePath);
    if (targetLower.empty()) return true;

    const std::wstring processPath = QueryProcessPath(pid);
    if (processPath.empty()) return false;

    const std::wstring processPathLower = ToLowerCopy(processPath);
    const std::wstring processFileLower = ToLowerCopy(FileNameFromPath(processPath));
    const std::wstring targetFileLower = ToLowerCopy(FileNameFromPath(targetLower));
    const bool targetIsStore = targetLower.find(L"windowsapps") != std::wstring::npos;
    const bool processIsStore = processPathLower.find(L"windowsapps") != std::wstring::npos;

    // 商店路径与经典路径即便文件名相同（如 Notepad.exe）也不是同一程序。
    // 之前用「仅文件名」匹配，会把系统记事本当成商店记事本 → 跳过自动打开 → 找图对不上。
    if (targetIsStore) {
        if (!processIsStore) return false;
        if (!targetFileLower.empty() && processFileLower == targetFileLower) return true;
        if (processPathLower == targetLower) return true;
        return false;
    }
    if (processIsStore) {
        // 仅启动等待显式开启 handoff：经典 notepad → 商店包。禁止日常查找误绑旧记事本。
        if (query.allowStoreNotepadHandoff
            && targetFileLower == L"notepad.exe"
            && processFileLower == L"notepad.exe") {
            return true;
        }
        return false;
    }

    if (!targetFileLower.empty() && processFileLower == targetFileLower) return true;
    if (processPathLower == targetLower) return true;
    if (!targetFileLower.empty() && processPathLower.find(targetFileLower) != std::wstring::npos) {
        return true;
    }
    return false;
}

// Edge/Chrome 标题噪声：标签数、用户配置、浏览器名、扩展钉窗戳记 —— 变化会导致误判「未找到窗」。
std::wstring StripBrowserTitleNoise(std::wstring title) {
    auto trim = [](std::wstring& s) {
        while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    };
    trim(title);
    if (title.empty()) return title;

    // 扩展标题戳记：QSTD########: ...
    if (title.size() > 5 && (title[0] == L'Q' || title[0] == L'q')
        && (title[1] == L'S' || title[1] == L's')
        && (title[2] == L'T' || title[2] == L't')
        && (title[3] == L'D' || title[3] == L'd')) {
        const auto colon = title.find(L':');
        if (colon != std::wstring::npos && colon >= 4 && colon < 20) {
            title = title.substr(colon + 1);
            trim(title);
        }
    }

    auto stripSuffixCi = [&](const wchar_t* suffix) {
        const size_t n = wcslen(suffix);
        if (title.size() < n) return;
        if (_wcsicmp(title.c_str() + (title.size() - n), suffix) == 0) {
            title.resize(title.size() - n);
            trim(title);
        }
    };
    stripSuffixCi(L" - microsoft edge");
    stripSuffixCi(L" - google chrome");
    stripSuffixCi(L" - chromium");
    stripSuffixCi(L" - brave");

    // " - 用户配置 N"
    {
        const std::wstring marker = L" - 用户配置 ";
        const auto pos = title.rfind(marker);
        if (pos != std::wstring::npos) {
            bool allDigit = true;
            for (size_t i = pos + marker.size(); i < title.size(); ++i) {
                if (title[i] < L'0' || title[i] > L'9') { allDigit = false; break; }
            }
            if (allDigit && pos + marker.size() < title.size()) {
                title.resize(pos);
                trim(title);
            }
        }
    }

    // " 和另外 N 个页面"
    {
        const std::wstring marker = L" 和另外 ";
        const auto pos = title.find(marker);
        if (pos != std::wstring::npos) {
            title.resize(pos);
            trim(title);
        }
    }
    return title;
}

bool TitleMatches(const std::wstring& title, const std::wstring& titleContains) {
    if (titleContains.empty()) return true;
    const std::wstring titleLower = ToLowerCopy(title);
    const std::wstring needleLower = ToLowerCopy(titleContains);
    if (titleLower.find(needleLower) != std::wstring::npos) return true;

    const std::wstring cleanTitle = ToLowerCopy(StripBrowserTitleNoise(title));
    const std::wstring cleanNeedle = ToLowerCopy(StripBrowserTitleNoise(titleContains));
    if (cleanNeedle.empty()) return true;
    if (cleanTitle.find(cleanNeedle) != std::wstring::npos) return true;
    // 配置标题更长（含旧标签数）时，允许窗口标题是配置的前缀主干。
    if (!cleanTitle.empty() && cleanNeedle.find(cleanTitle) != std::wstring::npos) return true;
    return false;
}

bool ClassMatches(const std::wstring& actual, const std::wstring& expected) {
    if (expected.empty()) return true;
    return ToLowerCopy(actual) == ToLowerCopy(expected);
}

bool IsLikelyMainWindow(HWND hwnd, bool relaxed); // defined below using exported helper

bool IsLikelyMainWindowImpl(HWND hwnd, bool relaxed) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!relaxed && !IsWindowVisible(hwnd)) return false;
    if (!relaxed && GetWindow(hwnd, GW_OWNER)) return false;

    const HWND parent = GetParent(hwnd);
    if (parent && parent != GetDesktopWindow()) return false;

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CHILD) != 0) return false;
    return true;
}

bool IsLikelyMainWindow(HWND hwnd, bool relaxed) {
    if (!IsLikelyMainWindowImpl(hwnd, relaxed)) return false;
    if (IsLikelyImeOrToolWindow(hwnd)) return false;
    return true;
}

struct FindWindowContext {
    WindowTargetQuery query;
    HWND best = nullptr;
    int bestArea = -1;  // 最小化窗 GetWindowRect 面积常为 0；用 -1 才能选中第一扇
    bool launchedProcessMatch = false;
    bool requirePickPoint = false;
    bool allowMinimized = false;
    const GUID* desktopFilter = nullptr;
    int desktopIndexFilter = -1;
    const std::vector<HWND>* excludeHwnds = nullptr;
    std::vector<HWND>* collectAll = nullptr;
};

bool IsExcludedHwnd(HWND hwnd, const std::vector<HWND>* exclude) {
    if (!exclude || !hwnd) return false;
    for (HWND e : *exclude) {
        if (e == hwnd) return true;
    }
    return false;
}

struct FindChildContext {
    std::wstring className;
    HWND found = nullptr;
};

BOOL CALLBACK FindChildByClassProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindChildContext*>(lp);
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (ClassMatches(cls, ctx->className)) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND FindChildInTree(HWND parent, const std::wstring& childClassName) {
    if (!parent || childClassName.empty()) return nullptr;
    FindChildContext ctx{childClassName, nullptr};
    EnumChildWindows(parent, FindChildByClassProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

struct FindLargestChildContext {
    std::wstring className;
    HWND found = nullptr;
    int bestArea = 0;
};

BOOL CALLBACK FindLargestChildByClassProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindLargestChildContext*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (!ClassMatches(cls, ctx->className)) return TRUE;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    const int area = std::max(0, static_cast<int>(rc.right - rc.left))
        * std::max(0, static_cast<int>(rc.bottom - rc.top));
    if (area > ctx->bestArea) {
        ctx->bestArea = area;
        ctx->found = hwnd;
    }
    return TRUE;
}

HWND FindLargestChildByClass(HWND parent, const std::wstring& childClassName) {
    if (!parent || childClassName.empty()) return nullptr;
    FindLargestChildContext ctx{childClassName, nullptr, 0};
    EnumChildWindows(parent, FindLargestChildByClassProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

BOOL CALLBACK EnumWindowsOnDesktopProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindWindowContext*>(lp);
    const bool relaxed = ctx->launchedProcessMatch || ctx->allowMinimized;
    if (!IsLikelyMainWindow(hwnd, relaxed)) return TRUE;
    if (IsExcludedHwnd(hwnd, ctx->excludeHwnds)) return TRUE;

    if (ctx->desktopIndexFilter >= 0) {
        if (!IsWindowOnVirtualDesktopIndex(hwnd, ctx->desktopIndexFilter)) return TRUE;
    } else if (ctx->desktopFilter) {
        if (!IsWindowOnVirtualDesktop(hwnd, *ctx->desktopFilter)) return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!ProcessMatchesQuery(pid, ctx->query)) return TRUE;

    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    if (!TitleMatches(title, ctx->query.titleContains)) return TRUE;

    const bool pidOnly = ctx->query.pid != 0;
    if (!pidOnly && ctx->requirePickPoint
        && (ctx->query.pickX != 0 || ctx->query.pickY != 0)) {
        RECT rcPick{};
        if (!GetWindowRect(hwnd, &rcPick)) return TRUE;
        const POINT pickPt{ctx->query.pickX, ctx->query.pickY};
        if (!PtInRect(&rcPick, pickPt)) return TRUE;
    }

    if (!pidOnly) {
        wchar_t cls[256]{};
        GetClassNameW(hwnd, cls, 256);
        if (!ClassMatches(cls, ctx->query.className)) return TRUE;

        if (!ctx->query.childClassName.empty()
            && !ctx->allowMinimized
            && !FindChildInTree(hwnd, ctx->query.childClassName)) {
            return TRUE;
        }
    }

    if (ctx->collectAll) {
        ctx->collectAll->push_back(hwnd);
        return TRUE;
    }

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    const int area = std::max(0, static_cast<int>(rc.right - rc.left))
        * std::max(0, static_cast<int>(rc.bottom - rc.top));
    if (area > ctx->bestArea) {
        ctx->bestArea = area;
        ctx->best = hwnd;
    }
    return TRUE;
}

}  // namespace

namespace {

bool IsDocumentOrUrlPath(const std::wstring& path) {
    if (path.size() >= 8
        && (_wcsnicmp(path.c_str(), L"https://", 8) == 0
            || (path.size() >= 7 && _wcsnicmp(path.c_str(), L"http://", 7) == 0))) {
        return true;
    }
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= path.size()) return false;
    std::wstring ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    static const wchar_t* kExts[] = {
        L".txt", L".csv", L".log", L".md", L".xml", L".json",
        L".xlsx", L".xls", L".xlsm", L".doc", L".docx", L".pdf",
        L".htm", L".html", L".url",
    };
    for (const wchar_t* e : kExts) {
        if (ext == e) return true;
    }
    return false;
}

std::wstring FileNameOnly(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

void SanitizeWindowTargetQuery(WindowTargetQuery& query) {
    if (!IsDocumentOrUrlPath(query.exePath)) return;

    if (query.titleContains.empty()) {
        const std::wstring name = FileNameOnly(query.exePath);
        if (!name.empty()) query.titleContains = name;
    }
    query.exePath.clear();
}

}  // namespace

bool IsLikelyImeOrToolWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return true;

    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    std::wstring clsLower = cls;
    for (auto& ch : clsLower) ch = static_cast<wchar_t>(towlower(ch));

    static const wchar_t* kReject[] = {
        L"sopy_", L"sgim", L"sogou", L"msctfime", L"ime:",
        L"tooltips_class32", L"workerw",
        L"shell_traywnd", L"notifyiconoverflowwindow",
        L"gdi+", L"foregroundstaging",
    };
    for (const wchar_t* p : kReject) {
        if (clsLower.find(p) != std::wstring::npos) return true;
    }
    if (clsLower.rfind(L"sopy", 0) == 0) return true;

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) != 0 && (ex & WS_EX_APPWINDOW) == 0) {
        return true;
    }

    // 最小化窗口在任务栏，GetWindowRect 常很小——跳过面积判定（与具体程序无关）。
    if (IsIconic(hwnd)) return false;

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd, &wp)
        && (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE
            || wp.showCmd == SW_SHOWMINNOACTIVE)) {
        return false;
    }

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return true;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w > 0 && h > 0 && (w < 200 || h < 120)) return true;

    return false;
}

// 标记：用 DWM Cloak 代替 SetLayeredWindowAttributes，避免 Win11/商店窗口
// 出现多余的「GDI+ Window」半透明残影。
constexpr DWORD kCloakViaDwmMarker = 0xD10C1001u;

WindowTargetQuery BuildTargetQuery(const WindowModeScriptConfig& config) {
    WindowTargetQuery query{};
    query.exePath = config.targetExePath;

    // 「不选择窗口」：只按目标程序查找/启动，忽略编辑时残留的类名/标题/准星点。
    if (config.selectMethod == WindowSelectMethod::NoSelect) {
        SanitizeWindowTargetQuery(query);
        return query;
    }

    query.className = config.windowClassName;
    query.childClassName = config.childWindowClassName;

    // 「指定窗口类」：类名 + 进程路径 +（若有）文档/标题关键词。
    // 标题用文档名 stem（如 test.txt），避免绑到同程序其它空白窗。
    if (config.selectMethod == WindowSelectMethod::UseEditorWindowClass) {
        const std::wstring& title = !config.windowName.empty()
            ? config.windowName : config.targetWindowTitle;
        std::wstring stem = title;
        {
            auto trim = [](std::wstring v) {
                while (!v.empty() && (v.front() == L' ' || v.front() == L'\t')) v.erase(v.begin());
                while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
                return v;
            };
            stem = trim(stem);
            const auto dash = stem.find(L" - ");
            if (dash != std::wstring::npos) stem = trim(stem.substr(0, dash));
            while (!stem.empty() && (stem.front() == L'*' || stem.front() == L' ')) {
                stem.erase(stem.begin());
            }
            stem = trim(stem);
            if (stem == L"无标题" || _wcsicmp(stem.c_str(), L"Untitled") == 0) stem.clear();
        }
        if (!stem.empty()) query.titleContains = stem;
        // 经典 notepad CreateProcess 常交接给商店包；指定窗口类开文档时要认交接后的窗。
        query.allowStoreNotepadHandoff = true;
        SanitizeWindowTargetQuery(query);
        return query;
    }

    query.pickX = config.targetPickX;
    query.pickY = config.targetPickY;
    const std::wstring& title = !config.windowName.empty() ? config.windowName : config.targetWindowTitle;
    query.titleContains = title;
    SanitizeWindowTargetQuery(query);
    return query;
}

bool DoesTopWindowMatchConfig(HWND top, const WindowModeScriptConfig& config) {
    if (!top || !IsWindow(top)) return false;
    top = GetAncestor(top, GA_ROOT);
    if (!top) return false;

    WindowTargetQuery query = BuildTargetQuery(config);
    // Win11 经典 notepad 常交接商店包；身份校验时允许同名交接，避免误判“没绑到”。
    query.allowStoreNotepadHandoff = true;
    DWORD pid = 0;
    GetWindowThreadProcessId(top, &pid);
    if (!ProcessMatchesQuery(pid, query)) return false;

    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (!ClassMatches(cls, query.className)) return false;

    // 「不选择窗口」：不强制标题。
    if (config.selectMethod == WindowSelectMethod::NoSelect) {
        return true;
    }

    // 「指定窗口类」：有文档/标题关键词时必须对上，否则会误绑空白同程序窗并跳过自动打开。
    if (config.selectMethod == WindowSelectMethod::UseEditorWindowClass) {
        if (query.titleContains.empty()) return true;
        wchar_t title[512]{};
        GetWindowTextW(top, title, 512);
        return TitleMatches(title, query.titleContains);
    }

    wchar_t title[512]{};
    GetWindowTextW(top, title, 512);
    if (!TitleMatches(title, query.titleContains)) return false;

    if (query.pickX != 0 || query.pickY != 0) {
        RECT rc{};
        if (!GetWindowRect(top, &rc)) return false;
        const POINT pickPt{query.pickX, query.pickY};
        if (!PtInRect(&rc, pickPt)) return false;
    }
    return true;
}

HWND FindMainWindowOnVirtualDesktopIndex(const WindowTargetQuery& query, int desktopIndex,
    bool allowMinimized) {
    if (desktopIndex < 0) return nullptr;

    auto search = [&](bool requirePick) -> HWND {
        FindWindowContext ctx{};
        ctx.query = query;
        ctx.launchedProcessMatch = query.pid != 0;
        ctx.requirePickPoint = requirePick;
        ctx.allowMinimized = allowMinimized;
        ctx.desktopIndexFilter = desktopIndex;
        EnumWindows(EnumWindowsOnDesktopProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.best;
    };

    HWND hwnd = search(true);
    if (!hwnd) hwnd = search(false);
    return hwnd;
}

HWND FindMainWindowOnVirtualDesktop(const WindowTargetQuery& query, const GUID& desktopId,
    bool allowMinimized) {
    GUID empty{};
    if (IsEqualGUID(desktopId, empty)) return nullptr;

    auto search = [&](bool requirePick) -> HWND {
        FindWindowContext ctx{};
        ctx.query = query;
        ctx.launchedProcessMatch = query.pid != 0;
        ctx.requirePickPoint = requirePick;
        ctx.allowMinimized = allowMinimized;
        ctx.desktopFilter = &desktopId;
        EnumWindows(EnumWindowsOnDesktopProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.best;
    };

    HWND hwnd = search(true);
    if (!hwnd) hwnd = search(false);
    return hwnd;
}

HWND FindMainWindowOnDesktop(const WindowTargetQuery& query, HDESK desktop) {
    if (!desktop) return nullptr;

    auto search = [&](bool requirePick) -> HWND {
        FindWindowContext ctx{};
        ctx.query = query;
        ctx.launchedProcessMatch = query.pid != 0;
        ctx.requirePickPoint = requirePick;
        EnumDesktopWindows(desktop, EnumWindowsOnDesktopProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.best;
    };

    HWND hwnd = search(true);
    if (!hwnd) hwnd = search(false);
    return hwnd;
}

HWND FindMainWindowForProcess(DWORD pid) {
    if (pid == 0) return nullptr;
    WindowTargetQuery query{};
    query.pid = pid;
    // Launch 路径会先 ForceHide/SW_SHOWMINNOACTIVE，必须允许最小化。
    return FindMainWindowDefault(query, true);
}

HWND FindMainWindowDefault(const WindowTargetQuery& query, bool allowMinimized) {
    auto search = [&](bool requirePick) -> HWND {
        FindWindowContext ctx{};
        ctx.query = query;
        ctx.launchedProcessMatch = query.pid != 0;
        ctx.requirePickPoint = requirePick;
        ctx.allowMinimized = allowMinimized;
        EnumWindows(EnumWindowsOnDesktopProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.best;
    };

    HWND hwnd = search(true);
    if (!hwnd) hwnd = search(false);
    return hwnd;
}

void CollectMatchingMainWindows(const WindowTargetQuery& query, bool allowMinimized,
    std::vector<HWND>& out) {
    FindWindowContext ctx{};
    ctx.query = query;
    ctx.launchedProcessMatch = query.pid != 0;
    ctx.requirePickPoint = false;
    ctx.allowMinimized = allowMinimized;
    ctx.collectAll = &out;
    EnumWindows(EnumWindowsOnDesktopProc, reinterpret_cast<LPARAM>(&ctx));
}

HWND FindNewlyAppearedMainWindow(const WindowTargetQuery& query,
    const std::vector<HWND>& excludeHwnds, bool allowMinimized) {
    auto search = [&](bool requirePick) -> HWND {
        FindWindowContext ctx{};
        ctx.query = query;
        ctx.launchedProcessMatch = query.pid != 0;
        ctx.requirePickPoint = requirePick;
        ctx.allowMinimized = allowMinimized;
        ctx.excludeHwnds = &excludeHwnds;
        EnumWindows(EnumWindowsOnDesktopProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.best;
    };

    HWND hwnd = search(true);
    if (!hwnd) hwnd = search(false);
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
    if (IsLikelyImeOrToolWindow(hwnd)) return nullptr;
    return hwnd;
}

HWND FindNewlyAppearedMainWindow(const WindowTargetQuery& query, HWND excludeHwnd,
    bool allowMinimized) {
    std::vector<HWND> exclude;
    if (excludeHwnd) exclude.push_back(excludeHwnd);
    return FindNewlyAppearedMainWindow(query, exclude, allowMinimized);
}

bool GetProcessCreationTime(DWORD pid, FILETIME& outCreate) {
    outCreate = {};
    if (pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    FILETIME create{}, exitT{}, kernel{}, user{};
    const BOOL ok = GetProcessTimes(process, &create, &exitT, &kernel, &user);
    CloseHandle(process);
    if (!ok) return false;
    outCreate = create;
    return true;
}

bool WindowProcessCreatedAtOrAfter(HWND hwnd, const FILETIME& launchTimeUtc) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    FILETIME create{};
    if (!GetProcessCreationTime(pid, create)) return false;
    // 允许少量时钟误差 / CreateProcess 前快照抖动。
    ULARGE_INTEGER launch{}, createUi{};
    launch.LowPart = launchTimeUtc.dwLowDateTime;
    launch.HighPart = launchTimeUtc.dwHighDateTime;
    createUi.LowPart = create.dwLowDateTime;
    createUi.HighPart = create.dwHighDateTime;
    const ULONGLONG skew = 20000000ULL; // 2 seconds in 100ns units
    return createUi.QuadPart + skew >= launch.QuadPart;
}

HWND FindLaunchResultMainWindow(const WindowTargetQuery& query,
    const std::vector<HWND>& excludeHwnds, const FILETIME& launchTimeUtc,
    bool allowReuseExisting, bool allowMinimized) {
    if (HWND newborn = FindNewlyAppearedMainWindow(query, excludeHwnds, allowMinimized)) {
        return newborn;
    }

    std::vector<HWND> candidates;
    CollectMatchingMainWindows(query, allowMinimized, candidates);
    HWND bestNewProc = nullptr;
    int bestArea = -1;
    HWND bestAny = nullptr;
    int bestAnyArea = -1;
    for (HWND hwnd : candidates) {
        if (!hwnd || !IsWindow(hwnd) || IsLikelyImeOrToolWindow(hwnd)) continue;
        RECT rc{};
        int area = 0;
        if (GetWindowRect(hwnd, &rc)) {
            area = std::max(0, static_cast<int>(rc.right - rc.left))
                * std::max(0, static_cast<int>(rc.bottom - rc.top));
        }
        if (WindowProcessCreatedAtOrAfter(hwnd, launchTimeUtc)) {
            if (area > bestArea) {
                bestArea = area;
                bestNewProc = hwnd;
            }
        }
        if (area > bestAnyArea) {
            bestAnyArea = area;
            bestAny = hwnd;
        }
    }
    if (bestNewProc) return bestNewProc;
    if (allowReuseExisting) return bestAny;
    return nullptr;
}

bool IsDescendantProcess(DWORD pid, DWORD ancestorPid) {
    return IsDescendantProcessImpl(pid, ancestorPid);
}

bool MainWindowHasChildClass(HWND hwnd, const std::wstring& childClassName) {
    if (!hwnd || childClassName.empty()) return true;
    return FindChildWindowByClass(hwnd, childClassName) != nullptr;
}

HWND FindChildWindowByClass(HWND parent, const std::wstring& childClassName) {
    return FindChildInTree(parent, childClassName);
}

HWND FindBrowserRenderWidget(HWND top) {
    top = TopLevelTargetWindow(top);
    if (!top || !IsWindow(top)) return nullptr;

    // 输入绑定：必须是真正的 RenderWidget；允许不可见/零面积（宏桌面/未布局）。
    // 禁止 Intermediate D3D Window（合成层，点不中页面）。
    static const wchar_t* kRenderClasses[] = {
        L"Chrome_RenderWidgetHostHWND",
        L"Internet Explorer_Server",
    };
    for (const wchar_t* cls : kRenderClasses) {
        if (HWND found = FindChildInTree(top, cls)) {
            if (!IsBrowserCompositorHwnd(found)) return found;
        }
    }
    return nullptr;
}

bool IsBrowserCompositorHwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    const std::wstring lower = ToLowerCopy(cls);
    if (lower.find(L"intermediate d3d") != std::wstring::npos) return true;
    if (lower.find(L"d3d") != std::wstring::npos
        && lower.find(L"renderwidget") == std::wstring::npos) {
        return true;
    }
    return false;
}

HWND FindBrowserCaptureSurface(HWND top) {
    top = TopLevelTargetWindow(top);
    if (!top || !IsWindow(top)) return nullptr;
    if (HWND render = FindBrowserRenderWidget(top)) return render;

    // 无 RenderWidget 时：找最大 compositor 供截图。
    struct Ctx { HWND best; int area; } ctx{nullptr, -1};
    EnumChildWindows(top, [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsBrowserCompositorHwnd(hwnd)) return TRUE;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) return TRUE;
        const int area = std::max(0, static_cast<int>(rc.right - rc.left))
            * std::max(0, static_cast<int>(rc.bottom - rc.top));
        if (area > c->area) {
            c->area = area;
            c->best = hwnd;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

HWND TopLevelTargetWindow(HWND hwnd) {
    if (!hwnd) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

bool IsWindowOnUserCurrentDesktop(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;

    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (vda.EnsureLoaded(err)) {
        const int onCurrent = vda.IsWindowOnCurrentVirtualDesktop(hwnd);
        if (onCurrent >= 0) return onCurrent > 0;

        const int userDesktop = vda.GetCurrentDesktopNumber();
        const int targetDesktop = vda.GetWindowDesktopNumber(hwnd);
        if (userDesktop >= 0 && targetDesktop >= 0) {
            return userDesktop == targetDesktop;
        }
    }
    // VDA 不可用或无法解析桌面号时，可见/最小化窗口视为当前桌面，避免误走宏桌面截图路径。
    return IsWindowVisible(hwnd) || IsIconic(hwnd);
}

struct ScopedThreadInputAttach {
    DWORD cur = GetCurrentThreadId();
    DWORD tidA = 0;
    DWORD tidB = 0;
    bool attachedA = false;
    bool attachedB = false;

    void Attach(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return;
        const DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
        if (!tid || tid == cur) return;
        if (tidA == 0) {
            tidA = tid;
            attachedA = AttachThreadInput(cur, tidA, TRUE) == TRUE;
            return;
        }
        if (tid == tidA || tidB != 0) return;
        tidB = tid;
        attachedB = AttachThreadInput(cur, tidB, TRUE) == TRUE;
    }

    ~ScopedThreadInputAttach() {
        if (attachedB && tidB) AttachThreadInput(cur, tidB, FALSE);
        if (attachedA && tidA) AttachThreadInput(cur, tidA, FALSE);
    }
};

void DropTopmost(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if ((GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void InsertWindowJustAboveShell(HWND hwnd) {
    HWND shell = FindWindowW(L"Progman", nullptr);
    if (!shell) shell = FindWindowW(L"WorkerW", nullptr);
    if (!shell || shell == hwnd) return;
    SetWindowPos(hwnd, shell, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void DebugLogTarget(const wchar_t* msg) {
    WindowModeLog(std::wstring(L"[窗口模式] ") + msg);
}

void EnsureWindowAtSavedNormalRect(HWND hwnd) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &wp)) return;

    int w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    int h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    if (w <= 0 || h <= 0) return;

    int x = wp.rcNormalPosition.left;
    int y = wp.rcNormalPosition.top;
    if (x < -20000 || y < -20000) {
        x = 80;
        y = 80;
    }

    // Always pin to bottom while resizing — never let the window pop above the user.
    SetWindowPos(hwnd, HWND_BOTTOM, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/// Apply invisibility before a restore so the window cannot flash on top.
/// Prefer DWM Cloak; fall back to near-invisible layered alpha only if Cloak fails.
bool BeginBottomRestoreCloak(HWND hwnd, LONG& savedExStyle, bool& layeredTouched,
    BYTE& savedLayeredAlpha, DWORD& savedLayeredFlags) {
    hwnd = TopLevelTargetWindow(hwnd);
    savedExStyle = 0;
    layeredTouched = false;
    savedLayeredAlpha = 255;
    savedLayeredFlags = 0;
    if (!hwnd || !IsWindow(hwnd)) return false;

    savedExStyle = static_cast<LONG>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));

    BOOL cloak = TRUE;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)))) {
        layeredTouched = true;
        savedLayeredFlags = kCloakViaDwmMarker;
        return true;
    }

    // Fallback：旧路径（部分环境不支持 Cloak）
    if ((savedExStyle & WS_EX_LAYERED) == 0) {
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, savedExStyle | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
        layeredTouched = true;
    } else {
        COLORREF key = 0;
        if (GetLayeredWindowAttributes(hwnd, &key, &savedLayeredAlpha, &savedLayeredFlags)) {
            layeredTouched = true;
            SetLayeredWindowAttributes(hwnd, key, 1, LWA_ALPHA);
        }
    }
    return true;
}

void EndBottomRestoreCloak(HWND hwnd, LONG savedExStyle, bool layeredTouched,
    BYTE savedLayeredAlpha, DWORD savedLayeredFlags) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd) || !layeredTouched) return;

    if (savedLayeredFlags == kCloakViaDwmMarker) {
        BOOL cloak = FALSE;
        DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
        return;
    }

    if ((savedExStyle & WS_EX_LAYERED) == 0) {
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, savedExStyle);
    } else {
        COLORREF key = 0;
        SetLayeredWindowAttributes(hwnd, key, savedLayeredAlpha, savedLayeredFlags);
    }
}

struct ScopedMinAnimateOff {
    bool disabled = false;
    ANIMATIONINFO saved{};
    ScopedMinAnimateOff() {
        saved.cbSize = sizeof(saved);
        if (SystemParametersInfoW(SPI_GETANIMATION, sizeof(saved), &saved, 0)
            && saved.iMinAnimate != 0) {
            ANIMATIONINFO off = saved;
            off.iMinAnimate = 0;
            if (SystemParametersInfoW(SPI_SETANIMATION, sizeof(off), &off, 0)) {
                disabled = true;
            }
        }
    }
    ~ScopedMinAnimateOff() {
        if (disabled) {
            SystemParametersInfoW(SPI_SETANIMATION, sizeof(saved), &saved, 0);
        }
    }
};

/// Soft restore without SW_RESTORE / activation. Window must already be cloaked or at bottom.
bool RestoreMinimizedAtBottom(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsIconic(hwnd)) return !IsIconic(hwnd);

    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    if (GetWindowPlacement(hwnd, &wp)) {
        wp.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(hwnd, &wp);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_SHOWNA);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    // Never call ShowWindow(SW_RESTORE) / SC_RESTORE — they activate and raise Z-order.
    return !IsIconic(hwnd);
}

void PlaceWindowAtBottomOfStack(HWND hwnd, HWND preserveFg) {
    if (!hwnd || !IsWindow(hwnd)) return;

    const bool onUserDesktop = IsWindowOnUserCurrentDesktop(hwnd);

    ScopedThreadInputAttach attach;
    attach.Attach(hwnd);
    if (onUserDesktop) {
        attach.Attach(preserveFg);
    }

    DropTopmost(hwnd);

    for (int i = 0; i < 3; ++i) {
        DWORD flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        if (onUserDesktop) flags |= SWP_SHOWWINDOW;
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, flags);
        if (onUserDesktop) {
            InsertWindowJustAboveShell(hwnd);
            if (preserveFg && IsWindow(preserveFg) && preserveFg != hwnd) {
                SetWindowPos(preserveFg, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    }

    if (onUserDesktop && preserveFg && IsWindow(preserveFg) && preserveFg != hwnd
        && GetForegroundWindow() == hwnd) {
        SetForegroundWindow(preserveFg);
    }
}

void EnsureTargetBelowUserWindows(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    if (!IsWindowOnUserCurrentDesktop(hwnd)) return;
    PlaceWindowAtBottomOfStack(hwnd, GetForegroundWindow());
}

void EnsureTargetAtBottomOfStack(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    PlaceWindowAtBottomOfStack(hwnd, GetForegroundWindow());
}

void ForceHideLaunchedWindowQuiet(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;

    HWND preserveFg = GetForegroundWindow();
    // 注意：工作线程上调用 DwmSetWindowAttribute / RedrawWindow 可能与 Store/ WinUI 应用死锁。
    // 这里只做无激活最小化 + 压底，不碰 DWM Cloak。
    DropTopmost(hwnd);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    if (!IsIconic(hwnd)) {
        // 部分商店窗无视 MINNOACTIVE：再试一次无激活最小化。
        ShowWindow(hwnd, SW_MINIMIZE);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    if (preserveFg && IsWindow(preserveFg) && preserveFg != hwnd) {
        if (GetForegroundWindow() == hwnd || GetForegroundWindow() == nullptr) {
            SetForegroundWindow(preserveFg);
        }
    }
}

void HideTransientGdiPlusWindows(DWORD extraPid) {
    struct Ctx { DWORD self; DWORD extra; } ctx{GetCurrentProcessId(), extraPid};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        const auto* c = reinterpret_cast<const Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c->self && (c->extra == 0 || pid != c->extra)) return TRUE;

        wchar_t cls[256]{};
        GetClassNameW(hwnd, cls, 256);
        wchar_t title[128]{};
        GetWindowTextW(hwnd, title, 128);
        const bool gdiClass = wcsstr(cls, L"GDI+") != nullptr || wcsstr(cls, L"Gdi+") != nullptr;
        const bool gdiTitle = _wcsicmp(title, L"GDI+ Window") == 0
            || _wcsicmp(title, L"GDI+Windows") == 0
            || wcsstr(title, L"GDI+") != nullptr;
        if (!gdiClass && !gdiTitle) return TRUE;

        ShowWindow(hwnd, SW_HIDE);
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((ex & WS_EX_TOOLWINDOW) == 0) {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        }
        SetWindowPos(hwnd, HWND_BOTTOM, -32000, -32000, 1, 1,
            SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOSENDCHANGING);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

bool RestoreWindowPlacementQuiet(HWND hwnd) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &wp)) return false;

    const UINT show = wp.showCmd;
    if (show != SW_SHOWMINIMIZED && show != SW_MINIMIZE && show != SW_SHOWMINNOACTIVE) {
        return !IsIconic(hwnd);
    }

    wp.showCmd = SW_SHOWNOACTIVATE;
    if (!SetWindowPlacement(hwnd, &wp)) return false;
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    return !IsIconic(hwnd);
}

bool RestoreMinimizedSoft(HWND hwnd) {
    return RestoreMinimizedAtBottom(hwnd);
}

bool ShowMacroDesktopWindow(HWND hwnd, LONG& savedExStyle, bool& layeredTouched,
    BYTE& savedLayeredAlpha, DWORD& savedLayeredFlags,
    bool& animationDisabled, ANIMATIONINFO& savedAnim);
bool BeginMacroCaptureLayeredInvisibility(HWND hwnd, LONG& savedExStyle, bool& layeredTouched,
    BYTE& savedLayeredAlpha, DWORD& savedLayeredFlags);

bool RestoreForVisionCaptureBottom(HWND hwnd, HWND preserveFg) {
    return RestoreOnUserDesktopBottom(hwnd, preserveFg);
}

void PinMacroDesktopWindowBottom(HWND hwnd);

bool BeginMacroCaptureLayeredInvisibility(HWND hwnd, LONG& savedExStyle, bool& layeredTouched,
    BYTE& savedLayeredAlpha, DWORD& savedLayeredFlags) {
    // 与 BeginBottomRestoreCloak 同一策略：优先 DWM Cloak，避免 layered 伪窗。
    return BeginBottomRestoreCloak(hwnd, savedExStyle, layeredTouched,
        savedLayeredAlpha, savedLayeredFlags);
}

bool ShowMacroDesktopWindow(HWND hwnd, LONG& savedExStyle, bool& layeredTouched,
    BYTE& savedLayeredAlpha, DWORD& savedLayeredFlags,
    bool& animationDisabled, ANIMATIONINFO& savedAnim) {
    hwnd = TopLevelTargetWindow(hwnd);
    savedExStyle = 0;
    layeredTouched = false;
    animationDisabled = false;
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!IsIconic(hwnd)) {
        WindowModeLog(L"[窗口模式] ShowMacroDesktopWindow: 跳过(窗口未最小化)");
        return true;
    }

    DropTopmost(hwnd);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    savedAnim.cbSize = sizeof(ANIMATIONINFO);
    if (SystemParametersInfoW(SPI_GETANIMATION, sizeof(savedAnim), &savedAnim, 0)
        && savedAnim.iMinAnimate != 0) {
        ANIMATIONINFO disabled = savedAnim;
        disabled.iMinAnimate = 0;
        if (SystemParametersInfoW(SPI_SETANIMATION, sizeof(disabled), &disabled, 0)) {
            animationDisabled = true;
        }
    }

    BeginMacroCaptureLayeredInvisibility(hwnd, savedExStyle, layeredTouched,
        savedLayeredAlpha, savedLayeredFlags);

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &wp)) return false;
    wp.showCmd = SW_SHOWNOACTIVATE;
    if (!SetWindowPlacement(hwnd, &wp)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    if (!IsIconic(hwnd)) {
        EnsureWindowAtSavedNormalRect(hwnd);
    }

    for (int i = 0; i < 3; ++i) {
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // 截图前必须恢复不透明度。LWA_ALPHA=1 对 Electron/Chrome 的 PrintWindow
    // 会产出“非空白但内容错误”的帧（blank=0 仍 0% 匹配）。目标在「鼠标宏」桌面，
    // 用户当前桌面通常看不到，可安全以正常 alpha 截图。
    EndBottomRestoreCloak(hwnd, savedExStyle, layeredTouched,
        savedLayeredAlpha, savedLayeredFlags);

    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    if (IsIconic(hwnd)) {
        RestoreWindowPlacementQuiet(hwnd);
        RedrawWindow(hwnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    const bool ok = !IsIconic(hwnd);
    wchar_t buf[96]{};
    swprintf_s(buf, L"ShowMacroDesktopWindow: iconic=%d ok=%d", IsIconic(hwnd) ? 1 : 0, ok ? 1 : 0);
    DebugLogTarget(buf);
    return ok;
}

void RevertMacroDesktopShowPrep(LONG savedExStyle, bool layeredTouched,
    BYTE savedLayeredAlpha, DWORD savedLayeredFlags,
    bool animationDisabled, const ANIMATIONINFO& savedAnim, HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (animationDisabled) {
        SystemParametersInfoW(SPI_SETANIMATION, sizeof(savedAnim),
            const_cast<ANIMATIONINFO*>(&savedAnim), 0);
    }
    EndBottomRestoreCloak(hwnd, savedExStyle, layeredTouched,
        savedLayeredAlpha, savedLayeredFlags);
}

void HideOwnGdiPlusHelperWindows() {
    HideTransientGdiPlusWindows(0);
}

bool RestoreOffDesktopForCapture(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsIconic(hwnd)) return !IsIconic(hwnd);

    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &wp)) return false;
    wp.showCmd = SW_SHOWNOACTIVATE;
    if (!SetWindowPlacement(hwnd, &wp)) return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    return !IsIconic(hwnd);
}

bool RestoreWindowNonActivating(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!IsIconic(hwnd)) return true;
    return RestoreOnUserDesktopBottom(hwnd, GetForegroundWindow());
}

bool RestoreMinimizedCore(HWND hwnd) {
    return RestoreMinimizedAtBottom(hwnd);
}

bool IsTargetWindowMinimized(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return true;
    return IsIconic(hwnd) == TRUE;
}

void EndQuietBottomCloak(HWND hwnd, LONG savedExStyle, bool layeredTouched,
    BYTE savedLayeredAlpha, DWORD savedLayeredFlags) {
    EndBottomRestoreCloak(hwnd, savedExStyle, layeredTouched,
        savedLayeredAlpha, savedLayeredFlags);
}

bool RestoreOnUserDesktopBottom(HWND hwnd, HWND preserveFg, bool keepCloaked,
    LONG* outSavedExStyle, bool* outLayeredTouched,
    BYTE* outSavedLayeredAlpha, DWORD* outSavedLayeredFlags) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;

    ScopedThreadInputAttach attach;
    attach.Attach(hwnd);
    if (preserveFg && IsWindowOnUserCurrentDesktop(hwnd)) {
        attach.Attach(preserveFg);
    }

    ScopedMinAnimateOff animOff;
    LONG savedExStyle = 0;
    bool layeredTouched = false;
    BYTE savedLayeredAlpha = 255;
    DWORD savedLayeredFlags = 0;
    BeginBottomRestoreCloak(hwnd, savedExStyle, layeredTouched,
        savedLayeredAlpha, savedLayeredFlags);

    DropTopmost(hwnd);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    if (IsIconic(hwnd)) {
        RestoreMinimizedAtBottom(hwnd);
    }

    if (!IsIconic(hwnd)) {
        EnsureWindowAtSavedNormalRect(hwnd);
    }

    PlaceWindowAtBottomOfStack(hwnd, preserveFg);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    PlaceWindowAtBottomOfStack(hwnd, preserveFg);

    if (keepCloaked) {
        // 保持 Cloak：截图/后台点击期间用户看不见展开的窗口。
        if (outSavedExStyle) *outSavedExStyle = savedExStyle;
        if (outLayeredTouched) *outLayeredTouched = layeredTouched;
        if (outSavedLayeredAlpha) *outSavedLayeredAlpha = savedLayeredAlpha;
        if (outSavedLayeredFlags) *outSavedLayeredFlags = savedLayeredFlags;
    } else {
        // Reveal only after the window is confirmed at the bottom of the Z-order.
        EndBottomRestoreCloak(hwnd, savedExStyle, layeredTouched,
            savedLayeredAlpha, savedLayeredFlags);
        for (int i = 0; i < 5; ++i) {
            PlaceWindowAtBottomOfStack(hwnd, preserveFg);
        }
    }

    if (preserveFg && IsWindow(preserveFg) && preserveFg != hwnd
        && GetForegroundWindow() == hwnd) {
        SetForegroundWindow(preserveFg);
    }

    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    PlaceWindowAtBottomOfStack(hwnd, preserveFg);

    const bool ok = !IsIconic(hwnd);
    if (!ok) {
        DebugLogTarget(L"RestoreOnUserDesktopBottom: still iconic after restore");
    }
    return ok;
}

void PinMacroDesktopWindowBottom(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return;

    DropTopmost(hwnd);
    for (int i = 0; i < 3; ++i) {
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void MinimizeForQuietDesktopMove(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return;
    // 跨虚拟桌面移动前先最小化，避免可见窗口被抽走时乱跳（与「不选择窗口」启动路径一致）。
    ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
}

ScopedVisionCapturePrep::ScopedVisionCapturePrep(HWND hwnd, bool backgroundMode)
    : background_(backgroundMode) {
    root_ = TopLevelTargetWindow(hwnd);
    if (!root_ || !IsWindow(root_)) return;

    const bool onUserDesktop = IsWindowOnUserCurrentDesktop(root_);
    const bool macroWindowMode = !background_ && !onUserDesktop;

    savedWp_.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(root_, &savedWp_)) return;

    if (macroWindowMode) {
        auto& vda = VirtualDesktopAccessor::Instance();
        userDeskAtStart_ = vda.GetCurrentDesktopNumber();
        restoreOnDestroy_ = true;
        // 宏桌面找图结束后统一回到最小化，避免还原为可见态（showCmd=1）引起切窗感。
        savedWp_.showCmd = SW_SHOWMINNOACTIVE;
        if (!IsIconic(root_)) {
            WindowModeLog(L"[窗口模式] 找图准备: 宏桌面可见，先最小化再无感展开");
            ShowWindow(root_, SW_SHOWMINNOACTIVE);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!IsIconic(root_)) {
                WindowModeLog(L"[窗口模式] 警告: SW_SHOWMINNOACTIVE 后仍非最小化，展开步骤可能跳过");
            }
            WindowModeLogDesktopSnap(L"最小化后", root_);
        } else {
            WindowModeLog(L"[窗口模式] 找图准备: 宏桌面最小化，分层无感展开(SetWindowPlacement)");
            WindowModeLogDesktopSnap(L"展开前", root_);
        }
        ready_ = ShowMacroDesktopWindow(root_, savedExStyle_, layeredTouched_,
            savedLayeredAlpha_, savedLayeredFlags_, animationDisabled_, savedAnim_);
        if (!ready_) {
            WindowModeLog(L"[窗口模式] 警告: ShowMacroDesktopWindow 未就绪");
        }
        WindowModeLogDesktopSnap(L"展开后", root_);
        return;
    }

    const bool wasMinimized = IsIconic(root_) == TRUE
        || savedWp_.showCmd == SW_SHOWMINIMIZED
        || savedWp_.showCmd == SW_MINIMIZE
        || savedWp_.showCmd == SW_SHOWMINNOACTIVE;

    if (background_ && onUserDesktop) {
        if (wasMinimized) {
            restoreOnDestroy_ = true;
            // 保持 Cloak 截图：避免置底展开后露脸「停一会儿」。
            ready_ = RestoreOnUserDesktopBottom(root_, GetForegroundWindow(), true,
                &savedExStyle_, &layeredTouched_, &savedLayeredAlpha_, &savedLayeredFlags_);
        } else {
            // 已可见（含最大化）：不改 Z 序/尺寸，避免闪屏及 WGC 截到空白游戏区。
            ready_ = true;
        }
        return;
    }

    if (wasMinimized) {
        restoreOnDestroy_ = true;
        ready_ = RestoreOnUserDesktopBottom(root_, GetForegroundWindow());
        return;
    }

    if (onUserDesktop) {
        EnsureTargetBelowUserWindows(root_);
    }
    ready_ = true;
}

ScopedVisionCapturePrep::~ScopedVisionCapturePrep() {
    if (!root_ || !IsWindow(root_)) {
        HideTransientGdiPlusWindows(0);
        return;
    }

    const bool macroWindowMode = !background_
        && !IsWindowOnUserCurrentDesktop(root_);

    if (macroWindowMode && layeredTouched_) {
        RevertMacroDesktopShowPrep(savedExStyle_, layeredTouched_,
            savedLayeredAlpha_, savedLayeredFlags_, animationDisabled_, savedAnim_, root_);
    }

    if (restoreOnDestroy_) {
        WindowModeLog(L"[窗口模式] 找图结束: 还原窗口状态");
        WINDOWPLACEMENT wp = savedWp_;
        if (!macroWindowMode
            && (wp.showCmd == SW_SHOWNORMAL || wp.showCmd == SW_SHOWMAXIMIZED)) {
            wp.showCmd = SW_SHOWNOACTIVATE;
        }
        // 后台模式若仍 Cloaked：先最小化再揭 Cloak，避免露脸。
        SetWindowPlacement(root_, &wp);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (background_ && layeredTouched_) {
            ShowWindow(root_, SW_SHOWMINNOACTIVE);
            EndQuietBottomCloak(root_, savedExStyle_, layeredTouched_,
                savedLayeredAlpha_, savedLayeredFlags_);
            layeredTouched_ = false;
            EnsureTargetBelowUserWindows(root_);
        }
    }

    if (macroWindowMode) {
        PinMacroDesktopWindowBottom(root_);
        if (!IsIconic(root_)) {
            ShowWindow(root_, SW_SHOWMINNOACTIVE);
        }
    }

    if (macroWindowMode && userDeskAtStart_ >= 0) {
        auto& vda = VirtualDesktopAccessor::Instance();
        const int nowDesk = vda.GetCurrentDesktopNumber();
        if (nowDesk >= 0 && nowDesk != userDeskAtStart_) {
            WindowModeLogf(L"[窗口模式] 找图结束: UserDesk %d->%d 切回用户桌面",
                nowDesk, userDeskAtStart_);
            vda.GoToDesktopNumber(userDeskAtStart_);
        }
        WindowModeLogDesktopSnap(L"找图收尾", root_);
    }

    // 找图过程可能激活 GDI+ 辅助窗，收尾时藏起以免观感跳窗。
    DWORD pid = 0;
    if (root_) GetWindowThreadProcessId(root_, &pid);
    HideTransientGdiPlusWindows(pid);
}

bool RestoreWindowQuiet(HWND hwnd, bool skipVisibleZOrder) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;

    const bool onUserDesktop = IsWindowOnUserCurrentDesktop(hwnd);
    HWND preserveFg = GetForegroundWindow();

    if (!IsIconic(hwnd)) {
        if (onUserDesktop && !skipVisibleZOrder) {
            PlaceWindowAtBottomOfStack(hwnd, preserveFg);
        }
        return true;
    }

    if (onUserDesktop) {
        return RestoreOnUserDesktopBottom(hwnd, preserveFg);
    }

    // 目标在其它虚拟桌面（如「鼠标宏」）：保持最小化，避免 ShowWindow(SW_RESTORE)
    // 触发系统切换到该桌面。找图用 PrintWindow，输入用 PostMessage。
    return true;
}

void RestoreBoundTargetTopWindow(HWND hwnd, const WINDOWPLACEMENT& wp) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;

    HWND preserveFg = GetForegroundWindow();
    WINDOWPLACEMENT restore = wp;
    restore.length = sizeof(WINDOWPLACEMENT);

    const UINT show = wp.showCmd;
    const bool wasMinimized = show == SW_SHOWMINIMIZED || show == SW_MINIMIZE
        || show == SW_SHOWMINNOACTIVE;

    if (wasMinimized) {
        restore.showCmd = SW_SHOWMINNOACTIVE;
        SetWindowPlacement(hwnd, &restore);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        // 非最小化：禁止 SetWindowPlacement / SWP_SHOWWINDOW，否则会把窗口抬到用户面前。
        return;
    }

    if (preserveFg && IsWindow(preserveFg) && preserveFg != hwnd
        && GetForegroundWindow() == hwnd) {
        SetForegroundWindow(preserveFg);
    }
}

bool PrepareWindowForCapture(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;

    const bool onUserDesktop = IsWindowOnUserCurrentDesktop(hwnd);
    HWND preserveFg = GetForegroundWindow();

    if (!IsIconic(hwnd)) {
        return true;
    }

    if (onUserDesktop) {
        return RestoreOnUserDesktopBottom(hwnd, preserveFg);
    }

    // 宏桌面：非最小化与后台窗口模式相同；仅最小化时需无感展开才能 PrintWindow 全尺寸截图。
    return true;
}

WindowModeHealth EvaluateTargetHealth(HWND hwnd, HDC /*probeDc*/) {
    if (!hwnd || !IsWindow(hwnd)) return WindowModeHealth::TargetNotFound;

    HWND root = TopLevelTargetWindow(hwnd);
    WINDOWPLACEMENT savedWp{};
    savedWp.length = sizeof(WINDOWPLACEMENT);
    const bool hadSavedWp = GetWindowPlacement(root, &savedWp);
    const bool wasMinimized = IsTargetWindowMinimized(hwnd);
    bool expandedForProbe = false;

    auto restoreIfNeeded = [&]() {
        if (expandedForProbe && wasMinimized && hadSavedWp && root && IsWindow(root)) {
            SetWindowPlacement(root, &savedWp);
            SetWindowPos(root, HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    };

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process && pid != 0) {
        return WindowModeHealth::PermissionMismatch;
    }
    if (process) CloseHandle(process);

    if (!IsWindowOnUserCurrentDesktop(root)) {
        return WindowModeHealth::Ok;
    }

    HWND captureHwnd = hwnd;
    if (root && root != hwnd && (IsIconic(root) || IsTargetWindowMinimized(hwnd))) {
        captureHwnd = root;
    }

    // Prefer a quiet GDI probe while still minimized to avoid restore flash.
    if (wasMinimized) {
        WindowCaptureResult minimizedProbe = CaptureWindowClientGdi(captureHwnd);
        if (minimizedProbe.bitmap && !IsCaptureLikelyBlank(minimizedProbe.bitmap)) {
            DeleteObject(minimizedProbe.bitmap);
            return WindowModeHealth::Ok;
        }
        if (minimizedProbe.bitmap) DeleteObject(minimizedProbe.bitmap);

        if (!PrepareWindowForCapture(root)) {
            return WindowModeHealth::TargetMinimized;
        }
        expandedForProbe = true;
    }

    WindowCaptureResult capture = CaptureWindowClientGdi(captureHwnd);
    restoreIfNeeded();
    if (!capture.bitmap) return WindowModeHealth::CaptureFailed;

    const bool blank = IsCaptureLikelyBlank(capture.bitmap);
    DeleteObject(capture.bitmap);

    if (blank && IsWindowOnUserCurrentDesktop(root)) {
        return WindowModeHealth::TargetNoRender;
    }
    return WindowModeHealth::Ok;
}

// =============================================================================
// CDP / 扩展停放与展开（≥1.1.39：不 Cloak/α=1，只 Move）
// =============================================================================

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef DWMWA_DISALLOW_PEEK
#define DWMWA_DISALLOW_PEEK 11
#endif
#ifndef DWMWA_FORCE_ICONIC_REPRESENTATION
#define DWMWA_FORCE_ICONIC_REPRESENTATION 7
#endif
#ifndef DWMWA_HAS_ICONIC_BITMAP
#define DWMWA_HAS_ICONIC_BITMAP 10
#endif

namespace {

std::mutex g_cdpParkMu;
std::unordered_map<HWND, bool> g_peekSuppressed;
std::unordered_map<HWND, bool> g_cdpPinned;
std::unordered_map<HWND, WINDOWPLACEMENT> g_cdpParkPlacement;
std::unordered_map<HWND, SIZE> g_cdpParkSize; // 首次屏外尺寸冻结
std::atomic<HWND> g_watchHwnd{nullptr};
std::atomic_bool g_watchStop{true};
std::thread g_watchThread;
std::mutex g_watchMu;

bool PlacementLooksOnScreen(const RECT& rc) {
    return rc.left > -10000 && rc.top > -10000
        && rc.right > rc.left + 64 && rc.bottom > rc.top + 64;
}

RECT WorkAreaFallbackRect() {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        return mi.rcWork;
    }
    return RECT{0, 0, 1280, 800};
}

/// 观看展开用：把目标矩形钳进工作区，避免 2582x1390@40,40 溢出屏幕「像没展开」。
void FitRectIntoWorkArea(RECT& dest, int& destW, int& destH) {
    const RECT wa = WorkAreaFallbackRect();
    const int waW = (std::max)(640, static_cast<int>(wa.right - wa.left));
    const int waH = (std::max)(400, static_cast<int>(wa.bottom - wa.top));
    if (destW > waW) destW = waW;
    if (destH > waH) destH = waH;
    if (destW < 640) destW = (std::min)(1280, waW);
    if (destH < 400) destH = (std::min)(800, waH);
    dest.left = wa.left;
    dest.top = wa.top;
    if (dest.left + destW > wa.right) dest.left = wa.right - destW;
    if (dest.top + destH > wa.bottom) dest.top = wa.bottom - destH;
    if (dest.left < wa.left) dest.left = wa.left;
    if (dest.top < wa.top) dest.top = wa.top;
    dest.right = dest.left + destW;
    dest.bottom = dest.top + destH;
}

bool SoftRestoreNonActivating(HWND hwnd);
void StripCloakAndNearInvisibleAlpha(HWND hwnd);

void RememberCdpParkPlacement(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    std::lock_guard<std::mutex> lock(g_cdpParkMu);

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &wp)) return;

    if (!IsIconic(hwnd)) {
        RECT wr{};
        if (GetWindowRect(hwnd, &wr) && PlacementLooksOnScreen(wr)) {
            wp.rcNormalPosition = wr;
        }
    }
    if (!PlacementLooksOnScreen(wp.rcNormalPosition)) {
        // 已有合格缓存则保留；否则用工作区兜底。
        auto it = g_cdpParkPlacement.find(hwnd);
        if (it != g_cdpParkPlacement.end()
            && PlacementLooksOnScreen(it->second.rcNormalPosition)) {
            return;
        }
        wp.rcNormalPosition = WorkAreaFallbackRect();
    }
    wp.flags = static_cast<UINT>(wp.flags & ~WPF_RESTORETOMAXIMIZED);
    g_cdpParkPlacement[hwnd] = wp;
}

bool RestoreCdpParkPlacementThenMinimize(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;

    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (vda.EnsureLoaded(err)) {
        if (vda.IsPinnedWindow(hwnd) > 0) vda.UnPinWindow(hwnd);
        std::lock_guard<std::mutex> lock(g_cdpParkMu);
        g_cdpPinned.erase(hwnd);
        g_cdpParkSize.erase(hwnd);
    }

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    bool haveSaved = false;
    {
        std::lock_guard<std::mutex> lock(g_cdpParkMu);
        auto it = g_cdpParkPlacement.find(hwnd);
        if (it != g_cdpParkPlacement.end()) {
            wp = it->second;
            haveSaved = true;
            g_cdpParkPlacement.erase(it);
        }
    }
    if (!haveSaved) {
        if (!GetWindowPlacement(hwnd, &wp)) return false;
    }
    if (!PlacementLooksOnScreen(wp.rcNormalPosition)) {
        wp.rcNormalPosition = WorkAreaFallbackRect();
    }
    wp.length = sizeof(WINDOWPLACEMENT);
    wp.flags = static_cast<UINT>(wp.flags & ~WPF_RESTORETOMAXIMIZED);
    wp.showCmd = SW_SHOWMINNOACTIVE;
    if (!SetWindowPlacement(hwnd, &wp)) {
        if (!IsIconic(hwnd)) ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
        return false;
    }
    return true;
}

// 定论（2026-07-23 晚）：
// - 异桌还原(无 Pin) → 鼠标/焦点会把用户切到宏桌面【刚证伪】
// - 长期最小化 → WebGL 静帧【证伪】
// - Pin+屏外 → 不切屏且出帧；须：① 保持工作区尺寸防点偏 ② 观看 UnPin+迁回宏桌面+还原 ③ 禁刷 SetWindowPlacement
bool WindowNeedsCdpReveal(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) return true;
    RECT wr{};
    if (GetWindowRect(hwnd, &wr) && wr.left <= -10000) return true;
    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (vda.EnsureLoaded(err) && vda.IsPinnedWindow(hwnd) > 0) return true;
    return false;
}

bool IsCdpLiveOffscreenParked(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;
    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (!vda.EnsureLoaded(err) || vda.IsPinnedWindow(hwnd) <= 0) return false;
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return false;
    return wr.left <= -10000;
}

void ResolveCdpParkSize(HWND hwnd, int& outW, int& outH) {
    outW = 1280;
    outH = 800;
    {
        std::lock_guard<std::mutex> lock(g_cdpParkMu);
        auto sz = g_cdpParkSize.find(hwnd);
        if (sz != g_cdpParkSize.end() && sz->second.cx >= 640 && sz->second.cy >= 400) {
            outW = sz->second.cx;
            outH = sz->second.cy;
            return;
        }
        auto it = g_cdpParkPlacement.find(hwnd);
        if (it != g_cdpParkPlacement.end()
            && PlacementLooksOnScreen(it->second.rcNormalPosition)) {
            const RECT& rc = it->second.rcNormalPosition;
            outW = (std::max)(640, static_cast<int>(rc.right - rc.left));
            outH = (std::max)(400, static_cast<int>(rc.bottom - rc.top));
        }
    }
    RECT wr{};
    if (GetWindowRect(hwnd, &wr) && PlacementLooksOnScreen(wr)) {
        outW = (std::max)(outW, static_cast<int>(wr.right - wr.left));
        outH = (std::max)(outH, static_cast<int>(wr.bottom - wr.top));
    }
    std::lock_guard<std::mutex> lock(g_cdpParkMu);
    g_cdpParkSize[hwnd] = SIZE{outW, outH};
}

bool CdpHideLiveOffscreen(HWND hwnd, int viewBefore) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsCdpLiveOffscreenParked(hwnd)) return true;

    auto& vda = VirtualDesktopAccessor::Instance();
    RememberCdpParkPlacement(hwnd);

    int w = 1280, h = 800;
    ResolveCdpParkSize(hwnd, w, h);

    // 关键：先把 placement 设到屏外再取消最小化，禁止「先还原到屏上再挪走」白屏闪一下。
    // Pin 放到挪屏外之后：Pin 窗全桌可见，先 Pin 再 SHOW 必闪。
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &wp)) {
        wp.length = sizeof(WINDOWPLACEMENT);
    }
    wp.flags = static_cast<UINT>(wp.flags & ~WPF_RESTORETOMAXIMIZED);
    wp.showCmd = SW_SHOWNOACTIVATE;
    wp.rcNormalPosition = RECT{-32000, -32000, -32000 + w, -32000 + h};
    SetWindowPlacement(hwnd, &wp);

    if (IsIconic(hwnd)) {
        SoftRestoreNonActivating(hwnd); // 此时 rcNormal 已是屏外 → quietPark
    }
    // 禁 SWP_SHOWWINDOW / FRAMECHANGED / 多余重绘：Pin 时会在用户桌闪白框。
    SetWindowPos(hwnd, HWND_BOTTOM, -32000, -32000, w, h,
        SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_NOREDRAW);
    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT again{};
        again.length = sizeof(WINDOWPLACEMENT);
        if (GetWindowPlacement(hwnd, &again)) {
            again.showCmd = SW_SHOWNOACTIVATE;
            again.rcNormalPosition = RECT{-32000, -32000, -32000 + w, -32000 + h};
            SetWindowPlacement(hwnd, &again);
        }
        SetWindowPos(hwnd, HWND_BOTTOM, -32000, -32000, w, h,
            SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_NOREDRAW);
    }

    if (vda.IsPinnedWindow(hwnd) <= 0) {
        if (!vda.PinWindow(hwnd)) {
            WindowModeLog(L"[窗口模式] CDP 出帧: PinWindow 失败");
        }
        std::lock_guard<std::mutex> lock(g_cdpParkMu);
        g_cdpPinned[hwnd] = true;
    }

    if (viewBefore >= 0) vda.HoldView(viewBefore, 80);

    const bool live = !IsIconic(hwnd);
    WindowModeLogf(L"[窗口模式] CDP 出帧隐藏: iconic=%d pin=%d offscreen=1 size=%dx%d",
        live ? 0 : 1, vda.IsPinnedWindow(hwnd) > 0 ? 1 : 0, w, h);
    return live;
}

bool CdpRevealOnMacroForWatch(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!UserOnMacroDesktopNow()) return false;
    // 已展开：绝对不要再 SetWindowPlacement（会二次切屏）。
    if (!WindowNeedsCdpReveal(hwnd)) return true;

    auto& vda = VirtualDesktopAccessor::Instance();
    for (int i = 0; i < 3 && vda.IsPinnedWindow(hwnd) > 0; ++i) {
        vda.UnPinWindow(hwnd);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    {
        std::lock_guard<std::mutex> lock(g_cdpParkMu);
        g_cdpPinned.erase(hwnd);
    }

    // UnPin 后窗可能落在当前桌；强制迁回「鼠标宏」。
    MacroVirtualDesktop desk;
    if (desk.OpenOrCreate()) {
        desk.MoveWindowToMacroDesktop(hwnd);
    }

    RECT dest = WorkAreaFallbackRect();
    int destW = dest.right - dest.left;
    int destH = dest.bottom - dest.top;
    {
        std::lock_guard<std::mutex> lock(g_cdpParkMu);
        auto it = g_cdpParkPlacement.find(hwnd);
        if (it != g_cdpParkPlacement.end()
            && PlacementLooksOnScreen(it->second.rcNormalPosition)) {
            dest = it->second.rcNormalPosition;
            destW = (std::max)(640, static_cast<int>(dest.right - dest.left));
            destH = (std::max)(400, static_cast<int>(dest.bottom - dest.top));
        } else {
            auto sz = g_cdpParkSize.find(hwnd);
            if (sz != g_cdpParkSize.end() && sz->second.cx >= 640 && sz->second.cy >= 400) {
                destW = sz->second.cx;
                destH = sz->second.cy;
                dest.right = dest.left + destW;
                dest.bottom = dest.top + destH;
            }
        }
    }
    FitRectIntoWorkArea(dest, destW, destH);

    RECT wr{};
    GetWindowRect(hwnd, &wr);
    const bool alreadyOnScreen = !IsIconic(hwnd) && PlacementLooksOnScreen(wr);
    // 最大化常见 left/top 为负（如 -11,-11）：仍算在屏上，但任务栏点不开时常因仍 Pin/Peek。
    // 仅剩 Pin：禁再刷 Placement。
    if (alreadyOnScreen) {
        if (vda.IsPinnedWindow(hwnd) > 0) {
            vda.UnPinWindow(hwnd);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        StripCloakAndNearInvisibleAlpha(hwnd);
        ClearMacroDesktopTaskbarPreviewSuppression(hwnd);
        // 已在屏上但尺寸大于工作区（停放冻结的超大框）：收拢，否则像「展不开」。
        {
            int w = wr.right - wr.left;
            int h = wr.bottom - wr.top;
            const RECT wa = WorkAreaFallbackRect();
            const int waW = wa.right - wa.left;
            const int waH = wa.bottom - wa.top;
            if (w > waW + 8 || h > waH + 8 || wr.left < wa.left - 8 || wr.top < wa.top - 8) {
                RECT fit = wr;
                FitRectIntoWorkArea(fit, w, h);
                SetWindowPos(hwnd, HWND_TOP, fit.left, fit.top, w, h,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
                wr = fit;
            }
        }
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        const bool ok = vda.IsPinnedWindow(hwnd) <= 0 && !IsIconic(hwnd);
        WindowModeLogf(L"[窗口模式] 观看：仅 UnPin（已在屏上）ok=%d pin=%d pos=%d,%d",
            ok ? 1 : 0, vda.IsPinnedWindow(hwnd) > 0 ? 1 : 0, wr.left, wr.top);
        return ok;
    }

    // 负边框/超大停放尺寸：钳进工作区（FitRectIntoWorkArea 已处理）。
    // 非 iconic 但屏外：SetWindowPlacement 只改「还原矩形」、不挪当前可见窗（已踩坑）。
    // 必须 SetWindowPos 强制搬回工作区。
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &wp)) {
        wp.length = sizeof(WINDOWPLACEMENT);
    }
    wp.flags = static_cast<UINT>(wp.flags & ~WPF_RESTORETOMAXIMIZED);
    wp.showCmd = SW_SHOWNOACTIVATE;
    wp.rcNormalPosition = RECT{dest.left, dest.top, dest.left + destW, dest.top + destH};
    SetWindowPlacement(hwnd, &wp);
    if (IsIconic(hwnd)) {
        SoftRestoreNonActivating(hwnd); // 用户已在宏桌面，同桌允许
    }
    SetWindowPos(hwnd, HWND_TOP, dest.left, dest.top, destW, destH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    if (vda.IsPinnedWindow(hwnd) > 0) {
        vda.UnPinWindow(hwnd);
    }
    if (desk.OpenOrCreate()) {
        desk.MoveWindowToMacroDesktop(hwnd);
    }
    StripCloakAndNearInvisibleAlpha(hwnd);
    ClearMacroDesktopTaskbarPreviewSuppression(hwnd);

    GetWindowRect(hwnd, &wr);
    const bool ok = !IsIconic(hwnd) && PlacementLooksOnScreen(wr)
        && vda.IsPinnedWindow(hwnd) <= 0;
    if (ok) {
        WindowModeLogf(L"[窗口模式] 观看：已 UnPin 并还原到宏桌面（可展开） pos=%d,%d %dx%d",
            wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top);
    } else {
        WindowModeLogf(L"[窗口模式] 观看：展开未完成 iconic=%d pin=%d left=%d",
            IsIconic(hwnd) ? 1 : 0, vda.IsPinnedWindow(hwnd) > 0 ? 1 : 0, wr.left);
    }
    return ok;
}

bool SoftRestoreCdpIfUserOnMacro(HWND hwnd) {
    return CdpRevealOnMacroForWatch(hwnd);
}

bool IsAppCloakedOrNearInvisible(HWND hwnd, BYTE* alphaOut = nullptr) {
    if (alphaOut) *alphaOut = 255;
    if (!hwnd || !IsWindow(hwnd)) return false;
    BOOL cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    const bool appCloak = (static_cast<DWORD>(cloaked) & 0x1) != 0; // DWM_CLOAKED_APP
    const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    BYTE alpha = 255;
    DWORD flags = 0;
    COLORREF key = 0;
    if (ex & WS_EX_LAYERED) {
        GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags);
    }
    if (alphaOut) *alphaOut = alpha;
    const bool layeredGhost = (ex & WS_EX_LAYERED) != 0
        && (flags & LWA_ALPHA) != 0 && alpha <= 2;
    return appCloak || layeredGhost;
}

void StripCloakAndNearInvisibleAlpha(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;

    BOOL cloak = FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));

    const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        COLORREF key = 0;
        GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags);
        if ((flags & LWA_ALPHA) && alpha <= 2) {
            // 还原为不透明；若原本无 layered 需求则剥掉 EXSTYLE。
            SetLayeredWindowAttributes(hwnd, key, 255, LWA_ALPHA);
            // 不强制去掉 WS_EX_LAYERED（浏览器常自带），只保证可见。
        }
    }
}

bool SoftRestoreNonActivating(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!IsIconic(hwnd)) return true;

    // 同桌面允许 ShowWindow；异桌面 Show 会把用户视图拽到该窗所在桌面（已证伪）。
    const bool onCurrentVd = IsWindowOnUserCurrentDesktop(hwnd);

    WINDOWPLACEMENT before{};
    before.length = sizeof(WINDOWPLACEMENT);
    const bool haveBefore = GetWindowPlacement(hwnd, &before) == TRUE;
    // Pin+屏外停放：placement 已在屏外时禁 ShowWindow（否则用户桌白闪一帧，已证伪）。
    const bool restoreOffscreen = haveBefore && before.rcNormalPosition.left <= -10000;
    bool pinned = false;
    {
        auto& vda = VirtualDesktopAccessor::Instance();
        std::wstring err;
        if (vda.EnsureLoaded(err)) pinned = vda.IsPinnedWindow(hwnd) > 0;
    }
    // Pin 窗全桌「可见」：连 SetWindowPlacement(SHOW*) / ShowWindow 都会在用户桌闪白框。
    const bool quietPark = restoreOffscreen || pinned;

    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);

    if (quietPark) {
        // 调用方已写屏外 Placement；此处禁任何 SHOW*（含再写 Placement），避免 Pin 全桌白闪。
        // 仍 iconic 时由 CdpHideLiveOffscreen 用 Placement+SetWindowPos(NOREDRAW) 收尾。
        return !IsIconic(hwnd);
    }

    WINDOWPLACEMENT wp = before;
    wp.length = sizeof(WINDOWPLACEMENT);
    if (haveBefore || GetWindowPlacement(hwnd, &wp)) {
        wp.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(hwnd, &wp);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (IsIconic(hwnd) && onCurrentVd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return !IsIconic(hwnd);
}

}  // namespace

bool UserOnMacroDesktopNow() {
    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (!vda.EnsureLoaded(err)) return false;
    const int cur = vda.GetCurrentDesktopNumber();
    if (cur < 0) return false;
    return vda.DesktopNameMatches(cur, kMacroDesktopDisplayName);
}

bool WindowFillsWorkArea(HWND hwnd, int slackPx) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    const RECT& wa = mi.rcWork;
    const int slack = std::max(0, slackPx);
    return wr.left <= wa.left + slack
        && wr.top <= wa.top + slack
        && wr.right >= wa.right - slack
        && wr.bottom >= wa.bottom - slack;
}

bool RestoreMinimizedQuietPreferMax(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &wp)) return false;

    // 曾最大化：安静铺满工作区（禁止 ShowWindow(MAXIMIZE)）。
    const bool wantFill = (wp.flags & WPF_RESTORETOMAXIMIZED) != 0
        || wp.showCmd == SW_SHOWMAXIMIZED;

    if (!SoftRestoreNonActivating(hwnd)) return false;

    if (wantFill) {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            const RECT& wa = mi.rcWork;
            SetWindowPos(hwnd, HWND_BOTTOM,
                wa.left, wa.top, wa.right - wa.left, wa.bottom - wa.top,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
    return !IsIconic(hwnd);
}

bool IsBrowserFakeFocusUnsupported(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    return LooksLikeChromiumBrowserClass(cls);
}

void SuppressMacroDesktopTaskbarPreview(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_DISALLOW_PEEK, &on, sizeof(on));
    DwmSetWindowAttribute(hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &on, sizeof(on));
    DwmSetWindowAttribute(hwnd, DWMWA_HAS_ICONIC_BITMAP, &on, sizeof(on));
    std::lock_guard<std::mutex> lock(g_cdpParkMu);
    g_peekSuppressed[hwnd] = true;
}

bool IsMacroDesktopTaskbarPreviewSuppressed(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd) return false;
    std::lock_guard<std::mutex> lock(g_cdpParkMu);
    const auto it = g_peekSuppressed.find(hwnd);
    return it != g_peekSuppressed.end() && it->second;
}

void ClearMacroDesktopTaskbarPreviewSuppression(HWND hwnd) {
    auto clearOne = [](HWND h) {
        if (!h || !IsWindow(h)) return;
        BOOL off = FALSE;
        DwmSetWindowAttribute(h, DWMWA_DISALLOW_PEEK, &off, sizeof(off));
        DwmSetWindowAttribute(h, DWMWA_FORCE_ICONIC_REPRESENTATION, &off, sizeof(off));
        DwmSetWindowAttribute(h, DWMWA_HAS_ICONIC_BITMAP, &off, sizeof(off));
    };
    std::lock_guard<std::mutex> lock(g_cdpParkMu);
    if (hwnd) {
        hwnd = TopLevelTargetWindow(hwnd);
        clearOne(hwnd);
        g_peekSuppressed.erase(hwnd);
        return;
    }
    for (auto& kv : g_peekSuppressed) {
        clearOne(kv.first);
    }
    g_peekSuppressed.clear();
}

bool IsMacroVisionLatched(HWND) { return false; }
bool IsMacroVisionCaptureReady(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;
    // CDP Pin+屏外即视为可出帧；用户在宏桌面且已展开亦就绪。勿恒 false（否则每次找图再 Park）。
    if (IsCdpLiveOffscreenParked(hwnd)) return true;
    if (UserOnMacroDesktopNow() && !WindowNeedsCdpReveal(hwnd)) return true;
    return false;
}
bool IsMacroVisionInvisibilityActive(HWND hwnd) {
    return IsAppCloakedOrNearInvisible(hwnd);
}

void ForceRevealMacroDesktopWindow(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    // 用户不在宏桌面时 ForceReveal/刮 α → Win11 切到宏桌面目标窗（已证伪）。
    if (!UserOnMacroDesktopNow()) return;
    if (!IsAppCloakedOrNearInvisible(hwnd)) return;
    StripCloakAndNearInvisibleAlpha(hwnd);
    WindowModeLog(L"[窗口模式] ForceReveal: 已刮掉 Cloak/近透明 α（用户已在宏桌面）");
}

void RaiseMacroDesktopWindowForWatch(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    if (!WindowNeedsCdpReveal(hwnd)) return; // 已展开：禁碰窗
    CdpRevealOnMacroForWatch(hwnd);
}

void ReleaseMacroDesktopVisionLatch(HWND hwnd) {
    ClearMacroDesktopTaskbarPreviewSuppression(hwnd);
}

void RestoreMacroDesktopWindowAfterRun(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    ClearMacroDesktopTaskbarPreviewSuppression(hwnd);
    if (RestoreCdpParkPlacementThenMinimize(hwnd)) {
        WindowModeLog(L"[窗口模式] 会话结束: 已 UnPin/恢复坐标并最小化（禁 GoTo）");
        return;
    }
    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (vda.EnsureLoaded(err) && vda.IsPinnedWindow(hwnd) > 0) {
        vda.UnPinWindow(hwnd);
    }
    if (!IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    }
    WindowModeLog(L"[窗口模式] 会话结束: 已最小化目标（禁 GoTo/ForceReveal）");
}

bool ParkCdpBrowserOnMacroDesktop(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;

    ClearMacroDesktopTaskbarPreviewSuppression(hwnd);

    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (!vda.EnsureLoaded(err)) {
        WindowModeLog(L"[窗口模式] CDP 停放: VDA 不可用");
        return false;
    }

    const int viewBefore = vda.GetCurrentDesktopNumber();

    MacroVirtualDesktop desk;
    if (!desk.OpenOrCreate()) {
        WindowModeLog(L"[窗口模式] CDP 停放: 宏桌面未就绪");
        return false;
    }
    const int macroIdx = desk.DesktopIndex();

    auto holdView = [&](int ms) {
        if (viewBefore >= 0) vda.HoldView(viewBefore, ms);
    };

    if (IsAppCloakedOrNearInvisible(hwnd)) {
        StripCloakAndNearInvisibleAlpha(hwnd);
    }
    RememberCdpParkPlacement(hwnd);

    if (UserOnMacroDesktopNow()) {
        const int winDesk = vda.GetWindowDesktopNumber(hwnd);
        if (macroIdx >= 0 && winDesk >= 0 && winDesk != macroIdx
            && !vda.IsWindowOnDesktopNumber(hwnd, macroIdx)
            && vda.IsPinnedWindow(hwnd) <= 0) {
            MinimizeForQuietDesktopMove(hwnd);
            desk.MoveWindowToMacroDesktop(hwnd);
        }
        CdpRevealOnMacroForWatch(hwnd);
        holdView(80);
        WindowModeLogf(L"[窗口模式] CDP 停放: 用户在宏桌面 iconic=%d",
            IsIconic(hwnd) ? 1 : 0);
        return true;
    }

    if (IsCdpLiveOffscreenParked(hwnd)) {
        WindowModeLog(L"[窗口模式] CDP 停放: 已 Pin+屏外出帧，跳过");
        return true;
    }

    const int winDesk = vda.GetWindowDesktopNumber(hwnd);
    const bool alreadyOnMacro = (macroIdx >= 0)
        && (winDesk == macroIdx || vda.IsWindowOnDesktopNumber(hwnd, macroIdx));

    if (!alreadyOnMacro) {
        // 先最小化再 UnPin：避免「UnPin 后仍可见」在用户桌闪一帧。
        MinimizeForQuietDesktopMove(hwnd);
        if (vda.IsPinnedWindow(hwnd) > 0) {
            vda.UnPinWindow(hwnd);
            std::lock_guard<std::mutex> lock(g_cdpParkMu);
            g_cdpPinned.erase(hwnd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        holdView(40);
        if (!desk.MoveWindowToMacroDesktop(hwnd)) {
            WindowModeLogf(L"[窗口模式] CDP 停放 Move 失败: %s", desk.LastError().c_str());
            holdView(150);
            return false;
        }
    }

    ClearMacroDesktopTaskbarPreviewSuppression(hwnd);
    const bool live = CdpHideLiveOffscreen(hwnd, viewBefore);
    holdView(80);
    WindowModeLogf(L"[窗口模式] CDP 停放: iconic=%d live=%d（Move+Pin屏外；禁异桌裸还原）",
        IsIconic(hwnd) ? 1 : 0, live ? 1 : 0);
    return live || alreadyOnMacro;
}

void PrepareMacroDesktopForCdpBind(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    ParkCdpBrowserOnMacroDesktop(hwnd);
}

bool EnsureCdpBrowserLiveFrames(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (UserOnMacroDesktopNow()) {
        if (!WindowNeedsCdpReveal(hwnd)) return true;
        return CdpRevealOnMacroForWatch(hwnd);
    }
    if (IsCdpLiveOffscreenParked(hwnd)) return true;
    const int viewBefore = VirtualDesktopAccessor::Instance().GetCurrentDesktopNumber();
    return CdpHideLiveOffscreen(hwnd, viewBefore);
}

bool EnsureTargetOnMacroDesktop(HWND hwnd, bool minimizeIfMoved) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return false;

    // Pin+屏外：已在宏桌面工作面；VDA 常报 desk=-1。再 Move/SoftRestore 会拆停放并切屏。
    if (IsCdpLiveOffscreenParked(hwnd)) return true;

    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (!vda.EnsureLoaded(err)) return false;
    const int macroIdx = vda.FindDesktopIndexByName(kMacroDesktopDisplayName);
    if (macroIdx >= 0 && vda.IsWindowOnDesktopNumber(hwnd, macroIdx)) {
        return true;
    }
    // 已 Pin（含尚未移到屏外的瞬间）：勿 Move，避免 PreservingView 失败切屏。
    if (vda.IsPinnedWindow(hwnd) > 0) return true;

    MacroVirtualDesktop desk;
    if (!desk.OpenOrCreate()) return false;
    if (minimizeIfMoved) {
        MinimizeForQuietDesktopMove(hwnd);
    }
    if (!desk.MoveWindowToMacroDesktop(hwnd)) return false;
    if (!minimizeIfMoved) {
        SoftRestoreNonActivating(hwnd);
    }
    StripCloakAndNearInvisibleAlpha(hwnd);
    ClearMacroDesktopTaskbarPreviewSuppression(hwnd);
    return true;
}

void ScheduleCdpParkViewPin(int /*preferDesk*/, int /*durationMs*/) {
    // 已废止：持续 GoTo 钉视违反「只搬窗」。
}

void StartCdpMacroDesktopWatchPump(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    std::lock_guard<std::mutex> lock(g_watchMu);
    g_watchStop.store(true, std::memory_order_release);
    g_watchHwnd.store(nullptr, std::memory_order_release);
    if (g_watchThread.joinable()) {
        g_watchThread.join();
    }
    g_watchStop.store(false, std::memory_order_release);
    g_watchHwnd.store(hwnd, std::memory_order_release);
    g_watchThread = std::thread([] {
        int onMacroStreak = 0;
        int offMacroStreak = 0;
        int revealTries = 0;
        int sinceRevealTick = 0;
        bool revealed = false;
        while (!g_watchStop.load(std::memory_order_acquire)) {
            HWND h = g_watchHwnd.load(std::memory_order_acquire);
            const bool onMacro = h && IsWindow(h) && UserOnMacroDesktopNow();
            if (onMacro) {
                offMacroStreak = 0;
                // ~0.3s 确认即揭开（原 ~4s 用户觉得展开太慢）；离桌仍用更长确认防误收
                if (++onMacroStreak >= 3) {
                    if (!WindowNeedsCdpReveal(h)) {
                        revealed = true;
                        revealTries = 0;
                        sinceRevealTick = 0;
                    } else {
                        ++sinceRevealTick;
                        // 首揭；失败后再隔 ~1s 重试（最多 3 次）
                        if (revealTries == 0 || (revealTries < 3 && sinceRevealTick >= 10)) {
                            RaiseMacroDesktopWindowForWatch(h);
                            ++revealTries;
                            sinceRevealTick = 0;
                            if (!WindowNeedsCdpReveal(h)) {
                                revealed = true;
                            } else {
                                RECT wr{};
                                if (GetWindowRect(h, &wr) && !IsIconic(h)
                                    && PlacementLooksOnScreen(wr)) {
                                    revealed = true;
                                }
                            }
                        }
                    }
                    onMacroStreak = 3;
                }
            } else if (h && IsWindow(h)) {
                onMacroStreak = 0;
                // ~1s 确认离桌
                if (++offMacroStreak >= 10) {
                    if (revealed && !IsCdpLiveOffscreenParked(h)
                        && !UserOnMacroDesktopNow()) {
                        const int view = VirtualDesktopAccessor::Instance().GetCurrentDesktopNumber();
                        if (CdpHideLiveOffscreen(h, view)) {
                            WindowModeLog(L"[窗口模式] 用户已离宏桌面：已回 Pin+屏外（防切屏）");
                        }
                        revealed = false;
                        revealTries = 0;
                        sinceRevealTick = 0;
                    }
                    offMacroStreak = 10;
                }
            } else {
                onMacroStreak = 0;
                offMacroStreak = 0;
                revealed = false;
                revealTries = 0;
                sinceRevealTick = 0;
            }
            // 100ms 一轮
            for (int i = 0; i < 2 && !g_watchStop.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });
}

void SignalStopCdpMacroDesktopWatchPump() {
    g_watchStop.store(true, std::memory_order_release);
    g_watchHwnd.store(nullptr, std::memory_order_release);
}

void StopCdpMacroDesktopWatchPump() {
    SignalStopCdpMacroDesktopWatchPump();
    std::lock_guard<std::mutex> lock(g_watchMu);
    if (g_watchThread.joinable()) {
        g_watchThread.join();
    }
}

}  // namespace windowmode
