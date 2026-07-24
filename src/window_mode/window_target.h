#pragma once

#include "window_mode_types.h"

#include <string>
#include <vector>

namespace windowmode {

struct WindowTargetQuery {
    std::wstring exePath;
    std::wstring titleContains;
    std::wstring className;
    std::wstring childClassName;
    DWORD pid = 0;
    int pickX = 0;
    int pickY = 0;
    /// 仅启动等待期间使用：允许 System32\\notepad.exe 交接给商店 Notepad（须配合排除启动前已有窗）。
    bool allowStoreNotepadHandoff = false;
};

WindowTargetQuery BuildTargetQuery(const WindowModeScriptConfig& config);

/// 绑定前校验：顶层窗口是否仍匹配当前配置（避免 relaxed 匹配导致跳过自动打开）。
bool DoesTopWindowMatchConfig(HWND top, const WindowModeScriptConfig& config);

HWND FindMainWindowOnDesktop(const WindowTargetQuery& query, HDESK desktop);
HWND FindMainWindowOnVirtualDesktop(const WindowTargetQuery& query, const GUID& desktopId,
    bool allowMinimized = false);
HWND FindMainWindowOnVirtualDesktopIndex(const WindowTargetQuery& query, int desktopIndex,
    bool allowMinimized = false);
HWND FindMainWindowDefault(const WindowTargetQuery& query, bool allowMinimized = false);
HWND FindMainWindowForProcess(DWORD pid);
/// 收集当前所有匹配主窗（启动前快照用）。
void CollectMatchingMainWindows(const WindowTargetQuery& query, bool allowMinimized,
    std::vector<HWND>& out);
/// 启动等待：按路径找窗，跳过启动前快照中的窗口（避免误绑旧实例）。
HWND FindNewlyAppearedMainWindow(const WindowTargetQuery& query,
    const std::vector<HWND>& excludeHwnds, bool allowMinimized = false);
HWND FindNewlyAppearedMainWindow(const WindowTargetQuery& query, HWND excludeHwnd,
    bool allowMinimized = false);
/// 启动结果窗：优先新出现 / 启动后新建进程；最后才允许复用已有实例（单实例激活）。
HWND FindLaunchResultMainWindow(const WindowTargetQuery& query,
    const std::vector<HWND>& excludeHwnds, const FILETIME& launchTimeUtc,
    bool allowReuseExisting, bool allowMinimized = false);
bool IsDescendantProcess(DWORD pid, DWORD ancestorPid);
HWND FindChildWindowByClass(HWND parent, const std::wstring& childClassName);
/// Edge/Chrome 页面渲染区（Flash/HTML5 游戏），优先于顶层 Chrome_WidgetWin_1。
HWND FindBrowserRenderWidget(HWND top);
/// Intermediate D3D Window 等合成层：仅截图可用，不可作输入绑定目标。
bool IsBrowserCompositorHwnd(HWND hwnd);
/// 找图/截图表面：优先 RenderWidget；否则可回退 compositor（D3D）。
HWND FindBrowserCaptureSurface(HWND top);
bool MainWindowHasChildClass(HWND hwnd, const std::wstring& childClassName);
HWND TopLevelTargetWindow(HWND hwnd);
bool IsWindowOnUserCurrentDesktop(HWND hwnd);
bool IsTargetWindowMinimized(HWND hwnd);
/// 输入法状态条/过小工具窗，不能当作脚本目标主窗。
bool IsLikelyImeOrToolWindow(HWND hwnd);
/// Restores a minimized target without stealing focus on the user's current desktop.
/// skipVisibleZOrder: 后台模式已可见窗口时不改 Z 序/尺寸，避免闪屏与 WGC 游戏区空白。
bool RestoreWindowQuiet(HWND hwnd, bool skipVisibleZOrder = false);
/// 当前用户桌面：置底展开。keepCloaked=true 时保持 DWM Cloak（供截图/点击，避免露脸）。
/// 若 keepCloaked 且 out* 非空，调用方须在最小化后再 EndQuietBottomCloak。
bool RestoreOnUserDesktopBottom(HWND hwnd, HWND preserveFg, bool keepCloaked = false,
    LONG* outSavedExStyle = nullptr, bool* outLayeredTouched = nullptr,
    BYTE* outSavedLayeredAlpha = nullptr, DWORD* outSavedLayeredFlags = nullptr);
void EndQuietBottomCloak(HWND hwnd, LONG savedExStyle, bool layeredTouched,
    BYTE savedLayeredAlpha, DWORD savedLayeredFlags);
/// Restores a saved top-level window placement after background window mode ends.
void RestoreBoundTargetTopWindow(HWND hwnd, const WINDOWPLACEMENT& wp);
/// Restores target for PrintWindow/OCR/find-image on the user's current desktop.
bool PrepareWindowForCapture(HWND hwnd);
/// Restores minimized window without activation (no SW_RESTORE).
bool RestoreWindowNonActivating(HWND hwnd);
/// Keeps a restored target at the bottom of the Z-order on the user's current desktop.
void EnsureTargetBelowUserWindows(HWND hwnd);
/// Same as above but works on any virtual desktop (e.g.「鼠标宏」).
void EnsureTargetAtBottomOfStack(HWND hwnd);
/// 后台启动：立刻 Cloak+最小化+压底，尽量压掉抢前台的一闪。
void ForceHideLaunchedWindowQuiet(HWND hwnd);
/// 藏起本进程/目标进程可见的 GDI+ 辅助窗（标题常为 GDI+ Window）。
void HideTransientGdiPlusWindows(DWORD extraPid = 0);
/// 宏桌面上将已展开窗口压到 Z 轴最底层。
void PinMacroDesktopWindowBottom(HWND hwnd);
/// 跨虚拟桌面移动前安静最小化，避免窗口乱跳。
void MinimizeForQuietDesktopMove(HWND hwnd);

/// 安静铺满监视器工作区（禁止 SW_SHOWMAXIMIZED——会切虚拟桌面）。
bool RestoreMinimizedQuietPreferMax(HWND hwnd);
/// 顶层窗是否已接近监视器工作区尺寸（安静铺满后的判据，替代 IsZoomed）。
bool WindowFillsWorkArea(HWND hwnd, int slackPx = 48);
/// Chrome/Edge 顶层不支持假焦点注入。
bool IsBrowserFakeFocusUnsupported(HWND hwnd);

/// CDP：Pin+屏外出帧；用户在宏桌面且仍需揭开时再 UnPin。
bool EnsureCdpBrowserLiveFrames(HWND hwnd);
/// CDP/扩展：Move「鼠标宏」后 Pin+屏外（禁异桌裸还原，否则鼠标切屏）。
bool ParkCdpBrowserOnMacroDesktop(HWND hwnd);
/// CDP 绑窗最小停放（内部走 ParkCdpBrowserOnMacroDesktop）。
void PrepareMacroDesktopForCdpBind(HWND hwnd);
/// 若目标已偏离「鼠标宏」则安静迁回（默认不强制最小化）。
bool EnsureTargetOnMacroDesktop(HWND hwnd, bool minimizeIfMoved = false);

/// 宏桌面目标：禁 Peek 预览（softMessage 可用；CDP 停放后须 Clear）。
void SuppressMacroDesktopTaskbarPreview(HWND hwnd);
bool IsMacroDesktopTaskbarPreviewSuppressed(HWND hwnd);
void ClearMacroDesktopTaskbarPreviewSuppression(HWND hwnd = nullptr);

/// 强制揭开 DWM Cloak / 近透明 Layered。
void ForceRevealMacroDesktopWindow(HWND hwnd);
/// 用户在「鼠标宏」观看：揭 Cloak（不抢前台激活）。仅当仍有隐身时生效。
void RaiseMacroDesktopWindowForWatch(HWND hwnd);

/// 找图 latch（遗留 softMessage Cloak 路径）；CDP ≥1.1.39 不 latch。
bool IsMacroVisionLatched(HWND hwnd);
bool IsMacroVisionCaptureReady(HWND hwnd);
bool IsMacroVisionInvisibilityActive(HWND hwnd);
void ReleaseMacroDesktopVisionLatch(HWND hwnd = nullptr);
void RestoreMacroDesktopWindowAfterRun(HWND hwnd);

/// 用户当前是否在「鼠标宏」虚拟桌面（只读，不切桌）。
bool UserOnMacroDesktopNow();

/// CDP 运行期：仅当用户已在「鼠标宏」且仍有隐身时 Raise；永不 GoTo。
void StartCdpMacroDesktopWatchPump(HWND hwnd);
/// 仅置停止标志（热键 UI 线程可用）；不 join。
void SignalStopCdpMacroDesktopWatchPump();
/// 置停止并 join（EndRun / 析构路径）。
void StopCdpMacroDesktopWatchPump();
/// 已废止：持续 GoTo 钉视。保留符号，调用为空操作。
void ScheduleCdpParkViewPin(int preferDesk, int durationMs = 5000);

/// 找图/OCR：宏桌面统一先最小化再无感展开 + GDI；结束后还原，若误切桌面则切回。
class ScopedVisionCapturePrep {
public:
    ScopedVisionCapturePrep(HWND hwnd, bool backgroundMode);
    ~ScopedVisionCapturePrep();
    ScopedVisionCapturePrep(const ScopedVisionCapturePrep&) = delete;
    ScopedVisionCapturePrep& operator=(const ScopedVisionCapturePrep&) = delete;

    bool Ready() const { return ready_; }
    HWND RootHwnd() const { return root_; }

private:
    HWND root_ = nullptr;
    bool background_ = false;
    bool ready_ = false;
    bool restoreOnDestroy_ = false;
    WINDOWPLACEMENT savedWp_{};
    LONG savedExStyle_ = 0;
    BYTE savedLayeredAlpha_ = 255;
    DWORD savedLayeredFlags_ = 0;
    bool layeredTouched_ = false;
    bool animationDisabled_ = false;
    ANIMATIONINFO savedAnim_{};
    int userDeskAtStart_ = -1;
};

WindowModeHealth EvaluateTargetHealth(HWND hwnd, HDC probeDc = nullptr);

}  // namespace windowmode
