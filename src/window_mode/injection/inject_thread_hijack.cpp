// =============================================================================
// 线程劫持：挂起目标工作线程 → SetThreadContext 指向桩 → 桩执行目标函数 →
// NtSetContextThread 恢复原上下文。不新建线程（可组合手动映射/LoadLibrary）。
// =============================================================================

#include "inject_common.h"
#include "inject_techniques_internal.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace windowmode {
namespace inject {
namespace {

struct ThreadEntry {
    DWORD tid = 0;
    HANDLE handle = nullptr;
    bool suspended = false;
    CONTEXT saved{};
    bool startInTargetExe = false;
    bool ownsWindow = false;
    uintptr_t startAddr = 0;
};

bool CollectCandidateThreads(DWORD pid, std::vector<ThreadEntry>& out,
                             std::wstring& err) {
    // 目标主模块（Toolhelp 快照第一个，通常为主 exe）
    uintptr_t exeBaseAddr = 0;
    {
        HANDLE modSnap = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (modSnap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            if (Module32FirstW(modSnap, &me)) {
                exeBaseAddr = reinterpret_cast<uintptr_t>(me.hModule);
            }
            CloseHandle(modSnap);
        }
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        err = L"CreateToolhelp32Snapshot(THREADS) 失败";
        return false;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == GetCurrentThreadId()) {
                continue;
            }
            ThreadEntry e;
            e.tid = te.th32ThreadID;
            e.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                FALSE, e.tid);
            if (!e.handle) continue;
            EnumThreadWindows(e.tid, [](HWND, LPARAM lp) -> BOOL {
                *reinterpret_cast<bool*>(lp) = true;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&e.ownsWindow));
            typedef LONG(NTAPI* NtQueryInformationThread_t)(
                HANDLE, ULONG, PVOID, ULONG, PULONG);
            auto* query = reinterpret_cast<NtQueryInformationThread_t>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                               "NtQueryInformationThread"));
            if (query) {
                constexpr ULONG kThreadQuerySetWin32StartAddress = 9;
                void* startAddr = nullptr;
                if (query(e.handle, kThreadQuerySetWin32StartAddress,
                          &startAddr, sizeof(startAddr), nullptr) >= 0 &&
                    startAddr) {
                    const uintptr_t start = reinterpret_cast<uintptr_t>(startAddr);
                    e.startAddr = start;
                    const uintptr_t ntdll = reinterpret_cast<uintptr_t>(
                        GetModuleHandleW(L"ntdll.dll"));
                    if (start >= ntdll && start < ntdll + 0x400000) {
                        e.startInTargetExe = false;  // 系统线程：不优先
                    }
                    if (exeBaseAddr != 0 &&
                        start >= exeBaseAddr && start < exeBaseAddr + 0x400000) {
                        e.startInTargetExe = true;
                    }
                }
            }
            out.push_back(e);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    if (out.empty()) {
        err = L"无法打开目标进程的任何线程（权限不足？）";
        return false;
    }
    return true;
}

void CloseThreads(std::vector<ThreadEntry>& threads) {
    for (auto& t : threads) {
        if (t.handle) CloseHandle(t.handle);
        t.handle = nullptr;
    }
}

}  // namespace

// 让目标进程的一个工作线程执行 fn(a1,a2,a3)，桩自动恢复原上下文后继续。
// 等待桩内 result 槽非零（fn 已返回）。成功时 outCodeBuf 为保留的桩代码区
// （含数据区，线程恢复可能仍在进行，故意不释放以保稳定）。
bool HijackThreadCall(HANDLE process, DWORD pid, uintptr_t fn,
                      uintptr_t a1, uintptr_t a2, uintptr_t a3,
                      DWORD waitMs, uintptr_t& outCodeBuf,
                      std::wstring& err) {
    outCodeBuf = 0;
    bool wow64 = false;
    if (!detail::TargetIsWow64(process, wow64, err)) return false;
    if (wow64) {
        err = L"线程劫持暂仅支持本机架构（x64->x64）；WOW64 目标请使用 APC/经典注入";
        return false;
    }

    uintptr_t getLastError = detail::ResolveRemoteProcAddress(process, pid,
        L"kernel32.dll", "GetLastError", err);
    if (!getLastError) return false;
    uintptr_t setCtx = detail::ResolveRemoteProcAddress(process, pid,
        L"ntdll.dll", "NtSetContextThread", err);
    if (!setCtx) return false;

    // 调试隔离：QST_HIJACK_NOOP=1 时只调用 GetCurrentProcessId
    bool noop = false;
    {
        char v[8]{};
        if (GetEnvironmentVariableA("QST_HIJACK_NOOP", v, 8) > 0 && v[0] == '1') {
            noop = true;
        }
    }
    if (noop) {
        fn = detail::ResolveRemoteProcAddress(process, pid,
            L"kernel32.dll", "GetCurrentProcessId", err);
        if (!fn) return false;
        a1 = a2 = a3 = 0;
    }

    std::vector<ThreadEntry> threads;
    if (!CollectCandidateThreads(pid, threads, err)) return false;

    // 优先级：主模块内非 UI 工作线程（短等待）> 任意非 UI 线程 > 任意线程
    ThreadEntry* chosen = nullptr;
    for (int pass = 0; pass < 3 && !chosen; ++pass) {
        for (auto& t : threads) {
            if (pass == 0 && !(t.startInTargetExe && !t.ownsWindow)) continue;
            if (pass == 1 && t.ownsWindow) continue;
            if (t.handle == nullptr) continue;
            if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
                CloseHandle(t.handle);
                t.handle = nullptr;
                continue;
            }
            t.suspended = true;
            t.saved.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(t.handle, &t.saved)) {
                chosen = &t;
                break;
            }
            ResumeThread(t.handle);
            t.suspended = false;
            CloseHandle(t.handle);
            t.handle = nullptr;
        }
    }
    if (!chosen) {
        CloseThreads(threads);
        err = L"无法挂起/读取任何目标线程上下文";
        return false;
    }

    const uintptr_t origIp = chosen->saved.Rip;
    // 数据区：保存的 CONTEXT + result/error/started 三个 8 字节槽（读写）
    const size_t dataSize = sizeof(CONTEXT) + 3 * sizeof(uint64_t);
    // 代码区：桩字节（写入后转 PAGE_EXECUTE_READ，避免 RWX 痕迹）
    constexpr size_t kCodeBytes = 0x1000;
    void* dataBuf = VirtualAllocEx(process, nullptr, dataSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* codeBuf = VirtualAllocEx(process, nullptr, kCodeBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!dataBuf || !codeBuf) {
        if (dataBuf) VirtualFreeEx(process, dataBuf, 0, MEM_RELEASE);
        if (codeBuf) VirtualFreeEx(process, codeBuf, 0, MEM_RELEASE);
        ResumeThread(chosen->handle);
        chosen->suspended = false;
        CloseThreads(threads);
        err = L"VirtualAllocEx(劫持区) 失败: " + detail::WinErrorText(GetLastError());
        return false;
    }
    const uintptr_t db = reinterpret_cast<uintptr_t>(dataBuf);
    const uintptr_t ctxAddr = db;
    const uintptr_t resultAddr = db + sizeof(CONTEXT);
    const uintptr_t errorAddr = resultAddr + sizeof(uint64_t);
    const uintptr_t startedAddr = errorAddr + sizeof(uint64_t);

    std::vector<uint8_t> stub;
    if (!detail::BuildHijackStub(true, fn, a1, a2, a3, ctxAddr, setCtx,
                                 origIp, startedAddr, resultAddr, errorAddr,
                                 getLastError, stub, err)) {
        VirtualFreeEx(process, dataBuf, 0, MEM_RELEASE);
        VirtualFreeEx(process, codeBuf, 0, MEM_RELEASE);
        ResumeThread(chosen->handle);
        chosen->suspended = false;
        CloseThreads(threads);
        return false;
    }
    if (stub.size() > kCodeBytes) {
        VirtualFreeEx(process, dataBuf, 0, MEM_RELEASE);
        VirtualFreeEx(process, codeBuf, 0, MEM_RELEASE);
        ResumeThread(chosen->handle);
        chosen->suspended = false;
        CloseThreads(threads);
        err = L"劫持桩超长";
        return false;
    }

    uint64_t zero = 0;
    bool ok = detail::WriteRemoteBytes(process, dataBuf,
        &chosen->saved, sizeof(CONTEXT), err);
    ok = ok && detail::WriteRemoteBytes(process,
        reinterpret_cast<void*>(resultAddr), &zero, sizeof(zero), err);
    ok = ok && detail::WriteRemoteBytes(process,
        reinterpret_cast<void*>(errorAddr), &zero, sizeof(zero), err);
    ok = ok && detail::WriteRemoteBytes(process,
        reinterpret_cast<void*>(startedAddr), &zero, sizeof(zero), err);
    ok = ok && detail::WriteRemoteBytes(process, codeBuf,
        stub.data(), stub.size(), err);
    if (!ok) {
        VirtualFreeEx(process, dataBuf, 0, MEM_RELEASE);
        VirtualFreeEx(process, codeBuf, 0, MEM_RELEASE);
        ResumeThread(chosen->handle);
        chosen->suspended = false;
        CloseThreads(threads);
        return false;
    }
    DWORD oldProtect = 0;
    VirtualProtectEx(process, codeBuf, kCodeBytes, PAGE_EXECUTE_READ, &oldProtect);

    CONTEXT hijacked = chosen->saved;
    hijacked.Rip = reinterpret_cast<uintptr_t>(codeBuf);
    hijacked.ContextFlags = CONTEXT_FULL;
    if (!SetThreadContext(chosen->handle, &hijacked)) {
        VirtualFreeEx(process, dataBuf, 0, MEM_RELEASE);
        VirtualFreeEx(process, codeBuf, 0, MEM_RELEASE);
        ResumeThread(chosen->handle);
        chosen->suspended = false;
        CloseThreads(threads);
        err = L"SetThreadContext 失败: " + detail::WinErrorText(GetLastError());
        return false;
    }
    const DWORD resumeRet = ResumeThread(chosen->handle);
    chosen->suspended = false;

    // 等待 fn 返回（result 槽非零）
    const DWORD deadline = GetTickCount() + waitMs;
    uint64_t result = 0;
    while (GetTickCount() < deadline) {
        uint64_t r = 0;
        if (detail::ReadRemoteBytes(process,
                reinterpret_cast<const void*>(resultAddr), &r, sizeof(r), err) &&
            r != 0) {
            result = r;
            break;
        }
        Sleep(50);
    }
    CloseThreads(threads);
    if (result == 0) {
        uint64_t started = 0, loadErr = 0;
        detail::ReadRemoteBytes(process,
            reinterpret_cast<const void*>(startedAddr), &started, sizeof(started), err);
        detail::ReadRemoteBytes(process,
            reinterpret_cast<const void*>(errorAddr), &loadErr, sizeof(loadErr), err);
        uintptr_t rip = 0;
        CONTEXT probe{};
        probe.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(chosen->handle, &probe)) {
            rip = static_cast<uintptr_t>(probe.Rip);
        }
        wchar_t buf[512]{};
        swprintf_s(buf,
            L"线程劫持调用未完成（tid=%lu resumeRet=%lu rip=0x%p stub=0x%p "
            L"started=0x%llX result=0x%llX error=%lu）",
            static_cast<unsigned long>(chosen->tid),
            static_cast<unsigned long>(resumeRet),
            reinterpret_cast<void*>(rip), codeBuf,
            static_cast<unsigned long long>(started),
            static_cast<unsigned long long>(result),
            static_cast<unsigned long>(loadErr));
        err = buf;
        // 保留缓冲：线程可能仍在恢复，立即释放有崩溃风险
        return false;
    }

    outCodeBuf = reinterpret_cast<uintptr_t>(codeBuf);
    return true;
}

bool InjectLoadLibraryThreadHijack(HANDLE process, DWORD pid,
                                   const std::wstring& dllPath,
                                   HMODULE& outModule, std::wstring& err) {
    outModule = nullptr;
    std::wstring baseName = detail::BaseNameOnly(dllPath);
    std::transform(baseName.begin(), baseName.end(), baseName.begin(), ::towlower);
    outModule = detail::FindRemoteModule(pid, baseName);
    if (outModule) return true;

    uintptr_t loadLib = detail::ResolveRemoteProcAddress(process, pid,
        L"kernel32.dll", "LoadLibraryW", err);
    if (!loadLib) return false;

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* pathBuf = VirtualAllocEx(process, nullptr, bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pathBuf) {
        err = L"VirtualAllocEx(路径) 失败: " + detail::WinErrorText(GetLastError());
        return false;
    }
    if (!detail::WriteRemoteBytes(process, pathBuf, dllPath.c_str(), bytes, err)) {
        VirtualFreeEx(process, pathBuf, 0, MEM_RELEASE);
        return false;
    }

    uintptr_t codeBuf = 0;
    const bool ok = HijackThreadCall(process, pid, loadLib,
        reinterpret_cast<uintptr_t>(pathBuf), 0, 0, 15000, codeBuf, err);
    if (ok) {
        outModule = detail::FindRemoteModule(pid, baseName);
    }
    VirtualFreeEx(process, pathBuf, 0, MEM_RELEASE);
    if (!outModule) {
        if (err.empty()) err = L"线程劫持后未发现模块（LoadLibraryW 可能返回失败）";
        return false;
    }
    return true;
}

}  // namespace inject
}  // namespace windowmode
