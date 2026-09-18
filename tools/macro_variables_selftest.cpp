// =============================================================================
// MacroVariablesSelfTest — 宏变量 / 条件 / 转义自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:MacroVariablesSelfTest
//   build\Release\MacroVariablesSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "macro_variables.h"
#include "var_compute.h"

#include <string>
#include <unordered_map>

namespace {

using selftest::Emit;

std::wstring Exported(const VarComputeResult& r, const wchar_t* name) {
    const auto it = r.exported.find(name);
    return it == r.exported.end() ? L"" : it->second;
}

const selftest::CaseInfo kCases[] = {
    {L"resolve_match_var_brace", L"default",
        L"{matchRet.x}/{matchRet.cx} expand from ImageMatchResult"},
    {L"resolve_cur_loops", L"default",
        L"{ctrl:CurLoops()} expands ctx.curLoops"},
    {L"decode_quick_input_escapes", L"default",
        L"\\\\n \\\\t \\\\\\\\ decode for quick input"},
    {L"resolve_quick_input_var_escapes", L"default",
        L"parseEscapes=0 strips newlines/tabs from {var}; 1 decodes \\\\n in var values"},
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
    {L"var_compute_return_exports", L"default",
        L"变量运算 return 导出；未 return 的局部不保留"},
    {L"var_compute_clipboard_string", L"default",
        L"变量运算中 ctrl:Clipboard() 为文本或文件路径，不是 0/1"},
    {L"var_compute_user_var_roundtrip", L"default",
        L"导出变量可被后续运算和条件读取"},
    {L"var_compute_optional_semi_newline", L"default",
        L"变量运算换行可省略分号，return 导出可供条件读取"},
    {L"var_compute_string_compare_sign", L"default",
        L"OCR 符号用 '+' / \"+\" 比较；裸写 + 是加法运算符"},
    {L"var_compute_split_string", L"default",
        L"split(s, sep) / [i] / count / toInt 按分隔符拆分字符串"},
    {L"collect_varcompute_return_names", L"default",
        L"CollectVarComputeReturnNames / BuildQuickInputVarItems 收集 return 导出名"},
    {L"resolve_match_list_index", L"default",
        L"{matchRet[0].x}/{matchRet.count}/{matchRet[n]} 多图匹配数组下标"},
    {L"build_quick_input_multimatch", L"default",
        L"BuildQuickInputVarItems 注册 matchRet[n] / [0].x / count"},
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

void CaseResolveQuickInputVarEscapes() {
    std::wstring withNl = L"x";
    withNl.push_back(L'\n');
    withNl += L"y";
    std::unordered_map<std::wstring, std::wstring> userNl{{L"a", withNl}};
    MacroVariableContext ctx;
    ctx.userVars = &userNl;

    const std::wstring offNl = ResolveQuickInputText(L"{a}", ctx, false);
    const bool offNlOk = offNl == L"xy";

    const std::wstring onNl = ResolveQuickInputText(L"{a}", ctx, true);
    const bool onNlOk = onNl.size() == 3
        && onNl[0] == L'x' && onNl[1] == L'\n' && onNl[2] == L'y';

    std::unordered_map<std::wstring, std::wstring> userEsc{{L"b", L"p\\nq"}};
    ctx.userVars = &userEsc;
    const std::wstring offEsc = ResolveQuickInputText(L"{b}", ctx, false);
    const bool offEscOk = offEsc == L"p\\nq";
    const std::wstring onEsc = ResolveQuickInputText(L"{b}", ctx, true);
    const bool onEscOk = onEsc.size() == 3
        && onEsc[0] == L'p' && onEsc[1] == L'\n' && onEsc[2] == L'q';

    std::wstring justNl(1, L'\n');
    std::unordered_map<std::wstring, std::wstring> userJust{{L"c", justNl}};
    ctx.userVars = &userJust;
    const std::wstring mixOff = ResolveQuickInputText(L"A\\n{c}", ctx, false);
    const bool mixOk = mixOff == L"A\\n";

    std::wstring tmplNl = L"A";
    tmplNl.push_back(L'\n');
    tmplNl += L"{c}";
    const std::wstring litOff = ResolveQuickInputText(tmplNl, ctx, false);
    const bool litOk = litOff.size() == 2
        && litOff[0] == L'A' && litOff[1] == L'\n';

    OcrVarResult ocr{};
    ocr.mode = OcrVarMode::Text;
    ocr.text = L"1";
    ocr.text.push_back(L'\t');
    ocr.text += L"2";
    std::unordered_map<std::wstring, OcrVarResult> ocrVars{{L"ocr", ocr}};
    MacroVariableContext ocrCtx;
    ocrCtx.ocrVars = &ocrVars;
    const std::wstring ocrOff = ResolveQuickInputText(L"{ocr}", ocrCtx, false);
    const bool ocrOk = ocrOff == L"12";

    const bool ok = offNlOk && onNlOk && offEscOk && onEscOk && mixOk && litOk && ocrOk;
    Emit(L"resolve_quick_input_var_escapes", ok,
        ok ? L"" : L"parseEscapes must apply to expanded variable values");
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

void CaseVarComputeReturnExports() {
    MacroVariableContext ctx;
    const auto r1 = RunVarCompute(L"int n = 4;\ncombo = n + 2;\nreturn combo;", ctx);
    const bool ok1 = r1.ok && Exported(r1, L"combo") == L"6" && r1.exported.find(L"n") == r1.exported.end();
    const auto r2 = RunVarCompute(L"int n = 9;\ncombo = n;", ctx);
    const bool ok2 = r2.ok && r2.exported.empty();
    const auto r3 = RunVarCompute(
        L"int s = 0;\nfor (int i = 0; i < 4; ++i) s += i;\nreturn s;", ctx);
    const bool ok3 = r3.ok && Exported(r3, L"s") == L"6";
    std::wstring err;
    const bool parsed = ParseVarCompute(L"if (1) { combo = 1; } return combo;", err);
    Emit(L"var_compute_return_exports", ok1 && ok2 && ok3 && parsed,
        (ok1 && ok2 && ok3 && parsed) ? L"" : (r1.error + L" | " + r2.error + L" | "
            + r3.error + L" s=" + Exported(r3, L"s") + L" | " + err).c_str());
}

void CaseVarComputeClipboardString() {
    MacroClipboardSnapshot snap;
    snap.text = L"hello clip";
    MacroVariableContext ctx;
    ctx.clipboardSnapshot = &snap;
    const auto rText = RunVarCompute(
        L"string s = ctrl:Clipboard();\nreturn s;", ctx);
    snap.text.clear();
    snap.files = { L"C:\\tmp\\a.txt", L"D:\\b.png" };
    const auto rFiles = RunVarCompute(
        L"string s = ctrl:Clipboard();\nreturn s;", ctx);
    const bool ok = rText.ok && Exported(rText, L"s") == L"hello clip"
        && rFiles.ok && Exported(rFiles, L"s") == L"C:\\tmp\\a.txt\nD:\\b.png";
    Emit(L"var_compute_clipboard_string", ok,
        ok ? L"" : (rText.error + L" | " + rFiles.error + L" t=" + Exported(rText, L"s")
            + L" f=" + Exported(rFiles, L"s")).c_str());
}

void CaseVarComputeUserVarRoundtrip() {
    std::unordered_map<std::wstring, std::wstring> user{ {L"combo", L"10"} };
    MacroVariableContext ctx;
    ctx.userVars = &user;
    const auto r = RunVarCompute(L"combo = combo + 1;\nreturn combo;", ctx);
    const bool exported = r.ok && Exported(r, L"combo") == L"11";
    if (exported) user[L"combo"] = Exported(r, L"combo");
    const bool cond = EvaluateConditionExpr(L"combo == 11", ctx);
    Emit(L"var_compute_user_var_roundtrip", exported && cond,
        (exported && cond) ? L"" : (r.error + L" v=" + Exported(r, L"combo")).c_str());
}

void CaseVarComputeOptionalSemiNewline() {
    MacroVariableContext ctx;
    const auto rNl = RunVarCompute(L"int a = 2\nreturn a", ctx);
    const bool okNl = rNl.ok && Exported(rNl, L"a") == L"2";
    const auto rFlat = RunVarCompute(L"int a = 2               return a", ctx);
    const bool okFlat = rFlat.ok && Exported(rFlat, L"a") == L"2";
    std::unordered_map<std::wstring, std::wstring> user;
    if (okNl) user[L"a"] = Exported(rNl, L"a");
    ctx.userVars = &user;
    const bool cond = EvaluateConditionExpr(L"a == 2", ctx);
    const auto rNeed = RunVarCompute(L"int a = 2 a = 3\nreturn a", ctx);
    const bool stillNeedSemi = !rNeed.ok;
    Emit(L"var_compute_optional_semi_newline", okNl && okFlat && cond && stillNeedSemi,
        (okNl && okFlat && cond && stillNeedSemi) ? L""
            : (rNl.error + L" | " + rFlat.error + L" | " + rNeed.error
                + L" n=" + Exported(rNl, L"a") + L" f=" + Exported(rFlat, L"a")).c_str());
}

void CaseVarComputeStringCompareSign() {
    std::unordered_map<std::wstring, OcrVarResult> ocr{
        {L"a", OcrVarResult{OcrVarMode::Text, L"1"}},
        {L"sign", OcrVarResult{OcrVarMode::Text, L"+"}},
        {L"b", OcrVarResult{OcrVarMode::Text, L"5"}},
    };
    MacroVariableContext ctx;
    ctx.ocrVars = &ocr;
    auto runWith = [&](const wchar_t* lit) {
        std::wstring src = L"int res = 0\nif (sign == ";
        src += lit;
        src += L") { res = a + b }\nelse { res = a - b }\nreturn res\n";
        return RunVarCompute(src, ctx);
    };
    const auto rSq = runWith(L"'+'");
    const auto rDq = runWith(L"\"+\"");
    const auto rFancy = runWith(L"\u2018+\u2019");
    std::wstring parseErr;
    const bool bareFails = !ParseVarCompute(
        L"int res = 0\nif (sign == +) { res = a + b }\nreturn res", parseErr)
        && parseErr.find(L"加法") != std::wstring::npos;
    const bool ok = rSq.ok && Exported(rSq, L"res") == L"6"
        && rDq.ok && Exported(rDq, L"res") == L"6"
        && rFancy.ok && Exported(rFancy, L"res") == L"6"
        && bareFails;
    std::wstring detail = rSq.error + L" | " + rDq.error + L" | " + rFancy.error
        + L" sq=" + Exported(rSq, L"res")
        + L" dq=" + Exported(rDq, L"res")
        + L" fancy=" + Exported(rFancy, L"res")
        + L" bare=" + parseErr;
    Emit(L"var_compute_string_compare_sign", ok, ok ? L"" : detail.c_str());
}

void CaseVarComputeSplitString() {
    MacroVariableContext ctx;
    const auto r = RunVarCompute(
        L"p = split('24150/24159', '/')\n"
        L"a = p[0]\n"
        L"b = p[1]\n"
        L"n = p.count\n"
        L"last = p[-1]\n"
        L"sum = toInt(a) + toInt(b)\n"
        L"return a, b, n, last, sum\n", ctx);
    const bool mainOk = r.ok
        && Exported(r, L"a") == L"24150"
        && Exported(r, L"b") == L"24159"
        && Exported(r, L"n") == L"2"
        && Exported(r, L"last") == L"24159"
        && Exported(r, L"sum") == L"48309";

    const auto rChain = RunVarCompute(
        L"a = split('24150/24159', '/')[0]\n"
        L"n = split('24150/24159', '/').count\n"
        L"return a, n\n", ctx);
    const bool chainOk = rChain.ok
        && Exported(rChain, L"a") == L"24150"
        && Exported(rChain, L"n") == L"2";

    const auto rMax = RunVarCompute(
        L"p = split('a/b/c', '/', 2)\n"
        L"x = p[0]\n"
        L"y = p[1]\n"
        L"n = p.length\n"
        L"return x, y, n\n", ctx);
    const bool maxOk = rMax.ok
        && Exported(rMax, L"x") == L"a"
        && Exported(rMax, L"y") == L"b/c"
        && Exported(rMax, L"n") == L"2";

    const auto rChar = RunVarCompute(
        L"p = split('ab', '')\n"
        L"a = p[0]\n"
        L"b = p[1]\n"
        L"c = 'xyz'[1]\n"
        L"return a, b, c\n", ctx);
    const bool charOk = rChar.ok
        && Exported(rChar, L"a") == L"a"
        && Exported(rChar, L"b") == L"b"
        && Exported(rChar, L"c") == L"y";

    const auto rTrim = RunVarCompute(
        L"t = trim('  12 ')\n"
        L"n = toInt(t)\n"
        L"s = toString(n + 1)\n"
        L"return t, n, s\n", ctx);
    const bool convOk = rTrim.ok
        && Exported(rTrim, L"t") == L"12"
        && Exported(rTrim, L"n") == L"12"
        && Exported(rTrim, L"s") == L"13";

    std::unordered_map<std::wstring, OcrVarResult> ocr{
        {L"s", OcrVarResult{OcrVarMode::Text, L"24150/24159"}},
    };
    ctx.ocrVars = &ocr;
    const auto rOcr = RunVarCompute(
        L"p = split(s, '/')\nleft = p[0]\nright = p[1]\nreturn left, right\n", ctx);
    const bool ocrOk = rOcr.ok
        && Exported(rOcr, L"left") == L"24150"
        && Exported(rOcr, L"right") == L"24159";

    ctx.ocrVars = nullptr;
    const auto rOob = RunVarCompute(L"x = split('a', '/')[1]\nreturn x\n", ctx);
    const bool oob = !rOob.ok && rOob.error.find(L"下标") != std::wstring::npos;
    const auto rUnk = RunVarCompute(L"x = foo(1)\nreturn x\n", ctx);
    const bool unk = !rUnk.ok && rUnk.error.find(L"未知函数") != std::wstring::npos;

    const bool ok = mainOk && chainOk && maxOk && charOk && convOk && ocrOk && oob && unk;
    std::wstring detail = r.error
        + L" a=" + Exported(r, L"a")
        + L" sum=" + Exported(r, L"sum")
        + L" chain=" + rChain.error
        + L" max=" + rMax.error + L" y=" + Exported(rMax, L"y")
        + L" char=" + rChar.error
        + L" trim=" + rTrim.error
        + L" ocr=" + rOcr.error
        + L" oob=" + rOob.error
        + L" unk=" + rUnk.error;
    Emit(L"var_compute_split_string", ok, ok ? L"" : detail.c_str());
}

void CaseCollectVarComputeReturnNames() {
    const auto fromAst = CollectVarComputeReturnNames(L"int a = 1;\nint b = 2;\nreturn a, b;");
    const bool astOk = fromAst.size() == 2 && fromAst[0] == L"a" && fromAst[1] == L"b";
    const auto fromFallback = CollectVarComputeReturnNames(L"@@@ return hp, combo");
    bool fallbackOk = false;
    if (fromFallback.size() >= 2) {
        fallbackOk = fromFallback[0] == L"hp" && fromFallback[1] == L"combo";
    }
    std::vector<ScriptAction> acts(1);
    acts[0].type = ActionType::VarCompute;
    acts[0].computeCode = L"int combo = 1\nreturn combo";
    const auto items = BuildQuickInputVarItems(acts);
    bool listed = false;
    for (const auto& it : items) {
        if (it.display == L"combo") { listed = true; break; }
    }
    const bool ok = astOk && fallbackOk && listed;
    Emit(L"collect_varcompute_return_names", ok,
        ok ? L"" : (L"ast=" + std::to_wstring(fromAst.size())
            + L" fb=" + std::to_wstring(fromFallback.size())
            + L" listed=" + (listed ? L"1" : L"0")).c_str());
}

void CaseResolveMatchListIndex() {
    ImageMatchResult m0{};
    m0.found = true;
    m0.score = 90.0;
    m0.topLeftX = 10;
    m0.topLeftY = 20;
    m0.bottomRightX = 30;
    m0.bottomRightY = 40;
    ImageMatchResult m1{};
    m1.found = true;
    m1.score = 80.0;
    m1.topLeftX = 100;
    m1.topLeftY = 200;
    m1.bottomRightX = 130;
    m1.bottomRightY = 240;
    ImageMatchListVar list;
    ImageMatchListHit h0;
    h0.match = m0;
    h0.templateIndex = 0;
    h0.templateName = L"a.png";
    ImageMatchListHit h1;
    h1.match = m1;
    h1.templateIndex = 1;
    h1.templateName = L"b.png";
    list.hits.push_back(h0);
    list.hits.push_back(h1);
    std::unordered_map<std::wstring, ImageMatchListVar> lists{{L"matchRet", list}};
    MacroVariableContext ctx;
    ctx.matchListVars = &lists;
    const std::wstring x0 = ResolveMacroVariables(L"{matchRet[0].x}", ctx);
    const std::wstring x1 = ResolveMacroVariables(L"{matchRet[1].x}", ctx);
    const std::wstring count = ResolveMacroVariables(L"{matchRet.count}", ctx);
    const std::wstring miss = ResolveMacroVariables(L"{matchRet[9].x}", ctx);
    const std::wstring ph = ResolveMacroVariables(L"{matchRet[n]}", ctx);
    const std::wstring hit0 = ResolveMacroVariables(L"{matchRet[0]}", ctx);
    const std::wstring name0 = ResolveMacroVariables(L"{matchRet[0].hitName}", ctx);
    const bool ok = x0 == L"10" && x1 == L"100" && count == L"2"
        && miss == L"0" && ph.empty() && hit0 == L"1" && name0 == L"a.png";
    std::wstring detail = L"x0=" + x0 + L" x1=" + x1 + L" count=" + count
        + L" miss=" + miss + L" ph=[" + ph + L"] hit0=" + hit0 + L" name=" + name0;
    Emit(L"resolve_match_list_index", ok, ok ? L"" : detail.c_str());
}

void CaseBuildQuickInputMultiMatch() {
    std::vector<ScriptAction> acts(1);
    acts[0].type = ActionType::MultiMatch;
    acts[0].matchVarName = L"matchRet";
    const auto items = BuildQuickInputVarItems(acts);
    bool hasN = false, has0x = false, hasCount = false;
    for (const auto& it : items) {
        if (it.display == L"matchRet[n]") hasN = true;
        if (it.display == L"matchRet[0].x") has0x = true;
        if (it.display == L"matchRet.count") hasCount = true;
    }
    const bool ok = hasN && has0x && hasCount;
    Emit(L"build_quick_input_multimatch", ok,
        ok ? L"" : L"missing matchRet[n] / [0].x / count in quick-input list");
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
    CaseResolveQuickInputVarEscapes();
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
    CaseVarComputeReturnExports();
    CaseVarComputeClipboardString();
    CaseVarComputeUserVarRoundtrip();
    CaseVarComputeOptionalSemiNewline();
    CaseVarComputeStringCompareSign();
    CaseVarComputeSplitString();
    CaseCollectVarComputeReturnNames();
    CaseResolveMatchListIndex();
    CaseBuildQuickInputMultiMatch();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
