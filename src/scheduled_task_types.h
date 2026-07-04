#pragma once
// ──────────────────────────────────────────────────────────────────
// scheduled_task_types.h — 定时任务数据结构与辅助函数
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>
#include <vector>

enum class ScheduledTaskKind {
    Recording = 0,
    Macro = 1,
};

enum class ScheduledFrequency {
    Hourly = 0,
    Daily = 1,
    Weekly = 2,
    Custom = 3,
};

enum class ScheduledTaskStatus {
    Enabled = 0,
    Disabled = 1,
};

struct ScheduledTaskTime {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 9;
    int minute = 0;
    int second = 0;
    int millisecond = 0;
    uint8_t weekDays = 0;  // bit0=Mon … bit6=Sun
};

struct ScheduledTask {
    std::wstring id;
    std::wstring name;
    ScheduledTaskKind kind = ScheduledTaskKind::Macro;
    std::wstring filePath;
    std::wstring fileDisplayName;
    ScheduledFrequency frequency = ScheduledFrequency::Custom;
    ScheduledTaskTime time{};
    ScheduledTaskStatus status = ScheduledTaskStatus::Enabled;
    bool customFired = false;
};

std::wstring ScheduledTaskKindLabel(ScheduledTaskKind kind);
std::wstring ScheduledFrequencyLabel(ScheduledFrequency freq);
std::wstring FormatScheduledRunTime(const ScheduledTask& task);
std::wstring DefaultScheduledTaskName();
std::wstring GenerateScheduledTaskId();

bool WeekDaySelected(uint8_t mask, int dayIndex);
void SetWeekDay(uint8_t& mask, int dayIndex, bool selected);
int SystemTimeWeekDayBit(const SYSTEMTIME& st);

bool ScheduledTaskShouldRun(const ScheduledTask& task, const SYSTEMTIME& now,
                            bool globalDisabled, bool paused);
