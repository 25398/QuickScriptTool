#pragma once

#include <windows.h>

#include <string>

namespace windowmode {

/// Host-side writer for FakeFocus soft input shared memory (Phase 2).
/// Create before DLL Install so the target can OpenFileMapping.
bool FakeFocusSoftInput_Attach(DWORD targetPid, std::wstring& err);
void FakeFocusSoftInput_Detach();
bool FakeFocusSoftInput_IsAttached();

void FakeFocusSoftInput_SetCursorScreen(int sx, int sy);
void FakeFocusSoftInput_SetMouseButtonVk(UINT vk, bool down);
void FakeFocusSoftInput_SetKey(UINT vk, bool down);
/// Chromium 壳：滚轮也进进程内队列（勿宿主外 PostMessage）。
void FakeFocusSoftInput_PushWheel(bool vertical, bool positive, int steps);
/// 共享内存里滚轮的**写入 / 消费**游标（诊断 + 自检用）。
///
/// ⚠ 这两个数就是"滚轮到底有没有发出去、目标有没有取走"的**唯一**硬证据 ——
///   2026-09-30 那次「滚动不能正常滚动」里，`PushWheel` 因为一个标志没置位
///   直接 return，而日志照样打"假焦点软滚轮 …"，从日志完全看不出问题。
///   现在：`write` 不涨 = 宿主没投；`write` 涨而 `read` 不涨 = 目标没取（DLL 没装钩/版本旧）。
/// @return false = 没挂上共享内存（未注入/已卸载），此时两个出参不动
bool FakeFocusSoftInput_WheelCursors(uint32_t& write, uint32_t& read);
/// Electron 真后台：按有序事件队列在窗口 PID 内 PostMessage 灌键（禁 Peek 伪造）。
void FakeFocusSoftInput_SetPostKeyEvents(bool enabled);
bool FakeFocusSoftInput_PostKeyEventsEnabled();
void FakeFocusSoftInput_ClearKeys();
void FakeFocusSoftInput_Reset();

/// 读目标进程变速 detour 的调用计数（DLL 写入共享内存，宿主直接读）。
/// out 至少要 count 个元素（当前 6：QPC/GetTickCount64/GetTickCount/
/// GetSystemTimeAsFileTime/timeGetTime/保留）。返回 false = 共享内存未挂载。
bool FakeFocusSoftInput_TimeHookCalls(uint32_t* out, int count);

/// 读 Unity(IL2CPP) 变速状态（编码见 fake_focus_unity_timescale.h 的 State()）。
/// 未挂载共享内存时返回 0。
uint32_t FakeFocusSoftInput_UnityState();

/// 窗口变速（变速齿轮）：把目标进程的时钟倍率下发给注入的 DLL。
/// speed = 1.0 原速（挂钩保留但原样转发）；0 或负数 = 关闭并让 DLL 卸载变速钩。
/// 返回 false 表示当前没有可写的共享内存（未注入 / 已卸载）。
bool FakeFocusSoftInput_SetTimeScale(double speed);
/// 读回当前下发的倍率（0 = 关闭）。无共享内存时返回 0。
double FakeFocusSoftInput_TimeScale();

/// 读 DLL 写回的冒险岛钩命中（共享内存，不 CreateRemoteThread）。
bool FakeFocusSoftInput_ReadMapleHits(DWORD& gaks, DWORD& diState, DWORD& diData, DWORD& lastCb,
    DWORD& hitReady, DWORD& gfw, DWORD& focus);
/// 读共享内存里的**假光标**（DLL 的 `GetCursorPos` 钩子返回值）+ 两个关键标志位。
/// 用来区分「宿主没喂光标」和「喂了但游戏不读」—— 前者是宿主缺陷，后者才该换方案。
/// @return false = 未挂共享内存（出参不动）
bool FakeFocusSoftInput_ReadSoftCursor(int& x, int& y, bool& cursorValid, bool& postKeyEvents);
/// 读 DLL 安装诊断（IAT 槽 / mapleDiag / 虚表 found|patched|heap）。
bool FakeFocusSoftInput_ReadMapleInstall(DWORD& diag, DWORD& iatPoll, DWORD& diVt);
/// 冒险岛输入路径体检：消息泵调用数 + WM_INPUT/WM_KEYDOWN/WM_ACTIVATE 计数
/// （区分 Raw Input / 消息驱动 / 激活态门控）+ 周期补挂的轮次与新增槽数。
bool FakeFocusSoftInput_ReadMaplePathProbe(DWORD& pump, DWORD& msgInput, DWORD& msgKey,
    DWORD& msgActivate, DWORD& rescanAdds, DWORD& rescanRounds);
/// 当前软键按下个数（共享内存 down[]）；未挂共享内存返回 -1。
int FakeFocusSoftInput_DownKeyCount();
/// 单个键当前是否处于"逻辑按下"（共享内存 down[vk]）；未挂共享内存返回 false。
/// 自检用：钉住"陈旧位会被清 / 恢复后会重发"必须能精确断言**是哪个键**，
/// 只看总数会把"清错了键、又置回了别的键"判成通过。
bool FakeFocusSoftInput_IsKeyDown(UINT vk);

}  // namespace windowmode
