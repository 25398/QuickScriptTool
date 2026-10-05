#include "input_emergency_teardown.h"

#include "breakout_input.h"
#include "foreground_input_router.h"
#include "hid_interception.h"
#include "virtual_hid.h"

#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace input_emergency {
namespace {

constexpr DWORD kLlStallMs = 1500;
constexpr DWORD kWatchdogPeriodMs = 250;

std::mutex g_extraMu;
std::vector<ExtraTeardownFn> g_extras;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_teardownBusy{false};
std::atomic<LONG> g_llDepth{0};
std::atomic<DWORD> g_llEnterTick{0};
std::atomic<HANDLE> g_watchdogThread{nullptr};
std::atomic<bool> g_watchdogStop{false};

LPTOP_LEVEL_EXCEPTION_FILTER g_prevUnhandled = nullptr;

void CallExtraSeh(ExtraTeardownFn fn) {
    if (!fn) return;
    __try {
        fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ForceTeardownSeh() {
    __try {
        ForegroundInputRouter::Instance().ForceTeardown();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        __try {
            VirtualHidBackend::Instance().Close();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        __try {
            HidInterceptionBackend::Instance().Close();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

void RunExtras() {
    std::vector<ExtraTeardownFn> copy;
    {
        std::lock_guard<std::mutex> lock(g_extraMu);
        copy = g_extras;
    }
    for (ExtraTeardownFn fn : copy) {
        CallExtraSeh(fn);
    }
}

void TeardownHidSessions() {
    ForceTeardownSeh();
}

DWORD WINAPI WatchdogThreadProc(LPVOID) {
    DWORD lastRecoverTick = 0;
    while (!g_watchdogStop.load(std::memory_order_acquire)) {
        Sleep(kWatchdogPeriodMs);
        if (g_llDepth.load(std::memory_order_acquire) <= 0) continue;
        const DWORD entered = g_llEnterTick.load(std::memory_order_acquire);
        if (entered == 0) continue;
        const DWORD now = GetTickCount();
        const DWORD elapsed = now - entered;
        if (elapsed < kLlStallMs) continue;
        // 刚恢复过就再等一个冷却期，避免风暴期间每 250ms 疯狂卸钩
        if (lastRecoverTick != 0 && now - lastRecoverTick < 5000) continue;
        lastRecoverTick = now;
        // LL 回调卡住时整桌面鼠标会假死；触摸屏常仍可用。先卸钩再收 HID。
        TeardownHooksOnly();
        TeardownHidSessions();
        g_llDepth.store(0, std::memory_order_release);
        g_llEnterTick.store(0, std::memory_order_release);
    }
    return 0;
}

void StartWatchdogLocked() {
    if (g_watchdogThread.load(std::memory_order_acquire)) return;
    g_watchdogStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, WatchdogThreadProc, nullptr, 0, nullptr);
    if (th) g_watchdogThread.store(th, std::memory_order_release);
}

/// ★★ **崩溃归因：地址 → 模块+偏移**（2026-09-30 闪退事故）。
///
/// 起因：连续三次闪退都是 `code=0xC00000FD`（**栈溢出 ⇒ 无限递归**），
///   而崩溃日志只有裸地址（`addr=00007FF8A424678C`）——
///   三次地址几乎相同 ⇒ 故障模块**不带 ASLR**（固定基址），但光看地址谁也不知道是谁。
///   现在把 `addr` 解析成「模块名 + 偏移」，一次就能点名（是 msedgewebview2、
///   是 VirtualDesktopAccessor11，还是我们自己的 exe）。
static void DescribeFaultAddress(void* addr, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!addr) return;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return;
    const auto base = reinterpret_cast<HMODULE>(mbi.AllocationBase);
    if (!base) return;
    wchar_t wpath[MAX_PATH]{};
    if (!GetModuleFileNameW(base, wpath, MAX_PATH)) return;
    const wchar_t* name = wcsrchr(wpath, L'\\');
    name = name ? (name + 1) : wpath;
    char narrow[MAX_PATH * 2]{};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
    const unsigned long long off = reinterpret_cast<unsigned long long>(addr)
        - reinterpret_cast<unsigned long long>(base);
    sprintf_s(out, outLen, "module=%s +0x%llX", narrow, off);
}

/// ★★ **递归指纹**：从当前栈里抓"像代码地址"的字，按「模块+偏移」聚合，报出现最多的几个。
///
/// 为什么需要：栈溢出时最要紧的信息是"**哪个函数在自己调自己**"。
///   x64 上不一定有帧指针，逐帧回溯不可靠；但**深递归会把同一个返回地址在栈上重复几百次**
///   ⇒ 按 (模块,偏移) 计数、取前几名，重复次数最高的那一条**就是那个递归函数**。
/// ⚠ 纯尽力而为（读栈可能越界/读到陈旧数据）：只用于崩溃后归因，不影响正常路径。
static void DescribeStackRecursion(char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    struct Frame { unsigned long long mod; unsigned long long off; int count; };
    Frame frames[12]{};
    int frameCount = 0;
    const unsigned long long sp = reinterpret_cast<unsigned long long>(_AddressOfReturnAddress());
    const unsigned long long limit = sp + 512ULL * 1024ULL;   // 最多扫 512KB 栈
    for (unsigned long long p = sp; p + 8 <= limit; p += 8) {
        unsigned long long v = 0;
        __try {
            v = *reinterpret_cast<const unsigned long long*>(p);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (v < 0x10000ULL) continue;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof(mbi)) == 0) continue;
        if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                | PAGE_EXECUTE_WRITECOPY))) {
            continue;
        }
        const unsigned long long mod = reinterpret_cast<unsigned long long>(mbi.AllocationBase);
        const unsigned long long off = v - mod;
        bool merged = false;
        for (int i = 0; i < frameCount; ++i) {
            if (frames[i].mod == mod && frames[i].off == off) {
                ++frames[i].count;
                merged = true;
                break;
            }
        }
        if (!merged && frameCount < 12) {
            frames[frameCount].mod = mod;
            frames[frameCount].off = off;
            frames[frameCount].count = 1;
            ++frameCount;
        }
    }
    // 按出现次数排序（冒泡，最多 12 项），取前 4
    for (int i = 0; i < frameCount; ++i) {
        for (int j = i + 1; j < frameCount; ++j) {
            if (frames[j].count > frames[i].count) {
                const Frame t = frames[i];
                frames[i] = frames[j];
                frames[j] = t;
            }
        }
    }
    size_t used = 0;
    for (int i = 0; i < frameCount && i < 4; ++i) {
        wchar_t wpath[MAX_PATH]{};
        char nm[64] = "?";
        if (GetModuleFileNameW(reinterpret_cast<HMODULE>(frames[i].mod), wpath, MAX_PATH)) {
            const wchar_t* b = wcsrchr(wpath, L'\\');
            b = b ? (b + 1) : wpath;
            WideCharToMultiByte(CP_UTF8, 0, b, -1, nm, sizeof(nm), nullptr, nullptr);
        }
        const int n = sprintf_s(out + used, outLen - used, "%s+0x%llX x%d; ",
            nm, frames[i].off, frames[i].count);
        if (n <= 0 || static_cast<size_t>(n) >= outLen - used) break;
        used += static_cast<size_t>(n);
    }
}

void AppendCrashBreadcrumb(DWORD code, void* addr) {
    // 极早/崩溃路径：尽量留下可读痕迹（exe 旁 + LocalAppData）
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char where[320]{};
    DescribeFaultAddress(addr, where, sizeof(where));
    char stackTop[512]{};
    if (code == 0xC00000FDu) {          // STATUS_STACK_OVERFLOW ⇒ 抓递归指纹
        DescribeStackRecursion(stackTop, sizeof(stackTop));
    }
    char line[1200]{};
    sprintf_s(line,
        "%04u-%02u-%02u %02u:%02u:%02u FATAL unhandled code=0x%08lX addr=%p %s%s%s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        code, addr, where,
        (stackTop[0] ? " | 递归指纹(重复最多的代码地址): " : ""), stackTop);
    const DWORD lineLen = static_cast<DWORD>(strlen(line));

    auto appendOne = [&](const wchar_t* path) {
        if (!path || !path[0]) return;
        HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, line, lineLen, &written, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
    };

    wchar_t logPath[MAX_PATH]{};
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)
        && wcsncpy_s(logPath, exePath, _TRUNCATE) == 0) {
        if (wchar_t* slash = wcsrchr(logPath, L'\\')) {
            if (wcscpy_s(slash + 1,
                    static_cast<size_t>(MAX_PATH - (slash + 1 - logPath)),
                    L"shell_startup.log") == 0) {
                appendOne(logPath);
            }
        }
    }
    wchar_t localApp[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) {
        wchar_t dir[MAX_PATH]{};
        if (swprintf_s(dir, L"%s\\QuickScriptTool", localApp) > 0) {
            CreateDirectoryW(dir, nullptr);
            if (swprintf_s(logPath, L"%s\\shell_startup.log", dir) > 0) {
                appendOne(logPath);
            }
        }
    }
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) {
    if (info && info->ExceptionRecord) {
        AppendCrashBreadcrumb(info->ExceptionRecord->ExceptionCode,
            info->ExceptionRecord->ExceptionAddress);
    }
    TeardownNow();
    if (g_prevUnhandled) return g_prevUnhandled(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

void AtexitTeardown() {
    g_watchdogStop.store(true, std::memory_order_release);
    TeardownNow();
    HANDLE th = g_watchdogThread.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        WaitForSingleObject(th, 1000);
        CloseHandle(th);
    }
}

}  // namespace

void RegisterExtraTeardown(ExtraTeardownFn fn) {
    if (!fn) return;
    std::lock_guard<std::mutex> lock(g_extraMu);
    for (ExtraTeardownFn existing : g_extras) {
        if (existing == fn) return;
    }
    g_extras.push_back(fn);
}

void UninstallBreakoutSeh() {
    __try {
        breakout_input::UninstallBreakoutHooks();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TeardownHooksOnly() {
    UninstallBreakoutSeh();
    RunExtras();
}

void TeardownNow() {
    bool expected = false;
    if (!g_teardownBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    TeardownHooksOnly();
    TeardownHidSessions();
    g_teardownBusy.store(false, std::memory_order_release);
}

void InstallProcessHandlers() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        StartWatchdogLocked();
        return;
    }
    g_prevUnhandled = SetUnhandledExceptionFilter(UnhandledFilter);
    std::atexit(AtexitTeardown);
    StartWatchdogLocked();
}

LlHookGuard::LlHookGuard() {
    if (g_llDepth.fetch_add(1, std::memory_order_acq_rel) == 0) {
        g_llEnterTick.store(GetTickCount(), std::memory_order_release);
    }
}

LlHookGuard::~LlHookGuard() {
    // 看门狗可能在回调仍活着时把深度清零；用钳位递减避免下溢成负数后长期屏蔽检测。
    LONG cur = g_llDepth.load(std::memory_order_acquire);
    while (cur > 0) {
        if (g_llDepth.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel)) break;
    }
    if (cur == 1) {
        g_llEnterTick.store(0, std::memory_order_release);
    }
}

}  // namespace input_emergency
