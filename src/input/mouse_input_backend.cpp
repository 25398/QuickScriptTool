#include "mouse_input_backend.h"

#include <windows.h>

#include <algorithm>

namespace {

#ifndef MOUSEEVENTF_MOVE_NOCOALESCE
#define MOUSEEVENTF_MOVE_NOCOALESCE 0x2000
#endif

// 轻微迟到（调度抖动）不强行垫间隔，避免把轨迹拉稀。
constexpr uint64_t kLatePaceThresholdUs = 600;

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
    lastWaitLateUs_ = 0;
    if (qpcFreq_.QuadPart == 0) QueryPerformanceFrequency(&qpcFreq_);
}

void MouseInputRouter::SetCatchUpGapUs(uint64_t minGapUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    catchUpGapUs_ = minGapUs;
}

void MouseInputRouter::NoteWaitLatenessUs(uint64_t lateUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastWaitLateUs_ = lateUs;
}

void MouseInputRouter::PaceLocked() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (qpcFreq_.QuadPart <= 0) {
        lastSendInputQpc_ = now.QuadPart;
        return;
    }
    // catchUpGapUs_==0：永不垫间隔，尽量贴绝对时间轴（迟到则连发追赶）。
    if (catchUpGapUs_ > 0 && lastWaitLateUs_ >= kLatePaceThresholdUs
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
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
    const bool ok = SendInput(1, &input, sizeof(input)) == 1;
    ok ? ++stats_.sentEvents : ++stats_.failedEvents;
    if (!ok) lastError_ = L"SendInput relative move failed";
    return ok;
}

bool MouseInputRouter::SendInputMoveBatchLocked(
    const std::vector<std::pair<int, int>>& deltas) {
    if (deltas.empty()) return true;
    // 批量一次提交会挤掉时间信息；迟到追赶时改逐条+限速更稳。
    if (lastWaitLateUs_ >= kLatePaceThresholdUs) {
        bool allOk = true;
        for (const auto& d : deltas) {
            if (!SendInputMoveLocked(d.first, d.second)) allOk = false;
        }
        if (allOk) ++stats_.batchedSubmits;
        return allOk;
    }
    PaceLocked();
    std::vector<INPUT> inputs(deltas.size());
    for (size_t i = 0; i < deltas.size(); ++i) {
        inputs[i].type = INPUT_MOUSE;
        inputs[i].mi.dx = deltas[i].first;
        inputs[i].mi.dy = deltas[i].second;
        inputs[i].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
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
    const bool ok = SendInput(1, &input, sizeof(input)) == 1;
    ok ? ++stats_.sentEvents : ++stats_.failedEvents;
    if (!ok) lastError_ = L"SendInput mouse button failed";
    return ok;
}

bool MouseInputRouter::SendInputWheelLocked(int delta, bool horizontal) {
    PaceLocked();
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);
    const bool ok = SendInput(1, &input, sizeof(input)) == 1;
    ok ? ++stats_.sentEvents : ++stats_.failedEvents;
    if (!ok) lastError_ = L"SendInput wheel failed";
    return ok;
}

bool MouseInputRouter::MoveRelative(int dx, int dy) {
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
