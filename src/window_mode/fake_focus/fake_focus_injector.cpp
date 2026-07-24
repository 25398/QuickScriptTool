#include "fake_focus_injector.h"
#include "fake_focus_soft_input_host.h"

#include "window_mode/window_mode_log.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace windowmode {
namespace {

std::wstring FileNameOnly(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return path;
    return path.substr(slash + 1);
}

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring full(path);
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return full.substr(0, slash + 1);
}

bool PathExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES
        && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsTargetWow64(HANDLE process, bool& outWow64, std::wstring& err) {
    outWow64 = false;
#if defined(_WIN64)
    BOOL wow = FALSE;
    if (!IsWow64Process(process, &wow)) {
        err = L"无法判断目标进程架构 (IsWow64Process 失败)";
        return false;
    }
    outWow64 = wow == TRUE;
    return true;
#else
    (void)process;
    outWow64 = true;
    return true;
#endif
}

std::wstring FormatWinError(DWORD code) {
    wchar_t buf[256]{};
    swprintf_s(buf, L"Win32=%lu", static_cast<unsigned long>(code));
    return buf;
}

void CollectChildProcessIds(DWORD parentPid, std::set<DWORD>& out) {
    if (parentPid == 0) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID == parentPid && pe.th32ProcessID != 0) {
                out.insert(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

}  // namespace

std::vector<DWORD> FakeFocusInjector::CollectInjectPids(DWORD windowPid, HWND targetTop) {
    std::set<DWORD> pids;
    if (windowPid != 0) pids.insert(windowPid);

    if (targetTop && IsWindow(targetTop)) {
        DWORD topPid = 0;
        GetWindowThreadProcessId(targetTop, &topPid);
        if (topPid != 0) pids.insert(topPid);

        struct EnumCtx {
            std::set<DWORD>* pids = nullptr;
        } ctx{&pids};
        EnumChildWindows(targetTop, [](HWND w, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<EnumCtx*>(lp);
            DWORD p = 0;
            GetWindowThreadProcessId(w, &p);
            if (p != 0) c->pids->insert(p);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    }

    // Edge/Chrome：渲染/插件常为窗口进程的子进程，窗口树里不一定有 HWND。
    const DWORD root = windowPid != 0 ? windowPid
        : (pids.empty() ? 0 : *pids.begin());
    if (root != 0) {
        CollectChildProcessIds(root, pids);
        // 再扩一层：部分 Flash/GPU 挂在渲染子进程下
        std::set<DWORD> grand;
        for (DWORD p : pids) {
            if (p == root) continue;
            CollectChildProcessIds(p, grand);
        }
        pids.insert(grand.begin(), grand.end());
    }

    return std::vector<DWORD>(pids.begin(), pids.end());
}

FakeFocusInjector::~FakeFocusInjector() {
    Unload();
}

bool FakeFocusInjector::ResolveDllPathForPid(DWORD pid, std::wstring& outPath, std::wstring& err) const {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        err = L"打开目标进程失败（查询架构）: " + FormatWinError(GetLastError());
        return false;
    }
    bool wow64 = false;
    const bool ok = IsTargetWow64(process, wow64, err);
    CloseHandle(process);
    if (!ok) return false;

#if defined(_WIN64)
    const wchar_t* dllName = wow64 ? L"FakeFocus32.dll" : L"FakeFocus64.dll";
#else
    if (!wow64) {
        err = L"当前为 32 位主程序，无法向 64 位目标注入假焦点";
        return false;
    }
    const wchar_t* dllName = L"FakeFocus32.dll";
#endif

    const std::wstring dir = ModuleDirectory();
    if (dir.empty()) {
        err = L"无法解析主程序目录以定位假焦点 DLL";
        return false;
    }
    outPath = dir + dllName;
    if (!PathExists(outPath)) {
        err = std::wstring(L"找不到假焦点 DLL: ") + outPath;
        return false;
    }
    return true;
}

HMODULE FakeFocusInjector::FindRemoteModule(DWORD pid, HANDLE process,
    const std::wstring& dllPath) const {
    (void)process;
    if (pid == 0) return nullptr;
    const std::wstring want = FileNameOnly(dllPath);
    std::wstring wantLower = want;
    std::transform(wantLower.begin(), wantLower.end(), wantLower.begin(), ::towlower);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    HMODULE found = nullptr;
    if (Module32FirstW(snap, &me)) {
        do {
            std::wstring name = me.szModule;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name == wantLower) {
                found = me.hModule;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

bool FakeFocusInjector::RemoteLoadLibrary(Target& t, const std::wstring& dllPath, std::wstring& err) {
    t.remoteModule = FindRemoteModule(t.pid, t.process, dllPath);
    if (t.remoteModule) return true;

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteBuf = VirtualAllocEx(t.process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf) {
        err = L"VirtualAllocEx 失败: " + FormatWinError(GetLastError());
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(t.process, remoteBuf, dllPath.c_str(), bytes, &written)) {
        VirtualFreeEx(t.process, remoteBuf, 0, MEM_RELEASE);
        err = L"WriteProcessMemory 失败: " + FormatWinError(GetLastError());
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto* loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(k32, "LoadLibraryW"));
    if (!loadLib) {
        VirtualFreeEx(t.process, remoteBuf, 0, MEM_RELEASE);
        err = L"GetProcAddress(LoadLibraryW) 失败";
        return false;
    }

    HANDLE thread = CreateRemoteThread(t.process, nullptr, 0, loadLib, remoteBuf, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(t.process, remoteBuf, 0, MEM_RELEASE);
        const DWORD code = GetLastError();
        err = L"CreateRemoteThread(LoadLibraryW) 失败: " + FormatWinError(code);
        if (code == ERROR_ACCESS_DENIED) {
            err += L"（权限不足；若目标更高完整性级别请以管理员运行）";
        }
        return false;
    }

    WaitForSingleObject(thread, 15000);
    CloseHandle(thread);
    VirtualFreeEx(t.process, remoteBuf, 0, MEM_RELEASE);

    t.remoteModule = FindRemoteModule(t.pid, t.process, dllPath);
    if (!t.remoteModule) {
        err = L"LoadLibraryW 注入后未找到模块（可能架构不匹配或 DLL 依赖缺失）";
        return false;
    }
    return true;
}

bool FakeFocusInjector::RemoteCallExport(Target& t, const char* exportName, HWND arg,
    std::wstring& err) {
    if (!t.process || !t.remoteModule || !exportName) {
        err = L"假焦点远程调用未就绪";
        return false;
    }

    HMODULE local = LoadLibraryW(dllPath_.c_str());
    if (!local) {
        err = L"本地加载假焦点 DLL 失败: " + FormatWinError(GetLastError());
        return false;
    }

    FARPROC localProc = GetProcAddress(local, exportName);
    if (!localProc) {
        FreeLibrary(local);
        err = std::wstring(L"假焦点 DLL 缺少导出: ")
            + std::wstring(exportName, exportName + std::strlen(exportName));
        return false;
    }

    const auto localBase = reinterpret_cast<BYTE*>(local);
    const auto localFn = reinterpret_cast<BYTE*>(localProc);
    const ptrdiff_t rva = localFn - localBase;
    FreeLibrary(local);

    auto* remoteFn = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<BYTE*>(t.remoteModule) + rva);

    HANDLE thread = CreateRemoteThread(t.process, nullptr, 0, remoteFn,
        reinterpret_cast<LPVOID>(arg), 0, nullptr);
    if (!thread) {
        err = L"CreateRemoteThread(假焦点导出) 失败: " + FormatWinError(GetLastError());
        return false;
    }
    WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);

    if (exitCode == 0) {
        err = std::wstring(L"假焦点导出返回失败: ")
            + std::wstring(exportName, exportName + std::strlen(exportName));
        return false;
    }
    return true;
}

bool FakeFocusInjector::InjectOne(DWORD pid, HWND targetTop, const std::wstring& dllPath,
    std::wstring& err) {
    Target t{};
    t.pid = pid;
    t.process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
            | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
        FALSE, pid);
    if (!t.process) {
        err = L"打开目标进程失败: " + FormatWinError(GetLastError());
        return false;
    }

    // 架构与主 DLL 不一致时换对侧 DLL。
    std::wstring path = dllPath;
    std::wstring pathErr;
    if (!ResolveDllPathForPid(pid, path, pathErr)) {
        CloseHandle(t.process);
        err = pathErr;
        return false;
    }

    if (!RemoteLoadLibrary(t, path, err)) {
        CloseHandle(t.process);
        return false;
    }

    HWND top = GetAncestor(targetTop, GA_ROOT);
    if (!top) top = targetTop;
    if (!RemoteCallExport(t, "FakeFocus_Install", top, err)) {
        // 尽力卸载已加载模块
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        auto* freeLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(k32, "FreeLibrary"));
        if (freeLib && t.remoteModule) {
            HANDLE thread = CreateRemoteThread(t.process, nullptr, 0, freeLib, t.remoteModule, 0, nullptr);
            if (thread) {
                WaitForSingleObject(thread, 5000);
                CloseHandle(thread);
            }
        }
        CloseHandle(t.process);
        return false;
    }
    t.installed = true;
    dllPath_ = path;
    targets_.push_back(t);
    return true;
}

void FakeFocusInjector::UnloadOne(Target& t) {
    if (!t.process) return;
    std::wstring ignore;
    if (t.installed && t.remoteModule) {
        RemoteCallExport(t, "FakeFocus_Uninstall", nullptr, ignore);
        t.installed = false;
    }
    if (t.remoteModule) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        auto* freeLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(k32, "FreeLibrary"));
        if (freeLib) {
            HANDLE thread = CreateRemoteThread(t.process, nullptr, 0, freeLib, t.remoteModule, 0, nullptr);
            if (thread) {
                WaitForSingleObject(thread, 5000);
                CloseHandle(thread);
            }
        }
        t.remoteModule = nullptr;
    }
    CloseHandle(t.process);
    t.process = nullptr;
}

bool FakeFocusInjector::InjectAndInstall(DWORD windowPid, HWND targetTop, std::wstring& err) {
    Unload();
    err.clear();
    lastError_.clear();

    if (windowPid == 0 || !targetTop || !IsWindow(targetTop)) {
        err = L"假焦点注入参数无效";
        lastError_ = err;
        return false;
    }

    windowPid_ = windowPid;
    std::wstring softErr;
    if (!FakeFocusSoftInput_Attach(windowPid, softErr)) {
        err = softErr.empty() ? L"假焦点软输入共享内存创建失败" : softErr;
        lastError_ = err;
        return false;
    }

    if (!ResolveDllPathForPid(windowPid, dllPath_, err)) {
        lastError_ = err;
        FakeFocusSoftInput_Detach();
        return false;
    }

    const std::vector<DWORD> pids = CollectInjectPids(windowPid, targetTop);
    std::wstring firstErr;
    int okCount = 0;
    for (DWORD pid : pids) {
        std::wstring oneErr;
        if (InjectOne(pid, targetTop, dllPath_, oneErr)) {
            ++okCount;
            WindowModeLogf(L"[窗口模式] 假焦点已注入 pid=%lu hwnd=0x%p",
                static_cast<unsigned long>(pid), targetTop);
        } else if (firstErr.empty()) {
            firstErr = oneErr;
        } else {
            WindowModeLogf(L"[窗口模式] 假焦点子进程注入跳过 pid=%lu: %s",
                static_cast<unsigned long>(pid), oneErr.c_str());
        }
    }

    if (okCount == 0) {
        err = firstErr.empty() ? L"假焦点注入失败" : firstErr;
        lastError_ = err;
        Unload();
        return false;
    }

    // 主窗口进程必须成功；若只注入到无关子进程则仍算失败。
    bool hasWindowPid = false;
    for (const auto& t : targets_) {
        if (t.pid == windowPid) {
            hasWindowPid = true;
            break;
        }
    }
    if (!hasWindowPid) {
        err = firstErr.empty()
            ? L"假焦点未能注入目标窗口进程"
            : (L"假焦点未能注入目标窗口进程: " + firstErr);
        lastError_ = err;
        Unload();
        return false;
    }

    WindowModeLogf(L"[窗口模式] 假焦点注入完成 processes=%d windowPid=%lu dll=%s",
        okCount, static_cast<unsigned long>(windowPid), dllPath_.c_str());
    return true;
}

bool FakeFocusInjector::UpdateTarget(HWND targetTop, std::wstring& err) {
    if (targets_.empty()) {
        err = L"假焦点尚未注入";
        return false;
    }
    if (!targetTop || !IsWindow(targetTop)) {
        err = L"假焦点 UpdateTarget 窗口无效";
        return false;
    }
    HWND top = GetAncestor(targetTop, GA_ROOT);
    if (!top) top = targetTop;

    bool any = false;
    std::wstring last;
    for (auto& t : targets_) {
        if (!t.installed) continue;
        std::wstring oneErr;
        if (RemoteCallExport(t, "FakeFocus_UpdateTarget", top, oneErr)) {
            any = true;
        } else {
            last = oneErr;
        }
    }
    if (!any) {
        err = last.empty() ? L"假焦点 UpdateTarget 失败" : last;
        lastError_ = err;
        return false;
    }
    return true;
}

void FakeFocusInjector::Unload() {
    // 先清软按键，避免卸载后切到宏桌面时游戏仍读到残留 down 态。
    FakeFocusSoftInput_ClearKeys();
    FakeFocusSoftInput_Reset();
    for (auto& t : targets_) {
        UnloadOne(t);
    }
    targets_.clear();
    FakeFocusSoftInput_Detach();
    windowPid_ = 0;
    dllPath_.clear();
}

}  // namespace windowmode
