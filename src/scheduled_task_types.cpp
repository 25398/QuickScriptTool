#include "scheduled_task_types.h"

#include "utils.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>

std::wstring ScheduledTaskKindLabel(ScheduledTaskKind kind) {
    return kind == ScheduledTaskKind::Recording ? L"鼠标录制" : L"鼠标宏";
}

std::wstring ScheduledFrequencyLabel(ScheduledFrequency freq) {
    switch (freq) {
    case ScheduledFrequency::Hourly: return L"每小时";
    case ScheduledFrequency::Daily: return L"每日";
    case ScheduledFrequency::Weekly: return L"每周";
    case ScheduledFrequency::Interval: return L"间隔";
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
    case ScheduledFrequency::Interval:
        return L"每" + FormatTimeParts(t.hour, t.minute, t.second, t.millisecond,
            true, false, 0, 0, 0);
    default:
        {
            wchar_t buf[160]{};
            swprintf_s(buf, L"%04d-%02d-%02d %d时%d分%d秒%d毫秒",
                t.year, t.month, t.day, t.hour, t.minute, t.second, t.millisecond);
            return buf;
        }
    }
}

std::wstring DefaultScheduledTaskName() {
    return L"任务-" + TimestampName();
}

std::wstring GenerateScheduledTaskId() {
    // 时间戳只有秒级精度：同一秒内创建多个任务会撞 ID，
    // 而调度器按 id 去重（lastFireKey_[id]），撞 ID 会让其中一单永不触发。
    // 追加毫秒 + 随机后缀保证唯一。
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned> dist(0, 9999u);
    return std::to_wstring(ms) + L"-" + std::to_wstring(dist(gen));
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
    // 主窗口用 1s SetTimer 驱动 Tick：不能要求毫秒精确相等，否则几乎永不触发。
    // 对齐到秒即可；同一秒内的去重由 ScheduledTaskScheduler::FireKey 负责。
    if (matchDate) {
        if (t.year != static_cast<int>(now.wYear)) return false;
        if (t.month != static_cast<int>(now.wMonth)) return false;
        if (t.day != static_cast<int>(now.wDay)) return false;
    }
    if (matchHour && t.hour != static_cast<int>(now.wHour)) return false;
    if (t.minute != static_cast<int>(now.wMinute)) return false;
    if (t.second != static_cast<int>(now.wSecond)) return false;
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
    case ScheduledFrequency::Interval:
        // 按启动/保存后的累计时长触发，不对照钟点；由调度器 IntervalDue 判定。
        return false;
    default:
        return TimeMatches(task.time, now, true, true);
    }
}

int64_t ScheduledIntervalDurationMs(const ScheduledTaskTime& t) {
    const int64_t hour = t.hour < 0 ? 0 : t.hour;
    const int64_t minute = t.minute < 0 ? 0 : t.minute;
    const int64_t second = t.second < 0 ? 0 : t.second;
    const int64_t millisecond = t.millisecond < 0 ? 0 : t.millisecond;
    return hour * 3600000 + minute * 60000 + second * 1000 + millisecond;
}

bool ScheduledIntervalPeriodDue(int64_t elapsedMs, int64_t intervalMs,
                                int64_t lastPeriod, int64_t* outPeriod) {
    if (intervalMs <= 0 || elapsedMs <= 0) return false;
    const int64_t period = elapsedMs / intervalMs;
    if (period < 1 || period <= lastPeriod) return false;
    if (outPeriod) *outPeriod = period;
    return true;
}
