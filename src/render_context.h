#pragma once
// ──────────────────────────────────────────────────────────────────
// render_context.h — GDI / Direct2D 统一绘制抽象层
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <memory>
#include <string>

#include "render_device.h"

/// 绘制上下文抽象：drawing.cpp 中的图元通过此接口绘制，底层可选 GDI 或 Direct2D。
class IRenderContext {
public:
    virtual ~IRenderContext() = default;

    virtual RenderBackend backend() const = 0;
    virtual HDC nativeHdc() const = 0;

    virtual void FillRect(const RECT& rc, COLORREF color) = 0;
    virtual void FillGradientRect(const RECT& rc, COLORREF start, COLORREF end, bool vertical) = 0;
    virtual void FillRoundRect(const RECT& rc, COLORREF color, float cornerRadius) = 0;
    virtual void DrawBorderRect(const RECT& rc, COLORREF color) = 0;
    virtual void DrawBorderRoundRect(const RECT& rc, COLORREF color, float cornerRadius) = 0;

    virtual void DrawLine(int x1, int y1, int x2, int y2, COLORREF color, float strokeWidth) = 0;
    virtual void DrawEllipse(int left, int top, int right, int bottom,
        COLORREF color, float strokeWidth, bool filled) = 0;
    virtual void DrawPolygon(const POINT* pts, int count, COLORREF color, bool filled) = 0;

    virtual void DrawUiText(const wchar_t* text, const RECT& rc, COLORREF color, UINT format,
        HFONT font = nullptr) = 0;

    virtual void PushClipRect(const RECT& rc) = 0;
    virtual void PopClip() = 0;

    /// 提交缓冲绘制（Direct2D 须在 BitBlt/EndPaint 前调用）
    virtual void Commit() {}
};

std::unique_ptr<IRenderContext> CreateGdiRenderContext(HDC hdc);
std::unique_ptr<IRenderContext> CreateD2dRenderContext(HDC hdc, const RECT& bindRect);

/// 从 HDC 推断绑定区域（mem DC 用位图尺寸，否则用 clip box）
RECT InferRenderBindRect(HDC hdc);

/// 根据 HDC 创建绘制上下文（优先 Direct2D，失败则 GDI）
std::unique_ptr<IRenderContext> CreateRenderContext(HDC hdc, const RECT* bindRect = nullptr);

/// RAII：在作用域内持有绘制上下文，析构时提交 Direct2D 帧
class RenderContextScope {
public:
    explicit RenderContextScope(HDC hdc);
    IRenderContext& ctx() { return *ctx_; }
    const IRenderContext& ctx() const { return *ctx_; }

private:
    std::unique_ptr<IRenderContext> ctx_;
};

/// 绘制批次：同一 WM_PAINT 内复用单个 Direct2D/GDI 上下文，避免每图元重建。
class RenderBatchScope {
public:
    explicit RenderBatchScope(HDC hdc);
    ~RenderBatchScope();
    RenderBatchScope(const RenderBatchScope&) = delete;
    RenderBatchScope& operator=(const RenderBatchScope&) = delete;

    IRenderContext& ctx() { return *ctx_; }
    HDC targetHdc() const { return targetHdc_; }
    static IRenderContext* activeFor(HDC hdc);

    /// 将 Direct2D 帧刷入 HDC（须在 BitBlt / EndPaint 之前调用）
    void End();

private:
    std::unique_ptr<IRenderContext> ctx_;
    HDC targetHdc_ = nullptr;
    RenderBatchScope* previous_ = nullptr;
    static thread_local RenderBatchScope* current_;
    bool ended_ = false;
};

/// 解析当前批次上下文；无批次时创建一次性 GDI 上下文
IRenderContext& ResolveRenderContext(HDC hdc);
