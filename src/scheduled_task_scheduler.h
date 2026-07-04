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

private:
    std::wstring FireKey(const ScheduledTask& task, const SYSTEMTIME& st) const;

    mutable std::mutex mutex_;
    std::vector<ScheduledTask> tasks_;
    bool globalDisabled_ = false;
    bool paused_ = false;
    RunCallback runCallback_;
    std::unordered_map<std::wstring, std::wstring> lastFireKey_;
};
