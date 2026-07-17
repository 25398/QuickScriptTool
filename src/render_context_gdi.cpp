// ── GDI 绘制上下文（带 brush/pen 缓存）──────────────────────────
#include "render_context.h"

#pragma comment(lib, "msimg32.lib")

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace {

struct PenKey {
    COLORREF color = 0;
    int width = 1;

    bool operator==(const PenKey& o) const {
        return color == o.color && width == o.width;
    }
};

struct PenKeyHash {
    size_t operator()(const PenKey& k) const {
        return static_cast<size_t>(k.color) ^ (static_cast<size_t>(k.width) << 24);
    }
};

class GdiResourceCache {
public:
    HBRUSH GetBrush(COLORREF color) {
        auto it = brushes_.find(color);
        if (it != brushes_.end()) return it->second;
        HBRUSH brush = CreateSolidBrush(color);
        brushes_[color] = brush;
        return brush;
    }

    HPEN GetPen(COLORREF color, int width) {
        const PenKey key{color, width};
        auto it = pens_.find(key);
        if (it != pens_.end()) return it->second;
        HPEN pen = CreatePen(PS_SOLID, width, color);
        pens_[key] = pen;
        return pen;
    }

private:
    std::unordered_map<COLORREF, HBRUSH> brushes_;
    std::unordered_map<PenKey, HPEN, PenKeyHash> pens_;
};

GdiResourceCache g_gdiCache;

class GdiRenderContext final : public IRenderContext {
public:
    explicit GdiRenderContext(HDC hdc) : hdc_(hdc) {}

    RenderBackend backend() const override { return RenderBackend::Gdi; }
    HDC nativeHdc() const override { return hdc_; }

    void FillRect(const RECT& rc, COLORREF color) override {
        ::FillRect(hdc_, &rc, g_gdiCache.GetBrush(color));
    }

    void FillGradientRect(const RECT& rc, COLORREF start, COLORREF end, bool vertical) override {
        TRIVERTEX vertex[2]{};
        vertex[0].x = rc.left;
        vertex[0].y = rc.top;
        vertex[0].Red = static_cast<COLOR16>(GetRValue(start) << 8);
        vertex[0].Green = static_cast<COLOR16>(GetGValue(start) << 8);
        vertex[0].Blue = static_cast<COLOR16>(GetBValue(start) << 8);
        vertex[0].Alpha = 0;
        vertex[1].x = rc.right;
        vertex[1].y = rc.bottom;
        vertex[1].Red = static_cast<COLOR16>(GetRValue(end) << 8);
        vertex[1].Green = static_cast<COLOR16>(GetGValue(end) << 8);
        vertex[1].Blue = static_cast<COLOR16>(GetBValue(end) << 8);
        vertex[1].Alpha = 0;
        GRADIENT_RECT gr{0, 1};
        GradientFill(hdc_, vertex, 2, &gr, 1, vertical ? GRADIENT_FILL_RECT_V : GRADIENT_FILL_RECT_H);
    }

    void FillRoundRect(const RECT& rc, COLORREF color, float cornerRadius) override {
        HPEN pen = g_gdiCache.GetPen(color, 1);
        HBRUSH brush = g_gdiCache.GetBrush(color);
        HGDIOBJ oldPen = SelectObject(hdc_, pen);
        HGDIOBJ oldBrush = SelectObject(hdc_, brush);
        const int r = std::max(1, static_cast<int>(cornerRadius + 0.5f));
        RoundRect(hdc_, rc.left, rc.top, rc.right, rc.bottom, r, r);
        SelectObject(hdc_, oldBrush);
        SelectObject(hdc_, oldPen);
    }

    void DrawBorderRect(const RECT& rc, COLORREF color) override {
        HPEN pen = g_gdiCache.GetPen(color, 1);
        HGDIOBJ oldPen = SelectObject(hdc_, pen);
        HGDIOBJ oldBrush = SelectObject(hdc_, GetStockObject(NULL_BRUSH));
        Rectangle(hdc_, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc_, oldBrush);
        SelectObject(hdc_, oldPen);
    }

    void DrawBorderRoundRect(const RECT& rc, COLORREF color, float cornerRadius) override {
        HPEN pen = g_gdiCache.GetPen(color, 1);
        HGDIOBJ oldPen = SelectObject(hdc_, pen);
        HGDIOBJ oldBrush = SelectObject(hdc_, GetStockObject(NULL_BRUSH));
        const int r = std::max(1, static_cast<int>(cornerRadius + 0.5f));
        RoundRect(hdc_, rc.left, rc.top, rc.right, rc.bottom, r, r);
        SelectObject(hdc_, oldBrush);
        SelectObject(hdc_, oldPen);
    }

    void DrawLine(int x1, int y1, int x2, int y2, COLORREF color, float strokeWidth) override {
        const int w = std::max(1, static_cast<int>(strokeWidth + 0.5f));
        HPEN pen = g_gdiCache.GetPen(color, w);
        HGDIOBJ oldPen = SelectObject(hdc_, pen);
        MoveToEx(hdc_, x1, y1, nullptr);
        LineTo(hdc_, x2, y2);
        SelectObject(hdc_, oldPen);
    }

    void DrawEllipse(int left, int top, int right, int bottom,
        COLORREF color, float strokeWidth, bool filled) override {
        const int w = std::max(1, static_cast<int>(strokeWidth + 0.5f));
        HPEN pen = g_gdiCache.GetPen(color, w);
        HBRUSH brush = filled ? g_gdiCache.GetBrush(color) : static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
        HGDIOBJ oldPen = SelectObject(hdc_, pen);
        HGDIOBJ oldBrush = SelectObject(hdc_, brush);
        Ellipse(hdc_, left, top, right, bottom);
        SelectObject(hdc_, oldBrush);
        SelectObject(hdc_, oldPen);
    }

    void DrawPolygon(const POINT* pts, int count, COLORREF color, bool filled) override {
        if (!pts || count < 2) return;
        HPEN pen = g_gdiCache.GetPen(color, filled ? 1 : 2);
        HBRUSH brush = filled ? g_gdiCache.GetBrush(color) : static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
        HGDIOBJ oldPen = SelectObject(hdc_, pen);
        HGDIOBJ oldBrush = SelectObject(hdc_, brush);
        Polygon(hdc_, pts, count);
        SelectObject(hdc_, oldBrush);
        SelectObject(hdc_, oldPen);
    }

    void DrawUiText(const wchar_t* text, const RECT& rc, COLORREF color, UINT format,
        HFONT font = nullptr) override {
        if (!text) return;
        HGDIOBJ oldFont = font ? SelectObject(hdc_, font) : nullptr;
        const int oldBk = SetBkMode(hdc_, TRANSPARENT);
        const COLORREF oldColor = SetTextColor(hdc_, color);
        ::DrawTextW(hdc_, text, -1, const_cast<RECT*>(&rc), format);
        SetBkMode(hdc_, oldBk);
        SetTextColor(hdc_, oldColor);
        if (oldFont) SelectObject(hdc_, oldFont);
    }

    void PushClipRect(const RECT& rc) override {
        SaveDC(hdc_);
        IntersectClipRect(hdc_, rc.left, rc.top, rc.right, rc.bottom);
    }

    void PopClip() override {
        RestoreDC(hdc_, -1);
    }

private:
    HDC hdc_ = nullptr;
};

}  // namespace

RECT InferRenderBindRect(HDC hdc) {
    RECT rc{};
    HBITMAP bmp = static_cast<HBITMAP>(GetCurrentObject(hdc, OBJ_BITMAP));
    if (bmp) {
        BITMAP bm{};
        if (GetObject(bmp, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
            rc.right = bm.bmWidth;
            rc.bottom = bm.bmHeight;
            return rc;
        }
    }
    if (GetClipBox(hdc, &rc) != ERROR && rc.right > rc.left && rc.bottom > rc.top) {
        return rc;
    }
    rc.right = GetDeviceCaps(hdc, HORZRES);
    rc.bottom = GetDeviceCaps(hdc, VERTRES);
    return rc;
}

std::unique_ptr<IRenderContext> CreateGdiRenderContext(HDC hdc) {
    return std::make_unique<GdiRenderContext>(hdc);
}
