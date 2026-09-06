#include "window_mode_executor.h"

#include "background_uia_input.h"
#include "background_window_input.h"
#include "background_input_target.h"
#include "action_utils.h"
#include "input/foreground_input_router.h"
#include "coord_space.h"
#include "ext_bridge/ext_bridge_server.h"
#include "window_mode_log.h"
#include "window_mode_requirements.h"
#include "window_capture.h"
#include "window_coords.h"
#include "window_mode_permission.h"
#include "window_target.h"
#include "window_list.h"
#include "virtual_desktop_accessor.h"
#include "fake_focus/fake_focus_soft_input_host.h"
#include "injection/inject_technique.h"

#include "action_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <thread>

namespace windowmode {

WindowModeExecutor::WindowModeExecutor() = default;
WindowModeExecutor::~WindowModeExecutor() {
    if (active_) EndRun();
}


namespace {

int g_mapleSoftKeyLogs = 0;

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
    ClientToScreenPoint(fromHwnd, x, y, sx, sy);
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


UINT VkFromMouseButton(MouseButtonType button) {
    switch (button) {
    case MouseButtonType::Right: return VK_RBUTTON;
    case MouseButtonType::Middle: return VK_MBUTTON;
    case MouseButtonType::X1: return VK_XBUTTON1;
    case MouseButtonType::X2: return VK_XBUTTON2;
    default: return VK_LBUTTON;
    }
}

bool FillClientSizeFromHwnd(HWND hwnd, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    outW = std::max(0, static_cast<int>(rc.right - rc.left));
    outH = std::max(0, static_cast<int>(rc.bottom - rc.top));
    if (outW < 400 || outH < 300) {
        HWND root = GetAncestor(hwnd, GA_ROOT);
        if (root && root != hwnd && GetClientRect(root, &rc)) {
            const int rw = std::max(0, static_cast<int>(rc.right - rc.left));
            const int rh = std::max(0, static_cast<int>(rc.bottom - rc.top));
            if (rw >= 400 && rh >= 300) {
                outW = rw;
                outH = rh;
            }
        }
    }
    return outW > 0 && outH > 0;
}

/// canvas 像素 → 宿主 surface；|sx-sy|>0.05 时强制等比。
void MapCanvasMatchesToSurface(ImageMatchOutput& output, int canvasW, int canvasH,
    int surfaceW, int surfaceH,
    int iframeCssX, int iframeCssY, int iframeCssW, int iframeCssH,
    int pageCssW, int pageCssH) {
    if (canvasW <= 0 || canvasH <= 0 || surfaceW <= 0 || surfaceH <= 0) return;
    double dx = 0, dy = 0, dw = surfaceW, dh = surfaceH;
    if (iframeCssW > 0 && iframeCssH > 0 && pageCssW > 0 && pageCssH > 0) {
        double scaleX = surfaceW / static_cast<double>(pageCssW);
        double scaleY = surfaceH / static_cast<double>(pageCssH);
        if (std::fabs(scaleX - scaleY) > 0.05) {
            const double s = 0.5 * (scaleX + scaleY);
            scaleX = s;
            scaleY = s;
        }
        dx = iframeCssX * scaleX;
        dy = iframeCssY * scaleY;
        dw = std::max(1.0, iframeCssW * scaleX);
        dh = std::max(1.0, iframeCssH * scaleY);
    }
    auto mapPt = [&](int& x, int& y) {
        x = static_cast<int>(std::lround(dx + x * dw / canvasW));
        y = static_cast<int>(std::lround(dy + y * dh / canvasH));
    };
    for (auto& m : output.matches) {
        mapPt(m.topLeftX, m.topLeftY);
        mapPt(m.bottomRightX, m.bottomRightY);
        mapPt(m.x, m.y);
    }
}

/// 可靠 surface：拒绝缩略客户区与严重非等比；优先 pageCss×均匀 dpr。
bool ResolveExtVisionSurface(ExtInputSession& ext, HWND hwnd, int preferW, int preferH,
    int& outW, int& outH) {
    outW = preferW;
    outH = preferH;
    if (outW < 400 || outH < 300) {
        FillClientSizeFromHwnd(TopLevelTargetWindow(hwnd), outW, outH);
    }
    int pw = 0, ph = 0;
    ext.GetPageCssSize(pw, ph);
    const double dpr = ext.DevicePixelRatio();
    if (pw >= 64 && ph >= 64) {
        const double sx = outW > 0 ? outW / static_cast<double>(pw) : 0.0;
        const double sy = outH > 0 ? outH / static_cast<double>(ph) : 0.0;
        const bool tiny = outW < 400 || outH < 300;
        const bool skew = (sx > 0.1 && sy > 0.1 && std::fabs(sx - sy) > 0.05);
        if (tiny || skew) {
            const double s = (dpr > 0.1) ? dpr : 1.5;
            outW = std::max(1, static_cast<int>(std::lround(pw * s)));
            outH = std::max(1, static_cast<int>(std::lround(ph * s)));
            return true;
        }
    }
    if (outW < 400 || outH < 300) {
        outW = 2560;
        outH = 1440;
    }
    return outW >= 64 && outH >= 64;
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

bool WindowModeExecutor::IsCdpInputMode() const {
    return active_ && UsesCdpInput(session_.Config());
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

    // CDP+扩展：找图/键鼠坐标统一用顶层客户区，勿绑 D3D/RenderWidget（ScreenToClient 会打偏）。
    if (IsCdpInputMode()) return root;

    // 截图可走合成层；输入绑定单独走 FindBrowserRenderWidget。
    if (HWND surface = FindBrowserCaptureSurface(root)) {
        RECT rc{};
        if (GetClientRect(surface, &rc)) {
            const int w = std::max(0, static_cast<int>(rc.right - rc.left));
            const int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
            if (w > 0 && h > 0) return surface;
        }
    }

    if (target == root) return root;

    RECT rc{};
    if (GetClientRect(target, &rc)) {
        const int w = std::max(0, static_cast<int>(rc.right - rc.left));
        const int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
        if (w > 0 && h > 0 && !IsBrowserCompositorHwnd(target)) return target;
    }

    const auto& st = session_.State();
    if (st.clientW > 0 && st.clientH > 0 && !IsBrowserCompositorHwnd(target)) return target;
    return root;
}

bool WindowModeExecutor::EnsureTargetReady(std::wstring& err) {
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        return RefreshTarget(err);
    }

    // CDP/扩展：不依赖 Win32 可见性；宏桌面查看/最小化都不打断标签页键鼠与找图。
    if (IsCdpInputMode()) {
        err.clear();
        return true;
    }
    if (PreferHardwareInput()) {
        err.clear();
        return true;
    }

    HWND root = TopLevelTargetWindow(hwnd);
    const bool background = UsesBackgroundWindow();
    const bool onUserDesktop = IsWindowOnUserCurrentDesktop(root);

    if (background || onUserDesktop) {
        const bool skipVisibleZOrder = background && !IsTargetWindowMinimized(hwnd);
        if (IsTargetWindowMinimized(hwnd)) {
            // 后台窗口模式：目标保持最小化/离屏，绝不恢复或前置。
            // PostMessage 对最小化窗口仍能送达鼠标/键盘消息；恢复只会让窗口“弹出”，
            // 违背后台操作的预期（用户已明确要求完全不切换前台）。
            if (background) {
                err.clear();
                return true;
            }
            RestoreWindowQuiet(root, skipVisibleZOrder);
            if (IsTargetWindowMinimized(hwnd)) {
                RestoreWindowNonActivating(root);
                if (IsTargetWindowMinimized(hwnd)) {
                    err = L"目标窗口仍处于最小化状态";
                    return false;
                }
            }
        } else if (!background) {
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
    // 后台：不迁宏桌面。CDP：安静停放（偏离则迁回并最小化），禁止 Cloak/还原。
    if (UsesBackgroundWindow()) {
        err.clear();
        return true;
    }
    if (IsCdpInputMode()) {
        // CDP 找图/键鼠只走扩展；禁止每次 Ensure 再 Park/Move。
        err.clear();
        return true;
    }
    if (PreferHardwareInput()) {
        err.clear();
        return true;
    }
    // softMessage/假焦点：目标须留在「鼠标宏」。
    auto& vda = VirtualDesktopAccessor::Instance();
    const int macroIdx = vda.FindDesktopIndexByName(kMacroDesktopDisplayName);
    if (macroIdx >= 0 && !vda.IsWindowOnDesktopNumber(root, macroIdx)) {
        const int desk = vda.GetWindowDesktopNumber(root);
        if (desk < 0 && IsMacroVisionLatched(root)) {
            // latch 且 desk 未知：跳过狂搬。
        } else {
            EnsureTargetOnMacroDesktop(root, false);
        }
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
        int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
        if (!ApplyImageRegionToMatch(a,
                match.topLeftX, match.topLeftY, match.bottomRightX, match.bottomRightY,
                cx1, cy1, cx2, cy2)) {
            return false;
        }
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
        // aiSearch* 为绝对（客户区）搜索范围；在该范围内找图，用匹配框作为最终截屏区
        if (a.aiSearchX2 > a.aiSearchX1 && a.aiSearchY2 > a.aiSearchY1) {
            probe.searchFullScreen = false;
            probe.searchX1 = a.aiSearchX1;
            probe.searchY1 = a.aiSearchY1;
            probe.searchX2 = a.aiSearchX2;
            probe.searchY2 = a.aiSearchY2;
        } else {
            probe.searchFullScreen = true;
            probe.searchX1 = 0;
            probe.searchY1 = 0;
            probe.searchX2 = 0;
            probe.searchY2 = 0;
        }
        ImageMatchOutput output = FindImageClient(probe, lockedBmp, lockX, lockY);
        if (output.matches.empty()) return false;
        const ImageMatchResult& match = output.matches.front();
        int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
        if (!ApplyImageRegionToMatch(a,
                match.topLeftX, match.topLeftY, match.bottomRightX, match.bottomRightY,
                cx1, cy1, cx2, cy2)) {
            return false;
        }
        return MapClientRect(cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2);
    }

    if (a.aiSearchX2 > a.aiSearchX1 && a.aiSearchY2 > a.aiSearchY1) {
        int x1 = a.aiSearchX1, y1 = a.aiSearchY1, x2 = a.aiSearchX2, y2 = a.aiSearchY2;
        if (session_.Config().windowRelativeCoordinates) {
            int recW = 0, recH = 0, liveW = 0, liveH = 0;
            if (RecordedClientSize(recW, recH) && LiveClientSize(liveW, liveH)) {
                ScaleWindowClientRect(recW, recH, liveW, liveH, x1, y1, x2, y2);
            }
        }
        return MapClientRect(x1, y1, x2, y2, sx1, sy1, sx2, sy2);
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
    if (active_) EndRun();
    if (!config.enabled) {
        active_ = false;
        WindowModeLogEvent(L"[窗口模式] BeginRun：窗口模式未启用，跳过（不会创建宏桌面）");
        return true;
    }

    WindowModeLogEventf(L"[窗口模式] BeginRun：窗口模式启用 kind=%s，开始准备宏桌面/绑窗",
        config.executionKind == WindowModeExecutionKind::HiddenDesktop
            ? L"HiddenDesktop" : L"BackgroundWindow");
    ResetSoftMouseState();
    extLayoutFresh_ = false;
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
    // 模拟器：在进 Session 前先降级，避免后续仍按 HiddenDesktop 分支决策。
    if (runConfig.executionKind == WindowModeExecutionKind::HiddenDesktop
        && ConfigLooksLikeEmulatorTarget(runConfig)) {
        runConfig.executionKind = WindowModeExecutionKind::BackgroundWindow;
        WindowModeLogEvent(
            L"[窗口模式] BeginRun：模拟器目标已改为后台窗口模式（不创建虚拟桌面、不搬窗）");
    }

    if (!session_.Start(runConfig, err)) {
        active_ = false;
        return false;
    }
    // Session::Start 可能再改 executionKind；后续分支以会话为准。
    runConfig = session_.Config();
    WindowModeLogEventf(L"[窗口模式] BeginRun：生效 kind=%s targetExe=%ls",
        runConfig.executionKind == WindowModeExecutionKind::HiddenDesktop
            ? L"HiddenDesktop" : L"BackgroundWindow",
        runConfig.targetExePath.c_str());

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
    // CDP/扩展走本机桥，不依赖 Win32 跨完整性输入；Chrome 渲染进程还常在 AppContainer，
    // 不能用 UAC 提示拦住（用户以管理员运行本工具时尤其容易误报）。
    if (!UsesCdpInput(runConfig) && st.targetPid != 0 && !CheckPermissionMatch(st.targetPid)) {
        WindowModeLogf(L"[窗口模式] UIPI：无法向目标 pid=%lu 发送输入（目标完整性更高）",
            static_cast<unsigned long>(st.targetPid));
        err = HealthToUserHint(WindowModeHealth::PermissionMismatch);
        EndRun();
        return false;
    }

    if (st.health != WindowModeHealth::Ok) {
        WindowModeLogf(L"[窗口模式] 目标未就绪，结束会话：%s",
            st.lastError.empty() ? HealthToUserHint(st.health) : st.lastError.c_str());
        err = HealthToUserHint(st.health);
        if (!st.lastError.empty()) err = st.lastError;
        EndRun();
        return false;
    }

    {
        HWND acTop = TopLevelTargetWindow(session_.State().targetHwnd);
        if (LooksLikeKernelAntiCheatProtectedTarget(runConfig, acTop)) {
            err = KernelAntiCheatBackgroundUnsupportedHint();
            WindowModeLog(
                L"[窗口模式] 内核反作弊目标（英雄联盟/Valorant 等）：拒绝后台窗口/宏桌面，"
                L"不尝试注入（VirtualAllocEx 会被拒绝，PostMessage 游戏不读）");
            EndRun();
            return false;
        }
    }

    hardwareFallback_ = false;
    active_ = true;
    {
        const auto strategy = ResolveInputStrategy(runConfig);
        const wchar_t* name = L"softMessage";
        if (strategy == WindowModeInputStrategy::Cdp) name = L"cdp";
        else if (strategy == WindowModeInputStrategy::Auto) name = L"auto";
        WindowModeLogf(L"[窗口模式] 输入策略=%s class=%s cdpPort=%d",
            name, runConfig.windowClassName.c_str(), runConfig.cdpPort);
    }
    if (UsesCdpInput(runConfig)) {
        // 尽早拉起本机桥，给扩展轮询留时间（扩展约 1.5s 扫一次端口）。
        {
            std::wstring warmErr;
            if (ExtBridgeServer::Instance().Start(warmErr)) {
                WindowModeLog(L"[窗口模式] 本机桥已预热；请保持扩展选项页打开或稍后点「重新连接」");
            }
        }
        std::wstring cdpErr;
        if (!EnsureCdpReady(cdpErr)) {
            err = cdpErr;
            EndRun();
            return false;
        }
        HWND top = TopLevelTargetWindow(session_.State().targetHwnd);
        if (top && IsWindow(top)) {
            if (UsesBackgroundWindow()) {
                EnsureTargetBelowUserWindows(top);
            } else {
                if (!IsMacroVisionCaptureReady(top) && !IsMacroVisionLatched(top)) {
                    PrepareMacroDesktopForCdpBind(top);
                }
                StartCdpMacroDesktopWatchPump(top);
                WindowModeLog(L"[窗口模式] CDP 路径就绪（扩展键鼠/找图；禁 Win32 展开）");
            }
        }
    } else if (LooksLikeChromiumShellTarget(runConfig, session_.State().targetHwnd)) {
        WindowModeLog(L"[窗口模式] Chromium 壳（Electron/CEF）：将优先假焦点 DLL 真后台（注入失败才假前台）");
    } else if (LooksLikeEmulatorTarget(runConfig, session_.State().targetHwnd)) {
        WindowModeLogEvent(
            L"[窗口模式] 桌面模拟器：将注入精简假焦点（仅前景+键态；禁 RawInput/WM_INPUT）");
    } else {
        HWND topHint = TopLevelTargetWindow(session_.State().targetHwnd);
        const bool androidEmu =
            LooksLikeAndroidEmulatorExecutable(runConfig.targetExePath)
            || LooksLikeAndroidEmulatorWindowClass(runConfig.windowClassName)
            || LooksLikeAndroidEmulatorWindowClass(runConfig.childWindowClassName)
            || LooksLikeAndroidEmulatorWindowTitle(runConfig.windowName)
            || LooksLikeQtRenderWindowClass(runConfig.windowClassName)
            || (topHint && IsWindow(topHint)
                && LooksLikeAndroidEmulatorExecutable(QueryHwndProcessImagePath(topHint)));
        if (androidEmu) {
            const bool qtMumu = AndroidEmulatorPrefersFakeFocusFromConfig(runConfig)
                || (topHint && AndroidEmulatorPrefersFakeFocus(topHint, &runConfig));
            WindowModeLog(qtMumu
                ? L"[窗口模式] MuMu 等 Qt 安卓壳：PostMessage 到渲染子窗（OpenGL；绑子窗坐标）"
                : L"[窗口模式] 安卓模拟器：PostMessage 到渲染子窗"
                  L"（LDPlayer 可设 childWindowClassName=TheRender）");
        } else {
            wchar_t clsHint[256]{};
            wchar_t titleHint[512]{};
            if (topHint) {
                GetClassNameW(topHint, clsHint, 256);
                GetWindowTextW(topHint, titleHint, 512);
            }
            const bool adobeAirHint = LooksLikeAdobeAirWindowClass(clsHint)
                || LooksLikeAdobeAirWindowClass(runConfig.windowClassName)
                || LooksLikeAdobeAirWindowClass(runConfig.childWindowClassName);
            const bool game3d = LooksLikeGameWindowClass(clsHint)
                || LooksLikeGameWindowClass(runConfig.windowClassName)
                || LooksLikeGameWindowClass(runConfig.childWindowClassName);
            const bool mapleHint = LooksLikeMapleStoryWindowClass(clsHint)
                || LooksLikeMapleStoryWindowClass(runConfig.windowClassName)
                || LooksLikeMapleStoryTitle(titleHint)
                || LooksLikeMapleStoryTitle(runConfig.windowName)
                || LooksLikeMapleStoryTitle(runConfig.targetWindowTitle)
                || LooksLikeMapleStoryExecutable(runConfig.targetExePath)
                || LooksLikeMapleStoryExecutable(QueryHwndProcessImagePath(topHint));
            WindowModeLog(adobeAirHint
                ? L"[窗口模式] Adobe AIR：将注入假焦点（只骗前景查询；键鼠 PostMessage）"
                : mapleHint
                ? L"[窗口模式] 冒险岛：将注入假焦点（IAT 软光标/按键；禁止 Phase2 CallThroughOriginal/子类化/RawInput）"
                : game3d
                ? L"[窗口模式] 3D/游戏窗口：将注入假焦点（钩光标/RawInput/焦点查询）；失败才 PostMessage"
                : L"[窗口模式] 使用 PostMessage/软消息（非 CDP）");
        }
    }
    loggedClientScale_ = false;
    wasMinimizedAtBeginRun_ = false;
    {
        HWND top = TopLevelTargetWindow(session_.State().targetHwnd);
        wasMinimizedAtBeginRun_ = top && IsWindow(top) && IsIconic(top);
        // 后台窗口模式（含 CDP 扩展找图）：最小化/托盘隐藏时置底还原以便截图与 Chromium 软输入。
        // 宏桌面 CDP 仍跳过 Win32 展开。
        const bool needsQuietGeo = top && IsWindow(top) && TargetNeedsQuietPlaybackRestore(top);
        if (needsQuietGeo && !(IsCdpInputMode() && !UsesBackgroundWindow())) {
            std::wstring geoErr;
            if (!EnsurePlaybackGeometry(geoErr)) {
                err = geoErr.empty() ? L"目标窗口处于最小化/隐藏状态，无法还原后执行" : geoErr;
                EndRun();
                return false;
            }
        }
    }
    TryInstallFakeFocus();
    // 假焦点未注入时的本机 SendInput：RDP / 独占全屏 / Electron 失败 / 游戏 Raw Input。
    rdpSavedForeground_ = nullptr;
    if (PreferHardwareInput()) {
        HWND top = TopLevelTargetWindow(session_.State().targetHwnd);
        wchar_t cls[256]{};
        if (top) GetClassNameW(top, cls, 256);
        const bool rdp = IsRemoteDesktopPlaybackTarget();
        const bool covering = top && IsWindow(top) && LooksLikeMonitorCoveringFullscreen(top);
        const bool electronShell =
            LooksLikeChromiumShellTarget(runConfig, session_.State().targetHwnd);
        WindowModeLogf(L"[窗口模式] 本机输入目标 class=%s hwnd=0x%p rdp=%d covering=%d fallback=%d",
            cls, top, rdp ? 1 : 0, covering ? 1 : 0, hardwareFallback_ ? 1 : 0);
        if (electronShell) {
            WindowModeLog(
                L"[窗口模式] Chromium 壳：假焦点注入失败，回退屏上假前台 SendInput（会占键盘焦点）");
        } else if (rdp && runConfig.executionKind == WindowModeExecutionKind::HiddenDesktop) {
            WindowModeLog(
                L"[窗口模式] 远程桌面：窗口模式(宏桌面)无效，改为用户桌面假前台"
                L"（与后台窗口模式同一输入路径）");
        } else if (covering) {
            WindowModeLog(
                L"[窗口模式] 独占全屏：假前台 SendInput（相对鼠标；无法真后台）");
        } else {
            WindowModeLog(
                L"[窗口模式] 假焦点未注入，回退假前台 SendInput（绝对坐标；会占键鼠）");
        }
        if (top && IsWindow(top) && !covering) {
            auto& vda = VirtualDesktopAccessor::Instance();
            std::wstring vdaErr;
            if (vda.EnsureLoaded(vdaErr)) {
                const int userDesk = vda.GetCurrentDesktopNumber();
                const int macroIdx = vda.FindDesktopIndexByName(kMacroDesktopDisplayName);
                if (userDesk >= 0 && macroIdx >= 0
                    && vda.IsWindowOnDesktopNumber(top, macroIdx)
                    && !vda.IsWindowOnDesktopNumber(top, userDesk)) {
                    vda.MoveWindowToDesktopNumberPreservingView(top, userDesk);
                    WindowModeLog(L"[窗口模式] 已从「鼠标宏」迁回用户桌面（本机输入回退）");
                }
            }
        }
        rdpSavedForeground_ = GetForegroundWindow();
        if (rdpSavedForeground_ == top) rdpSavedForeground_ = nullptr;
        hwOffscreenParked_ = false;
        hwParkedHwnd_ = nullptr;
        hwSavedTopmost_ = false;
        hwSavedWp_ = {};
        if (top && IsWindow(top) && !covering && !electronShell
            && ParkHardwareInputTargetOffscreen(top, &hwSavedWp_, &hwSavedTopmost_)) {
            hwOffscreenParked_ = true;
            hwParkedHwnd_ = top;
            WindowModeLog(
                L"[窗口模式] 本机输入：已屏外+顶置（仍保持前台焦点；"
                L"不能同时操作其它窗口，结束脚本会还原位置）");
        }
        if (!EnsureHardwareInputFocus()) {
            WindowModeLog(L"[窗口模式] 未能切到前台，SendInput 可能打到其它窗");
        } else {
            WindowModeLog(L"[窗口模式] 假前台本机输入已激活");
        }
        POINT pt{};
        HWND seedHwnd = TargetHwnd();
        if (seedHwnd && IsWindow(seedHwnd) && GetCursorPos(&pt)) {
            int scx = 0, scy = 0;
            if (ScreenToClientPoint(seedHwnd, pt.x, pt.y, scx, scy)) {
                RememberSoftMouseClientPos(seedHwnd, scx, scy);
                WindowModeLogf(L"[窗口模式] 本机输入软光标播种 客户区(%d,%d)", scx, scy);
            }
        }
    } else if (UsesChromiumShellInProcInput()) {
        FakeFocusSoftInput_SetPostKeyEvents(true);
        WindowModeLog(
            L"[窗口模式] Chromium 壳真后台就绪（焦点欺骗 + 进程内 PostMessage 队列）");
    }
    DebugLog(L"[WindowMode] Executor::BeginRun OK");
    return true;
}


void WindowModeExecutor::TryInstallFakeFocus() {
    HWND hwnd = TargetHwnd();
    HWND top = TopLevelTargetWindow(hwnd);
    if (!top) top = hwnd;
    const auto& cfg = session_.Config();
    wchar_t cls[256]{};
    if (top) GetClassNameW(top, cls, 256);
    const bool delphiVcl = HwndLooksLikeDelphiVclGame(top)
        || LooksLikeDelphiVclGameWindowClass(cls)
        || LooksLikeDelphiVclGameWindowClass(cfg.windowClassName)
        || LooksLikeDelphiVclGameWindowClass(cfg.childWindowClassName);

    if (IsRemoteDesktopPlaybackTarget()) {
        WindowModeLogEvent(
            L"[窗口模式] 独占全屏/远程桌面：跳过假焦点注入，键鼠走本机输入");
        return;
    }
    // 铺满/远程跳过与 UsesFakeFocusForTarget 同一套例外（传奇 Delphi、冒险岛标题/exe/类名）。
    if (!UsesFakeFocusForTarget(session_.Config(), top)) {
        if (top && IsWindow(top) && LooksLikeMonitorCoveringFullscreen(top) && !delphiVcl) {
            WindowModeLogEvent(
                L"[窗口模式] 独占全屏/远程桌面：跳过假焦点注入，键鼠走本机输入");
        } else {
            WindowModeLogf(L"[窗口模式] class=%s：未注入假焦点，键鼠走 PostMessage", cls);
        }
        return;
    }
    if (LooksLikeKernelAntiCheatProtectedTarget(session_.Config(), top)) {
        WindowModeLog(
            L"[窗口模式] 假焦点跳过：内核反作弊目标禁止注入（会被拒绝访问，且有封号风险）");
        return;
    }
    if (!enableFakeFocusInjection_) {
        if (!UsesBackgroundWindow()
            && GameTargetNeedsHardwareWithoutFakeFocus(session_.Config(), top)) {
            hardwareFallback_ = true;
            WindowModeLog(
                L"[窗口模式] 设置未启用假焦点注入：不注入 DLL，"
                L"游戏/3D 目标改走假前台 SendInput（PostMessage 无法驱动 Raw Input）");
        } else {
            WindowModeLog(
                L"[窗口模式] 设置未启用假焦点注入：不注入 DLL，改走软消息/必要时假前台");
        }
        return;
    }
    if (top && IsFakeFocusInjectionUnsupported(top)) {
        WindowModeLog(IsRemoteDesktopWindow(top)
            ? L"[窗口模式] 假焦点跳过：远程桌面注入会搞挂 mstsc，请保持窗口前台用本机输入"
            : L"[窗口模式] 假焦点跳过：真浏览器请用网页兼容（Electron 壳应已放行）");
        return;
    }
    DWORD pid = session_.State().targetPid;
    if (pid == 0 && top) GetWindowThreadProcessId(top, &pid);
    const bool chromiumShell = LooksLikeChromiumShellTarget(cfg, top);
    const bool androidQt = AndroidEmulatorPrefersFakeFocus(top, &cfg);
    const bool glfwSdl = LooksLikeGlfwOrSdlWindowClass(cls)
        || LooksLikeGlfwOrSdlWindowClass(cfg.windowClassName)
        || LooksLikeGlfwOrSdlWindowClass(cfg.childWindowClassName);
    const bool native3d = LooksLikeGameWindowClass(cls)
        || LooksLikeGameWindowClass(cfg.windowClassName)
        || LooksLikeGameWindowClass(cfg.childWindowClassName);
    const bool desktopEmu = LooksLikeEmulatorTarget(cfg, top)
        && !IsAndroidEmulatorTarget(top, &cfg);
    // SetWindowsHook 在目标 UI 线程 LoadLibrary。GLFW/Java《我的世界》这一下就会崩。
    // DeSmuME：同样禁止在消息线程装 DLL（启动高概率崩）。
    if ((chromiumShell || androidQt || native3d || desktopEmu)
        && fakeFocus_.injection_technique() == inject::Technique::SetWindowsHook) {
        WindowModeLogf(
            L"[窗口模式] %s：注入技术 setwindowshook 改为 classic（避免在游戏线程装 DLL 崩溃）",
            desktopEmu ? L"桌面模拟器"
                : (glfwSdl ? L"GLFW/SDL" : (androidQt ? L"Qt 安卓壳"
                : (chromiumShell ? L"Chromium 壳" : L"3D/游戏窗"))));
        fakeFocus_.SetInjectionTechnique(inject::Technique::ClassicRemoteThread);
    }
    const bool adobeAir = LooksLikeAdobeAirWindowClass(cls)
        || LooksLikeAdobeAirWindowClass(cfg.windowClassName)
        || LooksLikeAdobeAirWindowClass(cfg.childWindowClassName);
    wchar_t mapleTitle[512]{};
    if (top) GetWindowTextW(top, mapleTitle, 512);
    const bool mapleStory = LooksLikeMapleStoryWindowClass(cls)
        || LooksLikeMapleStoryWindowClass(cfg.windowClassName)
        || LooksLikeMapleStoryWindowClass(cfg.childWindowClassName)
        || LooksLikeMapleStoryExecutable(cfg.targetExePath)
        || LooksLikeMapleStoryExecutable(QueryHwndProcessImagePath(top))
        || LooksLikeMapleStoryTitle(mapleTitle)
        || LooksLikeMapleStoryTitle(cfg.windowName)
        || LooksLikeMapleStoryTitle(cfg.targetWindowTitle);
    const bool lite = chromiumShell
        || androidQt
        || desktopEmu
        || delphiVcl
        || glfwSdl
        || adobeAir
        || mapleStory
        || LooksLikeUnrealEngineWindowClass(cls)
        || LooksLikeUnrealEngineWindowClass(cfg.windowClassName)
        || LooksLikeUnrealEngineWindowClass(cfg.childWindowClassName);
    const bool emulator = LooksLikeEmulatorTarget(cfg, top);
    std::wstring ffErr;
    // Chromium 壳 / 原生 3D（GLFW/Unity/SDL）：只注入窗口 PID。
    // Minecraft javaw 的 helper 子进程没有消息泵，先注入它们会把主进程误记成「跳过」。
    if (!fakeFocus_.InjectAndInstall(pid, top, ffErr, lite,
            chromiumShell || native3d || desktopEmu || mapleStory /*windowPidOnly*/)) {
        WindowModeLogf(L"[窗口模式] 假焦点注入失败%s: %s",
            lite ? L"（精简/Chromium壳/Qt安卓）" : L"", ffErr.c_str());
        if (androidQt) {
            WindowModeLog(
                L"[窗口模式] MuMu 假焦点失败：仍仅用 PostMessage，不占用前台；"
                L"请确认模拟器为 OpenGL 渲染");
        } else if (glfwSdl) {
            WindowModeLog(
                L"[窗口模式] GLFW/SDL 假焦点失败：后台无法驱动暂停菜单/视角。"
                L"若安全中心拦截了 FakeFocus64.dll，请到「保护历史记录」允许后"
                L"完全退出游戏再试；并确认 exe 旁有该 DLL（静态 CRT）");
        } else if (native3d && !delphiVcl && !lite) {
            WindowModeLog(
                L"[窗口模式] 3D 假焦点失败：后台只剩 PostMessage，暂停菜单/视角会跟用户真光标走。"
                L"请确认目录含 FakeFocus64.dll");
        } else if (delphiVcl) {
            WindowModeLog(
                L"[窗口模式] 传奇/Delphi 假焦点注入失败：后台仍仅 PostMessage（鼠标会原地点击）。"
                L"请确认目录含 FakeFocus32.dll（32 位客户端）或 FakeFocus64.dll");
        } else if (adobeAir) {
            WindowModeLog(
                L"[窗口模式] Adobe AIR 假焦点失败：后台只剩 PostMessage（造梦等要点不到）。"
                L"请确认目录含 FakeFocus32.dll / FakeFocus64.dll");
        } else if (mapleStory) {
            WindowModeLog(
                L"[窗口模式] 冒险岛假焦点注入失败：后台 PostMessage 游戏不读。"
                L"请确认目录含 FakeFocus32.dll（32 位客户端）或 FakeFocus64.dll，并允许安全中心放行");
        } else if ((lite && !chromiumShell) || emulator) {
            if (!(UsesBackgroundWindow() && emulator)) {
                hardwareFallback_ = true;
            }
            WindowModeLog(emulator
                ? (UsesBackgroundWindow()
                    ? L"[窗口模式] 模拟器假焦点注入失败：后台模式不回退 SendInput（请检查 FakeFocus64.dll）"
                    : L"[窗口模式] 模拟器假焦点失败，回退假前台 SendInput（后台模式亦会占焦点）")
                : L"[窗口模式] UE5 假焦点失败，回退假前台 SendInput");
        }
        return;
    }
    if (chromiumShell) {
        FakeFocusSoftInput_SetPostKeyEvents(true);
        WindowModeLog(
            L"[窗口模式] Chromium 壳假焦点：仅窗口进程、焦点欺骗+灌键鼠线程"
            L"（适用 QQ/微信/Discord/VS Code/CEF 等；tech= 见上）");
    } else if (androidQt) {
        FakeFocusSoftInput_SetPostKeyEvents(true);
        WindowModeLog(
            L"[窗口模式] MuMu/Qt 安卓壳假焦点已注入（进程内灌键鼠 PostMessage；不占前台）");
    } else if (lite && desktopEmu) {
        WindowModeLogEvent(
            L"[窗口模式] 桌面模拟器精简假焦点已注入（仅前景+键态；禁 RawInput/WM_INPUT）");
    } else if (lite && delphiVcl) {
        WindowModeLog(
            L"[窗口模式] 传奇/Delphi 精简假焦点已注入（钩 GetCursorPos；后台不占前台）");
    } else if (lite && glfwSdl) {
        POINT seed{};
        if (GetCursorPos(&seed)) {
            FakeFocusSoftInput_SetCursorScreen(seed.x, seed.y);
        }
        WindowModeLog(
            L"[窗口模式] GLFW/SDL 精简假焦点已注入（GetCursorPos/RawInput，不钩 PeekMessage；"
            L"暂停菜单与视角由软光标驱动；真光标不夹不跟）");
    } else if (lite && adobeAir) {
        WindowModeLog(
            L"[窗口模式] Adobe AIR 假焦点已注入（只骗前景查询；不钩光标/RawInput/不改 WndProc）");
    } else if (lite && mapleStory) {
        HWND seedHwnd = top && IsWindow(top) ? top : TargetHwnd();
        POINT seed{};
        if (seedHwnd && IsWindow(seedHwnd)) {
            RECT rc{};
            GetClientRect(seedHwnd, &rc);
            seed.x = (std::max)(0, static_cast<int>(rc.right - rc.left)) / 2;
            seed.y = (std::max)(0, static_cast<int>(rc.bottom - rc.top)) / 2;
            ClientToScreen(seedHwnd, &seed);
            FakeFocusSoftInput_SetCursorScreen(seed.x, seed.y);
        } else if (GetCursorPos(&seed)) {
            FakeFocusSoftInput_SetCursorScreen(seed.x, seed.y);
        }
        WindowModeLog(
            L"[窗口模式] 冒险岛假焦点已注入（IAT+DirectInput 设备虚表软光标按键；"
            L"不改 user32/win32u 指令、不改 WndProc、不 RawInput；宿主不 PostMessage）");
        g_mapleSoftKeyLogs = 0;
        DWORD packed = 0;
        std::wstring iatErr;
        if (fakeFocus_.QueryMapleIatCount(packed, iatErr)) {
            const DWORD iatSlots = packed & 0xFFFFu;
            const DWORD diag = packed >> 16;
            WindowModeLogf(
                L"[窗口模式] 冒险岛 IAT 已补 slots=%lu diag=0x%04X "
                L"(cursor=%d asyncKey=%d keyState=%d kbState=%d diCreate=%d diState=%d flash=%d "
                L"diAcquire=%d dinput=%d fg=%d setFg=%d liveJmp=%d u32jmp=%d diData=%d "
                L"dataSlot=%d softOk=%d)",
                static_cast<unsigned long>(iatSlots),
                static_cast<unsigned>(diag),
                (diag & 0x0001) ? 1 : 0,
                (diag & 0x0002) ? 1 : 0,
                (diag & 0x0004) ? 1 : 0,
                (diag & 0x0008) ? 1 : 0,
                (diag & 0x0010) ? 1 : 0,
                (diag & 0x0020) ? 1 : 0,
                (diag & 0x0040) ? 1 : 0,
                (diag & 0x0100) ? 1 : 0,
                (diag & 0x0080) ? 1 : 0,
                (diag & 0x0200) ? 1 : 0,
                (diag & 0x0400) ? 1 : 0,
                (diag & 0x0800) ? 1 : 0,
                (diag & 0x1000) ? 1 : 0,
                (diag & 0x2000) ? 1 : 0,
                (diag & 0x4000) ? 1 : 0,
                (diag & 0x8000) ? 1 : 0);
            DWORD gaks = 0, diState = 0, diData = 0, lastCb = 0, hitReady = 0, gfw = 0, focus = 0;
            if (FakeFocusSoftInput_ReadMapleHits(gaks, diState, diData, lastCb, hitReady, gfw, focus)) {
                WindowModeLogf(
                    L"[窗口模式] 冒险岛钩映射 hitReady=%lu gfw=%lu focus=%lu gaks=%lu diState=%lu",
                    static_cast<unsigned long>(hitReady),
                    static_cast<unsigned long>(gfw),
                    static_cast<unsigned long>(focus),
                    static_cast<unsigned long>(gaks),
                    static_cast<unsigned long>(diState));
            }
            if (diag & 0x3000u) {
                WindowModeLog(
                    L"[窗口模式] 冒险岛：当前 FakeFocus32.dll 仍是会闪退的旧版（u32jmp/diData=1）。"
                    L"请结束 MapleStoryt.exe 和本软件，再覆盖安装目录的 FakeFocus32.dll 与 QuickScriptTool.exe");
            }
        } else if (!iatErr.empty()) {
            WindowModeLogf(L"[窗口模式] 冒险岛 IAT 计数读取失败: %s", iatErr.c_str());
        }
    } else if (lite) {
        WindowModeLog(
            L"[窗口模式] UE5 精简假焦点已注入（不钩 PeekMessage；不占用户前台）");
    } else if (emulator) {
        WindowModeLog(
            L"[窗口模式] 桌面模拟器假焦点已注入（GetAsyncKeyState/RawInput；真后台键鼠）");
    } else if (native3d) {
        WindowModeLog(
            L"[窗口模式] 3D/GLFW 假焦点已注入（钩 GetForegroundWindow/GetCursorPos/RawInput；"
            L"暂停菜单与视角不再跟用户真光标）");
    }
}

bool WindowModeExecutor::UsesChromiumShellInProcInput() const {
    if (!fakeFocus_.IsInjected() || !FakeFocusSoftInput_IsAttached()) return false;
    return LooksLikeChromiumShellTarget(session_.Config(), TargetHwnd());
}

bool WindowModeExecutor::UsesMapleStoryFakeFocusInput() const {
    if (!fakeFocus_.IsInjected() || !FakeFocusSoftInput_IsAttached()) return false;
    HWND top = TopLevelTargetWindow(TargetHwnd());
    wchar_t cls[256]{};
    wchar_t title[512]{};
    if (top) {
        GetClassNameW(top, cls, 256);
        GetWindowTextW(top, title, 512);
    }
    const auto& cfg = session_.Config();
    return LooksLikeMapleStoryWindowClass(cls)
        || LooksLikeMapleStoryWindowClass(cfg.windowClassName)
        || LooksLikeMapleStoryWindowClass(cfg.childWindowClassName)
        || LooksLikeMapleStoryTitle(title)
        || LooksLikeMapleStoryTitle(cfg.windowName)
        || LooksLikeMapleStoryTitle(cfg.targetWindowTitle)
        || LooksLikeMapleStoryExecutable(cfg.targetExePath)
        || LooksLikeMapleStoryExecutable(QueryHwndProcessImagePath(top));
}

bool WindowModeExecutor::UsesInProcFakeFocusSoftInput() const {
    return UsesChromiumShellInProcInput() || UsesMapleStoryFakeFocusInput();
}

bool WindowModeExecutor::PreferHardwareInput() const {
    if (UsesCdpInput(session_.Config())) return false;
    const auto& cfg = session_.Config();
    // Chromium 壳：假焦点注入成功 → 真后台进程内 PostMessage；失败才假前台。
    if (LooksLikeChromiumShellTarget(cfg, TargetHwnd())) {
        return !fakeFocus_.IsInjected();
    }
    if (UsesBackgroundWindow()) {
        // 后台窗口模式禁止假前台 SendInput（MuMu 等走假焦点+PostMessage）。
        if (hardwareFallback_ && LooksLikeEmulatorTarget(cfg, TargetHwnd())) return true;
        return false;
    }
    if (hardwareFallback_) return true;
    if (LooksLikeRemoteDesktopWindowClass(cfg.windowClassName)
        || LooksLikeRemoteDesktopWindowClass(cfg.childWindowClassName)
        || LooksLikeRemoteDesktopExePath(cfg.targetExePath)) {
        return true;
    }
    HWND top = TopLevelTargetWindow(TargetHwnd());
    if (top && IsRemoteDesktopWindow(top)) return true;
    if (top && IsWindow(top) && LooksLikeMonitorCoveringFullscreen(top)) return true;
    // 窗口化 UE5/Unity：未注入时 PostMessage 无效；勿等铺满才改走 SendInput。
    if (!fakeFocus_.IsInjected()
        && GameTargetNeedsHardwareWithoutFakeFocus(cfg, top)) {
        return true;
    }
    return false;
}

bool WindowModeExecutor::IsRemoteDesktopPlaybackTarget() const {
    const auto& cfg = session_.Config();
    if (LooksLikeRemoteDesktopExePath(cfg.targetExePath)
        || LooksLikeRemoteDesktopWindowClass(cfg.windowClassName)
        || LooksLikeRemoteDesktopWindowClass(cfg.childWindowClassName)) {
        return true;
    }
    HWND top = TopLevelTargetWindow(TargetHwnd());
    return top && IsRemoteDesktopWindow(top);
}

bool WindowModeExecutor::UsesRelativeHardwareCursor() const {
    // 远程桌面必须绝对坐标。窗口化 UE5（国王大道）也必须绝对 SetCursorPos，
    // 否则点击落在系统光标处；仅真铺满监视器才改相对位移（避免拆 DXGI）。
    if (IsRemoteDesktopPlaybackTarget()) return false;
    if (LooksLikeChromiumShellTarget(session_.Config(), TargetHwnd())) return false;
    HWND top = TopLevelTargetWindow(TargetHwnd());
    return top && IsWindow(top) && LooksLikeMonitorCoveringFullscreen(top);
}

void WindowModeExecutor::RestoreHardwareOffscreenPark() {
    if (!hwOffscreenParked_) return;
    HWND hwnd = hwParkedHwnd_;
    if (hwnd && IsWindow(hwnd)) {
        RestoreHardwareInputTargetOffscreen(hwnd, hwSavedWp_, hwSavedTopmost_);
        WindowModeLog(L"[窗口模式] 本机输入：已把屏外目标还原到原位置");
    }
    hwOffscreenParked_ = false;
    hwParkedHwnd_ = nullptr;
    hwSavedTopmost_ = false;
    hwSavedWp_ = {};
}

bool WindowModeExecutor::EnsureHardwareInputFocus() {
    if (!PreferHardwareInput()) return true;

    HWND top = TopLevelTargetWindow(TargetHwnd());
    if (!top || !IsWindow(top)) return false;

    HWND fg = GetForegroundWindow();
    if (fg == top) return true;
    if (fg && IsChild(top, fg)) return true;

    std::wstring err;
    if (!ActivateWindow(top, err)) {
        WindowModeLogf(L"[窗口模式] 本机输入假前台激活失败: %s", err.c_str());
        return false;
    }
    return true;
}

bool WindowModeExecutor::SendHardwareCursorToClient(int cx, int cy) {
    const bool chromiumShell = LooksLikeChromiumShellTarget(session_.Config(), TargetHwnd());
    if (fakeFocus_.IsInjected() && !chromiumShell) {
        fakeFocus_.Unload();
        WindowModeLogEvent(L"[窗口模式] 全屏游戏：已卸载假焦点（避免 PeekMessage 冻 DXGI）");
    }
    EnsureHardwareInputFocus();
    HWND hwnd = TargetHwnd();

    // 脚本约定 (0,0)=当前位置。绝对 SetCursorPos 到客户区原点 = 飞到窗口左上角。
    if (cx == 0 && cy == 0) {
        int lx = 0, ly = 0;
        POINT pt{};
        bool resolved = false;
        if (hwnd && IsWindow(hwnd) && GetCursorPos(&pt)
            && ScreenToClientPoint(hwnd, pt.x, pt.y, lx, ly)
            && (lx != 0 || ly != 0)) {
            cx = lx;
            cy = ly;
            resolved = true;
        } else if (GetLastSoftMouseClientPos(hwnd, lx, ly) && (lx != 0 || ly != 0)) {
            cx = lx;
            cy = ly;
            resolved = true;
        }
        if (!resolved) {
            WindowModeLogEvent(
                L"[窗口模式] 跳过绝对光标到客户区(0,0)（保持系统光标当前位置）");
            return true;
        }
    }

    // 调用约定：cx/cy 已是目标客户区（MoveMouseClient / MapScriptPointToClient 之后）。
    // 旧逻辑用 UsesClientCoords()（看 coordSpace）门控，窗口相对脚本常被标成 screenAbsolute，
    // 客户区小数被当成屏幕坐标 → SetCursorPos 飞到屏幕左上角。
    if (!UsesRelativeHardwareCursor()) {
        int sx = cx;
        int sy = cy;
        if (hwnd && IsWindow(hwnd)) {
            if (!ClientToScreenPoint(hwnd, cx, cy, sx, sy)) {
                WindowModeLogf(L"[窗口模式] ClientToScreen 失败 hwnd=0x%p client=(%d,%d)",
                    hwnd, cx, cy);
                return false;
            }
        }
        RememberSoftMouseClientPos(hwnd, cx, cy);
        WindowModeLogEventf(L"[窗口模式] 本机绝对光标 客户区(%d,%d) → 屏幕(%d,%d)",
            cx, cy, sx, sy);
        return SetCursorScreenPos(sx, sy);
    }

    // UE5 独占全屏：绝对客户区 → 相对 SendInput；不碰 SetCursorPos。
    int prevX = 0;
    int prevY = 0;
    bool havePrev = GetLastSoftMouseClientPos(hwnd, prevX, prevY);
    if (!havePrev) {
        havePrev = GetCursorClientPos(prevX, prevY);
    }
    RememberSoftMouseClientPos(hwnd, cx, cy);
    if (!havePrev) return true;
    const int dx = cx - prevX;
    const int dy = cy - prevY;
    if (dx == 0 && dy == 0) return true;
    SendMouseMoveRelative(dx, dy);
    return true;
}

void WindowModeExecutor::MoveMouseRelativeClient(int dx, int dy) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;
    if (dx == 0 && dy == 0) return;

    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) return;
        int cx = 0, cy = 0;
        if (!GetLastSoftMouseClientPos(TargetHwnd(), cx, cy)) {
            GetCursorClientPos(cx, cy);
        }
        MoveMouseClient(cx + dx, cy + dy, 0, 0, [](int) { return 0; }, false);
        return;
    }
    if (PreferHardwareInput()) {
        if (!PrepareSoftInput(err)) return;
        if (fakeFocus_.IsInjected()) fakeFocus_.Unload();
        EnsureHardwareInputFocus();
        int cx = 0, cy = 0;
        if (GetLastSoftMouseClientPos(TargetHwnd(), cx, cy)) {
            RememberSoftMouseClientPos(TargetHwnd(), cx + dx, cy + dy);
            if (!UsesRelativeHardwareCursor()) {
                SendHardwareCursorToClient(cx + dx, cy + dy);
                return;
            }
        }
        SendMouseMoveRelative(dx, dy);
        return;
    }
    if (!PrepareSoftInput(err)) return;
    int cx = 0, cy = 0;
    if (!GetLastSoftMouseClientPos(TargetHwnd(), cx, cy)) {
        GetCursorClientPos(cx, cy);
    }
    cx += dx;
    cy += dy;
    SyncFakeFocusCursor(cx, cy);
    RememberSoftMouseClientPos(TargetHwnd(), cx, cy);
    if (!UsesInProcFakeFocusSoftInput()) {
        HWND top = TopLevelTargetWindow(TargetHwnd());
        // 与绝对移动一致：DeSmuME 纯相对轨迹不洪泛 MOVE；按住拖拽才投递（且底层有节流）。
        const bool desktopEmu = IsDesktopEmulatorTarget(top, &session_.Config());
        if (!desktopEmu || SoftMouseButtonHeld()) {
            PostMouseMoveToWindow(TargetHwnd(), cx, cy);
        }
    }
}

void WindowModeExecutor::SyncFakeFocusCursor(int cx, int cy) const {
    if (!FakeFocusSoftInput_IsAttached()) return;
    HWND hwnd = TargetHwnd();
    int sx = cx;
    int sy = cy;
    if (hwnd && IsWindow(hwnd)) {
        ClientToScreenPoint(hwnd, cx, cy, sx, sy);
    }
    FakeFocusSoftInput_SetCursorScreen(sx, sy);
}

void WindowModeExecutor::SyncFakeFocusMouseButton(MouseButtonType button, bool down) const {
    if (!FakeFocusSoftInput_IsAttached()) return;
    FakeFocusSoftInput_SetMouseButtonVk(VkFromMouseButton(button), down);
}

void WindowModeExecutor::NotifyCancel() {
    ExtBridgeServer::Instance().AbortPending();
    SignalStopCdpMacroDesktopWatchPump();
}

void WindowModeExecutor::EndRun() {
    RestoreHardwareOffscreenPark();
    hardwareFallback_ = false;
    HWND top = TopLevelTargetWindow(session_.State().targetHwnd);
    const bool fsGame = top && IsWindow(top) && LooksLikeFullscreenGameTarget(top);
    const bool rdp = IsRemoteDesktopPlaybackTarget();
    if (!fsGame && !rdp && wasMinimizedAtBeginRun_ && top && IsWindow(top)
        && !UsesCdpInput(session_.Config())) {
        HWND preserveFg = GetForegroundWindow();
        ShowWindow(top, SW_SHOWMINNOACTIVE);
        if (preserveFg && IsWindow(preserveFg) && preserveFg != top
            && GetForegroundWindow() == top) {
            SetForegroundWindow(preserveFg);
        }
        WindowModeLog(L"[窗口模式] 会话结束: 已把目标窗口恢复为最小化");
    }
    wasMinimizedAtBeginRun_ = false;
    loggedClientScale_ = false;
    fakeFocus_.Unload();
    StopCdpMacroDesktopWatchPump();
    top = TopLevelTargetWindow(session_.State().targetHwnd);
    if (!fsGame && !rdp && top && IsWindow(top) && !UsesBackgroundWindow()) {
        if (UsesCdpInput(session_.Config())) {
            RestoreMacroDesktopWindowAfterRun(top);
        } else if (session_.Config().executionKind
                == WindowModeExecutionKind::HiddenDesktop) {
            // softMessage 窗口模式：把目标窗口从「鼠标宏」桌面移回用户当前桌面，
            // 避免回放结束后窗口留在宏桌面不可见（“窗口消失/没反应”）。
            auto& vda = VirtualDesktopAccessor::Instance();
            std::wstring err;
            if (vda.EnsureLoaded(err)) {
                const int userDesk = vda.GetCurrentDesktopNumber();
                const int macroIdx = vda.FindDesktopIndexByName(kMacroDesktopDisplayName);
                if (userDesk >= 0 && userDesk != macroIdx
                    && !vda.IsWindowOnDesktopNumber(top, userDesk)) {
                    vda.MoveWindowToDesktopNumberPreservingView(top, userDesk);
                    WindowModeLogEvent(L"[窗口模式] 会话结束: 目标已移回用户桌面");
                }
            }
        }
    }
    if (rdpSavedForeground_ && IsWindow(rdpSavedForeground_)) {
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow(rdpSavedForeground_);
        WindowModeLog(L"[窗口模式] 本机输入：已还原回放前的前台窗口");
    }
    rdpSavedForeground_ = nullptr;
    ExtBridgeServer::Instance().ClearAbort();
    cdp_.Disconnect();
    ext_.Disconnect();
    session_.Stop();
    active_ = false;
    cancelFlag_ = nullptr;
    extLayoutFresh_ = false;
    ResetSoftMouseState();
    WindowModeLogEvent(L"[窗口模式] EndRun：窗口模式会话结束（「鼠标宏」桌面保留，不会自动删除）");
}

void WindowModeExecutor::UpdateExtSurfaceSize() {
    if (!ext_.IsConnected()) return;
    if (IsCdpInputMode()) {
        const auto& st = session_.State();
        int sw = 0, sh = 0;
        if (ResolveExtVisionSurface(ext_, TargetHwnd(), st.clientW, st.clientH, sw, sh)) {
            static int sLastSw = 0, sLastSh = 0;
            if (sw != sLastSw || sh != sLastSh) {
                WindowModeLogf(L"[窗口模式] 扩展鼠标表面 %dx%d", sw, sh);
                sLastSw = sw;
                sLastSh = sh;
            }
        }
        if (sw >= 64 && sh >= 64) {
            ext_.SetSurfaceSize(sw, sh);
        }
        return;
    }
    HWND cap = VisionCaptureHwnd();
    if (!cap || !IsWindow(cap)) cap = TargetHwnd();
    RECT rc{};
    if (!cap || !GetClientRect(cap, &rc)) return;
    const int w = std::max(0, static_cast<int>(rc.right - rc.left));
    const int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
    if (w > 0 && h > 0) ext_.SetSurfaceSize(w, h);
}

void WindowModeExecutor::ToCaptureSurfaceClient(int& cx, int& cy) const {
    if (IsCdpInputMode()) return;
    HWND input = TargetHwnd();
    HWND cap = VisionCaptureHwnd();
    if (!input || !cap || !IsWindow(input) || !IsWindow(cap) || input == cap) return;
    const int inX = cx;
    const int inY = cy;

    RECT inRc{}, capRc{};
    GetClientRect(input, &inRc);
    GetClientRect(cap, &capRc);
    int inW = std::max(0, static_cast<int>(inRc.right - inRc.left));
    int inH = std::max(0, static_cast<int>(inRc.bottom - inRc.top));
    int capW = std::max(0, static_cast<int>(capRc.right - capRc.left));
    int capH = std::max(0, static_cast<int>(capRc.bottom - capRc.top));
    // 还原后偶发仍读到 215×28 缩略客户区，按会话纠正尺寸缩放，避免键鼠打偏。
    if (inW < 400 || inH < 300) {
        int rw = session_.State().clientW;
        int rh = session_.State().clientH;
        if (rw < 400 || rh < 300) {
            FillClientSizeFromHwnd(TopLevelTargetWindow(input), rw, rh);
        }
        if (rw >= 400 && rh >= 300 && inW > 0 && inH > 0) {
            cx = static_cast<int>(std::lround(cx * (static_cast<double>(rw) / inW)));
            cy = static_cast<int>(std::lround(cy * (static_cast<double>(rh) / inH)));
            inW = rw;
            inH = rh;
        }
    }
    if (capW < 400 || capH < 300) {
        int rw = 0, rh = 0;
        FillClientSizeFromHwnd(TopLevelTargetWindow(cap), rw, rh);
        if (rw >= 400 && rh >= 300) {
            capW = rw;
            capH = rh;
        }
    }

    MapMatchPointBetweenWindows(input, cap, cx, cy);
    if (cx != inX || cy != inY) {
        WindowModeLogf(
            L"[窗口模式] 网页坐标对齐 input=(%d,%d)@%dx%d -> surface=(%d,%d)@%dx%d",
            inX, inY, inW, inH, cx, cy, capW, capH);
    }
}

void WindowModeExecutor::MaybeRefreshExtLayout() {
    if (extLayoutFresh_ || !UsesCdpInput(session_.Config()) || !ext_.IsConnected()) return;
    if (WindowModeCancelled(cancelFlag_)) return;
    // 最小化也可向扩展要壳页 iframe 布局；surface 禁用 215×28 缩略尺寸。
    const auto& st = session_.State();
    int sw = 0, sh = 0;
    ResolveExtVisionSurface(ext_, TargetHwnd(), st.clientW, st.clientH, sw, sh);
    if (sw >= 64 && sh >= 64) {
        ext_.SetSurfaceSize(sw, sh);
    }

    // attach 已带 iframe 矩形时禁止再打 layout：旧扩展 soft-swap 会弄死 MV3 桥。
    if (ext_.HasValidIframeLayout()) {
        extLayoutFresh_ = true;
        int ix = 0, iy = 0, iw = 0, ih = 0, pw = 0, ph = 0;
        ext_.GetIframeCssRect(ix, iy, iw, ih);
        ext_.GetPageCssSize(pw, ph);
        WindowModeLogf(
            L"[窗口模式] 沿用 attach 布局 iframeCss=(%d,%d) %dx%d pageCss=%dx%d surface=%dx%d（跳过 layout 防断桥）",
            ix, iy, iw, ih, pw, ph, ext_.SurfaceW(), ext_.SurfaceH());
        return;
    }
    if (!ext_.SupportsStableBridgeApi()) {
        extLayoutFresh_ = true;
        WindowModeLog(L"[窗口模式] 扩展 <1.1.12：跳过 layout 刷新（旧版会断桥）。请重载到 v1.1.12+");
        return;
    }

    std::wstring err;
    if (ext_.RefreshLayout(err)) {
        extLayoutFresh_ = true;
        WindowModeLog(L"[窗口模式] 已刷新扩展坐标布局（未展开窗口）");
    } else if (!err.empty()) {
        WindowModeLogVerbosef(L"[窗口模式] 扩展布局刷新跳过: %s", err.c_str());
    }
}

HWND WindowModeExecutor::TargetHwnd() const {
    return session_.State().targetHwnd;
}

bool WindowModeExecutor::TargetStillAlive() const {
    if (!active_) return false;
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) return false;
    const DWORD expectPid = session_.State().targetPid;
    DWORD livePid = 0;
    GetWindowThreadProcessId(hwnd, &livePid);
    if (expectPid != 0 && livePid != 0 && livePid != expectPid) return false;
    if (expectPid == 0) return true;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, expectPid);
    if (!process) {
        // 窗口还在但打不开进程：多半权限问题，不按闪退处理。
        return livePid == expectPid || livePid == 0;
    }
    DWORD exitCode = STILL_ACTIVE;
    const BOOL ok = GetExitCodeProcess(process, &exitCode);
    CloseHandle(process);
    return ok && exitCode == STILL_ACTIVE;
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
    if (PreferHardwareInput()) {
        int cx = sx;
        int cy = sy;
        HWND hwnd = TargetHwnd();
        if (hwnd && IsWindow(hwnd) && UsesClientCoords()) {
            if (!ScreenToClientPoint(hwnd, sx, sy, cx, cy)) return false;
        }
        return SendHardwareCursorToClient(cx, cy);
    }
    return SetCursorPos(sx, sy) == TRUE;
}

bool WindowModeExecutor::PrepareSoftInput(std::wstring& err) {
    // Message-based input must NOT expand / raise the target.
    // Vision capture (find-image/OCR) uses ScopedVisionCapturePrep for quiet bottom restore.
    // 浏览器子控件可能晚于顶层出现：每次输入前刷新绑定到 RenderWidget。
    if (!session_.RefreshInputBinding(err)) {
        HWND hwnd = TargetHwnd();
        if (!hwnd || !IsWindow(hwnd)) {
            return RefreshTarget(err);
        }
        err.clear();
    }
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        return RefreshTarget(err);
    }
    if (!EnsurePlaybackGeometry(err)) return false;
    if (!EnsureTargetBound(err)) return false;
    // 本机 SendInput：每次键鼠前确认仍在前台，否则会打到本软件并响提示音。
    if (PreferHardwareInput()) {
        EnsureHardwareInputFocus();
    }
    return true;
}

bool WindowModeExecutor::EnsureCdpReady(std::wstring& err) {
    if (WindowModeCancelled(cancelFlag_)) {
        err = L"已取消";
        return false;
    }
    if (cdp_.IsConnected() || ext_.IsConnected()) return true;

    const auto& cfg = session_.Config();
    const std::wstring hint = !cfg.windowName.empty() ? cfg.windowName : cfg.targetWindowTitle;
    HWND top = TopLevelTargetWindow(TargetHwnd());

    // 主路径：配套扩展。勿先扫 9222~9230——WinHttp 连不上时会卡数十秒，热键 Abort 也打不进。
    WindowModeLog(L"[窗口模式] 网页兼容：经配套扩展桥投递键鼠（不重启）");
    std::wstring attachHint = hint;
    if (attachHint.empty() && top && IsWindow(top)) {
        wchar_t title[512]{};
        GetWindowTextW(top, title, 512);
        attachHint = title;
    }

    std::wstring extErr;
    if (ext_.EnsureReady(attachHint, top, extErr)) {
        err.clear();
        return true;
    }
    if (WindowModeCancelled(cancelFlag_) || extErr == L"已取消"
        || ExtBridgeServer::Instance().IsAborted()) {
        err = L"已取消";
        return false;
    }

    // 回退：用户已自行开启 remote-debugging-port 时再试直连（短超时）。
    if (top && IsWindow(top) && cdp_.ConnectForWindow(top, cfg.cdpPort, hint, err)) {
        return true;
    }
    if (WindowModeCancelled(cancelFlag_)) {
        err = L"已取消";
        return false;
    }

    OpenExtensionInstallGuide();
    err = L"未能连接配套浏览器扩展。请加载 extension\\edge 后重试。"
          L"（若已手动开启 --remote-debugging-port 也可直接使用）";
    if (!extErr.empty() && extErr != L"NO_EXTENSION") {
        err += L"\n";
        err += extErr;
    }
    WindowModeLogf(L"[窗口模式] %s", err.c_str());
    return false;
}


bool WindowModeExecutor::RecordedClientSize(int& w, int& h) const {
    w = session_.Config().recordClientWidth;
    h = session_.Config().recordClientHeight;
    if (w > 0 && h > 0) return true;
    if (session_.Config().windowRelativeCoordinates
        && coordMeta_.captureWidth > 0 && coordMeta_.captureHeight > 0) {
        w = coordMeta_.captureWidth;
        h = coordMeta_.captureHeight;
        return true;
    }
    w = 0;
    h = 0;
    return false;
}

bool WindowModeExecutor::LiveClientSize(int& w, int& h) const {
    w = 0;
    h = 0;
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) hwnd = VisionCaptureHwnd();
    HWND top = TopLevelTargetWindow(hwnd);
    // MuMu：缩放按渲染子窗（游戏区），勿用含顶栏的顶层客户区。
    if (top && hwnd == top && IsAndroidEmulatorTarget(top, &session_.Config())) {
        if (HWND render = FindBackgroundInputChild(top, &session_.Config())) {
            if (render != top) hwnd = render;
        }
    }
    RECT rc{};
    if (hwnd && IsWindow(hwnd) && GetClientRect(hwnd, &rc)) {
        w = std::max(0, static_cast<int>(rc.right - rc.left));
        h = std::max(0, static_cast<int>(rc.bottom - rc.top));
    }
    if (w <= 0 || h <= 0) {
        w = session_.State().clientW;
        h = session_.State().clientH;
    }
    return w > 0 && h > 0;
}

TemplateScale WindowModeExecutor::FindImageSurfaceScale(int surfaceW, int surfaceH) const {
    TemplateScale ts{};
    if (session_.Config().windowRelativeCoordinates) {
        int recW = 0, recH = 0;
        if (RecordedClientSize(recW, recH) && recW > 0 && recH > 0
            && surfaceW > 0 && surfaceH > 0) {
            CoordMeta clientMeta = coordMeta_;
            clientMeta.captureWidth = recW;
            clientMeta.captureHeight = recH;
            return ComputeTemplateScale(clientMeta, surfaceW, surfaceH);
        }
        return ts;
    }
    if (coordMeta_.captureWidth > 0 || coordMeta_.refWidth > 0) {
        int curX = 0, curY = 0, curW = 0, curH = 0;
        GetVirtualScreenBounds(curX, curY, curW, curH);
        return ComputeTemplateScale(coordMeta_, curW, curH);
    }
    return ts;
}

TemplateScale WindowModeExecutor::FindImageTemplateScale() const {
    int liveW = 0, liveH = 0;
    if (!LiveClientSize(liveW, liveH)) return {};
    return FindImageSurfaceScale(liveW, liveH);
}

bool WindowModeExecutor::EnsurePlaybackGeometry(std::wstring& err) {
    // 宏桌面 CDP：窗口停放在宏桌面，找图走扩展截图，禁止 Win32 展开抢前台。
    // 后台窗口模式（含浏览器扩展找图）最小化时仍置底还原，让页面/客户区能出图。
    if (IsCdpInputMode() && !UsesBackgroundWindow()) {
        err.clear();
        return true;
    }
    if (PreferHardwareInput()) {
        HWND root = TopLevelTargetWindow(TargetHwnd());
        if (root && IsWindow(root) && IsIconic(root)) {
            // 远程桌面：最小化时本机 SendInput 进不了会话；还原+假前台是唯一可行路径。
            if (IsRemoteDesktopWindow(root)) {
                WindowModeLog(L"[窗口模式] 远程桌面已最小化：还原并假前台（无法在最小化态操控远程会话）");
                ShowWindow(root, SW_RESTORE);
                EnsureHardwareInputFocus();
                if (IsIconic(root)) {
                    err = L"远程桌面仍处于最小化，无法回放（请先还原 mstsc 窗口）";
                    WindowModeLogf(L"[窗口模式] %s", err.c_str());
                    return false;
                }
                err.clear();
                return true;
            }
            err = L"全屏游戏处于最小化，无法回放";
            return false;
        }
        err.clear();
        return true;
    }
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        if (!RefreshTarget(err)) return false;
        hwnd = TargetHwnd();
    }
    HWND root = TopLevelTargetWindow(hwnd);
    if (!root || !IsWindow(root)) {
        return RefreshTarget(err);
    }

    // Chromium/Electron/CEF：最小化或隐藏后 GPU 合成停摆。
    // Win32 安静 ShowWindow/置底还原只会唤出空白壳（bestNcc≈0，已证伪），禁止再走。
    // 真浏览器走扩展截图；QQ/微信等壳须保持「已还原且可被遮挡」，勿最小化。
    if (UsesChromiumShellInProcInput()
        || LooksLikeChromiumShellTarget(session_.Config(), root)) {
        if (TargetNeedsQuietPlaybackRestore(root)
            || TargetNeedsQuietPlaybackRestore(hwnd)) {
            WindowModeLog(
                L"[窗口模式] Chromium 壳已最小化/隐藏：禁止 Win32 安静还原（只会得到空白窗）");
            WindowModeLog(
                L"[窗口模式] 请先还原目标窗口，再保持后台（可被其它窗完全挡住），勿点最小化");
            err = L"Chromium 壳最小化后无法截图与可靠输入，请还原窗口（可被遮挡）";
            return false;
        }
        err.clear();
        return true;
    }

    if (!TargetNeedsQuietPlaybackRestore(root)
        && !TargetNeedsQuietPlaybackRestore(hwnd)) {
        err.clear();
        return true;
    }
    HWND preserveFg = GetForegroundWindow();
    const bool iconic = IsIconic(root) != FALSE;
    const bool hidden = root && IsWindow(root) && !IsWindowVisible(root);
    WindowModeLogf(
        L"[窗口模式] 目标需安静还原以便回放（不抢前台） iconic=%d visible=%d",
        iconic ? 1 : 0, hidden ? 0 : 1);
    if (!RestoreOnUserDesktopBottom(root, preserveFg)) {
        RestoreWindowNonActivating(root);
        if (root && IsWindow(root) && !IsWindowVisible(root)) {
            ShowWindow(root, SW_SHOWNOACTIVATE);
            EnsureTargetBelowUserWindows(root);
        }
    }
    if (TargetNeedsQuietPlaybackRestore(root)) {
        err = L"目标窗口仍处于最小化/隐藏状态，无法执行";
        WindowModeLogf(L"[窗口模式] %s iconic=%d visible=%d",
            err.c_str(),
            IsIconic(root) ? 1 : 0,
            (root && IsWindowVisible(root)) ? 1 : 0);
        return false;
    }
    std::wstring bindErr;
    session_.RefreshInputBinding(bindErr);
    err.clear();
    return true;
}

void WindowModeExecutor::MoveMouseClient(int cx, int cy, int randomX, int randomY,
    const std::function<int(int)>& randomInt, bool scaleRecordedClient) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;
    if (!UsesCdpInput(session_.Config())) {
        std::wstring prepErr;
        if (!PrepareSoftInput(prepErr)) {
            DebugLog(L"[WindowMode] MoveMouseClient: target not ready");
            return;
        }
    }
    if (!MapScriptPointToClient(cx, cy, scaleRecordedClient)) return;
    // 与点击一致：(0,0)=当前位置；硬件绝对光标路径必须先解析，否则飞到左上角。
    ResolveClickClientPos(cx, cy);
    if (scaleRecordedClient && session_.Config().windowRelativeCoordinates
        && (randomX != 0 || randomY != 0)) {
        int recW = 0, recH = 0, liveW = 0, liveH = 0;
        if (RecordedClientSize(recW, recH) && LiveClientSize(liveW, liveH)) {
            ScaleWindowClientPoint(recW, recH, liveW, liveH, randomX, randomY);
        }
    }
    WindowModeLogf(L"[窗口模式] 移动 → 客户区(%d,%d) ±(%d,%d)", cx, cy, randomX, randomY);

    const int rx = randomInt(randomX);
    const int ry = randomInt(randomY);
    const int tx = cx + rx;
    const int ty = cy + ry;

    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) {
            DebugLog(L"[WindowMode] MoveMouseClient: CDP not ready");
            return;
        }
        if (UserOnMacroDesktopNow()) {
            RaiseMacroDesktopWindowForWatch(TargetHwnd());
        }
        MaybeRefreshExtLayout();
        UpdateExtSurfaceSize();
        RememberSoftMouseClientPos(TargetHwnd(), tx, ty);
        int sx = tx;
        int sy = ty;
        ToCaptureSurfaceClient(sx, sy);
        const bool ok = ext_.IsConnected() ? ext_.MouseMove(sx, sy, err)
                                           : cdp_.MouseMove(sx, sy, err);
        if (!ok) {
            WindowModeLogf(L"[窗口模式] 网页键鼠移动失败: %s", err.c_str());
        }
        return;
    }
    if (!PrepareSoftInput(err)) {
        DebugLog(L"[WindowMode] MoveMouseClient: target not ready");
        return;
    }
    if (PreferHardwareInput()) {
        SendHardwareCursorToClient(tx, ty);
        return;
    }
    SyncFakeFocusCursor(tx, ty);
    RememberSoftMouseClientPos(TargetHwnd(), tx, ty);
    if (!UsesInProcFakeFocusSoftInput()) {
        HWND top = TopLevelTargetWindow(TargetHwnd());
        // DeSmuME 纯移动会触发 ShowCursor 洪泛崩溃；按住键拖拽时仍须发 MOVE。
        const bool desktopEmu = IsDesktopEmulatorTarget(top, &session_.Config());
        if (!desktopEmu || SoftMouseButtonHeld()) {
            PostMouseMoveToWindow(TargetHwnd(), tx, ty);
        }
    }
}

void WindowModeExecutor::ResolveClickClientPos(int& cx, int& cy) const {
    // 脚本里鼠标点击 x=y=0 表示「当前位置」。窗口/网页键鼠不移动系统光标，
    // 必须落到上次软光标，否则会点到客户区 (0,0)。
    // 软光标若已是 (0,0) 视为未初始化（曾被错误 Remember），改用系统光标。
    if (cx != 0 || cy != 0) return;
    int lx = 0, ly = 0;
    if (GetLastSoftMouseClientPos(TargetHwnd(), lx, ly) && (lx != 0 || ly != 0)) {
        cx = lx;
        cy = ly;
        return;
    }
    if (GetCursorClientPos(lx, ly)) {
        cx = lx;
        cy = ly;
    }
}

bool WindowModeExecutor::MapScriptPointToClient(int& cx, int& cy, bool scaleRecordedClient) const {
    if (!active_) return false;
    // 窗口相对录制脚本：坐标已是目标窗口客户区。
    // （coordSpace 历史值不可靠——旧脚本被误标 windowClient 但存的是屏幕坐标。）
    if (session_.Config().windowRelativeCoordinates) {
        if (cx == 0 && cy == 0) return true;
        if (scaleRecordedClient) {
            int recW = 0, recH = 0, liveW = 0, liveH = 0;
            if (RecordedClientSize(recW, recH) && LiveClientSize(liveW, liveH)) {
                const int inX = cx;
                const int inY = cy;
                if (ScaleWindowClientPoint(recW, recH, liveW, liveH, cx, cy)
                    && !loggedClientScale_) {
                    WindowModeLogEventf(
                        L"[窗口模式] 客户区缩放 录制%dx%d → 当前%dx%d (%d,%d)→(%d,%d)",
                        recW, recH, liveW, liveH, inX, inY, cx, cy);
                    loggedClientScale_ = true;
                }
            }
        }
        return true;
    }
    // (0,0) = 「当前位置」（点击沿用上次软光标），保持原语义。
    if (cx == 0 && cy == 0) return true;

    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;

    // scaleRecordedClient=false：找图/OCR 等运行时落点，已是客户区坐标。
    // 不可再当屏幕坐标做 ScreenToClient，否则会误判「客户区外」并取消点击。
    if (!scaleRecordedClient) {
        if (cx < 0 || cy < 0 || cx >= client.right || cy >= client.bottom) {
            WindowModeLogEventf(L"[窗口模式] 目标点客户区(%d,%d) 超出目标窗口(%dx%d)，已取消",
                cx, cy, static_cast<int>(client.right), static_cast<int>(client.bottom));
            return false;
        }
        return true;
    }

    const int screenX = cx;
    const int screenY = cy;
    if (!ScreenToClientPoint(hwnd, screenX, screenY, cx, cy)) return false;
    if (cx < 0 || cy < 0 || cx >= client.right || cy >= client.bottom) {
        WindowModeLogEventf(L"[窗口模式] 目标点屏幕(%d,%d) 在目标窗口客户区外(%dx%d)，已取消",
            screenX, screenY, static_cast<int>(client.right), static_cast<int>(client.bottom));
        return false;
    }
    return true;
}

void WindowModeExecutor::PostMouseButtonAtClient(int cx, int cy, MouseButtonType button, bool down,
    bool scaleRecordedClient) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;
    if (!UsesCdpInput(session_.Config())) {
        std::wstring prepErr;
        if (!PrepareSoftInput(prepErr)) return;
    }
    if (!MapScriptPointToClient(cx, cy, scaleRecordedClient)) return;
    // UWP/WinUI：PostMessage 不生效，走 UIA Invoke 兜底（与是否窗口相对录制无关；
    // WindowUsesUiaClickFallback 已排除 Win32/Unity，避免误抢前台）。
    // Down 成功调用 Invoke 后，配对 Up 直接跳过，避免重复触发。
    if (!IsCdpInputMode() && down
        && WindowUsesUiaClickFallback(TopLevelTargetWindow(TargetHwnd()))) {
        if (TryUiaClickAtClient(cx, cy)) {
            uiaInvokePending_ = true;
            return;
        }
    }
    if (!down && uiaInvokePending_) {
        uiaInvokePending_ = false;
        return;
    }
    ResolveClickClientPos(cx, cy);

    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) return;
        MaybeRefreshExtLayout();
        UpdateExtSurfaceSize();
        RememberSoftMouseClientPos(TargetHwnd(), cx, cy);
        int sx = cx;
        int sy = cy;
        ToCaptureSurfaceClient(sx, sy);
        const bool ok = ext_.IsConnected()
            ? ext_.MouseButton(sx, sy, button, down, err)
            : cdp_.MouseButton(sx, sy, button, down, err);
        if (!ok) {
            WindowModeLogf(L"[窗口模式] 网页鼠标按键失败: %s", err.c_str());
        }
        return;
    }
    if (!PrepareSoftInput(err)) return;
    if (PreferHardwareInput()) {
        SendHardwareCursorToClient(cx, cy);
        MouseButtonEvent(button, down);
        return;
    }
    SyncFakeFocusCursor(cx, cy);
    SyncFakeFocusMouseButton(button, down);
    if (!UsesInProcFakeFocusSoftInput()) {
        PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, down);
    } else {
        WindowModeLogEventf(L"[窗口模式] 假焦点软鼠标 %s %s 客户区(%d,%d) (%s)",
            button == MouseButtonType::Left ? L"左键"
                : button == MouseButtonType::Right ? L"右键"
                : button == MouseButtonType::Middle ? L"中键" : L"侧键",
            down ? L"down" : L"up", cx, cy,
            UsesMapleStoryFakeFocusInput() ? L"共享内存/DirectInput" : L"DLL/PostMessage 队列");
    }
}

bool WindowModeExecutor::TryUiaClickAtClient(int cx, int cy) {
    if (PreferHardwareInput()) return false;
    HWND input = TargetHwnd();
    HWND top = TopLevelTargetWindow(input);
    if (!top || !IsWindow(top)) return false;
    int sx = cx, sy = cy;
    if (!ClientToScreenPoint(input ? input : top, sx, sy, sx, sy)) {
        if (!ClientToScreenPoint(top, cx, cy, sx, sy)) return false;
    }
    // UIA Invoke 对部分应用（UWP 计算器等）会把目标窗口唤到前台：
    // 记住点击前的前台窗口，点击后若目标被唤出则立即还原，保持后台不抢焦点。
    HWND preserveFg = GetForegroundWindow();
    const bool ok = TryUiaInvokeAtScreenPoint(top, sx, sy);
    if (ok) {
        ++uiaInvokeCount_;
        WindowModeLogEventf(L"[窗口模式] UIA 点击 屏幕(%d,%d) 客户区(%d,%d)",
            sx, sy, cx, cy);
        // UIA Invoke 的窗口激活是异步的：快速轮询，一旦发现目标被唤出立即还原原前台，
        // 尽量缩短可见闪烁（后台操作绝不占用前台）。
        if (preserveFg && IsWindow(preserveFg) && preserveFg != top) {
            bool restored = false;
            for (int i = 0; i < 12; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                HWND curFg = GetForegroundWindow();
                HWND curTop = TopLevelTargetWindow(curFg);
                if (curTop != top) continue;
                // 还原点击前的前台窗口：挂到当前前台线程后再 SetForegroundWindow。
                DWORD curThread = GetCurrentThreadId();
                DWORD fgThread = curFg ? GetWindowThreadProcessId(curFg, nullptr) : 0;
                if (fgThread && fgThread != curThread) {
                    AttachThreadInput(curThread, fgThread, TRUE);
                }
                AllowSetForegroundWindow(ASFW_ANY);
                SetForegroundWindow(preserveFg);
                if (fgThread && fgThread != curThread) {
                    AttachThreadInput(curThread, fgThread, FALSE);
                }
                WindowModeLogEvent(L"[窗口模式] UIA 点击后已还原原前台窗口");
                restored = true;
                break;
            }
            if (!restored) {
                // 未检测到目标被唤出：结束前兜底再查一次
                HWND curFg = GetForegroundWindow();
                HWND curTop = TopLevelTargetWindow(curFg);
                if (curTop == top) {
                    DWORD curThread = GetCurrentThreadId();
                    DWORD fgThread = curFg ? GetWindowThreadProcessId(curFg, nullptr) : 0;
                    if (fgThread && fgThread != curThread) {
                        AttachThreadInput(curThread, fgThread, TRUE);
                    }
                    AllowSetForegroundWindow(ASFW_ANY);
                    SetForegroundWindow(preserveFg);
                    if (fgThread && fgThread != curThread) {
                        AttachThreadInput(curThread, fgThread, FALSE);
                    }
                    WindowModeLogEvent(L"[窗口模式] UIA 点击后已还原原前台窗口");
                }
            }
        }
    }
    return ok;
}

void WindowModeExecutor::PostMouseClickAtClient(int cx, int cy, MouseButtonType button,
    bool scaleRecordedClient) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;
    if (!UsesCdpInput(session_.Config())) {
        std::wstring prepErr;
        if (!PrepareSoftInput(prepErr)) return;
    }
    if (!MapScriptPointToClient(cx, cy, scaleRecordedClient)) return;
    // UWP/WinUI：与 PostMouseButtonAtClient 相同，不依赖 windowRelativeCoordinates。
    if (!IsCdpInputMode()
        && WindowUsesUiaClickFallback(TopLevelTargetWindow(TargetHwnd()))
        && TryUiaClickAtClient(cx, cy)) {
        return;
    }
    ResolveClickClientPos(cx, cy);
    WindowModeLogEventf(L"[窗口模式] 点击 → 客户区(%d,%d) %s",
        cx, cy, button == MouseButtonType::Left ? L"左键"
            : button == MouseButtonType::Right ? L"右键"
            : button == MouseButtonType::Middle ? L"中键"
            : button == MouseButtonType::X1 ? L"侧键X1"
            : button == MouseButtonType::X2 ? L"侧键X2" : L"未知");

    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) return;
        MaybeRefreshExtLayout();
        UpdateExtSurfaceSize();
        RememberSoftMouseClientPos(TargetHwnd(), cx, cy);
        int sx = cx;
        int sy = cy;
        ToCaptureSurfaceClient(sx, sy);
        const bool ok = ext_.IsConnected()
            ? ext_.MouseClick(sx, sy, button, err)
            : cdp_.MouseClick(sx, sy, button, err);
        if (ok) return;
        WindowModeLogf(L"[窗口模式] 网页点击失败: %s，回退软点击", err.c_str());
        // 扩展桥超时/卡死时立刻 PostMessage，避免脚本空等像「失焦」。
        if (PrepareSoftInput(err)) {
            SyncFakeFocusCursor(cx, cy);
            SyncFakeFocusMouseButton(button, true);
            PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, true);
            if (UsesFakeFocus(session_.Config())) {
                WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(25));
            }
            SyncFakeFocusMouseButton(button, false);
            PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, false);
        }
        return;
    }
    if (!PrepareSoftInput(err)) return;
    if (PreferHardwareInput()) {
        SendHardwareCursorToClient(cx, cy);
        MouseClick(button);
        return;
    }
    SyncFakeFocusCursor(cx, cy);
    SyncFakeFocusMouseButton(button, true);
    if (!UsesInProcFakeFocusSoftInput()) {
        PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, true);
    }
    if (UsesFakeFocus(session_.Config())) {
        WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(25));
    }
    SyncFakeFocusMouseButton(button, false);
    if (!UsesInProcFakeFocusSoftInput()) {
        PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, false);
    }
    if (UsesInProcFakeFocusSoftInput()) {
        WindowModeLogEventf(L"[窗口模式] 假焦点软点击 客户区(%d,%d) (%s)",
            cx, cy,
            UsesMapleStoryFakeFocusInput() ? L"共享内存/DirectInput" : L"DLL/PostMessage 队列");
    }

}

void WindowModeExecutor::PostKeyToTarget(UINT vk, bool down) {
    if (!active_ || vk == 0 || WindowModeCancelled(cancelFlag_)) return;

    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) return;
        const bool ok = ext_.IsConnected()
            ? ext_.KeyEvent(vk, down, err)
            : cdp_.KeyEvent(vk, down, err);
        if (!ok) {
            WindowModeLogf(L"[窗口模式] 网页按键失败: %s", err.c_str());
        }
        return;
    }
    if (!PrepareSoftInput(err)) return;
    if (PreferHardwareInput()) {
        const bool chromiumShell = LooksLikeChromiumShellTarget(session_.Config(), TargetHwnd());
        if (fakeFocus_.IsInjected() && !chromiumShell) fakeFocus_.Unload();
        EnsureHardwareInputFocus();
        if (chromiumShell && FakeFocusSoftInput_IsAttached()) {
            FakeFocusSoftInput_SetKey(vk, down);
        }
        SendKeyboardKey(vk, down);
        WindowModeLogEventf(L"[窗口模式] 本机按键 vk=0x%02X %s%s",
            vk, down ? L"down" : L"up",
            chromiumShell ? L" (Chromium壳 假前台回退)" : L"");
        return;
    }
    // Chromium 壳：进程内 PostMessage；冒险岛：只写共享内存，由 DLL 填 DirectInput/GetAsyncKeyState。
    if (UsesInProcFakeFocusSoftInput()) {
        FakeFocusSoftInput_SetKey(vk, down);
        WindowModeLogEventf(L"[窗口模式] 假焦点软按键 vk=0x%02X %s (%s)",
            vk, down ? L"down" : L"up",
            UsesMapleStoryFakeFocusInput() ? L"共享内存/DirectInput" : L"DLL/PostMessage 队列");
        if (UsesMapleStoryFakeFocusInput()) {
            if (++g_mapleSoftKeyLogs == 8) {
                DWORD gaks = 0;
                DWORD diState = 0;
                DWORD diData = 0;
                DWORD lastCb = 0;
                DWORD hitReady = 0;
                DWORD gfw = 0;
                DWORD focus = 0;
                if (FakeFocusSoftInput_ReadMapleHits(gaks, diState, diData, lastCb, hitReady, gfw, focus)) {
                    WindowModeLogf(
                        L"[窗口模式] 冒险岛钩命中 hitReady=%lu gfw=%lu focus=%lu "
                        L"gaks=%lu diState=%lu diData=%lu lastCb=%lu",
                        static_cast<unsigned long>(hitReady),
                        static_cast<unsigned long>(gfw),
                        static_cast<unsigned long>(focus),
                        static_cast<unsigned long>(gaks),
                        static_cast<unsigned long>(diState),
                        static_cast<unsigned long>(diData),
                        static_cast<unsigned long>(lastCb));
                }
            }
        }
        return;
    }
    if (FakeFocusSoftInput_IsAttached()) {
        FakeFocusSoftInput_SetKey(vk, down);
    }
    HWND top = TopLevelTargetWindow(TargetHwnd());
    // DeSmuME 只吃 GetAsyncKeyState 轮询，外层 WM_KEY* 无效且可能干扰。
    if (!IsDesktopEmulatorTarget(top, &session_.Config())) {
        PostKeyToWindow(TargetHwnd(), vk, down);
    }
}

void WindowModeExecutor::PostScrollWheelAtClient(int cx, int cy, int steps, bool vertical, bool positive,
    bool scaleRecordedClient) {
    if (!active_ || steps <= 0) return;
    if (!UsesCdpInput(session_.Config())) {
        std::wstring prepErr;
        if (!PrepareSoftInput(prepErr)) return;
    }
    if (!MapScriptPointToClient(cx, cy, scaleRecordedClient)) return;
    ResolveClickClientPos(cx, cy);
    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) return;
        const bool ok = ext_.IsConnected()
            ? ext_.Scroll(cx, cy, steps, vertical, positive, err)
            : cdp_.Scroll(cx, cy, steps, vertical, positive, err);
        if (!ok) {
            WindowModeLogf(L"[窗口模式] 网页滚轮失败: %s", err.c_str());
        }
        return;
    }
    if (!PrepareSoftInput(err)) return;
    if (PreferHardwareInput()) {
        SendHardwareCursorToClient(cx, cy);
        const int delta = (positive ? WHEEL_DELTA : -WHEEL_DELTA) * steps;
        ForegroundInputRouter::Instance().Wheel(delta, !vertical);
        return;
    }
    SyncFakeFocusCursor(cx, cy);
    if (UsesInProcFakeFocusSoftInput()) {
        FakeFocusSoftInput_PushWheel(vertical, positive, steps);
        WindowModeLogEventf(L"[窗口模式] 假焦点软滚轮 steps=%d %s (%s)",
            steps, vertical ? L"竖向" : L"横向",
            UsesMapleStoryFakeFocusInput() ? L"共享内存/DirectInput" : L"DLL/PostMessage 队列");
        return;
    }
    PostScrollWheelToWindow(TargetHwnd(), cx, cy, steps, vertical, positive);
}

void WindowModeExecutor::SendQuickInputToTarget(const std::wstring& text, double charInterval) {
    if (!active_) return;
    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) {
            WindowModeLogf(L"[窗口模式] 快捷输入跳过(网页桥): %s", err.c_str());
            return;
        }
        const bool ok = ext_.IsConnected()
            ? ext_.InsertText(text, err)
            : cdp_.InsertText(text, err);
        if (!ok) {
            WindowModeLogf(L"[窗口模式] 网页快捷输入失败: %s", err.c_str());
        }
        (void)charInterval;
        return;
    }
    if (!PrepareSoftInput(err)) {
        WindowModeLogf(L"[窗口模式] 快捷输入跳过: %s", err.c_str());
        return;
    }
    if (!session_.RefreshInputBinding(err)) {
        WindowModeLogf(L"[窗口模式] 快捷输入刷新绑定失败: %s", err.c_str());
        return;
    }

    if (PreferHardwareInput()) {
        if (fakeFocus_.IsInjected()) fakeFocus_.Unload();
        SendQuickInputText(text, charInterval, cancelFlag_);
        return;
    }
    HWND hwnd = TargetHwnd();
    const bool allowForeground = UsesFakeFocus(session_.Config())
        ? false
        : session_.Config().allowForegroundInputFallback;
    PostQuickInputToWindow(hwnd, text, charInterval, allowForeground, cancelFlag_);

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

    int searchX1 = a.searchX1;
    int searchY1 = a.searchY1;
    int searchX2 = a.searchX2;
    int searchY2 = a.searchY2;
    if (session_.Config().windowRelativeCoordinates) {
        int recW = 0, recH = 0, liveW2 = 0, liveH2 = 0;
        if (RecordedClientSize(recW, recH) && LiveClientSize(liveW2, liveH2)) {
            ScaleWindowClientRect(recW, recH, liveW2, liveH2, searchX1, searchY1, searchX2, searchY2);
        }
    }

    if (a.searchFullScreen || (searchX1 == 0 && searchY1 == 0 && searchX2 == 0 && searchY2 == 0)) {
        x1 = 0;
        y1 = 0;
        x2 = clientW;
        y2 = clientH;
        return true;
    }

    // 找图配置为整屏(如 0,0,2560,1440) 时，窗口模式下等价于全客户区。
    if (UsesClientCoords() && searchX2 > searchX1 && searchY2 > searchY1
        && searchX1 <= 0 && searchY1 <= 0
        && searchX2 >= clientW && searchY2 >= clientH) {
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

        if (session_.Config().windowRelativeCoordinates) {
            if (applyClientRect(searchX1, searchY1, searchX2, searchY2)) {
                return true;
            }
            x1 = 0;
            y1 = 0;
            x2 = clientW;
            y2 = clientH;
            return true;
        }

        HWND root = TopLevelTargetWindow(CaptureTargetHwnd());
        const bool iconic = root && IsIconic(root);

        if (!iconic && hwnd) {
            int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
            if (ScreenSearchRectToClientRect(hwnd, searchX1, searchY1, searchX2, searchY2,
                    cx1, cy1, cx2, cy2)
                && applyClientRect(cx1, cy1, cx2, cy2)) {
                return true;
            }
        }

        const RECT& bound = st.clientRectScreen;
        if (bound.right > bound.left && bound.bottom > bound.top) {
            const int cx1 = searchX1 - bound.left;
            const int cy1 = searchY1 - bound.top;
            const int cx2 = searchX2 - bound.left;
            const int cy2 = searchY2 - bound.top;
            if (applyClientRect(cx1, cy1, cx2, cy2)) {
                WindowModeLogf(L"[窗口模式] ResolveClientSearchRect: 绑定原点映射 screen(%d,%d,%d,%d) bound(%d,%d,%d,%d)",
                    searchX1, searchY1, searchX2, searchY2,
                    bound.left, bound.top, bound.right, bound.bottom);
                return true;
            }
        }

        if (searchX2 > searchX1 && searchY2 > searchY1
            && searchX1 >= 0 && searchY1 >= 0
            && searchX2 <= clientW && searchY2 <= clientH
            && applyClientRect(searchX1, searchY1, searchX2, searchY2)) {
            WindowModeLog(L"[窗口模式] ResolveClientSearchRect: 按客户区坐标解析");
            return true;
        }

        WindowModeLogf(L"[窗口模式] ResolveClientSearchRect: 搜索区无效 iconic=%d screen(%d,%d,%d,%d) 回退全客户区 %dx%d",
            iconic ? 1 : 0, searchX1, searchY1, searchX2, searchY2, clientW, clientH);
        x1 = 0;
        y1 = 0;
        x2 = clientW;
        y2 = clientH;
        return true;
    }

    x1 = searchX1;
    y1 = searchY1;
    x2 = searchX2;
    y2 = searchY2;
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
    if (WindowModeCancelled(cancelFlag_)) return output;

    std::wstring err;
    if (!EnsureTargetBound(err)) {
        WindowModeLogf(L"[窗口模式] FindImageClient: EnsureTargetBound 失败 %s",
            err.empty() ? L"(无详情)" : err.c_str());
        return output;
    }
    if (WindowModeCancelled(cancelFlag_)) return output;

    if (UsesBackgroundWindow()) {
        std::wstring geoErr;
        if (!EnsurePlaybackGeometry(geoErr)) {
            WindowModeLogf(L"[窗口模式] FindImageClient: 最小化目标无法安静还原 %s",
                geoErr.c_str());
            return output;
        }
    }

    HWND prepRoot = TopLevelTargetWindow(CaptureTargetHwnd());
    int deskAtStart = -1;
    {
        auto& vda = VirtualDesktopAccessor::Instance();
        deskAtStart = vda.GetCurrentDesktopNumber();
    }
    WindowModeLogDesktopSnap(L"找图前", prepRoot);

    ScriptAction probe = a;
    if (probe.imagePath.empty() && !probe.aiTargetImagePath.empty()) {
        probe.imagePath = probe.aiTargetImagePath;
    }

    HBITMAP tmpl = nullptr;
    ImageMatchOptions opt;
    TemplateScale ts = FindImageTemplateScale();
    if (ts.sx <= 0.0 || ts.sy <= 0.0) {
        ts = FindImageSurfaceScale(0, 0);
    }
    const PreparedFindImageMatch findMatch = PrepareFindImageMatch(probe, ts);
    tmpl = findMatch.bitmap;
    opt = findMatch.options;
    if (!tmpl) {
        WindowModeLog(L"[窗口模式] FindImageClient: 模板图加载失败");
        return output;
    }
    opt.maxMatches = 20;
    opt.maxOverlap = 0.5;

    auto applyWindowRelativeSurface = [&](int surfaceW, int surfaceH,
            ImageMatchOptions& io, int& x1, int& y1, int& x2, int& y2) {
        if (surfaceW <= 0 || surfaceH <= 0) return;
        if (session_.Config().windowRelativeCoordinates) {
            const TemplateScale surfTs = FindImageSurfaceScale(surfaceW, surfaceH);
            if (surfTs.sx > 0.0 && surfTs.sy > 0.0) {
                ts = surfTs;
                io = BuildExecutionFindImageOptions(probe, ts);
                io.maxMatches = 20;
                io.maxOverlap = 0.5;
                // 窗口拉伸可大于 1.0；全局 anamorphic 选项曾把 scaleMax 封在 1.05。
                const double lo = std::min(surfTs.sx, surfTs.sy);
                const double hi = std::max(surfTs.sx, surfTs.sy);
                io.scaleMin = std::max(0.1, std::min(io.scaleMin, lo * 0.92));
                io.scaleMax = std::max(io.scaleMax, hi * 1.08);
                if (io.scaleMax < io.scaleMin) io.scaleMax = io.scaleMin;
            }
        }
        int liveW = 0, liveH = 0;
        LiveClientSize(liveW, liveH);
        if (liveW <= 0 || liveH <= 0) {
            x1 = 0;
            y1 = 0;
            x2 = surfaceW;
            y2 = surfaceH;
            return;
        }
        const bool fullClient = probe.searchFullScreen
            || (x1 <= 0 && y1 <= 0 && x2 >= liveW - 1 && y2 >= liveH - 1);
        if (fullClient) {
            x1 = 0;
            y1 = 0;
            x2 = surfaceW;
            y2 = surfaceH;
            return;
        }
        x1 = std::clamp((x1 * surfaceW) / liveW, 0, surfaceW);
        y1 = std::clamp((y1 * surfaceH) / liveH, 0, surfaceH);
        x2 = std::clamp((x2 * surfaceW) / liveW, 0, surfaceW);
        y2 = std::clamp((y2 * surfaceH) / liveH, 0, surfaceH);
    };

    auto mapSurfaceMatchesToLiveClient = [&](ImageMatchOutput& matched, int surfaceW, int surfaceH) {
        if (!session_.Config().windowRelativeCoordinates) return;
        if (surfaceW <= 0 || surfaceH <= 0) return;
        int liveW = 0, liveH = 0;
        if (!LiveClientSize(liveW, liveH) || liveW <= 0 || liveH <= 0) return;
        if (surfaceW == liveW && surfaceH == liveH) return;
        for (auto& m : matched.matches) {
            m.topLeftX = (m.topLeftX * liveW) / surfaceW;
            m.topLeftY = (m.topLeftY * liveH) / surfaceH;
            m.bottomRightX = (m.bottomRightX * liveW) / surfaceW;
            m.bottomRightY = (m.bottomRightY * liveH) / surfaceH;
            m.x = (m.x * liveW) / surfaceW;
            m.y = (m.y * liveH) / surfaceH;
        }
    };

    // 窗口模式 CDP：找图必须走扩展 HTTP 截图（PrintWindow/Cloak 必切屏，已证伪）。
    // 后台 CDP：可扩展优先，失败再同桌面 Win32。
    const bool windowModeCdp = !UsesBackgroundWindow() && UsesCdpInput(session_.Config());
    const bool cdpExt = UsesCdpInput(session_.Config());
    if (windowModeCdp && ext_.IsConnected() && !ext_.SupportsStableBridgeApi()) {
        WindowModeLog(L"[窗口模式] ★请重载扩展到 v1.1.43+★（HTTP 截图；轻量保活防卡帧）");
    }
    const bool cdpExtVision = UsesCdpInput(session_.Config())
        && ext_.IsConnected()
        && ext_.SupportsExtVision()
        && ext_.SupportsStableBridgeApi();
    const bool preferExtShot = UsesCdpInput(session_.Config())
        && ext_.IsConnected()
        && (ext_.SupportsExtVision() || ext_.SupportsSafeExtScreenshot())
        && ext_.SupportsStableBridgeApi();
    if (preferExtShot) {
        if (UserOnMacroDesktopNow() && prepRoot && IsWindow(prepRoot)) {
            const DWORD settle0 = GetTickCount();
            RaiseMacroDesktopWindowForWatch(prepRoot);
            for (int i = 0; i < 8; ++i) {
                if (WindowModeCancelled(cancelFlag_)) break;
                if (!IsMacroVisionInvisibilityActive(prepRoot) && !IsIconic(prepRoot)) {
                    RECT wr{};
                    if (GetWindowRect(prepRoot, &wr) && wr.left > -2000 && wr.top > -2000) break;
                }
                RaiseMacroDesktopWindowForWatch(prepRoot);
                WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(40));
            }
            WindowModeLogf(L"[窗口模式] 找图观看就绪 settle=%ums onMacro=1 ready=%d",
                GetTickCount() - settle0, IsMacroVisionCaptureReady(prepRoot) ? 1 : 0);
        }
        MaybeRefreshExtLayout();
        const auto& st = session_.State();
        int refW = 0, refH = 0;
        if (ResolveExtVisionSurface(ext_, CaptureTargetHwnd(), st.clientW, st.clientH, refW, refH)) {
            WindowModeLogf(L"[窗口模式] 找图表面: pageCss×dpr → %dx%d (onMacro=%d)",
                refW, refH, UserOnMacroDesktopNow() ? 1 : 0);
        }

        HBITMAP shot = nullptr;
        int sw = 0, sh = 0;
        bool canvasSpace = false;
        std::wstring shotErr;
        const DWORD tShot0 = GetTickCount();
        const bool got = cdpExtVision
            ? ext_.CaptureScreenshotForVisionMatch(refW, refH, &shot, &sw, &sh, &canvasSpace, shotErr)
            : ext_.CaptureScreenshotForClientMatch(refW, refH, &shot, &sw, &sh, shotErr);
        const DWORD shotMs = GetTickCount() - tShot0;
        if (got && shot && sw >= 64 && sh >= 64 && !IsCaptureLikelyBlank(shot)) {
            ext_.SetSurfaceSize(refW, refH);
            int cx1 = 0, cy1 = 0, cx2 = sw, cy2 = sh;
            if (!canvasSpace) {
                if (!ResolveClientSearchRect(probe, cx1, cy1, cx2, cy2)) {
                    cx1 = 0;
                    cy1 = 0;
                    cx2 = sw;
                    cy2 = sh;
                }
                if (cx2 > sw || cy2 > sh || cx2 <= cx1 || cy2 <= cy1) {
                    cx1 = 0;
                    cy1 = 0;
                    cx2 = sw;
                    cy2 = sh;
                }
            }
            applyWindowRelativeSurface(sw, sh, opt, cx1, cy1, cx2, cy2);
            cx1 = std::clamp(cx1, 0, sw);
            cy1 = std::clamp(cy1, 0, sh);
            cx2 = std::clamp(cx2, 0, sw);
            cy2 = std::clamp(cy2, 0, sh);

            ImageMatchOutput matched = FindTemplateInFrozenScreenMulti(
                shot, 0, 0, cx1, cy1, cx2, cy2, tmpl, opt);
            const DWORD tMatch0 = GetTickCount();
            if (canvasSpace) {
                int ix = 0, iy = 0, iw = 0, ih = 0, pw = 0, ph = 0;
                ext_.GetIframeCssRect(ix, iy, iw, ih);
                ext_.GetPageCssSize(pw, ph);
                MapCanvasMatchesToSurface(matched, sw, sh, refW, refH, ix, iy, iw, ih, pw, ph);
                WindowModeLogf(
                    L"[窗口模式] canvas→iframe映射: canvas=%dx%d surface=%dx%d iframeCss=(%d,%d)%dx%d pageCss=%dx%d",
                    sw, sh, refW, refH, ix, iy, iw, ih, pw, ph);
            }
            const DWORD matchMs = GetTickCount() - tMatch0;
            DeleteObject(shot);
            WindowModeLogf(
                L"[窗口模式] 匹配(扩展截图%s): 命中=%zu best=%.1f%% peakNcc=%.1f%% shot=%ums match=%ums",
                canvasSpace ? L"/canvas" : L"/client",
                matched.matches.size(),
                matched.matches.empty() ? 0.0 : matched.matches.front().score,
                matched.debugBestNccPercent,
                shotMs, matchMs);
            DeleteBitmapHandle(tmpl);
            WindowModeLogDesktopSnap(L"找图后", prepRoot);
            return matched;
        }
        if (shot) {
            DeleteObject(shot);
            shot = nullptr;
        }
        WindowModeLogf(
            L"[窗口模式] 扩展截图不可用: %s",
            shotErr.empty() ? L"(无详情)" : shotErr.c_str());
        if (windowModeCdp) {
            // 禁止回退 PrintWindow（sameProc Edge 必切屏）。
            WindowModeLog(L"[窗口模式] 窗口模式 CDP：禁止 Win32 PrintWindow 回退（零切屏）");
            DeleteBitmapHandle(tmpl);
            WindowModeLogDesktopSnap(L"找图后", prepRoot);
            return output;
        }
    } else if (UsesCdpInput(session_.Config()) && ext_.IsConnected()
        && !ext_.SupportsStableBridgeApi()) {
        MaybeRefreshExtLayout();
        WindowModeLog(L"[窗口模式] 扩展 <1.1.15：无 HTTP 截图能力");
        if (windowModeCdp) {
            DeleteBitmapHandle(tmpl);
            return output;
        }
    }

    // CDP：禁止 ShowMacro/Cloak latch。窗口模式不应再落到下方 Win32。
    ScopedVisionCapturePrep prep(CaptureTargetHwnd(), UsesBackgroundWindow());
    WindowModeLogDesktopSnap(L"截图准备后", prepRoot);

    if (WindowModeCancelled(cancelFlag_)) {
        DeleteBitmapHandle(tmpl);
        return output;
    }
    if (!prep.Ready()) {
        WindowModeLog(L"[窗口模式] FindImageClient: VisionPrep 未就绪");
        DeleteBitmapHandle(tmpl);
        return output;
    }
    MaybeRefreshExtLayout();
    if (UsesBackgroundWindow()) {
        WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(40));
        if (WindowModeCancelled(cancelFlag_)) {
            DeleteBitmapHandle(tmpl);
            return output;
        }
    }

    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
    if (!ResolveClientSearchRect(probe, cx1, cy1, cx2, cy2)) {
        WindowModeLog(L"[窗口模式] FindImageClient: ResolveClientSearchRect 失败");
        DeleteBitmapHandle(tmpl);
        return output;
    }

    WindowModeLogVerbosef(
        L"[窗口模式] FindImageClient: 模板 %dx%d scale=%.3fx%.3f matchScale=%.3f~%.3f thr=%.0f search=(%d,%d)-(%d,%d)",
        findMatch.templateW, findMatch.templateH, ts.sx, ts.sy,
        opt.scaleMin, opt.scaleMax, opt.thresholdPercent, cx1, cy1, cx2, cy2);

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
            BITMAP lbm{};
            int lw = 0, lh = 0;
            if (GetObjectW(lockedBmp, sizeof(lbm), &lbm)) {
                lw = lbm.bmWidth;
                lh = lbm.bmHeight;
            }
            int mx1 = cx1, my1 = cy1, mx2 = cx2, my2 = cy2;
            ImageMatchOptions matchOpt = opt;
            if (lw > 0 && lh > 0) {
                applyWindowRelativeSurface(lw, lh, matchOpt, mx1, my1, mx2, my2);
            }
            ImageMatchOutput matched = FindTemplateInFrozenScreenMulti(
                lockedBmp, 0, 0, mx1, my1, mx2, my2, tmpl, matchOpt);
            mapSurfaceMatchesToLiveClient(matched, lw, lh);
            MapMatchResultsToInputClient(captureHwnd, inputHwnd, matched);
            return matched;
        }
        WindowCaptureResult capture = CaptureWindowClientForVision(captureHwnd);
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
        WindowModeLogVerbosef(L"[窗口模式] FindImageClient: 截图 %dx%d blank=%d print=%d wgc=%d",
            capture.w, capture.h, blank ? 1 : 0, capture.fromPrintWindow ? 1 : 0,
            capture.fromPrintWindow ? 0 : 1);
        if (blank) {
            DeleteObject(capture.bitmap);
            return {};
        }
        int mx1 = cx1, my1 = cy1, mx2 = cx2, my2 = cy2;
        ImageMatchOptions matchOpt = opt;
        applyWindowRelativeSurface(capture.w, capture.h, matchOpt, mx1, my1, mx2, my2);
        ImageMatchOutput matched = FindTemplateInFrozenScreenMulti(
            capture.bitmap, 0, 0, mx1, my1, mx2, my2, tmpl, matchOpt);
        if (IsCdpInputMode() && ext_.IsConnected()) {
            const auto& stCap = session_.State();
            if (stCap.clientW >= 400 && stCap.clientH >= 300) {
                ext_.SetSurfaceSize(stCap.clientW, stCap.clientH);
            } else if (capture.w >= 64 && capture.h >= 64) {
                ext_.SetSurfaceSize(capture.w, capture.h);
            }
        }
        DeleteObject(capture.bitmap);
        mapSurfaceMatchesToLiveClient(matched, capture.w, capture.h);
        MapMatchResultsToInputClient(captureHwnd, inputHwnd, matched);
        WindowModeLogVerbosef(L"[窗口模式] FindImageClient: 匹配数=%zu 最高=%.1f%%",
            matched.matches.size(),
            matched.matches.empty() ? 0.0 : matched.matches.front().score);
        return matched;
    };

    output = captureAndMatch();
    // PrintWindow 可能短暂抢到宏桌面：立刻 Correct/HoldView（禁止放任闪屏）。
    // CDP：禁止 Correct/HoldView（用户可进「鼠标宏」）。
    if (!cdpExt) {
        if (!UserOnMacroDesktopNow()) {
            /* view-steal removed */
            /* HoldPreferred removed */
        }
    }
    WindowModeLogDesktopSnap(L"找图后", prepRoot);
    if (!cdpExt && deskAtStart >= 0) {
        auto& vda = VirtualDesktopAccessor::Instance();
        const int nowDesk = vda.GetCurrentDesktopNumber();
        if (nowDesk >= 0 && nowDesk != deskAtStart) {
            WindowModeLogf(L"[窗口模式] 找图后 UserDesk 变化: %d->%d",
                deskAtStart, nowDesk);
            if (!UserOnMacroDesktopNow()) {
                /* view-steal removed */
                /* HoldPreferred removed */
            }
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
    if (!active_ || WindowModeCancelled(cancelFlag_)) return false;
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
    if (!active_ || WindowModeCancelled(cancelFlag_)) return nullptr;
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
    // (0,0) soft 视为未播种，继续读系统光标（避免硬件路径再飞到左上角）。
    int softX = 0, softY = 0;
    if (GetLastSoftMouseClientPos(TargetHwnd(), softX, softY) && (softX != 0 || softY != 0)) {
        cx = softX;
        cy = softY;
        return true;
    }
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    // 硬件绝对光标 / 窗口相对：必须映射到目标客户区（勿把屏幕坐标当客户区）。
    if (PreferHardwareInput() || UsesClientCoords()
        || session_.Config().windowRelativeCoordinates) {
        HWND hwnd = TargetHwnd();
        if (!hwnd || !IsWindow(hwnd)) return false;
        return ScreenToClientPoint(hwnd, pt.x, pt.y, cx, cy);
    }
    cx = pt.x;
    cy = pt.y;
    return true;
}

}  // namespace windowmode
