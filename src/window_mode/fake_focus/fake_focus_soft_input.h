#pragma once

// Shared soft-input snapshot between QuickScriptTool (host) and FakeFocus*.dll (target).
// Layout is fixed for Wow64 (32/64) — no pointers.

#include <cstdint>
#include <cstdio>

#include <windows.h>

namespace fakefocus {

constexpr uint32_t kSoftInputMagic = 0x51465349u;  // 'QFSI'
constexpr uint32_t kSoftInputVersion = 2;

constexpr uint32_t kSoftFlagCursorValid = 1u << 0;
constexpr uint32_t kSoftFlagKeysValid = 1u << 1;

#pragma pack(push, 4)
struct SoftInputState {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t flags = 0;
    int32_t cursorScreenX = 0;
    int32_t cursorScreenY = 0;
    uint8_t down[256]{};  // non-zero => key/button logically down (0x80 style)
    uint32_t seq = 0;
    uint32_t reserved = 0;
};
#pragma pack(pop)

static_assert(sizeof(SoftInputState) == 4 + 4 + 4 + 4 + 4 + 256 + 4 + 4, "SoftInputState size");

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
