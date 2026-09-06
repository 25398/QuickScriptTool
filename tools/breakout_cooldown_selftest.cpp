// =============================================================================
// BreakoutCooldownSelfTest — 脱离冷却：按住不计时 / 松开后才开始 idle
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:BreakoutCooldownSelfTest
//   build\Release\BreakoutCooldownSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "breakout_cooldown.h"

#include <chrono>
#include <string>

namespace {

using selftest::Emit;
using breakout_input::BreakoutCooldownState;
using breakout_input::BreakoutCooldownStillWaiting;
using breakout_input::BreakoutHoldTracker;
using clock = std::chrono::steady_clock;

const selftest::CaseInfo kCases[] = {
    {L"hold_blocks_cooldown", L"default",
        L"Holding a key longer than idle must not finish cooldown"},
    {L"release_starts_idle", L"default",
        L"Cooldown starts only after release and then waits idle"},
    {L"fresh_input_resets_idle", L"default",
        L"New input after release restarts idle timer"},
    {L"hold_tracker_down_up", L"default",
        L"NoteDown/NoteUp toggle Holding"},
    {L"hold_tracker_multi_key", L"default",
        L"Release one of two keys still Holding"},
    {L"hold_tracker_zero_vk", L"default",
        L"vk=0 is ignored"},
    {L"reconcile_drops_released", L"default",
        L"Reconcile removes keys that IsDown says are up"},
};

void CaseHoldBlocksCooldown() {
    BreakoutCooldownState st{};
    const auto t0 = clock::now();
    const auto idle = std::chrono::milliseconds(200);
    bool still = true;
    for (int i = 0; i <= 6; ++i) {
        still = BreakoutCooldownStillWaiting(true, false, t0 + idle * i, idle, st);
        if (!still) break;
    }
    Emit(L"hold_blocks_cooldown", still && !st.armed,
        still ? L"holding kept waiting" : L"cooldown finished while holding");
}

void CaseReleaseStartsIdle() {
    BreakoutCooldownState st{};
    const auto t0 = clock::now();
    const auto idle = std::chrono::milliseconds(500);
    const bool duringHold = BreakoutCooldownStillWaiting(true, true, t0, idle, st);
    const bool justReleased = BreakoutCooldownStillWaiting(false, false, t0 + idle, idle, st);
    const bool stillShort = BreakoutCooldownStillWaiting(
        false, false, t0 + idle + std::chrono::milliseconds(200), idle, st);
    const bool done = !BreakoutCooldownStillWaiting(
        false, false, t0 + idle + idle, idle, st);
    const bool ok = duringHold && justReleased && stillShort && done && st.armed;
    Emit(L"release_starts_idle", ok,
        ok ? L"idle armed after release" : L"idle did not start/finish as expected");
}

void CaseFreshInputResetsIdle() {
    BreakoutCooldownState st{};
    const auto t0 = clock::now();
    const auto idle = std::chrono::milliseconds(400);
    BreakoutCooldownStillWaiting(false, false, t0, idle, st);
    const bool reset = BreakoutCooldownStillWaiting(
        false, true, t0 + std::chrono::milliseconds(300), idle, st);
    const bool still = BreakoutCooldownStillWaiting(
        false, false, t0 + std::chrono::milliseconds(600), idle, st);
    const bool done = !BreakoutCooldownStillWaiting(
        false, false, t0 + std::chrono::milliseconds(300) + idle, idle, st);
    const bool ok = reset && still && done;
    Emit(L"fresh_input_resets_idle", ok,
        ok ? L"fresh input rearmed idle" : L"reset failed");
}

void CaseHoldTrackerDownUp() {
    BreakoutHoldTracker h;
    h.NoteDown(0x41);
    const bool down = h.Holding() && h.Count() == 1;
    h.NoteUp(0x41);
    const bool up = !h.Holding() && h.Count() == 0;
    Emit(L"hold_tracker_down_up", down && up,
        (down && up) ? L"A down/up" : L"hold state mismatch");
}

void CaseHoldTrackerMulti() {
    BreakoutHoldTracker h;
    h.NoteDown(0x41);
    h.NoteDown(0x42);
    h.NoteDown(0x41);
    const bool two = h.Count() == 2;
    h.NoteUp(0x41);
    const bool still = h.Holding() && h.Count() == 1;
    h.NoteUp(0x42);
    const bool clear = !h.Holding();
    Emit(L"hold_tracker_multi_key", two && still && clear,
        (two && still && clear) ? L"A+B" : L"multi-key hold mismatch");
}

void CaseHoldTrackerZeroVk() {
    BreakoutHoldTracker h;
    h.NoteDown(0);
    const bool ignored = !h.Holding();
    Emit(L"hold_tracker_zero_vk", ignored,
        ignored ? L"vk0 ignored" : L"vk0 was tracked");
}

void CaseReconcileDropsReleased() {
    BreakoutHoldTracker h;
    h.NoteDown(0x41);
    h.NoteDown(0x42);
    h.Reconcile([](UINT vk) { return vk == 0x42; });
    const bool ok = h.Holding() && h.Count() == 1;
    h.Reconcile([](UINT) { return false; });
    const bool empty = !h.Holding();
    Emit(L"reconcile_drops_released", ok && empty,
        (ok && empty) ? L"dropped A kept B then cleared" : L"reconcile mismatch");
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"BreakoutCooldownSelfTest — 脱离冷却按住不计时\n"
        L"\n"
        L"用法:\n"
        L"  BreakoutCooldownSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/breakout_cooldown.h, src/breakout_input.h\n");
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
        selftest::PrintCaseList(L"BreakoutCooldownSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseHoldBlocksCooldown();
    CaseReleaseStartsIdle();
    CaseFreshInputResetsIdle();
    CaseHoldTrackerDownUp();
    CaseHoldTrackerMulti();
    CaseHoldTrackerZeroVk();
    CaseReconcileDropsReleased();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
