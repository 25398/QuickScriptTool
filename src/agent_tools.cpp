// ──────────────────────────────────────────────────────────────────
// agent_tools.cpp — AI Agent 工具实现
// 脚本读写 / 录制优化 / 定时任务管理 / 应用设置修改
// 所有工具的工厂函数
// ──────────────────────────────────────────────────────────────────

#include "agent_tools.h"

#include "action_utils.h"
#include "ai_logic_convert.h"
#include "app_settings_store.h"
#include "ui_scale.h"
#include "scheduled_task_store.h"
#include "scheduled_task_types.h"
#include "agent_script_ops.h"
#include "agent_reference.h"
#include "agent_ai_actions.h"
#include "agent_ui_notify.h"
#include "agent_undo.h"
#include "agent_shell.h"
#include "agent_web.h"
#include "recorder_timeline.h"
#include "script_action_builder.h"
#include "script_io.h"
#include "utils.h"
#include "window_mode/window_mode_json.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>

namespace {

// 整文件级撤销快照（settings / scheduled_tasks.json）。
struct AgentFileUndo {
    std::wstring id;
    std::wstring path;
    bool done = false;

    AgentFileUndo(const std::wstring& tool, const std::wstring& title,
                  const std::wstring& filePath) {
        path = filePath;
        id = AgentUndoBegin(tool, title, path, ReadAll(path),
                            GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
    }

    ~AgentFileUndo() {
        if (!done) AgentUndoFinish(id, L"", false);
    }

    void Success() {
        if (done) return;
        done = true;
        AgentUndoFinish(id, ReadAll(path), true);
    }
};

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

    std::wstring found;
    std::wstring hintDir = DirFromHint(dirHint);
    if (!hintDir.empty()) {
        if (FindScriptJsonByFileName(hintDir, fileName, found))
            return { found, true };
        std::wstring label = (dirHint == L"recordings") ? L"键鼠录制目录" : L"脚本宏目录";
        return { L"[错误] 在" + label + L"中未找到文件：" + fileName, false };
    }

    if (ResolveLibraryScriptPath(fileName, found))
        return { found, true };
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

// 列出单个目录下的脚本（只解析 name/actions 长度，避免大录制全量 ScriptAction 解析卡住）
void ListScriptsInDir(const std::wstring& dir, const std::wstring& label,
                      std::wstringstream& result, int& index) {
    std::wstring pattern = dir + L"\\*.json";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    struct Entry {
        std::wstring fileName;
        std::wstring displayName;
        int actionCount = 0;
        std::wstring modeSummary;
        int breakoutSec = 0;
        bool windowEnabled = false;
    };
    std::vector<Entry> entries;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fileName(fd.cFileName);
        if (fileName.size() < 5 || fileName.substr(fileName.size() - 5) != L".json") continue;
        std::wstring fullPath = dir + L"\\" + fileName;
        Entry e;
        e.fileName = fileName;
        e.displayName = fileName;
        try {
            const std::wstring raw = ReadAll(fullPath);
            const json j = json::parse(ToUtf8(raw));
            if (j.contains("scriptName") && j["scriptName"].is_string())
                e.displayName = FromUtf8(j["scriptName"].get<std::string>());
            else if (j.contains("name") && j["name"].is_string())
                e.displayName = FromUtf8(j["name"].get<std::string>());
            if (j.contains("actions") && j["actions"].is_array())
                e.actionCount = static_cast<int>(j["actions"].size());
            if (j.contains("windowMode") && j["windowMode"].is_object()) {
                const auto& wm = j["windowMode"];
                e.windowEnabled = wm.value("enabled", 0) != 0;
            }
            if (!e.windowEnabled && j.contains("breakoutTimeSeconds") && j["breakoutTimeSeconds"].is_number())
                e.breakoutSec = static_cast<int>(j["breakoutTimeSeconds"].get<double>());
            // 轻量摘要：避免再 LoadScriptFileData；模式文案用 enabled 即可
            e.modeSummary = e.windowEnabled ? L"窗口模式" : L"默认模式";
        } catch (...) {
            // 坏文件仍列出，动作数未知
            e.displayName = fileName;
            e.modeSummary = L"无法解析";
        }
        entries.push_back(std::move(e));
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if (entries.empty()) return;

    result << L"\n=== " << label << L" ===\n";
    for (const auto& e : entries) {
        result << index << L". " << e.fileName << L" — \"" << e.displayName << L"\" ("
            << e.actionCount << L" 个动作, " << e.modeSummary;
        if (!e.windowEnabled && e.breakoutSec > 0)
            result << L", 脱离" << e.breakoutSec << L"s";
        result << L")\n";
        ++index;
    }
}

// ── 录制优化：实现见 agent_script_ops（AgentOptimizeScriptFile）────────

/// 统计数据，写入 ss
void WriteScriptStats(const ScriptFileData& data, std::wstringstream& ss) {
    int moveCount = 0, waitCount = 0, keyCount = 0;
    double totalWait = 0;
    int loopCount = 0, ifCount = 0, findImageCount = 0, clickCount = 0;
    int keyDownCount = 0, keyUpCount = 0, keyClickCount = 0;
    int otherCount = 0;
    int substantive = 0;

    for (const auto& a : data.actions) {
        if (a.type != ActionType::Wait) ++substantive;
        switch (a.type) {
        case ActionType::MoveMouse: ++moveCount; break;
        case ActionType::Wait:
            ++waitCount;
            totalWait += ActionStepUs(a) / 1000000.0;
            break;
        case ActionType::Loop: ++loopCount; ++keyCount; break;
        case ActionType::EndLoop: ++keyCount; break;
        case ActionType::If: ++ifCount; ++keyCount; break;
        case ActionType::Else: ++keyCount; break;
        case ActionType::FindImage: ++findImageCount; ++keyCount; break;
        case ActionType::MultiMatch: ++findImageCount; ++keyCount; break;
        case ActionType::MouseClick: case ActionType::MouseDown: case ActionType::MouseUp:
            ++clickCount; ++keyCount; break;
        case ActionType::KeyDown: ++keyDownCount; ++keyCount; break;
        case ActionType::KeyUp: ++keyUpCount; ++keyCount; break;
        case ActionType::KeyClick: ++keyClickCount; ++keyCount; break;
        default: ++otherCount; ++keyCount; break;
        }
    }

    ss << L"脚本统计: " << data.scriptName << L"\n";
    ss << L"运行模式: " << windowmode::WindowModeConfigSummary(data.windowMode) << L"\n";
    if (!data.windowMode.enabled) {
        ss << L"脱离时间: " << EffectiveBreakoutTimeSeconds(data) << L" 秒\n";
    }
    ss << L"总动作数: " << data.actions.size()
       << L"（含显式等待；非等待 " << substantive << L"）\n";
    ss << L"总等待时长: " << totalWait << L" 秒\n";
    ss << L"记录时间: " << data.recordTime << L"\n\n";

    ss << L"动作分类:\n";
    ss << L"  鼠标移动: " << moveCount << L"\n";
    ss << L"  等待: " << waitCount << L" (合计 " << totalWait << L"秒，含 timingUs)\n";
    ss << L"  鼠标点击: " << clickCount << L"\n";
    ss << L"  按键: 按下 " << keyDownCount << L" 次, 松开 " << keyUpCount << L" 次, 点击 " << keyClickCount << L" 次\n";
    ss << L"  识图: " << findImageCount << L"\n";
    ss << L"  循环: " << loopCount << L"\n";
    ss << L"  条件: " << ifCount << L"\n";
    if (otherCount > 0) ss << L"  其他: " << otherCount << L"\n";

    ss << L"\n可优化分段（关键操作之间的 Move+Wait 连续块）:\n";
    int segmentIdx = 0;
    size_t segStart = 0;
    bool hasMoveInSeg = false;
    int segMoves = 0, segWaits = 0;
    constexpr int kMaxListedSegments = 40;

    auto flushSegment = [&](size_t endIdx) {
        if (hasMoveInSeg && endIdx > segStart) {
            ++segmentIdx;
            if (segmentIdx <= kMaxListedSegments) {
                size_t keyStart = segStart > 0 ? segStart - 1 : 0;
                size_t keyEnd = endIdx < data.actions.size() ? endIdx : data.actions.size() - 1;
                std::wstring startLabel = (segStart == 0) ? L"开头"
                    : (L"第" + std::to_wstring(data.actions[keyStart].originalNo) + L"步 "
                        + ActionName(data.actions[keyStart]));
                std::wstring endLabel = (endIdx >= data.actions.size()) ? L"结尾"
                    : (L"第" + std::to_wstring(data.actions[keyEnd].originalNo) + L"步 "
                        + ActionName(data.actions[keyEnd]));
                ss << L"  " << segmentIdx << L". [" << startLabel << L" → " << endLabel << L"] — "
                   << segMoves << L" 个移动, " << segWaits << L" 个等待\n";
            }
        }
        hasMoveInSeg = false;
        segMoves = 0;
        segWaits = 0;
    };

    for (size_t i = 0; i < data.actions.size(); ++i) {
        const auto t = data.actions[i].type;
        const bool isKey = t != ActionType::MoveMouse
            && t != ActionType::MoveMouseRelative
            && t != ActionType::Wait;
        if (isKey) { flushSegment(i); segStart = i + 1; }
        else {
            if (t == ActionType::MoveMouse || t == ActionType::MoveMouseRelative) {
                hasMoveInSeg = true;
                ++segMoves;
            }
            if (t == ActionType::Wait) ++segWaits;
        }
    }
    flushSegment(data.actions.size());

    if (segmentIdx == 0) ss << L"  (无可合并分段)\n";
    else {
        if (segmentIdx > kMaxListedSegments) {
            ss << L"  ...（另有 " << (segmentIdx - kMaxListedSegments)
               << L" 个分段未列出）\n";
        }
        ss << L"\n共 " << segmentIdx << L" 个可合并分段。"
            L"用户要优化时请调用 optimizeRecording / optimizeScript，mergeMode=merge"
            L"（与产品「鼠标移动合并」相同，通常大幅减少动作数）。"
            L"不要用 compressPath，除非用户明确要求压缩路径/去掉过密移动点（动作数只会略减）。\n";
    }
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
    tool.description =
        L"读取脚本/录制的【两级说明】（自动在 scripts 和 recordings 目录下查找）："
        L"默认只输出动作概览（每步=动作名+类型语义+备注，不含坐标/时长/阈值等数值），"
        L"先让 AI 模糊理解脚本意图；需要具体参数分析时传 detail=true 查详细说明"
        L"（含图片/坐标/条件/间隔/修饰键/容差/AI 超时等），或 raw=true 看原始 JSON（单次 ≤32KB）。"
        L"按 startIndex/maxActions 分页（每页 ≤60 步），不返回完整 JSON，避免长输入卡死 AI。"
        L"脚本引用图片输出 [[AGENT_IMG:路径]] 标记（多模态自动嵌入，非多模态降级为路径提示）。"
        L"优化路径请直接用 optimizeScript / optimizeRecording，不必先 read 全文。";

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
            },
            "includeImages": {
                "type": "boolean",
                "description": "是否输出脚本引用图片（找图/OCR/截图模板）的 [[AGENT_IMG:路径]] 标记，默认 true；多模态模型会自动嵌入图片，非多模态降级为路径文本"
            },
            "raw": {
                "type": "boolean",
                "description": "false=返回说明（默认）；true=返回原始 JSON（单次 ≤32KB，超长截断并提示改读摘要）"
            },
            "detail": {
                "type": "boolean",
                "description": "false=动作概览（动作名+备注，默认，先模糊理解）；true=详细说明（含坐标/时长/阈值等具体参数，需要分析时再查）"
            },
            "startIndex": {
                "type": "integer",
                "description": "动作分页偏移（0 起），大脚本逐页读取，默认 0"
            },
            "maxActions": {
                "type": "integer",
                "description": "本页最多展示多少步，默认 60（建议 ≤60 防止长输入）"
            }
        },
        "required": ["fileName"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        auto p = ParseCommonParams(paramsJson);
        if (p.parseError) return L"[错误] 参数 JSON 解析失败。";
        if (p.fileName.empty()) return L"[错误] 缺少 fileName 参数。";
        bool includeImages = true;
        bool raw = false;
        bool detail = false;
        size_t startIndex = 0;
        size_t maxActions = 60;
        try {
            const json params = json::parse(ToUtf8(paramsJson));
            if (params.contains("includeImages") && params["includeImages"].is_boolean())
                includeImages = params["includeImages"].get<bool>();
            if (params.contains("raw") && params["raw"].is_boolean())
                raw = params["raw"].get<bool>();
            if (params.contains("detail") && params["detail"].is_boolean())
                detail = params["detail"].get<bool>();
            if (params.contains("startIndex") && params["startIndex"].is_number_integer())
                startIndex = static_cast<size_t>(params["startIndex"].get<int>());
            if (params.contains("maxActions") && params["maxActions"].is_number_integer())
                maxActions = static_cast<size_t>(params["maxActions"].get<int>());
        } catch (...) {}

        auto found = FindScriptFile(p.fileName, p.dir);
        if (!found.found) return found.path;

        std::wstring content = ReadAll(found.path);
        if (content.empty()) return L"[提示] 文件内容为空：" + p.fileName;

        ScriptFileData data = LoadScriptFileData(found.path);

        std::wstring out;
        // 默认输出「泛化精炼说明」：把每个动作翻译成一句中文（含关键参数），
        // 省略默认字段，按 startIndex/maxActions 分页，防止长 JSON 输入卡死 AI。
        // 确需原始 JSON 时才传 raw=true（仍限制单次大小，超长提示改读摘要）。
        out += L"文件: " + p.fileName + L"\n";
        out += L"动作数: " + std::to_wstring(data.actions.size()) + L"\n";
        out += L"[脚本模式] " + windowmode::WindowModeConfigSummary(data.windowMode) + L"\n";
        if (!data.windowMode.enabled) {
            out += L"[脱离时间] " + std::to_wstring(
                static_cast<int>(EffectiveBreakoutTimeSeconds(data))) + L" 秒（0=禁用）\n";
        }
        if (raw) {
            constexpr size_t kMaxRawJsonChars = 32 * 1024;
            if (content.size() > kMaxRawJsonChars) {
                out += L"\n[原始 JSON 过长（" + std::to_wstring(content.size())
                    + L" 字符，上限 32KB）— 建议使用默认精炼说明并按 startIndex 分页，"
                    + L"或用 getScriptStats 看统计。前 16KB 预览：\n";
                out += content.substr(0, 16 * 1024);
                out += L"\n…(截断)";
            } else {
                out += L"\n" + content;
            }
        } else {
            size_t shown = 0;
            out += L"\n" + (detail
                ? DescribeScriptActionsDetail(data.actions, startIndex, maxActions, shown)
                : DescribeScriptActionsBrief(data.actions, startIndex, maxActions, shown));
            out += L"[提示] 概览按“动作名+备注”理解意图；需要坐标/时长/阈值等具体参数时"
                L" 传 detail=true（或 raw=true 看原始 JSON），每次只读关键片段。\n";
        }
        if (includeImages) {
            const auto imgPaths = CollectImagePathsFromJson(content);
            if (!imgPaths.empty()) {
                out += L"\n[脚本引用图片]";
                int idx = 1;
                for (const auto& img : imgPaths) {
                    out += L"\n" + std::to_wstring(idx++) + L". ";
                    if (GetFileAttributesW(img.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        out += L"[[AGENT_IMG:" + img + L"]]";
                    } else {
                        out += L"[缺失图片] " + img;
                    }
                }
                out += L"\n多模态模型会自动读取图片内容；非多模态会降级为路径提示，不会报错。";
            }
        }
        return out;
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

    std::vector<json> flat;
    std::wstring flattenErr;
    FlattenNestedActionParamList(items, flat, flattenErr);
    std::vector<ScriptAction> built;
    built.reserve(flat.size());
    for (const auto& item : flat) {
        auto builtOne = BuildScriptActionFromJson(item);
        if (builtOne.ok) built.push_back(std::move(builtOne.action));
    }
    if (!built.empty()) {
        EnsureStopMacroOnActions(built);
        NormalizeScriptActionList(built);
    }
    std::wstring summary = L"✓ 已构建 " + std::to_wstring(built.empty() ? items.size() : built.size())
        + L" 个动作。\n";
    summary += L"将下方 JSON 数组嵌入脚本的 actions 字段即可：\n\n";
    summary += jsonArray;
    if (!built.empty())
        summary += L"\n\n" + FormatScriptActionsOutline(built);
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
        L"必填参数：keyClick/keyDown/keyUp→keyText；quickInput→inputText；wait→duration；"
        L"findImage→imagePath；if→conditionExpr；goto→gotoStepExpr；runMacro→targetPath；"
        L"openFile/runProgram/openWebpage→targetPath；AI 动作→aiPrompt。缺必填会被拒绝。"
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
        L"步骤说明写 remark；禁止 customText 与 text/no 字段（工具自动分配序号与标准动作名）。"
        L"非无限循环脚本末尾会自动追加 stopMacro（结束宏运行）。"
        L"传入 actions 数组，每项含 type 及该类型参数；返回可直接嵌入脚本的 JSON 数组文本。"
        L"★ loop/if/else/defineBlock/watchImage 必须用 children 嵌套子动作（像写代码的花括号），"
        L"循环体放在 loop.children 里，不要写成循环后面的同级动作；空循环会构建失败。"
        L"含循环/条件时先 planScriptActions 核对动作树，再调用本工具。"
        L"支持全部编辑器动作（不含 AI 专用动作）。"
        L"AI 相关请用 buildGetCursorPosAction、buildAiTextAnalysisAction、"
        L"buildAiImageAnalysisAction（尽量少用）；buildAiActionExecuteAction 仅当用户明确要求 AI 动作执行时使用。"
        L"找图/OCR 保存变量用 followUp:\"saveVar\"；等待用 type:wait,duration；按键用 keyClick/keyDown/keyUp。"
        L"mouseClick/keyClick/runMacro/runBlock/mousePlayback 等含 clickCount 的动作：duration 是两次重复之间的间隔，"
        L"count=1 时不等待，也不在首前/末后插入等待。"
        L"endLoop 必须放在 loop 的 children 里，否则构建失败。"
        L"必填参数（缺了会构建失败）：keyClick/keyDown/keyUp→keyText；quickInput→inputText；"
        L"wait→duration；findImage/watchImage→imagePath；textRecognition→imagePath 或 ocrSearchText；"
        L"if→conditionExpr；goto→gotoStepExpr；defineBlock/runBlock→blockName；"
        L"varCompute→computeCode；runMacro/mousePlayback→targetPath；openFile/runProgram/openWebpage/closeProgram/"
        L"activateWindow→targetPath；AI 动作→aiPrompt。禁止省略必填参数或用默认值凑数。";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "actions": {
                "type": "array",
                "description": "动作参数对象数组。loop/if/else/defineBlock/watchImage 用 children 嵌套子动作，不要把循环体写成后面的同级项",
                "items": {
                    "type": "object",
                    "properties": {
                        "type": { "type": "string" },
                        "children": {
                            "type": "array",
                            "description": "容器的子动作（仅 loop/if/else/defineBlock/watchImage）"
                        }
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

// ── planScriptActions ─────────────────────────────────────────────
AgentTool MakePlanScriptActionsTool() {
    AgentTool tool;
    tool.name = L"planScriptActions";
    tool.description =
        L"规划脚本动作树（不保存、不校验坐标/路径等必填细节）。"
        L"像写代码一样传入嵌套 actions：loop/if/else/defineBlock/watchImage 用 children 包住循环体/分支。"
        L"返回带缩进的中文动作树，供核对父子关系。空循环（循环后面的动作都是同级）会报错。"
        L"含循环/条件/挂机刷任务时必须先调本工具确认树正确，再补齐参数调用 createMacroScript。"
        L"本步只需 type、remark、children、loopCount/conditionExpr；imagePath 等可留到下一步。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "actions": {
                "type": "array",
                "description": "动作树。容器用 children 嵌套；不必填齐必填参数",
                "items": {
                    "type": "object",
                    "properties": {
                        "type": { "type": "string" },
                        "remark": { "type": "string" },
                        "loopCount": { "type": "integer" },
                        "conditionExpr": { "type": "string" },
                        "children": { "type": "array" }
                    },
                    "required": ["type"]
                }
            }
        },
        "required": ["actions"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try {
            params = json::parse(ToUtf8(paramsJson));
        } catch (const json::parse_error&) {
            return L"[错误] 参数 JSON 解析失败。";
        }
        std::vector<json> items;
        if (params.contains("actions") && params["actions"].is_array()) {
            for (const auto& item : params["actions"]) {
                if (item.is_object()) items.push_back(item);
            }
        } else if (params.contains("type") && params["type"].is_string()) {
            items.push_back(params);
        }
        std::wstring error;
        const std::wstring outline = PlanScriptActionsOutline(items, error);
        if (!error.empty()) return L"[错误] " + error;
        return outline;
    };
    return tool;
}

// ── writeScript ───────────────────────────────────────────────────
AgentTool MakeWriteScriptTool() {
    AgentTool tool;
    tool.name = L"writeScript";
    tool.description = L"将内容写入指定脚本或录制文件（覆盖已有内容）。"
        L"内容会解析并规范化动作（序号 1..n、清除误写的 text 显示名），禁止绕过 buildScriptActions 手写动作对象。"
        L"空循环/空条件（循环体写成同级）会被拒绝；含循环/条件应走 planScriptActions → createMacroScript。"
        L"脚本宏必须包含 windowMode 字段（默认模式 enabled=0）；默认模式可含 breakoutTimeSeconds（0=禁用脱离）；"
        L"键鼠录制始终强制默认模式且 breakoutTimeSeconds=0。";

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
        ss << L"\n" << FormatScriptActionsOutline(data.actions, 80);
        return ss.str();
    };

    return tool;
}

// ── optimizeScript ────────────────────────────────────────────────
AgentTool MakeOptimizeScriptTool() {
    AgentTool tool;
    tool.name = L"optimizeScript";
    tool.description = L"优化脚本或录制：与产品录制优化对话框同一套算法。"
        L"用户说「优化」时 mergeMode 必须为 merge（鼠标移动合并，按关键动作分段，通常能把上百步收成十几步）。"
        L"禁止擅自 compressPath（鼠标移动压缩只去过密点，动作数几乎不降；仅用户明确要求时才用）。"
        L"禁止为优化去 readScript 拉全文或手写 JSON。自动在 scripts 和 recordings 目录下查找";

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
                "description": "默认 merge。用户说优化时必须 merge（鼠标移动合并）。compressPath 仅当用户明确要求压缩路径/去掉过密点"
            },
            "waitCalculation": {
                "type": "string",
                "enum": ["sum", "average", "first", "last", "fixed"],
                "description": "合并/压缩后等待时间（按每段或每个留下的移动间隔独立计算）：sum=累加, average=平均, first=该段第一个, last=该段最后一个, fixed=指定秒数。默认 sum"
            },
            "mergeWaitValue": {
                "type": "number",
                "description": "waitCalculation=fixed 时使用的等待秒数，默认 0.1"
            },
            "distanceThreshold": {
                "type": "number",
                "description": "路径压缩时相邻移动点最小保留距离（像素），默认 5"
            },
            "compressWait": {
                "type": "number",
                "description": "兼容旧参数。waitCalculation=fixed 且未传 mergeWaitValue 时作为指定秒数"
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
        opts.mergeWaitValue = params.value("mergeWaitValue", 0.1);
        opts.distanceThreshold = params.value("distanceThreshold", 5.0);
        opts.compressWait = params.value("compressWait", 0.05);
        if (!params.contains("mergeWaitValue") && params.contains("compressWait"))
            opts.mergeWaitValue = opts.compressWait;
        if (opts.distanceThreshold < 0.1) opts.distanceThreshold = 0.1;
        if (opts.compressWait < 0.0) opts.compressWait = 0.0;
        if (opts.mergeWaitValue < 0.0) opts.mergeWaitValue = 0.0;

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
        L"禁止 customText；说明写 remark；末尾自动追加 stopMacro（除非顶层无限 loop）。"
        L"★ loop/if/else/defineBlock/watchImage 必须用 children 嵌套子动作（像写代码）；"
        L"循环体放在 loop.children，不要写成循环后面的同级。含循环/条件时先 planScriptActions。"
        L"含 AI 动作时无需手写 aiModelName，保存时会自动从已添加模型中选取（图片分析优先识图模型）。"
        L"默认效率优先：优先 findImage/OCR，少调用 AI 分析；aiActionExecute 仅用户明确要求时使用。"
        L"准确度优先时 readAgentSkill section=scriptStrategy。"
        L"需窗口/后台模式时传 scriptMode 或 windowMode（见 readScriptReference section=windowMode）。"
        L"默认模式可传 breakoutTimeSeconds（秒，0=禁用；用户中途操作键鼠会暂停宏，松开后空闲该秒数再恢复）。"
        L"默认保存到 scripts 根目录（不分类）；用户指明分类/目录时传 folder 保存到 scripts/<folder>。"
        L"必填参数：keyClick/keyDown/keyUp→keyText；quickInput→inputText；wait→duration；"
        L"findImage/watchImage→imagePath；if→conditionExpr；goto→gotoStepExpr；runMacro→targetPath；"
        L"varCompute→computeCode；openFile/runProgram/openWebpage→targetPath；AI 动作→aiPrompt。缺必填会构建失败。";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "fileName": { "type": "string", "description": "文件名，如 mymacro.json" },
            "folder": {
                "type": "string",
                "description": "目标子目录（相对 scripts，默认空=根目录不分类；用户指明分类/目录时传，如“工作”）"
            },
            "scriptName": { "type": "string", "description": "宏显示名称" },
            "scriptMode": {
                "type": "string",
                "enum": ["default", "window", "backgroundWindow"],
                "description": "脚本运行模式：default=默认, window=窗口模式, backgroundWindow=后台窗口模式"
            },
            "breakoutTimeSeconds": {
                "type": "number",
                "description": "脱离时间（秒，仅默认模式生效）。0 或未填=禁用；用户中途操作键鼠会暂停宏，按住期间不计时，松开后空闲该秒数再从当前步骤重试"
            },
            "windowMode": {
                "type": "object",
                "description": "完整 windowMode 配置（与脚本 JSON 头字段一致，优先级高于 scriptMode）"
            },
            "actions": {
                "type": "array",
                "description": "动作参数数组。loop/if/else/defineBlock/watchImage 用 children 嵌套子动作（同 buildScriptActions）",
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
        const auto result = AgentCreateMacroScript(fileName, scriptName, items, params);
        return result.message;
    };

    return tool;
}

// ── optimizeRecording ─────────────────────────────────────────────
AgentTool MakeOptimizeRecordingTool() {
    AgentTool tool;
    tool.name = L"optimizeRecording";
    tool.description =
        L"优化键鼠录制：与产品「鼠标移动合并」同一算法。用户说「优化」时 mergeMode 必须为 merge"
        L"（按关键动作分段合并，通常能把上百步收成十几步）。"
        L"禁止擅自 compressPath（只去过密点，动作数几乎不降）。"
        L"默认 recordings 目录；禁止 readScript 拉全文手改。完成后自动刷新主界面。";

    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "fileName": { "type": "string", "description": "录制文件名" },
            "outputFileName": { "type": "string", "description": "另存为文件名，省略则覆盖" },
            "mergeMode": {
                "type": "string",
                "enum": ["merge", "compressPath"],
                "description": "默认 merge。用户说优化时必须 merge（鼠标移动合并）。compressPath 仅当用户明确要求压缩路径/去掉过密点"
            },
            "waitCalculation": {
                "type": "string",
                "enum": ["sum", "average", "first", "last", "fixed"],
                "description": "合并/压缩后等待时间（按每段或每个留下的移动间隔独立计算）：sum=累加, average=平均, first=该段第一个, last=该段最后一个, fixed=指定秒数。默认 sum"
            },
            "mergeWaitValue": {
                "type": "number",
                "description": "waitCalculation=fixed 时使用的等待秒数，默认 0.1"
            },
            "distanceThreshold": {
                "type": "number",
                "description": "路径压缩时相邻移动点最小保留距离（像素），默认 5"
            },
            "compressWait": {
                "type": "number",
                "description": "兼容旧参数。waitCalculation=fixed 且未传 mergeWaitValue 时作为指定秒数"
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
        opts.dir = L"recordings";
        opts.outputFileName = FromUtf8(params.value("outputFileName", ""));
        opts.mergeMode = FromUtf8(params.value("mergeMode", "merge"));
        opts.waitCalculation = FromUtf8(params.value("waitCalculation", "sum"));
        opts.mergeWaitValue = params.value("mergeWaitValue", 0.1);
        opts.distanceThreshold = params.value("distanceThreshold", 5.0);
        opts.compressWait = params.value("compressWait", 0.05);
        if (!params.contains("mergeWaitValue") && params.contains("compressWait"))
            opts.mergeWaitValue = opts.compressWait;
        if (opts.distanceThreshold < 0.1) opts.distanceThreshold = 0.1;
        if (opts.compressWait < 0.0) opts.compressWait = 0.0;
        if (opts.mergeWaitValue < 0.0) opts.mergeWaitValue = 0.0;

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
            case ScheduledFrequency::Interval: freqLabel = L"间隔"; break;
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
                "description": "目标文件名（如 myscript.json）。必须存在；请先用 listScripts 确认。"
            },
            "kind": {
                "type": "string",
                "enum": ["macro", "recording"],
                "description": "任务类型：macro=鼠标宏, recording=键鼠录制。默认 macro"
            },
            "frequency": {
                "type": "string",
                "enum": ["custom", "daily", "weekly", "hourly", "interval"],
                "description": "执行频率：custom=单次, daily=每天, weekly=每周, hourly=每小时, interval=从保存/软件启动起计时、到达间隔后执行、关闭软件后重置。默认 custom"
            },
            "year": { "type": "integer", "description": "年份（custom 必填，如 2026）" },
            "month": { "type": "integer", "description": "月份（custom 必填，1-12）" },
            "day": { "type": "integer", "description": "日（custom 必填，1-31）" },
            "hour": { "type": "integer", "description": "小时。daily/weekly/custom 为钟点 0-23；interval 为间隔时长的小时部分。hourly 忽略。默认 9（interval 默认 0）" },
            "minute": { "type": "integer", "description": "分钟（0-59）。interval 为间隔时长的分钟。默认 0" },
            "second": { "type": "integer", "description": "秒（0-59）。interval 为间隔时长的秒。默认 0" },
            "weekDays": {
                "type": "array", "items": { "type": "string" },
                "description": "星期数组（weekly 必填）：[\"Mon\",\"Tue\",\"Wed\",\"Thu\",\"Fri\",\"Sat\",\"Sun\"]"
            },
            "enabled": {
                "type": "boolean",
                "description": "是否启用，默认 true"
            }
        },
        "required": ["name", "targetFile"]
    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try { params = json::parse(ToUtf8(paramsJson)); }
        catch (const json::parse_error&) { return L"[错误] 参数 JSON 解析失败。"; }

        std::wstring name = FromUtf8(params.value("name", ""));
        if (name.empty()) return L"[错误] 缺少 name 参数。";

        std::wstring targetFile = FromUtf8(params.value("targetFile", ""));
        if (targetFile.empty()) return L"[错误] 缺少 targetFile 参数。";
        std::wstring kindStr = FromUtf8(params.value("kind", "macro"));
        std::wstring freqStr = FromUtf8(params.value("frequency", "custom"));

        auto found = FindScriptFile(targetFile, L"");
        if (!found.found)
            return L"[错误] 目标文件不存在：" + targetFile + L"。请先用 listScripts 确认文件名。";

        ScheduledTask task;
        task.id = GenerateScheduledTaskId();
        task.name = name;
        task.kind = (kindStr == L"recording") ? ScheduledTaskKind::Recording : ScheduledTaskKind::Macro;
        task.filePath = found.path;
        task.fileDisplayName = targetFile;

        if (freqStr == L"hourly") task.frequency = ScheduledFrequency::Hourly;
        else if (freqStr == L"daily") task.frequency = ScheduledFrequency::Daily;
        else if (freqStr == L"weekly") task.frequency = ScheduledFrequency::Weekly;
        else if (freqStr == L"interval") task.frequency = ScheduledFrequency::Interval;
        else task.frequency = ScheduledFrequency::Custom;

        task.time.year = params.value("year", 0);
        task.time.month = params.value("month", 0);
        task.time.day = params.value("day", 0);
        task.time.hour = params.value("hour",
            task.frequency == ScheduledFrequency::Interval ? 0 : 9);
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

        if (task.frequency == ScheduledFrequency::Weekly && task.time.weekDays == 0) {
            return L"[错误] weekly 频率必须提供 weekDays（至少一个星期）。";
        }
        if (task.frequency == ScheduledFrequency::Interval
            && ScheduledIntervalDurationMs(task.time) <= 0) {
            return L"[错误] interval 频率必须提供大于 0 的 hour/minute/second 间隔。";
        }
        if (task.frequency == ScheduledFrequency::Custom) {
            if (task.time.year < 1970 || task.time.month < 1 || task.time.month > 12
                || task.time.day < 1 || task.time.day > 31) {
                return L"[错误] custom 频率必须提供有效的 year/month/day。";
            }
        }

        task.status = params.value("enabled", true)
            ? ScheduledTaskStatus::Enabled : ScheduledTaskStatus::Disabled;

        std::vector<ScheduledTask> tasks;
        bool globalDisabled = false;
        LoadScheduledTasks(tasks, &globalDisabled);
        tasks.push_back(task);

        AgentFileUndo undo(L"createScheduledTask", L"创建定时任务", ScheduledTasksFilePath());
        if (!SaveScheduledTasks(tasks, globalDisabled))
            return L"[错误] 保存定时任务失败。";
        undo.Success();
        NotifyAgentScheduledTasksChanged(
            task.frequency == ScheduledFrequency::Interval ? task.id : std::wstring());

        std::wstringstream ss;
        ss << L"定时任务已创建：\n";
        ss << L"  名称: " << task.name << L"\n";
        ss << L"  类型: " << ((task.kind == ScheduledTaskKind::Macro) ? L"鼠标宏" : L"键鼠录制") << L"\n";
        if (!task.filePath.empty()) ss << L"  目标: " << task.fileDisplayName << L"\n";
        ss << L"  执行时间: " << FormatScheduledRunTime(task) << L"\n";
        ss << L"  状态: " << ((task.status == ScheduledTaskStatus::Enabled) ? L"启用" : L"禁用") << L"\n\n";
        ss << L"提示：任务已写入并通知主窗口重新加载，将在到期秒触发。";
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
            "frequency": { "type": "string", "enum": ["custom", "daily", "weekly", "hourly", "interval"], "description": "新执行频率" },
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
            else if (f == L"interval") target->frequency = ScheduledFrequency::Interval;
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
            if (newFile.empty()) return L"[错误] targetFile 不能为空。";
            auto found = FindScriptFile(newFile, L"");
            if (!found.found) return L"[错误] 目标文件不存在：" + newFile;
            target->filePath = found.path;
            target->fileDisplayName = newFile;
        }

        if (target->filePath.empty())
            return L"[错误] 任务缺少有效目标文件；请传入 targetFile。";
        if (target->frequency == ScheduledFrequency::Weekly && target->time.weekDays == 0)
            return L"[错误] weekly 频率必须至少选择一个星期（weekDays）。";
        if (target->frequency == ScheduledFrequency::Interval
            && ScheduledIntervalDurationMs(target->time) <= 0)
            return L"[错误] interval 频率必须提供大于 0 的 hour/minute/second 间隔。";
        if (target->frequency == ScheduledFrequency::Custom) {
            if (target->time.year < 1970 || target->time.month < 1 || target->time.month > 12
                || target->time.day < 1 || target->time.day > 31) {
                return L"[错误] custom 频率需要有效的 year/month/day。";
            }
            // 与 Web 保存路径一致：Custom 任务更新后允许再次触发
            target->customFired = false;
        }

        AgentFileUndo undo(L"updateScheduledTask", L"更新定时任务", ScheduledTasksFilePath());
        if (!SaveScheduledTasks(tasks, globalDisabled))
            return L"[错误] 保存定时任务失败。";
        undo.Success();
        NotifyAgentScheduledTasksChanged(
            target->frequency == ScheduledFrequency::Interval ? target->id : std::wstring());

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

        AgentFileUndo undo(L"deleteScheduledTask", L"删除定时任务", ScheduledTasksFilePath());
        if (!SaveScheduledTasks(tasks, globalDisabled))
            return L"[错误] 保存定时任务失败。";
        undo.Success();
        NotifyAgentScheduledTasksChanged();

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
        ss << L"  回放倍速: " << (settings.playback.enablePlaybackSpeed ? L"启用" : L"禁用")
           << L"（" << std::to_wstring(settings.playback.playbackSpeed)
           << L" 倍；仅录制页直接播放；"
           << (quickscript::HomeUiModeIsPro(settings.home)
               ? L"专业模式看勾选" : L"极简模式始终启用")
           << L"；嵌套运行录制回放用动作自身倍速）\n";
        ss << L"  调试输出窗口: " << (settings.playback.enableDebugOutputWindow ? L"启用" : L"禁用") << L"\n";
        ss << L"  关键函数调试: " << (settings.playback.autoOutputKeyFunctionDebug ? L"启用" : L"禁用") << L"\n";
        ss << L"  前台注入后端: "
           << quickscript::ForegroundInputBackendName(settings.playback.foregroundInputBackend) << L"\n";
        ss << L"  定时任务优先级: "
           << ScheduledTaskConflictPolicyLabel(ClampScheduledTaskConflictPolicy(
                  settings.playback.scheduledTaskConflictPolicy))
           << L"（0=执行脚本优先 1=定时脚本优先）\n";
        ss << L"  脚本中断后自动恢复: "
           << (settings.playback.scheduledTaskAutoResume ? L"启用" : L"禁用") << L"\n";
        ss << L"  低性能模式: "
           << (settings.playback.lowPerformanceMode
                   ? L"启用（找图限单线程、回放不提优先级/不抬系统定时器分辨率、减少自旋）"
                   : L"禁用（性能/精度优先）")
           << L"\n";
        ss << L"  找图 GPU 加速: "
           << (settings.playback.findImageGpuAccel
                   ? (settings.playback.lowPerformanceMode
                           ? L"已勾选但被低性能模式压制（低性能模式优先）"
                           : L"启用（≥500k 像素的找图走 OpenCL）")
                   : L"禁用")
           << L"\n\n";

        ss << L"【其他设置】\n";
        ss << L"  宏执行后自动隐藏主窗口: " << (settings.other.autoHideMainWindow ? L"是" : L"否") << L"\n";
        ss << L"  脚本启动时播放提示音: " << (settings.other.playSoundOnStart ? L"是" : L"否") << L"\n";
        ss << L"  脚本结束时播放提示音: " << (settings.other.playSoundOnEnd ? L"是" : L"否") << L"\n";
        ss << L"  隐藏右下角弹窗提示: " << (settings.other.hideBottomRightTip ? L"是" : L"否") << L"\n";
        ss << L"  关闭按钮最小化到托盘: " << (settings.other.closeToTray ? L"是" : L"否") << L"\n";
        ss << L"  显示桌面悬浮球: " << (settings.other.showFloatBall ? L"是" : L"否") << L"\n";
        ss << L"  开机自动启动: " << (settings.other.autoStartOnBoot ? L"是" : L"否") << L"\n";
        ss << L"  中文输入法不触发热键: " << (settings.other.resolveImeConflict ? L"是" : L"否") << L"\n";
        ss << L"  鼠标宏编辑界面默认视图: "
           << (quickscript::NormalizeEditorDefaultView(settings.other.editorDefaultView) == L"visual"
               ? L"可视化" : L"代码化") << L"\n";
        ss << L"  循环体标识: " << (settings.other.visualLoopWrap ? L"显示" : L"隐藏") << L"\n";
        ss << L"  指令块调用线: " << (settings.other.visualBlockCallWires ? L"显示" : L"隐藏") << L"\n";
        ss << L"  条件块标识: " << (settings.other.visualIfWrap ? L"显示" : L"隐藏") << L"\n";
        ss << L"  指令块包裹: " << (settings.other.visualBlockWrap ? L"显示" : L"隐藏") << L"\n";
        ss << L"  找图监视包裹: " << (settings.other.visualWatchWrap ? L"显示" : L"隐藏") << L"\n";
        ss << L"  跳转连线: " << (settings.other.visualJumpWires ? L"显示" : L"隐藏") << L"\n";
        ss << L"  可视化网格: " << (settings.other.visualShowGrid ? L"显示" : L"隐藏") << L"\n";
        ss << L"  卡片编号: " << (settings.other.visualShowCardId ? L"显示" : L"隐藏") << L"\n";
        ss << L"  编辑器动作顺序: "
           << (settings.other.editorActionOrder.empty() ? L"产品默认"
               : (std::to_wstring(settings.other.editorActionOrder.size()) + L" 项自定义"))
           << L"\n";
        ss << L"  编辑器隐藏动作: "
           << (settings.other.editorHiddenActions.empty()
               ? L"无"
               : (std::to_wstring(settings.other.editorHiddenActions.size()) + L" 种（仅添加列表，不影响已有步骤）"))
           << L"\n";
        {
            const std::wstring preset = quickscript::NormalizeEditorCatalogPreset(
                settings.other.editorCatalogPreset);
            ss << L"  编辑器目录预设: "
               << (preset == L"simple" ? L"精简"
                   : preset == L"office" ? L"办公"
                   : preset == L"game" ? L"游戏图色"
                   : preset == L"custom" ? L"自定义"
                   : L"全部")
               << L"\n";
            ss << L"  自定义目录槽: "
               << (settings.other.editorCustomActionOrder.empty()
                       && settings.other.editorCustomHiddenActions.empty()
                   ? L"空"
                   : (std::to_wstring(settings.other.editorCustomActionOrder.size()) + L" 项顺序 / "
                       + std::to_wstring(settings.other.editorCustomHiddenActions.size()) + L" 种隐藏"))
               << L"\n";
            ss << L"  搜索动作从所有动作里搜索: "
               << (settings.other.editorSearchAllActions ? L"是" : L"否") << L"\n";
            ss << L"  隐藏固定变量: "
               << (settings.other.editorHideFixedVars ? L"是" : L"否") << L"\n";
            ss << L"  隐藏坐标变量: "
               << (settings.other.editorHideCoordVars ? L"是" : L"否") << L"\n";
            ss << L"  多结果仅显示代指: "
               << (settings.other.editorMultiResultPlaceholderOnly ? L"是" : L"否") << L"\n";
            ss << L"  不启用修改按钮: "
               << (settings.other.editorDisableModifyButton ? L"是" : L"否") << L"\n";
            ss << L"  退出时自动保存: "
               << (settings.other.editorAutoSaveOnExit ? L"是" : L"否") << L"\n";
            ss << L"  启用批量插入: "
               << (settings.other.editorEnableBatchInsert ? L"是" : L"否") << L"\n";
        }
        ss << L"  长按判定(秒): " << FormatHoldThresholdLabel(settings.other.holdThresholdSeconds) << L"\n";
        ss << L"  界面缩放倍率: " << settings.other.uiScaleFactor
           << L"（叠在分辨率自适应之后，默认 1.0）\n";

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
            "enablePlaybackSpeed": { "type": "boolean", "description": "专业模式：录制页直接播放是否启用倍速；极简模式无视此勾选（视为已启用）。鼠标宏顶层不缩放" },
            "playbackSpeed": { "type": "number", "description": "录制页直接播放倍速 0.25~4，1=原速；与极简工具栏共用。嵌套「运行录制回放」用动作 playbackSpeed，不叠加" },
            "enableDebugOutputWindow": { "type": "boolean", "description": "启用调试输出窗口" },
            "autoOutputKeyFunctionDebug": { "type": "boolean", "description": "自动输出关键函数调试信息" },
            "enableHidDriverSimulation": { "type": "boolean", "description": "兼容旧字段：true≈Interception，false≈Software；优先用 foregroundInputBackend" },
            "foregroundInputBackend": { "type": "integer", "description": "前台注入后端：0=Software 1=Interception 2=VirtualHid" },
            "scheduledTaskConflictPolicy": { "type": "integer", "description": "定时任务优先级：0=执行脚本优先 1=定时脚本优先。未勾选自动恢复时 0=忙则跳过、1=打断不恢复；勾选时 0=结束后再跑、1=插入后从原步骤继续" },
            "scheduledTaskAutoResume": { "type": "boolean", "description": "脚本中断后自动恢复。false=跳过或打断不恢复；true=结束后再跑或插入后从原步骤继续" },
            "lowPerformanceMode": { "type": "boolean", "description": "低性能模式（省 CPU/降温）：找图限 1 个 OpenCV 线程、输入时间轴大幅减少自旋、回放不再提优先级/不抬全系统定时器分辨率、找图监视轮询下限 50→200ms。代价是注入节奏可有 ~1ms 抖动、单帧找图变慢。用户抱怨「跑脚本时电脑很烫/风扇很响」时建议开启" },
            "findImageGpuAccel": { "type": "boolean", "description": "找图 GPU 加速（OpenCL）：大区域全屏找图走显卡（实测约快 3 倍），面积小于 500k 像素的区域找图自动仍用 CPU；机器无 OpenCL 设备时自动忽略。与 lowPerformanceMode 同时开启时本项不生效（低性能模式优先）" },
            "autoHideMainWindow": { "type": "boolean", "description": "宏执行后自动隐藏主窗口" },
            "playSoundOnStart": { "type": "boolean", "description": "脚本启动时播放提示音" },
            "playSoundOnEnd": { "type": "boolean", "description": "脚本结束时播放提示音" },
            "hideBottomRightTip": { "type": "boolean", "description": "隐藏右下角弹窗提示" },
            "closeToTray": { "type": "boolean", "description": "关闭按钮最小化到托盘" },
            "showFloatBall": { "type": "boolean", "description": "显示桌面悬浮球（可贴边半露，也可拖到屏幕中间自由悬浮；悬停展开启停）" },
            "autoStartOnBoot": { "type": "boolean", "description": "开机自动启动" },
            "resolveImeConflict": { "type": "boolean", "description": "中文输入法处于中文模式时不触发热键；Shift 英文或关闭输入法后仍可触发" },
            "editorDefaultView": { "type": "string", "enum": ["code", "visual"], "description": "鼠标宏编辑界面默认视图：code=代码化 visual=可视化" },
            "visualLoopWrap": { "type": "boolean", "description": "可视化循环体大包裹框" },
            "visualBlockCallWires": { "type": "boolean", "description": "可视化定义宏与运行宏之间的虚线调用线" },
            "visualIfWrap": { "type": "boolean", "description": "可视化 if/else 大包裹框" },
            "visualBlockWrap": { "type": "boolean", "description": "可视化定义宏指令块大包裹框" },
            "visualWatchWrap": { "type": "boolean", "description": "可视化找图监视大包裹框" },
            "visualJumpWires": { "type": "boolean", "description": "可视化跳转/goto 连线" },
            "visualShowGrid": { "type": "boolean", "description": "可视化画布点阵网格" },
            "visualShowCardId": { "type": "boolean", "description": "可视化卡片标题中的 ID" },
            "editorActionOrder": { "type": "array", "items": { "type": "string" }, "description": "编辑器「请选择要添加的宏」排列顺序（动作 type）；空=产品默认" },
            "editorHiddenActions": { "type": "array", "items": { "type": "string" }, "description": "从添加列表隐藏的动作 type；不影响脚本里已有步骤" },
            "editorCatalogPreset": { "type": "string", "enum": ["simple", "office", "game", "all", "custom"], "description": "编辑器动作目录预设：simple=精简 office=办公 game=游戏图色 all=全部 custom=手改" },
            "editorCustomActionOrder": { "type": "array", "items": { "type": "string" }, "description": "动作目录「自定义」槽的排列顺序；切换其它预设时保留" },
            "editorCustomHiddenActions": { "type": "array", "items": { "type": "string" }, "description": "动作目录「自定义」槽的隐藏列表；仅在其它预设上改勾选/顺序时覆盖" },
            "editorSearchAllActions": { "type": "boolean", "description": "添加列表搜索是否包含已隐藏动作；false=只搜当前目录显示项" },
            "editorHideFixedVars": { "type": "boolean", "description": "插入变量列表隐藏 ctrl:CurLoops 等固定变量" },
            "editorHideCoordVars": { "type": "boolean", "description": "插入变量列表隐藏 .x/.y/.cx 等坐标字段" },
            "editorMultiResultPlaceholderOnly": { "type": "boolean", "description": "多结果只显示 matchRet[n] 代指，不列出 [0]/[1]/[2]" },
            "editorDisableModifyButton": { "type": "boolean", "description": "不启用修改按钮：隐藏「修改」，选中后改参数失焦即写回列表" },
            "editorAutoSaveOnExit": { "type": "boolean", "description": "退出时自动保存：×/关软件写盘，「取消」恢复到打开时" },
            "editorEnableBatchInsert": { "type": "boolean", "description": "启用批量插入：多选时非容器类型点添加逐条插入，容器类型仍并入" },
            "holdThresholdSeconds": { "type": "number", "description": "长按判定秒数（>0，热键按住启停与捕获共用）" },
            "uiScaleFactor": { "type": "number", "description": "界面缩放倍率（正数，可小数，默认 1.0；叠在分辨率自适应之后，0.25~3）" }
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
            setBool("enablePlaybackSpeed", settings.playback.enablePlaybackSpeed);
            if (params.contains("playbackSpeed")) {
                settings.playback.playbackSpeed = quickscript::ClampPlaybackSpeed(
                    params["playbackSpeed"].get<double>());
            }
            setBool("enableDebugOutputWindow", settings.playback.enableDebugOutputWindow);
            setBool("autoOutputKeyFunctionDebug", settings.playback.autoOutputKeyFunctionDebug);
            if (params.contains("foregroundInputBackend")) {
                settings.playback.foregroundInputBackend = quickscript::ClampForegroundInputBackend(
                    params["foregroundInputBackend"].get<int>());
                settings.playback.enableHidDriverSimulation =
                    settings.playback.foregroundInputBackend
                    != quickscript::ForegroundInputBackend::Software;
            } else if (params.contains("enableHidDriverSimulation")) {
                const bool on = params["enableHidDriverSimulation"].get<bool>();
                settings.playback.enableHidDriverSimulation = on;
                settings.playback.foregroundInputBackend = on
                    ? quickscript::ForegroundInputBackend::Interception
                    : quickscript::ForegroundInputBackend::Software;
            }
            if (params.contains("scheduledTaskConflictPolicy")) {
                settings.playback.scheduledTaskConflictPolicy = static_cast<int>(
                    ClampScheduledTaskConflictPolicy(
                        params["scheduledTaskConflictPolicy"].get<int>()));
            }
            setBool("scheduledTaskAutoResume", settings.playback.scheduledTaskAutoResume);
            setBool("lowPerformanceMode", settings.playback.lowPerformanceMode);
            setBool("findImageGpuAccel", settings.playback.findImageGpuAccel);
        } else if (category == L"other") {
            setBool("autoHideMainWindow", settings.other.autoHideMainWindow);
            setBool("playSoundOnStart", settings.other.playSoundOnStart);
            setBool("playSoundOnEnd", settings.other.playSoundOnEnd);
            setBool("hideBottomRightTip", settings.other.hideBottomRightTip);
            setBool("closeToTray", settings.other.closeToTray);
            setBool("showFloatBall", settings.other.showFloatBall);
            setBool("autoStartOnBoot", settings.other.autoStartOnBoot);
            setBool("resolveImeConflict", settings.other.resolveImeConflict);
            if (params.contains("editorDefaultView") && params["editorDefaultView"].is_string()) {
                settings.other.editorDefaultView = quickscript::NormalizeEditorDefaultView(
                    FromUtf8(params["editorDefaultView"].get<std::string>()));
            }
            setBool("visualLoopWrap", settings.other.visualLoopWrap);
            setBool("visualBlockCallWires", settings.other.visualBlockCallWires);
            setBool("visualIfWrap", settings.other.visualIfWrap);
            setBool("visualBlockWrap", settings.other.visualBlockWrap);
            setBool("visualWatchWrap", settings.other.visualWatchWrap);
            setBool("visualJumpWires", settings.other.visualJumpWires);
            setBool("visualShowGrid", settings.other.visualShowGrid);
            setBool("visualShowCardId", settings.other.visualShowCardId);
            auto setTokenArray = [&](const char* key, std::vector<std::wstring>& target) {
                if (!params.contains(key) || !params[key].is_array()) return;
                target.clear();
                for (const auto& el : params[key]) {
                    if (!el.is_string()) continue;
                    const std::wstring w = FromUtf8(el.get<std::string>());
                    if (w.empty() || w.size() > 64) continue;
                    bool tokenOk = true;
                    for (const wchar_t c : w) {
                        const bool chOk = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
                            || (c >= L'0' && c <= L'9') || c == L'_';
                        if (!chOk) {
                            tokenOk = false;
                            break;
                        }
                    }
                    if (!tokenOk) continue;
                    if (target.size() < 80
                        && std::find(target.begin(), target.end(), w) == target.end()) {
                        target.push_back(w);
                    }
                }
            };
            setTokenArray("editorActionOrder", settings.other.editorActionOrder);
            setTokenArray("editorHiddenActions", settings.other.editorHiddenActions);
            setTokenArray("editorCustomActionOrder", settings.other.editorCustomActionOrder);
            setTokenArray("editorCustomHiddenActions", settings.other.editorCustomHiddenActions);
            if (params.contains("editorCatalogPreset") && params["editorCatalogPreset"].is_string()) {
                settings.other.editorCatalogPreset = quickscript::NormalizeEditorCatalogPreset(
                    FromUtf8(params["editorCatalogPreset"].get<std::string>()));
            }
            setBool("editorSearchAllActions", settings.other.editorSearchAllActions);
            setBool("editorHideFixedVars", settings.other.editorHideFixedVars);
            setBool("editorHideCoordVars", settings.other.editorHideCoordVars);
            setBool("editorMultiResultPlaceholderOnly", settings.other.editorMultiResultPlaceholderOnly);
            setBool("editorDisableModifyButton", settings.other.editorDisableModifyButton);
            setBool("editorAutoSaveOnExit", settings.other.editorAutoSaveOnExit);
            setBool("editorEnableBatchInsert", settings.other.editorEnableBatchInsert);
            if (params.contains("holdThresholdSeconds")) {
                settings.other.holdThresholdSeconds = NormalizeHoldThresholdSeconds(
                    params["holdThresholdSeconds"].get<double>());
            }
            if (params.contains("uiScaleFactor")) {
                settings.other.uiScaleFactor = quickscript::NormalizeUiScaleFactor(
                    params["uiScaleFactor"].get<double>());
            }
        } else {
            return L"[错误] 无效的 category：" + category + L"。可选值：click, playback, other";
        }

        AgentFileUndo undo(L"updateSettings", L"修改应用设置", AppSettingsFilePath());
        if (!SaveAppSettings(settings))
            return L"[错误] 保存设置失败。";
        undo.Success();
        UiScaleSetUserFactor(settings.other.uiScaleFactor);

        return L"设置已更新（" + category + L" 分类）。\n"
               L"回放类设置会在当前宏的下一轮循环自动生效；界面缩放倍率请在设置中点保存以立刻应用到窗口，或重启软件。";
    };

    return tool;
}

// ── listAiModels ──────────────────────────────────────────────────
AgentTool MakeListAiModelsTool() {
    AgentTool tool;
    tool.name = L"listAiModels";
    tool.description =
        L"列出用户在「设置→AI助手」中已添加的 AI 模型，并标注是否支持识图。"
        L"添加 AI 动作前可先调用；常规脚本优先 findImage/OCR，不必先查模型。";
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
            "matchVarName": { "type": "string", "description": "变量名，默认 a" },
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
        L"构建「AI 文本分析」动作。★优先级低★：优先 OCR(textRecognition)；"
        L"仅当必须理解文字语义且 OCR 不够用时才调用。aiPrompt 必填；aiModelName 可省略。";
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
        L"构建「AI 图片分析」动作。★优先级低★：优先 findImage；"
        L"仅当必须理解画面且找图无法完成，或准确度模式兜底诊断界面状况时使用。"
        L"aiPrompt 必填；自动选择识图模型。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "aiPrompt": { "type": "string", "description": "对截图的分析问题（必填）" },
            "aiModelName": { "type": "string", "description": "可选；省略时自动选择识图模型" },
            "aiOutputVarName": { "type": "string", "description": "默认 aiImgResult" },
            "aiOutputType": { "type": "integer", "description": "0=文本 1=数字" },
            "aiImageScale": { "type": "number", "description": "截屏缩放 0.1~1" },
            "aiRegionByImage": { "type": "boolean", "description": "在绝对识别区域内找图并用匹配框截屏" },
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
        L"构建「AI 动作执行」动作。★极低优先级★：仅当用户明确要求「AI动作执行/让AI自动操作桌面」时使用；"
        L"禁止用其替代 findImage+键鼠 常规动作链。aiPrompt 为任务描述；可设置最大步数、超时等。"
        L"aiLogicConvert（逻辑转化）仅当用户明确要求「逻辑转化/自愈脚本」时才可置 true。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "aiPrompt": { "type": "string", "description": "任务描述（必填）" },
            "aiModelName": { "type": "string", "description": "可选；省略时自动选择" },
            "aiWithImage": { "type": "boolean", "description": "是否带截图，默认 true" },
            "aiLogicConvert": { "type": "boolean", "description": "逻辑转化；默认 false；须用户明确要求" },
            "aiLogicBlockName": { "type": "string", "description": "关联指令块名，可空" },
            "aiRegionByImage": { "type": "boolean" },
            "aiTargetImagePath": { "type": "string" },
            "aiSearchX1": { "type": "integer" },
            "aiSearchY1": { "type": "integer" },
            "aiSearchX2": { "type": "integer" },
            "aiSearchY2": { "type": "integer" },
            "aiMaxSteps": { "type": "integer", "description": "默认 10，-1 不限" },
            "aiTimeoutSec": { "type": "integer" },
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
        const std::wstring userCtx = GetAgentToolUserContext();
        // 「逻辑转化」可单独解锁 aiActionExecute（否则只能生成宏动作链）
        if (!UserExplicitlyRequestsAiActionExecute(userCtx)
            && !UserExplicitlyRequestsLogicConvert(userCtx)) {
            return L"[错误] 用户未明确要求「AI动作执行」或「逻辑转化」，禁止生成 aiActionExecute。"
                   L"请改用 findImage + 键鼠动作链。";
        }
        const bool wantLogic = [&]() {
            if (!params.contains("aiLogicConvert")) return false;
            if (params["aiLogicConvert"].is_boolean()) return params["aiLogicConvert"].get<bool>();
            if (params["aiLogicConvert"].is_number_integer())
                return params["aiLogicConvert"].get<int>() != 0;
            return false;
        }();
        if (wantLogic && !UserExplicitlyRequestsLogicConvert(userCtx)) {
            return L"[错误] 用户未明确要求「逻辑转化/自愈脚本」，禁止 aiLogicConvert=true。";
        }
        if (!wantLogic) {
            params["aiLogicConvert"] = false;
            params.erase("aiLogicBlockName");
        }
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

// ─────────────────────────────────────────────────────────────
// listAgentChanges / revertAgentChange — 变更撤销
// ─────────────────────────────────────────────────────────────

AgentTool MakeListAgentChangesTool() {
    AgentTool tool;
    tool.name = L"listAgentChanges";
    tool.description =
        L"列出助手最近修改过的文件（写脚本/建宏/优化/删除/定时任务/设置等），"
        L"含时间、工具、目标路径与状态（已应用/已恢复）。配合 revertAgentChange 使用。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "maxEntries": { "type": "integer", "description": "最多条数，默认 50" }
        },
        "required": []
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        size_t maxEntries = 50;
        try {
            const json p = json::parse(ToUtf8(paramsJson));
            if (p.contains("maxEntries") && p["maxEntries"].is_number_integer())
                maxEntries = static_cast<size_t>(p["maxEntries"].get<int>());
        } catch (...) {}
        return ListAgentChangesText(maxEntries);
    };
    return tool;
}

AgentTool MakeRevertAgentChangeTool() {
    AgentTool tool;
    tool.name = L"revertAgentChange";
    tool.description =
        L"恢复指定助手修改：把目标文件恢复为该次修改前的内容；若修改前文件不存在则删除。"
        L"id 来自 listAgentChanges；已恢复的记录不能再次恢复。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "id": { "type": "string", "description": "变更记录 id" }
        },
        "required": ["id"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        json params;
        try { params = json::parse(ToUtf8(paramsJson)); }
        catch (const json::parse_error&) { return L"[错误] 参数 JSON 解析失败。"; }
        const std::wstring id = FromUtf8(params.value("id", ""));
        if (id.empty()) return L"[错误] 缺少 id 参数。";
        std::wstring err;
        if (!RevertAgentChange(id, err)) return L"[错误] " + err;
        return L"已恢复变更 " + id + L"。目标文件已回到修改前状态。";
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
    tools.push_back(MakeFetchWebPageTool());
    tools.push_back(MakeRunAgentCommandTool());
    tools.push_back(MakeListAgentDirectoryTool());
    tools.push_back(MakeReadAgentFileTool());
    tools.push_back(MakeSearchAgentFilesTool());
    tools.push_back(MakeWriteAgentFileTool());
    tools.push_back(MakeCopyAgentTextToClipboardTool());
    tools.push_back(MakePasteAgentClipboardTextTool());
    tools.push_back(MakeListAgentChangesTool());
    tools.push_back(MakeRevertAgentChangeTool());
    tools.push_back(MakePlanScriptActionsTool());
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
