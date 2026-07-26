#include "selftest_harness.h"

#include "action_utils.h"
#include "coord_space.h"
#include "input/mouse_rel_split.h"
#include "input_timeline_scheduler.h"
#include "recorder_timeline.h"
#include "recording_to_findimage.h"

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"event_sort_timestamp_sequence", L"default", L"Raw/LL events sort by QPC then sequence"},
    {L"relative_delta_conserved", L"default", L"relative conversion preserves total dx/dy"},
    {L"same_timestamp_relative_keep", L"default", L"same-timestamp relative packets stay separate"},
    {L"micro_gap_relative_merge", L"default", L"sub-200us relative packets stay separate and gaps inflate"},
    {L"repair_compressed_rel_gaps", L"default", L"compressed Rel-Wait-Rel gaps inflate to median report interval"},
    {L"sub_threshold_rel_split", L"default", L"large relative packets split below accel threshold"},
    {L"mixed_capture_channels", L"default", L"auto mode transition keeps absolute and relative events"},
    {L"same_timestamp_button_order", L"default", L"same timestamp button down/up follows sequence"},
    {L"stop_hotkey_tail_trimmed", L"default", L"recording stop hotkey tail is removed"},
    {L"wheel_gap_becomes_wait", L"default", L"wheel pre-delay remains an explicit wait"},
    {L"compile_integer_timeline", L"default", L"action durations compile to absolute microseconds"},
    {L"legacy_wait_timeline", L"default", L"explicit wait actions advance absolute deadlines"},
    {L"random_duration_rejects_timeline", L"default", L"random jitter disables precision timeline"},
    {L"timing_us_prefers_over_duration", L"default", L"timingUs wins over duration float in compile"},
    {L"convert_gaps_become_waits", L"default", L"recorded gaps become explicit Wait actions"},
    {L"convert_skips_key_autorepeat", L"default", L"held-key KeyDown becomes Wait only"},
    {L"same_timestamp_no_wait", L"default", L"same-timestamp events insert no Wait between"},
    {L"timeline_fold_vs_explicit_equiv", L"default", L"folded pre-delay vs explicit Wait deadlines match"},
    {L"expand_recording_keeps_gaps", L"default", L"version1 recording Expand preserves wall time"},
    {L"expand_script_default_duration_no_wait", L"default", L"scripts default 0.1 cleared without Wait"},
    {L"expand_idempotent", L"default", L"Expand twice yields identical sequence"},
    {L"wait_stats_use_timing_us", L"default", L"ActionStepUs prefers timingUs for Wait"},
    {L"timeline_catchup_skips_stall", L"default", L"past-deadline waits catch up without stretching origin"},
    {L"scheduler_cancel_interrupts", L"default", L"precision scheduler responds to cancellation"},
    {L"scheduler_wait_until_elapsed", L"default", L"absolute WaitUntilElapsedUs is interruptible"},
    {L"click_capture_rect_clamped", L"findimage", L"near-edge clamp + exclusive rect + offset"},
    {L"pair_mousedown_mouseup", L"findimage", L"Down+Move+Up unit; KeyDown between fails"},
    {L"reject_drag_by_distance", L"findimage", L"Down/Up distance >25 rejects"},
    {L"reject_drag_by_move_count", L"findimage", L"too many abs moves rejects"},
    {L"reject_modifier_click", L"findimage", L"hold* or held Ctrl rejects"},
    {L"allow_relative_between_down_up", L"findimage", L"relative between down/up kept"},
    {L"delete_abs_moves_between_keep_relative", L"findimage", L"abs deleted relative kept"},
    {L"modifier_scan_forward_not_backward", L"findimage", L"forward Ctrl KeyDown without Up rejects"},
    {L"snap_timing_to_wait", L"findimage", L"snap+Down stepUs become leading Wait"},
    {L"capture_filename_uses_session_id", L"findimage", L"filename contains sessionId"},
    {L"convert_unit_to_findimage_fields", L"findimage", L"followUp/threshold/button + leading Wait"},
    {L"convert_findtime_until_found", L"findimage", L"options.findTimeExpr=-1 written"},
    {L"convert_drops_nearby_move", L"findimage", L"snap abs move removed"},
    {L"convert_drops_approach_moves", L"findimage", L"approach abs moves removed + Wait"},
    {L"convert_respects_move_selection", L"findimage", L"unselected approach Move kept"},
    {L"convert_skips_without_capture", L"findimage", L"batch skips without path"},
    {L"convert_mousedown_writes_xy", L"findimage", L"ConvertRecordedEvents writes Down x/y"},
    {L"convert_recorded_events_propagates_capture", L"findimage", L"capturePath/offset on MouseDown"},
    {L"relative_mode_click_still_has_screen_xy", L"findimage", L"relative moves + screen Down capture"},
    {L"timed_sequence_allows_findimage", L"findimage", L"FindImage allowed in timed sequence"},
    {L"reject_multiclick", L"findimage", L"MouseClick clickCount>1 rejected"},
    {L"findimage_offset_noffset_consistent", L"findimage", L"nOffset*80 restores offset"},
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

ScriptAction MakeDown(int x, int y, MouseButtonType btn = MouseButtonType::Left) {
    ScriptAction a{};
    a.type = ActionType::MouseDown;
    a.button = btn;
    a.x = x;
    a.y = y;
    return a;
}

ScriptAction MakeUp(int x, int y, MouseButtonType btn = MouseButtonType::Left) {
    ScriptAction a{};
    a.type = ActionType::MouseUp;
    a.button = btn;
    a.x = x;
    a.y = y;
    return a;
}

ScriptAction MakeAbs(int x, int y) {
    ScriptAction a{};
    a.type = ActionType::MoveMouse;
    a.x = x;
    a.y = y;
    return a;
}

ScriptAction MakeRel(int dx, int dy) {
    ScriptAction a{};
    a.type = ActionType::MoveMouseRelative;
    a.x = dx;
    a.y = dy;
    return a;
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

void CaseSameTimestampRelativeKeep() {
    // 同戳两包：保留分包，并插入设备报告间隔（无健康样本时默认 8ms）
    std::vector<RecordedEvent> events{
        Ev(100, 1, kWmRecordedRelativeMove, 4, -2),
        Ev(100, 2, kWmRecordedRelativeMove, -1, 7)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    int dx = 0, dy = 0, relCount = 0;
    for (const auto& a : converted.actions) {
        if (a.type == ActionType::MoveMouseRelative) {
            dx += a.x; dy += a.y; ++relCount;
        }
    }
    const bool ok = converted.actions.size() == 4
        && converted.actions[0].type == ActionType::Wait
        && converted.actions[1].type == ActionType::MoveMouseRelative
        && converted.actions[1].x == 4 && converted.actions[1].y == -2
        && converted.actions[2].type == ActionType::Wait
        && converted.actions[2].timingUs == 8000
        && converted.actions[3].type == ActionType::MoveMouseRelative
        && converted.actions[3].x == -1 && converted.actions[3].y == 7
        && dx == 3 && dy == 5 && relCount == 2
        && converted.relativeMoveCount == 2;
    Emit(L"same_timestamp_relative_keep", ok, L"");
}

void CaseMicroGapRelativeMerge() {
    // <200us 不同戳：不合并包；转换后把压缩间隔拉到默认报告间隔
    std::vector<RecordedEvent> events{
        Ev(1000, 1, kWmRecordedRelativeMove, 2, 3),
        Ev(1150, 2, kWmRecordedRelativeMove, 5, -1)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    const bool ok = converted.actions.size() == 4
        && converted.actions[0].type == ActionType::Wait
        && converted.actions[1].type == ActionType::MoveMouseRelative
        && converted.actions[1].x == 2
        && converted.actions[2].type == ActionType::Wait
        && converted.actions[2].timingUs == 8000
        && converted.actions[3].x == 5;
    Emit(L"micro_gap_relative_merge", ok, L"");
}

void CaseRepairCompressedRelGaps() {
    // 健康 8ms 样本决定中位数；夹在中间的 1µs 被拉到 8000
    std::vector<RecordedEvent> events{
        Ev(8000, 1, kWmRecordedRelativeMove, 1, 0),
        Ev(16000, 2, kWmRecordedRelativeMove, 1, 0),
        Ev(16001, 3, kWmRecordedRelativeMove, 1, 0),
        Ev(24001, 4, kWmRecordedRelativeMove, 1, 0)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    std::vector<uint64_t> relWaits;
    for (size_t i = 1; i + 1 < converted.actions.size(); ++i) {
        if (converted.actions[i].type != ActionType::Wait) continue;
        if (converted.actions[i - 1].type != ActionType::MoveMouseRelative) continue;
        if (converted.actions[i + 1].type != ActionType::MoveMouseRelative) continue;
        relWaits.push_back(ActionStepUs(converted.actions[i]));
    }
    const bool ok = relWaits.size() == 3
        && relWaits[0] == 8000
        && relWaits[1] == 8000
        && relWaits[2] == 8000;
    auto again = converted.actions;
    RepairCompressedRelativeGaps(again);
    const bool idem = again.size() == converted.actions.size();
    Emit(L"repair_compressed_rel_gaps", ok && idem, L"");
}

void CaseSubThresholdRelSplit() {
    std::vector<std::pair<int, int>> steps;
    AppendSubThresholdRelativeSteps(26, 9, 4, steps);
    int sx = 0, sy = 0;
    bool bounded = !steps.empty();
    for (const auto& s : steps) {
        if (std::abs(s.first) > 4 || std::abs(s.second) > 4) bounded = false;
        sx += s.first;
        sy += s.second;
    }
    steps.clear();
    AppendSubThresholdRelativeSteps(-3, 2, 4, steps);
    const bool singleSmall = steps.size() == 1
        && steps[0].first == -3 && steps[0].second == 2;
    Emit(L"sub_threshold_rel_split",
        bounded && sx == 26 && sy == 9 && singleSmall, L"");
}

void CaseMixed() {
    std::vector<RecordedEvent> events{
        Ev(0, 0, WM_MOUSEMOVE, 20, 30),
        Ev(1000, 1, kWmRecordedRelativeMove, 2, 1)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    Emit(L"mixed_capture_channels",
        converted.absoluteMoveCount == 1 && converted.relativeMoveCount == 1
            && converted.actions.size() == 3
            && converted.actions[0].type == ActionType::MoveMouse
            && converted.actions[1].type == ActionType::Wait
            && converted.actions[2].type == ActionType::MoveMouseRelative, L"");
}

void CaseButtonOrder() {
    std::vector<RecordedEvent> events{
        Ev(100, 8, WM_LBUTTONUP, 0, 0, VK_LBUTTON),
        Ev(100, 7, WM_LBUTTONDOWN, 0, 0, VK_LBUTTON)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    const bool ok = converted.actions.size() == 3
        && converted.actions[0].type == ActionType::Wait
        && converted.actions[1].type == ActionType::MouseDown
        && converted.actions[2].type == ActionType::MouseUp;
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
    const bool ok = converted.actions.size() == 4
        && converted.actions[0].type == ActionType::Wait
        && converted.actions[1].type == ActionType::KeyDown
        && converted.actions[2].type == ActionType::Wait
        && converted.actions[3].type == ActionType::KeyUp
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

void CaseConvertGapsBecomeWaits() {
    std::vector<RecordedEvent> events{
        Ev(0, 1, kWmRecordedRelativeMove, 1, 0),
        Ev(12345, 2, kWmRecordedRelativeMove, 2, 0),
        Ev(13000, 3, WM_KEYDOWN, 0, 0, 'W')};
    auto converted = ConvertRecordedEventsToActions(events, {});
    const bool ok = converted.actions.size() == 5
        && converted.actions[0].type == ActionType::MoveMouseRelative
        && converted.actions[0].timingUs == 0 && converted.actions[0].duration == 0.0
        && converted.actions[1].type == ActionType::Wait
        && converted.actions[1].timingUs == 12345
        && converted.actions[2].type == ActionType::MoveMouseRelative
        && converted.actions[2].timingUs == 0
        && converted.actions[3].type == ActionType::Wait
        && converted.actions[3].timingUs == 655
        && converted.actions[4].type == ActionType::KeyDown
        && converted.actions[4].timingUs == 0;
    Emit(L"convert_gaps_become_waits", ok, L"");
}

void CaseConvertSkipsKeyAutorepeat() {
    std::vector<RecordedEvent> events{
        Ev(0, 1, WM_KEYDOWN, 0, 0, VK_LSHIFT),
        Ev(30000, 2, WM_KEYDOWN, 0, 0, VK_LSHIFT), // autorepeat
        Ev(62000, 3, WM_KEYDOWN, 0, 0, VK_LSHIFT), // autorepeat
        Ev(100000, 4, WM_KEYUP, 0, 0, VK_LSHIFT)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    int keyDown = 0, keyUp = 0;
    uint64_t waitSum = 0;
    for (const auto& a : converted.actions) {
        if (a.type == ActionType::KeyDown && a.keyVk == VK_LSHIFT) ++keyDown;
        if (a.type == ActionType::KeyUp && a.keyVk == VK_LSHIFT) ++keyUp;
        if (a.type == ActionType::Wait) waitSum += ActionStepUs(a);
    }
    const bool ok = keyDown == 1 && keyUp == 1 && waitSum == 100000;
    Emit(L"convert_skips_key_autorepeat", ok, L"");
}

void CaseSameTimestampNoWait() {
    std::vector<RecordedEvent> events{
        Ev(0, 1, WM_LBUTTONDOWN, 1, 1, VK_LBUTTON),
        Ev(0, 2, WM_LBUTTONUP, 1, 1, VK_LBUTTON)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    const bool ok = converted.actions.size() == 2
        && converted.actions[0].type == ActionType::MouseDown
        && converted.actions[1].type == ActionType::MouseUp
        && converted.actions[0].timingUs == 0
        && converted.actions[1].timingUs == 0;
    Emit(L"same_timestamp_no_wait", ok, L"");
}

void CaseTimelineFoldVsExplicitEquiv() {
    ScriptAction m1{};
    m1.type = ActionType::MoveMouseRelative;
    m1.timingUs = 1000;
    ScriptAction m2{};
    m2.type = ActionType::MoveMouseRelative;
    m2.timingUs = 2500;
    const auto folded = CompileInputTimeline({m1, m2});

    ScriptAction w1 = MakeExplicitWaitUs(1000);
    ScriptAction i1{};
    i1.type = ActionType::MoveMouseRelative;
    ScriptAction w2 = MakeExplicitWaitUs(2500);
    ScriptAction i2{};
    i2.type = ActionType::MoveMouseRelative;
    const auto explicitTl = CompileInputTimeline({w1, i1, w2, i2});
    const bool ok = folded.size() == 2 && explicitTl.size() == 2
        && folded[0].deadlineUs == explicitTl[0].deadlineUs
        && folded[1].deadlineUs == explicitTl[1].deadlineUs
        && folded[0].deadlineUs == 1000
        && folded[1].deadlineUs == 3500;
    Emit(L"timeline_fold_vs_explicit_equiv", ok, L"");
}

void CaseExpandRecordingKeepsGaps() {
    ScriptAction m1{};
    m1.type = ActionType::MoveMouse;
    m1.timingUs = 5000;
    m1.x = 1;
    ScriptAction m2{};
    m2.type = ActionType::MouseDown;
    m2.timingUs = 3000;
    ExpandRecordingPreDelayPolicy policy{};
    policy.treatAsRecordingTimeline = true;
    auto expanded = ExpandRecordingPreDelaysToExplicitWaits({m1, m2}, policy);
    const auto tl = CompileInputTimeline(expanded);
    const bool ok = expanded.size() == 4
        && expanded[0].type == ActionType::Wait && expanded[0].timingUs == 5000
        && expanded[1].type == ActionType::MoveMouse && expanded[1].timingUs == 0
        && expanded[2].type == ActionType::Wait && expanded[2].timingUs == 3000
        && expanded[3].type == ActionType::MouseDown
        && tl.size() == 2 && tl[0].deadlineUs == 5000 && tl[1].deadlineUs == 8000;
    Emit(L"expand_recording_keeps_gaps", ok, L"");
}

void CaseExpandScriptDefaultNoWait() {
    ScriptAction m{};
    m.type = ActionType::MoveMouse;
    m.duration = 0.1; // 宏默认伪前延迟
    m.x = 10;
    ExpandRecordingPreDelayPolicy policy{};
    policy.treatAsRecordingTimeline = false;
    auto expanded = ExpandRecordingPreDelaysToExplicitWaits({m}, policy);
    const bool ok = expanded.size() == 1
        && expanded[0].type == ActionType::MoveMouse
        && expanded[0].duration == 0.0
        && expanded[0].timingUs == 0;
    Emit(L"expand_script_default_duration_no_wait", ok, L"");
}

void CaseExpandIdempotent() {
    ScriptAction m{};
    m.type = ActionType::MoveMouseRelative;
    m.timingUs = 1200;
    ExpandRecordingPreDelayPolicy policy{};
    policy.treatAsRecordingTimeline = true;
    auto once = ExpandRecordingPreDelaysToExplicitWaits({m}, policy);
    auto twice = ExpandRecordingPreDelaysToExplicitWaits(once, policy);
    const bool ok = once.size() == twice.size()
        && once.size() == 2
        && once[0].timingUs == twice[0].timingUs
        && once[1].type == twice[1].type;
    Emit(L"expand_idempotent", ok, L"");
}

void CaseWaitStatsUseTimingUs() {
    ScriptAction w{};
    w.type = ActionType::Wait;
    w.duration = 0.050;
    w.timingUs = 49937;
    Emit(L"wait_stats_use_timing_us", ActionStepUs(w) == 49937, L"");
}

void CaseTimelineCatchupSkipsStall() {
    PrecisionInputTimeline tl;
    tl.Reset();
    Sleep(20); // 人为落后：应追赶，不应再睡满后续 20ms
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 20; ++i)
        tl.WaitDeltaUs(1000, [] { return false; }); // 计划 20ms
    QueryPerformanceCounter(&t1);
    const double wallMs = (t1.QuadPart - t0.QuadPart) * 1000.0
        / static_cast<double>(freq.QuadPart);
    Emit(L"timeline_catchup_skips_stall", wallMs < 8.0, L"");
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

void CaseClickCaptureRect() {
    // vs: 0,0 - 1920x1080 exclusive right/bottom
    auto nearEdge = ComputeClickCaptureRect(5, 5, 40, 0, 0, 1920, 1080);
    const bool ok = nearEdge.valid
        && nearEdge.x1 == 0 && nearEdge.y1 == 0
        && nearEdge.x2 == 45 && nearEdge.y2 == 45
        && nearEdge.offsetX == 5 - (0 + 45 / 2)
        && nearEdge.offsetY == 5 - (0 + 45 / 2);
    auto mid = ComputeClickCaptureRect(100, 200, 40, 0, 0, 1920, 1080);
    const bool midOk = mid.valid && mid.x1 == 60 && mid.y1 == 160
        && mid.x2 == 140 && mid.y2 == 240
        && mid.offsetX == 0 && mid.offsetY == 0;
    Emit(L"click_capture_rect_clamped", ok && midOk, L"");
}

void CasePairDownUp() {
    std::vector<ScriptAction> okActs{MakeDown(10, 10), MakeAbs(11, 10), MakeUp(10, 10)};
    RenumberScriptActions(okActs);
    auto okProbe = ProbeClickUnitFromDown(okActs, 0, 0, 3);
    ScriptAction key{};
    key.type = ActionType::KeyDown;
    key.keyVk = 'A';
    std::vector<ScriptAction> bad{MakeDown(10, 10), key, MakeUp(10, 10)};
    RenumberScriptActions(bad);
    auto badProbe = ProbeClickUnitFromDown(bad, 0, 0, 3);
    Emit(L"pair_mousedown_mouseup",
        okProbe.unit.ok && !badProbe.unit.ok
            && badProbe.reject == ClickUnitRejectReason::ForbiddenBetween, L"");
}

void CaseRejectDragDistance() {
    std::vector<ScriptAction> acts{MakeDown(0, 0), MakeUp(40, 0)};
    RenumberScriptActions(acts);
    auto p = ProbeClickUnitFromDown(acts, 0, 0, 2);
    Emit(L"reject_drag_by_distance",
        !p.unit.ok && p.reject == ClickUnitRejectReason::DragDistance, L"");
}

void CaseRejectDragMoveCount() {
    std::vector<ScriptAction> acts{MakeDown(100, 100)};
    for (int i = 0; i < 9; ++i) acts.push_back(MakeAbs(100 + i, 100));
    acts.push_back(MakeUp(100, 100));
    RenumberScriptActions(acts);
    auto p = ProbeClickUnitFromDown(acts, 0, 0, static_cast<int>(acts.size()));
    Emit(L"reject_drag_by_move_count",
        !p.unit.ok && p.reject == ClickUnitRejectReason::DragAbsMoveCount, L"");
}

void CaseRejectModifier() {
    ScriptAction ctrlDown{};
    ctrlDown.type = ActionType::KeyDown;
    ctrlDown.keyVk = VK_LCONTROL;
    std::vector<ScriptAction> acts{ctrlDown, MakeDown(10, 10), MakeUp(10, 10)};
    RenumberScriptActions(acts);
    auto p = ProbeClickUnitFromDown(acts, 1, 0, 3);
    ScriptAction downHold = MakeDown(10, 10);
    downHold.holdLeftCtrl = true;
    std::vector<ScriptAction> acts2{downHold, MakeUp(10, 10)};
    RenumberScriptActions(acts2);
    auto p2 = ProbeClickUnitFromDown(acts2, 0, 0, 2);
    Emit(L"reject_modifier_click",
        !p.unit.ok && p.reject == ClickUnitRejectReason::ModifierHeld
            && !p2.unit.ok && p2.reject == ClickUnitRejectReason::ModifierHeld, L"");
}

void CaseAllowRelative() {
    std::vector<ScriptAction> acts{
        MakeDown(50, 50), MakeRel(3, -2), MakeUp(50, 50)};
    acts[0].recordedCapturePath = L"images\\t.bmp";
    RenumberScriptActions(acts);
    ConvertToFindImageOptions opt{};
    auto r = ConvertActionsToFindImage(acts, 0, 3, opt);
    bool hasRel = false;
    bool hasFi = false;
    for (const auto& a : acts) {
        if (a.type == ActionType::MoveMouseRelative) hasRel = true;
        if (a.type == ActionType::FindImage) hasFi = true;
    }
    Emit(L"allow_relative_between_down_up",
        r.converted == 1 && hasRel && hasFi, L"");
}

void CaseDeleteAbsKeepRel() {
    std::vector<ScriptAction> acts{
        MakeDown(50, 50), MakeAbs(51, 50), MakeRel(2, 0), MakeUp(50, 50)};
    acts[0].recordedCapturePath = L"images\\t.bmp";
    RenumberScriptActions(acts);
    ConvertActionsToFindImage(acts, 0, 4, {});
    int absCount = 0, relCount = 0, fiCount = 0;
    for (const auto& a : acts) {
        if (a.type == ActionType::MoveMouse) ++absCount;
        if (a.type == ActionType::MoveMouseRelative) ++relCount;
        if (a.type == ActionType::FindImage) ++fiCount;
    }
    Emit(L"delete_abs_moves_between_keep_relative",
        absCount == 0 && relCount == 1 && fiCount == 1, L"");
}

void CaseModifierScanForward() {
    ScriptAction ctrlDown{};
    ctrlDown.type = ActionType::KeyDown;
    ctrlDown.keyVk = VK_CONTROL;
    ScriptAction ctrlUp{};
    ctrlUp.type = ActionType::KeyUp;
    ctrlUp.keyVk = VK_CONTROL;
    // Forward: Down then Up then click → OK
    std::vector<ScriptAction> okActs{ctrlDown, ctrlUp, MakeDown(1, 1), MakeUp(1, 1)};
    RenumberScriptActions(okActs);
    auto ok = ProbeClickUnitFromDown(okActs, 2, 0, 4);
    // Forward: Down without Up → reject
    std::vector<ScriptAction> badActs{ctrlDown, MakeDown(1, 1), MakeUp(1, 1)};
    RenumberScriptActions(badActs);
    auto bad = ProbeClickUnitFromDown(badActs, 1, 0, 3);
    Emit(L"modifier_scan_forward_not_backward",
        ok.unit.ok && !bad.unit.ok
            && bad.reject == ClickUnitRejectReason::ModifierHeld, L"");
}

void CaseSnapTimingToWait() {
    auto move = MakeAbs(100, 100);
    move.timingUs = 5000;
    auto down = MakeDown(102, 100);
    down.timingUs = 12000;
    down.recordedCapturePath = L"images\\t.bmp";
    auto up = MakeUp(102, 100);
    std::vector<ScriptAction> acts{move, down, up};
    RenumberScriptActions(acts);
    ConvertOneClickUnitToFindImage(acts, ProbeClickUnitFromDown(acts, 1, 0, 3).unit, {});
    const bool ok = acts.size() == 2
        && acts[0].type == ActionType::Wait
        && acts[0].timingUs == 17000
        && acts[1].type == ActionType::FindImage
        && acts[1].timingUs == 0
        && acts[1].duration == 0.0;
    Emit(L"snap_timing_to_wait", ok, L"");
}

void CaseCaptureFilename() {
    const uint64_t sid = 0xABCDEFull;
    const auto name = MakeRecordingClickCaptureFileName(sid, 42);
    const std::wstring sidStr = std::to_wstring(sid);
    Emit(L"capture_filename_uses_session_id",
        name.find(L"rec_") == 0
            && name.find(L"42") != std::wstring::npos
            && name.find(sidStr) != std::wstring::npos, L"");
}

void CaseConvertFields() {
    auto down = MakeDown(10, 20, MouseButtonType::Right);
    down.timingUs = 9000;
    down.recordedCapturePath = L"images\\t.bmp";
    down.captureOffsetX = 2;
    down.captureOffsetY = -3;
    std::vector<ScriptAction> acts{down, MakeUp(10, 20, MouseButtonType::Right)};
    RenumberScriptActions(acts);
    ConvertOneClickUnitToFindImage(acts, ProbeClickUnitFromDown(acts, 0, 0, 2).unit, {});
    const bool ok = acts.size() == 2
        && acts[0].type == ActionType::Wait
        && acts[0].timingUs == 9000
        && acts[1].type == ActionType::FindImage
        && acts[1].findImageFollowUp == 0
        && acts[1].matchThreshold == 65.0
        && acts[1].button == MouseButtonType::Right
        && acts[1].timingUs == 0
        && acts[1].offsetX == 2 && acts[1].offsetY == -3
        && acts[1].findTimeExpr == L"0";
    Emit(L"convert_unit_to_findimage_fields", ok, L"");
}

void CaseConvertFindTimeUntil() {
    auto down = MakeDown(10, 20, MouseButtonType::Left);
    down.recordedCapturePath = L"images\\t.bmp";
    std::vector<ScriptAction> acts{down, MakeUp(10, 20, MouseButtonType::Left)};
    RenumberScriptActions(acts);
    ConvertToFindImageOptions opt{};
    opt.findTimeExpr = L"-1";
    ConvertOneClickUnitToFindImage(acts, ProbeClickUnitFromDown(acts, 0, 0, 2).unit, opt);
    Emit(L"convert_findtime_until_found",
        acts.size() == 1 && acts[0].type == ActionType::FindImage
            && acts[0].findTimeExpr == L"-1", L"");
}

void CaseConvertDropsNearbyMove() {
    std::vector<ScriptAction> acts{
        MakeAbs(100, 100), MakeDown(103, 100), MakeUp(103, 100)};
    acts[1].recordedCapturePath = L"images\\t.bmp";
    RenumberScriptActions(acts);
    ConvertActionsToFindImage(acts, 0, 3, {});
    Emit(L"convert_drops_nearby_move",
        acts.size() == 1 && acts[0].type == ActionType::FindImage, L"");
}

void CaseConvertDropsApproachMoves() {
    // 远距离前置连续绝对 Move 也应整段删除，避免回放仍移到录制点再找图
    auto m0 = MakeAbs(10, 10);
    m0.timingUs = 1000;
    auto m1 = MakeAbs(200, 200);
    m1.timingUs = 2000;
    auto m2 = MakeAbs(400, 300);
    m2.timingUs = 3000;
    auto down = MakeDown(400, 300);
    down.timingUs = 4000;
    down.recordedCapturePath = L"images\\t.bmp";
    std::vector<ScriptAction> acts{m0, m1, m2, down, MakeUp(400, 300)};
    RenumberScriptActions(acts);
    ConvertActionsToFindImage(acts, 0, 5, {});
    const bool ok = acts.size() == 2
        && acts[0].type == ActionType::Wait
        && acts[0].timingUs == 10000
        && acts[1].type == ActionType::FindImage
        && acts[1].timingUs == 0;
    Emit(L"convert_drops_approach_moves", ok, L"");
}

void CaseConvertRespectsMoveSelection() {
    auto m0 = MakeAbs(10, 10);
    m0.timingUs = 1000;
    auto m1 = MakeAbs(400, 300);
    m1.timingUs = 2000;
    auto down = MakeDown(400, 300);
    down.timingUs = 3000;
    down.recordedCapturePath = L"images\\t.bmp";
    std::vector<ScriptAction> acts{m0, m1, down, MakeUp(400, 300)};
    RenumberScriptActions(acts);
    // 只勾选 m1 + Down + Up，不勾选 m0 → m0 必须保留
    std::vector<char> sel{0, 1, 1, 1};
    ConvertActionsToFindImageSelected(acts, sel, {});
    int absCount = 0, fiCount = 0;
    bool firstIsM0 = false;
    for (const auto& a : acts) {
        if (a.type == ActionType::MoveMouse) {
            ++absCount;
            if (absCount == 1) firstIsM0 = (a.x == 10 && a.y == 10);
        }
        if (a.type == ActionType::FindImage) ++fiCount;
    }
    Emit(L"convert_respects_move_selection",
        absCount == 1 && firstIsM0 && fiCount == 1
            && acts.size() == 3
            && acts[0].type == ActionType::MoveMouse
            && acts[1].type == ActionType::Wait
            && acts[1].timingUs == 5000
            && acts[2].type == ActionType::FindImage
            && acts[2].timingUs == 0, L"");
}

void CaseConvertSkipsWithoutCapture() {
    std::vector<ScriptAction> acts{MakeDown(1, 1), MakeUp(1, 1)};
    RenumberScriptActions(acts);
    auto r = ConvertActionsToFindImage(acts, 0, 2, {});
    Emit(L"convert_skips_without_capture",
        r.converted == 0 && r.skipped >= 1
            && acts.size() == 2
            && acts[0].type == ActionType::MouseDown, L"");
}

void CaseConvertMouseDownXy() {
    std::vector<RecordedEvent> events{
        Ev(0, 1, WM_MOUSEMOVE, 100, 200),
        Ev(1000, 2, WM_LBUTTONDOWN, 110, 210, VK_LBUTTON),
        Ev(2000, 3, WM_LBUTTONUP, 110, 210, VK_LBUTTON)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    bool found = false;
    for (const auto& a : converted.actions) {
        if (a.type == ActionType::MouseDown) {
            found = a.x == 110 && a.y == 210;
            break;
        }
    }
    Emit(L"convert_mousedown_writes_xy", found, L"");
}

void CaseConvertPropagatesCapture() {
    RecordedEvent down = Ev(1000, 2, WM_LBUTTONDOWN, 50, 60, VK_LBUTTON);
    down.capturePath = L"C:\\tmp\\rec_1_2.bmp";
    down.captureOffsetX = 1;
    down.captureOffsetY = -2;
    std::vector<RecordedEvent> events{
        Ev(0, 1, WM_MOUSEMOVE, 50, 60),
        down,
        Ev(2000, 3, WM_LBUTTONUP, 50, 60, VK_LBUTTON)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    bool ok = false;
    for (const auto& a : converted.actions) {
        if (a.type == ActionType::MouseDown) {
            ok = a.recordedCapturePath == down.capturePath
                && a.captureOffsetX == 1 && a.captureOffsetY == -2;
            break;
        }
    }
    Emit(L"convert_recorded_events_propagates_capture", ok, L"");
}

void CaseRelativeModeScreenXy() {
    RecordedEvent down = Ev(500, 2, WM_LBUTTONDOWN, 300, 400, VK_LBUTTON);
    down.capturePath = L"images\\rel.bmp";
    down.captureOffsetX = 0;
    down.captureOffsetY = 0;
    std::vector<RecordedEvent> events{
        Ev(0, 1, kWmRecordedRelativeMove, 5, -3),
        down,
        Ev(600, 3, kWmRecordedRelativeMove, 1, 0),
        Ev(800, 4, WM_LBUTTONUP, 300, 400, VK_LBUTTON)};
    auto converted = ConvertRecordedEventsToActions(events, {});
    bool downOk = false;
    bool hasRel = false;
    for (const auto& a : converted.actions) {
        if (a.type == ActionType::MoveMouseRelative) hasRel = true;
        if (a.type == ActionType::MouseDown) {
            downOk = a.x == 300 && a.y == 400
                && a.recordedCapturePath == down.capturePath;
        }
    }
    Emit(L"relative_mode_click_still_has_screen_xy", downOk && hasRel, L"");
}

void CaseTimedAllowsFindImage() {
    ScriptAction fi{};
    fi.type = ActionType::FindImage;
    ScriptAction mv = MakeAbs(1, 2);
    Emit(L"timed_sequence_allows_findimage",
        ScriptIsTimedInputSequence({fi, mv}), L"");
}

void CaseRejectMulticlick() {
    ScriptAction click{};
    click.type = ActionType::MouseClick;
    click.clickCount = 2;
    click.x = 1;
    click.y = 1;
    std::vector<ScriptAction> acts{click};
    auto p = ProbeClickUnitFromSelection(acts, 0);
    Emit(L"reject_multiclick",
        !p.unit.ok && p.reject == ClickUnitRejectReason::MultiClick, L"");
}

void CaseFindImageOffsetNorm() {
    ScriptAction fi{};
    fi.type = ActionType::FindImage;
    fi.offsetX = 8;
    fi.offsetY = -4;
    fi.imagePath = L"images\\stub.bmp";
    SyncFindImageOffsetNorm(fi);
    const bool ok = std::abs(fi.nOffsetX * 80.0 - 8.0) < 1e-9
        && std::abs(fi.nOffsetY * 80.0 - (-4.0)) < 1e-9;
    Emit(L"findimage_offset_noffset_consistent", ok, L"");
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
    CaseSameTimestampRelativeKeep();
    CaseMicroGapRelativeMerge();
    CaseRepairCompressedRelGaps();
    CaseSubThresholdRelSplit();
    CaseMixed();
    CaseButtonOrder();
    CaseHotkeyTrim();
    CaseWheel();
    CaseCompile();
    CaseLegacyWait();
    CaseRandomReject();
    CaseTimingUsPrefers();
    CaseConvertGapsBecomeWaits();
    CaseConvertSkipsKeyAutorepeat();
    CaseSameTimestampNoWait();
    CaseTimelineFoldVsExplicitEquiv();
    CaseExpandRecordingKeepsGaps();
    CaseExpandScriptDefaultNoWait();
    CaseExpandIdempotent();
    CaseWaitStatsUseTimingUs();
    CaseTimelineCatchupSkipsStall();
    CaseSchedulerCancel();
    CaseSchedulerWaitUntil();
    CaseClickCaptureRect();
    CasePairDownUp();
    CaseRejectDragDistance();
    CaseRejectDragMoveCount();
    CaseRejectModifier();
    CaseAllowRelative();
    CaseDeleteAbsKeepRel();
    CaseModifierScanForward();
    CaseSnapTimingToWait();
    CaseCaptureFilename();
    CaseConvertFields();
    CaseConvertFindTimeUntil();
    CaseConvertDropsNearbyMove();
    CaseConvertDropsApproachMoves();
    CaseConvertRespectsMoveSelection();
    CaseConvertSkipsWithoutCapture();
    CaseConvertMouseDownXy();
    CaseConvertPropagatesCapture();
    CaseRelativeModeScreenXy();
    CaseTimedAllowsFindImage();
    CaseRejectMulticlick();
    CaseFindImageOffsetNorm();
    selftest::EmitSummary();
    return selftest::ExitCode();
}
