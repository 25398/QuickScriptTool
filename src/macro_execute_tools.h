#pragma once
// ──────────────────────────────────────────────────────────────────
// macro_execute_tools.h — AI 动作执行运行时工具
// 常用动作 = 独立工具（quickInput/keyClick/mouseClick…，参数 JSON Schema 强制必填）
// 批量/冷门类型 = submitMacroActions；查阅 = lookupMacroAction；收尾 = completeTask
// ──────────────────────────────────────────────────────────────────

#include "agent_core.h"
#include "ai_decide.h"
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
    /// 屏幕文字索引条目数（诊断/日志用）
    int textIndexCount = 0;
    /// ★统一「可点元素索引」（见 ai_locate_verify.h 的 BuildAiElementIndex）：
    /// 把 UIA 控件与 OCR 文字行合并成**一张带编号的表**，让模型做到「所见即所得」——
    /// 用编号/名字说话，宿主直接按索引解析坐标（0 次 VLM），而不是「看图 → 描述 → 再识图定位」。
    /// 这是「找图-定位-点击」三件事合并的入口：`locateAndClick(target)` 命中索引就不再烧识图。
    std::wstring elementIndex;
    /// 索引条目数（诊断用）
    int elementIndexCount = 0;
    /// 索引里的 UIA 控件条数（诊断用：为 0 说明这台机器/这个程序没有 UIA 树）
    int elementIndexUiCount = 0;
    /// 索引里「标签→图标槽」**推断**条目数（>0 说明这一帧发布了版面推断；
    /// 为 0 时模型只有文字坐标 —— 那正是「知道价格在哪、不知道卡片在哪」的成因）
    int elementIndexIconCount = 0;
};

/// Agent 观察用图片变量名（findImage followUp=3 / imageUseVar 同源）
inline constexpr const wchar_t* kAiObsImageVarName = L"aiObs";
/// 认定「界面未变」的最低匹配度（%）
inline constexpr double kAiObsUnchangedMatchThreshold = 90.0;

/// ── `zoom` 的纯判据（可逐格自检）──────────────────────────────────────
/// 请求区域（**upload 截图像素**）夹取到当前帧内；太小 / 反向 / 完全在帧外一律拒绝并给可读原因。
/// ⚠ 纯函数：不碰宿主状态，自检能直接钉边界格（本项目要求判断表穷尽、可逐格断言）。
struct AiZoomRect {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int width() const { return x2 - x1; }
    int height() const { return y2 - y1; }
};

/// `zoom` 交给模型的**一张图**：区域 + 交付尺寸 + 落盘字节数（三个数字都必须如实）。
struct AiZoomTile {
    std::wstring imagePath;   ///< 落盘文件（工具拼成 `[[AGENT_IMG:路径]]`）
    AiZoomRect area;          ///< 这张图覆盖的 upload 区域
    int outWidth = 0;         ///< **真正落盘那张**的尺寸（不是裁剪尺寸、不是请求尺寸）
    int outHeight = 0;
    size_t bytes = 0;         ///< 真正落盘那张的字节数（= 模型拿到的那张）
};

/// 宿主钩子：Agent 闭环时边提交边执行、边回传截图
/// `zoom` 的结果（宿主填）：成功时给交付图 + 三个**必须如实报**的数字。
struct AiZoomResult {
    bool ok = false;
    /// 交付给模型的图（1 张；区域过大时**分块**成 2 张 —— 绝不静默裁掉模型要的部分）
    std::vector<AiZoomTile> tiles;
    /// 失败：如实原因（越界 / 太小 / 宿主没提供放大能力 / 文字标签没解析出来…）
    std::wstring error;
    /// 模型**请求**的区域（回执要把「你要的」和「给你的」摆在一起，不能只说后者）
    AiZoomRect requested{};
    bool byTarget = false; ///< 这次走的是**文字标签**入口还是坐标入口（回执要写明）
    std::wstring resolvedName;  ///< 文字标签入口时解析到的条目名
    /// 交付方式上的**如实说明**：分块了 / 因为上限只能整块降采样（放大倍数已丢失）…
    std::wstring note;
};

struct AiActionHostHooks {
    /// 立刻执行已校验的动作 JSON 数组；返回给人看的状态摘要（可含错误）
    std::function<std::wstring(const std::wstring& actionsJson)> onExecuteActions;
    /// 重新截屏观察；成功则写出 jpeg base64 与尺寸（旧接口；优先用 onObserveScreen）
    std::function<bool(std::string& outBase64, int& outW, int& outH)> onCaptureScreen;
    /// 智能观察：forceRefresh=true 强制重截并更新基线；false 时先与 aiObs 图片变量比对
    std::function<AiObserveCaptureResult(bool forceRefresh)> onObserveScreen;
    /// 定位并点击：target 为目标描述；refineLevels 默认 1（识图轮次，最大 2）；
    /// button=left|right；clickCount=2 为双击（打开桌面图标/文件必须双击）。
    /// ★`elementId` > 0 时表示模型直接给了**本帧元素索引的编号**（`FormatAiElementIndex`
    ///   每行开头的 `[N]`）—— 宿主按编号直查该表拿坐标，比按名字解析更确定（同屏同名不会歧义）。
    ///   编号**只在本帧有效**；查不到就如实回执（不得退化成「按名字猜」）。
    std::function<std::wstring(const std::wstring& targetDescription, int refineLevels,
        const std::wstring& button, int clickCount, int elementId)> onLocateAndClick;
    /// 一次调用定位并点击**多个不同目标**（拿卡→放卡、多个僵尸同时进攻、开局选卡）：
    /// 逐个 VLM 定位 + 立即点击，中间不插观察/验收 —— 省掉每步一次主模型轮次与 settle。
    /// 任一目标定位失败即停（不做「猜着点」）。
    std::function<std::wstring(const std::vector<std::wstring>& targets,
        const std::wstring& button, int clickCount)> onLocateMulti;
    /// Alt+Tab 预览切换：paramsJson 含 action=openPreview|move|confirm|cancel 等
    std::function<std::wstring(const std::wstring& paramsJson)> onSwitchWindow;
    /// 前台窗口里是否有被禁用的「保存/确定」类按钮；有则写出按钮名并返回 true。
    /// 用于输入后立刻发现「内容非法导致提交键灰掉」，而不是等 AI 反复点。
    std::function<bool(std::wstring& disabledButtonName)> onProbeDisabledSubmit;
    /// 本地窗口台账：按 Z 序列出所有可切换窗口（纯文本，不烧图）
    std::function<std::wstring()> onListWindows;
    /// 按标题/进程名子串、**或按进程号**把窗口切到前台（不识图、不 Alt+Tab）。
    /// ★pid != 0 时按进程号精确锁定（台账里的 `pid=…`）—— 双开同名窗口时**唯一可靠**的手段；
    ///   query 可为空（空 = 该进程的全部窗口）。两者同时给 = 取交集。
    ///   ⚠ 产品必须收 pid：`FormatWindowList` 的台账明确叫模型「按 pid/客户区区分，别只按标题选」，
    ///     若不收，模型照做反被打回「缺少 match」⇒ 在「match 命中 2 个 / pid 不被接受」之间绕圈。
    std::function<std::wstring(const std::wstring& query, unsigned long pid)> onActivateWindow;
    /// 按进程名（EXCEL.EXE）激活 Z 序最前的该进程窗口；runProgram 后自动置顶用
    std::function<std::wstring(const std::wstring& processName)> onActivateByProcess;
    /// 探测前台系统对话框；返回 "kind=…;title=…;buttons=…;msg=…"，无对话框返回空串
    std::function<std::wstring()> onProbeForegroundDialog;
    /// 查询前台输入法状态文本（中文/英文、是否正在组字）；Agent 每轮注入观测用
    std::function<std::wstring()> onQueryImeStatus;
    /// 刚抓取过网页后启动程序：弹出确认框。返回 true 才放行。RPA 直接 runProgram 不经过此钩子。
    std::function<bool(const std::wstring& detail)> onConfirmWebLaunch;
    /// 当前 Edge 标签压缩可访问性树（pageKind + ref）。force=true 强制重抓。
    /// offset = 分页起点（0 起）。长列表页（作业/题库/搜索结果）一屏放不下时，
    /// 宿主回执里会给出 nextOffset，模型据此再抓下一段 —— 比「反复滚动 + 截图」可靠。
    std::function<std::wstring(bool force, const std::wstring& titleHint,
        const std::wstring& query, int offset)> onObservePage;
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
    /// `typeFilter` 非空时只列类型名包含它的控件（如「列表项」）——把滚动的长列表
    /// 从几十个按钮里捞出来，**过滤在宿主侧做，不进上下文**。
    /// `nameFilter` 非空时只列名字包含它的控件（大小写不敏感）。
    std::function<std::wstring(int maxCount, const std::wstring& typeFilter,
        const std::wstring& nameFilter)> onListUiControls;
    /// 按 (id, name) 触发 UIA 控件：宿主以 name 重新定位、以 id 交叉校验（不一致要写进
    /// 返回文本当警告），有 InvokePattern 直接触发，否则点控件中心。
    std::function<std::wstring(int id, const std::wstring& name, bool observeAfter)>
        onInvokeUiControl;
    /// ★`zoom`：把当前画面的**一块区域按原始分辨率放大**回传给模型（看不清小字/图标时用）。
    /// 入参是 **upload 截图像素**（与元素索引/文字索引同一套）——模型可直接抄索引坐标，零换算。
    /// `target` 非空时由宿主用**本帧元素索引**解析区域（与 locateAndClick 查表是同一张表），
    /// 此时忽略坐标（回执会写明这次走的哪条入口）；两者都空 ⇒ 如实报错，别猜。
    /// `maxEdge` = 回传图长边上限（宿主可按上限**收窄**区域并在回执里说明）。
    std::function<AiZoomResult(int x1, int y1, int x2, int y2, const std::wstring& target,
        int maxEdge)> onZoomRegion;
};

/// 构建工具时的约束（无图禁绝对坐标等）
struct AiActionToolOptions {
    /// false：拒绝靠绝对 x/y 点鼠标（无截图时映射无效）
    bool allowAbsolutePointer = true;
};

/// 记一次 locate 失败（累加 `AiLocateRetryCount()`；成功路径会清零）。
void AiNoteLocateMiss();
/// 成功动作后清零（含 locate 失败计数）
void AiResetLocateMiss();
/// 本轮 locate 失败计数（成功时清零）。**只用于如实回执**
/// （completeTask 的「别宣称已完成」守卫 + 每轮提示里报「已定位 N 次」）；
/// ⚠ 它**不再**用来拦截「同一目标再试」—— 引擎不做那个决策。
int AiLocateRetryCount();

/// 本次 AI 动作里 locateAndClick 的总次数（**单动作资源预算**，同工具轮上限；
/// 与「这个目标该不该再试」无关）
int AiLocateAttemptCount();
/// 记一次 locate 调用，返回累计次数
int AiNoteLocateAttempt();

/// ── `zoom` 的区域夹取 / 分块 / 回执（纯判据，可逐格自检）─────────────────
/// 请求区域（**upload 截图像素**）夹取到当前帧内；太小 / 反向 / 完全在帧外一律拒绝并给可读原因。
/// ⚠ 纯函数：不碰宿主状态，自检能直接钉边界格（本项目要求判断表穷尽、可逐格断言）。
bool AiZoomClampRect(int x1, int y1, int x2, int y2, int frameW, int frameH,
    int minSide, AiZoomRect& out, std::wstring& why);

/// 把请求区域切成**覆盖它的全部**的 ≤`maxTiles` 块，每块在**原生分辨率**下长边 ≤ `maxEdge`。
///
/// ★★为什么要有它（实测事故）：旧实现遇到超上限的区域会**居中收窄**（两边切掉）——
///   模型 `zoom(0,0,1024,80)`（整条卡槽）拿到的是中间 512 宽那一段；它对照回执发现
///   区域不是自己要的，却已经据此下了结论「卡槽是空的」，白烧两轮。
///   **引擎不该替模型决定「你其实只想看中间」**：要么给全（分块），要么给全（降采样）
///   并说清代价，**绝不静默丢弃**。
///
/// 返回值：
///   · 非空 = 按这个列表分块交付（每块都是原生分辨率 ⇒ 保值放大）；
///   · 空   = 该区域在原分辨率下需要 > `maxTiles` 块 ⇒ 调用方改用「整块降采样」，
///            并把 `needTiles` 如实写进回执（让模型自己决定要不要改小区域）。
std::vector<AiZoomRect> PlanAiZoomTiles(const AiZoomRect& area, double nativePerUpX,
    double nativePerUpY, int maxEdge, int maxTiles, int* needTiles = nullptr);

/// `zoom` 的如实回执（纯文本组装 ⇒ 自检直接断言文案）。
/// ⚠ 必须写清四件事：**你要的区域**、**给你的每张图各覆盖哪一块**、**每张的倍率 N**、
///   「**图内某点的 upload 坐标 = 该块 x1 + 图x/N**」——
///   少一句就又是一次坐标口径漂移（本仓刚因为三套坐标混说烧掉过几千 token）。
/// ⚠ 而且**倍率/字节数必须按「模型真正收到的那张图」算**：交付图由宿主一次编成
///   送图链路的形态（JPEG q82 + 长边 ≤ `kAgentAttachmentMaxLongEdge`），
///   三个数字直接引用宿主给的真值 ⇒ 回执不可能说谎。
std::wstring FormatAiZoomReceipt(const AiZoomResult& delivery);

/// `zoom` 放大图的落地路径：`<dir>\zoom_<pid>_<seq>.png`。
/// ★★**每次调用必须给不同的 seq**（实测事故）：同一轮里模型可以调两次 zoom，而
/// `[[AGENT_IMG:...]]` 是**整批工具跑完之后**才统一读盘编码的 ⇒ 用固定文件名时
/// 第二张会在第一张被读之前把它覆盖掉，模型收到**两张一模一样的图**，于是判定
/// 「zoom 只会返回最后一张」并**弃用了这个工具**（回去靠 1024 宽的整帧猜卡价）。
std::wstring FormatAiZoomTempPath(const std::wstring& dir, unsigned long pid, unsigned seq);

/// 清掉 zoom 落地目录里**早就没用了**的旧图（默认 10 分钟前）。
/// 名字唯一 ⇒ 文件会累积；而放大图的寿命只到「下一轮请求把它编码进历史」为止，
/// 之后只剩诊断价值 ⇒ 按年龄清，目录大小自然有界。
/// ⚠ 只删 `zoom_*.png`，且**未来时间戳/时间倒退一律不删**（算不出年龄时宁可不删 ——
///   删错的代价是模型拿到一张不存在的图）。
void PruneAiZoomTempDir(const std::wstring& dir,
    unsigned long long olderThanMs = 10ULL * 60ULL * 1000ULL);

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

/// ★「这个窗口类是不是浏览器」——一份事实、两处消费（前台判定 + 找浏览器窗口）。
///   类名表只在 macro_execute_tools.cpp 里写一份（同一事实两处写必然漂移）。
bool WindowClassIsBrowserClass(HWND hwnd);
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
/// 记录"我们刚打开过哪个网页"（宏动作回放侧也要记，否则 searchOnPage 不知道当前站点）。
void AiNoteLastOpenWebpageUrl(const std::wstring& url);
/// 记住最近一次控件树（clickRef 跟 href 导航用）
void AiNotePageSnapshot(const PageSnapshot& snap);
bool AiLastSnapshotClickPoint(const std::wstring& ref, int& screenX, int& screenY,
    std::wstring& name);
std::wstring AiLastSnapshotRefMatchingTarget(const std::wstring& target);
/// 最近一次 observePage/clickRef/typeRef/navigatePage 的控件树（节点含 inView/矩形）。
/// 浏览器 DOM 优先路径据此选 ref，避免为「点了哪个控件」再走一轮 API。
PageSnapshot AiLastPageSnapshot();
/// 树上名字含 query 且 href 可导航的第一个结果（搜索结果直达）。纯名字匹配 ——
/// 原先按站点形状（space.bilibili.com / `/video/`）排序，那部分已删（批 D，docs §47）。
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
/// 把当前线程局部的桌面事实（页型/前台类名/覆盖率…）装成判断表输入。
/// 导出是为了让自检能断言「旧布尔封装 == 判断表判决」（重构不许改行为）。
AiDecideSignals CollectAiDecideSignals();
/// 前台动态干扰过大时，locateAndClick 应改走键盘捷径。
/// `outDecision` 可选：写入本次判断的理由与置信度（第 2 刀按它决定加不加提示）。
/// ⚠ 它现在**只做如实留痕**（`NoteAiDecisionLog` 一行「vision_gate 判决 + 输入信号」）：
///   原先拿它拦识图定位、并按置信度往对话里塞提示的两处消费点**已删（批 D，docs §47）**
///   —— 引擎不决定模型该用哪种感知手段。
bool AiActionUiTooBusyForVisionLocate(AiDecisionRecord* outDecision = nullptr);
/// 前台是「没有控件树的动态画面」（桌面游戏 / 自绘应用 / 模拟器 / 画布页）。
/// ⚠ 只用来做**感知分档**（settle 节拍、视觉闸分流、判断日志）——
///   曾经它还会让引擎每轮往指令里注入「用视觉推进」的硬命令，那条注入**已撤销（批 C）**：
///   建议的归属地是 `lookupMacroAction(section=game)`，由模型自己去查。
bool AiActionGameForegroundLikely();
/// 最近一次游戏前台判断（判决 + 依据）。给**有延迟**的调用方用：
/// 工具循环跑完后状态会变，settle 节拍选择离判断点远，所以单独记一份。
AiGameForegroundDecision AiLastGameForegroundDecision();
/// 归一 `lookupMacroAction` 的查询词：剥掉 `section=` / `type:` 这类键前缀、
/// 包裹引号与空白，得到下游比较用的**裸值**。
/// 实测：模型照 Skill 写 `lookupMacroAction(section=game)` → 下游按裸值比较 →
/// 落进兜底「未找到」，白烧一轮（还会再试一次）。导出给自检钉死。
std::wstring NormalizeMacroLookupQuery(const std::wstring& raw);

/// 本次执行最终要不要回传观察帧（三档合取）：
///   ① 模型明确要求 → 看；
///   ② 结果不确定（settle 无反应 / 灰钮 / UIA 事实）→ 看；
///   ③ **本任务第一次动手** → 看（防「抢跑」：此时模型可能一个像素都没看过，
///      只能靠猜，等看到画面可能已经白点了一两步。收益最高、代价一帧）。
/// 导出给自检：判错会表现成「对着过期画面继续动手」或「每步白等一帧」。
bool AiDecideNeedObserveAfterExec(bool requestedObserve, bool firstActionOfTask,
    const std::wstring& execMsg, const std::wstring& summary);


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

/// ★工具参数名 → **动作字段名** 的对齐（docs §70）。
///
/// 「计划执行」那行预览是把**工具参数**当成**动作 JSON** 去描述的（两者恰好同名同形才成立）。
/// `mouseDrag` 恰好不成立：工具参数是 `fromX/fromY/toX/toY`，动作字段是 `x/y/endX/endY`
/// ⇒ 硬套就打印出 `· 鼠标拖拽左键 (0,0)→(0,0) 0.300秒`（实测真机日志里就有这一行，
/// 而同一批**真的**从 (228,47) 拖到了 (760,300)）—— **回执对不上事实**。
///
/// 返回 `true` = 这份参数已经可以按动作描述；返回 `false` = 参数里**有坐标类键名**
/// 但一个都对不上动作字段（eg. 将来新增的工具换了名字）⇒ 调用方**必须回退印原始参数**，
/// **不许**凭空编造 `(0,0)`：
/// 「宁可少说一句，也不许说一句错的」。
bool AlignToolParamsWithActionFields(const std::wstring& toolName, nlohmann::json& args);

/// API 运行时工具：原子动作工具 + submitMacroActions + lookup + completeTask
/// hooks 非空且提供 onExecuteActions 时，执行类工具会立刻跑动作并返回结果
std::vector<AgentTool> BuildAiActionExecuteTools(
    AiActionHostHooks* hooks = nullptr,
    AiActionToolOptions opts = {});

/// 与 BuildAiActionExecuteTools 相同（保留扩展入口）
std::vector<AgentTool> BuildAiActionExecuteToolsFull(
    AiActionHostHooks* hooks = nullptr,
    AiActionToolOptions opts = {});

/// 读取本地办公文档正文（xlsx/docx/pptx/pdf/csv/txt）的工具 —— 供 **AI 脚本助手**注册。
///
/// 为什么把它单独暴露出来：它原本只注册给**宏侧 AI 动作执行**（`BuildAiActionExecuteTools`），
/// 于是「AI 脚本助手」这个聊天助手**读不了办公文件** —— 用户丢一个 xlsx 进来，
/// 助手只能建议用户自己看，或者让用户去开宏。工具本身早就写好了，缺的只是注册。
///
/// ⚠ 名字必须与内部的 `MakeReadDocumentTool` **不同**：后者定义在匿名命名空间里
/// （内部链接）。在头文件里声明同名函数会造出两个重载，调用点直接 C2668「调用不明确」。
/// 所以对外只用这个名字，内部实现一个字都不动。
AgentTool MakeAgentReadDocumentTool();

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
