#pragma once

#include <windows.h>

namespace windowmode {

bool ClientToScreenPoint(HWND hwnd, int cx, int cy, int& sx, int& sy);
bool ScreenToClientPoint(HWND hwnd, int sx, int sy, int& cx, int& cy);
/// 屏幕坐标 → 客户区坐标；目标点落在客户区外时返回 false 且不改写输出。
bool ScreenPointToClientWithin(HWND hwnd, int sx, int sy, int& cx, int& cy);
bool MapClientRectToScreen(HWND hwnd, int cx1, int cy1, int cx2, int cy2,
                           int& sx1, int& sy1, int& sx2, int& sy2);
bool ScreenSearchRectToClientRect(HWND hwnd, int sx1, int sy1, int sx2, int sy2,
                                  int& cx1, int& cy1, int& cx2, int& cy2);

/// 窗口相对坐标：按录制客户区 → 当前客户区缩放。尺寸无效或比例≈1 时不改坐标，返回 false。
bool ScaleWindowClientPoint(int recordW, int recordH, int liveW, int liveH, int& x, int& y);
void ScaleWindowClientRect(int recordW, int recordH, int liveW, int liveH,
    int& x1, int& y1, int& x2, int& y2);

/// 窗口/后台窗口模式找图：忽略脚本里的绝对「选取区域」，始终用整个客户区。
/// 「根据图片选取区域」在命中后再用 ApplyImageRegionToMatch 二次筛选。
bool EffectiveWindowModeClientSearchRect(int clientW, int clientH,
    int& x1, int& y1, int& x2, int& y2);

}  // namespace windowmode
