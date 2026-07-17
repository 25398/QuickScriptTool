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

/// 将录制事件转换成兼容现有 JSON 的动作；duration 仍保存前延迟。
RecordingConversionResult ConvertRecordedEventsToActions(
    std::vector<RecordedEvent> events, const Hotkey& stopHotkey);

struct TimedInputEvent {
    uint64_t deadlineUs = 0;
    ScriptAction action;
};

/// 将兼容动作编译为整数微秒绝对时间轴，避免回放逐步浮点累加漂移。
std::vector<TimedInputEvent> CompileInputTimeline(
    const std::vector<ScriptAction>& actions);
