#pragma once
// ──────────────────────────────────────────────────────────────────
// drawing.h — 图形绘制辅助函数（声明）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <functional>

#include "config.h"
#include "render_context.h"

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

void DrawCrosshairGlyph(IRenderContext& ctx, int cx, int cy, int size,
    COLORREF lineColor, COLORREF fillColor, bool withFill,
    int stroke = 2);

HCURSOR CreateCrosshairDragCursor(COLORREF crosshairColor);

/// 简约线框时钟：12 点与 3 点指针 + 中心圆点（用于「定时」等功能入口）
void DrawClockGlyph(HDC hdc, int cx, int cy, int size, COLORREF color, int stroke = 2);

/// 标准箭头指针（取自系统 IDC_ARROW 轮廓，实心填充）
void DrawPointerCursorGlyph(HDC hdc, int cx, int cy, int size, COLORREF color);

void DrawBorderRect(HDC hdc, const RECT& rc, COLORREF color);
void FillRectColor(HDC hdc, const RECT& rc, COLORREF color);
void FillGradientRect(HDC hdc, const RECT& rc, COLORREF start, COLORREF end, bool vertical);
void FillAlphaRect(HDC hdc, const RECT& rc, COLORREF color, BYTE alpha);
void FillRoundRectColor(HDC hdc, const RECT& rc, COLORREF color, int cornerRadius);
void DrawFilledTriangle(HDC hdc, const POINT pts[3], COLORREF color);
void DrawExpandTriangle(HDC hdc, const RECT& rc, bool expanded, COLORREF color);
void DrawComboDownArrow(HDC hdc, int centerX, int centerY, COLORREF color = kMainGreen);
void DrawTopActionGlyph(HDC hdc, const RECT& rc, int iconType);
void DrawNavIcon(HDC hdc, const RECT& rc, int iconType, HFONT homeTabFont = nullptr);
void DrawHomeRadio(HDC hdc, const RECT& rc, bool checked);
void DrawRecorderEmptyIcon(HDC hdc);
void DrawBorderRoundRect(HDC hdc, const RECT& rc, COLORREF color, int cornerRadius);
void DrawCheckbox(HDC hdc, const RECT& rc, bool checked);

/// 单选圆钮：外圈描边 + 选中时内填绿点
void DrawRadioButton(HDC hdc, const RECT& rc, bool checked);

/// 应用品牌鼠标图标（图4造型；fillGreenCircle=true 时为绿底白线）
void DrawAppTitleGlyph(HDC hdc, const RECT& circleBounds, COLORREF color, float stroke = 2.0f,
    bool fillGreenCircle = false);

/// 六齿设置齿轮（描边：外圈齿廓 + 内圆）
void DrawGearGlyph(HDC hdc, const RECT& rc, COLORREF color, COLORREF holeColor);

/// 读取 HDC 当前字体绘制文本（Direct2D 批次模式下须先 SelectObject 字体）
void DrawTextIn(HDC hdc, const std::wstring& text, RECT rc, COLORREF color,
    UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
void DrawTextIn(HDC hdc, const wchar_t* text, RECT rc, COLORREF color,
    UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
