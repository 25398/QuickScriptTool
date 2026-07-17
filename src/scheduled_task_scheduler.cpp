#include "scheduled_task_scheduler.h"

#include "scheduled_task_store.h"

#include <cstdio>
#include <vector>

void ScheduledTaskScheduler::SetRunCallback(RunCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    runCallback_ = std::move(cb);
}

void ScheduledTaskScheduler::SetTasks(std::vector<ScheduledTask> tasks) {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_ = std::move(tasks);
}

std::vector<ScheduledTask>& ScheduledTaskScheduler::Tasks() {
    return tasks_;
}

const std::vector<ScheduledTask>& ScheduledTaskScheduler::Tasks() const {
    return tasks_;
}

void ScheduledTaskScheduler::SetGlobalDisabled(bool disabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    globalDisabled_ = disabled;
}

bool ScheduledTaskScheduler::GlobalDisabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return globalDisabled_;
}

void ScheduledTaskScheduler::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
}

bool ScheduledTaskScheduler::Paused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

void ScheduledTaskScheduler::Reload() {
    std::lock_guard<std::mutex> lock(mutex_);
    LoadScheduledTasks(tasks_, &globalDisabled_);
    lastFireKey_.clear();
}

bool ScheduledTaskScheduler::Save() {
    std::lock_guard<std::mutex> lock(mutex_);
    return SaveScheduledTasks(tasks_, globalDisabled_);
}

std::wstring ScheduledTaskScheduler::FireKey(const ScheduledTask& task, const SYSTEMTIME& st) const {
    wchar_t buf[128]{};
    // 秒级去重：与 TimeMatches（忽略毫秒）一致，避免同秒内重复开火。
    swprintf_s(buf, L"%s-%04d%02d%02d%02d%02d%02d",
        task.id.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void ScheduledTaskScheduler::Tick() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    TickAt(now);
}

void ScheduledTaskScheduler::TickAt(const SYSTEMTIME& now) {
    RunCallback callback;
    std::vector<std::wstring> pathsToRun;
    bool needSave = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (paused_ || !runCallback_) return;

        callback = runCallback_;
        for (auto& task : tasks_) {
            if (!ScheduledTaskShouldRun(task, now, globalDisabled_, paused_)) continue;
            const std::wstring key = FireKey(task, now);
            if (lastFireKey_[task.id] == key) continue;
            lastFireKey_[task.id] = key;
            if (!task.filePath.empty()) pathsToRun.push_back(task.filePath);
            if (task.frequency == ScheduledFrequency::Custom) {
                task.customFired = true;
                needSave = true;
            }
        }
    }

    if (needSave) Save();
    if (!callback) return;
    for (const auto& path : pathsToRun) {
        if (!path.empty()) callback(path);
    }
}
