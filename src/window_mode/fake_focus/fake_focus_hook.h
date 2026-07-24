#pragma once

#include <windows.h>

#include <cstddef>

namespace fakefocus {

/// Minimal absolute-jump hook. Originals are invoked via temporary unpatch
/// (avoids relocating relative CALLs in the stolen prologue).
struct InlineHook {
    void* target = nullptr;
    void* detour = nullptr;
    void* trampoline = nullptr;
    BYTE original[16]{};
    size_t patchSize = 0;
    bool installed = false;
};

bool InstallInlineHook(InlineHook& hook, void* target, void* detour);
bool RemoveInlineHook(InlineHook& hook);

/// Temporarily restore original bytes, run `fn(ctx)`, then re-apply the detour.
bool CallThroughOriginal(InlineHook& hook, void* (*fn)(void*), void* ctx, void** outResult);

}  // namespace fakefocus
