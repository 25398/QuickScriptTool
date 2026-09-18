// color_match.h — 屏幕取色 / 找色 / 颜色匹配
#pragma once

#include <windows.h>

#include <atomic>
#include <string>

struct ColorMatchHit {
    bool found = false;
    int x = 0;
    int y = 0;
    int r = 0;
    int g = 0;
    int b = 0;
    int distance = 0;  // 与目标色的 max 通道差或 RGB 距离
};

/// 解析 "#RRGGBB" / "RRGGBB" / "r,g,b"
bool TryParseColorSpec(const std::wstring& text, int& outR, int& outG, int& outB);

std::wstring FormatColorHex(int r, int g, int b);

/// 单通道最大差（0~255）
int ColorChannelDistance(int r1, int g1, int b1, int r2, int g2, int b2);

bool ColorsMatch(int r1, int g1, int b1, int r2, int g2, int b2, int tolerance);

/// 点击效果取样：5×5、步长 8px（约 ±16px 邻域），避免只打在数字/透明像素上漏检。
constexpr int kClickColorGridN = 25;

bool SampleClickColorGrid(int cx, int cy, int* outR, int* outG, int* outB,
    HBITMAP frozenScreen = nullptr, int frozenVirtX = 0, int frozenVirtY = 0);

/// 邻域内至少 minChanged 个点超出容差则视为外观已变。
bool ClickColorGridChanged(
    const int* beforeR, const int* beforeG, const int* beforeB,
    const int* afterR, const int* afterG, const int* afterB,
    int tolerance, int minChanged = 2);

/// 屏幕坐标取色（可选锁屏位图）
bool GetScreenPixelRgb(int x, int y, int& outR, int& outG, int& outB,
    HBITMAP frozenScreen = nullptr, int frozenVirtX = 0, int frozenVirtY = 0);

/// 在区域内找最接近目标色的像素（扫描步长 stepPx，默认 2）。
/// 一次截屏/读入位图后在内存扫描；勿逐点 GetPixel（大区域会卡死数分钟）。
/// cancelFlag 为 true 时提前返回 found=false。
ColorMatchHit FindColorInScreenRegion(
    int searchX1, int searchY1, int searchX2, int searchY2,
    int targetR, int targetG, int targetB, int tolerance,
    HBITMAP frozenScreen = nullptr, int frozenVirtX = 0, int frozenVirtY = 0,
    int stepPx = 2,
    const std::atomic_bool* cancelFlag = nullptr);

/// 判定 (x,y) 处颜色是否匹配目标
bool MatchColorAtScreenPoint(int x, int y,
    int targetR, int targetG, int targetB, int tolerance,
    int* outR = nullptr, int* outG = nullptr, int* outB = nullptr, int* outDist = nullptr,
    HBITMAP frozenScreen = nullptr, int frozenVirtX = 0, int frozenVirtY = 0);
