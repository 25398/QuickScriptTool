// ──────────────────────────────────────────────────────────────────
// recorder.cpp — 鼠标/键盘录制基础设施实现
// 注意：main.cpp 中的匿名命名空间包含此模块的本地副本，
// 并且 MainWindow 类实际使用本地副本而非本文件中的定义。
// 本文件作为独立模块提供，供将来解耦使用。
// ──────────────────────────────────────────────────────────────────

#include "recorder.h"

#include <atomic>
#include <mutex>
#include <vector>

std::vector<RecordedEvent> g_recordedEvents;
std::mutex g_recordMutex;  // 保护 g_recordedEvents 跨线程访问
std::atomic_bool g_recording{false};
std::atomic<DWORD> g_recordStartTick{0};
std::atomic<UINT> g_recordingIgnoreModifiers{0};
std::atomic<UINT> g_recordingIgnoreVk{0};
std::atomic_bool g_recordingIgnoreEnabled{false};
HHOOK g_keyboardHook = nullptr;
HHOOK g_mouseHook = nullptr;
std::atomic<DWORD> g_lastMouseMoveTick{0};

namespace {

bool IsModifierDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool ModifiersMatch(UINT modifiers) {
    const bool needAlt = (modifiers & MOD_ALT) != 0;
    const bool needCtrl = (modifiers & MOD_CONTROL) != 0;
    const bool needShift = (modifiers & MOD_SHIFT) != 0;
    const bool needWin = (modifiers & MOD_WIN) != 0;
    const bool altDown = IsModifierDown(VK_MENU) || IsModifierDown(VK_LMENU) || IsModifierDown(VK_RMENU);
    const bool ctrlDown = IsModifierDown(VK_CONTROL) || IsModifierDown(VK_LCONTROL) || IsModifierDown(VK_RCONTROL);
    const bool shiftDown = IsModifierDown(VK_SHIFT) || IsModifierDown(VK_LSHIFT) || IsModifierDown(VK_RSHIFT);
    const bool winDown = IsModifierDown(VK_LWIN) || IsModifierDown(VK_RWIN);
    if (needAlt != altDown || needCtrl != ctrlDown || needShift != shiftDown || needWin != winDown) return false;
    return true;
}

bool ShouldIgnoreRecordedInput(UINT msg, WPARAM vkOrButton) {
    if (!g_recordingIgnoreEnabled.load(std::memory_order_relaxed)) return false;
    const UINT ignoreVk = g_recordingIgnoreVk.load(std::memory_order_relaxed);
    if (!ignoreVk) return false;
    const UINT ignoreMod = g_recordingIgnoreModifiers.load(std::memory_order_relaxed);

    if (ignoreVk == VK_LBUTTON) {
        return msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP;
    }
    if (ignoreVk == VK_RBUTTON) {
        return msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP;
    }
    if (ignoreVk == VK_MBUTTON) {
        return msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP;
    }

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        if (static_cast<UINT>(vkOrButton) != ignoreVk) return false;
        return ModifiersMatch(ignoreMod);
    }
    return false;
}

} // namespace

void SetRecordingIgnoreHotkey(UINT modifiers, UINT vk, bool enabled) {
    g_recordingIgnoreModifiers.store(modifiers, std::memory_order_relaxed);
    g_recordingIgnoreVk.store(vk, std::memory_order_relaxed);
    g_recordingIgnoreEnabled.store(enabled && vk != 0, std::memory_order_relaxed);
}

/// 键盘低级钩子回调 — 捕获按键按下/释放事件
LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && g_recording) {
        auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN
            || wp == WM_KEYUP || wp == WM_SYSKEYUP) {
            if (ShouldIgnoreRecordedInput(
                    wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN ? WM_KEYDOWN : WM_KEYUP,
                    ks->vkCode)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            RecordedEvent ev{};
            ev.timeOffsetMs = GetTickCount()
                - g_recordStartTick.load(std::memory_order_relaxed);
            ev.msg = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)
                ? WM_KEYDOWN : WM_KEYUP;
            ev.vkOrButton = ks->vkCode;
            ev.x = 0; ev.y = 0;
            {
                std::lock_guard<std::mutex> lock(g_recordMutex);
                g_recordedEvents.push_back(ev);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

/// 鼠标低级钩子回调 — 按固定采样率捕获移动，即时捕获按键
LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && g_recording) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        DWORD now = GetTickCount();
        DWORD startTick = g_recordStartTick.load(std::memory_order_relaxed);
        if (wp == WM_MOUSEMOVE) {
            // 限制移动事件的采样频率
            if (now - g_lastMouseMoveTick.load(std::memory_order_relaxed)
                >= kMouseMoveSampleMs) {
                g_lastMouseMoveTick.store(now, std::memory_order_relaxed);
                RecordedEvent ev{};
                ev.timeOffsetMs = now - startTick;
                ev.msg = WM_MOUSEMOVE;
                ev.vkOrButton = 0;
                ev.x = ms->pt.x; ev.y = ms->pt.y;
                std::lock_guard<std::mutex> lock(g_recordMutex);
                g_recordedEvents.push_back(ev);
            }
        } else if (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP) {
            if (ShouldIgnoreRecordedInput(static_cast<UINT>(wp), VK_LBUTTON)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            RecordedEvent ev{};
            ev.timeOffsetMs = now - startTick;
            ev.msg = static_cast<UINT>(wp);
            ev.vkOrButton = VK_LBUTTON;
            ev.x = ms->pt.x; ev.y = ms->pt.y;
            std::lock_guard<std::mutex> lock(g_recordMutex);
            g_recordedEvents.push_back(ev);
        } else if (wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP) {
            if (ShouldIgnoreRecordedInput(static_cast<UINT>(wp), VK_RBUTTON)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            RecordedEvent ev{};
            ev.timeOffsetMs = now - startTick;
            ev.msg = static_cast<UINT>(wp);
            ev.vkOrButton = VK_RBUTTON;
            ev.x = ms->pt.x; ev.y = ms->pt.y;
            std::lock_guard<std::mutex> lock(g_recordMutex);
            g_recordedEvents.push_back(ev);
        } else if (wp == WM_MBUTTONDOWN || wp == WM_MBUTTONUP) {
            if (ShouldIgnoreRecordedInput(static_cast<UINT>(wp), VK_MBUTTON)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            RecordedEvent ev{};
            ev.timeOffsetMs = now - startTick;
            ev.msg = static_cast<UINT>(wp);
            ev.vkOrButton = VK_MBUTTON;
            ev.x = ms->pt.x; ev.y = ms->pt.y;
            std::lock_guard<std::mutex> lock(g_recordMutex);
            g_recordedEvents.push_back(ev);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

/// 安装全局键盘和鼠标钩子（重复调用安全）
void InstallRecordingHooks() {
    if (g_keyboardHook || g_mouseHook) return;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    g_keyboardHook = SetWindowsHookExW(
        WH_KEYBOARD_LL, KeyboardHookProc, inst, 0);
    g_mouseHook = SetWindowsHookExW(
        WH_MOUSE_LL, MouseHookProc, inst, 0);
}

/// 卸载全局键盘和鼠标钩子
void UninstallRecordingHooks() {
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
}
