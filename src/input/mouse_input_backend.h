#pragma once

#include "script_types.h"

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
};

/// SendInput 相对鼠标回放（关闭加速后 1:1 mickey）。
class MouseInputRouter {
public:
    static MouseInputRouter& Instance();

    void Configure();
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
    uint64_t lastWaitLateUs_ = 0;
};
