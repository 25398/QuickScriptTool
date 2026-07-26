#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <vector>

struct InputTimelineStats {
    uint64_t eventCount = 0;
    uint64_t lateEventCount = 0;
    uint64_t maxLateUs = 0;
    uint64_t p95LateUs = 0;
    uint64_t p99LateUs = 0;
};

class PrecisionInputTimeline {
public:
    PrecisionInputTimeline();
    ~PrecisionInputTimeline();
    PrecisionInputTimeline(const PrecisionInputTimeline&) = delete;
    PrecisionInputTimeline& operator=(const PrecisionInputTimeline&) = delete;

    void Reset();
    bool WaitDeltaSeconds(double seconds, const std::function<bool()>& cancelled);
    /// 绝对时间轴：相对 origin 累加；过点则追赶（不拉伸原点）。
    bool WaitDeltaUs(uint64_t deltaUs, const std::function<bool()>& cancelled);
    /// 间隙模式：从「此刻」睡满 deltaUs（测试/特殊用途；录制回放用 WaitDeltaUs）。
    bool WaitGapUs(uint64_t deltaUs, const std::function<bool()>& cancelled);
    bool WaitUntilElapsedUs(uint64_t targetElapsedUs,
        const std::function<bool()>& cancelled);
    InputTimelineStats Stats() const;
    uint64_t ElapsedUs() const { return elapsedUs_; }
    uint64_t LastLatenessUs() const { return lastLatenessUs_; }

private:
    int64_t NowQpc() const;
    uint64_t QpcDeltaToUs(int64_t delta) const;
    int64_t UsToQpcDelta(uint64_t us) const;
    void RecordLateness(int64_t deadlineQpc);
    bool WaitUntilDeadlineQpc(int64_t deadlineQpc,
        const std::function<bool()>& cancelled);

    LARGE_INTEGER frequency_{};
    int64_t originQpc_ = 0;
    uint64_t elapsedUs_ = 0;
    uint64_t lastLatenessUs_ = 0;
    HANDLE timer_ = nullptr;
    std::vector<uint64_t> latenessUs_;
};
