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
    if (m.internal_nudge)
        j["internal_nudge"] = true;
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
    if (j.contains("internal_nudge"))
        m.internal_nudge = j["internal_nudge"].get<bool>();
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

// 会话 id 会直接拼进文件路径：只允许安全字符，从存储层兜底防路径穿越
// （bridge 各分支的校验不一致，peek/busy 分支曾漏检）。
bool IsSafeConversationId(const std::wstring& id) {
    if (id.empty() || id.size() > 64) return false;
    for (wchar_t c : id) {
        if ((c >= L'0' && c <= L'9')
            || (c >= L'a' && c <= L'z')
            || (c >= L'A' && c <= L'Z')
            || c == L'-' || c == L'_') {
            continue;
        }
        return false;
    }
    return true;
}

std::wstring ExtractFirstUserPrompt(const std::vector<ChatMessage>& messages) {
    for (const auto& m : messages) {
        if (m.role != L"user" || m.internal_nudge) continue;
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
        if (m.role == L"user" && !m.internal_nudge) ++rounds;
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
            if (item.contains("folder"))
                meta.folder = NormalizeRelativeFolder(FromUtf8(item["folder"].get<std::string>()));
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
    if (!IsSafeConversationId(id)) return false;
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
        if (root.contains("draft"))
            out.draft = FromUtf8(root["draft"].get<std::string>());
        if (root.contains("attachments") && root["attachments"].is_array()) {
            for (const auto& p : root["attachments"]) {
                if (p.is_string())
                    out.attachmentPaths.push_back(FromUtf8(p.get<std::string>()));
            }
        }
        if (root.contains("editIndex") && root["editIndex"].is_number_integer())
            out.editIndex = root["editIndex"].get<int>();
        return true;
    } catch (...) {
        return false;
    }
}

bool SaveAgentConversation(const AgentConversationSavePayload& payload) {
    if (!payload.shouldSave || !IsSafeConversationId(payload.id)) return false;
    EnsureAgentConversationsDir();

    std::wstring title = payload.name.empty() ? L"新对话" : payload.name;
    const std::wstring path = ConversationFilePath(payload.id);
    const int rounds = payload.roundCount > 0
        ? payload.roundCount
        : CountConversationRounds(payload.messages);

    json root;
    root["id"] = ToUtf8(payload.id);
    root["name"] = ToUtf8(title);
    root["createdTime"] = ToUtf8(payload.createdTime);
    root["roundCount"] = rounds;
    root["chatDisplay"] = ToUtf8(payload.chatDisplay);
    json msgs = json::array();
    for (const auto& m : payload.messages) msgs.push_back(SerializeMessage(m));
    root["messages"] = msgs;
    root["draft"] = ToUtf8(payload.draft);
    root["editIndex"] = payload.editIndex;
    json atts = json::array();
    for (const auto& p : payload.attachmentPaths) atts.push_back(ToUtf8(p));
    root["attachments"] = atts;

    {
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        const std::string utf8 = root.dump(2);
        file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }

    std::vector<AgentConversationMeta> list;
    LoadAgentConversationList(list);
    std::wstring keepFolder;
    for (const auto& item : list) {
        if (item.id == payload.id) {
            keepFolder = item.folder;
            break;
        }
    }
    list.erase(std::remove_if(list.begin(), list.end(),
        [&](const AgentConversationMeta& m) { return m.id == payload.id; }), list.end());
    // 尚无用户轮次：只留磁盘草稿，不进对话列表（开了窗却没说话不应出现「新对话」）
    if (rounds > 0) {
        AgentConversationMeta meta;
        meta.id = payload.id;
        meta.name = title;
        meta.createdTime = payload.createdTime;
        meta.roundCount = rounds;
        meta.filePath = path;
        meta.folder = keepFolder;
        list.push_back(std::move(meta));
        std::sort(list.begin(), list.end(), [](const AgentConversationMeta& a, const AgentConversationMeta& b) {
            return a.createdTime > b.createdTime;
        });
    }

    return SaveAgentConversationIndex(list);
}

bool SaveAgentConversationIndex(const std::vector<AgentConversationMeta>& list) {
    EnsureAgentConversationsDir();
    json index;
    json arr = json::array();
    for (const auto& item : list) {
        arr.push_back({
            {"id", ToUtf8(item.id)},
            {"name", ToUtf8(item.name)},
            {"createdTime", ToUtf8(item.createdTime)},
            {"roundCount", item.roundCount},
            {"file", ToUtf8(item.filePath)},
            {"folder", ToUtf8(item.folder)}
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

bool SetAgentConversationName(const std::wstring& id, const std::wstring& name) {
    if (!IsSafeConversationId(id) || name.empty()) return false;
    AgentConversationRecord rec;
    if (!LoadAgentConversationRecord(id, rec)) return false;
    AgentConversationSavePayload payload;
    payload.shouldSave = true;
    payload.id = id;
    payload.name = name;
    payload.createdTime = rec.meta.createdTime;
    payload.roundCount = rec.meta.roundCount > 0
        ? rec.meta.roundCount
        : CountConversationRounds(rec.messages);
    payload.messages = std::move(rec.messages);
    payload.chatDisplay = rec.chatDisplay;
    payload.draft = rec.draft;
    payload.attachmentPaths = rec.attachmentPaths;
    payload.editIndex = rec.editIndex;
    return SaveAgentConversation(payload);
}

bool SaveAgentConversationDraft(const std::wstring& id, const std::wstring& draft,
    const std::vector<std::wstring>& attachmentPaths, int editIndex) {
    if (!IsSafeConversationId(id)) return false;
    AgentConversationRecord rec;
    const bool loaded = LoadAgentConversationRecord(id, rec);
    const int rounds = loaded
        ? (rec.meta.roundCount > 0 ? rec.meta.roundCount : CountConversationRounds(rec.messages))
        : CountConversationRounds(rec.messages);
    bool draftBlank = true;
    for (wchar_t c : draft) {
        if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') {
            draftBlank = false;
            break;
        }
    }
    const bool emptyDraft = draftBlank && attachmentPaths.empty() && editIndex < 0;
    if (rounds <= 0 && emptyDraft) {
        DeleteAgentConversation(id);
        return true;
    }
    AgentConversationSavePayload payload;
    payload.shouldSave = true;
    payload.id = id;
    payload.name = loaded && !rec.meta.name.empty() ? rec.meta.name : L"新对话";
    payload.createdTime = loaded && !rec.meta.createdTime.empty()
        ? rec.meta.createdTime : NowText();
    payload.roundCount = rounds;
    payload.messages = std::move(rec.messages);
    payload.chatDisplay = rec.chatDisplay;
    payload.draft = draft;
    payload.attachmentPaths = attachmentPaths;
    payload.editIndex = editIndex;
    return SaveAgentConversation(payload);
}

bool DeleteAgentConversation(const std::wstring& id) {
    if (!IsSafeConversationId(id)) return false;
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
            {"file", ToUtf8(item.filePath)},
            {"folder", ToUtf8(item.folder)}
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

bool SetAgentConversationFolder(const std::wstring& id, const std::wstring& folder) {
    if (!IsSafeConversationId(id) || !IsSafeRelativeFolder(folder)) return false;
    std::vector<AgentConversationMeta> list;
    if (!LoadAgentConversationList(list)) return false;
    bool found = false;
    const std::wstring norm = NormalizeRelativeFolder(folder);
    for (auto& m : list) {
        if (m.id == id) {
            m.folder = norm;
            found = true;
            break;
        }
    }
    if (!found) return false;
    EnsureLibraryKindDir(L"ai");
    if (!norm.empty() && !EnsureRelativeFolder(LibraryKindDir(L"ai"), norm)) return false;
    return SaveAgentConversationIndex(list);
}
