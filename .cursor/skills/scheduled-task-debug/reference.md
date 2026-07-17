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
| `custom_format_includes_year` | 自定义时间显示缺年 | `FormatScheduledRunTime` |
| `parse_bool_token` | `true` 子串误匹配 | `scheduled_task_store.cpp` ParseBool |
| `parse_global_disabled_true` | `globalDisabled` 未读 | `ParseScheduledTasksJson` |
| `drop_empty_filepath` | 空 `filePath` 未丢弃 | `ParseScheduledTasksJson` |

Agent CRUD / Reload：`src/agent_tools.cpp`（本 harness 不覆盖 UI 通知，改完后须人工或主程序路径确认 Reload）。
