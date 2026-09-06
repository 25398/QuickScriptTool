// ──────────────────────────────────────────────────────────────────
// image_match.h — 图像匹配 API
// 基于 OpenCV 的模板匹配，支持屏幕截图、多引擎共识匹配和 SIMD 验证。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <functional>
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
    bool crossResolutionMatch = false; // 跨分辨率：放宽候选阈值
    bool disablePyramid = false;       // 固定尺度时禁用金字塔（位置更准、更快）
    /// 完美匹配：NCC 粗定位 → 邻域精修 → 逐像素终审（过则 score=100，否则未找到）
    bool perfectMatch = false;
    /// 完美匹配每通道允许的最大绝对差（默认 1，吸收截图 1LSB 抖动）
    int perfectMatchChannelTol = 1;
};

// 图像匹配输出 (多结果)
struct ImageMatchOutput {
    bool found = false;                        // 是否有匹配
    int elapsedMs = 0;                         // 匹配耗时 (毫秒)
    std::vector<ImageMatchResult> matches;      // 匹配结果列表
    double debugBestNccPercent = 0.0;          // NCC 峰值（含未过阈值的候选，调试用）
    int debugRawCandidates = 0;                // 各引擎原始候选总数
};

// ── 位图加载/保存 ──────────────────────────────────────────────
HBITMAP LoadBitmapFromFile(const std::wstring& path);
bool SaveBitmapToFile(HBITMAP bitmap, const std::wstring& path);
void DeleteBitmapHandle(HBITMAP bitmap);

/// 从源图裁 [L,T,R,B)（右下开）并保存为 BMP。失败返回 false。
bool SaveCroppedTemplateRegion(const std::wstring& srcPath,
    int L, int T, int R, int B, const std::wstring& destPath);

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
// perfectMatch：像素终审已给出 found/score=100，不再用阈值二次砍掉
inline ImageMatchResult NormalizeMatchVarResult(ImageMatchResult match, double thresholdPercent,
                                                bool perfectMatch = false) {
    if (perfectMatch) {
        return match.found ? match : ImageMatchResult{};
    }
    const double threshold = std::clamp(thresholdPercent, 1.0, 100.0);
    if (!match.found || match.score <= threshold) {
        return ImageMatchResult{};
    }
    return match;
}

// 匹配区域几何中心（比 match.x/y 更可靠，避免多尺度匹配时中心偏移）
inline void FindImageMatchCenter(const ImageMatchResult& m, int& cx, int& cy) {
    if (!m.found) {
        cx = cy = 0;
        return;
    }
    cx = m.topLeftX + (m.bottomRightX - m.topLeftX) / 2;
    cy = m.topLeftY + (m.bottomRightY - m.topLeftY) / 2;
}

inline void FindImageClickPoint(const ImageMatchResult& m, int offsetX, int offsetY, int& tx, int& ty) {
    int cx = 0, cy = 0;
    FindImageMatchCenter(m, cx, cy);
    tx = cx + offsetX;
    ty = cy + offsetY;
}

// 屏幕点击点 → 相对匹配中心的偏移（选择偏移点击位置；与 FindImageClickPoint 互逆）
inline void FindImageRelativeClickOffset(const ImageMatchResult& m, int clickX, int clickY, int& ox, int& oy) {
    int cx = 0, cy = 0;
    FindImageMatchCenter(m, cx, cy);
    ox = clickX - cx;
    oy = clickY - cy;
}

// ── 辅助功能 ──────────────────────────────────────────────────
void SendMouseWheel(int steps, bool vertical, bool horizontal, bool positive);

// ── 全屏差分 / UI settle（Agent 观察：反应→稳定，无固定死延时）────────
struct ScreenChangeRoi {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;   // 相对位图坐标，右下开
    int areaPx = 0;
};

/// 网格忙碌掩码：连续多帧都在变的格子=视频/动画等动态区（应忽略）
struct ScreenBusyMask {
    int width = 0;
    int height = 0;
    int cellW = 32;
    int cellH = 32;
    int cols = 0;
    int rows = 0;
    /// 1=该格持续运动（动态噪声）
    std::vector<uint8_t> busy;
    double busyCoverageRatio = 0.0;

    bool valid() const {
        return width > 0 && height > 0 && cols > 0 && rows > 0
            && static_cast<int>(busy.size()) == cols * rows;
    }
    bool isBusyPixel(int x, int y) const {
        if (!valid() || x < 0 || y < 0 || x >= width || y >= height) return false;
        const int c = std::min(cols - 1, x / std::max(1, cellW));
        const int r = std::min(rows - 1, y / std::max(1, cellH));
        return busy[static_cast<size_t>(r * cols + c)] != 0;
    }
};

struct ScreenChangeDiffResult {
    bool ok = false;
    bool sameSize = false;
    /// 结构差分占比（已忽略动态忙碌格）0~1
    double changedRatio = 0.0;
    /// 未掩码前的原始差分占比
    double rawChangedRatio = 0.0;
    /// 忙碌格覆盖占比
    double busyCoverageRatio = 0.0;
    /// 原始变化大、结构变化很小 → 只有视频/动画在动
    bool onlyDynamicChanged = false;
    /// 通道容差内近似完美一致（结构）
    bool nearlyIdentical = false;
    /// 结构变化连通块（已排除忙碌格主导区）
    std::vector<ScreenChangeRoi> rois;
};

/// 三帧采样：两段都在变的格子标为忙碌（适配视频播放等持续动态区）
ScreenBusyMask BuildBusyMaskFromTripleFrames(
    HBITMAP f0, HBITMAP f1, HBITMAP f2,
    int channelTol = 12,
    int cellSize = 32,
    double cellChangeFrac = 0.08);

/// 两帧同尺寸位图像素差分 + 变化区域；可选忽略忙碌掩码
ScreenChangeDiffResult DiffBitmapsChangedRegions(
    HBITMAP a, HBITMAP b,
    int channelTol = 12,
    int minBlobArea = 48,
    int maxRois = 8,
    const ScreenBusyMask* busyMask = nullptr);

struct UiVisualSettleOptions {
    int pollIntervalMs = 180;
    int maxTotalMs = 15000;
    /// 多久内应看到相对基线的变化（第一次校验：有反应）
    int reactDeadlineMs = 5000;
    /// 连续稳定多久算加载告一段落（第二次校验：不再跳变）
    int stableHoldMs = 550;
    /// 超过则建议刷新/重开（给 Agent，而非宿主硬刷）
    int refreshSuggestMs = 10000;
    double reactedMinChangedRatio = 0.0008;
    double stableMaxChangedRatio = 0.0015;
    int channelTol = 12;
};

struct UiVisualSettleResult {
    enum class Outcome {
        Settled,        // 先变化后稳定
        StillChanging,  // 超时仍在跳变（可能仍在加载）
        NoReaction,     // 相对基线几乎无变化
        Cancelled
    } outcome = Outcome::NoReaction;
    int elapsedMs = 0;
    bool reacted = false;
    bool settled = false;
    bool suggestRefresh = false;
    double lastChangedRatio = 0.0;
    std::vector<ScreenChangeRoi> lastChangeRois;
    /// 最后一帧（调用方负责 DeleteBitmapHandle；失败为 nullptr）
    HBITMAP lastFrame = nullptr;
    std::wstring logLine;
    std::wstring agentHint;
};

/// 第一次：相对 baseline 出现变化；第二次：连续帧稳定。capture 每次返回新 HBITMAP。
UiVisualSettleResult WaitUiReactThenSettle(
    HBITMAP baseline,
    const std::function<HBITMAP()>& capture,
    const std::atomic_bool& stopFlag,
    const UiVisualSettleOptions& opts = {});
