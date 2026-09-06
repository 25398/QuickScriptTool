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

    // 窗口 PID 必须先注入：子进程（Java helper 等）没有消息泵时
    // setwindowshook 会先失败，日志还把主进程误记成「子进程跳过」。
    std::vector<DWORD> ordered;
    ordered.reserve(pids.size());
    if (windowPid != 0 && pids.count(windowPid)) {
        ordered.push_back(windowPid);
    }
    for (DWORD p : pids) {
        if (p != windowPid) ordered.push_back(p);
    }
    return ordered;
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

bool FakeFocusInjector::RemoteCallExport(Target& t, const char* exportName, HWND arg,
    std::wstring& err) {
    if (!t.process || !t.remoteModule || !exportName) {
        err = L"假焦点远程调用未就绪";
        return false;
    }
    DWORD exitCode = 0;
    const std::wstring& path = !t.dllPath.empty() ? t.dllPath : dllPath_;
    if (!inject::CallRemoteExport(t.process, t.pid, t.remoteModule, path,
                                  exportName, reinterpret_cast<void*>(arg),
                                  10000, &exitCode, err)) {
        return false;
    }
    if (exitCode == 0) {
        err = std::wstring(L"假焦点导出返回失败: ")
            + std::wstring(exportName, exportName + std::strlen(exportName));
        return false;
    }
    return true;
}

bool FakeFocusInjector::InjectOne(DWORD pid, HWND targetTop, const std::wstring& dllPath,
    std::wstring& err, bool lite) {
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

    inject::InjectOptions opts;
    opts.hideModule = hideModule_;
    opts.targetTop = targetTop;
    opts.hookProcName = "FakeFocus_HookProc";
    inject::InjectResult injectResult;
    if (!inject::InjectDll(pid, path, technique_, opts, injectResult)) {
        const std::wstring firstErr = injectResult.detail;
        inject::Technique used = technique_;
        bool recovered = false;
        // GLFW/Minecraft 等失焦后不泵消息：setwindowshook 等不到 DLL，改走远程线程。
        if (technique_ == inject::Technique::SetWindowsHook) {
            WindowModeLogf(
                L"[窗口模式] setwindowshook 未装入 DLL（%s），改试 classic",
                firstErr.c_str());
            used = inject::Technique::ClassicRemoteThread;
            recovered = inject::InjectDll(pid, path, used, opts, injectResult);
            if (!recovered) {
                DWORD procCode = 0;
                const bool dead = t.process
                    && GetExitCodeProcess(t.process, &procCode)
                    && procCode != STILL_ACTIVE;
                if (dead || (targetTop && !IsWindow(targetTop))) {
                    err = injectResult.detail.empty() ? firstErr
                        : (injectResult.detail + L"（目标已退出，停止继续注入）");
                    CloseHandle(t.process);
                    return false;
                }
                WindowModeLogf(
                    L"[窗口模式] classic 仍失败（%s），改试 ntcreatethreadex",
                    injectResult.detail.c_str());
                used = inject::Technique::NtCreateThreadEx;
                recovered = inject::InjectDll(pid, path, used, opts, injectResult);
            }
        }
        if (!recovered) {
            err = firstErr.empty() ? injectResult.detail : firstErr;
            CloseHandle(t.process);
            return false;
        }
        WindowModeLogf(L"[窗口模式] 假焦点注入已回退 tech=%s pid=%lu",
            inject::TechniqueName(used), static_cast<unsigned long>(pid));
    }
    t.remoteModule = injectResult.remoteModule;
    t.moduleHidden = injectResult.moduleHidden;
    t.hideState = injectResult.hideState;
    t.dllPath = path;

    HWND top = GetAncestor(targetTop, GA_ROOT);
    if (!top) top = targetTop;
    const char* primary = lite ? "FakeFocus_InstallLite" : "FakeFocus_Install";
    const char* fallback = lite ? "FakeFocus_Install" : "FakeFocus_InstallLite";
    if (!RemoteCallExport(t, primary, top, err)) {
        std::wstring fbErr;
        if (RemoteCallExport(t, fallback, top, fbErr)) {
            WindowModeLogf(
                L"[窗口模式] 假焦点导出 %hs 失败（%s），已改用 %hs",
                primary, err.c_str(), fallback);
            err.clear();
        } else {
            // 尽力卸载已加载模块
            HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            auto* freeLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                GetProcAddress(k32, "FreeLibrary"));
            if (freeLib && t.remoteModule) {
                HANDLE thread = CreateRemoteThread(t.process, nullptr, 0, freeLib,
                    t.remoteModule, 0, nullptr);
                if (thread) {
                    WaitForSingleObject(thread, 5000);
                    CloseHandle(thread);
                }
            }
            CloseHandle(t.process);
            return false;
        }
    }
    t.installed = true;
    dllPath_ = path;
    targets_.push_back(t);
    return true;
}

void FakeFocusInjector::UnloadOne(Target& t) {
    if (!t.process) return;
    std::wstring ignore;
    if (t.moduleHidden && t.remoteModule) {
        inject::RestoreModuleFromPeb(t.process, t.pid, t.remoteModule,
                                     t.hideState, ignore);
        t.moduleHidden = false;
    }
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

bool FakeFocusInjector::InjectAndInstall(DWORD windowPid, HWND targetTop, std::wstring& err,
    bool lite, bool windowPidOnly) {
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

    WindowModeLogf(L"[窗口模式] 假焦点注入技术=%s hideModule=%d windowPidOnly=%d lite=%d",
        inject::TechniqueName(technique_), hideModule_ ? 1 : 0,
        windowPidOnly ? 1 : 0, lite ? 1 : 0);

    const std::vector<DWORD> pids = windowPidOnly
        ? std::vector<DWORD>{windowPid}
        : CollectInjectPids(windowPid, targetTop);
    std::wstring firstErr;
    int okCount = 0;
    for (DWORD pid : pids) {
        std::wstring oneErr;
        if (InjectOne(pid, targetTop, dllPath_, oneErr, lite)) {
            ++okCount;
            WindowModeLogf(L"[窗口模式] 假焦点已注入 pid=%lu hwnd=0x%p lite=%d",
                static_cast<unsigned long>(pid), targetTop, lite ? 1 : 0);
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

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(dllPath_.c_str(), GetFileExInfoStandard, &fad)) {
        const ULONGLONG bytes =
            (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        SYSTEMTIME utc{}, local{};
        FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
        if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
            local = utc;
        }
        WindowModeLogf(
            L"[窗口模式] 假焦点注入完成 processes=%d windowPid=%lu dll=%s size=%llu "
            L"time=%04u-%02u-%02u %02u:%02u:%02u tech=%s",
            okCount, static_cast<unsigned long>(windowPid), dllPath_.c_str(),
            bytes,
            static_cast<unsigned>(local.wYear), static_cast<unsigned>(local.wMonth),
            static_cast<unsigned>(local.wDay), static_cast<unsigned>(local.wHour),
            static_cast<unsigned>(local.wMinute), static_cast<unsigned>(local.wSecond),
            inject::TechniqueName(technique_));
    } else {
        WindowModeLogf(L"[窗口模式] 假焦点注入完成 processes=%d windowPid=%lu dll=%s tech=%s",
            okCount, static_cast<unsigned long>(windowPid), dllPath_.c_str(),
            inject::TechniqueName(technique_));
    }
    return true;
}

bool FakeFocusInjector::QueryMapleIatCount(DWORD& count, std::wstring& err) {
    count = 0;
    err.clear();
    for (auto& t : targets_) {
        if (!t.installed || !t.process || !t.remoteModule) continue;
        const std::wstring& path = !t.dllPath.empty() ? t.dllPath : dllPath_;
        DWORD exitCode = 0;
        if (inject::CallRemoteExport(t.process, t.pid, t.remoteModule, path,
                "FakeFocus_MapleIatCount", nullptr, 5000, &exitCode, err)) {
            count = exitCode;
            err.clear();
            return true;
        }
    }
    if (err.empty()) err = L"假焦点 MapleIatCount 不可用";
    return false;
}

bool FakeFocusInjector::QueryMapleHookHits(DWORD& hits, std::wstring& err) {
    hits = 0;
    err.clear();
    for (auto& t : targets_) {
        if (!t.installed || !t.process || !t.remoteModule) continue;
        const std::wstring& path = !t.dllPath.empty() ? t.dllPath : dllPath_;
        DWORD exitCode = 0;
        if (inject::CallRemoteExport(t.process, t.pid, t.remoteModule, path,
                "FakeFocus_MapleHookHits", nullptr, 5000, &exitCode, err)) {
            hits = exitCode;
            err.clear();
            return true;
        }
    }
    if (err.empty()) err = L"假焦点 MapleHookHits 不可用";
    return false;
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
