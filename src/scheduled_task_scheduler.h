#pragma once
// ──────────────────────────────────────────────────────────────────
// scheduled_task_scheduler.h — 定时任务调度器
// ──────────────────────────────────────────────────────────────────

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "scheduled_task_types.h"

class ScheduledTaskScheduler {
public:
    using RunCallback = std::function<void(const std::wstring& filePath)>;

    void SetRunCallback(RunCallback cb);
    void SetTasks(std::vector<ScheduledTask> tasks);
    std::vector<ScheduledTask>& Tasks();
    const std::vector<ScheduledTask>& Tasks() const;

    void SetGlobalDisabled(bool disabled);
    bool GlobalDisabled() const;
    void SetPaused(bool paused);
    bool Paused() const;

    void Reload();
    bool Save();
    void Tick();
    /// 自检 / 可注入时钟：按给定本地时间评估到期任务并触发回调。
    void TickAt(const SYSTEMTIME& now);
    /// 自检：注入单调时钟（毫秒）。须在 SetTasks/Reload 之前设置，作为间隔原点。
    void SetNowMsForTest(unsigned long long nowMs);
    /// 该间隔任务刚被保存/启用：把累计原点重锚到现在（不写盘）。
    void TouchIntervalClock(const std::wstring& id);

private:
    struct IntervalClock {
        unsigned long long originMs = 0;
        int64_t lastPeriod = 0;
        std::wstring sig;
    };

    std::wstring FireKey(const ScheduledTask& task, const SYSTEMTIME& st) const;
    unsigned long long NowMsLocked() const;
    void SyncIntervalClocksLocked();
    void PruneLastFireKeysLocked();
    bool IntervalDueLocked(const ScheduledTask& task, unsigned long long nowMs);

    mutable std::mutex mutex_;
    std::vector<ScheduledTask> tasks_;
    bool globalDisabled_ = false;
    bool paused_ = false;
    RunCallback runCallback_;
    std::unordered_map<std::wstring, std::wstring> lastFireKey_;
    std::unordered_map<std::wstring, IntervalClock> intervalClocks_;
    bool useNowMsOverride_ = false;
    unsigned long long nowMsOverride_ = 0;
};
