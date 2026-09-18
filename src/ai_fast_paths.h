#pragma once

#include <atomic>

/// AI 动作执行的「高级加速」总开关（设置 → 宏回放设置，默认**开**）。
///
/// 这几条加速都是「用记忆/本地信息省掉一次识图或一次上传」，各自都有边界条件，
/// 出问题时必须能**一条开关全关**，让链路退回到「每步真识图 + 每轮回传整帧」的保守行为：
///   · 布局记忆（同一目标/同一窗口直接用上次坐标）
///   · 相对网格（一次定位推整片格子）
///   · 观察帧省上传 + 文字索引（OCR 文字坐标注入）
///
/// 关掉后行为 = 没有这些机制的老链路；单条机制的自检不受影响（自检直接调库函数）。
inline std::atomic_bool& AiFastPathsFlag() {
    static std::atomic_bool flag{true};
    return flag;
}

inline void SetAiFastPaths(bool on) {
    AiFastPathsFlag().store(on, std::memory_order_relaxed);
}

inline bool AiFastPathsEnabled() {
    return AiFastPathsFlag().load(std::memory_order_relaxed);
}
