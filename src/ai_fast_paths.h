#pragma once

#include <atomic>

/// AI 动作执行的「高级加速」总开关（设置 → 宏回放设置，默认**开**）。
///
/// 这几条加速都是「用记忆/本地信息省掉一次识图或一次上传」，各自都有边界条件，
/// 出问题时必须能**一条开关全关**，让链路退回到「每步真识图 + 每轮回传整帧」的保守行为：
///   · 观察帧省上传 + 文字索引（OCR 文字坐标注入）
///   · 元素索引（UIA ∪ OCR 合成一张带编号的表，命中就不识图）
///
/// ⚠ 记忆型加速（布局记忆 / 定位模板缓存 / 相对网格 / 找图「上一帧命中」）已**整体撤销**：
/// 它们都是引擎替模型维持的**跨帧世界状态**，见 docs §45。
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
