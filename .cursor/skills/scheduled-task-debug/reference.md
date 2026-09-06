# ScheduledTaskSelfTest — 用例与源码对照

FAIL 的 `name` → 直接跳文件。

| name | 失败通常表示 | 优先查看 |
|------|--------------|----------|
| `second_resolution_ignores_ms` | 按毫秒匹配导致几乎永不触发 | `scheduled_task_scheduler.cpp` `ScheduledTaskShouldRun` |
| `hourly_match` | 每小时 minute/second 错 | 同上 |
| `weekly_weekday_bits` | Sun=0 vs UI bit0=Mon 映射错 | `scheduled_task_types.cpp` `SystemTimeWeekDayBit` / `SetWeekDay` |
| `gates_disabled_paused_empty` | 全局禁用/暂停/空路径仍开火 | `ScheduledTaskShouldRun` 门控 |
| `custom_once` | Custom 同秒重复或未标 `customFired` | `ScheduledTaskScheduler::TickAt` |
| `one_second_window` | 目标秒内不同 ms 不接受 | `ScheduledTaskShouldRun` |
| `tick_all_due_tasks` | 一秒内只打响一个任务 | `TickAt` 遍历 |
| `conflict_policy_matrix` | 忙时跳过/打断/排队/插入判定错；插入期间 stopMacro 误当成全局停止 | `scheduled_task_types.h` `DecideScheduledTaskFire` / `StopMacroShouldEndEntireRun`；引擎：`engine_script_run.cpp` 定时插入用 `scheduledYieldLocalStop`（不置 `stopFlag_`），循环/指令块/脱离须一起退出；插入须隔离原脚本的 goto/block 栈 |
| `custom_format_includes_year` | 自定义时间显示缺年 | `FormatScheduledRunTime` |
| `parse_bool_token` | `true` 子串误匹配 | `scheduled_task_store.cpp` ParseBool |
| `parse_global_disabled_true` | `globalDisabled` 未读 | `ParseScheduledTasksJson` |
| `drop_empty_filepath` | 空 `filePath` 未丢弃 | `ParseScheduledTasksJson` |
| `interval_format` | 间隔显示缺「每」或时长 | `FormatScheduledRunTime` / `ScheduledFrequencyLabel` |
| `interval_wait_then_fire` | 间隔启动即跑、同周期重复、漏 tick 连打 | `scheduled_task_scheduler.cpp` `IntervalDueLocked` |
| `interval_zero_never_fires` | 0 时长仍开火 | `ScheduledIntervalDurationMs` / `IntervalDueLocked` |
| `interval_reset_on_duration_change` | 改间隔后仍用旧原点 | `SyncIntervalClocksLocked` |
| `interval_preserve_clock_unrelated` | 无关 Reload/SetTasks 重置了间隔计时 | `SyncIntervalClocksLocked` |
| `interval_ignores_wall_clock` | 间隔按钟点匹配或 clamp 错 | `ScheduledTaskShouldRun` / `ClampScheduledFrequency` |
| `parse_interval_frequency` | `frequency:4` 被夹成自定义 | `scheduled_task_store.cpp` `ClampScheduledFrequency` |
| `interval_touch_resets_clock` | 保存后间隔计时未从保存时刻重锚 | `TouchIntervalClock` |
| `interval_global_disable_reload_resets` | 解除全局禁用后间隔任务集中补火 | `Reload` 解除禁用时清 `intervalClocks_` |

Agent CRUD / Reload：`src/agent_tools.cpp`（本 harness 不覆盖 UI 通知，改完后须人工或主程序路径确认 Reload）。
