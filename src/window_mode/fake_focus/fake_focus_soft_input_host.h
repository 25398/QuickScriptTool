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
void FakeFocusSoftInput_ClearKeys();
void FakeFocusSoftInput_Reset();

}  // namespace windowmode
