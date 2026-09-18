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
/// Electron 真后台：按有序事件队列在窗口 PID 内 PostMessage 灌键（禁 Peek 伪造）。
void FakeFocusSoftInput_SetPostKeyEvents(bool enabled);
bool FakeFocusSoftInput_PostKeyEventsEnabled();
void FakeFocusSoftInput_ClearKeys();
void FakeFocusSoftInput_Reset();

/// 读 DLL 写回的冒险岛钩命中（共享内存，不 CreateRemoteThread）。
bool FakeFocusSoftInput_ReadMapleHits(DWORD& gaks, DWORD& diState, DWORD& diData, DWORD& lastCb,
    DWORD& hitReady, DWORD& gfw, DWORD& focus);
/// 读 DLL 安装诊断（IAT 槽 / mapleDiag / 虚表 found|patched|heap）。
bool FakeFocusSoftInput_ReadMapleInstall(DWORD& diag, DWORD& iatPoll, DWORD& diVt);

}  // namespace windowmode
