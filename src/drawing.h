#pragma once
// ──────────────────────────────────────────────────────────────────
// drawing.h — 图形绘制辅助函数（声明）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <algorithm>
#include <functional>

/// 弹出窗口外扩阴影边距（阴影区域点击穿透）
constexpr int kPopupShadowMargin = 10;

inline int PopupOuterSize(int contentSize) {
    return contentSize + 2 * kPopupShadowMargin;
}

inline void ClientToContentPoint(int& x, int& y) {
    x -= kPopupShadowMargin;
    y -= kPopupShadowMargin;
}

inline bool IsPopupShadowArea(int x, int y, int contentW, int contentH) {
    const int m = kPopupShadowMargin;
    return x < m || y < m || x >= m + contentW || y >= m + contentH;
}

/// 初始化 WS_EX_LAYERED 弹出层窗口
void InitLayeredPopupWindow(HWND hwnd);

/// 将 contentW×contentH 内容缓冲合成阴影并 UpdateLayeredWindow 呈现
bool PresentLayeredPopup(HWND hwnd, HDC contentHdc, int contentW, int contentH);

/// 尝试 layered 透明阴影呈现；失败则回退为普通 BitBlt（fallbackDc 为窗口 DC）
bool PresentPopupWindow(HWND hwnd, HDC contentHdc, int contentW, int contentH, HDC fallbackDc);

/// 阴影区域返回 HTTRANSPARENT，内容区委托 contentHitTest(cx,cy)
LRESULT PopupShadowHitTest(HWND hwnd, int contentW, int contentH, LPARAM lp,
    const std::function<LRESULT(int cx, int cy)>& contentHitTest);

void DrawCrosshairGlyph(HDC hdc, int cx, int cy, int size,
    COLORREF lineColor, COLORREF fillColor, bool withFill,
    int stroke = 2);

HCURSOR CreateCrosshairDragCursor(COLORREF crosshairColor);

inline void DrawBorderRect(HDC hdc, const RECT& rc, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

inline void FillRectColor(HDC hdc, const RECT& rc, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rc, brush);
    DeleteObject(brush);
}

inline void DrawBorderRoundRect(HDC hdc, const RECT& rc, COLORREF color, int cornerRadius) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, cornerRadius, cornerRadius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}
