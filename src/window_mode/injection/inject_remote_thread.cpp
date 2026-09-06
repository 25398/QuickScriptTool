#include "inject_common.h"
#include "inject_techniques_internal.h"

#include <algorithm>
#include <cwctype>

namespace windowmode {
namespace inject {
namespace {

bool AllocatePathBuffer(HANDLE process, const std::wstring& dllPath,
                        void*& remoteBuf, std::wstring& err) {
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    remoteBuf = VirtualAllocEx(process, nullptr, bytes,
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf) {
        err = L"VirtualAllocEx 失败: " + detail::WinErrorText(GetLastError());
        return false;
    }
    if (!detail::WriteRemoteBytes(process, remoteBuf, dllPath.c_str(),
                                  bytes, err)) {
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        remoteBuf = nullptr;
        return false;
    }
    return true;
}

bool ResolveLoadLibraryW(HANDLE process, DWORD pid, uintptr_t& out,
                         std::wstring& err) {
    out = detail::ResolveRemoteProcAddress(process, pid,
        L"kernel32.dll", "LoadLibraryW", err);
    return out != 0;
}

bool ProcessStillRunning(HANDLE process) {
    if (!process) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(process, &code)) return false;
    return code == STILL_ACTIVE;
}

std::wstring LoadLibraryMissingModuleErr(HANDLE process, DWORD threadExit,
                                         const wchar_t* api) {
    if (!ProcessStillRunning(process)) {
        return std::wstring(api)
            + L" 期间目标进程已退出（注入把游戏打崩了？）";
    }
    if (threadExit == STILL_ACTIVE) {
        return std::wstring(api) + L" LoadLibrary 超时（目标可能已卡死）";
    }
    if (threadExit == 0) {
        return std::wstring(api)
            + L"：LoadLibraryW 返回空。"
            L"若 Windows 安全中心提示「无法确认谁发布了 FakeFocus*.dll」"
            L"（智能应用控制拦截注入 Java/游戏），到「保护历史记录」允许该文件，"
            L"完全退出目标程序后再试；否则才是架构不匹配或 DLL 依赖缺失";
    }
    return std::wstring(api) + L" 注入后未发现模块（架构不匹配或 DLL 依赖缺失）";
}

}  // namespace

bool InjectLoadLibraryRemoteThread(HANDLE process, DWORD pid,
                                   const std::wstring& dllPath,
                                   HMODULE& outModule, std::wstring& err) {
    outModule = nullptr;
    std::wstring baseName = detail::BaseNameOnly(dllPath);
    std::transform(baseName.begin(), baseName.end(), baseName.begin(), ::towlower);
    outModule = detail::FindRemoteModule(pid, baseName);
    if (outModule) return true;

    uintptr_t loadLib = 0;
    if (!ResolveLoadLibraryW(process, pid, loadLib, err)) return false;

    void* remoteBuf = nullptr;
    if (!AllocatePathBuffer(process, dllPath, remoteBuf, err)) return false;

    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib), remoteBuf, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        err = L"CreateRemoteThread(LoadLibraryW) 失败: "
            + detail::WinErrorText(GetLastError());
        return false;
    }
    WaitForSingleObject(thread, 15000);
    DWORD threadExit = 0;
    GetExitCodeThread(thread, &threadExit);
    CloseHandle(thread);
    VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);

    outModule = detail::FindRemoteModule(pid, baseName);
    if (!outModule) {
        err = LoadLibraryMissingModuleErr(process, threadExit, L"CreateRemoteThread");
        return false;
    }
    return true;
}

bool InjectLoadLibraryNtCreateThreadEx(HANDLE process, DWORD pid,
                                       const std::wstring& dllPath,
                                       HMODULE& outModule, std::wstring& err) {
    outModule = nullptr;
    std::wstring baseName = detail::BaseNameOnly(dllPath);
    std::transform(baseName.begin(), baseName.end(), baseName.begin(), ::towlower);
    outModule = detail::FindRemoteModule(pid, baseName);
    if (outModule) return true;

    uintptr_t loadLib = 0;
    if (!ResolveLoadLibraryW(process, pid, loadLib, err)) return false;

    void* remoteBuf = nullptr;
    if (!AllocatePathBuffer(process, dllPath, remoteBuf, err)) return false;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    typedef LONG(NTAPI* NtCreateThreadEx_t)(
        PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID, PVOID,
        ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
    auto* ntCreateThreadEx = reinterpret_cast<NtCreateThreadEx_t>(
        GetProcAddress(ntdll, "NtCreateThreadEx"));
    if (!ntCreateThreadEx) {
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        err = L"无法解析 NtCreateThreadEx";
        return false;
    }

    HANDLE thread = nullptr;
    const LONG status = ntCreateThreadEx(&thread, THREAD_ALL_ACCESS, nullptr,
        process, reinterpret_cast<PVOID>(loadLib), remoteBuf, 0, 0, 0, 0, nullptr);
    if (status < 0 || !thread) {
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        wchar_t buf[128]{};
        swprintf_s(buf, L"NtCreateThreadEx 失败: 0x%08X", static_cast<unsigned>(status));
        err = buf;
        return false;
    }
    WaitForSingleObject(thread, 15000);
    DWORD threadExit = 0;
    GetExitCodeThread(thread, &threadExit);
    CloseHandle(thread);
    VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);

    outModule = detail::FindRemoteModule(pid, baseName);
    if (!outModule) {
        err = LoadLibraryMissingModuleErr(process, threadExit, L"NtCreateThreadEx");
        return false;
    }
    return true;
}

}  // namespace inject
}  // namespace windowmode
