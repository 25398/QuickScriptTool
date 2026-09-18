#pragma once
// 连点时序：间隔与按下抬起必须走微秒，禁止 int(seconds*1000) 截成 0ms。
// 每次点击必须先 down、撑满 hold、再 up，再等待间隔；禁止为赶间隔而省略抬起。

#include <algorithm>
#include <cmath>
#include <cstdint>

inline constexpr uint64_t kClickerMinHoldUs = 1000;  // 1ms 脉冲，系统/HID 才能看成一次完整点击

inline uint64_t ClickerSecondsToUs(double seconds, uint64_t minUs = 0) {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) return minUs;
    const double us = seconds * 1000000.0;
    if (us >= static_cast<double>(UINT64_MAX)) return UINT64_MAX;
    const uint64_t rounded = static_cast<uint64_t>(std::llround(us));
    return (std::max)(minUs, rounded);
}

// 按下到抬起：未启用按下抬起间隔时仍保留 1ms，避免 down/up 落在同一时刻被合成一次按住。
inline uint64_t ClickerHoldUs(bool enablePressRelease, double pressReleaseSeconds) {
    if (!enablePressRelease) return kClickerMinHoldUs;
    return ClickerSecondsToUs(pressReleaseSeconds, kClickerMinHoldUs);
}

inline uint64_t ClickerGapUs(double intervalSeconds) {
    return ClickerSecondsToUs(intervalSeconds, 1);
}
