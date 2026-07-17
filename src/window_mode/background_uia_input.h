#pragma once

#include <windows.h>

#include <string>

namespace windowmode {

bool SendQuickInputViaUiAutomation(HWND hwnd, const std::wstring& text);

}  // namespace windowmode
