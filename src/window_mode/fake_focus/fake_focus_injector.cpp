#include "fake_focus_injector.h"
#include "fake_focus_soft_input_host.h"
#include "fake_focus_stage.h"

#include "window_mode/window_mode_log.h"
#include "window_mode/window_mode_types.h"

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

bool ReadFileStamp(const std::wstring& path, ULONGLONG& bytes, FILETIME& writeTime) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    bytes = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    writeTime = fad.ftLastWriteTime;
    return true;
}

/// 目标进程里是否已经挂着 FakeFocus 注入模块（上次注入的残留）。
/// 为什么必须查：同一个游戏进程反复注入 = 双重挂钩 —— IAT 槽被两套 detour 覆盖、
/// DI 方法体 JMP 叠加、卸载时按各自保存的原始字节回写。典型症状就是**注入后游戏立刻闪退**，
/// 而且日志里 `pid`/`hwnd` 长时间不变（用户一直没重启游戏）。
/// outPath 回传**完整路径**：同一进程里挂两份不同路径的 FakeFocus 才是最危险的，
/// 只比模块名看不出这一点（用户换过好几次部署目录）。
bool TargetHasStaleFakeFocusModule(DWORD pid, std::wstring& outPath) {
    outPath.clear();
    if (pid == 0) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"FakeFocus32.dll") == 0
                || _wcsicmp(me.szModule, L"FakeFocus64.dll") == 0) {
                outPath = me.szExePath[0] ? me.szExePath : me.szModule;
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// 游戏进程常锁住 FakeFocus32.dll，新编译产物先落到 FakeFocus32.next.dll。
// 选时间戳最新那份；禁止按文件大小黑名单（164352 的 raw 包与现行 mapleSafe 同尺寸）。
std::wstring PickPreferredFakeFocusDll(const std::wstring& dir, const wchar_t* dllName) {
    if (!dllName || !*dllName) return {};
    if (lstrcmpiW(dllName, L"FakeFocus32.dll") == 0) {
        // FakeFocus32.next.dll 是「临时试新」的旁路槽。但它**不能无条件优先**：
        // 槽里一旦躺着旧副本（历史上就躺过一个 9/7 的构建），主 DLL 再怎么重建都不生效，
        // 症状是「明明改了代码却毫无变化」，极难排查。只有它不比主 DLL 旧时才优先。
        const std::wstring nextPath = dir + L"FakeFocus32.next.dll";
        const std::wstring mainPath = dir + L"FakeFocus32.dll";
        if (PathExists(nextPath)) {
            ULONGLONG nextBytes = 0;
            ULONGLONG mainBytes = 0;
            FILETIME nextTime{};
            FILETIME mainTime{};
            const bool nextStamped = ReadFileStamp(nextPath, nextBytes, nextTime);
            const bool mainStamped = ReadFileStamp(mainPath, mainBytes, mainTime);
            if (!mainStamped
                || (nextStamped && CompareFileTime(&nextTime, &mainTime) >= 0)) {
                return nextPath;
            }
        }
    }
    std::wstring chosen;
    ULONGLONG chosenBytes = 0;
    FILETIME chosenTime{};
    if (lstrcmpiW(dllName, L"FakeFocus32.dll") == 0) {
        WIN32_FIND_DATAW fd{};
        const std::wstring glob = dir + L"FakeFocus32*.dll";
        HANDLE find = FindFirstFileW(glob.c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                // 仅按文件名排除 crashy raw 包；勿按 164352 字节整类跳过（会误伤现行 mapleSafe）。
                if (wcsstr(fd.cFileName, L".raw.") != nullptr) continue;
                const std::wstring cand = dir + fd.cFileName;
                const ULONGLONG candBytes =
                    (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                const LONG newer = chosen.empty()
                    ? 1
                    : CompareFileTime(&fd.ftLastWriteTime, &chosenTime);
                const bool candWins = chosen.empty()
                    || newer > 0
                    || (newer == 0 && candBytes > chosenBytes)
                    || (chosenBytes == 155136ull && candBytes != 155136ull);
                if (candWins) {
                    chosen = cand;
                    chosenBytes = candBytes;
                    chosenTime = fd.ftLastWriteTime;
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    } else {
        const std::wstring primary = dir + dllName;
        if (PathExists(primary)) chosen = primary;
    }
    return chosen;
}

void WarnIfStaleFakeFocus32Choice(const std::wstring& dir, const std::wstring& chosen) {
    if (chosen.empty()) return;
    const auto slash = chosen.find_last_of(L"\\/");
    const std::wstring base =
        (slash == std::wstring::npos) ? chosen : chosen.substr(slash + 1);
    if (lstrcmpiW(base.c_str(), L"FakeFocus32.dll") != 0) return;
    ULONGLONG chosenBytes = 0;
    FILETIME chosenTime{};
    if (!ReadFileStamp(chosen, chosenBytes, chosenTime)) return;
    std::wstring newerPath;
    ULONGLONG newerBytes = 0;
    FILETIME newerTime{};
    WIN32_FIND_DATAW fd{};
    const std::wstring glob = dir + L"FakeFocus32*.dll";
    HANDLE find = FindFirstFileW(glob.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (wcsstr(fd.cFileName, L".raw.") != nullptr) continue;
        const std::wstring cand = dir + fd.cFileName;
        if (cand == chosen) continue;
        const ULONGLONG candBytes =
            (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        const LONG newer = CompareFileTime(&fd.ftLastWriteTime, &chosenTime);
        if (newer <= 0 && !(newer == 0 && candBytes > chosenBytes)) continue;
        if (newerPath.empty()
            || CompareFileTime(&fd.ftLastWriteTime, &newerTime) > 0
            || (CompareFileTime(&fd.ftLastWriteTime, &newerTime) == 0 && candBytes > newerBytes)) {
            newerPath = cand;
            newerBytes = candBytes;
            newerTime = fd.ftLastWriteTime;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (newerPath.empty()) return;
    WindowModeLogf(
        L"[窗口/后台窗口模式] 警告：将注入旧 FakeFocus32（%llu 字节 %s），同目录有更新副本 %s（%llu 字节）。"
        L"请先退出 MapleStoryt.exe，覆盖 FakeFocus32.dll 或改用 FakeFocus32.next.dll",
        chosenBytes, chosen.c_str(), newerPath.c_str(), newerBytes);
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
    const std::wstring source = PickPreferredFakeFocusDll(dir, dllName);
    if (source.empty() || !PathExists(source)) {
        err = std::wstring(L"找不到假焦点 DLL: ") + dir + dllName;
        return false;
    }
    if (lstrcmpiW(dllName, L"FakeFocus32.dll") == 0) {
        WarnIfStaleFakeFocus32Choice(dir, source);
    }
    // ★ 注入的是**副本**，不是 exe 旁那一份。理由（真实事故，别再改回去）：
    // 注入 = 目标进程长期映射这个文件，而宿主被强杀 / 桌面钩没拆干净 / 引用计数残留
    // 都会让这个映射活到目标进程退出（往往是重启电脑）⇒ 安装目录里的 FakeFocus32.dll
    // 删不掉、覆盖不了，安装包卡在覆盖这一步。改成注入副本后，安装目录那份
    // **没有任何进程映射它**，覆盖安装/卸载/手动删除都不再需要重启。
    // 详见 fake_focus_stage.cpp 顶部。
    std::wstring staged;
    std::wstring stageErr;
    if (!StageFakeFocusDllForInjection(source, staged, stageErr)) {
        err = L"准备假焦点注入副本失败: " + stageErr;
        return false;
    }
    if (_wcsicmp(staged.c_str(), source.c_str()) != 0) {
        WindowModeLogf(
            L"[窗口/后台窗口模式] 假焦点注入走副本（安装目录那份不被任何进程映射）: %s → %s",
            source.c_str(), staged.c_str());
    }
    outPath = staged;
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
                L"[窗口/后台窗口模式] setwindowshook 未装入 DLL（%s），改试 classic",
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
                    L"[窗口/后台窗口模式] classic 仍失败（%s），改试 ntcreatethreadex",
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
        WindowModeLogf(L"[窗口/后台窗口模式] 假焦点注入已回退 tech=%s pid=%lu",
            inject::TechniqueName(used), static_cast<unsigned long>(pid));
    }
    t.remoteModule = injectResult.remoteModule;
    t.moduleHidden = injectResult.moduleHidden;
    t.hideState = injectResult.hideState;
    t.dllPath = path;
    // 桌面钩路径必须把 HHOOK 与本地模块句柄一并收下，否则卸载时拆不掉
    // （钩子还在 ⇒ user32 钉住 DLL ⇒ 目标进程活着这份 DLL 就一直被锁）。
    t.hookModule = injectResult.hookModule;
    t.hookHandle = injectResult.hookHandle;
    // 注入用的是 LoadLibrary 系技术 ⇒ 目标是真 loader 模块，卸载靠远程 FreeLibrary
    // （手动映射/映像节映射不注册进 loader，remoteModule 只是映射基址，FreeLibrary 无意义）。
    t.loaderLoaded = injectResult.remoteModule != nullptr
        && inject::IsLoadLibraryBasedTechnique(technique_);

    HWND top = GetAncestor(targetTop, GA_ROOT);
    if (!top) top = targetTop;
    const char* primary = timeScaleOnly_
        ? "FakeFocus_InstallTimeScaleOnly"
        : (lite ? "FakeFocus_InstallLite" : "FakeFocus_Install");
    // 仅变速注入**不**回退到会装全套假焦点钩的导出：那等于把用户特意关掉的注入偷偷打开。
    // 宁可失败并明确报错，让用户去更新 DLL，也不要背着他改变键鼠行为。
    const char* fallback = timeScaleOnly_
        ? nullptr
        : (lite ? "FakeFocus_Install" : "FakeFocus_InstallLite");
    if (!RemoteCallExport(t, primary, top, err)) {
        std::wstring fbErr;
        if (fallback && RemoteCallExport(t, fallback, top, fbErr)) {
            WindowModeLogf(
                L"[窗口/后台窗口模式] 假焦点导出 %hs 失败（%s），已改用 %hs",
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
            if (timeScaleOnly_) {
                err = L"目标 DLL 不支持变速专用注入（缺少导出 FakeFocus_InstallTimeScaleOnly）；"
                    L"请把 exe 旁的 FakeFocus64.dll / FakeFocus32.dll 更新到与主程序同版本";
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

bool FakeFocusInjector::FreeRemoteModuleOnce(Target& t) {
    if (!t.process || !t.remoteModule) return false;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto* freeLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(k32, "FreeLibrary"));
    if (!freeLib) return false;
    HANDLE thread = CreateRemoteThread(t.process, nullptr, 0, freeLib,
        t.remoteModule, 0, nullptr);
    if (!thread) return false;
    WaitForSingleObject(thread, 5000);
    DWORD exitCode = 0;
    const bool got = GetExitCodeThread(thread, &exitCode) != FALSE;
    CloseHandle(thread);
    // 线程返回值就是 FreeLibrary 的 BOOL：FALSE = 这次没卸下（再试也没用）。
    return got && exitCode != 0;
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
    // ★ 先拆桌面钩：钩子装着的期间 user32 会把这个 DLL 钉在目标进程里，
    // 此时远程 FreeLibrary 只是把 LoadLibrary 那次引用还掉，模块**不会**被卸下
    // （目标进程活着 ⇒ 副本/安装目录那份文件一直被锁）。旧实现把 HHOOK 丢在
    // 函数局部变量里，于是这个钩永远拆不掉 —— 这就是「软件关了文件还删不掉」的根之一。
    if (t.hookHandle) {
        if (!UnhookWindowsHookEx(reinterpret_cast<HHOOK>(t.hookHandle))) {
            WindowModeLogf(L"[窗口/后台窗口模式] UnhookWindowsHookEx 失败 pid=%lu（Win32=%lu）",
                static_cast<unsigned long>(t.pid),
                static_cast<unsigned long>(GetLastError()));
        }
        t.hookHandle = nullptr;
    }
    if (t.hookModule) {
        FreeLibrary(t.hookModule);
        t.hookModule = nullptr;
    }
    if (t.remoteModule) {
        // 目标进程里的模块引用计数**可能 >1**：上一轮宿主被强杀 / 上次卸载只成功了一半
        // ⇒ 每跑一轮就 +1。只 FreeLibrary 一次等于永远卸不掉：DLL 常驻，文件常锁。
        // 所以按「模块还在不在」循环卸，直到它从模块列表里消失（有上限，绝不死循环）。
        int freed = 0;
        const int kMaxFreeAttempts = 8;
        for (int i = 0; i < kMaxFreeAttempts; ++i) {
            if (!FreeRemoteModuleOnce(t)) break;
            ++freed;
            if (!t.loaderLoaded) break;   // 手动映射/映像节映射：FreeLibrary 无意义，试一次即可
            std::wstring stillThere;
            if (!TargetHasStaleFakeFocusModule(t.pid, stillThere)) break;
        }
        if (freed > 1) {
            WindowModeLogf(L"[窗口/后台窗口模式] 假焦点模块引用计数残留：pid=%lu 共 FreeLibrary %d 次",
                static_cast<unsigned long>(t.pid), freed);
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
        // ★ 必须**持久化**（Event 级）：这一条失败会让**整个假焦点注入中止**，后果是
        //   「后台模式只剩 PostMessage 的攻击键、方向键全失效」（用户报障原文：
        //   「原地不动的平A，不能走A」）。而它原来只走 `WindowModeLogf`（非 Event），
        //   宏调试窗那条通道又被 `runningWindowMode_.enabled` 过滤掉（嵌套模式要到
        //   BeginRun 成功后才 publish）⇒ 诊断报告里**一个字都看不到**，
        //   现场表现为「日志里只有 BeginRun + EndRun，注入那三行凭空消失」。
        //   前科：2026-10-03 用户报障，整轮排查卡在「看不到任何注入期信息」。
        WindowModeLogEventf(
            L"[窗口/后台窗口模式] ⛔ 假焦点注入中止：软输入共享内存创建失败 pid=%lu：%s"
            L"（方向键/软键态会全部失效，后台只剩 PostMessage 的攻击键）",
            static_cast<unsigned long>(windowPid), err.c_str());
        return false;
    }

    if (!ResolveDllPathForPid(windowPid, dllPath_, err)) {
        lastError_ = err;
        FakeFocusSoftInput_Detach();
        return false;
    }

    WindowModeLogf(L"[窗口/后台窗口模式] 假焦点注入技术=%s hideModule=%d windowPidOnly=%d lite=%d",
        inject::TechniqueName(technique_), hideModule_ ? 1 : 0,
        windowPidOnly ? 1 : 0, lite ? 1 : 0);

    // 注入前先查残留：进程里已经有 FakeFocus 模块时再注入 = 双重挂钩，
    // 典型症状是**注入后游戏立刻闪退**（而且日志里 pid/hwnd 长时间不变）。
    // ★2026-09-30 起：**不同文件 ⇒ 硬阻断**（原来只警告不阻断）。
    //   现场（用户日志）：游戏里已加载 `F:\QuickScriptTool\FakeFocus32.dll`，本轮却注入
    //   `E:\QuickScriptTool\FakeFocus32.dll`（用户装了两份）⇒ 双重挂钩 ⇒
    //   `目标窗口已消失 … exit=0xC0000005 本轮已跑=0ms`，**游戏被带走**。
    //   §11 的判断标准很明确：「游戏闪退」比「没装上/拿不到诊断」严重得多 ⇒ 这里必须阻断。
    //   「同一份文件」仍只警告（复用已装实例是正常路径，且能拿到诊断）。
    {
        std::wstring stalePath;
        if (TargetHasStaleFakeFocusModule(windowPid, stalePath)) {
            const bool sameFile = _wcsicmp(stalePath.c_str(), dllPath_.c_str()) == 0;
            WindowModeLogf(
                L"[窗口/后台窗口模式] %s 目标进程 %lu 里已加载着 %s（本轮要注入的是 %s，%s）。%s",
                sameFile ? L"⚠" : L"⛔",
                static_cast<unsigned long>(windowPid), stalePath.c_str(), dllPath_.c_str(),
                sameFile ? L"同一份文件" : L"**不同文件**",
                sameFile
                    ? L"同一份会复用已装好的实例（不会重复挂钩）；若上一轮是**被强杀**的，"
                      L"实例可能停在半拆状态，**建议先完全退出 MapleStoryt.exe 再跑**。"
                    : L"**不同文件 = 进程里会存在两份 FakeFocus，双重挂钩（IAT 覆盖两次、"
                      L"DI 方法体 JMP 叠加）会直接闪退**。已**拒绝注入**：请完全退出"
                      L"MapleStoryt.exe（确认任务管理器里没有残留进程），并且只保留一份"
                      L"本软件安装（别同时用两个目录里的 exe/DLL）后重试。");
            if (!sameFile) {
                err = L"目标进程里已加载着**另一份** FakeFocus（" + stalePath
                    + L"），本轮要注入的是 " + dllPath_
                    + L" —— 两份同时挂钩会让游戏闪退，已拒绝注入。请完全退出游戏后只保留一份安装再试。";
                lastError_ = err;
                // ★ 持久化：这是「注入被拒」里最容易被误判成「软件坏了」的一种 ——
                //   日志上只看到 BeginRun/EndRun，没有任何注入行。见上面 Attach 失败那段注释。
                WindowModeLogEventf(
                    L"[窗口/后台窗口模式] ⛔ 假焦点注入被拒：目标进程 pid=%lu 已加载另一份 %s"
                    L"（本轮要注入 %s）⇒ 双重挂钩会带走游戏，已拒绝",
                    static_cast<unsigned long>(windowPid), stalePath.c_str(), dllPath_.c_str());
                FakeFocusSoftInput_Detach();
                return false;
            }
            // ★ 同路径 ≠ 同一份内容：用户升级/重建过软件后，游戏进程里跑的还是**旧代码**
            //   （`LoadLibrary` 同路径只加引用计数，不会重新执行 DllMain）。而共享内存结构
            //   一旦加过字段，旧 DLL 与新宿主就**不兼容** ⇒ `SoftInputStateLooksValid` 失败
            //   ⇒ DLL 静默关掉视图 ⇒ 软键态/DirectInput 全失效（＝后台只剩原地平A），
            //   且诊断计数全 0。判据：磁盘上这个 DLL 的修改时间**晚于目标进程启动时间**
            //   ⇒ 进程内必然是旧内容（同路径复用旧实例是正常路径，这里只报警不阻断）。
            {
                ULONGLONG procStart = 0;
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, windowPid);
                if (hProc) {
                    FILETIME create{}, exitT{}, kernel{}, user{};
                    if (GetProcessTimes(hProc, &create, &exitT, &kernel, &user)) {
                        procStart = (static_cast<ULONGLONG>(create.dwHighDateTime) << 32)
                            | create.dwLowDateTime;
                    }
                    CloseHandle(hProc);
                }
                WIN32_FILE_ATTRIBUTE_DATA dllFad{};
                ULONGLONG dllWrite = 0;
                if (GetFileAttributesExW(dllPath_.c_str(), GetFileExInfoStandard, &dllFad)) {
                    dllWrite = (static_cast<ULONGLONG>(dllFad.ftLastWriteTime.dwHighDateTime) << 32)
                        | dllFad.ftLastWriteTime.dwLowDateTime;
                }
                if (windowmode::InjectedModuleLooksStale(dllWrite, procStart)) {
                    WindowModeLogEventf(
                        L"[窗口/后台窗口模式] ⚠ 目标进程 %lu 里挂的是**旧版** FakeFocus："
                        L"%s 在进程启动之后被改写过（DLL=%llu > 进程=%llu）。"
                        L"同路径会复用旧实例 ⇒ 共享内存布局可能不兼容 ⇒ 软键态/DirectInput "
                        L"失效（表现为「原地平A、不能走A」）。**请完全退出游戏进程再运行**。",
                        static_cast<unsigned long>(windowPid), stalePath.c_str(), dllWrite, procStart);
                }
            }
        }
    }

    const std::vector<DWORD> pids = windowPidOnly
        ? std::vector<DWORD>{windowPid}
        : CollectInjectPids(windowPid, targetTop);
    std::wstring firstErr;
    int okCount = 0;
    for (DWORD pid : pids) {
        std::wstring oneErr;
        if (InjectOne(pid, targetTop, dllPath_, oneErr, lite)) {
            ++okCount;
            WindowModeLogf(L"[窗口/后台窗口模式] 假焦点已注入 pid=%lu hwnd=0x%p lite=%d",
                static_cast<unsigned long>(pid), targetTop, lite ? 1 : 0);
        } else if (firstErr.empty()) {
            firstErr = oneErr;
        } else {
            WindowModeLogf(L"[窗口/后台窗口模式] 假焦点子进程注入跳过 pid=%lu: %s",
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
            L"[窗口/后台窗口模式] 假焦点注入完成 processes=%d windowPid=%lu dll=%s size=%llu "
            L"time=%04u-%02u-%02u %02u:%02u:%02u tech=%s",
            okCount, static_cast<unsigned long>(windowPid), dllPath_.c_str(),
            bytes,
            static_cast<unsigned>(local.wYear), static_cast<unsigned>(local.wMonth),
            static_cast<unsigned>(local.wDay), static_cast<unsigned>(local.wHour),
            static_cast<unsigned>(local.wMinute), static_cast<unsigned>(local.wSecond),
            inject::TechniqueName(technique_));
        // 不要再用「文件大小 == 某个魔术数」判新旧 DLL：源码一改大小就漂移，
        // 曾经把 163840 判成「旧」而把真正会闪退的 164352 当成「现行」，
        // 反倒把排查带偏。新旧一律以共享内存里的 `diag`/`hitReady` 为准
        // （宿主 `冒险岛钩安装` 行已经在打），这里只报文件戳供比对。
    } else {
        WindowModeLogf(L"[窗口/后台窗口模式] 假焦点注入完成 processes=%d windowPid=%lu dll=%s tech=%s",
            okCount, static_cast<unsigned long>(windowPid), dllPath_.c_str(),
            inject::TechniqueName(technique_));
    }
    return true;
}

bool FakeFocusInjector::DisableFakeFocusKeepTimeScale(std::wstring& err) {
    err.clear();
    bool any = false;
    for (auto& t : targets_) {
        if (!t.installed || !t.process || !t.remoteModule) continue;
        std::wstring one;
        if (!RemoteCallExport(t, "FakeFocus_DisableFakeFocus", nullptr, one)) {
            err = one;
            continue;
        }
        any = true;
    }
    if (!any) {
        if (err.empty()) err = L"假焦点 DisableFakeFocus 不可用";
        return false;
    }
    // 钩已拆掉：输入路径判定必须立刻回到「假焦点不可用」，否则会把键鼠送进不存在的钩子。
    fakeFocusDisabled_ = true;
    return true;
}

bool FakeFocusInjector::QueryTimeScaleDiag(DWORD& value, std::wstring& err) {
    value = 0;
    err.clear();
    for (auto& t : targets_) {
        if (!t.installed || !t.process || !t.remoteModule) continue;
        const std::wstring& path = !t.dllPath.empty() ? t.dllPath : dllPath_;
        DWORD exitCode = 0;
        if (inject::CallRemoteExport(t.process, t.pid, t.remoteModule, path,
                "FakeFocus_TimeScaleDiag", nullptr, 5000, &exitCode, err)) {
            value = exitCode;
            err.clear();
            return true;
        }
    }
    if (err.empty()) err = L"假焦点 TimeScaleDiag 不可用";
    return false;
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
    // 注意：**不要**在这里清 timeScaleOnly_ —— 调用方先 SetTimeScaleOnly 再 InjectAndInstall，
    // 而 InjectAndInstall 开头就调 Unload，清掉会把刚设的模式抹掉。
    fakeFocusDisabled_ = false;
    windowPid_ = 0;
    dllPath_.clear();
}

}  // namespace windowmode
