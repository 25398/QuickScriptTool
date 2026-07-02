// ── 图形绘制工具实现 ──────────────────────────────────────────
#include "drawing.h"
#include <algorithm>
#include <cstdint>

void DrawCrosshairGlyph(HDC hdc, int cx, int cy, int size,
    COLORREF lineColor, COLORREF fillColor, bool withFill,
    int stroke) {
    const int half = size / 2;
    const int left = cx - half;
    const int top = cy - half;
    const int right = left + size;
    const int bottom = top + size;
    if (withFill) {
        HBRUSH brush = CreateSolidBrush(fillColor);
        RECT rc{left, top, right, bottom};
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
    }
    HPEN pen = CreatePen(PS_SOLID, stroke, lineColor);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    const int outerR = half - std::max(3, stroke + 1);
    const int innerR = std::max(2, size / 9);
    Ellipse(hdc, cx - outerR, cy - outerR, cx + outerR + 1, cy + outerR + 1);
    MoveToEx(hdc, left + 2, cy, nullptr);
    LineTo(hdc, cx - innerR - 1, cy);
    MoveToEx(hdc, cx + innerR + 1, cy, nullptr);
    LineTo(hdc, right - 2, cy);
    MoveToEx(hdc, cx, top + 2, nullptr);
    LineTo(hdc, cx, cy - innerR - 1);
    MoveToEx(hdc, cx, cy + innerR + 1, nullptr);
    LineTo(hdc, cx, bottom - 2);
    HBRUSH dotBrush = CreateSolidBrush(lineColor);
    SelectObject(hdc, dotBrush);
    Ellipse(hdc, cx - innerR, cy - innerR, cx + innerR + 1, cy + innerR + 1);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(dotBrush);
    DeleteObject(pen);
}

HCURSOR CreateCrosshairDragCursor(COLORREF crosshairColor) {
    constexpr int kSize = 40;
    constexpr int kHotspot = kSize / 2;
    HDC screen = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    BITMAPV5HEADER bih{};
    bih.bV5Size = sizeof(BITMAPV5HEADER);
    bih.bV5Width = kSize;
    bih.bV5Height = -kSize;
    bih.bV5Planes = 1;
    bih.bV5BitCount = 32;
    bih.bV5Compression = BI_BITFIELDS;
    bih.bV5RedMask   = 0x00FF0000;
    bih.bV5GreenMask = 0x0000FF00;
    bih.bV5BlueMask  = 0x000000FF;
    bih.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(memDC,
        reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS,
        &bits, nullptr, 0);
    HGDIOBJ oldColorBmp = SelectObject(memDC, colorBmp);
    std::fill_n(static_cast<std::uint32_t*>(bits),
        static_cast<size_t>(kSize * kSize), 0u);

    DrawCrosshairGlyph(memDC, kHotspot, kHotspot, kSize - 4,
        RGB(255, 255, 255), 0, false, 5);
    DrawCrosshairGlyph(memDC, kHotspot, kHotspot, kSize - 6,
        crosshairColor, 0, false, 3);

    auto* px = static_cast<std::uint32_t*>(bits);
    for (int i = 0; i < kSize * kSize; ++i) {
        if ((px[i] & 0x00FFFFFF) != 0) px[i] |= 0xFF000000;
    }

    HDC andDC = CreateCompatibleDC(nullptr);
    HBITMAP andMask = CreateBitmap(kSize, kSize, 1, 1, nullptr);
    HGDIOBJ oldMaskBmp = SelectObject(andDC, andMask);
    PatBlt(andDC, 0, 0, kSize, kSize, WHITENESS);

    ICONINFO ii{};
    ii.fIcon = FALSE;
    ii.xHotspot = kHotspot;
    ii.yHotspot = kHotspot;
    ii.hbmColor = colorBmp;
    ii.hbmMask = andMask;
    HCURSOR cursor = CreateIconIndirect(&ii);

    SelectObject(memDC, oldColorBmp);
    SelectObject(andDC, oldMaskBmp);
    DeleteObject(colorBmp);
    DeleteObject(andMask);
    DeleteDC(memDC);
    DeleteDC(andDC);
    return cursor;
}
