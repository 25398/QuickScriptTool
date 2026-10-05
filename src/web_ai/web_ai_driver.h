#pragma once
// ──────────────────────────────────────────────────────────────────
// web_ai_driver.h — 网页版 AI 的**驱动层**：经扩展桥驱动用户浏览器里的网页 AI
//
// 对标 chen-squared/browser-ai-bridge 的 `browser/provider-client.ts`（DOM 交互那层）。
// 我们不需要 Playwright：扩展已经用 `chrome.debugger`(CDP) 打通了「真实按键输入 +
// 注入脚本读页面」，本层只负责**按正确顺序问对问题**。
//
// ⚠⚠ 三条硬约束（每条都有前科）：
//
//  ① **只能从本进程调**：`ExtBridgeServer::Request()` 只把请求发给**连在本进程监听器上**
//     的扩展。想从外部进程（Python 探针 / 另起的第二实例）驱动扩展是**不成立**的 ——
//     桥的 `HandleClient` 只把入站消息当 `result`/`ready` 回执（`ext_bridge_server.cpp:946`），
//     **从不把外部消息转给扩展**。2026-09-24 的 `web_ai_live_probe.py` 就是踩了这个坑：
//     它在 `[0.5]` 之后的每一步都在白等超时（每步 30s），看起来像"卡住"。
//
//  ② **必须串行**：桥用**全局唯一**的 `waitingId_` 等回执（`ext_bridge_server.cpp:1037`）
//     ⇒ 两条并发的 `Request` 会互相吞掉对方的回执（表现为"莫名超时"）。
//     本层用一把静态互斥锁串行化全部调用。
//
//  ③ **必须显式给大超时**：桥的 `Request` 默认 8s，而网页一轮回答要 5~60s。
//     超时不代表失败，但会把在途回答丢掉 —— 所以每个动作单独给超时，见各函数默认值。
//
// ⚠ 网页 AI 是**有状态**的（对话留在页面上），所以本层是「写进去 → 等新回答」，
//   而不是无状态的一次调用；「这一轮该写什么」由 `web_ai_prompt.h` 的 `PlanTurn` 决定。
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace quickscript::webai {

/// 站点信息（来自扩展的 `webAiProviders`，配置在 extension/edge/web_ai_providers.json）
struct WebAiProviderInfo {
    std::string id;
    std::string label;
    std::string url;
    std::string urlHint;
    std::string inputKind;
};

/// 浏览器标签页（来自扩展的 `listPages`）
struct WebAiPageInfo {
    int tabId = 0;
    std::string title;
    std::string url;
};

/// 一次「读回答」的结果
struct WebAiReplyInfo {
    bool ok = false;
    std::string text;
    std::string error;    ///< 扩展侧错误码（NO_RESPONSE_NODE / IDLE_NO_CONTENT / ...）
    std::string message;
    std::string provider;
    std::string matchedBy;
    int elapsedMs = 0;
    int blocks = 0;       ///< 页面上助手消息块数量（判断"有没有新回答"的关键）
    int textLength = 0;
    bool truncated = false;
    /// ★ 页面**自己在忙**（provider 的 busySelectors 命中，如「已处理 N 秒/正在生成」）。
    ///   用来区分"我们没读到新回答"与"页面跑着它自己的任务"（2026-09-30）。
    bool pageBusy = false;
};

/// ★ 设置**本线程**的会话 key —— 它会随每个 webAi* 请求带给扩展。
///
/// 为什么用 thread_local：AI 助手的每个会话在自己的线程里发请求，
/// 而 `WebAiDriver` 是单例 ⇒ 放成员会被并发会话互相覆盖。
///
/// 用途：扩展按它**给每个会话分配自己的浏览器标签页**。
/// ⚠ 不区分会话时，两个 UI 会话会共用同一个网页对话 ⇒ 用户看到
/// "一个对话在回复后被打到了另一个网页端对话"（2026-09-26 真机）。
void SetCurrentSessionKey(const std::string& keyUtf8);
std::string CurrentSessionKey();

/// 一次「自动挑站点」的结果（`web` 模型走这条）
struct WebAiPickResult {
    bool ok = false;
    std::string provider;   ///< 选中的站点 id（doubao / deepseek / yuanbao / qwen）
    int tabId = 0;
    std::string reason;     ///< `open` = 本来开着；`login` = 探测到可用（≈ 已登录）
    std::string error;      ///< NO_WEB_AI_LOGIN / NO_PROVIDER / ...
    std::string message;
    nlohmann::json tried = nlohmann::json::array();  ///< 逐个探测的明细（诊断用）
};

/// 「写进去 + 提交 + 等新回答」的完整结果
struct WebAiSendOutcome {
    bool ok = false;
    std::string error;    ///< NO_EXTENSION / NO_PROVIDER / NO_TAB / WRITE_FAILED /
                          ///< SUBMIT_FAILED / WAIT_TIMEOUT / EMPTY_REPLY / ...
    std::string message;
    WebAiReplyInfo reply;
    int baselineBlocks = 0;   ///< 发送前的助手消息块数
    std::string baselineText; ///< 发送前页面上的最后一条回答（用于识别"读到的还是旧回答"）
    int attachMs = 0;
    int writeMs = 0;
    int submitMs = 0;
    int waitMs = 0;
    /// ★ 这次发送期间**新建了专属标签页**（原来那个丢了/被关）⇒ 网页对话是空的，
    ///   本轮写进去的是增量 ⇒ 上下文已丢。后端据此把会话状态清零，下一轮全量重发重建。
    bool tabRecreated = false;
};

class WebAiDriver {
public:
    static WebAiDriver& Instance();

    /// 桥在跑且扩展在线？（不比 `/qst/status` 更多信息，但省一次 HTTP 往返）
    bool ExtensionOnline();

    /// 列出扩展认得的站点
    /// @param extVersionOut 回执里的 `version`（**证明浏览器里跑的是哪一版扩展**的唯一硬证据：
    ///        文件版本一致 ≠ 浏览器加载的是新版 —— 它有自己的一份内存实例，不点 ⟳ 就不会换）
    bool ListProviders(std::vector<WebAiProviderInfo>& out, std::string& error,
                       int timeoutMs = 8000, std::string* extVersionOut = nullptr);

    /// 列出可抓取的标签页
    bool ListPages(std::vector<WebAiPageInfo>& out, std::string& error, int timeoutMs = 8000);

    /// ★ 穷举该站点**当前打开的所有**标签页（走扩展的 `webAiFindPage`）。
    ///
    /// ⚠⚠ 为什么不能用 `ListPages` 判「站点页面在不在」：那是给**挑游戏窗**用的 ——
    ///   按游戏 URL 打分、**每个窗口只留分最高的一个**、最后**只返回前 8 条**。
    ///   豆包这类站点 URL 得 0 分，会被游戏页挤掉 ⇒ 宿主看到"页面明明开着却说没有"，
    ///   报 `NO_TAB`（2026-09-25 用户实测就是这个症状）。
    ///   本函数只做域名匹配，不排序、不截断、不丢弃。
    bool FindProviderPages(const std::string& providerId, std::vector<WebAiPageInfo>& out,
                           std::string& error, int timeoutMs = 8000);

    /// ★★ 拿到**属于本软件**的站点标签页（没有就新建），并把网页 AI 的**独立调试会话**挂上去。
    ///
    /// ⚠⚠ 这是"绝不在用户自己的对话里乱发消息"的落地点：
    ///   扩展侧把 tabId 持久化在 `chrome.storage.local`，**每个站点只有一个属于本软件的标签页**；
    ///   用户的标签页一个都不碰。此前是"取第一个 URL 匹配的页面" ⇒ 那往往正是用户正在
    ///   跟豆包聊的那个对话（模型能看到用户的私人对话，我们的请求还会被追加进去）。
    ///
    /// @param createdOut  出参：这次是新建的吗（新建时站点已经给了干净上下文，不必再开新对话）
    /// @param attachedOut 出参：独立调试会话是否挂上了（false 时 CDP 写入/提交会失败）
    bool EnsureOwnTab(const std::string& providerId, int& tabIdOut, bool& createdOut,
                      bool& attachedOut, std::string& error, std::string& message,
                      int timeoutMs = 30000);

    /// ★ 把**我们自己的**标签页导航回站点的「新对话」入口 ⇒ 拿到干净上下文。
    /// ⚠ 只动我们自己的标签页；导航后必须**重新 attach**（document 换了）。
    bool StartNewChat(const std::string& providerId, int& tabIdOut, std::string& error,
                      std::string& message, int timeoutMs = 30000);

    /// ★★ 自动挑一个能用的站点（`web` 模型走这条）。
    ///
    /// 规则（用户 2026-09-25 指定）：
    ///   ① **已经开着**页面的站点优先（开着豆包就用豆包）；
    ///   ② 都没开 ⇒ 按内置顺序**逐个打开探测**，第一个"能找到可见输入框"（≈ 已登录）的用它，
    ///      探测失败的标签页由扩展**关掉**（不留垃圾）；
    ///   ③ 全都不行 ⇒ `error = "NO_WEB_AI_LOGIN"`，宿主在界面上提示"至少登录一个网页 AI"。
    ///
    /// ⚠ 超时给**很足**：最坏情况要开 4 个标签页并各等加载（实测一次约 25s）。
    ///   这是"首次配置"的一次性成本，之后 ① 就命中了。
    bool PickProvider(WebAiPickResult& out, int timeoutMs = 150000);

    /// 临时把我们的标签页切到前台（**读回答需要它真的渲染**）。
    /// ⚠ 后台标签页 `visibilityState === 'hidden'` ⇒ 站点不往 DOM 写流式回答
    ///   ⇒ 读不到回复（用户实测："必须我亲手手点网页前台…才能抓到对方的回复"）。
    /// @param prevTabIdOut 出参：原来那个活动标签页（读完要还回去）
    bool ActivateTab(const std::string& providerId, int tabId, int& prevTabIdOut,
                     std::string& error, int timeoutMs = 15000);

    /// 把前台还给用户原来那个标签页。
    /// ⚠ 只在「我们那个还是活动标签页」时还 —— 用户中途自己切走了就别抢回来。
    bool RestoreTab(int prevTabId, int ourTabId, std::string& error, int timeoutMs = 15000);

    /// 确保已 attach 到**我们自己的**站点标签页（幂等）。
    ///
    /// ⚠⚠ 语义已收紧（2026-09-25 真机事故后）：
    ///   ① 目标永远是「属于本软件的标签页」（`EnsureOwnTab`），**不是**「第一个 URL 匹配的页面」；
    ///   ② 不再走全局的 `attach` 消息 —— 那会 `detachDebugger()` 掉**正在跑的脚本**的会话。
    ///   ⚠ 原先的 `EnsurePageOpen()` 已**删除**：留两条"开页面"的路只会再踩坑。
    /// @param createdOut 可选出参：这次**新建了**专属标签页吗
    ///        （true = 网页对话是空的、上下文已丢 ⇒ 后端据此清零会话状态、下一轮全量重发重建）
    bool EnsureAttached(const std::string& providerId, std::string& error,
                        int timeoutMs = 20000, bool* createdOut = nullptr);

    /// 诊断用：输入框/发送按钮形态（`webAiProbe`）
    bool Probe(const std::string& providerId, nlohmann::json& probeOut, std::string& error,
               int timeoutMs = 20000);

    /// 把文字写进输入框（CDP 真实输入 + **扩展侧回读校验**）
    /// @param stepsOut 扩展的三段式明细（prepare/insertText/verify），失败时用它定位到段
    bool TypeRich(const std::string& providerId, const std::string& text,
                  nlohmann::json& stepsOut, std::string& error, std::string& message,
                  int timeoutMs = 30000);

    /// 按 Enter 提交（CDP 按键）
    bool Submit(std::string& error, std::string& message, int timeoutMs = 15000);

    /// ★ 传图：把宿主已落盘的本地图片挂到网页的上传框上（CDP `DOM.setFileInputFiles`）。
    ///
    /// ⚠ 必须在 `TypeRich` **之前**调用：先附图再写文字，最后才提交 —— 反过来的话
    ///   站点可能已经把消息发出去了，图只能落到下一轮。
    ///
    /// @param paths 宿主落盘的**绝对路径**（由 `web_ai_image.h` 的 `WriteTempImage` 产出）
    /// @param detailOut 扩展侧分段明细（find / fileButton / chooser / verify），失败时定位到段
    /// @return 只有「挂上 + 页面回读确认」才算 true（`setFileInputFiles` 不报错 ≠ 页面认了）
    bool UploadImages(const std::string& providerId, const std::vector<std::wstring>& paths,
                      nlohmann::json& detailOut, std::string& error, std::string& message,
                      int timeoutMs = 40000);

    /// 读当前对话的最后一条回答（扩展内部会等到"不忙且内容稳定 ≥1.2s"）
    bool ReadReply(const std::string& providerId, int idleTimeoutMs, int maxTotalMs,
                   WebAiReplyInfo& out, std::string& error, int timeoutMs = 60000);

    /// ★ 端到端一次：确保 attach → 记基线 → 写入 → 提交 → 等**新**回答出现
    /// @param idleTimeoutMs 单次阅读的空闲判据（毫秒）
    /// @param maxTotalMs    整体等待上限（毫秒）
    /// @param newConversation 这一轮是不是**新对话**（`TurnPlan::newConversation`）。
    ///        true ⇒ 先把我们自己的标签页导航到站点的「新对话」入口，
    ///        否则会在**上一轮那个会话**里继续（历史被重写时上下文就错了）。
    /// @param activateTab 读回答前是否**临时**把我们那个标签页切到前台。
    ///        默认 true：后台标签页 `visibilityState==='hidden'` ⇒ 站点不往 DOM 写流式回答
    ///        ⇒ 读不到回复（用户实测："必须我亲手手点网页前台…才能抓到对方的回复"）。
    ///        读完会**自动还回去**（用户中途自己切走了就不抢）。
    WebAiSendOutcome SendAndRead(const std::string& providerId, const std::string& text,
                                 int idleTimeoutMs = 8000, int maxTotalMs = 180000,
                                 bool newConversation = false, bool activateTab = true);

    /// 最近一次 `EnsureAttached`/`EnsureOwnTab` 拿到的专属标签页（0 = 还没有）
    int LastOwnTabId() const { return ownTabId_; }

private:
    int ownTabId_ = 0;
};

/// 便捷：桥端口（0 = 桥没在跑）
int CurrentBridgePort();

}  // namespace quickscript::webai
