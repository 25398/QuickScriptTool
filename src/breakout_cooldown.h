#pragma once
// ──────────────────────────────────────────────────────────────────
// breakout_cooldown.h — 脱离冷却策略（可单测）
// 按住键/按钮 = 持续交互：不开始、不推进倒计时；松开后才计 idle。
// ──────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <windows.h>

namespace breakout_input {

struct BreakoutHoldTracker {
    void NoteDown(UINT vk) {
        if (vk == 0) return;
        std::lock_guard<std::mutex> lock(mu_);
        held_.insert(vk);
        count_.store(static_cast<int>(held_.size()), std::memory_order_relaxed);
    }

    void NoteUp(UINT vk) {
        if (vk == 0) return;
        std::lock_guard<std::mutex> lock(mu_);
        held_.erase(vk);
        count_.store(static_cast<int>(held_.size()), std::memory_order_relaxed);
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mu_);
        held_.clear();
        count_.store(0, std::memory_order_relaxed);
    }

    bool Holding() const {
        return count_.load(std::memory_order_relaxed) > 0;
    }

    int Count() const {
        return count_.load(std::memory_order_relaxed);
    }

    template <typename IsDownFn>
    void Reconcile(IsDownFn&& isDown) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = held_.begin(); it != held_.end(); ) {
            if (!isDown(*it)) it = held_.erase(it);
            else ++it;
        }
        count_.store(static_cast<int>(held_.size()), std::memory_order_relaxed);
    }

private:
    mutable std::mutex mu_;
    std::unordered_set<UINT> held_;
    std::atomic<int> count_{0};
};

struct BreakoutCooldownState {
    bool armed = false;
    std::chrono::steady_clock::time_point deadline{};
};

/// 按住：冻结冷却。新输入：重置。空闲满 idle 后返回 false（可以恢复脚本）。
inline bool BreakoutCooldownStillWaiting(
    bool userHolding,
    bool freshUserInput,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds idleDuration,
    BreakoutCooldownState& state)
{
    if (userHolding) {
        state.armed = false;
        return true;
    }
    if (freshUserInput) {
        state.armed = false;
    }
    if (!state.armed) {
        state.deadline = now + idleDuration;
        state.armed = true;
    }
    return now < state.deadline;
}

}  // namespace breakout_input
