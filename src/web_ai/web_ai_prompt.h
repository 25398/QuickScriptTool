#pragma once
// ──────────────────────────────────────────────────────────────────
// web_ai_prompt.h — 网页版 AI 后端的**纯逻辑层**（不碰 Win32、不碰网络）
//
// 职责（对标 chen-squared/browser-ai-bridge 的 prompt.ts）：
//   ① OpenAI `messages` + `tools` → 「这一轮往网页输入框里写什么」
//   ② 网页回答 → OpenAI 形状的 `tool_calls`（反解析）
//   ③ OpenAI 响应体（非流式 JSON / 流式 SSE 文本）
//
// ⚠⚠ 为什么这一层必须独立成纯函数：网页 AI **没有原生 function calling**，
//   「工具调用」完全靠**约定文本协议**（见下）。协议解析错一次 = 静默执行错动作，
//   所以它必须能逐格自检（`WebAiSelfTest`），不许埋在 HTTP/桥/线程代码里。
//
// ── 工具调用协议（给模型看的唯一格式）──────────────────────────────
//   <<<QST_TOOL_CALLS>>>
//   [{"name":"mouseClick","arguments":{"x":10,"y":20,"button":"left"}}]
//   <<<END_QST_TOOL_CALLS>>>
//
//   选这个形态的理由（每一条都是踩坑后的取舍）：
//   · 用 ASCII 尖括号标记而不是 ```json 围栏 —— 网页版 AI 的回答常带 Markdown
//     渲染，代码围栏里的内容可能被加上零宽字符/换行规范化；自定义标记更稳，
//     且**不与正常回答撞车**（模型自己解释协议时也不会误触发）。
//   · 用 JSON 而不是「函数式调用文本」（`mouseClick(x=10)`）—— 参数里的中文、
//     引号、嵌套对象用 JSON 才不会歧义。
//   · 允许「有开头没结尾」被识别为**未完成**（`toolCallsIncomplete`）——
//     网页回答是流式渲染的，半截 JSON 若被当成完整结果解析，轻则丢一次动作，
//     重则 `arguments` 被截断后**静默用错坐标**。宁可多等一次。
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace quickscript::webai {

/// 给模型看的工具调用协议标记（改这里必须同步自检与文档）
extern const char* const kToolCallsBegin;
extern const char* const kToolCallsEnd;

/// 单条消息里的图片 part 个数
int CountImageParts(const nlohmann::json& msg);

/// 一次请求规划出来的「要写进输入框的文本」
struct TurnPlan {
    bool valid = false;
    /// 这一轮是**新对话**（要带 system + 工具清单 + 协议说明前缀）
    /// 要不要在**网页端开一个新对话**（用户可见）。
    /// ⚠ 只由「软件侧这个会话是不是第一次」决定 —— 见 `resendFull` 的说明。
    bool newConversation = false;

    /// ★★ 技术上要不要**重发全量**（系统设定 + 工具 + 协议 + 全部消息）。
    ///
    /// ⚠⚠ 与 `newConversation` 是**两件正交的事**（2026-09-26 用户要求拆开）：
    ///   · 历史被重写（引擎重建请求体 / 纠偏注入 / 历史压缩）⇒ **必须**重发全量，
    ///     否则网页对话里的上下文和实际内容对不上；
    ///   · 但这**不该**顺手在网页端开一个新对话 ——
    ///     用户原话："如果软件开的新对话，那 Agents 才应该开新对话。
    ///     否则就用之前我们软件为 Agents 开的新对话"。
    ///   ⇒ 原来两者绑在一个标志上 ⇒ 历史一被重写就**每问一次多一个豆包对话**。
    bool resendFull = false;
    /// 要写进网页输入框的文本（UTF-8）
    std::string text;
    /// 工具清单因超长被裁剪（只丢描述，不丢工具名/参数）
    bool toolsTrimmed = false;
    /// 本轮请求里**原本带的**图片张数（无论最后有没有真送过去）
    int imagesStripped = 0;
    /// 其中**已真的挂到网页上传框上**的张数（见 web_ai_image.h 的传图通道）
    ///
    /// ⚠ 两个数的关系必须如实：`imagesStripped` 是"请求里有几张"，
    ///   `imagesAttached` 是"实际送到网页几张"。提示词里必须按**差值**说话 ——
    ///   全部送到时说「已附上 N 张图」，一张没送到时才说「本通道无法回传图像」。
    ///   说反了就是静默错误：模型会凭空猜坐标，或对着真存在的图说"我看不到"。
    int imagesAttached = 0;
    /// 总字符数（用于日志与「超长」如实留痕）
    int totalChars = 0;
    /// 无效时的原因（英文短码，进错误回执）
    std::string error;
};

/// 会话状态：**必须由调用方持久化**（桥的每次 HTTP 请求都是无状态的）
struct SessionState {
    /// 已经写进网页输入框的 messages 条数
    int sentMessageCount = 0;
    /// 上一条已发送消息的指纹（用于判断「还是同一个对话」）
    std::string lastFingerprint;
    /// 这个会话已经发过几轮
    int turns = 0;

    /// ★★ **工具说明/清单已经发过了吗**（2026-10-02 协议瘦身）。
    ///
    /// ⚠ 为什么（用户要求）：工具说明是**每会话只需要一次**的东西，
    ///   而原来它挂在 `resendFull`（= 历史被重写）上 ⇒ **每次历史重写都重发一遍**
    ///   （日志实证：多数请求 ~500 字，但重写那一次 **9099 字**）。
    ///   ⇒ 改成"只在没发过时发"，把重发的那 9 KB 也省掉。
    ///
    /// ⚠ 唯一的例外：**专属标签页被重建**（换页签 / 扩展重启 / 对话丢了）⇒
    ///   网页端上下文整个没了 ⇒ 后端会把这个 `SessionState` **清零** ⇒
    ///   `toolsSent` 跟着回到 false ⇒ 下一轮全量重发重建上下文 ✓
    bool toolsSent = false;

    /// ★★ 上次发过的**工具清单内容指纹**（2026-10-02 修正）。
    ///
    /// ⚠⚠ 为什么不能只用 `toolsSent`（bool）：**同一个会话会被多种任务共用**
    ///   （练习题 / 桌面定位 / 脚本生成 …），而它们的**工具集不同**。
    ///   只按"每会话一次"发 ⇒ 练习题跑过之后 `turns > 0` ⇒ 后面的桌面任务
    ///   **永远收不到清单** ⇒ 模型开始**瞎猜工具名**
    ///   （真机日志：`未知工具：computer` / `screenCapture` / `findImage`，最后 `NOT_FOUND`）。
    ///
    ///   ⇒ 改成"**清单内容变了就重发**"：
    ///     · 同一任务续轮 ⇒ 指纹不变 ⇒ **不发**（省 token）✓
    ///     · 换成另一种任务 ⇒ 指纹变 ⇒ **发**（模型知道有哪些工具）✓
    std::string toolsFingerprint;
};

/// 上限（可在 web_ai_config.json 覆盖）：
/// 网页输入框能吃多少字**未实测**，所以先给一个保守上限，
/// 超了就丢**描述**保**结构**（工具名与参数名），而不是静默截断整段。
struct PromptBudget {
    int maxToolListChars = 12000;  ///< 工具清单段上限
    int maxTotalChars = 24000;     ///< 首次前缀（system+工具+协议）上限
    /// ★ 渐进披露：首轮只给**工具目录**（名字+参数名+一句话），
    ///   模型用 `loadTools` 索取完整定义（见本文件顶部那段说明）。
    ///   ⚠ 关掉它会退回"首轮就发全部完整定义"（30~50 KB），只在模型不听话时才需要。
    bool toolCatalogMode = true;
    /// 目录里每条描述截到多少字（太长就失去"瘦身"的意义）
    int catalogDescChars = 48;
};

/// 单条消息的指纹（role + 内容；用于判断请求是否延续上一轮）
std::string MessageFingerprint(const nlohmann::json& msg);

/// 取消息的纯文本内容（`content` 可能是字符串，也可能是 parts 数组）。
/// 图片 part 会被换成一行 `[图片: <mime>]` 占位（图片另走上传通道，见 docs）。
std::string MessageTextForPrompt(const nlohmann::json& msg);

/// ★ 核心：规划这一轮要写什么。
/// @param messages OpenAI messages 数组
/// @param tools    OpenAI tools 数组（空数组 = 不带工具）
/// @param session  上一轮的状态（turns==0 视为全新会话）
/// @param next     回填新状态（仅在 valid=true 时有意义）
/// @param budget   长度预算
/// @param imagesAttached 已经**真的**挂到网页上传框上的图片张数
///        （由调用方先跑 `ExtractImagesFromMessages` + 上传，再把成功数传进来）
TurnPlan PlanTurn(const nlohmann::json& messages, const nlohmann::json& tools,
                  const SessionState& session, SessionState& next,
                  const PromptBudget& budget = {},
                  int imagesAttached = 0);

/// 渲染工具清单（`1. name(参数:类型) — 描述`）。单独暴露是为了自检。
std::string RenderToolList(const nlohmann::json& tools, int maxChars, bool* trimmed);

// ── ★★ 渐进披露（Progressive Disclosure）：首轮只给「目录」，模型按需索取完整定义 ──
//
// 为什么：网页 AI 没有原生 tools 通道，工具定义只能当**文本**发。
// ⚠⚠ 先纠正一个容易说错的点（我自己第一版就说错了）：`PlanTurn` 的**工具清单只在首轮发**
//   （`if (plan.newConversation)`），**不是每轮重发** ⇒ 这是**一次性**成本，不是乘数。
//   （"每轮重发"那条说的是**原生 API** 路径，见 LESSONS §25，别混用。）
//
// **如实**的收益（自检里量过，`tool_catalog_is_far_smaller`）：
//   目录省掉的是「参数**类型** + 必填标记 + 工具描述（160→48 字节）」，
//   实测这个形状约 **2.2~2.5 倍**，**不是数量级**。
//   ⚠ 省不到更多的原因：参数**名字**必须留着（模型要靠它拼 arguments），
//     短枚举也**刻意**留着（`button:left|right` 才 10 来字符，却能挡住"模型自己编个值"）。
//
// 参考：Anthropic Agent Skills 的分层加载、社区 "Tool Search" 方案。

/// `loadTools` 元工具名（模型用它索取完整定义）
extern const char* const kLoadToolsName;

/// 渲染「工具**目录**」：`1. 名字(参数名[:短枚举]) — 短描述`。
/// ⚠ 与 `RenderToolList` 的区别：**不含参数类型 / 参数长描述** ——
///   那些由 `RenderToolDefinitions` 在模型真的要用时才给。
/// ⚠⚠ 但**短枚举必须留着**（`button:left|right|middle`、`section:all|format|…`）：
///   模型挑工具时唯一能看到"这个参数能填什么值"的地方就是这里
///   （描述只有 48 字节，写在描述里的取值等于没写）。见 LESSONS §79/§80。
std::string RenderToolCatalog(const nlohmann::json& tools, int descChars,
                              int maxChars, bool* trimmed);

/// 渲染**指定工具**的完整定义（参数类型 / 枚举 / 必填 / 描述）。
/// @param names 要渲染的工具名；不在 `tools` 里的会**如实标注"未找到"**（不许静默丢）
std::string RenderToolDefinitions(const nlohmann::json& tools,
                                  const std::vector<std::string>& names);

/// 从 `tool_calls` 里挑出 `loadTools` 调用：返回它要的工具名，并把这些调用**从数组里摘掉**
/// （它们不是真正要执行的工具，不能交给上层去跑）。
/// @return 是否含有 `loadTools` 调用
bool TakeLoadToolNames(nlohmann::json& toolCalls, std::vector<std::string>& namesOut);

// ── ★★ 类型安全地读 JSON 字段（**绝不抛异常**）────────────────────────────
//
// ⚠⚠ 为什么必须用它，而不是 `json::value(key, default)`：
//   `value()` 在**类型不符**时会抛 `nlohmann::json::type_error`。
//   而这些读法大量出现在**桥的 HTTP 线程**里（`HandleChatCompletion` 链路）⇒
//   异常逃出线程函数 ⇒ **`std::terminate` ⇒ 整个软件瞬间消失、无日志、无退出码**。
//
//   前科（2026-09-25 真机）：扩展回执里 `error` 是 **boolean** 而不是 string，
//   `out.value("error", std::string(...))` 直接抛 `type_error.302` ⇒ 闪退。
//   而且因为异常发生在**写日志之前**，连一行线索都没留下。
//
//   ⇒ 统一用这三个：类型不符时返回默认值，并把**实际类型**写到 `mismatchOut`
//     （调用方据此记账 —— **静默吞掉的话下次又要靠猜**）。
std::string JsonStr(const nlohmann::json& o, const char* key,
                    const std::string& dflt = std::string(),
                    std::string* mismatchOut = nullptr);
bool JsonBool(const nlohmann::json& o, const char* key, bool dflt = false,
              std::string* mismatchOut = nullptr);
int JsonInt(const nlohmann::json& o, const char* key, int dflt = 0,
            std::string* mismatchOut = nullptr);

/// 反解析结果
struct ParsedReply {
    /// 去掉协议块之后的正文（给用户看的）
    std::string content;
    /// OpenAI 形状的 tool_calls 数组（无则空数组）
    nlohmann::json toolCalls = nlohmann::json::array();
    /// 出现了开头标记但没有结尾标记 ⇒ 回答还没生成完（**不要**当成完整结果）
    bool incomplete = false;
    /// 有协议块但 JSON 解析失败（诊断用，附带原因）
    std::string parseError;
};

/// ★ 从网页回答里反解析工具调用
ParsedReply ParseReply(const std::string& replyText);

/// 响应诊断信息（进 `usage` 与 `x_web_ai` 扩展字段；AgentCore 不读它们，
/// 但**排障时必须能回答「这次回答是哪个站点、等了多久、有没有被截断」**）
struct CompletionMeta {
    int promptTokens = 0;
    int completionTokens = 0;
    std::string provider;   ///< doubao / deepseek / ...
    int elapsedMs = 0;
    bool truncated = false; ///< 网页回答被扩展侧 12000 字上限截断
    std::string note;       ///< 其他如实留痕（如协议块未完成）
};

/// 构造 OpenAI 非流式响应体（`/v1/chat/completions`）
std::string BuildCompletionJson(const std::string& id, const std::string& model,
                                const ParsedReply& reply, const CompletionMeta& meta = {});

/// 构造 OpenAI 流式响应（SSE 文本；单分片正文 + [DONE]）。
/// ⚠ 为什么要支持流式：`AgentSendCallbacks::preferNonStream` **不是处处为真**
///   （`ai_action_service.cpp` 里有若干 `preferNonStream = false` 的构造点）。
///   端点两种都答得出，就不需要在 7 个调用点上逐个改标志位（少改 = 少漏）。
/// ⚠ 分片里的 `tool_calls` 必须是**完整一段**（不拆 `arguments` 增量）：
///   消费端（`agent_core.cpp:1144-1148`）的 `arguments` 是**增量字符串累加**，
///   首片给完整 JSON 串结果等价；拆增量则任何一处漏拼都是**静默**错误。
/// ⚠ `finish_reason` 必须是字符串或 null：消费端那句 `.get<std::string>()`
///   在 `try` 块**之外**（`agent_core.cpp:1177-1180`），给数字会抛 `type_error`。
std::string BuildCompletionSse(const std::string& id, const std::string& model,
                               const ParsedReply& reply, const CompletionMeta& meta = {});

/// 生成 completion id（`chatcmpl-webai-<tick>`）
std::string MakeCompletionId();

/// 由模型名判定 provider id（`doubao-web` → `doubao`）；不认识返回空
std::string ProviderFromModelName(const std::string& modelName);

/// 该 url 是否「网页 AI 档案」（本机回环 + /v1/chat/completions）
bool IsWebAiApiUrl(const std::string& utf8Url);

/// 规范成当前桥端口（档案里存的端口可能过期：软件重启会换端口）
std::string CanonicalWebAiUrl(int port);

/// 从 url 里取端口（解析不出返回 0）
int PortFromUrl(const std::string& utf8Url);

/// 粗略 token 估算（≈ 字符数 / 3；只用于 usage 字段与日志，不作计费依据）
int EstimateTokens(const std::string& utf8Text);

}  // namespace quickscript::webai
