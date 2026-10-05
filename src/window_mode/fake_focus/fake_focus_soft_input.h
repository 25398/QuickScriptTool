#pragma once

// Shared soft-input snapshot between QuickScriptTool (host) and FakeFocus*.dll (target).
// Layout is fixed for Wow64 (32/64) — no pointers.

#include <cstdint>
#include <cstdio>

#include <windows.h>

namespace fakefocus {

constexpr uint32_t kSoftInputMagic = 0x51465349u;  // 'QFSI'
/// ⚠ **不要**为了「加字段」而抬版本号：游戏进程常常长时间不重启，里面可能还挂着**上一版**
/// `FakeFocus32.dll`（宿主会复用已装好的实例并在日志里警告）。抬版本 = 旧 DLL 判定
/// `SoftInputStateLooksValid` 失败 ⇒ 软键态/DI 全失效（比不加字段严重得多）。
/// 所以：**新字段一律追加在结构体末尾**（旧 DLL 只读它认识的偏移，互不影响）。
constexpr uint32_t kSoftInputVersion = 10;

constexpr uint32_t kSoftFlagCursorValid = 1u << 0;
constexpr uint32_t kSoftFlagKeysValid = 1u << 1;
/// Electron/Chromium：窗口 PID 内按事件队列 PostMessage 灌键鼠（禁止 Peek 伪造 / 假 WM_INPUT）。
constexpr uint32_t kSoftFlagPostKeyEvents = 1u << 2;

constexpr uint32_t kSoftKeyEventCap = 256;

#pragma pack(push, 4)
struct SoftKeyEvent {
    uint8_t vk = 0;
    uint8_t down = 0;  // 1=down 0=up
    uint16_t pad = 0;
};

struct SoftInputState {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t flags = 0;
    int32_t cursorScreenX = 0;
    int32_t cursorScreenY = 0;
    uint8_t down[256]{};  // non-zero => key/button logically down (0x80 style)
    uint32_t seq = 0;
    /// Monotonic count of key/button events pushed (host). DLL keeps a local read cursor.
    uint32_t keyWrite = 0;
    SoftKeyEvent keyEvents[kSoftKeyEventCap]{};
    /// Host bumps on cursor move; DLL posts WM_MOUSEMOVE when it advances.
    uint32_t mouseMoveWrite = 0;
    /// 目标进程钩命中（DLL 写，宿主读）。禁止再 CreateRemoteThread 查这些值。
    uint32_t hitGaks = 0;
    uint32_t hitDiState = 0;
    uint32_t hitDiData = 0;
    uint32_t lastDiStateCb = 0;
    uint32_t hitReady = 0;  // DLL 可写映射成功
    uint32_t hitGfw = 0;
    uint32_t hitFocus = 0;
    /// 安装诊断（DLL 写）。禁止再 CreateRemoteThread 读 MapleIatCount。
    uint32_t mapleDiag = 0;
    uint32_t mapleIatPoll = 0;
    /// foundVt | patchedSlot<<8 | heapVt<<16
    uint32_t mapleDiVt = 0;
    /// 目标进程时钟倍率（定点，1000 = 原速；0 = 关闭并卸载变速钩）。宿主写，DLL 读。
    /// 见 src/window_mode/time_scale_clock.h 与 fake_focus_time_scale.h。
    uint32_t timeScaleMilli = 0;
    /// 变速 detour 的调用计数（DLL 写，宿主读）。下标见 fake_focus_time_scale.h 的 HookId。
    /// **这是唯一能区分「钩子根本没被调到」和「钩子被调了但游戏速度不由它决定」的证据** ——
    /// 没有它就只能靠猜（本项目为此多花了好几轮）。
    uint32_t timeHookCalls[6]{};
    /// Unity(IL2CPP) 变速状态（DLL 写，宿主读）。编码见
    /// fake_focus_unity_timescale.h 的 State()。Unity 游戏靠这个变速，
    /// 系统时钟对它可能无效。
    uint32_t unityState = 0;
    /// ── 冒险岛「输入路径体检」（DLL 写，宿主读）──────────────────────────────
    /// 为什么要有这组：`gfw/gaks/diState/lastCb` 全 0 只能推出「客户端不走这些入口」，
    /// 却无法回答「那它走哪条」—— 于是只能继续补钩子（已浪费轮次）。
    /// 消息泵计数是**唯一能区分**三条路的东西：
    ///   · 泵都没被调到     → 客户端连消息都不是经我们的钩子取的（缓存指针/别的模块）
    ///   · 有 WM_INPUT      → Raw Input（焦点相关；后台拿不到 → 天生不适合真后台）
    ///   · 有 WM_KEYDOWN    → 消息驱动（那「后台不动」就是它自己按激活态门控）
    uint32_t hitPump = 0;       // Peek/Get/Dispatch/CallWindowProc 被调用次数
    uint32_t msgInput = 0;      // 取到的 WM_INPUT 数（Raw Input 证据）
    uint32_t msgKey = 0;        // 取到的 WM_KEYDOWN/WM_KEYUP 数（消息驱动证据）
    uint32_t msgActivate = 0;   // 取到的 WM_ACTIVATE/KILLFOCUS/ACTIVATEAPP 数（含被吞的）
    /// 周期补挂（晚加载模块/晚缓存指针/晚建 DI 设备）累计新补到的槽数。
    uint32_t rescanAdds = 0;
    uint32_t rescanRounds = 0;
    /// ── 滚轮（宿主写，DLL 读）───────────────────────────────────────────────
    /// ⚠⚠ 为什么滚轮要单独一条环形队列，而不是复用 `keyEvents`（2026-09-30 真机报障
    ///   「后台窗口模式滚动不能正常滚动」）：
    ///   原来的 `FakeFocusSoftInput_PushWheel` **一进门就查 `kSoftFlagPostKeyEvents`，
    ///   没置位就直接 return** —— 而那个标志只有 Chromium 壳 / Qt 安卓壳 / Electron 会置。
    ///   普通游戏（GLFW/Unity/UE/GL 客户端 —— 假焦点的主要服务对象）走的是
    ///   `UsesInProcFakeFocusSoftInput()` 分支，于是：
    ///     `PushWheel` 空转 → 日志照样打「假焦点软滚轮 …DLL/PostMessage 队列」→
    ///     宿主 PostMessage 的 WM_MOUSEWHEEL 又被 Raw Input 游戏忽略 ⇒ **滚轮完全不动**，
    ///     而且日志里**一句异常都没有**。
    ///   ⇒ 现在滚轮**无条件入队**，由 DLL 按目标类型分流（Raw Input 直投 / 进程内 PostMessage）。
    ///   单格增量独立成队列，是为了让每一格都有自己的 `WM_INPUT`：多格挤一条会被
    ///   `usButtonData`（USHORT）截断，和 `wParam` 高位的 `short` 是同一类坑。
    uint32_t wheelWrite = 0;
    uint32_t wheelRead = 0;  // 宿主不读；DLL 读游标（放共享内存便于诊断"到底消费了没"）
    /// 每格的 (vertical, positive, steps)：steps 只表达"这一格代表几格"，
    /// 语义与 `keyEvents[].pad` 一致，避免两处对"格数"的定义漂移。
    SoftKeyEvent wheelEvents[kSoftKeyEventCap]{};
};
#pragma pack(pop)

static_assert(sizeof(SoftKeyEvent) == 4, "SoftKeyEvent size");
static_assert(sizeof(SoftInputState)
        == 4 + 4 + 4 + 4 + 4 + 256 + 4 + 4 + (kSoftKeyEventCap * 4) + 4 + 16 + 12 + 12 + 4
        + 24 + 4 + 24
        // 滚轮字段（追加在末尾，见 kSoftInputVersion 的注释：新字段一律往后加，
        // **不要**抬版本号 —— 目标进程里可能还挂着上一版 DLL）：
        + 4 + 4 + (kSoftKeyEventCap * 4),
    "SoftInputState size");

inline void SoftInputMappingName(DWORD targetPid, wchar_t* out, size_t cch) {
    // Local\ prefix: same session only (sufficient for desktop apps).
    swprintf_s(out, cch, L"Local\\QstFakeFocusSoft_%lu", static_cast<unsigned long>(targetPid));
}

inline bool SoftInputStateLooksValid(const SoftInputState* st) {
    return st
        && st->magic == kSoftInputMagic
        && st->version == kSoftInputVersion;
}

}  // namespace fakefocus
