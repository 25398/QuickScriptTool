#include "window_coords.h"

#include <algorithm>
#include <cmath>

namespace windowmode {

bool ClientToScreenPoint(HWND hwnd, int cx, int cy, int& sx, int& sy) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    POINT pt{cx, cy};
    if (!ClientToScreen(hwnd, &pt)) return false;
    sx = pt.x;
    sy = pt.y;
    return true;
}

bool ScreenToClientPoint(HWND hwnd, int sx, int sy, int& cx, int& cy) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    POINT pt{sx, sy};
    if (!ScreenToClient(hwnd, &pt)) return false;
    cx = pt.x;
    cy = pt.y;
    return true;
}

bool ScreenPointToClientWithin(HWND hwnd, int sx, int sy, int& cx, int& cy) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;
    POINT pt{sx, sy};
    if (!ScreenToClient(hwnd, &pt)) return false;
    if (pt.x < 0 || pt.y < 0 || pt.x >= client.right || pt.y >= client.bottom) {
        return false;
    }
    cx = pt.x;
    cy = pt.y;
    return true;
}

bool MapClientRectToScreen(HWND hwnd, int cx1, int cy1, int cx2, int cy2,
                           int& sx1, int& sy1, int& sx2, int& sy2) {
    const int L = std::min(cx1, cx2);
    const int T = std::min(cy1, cy2);
    const int R = std::max(cx1, cx2);
    const int B = std::max(cy1, cy2);
    return ClientToScreenPoint(hwnd, L, T, sx1, sy1)
        && ClientToScreenPoint(hwnd, R, B, sx2, sy2);
}

bool ScreenSearchRectToClientRect(HWND hwnd, int sx1, int sy1, int sx2, int sy2,
                                  int& cx1, int& cy1, int& cx2, int& cy2) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    const int sL = std::min(sx1, sx2);
    const int sT = std::min(sy1, sy2);
    const int sR = std::max(sx1, sx2);
    const int sB = std::max(sy1, sy2);
    if (sR <= sL || sB <= sT) return false;

    int cL = 0, cT = 0, cR = 0, cB = 0;
    if (!ScreenToClientPoint(hwnd, sL, sT, cL, cT)) return false;
    if (!ScreenToClientPoint(hwnd, sR, sB, cR, cB)) return false;

    cx1 = std::min(cL, cR);
    cy1 = std::min(cT, cB);
    cx2 = std::max(cL, cR);
    cy2 = std::max(cT, cB);
    return cx2 > cx1 && cy2 > cy1;
}

bool ScaleWindowClientPoint(int recordW, int recordH, int liveW, int liveH, int& x, int& y) {
    if (recordW <= 0 || recordH <= 0 || liveW <= 0 || liveH <= 0) return false;
    const double sx = static_cast<double>(liveW) / static_cast<double>(recordW);
    const double sy = static_cast<double>(liveH) / static_cast<double>(recordH);
    if (std::fabs(sx - 1.0) < 0.02 && std::fabs(sy - 1.0) < 0.02) return false;
    x = static_cast<int>(std::lround(x * sx));
    y = static_cast<int>(std::lround(y * sy));
    return true;
}

void ScaleWindowClientRect(int recordW, int recordH, int liveW, int liveH,
    int& x1, int& y1, int& x2, int& y2) {
    ScaleWindowClientPoint(recordW, recordH, liveW, liveH, x1, y1);
    ScaleWindowClientPoint(recordW, recordH, liveW, liveH, x2, y2);
}

}  // namespace windowmode
