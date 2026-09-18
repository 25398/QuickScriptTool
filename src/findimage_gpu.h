#pragma once

#include "low_power_mode.h"

#include <atomic>

/// 找图 GPU（OpenCL）加速开关 —— 与低性能模式同样做成**头文件内联的进程级原子量**，
/// 这样 `app_settings_store_core` 这类轻量目标同步设置时不必把 OpenCV 拖进来。
///
/// 实测（`ImageMatchSelfTest / opencl_matchtemplate_bench`，RTX 4060 Laptop，
/// TM_SQDIFF_NORMED，含上传 + 结果回传）：
///   480x360 + 48x48：CPU 2ms / GPU 2ms   → 打平（纯亏传输开销）
///   1280x720 + 96x96：CPU 24ms / GPU 11ms → 2.2x
///   2560x1440 + 96x96：CPU 100ms / GPU 35ms → 2.9x
/// 因此 `image_match.cpp` 里设了 **500k 像素**的面积门槛：区域找图一律走 CPU。
///
/// 还有一个实测事实：**只调用 `cv::ocl::setUseOpenCL(true)` 没有用** ——
/// OpenCV 的 `matchTemplate` 只对 `UMat` 输入走 OpenCL，对普通 `Mat` 输入
/// 反而更慢（实测 2560x1440 从 46ms 掉到 89ms，因为它要隐式上传再回传）。
/// 所以真正生效的路径必须是显式 UMat（见 `TryMatchTemplateOnGpu`）。
inline std::atomic_bool& FindImageGpuAccelFlag() {
    static std::atomic_bool flag{false};
    return flag;
}

inline void SetFindImageGpuAccel(bool on) {
    FindImageGpuAccelFlag().store(on, std::memory_order_relaxed);
}

inline bool FindImageGpuAccelEnabled() {
    return FindImageGpuAccelFlag().load(std::memory_order_relaxed);
}

/// 真正的生效条件：设置开着 **且** 低性能模式没开。
/// 低性能模式优先 —— 笔记本 dGPU 算一次的功耗/发热比 CPU 更凶，与「省电降温」目标相反。
inline bool FindImageGpuAccelActive() {
    return FindImageGpuAccelEnabled() && !LowPerformanceMode();
}
