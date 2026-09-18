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
    // 只认 key 冒号后的下一个 JSON token（true/false），避免在窗口内误匹配其它字段字面量。
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return fallback;
    const auto colon = src.find(L':', pos);
    if (colon == std::wstring::npos) return fallback;
    size_t i = colon + 1;
    while (i < src.size() && (src[i] == L' ' || src[i] == L'\t' || src[i] == L'\r' || src[i] == L'\n')) {
        ++i;
    }
    if (i + 4 <= src.size()
        && src.compare(i, 4, L"true") == 0
        && (i + 4 >= src.size()
            || src[i + 4] == L',' || src[i + 4] == L'}' || src[i + 4] == L']'
            || src[i + 4] == L' ' || src[i + 4] == L'\t'
            || src[i + 4] == L'\r' || src[i + 4] == L'\n')) {
        return true;
    }
    if (i + 5 <= src.size()
        && src.compare(i, 5, L"false") == 0
        && (i + 5 >= src.size()
            || src[i + 5] == L',' || src[i + 5] == L'}' || src[i + 5] == L']'
            || src[i + 5] == L' ' || src[i + 5] == L'\t'
            || src[i + 5] == L'\r' || src[i + 5] == L'\n')) {
        return false;
    }
    return fallback;
}

std::vector<std::wstring> ExtractTasksArrayObjects(const std::wstring& content) {
    const auto arrPos = content.find(L"\"tasks\"");
    if (arrPos == std::wstring::npos) return {};
    const auto bracket = content.find(L'[', arrPos);
    if (bracket == std::wstring::npos) return {};

    std::vector<std::wstring> blocks;
    size_t pos = bracket + 1;
    bool inStr = false;
    bool esc = false;
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
            const wchar_t c = content[pos];
            if (inStr) {
                if (esc) esc = false;
                else if (c == L'\\') esc = true;
                else if (c == L'"') inStr = false;
                continue;
            }
            if (c == L'"') {
                inStr = true;
                continue;
            }
            if (c == L'{') ++depth;
            else if (c == L'}') {
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
    task.frequency = ClampScheduledFrequency(ParseIntField(obj, L"frequency", 3));
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
    task.time.weekDays = 0;
    {
        const int wd = ParseIntField(obj, L"weekDays", 0);
        task.time.weekDays = static_cast<uint8_t>(std::clamp(wd, 0, 127));
    }
    task.folder = NormalizeRelativeFolder(ExtractString(obj, L"folder"));
    if (task.id.empty()) task.id = GenerateScheduledTaskId();
    if (task.name.empty()) task.name = DefaultScheduledTaskName();
    return task;
}

void AppendTaskJson(std::wstring& out, const ScheduledTask& t) {
    out += L"    {\n";
    out += L"      \"id\": \"" + EscapeJson(t.id) + L"\",\n";
    out += L"      \"name\": \"" + EscapeJson(t.name) + L"\",\n";
    out += L"      \"kind\": ";
    out += (t.kind == ScheduledTaskKind::Recording ? L"0" : L"1");
    out += L",\n";
    out += L"      \"filePath\": \"" + EscapeJson(t.filePath) + L"\",\n";
    out += L"      \"fileDisplayName\": \"" + EscapeJson(t.fileDisplayName) + L"\",\n";
    out += L"      \"frequency\": " + std::to_wstring(static_cast<int>(t.frequency)) + L",\n";
    out += L"      \"status\": ";
    out += (t.status == ScheduledTaskStatus::Disabled ? L"1" : L"0");
    out += L",\n";
    out += L"      \"customFired\": ";
    out += (t.customFired ? L"true" : L"false");
    out += L",\n";
    out += L"      \"year\": " + std::to_wstring(t.time.year) + L",\n";
    out += L"      \"month\": " + std::to_wstring(t.time.month) + L",\n";
    out += L"      \"day\": " + std::to_wstring(t.time.day) + L",\n";
    out += L"      \"hour\": " + std::to_wstring(t.time.hour) + L",\n";
    out += L"      \"minute\": " + std::to_wstring(t.time.minute) + L",\n";
    out += L"      \"second\": " + std::to_wstring(t.time.second) + L",\n";
    out += L"      \"millisecond\": " + std::to_wstring(t.time.millisecond) + L",\n";
    out += L"      \"weekDays\": " + std::to_wstring(static_cast<int>(t.time.weekDays)) + L",\n";
    out += L"      \"folder\": \"" + EscapeJson(t.folder) + L"\"\n";
    out += L"    }";
}

bool WriteUtf8File(const std::wstring& path, const std::string& utf8) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == utf8.size();
}

}  // namespace

namespace {
std::wstring g_scheduledTasksPathOverride;
}

void SetScheduledTasksFilePathForTest(const std::wstring& path) {
    g_scheduledTasksPathOverride = path;
}

std::wstring ScheduledTasksFilePath() {
    if (!g_scheduledTasksPathOverride.empty()) return g_scheduledTasksPathOverride;
    return AppDir() + L"\\scheduled_tasks.json";
}

bool ParseScheduledTasksJson(const std::wstring& content,
                             std::vector<ScheduledTask>& out,
                             bool* globalDisabled) {
    out.clear();
    if (globalDisabled) *globalDisabled = false;
    if (content.empty()) return true;
    if (globalDisabled) *globalDisabled = ParseBoolField(content, L"globalDisabled", false);

    const auto blocks = ExtractTasksArrayObjects(content);
    out.reserve(blocks.size());
    for (const auto& block : blocks) {
        ScheduledTask task = ParseTaskObject(block);
        // 无目标路径的任务永远不会跑；丢弃以免脏数据进调度器。
        if (task.filePath.empty()) continue;
        // 丢弃 ScheduledTaskSelfTest 曾误写入产品目录的夹具任务
        if (task.name == L"selftest"
            && task.filePath.find(L"dummy\\script") != std::wstring::npos) {
            continue;
        }
        out.push_back(std::move(task));
    }
    return true;
}

bool LoadScheduledTasks(std::vector<ScheduledTask>& out, bool* globalDisabled) {
    return ParseScheduledTasksJson(ReadAll(ScheduledTasksFilePath()), out, globalDisabled);
}

bool SaveScheduledTasks(const std::vector<ScheduledTask>& tasks, bool globalDisabled) {
    // 必须写 UTF-8：wofstream 默认 locale 遇中文路径会截断，导致任务保存后列表为空
    std::wstring wide = L"{\n  \"tasks\": [\n";
    for (size_t i = 0; i < tasks.size(); ++i) {
        AppendTaskJson(wide, tasks[i]);
        if (i + 1 < tasks.size()) wide += L",";
        wide += L"\n";
    }
    wide += L"  ],\n";
    wide += L"  \"globalDisabled\": ";
    wide += (globalDisabled ? L"true" : L"false");
    wide += L"\n}\n";
    const std::wstring path = ScheduledTasksFilePath();
    const std::wstring tmp = path + L".tmp";
    const std::string utf8 = ToUtf8(wide);
    if (!WriteUtf8File(tmp, utf8)) return false;
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(path.c_str());
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
    }
    return true;
}

int RetargetScheduledTaskFilePaths(const std::wstring& oldPath, const std::wstring& newPath) {
    if (oldPath.empty() || newPath.empty() || LibraryPathsEqual(oldPath, newPath)) return 0;
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = false;
    if (!LoadScheduledTasks(tasks, &globalDisabled)) return 0;
    int n = 0;
    for (auto& t : tasks) {
        if (t.filePath.empty() || !LibraryPathsEqual(t.filePath, oldPath)) continue;
        t.filePath = newPath;
        const auto slash = newPath.find_last_of(L"\\/");
        t.fileDisplayName = (slash == std::wstring::npos) ? newPath : newPath.substr(slash + 1);
        ++n;
    }
    if (n == 0) return 0;
    return SaveScheduledTasks(tasks, globalDisabled) ? n : 0;
}

int RetargetScheduledTaskFilePathPrefix(const std::wstring& oldDir, const std::wstring& newDir) {
    if (oldDir.empty() || newDir.empty() || LibraryPathsEqual(oldDir, newDir)) return 0;
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = false;
    if (!LoadScheduledTasks(tasks, &globalDisabled)) return 0;
    int n = 0;
    for (auto& t : tasks) {
        if (t.filePath.empty()) continue;
        if (!RelocatePathUnderDir(t.filePath, oldDir, newDir)) continue;
        const auto slash = t.filePath.find_last_of(L"\\/");
        t.fileDisplayName = (slash == std::wstring::npos) ? t.filePath : t.filePath.substr(slash + 1);
        ++n;
    }
    if (n == 0) return 0;
    return SaveScheduledTasks(tasks, globalDisabled) ? n : 0;
}
