#include "fake_focus_hook.h"

#include <cstring>

namespace fakefocus {
namespace {

#if defined(_M_X64) || defined(__x86_64__)
constexpr size_t kAbsJmpSize = 12;  // mov rax, imm64; jmp rax
#else
constexpr size_t kAbsJmpSize = 5;   // jmp rel32
#endif

void WriteAbsoluteJump(BYTE* dst, const void* to) {
#if defined(_M_X64) || defined(__x86_64__)
    dst[0] = 0x48;
    dst[1] = 0xB8;
    const UINT64 addr = reinterpret_cast<UINT64>(to);
    std::memcpy(dst + 2, &addr, sizeof(addr));
    dst[10] = 0xFF;
    dst[11] = 0xE0;
#else
    dst[0] = 0xE9;
    const INT32 rel = static_cast<INT32>(
        reinterpret_cast<BYTE*>(const_cast<void*>(to)) - (dst + 5));
    std::memcpy(dst + 1, &rel, sizeof(rel));
#endif
}

bool PatchTarget(void* target, const void* bytes, size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(target, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    VirtualProtect(target, size, oldProtect, &oldProtect);
    return true;
}

}  // namespace

bool InstallInlineHook(InlineHook& hook, void* target, void* detour) {
    if (hook.installed || !target || !detour) return false;

    hook.target = target;
    hook.detour = detour;
    hook.patchSize = kAbsJmpSize;
    hook.trampoline = nullptr;  // originals use temporary unpatch (safe vs relative CALL)

    std::memcpy(hook.original, target, hook.patchSize);

    BYTE jump[16]{};
    WriteAbsoluteJump(jump, detour);
    if (!PatchTarget(target, jump, hook.patchSize)) return false;

    hook.installed = true;
    return true;
}

bool RemoveInlineHook(InlineHook& hook) {
    if (!hook.installed || !hook.target) return true;
    if (!PatchTarget(hook.target, hook.original, hook.patchSize)) return false;
    hook.installed = false;
    return true;
}

bool CallThroughOriginal(InlineHook& hook, void* (*fn)(void*), void* ctx, void** outResult) {
    if (!hook.installed || !hook.target || !fn) return false;
    if (!PatchTarget(hook.target, hook.original, hook.patchSize)) return false;
    void* result = fn(ctx);
    BYTE jump[16]{};
    WriteAbsoluteJump(jump, hook.detour);
    PatchTarget(hook.target, jump, hook.patchSize);
    if (outResult) *outResult = result;
    return true;
}

}  // namespace fakefocus
