#pragma once
// ──────────────────────────────────────────────────────────────────
// overlay_input_guard.h — 「我们扣住了用户鼠标」的自愈兜底（可单测）
//
// 事故形态（用户报障：QQ 远程协助操作被控端时，软件在最上层就失去控制权）：
//   选区 / 找图 / OCR / 拖拽取点 / 拖动准星 这些**全屏叠层**都是同一个套路：
//     ① `SetCapture(自己的窗口)`（或宿主主窗口）
//     ② 把窗口挪到屏外（-10000 / -32000）
//     ③ 跑一个**阻塞的模态消息循环**，只在收到「按钮抬起 / Esc」时才退出。
//   只要那个终止事件丢了（远控链路走 SendInput，注入的抬起本来就可能丢；
//   手一抖抬起落在别的窗口也会丢），叠层就永远停在「等 UP」：
//     · 鼠标捕获被我们扣着 ⇒ 用户所有点击都投给我们（而我们在屏外）⇒ **看得见屏幕、点不动**
//     · 窗口还在屏外 ⇒ 用户连我们的界面都看不到、关不掉
//     · 模态循环还在跑 ⇒ 主界面完全不响应
//   本地还能按 Esc 自救；**远控下几乎没有别的入口** ⇒ 用户体感「失去对方电脑的控制权」。
//
// 硬规则（本文件存在的理由）：
//   **任何「接管鼠标捕获 / 把窗口挪到屏外」的交互都必须有超时兜底；
//     一旦判定终止事件丢了，必须主动收尾，绝不把用户的鼠标控制权留在自己手里。**
//   ⇒ 循环一律用 `WaitMessageWithTimeout()`（禁止裸 `GetMessage`），超时后调
//     `ShouldAbortStuckCaptureNow()`。
//
// 判据三条同时成立才动手（避免误伤正常拖拽）：
//   ① 我们确实持有捕获（`GetCapture() == 我们的窗口`）
//   ② 没有任何鼠标键处于按下状态（`GetAsyncKeyState` 全抬起）
//   ③ 距最后一次系统输入已超过阈值（`GetLastInputInfo`）⇒ 用户已经停手
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

namespace overlay_guard {

/// 判定「抬起丢了」的空闲阈值：要明显大于人手拖拽中的自然停顿，又不能让人干等。
inline constexpr unsigned long long kStuckCaptureIdleMs = 1200;

/// 兜底自检节拍：消息循环最多阻塞这么久就必须醒一次做自检。
inline constexpr unsigned kGuardTickMs = 250;

/// 任一鼠标键按下？
/// ⚠ 判据必须是**物理**键态（GetAsyncKeyState）：脚本注入走驱动/VHID 时这里读不到，
///   而叠层本来就是给真人用的，所以「读不到 = 没按着」正是我们想要的语义。
inline bool AnyMouseButtonDown() {
    static const int kVks[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (int vk : kVks) {
        if ((GetAsyncKeyState(vk) & 0x8000) != 0) return true;
    }
    return false;
}

/// 纯判据（无副作用，供单测）。
///   weHoldCapture      — 当前线程是否持有鼠标捕获
///   anyMouseButtonDown — 是否还有鼠标键按着（按着 ⇒ 正常拖拽，一律不许动）
///   idleMs             — 距最后一次系统输入的毫秒数
inline bool ShouldAbortStuckCapture(bool weHoldCapture, bool anyMouseButtonDown,
    unsigned long long idleMs, unsigned long long idleThresholdMs) {
    if (!weHoldCapture) return false;       // 没扣着别人的鼠标，不管
    if (anyMouseButtonDown) return false;   // 用户还按着 ⇒ 这是正常拖拽
    return idleMs >= idleThresholdMs;       // 已停手却收不到抬起 ⇒ 抬起丢了
}

/// 距上次系统输入的毫秒数；拿不到返回 0（保守：不触发兜底）。
inline unsigned long long SystemIdleMs() {
    LASTINPUTINFO li{};
    li.cbSize = sizeof(li);
    if (!GetLastInputInfo(&li)) return 0;
    return static_cast<unsigned long long>(GetTickCount() - li.dwTime);
}

/// 现成判据：叠层/宿主在自己的消息循环里直接调这个。
inline bool ShouldAbortStuckCaptureNow(HWND ourHwnd,
    unsigned long long idleThresholdMs = kStuckCaptureIdleMs) {
    if (!ourHwnd || !IsWindow(ourHwnd)) return false;
    const bool hold = (GetCapture() == ourHwnd);
    return ShouldAbortStuckCapture(hold, AnyMouseButtonDown(), SystemIdleMs(), idleThresholdMs);
}

/// ★ 取一条消息，最多等 timeoutMs；返回 false = **超时**（调用方必须去做兜底自检）。
///
/// 为什么不能用 `GetMessage`：它在「抬起事件丢了」时**永久阻塞**，而捕获还在我们手上 ——
/// 那正是本文件要防的死法。超时唤醒是唯一的出路。
/// 取到 `WM_QUIT` 时返回 true 并原样交给调用方（调用方按既有约定处理）。
///
/// ⚠ 用**绝对 deadline**：即使 `MsgWaitForMultipleObjectsEx` 出现「无消息却立刻返回」
///   的异常情形，也只会空转到 deadline，不会退化成死循环。
inline bool WaitMessageWithTimeout(MSG& msg, unsigned timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) return true;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD left = static_cast<DWORD>(deadline - now);
        const DWORD w = MsgWaitForMultipleObjectsEx(
            0, nullptr, left, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (w == WAIT_TIMEOUT || w == WAIT_FAILED) return false;
        // 其余（有输入到达）⇒ 回到 PeekMessage；deadline 保证不会死转
    }
}

}  // namespace overlay_guard
