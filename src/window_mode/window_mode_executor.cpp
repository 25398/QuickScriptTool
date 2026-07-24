#include "window_mode_executor.h"

#include "background_window_input.h"
#include "action_utils.h"
#include "coord_space.h"
#include "ext_bridge/ext_bridge_server.h"
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
#include <cmath>
#include <cstdarg>
#include <thread>

namespace windowmode {

WindowModeExecutor::WindowModeExecutor() = default;
WindowModeExecutor::~WindowModeExecutor() {
    if (active_) EndRun();
}


namespace {

void DebugLog(const wchar_t* msg) {
    WindowModeLog(msg);
}

void WindowModeLogVerbosef(const wchar_t* fmt, ...) {
    if (!fmt) return;
    wchar_t buf[1024]{};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    WindowModeLog(buf);
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
    if (active_) EndRun();
    if (!config.enabled) {
        active_ = false;
        return true;
    }

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
    } else {
        WindowModeLog(L"[窗口模式] 使用 PostMessage/软消息（非 CDP）");
    }
    DebugLog(L"[WindowMode] Executor::BeginRun OK");
    return true;
}

void WindowModeExecutor::NotifyCancel() {
    ExtBridgeServer::Instance().AbortPending();
    SignalStopCdpMacroDesktopWatchPump();
}

void WindowModeExecutor::EndRun() {
    StopCdpMacroDesktopWatchPump();
    HWND top = TopLevelTargetWindow(session_.State().targetHwnd);
    if (top && IsWindow(top) && UsesCdpInput(session_.Config()) && !UsesBackgroundWindow()) {
        RestoreMacroDesktopWindowAfterRun(top);
    }
    ExtBridgeServer::Instance().ClearAbort();
    cdp_.Disconnect();
    ext_.Disconnect();
    session_.Stop();
    active_ = false;
    cancelFlag_ = nullptr;
    extLayoutFresh_ = false;
    ResetSoftMouseState();
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
    return EnsureTargetBound(err);
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

void WindowModeExecutor::MoveMouseClient(int cx, int cy, int randomX, int randomY,
    const std::function<int(int)>& randomInt) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;

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
    PostMouseMoveToWindow(TargetHwnd(), tx, ty);
}

void WindowModeExecutor::ResolveClickClientPos(int& cx, int& cy) const {
    // 脚本里鼠标点击 x=y=0 表示「当前位置」。窗口/网页键鼠不移动系统光标，
    // 必须落到上次软光标，否则会点到客户区 (0,0)。
    if (cx != 0 || cy != 0) return;
    int lx = 0, ly = 0;
    if (GetLastSoftMouseClientPos(TargetHwnd(), lx, ly)) {
        cx = lx;
        cy = ly;
        return;
    }
    if (GetCursorClientPos(lx, ly)) {
        cx = lx;
        cy = ly;
    }
}

void WindowModeExecutor::PostMouseButtonAtClient(int cx, int cy, MouseButtonType button, bool down) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;
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
    PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, down);
}

void WindowModeExecutor::PostMouseClickAtClient(int cx, int cy, MouseButtonType button) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;
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
            ? ext_.MouseClick(sx, sy, button, err)
            : cdp_.MouseClick(sx, sy, button, err);
        if (ok) return;
        WindowModeLogf(L"[窗口模式] 网页点击失败: %s，回退软点击", err.c_str());
        // 扩展桥超时/卡死时立刻 PostMessage，避免脚本空等像「失焦」。
        if (PrepareSoftInput(err)) {
            PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, true);
            PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, false);
        }
        return;
    }
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
    PostKeyToWindow(TargetHwnd(), vk, down);
}

void WindowModeExecutor::PostScrollWheelAtClient(int cx, int cy, int steps, bool vertical, bool positive) {
    if (!active_ || steps <= 0) return;
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

    const bool allowForeground = UsesFakeFocus(session_.Config())
        ? false
        : session_.Config().allowForegroundInputFallback;
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
    if (WindowModeCancelled(cancelFlag_)) return output;

    std::wstring err;
    if (!EnsureTargetBound(err)) {
        WindowModeLogf(L"[窗口模式] FindImageClient: EnsureTargetBound 失败 %s",
            err.empty() ? L"(无详情)" : err.c_str());
        return output;
    }
    if (WindowModeCancelled(cancelFlag_)) return output;

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
    TemplateScale ts{};
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
    opt.maxMatches = 20;
    opt.maxOverlap = 0.5;

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
            ImageMatchOutput matched = FindTemplateInFrozenScreenMulti(
                lockedBmp, 0, 0, cx1, cy1, cx2, cy2, tmpl, opt);
            MapMatchResultsToInputClient(captureHwnd, inputHwnd, matched);
            return matched;
        }
        WindowCaptureResult capture = windowModeCdp
            ? CaptureWindowClient(captureHwnd)
            : CaptureWindowClient(captureHwnd);
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
        ImageMatchOutput matched = FindTemplateInFrozenScreenMulti(
            capture.bitmap, 0, 0, cx1, cy1, cx2, cy2, tmpl, opt);
        if (IsCdpInputMode() && ext_.IsConnected()) {
            const auto& stCap = session_.State();
            if (stCap.clientW >= 400 && stCap.clientH >= 300) {
                ext_.SetSurfaceSize(stCap.clientW, stCap.clientH);
            } else if (capture.w >= 64 && capture.h >= 64) {
                ext_.SetSurfaceSize(capture.w, capture.h);
            }
        }
        DeleteObject(capture.bitmap);
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
