#include "scheduled_task_scheduler.h"

#include "scheduled_task_store.h"

#include <cstdio>

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
    swprintf_s(buf, L"%s-%04d%02d%02d%02d%02d%02d%03d",
        task.id.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void ScheduledTaskScheduler::Tick() {
    RunCallback callback;
    std::wstring pathToRun;
    bool needSave = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (paused_ || !runCallback_) return;

        SYSTEMTIME now{};
        GetLocalTime(&now);

        for (auto& task : tasks_) {
            if (!ScheduledTaskShouldRun(task, now, globalDisabled_, paused_)) continue;
            const std::wstring key = FireKey(task, now);
            if (lastFireKey_[task.id] == key) continue;
            lastFireKey_[task.id] = key;
            pathToRun = task.filePath;
            callback = runCallback_;
            if (task.frequency == ScheduledFrequency::Custom) {
                task.customFired = true;
                needSave = true;
            }
            break;
        }
    }

    if (needSave) Save();
    if (callback && !pathToRun.empty()) callback(pathToRun);
}
