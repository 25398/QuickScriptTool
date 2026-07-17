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

    void Set(HINTERNET h) { request.store(h); }
    void Clear() { request.store(nullptr); }
    // 用户强制中断：关闭进行中的请求句柄以解除 WinHTTP 阻塞
    void Abort() {
        HINTERNET h = request.exchange(nullptr);
        if (h) WinHttpCloseHandle(h);
    }
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

    /// 从 other 追加非 system 消息（startIndex 起，通常传 1 或追加起点）
    void ImportHistoryFrom(const AgentCore& other, size_t startIndex = 1);

    // 更新 API 配置与 system prompt（切换模型时使用）
    void UpdateConfig(const AgentConfig& config, const std::wstring& systemPrompt);

    // 热更新工具列表（新增工具后无需重建 AgentCore）
    void UpdateTools(const std::vector<AgentTool>& tools);

    void SetRecvTimeoutMs(int recvTimeoutMs) {
        config_.recvTimeoutMs = std::max(5000, recvTimeoutMs);
    }

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
    json BuildRequest(bool stripLastUserImages = false);

    AgentConfig config_;
    std::vector<ChatMessage> messages_;
    std::vector<AgentTool> tools_;
    AiHttpAbortSlot* activeHttpAbort_ = nullptr;
};
