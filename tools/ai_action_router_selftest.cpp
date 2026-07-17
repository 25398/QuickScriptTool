// =============================================================================
// AiActionRouterSelfTest — AI 动作路由分类自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:AiActionRouterSelfTest
//   build\Release\AiActionRouterSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "ai_action_router.h"

#include <string>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"route_no_image_tool_execute", L"default",
        L"withImage=false always ToolExecute"},
    {L"route_vision_query", L"default",
        L"识图问答关键词 -> VisionQuery"},
    {L"route_composite_click", L"default",
        L"简单点击 -> CompositeClick"},
    {L"route_multi_turn_then", L"default",
        L"点击然后输入 -> MultiTurnTools"},
    {L"route_tool_execute_open", L"default",
        L"打开网页 + image -> ToolExecute"},
    {L"route_default_vision", L"default",
        L"无操作动词默认 VisionQuery"},
    {L"parse_coord_paren", L"default",
        L"TryParseCoordinatePair (12,34)"},
    {L"parse_coord_cn_comma", L"default",
        L"TryParseCoordinatePair 12，34"},
    {L"parse_coord_reject", L"default",
        L"junk text rejected"},
    {L"map_api_point_linear", L"default",
        L"MapApiPointToScreen mid maps to region mid"},
    {L"build_click_json_with_stop", L"default",
        L"BuildScreenClickActionsJson includes stopMacro"},
    {L"build_click_json_no_stop", L"default",
        L"includeStopMacro=false omits stopMacro"},
    {L"vision_system_prompt_has_size", L"default",
        L"Vision system prompt embeds 800x600"},
    {L"route_label_zh", L"default",
        L"AiActionRouteLabel returns non-empty Chinese"},
};

void CaseNoImage() {
    Emit(L"route_no_image_tool_execute",
        ClassifyAiActionRoute(L"点击确认", false) == AiActionRouteKind::ToolExecute, L"");
}

void CaseVision() {
    Emit(L"route_vision_query",
        ClassifyAiActionRoute(L"这是什么颜色", true) == AiActionRouteKind::VisionQuery, L"");
}

void CaseComposite() {
    Emit(L"route_composite_click",
        ClassifyAiActionRoute(L"点击确认按钮", true) == AiActionRouteKind::CompositeClick, L"");
}

void CaseMultiTurn() {
    Emit(L"route_multi_turn_then",
        ClassifyAiActionRoute(L"点击确认然后输入hello", true)
            == AiActionRouteKind::MultiTurnTools, L"");
}

void CaseToolOpen() {
    Emit(L"route_tool_execute_open",
        ClassifyAiActionRoute(L"打开网页 https://example.com", true)
            == AiActionRouteKind::ToolExecute, L"");
}

void CaseDefaultVision() {
    Emit(L"route_default_vision",
        ClassifyAiActionRoute(L"看一下屏幕", true) == AiActionRouteKind::VisionQuery, L"");
}

void CaseParseParen() {
    int x = 0, y = 0;
    const bool ok = TryParseCoordinatePair(L"(12,34)", x, y) && x == 12 && y == 34;
    Emit(L"parse_coord_paren", ok, ok ? L"" : L"paren parse failed");
}

void CaseParseCnComma() {
    int x = 0, y = 0;
    const bool ok = TryParseCoordinatePair(L"12，34", x, y) && x == 12 && y == 34;
    Emit(L"parse_coord_cn_comma", ok, ok ? L"" : L"cn comma parse failed");
}

void CaseParseReject() {
    int x = 0, y = 0;
    Emit(L"parse_coord_reject", !TryParseCoordinatePair(L"no-coords-here", x, y), L"");
}

void CaseMapApi() {
    AiCaptureMapping map{};
    map.capX1 = 100;
    map.capY1 = 200;
    map.capX2 = 300;
    map.capY2 = 400;
    map.apiWidth = 100;
    map.apiHeight = 100;
    int sx = 0, sy = 0;
    MapApiPointToScreen(map, 50, 50, sx, sy);
    Emit(L"map_api_point_linear", sx == 200 && sy == 300,
        (L"sx=" + std::to_wstring(sx) + L" sy=" + std::to_wstring(sy)).c_str());
}

void CaseBuildWithStop() {
    const std::wstring j = BuildScreenClickActionsJson(10, 20, true);
    const bool ok = j.find(L"moveMouse") != std::wstring::npos
        && j.find(L"mouseClick") != std::wstring::npos
        && j.find(L"stopMacro") != std::wstring::npos;
    Emit(L"build_click_json_with_stop", ok, j.c_str());
}

void CaseBuildNoStop() {
    const std::wstring j = BuildScreenClickActionsJson(10, 20, false);
    Emit(L"build_click_json_no_stop",
        j.find(L"stopMacro") == std::wstring::npos, j.c_str());
}

void CaseVisionPromptSize() {
    const std::wstring p = BuildAiActionVisionQuerySystemPrompt(800, 600);
    Emit(L"vision_system_prompt_has_size",
        p.find(L"800") != std::wstring::npos && p.find(L"600") != std::wstring::npos,
        p.size() > 20 ? L"ok" : L"empty prompt");
}

void CaseLabels() {
    const bool ok = !AiActionRouteLabel(AiActionRouteKind::VisionQuery).empty()
        && !AiActionRouteLabel(AiActionRouteKind::CompositeClick).empty()
        && !AiActionRouteLabel(AiActionRouteKind::MultiTurnTools).empty()
        && !AiActionRouteLabel(AiActionRouteKind::ToolExecute).empty();
    Emit(L"route_label_zh", ok, L"");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--json") == 0) {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--list") == 0) {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            std::fwprintf(stderr,
                L"  AiActionRouterSelfTest.exe [--json] [--list] [--help]\n");
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"AiActionRouterSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    if (!selftest::gJson) {
        std::fwprintf(stderr, L"=== AiActionRouterSelfTest ===\n");
    }

    CaseNoImage();
    CaseVision();
    CaseComposite();
    CaseMultiTurn();
    CaseToolOpen();
    CaseDefaultVision();
    CaseParseParen();
    CaseParseCnComma();
    CaseParseReject();
    CaseMapApi();
    CaseBuildWithStop();
    CaseBuildNoStop();
    CaseVisionPromptSize();
    CaseLabels();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
