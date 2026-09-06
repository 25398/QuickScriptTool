// =============================================================================
// HotkeyStopSelfTest — 运行中停止热键：忙碌时不得被闩锁/指纹吞掉
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:HotkeyStopSelfTest
//   build\Release\HotkeyStopSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "hotkey_stop.h"

#include <string>

namespace {

using selftest::Emit;
using hotkey_stop::PollerState;
using hotkey_stop::PollTick;
using hotkey_stop::ShouldClearToggleLatchOnKeyUp;
using hotkey_stop::ShouldStartOnToggleKeyDown;
using hotkey_stop::ShouldStopOnToggleKeyDown;
using hotkey_stop::TickPoller;
using hotkey_stop::TickIdleStart;
using hotkey_stop::IdleStartState;
using hotkey_stop::IdleTick;
using hotkey_stop::LlOwnsToggleHotkey;
using hotkey_stop::TryConsumeTogglePress;
using hotkey_stop::AllowBusyToggleStop;
using hotkey_stop::kIdleFallbackConfirmMs;
using hotkey_stop::ShouldTreatRawMouseUpAsPhysical;
using hotkey_stop::DedicatedHoldOwnsRun;

const selftest::CaseInfo kCases[] = {
    {L"busy_physical_down_stops", L"default",
        L"Busy physical toggle key down must stop"},
    {L"busy_needkeyup_does_not_stop", L"default",
        L"Busy auto-repeat (NeedKeyUp) must not stop"},
    {L"busy_injected_down_ignored", L"default",
        L"Injected/tagged key down must not stop"},
    {L"idle_needkeyup_blocks_start", L"default",
        L"Idle NeedKeyUp/pending/handling still blocks start"},
    {L"idle_physical_can_start", L"default",
        L"Idle physical down without latches can start"},
    {L"physical_up_clears_latch", L"default",
        L"Physical KEYUP clears NeedKeyUp even if fingerprint would match"},
    {L"injected_up_does_not_clear", L"default",
        L"Injected KEYUP does not clear toggle latch"},
    {L"poller_hold_start_does_not_fire", L"default",
        L"Poller waits for start-key release before stop"},
    {L"poller_release_then_press_fires", L"default",
        L"Poller fires after release then a new press"},
    {L"poller_synthetic_press_skipped", L"default",
        L"Poller ignores GetAsyncKeyState echo of injected key"},
    {L"poller_idle_resets", L"default",
        L"Leaving busy resets poller latches"},
    {L"idle_fallback_waits_confirm", L"default",
        L"Idle fallback waits confirmMs before firing"},
    {L"idle_fallback_fires_after_confirm", L"default",
        L"Idle fallback fires after confirm if nothing consumed the press"},
    {L"idle_fallback_skips_if_consumed", L"default",
        L"Idle fallback skips when RegisterHotKey already consumed the press"},
    {L"idle_fallback_skips_when_ll_owns", L"default",
        L"Idle fallback skips when live LL hook owns the hotkey"},
    {L"idle_fallback_resets_on_release", L"default",
        L"Idle fallback can fire again after key up"},
    {L"idle_fallback_skips_synthetic", L"default",
        L"Idle fallback ignores injected GetAsyncKeyState echo"},
    {L"toggle_consume_second_press_blocked", L"default",
        L"Second toggle channel of the same press is consumed"},
    {L"ll_owns_only_when_fresh_and_hook_path", L"default",
        L"LL owns toggle only when hook is fresh and IME/suspend/reg-fail"},
    {L"busy_stop_honors_emergency_if_consume_stuck", L"default",
        L"Busy stop proceeds when emergency is set even if start-press consume latch stuck"},
    {L"busy_stop_blocks_duplicate_without_emergency", L"default",
        L"Busy stop still blocks a duplicate channel when emergency is not set"},
    {L"raw_mouse_up_physical", L"default",
        L"Real-device mouse up without ExtraInfo is physical release"},
    {L"raw_mouse_up_injected_not_physical", L"default",
        L"VHID device or ExtraInfo mouse up is not physical release"},
    {L"dedicated_hold_owns_run", L"default",
        L"Script/recording hold session is not stopped by global hold-release"},
};

void CaseBusyPhysicalDownStops() {
    const bool ok = ShouldStopOnToggleKeyDown(true, false, false, false);
    Emit(L"busy_physical_down_stops", ok,
        ok ? L"" : L"busy physical down did not request stop");
}

void CaseBusyNeedKeyUpDoesNotStop() {
    const bool ok = !ShouldStopOnToggleKeyDown(true, true, false, false);
    Emit(L"busy_needkeyup_does_not_stop", ok,
        ok ? L"" : L"NeedKeyUp auto-repeat was treated as stop");
}

void CaseBusyInjectedIgnored() {
    const bool inj = ShouldStopOnToggleKeyDown(true, false, true, false);
    const bool tag = ShouldStopOnToggleKeyDown(true, false, false, true);
    const bool idle = ShouldStopOnToggleKeyDown(false, false, false, false);
    const bool ok = !inj && !tag && !idle;
    Emit(L"busy_injected_down_ignored", ok,
        ok ? L"" : L"injected/idle incorrectly treated as stop");
}

void CaseIdleNeedKeyUpBlocksStart() {
    const bool blocked = !ShouldStartOnToggleKeyDown(
        false, true, false, false, false, false);
    const bool pending = !ShouldStartOnToggleKeyDown(
        false, false, true, false, false, false);
    const bool handling = !ShouldStartOnToggleKeyDown(
        false, false, false, true, false, false);
    const bool ok = blocked && pending && handling;
    Emit(L"idle_needkeyup_blocks_start", ok,
        ok ? L"" : L"idle latches failed to block start");
}

void CaseIdlePhysicalCanStart() {
    const bool ok = ShouldStartOnToggleKeyDown(
        false, false, false, false, false, false);
    Emit(L"idle_physical_can_start", ok,
        ok ? L"" : L"idle physical down could not start");
}

void CasePhysicalUpClearsLatch() {
    const bool ok = ShouldClearToggleLatchOnKeyUp(false, false);
    Emit(L"physical_up_clears_latch", ok,
        ok ? L"" : L"physical KEYUP did not clear latch");
}

void CaseInjectedUpDoesNotClear() {
    const bool inj = ShouldClearToggleLatchOnKeyUp(true, false);
    const bool tag = ShouldClearToggleLatchOnKeyUp(false, true);
    const bool ok = !inj && !tag;
    Emit(L"injected_up_does_not_clear", ok,
        ok ? L"" : L"injected KEYUP cleared latch");
}

void CasePollerHoldStartDoesNotFire() {
    PollerState st;
    const PollTick a = TickPoller(st, true, true, false);
    const PollTick b = TickPoller(st, true, true, false);
    const bool ok = a == PollTick::WaitRelease && b == PollTick::WaitRelease
        && !st.fired && !st.releaseSeen;
    Emit(L"poller_hold_start_does_not_fire", ok,
        ok ? L"" : L"poller fired while start key still down");
}

void CasePollerReleaseThenPressFires() {
    PollerState st;
    TickPoller(st, true, true, false);
    const PollTick armed = TickPoller(st, true, false, false);
    const PollTick fire = TickPoller(st, true, true, false);
    const PollTick again = TickPoller(st, true, true, false);
    const bool ok = armed == PollTick::Armed && fire == PollTick::FireStop
        && again == PollTick::Armed && st.fired;
    Emit(L"poller_release_then_press_fires", ok,
        ok ? L"" : L"poller did not fire on second press");
}

void CasePollerSyntheticSkipped() {
    PollerState st;
    TickPoller(st, true, false, false);
    const PollTick tick = TickPoller(st, true, true, true);
    const bool ok = tick == PollTick::Armed && !st.fired;
    Emit(L"poller_synthetic_press_skipped", ok,
        ok ? L"" : L"poller treated injected echo as stop");
}

void CasePollerIdleResets() {
    PollerState st;
    TickPoller(st, true, false, false);
    TickPoller(st, true, true, false);
    const PollTick idle = TickPoller(st, false, true, false);
    const bool ok = idle == PollTick::Idle && !st.fired && !st.releaseSeen;
    Emit(L"poller_idle_resets", ok,
        ok ? L"" : L"poller latches survived idle");
}

void CaseIdleFallbackWaitsConfirm() {
    IdleStartState st;
    const IdleTick a = TickIdleStart(st, true, false, false, false, 1000, kIdleFallbackConfirmMs);
    const IdleTick b = TickIdleStart(st, true, false, false, false, 1000 + kIdleFallbackConfirmMs - 1,
        kIdleFallbackConfirmMs);
    const bool ok = a == IdleTick::WaitConfirm && b == IdleTick::WaitConfirm && !st.fired;
    Emit(L"idle_fallback_waits_confirm", ok,
        ok ? L"" : L"fallback fired before confirmMs");
}

void CaseIdleFallbackFiresAfterConfirm() {
    IdleStartState st;
    TickIdleStart(st, true, false, false, false, 1000, kIdleFallbackConfirmMs);
    const IdleTick fire = TickIdleStart(st, true, false, false, false,
        1000 + kIdleFallbackConfirmMs, kIdleFallbackConfirmMs);
    const IdleTick again = TickIdleStart(st, true, false, false, false,
        1000 + kIdleFallbackConfirmMs + 10, kIdleFallbackConfirmMs);
    const bool ok = fire == IdleTick::FireStart && again == IdleTick::Skip && st.fired;
    Emit(L"idle_fallback_fires_after_confirm", ok,
        ok ? L"" : L"fallback did not fire after confirmMs");
}

void CaseIdleFallbackSkipsIfConsumed() {
    IdleStartState st;
    const IdleTick tick = TickIdleStart(st, true, false, true, false, 1000, 0);
    const bool ok = tick == IdleTick::Skip && st.fired;
    Emit(L"idle_fallback_skips_if_consumed", ok,
        ok ? L"" : L"fallback fired despite consume latch");
}

void CaseIdleFallbackSkipsWhenLlOwns() {
    IdleStartState st;
    const IdleTick tick = TickIdleStart(st, true, false, false, true, 1000, 0);
    const bool ok = tick == IdleTick::Skip;
    Emit(L"idle_fallback_skips_when_ll_owns", ok,
        ok ? L"" : L"fallback fired while LL owns the hotkey");
}

void CaseIdleFallbackResetsOnRelease() {
    IdleStartState st;
    TickIdleStart(st, true, false, false, false, 1000, 0);
    TickIdleStart(st, false, false, false, false, 1010, 0);
    const IdleTick fire = TickIdleStart(st, true, false, false, false, 1020, 0);
    const bool ok = fire == IdleTick::FireStart;
    Emit(L"idle_fallback_resets_on_release", ok,
        ok ? L"" : L"fallback did not re-arm after key up");
}

void CaseIdleFallbackSkipsSynthetic() {
    IdleStartState st;
    const IdleTick tick = TickIdleStart(st, true, true, false, false, 1000, 0);
    const bool ok = tick == IdleTick::Skip && st.fired;
    Emit(L"idle_fallback_skips_synthetic", ok,
        ok ? L"" : L"fallback treated injected echo as start");
}

void CaseToggleConsumeSecondBlocked() {
    bool consumed = false;
    const bool first = TryConsumeTogglePress(consumed);
    const bool second = TryConsumeTogglePress(consumed);
    consumed = false;
    const bool third = TryConsumeTogglePress(consumed);
    const bool ok = first && !second && third;
    Emit(L"toggle_consume_second_press_blocked", ok,
        ok ? L"" : L"consume latch failed to block duplicate toggle");
}

void CaseLlOwnsOnlyWhenFreshAndHookPath() {
    const bool normalFresh = LlOwnsToggleHotkey(true, false, false, false);
    const bool imeFresh = LlOwnsToggleHotkey(true, true, false, false);
    const bool staleIme = LlOwnsToggleHotkey(false, true, false, false);
    const bool susp = LlOwnsToggleHotkey(true, false, true, false);
    const bool fail = LlOwnsToggleHotkey(true, false, false, true);
    const bool ok = !normalFresh && imeFresh && !staleIme && susp && fail;
    Emit(L"ll_owns_only_when_fresh_and_hook_path", ok,
        ok ? L"" : L"LlOwnsToggleHotkey gating is wrong");
}

void CaseBusyStopHonorsEmergencyIfConsumeStuck() {
    const bool ok = AllowBusyToggleStop(false, true) && AllowBusyToggleStop(true, false)
        && AllowBusyToggleStop(true, true);
    Emit(L"busy_stop_honors_emergency_if_consume_stuck", ok,
        ok ? L"" : L"emergency stop was blocked by stale consume latch");
}

void CaseBusyStopBlocksDuplicateWithoutEmergency() {
    const bool ok = !AllowBusyToggleStop(false, false);
    Emit(L"busy_stop_blocks_duplicate_without_emergency", ok,
        ok ? L"" : L"duplicate busy stop channel was allowed without emergency");
}

void CaseRawMouseUpPhysical() {
    const bool ok = ShouldTreatRawMouseUpAsPhysical(false, false);
    Emit(L"raw_mouse_up_physical", ok,
        ok ? L"" : L"real mouse up was not treated as physical");
}

void CaseRawMouseUpInjectedNotPhysical() {
    const bool vhid = !ShouldTreatRawMouseUpAsPhysical(true, false);
    const bool extra = !ShouldTreatRawMouseUpAsPhysical(false, true);
    const bool both = !ShouldTreatRawMouseUpAsPhysical(true, true);
    const bool ok = vhid && extra && both;
    Emit(L"raw_mouse_up_injected_not_physical", ok,
        ok ? L"" : L"injected/VHID mouse up was treated as physical");
}

void CaseDedicatedHoldOwnsRun() {
    const bool scriptOwns = DedicatedHoldOwnsRun(800, 702);
    const bool recOwns = DedicatedHoldOwnsRun(900, 702);
    const bool globalDoesNot = !DedicatedHoldOwnsRun(702, 702);
    const bool idleDoesNot = !DedicatedHoldOwnsRun(0, 702);
    const bool ok = scriptOwns && recOwns && globalDoesNot && idleDoesNot;
    Emit(L"dedicated_hold_owns_run", ok,
        ok ? L"" : L"dedicated hold ownership gating is wrong");
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"HotkeyStopSelfTest — 运行中停止热键不得被闩锁/指纹吞掉\n"
        L"\n"
        L"用法:\n"
        L"  HotkeyStopSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/hotkey_stop.h, src/engine/engine_hotkeys.cpp\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i] ? argv[i] : L"";
        if (a == L"--json") {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--list") {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--help" || a == L"-h") {
            PrintHelp();
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"HotkeyStopSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseBusyPhysicalDownStops();
    CaseBusyNeedKeyUpDoesNotStop();
    CaseBusyInjectedIgnored();
    CaseIdleNeedKeyUpBlocksStart();
    CaseIdlePhysicalCanStart();
    CasePhysicalUpClearsLatch();
    CaseInjectedUpDoesNotClear();
    CasePollerHoldStartDoesNotFire();
    CasePollerReleaseThenPressFires();
    CasePollerSyntheticSkipped();
    CasePollerIdleResets();
    CaseIdleFallbackWaitsConfirm();
    CaseIdleFallbackFiresAfterConfirm();
    CaseIdleFallbackSkipsIfConsumed();
    CaseIdleFallbackSkipsWhenLlOwns();
    CaseIdleFallbackResetsOnRelease();
    CaseIdleFallbackSkipsSynthetic();
    CaseToggleConsumeSecondBlocked();
    CaseLlOwnsOnlyWhenFreshAndHookPath();
    CaseBusyStopHonorsEmergencyIfConsumeStuck();
    CaseBusyStopBlocksDuplicateWithoutEmergency();
    CaseRawMouseUpPhysical();
    CaseRawMouseUpInjectedNotPhysical();
    CaseDedicatedHoldOwnsRun();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
