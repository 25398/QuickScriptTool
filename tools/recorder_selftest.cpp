#include "selftest_harness.h"

#include "action_utils.h"
#include "coord_space.h"
#include "input/mouse_rel_split.h"
#include "input_timeline_scheduler.h"
#include "low_power_mode.h"
#include "recorder_timeline.h"
#include "recorder_diag_log.h"
#include "recorder_report_interval.h"
#include "recording_to_findimage.h"
#include "recorder.h"

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
    {L"repair_copy_leaves_source", L"default", L"RepairCompressedRelativeGaps must not mutate the caller's original vector"},
    {L"rel_report_interval_high_polling", L"default", L"8k/4k/1kHz 报告周期估计等于真实周期且不判为压缩"},
    {L"rel_report_interval_compressed_burst", L"default", L"积压样本判为压缩、中位数不被污染、样本不足走回退"},
    {L"rel_report_interval_background_cap", L"default", L"Win11 后台 125Hz(8ms) 不被误判为压缩；越界样本不入统计"},
    {L"high_polling_rel_gaps_preserved", L"default", L"4kHz 录制的 250µs 相对间隔端到端保持不拉伸"},
    {L"spread_relative_move_packets_preserves_totals", L"default",
        L"细分位移铺满窗口：Σ等待/Σ位移严格不变、按窗口自适应、幂等"},
    {L"prepare_playback_timeline_applies_spread", L"default",
        L"统一回放入口：关开关不改包数、开开关必细分且 Σ位移守恒"},
    {L"sum_relative_moves_counts_only_rel", L"default", L"保真度诊断只累加相对移动的位移与包数"},
    {L"recorder_diag_trim_line_start", L"default", L"诊断日志超限裁剪按整行切；无换行/换行在末位都全留"},
    {L"move_fidelity_verdict", L"default", L"回放保真判定：未统计必须优先于不一致（否则软输入模式误报）"},
    {L"relative_capture_decision", L"default", L"Auto 采集判定：强制模式/防抖/刻意切换不得被粘滞吞掉"},
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
    {L"timeline_injection_overrun_eats_short_wait", L"default",
        L"注入超支吃掉后续短等待预算：立刻返回且 late 如实记录、4ms 量级不触发 rebase"},
    {L"timeline_catchup_skips_stall", L"default", L"small past-deadline jitter still catches up"},
    {L"timeline_large_stall_rebases", L"default", L"large stall shifts origin so later waits are not burst"},
    {L"timeline_wait_until_rebases", L"default", L"WaitUntilElapsedUs overshoot also rebases origin"},
    {L"timeline_long_wait_wall", L"default", L"200ms wait keeps wall time (adaptive timer slices)"},
    {L"timeline_gap_from_now_no_catchup", L"default", L"WaitGapUs sleeps from now like 大漠 Delay"},
    {L"timeline_low_power_keeps_deadlines", L"default",
        L"低性能模式：自旋压到 800us 后仍按 deadline 命中（抖动 <=2ms），且关掉后恢复原行为"},
    {L"low_power_flag_toggles", L"default", L"低性能模式开关可开可关（进程级原子量）"},
    {L"scheduler_cancel_interrupts", L"default", L"precision scheduler responds to cancellation"},
    {L"scheduler_wait_until_elapsed", L"default", L"absolute WaitUntilElapsedUs is interruptible"},
    {L"click_capture_rect_clamped", L"findimage", L"near-edge clamp + exclusive rect + offset"},
    {L"hover_patch_covers_click", L"findimage", L"按下前悬停缓存命中半径"},
    {L"pair_mousedown_mouseup", L"findimage", L"Down+Move+Up unit; KeyDown between fails"},
    {L"reject_drag_by_distance", L"findimage", L"Down/Up distance >25 rejects"},
    {L"reject_drag_by_move_count", L"findimage", L"too many abs moves rejects"},
    {L"reject_modifier_click", L"findimage", L"hold* or held Ctrl rejects"},
    {L"allow_relative_between_down_up", L"findimage", L"relative between down/up kept"},
    {L"delete_abs_moves_between_keep_relative", L"findimage", L"abs deleted relative kept"},
    {L"modifier_scan_forward_not_backward", L"findimage", L"forward Ctrl KeyDown without Up rejects"},
    {L"snap_timing_to_wait", L"findimage", L"snap+Down stepUs become leading Wait"},
    {L"capture_filename_uses_session_id", L"findimage", L"filename contains sessionId"},
    {L"scope_global_always_accept", L"default", L"captureScope=1 accepts any candidate"},
    {L"scope_window_rejects_other_root", L"default", L"captureScope=0 rejects non-target root"},
    {L"scope_arm_locks_first_external", L"default", L"null root + arm locks external candidate"},
    {L"scope_arm_skips_own_process", L"default", L"arm does not lock own-process candidate"},
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
    {L"window_relative_conversion", L"default", L"窗口相对录制：动作标记 windowRelative 且保持客户区像素"},
    {L"convert_window_relative_findimage", L"findimage", L"窗口相对点击转找图时保留 windowRelative 并全客户区搜索"},
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

void CaseRepairCopyLeavesSource() {
    std::vector<ScriptAction> src;
    ScriptAction a{};
    a.type = ActionType::MoveMouseRelative;
    a.x = 1;
    src.push_back(a);
    ScriptAction w{};
    w.type = ActionType::Wait;
    w.timingUs = 100;
    w.duration = 0.0001;
    src.push_back(w);
    src.push_back(a);
    const uint64_t waitUs = ActionStepUs(src[1]);
    const size_t n = src.size();
    auto copy = src;
    RepairCompressedRelativeGaps(copy);
    const bool sourceUntouched = src.size() == n && ActionStepUs(src[1]) == waitUs
        && src[0].type == ActionType::MoveMouseRelative
        && src[2].type == ActionType::MoveMouseRelative;
    const bool copyChanged = ActionStepUs(copy[1]) != waitUs || copy.size() != n;
    Emit(L"repair_copy_leaves_source", sourceUntouched && copyChanged, L"");
}

// ── 报告周期模型：高轮询率鼠标的时间轴不得被拉伸 ──────────────────
// 旧实现：EMA 下限 1000µs + 只认 [2000,16000]µs 为「健康间隔」⇒ 2kHz 以上
// 鼠标的录制被整体拉伸（4kHz 累计 32 倍）。这些用例在旧实现下会变红。

void CaseRelReportIntervalHighPolling() {
    // 8kHz(125µs) 与 4kHz(250µs)：估计周期必须等于真实周期，且判定为「未压缩」。
    qst_recorder::ReportIntervalModel m8;
    for (int i = 0; i < 16; ++i) m8.ObserveGap(125);
    const bool ok8 = m8.MedianUs() == 125 && !m8.IsCompressedGap(125)
        && m8.LooksHighPolling();

    qst_recorder::ReportIntervalModel m4;
    for (int i = 0; i < 16; ++i) m4.ObserveGap(250);
    const bool ok4 = m4.MedianUs() == 250 && !m4.IsCompressedGap(250);

    // 1kHz：既不能被当成积压，也不能把正常的 1ms 间隔误判成压缩。
    qst_recorder::ReportIntervalModel m1;
    for (int i = 0; i < 16; ++i) m1.ObserveGap(1000);
    const bool ok1 = m1.MedianUs() == 1000 && !m1.IsCompressedGap(1000)
        && !m1.LooksHighPolling();

    Emit(L"rel_report_interval_high_polling", ok8 && ok4 && ok1, L"");
}

void CaseRelReportIntervalCompressedBurst() {
    // 1kHz 基线中夹入积压样本（10µs）⇒ 必须判为压缩；中位数不被污染。
    qst_recorder::ReportIntervalModel m;
    for (int i = 0; i < 10; ++i) m.ObserveGap(1000);
    m.ObserveGap(10);
    const bool burstFlagged = m.IsCompressedGap(10) && !m.IsCompressedGap(1000);
    const bool medianStable = m.MedianUs() == 1000;

    // 样本不足时走保守回退（Windows 11 后台 Raw Input ~125Hz ⇒ 8000µs），
    // 且此时过短间隔一律判为压缩 —— 与旧实现行为一致，避免短录制抖动。
    qst_recorder::ReportIntervalModel few;
    few.ObserveGap(150);
    const bool fallback = few.MedianUs() == qst_recorder::kFallbackReportIntervalUs
        && few.IsCompressedGap(150);

    // 手写宏：中位数 10ms 的相对序列里夹一个刻意的 1ms 等待 ⇒ 不得被判为积压
    // （旧实现阈值 500µs，这里必须保持一致，否则手写宏会被整体放慢 10 倍）。
    qst_recorder::ReportIntervalModel hw;
    for (int i = 0; i < 8; ++i) hw.ObserveGap(10000);
    const bool handWrittenSafe = !hw.IsCompressedGap(1000) && hw.IsCompressedGap(300);
    Emit(L"rel_report_interval_compressed_burst",
        burstFlagged && medianStable && fallback && handWrittenSafe, L"");
}

void CaseRelReportIntervalBackgroundCap() {
    // Windows 11 把后台 Raw Input 接收方限流到 ~125Hz：真实间隔就是 ~8ms，
    // 不能被当成「压缩」而改写（否则后台录制的相对位移全部被压缩到同一时刻）。
    qst_recorder::ReportIntervalModel m;
    for (int i = 0; i < 12; ++i) m.ObserveGap(8000);
    const bool ok = m.MedianUs() == 8000 && !m.IsCompressedGap(8000)
        && m.IsCompressedGap(300);
    // 越界样本只计数、不污染中位数（>20ms 的停顿不是报告周期）。
    qst_recorder::ReportIntervalModel g;
    for (int i = 0; i < 8; ++i) g.ObserveGap(1000);
    const bool accepted = g.ObserveGap(250000) == false;
    const bool ok2 = g.MedianUs() == 1000 && g.ObservedCount() == 9 && g.SampleCount() == 8;
    Emit(L"rel_report_interval_background_cap", ok && accepted && ok2, L"");
}

void CaseHighPollingRelGapsPreserved() {
    // 端到端：4kHz 鼠标录制的 250µs 间隔，经转换 + Repair 后必须原样保留。
    // 旧实现：250µs 既不算「健康」(需 ≥2000) 又 <500 ⇒ 全部改写成 8000µs，
    // 整段相对位移被拉伸 32 倍（回放时「转视角 + 走路」轨迹必然不同）。
    std::vector<RecordedEvent> events;
    for (int i = 0; i < 8; ++i) {
        events.push_back(Ev(static_cast<uint64_t>(i) * 250, static_cast<uint64_t>(i + 1),
            kWmRecordedRelativeMove, 1, 0));
    }
    auto converted = ConvertRecordedEventsToActions(events, {});
    std::vector<uint64_t> relWaits;
    for (size_t i = 1; i + 1 < converted.actions.size(); ++i) {
        if (converted.actions[i].type != ActionType::Wait) continue;
        if (converted.actions[i - 1].type != ActionType::MoveMouseRelative) continue;
        if (converted.actions[i + 1].type != ActionType::MoveMouseRelative) continue;
        relWaits.push_back(ActionStepUs(converted.actions[i]));
    }
    bool allPreserved = relWaits.size() == 7;
    for (uint64_t us : relWaits) {
        if (us != 250) allPreserved = false;
    }
    // 时间轴总时长必须仍是 7×250µs，而不是 7×8000µs。
    const bool durationOk = converted.durationSeconds > 0.0
        && converted.durationSeconds < 0.003;
    Emit(L"high_polling_rel_gaps_preserved",
        allPreserved && durationOk && converted.relativeMoveCount == 8, L"");
}

void CasePreparePlaybackTimelineAppliesSpread() {
    // 统一入口的不变量：开关关闭 ⇒ 包数不变；打开 ⇒ 包数变多且 Σ 位移守恒。
    // ⚠ 这条防的是「只补了一部分调用点」的回归 —— 2026-09-21 实测踩过：
    //   `RepairCompressedRelativeGaps` 有 6 个调用点（`RunCurrentActions` 里就有两个平行块），
    //   只补其中一个 ⇒ 用户勾了开关毫无效果（日志里包数仍是 790、等待仍是 0.008 秒）。
    std::vector<ScriptAction> base;
    for (int i = 0; i < 6; ++i) {
        base.push_back(MakeExplicitWaitUs(8000, 0));
        ScriptAction m{};
        m.type = ActionType::MoveMouseRelative;
        m.x = 4 + i;
        m.y = 1;
        base.push_back(std::move(m));
    }

    std::vector<ScriptAction> off = base;
    PreparePlaybackTimeline(off, false);
    uint64_t offBefore = 0, offAfter = 0;
    LastSpreadPacketCounts(offBefore, offAfter);

    std::vector<ScriptAction> on = base;
    PreparePlaybackTimeline(on, true);
    uint64_t onBefore = 0, onAfter = 0;
    LastSpreadPacketCounts(onBefore, onAfter);

    int64_t offDx = 0, onDx = 0;
    for (const auto& a : off) {
        if (a.type == ActionType::MoveMouseRelative) offDx += a.x;
    }
    for (const auto& a : on) {
        if (a.type == ActionType::MoveMouseRelative) onDx += a.x;
    }

    Emit(L"prepare_playback_timeline_applies_spread",
        offAfter == offBefore && onAfter > onBefore && offDx == onDx && onAfter > 0, L"");
}

void CaseSpreadRelativeMovePacketsPreservesTotals() {
    // 细分变换的不变量：Σ等待 与 Σ位移 必须**严格不变**（这是它敢用在回放前的底线），
    // 且细分数按窗口长度自适应、再跑一次不再变化（幂等）。
    std::vector<ScriptAction> acts;
    auto pushRel = [&](uint64_t waitUs, int dx, int dy) {
        acts.push_back(MakeExplicitWaitUs(waitUs, 0));
        ScriptAction m{};
        m.type = ActionType::MoveMouseRelative;
        m.x = dx;
        m.y = dy;
        acts.push_back(std::move(m));
    };
    pushRel(8000, 22, 4);     // 8ms ⇒ 4 份（每份 2ms）
    pushRel(8000, -22, -4);   // 负位移
    pushRel(8000, 1, 0);      // 小位移：部分子步为 0，被跳过但等待照发
    pushRel(1000, 9, 3);      // < 2ms 不细分
    pushRel(3000, 7, 5);      // 3ms ⇒ 2 份（每份 1.5ms）
    pushRel(0, 0, 0);         // 零位移不细分

    int64_t sumDx = 0, sumDy = 0;
    uint64_t sumWait = 0;
    int relCount = 0;
    for (const auto& a : acts) {
        if (a.type == ActionType::MoveMouseRelative) {
            sumDx += a.x;
            sumDy += a.y;
            ++relCount;
        } else if (a.type == ActionType::Wait) {
            sumWait += ActionStepUs(a);
        }
    }
    const int64_t wantDx = 22 - 22 + 1 + 9 + 7;
    const int64_t wantDy = 4 - 4 + 0 + 3 + 5;
    const uint64_t wantWait = 8000ULL * 3 + 1000 + 3000;
    const bool baselineOk = sumDx == wantDx && sumDy == wantDy && sumWait == wantWait;

    std::vector<ScriptAction> spread = acts;
    SpreadRelativeMovePackets(spread, 4);

    int64_t sDx = 0, sDy = 0;
    uint64_t sWait = 0;
    int sRelCount = 0;
    uint64_t maxPartUs = 0;
    for (const auto& a : spread) {
        if (a.type == ActionType::MoveMouseRelative) {
            sDx += a.x;
            sDy += a.y;
            ++sRelCount;
        } else if (a.type == ActionType::Wait) {
            const uint64_t w = ActionStepUs(a);
            sWait += w;
            if (w > maxPartUs) maxPartUs = w;
        }
    }
    const bool totalsPreserved = sDx == sumDx && sDy == sumDy && sWait == sumWait;
    const bool shapeOk = sRelCount > relCount && maxPartUs <= 2000;

    std::vector<ScriptAction> again = spread;
    SpreadRelativeMovePackets(again, 4);
    const bool idemOk = again.size() == spread.size();

    Emit(L"spread_relative_move_packets_preserves_totals",
        baselineOk && totalsPreserved && shapeOk && idemOk, L"");
}

void CaseRelativeCaptureDecision() {
    using M = RecordingCaptureMode;
    const uint64_t sticky = kAutoRelativeStickyUs;
    // 强制模式与相对状态无关。
    const bool forced = EvaluateRelativeCapture(M::Relative, false, 0, 5000, sticky)
        && !EvaluateRelativeCapture(M::Absolute, true, 0, 5000, sticky);

    // Auto：相对态生效时立即按相对；从未相对过则按绝对。
    const bool autoBase = EvaluateRelativeCapture(M::Auto, true, 0, 1000, sticky)
        && !EvaluateRelativeCapture(M::Auto, false, 0, 1000, sticky);

    // 防抖：离开相对态后**单帧级**抖动（16ms）仍按相对，跨过去就按绝对。
    const bool antiFlap = EvaluateRelativeCapture(M::Auto, false, 10000, 10000 + 16000, sticky)
        && !EvaluateRelativeCapture(M::Auto, false, 10000, 10000 + sticky, sticky);

    // ★ 关键：粘滞窗口必须**短到不会吞掉刻意的模式切换**。
    // 用户在抓取态按 E 开背包（游戏改为读光标位置）后，250ms 内的移动若仍按相对记账，
    // 回放时就是幻影镜头旋转。这里以 120ms 为界做 A/B：
    //   粘滞 60ms ⇒ 120ms 后已按绝对 ✔；粘滞 250ms ⇒ 仍按相对 ✗。
    const bool deliberateSwitch = !EvaluateRelativeCapture(
        M::Auto, false, 10000, 10000 + 120000, sticky);

    // 时间戳回绕/相等不得判成相对（nowUs < last 视为无效历史）。
    const bool clockGuard = !EvaluateRelativeCapture(M::Auto, false, 20000, 1000, sticky);

    Emit(L"relative_capture_decision",
        forced && autoBase && antiFlap && deliberateSwitch && clockGuard, L"");
}

void CaseSumRelativeMoves() {
    // 回放保真度诊断的分子：只累加 MoveMouseRelative，忽略其它动作。
    std::vector<ScriptAction> acts;
    ScriptAction rel{};
    rel.type = ActionType::MoveMouseRelative;
    rel.x = 7; rel.y = -3;
    acts.push_back(rel);
    rel.x = -2; rel.y = 5;
    acts.push_back(rel);
    ScriptAction abs{};
    abs.type = ActionType::MoveMouse;
    abs.x = 1000; abs.y = 2000;
    acts.push_back(abs);
    ScriptAction wait{};
    wait.type = ActionType::Wait;
    wait.timingUs = 1000;
    acts.push_back(wait);
    const auto t = SumRelativeMoves(acts);
    const auto empty = SumRelativeMoves({});
    Emit(L"sum_relative_moves_counts_only_rel",
        t.dx == 5 && t.dy == 2 && t.packets == 2
        && empty.dx == 0 && empty.dy == 0 && empty.packets == 0, L"");
}

/// 回放保真判定：**「未统计」必须优先于「不一致」**。
/// 窗口/后台窗口模式软输入 / CDP 下注入侧恒为 0，若先比数值就会报出「注入层改动了位移」，
/// 在一切正常时把排查引到错误的一侧。这条用例就是钉住这个优先级。
void CaseMoveFidelityVerdict() {
    const bool notCounted = EvaluateMoveFidelity(false, 1204, -337, 0, 0)
        == MoveFidelityVerdict::NotCounted;
    // 两侧恰好都是 0 也不能当成 Match —— 那是巧合，不是「输入层忠实」的证据
    const bool notCountedEvenEqual = EvaluateMoveFidelity(false, 0, 0, 0, 0)
        == MoveFidelityVerdict::NotCounted;
    const bool match = EvaluateMoveFidelity(true, 1204, -337, 1204, -337)
        == MoveFidelityVerdict::Match;
    const bool mismatchDx = EvaluateMoveFidelity(true, 1204, -337, 1200, -337)
        == MoveFidelityVerdict::Mismatch;
    const bool mismatchDy = EvaluateMoveFidelity(true, 1204, -337, 1204, -330)
        == MoveFidelityVerdict::Mismatch;
    // 无相对移动的脚本：两侧都是 0 ⇒ 一致，不该报异常
    const bool bothZero = EvaluateMoveFidelity(true, 0, 0, 0, 0)
        == MoveFidelityVerdict::Match;

    const bool ok = notCounted && notCountedEvenEqual && match && mismatchDx
        && mismatchDy && bothZero;
    if (ok) {
        Emit(L"move_fidelity_verdict", true, L"");
        return;
    }
    wchar_t msg[192]{};
    swprintf_s(msg, L"notCounted=%d evenEqual=%d match=%d dx=%d dy=%d zero=%d",
        notCounted ? 1 : 0, notCountedEvenEqual ? 1 : 0, match ? 1 : 0,
        mismatchDx ? 1 : 0, mismatchDy ? 1 : 0, bothZero ? 1 : 0);
    Emit(L"move_fidelity_verdict", false, msg);
}

/// `recorder_diag.log` 超限裁剪只许按**整行**切。
/// 直接按字节数切会把一行切成半行 —— 日志开头就是乱码，比不裁更糟。
/// 约定「宁可多留」：找不到换行、或换行恰在末位，都返回 0（整块全留）。
void CaseRecorderDiagTrimLineStart() {
    const std::wstring block = L"aaa\nbbb\nccc";  // 无结尾换行
    const wchar_t* p = block.c_str();
    const size_t n = block.size();

    const bool firstLine = qst_recorder::RecorderDiagLineStart(p, n) == 4;   // 跳过 "aaa\n"
    const bool noNewline = qst_recorder::RecorderDiagLineStart(p, 3) == 0;   // 整块无换行 ⇒ 全留
    const bool newlineLast = qst_recorder::RecorderDiagLineStart(p, 4) == 0; // 换行在末位 ⇒ 全留
    const bool empty = qst_recorder::RecorderDiagLineStart(p, 0) == 0
        && qst_recorder::RecorderDiagLineStart(nullptr, 5) == 0;
    const bool leadNewline = qst_recorder::RecorderDiagLineStart(L"\nabc", 4) == 1;

    // 保留量必须是整块的上限以内，且能整除一个 wchar（UTF-16 切半字符会成乱码）
    const bool keepChars = qst_recorder::RecorderDiagTrimKeepChars() > 0
        && qst_recorder::RecorderDiagTrimKeepChars() * sizeof(wchar_t) * 2
            <= static_cast<size_t>(qst_recorder::kRecorderDiagMaxBytes);

    // ★★同一套裁剪给**多个**诊断日志用（`recorder_diag.log` / `ai_action_debug.log`，
    //   docs §61）⇒ 上限是参数。逐格钉住参数化后的行为，别让它只对默认值成立。
    const bool paramKeep = qst_recorder::DiagTrimKeepChars(4 * 1024 * 1024)
            == (4u * 1024u * 1024u / 2u) / sizeof(wchar_t)
        && qst_recorder::DiagTrimKeepChars(qst_recorder::kRecorderDiagMaxBytes)
            == qst_recorder::RecorderDiagTrimKeepChars();
    const bool needsTrim = qst_recorder::DiagLogNeedsTrim(100, 50)
        && !qst_recorder::DiagLogNeedsTrim(50, 50)
        && !qst_recorder::DiagLogNeedsTrim(100, 0);      // 上限 0 = 不裁（别把日志裁没）
    // 单行裁剪：短行**原样**（不许无端加尾巴），超长行截断且**如实留痕**
    const std::wstring shortLine = qst_recorder::ClampDiagLogLine(std::wstring(L"abcdef"), 6);
    const std::wstring clamped = qst_recorder::ClampDiagLogLine(std::wstring(L"abcdef"), 3);
    const bool clampOk = shortLine == L"abcdef"
        && clamped.size() > 3
        && clamped.compare(0, 3, L"abc") == 0
        && clamped.find(L"已截断 3 字") != std::wstring::npos;

    const bool ok = firstLine && noNewline && newlineLast && empty
        && leadNewline && keepChars && paramKeep && needsTrim && clampOk;
    if (ok) {
        Emit(L"recorder_diag_trim_line_start", true, L"");
        return;
    }
    wchar_t msg[320]{};
    swprintf_s(msg, L"first=%d none=%d last=%d empty=%d lead=%d keep=%zu p=%d trim=%d clamp=%d",
        firstLine ? 1 : 0, noNewline ? 1 : 0, newlineLast ? 1 : 0,
        empty ? 1 : 0, leadNewline ? 1 : 0, qst_recorder::RecorderDiagTrimKeepChars(),
        paramKeep ? 1 : 0, needsTrim ? 1 : 0, clampOk ? 1 : 0);
    Emit(L"recorder_diag_trim_line_start", false, msg);
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

void CaseWindowRelativeConversion() {
    std::vector<RecordedEvent> events{
        Ev(0, 1, WM_MOUSEMOVE, 42, 37),
        Ev(10000, 2, WM_LBUTTONDOWN, 80, 120, VK_LBUTTON),
        Ev(12000, 3, WM_LBUTTONUP, 80, 120, VK_LBUTTON),
    };
    const auto normal = ConvertRecordedEventsToActions(events, {});
    const auto wm = ConvertRecordedEventsToActions(events, {}, true);

    bool normalOk = true;
    for (const auto& a : normal.actions) {
        if (a.type == ActionType::MoveMouse || a.type == ActionType::MouseDown
            || a.type == ActionType::MouseUp) {
            if (a.windowRelative || a.coordsAreNormalized) normalOk = false;
        }
    }
    bool wmOk = !wm.actions.empty();
    bool sawMove = false, sawDown = false;
    for (const auto& a : wm.actions) {
        if (a.type == ActionType::MoveMouse) {
            sawMove = true;
            wmOk = wmOk && a.windowRelative && !a.coordsAreNormalized
                && a.x == 42 && a.y == 37;
        } else if (a.type == ActionType::MouseDown) {
            sawDown = true;
            wmOk = wmOk && a.windowRelative && !a.coordsAreNormalized
                && a.x == 80 && a.y == 120;
        }
    }
    Emit(L"window_relative_conversion", normalOk && wmOk && sawMove && sawDown,
        wmOk ? L"" : L"windowRelative flags/coords not preserved");
}

void CaseConvertWindowRelativeFindImage() {
    auto down = MakeDown(120, 80);
    down.windowRelative = true;
    down.recordedCapturePath = L"images\\t.bmp";
    down.captureOffsetX = 3;
    down.captureOffsetY = 4;
    auto up = MakeUp(120, 80);
    up.windowRelative = true;
    std::vector<ScriptAction> acts{down, up};
    RenumberScriptActions(acts);
    ConvertActionsToFindImage(acts, 0, 2, {});
    bool found = false;
    bool ok = false;
    for (const auto& a : acts) {
        if (a.type != ActionType::FindImage) continue;
        found = true;
        ok = a.windowRelative && a.searchFullScreen
            && !a.coordsAreNormalized
            && a.offsetX == 3 && a.offsetY == 4;
    }
    Emit(L"convert_window_relative_findimage", found && ok,
        found ? L"" : L"no findImage after window-relative convert");
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

/// 「注入超预算 ⇒ 后续短等待立刻过点」的最小复现与判读。
///
/// 现场症状（MC 1.21.1 后台回放）：`[时间轴统计]` 里 `p95` 改善但 `max` 不降，
/// 且**每一行极短等待（0.000~0.003 秒）都带显著 `late=`**。看着像「等待实现不准」，
/// 实际成因是**相反的方向**：相对移动/键盘注入本身耗掉了预算，等到 `WaitDeltaUs`
/// 检查时 deadline 已经过去 ⇒ 它只能立刻返回并把超支如实记进 `late`。
///
/// 这条用例锁住的正是这个因果方向 —— 免得下次有人看到「短等待全带 late」
/// 就去改 `input_timeline_scheduler` 的自旋/定时器切片（那一层没问题）。
void CaseTimelineInjectionOverrunEatsShortWaitBudget() {
    PrecisionInputTimeline tl;
    tl.Reset();
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    auto nowQpc = [] {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    };
    auto msBetween = [&](long long a, long long b) {
        return (b - a) * 1000.0 / static_cast<double>(freq.QuadPart);
    };

    // 第一拍：正常 2ms，建立 origin 与 elapsedUs_。
    tl.WaitDeltaUs(2000, [] { return false; });

    // 模拟「一次注入把预算吃光」：在两次等待之间**烧掉 5ms**（真实里是
    // `PrepareSoftInput` 的全树枚举 / 跨进程序列化，不是睡眠）。
    {
        const long long b0 = nowQpc();
        while (msBetween(b0, nowQpc()) < 5.0) {
        }
    }

    // 紧接一次极短等待（1ms）。deadline = origin + 3ms，而此刻已经是 origin + 7ms
    // ⇒ 必须**立刻返回**，且 `LastLatenessUs()` 必须≈超出量（4ms 上下），
    // 绝不能是 0（那意味着超支被静默吞掉，`late` 就失去判读价值）。
    const long long t0 = nowQpc();
    tl.WaitDeltaUs(1000, [] { return false; });
    const double wallMs = msBetween(t0, nowQpc());
    const uint64_t lateUs = tl.LastLatenessUs();
    const auto st = tl.Stats();

    // 立刻返回：墙钟耗时应远小于 1ms（不是「睡满 1ms」）。
    const bool returnedImmediately = wallMs < 1.0;
    // 超支被如实记录：4ms 量级（给宽区间以吸收调度抖动）。
    const bool overrunRecorded = lateUs >= 2500 && lateUs <= 9000;
    // 4ms 超支 < 8ms 重基准阈值 ⇒ 不许 rebase（rebase 会把相位整体平移，
    // 用在「轻微超支」上会让落点整体漂移，正是用户说的「偏差更大」）。
    const bool noRebaseOnModerateOverrun = st.rebaseCount == 0;
    // 该笔迟到必须进入统计（否则 p95/max 看不见它）。
    const bool countedInStats = st.eventCount == 2 && st.maxLateUs >= 2500;

    const bool ok = returnedImmediately && overrunRecorded && noRebaseOnModerateOverrun
        && countedInStats;
    wchar_t detail[320]{};
    swprintf_s(detail,
        L"wall=%.2fms late=%lluus rebase=%llu events=%llu max=%lluus"
        L" | imm=%d rec=%d noRebase=%d counted=%d",
        wallMs, static_cast<unsigned long long>(lateUs),
        static_cast<unsigned long long>(st.rebaseCount),
        static_cast<unsigned long long>(st.eventCount),
        static_cast<unsigned long long>(st.maxLateUs),
        returnedImmediately ? 1 : 0, overrunRecorded ? 1 : 0,
        noRebaseOnModerateOverrun ? 1 : 0, countedInStats ? 1 : 0);
    Emit(L"timeline_injection_overrun_eats_short_wait", ok, detail);
}

void CaseTimelineCatchupSkipsStall() {
    PrecisionInputTimeline tl;
    tl.Reset();
    // 小抖动 5ms、<8ms 阈值：后面 4ms 计划应全部追赶。计划总长必须 < 卡顿，否则后半段会重新睡。
    LARGE_INTEGER freq{}, tSpin0{}, tSpin1{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tSpin0);
    for (;;) {
        QueryPerformanceCounter(&tSpin1);
        const double spinMs = (tSpin1.QuadPart - tSpin0.QuadPart) * 1000.0
            / static_cast<double>(freq.QuadPart);
        if (spinMs >= 5.0) break;
    }
    const double spunMs = (tSpin1.QuadPart - tSpin0.QuadPart) * 1000.0
        / static_cast<double>(freq.QuadPart);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 4; ++i)
        tl.WaitDeltaUs(1000, [] { return false; });
    QueryPerformanceCounter(&t1);
    const double wallMs = (t1.QuadPart - t0.QuadPart) * 1000.0
        / static_cast<double>(freq.QuadPart);
    if (spunMs >= 7.5) {
        Emit(L"timeline_catchup_skips_stall", true, L"spin overshot rebase band");
        return;
    }
    Emit(L"timeline_catchup_skips_stall",
        wallMs < 3.0 && tl.Stats().rebaseCount == 0, L"");
}

void CaseTimelineLargeStallRebases() {
    PrecisionInputTimeline tl;
    tl.Reset();
    LARGE_INTEGER freq{}, tSpin0{}, tSpin1{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tSpin0);
    for (;;) {
        QueryPerformanceCounter(&tSpin1);
        const double spinMs = (tSpin1.QuadPart - tSpin0.QuadPart) * 1000.0
            / static_cast<double>(freq.QuadPart);
        if (spinMs >= 40.0) break;
    }
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 10; ++i)
        tl.WaitDeltaUs(10000, [] { return false; }); // 计划 100ms
    QueryPerformanceCounter(&t1);
    const double wallMs = (t1.QuadPart - t0.QuadPart) * 1000.0
        / static_cast<double>(freq.QuadPart);
    const auto st = tl.Stats();
    // 若仍整段追赶，wall 会 <10ms；rebase 后应接近剩余计划时间。
    Emit(L"timeline_large_stall_rebases",
        wallMs > 50.0 && st.rebaseCount >= 1, L"");
}

void CaseTimelineWaitUntilRebases() {
    PrecisionInputTimeline tl;
    tl.Reset();
    LARGE_INTEGER freq{}, tSpin0{}, tSpin1{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tSpin0);
    for (;;) {
        QueryPerformanceCounter(&tSpin1);
        const double spinMs = (tSpin1.QuadPart - tSpin0.QuadPart) * 1000.0
            / static_cast<double>(freq.QuadPart);
        if (spinMs >= 40.0) break;
    }
    tl.WaitUntilElapsedUs(10000, [] { return false; });
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 8; ++i)
        tl.WaitDeltaUs(10000, [] { return false; });
    QueryPerformanceCounter(&t1);
    const double wallMs = (t1.QuadPart - t0.QuadPart) * 1000.0
        / static_cast<double>(freq.QuadPart);
    const auto st = tl.Stats();
    // 不 rebase：已过点约 40ms，8×10ms 只再睡 ~50ms；rebase 后应再睡满约 80ms。
    Emit(L"timeline_wait_until_rebases",
        wallMs > 70.0 && st.rebaseCount >= 1, L"");
}

void CaseTimelineLongWaitWall() {
    PrecisionInputTimeline tl;
    tl.Reset();
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    tl.WaitDeltaUs(200000, [] { return false; });
    QueryPerformanceCounter(&t1);
    const double wallMs = (t1.QuadPart - t0.QuadPart) * 1000.0
        / static_cast<double>(freq.QuadPart);
    Emit(L"timeline_long_wait_wall", wallMs > 150.0 && wallMs < 400.0, L"");
}

void CaseTimelineGapFromNowNoCatchup() {
    PrecisionInputTimeline tl;
    tl.Reset();
    LARGE_INTEGER freq{}, tSpin0{}, tSpin1{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tSpin0);
    for (;;) {
        QueryPerformanceCounter(&tSpin1);
        const double spinMs = (tSpin1.QuadPart - tSpin0.QuadPart) * 1000.0
            / static_cast<double>(freq.QuadPart);
        if (spinMs >= 30.0) break;
    }
    QueryPerformanceCounter(&t0);
    tl.WaitGapUs(20000, [] { return false; });
    QueryPerformanceCounter(&t1);
    const double wallMs = (t1.QuadPart - t0.QuadPart) * 1000.0
        / static_cast<double>(freq.QuadPart);
    Emit(L"timeline_gap_from_now_no_catchup", wallMs > 12.0, L"");
}

void CaseTimelineLowPowerKeepsDeadlines() {
    // 低性能模式只把忙自旋从 12ms 压到 800us：绝对时间轴仍必须按 deadline 命中，
    // 抖动放大到亚毫秒级可以接受，但不能退化成「整段睡过头」。
    PrecisionInputTimeline tl;
    SetLowPerformanceMode(true);
    tl.Reset();
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 12; ++i)
        tl.WaitDeltaUs(1000, [] { return false; });   // 计划 12ms
    tl.WaitDeltaUs(20000, [] { return false; });      // 再加 20ms → 计划 32ms
    QueryPerformanceCounter(&t1);
    const double wallMs = (t1.QuadPart - t0.QuadPart) * 1000.0
        / static_cast<double>(freq.QuadPart);
    const auto st = tl.Stats();
    SetLowPerformanceMode(false);

    const bool wallOk = wallMs > 28.0 && wallMs < 80.0;
    // p99 迟到 <= 3ms：高精度 waitable timer 唤醒误差约 0.5ms，机器忙时会被调度推迟，
    // 断言留足余量（只保证不退化，不追求亚毫秒）
    const bool jitterOk = st.p99LateUs <= 3000;
    Emit(L"timeline_low_power_keeps_deadlines", wallOk && jitterOk,
        (L"wall=" + std::to_wstring(static_cast<int>(wallMs)) + L"ms p99迟到="
            + std::to_wstring(st.p99LateUs) + L"us 事件=" + std::to_wstring(st.eventCount))
            .c_str());
    Emit(L"low_power_flag_toggles", !LowPerformanceMode(), L"关闭后开关已复位");
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

void CaseHoverPatchCoversClick() {
    const bool ok = HoverPatchCoversClick(100, 200, 100, 200, 8)
        && HoverPatchCoversClick(100, 200, 108, 192, 8)
        && !HoverPatchCoversClick(100, 200, 109, 200, 8)
        && !HoverPatchCoversClick(100, 200, 100, 191, 8)
        && HoverPatchCoversClick(0, 0, 0, 0, 0)
        && !HoverPatchCoversClick(0, 0, 1, 0, 0);
    Emit(L"hover_patch_covers_click", ok, ok ? L"" : L"cover radius mismatch");
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

void CaseRecordingScopeFilter() {
    const HWND rootA = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x100));
    const HWND rootB = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x200));
    {
        const auto e = EvaluateRecordingScopeFilter(1, rootA, false, rootB, false);
        Emit(L"scope_global_always_accept", e.accept && !e.newRoot, L"");
    }
    {
        const auto e = EvaluateRecordingScopeFilter(0, rootA, false, rootB, false);
        Emit(L"scope_window_rejects_other_root", !e.accept && !e.newRoot, L"");
    }
    {
        const auto e = EvaluateRecordingScopeFilter(0, nullptr, true, rootB, false);
        Emit(L"scope_arm_locks_first_external", e.accept && e.newRoot == rootB, L"");
    }
    {
        const auto e = EvaluateRecordingScopeFilter(0, nullptr, true, rootB, true);
        Emit(L"scope_arm_skips_own_process", !e.accept && !e.newRoot, L"");
    }
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
    CaseRepairCopyLeavesSource();
    CaseRelReportIntervalHighPolling();
    CaseRelReportIntervalCompressedBurst();
    CaseRelReportIntervalBackgroundCap();
    CaseHighPollingRelGapsPreserved();
    CaseSpreadRelativeMovePacketsPreservesTotals();
    CasePreparePlaybackTimelineAppliesSpread();
    CaseSumRelativeMoves();
    CaseRecorderDiagTrimLineStart();
    CaseMoveFidelityVerdict();
    CaseRelativeCaptureDecision();
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
    CaseWindowRelativeConversion();
    CaseSameTimestampNoWait();
    CaseTimelineFoldVsExplicitEquiv();
    CaseExpandRecordingKeepsGaps();
    CaseExpandScriptDefaultNoWait();
    CaseExpandIdempotent();
    CaseWaitStatsUseTimingUs();
    CaseTimelineInjectionOverrunEatsShortWaitBudget();
    CaseTimelineCatchupSkipsStall();
    CaseTimelineLargeStallRebases();
    CaseTimelineWaitUntilRebases();
    CaseTimelineLongWaitWall();
    CaseTimelineGapFromNowNoCatchup();
    CaseTimelineLowPowerKeepsDeadlines();
    CaseSchedulerCancel();
    CaseSchedulerWaitUntil();
    CaseClickCaptureRect();
    CaseHoverPatchCoversClick();
    CasePairDownUp();
    CaseRejectDragDistance();
    CaseRejectDragMoveCount();
    CaseRejectModifier();
    CaseAllowRelative();
    CaseDeleteAbsKeepRel();
    CaseModifierScanForward();
    CaseSnapTimingToWait();
    CaseCaptureFilename();
    CaseRecordingScopeFilter();
    CaseConvertFields();
    CaseConvertWindowRelativeFindImage();
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
