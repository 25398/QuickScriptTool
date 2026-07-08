// ── 图形绘制工具实现 ──────────────────────────────────────────
#include "drawing.h"

#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
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

constexpr UINT_PTR kWindowOuterShadowSubclassId = 0x5153;
constexpr wchar_t kOuterShadowClass[] = L"QuickScriptOuterShadow";

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

namespace {

struct PointerCursorCache {
    HBITMAP bmp = nullptr;
    int size = 0;
    int drawW = 0;
    int drawH = 0;
    int anchorX = 0;
    int anchorY = 0;

    void Clear() {
        if (bmp) {
            DeleteObject(bmp);
            bmp = nullptr;
        }
        size = 0;
        drawW = drawH = anchorX = anchorY = 0;
    }
};

PointerCursorCache g_pointerCursor;

bool EnsurePointerCursorBitmap(int size, COLORREF color) {
    if (size <= 0) return false;
    static COLORREF cachedColor = 0xFFFFFFFF;
    if (g_pointerCursor.bmp && g_pointerCursor.size == size && cachedColor == color) return true;

    g_pointerCursor.Clear();
    cachedColor = color;

    HCURSOR cur = LoadCursorW(nullptr, IDC_ARROW);
    if (!cur) return false;

    ICONINFO ii{};
    if (!GetIconInfo(cur, &ii) || !ii.hbmMask) return false;

    BITMAP bm{};
    GetObject(ii.hbmMask, sizeof(bm), &bm);
    const int srcW = bm.bmWidth;
    const int srcH = bm.bmHeight / 2;
    if (srcW <= 0 || srcH <= 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
        return false;
    }

    HDC screen = GetDC(nullptr);
    HDC maskDc = CreateCompatibleDC(screen);
    HGDIOBJ oldMaskBmp = SelectObject(maskDc, ii.hbmMask);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = srcW;
    bmi.bmiHeader.biHeight = -srcH * 2;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<BYTE> maskPx(static_cast<size_t>(srcW) * srcH * 2 * 4);
    if (!GetDIBits(maskDc, ii.hbmMask, 0, srcH * 2, maskPx.data(), &bmi, DIB_RGB_COLORS)) {
        SelectObject(maskDc, oldMaskBmp);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
        DeleteDC(maskDc);
        ReleaseDC(nullptr, screen);
        return false;
    }

    SelectObject(maskDc, oldMaskBmp);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    DeleteDC(maskDc);

    const BYTE fr = GetRValue(color);
    const BYTE fg = GetGValue(color);
    const BYTE fb = GetBValue(color);

    auto maskLum = [](const BYTE* px) {
        return (px[2] * 299 + px[1] * 587 + px[0] * 114) / 1000;
    };

    int minX = srcW, minY = srcH, maxX = -1, maxY = -1;
    std::vector<uint8_t> opaque(static_cast<size_t>(srcW) * srcH, 0);
    for (int y = 0; y < srcH; ++y) {
        for (int x = 0; x < srcW; ++x) {
            const size_t andIdx = (static_cast<size_t>(y) * srcW + x) * 4;
            if (maskLum(maskPx.data() + andIdx) >= 128) continue;
            opaque[static_cast<size_t>(y) * srcW + x] = 1;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }

    if (maxX < minX || maxY < minY) {
        ReleaseDC(nullptr, screen);
        return false;
    }

    const int cropW = maxX - minX + 1;
    const int cropH = maxY - minY + 1;
    const int margin = 1;
    const int avail = std::max(1, size - margin * 2);
    const double scale = std::min(static_cast<double>(avail) / cropW,
        static_cast<double>(avail) / cropH);
    const int drawW = std::max(1, static_cast<int>(cropW * scale + 0.5));
    const int drawH = std::max(1, static_cast<int>(cropH * scale + 0.5));
    const int dstX = (size - drawW) / 2;
    const int dstY = (size - drawH) / 2;

    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    void* outBits = nullptr;
    HBITMAP outBmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &outBits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!outBmp || !outBits) return false;

    auto* px = static_cast<uint32_t*>(outBits);
    std::fill_n(px, static_cast<size_t>(size) * size, 0u);
    const uint32_t fill = (static_cast<uint32_t>(0xFF) << 24)
        | (static_cast<uint32_t>(fr) << 16)
        | (static_cast<uint32_t>(fg) << 8)
        | static_cast<uint32_t>(fb);

    for (int dy = 0; dy < drawH; ++dy) {
        const int sy = minY + static_cast<int>(dy / scale);
        for (int dx = 0; dx < drawW; ++dx) {
            const int sx = minX + static_cast<int>(dx / scale);
            if (!opaque[static_cast<size_t>(sy) * srcW + sx]) continue;
            px[(dstY + dy) * size + (dstX + dx)] = fill;
        }
    }

    g_pointerCursor.bmp = outBmp;
    g_pointerCursor.size = size;
    g_pointerCursor.drawW = size;
    g_pointerCursor.drawH = size;
    g_pointerCursor.anchorX = 0;
    g_pointerCursor.anchorY = 0;
    return true;
}

}  // namespace

void DrawPointerCursorGlyph(HDC hdc, int cx, int cy, int size, COLORREF color) {
    if (!hdc || size <= 0) return;
    if (!EnsurePointerCursorBitmap(size, color) || !g_pointerCursor.bmp) return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, g_pointerCursor.bmp);

    const int w = g_pointerCursor.drawW > 0 ? g_pointerCursor.drawW : size;
    const int h = g_pointerCursor.drawH > 0 ? g_pointerCursor.drawH : size;
    const int x = cx - w / 2;
    const int y = cy - h / 2 + 1;
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    GdiAlphaBlend(hdc, x, y, w, h, mem,
        g_pointerCursor.anchorX, g_pointerCursor.anchorY, w, h, bf);

    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

WindowOuterShadow::~WindowOuterShadow() {
    Detach();
    if (shadow_) {
        DestroyWindow(shadow_);
        shadow_ = nullptr;
    }
}

bool WindowOuterShadow::EnsureShadowClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOuterShadowClass;
    registered = RegisterClassW(&wc) != 0;
    return registered;
}

bool WindowOuterShadow::EnsureShadowWindow() {
    if (shadow_ && IsWindow(shadow_)) return true;
    if (!EnsureShadowClass()) return false;
    shadow_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOuterShadowClass, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    return shadow_ != nullptr;
}

bool WindowOuterShadow::Render(int contentW, int contentH) {
    if (!shadow_ || !owner_ || contentW <= 0 || contentH <= 0) return false;
    const int m = kWindowEdgeShadowSize;
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
            const int alpha = (m - dist + 1) * kWindowEdgeShadowMaxAlpha / (m + 1);
            px[y * outerW + x] = static_cast<uint32_t>(alpha) << 24;
        }
    }

    HDC outerDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldOuter = SelectObject(outerDc, outerBmp);

    RECT wr{};
    GetWindowRect(owner_, &wr);
    POINT ptDst{wr.left - m, wr.top - m};
    SIZE size{outerW, outerH};
    POINT ptSrc{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL ok = UpdateLayeredWindow(shadow_, screenDc, &ptDst, &size, outerDc,
        &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(outerDc, oldOuter);
    ReleaseDC(nullptr, screenDc);
    DeleteObject(outerBmp);
    DeleteDC(outerDc);
    return ok == TRUE;
}

void WindowOuterShadow::Sync() {
    if (!owner_ || !IsWindow(owner_)) return;
    if (!IsWindowVisible(owner_) || IsIconic(owner_)) {
        if (shadow_) ShowWindow(shadow_, SW_HIDE);
        return;
    }
    if (!EnsureShadowWindow()) return;

    RECT wr{};
    GetWindowRect(owner_, &wr);
    const int cw = wr.right - wr.left;
    const int ch = wr.bottom - wr.top;
    if (cw <= 0 || ch <= 0) return;

    const int m = kWindowEdgeShadowSize;
    SetWindowPos(shadow_, owner_, wr.left - m, wr.top - m, cw + 2 * m, ch + 2 * m,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Render(cw, ch);
}

LRESULT CALLBACK WindowOuterShadow::OwnerSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR /*id*/, DWORD_PTR refData) {
    auto* self = reinterpret_cast<WindowOuterShadow*>(refData);
    if (self) {
        if (msg == WM_WINDOWPOSCHANGED || msg == WM_SIZE || msg == WM_SHOWWINDOW)
            self->Sync();
        if (msg == WM_DESTROY)
            self->Detach();
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

bool WindowOuterShadow::Attach(HWND owner) {
    if (!owner || !IsWindow(owner)) return false;
    Detach();
    owner_ = owner;
    if (!EnsureShadowWindow()) {
        owner_ = nullptr;
        return false;
    }
    subclassed_ = SetWindowSubclass(owner_, OwnerSubclassProc, kWindowOuterShadowSubclassId,
        reinterpret_cast<DWORD_PTR>(this)) != FALSE;
    Sync();
    return shadow_ != nullptr;
}

void WindowOuterShadow::Detach() {
    if (owner_ && subclassed_) {
        RemoveWindowSubclass(owner_, OwnerSubclassProc, kWindowOuterShadowSubclassId);
        subclassed_ = false;
    }
    owner_ = nullptr;
    if (shadow_) ShowWindow(shadow_, SW_HIDE);
}

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
