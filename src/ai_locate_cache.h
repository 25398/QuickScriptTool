#pragma once
// ──────────────────────────────────────────────────────────────────
// ai_locate_cache.h — 定位结果模板缓存（省掉重复的 VLM 识图）
//
// 场景：同一个 AI 动作在循环里反复点同一个目标（「下一题」「保存」…），
// 每轮都整屏截图 + VLM 定位纯属浪费；目标若在屏幕上没动，模板匹配（本地、毫秒级）
// 就能给出同一个点。
//
// 安全第一：命中必须同时满足「窗口键一致 + 模板高分 + 唯一命中 + 距缓存点不远」，
// 任何一条不满足都回落 VLM。并且**点击后画面没变就立刻作废该条缓存**——
// 宁可丢掉优化，也不能固化一个错点。
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>

/// 缓存键：短标签 + 窗口身份 + 观察区尺寸。
/// 窗口标题/类名或分辨率一变，坐标含义就变了，必须重新识图。
struct AiLocateCacheKey {
    std::wstring target;
    std::wstring windowTitle;
    std::wstring windowClass;
    int captureW = 0;
    int captureH = 0;
};

/// 把自然语言短标签归一成缓存键（去空白、统一小写、剥通用后缀）。
std::wstring AiLocateNormalizeTarget(const std::wstring& target);

/// 键的字符串形式（供日志/自检；不参与匹配逻辑）
std::wstring AiLocateCacheKeyString(const AiLocateCacheKey& key);

struct AiLocateCacheEntry {
    bool valid = false;
    /// 模板位图归缓存所有；调用方只读，不得 DeleteObject
    HBITMAP tmpl = nullptr;
    int screenX = 0;
    int screenY = 0;
    int boxW = 0;
    int boxH = 0;
    int hits = 0;
    /// 写入时的前台窗口矩形：窗口被拖动/移动后靠它把缓存点重新锚定
    RECT windowRect{};
    bool hasWindowRect = false;
};

/// 写入/覆盖同键条目（接管 tmpl 所有权；tmpl 为空或 lowFeature 时不入缓存）。
/// windowRect 可选：当前前台窗口矩形（窗口移动后仍能用缓存）。
void AiLocateCacheStore(const AiLocateCacheKey& key, HBITMAP tmpl,
    int screenX, int screenY, int boxW, int boxH, const RECT* windowRect = nullptr);

/// 查询（不转移所有权；miss 时 *out 保持 valid=false）
bool AiLocateCacheLookup(const AiLocateCacheKey& key, AiLocateCacheEntry* out);

/// 作废一条（点击后画面没变 → 这条多半是错点，下次老实走识图）
void AiLocateCacheInvalidate(const AiLocateCacheKey& key);

/// 记录一次命中（命中计数用于诊断）
void AiLocateCacheNoteHit(const AiLocateCacheKey& key);

/// 释放全部模板位图（AI 动作结束/会话重置时调用）
void AiLocateCacheClear();

/// 当前缓存条数（自检/诊断）
int AiLocateCacheSize();

// ── 命中判定（纯函数，可自检）─────────────────────────────────────
struct AiLocateCacheAcceptInput {
    /// 模板匹配最佳分（百分比）
    double bestScore = 0.0;
    /// 次佳分；<0 表示没有第二个命中
    double secondScore = -1.0;
    /// 最佳命中相对缓存点的距离（像素）
    int bestDistToCached = 0;
    /// 次佳命中相对缓存点的距离；<0 表示无
    int secondDistToCached = -1;
};

/// 是否接受缓存命中。接收门槛：最佳分 ≥ minScore，且
/// （没有次佳，或次佳明显更低 / 明显更远）——即「高分且唯一」。
bool ShouldAcceptAiLocateCacheHit(const AiLocateCacheAcceptInput& in,
    double minScore = 92.0, double secondScoreGap = 6.0, int secondDistGapPx = 40);

/// 缓存点按窗口位移重锚：窗口被拖动/移动后，模板仍在窗口内同一相对位置。
/// 只做纯算术（可自检）：expected = stored + (currentRect.TL - storedRect.TL)。
void AiLocateCacheReanchor(int storedX, int storedY, const RECT& storedRect,
    const RECT& currentRect, int* outX, int* outY);

/// N-of-M 迟滞：M 次快速采样里至少 N 次通过才算命中。
/// 依据（实数）：单帧检测摆动在 60~70%（最低 40%），
/// 「保留出现在 >50% 帧里的目标」后稳定率 80~100%。画面在动的场景（游戏/视频）
/// 单帧就点很容易点在残影上，宁可多花两次毫秒级小区域匹配。
bool ShouldAcceptAiLocateCacheHitHysteresis(int passCount, int sampleCount,
    int needPass = 2, int needSamples = 3);
