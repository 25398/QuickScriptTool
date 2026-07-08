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
};

struct AgentConversationRecord {
    AgentConversationMeta meta;
    std::vector<ChatMessage> messages;
    std::wstring chatDisplay;
};

struct AgentConversationSavePayload {
    bool shouldSave = false;
    std::wstring id;
    std::wstring name;
    std::wstring createdTime;
    int roundCount = 0;
    std::vector<ChatMessage> messages;
    std::wstring chatDisplay;
};

std::wstring AgentConversationsDir();
void EnsureAgentConversationsDir();

bool LoadAgentConversationList(std::vector<AgentConversationMeta>& out);
bool LoadAgentConversationRecord(const std::wstring& id, AgentConversationRecord& out);
bool SaveAgentConversation(const AgentConversationSavePayload& payload);
bool DeleteAgentConversation(const std::wstring& id);

std::wstring SummarizeConversationName(const std::vector<ChatMessage>& messages);
/// Cursor 风格：首条用户消息截断为标题（trim、首行、100 字上限；过短则「新对话」）
std::wstring DeriveConversationTitleFromPrompt(const std::wstring& prompt);
int CountConversationRounds(const std::vector<ChatMessage>& messages);
