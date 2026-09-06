---
name: module-selftest
description: >-
  QuickScriptTool 模块自检总索引。改窗口模式/定时任务/宏变量/脚本构建/坐标/脚本IO/
  找图引擎/AI 路由/设置库/主题弹窗布局/脱离冷却/停止热键，或用户报 找图/绑窗/定时不触发/宏条件错/Agent 写坏动作/
  主题窗裁切字号/按住键仍恢复宏/快捷键停止失灵时：读本 skill，选 suite，MSBuild Release 目标，跑 --json，exit 0 前不宣称修好。
  Prefer over guessing.
---

# Module SelfTest（Agent 总索引）

每模块一个独立 console exe。统一约定见下；专项逻辑见各 suite skill / reference。

## 一句话循环

```text
1) 选 suite  2) MSBuild /t:<Target> Release  3) 跑 exe --json
4) FAIL 的 name → 对照表改源码  5) 再编再跑直到 exit 0
```

## 硬性约定

| 项 | 约定 |
|----|------|
| CLI | `--json` / `--list` / `--help`（`WindowModeSelfTest` 另有 `--macro`） |
| stdout（`--json`） | UTF-8；每行 `{"name","ok","detail"}`；末行 `{"passed","failed","ok"}` |
| stdout（`--list`） | `name\twhen\tmeaning` 行 + 末行 `{"listed":N,"ok":true}` |
| exit | `0` = 全过；`N>0` = 失败数 |
| 产物 | `build\Release\<Target>.exe`（`RecorderSelfTest` 例外：`QstRecorderLogicTest.exe`，降低 360 HEUR 误删） |
| 宣称修好前 | 对应 suite `exit 0` |

构建模板（仓库根）：

```powershell
& "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:<Target> /m /v:minimal
# RecorderSelfTest 请跑：build\Release\QstRecorderLogicTest.exe --json
build\Release\<Target>.exe --json
```

PowerShell 不要用分号拼多个 `/t:A;B`（会拆成多条命令）；多个 target 请分开跑。
`RecorderSelfTest` 勿链 `script_core_common`（含 SendInput）；被 360 误删时先加信任区 `build\Release\`，再重编目标 `RecorderSelfTest`。

共享 Emit：`tools/selftest_harness.h`。CMake：`qst_add_selftest(...)`。

## Suite 索引

| id | Target / exe | 何时用 | Skill / reference | 核心源码 |
|----|--------------|--------|-------------------|----------|
| `window` | `WindowModeSelfTest` | 绑窗/后台输入/启动参数/IME/找图挂窗口模式；**AI 切窗台账**（`window_list_*` / `window_activate_foreground`） | [window-mode-debug](../window-mode-debug/SKILL.md) · [reference](../window-mode-debug/reference.md) | `src/window_mode/**`（切窗台账：`window_list.cpp`） |
| `scheduled` | `ScheduledTaskSelfTest` | 定时不触发、Tick、weekDays、Agent 定时 CRUD | [scheduled-task-debug](../scheduled-task-debug/SKILL.md) · [reference](../scheduled-task-debug/reference.md) | `scheduled_task_*.cpp`, `agent_tools.cpp` |
| `macro_variables` | `MacroVariablesSelfTest` | `{var}` 条件、找图时限、转义、loop/goto | 下表 FAIL→文件 | `src/macro_variables.cpp` |
| `script_action_builder` | `ScriptActionBuilderSelfTest` | Agent 写宏坏、endLoop、缺 stopMacro、type 拒识 | 下表 FAIL→文件 | `src/script_action_builder.cpp`, `action_tree.h` |
| `coord_space` | `CoordSpaceSelfTest` | 换分辨率、coordMeta、n* 归一化、找图 scale | 下表 | `src/coord_space.cpp` |
| `script_io` | `ScriptIoSelfTest` | 脚本存读丢字段、录制路径归类、坏 JSON | 下表 | `src/script_io.cpp` |
| `image_match` | `ImageMatchSelfTest` | 阈值清零、NMS、金字塔/分数量化、冷冻位图找模板 | 下表 | `src/image_match*.cpp` / `image_match_internal.h` |
| `ai_action_router` | `AiActionRouterSelfTest` | Vision/Click/Tool/多轮分类、坐标映射、点击 JSON | 下表 | `src/ai_action_router.cpp` |
| `agent_assistant` | `AgentAssistantSelfTest` | 对话编辑截断、撤销日志、白名单命令、文件工具、Skill 文件加载 | 下表 | `src/agent_undo.cpp`, `src/agent_shell.cpp`, `src/agent_core.cpp`, `tools/agent_assistant_selftest.cpp` |
| `app_settings_store` | `AppSettingsStoreSelfTest` | 设置存读、theme/preview clamp、坏文件 | 下表 | `src/app_settings_store.cpp` |
| `theme_ui` | `ThemeUiSelfTest` | 自定义主题/取色弹窗裁切、字号、随机色可用性 | 下表 | `src/theme_ui_layout.h`, `src/app_theme.cpp` |
| `breakout_cooldown` | `BreakoutCooldownSelfTest` | 默认模式脱离：按住不计冷却、松开后 idle、新输入重置 | 下表 | `src/breakout_cooldown.h`, `src/breakout_input.h` |
| `hotkey_stop` | `HotkeyStopSelfTest` | 运行中停止热键被吞/失灵、宏回放停不下来、键鼠假死 | 下表 | `src/hotkey_stop.h`, `src/engine/engine_hotkeys.cpp` |
| `recorder` | `RecorderSelfTest`（产物 `QstRecorderLogicTest.exe`） | 录制排序/转换/时间轴/调度器 | 下表 | `src/recorder*.cpp`, `src/input_timeline_scheduler.cpp` |
| `virtual_hid` | `VirtualHidSelfTest` | VirtualHid 键/相对/绝对/滚轮注入；进程被杀须抬起（驱动 FileCleanup） | 下表 | `src/input/virtual_hid.*`, `input_emergency_teardown.*`, `driver/qst_vhid/` |
| `injection` | `InjectionSelfTest` | 注入对抗测试：7 种注入技术 + PEB 隐藏 + XOR；`--inject <pid> <dll> <technique>` 驱动真实目标测试 | 见 `docs/anticheat-injection-testing.md` | `src/window_mode/injection/**`, `tools/injection_selftest.cpp`, `tools/injection_test_*.cpp` |

### 仍偏手工（无 exe）

连点 / OCR / Agent 对话·附件·热键观感、以及「拖拽闪烁」等时序观感 — 见 `docs/comprehensive-test-cases.md`。停止热键策略已由 `HotkeyStopSelfTest` 覆盖；布局裁切/字号已由 `ThemeUiSelfTest` 覆盖。

## FAIL → 源码

### MacroVariablesSelfTest

| name | 优先查看 |
|------|----------|
| `resolve_match_var_brace` | `ResolveMacroVariables` / `LookupMatchVarProperty` |
| `resolve_cur_loops` | `ResolveMacroVariables` (`ctrl:CurLoops()`) |
| `decode_quick_input_escapes` | `DecodeQuickInputEscapes` |
| `find_image_time_sec` | `ResolveFindImageTimeSec` |
| `condition_compare_and_or` | `EvaluateConditionExpr` / `ParseConditionParts` |
| `goto_step_from_literal` | `TryResolveGotoStepNo` |
| `loop_max_from_var` | `ResolveLoopMaxCount` |
| `unknown_var_no_recurse` | `ResolveMacroOperandImpl`（未知标识须返回空，禁止 `{t}` 递归） |
| `resolve_image_var_path` | `MacroVariableContext.imageVars` / `ResolveMacroOperandImpl` |
| `build_quick_input_image_var` | `BuildQuickInputVarItems`（findImage followUp=3） |
| `build_quick_input_fixed_vars` | `BuildQuickInputVarItems`（下拉仅 CurLoops/Random/Hour/Minute/Clipboard；`{Now}` 等魔法变量不进列表） |
| `resolve_ctrl_random` | `ResolveMacroVariables` / `ResolveMacroOperandImpl`（`ctrl:Random()`） |
| `resolve_ctrl_hour_minute` | `ResolveMacroOperandImpl`（`ctrl:Hour()` / `ctrl:Minute()`） |
| `resolve_ctrl_clipboard` | `ResolveMacroOperandImpl` / `ClipboardExpandMode`（`ctrl:Clipboard()`） |
| `user_var_beats_magic_name` | `ResolveMacroOperandImpl`（用户变量优先于无前缀魔法名） |

### ScriptActionBuilderSelfTest

| name | 优先查看 |
|------|----------|
| `build_wait_ok` | `BuildScriptActionFromJson` |
| `reject_custom_text` | `BuildScriptActionFromJson`（禁 customText） |
| `normalize_renumber` | `NormalizeScriptActionList` |
| `ensure_stop_macro_appends` | `EnsureStopMacroOnActions` |
| `ensure_stop_macro_skip_infinite` | `EnsureStopMacroOnActions` / `HasTopLevelInfiniteLoop` |
| `endloop_needs_parent` | `ValidateEndLoopPlacements` (`action_tree.h`) |
| `endloop_inside_loop_ok` | 同上 |
| `build_array_json` | `BuildScriptActionsJsonArray` |
| `flatten_children_loop_body` | `FlattenNestedActionParamList` / `BuildScriptActionsJsonArray` |
| `empty_loop_rejected` | `ValidateContainerBodies` (`action_tree.h`) |
| `flatten_if_else_inside_loop` | 同上（if/else 为 loop 子节点，分支再嵌套） |
| `plan_outline_shows_tree` | `PlanScriptActionsOutline` |
| `non_container_children_rejected` | `FlattenNestedActionParamList` |
| `fragment_loop_skips_tree_check` | `BuildScriptActionsJsonArray(..., validateContainerBodies=false)` |
| `indent_siblings_form_loop_body` | `ValidateContainerBodies`（indent=父级+1） |
| `build_move_mouse_relative` | `BuildScriptActionFromJson` (`moveMouseRelative`) |
| `inter_repeat_interval_*` | `ShouldWaitAfterRepeat` / `ActionUsesInterRepeatInterval`（`action_utils`；含 runMacro/runBlock） |
| `build_runblock_repeat_fields` | `BuildScriptActionFromJson` runBlock/runMacro 重复字段 |
| `build_mouseplayback_speed` | `BuildScriptActionFromJson` mousePlayback `playbackSpeed` |
| `disassemble_*` | `DisassembleActionAt`（`action_disassemble.h`） |
| `merge_*` | `MergeSelectedIntoContainer`（`action_assemble.h`） |
| `build_findimage_save_image` | `ParseFollowUpValue` / FindImage `saveImage`→3 |
| `build_quickinput_parse_escapes` | `BuildScriptActionFromJson` QuickInput `parseEscapes` |
| `resolve_key_nav_names` | `ResolveKeyVk`（Home/Up/Right/Delete 等须映射真 VK，禁止首字母回落） |
| `reject_unknown_keytext` | `ResolveKeyVk` / `BuildScriptActionFromJson`（未知多字符键名须拒绝） |

### 重复间隔语义（脚本动作，非连点器）

`mouseClick` / `keyClick` / `hotkeyShortcut` / `quickInput` / `scrollWheel` / `mousePlayback` / `runMacro` / `runBlock`：

- `clickCount` = 重复次数
- `duration` / `randomDuration` = **相邻两次之间**的间隔
- `clickCount=1`：完全不等待；不在第一次之前、最后一次之后插入等待
- 与 `wait` 动作的 `duration`（整段阻塞等待）不同；`quickInput.charInterval` 是字间间隔

Agent / `buildScriptActions` schema、`agent_reference`、工具描述须与此一致。

### CoordSpaceSelfTest

| name | 优先查看 |
|------|----------|
| `standard_meta_*` / `save_meta_*` / `exec_meta_*` | `StandardScriptCoordMeta` / `BuildScriptCoordMetaForSave` / `ScriptCoordMetaForExecution` |
| `coordmeta_json_*` / `has_coordmeta_*` | `WriteCoordMetaJson` / `ParseCoordMetaJson` / `HasCoordMetaJson` |
| `normalize_move_*` / `migrate_*` | `NormalizeActionCoords` / `MigrateLegacyScriptToNormalized` |
| `normalize_relative_skip` | `NormalizeActionCoords`（相对移动不得归一化） |
| `template_scale_*` / `exec_find_opts_*` | `ComputeTemplateScale` / `BuildExecutionFindImageOptions` |
| `resolve_click_point_*` | `ResolveFindImageClickPoint` |

### ScriptIoSelfTest

| name | 优先查看 |
|------|----------|
| `recording_path_*` | `IsRecordingScriptPath` / `RecordingsDir`（含正斜杠规范化） |
| `breakout_*` | `NormalizeBreakoutTimeSeconds` / `EffectiveBreakoutTimeSeconds` |
| `parse_action_*` | `ParseScriptActionBlock` |
| `parse_move_mouse_relative_*` / `write_move_mouse_relative_*` | `ParseScriptActionBlock` / `WriteActionJson` |
| `save_load_*` / `load_*` / `parse_truncated_*` | `SaveScriptFileData` / `LoadScriptFileData` / `ParseScriptContent` |
| `write_action_json_*` | `ScriptActionToJsonString` |
| `mouse_playback_speed_roundtrip` | `ParseScriptActionBlock` / `WriteActionJson` `playbackSpeed` |
| `write_norm_xy_keeps_pixel` | `WriteActionJson`（n* 须 max_digits10，禁默认 precision=6） |
| `write_norm_xy_keeps_pixel` | `WriteActionJson`（n* 须 max_digits10，禁默认 precision=6） |
| `parse_findimage_save_image` | `findImageFollowUp=3` / `imageUseVar` 读写 |
| `parse_quickinput_escapes` | `ParseScriptActionBlock` / `WriteActionJson` `parseEscapes` |
| `looks_like_file_path` | `LooksLikeFilePath` (`image_var_util`) |
| `window_relative_pixel_xy` | `ParseScriptActionBlock` `windowRelative` 像素路径 |
| `recording_keeps_window_relative_mode` | `SaveScriptFileData`：窗口相对录制不得清 `windowMode` |
| `recording_recovers_wm_from_rel_actions` | `SaveScriptFileData`：录制路径 `windowRelative` 动作 + 身份即使 `enabled=0` 也要复活 |
| `script_default_mode_not_revived` | `SaveScriptFileData`/`LoadScriptFileData`：`scripts` 路径明确 `enabled=0` 不得复活窗口模式 |
| `recording_wipes_editor_window_mode` | `SaveScriptFileData`：普通录制仍强制关残留 `windowMode` |
| `save_after_load_false_keeps_xy` | `SaveScriptFileData`：`Load(..., false)` 后改 wait 再存不得冲掉 n* |

### ImageMatchSelfTest

| name | 优先查看 |
|------|----------|
| `normalize_match_*` | `NormalizeMatchVarResult` |
| `match_center_*` / `click_point_*` | `FindImageMatchCenter` / `FindImageClickPoint` / `FindImageRelativeClickOffset` |
| `pyramid_*` / `score_*` / `threshold01_*` | `image_match_internal.h` |
| `nms_*` | `GlobalNms` |
| `frozen_bitmap_*` | `FindTemplateInFrozenScreenMulti` |
| `perfect_match_*` | `MatchPerfectPixel`（完美匹配像素终审） |

### AiActionRouterSelfTest

| name | 优先查看 |
|------|----------|
| `route_*` | `ClassifyAiActionRoute` / `IsAiAction*Prompt` / `HasActionVerb` |
| `route_single_click_composite` | 「单击」须归 CompositeClick（勿落识图问答） |
| `need_screen_scroll_yes` | `AiActionPromptLikelyNeedsScreenCapture`（滑动/滚轮/拖拽/移鼠要首帧截图） |
| `click_intent_*` | `PromptIntendsDoubleClick` / `PromptIntendsRightClick` |
| `composite_target_phrase_stripped` | `ExtractClickTargetPhrase`（剥动作前缀、截逗号，喂 `locateAndClick`） |
| `parse_bbox_leading_number` / `parse_bbox_parens` | `TryParseBoundingBox`（优先解析方括号/圆括号数字组） |
| `vision_not_found_variants` | `IsVisionLocateNotFound` |
| `usage_skill_*` | `MacroActionUsageSkill` / `LookupMacroActionSchema(usage)` |
| `agent_skill_*` | `MacroActionAgentSkill` / `LookupMacroActionSchema(agent)` |
| `run_action_recipe` | `MakeRunActionRecipeTool`（一次核对后信任模板全量复用） |
| `logic_convert_collapse_instance_data` | `CompileAiLogicConvert` / `CollapseInstanceDataEntries`（动态列表勿写死） |
| `logic_convert_per_locate_timers` | `CompileAiLogicConvert`（每找图独立 timer+if，禁整段共用计时器） |
| `logic_convert_compile_gate` | `CompileAiLogicConvert`（限时 findImage + 步间 Wait） |
| `reply_actions_buildable` | `BuildScriptActionsJsonArray`（quickInput+keyClick Enter） |
| `parse_coord_*` | `TryParseCoordinatePair` |
| `map_api_point_*` | `MapApiPointToScreen` |
| `build_click_json_*` | `BuildScreenClickActionsJson` |
| `vision_system_prompt_*` | `BuildAiActionVisionQuerySystemPrompt` |
| `route_label_*` | `AiActionRouteLabel` |
| `resolve_action_vision_fallback` | `ResolveActionAiModelName`（指定文本模型+需识图 → 自动换识图模型） |
| `resolve_vision_subtask_model` | `ResolveVisionSubtaskModelName`（非多模态主模型 → savedModels 识图模型） |
| `vision_route_model_routed` | `ModelSupportsVision` + `ResolveVisionSubtaskModelName`（带图路由判定） |
| `model_supports_vision` | `ModelSupportsVision`（多模态/文本模型名判定） |
| `adaptive_refine_gate` | `ShouldAcceptCoarseLocateWithoutRefine`（紧凑跳过二级 / 过小过大不跳） |
| `planner_observe_image_attach` | `ShouldAttachObserveImageToPlanner`（纯文本规划不附观察图） |
| `model_supports_vision` / `resolve_vision_subtask_*` | `ModelSupportsVision` / `ResolveVisionSubtaskModelName` |
| `task_data_cleared_on_reset` | `ResetAiActionSessionState` 须清空 `aiData`（防串上轮 historyRecords） |

### AppSettingsStoreSelfTest

| name | 优先查看 |
|------|----------|
| `default_*` / `settings_path_*` | `DefaultAppSettings` / `AppSettingsFilePath` |
| `load_missing_*` / `load_garbage_*` | `LoadAppSettings` |
| `save_load_*` / `theme_id_*` / `custom_theme_*` / `wm_preview_*` / `save_load_playback_speed` / `playback_speed_scale_math` / `save_load_home_ui_mode` | `SaveAppSettings` / clamp in load / `ScalePlaybackTimeSeconds` / `RecordingPlaybackTimeScale` |
| `save_load_scheduled_conflict_policy` | `scheduledTaskConflictPolicy` 存读 + clamp 0..1；`scheduledTaskAutoResume` 存读 |

### AgentAssistantSelfTest

| name | 优先查看 |
|------|----------|
| `truncate_history_*` | `AgentCore::TruncateHistoryToUserRound` |
| `undo_*` / `file_tools_*` | `src/agent_undo.cpp` |
| `shell_reject_*` / `shell_where_*` | `src/agent_shell.cpp` 白名单校验 |
| `skill_optimize_directs_to_tools` | `.cursor/skills/agent-optimize/SKILL.md` |
| `skill_script_tree_and_plan` | `.cursor/skills/agent-script/SKILL.md` / `MakePlanScriptActionsTool` |
| `write_script_rejects_empty_loop` | `ValidateContainerBodies`（writeScript 不得绕过空循环） |
| `recopt_*` | `src/recording_optimize_ops.cpp` 按关键动作分段合并（与产品对话框共用） |
| `clipboard_roundtrip` | `copyAgentTextToClipboard` / `pasteAgentClipboardText` |
| `conversation_draft_roundtrip` | `SaveAgentConversationDraft`（无轮次草稿不进列表） |
| `empty_conversation_not_listed` | 空会话关窗不得写入 `index.json` |
| `conversation_with_round_is_listed` | 有用户轮次仍进列表（防误伤） |

### ThemeUiSelfTest

| name | 优先查看 |
|------|----------|
| `font_design_*` | `theme_ui_layout.h`（须与 `settings_dialog` 字号一致） |
| `custom_layout_*` / `custom_footer_*` / `custom_action_*` | `MakeCustomThemeLayout` |
| `custom_label_width_*` | 标签 RECT 宽度 / `UiFontHeight(26)` |
| `color_picker_layout_*` / `color_picker_parts_*` | `MakeColorPickerLayout` |
| `random_main_color_*` | `RandomAttractiveThemeColors` / `MainColorLooksUsable` |
| `build_custom_theme_*` / `apply_custom_theme_*` | `BuildTheme` / `ApplyThemeFromSettings` |
| `theme_catalog_*` | `ThemeCatalog` / `kThemeCount` |

### BreakoutCooldownSelfTest

| name | 优先查看 |
|------|----------|
| `hold_blocks_cooldown` | `BreakoutCooldownStillWaiting`（按住不得 armed/结束） |
| `release_starts_idle` | 松开后才开始 idle，满时限才恢复 |
| `fresh_input_resets_idle` | 松开后的新输入重置倒计时 |
| `hold_tracker_*` / `reconcile_drops_released` | `BreakoutHoldTracker` |

### HotkeyStopSelfTest

| name | 优先查看 |
|------|----------|
| `busy_physical_down_stops` | `ShouldStopOnToggleKeyDown`（忙碌物理键必须停） |
| `busy_needkeyup_does_not_stop` | 启动键仍按着（自动连发）不得停 |
| `busy_injected_down_ignored` | 注入/ExtraInfo 不得当停止 |
| `idle_needkeyup_blocks_start` / `idle_physical_can_start` | `ShouldStartOnToggleKeyDown` |
| `physical_up_clears_latch` / `injected_up_does_not_clear` | `ShouldClearToggleLatchOnKeyUp` |
| `poller_*` | `TickPoller`（松手后再按才紧急停止） |
| `idle_fallback_*` / `toggle_consume_*` / `ll_owns_*` | `TickIdleStart` / `TryConsumeTogglePress`（全屏游戏 RegisterHotKey/LL 哑火时的空闲启动兜底） |
| `busy_stop_honors_emergency_if_consume_stuck` / `busy_stop_blocks_duplicate_without_emergency` | `AllowBusyToggleStop`（非管理员+提权游戏：启动次消费闩粘死时紧急停止仍须停） |

### RecorderSelfTest

| name | 优先查看 |
|------|----------|
| `event_sort_timestamp_sequence` | `SortRecordedEvents` |
| `relative_delta_conserved` / `mixed_capture_channels` | `ConvertRecordedEventsToActions` |
| `same_timestamp_button_order` / `stop_hotkey_tail_trimmed` | `ConvertRecordedEventsToActions` |
| `same_timestamp_relative_keep` | `ConvertRecordedEventsToActions`（同戳相对包不合并） |
| `repair_copy_leaves_source` | `RepairCompressedRelativeGaps`（只改执行副本） |
| `timeline_catchup_skips_stall` | `PrecisionInputTimeline::WaitDeltaUs`（小抖动过点追赶） |
| `timeline_large_stall_rebases` | `PrecisionInputTimeline::WaitDeltaUs`（大 stall 平移原点，禁止短等待连发） |
| `timeline_wait_until_rebases` | `WaitUntilElapsedUs` 过点后同样 rebase，避免只修入口路径 |
| `timeline_long_wait_wall` | 长等待用自适应切片，墙钟不得被压成瞬时 |
| `timeline_gap_from_now_no_catchup` | `WaitGapUs` 从此刻睡满（竞品 Delay 语义） |
| `wheel_gap_becomes_wait` | `ConvertRecordedEventsToActions`（滚轮前等待） |
| `compile_integer_timeline` / `legacy_wait_timeline` | `CompileInputTimeline` |
| `random_duration_rejects_timeline` | `CompileInputTimeline`（`randomDuration` 禁用精密轴） |
| `timing_us_prefers_over_duration` | `CompileInputTimeline` / `ActionStepUs` |
| `convert_gaps_become_waits` / `same_timestamp_no_wait` | `ConvertRecordedEventsToActions`（显式 Wait） |
| `timeline_fold_vs_explicit_equiv` | `CompileInputTimeline` 折叠 vs Wait 等价 |
| `expand_recording_keeps_gaps` / `expand_script_default_duration_no_wait` / `expand_idempotent` | `ExpandRecordingPreDelaysToExplicitWaits` |
| `wait_stats_use_timing_us` | `ActionStepUs` |
| `snap_timing_to_wait` / `convert_drops_approach_moves` | `recording_to_findimage`（前置 Wait） |
| `click_capture_rect_clamped` | `ComputeClickCaptureRect` |
| `hover_patch_covers_click` | `HoverPatchCoversClick`（图片定位按下前模板） |
| `convert_window_relative_findimage` | `MakeFindImageFromDown` 保留 `windowRelative` + 全客户区搜索 |
| `scheduler_cancel_interrupts` / `scheduler_wait_until_elapsed` | `PrecisionInputTimeline` |

### VirtualHidSelfTest

| name | 优先查看 |
|------|----------|
| `install_script_present` | `driver/qst_vhid/_elevate_install.ps1`（相对 exe 上溯仓库/发版目录） |
| `install_script_no_boot_policy` | 禁止 `bcdedit /set`、测试签名 on、HVCI、`shutdown`、固件重启、注册开机任务 |
| `install_script_no_root_certs` | 禁止 `import_certs.reg` / `certutil` 灌 Root |
| `install_script_no_driverstore_rm` | 禁止直接删 DriverStore 仓库目录 |
| `install_script_filter_after_start` | `FILTERS_ONLY_AFTER_RUNNING`：LowerFilters 仅在服务 Running 之后 |
| `product_no_firmware_reboot` | `qst_webview_shell.cpp` 禁止 `shutdown /r /fw` |
| `product_no_official_ic_exe_msg` | `hid_interception.cpp` 禁止引导用户跑 `install-interception.exe` |
| `release_pack_no_lab_installers` | `package_release.ps1` 禁止随包官方 IC 安装器 / `sign_and_install.ps1` |
| `device_open` | 驱动未装 / `_elevate_install.ps1` / 设备接口 GUID |
| `key_press_release` | `VirtualHidBackend::SendKey` / Report ID 1 / scan→HID |
| `mouse_rel_move` | Report ID 2 / `MoveRelative` |
| `mouse_abs_jump` | 仅更新内部状态（不发 Report ID 3）；桌面绝对由 SetCursorPos |
| `mouse_wheel` | Report ID 2 wheel/hwheel |
| `release_all_no_stick` | `ReleaseAll` / EndSession |

## 相关

- 仓库短路由：[`AGENTS.md`](../../../AGENTS.md)
- 自检源码：`tools/*_selftest.cpp`
