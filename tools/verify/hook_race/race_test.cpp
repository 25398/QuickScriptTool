// =============================================================================
// race_test.cpp —— 复现 fake_focus_hook.cpp「临时还原 + 重写跳转」的多线程竞态
// -----------------------------------------------------------------------------
// 待验证的假设（窗口/后台窗口模式导致目标闪退，exit=0xC0000005）：
//   `CallThroughOriginal()` 为了调用原函数，会
//     ① 把目标函数头的 12 字节**写回原始字节**（还原）
//     ② 调用原函数
//     ③ 把 12 字节**重新写成绝对跳转**（重写）
//   而 12 字节写入在 x86-64 上**不是原子的**（只有对齐的 8 字节以内才是）。
//   目标进程（Unity/UE 这类多线程游戏）在渲染线程 + 主线程上高频调用被钩函数时，
//   就可能取到「半个跳转」（`48 B8` + 部分地址 + 旧字节）⇒ `mov rax, <垃圾>; jmp rax`
//   ⇒ 跳到非法地址 ⇒ 0xC0000005。
//
// 本程序不做任何模拟：直接编译 `src/window_mode/fake_focus/fake_focus_hook.cpp` 的
// 真实实现，在多线程下高频调用被钩函数，统计：
//   · av          = 捕获到的访问违例次数（目标闪退的同型故障）
//   · missedDetour= 调用**绕过 detour** 的次数（还原窗口期 = 竞态的另一种可见形态）
//   · badResult   = 返回值错误的次数
// 旧实现：av > 0 或 missedDetour > 0（预期必红）。
// 修复后：三者都必须为 0。
//
// 构建 + 运行：python tools/verify/hook_race/run_hook_race.py
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "fake_focus_hook.h"

#pragma optimize("", off)

namespace {

std::atomic<long long> g_calls{0};
std::atomic<long long> g_detourCalls{0};
std::atomic<long long> g_av{0};
std::atomic<long long> g_badResult{0};
std::atomic<long long> g_lastAvCode{0};
std::atomic<bool> g_stop{false};

fakefocus::InlineHook g_hook;

// 被钩的目标函数。故意做得**短**且无副作用，便于验证返回值。
__declspec(noinline) int TargetFn(int x) {
    return x + 7;
}

// CallThroughOriginal 的回调：经 trampoline 调用原函数。
// ⚠ 修复后这里**不再碰目标函数头** —— trampoline 是安装时生成的一次性副本。
void* CallOrig(void* tramp, void* ctx) {
    using Fn = int (*)(int);
    const int x = *reinterpret_cast<int*>(ctx);
    return reinterpret_cast<void*>(
        static_cast<intptr_t>(reinterpret_cast<Fn>(tramp)(x)));
}

// detour：完全按 fake_focus_dll.cpp 的用法 —— 走 CallThroughOriginal 调原函数。
__declspec(noinline) int TargetDetour(int x) {
    g_detourCalls.fetch_add(1, std::memory_order_relaxed);
    void* out = nullptr;
    fakefocus::CallThroughOriginal(g_hook, CallOrig, &x, &out);
    return static_cast<int>(reinterpret_cast<intptr_t>(out));
}

DWORD WINAPI Worker(LPVOID) {
    while (!g_stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 2000; ++i) {
            __try {
                const int r = TargetFn(5);
                g_calls.fetch_add(1, std::memory_order_relaxed);
                if (r != 12) g_badResult.fetch_add(1, std::memory_order_relaxed);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // 访问违例 = 取到了半个跳转（目标进程里就是「闪退」）。
                g_av.fetch_add(1, std::memory_order_relaxed);
                g_lastAvCode.store(GetExceptionCode(), std::memory_order_relaxed);
                g_calls.fetch_add(1, std::memory_order_relaxed);
                return 0;   // 线程状态已不可信，退出
            }
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const int threads = (argc > 1) ? std::atoi(argv[1]) : 8;
    const int seconds = (argc > 2) ? std::atoi(argv[2]) : 6;

    void* target = reinterpret_cast<void*>(&TargetFn);
    if (!fakefocus::InstallInlineHook(g_hook, target,
            reinterpret_cast<void*>(&TargetDetour))) {
        std::printf("RESULT install_failed=1\n");
        return 2;
    }
    // 装好后先自检一次：确认钩子真的生效（否则测试没有意义）。
    {
        const int r = TargetFn(5);
        if (r != 12 || g_detourCalls.load() == 0) {
            std::printf("RESULT hook_not_effective=1 detour=%lld r=%d\n",
                g_detourCalls.load(), r);
            return 2;
        }
    }

    std::printf("threads=%d seconds=%d\n", threads, seconds);
    std::fflush(stdout);

    HANDLE th[64]{};
    for (int i = 0; i < threads && i < 64; ++i) {
        th[i] = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    }
    Sleep(static_cast<DWORD>(seconds) * 1000);
    g_stop.store(true, std::memory_order_relaxed);
    for (int i = 0; i < threads && i < 64; ++i) {
        if (th[i]) { WaitForSingleObject(th[i], 5000); CloseHandle(th[i]); }
    }

    const long long calls = g_calls.load();
    const long long detour = g_detourCalls.load();
    const long long av = g_av.load();
    const long long bad = g_badResult.load();
    const long long missed = calls - detour;

    std::printf("calls=%lld detour=%lld av=%lld badResult=%lld missedDetour=%lld avCode=0x%llX\n",
        calls, detour, av, bad, missed,
        static_cast<unsigned long long>(g_lastAvCode.load()));

    const bool ok = (av == 0) && (bad == 0) && (missed <= 0);
    std::printf("RESULT %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
