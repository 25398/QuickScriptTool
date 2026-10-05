#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_core.h — AI Agent 核心引擎声明
// 封装与 OpenAI 兼容 API 的通信、工具调用循环、对话历史管理
// ──────────────────────────────────────────────────────────────────

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

// ── 一条对话消息 ──────────────────────────────────────────────────
struct ChatContentPart {
    std::wstring type;       // "text" | "image_url"
    std::wstring text;
    std::wstring image_url;  // data:image/png;base64,...
};

struct ToolCallRecord {
    std::wstring id;
    std::wstring name;
    std::wstring arguments;
};

struct ChatMessage {
    std::wstring role;          // "system", "user", "assistant", "tool"
    std::wstring content;
    std::wstring reasoning_content;  // DeepSeek 等思考模型需在后续请求中回传
    bool requires_reasoning_content = false;
    /// 附图保留策略 —— **只在「历史图片剥不剥」这一件事上有意义**：
    ///   0  = 默认：只有它还挂在「最后一条 user 消息」上时保留（观察帧就是这样，
    ///        下一轮就有更新的帧，旧帧剥成一句占位文字）
    ///   N>0 = 额外再保留 N 个 user 轮（**观察类附图**：`zoom` 裁剪图 /
    ///        搜索结果附图 —— 模型下一轮往往还要照着它动手，但再往后就是陈旧快照）
    ///  -1  = 永不剥（真正的**参考资料**，当前没有使用者；`readScript` 的脚本引用图
    ///        走的是 N>0 这条窗口）
    /// ⚠ 由来（实测）：观察类附图原来是「永不剥」，于是一局游戏里 6 张裁剪图一直挂在
    ///   历史里 ⇒ `请求体拆解 835KB = … 图 704（6 张）`，每轮都在为**已经过期的快照**付钱，
    ///   而且 API 首 token 越来越慢。图能留多久要**有界**，而且边界要写在这儿。
    int image_keep_rounds = 0;
    /// 内部引导消息（重复提示/接近上限/继续推进/图片参考）：
    /// 不显示给用户、不计入用户轮次序号，但照常发给模型。
    bool internal_nudge = false;
    std::vector<ChatContentPart> parts;
    std::vector<ToolCallRecord> tool_calls;
    std::wstring tool_call_id;  // tool 消息专用
    std::wstring tool_name;     // 兼容旧逻辑（已弃用）
    std::wstring tool_args;
};

/// 纯函数：某条 user 消息里的图片，在**发请求时**该不该剥掉（可逐格自检）。
/// `msgUserRound`/`lastUserRound` = 该消息与最后一条 user 消息各自的 user 轮序号；
/// `stripLastUserImages` = 本轮刻意不传图（如纯 DOM 轮省 token），只对 keep=0 的生效。
bool ShouldStripMessageImages(int msgUserRound, int lastUserRound, bool isLastUserMessage,
    int imageKeepRounds, bool stripLastUserImages);

// ── 工具定义 ──────────────────────────────────────────────────────
struct AgentTool {
    std::wstring name;
    std::wstring description;
    std::wstring parameters_json;  // JSON Schema 字符串
    std::function<std::wstring(const std::wstring& paramsJson)> execute;
};

// ── API 配置 ──────────────────────────────────────────────────────
struct AgentConfig {
    std::wstring apiUrl;
    std::wstring apiKey;
    std::wstring model;
    double temperature = 0.3;
    int maxTokens = 4096;
    int recvTimeoutMs = 120000;
};

// ── 回调类型 ──────────────────────────────────────────────────────
using ChunkCallback = std::function<void(const std::wstring& chunk)>;
using ContentDeltaCallback = std::function<void(const std::wstring& delta)>;
using ToolCallCallback = std::function<void(const std::wstring& name, const std::wstring& args)>;
using ReasoningCallback = std::function<void(const std::wstring& reasoning)>;
using ReasoningDeltaCallback = std::function<void(const std::wstring& delta)>;
using StatusCallback = std::function<void(const std::wstring& status)>;

struct AiHttpAbortSlot {
    std::atomic<HINTERNET> request{nullptr};
    /// Abort() 已经关过句柄（所有权已转移给它）→ RAII 守卫不要再关一次：
    /// 重复关同一个句柄值有误关「已被系统复用给别人的句柄」的风险。
    std::atomic_bool closed{false};

    void Set(HINTERNET h) {
        closed.store(false, std::memory_order_release);
        request.store(h, std::memory_order_release);
    }
    void Clear() { request.store(nullptr); }
    // 用户强制中断 / 看门狗策略超时：关闭进行中的请求句柄以解除 WinHTTP 阻塞
    void Abort() {
        HINTERNET h = request.exchange(nullptr);
        if (!h) return;
        closed.store(true, std::memory_order_release);
        WinHttpCloseHandle(h);
    }
    /// 一次性取走「已被 Abort 关闭」标记（RAII 守卫用）
    bool ConsumeClosed() { return closed.exchange(false, std::memory_order_acq_rel); }
};

struct AgentSendCallbacks {
    ChunkCallback onChunk = nullptr;
    ContentDeltaCallback onContentDelta = nullptr;
    ToolCallCallback onToolCall = nullptr;
    std::function<void(const std::wstring& name, const std::wstring& result)> onToolResult = nullptr;
    ReasoningCallback onReasoning = nullptr;
    ReasoningDeltaCallback onReasoningDelta = nullptr;
    StatusCallback onStatus = nullptr;
    const std::atomic_bool* cancelFlag = nullptr;
    AiHttpAbortSlot* httpAbort = nullptr;
    // AI 动作执行：submitMacroActions 成功后不再发起下一轮 API
    std::function<bool()> stopToolLoopAfterTools = nullptr;
    // 宏回放中的 AI 调用：直接用完整 HTTP 响应，避免流式读流不稳定
    bool preferNonStream = false;
    /// 本轮 SendMessage 首次 API 请求的 tool_choice（如 L"required"）；空=auto。
    /// 工具循环续轮强制 auto，避免 required 死循环（对齐 OpenAI Agents forcing_tool_use）。
    std::wstring toolChoice;
};

// ── Agent 核心引擎 ────────────────────────────────────────────────
/// ★ 设置**本线程**的「会话 key」——它会作为 `user` 字段随请求体发出去。
///
/// 为什么用 thread_local 而不是成员：AI 助手的每个会话在**自己的线程**里发送
/// （`webview_bridge_backend.cpp` 的 `std::thread`），而 `AgentCore` 实例可能被多个面板共用
/// ⇒ 放成员会互相覆盖。
///
/// 用途：网页版 AI 后端（`/v1/chat/completions`）靠它**按会话分状态** ——
/// 否则同一站点的两个会话会共用「已发到第几条」，第二轮起内容就错乱了。
/// （对应 UI 的 `agentTabs[].key` / 桥消息里的 `panelKey`。）
void SetAgentSessionKeyForThisThread(const std::string& keyUtf8);

/// ★★ **引擎侧**的会话 key 覆盖（2026-10-02 用户要求）。
///
/// 场景：AI 助手调 `runDesktopTask` ⇒ 走「临时脚本 + 引擎回放」⇒
/// 执行发生在**引擎线程**（与助手线程不同）⇒ `thread_local` 的 sessionKey **传不过去**
/// ⇒ 引擎里的 AI 动作用的是自己的 `qst-once` ⇒ **在豆包那边又开了一个新对话** ✓
///
/// ⇒ 助手在**启动任务前**设上自己的 key，引擎执行 AI 动作时读它 ⇒
/// **复用助手那个对话**，不再新开 ✓
///
/// ⚠ 用**全局**而不是 thread_local：跨越线程边界。任务串行（引擎 `IsBusy` 门）⇒ 不会打架。
void SetEngineSessionKeyOverride(const std::string& keyUtf8);
std::string EngineSessionKeyOverride();

/// ★★ 读回本线程的会话 key（用于**保存/还原**）。
///
/// 为什么需要它（2026-10-02 真机"串台"事故）：引擎在跑一次 AI 动作前要临时把它换成
/// 这次执行专属的 key，跑完必须还原成原值 —— 没有 getter 就只能"清空"，
/// 那会把同线程上原有的会话身份抹掉（下一个请求又落回共享会话）。
std::string AgentSessionKeyForThisThread();

/// ★★ 本线程的请求**不要在网页端开新对话**（"生成对话标题"这类后台小请求用）。
///
/// ⚠⚠ 为什么必须（2026-09-26 真机：用户看到"一次新对话会开两个新对话"）：
///   `RequestAiConversationTitleAsync` 自己造了个 `AgentCore` 发一次请求，
///   它**不带会话 key** ⇒ 后端把它当成**新会话** ⇒ `turns == 0`
///   ⇒ `newConversation = true` ⇒ **在豆包那边又开一个对话**
///   （截图里那个叫「简单数学问题问答」的对话，内容就是标题生成的提示词）。
///   ⇒ 后台小请求必须显式声明"别开新对话"。
void SetAgentNoNewChatForThisThread(bool on);

class AgentCore {
public:
    AgentCore(const AgentConfig& config,
              const std::wstring& systemPrompt,
              const std::vector<AgentTool>& tools);

    // 发送用户消息，返回 AI 的最终文本回复
    // 自动处理 tool-call 循环（最多10轮）
    std::wstring SendMessage(const ChatMessage& userMessage,
                             const AgentSendCallbacks& callbacks = {});

    // 兼容旧接口
    std::wstring SendMessage(const ChatMessage& userMessage,
                             ChunkCallback onChunk,
                             ToolCallCallback onToolCall = nullptr);

    std::wstring SendMessage(const std::wstring& userMessage,
                             ChunkCallback onChunk = nullptr,
                             ToolCallCallback onToolCall = nullptr);

    // 获取完整对话历史
    const std::vector<ChatMessage>& GetHistory() const { return messages_; }

    // 清空对话历史（保留 system prompt）
    void ClearHistory();

    /// 用完整历史替换当前消息（用于恢复已保存对话）
    void SetFullHistory(std::vector<ChatMessage> messages);

    /// 编辑重发：按 user 消息序号（0 起）截断历史，保留该条消息之前的内容
    /// （不含该条消息）。序号越界返回 false。
    bool TruncateHistoryToUserRound(size_t userRoundIndex);

    /// 从 other 追加非 system 消息（startIndex 起，通常传 1 或追加起点）
    void ImportHistoryFrom(const AgentCore& other, size_t startIndex = 1);

    // 更新 API 配置与 system prompt（切换模型时使用）
    void UpdateConfig(const AgentConfig& config, const std::wstring& systemPrompt);

    // 热更新工具列表（新增工具后无需重建 AgentCore）
    void UpdateTools(const std::vector<AgentTool>& tools);

    void SetRecvTimeoutMs(int recvTimeoutMs) {
        config_.recvTimeoutMs = std::max(5000, recvTimeoutMs);
    }

    const AgentConfig& GetConfig() const { return config_; }

    // 强制中断进行中的 HTTP 请求（配合 AiHttpAbortSlot）
    void AbortActiveHttp();

    /// 上一轮请求体的成分拆解（KB）。用途：长任务里请求体一路涨到几百 KB，
    /// 但**光看总大小判不出是谁涨的** —— 是历史整帧图没剥掉、旧思考全文在回灌，
    /// 还是工具结果堆积？实测日志里三种猜测都像，只有拆开看才知道该改哪一条。
    /// 字段在每次 BuildRequest 后刷新。
    struct RequestBreakdown {
        size_t totalKb = 0;
        size_t systemKb = 0;
        size_t imagesKb = 0;      ///< 仍在请求里的 data URL（没被剥掉的帧）
        size_t reasoningKb = 0;   ///< 回灌的 reasoning_content
        size_t toolResultsKb = 0; ///< role=tool 的结果文本
        size_t assistantKb = 0;   ///< assistant 正文（含工具调用参数）
        size_t userTextKb = 0;    ///< user 文本部分
        size_t toolsSchemaKb = 0; ///< 工具定义
        int messageCount = 0;
        int imagesKept = 0;       ///< 实际带上去的图张数
        /// 本轮请求**实际**带没带 `thinking.type=disabled`，以及判据是哪一条。
        /// 为什么要记：日志一度写死「已关闭思考」而这只是**策略**（`ShouldDisableThinking`），
        /// 与「字段真的发出去了、网关真的认」是两件事 —— 排查时无法区分
        /// 「处置没生效」和「处置瞄错了对象」（docs §42）。
        bool thinkingFieldSent = false;
        std::wstring thinkingWhy;  ///< 判据来源（抑制计数 / QST_FAST_THINKING / 未关）
                                   ///< ⚠ 原「游戏前台」那一档批 D 已删（docs §47.4）
        /// 本轮请求**实际**带没带 `reasoning_effort=low`（思考档位）。
        /// 同 thinkingFieldSent 的理由：**发出去 ≠ 网关认**，所以要如实写「发了没有」，
        /// 而不是写「我们打算让它少想」。
        bool reasoningEffortSent = false;
    };
    const RequestBreakdown& LastRequestBreakdown() const { return lastBreakdown_; }
    /// 一行摘要（进诊断日志）
    std::wstring FormatLastRequestBreakdown() const;

private:
    struct StreamApiResult {
        ChatMessage message;
        bool ok = false;
        std::wstring error;
        std::string finishReason;
        /// 流是被**看门狗主动收束**的（不是网络/网关失败）。调用方的措辞要分开：
        /// 「收束」是我们自己的处置，写成「流式失败」就是日志说谎（§41.3／§42）。
        bool watchdogCut = false;
    };

    std::wstring CallApi(const json& requestBody, std::wstring* errorOut = nullptr,
                         const std::atomic_bool* cancelFlag = nullptr,
                         AiHttpAbortSlot* httpAbort = nullptr,
                         StatusCallback onStatus = nullptr);
    StreamApiResult CallApiStream(const json& requestBody, const AgentSendCallbacks& callbacks);
    json BuildRequest(bool stripLastUserImages = false,
                      const std::string& toolChoice = "auto");

    AgentConfig config_;
    std::vector<ChatMessage> messages_;
    std::vector<AgentTool> tools_;
    AiHttpAbortSlot* activeHttpAbort_ = nullptr;
    RequestBreakdown lastBreakdown_;
};

/// 终态工具结果转成对用户可见的短回复：去掉动作一览与内部约束提示。
std::wstring AgentUserFacingToolReply(const std::wstring& toolResult);

/// 是否对当前网关/模型下发 `thinking.type=disabled`。
/// 2026-09-16 起默认**允许思考**（准确率优先；复杂任务如办公文档/多步规划明显受益），
/// 「只想不调工具」由上一轮未调工具就催的机制兜底；设环境变量 QST_FAST_THINKING=1
/// 可恢复旧的「强制快速执行」行为（仅对原本支持该开关的网关生效）。
///
/// **两条判据（批 D 之后只剩两条）**：
///   ① 上一轮「只想不干」→ 接下来 2 轮关思考（计数按请求消耗，见 NoteThinkingRoundConsumed）；
///   ② `QST_FAST_THINKING=1` 逃生阀（外部开关，不是引擎决策）。
/// ★原 ③「游戏前台降档」（`AiGameForegroundDecisionIsDecisive` → 关思考）**已整条删除
///   （批 D，docs §47.4）**：引擎不替模型决定「这一轮要不要想」。
/// ⚠ `AiActionExecThinkingScope` / `InAiActionExecScope()` **仍然保留** —— 名字里带"思考"，
///   但它现在是 §42 两条协议闸的入参（区分「AI 动作执行」与「聊天助手」）。
bool ShouldDisableThinking(const std::wstring& apiUrl, const std::wstring& model);

/// AI 动作执行期的作用域标记（RAII）。放在 `ExecuteAiActionExecute` 作用域里。
/// ⚠ 它**不再**用于思考降档（那条已删），但仍被 §42 的协议闸读取：
///   `AiDecideStreamBrake(... inActionScope)` 与 `AiDecideSlowThinkingRound(... inActionScope)`
///   靠它区分「AI 动作执行」与「聊天助手」（聊天助手的长回答是用户要的，不该被看门狗收束）。
struct AiActionExecThinkingScope {
    AiActionExecThinkingScope();
    ~AiActionExecThinkingScope();
    AiActionExecThinkingScope(const AiActionExecThinkingScope&) = delete;
    AiActionExecThinkingScope& operator=(const AiActionExecThinkingScope&) = delete;
};
/// 当前是否在 AI 动作执行作用域内（§42 协议闸的入参；不再是思考降档条件）
bool InAiActionExecScope();

/// 「上一轮只想不干」→ 接下来 N 轮关掉思考，强制直接动手（通用提速）。
/// 由工具循环在检测到「长思考但没调工具」时调用；**每发一轮请求必须调一次
/// `NoteThinkingRoundConsumed()` 消耗它**，否则计数器永久 >0、思考再也回不来。
void SuppressThinkingForNextRounds(int rounds);
void NoteThinkingRoundConsumed();
/// 自检用：把「抑制 N 轮」计数清零（让用例从确定状态开始，不受用例顺序影响）。
void ResetThinkingSuppressionForTest();

/// ★★本轮（**同一条 assistant 消息**）后面还剩几个工具调用（docs §72）。
///
/// 由 `SendMessage` 的工具循环在**每次调用工具之前**写入；宿主侧只读。
///
/// 为什么要跨文件传这个数：引擎判「这一步要不要等界面稳定（settle）」的依据是
/// 「**本批之后还有没有交互步**」（`engine_script_run.cpp` 的 settle 前瞻）。
/// 而模型一次并行发 20 个 `mouseClick` 时，**每个工具调用都是独立的一批**
/// （各自 1~2 个动作）⇒ 每个都成了「最后一个交互步」⇒ 20 次点击各等一次 settle。
/// 实测（真机日志）：16 个工具的一轮 `本地执行 38125ms`、20 个工具的 `25843ms`，
/// 其中绝大部分是**每击一次就等一次界面**（每次 0.5~1.3s，游戏画面永远"仍在变化"，
/// 每次都要等满超时）。引擎看不到工具循环，所以由这里把数报过去。
void NoteAiToolCallsRemainingInRound(int remaining);
int AiToolCallsRemainingInRound();
