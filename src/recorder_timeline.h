#pragma once

#include "recorder.h"
#include "script_types.h"
#include "utils.h"

#include <cstdint>
#include <vector>

struct RecordingConversionResult {
    std::vector<ScriptAction> actions;
    double durationSeconds = 0.0;
    size_t absoluteMoveCount = 0;
    size_t relativeMoveCount = 0;
};

/// 跨 Raw Input/LL hook 线程按 QPC 时间和全局 sequence 建立确定顺序。
void SortRecordedEvents(std::vector<RecordedEvent>& events);

/// 将录制事件转为动作：间隔为显式 Wait；键鼠瞬时动作 timingUs/duration=0。
RecordingConversionResult ConvertRecordedEventsToActions(
    std::vector<RecordedEvent> events, const Hotkey& stopHotkey,
    bool windowRelative = false);

struct TimedInputEvent {
    uint64_t deadlineUs = 0;
    ScriptAction action;
};

/// 将动作编译为整数微秒绝对时间轴（Wait 与瞬时类前延迟两种推进方式均支持）。
std::vector<TimedInputEvent> CompileInputTimeline(
    const std::vector<ScriptAction>& actions);

/// timingUs>0 优先，否则由 duration 换算微秒。
uint64_t ActionStepUs(const ScriptAction& a);

/// 录制轨迹瞬时类：Move*/MouseDown/Up/KeyDown/Up/FindImage（不含 Wait、重复间隔类）。
bool ActionCarriesRecordingPreDelay(ActionType t);

/// 构造显式 Wait（gapUs>0）；调用方勿对 0 调用。
ScriptAction MakeExplicitWaitUs(uint64_t gapUs, int indent = 0);

struct ExpandRecordingPreDelayPolicy {
    /// true：duration>ε 的瞬时类也展开为 Wait（录制路径 / version==1 / 强制）。
    /// false：仅 timingUs>0 展开；否则清零伪前延迟（如宏默认 0.1）不插 Wait。
    bool treatAsRecordingTimeline = false;
};

/// 将瞬时类上的前延迟展开为显式 Wait，并清零动作 timing/duration。
std::vector<ScriptAction> ExpandRecordingPreDelaysToExplicitWaits(
    const std::vector<ScriptAction>& actions,
    ExpandRecordingPreDelayPolicy policy);

/// 合并相邻同 indent、无 random 的 Wait（timingUs/duration 相加）。
void MergeAdjacentExplicitWaits(std::vector<ScriptAction>& actions);

/// 修复相对移动之间被 Raw 队列积压压扁的间隔（<500µs → 样本中位数或 8ms）。
/// 相邻两个 MoveMouseRelative 无 Wait 时也会插入间隔。幂等。
void RepairCompressedRelativeGaps(std::vector<ScriptAction>& actions);

/// 把「显式 Wait + 紧跟一个相对移动」的成对结构，按窗口长度自适应细分成多份，
/// 让位移铺满整个窗口（见 .cpp 里为什么这能摊平「游戏帧边界相位」的影响）。
/// 细分数上限 `maxParts`；窗口 <2ms 不细分。**保证 Σ窗口 与 Σ位移 严格不变**。
/// 默认由回放设置开关控制（默认关闭，供 A/B 对比）。幂等（细分后的份都不足 2ms）。
void SpreadRelativeMovePackets(std::vector<ScriptAction>& actions, int maxParts);

/// 回放前的**统一时间轴准备**：先修压缩间隔，再按设置决定是否细分位移铺满窗口。
///
/// ⚠ 所有回放入口都必须走这个，**不要再直接调 `RepairCompressedRelativeGaps`** ——
///   否则会出现「只有某一条路径生效」的诡异现象。
///   2026-09-21 实测踩过：`RepairCompressedRelativeGaps` 在项目里有 6 个调用点
///   （`RunCurrentActions` 里就有**两个平行块**、外加 `nested` 与 `engine_host_window.h`），
///   只补其中一个 ⇒ 用户勾了开关却完全没效果（日志里包数仍是 790、等待仍是 0.008 秒）。
void PreparePlaybackTimeline(std::vector<ScriptAction>& actions, bool spreadRelativeMoves);

/// 供诊断：最近一次 `PreparePlaybackTimeline` 的「细分前 / 细分后」相对移动包数。
/// 开关没生效时两者相等 —— 直接看日志就能判断，不必再猜。
void LastSpreadPacketCounts(uint64_t& before, uint64_t& after);

/// 当前精密轴文件语义：时间只在显式 Wait（及重复间隔类 interval）。
constexpr int kInputTimingVersionExplicitWaits = 2;

/// 脚本里相对移动的位移总量与包数 —— 回放保真度诊断用。
/// 与 `MouseBackendStats::movedDx/movedDy` 对比：
///   相等 ⇒ 输入层忠实，落点偏差来自目标侧（帧边界/游戏内非线性）；
///   不等 ⇒ 注入层改动了位移（拆包/失败/被拦），先查注入。
struct RelativeMoveTotals {
    long long dx = 0;
    long long dy = 0;
    size_t packets = 0;
};

/// 只统计 MoveMouseRelative（x/y 即 dx/dy）；不展开循环，只看当前动作列表。
///
/// ⚠ **不要**拿它直接当「回放保真」的请求侧：它是**静态条数**，脚本含 `Loop`/`Goto`
/// 时与执行次数不等（`ScriptIsTimedInputSequence` 明确把 Loop/Goto 算作时间轴脚本），
/// 静态值会把排查引到错误的一侧。请求侧要用**执行计数**（引擎里的 `reqRel*`）。
RelativeMoveTotals SumRelativeMoves(const std::vector<ScriptAction>& actions);

/// 回放保真的判定结论。
enum class MoveFidelityVerdict {
    /// 注入侧未经 SendInput 计数器（窗口模式软输入 / CDP）⇒ 恒为 0，**不可判定**。
    NotCounted,
    Match,
    Mismatch,
};

/// 判定「本轮执行到的相对位移」与「实际注入的相对位移」是否一致。
///
/// ⚠ `injectedCounted == false` 时**必须**返回 `NotCounted` —— 这是本函数存在的唯一理由：
/// 那种模式下注入侧恒为 0，拿它去比非 0 会报出「⚠ 注入层改动了位移」，
/// 在一切正常时把排查引到错误的一侧。**别把这个分支挪到 Match/Mismatch 之后。**
inline MoveFidelityVerdict EvaluateMoveFidelity(bool injectedCounted,
    long long wantDx, long long wantDy, long long gotDx, long long gotDy) {
    if (!injectedCounted) return MoveFidelityVerdict::NotCounted;
    return (wantDx == gotDx && wantDy == gotDy)
        ? MoveFidelityVerdict::Match
        : MoveFidelityVerdict::Mismatch;
}
