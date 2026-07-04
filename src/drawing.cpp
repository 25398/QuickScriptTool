// ── 图形绘制工具实现 ──────────────────────────────────────────
#include "drawing.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <vector>

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

namespace {

int DistToContentRect(int x, int y, int contentW, int contentH, int margin) {
    const int cx = x - margin;
    const int cy = y - margin;
    int dx = 0;
    int dy = 0;
    if (cx < 0) dx = -cx;
    else if (cx >= contentW) dx = cx - contentW + 1;
    if (cy < 0) dy = -cy;
    else if (cy >= contentH) dy = cy - contentH + 1;
    return std::max(dx, dy);
}

}  // namespace

void DrawPopupShadowMargin(HDC hdc, int contentW, int contentH) {
    if (!hdc || contentW <= 0 || contentH <= 0) return;
    const int m = kPopupShadowMargin;
    const int outerW = contentW + 2 * m;
    const int outerH = contentH + 2 * m;
    for (int y = 0; y < outerH; ++y) {
        for (int x = 0; x < outerW; ++x) {
            const int dist = DistToContentRect(x, y, contentW, contentH, m);
            if (dist <= 0 || dist > m) continue;
            const int alpha = (m - dist + 1) * 140 / (m + 1);
            const BYTE shade = static_cast<BYTE>(255 - alpha);
            SetPixel(hdc, x, y, RGB(shade, shade, shade));
        }
    }
}

void InitLayeredPopupWindow(HWND hwnd) {
    if (!hwnd) return;
    SetWindowLongW(hwnd, GWL_EXSTYLE,
        GetWindowLongW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
}

bool PresentLayeredPopup(HWND hwnd, HDC contentHdc, int contentW, int contentH) {
    if (!hwnd || !contentHdc || contentW <= 0 || contentH <= 0) return false;
    const int m = kPopupShadowMargin;
    const int outerW = contentW + 2 * m;
    const int outerH = contentH + 2 * m;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = outerW;
    bmi.bmiHeader.biHeight = -outerH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* outerBits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP outerBmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &outerBits, nullptr, 0);
    if (!outerBmp || !outerBits) {
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    auto* px = static_cast<uint32_t*>(outerBits);
    const int total = outerW * outerH;
    for (int i = 0; i < total; ++i) px[i] = 0;

    for (int y = 0; y < outerH; ++y) {
        for (int x = 0; x < outerW; ++x) {
            const int dist = DistToContentRect(x, y, contentW, contentH, m);
            if (dist <= 0 || dist > m) continue;
            const int alpha = (m - dist + 1) * 140 / (m + 1);
            px[y * outerW + x] = static_cast<uint32_t>(alpha) << 24;
        }
    }

    HDC outerDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldOuter = SelectObject(outerDc, outerBmp);

    std::vector<BYTE> contentBuf(static_cast<size_t>(contentW) * contentH * 4);
    BITMAPINFO cbmi{};
    cbmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    cbmi.bmiHeader.biWidth = contentW;
    cbmi.bmiHeader.biHeight = -contentH;
    cbmi.bmiHeader.biPlanes = 1;
    cbmi.bmiHeader.biBitCount = 32;
    cbmi.bmiHeader.biCompression = BI_RGB;
    HDC readDc = CreateCompatibleDC(screenDc);
    HBITMAP readBmp = CreateCompatibleBitmap(screenDc, contentW, contentH);
    HGDIOBJ oldRead = SelectObject(readDc, readBmp);
    BitBlt(readDc, 0, 0, contentW, contentH, contentHdc, 0, 0, SRCCOPY);
    GetDIBits(readDc, readBmp, 0, contentH, contentBuf.data(), &cbmi, DIB_RGB_COLORS);

    for (int y = 0; y < contentH; ++y) {
        for (int x = 0; x < contentW; ++x) {
            const size_t si = (static_cast<size_t>(y) * contentW + x) * 4;
            const BYTE b = contentBuf[si];
            const BYTE g = contentBuf[si + 1];
            const BYTE r = contentBuf[si + 2];
            px[(y + m) * outerW + (x + m)] =
                (static_cast<uint32_t>(0xFF) << 24)
                | (static_cast<uint32_t>(r) << 16)
                | (static_cast<uint32_t>(g) << 8)
                | static_cast<uint32_t>(b);
        }
    }

    SelectObject(readDc, oldRead);
    DeleteObject(readBmp);
    DeleteDC(readDc);
    SelectObject(outerDc, oldOuter);

    RECT wr{};
    GetWindowRect(hwnd, &wr);
    POINT ptDst{wr.left, wr.top};
    SIZE size{outerW, outerH};
    POINT ptSrc{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL ok = UpdateLayeredWindow(hwnd, screenDc, &ptDst, &size, outerDc,
        &ptSrc, 0, &blend, ULW_ALPHA);

    ReleaseDC(nullptr, screenDc);
    DeleteObject(outerBmp);
    DeleteDC(outerDc);
    return ok == TRUE;
}

bool PresentPopupWindow(HWND hwnd, HDC contentHdc, int contentW, int contentH, HDC fallbackDc) {
    if (!hwnd || !contentHdc || contentW <= 0 || contentH <= 0) return false;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) {
        if (PresentLayeredPopup(hwnd, contentHdc, contentW, contentH)) return true;
        SetWindowLongW(hwnd, GWL_EXSTYLE,
            GetWindowLongW(hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }
    if (fallbackDc) {
        const int m = kPopupShadowMargin;
        BitBlt(fallbackDc, m, m, contentW, contentH, contentHdc, 0, 0, SRCCOPY);
    }
    return false;
}

LRESULT PopupShadowHitTest(HWND hwnd, int contentW, int contentH, LPARAM lp,
    const std::function<LRESULT(int cx, int cy)>& contentHitTest) {
    int x = GET_X_LPARAM(lp);
    int y = GET_Y_LPARAM(lp);
    POINT pt{x, y};
    ScreenToClient(hwnd, &pt);
    if (IsPopupShadowArea(pt.x, pt.y, contentW, contentH)) return HTTRANSPARENT;
    int cx = static_cast<int>(pt.x);
    int cy = static_cast<int>(pt.y);
    ClientToContentPoint(cx, cy);
    if (cx < 0 || cy < 0 || cx >= contentW || cy >= contentH) return HTTRANSPARENT;
    return contentHitTest(cx, cy);
}
