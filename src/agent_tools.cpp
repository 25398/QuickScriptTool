// ──────────────────────────────────────────────────────────────────
// agent_tools.cpp — AI Agent 工具实现
// 脚本读写 / 录制优化 / 定时任务管理 / 应用设置修改
// 所有工具的工厂函数
// ──────────────────────────────────────────────────────────────────

#include "agent_tools.h"

#include "app_settings_store.h"
#include "scheduled_task_store.h"
#include "scheduled_task_types.h"
#include "agent_script_ops.h"
#include "agent_reference.h"
#include "agent_ai_actions.h"
#include "script_action_builder.h"
#include "script_io.h"
#include "utils.h"

#include <fstream>
#include <string>
#include <sstream>
#include <vector>

namespace {

// ── 路径安全 + 双目录查找 ─────────────────────────────────────────

/// 校验文件名安全（无路径穿越）
bool IsSafeFileName(const std::wstring& fileName) {
    if (fileName.find(L"\\") != std::wstring::npos) return false;
    if (fileName.find(L"/") != std::wstring::npos) return false;
    if (fileName.find(L"..") != std::wstring::npos) return false;
    return true;
}

/// 根据 dir 参数获取目录路径
std::wstring DirFromHint(const std::wstring& dirHint) {
    if (dirHint == L"recordings") return RecordingsDir();
    if (dirHint == L"scripts") return ScriptsDir();
    return L"";  // "" 表示自动
}

/// 查找脚本文件：支持 scripts 和 recordings 两个目录
/// 返回格式：{fullPath, true} 表示找到；{errorMsg, false} 表示找不到
struct FindResult { std::wstring path; bool found = false; };

FindResult FindScriptFile(const std::wstring& fileName, const std::wstring& dirHint) {
    if (!IsSafeFileName(fileName))
        return { L"[错误] 文件名包含非法字符。", false };

    // 如果指定了目录，只在该目录查找
    std::wstring hintDir = DirFromHint(dirHint);
    if (!hintDir.empty()) {
        std::wstring path = hintDir + L"\\" + fileName;
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return { path, true };
        std::wstring label = (dirHint == L"recordings") ? L"键鼠录制目录" : L"脚本宏目录";
        return { L"[错误] 在" + label + L"中未找到文件：" + fileName, false };
    }

    // 自动模式：先查 scripts，再查 recordings
    std::wstring path = ScriptsDir() + L"\\" + fileName;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return { path, true };

    path = RecordingsDir() + L"\\" + fileName;
    attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return { path, true };

    return { L"[错误] 文件不存在：" + fileName + L"（已检查脚本目录和录制目录）", false };
}

/// 解析公共参数：fileName + dir
struct CommonParams {
    std::wstring fileName;
    std::wstring dir;
    bool parseError = false;
};

CommonParams ParseCommonParams(const std::wstring& paramsJson) {
    CommonParams p;
    json params;
    try {
        params = json::parse(ToUtf8(paramsJson));
    } catch (const json::parse_error&) {
        p.parseError = true;
        return p;
    }
    p.fileName = FromUtf8(params.value("fileName", ""));
    p.dir = FromUtf8(params.value("dir", ""));
    return p;
}

// 列出单个目录下的脚本
void ListScriptsInDir(const std::wstring& dir, const std::wstring& label,
                      std::wstringstream& result, int& index) {
    std::wstring pattern = dir + L"\\*.json";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    std::vector<std::pair<std::wstring, ScriptFileData>> entries;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fileName(fd.cFileName);
        if (fileName.size() < 5 || fileName.substr(fileName.size() - 5) != L".json") continue;
        std::wstring fullPath = dir + L"\\" + fileName;
        entries.push_back({ fileName, LoadScriptFileData(fullPath) });
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if (entries.empty()) return;

    result << L"\n=== " << label << L" ===\n";
    for (const auto& [fname, data] : entries) {
        std::wstring name = data.scriptName.empty() ? fname : data.scriptName;
        int count = static_cast<int>(data.actions.size());
        result << index << L". " << fname << L" — \"" << name << L"\" (" << count << L" 个动作)\n";
        ++index;
    }
}

// ── 录制优化辅助函数 ──────────────────────────────────────────────

bool IsKeyOperation(ActionType type) {
    return type != ActionType::MoveMouse && type != ActionType::Wait;
}

double ComputeMergedWait(const std::vector<double>& waits, const std::wstring& mode) {
    if (waits.empty()) return 0.0;
    if (mode == L"average" || mode == L"avg") {
        double sum = 0;
        for (double w : waits) sum += w;
        return sum / static_cast<double>(waits.size());
    }
    if (mode == L"first") return waits.front();
    if (mode == L"last") return waits.back();
    double sum = 0;
    for (double w : waits) sum += w;
    return sum;
}

std::wstring ApplyMoveMerge(ScriptFileData& data, const std::wstring& waitMode) {
    std::vector<ScriptAction> result;
    std::vector<ScriptAction> segment;
    int mergedSegments = 0;
    int removedActions = 0;

    auto flushSegment = [&]() {
        if (segment.empty()) return;
        std::vector<double> waits;
        ScriptAction lastMove{};
        bool hasMove = false;
        int indent = segment[0].indent;

        for (const auto& a : segment) {
            if (a.type == ActionType::Wait) waits.push_back(a.duration);
            else if (a.type == ActionType::MoveMouse) { lastMove = a; hasMove = true; }
        }

        if (hasMove) {
            double mergedWait = ComputeMergedWait(waits, waitMode);
            int before = static_cast<int>(segment.size());
            if (mergedWait > 0.0005) {
                ScriptAction wa{};
                wa.type = ActionType::Wait;
                wa.duration = mergedWait;
                wa.indent = indent;
                wa.customText = L"等待";
                wa.remark = L"已合并";
                result.push_back(wa);
            }
            result.push_back(lastMove);
            removedActions += before - (mergedWait > 0.0005 ? 2 : 1);
            ++mergedSegments;
        } else {
            for (auto& a : segment) result.push_back(std::move(a));
        }
        segment.clear();
    };

    for (auto& a : data.actions) {
        if (IsKeyOperation(a.type)) {
            flushSegment();
            result.push_back(std::move(a));
        } else {
            segment.push_back(std::move(a));
        }
    }
    flushSegment();

    data.actions = std::move(result);
    double total = 0;
    for (const auto& a : data.actions)
        if (a.type == ActionType::Wait) total += a.duration;
    data.durationSeconds = total;

    std::wstringstream ss;
    ss << L"优化完成：合并了 " << mergedSegments << L" 个分段，移除 " << removedActions
       << L" 个冗余动作。\n新脚本总动作数: " << data.actions.size()
       << L"，总等待时长: " << total << L" 秒。";
    return ss.str();
}

std::wstring ApplyMoveCompress(ScriptFileData& data, double distanceThreshold, double compressWait) {
    std::vector<ScriptAction> result;
    std::vector<ScriptAction> segment;
    int compressedSegments = 0;
    int removedPoints = 0;

    struct Point { int x; int y; };
    auto dist = [](const Point& a, const Point& b) {
        double dx = static_cast<double>(a.x - b.x);
        double dy = static_cast<double>(a.y - b.y);
        return std::sqrt(dx * dx + dy * dy);
    };

    auto flushSegment = [&]() {
        if (segment.empty()) return;
        std::vector<Point> points;
        int indent = segment[0].indent;
        for (const auto& a : segment) {
            if (a.type == ActionType::MoveMouse)
                points.push_back({a.x, a.y});
        }

        if (points.size() >= 2) {
            std::vector<Point> compressed{points.front()};
            for (size_t i = 1; i + 1 < points.size(); ++i) {
                if (dist(compressed.back(), points[i]) >= distanceThreshold)
                    compressed.push_back(points[i]);
            }
            if (compressed.back().x != points.back().x || compressed.back().y != points.back().y)
                compressed.push_back(points.back());

            for (size_t i = 0; i < compressed.size(); ++i) {
                if (i > 0) {
                    ScriptAction wa{};
                    wa.type = ActionType::Wait;
                    wa.duration = compressWait;
                    wa.indent = indent;
                    wa.customText = L"等待";
                    result.push_back(wa);
                }
                ScriptAction mv{};
                mv.type = ActionType::MoveMouse;
                mv.x = compressed[i].x;
                mv.y = compressed[i].y;
                mv.indent = indent;
                mv.customText = L"移动到 (" + std::to_wstring(mv.x) + L", " + std::to_wstring(mv.y) + L")";
                result.push_back(mv);
            }
            removedPoints += static_cast<int>(points.size()) - static_cast<int>(compressed.size());
            ++compressedSegments;
        } else {
            for (auto& a : segment) result.push_back(std::move(a));
        }
        segment.clear();
    };

    for (auto& a : data.actions) {
        if (IsKeyOperation(a.type)) {
            flushSegment();
            result.push_back(std::move(a));
        } else {
            segment.push_back(std::move(a));
        }
    }
    flushSegment();

    data.actions = std::move(result);
    double total = 0;
    for (const auto& a : data.actions)
        if (a.type == ActionType::Wait) total += a.duration;
    data.durationSeconds = total;

    std::wstringstream ss;
    ss << L"路径压缩完成：压缩了 " << compressedSegments << L" 个路径段，移除 "
       << removedPoints << L" 个冗余移动点（阈值 " << distanceThreshold << L" 像素）。\n"
       << L"新脚本总动作数: " << data.actions.size() << L"。";
    return ss.str();
}

/// 统计数据，写入 ss
void WriteScriptStats(const ScriptFileData& data, std::wstringstream& ss) {
    int moveCount = 0, waitCount = 0, keyCount = 0;
    double totalWait = 0;
    int loopCount = 0, ifCount = 0, findImageCount = 0, clickCount = 0;
    int keyDownCount = 0, keyUpCount = 0, keyClickCount = 0;
    int otherCount = 0;

    for (const auto& a : data.actions) {
        switch (a.type) {
        case ActionType::MoveMouse: ++moveCount; break;
        case ActionType::Wait: ++waitCount; totalWait += a.duration; break;
        case ActionType::Loop: ++loopCount; ++keyCount; break;
        case ActionType::EndLoop: ++keyCount; break;
        case ActionType::If: ++ifCount; ++keyCount; break;
        case ActionType::Else: ++keyCount; break;
        case ActionType::FindImage: ++findImageCount; ++keyCount; break;
        case ActionType::MouseClick: case ActionType::MouseDown: case ActionType::MouseUp:
            ++clickCount; ++keyCount; break;
        case ActionType::KeyDown: ++keyDownCount; ++keyCount; break;
        case ActionType::KeyUp: ++keyUpCount; ++keyCount; break;
        case ActionType::KeyClick: ++keyClickCount; ++keyCount; break;
        default: ++otherCount; ++keyCount; break;
        }
    }

    ss << L"脚本统计: " << data.scriptName << L"\n";
    ss << L"总动作数: " << data.actions.size() << L"\n";
    ss << L"总等待时长: " << totalWait << L" 秒\n";
    ss << L"记录时间: " << data.recordTime << L"\n\n";

    ss << L"动作分类:\n";
    ss << L"  鼠标移动: " << moveCount << L"\n";
    ss << L"  等待: " << waitCount << L" (合计 " << totalWait << L"秒)\n";
    ss << L"  鼠标点击: " << clickCount << L"\n";
    ss << L"  按键: " << keyDownCount << L" down, " << keyUpCount << L" up, " << keyClickCount << L" click\n";
    ss << L"  识图: " << findImageCount << L"\n";
    ss << L"  循环: " << loopCount << L"\n";
    ss << L"  条件: " << ifCount << L"\n";
    if (otherCount > 0) ss << L"  其他: " << otherCount << L"\n";

    ss << L"\n可优化分段（关键操作之间的 Move+Wait 连续块）:\n";
    int segmentIdx = 0;
    size_t segStart = 0;
    bool hasMoveInSeg = false;
    int segMoves = 0, segWaits = 0;

    auto flushSegment = [&](size_t endIdx) {
        if (hasMoveInSeg && endIdx > segStart) {
            ++segmentIdx;
            size_t keyStart = segStart > 0 ? segStart - 1 : 0;
            size_t keyEnd = endIdx < data.actions.size() ? endIdx : data.actions.size() - 1;
            std::wstring startLabel = (segStart == 0) ? L"开头"
                : (L"#" + std::to_wstring(data.actions[keyStart].originalNo) + L" " + data.actions[keyStart].customText);
            std::wstring endLabel = (endIdx >= data.actions.size()) ? L"结尾"
                : (L"#" + std::to_wstring(data.actions[keyEnd].originalNo) + L" " + data.actions[keyEnd].customText);
            ss << L"  " << segmentIdx << L". [" << startLabel << L" → " << endLabel << L"] — "
               << segMoves << L" 个移动, " << segWaits << L" 个等待\n";
        }
        hasMoveInSeg = false;
        segMoves = 0;
        segWaits = 0;
    };

    for (size_t i = 0; i < data.actions.size(); ++i) {
        bool isKey = (data.actions[i].type != ActionType::MoveMouse &&
                      data.actions[i].type != ActionType::Wait);
        if (isKey) { flushSegment(i); segStart = i + 1; }
        else {
            if (data.actions[i].type == ActionType::MoveMouse) { hasMoveInSeg = true; ++segMoves; }
            if (data.actions[i].type == ActionType::Wait) ++segWaits;
        }
    }
    flushSegment(data.actions.size());

    if (segmentIdx == 0) ss << L"  (无可压缩分段)\n";
    else ss << L"\n共 " << segmentIdx << L" 个可压缩分段，使用 optimizeScript 工具进行压缩。\n";
}

}  // namespace

// ── listScripts ───────────────────────────────────────────────────
AgentTool MakeListScriptsTool() {
    AgentTool tool;
    tool.name = L"listScripts";
    tool.description = L"列出脚本目录（scripts）和键鼠录制目录（recordings）下的所有 JSON 文件，返回每个文件的名称和动作数量";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "dir": {
                "type": "string",
                "enum": ["all", "scripts", "recordings"],
                "description": "要列出的目录：all=全部, scripts=脚本宏, recordings=键鼠录制。默认 all"
            }
        }
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring dirFilter;
        json params;
        try {
            params = json::parse(ToUtf8(paramsJson));
        } catch (...) { params = json::object(); }
        dirFilter = FromUtf8(params.value("dir", "all"));

        std::wstringstream result;
        result << L"脚本列表：\n";
        int index = 1;

        if (dirFilter == L"all" || dirFilter == L"scripts")
            ListScriptsInDir(ScriptsDir(), L"脚本宏", result, index);

        if (dirFilter == L"all" || dirFilter == L"recordings")
            ListScriptsInDir(RecordingsDir(), L"键鼠录制", result, index);

        if (index == 1) result << L"(两个目录均为空)";
        return result.str();
    };

    return tool;
}

// ── readScript ────────────────────────────────────────────────────
AgentTool MakeReadScriptTool() {
    AgentTool tool;
    tool.name = L"readScript";
    tool.description = L"读取指定脚本或录制的完整 JSON 内容（自动在 scripts 和 recordings 目录下查找）";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "fileName": {
                "type": "string",
                "description": "文件名（如 myscript.json）"
            },
            "dir": {
                "type": "string",
                "enum": ["scripts", "recordings"],
                "description": "限定目录：scripts=脚本宏, recordings=键鼠录制。省略则自动查找"
            }
        },
        "required": ["fileName"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        auto p = ParseCommonParams(paramsJson);
        if (p.parseError) return L"[错误] 参数 JSON 解析失败。";
        if (p.fileName.empty()) return L"[错误] 缺少 fileName 参数。";

        auto found = FindScriptFile(p.fileName, p.dir);
        if (!found.found) return found.path;

        std::wstring content = ReadAll(found.path);
        if (content.empty()) return L"[提示] 文件内容为空：" + p.fileName;
        return content;
    };

    return tool;
}

namespace {

std::wstring ExecuteBuildScriptActionsParams(const json& params, bool executionFormat) {
    if (params.value("showSchema", false))
        return ScriptActionBuilderSchema();

    std::vector<json> items;
    if (params.contains("actions") && params["actions"].is_array()) {
        for (const auto& item : params["actions"]) {
            if (item.is_object()) items.push_back(item);
        }
    } else if (params.contains("type") && params["type"].is_string()) {
        items.push_back(params);
    }

    if (items.empty()) {
        return L"[错误] 缺少 actions 数组。可先设 showSchema=true 查看各动作参数。\n\n"
            + ScriptActionBuilderSchema();
    }

    std::wstring error;
    const std::wstring jsonArray = BuildScriptActionsJsonArray(items, error);
    if (!error.empty()) return L"[错误] " + error;

    if (executionFormat) return jsonArray;

    std::wstring summary = L"✓ 已构建 " + std::to_wstring(items.size()) + L" 个动作。\n";
    summary += L"将下方 JSON 数组嵌入脚本的 actions 字段即可：\n\n";
    summary += jsonArray;
    return summary;
}

struct AiModelFillResult {
    bool ok = false;
    std::wstring error;
    std::wstring note;
    std::wstring modelName;
};

AiModelFillResult FillResolvedAiModel(json& params, bool requireVision) {
    AiModelFillResult r;
    if (ApplyResolvedAiModelToActionParams(params)) {
        r.modelName = FromUtf8(params["aiModelName"].get<std::string>());
        r.note = L"已选择模型：" + r.modelName;
        r.ok = true;
        return r;
    }

    const quickscript::AppSettings settings = LoadAgentAppSettings();
    const std::wstring preferred = FromUtf8(params.value("aiModelName", ""));
    const std::wstring resolved = ResolveAiModelName(settings.ai, requireVision, preferred);
    if (Trim(resolved).empty()) {
        r.error = L"[错误] 未配置可用 AI 模型。请先在「设置→AI助手」中添加模型，"
            L"或调用 listAiModels 查看可用列表。";
        return r;
    }
    params["aiModelName"] = ToUtf8(resolved);
    r.note = L"已选择模型：" + resolved;
    if (!preferred.empty() && preferred != resolved) {
        r.note += L"（指定「" + preferred + L"」不可用或不支持识图，已自动替换）";
    }
    r.modelName = resolved;
    r.ok = true;
    return r;
}

json ParseToolParams(const std::wstring& paramsJson, std::wstring& error) {
    try {
        return json::parse(ToUtf8(paramsJson));
    } catch (const json::parse_error&) {
        error = L"[错误] 参数 JSON 解析失败。";
        return json::object();
    }
}

}  // namespace

// ── submitMacroActions（AI 动作执行运行时）────────────────────────
AgentTool MakeSubmitMacroActionsTool() {
    AgentTool tool;
    tool.name = L"submitMacroActions";
    tool.description =
        L"【AI 动作执行必用】提交本批次要立刻执行的宏动作。禁止在文字回复中手写 JSON。"
        L"传入 actions 数组，每项含 type 及该类型参数，与编辑器手动添加动作完全一致。"
        L"返回经校验的动作 JSON 数组，程序将立即执行。"
        L"不确定参数时先 showSchema=true 查看字段说明。";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "actions": {
                "type": "array",
                "description": "动作参数对象数组，每项至少含 type 字段",
                "items": {
                    "type": "object",
                    "properties": {
                        "type": { "type": "string" }
                    },
                    "required": ["type"]
                }
            },
            "showSchema": {
                "type": "boolean",
                "description": "为 true 时返回各 type 参数字段说明，不构建动作"
            }
        },
        "required": []
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try {
            params = json::parse(ToUtf8(paramsJson));
        } catch (const json::parse_error&) {
            return L"[错误] 参数 JSON 解析失败。";
        }
        return ExecuteBuildScriptActionsParams(params, true);
    };

    return tool;
}

// ── buildScriptActions ────────────────────────────────────────────
AgentTool MakeBuildScriptActionsTool() {
    AgentTool tool;
    tool.name = L"buildScriptActions";
    tool.description =
        L"构建规范格式的脚本动作 JSON 数组，与用户在编辑器手动添加动作的逻辑完全一致。"
        L"创建或修改脚本时必须用此工具生成 actions，禁止手写动作 JSON 对象。"
        L"传入 actions 数组，每项含 type 及该类型参数；返回可直接嵌入脚本的 JSON 数组文本。"
        L"支持全部编辑器动作（不含 AI 专用动作）。"
        L"AI 相关请用 buildGetCursorPosAction、buildAiTextAnalysisAction、"
        L"buildAiImageAnalysisAction、buildAiActionExecuteAction。"
        L"找图/OCR 保存变量用 followUp:\"saveVar\"；等待用 type:wait,duration；按键用 keyClick/keyDown/keyUp。";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "actions": {
                "type": "array",
                "description": "动作参数对象数组，每项至少含 type 字段",
                "items": {
                    "type": "object",
                    "properties": {
                        "type": { "type": "string" }
                    },
                    "required": ["type"]
                }
            },
            "showSchema": {
                "type": "boolean",
                "description": "为 true 时返回各 type 参数字段说明，不构建动作"
            }
        },
        "required": []
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try {
            params = json::parse(ToUtf8(paramsJson));
        } catch (const json::parse_error&) {
            return L"[错误] 参数 JSON 解析失败。";
        }
        return ExecuteBuildScriptActionsParams(params, false);
    };

    return tool;
}

// ── writeScript ───────────────────────────────────────────────────
AgentTool MakeWriteScriptTool() {
    AgentTool tool;
    tool.name = L"writeScript";
    tool.description = L"将内容写入指定脚本或录制文件（覆盖已有内容）";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "fileName": {
                "type": "string",
                "description": "文件名（如 myscript.json）"
            },
            "content": {
                "type": "string",
                "description": "完整的脚本 JSON 内容"
            },
            "dir": {
                "type": "string",
                "enum": ["scripts", "recordings"],
                "description": "目标目录：scripts=脚本宏, recordings=键鼠录制。默认 scripts"
            }
        },
        "required": ["fileName", "content"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        auto p = ParseCommonParams(paramsJson);
        if (p.parseError) return L"[错误] 参数 JSON 解析失败。";

        json params;
        try { params = json::parse(ToUtf8(paramsJson)); }
        catch (const json::parse_error&) { return L"[错误] 参数 JSON 解析失败。"; }

        if (p.fileName.empty()) return L"[错误] 缺少 fileName 参数。";
        const std::wstring content = FromUtf8(params.value("content", ""));
        const auto result = AgentSaveScriptContent(p.fileName, content, p.dir);
        return result.message;
    };

    return tool;
}

// ── getScriptStats ────────────────────────────────────────────────
AgentTool MakeGetScriptStatsTool() {
    AgentTool tool;
    tool.name = L"getScriptStats";
    tool.description = L"获取脚本或录制的详细统计信息，包括按类型分类的动作数量、关键操作位置、可优化分段等";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "fileName": {
                "type": "string",
                "description": "文件名（如 myscript.json）"
            },
            "dir": {
                "type": "string",
                "enum": ["scripts", "recordings"],
                "description": "限定目录：scripts=脚本宏, recordings=键鼠录制。省略则自动查找"
            }
        },
        "required": ["fileName"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        auto p = ParseCommonParams(paramsJson);
        if (p.parseError) return L"[错误] 参数 JSON 解析失败。";
        if (p.fileName.empty()) return L"[错误] 缺少 fileName 参数。";

        auto found = FindScriptFile(p.fileName, p.dir);
        if (!found.found) return found.path;

        ScriptFileData data = LoadScriptFileData(found.path);
        if (data.actions.empty()) return L"[提示] 脚本为空或无有效动作：" + p.fileName;

        std::wstringstream ss;
        WriteScriptStats(data, ss);
        return ss.str();
    };

    return tool;
}

// ── optimizeScript ────────────────────────────────────────────────
AgentTool MakeOptimizeScriptTool() {
    AgentTool tool;
    tool.name = L"optimizeScript";
    tool.description = L"优化脚本或录制：合并关键操作之间的连续 MouseMove 和 Wait 为一组动作，或压缩鼠标移动路径中过密的点。自动在 scripts 和 recordings 目录下查找";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "fileName": {
                "type": "string",
                "description": "要优化的文件名（如 myscript.json）"
            },
            "dir": {
                "type": "string",
                "enum": ["scripts", "recordings"],
                "description": "限定目录：scripts=脚本宏, recordings=键鼠录制。省略则自动查找"
            },
            "outputFileName": {
                "type": "string",
                "description": "输出文件名，省略则覆盖原文件"
            },
            "outputDir": {
                "type": "string",
                "enum": ["scripts", "recordings"],
                "description": "输出目标目录，省略则与输入文件同目录"
            },
            "mergeMode": {
                "type": "string",
                "enum": ["merge", "compressPath"],
                "description": "优化模式：merge = 合并关键操作之间的移动和等待；compressPath = 按像素距离压缩路径"
            },
            "waitCalculation": {
                "type": "string",
                "enum": ["sum", "average", "first", "last"],
                "description": "合并后等待时间计算方式（仅 merge 模式）：sum=累加, average=平均, first=第一个, last=最后一个。默认 sum"
            },
            "distanceThreshold": {
                "type": "number",
                "description": "路径压缩时相邻移动点最小保留距离（像素），默认 5"
            },
            "compressWait": {
                "type": "number",
                "description": "路径压缩时移动点之间的等待时间（秒），默认 0.05"
            }
        },
        "required": ["fileName"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try { params = json::parse(ToUtf8(paramsJson)); }
        catch (const json::parse_error&) { return L"[错误] 参数 JSON 解析失败。"; }

        AgentOptimizeOptions opts;
        opts.fileName = FromUtf8(params.value("fileName", ""));
        opts.dir = FromUtf8(params.value("dir", ""));
        opts.outputFileName = FromUtf8(params.value("outputFileName", ""));
        opts.outputDir = FromUtf8(params.value("outputDir", ""));
        opts.mergeMode = FromUtf8(params.value("mergeMode", "merge"));
        opts.waitCalculation = FromUtf8(params.value("waitCalculation", "sum"));
        opts.distanceThreshold = params.value("distanceThreshold", 5.0);
        opts.compressWait = params.value("compressWait", 0.05);
        if (opts.distanceThreshold < 0.1) opts.distanceThreshold = 0.1;
        if (opts.compressWait < 0.0) opts.compressWait = 0.0;

        const auto result = AgentOptimizeScriptFile(opts);
        return result.message;
    };

    return tool;
}

// ── createMacroScript ─────────────────────────────────────────────
AgentTool MakeCreateMacroScriptTool() {
    AgentTool tool;
    tool.name = L"createMacroScript";
    tool.description =
        L"一步创建鼠标宏：构建动作并保存到 scripts 目录。"
        L"含 AI 动作时无需手写 aiModelName，保存时会自动从已添加模型中选取（图片分析优先识图模型）。"
        L"也可先用 buildAiTextAnalysisAction / buildAiImageAnalysisAction 等专用工具构建单个 AI 动作。";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "fileName": { "type": "string", "description": "文件名，如 mymacro.json" },
            "scriptName": { "type": "string", "description": "宏显示名称" },
            "actions": {
                "type": "array",
                "description": "动作参数数组，每项含 type 及字段（同 buildScriptActions）",
                "items": { "type": "object" }
            }
        },
        "required": ["fileName", "scriptName", "actions"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try { params = json::parse(ToUtf8(paramsJson)); }
        catch (const json::parse_error&) { return L"[错误] 参数 JSON 解析失败。"; }

        const std::wstring fileName = FromUtf8(params.value("fileName", ""));
        const std::wstring scriptName = FromUtf8(params.value("scriptName", ""));
        std::vector<json> items;
        if (params.contains("actions") && params["actions"].is_array()) {
            for (const auto& item : params["actions"]) {
                if (item.is_object()) items.push_back(item);
            }
        }
        const auto result = AgentCreateMacroScript(fileName, scriptName, items);
        return result.message;
    };

    return tool;
}

// ── optimizeRecording ─────────────────────────────────────────────
AgentTool MakeOptimizeRecordingTool() {
    AgentTool tool;
    tool.name = L"optimizeRecording";
    tool.description =
        L"优化键鼠录制：默认在 recordings 目录查找并合并移动/等待或压缩路径。"
        L"参数同 optimizeScript，但 dir 默认为 recordings；完成后自动刷新主界面。";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "fileName": { "type": "string", "description": "录制文件名" },
            "outputFileName": { "type": "string", "description": "另存为文件名，省略则覆盖" },
            "mergeMode": {
                "type": "string",
                "enum": ["merge", "compressPath"],
                "description": "merge=合并等待移动, compressPath=路径压缩"
            },
            "waitCalculation": {
                "type": "string",
                "enum": ["sum", "average", "first", "last"]
            },
            "distanceThreshold": { "type": "number" },
            "compressWait": { "type": "number" }
        },
        "required": ["fileName"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try { params = json::parse(ToUtf8(paramsJson)); }
        catch (const json::parse_error&) { return L"[错误] 参数 JSON 解析失败。"; }

        AgentOptimizeOptions opts;
        opts.fileName = FromUtf8(params.value("fileName", ""));
        opts.dir = L"recordings";
        opts.outputFileName = FromUtf8(params.value("outputFileName", ""));
        opts.mergeMode = FromUtf8(params.value("mergeMode", "merge"));
        opts.waitCalculation = FromUtf8(params.value("waitCalculation", "sum"));
        opts.distanceThreshold = params.value("distanceThreshold", 5.0);
        opts.compressWait = params.value("compressWait", 0.05);
        if (opts.distanceThreshold < 0.1) opts.distanceThreshold = 0.1;
        if (opts.compressWait < 0.0) opts.compressWait = 0.0;

        const auto result = AgentOptimizeScriptFile(opts);
        return result.message;
    };

    return tool;
}

// ── deleteScriptFile ──────────────────────────────────────────────
AgentTool MakeDeleteScriptFileTool() {
    AgentTool tool;
    tool.name = L"deleteScriptFile";
    tool.description =
        L"删除鼠标宏（dir=scripts）或键鼠录制（dir=recordings）；删除后自动刷新主界面。";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "fileName": { "type": "string", "description": "要删除的 .json 文件名" },
            "dir": {
                "type": "string",
                "enum": ["scripts", "recordings"],
                "description": "scripts=鼠标宏, recordings=键鼠录制"
            }
        },
        "required": ["fileName", "dir"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        auto p = ParseCommonParams(paramsJson);
        if (p.parseError) return L"[错误] 参数 JSON 解析失败。";
        if (p.fileName.empty()) return L"[错误] 缺少 fileName 参数。";
        if (p.dir != L"scripts" && p.dir != L"recordings")
            return L"[错误] dir 必须为 scripts 或 recordings。";
        const auto result = AgentDeleteScriptFile(p.fileName, p.dir);
        return result.message;
    };

    return tool;
}

// ── listScheduledTasks ────────────────────────────────────────────
AgentTool MakeListScheduledTasksTool() {
    AgentTool tool;
    tool.name = L"listScheduledTasks";
    tool.description = L"列出所有已配置的定时任务，包含名称、类型、执行时间、状态等信息";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {},
        "required": []
    })";

    tool.execute = [](const std::wstring& /*paramsJson*/) -> std::wstring {
        std::vector<ScheduledTask> tasks;
        bool globalDisabled = false;
        LoadScheduledTasks(tasks, &globalDisabled);

        if (tasks.empty()) {
            return L"当前没有配置定时任务。" +
                std::wstring(globalDisabled ? L"\n（定时任务已全局暂停）" : L"");
        }

        std::wstringstream ss;
        ss << L"定时任务列表" << (globalDisabled ? L"（已全局暂停）" : L"") << L"：\n\n";
        int idx = 1;
        for (const auto& t : tasks) {
            std::wstring kindLabel = (t.kind == ScheduledTaskKind::Macro) ? L"鼠标宏" : L"键鼠录制";
            std::wstring freqLabel;
            switch (t.frequency) {
            case ScheduledFrequency::Hourly: freqLabel = L"每小时"; break;
            case ScheduledFrequency::Daily: freqLabel = L"每天"; break;
            case ScheduledFrequency::Weekly: freqLabel = L"每周"; break;
            default: freqLabel = L"单次"; break;
            }
            std::wstring timeStr = FormatScheduledRunTime(t);
            std::wstring statusLabel = (t.status == ScheduledTaskStatus::Enabled) ? L"启用" : L"禁用";

            ss << idx << L". " << t.name << L"\n";
            ss << L"   ID: " << t.id << L"\n";
            ss << L"   类型: " << kindLabel << L"\n";
            ss << L"   目标文件: " << t.fileDisplayName;
            if (!t.fileDisplayName.empty()) ss << L" (" << t.filePath << L")";
            ss << L"\n";
            ss << L"   频率: " << freqLabel << L"\n";
            ss << L"   执行时间: " << timeStr << L"\n";
            ss << L"   状态: " << statusLabel << L"\n\n";
            ++idx;
        }
        return ss.str();
    };

    return tool;
}

// ── createScheduledTask ───────────────────────────────────────────
AgentTool MakeCreateScheduledTaskTool() {
    AgentTool tool;
    tool.name = L"createScheduledTask";
    tool.description = L"创建一个新的定时任务，用于在指定时间自动执行脚本或录制";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "name": {
                "type": "string",
                "description": "任务名称"
            },
            "targetFile": {
                "type": "string",
                "description": "目标文件名（如 myscript.json）。请先通过 listScripts 确认文件存在。"
            },
            "kind": {
                "type": "string",
                "enum": ["macro", "recording"],
                "description": "任务类型：macro=鼠标宏, recording=键鼠录制。默认 macro"
            },
            "frequency": {
                "type": "string",
                "enum": ["custom", "daily", "weekly", "hourly"],
                "description": "执行频率：custom=单次, daily=每天, weekly=每周, hourly=每小时。默认 custom"
            },
            "year": { "type": "integer", "description": "年份（仅 custom 频率需要，如 2026）" },
            "month": { "type": "integer", "description": "月份（仅 custom 频率需要，1-12）" },
            "day": { "type": "integer", "description": "日（仅 custom 频率需要，1-31）" },
            "hour": { "type": "integer", "description": "小时（0-23）。daily/weekly 也需要。默认 9" },
            "minute": { "type": "integer", "description": "分钟（0-59）。默认 0" },
            "second": { "type": "integer", "description": "秒（0-59）。默认 0" },
            "weekDays": {
                "type": "array", "items": { "type": "string" },
                "description": "星期数组（仅 weekly 频率需要）：[\"Mon\",\"Tue\",\"Wed\",\"Thu\",\"Fri\",\"Sat\",\"Sun\"]"
            },
            "enabled": {
                "type": "boolean",
                "description": "是否启用，默认 true"
            }
        },
        "required": ["name"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try { params = json::parse(ToUtf8(paramsJson)); }
        catch (const json::parse_error&) { return L"[错误] 参数 JSON 解析失败。"; }

        std::wstring name = FromUtf8(params.value("name", ""));
        if (name.empty()) return L"[错误] 缺少 name 参数。";

        std::wstring targetFile = FromUtf8(params.value("targetFile", ""));
        std::wstring kindStr = FromUtf8(params.value("kind", "macro"));
        std::wstring freqStr = FromUtf8(params.value("frequency", "custom"));

        std::wstring filePath;
        std::wstring displayName;
        if (!targetFile.empty()) {
            auto found = FindScriptFile(targetFile, L"");
            if (!found.found)
                return L"[错误] 目标文件不存在：" + targetFile + L"。请先用 listScripts 确认文件名。";
            filePath = found.path;
            displayName = targetFile;
        }

        ScheduledTask task;
        task.id = GenerateScheduledTaskId();
        task.name = name;
        task.kind = (kindStr == L"recording") ? ScheduledTaskKind::Recording : ScheduledTaskKind::Macro;
        task.filePath = filePath;
        task.fileDisplayName = displayName;

        if (freqStr == L"hourly") task.frequency = ScheduledFrequency::Hourly;
        else if (freqStr == L"daily") task.frequency = ScheduledFrequency::Daily;
        else if (freqStr == L"weekly") task.frequency = ScheduledFrequency::Weekly;
        else task.frequency = ScheduledFrequency::Custom;

        task.time.year = params.value("year", 0);
        task.time.month = params.value("month", 0);
        task.time.day = params.value("day", 0);
        task.time.hour = params.value("hour", 9);
        task.time.minute = params.value("minute", 0);
        task.time.second = params.value("second", 0);

        if (params.contains("weekDays") && params["weekDays"].is_array()) {
            task.time.weekDays = 0;
            for (const auto& d : params["weekDays"]) {
                std::wstring day = FromUtf8(d.get<std::string>());
                if (day == L"Mon") SetWeekDay(task.time.weekDays, 0, true);
                else if (day == L"Tue") SetWeekDay(task.time.weekDays, 1, true);
                else if (day == L"Wed") SetWeekDay(task.time.weekDays, 2, true);
                else if (day == L"Thu") SetWeekDay(task.time.weekDays, 3, true);
                else if (day == L"Fri") SetWeekDay(task.time.weekDays, 4, true);
                else if (day == L"Sat") SetWeekDay(task.time.weekDays, 5, true);
                else if (day == L"Sun") SetWeekDay(task.time.weekDays, 6, true);
            }
        }

        task.status = params.value("enabled", true)
            ? ScheduledTaskStatus::Enabled : ScheduledTaskStatus::Disabled;

        std::vector<ScheduledTask> tasks;
        bool globalDisabled = false;
        LoadScheduledTasks(tasks, &globalDisabled);
        tasks.push_back(task);

        if (!SaveScheduledTasks(tasks, globalDisabled))
            return L"[错误] 保存定时任务失败。";

        std::wstringstream ss;
        ss << L"定时任务已创建：\n";
        ss << L"  名称: " << task.name << L"\n";
        ss << L"  类型: " << ((task.kind == ScheduledTaskKind::Macro) ? L"鼠标宏" : L"键鼠录制") << L"\n";
        if (!task.filePath.empty()) ss << L"  目标: " << task.fileDisplayName << L"\n";
        ss << L"  执行时间: " << FormatScheduledRunTime(task) << L"\n";
        ss << L"  状态: " << ((task.status == ScheduledTaskStatus::Enabled) ? L"启用" : L"禁用") << L"\n\n";
        ss << L"提示：任务将在下次定时检查时自动生效。";
        return ss.str();
    };

    return tool;
}

// ── updateScheduledTask ───────────────────────────────────────────
AgentTool MakeUpdateScheduledTaskTool() {
    AgentTool tool;
    tool.name = L"updateScheduledTask";
    tool.description = L"更新已有的定时任务。每个修改只影响传入的参数；未传入的参数保持不变";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "taskId": {
                "type": "string",
                "description": "要更新的任务 ID（通过 listScheduledTasks 获取）"
            },
            "name": { "type": "string", "description": "新的任务名称" },
            "targetFile": { "type": "string", "description": "新的目标文件名" },
            "kind": { "type": "string", "enum": ["macro", "recording"], "description": "新的任务类型" },
            "frequency": { "type": "string", "enum": ["custom", "daily", "weekly", "hourly"], "description": "新执行频率" },
            "year": { "type": "integer" }, "month": { "type": "integer" }, "day": { "type": "integer" },
            "hour": { "type": "integer" }, "minute": { "type": "integer" }, "second": { "type": "integer" },
            "weekDays": { "type": "array", "items": { "type": "string" }, "description": "新的星期数组" },
            "enabled": { "type": "boolean", "description": "是否启用" }
        },
        "required": ["taskId"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try { params = json::parse(ToUtf8(paramsJson)); }
        catch (const json::parse_error&) { return L"[错误] 参数 JSON 解析失败。"; }

        std::wstring taskId = FromUtf8(params.value("taskId", ""));
        if (taskId.empty()) return L"[错误] 缺少 taskId 参数。";

        std::vector<ScheduledTask> tasks;
        bool globalDisabled = false;
        LoadScheduledTasks(tasks, &globalDisabled);

        ScheduledTask* target = nullptr;
        for (auto& t : tasks) {
            if (t.id == taskId) { target = &t; break; }
        }
        if (!target) return L"[错误] 未找到 ID 为 " + taskId + L" 的任务。";

        if (params.contains("name")) target->name = FromUtf8(params["name"].get<std::string>());
        if (params.contains("enabled")) target->status = params["enabled"].get<bool>()
            ? ScheduledTaskStatus::Enabled : ScheduledTaskStatus::Disabled;
        if (params.contains("kind")) {
            target->kind = FromUtf8(params["kind"].get<std::string>()) == L"recording"
                ? ScheduledTaskKind::Recording : ScheduledTaskKind::Macro;
        }
        if (params.contains("frequency")) {
            std::wstring f = FromUtf8(params["frequency"].get<std::string>());
            if (f == L"hourly") target->frequency = ScheduledFrequency::Hourly;
            else if (f == L"daily") target->frequency = ScheduledFrequency::Daily;
            else if (f == L"weekly") target->frequency = ScheduledFrequency::Weekly;
            else target->frequency = ScheduledFrequency::Custom;
        }
        if (params.contains("year")) target->time.year = params["year"].get<int>();
        if (params.contains("month")) target->time.month = params["month"].get<int>();
        if (params.contains("day")) target->time.day = params["day"].get<int>();
        if (params.contains("hour")) target->time.hour = params["hour"].get<int>();
        if (params.contains("minute")) target->time.minute = params["minute"].get<int>();
        if (params.contains("second")) target->time.second = params["second"].get<int>();
        if (params.contains("weekDays") && params["weekDays"].is_array()) {
            target->time.weekDays = 0;
            for (const auto& d : params["weekDays"]) {
                std::wstring day = FromUtf8(d.get<std::string>());
                if (day == L"Mon") SetWeekDay(target->time.weekDays, 0, true);
                else if (day == L"Tue") SetWeekDay(target->time.weekDays, 1, true);
                else if (day == L"Wed") SetWeekDay(target->time.weekDays, 2, true);
                else if (day == L"Thu") SetWeekDay(target->time.weekDays, 3, true);
                else if (day == L"Fri") SetWeekDay(target->time.weekDays, 4, true);
                else if (day == L"Sat") SetWeekDay(target->time.weekDays, 5, true);
                else if (day == L"Sun") SetWeekDay(target->time.weekDays, 6, true);
            }
        }
        if (params.contains("targetFile")) {
            std::wstring newFile = FromUtf8(params["targetFile"].get<std::string>());
            auto found = FindScriptFile(newFile, L"");
            if (!found.found) return L"[错误] 目标文件不存在：" + newFile;
            target->filePath = found.path;
            target->fileDisplayName = newFile;
        }

        if (!SaveScheduledTasks(tasks, globalDisabled))
            return L"[错误] 保存定时任务失败。";

        return L"定时任务已更新：" + target->name + L"\n"
               L"  新的执行时间: " + FormatScheduledRunTime(*target);
    };

    return tool;
}

// ── deleteScheduledTask ───────────────────────────────────────────
AgentTool MakeDeleteScheduledTaskTool() {
    AgentTool tool;
    tool.name = L"deleteScheduledTask";
    tool.description = L"删除指定的定时任务";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "taskId": {
                "type": "string",
                "description": "要删除的任务 ID"
            }
        },
        "required": ["taskId"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try { params = json::parse(ToUtf8(paramsJson)); }
        catch (const json::parse_error&) { return L"[错误] 参数 JSON 解析失败。"; }

        std::wstring taskId = FromUtf8(params.value("taskId", ""));
        if (taskId.empty()) return L"[错误] 缺少 taskId 参数。";

        std::vector<ScheduledTask> tasks;
        bool globalDisabled = false;
        LoadScheduledTasks(tasks, &globalDisabled);

        std::wstring deletedName;
        auto it = std::remove_if(tasks.begin(), tasks.end(),
            [&](const ScheduledTask& t) {
                if (t.id == taskId) { deletedName = t.name; return true; }
                return false;
            });
        if (it == tasks.end()) return L"[错误] 未找到 ID 为 " + taskId + L" 的任务。";
        tasks.erase(it, tasks.end());

        if (!SaveScheduledTasks(tasks, globalDisabled))
            return L"[错误] 保存定时任务失败。";

        return L"已删除定时任务：" + deletedName;
    };

    return tool;
}

// ── listSettings ──────────────────────────────────────────────────
AgentTool MakeListSettingsTool() {
    AgentTool tool;
    tool.name = L"listSettings";
    tool.description = L"列出当前的应用设置，包含点击设置、宏回放设置、其他设置（不包含 AI 助手自身的设置）";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {},
        "required": []
    })";

    tool.execute = [](const std::wstring& /*paramsJson*/) -> std::wstring {
        quickscript::AppSettings settings;
        LoadAppSettings(settings);

        std::wstringstream ss;
        ss << L"当前应用设置：\n\n";

        ss << L"【点击设置】\n";
        ss << L"  随机间隔: " << (settings.click.enableRandomInterval ? L"启用" : L"禁用")
           << (settings.click.enableRandomInterval ? L"（最大 " + std::to_wstring(settings.click.randomIntervalMaxSeconds) + L" 秒）" : L"") << L"\n";
        ss << L"  按键按下释放间隔: " << (settings.click.enablePressReleaseInterval ? L"启用" : L"禁用")
           << (settings.click.enablePressReleaseInterval ? L"（" + std::to_wstring(settings.click.pressReleaseIntervalSeconds) + L" 秒）" : L"") << L"\n";
        ss << L"  坐标抖动: " << (settings.click.enableCoordinateJitter ? L"启用" : L"禁用")
           << (settings.click.enableCoordinateJitter ? L"（X:" + std::to_wstring(settings.click.jitterX) + L" Y:" + std::to_wstring(settings.click.jitterY) + L"）" : L"") << L"\n";
        ss << L"  固定坐标: " << (settings.click.enableFixedCoordinates ? L"启用" : L"禁用")
           << (settings.click.enableFixedCoordinates ? L"（X:" + std::to_wstring(settings.click.fixedX) + L" Y:" + std::to_wstring(settings.click.fixedY) + L"）" : L"") << L"\n";
        ss << L"  点击次数限制: " << (settings.click.enableClickCountLimit ? L"启用" : L"禁用")
           << (settings.click.enableClickCountLimit ? L"（" + std::to_wstring(settings.click.clickCountLimit) + L" 次）" : L"") << L"\n\n";

        ss << L"【宏回放设置】\n";
        ss << L"  回放次数限制: " << (settings.playback.enablePlaybackCount ? L"启用" : L"禁用（无限循环）")
           << (settings.playback.enablePlaybackCount ? L"（" + std::to_wstring(settings.playback.playbackCount) + L" 次）" : L"") << L"\n";
        ss << L"  回放间隔: " << (settings.playback.enablePlaybackInterval ? L"启用" : L"禁用")
           << (settings.playback.enablePlaybackInterval ? L"（" + std::to_wstring(settings.playback.playbackIntervalMinSeconds) + L"~" + std::to_wstring(settings.playback.playbackIntervalMaxSeconds) + L" 秒）" : L"") << L"\n";
        ss << L"  调试输出窗口: " << (settings.playback.enableDebugOutputWindow ? L"启用" : L"禁用") << L"\n";
        ss << L"  关键函数调试: " << (settings.playback.autoOutputKeyFunctionDebug ? L"启用" : L"禁用") << L"\n\n";

        ss << L"【其他设置】\n";
        ss << L"  宏执行后自动隐藏主窗口: " << (settings.other.autoHideMainWindow ? L"是" : L"否") << L"\n";
        ss << L"  宏启动时播放提示音: " << (settings.other.playSoundOnStart ? L"是" : L"否") << L"\n";
        ss << L"  隐藏右下角弹窗提示: " << (settings.other.hideBottomRightTip ? L"是" : L"否") << L"\n";
        ss << L"  关闭按钮最小化到托盘: " << (settings.other.closeToTray ? L"是" : L"否") << L"\n";

        ss << L"\n修改设置请使用 updateSettings 工具。";
        return ss.str();
    };

    return tool;
}

// ── updateSettings ────────────────────────────────────────────────
AgentTool MakeUpdateSettingsTool() {
    AgentTool tool;
    tool.name = L"updateSettings";
    tool.description = L"修改应用设置。每个修改只影响传入的参数；未传入的参数保持不变。不可以修改 AI 助手自身的设置";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "category": {
                "type": "string",
                "enum": ["click", "playback", "other"],
                "description": "设置分类：click=点击设置, playback=宏回放设置, other=其他设置"
            },
            "enableRandomInterval": { "type": "boolean", "description": "启用随机间隔" },
            "randomIntervalMaxSeconds": { "type": "number", "description": "随机间隔最大值（秒）" },
            "enablePressReleaseInterval": { "type": "boolean", "description": "启用按键按下释放间隔" },
            "pressReleaseIntervalSeconds": { "type": "number", "description": "按键按下释放间隔（秒）" },
            "enableCoordinateJitter": { "type": "boolean", "description": "启用坐标抖动" },
            "jitterX": { "type": "integer", "description": "坐标抖动 X 范围" },
            "jitterY": { "type": "integer", "description": "坐标抖动 Y 范围" },
            "enableFixedCoordinates": { "type": "boolean", "description": "启用固定坐标" },
            "fixedX": { "type": "integer", "description": "固定坐标 X" },
            "fixedY": { "type": "integer", "description": "固定坐标 Y" },
            "enableClickCountLimit": { "type": "boolean", "description": "启用点击次数限制" },
            "clickCountLimit": { "type": "integer", "description": "点击次数限制" },
            "enablePlaybackCount": { "type": "boolean", "description": "启用回放次数限制；false=无限循环" },
            "playbackCount": { "type": "integer", "description": "回放次数" },
            "enablePlaybackInterval": { "type": "boolean", "description": "启用回放间隔" },
            "playbackIntervalMinSeconds": { "type": "number", "description": "回放间隔最小值（秒）" },
            "playbackIntervalMaxSeconds": { "type": "number", "description": "回放间隔最大值（秒）" },
            "enableDebugOutputWindow": { "type": "boolean", "description": "启用调试输出窗口" },
            "autoOutputKeyFunctionDebug": { "type": "boolean", "description": "自动输出关键函数调试信息" },
            "autoHideMainWindow": { "type": "boolean", "description": "宏执行后自动隐藏主窗口" },
            "playSoundOnStart": { "type": "boolean", "description": "宏启动时播放提示音" },
            "hideBottomRightTip": { "type": "boolean", "description": "隐藏右下角弹窗提示" },
            "closeToTray": { "type": "boolean", "description": "关闭按钮最小化到托盘" }
        },
        "required": ["category"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try { params = json::parse(ToUtf8(paramsJson)); }
        catch (const json::parse_error&) { return L"[错误] 参数 JSON 解析失败。"; }

        std::wstring category = FromUtf8(params.value("category", ""));
        if (category.empty()) return L"[错误] 缺少 category 参数。";

        quickscript::AppSettings settings;
        LoadAppSettings(settings);

        auto setBool = [&](const char* key, bool& target) {
            if (params.contains(key)) target = params[key].get<bool>();
        };
        auto setInt = [&](const char* key, int& target) {
            if (params.contains(key)) target = params[key].get<int>();
        };
        auto setDouble = [&](const char* key, double& target) {
            if (params.contains(key)) target = params[key].get<double>();
        };

        if (category == L"click") {
            setBool("enableRandomInterval", settings.click.enableRandomInterval);
            setDouble("randomIntervalMaxSeconds", settings.click.randomIntervalMaxSeconds);
            setBool("enablePressReleaseInterval", settings.click.enablePressReleaseInterval);
            setDouble("pressReleaseIntervalSeconds", settings.click.pressReleaseIntervalSeconds);
            setBool("enableCoordinateJitter", settings.click.enableCoordinateJitter);
            setInt("jitterX", settings.click.jitterX);
            setInt("jitterY", settings.click.jitterY);
            setBool("enableFixedCoordinates", settings.click.enableFixedCoordinates);
            setInt("fixedX", settings.click.fixedX);
            setInt("fixedY", settings.click.fixedY);
            setBool("enableClickCountLimit", settings.click.enableClickCountLimit);
            setInt("clickCountLimit", settings.click.clickCountLimit);
        } else if (category == L"playback") {
            setBool("enablePlaybackCount", settings.playback.enablePlaybackCount);
            setInt("playbackCount", settings.playback.playbackCount);
            setBool("enablePlaybackInterval", settings.playback.enablePlaybackInterval);
            setDouble("playbackIntervalMinSeconds", settings.playback.playbackIntervalMinSeconds);
            setDouble("playbackIntervalMaxSeconds", settings.playback.playbackIntervalMaxSeconds);
            setBool("enableDebugOutputWindow", settings.playback.enableDebugOutputWindow);
            setBool("autoOutputKeyFunctionDebug", settings.playback.autoOutputKeyFunctionDebug);
        } else if (category == L"other") {
            setBool("autoHideMainWindow", settings.other.autoHideMainWindow);
            setBool("playSoundOnStart", settings.other.playSoundOnStart);
            setBool("hideBottomRightTip", settings.other.hideBottomRightTip);
            setBool("closeToTray", settings.other.closeToTray);
        } else {
            return L"[错误] 无效的 category：" + category + L"。可选值：click, playback, other";
        }

        if (!SaveAppSettings(settings))
            return L"[错误] 保存设置失败。";

        return L"设置已更新（" + category + L" 分类）。\n"
               L"回放类设置会在当前宏的下一轮循环自动生效；其他设置在下次启动宏时生效。";
    };

    return tool;
}

// ── listAiModels ──────────────────────────────────────────────────
AgentTool MakeListAiModelsTool() {
    AgentTool tool;
    tool.name = L"listAiModels";
    tool.description =
        L"列出用户在「设置→AI助手」中已添加的 AI 模型，并标注是否支持识图。"
        L"添加 aiTextAnalysis / aiImageAnalysis / aiActionExecute 前可先调用此工具。";
    tool.parameters_json = LR"({"type":"object","properties":{}})";
    tool.execute = [](const std::wstring&) -> std::wstring {
        return FormatAvailableAiModelsList(LoadAgentAppSettings().ai);
    };
    return tool;
}

// ── buildGetCursorPosAction ───────────────────────────────────────
AgentTool MakeBuildGetCursorPosActionTool() {
    AgentTool tool;
    tool.name = L"buildGetCursorPosAction";
    tool.description =
        L"构建「获取鼠标位置」动作。结果存入变量，后续可用 {变量名}.x / {变量名}.y 引用屏幕坐标。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "matchVarName": { "type": "string", "description": "变量名，默认 cursor" },
            "remark": { "type": "string" },
            "no": { "type": "integer" },
            "indent": { "type": "integer" }
        },
        "required": []
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        params["type"] = "getCursorPos";
        return BuildSingleAgentActionResult(params);
    };
    return tool;
}

// ── buildAiTextAnalysisAction ─────────────────────────────────────
AgentTool MakeBuildAiTextAnalysisActionTool() {
    AgentTool tool;
    tool.name = L"buildAiTextAnalysisAction";
    tool.description =
        L"构建「AI 文本分析」动作。aiPrompt 必填；aiModelName 可省略，工具会从已添加模型中自动选择。"
        L"不确定可用模型时先 listAiModels。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "aiPrompt": { "type": "string", "description": "提示词/分析问题（必填）" },
            "aiModelName": { "type": "string", "description": "可选；省略时自动选择已添加模型" },
            "aiOutputVarName": { "type": "string", "description": "输出变量名，默认 aiResult" },
            "aiOutputType": { "type": "integer", "description": "0=文本 1=数字" },
            "aiContextMode": { "type": "integer", "description": "0无/1宏/2循环/3块" },
            "aiTimeoutSec": { "type": "integer" },
            "aiFallbackValue": { "type": "string" },
            "remark": { "type": "string" },
            "no": { "type": "integer" },
            "indent": { "type": "integer" }
        },
        "required": ["aiPrompt"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.contains("aiPrompt") || Trim(FromUtf8(params["aiPrompt"].get<std::string>())).empty())
            return L"[错误] 缺少 aiPrompt（提示词）。";
        const AiModelFillResult model = FillResolvedAiModel(params, false);
        if (!model.ok) return model.error;
        params["type"] = "aiTextAnalysis";
        return BuildSingleAgentActionResult(params, model.note);
    };
    return tool;
}

// ── buildAiImageAnalysisAction ────────────────────────────────────
AgentTool MakeBuildAiImageAnalysisActionTool() {
    AgentTool tool;
    tool.name = L"buildAiImageAnalysisAction";
    tool.description =
        L"构建「AI 图片分析」动作。aiPrompt 必填；自动从已添加模型中选择支持识图的模型（优先识图模型）。"
        L"可指定截屏区域、缩放等参数。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "aiPrompt": { "type": "string", "description": "对截图的分析问题（必填）" },
            "aiModelName": { "type": "string", "description": "可选；省略时自动选择识图模型" },
            "aiOutputVarName": { "type": "string", "description": "默认 aiImgResult" },
            "aiOutputType": { "type": "integer", "description": "0=文本 1=数字" },
            "aiImageScale": { "type": "number", "description": "截屏缩放 0.1~1" },
            "aiRegionByImage": { "type": "boolean", "description": "是否按找图区域截屏" },
            "aiTargetImagePath": { "type": "string" },
            "aiSearchX1": { "type": "integer" },
            "aiSearchY1": { "type": "integer" },
            "aiSearchX2": { "type": "integer" },
            "aiSearchY2": { "type": "integer" },
            "aiContextMode": { "type": "integer" },
            "aiTimeoutSec": { "type": "integer" },
            "aiFallbackValue": { "type": "string" },
            "remark": { "type": "string" },
            "no": { "type": "integer" },
            "indent": { "type": "integer" }
        },
        "required": ["aiPrompt"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.contains("aiPrompt") || Trim(FromUtf8(params["aiPrompt"].get<std::string>())).empty())
            return L"[错误] 缺少 aiPrompt（提示词）。";
        const AiModelFillResult model = FillResolvedAiModel(params, true);
        if (!model.ok) return model.error;
        params["type"] = "aiImageAnalysis";
        return BuildSingleAgentActionResult(params, model.note);
    };
    return tool;
}

// ── buildAiActionExecuteAction ────────────────────────────────────
AgentTool MakeBuildAiActionExecuteActionTool() {
    AgentTool tool;
    tool.name = L"buildAiActionExecuteAction";
    tool.description =
        L"构建「AI 动作执行」动作。aiPrompt 为任务描述；aiWithImage=1 时需识图模型，工具会自动选择。"
        L"可设置最大步数、超时、执行前确认等。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "aiPrompt": { "type": "string", "description": "任务描述（必填）" },
            "aiModelName": { "type": "string", "description": "可选；省略时自动选择" },
            "aiWithImage": { "type": "boolean", "description": "是否带截图，默认 true" },
            "aiRegionByImage": { "type": "boolean" },
            "aiTargetImagePath": { "type": "string" },
            "aiSearchX1": { "type": "integer" },
            "aiSearchY1": { "type": "integer" },
            "aiSearchX2": { "type": "integer" },
            "aiSearchY2": { "type": "integer" },
            "aiMaxSteps": { "type": "integer", "description": "默认 10，-1 不限" },
            "aiTimeoutSec": { "type": "integer" },
            "aiConfirmExecute": { "type": "boolean" },
            "aiContextMode": { "type": "integer" },
            "aiFallbackValue": { "type": "string" },
            "remark": { "type": "string" },
            "no": { "type": "integer" },
            "indent": { "type": "integer" }
        },
        "required": ["aiPrompt"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.contains("aiPrompt") || Trim(FromUtf8(params["aiPrompt"].get<std::string>())).empty())
            return L"[错误] 缺少 aiPrompt（任务描述）。";
        const bool withImage = [&]() {
            if (!params.contains("aiWithImage")) return true;
            if (params["aiWithImage"].is_boolean()) return params["aiWithImage"].get<bool>();
            if (params["aiWithImage"].is_number_integer()) return params["aiWithImage"].get<int>() != 0;
            return true;
        }();
        const AiModelFillResult model = FillResolvedAiModel(params, withImage);
        if (!model.ok) return model.error;
        params["type"] = "aiActionExecute";
        return BuildSingleAgentActionResult(params, model.note);
    };
    return tool;
}

std::vector<AgentTool> BuildDefaultAgentTools() {
    std::vector<AgentTool> tools;
    tools.push_back(MakeListScriptsTool());
    tools.push_back(MakeReadScriptTool());
    tools.push_back(MakeWriteScriptTool());
    tools.push_back(MakeReadScriptReferenceTool());
    tools.push_back(MakeReadAgentSkillTool());
    tools.push_back(MakeBuildScriptActionsTool());
    tools.push_back(MakeListAiModelsTool());
    tools.push_back(MakeBuildGetCursorPosActionTool());
    tools.push_back(MakeBuildAiTextAnalysisActionTool());
    tools.push_back(MakeBuildAiImageAnalysisActionTool());
    tools.push_back(MakeBuildAiActionExecuteActionTool());
    tools.push_back(MakeCreateMacroScriptTool());
    tools.push_back(MakeOptimizeRecordingTool());
    tools.push_back(MakeDeleteScriptFileTool());
    tools.push_back(MakeGetScriptStatsTool());
    tools.push_back(MakeOptimizeScriptTool());
    tools.push_back(MakeListScheduledTasksTool());
    tools.push_back(MakeCreateScheduledTaskTool());
    tools.push_back(MakeUpdateScheduledTaskTool());
    tools.push_back(MakeDeleteScheduledTaskTool());
    tools.push_back(MakeListSettingsTool());
    tools.push_back(MakeUpdateSettingsTool());
    return tools;
}
