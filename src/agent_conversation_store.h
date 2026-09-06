#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_conversation_store.h — AI 助手对话历史持久化
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

#include "agent_core.h"

struct AgentConversationMeta {
    std::wstring id;
    std::wstring name;
    std::wstring createdTime;
    int roundCount = 0;
    std::wstring filePath;
    /// 专业模式逻辑目录（相对 library/ai，空=未分类）
    std::wstring folder;
};

struct AgentConversationRecord {
    AgentConversationMeta meta;
    std::vector<ChatMessage> messages;
    std::wstring chatDisplay;
    /// 未发送草稿文本（退出重开仍保留）
    std::wstring draft;
    /// 未发送附件路径列表
    std::vector<std::wstring> attachmentPaths;
    /// 正在编辑的 user 消息序号（0 起；-1=未编辑）
    int editIndex = -1;
};

struct AgentConversationSavePayload {
    bool shouldSave = false;
    std::wstring id;
    std::wstring name;
    std::wstring createdTime;
    int roundCount = 0;
    std::vector<ChatMessage> messages;
    std::wstring chatDisplay;
    std::wstring draft;
    std::vector<std::wstring> attachmentPaths;
    int editIndex = -1;
};

std::wstring AgentConversationsDir();
void EnsureAgentConversationsDir();

bool LoadAgentConversationList(std::vector<AgentConversationMeta>& out);
bool LoadAgentConversationRecord(const std::wstring& id, AgentConversationRecord& out);
bool SaveAgentConversation(const AgentConversationSavePayload& payload);
/// 单独持久化会话草稿（未发送文本/附件/编辑态）。
/// 尚无用户轮次且草稿为空：不新建列表项；已误存的空会话则删除。
bool SaveAgentConversationDraft(const std::wstring& id, const std::wstring& draft,
    const std::vector<std::wstring>& attachmentPaths, int editIndex);
bool SaveAgentConversationIndex(const std::vector<AgentConversationMeta>& list);
bool SetAgentConversationName(const std::wstring& id, const std::wstring& name);
bool SetAgentConversationFolder(const std::wstring& id, const std::wstring& folder);
bool DeleteAgentConversation(const std::wstring& id);

std::wstring SummarizeConversationName(const std::vector<ChatMessage>& messages);
/// Cursor 风格：首条用户消息截断为标题（trim、首行、100 字上限；过短则「新对话」）
std::wstring DeriveConversationTitleFromPrompt(const std::wstring& prompt);
int CountConversationRounds(const std::vector<ChatMessage>& messages);
