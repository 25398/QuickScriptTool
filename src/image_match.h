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
    /// 「精确定位救援」（非归一化 SQDIFF）允许的最大降采样倍数：1=全分辨率（旧行为）。
    /// 大区域搜索时先在降采样图粗搜、再回全分辨率小窗口复算，语义与分数口径不变。
    /// 低性能模式可以调大它换取更低 CPU（默认 4）。
    int rescueMaxDownscale = 4;
};

// 图像匹配输出 (多结果)
struct ImageMatchOutput {
    bool found = false;                        // 是否有匹配
    int elapsedMs = 0;                         // 匹配耗时 (毫秒)
    std::vector<ImageMatchResult> matches;      // 匹配结果列表
    double debugBestNccPercent = 0.0;          // NCC 峰值（含未过阈值的候选，调试用）
    int debugRawCandidates = 0;                // 各引擎原始候选总数
    double debugBestPixelAgreePercent = 0.0;  // 候选里最好的像素容差分（未过阈也会记录）
    /// 未匹配的**原因**（给 AI/用户看）。典型：模板几乎是纯色 —— 归一化互相关/平方差
    /// 在零方差模板上会退化成「处处 1.0 / 处处 0」，于是报出一个假匹配（常在 (0,0)）。
    /// 这种情况必须显式说明并让调用方换手段（findColor/getColor 或换有纹理的模板）。
    std::wstring reason;
    /// 模板灰度标准差（诊断用；<0 表示没算出来）
    double debugTemplateStdDev = -1.0;
};

// ── 位图加载/保存 ──────────────────────────────────────────────
/// 加载模板/图片为 HBITMAP。内部带**解码缓存**（按 路径 + 大小 + 修改时间 失效），
/// 循环里反复找同一步模板时不再重新读盘+解码。返回句柄所有权不变：调用方负责 DeleteBitmapHandle。
HBITMAP LoadBitmapFromFile(const std::wstring& path);
bool SaveBitmapToFile(HBITMAP bitmap, const std::wstring& path);
void DeleteBitmapHandle(HBITMAP bitmap);

/// 模板解码缓存诊断/自检（hits/lookups 反映复用率；Entries 受上限约束）
uint64_t TemplateImageCacheLookups();
uint64_t TemplateImageCacheHits();
uint64_t TemplateImageCacheEvictions();
size_t TemplateImageCacheEntries();
void ClearTemplateImageCache();

/// 只查「已缓存模板的宽高」：命中返回 true 且不产生 HBITMAP；
/// 未命中返回 false（调用方再走 LoadBitmapFromFile，行为与之前一致）。
bool TryGetCachedTemplateImageSize(const std::wstring& path, int& outW, int& outH);

// ── 找图 GPU（OpenCL）加速 ─────────────────────────────────────
/// 开关本体在 `findimage_gpu.h`（进程级内联原子量，避免设置层依赖 OpenCV）：
/// `SetFindImageGpuAccel(bool)` / `FindImageGpuAccelEnabled()` / `FindImageGpuAccelActive()`。
/// 实测 2560x1440 + 96x96：CPU 46~100ms / GPU 19~35ms（≈2.9x，含上传与结果回传）；
/// 480x360 两者打平 → 面积 < 500k 像素一律走 CPU。低性能模式开启时强制回落 CPU。
/// 这里只提供诊断用的设备名。
std::wstring FindImageGpuDeviceName();

// ── 找图「上一帧命中」本地复核（快速路径）已整体撤销 ─────────────────────
// 它是一张**跨动作存活的状态表**（按请求指纹记住上一次全屏命中），
// 与「引擎只做感知 + 执行 + 如实回执、不保留跨帧世界状态」冲突：
// 一旦画面在动，本地复核要么拿旧坐标要么白跑一次小窗口，两条都不划算。
// 现在每次找图都是**真·全屏搜索**（宁可慢，不许拿旧坐标）。
// 历史判据（两个纯函数 + 一个自检用例）已删除，见 docs §19.4 / §45。

/// 从源图裁 [L,T,R,B)（右下开）并保存为 BMP。失败返回 false。
bool SaveCroppedTemplateRegion(const std::wstring& srcPath,
    int L, int T, int R, int B, const std::wstring& destPath);

// ── 屏幕捕获 ──────────────────────────────────────────────────
HBITMAP CaptureScreenRegion(int x1, int y1, int x2, int y2);
void GetVirtualScreenRect(int& x, int& y, int& w, int& h);
HBITMAP CaptureVirtualScreen(int& outX, int& outY);

// ── 模板匹配 (多引擎) ─────────────────────────────────────────
/// 按「低性能模式」同步 OpenCV 线程预算（勾选时限 1 线程，避免 matchTemplate 把
/// 全部物理核顶满导致升温）。找图入口内部已经调用，外部一般不需要手动调；
/// 自检里要断言行预算时可显式调用。
void SyncImageMatchThreadBudget();

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

/// 按搜索区中心合成模板尺寸的锚框（编辑器选偏移：把变量图区域画在屏幕中央）。
inline ImageMatchResult SynthesizeSearchRectCenterMatch(
    int x1, int y1, int x2, int y2, int templateW, int templateH) {
    ImageMatchResult m{};
    if (x2 <= x1 || y2 <= y1) return m;
    const int tw = templateW > 0 ? templateW : (x2 - x1);
    const int th = templateH > 0 ? templateH : (y2 - y1);
    const int cx = x1 + (x2 - x1) / 2;
    const int cy = y1 + (y2 - y1) / 2;
    m.found = true;
    m.scale = 1.0;
    m.score = 100.0;
    m.x = cx;
    m.y = cy;
    m.topLeftX = cx - tw / 2;
    m.topLeftY = cy - th / 2;
    m.bottomRightX = m.topLeftX + tw;
    m.bottomRightY = m.topLeftY + th;
    return m;
}

// 屏幕点击点 → 相对匹配中心的偏移（选择偏移点击位置；与 FindImageClickPoint 互逆）
inline void FindImageRelativeClickOffset(const ImageMatchResult& m, int clickX, int clickY, int& ox, int& oy) {
    int cx = 0, cy = 0;
    FindImageMatchCenter(m, cx, cy);
    ox = clickX - cx;
    oy = clickY - cy;
}

// 离点击/框选最近的命中（偏移点、相对区域必须以唯一锚框为基准）
inline const ImageMatchResult* FindNearestImageMatch(
    const std::vector<ImageMatchResult>& matches, int absX, int absY) {
    const ImageMatchResult* best = nullptr;
    double bestD = 1e100;
    for (const auto& m : matches) {
        if (!m.found) continue;
        int cx = 0, cy = 0;
        FindImageMatchCenter(m, cx, cy);
        const double dx = static_cast<double>(absX - cx);
        const double dy = static_cast<double>(absY - cy);
        const double d = dx * dx + dy * dy;
        if (!best || d < bestD) {
            best = &m;
            bestD = d;
        }
    }
    return best;
}

/// 偏移点 / 相对区域 / 回放点击都以唯一锚框为基准（与叠层拾取一致）。
inline void RestrictFindImageToSingleAnchor(ImageMatchOptions& opt) {
    opt.maxMatches = 1;
    opt.disablePyramid = true;
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
    /// 被比较位图的尺寸（差分算得出来就填；`AiJudgeUiReaction` 用它把
    /// 「大面积运动」判成**相对画面**的比例，见下）。
    int width = 0;
    int height = 0;
};

// ── 「这一击到底有没有引起反应」的判据（docs §39.4）────────────────────
//
// 由来（实测第十九份日志）：`reacted` 原本的判据是 **baseline 与当前帧的任意差异**，
// 而游戏画面每帧都在重绘 ⇒ `reacted` 必真 ⇒ 结局只可能是 `Settled`/`StillChanging`，
// **`NoReaction` 在动态画面上不可达**。于是一整串以「无反应」为前置的闸门全部失效：
// 死点表（§35）、布局记忆/定位缓存作废、同一目标连败表，以及 §36/§38 的
// 「界面没有变化 → 这一击很可能没生效」。日志里的样子是**自相矛盾的两句话**：
//   `UI settle：仍在变化 耗时1473ms 差分0bp`  ← 末段差分 0 却说仍在变化
//   `批量逐步校验：点击 12 次，首次点击附近无变化（整屏无小范围变化）`
// 两句都来自同一批数据 —— 因为我们**算出了**「没有小范围变化」，却只拿它写措辞。

/// 「局部变化」与「大面积运动」的分界（与回执侧 `compactRoiNear`/`largeMotion` 同一把尺，
/// 原本那三个阈值硬编码在 `engine_script_run.cpp` 的措辞里，现在收上来两处共用）。
///
/// ⚠⚠ **绝对像素阈值是错的，必须相对画面**（docs §40.1，实测踩到）：
///   这张卡牌高亮变了 `263×104`（占 2560×1440 的 **0.74%**），却因为 `263 > kAiReactionMaxRoiW`
///   被判成「大面积运动」⇒ **落在落点上的真实反应被丢掉**，同一批数据还因此写出
///   「算反应」与「第一次点击附近没有任何结构变化」两句互相拆台的话。
///   同一画面里真正的「整片在动」是 `1057×1143`（**32.8%**）。差 44 倍的两个东西
///   不该靠一个固定像素数分开 —— 画面尺寸一变（小窗口 / 区域截图）绝对阈值全失真。
constexpr double kAiReactionLargeAreaFrac = 0.06;   ///< 占画面面积 ≥ 6% → 大面积运动
constexpr double kAiReactionLargeSpanFrac = 0.55;   ///< 或横纵都跨过画面 55% → 大面积运动

/// 旧的绝对阈值：**只在拿不到画面尺寸时**兜底（保证纯函数在无尺寸信息下仍可用）。
constexpr int kAiReactionMaxRoiW = 220;
constexpr int kAiReactionMaxRoiH = 180;
constexpr int kAiReactionMaxRoiArea = 48000;

/// 这个变化区算「某个具体元素在动（局部）」还是「整片在动（大面积运动）」。
/// `frameW/frameH` 传 0 → 退回绝对阈值（旧行为）。
bool AiRoiIsLocalMotion(const ScreenChangeRoi& r, int frameW, int frameH);

enum class AiUiReactionKind {
    None = 0,         ///< 一个结构变化区都没有
    NearInput,        ///< 局部变化，且落在本批落点附近 —— 最可信（**唯一算反应的一档**）
    AwayFromInput,    ///< 有局部变化，但全不在落点附近（面板在别处打开 / 画面自身在动）
    LargeMotionOnly,  ///< **只有大面积运动**（动画/视频/游戏画面）
};

struct AiUiReactionVerdict {
    AiUiReactionKind kind = AiUiReactionKind::None;
    /// 有没有「这一击生效了」的**正证据** —— 驱动 settle 结局与回执措辞。
    bool reacted = false;
    /// 判决是否**确定** —— 驱动死点表 / 连败表 / 缓存作废这类**惩罚性**动作。
    ///
    /// ⚠⚠ 为什么必须和 `reacted` 分开（docs §40.1，实测踩到）：
    ///   动态画面上「画面自己在动」既不能证明生效、**也不能证明没生效**。
    ///   把这种「无法归因」当成「确定没反应」去记死点表，会让**正确的**点击
    ///   被永久拦下（§36.6「守卫不能把模型永久锁在外面」的同一类事故）。
    ///   只有「画面一动都没动」才是确定的「没反应」。
    bool conclusive = false;
    /// 诊断用短说明（进 settle 日志，**能复算**：写清是几个框、几个算局部）
    std::wstring why;
};

/// `rois` = 相对 baseline 的结构变化区（**位图局部坐标**）；
/// `inputs` = 本批**真正落点**（同坐标空间；空 = 键盘/无落点动作）；
/// `dynamicForeground` = 前台是游戏/自绘/持续动画画面；
/// `frameW/frameH` = 被比较位图尺寸（决定「大面积」的相对比例；0 = 退回绝对阈值）。
///
/// ⚠⚠ **只在 `dynamicForeground` 时收紧 —— 这不是偷懒，是必需的边界**：
///   静态界面上「一大片全变了」是**真的重绘**（点链接整页跳转就是一个巨框），
///   把它判成「没反应」会让一个**正确的**点击进死点表 ⇒ 模型再也点不动它
///   （§36.6 同一类事故）。而动态画面上「整片都在动」恰恰是**背景动画**。
///
/// ⚠⚠ **动态前台只有「落点附近有局部变化」算反应**（docs §40.1，实测踩到）：
///   第一版把「局部变化在**别处**」也算成反应（理由：面板可能在别处打开），
///   但动态画面上实体/计时器**每帧都在产生局部变化** —— 实测一整局**从没出现过
///   `局部 0`**，这一档因此**恒真 ⇒ 零信息量**，等于把 `reacted` 又变回「必真」：
///   模型每次点完都被告知「已经生效，不要重做」，于是同一张卡连点 3 次、
///   同一坐标空转 8 轮。**恒真的判据不是判据**（§39.4 那个 bug 的同一个形状）。
AiUiReactionVerdict AiJudgeUiReaction(const std::vector<ScreenChangeRoi>& rois,
    const std::vector<POINT>& inputs, bool dynamicForeground,
    int nearPx = 48, int frameW = 0, int frameH = 0);

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
    /// 本批**真正落点**（**位图局部坐标** —— 调用方先减掉截图区域原点）。
    /// 空 = 键盘/无落点动作。判据见 `AiJudgeUiReaction`（docs §39.4）。
    std::vector<POINT> inputPoints;
    /// 前台是游戏/自绘/持续动画画面（`AiActionGameForegroundLikely`）。
    /// 只有它为真时才收紧「算不算反应」—— 理由见 `AiJudgeUiReaction` 的注释。
    bool dynamicForeground = false;
    /// 「落点附近」的半径
    int inputNearPx = 48;
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
    /// 这个「无反应」判决是否**确定**（见 `AiUiReactionVerdict::conclusive`）。
    /// false = 画面自己在动、无法归因 ⇒ **只影响措辞，不许触发死点表等惩罚**。
    bool reactionConclusive = true;
    double lastChangedRatio = 0.0;
    std::vector<ScreenChangeRoi> lastChangeRois;
    /// 反应判据为什么这么判（§39.4）——进日志，别让人靠猜
    std::wstring reactionWhy;
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
