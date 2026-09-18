#pragma once
// ──────────────────────────────────────────────────────────────────
// drag_pick_overlay.h — 冻结画面上拖一条线，记下起点/终点/时长
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>

struct DragPickOutcome {
    bool ok = false;
    bool cancelled = false;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    double durationSec = 0.0;
};

/// 全屏冻结截图上拖拽；返回虚拟屏绝对坐标。
DragPickOutcome ShowScreenDragPickOverlay();

/// 在模板图上拖拽；返回相对图像中心的像素偏移（可负）。
DragPickOutcome ShowTemplateDragPickOverlay(HBITMAP templateBmp, int imgW, int imgH);
