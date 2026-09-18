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
    /// 工具轮内嵌入的脚本图片参考消息：即使非最后一条 user 也保留图片不剥离
    bool keep_images = false;
    /// 内部引导消息（重复提示/接近上限/继续推进/图片参考）：
    /// 不显示给用户、不计入用户轮次序号，但照常发给模型。
    bool internal_nudge = false;
    std::vector<ChatContentPart> parts;
    std::vector<ToolCallRecord> tool_calls;
    std::wstring tool_call_id;  // tool 消息专用
    std::wstring tool_name;     // 兼容旧逻辑（已弃用）
    std::wstring tool_args;
};

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

private:
    struct StreamApiResult {
        ChatMessage message;
        bool ok = false;
        std::wstring error;
        std::string finishReason;
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
};

/// 终态工具结果转成对用户可见的短回复：去掉动作一览与内部约束提示。
std::wstring AgentUserFacingToolReply(const std::wstring& toolResult);

/// 是否对当前网关/模型下发 `thinking.type=disabled`。
/// 2026-09-16 起默认**允许思考**（准确率优先；复杂任务如办公文档/多步规划明显受益），
/// 「只想不调工具」由上一轮未调工具就催的机制兜底；设环境变量 QST_FAST_THINKING=1
/// 可恢复旧的「强制快速执行」行为（仅对原本支持该开关的网关生效）。
bool ShouldDisableThinking(const std::wstring& apiUrl, const std::wstring& model);

/// 「上一轮只想不干」→ 接下来 N 轮关掉思考，强制直接动手（通用提速）。
/// 由工具循环在检测到「长思考但没调工具」时调用；每发出一次请求消费一轮。
void SuppressThinkingForNextRounds(int rounds);
void NoteThinkingRoundConsumed();
