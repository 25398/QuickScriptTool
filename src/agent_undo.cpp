#include "agent_undo.h"

#include "utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {

using json = nlohmann::json;

constexpr size_t kMaxJournalEntries = 100;

json EntryToJson(const AgentChangeEntry& e) {
    json j;
    j["id"] = ToUtf8(e.id);
    j["time"] = ToUtf8(e.time);
    j["tool"] = ToUtf8(e.tool);
    j["title"] = ToUtf8(e.title);
    j["path"] = ToUtf8(e.path);
    j["existed"] = e.existed;
    j["before"] = ToUtf8(e.before);
    j["after"] = ToUtf8(e.after);
    j["ok"] = e.ok;
    j["reverted"] = e.reverted;
    j["conversationId"] = ToUtf8(e.conversationId);
    return j;
}

AgentChangeEntry EntryFromJson(const json& j) {
    AgentChangeEntry e;
    if (j.contains("id")) e.id = FromUtf8(j["id"].get<std::string>());
    if (j.contains("time")) e.time = FromUtf8(j["time"].get<std::string>());
    if (j.contains("tool")) e.tool = FromUtf8(j["tool"].get<std::string>());
    if (j.contains("title")) e.title = FromUtf8(j["title"].get<std::string>());
    if (j.contains("path")) e.path = FromUtf8(j["path"].get<std::string>());
    if (j.contains("existed")) e.existed = j["existed"].get<bool>();
    if (j.contains("before")) e.before = FromUtf8(j["before"].get<std::string>());
    if (j.contains("after")) e.after = FromUtf8(j["after"].get<std::string>());
    if (j.contains("ok")) e.ok = j["ok"].get<bool>();
    if (j.contains("reverted")) e.reverted = j["reverted"].get<bool>();
    if (j.contains("conversationId"))
        e.conversationId = FromUtf8(j["conversationId"].get<std::string>());
    return e;
}

bool WriteJournal(const std::vector<AgentChangeEntry>& list) {
    CreateDirectoryW(AgentChangesDir().c_str(), nullptr);
    json root;
    json arr = json::array();
    for (const auto& e : list) arr.push_back(EntryToJson(e));
    root["changes"] = arr;
    const std::string utf8 = root.dump(2);
    std::ofstream file(AgentChangeJournalPath(), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return true;
}

bool LoadJournal(std::vector<AgentChangeEntry>& out) {
    out.clear();
    const std::wstring content = ReadAll(AgentChangeJournalPath());
    if (content.empty()) return true;
    try {
        const json root = json::parse(ToUtf8(content));
        if (!root.contains("changes") || !root["changes"].is_array()) return true;
        for (const auto& item : root["changes"]) {
            if (!item.is_object()) continue;
            out.push_back(EntryFromJson(item));
        }
    } catch (...) {
        return false;
    }
    std::sort(out.begin(), out.end(),
        [](const AgentChangeEntry& a, const AgentChangeEntry& b) {
            return a.time > b.time;
        });
    return true;
}

bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& content) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        const std::wstring dir = path.substr(0, slash);
        std::filesystem::create_directories(dir);
    }
    const std::string utf8 = ToUtf8(content);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return true;
}

}  // namespace

std::wstring AgentChangesDir() {
    return AppDir() + L"\\agent_changes";
}

std::wstring AgentChangeJournalPath() {
    return AgentChangesDir() + L"\\journal.json";
}

std::wstring AgentUndoBegin(const std::wstring& tool,
                            const std::wstring& title,
                            const std::wstring& path,
                            const std::wstring& beforeContent,
                            bool existed,
                            const std::wstring& conversationId) {
    if (path.empty()) return L"";
    std::vector<AgentChangeEntry> list;
    LoadJournal(list);
    if (list.size() >= kMaxJournalEntries) {
        list.resize(kMaxJournalEntries - 1);
    }
    AgentChangeEntry e;
    e.id = TimestampName() + L"-" + std::to_wstring(::GetTickCount64() % 1000000);
    e.time = NowText();
    e.tool = tool;
    e.title = title.empty() ? tool : title;
    e.path = path;
    e.existed = existed;
    e.before = beforeContent;
    e.ok = false;
    e.conversationId = conversationId;
    list.insert(list.begin(), std::move(e));
    if (!WriteJournal(list)) return L"";
    return list.front().id;
}

void AgentUndoFinish(const std::wstring& id,
                     const std::wstring& afterContent,
                     bool ok) {
    if (id.empty()) return;
    std::vector<AgentChangeEntry> list;
    if (!LoadJournal(list)) return;
    for (auto& e : list) {
        if (e.id == id) {
            e.after = afterContent;
            e.ok = ok;
            break;
        }
    }
    WriteJournal(list);
}

bool LoadAgentChanges(std::vector<AgentChangeEntry>& out, size_t maxEntries) {
    if (!LoadJournal(out)) return false;
    if (out.size() > maxEntries) out.resize(maxEntries);
    return true;
}

bool RevertAgentChange(const std::wstring& id, std::wstring& err) {
    err.clear();
    std::vector<AgentChangeEntry> list;
    if (!LoadJournal(list)) {
        err = L"无法读取变更日志";
        return false;
    }
    auto it = std::find_if(list.begin(), list.end(),
        [&](const AgentChangeEntry& e) { return e.id == id; });
    if (it == list.end()) {
        err = L"未找到该变更记录";
        return false;
    }
    if (it->reverted) {
        err = L"该变更已恢复过，不能重复恢复";
        return false;
    }
    if (!it->existed) {
        if (GetFileAttributesW(it->path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!DeleteFileW(it->path.c_str())) {
                err = L"恢复失败：无法删除文件（" + it->path + L"）";
                return false;
            }
        }
    } else {
        if (!WriteTextFileUtf8(it->path, it->before)) {
            err = L"恢复失败：无法写回文件（" + it->path + L"）";
            return false;
        }
    }
    it->reverted = true;
    if (!WriteJournal(list)) {
        err = L"恢复成功，但变更日志更新失败";
        return false;
    }
    return true;
}

std::wstring ListAgentChangesText(size_t maxEntries) {
    std::vector<AgentChangeEntry> list;
    LoadAgentChanges(list, maxEntries);
    if (list.empty()) {
        return L"暂无助手修改记录。";
    }
    std::wstring out;
    out += L"最近的助手修改（共 " + std::to_wstring(list.size()) + L" 条）：\n";
    int idx = 1;
    for (const auto& e : list) {
        out += std::to_wstring(idx++) + L". [" + e.tool + L"] " + e.title
            + L"\n   时间: " + e.time
            + L"\n   文件: " + e.path
            + L"\n   状态: "
            + (e.reverted ? L"已恢复" : (e.ok ? L"已应用" : L"未生效"))
            + L"\n   撤销: revertAgentChange({\"id\":\"" + e.id + L"\"})\n";
    }
    return out;
}

std::wstring AgentChangesToJson(size_t maxEntries) {
    std::vector<AgentChangeEntry> list;
    LoadAgentChanges(list, maxEntries);
    json arr = json::array();
    for (const auto& e : list) {
        json j;
        j["id"] = ToUtf8(e.id);
        j["time"] = ToUtf8(e.time);
        j["tool"] = ToUtf8(e.tool);
        j["title"] = ToUtf8(e.title);
        j["path"] = ToUtf8(e.path);
        j["ok"] = e.ok;
        j["reverted"] = e.reverted;
        arr.push_back(std::move(j));
    }
    return FromUtf8(arr.dump());
}
