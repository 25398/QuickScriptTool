#include "macro_virtual_desktop.h"

#include "virtual_desktop_accessor.h"
#include "window_mode_log.h"
#include "window_mode_requirements.h"
#include "window_target.h"

#include <chrono>
#include <thread>
#include <vector>

namespace windowmode {

namespace {

void DebugLog(const wchar_t* msg) {
    WindowModeLog(msg);
}

std::wstring FormatWin32Error(DWORD err) {
    if (err == 0) err = GetLastError();
    wchar_t* buf = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring text = len && buf ? std::wstring(buf, len) : L"";
    if (buf) LocalFree(buf);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

std::wstring FormatLaunchArgs(const std::wstring& args) {
    if (args.empty()) return L"";
    std::wstring a = args;
    while (!a.empty() && (a.front() == L' ' || a.front() == L'\t')) a.erase(a.begin());
    while (!a.empty() && (a.back() == L' ' || a.back() == L'\t')) a.pop_back();
    // JSON / 编辑器可能已带一层引号；再包一层会变成 \"C:\...\文件.txt\" → 记事本「文件名无效」。
    if (a.size() >= 2 && a.front() == L'"' && a.back() == L'"') {
        a = a.substr(1, a.size() - 2);
    }
    const bool needQuote = a.find_first_of(L" \t") != std::wstring::npos
        || a.find(L'"') != std::wstring::npos;
    if (!needQuote) return a;

    std::wstring escaped;
    escaped.reserve(a.size() + 8);
    for (wchar_t ch : a) {
        if (ch == L'"') escaped += L"\\\"";
        else escaped.push_back(ch);
    }
    return L"\"" + escaped + L"\"";
}

HWND TopLevelWindow(HWND hwnd) {
    if (!hwnd) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

bool GuidIsEmpty(const GUID& id) {
    GUID empty{};
    return IsEqualGUID(id, empty);
}

struct MacroDesktopCache {
    int index = -1;
    GUID id{};
};

MacroDesktopCache g_macroDesktopCache;

void SaveMacroDesktopCache(int index, const GUID& id) {
    g_macroDesktopCache.index = index;
    g_macroDesktopCache.id = id;
}

bool LoadMacroDesktopId(int index, GUID& outId) {
    auto& vda = VirtualDesktopAccessor::Instance();
    if (vda.GetDesktopIdByNumber(index, outId)) return true;
    if (!GuidIsEmpty(g_macroDesktopCache.id) && g_macroDesktopCache.index == index) {
        outId = g_macroDesktopCache.id;
        return true;
    }
    outId = GUID{};
    return false;
}

bool VerifyMacroDesktopIndex(int index) {
    if (index < 0) return false;
    auto& vda = VirtualDesktopAccessor::Instance();
    if (index >= vda.GetDesktopCount()) return false;
    return vda.DesktopNameMatches(index, kMacroDesktopDisplayName);
}

void SquashProcessTopWindowsQuiet(DWORD pid) {
    if (pid == 0) return;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        const DWORD want = static_cast<DWORD>(lp);
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid != want) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        if (!IsWindowVisible(hwnd) && !IsIconic(hwnd)) return TRUE;
        // Beat CreateProcess / Store host activation races: hide ASAP on the user's desktop.
        ForceHideLaunchedWindowQuiet(hwnd);
        return TRUE;
    }, static_cast<LPARAM>(pid));
}

bool TitleContainsMatch(HWND hwnd, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    if (title[0] == L'\0') return false;  // 标题尚未就绪，继续等
    std::wstring hay = title;
    std::wstring n = needle;
    for (auto& ch : hay) ch = static_cast<wchar_t>(towlower(ch));
    for (auto& ch : n) ch = static_cast<wchar_t>(towlower(ch));
    return hay.find(n) != std::wstring::npos;
}

HWND WaitForProcessMainWindow(DWORD pid, DWORD timeoutMs, const std::atomic_bool* cancelFlag,
    const std::wstring* exePathFallback = nullptr,
    const std::vector<HWND>* existingBefore = nullptr,
    const FILETIME* launchTimeUtc = nullptr,
    const std::wstring* titleContains = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    static const std::vector<HWND> kEmpty;
    const std::vector<HWND>& excl = existingBefore ? *existingBefore : kEmpty;
    FILETIME launchFt{};
    if (launchTimeUtc) {
        launchFt = *launchTimeUtc;
    } else {
        GetSystemTimeAsFileTime(&launchFt);
    }
    const std::wstring titleNeedle = titleContains ? *titleContains : L"";

    for (;;) {
        if (WindowModeCancelled(cancelFlag)) return nullptr;
        SquashProcessTopWindowsQuiet(pid);
        if (HWND hwnd = FindMainWindowForProcess(pid)) {
            if (TitleContainsMatch(hwnd, titleNeedle)) {
                ForceHideLaunchedWindowQuiet(hwnd);
                return hwnd;
            }
            ForceHideLaunchedWindowQuiet(hwnd);
        }
        if (exePathFallback && !exePathFallback->empty() && pid != 0) {
            WindowTargetQuery byPidPath{};
            byPidPath.pid = pid;
            byPidPath.exePath = *exePathFallback;
            byPidPath.titleContains = titleNeedle;
            if (HWND hwnd = FindMainWindowDefault(byPidPath, true)) {
                if (TitleContainsMatch(hwnd, titleNeedle)) {
                    ForceHideLaunchedWindowQuiet(hwnd);
                    return hwnd;
                }
            }
        }
        if (exePathFallback && !exePathFallback->empty()) {
            WindowTargetQuery byExe{};
            byExe.exePath = *exePathFallback;
            byExe.titleContains = titleNeedle;
            byExe.allowStoreNotepadHandoff = true;
            if (HWND hwnd = FindLaunchResultMainWindow(byExe, excl, launchFt, false, true)) {
                ForceHideLaunchedWindowQuiet(hwnd);
                return hwnd;
            }
            if (HWND hwnd = FindLaunchResultMainWindow(byExe, excl, launchFt, false, false)) {
                ForceHideLaunchedWindowQuiet(hwnd);
                return hwnd;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        WindowModeSleepInterruptible(cancelFlag, std::chrono::milliseconds(15));
    }

    if (exePathFallback && !exePathFallback->empty()) {
        WindowTargetQuery byExe{};
        byExe.exePath = *exePathFallback;
        byExe.titleContains = titleNeedle;
        byExe.allowStoreNotepadHandoff = true;
        if (HWND hwnd = FindLaunchResultMainWindow(byExe, excl, launchFt, true, true)) {
            ForceHideLaunchedWindowQuiet(hwnd);
            return hwnd;
        }
        if (HWND hwnd = FindLaunchResultMainWindow(byExe, excl, launchFt, true, false)) {
            ForceHideLaunchedWindowQuiet(hwnd);
            return hwnd;
        }
    }
    // 无标题要求时，最后才退回纯 PID 窗。
    if (titleNeedle.empty()) {
        if (HWND hwnd = FindMainWindowForProcess(pid)) {
            ForceHideLaunchedWindowQuiet(hwnd);
            return hwnd;
        }
    }
    SquashProcessTopWindowsQuiet(pid);
    return nullptr;
}

void MinimizeForCrossDesktopMove(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return;
    ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
}

}  // namespace

MacroVirtualDesktop::~MacroVirtualDesktop() {
    Close();
}

bool MacroVirtualDesktop::RefreshDesktopList() {
    auto& vda = VirtualDesktopAccessor::Instance();
    const int index = vda.FindDesktopIndexByName(kMacroDesktopDisplayName);
    if (index < 0) return false;

    desktopIndex_ = index;
    if (!LoadMacroDesktopId(index, desktopId_)) {
        desktopId_ = GUID{};
    }
    SaveMacroDesktopCache(desktopIndex_, desktopId_);
    return true;
}

bool MacroVirtualDesktop::OpenOrCreate() {
    // 需求：没有「鼠标宏」则创建；创建后不得把用户视图留在宏桌面。
    if (ready_) return true;

    std::wstring err;
    auto& vda = VirtualDesktopAccessor::Instance();
    if (!vda.EnsureLoaded(err)) {
        lastError_ = err;
        DebugLog(lastError_.c_str());
        return false;
    }

    const int viewBefore = vda.GetCurrentDesktopNumber();

    auto restoreUserView = [&]() {
        if (viewBefore < 0) return;
        for (int i = 0; i < 12; ++i) {
            const int now = vda.GetCurrentDesktopNumber();
            if (now < 0 || now == viewBefore) break;
            vda.GoToDesktopNumber(viewBefore);
            Sleep(8);
        }
    };

    if (g_macroDesktopCache.index >= 0) {
        const int count = vda.GetDesktopCount();
        if (count > 0 && g_macroDesktopCache.index < count
            && VerifyMacroDesktopIndex(g_macroDesktopCache.index)) {
            desktopIndex_ = g_macroDesktopCache.index;
            LoadMacroDesktopId(desktopIndex_, desktopId_);
            ready_ = true;
            DebugLog(L"[WindowMode] MacroVirtualDesktop reuse cached desktop");
            restoreUserView();
            return true;
        }
        g_macroDesktopCache = {};
    }

    if (RefreshDesktopList()) {
        ready_ = true;
        DebugLog((L"[WindowMode] MacroVirtualDesktop found 「" + std::wstring(kMacroDesktopDisplayName)
            + L"」via " + vda.LoadedDllName()).c_str());
        restoreUserView();
        return true;
    }

    // 没有则创建。CreateDesktop 常会切屏——立刻拉回用户原桌面。
    const int created = vda.CreateDesktopPreservingView();
    if (created < 0) {
        lastError_ = L"创建虚拟桌面失败 (VirtualDesktopAccessor CreateDesktop)";
        DebugLog(lastError_.c_str());
        return false;
    }

    desktopIndex_ = created;
    vda.SetDesktopName(desktopIndex_, kMacroDesktopDisplayName);
    LoadMacroDesktopId(desktopIndex_, desktopId_);

    if (!VerifyMacroDesktopIndex(desktopIndex_) && !RefreshDesktopList()) {
        lastError_ = L"创建后无法定位「鼠标宏」虚拟桌面";
        desktopIndex_ = -1;
        desktopId_ = GUID{};
        restoreUserView();
        return false;
    }

    SaveMacroDesktopCache(desktopIndex_, desktopId_);
    ready_ = true;
    restoreUserView();
    DebugLog((L"[WindowMode] MacroVirtualDesktop created 「" + std::wstring(kMacroDesktopDisplayName)
        + L"」via " + vda.LoadedDllName()).c_str());
    return true;
}

void MacroVirtualDesktop::Close() {
    if (desktopIndex_ >= 0) {
        SaveMacroDesktopCache(desktopIndex_, desktopId_);
    }
    desktopId_ = GUID{};
    desktopIndex_ = -1;
    ready_ = false;
}

void MacroVirtualDesktop::WarmupAtProcessStart() {
    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (!vda.EnsureLoaded(err)) return;
    const int index = vda.FindDesktopIndexByName(kMacroDesktopDisplayName);
    if (index < 0) {
        WindowModeLog(L"[窗口模式] 预热：未找到「鼠标宏」桌面（创建推迟到绑窗）");
        return;
    }
    GUID id{};
    if (!LoadMacroDesktopId(index, id)) {
        vda.GetDesktopIdByNumber(index, id);
    }
    if (!GuidIsEmpty(id)) {
        SaveMacroDesktopCache(index, id);
    }
    WindowModeLog(L"[窗口模式] 预热：已缓存「鼠标宏」桌面（CreateDesktop+HoldView 未跑）");
}

bool MacroVirtualDesktop::IsValid() const {
    return ready_ && desktopIndex_ >= 0;
}

bool MacroVirtualDesktop::MoveWindowToMacroDesktop(HWND hwnd) {
    if (!IsValid()) {
        lastError_ = L"宏虚拟桌面未就绪";
        return false;
    }

    hwnd = TopLevelWindow(hwnd);
    if (!hwnd || !IsWindow(hwnd)) {
        lastError_ = L"无效窗口句柄";
        return false;
    }

    auto& vda = VirtualDesktopAccessor::Instance();
    if (vda.IsWindowOnDesktopNumber(hwnd, desktopIndex_)) return true;

    const int currentDesktop = vda.GetWindowDesktopNumber(hwnd);
    const bool crossDesktop = currentDesktop < 0 || currentDesktop != desktopIndex_;
    (void)crossDesktop;

    // 只搬窗口；若系统带走视图则立刻切回（CreateDesktopPreservingView 同思路）。
    if (!vda.MoveWindowToDesktopNumberPreservingView(hwnd, desktopIndex_)) {
        lastError_ = L"移动窗口到「鼠标宏」虚拟桌面失败";
        DebugLog(lastError_.c_str());
        return false;
    }

    DebugLog(L"[WindowMode] MoveWindowToMacroDesktop OK");
    return true;
}

bool MacroVirtualDesktop::LaunchProcess(const std::wstring& exe, const std::wstring& args,
    PROCESS_INFORMATION& outPi, const std::wstring& titleContains) {
    if (!IsValid() || exe.empty()) {
        lastError_ = L"启动参数无效";
        return false;
    }
    if (IsCancelled()) {
        lastError_ = L"已取消";
        return false;
    }

    ZeroMemory(&outPi, sizeof(outPi));

    // 启动前快照：避免后面按 exe 回退时误绑用户桌面上已打开的同名程序。
    std::vector<HWND> existingBefore;
    {
        WindowTargetQuery pre{};
        pre.exePath = exe;
        pre.allowStoreNotepadHandoff = true;
        CollectMatchingMainWindows(pre, true, existingBefore);
        if (existingBefore.empty()) CollectMatchingMainWindows(pre, false, existingBefore);
    }

    FILETIME launchTimeUtc{};
    GetSystemTimeAsFileTime(&launchTimeUtc);

    std::wstring cmdLine = L"\"" + exe + L"\"";
    if (!args.empty()) {
        cmdLine += L" ";
        cmdLine += FormatLaunchArgs(args);
    }
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    // Minimized + no activate from the start. Hosts that ignore this still get squashed below.
    si.wShowWindow = SW_SHOWMINNOACTIVE;

    std::wstring workDir;
    const auto slash = exe.find_last_of(L"\\/");
    if (slash != std::wstring::npos) workDir = exe.substr(0, slash);

    const BOOL created = CreateProcessW(
        nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
        CREATE_DEFAULT_ERROR_MODE, nullptr,
        workDir.empty() ? nullptr : workDir.c_str(), &si, &outPi);

    if (!created) {
        lastError_ = L"CreateProcess 失败: " + FormatWin32Error(GetLastError());
        DebugLog(lastError_.c_str());
        return false;
    }

    // Prefer short idle wait — do not linger while a visible frame sits on the user desktop.
    if (outPi.hProcess) {
        const DWORD waitResult = WaitForInputIdle(outPi.hProcess, 200);
        if (waitResult == WAIT_FAILED && IsCancelled()) {
            lastError_ = L"已取消";
            return false;
        }
        (void)waitResult;
    }
    SquashProcessTopWindowsQuiet(outPi.dwProcessId);

    if (IsCancelled()) {
        lastError_ = L"已取消";
        return false;
    }

    if (HWND hwnd = WaitForProcessMainWindow(
            outPi.dwProcessId,
            titleContains.empty() ? 2000u : 3000u,
            cancelFlag_, &exe,
            &existingBefore, &launchTimeUtc, titleContains.empty() ? nullptr : &titleContains)) {
        MinimizeForCrossDesktopMove(hwnd);
        ForceHideLaunchedWindowQuiet(hwnd);
        if (!MoveWindowToMacroDesktop(hwnd)) {
            lastError_ = L"已启动目标程序，但无法移入「鼠标宏」虚拟桌面";
            DebugLog(lastError_.c_str());
            return false;
        }
        // Stay minimized on「鼠标宏」; expand only when capture/input needs it.
        SquashProcessTopWindowsQuiet(outPi.dwProcessId);
        ForceHideLaunchedWindowQuiet(hwnd);
        PinMacroDesktopWindowBottom(hwnd);
        ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    } else if (IsCancelled()) {
        lastError_ = L"已取消";
        return false;
    } else if (outPi.dwProcessId == 0) {
        lastError_ = L"已启动目标程序，但未获取到进程 ID";
        DebugLog(lastError_.c_str());
        return false;
    } else {
        // 最后一次尝试：含单实例复用（AppsFolder / 商店交接常见）。
        WindowTargetQuery byExe{};
        byExe.exePath = exe;
        byExe.titleContains = titleContains;
        byExe.allowStoreNotepadHandoff = true;
        HWND byPath = FindLaunchResultMainWindow(byExe, existingBefore, launchTimeUtc, true, true);
        if (!byPath) {
            byPath = FindLaunchResultMainWindow(byExe, existingBefore, launchTimeUtc, true, false);
        }
        if (byPath) {
            MinimizeForCrossDesktopMove(byPath);
            ForceHideLaunchedWindowQuiet(byPath);
            if (!MoveWindowToMacroDesktop(byPath)) {
                lastError_ = L"已启动目标程序，但无法移入「鼠标宏」虚拟桌面";
                DebugLog(lastError_.c_str());
                return false;
            }
            ForceHideLaunchedWindowQuiet(byPath);
            PinMacroDesktopWindowBottom(byPath);
            ShowWindow(byPath, SW_SHOWMINNOACTIVE);
        } else {
            SquashProcessTopWindowsQuiet(outPi.dwProcessId);
            WindowModeLogf(L"[窗口模式] 进程已启动 pid=%lu，等待主窗口出现…",
                static_cast<unsigned long>(outPi.dwProcessId));
        }
    }

    DebugLog(L"[WindowMode] LaunchProcess OK");
    return true;
}

bool GetWindowVirtualDesktopId(HWND hwnd, GUID& outId) {
    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (!vda.EnsureLoaded(err)) return false;
    return vda.GetWindowDesktopId(hwnd, outId);
}

bool IsWindowOnVirtualDesktop(HWND hwnd, const GUID& desktopId) {
    if (GuidIsEmpty(desktopId)) return false;

    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (!vda.EnsureLoaded(err)) return false;

    int index = -1;
    if (vda.FindDesktopIndexByGuid(desktopId, index)) {
        return vda.IsWindowOnDesktopNumber(hwnd, index);
    }

    GUID current{};
    if (!GetWindowVirtualDesktopId(hwnd, current)) return false;
    return IsEqualGUID(current, desktopId);
}

bool IsWindowOnVirtualDesktopIndex(HWND hwnd, int desktopIndex) {
    if (desktopIndex < 0) return false;
    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (!vda.EnsureLoaded(err)) return false;
    return vda.IsWindowOnDesktopNumber(hwnd, desktopIndex);
}

}  // namespace windowmode
