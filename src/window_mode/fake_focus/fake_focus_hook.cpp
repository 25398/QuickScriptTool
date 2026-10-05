#include "fake_focus_hook.h"

#include "MinHook.h"

namespace fakefocus {
namespace {

/// MinHook 的 `MH_Initialize` 是幂等的：已初始化时返回 MH_ERROR_ALREADY_INITIALIZED。
bool EnsureEngine() {
    const MH_STATUS st = MH_Initialize();
    return st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED;
}

}  // namespace

bool InitInlineHooks() { return EnsureEngine(); }

bool InstallInlineHook(InlineHook& hook, void* target, void* detour) {
    if (!target || !detour) return false;
    if (hook.installed) return hook.target == target;
    if (!EnsureEngine()) return false;

    // trampoline = 原函数前 N 字节的副本 + 跳回 target+N。
    // 有了它，调用原函数不再需要碰目标函数头 —— 这是本次修复的核心。
    void* trampoline = nullptr;
    if (MH_CreateHook(target, detour, &trampoline) != MH_OK) return false;

    // MH_EnableHook 会**先挂起其他线程**再落补丁（消除安装瞬间的撕裂窗口）。
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        MH_RemoveHook(target);
        return false;
    }

    hook.target = target;
    hook.detour = detour;
    hook.trampoline = trampoline;
    hook.installed = true;
    return true;
}

bool RemoveInlineHook(InlineHook& hook) {
    if (!hook.installed || !hook.target) return true;
    void* target = hook.target;
    // 先摘掉「已安装」标记：卸载期间并发进入 detour 的线程会走调用方的兜底分支，
    // 而不是拿着即将失效的 trampoline 去跳。
    hook.target = nullptr;
    hook.detour = nullptr;
    hook.trampoline = nullptr;
    hook.installed = false;
    MH_DisableHook(target);   // 恢复目标函数头
    MH_RemoveHook(target);    // 释放 trampoline
    return true;
}

bool CallThroughOriginal(InlineHook& hook, OriginalInvoker fn, void* ctx, void** outResult) {
    if (!hook.installed || !hook.trampoline || !fn) return false;
    void* result = fn(hook.trampoline, ctx);
    if (outResult) *outResult = result;
    return true;
}

}  // namespace fakefocus
