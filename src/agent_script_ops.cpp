#include "agent_script_ops.h"

#include "action_tree.h"
#include "agent_ui_notify.h"
#include "agent_ai_actions.h"
#include "script_action_builder.h"
#include "script_io.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace {

using json = nlohmann::json;

bool IsSafeFileName(const std::wstring& fileName) {
    if (fileName.find(L'\\') != std::wstring::npos) return false;
    if (fileName.find(L'/') != std::wstring::npos) return false;
    if (fileName.find(L"..") != std::wstring::npos) return false;
    return true;
}

std::wstring DirFromHint(const std::wstring& dirHint) {
    if (dirHint == L"recordings") return RecordingsDir();
    if (dirHint == L"scripts") return ScriptsDir();
    return L"";
}

struct FindResult { std::wstring path; bool found = false; };

size_t StripCustomTextActions(std::vector<ScriptAction>& actions) {
    const size_t before = actions.size();
    actions.erase(std::remove_if(actions.begin(), actions.end(),
        [](const ScriptAction& a) { return a.type == ActionType::CustomText; }),
        actions.end());
    return before - actions.size();
}

FindResult FindScriptFile(const std::wstring& fileName, const std::wstring& dirHint) {
    if (!IsSafeFileName(fileName))
        return {L"[错误] 文件名包含非法字符。", false};

    const std::wstring hintDir = DirFromHint(dirHint);
    if (!hintDir.empty()) {
        const std::wstring path = hintDir + L"\\" + fileName;
        const DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return {path, true};
        const std::wstring label = (dirHint == L"recordings") ? L"键鼠录制目录" : L"脚本宏目录";
        return {L"[错误] 在" + label + L"中未找到文件：" + fileName, false};
    }

    std::wstring path = ScriptsDir() + L"\\" + fileName;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return {path, true};

    path = RecordingsDir() + L"\\" + fileName;
    attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return {path, true};

    return {L"[错误] 文件不存在：" + fileName + L"（已检查脚本目录和录制目录）", false};
}

bool ValidateJsonFileName(const std::wstring& fileName, std::wstring& error) {
    if (fileName.empty()) {
        error = L"缺少 fileName 参数。";
        return false;
    }
    if (!IsSafeFileName(fileName)) {
        error = L"文件名包含非法字符。";
        return false;
    }
    if (fileName.size() < 5 || fileName.substr(fileName.size() - 5) != L".json") {
        error = L"文件名必须以 .json 结尾。";
        return false;
    }
    return true;
}

AgentScriptOpResult FailMsg(const std::wstring& msg) {
    return {false, msg};
}

AgentScriptOpResult OkMsg(const std::wstring& msg) {
    AgentScriptOpResult r;
    r.ok = true;
    r.message = msg;
    NotifyAgentScriptLibraryChanged();
    return r;
}

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
        const int indent = segment[0].indent;

        for (const auto& a : segment) {
            if (a.type == ActionType::Wait) waits.push_back(a.duration);
            else if (a.type == ActionType::MoveMouse) {
                lastMove = a;
                hasMove = true;
            }
        }

        if (hasMove) {
            const double mergedWait = ComputeMergedWait(waits, waitMode);
            const int before = static_cast<int>(segment.size());
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
        const double dx = static_cast<double>(a.x - b.x);
        const double dy = static_cast<double>(a.y - b.y);
        return std::sqrt(dx * dx + dy * dy);
    };

    auto flushSegment = [&]() {
        if (segment.empty()) return;
        std::vector<Point> points;
        const int indent = segment[0].indent;
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

bool BuildActionsFromJson(const std::vector<json>& actionParams,
    std::vector<ScriptAction>& out, std::wstring& error) {
    out.clear();
    for (size_t i = 0; i < actionParams.size(); ++i) {
        json item = actionParams[i];
        ApplyResolvedAiModelToActionParams(item);
        auto built = BuildScriptActionFromJson(item);
        if (!built.ok) {
            error = L"第 " + std::to_wstring(i + 1) + L" 个动作：" + built.error;
            return false;
        }
        out.push_back(std::move(built.action));
    }
    EnsureStopMacroOnActions(out);
    NormalizeScriptActionList(out);
    if (const std::wstring endLoopErr = ValidateEndLoopPlacements(out); !endLoopErr.empty()) {
        error = endLoopErr;
        out.clear();
        return false;
    }
    return true;
}

}  // namespace

AgentScriptOpResult AgentSaveScriptContent(const std::wstring& fileName,
    const std::wstring& content, const std::wstring& dirHint) {
    std::wstring err;
    if (!ValidateJsonFileName(fileName, err)) return FailMsg(L"[错误] " + err);
    if (content.empty()) return FailMsg(L"[错误] 缺少 content 参数。");

    ScriptFileData data = ParseScriptContent(content);
    if (data.scriptName.empty()) return FailMsg(L"[错误] 内容缺少 scriptName 字段。");
    if (data.actions.empty()) return FailMsg(L"[错误] 内容中没有找到动作（actions 数组为空）。");

    const size_t stripped = StripCustomTextActions(data.actions);
    if (data.actions.empty())
        return FailMsg(L"[错误] 动作均为无效的 customText/未知类型，已拒绝保存。请用 buildScriptActions 生成。");
    const bool addedStopMacro = EnsureStopMacroOnActions(data.actions);
    NormalizeScriptActionList(data.actions);

    EnsureScriptsDir();
    const std::wstring targetDir = (dirHint == L"recordings") ? RecordingsDir() : ScriptsDir();
    const std::wstring fullPath = targetDir + L"\\" + fileName;

    if (!SaveScriptFileData(fullPath, data))
        return FailMsg(L"[错误] 写入文件失败：" + fileName);

    ScriptFileData verify = LoadScriptFileData(fullPath);
    if (verify.actions.empty()) return FailMsg(L"[错误] 写入后验证失败。");

    bool modelFilled = false;
    for (auto& a : verify.actions) {
        const std::wstring before = a.aiModelName;
        EnsureAiModelOnAction(a);
        if (a.aiModelName != before) modelFilled = true;
    }
    if (modelFilled) {
        NormalizeScriptActionList(verify.actions);
        if (!SaveScriptFileData(fullPath, verify))
            return FailMsg(L"[错误] 补全 AI 模型后保存失败：" + fileName);
    }

    const std::wstring dirLabel = (dirHint == L"recordings") ? L"录制" : L"脚本";
    std::wstring msg = L"✓ " + dirLabel + L"已保存：" + fileName + L"\n"
        L"  名称: " + verify.scriptName + L"\n"
        L"  动作数: " + std::to_wstring(verify.actions.size());
    if (stripped > 0)
        msg += L"\n  [提示] 已移除 " + std::to_wstring(stripped) + L" 个无效 customText 动作（说明应写 remark）";
    if (addedStopMacro)
        msg += L"\n  [提示] 已自动追加 stopMacro（结束宏运行）";
    return OkMsg(msg);
}

AgentScriptOpResult AgentCreateMacroScript(const std::wstring& fileName,
    const std::wstring& scriptName, const std::vector<json>& actions) {
    std::wstring err;
    if (!ValidateJsonFileName(fileName, err)) return FailMsg(L"[错误] " + err);
    if (scriptName.empty()) return FailMsg(L"[错误] 缺少 scriptName 参数。");
    if (actions.empty()) return FailMsg(L"[错误] actions 数组为空。");

    std::vector<ScriptAction> builtActions;
    if (!BuildActionsFromJson(actions, builtActions, err))
        return FailMsg(L"[错误] " + err);

    ScriptFileData data;
    data.scriptName = scriptName;
    data.recordTime = NowText();
    data.actions = std::move(builtActions);
    double totalWait = 0;
    for (const auto& a : data.actions)
        if (a.type == ActionType::Wait) totalWait += a.duration;
    data.durationSeconds = totalWait;

    EnsureScriptsDir();
    const std::wstring fullPath = ScriptsDir() + L"\\" + fileName;
    if (!SaveScriptFileData(fullPath, data))
        return FailMsg(L"[错误] 保存鼠标宏失败：" + fileName);

    return OkMsg(L"✓ 鼠标宏已创建：" + fileName + L"\n"
        L"  名称: " + data.scriptName + L"\n"
        L"  动作数: " + std::to_wstring(data.actions.size()) + L"\n"
        L"  路径: scripts\\" + fileName);
}

AgentScriptOpResult AgentOptimizeScriptFile(const AgentOptimizeOptions& options) {
    if (options.fileName.empty()) return FailMsg(L"[错误] 缺少 fileName 参数。");

    const auto found = FindScriptFile(options.fileName, options.dir);
    if (!found.found) return FailMsg(found.path);

    ScriptFileData data = LoadScriptFileData(found.path);
    if (data.actions.empty()) return FailMsg(L"[提示] 脚本为空，无需优化：" + options.fileName);

    const int originalCount = static_cast<int>(data.actions.size());
    std::wstring resultMsg;
    if (options.mergeMode == L"compressPath")
        resultMsg = ApplyMoveCompress(data, options.distanceThreshold, options.compressWait);
    else
        resultMsg = ApplyMoveMerge(data, options.waitCalculation);

    std::wstring savePath;
    if (!options.outputFileName.empty()) {
        std::wstring outErr;
        if (!ValidateJsonFileName(options.outputFileName, outErr))
            return FailMsg(L"[错误] " + outErr);

        std::wstring targetDir;
        if (!options.outputDir.empty()) {
            targetDir = DirFromHint(options.outputDir);
            if (targetDir.empty())
                return FailMsg(L"[错误] 输出目录必须为 \"scripts\" 或 \"recordings\"。");
        } else {
            targetDir = found.path.substr(0, found.path.rfind(L'\\'));
        }
        savePath = targetDir + L"\\" + options.outputFileName;
    } else {
        savePath = found.path;
    }

    EnsureScriptsDir();
    if (!SaveScriptFileData(savePath, data))
        return FailMsg(L"[错误] 保存失败：" + savePath);

    const std::wstring savedName = options.outputFileName.empty()
        ? options.fileName : options.outputFileName;
    return OkMsg(resultMsg + L"\n\n已保存: " + savedName
        + L"\n原始动作数: " + std::to_wstring(originalCount)
        + L" → 优化后: " + std::to_wstring(data.actions.size()));
}

AgentScriptOpResult AgentDeleteScriptFile(const std::wstring& fileName,
    const std::wstring& dirHint) {
    std::wstring err;
    if (!ValidateJsonFileName(fileName, err)) return FailMsg(L"[错误] " + err);
    if (dirHint != L"scripts" && dirHint != L"recordings")
        return FailMsg(L"[错误] dir 必须为 scripts 或 recordings。");

    const auto found = FindScriptFile(fileName, dirHint);
    if (!found.found) return FailMsg(found.path);

    DeleteUnreferencedImagesOfScript(found.path);
    if (!DeleteFileW(found.path.c_str()))
        return FailMsg(L"[错误] 删除失败：" + fileName);

    const std::wstring label = (dirHint == L"recordings") ? L"键鼠录制" : L"鼠标宏";
    return OkMsg(L"✓ 已删除" + label + L"：" + fileName);
}
