#pragma once

#include "script_types.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct MouseBackendStats {
    uint64_t sentEvents = 0;
    uint64_t failedEvents = 0;
    uint64_t pacedWaits = 0;
    uint64_t batchedSubmits = 0;
    /// 实际注入的相对位移总量（回放保真度诊断：与录制脚本里的总量对比，
    /// 相等说明输入层忠实，落点偏差来自目标侧；不等说明注入层改动了位移）。
    long long movedDx = 0;
    long long movedDy = 0;
};

/// SendInput 相对鼠标回放（关加速 + 必要时亚阈值拆分，贴近 Raw 计数）。
class MouseInputRouter {
public:
    static MouseInputRouter& Instance();

    void Configure();
    /// 仅清零计数（多轮回放时每轮统计独立）。
    void ResetStats();
    /// 大包是否拆成亚阈值步进（仅加速未真正关闭时需要；拆包会改变游戏收到的报告次数）。
    void SetSplitLargeMoves(bool enable);
    /// 仅在时间轴迟到时限制突发；准点时不垫间隔。
    void SetCatchUpGapUs(uint64_t minGapUs);
    void NoteWaitLatenessUs(uint64_t lateUs);
    bool MoveRelative(int dx, int dy);
    bool MoveRelativeBatch(const std::vector<std::pair<int, int>>& deltas);
    bool Button(MouseButtonType button, bool down);
    bool Wheel(int delta, bool horizontal);
    std::wstring LastError() const;
    MouseBackendStats Stats() const;

private:
    /// 记录一次「已成功发出」的相对位移（拆分后按步累加，总和与请求一致）。
    void NoteMoveSentLocked(int dx, int dy) {
        stats_.movedDx += dx;
        stats_.movedDy += dy;
    }
    void PaceLocked();
    bool SendInputMoveLocked(int dx, int dy);
    bool SendInputMoveBatchLocked(const std::vector<std::pair<int, int>>& deltas);
    bool SendInputButtonLocked(MouseButtonType button, bool down);
    bool SendInputWheelLocked(int delta, bool horizontal);

    mutable std::mutex mutex_;
    std::wstring lastError_;
    MouseBackendStats stats_{};
    LARGE_INTEGER qpcFreq_{};
    int64_t lastSendInputQpc_ = 0;
    uint64_t catchUpGapUs_ = 250;
    bool splitLargeMoves_ = true;
    /// 由 Wait 热路径写入；Pace 只读，避免每次等待抢同一把锁。
    std::atomic<uint64_t> lastWaitLateUs_{0};
};
