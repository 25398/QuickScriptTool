#pragma once

// =============================================================================
// time_scale_clock.h — 变速齿轮（speedhack）的纯计算部分
// =============================================================================
// 为什么单独抽出来：变速的核心是「虚拟时钟」的整数运算，最容易写错（溢出、跳变、
// 换倍率时时间倒流），而它又完全不需要 Windows API。抽成无依赖头文件后，
// tools/time_scale_selftest.cpp 可以在 CI 上直接覆盖它。
//
// 原理（与 Cheat Engine / GearNT / speedhack 系同源）：
//   目标进程看到的时间 = 锚点 + (真实时间 - 锚点) * 倍率
//   只缩放「读时钟」的返回值，**不动 QueryPerformanceFrequency**，
//   于是目标里的 sleep / 冷却 / 帧间隔全部按同一倍率加速。
//
// 锚点（anchor）存在的意义：
//   1) 换倍率时先把虚拟时钟按旧倍率推进到「现在」再重设锚点 —— 时间连续、不倒流、不跳变；
//   2) 每 2 秒重设一次锚点 —— 把 (delta * num) 的乘积控制在 64 位安全范围内。
//
// 定点倍率：num/den，den 固定 kTimeScaleDen（1000），num 范围见 kTimeScaleMin/Max。
// 用整数而不是 double：共享内存要跨 32/64 位进程，整数没有对齐/浮点差异的歧义。
// =============================================================================

#include <cstdint>

namespace fakefocus {

/// 倍率分母固定 1000：num=1000 → 1.0x；num=2000 → 2.0x。
inline constexpr uint32_t kTimeScaleDen = 1000;
inline constexpr uint32_t kTimeScaleMinNum = 100;    // 0.1x
inline constexpr uint32_t kTimeScaleMaxNum = 16000;  // 16.0x
inline constexpr uint32_t kTimeScaleOff = 0;         // 0 = 不挂钩（宿主未启用）

/// 把倍率（double，例如回放倍速 2.0）夹到合法定点值；非法输入返回原速。
inline uint32_t TimeScaleFromDouble(double speed) {
    if (!(speed > 0.0)) return kTimeScaleDen;
    double milli = speed * static_cast<double>(kTimeScaleDen);
    if (milli < static_cast<double>(kTimeScaleMinNum)) return kTimeScaleMinNum;
    if (milli > static_cast<double>(kTimeScaleMaxNum)) return kTimeScaleMaxNum;
    return static_cast<uint32_t>(milli + 0.5);
}

inline bool TimeScaleIsIdentity(uint32_t num) {
    return num == 0 || num == kTimeScaleDen;
}

/// delta * num / den，带溢出饱和保护。
/// 正常路径 delta ≤ 2 秒的 QPC 计数（10MHz → 2e7），乘 16 也只有 3.2e8；
/// 饱和分支只在轮询线程被饿死几十小时时才可能触发，纯属兜底。
inline uint64_t ScaleElapsed(uint64_t delta, uint32_t num, uint32_t den) {
    if (num == den || num == 0) return delta;
    if (den == 0) return delta;
    constexpr uint64_t kSaturate = 1ull << 40;
    if (delta > kSaturate) delta = kSaturate;
    return (delta * static_cast<uint64_t>(num)) / static_cast<uint64_t>(den);
}

/// 虚拟时钟锚点。所有时间都从同一个 QPC 锚点派生，保证几个时钟互不漂移。
struct TimeScaleAnchors {
    uint64_t qpcRealBase = 0;    // 锚点处的真实 QPC
    uint64_t qpcVirtBase = 0;    // 锚点处的虚拟 QPC
    uint64_t tickVirtBaseMs = 0; // 锚点处的虚拟 tick（毫秒，GetTickCount / timeGetTime）
    uint64_t ftVirtBase = 0;     // 锚点处的虚拟 FILETIME（100ns 单位）
    uint32_t num = kTimeScaleDen;
    uint32_t den = kTimeScaleDen;

    bool identity() const { return TimeScaleIsIdentity(num); }
};

/// 首次建立锚点：虚拟时钟 = 真实时钟，倍率 1.0。
inline void InitAnchors(TimeScaleAnchors& a, uint64_t realQpc, uint64_t realTickMs,
    uint64_t realFileTime) {
    a.qpcRealBase = realQpc;
    a.qpcVirtBase = realQpc;
    a.tickVirtBaseMs = realTickMs;
    a.ftVirtBase = realFileTime;
    a.num = kTimeScaleDen;
    a.den = kTimeScaleDen;
}

/// 重设锚点：先把虚拟时钟按**旧**倍率推进到 realNow，再切到新倍率。
/// 这样换倍率（含 1.0 → 2.0 → 1.0）不会出现时间跳变或倒流。
inline void RebaseAnchors(TimeScaleAnchors& a, uint64_t realNow, uint64_t realTickMs,
    uint64_t realFileTime, uint32_t newNum, uint64_t qpcFreq) {
    if (a.qpcRealBase == 0) {
        InitAnchors(a, realNow, realTickMs, realFileTime);
        a.num = newNum;
        return;
    }
    const uint64_t realDelta = realNow >= a.qpcRealBase ? realNow - a.qpcRealBase : 0;
    const uint64_t virtNow = a.qpcVirtBase + ScaleElapsed(realDelta, a.num, a.den);

    if (qpcFreq == 0) qpcFreq = 10000000ull;
    const uint64_t virtDelta = virtNow >= a.qpcVirtBase ? virtNow - a.qpcVirtBase : 0;
    a.tickVirtBaseMs += (virtDelta * 1000ull) / qpcFreq;
    a.ftVirtBase += (virtDelta * 10000000ull) / qpcFreq;
    a.qpcRealBase = realNow;
    a.qpcVirtBase = virtNow;
    a.num = newNum;
    a.den = kTimeScaleDen;
}

/// 虚拟 QPC：倍率为 1.0 时等于真实值（调用方此时直接转发，不走这里）。
inline uint64_t VirtualQpc(const TimeScaleAnchors& a, uint64_t realNow) {
    const uint64_t realDelta = realNow >= a.qpcRealBase ? realNow - a.qpcRealBase : 0;
    return a.qpcVirtBase + ScaleElapsed(realDelta, a.num, a.den);
}

inline uint64_t VirtualTickMs(const TimeScaleAnchors& a, uint64_t virtQpc, uint64_t qpcFreq) {
    if (qpcFreq == 0) qpcFreq = 10000000ull;
    const uint64_t d = virtQpc >= a.qpcVirtBase ? virtQpc - a.qpcVirtBase : 0;
    return a.tickVirtBaseMs + (d * 1000ull) / qpcFreq;
}

inline uint64_t VirtualFileTime(const TimeScaleAnchors& a, uint64_t virtQpc, uint64_t qpcFreq) {
    if (qpcFreq == 0) qpcFreq = 10000000ull;
    const uint64_t d = virtQpc >= a.qpcVirtBase ? virtQpc - a.qpcVirtBase : 0;
    return a.ftVirtBase + (d * 10000000ull) / qpcFreq;
}

}  // namespace fakefocus
