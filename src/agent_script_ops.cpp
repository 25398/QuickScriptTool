#include "agent_script_ops.h"

#include "action_tree.h"
#include "action_utils.h"
#include "ai_logic_convert.h"
#include "agent_ui_notify.h"
#include "agent_undo.h"
#include "agent_ai_actions.h"
#include "recording_optimize_ops.h"
#include "recorder_timeline.h"
#include "script_action_builder.h"
#include "script_io.h"
#include "utils.h"
#include "window_mode/window_mode_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

using json = nlohmann::json;

// AI 工具改文件前的撤销快照作用域：析构时未显式成功则记为失败。
struct UndoScope {
    std::wstring id;
    std::wstring path;
    bool done = false;

    UndoScope(const std::wstring& tool, const std::wstring& title,
              const std::wstring& filePath) {
        path = filePath;
        const std::wstring before = ReadAll(path);
        const bool existed = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        id = AgentUndoBegin(tool, title, path, before, existed);
    }

    ~UndoScope() {
        if (!done) AgentUndoFinish(id, L"", false);
    }

    void Success() {
        if (done) return;
        done = true;
        AgentUndoFinish(id, ReadAll(path), true);
    }
};

bool IsSafeFileName(const std::wstring& fileName) {
    if (fileName.find(L'\\') != std::wstring::npos) return false;
    if (fileName.find(L'/') != std::wstring::npos) return false;
    if (fileName.find(L"..") != std::wstring::npos) return false;
    return true;
}

// 动作完整性校验：AI 手写残缺 JSON 时，重建校验（BuildScriptActionFromJson）往往仍能通过
// （类型专用字段有默认值），这里再按类型检查必填内容，保证只有规范化产物能落盘。
bool ValidateScriptActionCompleteness(const ScriptAction& a, std::wstring& err) {
    switch (a.type) {
    case ActionType::FindImage:
        if (Trim(a.imagePath).empty()) {
            err = L"findImage 缺少 imagePath（要找的图）。手写残缺动作会被拒绝，请用 buildScriptActions 生成";
            return false;
        }
        break;
    case ActionType::TextRecognition:
        if (Trim(a.imagePath).empty() && Trim(a.ocrSearchText).empty()) {
            err = L"textRecognition 需要 imagePath 或 ocrSearchText 至少一项";
            return false;
        }
        break;
    case ActionType::If:
        if (Trim(a.conditionExpr).empty()) {
            err = L"if 缺少 conditionExpr 条件表达式";
            return false;
        }
        break;
    case ActionType::Goto:
        if (Trim(a.gotoStepExpr).empty()) {
            err = L"goto 缺少 gotoStepExpr 目标序号";
            return false;
        }
        break;
    case ActionType::DefineBlock:
    case ActionType::RunBlock:
        if (Trim(a.blockName).empty()) {
            err = L"块动作缺少 blockName";
            return false;
        }
        break;
    case ActionType::RunMacro:
    case ActionType::MousePlayback:
        if (Trim(a.targetPath).empty()) {
            err = L"动作缺少 targetPath";
            return false;
        }
        break;
    case ActionType::QuickInput:
        if (Trim(a.inputText).empty()) {
            err = L"quickInput 缺少 inputText";
            return false;
        }
        break;
    case ActionType::AiTextAnalysis:
    case ActionType::AiImageAnalysis:
    case ActionType::AiActionExecute:
        if (Trim(a.aiPrompt).empty()) {
            err = L"AI 动作缺少 aiPrompt";
            return false;
        }
        break;
    default:
        break;
    }
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
        if (wm.contains("windowRelativeCoordinates")) {
            cfg.windowRelativeCoordinates = wm.value("windowRelativeCoordinates", 0) != 0;
        }
        if (wm.contains("recordClientWidth")) {
            cfg.recordClientWidth = wm.value("recordClientWidth", 0);
        }
        if (wm.contains("recordClientHeight")) {
            cfg.recordClientHeight = wm.value("recordClientHeight", 0);
        }
        if (wm.contains("autoLaunchTarget")) cfg.autoLaunchTarget = wm.value("autoLaunchTarget", 0) != 0;
        if (wm.contains("launchArgs")) cfg.launchArgs = FromUtf8(wm.value("launchArgs", ""));
        if (wm.contains("selectMethod")) {
            cfg.selectMethod = windowmode::SelectMethodFromJsonUtf8(
                wm.value("selectMethod", "selectOnStartup"));
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
        windowmode::StripRuntimeOnlySelectTarget(cfg);
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

bool BuildActionsFromJson(const std::vector<json>& actionParams,
    std::vector<ScriptAction>& out, std::wstring& error) {
    out.clear();
    std::vector<json> flat;
    if (!FlattenNestedActionParamList(actionParams, flat, error))
        return false;
    for (size_t i = 0; i < flat.size(); ++i) {
        json item = flat[i];
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
    if (const std::wstring bodyErr = ValidateContainerBodies(out); !bodyErr.empty()) {
        error = bodyErr;
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
    const int strippedLogic = StripUnauthorizedLogicConvert(data.actions);
    const bool addedStopMacro = EnsureStopMacroOnActions(data.actions);
    NormalizeScriptActionList(data.actions);

    // 强制规范动作：每个动作必须能由 buildScriptActions 重建（禁止 AI 手写动作 JSON）。
    // writeScript 只接受规范化产物；AI 生成脚本应走 buildScriptActions + createMacroScript。
    for (size_t i = 0; i < data.actions.size(); ++i) {
        json actionJson;
        try {
            actionJson = json::parse(ToUtf8(ScriptActionToJsonString(data.actions[i])));
        } catch (...) {
            return FailMsg(L"[错误] 第 " + std::to_wstring(i + 1)
                + L" 个动作无法序列化，疑似手写 JSON。请先用 buildScriptActions 生成规范动作，"
                + L"再通过 writeScript 或 createMacroScript 保存。");
        }
        auto rebuilt = BuildScriptActionFromJson(actionJson);
        if (!rebuilt.ok) {
            return FailMsg(L"[错误] 第 " + std::to_wstring(i + 1)
                + L" 个动作未通过规范化校验（" + rebuilt.error
                + L"）。禁止手写动作 JSON：请先用 buildScriptActions 生成 actions 数组，"
                + L"再通过 writeScript 或 createMacroScript 保存。");
        }
        std::wstring completeErr;
        if (!ValidateScriptActionCompleteness(data.actions[i], completeErr)) {
            return FailMsg(L"[错误] 第 " + std::to_wstring(i + 1)
                + L" 个动作不完整（" + completeErr
                + L"）。禁止手写动作 JSON：请先用 buildScriptActions 生成 actions 数组，"
                + L"再通过 writeScript 或 createMacroScript 保存。");
        }
    }

    if (const std::wstring endLoopErr = ValidateEndLoopPlacements(data.actions); !endLoopErr.empty())
        return FailMsg(L"[错误] " + endLoopErr);
    if (const std::wstring bodyErr = ValidateContainerBodies(data.actions); !bodyErr.empty())
        return FailMsg(L"[错误] " + bodyErr);

    EnsureScriptsDir();
    const std::wstring targetDir = (dirHint == L"recordings") ? RecordingsDir() : ScriptsDir();
    const std::wstring fullPath = targetDir + L"\\" + fileName;
    const std::wstring undoTitle = std::wstring(L"保存")
        + (dirHint == L"recordings" ? L"录制" : L"脚本") + L" " + fileName;
    UndoScope undo(L"writeScript", undoTitle,
        fullPath);

    if (!SaveScriptFileData(fullPath, data))
        return FailMsg(L"[错误] 写入文件失败：" + fileName);
    undo.Success();

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
    if (strippedLogic > 0)
        msg += L"\n  [提示] 已清除 " + std::to_wstring(strippedLogic)
            + L" 处未授权的逻辑转化（用户未明确要求）";
    if (addedStopMacro)
        msg += L"\n  [提示] 已自动追加 stopMacro（结束宏运行）";
    msg += L"\n\n" + FormatScriptActionsOutline(verify.actions);
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
    StripUnauthorizedLogicConvert(builtActions);

    ScriptFileData data;
    data.scriptName = scriptName;
    data.recordTime = NowText();
    data.actions = std::move(builtActions);
    data.windowMode = windowmode::DefaultWindowModeConfig();
    // 新建脚本不绑定专用热键：Hotkey 默认值是 F8+启用，会与全局启停热键冲突
    // （表现为「热键注册失败」），且用户未要求时不应给脚本设置热键。
    data.hotkey.vk = 0;
    data.hotkey.modifiers = 0;
    data.hotkey.text.clear();
    data.hotkey.enabled = false;
    data.hotkey.holdMode = false;
    ApplyAgentWindowModeParams(extraParams, data.windowMode);
    data.breakoutTimeSeconds = ApplyAgentBreakoutTimeParams(extraParams, data.windowMode.enabled);
    double totalWait = 0;
    for (const auto& a : data.actions)
        if (a.type == ActionType::Wait) totalWait += ActionStepUs(a) / 1000000.0;
    data.durationSeconds = totalWait;
    data.inputTimingVersion = kInputTimingVersionExplicitWaits;

    EnsureScriptsDir();
    // 目标目录：默认 scripts 根目录（不分类）；用户指明分类时用 folder 子目录
    std::wstring folder;
    if (extraParams.is_object())
        folder = NormalizeRelativeFolder(
            Trim(FromUtf8(extraParams.value("folder", ""))));
    if (!folder.empty() && !IsSafeRelativeFolder(folder))
        return FailMsg(L"[错误] folder 不合法：仅允许字母数字与 / 分隔的相对目录。");
    std::wstring saveDir = ScriptsDir();
    if (!folder.empty()) {
        if (!EnsureRelativeFolder(saveDir, folder))
            return FailMsg(L"[错误] 无法创建目标目录：" + folder);
        saveDir = saveDir + L"\\" + folder;
    }
    const std::wstring fullPath = saveDir + L"\\" + fileName;
    UndoScope undo(L"createMacroScript", L"创建鼠标宏 " + fileName, fullPath);
    if (!SaveScriptFileData(fullPath, data))
        return FailMsg(L"[错误] 保存鼠标宏失败：" + fileName);
    undo.Success();

    std::wstring msg = L"✓ 鼠标宏已创建：" + fileName + L"\n"
        L"  名称: " + data.scriptName + L"\n"
        L"  模式: " + windowmode::WindowModeConfigSummary(data.windowMode) + L"\n";
    if (!data.windowMode.enabled) {
        msg += L"  脱离时间: " + std::to_wstring(static_cast<int>(data.breakoutTimeSeconds)) + L" 秒\n";
    }
    msg += L"  动作数: " + std::to_wstring(data.actions.size()) + L"\n"
        L"  路径: scripts\\" + (folder.empty() ? L"" : (folder + L"\\")) + fileName;
    msg += L"\n\n" + FormatScriptActionsOutline(data.actions);
    return OkMsg(msg);
}

AgentScriptOpResult AgentOptimizeScriptFile(const AgentOptimizeOptions& options) {
    if (options.fileName.empty()) return FailMsg(L"[错误] 缺少 fileName 参数。");

    const auto found = FindScriptFile(options.fileName, options.dir);
    if (!found.found) return FailMsg(found.path);

    ScriptFileData data = LoadScriptFileData(found.path);
    if (data.actions.empty()) return FailMsg(L"[提示] 脚本为空，无需优化：" + options.fileName);

    const int originalCount = static_cast<int>(data.actions.size());
    recopt::OptimizeApplyResult applied;
    const bool compress = options.mergeMode == L"compressPath" || options.mergeMode == L"compress";
    if (compress) {
        applied = recopt::CompressAllKeySplit(
            data.actions, options.distanceThreshold, options.compressWait);
    } else {
        applied = recopt::MergeAllKeySplit(
            data.actions, ToUtf8(options.waitCalculation), options.mergeWaitValue);
    }
    if (!applied.collectOk)
        return FailMsg(L"[错误] " + FromUtf8(applied.collectErr));
    if (applied.applied == 0) {
        if (applied.skippedRelative > 0)
            return FailMsg(L"[错误] 相对移动轨迹受保护，没有可合并/压缩的绝对移动段。");
        return FailMsg(compress
            ? L"[错误] 没有可压缩的路径段（每段至少需要两个绝对移动点）。"
            : L"[错误] 没有可合并的鼠标移动段。");
    }

    double totalWait = 0;
    for (const auto& a : data.actions)
        if (a.type == ActionType::Wait) totalWait += ActionStepUs(a) / 1000000.0;
    data.durationSeconds = totalWait;
    data.inputTimingVersion = kInputTimingVersionExplicitWaits;

    std::wstringstream ss;
    if (compress) {
        ss << L"路径压缩完成：压缩了 " << applied.applied << L" 个路径段（阈值 "
           << options.distanceThreshold << L" 像素）。\n"
           << L"新脚本总动作数: " << data.actions.size()
           << L"（与产品录制优化「鼠标移动压缩」相同：按关键动作分段）。";
    } else {
        ss << L"优化完成：合并了 " << applied.applied << L" 个分段。\n"
           << L"新脚本总动作数: " << data.actions.size()
           << L"，总等待时长: " << totalWait << L" 秒。"
           << L"\n（与产品录制优化「鼠标移动合并」相同：按关键动作分段，等待时间按段计算）。";
    }
    if (applied.skippedRelative > 0) {
        ss << L"\n已保留 " << applied.skippedRelative
           << L" 段相对移动轨迹（FPS 相对位移不可合并/压缩）。";
    }
    const std::wstring resultMsg = ss.str();

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
    UndoScope undo(L"optimizeScript", L"优化脚本 " + options.fileName, savePath);
    if (!SaveScriptFileData(savePath, data))
        return FailMsg(L"[错误] 保存失败：" + savePath);
    undo.Success();

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

    const std::wstring undoTitle = std::wstring(L"删除")
        + (dirHint == L"recordings" ? L"录制" : L"脚本") + L" " + fileName;
    UndoScope undo(L"deleteScriptFile", undoTitle,
        found.path);
    DeleteUnreferencedImagesOfScript(found.path);
    if (!DeleteFileW(found.path.c_str()))
        return FailMsg(L"[错误] 删除失败：" + fileName);
    undo.Success();

    const std::wstring label = (dirHint == L"recordings") ? L"键鼠录制" : L"鼠标宏";
    return OkMsg(L"✓ 已删除" + label + L"：" + fileName);
}

namespace {

std::wstring TrimShort(const std::wstring& s, size_t maxChars) {
    std::wstring t = Trim(s);
    if (t.size() > maxChars) t = t.substr(0, maxChars) + L"…";
    return t;
}

std::wstring CoordText(const ScriptAction& a, int x, int y) {
    if (a.coordsAreNormalized) {
        wchar_t buf[64];
        swprintf_s(buf, L"(%.3f, %.3f)", a.nx, a.ny);
        return buf;
    }
    return L"(" + std::to_wstring(x) + L", " + std::to_wstring(y) + L")";
}

std::wstring FmtNum(double v) {
    wchar_t buf[48];
    swprintf_s(buf, L"%.2f", v);
    std::wstring s = buf;
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s.empty() ? L"0" : s;
}

std::wstring FmtColor(int r, int g, int b) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%02X%02X%02X",
        std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
    return buf;
}

std::wstring CoordOrExpr(const ScriptAction& a, int x, int y) {
    if (a.moveFromVar) {
        return L"(" + (a.moveVarExprX.empty() ? L"?" : a.moveVarExprX)
            + L", " + (a.moveVarExprY.empty() ? L"?" : a.moveVarExprY) + L")";
    }
    return CoordText(a, x, y);
}

std::wstring RepeatBrief(const ScriptAction& a) {
    if (a.clickCount <= 1 && a.duration <= 0.01 && a.randomDuration <= 0.0) return L"";
    std::wstring s;
    if (a.clickCount > 1) s += L" ×" + std::to_wstring(a.clickCount);
    if (a.duration > 0.01) s += L" 间隔" + FmtNum(a.duration) + L"s";
    if (a.randomDuration > 0.0) s += L" +随机" + FmtNum(a.randomDuration) + L"s";
    return s;
}

std::wstring HoldPrefix(const ScriptAction& a) {
    const std::wstring h = HoldText(a);
    return h.empty() ? L"" : (h + L"+");
}

std::wstring RegionText(const ScriptAction& a) {
    if (a.searchFullScreen) return L"";
    return L" 区域" + CoordText(a, a.searchX1, a.searchY1)
        + L"-" + CoordText(a, a.searchX2, a.searchY2);
}

std::wstring AiDetailText(const ScriptAction& a) {
    std::wstring s;
    if (a.type == ActionType::AiImageAnalysis) {
        if (!a.aiTargetImagePath.empty())
            s += L" 图:" + a.aiTargetImagePath + (a.aiImageUseVar ? L"(变量)" : L"");
        if (a.aiRegionByImage) s += L" 按锚点图";
    }
    if (a.type == ActionType::AiActionExecute && a.aiWithImage) s += L" 带截图";
    if (a.aiOutputType == 1) s += L" 输出整数";
    if (a.aiTimeoutSec != 30) s += L" 超时" + std::to_wstring(a.aiTimeoutSec) + L"s";
    if (!a.aiFallbackValue.empty()) s += L" 失败降级:" + TrimShort(a.aiFallbackValue, 30);
    if (a.type == ActionType::AiActionExecute && a.aiMaxSteps != 10)
        s += L" 最多" + std::to_wstring(a.aiMaxSteps) + L"步";
    if (a.aiLogicConvert) s += L" 逻辑转化";
    return s;
}

std::wstring KeyText(const ScriptAction& a) {
    if (!a.keyText.empty()) return a.keyText;
    if (a.keyVk != 0) return VkName(a.keyVk);
    return L"?";
}

std::wstring RunTargetText(const ScriptAction& a) {
    if (!a.targetPath.empty()) return TrimShort(a.targetPath, 80);
    if (!a.blockName.empty()) return a.blockName;
    return L"未选择";
}

std::wstring FindColorText(const ScriptAction& a) {
    std::wstring s = L"找色 " + FmtColor(a.colorR, a.colorG, a.colorB)
        + L" 容差" + std::to_wstring(a.colorTolerance);
    s += RegionText(a);
    if (a.findImageFollowUp == 0) {
        s += L" → 点击";
        if (a.offsetX != 0 || a.offsetY != 0)
            s += L" 偏移" + CoordText(a, a.offsetX, a.offsetY);
    } else if (a.findImageFollowUp == 1) {
        s += L" → 移动";
    } else {
        s += L" → 保存匹配度到" + a.matchVarName;
    }
    return s;
}

std::wstring FindImageActionText(const ScriptAction& a) {
    std::wstring s = L"找图 "
        + (a.imageUseVar ? std::wstring(L"变量/路径:") : std::wstring()) + a.imagePath;
    if (!a.searchFullScreen) {
        s += L" 区域" + CoordText(a, a.searchX1, a.searchY1)
            + L"-" + CoordText(a, a.searchX2, a.searchY2);
    }
    if (a.matchThreshold != 65.0 && !a.perfectMatch) {
        wchar_t buf[32];
        swprintf_s(buf, L" 阈值%.0f", a.matchThreshold);
        s += buf;
    }
    if (a.perfectMatch) s += L" 完美匹配";
    if (a.imageScaleMin != 1.0 || a.imageScaleMax != 1.0) {
        s += L" 缩放" + FmtNum(a.imageScaleMin) + L"-" + FmtNum(a.imageScaleMax);
    }
    if (a.findImageFollowUp == 0) {
        s += L" → 点击";
        if (a.offsetX != 0 || a.offsetY != 0)
            s += L" 偏移" + CoordText(a, a.offsetX, a.offsetY);
    } else if (a.findImageFollowUp == 1) {
        s += L" → 移动";
    } else if (a.findImageFollowUp == 2) {
        s += L" → 保存匹配度到" + a.matchVarName;
    } else if (a.findImageFollowUp == 3) {
        s += L" → 保存图片到" + a.matchVarName;
    }
    if (a.findUntilFound) s += L" 循环直到找到";
    else if (a.findTimeExpr != L"0" && !a.findTimeExpr.empty())
        s += L" 限时" + TrimShort(a.findTimeExpr, 16) + L"s";
    if (!a.recordedCapturePath.empty()) s += L"（录制截图）";
    return s;
}

std::wstring DescribeOneAction(const ScriptAction& a) {
    switch (a.type) {
    case ActionType::MoveMouse:
        return L"移动鼠标到 " + CoordOrExpr(a, a.x, a.y)
            + ((a.randomX != 0 || a.randomY != 0)
                ? (L" ±(" + std::to_wstring(a.randomX) + L"," + std::to_wstring(a.randomY) + L")")
                : L"");
    case ActionType::MoveMouseRelative:
        return L"相对移动鼠标 (" + std::to_wstring(a.x) + L", " + std::to_wstring(a.y) + L")"
            + ((a.randomX != 0 || a.randomY != 0)
                ? (L" ±(" + std::to_wstring(a.randomX) + L"," + std::to_wstring(a.randomY) + L")")
                : L"");
    case ActionType::Wait: {
        std::wstring s = L"等待 " + FmtNum(a.duration) + L"s";
        if (a.randomDuration > 0) {
            s += L" +随机" + FmtNum(a.randomDuration) + L"s";
        }
        return s;
    }
    case ActionType::MouseClick:
        return HoldPrefix(a) + ButtonText(a.button) + L"点击 "
            + CoordOrExpr(a, a.x, a.y) + RepeatBrief(a);
    case ActionType::MouseDown:
        return HoldPrefix(a) + ButtonText(a.button) + L"按下 "
            + CoordText(a, a.x, a.y)
            + (a.recordedCapturePath.empty() ? L"" : L"（录制截图）");
    case ActionType::MouseUp:
        return HoldPrefix(a) + ButtonText(a.button) + L"松开 " + CoordText(a, a.x, a.y);
    case ActionType::KeyClick:
        return HoldPrefix(a) + L"按键点击 " + KeyText(a) + RepeatBrief(a);
    case ActionType::KeyDown:
        return HoldPrefix(a) + L"键盘按下 " + KeyText(a);
    case ActionType::KeyUp:
        return HoldPrefix(a) + L"键盘松开 " + KeyText(a);
    case ActionType::HotkeyShortcut:
        return L"快捷按键 " + HoldPrefix(a) + KeyText(a) + RepeatBrief(a);
    case ActionType::Loop:
        if (a.loopFromVar)
            return L"循环 " + TrimShort(a.loopVarExpr, 24) + L" 次"
                + (a.loopVarName.empty() ? L"" : (L"（变量" + a.loopVarName + L"）"));
        if (a.loopCount < 0) return L"无限循环";
        return L"循环 " + std::to_wstring(a.loopCount) + L" 次"
            + (a.loopVarName.empty() ? L"" : (L"（变量" + a.loopVarName + L"）"));
    case ActionType::EndLoop:
        return L"跳出循环";
    case ActionType::DefineBlock:
        return L"定义块 " + (a.blockName.empty() ? L"未命名" : a.blockName);
    case ActionType::RunBlock:
        return L"运行块 " + (a.blockName.empty() ? L"未命名" : a.blockName) + RepeatBrief(a);
    case ActionType::MousePlayback: {
        std::wstring s = L"运行录制回放 " + RunTargetText(a) + RepeatBrief(a);
        if (std::abs(a.playbackSpeed - 1.0) > 1e-6)
            s += L" " + FmtNum(a.playbackSpeed) + L"x";
        return s;
    }
    case ActionType::RunMacro:
        return L"运行宏 " + RunTargetText(a) + RepeatBrief(a);
    case ActionType::QuickInput:
        return L"输入 \"" + TrimShort(a.inputText, 60) + L"\""
            + (a.parseEscapes ? L" 解析转义" : L"")
            + (a.charInterval != 0.01 ? (L" 间隔" + FmtNum(a.charInterval) + L"s") : L"")
            + RepeatBrief(a);
    case ActionType::ScrollWheel:
        return std::wstring(L"滚动滚轮 ")
            + (a.scrollVertical && a.scrollHorizontal ? L"垂直+水平"
                : a.scrollHorizontal ? L"水平" : L"垂直")
            + L" " + std::to_wstring(a.scrollSteps) + L" 步"
            + (a.scrollDirection == 1 ? L"（向下/右）" : L"（向上/左）")
            + RepeatBrief(a);
    case ActionType::FindImage:
        return FindImageActionText(a);
    case ActionType::TextRecognition: {
        std::wstring s = a.ocrRegionByImage
            ? L"OCR 按锚点区域"
            : (a.imagePath.empty() ? L"OCR 识别" : (L"OCR 识别 " + a.imagePath
                + (a.imageUseVar ? L"(变量)" : L"")));
        s += RegionText(a);
        if (!a.ocrSearchText.empty())
            s += L" 查找文字\"" + TrimShort(a.ocrSearchText, 40) + L"\"";
        if (a.ocrDigitsOnly) s += L" 数字模式";
        if (a.ocrFollowUp == 0) s += L" → 点击";
        else if (a.ocrFollowUp == 1) s += L" → 移动";
        else if (a.ocrFollowUp == 2) s += L" → 保存到" + a.matchVarName;
        if (a.findUntilFound) s += L" 循环直到找到";
        if (a.offsetX != 0 || a.offsetY != 0)
            s += L" 偏移" + CoordText(a, a.offsetX, a.offsetY);
        return s;
    }
    case ActionType::If:
        return L"如果 " + TrimShort(a.conditionExpr, 80);
    case ActionType::Else:
        return L"否则";
    case ActionType::Goto:
        return L"跳转到第" + TrimShort(a.gotoStepExpr, 16) + L"步";
    case ActionType::StopMacro:
        return L"结束宏运行";
    case ActionType::LockScreenshot:
        return L"锁定截屏";
    case ActionType::UnlockScreenshot:
        return L"解锁截屏";
    case ActionType::TimerRecordTime:
        return L"计时器记录时间 → "
            + (a.loopVarName.empty() ? L"未命名" : a.loopVarName);
    case ActionType::GetCursorPos:
        return L"获取光标位置 → "
            + (a.matchVarName.empty() ? L"a" : a.matchVarName);
    case ActionType::GetColor:
        return L"获取颜色 " + CoordOrExpr(a, a.x, a.y)
            + L" → " + (a.matchVarName.empty() ? L"colorRet" : a.matchVarName);
    case ActionType::FindColor:
        return FindColorText(a);
    case ActionType::ColorMatch:
        return L"颜色匹配 " + FmtColor(a.colorR, a.colorG, a.colorB)
            + L" 容差" + std::to_wstring(a.colorTolerance)
            + L" @" + CoordOrExpr(a, a.x, a.y)
            + L" → " + (a.matchVarName.empty() ? L"colorRet" : a.matchVarName);
    case ActionType::CustomText:
        return a.customText.empty() ? L"自定义文本" : a.customText;
    case ActionType::AiTextAnalysis:
        return L"AI 文字分析 \"" + TrimShort(a.aiPrompt, 40) + L"\""
            + L" → " + (a.aiOutputVarName.empty() ? L"aiResult" : a.aiOutputVarName)
            + AiDetailText(a);
    case ActionType::AiImageAnalysis:
        return L"AI 图片分析 \"" + TrimShort(a.aiPrompt, 40) + L"\""
            + L" → " + (a.aiOutputVarName.empty() ? L"aiImgResult" : a.aiOutputVarName)
            + AiDetailText(a);
    case ActionType::AiActionExecute:
        return L"AI 执行动作 \"" + TrimShort(a.aiPrompt, 40) + L"\""
            + AiDetailText(a);
    case ActionType::RunProgram:
        return L"运行程序 " + RunTargetText(a)
            + (a.inputText.empty() ? L"" : (L" 参数:\"" + TrimShort(a.inputText, 40) + L"\""));
    case ActionType::CloseProgram:
        return L"关闭程序 " + RunTargetText(a)
            + (a.matchFileNameOnly ? L"（按文件名匹配）" : L"");
    case ActionType::OpenWebpage:
        return L"打开网页 " + RunTargetText(a);
    case ActionType::OpenFile:
        return L"打开文件 " + RunTargetText(a);
    case ActionType::ActivateWindow:
        return L"激活窗口 " + RunTargetText(a);
    default:
        // 保险：全部类型已显式覆盖，兜底与编辑器名称一致
        return ActionName(a);
    }
}

}  // namespace

// 粗略版单动作：只保留动作名 + 类型级语义（按钮/跟随动作/循环方向），
// 不含坐标、时长、阈值、路径等数值参数；意图靠动作名 + 备注理解。
std::wstring QuickDescribeOneAction(const ScriptAction& a) {
    switch (a.type) {
    case ActionType::MoveMouse:
        return std::wstring(L"移动鼠标")
            + (a.moveFromVar ? L"（坐标来自变量）" : L"");
    case ActionType::MoveMouseRelative:
        return L"相对移动鼠标";
    case ActionType::MouseClick:
        return HoldPrefix(a) + ButtonText(a.button) + L"点击"
            + (a.moveFromVar ? L"（坐标来自变量）" : L"");
    case ActionType::MouseDown: return ButtonText(a.button) + L"按下";
    case ActionType::MouseUp: return ButtonText(a.button) + L"松开";
    case ActionType::KeyClick: return HoldPrefix(a) + L"按键点击";
    case ActionType::KeyDown: return HoldPrefix(a) + L"键盘按下";
    case ActionType::KeyUp: return HoldPrefix(a) + L"键盘松开";
    case ActionType::HotkeyShortcut: return HoldPrefix(a) + L"快捷按键";
    case ActionType::FindImage: {
        std::wstring s = std::wstring(L"找图")
            + (a.imageUseVar ? L"（变量图）" : L"");
        if (!a.searchFullScreen) s += L"（限定区域）";
        if (a.findImageFollowUp == 1) s += L"后移动";
        else if (a.findImageFollowUp == 2) s += L"后保存匹配度";
        else if (a.findImageFollowUp == 3) s += L"后保存图片";
        else s += L"后点击";
        if (a.findUntilFound) s += L"（循环直到找到）";
        return s;
    }
    case ActionType::TextRecognition: {
        std::wstring s = std::wstring(L"文字识别")
            + (a.ocrRegionByImage ? L"（按锚点图）" : L"")
            + (a.imageUseVar ? L"（变量图）" : L"")
            + (a.ocrDigitsOnly ? L"（数字模式）" : L"");
        if (a.ocrFollowUp == 1) s += L"后移动";
        else if (a.ocrFollowUp == 2) s += L"后保存结果";
        else s += L"后点击";
        if (a.findUntilFound) s += L"（循环直到找到）";
        return s;
    }
    case ActionType::Loop:
        if (a.loopFromVar)
            return std::wstring(L"循环（按变量")
                + (a.loopVarName.empty() ? L"次数" : a.loopVarName) + L"）";
        return a.loopCount < 0 ? L"无限循环" : L"循环";
    case ActionType::If: return L"条件判断";
    case ActionType::Goto: return L"跳转";
    case ActionType::RunMacro: return L"运行鼠标宏";
    case ActionType::MousePlayback: return L"运行录制回放";
    case ActionType::QuickInput: return L"快捷输入";
    case ActionType::ScrollWheel: return L"滚动滚轮";
    case ActionType::TimerRecordTime: return L"计时器记录时间";
    case ActionType::GetCursorPos: return L"获取光标位置";
    case ActionType::GetColor:
        return std::wstring(L"获取颜色")
            + (a.moveFromVar ? L"（坐标来自变量）" : L"");
    case ActionType::FindColor:
        return std::wstring(L"区域找色")
            + (a.searchFullScreen ? L"" : L"（限定区域）");
    case ActionType::ColorMatch:
        return std::wstring(L"颜色匹配")
            + (a.moveFromVar ? L"（坐标来自变量）" : L"");
    case ActionType::AiTextAnalysis:
        return std::wstring(L"AI文字分析")
            + (a.aiOutputType == 1 ? L"（输出整数）" : L"");
    case ActionType::AiImageAnalysis:
        return std::wstring(L"AI图片分析")
            + (a.aiRegionByImage ? L"（按锚点图）" : L"")
            + (a.aiOutputType == 1 ? L"（输出整数）" : L"");
    case ActionType::AiActionExecute:
        return std::wstring(L"AI动作执行")
            + (a.aiWithImage ? L"（带截图）" : L"")
            + (a.aiLogicConvert ? L"（逻辑转化）" : L"");
    case ActionType::CustomText:
        return a.customText.empty() ? L"自定义文本" : a.customText;
    case ActionType::EndLoop: return L"跳出循环";
    default: return ActionTypeBriefLabel(a.type);
    }
}

static std::wstring DescribeActionsCommon(const std::vector<ScriptAction>& actions,
    size_t startIndex, size_t maxActions, size_t& outShown, bool detailed) {
    outShown = 0;
    if (actions.empty()) return L"（空脚本，无动作）";
    if (maxActions == 0) maxActions = 60;
    if (startIndex >= actions.size()) {
        return L"[提示] startIndex=" + std::to_wstring(startIndex)
            + L" 超出动作总数 " + std::to_wstring(actions.size());
    }
    const size_t end = (std::min)(actions.size(), startIndex + maxActions);
    std::wstring out;
    out += (detailed
            ? std::wstring(L"【动作详细说明")
            : std::wstring(L"【动作概览（备注是意图关键）"))
        + L"（共 " + std::to_wstring(actions.size())
        + L" 步；展示第 " + std::to_wstring(startIndex + 1) + L".." + std::to_wstring(end)
        + L" 步）】\n";
    for (size_t i = startIndex; i < end; ++i) {
        const ScriptAction& a = actions[i];
        const int no = a.originalNo > 0 ? a.originalNo : static_cast<int>(i + 1);
        std::wstring indent;
        if (a.indent > 0) indent.assign(static_cast<size_t>(a.indent) * 2, L' ');
        std::wstring line = L"第" + std::to_wstring(no) + L"步 " + indent
            + (detailed ? DescribeOneAction(a) : QuickDescribeOneAction(a));
        if (!Trim(a.remark).empty()) {
            std::wstring rm = Trim(a.remark);
            for (wchar_t& ch : rm) {
                if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
            }
            line += L"（备注：" + rm + L"）";
        }
        out += line + L"\n";
        ++outShown;
    }
    if (end < actions.size()) {
        out += L"[还有 " + std::to_wstring(actions.size() - end)
            + L" 步未展示；继续请传 startIndex=" + std::to_wstring(end)
            + L"（每页建议 ≤60 步，避免长输入）]\n";
    }
    if (!detailed) {
        out += L"[提示] 本页为概览（动作名+备注）。需要坐标/时长/阈值等具体参数时，"
            L"再传 detail=true 查询对应页。\n";
    }
    return out;
}

std::wstring DescribeScriptActionsBrief(const std::vector<ScriptAction>& actions,
    size_t startIndex, size_t maxActions, size_t& outShown) {
    return DescribeActionsCommon(actions, startIndex, maxActions, outShown, false);
}

std::wstring DescribeScriptActionsDetail(const std::vector<ScriptAction>& actions,
    size_t startIndex, size_t maxActions, size_t& outShown) {
    return DescribeActionsCommon(actions, startIndex, maxActions, outShown, true);
}
