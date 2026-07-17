#include "selftest_harness.h"

#include "action_utils.h"
#include "input_timeline_scheduler.h"
#include "recorder_timeline.h"

#include <atomic>
#include <vector>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"event_sort_timestamp_sequence", L"default", L"Raw/LL events sort by QPC then sequence"},
    {L"relative_delta_conserved", L"default", L"relative conversion preserves total dx/dy"},
    {L"same_timestamp_relative_merge", L"default", L"same-timestamp relative packets merge dx/dy"},
    {L"micro_gap_relative_merge", L"default", L"sub-200us relative packets merge dx/dy"},
    {L"mixed_capture_channels", L"default", L"auto mode transition keeps absolute and relative events"},
    {L"same_timestamp_button_order", L"default", L"same timestamp button down/up follows sequence"},
    {L"stop_hotkey_tail_trimmed", L"default", L"recording stop hotkey tail is removed"},
    {L"wheel_gap_becomes_wait", L"default", L"wheel pre-delay remains an explicit wait"},
    {L"compile_integer_timeline", L"default", L"action durations compile to absolute microseconds"},
    {L"legacy_wait_timeline", L"default", L"explicit wait actions advance absolute deadlines"},
    {L"random_duration_rejects_timeline", L"default", L"random jitter disables precision timeline"},
    {L"timing_us_prefers_over_duration", L"default", L"timingUs wins over duration float in compile"},
    {L"convert_sets_timing_us", L"default", L"recorded gaps become timingUs on actions"},
    {L"scheduler_cancel_interrupts", L"default", L"precision scheduler responds to cancellation"},
    {L"scheduler_wait_until_elapsed", L"default", L"absolute WaitUntilElapsedUs is interruptible"},
};

RecordedEvent Ev(uint64_t us, uint64_t seq, UINT msg, int x = 0, int y = 0,
                 WPARAM button = 0) {
    RecordedEvent e{};
    e.timeOffsetUs = us;
    e.sequence = seq;
    e.msg = msg;
    e.x = x;
    e.y = y;
    e.vkOrButton = button;
    return e;
}

void CaseSort() {
    std::vector<RecordedEvent> events{
        Ev(20, 1, WM_MOUSEMOVE), Ev(10, 4, WM_KEYDOWN),
        Ev(10, 2, WM_KEYUP)};
    SortRecordedEvents(events);
    const bool ok = events[0].sequence == 2
        && events[1].sequence == 4 && events[2].timeOffsetUs == 20;
    Emit(L"event_sort_timestamp_sequence", ok, L"");
}

void CaseRelative() {
    std::vector<RecordedEvent> events{
        Ev(100, 1, kWmRecordedRelativeMove, 4, -2),
        Ev(200, 2, kWmRecordedRelativeMove, -1, 7)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    int dx = 0, dy = 0;
    for (const auto& a : converted.actions) {
        if (a.type == ActionType::MoveMouseRelative) { dx += a.x; dy += a.y; }
    }
    Emit(L"relative_delta_conserved",
        dx == 3 && dy == 5 && converted.relativeMoveCount == 2, L"");
}

void CaseSameTimestampRelativeMerge() {
    std::vector<RecordedEvent> events{
        Ev(100, 1, kWmRecordedRelativeMove, 4, -2),
        Ev(100, 2, kWmRecordedRelativeMove, -1, 7)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    const bool ok = converted.actions.size() == 1
        && converted.actions[0].type == ActionType::MoveMouseRelative
        && converted.actions[0].x == 3
        && converted.actions[0].y == 5
        && converted.relativeMoveCount == 2;
    Emit(L"same_timestamp_relative_merge", ok, L"");
}

void CaseMicroGapRelativeMerge() {
    // <200us 但不同戳：不应再合并，以免压扁轨迹
    std::vector<RecordedEvent> events{
        Ev(1000, 1, kWmRecordedRelativeMove, 2, 3),
        Ev(1150, 2, kWmRecordedRelativeMove, 5, -1)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    const bool ok = converted.actions.size() == 2
        && converted.actions[0].x == 2 && converted.actions[1].x == 5;
    Emit(L"micro_gap_relative_merge", ok, L"");
}

void CaseMixed() {
    std::vector<RecordedEvent> events{
        Ev(0, 0, WM_MOUSEMOVE, 20, 30),
        Ev(1000, 1, kWmRecordedRelativeMove, 2, 1)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    Emit(L"mixed_capture_channels",
        converted.absoluteMoveCount == 1 && converted.relativeMoveCount == 1
            && converted.actions.size() == 2, L"");
}

void CaseButtonOrder() {
    std::vector<RecordedEvent> events{
        Ev(100, 8, WM_LBUTTONUP, 0, 0, VK_LBUTTON),
        Ev(100, 7, WM_LBUTTONDOWN, 0, 0, VK_LBUTTON)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    const bool ok = converted.actions.size() == 2
        && converted.actions[0].type == ActionType::MouseDown
        && converted.actions[1].type == ActionType::MouseUp;
    Emit(L"same_timestamp_button_order", ok, L"");
}

void CaseHotkeyTrim() {
    Hotkey hotkey{};
    hotkey.enabled = true;
    hotkey.vk = VK_F8;
    std::vector<RecordedEvent> events{
        Ev(100, 1, WM_KEYDOWN, 0, 0, 'W'),
        Ev(200, 2, WM_KEYUP, 0, 0, 'W'),
        Ev(300, 3, WM_KEYDOWN, 0, 0, VK_F8),
        Ev(310, 4, WM_KEYUP, 0, 0, VK_F8)};
    auto converted = ConvertRecordedEventsToActions(events, hotkey);
    const bool ok = converted.actions.size() == 2
        && converted.durationSeconds == 0.0002;
    Emit(L"stop_hotkey_tail_trimmed", ok, L"");
}

void CaseWheel() {
    RecordedEvent wheel = Ev(5000, 1, WM_MOUSEWHEEL);
    wheel.wheelDelta = WHEEL_DELTA;
    auto converted = ConvertRecordedEventsToActions({wheel}, {});
    const bool ok = converted.actions.size() == 2
        && converted.actions[0].type == ActionType::Wait
        && converted.actions[1].type == ActionType::ScrollWheel;
    Emit(L"wheel_gap_becomes_wait", ok, L"");
}

void CaseCompile() {
    ScriptAction a{};
    a.type = ActionType::MoveMouseRelative;
    a.duration = 0.0015;
    ScriptAction b = a;
    b.duration = 0.00225;
    const auto timeline = CompileInputTimeline({a, b});
    const bool ok = timeline.size() == 2
        && timeline[0].deadlineUs == 1500
        && timeline[1].deadlineUs == 3750;
    Emit(L"compile_integer_timeline", ok, L"");
}

void CaseLegacyWait() {
    ScriptAction wait{};
    wait.type = ActionType::Wait;
    wait.duration = 0.1;
    ScriptAction move{};
    move.type = ActionType::MoveMouseRelative;
    move.duration = 0.001;
    const auto timeline = CompileInputTimeline({wait, move});
    const bool ok = timeline.size() == 1
        && timeline[0].deadlineUs == 101000;
    Emit(L"legacy_wait_timeline", ok, L"");
}

void CaseRandomReject() {
    ScriptAction a{};
    a.type = ActionType::MoveMouseRelative;
    a.duration = 0.001;
    a.randomDuration = 0.0001;
    const auto timeline = CompileInputTimeline({a});
    Emit(L"random_duration_rejects_timeline", timeline.empty(), L"");
}

void CaseTimingUsPrefers() {
    ScriptAction a{};
    a.type = ActionType::MoveMouseRelative;
    a.duration = 0.050; // would be 50000us via float path
    a.timingUs = 49937; // exact QPC-derived gap
    ScriptAction b = a;
    b.timingUs = 1000;
    b.duration = 0.002;
    const auto timeline = CompileInputTimeline({a, b});
    const bool ok = timeline.size() == 2
        && timeline[0].deadlineUs == 49937
        && timeline[1].deadlineUs == 50937;
    Emit(L"timing_us_prefers_over_duration", ok, L"");
}

void CaseConvertSetsTimingUs() {
    std::vector<RecordedEvent> events{
        Ev(0, 1, kWmRecordedRelativeMove, 1, 0),
        Ev(12345, 2, kWmRecordedRelativeMove, 2, 0),
        Ev(13000, 3, WM_KEYDOWN, 0, 0, 'W')};
    auto converted = ConvertRecordedEventsToActions(events, {});
    const bool ok = converted.actions.size() == 3
        && converted.actions[0].timingUs == 0
        && converted.actions[1].timingUs == 12345
        && converted.actions[2].timingUs == 655;
    Emit(L"convert_sets_timing_us", ok, L"");
}

void CaseSchedulerCancel() {
    PrecisionInputTimeline timeline;
    timeline.Reset();
    const bool completed = timeline.WaitDeltaSeconds(1.0, [] { return true; });
    Emit(L"scheduler_cancel_interrupts", !completed, L"");
}

void CaseSchedulerWaitUntil() {
    PrecisionInputTimeline timeline;
    timeline.Reset();
    const bool completed = timeline.WaitUntilElapsedUs(500000, [] { return true; });
    Emit(L"scheduler_wait_until_elapsed", !completed, L"");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--json") == 0) {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--list") == 0) {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--help") == 0) {
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"RecorderSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    CaseSort();
    CaseRelative();
    CaseSameTimestampRelativeMerge();
    CaseMicroGapRelativeMerge();
    CaseMixed();
    CaseButtonOrder();
    CaseHotkeyTrim();
    CaseWheel();
    CaseCompile();
    CaseLegacyWait();
    CaseRandomReject();
    CaseTimingUsPrefers();
    CaseConvertSetsTimingUs();
    CaseSchedulerCancel();
    CaseSchedulerWaitUntil();
    selftest::EmitSummary();
    return selftest::ExitCode();
}
