#pragma once

// 滚轮「步数 → 消息」的展开规则 —— **单一事实来源**。
//
// ⚠⚠ 为什么必须收成一个函数（2026-09-30 真机报障「后台窗口模式滚动不能正常滚动」）：
//   `WM_MOUSEWHEEL` / `WM_MOUSEHWHEEL` 的**增量在 wParam 的高 16 位**，是 `SHORT`
//   （−32768..32767）。而产品里三处各自手写同一句算式：
//
//       delta = (positive ? WHEEL_DELTA : -WHEEL_DELTA) * steps        // 120 * steps
//
//   乘积按 `int` 算完再 `static_cast<SHORT>` 截断，所以只要 `120 * steps > 32767`
//   就**回绕成负数** —— 临界点就在 **steps = 274**（273 → 32760 仍合法，
//   274 → 32880 → **−32656，方向整个反过来**）。
//   更早踩到的是**乘法叠加**：一次滚轮动作会同时乘「滚动步数」与 `clickCount`。
//
//   ⚠ 这里刻意**不写"取 273 就安全"** —— 那是把临界值当余量。
//     ⇒ 规则定死：**一条消息只表达 ±1..±64 格**，多出来的拆成多条。
//     这也让三条路径（宿主 PostMessage / 假焦点 DLL / 硬件 SendInput）语义一致。
//
// 使用方：`background_window_input.cpp`（宿主消息路径）、
//          `fake_focus_dll.cpp`（目标进程内队列）、
//          `window_mode_executor.cpp`（硬件路径，按本表拆包再 SendInput）。
//
// 纯函数，无 Win32 依赖 ⇒ 自检 `mouse_wheel_step_events` 直接钉住。

namespace windowmode {

/// 单条滚轮消息允许的最大格数。
///
/// 取 64 而不是 273（= 32767/120）是有意的：`WHEEL_DELTA * 64 = 7680`，
/// 离 `SHORT` 上限还差 4 倍，留足余量给未来改 `WHEEL_DELTA` 或加偏置，
/// 而且 64 格已经远超任何真实滚轮手势 —— 再多的步数本来就该拆成多条。
constexpr int kMaxWheelNotchesPerEvent = 64;

/// 单格增量。与 `windows.h` 的 `WHEEL_DELTA` 同值；本头不依赖 `windows.h`
/// （假焦点 DLL 与纯逻辑自检都要能用），所以自带一份。
/// ⚠ 一致性由编译期断言钉住（`window_mode_types.cpp` 里 `static_assert(kWheelNotchDelta == WHEEL_DELTA)`）
///   —— 别在别处再写一个 120 字面量。
constexpr int kWheelNotchDelta = 120;

/// 把 `steps` 格滚轮展开成若干条消息，每条 `|格数| <= kMaxWheelNotchesPerEvent`。
///
/// @param steps    总格数（< 1 视为 1：滚轮动作画不出"滚 0 格"）
/// @param positive true = 向上/向左（正向），false = 向下/向右
/// @param out      每条消息的**有符号**增量（`±WHEEL_DELTA * 格数`），可直接塞进 wParam 高位
/// @param cap      出参数组容量；超出部分**丢弃**（调用方负责记账，勿静默）
/// @return 实际写入 `out` 的条数
///
/// ⚠ 返回值可能 < 需要的条数（被 `cap` 截断）⇒ 调用方必须比对
///   `CeilDivWheelNotches(steps)`，别把"没写完"当成"滚完了"。
inline int MouseWheelEventsForSteps(int steps, bool positive, int* out, int cap) {
    if (steps < 1) steps = 1;
    if (cap <= 0) return 0;
    const int sign = positive ? 1 : -1;
    int written = 0;
    int remaining = steps;
    while (remaining > 0 && written < cap) {
        const int notch = remaining > kMaxWheelNotchesPerEvent
            ? kMaxWheelNotchesPerEvent : remaining;
        remaining -= notch;
        out[written++] = sign * notch * kWheelNotchDelta;
    }
    return written;
}

/// 展开 `steps` 格**需要**多少条消息（与 `MouseWheelEventsForSteps` 同一把尺）。
/// 调用方用它判断有没有被 `cap` 截断。
inline int WheelNotchEventCount(int steps) {
    if (steps < 1) steps = 1;
    return (steps + kMaxWheelNotchesPerEvent - 1) / kMaxWheelNotchesPerEvent;
}

}  // namespace windowmode
