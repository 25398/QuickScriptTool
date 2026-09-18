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

1. Tick 由 1s 定时驱动 — 按**秒**匹配，勿要求毫秒精确。产品 Web 壳里 headless 引擎窗是隐藏的，**不能只靠该窗 `SetTimer`/`WM_TIMER`**（会被 coalescing 饿死，间隔任务表现为启用了却永远不跑）。实际 Tick 来自可见主窗 `kStatusTimerId` + Timer Queue 投递 `WM_APP_SCHEDULED_TICK`。  
2. 同一秒内一次 Tick 须触发**所有**到期任务。  
3. Agent create/update/delete 须持久化并通知主窗 `Reload`。  
4. `createScheduledTask` 需要真实 `targetFile`；weekly 要 `weekDays`；custom 要日期字段；interval 要大于 0 的时长（`hour`/`minute`/`second`）。  
5. **自检禁止写产品 `scheduled_tasks.json`**：`TickAt` 对 Custom 会 `Save()`；自检须 `SetScheduledTasksFilePathForTest` 隔离到 `scheduled_tasks.selftest.json`。
6. 专业模式把脚本拖进子文件夹后，任务 `filePath` 仍可能是旧根路径。运行时须 `ResolveLibraryScriptPath`（按文件名在 `scripts/` / `recordings/` 子树找回）；`Reload` 会改写并落盘；拖拽当时应 `RetargetScheduledTaskFilePaths`。中文文件夹名须用 Win32 完整路径判定（勿只用 `std::filesystem::weakly_canonical`，可能带 `\\?\` 前缀导致“文件在库外”）。嵌套 `runMacro` / `mousePlayback` 同理。

## 相关路径

- 自检：`tools/scheduled_task_selftest.cpp`  
- 逻辑：`src/scheduled_task_types.cpp`, `scheduled_task_scheduler.cpp`, `scheduled_task_store.cpp`  
- Agent 工具：`src/agent_tools.cpp`
