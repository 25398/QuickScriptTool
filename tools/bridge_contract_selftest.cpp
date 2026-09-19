// =============================================================================
// BridgeContractSelfTest — JS ↔ C++ 桥接命令契约（架构评估 #10 / 验收报告 §7.2 C）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:BridgeContractSelfTest
//   build\Release\BridgeContractSelfTest.exe --json
//
// 为什么有这个 suite：
//   2026-09-18 的 P0（saveSettings 静默失效）就长在桥接缝上——JS 发的消息形状与
//   C++ 解析的严格度之间是**隐式契约**，两侧各写各的字符串字面量，没有任何东西
//   校验它们对得上。本次把 C++ 入站命令面显式声明进 src/webview/bridge_commands.h，
//   本 suite 做**双向**校验：
//
//     表 ──①──► C++ 分派（表里每条，分派里必须真有分支）
//     表 ──②──► JS 发送侧（表里每条，JS 必须真的会发）      ← 用户要求的核心断言
//     C++ 分派 ──③──► 表 ∪ 例外（分派里每条都要登记，防"加了 JS 调用忘加 C++ 分支"）
//     例外 ──④──► 必须写明理由，且当前 JS 不发（一旦发了就要升级为正式命令）
//
// 怎么提取（两侧都要，且必须避免误报）：
//   C++：`type == "X"`（qst_webview_shell.cpp）
//   JS ：分两类文件
//        · **专用发送方**（bridge.js / debug.html / agent.html）——整文件只发桥接
//          命令，取全部 `type: "X"`；
//        · **混合文件**（app.js / pro-mode.js / visual_editor.js / index.html）——
//          里面还有脚本动作 JSON（{type:"loop"} 等）与本地合成消息
//          （Object.assign({type:"crosshairPick.result"}, …)），必须只取
//          **出现在 post(...) / postMessage(...) / qst.post(...) 实参里**的字面量。
//        实测：不加这个区分，会出现 loop/else/macro/ai/paint 等 7 个假阳性。
//
// 源码不在时（例如把 build 目录拷走单独跑）自动跳过，判 ok=true ——
// 与 VirtualHidSelfTest 读驱动脚本的做法一致，避免假红。
// =============================================================================
#include "selftest_harness.h"

#include "webview/bridge_commands.h"

#include <windows.h>

#include <cstring>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"table_no_duplicates", L"default",
        L"命令表无重名；正式表与例外表不交集"},
    {L"table_all_handled_in_shell", L"default",
        L"表里每条命令，qst_webview_shell.cpp 的入站分派里必须真有分支"},
    {L"table_all_sent_by_js", L"default",
        L"表里每条命令，JS 侧必须真的会发（防加了 C++ 分支但没人调）"},
    {L"table_sender_files_exist", L"default",
        L"表里登记的 sender 文件必须存在于 ui/ 下（防拼错文件名）"},
    {L"shell_dispatch_all_declared", L"default",
        L"C++ 分派里的每条命令都要登记（表或例外）——防加了 JS 调用忘加 C++ 分支"},
    {L"cpp_only_all_handled_and_reasoned", L"default",
        L"例外表每条都要有理由，且确实出现在 C++ 分派里"},
    {L"cpp_only_not_sent_by_js", L"default",
        L"例外表里的命令当前不应被 JS 发；一旦被发就该升级成正式命令"},
    {L"js_sends_all_declared", L"default",
        L"JS 实际发的每条命令都要在表里（反向：防 JS 发了没人处理的孤儿命令）"},
};

// ── 文件读取 ─────────────────────────────────────────────────────
std::wstring DirName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    const wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring FindRepoRoot() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = DirName(exe);
    for (int i = 0; i < 6; ++i) {
        if (GetFileAttributesW(JoinPath(dir, L"CMakeLists.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
            return dir;
        }
        const std::wstring parent = DirName(dir);
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

bool ReadAllBytes(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (1 << 26)) {
        CloseHandle(h);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out->empty()
        ? TRUE
        : ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out->resize(read);
    // 去掉 UTF-8 BOM
    if (out->size() >= 3 && static_cast<unsigned char>((*out)[0]) == 0xEF
        && static_cast<unsigned char>((*out)[1]) == 0xBB
        && static_cast<unsigned char>((*out)[2]) == 0xBF) {
        out->erase(0, 3);
    }
    return true;
}

std::string ToNarrow(const wchar_t* w) {
    std::string s;
    for (const wchar_t* p = w; p && *p; ++p) {
        s.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
    }
    return s;
}

std::wstring ToWide(const char* a) {
    std::wstring s;
    for (const char* p = a; p && *p; ++p) s.push_back(static_cast<wchar_t>(*p));
    return s;
}

// ── 提取 ─────────────────────────────────────────────────────────
/// C++ 入站分派：`type == "X"`
std::set<std::string> ExtractCppDispatch(const std::string& text) {
    std::set<std::string> out;
    const std::regex re(R"RX(type\s*==\s*"([A-Za-z0-9_.]+)")RX");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

/// 宽松：整文件所有 `type: "X"`（只用于专用发送方文件）
std::set<std::string> ExtractTypeLiterals(const std::string& text) {
    std::set<std::string> out;
    const std::regex re(R"RX(type\s*:\s*"([A-Za-z0-9_.]+)")RX");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

/// 严格：只取出现在 post( / postMessage( / qst.post( 实参里的 `type: "X"`
std::set<std::string> ExtractPostedTypeLiterals(const std::string& text) {
    std::set<std::string> out;
    const std::regex callRe(R"RX((post|postMessage|qst\.post)\s*\()RX");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), callRe);
         it != std::sregex_iterator(); ++it) {
        const size_t open = static_cast<size_t>((*it).position() + (*it).length()) - 1;
        int depth = 0;
        size_t j = open;
        bool inStr = false;
        char quote = 0;
        for (; j < text.size(); ++j) {
            const char c = text[j];
            if (inStr) {
                if (c == '\\') { ++j; continue; }
                if (c == quote) inStr = false;
                continue;
            }
            if (c == '"' || c == '\'') { inStr = true; quote = c; continue; }
            if (c == '(') ++depth;
            else if (c == ')') {
                --depth;
                if (depth == 0) break;
            }
        }
        const std::string arg = text.substr(open, j - open + 1);
        const std::regex litRe(R"RX(type\s*:\s*"([A-Za-z0-9_.]+)")RX");
        for (auto li = std::sregex_iterator(arg.begin(), arg.end(), litRe);
             li != std::sregex_iterator(); ++li) {
            out.insert((*li)[1].str());
        }
    }
    return out;
}

/// 专用发送方：整文件只发桥接命令，宽松规则即可。
const char* const kDedicatedSenders[] = {"bridge.js", "debug.html", "agent.html"};
/// 混合文件：含脚本动作 JSON 与本地合成消息，必须用严格规则。
const char* const kMixedSenders[] = {"app.js", "pro-mode.js", "visual_editor.js", "index.html"};

struct ContractData {
    bool available = false;
    std::set<std::string> cppDispatch;
    std::set<std::string> jsSent;
    std::vector<std::string> missingSources;
};

ContractData LoadContractData() {
    ContractData d;
    const std::wstring root = FindRepoRoot();
    if (root.empty()) {
        d.missingSources.push_back("repo-root");
        return d;
    }
    const std::wstring shellPath = JoinPath(JoinPath(root, L"src\\webview"), L"qst_webview_shell.cpp");
    std::string shell;
    if (!ReadAllBytes(shellPath, &shell)) {
        d.missingSources.push_back("qst_webview_shell.cpp");
        return d;
    }
    d.cppDispatch = ExtractCppDispatch(shell);

    const std::wstring uiDir = JoinPath(root, L"ui");
    size_t read = 0;
    for (const char* f : kDedicatedSenders) {
        std::string t;
        const std::wstring p = JoinPath(uiDir, ToWide(f));
        if (!ReadAllBytes(p, &t)) continue;
        ++read;
        const auto s = ExtractTypeLiterals(t);
        d.jsSent.insert(s.begin(), s.end());
    }
    for (const char* f : kMixedSenders) {
        std::string t;
        const std::wstring p = JoinPath(uiDir, ToWide(f));
        if (!ReadAllBytes(p, &t)) continue;
        ++read;
        const auto s = ExtractPostedTypeLiterals(t);
        d.jsSent.insert(s.begin(), s.end());
    }
    if (read == 0) {
        d.missingSources.push_back("ui/*.js");
        return d;
    }
    d.available = true;
    return d;
}

// ── 用例 ─────────────────────────────────────────────────────────
std::set<std::string> TableNames() {
    std::set<std::string> t;
    for (const auto& c : qst::webview::kBridgeJsCommands) t.insert(ToNarrow(c.name));
    return t;
}

std::set<std::string> CppOnlyNames() {
    std::set<std::string> t;
    for (const auto& c : qst::webview::kBridgeCppOnlyCommands) t.insert(ToNarrow(c.name));
    return t;
}

std::set<std::string> DeclaredNames() {
    std::set<std::string> t = TableNames();
    const auto o = CppOnlyNames();
    t.insert(o.begin(), o.end());
    return t;
}

void CaseTableNoDuplicates() {
    std::set<std::string> seen;
    std::vector<std::string> dups;
    for (const auto& c : qst::webview::kBridgeJsCommands) {
        const std::string n = ToNarrow(c.name);
        if (!seen.insert(n).second) dups.push_back(n);
    }
    std::set<std::string> seenOnly;
    std::vector<std::string> dupsOnly;
    for (const auto& c : qst::webview::kBridgeCppOnlyCommands) {
        const std::string n = ToNarrow(c.name);
        if (!seenOnly.insert(n).second) dupsOnly.push_back(n);
    }
    std::vector<std::string> overlap;
    for (const auto& n : seenOnly) {
        if (seen.count(n)) overlap.push_back(n);
    }
    const bool ok = dups.empty() && dupsOnly.empty() && overlap.empty();
    std::wstring detail;
    if (!ok) {
        detail = L"重复/交集：";
        for (const auto& n : dups) detail += L" dup(" + std::wstring(n.begin(), n.end()) + L")RX";
        for (const auto& n : overlap) detail += L" both(" + std::wstring(n.begin(), n.end()) + L")RX";
    } else {
        detail = L"正式表 " + std::to_wstring(qst::webview::kBridgeJsCommandCount)
            + L" 条 / 例外表 " + std::to_wstring(qst::webview::kBridgeCppOnlyCommandCount) + L" 条";
    }
    Emit(L"table_no_duplicates", ok, detail.c_str());
}

void CaseTableAllHandledInShell(const ContractData& d) {
    if (!d.available) { Emit(L"table_all_handled_in_shell", true, L"跳过：源码不可达"); return; }
    std::vector<std::string> missing;
    for (const auto& c : qst::webview::kBridgeJsCommands) {
        const std::string n = ToNarrow(c.name);
        if (!d.cppDispatch.count(n)) missing.push_back(n);
    }
    const bool ok = missing.empty();
    std::wstring detail;
    if (ok) detail = L"表内 " + std::to_wstring(qst::webview::kBridgeJsCommandCount)
        + L" 条全部有入站分支";
    else {
        detail = L"表里有命令但 C++ 分派没有（会静默无响应）：";
        for (const auto& n : missing) detail += L"\n      " + std::wstring(n.begin(), n.end());
    }
    Emit(L"table_all_handled_in_shell", ok, detail.c_str());
}

void CaseTableAllSentByJs(const ContractData& d) {
    if (!d.available) { Emit(L"table_all_sent_by_js", true, L"跳过：源码不可达"); return; }
    std::vector<std::string> neverSent;
    for (const auto& c : qst::webview::kBridgeJsCommands) {
        const std::string n = ToNarrow(c.name);
        if (!d.jsSent.count(n)) neverSent.push_back(n);
    }
    const bool ok = neverSent.empty();
    std::wstring detail;
    if (ok) detail = L"表内每条命令 JS 侧都真的会发";
    else {
        detail = L"表里登记了但没有任何 JS 会发（要么该删，要么该移进例外表）：";
        for (const auto& n : neverSent) detail += L"\n      " + std::wstring(n.begin(), n.end());
    }
    Emit(L"table_all_sent_by_js", ok, detail.c_str());
}

void CaseTableSenderFilesExist(const ContractData& d) {
    const std::wstring root = FindRepoRoot();
    if (root.empty() || !d.available) {
        Emit(L"table_sender_files_exist", true, L"跳过：源码不可达");
        return;
    }
    const std::wstring uiDir = JoinPath(root, L"ui");
    std::vector<std::string> missing;
    std::set<std::string> checked;
    for (const auto& c : qst::webview::kBridgeJsCommands) {
        const std::string f = ToNarrow(c.sender);
        if (!checked.insert(f).second) continue;
        const std::wstring p = JoinPath(uiDir, ToWide(f.c_str()));
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) missing.push_back(f);
    }
    const bool ok = missing.empty();
    std::wstring detail;
    if (ok) detail = L"sender 文件均在 ui/ 下（" + std::to_wstring(checked.size()) + L" 个）";
    else {
        detail = L"表里登记的 sender 文件不存在：";
        for (const auto& f : missing) detail += L"\n      " + ToWide(f.c_str());
    }
    Emit(L"table_sender_files_exist", ok, detail.c_str());
}

void CaseShellDispatchAllDeclared(const ContractData& d) {
    if (!d.available) { Emit(L"shell_dispatch_all_declared", true, L"跳过：源码不可达"); return; }
    const std::set<std::string> declared = DeclaredNames();
    std::vector<std::string> undeclared;
    for (const auto& n : d.cppDispatch) {
        if (!declared.count(n)) undeclared.push_back(n);
    }
    const bool ok = undeclared.empty();
    std::wstring detail;
    if (ok) detail = L"C++ 分派 " + std::to_wstring(d.cppDispatch.size())
        + L" 条全部已登记（表 " + std::to_wstring(qst::webview::kBridgeJsCommandCount)
        + L" + 例外 " + std::to_wstring(qst::webview::kBridgeCppOnlyCommandCount) + L"）";
    else {
        detail = L"C++ 分派里有未登记的命令（新增命令请同步 bridge_commands.h）：";
        for (const auto& n : undeclared) detail += L"\n      " + std::wstring(n.begin(), n.end());
    }
    Emit(L"shell_dispatch_all_declared", ok, detail.c_str());
}

void CaseCppOnlyAllHandledAndReasoned() {
    std::vector<std::string> noReason;
    for (const auto& c : qst::webview::kBridgeCppOnlyCommands) {
        if (!c.reason || !*c.reason) noReason.push_back(ToNarrow(c.name));
    }
    const ContractData d = LoadContractData();
    std::vector<std::string> notHandled;
    if (d.available) {
        for (const auto& c : qst::webview::kBridgeCppOnlyCommands) {
            const std::string n = ToNarrow(c.name);
            if (!d.cppDispatch.count(n)) notHandled.push_back(n);
        }
    }
    const bool ok = noReason.empty() && notHandled.empty();
    std::wstring detail;
    if (ok) detail = L"例外 " + std::to_wstring(qst::webview::kBridgeCppOnlyCommandCount)
        + L" 条均有理由且确有分派";
    else {
        detail = L"例外表问题：";
        for (const auto& n : noReason) detail += L"\n      缺理由：" + std::wstring(n.begin(), n.end());
        for (const auto& n : notHandled) detail += L"\n      无对应分派：" + std::wstring(n.begin(), n.end());
    }
    Emit(L"cpp_only_all_handled_and_reasoned", ok, detail.c_str());
}

void CaseCppOnlyNotSentByJs(const ContractData& d) {
    if (!d.available) { Emit(L"cpp_only_not_sent_by_js", true, L"跳过：源码不可达"); return; }
    std::vector<std::string> nowSent;
    for (const auto& c : qst::webview::kBridgeCppOnlyCommands) {
        const std::string n = ToNarrow(c.name);
        if (d.jsSent.count(n)) nowSent.push_back(n);
    }
    const bool ok = nowSent.empty();
    std::wstring detail;
    if (ok) detail = L"例外表里的命令当前都无人发（别名/死分支状态未变）";
    else {
        detail = L"以下例外命令现在 JS 真的会发了 → 应升级进 kBridgeJsCommands：";
        for (const auto& n : nowSent) detail += L"\n      " + std::wstring(n.begin(), n.end());
    }
    Emit(L"cpp_only_not_sent_by_js", ok, detail.c_str());
}

void CaseJsSendsAllDeclared(const ContractData& d) {
    if (!d.available) { Emit(L"js_sends_all_declared", true, L"跳过：源码不可达"); return; }
    const std::set<std::string> declared = DeclaredNames();
    std::vector<std::string> orphans;
    for (const auto& n : d.jsSent) {
        if (!declared.count(n)) orphans.push_back(n);
    }
    const bool ok = orphans.empty();
    std::wstring detail;
    if (ok) detail = L"JS 实际发的 " + std::to_wstring(d.jsSent.size())
        + L" 条全部已登记（无孤儿命令）";
    else {
        detail = L"JS 发了但 C++ 不处理 / 未登记（会静默无响应）：";
        for (const auto& n : orphans) detail += L"\n      " + std::wstring(n.begin(), n.end());
    }
    Emit(L"js_sends_all_declared", ok, detail.c_str());
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"BridgeContractSelfTest — JS ↔ C++ 桥接命令契约\n"
        L"\n"
        L"用法:\n"
        L"  BridgeContractSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"契约表: src/webview/bridge_commands.h\n"
        L"背景:   docs/refactor-acceptance.md §3（P0）与 §7.2 C 段（#10）\n"
        L"Agent:  .cursor/skills/module-selftest/SKILL.md\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i] ? argv[i] : L"";
        if (a == L"--json") {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--list") {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--help" || a == L"-h") {
            PrintHelp();
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"BridgeContractSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    const ContractData d = LoadContractData();

    CaseTableNoDuplicates();
    CaseTableAllHandledInShell(d);
    CaseTableAllSentByJs(d);
    CaseTableSenderFilesExist(d);
    CaseShellDispatchAllDeclared(d);
    CaseCppOnlyAllHandledAndReasoned();
    CaseCppOnlyNotSentByJs(d);
    CaseJsSendsAllDeclared(d);

    selftest::EmitSummary();
    return selftest::ExitCode();
}
