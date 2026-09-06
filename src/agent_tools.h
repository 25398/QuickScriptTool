#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_tools.h — AI Agent 工具工厂函数声明
// 提供脚本读写、录制优化、定时任务管理、应用设置修改等工具
// ──────────────────────────────────────────────────────────────────

#include "agent_core.h"

// ── 脚本操作 ──────────────────────────────────────────────────────

/// 列出所有脚本和录制（名称、动作数量），同时搜索 scripts 和 recordings 目录
AgentTool MakeListScriptsTool();

/// 读取指定脚本或录制的内容（大文件返回摘要，避免完整 JSON 撑爆对话）
AgentTool MakeReadScriptTool();

/// 写入（覆盖）指定脚本或录制文件
AgentTool MakeWriteScriptTool();

/// 获取脚本的详细统计信息（动作分类、分段结构等）
AgentTool MakeGetScriptStatsTool();

/// 优化脚本：与产品录制优化同一套按关键动作分段的合并/压缩
AgentTool MakeOptimizeScriptTool();

/// 按手动编辑逻辑构建规范格式的脚本动作 JSON（禁止 AI 手写动作对象）
AgentTool MakeBuildScriptActionsTool();

/// 规划脚本动作树（嵌套 children），不保存；含循环/条件时先核对父子再构建
AgentTool MakePlanScriptActionsTool();

/// AI 动作执行专用：提交本批次要执行的宏动作（返回纯 JSON 数组，与 buildScriptActions 构建逻辑一致）
AgentTool MakeSubmitMacroActionsTool();

/// 创建鼠标宏（buildScriptActions + 保存到 scripts，并刷新主界面）
AgentTool MakeCreateMacroScriptTool();

/// 优化键鼠录制（默认 recordings 目录，并刷新主界面）
AgentTool MakeOptimizeRecordingTool();

/// 删除鼠标宏或键鼠录制（并刷新主界面）
AgentTool MakeDeleteScriptFileTool();

// ── 定时任务管理 ──────────────────────────────────────────────────

/// 列出所有定时任务
AgentTool MakeListScheduledTasksTool();

/// 创建新的定时任务
AgentTool MakeCreateScheduledTaskTool();

/// 更新已有的定时任务
AgentTool MakeUpdateScheduledTaskTool();

/// 删除定时任务
AgentTool MakeDeleteScheduledTaskTool();

// ── 应用设置修改 ──────────────────────────────────────────────────

/// 列出当前的应用设置（不包含 AI 助手自身的设置）
AgentTool MakeListSettingsTool();

/// 修改应用设置
AgentTool MakeUpdateSettingsTool();

// ── AI 动作专用工具 ───────────────────────────────────────────────

/// 列出已添加的 AI 模型及识图能力
AgentTool MakeListAiModelsTool();

/// 构建 getCursorPos 动作
AgentTool MakeBuildGetCursorPosActionTool();

/// 构建 aiTextAnalysis 动作（自动选择模型）
AgentTool MakeBuildAiTextAnalysisActionTool();

/// 构建 aiImageAnalysis 动作（自动选择识图模型）
AgentTool MakeBuildAiImageAnalysisActionTool();

/// 构建 aiActionExecute 动作（按是否带图选择模型）
AgentTool MakeBuildAiActionExecuteActionTool();

/// 注册 AI 助手全部工具（供 AgentDialog 复用）
std::vector<AgentTool> BuildDefaultAgentTools();
