#pragma once
// ──────────────────────────────────────────────────────────────────
// macro_execute_tools.h — AI 动作执行运行时工具
// 常用动作 = 独立工具（quickInput/keyClick/mouseClick…，参数 JSON Schema 强制必填）
// 批量/冷门类型 = submitMacroActions；查阅 = lookupMacroAction；收尾 = completeTask
// ──────────────────────────────────────────────────────────────────

#include "agent_core.h"

#include <functional>
#include <string>
#include <vector>

/// 观察截屏结果（可用图片变量本地比对，界面未变则跳过上传）
struct AiObserveCaptureResult {
    bool ok = false;
    /// true：相对基线未明显变化，base64 可为空（勿再向 API 传图）
    bool unchanged = false;
    std::string base64;
    int width = 0;
    int height = 0;
    /// 与基线找图匹配度（百分比）；未比对时为 -1
    double matchScore = -1.0;
    /// 结构差分占比 0~1（已忽略视频等动态区）；未算为 -1
    double changedRatio = -1.0;
    double rawChangedRatio = -1.0;
    double busyCoverageRatio = -1.0;
    /// true：只有动态区在变，控件区稳定 → 宿主应跳过上传
    bool onlyDynamicChanged = false;
    /// 本地 settle / 差分提示（注入 Agent 下一轮）
    bool settleChecked = false;
    bool uiReacted = false;
    bool uiSettled = false;
    bool suggestRefresh = false;
    int settleElapsedMs = 0;
    std::wstring settleHint;
    /// 结构变化区摘要（已滤动态区），如 "10,20,100,80;..."
    std::wstring changeRoisText;
};

/// Agent 观察用图片变量名（findImage followUp=3 / imageUseVar 同源）
inline constexpr const wchar_t* kAiObsImageVarName = L"aiObs";
/// 认定「界面未变」的最低匹配度（%）
inline constexpr double kAiObsUnchangedMatchThreshold = 90.0;

/// 宿主钩子：Agent 闭环时边提交边执行、边回传截图
struct AiActionHostHooks {
    /// 立刻执行已校验的动作 JSON 数组；返回给人看的状态摘要（可含错误）
    std::function<std::wstring(const std::wstring& actionsJson)> onExecuteActions;
    /// 重新截屏观察；成功则写出 jpeg base64 与尺寸（旧接口；优先用 onObserveScreen）
    std::function<bool(std::string& outBase64, int& outW, int& outH)> onCaptureScreen;
    /// 智能观察：forceRefresh=true 强制重截并更新基线；false 时先与 aiObs 图片变量比对
    std::function<AiObserveCaptureResult(bool forceRefresh)> onObserveScreen;
    /// 定位并点击：target 为目标描述；refineLevels 默认 1（识图轮次，最大 2）；
    /// button=left|right；clickCount=2 为双击（打开桌面图标/文件必须双击）
    std::function<std::wstring(const std::wstring& targetDescription, int refineLevels,
        const std::wstring& button, int clickCount)> onLocateAndClick;
    /// Alt+Tab 预览切换：paramsJson 含 action=openPreview|move|confirm|cancel 等
    std::function<std::wstring(const std::wstring& paramsJson)> onSwitchWindow;
    /// 前台窗口里是否有被禁用的「保存/确定」类按钮；有则写出按钮名并返回 true。
    /// 用于输入后立刻发现「内容非法导致提交键灰掉」，而不是等 AI 反复点。
    std::function<bool(std::wstring& disabledButtonName)> onProbeDisabledSubmit;
    /// 本地窗口台账：按 Z 序列出所有可切换窗口（纯文本，不烧图）
    std::function<std::wstring()> onListWindows;
    /// 按标题/进程名子串直接把窗口切到前台（不识图、不 Alt+Tab）
    std::function<std::wstring(const std::wstring& query)> onActivateWindow;
    /// 按进程名（EXCEL.EXE）激活 Z 序最前的该进程窗口；runProgram 后自动置顶用
    std::function<std::wstring(const std::wstring& processName)> onActivateByProcess;
    /// 探测前台系统对话框；返回 "kind=…;title=…;buttons=…;msg=…"，无对话框返回空串
    std::function<std::wstring()> onProbeForegroundDialog;
    /// 查询前台输入法状态文本（中文/英文、是否正在组字）；Agent 每轮注入观测用
    std::function<std::wstring()> onQueryImeStatus;
};

/// 构建工具时的约束（无图禁绝对坐标等）
struct AiActionToolOptions {
    /// false：拒绝靠绝对 x/y 点鼠标（无截图时映射无效）
    bool allowAbsolutePointer = true;
    /// true：动态填表等场景，禁止开程序/开网页/热键碰运气
    bool fillTableOnly = false;
};

/// locate 失败后禁止乱快捷键（白名单外须 confirmBlindKey）
void AiNoteLocateFailed();
void AiClearLocateFailKeyBlock();
bool AiLocateFailKeyBlockActive();
/// 本轮 locate 失败重试计数（成功时清零）
int AiLocateRetryCount();

/// AI动作执行嵌套深度上限（含顶层）：2 = 顶层 + 允许再嵌 1 层（类 browser-use breaker）
constexpr int kMaxAiActionExecuteNestDepth = 2;

/// locateAndClick 短描述上限（字），禁止粘贴整段任务原文烧 vision token
constexpr size_t kMaxLocateAndClickTargetChars = 80;

/// locateAndClick 嵌套深度上限（防定位链互相外包）
constexpr int kMaxLocateAndClickNestDepth = 1;

inline int& LocateAndClickNestDepthRef() {
    thread_local int depth = 0;
    return depth;
}

inline bool LocateAndClickCanEnter() {
    return LocateAndClickNestDepthRef() < kMaxLocateAndClickNestDepth;
}

class LocateAndClickNestGuard {
public:
    LocateAndClickNestGuard() {
        if (LocateAndClickNestDepthRef() >= kMaxLocateAndClickNestDepth) {
            entered_ = false;
            return;
        }
        ++LocateAndClickNestDepthRef();
        entered_ = true;
    }
    ~LocateAndClickNestGuard() {
        if (entered_ && LocateAndClickNestDepthRef() > 0)
            --LocateAndClickNestDepthRef();
    }
    LocateAndClickNestGuard(const LocateAndClickNestGuard&) = delete;
    LocateAndClickNestGuard& operator=(const LocateAndClickNestGuard&) = delete;
    bool entered() const { return entered_; }
private:
    bool entered_ = false;
};

/// 顶层 Agent：未写 goal/todos 前禁止改桌面（自检默认关闭）
void SetAiActionPlanGateEnabled(bool enabled);
bool AiActionPlanGateEnabled();
/// 备忘是否已有 goal 或非空 todos（或本轮已解锁）
bool AiActionPlanGateIsOpen();
/// 执行类工具若被计划门闩拦住，返回 [错误]…；否则空串
std::wstring CheckAiActionPlanGate(const std::wstring& toolName);
/// 是否为「几乎只做定位点击」的嵌套 aiActionExecute（应改 locateAndClick）
bool LooksLikeLocateOnlyOutsourcePrompt(const std::wstring& aiPrompt);

/// 观察后更新动态干扰指标（busyCoverage / settle 仍在变）
void NoteAiActionUiBusy(double busyCoverageRatio, bool settleStillChanging);
/// 前台动态干扰过大时，locateAndClick 应改走键盘捷径
bool AiActionUiTooBusyForVisionLocate();
/// 是否跳过本轮 lookahead（动态干扰大或已预规划过多）
bool AiActionShouldSkipLookahead();
void NoteAiActionLookaheadStarted();

// ── 任务数据缓存（saveTaskData/readTaskData 本地 md 缓存） ──────────
/// 当前任务数据缓存文件路径（长任务「识图一次→缓存→反复读取」）
std::wstring AiTaskDataPath();
/// 写入/追加数据块；replace=false 追加。返回更新后块内容（失败空串）
std::wstring UpsertAiTaskDataBlock(const std::wstring& name, const std::wstring& content,
    bool replace);
/// 读取指定数据块；无则空串
std::wstring ReadAiTaskDataBlock(const std::wstring& name);
/// 整份数据缓存文本（含块名），供 readTaskData 无参读取
std::wstring ReadAiTaskDataAllText();
/// 每轮注入用摘要（每块块名+前两行，控制 token）
std::wstring ReadAiTaskDataSummary();

/// 登记本轮工具批次签名，返回「连续重复轮数」（0=与上轮不同；≥1=连续重复）
int AiNoteToolBatchSignature(const std::wstring& signature);

// ── 输入法（IME） ──────────────────────────────────────────────────
/// 是否前台线程键盘布局是中文（PRIMARYLANGID == LANG_CHINESE）
bool ForegroundImeIsChineseLayout();
/// 把前台窗口 IME 切到中文(chineseMode=true)/英文(字母数字)；失败返回 false
bool SetForegroundImeMode(bool chineseMode);
/// 强制取消前台输入法的挂起组字（CPS_CANCEL，不向应用发 Escape）。
/// 组字状态下发 KEYEVENTF_UNICODE 会被拼进组字串（如「1」→「h1」），必须先清。
void CancelPendingImeComposition();
/// ASCII 单键 keyClick 前先清组字再强制英文 IME（防拼音吃掉 y/数字等加速键）
void ForceEnglishImeBeforeAsciiKey();
/// quickInput 前准备：清挂起组字并强制英文 IME（防拼音残留拼进 Unicode 文本）。
void PrepareImeForTextInput(const std::wstring& text);
/// 查询前台输入法状态文本：中文/英文、是否正在组字（GCS_COMPSTR）及组字内容；
/// 无前台窗口/无法读取返回空串。截图已可拍到候选框（CAPTUREBLT），此文本用于
/// 「正在组字」时精确告知 Agent 组字内容（候选框小字在观察帧里仍难读）。
std::wstring QueryForegroundImeStatusText();
/// 把输入法状态文字绘制到位图左上角，观察帧编码前调用。
/// 为什么画在图上：TSF/DirectComposition 输入法的候选窗/组字窗不是普通分层窗口，
/// BitBlt+CAPTUREBLT 仍拍不到；画到图上等于把 IME 状态烙进截图，Agent 看图即知。
/// 位图被缩放成 768 长边观察帧后依然可读（字号按位图高度比例）。
void DrawImeStatusOverlay(HBITMAP hbm, const std::wstring& text);

inline int& AiActionExecuteNestDepthRef() {
    thread_local int depth = 0;
    return depth;
}

inline int AiActionExecuteNestDepth() {
    return AiActionExecuteNestDepthRef();
}

inline bool AiActionExecuteCanNestMore() {
    return AiActionExecuteNestDepthRef() < kMaxAiActionExecuteNestDepth;
}

/// 进入一层 AI 动作执行；entered()==false 表示已达上限
class AiActionExecuteNestGuard {
public:
    AiActionExecuteNestGuard() {
        if (AiActionExecuteNestDepthRef() >= kMaxAiActionExecuteNestDepth) {
            entered_ = false;
            return;
        }
        ++AiActionExecuteNestDepthRef();
        entered_ = true;
    }
    ~AiActionExecuteNestGuard() {
        if (entered_ && AiActionExecuteNestDepthRef() > 0)
            --AiActionExecuteNestDepthRef();
    }
    AiActionExecuteNestGuard(const AiActionExecuteNestGuard&) = delete;
    AiActionExecuteNestGuard& operator=(const AiActionExecuteNestGuard&) = delete;
    bool entered() const { return entered_; }
private:
    bool entered_ = false;
};

/// 新一轮 AI 动作执行开始时重置会话态（openWebpage 去重、任务备忘、任务数据缓存）
void ResetAiActionSessionState(unsigned long long runId = 0);

/// quickInput 是否应在输入前清框（Ctrl+A + Delete）。
/// 表格软件前台默认跳过（Ctrl+A=全选整表）；另存为/打开文件对话框例外（焦点在文件名框）。
/// 清框用 Delete 而非只靠选区：壳地址栏对 Unicode 常追加不替换。
bool AiQuickInputNeedsCtrlAClear(bool clearFirst, bool spreadsheetForeground,
    const std::wstring& dialogKind);
/// 读取当前任务短备忘（供注入 Agent 轮询；可为空；固定章节排序）
std::wstring ReadAiTaskMemoText();
/// 追加一行短备忘（宿主/工具共用；自动截断）。无章节前缀时写入 [notes]
void AppendAiTaskMemoLine(const std::wstring& line);
/// 写入/更新固定章节：goal|facts|todos|windows|recipe|notes
/// goal/windows/recipe 默认整节替换；facts/todos/notes 默认追加
bool UpsertAiTaskMemoSection(const std::wstring& section, const std::wstring& body,
    bool replaceSection = false);
/// 入历史时按工具名的结果字符预算（Claude Code 式分层省 token）
size_t AiToolResultBudgetChars(const std::wstring& toolName);

/// 构建并校验单个宏动作，返回纯 JSON 数组（执行格式）；失败返回 [错误] 前缀文本
std::wstring BuildAndValidateMacroActionJson(const nlohmann::json& params);

/// 是否为宏动作执行相关工具名（原子动作 / submit / lookup / complete）
bool IsMacroExecutionToolName(const std::wstring& name);

/// 是否为会立刻执行桌面动作的工具（原子动作或 submitMacroActions）
bool IsMacroActionRunToolName(const std::wstring& name);

/// API 运行时工具：原子动作工具 + submitMacroActions + lookup + completeTask
/// hooks 非空且提供 onExecuteActions 时，执行类工具会立刻跑动作并返回结果
std::vector<AgentTool> BuildAiActionExecuteTools(
    AiActionHostHooks* hooks = nullptr,
    AiActionToolOptions opts = {});

/// 与 BuildAiActionExecuteTools 相同（保留扩展入口）
std::vector<AgentTool> BuildAiActionExecuteToolsFull(
    AiActionHostHooks* hooks = nullptr,
    AiActionToolOptions opts = {});

/// 工具结果是否表示任务完成
bool IsCompleteTaskToolResult(const std::wstring& result);

/// 最近一次执行类工具是否要求回传截图验收（observeAfter）
bool IsSubmitObserveToolResult(const std::wstring& result);
bool IsSubmitSkipObserveToolResult(const std::wstring& result);
/// Midscene 式：灰钮/无反应/定位失败等不确定结果 → 必须截屏再规划
bool ToolResultNeedsForceObserve(const std::wstring& result);
bool HistoryNeedsForceObserve(const std::vector<ChatMessage>& history, size_t afterIdx);

/// 从最近一轮 tool 消息判断是否调用了 completeTask / 是否执行了动作
bool HistoryHasCompleteTask(const std::vector<ChatMessage>& history, size_t afterIdx);
/// 从本轮历史提取 completeTask 的 reason（无则空）
std::wstring ExtractCompleteTaskReason(const std::vector<ChatMessage>& history, size_t afterIdx);
bool HistoryHasSubmitMacroActions(const std::vector<ChatMessage>& history, size_t afterIdx);
/// 本轮是否有要求观察的 submit（用于结束 tool-loop 并截屏）
bool HistoryHasSubmitRequestingObserve(const std::vector<ChatMessage>& history, size_t afterIdx);
