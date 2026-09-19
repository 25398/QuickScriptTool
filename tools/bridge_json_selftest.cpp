// =============================================================================
// BridgeJsonSelfTest — 桥接 JSON 边界契约（saveSettings 回归锁）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:BridgeJsonSelfTest
//   build\Release\BridgeJsonSelfTest.exe --json
//
// 为什么有这个 suite（docs/refactor-acceptance.md §7.3）：
//   2026-09-18 的 P0 —— 保存设置静默失效。链路是
//     bridge.js 发 {"type":"saveSettings","settings":{...}}
//     → 壳用 substr(首个 '{') 截到**末尾**，payload 多带一个外层 '}'
//     → 非法 JSON；旧的手写字符扫描容忍它，换成 nlohmann 严格解析后
//       所有键都取不到 → 各字段保持旧值 → 写回盘 → 仍报「保存成功」
//   641 个既有用例全绿也照不到这里，因为桥接层零覆盖。
//
//   本 suite 把「JS 消息形状 ↔ C++ 解析严格度」这条缝钉死：
//     - 5 条断言覆盖 §7.3 规定的正常路径与边界（键不在末尾、值含 '}'、
//       嵌套对象、键缺失/null、非法 JSON 不抛异常）；
//     - 另加 3 条把本次事故的**原始形状**与**诊断可观测性**也锁住。
//
// 为什么只链 qst_utils 而不是 qst_engine：
//   被修的逻辑已经上收到 src/json_util.h（纯 header，无引擎依赖），
//   所以这个契约测试不需要引擎。这样 B 段删掉 tools/engine_link_stubs.cpp
//   之后本测试仍可独立链接——桥接契约测试不该依赖「引擎能链上」这件事。
// =============================================================================
#include "selftest_harness.h"

#include "json_util.h"

#include <string>
#include <vector>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"subobject_last_key", L"default",
        L"settings 是最后一个键：子对象必须能被严格解析，字段取得到"},
    {L"subobject_not_last_key", L"default",
        L"settings 后面还有别的键：仍取到配对的子对象"},
    {L"subobject_brace_and_nested", L"default",
        L"值里含 '}' 字符与嵌套对象/数组：不能被括号扫描带偏"},
    {L"subobject_missing_or_null", L"default",
        L"键缺失或为 null：返回 false（不误报成功）"},
    {L"subobject_invalid_json_no_throw", L"default",
        L"整条 JSON 非法：返回 false 且不抛异常"},
    {L"regression_trailing_outer_brace", L"default",
        L"事故原始形状（payload 多带外层 '}'）：严格解析必须拒绝，不得静默取到旧值"},
    {L"subobject_applyable_fields", L"default",
        L"子对象文本可被 GetInt/GetBool/GetNumber/GetStringArray 直接消费"},
    {L"diagnostics_silent_on_valid", L"default",
        L"合法输入不产生解析失败诊断（避免日志噪音）"},
    {L"diagnostics_reports_invalid", L"default",
        L"非法输入必须上报诊断并带上下文标签（静默失败不可再出现）"},
};

// ── 诊断探针 ─────────────────────────────────────────────────────
struct SinkProbe {
    int calls = 0;
    std::string lastWhere;
    std::string lastText;
};

SinkProbe g_probe;

void ProbeSink(const char* where, const std::string& text, size_t /*offset*/) {
    ++g_probe.calls;
    g_probe.lastWhere = where ? where : "";
    g_probe.lastText = text;
}

void ResetProbe() {
    g_probe = SinkProbe{};
    qst::jsonutil::SetParseFailureSink(&ProbeSink);
    qst::jsonutil::ResetParseFailureCount();
}

// 1) settings 是最后一个键（bridge.js 的实际形状）
void CaseSubObjectLastKey() {
    const std::string msg = R"({"type":"saveSettings","settings":{"themeId":7}})";
    std::string payload;
    const bool got = qst::jsonutil::GetSubObjectText(msg, "settings", payload, "test");
    int themeId = -1;
    const bool read = got && qst::jsonutil::GetInt(payload, "themeId", themeId);
    const bool ok = got && read && themeId == 7;
    Emit(L"subobject_last_key", ok,
        ok ? L"themeId=7" : L"子对象取不到或字段读不出（事故形状）");
}

// 2) settings 后面还有别的键
void CaseSubObjectNotLastKey() {
    const std::string msg = R"({"settings":{"themeId":7},"type":"saveSettings"})";
    std::string payload;
    const bool got = qst::jsonutil::GetSubObjectText(msg, "settings", payload, "test");
    int themeId = -1;
    const bool read = got && qst::jsonutil::GetInt(payload, "themeId", themeId);
    const bool ok = got && read && themeId == 7;
    Emit(L"subobject_not_last_key", ok,
        ok ? L"themeId=7" : L"键不在末尾时子对象取错");
}

// 3) 值里含 '}' 字符 + 嵌套对象/数组
void CaseSubObjectBraceAndNested() {
    const std::string msg =
        R"({"settings":{"s":"a}b}c","nested":{"k":[1,2]},"q":"\"引号"},"x":1})";
    std::string payload;
    const bool got = qst::jsonutil::GetSubObjectText(msg, "settings", payload, "test");
    std::string s;
    const bool readS = got && qst::jsonutil::GetStringField(payload, "s", s);
    int q0 = 0;
    const bool readNested = readS
        && [&]() {
               nlohmann::json d;
               if (!qst::jsonutil::TryParse(payload, d)) return false;
               const auto it = d.find("nested");
               if (it == d.end() || !it->is_object()) return false;
               const auto k = it->find("k");
               if (k == it->end() || !k->is_array() || k->size() != 2) return false;
               q0 = (*k)[0].get<int>();
               return true;
           }();
    const bool ok = got && readS && s == "a}b}c" && readNested && q0 == 1;
    Emit(L"subobject_brace_and_nested", ok,
        ok ? L"字符串内 '}' 与嵌套结构均正确"
           : L"括号扫描会在这里出错（字符串内 '}' 提前截断）");
}

// 4) 键缺失 / 为 null
void CaseSubObjectMissingOrNull() {
    std::string payload;
    const bool missing = qst::jsonutil::GetSubObjectText(
        R"({"type":"saveSettings"})", "settings", payload, "test");
    const bool nulled = qst::jsonutil::GetSubObjectText(
        R"({"type":"saveSettings","settings":null})", "settings", payload, "test");
    const bool scalar = qst::jsonutil::GetSubObjectText(
        R"({"settings":123})", "settings", payload, "test");
    const bool ok = !missing && !nulled && !scalar;
    Emit(L"subobject_missing_or_null", ok,
        ok ? L"缺失/null/非对象均返回 false"
           : L"缺失或 null 时错误地返回了成功");
}

// 5) 非法 JSON：返回 false 且不抛异常
void CaseSubObjectInvalidJson() {
    bool threw = false;
    bool got = true;
    try {
        std::string payload;
        got = qst::jsonutil::GetSubObjectText(R"({"settings":{)", "settings", payload, "test");
        got = got || qst::jsonutil::GetSubObjectText("not json at all", "settings", payload, "test");
    } catch (...) {
        threw = true;
    }
    const bool ok = !threw && !got;
    Emit(L"subobject_invalid_json_no_throw", ok,
        ok ? L"截断/非 JSON 均安全返回 false" : L"非法 JSON 抛异常或误判成功");
}

// 6) 事故原始形状：payload 多带一个外层 '}'
void CaseRegressionTrailingBrace() {
    // 旧实现 substr(brace) 会产出这个（末尾多一个 '}'）
    const std::string brokenPayload = R"({"themeId":7}} )";
    const bool brokenRejected = !qst::jsonutil::IsParseableObject(brokenPayload);

    // 正确路径：整条消息经 GetSubObjectText 得到的子对象必须是合法 JSON
    const std::string msg = R"({"type":"saveSettings","settings":{"themeId":7}})";
    std::string payload;
    const bool got = qst::jsonutil::GetSubObjectText(msg, "settings", payload, "test");
    const bool payloadValid = got && qst::jsonutil::IsParseableObject(payload);

    const bool ok = brokenRejected && payloadValid;
    Emit(L"regression_trailing_outer_brace", ok,
        ok ? L"残破 payload 被拒绝；正确路径产出合法 JSON"
           : L"未能锁住本次事故形状");
}

// 7) 子对象文本可直接被各取值函数消费（等价 ApplySaveSettingsJson 的用法）
void CaseSubObjectApplyableFields() {
    const std::string msg =
        R"({"type":"saveSettings","settings":{"enableRandomInterval":true,)"
        R"("jitterX":3,"uiScaleFactor":1.25,"editorActionOrder":["wait","mouseClick"]}})";
    std::string payload;
    const bool got = qst::jsonutil::GetSubObjectText(msg, "settings", payload, "test");

    bool b = false;
    int i = 0;
    double d = 0;
    std::vector<std::wstring> order;
    const bool okB = got && qst::jsonutil::GetBool(payload, "enableRandomInterval", b) && b;
    const bool okI = got && qst::jsonutil::GetInt(payload, "jitterX", i) && i == 3;
    const bool okD = got && qst::jsonutil::GetNumber(payload, "uiScaleFactor", d)
        && d > 1.24 && d < 1.26;
    const bool okArr = got
        && qst::jsonutil::GetActionTokenArray(payload, "editorActionOrder", order)
        && order.size() == 2 && order[0] == L"wait" && order[1] == L"mouseClick";
    const bool ok = okB && okI && okD && okArr;
    Emit(L"subobject_applyable_fields", ok,
        ok ? L"4 种取值函数均命中" : L"子对象文本无法被取值函数消费");
}

// 8) 合法输入不产生诊断
void CaseDiagnosticsSilentOnValid() {
    ResetProbe();
    std::string payload;
    qst::jsonutil::GetSubObjectText(
        R"({"type":"saveSettings","settings":{"themeId":7}})", "settings", payload, "ok.case");
    const bool ok = g_probe.calls == 0 && qst::jsonutil::ParseFailureCount() == 0;
    Emit(L"diagnostics_silent_on_valid", ok,
        ok ? L"0 条诊断" : L"合法输入却上报了诊断（会产生日志噪音）");
}

// 9) 非法输入必须上报诊断，且带上下文标签
void CaseDiagnosticsReportsInvalid() {
    ResetProbe();
    std::string payload;
    // 事故形状：多带外层 '}'
    qst::jsonutil::GetSubObjectText(
        R"({"type":"saveSettings","settings":{"themeId":7}}})",
        "settings", payload, "saveSettings.payload");
    const bool reported = g_probe.calls == 1
        && g_probe.lastWhere == "saveSettings.payload"
        && !g_probe.lastText.empty()
        && qst::jsonutil::ParseFailureCount() == 1;
    // 前置校验路径（ApplySaveSettingsJson 入口用它）
    const bool precheck = !qst::jsonutil::IsParseableObject(R"({"a":1}})");
    const bool ok = reported && precheck;
    Emit(L"diagnostics_reports_invalid", ok,
        ok ? L"诊断 1 条，标签=saveSettings.payload"
           : L"非法输入未上报诊断（静默失败仍可能存在）");
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"BridgeJsonSelfTest — 桥接 JSON 边界契约（saveSettings 回归锁）\n"
        L"\n"
        L"用法:\n"
        L"  BridgeJsonSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/json_util.h（GetSubObjectText / 解析失败诊断）\n"
        L"  背景: docs/refactor-acceptance.md §3（P0）与 §7.3（测试规格）\n");
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
        selftest::PrintCaseList(L"BridgeJsonSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseSubObjectLastKey();
    CaseSubObjectNotLastKey();
    CaseSubObjectBraceAndNested();
    CaseSubObjectMissingOrNull();
    CaseSubObjectInvalidJson();
    CaseRegressionTrailingBrace();
    CaseSubObjectApplyableFields();
    CaseDiagnosticsSilentOnValid();
    CaseDiagnosticsReportsInvalid();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
