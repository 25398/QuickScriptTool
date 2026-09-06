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

void AppendCrashBreadcrumb(DWORD code, void* addr) {
    // 极早/崩溃路径：尽量留下可读痕迹（exe 旁 + LocalAppData）
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[192]{};
    sprintf_s(line,
        "%04u-%02u-%02u %02u:%02u:%02u FATAL unhandled code=0x%08lX addr=%p\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        code, addr);
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
