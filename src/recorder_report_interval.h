#pragma once
// ──────────────────────────────────────────────────────────────────
// recorder_report_interval.h — 鼠标 HID 报告周期估计（纯逻辑，可自检）
//
// 【为什么需要它】
// 录制的相对位移时间轴必须贴合鼠标真实的 HID 报告周期，否则回放整体
// 快放/慢放。3D 游戏里「转视角 + 走路」的位移与时间强耦合（同样的位移，
// 转得快慢不同 → 行走轨迹不同），时间轴被拉伸就直接表现为
// 「回放位置每次都和录制时不一样」。
//
// 【旧实现的两个硬编码，会把高轮询率鼠标的时间轴拉伸】
//   1) `recorder.cpp` 的 EMA 下限写死 1000µs（只在 [500,20000]µs 内学习），
//      ⇒ 2000Hz 以上学不到真实周期，4kHz(250µs) 被拉伸 4 倍、8kHz(125µs) 8 倍；
//   2) `recorder_timeline.cpp` 只把 [2000,16000]µs 认作「健康间隔」，
//      其余一律回退 8000µs、并把 <500µs 的间隔全部改写成回退值，
//      ⇒ 4kHz 的录制在转换阶段再被拉伸到 8000µs（累计 32 倍）。
//
// 【本模型】
//   用中位数估计报告周期（对少量积压样本稳健），下限 125µs（8kHz），
//   只在「明显短于估计周期」时才判定为积压/被合并、需要重建时间戳。
//   样本不足时退回保守值 8000µs（Windows 11 把**后台** Raw Input 接收方
//   限流到 ~125Hz，这是后台录制时的真实周期）。
//
// 注意：模型本身不加锁。调用方负责串行化 —— 录制侧只有 Raw Input 线程
// 会碰它（与既有 g_lastRelStampUs 的用法一致）。
// ──────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qst_recorder {

/// 可观测的 HID 报告周期范围：125µs(8kHz) ~ 20ms(50Hz)。
constexpr uint64_t kMinReportIntervalUs = 125;
constexpr uint64_t kMaxReportIntervalUs = 20000;
/// 样本不足时的保守回退（= Windows 11 后台 Raw Input 接收方的 ~125Hz 限流周期）。
constexpr uint64_t kFallbackReportIntervalUs = 8000;
/// 历史压缩阈值。常规轮询率下**必须**沿用它：手写宏里「相对移动 + 1ms 等待」
/// 是刻意的，若按中位数比例判定会被误判成积压、被拉到报告周期上（慢 10 倍）。
constexpr uint64_t kLegacyCompressedGapUs = 500;
/// 短于中位数 1/4 的间隔视为不可信（队列积压 / 被系统合并），需要重建时间戳。
constexpr uint64_t kCompressedGapDivisor = 4;
/// 少于该样本数时不信中位数，走保守回退（避免 2 个样本就下结论）。
constexpr std::size_t kMinSamplesForMedian = 4;
/// 参与中位数的样本上限（滚动窗口，避免长录制内存无界）。
constexpr std::size_t kMaxSamples = 512;

class ReportIntervalModel {
public:
    void Reset() {
        gaps_.clear();
        observed_ = 0;
        compressed_ = 0;
    }

    /// 观察一个相邻相对包的间隔（µs）。越界样本只计数、不入统计。
    /// 返回是否被纳入统计。
    bool ObserveGap(uint64_t gapUs) {
        ++observed_;
        if (gapUs < kMinReportIntervalUs || gapUs > kMaxReportIntervalUs) return false;
        gaps_.push_back(gapUs);
        if (gaps_.size() > kMaxSamples) {
            const std::size_t drop = gaps_.size() - kMaxSamples;
            gaps_.erase(gaps_.begin(), gaps_.begin() + static_cast<std::ptrdiff_t>(drop));
        }
        return true;
    }

    /// 当前估计的报告周期（µs）。
    uint64_t MedianUs() const {
        if (gaps_.size() < kMinSamplesForMedian) return kFallbackReportIntervalUs;
        std::vector<uint64_t> copy(gaps_);
        const std::size_t mid = copy.size() / 2;
        std::nth_element(copy.begin(), copy.begin() + static_cast<std::ptrdiff_t>(mid),
            copy.end());
        uint64_t v = copy[mid];
        if (v < kMinReportIntervalUs) v = kMinReportIntervalUs;
        if (v > kMaxReportIntervalUs) v = kMaxReportIntervalUs;
        return v;
    }

    /// 当前判定「间隔不可信」的阈值（µs）。
    ///
    /// 两段式，刻意保守：
    ///   - 估计周期 > 500µs（常规轮询率 / 后台被限流 / 手写宏）⇒ **沿用历史阈值 500µs**。
    ///     这保证手写宏与旧录制行为完全不变。
    ///   - 估计周期 ≤ 500µs（已实测证实是 2kHz 以上鼠标）⇒ 阈值收缩到周期 1/4，
    ///     且**绝不高于周期本身**，否则真实报文会被当成积压、时间轴被整体拉伸。
    uint64_t CompressedGapThresholdUs() const {
        const uint64_t median = MedianUs();
        if (median > kLegacyCompressedGapUs) return kLegacyCompressedGapUs;
        uint64_t t = median / kCompressedGapDivisor;
        if (t < kMinReportIntervalUs) t = kMinReportIntervalUs;
        if (t > median) t = median;
        return t;
    }

    /// 该间隔是否短到不可信（积压 / 被系统合并）⇒ 应用 MedianUs() 重建时间戳。
    bool IsCompressedGap(uint64_t gapUs) const {
        return gapUs < CompressedGapThresholdUs();
    }

    /// 供诊断：本次录制是否已判定为「高轮询率」（周期 < 1ms）。
    bool LooksHighPolling() const { return MedianUs() < 1000; }

    std::size_t SampleCount() const { return gaps_.size(); }
    uint64_t ObservedCount() const { return observed_; }
    uint64_t CompressedCount() const { return compressed_; }
    void NoteCompressed() { ++compressed_; }

    /// 把观测到的间隔换算成最接近的常见轮询率（Hz），用于日志可读性。
    static uint64_t NearestPollingHz(uint64_t intervalUs) {
        if (intervalUs == 0) return 0;
        const uint64_t hz = 1000000ULL / intervalUs;
        return hz;
    }

private:
    std::vector<uint64_t> gaps_;
    uint64_t observed_ = 0;
    uint64_t compressed_ = 0;
};

}  // namespace qst_recorder
