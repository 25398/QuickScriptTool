// =============================================================================
// ScriptActionBuilderSelfTest — Agent 脚本动作构建自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ScriptActionBuilderSelfTest
//   build\Release\ScriptActionBuilderSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "action_tree.h"
#include "action_assemble.h"
#include "action_disassemble.h"
#include "action_utils.h"
#include "script_action_builder.h"
#include "utils.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace {

using selftest::Emit;
using json = nlohmann::json;

const selftest::CaseInfo kCases[] = {
    {L"build_wait_ok", L"default",
        L"BuildScriptActionFromJson type=wait with duration"},
    {L"reject_custom_text", L"default",
        L"customText type must be rejected"},
    {L"normalize_renumber", L"default",
        L"NormalizeScriptActionList sets originalNo 1..n and clears customText"},
    {L"ensure_stop_macro_appends", L"default",
        L"EnsureStopMacroOnActions appends stopMacro when missing"},
    {L"ensure_stop_macro_skip_infinite", L"default",
        L"Top-level infinite loop skips auto stopMacro"},
    {L"endloop_needs_parent", L"default",
        L"ValidateEndLoopPlacements rejects orphan endLoop"},
    {L"endloop_inside_loop_ok", L"default",
        L"endLoop at loop.indent+1 is valid"},
    {L"build_array_json", L"default",
        L"BuildScriptActionsJsonArray builds wait+stopMacro JSON"},
    {L"flatten_children_loop_body", L"default",
        L"loop.children 展开后循环体 indent=1，且忽略子项 indent=0"},
    {L"empty_loop_rejected", L"default",
        L"循环后同级动作视为空循环，构建失败"},
    {L"flatten_if_else_inside_loop", L"default",
        L"loop 内 if/else 为兄弟，分支再嵌套一层"},
    {L"plan_outline_shows_tree", L"default",
        L"planScriptActions 输出缩进树且不要求必填细节"},
    {L"non_container_children_rejected", L"default",
        L"wait 等非容器不能带 children"},
    {L"fragment_loop_skips_tree_check", L"default",
        L"逐条构建（AI 执行合并）不因单独的 loop 头判空容器失败"},
    {L"indent_siblings_form_loop_body", L"default",
        L"loop 后 indent+1 的同级项视为循环体，整表校验通过"},
    {L"build_move_mouse_relative", L"default",
        L"BuildScriptActionFromJson type=moveMouseRelative with dx/dy"},
    {L"inter_repeat_interval_semantics", L"default",
        L"ShouldWaitAfterRepeat: gap only between repeats; count=1 never waits"},
    {L"build_findimage_save_image", L"default",
        L"followUp saveImage → findImageFollowUp=3；有模板保留 findTimeExpr；无模板清 0"},
    {L"build_multimatch_ok", L"default",
        L"multiMatch 缺图失败；imagePaths+mode+duration=0.05；saveImage 钳 2；保留 findTimeExpr"},
    {L"build_multimatch_image_use_var", L"default",
        L"multiMatch imageUseVar / imageUseVars 不被强制关掉"},
    {L"build_multimatch_hole_use_vars", L"default",
        L"imagePaths 中间空槽不得把后面的 imageUseVars 错位"},
    {L"lookup_multimatch", L"default",
        L"lookupMacroAction type=multiMatch 含 imagePaths / matchRet[0]"},
    {L"build_quickinput_parse_escapes", L"default",
        L"quickInput parseEscapes default 0; 1 enables decode"},
    {L"resolve_key_nav_names", L"default",
        L"Home/Up/Right/Delete/enter resolve to real VKs (not first letter)"},
    {L"reject_unknown_keytext", L"default",
        L"Unknown multi-char keyText is rejected (no first-letter fallback)"},
    {L"build_runblock_repeat_fields", L"default",
        L"runBlock accepts clickCount/duration/randomDuration"},
    {L"build_mouseplayback_speed", L"default",
        L"mousePlayback playbackSpeed default 1; clamp 0.25~4"},
    {L"build_nested_use_mode", L"default",
        L"runMacro/mousePlayback useMode inherit default; window+nestedWindowMode"},
    {L"disassemble_unwrap_loop", L"default",
        L"拆解循环：去掉循环壳，循环体一次放出并继承缩进"},
    {L"disassemble_run_block_keeps_define", L"default",
        L"拆解运行块：内联块体一层，定义块仍在"},
    {L"disassemble_nested_indent", L"default",
        L"拆解子节点运行块后，内联动作仍挂在原父循环下"},
    {L"disassemble_empty_rejected", L"default",
        L"空循环/缺目标拆解失败且原列表不变"},
    {L"disassemble_run_macro_loader", L"default",
        L"拆解运行宏：loader 拷一层动作，不改源；次数丢弃"},
    {L"merge_contiguous_loop", L"default",
        L"连续同级并入循环：原地替换，子节点 indent+1"},
    {L"merge_ignore_inner_checks", L"default",
        L"勾选容器及其内部时只把整棵子树当一个子节点"},
    {L"merge_discrete_placements", L"default",
        L"离散并入：before/after/first/last 位置与缩进"},
    {L"merge_else_requires_if", L"default",
        L"并入否则须紧跟同级如果，失败则列表不变"},
    {L"merge_define_block_to_top", L"default",
        L"并入定义块强制置顶 indent=0"},
    {L"merge_mixed_parent_rejected", L"default",
        L"不同父节点的勾选不能并入"},
    {L"merge_inplace_discrete_rejected", L"default",
        L"不连续时 InPlace 失败且列表不变"},
    {L"merge_nested_inside_loop", L"default",
        L"循环内同级子动作并入后仍挂在原循环下"},
    {L"merge_first_differs_from_before", L"default",
        L"离散时「最前」与「选择项前」在首个勾选非列表开头时不同"},
    {L"flatten_watchimage_children", L"default",
        L"watchImage children 展开为 indent+1 子动作"},
    {L"flatten_watchimage_nested_rejected", L"default",
        L"watchImage 写在 loop children 里必须失败"},
    {L"lookup_watchimage_resume", L"default",
        L"lookupMacroAction type=watchImage 含 resumeAfterWatch 续行"},
    {L"build_watchimage_time_mode", L"default",
        L"BuildScriptActionFromJson watchImage watchMode=time + watchPollSeconds"},
    {L"build_varcompute_code", L"default",
        L"BuildScriptActionFromJson type=varCompute 读 computeCode"},
    {L"build_mousedrag_abs", L"default",
        L"BuildScriptActionFromJson mouseDrag 绝对坐标 duration 为拖拽时长"},
    {L"build_mousedrag_image_locate_requires_path", L"default",
        L"mouseDrag imageLocate=1 缺 imagePath 失败"},
    {L"lookup_mousedrag", L"default",
        L"lookupMacroAction type=mouseDrag 含 imageLocate/endX/duration 拖拽时长"},
    {L"build_color_image_locate_requires_path", L"default",
        L"getColor/colorMatch/findColor imageLocate=1 缺 imagePath 失败"},
    {L"build_findcolor_followup_clamped", L"default",
        L"findColor followUp=saveImage 钳到 2（保存到变量）"},
    {L"lookup_getcolor", L"default",
        L"lookupMacroAction type=getColor 含 imageLocate"},
};

void CaseBuildWait() {
    json p = {{"type", "wait"}, {"duration", 0.5}};
    auto r = BuildScriptActionFromJson(p);
    const bool ok = r.ok && r.action.type == ActionType::Wait && r.action.duration == 0.5;
    Emit(L"build_wait_ok", ok, r.ok ? L"" : r.error.c_str());
}

void CaseRejectCustomText() {
    json p = {{"type", "customText"}, {"remark", "x"}};
    auto r = BuildScriptActionFromJson(p);
    Emit(L"reject_custom_text", !r.ok,
        r.ok ? L"customText should fail" : L"");
}

void CaseNormalizeRenumber() {
    std::vector<ScriptAction> actions(2);
    actions[0].type = ActionType::Wait;
    actions[0].originalNo = 99;
    actions[0].customText = L"junk";
    actions[1].type = ActionType::EndLoop;
    actions[1].originalNo = 0;
    NormalizeScriptActionList(actions);
    const bool ok = actions[0].originalNo == 1 && actions[0].customText.empty()
        && actions[1].originalNo == 2 && actions[1].customText == L"跳出循环";
    Emit(L"normalize_renumber", ok,
        ok ? L"" : L"NormalizeScriptActionList renumber/customText rules broken");
}

void CaseEnsureStopMacroAppends() {
    std::vector<ScriptAction> actions(1);
    actions[0].type = ActionType::Wait;
    const bool added = EnsureStopMacroOnActions(actions);
    const bool ok = added && actions.size() == 2 && actions[1].type == ActionType::StopMacro;
    Emit(L"ensure_stop_macro_appends", ok,
        ok ? L"" : L"Should append stopMacro when missing");
}

void CaseEnsureStopMacroSkipInfinite() {
    std::vector<ScriptAction> actions(1);
    actions[0].type = ActionType::Loop;
    actions[0].indent = 0;
    actions[0].loopCount = -1;
    const bool added = EnsureStopMacroOnActions(actions);
    Emit(L"ensure_stop_macro_skip_infinite", !added && actions.size() == 1,
        added ? L"Must not append stopMacro for top-level infinite loop" : L"");
}

void CaseEndLoopNeedsParent() {
    std::vector<ScriptAction> actions(1);
    actions[0].type = ActionType::EndLoop;
    actions[0].indent = 0;
    const std::wstring err = ValidateEndLoopPlacements(actions);
    Emit(L"endloop_needs_parent", !err.empty(),
        err.empty() ? L"orphan endLoop should error" : L"");
}

void CaseEndLoopInsideLoopOk() {
    std::vector<ScriptAction> actions(3);
    actions[0].type = ActionType::Loop;
    actions[0].indent = 0;
    actions[0].loopCount = 2;
    actions[1].type = ActionType::Wait;
    actions[1].indent = 1;
    actions[2].type = ActionType::EndLoop;
    actions[2].indent = 1;
    const std::wstring err = ValidateEndLoopPlacements(actions);
    Emit(L"endloop_inside_loop_ok", err.empty(), err.c_str());
}

void CaseBuildArrayJson() {
    std::wstring error;
    std::vector<json> params;
    params.push_back({{"type", "wait"}, {"duration", 0.1}});
    const std::wstring out = BuildScriptActionsJsonArray(params, error);
    // 断言动作 JSON 含 wait + 自动追加的 stopMacro（勿依赖中文 tip 文案）
    const bool ok = error.empty()
        && out.find(L"\"type\": \"wait\"") != std::wstring::npos
        && out.find(L"\"type\": \"stopMacro\"") != std::wstring::npos;
    Emit(L"build_array_json", ok, error.empty() ? L"ok" : error.c_str());
}

void CaseFlattenChildrenLoopBody() {
    json loop = {
        {"type", "loop"},
        {"loopCount", -1},
        {"remark", "无限循环"},
        {"children", json::array({
            {{"type", "wait"}, {"duration", 1}, {"indent", 0}, {"remark", "体内等待"}},
            {{"type", "keyClick"}, {"keyText", "a"}, {"indent", 0}}
        })}
    };
    std::wstring error;
    std::vector<json> flat;
    const bool flattened = FlattenNestedActionParamList({loop}, flat, error);
    bool indentsOk = flattened && error.empty() && flat.size() == 3
        && flat[0].value("indent", -1) == 0
        && flat[1].value("indent", -1) == 1
        && flat[2].value("indent", -1) == 1;

    std::wstring buildErr;
    const std::wstring built = BuildScriptActionsJsonArray({loop}, buildErr);
    const bool buildOk = buildErr.empty()
        && built.find(L"\"type\": \"loop\"") != std::wstring::npos
        && built.find(L"\"type\": \"wait\"") != std::wstring::npos
        && built.find(L"\"type\": \"keyClick\"") != std::wstring::npos
        && built.find(L"\"type\": \"stopMacro\"") == std::wstring::npos;

    const bool ok = indentsOk && buildOk;
    std::wstring detail;
    if (!flattened) detail += error;
    if (!buildErr.empty()) detail += L" " + buildErr;
    if (!indentsOk) detail += L" indent mismatch";
    Emit(L"flatten_children_loop_body", ok, detail.c_str());
}

void CaseEmptyLoopRejected() {
    std::vector<json> params = {
        {{"type", "loop"}, {"loopCount", -1}},
        {{"type", "wait"}, {"duration", 1}},
        {{"type", "keyClick"}, {"keyText", "a"}}
    };
    std::wstring error;
    const std::wstring out = BuildScriptActionsJsonArray(params, error);
    const bool ok = !error.empty()
        && error.find(L"没有子动作") != std::wstring::npos
        && error.find(L"children") != std::wstring::npos;
    Emit(L"empty_loop_rejected", ok,
        ok ? L"" : (error.empty() ? L"empty loop should fail" : error.c_str()));
    (void)out;
}

void CaseFlattenIfElseInsideLoop() {
    json tree = {
        {"type", "loop"},
        {"loopCount", -1},
        {"children", json::array({
            {{"type", "findImage"}, {"imagePath", "images\\btn.bmp"}, {"followUp", "saveVar"},
                {"matchVarName", "btn"}},
            {{"type", "if"}, {"conditionExpr", "btn.matchData > 0"},
                {"children", json::array({
                    {{"type", "mouseClick"}}
                })}},
            {{"type", "else"},
                {"children", json::array({
                    {{"type", "wait"}, {"duration", 0.5}}
                })}}
        })}
    };
    std::wstring error;
    std::vector<json> flat;
    const bool flattened = FlattenNestedActionParamList({tree}, flat, error);
    const bool ok = flattened && error.empty() && flat.size() == 6
        && flat[0].value("indent", -1) == 0
        && flat[1].value("indent", -1) == 1
        && flat[2].value("indent", -1) == 1
        && flat[3].value("indent", -1) == 2
        && flat[4].value("indent", -1) == 1
        && flat[5].value("indent", -1) == 2;
    std::wstring detail = error;
    if (ok) detail.clear();
    else if (detail.empty())
        detail = L"size=" + std::to_wstring(flat.size());
    Emit(L"flatten_if_else_inside_loop", ok, detail.c_str());
}

void CasePlanOutlineShowsTree() {
    json tree = {
        {"type", "loop"},
        {"loopCount", -1},
        {"remark", "挂机"},
        {"children", json::array({
            {{"type", "wait"}, {"remark", "体内"}}
        })}
    };
    std::wstring error;
    const std::wstring outline = PlanScriptActionsOutline({tree}, error);
    const bool ok = error.empty()
        && outline.find(L"无限循环") != std::wstring::npos
        && outline.find(L"等待") != std::wstring::npos
        && outline.find(L"  第2步") != std::wstring::npos
        && outline.find(L"createMacroScript") != std::wstring::npos;
    Emit(L"plan_outline_shows_tree", ok,
        ok ? L"" : (error.empty() ? outline.substr(0, 160).c_str() : error.c_str()));
}

void CaseNonContainerChildrenRejected() {
    json wait = {
        {"type", "wait"},
        {"duration", 1},
        {"children", json::array({
            {{"type", "keyClick"}, {"keyText", "a"}}
        })}
    };
    std::wstring error;
    std::vector<json> flat;
    const bool flattened = FlattenNestedActionParamList({wait}, flat, error);
    const bool ok = !flattened && !error.empty()
        && error.find(L"不是容器") != std::wstring::npos;
    Emit(L"non_container_children_rejected", ok,
        ok ? L"" : (error.empty() ? L"should reject children on wait" : error.c_str()));
}

void CaseFragmentLoopSkipsTreeCheck() {
    json loop = {{"type", "loop"}, {"loopCount", -1}};
    std::wstring errStrict;
    BuildScriptActionsJsonArray({loop}, errStrict, true);
    std::wstring errFrag;
    const std::wstring frag = BuildScriptActionsJsonArray({loop}, errFrag, false);
    const bool ok = !errStrict.empty()
        && errStrict.find(L"没有子动作") != std::wstring::npos
        && errFrag.empty()
        && frag.find(L"\"type\": \"loop\"") != std::wstring::npos;
    Emit(L"fragment_loop_skips_tree_check", ok,
        ok ? L"" : (errStrict + L" | frag=" + errFrag).c_str());
}

void CaseIndentSiblingsFormLoopBody() {
    std::vector<json> params = {
        {{"type", "loop"}, {"loopCount", -1}, {"indent", 0}},
        {{"type", "wait"}, {"duration", 1}, {"indent", 1}},
        {{"type", "keyClick"}, {"keyText", "a"}, {"indent", 1}}
    };
    std::wstring error;
    const std::wstring out = BuildScriptActionsJsonArray(params, error, true);
    const bool ok = error.empty()
        && out.find(L"\"type\": \"loop\"") != std::wstring::npos
        && out.find(L"\"type\": \"wait\"") != std::wstring::npos;
    Emit(L"indent_siblings_form_loop_body", ok,
        error.empty() ? L"" : error.c_str());
}

void CaseBuildMoveMouseRelative() {
    json p = {{"type", "moveMouseRelative"}, {"x", -12}, {"y", 34}};
    auto r = BuildScriptActionFromJson(p);
    const bool ok = r.ok && r.action.type == ActionType::MoveMouseRelative
        && r.action.x == -12 && r.action.y == 34
        && !r.action.coordsAreNormalized;
    Emit(L"build_move_mouse_relative", ok, r.ok ? L"" : r.error.c_str());
}

void CaseInterRepeatIntervalSemantics() {
    ScriptAction click{};
    click.type = ActionType::MouseClick;
    click.clickCount = 1;
    click.duration = 0.5;
    const bool onceOk = ActionUsesInterRepeatInterval(ActionType::MouseClick)
        && !ShouldWaitAfterRepeat(click, 0)
        && !ActionUsesInterRepeatInterval(ActionType::Wait);

    click.clickCount = 3;
    const bool betweenOk = ShouldWaitAfterRepeat(click, 0)
        && ShouldWaitAfterRepeat(click, 1)
        && !ShouldWaitAfterRepeat(click, 2)
        && !ShouldWaitAfterRepeat(click, -1);

    ScriptAction key{};
    key.type = ActionType::KeyClick;
    key.clickCount = 2;
    key.duration = 0.2;
    ScriptAction scroll{};
    scroll.type = ActionType::ScrollWheel;
    scroll.clickCount = 2;
    ScriptAction play{};
    play.type = ActionType::MousePlayback;
    play.clickCount = 2;
    ScriptAction runMac{};
    runMac.type = ActionType::RunMacro;
    runMac.clickCount = 2;
    ScriptAction runBlk{};
    runBlk.type = ActionType::RunBlock;
    runBlk.clickCount = 2;
    const bool typesOk = ShouldWaitAfterRepeat(key, 0)
        && ShouldWaitAfterRepeat(scroll, 0)
        && ShouldWaitAfterRepeat(play, 0)
        && ShouldWaitAfterRepeat(runMac, 0)
        && ShouldWaitAfterRepeat(runBlk, 0)
        && ActionUsesInterRepeatInterval(ActionType::HotkeyShortcut)
        && ActionUsesInterRepeatInterval(ActionType::QuickInput)
        && ActionUsesInterRepeatInterval(ActionType::RunMacro)
        && ActionUsesInterRepeatInterval(ActionType::RunBlock)
        && ActionUsesInterRepeatInterval(ActionType::FindImage)
        && ActionUsesInterRepeatInterval(ActionType::MultiMatch)
        && !ActionUsesInterRepeatInterval(ActionType::MouseDrag);

    const std::wstring schema = ScriptActionBuilderSchema();
    const bool schemaOk = schema.find(L"相邻两次之间的间隔") != std::wstring::npos
        && schema.find(L"count=1 时完全不等待") != std::wstring::npos;

    const bool ok = onceOk && betweenOk && typesOk && schemaOk;
    Emit(L"inter_repeat_interval_semantics", ok,
        ok ? L"" : L"inter-repeat interval helper/schema out of sync");
}

void CaseBuildFindImageSaveImage() {
    json p = {
        {"type", "findImage"},
        {"followUp", "saveImage"},
        {"imageUseVar", 1},
        {"imagePath", "image"},
        {"searchFullScreen", 1},
        {"matchThreshold", 65}
    };
    auto r = BuildScriptActionFromJson(p);
    const bool ok = r.ok
        && r.action.type == ActionType::FindImage
        && r.action.findImageFollowUp == 3
        && r.action.imageUseVar
        && r.action.matchVarName == L"image"
        && r.action.findTimeExpr == L"0";

    json timed = {
        {"type", "findImage"},
        {"followUp", "saveVar"},
        {"imagePath", "images\\a.bmp"},
        {"findTimeExpr", "5"}
    };
    auto rTime = BuildScriptActionFromJson(timed);
    const bool saveMatchTimeOk = rTime.ok && rTime.action.findImageFollowUp == 2
        && rTime.action.findTimeExpr == L"5";

    json saveImgTimed = {
        {"type", "findImage"},
        {"followUp", "saveImage"},
        {"imagePath", "images\\a.bmp"},
        {"findTimeExpr", "3"}
    };
    auto rImg = BuildScriptActionFromJson(saveImgTimed);
    const bool saveImgTimeOk = rImg.ok && rImg.action.findImageFollowUp == 3
        && rImg.action.findTimeExpr == L"3";

    json noTpl = {
        {"type", "findImage"},
        {"followUp", "saveImage"},
        {"findTimeExpr", "8"}
    };
    auto rNoTpl = BuildScriptActionFromJson(noTpl);
    const bool noTplOk = rNoTpl.ok && rNoTpl.action.findImageFollowUp == 3
        && rNoTpl.action.imagePath.empty()
        && rNoTpl.action.findTimeExpr == L"0";

    const bool all = ok && saveMatchTimeOk && saveImgTimeOk && noTplOk;
    std::wstring detail;
    if (!ok) detail += r.ok ? L"defaults " : r.error + L" ";
    if (!saveMatchTimeOk) detail += L"saveVar-time ";
    if (!saveImgTimeOk) detail += L"saveImage-time ";
    if (!noTplOk) detail += L"no-tpl-not-cleared";
    Emit(L"build_findimage_save_image", all, all ? L"" : detail.c_str());
}

void CaseBuildMultiMatch() {
    json miss = {{"type", "multiMatch"}};
    auto rMiss = BuildScriptActionFromJson(miss);
    const bool rejected = !rMiss.ok && rMiss.error.find(L"imagePaths") != std::wstring::npos;

    json p = {
        {"type", "multiMatch"},
        {"imagePaths", json::array({"images\\a.png", "images\\b.png"})},
        {"multiMatchMode", 1},
        {"followUp", "saveVar"},
        {"duration", 0.05}
    };
    auto r = BuildScriptActionFromJson(p);
    const bool ok = r.ok && r.action.type == ActionType::MultiMatch
        && r.action.imagePaths.size() == 2
        && r.action.multiMatchMode == 1
        && r.action.findImageFollowUp == 2
        && std::abs(r.action.duration - 0.05) < 1e-9;

    json clamp = {
        {"type", "multiMatch"},
        {"imagePaths", json::array({"images\\a.png"})},
        {"followUp", "saveImage"}
    };
    auto rClamp = BuildScriptActionFromJson(clamp);
    const bool clamped = rClamp.ok && rClamp.action.findImageFollowUp == 2;

    json timeKeep = {
        {"type", "multiMatch"},
        {"imagePaths", json::array({"images\\a.png"})},
        {"followUp", "saveVar"},
        {"findTimeExpr", "5"}
    };
    auto rTime = BuildScriptActionFromJson(timeKeep);
    const bool timeOk = rTime.ok && rTime.action.findTimeExpr == L"5";

    const bool all = rejected && ok && clamped && timeOk;
    std::wstring detail;
    if (!rejected) detail += L"missing-path-not-rejected ";
    if (!ok) detail += (r.ok ? L"fields " : r.error + L" ");
    if (!clamped) detail += L"saveImage-not-clamped ";
    if (!timeOk) detail += L"findTimeExpr-cleared";
    Emit(L"build_multimatch_ok", all, all ? L"" : detail.c_str());
}

void CaseBuildMultiMatchImageUseVar() {
    json p = {
        {"type", "multiMatch"},
        {"imagePaths", json::array({"fooVar", "images\\b.png"})},
        {"imageUseVar", 1},
        {"imageUseVars", json::array({1, 0})}
    };
    auto r = BuildScriptActionFromJson(p);
    const bool ok = r.ok
        && r.action.imageUseVar
        && r.action.imagePaths.size() == 2
        && r.action.imagePaths[0] == L"fooVar"
        && r.action.imageUseVars.size() == 2
        && r.action.imageUseVars[0] != 0
        && r.action.imageUseVars[1] == 0;
    Emit(L"build_multimatch_image_use_var", ok, r.ok ? L"" : r.error.c_str());
}

void CaseBuildMultiMatchHoleUseVars() {
    json p = {
        {"type", "multiMatch"},
        {"imagePaths", json::array({"aaVar", "", "images\\c.png"})},
        {"imageUseVars", json::array({1, 0, 0})}
    };
    auto r = BuildScriptActionFromJson(p);
    const bool ok = r.ok
        && r.action.imagePaths.size() == 2
        && r.action.imagePaths[0] == L"aaVar"
        && r.action.imagePaths[1].find(L"c.png") != std::wstring::npos
        && r.action.imageUseVars.size() == 2
        && r.action.imageUseVars[0] != 0
        && r.action.imageUseVars[1] == 0;
    Emit(L"build_multimatch_hole_use_vars", ok, r.ok ? L"" : r.error.c_str());
}

void CaseLookupMultiMatch() {
    const std::wstring d = LookupMacroActionSchema(L"multiMatch");
    const bool ok = d.find(L"imagePaths") != std::wstring::npos
        && d.find(L"[0].x") != std::wstring::npos
        && d.find(L"一图多处") != std::wstring::npos;
    Emit(L"lookup_multimatch", ok, ok ? L"" : d.substr(0, 220).c_str());
}

void CaseBuildQuickInputParseEscapes() {
    json def = {{"type", "quickInput"}, {"inputText", "a\\nb"}};
    auto r1 = BuildScriptActionFromJson(def);
    json on = {{"type", "quickInput"}, {"inputText", "a\\nb"}, {"parseEscapes", 1}};
    auto r2 = BuildScriptActionFromJson(on);
    json off = {{"type", "quickInput"}, {"inputText", "a\\nb"}, {"parseEscapes", 0}};
    auto r3 = BuildScriptActionFromJson(off);
    const std::wstring schema = ScriptActionBuilderSchema();
    const bool schemaOk = schema.find(L"parseEscapes") != std::wstring::npos
        && schema.find(L"缺省0") != std::wstring::npos;
    const bool ok = r1.ok && !r1.action.parseEscapes
        && r2.ok && r2.action.parseEscapes
        && r3.ok && !r3.action.parseEscapes
        && schemaOk;
    Emit(L"build_quickinput_parse_escapes", ok,
        ok ? L"" : (r1.ok && r2.ok && r3.ok
            ? L"schema missing parseEscapes/缺省0"
            : (r1.ok ? (r2.ok ? r3.error : r2.error) : r1.error)).c_str());
}

void CaseResolveKeyNavNames() {
    // 回归：旧 ResolveKeyVk 把 Home→'H'、Up→'U'、Right→'R'，Excel 填表会污染成 H序号/H1
    struct Expect {
        const char* keyText;
        UINT vk;
    };
    const Expect cases[] = {
        {"Home", VK_HOME},
        {"HOME", VK_HOME},
        {"home", VK_HOME},
        {"Up", VK_UP},
        {"UP", VK_UP},
        {"Right", VK_RIGHT},
        {"Delete", VK_DELETE},
        {"delete", VK_DELETE},
        {"Backspace", VK_BACK},
        {"enter", VK_RETURN},
        {"ENTER", VK_RETURN},
        {"PageDown", VK_NEXT},
        {"Page Up", VK_PRIOR},
        {"End", VK_END},
        {"Left", VK_LEFT},
        {"Down", VK_DOWN},
        {"Tab", VK_TAB},
        {"F2", VK_F2},
        {"a", 'A'},
        {"7", '7'},
    };
    bool ok = true;
    std::wstring detail;
    for (const auto& c : cases) {
        json p = {{"type", "keyClick"}, {"keyText", c.keyText}};
        auto r = BuildScriptActionFromJson(p);
        if (!r.ok || r.action.keyVk != c.vk) {
            ok = false;
            detail += L" ";
            detail += FromUtf8(c.keyText);
            detail += L"→vk=";
            detail += std::to_wstring(r.ok ? r.action.keyVk : 0);
            detail += L"(want ";
            detail += std::to_wstring(c.vk);
            detail += L")";
        }
    }
    const json arrow = json::parse("{\"type\":\"keyClick\",\"keyText\":\"\\u2190\"}");
    auto ra = BuildScriptActionFromJson(arrow);
    if (!ra.ok || ra.action.keyVk != VK_LEFT) {
        ok = false;
        detail += L" glyph←";
    }
    const json arrowBad = json::parse(
        "{\"type\":\"keyClick\",\"keyText\":\"\\u2190\",\"keyVk\":8592}");
    auto rb = BuildScriptActionFromJson(arrowBad);
    if (!rb.ok || rb.action.keyVk != VK_LEFT) {
        ok = false;
        detail += L" vk=8592";
    }
    Emit(L"resolve_key_nav_names", ok, detail.empty() ? L"" : detail.c_str());
}

void CaseRejectUnknownKeyText() {
    json p = {{"type", "keyClick"}, {"keyText", "NotARealKey"}};
    auto r = BuildScriptActionFromJson(p);
    // 旧逻辑会回落为首字母 'N' 并 ok；必须拒绝
    const bool ok = !r.ok && r.action.keyVk != L'N';
    Emit(L"reject_unknown_keytext", ok,
        r.ok ? L"unknown keyText must fail (no first-letter fallback)" : L"");
}

void CaseBuildRunBlockRepeatFields() {
    json p = {
        {"type", "runBlock"},
        {"blockName", "block1"},
        {"clickCount", 4},
        {"duration", 0.2},
        {"randomDuration", 0.05}
    };
    auto r = BuildScriptActionFromJson(p);
    json m = {
        {"type", "runMacro"},
        {"targetPath", "foo.json"},
        {"clickCount", 3},
        {"duration", 0.15}
    };
    auto rm = BuildScriptActionFromJson(m);
    const bool ok = r.ok && r.action.type == ActionType::RunBlock
        && r.action.clickCount == 4 && r.action.duration == 0.2
        && r.action.randomDuration == 0.05
        && rm.ok && rm.action.type == ActionType::RunMacro
        && rm.action.clickCount == 3 && rm.action.duration == 0.15;
    Emit(L"build_runblock_repeat_fields", ok, r.ok ? L"" : r.error.c_str());
}

void CaseBuildMousePlaybackSpeed() {
    json p = {
        {"type", "mousePlayback"},
        {"targetPath", "rec.json"},
        {"playbackSpeed", 2.0}
    };
    auto r = BuildScriptActionFromJson(p);
    json def = {{"type", "mousePlayback"}, {"targetPath", "rec.json"}};
    auto rd = BuildScriptActionFromJson(def);
    json hi = {
        {"type", "mousePlayback"},
        {"targetPath", "rec.json"},
        {"playbackSpeed", 9.0}
    };
    auto rh = BuildScriptActionFromJson(hi);
    const bool ok = r.ok && r.action.type == ActionType::MousePlayback
        && std::abs(r.action.playbackSpeed - 2.0) < 1e-9
        && rd.ok && std::abs(rd.action.playbackSpeed - 1.0) < 1e-9
        && rh.ok && std::abs(rh.action.playbackSpeed - 4.0) < 1e-9;
    Emit(L"build_mouseplayback_speed", ok, r.ok ? L"" : r.error.c_str());
}

void CaseBuildNestedUseMode() {
    json def = {{"type", "runMacro"}, {"targetPath", "a.json"}};
    auto rd = BuildScriptActionFromJson(def);
    json win = {
        {"type", "mousePlayback"},
        {"targetPath", "rec.json"},
        {"useMode", "window"},
        {"breakoutTimeSeconds", 0.8},
        {"nestedWindowMode", {
            {"enabled", 1},
            {"executionKind", "hiddenDesktop"},
            {"selectMethod", "useEditorWindowClass"},
            {"targetExePath", "game.exe"},
            {"windowClassName", "Wnd"}
        }}
    };
    auto rw = BuildScriptActionFromJson(win);
    json bg = {
        {"type", "runMacro"},
        {"targetPath", "b.json"},
        {"useMode", 2}
    };
    auto rb = BuildScriptActionFromJson(bg);
    const bool ok = rd.ok && rd.action.useMode == kNestedUseModeInherit
        && rw.ok && rw.action.useMode == kNestedUseModeWindow
        && rw.action.nestedWindowMode.windowClassName == L"Wnd"
        && rw.action.nestedWindowMode.targetExePath == L"game.exe"
        && std::abs(rw.action.breakoutTimeSeconds - 0.8) < 1e-9
        && rb.ok && rb.action.useMode == kNestedUseModeBackground
        && rb.action.nestedWindowMode.executionKind
            == windowmode::WindowModeExecutionKind::BackgroundWindow;
    Emit(L"build_nested_use_mode", ok, rw.ok ? L"" : rw.error.c_str());
}

void CaseDisassembleUnwrapLoop() {
    std::vector<ScriptAction> actions(3);
    actions[0].type = ActionType::Loop;
    actions[0].indent = 0;
    actions[0].loopCount = 5;
    actions[1].type = ActionType::MouseClick;
    actions[1].indent = 1;
    actions[2].type = ActionType::Wait;
    actions[2].indent = 1;
    const auto patch = DisassembleActionAt(actions, 0, nullptr);
    ApplyDisassemblePatch(actions, patch);
    const bool ok = patch.ok && actions.size() == 2
        && actions[0].type == ActionType::MouseClick && actions[0].indent == 0
        && actions[1].type == ActionType::Wait && actions[1].indent == 0;
    Emit(L"disassemble_unwrap_loop", ok, patch.error.c_str());
}

void CaseDisassembleRunBlockKeepsDefine() {
    std::vector<ScriptAction> actions(4);
    actions[0].type = ActionType::RunBlock;
    actions[0].blockName = L"block1";
    actions[0].indent = 0;
    actions[1].type = ActionType::DefineBlock;
    actions[1].blockName = L"block1";
    actions[1].indent = 0;
    actions[2].type = ActionType::MouseClick;
    actions[2].indent = 1;
    actions[3].type = ActionType::MoveMouse;
    actions[3].indent = 1;
    const auto patch = DisassembleActionAt(actions, 0, nullptr);
    ApplyDisassemblePatch(actions, patch);
    const bool ok = patch.ok && actions.size() == 5
        && actions[0].type == ActionType::MouseClick && actions[0].indent == 0
        && actions[1].type == ActionType::MoveMouse && actions[1].indent == 0
        && actions[2].type == ActionType::DefineBlock
        && actions[3].type == ActionType::MouseClick && actions[3].indent == 1
        && actions[4].type == ActionType::MoveMouse && actions[4].indent == 1;
    Emit(L"disassemble_run_block_keeps_define", ok, patch.error.c_str());
}

void CaseDisassembleNestedIndent() {
    std::vector<ScriptAction> actions(4);
    actions[0].type = ActionType::Loop;
    actions[0].indent = 0;
    actions[1].type = ActionType::RunBlock;
    actions[1].blockName = L"A";
    actions[1].indent = 1;
    actions[2].type = ActionType::DefineBlock;
    actions[2].blockName = L"A";
    actions[2].indent = 0;
    actions[3].type = ActionType::KeyClick;
    actions[3].indent = 1;
    const auto patch = DisassembleActionAt(actions, 1, nullptr);
    ApplyDisassemblePatch(actions, patch);
    const bool ok = patch.ok && actions.size() == 4
        && actions[0].type == ActionType::Loop && actions[0].indent == 0
        && actions[1].type == ActionType::KeyClick && actions[1].indent == 1
        && actions[2].type == ActionType::DefineBlock
        && actions[3].type == ActionType::KeyClick && actions[3].indent == 1;
    Emit(L"disassemble_nested_indent", ok, patch.error.c_str());
}

void CaseDisassembleEmptyRejected() {
    std::vector<ScriptAction> emptyLoop(1);
    emptyLoop[0].type = ActionType::Loop;
    emptyLoop[0].indent = 0;
    const auto p1 = DisassembleActionAt(emptyLoop, 0, nullptr);
    std::vector<ScriptAction> run(1);
    run[0].type = ActionType::RunMacro;
    run[0].indent = 0;
    const auto p2 = DisassembleActionAt(run, 0, nullptr);
    const bool ok = !p1.ok && emptyLoop.size() == 1 && emptyLoop[0].type == ActionType::Loop
        && !p2.ok && run.size() == 1 && run[0].type == ActionType::RunMacro;
    Emit(L"disassemble_empty_rejected", ok, L"");
}

void CaseDisassembleRunMacroLoader() {
    std::vector<ScriptAction> actions(1);
    actions[0].type = ActionType::RunMacro;
    actions[0].targetPath = L"nested.json";
    actions[0].indent = 2;
    actions[0].clickCount = 3;
    auto loader = [](const std::wstring& path, std::vector<ScriptAction>& out, std::wstring& err) {
        if (path != L"nested.json") {
            err = L"missing";
            return false;
        }
        ScriptAction a{};
        a.type = ActionType::Wait;
        a.indent = 0;
        ScriptAction b{};
        b.type = ActionType::StopMacro;
        b.indent = 0;
        out = {a, b};
        return true;
    };
    const auto patch = DisassembleActionAt(actions, 0, loader);
    ApplyDisassemblePatch(actions, patch);
    const bool ok = patch.ok && actions.size() == 2
        && actions[0].type == ActionType::Wait && actions[0].indent == 2
        && actions[1].type == ActionType::StopMacro && actions[1].indent == 2;
    Emit(L"disassemble_run_macro_loader", ok, patch.error.c_str());
}

ScriptAction MergeAct(ActionType type, int indent) {
    ScriptAction a;
    a.type = type;
    a.indent = indent;
    return a;
}

void CaseMergeContiguousLoop() {
    std::vector<ScriptAction> actions = {
        MergeAct(ActionType::Wait, 0),
        MergeAct(ActionType::MouseClick, 0),
        MergeAct(ActionType::KeyClick, 0),
    };
    ScriptAction loop = MergeAct(ActionType::Loop, 0);
    loop.loopCount = 3;
    const auto r = MergeSelectedIntoContainer(actions, {0, 1, 2}, loop, MergePlacement::InPlace);
    const bool ok = r.ok && r.containerIndex == 0 && r.actions.size() == 4
        && r.actions[0].type == ActionType::Loop && r.actions[0].indent == 0
        && r.actions[0].loopCount == 3
        && r.actions[1].type == ActionType::Wait && r.actions[1].indent == 1
        && r.actions[2].type == ActionType::MouseClick && r.actions[2].indent == 1
        && r.actions[3].type == ActionType::KeyClick && r.actions[3].indent == 1;
    Emit(L"merge_contiguous_loop", ok, r.error.c_str());
}

void CaseMergeIgnoreInnerChecks() {
    std::vector<ScriptAction> actions = {
        MergeAct(ActionType::Loop, 0),
        MergeAct(ActionType::Wait, 1),
        MergeAct(ActionType::MouseClick, 1),
        MergeAct(ActionType::KeyClick, 0),
    };
    ScriptAction wrap = MergeAct(ActionType::Loop, 0);
    const auto r = MergeSelectedIntoContainer(actions, {0, 1, 2, 3}, wrap, MergePlacement::InPlace);
    const bool ok = r.ok && r.actions.size() == 5
        && r.actions[0].type == ActionType::Loop && r.actions[0].indent == 0
        && r.actions[1].type == ActionType::Loop && r.actions[1].indent == 1
        && r.actions[2].type == ActionType::Wait && r.actions[2].indent == 2
        && r.actions[3].type == ActionType::MouseClick && r.actions[3].indent == 2
        && r.actions[4].type == ActionType::KeyClick && r.actions[4].indent == 1;
    Emit(L"merge_ignore_inner_checks", ok, r.error.c_str());
}

void CaseMergeDiscretePlacements() {
    const auto base = std::vector<ScriptAction>{
        MergeAct(ActionType::Wait, 0),
        MergeAct(ActionType::MouseClick, 0),
        MergeAct(ActionType::KeyClick, 0),
        MergeAct(ActionType::MoveMouse, 0),
    };
    ScriptAction loop = MergeAct(ActionType::Loop, 0);
    const auto before = MergeSelectedIntoContainer(base, {0, 2}, loop, MergePlacement::Before);
    const auto after = MergeSelectedIntoContainer(base, {0, 2}, loop, MergePlacement::After);
    const auto first = MergeSelectedIntoContainer(base, {0, 2}, loop, MergePlacement::First);
    const auto last = MergeSelectedIntoContainer(base, {0, 2}, loop, MergePlacement::Last);
    const bool okBefore = before.ok && before.containerIndex == 0
        && before.actions.size() == 5
        && before.actions[0].type == ActionType::Loop
        && before.actions[1].type == ActionType::Wait && before.actions[1].indent == 1
        && before.actions[2].type == ActionType::KeyClick && before.actions[2].indent == 1
        && before.actions[3].type == ActionType::MouseClick && before.actions[3].indent == 0
        && before.actions[4].type == ActionType::MoveMouse && before.actions[4].indent == 0;
    // after extract 0 and 2: remaining click(1), move(3). After last selected (key) → [click, NEW, move]
    const bool okAfter = after.ok && after.containerIndex == 1
        && after.actions[0].type == ActionType::MouseClick && after.actions[0].indent == 0
        && after.actions[1].type == ActionType::Loop
        && after.actions[2].type == ActionType::Wait && after.actions[2].indent == 1
        && after.actions[3].type == ActionType::KeyClick && after.actions[3].indent == 1
        && after.actions.size() == 5
        && after.actions[4].type == ActionType::MoveMouse && after.actions[4].indent == 0;
    const bool okFirst = first.ok && first.containerIndex == 0
        && first.actions[0].type == ActionType::Loop
        && first.actions[3].type == ActionType::MouseClick;
    const bool okLast = last.ok && last.containerIndex == 2
        && last.actions[0].type == ActionType::MouseClick
        && last.actions[1].type == ActionType::MoveMouse
        && last.actions[2].type == ActionType::Loop && last.actions[2].indent == 0;
    const bool ok = okBefore && okAfter && okFirst && okLast;
    Emit(L"merge_discrete_placements", ok, before.error.c_str());
}

void CaseMergeElseRequiresIf() {
    std::vector<ScriptAction> noIf = {
        MergeAct(ActionType::Wait, 0),
        MergeAct(ActionType::MouseClick, 0),
    };
    ScriptAction els = MergeAct(ActionType::Else, 0);
    const auto bad = MergeSelectedIntoContainer(noIf, {0, 1}, els, MergePlacement::InPlace);
    std::vector<ScriptAction> withIf = {
        MergeAct(ActionType::If, 0),
        MergeAct(ActionType::Wait, 1),
        MergeAct(ActionType::MouseClick, 0),
        MergeAct(ActionType::KeyClick, 0),
    };
    const auto good = MergeSelectedIntoContainer(withIf, {2, 3}, els, MergePlacement::InPlace);
    const bool ok = !bad.ok && noIf.size() == 2 && noIf[0].type == ActionType::Wait
        && good.ok && good.actions.size() == 5
        && good.actions[0].type == ActionType::If
        && good.actions[2].type == ActionType::Else && good.actions[2].indent == 0
        && good.actions[3].type == ActionType::MouseClick && good.actions[3].indent == 1
        && good.actions[4].type == ActionType::KeyClick && good.actions[4].indent == 1;
    Emit(L"merge_else_requires_if", ok, good.error.c_str());
}

void CaseMergeDefineBlockToTop() {
    std::vector<ScriptAction> actions = {
        MergeAct(ActionType::Wait, 0),
        MergeAct(ActionType::MouseClick, 0),
        MergeAct(ActionType::KeyClick, 0),
    };
    ScriptAction def = MergeAct(ActionType::DefineBlock, 0);
    def.blockName = L"block1";
    const auto r = MergeSelectedIntoContainer(actions, {1, 2}, def, MergePlacement::InPlace);
    const bool ok = r.ok && r.containerIndex == 0 && r.actions.size() == 4
        && r.actions[0].type == ActionType::DefineBlock && r.actions[0].indent == 0
        && r.actions[1].type == ActionType::MouseClick && r.actions[1].indent == 1
        && r.actions[2].type == ActionType::KeyClick && r.actions[2].indent == 1
        && r.actions[3].type == ActionType::Wait && r.actions[3].indent == 0;
    Emit(L"merge_define_block_to_top", ok, r.error.c_str());
}

void CaseMergeMixedParentRejected() {
    std::vector<ScriptAction> actions = {
        MergeAct(ActionType::Loop, 0),
        MergeAct(ActionType::Wait, 1),
        MergeAct(ActionType::Loop, 0),
        MergeAct(ActionType::MouseClick, 1),
    };
    const auto plan = AnalyzeMergeSelection(actions, {1, 3});
    ScriptAction loop = MergeAct(ActionType::Loop, 0);
    const auto r = MergeSelectedIntoContainer(actions, {1, 3}, loop, MergePlacement::InPlace);
    const bool ok = !plan.ok && !r.ok && actions.size() == 4
        && actions[1].type == ActionType::Wait && actions[3].type == ActionType::MouseClick;
    Emit(L"merge_mixed_parent_rejected", ok, L"");
}

void CaseMergeInplaceDiscreteRejected() {
    std::vector<ScriptAction> actions = {
        MergeAct(ActionType::Wait, 0),
        MergeAct(ActionType::MouseClick, 0),
        MergeAct(ActionType::KeyClick, 0),
    };
    ScriptAction loop = MergeAct(ActionType::Loop, 0);
    const auto plan = AnalyzeMergeSelection(actions, {0, 2});
    const auto r = MergeSelectedIntoContainer(actions, {0, 2}, loop, MergePlacement::InPlace);
    const bool ok = plan.ok && !plan.contiguous && !r.ok && actions.size() == 3
        && actions[1].type == ActionType::MouseClick;
    Emit(L"merge_inplace_discrete_rejected", ok, L"");
}

void CaseMergeNestedInsideLoop() {
    std::vector<ScriptAction> actions = {
        MergeAct(ActionType::Loop, 0),
        MergeAct(ActionType::Wait, 1),
        MergeAct(ActionType::MouseClick, 1),
        MergeAct(ActionType::KeyClick, 1),
    };
    ScriptAction inner = MergeAct(ActionType::If, 0);
    inner.conditionExpr = L"a == 1";
    const auto r = MergeSelectedIntoContainer(actions, {1, 2}, inner, MergePlacement::InPlace);
    const bool ok = r.ok && r.containerIndex == 1 && r.actions.size() == 5
        && r.actions[0].type == ActionType::Loop && r.actions[0].indent == 0
        && r.actions[1].type == ActionType::If && r.actions[1].indent == 1
        && r.actions[2].type == ActionType::Wait && r.actions[2].indent == 2
        && r.actions[3].type == ActionType::MouseClick && r.actions[3].indent == 2
        && r.actions[4].type == ActionType::KeyClick && r.actions[4].indent == 1;
    Emit(L"merge_nested_inside_loop", ok, r.error.c_str());
}

void CaseMergeFirstDiffersFromBefore() {
    const auto base = std::vector<ScriptAction>{
        MergeAct(ActionType::Wait, 0),
        MergeAct(ActionType::MouseClick, 0),
        MergeAct(ActionType::KeyClick, 0),
        MergeAct(ActionType::MoveMouse, 0),
    };
    ScriptAction loop = MergeAct(ActionType::Loop, 0);
    const auto before = MergeSelectedIntoContainer(base, {1, 3}, loop, MergePlacement::Before);
    const auto first = MergeSelectedIntoContainer(base, {1, 3}, loop, MergePlacement::First);
    const bool okBefore = before.ok && before.containerIndex == 1
        && before.actions[0].type == ActionType::Wait && before.actions[0].indent == 0
        && before.actions[1].type == ActionType::Loop && before.actions[1].indent == 0
        && before.actions[2].type == ActionType::MouseClick && before.actions[2].indent == 1
        && before.actions[3].type == ActionType::MoveMouse && before.actions[3].indent == 1
        && before.actions[4].type == ActionType::KeyClick && before.actions[4].indent == 0;
    const bool okFirst = first.ok && first.containerIndex == 0
        && first.actions[0].type == ActionType::Loop
        && first.actions[3].type == ActionType::Wait && first.actions[3].indent == 0
        && first.actions[4].type == ActionType::KeyClick && first.actions[4].indent == 0;
    Emit(L"merge_first_differs_from_before", okBefore && okFirst, before.error.c_str());
}

void CaseFlattenWatchImageChildren() {
    json watch = {
        {"type", "watchImage"},
        {"imagePath", "images\\\\d.bmp"},
        {"matchThreshold", 80},
        {"resumeAfterWatch", 1},
        {"children", json::array({
            {{"type", "wait"}, {"duration", 1}}
        })}
    };
    std::wstring error;
    std::vector<json> flat;
    const bool flattened = FlattenNestedActionParamList({watch}, flat, error);
    const bool ok = flattened && error.empty() && flat.size() == 2
        && flat[0]["type"] == "watchImage"
        && flat[1]["type"] == "wait"
        && flat[1].value("indent", -1) == 1;
    Emit(L"flatten_watchimage_children", ok,
        ok ? L"" : (error.empty() ? L"flatten mismatch" : error.c_str()));
}

void CaseFlattenWatchImageNestedRejected() {
    json loop = {
        {"type", "loop"},
        {"loopCount", 2},
        {"children", json::array({
            {
                {"type", "watchImage"},
                {"imagePath", "images\\\\d.bmp"},
                {"children", json::array({
                    {{"type", "wait"}, {"duration", 1}}
                })}
            }
        })}
    };
    std::wstring error;
    std::vector<json> flat;
    const bool flattened = FlattenNestedActionParamList({loop}, flat, error);
    json watchIndent = {
        {"type", "watchImage"},
        {"imagePath", "images\\\\d.bmp"},
        {"indent", 1}
    };
    std::wstring error2;
    std::vector<json> flat2;
    const bool flattened2 = FlattenNestedActionParamList({watchIndent}, flat2, error2);
    const bool ok = !flattened && !error.empty() && !flattened2 && !error2.empty();
    Emit(L"flatten_watchimage_nested_rejected", ok,
        ok ? L"" : (error + L" | " + error2).c_str());
}

void CaseLookupWatchImageResume() {
    const std::wstring d = LookupMacroActionSchema(L"watchImage");
    const std::wstring v = LookupMacroActionSchema(L"varCompute");
    const bool ok = d.find(L"resumeAfterWatch") != std::wstring::npos
        && d.find(L"imagePath") != std::wstring::npos
        && d.find(L"watchMode") != std::wstring::npos
        && d.find(L"watchPollSeconds") != std::wstring::npos
        && v.find(L"computeCode") != std::wstring::npos
        && v.find(L"ctrl:Clipboard()") != std::wstring::npos;
    Emit(L"lookup_watchimage_resume", ok,
        ok ? L"" : (d.substr(0, 180) + L" | " + v.substr(0, 180)).c_str());
}

void CaseBuildWatchImageTimeMode() {
    json p = {
        {"type", "watchImage"},
        {"imagePath", "images\\\\e.bmp"},
        {"watchMode", "time"},
        {"watchPollSeconds", 2.5},
        {"resumeAfterWatch", 1}
    };
    auto r = BuildScriptActionFromJson(p);
    json numeric = {
        {"type", "watchImage"},
        {"imagePath", "images\\\\e.bmp"},
        {"watchMode", 1},
        {"watchPollInterval", 3.0}
    };
    auto rNum = BuildScriptActionFromJson(numeric);
    std::wstring planErr;
    json planItem = {
        {"type", "watchImage"},
        {"watchMode", "time"},
        {"children", json::array({ {{"type", "wait"}, {"remark", "命中"}} })}
    };
    const std::wstring outline = PlanScriptActionsOutline({planItem}, planErr);
    const bool ok = r.ok && r.action.type == ActionType::WatchImage
        && r.action.watchMode == 1
        && std::abs(r.action.watchPollSeconds - 2.5) < 1e-9
        && rNum.ok && rNum.action.watchMode == 1
        && std::abs(rNum.action.watchPollSeconds - 3.0) < 1e-9
        && planErr.empty() && outline.find(L"时间监视") != std::wstring::npos;
    Emit(L"build_watchimage_time_mode", ok, r.ok ? L"" : r.error.c_str());
}

void CaseBuildVarComputeCode() {
    json p = {{"type", "varCompute"}, {"computeCode", "combo = 1;\nreturn combo;"}};
    auto r = BuildScriptActionFromJson(p);
    const bool ok = r.ok && r.action.type == ActionType::VarCompute
        && r.action.computeCode.find(L"return combo") != std::wstring::npos;
    Emit(L"build_varcompute_code", ok, r.ok ? L"" : r.error.c_str());
}

void CaseBuildMouseDragAbs() {
    json p = {
        {"type", "mouseDrag"},
        {"x", 10}, {"y", 20}, {"endX", 30}, {"endY", 40},
        {"duration", 0.4}, {"button", "right"}
    };
    auto r = BuildScriptActionFromJson(p);
    const bool ok = r.ok && r.action.type == ActionType::MouseDrag
        && r.action.x == 10 && r.action.endX == 30
        && std::abs(r.action.duration - 0.4) < 1e-9
        && r.action.button == MouseButtonType::Right
        && !r.action.imageLocate
        && r.action.clickCount == 1;
    Emit(L"build_mousedrag_abs", ok, r.ok ? L"" : r.error.c_str());
}

void CaseBuildMouseDragImageLocateRequiresPath() {
    json p = {{"type", "mouseDrag"}, {"imageLocate", 1}, {"x", 0}, {"y", 0}, {"endX", 1}, {"endY", 1}};
    auto r = BuildScriptActionFromJson(p);
    const bool rejected = !r.ok && r.error.find(L"imagePath") != std::wstring::npos;
    json p2 = {
        {"type", "mouseDrag"}, {"imageLocate", 1},
        {"imagePath", "images\\\\btn.bmp"},
        {"x", -2}, {"y", 3}, {"endX", 4}, {"endY", -5},
        {"duration", 0.2}
    };
    auto r2 = BuildScriptActionFromJson(p2);
    const bool ok2 = r2.ok && r2.action.imageLocate
        && r2.action.imagePath.find(L"btn.bmp") != std::wstring::npos
        && r2.action.endY == -5
        && std::abs(r2.action.duration - 0.2) < 1e-9;
    Emit(L"build_mousedrag_image_locate_requires_path", rejected && ok2,
        rejected && ok2 ? L"" : (r.error + L" | " + r2.error).c_str());
}

void CaseLookupMouseDrag() {
    const std::wstring d = LookupMacroActionSchema(L"mouseDrag");
    const bool ok = d.find(L"imageLocate") != std::wstring::npos
        && d.find(L"endX") != std::wstring::npos
        && d.find(L"拖拽时长") != std::wstring::npos
        && d.find(L"不是重复间隔") != std::wstring::npos;
    Emit(L"lookup_mousedrag", ok, ok ? L"" : d.substr(0, 220).c_str());
}

void CaseBuildColorImageLocateRequiresPath() {
    json g = {{"type", "getColor"}, {"imageLocate", 1}, {"x", 0}, {"y", 0}};
    auto rg = BuildScriptActionFromJson(g);
    const bool gReject = !rg.ok && rg.error.find(L"imagePath") != std::wstring::npos;
    json g2 = {
        {"type", "getColor"}, {"imageLocate", 1},
        {"imagePath", "images\\\\c.bmp"}, {"x", -2}, {"y", 3}
    };
    auto rg2 = BuildScriptActionFromJson(g2);
    const bool gOk = rg2.ok && rg2.action.imageLocate
        && rg2.action.imagePath.find(L"c.bmp") != std::wstring::npos
        && rg2.action.x == -2;

    json c = {{"type", "colorMatch"}, {"imageLocate", 1},
        {"x", 0}, {"y", 0}, {"color", "#ff0000"}};
    auto rc = BuildScriptActionFromJson(c);
    const bool cReject = !rc.ok && rc.error.find(L"imagePath") != std::wstring::npos;
    json c2 = {
        {"type", "colorMatch"}, {"imageLocate", 1},
        {"imagePath", "images\\\\m.bmp"}, {"x", 1}, {"y", 2}, {"color", "#00ff00"}
    };
    auto rc2 = BuildScriptActionFromJson(c2);
    const bool cOk = rc2.ok && rc2.action.imageLocate
        && rc2.action.imagePath.find(L"m.bmp") != std::wstring::npos;

    json f = {{"type", "findColor"}, {"imageLocate", 1}, {"color", "#0000ff"}};
    auto rf = BuildScriptActionFromJson(f);
    const bool fReject = !rf.ok && rf.error.find(L"imagePath") != std::wstring::npos;
    json f2 = {
        {"type", "findColor"}, {"imageLocate", 1},
        {"imagePath", "images\\\\f.bmp"}, {"color", "#0000ff"}
    };
    auto rf2 = BuildScriptActionFromJson(f2);
    const bool fOk = rf2.ok && rf2.action.imageLocate
        && rf2.action.imagePath.find(L"f.bmp") != std::wstring::npos;

    const bool ok = gReject && gOk && cReject && cOk && fReject && fOk;
    Emit(L"build_color_image_locate_requires_path", ok,
        ok ? L"" : (rg.error + L" | " + rc.error + L" | " + rf.error).c_str());
}

void CaseBuildFindColorFollowUpClamped() {
    json p = {{"type", "findColor"}, {"color", "#ff0000"}, {"followUp", "saveImage"}};
    auto r = BuildScriptActionFromJson(p);
    const bool ok = r.ok && r.action.type == ActionType::FindColor
        && r.action.findImageFollowUp == 2;
    Emit(L"build_findcolor_followup_clamped", ok, r.ok ? L"" : r.error.c_str());
}

void CaseLookupGetColor() {
    const std::wstring d = LookupMacroActionSchema(L"getColor");
    const bool ok = d.find(L"imageLocate") != std::wstring::npos
        && d.find(L"相对图中心") != std::wstring::npos;
    Emit(L"lookup_getcolor", ok, ok ? L"" : d.substr(0, 220).c_str());
}

void PrintHelp() {
    std::fwprintf(stderr,
        L"ScriptActionBuilderSelfTest — 脚本动作构建自检\n"
        L"\n"
        L"用法:\n"
        L"  ScriptActionBuilderSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/script_action_builder.cpp, src/action_tree.h\n");
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
        selftest::PrintCaseList(L"ScriptActionBuilderSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseBuildWait();
    CaseRejectCustomText();
    CaseNormalizeRenumber();
    CaseEnsureStopMacroAppends();
    CaseEnsureStopMacroSkipInfinite();
    CaseEndLoopNeedsParent();
    CaseEndLoopInsideLoopOk();
    CaseBuildArrayJson();
    CaseFlattenChildrenLoopBody();
    CaseEmptyLoopRejected();
    CaseFlattenIfElseInsideLoop();
    CasePlanOutlineShowsTree();
    CaseNonContainerChildrenRejected();
    CaseFragmentLoopSkipsTreeCheck();
    CaseIndentSiblingsFormLoopBody();
    CaseBuildMoveMouseRelative();
    CaseInterRepeatIntervalSemantics();
    CaseBuildFindImageSaveImage();
    CaseBuildMultiMatch();
    CaseBuildMultiMatchImageUseVar();
    CaseBuildMultiMatchHoleUseVars();
    CaseLookupMultiMatch();
    CaseBuildQuickInputParseEscapes();
    CaseResolveKeyNavNames();
    CaseRejectUnknownKeyText();
    CaseBuildRunBlockRepeatFields();
    CaseBuildMousePlaybackSpeed();
    CaseBuildNestedUseMode();
    CaseDisassembleUnwrapLoop();
    CaseDisassembleRunBlockKeepsDefine();
    CaseDisassembleNestedIndent();
    CaseDisassembleEmptyRejected();
    CaseDisassembleRunMacroLoader();
    CaseMergeContiguousLoop();
    CaseMergeIgnoreInnerChecks();
    CaseMergeDiscretePlacements();
    CaseMergeElseRequiresIf();
    CaseMergeDefineBlockToTop();
    CaseMergeMixedParentRejected();
    CaseMergeInplaceDiscreteRejected();
    CaseMergeNestedInsideLoop();
    CaseMergeFirstDiffersFromBefore();
    CaseFlattenWatchImageChildren();
    CaseFlattenWatchImageNestedRejected();
    CaseLookupWatchImageResume();
    CaseBuildWatchImageTimeMode();
    CaseBuildVarComputeCode();
    CaseBuildMouseDragAbs();
    CaseBuildMouseDragImageLocateRequiresPath();
    CaseLookupMouseDrag();
    CaseBuildColorImageLocateRequiresPath();
    CaseBuildFindColorFollowUpClamped();
    CaseLookupGetColor();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
