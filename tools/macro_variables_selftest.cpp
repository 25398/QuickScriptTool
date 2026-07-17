// =============================================================================
// MacroVariablesSelfTest — 宏变量 / 条件 / 转义自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:MacroVariablesSelfTest
//   build\Release\MacroVariablesSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "macro_variables.h"

#include <string>
#include <unordered_map>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"resolve_match_var_brace", L"default",
        L"{matchRet.x}/{matchRet.cx} expand from ImageMatchResult"},
    {L"resolve_cur_loops", L"default",
        L"{ctrl:CurLoops()} expands ctx.curLoops"},
    {L"decode_quick_input_escapes", L"default",
        L"\\\\n \\\\t \\\\\\\\ decode for quick input"},
    {L"find_image_time_sec", L"default",
        L"ResolveFindImageTimeSec: -1 / 0 / positive / non-numeric->0"},
    {L"condition_compare_and_or", L"default",
        L"EvaluateConditionExpr == / > with newline and/or"},
    {L"goto_step_from_literal", L"default",
        L"TryResolveGotoStepNo accepts positive int"},
    {L"loop_max_from_var", L"default",
        L"ResolveLoopMaxCount reads loopVarExpr from loopVars"},
    {L"unknown_var_no_recurse", L"default",
        L"Unknown {var} resolves to empty without stack overflow"},
};

void CaseResolveMatchVar() {
    ImageMatchResult m{};
    m.found = true;
    m.score = 88.0;
    m.topLeftX = 10;
    m.topLeftY = 20;
    m.bottomRightX = 30;
    m.bottomRightY = 40;
    std::unordered_map<std::wstring, ImageMatchResult> vars{{L"matchRet", m}};
    MacroVariableContext ctx;
    ctx.matchVars = &vars;
    const std::wstring out = ResolveMacroVariables(L"at {matchRet.x},{matchRet.cy} score={matchRet.matchData}", ctx);
    Emit(L"resolve_match_var_brace", out == L"at 10,30 score=88", out.c_str());
}

void CaseResolveCurLoops() {
    MacroVariableContext ctx;
    ctx.curLoops = 7;
    const std::wstring out = ResolveMacroVariables(L"n={ctrl:CurLoops()}", ctx);
    Emit(L"resolve_cur_loops", out == L"n=7", out.c_str());
}

void CaseDecodeEscapes() {
    const std::wstring out = DecodeQuickInputEscapes(L"a\\nb\\tc\\\\d");
    const bool ok = out.size() >= 6
        && out[0] == L'a' && out[1] == L'\n' && out[2] == L'b'
        && out[3] == L'\t' && out[4] == L'c' && out[5] == L'\\' && out[6] == L'd';
    Emit(L"decode_quick_input_escapes", ok, out.c_str());
}

void CaseFindImageTimeSec() {
    MacroVariableContext ctx;
    const bool ok = ResolveFindImageTimeSec(L"-1", ctx) == -1.0
        && ResolveFindImageTimeSec(L"0", ctx) == 0.0
        && ResolveFindImageTimeSec(L"2.5", ctx) == 2.5
        && ResolveFindImageTimeSec(L"notANumber", ctx) == 0.0;
    Emit(L"find_image_time_sec", ok,
        ok ? L"" : L"ResolveFindImageTimeSec numeric / fallback rules broken");
}

void CaseConditionAndOr() {
    std::unordered_map<std::wstring, int> loops{{L"i", 3}};
    MacroVariableContext ctx;
    ctx.loopVars = &loops;
    // 多行：and/or 写在行尾
    const bool ok = EvaluateConditionExpr(L"i == 3 and\ni > 1", ctx)
        && !EvaluateConditionExpr(L"i == 2", ctx)
        && EvaluateConditionExpr(L"i == 2 or\ni == 3", ctx);
    Emit(L"condition_compare_and_or", ok,
        ok ? L"" : L"EvaluateConditionExpr and/or / compare failed");
}

void CaseGotoStep() {
    MacroVariableContext ctx;
    int step = 0;
    const bool ok = TryResolveGotoStepNo(L"12", ctx, step) && step == 12
        && !TryResolveGotoStepNo(L"0", ctx, step)
        && !TryResolveGotoStepNo(L"", ctx, step);
    Emit(L"goto_step_from_literal", ok,
        ok ? L"" : L"TryResolveGotoStepNo positive-int rules broken");
}

void CaseLoopMaxFromVar() {
    std::unordered_map<std::wstring, int> loops{{L"n", 5}};
    MacroVariableContext ctx;
    ctx.loopVars = &loops;
    ScriptAction a{};
    a.type = ActionType::Loop;
    a.loopCount = 99;
    a.loopFromVar = true;
    a.loopVarExpr = L"n";
    const int max = ResolveLoopMaxCount(a, ctx);
    Emit(L"loop_max_from_var", max == 5,
        max == 5 ? L"" : L"ResolveLoopMaxCount should read loopVars via loopVarExpr");
}

void CaseUnknownVarNoRecurse() {
    MacroVariableContext ctx;
    // 旧实现会对未知标识符 {t} 递归包装导致栈溢出
    const std::wstring out = ResolveMacroVariables(L"x={noSuchVar.y}y", ctx);
    Emit(L"unknown_var_no_recurse", out == L"x=y", out.c_str());
}

void PrintHelp() {
    std::fwprintf(stderr,
        L"MacroVariablesSelfTest — 宏变量自检\n"
        L"\n"
        L"用法:\n"
        L"  MacroVariablesSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/macro_variables.cpp\n");
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
        }
        else if (a == L"--help" || a == L"-h") {
            PrintHelp();
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"MacroVariablesSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseResolveMatchVar();
    CaseResolveCurLoops();
    CaseDecodeEscapes();
    CaseFindImageTimeSec();
    CaseConditionAndOr();
    CaseGotoStep();
    CaseLoopMaxFromVar();
    CaseUnknownVarNoRecurse();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
