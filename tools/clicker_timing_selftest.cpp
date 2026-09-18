// =============================================================================
// ClickerTimingSelfTest — 连点间隔/按下抬起：0.001s 不得截成 0ms
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ClickerTimingSelfTest
//   build\Release\ClickerTimingSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "clicker_timing.h"

#include <string>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"custom_1ms_is_1000us", L"default",
        L"0.001s custom interval must be 1000us, not int(seconds*1000)=0"},
    {L"hold_always_at_least_1ms", L"default",
        L"down-to-up hold is at least 1ms even if press-release is off or 0"},
    {L"hold_uses_press_release_when_longer", L"default",
        L"enabled 0.05s press-release is 50000us"},
    {L"gap_extreme_10ms", L"default",
        L"extreme 0.01s gap is 10000us"},
    {L"old_ms_truncation_drops_sub_ms", L"default",
        L"int(seconds*1000) turns 0.0004s into 0ms; microseconds keep 400us"},
};

void PrintHelp() {
    std::fwprintf(stdout,
        L"ClickerTimingSelfTest — clicker interval/hold microseconds\n"
        L"  --json   JSON lines + summary\n"
        L"  --list   case table\n");
}

void CaseCustom1msIs1000us() {
    const uint64_t us = ClickerGapUs(0.001);
    Emit(L"custom_1ms_is_1000us", us == 1000,
        us == 1000 ? L"0.001s -> 1000us" : L"0.001s did not map to 1000us");
}

void CaseHoldAlwaysAtLeast1ms() {
    const uint64_t off = ClickerHoldUs(false, 0.0);
    const uint64_t zero = ClickerHoldUs(true, 0.0);
    const uint64_t tiny = ClickerHoldUs(true, 0.0001);
    const bool ok = off == kClickerMinHoldUs && zero == kClickerMinHoldUs
        && tiny == kClickerMinHoldUs;
    Emit(L"hold_always_at_least_1ms", ok,
        ok ? L"hold floored to 1ms" : L"hold shorter than 1ms");
}

void CaseHoldUsesPressReleaseWhenLonger() {
    const uint64_t us = ClickerHoldUs(true, 0.05);
    Emit(L"hold_uses_press_release_when_longer", us == 50000,
        us == 50000 ? L"0.05s -> 50000us" : L"press-release not preserved");
}

void CaseGapExtreme10ms() {
    const uint64_t us = ClickerGapUs(0.01);
    Emit(L"gap_extreme_10ms", us == 10000,
        us == 10000 ? L"0.01s -> 10000us" : L"extreme gap wrong");
}

void CaseOldMsTruncationDropsSubMs() {
    const int oldMs = static_cast<int>(0.0004 * 1000.0);
    const uint64_t us = ClickerSecondsToUs(0.0004, 0);
    const bool ok = oldMs == 0 && us == 400;
    Emit(L"old_ms_truncation_drops_sub_ms", ok,
        ok ? L"0.0004s kept as 400us" : L"sub-ms interval still lost");
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
        selftest::PrintCaseList(L"ClickerTimingSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseCustom1msIs1000us();
    CaseHoldAlwaysAtLeast1ms();
    CaseHoldUsesPressReleaseWhenLonger();
    CaseGapExtreme10ms();
    CaseOldMsTruncationDropsSubMs();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
