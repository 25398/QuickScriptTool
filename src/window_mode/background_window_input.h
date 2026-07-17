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
void PostMouseMoveToWindow(HWND hwnd, int cx, int cy);
void PostMouseButtonToWindow(HWND hwnd, int cx, int cy, MouseButtonType button, bool down);
void PostScrollWheelToWindow(HWND hwnd, int cx, int cy, int steps, bool vertical, bool positive);

/// Last client position posted via soft mouse APIs (for GetCursorPos in window modes).
bool GetLastSoftMouseClientPos(HWND hwnd, int& cx, int& cy);
void ResetSoftMouseState();

}  // namespace windowmode
