---
name: agent-conversation
description: >-
  编辑已发送消息并重发：只保留该消息之前的上下文，丢弃之后内容，把新文本当作该轮重新执行。
---

# 对话编辑与重发

## 语义

编辑任意历史 **user** 消息后重发：保留该条之前的上下文（含此前工具记录），
丢弃它之后的所有旧回复与工具结果。消息序号按 user 消息从 0 计（`rewindUserIndex`）。
编辑第 0 条时对话标题重置。忙绿时不可编辑。

## 交互

- user 气泡右上角“✎”回填原文；回车重发，Esc 取消。
- 前端发送前本地截断 `tab.messages` 并重绘；后端 `AgentCore::TruncateHistoryToUserRound`
  在 `BeginSendAgentMessage` 里同步截断，前后端一致。

## 实现

`src/agent_core.cpp`（截断）、`webview_bridge_backend.cpp`（`rewindUserIndex`）、
`ui/app.js`（编辑按钮/编辑态）。
