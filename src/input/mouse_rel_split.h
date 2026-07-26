#pragma once

#include <utility>
#include <vector>

/// 把一次相对移动拆成 |分量|≤maxStep 的步进。
/// 用途：SendInput 在系统鼠标加速未真正关闭时，超过阈值的包会被加倍；
/// Raw 录制不受加速影响，拆包可避免回放过冲。maxStep 建议 4（默认第一阈值约 6）。
inline void AppendSubThresholdRelativeSteps(
    int dx, int dy, int maxStep,
    std::vector<std::pair<int, int>>& out) {
    if (dx == 0 && dy == 0) return;
    if (maxStep < 1) maxStep = 1;
    while (dx != 0 || dy != 0) {
        int sx = dx;
        if (sx > maxStep) sx = maxStep;
        if (sx < -maxStep) sx = -maxStep;
        int sy = dy;
        if (sy > maxStep) sy = maxStep;
        if (sy < -maxStep) sy = -maxStep;
        out.emplace_back(sx, sy);
        dx -= sx;
        dy -= sy;
    }
}
