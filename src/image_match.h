// ──────────────────────────────────────────────────────────────────
// image_match.h — 图像匹配 API
// 基于 OpenCV 的模板匹配，支持屏幕截图、多引擎共识匹配和 SIMD 验证。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

// 图像匹配结果
struct ImageMatchResult {
    bool found = false;       // 是否找到匹配
    int x = 0;                // 匹配中心 X
    int y = 0;                // 匹配中心 Y
    int topLeftX = 0;         // 匹配区域左上 X
    int topLeftY = 0;         // 匹配区域左上 Y
    int bottomRightX = 0;     // 匹配区域右下 X
    int bottomRightY = 0;     // 匹配区域右下 Y
    double score = 0.0;       // 匹配置信度 (百分比)
    double scale = 1.0;       // 使用的模板缩放比
};

// 图像匹配选项
struct ImageMatchOptions {
    double thresholdPercent = 65.0;  // 最小匹配置信度 (%)
    double scaleMin = 1.0;           // 最小缩放比
    double scaleMax = 1.0;           // 最大缩放比
    double scaleStep = 0.05;         // 缩放步长
    int maxMatches = 20;             // 最多返回匹配数
    double maxOverlap = 0.5;         // 最大重叠比例 (NMS 参数)
};

// 图像匹配输出 (多结果)
struct ImageMatchOutput {
    bool found = false;                        // 是否有匹配
    int elapsedMs = 0;                         // 匹配耗时 (毫秒)
    std::vector<ImageMatchResult> matches;      // 匹配结果列表
};

// ── 位图加载/保存 ──────────────────────────────────────────────
HBITMAP LoadBitmapFromFile(const std::wstring& path);
bool SaveBitmapToFile(HBITMAP bitmap, const std::wstring& path);
void DeleteBitmapHandle(HBITMAP bitmap);

// ── 屏幕捕获 ──────────────────────────────────────────────────
HBITMAP CaptureScreenRegion(int x1, int y1, int x2, int y2);
void GetVirtualScreenRect(int& x, int& y, int& w, int& h);
HBITMAP CaptureVirtualScreen(int& outX, int& outY);

// ── 模板匹配 (多引擎) ─────────────────────────────────────────
ImageMatchOutput FindTemplateOnScreenMulti(
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, const ImageMatchOptions& options);

ImageMatchOutput FindTemplateInFrozenScreenMulti(
    HBITMAP frozenScreen, int virtX, int virtY,
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, const ImageMatchOptions& options);

// ── 模板匹配 (单引擎, 向后兼容) ──────────────────────────────
ImageMatchResult FindTemplateOnScreen(
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, double thresholdPercent, double scale,
    double scaleMax = 0.0);

ImageMatchResult FindTemplateInFrozenScreen(
    HBITMAP frozenScreen, int virtX, int virtY,
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, double thresholdPercent, double scale,
    int* outTemplateW = nullptr, int* outTemplateH = nullptr,
    double scaleMax = 0.0);

// 将找图结果规范为「保存到变量」语义：未找到或匹配度未超过阈值时全部归零
inline ImageMatchResult NormalizeMatchVarResult(ImageMatchResult match, double thresholdPercent) {
    const double threshold = std::clamp(thresholdPercent, 1.0, 100.0);
    if (!match.found || match.score <= threshold) {
        return ImageMatchResult{};
    }
    return match;
}

// ── 辅助功能 ──────────────────────────────────────────────────
void SendMouseWheel(int steps, bool vertical, bool horizontal, bool positive);
