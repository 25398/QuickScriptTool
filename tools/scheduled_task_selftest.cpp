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

#include <string>
#include <vector>

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
        L"Tasks with empty filePath dropped on load"},
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
    t.id = L"t1";
    t.name = L"selftest";
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
        L"\"hour\":9,\"minute\":0,\"second\":0,\"millisecond\":0,\"weekDays\":0}],"
        L"\"globalDisabled\":false}";
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = false;
    ParseScheduledTasksJson(json, tasks, &globalDisabled);
    Emit(L"drop_empty_filepath", tasks.empty(),
        tasks.empty() ? L"" : L"Tasks without filePath must be dropped on load");
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

    selftest::EmitSummary();
    return selftest::ExitCode();
}
