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
#include <unordered_set>
#include <vector>

namespace windowmode {

/// ── 冒险岛"键态停摆 / 恢复"（2026-10-01）────────────────────────────────
/// 现场判据：后台坏状态是「消息在泵（`泵=172 WM_KEY=36`）但**键态一次都不查**（`gaks=0 吞=0`）」，
/// 攻击（消息驱动）照打、移动（轮询键态）读不到；而客户端在"还按着 ←"时停了轮询，
/// 就永远看不到那次松开 ⇒ 恢复后一路顶墙"到头了还走"。
/// 处置：停摆期清掉共享内存里的**方向键陈旧位**，恢复轮询后按脚本持键**重新对齐**一次。
/// **不动投递流**（脚本的每个按下/抬起照发）。
/// ⚠⚠ 2026-10-02 修正：**陈旧位 ≠ 脚本正按着的键**。清掉脚本意图等于凭空丢一次按下，
/// 而"恢复"判据可能永远不触发（旧 DLL 把计数夹在 255，见 `EvaluateKeyStatePhase`）
/// ⇒ 这次按下永久丢失，用户看到「角色原地打、然后乱走」。故 `ClearStaleArrowSoftKeys`
/// 必须排除 `held`（见 `window_mode_requirements.h` 第 18 条）。
enum class KeyStatePhase {
    None,
    Stalled,
    Resumed,
};

/// 纯判据：GASK 计数没变且此前未判停摆 ⇒ Stalled；变了且此前判过 ⇒ Resumed。
/// ⚠ 计数**夹在旧版钳位值 255** 时判据失效 ⇒ 返回 None（不判），见实现处说明。
/// 自检逐格钉住（`maple_keystate_stall_resync`）。
KeyStatePhase EvaluateKeyStatePhase(DWORD prevGaks, DWORD nowGaks, bool wasStalled);
/// 清共享内存里方向键的**陈旧**按下位（**排除** `held` = 脚本此刻按着的键）；返回清掉个数。
int ClearStaleArrowSoftKeys(const std::unordered_set<UINT>& held);
/// 按 `held` 重新对齐（持键补按下、方向键里没持的置抬起）；返回补发按下个数。
int ResyncSoftHeldKeys(const std::vector<UINT>& held);

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

    /// 诊断用：相对移动当前会走哪条路（SendInput / 进程内软输入 / 跨进程软输入）。
    /// 相对移动在 `MoveMouseRelativeClient` 里**没有任何日志**，落点与卡顿排查时
    /// 只能靠 `SendInput ok=` 间接猜；把它暴露成只读查询，让引擎打一行路径证据。
    const wchar_t* RelativeMoveRouteName() const;

    void SetCoordMeta(const CoordMeta& meta) { coordMeta_ = meta; }
    const CoordMeta& GetCoordMeta() const { return coordMeta_; }

    /// 假焦点注入技术（默认 ClassicRemoteThread；对抗性测试用，见
    /// docs/anticheat-injection-testing.md）。
    void SetInjectionTechnique(inject::Technique t) { fakeFocus_.SetInjectionTechnique(t); }
    void SetHideInjectedModule(bool hide) { fakeFocus_.SetHideModule(hide); }
    /// 全局设置：关闭后 TryInstallFakeFocus 不注入 DLL（微信 4.x 后台仍强制精简注入）。
    void SetEnableFakeFocusInjection(bool enable) { enableFakeFocusInjection_ = enable; }

    /// 全局设置：启用「窗口变速」（回放倍速同步作用于目标窗口时钟）。
    /// 注意它**不**依赖「启用假焦点注入」：注入关掉时，本开关会让窗口/后台窗口模式改为
    /// 仅注入时钟补丁（FakeFocus_InstallTimeScaleOnly），键鼠路径保持不变。
    void SetEnableWindowTimeScale(bool enable) { enableWindowTimeScale_ = enable; }

    /// 窗口变速（变速齿轮）：把目标进程的时钟倍率下发给注入的假焦点 DLL。
    /// speed = 1.0 原速（挂钩保留但原样转发）；<=0 关闭并让 DLL 卸载变速钩。
    /// 只在「后台窗口模式 + 已注入 + 会话仍活跃」时真的下发，其余情况返回 false。
    bool SetWindowTimeScale(double speed);
    /// 当前是否具备窗口变速的下发条件（诊断/自检用）。
    bool CanWindowTimeScale() const;

    /// 变速下发后延迟一次远程诊断，把 DLL 侧「补了多少槽 / 当前倍率」打进日志。
    /// 没有它，用户报「还是不变速」时只能靠猜 —— 上一轮就卡在这里。
    /// 开销：每轮回放最多一次远程调用（打印过就置位不再查）。
    void MaybeLogTimeScaleDiag();

    /// 假焦点钩**真的**装上了吗？「仅变速注入」不算 —— 那种注入只改时钟，一个假焦点钩都没装。
    /// 所有**输入路径**判定都必须用它，而不是 fakeFocus_.IsInjected()：
    /// 否则仅变速注入会被误判成「假焦点可用」，键鼠被切到根本不存在的钩子路径上，直接全丢。
    bool FakeFocusActive() const;

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
    /// 回退本机（假前台）输入前撤掉假焦点。
    /// **不要**在这里直接 `fakeFocus_.Unload()`：Unload 会把 DLL 一起 FreeLibrary，
    /// 窗口变速（时钟补丁）就跟着没了 —— 用户实测「全屏游戏里第一下移动光标后变速即失效」。
    /// 现在改成「只拆钩、保时钟」，只有拆不动（DLL 版本旧）或没开变速时才真的卸载。
    void DropFakeFocusForHardwareInput(const wchar_t* reason);
    /// 本机 SendInput 必须落在目标前台（UE5/RDP）；失焦时重新激活。
    bool EnsureHardwareInputFocus();
    /// 仅真铺满监视器用相对位移；窗口化 UE5 / RDP 用绝对 SetCursorPos。
    bool UsesRelativeHardwareCursor() const;
    /// 配置 exe/类名或 HWND 判定为 mstsc 回放目标。
    bool IsRemoteDesktopPlaybackTarget() const;
    void RestoreHardwareOffscreenPark();

    /// `PrepareSoftInput` 的快速路径：绑定与几何未变时跳过全树枚举。
    /// 用于**每拍一次**的热路径：相对/绝对移动、鼠标按放、点击、滚轮、键盘。
    /// 快捷输入（QuickInput）仍走完整路径 —— 它不是逐拍动作，且自带一次 RefreshInputBinding。
    /// ⚠ 2026-10-04：本函数是**薄包装** —— 失败时统一落盘报一次（见实现），
    ///   真正干活的是 `PrepareSoftInputFastImpl`。收口的原因：它有 5 个调用点，
    ///   其中 4 个原来直接 `return`（一行日志都没有）、剩下的写 `WindowModeLogf`（不落盘）。
    bool PrepareSoftInputFast(std::wstring& err);
    bool PrepareSoftInputFastImpl(std::wstring& err);
    /// 快速路径的缓存失效（重新绑定目标 / EndRun 时必须清）。
    void InvalidateSoftInputFastPath() {
        fastPreparedHwnd_ = nullptr;
        fastPreparedTopClass_.clear();
    }

    HWND fastPreparedHwnd_ = nullptr;
    int fastPreparedClientW_ = 0;
    int fastPreparedClientH_ = 0;
    /// 缓存时的顶层窗类名。窗口句柄可能被系统复用（关掉旧窗再开新窗拿到同一 HWND 值），
    /// 单靠 HWND 相等会误判「还是那个目标」⇒ 拿旧绑定去投递。类名一起比，成本是
    /// 一次 `GetClassNameW`（纯用户态读，无跨进程），远比 `EnumChildWindows` 便宜。
    std::wstring fastPreparedTopClass_;

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
    /// 「软输入未就绪 ⇒ 移动/点击被**跳过**」是否已报过（热路径限流，只报第一次）。
    /// ⚠ 这类跳过原来走 `WindowModeLogf`（**非 Event ⇒ 不落盘**）⇒ 用户导出的日志里
    ///   只看到「移动 → 客户区」那行**没出现**，却不知道为什么 —— 判据是「用户能导出」。
    bool softInputSkipLogged_ = false;
    /// 本机输入回放开始前的前台窗；EndRun 时尽量还回去。
    HWND rdpSavedForeground_ = nullptr;
    bool hwOffscreenParked_ = false;
    HWND hwParkedHwnd_ = nullptr;
    WINDOWPLACEMENT hwSavedWp_{};
    bool hwSavedTopmost_ = false;
    bool enableFakeFocusInjection_ = true;
    bool enableWindowTimeScale_ = false;
    /// 变速下发时刻 / 诊断输出节流（供 MaybeLogTimeScaleDiag 用）。
    ULONGLONG timeScaleAppliedTick_ = 0;
    bool timeScaleActive_ = false;
    ULONGLONG timeScaleNextLogTick_ = 0;
    int timeScaleLoggedTimes_ = 0;
    /// 「目标主循环节拍」探针：消息泵调用次数的相邻两次差值（判别时钟倍率有没有
    /// 真的作用到游戏速度，见 MaybeLogTimeScaleDiag 注释）。
    DWORD timeScaleLastPump_ = 0;
    ULONGLONG timeScaleLastPumpTick_ = 0;
    /// 冒险岛后台会话看门狗（A/C，2026-09-29）：下次检查时刻、连续叫不醒次数、上次身份 hwnd。
    ULONGLONG mapleWatchdogNextTick_ = 0;
    int mapleWatchdogMisses_ = 0;
    HWND mapleIdentityHwnd_ = nullptr;
    /// 客户端停摆中（②：停摆期只发抬起、不发方向键按下）与"每秒重试叫醒"节流。
    bool mapleClientDormant_ = false;
    ULONGLONG mapleDormantRetryTick_ = 0;
    /// 自动叫醒只做**会话第一次**（见 BeginRun 的注释；每轮抢前台会打断用户打字）。
    bool mapleWokeOnce_ = false;
    /// 键态停摆/恢复（2026-10-01）：上次读到的 GAKS 计数、是否已判停摆、脚本持键影子集。
    DWORD keyStateLastGaks_ = 0;
    bool keyStateStalled_ = false;
    std::unordered_set<UINT> softHeldKeys_;
    bool hardwareFallback_ = false;
    /// 「后台模式没拿到假焦点，已拒绝回退假前台 SendInput」只告警一次。
    ///
    /// ⚠ 用**成员**而不是函数内 static：本类的实例在自检里被反复构造/析构，
    ///   函数内 static 会把"已告警"带过会话边界（第二次会话就静默了），
    ///   而这条告警恰恰是用户唯一的解释来源。
    ///
    /// ⚠ `mutable` 是必需的：判据本体 `PreferHardwareInput()` 是 `const`
    ///   （它在热路径上被反复调用，不该要非 const 访问），而"告警一次"是**观测副作用**，
    ///   不改变输入决策本身 —— 与 `mutable std::mutex` 同一性质。
    mutable bool backgroundNoFakeFocusWarned_ = false;
};

}  // namespace windowmode
