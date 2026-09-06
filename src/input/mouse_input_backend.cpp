#include "mouse_input_backend.h"
#include "mouse_rel_split.h"
#include "foreground_input_router.h"
#include "synthetic_input_filter.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace {

#ifndef MOUSEEVENTF_MOVE_NOCOALESCE
#define MOUSEEVENTF_MOVE_NOCOALESCE 0x2000
#endif

// 轻微迟到（调度抖动）不强行垫间隔，避免把轨迹拉稀。
constexpr uint64_t kLatePaceThresholdUs = 600;

// 默认第一加速阈值约 6；取 4 保证未关加速时也不会被加倍。
constexpr int kSubThresholdMaxStep = 4;

// 单步相对位移上限：防止畸形脚本/损坏录制给 INT_MIN 级增量，
// 导致亚阈值拆分产生数百万次 SendInput 的系统 DoS。
constexpr int kMaxSingleMoveDelta = 32767;

int ClampDelta(int v) {
    if (v > kMaxSingleMoveDelta) return kMaxSingleMoveDelta;
    if (v < -kMaxSingleMoveDelta) return -kMaxSingleMoveDelta;
    return v;
}

void FillMoveInput(INPUT& input, int dx, int dy) {
    input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
    input.mi.dwExtraInfo = synthetic_input::kSyntheticExtraInfo;
}

}  // namespace

MouseInputRouter& MouseInputRouter::Instance() {
    static MouseInputRouter router;
    return router;
}

void MouseInputRouter::Configure() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = {};
    lastError_.clear();
    lastSendInputQpc_ = 0;
    lastWaitLateUs_.store(0, std::memory_order_relaxed);
    splitLargeMoves_ = true;
    if (qpcFreq_.QuadPart == 0) QueryPerformanceFrequency(&qpcFreq_);
}

void MouseInputRouter::ResetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = {};
    lastError_.clear();
    lastSendInputQpc_ = 0;
    lastWaitLateUs_.store(0, std::memory_order_relaxed);
}

void MouseInputRouter::SetSplitLargeMoves(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    splitLargeMoves_ = enable;
}

void MouseInputRouter::SetCatchUpGapUs(uint64_t minGapUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    catchUpGapUs_ = minGapUs;
}

void MouseInputRouter::NoteWaitLatenessUs(uint64_t lateUs) {
    lastWaitLateUs_.store(lateUs, std::memory_order_relaxed);
}

void MouseInputRouter::PaceLocked() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (qpcFreq_.QuadPart <= 0) {
        lastSendInputQpc_ = now.QuadPart;
        return;
    }
    const uint64_t waitLateUs =
        lastWaitLateUs_.load(std::memory_order_relaxed);
    // catchUpGapUs_==0：永不垫间隔，尽量贴绝对时间轴（迟到则连发追赶）。
    if (catchUpGapUs_ > 0 && waitLateUs >= kLatePaceThresholdUs
        && lastSendInputQpc_ != 0) {
        const int64_t minGapQpc = static_cast<int64_t>(
            (static_cast<long double>(catchUpGapUs_) * qpcFreq_.QuadPart)
            / 1000000.0L);
        const int64_t earliest = lastSendInputQpc_ + minGapQpc;
        if (now.QuadPart < earliest) {
            ++stats_.pacedWaits;
            while (now.QuadPart < earliest) {
                YieldProcessor();
                QueryPerformanceCounter(&now);
            }
        }
    }
    lastSendInputQpc_ = now.QuadPart;
}

bool MouseInputRouter::SendInputMoveLocked(int dx, int dy) {
    PaceLocked();
    if (ForegroundInputRouter::Instance().IsHidActive()) {
        const bool needSplit = splitLargeMoves_
            && (std::abs(dx) > kSubThresholdMaxStep
                || std::abs(dy) > kSubThresholdMaxStep);
        if (!needSplit) {
            const bool ok = ForegroundInputRouter::Instance().MoveRelative(dx, dy);
            ok ? ++stats_.sentEvents : ++stats_.failedEvents;
            if (!ok) lastError_ = L"HID relative move failed";
            return ok;
        }
        std::vector<std::pair<int, int>> steps;
        AppendSubThresholdRelativeSteps(dx, dy, kSubThresholdMaxStep, steps);
        bool allOk = true;
        for (const auto& s : steps) {
            if (!ForegroundInputRouter::Instance().MoveRelative(s.first, s.second)) {
                allOk = false;
            } else {
                ++stats_.sentEvents;
            }
        }
        if (!allOk) {
            ++stats_.failedEvents;
            lastError_ = L"HID relative move failed";
        }
        return allOk;
    }
    // 加速已关：按 Raw 原包一次注入，避免拆成多次报告改变游戏滤波。
    // 加速未关：大包拆成亚阈值步进，防止阈值加倍。
    const bool needSplit = splitLargeMoves_
        && (std::abs(dx) > kSubThresholdMaxStep
            || std::abs(dy) > kSubThresholdMaxStep);
    if (!needSplit) {
        INPUT input{};
        FillMoveInput(input, dx, dy);
        const bool ok = SendInput(1, &input, sizeof(input)) == 1;
        ok ? ++stats_.sentEvents : ++stats_.failedEvents;
        if (!ok) lastError_ = L"SendInput relative move failed";
        return ok;
    }
    std::vector<std::pair<int, int>> steps;
    steps.reserve(static_cast<size_t>(
        (std::abs(dx) + kSubThresholdMaxStep - 1) / kSubThresholdMaxStep
        + (std::abs(dy) + kSubThresholdMaxStep - 1) / kSubThresholdMaxStep));
    AppendSubThresholdRelativeSteps(dx, dy, kSubThresholdMaxStep, steps);
    std::vector<INPUT> inputs(steps.size());
    for (size_t i = 0; i < steps.size(); ++i) {
        FillMoveInput(inputs[i], steps[i].first, steps[i].second);
    }
    const UINT sent = SendInput(static_cast<UINT>(inputs.size()),
        inputs.data(), sizeof(INPUT));
    const bool ok = sent == inputs.size();
    if (ok) {
        stats_.sentEvents += sent;
    } else {
        stats_.failedEvents += inputs.size() - sent;
        lastError_ = L"SendInput relative move failed";
    }
    return ok;
}

bool MouseInputRouter::SendInputMoveBatchLocked(
    const std::vector<std::pair<int, int>>& deltas) {
    if (deltas.empty()) return true;
    // 单步位移上限：畸形脚本的 INT_MIN 增量会在拆分阶段放大成 DoS。
    std::vector<std::pair<int, int>> clamped;
    clamped.reserve(deltas.size());
    for (const auto& d : deltas) {
        const int cx = ClampDelta(d.first);
        const int cy = ClampDelta(d.second);
        if (cx != 0 || cy != 0) clamped.emplace_back(cx, cy);
    }
    if (clamped.empty()) return true;
    // 批量一次提交会挤掉时间信息；迟到追赶时改逐条+限速更稳。
    if (lastWaitLateUs_ >= kLatePaceThresholdUs) {
        bool allOk = true;
        for (const auto& d : clamped) {
            if (!SendInputMoveLocked(d.first, d.second)) allOk = false;
        }
        if (allOk) ++stats_.batchedSubmits;
        return allOk;
    }
    PaceLocked();
    if (ForegroundInputRouter::Instance().IsHidActive()) {
        std::vector<std::pair<int, int>> steps;
        if (splitLargeMoves_) {
            steps.reserve(clamped.size() * 2);
            for (const auto& d : clamped) {
                AppendSubThresholdRelativeSteps(
                    d.first, d.second, kSubThresholdMaxStep, steps);
            }
        } else {
            steps = clamped;
        }
        bool allOk = true;
        for (const auto& s : steps) {
            if (!ForegroundInputRouter::Instance().MoveRelative(s.first, s.second)) {
                allOk = false;
            } else {
                ++stats_.sentEvents;
            }
        }
        if (allOk) ++stats_.batchedSubmits;
        else {
            ++stats_.failedEvents;
            lastError_ = L"HID relative batch failed";
        }
        return allOk;
    }
    std::vector<std::pair<int, int>> steps;
    if (splitLargeMoves_) {
        steps.reserve(clamped.size() * 2);
        for (const auto& d : clamped) {
            AppendSubThresholdRelativeSteps(
                d.first, d.second, kSubThresholdMaxStep, steps);
        }
    } else {
        steps = clamped;
    }
    if (steps.empty()) return true;
    std::vector<INPUT> inputs(steps.size());
    for (size_t i = 0; i < steps.size(); ++i) {
        FillMoveInput(inputs[i], steps[i].first, steps[i].second);
    }
    const UINT sent = SendInput(static_cast<UINT>(inputs.size()),
        inputs.data(), sizeof(INPUT));
    const bool ok = sent == inputs.size();
    if (ok) {
        stats_.sentEvents += sent;
        ++stats_.batchedSubmits;
    } else {
        stats_.failedEvents += inputs.size() - sent;
        lastError_ = L"SendInput relative batch failed";
    }
    return ok;
}

bool MouseInputRouter::SendInputButtonLocked(MouseButtonType button, bool down) {
    PaceLocked();
    if (ForegroundInputRouter::Instance().IsHidActive()) {
        const bool ok = ForegroundInputRouter::Instance().Button(button, down);
        ok ? ++stats_.sentEvents : ++stats_.failedEvents;
        if (!ok) lastError_ = L"HID mouse button failed";
        return ok;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    switch (button) {
    case MouseButtonType::Right:
        input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case MouseButtonType::Middle:
        input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case MouseButtonType::X1:
        input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON1; break;
    case MouseButtonType::X2:
        input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON2; break;
    case MouseButtonType::Left:
    default:
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    }
    input.mi.dwExtraInfo = synthetic_input::kSyntheticExtraInfo;
    const bool ok = SendInput(1, &input, sizeof(input)) == 1;
    ok ? ++stats_.sentEvents : ++stats_.failedEvents;
    if (!ok) lastError_ = L"SendInput mouse button failed";
    return ok;
}

bool MouseInputRouter::SendInputWheelLocked(int delta, bool horizontal) {
    PaceLocked();
    if (ForegroundInputRouter::Instance().IsHidActive()) {
        const bool ok = ForegroundInputRouter::Instance().Wheel(delta, horizontal);
        ok ? ++stats_.sentEvents : ++stats_.failedEvents;
        if (!ok) lastError_ = L"HID wheel failed";
        return ok;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);
    input.mi.dwExtraInfo = synthetic_input::kSyntheticExtraInfo;
    const bool ok = SendInput(1, &input, sizeof(input)) == 1;
    ok ? ++stats_.sentEvents : ++stats_.failedEvents;
    if (!ok) lastError_ = L"SendInput wheel failed";
    return ok;
}

bool MouseInputRouter::MoveRelative(int dx, int dy) {
    dx = ClampDelta(dx);
    dy = ClampDelta(dy);
    if (dx == 0 && dy == 0) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    return SendInputMoveLocked(dx, dy);
}

bool MouseInputRouter::MoveRelativeBatch(
    const std::vector<std::pair<int, int>>& deltas) {
    if (deltas.empty()) return true;
    if (deltas.size() == 1) return MoveRelative(deltas[0].first, deltas[0].second);

    std::vector<std::pair<int, int>> filtered;
    filtered.reserve(deltas.size());
    for (const auto& d : deltas) {
        if (d.first != 0 || d.second != 0) filtered.push_back(d);
    }
    if (filtered.empty()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    return SendInputMoveBatchLocked(filtered);
}

bool MouseInputRouter::Button(MouseButtonType button, bool down) {
    std::lock_guard<std::mutex> lock(mutex_);
    return SendInputButtonLocked(button, down);
}

bool MouseInputRouter::Wheel(int delta, bool horizontal) {
    std::lock_guard<std::mutex> lock(mutex_);
    return SendInputWheelLocked(delta, horizontal);
}

std::wstring MouseInputRouter::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

MouseBackendStats MouseInputRouter::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}
