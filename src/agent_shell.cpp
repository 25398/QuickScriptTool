#include "agent_shell.h"

#include "agent_mcp.h"
#include "agent_undo.h"
#include "ooxml/xlsx_doc.h"
#include "utils.h"

#include <nlohmann/json.hpp>

#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr size_t kMaxCommandOutput = 64 * 1024;
constexpr DWORD kCommandTimeoutMs = 60000;
constexpr size_t kMaxReadBytes = 64 * 1024;
constexpr size_t kMaxSearchFileBytes = 4 * 1024 * 1024;
constexpr size_t kMaxSearchResults = 50;
constexpr int kMaxListDepth = 4;

std::wstring NormalizeSlashes(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return p;
}

std::wstring LowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), towlower);
    return s;
}

bool HasDotDotSegment(const std::wstring& p) {
    const std::wstring norm = NormalizeSlashes(p);
    if (norm.find(L"..") != std::wstring::npos) {
        // 只拒绝真正的 .. 路径段（..\ 或 \..\ 或结尾 \..）
        const std::wstring lower = LowerCopy(norm);
        if (lower.find(L"..\\") != std::wstring::npos
            || lower.find(L"\\..") != std::wstring::npos
            || lower == L"..") {
            return true;
        }
    }
    return false;
}

std::wstring StripNtPrefix(std::wstring p) {
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + p.substr(8);
    if (p.rfind(L"\\\\?\\", 0) == 0) return p.substr(4);
    return p;
}

std::wstring CanonicalizePath(const std::wstring& path) {
    wchar_t full[32768]{};
    const DWORD n = GetFullPathNameW(path.c_str(), 32768, full, nullptr);
    if (n == 0 || n >= 32768) return NormalizeSlashes(path);
    std::wstring out = full;
    auto openPath = [](const std::wstring& p) {
        return CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    };
    HANDLE h = openPath(out);
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t finalP[32768]{};
        const DWORD m = GetFinalPathNameByHandleW(h, finalP, 32768, FILE_NAME_NORMALIZED);
        CloseHandle(h);
        if (m > 0 && m < 32768) return NormalizeSlashes(StripNtPrefix(finalP));
        return NormalizeSlashes(out);
    }
    const size_t slash = out.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return NormalizeSlashes(out);
    const std::wstring parent = out.substr(0, slash);
    const std::wstring name = out.substr(slash + 1);
    HANDLE hp = openPath(parent);
    if (hp == INVALID_HANDLE_VALUE) return NormalizeSlashes(out);
    wchar_t finalP[32768]{};
    const DWORD m = GetFinalPathNameByHandleW(hp, finalP, 32768, FILE_NAME_NORMALIZED);
    CloseHandle(hp);
    if (m == 0 || m >= 32768) return NormalizeSlashes(out);
    return NormalizeSlashes(StripNtPrefix(finalP) + L"\\" + name);
}

bool IsPathWithin(const std::wstring& root, const std::wstring& path) {
    if (root.empty() || path.empty()) return false;
    if (HasDotDotSegment(path)) return false;
    std::wstring r = LowerCopy(CanonicalizePath(root));
    std::wstring p = LowerCopy(CanonicalizePath(path));
    while (r.size() > 1 && r.back() == L'\\') r.pop_back();
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    if (p == r) return true;
    return p.size() > r.size()
        && p.compare(0, r.size(), r) == 0
        && p[r.size()] == L'\\';
}

std::wstring ResolveAbsolutePath(const std::wstring& path) {
    if (path.empty()) return L"";
    if (path.size() >= 2 && path[1] == L':') return NormalizeSlashes(path);
    // 相对路径按 AppDir 解析（覆盖 scripts/recordings/images/library 等）
    if (path[0] == L'\\') return NormalizeSlashes(AppDir() + L"\\" + path.substr(1));
    return NormalizeSlashes(AppDir() + L"\\" + path);
}

std::wstring DetectRepoRoot() {
    std::wstring dir = AppDir();
    for (int up = 0; up < 4; ++up) {
        if (GetFileAttributesW((dir + L"\\CMakeLists.txt").c_str()) != INVALID_FILE_ATTRIBUTES
            && GetFileAttributesW((dir + L"\\build").c_str()) != INVALID_FILE_ATTRIBUTES) {
            return dir;
        }
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        dir = dir.substr(0, slash);
    }
    return L"";
}

std::wstring RepoRoot() {
    static const std::wstring root = DetectRepoRoot();
    return root;
}

std::vector<std::wstring> AgentReadRoots() {
    std::vector<std::wstring> roots;
    roots.push_back(ScriptsDir());
    roots.push_back(RecordingsDir());
    roots.push_back(FindImagesDir());
    roots.push_back(LibraryKindDir(L"ai"));
    roots.push_back(AppDir());
    if (!RepoRoot().empty()) roots.push_back(RepoRoot());
    return roots;
}

std::vector<std::wstring> AgentWriteRoots() {
    std::vector<std::wstring> roots;
    roots.push_back(ScriptsDir());
    roots.push_back(RecordingsDir());
    roots.push_back(FindImagesDir());
    roots.push_back(LibraryKindDir(L"ai"));
    if (!RepoRoot().empty()) {
        roots.push_back(RepoRoot() + L"\\docs");
        roots.push_back(RepoRoot() + L"\\.cursor\\skills");
        roots.push_back(RepoRoot() + L"\\skills");
    }
    return roots;
}

bool IsSensitiveAgentPath(const std::wstring& path) {
    std::wstring p = LowerCopy(NormalizeSlashes(path));
    auto has = [&](const wchar_t* s) {
        return p.find(s) != std::wstring::npos;
    };
    auto endsWith = [&](const wchar_t* s) {
        const size_t n = wcslen(s);
        return p.size() >= n && p.compare(p.size() - n, n, s) == 0;
    };
    if (has(L"\\app_settings.json") || endsWith(L"app_settings.json")) return true;
    if (has(L"\\scheduled_tasks.json") || endsWith(L"scheduled_tasks.json")) return true;
    if (has(L"ext_bridge.json") || has(L"bridge_runtime.json")) return true;
    if (has(L"\\agent_changes\\") || has(L"\\agent_conversations\\")) return true;
    if (has(L"\\webview2userdata\\") || has(L"\\webview2fixed\\")) return true;
    if (has(L"\\driver\\")) return true;
    if (endsWith(L".exe") || endsWith(L".dll") || endsWith(L".sys")
        || endsWith(L".pem") || endsWith(L".key") || endsWith(L".pfx")
        || endsWith(L".p12") || endsWith(L".pdb")) {
        return true;
    }
    return false;
}

bool IsPathInAnyRoot(const std::vector<std::wstring>& roots, const std::wstring& path) {
    for (const auto& root : roots) {
        if (IsPathWithin(root, path)) return true;
    }
    return false;
}

std::wstring AllowedRootsHint() {
    std::wstring out = L"允许目录：scripts / recordings / images / library / AppDir";
    if (!RepoRoot().empty())
        out += L" / 开发仓库（" + RepoRoot() + L"）";
    return out;
}

bool ReadFileBytes(const std::wstring& path, std::string& bytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > kMaxSearchFileBytes) return false;
    f.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(size));
    if (size > 0) f.read(bytes.data(), size);
    return f.good() || f.eof();
}

bool LooksBinary(const std::string& bytes) {
    const size_t n = (std::min)(bytes.size(), size_t(4096));
    return std::find(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n), '\0') != bytes.begin() + static_cast<std::ptrdiff_t>(n);
}

std::wstring ParseToolParamJson(const std::wstring& paramsJson, json& out, std::wstring& err) {
    err.clear();
    try {
        out = json::parse(ToUtf8(paramsJson));
    } catch (const json::parse_error&) {
        err = L"参数 JSON 解析失败";
        return err;
    }
    return L"";
}

// ---------------------------------------------------------------------------
// runAgentCommand 白名单
// ---------------------------------------------------------------------------

bool IsReadOnlyGitFlag(const std::wstring& arg) {
    static const wchar_t* kFlags[] = {
        L"--short", L"--stat", L"--name-only", L"--no-color", L"--oneline",
        L"--all", L"--cached", L"--decorate", L"--follow", L"--numstat",
        L"--summary", L"--abbrev-commit", L"--no-pager", L"--quiet", L"--",
        L"--version", L"-n", L"-1", L"-2", L"-3", L"-4", L"-5", L"-6", L"-7",
        L"-8", L"-9", L"-p", L"--patch", L"--color", L"--no-patch",
        L"--name-status", L"--raw", L"--pretty=oneline", L"--pretty=short",
        L"--pretty=full", L"--pretty=medium"
    };
    for (const auto* f : kFlags) {
        if (arg == f) return true;
    }
    if (arg.size() > 2 && arg[0] == L'-' && arg[1] == L'n'
        && arg.find_first_not_of(L"0123456789", 2) == std::wstring::npos) {
        return true;
    }
    return false;
}

bool ContainsMetaChar(const std::wstring& s) {
    for (wchar_t c : s) {
        if (c == L'|' || c == L';' || c == L'&' || c == L'<' || c == L'>'
            || c == L'$' || c == L'%' || c == L'`' || c == L'\n' || c == L'\r') {
            return true;
        }
    }
    return false;
}

std::wstring Basename(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool HasPathQualifier(const std::wstring& s) {
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'/' || c == L':') return true;
    }
    return false;
}

bool InFlagSet(const std::wstring& a, std::initializer_list<const wchar_t*> set) {
    for (const auto* s : set) {
        if (a == s) return true;
    }
    return false;
}

// branch/remote/config 虽然名字里带“只读”含义，但某些位置参数会写入 .git/config、
// 创建/改写分支或远程，因此逐参数收紧，避免只读白名单被写操作绕过。
bool ValidateReadOnlyGitSub(const std::wstring& sub,
                            const std::vector<std::wstring>& parts,
                            std::wstring& error) {
    auto reject = [&](const std::wstring& msg) {
        error = msg;
        return false;
    };
    if (sub == L"branch") {
        for (size_t i = 2; i < parts.size(); ++i) {
            const std::wstring& a = parts[i];
            if (ContainsMetaChar(a)) return reject(L"参数含禁止字符：" + a);
            if (a.empty() || a[0] != L'-') {
                return reject(L"git branch 不允许位置参数（会创建/改写分支）：" + a);
            }
            if (!InFlagSet(a, { L"-a", L"--all", L"-r", L"--remotes", L"-l", L"--list",
                                L"--show-current", L"-v", L"-vv", L"--verbose", L"--merged",
                                L"--no-merged", L"--color", L"--no-color", L"-q", L"--quiet",
                                L"--no-abbrev", L"--column", L"--no-column", L"--ignore-case" })) {
                return reject(L"git branch 参数不在只读白名单：" + a);
            }
        }
        return true;
    }
    if (sub == L"remote") {
        if (parts.size() == 2) return true;
        const std::wstring a = LowerCopy(parts[2]);
        if (parts.size() == 3 && InFlagSet(a, { L"-v", L"--verbose", L"--no-color" })) {
            if (ContainsMetaChar(parts[2])) return reject(L"参数含禁止字符：" + parts[2]);
            return true;
        }
        if (parts.size() == 4 && InFlagSet(a, { L"show", L"get-url" })) {
            if (ContainsMetaChar(parts[3])) return reject(L"参数含禁止字符：" + parts[3]);
            return true;
        }
        return reject(L"git remote 仅允许只读：无参数 / -v / show <name> / get-url <name>");
    }
    if (sub == L"config") {
        size_t positional = 0;
        for (size_t i = 2; i < parts.size(); ++i) {
            const std::wstring& a = parts[i];
            if (ContainsMetaChar(a)) return reject(L"参数含禁止字符：" + a);
            if (!a.empty() && a[0] == L'-') {
                if (!InFlagSet(a, { L"--list", L"-l", L"--get", L"--get-regexp",
                                    L"--show-origin", L"--show-scope", L"--null",
                                    L"--name-only", L"--local", L"--no-color" })) {
                    return reject(L"git config 参数不在只读白名单：" + a);
                }
            } else if (++positional > 1) {
                return reject(L"git config 只读模式仅允许一个键名（写入需用户确认）");
            }
        }
        return true;
    }
    return reject(L"git 子命令不支持：" + sub);
}

bool IsSelfTestTarget(const std::wstring& target) {
    static const wchar_t* kTargets[] = {
        L"WindowModeSelfTest", L"ScheduledTaskSelfTest", L"MacroVariablesSelfTest",
        L"ScriptActionBuilderSelfTest", L"CoordSpaceSelfTest", L"ScriptIoSelfTest",
        L"ImageMatchSelfTest", L"AiActionRouterSelfTest", L"AppSettingsStoreSelfTest",
        L"ThemeUiSelfTest", L"RecorderSelfTest", L"VirtualHidSelfTest",
        L"AgentAssistantSelfTest", L"AgentDesktopTaskSelfTest", L"OcrSelfTest"
    };
    for (const auto* t : kTargets) {
        if (_wcsicmp(target.c_str(), t) == 0) return true;
    }
    return false;
}

// 解析命令行为 argv；program 经白名单校验后，返回可执行命令串；违规返回错误说明。
std::wstring ValidateCommand(const std::wstring& command, const std::wstring& cwd,
                             std::wstring& error,
                             std::vector<std::wstring>& argv) {
    error.clear();
    argv.clear();
    if (Trim(command).empty()) {
        error = L"command 不能为空";
        return L"";
    }
    if (command.find(L'\n') != std::wstring::npos
        || command.find(L'\r') != std::wstring::npos) {
        error = L"command 不允许换行";
        return L"";
    }
    int argc = 0;
    wchar_t** raw = CommandLineToArgvW(command.c_str(), &argc);
    if (!raw || argc <= 0) {
        error = L"无法解析命令参数";
        if (raw) LocalFree(raw);
        return L"";
    }
    std::vector<std::wstring> parts;
    for (int i = 0; i < argc; ++i) parts.emplace_back(raw[i]);
    LocalFree(raw);

    const auto resolveProg = [&](const std::wstring& p) {
        if (p.size() >= 2 && p[1] == L':') return NormalizeSlashes(p);
        if (!p.empty() && p[0] == L'\\') return NormalizeSlashes(cwd + L"\\" + p.substr(1));
        return NormalizeSlashes(cwd + L"\\" + p);
    };
    const std::wstring program = Basename(parts[0]);
    const std::wstring programLower = LowerCopy(program);

    if (programLower == L"git" || programLower == L"git.exe") {
        if (parts.size() < 2) {
            error = L"git 需要子命令（仅允许只读子命令）";
            return L"";
        }
        if (HasPathQualifier(parts[0])) {
            error = L"git 程序必须使用 PATH 中的裸名，不接受带路径的程序名";
            return L"";
        }
        const std::wstring sub = LowerCopy(parts[1]);
        static const wchar_t* kReadOnly[] = {
            L"status", L"diff", L"log", L"show", L"rev-parse",
            L"ls-files", L"help", L"version"
        };
        bool ok = false;
        for (const auto* s : kReadOnly) {
            if (sub == s) { ok = true; break; }
        }
        if (sub == L"stash") {
            ok = parts.size() >= 3 && LowerCopy(parts[2]) == L"list";
        }
        if (!ok && (sub == L"branch" || sub == L"remote" || sub == L"config")) {
            ok = ValidateReadOnlyGitSub(sub, parts, error);
        }
        if (!ok) {
            error = L"git 仅允许只读子命令：status/diff/log/show/rev-parse/ls-files/help/version/stash list，以及 branch/remote/config 的只读形式";
            return L"";
        }
        if (sub != L"branch" && sub != L"remote" && sub != L"config") {
            for (size_t i = 2; i < parts.size(); ++i) {
                if (ContainsMetaChar(parts[i])) {
                    error = L"参数含禁止字符：" + parts[i];
                    return L"";
                }
                if (!parts[i].empty() && parts[i][0] == L'-' && !IsReadOnlyGitFlag(parts[i])) {
                    error = L"git 参数不在只读白名单：" + parts[i];
                    return L"";
                }
            }
        }
        argv = parts;
        return L"git";
    }

    if (programLower == L"msbuild.exe") {
        if (HasPathQualifier(parts[0])) {
            error = L"MSBuild 程序必须使用 PATH 中的裸名，不接受带路径的程序名";
            return L"";
        }
        if (RepoRoot().empty()) {
            error = L"未检测到开发仓库，MSBuild 自检不可用";
            return L"";
        }
        bool hasTarget = false;
        for (size_t i = 1; i < parts.size(); ++i) {
            const std::wstring a = parts[i];
            const std::wstring low = LowerCopy(a);
            if (low == L"/m" || low == L"/v:minimal" || low == L"/nologo"
                || low == L"/p:configuration=release") {
                continue;
            }
            if (a.size() > 3 && (low[0] == L'/' || low[0] == L'-')
                && low.compare(0, 3, L"/t:") == 0) {
                const std::wstring target = a.substr(3);
                if (!IsSelfTestTarget(target)) {
                    error = L"MSBuild 仅允许自检 Target（" + target + L" 不在白名单）";
                    return L"";
                }
                if (hasTarget) {
                    error = L"MSBuild 只允许一个 /t: 目标";
                    return L"";
                }
                hasTarget = true;
                continue;
            }
            if (low.find(L"quickscripttool.sln") != std::wstring::npos
                && IsPathWithin(RepoRoot(), resolveProg(a))) {
                continue;
            }
            error = L"MSBuild 参数不在白名单：" + a;
            return L"";
        }
        if (!hasTarget) {
            error = L"MSBuild 必须带 /t:<自检Target>";
            return L"";
        }
        argv = parts;
        return L"msbuild";
    }

    // ⚠⚠ 这里的长度守卫**必须是 `>=` 而不是 `> 4`**（2026-09-23 实测抓到的 bug）：
    //   `programLower.size() - 12` 是 size_t 运算，`size < 12` 时**下溢**成
    //   `2^64 - (12 - size)`，而它**恰好等于 `npos`**（= 2^64-1）当且仅当 `size == 11`。
    //   `rfind` 找不到时返回 npos ⇒ 于是**任何长度为 11 的程序名都会被误判成自检 exe**
    //   （实测：`libreoffice` 正好 11 个字符，报的是「自检 exe 必须位于 …\build\Release」
    //   而不是「程序不在白名单」—— 提示完全误导，很难联想）。
    //   同理第二条在 `size == 12` 时也会误判。
    //   ⇒ 不是安全漏洞（这条分支随后仍会拒绝），但**判据本身是坏的**，必须修。
    // ⚠ A/B 验证过：把这里的 `>= 12` 改回旧的 `> 4` ⇒ AgentAssistantSelfTest 的
    //   `shell_rejects_third_party` 转红（rejected=7/9，两个 11/12 字符的名字被误判）。
    const bool isSelfTestExe = programLower.size() >= 12
        && programLower.rfind(L"selftest.exe") == programLower.size() - 12;
    const bool isLogicTestExe = programLower.size() >= 13
        && programLower.rfind(L"logictest.exe") == programLower.size() - 13;
    if (isSelfTestExe || isLogicTestExe) {
        if (RepoRoot().empty()) {
            error = L"未检测到开发仓库，自检 exe 不可用";
            return L"";
        }
        if (!IsPathWithin(RepoRoot() + L"\\build\\Release", resolveProg(parts[0]))) {
            error = L"自检 exe 必须位于 " + RepoRoot() + L"\\build\\Release";
            return L"";
        }
        for (size_t i = 1; i < parts.size(); ++i) {
            const std::wstring a = parts[i];
            if (a != L"--json" && a != L"--list" && a != L"--help"
                && a != L"--macro" && a != L"--list-cases") {
                error = L"自检 exe 仅允许 --json/--list/--help/--macro/--list-cases";
                return L"";
            }
        }
        argv = parts;
        return L"selftest";
    }

    if (programLower == L"where" || programLower == L"where.exe") {
        if (HasPathQualifier(parts[0])) {
            error = L"where 程序必须使用 PATH 中的裸名，不接受带路径的程序名";
            return L"";
        }
        for (size_t i = 1; i < parts.size(); ++i) {
            if (ContainsMetaChar(parts[i])) {
                error = L"参数含禁止字符：" + parts[i];
                return L"";
            }
            const std::wstring a = parts[i];
            if (a.empty()) continue;
            const std::wstring low = LowerCopy(a);
            if (low == L"/r" || low == L"-r" || low.rfind(L"/r:", 0) == 0
                || HasPathQualifier(a) || a[0] == L'/' || a[0] == L'-') {
                error = L"where 仅允许查找 PATH 中的裸程序名，禁止 /R 与路径参数";
                return L"";
            }
        }
        argv = parts;
        return L"where";
    }

    // ⚠ 这里原来有一条 `genoffice` 白名单分支（放行 GenOffice CLI 的读/写子命令，
    //   并把它解析成绝对路径）——**已删除**（2026-09-23）。
    //
    // 删掉的理由（产品原则，别再加回来）：本产品**不依赖任何别人的软件运行**。
    // 给某个第三方工具开白名单特例，等于把「写 xlsx/docx/pptx」这项能力外包出去：
    // 对方没装就能力缺失、版本不同就行为不同，而且这份子命令清单还要跟着对方发版维护
    // （它一改我们就得改，否则白名单静默失效）。
    //
    // 正确做法：借鉴它的**做法与思路**（工具层不调模型 / 字节保留式编辑 / 产物可验收），
    // 在**我们自己的引擎里实现**办公文档能力 —— 见 `src/ooxml/`。
    // 用户若仍想接它，走**厂商中立**的通用 MCP 配置（`mcp_servers.json`），
    // 产品不替任何人做决定、也不主动探测任何东西。

    error = L"程序不在白名单：" + program
        + L"。允许：git（只读）、MSBuild（自检）、build\\Release\\*SelfTest.exe、where";
    return L"";
}

std::wstring BuildCommandLine(const std::vector<std::wstring>& argv) {
    std::wstring line;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) line += L' ';
        const std::wstring& a = argv[i];
        if (a.empty() || a.find_first_of(L" \t\"") == std::wstring::npos) {
            line += a;
            continue;
        }
        line += L'"';
        for (wchar_t c : a) {
            if (c == L'"') line += L"\\\"";
            else line += c;
        }
        line += L'"';
    }
    return line;
}

std::wstring RunProcess(const std::wstring& commandLine, const std::wstring& cwd,
    DWORD timeoutMs = kCommandTimeoutMs) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hOutRead = nullptr, hOutWrite = nullptr;
    HANDLE hErrRead = nullptr, hErrWrite = nullptr;
    if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0)
        || !CreatePipe(&hErrRead, &hErrWrite, &sa, 0)) {
        return L"[错误] 创建管道失败";
    }
    SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hErrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOutWrite;
    si.hStdError = hErrWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = commandLine;
    std::wstring mutableCwd = cwd.empty() ? L"" : cwd;
    const BOOL created = CreateProcessW(
        nullptr, &mutableCmd[0], nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr,
        mutableCwd.empty() ? nullptr : mutableCwd.c_str(), &si, &pi);
    CloseHandle(hOutWrite);
    CloseHandle(hErrWrite);
    if (!created) {
        CloseHandle(hOutRead);
        CloseHandle(hErrRead);
        return L"[错误] 进程启动失败，错误码 " + std::to_wstring(GetLastError());
    }
    CloseHandle(pi.hThread);

    std::string outBuf, errBuf;
    char buf[4096];
    // ⚠ 必须用 GetTickCount64 + 显式 startTick：GetTickCount() 是 32 位、约 49.7 天回绕，
    //   而 `deadline > GetTickCount()` 这种比较在回绕点附近会立刻判成「已超时」
    //   ⇒ 命令被无故杀掉。超时给到 300s（GenOffice）后这个窗口更值得避开。
    const ULONGLONG startTick = GetTickCount64();
    DWORD exitCode = STILL_ACTIVE;
    while (exitCode == STILL_ACTIVE) {
        const ULONGLONG elapsed = GetTickCount64() - startTick;
        if (elapsed >= timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            exitCode = 1;
            outBuf += "\n[超时] 命令超过 " + std::to_string(timeoutMs / 1000) + " 秒已终止";
            break;
        }
        DWORD n = 0;
        while (PeekNamedPipe(hOutRead, nullptr, 0, nullptr, &n, nullptr) && n > 0) {
            DWORD rd = 0;
            if (ReadFile(hOutRead, buf, (std::min)(n, DWORD(sizeof(buf))), &rd, nullptr)) {
                outBuf.append(buf, rd);
            }
        }
        while (PeekNamedPipe(hErrRead, nullptr, 0, nullptr, &n, nullptr) && n > 0) {
            DWORD rd = 0;
            if (ReadFile(hErrRead, buf, (std::min)(n, DWORD(sizeof(buf))), &rd, nullptr)) {
                errBuf.append(buf, rd);
            }
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &exitCode);
        }
    }
    // 进程结束后再排空一次
    DWORD n = 0;
    while (PeekNamedPipe(hOutRead, nullptr, 0, nullptr, &n, nullptr) && n > 0) {
        DWORD rd = 0;
        if (ReadFile(hOutRead, buf, (std::min)(n, DWORD(sizeof(buf))), &rd, nullptr)) {
            outBuf.append(buf, rd);
        }
    }
    while (PeekNamedPipe(hErrRead, nullptr, 0, nullptr, &n, nullptr) && n > 0) {
        DWORD rd = 0;
        if (ReadFile(hErrRead, buf, (std::min)(n, DWORD(sizeof(buf))), &rd, nullptr)) {
            errBuf.append(buf, rd);
        }
    }
    CloseHandle(hOutRead);
    CloseHandle(hErrRead);
    CloseHandle(pi.hProcess);

    std::string combined;
    combined.reserve(outBuf.size() + errBuf.size() + 64);
    combined += outBuf;
    if (!errBuf.empty()) {
        if (!combined.empty() && combined.back() != '\n') combined += "\n";
        combined += errBuf;
    }
    if (combined.size() > kMaxCommandOutput) {
        combined.resize(kMaxCommandOutput);
        combined += "\n...（输出已截断）";
    }
    std::wstring result = L"exit=" + std::to_wstring(exitCode) + L"\n";
    result += FromUtf8(combined);
    return result;
}

std::wstring FormatFileSize(uint64_t size) {
    if (size < 1024) return std::to_wstring(size) + L" B";
    if (size < 1024 * 1024) return std::to_wstring(size / 1024) + L" KB";
    return std::to_wstring(size / (1024 * 1024)) + L" MB";
}

std::wstring ListDirectoryRec(const std::wstring& absDir, const std::wstring& rel,
                              int depth, size_t& count, size_t maxEntries,
                              std::wstring& out) {
    if (depth > kMaxListDepth || count >= maxEntries) return L"";
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = absDir + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0
            || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
            continue;
        }
        if (count >= maxEntries) break;
        const std::wstring name = fd.cFileName;
        const std::wstring childRel = rel.empty() ? name : rel + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            out += L"[目录] " + childRel + L"\\\n";
            ++count;
            ListDirectoryRec(absDir + L"\\" + name, childRel, depth + 1,
                             count, maxEntries, out);
        } else {
            const uint64_t size =
                (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            out += L"[文件] " + childRel + L"  (" + FormatFileSize(size) + L")\n";
            ++count;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return L"";
}

}  // namespace

AgentTool MakeRunAgentCommandTool() {
    AgentTool tool;
    tool.name = L"runAgentCommand";
    tool.description =
        L"在受限白名单内运行命令行工具。允许：git（只读子命令）、MSBuild（仅自检 Target，如 "
        L"/t:WindowModeSelfTest /p:Configuration=Release）、build\\Release\\*SelfTest.exe（--json/--list）、where。"
        L"参数由白名单逐条校验，不经 cmd/powershell。cwd 可选，必须位于允许目录内。"
        L"自检流程见 readAgentSkill section=shell；办公文件见 section=office。"
        L"⚠ 想跑白名单外的程序（含第三方办公工具）请走 MCP：让用户把 server 写进 "
        L"mcp_servers.json，别指望这里放行 —— 产品不为任何第三方软件开特例。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "command": {
                "type": "string",
                "description": "完整命令行，如 git status --short 或 MSBuild 自检命令"
            },
            "cwd": {
                "type": "string",
                "description": "工作目录（可选），必须位于允许目录内"
            }
        },
        "required": ["command"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        std::wstring err;
        if (!ParseToolParamJson(paramsJson, params, err).empty())
            return L"[错误] " + err;
        const std::wstring command = FromUtf8(params.value("command", ""));
        const std::wstring cwdRaw = FromUtf8(params.value("cwd", ""));
        std::wstring cwd;
        if (!cwdRaw.empty()) {
            cwd = ResolveAbsolutePath(cwdRaw);
            if (!IsPathInAnyRoot(AgentReadRoots(), cwd)) {
                return L"[错误] cwd 不在允许目录内。\n" + AllowedRootsHint();
            }
        } else if (!RepoRoot().empty()) {
            cwd = RepoRoot();
        } else {
            cwd = ScriptsDir();
        }
        std::vector<std::wstring> argv;
        const std::wstring kind = ValidateCommand(command, cwd, err, argv);
        if (kind.empty()) return L"[错误] " + err;
        const std::wstring cmdLine = BuildCommandLine(argv);
        return RunProcess(cmdLine, cwd, kCommandTimeoutMs);
    };
    return tool;
}

AgentTool MakeListAgentDirectoryTool() {
    AgentTool tool;
    tool.name = L"listDirectory";
    tool.description =
        L"列出允许目录内的文件与子目录（含大小）。path 可相对（相对 AppDir）或绝对，"
        L"必须位于允许目录内。" + AllowedRootsHint() + L"";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "path": { "type": "string", "description": "目录路径" },
            "maxEntries": { "type": "integer", "description": "最多列出条目数，默认 200" }
        },
        "required": ["path"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        std::wstring err;
        if (!ParseToolParamJson(paramsJson, params, err).empty())
            return L"[错误] " + err;
        const std::wstring abs = ResolveAbsolutePath(FromUtf8(params.value("path", "")));
        if (abs.empty()) return L"[错误] path 不能为空";
        if (!IsPathInAnyRoot(AgentReadRoots(), abs)) {
            return L"[错误] 路径不在允许目录内：" + abs + L"\n" + AllowedRootsHint();
        }
        const DWORD attr = GetFileAttributesW(abs.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return L"[错误] 目录不存在：" + abs;
        const size_t maxEntries =
            static_cast<size_t>(params.value("maxEntries", 200));
        std::wstring out;
        out += L"目录 " + abs + L"\n";
        size_t count = 0;
        ListDirectoryRec(abs, L"", 0, count, maxEntries, out);
        if (count == 0) out += L"（空目录）\n";
        out += L"\n共 " + std::to_wstring(count) + L" 项";
        return out;
    };
    return tool;
}

AgentTool MakeReadAgentFileTool() {
    AgentTool tool;
    tool.name = L"readAgentFile";
    tool.description =
        L"读取允许目录内的文本文件。path 可相对（相对 AppDir）或绝对；maxBytes 默认 65536，"
        L"超出截断。用于排查脚本/设置/日志内容。" + AllowedRootsHint() + L"";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "path": { "type": "string", "description": "文件路径" },
            "maxBytes": { "type": "integer", "description": "读取上限，默认 65536" }
        },
        "required": ["path"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        std::wstring err;
        if (!ParseToolParamJson(paramsJson, params, err).empty())
            return L"[错误] " + err;
        const std::wstring abs = ResolveAbsolutePath(FromUtf8(params.value("path", "")));
        if (abs.empty()) return L"[错误] path 不能为空";
        if (!IsPathInAnyRoot(AgentReadRoots(), abs)) {
            return L"[错误] 路径不在允许目录内：" + abs + L"\n" + AllowedRootsHint();
        }
        if (IsSensitiveAgentPath(abs)) {
            return L"[错误] 该路径受保护，禁止助手读取：" + abs;
        }
        std::string bytes;
        if (!ReadFileBytes(abs, bytes)) return L"[错误] 无法读取文件（不存在或过大）：" + abs;
        if (LooksBinary(bytes)) return L"[错误] 二进制文件不支持文本读取：" + abs;
        size_t maxBytes = static_cast<size_t>(params.value("maxBytes", kMaxReadBytes));
        if (maxBytes < 1024) maxBytes = 1024;
        if (maxBytes > kMaxReadBytes) maxBytes = kMaxReadBytes;
        if (bytes.size() > maxBytes) {
            bytes.resize(maxBytes);
            bytes += "\n...（内容已截断）";
        }
        return FromUtf8(bytes);
    };
    return tool;
}

AgentTool MakeSearchAgentFilesTool() {
    AgentTool tool;
    tool.name = L"searchAgentFiles";
    tool.description =
        L"在允许目录内递归搜索文本（大小写不敏感），返回 路径:行号: 内容。"
        L"pattern 为普通文本；glob 可选（如 *.cpp）用于限定文件类型。" + AllowedRootsHint() + L"";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "path": { "type": "string", "description": "搜索根目录" },
            "pattern": { "type": "string", "description": "要搜索的文本" },
            "glob": { "type": "string", "description": "文件后缀过滤，如 *.cpp" },
            "maxResults": { "type": "integer", "description": "最多结果数，默认 50" }
        },
        "required": ["path", "pattern"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        std::wstring err;
        if (!ParseToolParamJson(paramsJson, params, err).empty())
            return L"[错误] " + err;
        const std::wstring abs = ResolveAbsolutePath(FromUtf8(params.value("path", "")));
        const std::wstring pattern = FromUtf8(params.value("pattern", ""));
        if (abs.empty() || pattern.empty()) return L"[错误] path 与 pattern 不能为空";
        if (!IsPathInAnyRoot(AgentReadRoots(), abs)) {
            return L"[错误] 路径不在允许目录内：" + abs + L"\n" + AllowedRootsHint();
        }
        const std::wstring glob = FromUtf8(params.value("glob", ""));
        std::wstring globExt;
        if (!glob.empty()) {
            const size_t dot = glob.find_last_of(L'.');
            if (dot != std::wstring::npos) globExt = LowerCopy(glob.substr(dot));
        }
        const size_t maxResults =
            (std::min)(size_t(params.value("maxResults", 50)), kMaxSearchResults);
        const std::wstring lowerPattern = LowerCopy(pattern);
        std::wstring out;
        size_t found = 0;
        std::function<void(const std::wstring&)> walk =
            [&](const std::wstring& dir) {
                if (found >= maxResults) return;
                WIN32_FIND_DATAW fd{};
                const std::wstring patternAll = dir + L"\\*";
                HANDLE h = FindFirstFileW(patternAll.c_str(), &fd);
                if (h == INVALID_HANDLE_VALUE) return;
                do {
                    if (found >= maxResults) break;
                    if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0
                        || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
                        continue;
                    }
                    const std::wstring name = fd.cFileName;
                    const std::wstring full = dir + L"\\" + name;
                    if (IsSensitiveAgentPath(full)) continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        walk(full);
                        continue;
                    }
                    if (!globExt.empty()) {
                        const size_t dot = name.find_last_of(L'.');
                        if (dot == std::wstring::npos
                            || LowerCopy(name.substr(dot)) != globExt) {
                            continue;
                        }
                    }
                    std::string bytes;
                    if (!ReadFileBytes(full, bytes) || LooksBinary(bytes)) continue;
                    const std::wstring text = FromUtf8(bytes);
                    const std::wstring lowerText = LowerCopy(text);
                    size_t pos = 0;
                    int line = 1;
                    size_t lineStart = 0;
                    while (pos <= lowerText.size() && found < maxResults) {
                        const size_t hit = lowerText.find(lowerPattern, pos);
                        if (hit == std::wstring::npos) break;
                        while (lineStart < hit) {
                            if (lowerText[lineStart] == L'\n') ++line;
                            ++lineStart;
                        }
                        const size_t eol = text.find(L'\n', hit);
                        std::wstring snippet = text.substr(
                            hit, (eol == std::wstring::npos ? text.size() : eol) - hit);
                        if (snippet.size() > 200) snippet = snippet.substr(0, 200) + L"...";
                        out += full + L":" + std::to_wstring(line) + L": " + snippet + L"\n";
                        ++found;
                        pos = hit + lowerPattern.size();
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            };
        walk(abs);
        if (found == 0) return L"未找到匹配内容：" + pattern;
        return out + L"\n共 " + std::to_wstring(found) + L" 处匹配";
    };
    return tool;
}

AgentTool MakeWriteAgentFileTool() {
    AgentTool tool;
    tool.name = L"writeAgentFile";
    tool.description =
        L"向允许目录写入 UTF-8 文本文件（自动记入撤销日志，可在「撤销」里恢复）。"
        L"允许写入：scripts / recordings / images / library；开发仓库下仅 docs、.cursor/skills、skills。"
        L"禁止写入 build、WebView2Fixed、WebView2UserData、agent_conversations、agent_changes 与根级配置文件。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "path": { "type": "string", "description": "目标文件路径" },
            "content": { "type": "string", "description": "要写入的文本内容" }
        },
        "required": ["path", "content"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        std::wstring err;
        if (!ParseToolParamJson(paramsJson, params, err).empty())
            return L"[错误] " + err;
        const std::wstring abs = ResolveAbsolutePath(FromUtf8(params.value("path", "")));
        const std::wstring content = FromUtf8(params.value("content", ""));
        if (abs.empty()) return L"[错误] path 不能为空";
        if (!IsPathInAnyRoot(AgentWriteRoots(), abs)) {
            return L"[错误] 目标路径不在可写目录内：" + abs;
        }
        if (IsSensitiveAgentPath(abs)) {
            return L"[错误] 该路径受保护，禁止助手写入：" + abs;
        }
        // 写操作前快照（撤销日志）
        const std::wstring before = ReadAll(abs);
        const bool existed = GetFileAttributesW(abs.c_str()) != INVALID_FILE_ATTRIBUTES;
        const std::wstring changeId = AgentUndoBegin(
            L"writeAgentFile", L"写入文件 " + Basename(abs), abs, before, existed);
        const std::string utf8 = ToUtf8(content);
        {
            const size_t slash = abs.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                std::filesystem::create_directories(abs.substr(0, slash));
            std::ofstream f(abs, std::ios::binary | std::ios::trunc);
            if (!f) {
                AgentUndoFinish(changeId, L"", false);
                return L"[错误] 无法写入文件：" + abs;
            }
            f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        }
        AgentUndoFinish(changeId, ReadAll(abs), true);
        return L"已写入：" + abs + L"（" + std::to_wstring(utf8.size())
            + L" 字节）。如需恢复，可在「撤销」中选择本次变更。";
    };
    return tool;
}

AgentTool MakeCopyAgentTextToClipboardTool() {
    AgentTool tool;
    tool.name = L"copyAgentTextToClipboard";
    tool.description =
        L"把指定文本复制到系统剪贴板。用户说「复制...」时使用；超大文本建议截断到合理长度。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "text": { "type": "string", "description": "要复制的文本" }
        },
        "required": ["text"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        std::wstring err;
        if (!ParseToolParamJson(paramsJson, params, err).empty())
            return L"[错误] " + err;
        std::wstring text = FromUtf8(params.value("text", ""));
        if (text.size() > 512 * 1024) text = text.substr(0, 512 * 1024);
        if (!OpenClipboard(nullptr)) return L"[错误] 无法打开剪贴板";
        if (!EmptyClipboard()) {
            CloseClipboard();
            return L"[错误] 无法清空剪贴板";
        }
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool ok = false;
        if (hMem) {
            void* dst = GlobalLock(hMem);
            if (dst) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(hMem);
                ok = SetClipboardData(CF_UNICODETEXT, hMem) != nullptr;
            }
            if (!ok) GlobalFree(hMem);
        }
        CloseClipboard();
        return ok ? L"已复制 " + std::to_wstring(text.size()) + L" 个字符到剪贴板。"
                  : L"[错误] 写入剪贴板失败";
    };
    return tool;
}

AgentTool MakePasteAgentClipboardTextTool() {
    AgentTool tool;
    tool.name = L"pasteAgentClipboardText";
    tool.description = L"读取系统剪贴板中的文本（无文本时返回提示）。";
    tool.parameters_json = LR"({"type":"object","properties":{},"required":[]})";
    tool.execute = [](const std::wstring&) -> std::wstring {
        if (!OpenClipboard(nullptr)) return L"[错误] 无法打开剪贴板";
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (!h) {
            CloseClipboard();
            return L"剪贴板中没有文本。";
        }
        const SIZE_T sizeBytes = GlobalSize(h);
        const wchar_t* src = static_cast<const wchar_t*>(GlobalLock(h));
        if (!src) {
            CloseClipboard();
            return L"[错误] 读取剪贴板失败";
        }
        const size_t maxChars = sizeBytes / sizeof(wchar_t);
        size_t len = 0;
        while (len < maxChars && src[len] != L'\0') ++len;
        std::wstring text(src, len);
        if (text.size() > 512 * 1024) text = text.substr(0, 512 * 1024);
        GlobalUnlock(h);
        CloseClipboard();
        return L"剪贴板文本（" + std::to_wstring(text.size()) + L" 字符）：\n" + text;
    };
    return tool;
}

namespace {

/// 用户「桌面 / 文档 / 下载」——**只给 writeSpreadsheet 用**的额外可写根。
///
/// 为什么需要：`AgentWriteRoots()` 只覆盖 scripts/recordings/images/docs/skills（那是
/// `writeAgentFile` 的边界，刻意很窄）。但「把这堆数据做成 xlsx 放桌面」是办公场景的
/// **主用例**，落在桌面上完全合理 —— 不放宽的话这个工具基本没法用。
/// ⇒ **只给这一个工具**放宽到这三个用户目录，**不去动 `AgentWriteRoots()`**
///   （别顺手把 `writeAgentFile` 也放宽了，那是另一件事）。
/// ⚠ 放宽的只是「在哪儿写」：`IsSensitiveAgentPath()` 仍拦 app_settings.json / .exe /
///   仓库内部文件；`create` 默认不覆盖；`setCells` 改前先落字节级备份。
std::vector<std::wstring> SpreadsheetWriteRoots() {
    std::vector<std::wstring> roots = AgentWriteRoots();
    for (REFKNOWNFOLDERID id : { FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads }) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
            roots.push_back(NormalizeSlashes(raw));
            CoTaskMemFree(raw);
        }
    }
    return roots;
}

}  // namespace

// ── writeSpreadsheet：xlsx 读写（**不依赖 Office、不依赖任何第三方库**）────────────
// 为什么值得单独做一个工具：在此之前，助手要写 .xlsx 只有一条路 —— `runCommand` + Excel COM，
// 也就是**必须装 Office**。没装就只能出 CSV。这里用自研的 `src/ooxml/` 补上。
//
// 两种模式的分工（**这不是可有可无的选项，是安全边界**）：
//   · `create`  新建文件。默认**拒绝覆盖**已存在的文件（`overwrite=true` 才覆盖）。
//               撤销走撤销日志：`existed=false` ⇒ 恢复 = 删掉这个文件。**二进制无关**，所以成立。
//   · `setCells` 改已有文件的指定单元格。⚠ **字节保留**：只改那几个 `<c>`，
//               图表 / 公式 / 样式 / 别的表一字不动（见 src/ooxml/xlsx_doc.h 的说明）。
//               ⚠⚠ **改前必须先落一份字节级备份**：撤销日志存的是 UTF-8 文本，装不下二进制 xlsx
//                  —— 拿它当撤销会**把用户的文件清空**。所以这里备份到我们自己的目录，
//                  并把路径明确告诉用户（不假装它进了「撤销」列表）。
AgentTool MakeWriteSpreadsheetTool() {
    AgentTool tool;
    tool.name = L"writeSpreadsheet";
    tool.description =
        L"写 Excel 文件（.xlsx）。**不需要装 Office**。两种模式：\n"
        L"· mode=\"create\"（默认）：新建一个 xlsx，用 `tsv` 填内容（制表符分隔，首行通常是表头）。\n"
        L"  ⚠ 目标文件已存在时**默认拒绝**，要覆盖得显式传 overwrite=true。\n"
        L"· mode=\"setCells\"：改**已有**文件的指定单元格。**只动那几个格子** ——\n"
        L"  图表 / 公式 / 条件格式 / 样式 / 别的表**原样保留**（这是「另存一份」做不到的）。\n"
        L"  ⚠ 改前会自动备份一份原始文件，路径在返回里给你。\n"
        L"写完会把结果读回来给你核对（不用另开工具）。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "path": { "type": "string", "description": "目标 .xlsx 路径" },
            "mode": { "type": "string", "enum": ["create", "setCells"], "description": "默认 create" },
            "sheet": { "type": "string", "description": "工作表名（可选；create 时默认 Sheet1，setCells 时默认第一张表）" },
            "tsv": { "type": "string", "description": "create 用：制表符分隔的内容，一行一记录" },
            "overwrite": { "type": "boolean", "description": "create 时是否允许覆盖已存在文件，默认 false" },
            "cells": {
                "type": "array",
                "description": "setCells 用：要改的单元格",
                "items": {
                    "type": "object",
                    "properties": {
                        "ref": { "type": "string", "description": "如 B2" },
                        "text": { "type": "string" },
                        "number": { "type": "number" },
                        "bool": { "type": "boolean" },
                        "formula": { "type": "string", "description": "不含前导 = 的公式原文" },
                        "cached": { "type": "string", "description": "公式的缓存值（可选；不给就留空，Excel 打开时重算）" }
                    },
                    "required": ["ref"]
                }
            }
        },
        "required": ["path"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        std::wstring err;
        if (!ParseToolParamJson(paramsJson, params, err).empty()) return L"[错误] " + err;

        const std::wstring abs = ResolveAbsolutePath(FromUtf8(params.value("path", "")));
        if (abs.empty()) return L"[错误] path 不能为空";
        const std::wstring low = LowerCopy(abs);
        if (low.size() < 5
            || (low.rfind(L".xlsx") != low.size() - 5 && low.rfind(L".xlsm") != low.size() - 5)) {
            return L"[错误] 只支持 .xlsx / .xlsm（老格式 .xls 请先用 Excel 另存为 .xlsx）。目标：" + abs;
        }
        if (!IsPathInAnyRoot(SpreadsheetWriteRoots(), abs)) {
            return L"[错误] 目标路径不在可写目录内（脚本/录制/图片/文档/技能 或 桌面/文档/下载）：" + abs;
        }
        if (IsSensitiveAgentPath(abs)) {
            return L"[错误] 该路径受保护，禁止助手写入：" + abs;
        }

        const std::wstring mode = FromUtf8(params.value("mode", "create"));
        const bool exists = GetFileAttributesW(abs.c_str()) != INVALID_FILE_ATTRIBUTES;

        qst::ooxml::XlsxDoc doc;
        if (mode == L"create") {
            if (exists && !params.value("overwrite", false)) {
                return L"[错误] 文件已存在，未覆盖：" + abs
                    + L"。要覆盖请显式传 overwrite=true（或换一个文件名）。";
            }
            std::wstring sheet = FromUtf8(params.value("sheet", ""));
            if (sheet.empty()) sheet = L"Sheet1";
            std::string cerr;
            if (!qst::ooxml::XlsxDoc::CreateNew(ToUtf8(sheet), doc, cerr)) {
                return L"[错误] 新建 xlsx 失败：" + FromUtf8(cerr);
            }
        } else if (mode == L"setCells") {
            if (!exists) {
                return L"[错误] 文件不存在：" + abs + L"（要新建请用 mode=create）";
            }
            std::string cerr;
            if (!doc.LoadFromFile(abs, cerr)) {
                return L"[错误] 打不开这个 xlsx：" + FromUtf8(cerr)
                    + L"。加密文件 / 老 .xls / 损坏文件都读不了。";
            }
        } else {
            return L"[错误] mode 只能是 create 或 setCells，收到：" + mode;
        }

        // 选表
        std::wstring sheetName = FromUtf8(params.value("sheet", ""));
        if (sheetName.empty()) {
            if (doc.Sheets().empty()) return L"[错误] 这个工作簿里没有工作表";
            sheetName = FromUtf8(doc.Sheets()[0].name);
        } else if (doc.FindSheet(ToUtf8(sheetName)) == nullptr) {
            std::wstring have;
            for (const auto& s : doc.Sheets()) {
                if (!have.empty()) have += L"、";
                have += FromUtf8(s.name);
            }
            return L"[错误] 没有这张工作表：" + sheetName + L"。现有：" + have;
        }

        int written = 0;
        if (mode == L"create") {
            const std::wstring tsv = FromUtf8(params.value("tsv", ""));
            if (tsv.empty()) return L"[错误] create 模式需要 tsv（制表符分隔的内容）";
            int row = 1;
            size_t pos = 0;
            while (pos <= tsv.size()) {
                size_t nl = tsv.find(L'\n', pos);
                if (nl == std::wstring::npos) nl = tsv.size();
                std::wstring line = tsv.substr(pos, nl - pos);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                int col = 0;
                size_t cp = 0;
                while (cp <= line.size()) {
                    size_t tab = line.find(L'\t', cp);
                    if (tab == std::wstring::npos) tab = line.size();
                    std::wstring field = line.substr(cp, tab - cp);
                    const std::string ref =
                        qst::ooxml::ColumnIndexToName(col) + std::to_string(row);
                    std::string cerr;
                    if (!field.empty()) {
                        // 数字自动识别：整串是数字（且没有前导 0，避免把「007」「手机号」变成数）
                        const std::wstring t = Trim(field);
                        bool isNum = !t.empty();
                        for (wchar_t ch : t) {
                            if (!((ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'.'
                                || ch == L'e' || ch == L'E' || ch == L'+')) { isNum = false; break; }
                        }
                        if (isNum && t.size() > 1 && t[0] == L'0' && t[1] != L'.') isNum = false;
                        if (isNum) {
                            doc.SetCellNumber(ToUtf8(sheetName), ref, _wtof(t.c_str()), cerr);
                        } else {
                            doc.SetCellText(ToUtf8(sheetName), ref, ToUtf8(field), cerr);
                        }
                        ++written;
                    }
                    if (tab >= line.size()) break;
                    cp = tab + 1;
                    ++col;
                }
                if (nl >= tsv.size()) break;
                pos = nl + 1;
                ++row;
            }
        } else {
            if (!params.contains("cells") || !params["cells"].is_array()
                || params["cells"].empty()) {
                return L"[错误] setCells 模式需要 cells 数组";
            }
            for (const auto& c : params["cells"]) {
                if (!c.is_object()) continue;
                const std::wstring ref = FromUtf8(c.value("ref", ""));
                if (ref.empty()) return L"[错误] cells 里有一项缺 ref";
                std::string cerr;
                bool ok = false;
                if (c.contains("formula") && c["formula"].is_string()) {
                    ok = doc.SetCellFormula(ToUtf8(sheetName), ToUtf8(ref),
                        c["formula"].get<std::string>(), c.value("cached", ""), cerr);
                } else if (c.contains("number") && c["number"].is_number()) {
                    ok = doc.SetCellNumber(ToUtf8(sheetName), ToUtf8(ref),
                        c["number"].get<double>(), cerr);
                } else if (c.contains("bool") && c["bool"].is_boolean()) {
                    qst::ooxml::CellValue v;
                    v.kind = qst::ooxml::CellKind::Bool;
                    v.boolean = c["bool"].get<bool>();
                    ok = doc.SetCell(ToUtf8(sheetName), ToUtf8(ref), v, cerr);
                } else if (c.contains("text") && c["text"].is_string()) {
                    ok = doc.SetCellText(ToUtf8(sheetName), ToUtf8(ref),
                        c["text"].get<std::string>(), cerr);
                } else {
                    qst::ooxml::CellValue v;   // 清空该格
                    ok = doc.SetCell(ToUtf8(sheetName), ToUtf8(ref), v, cerr);
                }
                if (!ok) return L"[错误] 写 " + ref + L" 失败：" + FromUtf8(cerr);
                ++written;
            }
        }

        // ★ setCells 改的是**用户的文件**，而撤销日志装不下二进制 ⇒ 改前先落字节级备份
        std::wstring backupPath;
        if (mode == L"setCells") {
            std::wstring dir = AgentChangesDir();
            EnsureDirectoryTree(dir);
            const std::wstring base = Basename(abs);
            std::wstring stamp = NowText();
            for (auto& ch : stamp) { if (ch == L':' || ch == L' ' || ch == L'/') ch = L'-'; }
            backupPath = dir + L"\\xlsx_backup_" + base + L"_" + stamp + L".xlsx";
            std::vector<uint8_t> original;
            if (!qst::ooxml::ReadZipFileBytes(abs, original)
                || !qst::ooxml::WriteZipFileBytes(backupPath, original)) {
                return L"[错误] 备份原始文件失败，**已中止**（不敢在没有退路的情况下改你的文件）："
                    + backupPath;
            }
        }

        std::string serr;
        if (!doc.SaveToFile(abs, serr)) {
            return L"[错误] 写入失败：" + FromUtf8(serr);
        }

        // 撤销登记：只有「新建」是二进制无关的（existed=false ⇒ 恢复=删除文件）。
        std::wstring undoNote;
        if (mode == L"create") {
            const std::wstring id = AgentUndoBegin(L"writeSpreadsheet",
                L"新建表格 " + Basename(abs), abs, std::wstring(), false);
            if (!id.empty()) {
                AgentUndoFinish(id, std::wstring(), true);
                undoNote = L"\n可在「撤销」里恢复（会删掉这个文件）。";
            }
        } else {
            undoNote = L"\n⚠ 原始文件已备份到：" + backupPath
                + L"\n（xlsx 是二进制，撤销日志装不下，所以走备份文件；要还原就把它复制回去。）";
        }

        // 产物可验收：写完读回来给模型核对（不用再调一次读工具）
        std::string dump;
        std::string derr;
        std::wstring verify = L"\n[读回核对失败：" ;
        if (doc.DumpSheetAsTsv(ToUtf8(sheetName), 50, 20, dump, derr)) {
            verify = L"\n[读回核对 " + sheetName + L"]\n" + FromUtf8(dump);
        } else {
            verify += FromUtf8(derr) + L"]";
        }
        return L"[已写入 " + std::to_wstring(written) + L" 个单元格] " + abs
            + L"\n工作表：" + sheetName + undoNote + verify;
    };
    return tool;
}
