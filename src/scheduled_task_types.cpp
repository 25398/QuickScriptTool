#include "scheduled_task_types.h"

#include "utils.h"

#include <algorithm>
#include <cstdio>

std::wstring ScheduledTaskKindLabel(ScheduledTaskKind kind) {
    return kind == ScheduledTaskKind::Recording ? L"鼠标录制" : L"鼠标宏";
}

std::wstring ScheduledFrequencyLabel(ScheduledFrequency freq) {
    switch (freq) {
    case ScheduledFrequency::Hourly: return L"每小时";
    case ScheduledFrequency::Daily: return L"每日";
    case ScheduledFrequency::Weekly: return L"每周";
    default: return L"自定义";
    }
}

static std::wstring FormatTimeParts(int hour, int minute, int second, int millisecond,
                                    bool includeHour, bool includeDate,
                                    int year, int month, int day) {
    wchar_t buf[128]{};
    if (includeDate) {
        swprintf_s(buf, L"%04d-%02d-%02d ", year, month, day);
    }
    std::wstring prefix = includeDate ? buf : L"";
    if (includeHour) {
        swprintf_s(buf, L"%d时%d分%d秒%d毫秒", hour, minute, second, millisecond);
    } else {
        swprintf_s(buf, L"%d分%d秒%d毫秒", minute, second, millisecond);
    }
    return prefix + buf;
}

std::wstring FormatScheduledRunTime(const ScheduledTask& task) {
    const auto& t = task.time;
    switch (task.frequency) {
    case ScheduledFrequency::Hourly:
        return FormatTimeParts(0, t.minute, t.second, t.millisecond, false, false, 0, 0, 0);
    case ScheduledFrequency::Daily:
    case ScheduledFrequency::Weekly:
        return FormatTimeParts(t.hour, t.minute, t.second, t.millisecond, true, false, 0, 0, 0);
    default:
        {
            wchar_t buf[128]{};
            swprintf_s(buf, L"%02d-%02d %d时%d分%d秒%d毫秒",
                t.month, t.day, t.hour, t.minute, t.second, t.millisecond);
            return buf;
        }
    }
}

std::wstring DefaultScheduledTaskName() {
    return L"任务-" + TimestampName();
}

std::wstring GenerateScheduledTaskId() {
    return TimestampName();
}

bool WeekDaySelected(uint8_t mask, int dayIndex) {
    if (dayIndex < 0 || dayIndex > 6) return false;
    return (mask & static_cast<uint8_t>(1u << dayIndex)) != 0;
}

void SetWeekDay(uint8_t& mask, int dayIndex, bool selected) {
    if (dayIndex < 0 || dayIndex > 6) return;
    const uint8_t bit = static_cast<uint8_t>(1u << dayIndex);
    if (selected) mask |= bit;
    else mask &= static_cast<uint8_t>(~bit);
}

int SystemTimeWeekDayBit(const SYSTEMTIME& st) {
    // SYSTEMTIME: 0=Sun … 6=Sat; UI bits: 0=Mon … 6=Sun
    return st.wDayOfWeek == 0 ? 6 : static_cast<int>(st.wDayOfWeek) - 1;
}

static bool TimeMatches(const ScheduledTaskTime& t, const SYSTEMTIME& now,
                        bool matchHour, bool matchDate) {
    if (matchDate) {
        if (t.year != static_cast<int>(now.wYear)) return false;
        if (t.month != static_cast<int>(now.wMonth)) return false;
        if (t.day != static_cast<int>(now.wDay)) return false;
    }
    if (matchHour && t.hour != static_cast<int>(now.wHour)) return false;
    if (t.minute != static_cast<int>(now.wMinute)) return false;
    if (t.second != static_cast<int>(now.wSecond)) return false;
    if (t.millisecond != static_cast<int>(now.wMilliseconds)) return false;
    return true;
}

bool ScheduledTaskShouldRun(const ScheduledTask& task, const SYSTEMTIME& now,
                            bool globalDisabled, bool paused) {
    if (paused || globalDisabled) return false;
    if (task.status == ScheduledTaskStatus::Disabled) return false;
    if (task.filePath.empty()) return false;
    if (task.frequency == ScheduledFrequency::Custom && task.customFired) return false;

    switch (task.frequency) {
    case ScheduledFrequency::Hourly:
        return TimeMatches(task.time, now, false, false);
    case ScheduledFrequency::Daily:
        return TimeMatches(task.time, now, true, false);
    case ScheduledFrequency::Weekly:
        if (!WeekDaySelected(task.time.weekDays, SystemTimeWeekDayBit(now))) return false;
        return TimeMatches(task.time, now, true, false);
    default:
        return TimeMatches(task.time, now, true, true);
    }
}
