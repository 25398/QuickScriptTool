---
name: scheduled-task-debug
description: >-
  Debug QuickScriptTool 定时任务 via ScheduledTaskSelfTest.exe. Use when: tasks
  never fire, weekDays wrong, custom once-fire, Agent create/update/delete
  missing Reload, or parse/globalDisabled bugs. Prefer --json loop over guessing.
---

# Scheduled Task Debug（Agent）

总索引：[module-selftest](../module-selftest/SKILL.md)  
用例对照：[reference.md](reference.md)

## 一句话怎么用

```text
1) 编 ScheduledTaskSelfTest  2) --json  3) FAIL name → reference.md  4) 再绿
```

## 构建 / 运行

```powershell
& "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:ScheduledTaskSelfTest /m /v:minimal
build\Release\ScheduledTaskSelfTest.exe --json
```

| 命令 | 作用 |
|------|------|
| `... --help` | 用法 |
| `... --list` | 用例名 + 说明 |
| `... --json` | Agent 默认；exit `0` = 全过 |

## 硬性约定

1. Tick 由 1s `SetTimer` 驱动 — 按**秒**匹配，勿要求毫秒精确。  
2. 同一秒内一次 Tick 须触发**所有**到期任务。  
3. Agent create/update/delete 须持久化并通知主窗 `Reload`。  
4. `createScheduledTask` 需要真实 `targetFile`；weekly 要 `weekDays`；custom 要日期字段。

## 相关路径

- 自检：`tools/scheduled_task_selftest.cpp`  
- 逻辑：`src/scheduled_task_types.cpp`, `scheduled_task_scheduler.cpp`, `scheduled_task_store.cpp`  
- Agent 工具：`src/agent_tools.cpp`
