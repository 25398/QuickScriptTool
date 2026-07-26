#include "input_timeline_scheduler.h"

#include <algorithm>
#include <cmath>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {
// FPS 相对包常见 ~8ms：整段自旋，避免 waitable timer 唤醒抖动导致每次回放相位不同。
constexpr uint64_t kSpinRemainUs = 12000;
constexpr uint64_t kTightSpinUs = 1500;
constexpr uint64_t kMaxTimerSliceUs = 500;
}

PrecisionInputTimeline::PrecisionInputTimeline() {
    QueryPerformanceFrequency(&frequency_);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
}

PrecisionInputTimeline::~PrecisionInputTimeline() {
    if (timer_) CloseHandle(timer_);
}

int64_t PrecisionInputTimeline::NowQpc() const {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

uint64_t PrecisionInputTimeline::QpcDeltaToUs(int64_t delta) const {
    if (delta <= 0 || frequency_.QuadPart <= 0) return 0;
    return static_cast<uint64_t>(
        (static_cast<long double>(delta) * 1000000.0L) / frequency_.QuadPart);
}

int64_t PrecisionInputTimeline::UsToQpcDelta(uint64_t us) const {
    if (frequency_.QuadPart <= 0) return 0;
    return static_cast<int64_t>(
        (static_cast<long double>(us) * frequency_.QuadPart) / 1000000.0L);
}

void PrecisionInputTimeline::Reset() {
    originQpc_ = NowQpc();
    elapsedUs_ = 0;
    lastLatenessUs_ = 0;
    latenessUs_.clear();
}

void PrecisionInputTimeline::RecordLateness(int64_t deadlineQpc) {
    lastLatenessUs_ = QpcDeltaToUs(NowQpc() - deadlineQpc);
    latenessUs_.push_back(lastLatenessUs_);
}

bool PrecisionInputTimeline::WaitUntilDeadlineQpc(
    int64_t deadlineQpc, const std::function<bool()>& cancelled) {
    for (;;) {
        if (cancelled()) return false;
        const int64_t now = NowQpc();
        if (now >= deadlineQpc) break;
        const uint64_t remainingUs = QpcDeltaToUs(deadlineQpc - now);

        if (timer_ && remainingUs > kSpinRemainUs) {
            const uint64_t sliceUs = std::min<uint64_t>(
                remainingUs - (kSpinRemainUs / 2), kMaxTimerSliceUs);
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(sliceUs * 10);
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer_,
                    static_cast<DWORD>((sliceUs + 999) / 1000 + 1));
                continue;
            }
        }

        if (remainingUs <= kTightSpinUs) {
            for (;;) {
                if (cancelled()) return false;
                if (NowQpc() >= deadlineQpc) {
                    RecordLateness(deadlineQpc);
                    return true;
                }
                YieldProcessor();
            }
        }

        for (int i = 0; i < 32; ++i) {
            if ((i & 7) == 0 && cancelled()) return false;
            if (NowQpc() >= deadlineQpc) {
                RecordLateness(deadlineQpc);
                return true;
            }
            YieldProcessor();
        }
    }
    RecordLateness(deadlineQpc);
    return true;
}

bool PrecisionInputTimeline::WaitUntilElapsedUs(
    uint64_t targetElapsedUs, const std::function<bool()>& cancelled) {
    if (originQpc_ == 0) Reset();
    if (targetElapsedUs > elapsedUs_) elapsedUs_ = targetElapsedUs;
    const int64_t deadline = originQpc_ + UsToQpcDelta(elapsedUs_);
    return WaitUntilDeadlineQpc(deadline, cancelled);
}

bool PrecisionInputTimeline::WaitDeltaUs(
    uint64_t deltaUs, const std::function<bool()>& cancelled) {
    if (deltaUs == 0) return !cancelled();
    if (originQpc_ == 0) Reset();
    elapsedUs_ += deltaUs;
    const int64_t deadline = originQpc_ + UsToQpcDelta(elapsedUs_);
    const int64_t now = NowQpc();
    if (now < deadline)
        return WaitUntilDeadlineQpc(deadline, cancelled);

    // 已过点：立刻追赶，不拉伸原点。
    // 拉伸会让后续键鼠相对「开局时刻」永久偏相，FPS 开环下比偶发连发更伤还原。
    lastLatenessUs_ = QpcDeltaToUs(now - deadline);
    latenessUs_.push_back(lastLatenessUs_);
    return !cancelled();
}

bool PrecisionInputTimeline::WaitGapUs(
    uint64_t deltaUs, const std::function<bool()>& cancelled) {
    if (deltaUs == 0) return !cancelled();
    // 从此刻起睡满间隔：不追赶、不压缩，相对视角节奏与录制一致。
    const int64_t deadline = NowQpc() + UsToQpcDelta(deltaUs);
    elapsedUs_ += deltaUs;
    return WaitUntilDeadlineQpc(deadline, cancelled);
}

bool PrecisionInputTimeline::WaitDeltaSeconds(
    double seconds, const std::function<bool()>& cancelled) {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) return !cancelled();
    return WaitDeltaUs(
        static_cast<uint64_t>(std::llround(seconds * 1000000.0)), cancelled);
}

InputTimelineStats PrecisionInputTimeline::Stats() const {
    InputTimelineStats out{};
    out.eventCount = latenessUs_.size();
    if (latenessUs_.empty()) return out;
    std::vector<uint64_t> sorted = latenessUs_;
    std::sort(sorted.begin(), sorted.end());
    out.maxLateUs = sorted.back();
    out.p95LateUs = sorted[(sorted.size() - 1) * 95 / 100];
    out.p99LateUs = sorted[(sorted.size() - 1) * 99 / 100];
    out.lateEventCount = static_cast<uint64_t>(
        std::count_if(sorted.begin(), sorted.end(),
            [](uint64_t us) { return us > 1000; }));
    return out;
}
