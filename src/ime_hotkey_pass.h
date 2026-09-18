#pragma once
// ──────────────────────────────────────────────────────────────────
// ime_hotkey_pass.h — 「中文输入法不触发热键」判定（可单测）
//
// 勾选后：中文转换模式或正在组字时不触发空闲热键；Shift 英文仍可用。
// 热路径（LL 钩子 / TIME_CRITICAL 轮询）只读原子缓存，禁止 Imm/SendMessage。
// UI 定时器只问一次默认 IME 窗口的 IMC_GETCONVERSIONMODE（短超时）。
// ──────────────────────────────────────────────────────────────────

#include <windows.h>
#include <imm.h>

#pragma comment(lib, "imm32.lib")

namespace ime_hotkey_pass {

constexpr UINT kImcGetConversionMode = 0x0001;
constexpr UINT kRemoteImeTimeoutMs = 8;

struct Snapshot {
    bool hasForeground = false;
    bool ownProcess = false;
    bool composing = false;
    bool native = false;
};

// 仅中文模式/组字才挡；中文布局、输入法打开不能挡（拼音 Shift 英文仍开着）。
inline bool ShouldPassThroughHotkey(const Snapshot& s) {
    if (!s.hasForeground || s.ownProcess) return false;
    return s.composing || s.native;
}

enum class NativeProbe {
    Unknown = 0,
    English = 1,
    Chinese = 2,
};

// UI 线程专用。一次短超时；失败返回 Unknown，调用方保留旧缓存。
inline NativeProbe ProbeForegroundNativeMode(HWND fg) {
    if (!fg || !IsWindow(fg)) return NativeProbe::Unknown;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return NativeProbe::English;

    const HWND imeWnd = ImmGetDefaultIMEWnd(fg);
    if (!imeWnd) return NativeProbe::Unknown;

    DWORD_PTR mode = 0;
    if (SendMessageTimeoutW(imeWnd, WM_IME_CONTROL, kImcGetConversionMode, 0,
            SMTO_ABORTIFHUNG | SMTO_NORMAL, kRemoteImeTimeoutMs, &mode) == 0) {
        return NativeProbe::Unknown;
    }
    return ((mode & IME_CMODE_NATIVE) != 0) ? NativeProbe::Chinese : NativeProbe::English;
}

inline bool ProbeForegroundBlocksHotkey() {
    return ProbeForegroundNativeMode(GetForegroundWindow()) == NativeProbe::Chinese;
}

}  // namespace ime_hotkey_pass
