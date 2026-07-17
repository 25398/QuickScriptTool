#include "window_mode_executor.h"

#include "background_window_input.h"
#include "action_utils.h"
#include "coord_space.h"
#include "window_mode_log.h"
#include "window_mode_requirements.h"
#include "window_capture.h"
#include "window_coords.h"
#include "window_mode_permission.h"
#include "window_target.h"
#include "virtual_desktop_accessor.h"

#include "action_utils.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace windowmode {

namespace {

void DebugLog(const wchar_t* msg) {
    WindowModeLog(msg);
}

void ClientMatchResultsToScreen(HWND hwnd, ImageMatchOutput& output) {
    if (!hwnd || !IsWindow(hwnd)) return;
    for (auto& m : output.matches) {
        ClientToScreenPoint(hwnd, m.topLeftX, m.topLeftY, m.topLeftX, m.topLeftY);
        ClientToScreenPoint(hwnd, m.bottomRightX, m.bottomRightY, m.bottomRightX, m.bottomRightY);
        ClientToScreenPoint(hwnd, m.x, m.y, m.x, m.y);
    }
}

void MapMatchPointBetweenWindows(HWND fromHwnd, HWND toHwnd, int& x, int& y) {
    if (!fromHwnd || !toHwnd || fromHwnd == toHwnd) return;
    int sx = 0;
    int sy = 0;
    if (!ClientToScreenPoint(fromHwnd, x, y, sx, sy)) return;
    ScreenToClientPoint(toHwnd, sx, sy, x, y);
}

void MapMatchResultsToInputClient(HWND captureHwnd, HWND inputHwnd, ImageMatchOutput& output) {
    if (!captureHwnd || !inputHwnd || captureHwnd == inputHwnd) return;
    for (auto& m : output.matches) {
        MapMatchPointBetweenWindows(captureHwnd, inputHwnd, m.topLeftX, m.topLeftY);
        MapMatchPointBetweenWindows(captureHwnd, inputHwnd, m.bottomRightX, m.bottomRightY);
        MapMatchPointBetweenWindows(captureHwnd, inputHwnd, m.x, m.y);
    }
}

}  // namespace

bool WindowModeExecutor::CheckRunHealth(const WindowModeScriptConfig& config, std::wstring& err) {
    if (!config.enabled) return true;

    if (config.executionKind == WindowModeExecutionKind::BackgroundWindow) {
        if (config.targetExePath.empty()
            && config.windowClassName.empty()
            && config.windowName.empty()
            && config.targetWindowTitle.empty()
            && config.targetPickX == 0 && config.targetPickY == 0) {
            err = L"后台窗口模式请先指定目标窗口";
            return false;
        }

        WindowModeScriptConfig probe = config;
        probe.autoLaunchTarget = false;
        WindowModeSession session;
        if (!session.Start(probe, err)) return false;
        if (session.State().targetHwnd) return true;
        if (!config.targetExePath.empty()) return session.ValidateTargetExe(err);
        err = L"未找到目标窗口，请确认窗口已打开且类名/标题匹配";
        return false;
    }

    if (config.targetExePath.empty()) {
        err = L"请填写目标程序路径";
        return false;
    }

    WindowModeScriptConfig probe = config;
    probe.autoLaunchTarget = true;

    WindowModeSession session;
    if (!session.Start(probe, err)) return false;
    return session.ValidateTargetExe(err);
}

bool WindowModeExecutor::UsesBackgroundWindow() const {
    return active_ && session_.Config().executionKind == WindowModeExecutionKind::BackgroundWindow;
}

bool WindowModeExecutor::UsesClientCoords() const {
    return session_.Config().coordSpace == WindowModeCoordinateSpace::WindowClient;
}

HWND WindowModeExecutor::CaptureTargetHwnd() const {
    HWND hwnd = TargetHwnd();
    if (!hwnd) return nullptr;
    const bool childBinding = !session_.Config().childWindowClassName.empty()
        || (GetParent(hwnd) != nullptr && GetParent(hwnd) != GetDesktopWindow())
        || FindBrowserRenderWidget(hwnd) == hwnd;
    if (childBinding) {
        HWND root = GetAncestor(hwnd, GA_ROOT);
        if (root && IsWindow(root)) return root;
    }
    return hwnd;
}

HWND WindowModeExecutor::VisionCaptureHwnd() const {
    HWND target = TargetHwnd();
    HWND root = CaptureTargetHwnd();
    if (!target || !root) return root;

    if (target == root) {
        if (HWND render = FindBrowserRenderWidget(root)) {
            RECT rc{};
            if (GetClientRect(render, &rc)) {
                const int w = std::max(0, static_cast<int>(rc.right - rc.left));
                const int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
                if (w > 0 && h > 0) return render;
            }
        }
        return root;
    }

    RECT rc{};
    if (GetClientRect(target, &rc)) {
        const int w = std::max(0, static_cast<int>(rc.right - rc.left));
        const int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
        if (w > 0 && h > 0) return target;
    }

    const auto& st = session_.State();
    if (st.clientW > 0 && st.clientH > 0) return target;
    return root;
}

bool WindowModeExecutor::EnsureTargetReady(std::wstring& err) {
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        return RefreshTarget(err);
    }

    HWND root = TopLevelTargetWindow(hwnd);
    const bool background = UsesBackgroundWindow();
    const bool onUserDesktop = IsWindowOnUserCurrentDesktop(root);

    if (background || onUserDesktop) {
        const bool skipVisibleZOrder = background && !IsTargetWindowMinimized(hwnd);
        if (IsTargetWindowMinimized(hwnd)) {
            RestoreWindowQuiet(root, skipVisibleZOrder);
            if (IsTargetWindowMinimized(hwnd)) {
                RestoreWindowNonActivating(root);
                if (IsTargetWindowMinimized(hwnd)) {
                    err = L"目标窗口仍处于最小化状态";
                    return false;
                }
            }
        } else {
            RestoreWindowQuiet(root, skipVisibleZOrder);
        }
    }
    return true;
}

bool WindowModeExecutor::EnsureTargetBound(std::wstring& err) {
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        return RefreshTarget(err);
    }

    HWND root = TopLevelTargetWindow(hwnd);
    if (!root || !IsWindow(root)) {
        return RefreshTarget(err);
    }
    return true;
}

bool WindowModeExecutor::PrepareVisionCapture() {
    std::wstring err;
    return EnsureTargetBound(err);
}

bool WindowModeExecutor::ResolveOcrScreenRect(const ScriptAction& a,
    int& sx1, int& sy1, int& sx2, int& sy2,
    HBITMAP lockedBmp, int lockX, int lockY) {
    if (!active_) return false;

    if (a.ocrRegionByImage) {
        ScriptAction probe = a;
        ImageMatchOutput output = FindImageClient(probe, lockedBmp, lockX, lockY);
        if (output.matches.empty()) return false;
        const ImageMatchResult& match = output.matches.front();
        const int cx1 = match.topLeftX + a.searchX1;
        const int cy1 = match.topLeftY + a.searchY1;
        const int cx2 = match.topLeftX + a.searchX2;
        const int cy2 = match.topLeftY + a.searchY2;
        return MapClientRect(cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2);
    }

    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
    if (!ResolveClientSearchRect(a, cx1, cy1, cx2, cy2)) return false;
    return MapClientRect(cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2);
}

bool WindowModeExecutor::ResolveAiScreenRect(const ScriptAction& a,
    int& sx1, int& sy1, int& sx2, int& sy2,
    HBITMAP lockedBmp, int lockX, int lockY) {
    if (!active_) return false;

    if (a.aiRegionByImage && !a.aiTargetImagePath.empty()) {
        ScriptAction probe = a;
        if (probe.imagePath.empty()) probe.imagePath = probe.aiTargetImagePath;
        ImageMatchOutput output = FindImageClient(probe, lockedBmp, lockX, lockY);
        if (output.matches.empty()) return false;
        const ImageMatchResult& match = output.matches.front();
        int cx1 = 0;
        int cy1 = 0;
        int cx2 = 0;
        int cy2 = 0;
        if (a.aiSearchX2 > a.aiSearchX1 && a.aiSearchY2 > a.aiSearchY1) {
            cx1 = match.topLeftX + a.aiSearchX1;
            cy1 = match.topLeftY + a.aiSearchY1;
            cx2 = match.topLeftX + a.aiSearchX2;
            cy2 = match.topLeftY + a.aiSearchY2;
        } else {
            cx1 = match.topLeftX;
            cy1 = match.topLeftY;
            cx2 = match.bottomRightX;
            cy2 = match.bottomRightY;
        }
        return MapClientRect(cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2);
    }

    if (a.aiSearchX2 > a.aiSearchX1 && a.aiSearchY2 > a.aiSearchY1) {
        return MapClientRect(a.aiSearchX1, a.aiSearchY1, a.aiSearchX2, a.aiSearchY2,
            sx1, sy1, sx2, sy2);
    }

    const auto& st = session_.State();
    sx1 = st.clientRectScreen.left;
    sy1 = st.clientRectScreen.top;
    sx2 = st.clientRectScreen.right;
    sy2 = st.clientRectScreen.bottom;
    return sx2 > sx1 && sy2 > sy1;
}

bool WindowModeExecutor::BeginRun(const WindowModeScriptConfig& config, std::wstring& err,
    BeginRunOptions options) {
    // 需求见 window_mode_requirements.h：
    // §1 指定窗口类按身份打开；§2 启动不切宏桌面视图；§3 等待要短。
    EndRun();
    if (!config.enabled) {
        active_ = false;
        return true;
    }

    ResetSoftMouseState();
    cancelFlag_ = options.cancelFlag;
    session_.SetCancelFlag(options.cancelFlag);
    session_.SetLaunchSearchDir(options.launchSearchDir);

    if (WindowModeCancelled(options.cancelFlag)) {
        err = L"已取消";
        EndRun();
        return false;
    }

    // 统一写入：避免 JSON 里 autoLaunchTarget=0 导致 Start/Bind 提前失败。
    WindowModeScriptConfig runConfig = config;
    runConfig.autoLaunchTarget = ShouldAutoLaunchTarget(runConfig);

    if (!session_.Start(runConfig, err)) {
        active_ = false;
        return false;
    }

    const bool background = runConfig.executionKind == WindowModeExecutionKind::BackgroundWindow;
    const bool shouldAutoLaunch = ShouldAutoLaunchTarget(runConfig);

    if (shouldAutoLaunch) {
        if (options.launchTarget) {
            // 「指定窗口类」绑定后须再校验标题/文档身份：勿把同程序其它窗当成已找到而跳过打开。
            bool bound = session_.RefreshTarget(err);
            if (bound && session_.State().targetHwnd
                && runConfig.selectMethod == WindowSelectMethod::UseEditorWindowClass) {
                HWND top = session_.State().targetHwnd;
                HWND root = GetAncestor(top, GA_ROOT);
                if (!root) root = top;
                if (!DoesTopWindowMatchConfig(root, runConfig)) {
                    WindowModeLog(L"[窗口模式] 已绑窗口与指定标题/文档不符，改为自动打开目标");
                    session_.ClearTargetBinding();
                    bound = false;
                    err.clear();
                }
            }
            if (bound && session_.State().targetHwnd) {
                WindowModeLog(L"[窗口模式] 已找到匹配的目标窗口，跳过自动打开");
            } else {
                WindowModeLogf(L"[窗口模式] 未找到目标窗口，自动打开: %s",
                    runConfig.targetExePath.c_str());
                // 只清绑定，保留宏桌面，避免 Close/Open 虚拟桌面造成卡顿。
                session_.ClearTargetBinding();
                const bool launched = background
                    ? session_.LaunchTargetOnDefaultDesktop(err)
                    : session_.LaunchTargetOnDesktop(err);
                if (!launched) {
                    EndRun();
                    return false;
                }
                // 启动后再次核验指定窗口类身份（防止绑到空白同程序窗）。
                if (runConfig.selectMethod == WindowSelectMethod::UseEditorWindowClass
                    && session_.State().targetHwnd) {
                    HWND top = session_.State().targetHwnd;
                    HWND root = GetAncestor(top, GA_ROOT);
                    if (!root) root = top;
                    if (!DoesTopWindowMatchConfig(root, runConfig)) {
                        err = L"已启动程序，但未绑定到指定标题/文档对应的窗口。"
                              L"请确认脚本目录旁有该文件，或在「指定窗口类」拾取时保存文档路径。";
                        EndRun();
                        return false;
                    }
                }
            }
        } else if (!session_.RefreshTarget(err)) {
            if (!session_.ValidateTargetExe(err)) {
                EndRun();
                return false;
            }
            active_ = false;
            EndRun();
            return true;
        }
    } else if (!session_.RefreshTarget(err)) {
        if (background) {
            if (runConfig.windowClassName.empty() && runConfig.windowName.empty()) {
                err = L"后台窗口模式请先指定目标窗口";
            }
        } else if (runConfig.targetExePath.empty()) {
            err = L"窗口模式已启用，但未配置目标程序";
        }
        EndRun();
        return false;
    }

    const auto& st = session_.State();
    if (st.targetPid != 0 && !CheckPermissionMatch(st.targetPid)) {
        err = HealthToUserHint(WindowModeHealth::PermissionMismatch);
        EndRun();
        return false;
    }

    if (st.health != WindowModeHealth::Ok) {
        err = HealthToUserHint(st.health);
        if (!st.lastError.empty()) err = st.lastError;
        EndRun();
        return false;
    }

    active_ = true;
    DebugLog(L"[WindowMode] Executor::BeginRun OK");
    return true;
}

void WindowModeExecutor::EndRun() {
    session_.Stop();
    active_ = false;
    cancelFlag_ = nullptr;
    ResetSoftMouseState();
}

HWND WindowModeExecutor::TargetHwnd() const {
    return session_.State().targetHwnd;
}

WindowModeHealth WindowModeExecutor::Health() const {
    return session_.State().health;
}

bool WindowModeExecutor::RefreshTarget(std::wstring& err) {
    if (!active_) return false;
    if (!session_.RefreshTarget(err)) return false;
    const auto health = session_.State().health;
    if (health != WindowModeHealth::Ok) {
        err = session_.State().lastError.empty()
            ? HealthToUserHint(health) : session_.State().lastError;
        return false;
    }
    return true;
}

bool WindowModeExecutor::MoveToScreen(int sx, int sy) {
    return SetCursorPos(sx, sy) == TRUE;
}

bool WindowModeExecutor::PrepareSoftInput(std::wstring& err) {
    // Message-based input must NOT expand / raise the target.
    // Vision capture (find-image/OCR) uses ScopedVisionCapturePrep for quiet bottom restore.
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        return RefreshTarget(err);
    }
    return EnsureTargetBound(err);
}

void WindowModeExecutor::MoveMouseClient(int cx, int cy, int randomX, int randomY,
    const std::function<int(int)>& randomInt) {
    if (!active_) return;

    const int rx = randomInt(randomX);
    const int ry = randomInt(randomY);
    const int tx = cx + rx;
    const int ty = cy + ry;

    std::wstring err;
    if (!PrepareSoftInput(err)) {
        DebugLog(L"[WindowMode] MoveMouseClient: target not ready");
        return;
    }
    PostMouseMoveToWindow(TargetHwnd(), tx, ty);
}

void WindowModeExecutor::PostMouseButtonAtClient(int cx, int cy, MouseButtonType button, bool down) {
    if (!active_) return;

    std::wstring err;
    if (!PrepareSoftInput(err)) return;
    PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, down);
}

void WindowModeExecutor::PostMouseClickAtClient(int cx, int cy, MouseButtonType button) {
    if (!active_) return;

    std::wstring err;
    if (!PrepareSoftInput(err)) return;

    HWND inputHwnd = TargetHwnd();
    HWND top = TopLevelTargetWindow(inputHwnd);
    const bool background = UsesBackgroundWindow();
    const bool needQuietRestore = background && top && IsWindow(top) && IsIconic(top);
    HWND preserveFg = GetForegroundWindow();
    LONG savedExStyle = 0;
    bool layeredTouched = false;
    BYTE savedLayeredAlpha = 255;
    DWORD savedLayeredFlags = 0;
    if (needQuietRestore) {
        // 找图结束后目标常被还原为最小化；最小化态下 RichEdit 往往不理会点击，光标不会动。
        // 保持 Cloak 展开，避免点击瞬间再露脸。
        RestoreOnUserDesktopBottom(top, preserveFg, true,
            &savedExStyle, &layeredTouched, &savedLayeredAlpha, &savedLayeredFlags);
        WindowModeLog(L"[窗口模式] 后台点击前: Cloak置底展开以便落入输入点");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    PostMouseButtonToWindow(inputHwnd, cx, cy, button, true);
    PostMouseButtonToWindow(inputHwnd, cx, cy, button, false);

    if (needQuietRestore && top && IsWindow(top)) {
        ShowWindow(top, SW_SHOWMINNOACTIVE);
        EndQuietBottomCloak(top, savedExStyle, layeredTouched,
            savedLayeredAlpha, savedLayeredFlags);
        EnsureTargetBelowUserWindows(top);
        if (preserveFg && IsWindow(preserveFg) && GetForegroundWindow() == top) {
            SetForegroundWindow(preserveFg);
        }
    }
}

void WindowModeExecutor::PostKeyToTarget(UINT vk, bool down) {
    if (!active_ || vk == 0) return;

    std::wstring err;
    if (!PrepareSoftInput(err)) return;
    PostKeyToWindow(TargetHwnd(), vk, down);
}

void WindowModeExecutor::PostScrollWheelAtClient(int cx, int cy, int steps, bool vertical, bool positive) {
    if (!active_ || steps <= 0) return;
    std::wstring err;
    if (!PrepareSoftInput(err)) return;
    PostScrollWheelToWindow(TargetHwnd(), cx, cy, steps, vertical, positive);
}

void WindowModeExecutor::SendQuickInputToTarget(const std::wstring& text, double charInterval) {
    if (!active_) return;
    std::wstring err;
    if (!PrepareSoftInput(err)) {
        WindowModeLogf(L"[窗口模式] 快捷输入跳过: %s", err.c_str());
        return;
    }
    if (!session_.RefreshInputBinding(err)) {
        WindowModeLogf(L"[窗口模式] 快捷输入刷新绑定失败: %s", err.c_str());
        return;
    }

    HWND hwnd = TargetHwnd();
    HWND top = TopLevelTargetWindow(hwnd);
    const bool background = UsesBackgroundWindow();
    const bool needQuietRestore = background && top && IsWindow(top) && IsIconic(top);
    HWND preserveFg = GetForegroundWindow();
    LONG savedExStyle = 0;
    bool layeredTouched = false;
    BYTE savedLayeredAlpha = 255;
    DWORD savedLayeredFlags = 0;
    if (needQuietRestore) {
        RestoreOnUserDesktopBottom(top, preserveFg, true,
            &savedExStyle, &layeredTouched, &savedLayeredAlpha, &savedLayeredFlags);
        WindowModeLog(L"[窗口模式] 后台输入前: Cloak置底展开");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    const bool allowForeground = session_.Config().allowForegroundInputFallback;
    PostQuickInputToWindow(hwnd, text, charInterval, allowForeground, cancelFlag_);

    if (needQuietRestore && top && IsWindow(top)) {
        ShowWindow(top, SW_SHOWMINNOACTIVE);
        EndQuietBottomCloak(top, savedExStyle, layeredTouched,
            savedLayeredAlpha, savedLayeredFlags);
        EnsureTargetBelowUserWindows(top);
        if (preserveFg && IsWindow(preserveFg) && GetForegroundWindow() == top) {
            SetForegroundWindow(preserveFg);
        }
    }
}

bool WindowModeExecutor::ResolveClientSearchRect(const ScriptAction& a,
    int& x1, int& y1, int& x2, int& y2) const {
    if (!active_) return false;

    HWND hwnd = VisionCaptureHwnd();
    if (!hwnd || !IsWindow(hwnd)) hwnd = TargetHwnd();

    int clientW = 0;
    int clientH = 0;
    int liveW = 0;
    int liveH = 0;
    RECT clientRc{};
    if (hwnd && GetClientRect(hwnd, &clientRc)) {
        liveW = std::max(0, static_cast<int>(clientRc.right - clientRc.left));
        liveH = std::max(0, static_cast<int>(clientRc.bottom - clientRc.top));
        clientW = liveW;
        clientH = liveH;
    }

    const auto& st = session_.State();
    if (clientW <= 0 || clientH <= 0) {
        clientW = st.clientW;
        clientH = st.clientH;
    }
    if (clientW <= 0 || clientH <= 0) {
        WindowModeLogf(L"[窗口模式] ResolveClientSearchRect: 客户区无效 hwnd=0x%p live=%dx%d bound=%dx%d",
            hwnd, liveW, liveH, st.clientW, st.clientH);
        return false;
    }

    if (a.searchFullScreen || (a.searchX1 == 0 && a.searchY1 == 0 && a.searchX2 == 0 && a.searchY2 == 0)) {
        x1 = 0;
        y1 = 0;
        x2 = clientW;
        y2 = clientH;
        return true;
    }

    // 找图配置为整屏(如 0,0,2560,1440) 时，窗口模式下等价于全客户区。
    if (UsesClientCoords() && a.searchX2 > a.searchX1 && a.searchY2 > a.searchY1
        && a.searchX1 <= 0 && a.searchY1 <= 0
        && a.searchX2 >= clientW && a.searchY2 >= clientH) {
        x1 = 0;
        y1 = 0;
        x2 = clientW;
        y2 = clientH;
        WindowModeLog(L"[窗口模式] ResolveClientSearchRect: 整屏搜索映射为全客户区");
        return true;
    }

    if (UsesClientCoords()) {
        auto applyClientRect = [&](int cx1, int cy1, int cx2, int cy2) -> bool {
            const int L = std::min(cx1, cx2);
            const int T = std::min(cy1, cy2);
            const int R = std::max(cx1, cx2);
            const int B = std::max(cy1, cy2);
            x1 = std::clamp(L, 0, clientW);
            y1 = std::clamp(T, 0, clientH);
            x2 = std::clamp(R, 0, clientW);
            y2 = std::clamp(B, 0, clientH);
            return x2 > x1 && y2 > y1;
        };

        HWND root = TopLevelTargetWindow(CaptureTargetHwnd());
        const bool iconic = root && IsIconic(root);

        if (!iconic && hwnd) {
            int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
            if (ScreenSearchRectToClientRect(hwnd, a.searchX1, a.searchY1, a.searchX2, a.searchY2,
                    cx1, cy1, cx2, cy2)
                && applyClientRect(cx1, cy1, cx2, cy2)) {
                return true;
            }
        }

        const RECT& bound = st.clientRectScreen;
        if (bound.right > bound.left && bound.bottom > bound.top) {
            const int cx1 = a.searchX1 - bound.left;
            const int cy1 = a.searchY1 - bound.top;
            const int cx2 = a.searchX2 - bound.left;
            const int cy2 = a.searchY2 - bound.top;
            if (applyClientRect(cx1, cy1, cx2, cy2)) {
                WindowModeLogf(L"[窗口模式] ResolveClientSearchRect: 绑定原点映射 screen(%d,%d,%d,%d) bound(%d,%d,%d,%d)",
                    a.searchX1, a.searchY1, a.searchX2, a.searchY2,
                    bound.left, bound.top, bound.right, bound.bottom);
                return true;
            }
        }

        if (a.searchX2 > a.searchX1 && a.searchY2 > a.searchY1
            && a.searchX1 >= 0 && a.searchY1 >= 0
            && a.searchX2 <= clientW && a.searchY2 <= clientH
            && applyClientRect(a.searchX1, a.searchY1, a.searchX2, a.searchY2)) {
            WindowModeLog(L"[窗口模式] ResolveClientSearchRect: 按客户区坐标解析");
            return true;
        }

        WindowModeLogf(L"[窗口模式] ResolveClientSearchRect: 搜索区无效 iconic=%d screen(%d,%d,%d,%d) 回退全客户区 %dx%d",
            iconic ? 1 : 0, a.searchX1, a.searchY1, a.searchX2, a.searchY2, clientW, clientH);
        x1 = 0;
        y1 = 0;
        x2 = clientW;
        y2 = clientH;
        return true;
    }

    x1 = a.searchX1;
    y1 = a.searchY1;
    x2 = a.searchX2;
    y2 = a.searchY2;
    return x2 > x1 && y2 > y1;
}

bool WindowModeExecutor::MapClientRect(int cx1, int cy1, int cx2, int cy2,
    int& sx1, int& sy1, int& sx2, int& sy2) const {
    if (!active_) return false;
    if (!UsesClientCoords()) {
        sx1 = cx1;
        sy1 = cy1;
        sx2 = cx2;
        sy2 = cy2;
        return cx2 > cx1 && cy2 > cy1;
    }
    return MapClientRectToScreen(VisionCaptureHwnd(), cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2);
}

ImageMatchOutput WindowModeExecutor::FindImageClient(const ScriptAction& a,
    HBITMAP lockedBmp, int lockX, int lockY) {
    (void)lockX;
    (void)lockY;
    ImageMatchOutput output{};
    if (!active_) {
        WindowModeLog(L"[窗口模式] FindImageClient: executor 未激活");
        return output;
    }

    std::wstring err;
    if (!EnsureTargetBound(err)) {
        WindowModeLogf(L"[窗口模式] FindImageClient: EnsureTargetBound 失败 %s",
            err.empty() ? L"(无详情)" : err.c_str());
        return output;
    }

    HWND prepRoot = TopLevelTargetWindow(CaptureTargetHwnd());
    int deskAtStart = -1;
    {
        auto& vda = VirtualDesktopAccessor::Instance();
        deskAtStart = vda.GetCurrentDesktopNumber();
    }
    WindowModeLogDesktopSnap(L"找图前", prepRoot);

    // 最小化时先完成截图准备（不展开窗口），再解析客户区坐标。
    ScopedVisionCapturePrep prep(CaptureTargetHwnd(), UsesBackgroundWindow());
    WindowModeLogDesktopSnap(L"截图准备后", prepRoot);

    if (!prep.Ready()) {
        WindowModeLog(L"[窗口模式] FindImageClient: VisionPrep 未就绪");
        return output;
    }
    // 置底展开后给一点绘制时间。
    if (UsesBackgroundWindow()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    ScriptAction probe = a;
    if (probe.imagePath.empty() && !probe.aiTargetImagePath.empty()) {
        probe.imagePath = probe.aiTargetImagePath;
    }

    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
    if (!ResolveClientSearchRect(probe, cx1, cy1, cx2, cy2)) {
        WindowModeLog(L"[窗口模式] FindImageClient: ResolveClientSearchRect 失败");
        return output;
    }

    HBITMAP tmpl = nullptr;
    ImageMatchOptions opt;
    TemplateScale ts{};
    // 模板与窗口截图像素同属屏幕 DPI 坐标系。缩放必须与默认模式一致：
    // currentVirtualScreen / captureScreen。用客户区尺寸去除屏幕 capture 会把模板
    // 错误缩小（如 1097/2560），导致匹配率直接掉到 0%。
    if (coordMeta_.captureWidth > 0 || coordMeta_.refWidth > 0) {
        int curX = 0, curY = 0, curW = 0, curH = 0;
        GetVirtualScreenBounds(curX, curY, curW, curH);
        ts = ComputeTemplateScale(coordMeta_, curW, curH);
    }
    const PreparedFindImageMatch findMatch = PrepareFindImageMatch(probe, ts);
    tmpl = findMatch.bitmap;
    opt = findMatch.options;
    if (!tmpl) {
        WindowModeLog(L"[窗口模式] FindImageClient: 模板图加载失败");
        return output;
    }

    WindowModeLogf(
        L"[窗口模式] FindImageClient: 模板 %dx%d scale=%.3fx%.3f matchScale=%.3f~%.3f thr=%.0f search=(%d,%d)-(%d,%d)",
        findMatch.templateW, findMatch.templateH, ts.sx, ts.sy,
        opt.scaleMin, opt.scaleMax, opt.thresholdPercent, cx1, cy1, cx2, cy2);

    opt.maxMatches = 20;
    opt.maxOverlap = 0.5;

    int sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
    if (!MapClientRect(cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2)) {
        WindowModeLog(L"[窗口模式] FindImageClient: MapClientRect 失败");
        DeleteBitmapHandle(tmpl);
        return output;
    }
    (void)sx1;
    (void)sy1;
    (void)sx2;
    (void)sy2;

    const HWND captureHwnd = VisionCaptureHwnd();
    const HWND inputHwnd = TargetHwnd();

    auto captureAndMatch = [&]() -> ImageMatchOutput {
        if (lockedBmp) {
            ImageMatchOutput matched = FindTemplateInFrozenScreenMulti(
                lockedBmp, 0, 0, cx1, cy1, cx2, cy2, tmpl, opt);
            MapMatchResultsToInputClient(captureHwnd, inputHwnd, matched);
            return matched;
        }
        WindowCaptureResult capture = CaptureWindowClient(captureHwnd);
        if (!capture.bitmap) {
            WindowModeLog(L"[窗口模式] FindImageClient: 截图失败");
            return {};
        }
        const auto& st = session_.State();
        if (st.clientW > 0 && st.clientH > 0
            && (capture.w < st.clientW / 2 || capture.h < st.clientH / 2)) {
            WindowModeLogf(L"[窗口模式] FindImageClient: 截图尺寸偏小 %dx%d 期望约%dx%d",
                capture.w, capture.h, st.clientW, st.clientH);
        }
        const bool blank = IsCaptureLikelyBlank(capture.bitmap);
        WindowModeLogf(L"[窗口模式] FindImageClient: 截图 %dx%d blank=%d print=%d wgc=%d",
            capture.w, capture.h, blank ? 1 : 0, capture.fromPrintWindow ? 1 : 0,
            capture.fromPrintWindow ? 0 : 1);
        if (blank) {
            DeleteObject(capture.bitmap);
            return {};
        }
        ImageMatchOutput matched = FindTemplateInFrozenScreenMulti(
            capture.bitmap, 0, 0, cx1, cy1, cx2, cy2, tmpl, opt);
        DeleteObject(capture.bitmap);
        MapMatchResultsToInputClient(captureHwnd, inputHwnd, matched);
        WindowModeLogf(L"[窗口模式] FindImageClient: 匹配数=%zu 最高=%.1f%%",
            matched.matches.size(),
            matched.matches.empty() ? 0.0 : matched.matches.front().score);
        return matched;
    };

    output = captureAndMatch();
    WindowModeLogDesktopSnap(L"找图后", prepRoot);
    if (deskAtStart >= 0) {
        auto& vda = VirtualDesktopAccessor::Instance();
        const int nowDesk = vda.GetCurrentDesktopNumber();
        if (nowDesk >= 0 && nowDesk != deskAtStart) {
            WindowModeLogf(L"[窗口模式] 找图后 UserDesk 变化: %d->%d",
                deskAtStart, nowDesk);
        }
    }

    DeleteBitmapHandle(tmpl);
    return output;
}

WindowModeExecutor::VisionPipelineDiag WindowModeExecutor::DiagnoseVisionPipeline() {
    VisionPipelineDiag diag{};
    if (!active_) {
        diag.ensureErr = L"executor 未激活";
        return diag;
    }

    diag.ensureReadyOk = EnsureTargetBound(diag.ensureErr);
    if (!diag.ensureReadyOk) return diag;

    if (!PrepareVisionCapture()) {
        diag.ensureErr = L"PrepareVisionCapture 失败";
        return diag;
    }

    ScopedVisionCapturePrep prep(CaptureTargetHwnd(), UsesBackgroundWindow());
    diag.prepReady = prep.Ready();
    if (!diag.prepReady) return diag;

    const HWND captureHwnd = VisionCaptureHwnd();
    WindowCaptureResult capture = CaptureWindowClient(captureHwnd);
    if (!capture.bitmap) return diag;

    diag.captureW = capture.w;
    diag.captureH = capture.h;
    diag.captureBlank = IsCaptureLikelyBlank(capture.bitmap);
    diag.captureOk = true;
    DeleteObject(capture.bitmap);
    return diag;
}

bool WindowModeExecutor::LockWindowCapture(HBITMAP& outBmp, int& outX, int& outY) {
    if (!active_) return false;
    if (!PrepareVisionCapture()) return false;
    ScopedVisionCapturePrep prep(CaptureTargetHwnd(), UsesBackgroundWindow());
    if (!prep.Ready()) return false;

    const HWND captureHwnd = VisionCaptureHwnd();
    WindowCaptureResult capture = CaptureWindowClient(captureHwnd);
    if (!capture.bitmap) return false;
    outBmp = capture.bitmap;
    outX = 0;
    outY = 0;
    return !IsCaptureLikelyBlank(outBmp);
}

HBITMAP WindowModeExecutor::CaptureScreenRegionFromWindow(int sx1, int sy1, int sx2, int sy2,
    HBITMAP lockedBmp, int lockX, int lockY) {
    (void)lockX;
    (void)lockY;
    if (!active_) return nullptr;
    if (!PrepareVisionCapture()) return nullptr;

    ScopedVisionCapturePrep prep(CaptureTargetHwnd(), UsesBackgroundWindow());
    if (!prep.Ready()) return nullptr;

    if (lockedBmp) {
        HWND hwnd = VisionCaptureHwnd();
        int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
        if (!ScreenToClientPoint(hwnd, sx1, sy1, cx1, cy1)
            || !ScreenToClientPoint(hwnd, sx2, sy2, cx2, cy2)) {
            return nullptr;
        }
        return CropBitmapClientRegion(lockedBmp, cx1, cy1, cx2, cy2);
    }

    HWND hwnd = VisionCaptureHwnd();
    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
    if (!ScreenToClientPoint(hwnd, sx1, sy1, cx1, cy1)
        || !ScreenToClientPoint(hwnd, sx2, sy2, cx2, cy2)) {
        return nullptr;
    }
    WindowCaptureResult region = CaptureWindowRegion(hwnd, cx1, cy1, cx2, cy2);
    return region.bitmap;
}

OcrEngineOutput WindowModeExecutor::RunOcrOnClientRegion(const ScriptAction& a,
    HBITMAP lockedBmp, int lockX, int lockY) {
    OcrEngineOutput output{};
    if (!active_) {
        output.error = L"目标窗口模式未激活";
        return output;
    }

    int sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
    if (!ResolveOcrScreenRect(a, sx1, sy1, sx2, sy2, lockedBmp, lockX, lockY)) {
        output.error = L"无法定位识别区域";
        return output;
    }

    HBITMAP regionBmp = CaptureScreenRegionFromWindow(sx1, sy1, sx2, sy2, lockedBmp, lockX, lockY);
    if (!regionBmp) {
        output.error = L"无法截取识别区域";
        return output;
    }

    output = RunOcrOnBitmap(regionBmp, sx1, sy1, a.ocrDigitsOnly);
    DeleteObject(regionBmp);
    return output;
}

bool WindowModeExecutor::GetCursorClientPos(int& cx, int& cy) const {
    if (!active_) return false;
    // Soft-input modes do not move the system cursor; report last posted client position.
    if (GetLastSoftMouseClientPos(TargetHwnd(), cx, cy)) {
        return true;
    }
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    if (!UsesClientCoords()) {
        cx = pt.x;
        cy = pt.y;
        return true;
    }
    return ScreenToClientPoint(TargetHwnd(), pt.x, pt.y, cx, cy);
}

}  // namespace windowmode
