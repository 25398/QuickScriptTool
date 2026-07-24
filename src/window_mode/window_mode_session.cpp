#include "window_mode_session.h"

#include "background_window_input.h"
#include "window_capture.h"
#include "window_coords.h"
#include "window_target.h"

#include "window_mode_log.h"
#include "window_mode_requirements.h"
#include "macro_virtual_desktop.h"
#include "virtual_desktop_accessor.h"
#include "image_match.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace windowmode {

namespace {

void DebugLog(const wchar_t* msg) {
    WindowModeLog(msg);
}

std::wstring ResolveExePath(const std::wstring& target) {
    if (target.empty()) return L"";

    DWORD attrs = GetFileAttributesW(target.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return target;
    }

    wchar_t buf[MAX_PATH]{};
    DWORD found = SearchPathW(nullptr, target.c_str(), nullptr, MAX_PATH, buf, nullptr);
    if (found > 0 && found < MAX_PATH) return buf;
    return target;
}

bool IsUrlPath(const std::wstring& path) {
    return path.size() >= 8
        && (_wcsnicmp(path.c_str(), L"https://", 8) == 0
            || (path.size() >= 7 && _wcsnicmp(path.c_str(), L"http://", 7) == 0));
}

bool IsDocumentPath(const std::wstring& path) {
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

bool ShouldOpenViaShell(const std::wstring& path) {
    return IsUrlPath(path) || IsDocumentPath(path);
}

std::wstring TrimCopy(std::wstring value) {
    const auto b = value.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    const auto e = value.find_last_not_of(L" \t\r\n");
    return value.substr(b, e - b + 1);
}

std::wstring ExtractDocumentLaunchArg(const WindowModeScriptConfig& config) {
    if (!config.launchArgs.empty()) return L"";

    std::wstring title = !config.windowName.empty()
        ? config.windowName : config.targetWindowTitle;
    title = TrimCopy(title);
    if (title.empty()) return L"";

    const auto dash = title.find(L" - ");
    if (dash != std::wstring::npos) {
        title = TrimCopy(title.substr(0, dash));
    }
    if (title.empty() || title == L"\u65e0\u6807\u9898" || _wcsicmp(title.c_str(), L"Untitled") == 0) {
        return L"";
    }

    // ?
    if (title.find(L':') != std::wstring::npos || title.find(L'\\') != std::wstring::npos
        || title.find(L'/') != std::wstring::npos) {
        return L"\"" + title + L"\"";
    }
    return L"";
}

std::wstring DocumentNameFromWindowTitle(const std::wstring& windowTitle) {
    std::wstring title = TrimCopy(windowTitle);
    if (title.empty()) return L"";

    const auto dash = title.find(L" - ");
    if (dash != std::wstring::npos) {
        title = TrimCopy(title.substr(0, dash));
    }
    if (title.empty() || title == L"\u65e0\u6807\u9898" || _wcsicmp(title.c_str(), L"Untitled") == 0) {
        return L"";
    }
    if (IsDocumentPath(title) || title.find(L':') != std::wstring::npos
        || title.find(L'\\') != std::wstring::npos || title.find(L'/') != std::wstring::npos) {
        return title;
    }
    return L"";
}

bool FilePathExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring JoinDirFile(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring StripDocumentStemFromTitle(std::wstring title) {
    title = TrimCopy(title);
    if (title.empty()) return L"";
    const auto dash = title.find(L" - ");
    if (dash != std::wstring::npos) {
        title = TrimCopy(title.substr(0, dash));
    }
    while (!title.empty() && (title.front() == L'*' || title.front() == L' ')) {
        title.erase(title.begin());
    }
    title = TrimCopy(title);
    if (title.empty() || title == L"\u65e0\u6807\u9898" || _wcsicmp(title.c_str(), L"Untitled") == 0) {
        return L"";
    }
    return title;
}

std::wstring ResolveDocumentFileToOpen(const WindowModeScriptConfig& config,
    const std::wstring& searchDir) {
    // NoSelect: launch exe only ? ignore leftover launchArgs / title document identity.
    if (config.selectMethod == WindowSelectMethod::NoSelect) return L"";

    auto unquote = [](std::wstring arg) {
        arg = TrimCopy(arg);
        if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"') {
            arg = arg.substr(1, arg.size() - 2);
        }
        return TrimCopy(arg);
    };
    auto isFullPath = [](const std::wstring& p) {
        return p.size() >= 2
            && (p[1] == L':' || p.find(L'\\') != std::wstring::npos
                || p.find(L'/') != std::wstring::npos);
    };
    auto tryResolve = [&](std::wstring arg) -> std::wstring {
        arg = unquote(arg);
        if (arg.empty()) return L"";
        if (isFullPath(arg) && FilePathExists(arg)) return arg;
        if (!searchDir.empty()) {
            const std::wstring nameOnly = [&]() {
                const auto slash = arg.find_last_of(L"\\/");
                return slash == std::wstring::npos ? arg : arg.substr(slash + 1);
            }();
            if (!nameOnly.empty()) {
                const std::wstring full = JoinDirFile(searchDir, nameOnly);
                if (FilePathExists(full)) return full;
            }
            if (!isFullPath(arg)) {
                const std::wstring full = JoinDirFile(searchDir, arg);
                if (FilePathExists(full)) return full;
            }
        }
        return L"";
    };

    if (!config.launchArgs.empty()) {
        if (std::wstring hit = tryResolve(config.launchArgs); !hit.empty()) return hit;
    }

    // ?
    const std::wstring title = !config.windowName.empty()
        ? config.windowName : config.targetWindowTitle;
    const std::wstring stem = StripDocumentStemFromTitle(title);
    if (!stem.empty()) {
        if (std::wstring hit = tryResolve(stem); !hit.empty()) return hit;
        // ?
        if (isFullPath(stem) && FilePathExists(stem)) return stem;
    }

    std::wstring fromTitle = ExtractDocumentLaunchArg(config);
    if (!fromTitle.empty()) {
        if (std::wstring hit = tryResolve(fromTitle); !hit.empty()) return hit;
    }

    // ?
    return L"";
}

std::wstring EffectiveLaunchArgs(const WindowModeScriptConfig& config) {
    // NoSelect: direct exe launch; do not pass leftover CLI / document args.
    if (config.selectMethod == WindowSelectMethod::NoSelect) return L"";
    if (!config.launchArgs.empty()) return config.launchArgs;
    return ExtractDocumentLaunchArg(config);
}

HWND ResolveBindHwnd(HWND top, const WindowModeScriptConfig& config, bool background) {
    if (!top || !IsWindow(top)) return nullptr;

    HWND bindHwnd = top;
    if (!config.childWindowClassName.empty()) {
        HWND child = FindChildWindowByClass(top, config.childWindowClassName);
        if (child) {
            bindHwnd = child;
        } else if (HWND render = FindBrowserRenderWidget(top)) {
            bindHwnd = render;
        } else if (HWND input = FindTextInputTarget(top)) {
            bindHwnd = input;
        }
    } else if (HWND render = FindBrowserRenderWidget(top)) {
        // Prefer browser content widget so message clicks land in the page/game surface.
        bindHwnd = render;
    } else if (background) {
        if (HWND input = FindTextInputTarget(top)) {
            bindHwnd = input;
        }
    }
    return bindHwnd;
}

bool ResolveInputBindHwnd(HWND top, const WindowModeScriptConfig& config, bool background,
    HWND& outHwnd) {
    outHwnd = nullptr;
    if (!top || !IsWindow(top)) return false;

    HWND bindHwnd = ResolveBindHwnd(top, config, background);
    if (bindHwnd && bindHwnd != top) {
        outHwnd = bindHwnd;
        return true;
    }
    if (!config.childWindowClassName.empty()) {
        if (HWND child = FindChildWindowByClass(top, config.childWindowClassName)) {
            outHwnd = child;
            return true;
        }
    }
    if (HWND input = FindTextInputTarget(top)) {
        outHwnd = input;
        return true;
    }
    outHwnd = top;
    return true;
}

bool LaunchViaShell(const std::wstring& path, const std::wstring& args,
    PROCESS_INFORMATION& pi, std::wstring& err, int nShow = SW_SHOWNOACTIVATE) {
    ZeroMemory(&pi, sizeof(pi));
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    // ?
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    if (!args.empty()) sei.lpParameters = args.c_str();
    sei.nShow = nShow;
    if (!ShellExecuteExW(&sei)) {
        err = L"ShellExecute \u5931\u8d25: " + path;
        return false;
    }
    if (sei.hProcess) {
        pi.hProcess = sei.hProcess;
        pi.hThread = nullptr;
        pi.dwProcessId = GetProcessId(sei.hProcess);
        pi.dwThreadId = 0;
    }
            WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] ShellExecute \u5df2\u542f\u52a8 path=%s pid=%lu",
        path.c_str(), static_cast<unsigned long>(pi.dwProcessId));
    return true;
}

std::wstring QueryAssocExecutable(const std::wstring& filePath) {
    const auto dot = filePath.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= filePath.size()) return L"";

    std::wstring ext = filePath.substr(dot);
    wchar_t exe[MAX_PATH]{};
    DWORD cch = MAX_PATH;
    const HRESULT hr = AssocQueryStringW(
        ASSOCF_INIT_IGNOREUNKNOWN | ASSOCF_NOFIXUPS,
        ASSOCSTR_EXECUTABLE,
        ext.c_str(),
        nullptr,
        exe,
        &cch);
    if (FAILED(hr) || exe[0] == L'\0') return L"";
    return exe;
}

bool HandlerPathExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsStorePackagedHandler(const std::wstring& handler) {
    return handler.find(L"WindowsApps") != std::wstring::npos;
}

std::wstring ResolveDocumentHandler(const std::wstring& filePath) {
    const auto dot = filePath.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= filePath.size()) return L"";

    std::wstring ext = filePath.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });

    if (ext == L".txt" || ext == L".log" || ext == L".md" || ext == L".csv") {
        static const wchar_t* kClassicNotepad[] = {
            L"C:\\Windows\\System32\\notepad.exe",
            L"C:\\Windows\\notepad.exe",
        };
        for (const wchar_t* candidate : kClassicNotepad) {
            if (HandlerPathExists(candidate)) return candidate;
        }
    }

    const std::wstring assoc = QueryAssocExecutable(filePath);
    if (assoc.empty() || IsStorePackagedHandler(assoc)) return L"";
    return assoc;
}

bool SquashDocumentWindowOnMacroDesktop(const std::wstring& docPath, HiddenDesktop& desktop,
    const std::atomic_bool* cancelFlag) {
    std::wstring name = docPath;
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    WindowTargetQuery query{};
    if (!name.empty()) query.titleContains = name;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (std::chrono::steady_clock::now() < deadline) {
        if (WindowModeCancelled(cancelFlag)) return false;
        HWND hwnd = FindMainWindowDefault(query, true);
        if (!hwnd) hwnd = FindMainWindowDefault(query, false);
        if (hwnd) {
            HWND top = GetAncestor(hwnd, GA_ROOT);
            if (!top) top = hwnd;
            ShowWindow(top, SW_SHOWMINNOACTIVE);
            desktop.MoveWindowToMacroDesktop(top);
            PinMacroDesktopWindowBottom(top);
            ShowWindow(top, SW_SHOWMINNOACTIVE);
            return true;
        }
        WindowModeSleepInterruptible(cancelFlag, std::chrono::milliseconds(10));
    }
    return false;
}

bool LaunchDocumentOnMacroDesktop(const std::wstring& docPath, const std::wstring& args,
    HiddenDesktop& desktop, PROCESS_INFORMATION& pi, std::wstring& err,
    const std::atomic_bool* /*cancelFlag*/) {
    const std::wstring handler = ResolveDocumentHandler(docPath);
    if (handler.empty()) return false;

    std::wstring launchArgs = docPath;
    if (!args.empty()) launchArgs += L" " + args;

    std::wstring expectTitle = docPath;
    const auto slash = expectTitle.find_last_of(L"\\/");
    if (slash != std::wstring::npos) expectTitle = expectTitle.substr(slash + 1);

    if (!desktop.LaunchProcess(handler, launchArgs, pi, expectTitle)) {
        err = desktop.LastError();
        return false;
    }
    WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u6587\u6863\u5904\u7406\u5668\u542f\u52a8: %s %s", handler.c_str(), launchArgs.c_str());
    return true;
}

bool WaitForBindHwnd(HWND top, const WindowModeScriptConfig& config, bool background,
    const std::atomic_bool* cancelFlag, HWND& outHwnd) {
    outHwnd = nullptr;
    if (!top || !IsWindow(top)) return false;

    // ?
    const bool needsChild = !config.childWindowClassName.empty()
        || (background && config.selectMethod != WindowSelectMethod::NoSelect);
    if (!needsChild) {
        outHwnd = top;
        return true;
    }

    if (ResolveInputBindHwnd(top, config, background, outHwnd) && outHwnd != top) {
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
        if (WindowModeCancelled(cancelFlag)) return false;
        HWND bindHwnd = ResolveBindHwnd(top, config, background);
        if (bindHwnd && bindHwnd != top) {
            outHwnd = bindHwnd;
            return true;
        }
        if (!config.childWindowClassName.empty()) {
            if (HWND child = FindChildWindowByClass(top, config.childWindowClassName)) {
                outHwnd = child;
                return true;
            }
        } else if (background) {
            if (HWND input = FindTextInputTarget(top)) {
                outHwnd = input;
                return true;
            }
        }
        WindowModeSleepInterruptible(cancelFlag, std::chrono::milliseconds(25));
    }

    return ResolveInputBindHwnd(top, config, background, outHwnd);
}

bool ApplyBoundTargetState(HWND top, HWND bindHwnd, WindowModeSessionState& state,
    const WindowModeScriptConfig& config, std::wstring& err, bool logBind = true,
    bool runHealthCheck = true) {
    if (!top || !bindHwnd || !IsWindow(bindHwnd)) {
        err = L"\u65e0\u6548\u7a97\u53e3\u53e5\u67c4";
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(top, &pid);

    state.targetHwnd = bindHwnd;
    state.targetPid = pid;

    RECT clientRc{};
    GetClientRect(bindHwnd, &clientRc);
    state.clientW = std::max(0, static_cast<int>(clientRc.right - clientRc.left));
    state.clientH = std::max(0, static_cast<int>(clientRc.bottom - clientRc.top));
    // Pin+屏外时 Chromium 偶发回报缩略客户区（如 215x28），会导致找图映射/点击全偏。
    if (state.clientW < 400 || state.clientH < 300) {
        HWND root = GetAncestor(bindHwnd, GA_ROOT);
        if (!root) root = top;
        RECT rootRc{};
        if (root && GetClientRect(root, &rootRc)) {
            const int rw = std::max(0, static_cast<int>(rootRc.right - rootRc.left));
            const int rh = std::max(0, static_cast<int>(rootRc.bottom - rootRc.top));
            if (rw >= 400 && rh >= 300) {
                state.clientW = rw;
                state.clientH = rh;
                bindHwnd = root;
            }
        }
        if (state.clientW < 400 || state.clientH < 300) {
            RECT wr{};
            if (root && GetWindowRect(root, &wr)) {
                const int ww = std::max(0, static_cast<int>(wr.right - wr.left));
                const int wh = std::max(0, static_cast<int>(wr.bottom - wr.top));
                if (ww >= 640 && wh >= 400) {
                    state.clientW = ww;
                    state.clientH = wh;
                    WindowModeLogf(L"[窗口模式] 客户区异常，改用窗口外框 %dx%d", ww, wh);
                }
            }
        }
    }

    int sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
    if (!MapClientRectToScreen(bindHwnd, 0, 0, state.clientW, state.clientH, sx1, sy1, sx2, sy2)) {
        err = L"\u5ba2\u6237\u533a\u5750\u6807\u6620\u5c04\u5931\u8d25";
        state.targetHwnd = nullptr;
        state.targetPid = 0;
        return false;
    }
    state.clientRectScreen = RECT{sx1, sy1, sx2, sy2};

    if (config.executionKind == WindowModeExecutionKind::BackgroundWindow) {
        if (runHealthCheck) {
            // ?
            DWORD pidCheck = 0;
            GetWindowThreadProcessId(top, &pidCheck);
            HANDLE process = pidCheck
                ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pidCheck) : nullptr;
            if (!process && pidCheck != 0) {
                state.health = WindowModeHealth::PermissionMismatch;
                err = HealthToUserHint(state.health);
                state.lastError = err;
                return false;
            }
            if (process) CloseHandle(process);
            state.health = WindowModeHealth::Ok;
        } else {
            state.health = WindowModeHealth::Ok;
        }
    } else {
        state.health = WindowModeHealth::Ok;
    }

    wchar_t title[256]{};
    GetWindowTextW(top, title, 256);
    if (logBind) {
            WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u5df2\u7ed1\u5b9a pid=%lu top=0x%p bind=0x%p title=%s",
            static_cast<unsigned long>(pid), top, bindHwnd, title);
    }
    return true;
}

bool IsStorePackagedPath(const std::wstring& path) {
    return path.find(L"WindowsApps") != std::wstring::npos
        || path.find(L"windowsapps") != std::wstring::npos;
}

// ?
/// ?? ...\Microsoft.WindowsNotepad_11.x_x64__8wekyb3d8bbwe\...
///  ??Microsoft.WindowsNotepad_8wekyb3d8bbwe
bool TryParsePackageFamilyName(const std::wstring& path, std::wstring& outPfn) {
    outPfn.clear();
    std::wstring lower = path;
    for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    auto pos = lower.find(L"windowsapps\\");
    size_t prefixLen = 12;
    if (pos == std::wstring::npos) {
        pos = lower.find(L"windowsapps/");
        prefixLen = 12;
    }
    if (pos == std::wstring::npos) return false;
    const size_t start = pos + prefixLen;
    const size_t end = path.find_first_of(L"\\/", start);
    if (end == std::wstring::npos || end <= start) return false;

    const std::wstring fullName = path.substr(start, end - start);
    const auto dbl = fullName.rfind(L"__");
    if (dbl == std::wstring::npos || dbl + 2 >= fullName.size()) return false;
    const std::wstring publisher = fullName.substr(dbl + 2);
    const auto firstUnderscore = fullName.find(L'_');
    if (firstUnderscore == std::wstring::npos || firstUnderscore == 0) return false;
    const std::wstring pkgName = fullName.substr(0, firstUnderscore);
    if (publisher.empty() || pkgName.empty()) return false;
    outPfn = pkgName + L"_" + publisher;
    return true;
}

bool LaunchStoreAppFromPath(const std::wstring& exePath, PROCESS_INFORMATION& pi, std::wstring& err) {
    std::wstring pfn;
    if (!TryParsePackageFamilyName(exePath, pfn)) return false;

    // ?
    // ?
    const std::wstring uri = L"shell:AppsFolder\\" + pfn + L"!App";
    WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u5c1d\u8bd5 AppsFolder \u542f\u52a8\u5546\u5e97\u5e94\u7528: %s", uri.c_str());
    if (LaunchViaShell(uri, L"", pi, err, SW_SHOWMINNOACTIVE)) return true;

    WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] AppsFolder \u5931\u8d25\uff0c\u56de\u9000 ShellExecute \u76f4\u63a5\u6253\u5f00 exe");
    err.clear();
    return LaunchViaShell(exePath, L"", pi, err, SW_SHOWMINNOACTIVE);
}

void SquashBackgroundLaunchedWindow(HWND hwnd, const std::atomic_bool* cancelFlag) {
    if (!hwnd || !IsWindow(hwnd)) return;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    ForceHideLaunchedWindowQuiet(hwnd);
    HideTransientGdiPlusWindows(pid);
    for (int i = 0; i < 4 && !IsIconic(hwnd); ++i) {
        WindowModeSleepInterruptible(cancelFlag, std::chrono::milliseconds(40));
        ForceHideLaunchedWindowQuiet(hwnd);
    }
    HideTransientGdiPlusWindows(pid);
}

bool PidLooksLikeLaunchTarget(DWORD pid, const std::wstring& exePath) {
    if (pid == 0 || exePath.empty()) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok || path[0] == L'\0') return false;

    auto toLower = [](std::wstring s) {
        for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
        return s;
    };
    const std::wstring proc = toLower(path);
    const std::wstring target = toLower(exePath);
    const auto slashP = proc.find_last_of(L"\\/");
    const auto slashT = target.find_last_of(L"\\/");
    const std::wstring procFile = slashP == std::wstring::npos ? proc : proc.substr(slashP + 1);
    const std::wstring targetFile = slashT == std::wstring::npos ? target : target.substr(slashT + 1);
    if (!targetFile.empty() && procFile == targetFile) {
        const bool targetStore = target.find(L"windowsapps") != std::wstring::npos;
        const bool procStore = proc.find(L"windowsapps") != std::wstring::npos;
        if (targetStore == procStore) return true;
        if (targetStore && procStore) return true;
    }
    std::wstring pfnA;
    std::wstring pfnB;
    if (TryParsePackageFamilyName(path, pfnA) && TryParsePackageFamilyName(exePath, pfnB)
        && _wcsicmp(pfnA.c_str(), pfnB.c_str()) == 0) {
        return true;
    }
    return false;
}

bool HwndInExcludeList(HWND wnd, const std::vector<HWND>& exclude) {
    for (HWND e : exclude) {
        if (e == wnd) return true;
    }
    return false;
}

// ?
// ?

bool PreferredClassLooksLikeChildControl(const std::wstring& preferredClass) {
    if (preferredClass.empty()) return false;
    return preferredClass.find(L"RichEdit") != std::wstring::npos
        || _wcsicmp(preferredClass.c_str(), L"Edit") == 0
        || preferredClass.find(L"Scintilla") != std::wstring::npos
        || preferredClass.find(L"Chrome_RenderWidget") != std::wstring::npos;
}

HWND FindNewlyLaunchedTargetWindow(const std::wstring& exePath,
    const std::vector<HWND>& existingBefore,
    const std::wstring& preferredClass, const std::wstring& titleHint) {
    // ?
    // ?
    const bool childLike = PreferredClassLooksLikeChildControl(preferredClass);
    const std::wstring topClass = (!preferredClass.empty() && !childLike)
        ? preferredClass : L"";

    auto accept = [&](HWND wnd) -> HWND {
        if (!wnd || !IsWindow(wnd)) return nullptr;
        if (HwndInExcludeList(wnd, existingBefore)) return nullptr;
        if (IsLikelyImeOrToolWindow(wnd)) return nullptr;
        if (!topClass.empty()) {
            wchar_t cls[256]{};
            GetClassNameW(wnd, cls, 256);
            if (_wcsicmp(cls, topClass.c_str()) != 0) return nullptr;
        }
        if (!titleHint.empty()) {
            wchar_t title[512]{};
            GetWindowTextW(wnd, title, 512);
            if (title[0] != L'\0') {
                std::wstring titleLower = title;
                std::wstring tipLower = titleHint;
                for (auto& ch : titleLower) ch = static_cast<wchar_t>(towlower(ch));
                for (auto& ch : tipLower) ch = static_cast<wchar_t>(towlower(ch));
                if (titleLower.find(tipLower) == std::wstring::npos) return nullptr;
            }
        }
        return wnd;
    };

    WindowTargetQuery q{};
    q.exePath = exePath;
    q.className = topClass;
    q.allowStoreNotepadHandoff = true;
    if (!titleHint.empty()) q.titleContains = titleHint;

    HWND wnd = accept(FindNewlyAppearedMainWindow(q, existingBefore, true));
    if (!wnd && !titleHint.empty()) {
        q.titleContains.clear();
        wnd = accept(FindNewlyAppearedMainWindow(q, existingBefore, true));
    }
    if (!wnd && !topClass.empty()) {
        // ?
        WindowTargetQuery byExe{};
        byExe.exePath = exePath;
        byExe.allowStoreNotepadHandoff = true;
        wnd = accept(FindNewlyAppearedMainWindow(byExe, existingBefore, true));
    }
    return wnd;
}

HWND WaitAndSquashLaunchedWindow(const std::wstring& exePath,
    const std::vector<HWND>& existingBefore,
    const FILETIME& launchTimeUtc,
    const std::wstring& preferredClass, const std::wstring& titleHint,
    PROCESS_INFORMATION& pi, const std::atomic_bool* cancelFlag) {
    // ?
    WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] \u7b49\u5f85\u542f\u52a8\u7a97\u53e3\u51fa\u73b0\u2026");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    HWND found = nullptr;
    int tick = 0;
    const bool childLike = PreferredClassLooksLikeChildControl(preferredClass);
    const std::wstring topClass = (!preferredClass.empty() && !childLike)
        ? preferredClass : L"";

    while (std::chrono::steady_clock::now() < deadline) {
        if (WindowModeCancelled(cancelFlag)) break;

        found = nullptr;
        if (pi.dwProcessId != 0) {
            WindowTargetQuery byPid{};
            byPid.pid = pi.dwProcessId;
            byPid.exePath = exePath;
            byPid.className = topClass;
            HWND cand = FindMainWindowDefault(byPid, true);
            if (!cand && !topClass.empty()) {
                byPid.className.clear();
                cand = FindMainWindowDefault(byPid, true);
            }
            if (cand && !HwndInExcludeList(cand, existingBefore)
                && !IsLikelyImeOrToolWindow(cand)) {
                found = cand;
            }
        }
        if (!found) {
            found = FindNewlyLaunchedTargetWindow(exePath, existingBefore, preferredClass, titleHint);
        }
        if (!found) {
            WindowTargetQuery byExe{};
            byExe.exePath = exePath;
            byExe.className = topClass;
            byExe.allowStoreNotepadHandoff = true;
            found = FindLaunchResultMainWindow(byExe, existingBefore, launchTimeUtc, false, true);
            if (!found && !topClass.empty()) {
                byExe.className.clear();
                found = FindLaunchResultMainWindow(byExe, existingBefore, launchTimeUtc, false, true);
            }
        }

        if (found && IsWindow(found)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(found, &pid);
            if (pid != 0) pi.dwProcessId = pid;
            wchar_t cls[128]{};
            wchar_t title[256]{};
            GetClassNameW(found, cls, 128);
            GetWindowTextW(found, title, 256);
            WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u627e\u5230\u542f\u52a8\u7a97\u53e3 hwnd=0x%p pid=%lu class=%s title=%s",
                found, static_cast<unsigned long>(pid), cls, title);
            ForceHideLaunchedWindowQuiet(found);
            WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] \u5df2\u9759\u9ed8\u7f29\u5c0f");
            return found;
        }

        if ((++tick % 8) == 0) {
            WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u4ecd\u5728\u7b49\u5f85\u542f\u52a8\u7a97\u2026%d ms", tick * 40);
        }
        WindowModeSleepInterruptible(cancelFlag, std::chrono::milliseconds(40));
    }

    // ?
    WindowTargetQuery byExe{};
    byExe.exePath = exePath;
    byExe.allowStoreNotepadHandoff = true;
    found = FindLaunchResultMainWindow(byExe, existingBefore, launchTimeUtc, true, true);
    if (!found) {
        found = FindLaunchResultMainWindow(byExe, existingBefore, launchTimeUtc, true, false);
    }
    if (found && IsWindow(found)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(found, &pid);
        if (pid != 0) pi.dwProcessId = pid;
        ForceHideLaunchedWindowQuiet(found);
        WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] \u8d85\u65f6\u540e\u590d\u7528\u5df2\u6709\u5339\u914d\u7a97");
        return found;
    }
    WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] \u7b49\u5f85\u542f\u52a8\u7a97\u53e3\u8d85\u65f6");
    return found;
}

bool TryCreateProcessLaunch(const std::wstring& exePath, const std::wstring& args,
    PROCESS_INFORMATION& pi, std::wstring& err) {
    ZeroMemory(&pi, sizeof(pi));
    std::wstring arg = args;
    while (!arg.empty() && (arg.front() == L' ' || arg.front() == L'\t')) arg.erase(arg.begin());
    while (!arg.empty() && (arg.back() == L' ' || arg.back() == L'\t')) arg.pop_back();
    if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"') {
        arg = arg.substr(1, arg.size() - 2);
    }

    std::wstring cmdLine = L"\"" + exePath + L"\"";
    if (!arg.empty()) {
        if (arg.find_first_of(L" \t") != std::wstring::npos) {
            cmdLine += L" \"" + arg + L"\"";
        } else {
            cmdLine += L" " + arg;
        }
    }
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    std::wstring workDir;
    const auto slash = exePath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) workDir = exePath.substr(0, slash);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;

    // ?
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
            0, nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        err = L"CreateProcess \u5931\u8d25";
        return false;
    }
            WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] CreateProcess \u5df2\u542f\u52a8 pid=%lu cmd=%s",
        static_cast<unsigned long>(pi.dwProcessId), cmdLine.c_str());
    return true;
}

bool LaunchExeOnDefaultDesktop(const std::wstring& exePath, const std::wstring& args,
    PROCESS_INFORMATION& pi, std::wstring& err) {
    // ?
    if (IsStorePackagedPath(exePath)) {
        std::wstring cpErr;
        if (TryCreateProcessLaunch(exePath, args, pi, cpErr)) return true;
        WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] CreateProcess \u5931\u8d25\uff0c\u5c1d\u8bd5 AppsFolder (%s)", cpErr.c_str());
        if (args.empty() && LaunchStoreAppFromPath(exePath, pi, err)) return true;
        return LaunchViaShell(exePath, args, pi, err, SW_SHOWMINNOACTIVE);
    }

    return TryCreateProcessLaunch(exePath, args, pi, err);
}

HWND TopLevelWindow(HWND hwnd) {
    if (!hwnd) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

std::wstring ResolveLaunchPath(const WindowModeScriptConfig& config, const std::wstring& searchDir) {
    // ?
    // ?
    std::wstring path = ResolveExePath(config.targetExePath);
    if (path.empty()) return path;

    if (ShouldOpenViaShell(path)) {
        if (FilePathExists(path)) return path;
        if (!searchDir.empty()
            && path.find(L'\\') == std::wstring::npos
            && path.find(L':') == std::wstring::npos
            && IsDocumentPath(path)) {
            const std::wstring full = JoinDirFile(searchDir, path);
            if (FilePathExists(full)) return full;
        }
        return path;
    }

    // ?
    (void)searchDir;
    return path;
}

}  // namespace

WindowModeSession::~WindowModeSession() {
    Stop();
}

bool WindowModeSession::EnsureDesktop(std::wstring& err) {
    if (IsCancelled()) {
        err = L"\u5df2\u53d6\u6d88";
        state_.lastError = err;
        return false;
    }
    if (config_.executionKind == WindowModeExecutionKind::BackgroundWindow) {
        state_.macroDesktopId = GUID{};
        state_.macroDesktopIndex = -1;
        return true;
    }
    if (!desktop_.OpenOrCreate()) {
        state_.health = WindowModeHealth::DesktopNotReady;
        err = desktop_.LastError();
        state_.lastError = err;
        return false;
    }
    state_.macroDesktopId = desktop_.DesktopId();
    state_.macroDesktopIndex = desktop_.DesktopIndex();
    return true;
}

bool WindowModeSession::BindTargetWindow(std::wstring& err) {
    if (IsCancelled()) {
        err = L"\u5df2\u53d6\u6d88";
        state_.lastError = err;
        return false;
    }

    const bool background = config_.executionKind == WindowModeExecutionKind::BackgroundWindow;
    if (!background && !desktop_.IsValid()) {
        state_.health = WindowModeHealth::DesktopNotReady;
        err = L"\u5b8f\u865a\u62df\u684c\u9762\u672a\u5c31\u7eea";
        state_.lastError = err;
        return false;
    }

    WindowTargetQuery query = BuildTargetQuery(config_);

    const int macroDesktopIndex = desktop_.DesktopIndex();

    auto findOnMacroDesktop = [&](const WindowTargetQuery& q, bool allowMinimized) -> HWND {
        HWND found = FindMainWindowOnVirtualDesktopIndex(q, macroDesktopIndex, allowMinimized);
        if (!found) found = FindMainWindowOnVirtualDesktop(q, desktop_.DesktopId(), allowMinimized);
        return found;
    };
    auto findAnywhere = [&](const WindowTargetQuery& q, bool allowMinimized) -> HWND {
        return FindMainWindowDefault(q, allowMinimized);
    };

    auto moveCandidateToMacro = [&](HWND candidate, const WindowTargetQuery& refindQuery) -> HWND {
        if (IsCancelled()) return nullptr;
        if (!candidate) return nullptr;
        candidate = TopLevelWindow(candidate);
        if (IsWindowOnVirtualDesktopIndex(candidate, macroDesktopIndex)) {
            if (HWND found = findOnMacroDesktop(refindQuery, true)) return found;
            return candidate;
        }
        const bool cdp = UsesCdpInput(config_);
        if (cdp) {
            // CDP：Park = Minimize→Move「鼠标宏」；命中宏桌面 Enum 优先。
            PrepareMacroDesktopForCdpBind(candidate);
            WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(10));
            if (IsCancelled()) return nullptr;
            candidate = TopLevelWindow(candidate);
            if (candidate && IsWindow(candidate)) {
                if (HWND found = findOnMacroDesktop(refindQuery, true)) return found;
                return candidate;
            }
            return nullptr;
        }
        ForceHideLaunchedWindowQuiet(candidate);
        MinimizeForQuietDesktopMove(candidate);
        WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(15));
        if (IsCancelled()) return nullptr;
        if (!desktop_.MoveWindowToMacroDesktop(candidate)) return nullptr;
        WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(10));
        if (IsCancelled()) return nullptr;
        candidate = TopLevelWindow(candidate);
        ForceHideLaunchedWindowQuiet(candidate);
        PinMacroDesktopWindowBottom(candidate);
        ShowWindow(candidate, SW_SHOWMINNOACTIVE);
        WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(10));
        if (IsCancelled()) return nullptr;
        candidate = TopLevelWindow(candidate);
        if (HWND found = findOnMacroDesktop(refindQuery, true)) return found;
        WindowTargetQuery relaxed = refindQuery;
        relaxed.titleContains.clear();
        relaxed.className.clear();
        relaxed.childClassName.clear();
        relaxed.pickX = 0;
        relaxed.pickY = 0;
        if (HWND found = findOnMacroDesktop(relaxed, true)) return found;
        return IsWindowOnVirtualDesktopIndex(candidate, macroDesktopIndex) ? candidate : nullptr;
    };

    auto findMacroTarget = [&](const WindowTargetQuery& q) -> HWND {
        HWND found = findOnMacroDesktop(q, true);
        if (!found) found = findOnMacroDesktop(q, false);
        // CDP：Enum 偶发丢窗时按标题在任意桌面回退，再 Park 进宏桌面。
        if (!found && UsesCdpInput(config_)) {
            found = findAnywhere(q, true);
            if (!found) found = findAnywhere(q, false);
        }
        return found;
    };

    HWND hwnd = nullptr;
    if (background) {
        if (state_.targetHwnd) {
            HWND existingTop = TopLevelWindow(state_.targetHwnd);
            if (existingTop && IsWindow(existingTop)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(existingTop, &pid);
                if (state_.targetPid == 0 || pid == state_.targetPid) {
                    SaveTargetTopPlacementIfNeeded(existingTop);
                    HWND bindHwnd = nullptr;
                    if (WaitForBindHwnd(existingTop, config_, background, cancelFlag_, bindHwnd)) {
                        if (ApplyBoundTargetState(existingTop, bindHwnd, state_, config_, err, false, false)) {
                            err.clear();
                            state_.lastError.clear();
                            return true;
                        }
                    }
                }
            }
        }

        // ?
        if (ownsLaunchedProcess_ && launchedProcess_.dwProcessId != 0) {
            query.pid = launchedProcess_.dwProcessId;
            query.pickX = 0;
            query.pickY = 0;
        }

        hwnd = findAnywhere(query, false);
        if (!hwnd) hwnd = findAnywhere(query, true);
        if (!hwnd) {
            WindowTargetQuery relaxed = query;
            // ?
            if (config_.selectMethod != WindowSelectMethod::UseEditorWindowClass
                || ownsLaunchedProcess_) {
                relaxed.titleContains.clear();
            }
            relaxed.pickX = 0;
            relaxed.pickY = 0;
            relaxed.childClassName.clear();
            hwnd = findAnywhere(relaxed, true);
        }
        // ?
        if (!hwnd && !query.exePath.empty()
            && (ownsLaunchedProcess_
                || config_.selectMethod == WindowSelectMethod::NoSelect)) {
            WindowTargetQuery byExe{};
            byExe.exePath = query.exePath;
            byExe.pid = query.pid;
            hwnd = findAnywhere(byExe, true);
            if (!hwnd && !query.className.empty()) {
                WindowTargetQuery byClass = byExe;
                byClass.className = query.className;
                hwnd = findAnywhere(byClass, true);
            }
        }
        if (!hwnd && query.pid != 0) {
            // ?
            const DWORD waitMs = ownsLaunchedProcess_
                ? (!query.titleContains.empty() ? 3000u : 2000u)
                : 400u;
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(waitMs);
            WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u7b49\u5f85\u8fdb\u7a0b\u4e3b\u7a97\u53e3 pid=%lu \u2026",
                static_cast<unsigned long>(query.pid));
            while (!hwnd && std::chrono::steady_clock::now() < deadline) {
                if (IsCancelled()) {
                    err = L"\u5df2\u53d6\u6d88";
                    state_.lastError = err;
                    return false;
                }
                if (HWND procWnd = FindMainWindowForProcess(query.pid)) {
                    if (!IsLikelyImeOrToolWindow(procWnd)) {
                        hwnd = procWnd;
                        break;
                    }
                }
                // ?
                if (ownsLaunchedProcess_ && !query.exePath.empty()) {
                    WindowTargetQuery byExe{};
                    byExe.exePath = query.exePath;
                    byExe.titleContains = query.titleContains;
                    byExe.allowStoreNotepadHandoff = true;
                    if (!query.className.empty()
                        && !PreferredClassLooksLikeChildControl(query.className)) {
                        byExe.className = query.className;
                    }
                    FILETIME launchFt = launchTimeUtc_;
                    if (!hasLaunchTimeUtc_) GetSystemTimeAsFileTime(&launchFt);
                    hwnd = FindLaunchResultMainWindow(
                        byExe, preLaunchWindows_, launchFt, false, true);
                    if (hwnd) break;
                    byExe.className.clear();
                    hwnd = FindLaunchResultMainWindow(
                        byExe, preLaunchWindows_, launchFt, true, true);
                    if (hwnd) break;
                } else if (!query.exePath.empty()) {
                    WindowTargetQuery byExe{};
                    byExe.exePath = query.exePath;
                    if (!query.className.empty()
                        && !PreferredClassLooksLikeChildControl(query.className)) {
                        byExe.className = query.className;
                    }
                    hwnd = findAnywhere(byExe, true);
                    if (hwnd) break;
                    byExe.className.clear();
                    hwnd = findAnywhere(byExe, true);
                    if (hwnd) break;
                }
                WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(25));
            }
        }
        if (hwnd) {
            SaveTargetTopPlacementIfNeeded(hwnd);
            // ?
            // ?
        }
    } else {
        if (ownsLaunchedProcess_ && launchedProcess_.dwProcessId != 0) {
            query.pid = launchedProcess_.dwProcessId;
            query.pickX = 0;
            query.pickY = 0;
        }

        hwnd = findMacroTarget(query);

        // ?
        if (!hwnd && !ownsLaunchedProcess_) {
            if (HWND anywhere = findAnywhere(query, true)) {
                hwnd = moveCandidateToMacro(anywhere, query);
            } else if (HWND anywhereVisible = findAnywhere(query, false)) {
                hwnd = moveCandidateToMacro(anywhereVisible, query);
            }
        }

        // ?
        if (!hwnd
            && (config_.selectMethod == WindowSelectMethod::UseEditorWindowClass
                || config_.selectMethod == WindowSelectMethod::NoSelect
                || ownsLaunchedProcess_)) {
            WindowTargetQuery soft = query;
            // ?
            if (config_.selectMethod == WindowSelectMethod::NoSelect) {
                soft.titleContains.clear();
            }
            soft.pickX = 0;
            soft.pickY = 0;
            soft.childClassName.clear();
            hwnd = findMacroTarget(soft);
            if (!hwnd && !ownsLaunchedProcess_) {
                if (HWND anywhere = findAnywhere(soft, true)) {
                    hwnd = moveCandidateToMacro(anywhere, soft);
                }
            }
            if (!hwnd && ownsLaunchedProcess_ && !soft.className.empty()) {
                soft.className.clear();
                if (config_.selectMethod != WindowSelectMethod::UseEditorWindowClass) {
                    soft.titleContains.clear();
                }
                hwnd = findMacroTarget(soft);
            }
            if (!hwnd && ownsLaunchedProcess_ && !query.exePath.empty()) {
                WindowTargetQuery byExe{};
                byExe.exePath = query.exePath;
                byExe.titleContains = query.titleContains;
                byExe.className = query.className;
                byExe.allowStoreNotepadHandoff = true;
                FILETIME launchFt = launchTimeUtc_;
                if (!hasLaunchTimeUtc_) GetSystemTimeAsFileTime(&launchFt);
                if (HWND newborn = FindNewlyAppearedMainWindow(byExe, preLaunchWindows_, true)) {
                    hwnd = moveCandidateToMacro(newborn, byExe);
                } else if (HWND launchWnd = FindLaunchResultMainWindow(
                        byExe, preLaunchWindows_, launchFt, /*allowReuse=*/false, true)) {
                    hwnd = moveCandidateToMacro(launchWnd, byExe);
                } else if (HWND launchReuse = FindLaunchResultMainWindow(
                        byExe, preLaunchWindows_, launchFt, /*allowReuse=*/true, true)) {
                    // ?
                    hwnd = moveCandidateToMacro(launchReuse, byExe);
                } else if (HWND onMacro = findMacroTarget(byExe)) {
                    if (!HwndInExcludeList(onMacro, preLaunchWindows_)) hwnd = onMacro;
                }
            }
        }

        if (!hwnd && query.pid != 0) {
            // ?
            // ?
            const DWORD waitMs = ownsLaunchedProcess_
                ? (!query.titleContains.empty() ? 3000u : 2000u)
                : 400u;
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(waitMs);
            while (!hwnd && std::chrono::steady_clock::now() < deadline) {
                if (IsCancelled()) {
                    err = L"\u5df2\u53d6\u6d88";
                    state_.lastError = err;
                    return false;
                }
                // ?
                if (HWND procWnd = FindMainWindowForProcess(query.pid)) {
                    // ?
                    const bool titleOk = query.titleContains.empty()
                        || DoesTopWindowMatchConfig(procWnd, config_);
                    if (titleOk || config_.selectMethod != WindowSelectMethod::UseEditorWindowClass) {
                        ForceHideLaunchedWindowQuiet(procWnd);
                        hwnd = moveCandidateToMacro(procWnd, query);
                    }
                } else if (ownsLaunchedProcess_ && !query.exePath.empty()) {
                    WindowTargetQuery byExe{};
                    byExe.exePath = query.exePath;
                    byExe.titleContains = query.titleContains;
                    byExe.className = query.className;
                    byExe.allowStoreNotepadHandoff = true;
                    FILETIME launchFt = launchTimeUtc_;
                    if (!hasLaunchTimeUtc_) GetSystemTimeAsFileTime(&launchFt);
                    if (HWND newborn = FindNewlyAppearedMainWindow(
                            byExe, preLaunchWindows_, true)) {
                        ForceHideLaunchedWindowQuiet(newborn);
                        hwnd = moveCandidateToMacro(newborn, byExe);
                    } else if (HWND launchWnd = FindLaunchResultMainWindow(
                            byExe, preLaunchWindows_, launchFt, false, true)) {
                        ForceHideLaunchedWindowQuiet(launchWnd);
                        hwnd = moveCandidateToMacro(launchWnd, byExe);
                    } else if (HWND launchReuse = FindLaunchResultMainWindow(
                            byExe, preLaunchWindows_, launchFt, true, true)) {
                        ForceHideLaunchedWindowQuiet(launchReuse);
                        hwnd = moveCandidateToMacro(launchReuse, byExe);
                    } else if (HWND onMacro = findMacroTarget(byExe)) {
                        if (!HwndInExcludeList(onMacro, preLaunchWindows_)) hwnd = onMacro;
                    }
                }
                if (!hwnd) {
                    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
                        DWORD want = static_cast<DWORD>(lp), pid = 0;
                        GetWindowThreadProcessId(w, &pid);
                        if (pid != want || GetWindow(w, GW_OWNER)) return TRUE;
                        if (IsWindowVisible(w) || IsIconic(w)) ForceHideLaunchedWindowQuiet(w);
                        return TRUE;
                    }, static_cast<LPARAM>(query.pid));
                    WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(15));
                }
            }
        }

        if (hwnd) {
            HWND top = TopLevelWindow(hwnd);
            if (!IsWindowOnVirtualDesktopIndex(top, macroDesktopIndex)) {
                MinimizeForQuietDesktopMove(top);
                WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(20));
                if (!desktop_.MoveWindowToMacroDesktop(top)) {
                    err = desktop_.LastError().empty()
                        ? L"\u65e0\u6cd5\u5c06\u7a97\u53e3\u79fb\u5165\u300c\u9f20\u6807\u5b8f\u300d\u865a\u62df\u684c\u9762" : desktop_.LastError();
                    state_.lastError = err;
                    state_.health = WindowModeHealth::DesktopNotReady;
                    return false;
                }
            }
        }
    }

    if (IsCancelled()) {
        err = L"\u5df2\u53d6\u6d88";
        state_.lastError = err;
        return false;
    }

    if (!hwnd) {
        state_.health = WindowModeHealth::TargetNotFound;
        const bool canLaunch = ShouldAutoLaunchTarget(config_);
        if (background) {
            err = canLaunch
                ? L"\u672a\u627e\u5230\u76ee\u6807\u7a97\u53e3\uff0c\u4e14\u81ea\u52a8\u542f\u52a8\u5931\u8d25\n"
                  L"\u00b7 \u5546\u5e97\u8def\u5f84(WindowsApps)\u53ef\u7528 AppsFolder \u6216\u7ecf\u5178 notepad.exe\n"
                  L"\u00b7 \u8bf7\u786e\u8ba4\u76ee\u6807\u7a0b\u5e8f\u53ef\u542f\u52a8\u4e14\u7a97\u53e3\u6807\u9898/\u7c7b\u540d\u5339\u914d"
                : (config_.selectMethod == WindowSelectMethod::NoSelect
                    ? L"\u672a\u627e\u5230\u76ee\u6807\u7a97\u53e3\uff08\u4e0d\u9009\u62e9\u7a97\u53e3\uff1a\u8bf7\u5148\u6253\u5f00\u76ee\u6807\u7a0b\u5e8f\uff09"
                    : L"\u672a\u627e\u5230\u5339\u914d\u7684\u76ee\u6807\u7a97\u53e3\uff08\u8bf7\u786e\u8ba4 exe / \u7c7b\u540d / \u6807\u9898 \u662f\u5426\u4e0e\u7f16\u8f91\u65f6\u4e00\u81f4\uff09");
        } else if (canLaunch) {
            err = L"\u672a\u627e\u5230\u76ee\u6807\u7a97\u53e3\uff0c\u4e14\u672a\u80fd\u5b8c\u6210\u81ea\u52a8\u542f\u52a8\u6216\u79fb\u5165\u300c\u9f20\u6807\u5b8f\u300d\n"
                L"\u00b7 \u8bb0\u4e8b\u672c\u53ef\u6539\u7528 C:\\Windows\\System32\\notepad.exe\n"
                L"\u00b7 \u5546\u5e97/UWP(WindowsApps) \u8def\u5f84\u542f\u52a8\u540e\u82e5\u65e0\u7a97\uff0c\u8bf7\u68c0\u67e5\u6743\u9650\u4e0e\u684c\u9762";
        } else {
            err = L"\u672a\u627e\u5230\u76ee\u6807\u7a97\u53e3\n"
                L"\u00b7 \u8bf7\u5148\u624b\u52a8\u6253\u5f00\u76ee\u6807\u7a0b\u5e8f\n"
                L"\u00b7 \u6216\u5f00\u542f\u81ea\u52a8\u542f\u52a8\uff0c\u5e76\u786e\u8ba4 exe / \u7c7b\u540d / \u6807\u9898\u914d\u7f6e\u6b63\u786e";
        }
        state_.lastError = err;
        state_.targetHwnd = nullptr;
        state_.targetPid = 0;
        return false;
    }

    HWND top = TopLevelWindow(hwnd);
    HWND bindHwnd = nullptr;
    if (!WaitForBindHwnd(top, config_, background, cancelFlag_, bindHwnd)) {
        state_.health = WindowModeHealth::TargetNotFound;
        err = L"\u7b49\u5f85\u7ed1\u5b9a\u5b50\u7a97\u53e3/\u8f93\u5165\u76ee\u6807\u5931\u8d25";
        state_.lastError = err;
        state_.targetHwnd = nullptr;
        state_.targetPid = 0;
        return false;
    }

    if (!background && config_.selectMethod != WindowSelectMethod::NoSelect
        && config_.selectMethod != WindowSelectMethod::UseEditorWindowClass
        && !config_.childWindowClassName.empty()
        && !config_.useTopLevelWindow
        && bindHwnd == top) {
        state_.health = WindowModeHealth::TargetNotFound;
        err = L"\u672a\u80fd\u7ed1\u5b9a\u5230\u6307\u5b9a\u5b50\u7a97\u53e3\u7c7b";
        state_.lastError = err;
        state_.targetHwnd = nullptr;
        state_.targetPid = 0;
        return false;
    }

    if (!ApplyBoundTargetState(top, bindHwnd, state_, config_, err)) {
        state_.lastError = err;
        return false;
    }

    if (!background) {
        if (UsesCdpInput(config_)) {
            PrepareMacroDesktopForCdpBind(top);
        } else {
            PinMacroDesktopWindowBottom(top);
            if (ShouldMinimizeTargetAfterBind(config_) && !IsIconic(top)) {
                ShowWindow(top, SW_SHOWMINNOACTIVE);
            }
        }
    } else if (ownsLaunchedProcess_) {
        SquashBackgroundLaunchedWindow(top, cancelFlag_);
        DWORD pid = 0;
        GetWindowThreadProcessId(top, &pid);
        HideTransientGdiPlusWindows(pid);
    } else {
        EnsureTargetBelowUserWindows(top);
    }

    err.clear();
    state_.lastError.clear();
    DebugLog(L"[WindowMode] BindTargetWindow OK");
    return true;
}

bool WindowModeSession::RefreshInputBinding(std::wstring& err) {
    if (!config_.enabled || !state_.targetHwnd) {
        err = L"\u5c1a\u672a\u7ed1\u5b9a\u76ee\u6807\u7a97\u53e3";
        return false;
    }

    HWND top = TopLevelWindow(state_.targetHwnd);
    if (!top || !IsWindow(top)) {
        err = L"\u76ee\u6807\u7a97\u53e3\u5df2\u5931\u6548";
        return false;
    }

    const bool background = config_.executionKind == WindowModeExecutionKind::BackgroundWindow;
    HWND bindHwnd = nullptr;
    if (!ResolveInputBindHwnd(top, config_, background, bindHwnd)) {
        err = L"\u5237\u65b0\u8f93\u5165\u7ed1\u5b9a\u5931\u8d25";
        return false;
    }

    return ApplyBoundTargetState(top, bindHwnd, state_, config_, err, false);
}

bool WindowModeSession::Start(const WindowModeScriptConfig& config, std::wstring& err) {
    Stop();
    config_ = config;
    if (!config_.enabled) {
        state_.health = WindowModeHealth::Unknown;
        return true;
    }
    if (!EnsureDesktop(err)) return false;
    // ?
    // ?
    if (ShouldAutoLaunchTarget(config_)) {
        state_.health = WindowModeHealth::Unknown;
        return true;
    }
    return RefreshTarget(err);
}

void WindowModeSession::ReleaseLaunchedProcess() {
    if (!ownsLaunchedProcess_) return;

    if (launchedProcess_.hThread) {
        CloseHandle(launchedProcess_.hThread);
        launchedProcess_.hThread = nullptr;
    }
    if (launchedProcess_.hProcess) {
        CloseHandle(launchedProcess_.hProcess);
        launchedProcess_.hProcess = nullptr;
    }
    launchedProcess_.dwProcessId = 0;
    launchedProcess_.dwThreadId = 0;
    ownsLaunchedProcess_ = false;
    preLaunchWindows_.clear();
    hasLaunchTimeUtc_ = false;
    launchTimeUtc_ = {};
}

void WindowModeSession::SaveTargetTopPlacementIfNeeded(HWND top) {
    top = TopLevelWindow(top);
    if (!top || !IsWindow(top) || hasSavedTargetTopPlacement_) return;

    // ?
    if (!IsIconic(top)) return;

    savedTargetTopWp_.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(top, &savedTargetTopWp_)) return;

    savedTargetTopHwnd_ = top;
    hasSavedTargetTopPlacement_ = true;
}

void WindowModeSession::RestoreSavedTargetTopPlacement() {
    if (!hasSavedTargetTopPlacement_) return;
    if (config_.executionKind != WindowModeExecutionKind::BackgroundWindow) {
        hasSavedTargetTopPlacement_ = false;
        savedTargetTopHwnd_ = nullptr;
        return;
    }

    if (savedTargetTopHwnd_ && IsWindow(savedTargetTopHwnd_)) {
        // ?
        RestoreBoundTargetTopWindow(savedTargetTopHwnd_, savedTargetTopWp_);
        WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] \u4f1a\u8bdd\u7ed3\u675f: \u5df2\u91ca\u653e\u542f\u52a8\u8fdb\u7a0b\u4e0e\u76ee\u6807\u7ed1\u5b9a");
    }

    hasSavedTargetTopPlacement_ = false;
    savedTargetTopHwnd_ = nullptr;
}

void WindowModeSession::Stop() {
    RestoreSavedTargetTopPlacement();
    ReleaseLaunchedProcess();
    desktop_.Close();
    state_ = WindowModeSessionState{};
    cancelFlag_ = nullptr;
    launchSearchDir_.clear();
}

bool WindowModeSession::EnsureMacroDesktopReady(std::wstring& err) {
    err.clear();
    if (config_.executionKind == WindowModeExecutionKind::BackgroundWindow) {
        return true;
    }
    if (!desktop_.IsValid()) {
        if (!desktop_.OpenOrCreate()) {
            err = desktop_.LastError().empty()
                ? L"无法重建「鼠标宏」虚拟桌面" : desktop_.LastError();
            state_.macroDesktopIndex = -1;
            return false;
        }
    }
    state_.macroDesktopId = desktop_.DesktopId();
    state_.macroDesktopIndex = desktop_.DesktopIndex();

    HWND top = TopLevelWindow(state_.targetHwnd);
    if (!top || !IsWindow(top)) return true;

    if (UsesCdpInput(config_)) {
        // CDP 运行期严禁 Move/Pin/SoftRestore：VDA 误报或 Pin 残留会导致
        // 每次找图「0→1 Move + 出帧」闪屏，窗也落不稳在宏桌面。
        return true;
    }

    if (!IsWindowOnVirtualDesktopIndex(top, state_.macroDesktopIndex)) {
        EnsureTargetOnMacroDesktop(top, ShouldMinimizeTargetAfterBind(config_));
    }
    return true;
}

void WindowModeSession::ClearTargetBinding() {
    RestoreSavedTargetTopPlacement();
    ReleaseLaunchedProcess();
    state_.targetHwnd = nullptr;
    state_.targetPid = 0;
    state_.health = WindowModeHealth::Unknown;
    state_.lastError.clear();
}

void WindowModeSession::SetCancelFlag(const std::atomic_bool* flag) {
    cancelFlag_ = flag;
    desktop_.SetCancelFlag(flag);
}

bool WindowModeSession::ValidateTargetExe(std::wstring& err) const {
    const std::wstring exePath = ResolveExePath(config_.targetExePath);
    if (exePath.empty()) {
        err = L"\u672a\u914d\u7f6e\u76ee\u6807\u7a0b\u5e8f\u8def\u5f84";
        return false;
    }

    if (IsUrlPath(exePath) || IsDocumentPath(exePath)) {
        err.clear();
        return true;
    }

    DWORD attrs = GetFileAttributesW(exePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        err = L"\u76ee\u6807\u7a0b\u5e8f\u4e0d\u5b58\u5728\u6216\u8def\u5f84\u65e0\u6548: " + exePath;
        return false;
    }
    err.clear();
    return true;
}

bool WindowModeSession::RefreshTarget(std::wstring& err) {
    if (!config_.enabled) {
        err = L"\u7a97\u53e3\u6a21\u5f0f\u672a\u542f\u7528";
        return false;
    }
    if (!EnsureDesktop(err)) return false;
    return BindTargetWindow(err);
}

bool WindowModeSession::LaunchTargetOnDesktop(std::wstring& err) {
    if (!config_.enabled) {
        err = L"\u7a97\u53e3\u6a21\u5f0f\u672a\u542f\u7528";
        return false;
    }
    if (!EnsureDesktop(err)) return false;

    const std::wstring targetPath = ResolveLaunchPath(config_, launchSearchDir_);
    if (targetPath.empty()) {
        err = L"\u672a\u89e3\u6790\u5230\u542f\u52a8\u8def\u5f84";
        state_.lastError = err;
        return false;
    }
    if (targetPath != ResolveExePath(config_.targetExePath)) {
        WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u542f\u52a8\u8def\u5f84\u5df2\u89e3\u6790\u4e3a: %s", targetPath.c_str());
    }

    PROCESS_INFORMATION pi{};
    bool launched = false;
    const std::wstring documentFile = ResolveDocumentFileToOpen(config_, launchSearchDir_);
    // ?
    const std::wstring launchArgs = !documentFile.empty()
        ? documentFile
        : EffectiveLaunchArgs(config_);

    if (!documentFile.empty()) {
        WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u5b8f\u684c\u9762\u6587\u6863\u542f\u52a8: %s + %s",
            targetPath.c_str(), documentFile.c_str());
    }

    std::wstring expectTitle;
    if (!documentFile.empty()) {
        const auto slash = documentFile.find_last_of(L"\\/");
        expectTitle = (slash == std::wstring::npos)
            ? documentFile : documentFile.substr(slash + 1);
    } else if (config_.selectMethod == WindowSelectMethod::UseEditorWindowClass) {
        expectTitle = StripDocumentStemFromTitle(
            !config_.windowName.empty() ? config_.windowName : config_.targetWindowTitle);
    }

    const bool editorNeedsIdentity =
        config_.selectMethod == WindowSelectMethod::UseEditorWindowClass
        && !expectTitle.empty();

    {
        WindowTargetQuery pre{};
        pre.exePath = targetPath;
        pre.allowStoreNotepadHandoff = true;
        preLaunchWindows_.clear();
        CollectMatchingMainWindows(pre, true, preLaunchWindows_);
        if (preLaunchWindows_.empty()) {
            CollectMatchingMainWindows(pre, false, preLaunchWindows_);
        }
        GetSystemTimeAsFileTime(&launchTimeUtc_);
        hasLaunchTimeUtc_ = true;
    }

    // 「指定窗口类」有标题但无本地文档：禁止开空白记事本；先绑已有窗。
    // CDP/浏览器：标题是网页名，不是本地文件 —— 禁止报「文档参数缺失」。
    if (editorNeedsIdentity && documentFile.empty()) {
        WindowModeLog(L"[窗口模式] 指定窗口类：无可用文档路径，尝试绑定已有标题窗（不开空白）");
        if (BindTargetWindow(err)) return true;
        const bool browserTarget =
            UsesCdpInput(config_)
            || LooksLikeChromiumBrowserClass(config_.windowClassName)
            || LooksLikeChromiumBrowserClass(config_.childWindowClassName);
        if (browserTarget) {
            WindowTargetQuery anywhereQ{};
            anywhereQ.exePath = targetPath;
            anywhereQ.allowStoreNotepadHandoff = true;
            anywhereQ.titleContains =
                !config_.windowName.empty() ? config_.windowName : config_.targetWindowTitle;
            if (anywhereQ.titleContains.empty()) anywhereQ.titleContains = expectTitle;
            if (!config_.windowClassName.empty()) {
                anywhereQ.className = config_.windowClassName;
            }
            HWND hit = FindMainWindowDefault(anywhereQ, true);
            if (!hit) hit = FindMainWindowDefault(anywhereQ, false);
            if (hit) {
                PrepareMacroDesktopForCdpBind(hit);
                if (BindTargetWindow(err)) return true;
                HWND bindHwnd = nullptr;
                if (WaitForBindHwnd(hit, config_, false, cancelFlag_, bindHwnd)
                    && ApplyBoundTargetState(hit, bindHwnd, state_, config_, err, false, false)) {
                    err.clear();
                    state_.lastError.clear();
                    WindowModeLog(L"[窗口模式] CDP：已按浏览器窗绑定（跳过文档启动）");
                    return true;
                }
            }
            // 有 URL/启动参数时继续走下方启动；否则提示先打开网页（勿报文档缺失）。
            const std::wstring args = EffectiveLaunchArgs(config_);
            const bool looksUrl = args.size() >= 7
                && (_wcsnicmp(args.c_str(), L"http://", 7) == 0
                    || (args.size() >= 8 && _wcsnicmp(args.c_str(), L"https://", 8) == 0));
            if (!looksUrl && args.empty()) {
                err = L"未找到标题含「" + expectTitle + L"」的浏览器窗口。"
                      L"请先打开该网页后再运行（浏览器窗口模式不需要本地文档参数）。";
                state_.lastError = err;
                return false;
            }
            WindowModeLog(L"[窗口模式] CDP/浏览器：无已有窗，使用启动参数打开（非本地文档）");
        } else {
            err = L"指定窗口类需要标题对应文档，但未解析到文件"
                  L"（请设置完整启动参数，或把文件放在脚本目录）";
            state_.lastError = err;
            return false;
        }
    }

    auto squashMoved = [&](HWND wnd) {
        if (!wnd || !IsWindow(wnd)) return;
        ForceHideLaunchedWindowQuiet(wnd);
        MinimizeForQuietDesktopMove(wnd);
        desktop_.MoveWindowToMacroDesktop(wnd);
        PinMacroDesktopWindowBottom(wnd);
        ShowWindow(wnd, SW_SHOWMINNOACTIVE);
    };

    if (ShouldOpenViaShell(targetPath) && documentFile.empty()) {
        launched = LaunchDocumentOnMacroDesktop(targetPath, launchArgs, desktop_, pi, err, cancelFlag_);
        if (!launched) {
            WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] \u6587\u6863\u684c\u9762\u542f\u52a8\u5931\u8d25\uff0c\u56de\u9000 ShellExecute");
            launched = LaunchViaShell(targetPath, launchArgs, pi, err);
            if (launched) {
                SquashDocumentWindowOnMacroDesktop(targetPath, desktop_, cancelFlag_);
            }
        }
    } else if (IsStorePackagedPath(targetPath) && launchArgs.empty() && !editorNeedsIdentity) {
        // ?
        launched = LaunchStoreAppFromPath(targetPath, pi, err);
        if (launched && pi.dwProcessId != 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            while (std::chrono::steady_clock::now() < deadline) {
                if (IsCancelled()) break;
                EnumWindows([](HWND w, LPARAM lp) -> BOOL {
                    DWORD want = static_cast<DWORD>(lp), pid = 0;
                    GetWindowThreadProcessId(w, &pid);
                    if (pid != want || GetWindow(w, GW_OWNER)) return TRUE;
                    if (IsWindowVisible(w) || IsIconic(w)) ForceHideLaunchedWindowQuiet(w);
                    return TRUE;
                }, static_cast<LPARAM>(pi.dwProcessId));
                if (HWND wnd = FindMainWindowForProcess(pi.dwProcessId)) {
                    squashMoved(wnd);
                    break;
                }
                WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(10));
            }
        } else if (launched) {
            WindowTargetQuery waitQ{};
            waitQ.exePath = targetPath;
            waitQ.allowStoreNotepadHandoff = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            while (std::chrono::steady_clock::now() < deadline) {
                if (IsCancelled()) break;
                HWND wnd = FindLaunchResultMainWindow(
                    waitQ, preLaunchWindows_, launchTimeUtc_, false, true);
                if (!wnd) {
                    wnd = FindLaunchResultMainWindow(
                        waitQ, preLaunchWindows_, launchTimeUtc_, false, false);
                }
                if (!wnd) {
                    wnd = FindLaunchResultMainWindow(
                        waitQ, preLaunchWindows_, launchTimeUtc_, true, true);
                }
                if (wnd) {
                    squashMoved(wnd);
                    break;
                }
                WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(10));
            }
        }
    } else {
        // ?
        launched = desktop_.LaunchProcess(targetPath, launchArgs, pi, expectTitle);
        if (!launched && IsStorePackagedPath(targetPath)) {
            WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] CreateProcess \u5931\u8d25\uff0c\u56de\u9000 Shell\uff08\u5e26\u6587\u6863\uff0c\u4e0d\u5f00\u7a7a\u767d AppsFolder\uff09");
            if (!documentFile.empty()) {
                launched = LaunchViaShell(documentFile, L"", pi, err, SW_SHOWMINNOACTIVE);
                if (!launched) {
                    launched = LaunchViaShell(targetPath, documentFile, pi, err, SW_SHOWMINNOACTIVE);
                }
                if (launched) {
                    SquashDocumentWindowOnMacroDesktop(
                        documentFile.empty() ? targetPath : documentFile, desktop_, cancelFlag_);
                }
            } else if (!editorNeedsIdentity) {
                launched = LaunchStoreAppFromPath(targetPath, pi, err);
                if (!launched) launched = LaunchViaShell(targetPath, launchArgs, pi, err);
                if (launched && pi.dwProcessId != 0) {
                    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(2000);
                    while (std::chrono::steady_clock::now() < deadline) {
                        if (IsCancelled()) break;
                        if (HWND wnd = FindMainWindowForProcess(pi.dwProcessId)) {
                            squashMoved(wnd);
                            break;
                        }
                        WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(10));
                    }
                }
            }
        }
        if (!launched && err.empty()) err = desktop_.LastError();
    }

    if (!launched) {
        state_.lastError = err;
        return false;
    }

    if (ownsLaunchedProcess_) {
        ReleaseLaunchedProcess();
    }
    if (pi.hProcess) {
        launchedProcess_ = pi;
        ownsLaunchedProcess_ = true;
    }

    return BindTargetWindow(err);
}

bool WindowModeSession::LaunchTargetOnDefaultDesktop(std::wstring& err) {
    if (!config_.enabled) {
        err = L"\u7a97\u53e3\u6a21\u5f0f\u672a\u542f\u7528";
        return false;
    }
    if (!EnsureDesktop(err)) return false;

    const std::wstring targetPath = ResolveLaunchPath(config_, launchSearchDir_);
    if (targetPath.empty()) {
        err = L"\u672a\u89e3\u6790\u5230\u542f\u52a8\u8def\u5f84";
        state_.lastError = err;
        return false;
    }
    if (targetPath != ResolveExePath(config_.targetExePath)) {
        WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u542f\u52a8\u8def\u5f84\u5df2\u89e3\u6790\u4e3a: %s", targetPath.c_str());
    }

    if (ownsLaunchedProcess_) {
        ReleaseLaunchedProcess();
    }

    PROCESS_INFORMATION pi{};
    bool launched = false;
    const std::wstring documentFile = ResolveDocumentFileToOpen(config_, launchSearchDir_);
    const std::wstring launchArgs = !documentFile.empty()
        ? documentFile
        : EffectiveLaunchArgs(config_);

    std::wstring expectTitle;
    if (!documentFile.empty()) {
        const auto slash = documentFile.find_last_of(L"\\/");
        expectTitle = (slash == std::wstring::npos) ? documentFile : documentFile.substr(slash + 1);
    } else if (config_.selectMethod == WindowSelectMethod::UseEditorWindowClass) {
        expectTitle = StripDocumentStemFromTitle(
            !config_.windowName.empty() ? config_.windowName : config_.targetWindowTitle);
    }
    const bool editorNeedsIdentity =
        config_.selectMethod == WindowSelectMethod::UseEditorWindowClass
        && !expectTitle.empty();

    WindowTargetQuery preLaunchQuery{};
    preLaunchQuery.exePath = targetPath;
    preLaunchQuery.allowStoreNotepadHandoff = true;
    preLaunchWindows_.clear();
    CollectMatchingMainWindows(preLaunchQuery, true, preLaunchWindows_);
    if (preLaunchWindows_.empty()) {
        CollectMatchingMainWindows(preLaunchQuery, false, preLaunchWindows_);
    }
    GetSystemTimeAsFileTime(&launchTimeUtc_);
    hasLaunchTimeUtc_ = true;

    if (editorNeedsIdentity && documentFile.empty()) {
        WindowModeLog(L"[窗口模式] 后台指定窗口类：无文档路径，仅绑定已有标题窗");
        if (BindTargetWindow(err)) return true;
        const bool browserTarget =
            UsesCdpInput(config_)
            || LooksLikeChromiumBrowserClass(config_.windowClassName)
            || LooksLikeChromiumBrowserClass(config_.childWindowClassName);
        if (browserTarget) {
            const std::wstring args = EffectiveLaunchArgs(config_);
            const bool looksUrl = args.size() >= 7
                && (_wcsnicmp(args.c_str(), L"http://", 7) == 0
                    || (args.size() >= 8 && _wcsnicmp(args.c_str(), L"https://", 8) == 0));
            if (!looksUrl && args.empty()) {
                err = L"未找到标题含「" + expectTitle + L"」的浏览器窗口。"
                      L"请先打开该网页后再运行（浏览器窗口模式不需要本地文档参数）。";
                state_.lastError = err;
                return false;
            }
            WindowModeLog(L"[窗口模式] 后台 CDP/浏览器：使用启动参数打开（非本地文档）");
        } else {
            err = L"指定窗口类需要标题对应文档，但未解析到文件"
                  L"（请设置完整启动参数，或把文件放在脚本目录）";
            state_.lastError = err;
            return false;
        }
    }

    if (!documentFile.empty()) {
        WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u540e\u53f0\u6587\u6863\u542f\u52a8: %s + %s",
            targetPath.c_str(), documentFile.c_str());
        std::wstring docArg = documentFile;
        if (docArg.size() >= 2 && docArg.front() == L'"' && docArg.back() == L'"') {
            docArg = docArg.substr(1, docArg.size() - 2);
        }
        launched = LaunchExeOnDefaultDesktop(targetPath, docArg, pi, err);
        if (!launched) {
            WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] \u540e\u53f0 CreateProcess \u5931\u8d25\uff0c\u56de\u9000 ShellExecute \u6587\u6863");
            launched = LaunchViaShell(documentFile, L"", pi, err, SW_SHOWMINNOACTIVE);
        }
    } else if (ShouldOpenViaShell(targetPath)) {
        launched = LaunchViaShell(targetPath, launchArgs, pi, err, SW_SHOWMINNOACTIVE);
    } else {
        WindowModeLogf(L"[\u7a97\u53e3\u6a21\u5f0f] \u540e\u53f0\u542f\u52a8: %s", targetPath.c_str());
        launched = LaunchExeOnDefaultDesktop(targetPath, launchArgs, pi, err);
    }
    if (!launched) {
        state_.lastError = err;
        return false;
    }

    std::wstring titleHint;
    if (!documentFile.empty()) {
        const auto slash = documentFile.find_last_of(L"\\/");
        titleHint = (slash == std::wstring::npos) ? documentFile : documentFile.substr(slash + 1);
    } else if (!config_.windowName.empty()) {
        titleHint = StripDocumentStemFromTitle(config_.windowName);
    }

    WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] \u7b49\u5f85\u542f\u52a8\u7a97\u53e3\u51fa\u73b0\uff08\u8f6e\u8be2\uff0c\u4e0d\u7528 WinEvent\uff09");
    HWND found = WaitAndSquashLaunchedWindow(
        targetPath.empty() ? config_.targetExePath : targetPath,
        preLaunchWindows_,
        launchTimeUtc_,
        config_.windowClassName,
        titleHint,
        pi,
        cancelFlag_);

    if (pi.hProcess) {
        launchedProcess_ = pi;
        ownsLaunchedProcess_ = true;
    } else if (pi.dwProcessId != 0) {
        launchedProcess_ = pi;
        ownsLaunchedProcess_ = true;
    } else if (found && IsWindow(found)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(found, &pid);
        if (pid != 0) {
            pi.dwProcessId = pid;
            launchedProcess_ = pi;
            ownsLaunchedProcess_ = true;
        }
    }

    WindowModeLog(L"[\u7a97\u53e3\u6a21\u5f0f] \u542f\u52a8\u5b8c\u6210\uff0c\u5f00\u59cb\u7ed1\u5b9a");
    return BindTargetWindow(err);
}

bool WindowModeSession::CaptureTargetToFile(const std::wstring& path, std::wstring& err) {
    if (!config_.enabled) {
        err = L"\u7a97\u53e3\u6a21\u5f0f\u672a\u542f\u7528";
        return false;
    }
    if (!EnsureDesktop(err)) return false;
    if (!state_.targetHwnd || !IsWindow(state_.targetHwnd)) {
        if (!BindTargetWindow(err)) return false;
    }

    ScopedVisionCapturePrep prep(state_.targetHwnd, false);
    if (!prep.Ready()) {
        err = L"\u622a\u56fe\u51c6\u5907\u5931\u8d25";
        state_.lastError = err;
        return false;
    }

    WindowCaptureResult capture = CaptureWindowClient(state_.targetHwnd);
    if (!capture.bitmap) {
        state_.health = WindowModeHealth::CaptureFailed;
        err = HealthToUserHint(state_.health);
        state_.lastError = err;
        return false;
    }

    if (IsCaptureLikelyBlank(capture.bitmap)) {
        DeleteObject(capture.bitmap);
        state_.health = WindowModeHealth::TargetNoRender;
        err = HealthToUserHint(state_.health);
        state_.lastError = err;
        return false;
    }

    const bool saved = SaveBitmapToFile(capture.bitmap, path);
    DeleteObject(capture.bitmap);

    if (!saved) {
        state_.health = WindowModeHealth::CaptureFailed;
        err = L"\u4fdd\u5b58\u622a\u56fe\u5931\u8d25";
        state_.lastError = err;
        return false;
    }

    state_.health = WindowModeHealth::Ok;
    state_.lastError.clear();
    err.clear();
    return true;
}

}  // namespace windowmode
