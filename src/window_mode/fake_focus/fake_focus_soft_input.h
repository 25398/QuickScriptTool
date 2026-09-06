#pragma once

// Shared soft-input snapshot between QuickScriptTool (host) and FakeFocus*.dll (target).
// Layout is fixed for Wow64 (32/64) — no pointers.

#include <cstdint>
#include <cstdio>

#include <windows.h>

namespace fakefocus {

constexpr uint32_t kSoftInputMagic = 0x51465349u;  // 'QFSI'
constexpr uint32_t kSoftInputVersion = 6;

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
};
#pragma pack(pop)

static_assert(sizeof(SoftKeyEvent) == 4, "SoftKeyEvent size");
static_assert(sizeof(SoftInputState)
        == 4 + 4 + 4 + 4 + 4 + 256 + 4 + 4 + (kSoftKeyEventCap * 4) + 4 + 16 + 12,
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
