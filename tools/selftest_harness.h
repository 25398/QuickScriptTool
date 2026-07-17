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
#endif

namespace selftest {

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
