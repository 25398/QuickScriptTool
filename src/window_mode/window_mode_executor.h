#pragma once

#include "window_mode_session.h"
#include "window_mode_types.h"
#include "cdp/cdp_input.h"
#include "ext_bridge/ext_input.h"
#include "fake_focus/fake_focus_injector.h"

#include "coord_space.h"
#include "image_match.h"
#include "ocr_engine.h"
#include "script_types.h"

#include <atomic>
#include <functional>
#include <string>

namespace windowmode {

struct BeginRunOptions {
    bool launchTarget = true;
    const std::atomic_bool* cancelFlag = nullptr;
    std::wstring launchSearchDir;
};

class WindowModeExecutor {
public:
    // 构造/析构必须在单一 TU 定义，避免头文件 =default 多处内联时与成员顺序/布局漂移。
    WindowModeExecutor();
    ~WindowModeExecutor();

    static bool CheckRunHealth(const WindowModeScriptConfig& config, std::wstring& err);

    bool BeginRun(const WindowModeScriptConfig& config, std::wstring& err,
        BeginRunOptions options = {});
    void EndRun();
    /// 热键停止时打断扩展桥等待（FindImage/CDP 请求中也能尽快退出）。
    static void NotifyCancel();
    bool IsActive() const { return active_; }
    bool UsesBackgroundWindow() const;
    bool IsCdpInputMode() const;
    /// 已成功执行的 UIA 兜底点击次数（诊断/自检用）。
    int UiaInvokeCount() const { return uiaInvokeCount_; }

    void SetCoordMeta(const CoordMeta& meta) { coordMeta_ = meta; }
    const CoordMeta& GetCoordMeta() const { return coordMeta_; }

    /// 假焦点注入技术（默认 ClassicRemoteThread；对抗性测试用，见
    /// docs/anticheat-injection-testing.md）。
    void SetInjectionTechnique(inject::Technique t) { fakeFocus_.SetInjectionTechnique(t); }
    void SetHideInjectedModule(bool hide) { fakeFocus_.SetHideModule(hide); }
    /// 全局设置：关闭后 TryInstallFakeFocus 不注入 DLL（微信 4.x 后台仍强制精简注入）。
    void SetEnableFakeFocusInjection(bool enable) { enableFakeFocusInjection_ = enable; }

    HWND TargetHwnd() const;
    WindowModeHealth Health() const;
    /// 绑窗后目标 HWND 仍在、且输入窗进程未退出。游戏闪退后必须停脚本。
    /// UWP：输入窗 PID 与 ApplicationFrameHost 不同，不算闪退。
    bool TargetStillAlive() const;
    std::wstring TargetAliveDebug() const;

    bool RefreshTarget(std::wstring& err);

    /// 目标当前铺满监视器（UE5 独占全屏等）：假焦点会冻画面，改走本机 SendInput。
    /// 窗口化 UE5 在未注入时同样走本机输入（PostMessage 无法驱动 Raw Input）。
    bool PreferHardwareInput() const;
    void MoveMouseRelativeClient(int dx, int dy);

    void MoveMouseClient(int cx, int cy, int randomX, int randomY,
        const std::function<int(int)>& randomInt, bool scaleRecordedClient = true);
    void PostMouseButtonAtClient(int cx, int cy, MouseButtonType button, bool down,
        bool scaleRecordedClient = true);
    void PostMouseClickAtClient(int cx, int cy, MouseButtonType button,
        bool scaleRecordedClient = true);
    void PostKeyToTarget(UINT vk, bool down);
    void PostScrollWheelAtClient(int cx, int cy, int steps, bool vertical, bool positive,
        bool scaleRecordedClient = true);
    void SendQuickInputToTarget(const std::wstring& text, double charInterval);

    bool ResolveClientSearchRect(const ScriptAction& a, int& x1, int& y1, int& x2, int& y2) const;
    bool MapClientRect(int cx1, int cy1, int cx2, int cy2,
        int& sx1, int& sy1, int& sx2, int& sy2) const;

    ImageMatchOutput FindImageClient(const ScriptAction& a,
        HBITMAP lockedBmp, int lockX, int lockY);
    /// 窗口相对找图：模板缩放 = 当前客户区 / 录制客户区（与点击坐标等比拉伸同一套）。
    TemplateScale FindImageTemplateScale() const;

    bool LockWindowCapture(HBITMAP& outBmp, int& outX, int& outY);
    HBITMAP CaptureScreenRegionFromWindow(int sx1, int sy1, int sx2, int sy2,
        HBITMAP lockedBmp, int lockX, int lockY);
    bool ResolveAiScreenRect(const ScriptAction& a, int& sx1, int& sy1, int& sx2, int& sy2,
        HBITMAP lockedBmp, int lockX, int lockY);
    OcrEngineOutput RunOcrOnClientRegion(const ScriptAction& a,
        HBITMAP lockedBmp, int lockX, int lockY);
    bool GetCursorClientPos(int& cx, int& cy) const;

    WindowModeSession& Session() { return session_; }
    const WindowModeSession& Session() const { return session_; }

    /// 诊断用：复现 FindImageClient 的 EnsureTargetReady + VisionPrep + Capture，不写文件。
    struct VisionPipelineDiag {
        bool ensureReadyOk = false;
        bool prepReady = false;
        bool captureOk = false;
        bool captureBlank = false;
        int captureW = 0;
        int captureH = 0;
        std::wstring ensureErr;
    };
    VisionPipelineDiag DiagnoseVisionPipeline();

private:
    bool MoveToScreen(int sx, int sy);
    bool EnsureTargetReady(std::wstring& err);
    bool EnsureTargetBound(std::wstring& err);
    /// Bind / quietly restore for message injection — never expands macro-desktop windows.
    bool PrepareSoftInput(std::wstring& err);
    bool PrepareVisionCapture();
    bool EnsureCdpReady(std::wstring& err);
    /// 目标已非最小化时，让扩展重测壳页 iframe 布局（不还原窗口）。
    void MaybeRefreshExtLayout();
    void UpdateExtSurfaceSize();
    /// 脚本/找图点在输入窗客户区；扩展 surface 取自截图控件（常为 Intermediate D3D）。
    /// 投递网页键鼠前必须对齐到截图控件客户区，否则 iframe 映射会偏。
    void ToCaptureSurfaceClient(int& cx, int& cy) const;
    bool ResolveOcrScreenRect(const ScriptAction& a, int& sx1, int& sy1, int& sx2, int& sy2,
        HBITMAP lockedBmp, int lockX, int lockY);
    bool UsesClientCoords() const;
    HWND CaptureTargetHwnd() const;
    HWND VisionCaptureHwnd() const;
    void ResolveClickClientPos(int& cx, int& cy) const;
    /// 脚本坐标 → 目标窗口客户区坐标。
    /// 窗口相对脚本：按录制客户区→当前客户区缩放（找图/OCR 落点传 scaleRecordedClient=false）。
    /// 屏幕绝对脚本执行「屏幕→客户区」映射，目标点落在客户区外时返回 false。
    /// (0,0) 保留「当前位置」语义，不做转换。
    bool MapScriptPointToClient(int& cx, int& cy, bool scaleRecordedClient = true) const;
    bool RecordedClientSize(int& w, int& h) const;
    bool LiveClientSize(int& w, int& h) const;
    TemplateScale FindImageSurfaceScale(int surfaceW, int surfaceH) const;
    /// 最小化目标：置底安静还原，不抢前台。供回放键鼠/假焦点使用。
    bool EnsurePlaybackGeometry(std::wstring& err);
    /// 仅 UWP/WinUI 宿主：UIA Invoke 兜底。Win32/Unity 禁止走这条（会抢前台）。
    /// 成功返回 true；调用方应跳过后续按键消息并取消配对的 Up 消息。
    bool TryUiaClickAtClient(int cx, int cy);
    /// Unity/游戏：注入 FakeFocus DLL，并把软光标/按键写入共享内存。失败只打日志，不中止回放。
    void TryInstallFakeFocus();
    /// Chromium/Electron/CEF 壳且假焦点已注入：键鼠走进程内队列（勿宿主外 PostMessage）。
    bool UsesChromiumShellInProcInput() const;
    /// 恒 false：冒险岛技能键必须 PostMessage。走路靠 mapleSafe 注入写共享内存/DI，不走这条。
    bool UsesMapleStoryFakeFocusInput() const;
    bool UsesInProcFakeFocusSoftInput() const;
    void SyncFakeFocusCursor(int cx, int cy) const;
    void SyncFakeFocusMouseButton(MouseButtonType button, bool down) const;
    bool SendHardwareCursorToClient(int cx, int cy);
    /// 本机 SendInput 必须落在目标前台（UE5/RDP）；失焦时重新激活。
    bool EnsureHardwareInputFocus();
    /// 仅真铺满监视器用相对位移；窗口化 UE5 / RDP 用绝对 SetCursorPos。
    bool UsesRelativeHardwareCursor() const;
    /// 配置 exe/类名或 HWND 判定为 mstsc 回放目标。
    bool IsRemoteDesktopPlaybackTarget() const;
    void RestoreHardwareOffscreenPark();

    WindowModeSession session_;
    FakeFocusInjector fakeFocus_;
    CdpInputSession cdp_;
    ExtInputSession ext_;
    bool active_ = false;
    bool extLayoutFresh_ = false;
    bool uiaInvokePending_ = false;
    int uiaInvokeCount_ = 0;
    const std::atomic_bool* cancelFlag_ = nullptr;
    CoordMeta coordMeta_{};
    bool wasMinimizedAtBeginRun_ = false;
    mutable bool loggedClientScale_ = false;
    /// 本机输入回放开始前的前台窗；EndRun 时尽量还回去。
    HWND rdpSavedForeground_ = nullptr;
    bool hwOffscreenParked_ = false;
    HWND hwParkedHwnd_ = nullptr;
    WINDOWPLACEMENT hwSavedWp_{};
    bool hwSavedTopmost_ = false;
    bool enableFakeFocusInjection_ = true;
    bool hardwareFallback_ = false;
};

}  // namespace windowmode
