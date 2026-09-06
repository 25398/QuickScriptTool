#pragma once
// ──────────────────────────────────────────────────────────────────
// breakout_input.h — 默认模式脱离时间：检测用户键鼠输入（排除热键）
// ──────────────────────────────────────────────────────────────────

#include "utils.h"
#include "breakout_cooldown.h"
#include "input/foreground_input_router.h"
#include "input/input_emergency_teardown.h"
#include "input/synthetic_input_filter.h"

#include <atomic>
#include <vector>
#include <windows.h>

struct BreakoutHookState {
    std::atomic<bool>* running = nullptr;
    std::atomic<int>* simulatingDepth = nullptr;
    std::atomic<bool>* userInput = nullptr;
    std::vector<Hotkey> ignoreHotkeys;
};

inline BreakoutHookState* g_breakoutHookState = nullptr;
inline HHOOK g_breakoutKbHook = nullptr;
inline HHOOK g_breakoutMouseHook = nullptr;

namespace breakout_input {

inline void UninstallBreakoutHooks();

inline bool BreakoutModifiersMatch(UINT required) {
    const bool alt = (GetAsyncKeyState(VK_LMENU) & 0x8000) || (GetAsyncKeyState(VK_RMENU) & 0x8000);
    const bool ctrl = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
    const bool shift = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
    const bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
    if (required == 0) return !alt && !ctrl && !shift && !win;
    if ((required & MOD_ALT) && !alt) return false;
    if ((required & MOD_CONTROL) && !ctrl) return false;
    if ((required & MOD_SHIFT) && !shift) return false;
    if ((required & MOD_WIN) && !win) return false;
    return true;
}

inline bool BreakoutIsMouseVk(UINT vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON
        || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

inline bool BreakoutMatchesIgnoreHotkey(UINT msg, UINT vk, const Hotkey& hk) {
    if (!hk.enabled || !hk.vk) return false;
    if (BreakoutIsMouseVk(hk.vk)) {
        if (!BreakoutModifiersMatch(hk.modifiers)) return false;
        if (hk.vk == VK_LBUTTON) return vk == VK_LBUTTON && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP);
        if (hk.vk == VK_RBUTTON) return vk == VK_RBUTTON && (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP);
        if (hk.vk == VK_MBUTTON) return vk == VK_MBUTTON && (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP);
        if (hk.vk == VK_XBUTTON1 || hk.vk == VK_XBUTTON2) {
            return vk == hk.vk && (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP);
        }
        return false;
    }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        if (vk != hk.vk) return false;
        return BreakoutModifiersMatch(hk.modifiers);
    }
    return false;
}

inline bool BreakoutShouldIgnoreInput(UINT msg, UINT vk) {
    if (!g_breakoutHookState) return false;
    for (const auto& hk : g_breakoutHookState->ignoreHotkeys) {
        if (BreakoutMatchesIgnoreHotkey(msg, vk, hk)) return true;
    }
    return false;
}

inline BreakoutHoldTracker g_breakoutUserHolds;

inline void BreakoutNoteUserHold(UINT vk) {
    g_breakoutUserHolds.NoteDown(vk);
}

inline void BreakoutNoteUserRelease(UINT vk) {
    g_breakoutUserHolds.NoteUp(vk);
}

inline void BreakoutClearUserHolds() {
    g_breakoutUserHolds.Clear();
}

inline bool BreakoutUserHolding() {
    return g_breakoutUserHolds.Holding();
}

inline void BreakoutReconcileUserHolds() {
    g_breakoutUserHolds.Reconcile([](UINT vk) {
        return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    });
}

inline void BreakoutSignalUserInput() {
    if (!g_breakoutHookState || !g_breakoutHookState->userInput) return;
    g_breakoutHookState->userInput->store(true, std::memory_order_relaxed);
}

inline bool BreakoutShouldMonitor() {
    if (!g_breakoutHookState || !g_breakoutHookState->running) return false;
    if (!g_breakoutHookState->running->load(std::memory_order_relaxed)) return false;
    // Software SendInput：仍用 simulatingDepth；驱动级改走指纹 / VirtualHid Raw 设备过滤。
    if (g_breakoutHookState->simulatingDepth
        && g_breakoutHookState->simulatingDepth->load(std::memory_order_relaxed) > 0
        && !ForegroundInputRouter::Instance().IsHidActive()) {
        return false;
    }
    return true;
}

/// VirtualHid 会话：Raw 用于识别自家设备；鼠标脱离主路径改回 LL（见 BreakoutMouseProc）。
/// 保留此函数供 WM_INPUT 判断是否仍需做 Raw 侧辅助。
inline bool BreakoutUseRawInputForMouse() {
    return ForegroundInputRouter::Instance().IsHidActive()
        && ForegroundInputRouter::Instance().ActiveBackend()
            == quickscript::ForegroundInputBackend::VirtualHid;
}

inline LRESULT CALLBACK BreakoutKbProc(int code, WPARAM wp, LPARAM lp) {
    input_emergency::LlHookGuard llGuard;
    if (code >= 0 && BreakoutShouldMonitor()) {
        const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        if (down || up) {
            auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
            const bool injected = (ks->flags & LLKHF_INJECTED) != 0;
            const bool ext = (ks->flags & LLKHF_EXTENDED) != 0;
            const UINT vk = static_cast<UINT>(ks->vkCode);
            const auto scan = static_cast<unsigned short>(ks->scanCode);
            const UINT ignoreMsg = down ? static_cast<UINT>(WM_KEYDOWN) : static_cast<UINT>(WM_KEYUP);
            // 脚本注入 / 驱动指纹 不算「用户脱离」
            if (!injected
                && !synthetic_input::MatchesKey(vk, scan, ext, down)
                && !BreakoutShouldIgnoreInput(ignoreMsg, vk)) {
                if (down) {
                    BreakoutNoteUserHold(vk);
                    BreakoutSignalUserInput();
                } else {
                    BreakoutNoteUserRelease(vk);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

inline LRESULT CALLBACK BreakoutMouseProc(int code, WPARAM wp, LPARAM lp) {
    input_emergency::LlHookGuard llGuard;
    if (code >= 0 && BreakoutShouldMonitor()) {
        // VirtualHid 与 Interception 均走 LL：
        // - SetCursorPos 带 LLMHF_INJECTED，不会误脱离
        // - 驱动注入无 INJECTED，靠 breakout 指纹（移动/键/按钮）过滤
        // Raw Input 仍负责：识别自家 VirtualHid 设备；非自家 Raw 纯移动忽略（防 SetCursorPos 回灌）
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        const bool injected = (ms->flags & LLMHF_INJECTED) != 0;
        UINT btnVk = 0;
        const UINT msg = static_cast<UINT>(wp);
        bool down = false;
        if (wp == WM_LBUTTONDOWN) { btnVk = VK_LBUTTON; down = true; }
        else if (wp == WM_LBUTTONUP) { btnVk = VK_LBUTTON; }
        else if (wp == WM_RBUTTONDOWN) { btnVk = VK_RBUTTON; down = true; }
        else if (wp == WM_RBUTTONUP) { btnVk = VK_RBUTTON; }
        else if (wp == WM_MBUTTONDOWN) { btnVk = VK_MBUTTON; down = true; }
        else if (wp == WM_MBUTTONUP) { btnVk = VK_MBUTTON; }
        else if (wp == WM_XBUTTONDOWN || wp == WM_XBUTTONUP) {
            btnVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            down = (wp == WM_XBUTTONDOWN);
        }
        const bool buttonUp = wp == WM_LBUTTONUP || wp == WM_RBUTTONUP || wp == WM_MBUTTONUP
            || wp == WM_XBUTTONUP;
        const bool motion = wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL || wp == WM_MOUSEMOVE;
        bool synthetic = injected;
        if (!synthetic) {
            if (wp == WM_MOUSEMOVE) synthetic = synthetic_input::MatchesMouseMove();
            else if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) synthetic = synthetic_input::MatchesMouseWheel();
            else if (btnVk) synthetic = synthetic_input::MatchesMouseButton(btnVk, down);
        }
        if (!synthetic && !BreakoutShouldIgnoreInput(msg, btnVk)) {
            if (down && btnVk) {
                BreakoutNoteUserHold(btnVk);
                BreakoutSignalUserInput();
            } else if (buttonUp && btnVk) {
                BreakoutNoteUserRelease(btnVk);
            } else if (motion) {
                BreakoutSignalUserInput();
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

inline void InstallBreakoutHooks(BreakoutHookState& state) {
    UninstallBreakoutHooks();
    g_breakoutHookState = &state;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    g_breakoutKbHook = SetWindowsHookExW(WH_KEYBOARD_LL, BreakoutKbProc, inst, 0);
    g_breakoutMouseHook = SetWindowsHookExW(WH_MOUSE_LL, BreakoutMouseProc, inst, 0);
}

inline void UninstallBreakoutHooks() {
    if (g_breakoutKbHook) {
        UnhookWindowsHookEx(g_breakoutKbHook);
        g_breakoutKbHook = nullptr;
    }
    if (g_breakoutMouseHook) {
        UnhookWindowsHookEx(g_breakoutMouseHook);
        g_breakoutMouseHook = nullptr;
    }
    g_breakoutHookState = nullptr;
    BreakoutClearUserHolds();
}

} // namespace breakout_input
