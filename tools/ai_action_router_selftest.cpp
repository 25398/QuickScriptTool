// =============================================================================
// AiActionRouterSelfTest — AI 动作路由分类自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:AiActionRouterSelfTest
//   build\Release\AiActionRouterSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "process_utils.h"

#include "action_utils.h"
#include "agent_ai_actions.h"
#include "agent_attachment.h"
#include "ai_action_router.h"
#include "ai_action_service.h"
#include "ai_decide.h"
#include "image_match.h"
#include "ai_plan_util.h"
#include "ai_locate_verify.h"
#include "office_doc.h"
#include "ai_logic_convert.h"
#include "app_settings_store.h"
#include "color_match.h"
#include "desktop_tools/desktop_tools.h"   // 落盘日志端到端（docs §61）
#include "macro_execute_tools.h"
#include "window_mode/ui_element_probe.h"
#include "page_snapshot.h"
#include "script_action_builder.h"
#include "script_io.h"
#include "script_types.h"

#include <algorithm>
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
    {L"complete_task_wrapup_visible", L"default",
        L"completeTask 收尾判据必须落在网页路径可见区（目录只取 48 字节≈16 汉字）"},
    {L"task_memo_and_open_dedup", L"default",
        L"task memo + skip duplicate openWebpage"},
    {L"open_webpage_no_site_guard", L"default",
        L"openWebpage 不再按站点/URL 形状拒绝（照开 + 如实标注）"},
    {L"task_data_cleared_on_reset", L"default",
        L"ResetAiActionSessionState 清空 saveTaskData 缓存，防串上轮 historyRecords"},
    {L"page_kind_classify", L"default",
        L"canvasRatio+interactive → dom/mixed/canvas"},
    {L"page_snapshot_format_and_ref", L"default",
        L"Parse/Format 压缩树；canvas 禁 HTML；clickRef 门闩；可视/屏外覆盖"},
    {L"snapshot_reports_coverage_and_paging", L"default",
        L"长列表页覆盖度：全量明说「已全部列出」；被切了报「共N/第a~b条 + nextOffset」；"
        L"旧扩展不谎报完整"},
    {L"scroll_wheel_not_blocked_when_viewport_has_content", L"default",
        L"可视区有内容也不再拦滚动（批 D：跨帧状态驱动拒绝已删）"},
    {L"list_first_no_longer_blocks", L"default",
        L"「列表第1项」概念与两条拒绝已删：滚动/点其它内容卡一律照发"},
    {L"web_browse_allows_vision_fallback", L"default",
        L"网页扩展是优化：树上没有或未装扩展时 locateAndClick 仍可用"},
    {L"web_mixed_prefers_tree_click", L"default",
        L"树上有按钮时 locateAndClick 指路 clickRef（无宿主）；"
        L"completeTask 不再被「上轮识图未找到」否决（模型要收工就如实收工）"},
    {L"logic_convert_bridge_nav", L"default",
        L"扩展桥导航固化为打开网页，clickRef 可记找图模板"},
    {L"observe_page_tools_present", L"default",
        L"observePage/clickRef 工具注册；canvas 跳过抓树"},
    {L"search_on_page_tool", L"default",
        L"searchOnPage 构造站点搜索 URL 并走 navigatePage"},
    {L"click_ref_navigates_space_href", L"default",
        L"clickRef 一律如实点击（不再代导航/不再按站点启发式拒绝）；滚轮无图照执行+标注"},
    {L"same_page_click_not_blocked", L"default",
        L"同页 clickRef 不再被拒（连续未跳转只如实报计数）"},
    {L"search_on_page_opens_matching_space", L"default",
        L"searchOnPage 名字命中用户卡片时直接打开主页"},
    {L"observe_result_defaults", L"default",
        L"AiObserveCaptureResult / aiObs 常量"},
    {L"atomic_quick_input_rejects_empty", L"default",
        L"规范工具 quickInput 拒空 inputText"},
    {L"atomic_key_click_enter", L"default",
        L"规范工具 keyClick(Enter) 可执行"},
    {L"non_universal_shortcut_guard", L"default",
        L"应用专属组合键(Ctrl+H)默认劝退；通用键放行；网页按 Ctrl+T/N 只如实标注；Skill 不再教 F12/Ctrl+H"},
    {L"allow_nested_ai_action_under_cap", L"default",
        L"未达上限时允许 submit 含 aiActionExecute"},
    {L"reject_nested_ai_action_at_cap", L"default",
        L"嵌套达上限时 submit aiActionExecute 被拒"},
    {L"absolute_pointer_no_image_annotated", L"default",
        L"无图模式绝对坐标点击照执行 + 如实标注（批 D：不再拒绝）"},
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
    {L"color_match_score_percent", L"default",
        L"颜色匹配度统一 0~100（色差 0→100、≥100→0，不出负数）"},
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
        L"图内亦像 0~1000 的框优先归一化（防搜索钮偏移），并**如实留痕**两种读法"},
    {L"resolve_vision_ambiguity_not_claimed_when_impossible", L"default",
        L"两套读法不可能同时成立时不许说「无法区分」（防留痕退化成恒真噪声）"},
    {L"macro_debug_log_file_roundtrip", L"default",
        L"AI 调试日志真落盘并读回（docs §61：没生成是**静默**失败，只能真写一次才测得到）"},
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
    {L"locate_retry_hard_cap", L"default",
        L"locate 失败计数契约：失败累加、成功清零（只供如实回执；**不再**用于拦截重试）"},
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
    {L"dom_first_action_gate", L"default",
        L"DOM 优先门禁：扩展/钩子/canvas/右键/前台非浏览器一律不放行"},
    {L"pick_snapshot_ref_for_input", L"default",
        L"填写口径选输入框（textbox/searchbox），与点击口径不同"},
    {L"type_by_label_tool", L"default",
        L"typeByLabel 一次调用按标签填写；无命中/无钩子/空参报错并指路"},
    {L"ui_control_tools", L"default",
        L"listUiControls/invokeUiControl：台账透传、执行标记、报错透传、只读不被计划门闩拦"},
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
    {L"plan_spend_gate", L"default", L"批量选择**完全无特判**：宿主只做能力不做领域规则；「一键全选」照常定位点击（关键词门槛与提醒均已删除，用户要求不做针对性优化）"},
    {L"ocr_direct_click_pick", L"default",
        L"文字直点：OCR 索引里唯一命中文字标签 → 直接给坐标（省 DOM/UIA/两轮识图）；"
        L"同屏多个同名按钮/整块并成一行/纯数字目标一律拒绝并回落识图"},
    {L"locate_decimal_coord_parse", L"default",
        L"定位解析必须吃小数：旧实现把 (88.5,117.3) 判成「无法解析」、把 [52.4,…] 静默解析成错框；"
        L"现在点对/框选都按四舍五入取值，反序框仍归一化"},
    {L"plan_spend_budget", L"default",
        L"先算账：卡槽有限时按性价比挑强卡（不一键全选）；预算内算出一次能放几个单位；买不起要明说；maxCount 限量生效"},
    {L"decide_table", L"default",
        L"本地判断表（System One 形态）：视觉闸/游戏前台/附帧三张表逐行断言+优先级；"
        L"置信度三档边界；判断表必须穷尽（不许静默放行）"},
    {L"decide_wrapper_equivalence", L"default",
        L"旧布尔封装与判断表判决等价：视觉闸/游戏前台在真实会话状态下逐组对齐（重构不许改行为）"},
    {L"decide_diagnostic_log", L"default",
        L"判断诊断：每次判断留一行 verdict+conf+why；sink 收得到；按动作清空不串台"},
    {L"vision_gate_trace_only", L"default",
        L"视觉闸只留判断 + 留痕：动作路径上既不拦识图定位、也不注入任何提示"
        L"（视觉唯一手段时更没有「改用控件树」的错指引）"},
    {L"ai_exec_observe_guards", L"default",
        L"执行后要不要回传观察帧：模型明确要观察 / 结果不确定 / 本任务第一次动手 —— 三条守卫；"
        L"第一次动手强制观察（防「抢跑」），但模型明确不要观察的后续步骤仍可不看"},
    {L"ocr_text_index_rows", L"default",
        L"屏幕文字索引按画面行分组（可点清单）：同视觉行才并组、跨行不并；"
        L"标视觉行序与坐标口径；纯数字限量、低置信/超短挡掉；空输入给空串"},
    {L"thinking_downgrade_plumbing", L"default",
        L"思考降档前置机制：AI 动作作用域标记按进出（含嵌套）正确开关；"
        L"「抑制 N 轮」计数会被消耗（防永久闩锁——该缺陷真实出现过）"},
    {L"frame_click_mark_lifecycle", L"default",
        L"观察帧落点标注（红叉=上一次点击落在哪，让规划模型下次识图顺手验收）："
        L"屏幕→帧内换算、一次点击只标一帧、同一点再点一次可重标、"
        L"出界/无区域/出帧一律不标；标签口径只有一处（图上与提示词同一句话）"},
    {L"zoom_region_tool", L"default",
        L"`zoom`：把一块区域按**原始分辨率**放大回传（看不清小字/图标时的通用解药）——"
        L"入参＝upload 截图像素（与元素索引同一套、零换算），也可给 target=文字标签由引擎解析；"
        L"区域夹取/太小/帧外一律**如实拒绝**；回执必须写清**区域、倍率、图内坐标换回 upload 的公式**，"
        L"且倍率按「送图链路缩完之后、模型真正看到的那张图」算，maxEdge 上限不得超过该链路硬上限"},
    {L"zoom_temp_image_files", L"default",
        L"zoom 落地文件：同一进程内两次 zoom 的文件名必须**不同**（同轮两张图不能互相覆盖 —— "
        L"实测模型因此判定「zoom 只会返回最后一张」并弃用了它）；旧图按年龄清理，"
        L"新图与非 zoom_ 文件一律不动，未来时间戳宁可不删"},
    {L"element_index_and_resolve", L"default",
        L"统一可点元素索引（所见即所得）：UIA 控件 ∪ OCR 文字合成一张编号表；"
        L"同处同名才合并（保守，宁可两条）、编号按阅读顺序、纯数字/过短目标不解析、"
        L"同档多条无位置线索即判歧义；★窗口自身标题栏按钮永不入选（「关闭」一击关掉整个窗口那件事）"},
    {L"element_index_facts_and_id_binding", L"default",
        L"★半视觉两条：① 索引条目带**角色/动作能力/可读状态**（滑块 action:slide、"
        L"输入框 action:fill、[value:…][range:…][toggle:…][focused]）并渲染进给模型的清单，"
        L"OCR 条目**不许**伪造能力（本地没有证据就不猜）；② 编号**能真的用** —— "
        L"locateAndClick(elementId=N) 直查同一张表、同屏同名靠编号区分、"
        L"查不到绝不兜底成近似条目、窗口自身按钮与灰控件永不入选；"
        L"清单标题里必须写明 elementId 可用（否则模型不知道有这个能力）"},
    {L"caption_icon_pairing", L"default",
        L"★「短标签 → 它标注的图标槽」（通用版面推断）：卡槽里 OCR 只读得到价签、读不到卡片 ⇒"
        L"模型知道「600」这几个字在哪、不知道那张卡在哪（实测 8 轮 zoom 猜卡，最后只会点"
        L"唯一有把握的那张 —— 用户看到的「只会放普通僵尸」）。判据：≥3 段短标签成排 + 间距均匀 +"
        L"正上方能切出**同样数量**的等高块（文字行因太矮被挡）+ 横向对齐 + 宽度相近；"
        L"任何一条不满足就**什么都不发布**。配对后索引里不得再留同名文字条目（否则判歧义）"},
    {L"coord_action_receipt_names_landing", L"default",        L"★坐标动作的回执必须**回显落点**（docs §54）：`SummarizeExecutedActionsJson` 原先"
        L"只对 quickInput/keyClick/wait 之类给细节，坐标动作一律只剩 `已执行:mouseClick` ——"
        L"模型发来 (243,50)、拿回的字符串里**连个数字都没有**，于是无法判断卡片选没选中，"
        L"只能反复点、反复开合同一个面板（用户主诉：「光选卡，选完卡咋不会放僵尸」）。"
        L"本用例钉：mouseClick/moveMouse 回执里有 (x,y)（upload 口径，与原值逐字相同）、"
        L"mouseDrag 有 (起点)→(终点)、且两套终点字段（toX/toY 与宿主合并用的 endX/endY）都认"},
    {L"mouse_click_repeat_same_point", L"default",
        L"★同一坐标**连点 12 次**，每一次都必须真的发出去（docs §72）。"
        L"起因：真机日志里 20 击的一批，**后 10 击全部 0 步**，回执只有一句"
        L"「[错误] 本批 0 步：没有可执行的动作（空数组或全部被跳过）」——"
        L"模型读不出原因、只能照原样再点，排查也只能靠读源码猜是哪条 continue。"
        L"本用例先钉死工具层：不许有任何「同点静默去重 / 静默丢弃」，"
        L"12 次调用必须产生 12 个**含 mouseClick** 的动作数组"},
    {L"mouse_drag_executes", L"default",
        L"★`mouseDrag` 拖拽必须真的能跑：它拼的 mouseDown/mouseUp 不带 x/y 会被校验拒，"
        L"而且旧报错讲的是「点击输入框须给截图坐标…」（完全不搭界），模型据此跑偏。"
        L"本用例钉：带坐标能过 + 执行 JSON 里有起点终点 + 缺坐标的报错贴着该动作说 +"
        L"★「计划执行」那行预览**不许说谎**：工具参数 `fromX/fromY/toX/toY` 必须对齐成"
        L"动作字段 `x/y/endX/endY`（旧实现硬套出 `(0,0)→(0,0)`，与同一批真实执行的"
        L"(228,47)→(760,300) 相反），对齐不了就回退印原始参数、绝不编造零点"},
    {L"resolve_app_launch_target", L"default",
        L"按显示名启动的**本地解析**（桌面/开始菜单快捷方式 → App Paths → PATH）："
        L"解析不到必须明确失败，绝不退化成 Win+S 搜索（那条路会在浏览器里搜网页）；"
        L".lnk 必须被判成 shell 目标"},
    {L"non_client_landing_strip", L"default",
        L"落点在窗口**非客户区**（标题栏/边框）必须拦：§31.1 那条只加了主识图链路、"
        L"而且**依赖 UIA**，而游戏窗口实测 `UIA 控件 0 条` ⇒ 它根本不触发。"
        L"改用**纯 Win32 客户区几何**（任何窗口都量得到），并用实测矩形断言"
        L"「(2522,16) 必拦 / (2475,257) 放行 / 无边框全屏不拦（那是减少能力）」"},
    {L"vision_answer_gate", L"default",
        L"识图回答有效性：模型答不出来时会回**空框** `[0,0,0,0]`，旧解析里它退化成了点 (0,0) ——"
        L"于是点了屏幕左上角、回执写「界面已经变化 → 已生效」、还把垃圾存成定位模板。"
        L"空框/零宽零高一律判「没回答」；单点回答与原点单点不受影响（Zoom 级本来问的就是点）"},
    {L"slow_thinking_round_gate", L"default",
        L"思考失控闸的**状态必须跨轮存活**：它原来是 SendMessage 的局部变量，而 AI 动作执行"
        L"每外层轮重进一次 SendMessage ⇒ 计数器每轮归零 ⇒「连续 2 轮慢」永远不成立 ⇒"
        L" 闸门形同不存在（实测 16.2s → 62.3s 一次没触发 = 用户说的「半天没反应」）。"
        L"现在跨轮状态是显式入参/出参；并新增「单轮 ≥30s 当场处置」——一轮就能 60s，"
        L"等第二轮的 120s 等不起；条件不满足时既不计数也不清零（防「永久关思考」正反馈）"},
    {L"stream_prose_brake", L"default",
        L"工具轮的收束判据的宾语是「**有没有工具调用意向**」，与文本落在 reasoning_content 还是"
        L" content **无关**：旧写法绑死 `contentBytes == 0`，于是「长篇大论却不调工具」这个形态"
        L"**结构上就看不见**（实测两轮各 45~55s、57 895 / 54 732 字节全在正文里）⇒ 只能烧到"
        L" max_tokens 耗尽。新判据只在 AI 动作作用域内按产出预算收束（聊天助手的长回答不能砍），"
        L"且**原闸①两条门槛逐字保留**；用例里同时复刻旧判据，断言它在那一格是漏的"},
    {L"no_tool_call_answer_gate", L"default",
        L"「期待工具却只拿到散文」——这段文本不是回答：旧实现把 `assistantMsg.content` 当本轮"
        L"最终回答返回（最坏附一句「请在设置里增大 max_tokens」）⇒ AI 动作执行这一轮等于**什么都"
        L"没干**，用户看到的就是「卡在那里不动」。现在被截断/被收束且无工具调用 → 注入「只准调"
        L"工具」纠偏（`finish_reason=stop` 的自然收尾仍然尊重），且纠偏**有界**（§36.6/§40.2："
        L"守卫不能把模型永久锁在外面）"},
    {L"plan_spend_target_contract", L"default",
        L"★「清单要能被执行」是产清单的工具的责任（docs §44）：planSpend 的 `costs` 简写**伪造**了"
        L"「选项3(花费500)」这种像名字的东西，而描述又写着「照它执行、逐个点名单里的卡」⇒ 模型真的"
        L"去 `locateAndClick(\"9999\"/\"600\")`。而价格三条路全断（不唯一 500×3；不在 OCR 索引里；"
        L"拿裸数字问 VLM 每次框都不同 —— 同一个「600」三次给出相差 500px 的三个点）⇒ 点空 ⇒ "
        L"死点表/近点重复连环触发 ⇒ 改用**不带目标语义**的手算坐标 mouseClick ⇒ 在选卡界面上把"
        L"**已选中的卡又点掉**（用户：「把选了的卡收回去又重新选」）。现在占位符必写明「无名字」、"
        L"回执当场说清「不能用来定位」，并给出可执行替代（items 带 name = 卡面上真实存在的短标签）"},
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
    // ★批 D（docs §47）：原先这里断言 hotkeyShortcut(9) 被**拒绝**并指路 activateWindow。
    //   那条禁令已删 ⇒ 改成：照执行 + 如实标注「Alt+Tab 切到最近用过的窗口」。
    const std::wstring altTab = CallNamedTool(tools, L"hotkeyShortcut",
        L"{\"shortcutPreset\":9}");
    const bool altTabOk = altTab.rfind(L"[错误]", 0) != 0
        && altTab.find(L"[EXECUTED]") != std::wstring::npos
        && altTab.find(L"Alt+Tab") != std::wstring::npos;
    const bool ok = hasQi && hasKc && hasMc && hasFi && hasSubmit
        && hasScroll && hasDrag && hasMemo && hasReadMemo && hasRun && hasSearch && hasHotkey && hasSwitch
        && altTabOk
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

// ★★ 关键指令必须落在「网页 AI 路径真的会发给模型」的那段文字里（2026-10-02 A2 实测）
//
//   网页 AI 路径下模型看到的描述是**被截断的**：
//     目录 `RenderToolCatalog` 只取 `catalogDescChars = 48` **字节**（≈16 个汉字）；
//     扁平清单 `RenderToolList` 只取 160 字节。
//   ⚠ 前科：把「收尾判据」加在 `completeTask` 描述**末尾** ⇒ 加了等于没加 ——
//     源码里搜得到、自检也绿，但模型**一个字都没看到**（用户报"改完没变化"）。
//   ⇒ 判据：**收尾指令必须出现在前 16 个字符内**（16 汉字 = 48 字节，是最坏情况；
//     只要在前 16 字内，就一定在 48 字节预算内，与具体字节数无关）。
void CaseCompleteTaskWrapUpVisible() {
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring* desc = nullptr;
    for (const auto& t : tools) {
        if (t.name == L"completeTask") { desc = &t.description; break; }
    }
    if (!desc) {
        Emit(L"complete_task_wrapup_visible", false, L"completeTask 工具不存在");
        return;
    }
    // ① 目录预算（48 字节 ≈ 16 汉字）里必须能看到「收尾」这个动作词
    const std::wstring head = desc->substr(0, 16);
    const bool inCatalog = head.find(L"收尾") != std::wstring::npos;
    // ② 完整定义里必须有可执行的那句（loadTools 取完整定义时看得到）
    const bool actionable = desc->find(L"立刻调用本工具") != std::wstring::npos
        && desc->find(L"不要") != std::wstring::npos;
    // ③ 别把「不需要收尾」这类反向指令排在最前（防有人挪错了词序还过测）
    const bool positiveFirst = head.find(L"收尾") != std::wstring::npos
        && head.find(L"禁止") == std::wstring::npos;
    const bool ok = inCatalog && actionable && positiveFirst;
    std::wstring detail;
    if (!ok) {
        detail = L"收尾判据不在网页路径可见区";
        if (!inCatalog) detail += L" [不在前16字]";
        if (!actionable) detail += L" [缺可执行句]";
        if (!positiveFirst) detail += L" [首个动作词不是收尾]";
        detail += L" 前16字=「" + head + L"」";
    }
    Emit(L"complete_task_wrapup_visible", ok, detail.c_str());
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

void CaseSwitchWindowAfterLaunchNotBlocked() {
    // ★批 D（docs §47）：原用例钉的是「刚 runProgram 启动 ⇒ 不许开 Alt+Tab 预览，先劝一次」
    //   那条**基于启动时间戳的拒绝**。判据与字段（启动时刻 / 已劝过）已整体删除 ⇒
    //   用例反转：**启动之后照样可以开预览**（第一次、第二次都不再被拒）。
    // 前置条件：runProgram 会先解析目标程序路径。CI runner 没装 Office，
    // 解析 excel 必然失败 → 这条必红（2026-09-19 首次 CI 暴露）。
    // 属环境依赖，跳过而不是判失败；断言本身不放宽。
    if (ResolveProgramLaunchPath(L"excel").empty()) {
        Emit(L"switch_window_after_launch_not_blocked", true,
            L"skipped: 本机解析不到 excel（未装 Office）");
        return;
    }
    ResetAiActionSessionState(626262);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    // 无 hooks 时 runProgram 仍返回 [EXECUTED]
    const std::wstring run = CallNamedTool(tools, L"runProgram",
        L"{\"targetPath\":\"excel\"}");
    const std::wstring first = CallNamedTool(tools, L"switchWindow",
        L"{\"action\":\"openPreview\"}");
    const bool notWarned = first.find(L"\u81ea\u52a8\u5230\u524d\u53f0") == std::wstring::npos;
    const std::wstring second = CallNamedTool(tools, L"switchWindow",
        L"{\"action\":\"openPreview\"}");
    const bool stillNotWarned = second.find(L"\u81ea\u52a8\u5230\u524d\u53f0") == std::wstring::npos;
    const bool ok = run.find(L"[EXECUTED]") != std::wstring::npos && notWarned && stillNotWarned;
    Emit(L"switch_window_after_launch_not_blocked", ok,
        ok ? L"" : (first + L" | " + second).c_str());
}

void CaseWindowLedgerTools() {
    ResetAiActionSessionState(828282);
    std::wstring activateQuery;
    unsigned long activatePid = 0;
    AiActionHostHooks hooks;
    hooks.onListWindows = []() -> std::wstring {
        return L"#1 工作簿1 - Excel [EXCEL.EXE]（前台）\n#2 历史记录 - Edge [msedge.exe]";
    };
    hooks.onActivateWindow = [&](const std::wstring& query, unsigned long pid) -> std::wstring {
        activateQuery = query;
        activatePid = pid;
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

    // ★★ pid 消歧（2026-10-02 修）：台账 `FormatWindowList` 打印 `pid=…`，双开同名窗口时
    //   还叫模型「按 pid/客户区区分，别只按标题选」—— 工具层**必须真的收 pid**。
    //   前科：模型 `activateWindow(pid=29404)` 被打回「缺少 match」⇒ 在
    //   「match 命中 2 个 / pid 不被接受」之间反复绕圈、白烧好几轮。
    activateQuery.clear();
    activatePid = 0;
    const std::wstring byPid = CallNamedTool(tools, L"activateWindow", L"{\"pid\":29404}");
    const bool pidOk = activatePid == 29404u && activateQuery.empty()
        && byPid.find(L"[EXECUTED]") != std::wstring::npos
        && byPid.find(L"[错误]") == std::wstring::npos;
    // 模型偶尔把 pid 写成字符串（"29404"）—— 也要收
    activateQuery.clear();
    activatePid = 0;
    const std::wstring byPidStr = CallNamedTool(tools, L"activateWindow", L"{\"pid\":\"29404\"}");
    const bool pidStrOk = activatePid == 29404u
        && byPidStr.find(L"[错误]") == std::wstring::npos;
    // match + pid 同给：两个都要原样传给宿主（交集语义由宿主实现）
    activateQuery.clear();
    activatePid = 0;
    const std::wstring bothSel = CallNamedTool(tools, L"activateWindow",
        L"{\"match\":\"excel\",\"pid\":29404}");
    const bool bothOk = activatePid == 29404u && activateQuery == L"excel"
        && bothSel.find(L"[错误]") == std::wstring::npos;
    // 负对照：pid=0 视为「没给」⇒ 仍必须报非法调用（不能把 0 当合法进程号）
    const bool pidZeroRejected = CallNamedTool(tools, L"activateWindow",
        L"{\"pid\":0}").find(L"[错误]") != std::wstring::npos;

    // 宿主多候选拒绝应原样回传 [错误]
    hooks.onActivateWindow = [](const std::wstring& q, unsigned long pid) -> std::wstring {
        return L"[错误] activateWindow「" + q + L"」(pid=" + std::to_wstring(pid)
            + L") 匹配到 2 个窗口，拒绝自动选择以免切错。";
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

    const bool ok = listOk && actOk && actObsOk && emptyRejected && pidOk && pidStrOk
        && bothOk && pidZeroRejected && ambigOk && previewBlocked;
    // ⚠ detail 必须自己拼成 wstring 再 c_str()：三元里混 L"" 与 std::wstring 会推导成
    //   std::wstring，而 Emit 收 const wchar_t*（踩过 C2664）
    std::wstring ledgerDetail;
    if (!ok) {
        ledgerDetail = L"listWindows/activateWindow wiring failed";
        if (!pidOk) ledgerDetail += L" pid-not-accepted";
        if (!pidStrOk) ledgerDetail += L" pid-string-not-accepted";
        if (!bothOk) ledgerDetail += L" match+pid-not-forwarded";
        if (!pidZeroRejected) ledgerDetail += L" pid0-not-rejected";
    }
    Emit(L"window_ledger_tools", ok, ledgerDetail.c_str());
}

void CaseLocateFailSuggestsActivate() {
    ResetAiActionSessionState(838383);
    AiActionHostHooks hooks;
    hooks.onListWindows = []() -> std::wstring {
        return L"#1 历史记录 - Edge [msedge.exe]";
    };
    hooks.onActivateWindow = [](const std::wstring&, unsigned long) -> std::wstring {
        return L"已切到前台：x";
    };
    hooks.onLocateAndClick = [](const std::wstring&, int, const std::wstring&, int, int) {
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
    hooks.onLocateAndClick = [](const std::wstring&, int, const std::wstring&, int, int) {
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
        const std::wstring& button, int clickCount, int) -> std::wstring {
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
    // ★批 D（docs §47 / D1）：原先三条**站点专属**的静默拒绝（「站点接口不是给人看的页面」
    //   「不要打开 space 空根路径」「不要猜用户数字 UID」）与「已在同一站点 ⇒ 禁止再编地址」
    //   都已删，改成**放行 + 如实标注**。本用例反转钉新不变式。
    ResetAiActionSessionState(91);
    const bool apiUrl = LooksLikeNonUserFacingWebUrl(
        L"https://api.bilibili.com/x/web-interface/search/type?search_type=bili_user&keyword=x");
    const bool searchOk = !LooksLikeNonUserFacingWebUrl(
        L"https://search.bilibili.com/upuser?keyword=%E6%9C%89%E5%B1%B1");
    const bool spaceOk = !LooksLikeNonUserFacingWebUrl(L"https://space.bilibili.com/123");
    const bool sanitize = SanitizeObservePageQuery(L"搜索 有山先生") == L"\u6709\u5c71\u5148\u751f"
        && SanitizeObservePageQuery(L"\u641c\u7d22").empty();
    const bool site = ExtractUrlSiteKey(L"https://search.bilibili.com/upuser?k=a") == L"bilibili.com"
        && ExtractUrlSiteKey(L"https://api.bilibili.com/x/foo") == L"bilibili.com"
        && ExtractUrlSiteKey(L"https://www.example.com/a") == L"example.com";

    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring api = CallNamedTool(tools, L"openWebpage",
        L"{\"targetPath\":\"https://api.bilibili.com/x/web-interface/search/type?keyword=x\"}");
    const std::wstring spaceRoot = CallNamedTool(tools, L"openWebpage",
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
    const bool ok = apiUrl && searchOk && spaceOk && sanitize && site
        // 站点接口：照开，只多一句事实
        && api.rfind(L"[错误]", 0) != 0
        && api.find(L"[EXECUTED]") != std::wstring::npos
        && api.find(L"站点接口") != std::wstring::npos
        && api.find(L"风控") == std::wstring::npos
        // 空根路径 / 同站点第二个地址：都照开
        && spaceRoot.rfind(L"[错误]", 0) != 0
        && spaceRoot.find(L"[EXECUTED]") != std::wstring::npos
        && first.find(L"[EXECUTED]") != std::wstring::npos
        && second.find(L"[EXECUTED]") != std::wstring::npos
        && second.find(L"已在站点") == std::wstring::npos
        && home.find(L"[EXECUTED]") != std::wstring::npos
        && searchNav.find(L"[EXECUTED]") != std::wstring::npos
        && searchNav.find(L"已在站点") == std::wstring::npos
        && searchUrlOk;
    Emit(L"open_webpage_no_site_guard", ok,
        ok ? L"" : (api + L" | " + spaceRoot + L" | " + first + L" | " + second + L" | " + searchNav
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
    // ★批 D（docs §47 / D1）：站点专属的「已在搜索结果页…点 href 含 space.bilibili.com」已删 ⇒ 反向断言
    //   （注意：节点清单里仍会打印该节点自己的 href，那是**观测事实**，不算文案残留）
    const bool searchHint = searchText.find(L"已在搜索结果页") == std::wstring::npos
        && searchText.find(L"点 href 含") == std::wstring::npos
        && searchText.find(L"有山先生") != std::wstring::npos;

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
    // ★批 D（docs §47 / D1）：站点专属的「这是某个用户空间…」与「列表第1项=e3」不再出现；
    //   树文本只留**观测事实**（节点清单 + 可视/屏外分组）。
    const bool spaceHint = spaceText.find(L"用户空间") == std::wstring::npos
        && spaceText.find(L"列表第1项") == std::wstring::npos
        && spaceText.find(L"日本篇") != std::wstring::npos
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
    // ★批 D：站点专属的「播放页：工具栏按钮优先 clickRef…相关推荐不是目标」已删
    const bool watchHint = watchText.find(L"[mixed]") != std::wstring::npos
        && watchText.find(L"clickRef") != std::wstring::npos
        && watchText.find(L"点赞") != std::wstring::npos
        && watchText.find(L"点赞按钮") == std::wstring::npos
        && watchText.find(L"勿点分享") == std::wstring::npos
        && watchText.find(L"locateAndClick") != std::wstring::npos
        && watchText.find(L"列表第1项") == std::wstring::npos
        && watchText.find(L"工具栏按钮") == std::wstring::npos
        && watchText.find(L"相关推荐不是目标") == std::wstring::npos
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
        && gridText.find(L"列表第1项") == std::wstring::npos
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

// ★长列表页覆盖度：树被砍过必须**明说**，且分页信息要可行动。
//
// 实测背景（超星「第1章概述练习题」50 题作业页）：扩展在排序后把候选砍到 64 条，
// 一道多选题连题干带选项吃掉 5~6 条 ⇒ **只有前 1~2 题进树**，第 3 题起全没了。
// 当时的回执只有 `n=21` +「屏外0 · 下17.6屏」—— 树**自洽**，看不出丢了 48 道题，
// 模型于是退化成「盲滚 + 截图抄」。所以本用例钉三件事：
//   ① 全量时明说「本页 N 个…已全部列出」（模型才能区分「真的只有这些」）；
//   ② 被切过时明说「共 N 个，本次给的是第 a~b 条」并给可用的 nextOffset；
//   ③ 旧扩展（无覆盖度字段）**不能谎报完整**。
void CaseSnapshotReportsCoverageAndPaging() {
    // ① 全量：enumerateTotal == nodes.size() ⇒ 明确告知「已全部列出」
    const std::string fullJson =
        R"({"ok":true,"pageKind":"dom","url":"https://example.com/q","title":"练习题","enumerateTotal":3,"offset":0,"nextOffset":0,)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"checkbox","name":"选项A","x":10,"y":100,"w":20,"h":20,"inView":true},)"
        R"({"ref":"e2","role":"checkbox","name":"选项B","x":10,"y":130,"w":20,"h":20,"inView":true},)"
        R"({"ref":"e3","role":"checkbox","name":"选项C","x":10,"y":160,"w":20,"h":20,"inView":true}]})";
    const PageSnapshot full = ParsePageSnapshotJson(fullJson);
    const std::wstring fullText = FormatPageSnapshotForAgent(full);
    const bool fullOk = full.enumerateTotal == 3 && full.nextOffset == 0
        && fullText.find(L"已全部列出") != std::wstring::npos
        && fullText.find(L"没给完") == std::wstring::npos;

    // ② 分页中：给了第 1~2 条 / 共 120 条 ⇒ 必须说清「还有」，并给出下一段 offset
    const std::string pageJson =
        R"({"ok":true,"pageKind":"dom","url":"https://example.com/q","title":"练习题","enumerateTotal":120,"offset":0,"nextOffset":2,)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"checkbox","name":"第1题选项A","x":10,"y":100,"w":20,"h":20,"inView":true},)"
        R"({"ref":"e2","role":"checkbox","name":"第1题选项B","x":10,"y":130,"w":20,"h":20,"inView":true}]})";
    const PageSnapshot paged = ParsePageSnapshotJson(pageJson);
    const std::wstring pagedText = FormatPageSnapshotForAgent(paged);
    const bool pagedOk = paged.enumerateTotal == 120 && paged.nextOffset == 2
        && pagedText.find(L"共 120") != std::wstring::npos
        && pagedText.find(L"第 1~2 条") != std::wstring::npos
        && pagedText.find(L"没给完") != std::wstring::npos
        && pagedText.find(L"offset=2") != std::wstring::npos;

    // ③ 第二段（offset=2）：措辞要反映真实区间「第 3~3 条」，别再说第 1 条
    const std::string page2Json =
        R"({"ok":true,"pageKind":"dom","url":"https://example.com/q","title":"练习题","enumerateTotal":120,"offset":2,"nextOffset":3,)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"checkbox","name":"第2题选项A","x":10,"y":100,"w":20,"h":20,"inView":true}]})";
    const std::wstring page2Text = FormatPageSnapshotForAgent(ParsePageSnapshotJson(page2Json));
    const bool page2Ok = page2Text.find(L"第 3~3 条") != std::wstring::npos;

    // ④ 旧扩展（无 enumerateTotal 字段）：**覆盖度未知**——既不能说「已全部列出」，
    //    也不能只说「没给完」了事（那是分页场景的措辞）。必须**明说「覆盖度未知」**，
    //    否则模型会把「可视区这 20 条」当成整页，退化成盲滚 + 截图（实测 3.77 CNY 那一局）。
    const std::string legacyJson =
        R"({"ok":true,"pageKind":"dom","url":"https://example.com/q","nodes":[)"
        R"({"ref":"e1","role":"button","name":"旧扩展","x":1,"y":2,"w":3,"h":4}]})";
    const PageSnapshot legacy = ParsePageSnapshotJson(legacyJson);
    const std::wstring legacyText = FormatPageSnapshotForAgent(legacy);
    const bool legacyOk = legacy.enumerateTotal == -1
        && legacyText.find(L"已全部列出") == std::wstring::npos
        && legacyText.find(L"没给完") == std::wstring::npos
        && legacyText.find(L"覆盖度未知") != std::wstring::npos;

    // ⑤ absent 字段：新扩展会标出「与视口完全不相交」的节点。
    //    ① 解析要认这个字段（absent=true ⇒ 1）；
    //    ② 屏外的节点**照样要出现在树里**（长列表页正是靠这个才能一次枚举全），
    //       同时覆盖度要把它算进「屏外」计数，不能再报「屏外0」。
    //    ⚠ 当前扩展里 absent 与 inView 同源（同一判据）⇒ 恒有 absent == !inView。
    //      这是**契约现状**，不是巧合：两个字段语义不同（算「可视N」vs 算「屏外M」），
    //      将来要区分「横跨边界」时才拆开。用例只钉**当前**契约，别把两者当成反义去简化。
    const std::string absentJson =
        R"({"ok":true,"pageKind":"dom","url":"https://example.com/q","title":"练习题",)"
        R"("enumerateTotal":3,"offset":0,"nextOffset":0,"vh":800,"scrollY":0,"pageHeight":4000,)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"checkbox","name":"可视项","x":10,"y":100,"w":20,"h":20,"inView":true,"absent":false},)"
        R"({"ref":"e2","role":"checkbox","name":"屏外项","x":10,"y":1500,"w":20,"h":20,"inView":false,"absent":true},)"
        R"({"ref":"e3","role":"checkbox","name":"更远屏外项","x":10,"y":2500,"w":20,"h":20,"inView":false,"absent":true}]})";
    const PageSnapshot absent = ParsePageSnapshotJson(absentJson);
    const int absentFlagged = absent.nodes.size() == 3 && absent.nodes[1].absent == 1
        && absent.nodes[2].absent == 1 && absent.nodes[0].absent == 0 ? 1 : 0;
    const std::wstring absentText = FormatPageSnapshotForAgent(absent);
    // 三条都在树里（没被砍）+ 屏外计数如实（2 条）
    const bool absentOk = absentFlagged == 1 && absent.nodes.size() == 3
        && PageSnapshotNodeAbsent(absent.nodes[1], absent)
        && !PageSnapshotNodeAbsent(absent.nodes[0], absent)
        && absentText.find(L"屏外2") != std::wstring::npos
        && absentText.find(L"可视1") != std::wstring::npos;

    // ⑥ 旧扩展（**没有** absent 字段）⇒ 回落 y/vh 推断，不能用未初始化值误报屏外。
    //    这条是 ⑤ 的负对照：把 absent 删掉，覆盖度仍要算对（不能变成 0）。
    const std::string noAbsentJson =
        R"({"ok":true,"pageKind":"dom","url":"https://example.com/q","title":"练习题",)"
        R"("enumerateTotal":2,"offset":0,"nextOffset":0,"vh":800,"scrollY":0,"pageHeight":4000,)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"checkbox","name":"可视项","x":10,"y":100,"w":20,"h":20,"inView":true},)"
        R"({"ref":"e2","role":"checkbox","name":"屏外项","x":10,"y":1500,"w":20,"h":20,"inView":false}]})";
    const PageSnapshot noAbsent = ParsePageSnapshotJson(noAbsentJson);
    const std::wstring noAbsentText = FormatPageSnapshotForAgent(noAbsent);
    const bool noAbsentOk = noAbsent.nodes.size() == 2
        && noAbsent.nodes[1].absent == -1            // 未提供 ⇒ 保持 -1
        && PageSnapshotNodeAbsent(noAbsent.nodes[1], noAbsent)   // y=1500 ≥ vh=800 ⇒ 屏外
        && !PageSnapshotNodeAbsent(noAbsent.nodes[0], noAbsent)
        && noAbsentText.find(L"屏外1") != std::wstring::npos;

    const bool ok = fullOk && pagedOk && page2Ok && legacyOk && absentOk && noAbsentOk;
    Emit(L"snapshot_reports_coverage_and_paging", ok,
        ok ? L"" : (L"full=" + std::to_wstring(fullOk) + L" paged=" + std::to_wstring(pagedOk)
            + L" page2=" + std::to_wstring(page2Ok) + L" legacy=" + std::to_wstring(legacyOk)
            + L" absent=" + std::to_wstring(absentOk) + L" noAbsent=" + std::to_wstring(noAbsentOk)
            + L" | fullText=" + fullText.substr(0, 160)
            + L" | pagedText=" + pagedText.substr(0, 200)).c_str());
}

void CaseScrollWheelNotBlockedWhenViewportHasContent() {
    // ★批 D（docs §47）：原先「可视区已有主内容 ⇒ 必须 confirmScroll 才准滚」的静默拒绝
    //   已删（拿跨帧的「上一次」状态替模型决定该不该滚）。本用例反转钉新不变式：
    //   **照滚**，`confirmScroll` 入参也不复存在。
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
    const std::wstring scrolled = CallNamedTool(tools, L"scrollWheel",
        L"{\"scrollDirection\":1,\"scrollSteps\":5}");
    const bool scrolledOk = scrolled.rfind(L"[错误]", 0) != 0
        && scrolled.find(L"[EXECUTED]") != std::wstring::npos
        && scrolled.find(L"confirmScroll") == std::wstring::npos;

    ResetAiActionSessionState(815);
    AiNotePageKind(L"dom");
    const auto tools2 = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring empty = CallNamedTool(tools2, L"scrollWheel",
        L"{\"scrollDirection\":1,\"scrollSteps\":3}");
    const bool emptyOk = empty.find(L"[错误]") == std::wstring::npos;

    const bool ok = scrolledOk && emptyOk;
    Emit(L"scroll_wheel_not_blocked_when_viewport_has_content", ok,
        ok ? L"" : (L"scrolled=" + scrolled.substr(0, 160) + L" | empty=" + empty.substr(0, 80)).c_str());
}

void CaseListFirstNoLongerBlocks() {
    // ★批 D（docs §47）：原先「有列表第1项 ⇒ 禁止滚动 / 禁止点其它内容卡」两条静默拒绝
    //   已删（判据是「重复卡片网格的第 1 项」+ 按 URL 形状判播放页）。反转钉新不变式：
    //   滚动与点击**一律照发**，快照文本里也不再出现「列表第1项」这个已消失的概念。
    ResetAiActionSessionState(818);
    const std::string gridJson =
        R"({"ok":true,"pageKind":"dom","vw":1699,"vh":780,)"
        R"("url":"https://example.com/upload/video","title":"投稿",)"
        R"("nodes":[)"
        R"({"ref":"e1","role":"link","name":"最上最左投稿","href":"/video/BV1aa","inView":true,"x":32,"y":240,"w":180,"h":90},)"
        R"({"ref":"e5","role":"link","name":"后面一张投稿","href":"/video/BV1ee","inView":true,"x":220,"y":240,"w":180,"h":90},)"
        R"({"ref":"e7","role":"link","name":"投稿","href":"/upload/video","inView":true,"x":80,"y":80,"w":48,"h":24}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(gridJson));
    // clickRef 现在会真的走到扩展钩子（原先这些断言在「站点/列表」拒绝处就提前返回了）
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) -> std::wstring { return L"ok"; };
    hooks.onClickRef = [](const std::wstring& ref, bool) -> std::wstring {
        return L"clicked " + ref;
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring scroll = CallNamedTool(tools, L"scrollWheel",
        L"{\"scrollDirection\":1,\"scrollSteps\":6}");
    const bool scrollOk = scroll.rfind(L"[错误]", 0) != 0
        && scroll.find(L"列表第1项") == std::wstring::npos;
    const std::wstring otherCard = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e5\"}");
    const bool otherNotBlocked = otherCard.rfind(L"[错误]", 0) != 0;
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
    const bool watchNoListFirst = watchFmt.find(L"列表第1项") == std::wstring::npos
        && watchFmt.find(L"播放页") == std::wstring::npos;   // 站点专属「播放页」建议也已删
    const std::wstring like = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e3\"}");
    const bool likeNotListBan = like.find(L"另一张卡") == std::wstring::npos;
    const std::wstring watchScroll = CallNamedTool(tools, L"scrollWheel",
        L"{\"scrollDirection\":1,\"scrollSteps\":3}");
    const bool watchScrollOk = watchScroll.find(L"列表第1项") == std::wstring::npos;

    const bool ok = scrollOk && otherNotBlocked && firstNotListBan && tabNotListBan
        && watchNoListFirst && likeNotListBan && watchScrollOk;
    Emit(L"list_first_no_longer_blocks", ok,
        ok ? L"" : (L"scroll=" + scroll.substr(0, 140)
            + L" | e5=" + otherCard.substr(0, 120)
            + L" | e1=" + firstCard.substr(0, 80)
            + L" | e7=" + tab.substr(0, 80)
            + L" | like=" + like.substr(0, 80)
            + L" | wfmt=" + watchFmt.substr(0, 80)
            + L" | wscroll=" + watchScroll.substr(0, 80)).c_str());
}

void CaseWebBrowseAllowsVisionFallback() {
    ResetAiActionSessionState(816);
    AiNoteWebBrowseSession(true);

    // 「前台是浏览器」必须固定：本用例断言网页上下文的判断生效，而它读真实前台窗口，
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
    // ★批 D（docs §47）：原先这里断言「有控件树 ⇒ 拦 Enter 并要求 searchOnPage」。
    //   那条静默拒绝已删 → 反转成新不变式：**Enter 照发**，只多一句如实标注。
    const bool enterWithTree = enter2.rfind(L"[错误]", 0) != 0
        && enter2.find(L"searchOnPage") == std::wstring::npos
        && enter2.find(L"[事实]") != std::wstring::npos;

    ResetAiActionSessionState(817);

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

    // ★引擎**不**拿「上一轮识图没找到目标」否决 completeTask（那条内联否决闸已删）：
    //   模型要收工就如实收工 —— 刚记过一次定位失败也必须放行，否则它被锁死在任务里。
    AiNoteLocateMiss();
    const std::wstring fakeDone = CallNamedTool(tools, L"completeTask",
        L"{\"reason\":\"\u5df2\u70b9\u8d5e\"}");
    const bool allowAfterMiss = fakeDone.find(L"[TASK_COMPLETE]") != std::wstring::npos;
    const std::wstring honest = CallNamedTool(tools, L"completeTask",
        L"{\"reason\":\"\u672a\u627e\u5230\u76ee\u6807\"}");
    const bool allowFail = honest.find(L"[TASK_COMPLETE]") != std::wstring::npos;

    const bool ok = locToRef && noGuess && allowAfterMiss && allowFail;
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
    hooks.onObservePage = [](bool, const std::wstring&, const std::wstring&,
                                int) -> std::wstring {
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
        // ★批 D（docs §47）：原先这两条断言钉的是「已在搜索结果页 ⇒ 拦 typeRef」
        //   与「有控件树 ⇒ 拦 Enter」——两条静默拒绝都已删，反转成「照常执行」。
        && typeAgain.rfind(L"[错误]", 0) != 0
        && typeAgain.find(L"\u641c\u7d22\u7ed3\u679c") == std::wstring::npos
        && typeAgain.find(L"typeRef") != std::wstring::npos
        && enterDom.rfind(L"[错误]", 0) != 0
        && enterDom.find(L"typeRef") == std::wstring::npos;
    AiNotePageKind(L"canvas");
    const std::wstring skipHtml = CallNamedTool(tools, L"observePage", L"{}");
    const std::wstring canvasClick = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e1\"}");
    const std::wstring canvasType = CallNamedTool(tools, L"typeRef",
        L"{\"ref\":\"e1\",\"text\":\"x\"}");
    const std::wstring force = CallNamedTool(tools, L"observePage", L"{\"force\":true}");
    const bool canvasGate = skipHtml.find(L"禁止再抓 HTML") != std::wstring::npos
        // ★批 D：画布页的拒绝文案改成**事实句**（「没有控件树 ⇒ 解析不了」），
        //   不再是「禁止 clickRef/typeRef」（引擎不该用「禁止」掩盖能力缺失）。
        && canvasClick.find(L"canvas") != std::wstring::npos
        && canvasClick.find(L"没有可解析的 ref") != std::wstring::npos
        && canvasType.find(L"canvas") != std::wstring::npos
        && canvasType.find(L"没有可解析的 ref") != std::wstring::npos
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
    // ★批 D（docs §47 / D1）：原先「已在「X」的搜索结果页 ⇒ 拒绝再搜」那条站点专属
    //   静默拒绝已删 ⇒ 同一个词再搜一次也**照执行**（要不要再搜由模型判断）。
    const bool dupOk = again.rfind(L"[错误]", 0) != 0
        && again.find(L"[EXECUTED]") != std::wstring::npos
        && again.find(L"搜索结果") == std::wstring::npos;
    // ★批 E（docs §48）反转：原先这里还断言「动态填表场景下 searchOnPage 被裁掉」
    //   （按提示词子串裁工具表）。裁表整族已撤销 ⇒ 该断言连同构造一起删，
    //   改为断言**工具表永远给全**：同一份工具表里 openWebpage / searchOnPage / runProgram
    //   必须都在（被裁掉的就是它们，包括那条「游戏里查玩法的唯一出口」fetchWebPage）。
    const auto fullTools = BuildAiActionExecuteTools(nullptr, {});
    bool hasSearch = false, hasOpenWeb = false, hasRunProg = false, hasFetchWeb = false;
    for (const auto& t : fullTools) {
        if (t.name == L"searchOnPage") hasSearch = true;
        if (t.name == L"openWebpage") hasOpenWeb = true;
        if (t.name == L"runProgram") hasRunProg = true;
        if (t.name == L"fetchWebPage") hasFetchWeb = true;
    }
    const bool toolTableComplete = hasSearch && hasOpenWeb && hasRunProg && hasFetchWeb;
    const bool ok = needSiteOk && navOk && dupOk && toolTableComplete;
    Emit(L"search_on_page_tool", ok,
        ok ? L"" : (needSite + L" | " + searched + L" | " + navUrl + L" | " + again).c_str());
    ResetAiActionSessionState(79);
}

void CaseClickRefNavigatesSpaceHref() {
    // ★批 D（docs §47）反转：clickRef 现在**只如实点击**——
    //   ① 不再按站点启发式「直接导航」（原来命中某站空间 URL 就跳过点击自己 onNavigatePage）；
    //   ② 不再有「列表第1项」拦点别的卡；
    //   ③ 连续两次未跳转时不再**自己从快照挑一个链接导航过去**。
    //   相对路径解析也改成与站点无关（`/space/28626598` 不再被魔改成 space.* 域名）。
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
        && rAll.empty()                                  // 同路径筛选项（/all?vt=1）不算导航
        && rBare == L"https://search.bilibili.com/28626598"  // 相对路径按当前站点解析
        && rSlashSpace == L"https://search.bilibili.com/space/28626598"
        && PageUrlsSameDocument(searchUrl, L"https://search.bilibili.com/all?vt=9&keyword=x")
        && !PageUrlsSameDocument(searchUrl, spaceUrl);

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
    // 空间链接也走**点击**（不再由引擎直接导航）
    const bool navClick = clickCount == 1
        && navUrl.empty()
        && clicked.find(L"clicked e1") != std::wstring::npos;

    ResetAiActionSessionState(811);
    navUrl.clear();
    clickCount = 0;
    AiNotePageSnapshot(ParsePageSnapshotJson(searchJson));
    AiNotePageKind(L"dom");
    const std::wstring tab1 = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e2\"}");
    const std::wstring tab2 = CallNamedTool(tools, L"clickRef", L"{\"ref\":\"e3\"}");
    // 连续两次未跳转：只如实报计数，**不再**替模型挑链接导航
    const bool streakHonest = clickCount == 2
        && navUrl.empty()
        && tab1.find(L"[事实]") != std::wstring::npos
        && tab2.find(L"[事实]") != std::wstring::npos
        && tab2.find(L"2 次") != std::wstring::npos
        && tab2.find(L"space.bilibili.com") == std::wstring::npos;

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
    const std::wstring spaceGuess = CallNamedTool(tools2, L"openWebpage",
        L"{\"targetPath\":\"https://space.bilibili.com/28626598\"}");
    // 「不要猜用户数字 UID」那条站点专属拒绝已删 ⇒ 照开
    const bool guessAllowed = spaceGuess.rfind(L"[错误]", 0) != 0
        && spaceGuess.find(L"猜") == std::wstring::npos;

    AiActionToolOptions opts;
    opts.allowAbsolutePointer = false;
    const auto noImgTools = BuildAiActionExecuteTools(nullptr, opts);
    // 无图：mouseClick 照执行 + 如实标注（批 D 由「拒绝」改成「执行 + 标注」）
    const std::wstring noImgClick = CallNamedTool(noImgTools, L"mouseClick",
        L"{\"x\":10,\"y\":20}");
    const bool noImgOk = noImgClick.rfind(L"[错误]", 0) != 0
        && noImgClick.find(L"[EXECUTED]") != std::wstring::npos
        && noImgClick.find(L"本轮没有把截图回传给模型") != std::wstring::npos;
    // scrollWheel 的 x/y 在无图时被工具自己忽略（本来就没有可映射的基准），照滚
    const std::wstring wheel = CallNamedTool(noImgTools, L"scrollWheel",
        L"{\"scrollDirection\":0,\"x\":10,\"y\":20}");
    const bool wheelOk = wheel.rfind(L"[错误]", 0) != 0
        && wheel.find(L"[EXECUTED]") != std::wstring::npos
        && wheel.find(L"禁止") == std::wstring::npos;

    const bool ok = resolveOk && spaceFromSearch && navClick && streakHonest
        && videoClicksElement && guessAllowed && noImgOk && wheelOk;
    std::wstring flags = L" f=";
    flags.push_back(resolveOk ? L'1' : L'0');
    flags.push_back(spaceFromSearch ? L'1' : L'0');
    flags.push_back(navClick ? L'1' : L'0');
    flags.push_back(streakHonest ? L'1' : L'0');
    flags.push_back(videoClicksElement ? L'1' : L'0');
    flags.push_back(guessAllowed ? L'1' : L'0');
    flags.push_back(noImgOk ? L'1' : L'0');
    flags.push_back(wheelOk ? L'1' : L'0');
    Emit(L"click_ref_navigates_space_href", ok,
        ok ? L"" : (flags + L" rSpace=" + rSpace + L" rAll=[" + rAll + L"] rBare=[" + rBare
            + L"] rSlash=" + rSlashSpace
            + L" | " + spaceOpen + L" | " + clicked + L" | " + tab1 + L" | " + tab2
            + L" | " + videoClick + L" | " + spaceGuess + L" | " + noImgClick + L" | " + wheel).c_str());
    ResetAiActionSessionState(810);
}

void CaseSamePageClickNotBlocked() {
    // ★批 D（docs §47）反转：同页点击不再被任何闸拦（原「连续两次未跳转 ⇒ 拒绝 +
    //   引擎自己挑一个链接导航过去」整块删除）。新不变式：**照点**，只如实报计数。
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
    // ★批 D：作者链接也走**点击**（引擎不再自己挑链接导航）
    const bool upClicksElement = up.find(L"[错误]") != 0
        && clickCount == 2
        && navUrl.empty()
        && up.find(L"clicked e3") != std::wstring::npos;

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
    // ★批 D：那条站点专属药方（「请点 href 含 space.bilibili.com 的卡片」）已删 ⇒ 反向断言
    const bool hintGone = tab.find(L"请点 href 含 space.bilibili.com") == std::wstring::npos
        && tab.find(L"[事实]") != std::wstring::npos;

    const bool ok = likeOk && upClicksElement && hintGone;
    Emit(L"same_page_click_not_blocked", ok,
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
    // ★批 D（docs §47）：原先「网页标签内 ⇒ 拦 Ctrl+T/N」的静默拒绝已删。
    //   反转钉新不变式：**照按**，回执里补一句「会新开一个标签页」的事实。
    AiNotePageKind(L"mixed");
    const std::wstring ctrlT = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"t\",\"holdLeftCtrl\":true,\"confirmShortcut\":true,\"observeAfter\":false}");
    const bool newTabAnnotated = ctrlT.rfind(L"[错误]", 0) != 0
        && ctrlT.find(L"[EXECUTED]") != std::wstring::npos
        && ctrlT.find(L"新开一个标签页") != std::wstring::npos;
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
    const bool ok = blocked && confirmed && universal && skillClean && newTabAnnotated;
    std::wstring flags = L" f=";
    flags.push_back(blocked ? L'1' : L'0');
    flags.push_back(confirmed ? L'1' : L'0');
    flags.push_back(universal ? L'1' : L'0');
    flags.push_back(skillClean ? L'1' : L'0');
    flags.push_back(newTabAnnotated ? L'1' : L'0');
    Emit(L"non_universal_shortcut_guard", ok,
        ok ? L"" : (flags + L" ctrlH=" + ctrlH.substr(0, 120) + L" ctrlT=" + ctrlT.substr(0, 120)).c_str());
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

void CasePlanGateRemoved() {
    // ★批 D（docs §47）：「先写 goal/todos 才准动手」的计划门闩**整族已删**。
    //   本用例由旧 `plan_gate_requires_memo` **反转**而来（旧断言是 blocked=[错误]+
    //   updateTaskMemo —— 那正是「守卫把模型锁在门外」）。新不变式：
    //   **没写任何备忘时执行类工具也照常执行**，备忘工具本身保留。
    ResetAiActionSessionState(424242);
    const auto tools = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring first = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"hello\",\"observeAfter\":false}");
    const bool noLock = first.rfind(L"[错误]", 0) != 0
        && first.find(L"[EXECUTED]") != std::wstring::npos
        && first.find(L"updateTaskMemo") == std::wstring::npos;
    const std::wstring memo = CallNamedTool(tools, L"updateTaskMemo",
        L"{\"section\":\"goal\",\"note\":\"\u6d4b\u8bd5\u76ee\u6807\"}");
    const bool memoOk = memo.find(L"[错误]") == std::wstring::npos;
    const std::wstring prefixMemo = CallNamedTool(tools, L"updateTaskMemo",
        L"{\"note\":\"goal: \u6253\u5f00B\u7ad9\\ntodos:\\n1) searchOnPage\"}");
    const bool prefixOk = prefixMemo.find(L"[错误]") == std::wstring::npos;

    const bool ok = noLock && memoOk && prefixOk;
    Emit(L"plan_gate_removed", ok,
        ok ? L"" : (first + L" | " + memo + L" | " + prefixMemo).c_str());
}

void CaseLocateTargetLengthGuard() {
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

    // ★批 D（docs §47）：原先这里钉着「禁止用 aiActionExecute 外包定位」那条
    //   **对提示词做文本匹配**的判据（要求 nested 回执里出现 locateAndClick）。
    //   判据已删 ⇒ 断言反过来：回执里**不再**出现那条外包禁令文案
    //   （不再断言它必然成功：嵌套动作还要过「AI 模型已配置」这道真实的参数校验）。
    const std::wstring nested = CallSubmitTool(tools,
        L"{\"actions\":[{\"type\":\"aiActionExecute\",\"aiPrompt\":\"\u70b9\u51fb\u786e\u5b9a\u6309\u94ae\"}]}");
    const bool nestedOk = nested.find(L"外包") == std::wstring::npos
        && nested.find(L"禁止用 aiActionExecute") == std::wstring::npos;
    const bool ok = longOk && nestedOk;
    Emit(L"locate_target_length_guard", ok,
        ok ? L"" : (longR + L" | " + nested).c_str());
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
    // ★批 D（docs §47 / D6④）：视觉闸只保留**判断 + 留痕**，不再拦 locateAndClick。
    //   本用例由旧 `history_sidebar_and_busy_guards` 的「高频动态 ⇒ 拦识图」**反转**：
    //   同样一份宿主信号下，定位请求**必须照发**（回执里不允许再出现「动态干扰过大」）。
    SetAiForegroundBrowserOverrideForTest(1);
    NoteAiActionUiBusy(0.20, true);
    const bool busy = AiActionUiTooBusyForVisionLocate();   // 判断照做（只留痕）
    const auto tools2 = BuildAiActionExecuteTools(nullptr, {});

    const std::wstring loc = CallNamedTool(tools2, L"locateAndClick",
        L"{\"target\":\"\u67e5\u770b\u66f4\u591a\"}");
    const bool locNotBlocked = loc.find(L"动态") == std::wstring::npos
        && loc.find(L"Ctrl+T") == std::wstring::npos
        && loc.find(L"Ctrl+t") == std::wstring::npos;

    ResetAiActionSessionState(454547);
    AiNotePageKind(L"mixed");
    NoteAiActionUiBusy(0.20, true);

    const auto tools3 = BuildAiActionExecuteTools(nullptr, {});
    const std::wstring like = CallNamedTool(tools3, L"locateAndClick",
        L"{\"target\":\"\u89c6\u9891\u4e0b\u65b9\u5de6\u4fa7\u5927\u62c7\u6307\u5411\u4e0a\u7684\u70b9\u8d5e\u6309\u94ae\"}");
    const bool likeFallback = like.find(L"\u52a8\u6001\u5e72\u6270") == std::wstring::npos
        && like.find(L"\u6b63\u5728\u6d4f\u89c8\u7f51\u9875") == std::wstring::npos
        && like.find(L"Ctrl+T") == std::wstring::npos;

    const bool ok = histRejected && descClean && busy && locNotBlocked && likeFallback;
    Emit(L"history_sidebar_and_busy_guards", ok,
        ok ? L"" : (L"loc=" + loc.substr(0, 120) + L" | like=" + like.substr(0, 120)).c_str());
}

void CaseLocateMultiTargets() {
    // 「拿卡→放卡」这类两步操作要能一次调用做完（一次识图定位 + 立即连点），
    // 而不是发两次 tool call（每次都要主模型一轮 + 1.5~2.6s 界面稳定等待）。
    bool ok = true;
    std::wstring detail;
    ResetAiActionSessionState(484848);
    SetAiForegroundBrowserOverrideForTest(-1);
    SetAiSpreadsheetForegroundOverrideForTest(0);


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
    hooks.onLocateAndClick = [&](const std::wstring&, int, const std::wstring&, int, int) -> std::wstring {
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


    int hostCalls = 0;
    AiActionHostHooks hooks;
    hooks.onLocateAndClick = [&](const std::wstring&, int, const std::wstring&, int, int) -> std::wstring {
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
    hooks2.onLocateAndClick = [&](const std::wstring&, int, const std::wstring&, int, int) -> std::wstring {
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

void CaseLocateRetryHardCap() {
    ResetAiActionSessionState(42);
    const int initial = AiLocateRetryCount();
    AiNoteLocateMiss();
    const int afterOne = AiLocateRetryCount();
    AiNoteLocateMiss();
    const int afterTwo = AiLocateRetryCount();
    AiResetLocateMiss();
    const int afterClear = AiLocateRetryCount();
    const bool ok = initial == 0 && afterOne == 1 && afterTwo == 2 && afterClear == 0;
    Emit(L"locate_retry_hard_cap", ok,
        (ok ? L"" : (L"count=" + std::to_wstring(initial) + L"," + std::to_wstring(afterOne)
            + L"," + std::to_wstring(afterTwo) + L"," + std::to_wstring(afterClear)).c_str()));
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

// ★批 D（docs §47）：宿主预规划（引擎每轮工具批之后自己再发一次 API 猜下一步）
//   整条链路连同它的两个用例（预规划接受/丢弃、预规划等待预算）已整体删除 ——
//   引擎替模型先想一步 = 引擎在决策。用例层面不再有可钉的不变式：
//   它不是一个「闸」，而是一整块被删掉的引擎行为。

void CaseRejectAbsoluteNoImage() {
    // ★批 D（docs §47）反转：原先「无截图 ⇒ 拒绝绝对坐标点击」改成**执行 + 如实标注**
    //   （宿主本来就有截图映射，模型可能知道得比守卫多）。用例名保留旧名以便对照 git 历史，
    //   断言换成新不变式：**照点**，回执里带那句事实。
    AiActionToolOptions opts;
    opts.allowAbsolutePointer = false;
    const auto tools = BuildAiActionExecuteTools(nullptr, opts);
    const std::wstring r = CallSubmitTool(tools,
        L"{\"actions\":[{\"type\":\"mouseClick\",\"x\":100,\"y\":200}]}");
    const bool ok = r.rfind(L"[错误]", 0) != 0
        && r.find(L"[EXECUTED]") != std::wstring::npos
        && r.find(L"本轮没有把截图回传给模型") != std::wstring::npos
        && r.find(L"禁止") == std::wstring::npos;
    Emit(L"absolute_pointer_no_image_annotated", ok, r.c_str());
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

/// 颜色的「匹配度」必须统一在 0~100（找色/颜色匹配过去写 `100 - 色差`，
/// 而色差是单通道最大差 0~255 ⇒ 会出负数）。钉住四档：
/// 同色=100、容差内、超过 100 的色差=0（不是负）、极端值仍钳在 0~100。
void CaseColorMatchScorePercent() {
    bool ok = ColorMatchScorePercent(0) == 100
        && ColorMatchScorePercent(4) == 96
        && ColorMatchScorePercent(16) == 84
        && ColorMatchScorePercent(100) == 0      // 过去这里是 0，再大就是负数
        && ColorMatchScorePercent(255) == 0;
    std::wstring detail;
    if (!ok) {
        detail = L"0→" + std::to_wstring(ColorMatchScorePercent(0))
            + L" 16→" + std::to_wstring(ColorMatchScorePercent(16))
            + L" 100→" + std::to_wstring(ColorMatchScorePercent(100))
            + L" 255→" + std::to_wstring(ColorMatchScorePercent(255));
    }
    Emit(L"color_match_score_percent", ok, ok ? L"" : detail.c_str());
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
    // ★★歧义必须**留痕**（docs §61）：本帧 1024×576 ≤1000 ⇒ 「按像素读」同样成立，
    //   判据（四个数都 ≤1000）**认不出**这两套。纪律是不挑一种当结论、也不沉默，
    //   而是把另一种读法的原值一起写进诊断 —— 让真实日志去回答模型按哪套回答。
    const bool notedAmbiguity = ok
        && note.find(L"无法区分") != std::wstring::npos
        && note.find(L"677,134,717,186") != std::wstring::npos;
    Emit(L"resolve_vision_ambiguous_in_image_as_norm1000", mappedOk && notedAmbiguity,
        (L"api=[" + std::to_wstring(x1) + L"," + std::to_wstring(y1) + L","
            + std::to_wstring(x2) + L"," + std::to_wstring(y2) + L"] c=("
            + std::to_wstring(cx) + L"," + std::to_wstring(cy)
            + L") screenY≈" + std::to_wstring(screenY) + L" " + note).c_str());
}

/// 反过来钉：**两套读法不可能同时成立时，不许说「无法区分」**
/// （否则「有歧义」这句话会退化成恒真的噪声，等于没有留痕）。
void CaseResolveVisionAmbiguityNotClaimedWhenImpossible() {
    int x1 = 356, y1 = 963, x2 = 375, y2 = 993;   // 993 > 720+2 ⇒ 「按像素读」不成立
    std::wstring note;
    const bool ok = ResolveVisionRectToApiImage(x1, y1, x2, y2, 1280, 720, 2560, 1440, &note);
    Emit(L"resolve_vision_ambiguity_not_claimed_when_impossible",
        ok && note.find(L"无法区分") == std::wstring::npos,
        (L"note=" + note).c_str());
}

/// ★落盘**端到端**（docs §61）：这条链路的失败形态是**静默**的 —— 日志根本不生成，
/// 而"没生成"只有在**真写一次、真读回来**时才发现得了（纯逻辑自检测不到路径解析、
/// 目录可写性、编码、裁剪这几段）。本 exe 就链着 `qst_desktop_tools`，所以能真做。
/// ⚠ 只**追加**、绝不清理：它跑在产品目录里，清理会删掉用户真实的日志。
void CaseMacroDebugLogFileRoundTrip() {
    const std::wstring marker = L"[自检] macro_debug_log_file_roundtrip pid="
        + std::to_wstring(GetCurrentProcessId());
    qst::desktop_tools::AppendMacroDebugLogFile(marker);
    const std::wstring path = qst::desktop_tools::MacroDebugLogFilePath();

    // 读**尾部**（文件上限 4MB，标记刚追加在末尾 ⇒ 从头读固定长度会读不到）
    std::wstring content;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") == 0 && fp) {
        _fseeki64(fp, 0, SEEK_END);
        const long long size = _ftelli64(fp);
        const long long keepBytes = 64 * 1024;
        const long long from = size > keepBytes ? size - keepBytes : 0;
        // 对齐到 wchar 边界，否则读出来是错位的乱码（UTF-16LE）
        _fseeki64(fp, from - (from % 2), SEEK_SET);
        std::vector<wchar_t> buf(static_cast<size_t>(keepBytes / 2) + 1);
        const size_t got = fread(buf.data(), sizeof(wchar_t), buf.size(), fp);
        fclose(fp);
        content.assign(buf.data(), got);
    }
    const bool ok = !content.empty()
        && content.find(marker) != std::wstring::npos
        && content.find(L"[自检]") != std::wstring::npos;
    Emit(L"macro_debug_log_file_roundtrip", ok,
        (L"path=" + path + L" readChars=" + std::to_wstring(content.size())).c_str());
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

// ★ openWebpage 的「打开目标」解析（2026-09-28）：用户任务写「打开哔哩哔哩」，
//   而旧实现只认完整 URL ⇒ 模型两次空调用、两轮白烧（实测报障，见 docs §4.16）。
//   这里测**纯函数**（不会真开浏览器）：站点名/裸域名/内置页/不认识四种。
void CaseResolveOpenWebpageTarget() {
    const bool ok = ResolveOpenWebpageTargetUrl(L"哔哩哔哩") == L"https://www.bilibili.com/"
        && ResolveOpenWebpageTargetUrl(L"B站") == L"https://www.bilibili.com/"
        && ResolveOpenWebpageTargetUrl(L"bilibili") == L"https://www.bilibili.com/"
        && ResolveOpenWebpageTargetUrl(L"www.bilibili.com") == L"https://www.bilibili.com"
        && ResolveOpenWebpageTargetUrl(L"https://example.com/a?b=1") == L"https://example.com/a?b=1"
        && ResolveOpenWebpageTargetUrl(L"edge://history") == L"edge://history"
        && ResolveOpenWebpageTargetUrl(L"随便什么不认识的东西").empty();
    // 提示文案必须**可执行**（列出可用写法）——它是给模型看的纠错信息
    const std::wstring hint = OpenWebpageTargetHint();
    const bool hintOk = hint.find(L"完整网址") != std::wstring::npos
        && hint.find(L"站点名") != std::wstring::npos
        && hint.find(L"哔哩哔哩") != std::wstring::npos;
    Emit(L"resolve_open_webpage_target", ok && hintOk, hint.c_str());
}
void CaseNeedScreenCaptureForInteractionVerbs() {
    // ★ 用户实测报障（2026-09-28）：「帮我打开哔哩哔哩，给up主"有山先生"主页的第一个视频点赞。」
    //   —— 旧判据只认「点击」这类**字面词**，而「点赞」语义上就是点屏幕 ⇒ 判成"无需看屏"
    //   ⇒ 未勾选带截图时首轮无图，模型盲着干（点到了帮助中心那种地方）。
    const bool yes =
        AiActionPromptLikelyNeedsScreenCapture(
            L"帮我打开哔哩哔哩，给up主\"有山先生\"主页的第一个视频点赞。")
        && AiActionPromptLikelyNeedsScreenCapture(L"把第一个商品加入购物车并下单")
        && AiActionPromptLikelyNeedsScreenCapture(L"关注这个up主并收藏视频")
        && AiActionPromptLikelyNeedsScreenCapture(L"登录后提交表单")
        && AiActionPromptLikelyNeedsScreenCapture(L"翻到下一页继续找");
    // ⚠ 纯数据任务**仍不许**判成"要看屏"（否则每次都白截一帧、白花 token）
    const bool no = !AiActionPromptLikelyNeedsScreenCapture(L"读取 D:\\a.xlsx 第三列求和")
        && !AiActionPromptLikelyNeedsScreenCapture(L"分析变量 count，若大于0则按 Enter")
        && !AiActionPromptLikelyNeedsScreenCapture(L"把这段文字翻译成英文");
    Emit(L"need_screen_capture_interaction_verbs", yes && no, L"");
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
        // ★★ 网页 AI 桥接（`web` / `doubao-web` …）**能传图**：网页通道经扩展把图上传进对话
        //   （`AttachImagesToPage` → `UploadImages`），PlanTurn 也真收到 imagesAttached。
        //   ⚠ 曾经漏判 ⇒ 观察截图一律不上传，模型看不到屏幕（用户实测报障）。
        && ModelSupportsVision(L"web")
        && ModelSupportsVision(L"doubao-web")
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

    const std::string watchJson =
        R"({"ok":true,"pageKind":"dom","url":"https://example.com/","nodes":[)"
        R"({"ref":"e1","role":"button","name":"登录","x":10,"y":200,"w":120,"h":36,"inView":true}]})";
    AiNotePageSnapshot(ParsePageSnapshotJson(watchJson));
    AiActionHostHooks hooks;
    hooks.onLocateAndClick = [](const std::wstring& target, int, const std::wstring&, int, int) {
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

// ★批 D（docs §47）：宿主预规划整条链路已删（连它的等待预算常量与次数上限一起），
//   原「预规划等待预算」用例随之删除 —— 没有「闸」可反转，只有一整块行为被移除。

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

    AiActionHostHooks hooks;
    hooks.onObservePage = [](bool, const std::wstring&, const std::wstring& query, int) {
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

    AiActionHostHooks hooks;
    int listedMax = 0;
    // ⚠ 签名与 `AiActionHostHooks::onListUiControls` **必须一致**（三参）。
    //   加了 typeFilter/nameFilter 之后，过滤在**宿主侧**做 —— 这里如实记下来，
    //   下面 `filterOk` 会核对「过滤真的传到宿主了」。
    std::wstring listedTypeFilter, listedNameFilter;
    hooks.onListUiControls = [&](int maxCount, const std::wstring& typeFilter,
                                     const std::wstring& nameFilter) -> std::wstring {
        listedMax = maxCount;
        listedTypeFilter = typeFilter;
        listedNameFilter = nameFilter;
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
    // ★批 D（docs §47）：原先这里断言「台账是只读工具，不该被计划门闩拦住」——
    //   门闩整族已删，只读/执行工具都不再有任何「先写 goal/todos」的前置条件。
    //   （只读工具确实照常可用，见上面 noHookList 的钩子缺失报错口径。）
    const bool readExempt = true;
    // ★过滤参数必须**原样透传到宿主**（过滤在宿主侧做 ⇒ 省 token；这里钉住"没被吞掉"）。
    //   同时钉住 maxCount 的**独立**透传（过滤不该影响它）。
    const std::wstring filtered = CallNamedTool(tools, L"listUiControls",
        L"{\"maxCount\":30,\"typeFilter\":\"ListItem\",\"nameFilter\":\"计算机\"}");
    const bool filterOk = listedTypeFilter == L"ListItem"
        && listedNameFilter == L"计算机" && listedMax == 30
        && filtered.find(L"[1]") != std::wstring::npos;
    // invokeUiControl 属于会改界面的执行类工具
    const bool listed = IsMacroActionRunToolName(L"invokeUiControl")
        && IsMacroExecutionToolName(L"invokeUiControl")
        && IsMacroExecutionToolName(L"listUiControls");
    const bool ok = hasList && hasInvoke && ledgerOk && invOk && skipOk && errOk && badOk
        && readExempt && listed && filterOk;
    Emit(L"ui_control_tools", ok,
        ok ? L"" : (L"ledger=" + ledger.substr(0, 60) + L" | inv=" + inv.substr(0, 60)
            + L" | skip=" + invSkip.substr(0, 40) + L" | err=" + invErr.substr(0, 40)
            + L" | noHook=" + noHookInvoke.substr(0, 60)).c_str());
    ResetAiActionSessionState(952);
}


HBITMAP CreateDibSection32(int w, int h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return nullptr;
    }
    return bmp;
}

// 从 src 的 (sx,sy) 起裁一块 w×h 出来（自检专用）
HBITMAP BlitSubRect32(HBITMAP src, int sx, int sy, int w, int h) {
    if (!src || w <= 0 || h <= 0) return nullptr;
    HBITMAP dst = CreateDibSection32(w, h);
    if (!dst) return nullptr;
    HDC dc = GetDC(nullptr);
    HDC sd = CreateCompatibleDC(dc);
    HDC dd = CreateCompatibleDC(dc);
    BOOL ok = FALSE;
    if (sd && dd) {
        HGDIOBJ o1 = SelectObject(sd, src);
        HGDIOBJ o2 = SelectObject(dd, dst);
        ok = BitBlt(dd, 0, 0, w, h, sd, sx, sy, SRCCOPY);
        SelectObject(dd, o2);
        SelectObject(sd, o1);
    }
    if (sd) DeleteDC(sd);
    if (dd) DeleteDC(dd);
    ReleaseDC(nullptr, dc);
    if (!ok) {
        DeleteObject(dst);
        return nullptr;
    }
    return dst;
}

// 冻结帧：一张已拍好的屏幕区域 + 它的屏幕矩形；帧源按请求裁它的子区。
// `outOfRange` 是**坐标口径**的直接判据 —— 搜索区是按 `s->box` 算的，
// 若 box 被存成**帧内**坐标，搜索区会整块落到冻结区外面（真 bug 时 3/3 全越界）。
struct FrozenFrame {
    HBITMAP bmp = nullptr;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    int outOfRange = 0;
};

HBITMAP FrozenFrameSource(int x1, int y1, int x2, int y2, void* user) {
    auto* ff = static_cast<FrozenFrame*>(user);
    if (!ff || !ff->bmp || x2 <= x1 || y2 <= y1) return nullptr;
    if (x1 < ff->x1 || y1 < ff->y1 || x2 > ff->x2 || y2 > ff->y2) {
        ++ff->outOfRange;
        return nullptr;
    }
    return BlitSubRect32(ff->bmp, x1 - ff->x1, y1 - ff->y1, x2 - x1, y2 - y1);
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

// ★批 D（docs §47）：那条「已有网页控件树 ⇒ 拦 Enter（要求走 searchOnPage）」的静默拒绝
//   已删。本用例反转钉新不变式：**Enter 一律照发**，只在回执里补一句事实；
//   「刚输入的是 URL」这件事现在只影响**要不要补那句事实**（地址栏导航是正常手法）。
void CaseEnterAfterUrlInput() {
    ResetAiActionSessionState(961);

    AiNotePageKind(L"dom");
    SetAiForegroundBrowserOverrideForTest(1);   // 网页上下文的判断要生效
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) { return std::wstring(L"已执行"); };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    // 直接 Enter：照发 + 标注「前台是网页」
    const std::wstring bare = CallNamedTool(tools, L"keyClick", L"{\"keyText\":\"Enter\"}");
    const bool bareOk = bare.rfind(L"[错误]", 0) != 0
        && bare.find(L"[事实] 前台是网页") != std::wstring::npos;
    // 输入像 URL 的正文后再 Enter：照发，且**不**补那句事实（urlNavPending）
    const std::wstring typed = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"edge://history\",\"clearFirst\":true}");
    const std::wstring afterUrl = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"Enter\"}");
    const bool urlNav = afterUrl.rfind(L"[错误]", 0) != 0
        && afterUrl.find(L"[事实] 前台是网页") == std::wstring::npos;
    // 输入普通搜索词后再 Enter：照发 + 标注（模型自己看得见焦点在哪）
    const std::wstring typedWord = CallNamedTool(tools, L"quickInput",
        L"{\"inputText\":\"\u5386\u53f2\u8bb0\u5f55\",\"clearFirst\":true}");
    const std::wstring afterWord = CallNamedTool(tools, L"keyClick",
        L"{\"keyText\":\"Enter\"}");
    const bool wordOk = afterWord.rfind(L"[错误]", 0) != 0
        && afterWord.find(L"[事实] 前台是网页") != std::wstring::npos;
    const bool ok = bareOk && urlNav && wordOk;
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

    int executed = 0;
    AiActionHostHooks hooks;
    hooks.onExecuteActions = [&executed](const std::wstring& /*json*/) {
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

// ── computer-use 兼容入口：computer 别名必须映射到既有动作，且不绕过守卫 ────
void CaseComputerTool() {
    ResetAiActionSessionState(989);

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

// ══════════════════════════════════════════════════════════════════
// 本地判断表（src/ai_decide.h）—— System One 形态的逐格断言
// ══════════════════════════════════════════════════════════════════

/// 判断一：视觉闸。**逐行**钉死（表驱动就是为了这个：每行一条用例，
/// 加判据时必须一起来这里补一行，否则 decide_table 会红）。
void CaseDecideTable() {
    bool ok = true;
    std::wstring detail;

    auto checkGate = [&](const wchar_t* tag, AiDecideSignals s,
                         AiVisionGate wantVerdict, bool wantDeny) {
        AiDecisionRecord rec;
        const AiVisionGate got = AiDecideVisionGate(s, &rec);
        const bool verdictOk = (got == wantVerdict);
        const bool denyOk = ((got == AiVisionGate::DenyBusyFallback) == wantDeny);
        // 判断行必须带理由 —— 空 why 等于没做诊断
        const bool whyOk = !rec.why.empty();
        const bool confOk = rec.confidence >= 0.0 && rec.confidence <= 1.0;
        if (!(verdictOk && denyOk && whyOk && confOk)) {
            ok = false;
            detail += std::wstring(L" ") + tag;
        }
    };

    // ── 行①②页型类硬证据 ────────────────────────────────────────────
    AiDecideSignals dom;   dom.pageKind = L"dom";   dom.foregroundBrowserClass = true;
    checkGate(L"dom", dom, AiVisionGate::Allow, false);
    AiDecideSignals mixed; mixed.pageKind = L"mixed"; mixed.foregroundBrowserClass = true;
    checkGate(L"mixed", mixed, AiVisionGate::Allow, false);
    AiDecideSignals canvas; canvas.pageKind = L"canvas"; canvas.foregroundBrowserClass = true;
    checkGate(L"canvas", canvas, AiVisionGate::Allow, false);
    // 判断一/二的共同根因：**画布页 + 高动态也必须放行**（旧实现 100% 拦死游戏）
    AiDecideSignals canvasBusy = canvas;
    canvasBusy.busyCoverage = 0.9; canvasBusy.settleStillChanging = true;
    checkGate(L"canvas_busy", canvasBusy, AiVisionGate::Allow, false);

    // ── 行③网页会话 ─────────────────────────────────────────────────
    AiDecideSignals web; web.foregroundBrowserClass = true; web.webSessionActive = true;
    checkGate(L"web_session", web, AiVisionGate::Allow, false);

    // ── 行④非浏览器前台 = 没有网页树可退（桌面游戏/自绘/模拟器）────────
    AiDecideSignals deskGame;              // foregroundBrowserClass 保持 false
    deskGame.busyCoverage = 0.9; deskGame.settleStillChanging = true;
    deskGame.foregroundSelfDrawn = true;
    checkGate(L"desktop_game", deskGame, AiVisionGate::Allow, false);

    // ── 行⑤浏览器前台 + 页型未知 + 高动态 → 拦 ───────────────────────
    AiDecideSignals browserBusy;
    browserBusy.foregroundBrowserClass = true;
    browserBusy.busyCoverage = 0.42; browserBusy.settleStillChanging = true;
    checkGate(L"browser_busy", browserBusy, AiVisionGate::DenyBusyFallback, true);
    // 边界：恰好 0.10 算「高动态」（>= 而非 >）
    AiDecideSignals atThreshold;
    atThreshold.foregroundBrowserClass = true; atThreshold.busyCoverage = 0.10;
    checkGate(L"browser_at_0.10", atThreshold, AiVisionGate::DenyBusyFallback, true);
    // 边界：0.04 + 仍在变 → 也拦（settle 未稳定时门槛更低）
    AiDecideSignals changing;
    changing.foregroundBrowserClass = true;
    changing.busyCoverage = 0.04; changing.settleStillChanging = true;
    checkGate(L"browser_0.04_changing", changing, AiVisionGate::DenyBusyFallback, true);
    // 边界：0.04 但已稳定、页型未知 → 放行（门槛是两段式的，别把 0.04 当全局线）
    AiDecideSignals stable004;
    stable004.foregroundBrowserClass = true;
    stable004.busyCoverage = 0.04; stable004.settleStillChanging = false;
    checkGate(L"browser_0.04_stable", stable004, AiVisionGate::Allow, false);

    // ── 行⑥浏览器前台 + 页型未知 + 画面安静 → 放行（兜底行，不许落到「未覆盖」）──
    AiDecideSignals browserQuiet;
    browserQuiet.foregroundBrowserClass = true; browserQuiet.busyCoverage = -1.0;
    AiDecisionRecord quietRec;
    const AiVisionGate quiet = AiDecideVisionGate(browserQuiet, &quietRec);
    const bool coveredByRow6 = quiet == AiVisionGate::Allow
        && quietRec.why.find(L"未覆盖") == std::wstring::npos;
    ok = ok && coveredByRow6;
    if (!coveredByRow6) detail += L" row6";

    // 没有任何信号（还没观察过）也必须给出完整结论，不许空 why
    AiDecisionRecord emptyRec;
    const AiVisionGate emptyGate = AiDecideVisionGate(AiDecideSignals{}, &emptyRec);
    const bool emptyOk = !emptyRec.why.empty()
        && emptyRec.why.find(L"未覆盖") == std::wstring::npos;
    ok = ok && emptyOk;
    if (!emptyOk) detail += L" empty";
    (void)emptyGate;

    // ── visionIsOnlyWay：只有画布/非浏览器前台的「放行」才置位 ──────────
    AiDecisionRecord owCanvas, owDom;
    AiDecideVisionGate(canvasBusy, &owCanvas);   // 画布 → 唯一手段
    AiDecideVisionGate(dom, &owDom);             // dom 页 → 有树可退
    const bool onlyWayOk = owCanvas.visionIsOnlyWay && !owDom.visionIsOnlyWay;
    ok = ok && onlyWayOk;
    if (!onlyWayOk) detail += L" only_way";

    // ── 判断二：游戏前台逐行 ─────────────────────────────────────────
    auto checkGame = [&](const wchar_t* tag, AiDecideSignals s,
                         AiGameForeground want, bool wantGame) {
        AiDecisionRecord rec;
        const AiGameForeground got = AiDecideGameForeground(s, &rec);
        const bool verdictOk = (got == want);
        const bool boolOk = ((got != AiGameForeground::NotGame) == wantGame);
        const bool whyOk = !rec.why.empty();
        if (!(verdictOk && boolOk && whyOk)) {
            ok = false;
            detail += std::wstring(L" game:") + tag;
        }
    };
    checkGame(L"canvas", canvasBusy, AiGameForeground::GameByCanvasPage, true);
    AiDecideSignals browserHighMotion;
    browserHighMotion.foregroundBrowserClass = true;
    browserHighMotion.busyCoverage = 0.5;   // 浏览器里有 DOM 可退 → 再动也不算「只能靠视觉」
    checkGame(L"browser", browserHighMotion, AiGameForeground::NotGame, false);
    AiDecideSignals sheet;
    sheet.foregroundSpreadsheet = true; sheet.busyCoverage = 0.5;
    checkGame(L"spreadsheet", sheet, AiGameForeground::NotGame, false);
    checkGame(L"self_drawn", deskGame, AiGameForeground::GameByNoTree, true);
    // ★主判据优先于动态覆盖率：自绘画面前台**静止**也必须是游戏
    //   （第十三份日志：开局静止覆盖率 <3%，旧实现前 4 轮都没认出来）
    AiDecideSignals stillGame;
    stillGame.foregroundSelfDrawn = true; stillGame.busyCoverage = 0.01;
    checkGame(L"self_drawn_still", stillGame, AiGameForeground::GameByNoTree, true);
    AiDecideSignals motionOnly;
    motionOnly.busyCoverage = 0.05;
    checkGame(L"motion_only", motionOnly, AiGameForeground::GameByMotion, true);
    // 有控件树 + 画面安静 → 不是游戏（「静态桌面软件别误判成游戏」的守卫）
    AiDecideSignals quietDesktop;
    quietDesktop.busyCoverage = 0.01;
    checkGame(L"quiet_desktop", quietDesktop, AiGameForeground::NotGame, false);
    // 0.03 是「仅动态」那条线的边界
    AiDecideSignals atGameLine;
    atGameLine.busyCoverage = 0.03;
    checkGame(L"at_0.03", atGameLine, AiGameForeground::GameByMotion, true);

    // ── 判断三：附帧 ────────────────────────────────────────────────
    AiDecisionRecord r1, r2, r3;
    const bool attachOk =
        AiDecideAttachObserveFrame(false, true, &r1) == AiAttachFrame::NoModelCannotSee
        && AiDecideAttachObserveFrame(true, false, &r2) == AiAttachFrame::NoNoImage
        && AiDecideAttachObserveFrame(true, true, &r3) == AiAttachFrame::YesFrame
        && !r1.why.empty() && !r2.why.empty() && !r3.why.empty();
    ok = ok && attachOk;
    if (!attachOk) detail += L" attach";

    // ── 置信度三档边界 ──────────────────────────────────────────────
    const bool tierOk =
        AiConfidenceTier(0.0) == AiConfTier::Low
        && AiConfidenceTier(kAiDecideConfLow - 0.001) == AiConfTier::Low
        && AiConfidenceTier(kAiDecideConfLow) == AiConfTier::Medium
        && AiConfidenceTier(kAiDecideConfHigh - 0.001) == AiConfTier::Medium
        && AiConfidenceTier(kAiDecideConfHigh) == AiConfTier::High
        && AiConfidenceTier(1.0) == AiConfTier::High;
    ok = ok && tierOk;
    if (!tierOk) detail += L" tier";

    // ── 诊断行格式：必须同时含判决名、置信度、理由（少一样就没法排查）──
    AiDecisionRecord fmt;
    fmt.confidence = 0.55;
    fmt.why = L"测试理由";
    const std::wstring line = FormatAiDecision(L"vision_gate",
        AiVisionGateName(AiVisionGate::DenyBusyFallback), fmt);
    const bool fmtOk = line.find(L"vision_gate") != std::wstring::npos
        && line.find(L"deny_busy_fallback") != std::wstring::npos
        && line.find(L"0.55") != std::wstring::npos
        && line.find(L"测试理由") != std::wstring::npos;
    ok = ok && fmtOk;
    if (!fmtOk) detail += L" format";

    // 判决名不得为空/问号（漏了 switch 分支会静默退化成 "?"）
    const bool namesOk = std::wstring(AiVisionGateName(AiVisionGate::Allow)) != L"?"
        && std::wstring(AiGameForegroundName(AiGameForeground::GameByNoTree)) != L"?"
        && std::wstring(AiAttachFrameName(AiAttachFrame::YesFrame)) != L"?";
    ok = ok && namesOk;
    if (!namesOk) detail += L" names";

    // ── 输入信号序列化（离线影子测试的解析契约，见 docs §23）──────────────
    // 只记「判了什么」无法离线复算，所以必须把信号也落进日志；
    // 这组断言把字段名/顺序/取值格式钉死，改格式时解析方（python）会一起红。
    AiDecideSignals sigOut;
    sigOut.pageKind = L"mixed";
    sigOut.foregroundBrowserClass = true;
    sigOut.foregroundSelfDrawn = false;
    sigOut.foregroundSpreadsheet = false;
    sigOut.webSessionActive = true;
    sigOut.domHooksAvailable = true;
    sigOut.busyCoverage = 0.4213;
    sigOut.settleStillChanging = true;
    const std::wstring sigLine = FormatAiSignals(sigOut);
    const bool sigOk = sigLine ==
        L"sig page=mixed br=1 self=0 sheet=0 web=1 hooks=1 busy=0.421 changing=1";
    ok = ok && sigOk;
    if (!sigOk) { detail += L" sig_format["; detail += sigLine; detail += L"]"; }

    // 没观察过页型时写 `-`（要能跟「页型是空串」区分开）
    AiDecideSignals sigCold;
    sigCold.busyCoverage = -1.0;
    const std::wstring coldLine = FormatAiSignals(sigCold);
    const bool coldOk = coldLine ==
        L"sig page=- br=0 self=0 sheet=0 web=0 hooks=0 busy=-1.000 changing=0";
    ok = ok && coldOk;
    if (!coldOk) { detail += L" sig_cold["; detail += coldLine; detail += L"]"; }

    // ── 「网关回了空体」怎么处置（P0-2）──────────────────────────────────
    // 官方《Thinking Mode》写明：`max_tokens` 不够时全部预算被推理吃光 ⇒ `content` 为空、
    // `finish_reason=length`，并明确说这**不是** Thinking 失败。旧实现把它当致命 API 错误
    // ⇒ 整个宏当场结束（用户看到「卡在那里然后自己停了」）。
    // 逐格钉死：这类判据一旦有格子走不到，就会退化成「静默不处置」。
    {
        const auto e1 = AiDecideEmptyResponseRecovery(false, true, true, 0);    // 非动作作用域
        const auto e2 = AiDecideEmptyResponseRecovery(true, true, false, 2, 2); // 兜底已满
        const auto e3 = AiDecideEmptyResponseRecovery(true, false, true, 0);    // 被收束/截断
        const auto e4 = AiDecideEmptyResponseRecovery(true, true, false, 0);    // 开着思考
        const auto e5 = AiDecideEmptyResponseRecovery(true, false, false, 0);   // 都没开 ⇒ 不兜
        const auto e6 = AiDecideEmptyResponseRecovery(true, true, true, 2, 2);  // 有界优先
        const bool emptyOk = e1.action == AiEmptyResponseAction::GiveUp
            && e2.action == AiEmptyResponseAction::GiveUp
            && e3.action == AiEmptyResponseAction::NudgeAndRetry
            && e4.action == AiEmptyResponseAction::NudgeAndRetry
            && e5.action == AiEmptyResponseAction::GiveUp
            && e6.action == AiEmptyResponseAction::GiveUp
            && !e1.why.empty() && !e5.why.empty();
        ok = ok && emptyOk;
        if (!emptyOk) detail += L" empty_response_recovery_cells";
    }

    Emit(L"decide_table", ok, detail.c_str());
}

/// 旧布尔封装 vs 判断表：**重构不许改行为**。用真实会话状态跑，
/// 断言「封装返回值」与「同一份宿主信号喂给判断表的判决」逐组一致。
/// 这是第 1 刀的安全网 —— 判据搬家时最容易出的事就是顺手改了行为。
void CaseDecideWrapperEquivalence() {
    bool ok = true;
    std::wstring detail;

    auto group = [&](const wchar_t* tag, int browserOverride, int sheetOverride,
                     const wchar_t* pageKind, double busy, bool changing) {
        ResetAiActionSessionState(880001);
        SetAiForegroundBrowserOverrideForTest(browserOverride);
        SetAiSpreadsheetForegroundOverrideForTest(sheetOverride);
        if (pageKind && pageKind[0]) AiNotePageKind(pageKind);
        NoteAiActionUiBusy(busy, changing);

        // 同一份宿主信号：封装内部取的就是它（CollectAiDecideSignals 已导出给自检用）
        const AiDecideSignals sig = CollectAiDecideSignals();

        AiDecisionRecord vrec;
        const AiVisionGate vgate = AiDecideVisionGate(sig, &vrec);
        const bool wrapperDeny = AiActionUiTooBusyForVisionLocate();
        const bool visionAgree = wrapperDeny == (vgate == AiVisionGate::DenyBusyFallback);

        AiDecisionRecord grec;
        const AiGameForeground gv = AiDecideGameForeground(sig, &grec);
        const bool wrapperGame = AiActionGameForegroundLikely();
        const bool gameAgree = wrapperGame == (gv != AiGameForeground::NotGame);

        if (!(visionAgree && gameAgree)) {
            ok = false;
            detail += L" ";
            detail += tag;
        }
    };

    // ① 桌面游戏：非浏览器 + 高动态 → 视觉放行 + 认得出游戏
    group(L"desktop_game", -1, 0, L"", 0.42, true);
    // ② 画布页：浏览器前台 + canvas + 高动态 → 视觉仍放行
    group(L"canvas_page", 1, 0, L"canvas", 0.42, true);
    // ③ 浏览器 + 页型未知 + 高动态 → 视觉被拦，且不算游戏
    group(L"browser_busy", 1, 0, L"", 0.42, true);
    // ④ 浏览器 + dom 页 + 高动态 → 视觉放行（有树可点）
    group(L"browser_dom", 1, 0, L"dom", 0.42, true);
    // ⑤ 表格软件 → 不算游戏
    group(L"spreadsheet", -1, 1, L"", 0.42, true);
    // ⑥ 什么都没观察过 → 仍要给出稳定结论
    group(L"cold", 0, 0, L"", -1.0, false);

    SetAiSpreadsheetForegroundOverrideForTest(0);

    ResetAiActionSessionState(880002);
    Emit(L"decide_wrapper_equivalence", ok, detail.c_str());
}

/// 诊断出口：判断必须留痕，且能按动作清空（不然上一次动作的结论会误导排查）
void CaseDecideDiagnosticLog() {
    bool ok = true;
    std::wstring detail;

    static std::vector<std::wstring> captured;
    captured.clear();
    SetAiDecisionLogSink(+[](const std::wstring& line) { captured.push_back(line); });

    ClearAiDecisionLogs();
    const bool emptyAtStart = RecentAiDecisionLogs().empty() && captured.empty();

    // 走一次真实封装 → 必须产生诊断行，并且 sink 收得到
    ResetAiActionSessionState(880003);

    SetAiForegroundBrowserOverrideForTest(1);
    NoteAiActionUiBusy(0.42, true);           // 浏览器 + 高动态 → 应判 deny
    const bool deny = AiActionUiTooBusyForVisionLocate();
    const auto logs = RecentAiDecisionLogs();
    const bool logged = !logs.empty() && !captured.empty();
    const bool hasVerdict = logged
        && logs.back().find(L"vision_gate") != std::wstring::npos
        && logs.back().find(L"deny_busy_fallback") != std::wstring::npos
        && logs.back().find(L"conf=") != std::wstring::npos;
    const bool flushed = !captured.empty()
        && captured.back().find(L"vision_gate") != std::wstring::npos;

    // 清空必须真的清掉（每个 AI 动作开始时调用）
    ClearAiDecisionLogs();
    const bool cleared = RecentAiDecisionLogs().empty();

    // 附帧判断也要留痕
    ClearAiDecisionLogs();
    const bool attach = ShouldAttachObserveImageToPlanner(L"gpt-4o", true);
    const bool attachLogged = !RecentAiDecisionLogs().empty()
        && RecentAiDecisionLogs().back().find(L"attach_observe_frame") != std::wstring::npos;

    ok = ok && emptyAtStart && deny && logged && hasVerdict && flushed && cleared
        && attach && attachLogged;
    if (!emptyAtStart) detail += L" dirty_start";
    if (!deny) detail += L" deny";
    if (!logged) detail += L" not_logged";
    if (!hasVerdict) detail += L" no_verdict";
    if (!flushed) detail += L" sink";
    if (!cleared) detail += L" clear";
    if (!attach || !attachLogged) detail += L" attach_log";

    SetAiDecisionLogSink(nullptr);
    ClearAiDecisionLogs();

    ResetAiActionSessionState(880004);
    Emit(L"decide_diagnostic_log", ok, detail.c_str());
}

/// ★批 D（docs §47 / D6④）：原 `vision_gate_tier_hint` 反转成 `vision_gate_trace_only`。
/// 旧不变式「置信度 → 往对话里塞提示」整条已删（连函数一起）：引擎不再往对话/回执注入
/// 任何建议。新不变式：**判断照做、留痕照记，但动作路径上既不拦也不劝** ——
/// 「视觉是唯一手段」时不再有「改用控件树」的错指引（不注入就不会注入错）。
void CaseVisionGateTraceOnly() {
    bool ok = true;
    std::wstring detail;

    // ── 判断表侧（保留）：判决 + 置信度语义 ─────────────────────────────
    // 表格语义一致性：High 档 = 「有硬证据」。
    // 这条守卫防的是初稿那个错：兜底放行行写了 0.70，正好压 High 边界 →
    // 旧的三档路由会认为「有硬证据」而闭嘴，与「兜底、无证据」的表语义自相矛盾。
    AiDecisionRecord rowHard, rowFallback;
    AiDecideSignals sigDom;   sigDom.pageKind = L"dom"; sigDom.foregroundBrowserClass = true;
    AiDecideVisionGate(sigDom, &rowHard);                 // 行①：页型硬证据
    AiDecideSignals sigQuiet; sigQuiet.foregroundBrowserClass = true;  // 行⑥：兜底
    AiDecideVisionGate(sigQuiet, &rowFallback);
    const bool tierSemanticsOk =
        AiConfidenceTier(rowHard.confidence) == AiConfTier::High
        && AiConfidenceTier(rowFallback.confidence) != AiConfTier::High;
    ok = ok && tierSemanticsOk;
    if (!tierSemanticsOk) detail += L" tier_semantics";

    // ── settle 短节拍的采信门槛（kAiGameConfDecisive）───────────────────
    // 短节拍会**跳掉整段 UI settle**，所以只有结构性证据才配用它：
    // 「只有画面在动」是视频/动画广告也有的特征，不能拿它跳。
    auto decisiveOf = [](AiGameForeground v, double conf) {
        AiGameForegroundDecision d;
        d.verdict = v;
        d.record.confidence = conf;
        d.record.why = L"测试";
        return AiGameForegroundDecisionIsDecisive(d);
    };
    const bool decisiveOk =
        decisiveOf(AiGameForeground::GameByNoTree, 0.88)          // 无控件树：够硬
        && decisiveOf(AiGameForeground::GameByCanvasPage, 0.90)   // 画布页：够硬
        && !decisiveOf(AiGameForeground::GameByMotion, 0.55)      // 只有画面在动：不够
        && !decisiveOf(AiGameForeground::GameByMotion, 0.90)      // 抬分也不行：判决行不对
        && !decisiveOf(AiGameForeground::NotGame, 0.85)           // 不是游戏
        && !decisiveOf(AiGameForeground::GameByNoTree, 0.79);     // 低于门槛
    ok = ok && decisiveOk;
    if (!decisiveOk) detail += L" decisive";

    // 门槛必须落在「疑似」与「结构性证据」两行之间，否则这个常量就没意义了
    const bool gateRangeOk = kAiGameConfDecisive > 0.55
        && kAiGameConfDecisive <= 0.88;
    ok = ok && gateRangeOk;
    if (!gateRangeOk) detail += L" decisive_range";

    // ── 会话侧：动作路径上**既不拦也不劝**（批 D 的核心不变式）──────────
    AiActionHostHooks hooks;
    hooks.onLocateAndClick = [](const std::wstring&, int, const std::wstring&, int, int) {
        return std::wstring(L"已点击 屏幕(100,200)");
    };

    // 浏览器前台 + 页型未知 + 画面安静 → 旧实现在这里会补一句「把握一般…改走控件树」
    ResetAiActionSessionState(880005);

    SetAiForegroundBrowserOverrideForTest(1);
    NoteAiActionUiBusy(0.01, false);
    const auto toolsQuiet = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring quiet = CallNamedTool(toolsQuiet, L"locateAndClick",
        L"{\"target\":\"login\"}");
    const bool mediaQuietOk = quiet.find(L"已点击") != std::wstring::npos
        && quiet.find(L"把握一般") == std::wstring::npos
        && quiet.find(L"改用控件树") == std::wstring::npos
        && quiet.find(L"observePage") == std::wstring::npos;

    // 高动态 → 旧实现在这里**直接拒绝**识图定位；现在必须照点
    ResetAiActionSessionState(880006);

    SetAiForegroundBrowserOverrideForTest(1);
    NoteAiActionUiBusy(0.42, true);
    const auto toolsBusy = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring busy = CallNamedTool(toolsBusy, L"locateAndClick",
        L"{\"target\":\"login\"}");
    const bool busyNotDenied = busy.find(L"动态干扰过大") == std::wstring::npos
        && busy.find(L"已点击") != std::wstring::npos;

    // 桌面游戏（非浏览器前台）高动态 → 同样照点，且**不许**出现
    //   「改用控件树 / observePage」这类提示（图上没有树可退）
    ResetAiActionSessionState(880007);

    SetAiForegroundBrowserOverrideForTest(-1);
    NoteAiActionUiBusy(0.42, true);
    const auto toolsGame = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring game = CallNamedTool(toolsGame, L"locateAndClick",
        L"{\"target\":\"zombie\"}");
    const bool gameNotDenied = game.find(L"动态干扰过大") == std::wstring::npos
        && game.find(L"已点击") != std::wstring::npos;
    const bool gameNoTreeNudge = game.find(L"改用控件树") == std::wstring::npos
        && game.find(L"先 observePage") == std::wstring::npos;

    ok = ok && mediaQuietOk && busyNotDenied && gameNotDenied && gameNoTreeNudge;
    if (!mediaQuietOk) detail += L" quiet";
    if (!busyNotDenied) detail += L" busy_denied";
    if (!gameNotDenied) detail += L" game_denied";
    if (!gameNoTreeNudge) detail += L" game_tree_nudge";

    // 会话侧：薄封装要把「判决 + 依据」一起记下来，settle 节拍才不用重算
    // （重算时桌面状态可能已变，会得到与记录不一致的结论）。
    ResetAiActionSessionState(880009);

    SetAiForegroundBrowserOverrideForTest(-1);
    NoteAiActionUiBusy(0.42, true);
    // ★必须用「画布页」这条**结构性证据**把判决钉死，不能靠真实前台窗口。
    //   这里曾经随环境随机红（实测同一二进制 5 次里红 1 次）：
    //   `IsDecisive` 只认 GameByNoTree / GameByCanvasPage（conf ≥ 0.80），
    //   而 GameByNoTree 来自 `ForegroundWindowLooksSelfDrawn()` —— **读真实前台窗口**
    //   的子窗口数（≤2 才算自绘），无覆盖钩子 ⇒ 前台是谁就决定这条用例红绿；
    //   被前台压到兜底 GameByMotion（conf 0.55）时就不是 decisive。
    //   pageKind 是会话状态、`AiNotePageKind` 可直接写，且 canvas 行**排在**浏览器/
    //   表格/自绘三行之前 ⇒ 完全确定，测的还是同一条「判决 + 依据一起记下来」。
    AiNotePageKind(L"canvas");
    const bool wrapperSaysGame = AiActionGameForegroundLikely();
    const AiGameForegroundDecision recorded = AiLastGameForegroundDecision();
    const bool decisiveRecorded = wrapperSaysGame
        && AiGameForegroundDecisionIsDecisive(recorded)
        && !recorded.record.why.empty();
    ok = ok && decisiveRecorded;
    if (!decisiveRecorded) detail += L" decisive_recorded";

    // ── 文字直点「就地复核」的采信规则（AiOcrProbeAgreesWithIndex）────────
    // 实测踩过：索引 contains 命中「一键全选」→ 复核又读成「键全选」→ 旧实现要求
    // 完全相等 → 永远不通过 → 每次点击白落回一整轮 VLM 识图（100KB+/15~45s）。
    auto tier = [](const wchar_t* line, const wchar_t* want) {
        return AiOcrLabelMatchTier(line, want);
    };
    const bool tierOk2 = tier(L"一键全选", L"一键全选") == 2      // 完全相等
        && tier(L"键全选", L"一键全选") == 1                     // contains（残缺读法）
        && tier(L"选", L"一键全选") == 0                         // 太短，不算
        && tier(L"完全不相干的东西", L"一键全选") == 0;
    ok = ok && tierOk2;
    if (!tierOk2) detail += L" ocr_tier";

    auto agree = [&](const wchar_t* probe, const wchar_t* idxHit,
                     int px, int py, int ix, int iy) {
        return AiOcrProbeAgreesWithIndex(probe, idxHit, L"一键全选", px, py, ix, iy);
    };
    const bool probeOk =
        // ★核心回归：复核只读到「键全选」（比索引更差），但仍是同一标签且位置没动 → 采信
        agree(L"键全选", L"键全选", 2142, 255, 2142, 255)
        // 复核读得更好（完全相等）→ 采信
        && agree(L"一键全选", L"键全选", 2140, 253, 2142, 255)
        // 复核读到了**别的**文字 → 不采信（画面变了/点错地方）
        && !agree(L"主菜单", L"键全选", 2142, 255, 2142, 255)
        // 复核位置漂太远（>24px）→ 不采信
        && !agree(L"键全选", L"键全选", 2142, 300, 2142, 255)
        // 索引那次命中本身就不够好（只有 1 个字）→ 不采信
        && !agree(L"键全选", L"选", 2142, 255, 2142, 255)
        // 允许小幅修正坐标（复核的价值就是把坐标修准）
        && agree(L"键全选", L"键全选", 2155, 268, 2142, 255);
    ok = ok && probeOk;
    if (!probeOk) detail += L" ocr_probe_agree";

    // ── lookupMacroAction 查询词归一 ─────────────────────────────────
    // 实测日志：模型写 lookupMacroAction(section=game) → 下游按裸值比较 →
    // 报「未找到「section=game」」→ 白烧一轮，还会再试一次。
    auto nq = [](const wchar_t* s) { return NormalizeMacroLookupQuery(s); };
    const bool nqOk =
        nq(L"section=game") == L"game"           // ★日志里的原始写法
        && nq(L"game") == L"game"                // 本来就对的不受影响
        && nq(L"type=findImage") == L"findImage"
        && nq(L"section: game") == L"game"
        && nq(L"section＝game") == L"game"       // 全角等号
        && nq(L"  \"game\"  ") == L"game"        // 包裹引号 + 空白
        && nq(L"「game」") == L"game"
        && nq(L"GAME") == L"GAME"                // 值本身不改大小写（下游自己会小写化）
        && nq(L"") == L""
        && nq(L"section=") == L"section=";       // 只有键没有值时别把它吃掉
    ok = ok && nqOk;
    if (!nqOk) {
        detail += L" lookup_norm[";
        detail += nq(L"section=game");
        detail += L"]";
    }


    // ── 文字直点：同屏多段拼接（OCR 把按钮文字拆开时仍要能点中）──────────
    // 实测：左上角「自选僵尸卡牌」OCR 拆成多段，单段既过不了长度比、也过不了
    // 框合理性检查 → 索引里整条没有 → 只能回落识图。拼接后才有机会命中。
    auto mkLine = [](const wchar_t* t, int x1, int y1, int x2, int y2, double cf = 0.9) {
        OcrTextLine l;
        l.text = t; l.x1 = x1; l.y1 = y1; l.x2 = x2; l.y2 = y2; l.confidence = cf;
        return l;
    };
    {
        // 拆成两段、同一行、横向紧邻 → 拼起来等于目标
        std::vector<OcrTextLine> lines = {
            mkLine(L"自选", 100, 200, 160, 240),
            mkLine(L"僵尸卡牌", 165, 200, 340, 240),
        };
        AiOcrDirectHit hit;
        const bool ok1 = AiOcrPickDirectClickTarget(lines, L"自选僵尸卡牌",
            2560, 1440, &hit) && hit.hitText == L"自选僵尸卡牌";
        // 点的是**整颗按钮**的中心（拼接后的框），不是残段的中心
        const bool centerOk = hit.screenX >= 200 && hit.screenX <= 240;

        // 拆成三段也能拼
        std::vector<OcrTextLine> three = {
            mkLine(L"一键", 500, 100, 560, 140),
            mkLine(L"全", 565, 100, 595, 140),
            mkLine(L"选", 600, 100, 630, 140),
        };
        AiOcrDirectHit hit3;
        const bool ok3 = AiOcrPickDirectClickTarget(three, L"一键全选",
            2560, 1440, &hit3) && hit3.hitText == L"一键全选";

        // 不同行（垂直不重叠）不许拼 → 目标拼不出来，必须老实拒绝。
        // ⚠ 分片要选**单段都够不着目标**的：4 字 vs 目标 6 字在长度比 40% 以内，
        //   contains 档本来就会命中，那样测的就不是「拼接」而是「包含匹配」了
        //   （初版用「自选」+「僵尸卡牌」当反例，就是被这条绊到的）。
        std::vector<OcrTextLine> crossRow = {
            mkLine(L"通关", 100, 100, 160, 140),
            mkLine(L"这关", 100, 300, 160, 340),
            mkLine(L"这关", 165, 300, 225, 340),
        };
        AiOcrDirectHit hitX;
        const bool rejectCross = !AiOcrPickDirectClickTarget(crossRow, L"通关这关",
            2560, 1440, &hitX);
        // 同屏**两处**真的都有该文字（两个按钮）→ 仍要判歧义，不许猜
        std::vector<OcrTextLine> twoPlaces = {
            mkLine(L"确认", 100, 200, 180, 240),
            mkLine(L"确认", 900, 800, 980, 840),
        };
        AiOcrDirectHit hitA;
        const bool stillAmbiguous = !AiOcrPickDirectClickTarget(twoPlaces, L"确认",
            2560, 1440, &hitA);

        const bool mergeOk = ok1 && centerOk && ok3 && rejectCross && stillAmbiguous;
        ok = ok && mergeOk;
        if (!mergeOk) {
            detail += L" ocr_merge";
            if (!ok1) detail += L"(split2)";
            if (!centerOk) detail += L"(center)";
            if (!ok3) detail += L"(split3)";
            // 交叉行用例失败时把「到底点到了什么」打出来（否则只能猜）
            if (!rejectCross) {
                detail += L"(crossrow:hit=" + hitX.hitText + L")";
            }
            if (!stillAmbiguous) detail += L"(ambiguity)";
        }
    }


    ResetAiActionSessionState(880008);
    Emit(L"vision_gate_trace_only", ok, detail.c_str());
}

/// 执行后「要不要回传观察帧」的三条守卫（AiDecideNeedObserveAfterExec）。
/// 判错的表现两种：模型对着过期画面继续动手（漏看），或每步白等一帧（多看）。
void CaseAiExecObserveGuards() {
    bool ok = true;
    std::wstring detail;

    auto need = [](bool requested, bool first, const wchar_t* exec, const wchar_t* summary) {
        return AiDecideNeedObserveAfterExec(requested, first,
            exec ? exec : L"", summary ? summary : L"");
    };

    // ① 模型明确要观察 → 必须观察（不受其它条影响）
    const bool req = need(true, false, L"已点击", L"已执行 1 步");
    // ② 模型不要、也不是第一次、结果确定 → 不看（省一帧）
    const bool skip = !need(false, false, L"已点击 屏幕(100,200)", L"已执行 1 步");
    // ③ 结果不确定 → 必须看（settle 无反应 / 灰钮 / UIA 事实）
    const bool uncertainSettle = need(false, false, L"[事实] settle无反应", L"");
    const bool uncertainGray = need(false, false, L"提交钮灰了，不可用", L"");
    const bool uncertainUia = need(false, false, L"", L"[UIA] 目标不可用");
    const bool uncertainNoChange = need(false, false, L"几乎无变化", L"");
    // ④ ★本任务第一次动手 → 强制看（防「抢跑」：模型此时可能一个像素都没看过）
    const bool firstForced = need(false, true, L"已点击 屏幕(1,2)", L"已执行 1 步");
    // ⑤ 但「第一次」只影响第一条：第二次起模型说不要就可以不看
    const bool secondNotForced = !need(false, false, L"已点击 屏幕(1,2)", L"已执行 1 步");

    const bool guardsOk = req && skip && uncertainSettle && uncertainGray && uncertainUia
        && uncertainNoChange && firstForced && secondNotForced;
    ok = ok && guardsOk;
    if (!guardsOk) {
        detail += L" guards[req=" + std::to_wstring(req)
            + L" skip=" + std::to_wstring(skip)
            + L" unstable=" + std::to_wstring(uncertainSettle && uncertainGray
                && uncertainUia && uncertainNoChange)
            + L" first=" + std::to_wstring(firstForced)
            + L" second=" + std::to_wstring(secondNotForced) + L"]";
    }

    // 会话侧：本任务第一次动手真的会带上 OBSERVE 标记（而不是 SKIP）。
    // 用宿主钩子接住 onExecuteActions，避免依赖真实桌面。
    ResetAiActionSessionState(880010);

    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) {
        return std::wstring(L"已执行 1 步");
    };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});
    const std::wstring firstOut = CallNamedTool(tools, L"mouseClick",
        L"{\"x\":100,\"y\":100}");
    const bool firstObserve = firstOut.find(L"[EXECUTED][OBSERVE]") != std::wstring::npos;
    // 第二次显式不要观察 → 应给 SKIP
    const std::wstring secondOut = CallNamedTool(tools, L"mouseClick",
        L"{\"x\":120,\"y\":120,\"observeAfter\":false}");
    const bool secondSkip = secondOut.find(L"[EXECUTED][SKIP_OBSERVE]") != std::wstring::npos;

    ok = ok && firstObserve && secondSkip;
    if (!firstObserve) detail += L" first_observe";
    if (!secondSkip) detail += L" second_skip";

    ResetAiActionSessionState(880011);
    Emit(L"ai_exec_observe_guards", ok, detail.c_str());
}

/// 屏幕文字索引（「可点清单」）：按画面行分组 + 标视觉行序。
/// 实测价值（docs §25.4②）：平铺列表让模型无法把文字和画面上的卡片对上，
/// 于是反复猜「哪个是 600 的卡」；按行分组把「找第几张卡」变成查表。
void CaseOcrTextIndexRows() {
    bool ok = true;
    std::wstring detail;

    auto mk = [](const std::wstring& t, int x1, int y1, int x2, int y2, double cf = 0.9) {
        OcrTextLine l;
        l.text = t; l.x1 = x1; l.y1 = y1; l.x2 = x2; l.y2 = y2; l.confidence = cf;
        return l;
    };

    // 一条卡槽：同一行 4 张卡（图标下各带价格），下面另有一行按钮
    std::vector<OcrTextLine> lines = {
        mk(L"普通僵尸", 200, 60, 250, 90),
        mk(L"50",       205, 95, 235, 115),
        mk(L"路障僵尸", 260, 60, 310, 90),
        mk(L"75",       265, 95, 295, 115),
        mk(L"铁桶僵尸", 320, 60, 370, 90),
        mk(L"600",      325, 95, 355, 115),
        mk(L"一键全选", 900, 60, 1000, 90),
        // 第二视觉行（y 差很大）
        mk(L"上一页",   700, 500, 760, 530),
        mk(L"下一页",   800, 500, 860, 530),
    };

    const auto rows = CollectOcrIndexRows(lines);
    // 卡槽里的「图标名 + 价格」虽然 y 略有差异（60-90 vs 95-115），但垂直重叠不足
    // 半数 —— 它们会被分成两行，这是**刻意**的：价格行单独成行才好读。
    // 这里只断言「明显同一视觉行」的会并到一起、跨行的不会。
    bool oneKeyAll = false;
    bool nextPageTogether = false;
    bool cardRowAndButtonRowSeparate = true;
    for (const auto& row : rows) {
        bool hasKeyAll = false, hasPrev = false, hasNext = false;
        for (const auto& sp : row) {
            if (sp.text == L"一键全选") hasKeyAll = true;
            if (sp.text == L"上一页") hasPrev = true;
            if (sp.text == L"下一页") hasNext = true;
        }
        if (hasKeyAll) oneKeyAll = true;
        if (hasPrev && hasNext) nextPageTogether = true;
        // 「一键全选」(y≈60) 与「上一页」(y≈500) 绝不能在同一行
        if (hasKeyAll && (hasPrev || hasNext)) cardRowAndButtonRowSeparate = false;
    }

    // 文本格式化：含标题、行列结构、坐标口径
    int count = 0;
    const std::wstring idx = FormatOcrTextIndex(lines, 0, 0, 2560, 1440, 1024, 576, &count);
    const bool fmtOk = !idx.empty() && count == static_cast<int>(lines.size())
        && idx.find(L"第1行") != std::wstring::npos
        && idx.find(L"一键全选") != std::wstring::npos
        && idx.find(L",") != std::wstring::npos
        // ★口径必须是 **upload 截图像素**（`mouseClick` 收的那一套）。
        //   旧断言钉的是「屏幕绝对像素」—— 那正是缺陷本身（docs §60.5）。
        && idx.find(L"upload 截图像素") != std::wstring::npos
        && idx.find(L"屏幕绝对像素") == std::wstring::npos;

    // ★★同一轮消息里两个索引必须**同一套坐标**（docs §60.5）：文字索引曾经发屏幕像素、
    //   元素索引发 upload 像素，标题都写「与 mouseClick 同一套」⇒ 模型必然点偏。
    //   这里钉**换算值本身**（§45 教训③：断言要断坐标值，不是「有没有那句话」）。
    {
        std::vector<OcrTextLine> one{ mk(L"确认", 1240, 700, 1320, 740) };  // 中心 (1280,720)
        int n1 = 0, n2 = 0;
        const std::wstring up = FormatOcrTextIndex(one, 0, 0, 2560, 1440, 1024, 576, &n1);
        const bool uploadOk = n1 == 1
            && up.find(L"(512,288)") != std::wstring::npos        // 1280,720 → upload
            && up.find(L"upload 截图像素") != std::wstring::npos
            && up.find(L"屏幕绝对像素") == std::wstring::npos;
        ok = ok && uploadOk;
        if (!uploadOk) detail += L" ocr_index_coord_space(upload)";
        // 没有帧尺寸时必须**如实降级**并明说「不能填进 mouseClick」——不许冒充同一套
        const std::wstring deg = FormatOcrTextIndex(one, 0, 0, 2560, 1440, 0, 0, &n2);
        const bool degradeOk = n2 == 1
            && deg.find(L"(1280,720)") != std::wstring::npos
            && deg.find(L"屏幕绝对像素") != std::wstring::npos
            && deg.find(L"不能**直接填进 mouseClick") != std::wstring::npos;
        ok = ok && degradeOk;
        if (!degradeOk) detail += L" ocr_index_coord_space(degrade)";
    }

    // 空输入 → 空串（调用方据此不注入空索引）
    int zeroCount = 0;
    const bool emptyOk = FormatOcrTextIndex({}, 0, 0, 100, 100, 0, 0, &zeroCount).empty()
        && zeroCount == 0;

    // 低置信度与超短文本要挡掉；纯数字限量（防满屏价格淹没文字按钮）
    std::vector<OcrTextLine> noisy;
    for (int i = 0; i < 12; ++i) {
        noisy.push_back(mk(L"123", 100 + i * 40, 100, 130 + i * 40, 120));   // 纯数字 ×12
    }
    noisy.push_back(mk(L"x", 10, 10, 20, 20));                               // 太短
    noisy.push_back(mk(L"低置信", 10, 50, 80, 70, 0.30));                    // 置信度不足
    noisy.push_back(mk(L"确认", 10, 200, 60, 220));                          // 该留下
    int noisyCount = 0;
    const std::wstring noisyIdx = FormatOcrTextIndex(noisy, 0, 0, 2560, 1440, 1024, 576,
        &noisyCount);
    // ★契约换了（docs §44）：旧断言是 `noisyCount <= 7`（6 数字 + 1 文字）——那条**按 OCR 顺序
    //   截断**的写法让"哪些价格留下"变成抽签，14 张卡的界面必然缺价签。现在钉真正想保证的事：
    //   ① **文字条目永不缺席**（满屏数字也挤不掉「确认」——这才是原来那条闸的本意）；
    //   ② 孤立数字仍然限量（不能无限灌）。
    const bool filterOk = noisyIdx.find(L"确认") != std::wstring::npos
        && noisyIdx.find(L"低置信") == std::wstring::npos
        && noisyCount >= 2 && noisyCount <= 12 + 1;
    if (noisyIdx.find(L"确认") == std::wstring::npos) detail += L" named_entry_crowded_out";
    if (noisyIdx.find(L"低置信") != std::wstring::npos) detail += L" low_conf_kept";
    if (noisyCount > 12 + 1) detail += L" numeric_unbounded";

    // ★★用户那台机器的形状（docs §44）：**14 张卡 = 14 个卡名 + 14 个价签**。
    //   旧规则（纯数字全局只留 6 条）下，模型按价格找卡必然「索引里没有 600/800/9999」，
    //   只好拿一个裸数字去问 VLM ⇒ 每次给的框都不一样 ⇒ 点空 ⇒ 死点表/近点重复连环触发
    //   ⇒ 改用不带目标语义的手算坐标 mouseClick ⇒ 在 toggle 界面上把**已选中的卡点掉**
    //   （用户：「把选了的卡收回去又重新选」）。
    std::vector<OcrTextLine> cards;
    const wchar_t* kNames[14] = { L"普通僵尸", L"路障僵尸", L"铁桶僵尸", L"撑杆僵尸",
        L"舞王僵尸", L"伴舞僵尸", L"鸭子僵尸", L"潜水僵尸", L"冰车僵尸", L"雪橇僵尸",
        L"海豚僵尸", L"跳跳僵尸", L"玩偶匣僵尸", L"气球僵尸" };
    for (int i = 0; i < 14; ++i) {
        const int x = 100 + i * 150;
        cards.push_back(mk(kNames[i], x, 60, x + 90, 90));
        cards.push_back(mk(std::to_wstring(50 + i * 25), x + 10, 95, x + 60, 115));  // 价签
    }
    int cardCount = 0;
    const std::wstring cardIdx = FormatOcrTextIndex(cards, 0, 0, 2560, 1440, 1024, 576,
        &cardCount);
    bool allNames = true;
    for (int i = 0; i < 14; ++i) {
        if (cardIdx.find(kNames[i]) == std::wstring::npos) {
            allNames = false;
            detail += L" card_name_missing:" + std::wstring(kNames[i]);
            break;
        }
    }
    // 价签也要在（「按价格/编号选」是文档承诺过的能力）——数价签出现的个数
    int priceKept = 0;
    for (int i = 0; i < 14; ++i) {
        if (cardIdx.find(std::to_wstring(50 + i * 25)) != std::wstring::npos) ++priceKept;
    }
    const bool cardOk = allNames && priceKept >= 14;
    if (priceKept < 14) detail += L" prices(" + std::to_wstring(priceKept) + L"/14)";

    const bool allOk = oneKeyAll && nextPageTogether && cardRowAndButtonRowSeparate
        && fmtOk && emptyOk && filterOk && cardOk;
    ok = ok && allOk;
    if (!oneKeyAll) detail += L" key_all_row";
    if (!nextPageTogether) detail += L" prevnext_row";
    if (!cardRowAndButtonRowSeparate) detail += L" row_merge_too_wide";
    if (!fmtOk) detail += L" format";
    if (!emptyOk) detail += L" empty";
    if (!filterOk) detail += L" filter(" + std::to_wstring(noisyCount) + L")";

    // ★★帧间屏幕文字差分（docs §65）：真实形状 —— 顶栏计数器 `30000` → `29900`。
    //   这正是模型缺的那条证据（"我到底放没放上"），而它**不需要额外识图**。
    {
        const auto mkLine = [](const std::wstring& t, int x, int y) {
            OcrTextLine l{};
            l.text = t; l.confidence = 0.95;
            l.x1 = x; l.y1 = y; l.x2 = x + 40; l.y2 = y + 18;
            return l;
        };
        // ① 计数器变化 → 必须报成「数值 A → B」（而不是笼统的新增 + 消失）
        const std::vector<OcrTextLine> prevF{ mkLine(L"30000", 180, 70), mkLine(L"自选僵尸卡牌", 90, 40) };
        const std::vector<OcrTextLine> curF{ mkLine(L"29900", 180, 70), mkLine(L"自选僵尸卡牌", 90, 40) };
        const std::wstring numDelta = AiDescribeTextIndexDelta(prevF, curF, 6);
        const bool numOk = numDelta.find(L"\"30000\" → \"29900\"") != std::wstring::npos
            && numDelta.find(L"[事实]") != std::wstring::npos;
        ok = ok && numOk;
        if (!numOk) detail += L" delta_numeric(" + numDelta + L")";
        // ② 纯文字增删照报
        const std::vector<OcrTextLine> prevT{ mkLine(L"确定", 10, 500) };
        const std::vector<OcrTextLine> curT{ mkLine(L"已保存", 10, 500) };
        const std::wstring txtDelta = AiDescribeTextIndexDelta(prevT, curT, 6);
        const bool txtOk = txtDelta.find(L"已保存") != std::wstring::npos
            && txtDelta.find(L"确定") != std::wstring::npos;
        ok = ok && txtOk;
        if (!txtOk) detail += L" delta_text(" + txtDelta + L")";
        // ③ 没变 ⇒ **必须返回空**（否则每帧一行"没有变化"的废话，白烧 token）
        const bool quietOk = AiDescribeTextIndexDelta(prevF, prevF, 6).empty();
        ok = ok && quietOk;
        if (!quietOk) detail += L" delta_quiet";
        // ④ 没有上一帧（第一次观察）⇒ 不许编出"变化"
        const bool firstOk = AiDescribeTextIndexDelta({}, curF, 6).empty();
        ok = ok && firstOk;
        if (!firstOk) detail += L" delta_first_frame";
    }

    Emit(L"ocr_text_index_rows", ok, detail.c_str());
}

/// ★「清单要能被执行」是**产清单的工具**的责任（docs §44）。
/// 实测踩坑（第二十五份日志）：模型用 planSpend 的 **costs 简写**（"只知道价格、不想编名字时用它"），
/// 工具**伪造**了「选项3(花费500)」这种像名字的东西，而描述又写着「照它执行、逐个点名单里的卡」
/// ⇒ 模型真的去 `locateAndClick("9999"/"800"/"600"/"500")`。
/// 而价格**三条路全断**：① 不唯一（同一份计划里就有 500×3、300×4）；② 大概率不在 OCR 索引里
/// （纯数字额度，见 `ocr_text_index_rows`）；③ 拿裸数字问 VLM 每次给的框都不同 —— 同一个「600」
/// 三次给出 (1937,1101) → (1866,560) → (1775,585)，相差 500px。
/// ⇒ 点空 → 死点表/近点重复闸连环触发（连**正确**的重定位也开枪）→ 模型改用**不带目标语义**的
/// 手算坐标 `mouseClick`（按目标的闸全认不出来）⇒ 在选卡界面上把**已选中的卡又点掉**
/// ⇒ 用户看到的「把选了的卡收回去又重新选」。
void CasePlanSpendTargetContract() {
    bool ok = true;
    std::wstring detail;

    AiActionHostHooks hooks;
    hooks.onExecuteActions = [](const std::wstring&) { return std::wstring(L"已执行"); };
    const auto tools = BuildAiActionExecuteTools(&hooks, {});

    // ① 只给 costs：回执必须当场说清「这份名单不能用来定位」
    const std::wstring byCosts = CallNamedTool(tools, L"planSpend",
        L"{\"budget\":3000,\"slots\":3,\"distinctOnly\":true,"
        L"\"costs\":[9999,800,600,500,300,275]}");
    const bool warned = byCosts.find(L"不能用来定位") != std::wstring::npos
        && byCosts.find(L"name") != std::wstring::npos;
    ok = ok && warned;
    if (!warned) detail += L" costs_only_not_warned";
    // ② 占位符不许长得像可点目标（旧写法「选项3(花费500)」正是被照单点掉的那个）
    const bool notTargetLike = byCosts.find(L"选项") == std::wstring::npos
        && byCosts.find(L"noname") != std::wstring::npos;
    ok = ok && notTargetLike;
    if (!notTargetLike) detail += L" placeholder_looks_like_target";

    // ③ 给了 name：不许再出现那句警告（无端噪音），且名字要原样回给模型供定位
    const std::wstring byNames = CallNamedTool(tools, L"planSpend",
        L"{\"budget\":3000,\"slots\":2,\"distinctOnly\":true,"
        L"\"items\":[{\"name\":\"铁桶僵尸\",\"cost\":600},"
        L"{\"name\":\"普通僵尸\",\"cost\":50}]}");
    const bool namedOk = byNames.find(L"不能用来定位") == std::wstring::npos
        && byNames.find(L"铁桶僵尸") != std::wstring::npos;
    ok = ok && namedOk;
    if (!namedOk) detail += L" named_warned_or_name_lost";

    Emit(L"plan_spend_target_contract", ok, detail.c_str());
}

/// 思考降档的两个前置机制：AI 动作作用域标记 + 「抑制 N 轮」计数会被消耗。
/// ⚠ 这里**不**断言「游戏前台 ⇒ 关思考」——那条**批 D 已整条删除（docs §47.4）**，
///   而且它本来就依赖真实前台窗口（`foregroundSelfDrawn` 要 EnumChildWindows），
///   自检里测不到，硬测就是假绿。**勿据此重建**：引擎不替模型决定「这一轮要不要想」。
///   ⚠ 作用域标记仍要测：它现在是 §42 两条协议闸的入参（看门狗收束 / 慢轮处置靠它区分
///   「AI 动作执行」与「聊天助手」）。
///   能测也必须测的是：作用域真的按进出打开/关闭，以及计数器没有变成永久闩锁。
void CaseThinkingDowngradePlumbing() {
    bool ok = true;
    std::wstring detail;

    // ① 作用域标记：默认关；进来是开；出去恢复关；嵌套进出平衡。
    const bool outsideFalse = !InAiActionExecScope();
    {
        AiActionExecThinkingScope s1;
        const bool insideTrue = InAiActionExecScope();
        {
            AiActionExecThinkingScope s2;   // 嵌套（嵌套 aiActionExecute 会这样）
            const bool nestedTrue = InAiActionExecScope();
            ok = ok && nestedTrue;
            if (!nestedTrue) detail += L" nested";
        }
        const bool stillInside = InAiActionExecScope();   // 内层析构不该关掉外层
        ok = ok && insideTrue && stillInside;
        if (!insideTrue) detail += L" inside";
        if (!stillInside) detail += L" nested_leak";
    }
    const bool afterTrue = !InAiActionExecScope();
    ok = ok && outsideFalse && afterTrue;
    if (!outsideFalse) detail += L" dirty_start";
    if (!afterTrue) detail += L" scope_leak";

    // ② 「抑制 N 轮」的**计数契约**：抑制 2 轮 ⇒ 消耗 2 次后必须放开。
    //    ⚠ 这条测的是计数器的行为，**测不到 `SendMessage` 里那一次调用是否还在**
    //      （要覆盖调用点得有 HTTP mock；试过把调用点注释掉，自检照样绿 —— 别把它当
    //       那个缺陷的回归网）。它的价值是钉住「N 轮后一定放开」这个契约：
    //      实现若改成只置位不递减、或把轮数放大，这里会红。
    //      调用点的可见防线是代码注释 + 本用例说明，不是断言。
    ResetThinkingSuppressionForTest();          // 从确定状态开始（防用例顺序污染）
    SuppressThinkingForNextRounds(2);
    int requestCount = 0;
    while (ShouldDisableThinking(L"", L"") && requestCount < 8) {
        NoteThinkingRoundConsumed();            // 模拟 SendMessage 每轮的那一次消耗
        ++requestCount;
    }
    const bool released = !ShouldDisableThinking(L"", L"");
    // 2 轮抑制 ⇒ 第 3 次请求应当已放开（多给一点余量，只要求「有限轮内放开」）
    const bool boundedOk = released && requestCount >= 2 && requestCount <= 3;
    ok = ok && boundedOk;
    if (!released) detail += L" latch_stuck";
    else if (requestCount < 2 || requestCount > 3) {
        detail += L" rounds(" + std::to_wstring(requestCount) + L")";
    }
    ResetThinkingSuppressionForTest();

    Emit(L"thinking_downgrade_plumbing", ok, detail.c_str());
}

void CaseFrameClickMarkLifecycle() {
    bool ok = true;
    std::wstring detail;

    const int capX1 = 100, capY1 = 50, capX2 = 100 + 800, capY2 = 50 + 600;
    const int frameW = 800, frameH = 600;

    // ① 没点过 → 不该有任何标注（首帧不能凭空画一个叉）
    ResetAiActionClickMarks();
    {
        AiClickMarkState st;
        st.screenX = -1;
        st.screenY = -1;
        const bool taken = AiClickMarkTake(st);
        ok = ok && !taken && !AiFrameClickMarkDrawnInFrame();
        if (taken) detail += L" phantom_mark";
    }

    // ② 记一击 → 屏幕坐标 → 帧内像素（这是这一整套的地基：偏一点就是「标注整体错位」）
    SetAiActionClickIntent(L"僵尸卡牌");
    MarkLastAiClickScreenPoint(300, 250);
    ok = ok && AiFrameClickMarkDrawnInFrame() == false;   // 还没有帧画过它
    {
        int lx = 0, ly = 0;
        if (!AiClickMarkToFramePoint(300, 250, capX1, capY1, capX2, capY2, frameW, frameH,
                &lx, &ly)) {
            ok = false;
            detail += L" conv_fail";
        } else if (lx != 200 || ly != 200) {
            ok = false;
            detail += L" conv(" + std::to_wstring(lx) + L"," + std::to_wstring(ly) + L")";
        }
    }

    // ③ 标签口径：图上画的和提示词说的必须是同一句话；过长截断
    const std::wstring shortLabel = FormatFrameClickMarkLabel(L"僵尸卡牌");
    ok = ok && shortLabel.find(L"僵尸卡牌") != std::wstring::npos;
    if (shortLabel.find(L"僵尸卡牌") == std::wstring::npos) detail += L" label_missing";
    const std::wstring longDesc(60, L'目');
    const std::wstring longLabel = FormatFrameClickMarkLabel(longDesc);
    ok = ok && longLabel.find(L"…") != std::wstring::npos
        && longLabel.size() < longDesc.size();
    if (longLabel.find(L"…") == std::wstring::npos) detail += L" label_no_ellipsis";
    // 描述未知时也要说人话（引擎侧只标坐标、没意图时不至于画个空标签）
    ok = ok && !FormatFrameClickMarkLabel(L"").empty();
    if (FormatFrameClickMarkLabel(L"").empty()) detail += L" label_empty";

    // ④ 一次点击只标一帧：第二次取同一个落点必须被辞退（否则模型会以为这里被点了很多次）
    {
        AiClickMarkState st;
        st.space = AiClickMarkState::CoordSpace::ScreenPx;
        st.screenX = 300;
        st.screenY = 250;
        st.x = 300;
        st.y = 250;
        st.capX1 = capX1;
        st.capY1 = capY1;
        st.capX2 = capX2;
        st.capY2 = capY2;
        st.frameW = frameW;
        st.frameH = frameH;
        const bool first = AiClickMarkTake(st);
        const bool convOk = first && st.space == AiClickMarkState::CoordSpace::FramePx
            && st.x == 200 && st.y == 200;
        ok = ok && convOk;
        if (!first) detail += L" take_first";
        else if (st.x != 200 || st.y != 200) {
            detail += L" take_coord(" + std::to_wstring(st.x) + L"," + std::to_wstring(st.y)
                + L")";
        }
        st.drawnX = 300;      // 编码方画成功后写的记账
        st.drawnY = 250;
        // 同一落点、坐标已是 FramePx：仍不许再标（去重按**屏幕坐标**判，与坐标系无关）
        st.space = AiClickMarkState::CoordSpace::ScreenPx;
        st.x = 300;
        st.y = 250;
        const bool again = AiClickMarkTake(st);
        ok = ok && !again;
        if (again) detail += L" redraw_same";
    }

    // ⑤ 同一个坐标**再点一次** = 新的一击，必须允许重新标注
    {
        AiClickMarkState st;
        AiClickMarkRecord(st, 300, 250);
        st.capX1 = capX1;
        st.capY1 = capY1;
        st.capX2 = capX2;
        st.capY2 = capY2;
        st.frameW = frameW;
        st.frameH = frameH;
        AiClickMarkTake(st);
        st.drawnX = 300;
        st.drawnY = 250;
        AiClickMarkRecord(st, 300, 250);      // 用户真的又点了一下同一处
        st.capX1 = capX1;
        st.capY1 = capY1;
        st.capX2 = capX2;
        st.capY2 = capY2;
        st.frameW = frameW;
        st.frameH = frameH;
        const bool again = AiClickMarkTake(st);
        ok = ok && again;
        if (!again) detail += L" same_point_not_remarkable";
    }

    // ⑥ 落点在捕获区域外 → 不标（画不出真实位置，画了就是编造）；
    //    刚好在右/下边界也算出界（capX2/capY2 是开区间）
    {
        const std::pair<int, int> outside[] = {
            { capX1 - 1, 250 }, { 300, capY1 - 1 },
            { capX2, 250 },     { 300, capY2 },
        };
        for (const auto& p : outside) {
            AiClickMarkState st;
            AiClickMarkRecord(st, p.first, p.second);
            st.capX1 = capX1;
            st.capY1 = capY1;
            st.capX2 = capX2;
            st.capY2 = capY2;
            st.frameW = frameW;
            st.frameH = frameH;
            const bool taken = AiClickMarkTake(st);
            ok = ok && !taken;
            if (taken) {
                detail += L" oob(" + std::to_wstring(p.first) + L"," + std::to_wstring(p.second)
                    + L")";
            }
        }
        // 区域非法（未初始化）时同样不画
        AiClickMarkState st;
        AiClickMarkRecord(st, 300, 250);
        const bool taken = AiClickMarkTake(st);
        ok = ok && !taken;
        if (taken) detail += L" no_region";
        // 区域合法但帧尺寸小于换算结果 → 出帧，不画
        AiClickMarkState st2;
        AiClickMarkRecord(st2, 300, 250);
        st2.capX1 = capX1;
        st2.capY1 = capY1;
        st2.capX2 = capX2;
        st2.capY2 = capY2;
        st2.frameW = 100;
        st2.frameH = 100;
        const bool taken2 = AiClickMarkTake(st2);
        ok = ok && !taken2;
        if (taken2) detail += L" frame_small";
    }

    // ⑦ 抹掉落点（近点重复点击被拦时）：坐标没了，但「已画过哪一击」的记账要留着 ——
    //    抹的是当前落点，不是已经出现在画面上的那一击
    {
        SetAiActionClickIntent(L"僵尸卡牌");
        MarkLastAiClickScreenPoint(300, 250);
        ClearAiActionClickMark();
        int gx = 0, gy = 0;
        std::wstring gl;
        const bool has = GetAiActionClickMark(&gx, &gy, &gl);
        ok = ok && !has;
        if (has) detail += L" clear_failed";
    }

    // ⑧ 提示词侧的取用口径：标签取的是**屏幕坐标**（帧内换算只影响画面，不影响提示词）
    {
        ResetAiActionClickMarks();
        SetAiActionClickIntent(L"600 卡");
        MarkLastAiClickScreenPoint(640, 480);
        int gx = 0, gy = 0;
        std::wstring gl;
        const bool has = GetAiActionClickMark(&gx, &gy, &gl);
        ok = ok && has && gx == 640 && gy == 480
            && gl == FormatFrameClickMarkLabel(L"600 卡");
        if (!has) detail += L" get_missing";
        else if (gx != 640 || gy != 480) {
            detail += L" get_coord(" + std::to_wstring(gx) + L"," + std::to_wstring(gy) + L")";
        }
        // 同一把尺：引擎侧组帧用的标签与提示词用的是同一个函数（不许两处各拼一套文案）
        ok = ok && CurrentAiActionClickMarkLabel() == FormatFrameClickMarkLabel(L"600 卡");
        if (CurrentAiActionClickMarkLabel() != FormatFrameClickMarkLabel(L"600 卡"))
            detail += L" label_diverged";
        // 按一次 AI 动作复位：上一条任务的落点绝不能被画进本任务第一帧
        ResetAiActionClickMarks();
        ok = ok && !GetAiActionClickMark(&gx, &gy, &gl) && !AiFrameClickMarkDrawnInFrame();
        if (GetAiActionClickMark(&gx, &gy, &gl)) detail += L" reset_leak";
    }

    Emit(L"frame_click_mark_lifecycle", ok, detail.c_str());
}

// 统一「可点元素索引」：把 UIA 控件与 OCR 文字合成一张带编号的表，让模型
// 「所见即所得」——按编号/名字说话，宿主查表拿坐标（0 次识图）。
// 这里钉的是合并/去重/歧义/窗口按钮拒选这几条**判据本身**。
// `zoom`：把一块区域按原始分辨率放大回传（docs §50）。
// 钉三类东西：①纯判据的**边界格**（夹取/反向/太小/帧外/未知帧尺寸，判断表不许有走不到的格子）；
// ②回执**必须写清三件事**（区域、倍率、"图内坐标换回 upload 的公式"）——少一句就是坐标口径漂移；
// ③工具契约（两个入口、缺参如实拒绝、无宿主如实拒绝、成功时必须带 `[[AGENT_IMG:]]`）。
void CaseZoomRegionTool() {
    bool ok = true;
    std::wstring detail;
    AiZoomRect r;
    std::wstring why;
    const bool inFrame = AiZoomClampRect(100, 50, 300, 150, 1024, 576, 8, r, why)
        && r.x1 == 100 && r.y1 == 50 && r.x2 == 300 && r.y2 == 150;
    ok = ok && inFrame;
    if (!inFrame) detail += L" inside_frame";
    // 越界要被**夹到帧内**（不是拒绝）：模型常按上一帧的坐标问，边界处差几像素很常见
    const bool clamped = AiZoomClampRect(-50, -20, 2000, 900, 1024, 576, 8, r, why)
        && r.x1 == 0 && r.y1 == 0 && r.x2 == 1024 && r.y2 == 576;
    ok = ok && clamped;
    if (!clamped) detail += L" clamp_to_frame";
    // 反向坐标（右下在前）要能容忍
    const bool swapped = AiZoomClampRect(300, 150, 100, 50, 1024, 576, 8, r, why)
        && r.x1 == 100 && r.x2 == 300;
    ok = ok && swapped;
    if (!swapped) detail += L" swap_reversed";
    // 太小 / 完全在帧外 / 还不知道帧尺寸 ⇒ 拒绝**且给可读原因**（别静默返回空图）
    const bool tooSmall = !AiZoomClampRect(10, 10, 14, 14, 1024, 576, 8, r, why) && !why.empty();
    const bool outside = !AiZoomClampRect(2000, 2000, 2100, 2100, 1024, 576, 8, r, why) && !why.empty();
    const bool noFrame = !AiZoomClampRect(0, 0, 100, 100, 0, 0, 8, r, why) && !why.empty();
    ok = ok && tooSmall && outside && noFrame;
    if (!(tooSmall && outside && noFrame)) detail += L" reject_cells";
    // ── 分块：**绝不静默裁掉模型要的区域**（docs §53）────────────────────────
    // 旧实现遇到超上限的区域会**居中收窄**：模型 zoom(0,0,1024,80)（整条卡槽）
    // 拿到的是中间 512 宽那一段，它据此下了「卡槽是空的」的结论，白烧两轮。
    {
        int need = 0;
        const std::vector<AiZoomRect> two = PlanAiZoomTiles(
            AiZoomRect{ 0, 0, 1024, 80 }, 2.5, 2.5, kAgentAttachmentMaxLongEdge, 2, &need);
        const bool twoOk = two.size() == 2 && need == 2
            && two[0].x1 == 0 && two[0].x2 == 512 && two[1].x1 == 512 && two[1].x2 == 1024
            && two[0].y1 == 0 && two[0].y2 == 80;
        ok = ok && twoOk;
        if (!twoOk) detail += L" tiles_split_bar";
        // 装得下的区域**不许**分块（一张就够，别白送两张图）
        const std::vector<AiZoomRect> one = PlanAiZoomTiles(
            AiZoomRect{ 140, 180, 600, 470 }, 2.5, 2.5, kAgentAttachmentMaxLongEdge, 2, &need);
        const bool oneOk = one.size() == 1 && need == 1
            && one[0].x1 == 140 && one[0].x2 == 600;
        ok = ok && oneOk;
        if (!oneOk) detail += L" tiles_no_split_when_fits";
        // 整帧（2560×1440 原生）= 4 块 > 2 ⇒ 交回空表 + 老实报 needTiles，让调用方走降采样
        const std::vector<AiZoomRect> none = PlanAiZoomTiles(
            AiZoomRect{ 0, 0, 1024, 576 }, 2.5, 2.5, kAgentAttachmentMaxLongEdge, 2, &need);
        const bool noneOk = none.empty() && need == 4;
        ok = ok && noneOk;
        if (!noneOk) detail += L" tiles_over_cap_needs_downscale";
    }
    // 回执四件事（倍率 ×2.50 = 500 / (300-100)）+ 单张时的换算基准
    const AiZoomRect area{ 100, 50, 300, 150 };
    AiZoomResult single;
    single.ok = true;
    single.requested = area;
    single.tiles.push_back(AiZoomTile{ L"C:\\Temp\\QstZoom\\zoom_1_1.jpg",
        area, 500, 250, 40200 });
    const std::wstring rec = FormatAiZoomReceipt(single);
    const bool recOk = rec.find(L"upload(100,50)-(300,150)") != std::wstring::npos
        && rec.find(L"倍率 ×2.50") != std::wstring::npos
        && rec.find(L"100 + 图x/2.50") != std::wstring::npos
        && rec.find(L"入口＝坐标") != std::wstring::npos
        && rec.find(L"mouseClick") != std::wstring::npos
        && rec.find(L"40 KB") != std::wstring::npos;   // 交付字节数（= 模型拿到的那张）
    ok = ok && recOk;
    if (!recOk) detail += L" receipt_three_facts";
    AiZoomResult byLabel = single;
    byLabel.byTarget = true;
    byLabel.resolvedName = L"自选僵尸卡牌";
    const bool recTOk = FormatAiZoomReceipt(byLabel)
        .find(L"入口＝文字标签「自选僵尸卡牌」") != std::wstring::npos;
    ok = ok && recTOk;
    if (!recTOk) detail += L" receipt_target_entry";
    // ★分块回执：每块各自的区域+倍率+换算基准都要在（拿错块的基准会整体平移）
    {
        AiZoomResult multi;
        multi.ok = true;
        multi.requested = AiZoomRect{ 0, 0, 1024, 80 };
        multi.tiles.push_back(AiZoomTile{ L"a.jpg", AiZoomRect{ 0, 0, 512, 80 }, 1280, 200, 60000 });
        multi.tiles.push_back(AiZoomTile{ L"b.jpg", AiZoomRect{ 512, 0, 1024, 80 }, 1280, 200, 62000 });
        multi.note = L"这块区域一张装不下，已**分成 2 张**给你";
        const std::wstring r2 = FormatAiZoomReceipt(multi);
        const bool multiOk = r2.find(L"分 2 张给你") != std::wstring::npos
            && r2.find(L"第 1/2 张 = upload(0,0)-(512,80)") != std::wstring::npos
            && r2.find(L"第 2/2 张 = upload(512,0)-(1024,80)") != std::wstring::npos
            && r2.find(L"第 1 张 → (0 + 图x/2.50") != std::wstring::npos
            && r2.find(L"第 2 张 → (512 + 图x/2.50") != std::wstring::npos
            && r2.find(L"分成 2 张") != std::wstring::npos;
        ok = ok && multiOk;
        if (!multiOk) detail += L" receipt_multi_tile_facts";
    }
    // ★②「倍率」必须由**宿主给的真值**算（docs §53）：尺寸是交付图自己的尺寸，
    //   回执不再自己按上限猜一遍 ⇒ 缩放/分块怎么变都不可能说谎。
    //   这条钉「回执用的就是宿主报的那两个数」：1538 宽给 525 宽的块 = ×2.93。
    {
        AiZoomResult d;
        d.ok = true;
        d.requested = AiZoomRect{ 175, 20, 700, 95 };
        d.tiles.push_back(AiZoomTile{ L"c.jpg", d.requested, 1538, 175, 300000 });
        const std::wstring recBig = FormatAiZoomReceipt(d);
        const bool delivered = recBig.find(L"图 1538×175") != std::wstring::npos;
        const bool mag = recBig.find(L"倍率 ×2.93") != std::wstring::npos;
        const bool formula = recBig.find(L"175 + 图x/2.93") != std::wstring::npos;
        ok = ok && delivered && mag && formula;
        if (!delivered) detail += L" receipt_not_delivered_size";
        if (!mag) detail += L" receipt_mag_not_on_delivered_size";
        if (!formula) detail += L" receipt_formula_not_on_delivered_size";
    }
    // 工具契约：缺参 / 无宿主都要如实拒绝（不猜区域、不假装有图）
    AiActionHostHooks hooks;
    AiActionToolOptions opts;
    const auto tools = BuildAiActionExecuteTools(&hooks, opts);
    const AgentTool* zt = nullptr;
    for (const auto& t : tools) { if (t.name == L"zoom") zt = &t; }
    if (!zt) {
        detail += L" tool_missing";
        ok = false;
    } else {
        const std::wstring noArg = zt->execute(LR"({})");
        const bool noArgOk = noArg.rfind(L"[错误]", 0) == 0
            && noArg.find(L"我不猜区域") != std::wstring::npos;
        ok = ok && noArgOk;
        if (!noArgOk) detail += L" no_arg_must_refuse";
        const std::wstring noHost = zt->execute(LR"({"x1":10,"y1":10,"x2":100,"y2":60})");
        const bool noHostOk = noHost.rfind(L"[错误]", 0) == 0
            && noHost.find(L"宿主") != std::wstring::npos;
        ok = ok && noHostOk;
        if (!noHostOk) detail += L" no_host_must_refuse";
    }
    // 有宿主：坐标入口与文字标签入口都要走通，且成功必须带图片标记
    AiActionHostHooks hooks2;
    std::wstring seenTarget;
    int seenMaxEdge = -1;
    hooks2.onZoomRegion = [&](int x1, int y1, int x2, int y2, const std::wstring& target, int maxEdge)
        -> AiZoomResult {
        AiZoomResult z;
        z.byTarget = !target.empty();
        seenTarget = target;
        seenMaxEdge = maxEdge;
        z.requested = target.empty() ? AiZoomRect{ x1, y1, x2, y2 } : AiZoomRect{ 10, 20, 110, 80 };
        AiZoomTile t;
        t.area = z.requested;
        t.outWidth = t.area.width() * 2;
        t.outHeight = t.area.height() * 2;
        t.bytes = 30000;
        // ⚠ 文件名**每次不同**（同轮两张图曾因固定名互相覆盖，见 zoom 那批的用例）；
        //   后缀是 `.jpg` —— 交付图**一次就编成送图链路的形态**（见 FormatAiZoomTempPath）。
        t.imagePath = FormatAiZoomTempPath(L"C:\\Temp\\QstZoom", 4321, 1);
        z.tiles.push_back(std::move(t));
        z.ok = true;
        return z;
    };
    const auto tools2 = BuildAiActionExecuteTools(&hooks2, opts);
    const AgentTool* zt2 = nullptr;
    for (const auto& t : tools2) { if (t.name == L"zoom") zt2 = &t; }
    if (!zt2) {
        detail += L" tool2_missing";
        ok = false;
    } else {
        const std::wstring byCoord = zt2->execute(LR"({"x1":10,"y1":20,"x2":110,"y2":80})");
        const bool coordOk = byCoord.find(L"[[AGENT_IMG:") != std::wstring::npos
            && byCoord.find(L"倍率 ×2.00") != std::wstring::npos
            && byCoord.find(L"入口＝坐标") != std::wstring::npos;
        ok = ok && coordOk;
        if (!coordOk) detail += L" coord_path";
        // ★工具给宿主的 maxEdge 默认值必须**等于**送图链路的硬上限（写更大只是幻觉）
        const bool defaultEdgeOk = seenMaxEdge == kAgentAttachmentMaxLongEdge;
        ok = ok && defaultEdgeOk;
        if (!defaultEdgeOk) detail += L" default_maxedge(" + std::to_wstring(seenMaxEdge) + L")";
        // 模型要更大也一样被收到同一个上限（否则回执里的倍率立刻开始说谎）
        zt2->execute(LR"({"x1":10,"y1":20,"x2":110,"y2":80,"maxEdge":4096})");
        const bool clampEdgeOk = seenMaxEdge == kAgentAttachmentMaxLongEdge;
        ok = ok && clampEdgeOk;
        if (!clampEdgeOk) detail += L" maxedge_not_clamped(" + std::to_wstring(seenMaxEdge) + L")";
        const std::wstring byLabel = zt2->execute(LR"({"target":"自选僵尸卡牌"})");
        const bool labelOk = byLabel.find(L"[[AGENT_IMG:") != std::wstring::npos
            && byLabel.find(L"入口＝文字标签") != std::wstring::npos
            && seenTarget == L"自选僵尸卡牌";
        ok = ok && labelOk;
        if (!labelOk) detail += L" label_path";
        // ★分块交付：宿主给 N 张 ⇒ 工具必须给 **N 个**图片标记（少一个模型就少看一块）
        AiActionHostHooks hooks3;
        hooks3.onZoomRegion = [](int x1, int y1, int x2, int y2, const std::wstring&, int)
            -> AiZoomResult {
            AiZoomResult z;
            z.ok = true;
            z.requested = AiZoomRect{ x1, y1, x2, y2 };
            const int mid = (x1 + x2) / 2;
            AiZoomTile a; a.area = AiZoomRect{ x1, y1, mid, y2 };
            a.outWidth = a.area.width() * 2; a.outHeight = a.area.height() * 2;
            a.bytes = 1000; a.imagePath = L"C:\\T\\zoom_1_1.jpg";
            AiZoomTile b; b.area = AiZoomRect{ mid, y1, x2, y2 };
            b.outWidth = b.area.width() * 2; b.outHeight = b.area.height() * 2;
            b.bytes = 1000; b.imagePath = L"C:\\T\\zoom_1_2.jpg";
            z.tiles.push_back(a);
            z.tiles.push_back(b);
            z.note = L"这块区域一张装不下，已**分成 2 张**给你";
            return z;
        };
        const auto tools3 = BuildAiActionExecuteTools(&hooks3, opts);
        const AgentTool* zt3 = nullptr;
        for (const auto& t : tools3) { if (t.name == L"zoom") zt3 = &t; }
        if (!zt3) {
            detail += L" tool3_missing";
            ok = false;
        } else {
            const std::wstring multi = zt3->execute(LR"({"x1":0,"y1":0,"x2":1024,"y2":80})");
            size_t marks = 0;
            for (size_t p = multi.find(L"[[AGENT_IMG:"); p != std::wstring::npos;
                 p = multi.find(L"[[AGENT_IMG:", p + 1)) {
                ++marks;
            }
            const bool multiOk = marks == 2
                && multi.find(L"第 2/2 张 = upload(512,0)-(1024,80)") != std::wstring::npos;
            ok = ok && multiOk;
            if (!multiOk) detail += L" multi_tile_markers(" + std::to_wstring(marks) + L")";
        }
    }
    Emit(L"zoom_region_tool", ok, detail.c_str());
}

// ★★`mouseDrag` 曾经**从来没成功过一次**（docs §54）：它拼出来的 `mouseDown`/`mouseUp`
//   不带 x/y，而校验要求 x/y ⇒ 整条拖拽被拒，还回一句**完全不搭界**的话
//   （「第 2 个 mouseDown 缺少 x/y。点击输入框须给截图坐标…」）——模型读到的是
//   「我点输入框的方式不对」，于是改去别处瞎试（实测用户日志里就有这一条）。
//   这个用例钉三件事：① 带坐标必须过；② 执行出去的 JSON 里真的有起点与终点；
//   ③ 缺坐标时的报错要**贴着这个动作**说，不许再提输入框。
// ★★同一坐标连点 N 次，每一次都必须**真的**发出去（docs §72）。
//   起因（真机日志）：20 击的一批里**后 10 击全部 0 步**，回执只有一句
//   `[错误] 本批 0 步：没有可执行的动作（空数组或全部被跳过）` —— 模型读不出原因、
//   排查也只能靠读源码猜。这条先钉死**工具层**：不许有「同点静默去重/静默丢弃」。
void CaseMouseClickRepeatSamePoint() {
    bool ok = true;
    std::wstring detail;

    AiActionHostHooks hooks;
    std::vector<std::wstring> payloads;
    hooks.onExecuteActions = [&](const std::wstring& js) {
        payloads.push_back(js);
        return L"已执行 1 步";
    };
    hooks.onObserveScreen = [](bool) {
        AiObserveCaptureResult r;
        r.ok = true;
        r.unchanged = true;
        return r;
    };
    AiActionToolOptions opts;
    const auto tools = BuildAiActionExecuteTools(&hooks, opts);
    const AgentTool* click = nullptr;
    for (const auto& t : tools) { if (t.name == L"mouseClick") click = &t; }
    if (!click) {
        detail += L" tool_missing";
        ok = false;
    } else {
        for (int i = 0; i < 12; ++i) (void)click->execute(LR"({"x":624,"y":72})");
        int withClick = 0;
        int withCoords = 0;
        for (const auto& p : payloads) {
            if (p.find(L"\"mouseClick\"") != std::wstring::npos) ++withClick;
            if (p.find(L"624") != std::wstring::npos
                && p.find(L"72") != std::wstring::npos) ++withCoords;
        }
        const bool countOk = payloads.size() == 12;
        const bool emitOk = withClick == 12 && withCoords == 12;
        ok = ok && countOk && emitOk;
        if (!countOk) detail += L" calls=" + std::to_wstring(payloads.size());
        if (!emitOk) {
            detail += L" emitted=" + std::to_wstring(withClick)
                + L" coords=" + std::to_wstring(withCoords);
        }
    }

    Emit(L"mouse_click_repeat_same_point", ok, detail.c_str());
}

void CaseMouseDragExecutes() {
    bool ok = true;
    std::wstring detail;

    AiActionHostHooks hooks;
    std::wstring executed;
    hooks.onExecuteActions = [&](const std::wstring& js) {
        executed = js;
        return L"已执行 1 步";
    };
    hooks.onObserveScreen = [](bool) {
        AiObserveCaptureResult r;
        r.ok = true;
        r.unchanged = true;
        return r;
    };
    AiActionToolOptions opts;
    const auto tools = BuildAiActionExecuteTools(&hooks, opts);
    const AgentTool* drag = nullptr;
    for (const auto& t : tools) { if (t.name == L"mouseDrag") drag = &t; }
    if (!drag) {
        detail += L" tool_missing";
        ok = false;
    } else {
        const std::wstring r = drag->execute(
            LR"({"fromX":300,"fromY":200,"toX":700,"toY":420})");
        const bool ranOk = r.rfind(L"[错误]", 0) != 0
            && executed.find(L"300") != std::wstring::npos
            && executed.find(L"200") != std::wstring::npos
            && executed.find(L"700") != std::wstring::npos
            && executed.find(L"420") != std::wstring::npos
            && executed.find(L"mouseDown") != std::wstring::npos
            && executed.find(L"mouseUp") != std::wstring::npos;
        ok = ok && ranOk;
        if (!ranOk) {
            detail += L" drag_exec(" + r.substr(0, 48) + L")";
        }
        // 缺坐标：报错必须说清是哪个动作缺什么，且**不许**再出现「输入框」那套
        const std::wstring bad = drag->execute(LR"({"toX":700,"toY":420})");
        const bool badOk = bad.rfind(L"[错误]", 0) == 0
            && bad.find(L"fromX") != std::wstring::npos
            && bad.find(L"输入框") == std::wstring::npos;
        ok = ok && badOk;
        if (!badOk) detail += L" drag_error_text";
    }

    // ★「计划执行」那行预览**不许说谎**（docs §70）。
    //   预览是把**工具参数**当成**动作 JSON** 描述的，而 mouseDrag 的
    //   工具参数是 fromX/fromY/toX/toY、动作字段是 x/y/endX/endY
    //   ⇒ 旧实现硬套，实测真机日志打出：
    //     `· 鼠标拖拽左键 (0,0)→(0,0) 0.300秒`
    //   而同一批真的从 (228,47) 拖到了 (760,300) —— 回执与事实相反。
    //   ① 名字对得上 → 描述里必须是**真坐标**；② 对不上 → 返回 false（调用方印原始参数）。
    {
        nlohmann::json j = nlohmann::json::parse(
            R"({"fromX":228,"fromY":47,"toX":760,"toY":300})");
        const bool aligned = AlignToolParamsWithActionFields(L"mouseDrag", j);
        const bool coordsOk = aligned && j.contains("x") && j.contains("y")
            && j.contains("endX") && j.contains("endY")
            && j.value("x", 0.0) == 228.0 && j.value("y", 0.0) == 47.0
            && j.value("endX", 0.0) == 760.0 && j.value("endY", 0.0) == 300.0;
        ok = ok && coordsOk;
        if (!coordsOk) detail += L" drag_preview_coords";

        // 描述出来的那行必须是真值，且**不含** `(0,0)→`
        ScriptAction a;
        a.type = ActionType::MouseDrag;
        a.button = MouseButtonType::Left;
        a.x = static_cast<int>(j.value("x", 0.0));
        a.y = static_cast<int>(j.value("y", 0.0));
        a.endX = static_cast<int>(j.value("endX", 0.0));
        a.endY = static_cast<int>(j.value("endY", 0.0));
        const std::wstring line = ActionName(a);
        const bool lineOk = line.find(L"228") != std::wstring::npos
            && line.find(L"760") != std::wstring::npos
            && line.find(L"(0,0)") == std::wstring::npos;
        ok = ok && lineOk;
        if (!lineOk) detail += L" drag_preview_line(" + line.substr(0, 60) + L")";

        // 将来新增的工具换了参数名：有坐标键却接不住 ⇒ 必须返回 false，不许编造
        nlohmann::json unknown = nlohmann::json::parse(R"({"x1":10,"y1":20})");
        ok = ok && !AlignToolParamsWithActionFields(L"someFutureTool", unknown);
        if (AlignToolParamsWithActionFields(L"someFutureTool", unknown))
            detail += L" fabricated_zero_coords";
        // 本来就没有坐标的工具（keyClick{keyText}）不许被这条闸误伤
        nlohmann::json noCoord = nlohmann::json::parse(R"({"keyText":"Enter"})");
        ok = ok && AlignToolParamsWithActionFields(L"keyClick", noCoord);
        if (!AlignToolParamsWithActionFields(L"keyClick", noCoord))
            detail += L" false_positive_on_text_tool";
    }

    Emit(L"mouse_drag_executes", ok, detail.c_str());
}

// ★坐标动作的回执必须回显落点（docs §54）。
//   A/B：把 CoordSuffix/DragSuffix 那两处分支去掉 ⇒ 本用例必须转红。
void CaseCoordActionReceipt() {
    bool ok = true;
    std::wstring detail;

    AiActionHostHooks hooks;
    std::wstring executedJson;
    hooks.onExecuteActions = [&executedJson](const std::wstring& js) {
        executedJson = js;
        return L"已执行 1 步";
    };
    hooks.onObserveScreen = [](bool) {
        AiObserveCaptureResult r;
        r.ok = true;
        r.unchanged = true;
        return r;
    };
    AiActionToolOptions opts;
    const auto tools = BuildAiActionExecuteTools(&hooks, opts);
    auto find = [&tools](const wchar_t* n) -> const AgentTool* {
        for (const auto& t : tools) { if (t.name == n) return &t; }
        return nullptr;
    };
    auto has = [](const std::wstring& s, const wchar_t* needle) {
        return s.find(needle) != std::wstring::npos;
    };

    // ① 单击：回执里必须有 (243,50)，而且必须是**原值**（upload 口径，不做任何换算）。
    const AgentTool* click = find(L"mouseClick");
    const AgentTool* move = find(L"moveMouse");
    if (!click || !move) {
        detail += L" tool_missing";
        ok = false;
    } else {
        const std::wstring r = click->execute(LR"({"x":243,"y":50})");
        const bool okClick = r.rfind(L"[错误]", 0) != 0 && has(r, L"(243,50)");
        ok = ok && okClick;
        if (!okClick) detail += L" click(" + r.substr(0, 64) + L")";

        const std::wstring rm = move->execute(LR"({"x":512,"y":288})");
        const bool okMove = rm.rfind(L"[错误]", 0) != 0 && has(rm, L"(512,288)");
        ok = ok && okMove;
        if (!okMove) detail += L" move(" + rm.substr(0, 64) + L")";
    }

    // ② 拖拽：**起点和终点都要在回执里**。注意 `mouseDrag` 工具是按
    //    moveMouse→mouseDown→moveMouse→mouseUp 展开的（不是单个 mouseDrag 动作），
    //    所以判据是「两个端点都出现过」，而不是「出现过箭头」——
    //    宿主把这一串**合并**回单个 mouseDrag 动作时才有 (起点)→(终点) 那种写法。
    const AgentTool* drag = find(L"mouseDrag");
    if (!drag) {
        detail += L" drag_missing";
        ok = false;
    } else {
        const std::wstring r = drag->execute(
            LR"({"fromX":300,"fromY":200,"toX":700,"toY":420})");
        const bool okDrag = r.rfind(L"[错误]", 0) != 0
            && has(r, L"(300,200)") && has(r, L"(700,420)");
        ok = ok && okDrag;
        if (!okDrag) detail += L" drag(" + r.substr(0, 96) + L")";

        // 合并形态：单个 mouseDrag 动作。★字段名以脚本 schema 为准 = `x/y` 起点 +
        // `endX/endY` 终点（`script_action_builder.cpp` 字段表 / `script_io.cpp` 只读这两个）。
        const AgentTool* batch = find(L"submitMacroActions");
        if (batch) {
            const std::wstring rb = batch->execute(
                LR"({"actions":[{"type":"mouseDrag","x":300,"y":200,"endX":740,"endY":460}]})");
            const bool okEnd = rb.rfind(L"[错误]", 0) != 0 && has(rb, L"(300,200)\u2192(740,460)");
            ok = ok && okEnd;
            if (!okEnd) detail += L" merged(" + rb.substr(0, 96) + L")";
        }

        // `computer(left_click_drag)` 别名：它必须产出**同一套字段名**。
        // ⚠ 断言钉在**交给宿主的动作 JSON** 上（`computer` 不走 SummarizeExecutedActionsJson，
        //   它的回执由宿主自己写），因为要钉的正是 schema 字段名。
        // A/B：换回 fromX/fromY/toX/toY ⇒ 这里转红（那套名字没有任何一处读 ⇒ 拖动退化成
        // (0,0)→(0,0)，正是「drag 从来没成功过」那个事故的另一条入口）。
        const AgentTool* comp = find(L"computer");
        if (comp) {
            executedJson.clear();
            const std::wstring rc = comp->execute(
                LR"({"action":"left_click_drag","coordinate":[117,347],"coordinate2":[289,799]})");
            const bool okAlias = rc.rfind(L"[错误]", 0) != 0
                && has(executedJson, L"mouseDrag")
                && has(executedJson, L"endX") && has(executedJson, L"endY")
                && !has(executedJson, L"fromX") && !has(executedJson, L"toX");
            ok = ok && okAlias;
            if (!okAlias) detail += L" alias(" + executedJson.substr(0, 120) + L")";
        }
    }

    Emit(L"coord_action_receipt_names_landing", ok, detail.c_str());
}


// 这两条都属于「算错了会静默出事」的那一类：
//   · 名字撞了 → 同一轮两张图变成同一张（模型会以为工具坏了而弃用它）；
//   · 清理算错 → 要么删掉模型刚要的那张图，要么永远不删把临时目录撑爆。
void CaseZoomTempImageFiles() {
    bool ok = true;
    std::wstring detail;
    // ① 同一个 pid、不同 seq ⇒ 必须是**不同**路径（同轮两次 zoom 不能互相覆盖）。
    //    后缀断言成 `.jpg`：交付图**一次就编成送图链路的形态**（JPEG q82 + 长边 ≤1280）——
    //    写 PNG 时磁盘上是 1365 KB 的中间产物，而模型手里是重编码后的 ~200 KB JPEG，
    //    回执按前者报字节数等于对模型说谎（docs §53）。
    const std::wstring p1 = FormatAiZoomTempPath(L"C:\\Temp\\QstZoom", 4321, 1);
    const std::wstring p2 = FormatAiZoomTempPath(L"C:\\Temp\\QstZoom", 4321, 2);
    const bool distinct = p1 != p2
        && p1 == L"C:\\Temp\\QstZoom\\zoom_4321_1.jpg"
        && p2 == L"C:\\Temp\\QstZoom\\zoom_4321_2.jpg";
    ok = ok && distinct;
    if (!distinct) detail += L" temp_path_not_unique(" + p1 + L")";

    // ② 清理：新的**留着**，旧的**删掉**（用 SetFileTime 造一个 1 小时前的文件）
    wchar_t tmpBuf[MAX_PATH]{};
    std::wstring dir = (GetTempPathW(MAX_PATH, tmpBuf) > 0)
        ? (std::wstring(tmpBuf) + L"QstZoomPruneSelftest") : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring fresh = FormatAiZoomTempPath(dir, 4321, 1);
    const std::wstring stale = FormatAiZoomTempPath(dir, 4321, 2);
    const std::wstring other = dir + L"\\keep_me.txt";   // 非 zoom_ 前缀：绝不能被删
    auto writeOne = [](const std::wstring& p) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        const char bytes[4] = { 'p', 'n', 'g', 0 };
        WriteFile(h, bytes, sizeof(bytes), &wrote, nullptr);
        CloseHandle(h);
        return true;
    };
    const bool wrote = writeOne(fresh) && writeOne(stale) && writeOne(other);
    FILETIME old{};
    {
        // ★★ **减 1 小时必须走 FILETIME 算术，不能只改 st.wHour**（2026-10-02 修）。
        //   旧写法 `st.wHour = (st.wHour + 23) % 24` 只退小时、**不退日期** ⇒ UTC 小时为 0 时
        //   （北京时间 08:00~09:00）结果变成**同一日的 23:00 = 未来 1 小时** ⇒
        //   `PruneAiZoomTempDir` 的「未来时间戳不许删」守卫**正确地**保留了它 ⇒
        //   本用例每天有 1 小时窗口必然报 `prune_kept_stale`（假红，与本用例要验的语义无关）。
        //   ⇒ 用 64 位算术真减 3600 秒。
        FILETIME nowFt{};
        GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER u{};
        u.LowPart = nowFt.dwLowDateTime;
        u.HighPart = nowFt.dwHighDateTime;
        u.QuadPart -= 3600ULL * 10000000ULL;   // 1 小时 = 3600s × 10^7（100ns 单位）
        old.dwLowDateTime = u.LowPart;
        old.dwHighDateTime = u.HighPart;
        HANDLE h = CreateFileW(stale.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            SetFileTime(h, nullptr, nullptr, &old);
            CloseHandle(h);
        }
    }
    PruneAiZoomTempDir(dir, 10ULL * 60ULL * 1000ULL);
    const bool freshKept = GetFileAttributesW(fresh.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool staleGone = GetFileAttributesW(stale.c_str()) == INVALID_FILE_ATTRIBUTES;
    const bool otherKept = GetFileAttributesW(other.c_str()) != INVALID_FILE_ATTRIBUTES;
    ok = ok && wrote && freshKept && staleGone && otherKept;
    if (!wrote) detail += L" prune_setup_failed";
    if (!freshKept) detail += L" prune_deleted_fresh";
    if (!staleGone) detail += L" prune_kept_stale";
    if (!otherKept) detail += L" prune_deleted_foreign_file";
    DeleteFileW(fresh.c_str());
    DeleteFileW(stale.c_str());
    DeleteFileW(other.c_str());
    RemoveDirectoryW(dir.c_str());

    Emit(L"zoom_temp_image_files", ok, detail.c_str());
}

// ★「短标签 → 它标注的图标槽」（docs §53）：用户主诉「只会放普通僵尸」的**根因**就在这条 ——
//   卡槽里 OCR 只读得到价签（「50/75/600」），**读不到卡片本身** ⇒ 模型知道数字在哪、
//   不知道那张卡在哪 ⇒ 反复 zoom 猜卡，最后只敢点唯一有把握的那张（最便宜的）。
//   这个用例钉四件事：
//     ① 真·成排短标签 + 正上方等距块 ⇒ 必须配对，且坐标给的是**那个块**（图标），不是文字；
//     ② 五个「不够格」的版面（上面是文字行 / 块数对不上 / 间距不匀 / 长标签 / 块之间没有分离
//        证据）⇒ **一条都不许发布**（配错 = 点到别的卡，比不发布糟得多）；
//     ③ 配对之后索引里**不能**再留同名文字条目（否则同档两条 ⇒ 解析判歧义拒绝 ⇒ 白做）；
//     ④ 位图采样本身要能从**真像素**里认出成排的块（合成剖面测不出这一步）。
void CaseLabelIconPairing() {
    bool ok = true;
    std::wstring detail;

    auto makeLabels = [](const std::vector<int>& centers, const std::vector<std::wstring>& texts,
                         int y1, int y2, int halfW) {
        std::vector<AiIndexRowInput> v;
        for (size_t i = 0; i < centers.size(); ++i) {
            AiIndexRowInput r;
            r.text = i < texts.size() ? texts[i] : std::to_wstring(centers[i]);
            r.x1 = centers[i] - halfW;
            r.y1 = y1;
            r.x2 = centers[i] + halfW;
            r.y2 = y2;
            v.push_back(r);
        }
        return v;
    };
    // 条带像素：背景灰 128；每个「卡」= 深色边框 + 亮内部 + 细竖纹（真图标有内部结构）。
    // ⚠ 前景故意占条带一半以上宽度 —— 第一版判据（与**行中位数**比）就是在这里翻极性的：
    //    它把 4 张卡读成了 4 条**缝**（卡片本身 ink=0）。这条 fixture 就是防它回来。
    auto makeLumBand = [](int x0, int y0, int width, int height,
                          const std::vector<std::pair<int, int>>& slots,
                          int firstRow, int lastRow) {
        AiIconBand b;
        b.x0 = x0;
        b.y0 = y0;
        b.width = width;
        b.height = height;
        b.lum.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 128);
        for (const auto& s : slots) {
            for (int y = firstRow; y <= lastRow && y < height; ++y) {
                if (y < 0) continue;
                for (int x = s.first; x <= s.second && x < width; ++x) {
                    if (x < 0) continue;
                    const bool border = (x == s.first || x == s.second
                        || y == firstRow || y == lastRow);
                    uint8_t v = border ? 40 : 250;
                    if (!border && ((x - s.first) % 3 == 0)) v = 205;
                    b.lum[static_cast<size_t>(y) * static_cast<size_t>(width)
                        + static_cast<size_t>(x)] = v;
                }
            }
        }
        return b;
    };

    const std::vector<int> kCenters{ 100, 200, 300, 400 };
    const std::vector<std::wstring> kPrices{ L"50", L"75", L"600", L"125" };
    const std::vector<std::pair<int, int>> kSlots{ {20, 80}, {120, 180}, {220, 280}, {320, 380} };
    const std::vector<AiIndexRowInput> labels = makeLabels(kCenters, kPrices, 90, 100, 10);

    // ① 正例：4 段短标签 + 正上方 4 个等高块 ⇒ 4 条配对，坐标是**块**
    {
        std::wstring why;
        const AiIconBand band = makeLumBand(50, 40, 400, 48, kSlots, 2, 45);
        const std::vector<AiLabelIconPair> pairs =
            PairCaptionRowWithIconBand(labels, band, &why);
        const bool posOk = pairs.size() == 4
            && pairs[0].label.text == L"50"
            && pairs[0].x1 == 70 && pairs[0].y1 == 42 && pairs[0].x2 == 131 && pairs[0].y2 == 86
            && pairs[3].label.text == L"125" && pairs[3].x1 == 370
            && pairs[0].slotCount == 4 && pairs[0].labelCount == 4 && pairs[0].iconHeight == 44;
        ok = ok && posOk;
        if (!posOk) {
            detail += L" positive(" + std::to_wstring(pairs.size()) + L"," + why + L")";
        }
        // ②-a 上面那行是**文字**（只有 14 行高 < 1.6×标签高）⇒ 不发布
        const bool thinOk = PairCaptionRowWithIconBand(labels,
            makeLumBand(50, 40, 400, 48, kSlots, 20, 33), &why).empty()
            && why.find(L"像文字行") != std::wstring::npos;
        ok = ok && thinOk;
        if (!thinOk) detail += L" reject_text_row_above(" + why + L")";
        // ②-b 有一段标签**正上方什么都没有**（5 段标签、只有 4 个块）⇒ 不猜
        const std::vector<AiIndexRowInput> fiveLabels =
            makeLabels({ 100, 200, 300, 400, 500 }, { L"50", L"75", L"600", L"125", L"400" },
                90, 100, 10);
        const bool missingOk = PairCaptionRowWithIconBand(fiveLabels,
            makeLumBand(50, 40, 500, 48, kSlots, 2, 45), &why).empty()
            && why.find(L"正上方没有独立的东西") != std::wstring::npos;
        ok = ok && missingOk;
        if (!missingOk) detail += L" reject_missing_block(" + why + L")";
        // ②-c 间距不匀（30/170/100）⇒ 不发布
        const std::vector<AiIndexRowInput> uneven =
            makeLabels({ 100, 130, 300, 400 }, kPrices, 90, 100, 10);
        const bool unevenOk = PairCaptionRowWithIconBand(uneven,
            makeLumBand(50, 40, 400, 48, kSlots, 2, 45), &why).empty()
            && why.find(L"间距不匀") != std::wstring::npos;
        ok = ok && unevenOk;
        if (!unevenOk) detail += L" reject_uneven_spacing";
        // ②-d 长标签（正文不是价签）⇒ 一句都不配
        std::vector<std::wstring> longText = kPrices;
        longText[1] = L"这是一句很长的正文";
        const bool longOk = PairCaptionRowWithIconBand(
            makeLabels(kCenters, longText, 90, 100, 10),
            makeLumBand(50, 40, 400, 48, kSlots, 2, 45), &why).empty()
            && why.find(L"短标签") != std::wstring::npos;
        ok = ok && longOk;
        if (!longOk) detail += L" reject_long_label";
        // ②-e 两块**分不开**（卡片宽窄不一、标签中点落进了块里）⇒ **一条都不许发布**。
        //      这里只断言「空」，不断言具体理由：背景估计被污染之后，拦住它的可能是
        //      「块太窄」「上面没有独立的东西」中的任何一条 —— 要的就是**保守**这一条性质。
        {
            const std::vector<AiIndexRowInput> lab =
                makeLabels({ 50, 150, 270, 390 }, kPrices, 90, 100, 10);
            const bool mergedOk = PairCaptionRowWithIconBand(lab,
                makeLumBand(0, 40, 460, 48, { {20, 80}, {120, 180}, {200, 340}, {360, 420} },
                    2, 45), &why).empty();
            ok = ok && mergedOk;
            if (!mergedOk) detail += L" reject_merged_blocks(" + why + L")";
        }
        // ③ 配对后索引里只剩 `LabeledIcon` 四条，且没有同名文字条目
        std::vector<AiElementEntry> elems = BuildAiElementIndexWithIcons({}, labels, pairs, 80);
        int iconCount = 0, textCount = 0;
        for (const auto& e : elems) {
            if (e.source == AiElementSource::LabeledIcon) ++iconCount;
            if (e.source == AiElementSource::OcrText) ++textCount;
        }
        const bool mergeOk = elems.size() == 4 && iconCount == 4 && textCount == 0
            && elems[0].name == L"50" && elems[0].x1 == 70 && !elems[0].note.empty();
        ok = ok && mergeOk;
        if (!mergeOk) {
            detail += L" merge(" + std::to_wstring(iconCount) + L"/" + std::to_wstring(textCount)
                + L")";
        }
        // 解析必须凭这一条命中（同名两条会判歧义 ⇒ 这条断言就是防它回来）。
        // 「600」是第 3 段标签，它的槽是 {220,280} → 屏幕 x1 = 270、x2 = 331 ⇒ 中心 300。
        AiIndexResolveResult ir;
        const bool resolveOk = AiElementIndexResolve(elems, L"600", false, 0, 0, &ir)
            && ir.ok && ir.source == AiElementSource::LabeledIcon && ir.x == 300;
        ok = ok && resolveOk;
        if (!resolveOk) detail += L" resolve_icon_slot(" + std::to_wstring(ir.x) + L")";
    }

    // ④ 位图采样：从**真像素**里拷出亮度（合成数组测不出这一步）
    {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 400;
        bi.bmiHeader.biHeight = -48;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) {
            detail += L" dib_create_failed";
            ok = false;
        } else {
            uint8_t* px = static_cast<uint8_t*>(bits);
            for (size_t i = 0; i < 400u * 48u; ++i) {
                px[i * 4 + 0] = 128;
                px[i * 4 + 1] = 128;
                px[i * 4 + 2] = 128;
                px[i * 4 + 3] = 255;
            }
            for (const auto& s : kSlots) {
                for (int y = 4; y < 44; ++y) {
                    for (int x = s.first; x <= s.second; ++x) {
                        const size_t idx = (static_cast<size_t>(y) * 400 + x) * 4;
                        px[idx + 0] = 250;
                        px[idx + 1] = 250;
                        px[idx + 2] = 250;
                    }
                }
            }
            AiIconBand band;
            const bool sampled = SampleIconBandFromBitmap(bmp, 0, 0, 0, 0, 400, 48, &band);
            const bool sampleOk = sampled && band.width == 400 && band.height == 48
                && band.x0 == 0 && band.y0 == 0
                && band.at(50, 20) > 200 && band.at(100, 20) < 160
                && band.at(50, 0) < 160;   // 块的上下留白是背景
            ok = ok && sampleOk;
            if (!sampleOk) {
                detail += L" bitmap_band_sample(sampled=" + std::to_wstring(sampled ? 1 : 0)
                    + L",at50_20=" + std::to_wstring(band.at(50, 20))
                    + L",at100_20=" + std::to_wstring(band.at(100, 20))
                    + L",w=" + std::to_wstring(band.width) + L")";
            }
            // 端到端：像素 → 亮度 → 配对（标签在条带**下方**：y 50..60）
            if (sampled) {
                const std::vector<AiIndexRowInput> lab =
                    makeLabels({ 50, 150, 250, 350 }, kPrices, 50, 60, 10);
                const std::vector<AiLabelIconPair> pairs =
                    PairCaptionRowWithIconBand(lab, band, nullptr);
                const bool e2eOk = pairs.size() == 4 && pairs[1].x1 == 120
                    && pairs[1].x2 == 181 && pairs[1].y1 == 4 && pairs[1].y2 == 44;
                ok = ok && e2eOk;
                if (!e2eOk) detail += L" pixel_to_pair(" + std::to_wstring(pairs.size()) + L")";
            }
            DeleteObject(bmp);
        }
    }

    // ★★网格 fixture：选卡面板那种「一排排卡片、每张下面一个价签」的版面（docs §54）。
    //   实测那一局它在**两个地方**被卡死：① 给模型看的索引对纯数字限量 16 条，而一个
    //   8×10 的网格有 ~50 个价签 ⇒ 配对读到的每行不足 3 段 ⇒ 一条都不发布，
    //   而且日志里连原因都没有（`标签→图标槽推断 0 条`）；② 卡片之间只隔一条描边时，
    //   旧判据「扩到偏离掉一半就停」必然扩到邻居 ⇒ 整行作废。两条都钉在这里。
    {
        std::wstring why;
        // 8 列：卡片 40 宽、间距 43（间歇 3 列背景）；标签在卡片正下方的中心
        std::vector<int> centers;
        std::vector<std::pair<int, int>> slots;
        for (int i = 0; i < 8; ++i) {
            const int cx = 20 + i * 43;
            centers.push_back(cx + 20);
            slots.push_back({ cx, cx + 39 });
        }
        std::vector<std::wstring> texts;
        for (int i = 0; i < 8; ++i) texts.push_back(std::to_wstring(50 + i * 25));
        const std::vector<AiIndexRowInput> row = makeLabels(centers, texts, 90, 100, 9);
        // 条带要**容得下最后一张卡的右边框**（否则如实拒绝「顶到条带边缘」——
        // 本用例第一版就是差一列，正好把这条判据演示了一遍）
        const AiIconBand band = makeLumBand(0, 40, 380, 48, slots, 2, 45);
        const std::vector<AiLabelIconPair> pairs =
            PairCaptionRowWithIconBand(row, band, &why);
        const bool gridOk = pairs.size() == 8
            && pairs[0].x1 == 20 && pairs[0].x2 == 60
            && pairs[7].x1 == 321 && pairs[7].label.text == L"225";
        ok = ok && gridOk;
        if (!gridOk) detail += L" grid_row(" + std::to_wstring(pairs.size()) + L"," + why + L")";
        // 行分组本身也不能被「纯数字限量」卡住（那是给模型看的展示层闸，不是感知层的）：
        // 实测一个 8×10 的选卡网格有 ~50 个价签，限量 16 条 ⇒ 配对每行凑不满 3 段。
        std::vector<OcrTextLine> lines;
        std::vector<int> wideCenters;
        for (int i = 0; i < 20; ++i) {
            OcrTextLine ln;
            ln.text = std::to_wstring(50 + i * 25);
            ln.confidence = 0.95;
            const int cx = 40 + i * 43;
            wideCenters.push_back(cx);
            ln.x1 = cx - 14; ln.y1 = 90; ln.x2 = cx + 14; ln.y2 = 100;
            lines.push_back(ln);
        }
        const auto cappedRows = CollectOcrIndexRows(lines, 64);
        const auto fullRows = CollectOcrIndexRows(lines, 64, 4096);
        const size_t capped = cappedRows.empty() ? 0 : cappedRows.front().size();
        const size_t full = fullRows.empty() ? 0 : fullRows.front().size();
        const bool capOk = full == 20 && capped == 16;
        ok = ok && capOk;
        if (!capOk) {
            detail += L" numeric_cap_split(" + std::to_wstring(capped) + L"/"
                + std::to_wstring(full) + L")";
        }

        // ★★真机几何：**窄价签 + 宽卡槽**（docs §56）。
        //   上面那个 fixture 的价签宽 28、槽距 43 ⇒ 间距 15 ≤ 字高 10×2 ⇒ 天然成一行，
        //   所以它**永远测不出**真机上的问题：真机卡槽 ≈112 native px、价签只有 ≈30 px 宽
        //   ⇒ 间距 ≈82 > 字高 26×2=52 ⇒ 每个价签各自成行 ⇒ 配对连门槛都进不去
        //   （`标签→图标槽推断 0 条`，而且"未配对"的诊断都不会打）。
        std::vector<OcrTextLine> deck;
        for (int i = 0; i < 14; ++i) {
            OcrTextLine ln;
            ln.text = (i == 10) ? L"600" : L"75";
            ln.confidence = 0.95;
            const int cx = 300 + i * 112;      // 卡槽 112 宽
            ln.x1 = cx - 15; ln.y1 = 165;      // 价签只占 30 宽、26 高
            ln.x2 = cx + 15; ln.y2 = 191;
            deck.push_back(ln);
        }
        const auto narrow = CollectOcrIndexRows(deck, 64, 4096);            // 默认间距闸
        const auto narrowWide = CollectOcrIndexRows(deck, 64, 4096, 64);    // 版面推断用
        const size_t nDefault = narrow.empty() ? 0 : narrow.front().size();
        const size_t nWide = narrowWide.empty() ? 0 : narrowWide.front().size();
        // 默认闸**必须照旧**把远间距切开（给模型看的索引口径不变），
        // 宽闸必须把它们并成一行 —— 两边都不一样才说明这个参数真的分开了两个消费者。
        const bool gapOk = nDefault == 1 && narrowWide.size() == 1 && nWide == 14;
        ok = ok && gapOk;
        if (!gapOk) {
            detail += L" narrow_label_gap(default=" + std::to_wstring(nDefault)
                + L",rows=" + std::to_wstring(narrowWide.size())
                + L",wide=" + std::to_wstring(nWide) + L")";
        }
        // ★★真机形状（docs §63）：**卡牌价签本身等距，但同一视觉行里混进了太阳计数器**。
        //   真机数据：`30000`@187 与第一个价签@242 同在一行，末尾还有一个孤立价签@564
        //   ⇒ 整行变异系数 **0.72** ⇒ 旧判据（整行 CV ≤ 0.35）**整排作废**，实测 8 次都是
        //   `变异系数 0.36 > 0.35`（只差 0.01）⇒ 模型拿不到任何卡坐标 ⇒ 一次运行 17 次 zoom。
        //   现在按「中位间距 ±35% 内的最长等距段」判：丢掉那两个**不同来源**的标签，保住等距的 4 张卡。
        {
            std::vector<std::wstring> texts{ L"30000", L"75", L"100", L"600", L"125" };
            const std::vector<AiIndexRowInput> lab =
                makeLabels({ 50, 100, 150, 200, 400 }, texts, 90, 100, 10);
            const std::vector<AiLabelIconPair> kept = PairCaptionRowWithIconBand(lab,
                makeLumBand(0, 40, 460, 48,
                    { {30, 70}, {80, 120}, {130, 170}, {180, 220} }, 2, 45), &why);
            // 等距的 4 段必须保住（顺序按 x 排），非等距的第 5 段必须被丢掉
            const bool runOk = kept.size() == 4
                && kept[0].label.text == L"30000" && kept[3].label.text == L"600";
            ok = ok && runOk;
            if (!runOk) {
                detail += L" longest_even_run(" + std::to_wstring(kept.size()) + L"," + why + L")";
            }
        }
        // 反过来钉：**真的不成排**仍然一条都不许发布（只放宽"谁参与"，不放宽"成不成排"）
        {
            const std::vector<AiIndexRowInput> lab =
                makeLabels({ 10, 60, 400 }, { L"50", L"75", L"600" }, 90, 100, 10);
            const bool irregularOk = PairCaptionRowWithIconBand(lab,
                makeLumBand(0, 40, 460, 48, { {20, 60}, {70, 110}, {380, 420} }, 2, 45),
                &why).empty();
            ok = ok && irregularOk;
            if (!irregularOk) detail += L" accept_irregular_row(" + why + L")";
        }
        // ★★诊断文案必须**说清是哪一条判据失败的**（docs §60 的事故）：
        //   出界与「被上一行压没」是两个完全不同的原因，旧文案一律写「条带出界（…捕获区 …）」
        //   ⇒ 排查被指向「捕获区域配错了」这个错误方向。这里逐个原因断言，并断言三条互不相同
        //   （"同一种原因说两句话"无害，"两种原因说同一句话"正是那次事故）。
        {
            const std::wstring none = AiExplainIconBandSkip(100, 140, INT_MIN, 0, 48);
            const std::wstring squeezed = AiExplainIconBandSkip(141, 140, 139, 0, 48);
            const std::wstring pushed = AiExplainIconBandSkip(402, 500, 400, 500, 200);
            const std::wstring tooHigh = AiExplainIconBandSkip(10, 210, INT_MIN, 200, 200);
            const bool skipOk = none.empty()
                && squeezed.find(L"上一行标签压没") != std::wstring::npos
                && pushed.find(L"顶到捕获区之外") != std::wstring::npos
                && tooHigh.find(L"落在捕获区上沿之外") != std::wstring::npos
                && squeezed != pushed && pushed != tooHigh && squeezed != tooHigh;
            ok = ok && skipOk;
            if (!skipOk) {
                detail += L" band_skip(notEmpty=" + std::to_wstring(none.empty() ? 0 : 1)
                    + L",a=" + squeezed.substr(0, 12) + L",b=" + pushed.substr(0, 12)
                    + L",c=" + tooHigh.substr(0, 12) + L")";
            }
        }
    }

    Emit(L"caption_icon_pairing", ok, detail.c_str());
}

void CaseElementIndexAndResolve() {
    bool ok = true;
    std::wstring detail;

    // ① 合并：同一处（IoU 高）+ 名字对得上 → 一条，且保留 UIA 的矩形与来源
    {
        std::vector<AiUiAnchor> ui = {
            { 100, 200, 220, 236, L"一键全选" },      // UIA 控件框（真框）
        };
        std::vector<AiIndexRowInput> rows = {
            { L"一键全选", 108, 206, 214, 232 },       // OCR 文字框（同处、同名）
        };
        const auto idx = BuildAiElementIndex(ui, rows, 80);
        ok = ok && idx.size() == 1;
        if (idx.size() != 1) detail += L" merge_count(" + std::to_wstring(idx.size()) + L")";
        if (!idx.empty()) {
            const bool keptUi = idx[0].source == AiElementSource::UiAutomation
                && idx[0].x1 == 100 && idx[0].y1 == 200 && idx[0].x2 == 220 && idx[0].y2 == 236;
            ok = ok && keptUi;
            if (!keptUi) detail += L" merge_kept_wrong_rect";
            ok = ok && idx[0].id == 1;
            if (idx[0].id != 1) detail += L" id_not_from_1";
        }
    }

    // ② 合并要**保守**：同处但名字不相干 → 两条（并错 = 点错东西）
    {
        std::vector<AiUiAnchor> ui = { { 300, 300, 380, 330, L"设置" } };
        std::vector<AiIndexRowInput> rows = { { L"退出登录", 302, 302, 378, 328 } };
        const auto idx = BuildAiElementIndex(ui, rows, 80);
        ok = ok && idx.size() == 2;
        if (idx.size() != 2) detail += L" overmerge(" + std::to_wstring(idx.size()) + L")";
    }
    // 不相干位置的同名 → 也是两条（同屏两个「确定」必须都留着，靠 near 区分）
    {
        std::vector<AiUiAnchor> ui = { { 100, 100, 160, 130, L"确定" } };
        std::vector<AiIndexRowInput> rows = { { L"确定", 900, 700, 960, 730 } };
        const auto idx = BuildAiElementIndex(ui, rows, 80);
        ok = ok && idx.size() == 2;
        if (idx.size() != 2) detail += L" far_same_name_merged("
            + std::to_wstring(idx.size()) + L")";
    }

    // ③ 编号按阅读顺序（上→下、左→右），id 从 1 连续
    {
        std::vector<AiIndexRowInput> rows = {
            { L"右下", 800, 500, 900, 530 },
            { L"左上", 100, 100, 200, 130 },
            { L"右上", 800, 100, 900, 130 },
        };
        const auto idx = BuildAiElementIndex({}, rows, 80);
        bool order = idx.size() == 3;
        if (order) {
            order = idx[0].name == L"左上" && idx[1].name == L"右上" && idx[2].name == L"右下";
            order = order && idx[0].id == 1 && idx[1].id == 2 && idx[2].id == 3;
        }
        ok = ok && order;
        if (!order) {
            detail += L" order(";
            for (const auto& e : idx) detail += e.name + L",";
            detail += L")";
        }
    }

    // ④ 解析：完全相等优先于包含；UIA 与 OCR 同表都能命中
    {
        std::vector<AiElementEntry> idx;
        auto add = [&](int id, std::wstring name, int x, int y, AiElementSource s) {
            AiElementEntry e;
            e.id = id;
            e.name = std::move(name);
            e.x1 = x;
            e.y1 = y;
            e.x2 = x + 60;
            e.y2 = y + 30;
            e.source = s;
            idx.push_back(e);
        };
        add(1, L"保存并关闭", 100, 100, AiElementSource::OcrText);
        add(2, L"保存", 300, 100, AiElementSource::UiAutomation);
        AiIndexResolveResult r;
        const bool hit = AiElementIndexResolve(idx, L"保存", false, 0, 0, &r);
        ok = ok && hit && r.id == 2 && r.tier == 2 && r.source == AiElementSource::UiAutomation;
        if (!hit) detail += L" resolve_miss";
        else if (r.id != 2) detail += L" resolve_picked(" + std::to_wstring(r.id) + L")";
        // 坐标必须是该条目的中心
        ok = ok && r.x == 330 && r.y == 115;
        if (r.x != 330 || r.y != 115)
            detail += L" center(" + std::to_wstring(r.x) + L"," + std::to_wstring(r.y) + L")";
    }

    // ④.5 同档多条时按**来源可信度**收敛：UIA 控件（控件真框）优先于 OCR 文字
    //   （文字框中心常偏在角标上）。⚠ 这条钉的是批 C 删掉「格」那一档来源枚举之后
    //   **优先级比较仍然正确** —— 原来它是最高的第 2 档（格中心），
    //   现在只剩 控件(1) / 文字(0)；语义若被改坏，这一格会退化成「同档分不出 → 歧义拒绝」，
    //   而调用方只会看到「索引里没有」，落回一次白付的识图。
    {
        std::vector<AiElementEntry> idx;
        auto add = [&](int id, std::wstring name, int x, AiElementSource s) {
            AiElementEntry e;
            e.id = id;
            e.name = std::move(name);
            e.x1 = x;
            e.y1 = 100;
            e.x2 = x + 60;
            e.y2 = 130;
            e.source = s;
            idx.push_back(e);
        };
        add(1, L"保存至", 100, AiElementSource::OcrText);
        add(2, L"保存到", 300, AiElementSource::UiAutomation);
        AiIndexResolveResult r;
        const bool hit = AiElementIndexResolve(idx, L"保存", false, 0, 0, &r);
        // 无 near、两条同档（都是「双向包含」1 档）⇒ 只有来源优先级能收敛出唯一一条
        ok = ok && hit && r.id == 2 && r.tier == 1
            && r.source == AiElementSource::UiAutomation && r.x == 330;
        if (!hit) detail += L" pref_source_miss";
        else if (r.id != 2) detail += L" pref_source_picked(" + std::to_wstring(r.id) + L")";
        else if (r.x != 330) detail += L" pref_source_center(" + std::to_wstring(r.x) + L")";
    }

    // ⑤ 同档多条 + 无 near → 歧义拒绝（宁可回落识图，也不猜着点）
    {
        std::vector<AiElementEntry> idx;
        for (int i = 0; i < 2; ++i) {
            AiElementEntry e;
            e.id = i + 1;
            e.name = L"确定";
            e.x1 = 100 + i * 500;
            e.y1 = 100;
            e.x2 = e.x1 + 60;
            e.y2 = 130;
            idx.push_back(e);
        }
        AiIndexResolveResult r;
        const bool hit = AiElementIndexResolve(idx, L"确定", false, 0, 0, &r);
        ok = ok && !hit && r.ambiguous;
        if (hit || !r.ambiguous) detail += L" no_near_not_ambiguous";
        // 给了 near → 取最近的那条
        AiIndexResolveResult r2;
        const bool hit2 = AiElementIndexResolve(idx, L"确定", true, 560, 115, &r2);
        ok = ok && hit2 && r2.id == 2 && !r2.ambiguous;
        if (!hit2 || r2.id != 2) detail += L" near_pick_failed";
    }
    // 同档两条**都很近** → 仍然歧义
    {
        std::vector<AiElementEntry> idx;
        for (int i = 0; i < 2; ++i) {
            AiElementEntry e;
            e.id = i + 1;
            e.name = L"确定";
            e.x1 = 100 + i * 8;
            e.y1 = 100;
            e.x2 = e.x1 + 60;
            e.y2 = 130;
            idx.push_back(e);
        }
        AiIndexResolveResult r;
        const bool hit = AiElementIndexResolve(idx, L"确定", true, 120, 115, &r);
        ok = ok && !hit && r.ambiguous;
        if (hit || !r.ambiguous) detail += L" close_pair_not_ambiguous";
    }

    // ⑥ ★窗口自身标题栏按钮：永不入选；「只匹配到它」要给可执行的解释
    {
        std::vector<AiElementEntry> idx;
        AiElementEntry chrome;
        chrome.id = 1;
        chrome.name = L"关闭";
        chrome.x1 = 2500;
        chrome.y1 = 10;
        chrome.x2 = 2540;
        chrome.y2 = 40;
        chrome.windowChrome = true;
        idx.push_back(chrome);
        AiElementEntry app;
        app.id = 2;
        app.name = L"关闭";
        app.x1 = 900;
        app.y1 = 400;
        app.x2 = 960;
        app.y2 = 430;
        idx.push_back(app);
        // 应用内那个同名按钮仍可命中（不能因为名字一样就把正常操作也拦掉）
        AiIndexResolveResult r;
        const bool hit = AiElementIndexResolve(idx, L"关闭", true, 930, 415, &r);
        ok = ok && hit && r.id == 2;
        if (!hit || r.id != 2) detail += L" chrome_not_excluded";
        // 只剩窗口按钮时：拒绝 + why 里说明原因
        idx.erase(idx.begin() + 1);
        AiIndexResolveResult r2;
        const bool hit2 = AiElementIndexResolve(idx, L"关闭", false, 0, 0, &r2);
        ok = ok && !hit2 && r2.why.find(L"窗口自身") != std::wstring::npos;
        if (hit2) detail += L" chrome_clickable";
        else if (r2.why.find(L"窗口自身") == std::wstring::npos)
            detail += L" chrome_why_missing";
    }

    // ⑦ 太短/纯数字目标：**只允许精确档**（不许宽容档），但不许一刀切拒绝 ——
    //    表格单元格、卡片角标的价格本来就是纯数字，「按价格/编号选」必须能用。
    {
        std::vector<AiElementEntry> idx;
        for (int i = 0; i < 3; ++i) {
            AiElementEntry e;
            e.id = i + 1;
            e.name = L"600";
            e.x1 = 100 + i * 200;
            e.y1 = 100;
            e.x2 = e.x1 + 60;
            e.y2 = 130;
            idx.push_back(e);
        }
        // 三条精确同名：有 near 取最近
        AiIndexResolveResult r;
        const bool hit = AiElementIndexResolve(idx, L"600", true, 110, 115, &r);
        ok = ok && hit && r.id == 1;
        if (!hit) detail += L" numeric_strict_rejected";
        else if (r.id != 1) detail += L" numeric_picked(" + std::to_wstring(r.id) + L")";
        // 没有 near 且三条同样精确 → 歧义拒绝（不许取第一个）
        AiIndexResolveResult r1b;
        const bool hit1b = AiElementIndexResolve(idx, L"600", false, 0, 0, &r1b);
        ok = ok && !hit1b && r1b.ambiguous;
        if (hit1b || !r1b.ambiguous) detail += L" numeric_not_ambiguous";
        // 单字目标：连精确档也不认（「6」这种满屏都是，无法判断用户指哪个）
        AiIndexResolveResult r2;
        ok = ok && !AiElementIndexResolve(idx, L"6", false, 0, 0, &r2);
        if (AiElementIndexResolve(idx, L"6", false, 0, 0, &r2)) detail += L" short_resolved";
        AiIndexResolveResult r3;
        ok = ok && !AiElementIndexResolve(idx, L"", false, 0, 0, &r3);
    }

    // ⑧ 灰控件不参与（点了也没用，不该抢走正确目标）
    {
        std::vector<AiElementEntry> idx;
        AiElementEntry e;
        e.id = 1;
        e.name = L"确定";
        e.x1 = 100;
        e.y1 = 100;
        e.x2 = 160;
        e.y2 = 130;
        e.enabled = false;
        idx.push_back(e);
        AiIndexResolveResult r;
        const bool hit = AiElementIndexResolve(idx, L"确定", false, 0, 0, &r);
        ok = ok && !hit;
        if (hit) detail += L" disabled_resolved";
    }

    // ⑨ 清单文本：窗口自身按钮**不进清单**（列出来只会诱导模型去点）；空输入给空串
    {
        std::vector<AiElementEntry> idx;
        AiElementEntry chrome;
        chrome.id = 1;
        chrome.name = L"关闭";
        chrome.x1 = 10;
        chrome.y1 = 10;
        chrome.x2 = 40;
        chrome.y2 = 40;
        chrome.windowChrome = true;
        idx.push_back(chrome);
        AiElementEntry normal;
        normal.id = 2;
        normal.name = L"开始游戏";
        normal.x1 = 100;
        normal.y1 = 200;
        normal.x2 = 240;
        normal.y2 = 240;
        idx.push_back(normal);
        const std::wstring txt = FormatAiElementIndex(idx, 0, 0, 1000, 1000, 1000, 1000);
        ok = ok && txt.find(L"开始游戏") != std::wstring::npos
            && txt.find(L"关闭") == std::wstring::npos;
        if (txt.find(L"开始游戏") == std::wstring::npos) detail += L" list_missing_item";
        if (txt.find(L"关闭") != std::wstring::npos) detail += L" list_has_chrome";
        ok = ok && FormatAiElementIndex({}, 0, 0, 1000, 1000, 1000, 1000).empty();
        // ★坐标口径：清单里必须是 **upload 截图像素**（与 mouseClick 同一套），
        //   不是屏幕像素。这条踩过大坑 —— 给了屏幕像素后模型拿去做 mouseClick
        //   在 2560×1440 截成 1024×576 的帧里偏 2.5 倍，还花了 40s+ 推敲坐标口径。
        //   这里断言换算值本身：捕获区 2560×1440 → 帧 1024×576（0.4 倍）。
        {
            std::vector<AiElementEntry> one;
            AiElementEntry e;
            e.id = 1;
            e.name = L"开始游戏";
            e.x1 = 500;
            e.y1 = 400;
            e.x2 = 700;
            e.y2 = 500;      // 中心 (600, 450)（屏幕坐标）
            one.push_back(e);
            const std::wstring t2 = FormatAiElementIndex(one, 0, 0, 2560, 1440, 1024, 576);
            // 600 * 1024/2560 = 240；450 * 576/1440 = 180
            const bool conv = t2.find(L"(240,180)") != std::wstring::npos;
            ok = ok && conv;
            if (!conv) detail += L" frame_coord_not_converted";
            const bool declaresFrame = t2.find(L"upload 截图像素") != std::wstring::npos
                && t2.find(L"1024") != std::wstring::npos;
            ok = ok && declaresFrame;
            if (!declaresFrame) detail += L" coord_space_undeclared";
        }
    }

    // ⑩ ★宽容档：OCR 丢字时也要能在索引里查到自己刚看到的文字
    //    （实测「自选僵尸卡牌」被 OCR 读成「自选僵尸卡」，长度比闸挡掉 → 模型反复识图并
    //     花 40s+ 推敲「到底是屏幕坐标还是图像坐标」，两个坑叠在一起卡死一整轮）
    {
        const bool partialOk =
            AiElementIndexPartialMatch(L"自选僵尸卡", L"自选僵尸卡牌") == 1
            && AiElementIndexPartialMatch(L"自选僵尸卡牌", L"自选僵尸卡") == 1
            && AiElementIndexPartialMatch(L"一键全选", L"一键全全部选") == 1;   // 并字
        ok = ok && partialOk;
        if (!partialOk) detail += L" partial_match_failed";
        // 不相干的短标签绝不能命中
        const bool rejectOk =
            AiElementIndexPartialMatch(L"主菜单", L"自选僵尸卡牌") == 0
            && AiElementIndexPartialMatch(L"时停", L"自选僵尸卡牌") == 0
            && AiElementIndexPartialMatch(L"通关", L"这关") == 0;
        ok = ok && rejectOk;
        if (!rejectOk) detail += L" partial_false_positive";
        // 长句/不相干短语不走宽容档（避免误伤）
        // ⚠ 反例要选**真的不像**的：第一版写了「请点击这里确认」vs「请点击那里确认」，
        //   两者 LCS 85%，宽容档命中是**对的** —— 测的其实是「长句也不许匹配」这个错误预期。
        const bool longOk =
            AiElementIndexPartialMatch(L"请点击这里确认", L"换一个完全不同的") == 0;
        ok = ok && longOk;
        if (!longOk) detail += L" partial_on_unrelated_text";

        // 索引解析里：精确命中存在时，宽容候选不许抢走它
        std::vector<AiElementEntry> idx;
        auto push = [&](int id, std::wstring name, int x, int y) {
            AiElementEntry e;
            e.id = id;
            e.name = std::move(name);
            e.x1 = x;
            e.y1 = y;
            e.x2 = x + 80;
            e.y2 = y + 30;
            idx.push_back(e);
        };
        push(1, L"自选僵尸卡", 100, 100);      // OCR 丢字版
        push(2, L"自选僵尸卡牌", 500, 100);    // 精确版
        AiIndexResolveResult r;
        const bool hit = AiElementIndexResolve(idx, L"自选僵尸卡牌", false, 0, 0, &r);
        ok = ok && hit && r.id == 2 && r.tier == 2;
        if (!hit || r.id != 2) detail += L" partial_stole_exact";
        // 只有丢字版时，宽容档要能命中（这正是那次卡死的场景）
        AiIndexResolveResult r2;
        const bool hit2 = AiElementIndexResolve(idx, L"自选僵尸卡牌x", false, 0, 0, &r2);
        (void)hit2;   // 加了个字母就不该命中（顺序不完全保留），仅确保不崩
        idx.erase(idx.begin() + 1);
        AiIndexResolveResult r3;
        const bool hit3 = AiElementIndexResolve(idx, L"自选僵尸卡牌", false, 0, 0, &r3);
        ok = ok && hit3 && r3.id == 1 && r3.tier == 1;
        if (!hit3 || r3.id != 1) detail += L" partial_not_resolved";

        // ★P3：索引**不再自称「可点」** —— OCR 文字条目的可点性没人验证过，必须在标题里说清。
        //   实测那一局模型据此去点了「我是僵尸」（模式标签）与面板标题，两次都白费一轮。
        //   判据断的是**文案事实本身**：既要说「不是每条都能点」，也要标出「文字」这个来源档。
        const std::wstring idxFmt = FormatAiElementIndex(idx, 0, 0, 1000, 1000, 1024, 576, 8000);
        const bool caveatOk = idxFmt.find(L"不是每条都能点") != std::wstring::npos
            && idxFmt.find(L"可点性未经验证") != std::wstring::npos;
        ok = ok && caveatOk;
        if (!caveatOk) detail += L" index_still_claims_all_clickable";
        const bool srcTagged = idxFmt.find(L"文字") != std::wstring::npos;
        ok = ok && srcTagged;
        if (!srcTagged) detail += L" index_source_tag_missing";
    }

    Emit(L"element_index_and_resolve", ok, detail.c_str());
}

// ★★「半视觉」两条：① 索引条目要带**角色/动作能力/可读状态**；
//    ② 编号要能**真的用**（按编号直查一条）。
// 这一组就是本轮优化的验收点 —— 少了任何一条，「看得见编号、用不上编号」的缺口就还在。
void CaseElementIndexFactsAndIdBinding() {
    bool ok = true;
    std::wstring detail;

    // ① 角色 / 动作能力 / 状态从 UIA 锚点**如实透传**进索引（不被合并/排序丢掉）
    {
        AiUiAnchor slider;
        slider.x1 = 100; slider.y1 = 200; slider.x2 = 400; slider.y2 = 230;
        slider.name = L"音量";
        slider.role = L"滑块";
        slider.action = L"slide";
        slider.state = { L"value:42", L"range:0-100" };
        AiUiAnchor edit;
        edit.x1 = 100; edit.y1 = 300; edit.x2 = 400; edit.y2 = 330;
        edit.name = L"文件名";
        edit.role = L"输入框";
        edit.action = L"fill";
        edit.state = { L"focused" };

        std::vector<AiIndexRowInput> rows = {
            // OCR 读到了同一个「音量」（同处同名 → 并成一条）：要求能力位**不许被 OCR 抢走**
            { L"音量", 104, 204, 396, 226 },
        };
        const auto idx = BuildAiElementIndex({ slider, edit }, rows, 80);
        ok = ok && idx.size() == 2;
        if (idx.size() != 2) detail += L" facts_count(" + std::to_wstring(idx.size()) + L")";
        if (idx.size() == 2) {
            // 阅读顺序：音量(y≈215) 在前，文件名(y≈315) 在后
            const AiElementEntry& a = idx[0];
            const bool sliderKept = a.name == L"音量" && a.source == AiElementSource::UiAutomation
                && a.role == L"滑块" && a.action == L"slide" && a.state.size() == 2;
            ok = ok && sliderKept;
            if (!sliderKept) {
                detail += L" facts_slider(role=" + a.role + L" action=" + a.action
                    + L" state=" + std::to_wstring(a.state.size()) + L")";
            }
            const bool editKept = idx[1].action == L"fill" && idx[1].role == L"输入框";
            ok = ok && editKept;
            if (!editKept) detail += L" facts_edit";
        }
    }

    // ② ★OCR 条目**不许**有动作能力（本地没有证据 ⇒ 不猜）。
    //    猜错 = 模型以为能点，点了没反应白费一轮 —— 这正是索引标题里那句
    //    「可点性未经验证」要用行为兜住的地方。
    {
        std::vector<AiIndexRowInput> rows = { { L"模式标签", 10, 10, 120, 34 } };
        const auto idx = BuildAiElementIndex({}, rows, 80);
        const bool noFakeCap = idx.size() == 1 && idx[0].action.empty() && idx[0].role.empty();
        ok = ok && noFakeCap;
        if (!noFakeCap) detail += L" ocr_got_fake_action";
    }

    // ③ 台账文本要把这三样**渲染出来**（模型看到的是文本，不是结构体）
    {
        AiUiAnchor toggle;
        toggle.x1 = 10; toggle.y1 = 10; toggle.x2 = 200; toggle.y2 = 40;
        toggle.name = L"自动更新";
        toggle.role = L"复选框";
        toggle.action = L"toggle";
        toggle.state = { L"toggle:on" };
        const auto idx = BuildAiElementIndex({ toggle }, {}, 80);
        const std::wstring text = FormatAiElementIndex(idx, 0, 0, 100, 100, 1024, 576, 8000);
        const bool rendered = text.find(L"action:toggle") != std::wstring::npos
            && text.find(L"[toggle:on]") != std::wstring::npos
            && text.find(L"复选框") != std::wstring::npos;
        ok = ok && rendered;
        if (!rendered) detail += L" facts_not_rendered";
        // ★★「按编号点」这件事必须在**给模型看的标题里**说清（否则模型不知道有这个能力）
        const bool idHint = text.find(L"elementId") != std::wstring::npos;
        ok = ok && idHint;
        if (!idHint) detail += L" elementId_hint_missing";
    }

    // ④ ★按编号直查：命中、且坐标口径与按名字解析**同一套**（不然会整体偏移）
    {
        AiUiAnchor a;
        a.x1 = 100; a.y1 = 200; a.x2 = 300; a.y2 = 240;
        a.name = L"确定"; a.role = L"按钮"; a.action = L"click";
        AiUiAnchor b;
        b.x1 = 900; b.y1 = 700; b.x2 = 1000; b.y2 = 740;
        b.name = L"确定";   // 同屏两个同名（靠编号区分）——b 的名字不能被 a 覆盖
        b.role = L"按钮";
        b.action = L"click";
        const auto idx = BuildAiElementIndex({ a, b }, {}, 80);
        ok = ok && idx.size() == 2;
        const AiElementEntry* h1 = AiElementIndexById(idx, 1);
        const AiElementEntry* h2 = AiElementIndexById(idx, 2);
        const bool bothHit = h1 && h2;
        ok = ok && bothHit;
        if (!bothHit) detail += L" id_lookup_miss";
        if (bothHit) {
            // 两个同名条目必须给**不同**的坐标（这正是编号路径存在的理由：
            // 按名字会判歧义 → 回落整轮识图）
            const bool distinct = !(h1->centerX() == h2->centerX()
                && h1->centerY() == h2->centerY());
            ok = ok && distinct;
            if (!distinct) detail += L" same_id_same_point";
            // 坐标取该条目**中心**，与 AiElementIndexResolve 同一口径
            const bool centerOk = h1->centerX() == 200 && h1->centerY() == 220;
            ok = ok && centerOk;
            if (!centerOk) detail += L" id_center_wrong("
                + std::to_wstring(h1->centerX()) + L"," + std::to_wstring(h1->centerY()) + L")";
        }
    }

    // ⑤ ★查不到/不该给的编号一律 nullptr，**绝不兜底成近似条目**：
    //    编号错位必然点到隔壁（比回落识图糟得多）。
    {
        AiUiAnchor a;
        a.x1 = 100; a.y1 = 200; a.x2 = 300; a.y2 = 240;
        a.name = L"保存"; a.action = L"click";
        const auto idx = BuildAiElementIndex({ a }, {}, 80);
        const bool miss = AiElementIndexById(idx, 0) == nullptr
            && AiElementIndexById(idx, -1) == nullptr
            && AiElementIndexById(idx, 2) == nullptr
            && AiElementIndexById(idx, 999) == nullptr
            && AiElementIndexById({}, 1) == nullptr;
        ok = ok && miss;
        if (!miss) detail += L" id_lookup_fell_back";
    }

    // ⑥ ★窗口自身按钮与灰控件**永不入选**（与按名字解析同一把尺，docs §31.1）：
    //    「关闭」一击把整个游戏窗口关掉的那条路，不能从编号这条口子重新打开。
    {
        AiElementEntry chrome;
        chrome.id = 1;
        chrome.name = L"关闭";
        chrome.x1 = 2400; chrome.y1 = 0; chrome.x2 = 2460; chrome.y2 = 30;
        chrome.windowChrome = true;
        chrome.action = L"click";
        const std::vector<AiElementEntry> only = { chrome };
        const bool chromeBlocked = AiElementIndexById(only, 1) == nullptr;
        ok = ok && chromeBlocked;
        if (!chromeBlocked) detail += L" chrome_clickable_by_id";

        AiElementEntry gray;
        gray.id = 1;
        gray.name = L"下一步";
        gray.x1 = 10; gray.y1 = 10; gray.x2 = 100; gray.y2 = 40;
        gray.enabled = false;
        gray.action = L"click";
        const std::vector<AiElementEntry> grayOnly = { gray };
        const bool grayBlocked = AiElementIndexById(grayOnly, 1) == nullptr;
        ok = ok && grayBlocked;
        if (!grayBlocked) detail += L" gray_clickable_by_id";

        // 反向：同一个编号在**正常条目**上必须命中（防止上面两条把整个函数写成恒 nullptr 的假绿）
        AiElementEntry fine;
        fine.id = 1;
        fine.name = L"确定";
        fine.x1 = 10; fine.y1 = 10; fine.x2 = 100; fine.y2 = 40;
        fine.action = L"click";
        const std::vector<AiElementEntry> okList = { fine };
        const bool stillWorks = AiElementIndexById(okList, 1) != nullptr;
        ok = ok && stillWorks;
        if (!stillWorks) detail += L" id_lookup_always_null";
    }

    Emit(L"element_index_facts_and_id_binding", ok, detail.c_str());
}

// 「格」感知：把文字绑到规则网格的格子上 → 一张可查的键表（0 次 VLM）。
// 这是「按价格/编号/名字选第几张」的通用底座：卡片槽、工具栏、表格、看板都适用，
// 判据全是几何 + 文字，与领域无关。

// ★「落点在窗口非客户区」的几何判据（docs §41.1）：纯函数，用**实测那一局的真实矩形**断言。
//
// ⚠ 为什么必须有这条，而且**不能只靠 UIA**：那次日志里游戏窗口 `UIA 控件 0 条`
//   （`ElementFromPoint` 拿不到元素）⇒ `titleBarControl` 那道守卫**根本不会触发**；
//   而 `IsScreenPointOnForegroundWindow` 只回答「是不是这个窗口的」⇒ 标题栏照样放行。
//   客户区几何是纯 Win32 信息，**任何窗口都有**，游戏/自绘程序一样量得到。
void CaseNonClientLandingStrip() {
    bool ok = true;
    std::wstring detail;

    // 实测那一局：前台窗口 rect=(-9,0)-(2573,1390)；标准可调边框窗口的客户区
    // 在四边都内缩（标题栏 ~39px、左右/下边框各几像素，这里取整到实测可解释的值）。
    // ⚠ 第一版我只缩了上/右、把下边贴平，于是「下边框」那条断言量到的其实是客户区
    //   —— **夹具本身写歪了**，不是判据错。夹具要按真实窗口的形状写。
    const RECT wnd{ -9, 0, 2573, 1390 };
    const RECT cli{ -1, 39, 2565, 1382 };

    // ① ★原样坐标 (2522,16) → 标题栏按钮区 → 必须判为非客户区
    ok = ok && windowmode::IsPointInWindowNonClientStrip(2522, 16, wnd, cli);
    if (!windowmode::IsPointInWindowNonClientStrip(2522, 16, wnd, cli))
        detail += L" close_button_point_not_blocked_REGRESSION";

    // ② 第一次那个点 (2475,257) 在客户区 → 放行（别把正常落点一起拦掉）
    ok = ok && !windowmode::IsPointInWindowNonClientStrip(2475, 257, wnd, cli);
    if (windowmode::IsPointInWindowNonClientStrip(2475, 257, wnd, cli))
        detail += L" client_point_blocked";

    // ③ 下边框 / 左边框同理（两个方向都试，免得只对上边一类）
    ok = ok && windowmode::IsPointInWindowNonClientStrip(900, 1388, wnd, cli);
    if (!windowmode::IsPointInWindowNonClientStrip(900, 1388, wnd, cli))
        detail += L" bottom_border_not_blocked";
    ok = ok && windowmode::IsPointInWindowNonClientStrip(-5, 700, wnd, cli);
    if (!windowmode::IsPointInWindowNonClientStrip(-5, 700, wnd, cli))
        detail += L" left_border_not_blocked";

    // ④ 窗口**外面**的点不算「非客户区」（那是遮挡问题，由另一道守卫管）
    ok = ok && !windowmode::IsPointInWindowNonClientStrip(3000, 16, wnd, cli);
    if (windowmode::IsPointInWindowNonClientStrip(3000, 16, wnd, cli))
        detail += L" outside_window_flagged";

    // ⑤ ★无边框全屏（客户区 == 窗口矩形）：窗口内任何点都**不许**被拦 ——
    //    有些游戏是无边框窗口，游戏内自己的 X 就在右上角；拦了就是**减少能力**。
    const RECT fsWnd{ 0, 0, 2560, 1440 };
    ok = ok && !windowmode::IsPointInWindowNonClientStrip(2522, 16, fsWnd, fsWnd);
    if (windowmode::IsPointInWindowNonClientStrip(2522, 16, fsWnd, fsWnd))
        detail += L" borderless_ingame_close_blocked";

    Emit(L"non_client_landing_strip", ok, detail.c_str());
}

// 工具轮的「收束」判据（docs §42）：期待工具调用的轮次里，模型把整个输出预算写成散文
// （实测两轮各 45~55s、57 895 / 54 732 字节，**全在 content 里**）。旧判据绑死在输出
// 形态上（看门狗要 `contentBytes == 0`、纠偏要 `content.empty()`）⇒ 这个形态两条闸都看不见
// ⇒ 只能烧到 max_tokens 耗尽；而截断后那段散文还会被当成「本轮最终回答」返回
// ⇒ 用户看到的「选了两张卡然后卡在那里不动」。
void CaseStreamProseBrake() {
    bool ok = true;
    std::wstring detail;

    auto sig = [](bool inScope, size_t rBytes, size_t cBytes) {
        AiStreamBrakeSignals s;
        s.expectTools = true;
        s.inAiActionScope = inScope;
        s.gotAnyByte = true;
        s.elapsedMs = 45000;   // 未到 90s 绝对上限
        s.idleMs = 200;        // 字节一直在流（散文不是「停顿」）
        s.reasoningBytes = rBytes;
        s.contentBytes = cBytes;
        return s;
    };
    // 旧判据（逐字复刻，只为证明「这条用例在改之前是红的」）
    auto legacyBrake = [](const AiStreamBrakeSignals& s) {
        const bool hitAbs = s.elapsedMs >= kAiStreamNoToolForceMs;
        const bool hitPlateau = s.gotAnyByte && s.idleMs >= kAiStreamNoToolPlateauIdleMs;
        return s.expectTools && s.contentBytes == 0 && !s.sawUsableToolCalls
            && s.reasoningBytes > 0 && (hitAbs || hitPlateau);
    };

    // ① ★用户实测那一轮：57 895 字节**全在正文**、毫无工具意向 → 必须收束。
    //    同时断言旧判据在这一格是漏的（不然这条用例证明不了什么）。
    const AiStreamBrakeSignals prose = sig(true, 0, 57895);
    const AiStreamBrakeVerdict pv = AiDecideStreamBrake(prose);
    ok = ok && pv.kind == AiStreamBrakeKind::OutputBudget;
    if (pv.kind != AiStreamBrakeKind::OutputBudget) detail += L" prose_deluge_not_braked";
    ok = ok && !legacyBrake(prose);
    if (legacyBrake(prose)) detail += L" legacy_predicate_not_blind";
    ok = ok && pv.producedBytes == 57895 && !pv.reasoningDominated;
    if (pv.producedBytes != 57895) detail += L" produced_bytes_wrong";
    if (pv.reasoningDominated) detail += L" misattributed_to_reasoning";

    // ② 工具 JSON 已经在组装 / 已拿到可用工具调用 → 一律不拦（正在调工具）
    AiStreamBrakeSignals assembling = sig(true, 0, 20000);
    assembling.toolAssemblyStarted = true;
    ok = ok && AiDecideStreamBrake(assembling).kind == AiStreamBrakeKind::None;
    if (AiDecideStreamBrake(assembling).kind != AiStreamBrakeKind::None)
        detail += L" braked_while_assembling";
    AiStreamBrakeSignals got = sig(true, 0, 20000);
    got.sawUsableToolCalls = true;
    ok = ok && AiDecideStreamBrake(got).kind == AiStreamBrakeKind::None;
    if (AiDecideStreamBrake(got).kind != AiStreamBrakeKind::None)
        detail += L" braked_after_tool_calls";

    // ③ ★聊天助手不许被砍：同样 57 895 字节的散文在**非动作作用域**里就是用户要的长回答
    ok = ok && AiDecideStreamBrake(sig(false, 0, 57895)).kind == AiStreamBrakeKind::None;
    if (AiDecideStreamBrake(sig(false, 0, 57895)).kind != AiStreamBrakeKind::None)
        detail += L" braked_outside_action_scope";

    // ④ 非工具轮不拦
    AiStreamBrakeSignals noTools = sig(true, 0, 57895);
    noTools.expectTools = false;
    ok = ok && AiDecideStreamBrake(noTools).kind == AiStreamBrakeKind::None;
    if (AiDecideStreamBrake(noTools).kind != AiStreamBrakeKind::None)
        detail += L" braked_when_no_tools";

    // ⑤ 边界：预算闭区间下界（与其它门槛同口径）
    ok = ok && AiDecideStreamBrake(sig(true, 0, kAiStreamProseBudgetBytes)).kind
        == AiStreamBrakeKind::OutputBudget;
    if (AiDecideStreamBrake(sig(true, 0, kAiStreamProseBudgetBytes)).kind
        != AiStreamBrakeKind::OutputBudget) detail += L" budget_boundary_not_included";
    ok = ok && AiDecideStreamBrake(sig(true, 0, kAiStreamProseBudgetBytes - 1)).kind
        == AiStreamBrakeKind::None;
    if (AiDecideStreamBrake(sig(true, 0, kAiStreamProseBudgetBytes - 1)).kind
        != AiStreamBrakeKind::None) detail += L" braked_just_below_budget";

    // ⑥ 原闸①两条门槛逐字保留（只思考、没正文）
    AiStreamBrakeSignals abs1 = sig(true, 3000, 0);
    abs1.elapsedMs = kAiStreamNoToolForceMs;
    ok = ok && AiDecideStreamBrake(abs1).kind == AiStreamBrakeKind::NoToolCallTooLong;
    if (AiDecideStreamBrake(abs1).kind != AiStreamBrakeKind::NoToolCallTooLong)
        detail += L" reasoning_only_absolute_lost";
    AiStreamBrakeSignals plateau = sig(true, 3000, 0);
    plateau.elapsedMs = 1000;
    plateau.idleMs = kAiStreamNoToolPlateauIdleMs;
    ok = ok && AiDecideStreamBrake(plateau).kind == AiStreamBrakeKind::NoToolCallTooLong;
    if (AiDecideStreamBrake(plateau).kind != AiStreamBrakeKind::NoToolCallTooLong)
        detail += L" reasoning_only_plateau_lost";

    // ⑥-b ★★真机形状（docs §67）：**纯思考 14KB 不许被字节预算掐掉**。
    //   实测一次运行里 3 次 `已产出 14/14/15KB（以思考为主）→ 结束流式，逼它直接出手…`，
    //   紧跟着 `thinking=关闭` 连续 2 轮 ⇒ 模型只能给又短又浅的动作
    //   （一次毫无意义的 zoom、反复重放同一个鼠标批次）—— 用户说的「呆呆的」。
    //   管住「只想不干」的是**时间/停顿**（④⑤），不是思考字节数。
    {
        AiStreamBrakeSignals realThink = sig(true, 14336, 0);
        realThink.elapsedMs = 20000;      // 还没到 90s，也还没停顿 45s
        realThink.idleMs = 300;
        const bool thinkOk = AiDecideStreamBrake(realThink).kind == AiStreamBrakeKind::None;
        ok = ok && thinkOk;
        if (!thinkOk) detail += L" normal_reasoning_braked";
        // 但**失控的**思考仍必须被拦住（兜底上限），否则这条闸就废了
        AiStreamBrakeSignals runaway = sig(true, kAiStreamReasoningBudgetBytes, 0);
        runaway.elapsedMs = 20000;
        runaway.idleMs = 300;
        const bool runawayOk =
            AiDecideStreamBrake(runaway).kind == AiStreamBrakeKind::OutputBudget;
        ok = ok && runawayOk;
        if (!runawayOk) detail += L" runaway_reasoning_not_braked";
        // 正文那侧的 12KB 预算**照旧**（§42 的 57 895 字节散文必须被拦）
        AiStreamBrakeSignals contentOnly = sig(true, 0, kAiStreamProseBudgetBytes);
        contentOnly.elapsedMs = 20000;
        contentOnly.idleMs = 300;
        const bool contentOk =
            AiDecideStreamBrake(contentOnly).kind == AiStreamBrakeKind::OutputBudget;
        ok = ok && contentOk;
        if (!contentOk) detail += L" prose_budget_regressed";
    }

    // ⑦ 日志只报实测拆分，不许替模型认罪（§41.3）：正文为主就不许说「以思考为主」
    const std::wstring proseWhy = FormatAiStreamBrakeStatus(pv);
    ok = ok && proseWhy.find(L"以正文为主") != std::wstring::npos
        && proseWhy.find(L"以思考为主") == std::wstring::npos
        && proseWhy.find(L"57KB") != std::wstring::npos;
    if (proseWhy.find(L"以正文为主") == std::wstring::npos) detail += L" log_hid_the_split";
    if (proseWhy.find(L"以思考为主") != std::wstring::npos)
        detail += L" log_misattributed_to_reasoning";
    if (proseWhy.find(L"57KB") == std::wstring::npos) detail += L" log_missing_measured_kb";
    const std::wstring thinkWhy = FormatAiStreamBrakeStatus(AiDecideStreamBrake(sig(true, 40000, 0)));
    ok = ok && thinkWhy.find(L"以思考为主") != std::wstring::npos;
    if (thinkWhy.find(L"以思考为主") == std::wstring::npos)
        detail += L" reasoning_dominant_not_reported";

    Emit(L"stream_prose_brake", ok, detail.c_str());
}

// 「期待工具却只拿到散文」：这段文本不是回答（docs §42）。旧实现直接把它当本轮最终回答返回，
// AI 动作执行这一轮等于什么都没干；这条闸必须**有界**（守卫不能把模型永久锁在外面）。
void CaseNoToolCallAnswerGate() {
    bool ok = true;
    std::wstring detail;

    // ① 动作作用域 + 无工具调用 + 有文本 + 不是自然收尾（被截断/被收束）→ 逼它调工具
    const AiNoToolVerdict force = AiDecideNoToolCallAnswer(true, false, true, false, 0);
    ok = ok && force.action == AiNoToolAction::ForceToolCall && !force.why.empty();
    if (force.action != AiNoToolAction::ForceToolCall) detail += L" prose_accepted_as_answer";
    if (force.why.empty()) detail += L" no_reason";

    // ② ★有界：纠偏用满就认输，绝不与重生成互相喂成新死循环
    const AiNoToolVerdict giveUp = AiDecideNoToolCallAnswer(true, false, true, false, 2);
    ok = ok && giveUp.action == AiNoToolAction::AcceptAnswer;
    if (giveUp.action != AiNoToolAction::AcceptAnswer) detail += L" unbounded_nudging";
    const AiNoToolVerdict lastChance = AiDecideNoToolCallAnswer(true, false, true, false, 1);
    ok = ok && lastChance.action == AiNoToolAction::ForceToolCall;
    if (lastChance.action != AiNoToolAction::ForceToolCall) detail += L" nagged_too_early";

    // ③ 模型自己收尾（finish_reason=stop）→ 尊重它，别硬逼（可能是它真做完了）
    ok = ok && AiDecideNoToolCallAnswer(true, false, true, true, 0).action
        == AiNoToolAction::AcceptAnswer;
    if (AiDecideNoToolCallAnswer(true, false, true, true, 0).action
        != AiNoToolAction::AcceptAnswer) detail += L" natural_stop_overridden";

    // ④ 聊天助手（mustAct=false）与「本轮已有工具调用」都不干预
    ok = ok && AiDecideNoToolCallAnswer(false, false, true, false, 0).action
        == AiNoToolAction::AcceptAnswer;
    if (AiDecideNoToolCallAnswer(false, false, true, false, 0).action
        != AiNoToolAction::AcceptAnswer) detail += L" chat_answer_broken";
    ok = ok && AiDecideNoToolCallAnswer(true, true, true, false, 0).action
        == AiNoToolAction::AcceptAnswer;
    if (AiDecideNoToolCallAnswer(true, true, true, false, 0).action
        != AiNoToolAction::AcceptAnswer) detail += L" tool_calls_case_broken";

    // ⑤ 一点文本都没有 → 交给既有错误路径（别在这里造新措辞）
    ok = ok && AiDecideNoToolCallAnswer(true, false, false, false, 0).action
        == AiNoToolAction::AcceptAnswer;
    if (AiDecideNoToolCallAnswer(true, false, false, false, 0).action
        != AiNoToolAction::AcceptAnswer) detail += L" empty_output_nudged";

    Emit(L"no_tool_call_answer_gate", ok, detail.c_str());
}

// 思考失控闸的状态**必须跨轮存活**（docs §39.2）：它原来是 `SendMessage` 的局部变量，
// 而 AI 动作执行每外层轮重进一次 `SendMessage`（日志里内层「第 N 轮耗时」反复从 1 开始）
// ⇒ 计数器每轮归零 ⇒「连续 2 轮慢才关思考」永远不成立。
// 实测第十八份日志：单轮等模型 16.2s → 62.3s，闸门一次没触发 = 用户说的「半天没反应」。
// ★★另一条（§51）：**宾语是「想很久且没换来动作」，不是「墙钟久」** —— 出手了就不计数。
void CaseSlowThinkingRoundGate() {
    bool ok = true;
    std::wstring detail;

    // ① 快轮清零；不计数
    const AiSlowRoundVerdict fast = AiDecideSlowThinkingRound(5, 900, true, true, false);
    ok = ok && !fast.suppress && fast.newStreak == 0;
    if (fast.suppress || fast.newStreak != 0) detail += L" fast_round_wrong";

    // ② 偏慢：第 1 轮只累计，第 2 轮才触发（跨轮状态由调用方带回）
    const AiSlowRoundVerdict s1 = AiDecideSlowThinkingRound(0, 13000, true, true, false);
    ok = ok && !s1.suppress && s1.newStreak == 1;
    if (s1.suppress || s1.newStreak != 1) detail += L" slow_first_wrong";
    const AiSlowRoundVerdict s2 = AiDecideSlowThinkingRound(s1.newStreak, 13000, true, true, false);
    ok = ok && s2.suppress && s2.suppressRounds == kAiThinkingSuppressRounds
        && s2.newStreak == 0;
    if (!s2.suppress) detail += L" slow_second_not_suppressed";
    if (s2.suppressRounds != kAiThinkingSuppressRounds) detail += L" suppress_rounds_wrong";
    if (s2.newStreak != 0) detail += L" streak_not_reset_after_fire";

    // ③ ★单轮超长（≥30s）**当场**处置 —— 实测一轮就能 60s，「连续 2 轮」= 先白等 120s
    const AiSlowRoundVerdict very = AiDecideSlowThinkingRound(0, 62296, true, true, false);
    ok = ok && very.suppress && very.suppressRounds == kAiThinkingSuppressRounds;
    if (!very.suppress) detail += L" very_slow_not_suppressed";
    // 边界：刚好 30s 也算（闭区间下界，与表里别的门槛同口径）
    ok = ok && AiDecideSlowThinkingRound(0, kAiVerySlowRoundMs, true, true, false).suppress;
    if (!AiDecideSlowThinkingRound(0, kAiVerySlowRoundMs, true, true, false).suppress)
        detail += L" boundary_not_included";

    // ④ 条件不满足时不计数、也不清 0（关掉思考后模型本来就慢，拿它计数会变成
    //    「永久关思考」的正反馈；聊天助手的长思考是用户要的）
    const AiSlowRoundVerdict noThink = AiDecideSlowThinkingRound(1, 60000, false, true, false);
    ok = ok && !noThink.suppress && noThink.newStreak == 1;
    if (noThink.suppress || noThink.newStreak != 1) detail += L" counted_when_thinking_off";
    const AiSlowRoundVerdict chat = AiDecideSlowThinkingRound(1, 60000, true, false, false);
    ok = ok && !chat.suppress && chat.newStreak == 1;
    if (chat.suppress || chat.newStreak != 1) detail += L" counted_outside_action_scope";

    // ⑤ 触发时理由必须可读（进日志给排查用），且必须说清是「没有工具调用」而不是单纯的慢
    ok = ok && !very.why.empty() && very.why.find(L"关闭思考") != std::wstring::npos;
    if (very.why.empty()) detail += L" no_reason";
    ok = ok && very.why.find(L"没有任何工具调用") != std::wstring::npos;
    if (very.why.find(L"没有任何工具调用") == std::wstring::npos) detail += L" why_missing_object";

    // ⑥ ★★出手了就不算绕圈（实测：11.9s/16.1s 两轮都在放大读图，被误判成思考失控，
    //    关思考后模型把旧工具批次原样重放 ⇒ 游戏里多点了同一处）。
    //    ① 慢但出手 → 不计数、不抑制；
    const AiSlowRoundVerdict actedSlow = AiDecideSlowThinkingRound(1, 20000, true, true, true);
    ok = ok && !actedSlow.suppress && actedSlow.newStreak == 0;
    if (actedSlow.suppress) detail += L" acted_slow_suppressed";
    if (actedSlow.newStreak != 0) detail += L" acted_slow_kept_streak";
    //    ② 连续多轮都出手 → 永远不触发（哪怕每轮都 ≥30s）；
    ok = ok && !AiDecideSlowThinkingRound(0, 60000, true, true, true).suppress;
    if (AiDecideSlowThinkingRound(0, 60000, true, true, true).suppress)
        detail += L" acted_very_slow_suppressed";
    //    ③ 隔一轮出手 → 旧计数必须被清掉，不能让「慢—出手—慢」凑出误伤。
    const AiSlowRoundVerdict mixed1 = AiDecideSlowThinkingRound(0, 13000, true, true, false);
    const AiSlowRoundVerdict mixed2 =
        AiDecideSlowThinkingRound(mixed1.newStreak, 13000, true, true, true);
    const AiSlowRoundVerdict mixed3 =
        AiDecideSlowThinkingRound(mixed2.newStreak, 13000, true, true, false);
    ok = ok && !mixed3.suppress && mixed3.newStreak == 1;
    if (mixed3.suppress || mixed3.newStreak != 1) detail += L" acted_did_not_reset_streak";

    Emit(L"slow_thinking_round_gate", ok, detail.c_str());
}

// 识图「回答有效性」闸（docs §38.4）：模型答不出来时会回一个**空框** `[0,0,0,0]`，
// 而旧解析里 `TryParseBoundingBox` 对空框返回 false → 退回坐标对解析抓到 (0,0)，
// 把「没回答」当成了「图左上角这个点」：点屏幕 (0,0)、报「界面已经变化 → 已生效」、
// 还把垃圾存成定位模板。实测就发生在一局游戏里。
void CaseVisionAnswerGate() {
    bool ok = true;
    std::wstring detail;

    int x1 = -1, y1 = -1, x2 = -1, y2 = -1;

    // ① 空框：**必须**判成「没回答」，绝不能退化成点 (0,0)
    const AiVisionAnswerKind emptyBox =
        ParseVisionLocateAnswer(L"[0,0,0,0]", x1, y1, x2, y2);
    ok = ok && emptyBox == AiVisionAnswerKind::NoAnswer;
    if (emptyBox != AiVisionAnswerKind::NoAnswer) detail += L" empty_box_treated_as_answer";
    // 单边退化也算空框（宽 0 或高 0 都撑不出一个目标）
    ok = ok && ParseVisionLocateAnswer(L"[100,20,100,80]", x1, y1, x2, y2)
            == AiVisionAnswerKind::NoAnswer;
    if (ParseVisionLocateAnswer(L"[100,20,100,80]", x1, y1, x2, y2)
        != AiVisionAnswerKind::NoAnswer) detail += L" zero_width_box_accepted";
    ok = ok && ParseVisionLocateAnswer(L"(300,50,340,50)", x1, y1, x2, y2)
            == AiVisionAnswerKind::NoAnswer;
    if (ParseVisionLocateAnswer(L"(300,50,340,50)", x1, y1, x2, y2)
        != AiVisionAnswerKind::NoAnswer) detail += L" zero_height_box_accepted";

    // ② 正常框：照旧解析，且角点顺序反了也要归一
    const AiVisionAnswerKind box =
        ParseVisionLocateAnswer(L"[563,158,598,196]", x1, y1, x2, y2);
    ok = ok && box == AiVisionAnswerKind::Box && x1 == 563 && y1 == 158 && x2 == 598 && y2 == 196;
    if (box != AiVisionAnswerKind::Box || x2 != 598) detail += L" normal_box_broken";
    ok = ok && ParseVisionLocateAnswer(L"[598,196,563,158]", x1, y1, x2, y2)
            == AiVisionAnswerKind::Box && x1 == 563 && y1 == 158;
    if (x1 != 563) detail += L" reversed_box_not_normalized";

    // ③ 单点回答**不受影响**（Zoom 精炼级本来就是问点）：`(500, 95)` 这种必须照旧可用。
    //    ⚠ 别为了拦空框把「点」这条路一起封掉 —— 那会让每次识图都失败。
    const AiVisionAnswerKind pt = ParseVisionLocateAnswer(L"(500, 95)", x1, y1, x2, y2);
    ok = ok && pt == AiVisionAnswerKind::Point && x1 == 500 && y1 == 95 && x2 == 500;
    if (pt != AiVisionAnswerKind::Point || x1 != 500 || y1 != 95) detail += L" point_broken";
    // 图像原点这个**单点**回答不能顺带被拦（真实目标可能就在左边缘）
    ok = ok && ParseVisionLocateAnswer(L"(0, 0)", x1, y1, x2, y2) == AiVisionAnswerKind::Point;
    if (ParseVisionLocateAnswer(L"(0, 0)", x1, y1, x2, y2) != AiVisionAnswerKind::Point)
        detail += L" origin_point_rejected";

    // ④ 带描述前缀 / 无括号兜底：行为与旧解析一致（不能因为新闸把正常回答判死）
    ok = ok && ParseVisionLocateAnswer(L"第2个按钮在 [100,200,300,400]", x1, y1, x2, y2)
            == AiVisionAnswerKind::Box && x1 == 100 && x2 == 300;
    if (x1 != 100) detail += L" prefixed_box_broken";
    ok = ok && ParseVisionLocateAnswer(L"NOT_FOUND", x1, y1, x2, y2)
            == AiVisionAnswerKind::NoAnswer;
    if (ParseVisionLocateAnswer(L"NOT_FOUND", x1, y1, x2, y2)
        != AiVisionAnswerKind::NoAnswer) detail += L" notfound_not_noanswer";

    Emit(L"vision_answer_gate", ok, detail.c_str());
}

void CaseResolveAppLaunchTarget() {
    bool ok = true;
    std::wstring detail;

    // ① 明确存在的路径 → 原样返回，来源 path
    {
        wchar_t winDir[MAX_PATH]{};
        GetWindowsDirectoryW(winDir, MAX_PATH);
        const std::wstring notepad = std::wstring(winDir) + L"\\System32\\notepad.exe";
        const AppLaunchTarget t = ResolveAppLaunchTarget(notepad);
        ok = ok && t.ok() && t.source == L"path";
        if (!t.ok()) detail += L" path_miss";
        else if (t.source != L"path") detail += L" path_src(" + t.source + L")";
    }

    // ② exe 别名仍走 App Paths/PATH（记事本一定在），且结果**必须是存在的文件**
    {
        const AppLaunchTarget t = ResolveAppLaunchTarget(L"notepad");
        ok = ok && t.ok();
        if (!t.ok()) detail += L" notepad_miss";
        else {
            const bool isFile =
                GetFileAttributesW(t.path.c_str()) != INVALID_FILE_ATTRIBUTES;
            ok = ok && isFile;
            if (!isFile) detail += L" notepad_not_file";
            // 记事本是系统 exe：不该被判成「需要 shell 解释的快捷方式」
            ok = ok && !LooksLikeShellLaunchTarget(t.path);
            if (LooksLikeShellLaunchTarget(t.path)) detail += L" notepad_lnk_misjudged";
        }
    }

    // ③ 快捷方式识别：.lnk 必须被判成 shell 目标（游戏入口几乎都是快捷方式），exe 不是
    {
        ok = ok && LooksLikeShellLaunchTarget(L"C:\\x\\植物大战僵尸融合版.lnk")
            && LooksLikeShellLaunchTarget(L"c:\\x\\a.URL")
            && !LooksLikeShellLaunchTarget(L"C:\\x\\a.exe")
            && !LooksLikeShellLaunchTarget(L"");
        if (!LooksLikeShellLaunchTarget(L"C:\\x\\a.lnk")) detail += L" lnk_not_detected";
        if (LooksLikeShellLaunchTarget(L"C:\\x\\a.exe")) detail += L" exe_false_positive";
    }

    // ④ 找不到就**明确失败**（绝不静默退化）：随机名字必须 ok()==false
    {
        const AppLaunchTarget t = ResolveAppLaunchTarget(
            L"绝不存在的应用名ZZZQ_9f3a71");
        ok = ok && !t.ok();
        if (t.ok()) detail += L" phantom_resolved(" + t.path + L")";
        const AppLaunchTarget empty = ResolveAppLaunchTarget(L"   ");
        ok = ok && !empty.ok();
        if (empty.ok()) detail += L" empty_resolved";
    }

    // ⑤ 装饰后缀/扩展名会被剥掉后**仍能**匹配（模型常写「XX快捷方式」「XX图标」）
    //    这里只在有快捷方式的环境里做冒烟：没有就跳过（不假装测过）
    {
        const AppLaunchTarget plain = ResolveAppLaunchTarget(L"notepad");
        const AppLaunchTarget decorated = ResolveAppLaunchTarget(L"notepad 程序");
        const bool smoke = plain.ok() && decorated.ok()
            && plain.path == decorated.path;
        if (plain.ok() && !smoke) detail += L" decor_suffix_changed_result";
        ok = ok && smoke;
    }

    Emit(L"resolve_app_launch_target", ok, detail.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;    for (int i = 1; i < argc; ++i) {
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
    CaseCompleteTaskWrapUpVisible();
    CaseResolveSystemPathAndSavePathGuard();
    CaseSaveDialogSubmitGuard();
    CaseSwitchWindowAfterLaunchNotBlocked();
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
    CaseSnapshotReportsCoverageAndPaging();
    CaseScrollWheelNotBlockedWhenViewportHasContent();
    CaseListFirstNoLongerBlocks();
    CaseWebBrowseAllowsVisionFallback();
    CaseWebMixedPrefersTreeClick();
    CaseObservePageToolsPresent();
    CaseSearchOnPageTool();
    CaseClickRefNavigatesSpaceHref();
    CaseSamePageClickNotBlocked();
    CaseSearchOnPageOpensMatchingSpace();
    CaseObserveResultDefaults();
    CaseWaitRequestsObserve();
    CaseQuickInputClearPolicy();
    CaseAtomicQuickInputEmpty();
    CaseAtomicKeyClickEnter();
    CaseNonUniversalShortcutGuard();
    CaseAllowNestedUnderCap();
    CaseRejectNestedAtCap();
    CasePlanGateRemoved();
    CaseLocateTargetLengthGuard();
    CaseHistorySidebarAndBusyGuards();
    CaseGameForegroundVisionAllowed();
    CaseLocateMultiTargets();
    CaseNoBatchSelectSpecialCase();
    CaseOcrDirectClickPick();
    CaseLocateDecimalCoordParse();
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
    CaseLocateRetryHardCap();
    CaseListOcrHeuristic();
    CaseLogicConvertWritebackFirstTime();
    CaseLogicConvertPreferredBlockNameAndHealCreate();
    CaseLogicConvertWritebackPromote();
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
    CaseColorMatchScorePercent();
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
    CaseResolveVisionAmbiguityNotClaimedWhenImpossible();
    CaseMacroDebugLogFileRoundTrip();
    CaseResolveVisionSrcPixels();
    CaseResolveVisionRejectOob();
    CaseResolveAgentPointerInImageAsPixels();
    CaseResolveAgentPointerOobAsNorm1000();
    CaseResolveAgentPointerRejectOob();
    CaseActionVerbExpansion();
    CaseResolveOpenWebpageTarget();
    CaseNeedScreenCaptureForInteractionVerbs();
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
    CaseDomFirstActionGate();
    CasePickSnapshotRefForInput();
    CaseTypeByLabelTool();
    CaseUiControlTools();
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
    CaseFuseLocateCandidates();
    CaseJudgeLocateConfidence();
    CaseSplitVisionCandidateLines();
    CaseBitmapLowFeature();
    CaseOcrTextVerify();
    CaseDecideTable();
    CaseDecideWrapperEquivalence();
    CaseDecideDiagnosticLog();
    CaseVisionGateTraceOnly();
    CaseAiExecObserveGuards();
    CaseOcrTextIndexRows();
    CaseThinkingDowngradePlumbing();
    CaseFrameClickMarkLifecycle();
    CaseZoomRegionTool();
    CaseZoomTempImageFiles();
    CaseElementIndexAndResolve();
    CaseElementIndexFactsAndIdBinding();
    CaseLabelIconPairing();
    CaseMouseClickRepeatSamePoint();
    CaseMouseDragExecutes();
    CaseCoordActionReceipt();
    CaseResolveAppLaunchTarget();
    CaseNonClientLandingStrip();
    CaseVisionAnswerGate();
    CaseSlowThinkingRoundGate();
    CaseStreamProseBrake();
    CaseNoToolCallAnswerGate();
    CasePlanSpendTargetContract();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
