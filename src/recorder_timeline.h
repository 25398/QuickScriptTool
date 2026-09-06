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

/// 当前精密轴文件语义：时间只在显式 Wait（及重复间隔类 interval）。
constexpr int kInputTimingVersionExplicitWaits = 2;
