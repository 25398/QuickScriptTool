#pragma once

#include <atomic>

/// 低性能模式（设置 → 宏回放设置「低性能模式」）。
///
/// 用户反馈「挂机脚本时电脑急剧升温（i7-7700 跑到 80°C）」后的取舍开关：
/// 勾选后以**少占 CPU / 降温**为第一目标，允许损失少量注入时机精度与找图召回。
///
/// 具体生效点（每个点都必须查这里，不要再各留一份开关）：
///   · 输入时间轴：忙自旋 12ms → 0.8ms，其余交给 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
///     （`input_timeline_scheduler.cpp`）
///   · 回放期间**不**提进程/线程优先级、**不**绑核、**不**抬系统定时器分辨率
///     （`engine_script_run.cpp` 的三个 playback guard）——抬高全局定时器分辨率会让
///     整机无法进深度 C-state，是长时间挂机发热的隐形大头
///   · 找图：OpenCV 线程预算限 1（默认按核数 fan-out，4 核瞬间满载）
///     （`image_match_engines.cpp` `SyncImageMatchThreadBudget`）
///   · 找图监视（WatchImage 时间模式）最小轮询间隔 50ms → 200ms
///     （`engine_script_run.cpp` `watchPollIntervalSec`）
///
/// 开关是进程级原子量：设置保存后立即生效，无需重启。头文件内联实现，
/// 这样 `app_settings_store_core` 这种轻量自检目标也能用，不必拖进引擎依赖。
inline std::atomic_bool& LowPerformanceModeFlag() {
    static std::atomic_bool flag{false};
    return flag;
}

inline void SetLowPerformanceMode(bool on) {
    LowPerformanceModeFlag().store(on, std::memory_order_relaxed);
}

inline bool LowPerformanceMode() {
    return LowPerformanceModeFlag().load(std::memory_order_relaxed);
}
