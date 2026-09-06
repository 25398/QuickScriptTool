#pragma once
// ──────────────────────────────────────────────────────────────────
// hotkey_stop.h — 热键启停策略（可单测）
//
// 背景：回放会卸掉 RegisterHotKey，停止只靠 LL 钩子 + UI 线程。
// 精密鼠标宏把工作线程拉到过高优先级忙等时，UI/钩子会被饿死或
// 被 MatchesKey / 卡住的 NeedKeyUp 吞掉物理 F7，表现为「停止热键没用、
// 一直闪、键鼠操作不了」。策略：
//   1) 忙碌时物理单击热键必须能停，不被 Handling / 指纹吞掉
//   2) 启动那次按键仍按着时（NeedKeyUp）不得把自动连发当成停止
//   3) 独立轮询：等启动键松开后再认下一次按下为停止
//
// 全屏游戏 / 反作弊（英雄联盟、逆战等）还会让 RegisterHotKey 与 WH_KEYBOARD_LL
// 同时哑火（RIDEV_NOHOTKEYS、独占全屏、UIPI：游戏提权而本进程未提权时钩子
// 看不到键）。空闲时用 GetAsyncKeyState / Raw 兜底启动；忙碌停止也必须靠轮询，
// 且不得让「启动那次按键」的消费闩把停止 PostMessage 丢掉（UIPI 常丢 KEYUP，
// 忙碌时 idle 轮询不跑，独占全屏还会饿死 16ms 清闩定时器）。
// 与 WM_HOTKEY 双通道靠「本按下已消费」闩锁去重，避免先开后停。
// ──────────────────────────────────────────────────────────────────

namespace hotkey_stop {

struct PollerState {
    bool releaseSeen = false;  // 本轮忙碌已见过热键抬起
    bool fired = false;        // 已发出紧急停止
};

// 忙碌中的单击热键 KEYDOWN：是否应立刻停止（LL 钩子）。
// needKeyUp：启动键还没抬起（含 Windows 自动连发），不得停。
// injected / ExtraInfo 标记仍视为脚本注入，不得当作用户停止。
// 真丢 KEYUP 时由 UI 16ms SyncHotkeyLatches / 轮询松手后再按 兜底。
inline bool ShouldStopOnToggleKeyDown(bool sessionBusy, bool needKeyUp,
    bool injected, bool taggedSynthetic) {
    if (!sessionBusy || needKeyUp) return false;
    if (injected || taggedSynthetic) return false;
    return true;
}

// 空闲时启动：NeedKeyUp / pending / handling 仍挡连发与双通道。
inline bool ShouldStartOnToggleKeyDown(bool sessionBusy, bool needKeyUp,
    bool pending, bool handling, bool injected, bool taggedSynthetic) {
    if (sessionBusy) return false;
    if (injected || taggedSynthetic) return false;
    if (needKeyUp || pending || handling) return false;
    return true;
}

// 物理 KEYUP 必须清 NeedKeyUp，即使指纹把该键当成注入回声。
inline bool ShouldClearToggleLatchOnKeyUp(bool injected, bool taggedSynthetic) {
    return !injected && !taggedSynthetic;
}

enum class PollTick {
    Idle,         // 未在跑
    WaitRelease,  // 忙碌但启动键还按着
    Armed,        // 已松开，等待下一次按下
    FireStop,     // 松开后再按下 → 紧急停止
};

// looksSynthetic：GetAsyncKeyState 也会看到 SendInput，用指纹挡脚本同键。
// 忙碌时 LL 路径仍忽略指纹，保证真人键能停；轮询只作 UI 饿死时的兜底。
inline PollTick TickPoller(PollerState& st, bool sessionBusy, bool keyDown,
    bool looksSynthetic) {
    if (!sessionBusy) {
        st.releaseSeen = false;
        st.fired = false;
        return PollTick::Idle;
    }
    if (st.fired) return PollTick::Armed;
    if (!keyDown) {
        st.releaseSeen = true;
        return PollTick::Armed;
    }
    if (!st.releaseSeen) return PollTick::WaitRelease;
    if (looksSynthetic) return PollTick::Armed;
    st.fired = true;
    return PollTick::FireStop;
}

// ── 空闲启动兜底（RegisterHotKey / LL 被游戏吃掉时）────────────────
// confirmMs：按下后稍等，让正常的 WM_HOTKEY 先到；到点仍未被消费再投递。
constexpr unsigned kIdleFallbackConfirmMs = 30;

struct IdleStartState {
    bool wasDown = false;
    unsigned downTick = 0;
    bool fired = false;
};

enum class IdleTick {
    Idle,         // 键已抬起
    WaitConfirm,  // 已按下，等待 confirm
    Skip,         // 本按下已处理 / 注入 / LL 已接管
    FireStart,    // 应投递启动
};

// llOwnsHotkey：LL 钩子活着且本热键由钩子触发（IME 放行 / 回放挂起 / 注册失败 /
// 长按吞键）。此时异步键态常因吞键误报，轮询不得再启动。
// 正常 RegisterHotKey 模式即使 LL 活着也 llOwns=false：游戏 NOHOTKEYS 时钩子
// 只放行、系统热键不来，必须靠轮询。
inline IdleTick TickIdleStart(IdleStartState& st, bool keyDown, bool looksSynthetic,
    bool alreadyConsumed, bool llOwnsHotkey, unsigned now, unsigned confirmMs) {
    if (!keyDown) {
        st.wasDown = false;
        st.downTick = 0;
        st.fired = false;
        return IdleTick::Idle;
    }
    if (looksSynthetic || alreadyConsumed || llOwnsHotkey) {
        st.fired = true;
        st.wasDown = true;
        return IdleTick::Skip;
    }
    if (st.fired) return IdleTick::Skip;
    if (!st.wasDown) {
        st.wasDown = true;
        st.downTick = now;
        if (confirmMs == 0) {
            st.fired = true;
            return IdleTick::FireStart;
        }
        return IdleTick::WaitConfirm;
    }
    if (now - st.downTick < confirmMs) return IdleTick::WaitConfirm;
    st.fired = true;
    return IdleTick::FireStart;
}

inline bool LlOwnsToggleHotkey(bool llFresh, bool passMode, bool playbackSuspended,
    bool registerFailed) {
    if (!llFresh) return false;
    return passMode || playbackSuspended || registerFailed;
}

inline bool TryConsumeTogglePress(bool& consumed) {
    if (consumed) return false;
    consumed = true;
    return true;
}

// 忙碌单击停止：消费闩仍粘在「启动那次按下」时，只要紧急停止已置位就必须停。
// consumeOk 为本次 TryConsume 结果；emergencyStop 为 RequestEmergencyStop 已置位。
inline bool AllowBusyToggleStop(bool consumeOk, bool emergencyStop) {
    return consumeOk || emergencyStop;
}

// Raw 鼠标抬起是否视为真人松手。连点/宏会持续 Note 同键，MatchesMouseButton
// 不能单独否决松手，否则 HID 宏之后「左键按住即停」会失效。
// 自家 VHID 设备、或 ExtraInfo 标记的注入，都不是真人松手。
inline bool ShouldTreatRawMouseUpAsPhysical(bool fromOurHidDevice, bool extraIsSynthetic) {
    return !fromOurHidDevice && !extraIsSynthetic;
}

// 专属长按（脚本/录制 id）正在拉起运行时，全局按住即停不得抢走会话、不得 StopRun。
// 选中脚本后全局热键也会跑同一条宏；脚本自己的鼠标动作或点选残留的左键
// 会被当成全局松手，表现为开一下关一下。
inline bool DedicatedHoldOwnsRun(int activeHoldId, int globalHotkeyId) {
    return activeHoldId != 0 && activeHoldId != globalHotkeyId;
}

}  // namespace hotkey_stop
