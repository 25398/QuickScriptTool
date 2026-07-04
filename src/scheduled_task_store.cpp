#include "scheduled_task_store.h"

#include "utils.h"

#include <algorithm>
#include <fstream>

namespace {

int ParseIntField(const std::wstring& src, const std::wstring& key, int fallback) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return fallback;
    const auto colon = src.find(L':', pos);
    if (colon == std::wstring::npos) return fallback;
    size_t i = colon + 1;
    while (i < src.size() && (src[i] == L' ' || src[i] == L'\t')) ++i;
    return static_cast<int>(std::wcstol(src.c_str() + i, nullptr, 10));
}

bool ParseBoolField(const std::wstring& src, const std::wstring& key, bool fallback) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return fallback;
    const auto colon = src.find(L':', pos);
    if (colon == std::wstring::npos) return fallback;
    const auto valPos = src.find(L"true", colon);
    if (valPos != std::wstring::npos && valPos < colon + 12) return true;
    const auto falsePos = src.find(L"false", colon);
    if (falsePos != std::wstring::npos && falsePos < colon + 12) return false;
    return fallback;
}

std::vector<std::wstring> ExtractTasksArrayObjects(const std::wstring& content) {
    const auto arrPos = content.find(L"\"tasks\"");
    if (arrPos == std::wstring::npos) return {};
    const auto bracket = content.find(L'[', arrPos);
    if (bracket == std::wstring::npos) return {};

    std::vector<std::wstring> blocks;
    size_t pos = bracket + 1;
    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == L' ' || content[pos] == L'\n'
            || content[pos] == L'\r' || content[pos] == L'\t' || content[pos] == L',')) {
            ++pos;
        }
        if (pos >= content.size() || content[pos] == L']') break;
        if (content[pos] != L'{') { ++pos; continue; }
        int depth = 0;
        const size_t start = pos;
        for (; pos < content.size(); ++pos) {
            if (content[pos] == L'{') ++depth;
            else if (content[pos] == L'}') {
                --depth;
                if (depth == 0) {
                    blocks.push_back(content.substr(start, pos - start + 1));
                    ++pos;
                    break;
                }
            }
        }
    }
    return blocks;
}

ScheduledTask ParseTaskObject(const std::wstring& obj) {
    ScheduledTask task{};
    task.id = ExtractString(obj, L"id");
    task.name = ExtractString(obj, L"name");
    task.filePath = ExtractString(obj, L"filePath");
    task.fileDisplayName = ExtractString(obj, L"fileDisplayName");
    task.kind = ParseIntField(obj, L"kind", 1) == 0
        ? ScheduledTaskKind::Recording : ScheduledTaskKind::Macro;
    task.frequency = static_cast<ScheduledFrequency>(
        std::clamp(ParseIntField(obj, L"frequency", 3), 0, 3));
    task.status = ParseIntField(obj, L"status", 0) == 1
        ? ScheduledTaskStatus::Disabled : ScheduledTaskStatus::Enabled;
    task.customFired = ParseBoolField(obj, L"customFired", false);
    task.time.year = ParseIntField(obj, L"year", 0);
    task.time.month = ParseIntField(obj, L"month", 0);
    task.time.day = ParseIntField(obj, L"day", 0);
    task.time.hour = ParseIntField(obj, L"hour", 9);
    task.time.minute = ParseIntField(obj, L"minute", 0);
    task.time.second = ParseIntField(obj, L"second", 0);
    task.time.millisecond = ParseIntField(obj, L"millisecond", 0);
    task.time.weekDays = static_cast<uint8_t>(ParseIntField(obj, L"weekDays", 0));
    if (task.id.empty()) task.id = GenerateScheduledTaskId();
    if (task.name.empty()) task.name = DefaultScheduledTaskName();
    return task;
}

void WriteTask(std::wofstream& file, const ScheduledTask& t) {
    file << L"    {\n";
    file << L"      \"id\": \"" << EscapeJson(t.id) << L"\",\n";
    file << L"      \"name\": \"" << EscapeJson(t.name) << L"\",\n";
    file << L"      \"kind\": " << (t.kind == ScheduledTaskKind::Recording ? 0 : 1) << L",\n";
    file << L"      \"filePath\": \"" << EscapeJson(t.filePath) << L"\",\n";
    file << L"      \"fileDisplayName\": \"" << EscapeJson(t.fileDisplayName) << L"\",\n";
    file << L"      \"frequency\": " << static_cast<int>(t.frequency) << L",\n";
    file << L"      \"status\": " << (t.status == ScheduledTaskStatus::Disabled ? 1 : 0) << L",\n";
    file << L"      \"customFired\": " << (t.customFired ? L"true" : L"false") << L",\n";
    file << L"      \"year\": " << t.time.year << L",\n";
    file << L"      \"month\": " << t.time.month << L",\n";
    file << L"      \"day\": " << t.time.day << L",\n";
    file << L"      \"hour\": " << t.time.hour << L",\n";
    file << L"      \"minute\": " << t.time.minute << L",\n";
    file << L"      \"second\": " << t.time.second << L",\n";
    file << L"      \"millisecond\": " << t.time.millisecond << L",\n";
    file << L"      \"weekDays\": " << static_cast<int>(t.time.weekDays) << L"\n";
    file << L"    }";
}

}  // namespace

std::wstring ScheduledTasksFilePath() {
    return AppDir() + L"\\scheduled_tasks.json";
}

bool LoadScheduledTasks(std::vector<ScheduledTask>& out, bool* globalDisabled) {
    out.clear();
    if (globalDisabled) *globalDisabled = false;
    const std::wstring path = ScheduledTasksFilePath();
    const std::wstring content = ReadAll(path);
    if (content.empty()) return true;
    if (globalDisabled) *globalDisabled = ParseBoolField(content, L"globalDisabled", false);

    const auto blocks = ExtractTasksArrayObjects(content);
    out.reserve(blocks.size());
    for (const auto& block : blocks) {
        ScheduledTask task = ParseTaskObject(block);
        if (!task.filePath.empty() || !task.name.empty()) {
            out.push_back(std::move(task));
        }
    }
    return true;
}

bool SaveScheduledTasks(const std::vector<ScheduledTask>& tasks, bool globalDisabled) {
    const std::wstring path = ScheduledTasksFilePath();
    std::wofstream file(path, std::ios::binary);
    if (!file) return false;
    file << L"{\n  \"tasks\": [\n";
    for (size_t i = 0; i < tasks.size(); ++i) {
        WriteTask(file, tasks[i]);
        if (i + 1 < tasks.size()) file << L",";
        file << L"\n";
    }
    file << L"  ],\n";
    file << L"  \"globalDisabled\": " << (globalDisabled ? L"true" : L"false") << L"\n";
    file << L"}\n";
    return true;
}
