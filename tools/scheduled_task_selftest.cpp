// =============================================================================
// ScheduledTaskSelfTest — 定时任务调度逻辑自检（Agent / 本机）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
// 专项：.cursor/skills/scheduled-task-debug/SKILL.md + reference.md
//
//   MSBuild ... /t:ScheduledTaskSelfTest
//   build\Release\ScheduledTaskSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "scheduled_task_scheduler.h"
#include "scheduled_task_store.h"
#include "scheduled_task_types.h"
#include "utils.h"

#include <string>
#include <vector>
#include <windows.h>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"second_resolution_ignores_ms", L"default",
        L"ShouldRun matches H:M:S even when millisecond differs (1s SetTimer)"},
    {L"hourly_match", L"default",
        L"Hourly frequency matches minute+second only"},
    {L"weekly_weekday_bits", L"default",
        L"Sunday bit / SystemTimeWeekDayBit mapping"},
    {L"gates_disabled_paused_empty", L"default",
        L"globalDisabled / paused / Disabled / empty filePath block fire"},
    {L"custom_once", L"default",
        L"Custom fires once, sets customFired, no same-second re-fire"},
    {L"one_second_window", L"default",
        L"Any ms within target second is accepted"},
    {L"tick_all_due_tasks", L"default",
        L"One TickAt fires every due task in that second"},
    {L"custom_format_includes_year", L"default",
        L"Custom FormatScheduledRunTime includes YYYY-"},
    {L"parse_bool_token", L"default",
        L"ParseBoolField reads JSON token only (not substring)"},
    {L"parse_global_disabled_true", L"default",
        L"globalDisabled true parses correctly"},
    {L"drop_empty_filepath", L"default",
        L"Tasks with empty filePath / selftest dummy fixtures dropped on load"},
    {L"conflict_policy_matrix", L"default",
        L"DecideScheduledTaskFire: skip/interrupt/queue/yield; stopMacro during yield is local; clamp 0..1"},
    {L"interval_format", L"default",
        L"Interval FormatScheduledRunTime is duration with 每 prefix"},
    {L"interval_wait_then_fire", L"default",
        L"Interval waits one full period, repeats, no same-period re-fire, no burst"},
    {L"interval_zero_never_fires", L"default",
        L"Zero interval duration never fires"},
    {L"interval_reset_on_duration_change", L"default",
        L"Changing interval duration resets the elapsed clock"},
    {L"interval_preserve_clock_unrelated", L"default",
        L"Unrelated SetTasks keeps an interval task origin"},
    {L"interval_ignores_wall_clock", L"default",
        L"Interval ShouldRun is false; does not match clock time"},
    {L"parse_interval_frequency", L"default",
        L"frequency 4 parses as Interval; out-of-range stays Custom"},
    {L"interval_touch_resets_clock", L"default",
        L"TouchIntervalClock re-anchors origin (save/enable)"},
    {L"interval_global_disable_reload_resets", L"default",
        L"Clearing globalDisabled via Reload re-anchors interval clocks"},
};

SYSTEMTIME MakeSt(int y, int mo, int d, int h, int mi, int s, int ms, int dow) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(mo);
    st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(mi);
    st.wSecond = static_cast<WORD>(s);
    st.wMilliseconds = static_cast<WORD>(ms);
    st.wDayOfWeek = static_cast<WORD>(dow);  // 0=Sun … 6=Sat
    return st;
}

ScheduledTask MakeTask(ScheduledFrequency freq) {
    ScheduledTask t{};
    t.id = L"fixture_t1";
    t.name = L"fixture";
    t.filePath = L"C:\\dummy\\script.json";
    t.frequency = freq;
    t.status = ScheduledTaskStatus::Enabled;
    t.time.year = 2026;
    t.time.month = 7;
    t.time.day = 14;
    t.time.hour = 20;
    t.time.minute = 30;
    t.time.second = 15;
    t.time.millisecond = 500;
    return t;
}

void CaseSecondResolutionIgnoresMs() {
    auto task = MakeTask(ScheduledFrequency::Daily);
    task.time.millisecond = 500;
    const SYSTEMTIME now = MakeSt(2026, 7, 14, 20, 30, 15, 0, 2);
    const bool ok = ScheduledTaskShouldRun(task, now, false, false);
    Emit(L"second_resolution_ignores_ms", ok,
        ok ? L"" : L"Daily H:M:S match fails when millisecond differs (1s timer incompatible)");
}

void CaseHourlyMatch() {
    auto task = MakeTask(ScheduledFrequency::Hourly);
    task.time.minute = 12;
    task.time.second = 34;
    task.time.millisecond = 999;
    const SYSTEMTIME hit = MakeSt(2026, 7, 14, 3, 12, 34, 1, 2);
    const SYSTEMTIME miss = MakeSt(2026, 7, 14, 3, 12, 35, 1, 2);
    const bool ok = ScheduledTaskShouldRun(task, hit, false, false)
        && !ScheduledTaskShouldRun(task, miss, false, false);
    Emit(L"hourly_match", ok, ok ? L"" : L"Hourly minute/second match incorrect");
}

void CaseWeeklyWeekdayBits() {
    auto task = MakeTask(ScheduledFrequency::Weekly);
    task.time.weekDays = 0;
    SetWeekDay(task.time.weekDays, 6, true);
    const SYSTEMTIME sunday = MakeSt(2026, 7, 12, 20, 30, 15, 0, 0);
    const SYSTEMTIME monday = MakeSt(2026, 7, 13, 20, 30, 15, 0, 1);
    const bool bitOk = SystemTimeWeekDayBit(sunday) == 6 && SystemTimeWeekDayBit(monday) == 0;
    const bool runOk = ScheduledTaskShouldRun(task, sunday, false, false)
        && !ScheduledTaskShouldRun(task, monday, false, false);
    Emit(L"weekly_weekday_bits", bitOk && runOk,
        (bitOk && runOk) ? L"" : L"Sunday bit / ShouldRun weekly mapping wrong");
}

void CaseGatesDisabledPausedEmpty() {
    auto task = MakeTask(ScheduledFrequency::Daily);
    const SYSTEMTIME now = MakeSt(2026, 7, 14, 20, 30, 15, 0, 2);
    bool ok = true;
    if (ScheduledTaskShouldRun(task, now, true, false)) ok = false;
    if (ScheduledTaskShouldRun(task, now, false, true)) ok = false;
    task.status = ScheduledTaskStatus::Disabled;
    if (ScheduledTaskShouldRun(task, now, false, false)) ok = false;
    task.status = ScheduledTaskStatus::Enabled;
    task.filePath.clear();
    if (ScheduledTaskShouldRun(task, now, false, false)) ok = false;
    Emit(L"gates_disabled_paused_empty", ok,
        ok ? L"" : L"globalDisabled/paused/status/empty-path gates failed");
}

void CaseCustomOnceViaTick() {
    ScheduledTaskScheduler sched;
    int fires = 0;
    sched.SetRunCallback([&](const std::wstring&) { ++fires; });

    auto task = MakeTask(ScheduledFrequency::Custom);
    task.customFired = false;
    task.time.millisecond = 777;
    sched.SetTasks({task});

    const SYSTEMTIME now = MakeSt(task.time.year, task.time.month, task.time.day,
        task.time.hour, task.time.minute, task.time.second, 0, 2);
    sched.TickAt(now);
    sched.TickAt(now);
    const SYSTEMTIME laterMs = MakeSt(task.time.year, task.time.month, task.time.day,
        task.time.hour, task.time.minute, task.time.second, 900, 2);
    sched.TickAt(laterMs);

    const bool ok = fires == 1 && sched.Tasks().size() == 1 && sched.Tasks()[0].customFired;
    Emit(L"custom_once", ok,
        ok ? L"" : L"Custom must fire once then set customFired / suppress same-second re-fire");
}

void CaseTickFiresOncePerSecondKey() {
    auto task = MakeTask(ScheduledFrequency::Daily);
    task.time.millisecond = 0;
    const SYSTEMTIME a = MakeSt(2026, 7, 14, 20, 30, 15, 10, 2);
    const SYSTEMTIME b = MakeSt(2026, 7, 14, 20, 30, 15, 990, 2);
    const SYSTEMTIME c = MakeSt(2026, 7, 14, 20, 30, 16, 10, 2);
    const bool ok = ScheduledTaskShouldRun(task, a, false, false)
        && ScheduledTaskShouldRun(task, b, false, false)
        && !ScheduledTaskShouldRun(task, c, false, false);
    Emit(L"one_second_window", ok,
        ok ? L"" : L"ShouldRun must accept any ms within the target second");
}

void CaseTickRunsAllDueTasks() {
    ScheduledTask a = MakeTask(ScheduledFrequency::Daily);
    a.id = L"a";
    a.filePath = L"C:\\a.json";
    a.time.millisecond = 100;
    ScheduledTask b = MakeTask(ScheduledFrequency::Daily);
    b.id = L"b";
    b.filePath = L"C:\\b.json";
    b.time.millisecond = 200;

    ScheduledTaskScheduler sched;
    std::vector<std::wstring> fired;
    sched.SetRunCallback([&](const std::wstring& path) { fired.push_back(path); });
    sched.SetTasks({a, b});

    const SYSTEMTIME now = MakeSt(2026, 7, 14, 20, 30, 15, 50, 2);
    sched.TickAt(now);
    const bool ok = fired.size() == 2
        && ((fired[0] == L"C:\\a.json" && fired[1] == L"C:\\b.json")
            || (fired[0] == L"C:\\b.json" && fired[1] == L"C:\\a.json"));
    Emit(L"tick_all_due_tasks", ok,
        ok ? L"" : L"TickAt must invoke callback for every due task in the same second");
}

void CaseCustomFormatIncludesYear() {
    auto task = MakeTask(ScheduledFrequency::Custom);
    task.time.year = 2026;
    task.time.month = 7;
    task.time.day = 14;
    const std::wstring text = FormatScheduledRunTime(task);
    const bool ok = text.find(L"2026-") != std::wstring::npos;
    Emit(L"custom_format_includes_year", ok,
        ok ? L"" : L"Custom FormatScheduledRunTime must include YYYY-");
}

void CaseParseBoolTokenNotSubstring() {
    const std::wstring json =
        L"{\n"
        L"  \"tasks\": [\n"
        L"    {\n"
        L"      \"id\": \"1\",\n"
        L"      \"name\": \"truefriend\",\n"
        L"      \"kind\": 1,\n"
        L"      \"filePath\": \"C:\\\\a.json\",\n"
        L"      \"fileDisplayName\": \"a.json\",\n"
        L"      \"frequency\": 3,\n"
        L"      \"status\": 0,\n"
        L"      \"customFired\": false,\n"
        L"      \"year\": 2026, \"month\": 7, \"day\": 14,\n"
        L"      \"hour\": 9, \"minute\": 0, \"second\": 0, \"millisecond\": 0,\n"
        L"      \"weekDays\": 0\n"
        L"    }\n"
        L"  ],\n"
        L"  \"globalDisabled\": false\n"
        L"}\n";
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = true;
    const bool parsed = ParseScheduledTasksJson(json, tasks, &globalDisabled);
    const bool ok = parsed && !globalDisabled && tasks.size() == 1
        && !tasks[0].customFired
        && tasks[0].filePath == L"C:\\a.json";
    Emit(L"parse_bool_token", ok,
        ok ? L"" : L"ParseBoolField must read JSON token; drop empty filePath tasks");
}

void CaseParseGlobalDisabledTrue() {
    const std::wstring json =
        L"{\"tasks\":[],\"globalDisabled\":true}\n";
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = false;
    const bool parsed = ParseScheduledTasksJson(json, tasks, &globalDisabled);
    Emit(L"parse_global_disabled_true", parsed && globalDisabled && tasks.empty(),
        (parsed && globalDisabled) ? L"" : L"globalDisabled true not parsed");
}

void CaseDropEmptyFilePath() {
    const std::wstring json =
        L"{\"tasks\":[{\"id\":\"x\",\"name\":\"orphan\",\"filePath\":\"\",\"frequency\":1,"
        L"\"status\":0,\"customFired\":false,\"year\":0,\"month\":0,\"day\":0,"
        L"\"hour\":9,\"minute\":0,\"second\":0,\"millisecond\":0,\"weekDays\":0},"
        L"{\"id\":\"t1\",\"name\":\"selftest\",\"filePath\":\"C:\\\\dummy\\\\script.json\","
        L"\"frequency\":3,\"status\":0,\"customFired\":true,\"year\":2026,\"month\":7,\"day\":14,"
        L"\"hour\":20,\"minute\":30,\"second\":15,\"millisecond\":777,\"weekDays\":0}],"
        L"\"globalDisabled\":false}";
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = false;
    ParseScheduledTasksJson(json, tasks, &globalDisabled);
    Emit(L"drop_empty_filepath", tasks.empty(),
        tasks.empty() ? L"" : L"Empty filePath and selftest dummy fixtures must be dropped on load");
}

void CaseConflictPolicyMatrix() {
    using P = ScheduledTaskConflictPolicy;
    using A = ScheduledTaskFireAction;
    const bool ok =
        DecideScheduledTaskFire(P::RunningScriptFirst, false) == A::RunNow
        && DecideScheduledTaskFire(P::ScheduledScriptFirst, false) == A::RunNow
        && DecideScheduledTaskFire(P::RunningScriptFirst, true) == A::Skip
        && DecideScheduledTaskFire(P::ScheduledScriptFirst, true, false) == A::InterruptAndQueue
        && DecideScheduledTaskFire(P::ScheduledScriptFirst, true, true) == A::Skip
        && DecideScheduledTaskFire(P::RunningScriptFirst, false, false, true) == A::RunNow
        && DecideScheduledTaskFire(P::RunningScriptFirst, true, false, true) == A::Queue
        && DecideScheduledTaskFire(P::RunningScriptFirst, true, true, true) == A::Queue
        && DecideScheduledTaskFire(P::ScheduledScriptFirst, true, false, true) == A::YieldAndResume
        && DecideScheduledTaskFire(P::ScheduledScriptFirst, true, true, true) == A::Skip
        && StopMacroShouldEndEntireRun(0)
        && !StopMacroShouldEndEntireRun(1)
        && ClampScheduledTaskConflictPolicy(0) == P::RunningScriptFirst
        && ClampScheduledTaskConflictPolicy(1) == P::ScheduledScriptFirst
        && ClampScheduledTaskConflictPolicy(2) == P::RunningScriptFirst
        && ClampScheduledTaskConflictPolicy(9) == P::RunningScriptFirst
        && ClampScheduledTaskConflictPolicy(-1) == P::RunningScriptFirst;
    Emit(L"conflict_policy_matrix", ok, ok ? L"" : L"DecideScheduledTaskFire / clamp mismatch");
}

void CaseIntervalFormat() {
    auto task = MakeTask(ScheduledFrequency::Interval);
    task.time.hour = 8;
    task.time.minute = 0;
    task.time.second = 30;
    task.time.millisecond = 0;
    const std::wstring text = FormatScheduledRunTime(task);
    const bool ok = text.find(L"每") != std::wstring::npos
        && text.find(L"8时") != std::wstring::npos
        && ScheduledFrequencyLabel(ScheduledFrequency::Interval) == L"间隔";
    Emit(L"interval_format", ok, ok ? L"" : L"Interval format/label missing 每/时长");
}

void CaseIntervalWaitThenFire() {
    ScheduledTaskScheduler sched;
    int fires = 0;
    sched.SetRunCallback([&](const std::wstring&) { ++fires; });
    sched.SetNowMsForTest(10000);

    auto task = MakeTask(ScheduledFrequency::Interval);
    task.time.hour = 0;
    task.time.minute = 0;
    task.time.second = 5;
    task.time.millisecond = 0;
    sched.SetTasks({task});

    const SYSTEMTIME dummy = MakeSt(2026, 7, 14, 20, 30, 15, 0, 2);
    sched.TickAt(dummy);
    const int atOrigin = fires;

    sched.SetNowMsForTest(14999);
    sched.TickAt(dummy);
    const int beforeDue = fires;

    sched.SetNowMsForTest(15000);
    sched.TickAt(dummy);
    sched.TickAt(dummy);
    const int firstDue = fires;

    sched.SetNowMsForTest(20000);
    sched.TickAt(dummy);
    const int secondDue = fires;

    sched.SetNowMsForTest(35000);
    sched.TickAt(dummy);
    const int afterSkip = fires;

    const bool ok = atOrigin == 0 && beforeDue == 0 && firstDue == 1
        && secondDue == 2 && afterSkip == 3;
    Emit(L"interval_wait_then_fire", ok,
        ok ? L"" : L"Interval must wait one period, repeat, dedupe, and not burst missed periods");
}

void CaseIntervalZeroNeverFires() {
    ScheduledTaskScheduler sched;
    int fires = 0;
    sched.SetRunCallback([&](const std::wstring&) { ++fires; });
    sched.SetNowMsForTest(1000);

    auto task = MakeTask(ScheduledFrequency::Interval);
    task.time.hour = 0;
    task.time.minute = 0;
    task.time.second = 0;
    task.time.millisecond = 0;
    sched.SetTasks({task});
    sched.SetNowMsForTest(999999);
    sched.TickAt(MakeSt(2026, 7, 14, 20, 30, 15, 0, 2));
    Emit(L"interval_zero_never_fires", fires == 0,
        fires == 0 ? L"" : L"Zero interval duration must never fire");
}

void CaseIntervalResetOnDurationChange() {
    ScheduledTaskScheduler sched;
    int fires = 0;
    sched.SetRunCallback([&](const std::wstring&) { ++fires; });
    sched.SetNowMsForTest(1000);

    auto task = MakeTask(ScheduledFrequency::Interval);
    task.time.hour = 0;
    task.time.minute = 0;
    task.time.second = 5;
    task.time.millisecond = 0;
    sched.SetTasks({task});

    sched.SetNowMsForTest(4000);
    task.time.second = 10;
    sched.SetTasks({task});

    const SYSTEMTIME dummy = MakeSt(2026, 7, 14, 20, 30, 15, 0, 2);
    sched.SetNowMsForTest(9000);
    sched.TickAt(dummy);
    const int beforeNewDue = fires;
    sched.SetNowMsForTest(14000);
    sched.TickAt(dummy);
    const bool ok = beforeNewDue == 0 && fires == 1;
    Emit(L"interval_reset_on_duration_change", ok,
        ok ? L"" : L"Saving a new interval duration must restart the elapsed clock");
}

void CaseIntervalPreserveClockUnrelated() {
    ScheduledTaskScheduler sched;
    int fires = 0;
    sched.SetRunCallback([&](const std::wstring&) { ++fires; });
    sched.SetNowMsForTest(1000);

    auto interval = MakeTask(ScheduledFrequency::Interval);
    interval.id = L"iv";
    interval.time.hour = 0;
    interval.time.minute = 0;
    interval.time.second = 5;
    interval.time.millisecond = 0;
    sched.SetTasks({interval});

    sched.SetNowMsForTest(3000);
    auto daily = MakeTask(ScheduledFrequency::Daily);
    daily.id = L"d";
    daily.filePath = L"C:\\d.json";
    sched.SetTasks({interval, daily});

    sched.SetNowMsForTest(6000);
    sched.TickAt(MakeSt(2026, 7, 14, 8, 0, 0, 0, 2));
    Emit(L"interval_preserve_clock_unrelated", fires == 1,
        fires == 1 ? L"" : L"Unrelated SetTasks must keep interval origin");
}

void CaseIntervalIgnoresWallClock() {
    auto task = MakeTask(ScheduledFrequency::Interval);
    task.time.hour = 20;
    task.time.minute = 30;
    task.time.second = 15;
    const SYSTEMTIME now = MakeSt(2026, 7, 14, 20, 30, 15, 0, 2);
    const bool ok = !ScheduledTaskShouldRun(task, now, false, false)
        && ClampScheduledFrequency(4) == ScheduledFrequency::Interval
        && ClampScheduledFrequency(5) == ScheduledFrequency::Custom
        && ClampScheduledFrequency(-1) == ScheduledFrequency::Custom;
    Emit(L"interval_ignores_wall_clock", ok,
        ok ? L"" : L"Interval must not match wall clock; clamp 4=Interval else Custom");
}

void CaseParseIntervalFrequency() {
    const std::wstring json =
        L"{\"tasks\":[{\"id\":\"iv1\",\"name\":\"gap\",\"filePath\":\"C:\\\\a.json\","
        L"\"frequency\":4,\"status\":0,\"customFired\":false,\"year\":0,\"month\":0,\"day\":0,"
        L"\"hour\":0,\"minute\":1,\"second\":30,\"millisecond\":0,\"weekDays\":0}],"
        L"\"globalDisabled\":false}";
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = false;
    ParseScheduledTasksJson(json, tasks, &globalDisabled);
    const bool ok = tasks.size() == 1
        && tasks[0].frequency == ScheduledFrequency::Interval
        && tasks[0].time.minute == 1
        && tasks[0].time.second == 30
        && ScheduledIntervalDurationMs(tasks[0].time) == 90000;
    Emit(L"parse_interval_frequency", ok,
        ok ? L"" : L"frequency 4 must parse as Interval duration");
}

void CaseIntervalTouchResetsClock() {
    ScheduledTaskScheduler sched;
    int fires = 0;
    sched.SetRunCallback([&](const std::wstring&) { ++fires; });
    sched.SetNowMsForTest(1000);

    auto task = MakeTask(ScheduledFrequency::Interval);
    task.time.hour = 0;
    task.time.minute = 0;
    task.time.second = 5;
    task.time.millisecond = 0;
    sched.SetTasks({task});

    sched.SetNowMsForTest(4000);
    sched.TouchIntervalClock(task.id);

    const SYSTEMTIME dummy = MakeSt(2026, 7, 14, 20, 30, 15, 0, 2);
    sched.SetNowMsForTest(8000);
    sched.TickAt(dummy);
    const int before = fires;
    sched.SetNowMsForTest(9000);
    sched.TickAt(dummy);
    const bool ok = before == 0 && fires == 1;
    Emit(L"interval_touch_resets_clock", ok,
        ok ? L"" : L"TouchIntervalClock must restart the elapsed clock");
}

void CaseIntervalGlobalDisableReloadResets() {
    ScheduledTaskScheduler sched;
    int fires = 0;
    sched.SetRunCallback([&](const std::wstring&) { ++fires; });
    sched.SetNowMsForTest(1000);

    auto task = MakeTask(ScheduledFrequency::Interval);
    task.time.hour = 0;
    task.time.minute = 0;
    task.time.second = 5;
    task.time.millisecond = 0;
    SaveScheduledTasks({task}, false);
    sched.Reload();

    const SYSTEMTIME dummy = MakeSt(2026, 7, 14, 20, 30, 15, 0, 2);
    sched.SetNowMsForTest(3000);
    SaveScheduledTasks({task}, true);
    sched.Reload();

    sched.SetNowMsForTest(20000);
    sched.TickAt(dummy);
    const int whileDisabled = fires;

    SaveScheduledTasks({task}, false);
    sched.Reload();
    sched.TickAt(dummy);
    const int atReenable = fires;

    sched.SetNowMsForTest(25000);
    sched.TickAt(dummy);
    const bool ok = whileDisabled == 0 && atReenable == 0 && fires == 1;
    Emit(L"interval_global_disable_reload_resets", ok,
        ok ? L"" : L"Re-enabling global disable must re-anchor interval clocks, not burst-fire");
}

void PrintHelp() {
    std::fwprintf(stderr,
        L"ScheduledTaskSelfTest — 定时任务调度自检\n"
        L"\n"
        L"用法:\n"
        L"  ScheduledTaskSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  FAIL name → .cursor/skills/scheduled-task-debug/reference.md\n"
        L"\n"
        L"标志:\n"
        L"  --json   machine-readable lines + summary\n"
        L"  --list   list case names + meaning\n"
        L"  --help   this help\n");
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
        }
        else if (a == L"--help" || a == L"-h") {
            PrintHelp();
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"ScheduledTaskSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    // 隔离落盘：TickAt 对 Custom 会 Save()，绝不能覆盖产品 scheduled_tasks.json
    const std::wstring isolated = AppDir() + L"\\scheduled_tasks.selftest.json";
    DeleteFileW(isolated.c_str());
    SetScheduledTasksFilePathForTest(isolated);

    CaseSecondResolutionIgnoresMs();
    CaseHourlyMatch();
    CaseWeeklyWeekdayBits();
    CaseGatesDisabledPausedEmpty();
    CaseCustomOnceViaTick();
    CaseTickFiresOncePerSecondKey();
    CaseTickRunsAllDueTasks();
    CaseCustomFormatIncludesYear();
    CaseParseBoolTokenNotSubstring();
    CaseParseGlobalDisabledTrue();
    CaseDropEmptyFilePath();
    CaseConflictPolicyMatrix();
    CaseIntervalFormat();
    CaseIntervalWaitThenFire();
    CaseIntervalZeroNeverFires();
    CaseIntervalResetOnDurationChange();
    CaseIntervalPreserveClockUnrelated();
    CaseIntervalIgnoresWallClock();
    CaseParseIntervalFrequency();
    CaseIntervalTouchResetsClock();
    CaseIntervalGlobalDisableReloadResets();

    SetScheduledTasksFilePathForTest(L"");
    DeleteFileW(isolated.c_str());

    selftest::EmitSummary();
    return selftest::ExitCode();
}
