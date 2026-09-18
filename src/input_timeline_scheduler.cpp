#include "input_timeline_scheduler.h"

#include "low_power_mode.h"

#include <algorithm>
#include <cmath>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {
// FPS 相对包常见 ~8ms：整段自旋，避免 waitable timer 唤醒抖动导致每次回放相位不同。
constexpr uint64_t kSpinRemainUs = 12000;
// ★低性能模式：把自旋压到 ~800us，其余交给高精度 waitable timer
//（Win10 1803+ 实测唤醒精度约 0.5ms）。代价是注入节奏可能出现亚毫秒级抖动，
// 换来的是回放期间不再长时间占满一个核 —— 用户实测「挂机脚本 CPU 温度 80°C」。
constexpr uint64_t kLowPowerSpinRemainUs = 800;
constexpr uint64_t kTightSpinUs = 1500;
constexpr uint64_t kLowPowerTightSpinUs = 300;
// 收尾接近 deadline 时用短切片；长等待若整段 500us 切片，4 秒会进内核近万次，自己把时间轴卡变形。
constexpr uint64_t kNearTimerSliceUs = 500;
constexpr uint64_t kLongTimerSliceUs = 10000;
constexpr uint64_t kLongWaitRemainUs = 50000;
// 小于此值：调度抖动，追赶（保持相对包相位）。
// 大于此值：真实卡顿。若仍追赶，后面所有 2~8ms 等待会连发，键盘按住被压短。
constexpr uint64_t kRebaseLateUs = 8000;
constexpr uint64_t kRebaseKeepLateUs = 500;

/// 低性能模式开关见 low_power_mode.h（进程级原子量，设置保存后立即生效）
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

void PrecisionInputTimeline::Reset(size_t waitHint) {
    originQpc_ = NowQpc();
    elapsedUs_ = 0;
    lastLatenessUs_ = 0;
    rebaseCount_ = 0;
    latenessUs_.clear();
    const size_t want = (waitHint > 8192) ? waitHint : 8192;
    if (latenessUs_.capacity() < want) latenessUs_.reserve(want);
}

void PrecisionInputTimeline::RecordLateness(int64_t deadlineQpc) {
    lastLatenessUs_ = QpcDeltaToUs(NowQpc() - deadlineQpc);
    latenessUs_.push_back(lastLatenessUs_);
}

void PrecisionInputTimeline::RebaseIfVeryLate() {
    if (lastLatenessUs_ <= kRebaseLateUs) return;
    const uint64_t absorb = lastLatenessUs_ - kRebaseKeepLateUs;
    const int64_t shift = UsToQpcDelta(absorb);
    if (shift > 0) {
        originQpc_ += shift;
        ++rebaseCount_;
    }
}

bool PrecisionInputTimeline::WaitUntilDeadlineQpc(
    int64_t deadlineQpc, const std::function<bool()>& cancelled) {
    const bool lowPower = LowPerformanceMode();
    const uint64_t spinRemainUs = lowPower ? kLowPowerSpinRemainUs : kSpinRemainUs;
    const uint64_t tightSpinUs = lowPower ? kLowPowerTightSpinUs : kTightSpinUs;
    for (;;) {
        if (cancelled()) return false;
        const int64_t now = NowQpc();
        if (now >= deadlineQpc) break;
        const uint64_t remainingUs = QpcDeltaToUs(deadlineQpc - now);

        if (timer_ && remainingUs > spinRemainUs) {
            const uint64_t maxSlice = remainingUs > kLongWaitRemainUs
                ? kLongTimerSliceUs : kNearTimerSliceUs;
            const uint64_t sliceUs = std::min<uint64_t>(
                remainingUs - (spinRemainUs / 2), maxSlice);
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(sliceUs * 10);
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer_,
                    static_cast<DWORD>((sliceUs + 999) / 1000 + 1));
                continue;
            }
        }

        if (remainingUs <= tightSpinUs) {
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
        // 12ms 自旋不得占死核：否则 UI/LL 钩子饿死，停止热键失灵、键鼠假死。
        SwitchToThread();
    }
    RecordLateness(deadlineQpc);
    return true;
}

bool PrecisionInputTimeline::WaitUntilElapsedUs(
    uint64_t targetElapsedUs, const std::function<bool()>& cancelled) {
    if (originQpc_ == 0) Reset();
    if (targetElapsedUs > elapsedUs_) elapsedUs_ = targetElapsedUs;
    const int64_t deadline = originQpc_ + UsToQpcDelta(elapsedUs_);
    const bool ok = WaitUntilDeadlineQpc(deadline, cancelled);
    if (ok) RebaseIfVeryLate();
    return ok;
}

bool PrecisionInputTimeline::WaitDeltaUs(
    uint64_t deltaUs, const std::function<bool()>& cancelled) {
    if (deltaUs == 0) return !cancelled();
    if (originQpc_ == 0) Reset();
    elapsedUs_ += deltaUs;
    const int64_t deadline = originQpc_ + UsToQpcDelta(elapsedUs_);
    const int64_t now = NowQpc();
    if (now < deadline) {
        const bool ok = WaitUntilDeadlineQpc(deadline, cancelled);
        if (ok) RebaseIfVeryLate();
        return ok;
    }

    lastLatenessUs_ = QpcDeltaToUs(now - deadline);
    latenessUs_.push_back(lastLatenessUs_);
    RebaseIfVeryLate();
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
    out.rebaseCount = rebaseCount_;
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
