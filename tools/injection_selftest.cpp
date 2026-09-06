// =============================================================================
// InjectionSelfTest —— 注入技术库自测 + 对抗性测试驱动
// -----------------------------------------------------------------------------
// 自测（默认 / --json / --list）：
//   对每种注入技术，启动 InjectionTestTarget.exe 并注入 InjectionTestPayload
//   （写 %TEMP%\QstInjectionTestPayload_<pid>.marker 标记），验证成功。
//
// 对抗测试驱动：
//   InjectionSelfTest.exe --inject <pid> <dll> <technique> [--hide] [--hook-proc n] [--xor]
//   对真实目标（如游戏）执行指定注入技术，并输出模块可见性/可执行区等可观测面。
// =============================================================================

#include "selftest_harness.h"

#include "window_mode/injection/inject_common.h"
#include "window_mode/injection/inject_peb_hide.h"
#include "window_mode/injection/inject_technique.h"
#include "window_mode/fake_focus/fake_focus_injector.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

#include <tlhelp32.h>

namespace {

using selftest::Emit;
using selftest::gJson;

const selftest::CaseInfo kCases[] = {
    {L"classic_inject", L"default",
        L"CreateRemoteThread+LoadLibraryW 注入标记载荷并验证 DllMain 执行"},
    {L"nt_createthreadex_inject", L"default",
        L"NtCreateThreadEx+LoadLibraryW 注入"},
    {L"apc_inject", L"default",
        L"QueueUserAPC 注入（靶进程含可告警等待线程）"},
    {L"thread_hijack_inject", L"default",
        L"线程劫持注入（挂起+SetThreadContext，随后恢复原上下文）"},
    {L"manual_map_inject", L"default",
        L"手动映射注入：DllMain 执行成功且模块不出现在 Toolhelp 模块列表"},
    {L"manual_map_xor_inject", L"default",
        L"XOR 加密载荷 + 内存解密 + 手动映射"},
    {L"manualmap_hijack_inject", L"default",
        L"复合：手动映射 + 线程劫持入口（不新建线程，模块列表不可见）"},
    {L"manualmap_hijack_xor_inject", L"default",
        L"复合：XOR 载荷 + 手动映射 + 线程劫持入口（最隐蔽组合）"},
    {L"imagemap_inject", L"default",
        L"SEC_IMAGE 映像节映射（MEM_IMAGE，内存扫描看起来像正常镜像）"},
    {L"imagemap_hijack_inject", L"default",
        L"复合：映像节映射 + 线程劫持入口（镜像区域 + 不新建线程）"},
    {L"set_windows_hook_inject", L"default",
        L"SetWindowsHookEx(WH_GETMESSAGE) 钩子注入"},
    {L"peb_hide_restore", L"default",
        L"经典注入 + PEB 链表摘除后模块不可见，恢复后可见且可 FreeLibrary"},
    {L"fakefocus_injector_technique", L"default",
        L"产品类 FakeFocusInjector：映像节映射+线程劫持入口+PEB 隐藏注入真实靶进程并安全卸载"},
};

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring full(path);
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return full.substr(0, slash + 1);
}

std::wstring PayloadDllPath() {
#if defined(_WIN64)
    return ModuleDir() + L"InjectionTestPayload64.dll";
#else
    return ModuleDir() + L"InjectionTestPayload32.dll";
#endif
}

std::wstring MarkerPath(DWORD pid) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    wchar_t pidStr[32]{};
    swprintf_s(pidStr, L"%lu", static_cast<unsigned long>(pid));
    return std::wstring(temp) + L"QstInjectionTestPayload_" + pidStr + L".marker";
}

bool ReadTextFile(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[512]{};
    DWORD read = 0;
    while (ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
        buf[read] = '\0';
        out.append(buf, read);
    }
    CloseHandle(h);
    return true;
}

bool WaitMarker(DWORD pid, DWORD timeoutMs, std::string& content) {
    const DWORD deadline = GetTickCount() + timeoutMs;
    do {
        if (ReadTextFile(MarkerPath(pid), content)) return true;
        if (GetTickCount() >= deadline) break;
        Sleep(80);
    } while (true);
    return false;
}

void DeleteFileQuiet(const std::wstring& path) {
    DeleteFileW(path.c_str());
}

struct TargetProc {
    HANDLE process = nullptr;
    DWORD pid = 0;
    std::wstring outFile;
};

bool SpawnTarget(TargetProc& out, std::wstring& err) {
    out = TargetProc{};
    const std::wstring exe = ModuleDir() + L"InjectionTestTarget.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = L"找不到测试靶进程: " + exe;
        return false;
    }

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    wchar_t name[64]{};
    swprintf_s(name, L"qst_itt_%lu_%lu.out",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetTickCount()));
    out.outFile = std::wstring(temp) + name;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE outH = CreateFileW(out.outFile.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (outH == INVALID_HANDLE_VALUE) {
        err = L"创建输出文件失败: " + out.outFile;
        return false;
    }
    HANDLE inH = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inH;
    si.hStdOutput = outH;
    si.hStdError = outH;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    const BOOL ok = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (inH != INVALID_HANDLE_VALUE) CloseHandle(inH);
    CloseHandle(outH);
    if (!ok) {
        DeleteFileQuiet(out.outFile);
        wchar_t buf[128]{};
        swprintf_s(buf, L"CreateProcess 失败: Win32=%lu",
            static_cast<unsigned long>(GetLastError()));
        err = buf;
        return false;
    }
    CloseHandle(pi.hThread);
    out.process = pi.hProcess;
    out.pid = pi.dwProcessId;
    return true;
}

bool WaitReadyPid(const TargetProc& t, DWORD timeoutMs, DWORD& pid) {
    const DWORD deadline = GetTickCount() + timeoutMs;
    do {
        std::string content;
        const size_t pos = ReadTextFile(t.outFile, content)
            ? content.find("READY ") : std::string::npos;
        if (pos != std::string::npos) {
            pid = static_cast<DWORD>(std::strtoul(content.c_str() + pos + 6,
                                                  nullptr, 10));
            return pid != 0;
        }
        if (GetTickCount() >= deadline) break;
        Sleep(80);
    } while (true);
    return false;
}

void KillTarget(TargetProc& t) {
    if (t.process) {
        TerminateProcess(t.process, 1);
        WaitForSingleObject(t.process, 3000);
        CloseHandle(t.process);
        t.process = nullptr;
    }
    if (t.pid) DeleteFileQuiet(MarkerPath(t.pid));
    DeleteFileQuiet(t.outFile);
    t.pid = 0;
}

bool VerifyInject(windowmode::inject::Technique tech, bool hideModule,
                  bool expectModuleHidden, bool checkMarker, std::wstring& detail) {
    TargetProc t;
    std::wstring err;
    if (!SpawnTarget(t, err)) {
        detail = L"靶进程启动失败: " + err;
        return false;
    }
    DWORD targetPid = 0;
    if (!WaitReadyPid(t, 10000, targetPid)) {
        std::string raw;
        const bool haveOut = ReadTextFile(t.outFile, raw);
        DWORD exitCode = 0;
        GetExitCodeProcess(t.process, &exitCode);
        const DWORD attrs = GetFileAttributesW(t.outFile.c_str());
        wchar_t extra[96]{};
        swprintf_s(extra, L"（存在=%d 退出码=0x%08X 属性=0x%08X 错误=%lu）",
            haveOut ? 1 : 0, static_cast<unsigned>(exitCode), attrs,
            static_cast<unsigned long>(haveOut ? 0 : GetLastError()));
        detail = std::wstring(L"靶进程 READY 超时（输出: ") +
                 std::wstring(raw.begin(), raw.end()) + std::wstring(extra) + L"）";
        KillTarget(t);
        return false;
    }

    windowmode::inject::InjectOptions opts;
    opts.hideModule = hideModule;
    opts.timeoutMs = 20000;
    opts.hookProcName = "TestHookProc";

    std::wstring payloadPath = PayloadDllPath();
    std::wstring xorPath;
    if (tech == windowmode::inject::Technique::ManualMapXor ||
        tech == windowmode::inject::Technique::ManualMapHijackXor) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        wchar_t name[64]{};
        swprintf_s(name, L"qst_itt_xor_%lu.bin",
            static_cast<unsigned long>(targetPid));
        xorPath = std::wstring(temp) + name;
        std::wstring xorErr;
        if (!windowmode::inject::detail::XorFile(payloadPath, xorPath,
                                                 0x5A, xorErr)) {
            detail = L"XOR 打包失败: " + xorErr;
            KillTarget(t);
            return false;
        }
        payloadPath = xorPath;
    }

    windowmode::inject::InjectResult r;
    const bool ok = windowmode::inject::InjectDll(
        targetPid, payloadPath, tech, opts, r);
    if (!ok) {
        detail = r.detail.empty() ? L"注入失败" : r.detail;
        if (!xorPath.empty()) DeleteFileQuiet(xorPath);
        KillTarget(t);
        return false;
    }
    if (!xorPath.empty()) DeleteFileQuiet(xorPath);

    std::string marker;
    if (checkMarker && !WaitMarker(targetPid, 10000, marker)) {
        detail = L"注入返回成功但 DllMain 未执行（标记文件缺失）";
        KillTarget(t);
        return false;
    }
    if (marker.find("attach") == std::string::npos) {
        detail = std::wstring(L"标记文件内容异常: ") +
                 std::wstring(marker.begin(), marker.end());
        KillTarget(t);
        return false;
    }

    // 目标必须存活（曾出现入口桩把目标进程搞崩的问题，此处显式断言）
    DWORD exitCode = 0;
    GetExitCodeProcess(t.process, &exitCode);
    if (exitCode != STILL_ACTIVE) {
        detail = L"注入后目标进程已退出（exit=0x" +
            std::to_wstring(static_cast<unsigned>(exitCode)) + L"）";
        KillTarget(t);
        return false;
    }

    // 模块可见性（LoadLibrary 系应可见；手动映射应不可见）
    std::wstring baseName = windowmode::inject::detail::BaseNameOnly(PayloadDllPath());
    std::transform(baseName.begin(), baseName.end(), baseName.begin(), ::towlower);
    const bool visible =
        windowmode::inject::detail::FindRemoteModule(targetPid, baseName) != nullptr;
    if (expectModuleHidden && visible) {
        detail = L"预期模块隐藏但 Toolhelp 仍可见";
        KillTarget(t);
        return false;
    }
    if (!expectModuleHidden && !visible) {
        detail = L"预期模块可见但 Toolhelp 未发现";
        KillTarget(t);
        return false;
    }

    if (hideModule && r.moduleHidden) {
        // 恢复后应重新可见，并验证 FreeLibrary 安全
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
            | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD
            | SYNCHRONIZE, FALSE, targetPid);
        if (!process) {
            detail = L"恢复测试：OpenProcess 失败";
            KillTarget(t);
            return false;
        }
        std::wstring restoreErr;
        const bool restored = windowmode::inject::RestoreModuleFromPeb(
            process, targetPid, r.remoteModule, r.hideState, restoreErr);
        if (!restored) {
            detail = L"恢复 PEB 链表失败: " + restoreErr;
            CloseHandle(process);
            KillTarget(t);
            return false;
        }
        const bool visibleAfter =
            windowmode::inject::detail::FindRemoteModule(targetPid, baseName) != nullptr;
        if (!visibleAfter) {
            detail = L"恢复后模块仍未出现在模块列表";
            CloseHandle(process);
            KillTarget(t);
            return false;
        }
        // FreeLibrary 卸载（触发 detach 标记）
        uintptr_t freeLib = windowmode::inject::detail::ResolveRemoteProcAddress(
            process, targetPid, L"kernel32.dll", "FreeLibrary", restoreErr);
        if (!freeLib) {
            detail = L"解析 FreeLibrary 失败";
            CloseHandle(process);
            KillTarget(t);
            return false;
        }
        windowmode::inject::detail::RunRemoteThread(process,
            reinterpret_cast<void*>(freeLib), r.remoteModule, 10000,
            nullptr, restoreErr);
        CloseHandle(process);
        std::string detach;
        if (!WaitMarker(targetPid, 5000, detach) ||
            detach.find("detach") == std::string::npos) {
            detail = L"FreeLibrary 后未触发 DLL_PROCESS_DETACH";
            KillTarget(t);
            return false;
        }
    }

    KillTarget(t);
    return true;
}

bool VerifyFakeFocusInjector(std::wstring& detail) {
    TargetProc t;
    std::wstring err;
    if (!SpawnTarget(t, err)) {
        detail = L"靶进程启动失败: " + err;
        return false;
    }
    DWORD targetPid = 0;
    if (!WaitReadyPid(t, 10000, targetPid)) {
        detail = L"靶进程 READY 超时";
        KillTarget(t);
        return false;
    }
    // 消息专用窗口（HWND_MESSAGE）不进顶层窗口枚举，需按线程枚举
    HWND hwnd = nullptr;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != targetPid) continue;
                    HWND found = nullptr;
                    EnumThreadWindows(te.th32ThreadID,
                        [](HWND w, LPARAM lp) -> BOOL {
                            *reinterpret_cast<HWND*>(lp) = w;
                            return FALSE;
                        }, reinterpret_cast<LPARAM>(&found));
                    if (found) {
                        hwnd = found;
                        break;
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
    }
    if (!hwnd) {
        detail = L"未找到靶窗口";
        KillTarget(t);
        return false;
    }

    windowmode::FakeFocusInjector injector;
    injector.SetInjectionTechnique(windowmode::inject::Technique::ImageMapHijack);
    injector.SetHideModule(true);
    std::wstring injErr;
    if (!injector.InjectAndInstall(targetPid, hwnd, injErr)) {
        detail = L"FakeFocusInjector 注入失败: " + injErr;
        KillTarget(t);
        return false;
    }
    if (!injector.IsInjected()) {
        detail = L"注入后未标记已注入";
        injector.Unload();
        KillTarget(t);
        return false;
    }
    injector.Unload();
    const bool still = windowmode::inject::detail::FindRemoteModule(
        targetPid, L"fakefocus64.dll") != nullptr;
    KillTarget(t);
    if (still) {
        detail = L"卸载后 FakeFocus64.dll 仍留在目标进程（PEB 恢复/FreeLibrary 失败）";
        return false;
    }
    return true;
}

void RunSelfTest() {
    struct Case {
        const wchar_t* name;
        windowmode::inject::Technique tech;
        bool hide;
        bool expectHidden;
    };
    const Case cases[] = {
        {L"classic_inject", windowmode::inject::Technique::ClassicRemoteThread, false, false},
        {L"nt_createthreadex_inject", windowmode::inject::Technique::NtCreateThreadEx, false, false},
        {L"apc_inject", windowmode::inject::Technique::ApcQueue, false, false},
        {L"thread_hijack_inject", windowmode::inject::Technique::ThreadHijack, false, false},
        {L"manual_map_inject", windowmode::inject::Technique::ManualMap, false, true},
        {L"manual_map_xor_inject", windowmode::inject::Technique::ManualMapXor, false, true},
        {L"manualmap_hijack_inject", windowmode::inject::Technique::ManualMapHijack, false, true},
        {L"manualmap_hijack_xor_inject", windowmode::inject::Technique::ManualMapHijackXor, false, true},
        {L"imagemap_inject", windowmode::inject::Technique::ImageMap, false, true},
        {L"imagemap_hijack_inject", windowmode::inject::Technique::ImageMapHijack, false, true},
        {L"set_windows_hook_inject", windowmode::inject::Technique::SetWindowsHook, false, false},
        {L"peb_hide_restore", windowmode::inject::Technique::ClassicRemoteThread, true, true},
    };
    for (const Case& c : cases) {
        std::wstring detail;
        const bool ok = VerifyInject(c.tech, c.hide, c.expectHidden, true, detail);
        Emit(c.name, ok, detail.c_str());
    }
    std::wstring detail;
    const bool ffOk = VerifyFakeFocusInjector(detail);
    Emit(L"fakefocus_injector_technique", ffOk, detail.c_str());
}

void PrintHelp() {
    std::fwprintf(stderr,
        L"Usage:\n"
        L"  InjectionSelfTest.exe [--json] [--list] [--help]\n"
        L"  InjectionSelfTest.exe --inject <pid> <dll> <technique> "
        L"[--hide] [--hook-proc <name>] [--xor-key <0-255>]\n"
        L"\n"
        L"technique: classic | ntcreatethreadex | apc | threadhijack |\n"
        L"           manualmap | manualmapxor | setwindowshook |\n"
        L"           manualmaphijack | manualmaphijackxor |\n"
        L"           imagemap | imagemaphijack\n"
        L"\n"
        L"自测：对每种注入技术启动测试靶进程并验证标记载荷执行。\n"
        L"--inject：对指定进程执行注入（对抗性测试驱动），输出可观测面。\n");
}

int RunDriver(int argc, wchar_t** argv, int startIdx) {
    if (argc - startIdx < 3) {
        std::fwprintf(stderr, L"--inject 需要 <pid> <dll> <technique>\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(_wtoi(argv[startIdx]));
    const std::wstring dll = argv[startIdx + 1];
    const std::wstring techName = argv[startIdx + 2];
    windowmode::inject::Technique tech;
    if (!windowmode::inject::ParseTechnique(techName, tech)) {
        std::fwprintf(stderr, L"未知技术: %s\n", techName.c_str());
        return 2;
    }
    windowmode::inject::InjectOptions opts;
    opts.timeoutMs = 30000;
    for (int i = startIdx + 3; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--hide") == 0) opts.hideModule = true;
        else if (_wcsicmp(argv[i], L"--hook-proc") == 0 && i + 1 < argc) {
            char narrow[128]{};
            WideCharToMultiByte(CP_ACP, 0, argv[++i], -1, narrow, 128, nullptr, nullptr);
            opts.hookProcName = narrow;
        } else if (_wcsicmp(argv[i], L"--xor-key") == 0 && i + 1 < argc) {
            opts.xorKey = static_cast<uint8_t>(_wtoi(argv[++i]) & 0xFF);
        }
    }

    // manualmapxor：若传入的是明文 PE，先 XOR 打包到临时文件
    std::wstring injectPath = dll;
    std::wstring tempXor;
    if (tech == windowmode::inject::Technique::ManualMapXor) {
        std::vector<uint8_t> head;
        std::wstring readErr;
        if (windowmode::inject::detail::ReadFileBytes(dll, head, readErr) &&
            head.size() >= 2 && head[0] == 'M' && head[1] == 'Z') {
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp);
            wchar_t name[64]{};
            swprintf_s(name, L"qst_itt_xor_%lu_%lu.bin",
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned long>(GetTickCount()));
            tempXor = std::wstring(temp) + name;
            std::wstring packErr;
            if (!windowmode::inject::detail::XorFile(dll, tempXor,
                                                     opts.xorKey, packErr)) {
                std::fwprintf(stderr, L"XOR 打包失败: %s\n", packErr.c_str());
                return 2;
            }
            injectPath = tempXor;
        }
    }

    windowmode::inject::InjectResult r;
    const bool ok = windowmode::inject::InjectDll(pid, injectPath, tech, opts, r);
    if (!tempXor.empty()) DeleteFileQuiet(tempXor);
    std::wstring baseName = windowmode::inject::detail::BaseNameOnly(dll);
    std::transform(baseName.begin(), baseName.end(), baseName.begin(), ::towlower);
    windowmode::inject::detail::TargetObservable obs{};
    std::wstring obsErr;
    windowmode::inject::detail::SnapshotTarget(pid, baseName, obs, obsErr);

    std::fwprintf(stdout,
        L"{\"inject\":\"%s\",\"technique\":\"%s\",\"module_visible\":%s,"
        L"\"exec_regions\":%d,\"detail\":\"%s\"}\n",
        ok ? L"ok" : L"fail",
        windowmode::inject::TechniqueName(tech),
        obs.moduleInList ? L"true" : L"false",
        obs.execRegionCount,
        selftest::JsonEscape(r.detail.c_str()).c_str());
    if (!ok && !r.detail.empty()) {
        std::fwprintf(stderr, L"注入失败: %s\n", r.detail.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    int injectStart = -1;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--json") == 0) {
            gJson = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--list") == 0) {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--inject") == 0) {
            injectStart = i + 1;
        } else if (_wcsicmp(argv[i], L"--help") == 0 ||
                   _wcsicmp(argv[i], L"-h") == 0) {
            PrintHelp();
            return 0;
        }
    }
    if (injectStart > 0) {
        selftest::InitUtf8Stdout();
        return RunDriver(argc, argv, injectStart);
    }
    if (listOnly) {
        selftest::PrintCaseList(L"InjectionSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    if (!gJson) {
        std::fwprintf(stderr,
            L"=== InjectionSelfTest ===\n"
            L"(Agent: .cursor/skills/module-selftest/SKILL.md)\n");
    }
    RunSelfTest();
    selftest::EmitSummary();
    return selftest::ExitCode();
}
