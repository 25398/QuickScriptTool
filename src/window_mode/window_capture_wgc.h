#pragma once

#include <windows.h>

namespace windowmode {

bool IsWgcCaptureAvailable();
HBITMAP CaptureWindowWgc(HWND hwnd, int& outW, int& outH);
// 整屏合成桌面采集（微信截图同原理）：能拍到 GDI BitBlt 物理上拍不到的
// DirectComposition 表面——如 TSF 输入法候选框/组字框。hMon 为要抓的显示器，
// 为 null 时抓主显示器。失败返回 nullptr（调用方回退 BitBlt）。
HBITMAP CaptureMonitorWgc(HMONITOR hMon, int& outW, int& outH);

}  // namespace windowmode
