#pragma once
// ──────────────────────────────────────────────────────────────────
// macro_execute_tools.h — AI 动作执行运行时工具
// 常用动作 = 独立工具（quickInput/keyClick/mouseClick…，参数 JSON Schema 强制必填）
// 批量/冷门类型 = submitMacroActions；查阅 = lookupMacroAction；收尾 = completeTask
// ──────────────────────────────────────────────────────────────────

#include "agent_core.h"
#include "page_snapshot.h"

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
    /// 宿主对**当前前台状态**的判断（如「前台是另存为对话框」）——比截图更权威，
    /// 调用方应把它拼进本轮指令：模型看不到窗口类，只能靠这条知道「现在有个模态框挡着」。
    std::wstring foregroundFact;
    /// ★本地 OCR 得到的**屏幕文字坐标索引**（如「自选僵尸卡牌(537,107)；一键全选(2272,255)」）。
    /// 这是「模型看不懂界面」的通用解药：不烧图片 token 就能拿到可点文字的**真实屏幕坐标**，
    /// 于是可以直接 locateAndClick(target=该文字) 或按坐标点，不必靠看图猜位置。
    /// OCR 引擎未安装时为空（整段静默跳过）。
    std::wstring textIndex;
    /// 文字索引条目数（诊断/日志用）
    int textIndexCount = 0;
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
    /// 一次调用定位并点击**多个不同目标**（拿卡→放卡、多个僵尸同时进攻、开局选卡）：
    /// 逐个 VLM 定位 + 立即点击，中间不插观察/验收 —— 省掉每步一次主模型轮次与 settle。
    /// 任一目标定位失败即停（不做「猜着点」）。
    std::function<std::wstring(const std::vector<std::wstring>& targets,
        const std::wstring& button, int clickCount)> onLocateMulti;
    /// 网格定位点击：anchor=已定位过的锚点描述（草坪/卡槽/图标…）；cells=相对锚点的
    /// (row,col) 偏移。锚点第一次会真识图定位，之后走布局记忆；网格由画面周期推断。
    /// 一次调用可点任意多格，**不再逐格识图**。
    std::function<std::wstring(const std::wstring& anchor,
        const std::vector<std::pair<int, int>>& cells,
        const std::wstring& button, int clickCount)> onLocateGrid;
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
    /// 刚抓取过网页后启动程序：弹出确认框。返回 true 才放行。RPA 直接 runProgram 不经过此钩子。
    std::function<bool(const std::wstring& detail)> onConfirmWebLaunch;
    /// 当前 Edge 标签压缩可访问性树（pageKind + ref）。force=true 强制重抓。
    std::function<std::wstring(bool force, const std::wstring& titleHint, const std::wstring& query)>
        onObservePage;
    /// 按 observePage 的 ref 点击（e1）。doubleClick=true 为双击。
    std::function<std::wstring(const std::wstring& ref, bool doubleClick)> onClickRef;
    /// 按 ref 填输入框/下拉框（Playwright browser_type）。submit=true 再回车。
    std::function<std::wstring(const std::wstring& ref, const std::wstring& text, bool clearFirst,
        bool submit)>
        onTypeRef;
    /// 当前标签转到 url（Playwright browser_navigate / browser-use go_to_url），再回传控件树。
    /// query 仅用于导航后过滤快照。
    std::function<std::wstring(const std::wstring& url, const std::wstring& query)> onNavigatePage;
    /// 前台窗口的可交互控件台账（UIA 枚举，纯文本、不烧图）：
    /// "[编号] 类型 \"名字\" @x,y" 形式，供模型先用 listUiControls 看清再按编号触发。
    std::function<std::wstring(int maxCount)> onListUiControls;
    /// 按 (id, name) 触发 UIA 控件：宿主以 name 重新定位、以 id 交叉校验（不一致要写进
    /// 返回文本当警告），有 InvokePattern 直接触发，否则点控件中心。
    std::function<std::wstring(int id, const std::wstring& name, bool observeAfter)>
        onInvokeUiControl;
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

/// 本次 AI 动作里 locateAndClick 的总次数（不论成败；换措辞反复定位的闸门）
int AiLocateAttemptCount();
/// 记一次 locate 调用，返回累计次数
int AiNoteLocateAttempt();

/// 本轮 fetchWebPage 后启动程序须确认（RPA 直接 runProgram 不经过此钩子）
void AiNoteWebFetchUntrusted();
void AiClearWebFetchUntrusted();
bool AiWebFetchUntrustedActive();

/// 最近一次 observePage 的 pageKind（dom|mixed|canvas）
void AiNotePageKind(const std::wstring& kind);
std::wstring AiLastPageKind();

/// 最近一次控件树是否**可信**（与前台标签一致）。
/// 用途：树不可信时必须保留截图走视觉兜底——实测「前台已是历史记录页，树却还是上一个
/// 页面」，而 dom 判定会把截图从规划轮里剥掉，模型于是既看不到新画面、又拿着旧树决策。
void AiNotePageTreeTrusted(bool trusted, const std::wstring& why = L"");
bool AiPageTreeTrusted();
/// 不可信原因（给模型/日志看；可信时为空）
std::wstring AiPageTreeUntrustedReason();
bool AiPageKindIsCanvas();
bool AiPageKindIsDom();

/// 前台窗口是不是浏览器（决定「网页守卫」是否生效：网页控件树拦 Enter、禁止开新标签等）。
/// ★答案取自运行时真实前台窗口 → 自检里必须用下面的覆盖函数固定，否则用例会随
/// 测试机当前前台是不是浏览器而随机红/绿（实测同一二进制连跑 3 次：153/153、148/5、148/5）。
bool AiForegroundIsBrowserWindow();
/// 前台窗口的**类名**是否像浏览器（Chrome_WidgetWin_* / MozillaWindowClass；#32770 对话框不算）。
/// 标题读不出来时的兜底判据：模态对话框标题常为空，只看标题会把 clickRef 打进浏览器。
bool ForegroundWindowIsBrowserClass();
/// 自检用：force>0 强制「是浏览器」，force<0 强制「否」，0=恢复按真实前台
void SetAiForegroundBrowserOverrideForTest(int force);
/// 自检用：指定打开内置页时用的浏览器启动目标（名或路径；须是真实存在的文件）
void SetAiBrowserLaunchTargetOverrideForTest(const std::wstring& target);
/// 自检用：force>0 强制「前台是表格软件」，force<0 强制「否」，0=按真实前台
void SetAiSpreadsheetForegroundOverrideForTest(int force);
/// 已 openWebpage / 切到浏览器 / observe 到 dom|mixed。canvas 会清掉。
void AiNoteWebBrowseSession(bool on);
bool AiWebBrowseSessionActive();
void AiNotePageUrl(const std::wstring& url);
std::wstring AiLastPageUrl();
std::wstring AiLastOpenWebpageUrl();
/// 记住最近一次控件树（clickRef 跟 href 导航、搜索结果允许打开 space UID）
void AiNotePageSnapshot(const PageSnapshot& snap);
std::wstring AiLastSnapshotHrefForRef(const std::wstring& ref);
bool AiLastSnapshotClickPoint(const std::wstring& ref, int& screenX, int& screenY,
    std::wstring& name);
std::wstring AiLastSnapshotRefMatchingTarget(const std::wstring& target);
/// 最近一次 observePage/clickRef/typeRef/navigatePage 的控件树（节点含 inView/矩形）。
/// 浏览器 DOM 优先路径据此选 ref，避免为「点了哪个控件」再走一轮 API。
PageSnapshot AiLastPageSnapshot();
bool AiLastSnapshotContainsUrl(const std::wstring& url);
/// 树上名字含 query 且 href 可导航的用户主页（否则视频）。Playwright getByRole(name) 式。
std::wstring AiQueryMatchingNavigationUrl(const std::wstring& query);

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

// ── 浏览器 DOM 优先操作门禁（配套扩展优化）──────────────────────────
/// 宿主钩子在「按控件树直接操作」前必须先过这道门：命中即不截屏、不上传识图。
/// 判定集中在这里是为了可自检——门禁判错会点到别的窗口，是准确度关键路径。
struct DomFirstActionGateInput {
    bool extensionConnected = false;
    /// 宿主是否注册了 onObservePage + onClickRef/onTypeRef
    bool hasDomHooks = false;
    /// 最近一次观测判定为 canvas（无 DOM 可点）
    bool pageIsCanvas = false;
    bool foregroundLooksBrowser = false;
    /// 前台窗口**类名**像浏览器（Chrome_WidgetWin_* / MozillaWindowClass，#32770 不算）。
    /// 标题读不出来时靠它兜底；模态对话框标题常为空，只看标题会点错窗口。
    bool foregroundBrowserClass = false;
    /// 本次上下文已进入网页会话（dom/mixed）
    bool webSessionActive = false;
    /// 前台标题是否读得出来（仅作诊断；不再作为放行依据）
    bool foregroundTitleReadable = true;
    /// 右键语义不是「点一下控件」，走 DOM 反而可能出上下文菜单
    bool rightButton = false;
};

/// true = 可按控件树直接操作（省整屏截图 + 1~2 轮识图）；
/// outWhy 可选，写入未放行的原因（供诊断日志）。
bool ShouldUseDomFirstAction(const DomFirstActionGateInput& in, std::wstring* outWhy = nullptr);

/// 观察后更新动态干扰指标（busyCoverage / settle 仍在变）
void NoteAiActionUiBusy(double busyCoverageRatio, bool settleStillChanging);
/// 前台动态干扰过大时，locateAndClick 应改走键盘捷径
bool AiActionUiTooBusyForVisionLocate();
/// 前台是「没有控件树的动态画面」（桌面游戏 / 自绘应用 / 模拟器 / 画布页）：
/// 视觉是唯一手段，每轮必须推进入指令教它用 locateAndClick + 键鼠。
bool AiActionGameForegroundLikely();
/// 游戏玩法指针（每任务一次；自带可照抄的操作要点，省一轮 lookup）
std::wstring AiGameNudgeOnce();
/// 选卡/批量选择硬门槛：本次动作是否调用过 planSpend（工具层拦截「一键全选」这类批量选择）
bool AiPlanSpendCalledThisAction();
void AiNotePlanSpendCalled();
void AiResetPlanSpendGate();
/// 「模型明确要了截图」（computer(action=screenshot)）→ 下一次观察**必须**回传帧。
/// 历史里的旧图会被剥成「(历史截图已省略)」，所以「界面未变就省掉这一帧」会把模型变成
/// 瞎子：实测它反复自问「我看不到图」并空转好几轮。这里只标记，由宿主观察时消费。
void NoteAiExplicitScreenshotRequest();
/// 取用并清除「要截图」标记（消费一次）
bool AiTakeExplicitScreenshotRequest();

/// 最近一批动作「点完之后界面到底有没有反应」（settle 的权威判定，不是局部采样）。
/// ★任何「点没中就重试/补点」的逻辑都必须先问这个：
///   实测只看局部颜色+变化区就补点，会在**开关类按钮**上打第二下（开→关），
///   把界面搞成不一致状态，之后点什么都没反应（用户报障「拿下来一张卡就点不动了」）。
void NoteAiUiSettleReacted(bool reacted);
bool AiLastUiSettleReacted();
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

/// 文本是否「像 URL/地址栏输入」（http(s)://、edge://、www.*、裸域名）。
/// 用途：Ctrl+L → quickInput(URL) → Enter 是打开网页的正常手法，
/// 不能和「在站内搜索框里按 Enter」混为一谈。
bool LooksLikeUrlInput(const std::wstring& text);

/// 是否浏览器内置页（edge:// / chrome:// / about: / view-source:）。/// 这些页面扩展无法用 tabs.update 导航，只能走地址栏，且**扩展/页面 DOM 常常看不到**
///（例如 Ctrl+H 打开的是浏览器侧边栏）——所以调用方要对「是否真到了」做核对。
bool IsBrowserInternalUrl(const std::wstring& url);

/// 记下「刚用地址栏打开的内置页」，供下一次 observePage 核对是否真的到了该页。
void AiNoteInternalPagePendingVerify(const std::wstring& url);
/// 取出并清空待核对的内置页（一次性）。
std::wstring AiConsumeInternalPagePendingVerify();

/// 从「<JSON 数组> + 可能的 [提示] 尾巴」里安全取出数组正文（引号内的括号不参与配对）。
/// ★引擎在执行动作批前**必须**用它：构建器会在数组后追加「[提示] 已自动…stopMacro…」，
/// 而提示正文自带 [ ]，用裸 rfind(']') 取结尾会把提示里的 ] 当数组结尾 → 「JSON 解析失败」
/// （runCommand 实测踩到：模型最省事的那条路线直接死掉，只能回去瞎点界面）。
std::wstring ExtractActionJsonArrayText(const std::wstring& text);

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
