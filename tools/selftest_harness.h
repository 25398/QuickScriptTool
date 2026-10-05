// =============================================================================
// selftest_harness.h — QuickScriptTool Agent 自检共用约定
// =============================================================================
//
// 所有 *SelfTest.exe 统一：
//   --json / --list / --help
//   stdout (--json): 每行 {"name","ok","detail"}；末行 {"passed","failed","ok"}
//   stdout (--list): 用例名\twhen\tmeaning + 末行 {"listed":N,"ok":true}
//   exit: 0 = 全过；N>0 = 失败用例数
//
// 总索引：.cursor/skills/module-selftest/SKILL.md
//
// =============================================================================
#pragma once

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace selftest {

/// ── 外部启动器护栏（2026-09-24 事故后加，所有 suite 自动生效）────────────────
/// 事故：原生消息宿主 `com.quickscripttool.bridge` 一度被误注册成
/// `build\Release\WindowModeSelfTest.exe`（当时的 `RegisterExtNativeMessagingHost()` 用的是
/// **当前进程 exe**，而自测进程也会起扩展桥常开服务、每 15s 重注册）。于是浏览器每次
/// `connectNative` 都用 `chrome-extension://<id>/ --parent-window=0` 拉起这个自测 exe，
/// 而它忽略参数、**直接跑整套自测** ⇒ 不停弹 notepad 窗口 + 写日志，用户体感「自测模块在无限弹窗」。
/// 根因已在 `ext_native_host.cpp` 修掉（只允许写产品 exe），这里是**第二道**：
/// 任何 `*SelfTest.exe` 只要是被浏览器/WebView 这类外部启动器拉起的，就**不跑用例**，
/// 打一行说明后 exit 0（对浏览器来说宿主"正常退出"，不会反复重试得更凶）。
/// 为什么用「命令行子串」而不是解析参数：判据只需要认几个不可能被自测用到的标记，
/// 免得给 30 个自测目标各加一份 shell32/参数解析依赖。
inline bool IsForeignLauncherCommandLine(const wchar_t* cmd) {
    if (!cmd || !*cmd) return false;
    static const wchar_t* kMarkers[] = {
        L"chrome-extension://",
        L"chrome-extension-",
        L"mozextension://",
        L"--parent-window=",
        L"--type=",
    };
    for (const wchar_t* m : kMarkers) {
        if (wcsstr(cmd, m)) return true;
    }
    return false;
}

inline void RefuseForeignLauncherAndExit() {
#ifdef _WIN32
    const wchar_t* cmd = GetCommandLineW();
    if (!IsForeignLauncherCommandLine(cmd)) return;
    // 不碰 CRT 流（此刻可能还没初始化完），直接写句柄。
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        char buf[512]{};
        const int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "SelfTest: launched by an external launcher (browser/WebView) - refusing to run "
            "any test case. Usage: <SelfTest>.exe [--json] [--list] [--macro]\n");
        DWORD wrote = 0;
        if (n > 0) WriteFile(out, buf, static_cast<DWORD>(n), &wrote, nullptr);
    }
    ExitProcess(0);
#else
    (void)0;
#endif
}

/// 静态初始化期就判：早于 wmain，因此**任何** suite 都拦得住（无需改 30 个 wmain）。
inline const bool g_foreignLauncherGuard = (RefuseForeignLauncherAndExit(), true);

inline bool gJson = false;
inline int gFailed = 0;
inline int gPassed = 0;

// --json / --list 时把 stdout 切到 UTF-8 宽字符模式，避免管道按 ACP 乱码。
inline void InitUtf8Stdout() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_U8TEXT);
#endif
}

// 兼容旧调用名
inline void InitJsonStdout() { InitUtf8Stdout(); }

inline std::wstring JsonEscape(const wchar_t* s) {
    std::wstring out;
    if (!s) return out;
    for (const wchar_t* p = s; *p; ++p) {
        if (*p == L'\\' || *p == L'"') {
            out.push_back(L'\\');
            out.push_back(*p);
            continue;
        }
        // 控制字符会拆坏 JSON 行；统一成空格
        if (*p < 0x20) {
            out.push_back(L' ');
            continue;
        }
        out.push_back(*p);
    }
    return out;
}

inline void Emit(const wchar_t* name, bool ok, const wchar_t* detail = L"") {
    if (ok) ++gPassed;
    else ++gFailed;
    if (gJson) {
        const std::wstring d = JsonEscape(detail);
        std::fwprintf(stdout,
            L"{\"name\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}\n",
            name, ok ? L"true" : L"false", d.c_str());
        return;
    }
    std::fwprintf(stderr, L"[%s] %s%s%s\n",
        ok ? L"PASS" : L"FAIL",
        name,
        (detail && detail[0]) ? L" — " : L"",
        detail ? detail : L"");
}

inline void EmitSummary() {
    if (gJson) {
        std::fwprintf(stdout, L"{\"passed\":%d,\"failed\":%d,\"ok\":%s}\n",
            gPassed, gFailed, gFailed == 0 ? L"true" : L"false");
        std::fflush(stdout);
    } else {
        std::fwprintf(stderr, L"\nSummary: %d passed, %d failed\n", gPassed, gFailed);
    }
}

inline int ExitCode() {
    return gFailed == 0 ? 0 : gFailed;
}

struct CaseInfo {
    const wchar_t* name;
    const wchar_t* when;    // default / macro / optional
    const wchar_t* meaning;
};

inline void PrintCaseList(const wchar_t* suiteTitle, const CaseInfo* cases, size_t count) {
    // stdout：Agent / 管道可读（勿写 stderr）
    std::fwprintf(stdout, L"%s cases (%u):\n", suiteTitle, static_cast<unsigned>(count));
    for (size_t i = 0; i < count; ++i) {
        const CaseInfo& c = cases[i];
        std::fwprintf(stdout, L"%s\t%s\t%s\n", c.name, c.when, c.meaning);
    }
    std::fwprintf(stdout, L"{\"listed\":%u,\"ok\":true}\n", static_cast<unsigned>(count));
    std::fflush(stdout);
}

}  // namespace selftest
