// ── 图形绘制工具实现 ──────────────────────────────────────────
#include "drawing.h"

#include "render_context.h"
#include "ui_scale.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>

#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

void DrawCheckboxImpl(IRenderContext& ctx, const RECT& rc, bool checked) {
    const COLORREF border = checked ? kMainGreen : RGB(190, 190, 190);
    const COLORREF fill = checked ? kMainGreen : kWhite;
    ctx.FillRoundRect(rc, fill, 3.0f);
    ctx.DrawBorderRoundRect(rc, border, 3.0f);
    if (checked) {
        // 几何勾选：避免依赖字体字形在不同绘制路径下显示不一致
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        const float stroke = std::max(1.5f, static_cast<float>(std::min(w, h)) * 0.12f);
        const int x1 = rc.left + w * 22 / 100;
        const int y1 = rc.top + h * 50 / 100;
        const int x2 = rc.left + w * 42 / 100;
        const int y2 = rc.top + h * 70 / 100;
        const int x3 = rc.left + w * 78 / 100;
        const int y3 = rc.top + h * 28 / 100;
        ctx.DrawLine(x1, y1, x2, y2, kWhite, stroke);
        ctx.DrawLine(x2, y2, x3, y3, kWhite, stroke);
    }
}

void DrawRadioButtonImpl(IRenderContext& ctx, const RECT& rc, bool checked) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    const int inset = 1;
    const RECT inner{rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset};
    const COLORREF border = checked ? kMainGreen : kComboBorderGray;
    ctx.DrawEllipse(inner.left, inner.top, inner.right, inner.bottom, kWhite, 1.0f, true);
    ctx.DrawEllipse(inner.left, inner.top, inner.right, inner.bottom, border, 2.0f, false);
    if (checked) {
        const int dotR = std::max(4, std::min(w, h) / 2 - 6);
        ctx.DrawEllipse(cx - dotR, cy - dotR, cx + dotR, cy + dotR, kMainGreen, 1.0f, true);
    }
}

void DrawMouseBodyOutline(HDC hdc, int ml, int mt, int mr, int mb, int mw, COLORREF color, float stroke) {
    const int topInset = std::max(2, mw / 10);
    const int topY = mt;
    const int joinY = mb - mw / 2;
    const int penW = std::max(1, static_cast<int>(stroke + 0.5f));
    HPEN pen = CreatePen(PS_SOLID, penW, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    MoveToEx(hdc, ml + topInset, topY, nullptr);
    LineTo(hdc, mr - topInset, topY);
    LineTo(hdc, mr, joinY);
    Arc(hdc, ml, mb - mw, mr, mb, mr, joinY, ml, joinY);
    LineTo(hdc, ml + topInset, topY);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void DrawMouseBrandGlyphImpl(IRenderContext& ctx, const RECT& bounds, COLORREF lineColor,
    float stroke, bool fillGreenCircle) {
    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    const int size = std::max(1, std::min(w, h));
    const int cx = (bounds.left + bounds.right) / 2;
    const int cy = (bounds.top + bounds.bottom) / 2;

    COLORREF drawColor = lineColor;
    if (fillGreenCircle) {
        const int r = size / 2;
        ctx.DrawEllipse(cx - r, cy - r, cx + r, cy + r, kMainGreen, 1.0f, true);
        drawColor = kWhite;
    }

    const int mw = size * 13 / 20;
    const int mh = size * 17 / 24;
    const int ml = cx - mw / 2;
    const int mr = ml + mw;
    const int mt = cy - mh / 2 + size / 12;
    const int mb = mt + mh;

    DrawMouseBodyOutline(ctx.nativeHdc(), ml, mt, mr, mb, mw, drawColor, stroke);

    const int midY = mt + mh * 2 / 5;
    ctx.DrawLine(ml + mw / 7, midY, mr - mw / 7, midY, drawColor, stroke);
    ctx.DrawLine(cx, mt + mh / 10, cx, midY, drawColor, stroke);

    const int wheelW = std::max(2, mw / 8);
    const int wheelH = std::max(3, mh / 6);
    const int wheelTop = mt + mh / 8;
    ctx.FillRoundRect(RECT{cx - wheelW / 2, wheelTop, cx + wheelW / 2 + 1, wheelTop + wheelH},
        drawColor, 1.5f);

    const int cordTop = mt - mh / 5;
    const int cordMidX = cx + mw / 4;
    const int cordMidY = mt - mh / 12;
    ctx.DrawLine(cx, mt + 1, cx, cordMidY, drawColor, stroke);
    ctx.DrawLine(cx, cordMidY, cordMidX, cordTop, drawColor, stroke);
}

}  // namespace

namespace {

void BuildFlatTipCogPoints(float cx, float cy, float outerR, float innerR, int teeth,
    POINT* pts, int& count) {
    // Smooth gear outline via continuous angular sweep at kStep resolution.
    // Valley arc ≈ 22°, slant ≈ 5° per side, flat tooth top ≈ 28° (per 60° pitch).
    // Valley centres are at 12 / 6 o'clock.
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kStep      = 0.8 * kPi / 180.0;  // 0.8° angular step
    static constexpr double kValleyHalf = 11.0 * kPi / 180.0; // half valley = 11°
    static constexpr double kSlant      =  5.0 * kPi / 180.0; // slant width = 5°
    const double pitch = (2.0 * kPi) / teeth;

    count = 0;
    for (int i = 0; i < teeth; ++i) {
        const double valleyMid  = -kPi * 0.5 + i * pitch;
        const double toothStart = valleyMid + kValleyHalf;
        const double toothEnd   = valleyMid + pitch - kValleyHalf;

        const double segStart   = valleyMid - kValleyHalf;
        const double segEnd     = valleyMid + pitch - kValleyHalf;

        for (double ang = segStart; ang < segEnd; ang += kStep) {
            float r;
            if (ang < toothStart) {
                r = innerR;                                           // valley floor
            } else if (ang < toothStart + kSlant) {
                const double t = (ang - toothStart) / kSlant;
                r = static_cast<float>(innerR + t * (outerR - innerR)); // left rise
            } else if (ang < toothEnd - kSlant) {
                r = outerR;                                           // flat tooth top
            } else {
                const double t = (ang - (toothEnd - kSlant)) / kSlant;
                r = static_cast<float>(outerR - t * (outerR - innerR)); // right fall
            }
            pts[count].x = static_cast<LONG>(std::lround(cx + r * std::cos(ang)));
            pts[count].y = static_cast<LONG>(std::lround(cy + r * std::sin(ang)));
            ++count;
        }
    }
}

}  // namespace

void FillRectColor(HDC hdc, const RECT& rc, COLORREF color) {
    ResolveRenderContext(hdc).FillRect(rc, color);
}

void FillGradientRect(HDC hdc, const RECT& rc, COLORREF start, COLORREF end, bool vertical) {
    ResolveRenderContext(hdc).FillGradientRect(rc, start, end, vertical);
}

void FillAlphaRect(HDC hdc, const RECT& rc, COLORREF color, BYTE alpha) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (!hdc || w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    FillRectColor(mem, RECT{0, 0, w, h}, color);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
    AlphaBlend(hdc, rc.left, rc.top, w, h, mem, 0, 0, w, h, blend);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void DrawFilledTriangle(HDC hdc, const POINT pts[3], COLORREF color) {
    if (!hdc || !pts) return;
    ResolveRenderContext(hdc).DrawPolygon(pts, 3, color, true);
}

void DrawExpandTriangle(HDC hdc, const RECT& rc, bool expanded, COLORREF color) {
    POINT pts[3]{};
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    if (expanded) {
        pts[0] = {cx - 4, cy - 2};
        pts[1] = {cx + 4, cy - 2};
        pts[2] = {cx, cy + 4};
    } else {
        pts[0] = {cx - 2, cy - 4};
        pts[1] = {cx - 2, cy + 4};
        pts[2] = {cx + 4, cy};
    }
    DrawFilledTriangle(hdc, pts, color);
}

void DrawComboDownArrow(HDC hdc, int centerX, int centerY, COLORREF color) {
    POINT arrow[3] = {
        {centerX - 5, centerY - 3},
        {centerX + 5, centerY - 3},
        {centerX, centerY + 4},
    };
    DrawFilledTriangle(hdc, arrow, color);
}

void DrawTopActionGlyph(HDC hdc, const RECT& rc, int iconType) {
    IRenderContext& ctx = ResolveRenderContext(hdc);
    const int x = rc.left + UiLen(8);
    const int y = rc.top + UiLen(9);
    if (iconType == 0 || iconType == 1) {
        const float sw = static_cast<float>(std::max(1, UiLen(2)));
        ctx.DrawLine(x + UiLen(2), y + UiLen(18), x + UiLen(18), y + UiLen(18), kWhite, sw);
        ctx.DrawLine(x + UiLen(2), y + UiLen(14), x + UiLen(2), y + UiLen(18), kWhite, sw);
        ctx.DrawLine(x + UiLen(18), y + UiLen(14), x + UiLen(18), y + UiLen(18), kWhite, sw);
        if (iconType == 0) {
            ctx.DrawLine(x + UiLen(10), y + UiLen(3), x + UiLen(10), y + UiLen(13), kWhite, sw);
            ctx.DrawLine(x + UiLen(10), y + UiLen(13), x + UiLen(6), y + UiLen(9), kWhite, sw);
            ctx.DrawLine(x + UiLen(10), y + UiLen(13), x + UiLen(14), y + UiLen(9), kWhite, sw);
        } else {
            ctx.DrawLine(x + UiLen(10), y + UiLen(16), x + UiLen(10), y + UiLen(6), kWhite, sw);
            ctx.DrawLine(x + UiLen(10), y + UiLen(6), x + UiLen(6), y + UiLen(10), kWhite, sw);
            ctx.DrawLine(x + UiLen(10), y + UiLen(6), x + UiLen(14), y + UiLen(10), kWhite, sw);
        }
    } else {
        DrawClockGlyph(hdc, x + UiLen(11), y + UiLen(11), UiLen(18), kWhite, std::max(1, UiLen(2)));
    }
}

void DrawNavIcon(HDC hdc, const RECT& rc, int iconType, HFONT homeTabFont) {
    const int cx = rc.left + UiLen(29);
    const int cy = rc.top + (rc.bottom - rc.top) / 2;
    IRenderContext& ctx = ResolveRenderContext(hdc);
    const float sw = static_cast<float>(std::max(1, UiLen(2)));
    if (iconType == 0) {
        DrawPointerCursorGlyph(hdc, cx, cy, UiLen(20), kWhite);
        return;
    }
    if (iconType == 1) {
        const int half = UiLen(10);
        const RECT box{cx - half, cy - half, cx + half, cy + half};
        ctx.DrawBorderRoundRect(box, kWhite, static_cast<float>(UiLen(5)));
        const int dot = UiLen(4);
        ctx.DrawEllipse(cx - dot, cy - dot, cx + dot, cy + dot, kWhite, 1.0f, true);
        return;
    }
    if (iconType == 2) {
        if (homeTabFont) SelectObject(hdc, homeTabFont);
        DrawTextIn(hdc, L"宏",
            RECT{cx - UiLen(13), cy - UiLen(14), cx + UiLen(15), cy + UiLen(14)}, kWhite,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const int hw = UiLen(13);
    const int hh = UiLen(10);
    const RECT box{cx - hw, cy - hh, cx + hw, cy + hh};
    ctx.DrawBorderRoundRect(box, kWhite, static_cast<float>(UiLen(5)));
    ctx.DrawLine(cx - UiLen(5), cy - UiLen(4), cx - UiLen(8), cy, kWhite, sw);
    ctx.DrawLine(cx - UiLen(8), cy, cx - UiLen(5), cy + UiLen(4), kWhite, sw);
    ctx.DrawLine(cx + UiLen(5), cy - UiLen(4), cx + UiLen(7), cy, kWhite, sw);
    ctx.DrawLine(cx + UiLen(7), cy, cx + UiLen(5), cy + UiLen(4), kWhite, sw);
}

void DrawHomeRadio(HDC hdc, const RECT& rc, bool checked) {
    IRenderContext& ctx = ResolveRenderContext(hdc);
    ctx.DrawEllipse(rc.left, rc.top, rc.right, rc.bottom, kMainGreen, 1.0f, true);
    ctx.DrawEllipse(rc.left, rc.top, rc.right, rc.bottom, kWhite, 2.0f, false);
    if (checked) {
        const int inset = UiLen(6);
        const RECT inner{rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset};
        ctx.DrawEllipse(inner.left, inner.top, inner.right, inner.bottom, kWhite, 1.0f, true);
    }
}

void DrawRecorderEmptyIcon(HDC hdc) {
    IRenderContext& ctx = ResolveRenderContext(hdc);
    const int centerX = UiLen(359);
    const int centerY = UiLen(252);
    const int half = UiLen(44);
    const RECT outer{centerX - half, centerY - half, centerX + half, centerY + half};
    const int dotR = UiLen(12);
    const RECT dot{centerX - dotR, centerY - dotR, centerX + dotR, centerY + dotR};
    HDC native = ctx.nativeHdc();
    HPEN pen = CreatePen(PS_SOLID, std::max(1, UiLen(8)), kWhite);
    HGDIOBJ oldPen = SelectObject(native, pen);
    HGDIOBJ oldBrush = SelectObject(native, GetStockObject(NULL_BRUSH));
    RoundRect(native, outer.left, outer.top, outer.right, outer.bottom, UiLen(14), UiLen(14));
    SelectObject(native, oldBrush);
    SelectObject(native, oldPen);
    DeleteObject(pen);
    ctx.DrawEllipse(dot.left, dot.top, dot.right, dot.bottom, kSecondaryText, 1.0f, true);
}

void FillRoundRectColor(HDC hdc, const RECT& rc, COLORREF color, int cornerRadius) {
    ResolveRenderContext(hdc).FillRoundRect(rc, color, static_cast<float>(cornerRadius));
}

void DrawTextIn(HDC hdc, const std::wstring& text, RECT rc, COLORREF color, UINT format) {
    DrawTextIn(hdc, text.c_str(), rc, color, format);
}

void DrawTextIn(HDC hdc, const wchar_t* text, RECT rc, COLORREF color, UINT format) {
    if (!hdc || !text) return;
    const HFONT font = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
    ResolveRenderContext(hdc).DrawUiText(text, rc, color, format, font);
}

void DrawBorderRect(HDC hdc, const RECT& rc, COLORREF color) {
    ResolveRenderContext(hdc).DrawBorderRect(rc, color);
}

void DrawBorderRoundRect(HDC hdc, const RECT& rc, COLORREF color, int cornerRadius) {
    ResolveRenderContext(hdc).DrawBorderRoundRect(rc, color, static_cast<float>(cornerRadius));
}

void DrawCheckbox(HDC hdc, const RECT& rc, bool checked) {
    DrawCheckboxImpl(ResolveRenderContext(hdc), rc, checked);
}

void DrawRadioButton(HDC hdc, const RECT& rc, bool checked) {
    DrawRadioButtonImpl(ResolveRenderContext(hdc), rc, checked);
}

void DrawAppTitleGlyph(HDC hdc, const RECT& circleBounds, COLORREF color, float stroke,
    bool fillGreenCircle) {
    DrawMouseBrandGlyphImpl(ResolveRenderContext(hdc), circleBounds, color, stroke, fillGreenCircle);
}

void DrawGearGlyph(HDC hdc, const RECT& rc, COLORREF color, COLORREF /*holeColor*/) {
    IRenderContext& ctx = ResolveRenderContext(hdc);
    const float cx = (rc.left + rc.right) * 0.5f;
    const float cy = (rc.top + rc.bottom) * 0.5f;
    const float fitR = std::min(rc.right - rc.left, rc.bottom - rc.top) * 0.5f;
    const float outerR = std::max(6.0f, fitR - 6.5f);
    const float innerR = outerR * 0.78f;
    const float holeR = outerR * 0.32f;
    static constexpr float kStroke = 1.75f;

    static constexpr int kTeeth = 6;
    static constexpr int kMaxPts = 512;
    POINT pts[kMaxPts]{};
    int count = 0;
    BuildFlatTipCogPoints(cx, cy, outerR, innerR, kTeeth, pts, count);
    ctx.DrawPolygon(pts, count, color, false);
    ctx.DrawEllipse(
        static_cast<int>(std::lround(cx - holeR)),
        static_cast<int>(std::lround(cy - holeR)),
        static_cast<int>(std::lround(cx + holeR)),
        static_cast<int>(std::lround(cy + holeR)),
        color, kStroke, false);
}

void DrawClockGlyph(HDC hdc, int cx, int cy, int size, COLORREF color, int stroke) {
    if (!hdc || size <= 0) return;
    IRenderContext& ctx = ResolveRenderContext(hdc);
    const int r = size / 2;
    ctx.DrawEllipse(cx - r, cy - r, cx + r + 1, cy + r + 1, color,
        static_cast<float>(stroke), false);
    const int handLen = std::max(4, (r * 5) / 9);
    ctx.DrawLine(cx, cy, cx, cy - handLen, color, static_cast<float>(stroke));
    ctx.DrawLine(cx, cy, cx + handLen, cy, color, static_cast<float>(stroke));
    const int dotR = std::max(2, stroke);
    ctx.DrawEllipse(cx - dotR, cy - dotR, cx + dotR + 1, cy + dotR + 1, color, 1.0f, true);
}

void DrawCrosshairGlyph(IRenderContext& ctx, int cx, int cy, int size,
    COLORREF lineColor, COLORREF fillColor, bool withFill,
    int stroke) {
    const int half = size / 2;
    const int left = cx - half;
    const int top = cy - half;
    const int right = left + size;
    const int bottom = top + size;
    if (withFill) {
        ctx.FillRect(RECT{left, top, right, bottom}, fillColor);
    }
    const int outerR = half - std::max(3, stroke + 1);
    const int innerR = std::max(2, size / 9);
    ctx.DrawEllipse(cx - outerR, cy - outerR, cx + outerR + 1, cy + outerR + 1,
        lineColor, static_cast<float>(stroke), false);
    ctx.DrawLine(left + 2, cy, cx - innerR - 1, cy, lineColor, static_cast<float>(stroke));
    ctx.DrawLine(cx + innerR + 1, cy, right - 2, cy, lineColor, static_cast<float>(stroke));
    ctx.DrawLine(cx, top + 2, cx, cy - innerR - 1, lineColor, static_cast<float>(stroke));
    ctx.DrawLine(cx, cy + innerR + 1, cx, bottom - 2, lineColor, static_cast<float>(stroke));
    ctx.DrawEllipse(cx - innerR, cy - innerR, cx + innerR + 1, cy + innerR + 1,
        lineColor, 1.0f, true);
}

void DrawCrosshairGlyph(HDC hdc, int cx, int cy, int size,
    COLORREF lineColor, COLORREF fillColor, bool withFill,
    int stroke) {
    DrawCrosshairGlyph(ResolveRenderContext(hdc), cx, cy, size, lineColor, fillColor, withFill, stroke);
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
    AlphaBlend(hdc, x, y, w, h, mem,
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
    BOOL cloaked = FALSE;
    const bool isCloaked = SUCCEEDED(DwmGetWindowAttribute(owner_, DWMWA_CLOAK, &cloaked, sizeof(cloaked)))
        && cloaked == TRUE;
    if (!IsWindowVisible(owner_) || IsIconic(owner_) || isCloaked) {
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
