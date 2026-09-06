// =============================================================================
// AiActionRouterSelfTest — AI 动作路由分类自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:AiActionRouterSelfTest
//   build\Release\AiActionRouterSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "action_utils.h"
#include "agent_ai_actions.h"
#include "ai_action_lookahead.h"
#include "ai_action_router.h"
#include "ai_logic_convert.h"
#include "app_settings_store.h"
#include "color_match.h"
#include "macro_execute_tools.h"
#include "script_action_builder.h"
#include "script_io.h"
#include "script_types.h"

#include <atomic>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"route_no_image_tool_execute", L"default",
        L"withImage=false always ToolExecute"},
    {L"route_vision_query", L"default",
        L"识图问答关键词 -> VisionQuery"},
    {L"route_composite_click", L"default",
        L"简单点击 -> CompositeClick（本地 locateAndClick）"},
    {L"route_click_search_input_box_composite", L"default",
        L"点击…搜索输入框… 勿因「输入」名词误进 MultiTurn"},
    {L"route_click_input_box_composite", L"default",
        L"点击输入框 -> CompositeClick"},
    {L"correct_locate_prompt_has_prev_box", L"default",
        L"全图纠偏 prompt 含上一轮候选框"},
    {L"route_multi_turn_then", L"default",
        L"点击然后输入 -> MultiTurnTools"},
    {L"route_tool_execute_open", L"default",
        L"打开网页 + image -> ToolExecute"},
    {L"route_look_screen_vision", L"default",
        L"看一下屏幕 -> VisionQuery"},
    {L"route_reply_message_tool", L"default",
        L"回答/回复对方消息 -> ToolExecute（须能调 quickInput）"},
    {L"route_reply_send_not_multiturn", L"default",
        L"回答并发送 -> ToolExecute（快路径，非 MultiTurn）"},
    {L"route_ambiguous_default_tool", L"default",
        L"模糊任务默认 ToolExecute 非 VisionQuery"},
    {L"need_screen_click_yes", L"default",
        L"点击类 prompt -> LikelyNeedsScreenCapture"},
    {L"need_screen_var_key_no", L"default",
        L"变量分析+按键 -> 不需要首轮截图"},
    {L"usage_skill_reply_chain", L"default",
        L"section=usage 含 quickInput+Enter 回复链"},
    {L"agent_skill_observe_loop", L"default",
        L"section=agent 含观察/completeTask/分步"},
    {L"atomic_tools_present", L"default",
        L"BuildAiActionExecuteTools 含 quickInput/keyClick/findImage 等规范工具"},
    {L"task_memo_and_open_dedup", L"default",
        L"task memo + skip duplicate openWebpage"},
    {L"task_data_cleared_on_reset", L"default",
        L"ResetAiActionSessionState 清空 saveTaskData 缓存，防串上轮 historyRecords"},
    {L"observe_result_defaults", L"default",
        L"AiObserveCaptureResult / aiObs 常量"},
    {L"atomic_quick_input_rejects_empty", L"default",
        L"规范工具 quickInput 拒空 inputText"},
    {L"atomic_key_click_enter", L"default",
        L"规范工具 keyClick(Enter) 可执行"},
    {L"non_universal_shortcut_guard", L"default",
        L"应用专属组合键(Ctrl+H)默认劝退；通用键放行；Skill 不再教 F12/Ctrl+H"},
    {L"allow_nested_ai_action_under_cap", L"default",
        L"未达上限时允许 submit 含 aiActionExecute"},
    {L"reject_nested_ai_action_at_cap", L"default",
        L"嵌套达上限时 submit aiActionExecute 被拒"},
    {L"reject_absolute_pointer_no_image", L"default",
        L"无图模式绝对坐标点击被拒"},
    {L"allow_absolute_pointer_with_image_opt", L"default",
        L"允许绝对坐标时 mouseClick 可构建"},
    {L"reject_empty_quick_input", L"default",
        L"空 inputText 的 quickInput 被拒"},
    {L"reject_bare_key_click", L"default",
        L"无 keyText/keyVk 的 keyClick 被拒（防误变 7）"},
    {L"reply_actions_buildable", L"default",
        L"quickInput+keyClick(Enter)+stopMacro 可构建"},
    {L"reply_enter_vk_return", L"default",
        L"keyClick Enter 解析为 VK_RETURN(13)"},
    {L"observe_markers", L"default",
        L"OBSERVE / SKIP_OBSERVE / TASK_COMPLETE 标记识别"},
    {L"force_observe_submit_block", L"default",
        L"灰钮强制观察；提交不可用时拦 Enter"},
    {L"history_complete_and_submit", L"default",
        L"HistoryHasCompleteTask / Submit / Observe"},
    {L"hybrid_prompt_has_agent", L"default",
        L"Hybrid/ToolExecute system 含硬规则且 Skill 不进请求体"},
    {L"unknown_action_type_rejected", L"default",
        L"未知动作 type 在 Validate 阶段返回 [错误]"},
    {L"lookup_skills_no_catalog_dump", L"default",
        L"section=usage/agent 返回 Skill 全文但不附带目录"},
    {L"parse_coord_paren", L"default",
        L"TryParseCoordinatePair (12,34)"},
    {L"parse_bbox_brackets", L"default",
        L"TryParseBoundingBox [10,20,100,80]"},
    {L"zoom_roi_around_point", L"default",
        L"BuildZoomRoiAroundScreenPoint clamps to bounds"},
    {L"adaptive_refine_gate", L"default",
        L"紧凑粗框/宽控件可跳过二级；过大/过小图标不跳过"},
    {L"parse_color_hex", L"default",
        L"TryParseColorSpec #FF8800"},
    {L"build_find_color_action", L"default",
        L"findColor JSON 可构建"},
    {L"locate_and_click_tool_present", L"default",
        L"locateAndClick/openWebpage 工具注册且必填字段正确"},
    {L"vision_not_found_and_box_filter", L"default",
        L"NOT_FOUND 识别与过大粗框拒绝"},
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
    {L"resolve_vision_rect_norm1000", L"default",
        L"越界矩形按 0~1000 映射到 api 图"},
    {L"resolve_vision_ambiguous_in_image_as_norm1000", L"default",
        L"图内亦像 0~1000 的框优先归一化（防搜索钮偏移）"},
    {L"resolve_vision_point_src_pixels", L"default",
        L"超大坐标按截屏原图像素换算"},
    {L"resolve_vision_rect_reject_oob", L"default",
        L"无法解释的越界矩形拒绝"},
    {L"resolve_agent_pointer_in_image_as_pixels", L"default",
        L"Agent 指针：图内坐标按上传像素（勿误当 0~1000）"},
    {L"resolve_agent_pointer_oob_as_norm1000", L"default",
        L"Agent 指针：越界且≤1000 按 0~1000 归一化"},
    {L"resolve_agent_pointer_reject_oob", L"default",
        L"Agent 指针：无法解释的飞点拒绝"},
    {L"refine_prompt_coord_only", L"default",
        L"精炼 prompt 仅含尺寸与 (x,y) 契约"},
    {L"api_point_outside_reject", L"default",
        L"越界 api 点判定（防精炼幻觉）"},
    {L"refine_drift_too_far", L"default",
        L"精炼相对上级漂移过大则回退"},
    {L"route_label_zh", L"default",
        L"AiActionRouteLabel returns non-empty Chinese"},
    {L"route_scroll_wheel_tool", L"default",
        L"滚动/滑动/滚轮 -> ToolExecute（scrollWheel 工具）"},
    {L"route_move_mouse_verb", L"default",
        L"移动鼠标 -> ToolExecute（勿当识图问答）"},
    {L"route_switch_window_tool", L"default",
        L"切换窗口 -> ToolExecute（本地 activateWindow）"},
    {L"route_save_rename_tool", L"default",
        L"保存/重命名/新建/删除 -> ToolExecute"},
    {L"route_mobile_device_not_move", L"default",
        L"「移动端设备」不触发移动鼠标动作词"},
    {L"need_screen_scroll_yes", L"default",
        L"滚动/滑动/移动鼠标需要首帧截图"},
    {L"click_intent_double_right", L"default",
        L"双击/右键意图检测（CompositeClick 用）"},
    {L"composite_target_phrase_stripped", L"default",
        L"ExtractClickTargetPhrase 剥动作前缀并截断到逗号"},
    {L"parse_bbox_leading_number", L"default",
        L"「第2个按钮在 […]」优先取方括号组"},
    {L"parse_bbox_parens", L"default",
        L"TryParseBoundingBox (…) 圆括号组"},
    {L"vision_not_found_variants", L"default",
        L"未检测到/没有找到/没看到/NOT FOUND 均判定未找到"},
    {L"logic_convert_compile_gate", L"default",
        L"逻辑转化编译：每找图 timer+限时findImage+if/else→AI + 步间Wait"},
    {L"logic_convert_per_locate_timers", L"default",
        L"逻辑转化：多个找图各自 timer+if 分支（禁止整段共用一个计时器）"},
    {L"logic_convert_assistant_gate", L"default",
        L"助手硬闸：未授权则剥离 aiLogicConvert"},
    {L"logic_convert_writeback_guards", L"default",
        L"逻辑转化：失败收尾不写回 + CJK 块名哈希去撞车"},
    {L"logic_convert_session_recording", L"default",
        L"逻辑转化：会话轨迹录制内容/顺序/结束"},
    {L"logic_convert_compile_wait_filter", L"default",
        L"逻辑转化：编译过滤 AI 一次性长等待（防 timer 门闩被撑爆）"},
    {L"logic_convert_compile_no_anchor", L"default",
        L"逻辑转化：无定位锚编译出不带 findImage 门闩的 if/else 骨架"},
    {L"logic_convert_collapse_instance_data", L"default",
        L"逻辑转化：多条实例 quickInput 折叠为文字识别+抽象AI填写（勿写死浏览记录）"},
    {L"logic_convert_ocr_list_activate_meta", L"default",
        L"OCR 备注带 listActivate；动态填表步数收紧"},
    {L"logic_convert_collapse_skip_filename", L"default",
        L"逻辑转化：短文件名不误折叠为动态数据"},
    {L"locate_fail_key_block", L"default",
        L"locate 失败后 F12/hotkey 硬拦，Enter 放行"},
    {L"locate_retry_hard_cap", L"default",
        L"locate 连败计数：成功清零，供超限硬拦"},
    {L"fill_table_only_tools", L"default",
        L"动态填表工具白名单去掉 runProgram/网页/hotkey"},
    {L"list_ocr_heuristic", L"default",
        L"列表 OCR 启发：多行列表通过、另存为拒"},
    {L"logic_convert_writeback_first_time", L"default",
        L"逻辑转化：首次写回=块置顶+源动作替换为执行指令块"},
    {L"logic_convert_heal_create_missing", L"default",
        L"逻辑转化：heal/指定块名不存在时创建块（勿放弃写回）"},
    {L"logic_convert_writeback_promote", L"default",
        L"逻辑转化：promote=整块替换 Define 体（模板更新+新步骤）"},
    {L"resolve_action_vision_fallback", L"default",
        L"ResolveActionAiModelName：指定文本模型+需识图 → 自动换识图模型"},
    {L"resolve_vision_subtask_model", L"default",
        L"ResolveVisionSubtaskModelName：非多模态主模型 → savedModels 识图模型"},
    {L"vision_route_model_routed", L"default",
        L"带图视觉路由：文本主模型判定非多模态并解析出识图模型"},
    {L"model_supports_vision", L"default",
        L"ModelSupportsVision 对主流多模态/文本模型的判定"},
    {L"planner_observe_image_attach", L"default",
        L"纯文本规划模型不附观察截图；多模态才附"},
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

void CaseCompositeNotFooledByInputBoxNoun() {
    // 「输入框」是名词；勿因「输入」子串误判多轮 Agent（真实失败：嘴炮无工具）
    Emit(L"route_click_search_input_box_composite",
        ClassifyAiActionRoute(L"点击搜索图标，搜索输入框里面的内容", true)
            == AiActionRouteKind::CompositeClick, L"");
    Emit(L"route_click_input_box_composite",
        ClassifyAiActionRoute(L"点击输入框", true) == AiActionRouteKind::CompositeClick, L"");
}

void CaseCorrectLocatePrompt() {
    const std::wstring p = BuildCompositeCorrectLocatePrompt(
        L"点击搜索按钮", 677, 132, 717, 188, 1280, 720);
    Emit(L"correct_locate_prompt_has_prev_box",
        p.find(L"677") != std::wstring::npos && p.find(L"重新框选") != std::wstring::npos,
        p.size() > 20 ? L"ok" : L"empty");
}

void CaseNeedScreenCaptureHeuristic() {
    Emit(L"need_screen_click_yes",
        AiActionPromptLikelyNeedsScreenCapture(L"点击屏幕中的搜索按钮"), L"");
    Emit(L"need_screen_var_key_no",
        !AiActionPromptLikelyNeedsScreenCapture(L"分析变量 count，若大于0则按 Enter"), L"");
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

void CaseLookScreenVision() {
    Emit(L"route_look_screen_vision",
        ClassifyAiActionRoute(L"看一下屏幕", true) == AiActionRouteKind::VisionQuery, L"");
}

void CaseReplyMessageTool() {
    const std::wstring p =
        L"回答对方发送的最后一条消息（如果最后一条消息不是对方发的，你什么都不用做）";
    Emit(L"route_reply_message_tool",
        ClassifyAiActionRoute(p, true) == AiActionRouteKind::ToolExecute, L"");
}

void CaseReplySendNotMultiTurn() {
    const std::wstring p = L"回答对方消息并发送";
    Emit(L"route_reply_send_not_multiturn",
        ClassifyAiActionRoute(p, true) == AiActionRouteKind::ToolExecute, L"");
}

void CaseAmbiguousDefaultTool() {
    Emit(L"route_ambiguous_default_tool",
        ClassifyAiActionRoute(L"处理当前窗口", true) == AiActionRouteKind::ToolExecute, L"");
}

void CaseUsageSkill() {
    const std::wstring skill = LookupMacroActionSchema(L"usage");
    const std::wstring usage = MacroActionUsageSkill();
    const bool ok = skill.find(L"quickInput") != std::wstring::npos
        && skill.find(L"Enter") != std::wstring::npos
        && skill.find(L"locateAndClick") != std::wstring::npos
        && skill.find(L"openWebpage") != std::wstring::npos
        && usage.find(L"openWebpage") != std::wstring::npos
        && usage.find(L"saveTaskData") != std::wstring::npos
        && usage.find(L"runActionRecipe") != std::wstring::npos
        && usage.find(L"另存为") != std::wstring::npos
        && usage.find(L"clearFirst") != std::wstring::npos
        && usage.find(L"activateWindow") != std::wstring::npos
        && usage.find(L"resolveSystemPath") != std::wstring::npos
        && usage.find(L"mouseDrag") != std::wstring::npos
        && usage.find(L"doubleClick=true") != std::wstring::npos
        && usage.find(L"locateAndClick") != std::wstring::npos
        && usage.size() < 3200;
    Emit(L"usage_skill_reply_chain", ok,
        ok ? L"" : L"usage skill missing core openWebpage/saveTaskData/recipe rules");
}

void CaseAgentSkill() {
    const std::wstring skill = LookupMacroActionSchema(L"agent");
    const bool ok = skill.find(L"completeTask") != std::wstring::npos
        && skill.find(L"quickInput") != std::wstring::npos
        && skill.find(L"locateAndClick") != std::wstring::npos
        && skill.find(L"activateWindow") != std::wstring::npos
        && skill.find(L"resolveSystemPath") != std::wstring::npos
        && skill.find(L"runActionRecipe") != std::wstring::npos
        && skill.find(L"scrollWheel") != std::wstring::npos
        && skill.find(L"mouseDrag") != std::wstring::npos
        && skill.find(L"clearFirst") != std::wstring::npos
        && skill.size() < 1600;
    Emit(L"agent_skill_observe_loop", ok,
        ok ? L"" : L"agent skill missing core tools or too long");
}

std::wstring CallSubmitTool(const std::vector<AgentTool>& tools, const std::wstring& argsJson) {
    for (const auto& t : tools) {
        if (t.name == L"submitMacroActions" && t.execute)
            return t.execute(argsJson);
    }
    return L"[错误] submitMacroActions missing";
}

std::wstring CallNamedTool(const std::vector<AgentTool>& tools,
    const std::wstring& name, const std::wstring& argsJson) {
    for (const auto& t : tools) {
        if (t.name == name && t.execute)
            return t.execute(argsJson);
    }
    return L"[错误] tool missing: " + name;
}

void CaseAtomicToolsPresent() {
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    bool hasQi = false, hasKc = false, hasMc = false, hasFi = false, hasSubmit = false;
    bool hasScroll = false, hasMemo = false, hasReadMemo = false;
    bool hasRun = false, hasSearch = false, hasHotkey = false, hasSwitch = false;
    bool hasDrag = false;
    for (const auto& t : tools) {
        if (t.name == L"quickInput") {
            hasQi = t.parameters_json.find(L"inputText") != std::wstring::npos
                && t.parameters_json.find(L"required") != std::wstring::npos;
        }
        if (t.name == L"keyClick") {
            hasKc = t.parameters_json.find(L"keyText") != std::wstring::npos;
        }
        if (t.name == L"mouseClick") hasMc = true;
        if (t.name == L"scrollWheel") hasScroll = true;
        if (t.name == L"mouseDrag"
            && t.parameters_json.find(L"fromX") != std::wstring::npos
            && t.parameters_json.find(L"toX") != std::wstring::npos) {
            hasDrag = true;
        }
        if (t.name == L"runProgram") hasRun = true;
        if (t.name == L"openAppViaSearch"
            && t.parameters_json.find(L"query") != std::wstring::npos) {
            hasSearch = true;
        }
        if (t.name == L"hotkeyShortcut"
            && t.parameters_json.find(L"maximum") != std::wstring::npos
            && t.parameters_json.find(L"11") != std::wstring::npos) {
            hasHotkey = true;
        }
        if (t.name == L"switchWindow"
            && t.parameters_json.find(L"openPreview") != std::wstring::npos
            && t.parameters_json.find(L"confirm") != std::wstring::npos) {
            hasSwitch = true;
        }
        if (t.name == L"updateTaskMemo") hasMemo = true;
        if (t.name == L"readTaskMemo") hasReadMemo = true;
        if (t.name == L"findImage") {
            hasFi = t.parameters_json.find(L"imagePath") != std::wstring::npos
                && t.parameters_json.find(L"imageUseVar") != std::wstring::npos;
        }
        if (t.name == L"submitMacroActions") hasSubmit = true;
    }
    const std::wstring banAltTab = CallNamedTool(tools, L"hotkeyShortcut",
        L"{\"shortcutPreset\":9}");
    const bool banOk = banAltTab.find(L"[错误]") != std::wstring::npos
        && banAltTab.find(L"activateWindow") != std::wstring::npos;
    const bool ok = hasQi && hasKc && hasMc && hasFi && hasSubmit
        && hasScroll && hasDrag && hasMemo && hasReadMemo && hasRun && hasSearch && hasHotkey && hasSwitch
        && banOk
        && IsMacroActionRunToolName(L"quickInput")
        && IsMacroActionRunToolName(L"findImage")
        && IsMacroActionRunToolName(L"scrollWheel")
        && IsMacroActionRunToolName(L"mouseDrag")
        && IsMacroActionRunToolName(L"runProgram")
        && IsMacroActionRunToolName(L"openAppViaSearch")
        && IsMacroActionRunToolName(L"switchWindow")
        && IsMacroActionRunToolName(L"activateWindow")
        && IsMacroActionRunToolName(L"runActionRecipe")
        && IsMacroExecutionToolName(L"listWindows")
        && IsMacroExecutionToolName(L"resolveSystemPath")
        && ShortcutPresetCount() >= 12
        && kAiObsImageVarName != nullptr
        && kAiObsUnchangedMatchThreshold >= 80.0;
    Emit(L"atomic_tools_present", ok,
        ok ? L"" : L"missing typed tools/switchWindow or hotkey9 ban");
}

void CaseResolveSystemPathAndSavePathGuard() {
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring desktop = CallNamedTool(tools, L"resolveSystemPath",
        L"{\"folder\":\"desktop\",\"fileName\":\"history.xlsx\"}");
    // 真实桌面路径必须是绝对路径且带上传入的文件名
    const bool resolveOk = desktop.find(L":\\") != std::wstring::npos
        && desktop.find(L".xlsx") != std::wstring::npos
        && desktop.find(L"[错误]") == std::wstring::npos;
    const std::wstring bad = CallNamedTool(tools, L"resolveSystemPath",
        L"{\"folder\":\"nosuch\"}");
    const std::wstring sep = CallNamedTool(tools, L"resolveSystemPath",
        L"{\"fileName\":\"a\\\\b.xlsx\"}");
    const bool rejectOk = bad.find(L"[错误]") != std::wstring::npos
        && sep.find(L"[错误]") != std::wstring::npos;

    // 猜的绝对路径（父目录不存在）必须被 quickInput 拒绝并指向 resolveSystemPath
    const std::wstring guessed = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"C:\\\\Users\\\\NoSuchUser_QstTest\\\\Desktop\\\\a.xlsx\"}");
    const bool guardOk = guessed.find(L"[错误]") != std::wstring::npos
        && guessed.find(L"resolveSystemPath") != std::wstring::npos;
    // 普通文本不受影响
    const std::wstring plain = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"hello\"}");
    const bool plainOk = plain.find(L"resolveSystemPath") == std::wstring::npos;

    const bool ok = resolveOk && rejectOk && guardOk && plainOk;
    Emit(L"resolve_system_path_and_save_guard", ok,
        ok ? L"" : L"resolveSystemPath or guessed-path guard failed");
}

void CaseSaveDialogSubmitGuard() {
    ResetAiActionSessionState(515151);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring resolved = CallNamedTool(tools, L"resolveSystemPath",
        L"{\"folder\":\"desktop\",\"fileName\":\"guard.xlsx\"}");
    const size_t at = resolved.find(L"=");
    std::wstring dir = resolved.substr(at + 1);
    const size_t semi = dir.find(L'\uFF1B');
    if (semi != std::wstring::npos) dir = dir.substr(0, semi);
    std::wstring json = L"{\"inputText\":\"";
    for (wchar_t c : dir + L"\\guard.xlsx") {
        if (c == L'\\') json += L"\\\\";
        else json += c;
    }
    json += L"\"}";

    // 输入真实完整路径 → 应放行并记 pending（随后换目录被拦）
    const std::wstring typed = CallNamedTool(tools, L"quickInput", json);
    const bool hintOk = typed.find(L"[错误]") == std::wstring::npos;
    // 此时在原地点「这台电脑」换目录应被拒
    const std::wstring detour = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\\u5de6\\u4fa7\\u7684\\u8fd9\\u53f0\\u7535\\u8111\"}");
    const bool detourBlocked = detour.find(L"\u4e0d\u8981\u518d\u70b9") != std::wstring::npos;
    // 「浏览 / 更多选项」是逃出迷你保存框的正路，不能拦
    const std::wstring escape1 = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\\u66f4\\u591a\\u9009\\u9879\"}");
    const std::wstring escape2 = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\\u6d4f\\u89c8\"}");
    const bool escapeAllowed = escape1.find(L"\u4e0d\u8981\u518d\u70b9") == std::wstring::npos
        && escape2.find(L"\u4e0d\u8981\u518d\u70b9") == std::wstring::npos;
    // Enter 提交后限制解除：此时点「这台电脑」不该再被拦
    CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"Enter\"}");
    const std::wstring after = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\\u5de6\\u4fa7\\u7684\\u8fd9\\u53f0\\u7535\\u8111\"}");
    const bool clearedOk = after.find(L"\u4e0d\u8981\u518d\u70b9") == std::wstring::npos;
    // 点「保存」也算提交：重新输入路径后点保存，随后换目录不再被拦
    CallNamedTool(tools, L"quickInput", json);
    const std::wstring submit = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\\u4fdd\\u5b58\\u6309\\u94ae\"}");
    const std::wstring afterSubmit = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\\u5de6\\u4fa7\\u7684\\u8fd9\\u53f0\\u7535\\u8111\"}");
    const bool submitAllowed = submit.find(L"\u4e0d\u8981\u518d\u70b9") == std::wstring::npos
        && afterSubmit.find(L"\u4e0d\u8981\u518d\u70b9") == std::wstring::npos;

    const bool ok = hintOk && detourBlocked && escapeAllowed && submitAllowed && clearedOk;
    Emit(L"save_dialog_submit_guard", ok,
        ok ? L"" : L"save-path submit guard failed");
}

void CaseSwitchWindowAfterLaunchWarned() {
    ResetAiActionSessionState(626262);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    // 无 hooks 时 runProgram 仍返回 [EXECUTED]，会记下启动时刻
    const std::wstring run = CallNamedTool(tools, L"runProgram",
        L"{\"targetPath\":\"excel\"}");
    const std::wstring first = CallNamedTool(tools, L"switchWindow",
        L"{\"action\":\"openPreview\"}");
    const bool warnedOk = first.find(L"[错误]") != std::wstring::npos
        && first.find(L"\u81ea\u52a8\u5230\u524d\u53f0") != std::wstring::npos;
    // 第二次不再劝（无 hooks 时落到缺钩子错误，说明已放行）
    const std::wstring second = CallNamedTool(tools, L"switchWindow",
        L"{\"action\":\"openPreview\"}");
    const bool passOk = second.find(L"\u81ea\u52a8\u5230\u524d\u53f0") == std::wstring::npos;
    const bool ok = run.find(L"[EXECUTED]") != std::wstring::npos && warnedOk && passOk;
    Emit(L"switch_window_after_launch_warned", ok,
        ok ? L"" : L"switchWindow-after-launch nudge failed");
}

void CaseWindowLedgerTools() {
    ResetAiActionSessionState(828282);
    std::wstring activateQuery;
    AiActionHostHooks hooks;
    hooks.onListWindows = []() -> std::wstring {
        return L"#1 工作簿1 - Excel [EXCEL.EXE]（前台）\n#2 历史记录 - Edge [msedge.exe]";
    };
    hooks.onActivateWindow = [&](const std::wstring& query) -> std::wstring {
        activateQuery = query;
        return L"已切到前台：工作簿1 - Excel [EXCEL.EXE]";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});

    const std::wstring list = CallNamedTool(tools, L"listWindows", L"{}");
    const bool listOk = list.find(L"EXCEL.EXE") != std::wstring::npos
        && list.find(L"[错误]") == std::wstring::npos;

    const std::wstring act = CallNamedTool(tools, L"activateWindow",
        L"{\"match\":\"excel\"}");
    const bool actOk = activateQuery == L"excel"
        && act.find(L"[EXECUTED]") != std::wstring::npos
        && act.find(L"[SKIP_OBSERVE]") != std::wstring::npos
        && act.find(L"\u5df2\u5207\u5230\u524d\u53f0") != std::wstring::npos;
    const std::wstring actObs = CallNamedTool(tools, L"activateWindow",
        L"{\"match\":\"\u6d4f\u89c8\u8bb0\u5f55\",\"observeAfter\":true}");
    const bool actObsOk = actObs.find(L"[OBSERVE]") != std::wstring::npos
        && actObs.find(L"[SKIP_OBSERVE]") == std::wstring::npos;
    const bool emptyRejected = CallNamedTool(tools, L"activateWindow",
        L"{\"match\":\"  \"}").find(L"[错误]") != std::wstring::npos;

    // 宿主多候选拒绝应原样回传 [错误]
    hooks.onActivateWindow = [](const std::wstring& q) -> std::wstring {
        return L"[错误] activateWindow「" + q + L"」匹配到 2 个窗口，拒绝自动选择以免切错。";
    };
    const auto tools2 = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring ambig = CallNamedTool(tools2, L"activateWindow",
        L"{\"match\":\"edge\"}");
    const bool ambigOk = ambig.find(L"[错误]") != std::wstring::npos
        && ambig.find(L"\u62d2\u7edd") != std::wstring::npos; // 拒绝

    // Alt+Tab 预览应被挡下并附上台账，逼模型改走本地激活
    const std::wstring preview = CallNamedTool(tools, L"switchWindow",
        L"{\"action\":\"openPreview\"}");
    const bool previewBlocked = preview.find(L"[错误]") != std::wstring::npos
        && preview.find(L"activateWindow") != std::wstring::npos
        && preview.find(L"msedge.exe") != std::wstring::npos;

    const bool ok = listOk && actOk && actObsOk && emptyRejected && ambigOk && previewBlocked;
    Emit(L"window_ledger_tools", ok, ok ? L"" : L"listWindows/activateWindow wiring failed");
}

void CaseLocateFailSuggestsActivate() {
    ResetAiActionSessionState(838383);
    AiActionHostHooks hooks;
    hooks.onListWindows = []() -> std::wstring {
        return L"#1 历史记录 - Edge [msedge.exe]";
    };
    hooks.onActivateWindow = [](const std::wstring&) -> std::wstring { return L"已切到前台：x"; };
    hooks.onLocateAndClick = [](const std::wstring&, int, const std::wstring&, int) {
        return std::wstring(L"[错误] 未找到目标（识图返回 NOT_FOUND）。");
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    // 想点任务栏切窗却没找到 → 必须提示改用本地激活，而不是继续识图
    const std::wstring taskbar = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"任务栏上的Excel图标\"}");
    const bool nudged = taskbar.find(L"activateWindow") != std::wstring::npos
        && taskbar.find(L"msedge.exe") != std::wstring::npos;
    // 普通目标找不到时不该塞窗口台账（白费 token）
    const std::wstring plain = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"空白工作簿\"}");
    const bool quiet = plain.find(L"msedge.exe") == std::wstring::npos;
    const bool ok = nudged && quiet;
    Emit(L"locate_fail_suggests_activate", ok,
        ok ? L"" : L"NOT_FOUND ledger nudge failed");
}

void CaseRunActionRecipe() {
    ResetAiActionSessionState(848484);
    SetAiActionPlanGateEnabled(false);
    std::wstring executedJson;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [&](const std::wstring& actionsJson) -> std::wstring {
        executedJson = actionsJson;
        return L"ok-recipe";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    // rows>=3 => first call is a forced 2-group preview (observe), not the full batch
    const std::wstring preview = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":["
        L"{\"type\":\"quickInput\",\"inputText\":\"{0}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Tab\"},"
        L"{\"type\":\"quickInput\",\"inputText\":\"{1}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Enter\"}"
        L"],\"rows\":[[\"1\",\"alpha\"],[\"2\",\"beta\"],[\"3\",\"gamma\"]],"
        L"\"observeAfter\":false}");
    const bool previewMode = preview.find(L"[EXECUTED]") != std::wstring::npos
        && executedJson.find(L"alpha") != std::wstring::npos
        && executedJson.find(L"beta") != std::wstring::npos
        && executedJson.find(L"gamma") == std::wstring::npos;
    executedJson.clear();
    // same args again but without verifiedGroups => rejected (must confirm preview first)
    const std::wstring unverified = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":["
        L"{\"type\":\"quickInput\",\"inputText\":\"{0}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Tab\"},"
        L"{\"type\":\"quickInput\",\"inputText\":\"{1}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Enter\"}"
        L"],\"rows\":[[\"1\",\"alpha\"],[\"2\",\"beta\"],[\"3\",\"gamma\"]],"
        L"\"observeAfter\":false}");
    const bool unverifiedRejected = unverified.find(L"[错误]") != std::wstring::npos
        && unverified.find(L"verifiedGroups") != std::wstring::npos;
    // same args + verifiedGroups=2 => skip the 2 previewed groups, run the remaining rows only
    const std::wstring fill = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":["
        L"{\"type\":\"quickInput\",\"inputText\":\"{0}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Tab\"},"
        L"{\"type\":\"quickInput\",\"inputText\":\"{1}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Enter\"}"
        L"],\"rows\":[[\"1\",\"alpha\"],[\"2\",\"beta\"],[\"3\",\"gamma\"]],"
        L"\"observeAfter\":false,\"verifiedGroups\":2}");
    const bool fillMode = fill.find(L"[EXECUTED]") != std::wstring::npos
        && executedJson.find(L"gamma") != std::wstring::npos
        && executedJson.find(L"alpha") == std::wstring::npos;
    executedJson.clear();
    // 模板信任后：只传剩余新行也应一次全跑、不再试跑（禁止每 2 行重检）
    const std::wstring trustedMore = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":["
        L"{\"type\":\"quickInput\",\"inputText\":\"{0}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Tab\"},"
        L"{\"type\":\"quickInput\",\"inputText\":\"{1}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Enter\"}"
        L"],\"rows\":[[\"4\",\"delta\"],[\"5\",\"epsilon\"],[\"6\",\"zeta\"]],"
        L"\"observeAfter\":false}");
    const bool trustedReuse = trustedMore.find(L"[EXECUTED]") != std::wstring::npos
        && trustedMore.find(L"模板已信任") != std::wstring::npos
        && executedJson.find(L"delta") != std::wstring::npos
        && executedJson.find(L"epsilon") != std::wstring::npos
        && executedJson.find(L"zeta") != std::wstring::npos;
    executedJson.clear();
    // rows=2 => forced 1-group preview (only first row runs), then verifiedGroups=1 fills rest
    ResetAiActionSessionState(848485);
    SetAiActionPlanGateEnabled(false);
    const std::wstring twoRows = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":[{\"type\":\"quickInput\",\"inputText\":\"{0}\"}],"
        L"\"rows\":[[\"a\"],[\"b\"]],\"observeAfter\":false}");
    const bool twoRowsPreview = twoRows.find(L"[EXECUTED]") != std::wstring::npos
        && executedJson.find(L"inputText\":\"a") != std::wstring::npos
        && executedJson.find(L"inputText\":\"b") == std::wstring::npos;
    executedJson.clear();
    const std::wstring twoRowsFill = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":[{\"type\":\"quickInput\",\"inputText\":\"{0}\"}],"
        L"\"rows\":[[\"a\"],[\"b\"]],\"observeAfter\":false,\"verifiedGroups\":1}");
    const bool twoRowsOk = twoRowsFill.find(L"[EXECUTED]") != std::wstring::npos
        && executedJson.find(L"inputText\":\"b") != std::wstring::npos
        && executedJson.find(L"inputText\":\"a") == std::wstring::npos;
    // header/static text inside the reusable template must be rejected hard
    const std::wstring fixedText = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":[{\"type\":\"quickInput\",\"inputText\":\"序号\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Tab\"},"
        L"{\"type\":\"quickInput\",\"inputText\":\"{0}\"}],"
        L"\"rows\":[[\"1\"],[\"2\"]],\"observeAfter\":false}");
    const bool fixedTextRejected = fixedText.find(L"[错误]") != std::wstring::npos
        && fixedText.find(L"固定文本") != std::wstring::npos
        && executedJson.find(L"序号") == std::wstring::npos;
    executedJson.clear();
    // coordinate-only steps must be rejected (no blind reuse)
    const std::wstring bad = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":[{\"type\":\"mouseClick\",\"x\":1,\"y\":2}],\"rows\":[[\"a\"]]}");
    const bool rejected = bad.find(L"[\u9519\u8bef]") != std::wstring::npos
        && bad.find(L"locateAndClick") != std::wstring::npos;
    // empty rows must be rejected
    const std::wstring emptyRows = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":[{\"type\":\"keyClick\",\"keyText\":\"Enter\"}],\"rows\":[]}");
    const bool emptyRejected = emptyRows.find(L"[\u9519\u8bef]") != std::wstring::npos;
    // empty cell / missing placeholder column must be rejected before execute (prevents blind Tab)
    ResetAiActionSessionState(848485);
    SetAiActionPlanGateEnabled(false);
    const std::wstring emptyCell = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":["
        L"{\"type\":\"quickInput\",\"inputText\":\"{0}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Tab\"},"
        L"{\"type\":\"quickInput\",\"inputText\":\"{1}\"}"
        L"],\"rows\":[[\"1\",\"\"],[\"2\",\"ok\"]],\"observeAfter\":false}");
    const bool emptyCellRejected = emptyCell.find(L"[错误]") != std::wstring::npos
        && emptyCell.find(L"空") != std::wstring::npos
        && executedJson.empty();
    const bool ok = previewMode && unverifiedRejected && fillMode && trustedReuse
        && twoRowsPreview && twoRowsOk && fixedTextRejected && rejected && emptyRejected
        && emptyCellRejected;
    std::wstring detail;
    if (!previewMode) detail = L"preview";
    else if (!unverifiedRejected) detail = L"unverified";
    else if (!fillMode) detail = L"fill";
    else if (!trustedReuse) detail = L"trustedReuse";
    else if (!twoRowsPreview) detail = L"twoRowsPreview";
    else if (!twoRowsOk) detail = L"twoRowsFill";
    else if (!fixedTextRejected) detail = L"fixedText";
    else if (!rejected) detail = L"rejectMouse";
    else if (!emptyRejected) detail = L"emptyRows";
    else if (!emptyCellRejected) detail = L"emptyCell";
    Emit(L"run_action_recipe", ok, detail.c_str());
}

void CaseHotkeyNameAndBrowserReuse() {
    ResetAiActionSessionState(919191);
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) -> std::wstring { return L"已执行 1 步"; };
    hooks.onListWindows = []() -> std::wstring {
        return L"#1 历史记录 - Edge [msedge.exe]（前台）\n#2 工作簿 - Excel [EXCEL.EXE]";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});

    // name=save → preset 3，勿与 find 混淆
    const std::wstring save = CallNamedTool(tools, L"hotkeyShortcut",
        L"{\"name\":\"save\",\"observeAfter\":false}");
    const bool saveOk = save.find(L"[错误]") == std::wstring::npos
        && save.find(L"[EXECUTED]") != std::wstring::npos;
    const std::wstring badName = CallNamedTool(tools, L"hotkeyShortcut",
        L"{\"name\":\"notAKey\"}");
    const bool badNameRejected = badName.find(L"[错误]") != std::wstring::npos;

    // 已开 Edge：runProgram(edge) 必须拒绝并指向 activateWindow
    const std::wstring edge = CallNamedTool(tools, L"runProgram",
        L"{\"targetPath\":\"edge\"}");
    const bool edgeReuse = edge.find(L"[错误]") != std::wstring::npos
        && edge.find(L"activateWindow") != std::wstring::npos
        && edge.find(L"msedge") != std::wstring::npos;

    // excel 不在浏览器复用规则里（允许新开）；无真实启动环境时只要不误套浏览器拒绝即可
    // 这里 hooks 有 Excel 窗口但 IsBrowserLaunchTarget(excel)=false，应走到执行路径
    const std::wstring excel = CallNamedTool(tools, L"runProgram",
        L"{\"targetPath\":\"excel\"}");
    const bool excelNotBlockedAsBrowser = excel.find(L"浏览器进程") == std::wstring::npos;

    const bool ok = saveOk && badNameRejected && edgeReuse && excelNotBlockedAsBrowser;
    Emit(L"hotkey_name_and_browser_reuse", ok,
        ok ? L"" : L"save/edge-reuse guard failed");
}

void CaseOpenAppViaSearchFallback() {
    ResetAiActionSessionState(929292);
    // 无 hooks：合成 Win+S→输入→Enter，不得真启动本机程序
    {
        const auto tools = BuildAiActionExecuteTools(nullptr, {});
        const std::wstring r = CallNamedTool(tools, L"openAppViaSearch",
            L"{\"query\":\"edge\",\"observeAfter\":false}");
        const bool ok = r.find(L"[EXECUTED]") != std::wstring::npos
            && r.find(L"Microsoft Edge") != std::wstring::npos
            && r.find(L"[错误]") == std::wstring::npos;
        Emit(L"open_app_via_search_compose", ok,
            ok ? L"" : L"search compose/normalize failed");
    }
    // 台账已有 Edge：默认拒绝搜索新开；forceNew 才真正下发键盘序列
    {
        int execCount = 0;
        std::wstring lastExec;
        AiActionHostHooks hooks;
        hooks.onExecuteActions = [&](const std::wstring& j) -> std::wstring {
            ++execCount;
            lastExec = j;
            return L"已执行 5 步";
        };
        hooks.onListWindows = []() -> std::wstring {
            return L"#1 历史记录 - Edge [msedge.exe]（前台）";
        };
        const auto tools = BuildAiActionExecuteTools(&hooks, {});
        const std::wstring blocked = CallNamedTool(tools, L"openAppViaSearch",
            L"{\"query\":\"Microsoft Edge\"}");
        const bool reuseOk = blocked.find(L"[错误]") != std::wstring::npos
            && blocked.find(L"activateWindow") != std::wstring::npos
            && execCount == 0;
        const std::wstring forced = CallNamedTool(tools, L"openAppViaSearch",
            L"{\"query\":\"edge\",\"forceNew\":true,\"observeAfter\":false}");
        const bool forceOk = forced.find(L"[EXECUTED]") != std::wstring::npos
            && execCount == 1
            && lastExec.find(L"holdLeftWin") != std::wstring::npos
            && lastExec.find(L"Microsoft Edge") != std::wstring::npos;
        Emit(L"open_app_via_search_reuse", reuseOk && forceOk,
            (reuseOk && forceOk) ? L"" : L"search reuse/forceNew failed");
    }
    // runProgram 找不到裸名 → 指向 openAppViaSearch
    {
        const auto tools = BuildAiActionExecuteTools(nullptr, {});
        const std::wstring miss = CallNamedTool(tools, L"runProgram",
            L"{\"targetPath\":\"DefinitelyNotAnInstalledAppXYZ123\"}");
        const bool hintOk = miss.find(L"[错误]") != std::wstring::npos
            && miss.find(L"openAppViaSearch") != std::wstring::npos;
        Emit(L"run_program_points_to_search", hintOk,
            hintOk ? L"" : L"missing openAppViaSearch hint");
    }
    // prompt 阶梯写明搜索兜底
    {
        const std::wstring usage = LookupMacroActionSchema(L"usage");
        const std::wstring hybrid = BuildAiActionHybridSystemPrompt(100, 80);
        const std::wstring skill = MacroActionAgentSkill();
        const bool ok = usage.find(L"openAppViaSearch") != std::wstring::npos
            && (hybrid.find(L"openAppViaSearch") != std::wstring::npos
                || skill.find(L"openAppViaSearch") != std::wstring::npos);
        Emit(L"open_app_via_search_in_prompts", ok,
            ok ? L"" : L"prompts missing openAppViaSearch");
    }
}

void CaseSaveDialogGuards() {
    ResetAiActionSessionState(939393);
    std::wstring dialogProbe;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) -> std::wstring { return L"已执行 2 步"; };
    hooks.onProbeForegroundDialog = [&]() { return dialogProbe; };
    hooks.onLocateAndClick = [](const std::wstring&, int, const std::wstring&, int) {
        return L"locateAndClick 已点击屏幕(1,1)";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});

    const std::wstring resolved = CallNamedTool(tools, L"resolveSystemPath",
        L"{\"folder\":\"desktop\",\"fileName\":\"浏览记录.xlsx\"}");
    const bool resolvedOk = resolved.find(L"完整路径=") != std::wstring::npos;

    dialogProbe = L"kind=saveAs;title=另存为";
    const std::wstring bare = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"浏览记录.xlsx\"}");
    const bool bareAllowed = bare.find(L"[错误]") == std::wstring::npos;

    // 目录路径：放行但提示勿当文件名提交
    const std::wstring deskOnly = CallNamedTool(tools, L"resolveSystemPath",
        L"{\"folder\":\"desktop\"}");
    std::wstring dirJson = L"{\"inputText\":\"";
    {
        // 格式：desktop=C:\Users\...\Desktop
        const size_t eq = deskOnly.find(L'=');
        std::wstring path = eq == std::wstring::npos ? L"" : deskOnly.substr(eq + 1);
        const size_t nl = path.find_first_of(L"\r\n（★；");
        if (nl != std::wstring::npos) path = path.substr(0, nl);
        for (wchar_t c : path) {
            if (c == L'\\') dirJson += L"\\\\";
            else dirJson += c;
        }
    }
    dirJson += L"\"}";
    const std::wstring dirTyped = CallNamedTool(tools, L"quickInput", dirJson);
    const bool dirTipOk = dirTyped.find(L"[错误]") == std::wstring::npos
        && dirTyped.find(L"目录") != std::wstring::npos;

    // 完整文件路径：放行；已 pending 时拦「此电脑」绕路；滚轮应放行
    std::wstring full = L"{\"inputText\":\"";
    {
        const size_t at = resolved.find(L"完整路径=");
        std::wstring path = at == std::wstring::npos ? L"" : resolved.substr(at + 5);
        const size_t nl = path.find_first_of(L"\r\n（★");
        if (nl != std::wstring::npos) path = path.substr(0, nl);
        for (wchar_t c : path) {
            if (c == L'\\') full += L"\\\\";
            else full += c;
        }
    }
    full += L"\"}";
    const std::wstring typed = CallNamedTool(tools, L"quickInput", full);
    const bool fullPathOk = typed.find(L"[错误]") == std::wstring::npos;
    const std::wstring scroll = CallNamedTool(tools, L"scrollWheel",
        L"{\"scrollDirection\":0,\"scrollSteps\":5}");
    const bool scrollOk = scroll.find(L"[错误]") == std::wstring::npos;
    const std::wstring detour = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"另存为左侧此电脑\"}");
    const bool detourBlocked = detour.find(L"[错误]") != std::wstring::npos;
    const std::wstring deskClick = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"另存为左侧桌面\"}");
    const bool deskOk = deskClick.find(L"[错误]") == std::wstring::npos;

    // 假路径拒绝
    const std::wstring fake = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"C:\\\\Users\\\\NoSuchUserXYZ\\\\Desktop\\\\a.xlsx\"}");
    const bool fakeBlocked = fake.find(L"[错误]") != std::wstring::npos;

    // Escape：先劝一次，再按放行
    dialogProbe = L"kind=confirmOverwrite;title=确认另存为;buttons=是(Y)|否(N);msg=已存在";
    const std::wstring esc1 = CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"Escape\"}");
    const bool escBlocked = esc1.find(L"[错误]") != std::wstring::npos
        && esc1.find(L"覆盖") != std::wstring::npos;
    const std::wstring esc2 = CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"Escape\"}");
    const bool escInsist = esc2.find(L"[错误]") == std::wstring::npos;

    ResetAiActionSessionState(939394);
    dialogProbe.clear();
    const std::wstring escPlain = CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"Escape\"}");
    const bool escPlainOk = escPlain.find(L"[错误]") == std::wstring::npos;

    // 迷你保存框：不教招拦截；目录可输入（靠灰钮/观察纠偏）；F4 可试
    ResetAiActionSessionState(939395);
    dialogProbe = L"kind=saveAsMini;title=保存此文件;buttons=更多选项|保存|取消";
    const std::wstring miniDir = CallNamedTool(tools, L"quickInput", dirJson);
    const bool miniDirOk = miniDir.find(L"[错误]") == std::wstring::npos
        && miniDir.find(L"更多选项") == std::wstring::npos;
    const std::wstring miniF4 = CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"F4\"}");
    const bool miniF4Ok = miniF4.find(L"[错误]") == std::wstring::npos;
    const std::wstring miniName = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"浏览记录\",\"clearFirst\":true}");
    const bool miniNameOk = miniName.find(L"[错误]") == std::wstring::npos;

    const bool ok = resolvedOk && bareAllowed && dirTipOk && fullPathOk && scrollOk
        && detourBlocked && deskOk && fakeBlocked && escBlocked && escInsist && escPlainOk
        && miniDirOk && miniF4Ok && miniNameOk;
    const std::wstring detail = ok ? std::wstring()
        : (L"resolved=" + std::to_wstring(resolvedOk)
            + L" bare=" + std::to_wstring(bareAllowed)
            + L" dir=" + std::to_wstring(dirTipOk)
            + L" full=" + std::to_wstring(fullPathOk)
            + L" scroll=" + std::to_wstring(scrollOk)
            + L" detour=" + std::to_wstring(detourBlocked)
            + L" desk=" + std::to_wstring(deskOk)
            + L" fake=" + std::to_wstring(fakeBlocked)
            + L" esc1=" + std::to_wstring(escBlocked)
            + L" esc2=" + std::to_wstring(escInsist)
            + L" miniDir=" + std::to_wstring(miniDirOk)
            + L" miniF4=" + std::to_wstring(miniF4Ok)
            + L" miniName=" + std::to_wstring(miniNameOk)
            + L" |" + dirTyped + L"|" + detour + L"|" + miniDir);
    Emit(L"save_dialog_guards", ok, detail.c_str());
}

void CaseHideWindowsGuard() {
    ResetAiActionSessionState(727272);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    // 第一次 Win+D 放行
    const std::wstring first = CallNamedTool(tools, L"hotkeyShortcut",
        L"{\"shortcutPreset\":6}");
    const bool firstOk = first.find(L"[错误]") == std::wstring::npos;
    // 紧接着再来一次（含 keyClick Win+D 绕道）都要被拒，并指向唤窗正路
    const std::wstring again = CallNamedTool(tools, L"hotkeyShortcut",
        L"{\"shortcutPreset\":6}");
    const std::wstring viaKey = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"d\",\"holdLeftWin\":true}");
    auto blocked = [](const std::wstring& s) {
        return s.find(L"[错误]") != std::wstring::npos
            && s.find(L"activateWindow") != std::wstring::npos
            && s.find(L"listWindows") != std::wstring::npos;
    };
    const bool ok = firstOk && blocked(again) && blocked(viaKey);
    Emit(L"hide_windows_guard", ok, ok ? L"" : L"Win+D repeat guard failed");
}

void CaseLocateDoubleClick() {
    std::wstring gotButton;
    int gotClicks = 0;
    AiActionHostHooks hooks;
    hooks.onLocateAndClick = [&](const std::wstring&, int,
        const std::wstring& button, int clickCount) -> std::wstring {
        gotButton = button;
        gotClicks = clickCount;
        return L"locateAndClick 已双击屏幕(10,10)";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"desktop icon\",\"doubleClick\":true}");
    const bool dblOk = gotClicks == 2 && gotButton == L"left";
    CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"desktop icon\",\"button\":\"right\"}");
    const bool rightOk = gotClicks == 1 && gotButton == L"right";
    // 点击动作 JSON 必须带上 clickCount，宿主才会真的双击
    const std::wstring dblJson = BuildScreenClickActionsJson(120, 240, false, L"left", 2);
    const bool jsonOk = dblJson.find(L"\"clickCount\":2") != std::wstring::npos
        && dblJson.find(L"\"button\":\"left\"") != std::wstring::npos;
    const std::wstring rightJson = BuildScreenClickActionsJson(120, 240, false, L"right", 1);
    const bool rightJsonOk = rightJson.find(L"\"button\":\"right\"") != std::wstring::npos
        && rightJson.find(L"\"clickCount\":1") != std::wstring::npos;
    const bool ok = dblOk && rightOk && jsonOk && rightJsonOk;
    Emit(L"locate_double_click", ok, ok ? L"" : L"locateAndClick double/right click failed");
}

void CaseTaskMemoAndOpenDedup() {
    ResetAiActionSessionState(424242);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring w1 = CallNamedTool(tools, L"updateTaskMemo",
        L"{\"note\":\"\u6284\u5386\u53f2\u5230Excel\",\"section\":\"goal\"}");
    const std::wstring w2 = CallNamedTool(tools, L"updateTaskMemo",
        L"{\"note\":\"Ctrl+H\u4fa7\u680f\u5df2\u670910\u6761\",\"section\":\"facts\"}");
    const std::wstring r1 = CallNamedTool(tools, L"readTaskMemo", L"{}");
    const std::wstring open1 = CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://example.com/test-dedup\"}");
    const std::wstring open2 = CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://example.com/test-dedup\"}");
    const bool memoOk = w1.find(L"备忘已更新") != std::wstring::npos
        && w2.find(L"[facts]") != std::wstring::npos
        && r1.find(L"[goal]") != std::wstring::npos
        && r1.find(L"[facts]") != std::wstring::npos
        && r1.find(L"Excel") != std::wstring::npos;
    const bool dedupOk = open1.find(L"[EXECUTED]") != std::wstring::npos
        && open2.find(L"跳过重复") != std::wstring::npos;
    const bool budgetOk = AiToolResultBudgetChars(L"listWindows") > AiToolResultBudgetChars(L"lookupMacroAction")
        && AiToolResultBudgetChars(L"resolveSystemPath") < 800;
    Emit(L"task_memo_and_open_dedup", memoOk && dedupOk && budgetOk,
        (memoOk && dedupOk && budgetOk) ? L"" : L"memo/sections/budget or openWebpage dedup failed");
}

void CaseTaskDataClearedOnReset() {
    ResetAiActionSessionState(424250);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring saved = CallNamedTool(tools, L"saveTaskData",
        L"{\"name\":\"historyRecords\",\"content\":\"row1\\nrow2\",\"replace\":true}");
    const std::wstring before = CallNamedTool(tools, L"readTaskData",
        L"{\"name\":\"historyRecords\"}");
    ResetAiActionSessionState(424250);  // 同 runId 再开一轮须清空
    const std::wstring after = CallNamedTool(tools, L"readTaskData",
        L"{\"name\":\"historyRecords\"}");
    const std::wstring allAfter = CallNamedTool(tools, L"readTaskData", L"{}");
    const bool ok = saved.find(L"已缓存") != std::wstring::npos
        && before.find(L"row1") != std::wstring::npos
        && after.find(L"[错误]") != std::wstring::npos
        && (allAfter.find(L"为空") != std::wstring::npos || allAfter.find(L"row1") == std::wstring::npos);
    Emit(L"task_data_cleared_on_reset", ok,
        ok ? L"" : (saved + L" | " + before + L" | " + after + L" | " + allAfter).c_str());
}

void CaseObserveResultDefaults() {
    AiObserveCaptureResult r;
    const bool ok = !r.ok && !r.unchanged && r.base64.empty()
        && r.matchScore < 0
        && std::wstring(kAiObsImageVarName) == L"aiObs";
    Emit(L"observe_result_defaults", ok, ok ? L"" : L"AiObserveCaptureResult / aiObs constants");
}

void CaseWaitRequestsObserve() {
    // 等待就是为了看新界面：wait 必须要求观察；短 wait 默认被拦，长 wait 或 confirmWait 才放行
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring blocked = CallNamedTool(tools, L"wait", L"{\"duration\":0.01}");
    const bool shortBlocked = blocked.find(L"[错误]") != std::wstring::npos;
    const std::wstring r = CallNamedTool(tools, L"wait",
        L"{\"duration\":0.01,\"confirmWait\":true}");
    const bool ok = shortBlocked && IsSubmitObserveToolResult(r)
        && !IsSubmitSkipObserveToolResult(r);
    Emit(L"wait_requests_observe", ok, ok ? L"" : (blocked + L" | " + r).c_str());
}

void CaseQuickInputClearPolicy() {
    // Excel 另存为：前台虽是表格进程，仍须 Ctrl+A 替换默认文件名（否则追加）
    const bool saveAsExcel = AiQuickInputNeedsCtrlAClear(true, true, L"saveAs");
    const bool openFileExcel = AiQuickInputNeedsCtrlAClear(true, true, L"openFile");
    // 表格格子：跳过 Ctrl+A（避免全选整表）
    const bool sheetCell = !AiQuickInputNeedsCtrlAClear(true, true, L"");
    // 普通软件：替换
    const bool normal = AiQuickInputNeedsCtrlAClear(true, false, L"");
    // 追加意图
    const bool append = !AiQuickInputNeedsCtrlAClear(false, false, L"saveAs");
    const bool ok = saveAsExcel && openFileExcel && sheetCell && normal && append;
    Emit(L"quick_input_clear_policy", ok,
        ok ? L"" : L"Ctrl+A policy for saveAs/sheet/append wrong");
}

void CaseAtomicQuickInputEmpty() {
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring r = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"\"}");
    const bool ok = r.rfind(L"[错误]", 0) == 0
        && r.find(L"inputText") != std::wstring::npos;
    Emit(L"atomic_quick_input_rejects_empty", ok, r.c_str());
}

void CaseAtomicKeyClickEnter() {
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring r = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"Enter\",\"observeAfter\":false}");
    const bool ok = r.find(L"[EXECUTED]") != std::wstring::npos
        && r.find(L"keyClick") != std::wstring::npos
        && r.rfind(L"[错误]", 0) != 0;
    Emit(L"atomic_key_click_enter", ok, r.c_str());
}

void CaseNonUniversalShortcutGuard() {
    ResetAiActionSessionState(878001);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    // 应用专属组合键（如 Ctrl+H）默认劝退：不执行、指引视觉点击、说明 confirmShortcut
    const std::wstring ctrlH = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"h\",\"holdLeftCtrl\":true}");
    const bool blocked = ctrlH.find(L"[提示]") != std::wstring::npos
        && ctrlH.find(L"locateAndClick") != std::wstring::npos
        && ctrlH.find(L"confirmShortcut") != std::wstring::npos;
    // 加 confirmShortcut=true 才放行执行
    const std::wstring ctrlHOk = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"h\",\"holdLeftCtrl\":true,\"confirmShortcut\":true}");
    const bool confirmed = ctrlHOk.find(L"[EXECUTED]") != std::wstring::npos;
    // 通用组合键（Ctrl+A）无需 confirm 直接放行
    const std::wstring ctrlA = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"a\",\"holdLeftCtrl\":true}");
    const bool universal = ctrlA.find(L"[EXECUTED]") != std::wstring::npos;
    // Skill 不再教 F12 / 裸 Ctrl+H（应用专属）；允许出现 Ctrl+Home（表格回 A1，通用）
    auto hasBareCtrlH = [](const std::wstring& s) {
        for (size_t p = 0; (p = s.find(L"Ctrl+H", p)) != std::wstring::npos; ) {
            if (p + 9 <= s.size() && s.compare(p, 9, L"Ctrl+Home") == 0) {
                p += 9;
                continue;
            }
            return true;
        }
        return false;
    };
    const bool skillClean = MacroActionUsageSkill().find(L"F12") == std::wstring::npos
        && !hasBareCtrlH(MacroActionUsageSkill())
        && MacroActionAgentSkill().find(L"F12") == std::wstring::npos
        && !hasBareCtrlH(MacroActionAgentSkill());
    const bool ok = blocked && confirmed && universal && skillClean;
    Emit(L"non_universal_shortcut_guard", ok,
        ok ? L"" : L"shortcut guard / skill F12 Ctrl+H cleanup failed");
}

void CaseAllowNestedUnderCap() {
    // 未占满深度时应允许提交嵌套类型（不应被「上限/禁止嵌套」拦住）
    const bool can = AiActionExecuteCanNestMore() && AiActionExecuteNestDepth() == 0;
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring r = CallSubmitTool(tools,
        L"{\"actions\":[{\"type\":\"aiActionExecute\",\"aiPrompt\":\"子任务\"}],\"observeAfter\":false}");
    const bool blockedByNestPolicy = r.find(L"上限") != std::wstring::npos
        || r.find(L"禁止在 submitMacroActions 中嵌套") != std::wstring::npos;
    const bool ok = can && !blockedByNestPolicy;
    Emit(L"allow_nested_ai_action_under_cap", ok, r.c_str());
}

void CaseRejectNestedAtCap() {
    // 顶层+1 层占满后，再 submit 嵌套应被拒
    AiActionExecuteNestGuard g0;
    AiActionExecuteNestGuard g1;
    const bool guardsOk = g0.entered() && g1.entered() && !AiActionExecuteCanNestMore();
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring r = CallSubmitTool(tools,
        L"{\"actions\":[{\"type\":\"aiActionExecute\",\"aiPrompt\":\"x\"}]}");
    const bool ok = guardsOk && r.rfind(L"[错误]", 0) == 0
        && r.find(L"上限") != std::wstring::npos;
    Emit(L"reject_nested_ai_action_at_cap", ok, r.c_str());
}

void CasePlanGateRequiresMemo() {
    ResetAiActionSessionState(424242);
    SetAiActionPlanGateEnabled(true);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring blocked = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"hello\",\"observeAfter\":false}");
    const bool blockedOk = blocked.find(L"[错误]") != std::wstring::npos
        && blocked.find(L"updateTaskMemo") != std::wstring::npos;
    const std::wstring memo = CallNamedTool(tools, L"updateTaskMemo",
        L"{\"section\":\"goal\",\"note\":\"\u6d4b\u8bd5\u76ee\u6807\"}");
    const bool memoOk = memo.find(L"[错误]") == std::wstring::npos
        && AiActionPlanGateIsOpen();
    const std::wstring allowed = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"hello\",\"observeAfter\":false}");
    const bool allowedOk = allowed.find(L"[EXECUTED]") != std::wstring::npos;
    // listWindows 只读，无计划也应放行
    ResetAiActionSessionState(424243);
    SetAiActionPlanGateEnabled(true);
    const auto tools2 = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring list = CallNamedTool(tools2, L"listWindows", L"{}");
    const bool listOk = list.find(L"updateTaskMemo") == std::wstring::npos;
    SetAiActionPlanGateEnabled(false);
    const bool ok = blockedOk && memoOk && allowedOk && listOk;
    Emit(L"plan_gate_requires_memo", ok,
        ok ? L"" : L"plan gate blocked/unlock failed");
}

void CaseLocateTargetLengthAndOutsourceReject() {
    ResetAiActionSessionState(434343);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    std::wstring longTarget(kMaxLocateAndClickTargetChars + 5, L'x');
    // JSON string escape
    std::string args = "{\"target\":\"";
    args.append(static_cast<size_t>(kMaxLocateAndClickTargetChars + 5), 'x');
    args += "\"}";
    const std::wstring longR = CallNamedTool(tools, L"locateAndClick",
        std::wstring(args.begin(), args.end()));
    const bool longOk = longR.find(L"[错误]") != std::wstring::npos
        && longR.find(L"过长") != std::wstring::npos;

    const bool looksLocate = LooksLikeLocateOnlyOutsourcePrompt(L"点击确定按钮");
    const bool looksMulti = !LooksLikeLocateOnlyOutsourcePrompt(
        L"点击确定然后输入姓名并保存");
    const std::wstring nested = CallSubmitTool(tools,
        L"{\"actions\":[{\"type\":\"aiActionExecute\",\"aiPrompt\":\"\u70b9\u51fb\u786e\u5b9a\u6309\u94ae\"}]}");
    const bool nestedOk = nested.find(L"[错误]") != std::wstring::npos
        && nested.find(L"locateAndClick") != std::wstring::npos;
    const bool ok = longOk && looksLocate && looksMulti && nestedOk;
    Emit(L"locate_target_and_outsource_reject", ok,
        ok ? L"" : L"locate harden failed");
}

void CaseHistorySidebarAndBusyGuards() {
    ResetAiActionSessionState(454545);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    // 应用专有快捷键已从 Prompt/工具移除：browserHistory 不再是合法 name，应被拒并指引通用菜单导航
    const std::wstring hist = CallNamedTool(tools, L"hotkeyShortcut",
        L"{\"name\":\"browserHistory\",\"observeAfter\":false}");
    const bool histRejected = hist.find(L"[错误]") != std::wstring::npos
        && hist.find(L"locateAndClick") != std::wstring::npos;
    // 工具描述不再兜售浏览器历史快捷键，避免污染 Prompt
    bool descClean = false;
    for (const auto& t : tools) {
        if (t.name == L"hotkeyShortcut") {
            descClean = t.description.find(L"browserHistory") == std::wstring::npos;
            break;
        }
    }

    ResetAiActionSessionState(454546);
    NoteAiActionUiBusy(0.20, true);
    const bool busy = AiActionUiTooBusyForVisionLocate();
    const bool skipLa = AiActionShouldSkipLookahead();
    const auto tools2 = BuildAiActionExecuteTools(nullptr, {});
    // 计划门闩关：直接测 busy locate
    SetAiActionPlanGateEnabled(false);
    const std::wstring loc = CallNamedTool(tools2, L"locateAndClick",
        L"{\"target\":\"\u67e5\u770b\u66f4\u591a\"}");
    const bool locBlocked = loc.find(L"[错误]") != std::wstring::npos
        && loc.find(L"动态") != std::wstring::npos;

    const bool ok = histRejected && descClean && busy && skipLa && locBlocked;
    Emit(L"history_sidebar_and_busy_guards", ok,
        ok ? L"" : L"history/busy host guards failed");
}

void CaseLogicConvertCompileGate() {
    AiLogicConvertCompileInput in;
    in.taskPrompt = L"open notepad";
    in.preferredBlockName = L"LogicTestGate";
    AiLogicTranscriptEntry e1;
    e1.action.type = ActionType::HotkeyShortcut;
    e1.action.shortcutPreset = 6;
    in.transcript.push_back(e1);
    AiLogicTranscriptEntry e2;
    e2.fromLocate = true;
    e2.locateTarget = L"start";
    e2.templatePath = L"images\\logic_gate_dummy.bmp";
    e2.action.button = MouseButtonType::Left;
    e2.action.clickCount = 1;
    in.transcript.push_back(e2);
    AiLogicTranscriptEntry e3;
    e3.action.type = ActionType::QuickInput;
    e3.action.inputText = L"hello";
    in.transcript.push_back(e3);

    const auto compiled = CompileAiLogicConvert(in);
    bool hasTimer = false, hasIf = false, hasElse = false, hasAi = false, hasFind = false;
    bool hasFindTime = false, hasGapWait = false, findThrOk = true;
    for (const auto& a : compiled.defineAndBody) {
        if (a.type == ActionType::TimerRecordTime) hasTimer = true;
        if (a.type == ActionType::If) hasIf = true;
        if (a.type == ActionType::Else) hasElse = true;
        if (a.type == ActionType::FindImage) {
            hasFind = true;
            if (!a.findTimeExpr.empty() && a.findTimeExpr != L"0") hasFindTime = true;
            if (a.matchThreshold < 84.5) findThrOk = false;
        }
        if (a.type == ActionType::Wait && a.remark.find(L"步间") != std::wstring::npos)
            hasGapWait = true;
        if (a.type == ActionType::AiActionExecute && a.aiLogicConvert) hasAi = true;
    }
    const bool runOk = compiled.runBlock.type == ActionType::RunBlock
        && compiled.runBlock.blockName == L"LogicTestGate";
    std::wstring detail;
    if (!compiled.ok) detail = compiled.error.empty() ? L"compile not ok" : compiled.error;
    else if (!hasTimer) detail = L"missing timer";
    else if (!hasFind) detail = L"missing findImage";
    else if (!hasFindTime) detail = L"findImage missing findTimeExpr";
    else if (!findThrOk) detail = L"findImage threshold < 85";
    else if (!hasIf) detail = L"missing if";
    else if (!hasElse) detail = L"missing else";
    else if (!hasAi) detail = L"missing fallback ai";
    else if (!hasGapWait) detail = L"missing inter-step wait";
    else if (!runOk) detail = L"bad runBlock";
    const bool ok = compiled.ok && hasTimer && hasIf && hasElse && hasAi && hasFind
        && hasFindTime && hasGapWait && runOk && findThrOk;
    Emit(L"logic_convert_compile_gate", ok, detail.c_str());
}

void CaseLogicConvertPerLocateTimers() {
    AiLogicConvertCompileInput in;
    in.taskPrompt = L"multi locate";
    in.preferredBlockName = L"LogicMultiLoc";
    AiLogicTranscriptEntry e1;
    e1.fromLocate = true;
    e1.locateTarget = L"btnA";
    e1.templatePath = L"images\\logic_a.bmp";
    e1.action.button = MouseButtonType::Left;
    e1.action.clickCount = 1;
    e1.settleSec = 2.0; // → findTime 约 8
    in.transcript.push_back(e1);
    AiLogicTranscriptEntry mid;
    mid.action.type = ActionType::KeyClick;
    mid.action.keyText = L"Enter";
    mid.action.keyVk = 13;
    in.transcript.push_back(mid);
    AiLogicTranscriptEntry e2;
    e2.fromLocate = true;
    e2.locateTarget = L"btnB";
    e2.templatePath = L"images\\logic_b.bmp";
    e2.action.button = MouseButtonType::Left;
    e2.action.clickCount = 1;
    e2.settleSec = 5.0; // → findTime 约 20
    in.transcript.push_back(e2);
    AiLogicTranscriptEntry tail;
    tail.action.type = ActionType::QuickInput;
    tail.action.inputText = L"x";
    in.transcript.push_back(tail);

    const auto c = CompileAiLogicConvert(in);
    int timers = 0, ifs = 0, elses = 0, finds = 0, ais = 0;
    std::wstring t1, t2;
    bool fbHasAnchor = false, fbHasDone = false, fbHasNext = false;
    for (const auto& a : c.defineAndBody) {
        if (a.type == ActionType::TimerRecordTime) ++timers;
        if (a.type == ActionType::If) ++ifs;
        if (a.type == ActionType::Else) ++elses;
        if (a.type == ActionType::FindImage) {
            ++finds;
            if (t1.empty()) t1 = a.findTimeExpr;
            else if (t2.empty()) t2 = a.findTimeExpr;
        }
        if (a.type == ActionType::AiActionExecute && a.aiLogicConvert) {
            ++ais;
            if (a.aiPrompt.find(L"失败锚点") != std::wstring::npos) fbHasAnchor = true;
            if (a.aiPrompt.find(L"已完成") != std::wstring::npos) fbHasDone = true;
            if (a.aiPrompt.find(L"下一步") != std::wstring::npos) fbHasNext = true;
        }
    }
    // 两个找图 → 两个 timer / if / else / fallback AI；时限随 settle 不同；回退含进度
    const bool ok = c.ok && timers == 2 && ifs == 2 && elses == 2 && finds == 2 && ais == 2
        && t1 != t2 && !t1.empty() && !t2.empty()
        && fbHasAnchor && fbHasDone && fbHasNext;
    std::wstring detail;
    if (!c.ok) detail = c.error;
    else if (timers != 2) detail = L"timers=" + std::to_wstring(timers);
    else if (ifs != 2) detail = L"ifs=" + std::to_wstring(ifs);
    else if (t1 == t2) detail = L"same findTimeExpr";
    else if (!fbHasAnchor) detail = L"fallback missing 失败锚点";
    else if (!fbHasDone) detail = L"fallback missing 已完成";
    else if (!fbHasNext) detail = L"fallback missing 下一步";
    Emit(L"logic_convert_per_locate_timers", ok, detail.c_str());
}

void CaseLogicConvertAssistantGate() {
    SetAgentToolUserContext(L"write a notepad macro");
    const bool noLogic = !UserExplicitlyRequestsLogicConvert(GetAgentToolUserContext());
    const bool noAi = !UserExplicitlyRequestsAiActionExecute(GetAgentToolUserContext());
    std::vector<ScriptAction> acts(1);
    acts[0].type = ActionType::AiActionExecute;
    acts[0].aiPrompt = L"open notepad";
    acts[0].aiLogicConvert = true;
    acts[0].aiLogicBlockName = L"Logic_X";
    const int stripped = StripUnauthorizedLogicConvert(acts);
    const bool strippedOk = stripped == 1 && !acts[0].aiLogicConvert
        && acts[0].aiLogicBlockName.empty();

    // "请用逻辑转化做自愈脚本" — 解锁逻辑转化；单独「逻辑转化」不再误等同「AI动作执行」词
    SetAgentToolUserContext(L"\u8bf7\u7528\u903b\u8f91\u8f6c\u5316\u505a\u81ea\u6108\u811a\u672c");
    const bool allowLogic = UserExplicitlyRequestsLogicConvert(GetAgentToolUserContext());
    const bool allowAiPhrase = UserExplicitlyRequestsAiActionExecute(GetAgentToolUserContext());
    acts[0].aiLogicConvert = true;
    acts[0].aiLogicBlockName = L"Logic_Y";
    const int keep = StripUnauthorizedLogicConvert(acts);
    const bool keepOk = keep == 0 && acts[0].aiLogicConvert;
    // 助手工具侧：逻辑转化话术仍可生成 aiActionExecute（与 build 硬闸一致）
    const bool scaffoldOk = allowLogic && !allowAiPhrase;

    const bool ok = noLogic && noAi && strippedOk && allowLogic && keepOk && scaffoldOk;
    Emit(L"logic_convert_assistant_gate", ok,
        ok ? L"" : L"assistant logic-convert gate failed");
    SetAgentToolUserContext(L"");
}

void CaseLogicConvertWritebackGuards() {
    const bool failReason = AiLogicConvertLooksLikeFailedComplete(L"未找到目标按钮")
        && AiLogicConvertLooksLikeFailedComplete(L"任务失败：卡点")
        && !AiLogicConvertLooksLikeFailedComplete(L"已导出到 Excel");

    AiLogicConvertSessionBegin(true, L"", L"\u6d4f\u89c8\u8bb0\u5f55\u5bfcExcel", L"", 0, false);
    const bool emptyNoWrite = !AiLogicConvertShouldWriteback(
        true, true, true, L"已完成", L"[agent-complete]");
    AiLogicConvertNoteWindowActivate(L"Edge");
    const bool okWrite = AiLogicConvertShouldWriteback(
        true, true, true, L"已导出到 Excel", L"[agent-complete]");
    const bool noWriteFail = !AiLogicConvertShouldWriteback(
        true, true, true, L"未找到历史侧栏", L"[agent-complete]");
    const bool noWriteNoExec = !AiLogicConvertShouldWriteback(
        true, true, false, L"已完成", L"[agent-complete]");
    AiLogicConvertSessionEnd();

    const std::wstring n1 = MakeAiLogicBlockName(L"", L"\u6d4f\u89c8\u8bb0\u5f55\u5bfcExcel");
    const std::wstring n2 = MakeAiLogicBlockName(L"", L"\u6253\u5f00\u8bb0\u4e8b\u672c");
    const bool cjkDistinct = n1 != n2 && n1 != L"LogicTask" && n2 != L"LogicTask"
        && n1.rfind(L"Logic", 0) == 0 && n2.rfind(L"Logic", 0) == 0;

    AiLogicConvertCompileInput in;
    in.taskPrompt = L"\u6d4f\u89c8\u8bb0\u5f55";
    AiLogicTranscriptEntry act;
    act.fromActivate = true;
    act.activateMatch = L"Edge";
    in.transcript.push_back(act);
    AiLogicTranscriptEntry e2;
    e2.action.type = ActionType::HotkeyShortcut;
    e2.action.shortcutPreset = 11;
    in.transcript.push_back(e2);
    const auto compiled = CompileAiLogicConvert(in);
    bool hasActivate = false;
    for (const auto& a : compiled.defineAndBody) {
        if (a.type == ActionType::ActivateWindow
            && a.remark.find(L"activateWindow:") != std::wstring::npos) {
            hasActivate = true;
            break;
        }
        // 兼容旧 Wait 备注形态
        if (a.type == ActionType::Wait
            && a.remark.find(L"activateWindow:") != std::wstring::npos) {
            hasActivate = true;
            break;
        }
    }

    const bool ok = failReason && emptyNoWrite && okWrite && noWriteFail && noWriteNoExec
        && cjkDistinct && compiled.ok && hasActivate;
    std::wstring detail;
    if (!failReason) detail = L"fail reason detect";
    else if (!emptyNoWrite) detail = L"empty transcript should skip";
    else if (!okWrite) detail = L"valid writeback rejected";
    else if (!noWriteFail) detail = L"failed complete should skip";
    else if (!noWriteNoExec) detail = L"no exec should skip";
    else if (!cjkDistinct) detail = L"cjk block names collide: " + n1 + L" / " + n2;
    else if (!compiled.ok) detail = compiled.error;
    else if (!hasActivate) detail = L"activate not compiled";
    Emit(L"logic_convert_writeback_guards", ok, detail.c_str());
}

namespace {

std::wstring MakeLogicTestScriptPath(const wchar_t* name) {
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring dir = std::wstring(tempDir) + L"qst_logic_wb";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + name;
}

}  // namespace

void CaseLogicConvertSessionRecording() {
    AiLogicConvertSessionBegin(true, L"LogicSess", L"open notepad", L"", 3, false);
    const bool active = AiLogicConvertSessionActive();

    ScriptAction k;
    k.type = ActionType::HotkeyShortcut;
    k.shortcutPreset = 6;
    AiLogicConvertNoteAction(k);
    AiLogicConvertNoteWindowActivate(L"  记事本  ");
    AiLogicConvertNoteLocate(L"打开", 100, 200, L"left", 1, L"images\\t1.bmp");
    ScriptAction mv;
    mv.type = ActionType::MoveMouse;  // 不可转化类型不应记录
    AiLogicConvertNoteAction(mv);

    bool ok = active;
    std::wstring detail;
    const auto& tr = AiLogicConvertTranscript();
    if (tr.size() != 3) {
        ok = false;
        detail = L"transcript size=" + std::to_wstring(tr.size());
    } else {
        ok = tr[0].action.type == ActionType::HotkeyShortcut
            && tr[1].fromActivate && tr[1].activateMatch == L"记事本"
            && tr[1].action.type == ActionType::ActivateWindow
            && tr[1].action.targetPath == L"记事本"
            && tr[1].action.remark.find(L"activateWindow:") != std::wstring::npos
            && tr[2].fromLocate && tr[2].templatePath == L"images\\t1.bmp"
            && tr[2].locateTarget == L"打开";
        if (!ok) detail = L"transcript order/content mismatch";
    }
    AiLogicConvertSessionEnd();
    if (ok && AiLogicConvertSessionActive()) { ok = false; detail = L"session not ended"; }
    if (ok && !AiLogicConvertTranscript().empty()) { ok = false; detail = L"transcript not cleared"; }
    Emit(L"logic_convert_session_recording", ok, detail.c_str());
}

void CaseLogicConvertCompileWaitFilter() {
    AiLogicConvertCompileInput in;
    in.taskPrompt = L"test";
    AiLogicTranscriptEntry w1;
    w1.action.type = ActionType::Wait;
    w1.action.duration = 0.5;
    AiLogicTranscriptEntry w2;
    w2.action.type = ActionType::Wait;
    w2.action.duration = 5.0;  // AI 一次性长等待应被过滤
    AiLogicTranscriptEntry k;
    k.action.type = ActionType::HotkeyShortcut;
    k.action.shortcutPreset = 3;
    in.transcript.push_back(w1);
    in.transcript.push_back(w2);
    in.transcript.push_back(k);
    const auto c = CompileAiLogicConvert(in);
    int waits = 0;
    bool hasLongWait = false;
    bool hasTranscriptWait = false;
    for (const auto& a : c.defineAndBody) {
        if (a.type != ActionType::Wait) continue;
        ++waits;
        if (a.duration > 2.0) hasLongWait = true;
        if (std::fabs(a.duration - 0.5) < 1e-6) hasTranscriptWait = true;
    }
    // 5s 长等待过滤掉；0.5 保留；另有步间短 Wait
    const bool ok = c.ok && !hasLongWait && hasTranscriptWait && waits >= 2;
    Emit(L"logic_convert_compile_wait_filter", ok,
        (ok ? L"" : (c.ok ? L"wait filter failed (waits=" + std::to_wstring(waits) : c.error)).c_str());
}

void CaseLogicConvertCompileNoAnchor() {
    AiLogicConvertCompileInput in;
    in.taskPrompt = L"test";
    AiLogicTranscriptEntry k1;
    k1.action.type = ActionType::KeyClick;
    k1.action.keyText = L"Enter";
    k1.action.keyVk = 13;
    AiLogicTranscriptEntry k2;
    k2.action.type = ActionType::HotkeyShortcut;
    k2.action.shortcutPreset = 6;
    in.transcript.push_back(k1);
    in.transcript.push_back(k2);
    const auto c = CompileAiLogicConvert(in);
    bool hasGate = false, hasIf = false, hasElse = false, hasAi = false;
    for (const auto& a : c.defineAndBody) {
        if (a.type == ActionType::FindImage) hasGate = true;
        if (a.type == ActionType::If) hasIf = true;
        if (a.type == ActionType::Else) hasElse = true;
        if (a.type == ActionType::AiActionExecute) hasAi = true;
    }
    // 无定位锚：不应生成 findImage 门闩，但仍要有 if/else→AI 兜底骨架
    const bool ok = c.ok && !hasGate && hasIf && hasElse && hasAi;
    Emit(L"logic_convert_compile_no_anchor", ok,
        (ok ? L"" : (c.ok ? L"unexpected gate/skeleton" : c.error)).c_str());
}

void CaseLogicConvertCollapseInstanceData() {
    AiLogicConvertCompileInput in;
    in.taskPrompt = L"把最近浏览记录填入Excel";
    in.preferredBlockName = L"LogicHist";
    auto pushQi = [&](const std::wstring& text) {
        AiLogicTranscriptEntry e;
        e.action.type = ActionType::QuickInput;
        e.action.inputText = text;
        in.transcript.push_back(e);
    };
    auto pushKey = [&](const std::wstring& key) {
        AiLogicTranscriptEntry e;
        e.action.type = ActionType::KeyClick;
        e.action.keyText = key;
        in.transcript.push_back(e);
    };
    auto pushActivate = [&](const std::wstring& match) {
        AiLogicTranscriptEntry e;
        e.fromActivate = true;
        e.activateMatch = match;
        e.action.type = ActionType::Wait;
        e.action.duration = 0.2;
        e.action.remark = L"activateWindow:" + match;
        in.transcript.push_back(e);
    };
    // 列表窗 → 再切回 Excel → 表头 → 实例行（OCR 应插在切 Excel 之前）
    pushActivate(L"Edge 历史");
    pushActivate(L"浏览记录.xlsx - Excel");
    pushQi(L"序号");
    pushKey(L"Tab");
    pushQi(L"标题");
    pushKey(L"Tab");
    pushQi(L"时间");
    pushKey(L"Enter");
    pushKey(L"Home");
    // 4+ 条实例数据 → 应折叠，禁止写死具体标题
    for (int i = 1; i <= 4; ++i) {
        pushQi(L"条目" + std::to_wstring(i));
        pushKey(L"Tab");
        pushQi(L"火山方舟标题" + std::to_wstring(i));
        pushKey(L"Tab");
        pushQi(L"23:15");
        pushKey(L"Enter");
        pushKey(L"Home");
    }
    const auto c = CompileAiLogicConvert(in);
    bool hasOcr = false, hasDynAi = false, hasFrozenTitle = false, hasHeader = false;
    bool ocrBeforeExcelActivate = false;
    bool listActivateBeforeOcr = false;
    bool dynFillOnlyTable = false;
    bool findThrOk = true;
    bool seenListAct = false, seenOcr = false, seenExcelActAfterOcr = false;
    for (const auto& a : c.defineAndBody) {
        if (a.type == ActionType::FindImage && a.matchThreshold < 84.5)
            findThrOk = false;
        if (a.type == ActionType::ActivateWindow
            && a.remark.find(L"activateWindow:") != std::wstring::npos
            && a.remark.find(L"Edge") != std::wstring::npos) {
            seenListAct = true;
        }
        if (a.type == ActionType::Wait
            && a.remark.find(L"activateWindow:") != std::wstring::npos
            && a.remark.find(L"Edge") != std::wstring::npos) {
            seenListAct = true;
        }
        if (a.type == ActionType::TextRecognition) {
            hasOcr = true;
            if (seenListAct) listActivateBeforeOcr = true;
            seenOcr = true;
        }
        if ((a.type == ActionType::ActivateWindow || a.type == ActionType::Wait)
            && a.remark.find(L"activateWindow:") != std::wstring::npos
            && a.remark.find(L"Excel") != std::wstring::npos && seenOcr) {
            seenExcelActAfterOcr = true;
        }
        if (a.type == ActionType::AiActionExecute
            && a.remark.find(L"动态列表") != std::wstring::npos) {
            hasDynAi = true;
            if (a.aiPrompt.find(L"只填表") != std::wstring::npos
                && a.aiPrompt.find(L"勿重做") != std::wstring::npos)
                dynFillOnlyTable = true;
        }
        if (a.type == ActionType::QuickInput) {
            if (a.inputText.find(L"火山方舟") != std::wstring::npos) hasFrozenTitle = true;
            if (a.inputText == L"序号" || a.inputText == L"标题") hasHeader = true;
        }
    }
    ocrBeforeExcelActivate = seenOcr && seenExcelActAfterOcr;
    const bool ok = c.ok && hasOcr && hasDynAi && hasHeader && !hasFrozenTitle
        && ocrBeforeExcelActivate && listActivateBeforeOcr && dynFillOnlyTable && findThrOk;
    std::wstring detail;
    if (!c.ok) detail = c.error;
    else if (!hasOcr) detail = L"noOcr";
    else if (!hasDynAi) detail = L"noDynAi";
    else if (!hasHeader) detail = L"noHeader";
    else if (hasFrozenTitle) detail = L"frozenTitle";
    else if (!ocrBeforeExcelActivate) detail = L"ocrNotBeforeExcelActivate";
    else if (!listActivateBeforeOcr) detail = L"listActivateNotBeforeOcr";
    else if (!dynFillOnlyTable) detail = L"dynFillMissingOnlyTable";
    else if (!findThrOk) detail = L"findImageThresholdLow";
    Emit(L"logic_convert_collapse_instance_data", ok, detail.c_str());

    // OCR 备注应带 listActivate；动态填表 aiMaxSteps 收紧
    bool ocrListAct = false, fillCap = false;
    for (const auto& a : c.defineAndBody) {
        if (a.type == ActionType::TextRecognition
            && a.remark.find(L"|listActivate:") != std::wstring::npos)
            ocrListAct = true;
        if (a.type == ActionType::AiActionExecute
            && a.remark.find(L"动态列表") != std::wstring::npos
            && a.aiMaxSteps > 0 && a.aiMaxSteps <= 30)
            fillCap = true;
    }
    Emit(L"logic_convert_ocr_list_activate_meta", ocrListAct && fillCap,
        (ocrListAct && fillCap) ? L"" : L"ocr缺listActivate或填表步数未收紧");

    // 另存为短文件名「浏览记录」+ 无列表窗：不得折叠成 OCR/动态 AI
    AiLogicConvertCompileInput saveAs;
    saveAs.taskPrompt = L"保存到桌面命名浏览记录";
    saveAs.preferredBlockName = L"LogicSaveAs";
    {
        AiLogicTranscriptEntry qi;
        qi.action.type = ActionType::QuickInput;
        qi.action.inputText = L"浏览记录";
        saveAs.transcript.push_back(qi);
        // 再塞几个也会被短文件名规则挡住；补几条长实例但无 Edge → 仍不折叠
        for (int i = 0; i < 4; ++i) {
            AiLogicTranscriptEntry e;
            e.action.type = ActionType::QuickInput;
            e.action.inputText = L"浏览记录";
            saveAs.transcript.push_back(e);
        }
    }
    const auto c2 = CompileAiLogicConvert(saveAs);
    bool c2Ocr = false, c2Dyn = false, c2KeepName = false;
    for (const auto& a : c2.defineAndBody) {
        if (a.type == ActionType::TextRecognition) c2Ocr = true;
        if (a.type == ActionType::AiActionExecute
            && a.remark.find(L"动态列表") != std::wstring::npos) c2Dyn = true;
        if (a.type == ActionType::QuickInput && a.inputText == L"浏览记录") c2KeepName = true;
    }
    const bool saveAsOk = c2.ok && !c2Ocr && !c2Dyn && c2KeepName;
    Emit(L"logic_convert_collapse_skip_filename", saveAsOk,
        saveAsOk ? L"" : L"短文件名被误折叠或未保留");
}

void CaseLocateFailKeyBlock() {
    ResetAiActionSessionState(42);
    AiNoteLocateFailed();
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const AgentTool* key = nullptr;
    const AgentTool* hot = nullptr;
    for (const auto& t : tools) {
        if (t.name == L"keyClick") key = &t;
        if (t.name == L"hotkeyShortcut") hot = &t;
    }
    bool ok = key && hot && key->execute && hot->execute;
    std::wstring detail;
    if (ok) {
        const std::wstring f12 = key->execute(L"{\"keyText\":\"F12\"}");
        if (f12.find(L"上轮 locate") == std::wstring::npos) {
            ok = false;
            detail = L"F12未硬拦";
        }
        const std::wstring ent = key->execute(L"{\"keyText\":\"Enter\"}");
        if (ent.find(L"上轮 locate") != std::wstring::npos) {
            ok = false;
            detail = L"Enter被误拦";
        }
        const std::wstring sav = hot->execute(L"{\"name\":\"save\"}");
        if (sav.find(L"上轮 locate") == std::wstring::npos) {
            ok = false;
            detail = L"hotkey未硬拦";
        }
    } else {
        detail = L"缺工具";
    }
    AiClearLocateFailKeyBlock();
    Emit(L"locate_fail_key_block", ok, detail.c_str());
}

void CaseLocateRetryHardCap() {
    ResetAiActionSessionState(42);
    const int initial = AiLocateRetryCount();
    AiNoteLocateFailed();
    const int afterOne = AiLocateRetryCount();
    AiNoteLocateFailed();
    const int afterTwo = AiLocateRetryCount();
    AiClearLocateFailKeyBlock();
    const int afterClear = AiLocateRetryCount();
    const bool ok = initial == 0 && afterOne == 1 && afterTwo == 2 && afterClear == 0;
    Emit(L"locate_retry_hard_cap", ok,
        (ok ? L"" : (L"count=" + std::to_wstring(initial) + L"," + std::to_wstring(afterOne)
            + L"," + std::to_wstring(afterTwo) + L"," + std::to_wstring(afterClear)).c_str()));
}

void CaseFillTableOnlyTools() {
    AiActionToolOptions opts;
    opts.fillTableOnly = true;
    const auto tools = BuildAiActionExecuteTools(nullptr, opts);
    bool hasQi = false, hasLocate = false, hasRun = false, hasHot = false, hasWeb = false;
    for (const auto& t : tools) {
        if (t.name == L"quickInput") hasQi = true;
        if (t.name == L"locateAndClick") hasLocate = true;
        if (t.name == L"runProgram") hasRun = true;
        if (t.name == L"hotkeyShortcut") hasHot = true;
        if (t.name == L"openWebpage") hasWeb = true;
    }
    const bool ok = hasQi && hasLocate && !hasRun && !hasHot && !hasWeb;
    Emit(L"fill_table_only_tools", ok,
        ok ? L"" : L"白名单过滤不符合预期");
}

void CaseListOcrHeuristic() {
    const bool listOk = LooksLikeListOcrText(
        L"1\n火山方舟标题\n23:15\n另一条\n22:01\nhttps://example.com");
    const bool saveReject = !LooksLikeListOcrText(L"另存为\n文件名\n浏览记录");
    const bool shortReject = !LooksLikeListOcrText(L"短");
    const bool ok = listOk && saveReject && shortReject;
    Emit(L"list_ocr_heuristic", ok, ok ? L"" : L"列表OCR启发不符合预期");
}

void CaseLogicConvertWritebackFirstTime() {
    const std::wstring path = MakeLogicTestScriptPath(L"t_first.json");
    ScriptFileData seed;
    seed.scriptName = L"t_first";
    ScriptAction ai;
    ai.type = ActionType::AiActionExecute;
    ai.aiPrompt = L"打开记事本并输入 hello";
    ai.aiLogicConvert = true;
    ai.aiWithImage = true;
    ai.originalNo = 1;
    ScriptAction tail;
    tail.type = ActionType::Wait;
    tail.duration = 0.3;
    tail.originalNo = 2;
    seed.actions.push_back(ai);
    seed.actions.push_back(tail);
    DeleteFileW(path.c_str());
    if (!SaveScriptFileData(path, seed)) {
        Emit(L"logic_convert_writeback_first_time", false, L"seed save failed");
        return;
    }

    AiLogicConvertCompileInput in;
    in.taskPrompt = ai.aiPrompt;
    in.preferredBlockName = L"LogicFirst";
    AiLogicTranscriptEntry e1;
    e1.action.type = ActionType::HotkeyShortcut;
    e1.action.shortcutPreset = 6;
    in.transcript.push_back(e1);
    AiLogicTranscriptEntry e2;
    e2.fromLocate = true;
    e2.locateTarget = L"打开";
    e2.templatePath = L"images\\logic_first_gate.bmp";
    e2.action.button = MouseButtonType::Left;
    e2.action.clickCount = 1;
    in.transcript.push_back(e2);
    AiLogicTranscriptEntry e3;
    e3.action.type = ActionType::QuickInput;
    e3.action.inputText = L"hello";
    in.transcript.push_back(e3);

    AiLogicConvertWritebackRequest req;
    req.scriptPath = path;
    req.sourceOriginalNo = 1;
    req.sourcePrompt = ai.aiPrompt;
    req.compiled = CompileAiLogicConvert(in);
    std::wstring err;
    const bool okWrite = ApplyAiLogicConvertWriteback(req, err);

    bool ok = okWrite;
    std::wstring detail = okWrite ? L"" : err;
    if (okWrite) {
        const ScriptFileData out = LoadScriptFileData(path, false);
        if (out.actions.size() < 3) {
            ok = false;
            detail = L"actions too few";
        } else {
            const auto& def = out.actions[0];
            if (def.type != ActionType::DefineBlock || def.blockName != L"LogicFirst") {
                ok = false;
                detail = L"block not at top / name mismatch";
            }
            bool hasTimer = false, hasGate = false, hasIf = false, hasElse = false;
            bool hasFallback = false, hasRun = false;
            for (const auto& a : out.actions) {
                if (a.type == ActionType::TimerRecordTime) hasTimer = true;
                if (a.type == ActionType::FindImage) hasGate = true;
                if (a.type == ActionType::If) hasIf = true;
                if (a.type == ActionType::Else) hasElse = true;
                if (a.type == ActionType::AiActionExecute && a.aiLogicConvert) hasFallback = true;
                if (a.type == ActionType::RunBlock && a.blockName == L"LogicFirst") hasRun = true;
            }
            if (!(hasTimer && hasGate && hasIf && hasElse && hasFallback && hasRun)) {
                ok = false;
                detail = L"block skeleton incomplete";
            }
            // 源顶层 AI 动作必须被 RunBlock 替换：不再有顶层 aiLogicConvert 残留
            int topLevelConvert = 0;
            for (const auto& a : out.actions) {
                if (a.type == ActionType::AiActionExecute && a.aiLogicConvert && a.indent == 0)
                    ++topLevelConvert;
            }
            if (topLevelConvert != 0) {
                ok = false;
                detail = L"source AI not replaced by runBlock";
            }
        }
    }
    DeleteFileW(path.c_str());
    Emit(L"logic_convert_writeback_first_time", ok, detail.c_str());
}

void CaseLogicConvertPreferredBlockNameAndHealCreate() {
    // 指定中文块名应保留（不再被 ASCII lettersOnly 剥光）；英文 locale 下 iswalnum 可能不认 CJK 则跳过
    const std::wstring cjk = MakeAiLogicBlockName(L"\u6d4f\u89c8\u8bb0\u5f55\u5757", L"ignored");
    const bool localeCjk = iswalnum(L'\u6d4f') != 0;
    const bool cjkOk = !localeCjk || cjk == L"\u6d4f\u89c8\u8bb0\u5f55\u5757";
    const std::wstring asciiPref = MakeAiLogicBlockName(L"LogicHealCreate", L"ignored");
    const bool asciiOk = asciiPref == L"LogicHealCreate";

    const std::wstring path = MakeLogicTestScriptPath(L"t_heal_create.json");
    ScriptFileData seed;
    seed.scriptName = L"t_heal_create";
    ScriptAction ai;
    ai.type = ActionType::AiActionExecute;
    ai.aiPrompt = L"\u6d4f\u89c8\u8bb0\u5f55\u5bfcExcel";
    ai.aiLogicConvert = true;
    ai.aiLogicBlockName = L"LogicHealCreate";
    ai.aiWithImage = true;
    ai.remark = L"\u903b\u8f91\u8f6c\u5316\u56de\u9000\uff08\u754c\u9762\u6f02\u79fb\u65f6\u81ea\u6108\u5e76\u63d0\u5347\u5feb\u8def\u5f84\uff09";
    ai.originalNo = 1;
    seed.actions.push_back(ai);
    DeleteFileW(path.c_str());
    if (!SaveScriptFileData(path, seed)) {
        Emit(L"logic_convert_heal_create_missing", false, L"seed save failed");
        return;
    }

    AiLogicConvertSessionBegin(true, L"LogicHealCreate", ai.aiPrompt, path, 1, true);
    AiLogicConvertNoteWindowActivate(L"Edge");
    AiLogicTranscriptEntry e;
    e.action.type = ActionType::HotkeyShortcut;
    e.action.shortcutPreset = 6;
    // NoteAction via public API
    AiLogicConvertNoteAction(e.action);
    AiLogicConvertMarkPendingWriteback(true);
    std::wstring summary, err;
    const bool flushed = TryFlushAiLogicConvertSession(path, summary, err);
    AiLogicConvertSessionEnd();

    bool hasBlock = false;
    if (flushed) {
        const ScriptFileData out = LoadScriptFileData(path, false);
        for (const auto& a : out.actions) {
            if (a.type == ActionType::DefineBlock && a.blockName == L"LogicHealCreate") {
                hasBlock = true;
                break;
            }
        }
    }
    const bool ok = cjkOk && asciiOk && flushed && hasBlock;
    std::wstring detail;
    if (!asciiOk) detail = L"ascii preferred mangled:" + asciiPref;
    else if (!cjkOk) detail = L"cjk preferred name mangled:" + cjk;
    else if (!flushed) detail = err.empty() ? L"flush failed" : err;
    else if (!hasBlock) detail = L"block not created";
    Emit(L"logic_convert_heal_create_missing", ok, detail.c_str());
    DeleteFileW(path.c_str());
}

void CaseLogicConvertWritebackPromote() {
    const std::wstring path = MakeLogicTestScriptPath(L"t_promote.json");
    ScriptFileData seed;
    seed.scriptName = L"t_promote";
    ScriptAction ai;
    ai.type = ActionType::AiActionExecute;
    ai.aiPrompt = L"打开记事本";
    ai.aiLogicConvert = true;
    ai.aiWithImage = true;
    ai.originalNo = 1;
    seed.actions.push_back(ai);
    DeleteFileW(path.c_str());
    if (!SaveScriptFileData(path, seed)) {
        Emit(L"logic_convert_writeback_promote", false, L"seed save failed");
        return;
    }

    auto makeCompile = [&](const std::wstring& gatePath, bool appendEnter) {
        AiLogicConvertCompileInput in;
        in.taskPrompt = ai.aiPrompt;
        in.preferredBlockName = L"LogicProm";
        AiLogicTranscriptEntry e1;
        e1.action.type = ActionType::HotkeyShortcut;
        e1.action.shortcutPreset = 6;
        in.transcript.push_back(e1);
        AiLogicTranscriptEntry e2;
        e2.fromLocate = true;
        e2.locateTarget = L"打开";
        e2.templatePath = gatePath;
        e2.action.button = MouseButtonType::Left;
        e2.action.clickCount = 1;
        in.transcript.push_back(e2);
        if (appendEnter) {
            AiLogicTranscriptEntry e3;
            e3.action.type = ActionType::KeyClick;
            e3.action.keyText = L"Enter";
            e3.action.keyVk = 13;
            in.transcript.push_back(e3);
        }
        return CompileAiLogicConvert(in);
    };

    AiLogicConvertWritebackRequest r1;
    r1.scriptPath = path;
    r1.sourceOriginalNo = 1;
    r1.sourcePrompt = ai.aiPrompt;
    r1.compiled = makeCompile(L"images\\gate_v1.bmp", false);
    std::wstring err;
    const bool firstOk = ApplyAiLogicConvertWriteback(r1, err);

    // 第二次「回退自愈」：同目标 locate 换新模板 v2 + 新增 Enter 按键
    AiLogicConvertWritebackRequest r2;
    r2.scriptPath = path;
    r2.sourceOriginalNo = 1;
    r2.sourcePrompt = ai.aiPrompt;
    r2.compiled = makeCompile(L"images\\gate_v2.bmp", true);
    const bool secondOk = ApplyAiLogicConvertWriteback(r2, err);

    bool ok = firstOk && secondOk;
    std::wstring detail = ok ? L"" : err;
    if (ok) {
        const ScriptFileData out = LoadScriptFileData(path, false);
        int defs = 0;
        bool findHealed = false;
        int findImageCount = 0, enterCount = 0, timerCount = 0;
        const std::wstring wantFind = ResolveImagePath(L"images\\gate_v2.bmp");
        for (const auto& a : out.actions) {
            if (a.type == ActionType::DefineBlock) ++defs;
            if (a.type == ActionType::TimerRecordTime) ++timerCount;
            if (a.type == ActionType::FindImage) {
                ++findImageCount;
                if (a.imagePath == wantFind) findHealed = true;
            }
            if (a.type == ActionType::KeyClick && a.keyVk == 13) ++enterCount;
        }
        if (defs != 1) { ok = false; detail = L"block duplicated after promote"; }
        else if (!findHealed) { ok = false; detail = L"findImage template not replaced with v2"; }
        else if (findImageCount != 1) { ok = false; detail = L"findImage count after promote"; }
        else if (timerCount < 1) { ok = false; detail = L"missing per-locate timer"; }
        else if (enterCount != 1) { ok = false; detail = L"new keyClick not promoted"; }
    }
    DeleteFileW(path.c_str());
    Emit(L"logic_convert_writeback_promote", ok, detail.c_str());
}

void CaseLookaheadAcceptReject() {
    // 纯逻辑：界面未变丢弃；变化则给出 hint
    const bool discardUnchanged = ShouldDiscardAiLookahead(true, false, false);
    const bool keepDynamicOnly = !ShouldDiscardAiLookahead(true, true, false);
    const bool discardDialog = ShouldDiscardAiLookahead(false, false, true);

    AiActionLookahead la;
    std::atomic_bool stop{false};
    la.SetFetchOverrideForTest([&](const std::wstring&, const std::atomic_bool&) {
        ToolCallRecord tc;
        tc.id = L"1";
        tc.name = L"keyClick";
        tc.arguments = L"{\"keyText\":\"Enter\"}";
        return std::vector<ToolCallRecord>{tc};
    });
    AgentConfig cfg;
    cfg.model = L"test";
    cfg.apiUrl = L"http://127.0.0.1";
    la.BeginAfterTools(cfg, L"[goal] t", L"quickInput -> ok", stop, nullptr);
    const std::wstring hintOk = la.TakeHintAfterObserve(false, false, false, 2000);
    const bool hintReady = hintOk.find(L"keyClick") != std::wstring::npos
        || hintOk.find(L"预规划") != std::wstring::npos;

    AiActionLookahead la2;
    la2.SetFetchOverrideForTest([&](const std::wstring&, const std::atomic_bool&) {
        ToolCallRecord tc;
        tc.name = L"wait";
        return std::vector<ToolCallRecord>{tc};
    });
    la2.BeginAfterTools(cfg, L"[goal] t", L"click", stop, nullptr);
    const std::wstring hintDrop = la2.TakeHintAfterObserve(true, false, false, 2000);
    const bool dropped = hintDrop.empty();

    AiLookaheadCachedPlan plan;
    plan.ready = true;
    ToolCallRecord t2;
    t2.name = L"locateAndClick";
    t2.arguments = L"{\"target\":\"OK\"}";
    plan.toolCalls = {t2};
    const std::wstring fmt = FormatAiLookaheadHint(plan);
    const bool fmtOk = fmt.find(L"locateAndClick") != std::wstring::npos;

    const bool ok = discardUnchanged && keepDynamicOnly && discardDialog
        && hintReady && dropped && fmtOk;
    Emit(L"lookahead_accept_reject", ok,
        ok ? L"" : L"lookahead unit logic failed");
}

void CaseRejectAbsoluteNoImage() {
    AiActionToolOptions opts;
    opts.allowAbsolutePointer = false;
    const auto tools = BuildAiActionExecuteTools(nullptr, opts);
    const std::wstring r = CallSubmitTool(tools,
        L"{\"actions\":[{\"type\":\"mouseClick\",\"x\":100,\"y\":200}]}");
    const bool ok = r.rfind(L"[错误]", 0) == 0
        && r.find(L"绝对坐标") != std::wstring::npos;
    Emit(L"reject_absolute_pointer_no_image", ok, r.c_str());
}

void CaseAllowAbsoluteWithOpt() {
    AiActionToolOptions opts;
    opts.allowAbsolutePointer = true;
    const auto tools = BuildAiActionExecuteTools(nullptr, opts);
    const std::wstring r = CallSubmitTool(tools,
        L"{\"actions\":[{\"type\":\"mouseClick\",\"x\":10,\"y\":20}],\"observeAfter\":false}");
    const bool ok = r.find(L"[EXECUTED]") != std::wstring::npos
        && r.find(L"mouseClick") != std::wstring::npos
        && r.rfind(L"[错误]", 0) != 0;
    Emit(L"allow_absolute_pointer_with_image_opt", ok, r.c_str());
}

void CaseRejectEmptyQuickInput() {
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring r = CallSubmitTool(tools,
        L"{\"actions\":[{\"type\":\"quickInput\",\"inputText\":\"\"}]}");
    const bool ok = r.rfind(L"[错误]", 0) == 0
        && r.find(L"inputText") != std::wstring::npos;
    Emit(L"reject_empty_quick_input", ok, r.c_str());
}

void CaseRejectBareKeyClick() {
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring r = CallSubmitTool(tools,
        L"{\"actions\":[{\"type\":\"keyClick\"}]}");
    const bool ok = r.rfind(L"[错误]", 0) == 0
        && (r.find(L"keyText") != std::wstring::npos || r.find(L"Enter") != std::wstring::npos);
    Emit(L"reject_bare_key_click", ok, r.c_str());
}

void CaseReplyActionsBuildable() {
    // 与 submitMacroActions 同路径：回复正文 + Enter 发送必须能构建
    nlohmann::json quick = {
        {"type", "quickInput"},
        {"inputText", "hello reply"}
    };
    nlohmann::json enter = {
        {"type", "keyClick"},
        {"keyText", "Enter"}
    };
    nlohmann::json stop = {{"type", "stopMacro"}};
    std::wstring err;
    const std::wstring json = BuildScriptActionsJsonArray({quick, enter, stop}, err);
    const bool ok = err.empty()
        && json.find(L"quickInput") != std::wstring::npos
        && json.find(L"keyClick") != std::wstring::npos
        && json.find(L"stopMacro") != std::wstring::npos;
    Emit(L"reply_actions_buildable", ok,
        err.empty() ? json.c_str() : err.c_str());
}

void CaseReplyEnterVk() {
    nlohmann::json enter = {
        {"type", "keyClick"},
        {"keyText", "Enter"}
    };
    auto built = BuildScriptActionFromJson(enter);
    const bool ok = built.ok && built.action.keyVk == 13
        && built.action.keyText == L"Enter";
    Emit(L"reply_enter_vk_return", ok,
        ok ? L"" : (built.ok ? L"keyVk!=13" : built.error.c_str()));
}

void CaseObserveMarkers() {
    const bool ok = IsSubmitObserveToolResult(L"[EXECUTED][OBSERVE]\n[]")
        && IsSubmitSkipObserveToolResult(L"[EXECUTED][SKIP_OBSERVE]\n[]")
        && !IsSubmitObserveToolResult(L"[EXECUTED][SKIP_OBSERVE]\n[]")
        && IsCompleteTaskToolResult(L"[TASK_COMPLETE] done")
        && !IsCompleteTaskToolResult(L"[EXECUTED][OBSERVE]")
        // Midscene：SKIP 但带灰钮事实 → 仍视为要观察
        && IsSubmitObserveToolResult(
            L"[EXECUTED][SKIP_OBSERVE]\n[UIA] 提交钮「保存」当前不可用。")
        && ToolResultNeedsForceObserve(L"[事实] settle无反应：界面相对操作前几乎无变化。")
        && !IsSubmitSkipObserveToolResult(
            L"[EXECUTED][SKIP_OBSERVE]\n[UIA] 提交钮「保存」当前不可用。");
    Emit(L"observe_markers", ok, ok ? L"" : L"marker helpers mismatch");
}

void CaseForceObserveSubmitBlock() {
    ResetAiActionSessionState(424244);
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) -> std::wstring {
        return L"已执行 1 步";
    };
    hooks.onProbeDisabledSubmit = [](std::wstring& name) {
        name = L"保存";
        return true;
    };
    hooks.onProbeForegroundDialog = []() {
        return L"kind=saveAs;title=另存为";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring uia = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"浏览记录\",\"clearFirst\":true}");
    const bool uiaForce = ToolResultNeedsForceObserve(uia)
        && IsSubmitObserveToolResult(uia)
        && uia.find(L"[UIA]") != std::wstring::npos;
    const std::wstring ent = CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"Enter\"}");
    const bool enterBlocked = ent.find(L"提交钮不可用") != std::wstring::npos;
    const bool enterObserve = IsSubmitObserveToolResult(ent);
    const bool ok = uiaForce && enterBlocked && enterObserve;
    const std::wstring detail = ok ? std::wstring()
        : (L"uia=" + uia.substr(0, 120) + L" ent=" + ent.substr(0, 120));
    Emit(L"force_observe_submit_block", ok, detail.c_str());
}

void CaseHistoryHelpers() {
    std::vector<ChatMessage> hist;
    ChatMessage user;
    user.role = L"user";
    user.content = L"task";
    hist.push_back(user);

    ChatMessage asst;
    asst.role = L"assistant";
    ToolCallRecord tc;
    tc.name = L"submitMacroActions";
    tc.arguments = L"{}";
    asst.tool_calls.push_back(tc);
    hist.push_back(asst);

    ChatMessage tool;
    tool.role = L"tool";
    tool.content = L"[EXECUTED][OBSERVE]\n[]";
    hist.push_back(tool);

    ChatMessage asst2;
    asst2.role = L"assistant";
    ToolCallRecord tc2;
    tc2.name = L"completeTask";
    asst2.tool_calls.push_back(tc2);
    hist.push_back(asst2);

    ChatMessage tool2;
    tool2.role = L"tool";
    tool2.content = L"[TASK_COMPLETE]";
    hist.push_back(tool2);

    const bool ok = HistoryHasSubmitMacroActions(hist, 0)
        && HistoryHasSubmitRequestingObserve(hist, 0)
        && HistoryHasCompleteTask(hist, 0);
    Emit(L"history_complete_and_submit", ok, ok ? L"" : L"history helpers failed");
}

void CaseUnknownActionTypeRejected() {
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring r = CallSubmitTool(tools,
        L"{\"actions\":[{\"type\":\"notARealActionTypeXYZ\"}]}");
    const bool ok = r.rfind(L"[错误]", 0) == 0
        && (r.find(L"\u672a\u77e5") != std::wstring::npos // 未知
            || r.find(L"notARealActionTypeXYZ") != std::wstring::npos);
    Emit(L"unknown_action_type_rejected", ok, ok ? L"" : r.c_str());
}

void CaseHybridPromptAgent() {
    const std::wstring p = BuildAiActionHybridSystemPrompt(100, 80);
    const std::wstring shortP = BuildAiActionToolExecuteSystemPrompt(100, 80, true);
    const std::wstring skill = MacroActionAgentSkill();
    const bool ok = p.find(L"section=agent") != std::wstring::npos
        && p.find(L"submitMacroActions") != std::wstring::npos
        && p.find(L"100") != std::wstring::npos
        && shortP.find(L"section=agent") != std::wstring::npos
        && shortP.find(L"goal|todos") != std::wstring::npos
        && skill.find(L"completeTask") != std::wstring::npos
        && skill.find(L"locateAndClick") != std::wstring::npos
        && skill.size() < 1200
        && shortP.size() < 600
        && p.find(L"参考 Computer Use") == std::wstring::npos
        && p.find(L"【宏动作目录") == std::wstring::npos
        && shortP.find(L"Ctrl+Space") == std::wstring::npos;
    Emit(L"hybrid_prompt_has_agent", ok, ok ? L"" : L"hybrid/ToolExecute prompt too heavy or missing pointers");
}

void CaseLookupSkillsNoCatalogDump() {
    const std::wstring usage = LookupMacroActionSchema(L"usage");
    const std::wstring agent = LookupMacroActionSchema(L"agent");
    const bool ok = usage.find(L"quickInput") != std::wstring::npos
        && agent.find(L"completeTask") != std::wstring::npos
        && usage.find(L"【宏动作目录") == std::wstring::npos
        && agent.find(L"【宏动作目录") == std::wstring::npos;
    Emit(L"lookup_skills_no_catalog_dump", ok, ok ? L"" : L"usage/agent should not append catalog");
}

void CaseParseParen() {
    int x = 0, y = 0;
    const bool ok = TryParseCoordinatePair(L"(12,34)", x, y) && x == 12 && y == 34;
    Emit(L"parse_coord_paren", ok, ok ? L"" : L"paren parse failed");
}

void CaseParseBBox() {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    const bool ok = TryParseBoundingBox(L"[10,20,100,80]", x1, y1, x2, y2)
        && x1 == 10 && y1 == 20 && x2 == 100 && y2 == 80;
    Emit(L"parse_bbox_brackets", ok, ok ? L"" : L"bbox parse failed");
}

void CaseZoomRoi() {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    BuildZoomRoiAroundScreenPoint(50, 50, 40, 0, 0, 200, 200, x1, y1, x2, y2);
    const bool ok = x1 >= 0 && y1 >= 0 && x2 <= 200 && y2 <= 200 && x2 > x1 && y2 > y1;
    Emit(L"zoom_roi_around_point", ok,
        (L"roi=" + std::to_wstring(x1) + L"," + std::to_wstring(y1) + L","
            + std::to_wstring(x2) + L"," + std::to_wstring(y2)).c_str());
}

void CaseAdaptiveRefineGate() {
    CoarseLocateRefineGateInput in;
    in.haveScreenBox = true;
    in.captureW = 1920;
    in.captureH = 1080;
    in.adaptiveRefineDepth = true;
    in.acceptCompactBboxWithoutRefine = false;

    // 紧凑按钮（像素空间）：应跳过二级
    in.boxW = 80;
    in.boxH = 36;
    in.coordsWereRemapped = false;
    CoarseLocateSkipReason why = CoarseLocateSkipReason::None;
    const bool compactOk = ShouldAcceptCoarseLocateWithoutRefine(in, &why)
        && why == CoarseLocateSkipReason::CompactPixel;

    // 过大区域：不应跳过
    in.boxW = 600;
    in.boxH = 400;
    why = CoarseLocateSkipReason::None;
    const bool largeReject = !ShouldAcceptCoarseLocateWithoutRefine(in, &why);

    // 任务栏级小图标（边长过小）：自适应不应跳过，避免邻近误点
    in.boxW = 24;
    in.boxH = 24;
    why = CoarseLocateSkipReason::None;
    const bool tinyReject = !ShouldAcceptCoarseLocateWithoutRefine(in, &why);

    // 归一化宽控件：应跳过
    in.boxW = 320;
    in.boxH = 40;
    in.coordsWereRemapped = true;
    why = CoarseLocateSkipReason::None;
    const bool wideOk = ShouldAcceptCoarseLocateWithoutRefine(in, &why)
        && why == CoarseLocateSkipReason::WideControlRemapped;

    // 无框 + 上传像素点：不跳过（需 Zoom）
    in.haveScreenBox = false;
    in.pointOnly = true;
    in.coordsWereRemapped = false;
    const bool noBoxPixelReject = !ShouldAcceptCoarseLocateWithoutRefine(in, nullptr);

    // 无框 + 归一化点：应跳过二级（省一轮 API）
    in.coordsWereRemapped = true;
    why = CoarseLocateSkipReason::None;
    const bool pointRemapOk = ShouldAcceptCoarseLocateWithoutRefine(in, &why)
        && why == CoarseLocateSkipReason::PointRemapped;

    // capture 缩放门禁：同尺寸粗框在 1080p 触发精炼，4K 下相对观察区更小可跳过
    CoarseLocateRefineGateInput s4k = in;
    s4k.haveScreenBox = true;
    s4k.pointOnly = false;
    s4k.coordsWereRemapped = false;
    s4k.captureW = 3840;
    s4k.captureH = 2160;
    s4k.boxW = 240;
    s4k.boxH = 100;
    why = CoarseLocateSkipReason::None;
    const bool fourKSkip = ShouldAcceptCoarseLocateWithoutRefine(s4k, &why)
        && why == CoarseLocateSkipReason::CompactPixel;

    CoarseLocateRefineGateInput fhd = s4k;
    fhd.captureW = 1920;
    fhd.captureH = 1080;
    why = CoarseLocateSkipReason::None;
    const bool fhdRefine = !ShouldAcceptCoarseLocateWithoutRefine(fhd, &why);

    // 扁宽文件名框（高度 <24）：应跳过，勿 Zoom 裁没
    in.haveScreenBox = true;
    in.pointOnly = false;
    in.boxW = 400;
    in.boxH = 18;
    in.coordsWereRemapped = true;
    in.captureW = 1920;
    in.captureH = 1080;
    why = CoarseLocateSkipReason::None;
    const bool thinWideOk = ShouldAcceptCoarseLocateWithoutRefine(in, &why)
        && why == CoarseLocateSkipReason::WideControlRemapped;

    // 归一化小图标（工具栏）：强制 Zoom，勿跳过
    in.boxW = 40;
    in.boxH = 32;
    in.coordsWereRemapped = true;
    why = CoarseLocateSkipReason::None;
    const bool smallIconForce = !ShouldAcceptCoarseLocateWithoutRefine(in, &why);

    // 归一化较大紧凑按钮：仍可跳过
    in.boxW = 96;
    in.boxH = 40;
    why = CoarseLocateSkipReason::None;
    const bool remapCompactSkip = ShouldAcceptCoarseLocateWithoutRefine(in, &why)
        && why == CoarseLocateSkipReason::CompactRemapped;

    const bool ok = compactOk && largeReject && tinyReject && wideOk
        && noBoxPixelReject && pointRemapOk && fourKSkip && fhdRefine && thinWideOk
        && smallIconForce && remapCompactSkip;
    Emit(L"adaptive_refine_gate", ok,
        ok ? L"" : L"compact/large/tiny/wide/point/capture-scale/thinWide/smallIcon gate mismatch");
}

void CaseParseColorHex() {
    int r = 0, g = 0, b = 0;
    const bool ok = TryParseColorSpec(L"#FF8800", r, g, b)
        && r == 255 && g == 136 && b == 0;
    Emit(L"parse_color_hex", ok, FormatColorHex(r, g, b).c_str());
}

void CaseBuildFindColor() {
    nlohmann::json j = {
        {"type", "findColor"},
        {"color", "#00FF00"},
        {"colorTolerance", 20},
        {"searchFullScreen", true},
        {"findImageFollowUp", 2}
    };
    auto built = BuildScriptActionFromJson(j);
    const bool ok = built.ok && built.action.type == ActionType::FindColor
        && built.action.colorG == 255 && built.action.colorTolerance == 20;
    Emit(L"build_find_color_action", ok,
        ok ? L"" : (built.ok ? L"field mismatch" : built.error.c_str()));
}

void CaseLocateAndClickToolPresent() {
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    bool found = false;
    bool hasTarget = false;
    bool hasRefine = false;
    bool hasOpen = false;
    for (const auto& t : tools) {
        if (t.name == L"locateAndClick") {
            found = true;
            hasTarget = t.parameters_json.find(L"target") != std::wstring::npos
                && t.parameters_json.find(L"required") != std::wstring::npos;
            hasRefine = t.parameters_json.find(L"refineLevels") != std::wstring::npos;
        }
        if (t.name == L"openWebpage") {
            hasOpen = t.parameters_json.find(L"targetPath") != std::wstring::npos;
        }
    }
    const std::wstring missing = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"搜索框\"}");
    const bool errOk = missing.rfind(L"[错误]", 0) == 0;
    const bool ok = found && hasTarget && hasRefine && hasOpen && errOk
        && IsMacroActionRunToolName(L"locateAndClick")
        && IsMacroActionRunToolName(L"openWebpage")
        && IsMacroActionRunToolName(L"findColor");
    Emit(L"locate_and_click_tool_present", ok,
        ok ? L"" : missing.c_str());
}

void CaseVisionNotFoundAndBoxTooLarge() {
    const bool nf = IsVisionLocateNotFound(L"NOT_FOUND")
        && IsVisionLocateNotFound(L"找不到该按钮")
        && !IsVisionLocateNotFound(L"[677,134,717,186]");
    const bool boxTooBig = IsVisionApiBoxTooLarge(0, 0, 900, 500, 1024, 576, 0.22);
    const bool boxOk = !IsVisionApiBoxTooLarge(693, 76, 733, 107, 1024, 576, 0.22);
    // 竖向大片（历史侧栏乱框）：面积比可能不高，单边仍应拒
    const bool tallReject = IsVisionApiBoxTooLarge(718, 42, 928, 509, 1000, 1000, 0.35);
    Emit(L"vision_not_found_and_box_filter", nf && boxTooBig && boxOk && tallReject,
        (nf && boxTooBig && boxOk && tallReject) ? L"" : L"NOT_FOUND or area filter failed");
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
        && j.find(L"stopMacro") != std::wstring::npos
        && j.find(L"coordSpace") != std::wstring::npos
        && j.find(L"screen") != std::wstring::npos;
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

void CaseRefinePromptCoordOnly() {
    const std::wstring p = BuildCompositeRefinePointPrompt(L"点击搜索按钮", 1, 112, 140);
    const bool ok = p.find(L"112") != std::wstring::npos
        && p.find(L"140") != std::wstring::npos
        && p.find(L"(x,y)") != std::wstring::npos;
    Emit(L"refine_prompt_coord_only", ok, ok ? L"" : L"refine prompt missing size/format");
}

void CaseApiPointOutside() {
    const bool out = IsApiPointClearlyOutsideImage(984, 327, 720, 720);
    int x = 10, y = 10;
    const bool clampOk = TryClampApiPointToImage(x, y, 720, 720);
    int badX = 984, badY = 327;
    const bool clampBad = !TryClampApiPointToImage(badX, badY, 720, 720);
    Emit(L"api_point_outside_reject", out && clampOk && clampBad, L"");
}

void CaseRefineDrift() {
    Emit(L"refine_drift_too_far",
        IsRefineScreenDriftTooFar(1396, 321, 2020, 327, 220)
        && !IsRefineScreenDriftTooFar(1396, 321, 1410, 330, 220), L"");
}

void CaseResolveVisionNorm1000() {
    int x1 = 356, y1 = 963, x2 = 375, y2 = 993;
    std::wstring note;
    const bool ok = ResolveVisionRectToApiImage(x1, y1, x2, y2, 1280, 720, 2560, 1440, &note);
    // 0~1000 → api: y≈693..715（任务栏带），不再飞出 720
    const bool yOk = ok && y1 >= 680 && y1 <= 710 && y2 >= 700 && y2 <= 720
        && x1 >= 440 && x1 <= 470;
    Emit(L"resolve_vision_rect_norm1000", yOk && note.find(L"1000") != std::wstring::npos,
        (L"api=[" + std::to_wstring(x1) + L"," + std::to_wstring(y1) + L","
            + std::to_wstring(x2) + L"," + std::to_wstring(y2) + L"] " + note).c_str());
}

void CaseResolveVisionAmbiguousInImageAsNorm1000() {
    // 真实失败：搜索钮 [677,134,717,186] 落在 1024×576 图内，误当像素 → 屏幕(~1740,400) 点到剪映
    // 应按 0~1000 → api≈[693,77,734,107] → 屏幕中心 y≈230（搜索栏高度），不是 400
    int x1 = 677, y1 = 134, x2 = 717, y2 = 186;
    std::wstring note;
    const bool ok = ResolveVisionRectToApiImage(x1, y1, x2, y2, 1024, 576, 2560, 1440, &note);
    const int cx = (x1 + x2) / 2;
    const int cy = (y1 + y2) / 2;
    const int screenY = cy * 1440 / 576;
    const bool mappedOk = ok && note.find(L"1000") != std::wstring::npos
        && y1 >= 70 && y1 <= 90 && y2 >= 100 && y2 <= 120
        && screenY >= 180 && screenY <= 280;
    Emit(L"resolve_vision_ambiguous_in_image_as_norm1000", mappedOk,
        (L"api=[" + std::to_wstring(x1) + L"," + std::to_wstring(y1) + L","
            + std::to_wstring(x2) + L"," + std::to_wstring(y2) + L"] c=("
            + std::to_wstring(cx) + L"," + std::to_wstring(cy)
            + L") screenY≈" + std::to_wstring(screenY) + L" " + note).c_str());
}

void CaseResolveVisionSrcPixels() {
    int x = 2000, y = 1000;
    std::wstring note;
    const bool ok = ResolveVisionPointToApiImage(x, y, 1280, 720, 2560, 1440, &note);
    Emit(L"resolve_vision_point_src_pixels",
        ok && x == 1000 && y == 500 && note.find(L"原图") != std::wstring::npos,
        (L"api=(" + std::to_wstring(x) + L"," + std::to_wstring(y) + L") " + note).c_str());
}

void CaseResolveVisionRejectOob() {
    int x1 = 10, y1 = 10, x2 = 2000, y2 = 1800;
    Emit(L"resolve_vision_rect_reject_oob",
        !ResolveVisionRectToApiImage(x1, y1, x2, y2, 1280, 720, 2560, 1440, nullptr), L"");
}

void CaseResolveAgentPointerInImageAsPixels() {
    // 768×432 图内点若按 0~1000 会错位；Agent 契约应保留像素
    int x = 304, y = 320;
    std::wstring note;
    const bool ok = ResolveAgentPointerToApiImage(x, y, 768, 432, 2560, 1440, &note);
    Emit(L"resolve_agent_pointer_in_image_as_pixels",
        ok && x == 304 && y == 320 && note.find(L"像素") != std::wstring::npos,
        (L"api=(" + std::to_wstring(x) + L"," + std::to_wstring(y) + L") " + note).c_str());
}

void CaseResolveAgentPointerOobAsNorm1000() {
    // 日志案例：@258,978 / @860,108 在 768×432 外，应按 0~1000（勿线性放大到屏幕外）
    int x = 258, y = 978;
    std::wstring note;
    const bool ok1 = ResolveAgentPointerToApiImage(x, y, 768, 432, 2560, 1440, &note);
    const bool a = ok1 && note.find(L"1000") != std::wstring::npos
        && x >= 190 && x <= 210 && y >= 410 && y <= 430;
    x = 860; y = 108; note.clear();
    const bool ok2 = ResolveAgentPointerToApiImage(x, y, 768, 432, 2560, 1440, &note);
    const bool b = ok2 && note.find(L"1000") != std::wstring::npos
        && x >= 650 && x <= 670 && y >= 40 && y <= 55;
    Emit(L"resolve_agent_pointer_oob_as_norm1000", a && b,
        (L"978→ok=" + std::to_wstring(a ? 1 : 0) + L" 860→api=("
            + std::to_wstring(x) + L"," + std::to_wstring(y) + L") " + note).c_str());
}

void CaseResolveAgentPointerRejectOob() {
    int x = 1500, y = 2000;
    Emit(L"resolve_agent_pointer_reject_oob",
        !ResolveAgentPointerToApiImage(x, y, 768, 432, 2560, 1440, nullptr), L"");
}

void CaseActionVerbExpansion() {
    // 扩充的动作词：滚动/移动鼠标/切换/保存/重命名等 → ToolExecute（走工具而非识图问答）
    const bool scroll = ClassifyAiActionRoute(L"滚动到页面底部", true) == AiActionRouteKind::ToolExecute
        && ClassifyAiActionRoute(L"滑动验证码", true) == AiActionRouteKind::ToolExecute
        && ClassifyAiActionRoute(L"滚轮向下翻一页", true) == AiActionRouteKind::ToolExecute;
    const bool move = ClassifyAiActionRoute(L"移动鼠标到屏幕中央", true) == AiActionRouteKind::ToolExecute;
    const bool sw = ClassifyAiActionRoute(L"切换到微信窗口", true) == AiActionRouteKind::ToolExecute;
    const bool saveOp = ClassifyAiActionRoute(L"保存当前文档", true) == AiActionRouteKind::ToolExecute
        && ClassifyAiActionRoute(L"重命名文件为测试", true) == AiActionRouteKind::ToolExecute
        && ClassifyAiActionRoute(L"新建一个文档", true) == AiActionRouteKind::ToolExecute
        && ClassifyAiActionRoute(L"删除选中的文件", true) == AiActionRouteKind::ToolExecute;
    Emit(L"route_scroll_wheel_tool", scroll, L"");
    Emit(L"route_move_mouse_verb", move, L"");
    Emit(L"route_switch_window_tool", sw, L"");
    Emit(L"route_save_rename_tool", saveOp, L"");
    // 「移动端设备」里的「移动」不是鼠标动作词；无其它动作词 → 默认 ToolExecute（非识图）
    Emit(L"route_mobile_device_not_move",
        ClassifyAiActionRoute(L"移动端设备的配置", true) == AiActionRouteKind::ToolExecute, L"");
}

void CaseNeedScreenScrollCapture() {
    const bool yes = AiActionPromptLikelyNeedsScreenCapture(L"滚动到页面底部")
        && AiActionPromptLikelyNeedsScreenCapture(L"滑动验证码")
        && AiActionPromptLikelyNeedsScreenCapture(L"移动鼠标到屏幕中央")
        && AiActionPromptLikelyNeedsScreenCapture(L"把文件拖拽到回收站");
    const bool no = !AiActionPromptLikelyNeedsScreenCapture(L"分析变量 count，若大于0则按 Enter");
    Emit(L"need_screen_scroll_yes", yes && no, L"");
}

void CaseClickIntent() {
    Emit(L"click_intent_double_right",
        PromptIntendsDoubleClick(L"双击打开桌面上的QQ图标")
            && !PromptIntendsDoubleClick(L"点击确认按钮")
            && PromptIntendsRightClick(L"右键点击桌面空白处")
            && PromptIntendsRightClick(L"right click the icon")
            && !PromptIntendsRightClick(L"点击确定"), L"");
}

void CaseSingleClickVerb() {
    // 「单击」是同「点击」的一步点击动词：须走 CompositeClick 快路径 + 首帧截图，
    // 不能落入识图问答（否则整句被当问题丢给视觉模型）。
    const bool ok = ClassifyAiActionRoute(L"单击确认按钮", true) == AiActionRouteKind::CompositeClick
        && ClassifyAiActionRoute(L"单击开始菜单里的设置", true) == AiActionRouteKind::CompositeClick
        && AiActionPromptLikelyNeedsScreenCapture(L"单击确认按钮");
    Emit(L"route_single_click_composite", ok, L"");
}

void CaseExtractTargetPhrase() {
    const bool a = ExtractClickTargetPhrase(L"点击确认按钮") == L"确认按钮";
    const bool b = ExtractClickTargetPhrase(L"双击打开桌面上的QQ图标") == L"桌面上的QQ图标";
    const bool c = ExtractClickTargetPhrase(L"点击搜索图标，再输入关键词") == L"搜索图标";
    const bool d = ExtractClickTargetPhrase(L"右键点击桌面空白处") == L"桌面空白处";
    const bool e = ExtractClickTargetPhrase(L"请帮我点击开始按钮。") == L"开始按钮";
    // 邻近相似图标：保留「位于…」消歧，勿只留短标签导致点错邻居
    const std::wstring hist = ExtractClickTargetPhrase(
        L"历史记录图标，位于Edge浏览器顶部地址栏右侧，时钟形状图标");
    const bool f = hist.find(L"历史记录") != std::wstring::npos
        && hist.find(L"位于") != std::wstring::npos;
    // 短引号 + 方位：勿只留「桌面」，否则 VLM 易整屏乱框
    const std::wstring desk = ExtractClickTargetPhrase(
        L"另存为窗口左侧导航栏里的“桌面”选项");
    const bool g = desk.find(L"桌面") != std::wstring::npos
        && desk.find(L"左侧") != std::wstring::npos
        && desk != L"桌面";
    Emit(L"composite_target_phrase_stripped",
        a && b && c && d && e && f && g,
        (L"[a=" + std::to_wstring(a ? 1 : 0)
            + L" b=" + std::to_wstring(b ? 1 : 0)
            + L" c=" + std::to_wstring(c ? 1 : 0)
            + L" d=" + std::to_wstring(d ? 1 : 0)
            + L" e=" + std::to_wstring(e ? 1 : 0)
            + L" f=" + std::to_wstring(f ? 1 : 0)
            + L" g=" + std::to_wstring(g ? 1 : 0) + L"]").c_str());
}

void CaseParseBBoxLeadingNumber() {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    const bool ok = TryParseBoundingBox(L"第2个按钮在 [100,200,300,400]", x1, y1, x2, y2)
        && x1 == 100 && y1 == 200 && x2 == 300 && y2 == 400;
    Emit(L"parse_bbox_leading_number", ok,
        ok ? L"" : (L"parsed " + std::to_wstring(x1) + L"," + std::to_wstring(y1)
            + L"," + std::to_wstring(x2) + L"," + std::to_wstring(y2)).c_str());
}

void CaseParseBBoxParens() {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    const bool ok = TryParseBoundingBox(L"目标位于 (10,20,100,80)", x1, y1, x2, y2)
        && x1 == 10 && y1 == 20 && x2 == 100 && y2 == 80;
    Emit(L"parse_bbox_parens", ok,
        ok ? L"" : (L"parsed " + std::to_wstring(x1) + L"," + std::to_wstring(y1)
            + L"," + std::to_wstring(x2) + L"," + std::to_wstring(y2)).c_str());
}

void CaseVisionNotFoundVariants() {
    const bool yes = IsVisionLocateNotFound(L"未检测到目标")
        && IsVisionLocateNotFound(L"屏幕上没有找到该按钮")
        && IsVisionLocateNotFound(L"没看到任何内容")
        && IsVisionLocateNotFound(L"NOT FOUND")
        && IsVisionLocateNotFound(L"检测不到这个元素");
    const bool no = !IsVisionLocateNotFound(L"[100,200,300,400]")
        && !IsVisionLocateNotFound(L"找到了，在第2个位置");
    Emit(L"vision_not_found_variants", yes && no, L"");
}

void CaseLabels() {
    const bool ok = !AiActionRouteLabel(AiActionRouteKind::VisionQuery).empty()
        && !AiActionRouteLabel(AiActionRouteKind::CompositeClick).empty()
        && !AiActionRouteLabel(AiActionRouteKind::MultiTurnTools).empty()
        && !AiActionRouteLabel(AiActionRouteKind::ToolExecute).empty();
    Emit(L"route_label_zh", ok, L"");
}

// ── 模型适用性路由：非多模态主模型 → 备用多模态/识图模型 ────────

void CaseResolveActionVisionFallback() {
    // 用户给 aiImageAnalysis / aiActionExecute 指定纯文本模型时，
    // ResolveActionAiModelName 必须自动换 savedModels 中的识图模型。
    quickscript::AiApiSettings ai;
    ai.modelName = L"deepseek-chat";
    ai.savedModels.push_back({ L"https://api.openai.com/v1/chat/completions", L"", L"gpt-4o" });
    ai.savedModels.push_back({ L"", L"", L"deepseek-chat" });

    ScriptAction imageAnalysis;
    imageAnalysis.type = ActionType::AiImageAnalysis;
    imageAnalysis.aiModelName = L"deepseek-chat";  // 显式指定文本模型
    const std::wstring m1 = ResolveActionAiModelName(imageAnalysis, ai);

    ScriptAction actionExecute;
    actionExecute.type = ActionType::AiActionExecute;
    actionExecute.aiWithImage = true;
    actionExecute.aiModelName = L"deepseek-chat";
    const std::wstring m2 = ResolveActionAiModelName(actionExecute, ai);

    // 纯文本分析不换模型
    ScriptAction textAnalysis;
    textAnalysis.type = ActionType::AiTextAnalysis;
    textAnalysis.aiModelName = L"deepseek-chat";
    const std::wstring m3 = ResolveActionAiModelName(textAnalysis, ai);

    const bool ok = m1 == L"gpt-4o" && m2 == L"gpt-4o" && m3 == L"deepseek-chat";
    Emit(L"resolve_action_vision_fallback", ok, L"");
}

void CaseResolveVisionSubtask() {
    quickscript::AiApiSettings ai;
    ai.modelName = L"deepseek-chat";
    ai.savedModels.push_back({ L"", L"", L"gpt-4o" });
    ai.savedModels.push_back({ L"", L"", L"qwen-vl-plus" });
    ai.savedModels.push_back({ L"", L"", L"deepseek-chat" });

    // 主模型支持识图 → 直接用主模型
    const std::wstring m1 = ResolveVisionSubtaskModelName(ai, L"gpt-4o");
    // 主模型为文本 → 换 savedModels 中第一个识图模型
    const std::wstring m2 = ResolveVisionSubtaskModelName(ai, L"deepseek-chat");
    // 主模型为空 → 仍能选出识图模型
    const std::wstring m3 = ResolveVisionSubtaskModelName(ai, L"");

    quickscript::AiApiSettings noVision;
    noVision.modelName = L"deepseek-chat";
    noVision.savedModels.push_back({ L"", L"", L"deepseek-r1" });
    // 没有任何多模态模型 → 返回空（提示配置识图模型）
    const std::wstring m4 = ResolveVisionSubtaskModelName(noVision, L"deepseek-chat");

    const bool hasOk = HasUsableVisionModel(ai, L"deepseek-v4-flash")
        && !HasUsableVisionModel(noVision, L"deepseek-v4-flash");
    const std::wstring miss = MissingVisionModelError(L"deepseek-v4-flash");
    const bool missOk = miss.find(L"跳过") != std::wstring::npos
        && miss.find(L"识图") != std::wstring::npos;

    const bool ok = m1 == L"gpt-4o" && m2 == L"gpt-4o"
        && m3 == L"gpt-4o" && m4.empty() && hasOk && missOk;
    Emit(L"resolve_vision_subtask_model", ok, L"");
}

void CaseVisionRouteModelRouted() {
    // 文本主模型 → 列表识图模型（locate / Vision 子任务）
    quickscript::AiApiSettings ai;
    ai.modelName = L"deepseek-chat";
    ai.savedModels.push_back({ L"", L"", L"glm-4v" });

    const bool textMain = !ModelSupportsVision(L"deepseek-chat");
    const bool visionMain = ModelSupportsVision(L"gpt-4o");
    const bool fallbackFound = ResolveVisionSubtaskModelName(ai, L"deepseek-chat") == L"glm-4v";
    const bool usable = HasUsableVisionModel(ai, L"deepseek-v4-flash");

    Emit(L"vision_route_model_routed", textMain && visionMain && fallbackFound && usable, L"");
}

void CaseModelSupportsVision() {
    const bool ok = ModelSupportsVision(L"gpt-4o")
        && ModelSupportsVision(L"qwen-vl-max")
        && ModelSupportsVision(L"claude-3-5-sonnet")
        && ModelSupportsVision(L"glm-4.5v")
        && ModelSupportsVision(L"doubao-seed-2-1-pro-260628")
        && !ModelSupportsVision(L"deepseek-chat")
        && !ModelSupportsVision(L"deepseek-v4-flash")
        && !ModelSupportsVision(L"deepseek-r1")
        && !ModelSupportsVision(L"qwen-turbo");
    Emit(L"model_supports_vision", ok, L"");
}

void CasePlannerObserveImageAttach() {
    const bool textNo = !ShouldAttachObserveImageToPlanner(L"deepseek-v4-flash", true)
        && !ShouldAttachObserveImageToPlanner(L"deepseek-chat", true);
    const bool visionYes = ShouldAttachObserveImageToPlanner(L"doubao-seed-1", true)
        && ShouldAttachObserveImageToPlanner(L"gpt-4o", true);
    const bool emptyNo = !ShouldAttachObserveImageToPlanner(L"gpt-4o", false);
    Emit(L"planner_observe_image_attach", textNo && visionYes && emptyNo, L"");
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
    CaseCompositeNotFooledByInputBoxNoun();
    CaseCorrectLocatePrompt();
    CaseNeedScreenCaptureHeuristic();
    CaseMultiTurn();
    CaseToolOpen();
    CaseLookScreenVision();
    CaseReplyMessageTool();
    CaseReplySendNotMultiTurn();
    CaseAmbiguousDefaultTool();
    CaseUsageSkill();
    CaseAgentSkill();
    CaseAtomicToolsPresent();
    CaseResolveSystemPathAndSavePathGuard();
    CaseSaveDialogSubmitGuard();
    CaseSwitchWindowAfterLaunchWarned();
    CaseHideWindowsGuard();
    CaseWindowLedgerTools();
    CaseSaveDialogGuards();
    CaseHotkeyNameAndBrowserReuse();
    CaseOpenAppViaSearchFallback();
    CaseLocateFailSuggestsActivate();
    CaseRunActionRecipe();
    CaseLocateDoubleClick();
    CaseTaskMemoAndOpenDedup();
    CaseTaskDataClearedOnReset();
    CaseObserveResultDefaults();
    CaseWaitRequestsObserve();
    CaseQuickInputClearPolicy();
    CaseAtomicQuickInputEmpty();
    CaseAtomicKeyClickEnter();
    CaseNonUniversalShortcutGuard();
    CaseAllowNestedUnderCap();
    CaseRejectNestedAtCap();
    CasePlanGateRequiresMemo();
    CaseLocateTargetLengthAndOutsourceReject();
    CaseHistorySidebarAndBusyGuards();
    CaseLogicConvertCompileGate();
    CaseLogicConvertPerLocateTimers();
    CaseLogicConvertAssistantGate();
    CaseLogicConvertWritebackGuards();
    CaseLogicConvertSessionRecording();
    CaseLogicConvertCompileWaitFilter();
    CaseLogicConvertCompileNoAnchor();
    CaseLogicConvertCollapseInstanceData();
    CaseLocateFailKeyBlock();
    CaseLocateRetryHardCap();
    CaseFillTableOnlyTools();
    CaseListOcrHeuristic();
    CaseLogicConvertWritebackFirstTime();
    CaseLogicConvertPreferredBlockNameAndHealCreate();
    CaseLogicConvertWritebackPromote();
    CaseLookaheadAcceptReject();
    CaseRejectAbsoluteNoImage();
    CaseAllowAbsoluteWithOpt();
    CaseRejectEmptyQuickInput();
    CaseRejectBareKeyClick();
    CaseReplyActionsBuildable();
    CaseReplyEnterVk();
    CaseObserveMarkers();
    CaseForceObserveSubmitBlock();
    CaseHistoryHelpers();
    CaseUnknownActionTypeRejected();
    CaseHybridPromptAgent();
    CaseLookupSkillsNoCatalogDump();
    CaseParseParen();
    CaseParseBBox();
    CaseZoomRoi();
    CaseAdaptiveRefineGate();
    CaseParseColorHex();
    CaseBuildFindColor();
    CaseLocateAndClickToolPresent();
    CaseVisionNotFoundAndBoxTooLarge();
    CaseParseCnComma();
    CaseParseReject();
    CaseMapApi();
    CaseBuildWithStop();
    CaseBuildNoStop();
    CaseVisionPromptSize();
    CaseRefinePromptCoordOnly();
    CaseApiPointOutside();
    CaseRefineDrift();
    CaseResolveVisionNorm1000();
    CaseResolveVisionAmbiguousInImageAsNorm1000();
    CaseResolveVisionSrcPixels();
    CaseResolveVisionRejectOob();
    CaseResolveAgentPointerInImageAsPixels();
    CaseResolveAgentPointerOobAsNorm1000();
    CaseResolveAgentPointerRejectOob();
    CaseActionVerbExpansion();
    CaseNeedScreenScrollCapture();
    CaseClickIntent();
    CaseSingleClickVerb();
    CaseExtractTargetPhrase();
    CaseParseBBoxLeadingNumber();
    CaseParseBBoxParens();
    CaseVisionNotFoundVariants();
    CaseLabels();
    CaseResolveActionVisionFallback();
    CaseResolveVisionSubtask();
    CaseVisionRouteModelRouted();
    CaseModelSupportsVision();
    CasePlannerObserveImageAttach();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
