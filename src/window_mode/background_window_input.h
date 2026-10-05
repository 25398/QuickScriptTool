#pragma once

#include "script_types.h"

#include <atomic>
#include <string>

namespace windowmode {

HWND FindTextInputTarget(HWND root);
void PostQuickInputToWindow(HWND hwnd, const std::wstring& text, double charInterval,
    bool allowForegroundFallback = false,
    const std::atomic_bool* cancelFlag = nullptr);
void SendQuickInputViaForeground(HWND hwnd, const std::wstring& text, double charInterval,
    const std::atomic_bool* cancelFlag = nullptr);
void PostKeyToWindow(HWND hwnd, UINT vk, bool down);
/// 目标窗口（或其顶层）此刻是否就是前台窗。
/// 方向键的本机键态兜底（SendInput）只在目标就是前台时才有意义：目标在后台时
/// SendInput 打的是当前前台窗（用户正在看的浏览器/视频会收到 ←/→/↑/↓），
/// 而目标自己失焦停轮询，照样不走 —— 所以后台一律不补真键。
bool TargetOwnsForegroundWindow(HWND hwnd);
/// 松开本会话用 SendInput 补过 KEYDOWN 的方向键。
/// BeginRun 开头与 EndRun 都要调：脚本中途停止 / 目标闪退 / 用户切走都可能让
/// 某一对 DOWN/UP 不配对，留下**系统级卡键**（游戏朝一个方向一直走）。
void ReleaseMirroredLcaNavKeys();

/// 方向键兜底真键（SendInput）到底发不发。
/// - `down`：**只在目标就是前台窗**时补。后台时 SendInput 打的是当前前台窗
///   （用户正在看的浏览器/视频会收到 ←/→/↑/↓）。
/// - `up`：**只看当初有没有补过**，与此刻是否前台无关。若按「此刻不在前台就不补」跳过，
///   按下时在前台、松开时用户已切走 ⇒ 真键**永久卡在按下状态**：游戏朝那个方向一直走，
///   而且整个系统都认为该键被按住。
inline bool ShouldMirrorNavKeySend(bool down, bool targetOwnsForeground, bool mirroredDown) {
    return down ? targetOwnsForeground : mirroredDown;
}
/// WM_KEYDOWN/UP 的 lParam（方向键扫描码 0x4B 等 + KF_EXTENDED）。
LPARAM BuildWindowKeyLParam(UINT vk, bool down);
/// BeginRun/EndRun：本会话走 LCA 后台窗口消息（假焦点失败回退或未登记游戏）。
void SetLcaBackgroundMessageMode(bool enabled);
/// 假焦点灌键（Chromium 壳/Qt/微信）每投递一笔后等目标处理完再写下一步键态。
/// 软键的**状态**（down[]）与**事件**（DLL 灌键线程 PostMessage）是两条路：宿主一次把
/// DOWN/UP 全写完，目标却在自己的节奏里处理已投递的键消息 —— 修饰键组合（Ctrl+V/Ctrl+C…）
/// 会被目标读成「Ctrl 已抬起」→ 退化成普通字符（症状：只出 v 不粘贴）。
/// 返回 false = 目标没应答（超时/UIPI），调用方可停止后续等待。
bool WaitSoftKeyPostTurn(HWND target, int timeoutMs = 80);
void PostMouseMoveToWindow(HWND hwnd, int cx, int cy);
void PostMouseButtonToWindow(HWND hwnd, int cx, int cy, MouseButtonType button, bool down);
void PostScrollWheelToWindow(HWND hwnd, int cx, int cy, int steps, bool vertical, bool positive);

/// 自检用：鼠标/滚轮的**投递目标选择**（纯选择，不发消息）。
///
/// ★ 为什么必须把它暴露出来：2026-09-30 真机报障「后台窗口滚动没效果」的根因就在
///   **选窗**这一步 —— 绑定的是 `RichEditD2DPT`，实际投给了包着它的容器
///   （`Microsoft.UI.Content.DesktopChildSiteBridge`），日志还如实报「成功=1/1」。
///   而"投出去之后消息落在哪"在我们自建的假 WndProc 上**不可观测**
///   （`SendNotifyMessage` + `DefWindowProc` 的行为不由我们决定），
///   所以用例必须直接钉**选择结果**，而不是去数消息条数。
/// @param cx,cy 进出参：客户区坐标，函数内会换算到被选中窗口的客户区
HWND ResolveMousePostTargetForTest(HWND bound, int& cx, int& cy);

/// Last client position posted via soft mouse APIs (for GetCursorPos in window modes).
bool GetLastSoftMouseClientPos(HWND hwnd, int& cx, int& cy);
void ResetSoftMouseState();
/// CDP/扩展：Move 后记住客户区坐标，供后续「点击当前位置」(0,0) 使用。
void RememberSoftMouseClientPos(HWND hwnd, int cx, int cy);
/// 桌面模拟器拖拽：按下期间须发 MOVE（DeSmuME 靠 MK_LBUTTON+MOVE 更新触摸）。
bool SoftMouseButtonHeld();

}  // namespace windowmode
