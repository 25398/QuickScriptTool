#pragma once
// ──────────────────────────────────────────────────────────────────
// breakout_input.h — 默认模式脱离时间：检测用户键鼠输入（排除热键）
// ──────────────────────────────────────────────────────────────────

#include "utils.h"

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

inline void BreakoutSignalUserInput() {
    if (!g_breakoutHookState || !g_breakoutHookState->userInput) return;
    g_breakoutHookState->userInput->store(true, std::memory_order_relaxed);
}

inline bool BreakoutShouldMonitor() {
    if (!g_breakoutHookState || !g_breakoutHookState->running) return false;
    if (!g_breakoutHookState->running->load(std::memory_order_relaxed)) return false;
    if (g_breakoutHookState->simulatingDepth
        && g_breakoutHookState->simulatingDepth->load(std::memory_order_relaxed) > 0) {
        return false;
    }
    return true;
}

inline LRESULT CALLBACK BreakoutKbProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && BreakoutShouldMonitor()) {
        const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        if (down) {
            auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
            // 脚本注入的按键不算「用户脱离」
            if (!(ks->flags & LLKHF_INJECTED)
                && !BreakoutShouldIgnoreInput(WM_KEYDOWN, static_cast<UINT>(ks->vkCode))) {
                BreakoutSignalUserInput();
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

inline LRESULT CALLBACK BreakoutMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && BreakoutShouldMonitor()) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        if (!(ms->flags & LLMHF_INJECTED)) {
            UINT btnVk = 0;
            const UINT msg = static_cast<UINT>(wp);
            if (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP) btnVk = VK_LBUTTON;
            else if (wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP) btnVk = VK_RBUTTON;
            else if (wp == WM_MBUTTONDOWN || wp == WM_MBUTTONUP) btnVk = VK_MBUTTON;
            else if (wp == WM_XBUTTONDOWN || wp == WM_XBUTTONUP) {
                btnVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            }
            const bool actionable = wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN
                || wp == WM_XBUTTONDOWN || wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL || wp == WM_MOUSEMOVE;
            if (actionable && !BreakoutShouldIgnoreInput(msg, btnVk)) {
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
}

} // namespace breakout_input
