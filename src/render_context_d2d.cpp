// ── Direct2D 绘制上下文（绑定 GDI HDC，逐步替换 drawing.cpp）────
#include "render_context.h"

#include "render_font.h"

#include <d2d1helper.h>
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace {

D2D1_COLOR_F ColorFromRef(COLORREF c) {
    return D2D1::ColorF(
        GetRValue(c) / 255.0f,
        GetGValue(c) / 255.0f,
        GetBValue(c) / 255.0f,
        1.0f);
}

D2D1_RECT_F RectFromWin(const RECT& rc) {
    return D2D1::RectF(
        static_cast<FLOAT>(rc.left),
        static_cast<FLOAT>(rc.top),
        static_cast<FLOAT>(rc.right),
        static_cast<FLOAT>(rc.bottom));
}

class D2dBrushCache {
public:
    explicit D2dBrushCache(ID2D1RenderTarget* rt) : rt_(rt) {}

    ID2D1SolidColorBrush* Get(COLORREF color) {
        auto it = brushes_.find(color);
        if (it != brushes_.end()) return it->second;
        ID2D1SolidColorBrush* brush = nullptr;
        if (FAILED(rt_->CreateSolidColorBrush(ColorFromRef(color), &brush))) return nullptr;
        brushes_[color] = brush;
        return brush;
    }

    ~D2dBrushCache() {
        for (auto& kv : brushes_) {
            if (kv.second) kv.second->Release();
        }
    }

private:
    ID2D1RenderTarget* rt_ = nullptr;
    std::unordered_map<COLORREF, ID2D1SolidColorBrush*> brushes_;
};

class D2dRenderContext final : public IRenderContext {
public:
    D2dRenderContext(HDC hdc, const RECT& bindRect)
        : hdc_(hdc), bindRect_(bindRect) {
        ID2D1Factory* factory = GetD2DFactory();
        if (!factory || !hdc_) return;

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);

        ID2D1DCRenderTarget* dcRt = nullptr;
        if (FAILED(factory->CreateDCRenderTarget(&props, &dcRt))) return;

        dcRt_ = dcRt;
        if (FAILED(dcRt_->BindDC(hdc_, &bindRect_))) {
            dcRt_->Release();
            dcRt_ = nullptr;
            return;
        }

        dcRt_->BeginDraw();
        begun_ = true;
        brushCache_ = std::make_unique<D2dBrushCache>(dcRt_);
    }

    ~D2dRenderContext() override {
        Commit();
        if (dcRt_) {
            dcRt_->Release();
            dcRt_ = nullptr;
        }
    }

    bool IsValid() const { return dcRt_ != nullptr; }

    RenderBackend backend() const override { return RenderBackend::Direct2D; }
    HDC nativeHdc() const override { return hdc_; }

    void FillRect(const RECT& rc, COLORREF color) override {
        ID2D1SolidColorBrush* brush = brushCache_->Get(color);
        if (brush) dcRt_->FillRectangle(RectFromWin(rc), brush);
    }

    void FillGradientRect(const RECT& rc, COLORREF start, COLORREF end, bool vertical) override {
        if (!dcRt_) return;
        D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, ColorFromRef(start)},
            {1.0f, ColorFromRef(end)},
        };
        ID2D1GradientStopCollection* stopCollection = nullptr;
        if (FAILED(dcRt_->CreateGradientStopCollection(stops, 2, &stopCollection))) return;

        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props{};
        if (vertical) {
            props.startPoint = D2D1::Point2F(static_cast<FLOAT>(rc.left), static_cast<FLOAT>(rc.top));
            props.endPoint = D2D1::Point2F(static_cast<FLOAT>(rc.left), static_cast<FLOAT>(rc.bottom));
        } else {
            props.startPoint = D2D1::Point2F(static_cast<FLOAT>(rc.left), static_cast<FLOAT>(rc.top));
            props.endPoint = D2D1::Point2F(static_cast<FLOAT>(rc.right), static_cast<FLOAT>(rc.top));
        }

        ID2D1LinearGradientBrush* gradient = nullptr;
        if (SUCCEEDED(dcRt_->CreateLinearGradientBrush(props, stopCollection, &gradient))) {
            dcRt_->FillRectangle(RectFromWin(rc), gradient);
            gradient->Release();
        }
        stopCollection->Release();
    }

    void FillRoundRect(const RECT& rc, COLORREF color, float cornerRadius) override {
        ID2D1SolidColorBrush* brush = brushCache_->Get(color);
        if (!brush) return;
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(RectFromWin(rc), cornerRadius, cornerRadius);
        dcRt_->FillRoundedRectangle(rr, brush);
    }

    void DrawBorderRect(const RECT& rc, COLORREF color) override {
        ID2D1SolidColorBrush* brush = brushCache_->Get(color);
        if (brush) dcRt_->DrawRectangle(RectFromWin(rc), brush, 1.0f);
    }

    void DrawBorderRoundRect(const RECT& rc, COLORREF color, float cornerRadius) override {
        ID2D1SolidColorBrush* brush = brushCache_->Get(color);
        if (!brush) return;
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(RectFromWin(rc), cornerRadius, cornerRadius);
        dcRt_->DrawRoundedRectangle(rr, brush, 1.0f);
    }

    void DrawLine(int x1, int y1, int x2, int y2, COLORREF color, float strokeWidth) override {
        ID2D1SolidColorBrush* brush = brushCache_->Get(color);
        if (!brush) return;
        dcRt_->DrawLine(
            D2D1::Point2F(static_cast<FLOAT>(x1), static_cast<FLOAT>(y1)),
            D2D1::Point2F(static_cast<FLOAT>(x2), static_cast<FLOAT>(y2)),
            brush, strokeWidth);
    }

    void DrawEllipse(int left, int top, int right, int bottom,
        COLORREF color, float strokeWidth, bool filled) override {
        ID2D1SolidColorBrush* brush = brushCache_->Get(color);
        if (!brush) return;
        const D2D1_ELLIPSE ellipse = D2D1::Ellipse(
            D2D1::Point2F((left + right) * 0.5f, (top + bottom) * 0.5f),
            (right - left) * 0.5f, (bottom - top) * 0.5f);
        if (filled) dcRt_->FillEllipse(ellipse, brush);
        else dcRt_->DrawEllipse(ellipse, brush, strokeWidth);
    }

    void DrawPolygon(const POINT* pts, int count, COLORREF color, bool filled) override {
        if (!pts || count < 2) return;
        ID2D1SolidColorBrush* brush = brushCache_->Get(color);
        if (!brush) return;

        ID2D1Factory* factory = GetD2DFactory();
        if (!factory) return;

        ID2D1PathGeometry* geom = nullptr;
        if (FAILED(factory->CreatePathGeometry(&geom))) return;

        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(geom->Open(&sink))) {
            geom->Release();
            return;
        }

        sink->BeginFigure(
            D2D1::Point2F(static_cast<FLOAT>(pts[0].x), static_cast<FLOAT>(pts[0].y)),
            D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < count; ++i) {
            sink->AddLine(D2D1::Point2F(static_cast<FLOAT>(pts[i].x), static_cast<FLOAT>(pts[i].y)));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();

        if (filled) dcRt_->FillGeometry(geom, brush);
        else {
            static ID2D1StrokeStyle* roundStroke = nullptr;
            if (!roundStroke) {
                factory->CreateStrokeStyle(
                    D2D1::StrokeStyleProperties(
                        D2D1_CAP_STYLE_ROUND,
                        D2D1_CAP_STYLE_ROUND,
                        D2D1_CAP_STYLE_ROUND,
                        D2D1_LINE_JOIN_ROUND),
                    nullptr, 0, &roundStroke);
            }
            dcRt_->DrawGeometry(geom, brush, 1.5f, roundStroke);
        }
        geom->Release();
    }

    void DrawUiText(const wchar_t* text, const RECT& rc, COLORREF color, UINT format,
        HFONT font = nullptr) override {
        if (!text || !dcRt_) return;
        IDWriteTextFormat* textFormat = AcquireDWriteTextFormat(font);
        if (!textFormat) {
            HGDIOBJ oldFont = font ? SelectObject(hdc_, font) : nullptr;
            const int oldBk = SetBkMode(hdc_, TRANSPARENT);
            const COLORREF oldColor = SetTextColor(hdc_, color);
            ::DrawTextW(hdc_, text, -1, const_cast<RECT*>(&rc), format);
            SetBkMode(hdc_, oldBk);
            SetTextColor(hdc_, oldColor);
            if (oldFont) SelectObject(hdc_, oldFont);
            return;
        }
        ID2D1SolidColorBrush* brush = brushCache_->Get(color);
        if (!brush) return;

        const D2D1_RECT_F layoutRect = RectFromWin(rc);
        DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING;
        DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        if (format & DT_CENTER) hAlign = DWRITE_TEXT_ALIGNMENT_CENTER;
        else if (format & DT_RIGHT) hAlign = DWRITE_TEXT_ALIGNMENT_TRAILING;
        if (format & DT_VCENTER) vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
        else if (format & DT_BOTTOM) vAlign = DWRITE_PARAGRAPH_ALIGNMENT_FAR;

        IDWriteTextLayout* layout = nullptr;
        const size_t len = wcslen(text);
        if (FAILED(GetDWriteFactory()->CreateTextLayout(
                text, static_cast<UINT32>(len), textFormat,
                layoutRect.right - layoutRect.left,
                layoutRect.bottom - layoutRect.top, &layout))) {
            return;
        }

        layout->SetTextAlignment(hAlign);
        layout->SetParagraphAlignment(vAlign);

        if (format & DT_WORDBREAK) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        } else if (format & DT_SINGLELINE) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (format & DT_END_ELLIPSIS) {
            const DWRITE_TRIMMING trim{
                DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trim, nullptr);
        }

        dcRt_->DrawTextLayout(
            D2D1::Point2F(layoutRect.left, layoutRect.top),
            layout, brush);
        layout->Release();
    }

    void PushClipRect(const RECT& rc) override {
        if (!dcRt_) return;
        ID2D1Layer* layer = nullptr;
        if (FAILED(dcRt_->CreateLayer(&layer))) return;
        dcRt_->PushLayer(
            D2D1::LayerParameters(RectFromWin(rc)), layer);
        layer->Release();
        clipDepth_++;
    }

    void PopClip() override {
        if (!dcRt_ || clipDepth_ <= 0) return;
        dcRt_->PopLayer();
        clipDepth_--;
    }

    void Commit() override {
        if (dcRt_ && begun_) {
            dcRt_->EndDraw();
            begun_ = false;
        }
    }

    HDC hdc_ = nullptr;
    RECT bindRect_{};
    ID2D1DCRenderTarget* dcRt_ = nullptr;
    bool begun_ = false;
    int clipDepth_ = 0;
    std::unique_ptr<D2dBrushCache> brushCache_;
};

}  // namespace

std::unique_ptr<IRenderContext> CreateD2dRenderContext(HDC hdc, const RECT& bindRect) {
    auto ctx = std::make_unique<D2dRenderContext>(hdc, bindRect);
    if (ctx->IsValid()) return ctx;
    return nullptr;
}

std::unique_ptr<IRenderContext> CreateRenderContext(HDC hdc, const RECT* bindRect) {
    if (!hdc) return nullptr;

    RECT rect = bindRect ? *bindRect : InferRenderBindRect(hdc);

    if (GetPreferredRenderBackend() == RenderBackend::Direct2D
        && IsDirect2DAvailable() && IsDirectWriteAvailable()) {
        if (auto d2d = CreateD2dRenderContext(hdc, rect)) {
            return d2d;
        }
    }
    return CreateGdiRenderContext(hdc);
}

RenderContextScope::RenderContextScope(HDC hdc) {
    ctx_ = CreateRenderContext(hdc, nullptr);
}

thread_local RenderBatchScope* RenderBatchScope::current_ = nullptr;

RenderBatchScope::RenderBatchScope(HDC hdc) {
    previous_ = current_;
    targetHdc_ = hdc;
    ctx_ = CreateRenderContext(hdc, nullptr);
    current_ = this;
}

RenderBatchScope::~RenderBatchScope() {
    End();
    current_ = previous_;
}

IRenderContext* RenderBatchScope::activeFor(HDC hdc) {
    if (!current_ || !hdc) return nullptr;
    if (current_->targetHdc_ != hdc) return nullptr;
    return &current_->ctx();
}

void RenderBatchScope::End() {
    if (!ended_ && ctx_) {
        ctx_->Commit();
        ended_ = true;
    }
}

IRenderContext& ResolveRenderContext(HDC hdc) {
    if (IRenderContext* active = RenderBatchScope::activeFor(hdc)) {
        return *active;
    }
    thread_local std::unique_ptr<IRenderContext> scratch;
    scratch = CreateGdiRenderContext(hdc);
    return *scratch;
}
