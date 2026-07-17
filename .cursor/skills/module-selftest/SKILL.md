---
name: module-selftest
description: >-
  QuickScriptTool 模块自检总索引。改窗口模式/定时任务/宏变量/脚本构建/坐标/脚本IO/
  找图引擎/AI 路由/设置库/主题弹窗布局，或用户报 找图/绑窗/定时不触发/宏条件错/Agent 写坏动作/
  主题窗裁切字号时：读本 skill，选 suite，MSBuild Release 目标，跑 --json，exit 0 前不宣称修好。
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
| `window` | `WindowModeSelfTest` | 绑窗/后台输入/启动参数/IME/找图挂窗口模式 | [window-mode-debug](../window-mode-debug/SKILL.md) · [reference](../window-mode-debug/reference.md) | `src/window_mode/**` |
| `scheduled` | `ScheduledTaskSelfTest` | 定时不触发、Tick、weekDays、Agent 定时 CRUD | [scheduled-task-debug](../scheduled-task-debug/SKILL.md) · [reference](../scheduled-task-debug/reference.md) | `scheduled_task_*.cpp`, `agent_tools.cpp` |
| `macro_variables` | `MacroVariablesSelfTest` | `{var}` 条件、找图时限、转义、loop/goto | 下表 FAIL→文件 | `src/macro_variables.cpp` |
| `script_action_builder` | `ScriptActionBuilderSelfTest` | Agent 写宏坏、endLoop、缺 stopMacro、type 拒识 | 下表 FAIL→文件 | `src/script_action_builder.cpp`, `action_tree.h` |
| `coord_space` | `CoordSpaceSelfTest` | 换分辨率、coordMeta、n* 归一化、找图 scale | 下表 | `src/coord_space.cpp` |
| `script_io` | `ScriptIoSelfTest` | 脚本存读丢字段、录制路径归类、坏 JSON | 下表 | `src/script_io.cpp` |
| `image_match` | `ImageMatchSelfTest` | 阈值清零、NMS、金字塔/分数量化、冷冻位图找模板 | 下表 | `src/image_match*.cpp` / `image_match_internal.h` |
| `ai_action_router` | `AiActionRouterSelfTest` | Vision/Click/Tool/多轮分类、坐标映射、点击 JSON | 下表 | `src/ai_action_router.cpp` |
| `app_settings_store` | `AppSettingsStoreSelfTest` | 设置存读、theme/preview clamp、坏文件 | 下表 | `src/app_settings_store.cpp` |
| `theme_ui` | `ThemeUiSelfTest` | 自定义主题/取色弹窗裁切、字号、随机色可用性 | 下表 | `src/theme_ui_layout.h`, `src/app_theme.cpp` |
| `recorder` | `RecorderSelfTest`（产物 `QstRecorderLogicTest.exe`） | 录制排序/转换/时间轴/调度器 | 下表 | `src/recorder*.cpp`, `src/input_timeline_scheduler.cpp` |

### 仍偏手工（无 exe）

连点 / OCR / Agent 对话·附件·热键观感、以及「拖拽闪烁」等时序观感 — 见 `docs/comprehensive-test-cases.md`。布局裁切/字号已由 `ThemeUiSelfTest` 覆盖。

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
| `build_move_mouse_relative` | `BuildScriptActionFromJson` (`moveMouseRelative`) |
| `inter_repeat_interval_*` | `ShouldWaitAfterRepeat` / `ActionUsesInterRepeatInterval`（`action_utils`） |

### 重复间隔语义（脚本动作，非连点器）

`mouseClick` / `keyClick` / `hotkeyShortcut` / `quickInput` / `scrollWheel` / `mousePlayback`：

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
| `recording_path_*` | `IsRecordingScriptPath` / `RecordingsDir` |
| `breakout_*` | `NormalizeBreakoutTimeSeconds` / `EffectiveBreakoutTimeSeconds` |
| `parse_action_*` | `ParseScriptActionBlock` |
| `parse_move_mouse_relative_*` / `write_move_mouse_relative_*` | `ParseScriptActionBlock` / `WriteActionJson` |
| `save_load_*` / `load_*` / `parse_truncated_*` | `SaveScriptFileData` / `LoadScriptFileData` / `ParseScriptContent` |
| `write_action_json_*` | `ScriptActionToJsonString` |

### ImageMatchSelfTest

| name | 优先查看 |
|------|----------|
| `normalize_match_*` | `NormalizeMatchVarResult` |
| `match_center_*` / `click_point_*` | `FindImageMatchCenter` / `FindImageClickPoint` |
| `pyramid_*` / `score_*` / `threshold01_*` | `image_match_internal.h` |
| `nms_*` | `GlobalNms` |
| `frozen_bitmap_*` | `FindTemplateInFrozenScreenMulti` |

### AiActionRouterSelfTest

| name | 优先查看 |
|------|----------|
| `route_*` | `ClassifyAiActionRoute` / `IsAiAction*Prompt` |
| `parse_coord_*` | `TryParseCoordinatePair` |
| `map_api_point_*` | `MapApiPointToScreen` |
| `build_click_json_*` | `BuildScreenClickActionsJson` |
| `vision_system_prompt_*` | `BuildAiActionVisionQuerySystemPrompt` |
| `route_label_*` | `AiActionRouteLabel` |

### AppSettingsStoreSelfTest

| name | 优先查看 |
|------|----------|
| `default_*` / `settings_path_*` | `DefaultAppSettings` / `AppSettingsFilePath` |
| `load_missing_*` / `load_garbage_*` | `LoadAppSettings` |
| `save_load_*` / `theme_id_*` / `custom_theme_*` / `wm_preview_*` | `SaveAppSettings` / clamp in load |

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

### RecorderSelfTest

| name | 优先查看 |
|------|----------|
| `event_sort_timestamp_sequence` | `SortRecordedEvents` |
| `relative_delta_conserved` / `mixed_capture_channels` | `ConvertRecordedEventsToActions` |
| `same_timestamp_button_order` / `stop_hotkey_tail_trimmed` | `ConvertRecordedEventsToActions` |
| `same_timestamp_relative_merge` | `ConvertRecordedEventsToActions`（同戳相对包合并） |
| `wheel_gap_becomes_wait` | `ConvertRecordedEventsToActions`（滚轮前等待） |
| `compile_integer_timeline` / `legacy_wait_timeline` | `CompileInputTimeline` |
| `random_duration_rejects_timeline` | `CompileInputTimeline`（`randomDuration` 禁用精密轴） |
| `scheduler_cancel_interrupts` / `scheduler_wait_until_elapsed` | `PrecisionInputTimeline` |

## 相关

- 仓库短路由：[`AGENTS.md`](../../../AGENTS.md)
- 自检源码：`tools/*_selftest.cpp`
