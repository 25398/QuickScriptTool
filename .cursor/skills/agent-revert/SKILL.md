---
name: agent-revert
description: >-
  助手每次修改文件前记录快照，用户可随时恢复为修改前状态。
---

# 变更撤销 / 恢复

## 范围

所有落盘工具都接入变更日志：`writeScript` / `createMacroScript` /
`optimizeScript` / `optimizeRecording` / `deleteScriptFile` / 定时任务 CRUD /
`updateSettings` / `writeAgentFile`。

## 行为

- 每个修改一条记录：工具、标题、路径、修改前内容、修改后内容、状态。
- 日志上限 100 条，存 `AppDir()\agent_changes\journal.json`。
- `revertAgentChange` 恢复：把修改前内容写回；修改前文件不存在则删除。
- 每条只能恢复一次；恢复后脚本/录制列表自动刷新。

## 接口

`listAgentChanges` 查看；`revertAgentChange({"id":"<id>"})` 恢复。
前端“撤销”按钮（↺）弹出列表，点选即恢复。
