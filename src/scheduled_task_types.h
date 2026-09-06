#pragma once
// ──────────────────────────────────────────────────────────────────
// scheduled_task_types.h — 定时任务数据结构与辅助函数
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <cstdint>
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
    Interval = 4,  // 从启动/保存起累计时长，到期执行，关闭软件后重置
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
    /// 专业模式逻辑目录（相对 library/sched，空=未分类）
    std::wstring folder;
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

inline ScheduledFrequency ClampScheduledFrequency(int v) {
    if (v < 0 || v > static_cast<int>(ScheduledFrequency::Interval))
        return ScheduledFrequency::Custom;
    return static_cast<ScheduledFrequency>(v);
}

/// 间隔时长（时/分/秒/毫秒按时长累加，不是钟点）。≤0 表示无效。
int64_t ScheduledIntervalDurationMs(const ScheduledTaskTime& t);

/// elapsed 达到 1x/2x/… 间隔则到期；漏 tick 只报当前周期一次（不连打）。
bool ScheduledIntervalPeriodDue(int64_t elapsedMs, int64_t intervalMs,
                                int64_t lastPeriod, int64_t* outPeriod);

/// 定时任务与正在执行脚本冲突时的策略（设置「定时任务优先级」）。
enum class ScheduledTaskConflictPolicy {
    RunningScriptFirst = 0,   // 执行脚本优先：忙则跳过本次定时
    ScheduledScriptFirst = 1, // 定时脚本优先：打断当前（非定时）脚本再跑定时
};

enum class ScheduledTaskFireAction {
    RunNow = 0,
    Skip = 1,
    InterruptAndQueue = 2, // 停掉当前脚本再跑定时，不恢复
    Queue = 3,             // 等当前脚本结束后再跑定时
    YieldAndResume = 4,    // 同线程插入跑定时，再从原步骤继续
};

inline ScheduledTaskConflictPolicy ClampScheduledTaskConflictPolicy(int v) {
    if (v < 0 || v > 1) return ScheduledTaskConflictPolicy::RunningScriptFirst;
    return static_cast<ScheduledTaskConflictPolicy>(v);
}

inline const wchar_t* ScheduledTaskConflictPolicyLabel(ScheduledTaskConflictPolicy p) {
    switch (p) {
    case ScheduledTaskConflictPolicy::ScheduledScriptFirst: return L"定时脚本优先";
    case ScheduledTaskConflictPolicy::RunningScriptFirst:
    default: return L"执行脚本优先";
    }
}

/// scriptRunning：引擎正在跑脚本；runningIsScheduled：当前这次已是定时拉起的（不再打断）。
/// autoResume：「脚本中断后自动恢复」勾选。
inline ScheduledTaskFireAction DecideScheduledTaskFire(
    ScheduledTaskConflictPolicy policy, bool scriptRunning,
    bool runningIsScheduled = false, bool autoResume = false) {
    if (!scriptRunning) return ScheduledTaskFireAction::RunNow;
    if (policy == ScheduledTaskConflictPolicy::ScheduledScriptFirst) {
        if (runningIsScheduled) return ScheduledTaskFireAction::Skip;
        return autoResume ? ScheduledTaskFireAction::YieldAndResume
                          : ScheduledTaskFireAction::InterruptAndQueue;
    }
    // 执行脚本优先
    if (autoResume) return ScheduledTaskFireAction::Queue;
    return ScheduledTaskFireAction::Skip;
}

/// 「结束宏运行」是否结束整条工作线程。
/// 运行宏（RunMacro）嵌套：被调脚本的 stopMacro 应结束调用方 → true（depth==0）。
/// 定时插入：被中断的原脚本不是调用方，定时任务独立 → false（depth>0），只结束这次定时。
/// 热键/紧急停止仍走全局 stopFlag，不走此函数。
inline bool StopMacroShouldEndEntireRun(int scheduledYieldDepth) {
    return scheduledYieldDepth <= 0;
}
