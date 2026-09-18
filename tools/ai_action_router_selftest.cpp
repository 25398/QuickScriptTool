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
#include "ai_locate_cache.h"
#include "ai_ui_layout.h"
#include "ai_plan_util.h"
#include "ai_locate_verify.h"
#include "office_doc.h"
#include "ai_logic_convert.h"
#include "app_settings_store.h"
#include "color_match.h"
#include "macro_execute_tools.h"
#include "page_snapshot.h"
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

/// 临时测试目录（自检用；不存在就建）
std::wstring MakeTempTestDir(const std::wstring& name) {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".\\");
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\" + name;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

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
    {L"open_webpage_api_and_same_site", L"default",
        L"禁开 api 接口；同站禁止再编 URL"},
    {L"task_data_cleared_on_reset", L"default",
        L"ResetAiActionSessionState 清空 saveTaskData 缓存，防串上轮 historyRecords"},
    {L"page_kind_classify", L"default",
        L"canvasRatio+interactive → dom/mixed/canvas"},
    {L"page_snapshot_format_and_ref", L"default",
        L"Parse/Format 压缩树；canvas 禁 HTML；clickRef 门闩；可视/屏外覆盖"},
    {L"scroll_wheel_confirm_when_viewport_has_content", L"default",
        L"可视区已有主内容时 scrollWheel 须 confirmScroll"},
    {L"list_first_blocks_scroll_and_other_cards", L"default",
        L"有列表第1项时禁止滚动/点其它内容卡；播放页相关推荐不算第1项"},
    {L"web_browse_allows_vision_fallback", L"default",
        L"网页扩展是优化：树上没有或未装扩展时 locateAndClick 仍可用"},
    {L"web_mixed_prefers_tree_click", L"default",
        L"播放页树上有按钮则 locateAndClick/mouseClick 改 clickRef；识图失败后禁假完成"},
    {L"logic_convert_bridge_nav", L"default",
        L"扩展桥导航固化为打开网页，clickRef 可记找图模板"},
    {L"observe_page_tools_present", L"default",
        L"observePage/clickRef 工具注册；canvas 跳过抓树"},
    {L"search_on_page_tool", L"default",
        L"searchOnPage 构造站点搜索 URL 并走 navigatePage"},
    {L"click_ref_navigates_space_href", L"default",
        L"clickRef 跟 href 打开用户主页；搜索结果允许 space URL；滚轮无图不拦"},
    {L"same_page_click_hint_scoped", L"default",
        L"同页 clickRef：仅搜索页提示点卡片；内容页不拦分享/作者、不误导去跳转"},
    {L"search_on_page_opens_matching_space", L"default",
        L"searchOnPage 名字命中用户卡片时直接打开主页"},
    {L"observe_result_defaults", L"default",
        L"AiObserveCaptureResult / aiObs 常量"},
    {L"atomic_quick_input_rejects_empty", L"default",
        L"规范工具 quickInput 拒空 inputText"},
    {L"atomic_key_click_enter", L"default",
        L"规范工具 keyClick(Enter) 可执行"},
    {L"non_universal_shortcut_guard", L"default",
        L"应用专属组合键(Ctrl+H)默认劝退；通用键放行；网页禁开新标签；Skill 不再教 F12/Ctrl+H"},
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
        L"ModelSupportsVision 对主流多模态/文本模型的判定（DeepSeek v4 起算多模态）"},
    {L"action_model_not_silently_swapped", L"default",
        L"AI 动作执行：选了多模态模型就原样用；写库时不静默改写动作模型；纯文本+识图才兜底换模型"},
    {L"planner_observe_image_attach", L"default",
        L"纯文本规划模型不附观察截图；多模态才附"},
    {L"pick_snapshot_ref_for_text", L"default",
        L"控件树按短标签选 ref：完全同名优先，树上没有则交回识图"},
    {L"pick_snapshot_ref_ambiguous", L"default",
        L"部分命中且近似竞争 → ambiguous 不许 DOM 直点；同名多项取列表第1项"},
    {L"snapshot_target_keyword", L"default",
        L"短标签压成扩展 query 关键字（剥动作前缀/通用后缀）"},
    {L"vision_prompt_normalized_contract", L"default",
        L"定位 prompt 明确 0~1000 归一化并禁止像素坐标"},
    {L"locate_tool_defers_to_host_dom", L"default",
        L"有宿主 DOM 钩子时 locateAndClick 不再报错浪费一轮；无宿主保留指路错误"},
    {L"lookahead_wait_budget", L"default",
        L"预规划等待上限收紧 + 次数上限 2 + 无在途时 Cancel 立刻返回"},
    {L"dom_first_action_gate", L"default",
        L"DOM 优先门禁：扩展/钩子/canvas/右键/前台非浏览器一律不放行"},
    {L"pick_snapshot_ref_for_input", L"default",
        L"填写口径选输入框（textbox/searchbox），与点击口径不同"},
    {L"type_by_label_tool", L"default",
        L"typeByLabel 一次调用按标签填写；无命中/无钩子/空参报错并指路"},
    {L"ui_control_tools", L"default",
        L"listUiControls/invokeUiControl：台账透传、执行标记、报错透传、只读不被计划门闩拦"},
    {L"locate_cache", L"default",
        L"定位模板缓存：键归一/窗口隔离/高分唯一才命中/无效果作废/清理释放"},
    {L"wide_row_still_needs_refine", L"default",
        L"过宽的菜单行框必须继续 Zoom 精炼；≤420px 宽按钮与地址栏仍可省一轮"},
    {L"url_input_heuristic", L"default",
        L"LooksLikeUrlInput：edge:// / http / www / 裸域名放行；中文与普通词不算 URL"},
    {L"enter_after_url_input", L"default",
        L"页面树存在时 Enter 默认拦；刚输入 URL 后放行（地址栏导航）"},
    {L"browser_title_hint_strip", L"default",
        L"observePage 的 hint 剥掉「和另外 N 个页面 - 个人 - Edge」等窗口装饰"},
    {L"run_command_tool", L"default",
        L"runCommand 落到「运行程序」动作：powershell -Command / cmd /c，可回放，空参报错；"
        L"交宿主的必须是纯 JSON 数组（提示尾巴/命令里的 ] 都不能截断）"},
    {L"extract_action_json_array", L"default",
        L"动作 JSON 抽取：跳过字符串内的括号、忽略「[提示]…」尾巴；内置页 URL 识别"},
    {L"open_webpage_internal_page", L"default",
        L"openWebpage(edge://…)：首选让浏览器带 URL 打开（不再合成键把 URL 打进搜索栏），"
        L"识别不出浏览器才退回地址栏路线（带等待）"},
    {L"recipe_reuse_guards", L"default",
        L"配方复用：Ctrl+Home 只提醒不拦（尊重「每组回到固定区域覆盖填写」的合法用法）；"
        L"已写入后 Ctrl+A 全选要先确认；路线指针按需给一次（Skill + 工具函数，不占系统提示词）"},
    {L"run_command_salvage", L"default",
        L"runCommand 非法 JSON 时从正文恢复命令；空参数仍报错"},
    {L"dom_first_dialog_gate", L"default",
        L"DOM 优先门禁：模态对话框（标题常为空）不放行 clickRef；标题读不出时按窗口类判定"},
    {L"route_nudge", L"default",
        L"路线指针：成批表格数据落缓存时给一次自带命令的 runCommand 提示（非表格不打扰、每任务一次）"},
    {L"hotkey_label_real_keys", L"default",
        L"hotkeyShortcut 描述按实键显示（预设索引与实际组合不符时不再写错成 Ctrl+C）"},
    {L"read_document_tool", L"default",
        L"readDocument：CSV(UTF-8 BOM 中文) / txt 读得对，缺文件与不支持扩展名报可执行错误"},
    {L"computer_alias_tool", L"default",
        L"computer-use 兼容入口：screenshot/点击/拖动/输入/组合键/滚动/长按映射到既有动作，且不绕过网页与表格守卫"},
    {L"locate_cache_hysteresis", L"default",
        L"定位缓存：窗口位移重锚（拖动后仍可用）+ N-of-M 迟滞（抖动画面不点残影）"},
    {L"fuse_locate_candidates", L"default",
        L"多候选聚类取簇心（不平均）：一致候选收敛、分歧不误导、UIA 锚点优先用精确框"
        L"（含单候选 + 名字匹配才采信）"},
    {L"judge_locate_confidence", L"default",
        L"定位置信判决：UIA 确认/可交互点=可用，低特征与分歧=需精炼，单候选=可疑"},
    {L"split_vision_candidate_lines", L"default",
        L"多候选回复按行拆分：去列表前缀、最多 3 行、空行丢弃"},
    {L"bitmap_low_feature", L"default",
        L"框内特征密度：纯色/空白判低特征（拒绝存模板），有纹理不判低"},
    {L"ocr_text_verify", L"default",
        L"OCR 文本核对：只在装了识别引擎且目标是纯短文本时走；未读到/偏差大都能正确处置"},
    {L"game_foreground_vision_allowed", L"default",
        L"桌面游戏/画布页前台：高动态也放行 locateAndClick（无控件树，视觉是唯一手段）；"
        L"浏览器未知页型仍先 observePage；表格软件不算游戏"},
    {L"locate_multi_targets", L"default",
        L"locateAndClick(targets=[…]) 一次定位并连点多个目标（拿卡→放卡）；单目标仍走原路径；"
        L"只给一个 target 又不写 target 时报错"},
    {L"ui_layout_memory_and_grid", L"default",
        L"布局记忆：定位成功一次即记住坐标；窗口身份/分辨率/目标变化即失效；点击无变化即作废；"
        L"网格 (行,列) → 坐标，越界挡住"},
    {L"plan_spend_gate", L"default", L"批量选择**完全无特判**：宿主只做能力不做领域规则；「一键全选」照常定位点击（关键词门槛与提醒均已删除，用户要求不做针对性优化）"},
    {L"ui_layout_signature_gate", L"default", L"布局记忆硬校验：外观签名一致才允许 0 识图直达；界面变了必须拒绝命中"},
    {L"ui_grid_period_detect", L"default",
        L"网格周期检测（自位移平均绝对差）：规则网格检出正确列距；噪声画面与「周期远小于元素自身」都不报周期"},
    {L"ocr_direct_click_pick", L"default",
        L"文字直点：OCR 索引里唯一命中文字标签 → 直接给坐标（省 DOM/UIA/两轮识图）；"
        L"同屏多个同名按钮/整块并成一行/纯数字目标一律拒绝并回落识图"},
    {L"miss_self_correct_prompt", L"default",
        L"错点自纠 prompt 契约：红叉=刚点过的错点 + 只要绝对坐标、明确禁止偏移量（否则准确率打对折）"},
    {L"locate_decimal_coord_parse", L"default",
        L"定位解析必须吃小数：旧实现把 (88.5,117.3) 判成「无法解析」、把 [52.4,…] 静默解析成错框；"
        L"现在点对/框选都按四舍五入取值，反序框仍归一化"},
    {L"plan_spend_budget", L"default",
        L"先算账：卡槽有限时按性价比挑强卡（不一键全选）；预算内算出一次能放几个单位；买不起要明说；maxCount 限量生效"},
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
        && usage.find(L"observePage") != std::wstring::npos
        && usage.find(L"searchOnPage") != std::wstring::npos
        && usage.size() < 3600;
    Emit(L"usage_skill_reply_chain", ok,
        ok ? L"" : L"usage skill missing core openWebpage/saveTaskData/recipe rules");
}

void CaseAgentSkill() {
    const std::wstring skill = LookupMacroActionSchema(L"agent");
    const bool ok = skill.find(L"completeTask") != std::wstring::npos
        && skill.find(L"quickInput") != std::wstring::npos
        && skill.find(L"locateAndClick") != std::wstring::npos
        && skill.find(L"observePage") != std::wstring::npos
        && skill.find(L"clickRef") != std::wstring::npos
        && skill.find(L"typeRef") != std::wstring::npos
        && skill.find(L"searchOnPage") != std::wstring::npos
        && skill.find(L"activateWindow") != std::wstring::npos
        && skill.find(L"resolveSystemPath") != std::wstring::npos
        && skill.find(L"runActionRecipe") != std::wstring::npos
        && skill.find(L"scrollWheel") != std::wstring::npos
        && skill.find(L"勿先滚") != std::wstring::npos
        && skill.find(L"mouseDrag") != std::wstring::npos
        && skill.find(L"clearFirst") != std::wstring::npos
        && skill.size() < 1800;
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

void CaseOpenWebpageApiAndSameSiteGuard() {
    ResetAiActionSessionState(91);
    const bool apiUrl = LooksLikeNonUserFacingWebUrl(
        L"https://api.bilibili.com/x/web-interface/search/type?search_type=bili_user&keyword=x");
    const bool searchOk = !LooksLikeNonUserFacingWebUrl(
        L"https://search.bilibili.com/upuser?keyword=%E6%9C%89%E5%B1%B1");
    const bool spaceOk = !LooksLikeNonUserFacingWebUrl(L"https://space.bilibili.com/123");
    const bool guessed = LooksLikeGuessedUserSpaceUrl(L"https://space.bilibili.com/")
        && LooksLikeGuessedUserSpaceUrl(L"https://space.bilibili.com/123")
        && !LooksLikeGuessedUserSpaceUrl(L"https://www.bilibili.com/")
        && LooksLikeSiteSearchResultsUrl(L"https://search.bilibili.com/all?keyword=x")
        && SanitizeObservePageQuery(L"搜索 有山先生") == L"\u6709\u5c71\u5148\u751f"
        && SanitizeObservePageQuery(L"\u641c\u7d22").empty();
    const bool site = ExtractUrlSiteKey(L"https://search.bilibili.com/upuser?k=a") == L"bilibili.com"
        && ExtractUrlSiteKey(L"https://api.bilibili.com/x/foo") == L"bilibili.com"
        && ExtractUrlSiteKey(L"https://www.example.com/a") == L"example.com";

    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring blocked = CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://api.bilibili.com/x/web-interface/search/type?keyword=x\"}");
    const std::wstring blockedSpace = CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://space.bilibili.com/\"}");
    const std::wstring first = CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://search.bilibili.com/upuser?keyword=test\"}");
    const std::wstring second = CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://www.bilibili.com/\"}");
    ResetAiActionSessionState(92);
    const auto tools2 = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring home = CallNamedTool(tools2, L"openWebpage",
        L"{\"targetPath\":\"https://www.bilibili.com/\"}");
    const std::wstring searchNav = CallNamedTool(tools2, L"openWebpage",
        L"{\"targetPath\":\"https://search.bilibili.com/all?keyword=x\"}");
    const std::wstring biliSearch = BuildSiteSearchUrl(L"\u6709\u5c71\u5148\u751f",
        L"https://www.bilibili.com/");
    const bool searchUrlOk = biliSearch.find(L"search.bilibili.com/all?keyword=") != std::wstring::npos
        && LooksLikeSiteHomepageUrl(L"https://www.bilibili.com/")
        && LooksLikeSiteHomepageUrl(L"https://www.bilibili.com/?spm=1")
        && !LooksLikeSiteHomepageUrl(L"https://search.bilibili.com/all?keyword=x");
    const bool ok = apiUrl && searchOk && spaceOk && guessed && site
        && blocked.find(L"[错误]") == 0
        && blocked.find(L"风控") != std::wstring::npos
        && blockedSpace.find(L"[错误]") == 0
        && first.find(L"[EXECUTED]") != std::wstring::npos
        && second.find(L"已在站点") != std::wstring::npos
        && home.find(L"[EXECUTED]") != std::wstring::npos
        && searchNav.find(L"[EXECUTED]") != std::wstring::npos
        && searchNav.find(L"已在站点") == std::wstring::npos
        && searchUrlOk;
    Emit(L"open_webpage_api_and_same_site", ok,
        ok ? L"" : (blocked + L" | " + first + L" | " + second + L" | " + searchNav
            + L" | " + biliSearch).c_str());
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

void CasePageKindClassify() {
    const bool canvas = ClassifyPageKind(0.82, 2) == PageKind::Canvas;
    const bool mixed = ClassifyPageKind(0.40, 20) == PageKind::Mixed;
    const bool dom = ClassifyPageKind(0.02, 12) == PageKind::Dom;
    const bool overlayKeepsMixed = ClassifyPageKind(0.55, 10) == PageKind::Mixed;
    const bool ok = canvas && mixed && dom && overlayKeepsMixed
        && ParsePageKindName(L"canvas") == PageKind::Canvas
        && std::wstring(PageKindName(PageKind::Dom)) == L"dom"
        && LooksLikePageSnapshotRef(L"e1")
        && LooksLikePageSnapshotRef(L"e12")
        && !LooksLikePageSnapshotRef(L"e0")
        && !LooksLikePageSnapshotRef(L"button");
    Emit(L"page_kind_classify", ok, ok ? L"" : L"classify/ref 不符合阈值");
}

void CasePageSnapshotFormatAndRef() {
    ResetAiActionSessionState(77);
    const std::string json =
        R"({"ok":true,"pageKind":"dom","canvasRatio":0.02,"interactive":2,)"
        R"("skippedHtml":false,"url":"https://example.com/login","title":"登录",)"
        R"("vw":1920,"vh":1080,"nodes":[)"
        R"({"ref":"e1","role":"textbox","name":"手机号","path":"登录","value":"138","x":100,"y":200,"w":280,"h":36},)"
        R"({"ref":"e2","role":"link","name":"日本篇","href":"https://www.bilibili.com/video/BV1GJ4m1w7Qb/","x":10,"y":400,"w":200,"h":80},)"
        R"({"ref":"e3","role":"checkbox","name":"同意","checked":true,"x":400,"y":500,"w":120,"h":40}]})";
    const PageSnapshot snap = ParsePageSnapshotJson(json);
    const std::wstring text = FormatPageSnapshotForAgent(snap);
    const bool parseOk = snap.kind == PageKind::Dom && snap.nodes.size() == 3
        && snap.nodes[0].ref == L"e1"
        && snap.nodes[0].value == L"138"
        && snap.nodes[1].href == L"//www.bilibili.com/video/BV1GJ4m1w7Qb/"
        && snap.nodes[2].checked;
    const bool fmtOk = text.find(L"[dom]") != std::wstring::npos
        && text.find(L"[ref=e1") != std::wstring::npos
        && text.find(L"clickRef") != std::wstring::npos
        && text.find(L"typeRef") != std::wstring::npos
        && text.find(L"=\"138\"") != std::wstring::npos
        && text.find(L"BV1GJ4m1w7Qb") != std::wstring::npos
        && text.find(L"checked") != std::wstring::npos
        && text.find(L"禁止抓 HTML") == std::wstring::npos;
    const bool hostKept = snap.nodes.size() > 1
        && snap.nodes[1].href.find(L"//www.bilibili.com/") == 0;

    const std::string searchJson =
        R"({"ok":true,"pageKind":"dom","url":"https://search.bilibili.com/all?keyword=x",)"
        R"("title":"搜索","nodes":[{"ref":"e1","role":"link","name":"有山先生","href":"//space.bilibili.com/1"}]})";
    const std::wstring searchText = FormatPageSnapshotForAgent(ParsePageSnapshotJson(searchJson));
    const bool searchHint = searchText.find(L"已在搜索结果页") != std::wstring::npos
        && searchText.find(L"space.bilibili.com") != std::wstring::npos;

    const std::string canvasJson =
        R"({"ok":true,"pageKind":"canvas","canvasRatio":0.81,"interactive":1,)"
        R"("skippedHtml":true,"url":"https://cloud.game/play","title":"云游戏",)"
        R"("vw":1920,"vh":1080,"nodes":[]})";
    const std::wstring canvasText = FormatPageSnapshotForAgent(ParsePageSnapshotJson(canvasJson));
    const bool canvasOk = canvasText.find(L"[canvas]") != std::wstring::npos
        && canvasText.find(L"禁止抓 HTML") != std::wstring::npos
        && canvasText.find(L"[ref=") == std::wstring::npos;

    std::string longJson = R"({"ok":true,"pageKind":"dom","nodes":[)";
    for (int i = 1; i <= 80; ++i) {
        if (i > 1) longJson += ",";
        longJson += "{\"ref\":\"e" + std::to_string(i)
            + R"(","role":"button","name":"很长的按钮文案用来撑满快照ABCDEFGHIJKLMN","x":1,"y":2,"w":3,"h":4})";
    }
    longJson += "]}";
    const std::wstring trunc = FormatPageSnapshotForAgent(ParsePageSnapshotJson(longJson), 800);
    const bool truncOk = trunc.size() <= 800 && trunc.find(L"截断") != std::wstring::npos;

    const std::string spaceJson =
        R"({"ok":true,"pageKind":"dom","url":"https://space.bilibili.com/28626598","title":"有山先生",)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"link","name":"主页","href":"https://space.bilibili.com/28626598","x":80,"y":80,"w":40,"h":24},)"
        R"({"ref":"e2","role":"link","name":"一万人投稿","path":"代表作","href":"https://www.bilibili.com/video/BVfeat","x":20,"y":120,"w":400,"h":220},)"
        R"({"ref":"e3","role":"link","name":"日本篇 汉字文化圈","href":"https://www.bilibili.com/video/BV1jp111","x":10,"y":400,"w":180,"h":90},)"
        R"({"ref":"e4","role":"link","name":"硅控赛车冠军","href":"https://www.bilibili.com/video/BV1si111","x":220,"y":400,"w":180,"h":90},)"
        R"({"ref":"e5","role":"link","name":"b站网友写诗","href":"https://www.bilibili.com/video/BV1cy4y1L7vs","x":430,"y":400,"w":180,"h":90},)"
        R"({"ref":"e6","role":"link","name":"页头错误推荐","href":"https://www.bilibili.com/video/BVbad","x":10,"y":80,"w":180,"h":90}]})";
    const std::wstring spaceText = FormatPageSnapshotForAgent(ParsePageSnapshotJson(spaceJson));
    const bool spaceHint = spaceText.find(L"用户空间") != std::wstring::npos
        && spaceText.find(L"searchOnPage") != std::wstring::npos
        && spaceText.find(L"日本篇") != std::wstring::npos
        && spaceText.find(L"列表第1项=e3") != std::wstring::npos
        && spaceText.find(L"第一个视频") == std::wstring::npos;

    const std::string watchJson =
        R"({"ok":true,"pageKind":"mixed","canvasRatio":0.29,"url":"https://www.bilibili.com/video/BV1cy4y1L7vs/","title":"播放",)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"button","name":"分享","x":220,"y":700,"w":48,"h":32},)"
        R"({"ref":"e2","role":"button","name":"点赞","x":40,"y":700,"w":48,"h":32},)"
        R"({"ref":"e3","role":"link","name":"有山先生","href":"https://space.bilibili.com/28626598","x":1400,"y":80,"w":80,"h":24},)"
        R"({"ref":"e4","role":"link","name":"相关推荐甲","href":"https://www.bilibili.com/video/BVrel1","x":10,"y":820,"w":180,"h":90},)"
        R"({"ref":"e5","role":"link","name":"相关推荐乙","href":"https://www.bilibili.com/video/BVrel2","x":220,"y":820,"w":180,"h":90}]})";
    const std::wstring watchText = FormatPageSnapshotForAgent(ParsePageSnapshotJson(watchJson));
    const bool watchHint = watchText.find(L"[mixed]") != std::wstring::npos
        && watchText.find(L"clickRef") != std::wstring::npos
        && watchText.find(L"点赞") != std::wstring::npos
        && watchText.find(L"点赞按钮") == std::wstring::npos
        && watchText.find(L"勿点分享") == std::wstring::npos
        && watchText.find(L"locateAndClick") != std::wstring::npos
        && watchText.find(L"列表第1项") == std::wstring::npos
        && watchText.find(L"点赞=e2") != std::wstring::npos
        && watchText.find(L"工具栏按钮") != std::wstring::npos
        && SnapshotRefMatchingLocateTarget(ParsePageSnapshotJson(watchJson),
            L"\u89c6\u9891\u4e0b\u65b9\u7684\u70b9\u8d5e\u5927\u62c7\u6307") == L"e2";

    const std::string gridJson =
        R"({"ok":true,"pageKind":"dom","vw":1699,"vh":780,"scrollY":0,"pageHeight":2200,)"
        R"("url":"https://example.com/list","title":"列表",)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"link","name":"日本篇汉字文化圈","href":"/video/BV1aa","inView":true,"x":32,"y":240,"w":180,"h":90},)"
        R"({"ref":"e2","role":"link","name":"硬控赛车冠军","href":"/video/BV1cc","inView":true,"x":220,"y":240,"w":180,"h":90},)"
        R"({"ref":"e3","role":"link","name":"更早的投稿标题","href":"/video/BV1bb","inView":false,"x":32,"y":900,"w":180,"h":90}]})";
    const PageSnapshot gridSnap = ParsePageSnapshotJson(gridJson);
    const std::wstring gridText = FormatPageSnapshotForAgent(gridSnap);
    const size_t visHdr = gridText.find(L"可视区:");
    const size_t offHdr = gridText.find(L"屏外:");
    const size_t firstCard = gridText.find(L"日本篇汉字文化圈");
    const size_t laterCard = gridText.find(L"更早的投稿标题");
    const bool gridOk = gridText.find(L"可视2") != std::wstring::npos
        && gridText.find(L"屏外1") != std::wstring::npos
        && gridText.find(L"下") != std::wstring::npos
        && visHdr != std::wstring::npos && offHdr != std::wstring::npos
        && firstCard != std::wstring::npos && laterCard != std::wstring::npos
        && visHdr < firstCard && firstCard < offHdr && offHdr < laterCard
        && CountPageSnapshotInViewMainContent(gridSnap) == 2
        && gridText.find(L"列表第1项=e1") != std::wstring::npos
        && gridText.find(L"第一个视频") == std::wstring::npos;

    const std::string missJson =
        R"({"ok":true,"pageKind":"mixed","query":"点赞","queryHits":0,"nodes":[],)"
        R"("url":"https://www.bilibili.com/video/BV1xx","title":"播放"})";
    const std::wstring missText = FormatPageSnapshotForAgent(ParsePageSnapshotJson(missJson));
    const bool queryMiss = missText.find(L"树上无") != std::wstring::npos
        && missText.find(L"点赞") != std::wstring::npos
        && missText.find(L"locateAndClick") != std::wstring::npos;

    Emit(L"page_snapshot_format_and_ref",
        parseOk && fmtOk && canvasOk && truncOk && searchHint && hostKept && spaceHint && watchHint && gridOk && queryMiss,
        (parseOk && fmtOk && canvasOk && truncOk && searchHint && hostKept && spaceHint && watchHint && gridOk && queryMiss) ? L""
            : (L"href=" + (snap.nodes.size() > 1 ? snap.nodes[1].href : L"?")
                + L" search=" + searchText.substr(0, 120)
                + L" space=" + spaceText.substr(0, 180)
                + L" grid=" + gridText.substr(0, 220)
                + L" miss=" + missText.substr(0, 180)).c_str());
}

void CaseScrollWheelConfirmWhenViewportHasContent() {
    ResetAiActionSessionState(814);
    const std::string json =
        R"({"ok":true,"pageKind":"dom","vw":1699,"vh":780,)"
        R"("url":"https://example.com/cards","title":"列表",)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"link","name":"卡片甲标题","href":"/a","inView":true,"x":10,"y":240,"w":180,"h":90},)"
        R"({"ref":"e2","role":"link","name":"卡片乙标题","href":"/b","inView":true,"x":200,"y":240,"w":180,"h":90},)"
        R"({"ref":"e3","role":"link","name":"卡片丙标题","href":"/c","inView":true,"x":390,"y":240,"w":180,"h":90}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(json));
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring blocked = CallNamedTool(tools, L"scrollWheel",
        L"{\"scrollDirection\":1,\"scrollSteps\":5}");
    const bool blockedOk = blocked.rfind(L"[错误]", 0) == 0
        && blocked.find(L"confirmScroll") != std::wstring::npos
        && blocked.find(L"clickRef") != std::wstring::npos;
    const std::wstring allowed = CallNamedTool(tools, L"scrollWheel",
        L"{\"scrollDirection\":1,\"scrollSteps\":5,\"confirmScroll\":true}");
    const bool confirmOk = allowed.find(L"[错误]") == std::wstring::npos;

    ResetAiActionSessionState(815);
    AiNotePageKind(L"dom");
    const auto tools2 = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring empty = CallNamedTool(tools2, L"scrollWheel",
        L"{\"scrollDirection\":1,\"scrollSteps\":3}");
    const bool emptyOk = empty.find(L"[错误]") == std::wstring::npos;

    const bool ok = blockedOk && confirmOk && emptyOk;
    Emit(L"scroll_wheel_confirm_when_viewport_has_content", ok,
        ok ? L"" : (L"blocked=" + blocked.substr(0, 160) + L" | allowed=" + allowed.substr(0, 80)
            + L" | empty=" + empty.substr(0, 80)).c_str());
}

void CaseListFirstBlocksScrollAndOtherCards() {
    ResetAiActionSessionState(818);
    const std::string gridJson =
        R"({"ok":true,"pageKind":"dom","vw":1699,"vh":780,)"
        R"("url":"https://example.com/upload/video","title":"投稿",)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"link","name":"最上最左投稿","href":"/video/BV1aa","inView":true,"x":32,"y":240,"w":180,"h":90},)"
        R"({"ref":"e5","role":"link","name":"后面一张投稿","href":"/video/BV1ee","inView":true,"x":220,"y":240,"w":180,"h":90},)"
        R"({"ref":"e7","role":"link","name":"投稿","href":"/upload/video","inView":true,"x":80,"y":80,"w":48,"h":24}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(gridJson));
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring confirmBlocked = CallNamedTool(tools, L"scrollWheel",
        L"{\"scrollDirection\":1,\"scrollSteps\":6,\"confirmScroll\":true}");
    const bool noMindlessScroll = confirmBlocked.rfind(L"[错误]", 0) == 0
        && confirmBlocked.find(L"列表第1项") != std::wstring::npos
        && confirmBlocked.find(L"e1") != std::wstring::npos;
    const std::wstring otherCard = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e5\"}");
    const bool otherBlocked = otherCard.rfind(L"[错误]", 0) == 0
        && otherCard.find(L"e1") != std::wstring::npos
        && otherCard.find(L"e5") != std::wstring::npos;
    const std::wstring firstCard = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e1\"}");
    const bool firstNotListBan = firstCard.find(L"不要点") == std::wstring::npos
        && firstCard.find(L"另一张卡") == std::wstring::npos;
    const std::wstring tab = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e7\"}");
    const bool tabNotListBan = tab.find(L"另一张卡") == std::wstring::npos;

    const std::string watchJson =
        R"({"ok":true,"pageKind":"mixed","url":"https://www.bilibili.com/video/BV1aa/","title":"播放",)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"link","name":"相关甲","href":"/video/BVrel1","inView":true,"x":10,"y":820,"w":180,"h":90},)"
        R"({"ref":"e2","role":"link","name":"相关乙","href":"/video/BVrel2","inView":true,"x":220,"y":820,"w":180,"h":90},)"
        R"({"ref":"e3","role":"button","name":"点赞","x":40,"y":700,"w":48,"h":32}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(watchJson));
    const std::wstring watchFmt = FormatPageSnapshotForAgent(ParsePageSnapshotJson(watchJson));
    const bool watchNoListFirst = watchFmt.find(L"列表第1项") == std::wstring::npos;
    const std::wstring like = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e3\"}");
    const bool likeNotListBan = like.find(L"另一张卡") == std::wstring::npos;
    const std::wstring watchScroll = CallNamedTool(tools, L"scrollWheel",
        L"{\"scrollDirection\":1,\"scrollSteps\":3,\"confirmScroll\":true}");
    const bool watchScrollOk = watchScroll.find(L"列表第1项") == std::wstring::npos;

    const bool ok = noMindlessScroll && otherBlocked && firstNotListBan && tabNotListBan
        && watchNoListFirst && likeNotListBan && watchScrollOk;
    Emit(L"list_first_blocks_scroll_and_other_cards", ok,
        ok ? L"" : (L"scroll=" + confirmBlocked.substr(0, 140)
            + L" | e5=" + otherCard.substr(0, 120)
            + L" | e1=" + firstCard.substr(0, 80)
            + L" | e7=" + tab.substr(0, 80)
            + L" | like=" + like.substr(0, 80)
            + L" | wscroll=" + watchScroll.substr(0, 80)).c_str());
}

void CaseWebBrowseAllowsVisionFallback() {
    ResetAiActionSessionState(816);
    AiNoteWebBrowseSession(true);
    SetAiActionPlanGateEnabled(false);
    // 「前台是浏览器」必须固定：本用例断言网页守卫生效，而该守卫读真实前台窗口，
    // 不固定就会随测试机当前前台而随机红/绿
    SetAiForegroundBrowserOverrideForTest(1);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring loc = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\u9875\u9762\u9876\u90e8\u641c\u7d22\u6846\"}");
    const bool locNotWebBan = loc.find(L"\u6b63\u5728\u6d4f\u89c8\u7f51\u9875") == std::wstring::npos
        && loc.find(L"\u7981\u6b62 locateAndClick") == std::wstring::npos;
    const std::wstring enter = CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"Enter\"}");
    const bool enterNoExtOk = enter.find(L"searchOnPage") == std::wstring::npos;

    AiNotePageKind(L"dom");
    const std::wstring enter2 = CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"Enter\"}");
    const bool enterWithTree = enter2.find(L"searchOnPage") != std::wstring::npos;

    ResetAiActionSessionState(817);
    SetAiActionPlanGateEnabled(false);
    const auto tools2 = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring desk = CallNamedTool(tools2, L"locateAndClick",
        L"{\"target\":\"\u684c\u9762\u56fe\u6807\"}");
    const bool deskNotWeb = desk.find(L"\u6d4f\u89c8\u7f51\u9875") == std::wstring::npos;

    const bool ok = locNotWebBan && enterNoExtOk && enterWithTree && deskNotWeb;
    Emit(L"web_browse_allows_vision_fallback", ok,
        ok ? L"" : (L"loc=" + loc.substr(0, 160) + L" | enter=" + enter.substr(0, 80)
            + L" | enter2=" + enter2.substr(0, 80) + L" | desk=" + desk.substr(0, 80)).c_str());
}

void CaseWebMixedPrefersTreeClick() {
    ResetAiActionSessionState(819);
    SetAiActionPlanGateEnabled(false);
    SetAiForegroundBrowserOverrideForTest(1);   // 断言网页守卫（mouseClick 拒绝等）生效
    const std::string watchJson =
        R"({"ok":true,"pageKind":"mixed","canvasRatio":0.35,)"
        R"("url":"https://www.bilibili.com/video/BV1aa/","title":"播放","nodes":[)"
        R"({"ref":"e1","role":"button","name":"分享","x":220,"y":700,"w":48,"h":32},)"
        R"({"ref":"e2","role":"button","name":"点赞","x":40,"y":700,"w":48,"h":32}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(watchJson));
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring loc = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\u89c6\u9891\u4e0b\u65b9\u7684\u70b9\u8d5e\u5927\u62c7\u6307\"}");
    const bool locToRef = loc.rfind(L"[错误]", 0) == 0
        && loc.find(L"clickRef(e2)") != std::wstring::npos;
    const std::wstring click = CallNamedTool(tools, L"mouseClick",
        L"{\"x\":54,\"y\":776}");
    const bool noGuess = click.rfind(L"[错误]", 0) == 0
        && click.find(L"clickRef") != std::wstring::npos;

    AiNoteLocateFailed();
    const std::wstring fakeDone = CallNamedTool(tools, L"completeTask",
        L"{\"reason\":\"\u5df2\u70b9\u8d5e\"}");
    const bool rejectFake = fakeDone.rfind(L"[错误]", 0) == 0
        && fakeDone.find(L"\u8bc6\u56fe\u672a\u627e\u5230") != std::wstring::npos;
    const std::wstring honest = CallNamedTool(tools, L"completeTask",
        L"{\"reason\":\"\u672a\u627e\u5230\u76ee\u6807\"}");
    const bool allowFail = honest.find(L"[TASK_COMPLETE]") != std::wstring::npos;

    const bool ok = locToRef && noGuess && rejectFake && allowFail;
    Emit(L"web_mixed_prefers_tree_click", ok,
        ok ? L"" : (L"loc=" + loc.substr(0, 120) + L" | click=" + click.substr(0, 80)
            + L" | fake=" + fakeDone.substr(0, 80) + L" | honest=" + honest.substr(0, 80)).c_str());
    ResetAiActionSessionState(819);
}

void CaseLogicConvertBridgeNav() {
    AiLogicConvertSessionBegin(true, L"LogicWeb", L"open and like", L"", 1, false);
    AiLogicConvertNoteOpenWebpage(L"https://space.bilibili.com/28626598");
    AiLogicConvertNoteLocate(L"\u70b9\u8d5e", 100, 200, L"left", 1, L"images\\like.bmp");
    const auto& tr = AiLogicConvertTranscript();
    bool recOk = tr.size() >= 2
        && tr[0].action.type == ActionType::OpenWebpage
        && tr[0].action.targetPath.find(L"space.bilibili.com") != std::wstring::npos
        && tr[1].fromLocate && tr[1].locateTarget == L"\u70b9\u8d5e";
    AiLogicConvertCompileInput in;
    in.taskPrompt = L"open and like";
    in.transcript = tr;
    const auto compiled = CompileAiLogicConvert(in);
    bool hasWeb = false, hasFind = false;
    for (const auto& a : compiled.defineAndBody) {
        if (a.type == ActionType::OpenWebpage
            && a.targetPath.find(L"space.bilibili.com") != std::wstring::npos) hasWeb = true;
        if (a.type == ActionType::FindImage) hasFind = true;
    }
    AiLogicConvertSessionEnd();
    const bool ok = recOk && compiled.ok && hasWeb && hasFind;
    Emit(L"logic_convert_bridge_nav", ok,
        ok ? L"" : (compiled.ok ? L"missing openWebpage/findImage" : compiled.error).c_str());
}

void CaseObservePageToolsPresent() {
    ResetAiActionSessionState(78);
    SetAiForegroundBrowserOverrideForTest(1);   // 断言网页守卫（mouseClick/Enter 拒绝）生效
    AiActionHostHooks hooks;
    hooks.onObservePage = [](bool, const std::wstring&, const std::wstring&) -> std::wstring {
        AiNotePageKind(L"dom");
        return L"[dom] 登录 | https://example.com | 800x600 canvas=0.01 n=1\n"
            L"优先 clickRef / typeRef；失败再用 locateAndClick。旧 ref 已作废。\n"
            L"- button \"进入游戏\" [ref=e1 @10,20]\n";
    };
    hooks.onClickRef = [](const std::wstring& ref, bool) -> std::wstring {
        return L"已 clickRef(" + ref + L")";
    };
    hooks.onTypeRef = [](const std::wstring& ref, const std::wstring&, bool, bool) -> std::wstring {
        return L"已 typeRef(" + ref + L")";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    bool hasObs = false, hasClick = false, hasType = false, hasSearchPage = false;
    for (const auto& t : tools) {
        if (t.name == L"observePage") hasObs = true;
        if (t.name == L"clickRef") hasClick = true;
        if (t.name == L"typeRef") hasType = true;
        if (t.name == L"searchOnPage") hasSearchPage = true;
    }
    const std::wstring obs = CallNamedTool(tools, L"observePage", L"{}");
    const bool obsOk = obs.find(L"[dom]") != std::wstring::npos
        && obs.find(L"[ref=e1") != std::wstring::npos;
    AiNotePageKind(L"dom");
    const std::wstring clicked = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e1\"}");
    const bool clickOk = IsSubmitSkipObserveToolResult(clicked)
        && clicked.find(L"e1") != std::wstring::npos;
    const std::wstring typed = CallNamedTool(tools, L"typeRef",
        L"{\"ref\":\"e1\",\"text\":\"13800138000\"}");
    const bool typeOk = IsSubmitSkipObserveToolResult(typed) && typed.find(L"e1") != std::wstring::npos;
    const std::wstring emptyType = CallNamedTool(tools, L"typeRef", L"{\"ref\":\"e1\",\"text\":\"\"}");
    const std::wstring badRef = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"button\"}");
    const std::wstring mcDom = CallNamedTool(tools, L"mouseClick", L"{\"x\":10,\"y\":10}");
    const std::wstring locDom = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\\u7b2c\\u4e00\\u4e2a\\u89c6\\u9891\"}");
    AiNotePageUrl(L"https://search.bilibili.com/all?keyword=x");
    const std::wstring typeAgain = CallNamedTool(tools, L"typeRef",
        L"{\"ref\":\"e1\",\"text\":\"again\"}");
    const std::wstring enterDom = CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"Enter\"}");
    const bool visionBlocked = mcDom.rfind(L"[错误]", 0) == 0
        && mcDom.find(L"clickRef") != std::wstring::npos
        && locDom.find(L"\u6b63\u5728\u6d4f\u89c8\u7f51\u9875") == std::wstring::npos
        && typeAgain.find(L"\u641c\u7d22\u7ed3\u679c") != std::wstring::npos
        && enterDom.find(L"typeRef") != std::wstring::npos;
    AiNotePageKind(L"canvas");
    const std::wstring skipHtml = CallNamedTool(tools, L"observePage", L"{}");
    const std::wstring canvasClick = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e1\"}");
    const std::wstring canvasType = CallNamedTool(tools, L"typeRef",
        L"{\"ref\":\"e1\",\"text\":\"x\"}");
    const std::wstring force = CallNamedTool(tools, L"observePage", L"{\"force\":true}");
    const bool canvasGate = skipHtml.find(L"禁止再抓 HTML") != std::wstring::npos
        && canvasClick.find(L"禁止 clickRef") != std::wstring::npos
        && canvasType.find(L"禁止 typeRef") != std::wstring::npos
        && force.find(L"[dom]") != std::wstring::npos;
    const bool runFlags = IsMacroActionRunToolName(L"clickRef")
        && IsMacroActionRunToolName(L"typeRef")
        && IsMacroActionRunToolName(L"searchOnPage")
        && !IsMacroActionRunToolName(L"observePage")
        && IsMacroExecutionToolName(L"observePage");
    ResetAiActionSessionState(78);
    const bool cleared = AiLastPageKind().empty();
    const bool ok = hasObs && hasClick && hasType && hasSearchPage && obsOk && clickOk && typeOk
        && emptyType.find(L"[错误]") == 0
        && badRef.find(L"[错误]") == 0
        && visionBlocked
        && canvasGate && runFlags && cleared;
    Emit(L"observe_page_tools_present", ok,
        ok ? L"" : (obs + L" | " + clicked + L" | " + skipHtml).c_str());
}

void CaseSearchOnPageTool() {
    ResetAiActionSessionState(79);
    std::wstring navUrl;
    std::wstring navQuery;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) -> std::wstring { return L"opened"; };
    hooks.onNavigatePage = [&](const std::wstring& url, const std::wstring& query) -> std::wstring {
        navUrl = url;
        navQuery = query;
        AiNotePageKind(L"dom");
        AiNotePageUrl(url);
        return L"[dom] 搜索 | " + url + L" | 800x600 canvas=0.00 n=1\n"
            L"- link \"\u6709\u5c71\u5148\u751f\" href=/space/1 [ref=e1 @10,20]\n";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring needSite = CallNamedTool(tools, L"searchOnPage",
        L"{\"query\":\"\u6709\u5c71\u5148\u751f\"}");
    const bool needSiteOk = needSite.find(L"[错误]") == 0
        && needSite.find(L"openWebpage") != std::wstring::npos;
    const std::wstring opened = CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://www.bilibili.com/\",\"observeAfter\":false}");
    const std::wstring searched = CallNamedTool(tools, L"searchOnPage",
        L"{\"query\":\"\u6709\u5c71\u5148\u751f\"}");
    const bool navOk = opened.find(L"[EXECUTED]") != std::wstring::npos
        && searched.find(L"[EXECUTED]") != std::wstring::npos
        && IsSubmitSkipObserveToolResult(searched)
        && navUrl.find(L"search.bilibili.com/all?keyword=") != std::wstring::npos
        && navQuery.find(L"\u6709\u5c71") != std::wstring::npos;
    const std::wstring again = CallNamedTool(tools, L"searchOnPage",
        L"{\"query\":\"\u6709\u5c71\u5148\u751f\"}");
    const bool dupOk = again.find(L"搜索结果") != std::wstring::npos;
    AiActionToolOptions fillOpts;
    fillOpts.fillTableOnly = true;
    const auto fillTools = BuildAiActionExecuteTools(nullptr, fillOpts);
    bool fillHasSearch = false;
    for (const auto& t : fillTools) {
        if (t.name == L"searchOnPage") fillHasSearch = true;
    }
    const bool ok = needSiteOk && navOk && dupOk && !fillHasSearch;
    Emit(L"search_on_page_tool", ok,
        ok ? L"" : (needSite + L" | " + searched + L" | " + navUrl + L" | " + again).c_str());
    ResetAiActionSessionState(79);
}

void CaseClickRefNavigatesSpaceHref() {
    ResetAiActionSessionState(810);
    const std::wstring searchUrl = L"https://search.bilibili.com/all?keyword=x";
    const std::wstring spaceUrl = L"https://space.bilibili.com/28626598";
    const std::wstring rSpace = ResolvePageNavigationUrl(
        L"//space.bilibili.com/28626598", searchUrl);
    const std::wstring rAll = ResolvePageNavigationUrl(L"/all?vt=1", searchUrl);
    const std::wstring rBare = ResolvePageNavigationUrl(L"/28626598", searchUrl);
    const std::wstring rSlashSpace = ResolvePageNavigationUrl(L"/space/28626598", searchUrl);
    const bool resolveOk =
        rSpace == spaceUrl
        && rAll.empty()
        && rBare.empty()
        && rSlashSpace == spaceUrl
        && PageUrlsSameDocument(searchUrl, L"https://search.bilibili.com/all?vt=9&keyword=x")
        && !PageUrlsSameDocument(searchUrl, spaceUrl)
        && LooksLikeEmptyUserSpaceUrl(L"https://space.bilibili.com/")
        && LooksLikeGuessedUserSpaceUrl(spaceUrl)
        && ShouldDirectNavigateSnapshotHref(spaceUrl)
        && !ShouldDirectNavigateSnapshotHref(L"https://www.bilibili.com/video/BV1aa");

    std::wstring navUrl;
    int clickCount = 0;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) -> std::wstring { return L"ok"; };
    hooks.onNavigatePage = [&](const std::wstring& url, const std::wstring&) {
        navUrl = url;
        AiNotePageKind(L"dom");
        AiNotePageUrl(url);
        return L"[dom] space | " + url + L"\n- link \"v\" href=//www.bilibili.com/video/BV1 [ref=e1]\n";
    };
    hooks.onClickRef = [&](const std::wstring& ref, bool) {
        ++clickCount;
        return L"clicked " + ref;
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    const std::string searchJson =
        R"({"ok":true,"pageKind":"dom","url":"https://search.bilibili.com/all?keyword=x",)"
        R"("title":"搜索","nodes":[)"
        R"({"ref":"e1","role":"link","name":"有山先生","href":"//space.bilibili.com/28626598"},)"
        R"({"ref":"e2","role":"tab","name":"综合","href":"https://search.bilibili.com/all?keyword=x"},)"
        R"({"ref":"e3","role":"tab","name":"用户","href":"https://search.bilibili.com/all?vt=1"}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(searchJson));
    AiNotePageKind(L"dom");

    CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://www.bilibili.com/\"}");
    AiNotePageUrl(searchUrl);
    const std::wstring spaceOpen = CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://space.bilibili.com/28626598\"}");
    const bool spaceFromSearch = spaceOpen.find(L"[EXECUTED]") != std::wstring::npos
        && spaceOpen.find(L"猜") == std::wstring::npos
        && spaceOpen.find(L"已在站点") == std::wstring::npos;

    navUrl.clear();
    const std::wstring clicked = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e1\"}");
    const bool navClick = clickCount == 0
        && navUrl.find(L"space.bilibili.com/28626598") != std::wstring::npos
        && clicked.find(L"打开") != std::wstring::npos;

    ResetAiActionSessionState(811);
    navUrl.clear();
    clickCount = 0;
    AiNotePageSnapshot(ParsePageSnapshotJson(searchJson));
    AiNotePageKind(L"dom");
    const std::wstring tab1 = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e2\"}");
    const std::wstring tab2 = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e3\"}");
    const bool streakNav = clickCount == 2
        && tab1.find(L"页面未跳转") != std::wstring::npos
        && tab2.find(L"打开") != std::wstring::npos
        && navUrl.find(L"space.bilibili.com/28626598") != std::wstring::npos;

    ResetAiActionSessionState(813);
    navUrl.clear();
    clickCount = 0;
    const std::string listingJson =
        R"({"ok":true,"pageKind":"dom","url":"https://space.bilibili.com/1","nodes":[)"
        R"({"ref":"e1","role":"link","name":"稿件甲","href":"https://www.bilibili.com/video/BV1aa","inView":true,"x":10,"y":400,"w":180,"h":90}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(listingJson));
    AiNotePageKind(L"dom");
    const std::wstring videoClick = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e1\"}");
    const bool videoClicksElement = clickCount == 1 && navUrl.empty()
        && videoClick.find(L"clicked e1") != std::wstring::npos;

    ResetAiActionSessionState(812);
    AiNotePageUrl(L"https://www.bilibili.com/");
    const auto tools2 = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring guessed = CallNamedTool(tools2, L"openWebpage",
        L"{\"targetPath\":\"https://space.bilibili.com/28626598\"}");
    const bool stillBlockGuess = guessed.find(L"[错误]") == 0;

    AiActionToolOptions opts;
    opts.allowAbsolutePointer = false;
    const auto wheelTools = BuildAiActionExecuteTools(nullptr, opts);
    const std::wstring wheel = CallNamedTool(wheelTools, L"scrollWheel",
        L"{\"scrollDirection\":0,\"x\":10,\"y\":20}");
    const bool wheelOk = wheel.find(L"[EXECUTED]") != std::wstring::npos
        && wheel.find(L"绝对坐标") == std::wstring::npos;

    const bool ok = resolveOk && spaceFromSearch && navClick && streakNav
        && videoClicksElement && stillBlockGuess && wheelOk;
    std::wstring flags = L" f=";
    flags.push_back(resolveOk ? L'1' : L'0');
    flags.push_back(spaceFromSearch ? L'1' : L'0');
    flags.push_back(navClick ? L'1' : L'0');
    flags.push_back(streakNav ? L'1' : L'0');
    flags.push_back(videoClicksElement ? L'1' : L'0');
    flags.push_back(stillBlockGuess ? L'1' : L'0');
    flags.push_back(wheelOk ? L'1' : L'0');
    Emit(L"click_ref_navigates_space_href", ok,
        ok ? L"" : (flags + L" rSpace=" + rSpace + L" rAll=[" + rAll + L"] rBare=[" + rBare
            + L"] rSlash=" + rSlashSpace
            + L" | " + spaceOpen + L" | " + clicked + L" | " + tab1 + L" | " + tab2
            + L" | " + videoClick + L" | " + guessed + L" | " + wheel).c_str());
    ResetAiActionSessionState(810);
}

void CaseSamePageClickHintScoped() {
    ResetAiActionSessionState(811);
    int clickCount = 0;
    std::wstring navUrl;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) -> std::wstring { return L"ok"; };
    hooks.onNavigatePage = [&](const std::wstring& url, const std::wstring&) {
        navUrl = url;
        AiNotePageUrl(url);
        return L"[dom] left " + url;
    };
    hooks.onClickRef = [&](const std::wstring& ref, bool) {
        ++clickCount;
        return L"clicked " + ref;
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    const std::string watchJson =
        R"({"ok":true,"pageKind":"mixed","canvasRatio":0.29,)"
        R"("url":"https://www.bilibili.com/video/BV1i5tU6NEAd/",)"
        R"("title":"播放","nodes":[)"
        R"({"ref":"e1","role":"button","name":"分享","x":220,"y":700,"w":48,"h":32},)"
        R"({"ref":"e2","role":"button","name":"点赞","x":40,"y":700,"w":48,"h":32},)"
        R"({"ref":"e3","role":"link","name":"有山先生","href":"https://space.bilibili.com/28626598","x":1400,"y":80,"w":80,"h":24}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(watchJson));
    const std::wstring like = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e2\"}");
    const bool likeOk = like.find(L"[错误]") != 0
        && clickCount == 1
        && like.find(L"请点 href 含 space.bilibili.com") == std::wstring::npos
        && like.find(L"页内按钮") != std::wstring::npos;
    const std::wstring up = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e3\"}");
    const bool upNav = up.find(L"[错误]") != 0
        && navUrl.find(L"space.bilibili.com") != std::wstring::npos;

    ResetAiActionSessionState(812);
    clickCount = 0;
    navUrl.clear();
    const std::string searchJson =
        R"({"ok":true,"pageKind":"dom","url":"https://search.bilibili.com/all?keyword=x",)"
        R"("title":"搜索","nodes":[)"
        R"({"ref":"e1","role":"tab","name":"综合","href":"https://search.bilibili.com/all?keyword=x"},)"
        R"({"ref":"e2","role":"link","name":"有山先生","href":"//space.bilibili.com/28626598"}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(searchJson));
    const std::wstring tab = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e1\"}");
    const bool searchHint = tab.find(L"请点 href 含 space.bilibili.com") != std::wstring::npos;

    const bool ok = likeOk && upNav && searchHint;
    Emit(L"same_page_click_hint_scoped", ok,
        ok ? L"" : (like + L" | " + up + L" | " + tab).c_str());
    ResetAiActionSessionState(811);
}

void CaseSearchOnPageOpensMatchingSpace() {
    ResetAiActionSessionState(813);
    std::vector<std::wstring> navs;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) -> std::wstring { return L"ok"; };
    hooks.onNavigatePage = [&](const std::wstring& url, const std::wstring&) -> std::wstring {
        navs.push_back(url);
        if (url.find(L"search.bilibili.com") != std::wstring::npos) {
            const std::string json =
                R"({"ok":true,"pageKind":"dom","url":"https://search.bilibili.com/all?keyword=x",)"
                R"("title":"搜索","nodes":[)"
                R"({"ref":"e1","role":"link","name":"\u6709\u5c71\u5148\u751f","href":"//space.bilibili.com/28626598"},)"
                R"({"ref":"e2","role":"tab","name":"综合","href":"/all"}]})";
            AiNotePageSnapshot(ParsePageSnapshotJson(json));
            return L"[dom] search\n- link \"\u6709\u5c71\u5148\u751f\" [ref=e1]\n";
        }
        AiNotePageKind(L"dom");
        AiNotePageUrl(url);
        return L"[dom] space | " + url + L"\n";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://www.bilibili.com/\"}");
    const std::wstring searched = CallNamedTool(tools, L"searchOnPage",
        L"{\"query\":\"\u6709\u5c71\u5148\u751f\"}");
    const bool ok = navs.size() >= 2
        && navs[0].find(L"search.bilibili.com") != std::wstring::npos
        && navs[1].find(L"space.bilibili.com/28626598") != std::wstring::npos
        && searched.find(L"\u5339\u914d\u7ed3\u679c") != std::wstring::npos
        && searched.find(L"space.bilibili.com/28626598") != std::wstring::npos;
    Emit(L"search_on_page_opens_matching_space", ok,
        ok ? L"" : (searched + L" navs=" + std::to_wstring(navs.size())).c_str());
    ResetAiActionSessionState(813);
}

void CaseObserveResultDefaults() {
    AiObserveCaptureResult r;
    const bool ok = !r.ok && !r.unchanged && r.base64.empty()
        && r.matchScore < 0
        && std::wstring(kAiObsImageVarName) == L"aiObs";
    Emit(L"observe_result_defaults", ok, ok ? L"" : L"AiObserveCaptureResult / aiObs constants");
}

void CaseWaitRequestsObserve() {
    // 等待就是为了看新界面：wait 必须要求观察；短 wait 默认跳过（不烧一轮），长 wait 或 confirmWait 才放行
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring blocked = CallNamedTool(tools, L"wait", L"{\"duration\":0.01}");
    const bool shortSkipped = IsSubmitSkipObserveToolResult(blocked)
        && blocked.find(L"已跳过") != std::wstring::npos;
    const std::wstring r = CallNamedTool(tools, L"wait",
        L"{\"duration\":0.01,\"confirmWait\":true}");
    const bool ok = shortSkipped && IsSubmitObserveToolResult(r)
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
    ResetAiActionSessionState(878000);
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
    SetAiForegroundBrowserOverrideForTest(1);   // 「已在网页标签内禁止开新标签」要生效
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
    AiNotePageKind(L"mixed");
    const std::wstring ctrlT = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"t\",\"holdLeftCtrl\":true,\"confirmShortcut\":true,\"observeAfter\":false}");
    const bool noNewTab = ctrlT.find(L"[错误]") != std::wstring::npos
        && ctrlT.find(L"新标签") != std::wstring::npos
        && ctrlT.find(L"[EXECUTED]") == std::wstring::npos;
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
    const bool ok = blocked && confirmed && universal && skillClean && noNewTab;
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
    // note 里写 goal: 且不带 section，也应解锁
    ResetAiActionSessionState(424244);
    SetAiActionPlanGateEnabled(true);
    const auto tools3 = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring prefixMemo = CallNamedTool(tools3, L"updateTaskMemo",
        L"{\"note\":\"goal: \u6253\u5f00B\u7ad9\\ntodos:\\n1) searchOnPage\"}");
    const bool prefixOk = prefixMemo.find(L"[错误]") == std::wstring::npos
        && AiActionPlanGateIsOpen();
    SetAiActionPlanGateEnabled(false);
    const bool ok = blockedOk && memoOk && allowedOk && listOk && prefixOk;
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
    // ★这条闸只针对「网页内容在动」：必须显式声明前台是浏览器窗口，
    //   否则（桌面游戏/自绘应用）没有控件树可退，视觉是唯一手段，绝不能拦。
    SetAiForegroundBrowserOverrideForTest(1);
    NoteAiActionUiBusy(0.20, true);
    const bool busy = AiActionUiTooBusyForVisionLocate();
    const bool skipLa = AiActionShouldSkipLookahead();
    const auto tools2 = BuildAiActionExecuteTools(nullptr, {});
    // 计划门闩关：直接测 busy locate
    SetAiActionPlanGateEnabled(false);
    const std::wstring loc = CallNamedTool(tools2, L"locateAndClick",
        L"{\"target\":\"\u67e5\u770b\u66f4\u591a\"}");
    const bool locBlocked = loc.find(L"[错误]") != std::wstring::npos
        && loc.find(L"动态") != std::wstring::npos
        && loc.find(L"Ctrl+T") == std::wstring::npos
        && loc.find(L"Ctrl+t") == std::wstring::npos;

    ResetAiActionSessionState(454547);
    AiNotePageKind(L"mixed");
    NoteAiActionUiBusy(0.20, true);
    SetAiActionPlanGateEnabled(false);
    const auto tools3 = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring like = CallNamedTool(tools3, L"locateAndClick",
        L"{\"target\":\"\u89c6\u9891\u4e0b\u65b9\u5de6\u4fa7\u5927\u62c7\u6307\u5411\u4e0a\u7684\u70b9\u8d5e\u6309\u94ae\"}");
    const bool likeFallback = like.find(L"\u52a8\u6001\u5e72\u6270") == std::wstring::npos
        && like.find(L"\u6b63\u5728\u6d4f\u89c8\u7f51\u9875") == std::wstring::npos
        && like.find(L"Ctrl+T") == std::wstring::npos;

    const bool ok = histRejected && descClean && busy && skipLa && locBlocked && likeFallback;
    Emit(L"history_sidebar_and_busy_guards", ok,
        ok ? L"" : L"history/busy host guards failed");
}

void CaseLocateMultiTargets() {
    // 「拿卡→放卡」这类两步操作要能一次调用做完（一次识图定位 + 立即连点），
    // 而不是发两次 tool call（每次都要主模型一轮 + 1.5~2.6s 界面稳定等待）。
    bool ok = true;
    std::wstring detail;
    ResetAiActionSessionState(484848);
    SetAiForegroundBrowserOverrideForTest(-1);
    SetAiSpreadsheetForegroundOverrideForTest(0);
    SetAiActionPlanGateEnabled(false);

    AiActionHostHooks hooks;
    std::vector<std::wstring> got;
    std::wstring gotButton;
    int gotClicks = 0;
    hooks.onLocateMulti = [&](const std::vector<std::wstring>& targets,
                              const std::wstring& button, int clickCount) -> std::wstring {
        got = targets;
        gotButton = button;
        gotClicks = clickCount;
        return L"[1/2] 已点击卡片\n[2/2] 已点击草坪";
    };
    int singleCalls = 0;
    hooks.onLocateAndClick = [&](const std::wstring&, int, const std::wstring&, int) -> std::wstring {
        ++singleCalls;
        return L"locateAndClick 已点击屏幕(10,20)";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});

    const std::wstring multi = CallNamedTool(tools, L"locateAndClick",
        L"{\"targets\":[\"\u50f5\u5c38\u5361\",\"\u7b2c3\u884c\u8349\u576a\"]}");
    const bool multiOk = got.size() == 2 && got[0] == L"\u50f5\u5c38\u5361"
        && got[1] == L"\u7b2c3\u884c\u8349\u576a" && singleCalls == 0
        && multi.find(L"[1/2]") != std::wstring::npos;
    ok = ok && multiOk;
    if (!multiOk) detail += L" multi";

    // 单目标仍然走原路径
    const std::wstring one = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\u786e\u5b9a\"}");
    const bool oneOk = singleCalls == 1 && one.find(L"10,20") != std::wstring::npos;
    ok = ok && oneOk;
    if (!oneOk) detail += L" single";

    // targets 只有一项、又没有 target → 明确报错，不静默什么都不做
    const std::wstring bad = CallNamedTool(tools, L"locateAndClick",
        L"{\"targets\":[\"\u786e\u5b9a\"]}");
    const bool badOk = bad.find(L"[错误]") != std::wstring::npos;
    ok = ok && badOk;
    if (!badOk) detail += L" bad";

    Emit(L"locate_multi_targets", ok, detail.c_str());
}

void CaseUiLayoutMemoryAndGrid() {
    // 布局记忆 + 相对网格（通用能力）：定位成功一次就记住坐标；窗口身份/分辨率变化即失效；
    // 点下去没变化即作废；网格按 (行,列) 直接算坐标。
    AiUiLayoutClear();
    bool ok = true;
    std::wstring detail;

    AiUiLayoutKey key;
    key.target = L"\u8349\u576a";
    key.windowIdentity = L"PvZ|Class|PvZ.exe|1920x1080";
    key.screenW = 2560;
    key.screenH = 1440;

    const AiUiLayoutRect r{100, 200, 160, 260};
    AiUiLayoutRect got{};
    ok = ok && !AiUiLayoutRecall(key, got);            // 记忆前取不到
    AiUiLayoutRemember(key, r);
    ok = ok && AiUiLayoutRecall(key, got) && got.cx() == 130 && got.cy() == 230;

    AiUiLayoutKey other = key;                          // 换窗/改客户区 → 新界面
    other.windowIdentity = L"PvZ|Class|PvZ.exe|1280x720";
    ok = ok && !AiUiLayoutRecall(other, got);
    AiUiLayoutKey res = key;                            // 改分辨率 → 失效
    res.screenW = 1920;
    ok = ok && !AiUiLayoutRecall(res, got);
    AiUiLayoutKey otherTarget = key;                    // 不同目标 → 不同条目
    otherTarget.target = L"\u50f5\u5c38\u5361";
    ok = ok && !AiUiLayoutRecall(otherTarget, got);

    const AiUiGridSpec grid{130, 230, 120, 100, 7, 4};
    AiUiLayoutRememberGrid(key, grid);
    AiUiGridSpec gotGrid{};
    ok = ok && AiUiLayoutRecallGrid(key, gotGrid)
        && gotGrid.stepX == 120 && gotGrid.stepY == 100;
    // ★单次「点下去没变化」**不删**条目（实测每次点击都误报）：仍能取回；
    //   连续两次才判过期 → 回退真识图（条目仍在，下次 Remember 刷新它）
    AiUiLayoutNoteClickNoEffect(key);
    ok = ok && AiUiLayoutRecall(key, got) && AiUiLayoutStaleCount() == 0;
    AiUiLayoutNoteClickNoEffect(key);
    ok = ok && !AiUiLayoutRecall(key, got) && AiUiLayoutStaleCount() == 1;
    ok = ok && AiUiLayoutCount() == 1;              // 条目没被删
    AiUiLayoutRemember(key, r);                     // 重新识图成功 → 恢复可用
    ok = ok && AiUiLayoutRecall(key, got) && AiUiLayoutStaleCount() == 0;
    // 多次定位取平均：偏 6px 的第二次会把坐标往中间拉（越用越准；具体权重看样本数）
    AiUiLayoutRect r2{106, 206, 166, 266};
    AiUiLayoutRemember(key, r2);
    AiUiLayoutRect avg{};
    ok = ok && AiUiLayoutRecall(key, avg) && avg.cx() > 130 && avg.cx() <= 136;
    // 偏得太多（>24px）→ 直接换新值，不拉平均
    AiUiLayoutRect r3{500, 600, 560, 660};
    AiUiLayoutRemember(key, r3);
    ok = ok && AiUiLayoutRecall(key, avg) && avg.cx() == 530;
    AiUiLayoutForget(key);
    ok = ok && !AiUiLayoutRecall(key, got) && !AiUiLayoutRecallGrid(key, gotGrid);
    ok = ok && AiUiLayoutCount() == 0;

    int x = 0, y = 0;
    const bool c00 = AiUiGridCellCenter(grid, 0, 0, x, y) && x == 130 && y == 230;
    const bool c01 = AiUiGridCellCenter(grid, 0, 1, x, y) && x == 250 && y == 230;
    const bool c10 = AiUiGridCellCenter(grid, 1, 0, x, y) && x == 130 && y == 330;
    const bool cNeg = AiUiGridCellCenter(grid, -1, -1, x, y) && x == 10 && y == 130;
    const bool cOob = !AiUiGridCellCenter(grid, 999, 0, x, y);
    ok = ok && c00 && c01 && c10 && cNeg && cOob;
    if (!(c00 && c01 && c10 && cNeg && cOob)) detail += L" cell";
    AiUiLayoutClear();
    Emit(L"ui_layout_memory_and_grid", ok, detail.c_str());
}

void CaseUiLayoutSignatureGate() {
    // 「点错按钮」的闸：记住坐标时留外观签名；命中前用当前画面重算，
    // 签名不一致（界面变了）→ 必须拒绝命中，回退真识图。
    AiUiLayoutClear();
    bool ok = true;
    std::wstring detail;
    AiUiLayoutKey key;
    key.target = L"确认";
    key.windowIdentity = L"Game|Class|g.exe|1280x720";
    key.screenW = 1920;
    key.screenH = 1080;
    const AiUiLayoutRect r{100, 100, 140, 140};
    AiUiLayoutRemember(key, r);

    // 造一帧：目标区亮块
    const int w = 400, h = 300;
    std::vector<uint8_t> frame(static_cast<size_t>(w) * h, 30);
    for (int y = 95; y < 145; ++y)
        for (int x = 95; x < 145; ++x) frame[static_cast<size_t>(y) * w + x] = 220;
    uint8_t sig[kAiUiSigN * kAiUiSigN]{};
    const bool gotSig = AiUiLayoutSignature(frame.data(), w, h, w, r, sig);
    AiUiLayoutRememberSignature(key, sig);
    // 同一帧 → 必须通过
    uint8_t sig2[kAiUiSigN * kAiUiSigN]{};
    AiUiLayoutSignature(frame.data(), w, h, w, r, sig2);
    const bool sameOk = gotSig && AiUiLayoutSignatureMatches(key, sig2);

    // 目标区被换掉（面板关了/换页）→ 必须拒绝
    std::vector<uint8_t> frame2(static_cast<size_t>(w) * h, 30);
    for (int y = 95; y < 145; ++y)
        for (int x = 95; x < 145; ++x) frame2[static_cast<size_t>(y) * w + x] = 40;
    uint8_t sig3[kAiUiSigN * kAiUiSigN]{};
    AiUiLayoutSignature(frame2.data(), w, h, w, r, sig3);
    const bool changedRejected = !AiUiLayoutSignatureMatches(key, sig3);
    ok = ok && sameOk && changedRejected;
    if (!(sameOk && changedRejected)) detail += L" sig(same=" + std::to_wstring(sameOk ? 1 : 0)
        + L",changed=" + std::to_wstring(changedRejected ? 1 : 0) + L")";
    AiUiLayoutClear();
    Emit(L"ui_layout_signature_gate", ok, detail.c_str());
}

void CaseUiGridPeriodDetect() {
    bool ok = true;
    std::wstring detail;
    auto makeGrid = [](int w, int h, int ox, int oy, int stepX, int stepY,
                       int cols, int rows, int cw, int ch) {
        std::vector<uint8_t> img(static_cast<size_t>(w) * h, 40);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const int cx = ox + c * stepX;
                const int cy = oy + r * stepY;
                for (int yy = cy - ch / 2; yy <= cy + ch / 2; ++yy) {
                    if (yy < 0 || yy >= h) continue;
                    for (int xx = cx - cw / 2; xx <= cx + cw / 2; ++xx) {
                        if (xx < 0 || xx >= w) continue;
                        img[static_cast<size_t>(yy) * w + xx] =
                            static_cast<uint8_t>(170 + ((xx * 7 + yy * 13) % 60));
                    }
                }
            }
        }
        return img;
    };
    // ① 规则网格（一行 9 格、列距 120、行距 100）：锚点给一格，应检出列距 120
    {
        const int w = 1200, h = 400;
        const auto img = makeGrid(w, h, 100, 200, 120, 100, 9, 3, 70, 60);
        const AiUiLayoutRect anchor{70, 170, 130, 230};
        int px = 0, py = 0;
        const bool got = AiUiDetectGridPeriod(img.data(), w, h, w, anchor, 16, 420, px, py);
        const bool okX = got && std::abs(px - 120) <= 4;
        const bool okY = !got || py == 0 || std::abs(py - 100) <= 4;
        ok = ok && okX && okY;
        if (!(okX && okY))
            detail += L" grid(px=" + std::to_wstring(px) + L",py=" + std::to_wstring(py) + L")";
    }
    // ② 不规则画面（噪声）：不许报出周期，否则会把不相干的地方当格子点
    {
        const int w = 900, h = 300;
        std::vector<uint8_t> noise(static_cast<size_t>(w) * h);
        unsigned int s = 0x12345678u;
        for (auto& v : noise) {
            s = s * 1664525u + 1013904223u;
            v = static_cast<uint8_t>((s >> 19) & 0xFF);
        }
        const AiUiLayoutRect anchor{200, 120, 260, 180};
        int px = 0, py = 0;
        const bool got = AiUiDetectGridPeriod(noise.data(), w, h, w, anchor, 16, 420, px, py);
        ok = ok && !got;
        if (got) detail += L" noise(px=" + std::to_wstring(px) + L",py=" + std::to_wstring(py) + L")";
    }
    // ③ 周期远小于元素自身 → 判为假周期（实测：暂停菜单上误报 16×16px，会去点 79 列）
    {
        const int w = 1200, h = 400;
        const auto img = makeGrid(w, h, 100, 200, 8, 8, 120, 40, 6, 6);
        const AiUiLayoutRect anchor{70, 170, 130, 230};   // 60×60 的锚点，周期只有 8px
        int px = 0, py = 0;
        const bool got = AiUiDetectGridPeriod(img.data(), w, h, w, anchor, 16, 420, px, py);
        ok = ok && !got;
        if (got) detail += L" tiny(px=" + std::to_wstring(px) + L",py=" + std::to_wstring(py) + L")";
    }
    Emit(L"ui_grid_period_detect", ok, detail.c_str());
}

void CaseLocateDecimalCoordParse() {
    // 模型回小数坐标是常态（带 grounding 训练/长推理链的尤其多）。旧实现用 wcstol 逐个抓整数：
    //  · "(88.5,117.3)" → x=88，y 从 ".5" 解析失败 → 整条判「无法解析」（白烧一轮 API）；
    //  · "[52.4,100.2,127.6,137.9]" → 静默错框（抓到 52，再把小数点后的 5 当 y1）。
    bool ok = true;
    std::wstring detail;
    {
        int x = 0, y = 0;
        const bool p1 = TryParseCoordinatePair(L"(88.5,117.3)", x, y) && x == 89 && y == 117;
        const bool p2 = TryParseCoordinatePair(L"[52.4, 100.6]", x, y) && x == 52 && y == 101;
        const bool p3 = TryParseCoordinatePair(L"(640,360)", x, y) && x == 640 && y == 360;
        const bool p4 = TryParseCoordinatePair(L"目标在 (0,0)", x, y) && x == 0 && y == 0;
        ok = ok && p1 && p2 && p3 && p4;
        if (!(p1 && p2 && p3 && p4)) detail += L" pair";
    }
    {
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        const bool b1 = TryParseBoundingBox(L"[52.4,100.2,127.6,137.9]", x1, y1, x2, y2)
            && x1 == 52 && y1 == 100 && x2 == 128 && y2 == 138;
        const bool b2 = TryParseBoundingBox(L"[10,20,110,220]", x1, y1, x2, y2)
            && x1 == 10 && y1 == 20 && x2 == 110 && y2 == 220;
        // 反序框仍要归一化成 min/max
        const bool b3 = TryParseBoundingBox(L"框选 (127.9,137.2,52.1,100.4)", x1, y1, x2, y2)
            && x1 == 52 && y1 == 100 && x2 == 128 && y2 == 137;
        // 描述性前置 + 小数：不能抓错
        const bool b4 = TryParseBoundingBox(L"第2个按钮在 [300.5, 200.5, 400.5, 260.5]",
            x1, y1, x2, y2) && x1 == 301 && y1 == 201 && x2 == 401 && y2 == 261;
        ok = ok && b1 && b2 && b3 && b4;
        if (!(b1 && b2 && b3 && b4)) {
            detail += L" box(" + std::to_wstring(x1) + L"," + std::to_wstring(y1) + L","
                + std::to_wstring(x2) + L"," + std::to_wstring(y2) + L")";
        }
    }
    Emit(L"locate_decimal_coord_parse", ok, detail.c_str());
}

void CaseMissSelfCorrectPrompt() {
    // 「错点自纠」的 prompt 契约（备用项）：必须 ① 说清红叉=刚点过的错点、
    // ② 给出放大图与错点的坐标上下文、③ 只要**绝对坐标**、④ 明确禁止偏移量。
    // 依据：PrecisionCUA（arXiv 2604.13019）——红叉+绝对坐标让 GPT-5.4-Pro 13.5%→41.0%，
    // 而「用锚点估算相对位置」的提示词把它打到 18.5%。
    bool ok = true;
    std::wstring detail;
    const std::wstring p = BuildMissSelfCorrectPrompt(L"自选僵尸卡牌", 768, 768, 226, 169);
    const bool hasCross = p.find(L"红色十字") != std::wstring::npos
        || p.find(L"红叉") != std::wstring::npos;
    const bool hasFailed = p.find(L"226") != std::wstring::npos
        && p.find(L"169") != std::wstring::npos;
    const bool hasTarget = p.find(L"自选僵尸卡牌") != std::wstring::npos;
    const bool noOffsetAsk = p.find(L"不要输出偏移量") != std::wstring::npos
        || p.find(L"❌不要输出偏移量") != std::wstring::npos;
    const bool absAsk = p.find(L"绝对坐标") != std::wstring::npos;
    const bool notFound = p.find(L"NOT_FOUND") != std::wstring::npos;
    ok = ok && hasCross && hasFailed && hasTarget && noOffsetAsk && absAsk && notFound;
    if (!ok) {
        detail += L" cross=" + std::to_wstring(hasCross ? 1 : 0)
            + L" failed=" + std::to_wstring(hasFailed ? 1 : 0)
            + L" target=" + std::to_wstring(hasTarget ? 1 : 0)
            + L" noOffset=" + std::to_wstring(noOffsetAsk ? 1 : 0)
            + L" abs=" + std::to_wstring(absAsk ? 1 : 0)
            + L" notFound=" + std::to_wstring(notFound ? 1 : 0);
    }
    Emit(L"miss_self_correct_prompt", ok, detail.c_str());
}

void CaseOcrDirectClickPick() {
    bool ok = true;
    std::wstring detail;
    auto line = [](const wchar_t* t, int x1, int y1, int x2, int y2, double conf = 0.95) {
        OcrTextLine l;
        l.text = t;
        l.x1 = x1; l.y1 = y1; l.x2 = x2; l.y2 = y2;
        l.confidence = conf;
        return l;
    };
    const int sw = 2560, sh = 1440;

    // ① 唯一命中：目标从描述里剥出「保存」→ 直点该行中心
    {
        std::vector<OcrTextLine> lines{
            line(L"文件", 100, 100, 200, 140),
            line(L"保存", 900, 300, 1000, 344),
            line(L"取消", 1100, 300, 1200, 344),
        };
        std::wstring want;
        const bool extracted = AiLocateExtractOcrTarget(L"保存按钮", &want);
        AiOcrDirectHit hit;
        const bool got = extracted
            && AiOcrPickDirectClickTarget(lines, want, sw, sh, &hit);
        const bool pos = got && hit.screenX == 950 && hit.screenY == 322
            && hit.matchKind == L"exact";
        ok = ok && pos;
        if (!pos) detail += L" exact(got=" + std::to_wstring(got ? 1 : 0)
            + L",x=" + std::to_wstring(hit.screenX) + L",y=" + std::to_wstring(hit.screenY)
            + L",kind=" + hit.matchKind + L",why=" + hit.why + L")";
    }
    // ② 同屏两个「确定」→ 歧义，必须拒绝（宁可回落识图，也不能点错那个）
    {
        std::vector<OcrTextLine> lines{
            line(L"确定", 300, 200, 380, 240),
            line(L"确定", 1600, 900, 1680, 940),
        };
        AiOcrDirectHit hit;
        const bool got = AiOcrPickDirectClickTarget(lines, L"确定", sw, sh, &hit);
        ok = ok && !got;
        if (got) detail += L" ambiguous(x=" + std::to_wstring(hit.screenX) + L")";
    }
    // ③ 「保存并关闭」包含「保存」：长度接近才允许包含档命中；且要能被点到
    {
        std::vector<OcrTextLine> lines{ line(L"保存并关闭", 700, 500, 900, 544) };
        AiOcrDirectHit hit;
        const bool got = AiOcrPickDirectClickTarget(lines, L"保存并关闭", sw, sh, &hit);
        const bool pos = got && hit.screenX == 800 && hit.screenY == 522;
        ok = ok && pos;
        if (!pos) detail += L" contains(x=" + std::to_wstring(hit.screenX) + L")";
    }
    // ④ 整块面板被 OCR 并成「一行」：框过大 → 拒绝（点中心会打到别处）
    {
        std::vector<OcrTextLine> lines{ line(L"设置", 100, 100, 2200, 1300) };
        AiOcrDirectHit hit;
        const bool got = AiOcrPickDirectClickTarget(lines, L"设置", sw, sh, &hit);
        ok = ok && !got;
        if (got) detail += L" hugebox";
    }
    // ⑤ 目标不在索引里 → 拒绝（回落识图），并给出原因
    {
        std::vector<OcrTextLine> lines{ line(L"确定", 300, 200, 380, 240) };
        AiOcrDirectHit hit;
        const bool got = AiOcrPickDirectClickTarget(lines, L"开始游戏", sw, sh, &hit);
        const bool why = !hit.why.empty();
        ok = ok && !got && why;
        if (got || !why) detail += L" nomatch";
    }
    // ⑥ 低置信度行不参与直点（识别噪声不该变成点击）
    {
        std::vector<OcrTextLine> lines{ line(L"确定", 300, 200, 380, 240, 0.20) };
        AiOcrDirectHit hit;
        const bool got = AiOcrPickDirectClickTarget(lines, L"确定", sw, sh, &hit);
        ok = ok && !got;
        if (got) detail += L" lowconf";
    }
    // ⑦ 非文字目标（图标/格子/方位）连提取都过不了 → 根本不会走直点
    {
        std::wstring want;
        const bool icon = AiLocateExtractOcrTarget(L"右上角齿轮图标", &want);
        const bool grid = AiLocateExtractOcrTarget(L"草坪格子", &want);
        const bool pureNum = AiLocateExtractOcrTarget(L"3000", &want);
        ok = ok && !icon && !grid && !pureNum;
        if (icon || grid || pureNum) detail += L" extract";
    }
    Emit(L"ocr_direct_click_pick", ok, detail.c_str());
}

void CasePlanSpendBudget() {
    // 「先算账」：选卡（卡槽有限、按性价比挑）与放单位（预算内尽量多放）
    bool ok = true;
    std::wstring detail;

    // ① 选卡：卡槽 4 个，20 张里既有 600 的强卡也有 25 的弱卡 → 必须挑强的，不能全选
    std::vector<PlanSpendItem> deck;
    deck.push_back({L"巨人僵尸", 600, 600, -1, false});
    deck.push_back({L"冰车僵尸", 400, 400, -1, false});
    deck.push_back({L"铁桶僵尸", 175, 175, -1, false});
    deck.push_back({L"路障僵尸", 75, 75, -1, false});
    deck.push_back({L"普通僵尸", 50, 50, -1, false});
    for (int i = 0; i < 15; ++i) deck.push_back({L"弱卡" + std::to_wstring(i), 25, 20, -1, false});
    const PlanSpendPlan pick = PlanSpendBudget(30000, 4, true, deck);
    ok = ok && pick.totalCount == 4 && pick.picks.size() == 4;
    const bool strongFirst = !pick.picks.empty() && pick.picks[0].name == L"巨人僵尸";
    ok = ok && strongFirst;
    if (!(pick.totalCount == 4 && strongFirst))
        detail += L" deck(" + std::to_wstring(pick.totalCount) + L")";

    // ② 放单位：阳光 3000、单只 600 → 一次能放 5 只（不是 1 只）
    std::vector<PlanSpendItem> units;
    units.push_back({L"巨人僵尸", 600, 600, -1, false});
    const PlanSpendPlan wave = PlanSpendBudget(3000, 0, false, units);
    ok = ok && wave.totalCount == 5 && wave.remaining == 0;
    if (wave.totalCount != 5)
        detail += L" wave(" + std::to_wstring(wave.totalCount) + L")";

    // ③ 买不起 → 明确说买不起，而不是给个空方案糊过去
    std::vector<PlanSpendItem> pricey;
    pricey.push_back({L"僵尸博士", 9999, 9999, -1, false});
    const PlanSpendPlan none = PlanSpendBudget(500, 0, false, pricey);
    ok = ok && none.picks.empty() && none.summary.find(L"买不起") != std::wstring::npos;

    // ④ maxCount 生效（限量商品）
    std::vector<PlanSpendItem> limited;
    limited.push_back({L"限购卡", 100, 100, 2, false});
    limited.push_back({L"普通卡", 100, 100, -1, false});
    const PlanSpendPlan lim = PlanSpendBudget(1000, 0, false, limited);
    ok = ok && lim.totalCount == 10;   // 2 个限购 + 8 个普通（各 100，共 1000）
    if (lim.totalCount != 10) detail += L" lim(" + std::to_wstring(lim.totalCount) + L")";

    Emit(L"plan_spend_budget", ok, detail.c_str());
}

void CaseNoBatchSelectSpecialCase() {
    // 选卡/批量选择**软提示**：没算账时「一键全选」**照常执行**，但结果里必须带 planSpend 提醒。
    // ★为什么不再硬拦：硬拦要模型给「各卡价格/卡槽数」，而那是它拿不到的数据 ——
    //   实测被拦后模型当场放弃整个选卡面板，转去 listUiControls/截图/Escape 开暂停菜单，
    //   白烧 4 轮，最后只在种卡栏塞进一张卡（用户报障「选卡只选了一张」）。
    //   规矩：**挡路又不给可行替代，比不拦更糟**。
    // 关键词判定是通用的（全选/批量选/select all），不绑定某个游戏。
    bool ok = true;
    std::wstring detail;
    ResetAiActionSessionState(494949);          // 复位标记
    SetAiForegroundBrowserOverrideForTest(-1);
    SetAiSpreadsheetForegroundOverrideForTest(0);
    SetAiActionPlanGateEnabled(false);

    int hostCalls = 0;
    AiActionHostHooks hooks;
    hooks.onLocateAndClick = [&](const std::wstring&, int, const std::wstring&, int) -> std::wstring {
        ++hostCalls;
        return L"locateAndClick 已点击屏幕(10,20)";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});

    // ① 批量选择**不再有任何特判**（宿主只做能力，不做领域规则）：
    //    无论有没有算过账，「一键全选」都照常定位点击、结果里也不再出现 planSpend 说教。
    const std::wstring sel = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\u4e00\u952e\u5168\u9009\"}");
    const bool selOk = sel.find(L"[错误]") == std::wstring::npos && hostCalls == 1
        && sel.find(L"planSpend") == std::wstring::npos;
    ok = ok && selOk;
    if (!selOk) detail += L" selectAll";

    // ② 算过账后同样照常（不该有任何差别对待）
    AiNotePlanSpendCalled();
    const std::wstring allowed = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\u4e00\u952e\u5168\u9009\"}");
    const bool allowedOk = allowed.find(L"[错误]") == std::wstring::npos && hostCalls == 2;
    ok = ok && allowedOk;
    if (!allowedOk) detail += L" allowed";

    // ③ 非批量目标不受影响（门槛只拦批量选择）
    ResetAiActionSessionState(494950);
    int hostCalls2 = 0;
    AiActionHostHooks hooks2;
    hooks2.onLocateAndClick = [&](const std::wstring&, int, const std::wstring&, int) -> std::wstring {
        ++hostCalls2;
        return L"ok";
    };
    const auto tools2 = BuildAiActionExecuteTools(&hooks2, {});
    const std::wstring normal = CallNamedTool(tools2, L"locateAndClick",
        L"{\"target\":\"\u786e\u5b9a\"}");
    const bool normalOk = normal.find(L"[错误]") == std::wstring::npos && hostCalls2 == 1;
    ok = ok && normalOk;
    if (!normalOk) detail += L" normal";

    Emit(L"plan_spend_gate", ok, detail.c_str());
}

void CaseGameForegroundVisionAllowed() {
    // 根因是这条闸把「画面一直在动」判成「别点」—— 但游戏没有控件树/DOM 可退，
    // locateAndClick 是唯一推进手段，必须放行。这里把四种前台钉死。
    bool ok = true;
    std::wstring detail;

    // ① 桌面游戏/自绘前台：高动态也**不拦**识图，且要认得出「游戏前台」
    ResetAiActionSessionState(474747);
    SetAiForegroundBrowserOverrideForTest(-1);
    NoteAiActionUiBusy(0.42, true);
    const bool gameAllowed = !AiActionUiTooBusyForVisionLocate();
    const bool gameRecognized = AiActionGameForegroundLikely();
    SetAiActionPlanGateEnabled(false);
    const auto toolsGame = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring gameLoc = CallNamedTool(toolsGame, L"locateAndClick", L"{\"target\":\"僵尸\"}");
    const bool gameNotBusyBlocked = gameLoc.find(L"动态干扰过大") == std::wstring::npos;
    ok = ok && gameAllowed && gameRecognized && gameNotBusyBlocked;
    if (!(gameAllowed && gameRecognized && gameNotBusyBlocked)) detail += L" game";

    // ② 画布页（网页游戏）：即便前台是浏览器也不能拦
    ResetAiActionSessionState(474748);
    SetAiForegroundBrowserOverrideForTest(1);
    AiNotePageKind(L"canvas");
    NoteAiActionUiBusy(0.42, true);
    const bool canvasAllowed = !AiActionUiTooBusyForVisionLocate();
    const bool canvasRecognized = AiActionGameForegroundLikely();
    ok = ok && canvasAllowed && canvasRecognized;
    if (!(canvasAllowed && canvasRecognized)) detail += L" canvas";

    // ③ 浏览器前台 + 页面类型未知 + 高动态：仍然拦（先 observePage 拿树）
    ResetAiActionSessionState(474749);
    SetAiForegroundBrowserOverrideForTest(1);
    NoteAiActionUiBusy(0.42, true);
    const bool browserBlocked = AiActionUiTooBusyForVisionLocate();
    const bool browserNotGame = !AiActionGameForegroundLikely();
    ok = ok && browserBlocked && browserNotGame;
    if (!(browserBlocked && browserNotGame)) detail += L" browser";

    // ④ 表格软件：有更准的定位路线，不算游戏
    ResetAiActionSessionState(474750);
    SetAiForegroundBrowserOverrideForTest(-1);
    SetAiSpreadsheetForegroundOverrideForTest(1);
    NoteAiActionUiBusy(0.42, true);
    const bool spreadsheetNotGame = !AiActionGameForegroundLikely();
    SetAiSpreadsheetForegroundOverrideForTest(0);
    ok = ok && spreadsheetNotGame;
    if (!spreadsheetNotGame) detail += L" sheet";

    Emit(L"game_foreground_vision_allowed", ok, detail.c_str());
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
    bool hasObs = false, hasClickRef = false, hasTypeRef = false;
    for (const auto& t : tools) {
        if (t.name == L"quickInput") hasQi = true;
        if (t.name == L"locateAndClick") hasLocate = true;
        if (t.name == L"runProgram") hasRun = true;
        if (t.name == L"hotkeyShortcut") hasHot = true;
        if (t.name == L"openWebpage") hasWeb = true;
        if (t.name == L"observePage") hasObs = true;
        if (t.name == L"clickRef") hasClickRef = true;
        if (t.name == L"typeRef") hasTypeRef = true;
    }
    const bool ok = hasQi && hasLocate && hasObs && hasClickRef && hasTypeRef
        && !hasRun && !hasHot && !hasWeb;
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
    const std::wstring firstVideo = ExtractClickTargetPhrase(
        L"视频列表「最新发布」下第一个视频缩略图（日本篇 汉字文化圈）");
    const bool h = firstVideo.find(L"第一个") != std::wstring::npos
        && firstVideo.find(L"缩略图") != std::wstring::npos
        && firstVideo != L"最新发布";
    Emit(L"composite_target_phrase_stripped",
        a && b && c && d && e && f && g && h,
        (L"[a=" + std::to_wstring(a ? 1 : 0)
            + L" b=" + std::to_wstring(b ? 1 : 0)
            + L" c=" + std::to_wstring(c ? 1 : 0)
            + L" d=" + std::to_wstring(d ? 1 : 0)
            + L" e=" + std::to_wstring(e ? 1 : 0)
            + L" f=" + std::to_wstring(f ? 1 : 0)
            + L" g=" + std::to_wstring(g ? 1 : 0)
            + L" h=" + std::to_wstring(h ? 1 : 0) + L"]").c_str());
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
        // ★DeepSeek v4 起是原生多模态。旧规则「带 deepseek 且没 vl/vision 就算纯文本」
        //   把用户选的 deepseek-v4.1-flash 判成不能识图 → 动作模型被静默换成豆包（实测报障）
        && ModelSupportsVision(L"deepseek-v4-flash")
        && ModelSupportsVision(L"deepseek v4.1 flash")
        && ModelSupportsVision(L"deepseek-v4.1-flash-vision")
        && !ModelSupportsVision(L"deepseek-chat")
        && !ModelSupportsVision(L"deepseek-r1")
        && !ModelSupportsVision(L"deepseek-reasoner")
        && !ModelSupportsVision(L"deepseek-v3.1-turbo")
        && !ModelSupportsVision(L"qwen-turbo");
    Emit(L"model_supports_vision", ok, L"");
}

void CaseActionModelNotSilentlySwapped() {
    // 用户报障：设置里选的是 deepseek-v4.1-flash，AI 动作执行却跑了豆包。
    // 两条要求：① 选了多模态模型就**原样用它**（别挑列表第一个识图模型）；
    //          ② AI 动作执行**不允许**在写库时静默改写动作里的模型。
    quickscript::AiApiSettings ai;
    ai.modelName = L"deepseek v4.1 flash";
    quickscript::AiModelProfile doubao;
    doubao.modelName = L"doubao-seed-2-1-pro-260628";
    ai.savedModels.push_back(doubao);
    quickscript::AiModelProfile ds;
    ds.modelName = L"deepseek v4.1 flash";
    ai.savedModels.push_back(ds);

    ScriptAction a;
    a.type = ActionType::AiActionExecute;
    a.aiWithImage = true;
    a.aiModelName = L"deepseek v4.1 flash";
    const std::wstring resolved = ResolveActionAiModelName(a, ai);
    const bool keepUserModel = resolved == L"deepseek v4.1 flash";

    // 兜底仍在，但顺序是「设置里当前配置的模型 → 列表里第一个识图模型」：
    // 纯文本动作模型 + 需要识图 → 用户配置的 deepseek v4.1 flash（它能识图）
    ScriptAction textOnly = a;
    textOnly.aiModelName = L"deepseek-chat";
    const std::wstring fallback = ResolveActionAiModelName(textOnly, ai);
    const bool fallbackToUserDefault = fallback == L"deepseek v4.1 flash";
    // 用户配置的模型也不能识图时 → 才退到列表里的识图模型
    quickscript::AiApiSettings aiTextOnly = ai;
    aiTextOnly.modelName = L"deepseek-chat";
    const std::wstring fallbackVision = ResolveActionAiModelName(textOnly, aiTextOnly);
    const bool fallbackToAnyVision = fallbackVision.find(L"doubao") != std::wstring::npos;

    // 写库/构建动作时不得改写 AI 动作执行的模型
    ScriptAction keep = a;
    keep.aiModelName = L"deepseek-chat";
    EnsureAiModelOnAction(keep);
    const bool notRewritten = keep.aiModelName == L"deepseek-chat";

    // 动作没写模型名 → 用「设置里当前配置的模型」，而不是列表第 1 个
    ScriptAction noModel = a;
    noModel.aiModelName.clear();
    const std::wstring defResolved = ResolveActionAiModelName(noModel, ai);
    const bool defaultWins = defResolved == L"deepseek v4.1 flash";

    const bool ok = keepUserModel && fallbackToUserDefault && fallbackToAnyVision
        && notRewritten && defaultWins;
    Emit(L"action_model_not_silently_swapped", ok,
        (L"解析=" + resolved + L" 纯文本兜底=" + fallback
            + L" 默认也不识图时=" + fallbackVision
            + L" 写库后=" + keep.aiModelName + L" 空模型默认=" + defResolved).c_str());
}

void CasePlannerObserveImageAttach() {
    // DeepSeek v4 起可收图 → 规划轮必须附观察截图（否则游戏/界面只能盲猜）；
    // 真·纯文本模型才不附（避免无效多模态请求 400）。
    const bool v4Yes = ShouldAttachObserveImageToPlanner(L"deepseek-v4-flash", true)
        && ShouldAttachObserveImageToPlanner(L"deepseek v4.1 flash", true);
    const bool textNo = !ShouldAttachObserveImageToPlanner(L"deepseek-chat", true)
        && !ShouldAttachObserveImageToPlanner(L"deepseek-reasoner", true);
    const bool visionYes = ShouldAttachObserveImageToPlanner(L"doubao-seed-1", true)
        && ShouldAttachObserveImageToPlanner(L"gpt-4o", true);
    const bool emptyNo = !ShouldAttachObserveImageToPlanner(L"gpt-4o", false);
    Emit(L"planner_observe_image_attach", v4Yes && textNo && visionYes && emptyNo, L"");
}

// ── DOM 优先点击：控件树选 ref ────────────────────────────────────────
PageSnapshot MakePickSnapshot() {
    const std::string json =
        R"({"ok":true,"pageKind":"dom","url":"https://example.com/","title":"登录页","nodes":[)"
        R"({"ref":"e1","role":"link","name":"忘记密码","x":10,"y":300,"w":80,"h":20,"inView":true},)"
        R"({"ref":"e2","role":"button","name":"登录","x":10,"y":200,"w":120,"h":36,"inView":true},)"
        R"({"ref":"e3","role":"button","name":"登录/注册","x":10,"y":260,"w":120,"h":36,"inView":true},)"
        R"({"ref":"e4","role":"textbox","name":"用户名","x":10,"y":100,"w":200,"h":30,"inView":true}]})";
    return ParsePageSnapshotJson(json);
}

void CasePickSnapshotRefForText() {
    const PageSnapshot snap = MakePickSnapshot();
    // 完全同名优先于部分命中（「登录」不该选中「登录/注册」）
    const PageSnapshotPick exact = PickPageSnapshotRefForText(snap, L"登录按钮");
    const bool exactOk = exact.ref == L"e2" && exact.exact && !exact.ambiguous;
    // 完全同名唯一时才敢 DOM 直接点；这里名字含「登录」但无完全同名 → 部分命中
    const PageSnapshotPick partial = PickPageSnapshotRefForText(snap, L"登录入口");
    const bool partialOk = partial.ref.empty() || !partial.exact;
    // 树上没有 → 交回识图
    const PageSnapshotPick miss = PickPageSnapshotRefForText(snap, L"立即购买");
    const bool missOk = miss.ref.empty();
    // 空标签 / 单词标签不猜
    const bool emptyOk = PickPageSnapshotRefForText(snap, L"").ref.empty();
    const bool ok = exactOk && partialOk && missOk && emptyOk;
    Emit(L"pick_snapshot_ref_for_text", ok,
        ok ? L"" : (L"exact=" + exact.ref + L"/" + std::to_wstring(exact.exact)
            + L" partial=" + partial.ref + L" miss=" + miss.ref).c_str());
}

void CasePickSnapshotRefAmbiguous() {
    // 两个都只是「包含」命中且分数接近 → 不许 DOM 直接点（回退识图更安全）
    const std::string json =
        R"({"ok":true,"pageKind":"dom","nodes":[)"
        R"({"ref":"e1","role":"button","name":"确定提交订单","x":10,"y":200,"w":120,"h":36,"inView":true},)"
        R"({"ref":"e2","role":"button","name":"确定放弃订单","x":10,"y":210,"w":120,"h":36,"inView":true}]})";
    const PageSnapshot snap = ParsePageSnapshotJson(json);
    const PageSnapshotPick p = PickPageSnapshotRefForText(snap, L"订单");
    const bool ambiguousOk = p.ambiguous && p.candidates >= 2;
    // 同名多项（列表重复卡片）不算歧义：取阅读顺序最前 = 列表第 1 项
    const std::string dupJson =
        R"({"ok":true,"pageKind":"dom","nodes":[)"
        R"({"ref":"e1","role":"link","name":"订阅","x":300,"y":100,"w":60,"h":24,"inView":true},)"
        R"({"ref":"e2","role":"link","name":"订阅","x":40,"y":500,"w":60,"h":24,"inView":true}]})";
    const PageSnapshot dup = ParsePageSnapshotJson(dupJson);
    const PageSnapshotPick d = PickPageSnapshotRefForText(dup, L"订阅");
    const bool dupOk = !d.ref.empty() && !d.ambiguous && d.sameNameCount == 2 && d.ref == L"e1";
    const bool ok = ambiguousOk && dupOk;
    Emit(L"pick_snapshot_ref_ambiguous", ok,
        ok ? L"" : (L"amb=" + std::to_wstring(p.ambiguous) + L" cand="
            + std::to_wstring(p.candidates) + L" dup=" + d.ref + L"/"
            + std::to_wstring(d.sameNameCount)).c_str());
}

void CaseSnapshotTargetKeyword() {
    // 扩展 observePage(query) 是「控件名包含关键字」过滤：整句必 0 命中
    const bool ok = PageSnapshotTargetKeyword(L"点击登录按钮") == L"登录"
        && PageSnapshotTargetKeyword(L"请帮我点击「搜索」") == L"搜索"
        && PageSnapshotTargetKeyword(L"订阅图标") == L"订阅"
        && PageSnapshotTargetKeyword(L"会员中心") == L"会员中心";
    Emit(L"snapshot_target_keyword", ok,
        (std::wstring(L"got=") + PageSnapshotTargetKeyword(L"点击登录按钮")).c_str());
}

void CaseVisionPromptNormalizedContract() {
    const std::wstring loc = BuildCompositeLocatePrompt(L"搜索按钮", 1280, 720);
    const std::wstring ref = BuildCompositeRefinePointPrompt(L"搜索按钮", 1, 768, 768);
    const std::wstring cor = BuildCompositeCorrectLocatePrompt(L"搜索按钮", 1, 2, 3, 4, 1280, 720);
    const std::wstring sys = BuildAiActionVisionQuerySystemPrompt(1280, 720);
    auto hasNorm = [](const std::wstring& s) {
        return s.find(L"0~1000") != std::wstring::npos
            && s.find(L"禁止输出像素坐标") != std::wstring::npos;
    };
    const bool ok = hasNorm(loc) && hasNorm(ref) && hasNorm(cor) && hasNorm(sys)
        && loc.find(L"NOT_FOUND") != std::wstring::npos
        && sys.find(L"1280") != std::wstring::npos;
    Emit(L"vision_prompt_normalized_contract", ok, ok ? L"" : loc.c_str());
}

// ── 宿主有 DOM 钩子时 locateAndClick 不再「报错让模型再调一次」──────────
void CaseLocateToolDefersToHostDom() {
    ResetAiActionSessionState(941);
    SetAiActionPlanGateEnabled(false);
    const std::string watchJson =
        R"({"ok":true,"pageKind":"dom","url":"https://example.com/","nodes":[)"
        R"({"ref":"e1","role":"button","name":"登录","x":10,"y":200,"w":120,"h":36,"inView":true}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(watchJson));
    AiActionHostHooks hooks;
    hooks.onLocateAndClick = [](const std::wstring& target, int, const std::wstring&, int) {
        return L"locateAndClick 已按控件树点击「" + target
            + L"」(e1，DOM 精确点击，未截屏/未识图)";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring loc = CallNamedTool(tools, L"locateAndClick",
        L"{\"target\":\"\u767b\u5f55\u6309\u94ae\"}");
    const bool ok = loc.rfind(L"[错误]", 0) != 0
        && loc.find(L"DOM 精确点击") != std::wstring::npos
        && loc.find(L"clickRef") != std::wstring::npos;
    // 无宿主时保留原「改用 clickRef」的指路错误（离线规划路径）
    const auto noHost = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring locNoHost = CallNamedTool(noHost, L"locateAndClick",
        L"{\"target\":\"\u767b\u5f55\u6309\u94ae\"}");
    const bool noHostOk = locNoHost.rfind(L"[错误]", 0) == 0
        && locNoHost.find(L"clickRef(e1)") != std::wstring::npos;
    Emit(L"locate_tool_defers_to_host_dom", ok && noHostOk,
        ok ? (noHostOk ? L"" : locNoHost.c_str()) : loc.c_str());
    ResetAiActionSessionState(941);
}

// ── 预规划（lookahead）预算：不许把主链路拖住 ─────────────────────────
void CaseLookaheadWaitBudget() {
    const bool budgetOk = kAiLookaheadAfterObserveWaitMs <= 500
        && kAiLookaheadSkipObserveWaitMs <= 300
        && kAiLookaheadMaxStartsPerAction == 2;
    ResetAiActionSessionState(942);
    NoteAiActionUiBusy(-1.0, false);
    const bool firstOk = !AiActionShouldSkipLookahead();
    NoteAiActionLookaheadStarted();
    NoteAiActionLookaheadStarted();
    const bool cappedOk = AiActionShouldSkipLookahead();
    AiActionLookahead la;
    la.Cancel();  // 无在途预取时 Cancel 必须立刻返回（旧实现在此 join 到 API 超时）
    la.Reset();
    Emit(L"lookahead_wait_budget", budgetOk && firstOk && cappedOk,
        budgetOk ? L"" : L"budget constants drifted");
    ResetAiActionSessionState(942);
}

// ── 浏览器 DOM 优先门禁（判错会点到别的窗口 → 必须可自检）────────────
void CaseDomFirstActionGate() {
    auto gate = [](bool ext, bool hooks, bool canvas, bool browser, bool web,
                   bool titleReadable, bool right, bool browserClass = false) {
        DomFirstActionGateInput in;
        in.extensionConnected = ext;
        in.hasDomHooks = hooks;
        in.pageIsCanvas = canvas;
        in.foregroundLooksBrowser = browser;
        in.foregroundBrowserClass = browserClass;
        in.webSessionActive = web;
        in.foregroundTitleReadable = titleReadable;
        in.rightButton = right;
        return in;
    };
    std::wstring why;
    // 正常网页：放行
    const bool happy = ShouldUseDomFirstAction(
        gate(true, true, false, true, true, true, false), &why);
    // 未装扩展 / 宿主无钩子 / canvas / 右键：一律不放行
    const bool noExt = !ShouldUseDomFirstAction(
        gate(false, true, false, true, true, true, false), &why);
    const bool noHooks = !ShouldUseDomFirstAction(
        gate(true, false, false, true, true, true, false), &why);
    const bool canvas = !ShouldUseDomFirstAction(
        gate(true, true, true, true, true, true, false), &why);
    const bool rightBtn = !ShouldUseDomFirstAction(
        gate(true, true, false, true, true, true, true), &why);
    // 网页会话还在、但前台已切到别的程序 → 必须拦（否则点错窗口）
    const bool otherApp = !ShouldUseDomFirstAction(
        gate(true, true, false, false, true, true, false), &why);
    // ★标题读不出来**不再**只凭「本会话是网页」放行：另存为/打开这类模态对话框
    //   （#32770）标题常为空，旧策略会把 clickRef 打进浏览器，模型随后卡在保存界面。
    //   现在只有**窗口类**像浏览器才放行。
    const bool titleBlind = !ShouldUseDomFirstAction(
        gate(true, true, false, false, true, false, false), &why);
    const bool titleBlindNoWeb = !ShouldUseDomFirstAction(
        gate(true, true, false, false, false, false, false), &why);
    // 标题读不出来但窗口类确实是 Chromium/火狐 → 放行（正常浏览器窗口不受影响）
    const bool titleBlindButBrowserClass = ShouldUseDomFirstAction(
        gate(true, true, false, false, true, false, false, true), &why);
    const bool ok = happy && noExt && noHooks && canvas && rightBtn && otherApp
        && titleBlind && titleBlindNoWeb && titleBlindButBrowserClass;
    Emit(L"dom_first_action_gate", ok, ok ? L"" : L"gate policy drifted");
}

// ── DOM 优先填写：输入类选 ref ────────────────────────────────────────
void CasePickSnapshotRefForInput() {
    // A) 真实登录页：目标「密码」应选 textbox，而不是「忘记密码」链接
    const std::string loginJson =
        R"({"ok":true,"pageKind":"dom","nodes":[)"
        R"({"ref":"e1","role":"link","name":"忘记密码","x":10,"y":300,"w":80,"h":20,"inView":true},)"
        R"({"ref":"e2","role":"textbox","name":"密码","x":10,"y":200,"w":200,"h":30,"inView":true},)"
        R"({"ref":"e3","role":"button","name":"密码登录","x":10,"y":260,"w":120,"h":36,"inView":true}]})";
    const PageSnapshot login = ParsePageSnapshotJson(loginJson);
    const PageSnapshotPick p = PickPageSnapshotRefForText(
        login, L"密码", PageSnapshotPickKind::Input);
    const bool pickInput = p.ref == L"e2" && p.role == L"textbox" && !p.ambiguous;

    // B) 同名同位置、role 不同：两种口径必须给出不同答案（role 加权生效）
    const std::string tieJson =
        R"({"ok":true,"pageKind":"dom","nodes":[)"
        R"({"ref":"e1","role":"link","name":"验证码","x":10,"y":100,"w":120,"h":24,"inView":true},)"
        R"({"ref":"e2","role":"textbox","name":"验证码","x":10,"y":100,"w":120,"h":24,"inView":true}]})";
    const PageSnapshot tie = ParsePageSnapshotJson(tieJson);
    const PageSnapshotPick clickPick = PickPageSnapshotRefForText(
        tie, L"验证码", PageSnapshotPickKind::Clickable);
    const PageSnapshotPick inputPick = PickPageSnapshotRefForText(
        tie, L"验证码", PageSnapshotPickKind::Input);
    const bool differs = clickPick.ref == L"e1" && inputPick.ref == L"e2";
    Emit(L"pick_snapshot_ref_for_input", pickInput && differs,
        (L"login=" + p.ref + L"/" + p.role + L" click=" + clickPick.ref
            + L" input=" + inputPick.ref).c_str());
}

// ── typeByLabel：一次调用完成「按标签填写」────────────────────────────
void CaseTypeByLabelTool() {
    ResetAiActionSessionState(951);
    SetAiActionPlanGateEnabled(false);
    AiActionHostHooks hooks;
    hooks.onObservePage = [](bool, const std::wstring&, const std::wstring& query) {
        AiNotePageKind(L"dom");
        const std::string json =
            R"({"ok":true,"pageKind":"dom","url":"https://example.com/login","nodes":[)"
            R"({"ref":"e1","role":"textbox","name":"用户名","x":10,"y":100,"w":200,"h":30,"inView":true},)"
            R"({"ref":"e2","role":"button","name":"登录","x":10,"y":200,"w":120,"h":36,"inView":true}]})";
        AiNotePageSnapshot(ParsePageSnapshotJson(json));
        return L"[dom] 登录页（query=" + query + L"）\n- textbox \"用户名\" [ref=e1]\n";
    };
    std::wstring typedRef, typedText;
    hooks.onTypeRef = [&typedRef, &typedText](const std::wstring& ref,
        const std::wstring& text, bool, bool) -> std::wstring {
        typedRef = ref;
        typedText = text;
        return L"已 typeRef(" + ref + L")";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    bool hasTool = false;
    for (const auto& t : tools) {
        if (t.name == L"typeByLabel") hasTool = true;
    }
    const std::wstring filled = CallNamedTool(tools, L"typeByLabel",
        L"{\"label\":\"\u7528\u6237\u540d\",\"text\":\"alice\"}");
    const bool fillOk = filled.rfind(L"[错误]", 0) != 0
        && typedRef == L"e1" && typedText == L"alice"
        && filled.find(L"DOM \u7cbe\u786e\u586b\u5199") != std::wstring::npos;
    // 树上没有该输入框 → 报错并指路 observePage + typeRef
    const std::wstring miss = CallNamedTool(tools, L"typeByLabel",
        L"{\"label\":\"\u90ae\u7bb1\u5730\u5740\",\"text\":\"x\"}");
    const bool missOk = miss.rfind(L"[错误]", 0) == 0
        && miss.find(L"typeRef") != std::wstring::npos;
    // 无扩展钩子 → 明确报错，不静默失败
    const auto bare = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring noHook = CallNamedTool(bare, L"typeByLabel",
        L"{\"label\":\"\u7528\u6237\u540d\",\"text\":\"x\"}");
    const bool noHookOk = noHook.rfind(L"[错误]", 0) == 0;
    // 空 label / 空 text 直接被拒
    const std::wstring emptyLabel = CallNamedTool(tools, L"typeByLabel",
        L"{\"label\":\"\",\"text\":\"x\"}");
    const std::wstring emptyText = CallNamedTool(tools, L"typeByLabel",
        L"{\"label\":\"\u7528\u6237\u540d\",\"text\":\"\"}");
    const bool emptyOk = emptyLabel.rfind(L"[错误]", 0) == 0
        && emptyText.rfind(L"[错误]", 0) == 0;
    // 也算执行类工具（计划门闩 / 执行标记都依赖这两个名单）
    const bool listed = IsMacroActionRunToolName(L"typeByLabel")
        && IsMacroExecutionToolName(L"typeByLabel");
    const bool ok = hasTool && fillOk && missOk && noHookOk && emptyOk && listed;
    Emit(L"type_by_label_tool", ok,
        ok ? L"" : (L"filled=" + filled.substr(0, 100) + L" | miss=" + miss.substr(0, 80)
            + L" | noHook=" + noHook.substr(0, 60)).c_str());
    ResetAiActionSessionState(951);
}

// ── UIA 台账 / 按编号触发：工具层（宿主钩子契约）──────────────────────
void CaseUiControlTools() {
    ResetAiActionSessionState(952);
    SetAiActionPlanGateEnabled(false);
    AiActionHostHooks hooks;
    int listedMax = 0;
    hooks.onListUiControls = [&listedMax](int maxCount) -> std::wstring {
        listedMax = maxCount;
        return L"前台窗口：记事本\n[1] 按钮 \"保存\" @100,200\n[2] 输入框 \"文件名\" @300,400\n";
    };
    int invokedId = 0;
    std::wstring invokedName;
    bool invokedObserve = false;
    hooks.onInvokeUiControl = [&](int id, const std::wstring& name, bool observeAfter) {
        invokedId = id;
        invokedName = name;
        invokedObserve = observeAfter;
        return L"invokeUiControl 已触发「" + name + L"」";
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    bool hasList = false, hasInvoke = false;
    for (const auto& t : tools) {
        if (t.name == L"listUiControls") hasList = true;
        if (t.name == L"invokeUiControl") hasInvoke = true;
    }
    // 台账透传（含 maxCount）
    const std::wstring ledger = CallNamedTool(tools, L"listUiControls",
        L"{\"maxCount\":12}");
    const bool ledgerOk = ledger.find(L"[1]") != std::wstring::npos
        && ledger.find(L"保存") != std::wstring::npos && listedMax == 12;
    // 编号触发：默认 observeAfter=true → [EXECUTED][OBSERVE]
    const std::wstring inv = CallNamedTool(tools, L"invokeUiControl",
        L"{\"id\":1,\"name\":\"\u4fdd\u5b58\"}");
    const bool invOk = invokedId == 1 && invokedName == L"保存" && invokedObserve
        && IsSubmitObserveToolResult(inv);
    // observeAfter=false → [EXECUTED][SKIP_OBSERVE]
    const std::wstring invSkip = CallNamedTool(tools, L"invokeUiControl",
        L"{\"id\":2,\"name\":\"\u6587\u4ef6\u540d\",\"observeAfter\":false}");
    const bool skipOk = IsSubmitSkipObserveToolResult(invSkip);
    // 宿主报错 → 原样透传，不加执行标记
    hooks.onInvokeUiControl = [](int, const std::wstring&, bool) {
        return std::wstring(L"[错误] 找不到该控件");
    };
    const auto tools2 = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring invErr = CallNamedTool(tools2, L"invokeUiControl",
        L"{\"id\":1,\"name\":\"\u4fdd\u5b58\"}");
    const bool errOk = invErr.rfind(L"[错误]", 0) == 0
        && invErr.find(L"[EXECUTED]") == std::wstring::npos;
    // 只给名字（不传 id）也必须能用：界面一变 UIA 编号就错位，名称才是稳定标识
    // （实测模型跨台账复用编号必然对不上，白烧一轮「编号不一致」告警）
    const std::wstring nameOnly = CallNamedTool(tools2, L"invokeUiControl",
        L"{\"name\":\"\u4fdd\u5b58\"}");
    const bool nameOnlyOk = nameOnly.rfind(L"[错误]", 0) == 0;   // tools2 的钩子故意报错
    // 空 name 直接拒（用正常钩子的工具集）
    const std::wstring badName = CallNamedTool(tools, L"invokeUiControl",
        L"{\"id\":1,\"name\":\"\"}");
    const auto bare = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring noHookList = CallNamedTool(bare, L"listUiControls", L"{}");
    const std::wstring noHookInvoke = CallNamedTool(bare, L"invokeUiControl",
        L"{\"id\":1,\"name\":\"\u4fdd\u5b58\"}");
    const bool badOk = nameOnlyOk && badName.rfind(L"[错误]", 0) == 0
        && noHookList.rfind(L"[错误]", 0) == 0 && noHookInvoke.rfind(L"[错误]", 0) == 0;
    // 台账是只读工具：不能被「先写 goal/todos」门闩拦住（否则第一步就没法看界面）
    const bool readExempt = CheckAiActionPlanGate(L"listUiControls").empty();
    // invokeUiControl 属于会改界面的执行类工具
    const bool listed = IsMacroActionRunToolName(L"invokeUiControl")
        && IsMacroExecutionToolName(L"invokeUiControl")
        && IsMacroExecutionToolName(L"listUiControls");
    const bool ok = hasList && hasInvoke && ledgerOk && invOk && skipOk && errOk && badOk
        && readExempt && listed;
    Emit(L"ui_control_tools", ok,
        ok ? L"" : (L"ledger=" + ledger.substr(0, 60) + L" | inv=" + inv.substr(0, 60)
            + L" | skip=" + invSkip.substr(0, 40) + L" | err=" + invErr.substr(0, 40)
            + L" | noHook=" + noHookInvoke.substr(0, 60)).c_str());
    ResetAiActionSessionState(952);
}

// ── 定位模板缓存：键归一 / 命中门槛 / 存取作废 ────────────────────────
void CaseLocateCache() {
    // 键归一：剥通用后缀、折叠空白、统一小写
    const bool normOk = AiLocateNormalizeTarget(L"保存按钮") == L"保存"
        && AiLocateNormalizeTarget(L"  Save  ") == L"save"
        && AiLocateNormalizeTarget(L"下一题") == L"下一题";
    // 命中门槛：高分且唯一才接受
    AiLocateCacheAcceptInput ok1;
    ok1.bestScore = 95.0;
    const bool accept1 = ShouldAcceptAiLocateCacheHit(ok1);
    AiLocateCacheAcceptInput low;
    low.bestScore = 80.0;
    const bool rejectLow = !ShouldAcceptAiLocateCacheHit(low);
    AiLocateCacheAcceptInput twin;
    twin.bestScore = 95.0;
    twin.secondScore = 93.0;
    twin.bestDistToCached = 3;
    twin.secondDistToCached = 5;
    const bool rejectTwin = !ShouldAcceptAiLocateCacheHit(twin);   // 同屏两个都像 → 回识图
    AiLocateCacheAcceptInput farAway;   // 注意：不能叫 far（MSVC 保留字）
    farAway.bestScore = 95.0;
    farAway.secondScore = 93.0;
    farAway.bestDistToCached = 3;
    farAway.secondDistToCached = 220;
    const bool acceptFar = ShouldAcceptAiLocateCacheHit(farAway);  // 次佳离得远 → 仍可信
    AiLocateCacheAcceptInput weak2;
    weak2.bestScore = 95.0;
    weak2.secondScore = 85.0;
    weak2.bestDistToCached = 3;
    weak2.secondDistToCached = 5;
    const bool acceptWeak2 = ShouldAcceptAiLocateCacheHit(weak2);  // 次佳明显低 → 可信

    // 存取 / 作废 / 清理（用 2×2 位图当模板，只验证生命周期不验证匹配）
    AiLocateCacheClear();
    AiLocateCacheKey key;
    key.target = L"保存";
    key.windowTitle = L"记事本";
    key.windowClass = L"Notepad";
    key.captureW = 1280;
    key.captureH = 720;
    HBITMAP tmpl = CreateBitmap(2, 2, 1, 32, nullptr);
    AiLocateCacheStore(key, tmpl, 100, 200, 80, 80);
    AiLocateCacheEntry got;
    const bool stored = AiLocateCacheSize() == 1
        && AiLocateCacheLookup(key, &got) && got.screenX == 100 && got.screenY == 200
        && got.tmpl != nullptr;
    // 窗口变了 → 键不同 → 查不到（不许跨窗口复用坐标）
    AiLocateCacheKey other = key;
    other.windowTitle = L"另一个窗口";
    AiLocateCacheEntry miss;
    const bool windowScoped = !AiLocateCacheLookup(other, &miss);
    // 点击无效果 → 作废
    AiLocateCacheInvalidate(key);
    const bool invalidated = AiLocateCacheSize() == 0 && !AiLocateCacheLookup(key, &got);
    // 再存一条，Clear 必须释放干净
    AiLocateCacheStore(key, CreateBitmap(2, 2, 1, 32, nullptr), 10, 20, 40, 40);
    AiLocateCacheClear();
    const bool cleared = AiLocateCacheSize() == 0;

    const bool ok = normOk && accept1 && rejectLow && rejectTwin && acceptFar
        && acceptWeak2 && stored && windowScoped && invalidated && cleared;
    Emit(L"locate_cache", ok,
        ok ? L"" : (L"norm=" + std::to_wstring(normOk ? 1 : 0) + L" acc="
            + std::to_wstring(accept1 ? 1 : 0) + L"/" + std::to_wstring(rejectLow ? 1 : 0)
            + L"/" + std::to_wstring(rejectTwin ? 1 : 0) + L"/"
            + std::to_wstring(acceptFar ? 1 : 0) + L"/" + std::to_wstring(acceptWeak2 ? 1 : 0)
            + L" store=" + std::to_wstring(stored ? 1 : 0) + L" scope="
            + std::to_wstring(windowScoped ? 1 : 0) + L" inv="
            + std::to_wstring(invalidated ? 1 : 0) + L" clr="
            + std::to_wstring(cleared ? 1 : 0)).c_str());
    AiLocateCacheClear();
}

// ── 非浏览器链路：宽框/地址栏 Enter/URL 判定（本轮日志回归）────────────
void CaseWideRowStillNeedsRefine() {
    auto gate = [](int w, int h) {
        CoarseLocateRefineGateInput in;
        in.haveScreenBox = true;
        in.boxW = w;
        in.boxH = h;
        in.captureW = 2560;      // 2560×1440 观察区（与实测日志一致）
        in.captureH = 1440;
        in.coordsWereRemapped = true;
        in.adaptiveRefineDepth = true;
        return in;
    };
    CoarseLocateSkipReason why = CoarseLocateSkipReason::None;
    // 实测回归：模型给「历史记录」菜单项回了 533×40 的宽框，旧逻辑当「宽控件」直接点中心，
    // 结果点开了「设置」。现在这种过分宽的框必须继续做 Zoom 精炼。
    const bool menuRow = !ShouldAcceptCoarseLocateWithoutRefine(gate(533, 40), &why);
    // 真正的工具栏宽按钮（≤420px）仍可省一轮
    const bool wideBtn = ShouldAcceptCoarseLocateWithoutRefine(gate(300, 30), &why);
    // 地址栏/公式栏那种「很扁很长」仍由 thinWideBar 放行，不受本次收紧影响
    const bool addrBar = ShouldAcceptCoarseLocateWithoutRefine(gate(1500, 24), &why);
    // 紧凑小按钮照旧跳过二级
    const bool compact = ShouldAcceptCoarseLocateWithoutRefine(gate(120, 36), &why);
    // ★实测回归（游戏/自绘前台，用户报「选个卡牌、点个按钮都要十几秒」）：
    // 模型对「正常模式（要过关点这个）」回粗框 245×93 —— 旧逻辑 compactBox（宽 > 观察区 8%）
    // 与 wideControl（高 > 56）双双拒绝 → 白烧一轮 15s 的二级 Zoom，而那一轮还答错
    // （模型答成「编辑模式」，漂移被拒，最后仍点一级框）。现在按**相对面积 ≤1%** 放行。
    why = CoarseLocateSkipReason::None;
    const bool smallLabel = ShouldAcceptCoarseLocateWithoutRefine(gate(245, 93), &why)
        && why == CoarseLocateSkipReason::SmallLabel;
    // 选卡画面里的僵尸卡牌（~96×140）：同样不该为它再烧一轮 API
    why = CoarseLocateSkipReason::None;
    const bool cardCell = ShouldAcceptCoarseLocateWithoutRefine(gate(96, 140), &why);
    // 半屏面板（相对面积大）仍必须 Zoom：别把「整块面板中心」当按钮点
    why = CoarseLocateSkipReason::None;
    const bool panelForce = !ShouldAcceptCoarseLocateWithoutRefine(gate(1100, 700), &why);
    // 过宽的行（533×40）继续走 Zoom：这是「比真行更宽」的菜单行，点中心会点错
    const bool ok = menuRow && wideBtn && addrBar && compact && smallLabel
        && cardCell && panelForce;
    Emit(L"wide_row_still_needs_refine", ok,
        (L"menuRow(no-skip)=" + std::to_wstring(menuRow ? 1 : 0) + L" wideBtn="
            + std::to_wstring(wideBtn ? 1 : 0) + L" addrBar="
            + std::to_wstring(addrBar ? 1 : 0) + L" compact="
            + std::to_wstring(compact ? 1 : 0)
            + L" smallLabel=" + std::to_wstring(smallLabel ? 1 : 0)
            + L" cardCell=" + std::to_wstring(cardCell ? 1 : 0)
            + L" panelForce=" + std::to_wstring(panelForce ? 1 : 0)).c_str());
}

void CaseUrlInputHeuristic() {
    const bool pos = LooksLikeUrlInput(L"edge://history")
        && LooksLikeUrlInput(L"https://www.example.com/a")
        && LooksLikeUrlInput(L"www.baidu.com")
        && LooksLikeUrlInput(L"localhost:8080")
        && LooksLikeUrlInput(L"example.com");
    const bool neg = !LooksLikeUrlInput(L"历史记录")
        && !LooksLikeUrlInput(L"搜索 关键词")
        && !LooksLikeUrlInput(L"用户名")
        && !LooksLikeUrlInput(L"a")
        && !LooksLikeUrlInput(L"保存并关闭")
        && !LooksLikeUrlInput(L"user name");
    Emit(L"url_input_heuristic", pos && neg,
        (L"pos=" + std::to_wstring(pos ? 1 : 0) + L" neg="
            + std::to_wstring(neg ? 1 : 0)).c_str());
}

// 页面树已建立时 Enter 默认被拦；但「Ctrl+L → 输入 URL → Enter」是正常导航手法，必须放行。
void CaseEnterAfterUrlInput() {
    ResetAiActionSessionState(961);
    SetAiActionPlanGateEnabled(false);
    AiNotePageKind(L"dom");
    SetAiForegroundBrowserOverrideForTest(1);   // 「网页控件树拦 Enter」要生效
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) { return std::wstring(L"已执行"); };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    // 先直接 Enter：应被「已有网页控件树」拦住
    const std::wstring bare = CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"Enter\"}");
    const bool blocked = bare.rfind(L"[错误]", 0) == 0
        && bare.find(L"searchOnPage") != std::wstring::npos;
    // 输入像 URL 的正文后再 Enter：应放行
    const std::wstring typed = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"edge://history\",\"clearFirst\":true}");
    const std::wstring afterUrl = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"Enter\"}");
    const bool allowed = afterUrl.rfind(L"[错误]", 0) != 0;
    // 输入普通搜索词后再 Enter：仍应被拦（站内搜索请走 searchOnPage）
    const std::wstring typedWord = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"\u5386\u53f2\u8bb0\u5f55\",\"clearFirst\":true}");
    const std::wstring afterWord = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"Enter\"}");
    const bool stillBlocked = afterWord.rfind(L"[错误]", 0) == 0;
    const bool ok = blocked && allowed && stillBlocked;
    Emit(L"enter_after_url_input", ok,
        ok ? L"" : (L"bare=" + bare.substr(0, 60) + L" | url=" + afterUrl.substr(0, 60)
            + L" | word=" + afterWord.substr(0, 60)).c_str());
    ResetAiActionSessionState(961);
}

void CaseBrowserTitleHintStrip() {
    // 实测回归：前台已是「设置」页，但窗口标题带装饰 → 扩展匹配不到标签，一直回旧页面树
    const std::wstring decorated = L"设置 和另外 4 个页面 - 个人 - Microsoft Edge";
    const std::wstring stripped = StripBrowserWindowTitleDecorations(decorated);
    const bool main = stripped == L"设置";
    const bool profileEn = StripBrowserWindowTitleDecorations(
        L"Downloads - Profile 1 - Google Chrome") == L"Downloads";
    const bool andMoreEn = StripBrowserWindowTitleDecorations(
        L"Settings and 3 more pages - Microsoft Edge") == L"Settings";
    const bool plain = StripBrowserWindowTitleDecorations(L"百度一下，你就知道") == L"百度一下，你就知道";
    // 别把普通窗口标题里的「 - 」当成装饰剥掉
    const bool keepDash = StripBrowserWindowTitleDecorations(L"报表 - Excel") == L"报表 - Excel";
    const bool ok = main && profileEn && andMoreEn && plain && keepDash;
    Emit(L"browser_title_hint_strip", ok,
        (L"stripped=[" + stripped + L"] en=[" + StripBrowserWindowTitleDecorations(
            L"Download - Profile 1 - Google Chrome") + L"]").c_str());
}

// ── 命令行路线：runCommand 必须落到既有的「运行程序」动作上 ─────────────
void CaseRunCommandTool() {
    ResetAiActionSessionState(971);
    SetAiActionPlanGateEnabled(false);
    std::wstring lastActions;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [&lastActions](const std::wstring& json) {
        lastActions = json;
        return std::wstring(L"已执行");
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    bool hasTool = false;
    for (const auto& t : tools) {
        if (t.name == L"runCommand") hasTool = true;
    }
    // powershell（默认）：动作必须是 runProgram + 程序 powershell + 参数带 -Command
    const std::wstring ps = CallNamedTool(tools, L"runCommand",
        L"{\"command\":\"Get-Date | Out-File C:\\\\t.txt\",\"waitMs\":0}");
    const bool psOk = ps.rfind(L"[错误]", 0) != 0
        && lastActions.find(L"runProgram") != std::wstring::npos
        && lastActions.find(L"powershell") != std::wstring::npos
        && lastActions.find(L"-Command") != std::wstring::npos
        && IsSubmitObserveToolResult(ps);
    // cmd：走 /c
    const std::wstring cmd = CallNamedTool(tools, L"runCommand",
        L"{\"shell\":\"cmd\",\"command\":\"dir\",\"waitMs\":0}");
    const bool cmdOk = cmd.rfind(L"[错误]", 0) != 0
        && lastActions.find(L"cmd") != std::wstring::npos
        && lastActions.find(L"/c") != std::wstring::npos;
    // 空命令/无宿主：明确报错
    const std::wstring empty = CallNamedTool(tools, L"runCommand", L"{\"command\":\"\"}");
    const auto bare = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring noHook = CallNamedTool(bare, L"runCommand",
        L"{\"command\":\"echo 1\"}");
    const bool errOk = empty.rfind(L"[错误]", 0) == 0 && noHook.rfind(L"[错误]", 0) == 0;
    // ★交给宿主的必须是**纯 JSON 数组**：构建器会在数组后追加「[提示] 已自动…stopMacro…」，
    // 提示自带 [ ] —— 引擎早期用裸 rfind(']') 取结尾会连提示一起吞掉 → 「JSON 解析失败」，
    // AI 最省事的命令行路线第一次调用就死掉。这里直接解一遍，防回归。
    bool jsonOk = false;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(ToUtf8(lastActions));
        jsonOk = parsed.is_array() && !parsed.empty();
    } catch (...) {
        jsonOk = false;
    }
    // 命令里带 ] 也不能被截断（PowerShell 里 $env:X[0]、[Environment] 很常见）
    const std::wstring bracketCmd = CallNamedTool(tools, L"runCommand",
        L"{\"command\":\"$a=@(1,2); $a[0] | Out-File C:\\\\b.txt\",\"waitMs\":0}");
    bool bracketOk = false;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(ToUtf8(lastActions));
        bracketOk = parsed.is_array() && !parsed.empty()
            && lastActions.find(L"[0]") != std::wstring::npos;
    } catch (...) {
        bracketOk = false;
    }
    // 与运行程序同族：能进逻辑转化/回放（执行类工具名单）
    const bool listed = IsMacroActionRunToolName(L"runCommand")
        && IsMacroExecutionToolName(L"runCommand");
    const bool ok = hasTool && psOk && cmdOk && errOk && listed && jsonOk && bracketOk;
    Emit(L"run_command_tool", ok,
        (L"ps=" + std::to_wstring(psOk) + L"/cmd=" + std::to_wstring(cmdOk)
            + L"/err=" + std::to_wstring(errOk) + L"/listed=" + std::to_wstring(listed)
            + L"/json=" + std::to_wstring(jsonOk) + L"/bracket=" + std::to_wstring(bracketOk)
            + L" | " + (ok ? std::wstring() : lastActions.substr(0, 100))).c_str());
    ResetAiActionSessionState(971);
}

// ── 动作 JSON 抽取：带 [提示] 尾巴 / 字符串里有 ] 都必须取对 ─────────────
void CaseExtractActionJsonArray() {
    const std::wstring withHint =
        L"[\n{\"type\":\"runProgram\",\"targetPath\":\"powershell\"}\n]"
        L"\n[提示] 已自动在末尾追加 stopMacro（结束宏运行），避免脚本无限重复执行。";
    const std::wstring extracted = ExtractActionJsonArrayText(withHint);
    bool hintOk = false;
    try {
        hintOk = nlohmann::json::parse(ToUtf8(extracted)).is_array();
    } catch (...) {
        hintOk = false;
    }
    // 正文里含 ] 与 ] 后面的内容：必须按括号配对 + 跳过字符串
    const std::wstring inString =
        L"[{\"type\":\"quickInput\",\"inputText\":\"$a[0]] b\"},{\"type\":\"wait\"}]";
    const std::wstring ex2 = ExtractActionJsonArrayText(inString);
    bool strOk = false;
    try {
        const nlohmann::json j = nlohmann::json::parse(ToUtf8(ex2));
        strOk = j.is_array() && j.size() == 2
            && FromUtf8(j[0]["inputText"].get<std::string>()) == L"$a[0]] b";
    } catch (...) {
        strOk = false;
    }
    const bool noneOk = ExtractActionJsonArrayText(L"没有数组").empty()
        && ExtractActionJsonArrayText(L"").empty();
    // 内置页识别（决定要不要核对地址栏导航到底成没成）
    const bool urlOk = IsBrowserInternalUrl(L"edge://history")
        && IsBrowserInternalUrl(L"CHROME://downloads")
        && IsBrowserInternalUrl(L"about:blank")
        && !IsBrowserInternalUrl(L"https://example.com/edge://x")
        && !IsBrowserInternalUrl(L"");
    Emit(L"extract_action_json_array", hintOk && strOk && noneOk && urlOk,
        (L"提示尾巴=" + std::to_wstring(hintOk) + L"/字符串含]= " + std::to_wstring(strOk)
            + L"/空=" + std::to_wstring(noneOk) + L"/内置页=" + std::to_wstring(urlOk)).c_str());
}

// ── 浏览器内置页：首选「让浏览器自己带 URL 打开」，退回地址栏合成键 ──────
void CaseOpenWebpageInternalPage() {
    ResetAiActionSessionState(981);
    SetAiActionPlanGateEnabled(false);
    std::wstring acted;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [&acted](const std::wstring& json) {
        acted = json;
        return std::wstring(L"已执行");
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    // ① 前台是浏览器（自检里用存在的文件当浏览器启动目标）→ 走 runProgram 带 URL：
    //    不再合成 Ctrl+L 打字（实测会把 "edge://history" 打成 "dge://history" → 进搜索栏）
    SetAiBrowserLaunchTargetOverrideForTest(L"C:\\Windows\\System32\\cmd.exe");
    const std::wstring hist = CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"edge://history\"}");
    const bool launchOk = hist.rfind(L"[错误]", 0) != 0
        && acted.find(L"runProgram") != std::wstring::npos
        && acted.find(L"edge://history") != std::wstring::npos
        && acted.find(L"keyClick") == std::wstring::npos
        && acted.find(L"quickInput") == std::wstring::npos;
    // ② 识别不出浏览器 → 退回地址栏合成键，但必须带等待（首字母被吞就是「刚聚焦就打字」）
    ResetAiActionSessionState(981);
    SetAiActionPlanGateEnabled(false);
    SetAiForegroundBrowserOverrideForTest(-1);
    const auto tools2 = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring hist2 = CallNamedTool(tools2, L"openWebpage",
        L"{\"targetPath\":\"edge://history\"}");
    const bool fallbackOk = hist2.rfind(L"[错误]", 0) != 0
        && acted.find(L"keyClick") != std::wstring::npos
        && acted.find(L"quickInput") != std::wstring::npos
        && acted.find(L"wait") != std::wstring::npos
        && acted.find(L"edge://history") != std::wstring::npos
        && acted.find(L"Enter") != std::wstring::npos;
    // 两条路线都必须提醒「核对是否真到了该页」
    const bool warnOk = hist.find(L"若还停在旧页面") != std::wstring::npos
        || hist2.find(L"务必核对") != std::wstring::npos;
    Emit(L"open_webpage_internal_page", launchOk && fallbackOk && warnOk,
        (L"launch=" + std::to_wstring(launchOk) + L"/fallback=" + std::to_wstring(fallbackOk)
            + L"/warn=" + std::to_wstring(warnOk) + L" | " + acted.substr(0, 140)).c_str());
    ResetAiActionSessionState(981);
}

// ── 配方复用：Ctrl+Home 只提醒不拦；写完后 Ctrl+A 必须先确认 ─────────────
void CaseRecipeReuseGuards() {
    ResetAiActionSessionState(983);
    SetAiActionPlanGateEnabled(false);
    int executed = 0;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [&executed](const std::wstring& json) {
        ++executed;
        return std::wstring(L"已执行");
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    // ① 模板里含 Ctrl+Home：**不拦**（有些复用就是要每组回到固定区域覆盖填写），
    //    只把后果说清楚，让模型自己判断——硬拦会让这类合法用法抓瞎
    const std::wstring homeTemplate = CallNamedTool(tools, L"runActionRecipe",
        L"{\"steps\":[{\"type\":\"keyClick\",\"keyText\":\"Home\",\"holdLeftCtrl\":true},"
        L"{\"type\":\"quickInput\",\"inputText\":\"{0}\"},{\"type\":\"keyClick\",\"keyText\":\"Tab\"}],"
        L"\"rows\":[[\"a\"],[\"b\"],[\"c\"]]}");
    const bool warnedNotBlocked = homeTemplate.rfind(L"[错误]", 0) != 0
        && homeTemplate.find(L"Ctrl+Home") != std::wstring::npos
        && homeTemplate.find(L"忽略即可") != std::wstring::npos
        && executed > 0;
    // ② 正常模板（行内 Tab/Enter/Home）→ 先试跑前 2 组，并给一次路线指针
    ResetAiActionSessionState(984);
    SetAiActionPlanGateEnabled(false);
    executed = 0;
    const auto tools2 = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring good = CallNamedTool(tools2, L"runActionRecipe",
        L"{\"steps\":[{\"type\":\"quickInput\",\"inputText\":\"{0}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Tab\"},{\"type\":\"keyClick\",\"keyText\":\"Enter\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Home\"}],"
        L"\"rows\":[[\"甲\"],[\"乙\"],[\"丙\"]]}");
    const bool previewed = good.find(L"强制试跑") != std::wstring::npos
        && good.find(L"lookupMacroAction(section=command)") != std::wstring::npos;
    // 路线指针每任务只给一次：第二次调用不再重复
    const std::wstring second = CallNamedTool(tools2, L"runActionRecipe",
        L"{\"steps\":[{\"type\":\"quickInput\",\"inputText\":\"{0}\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Tab\"},{\"type\":\"keyClick\",\"keyText\":\"Enter\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"Home\"}],"
        L"\"rows\":[[\"甲\"],[\"乙\"],[\"丙\"]]}");
    const bool nudgeOnce = second.find(L"路线提示") == std::wstring::npos;
    // ③ 已写入 ≥2 组后按 Ctrl+A → 必须提示（实测就是这个把已写的前两行清掉的）
    const std::wstring ctrlA = CallNamedTool(tools2, L"keyClick",
        L"{\"keyText\":\"a\",\"holdLeftCtrl\":true}");
    const bool guardCtrlA = ctrlA.find(L"[提示]") == 0
        && ctrlA.find(L"全选整表") != std::wstring::npos
        && ctrlA.find(L"confirmShortcut") != std::wstring::npos;
    // ④ 明确确认后仍放行（不做死锁）
    const std::wstring ctrlAOk = CallNamedTool(tools2, L"keyClick",
        L"{\"keyText\":\"a\",\"holdLeftCtrl\":true,\"confirmShortcut\":true,\"observeAfter\":false}");
    const bool confirmOk = ctrlAOk.find(L"[EXECUTED]") != std::wstring::npos;
    // ⑤ 路线指针也会在「在表格软件里逐格输入」时出现（Skill + 工具函数配合，不占系统提示词）
    ResetAiActionSessionState(985);
    SetAiActionPlanGateEnabled(false);
    SetAiSpreadsheetForegroundOverrideForTest(1);
    const auto tools3 = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring cell = CallNamedTool(tools3, L"quickInput",
        L"{\"inputText\":\"序号\",\"clearFirst\":false}");
    const bool cellNudge = cell.find(L"lookupMacroAction(section=command)") != std::wstring::npos;
    const bool ok = warnedNotBlocked && previewed && nudgeOnce && guardCtrlA && confirmOk
        && cellNudge;
    Emit(L"recipe_reuse_guards", ok,
        (L"Ctrl+Home只提醒=" + std::to_wstring(warnedNotBlocked) + L"/试跑+指针="
            + std::to_wstring(previewed) + L"/指针只一次=" + std::to_wstring(nudgeOnce)
            + L"/拦Ctrl+A=" + std::to_wstring(guardCtrlA) + L"/确认放行="
            + std::to_wstring(confirmOk) + L"/表格输入指针=" + std::to_wstring(cellNudge)).c_str());
    ResetAiActionSessionState(983);
}

// ── DOM 优先门禁：模态对话框（标题常为空）绝不能让 clickRef 打进浏览器 ────
void CaseDomFirstDialogGate() {
    DomFirstActionGateInput base;
    base.extensionConnected = true;
    base.hasDomHooks = true;
    base.foregroundLooksBrowser = true;
    const bool allowBrowser = ShouldUseDomFirstAction(base, nullptr);
    // 前台是另存为对话框：标题空 + 会话仍认为在网页 → 旧逻辑放行（实测把 clickRef 打进了
    // 浏览器 DOM，随后 activateWindow 又被模态框挡住，模型整轮卡在保存界面）
    DomFirstActionGateInput dlg = base;
    dlg.foregroundLooksBrowser = false;
    dlg.foregroundBrowserClass = false;
    dlg.webSessionActive = true;
    dlg.foregroundTitleReadable = false;
    std::wstring why;
    const bool rejectDialog = !ShouldUseDomFirstAction(dlg, &why)
        && why.find(L"前台不是浏览器") != std::wstring::npos;
    // 标题读不出来但窗口类确实是 Chromium/火狐 → 仍可按树操作
    DomFirstActionGateInput clsOnly = base;
    clsOnly.foregroundLooksBrowser = false;
    clsOnly.foregroundBrowserClass = true;
    clsOnly.foregroundTitleReadable = false;
    const bool allowByClass = ShouldUseDomFirstAction(clsOnly, nullptr);
    DomFirstActionGateInput rightClick = base;
    rightClick.rightButton = true;
    DomFirstActionGateInput canvas = base;
    canvas.pageIsCanvas = true;
    const bool otherGates = !ShouldUseDomFirstAction(rightClick, nullptr)
        && !ShouldUseDomFirstAction(canvas, nullptr);
    Emit(L"dom_first_dialog_gate", allowBrowser && rejectDialog && allowByClass && otherGates,
        (L"浏览器放行=" + std::to_wstring(allowBrowser) + L"/对话框拒绝="
            + std::to_wstring(rejectDialog) + L"/按窗口类放行=" + std::to_wstring(allowByClass)
            + L"/其它门=" + std::to_wstring(otherGates)).c_str());
}

// ── 路线指针：自带可照抄命令、每任务一次（实测「只给指针」模型不会去查）─────
void CaseRouteNudge() {
    ResetAiActionSessionState(986);
    SetAiActionPlanGateEnabled(false);
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) { return std::wstring(L"已执行"); };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring data = CallNamedTool(tools, L"saveTaskData",
        L"{\"name\":\"historyRecords\",\"content\":\"1|标题A|a.com|23:54\\n2|标题B|b.com|23:52\"}");
    const bool nudged = data.find(L"runCommand") != std::wstring::npos
        && data.find(L"Out-File") != std::wstring::npos
        && data.find(L"lookupMacroAction(section=command)") != std::wstring::npos;
    // 同一任务里再存一次表格数据 → 不再重复提示（每任务只一次）
    const std::wstring again = CallNamedTool(tools, L"saveTaskData",
        L"{\"name\":\"historyRecords2\",\"content\":\"3|标题C|c.com|23:50\\n4|标题D|d.com|23:49\"}");
    const bool once = again.find(L"路线提示") == std::wstring::npos;
    // 非表格内容（单行）不该打扰
    ResetAiActionSessionState(987);
    SetAiActionPlanGateEnabled(false);
    const auto tools2 = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring plain = CallNamedTool(tools2, L"saveTaskData",
        L"{\"name\":\"note\",\"content\":\"一句话备注\"}");
    const bool quiet = plain.find(L"路线提示") == std::wstring::npos;
    Emit(L"route_nudge", nudged && quiet && once,
        (L"表格数据提示=" + std::to_wstring(nudged) + L"/非表格不打扰=" + std::to_wstring(quiet)
            + L"/只一次=" + std::to_wstring(once)).c_str());
    ResetAiActionSessionState(986);
}

// ── 热键动作描述必须按**实键**显示（预设标签与实际不符会误导日志/编辑器）────
void CaseHotkeyLabelRealKeys() {
    ScriptAction a;
    a.type = ActionType::HotkeyShortcut;
    a.keyVk = 'S';
    a.holdLeftCtrl = true;
    a.shortcutPreset = 0;   // 误留默认预设索引（AI 直接给组合键时就是这样）
    const std::wstring name = ActionName(a);
    const bool realKeys = name.find(L"Ctrl+S") != std::wstring::npos
        && name.find(L"Ctrl+C") == std::wstring::npos;
    ScriptAction b;
    b.type = ActionType::HotkeyShortcut;
    const auto& p0 = ShortcutPresetAt(0);
    b.keyVk = p0.vk;
    b.holdLeftCtrl = p0.ctrl;
    b.shortcutPreset = 0;
    const bool presetLabel = ActionName(b).find(L"Ctrl+C") != std::wstring::npos;
    Emit(L"hotkey_label_real_keys", realKeys && presetLabel,
        (L"实键=" + name + L" | 预设=" + ActionName(b)).c_str());
}

// ── 办公文档：readDocument 工具 + 提取脚本（Excel/Word/PPT/PDF/CSV/文本）─────
void CaseReadDocumentTool() {
    ResetAiActionSessionState(988);
    SetAiActionPlanGateEnabled(false);
    const std::wstring dir = MakeTempTestDir(L"readdoc_test");
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring csvPath = dir + L"\\sample.csv";
    const std::wstring txtPath = dir + L"\\note.txt";
    // ★必须用 Win32 API 写：std::ofstream 的窄字符串路径走 ANSI 代码页，
    // 用户目录含中文（冯思乾）时会被写到乱码文件名下（自检就会「文件不存在」）。
    auto writeFileBytes = [](const std::wstring& path, const std::string& bytes) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
            &written, nullptr);
        CloseHandle(h);
        return ok == TRUE && written == bytes.size();
    };
    const bool wroteCsv = writeFileBytes(csvPath,
        "\xEF\xBB\xBF" "序号,标题\r\n1,费用中心-火山引擎\r\n2,中文测试\r\n");
    const bool wroteTxt = writeFileBytes(txtPath, "\xEF\xBB\xBF" "第一行中文\nsecond line\n");
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) { return std::wstring(L"已执行"); };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    bool hasTool = false;
    for (const auto& t : tools) {
        if (t.name == L"readDocument") hasTool = true;
    }
    auto pathParam = [](const std::wstring& p) {
        std::wstring esc;
        for (wchar_t c : p) {
            if (c == L'\\') esc += L"\\\\";
            else esc += c;
        }
        return esc;
    };
    // ① CSV：中文必须原样读出（不能乱码），并带类型说明
    const std::wstring csvOut = CallNamedTool(tools, L"readDocument",
        L"{\"path\":\"" + pathParam(csvPath) + L"\"}");
    const bool csvOk = csvOut.find(L"费用中心-火山引擎") != std::wstring::npos
        && csvOut.find(L"中文测试") != std::wstring::npos
        && csvOut.find(L"表格文本") != std::wstring::npos;
    // ② 纯文本
    const std::wstring txtOut = CallNamedTool(tools, L"readDocument",
        L"{\"path\":\"" + pathParam(txtPath) + L"\",\"maxChars\":200}");
    const bool txtOk = txtOut.find(L"第一行中文") != std::wstring::npos
        && txtOut.find(L"second line") != std::wstring::npos;
    // ③ 不存在的文件 / 不支持的扩展名 → 可执行的错误
    const std::wstring missing = CallNamedTool(tools, L"readDocument",
        L"{\"path\":\"" + pathParam(dir + L"\\nope.csv") + L"\"}");
    const std::wstring badExt = CallNamedTool(tools, L"readDocument",
        L"{\"path\":\"" + pathParam(dir + L"\\a.xyz") + L"\"}");
    const bool errOk = missing.rfind(L"[错误]", 0) == 0
        && missing.find(L"不存在") != std::wstring::npos
        && badExt.rfind(L"[错误]", 0) == 0;
    // ④ 扩展名支持表
    std::wstring fmt;
    const bool typesOk = IsSupportedOfficeDocument(L"C:\\x\\a.xlsx", &fmt) && fmt == L"xlsx"
        && IsSupportedOfficeDocument(L"C:\\x\\a.PDF", &fmt) && fmt == L"pdf"
        && IsSupportedOfficeDocument(L"C:\\x\\a.docx") && IsSupportedOfficeDocument(L"C:\\x\\a.pptx")
        && !IsSupportedOfficeDocument(L"C:\\x\\a.exe")
        && OfficeDocFormatLabel(L"xlsx").find(L"Excel") != std::wstring::npos;
    DeleteFileW(csvPath.c_str());
    DeleteFileW(txtPath.c_str());
    RemoveDirectoryW(dir.c_str());
    Emit(L"read_document_tool",
        hasTool && wroteCsv && wroteTxt && csvOk && txtOk && errOk && typesOk,
        (L"工具=" + std::to_wstring(hasTool) + L"/写入=" + std::to_wstring(wroteCsv && wroteTxt)
            + L"/CSV=" + std::to_wstring(csvOk) + L"/文本=" + std::to_wstring(txtOk)
            + L"/错误=" + std::to_wstring(errOk) + L"/类型=" + std::to_wstring(typesOk)
            + L" | " + csvOut.substr(0, 70)).c_str());
    ResetAiActionSessionState(988);
}

// ── 定位缓存：窗口位移重锚 + N-of-M 迟滞（动态画面不点残影）──────────────
void CaseLocateCacheHysteresis() {
    RECT stored{100, 200, 1100, 900};
    RECT moved{220, 280, 1220, 980};
    int ex = 0, ey = 0;
    AiLocateCacheReanchor(500, 400, stored, moved, &ex, &ey);
    const bool reanchorOk = ex == 620 && ey == 480;
    RECT same{100, 200, 1100, 900};
    AiLocateCacheReanchor(500, 400, same, same, &ex, &ey);
    const bool sameOk = ex == 500 && ey == 400;
    const bool hysOk = !ShouldAcceptAiLocateCacheHitHysteresis(1, 3)
        && ShouldAcceptAiLocateCacheHitHysteresis(2, 3)
        && ShouldAcceptAiLocateCacheHitHysteresis(3, 3)
        && !ShouldAcceptAiLocateCacheHitHysteresis(1, 1)
        && !ShouldAcceptAiLocateCacheHitHysteresis(0, 3);
    AiLocateCacheAcceptInput strong;
    strong.bestScore = 97.0;
    strong.bestDistToCached = 3;
    AiLocateCacheAcceptInput ambiguous;
    ambiguous.bestScore = 97.0;
    ambiguous.bestDistToCached = 3;
    ambiguous.secondScore = 96.0;
    ambiguous.secondDistToCached = 6;
    const bool gateOk = ShouldAcceptAiLocateCacheHit(strong)
        && !ShouldAcceptAiLocateCacheHit(ambiguous);
    Emit(L"locate_cache_hysteresis", reanchorOk && sameOk && hysOk && gateOk,
        (L"重锚=" + std::to_wstring(reanchorOk) + L"/同位=" + std::to_wstring(sameOk)
            + L"/迟滞=" + std::to_wstring(hysOk) + L"/门槛=" + std::to_wstring(gateOk)).c_str());
}

// ── computer-use 兼容入口：computer 别名必须映射到既有动作，且不绕过守卫 ────
void CaseComputerTool() {
    ResetAiActionSessionState(989);
    SetAiActionPlanGateEnabled(false);
    SetAiForegroundBrowserOverrideForTest(-1);      // 前台不是浏览器（确定性）
    SetAiSpreadsheetForegroundOverrideForTest(-1);  // 前台不是表格
    std::wstring lastActions;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [&lastActions](const std::wstring& json) {
        lastActions = json;
        return std::wstring(L"已执行");
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    bool hasTool = false;
    for (const auto& t : tools) {
        if (t.name == L"computer") hasTool = true;
    }
    // ① screenshot：刷新观察帧（返回 OBSERVE 标记）
    const std::wstring shot = CallNamedTool(tools, L"computer", L"{\"action\":\"screenshot\"}");
    const bool shotOk = shot.find(L"[EXECUTED][OBSERVE]") != std::wstring::npos
        && shot.find(L"0~1000") != std::wstring::npos;
    // ①b ★「模型明确要了截图」必须让宿主下一次观察强制回传帧（历史旧图已被剥成
    //     「(历史截图已省略)」，省掉这帧 = 模型全盲，实测它反复自问「我看不到图」空转）。
    //     标记必须是消费一次即清（否则后续每轮都强制上传，省上传优化就废了）。
    const bool shotForce = AiTakeExplicitScreenshotRequest()
        && !AiTakeExplicitScreenshotRequest();
    // ①c 其它 action（不涉及看画面）不得留下「要截图」标记
    const bool noStrayForce = !AiTakeExplicitScreenshotRequest();
    // ② left_click(coordinate) → mouseClick（坐标原样透传，由引擎做归一化/像素判定）
    const std::wstring click = CallNamedTool(tools, L"computer",
        L"{\"action\":\"left_click\",\"coordinate\":[512,384]}");
    const bool clickOk = click.find(L"[EXECUTED]") != std::wstring::npos
        && lastActions.find(L"mouseClick") != std::wstring::npos
        && lastActions.find(L"\"x\": 512") != std::wstring::npos
        && lastActions.find(L"\"y\": 384") != std::wstring::npos;
    // ③ double_click / right_click 映射到 clickCount / button
    const std::wstring dbl = CallNamedTool(tools, L"computer",
        L"{\"action\":\"double_click\",\"coordinate\":[10,20]}");
    const bool dblOk = lastActions.find(L"\"clickCount\": 2") != std::wstring::npos;
    const std::wstring rc = CallNamedTool(tools, L"computer",
        L"{\"action\":\"right_click\",\"coordinate\":[10,20]}");
    const bool rcOk = lastActions.find(L"\"right\"") != std::wstring::npos;
    // ④ key 组合键 "ctrl+s" → keyClick + holdLeftCtrl
    const std::wstring key = CallNamedTool(tools, L"computer",
        L"{\"action\":\"key\",\"key\":\"ctrl+s\"}");
    const bool keyOk = lastActions.find(L"keyClick") != std::wstring::npos
        && lastActions.find(L"holdLeftCtrl") != std::wstring::npos;
    // ⑤ type → quickInput（computer-use 语义=光标处输入；不得带 clearFirst=true 去替换已有内容）
    const std::wstring typed = CallNamedTool(tools, L"computer",
        L"{\"action\":\"type\",\"text\":\"\u5e8f\u53f7\"}");
    const bool typeOk = typed.find(L"[EXECUTED]") != std::wstring::npos
        && lastActions.find(L"quickInput") != std::wstring::npos
        && lastActions.find(L"\"clearFirst\": true") == std::wstring::npos;
    // ⑥ scroll → scrollWheel（方向/步数）
    const std::wstring scroll = CallNamedTool(tools, L"computer",
        L"{\"action\":\"scroll\",\"scroll_direction\":\"up\",\"scroll_amount\":4}");
    const bool scrollOk = lastActions.find(L"scrollWheel") != std::wstring::npos
        && lastActions.find(L"\"scrollSteps\": 4") != std::wstring::npos;
    // ⑦ hold_key → keyDown + wait + keyUp 一批
    const std::wstring hold = CallNamedTool(tools, L"computer",
        L"{\"action\":\"hold_key\",\"key\":\"w\",\"duration\":0.4}");
    const bool holdOk = lastActions.find(L"keyDown") != std::wstring::npos
        && lastActions.find(L"keyUp") != std::wstring::npos
        && lastActions.find(L"wait") != std::wstring::npos;
    // ⑧ cursor_position：直接回屏幕坐标，不落动作
    lastActions.clear();
    const std::wstring cursor = CallNamedTool(tools, L"computer", L"{\"action\":\"cursor_position\"}");
    const bool cursorOk = cursor.find(L"光标屏幕坐标") != std::wstring::npos && lastActions.empty();
    // ⑨ 守卫不能被绕过：网页会话 + 前台是浏览器 → 拒绝坐标点击并指向 clickRef
    SetAiForegroundBrowserOverrideForTest(1);
    AiNotePageKind(L"dom");
    const std::wstring webClick = CallNamedTool(tools, L"computer",
        L"{\"action\":\"left_click\",\"coordinate\":[100,100]}");
    const bool webGuard = webClick.rfind(L"[错误]", 0) == 0
        && webClick.find(L"clickRef") != std::wstring::npos;
    // 表格前台 → 拒绝点网格
    SetAiForegroundBrowserOverrideForTest(-1);
    SetAiSpreadsheetForegroundOverrideForTest(1);
    const std::wstring sheetClick = CallNamedTool(tools, L"computer",
        L"{\"action\":\"left_click\",\"coordinate\":[100,100]}");
    const bool sheetGuard = sheetClick.rfind(L"[错误]", 0) == 0
        && sheetClick.find(L"表格") != std::wstring::npos;
    SetAiSpreadsheetForegroundOverrideForTest(-1);
    // ⑩ 未知 action / 缺参数 → 可执行错误
    const std::wstring bad = CallNamedTool(tools, L"computer", L"{\"action\":\"teleport\"}");
    const std::wstring noCoord = CallNamedTool(tools, L"computer", L"{\"action\":\"left_click\"}");
    const bool errOk = bad.rfind(L"[错误]", 0) == 0 && noCoord.rfind(L"[错误]", 0) == 0;
    const bool ok = hasTool && shotOk && shotForce && noStrayForce && clickOk && dblOk
        && rcOk && keyOk && typeOk
        && scrollOk && holdOk && cursorOk && webGuard && sheetGuard && errOk;
    Emit(L"computer_alias_tool", ok,
        (L"存在=" + std::to_wstring(hasTool) + L"/截图=" + std::to_wstring(shotOk)
            + L"/截图强制回传=" + std::to_wstring(shotForce ? 1 : 0)
            + L"/无残留标记=" + std::to_wstring(noStrayForce ? 1 : 0)
            + L"/点击=" + std::to_wstring(clickOk) + L"/双击=" + std::to_wstring(dblOk)
            + L"/右键=" + std::to_wstring(rcOk) + L"/组合键=" + std::to_wstring(keyOk)
            + L"/输入=" + std::to_wstring(typeOk) + L"/滚动=" + std::to_wstring(scrollOk)
            + L"/长按=" + std::to_wstring(holdOk) + L"/光标=" + std::to_wstring(cursorOk)
            + L"/网页守卫=" + std::to_wstring(webGuard) + L"/表格守卫="
            + std::to_wstring(sheetGuard) + L"/错误=" + std::to_wstring(errOk)).c_str());
    ResetAiActionSessionState(989);
}

// ── runCommand：非法 JSON 也要能从正文里恢复命令（实测卡死点）──────────
void CaseRunCommandSalvage() {
    ResetAiActionSessionState(982);
    SetAiActionPlanGateEnabled(false);
    std::wstring lastActions;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [&lastActions](const std::wstring& json) {
        lastActions = json;
        return std::wstring(L"已执行");
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    // 模型给的多行命令没转义 → 整段不是合法 JSON：必须仍能执行，而不是「JSON 解析失败」
    const std::wstring broken =
        L"{\"command\": \"$p='C:\\\\t.csv'\n'序号' | Out-File $p\"}";
    const std::wstring out = CallNamedTool(tools, L"runCommand", broken);
    const bool salvaged = out.rfind(L"[错误]", 0) != 0
        && lastActions.find(L"runProgram") != std::wstring::npos
        && lastActions.find(L"Out-File") != std::wstring::npos;
    // 完全空的参数仍然报错（别把垃圾当命令执行）
    const std::wstring empty = CallNamedTool(tools, L"runCommand", L"{}");
    const bool emptyOk = empty.rfind(L"[错误]", 0) == 0;
    Emit(L"run_command_salvage", salvaged && emptyOk,
        (L"out=" + out.substr(0, 70) + L" | acted=" + lastActions.substr(0, 120)).c_str());
    ResetAiActionSessionState(982);
}

// ── 多候选聚类融合（ai_locate_verify） ──────────────────────────────
void CaseFuseLocateCandidates() {
    // ① 两个候选基本重合 → 聚成一簇取簇心，簇内平均（不是把矛盾候选平均掉）
    bool sameOk = false;
    {
        std::vector<AiVisionCandidate> c(2);
        c[0] = {100, 200, 140, 220, false};
        c[1] = {104, 202, 144, 222, false};
        const AiLocateFusionResult f = FuseLocateCandidates(c, {}, 0.5);
        sameOk = f.ok && f.clusterSize == 2 && f.cx == 122 && f.cy == 211
            && !f.uiaConfirmed && f.candidateCount == 2;
    }
    // ② 候选互相矛盾（间距 > 合并阈值）→ 不平均：取最大簇里的那一个，绝不落到两点中间
    bool splitOk = false;
    {
        std::vector<AiVisionCandidate> c(2);
        c[0] = {100, 200, 140, 220, false};
        c[1] = {900, 600, 940, 620, false};
        const AiLocateFusionResult f = FuseLocateCandidates(c, {}, 0.5);
        const int midX = (120 + 920) / 2;
        splitOk = f.ok && f.clusterSize == 1 && f.cx == 120 && f.cy == 210
            && f.cx != midX && f.note.find(L"分歧") != std::wstring::npos;
    }
    // ③ IoU 达标 → 直接用 UIA 控件的精确矩形（比 VLM 的框可信）
    bool iouOk = false;
    {
        std::vector<AiVisionCandidate> c(1);
        c[0] = {100, 200, 140, 240, false};
        std::vector<AiUiAnchor> a(1);
        a[0] = {98, 198, 142, 242, L"保存按钮"};
        const AiLocateFusionResult f = FuseLocateCandidates(c, a, 0.5);
        iouOk = f.ok && f.uiaConfirmed && f.uiaName == L"保存按钮"
            && f.boxX1 == 98 && f.boxY1 == 198 && f.boxX2 == 142 && f.boxY2 == 242
            && f.cx == 120 && f.cy == 220;
    }
    // ④ IoU 很低但候选中心落在锚点内 → 同样认定命中（UFO² 的做法）
    bool insideOk = false;
    {
        std::vector<AiVisionCandidate> c(1);
        c[0] = {90, 90, 100, 100, false};
        std::vector<AiUiAnchor> a(1);
        a[0] = {0, 0, 200, 200, L"编辑区"};
        const AiLocateFusionResult f = FuseLocateCandidates(c, a, 0.9);
        insideOk = f.ok && f.uiaConfirmed && f.uiaName == L"编辑区"
            && f.boxX1 == 0 && f.boxX2 == 200 && f.cx == 100 && f.cy == 100;
    }
    // ⑤ 退化点候选（只有中心）也能聚类；空候选 → ok=false
    bool pointOk = false;
    {
        std::vector<AiVisionCandidate> c(2);
        c[0] = {50, 60, 50, 60, true};
        c[1] = {52, 62, 52, 62, true};
        const AiLocateFusionResult f = FuseLocateCandidates(c, {}, 0.5);
        const AiLocateFusionResult none = FuseLocateCandidates({}, {}, 0.5);
        pointOk = f.ok && f.clusterSize == 2 && f.cx == 51 && f.cy == 61
            && !none.ok && !none.note.empty();
    }
    // ⑥ 单候选 + 名字对得上的 UIA 锚点 → 采信控件精确框（这一步能省掉一整轮 Zoom 识图）
    bool singleUiaOk = false;
    {
        std::vector<AiVisionCandidate> c(1);
        c[0] = {100, 200, 140, 240, false};
        std::vector<AiUiAnchor> a(1);
        a[0] = {96, 196, 144, 244, L"设置及其他(C)"};
        const AiLocateFusionResult f = FuseLocateCandidates(c, a, 0.5, L"右上角更多");
        singleUiaOk = f.ok && f.uiaConfirmed && f.boxX1 == 96 && f.boxX2 == 144;
    }
    // ⑦ 几何弱（只是中心落在大容器里）+ 名字对不上 → 绝不采信
    //   （真实场景：候选点落在「编辑区」这类大容器内，但容器名字与目标无关 → 借它的矩形会点歪）
    bool nameFilterOk = false;
    {
        std::vector<AiVisionCandidate> c(1);
        c[0] = {90, 90, 100, 100, false};
        std::vector<AiUiAnchor> a(1);
        a[0] = {0, 0, 200, 200, L"收藏夹"};
        const AiLocateFusionResult f = FuseLocateCandidates(c, a, 0.5, L"保存");
        nameFilterOk = f.ok && !f.uiaConfirmed && f.cx == 95 && f.cy == 95;
    }
    const bool nameMatchOk = UiNameMatchesTarget(L"保存(&S)", L"保存按钮")
        && UiNameMatchesTarget(L"History", L"history")
        && !UiNameMatchesTarget(L"设置及其他(C)", L"右上角更多")   // 名字确实对不上 → 只能靠几何
        && !UiNameMatchesTarget(L"收藏夹", L"保存")
        && !UiNameMatchesTarget(L"", L"保存")
        && !UiNameMatchesTarget(L"保存", L"");
    Emit(L"fuse_locate_candidates",
        sameOk && splitOk && iouOk && insideOk && pointOk && singleUiaOk && nameFilterOk
            && nameMatchOk,
        (L"簇心=" + std::to_wstring(sameOk) + L"/分歧=" + std::to_wstring(splitOk)
            + L"/IoU=" + std::to_wstring(iouOk) + L"/内含=" + std::to_wstring(insideOk)
            + L"/点=" + std::to_wstring(pointOk) + L"/单候选UIA=" + std::to_wstring(singleUiaOk)
            + L"/名字过滤=" + std::to_wstring(nameFilterOk)
            + L"/名字匹配=" + std::to_wstring(nameMatchOk)).c_str());
}

// ── 定位置信判决 ────────────────────────────────────────────────────
void CaseJudgeLocateConfidence() {
    AiLocateVerifyInput in;
    in.boxW = 80;
    in.boxH = 30;
    in.clusterAgreement = 1.0;
    std::wstring why;
    const AiLocateVerdict single = JudgeLocateConfidence(in, &why);
    const bool singleOk = single == AiLocateVerdict::Suspect && !why.empty();

    AiLocateVerifyInput uia = in;
    uia.uiaConfirmed = true;
    const AiLocateVerdict vUia = JudgeLocateConfidence(uia, nullptr);

    AiLocateVerifyInput low = in;
    low.lowFeature = true;
    const AiLocateVerdict vLow = JudgeLocateConfidence(low, nullptr);

    AiLocateVerifyInput split = in;
    split.clusterAgreement = 0.34;   // 3 个候选里只有 1 个进簇
    const AiLocateVerdict vSplit = JudgeLocateConfidence(split, nullptr);

    AiLocateVerifyInput ctrl = in;
    ctrl.pointOnInteractiveControl = true;
    const AiLocateVerdict vCtrl = JudgeLocateConfidence(ctrl, nullptr);

    AiLocateVerifyInput tiny = in;
    tiny.boxW = 6;
    tiny.boxH = 6;
    const AiLocateVerdict vTiny = JudgeLocateConfidence(tiny, nullptr);

    AiLocateVerifyInput none = in;
    none.boxW = 0;
    none.boxH = 0;
    none.clusterAgreement = 0.0;
    const AiLocateVerdict vNone = JudgeLocateConfidence(none, nullptr);

    // 低特征 + UIA 确认：UIA 优先（控件是真的，特征低可能是纯色按钮）
    AiLocateVerifyInput both = low;
    both.uiaConfirmed = true;
    const AiLocateVerdict vBoth = JudgeLocateConfidence(both, nullptr);

    const bool ok = singleOk
        && vUia == AiLocateVerdict::Accept && vCtrl == AiLocateVerdict::Accept
        && vLow == AiLocateVerdict::Refine && vSplit == AiLocateVerdict::Refine
        && vTiny == AiLocateVerdict::Suspect && vNone == AiLocateVerdict::Suspect
        && vBoth == AiLocateVerdict::Accept
        && wcscmp(AiLocateVerdictName(AiLocateVerdict::Accept), L"可用") == 0
        && wcscmp(AiLocateVerdictName(AiLocateVerdict::Refine), L"需精炼") == 0
        && wcscmp(AiLocateVerdictName(AiLocateVerdict::Suspect), L"可疑") == 0;
    Emit(L"judge_locate_confidence", ok,
        (L"单候选=" + std::wstring(AiLocateVerdictName(single)) + L"/UIA="
            + AiLocateVerdictName(vUia) + L"/低特征=" + AiLocateVerdictName(vLow)
            + L"/分歧=" + AiLocateVerdictName(vSplit) + L"/控点="
            + AiLocateVerdictName(vCtrl) + L"/过小=" + AiLocateVerdictName(vTiny)
            + L"/无证据=" + AiLocateVerdictName(vNone)).c_str());
}

// ── 多候选行拆分 ────────────────────────────────────────────────────
void CaseSplitVisionCandidateLines() {
    const std::vector<std::wstring> l3 = SplitVisionCandidateLines(
        L"  最可能：- 100,200  \n2. 300,400\n* 500,600\n700,800", 3);
    const bool capOk = l3.size() == 3 && l3[0] == L"最可能：- 100,200"
        && l3[1] == L"300,400" && l3[2] == L"500,600";
    const std::vector<std::wstring> l4 = SplitVisionCandidateLines(
        L"1\n2、800,900\n   \n", 3);
    const bool cjkOk = l4.size() == 2 && l4[0] == L"1" && l4[1] == L"800,900";
    const bool emptyOk = SplitVisionCandidateLines(L"   \n  ", 3).empty()
        && SplitVisionCandidateLines(L"100,200", 0).empty();
    Emit(L"split_vision_candidate_lines", capOk && cjkOk && emptyOk,
        (L"3行=" + std::to_wstring(capOk) + L"/顿号=" + std::to_wstring(cjkOk)
            + L"/空=" + std::to_wstring(emptyOk)).c_str());
}

HBITMAP MakeTestDib(int w, int h, int kind) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!bmp || !bits) return nullptr;
    auto* px = static_cast<uint8_t*>(bits);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            uint8_t v = 255;
            if (kind == 1) {           // 纯色（空白区）
                v = 255;
            } else if (kind == 2) {    // 2px 棋盘格（有纹理）
                v = (((x / 2) + (y / 2)) % 2 == 0) ? 0 : 255;
            } else {                   // 中灰纯色
                v = 128;
            }
            px[i] = v;
            px[i + 1] = v;
            px[i + 2] = v;
            px[i + 3] = 255;
        }
    }
    return bmp;
}

// ── 框内特征密度（低特征=纯色/空白区） ─────────────────────────────
void CaseBitmapLowFeature() {
    double sdBlank = -1.0;
    double sdTexture = -1.0;
    double sdGray = -1.0;
    HBITMAP blank = MakeTestDib(64, 64, 1);
    HBITMAP texture = MakeTestDib(64, 64, 2);
    HBITMAP gray = MakeTestDib(64, 64, 3);
    const bool blankLow = BitmapRegionLooksLowFeature(blank, &sdBlank);
    const bool textureLow = BitmapRegionLooksLowFeature(texture, &sdTexture);
    const bool grayLow = BitmapRegionLooksLowFeature(gray, &sdGray);
    const bool nullLow = BitmapRegionLooksLowFeature(nullptr, nullptr);
    if (blank) DeleteObject(blank);
    if (texture) DeleteObject(texture);
    if (gray) DeleteObject(gray);
    const bool ok = blankLow && grayLow && !textureLow && nullLow
        && sdBlank >= 0.0 && sdBlank < 1.0 && sdTexture > 20.0 && sdGray < 1.0;
    Emit(L"bitmap_low_feature", ok,
        (L"空白sd=" + std::to_wstring(static_cast<int>(sdBlank)) + L"/纹理sd="
            + std::to_wstring(static_cast<int>(sdTexture)) + L"/中灰sd="
            + std::to_wstring(static_cast<int>(sdGray))).c_str());
}

// ── OCR 文本核对（可选能力）────────────────────────────────────────
void CaseOcrTextVerify() {
    // ① 该不该核对：纯短文本标签可以，颜色/方位/图标/长句/坐标不行
    std::wstring want;
    const bool extractOk =
        AiLocateExtractOcrTarget(L"保存按钮", &want) && want == L"保存"
        && AiLocateExtractOcrTarget(L"点击登录", &want) && want == L"登录"
        && AiLocateExtractOcrTarget(L"确定", &want) && want == L"确定"
        && AiLocateExtractOcrTarget(L"下一页", &want) && want == L"下一页"
        && !AiLocateExtractOcrTarget(L"右上角的红色关闭按钮", &want)
        && !AiLocateExtractOcrTarget(L"第一个图标", &want)
        && !AiLocateExtractOcrTarget(L"列表里第3个卡片", &want)
        && !AiLocateExtractOcrTarget(L"123", &want)
        && !AiLocateExtractOcrTarget(L"100,200 附近的输入框", &want)
        && !AiLocateExtractOcrTarget(L"", &want)
        && !AiLocateExtractOcrTarget(L"这个很长很长的目标描述文本内容区", &want);
    // ② 没装引擎 → 静默跳过：checked=false 且不改点
    const AiLocateOcrOutcome skip = JudgeLocateOcrText(false, false, 0, 0, 0, 0, 400, 300);
    const bool skipOk = !skip.checked && !skip.found && !skip.moved && skip.note.empty();
    // ③ 引擎可用但没读到目标 → checked && !found（提示疑似未命中，但不改点）
    const AiLocateOcrOutcome miss = JudgeLocateOcrText(true, false, 0, 0, 0, 0, 400, 300);
    const bool missOk = miss.checked && !miss.found && !miss.moved && !miss.note.empty();
    // ④ 读到目标且位置一致 → checked && found && !moved
    const AiLocateOcrOutcome hit = JudgeLocateOcrText(true, true, 390, 290, 410, 310, 400, 300);
    const bool hitOk = hit.checked && hit.found && !hit.moved && hit.note.find(L"通过") != std::wstring::npos;
    // ⑤ 读到目标但偏得多 → 按识别框中心修正（并夹在框内）
    const AiLocateOcrOutcome fix = JudgeLocateOcrText(true, true, 468, 336, 508, 356, 400, 300);
    const bool fixOk = fix.checked && fix.found && fix.moved && fix.dx == 88 && fix.dy == 46
        && fix.cx >= 470 && fix.cx <= 506 && fix.cy >= 337 && fix.cy <= 355;
    Emit(L"ocr_text_verify", extractOk && skipOk && missOk && hitOk && fixOk,
        (L"提取=" + std::to_wstring(extractOk) + L"/无引擎=" + std::to_wstring(skipOk)
            + L"/未读到=" + std::to_wstring(missOk) + L"/一致=" + std::to_wstring(hitOk)
            + L"/修正=" + std::to_wstring(fixOk)).c_str());
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
    CaseOpenWebpageApiAndSameSiteGuard();
    CaseTaskDataClearedOnReset();
    CasePageKindClassify();
    CasePageSnapshotFormatAndRef();
    CaseScrollWheelConfirmWhenViewportHasContent();
    CaseListFirstBlocksScrollAndOtherCards();
    CaseWebBrowseAllowsVisionFallback();
    CaseWebMixedPrefersTreeClick();
    CaseObservePageToolsPresent();
    CaseSearchOnPageTool();
    CaseClickRefNavigatesSpaceHref();
    CaseSamePageClickHintScoped();
    CaseSearchOnPageOpensMatchingSpace();
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
    CaseGameForegroundVisionAllowed();
    CaseLocateMultiTargets();
    CaseUiLayoutMemoryAndGrid();
    CaseUiLayoutSignatureGate();
    CaseNoBatchSelectSpecialCase();
    CaseUiGridPeriodDetect();
    CaseOcrDirectClickPick();
    CaseLocateDecimalCoordParse();
    CaseMissSelfCorrectPrompt();
    CasePlanSpendBudget();
    CaseLogicConvertCompileGate();
    CaseLogicConvertPerLocateTimers();
    CaseLogicConvertAssistantGate();
    CaseLogicConvertWritebackGuards();
    CaseLogicConvertSessionRecording();
    CaseLogicConvertBridgeNav();
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
    CaseActionModelNotSilentlySwapped();
    CasePlannerObserveImageAttach();
    CasePickSnapshotRefForText();
    CasePickSnapshotRefAmbiguous();
    CaseSnapshotTargetKeyword();
    CaseVisionPromptNormalizedContract();
    CaseLocateToolDefersToHostDom();
    CaseLookaheadWaitBudget();
    CaseDomFirstActionGate();
    CasePickSnapshotRefForInput();
    CaseTypeByLabelTool();
    CaseUiControlTools();
    CaseLocateCache();
    CaseWideRowStillNeedsRefine();
    CaseUrlInputHeuristic();
    CaseEnterAfterUrlInput();
    CaseBrowserTitleHintStrip();
    CaseRunCommandTool();
    CaseExtractActionJsonArray();
    CaseOpenWebpageInternalPage();
    CaseRecipeReuseGuards();
    CaseRunCommandSalvage();
    CaseDomFirstDialogGate();
    CaseRouteNudge();
    CaseHotkeyLabelRealKeys();
    CaseReadDocumentTool();
    CaseComputerTool();
    CaseLocateCacheHysteresis();
    CaseFuseLocateCandidates();
    CaseJudgeLocateConfidence();
    CaseSplitVisionCandidateLines();
    CaseBitmapLowFeature();
    CaseOcrTextVerify();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
