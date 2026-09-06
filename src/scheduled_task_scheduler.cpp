#include "scheduled_task_scheduler.h"

#include "scheduled_task_store.h"

#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace {

std::wstring IntervalSignature(const ScheduledTask& t) {
    return std::to_wstring(static_cast<int>(t.frequency)) + L"|"
        + std::to_wstring(static_cast<int>(t.status)) + L"|"
        + std::to_wstring(t.time.hour) + L":"
        + std::to_wstring(t.time.minute) + L":"
        + std::to_wstring(t.time.second) + L"."
        + std::to_wstring(t.time.millisecond);
}

}  // namespace

void ScheduledTaskScheduler::SetRunCallback(RunCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    runCallback_ = std::move(cb);
}

void ScheduledTaskScheduler::SetTasks(std::vector<ScheduledTask> tasks) {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_ = std::move(tasks);
    PruneLastFireKeysLocked();
    SyncIntervalClocksLocked();
}

void ScheduledTaskScheduler::SetNowMsForTest(unsigned long long nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    useNowMsOverride_ = true;
    nowMsOverride_ = nowMs;
}

unsigned long long ScheduledTaskScheduler::NowMsLocked() const {
    return useNowMsOverride_ ? nowMsOverride_ : GetTickCount64();
}

void ScheduledTaskScheduler::SyncIntervalClocksLocked() {
    // 只保留当前 Interval 任务的运行时时钟：不落盘，进程退出即丢。
    std::unordered_map<std::wstring, IntervalClock> next;
    next.reserve(tasks_.size());
    const unsigned long long nowMs = NowMsLocked();
    for (const auto& task : tasks_) {
        if (task.frequency != ScheduledFrequency::Interval) continue;
        const std::wstring sig = IntervalSignature(task);
        const auto it = intervalClocks_.find(task.id);
        if (it != intervalClocks_.end() && it->second.sig == sig) {
            next.emplace(task.id, it->second);
        } else {
            IntervalClock clock;
            clock.originMs = nowMs;
            clock.lastPeriod = 0;
            clock.sig = sig;
            next.emplace(task.id, std::move(clock));
        }
    }
    intervalClocks_.swap(next);
}

void ScheduledTaskScheduler::PruneLastFireKeysLocked() {
    // 按当前任务 id 收缩，避免删任务后 lastFireKey_ 只增不减。
    for (auto it = lastFireKey_.begin(); it != lastFireKey_.end(); ) {
        bool keep = false;
        for (const auto& task : tasks_) {
            if (task.id == it->first) {
                keep = true;
                break;
            }
        }
        if (keep) ++it;
        else it = lastFireKey_.erase(it);
    }
}

void ScheduledTaskScheduler::TouchIntervalClock(const std::wstring& id) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = intervalClocks_.find(id);
    if (it == intervalClocks_.end()) return;
    it->second.originMs = NowMsLocked();
    it->second.lastPeriod = 0;
}

bool ScheduledTaskScheduler::IntervalDueLocked(const ScheduledTask& task,
                                               unsigned long long nowMs) {
    if (paused_ || globalDisabled_) return false;
    if (task.status == ScheduledTaskStatus::Disabled) return false;
    if (task.filePath.empty()) return false;

    auto it = intervalClocks_.find(task.id);
    if (it == intervalClocks_.end()) {
        IntervalClock clock;
        clock.originMs = nowMs;
        clock.lastPeriod = 0;
        clock.sig = IntervalSignature(task);
        it = intervalClocks_.emplace(task.id, std::move(clock)).first;
        return false;
    }

    const int64_t intervalMs = ScheduledIntervalDurationMs(task.time);
    const int64_t elapsed = (nowMs >= it->second.originMs)
        ? static_cast<int64_t>(nowMs - it->second.originMs) : 0;
    int64_t period = 0;
    if (!ScheduledIntervalPeriodDue(elapsed, intervalMs, it->second.lastPeriod, &period)) {
        return false;
    }
    // 把原点推到本次周期末，elapsed/lastPeriod 不随开机时长单调膨胀。
    const unsigned long long step =
        static_cast<unsigned long long>(period) * static_cast<unsigned long long>(intervalMs);
    if (it->second.originMs + step >= it->second.originMs)
        it->second.originMs += step;
    else
        it->second.originMs = nowMs;
    it->second.lastPeriod = 0;
    return true;
}

std::vector<ScheduledTask>& ScheduledTaskScheduler::Tasks() {
    return tasks_;
}

const std::vector<ScheduledTask>& ScheduledTaskScheduler::Tasks() const {
    return tasks_;
}

void ScheduledTaskScheduler::SetGlobalDisabled(bool disabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (globalDisabled_ && !disabled) intervalClocks_.clear();
    globalDisabled_ = disabled;
    if (!globalDisabled_) SyncIntervalClocksLocked();
}

bool ScheduledTaskScheduler::GlobalDisabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return globalDisabled_;
}

void ScheduledTaskScheduler::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_ && !paused) intervalClocks_.clear();
    paused_ = paused;
    if (!paused_) SyncIntervalClocksLocked();
}

bool ScheduledTaskScheduler::Paused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

void ScheduledTaskScheduler::Reload() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool wasDisabled = globalDisabled_;
    LoadScheduledTasks(tasks_, &globalDisabled_);
    PruneLastFireKeysLocked();
    // 全局禁用期间 elapsed 仍在走；解除时重锚，避免一开闸所有间隔任务同时补火。
    if (wasDisabled && !globalDisabled_) intervalClocks_.clear();
    SyncIntervalClocksLocked();
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
        const unsigned long long nowMs = NowMsLocked();
        for (auto& task : tasks_) {
            const bool due = (task.frequency == ScheduledFrequency::Interval)
                ? IntervalDueLocked(task, nowMs)
                : ScheduledTaskShouldRun(task, now, globalDisabled_, paused_);
            if (!due) continue;
            if (task.frequency != ScheduledFrequency::Interval) {
                const std::wstring key = FireKey(task, now);
                if (lastFireKey_[task.id] == key) continue;
                lastFireKey_[task.id] = key;
            }
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
