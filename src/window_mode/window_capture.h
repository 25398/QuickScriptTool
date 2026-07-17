#pragma once

#include <windows.h>

namespace windowmode {

struct WindowCaptureResult {
    HBITMAP bitmap = nullptr;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool fromPrintWindow = true;
};

WindowCaptureResult CaptureWindowClientGdi(HWND hwnd);
WindowCaptureResult CaptureWindowClient(HWND hwnd);
WindowCaptureResult CaptureWindowRegion(HWND hwnd, int cx1, int cy1, int cx2, int cy2);
HBITMAP CropBitmapScreenRegion(HBITMAP src, int srcOriginX, int srcOriginY,
    int regionX1, int regionY1, int regionX2, int regionY2);
HBITMAP CropBitmapClientRegion(HBITMAP src, int cx1, int cy1, int cx2, int cy2);
bool IsCaptureLikelyBlank(HBITMAP bmp);

}  // namespace windowmode
