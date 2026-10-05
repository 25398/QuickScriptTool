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
                          HMODULE& outModule, HMODULE& outHookModule,
                          void*& outHookHandle, std::wstring& err) {
    outModule = nullptr;
    outHookModule = nullptr;
    outHookHandle = nullptr;
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
    if (outModule) {
        // 目标里已经有这个模块：本轮**不再装钩**（重复装钩 = 双重挂钩）。
        // 也正因为没装钩，这里没有可归还的 HHOOK / 本地模块 —— 卸载走远程 FreeLibrary。
        return true;
    }

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
        // 旧实现这里直接 return ⇒ 本地模块引用永久 +1（本进程也就一直锁着这个文件）。
        FreeLibrary(local);
        return false;
    }

    HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE,
        reinterpret_cast<HOOKPROC>(proc), local, threadId);
    if (!hook) {
        err = L"SetWindowsHookEx 失败: " + detail::WinErrorText(GetLastError());
        FreeLibrary(local);
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
        FreeLibrary(local);
        err = L"SetWindowsHookEx 后目标进程未出现钩子 DLL（目标线程未处理消息？）";
        return false;
    }
    outModule = found;
    // ⚠ 这两样必须交回调用方：HHOOK 决定「钩子还在不在」（钩在 ⇒ user32 钉住 DLL），
    // 本地模块引用决定「我们进程自己锁不锁这个文件」。旧实现在这里丢掉句柄，
    // 于是**永远拆不掉**这个钩：DLL 常驻目标进程 = 文件锁到目标退出/重启电脑。
    outHookModule = local;
    outHookHandle = hook;
    return true;
}

}  // namespace inject
}  // namespace windowmode
