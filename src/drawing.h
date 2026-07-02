#pragma once
// ──────────────────────────────────────────────────────────────────
// drawing.h — 图形绘制辅助函数（声明）
// 提供准星图标绘制、拖拽光标创建等 GDI 绘制工具
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

/// 绘制准星十字标靶图形
/// @param hdc 设备上下文
/// @param cx, cy 中心坐标
/// @param size 外圆直径
/// @param lineColor 线条颜色
/// @param fillColor 填充颜色（withFill=true时使用）
/// @param withFill 是否绘制填充背景色
/// @param stroke 线条宽度（默认2）
void DrawCrosshairGlyph(HDC hdc, int cx, int cy, int size,
    COLORREF lineColor, COLORREF fillColor, bool withFill,
    int stroke = 2);

/// 创建半透明准星拖拽光标（用于"拖动准星获取坐标"功能）
/// @return HCURSOR 句柄，调用方负责销毁
HCURSOR CreateCrosshairDragCursor(COLORREF crosshairColor);
