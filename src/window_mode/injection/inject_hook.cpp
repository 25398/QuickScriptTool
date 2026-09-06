#include "inject_common.h"
#include "inject_techniques_internal.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>

namespace windowmode {
namespace inject {
namespace {

DWORD FindUiThreadId(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    DWORD first = 0;
    DWORD withWindow = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (!first) first = te.th32ThreadID;
            bool hasWindow = false;
            EnumThreadWindows(te.th32ThreadID, [](HWND, LPARAM lp) -> BOOL {
                    *reinterpret_cast<bool*>(lp) = true;
                    return FALSE;
                }, reinterpret_cast<LPARAM>(&hasWindow));
            if (hasWindow) {
                withWindow = te.th32ThreadID;
                break;
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return withWindow ? withWindow : first;
}

}  // namespace

bool InjectSetWindowsHook(HANDLE process, DWORD pid,
                          const std::wstring& dllPath,
                          const InjectOptions& opts,
                          HMODULE& outModule, std::wstring& err) {
    outModule = nullptr;
    if (opts.hookProcName.empty()) {
        err = L"SetWindowsHook 技术需要 hookProcName（DLL 导出的钩子过程名）";
        return false;
    }

    bool wow64 = false;
    if (!detail::TargetIsWow64(process, wow64, err)) return false;
    if (wow64) {
        err = L"SetWindowsHook 注入暂仅支持本机架构（x64->x64）";
        return false;
    }

    std::wstring baseName = detail::BaseNameOnly(dllPath);
    std::transform(baseName.begin(), baseName.end(), baseName.begin(), ::towlower);
    outModule = detail::FindRemoteModule(pid, baseName);
    if (outModule) return true;

    DWORD threadId = opts.hookThreadId;
    if (threadId == 0 && opts.targetTop && IsWindow(opts.targetTop)) {
        DWORD winPid = 0;
        threadId = GetWindowThreadProcessId(opts.targetTop, &winPid);
        if (winPid != pid) threadId = 0;
    }
    if (threadId == 0) threadId = FindUiThreadId(pid);
    if (threadId == 0) {
        err = L"无法确定目标线程（无窗口线程）";
        return false;
    }

    HMODULE local = LoadLibraryW(dllPath.c_str());
    if (!local) {
        err = L"本地加载钩子 DLL 失败: " + detail::WinErrorText(GetLastError());
        return false;
    }
    FARPROC proc = GetProcAddress(local, opts.hookProcName.c_str());
    if (!proc) {
        wchar_t wide[128]{};
        MultiByteToWideChar(CP_ACP, 0, opts.hookProcName.c_str(), -1, wide, 128);
        err = std::wstring(L"钩子 DLL 未导出: ") + wide;
        return false;
    }

    HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE,
        reinterpret_cast<HOOKPROC>(proc), local, threadId);
    if (!hook) {
        err = L"SetWindowsHookEx 失败: " + detail::WinErrorText(GetLastError());
        return false;
    }

    // WH_GETMESSAGE 只有在目标线程下次 Get/PeekMessage 时才会把 DLL 装进去。
    // GLFW/Unity 等 3D 游戏失焦后常停泵或只 Peek 指定 HWND，线程消息进不去。
    // 同时往窗口队列和线程队列投 WM_NULL，强制泵一次。
    auto poke = [&]() {
        if (opts.targetTop && IsWindow(opts.targetTop)) {
            PostMessageW(opts.targetTop, WM_NULL, 0, 0);
            SendNotifyMessageW(opts.targetTop, WM_NULL, 0, 0);
        }
        PostThreadMessageW(threadId, WM_NULL, 0, 0);
    };
    poke();

    HMODULE found = nullptr;
    const DWORD deadline = GetTickCount() + 3000;
    do {
        found = detail::FindRemoteModule(pid, baseName);
        if (found) break;
        if (GetTickCount() >= deadline) break;
        poke();
        Sleep(40);
    } while (true);
    if (!found) {
        UnhookWindowsHookEx(hook);
        err = L"SetWindowsHookEx 后目标进程未出现钩子 DLL（目标线程未处理消息？）";
        return false;
    }
    outModule = found;
    return true;
}

}  // namespace inject
}  // namespace windowmode
