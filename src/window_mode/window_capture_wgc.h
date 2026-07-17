#pragma once

#include <windows.h>

namespace windowmode {

bool IsWgcCaptureAvailable();
HBITMAP CaptureWindowWgc(HWND hwnd, int& outW, int& outH);

}  // namespace windowmode
