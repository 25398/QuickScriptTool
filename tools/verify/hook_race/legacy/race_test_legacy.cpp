// =============================================================================
// race_test_legacy.cpp —— **负对照**：跑修复前的 `fake_focus_hook.cpp`
// -----------------------------------------------------------------------------
// 本目录下的 `fake_focus_hook.h/.cpp` 是修复前（git HEAD）的原样副本，
// 本文件是配套的旧签名测试（旧 `CallThroughOriginal` 的 fn 是 `void*(*)(void*)`，
// 原函数靠「临时还原」后按名字调用 —— 新实现是 trampoline，签名不同，故必须分开）。
//
// 目的只有一个：**证明 `run_hook_race.py` 那套断言真的会红**。
// 期望结果（2026-10-03 实测）：
//   1 线程 → PASS（单线程无并发，还原窗口不会被别人撞上）
//   2 线程 → FAIL，av≥1（0xC0000005）+ missedDetour 十几万
//   8 线程 → 进程直接段错误（连汇总行都来不及打）
//
// 由 `tools/verify/hook_race/negcontrol_hook_race.py` 驱动，不要手工跑。
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

// 被钩的目标函数：必须**足够长**（>12 字节），否则旧实现固定覆盖 12 字节
// 会切到相邻函数，干扰判据。
__declspec(noinline) int TargetFn(int x) {
    volatile int acc = x;
    acc = acc + 7;
    return acc;
}

// 旧版回调：`CallThroughOriginal` 已把目标函数头还原，这里直接按名字调用原函数。
void* CallOrig(void* ctx) {
    const int x = *reinterpret_cast<int*>(ctx);
    return reinterpret_cast<void*>(static_cast<intptr_t>(TargetFn(x)));
}

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
                g_av.fetch_add(1, std::memory_order_relaxed);
                g_lastAvCode.store(GetExceptionCode(), std::memory_order_relaxed);
                g_calls.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const int threads = (argc > 1) ? std::atoi(argv[1]) : 8;
    const int seconds = (argc > 2) ? std::atoi(argv[2]) : 5;

    if (!fakefocus::InstallInlineHook(g_hook, reinterpret_cast<void*>(&TargetFn),
            reinterpret_cast<void*>(&TargetDetour))) {
        std::printf("RESULT install_failed=1\n");
        return 2;
    }
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
