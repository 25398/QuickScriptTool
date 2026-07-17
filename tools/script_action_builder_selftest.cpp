// =============================================================================
// ScriptActionBuilderSelfTest — Agent 脚本动作构建自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ScriptActionBuilderSelfTest
//   build\Release\ScriptActionBuilderSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "action_tree.h"
#include "action_utils.h"
#include "script_action_builder.h"

#include <nlohmann/json.hpp>

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
    {L"build_move_mouse_relative", L"default",
        L"BuildScriptActionFromJson type=moveMouseRelative with dx/dy"},
    {L"inter_repeat_interval_semantics", L"default",
        L"ShouldWaitAfterRepeat: gap only between repeats; count=1 never waits"},
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
    const bool typesOk = ShouldWaitAfterRepeat(key, 0)
        && ShouldWaitAfterRepeat(scroll, 0)
        && ShouldWaitAfterRepeat(play, 0)
        && ActionUsesInterRepeatInterval(ActionType::HotkeyShortcut)
        && ActionUsesInterRepeatInterval(ActionType::QuickInput);

    const std::wstring schema = ScriptActionBuilderSchema();
    const bool schemaOk = schema.find(L"相邻两次之间的间隔") != std::wstring::npos
        && schema.find(L"count=1 时完全不等待") != std::wstring::npos;

    const bool ok = onceOk && betweenOk && typesOk && schemaOk;
    Emit(L"inter_repeat_interval_semantics", ok,
        ok ? L"" : L"inter-repeat interval helper/schema out of sync");
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
    CaseBuildMoveMouseRelative();
    CaseInterRepeatIntervalSemantics();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
