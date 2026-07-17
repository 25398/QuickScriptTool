#pragma once
// ──────────────────────────────────────────────────────────────────
// ui_scale.h — 统一 UI 分辨率缩放
// 设计基准：2560×1440 屏幕下首页 720×540、编辑器 1200×1080（缩放 100%）
// 较小分辨率等比缩小；不随 Windows DPI 百分比放大（避免与标准布局不一致）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

// 从主显示器或窗口所在显示器初始化/更新缩放比例
void UiScaleInitFromPrimaryMonitor();
void UiScaleInitFromHwnd(HWND hwnd);
// 显示器/DPI 变化时刷新（按分辨率重算，忽略 DPI 数值）
void UiScaleSetFromDpi(int dpi, HWND hwnd = nullptr);
void UiScaleSetPercent(int percent);

// 当前缩放百分比（2560×1440 = 100）
int UiScalePercent();

// 将设计稿像素换算为当前缩放下的物理像素
int UiLen(int designPx);

int UiHomeWidth();
int UiHomeHeight();
int UiEditorWidth();
int UiEditorHeight();

RECT UiRect4(int left, int top, int right, int bottom);
int UiFontHeight(int designHeight);

// 将窗口客户区调整为当前缩放下的首页/编辑器设计尺寸
void UiResizeWindowClient(HWND hwnd, int clientW, int clientH, bool force = false);
void UiResizeHwndToHome(HWND hwnd);
void UiResizeHwndToEditor(HWND hwnd);

// 在已缩放容器内再按设计稿 inset（用于卡片、横幅等内部布局）
inline RECT UiInsetRect(const RECT& base, int left, int top, int rightInset, int bottomY) {
    return RECT{
        base.left + UiLen(left),
        base.top + UiLen(top),
        base.right - UiLen(rightInset),
        base.top + UiLen(bottomY)
    };
}

inline RECT UiEdgeRect(const RECT& base, int rightStart, int top, int rightEnd, int bottomY) {
    return RECT{
        base.right - UiLen(rightStart),
        base.top + UiLen(top),
        base.right - UiLen(rightEnd),
        base.top + UiLen(bottomY)
    };
}

inline RECT UiPadRect(const RECT& base, int padLeft, int padTop, int padRight, int padBottom) {
    return RECT{
        base.left + UiLen(padLeft),
        base.top + UiLen(padTop),
        base.right - UiLen(padRight),
        base.bottom - UiLen(padBottom)
    };
}
