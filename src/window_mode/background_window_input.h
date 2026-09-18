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

/// Last client position posted via soft mouse APIs (for GetCursorPos in window modes).
bool GetLastSoftMouseClientPos(HWND hwnd, int& cx, int& cy);
void ResetSoftMouseState();
/// CDP/扩展：Move 后记住客户区坐标，供后续「点击当前位置」(0,0) 使用。
void RememberSoftMouseClientPos(HWND hwnd, int cx, int cy);
/// 桌面模拟器拖拽：按下期间须发 MOVE（DeSmuME 靠 MK_LBUTTON+MOVE 更新触摸）。
bool SoftMouseButtonHeld();

}  // namespace windowmode
