#pragma once
// =============================================================================
// agent_shell.h - AI 助手命令行 / 文件操作工具
// 白名单命令执行（git 只读、MSBuild 自检、自检 exe、where）+ 受限文件操作 + 剪贴板。
// =============================================================================

#include "agent_core.h"

/// 运行白名单内的命令行（git 只读 / MSBuild 自检 / 自检 exe / where）
AgentTool MakeRunAgentCommandTool();

/// 列出允许目录内的文件/子目录
AgentTool MakeListAgentDirectoryTool();

/// 读取允许目录内的文本文件（默认上限 64KB）
AgentTool MakeReadAgentFileTool();

/// 递归文本搜索（路径:行号: 内容）
AgentTool MakeSearchAgentFilesTool();

/// 写入允许目录内的文本文件（UTF-8，自动记入撤销日志）
AgentTool MakeWriteAgentFileTool();

/// 复制文本到剪贴板
AgentTool MakeCopyAgentTextToClipboardTool();

/// 读取剪贴板文本
AgentTool MakePasteAgentClipboardTextTool();
