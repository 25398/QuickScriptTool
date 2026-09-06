#include "inject_common.h"
#include "inject_techniques_internal.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>

namespace windowmode {
namespace inject {
namespace {

std::vector<DWORD> EnumerateThreads(DWORD pid) {
    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return tids;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) tids.push_back(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return tids;
}

}  // namespace

bool InjectLoadLibraryApc(HANDLE process, DWORD pid,
                          const std::wstring& dllPath,
                          HMODULE& outModule, std::wstring& err) {
    outModule = nullptr;
    std::wstring baseName = detail::BaseNameOnly(dllPath);
    std::transform(baseName.begin(), baseName.end(), baseName.begin(), ::towlower);
    outModule = detail::FindRemoteModule(pid, baseName);
    if (outModule) return true;

    uintptr_t loadLib = 0;
    loadLib = detail::ResolveRemoteProcAddress(process, pid,
        L"kernel32.dll", "LoadLibraryW", err);
    if (!loadLib) return false;

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteBuf = VirtualAllocEx(process, nullptr, bytes,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf) {
        err = L"VirtualAllocEx 失败: " + detail::WinErrorText(GetLastError());
        return false;
    }
    if (!detail::WriteRemoteBytes(process, remoteBuf, dllPath.c_str(),
                                  bytes, err)) {
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        return false;
    }

    const std::vector<DWORD> tids = EnumerateThreads(pid);
    int queued = 0;
    for (DWORD tid : tids) {
        if (tid == GetCurrentThreadId()) continue;
        HANDLE thread = OpenThread(THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, tid);
        if (!thread) continue;
        if (QueueUserAPC(reinterpret_cast<PAPCFUNC>(loadLib), thread,
                         reinterpret_cast<ULONG_PTR>(remoteBuf))) {
            ++queued;
        }
        CloseHandle(thread);
    }
    if (queued == 0) {
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        err = L"无法向任何目标线程投递 APC（OpenThread/QueueUserAPC 失败）";
        return false;
    }

    // APC 需在目标线程进入可告警等待时执行
    const DWORD deadline = GetTickCount() + 15000;
    do {
        outModule = detail::FindRemoteModule(pid, baseName);
        if (outModule) break;
        if (GetTickCount() >= deadline) break;
        Sleep(80);
    } while (true);
    VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);

    if (!outModule) {
        wchar_t buf[256]{};
        swprintf_s(buf, L"APC 已投递到 %d 个线程，但目标线程未进入可告警等待（15s 超时）",
            queued);
        err = buf;
        return false;
    }
    return true;
}

}  // namespace inject
}  // namespace windowmode
