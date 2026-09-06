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
    {L"resolve_image_var_path", L"default",
        L"{image} expands from imageVars map to absolute path"},
    {L"build_quick_input_image_var", L"default",
        L"BuildQuickInputVarItems registers findImage followUp=3 image vars"},
    {L"build_quick_input_fixed_vars", L"default",
        L"BuildQuickInputVarItems always registers CurLoops/Random/Hour/Minute/Clipboard only"},
    {L"time_magic_vars", L"default",
        L"{Now}/{time:格式}/{date:格式} 展开为当前时间"},
    {L"common_magic_vars", L"default",
        L"{random}/{username}/{cursor.x}/{screen.w} 等常用魔法变量展开"},
    {L"resolve_ctrl_random", L"default",
        L"{ctrl:Random()} 每次引用取 1~100 整数（裸写/花括号/if 条件）"},
    {L"resolve_ctrl_hour_minute", L"default",
        L"ctrl:Hour()/ctrl:Minute() 为本地时 0–23 / 0–59（裸写/花括号/条件）"},
    {L"resolve_ctrl_clipboard", L"default",
        L"ctrl:Clipboard() 条件 0/1；文本展开路径；AI 模式见附图占位"},
    {L"user_var_beats_magic_name", L"default",
        L"条件裸名优先用户变量，避免 clipboard/cursor.x 盖住脚本变量"},
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

void CaseResolveImageVar() {
    std::unordered_map<std::wstring, std::wstring> imgs{
        {L"image", L"C:\\temp\\shot.bmp"}};
    MacroVariableContext ctx;
    ctx.imageVars = &imgs;
    const std::wstring out = ResolveMacroVariables(L"p={image}", ctx);
    Emit(L"resolve_image_var_path", out == L"p=C:\\temp\\shot.bmp", out.c_str());
}

void CaseBuildQuickInputImageVar() {
    std::vector<ScriptAction> acts(1);
    acts[0].type = ActionType::FindImage;
    acts[0].findImageFollowUp = 3;
    acts[0].matchVarName = L"image";
    const auto items = BuildQuickInputVarItems(acts);
    bool found = false;
    for (const auto& it : items) {
        if (it.display == L"image") { found = true; break; }
    }
    Emit(L"build_quick_input_image_var", found,
        found ? L"" : L"followUp=3 image var missing from quick-input list");
}

void CaseBuildQuickInputFixedVars() {
    const auto items = BuildQuickInputVarItems({});
    const wchar_t* need[] = {
        L"ctrl:CurLoops()", L"ctrl:Random()", L"ctrl:Hour()", L"ctrl:Minute()",
        L"ctrl:Clipboard()"
    };
    bool all = true;
    std::wstring missing;
    for (const wchar_t* name : need) {
        bool found = false;
        for (const auto& it : items) {
            if (it.display == name) { found = true; break; }
        }
        if (!found) {
            all = false;
            if (!missing.empty()) missing += L",";
            missing += name;
        }
    }
    const wchar_t* hidden[] = {
        L"Now", L"clipboard", L"random:1,100", L"cursor.x", L"cursor.y",
        L"screen.w", L"screen.h", L"username", L"time:yyyy-MM-dd HH:mm:ss",
        L"date:yyyy-MM-dd"
    };
    std::wstring leaked;
    for (const wchar_t* name : hidden) {
        for (const auto& it : items) {
            if (it.display == name) {
                all = false;
                if (!leaked.empty()) leaked += L",";
                leaked += name;
                break;
            }
        }
    }
    std::wstring detail;
    if (!missing.empty()) detail = L"missing " + missing;
    if (!leaked.empty()) {
        if (!detail.empty()) detail += L"; ";
        detail += L"dropdown leaked " + leaked;
    }
    Emit(L"build_quick_input_fixed_vars", all,
        all ? L"" : detail.c_str());
}

void CaseTimeMagicVars() {
    MacroVariableContext ctx;
    const std::wstring now = ResolveMacroVariables(L"{Now}", ctx);
    const std::wstring t = ResolveMacroVariables(L"T={time:yyyy/MM/dd HH:mm:ss}", ctx);
    const std::wstring d = ResolveMacroVariables(L"D={date:yyyy-MM-dd}", ctx);
    const bool ok = now.size() >= 16
        && now.find(L"-") != std::wstring::npos
        && now.find(L'{') == std::wstring::npos
        && t.find(L"T=20") == 0
        && t.find(L"/") != std::wstring::npos
        && t.find(L":") != std::wstring::npos
        && d.find(L"D=20") == 0
        && d.find(L"-") != std::wstring::npos;
    Emit(L"time_magic_vars", ok,
        ok ? L"" : (L"now=" + now + L" t=" + t + L" d=" + d).c_str());
}

void CaseCommonMagicVars() {
    MacroVariableContext ctx;
    const std::wstring rnd = ResolveMacroVariables(L"R={random:5,5}", ctx);
    const std::wstring usr = ResolveMacroVariables(L"U={username}", ctx);
    const std::wstring cx = ResolveMacroVariables(L"X={cursor.x}", ctx);
    const std::wstring sw = ResolveMacroVariables(L"W={screen.w}", ctx);
    const std::wstring okStr = ResolveMacroVariables(L"K={random:1,2}", ctx);
    const bool ok = rnd == L"R=5"
        && usr.find(L"U=") == 0 && usr.size() > 2
        && cx.find(L"X=") == 0 && cx.size() > 2
        && sw.find(L"W=") == 0 && sw.size() > 2
        && (okStr == L"K=1" || okStr == L"K=2");
    Emit(L"common_magic_vars", ok,
        ok ? L"" : (L"rnd=" + rnd + L" usr=" + usr
            + L" cx=" + cx + L" sw=" + sw).c_str());
}

bool ParseIntInRange(const std::wstring& s, int lo, int hi) {
    try {
        const int v = std::stoi(s);
        return v >= lo && v <= hi;
    } catch (...) {
        return false;
    }
}

void CaseResolveCtrlRandom() {
    MacroVariableContext ctx;
    // 花括号形式：{ctrl:Random()} → 1~100 整数
    const std::wstring out = ResolveMacroVariables(L"R={ctrl:Random()}", ctx);
    bool ok = out.size() > 2 && out.rfind(L"R=", 0) == 0
        && ParseIntInRange(out.substr(2), 1, 100);
    // 裸写形式：条件表达式里的 ctrl:Random()（无花括号）
    ok = ok && ParseIntInRange(ResolveMacroOperand(L"ctrl:Random()", ctx), 1, 100);
    // if 条件：随机值恒在 [1,100]，随机分支可用
    ok = ok && EvaluateConditionExpr(L"ctrl:Random() >= 1 and ctrl:Random() <= 100", ctx);
    Emit(L"resolve_ctrl_random", ok,
        ok ? L"" : (L"out=" + out).c_str());
}

void CaseResolveCtrlHourMinute() {
    MacroVariableContext ctx;
    const std::wstring hBrace = ResolveMacroVariables(L"{ctrl:Hour()}", ctx);
    const std::wstring mBrace = ResolveMacroVariables(L"{ctrl:Minute()}", ctx);
    const std::wstring hBare = ResolveMacroOperand(L"ctrl:Hour()", ctx);
    const std::wstring mBare = ResolveMacroOperand(L"ctrl:Minute()", ctx);
    bool ok = ParseIntInRange(hBrace, 0, 23) && ParseIntInRange(mBrace, 0, 59)
        && hBrace == hBare && mBrace == mBare
        && EvaluateConditionExpr(L"ctrl:Hour() >= 0 and ctrl:Hour() <= 23", ctx)
        && EvaluateConditionExpr(L"ctrl:Minute() >= 0 and ctrl:Minute() <= 59", ctx);
    Emit(L"resolve_ctrl_hour_minute", ok,
        ok ? L"" : (L"H=" + hBrace + L" M=" + mBrace).c_str());
}

void CaseResolveCtrlClipboard() {
    MacroClipboardSnapshot snap;
    MacroVariableContext ctx;
    ctx.clipboardSnapshot = &snap;

    bool ok = LooksLikeImageFilePath(L"C:\\a.PNG")
        && LooksLikeImageFilePath(L"d:\\x.jpg")
        && !LooksLikeImageFilePath(L"C:\\a.txt")
        && PromptMentionsCtrlClipboard(L"看 {ctrl:Clipboard()}")
        && !PromptMentionsCtrlClipboard(L"{clipboard}");

    snap.text.clear();
    snap.files.clear();
    snap.hasBitmap = false;
    ok = ok && ResolveMacroOperand(L"ctrl:Clipboard()", ctx) == L"0";
    ok = ok && EvaluateConditionExpr(L"ctrl:Clipboard() == 0", ctx);
    ok = ok && ResolveMacroVariables(L"T={ctrl:Clipboard()}", ctx) == L"T=";

    snap.text = L"hello";
    ok = ok && ResolveMacroOperand(L"ctrl:Clipboard()", ctx) == L"1";
    ok = ok && EvaluateConditionExpr(L"ctrl:Clipboard() == 1", ctx);
    ok = ok && ResolveMacroVariables(L"T={ctrl:Clipboard()}", ctx) == L"T=hello";
    ok = ok && ResolveMacroVariables(L"P={clipboard}", ctx) == L"P=hello";

    snap.text.clear();
    snap.files = {L"C:\\a.png", L"C:\\b.txt"};
    ok = ok && ResolveMacroOperand(L"ctrl:Clipboard()", ctx) == L"1";
    ok = ok && ResolveMacroVariables(L"{ctrl:Clipboard()}", ctx) == L"C:\\a.png\nC:\\b.txt";

    ctx.clipboardExpandMode = ClipboardExpandMode::Ai;
    const std::wstring aiFiles = ResolveMacroVariables(L"{ctrl:Clipboard()}", ctx);
    ok = ok && aiFiles.find(L"C:\\b.txt") != std::wstring::npos
        && aiFiles.find(L"见附图") != std::wstring::npos
        && aiFiles.find(L"C:\\a.png") == std::wstring::npos;

    snap.files.clear();
    snap.hasBitmap = true;
    snap.text = L"title";
    const std::wstring aiMix = ResolveMacroVariables(L"{ctrl:Clipboard()}", ctx);
    ok = ok && aiMix.find(L"title") != std::wstring::npos
        && aiMix.find(L"见附图") != std::wstring::npos;

    snap.text.clear();
    snap.hasBitmap = true;
    ok = ok && ResolveMacroVariables(L"X={ctrl:Clipboard()}", ctx)
        == L"X=（见附图：剪贴板图片）";
    ctx.clipboardExpandMode = ClipboardExpandMode::Text;
    ok = ok && ResolveMacroVariables(L"X={ctrl:Clipboard()}", ctx) == L"X=";

    Emit(L"resolve_ctrl_clipboard", ok,
        ok ? L"" : (L"aiFiles=" + aiFiles + L" aiMix=" + aiMix).c_str());
}

void CaseUserVarBeatsMagicName() {
    std::unordered_map<std::wstring, int> loops{{L"clipboard", 7}, {L"username", 3}};
    MacroVariableContext ctx;
    ctx.loopVars = &loops;
    ImageMatchResult m{};
    m.found = true;
    m.topLeftX = 42;
    m.topLeftY = 1;
    m.bottomRightX = 50;
    m.bottomRightY = 9;
    std::unordered_map<std::wstring, ImageMatchResult> matches{{L"cursor", m}};
    ctx.matchVars = &matches;
    const std::wstring cx = ResolveMacroOperand(L"cursor.x", ctx);
    const bool ok = EvaluateConditionExpr(L"clipboard == 7", ctx)
        && EvaluateConditionExpr(L"username == 3", ctx)
        && cx == L"42"
        && ResolveMacroVariables(L"{ctrl:Hour()}", ctx).size() <= 2;
    Emit(L"user_var_beats_magic_name", ok,
        ok ? L"" : (L"cursor.x=" + cx).c_str());
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
    CaseResolveImageVar();
    CaseBuildQuickInputImageVar();
    CaseBuildQuickInputFixedVars();
    CaseTimeMagicVars();
    CaseCommonMagicVars();
    CaseResolveCtrlRandom();
    CaseResolveCtrlHourMinute();
    CaseResolveCtrlClipboard();
    CaseUserVarBeatsMagicName();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
