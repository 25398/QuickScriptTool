#pragma once

#include <windows.h>

namespace windowmode {

bool ClientToScreenPoint(HWND hwnd, int cx, int cy, int& sx, int& sy);
bool ScreenToClientPoint(HWND hwnd, int sx, int sy, int& cx, int& cy);
bool MapClientRectToScreen(HWND hwnd, int cx1, int cy1, int cx2, int cy2,
                           int& sx1, int& sy1, int& sx2, int& sy2);
bool ScreenSearchRectToClientRect(HWND hwnd, int sx1, int sy1, int sx2, int sy2,
                                  int& cx1, int& cy1, int& cx2, int& cy2);

}  // namespace windowmode
