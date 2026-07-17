#include "window_target.h"

#include "macro_virtual_desktop.h"
#include "window_mode_log.h"
#include "virtual_desktop_accessor.h"
#include "window_capture.h"

#include <algorithm>
#include <chrono>
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

bool TitleMatches(const std::wstring& title, const std::wstring& titleContains) {
    if (titleContains.empty()) return true;
    const std::wstring titleLower = ToLowerCopy(title);
    const std::wstring needleLower = ToLowerCopy(titleContains);
    return titleLower.find(needleLower) != std::wstring::npos;
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

    static const wchar_t* kRenderClasses[] = {
        L"Chrome_RenderWidgetHostHWND",
        L"Internet Explorer_Server",
    };
    HWND best = nullptr;
    int bestArea = 0;
    for (const wchar_t* cls : kRenderClasses) {
        if (HWND found = FindLargestChildByClass(top, cls)) {
            RECT rc{};
            if (!GetClientRect(found, &rc)) continue;
            const int area = std::max(0, static_cast<int>(rc.right - rc.left))
                * std::max(0, static_cast<int>(rc.bottom - rc.top));
            if (area > bestArea) {
                bestArea = area;
                best = found;
            }
        }
    }
    return best;
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

}  // namespace windowmode
