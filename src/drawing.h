#pragma once
// ──────────────────────────────────────────────────────────────────
// drawing.h — 图形绘制辅助函数（声明）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <algorithm>
#include <functional>

#include "config.h"

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

/// 主窗口/对话框外扩透明阴影（Layered + UpdateLayeredWindow，点击穿透）
class WindowOuterShadow {
public:
    WindowOuterShadow() = default;
    ~WindowOuterShadow();

    bool Attach(HWND owner);
    void Detach();
    void Sync();

private:
    static LRESULT CALLBACK OwnerSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR refData);
    static bool EnsureShadowClass();
    bool EnsureShadowWindow();
    bool Render(int contentW, int contentH);

    HWND owner_ = nullptr;
    HWND shadow_ = nullptr;
    bool subclassed_ = false;
};

void DrawCrosshairGlyph(HDC hdc, int cx, int cy, int size,
    COLORREF lineColor, COLORREF fillColor, bool withFill,
    int stroke = 2);

HCURSOR CreateCrosshairDragCursor(COLORREF crosshairColor);

/// 简约线框时钟：12 点与 3 点指针 + 中心圆点（用于「定时」等功能入口）
inline void DrawClockGlyph(HDC hdc, int cx, int cy, int size,
    COLORREF color, int stroke = 2) {
    if (!hdc || size <= 0) return;
    HPEN pen = CreatePen(PS_SOLID, stroke, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    const int r = size / 2;
    Ellipse(hdc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    const int handLen = std::max(4, (r * 5) / 9);
    MoveToEx(hdc, cx, cy, nullptr);
    LineTo(hdc, cx, cy - handLen);
    MoveToEx(hdc, cx, cy, nullptr);
    LineTo(hdc, cx + handLen, cy);
    const int dotR = std::max(2, stroke);
    HBRUSH dot = CreateSolidBrush(color);
    SelectObject(hdc, dot);
    Ellipse(hdc, cx - dotR, cy - dotR, cx + dotR + 1, cy + dotR + 1);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(dot);
    DeleteObject(pen);
}

/// 标准箭头指针（取自系统 IDC_ARROW 轮廓，实心填充）
void DrawPointerCursorGlyph(HDC hdc, int cx, int cy, int size, COLORREF color);

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

/// 统一勾选框：选中为绿色底 + 白色 ✓，未选中为白底 + 灰边。
inline void DrawCheckbox(HDC hdc, const RECT& rc, bool checked) {
    const COLORREF border = checked ? kMainGreen : RGB(190, 190, 190);
    const COLORREF fill = checked ? kMainGreen : kWhite;
    HPEN boxPen = CreatePen(PS_SOLID, 1, border);
    HBRUSH boxBrush = CreateSolidBrush(fill);
    HGDIOBJ oldPen = SelectObject(hdc, boxPen);
    HGDIOBJ oldBrush = SelectObject(hdc, boxBrush);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 3, 3);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(boxPen);
    DeleteObject(boxBrush);
    if (checked) {
        const int oldBk = SetBkMode(hdc, TRANSPARENT);
        const COLORREF oldColor = SetTextColor(hdc, kWhite);
        DrawTextW(hdc, L"✓", -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetBkMode(hdc, oldBk);
        SetTextColor(hdc, oldColor);
    }
}
