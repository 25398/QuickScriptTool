#pragma once

#include <windows.h>

namespace fakefocus {

/// 内联钩。原函数经 **trampoline** 调用（安装时一次性生成，之后只读）。
///
/// ⚠⚠ 2026-10-03 事故修复（窗口/后台窗口模式导致目标闪退，`exit=0xC0000005`）：
///   旧实现用「**临时还原 + 重写跳转**」来调用原函数：
///       还原 12 字节 → 调用原函数 → 重新写入 12 字节绝对跳转
///   而 x86-64 上 **12 字节写入不是原子的**（只有对齐的 8 字节以内才是）。
///   目标进程（Unity / UE 这类多线程游戏）在多个线程上同时调用被钩函数时，
///   会取到「半个跳转」⇒ `mov rax, <垃圾>; jmp rax` ⇒ **目标进程 0xC0000005 闪退**。
///   现场特征：**单线程目标不崩，多线程 3D/游戏目标崩**；同一路径还会静默
///   **漏钩**（还原窗口期调用绕过 detour），表现为「后台输入时灵时不灵」。
///   复现（`build/_tmp/hookrace/`，直接编译本文件的真实实现）：
///       单线程 5s：1,676,000 次调用，av=0、漏钩 0     → PASS
///       双线程 5s：1,924,095 次调用，av=1（0xC0000005）、漏钩 262,996 → FAIL
///       八线程 6s：进程直接段错误
///   单线程全绿 ⇒ 与「12 字节覆盖越界」无关，纯粹是并发写入竞态。
///
///   现在改用 **MinHook**（BSD-2-Clause，`third_party/minhook/`）的 trampoline：
///   目标函数头**只在安装时写一次**（`MH_EnableHook` 还会先挂起其他线程保证不撕裂），
///   之后永远只读 —— 写入竞态从根上消失。顺带修掉两个同源缺陷：
///     ① 固定覆盖 12 字节、不看指令边界（MinHook 用 HDE32/HDE64 解码到完整指令）；
///     ② 安装瞬间的撕裂窗口（`MH_EnableHook` 冻结线程后再落补丁）。
struct InlineHook {
    void* target = nullptr;
    void* detour = nullptr;
    void* trampoline = nullptr;   // MinHook 的 original，可直接按原签名调用
    bool installed = false;
};

/// 进程内初始化 hook 引擎（幂等）。可以不调 —— `InstallInlineHook` 会惰性初始化。
/// ⚠ **不要**在 `DllMain` 里调用（MinHook 会建堆/自旋锁，loader lock 下不安全）。
bool InitInlineHooks();

bool InstallInlineHook(InlineHook& hook, void* target, void* detour);
bool RemoveInlineHook(InlineHook& hook);

/// 调用原函数：`fn(trampoline, ctx)` 里把 trampoline 按原签名转型后调用。
/// **线程安全** —— 只读 trampoline 代码，不写目标函数头。
using OriginalInvoker = void* (*)(void* trampoline, void* ctx);
bool CallThroughOriginal(InlineHook& hook, OriginalInvoker fn, void* ctx, void** outResult);

}  // namespace fakefocus
