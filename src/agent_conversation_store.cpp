#include "agent_conversation_store.h"

#include "utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace {

using json = nlohmann::json;

json SerializeMessage(const ChatMessage& m) {
    json j;
    j["role"] = ToUtf8(m.role);
    j["content"] = ToUtf8(m.content);
    if (!m.reasoning_content.empty())
        j["reasoning_content"] = ToUtf8(m.reasoning_content);
    if (m.requires_reasoning_content)
        j["requires_reasoning_content"] = true;
    if (!m.tool_call_id.empty())
        j["tool_call_id"] = ToUtf8(m.tool_call_id);
    if (!m.tool_name.empty())
        j["tool_name"] = ToUtf8(m.tool_name);
    if (!m.tool_args.empty())
        j["tool_args"] = ToUtf8(m.tool_args);
    if (!m.tool_calls.empty()) {
        json tcs = json::array();
        for (const auto& tc : m.tool_calls) {
            tcs.push_back({
                {"id", ToUtf8(tc.id)},
                {"name", ToUtf8(tc.name)},
                {"arguments", ToUtf8(tc.arguments)}
            });
        }
        j["tool_calls"] = tcs;
    }
    return j;
}

ChatMessage DeserializeMessage(const json& j) {
    ChatMessage m;
    if (j.contains("role")) m.role = FromUtf8(j["role"].get<std::string>());
    if (j.contains("content")) m.content = FromUtf8(j["content"].get<std::string>());
    if (j.contains("reasoning_content"))
        m.reasoning_content = FromUtf8(j["reasoning_content"].get<std::string>());
    if (j.contains("requires_reasoning_content"))
        m.requires_reasoning_content = j["requires_reasoning_content"].get<bool>();
    if (j.contains("tool_call_id"))
        m.tool_call_id = FromUtf8(j["tool_call_id"].get<std::string>());
    if (j.contains("tool_name"))
        m.tool_name = FromUtf8(j["tool_name"].get<std::string>());
    if (j.contains("tool_args"))
        m.tool_args = FromUtf8(j["tool_args"].get<std::string>());
    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        for (const auto& tc : j["tool_calls"]) {
            ToolCallRecord rec;
            if (tc.contains("id")) rec.id = FromUtf8(tc["id"].get<std::string>());
            if (tc.contains("name")) rec.name = FromUtf8(tc["name"].get<std::string>());
            if (tc.contains("arguments")) rec.arguments = FromUtf8(tc["arguments"].get<std::string>());
            m.tool_calls.push_back(std::move(rec));
        }
    }
    return m;
}

std::wstring ConversationFilePath(const std::wstring& id) {
    return AgentConversationsDir() + L"\\" + id + L".json";
}

std::wstring ExtractFirstUserPrompt(const std::vector<ChatMessage>& messages) {
    for (const auto& m : messages) {
        if (m.role != L"user") continue;
        std::wstring text = Trim(m.content);
        if (text.empty() && !m.parts.empty()) {
            for (const auto& p : m.parts) {
                if (p.type == L"text" && !Trim(p.text).empty()) {
                    text = Trim(p.text);
                    break;
                }
            }
        }
        if (!text.empty()) return text;
    }
    return {};
}

std::wstring CollapsePromptWhitespace(std::wstring text) {
    std::wstring out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (wchar_t ch : text) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == L' ') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(L' ');
            pendingSpace = false;
        }
        out.push_back(ch);
    }
    return Trim(out);
}

std::wstring FirstPromptLine(std::wstring text) {
    const size_t pos = text.find_first_of(L"\r\n");
    if (pos != std::wstring::npos) text = text.substr(0, pos);
    return Trim(text);
}

bool IsBriefConversationPrompt(const std::wstring& text) {
    if (text.empty()) return true;
    if (text.size() < 3) return true;
    if (text.front() == L'/' && text.find(L' ') == std::wstring::npos) return true;
    return false;
}

std::wstring TruncateConversationTitle(std::wstring text, size_t maxChars) {
    if (text.size() <= maxChars) return text;
    size_t cut = maxChars;
    while (cut > 0 && cut < text.size()
        && ((text[cut - 1] >= 0xD800 && text[cut - 1] <= 0xDBFF)
            || (text[cut] >= 0xDC00 && text[cut] <= 0xDFFF))) {
        --cut;
    }
    if (cut == 0) return text.substr(0, maxChars);
    return text.substr(0, cut) + L"…";
}

constexpr size_t kConversationTitleMaxChars = 100;

}  // namespace

std::wstring DeriveConversationTitleFromPrompt(const std::wstring& prompt) {
    std::wstring text = FirstPromptLine(CollapsePromptWhitespace(prompt));
    if (IsBriefConversationPrompt(text)) return L"新对话";
    return TruncateConversationTitle(std::move(text), kConversationTitleMaxChars);
}

std::wstring SummarizeConversationName(const std::vector<ChatMessage>& messages) {
    const std::wstring prompt = ExtractFirstUserPrompt(messages);
    if (prompt.empty()) return L"新对话";
    return DeriveConversationTitleFromPrompt(prompt);
}

std::wstring AgentConversationsDir() {
    return AppDir() + L"\\agent_conversations";
}

void EnsureAgentConversationsDir() {
    CreateDirectoryW(AgentConversationsDir().c_str(), nullptr);
}

int CountConversationRounds(const std::vector<ChatMessage>& messages) {
    int rounds = 0;
    for (const auto& m : messages) {
        if (m.role == L"user") ++rounds;
    }
    return rounds;
}

bool LoadAgentConversationList(std::vector<AgentConversationMeta>& out) {
    out.clear();
    EnsureAgentConversationsDir();
    const std::wstring indexPath = AgentConversationsDir() + L"\\index.json";
    const std::wstring content = ReadAll(indexPath);
    if (content.empty()) return true;

    try {
        const json root = json::parse(ToUtf8(content));
        if (!root.contains("conversations") || !root["conversations"].is_array()) return true;
        for (const auto& item : root["conversations"]) {
            AgentConversationMeta meta;
            if (item.contains("id")) meta.id = FromUtf8(item["id"].get<std::string>());
            if (item.contains("name")) meta.name = FromUtf8(item["name"].get<std::string>());
            if (item.contains("createdTime"))
                meta.createdTime = FromUtf8(item["createdTime"].get<std::string>());
            if (item.contains("roundCount")) meta.roundCount = item["roundCount"].get<int>();
            if (item.contains("file")) meta.filePath = FromUtf8(item["file"].get<std::string>());
            if (meta.id.empty()) continue;
            if (meta.filePath.empty()) meta.filePath = ConversationFilePath(meta.id);
            out.push_back(std::move(meta));
        }
    } catch (...) {
        return false;
    }
    std::sort(out.begin(), out.end(), [](const AgentConversationMeta& a, const AgentConversationMeta& b) {
        return a.createdTime > b.createdTime;
    });
    return true;
}

bool LoadAgentConversationRecord(const std::wstring& id, AgentConversationRecord& out) {
    out = {};
    if (id.empty()) return false;
    const std::wstring path = ConversationFilePath(id);
    const std::wstring content = ReadAll(path);
    if (content.empty()) return false;

    try {
        const json root = json::parse(ToUtf8(content));
        out.meta.id = id;
        if (root.contains("name")) out.meta.name = FromUtf8(root["name"].get<std::string>());
        if (root.contains("createdTime"))
            out.meta.createdTime = FromUtf8(root["createdTime"].get<std::string>());
        if (root.contains("roundCount")) out.meta.roundCount = root["roundCount"].get<int>();
        out.meta.filePath = path;
        if (root.contains("chatDisplay"))
            out.chatDisplay = FromUtf8(root["chatDisplay"].get<std::string>());
        if (root.contains("messages") && root["messages"].is_array()) {
            for (const auto& jm : root["messages"]) {
                out.messages.push_back(DeserializeMessage(jm));
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool SaveAgentConversation(const AgentConversationSavePayload& payload) {
    if (!payload.shouldSave || payload.id.empty()) return false;
    EnsureAgentConversationsDir();

    std::wstring title = payload.name;
    const std::wstring path = ConversationFilePath(payload.id);
    {
        const std::wstring existing = ReadAll(path);
        if (!existing.empty()) {
            try {
                const json prev = json::parse(ToUtf8(existing));
                if (prev.contains("name")) {
                    const std::wstring prevName = FromUtf8(prev["name"].get<std::string>());
                    if (!prevName.empty()) title = prevName;
                }
            } catch (...) {}
        }
    }

    json root;
    root["id"] = ToUtf8(payload.id);
    root["name"] = ToUtf8(title);
    root["createdTime"] = ToUtf8(payload.createdTime);
    root["roundCount"] = payload.roundCount;
    root["chatDisplay"] = ToUtf8(payload.chatDisplay);
    json msgs = json::array();
    for (const auto& m : payload.messages) msgs.push_back(SerializeMessage(m));
    root["messages"] = msgs;

    {
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        const std::string utf8 = root.dump(2);
        file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }

    std::vector<AgentConversationMeta> list;
    LoadAgentConversationList(list);
    bool found = false;
    for (auto& item : list) {
        if (item.id == payload.id) {
            if (item.name.empty()) item.name = title;
            item.createdTime = payload.createdTime;
            item.roundCount = payload.roundCount;
            item.filePath = path;
            found = true;
            break;
        }
    }
    if (!found) {
        AgentConversationMeta meta;
        meta.id = payload.id;
        meta.name = title;
        meta.createdTime = payload.createdTime;
        meta.roundCount = payload.roundCount;
        meta.filePath = path;
        list.push_back(std::move(meta));
    }
    std::sort(list.begin(), list.end(), [](const AgentConversationMeta& a, const AgentConversationMeta& b) {
        return a.createdTime > b.createdTime;
    });

    json index;
    json arr = json::array();
    for (const auto& item : list) {
        arr.push_back({
            {"id", ToUtf8(item.id)},
            {"name", ToUtf8(item.name)},
            {"createdTime", ToUtf8(item.createdTime)},
            {"roundCount", item.roundCount},
            {"file", ToUtf8(item.filePath)}
        });
    }
    index["conversations"] = arr;

    const std::wstring indexPath = AgentConversationsDir() + L"\\index.json";
    std::ofstream indexFile(indexPath, std::ios::binary);
    if (!indexFile) return false;
    const std::string indexUtf8 = index.dump(2);
    indexFile.write(indexUtf8.data(), static_cast<std::streamsize>(indexUtf8.size()));
    return true;
}

bool DeleteAgentConversation(const std::wstring& id) {
    if (id.empty()) return false;
    DeleteFileW(ConversationFilePath(id).c_str());

    std::vector<AgentConversationMeta> list;
    LoadAgentConversationList(list);
    list.erase(std::remove_if(list.begin(), list.end(),
        [&](const AgentConversationMeta& m) { return m.id == id; }), list.end());

    json index;
    json arr = json::array();
    for (const auto& item : list) {
        arr.push_back({
            {"id", ToUtf8(item.id)},
            {"name", ToUtf8(item.name)},
            {"createdTime", ToUtf8(item.createdTime)},
            {"roundCount", item.roundCount},
            {"file", ToUtf8(item.filePath)}
        });
    }
    index["conversations"] = arr;
    const std::wstring indexPath = AgentConversationsDir() + L"\\index.json";
    std::ofstream indexFile(indexPath, std::ios::binary);
    if (!indexFile) return false;
    const std::string indexUtf8 = index.dump(2);
    indexFile.write(indexUtf8.data(), static_cast<std::streamsize>(indexUtf8.size()));
    return true;
}
