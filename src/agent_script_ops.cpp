#include "agent_script_ops.h"

#include "action_tree.h"
#include "agent_ui_notify.h"
#include "agent_ai_actions.h"
#include "recorder_timeline.h"
#include "script_action_builder.h"
#include "script_io.h"
#include "utils.h"
#include "window_mode/window_mode_json.h"

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

bool ApplyAgentWindowModeParams(const json& params, windowmode::WindowModeScriptConfig& cfg) {
    bool changed = false;
    if (params.contains("scriptMode") && params["scriptMode"].is_string()) {
        cfg = windowmode::DefaultWindowModeConfig();
        const std::string mode = params["scriptMode"].get<std::string>();
        if (mode == "window") {
            cfg.enabled = true;
            cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
        } else if (mode == "backgroundWindow" || mode == "background") {
            cfg.enabled = true;
            cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
        }
        changed = true;
    }
    if (params.contains("windowMode") && params["windowMode"].is_object()) {
        const auto& wm = params["windowMode"];
        cfg = windowmode::DefaultWindowModeConfig();
        if (wm.contains("enabled")) cfg.enabled = wm.value("enabled", 0) != 0;
        if (wm.contains("executionKind")) {
            const std::string kind = wm.value("executionKind", "hiddenDesktop");
            cfg.executionKind = kind == "backgroundWindow"
                ? windowmode::WindowModeExecutionKind::BackgroundWindow
                : windowmode::WindowModeExecutionKind::HiddenDesktop;
        }
        if (wm.contains("targetExePath")) cfg.targetExePath = FromUtf8(wm.value("targetExePath", ""));
        if (wm.contains("targetWindowTitle")) cfg.targetWindowTitle = FromUtf8(wm.value("targetWindowTitle", ""));
        if (wm.contains("coordSpace")) {
            const std::string space = wm.value("coordSpace", "windowClient");
            cfg.coordSpace = space == "screenAbsolute"
                ? windowmode::WindowModeCoordinateSpace::ScreenAbsolute
                : windowmode::WindowModeCoordinateSpace::WindowClient;
        }
        if (wm.contains("autoLaunchTarget")) cfg.autoLaunchTarget = wm.value("autoLaunchTarget", 0) != 0;
        if (wm.contains("launchArgs")) cfg.launchArgs = FromUtf8(wm.value("launchArgs", ""));
        if (wm.contains("selectMethod")) {
            const std::string method = wm.value("selectMethod", "selectOnStartup");
            if (method == "mousePositionOnStartup") {
                cfg.selectMethod = windowmode::WindowSelectMethod::MousePositionOnStartup;
            } else if (method == "useEditorWindowClass") {
                cfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
            } else if (method == "noSelect") {
                cfg.selectMethod = windowmode::WindowSelectMethod::NoSelect;
            } else {
                cfg.selectMethod = windowmode::WindowSelectMethod::SelectOnStartup;
            }
        }
        if (wm.contains("windowName")) cfg.windowName = FromUtf8(wm.value("windowName", ""));
        if (wm.contains("windowClassName")) cfg.windowClassName = FromUtf8(wm.value("windowClassName", ""));
        if (wm.contains("childWindowClassName")) {
            cfg.childWindowClassName = FromUtf8(wm.value("childWindowClassName", ""));
        }
        if (wm.contains("useTopLevelWindow")) cfg.useTopLevelWindow = wm.value("useTopLevelWindow", 1) != 0;
        if (wm.contains("targetPickX")) cfg.targetPickX = wm.value("targetPickX", 0);
        if (wm.contains("targetPickY")) cfg.targetPickY = wm.value("targetPickY", 0);
        if (wm.contains("allowForegroundInputFallback")) {
            cfg.allowForegroundInputFallback = wm.value("allowForegroundInputFallback", 0) != 0;
        }
        if (wm.contains("fakeFocusEnabled")) {
            cfg.fakeFocusEnabled = wm.value("fakeFocusEnabled", 0) != 0;
        }
        if (wm.contains("inputStrategy")) {
            const std::string s = wm.value("inputStrategy", "auto");
            if (s == "softMessage" || s == "postMessage") {
                cfg.inputStrategy = windowmode::WindowModeInputStrategy::SoftMessage;
            } else if (s == "cdp") {
                cfg.inputStrategy = windowmode::WindowModeInputStrategy::Cdp;
            } else {
                cfg.inputStrategy = windowmode::WindowModeInputStrategy::Auto;
            }
        }
        if (wm.contains("cdpPort")) {
            cfg.cdpPort = wm.value("cdpPort", 9222);
            if (cfg.cdpPort <= 0) cfg.cdpPort = 9222;
        }
        windowmode::AnnotateInputStrategyForSave(cfg);
        if (cfg.windowName.empty() && !cfg.targetWindowTitle.empty()) {
            cfg.windowName = cfg.targetWindowTitle;
        }
        changed = true;
    }
    return changed;
}

double ApplyAgentBreakoutTimeParams(const json& params, bool windowModeEnabled) {
    if (windowModeEnabled) return 0.0;
    if (params.contains("breakoutTimeSeconds")) {
        const double raw = params.value("breakoutTimeSeconds", 0.0);
        return NormalizeBreakoutTimeSeconds(raw);
    }
    return 0.0;
}

bool IsKeyOperation(ActionType type) {
    return type != ActionType::MoveMouse
        && type != ActionType::MoveMouseRelative
        && type != ActionType::Wait;
}

double WaitSecondsFromAction(const ScriptAction& a) {
    return ActionStepUs(a) / 1000000.0;
}

ScriptAction MakeSyncedWait(double seconds, int indent, const std::wstring& remark = L"") {
    ScriptAction wa{};
    wa.type = ActionType::Wait;
    wa.duration = std::max(0.0, seconds);
    wa.timingUs = ActionStepUs(wa); // uses duration when timingUs==0 — need set after
    if (wa.duration > 0.0) {
        const long double us = static_cast<long double>(wa.duration) * 1000000.0L;
        wa.timingUs = static_cast<uint64_t>(std::llround(us));
    } else {
        wa.timingUs = 0;
    }
    wa.randomDuration = 0.0;
    wa.indent = indent;
    wa.customText = L"等待";
    wa.remark = remark;
    return wa;
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
    int preservedRelativeSegments = 0;

    auto flushSegment = [&]() {
        if (segment.empty()) return;
        // 相对轨迹受保护：与优化对话框一致，禁止 merge 丢掉中间相对包。
        bool hasRelative = false;
        for (const auto& a : segment) {
            if (a.type == ActionType::MoveMouseRelative) { hasRelative = true; break; }
        }
        if (hasRelative) {
            for (auto& a : segment) result.push_back(std::move(a));
            segment.clear();
            ++preservedRelativeSegments;
            return;
        }

        std::vector<double> waits;
        ScriptAction lastMove{};
        bool hasMove = false;
        const int indent = segment[0].indent;

        for (const auto& a : segment) {
            if (a.type == ActionType::Wait) waits.push_back(WaitSecondsFromAction(a));
            else if (a.type == ActionType::MoveMouse) {
                lastMove = a;
                lastMove.duration = 0.0;
                lastMove.timingUs = 0;
                lastMove.randomDuration = 0.0;
                hasMove = true;
            }
        }

        if (hasMove) {
            const double mergedWait = ComputeMergedWait(waits, waitMode);
            const int before = static_cast<int>(segment.size());
            if (mergedWait > 0.0005)
                result.push_back(MakeSyncedWait(mergedWait, indent, L"已合并"));
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
        if (a.type == ActionType::Wait) total += WaitSecondsFromAction(a);
    data.durationSeconds = total;
    data.inputTimingVersion = kInputTimingVersionExplicitWaits;

    std::wstringstream ss;
    ss << L"优化完成：合并了 " << mergedSegments << L" 个分段，移除 " << removedActions
       << L" 个冗余动作。\n新脚本总动作数: " << data.actions.size()
       << L"，总等待时长: " << total << L" 秒。";
    if (preservedRelativeSegments > 0) {
        ss << L"\n已保留 " << preservedRelativeSegments
           << L" 段相对移动轨迹（FPS 相对位移不可合并）。";
    }
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
        const int indent = segment[0].indent;

        // 含相对移动：整段原样保留（与优化对话框「相对轨迹受保护」一致）
        bool hasRelative = false;
        for (const auto& a : segment) {
            if (a.type == ActionType::MoveMouseRelative) { hasRelative = true; break; }
        }
        if (hasRelative) {
            for (auto& a : segment) result.push_back(std::move(a));
            segment.clear();
            return;
        }

        // 收集绝对 Move 点，以及点之间 Wait/残留前延迟的 ActionStepUs 之和
        std::vector<Point> points;
        std::vector<uint64_t> gapBefore; // gapBefore[i] = 到达 points[i+1] 前的等待微秒
        uint64_t pendingGap = 0;
        std::vector<ScriptAction> leadingWaits;
        bool seenMove = false;
        for (const auto& a : segment) {
            if (a.type == ActionType::Wait) {
                if (!seenMove) leadingWaits.push_back(a);
                else pendingGap += ActionStepUs(a);
                continue;
            }
            if (a.type == ActionType::MoveMouse) {
                if (!seenMove) {
                    for (auto& w : leadingWaits) result.push_back(std::move(w));
                    leadingWaits.clear();
                    seenMove = true;
                    points.push_back({a.x, a.y});
                    pendingGap = ActionStepUs(a); // 迁移期残留前延迟并入下一段间隙
                } else {
                    gapBefore.push_back(pendingGap + ActionStepUs(a));
                    pendingGap = 0;
                    points.push_back({a.x, a.y});
                }
            }
        }
        for (auto& w : leadingWaits) result.push_back(std::move(w));

        if (points.size() < 2) {
            for (auto& a : segment) result.push_back(std::move(a));
            segment.clear();
            return;
        }

        std::vector<Point> compressed;
        std::vector<uint64_t> compressedGaps;
        compressed.push_back(points.front());
        size_t lastKept = 0;
        for (size_t i = 1; i + 1 < points.size(); ++i) {
            if (dist(compressed.back(), points[i]) < distanceThreshold) continue;
            uint64_t gapSum = 0;
            for (size_t g = lastKept; g < i; ++g) gapSum += gapBefore[g];
            compressedGaps.push_back(gapSum);
            compressed.push_back(points[i]);
            lastKept = i;
        }
        {
            uint64_t gapSum = 0;
            for (size_t g = lastKept; g < gapBefore.size(); ++g) gapSum += gapBefore[g];
            if (compressed.back().x != points.back().x || compressed.back().y != points.back().y) {
                compressedGaps.push_back(gapSum);
                compressed.push_back(points.back());
            } else if (!compressedGaps.empty()) {
                // 终点已保留：丢弃的中间点间隙已计入上一 gap；尾间隙无下一 Wait
            } else {
                // 仅首尾且首==尾不应发生
            }
        }

        for (size_t i = 0; i < compressed.size(); ++i) {
            if (i > 0) {
                const uint64_t gap = (i - 1 < compressedGaps.size()) ? compressedGaps[i - 1] : 0;
                if (gap > 0)
                    result.push_back(MakeExplicitWaitUs(gap, indent));
                else if (compressWait > 0.0005)
                    result.push_back(MakeSyncedWait(compressWait, indent));
            }
            ScriptAction mv{};
            mv.type = ActionType::MoveMouse;
            mv.x = compressed[i].x;
            mv.y = compressed[i].y;
            mv.indent = indent;
            mv.duration = 0.0;
            mv.timingUs = 0;
            mv.customText = L"移动到 (" + std::to_wstring(mv.x) + L", "
                + std::to_wstring(mv.y) + L")";
            result.push_back(mv);
        }
        removedPoints += static_cast<int>(points.size()) - static_cast<int>(compressed.size());
        ++compressedSegments;
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
        if (a.type == ActionType::Wait) total += WaitSecondsFromAction(a);
    data.durationSeconds = total;
    data.inputTimingVersion = kInputTimingVersionExplicitWaits;

    std::wstringstream ss;
    ss << L"路径压缩完成：压缩了 " << compressedSegments << L" 个路径段，移除 "
       << removedPoints << L" 个冗余移动点（阈值 " << distanceThreshold << L" 像素）。\n"
       << L"新脚本总动作数: " << data.actions.size()
       << L"（点间优先保留原 Wait 总和；compressWait 仅在总和为 0 时作可选间隔）。";
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
    if (dirHint == L"recordings") {
        data.windowMode = windowmode::DefaultWindowModeConfig();
        data.breakoutTimeSeconds = 0;
        NormalizeInputTiming(data, RecordingsDir() + L"\\" + fileName, true);
    } else {
        data.breakoutTimeSeconds = EffectiveBreakoutTimeSeconds(data);
        NormalizeInputTiming(data, ScriptsDir() + L"\\" + fileName, false);
    }

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
        L"  模式: " + windowmode::WindowModeConfigSummary(verify.windowMode) + L"\n";
    if (!verify.windowMode.enabled) {
        msg += L"  脱离时间: " + std::to_wstring(
            static_cast<int>(EffectiveBreakoutTimeSeconds(verify))) + L" 秒\n";
    }
    msg += L"  动作数: " + std::to_wstring(verify.actions.size());
    if (stripped > 0)
        msg += L"\n  [提示] 已移除 " + std::to_wstring(stripped) + L" 个无效 customText 动作（说明应写 remark）";
    if (addedStopMacro)
        msg += L"\n  [提示] 已自动追加 stopMacro（结束宏运行）";
    return OkMsg(msg);
}

AgentScriptOpResult AgentCreateMacroScript(const std::wstring& fileName,
    const std::wstring& scriptName, const std::vector<json>& actions,
    const json& extraParams) {
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
    data.windowMode = windowmode::DefaultWindowModeConfig();
    ApplyAgentWindowModeParams(extraParams, data.windowMode);
    data.breakoutTimeSeconds = ApplyAgentBreakoutTimeParams(extraParams, data.windowMode.enabled);
    double totalWait = 0;
    for (const auto& a : data.actions)
        if (a.type == ActionType::Wait) totalWait += ActionStepUs(a) / 1000000.0;
    data.durationSeconds = totalWait;
    data.inputTimingVersion = kInputTimingVersionExplicitWaits;

    EnsureScriptsDir();
    const std::wstring fullPath = ScriptsDir() + L"\\" + fileName;
    if (!SaveScriptFileData(fullPath, data))
        return FailMsg(L"[错误] 保存鼠标宏失败：" + fileName);

    std::wstring msg = L"✓ 鼠标宏已创建：" + fileName + L"\n"
        L"  名称: " + data.scriptName + L"\n"
        L"  模式: " + windowmode::WindowModeConfigSummary(data.windowMode) + L"\n";
    if (!data.windowMode.enabled) {
        msg += L"  脱离时间: " + std::to_wstring(static_cast<int>(data.breakoutTimeSeconds)) + L" 秒\n";
    }
    msg += L"  动作数: " + std::to_wstring(data.actions.size()) + L"\n"
        L"  路径: scripts\\" + fileName;
    return OkMsg(msg);
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
