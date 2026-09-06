// ──────────────────────────────────────────────────────────────────
// agent_core.cpp — AI Agent 核心通信层实现
// WinHTTP 通信、JSON 请求构建、tool-call 自动循环
// ──────────────────────────────────────────────────────────────────

#include "agent_core.h"
#include "agent_attachment.h"
#include "agent_ai_actions.h"
#include "macro_execute_tools.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// ── WinHTTP handle 安全管理 ───────────────────────────────────────
struct WinHttpHandle {
    HINTERNET handle = nullptr;
    WinHttpHandle(HINTERNET h = nullptr) : handle(h) {}
    ~WinHttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    operator HINTERNET() const { return handle; }
    // 禁止拷贝，允许移动
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) { if (handle) WinHttpCloseHandle(handle); handle = other.handle; other.handle = nullptr; }
        return *this;
    }
    HINTERNET Detach() {
        HINTERNET h = handle;
        handle = nullptr;
        return h;
    }
};

// ── URL 解析 ──────────────────────────────────────────────────────
struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
    bool isHttps = true;
};

ParsedUrl ParseUrl(const std::wstring& url) {
    ParsedUrl result;
    size_t schemeEnd = url.find(L"://");
    if (schemeEnd == std::wstring::npos) return result;
    result.isHttps = (url.substr(0, schemeEnd) == L"https");
    if (!result.isHttps) result.port = INTERNET_DEFAULT_HTTP_PORT;
    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find(L'/', hostStart);
    std::wstring hostPort;
    if (pathStart != std::wstring::npos) {
        hostPort = url.substr(hostStart, pathStart - hostStart);
        result.path = url.substr(pathStart);
    } else {
        hostPort = url.substr(hostStart);
        result.path = L"/";
    }
    size_t colon = hostPort.find(L':');
    if (colon != std::wstring::npos) {
        result.host = hostPort.substr(0, colon);
        result.port = static_cast<INTERNET_PORT>(std::wcstol(hostPort.c_str() + colon + 1, nullptr, 10));
    } else {
        result.host = hostPort;
    }
    return result;
}

std::wstring WinHttpErrorText(DWORD err) {
    if (err == 0) return L"未知错误(0)";
    auto formatFrom = [](DWORD flags, HMODULE mod, DWORD code) -> std::wstring {
        wchar_t* msg = nullptr;
        const DWORD n = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | flags | FORMAT_MESSAGE_IGNORE_INSERTS,
            mod, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
        std::wstring out;
        if (n && msg) out = msg;
        if (msg) LocalFree(msg);
        return out;
    };
    if (HMODULE wh = GetModuleHandleW(L"winhttp.dll")) {
        if (std::wstring out = formatFrom(FORMAT_MESSAGE_FROM_HMODULE, wh, err); !out.empty())
            return out;
    }
    if (std::wstring out = formatFrom(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err); !out.empty())
        return out;
    wchar_t buf[48]{};
    swprintf_s(buf, L"错误码 %lu (0x%08lX)", err, err);
    return buf;
}

constexpr DWORD kHttpBodyPollMs = 250;
// 5s 轮询：等待期间可打心跳/响应取消；超时当临时错误重试（见 IsTransientHttpReceiveError）
constexpr DWORD kHttpReceivePollMs = 5000;

bool HttpSendJsonBody(HINTERNET hRequest, const std::wstring& headers, const std::string& body,
                      const std::atomic_bool* cancelFlag, std::wstring* errorOut,
                      StatusCallback onStatus = nullptr) {
    const DWORD total = static_cast<DWORD>(body.size());
    if (onStatus) onStatus(L"正在发送请求头…");
    if (!WinHttpSendRequest(
            hRequest, headers.c_str(), static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, total, 0)) {
        if (errorOut) *errorOut = L"发送请求失败：" + WinHttpErrorText(GetLastError());
        return false;
    }
    size_t sent = 0;
    constexpr size_t kChunk = 65536;
    int lastPct = -1;
    while (sent < body.size()) {
        if (cancelFlag && cancelFlag->load()) {
            if (errorOut) *errorOut = L"已取消";
            return false;
        }
        const size_t remain = body.size() - sent;
        const DWORD chunk = static_cast<DWORD>(std::min(kChunk, remain));
        DWORD written = 0;
        if (!WinHttpWriteData(hRequest, body.data() + sent, chunk, &written)) {
            if (cancelFlag && cancelFlag->load()) {
                if (errorOut) *errorOut = L"已取消";
                return false;
            }
            if (errorOut) *errorOut = L"上传请求体失败：" + WinHttpErrorText(GetLastError());
            return false;
        }
        if (written == 0) break;
        sent += written;
        if (onStatus && total > 0) {
            const int pct = static_cast<int>((sent * 100ull) / total);
            if (pct >= lastPct + 10 || sent >= body.size()) {
                lastPct = pct;
                onStatus(L"上传中 " + std::to_wstring(pct) + L"%（"
                    + std::to_wstring((body.size() + 1023) / 1024) + L" KB）…");
            }
        }
    }
    return true;
}

bool IsTransientHttpReceiveError(DWORD err) {
    if (err == 0) return true;
    if (err >= 12000 && err <= 12180) return true;
    return err == ERROR_WINHTTP_TIMEOUT
        || err == ERROR_WINHTTP_CONNECTION_ERROR
        || err == ERROR_OPERATION_ABORTED
        || err == ERROR_INVALID_HANDLE
        || err == 12119; // ERROR_WINHTTP_INVALID_OPERATION
}

// 调用方显式设置了 recvTimeoutMs 时必须尊重，禁止再用 maxTokens 抬到数分钟
// （否则 locate 设 45s 会被 maxTokens≈10800 抬成 270s，表现为「点浏览卡死」）。
int ComputeEffectiveRecvTimeoutMs(int recvTimeoutMs, int maxTokens) {
    if (recvTimeoutMs > 0)
        return std::max(15000, recvTimeoutMs);
    const int baseMs = 90000;
    const int scaledMs = maxTokens > 0 ? maxTokens * 25 : 0;
    return std::max(std::max(15000, baseMs), scaledMs);
}

// 按模型调整 max_tokens 上限：不同模型输出上限差异很大，
// 用户配置超限时直接 clamp 到该模型常见上限，避免 400。
int ClampApiMaxTokens(int value, const std::wstring& model) {
    int maxCap = 393216;  // DeepSeek V3.1 等长输出模型
    const std::wstring lower = Trim(model);
    std::wstring low;
    low.reserve(lower.size());
    for (wchar_t c : lower) low.push_back(static_cast<wchar_t>(std::towlower(c)));
    if (low.find(L"deepseek") != std::wstring::npos) maxCap = 32768;
    else if (low.find(L"gpt-4o") != std::wstring::npos
        || low.find(L"gpt-4.1") != std::wstring::npos) maxCap = 16384;
    else if (low.find(L"o1") != std::wstring::npos
        || low.find(L"o3") != std::wstring::npos
        || low.find(L"o4") != std::wstring::npos) maxCap = 100000;
    if (value < 1) return 4096;
    return std::min(value, maxCap);
}

// 需要强制关闭默认深度思考的网关：DeepSeek 官方、火山方舟（豆包 seed/pro 等）。
// 实测这些模型默认/显式 enabled 都会长思考（单轮数十秒~数分钟），
// 而 thinking.type=disabled 可把单轮压到 1~3s 且保持正确流程。
bool ShouldForceFastThinking(const std::wstring& apiUrl, const std::wstring& model) {
    const std::wstring lowUrl = Trim(apiUrl);
    std::wstring url;
    url.reserve(lowUrl.size());
    for (wchar_t c : lowUrl) url.push_back(static_cast<wchar_t>(std::towlower(c)));
    if (url.find(L"deepseek") != std::wstring::npos) return true;
    if (url.find(L"volces.com") == std::wstring::npos
        && url.find(L"ark") == std::wstring::npos) {
        return false;
    }
    // 方舟网关：只对思考型模型（seed/pro/max/ultra）下发，避免其它模型拒参。
    const std::wstring lowModel = Trim(model);
    std::wstring m;
    m.reserve(lowModel.size());
    for (wchar_t c : lowModel) m.push_back(static_cast<wchar_t>(std::towlower(c)));
    return m.find(L"seed") != std::wstring::npos
        || m.find(L"pro") != std::wstring::npos
        || m.find(L"max") != std::wstring::npos
        || m.find(L"ultra") != std::wstring::npos;
}

// 指定范围内的对话是否已调用过 readAgentSkill section=scriptStrategy。
// 用于工具层强制「先读脚本生成规范，再构建/创建」，避免模型逐条翻参考绕圈。
bool SkillScriptStrategyRead(const std::vector<ChatMessage>& history, size_t endIndex) {
    const size_t limit = (std::min)(endIndex, history.size());
    for (size_t i = 0; i < limit; ++i) {
        const auto& m = history[i];
        if (m.role != L"assistant") continue;
        for (const auto& tc : m.tool_calls) {
            if (tc.name == L"readAgentSkill"
                && tc.arguments.find(L"scriptStrategy") != std::wstring::npos) {
                return true;
            }
        }
    }
    return false;
}

bool ReceiveResponseWithPolling(HINTERNET hRequest, const std::atomic_bool* cancelFlag,
                                int maxWaitMs, std::wstring* errorOut,
                                StatusCallback onStatus = nullptr) {
    DWORD pollMs = kHttpReceivePollMs;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &pollMs, sizeof(pollMs));
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max(5000, maxWaitMs));
    const auto waitStart = std::chrono::steady_clock::now();
    int lastReportSec = 0;
    DWORD lastErr = 0;
    for (;;) {
        if (cancelFlag && cancelFlag->load()) {
            if (errorOut) *errorOut = L"已取消";
            return false;
        }
        const int waitedSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - waitStart).count());
        if (onStatus && waitedSec >= lastReportSec + 5) {
            lastReportSec = waitedSec - (waitedSec % 5);
            const int remainSec = std::max(0, maxWaitMs / 1000 - waitedSec);
            onStatus(L"等待响应 " + std::to_wstring(waitedSec) + L"s（剩余约 "
                + std::to_wstring(remainSec) + L"s）…");
        }
        if (WinHttpReceiveResponse(hRequest, nullptr)) return true;
        lastErr = GetLastError();
        if (cancelFlag && cancelFlag->load()) {
            if (errorOut) *errorOut = L"已取消";
            return false;
        }
        if (IsTransientHttpReceiveError(lastErr)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                if (errorOut) {
                    *errorOut = L"等待服务器响应超时（已超过 "
                        + std::to_wstring(maxWaitMs) + L" ms，最后错误="
                        + WinHttpErrorText(lastErr)
                        + L" code=" + std::to_wstring(lastErr) + L"）";
                }
                if (onStatus)
                    onStatus(L"接收超时: " + WinHttpErrorText(lastErr) + L" (code=" + std::to_wstring(lastErr) + L")");
                return false;
            }
            continue;
        }
        // 非临时错误 — 立即返回
        if (errorOut) {
            *errorOut = L"接收响应失败：" + WinHttpErrorText(lastErr)
                + L" (code=" + std::to_wstring(lastErr) + L")";
        }
        if (onStatus)
            onStatus(L"接收失败(非临时): " + WinHttpErrorText(lastErr) + L" (code=" + std::to_wstring(lastErr) + L")");
        return false;
    }
}

bool HostLooksValid(const std::wstring& host) {
    if (host.empty() || host.find(L' ') != std::wstring::npos) return false;
    if (host.find(L"://") != std::wstring::npos) return false;
    return true;
}

std::wstring NormalizeChatCompletionsUrl(std::wstring url) {
    url = Trim(url);
    while (!url.empty() && url.back() == L'/') url.pop_back();
    if (url.empty()) return url;
    if (url.find(L"chat/completions") != std::wstring::npos) return url;

    const size_t schemeEnd = url.find(L"://");
    if (schemeEnd == std::wstring::npos) return url;
    const size_t hostStart = schemeEnd + 3;
    const size_t pathStart = url.find(L'/', hostStart);
    const std::wstring origin = pathStart != std::wstring::npos ? url.substr(0, pathStart) : url;
    const std::wstring path = pathStart != std::wstring::npos ? url.substr(pathStart) : L"";

    if (path.empty() || path == L"/") return origin + L"/v1/chat/completions";
    if (path == L"/v1") return origin + L"/v1/chat/completions";
    return url + L"/chat/completions";
}

void FillAssistantFromApiMessage(ChatMessage& dst, const json& message) {
    dst.tool_calls.clear();
    if (message.contains("content") && !message["content"].is_null()) {
        if (message["content"].is_string())
            dst.content = FromUtf8(message["content"].get<std::string>());
    } else {
        dst.content.clear();
    }
    auto mergeReasoning = [&](const char* key) {
        if (!message.contains(key) || !message[key].is_string()) return;
        const std::wstring piece = FromUtf8(message[key].get<std::string>());
        if (piece.empty()) return;
        dst.requires_reasoning_content = true;
        if (!dst.reasoning_content.empty()) dst.reasoning_content += L"\n";
        dst.reasoning_content += piece;
    };
    mergeReasoning("reasoning_content");
    mergeReasoning("reasoning");
    mergeReasoning("thinking");
    mergeReasoning("thought");
    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        for (const json& tc : message["tool_calls"]) {
            ToolCallRecord rec;
            rec.id = FromUtf8(tc.value("id", ""));
            const json& func = tc.value("function", json::object());
            rec.name = FromUtf8(func.value("name", ""));
            rec.arguments = FromUtf8(func.value("arguments", "{}"));
            dst.tool_calls.push_back(std::move(rec));
        }
    }
}

// ── 工具循环治理辅助 ──────────────────────────────────────────────
// 终态工具：脚本/录制创建保存、定时任务创建/更新。这些工具成功即代表
// 用户目标达成，立即收尾，避免模型在「已创建」后仍反复查参考绕圈。
bool IsTerminalToolName(const std::wstring& name) {
    return name == L"createMacroScript" || name == L"writeScript"
        || name == L"optimizeScript" || name == L"optimizeRecording"
        || name == L"createScheduledTask" || name == L"updateScheduledTask";
}

// 判定工具结果是否表示成功（而非报错）。
bool ToolResultIsSuccess(const std::wstring& result) {
    if (result.find(L"[错误]") != std::wstring::npos) return false;
    return result.find(L"✓") != std::wstring::npos
        || result.find(L"已创建") != std::wstring::npos
        || result.find(L"已保存") != std::wstring::npos
        || result.find(L"已更新") != std::wstring::npos;
}

}  // namespace

// ── 构造 / 析构 ───────────────────────────────────────────────────
AgentCore::AgentCore(const AgentConfig& config,
                     const std::wstring& systemPrompt,
                     const std::vector<AgentTool>& tools)
    : config_(config), tools_(tools) {
    ChatMessage sysMsg;
    sysMsg.role = L"system";
    sysMsg.content = systemPrompt;
    messages_.push_back(sysMsg);
}

void AgentCore::ClearHistory() {
    if (!messages_.empty() && messages_[0].role == L"system") {
        messages_.erase(messages_.begin() + 1, messages_.end());
    }
}

void AgentCore::SetFullHistory(std::vector<ChatMessage> messages) {
    messages_ = std::move(messages);
}

bool AgentCore::TruncateHistoryToUserRound(size_t userRoundIndex) {
    size_t seen = 0;
    size_t cut = messages_.size();
    for (size_t i = 0; i < messages_.size(); ++i) {
        // 内部引导消息（nudge/图片参考）不是真正的用户轮次，不计入序号
        if (messages_[i].role == L"user" && !messages_[i].internal_nudge) {
            if (seen == userRoundIndex) {
                cut = i;
                break;
            }
            ++seen;
        }
    }
    if (cut == messages_.size()) return false;
    messages_.resize(cut);
    return true;
}

void AgentCore::ImportHistoryFrom(const AgentCore& other, size_t startIndex) {
    const auto& src = other.messages_;
    if (startIndex >= src.size()) return;
    messages_.insert(messages_.end(), src.begin() + static_cast<std::ptrdiff_t>(startIndex), src.end());
}

void AgentCore::UpdateConfig(const AgentConfig& config, const std::wstring& systemPrompt) {
    config_ = config;
    if (!messages_.empty() && messages_[0].role == L"system") {
        messages_[0].content = systemPrompt;
    } else {
        ChatMessage sysMsg;
        sysMsg.role = L"system";
        sysMsg.content = systemPrompt;
        messages_.insert(messages_.begin(), sysMsg);
    }
}

void AgentCore::UpdateTools(const std::vector<AgentTool>& tools) {
    tools_ = tools;
}

void AgentCore::AbortActiveHttp() {
    if (activeHttpAbort_) activeHttpAbort_->Abort();
}

// ── 构建 API 请求 ─────────────────────────────────────────────────
json AgentCore::BuildRequest(bool stripLastUserImages,
                             const std::string& toolChoice) {
    json req;
    req["model"] = ToUtf8(config_.model);
    // 推理模型（o1/o3/o4、DeepSeek R1/Reasoner 等）不接受 temperature 采样参数，
    // 硬发会 400；非推理模型照常下发。
    if (!ModelIsReasoningType(config_.model)) {
        req["temperature"] = config_.temperature;
    }
    req["max_tokens"] = ClampApiMaxTokens(config_.maxTokens, config_.model);

    // 上下文滑动窗口：只保留 system + 最近 kContextKeepRounds 个 user 轮
    // （含随后的 tool/assistant），丢弃更早轮次，避免长对话撑爆上下文。
    // 以 user 消息为轮次边界裁剪，保证 tool_call_id 配对完整、序列合法。
    static constexpr size_t kContextKeepRounds = 10;
    size_t startIdx = 0;
    {
        size_t userCount = 0;
        for (const auto& m : messages_) {
            if (m.role == L"user") ++userCount;
        }
        if (userCount > kContextKeepRounds) {
            size_t toSkip = userCount - kContextKeepRounds;
            for (size_t i = 1; i < messages_.size(); ++i) {
                if (messages_[i].role == L"user") {
                    if (toSkip > 0) {
                        --toSkip;
                        startIdx = i + 1;
                    } else {
                        break;
                    }
                }
            }
        }
    }

    size_t lastUserIdx = messages_.size();
    for (size_t mi = 0; mi < messages_.size(); ++mi) {
        if (mi != 0 && mi < startIdx) continue;
        if (messages_[mi].role == L"user") lastUserIdx = mi;
    }

    // 构建 messages 数组（system 恒保留；裁剪轮次跳过）
    json msgs = json::array();
    for (size_t mi = 0; mi < messages_.size(); ++mi) {
        if (mi != 0 && mi < startIdx) continue;
        const auto& m = messages_[mi];
        const bool stripImages = (m.role == L"user" && mi != lastUserIdx)
            || (stripLastUserImages && m.role == L"user" && mi == lastUserIdx);
        const bool stripThis = stripImages && !m.keep_images;
        json msg;
        msg["role"] = ToUtf8(m.role);
        if (!m.parts.empty()) {
            json parts = json::array();
            for (const auto& p : m.parts) {
                if (p.type == L"text") {
                    parts.push_back({{"type", "text"}, {"text", ToUtf8(p.text)}});
                } else if (p.type == L"image_url") {
                    if (stripThis) {
                        const char* hint = (stripLastUserImages && mi == lastUserIdx)
                            ? "(截图已在上一轮流式请求中发送，请根据文字描述直接调用工具完成脚本，勿重复长篇思考)"
                            : "(历史截图已省略)";
                        parts.push_back({{"type", "text"}, {"text", hint}});
                    } else {
                        parts.push_back({
                            {"type", "image_url"},
                            {"image_url", {{"url", ToUtf8(p.image_url)}}}
                        });
                    }
                }
            }
            msg["content"] = parts;
        } else if (!m.content.empty()) {
            msg["content"] = ToUtf8(m.content);
        } else if (!m.tool_calls.empty()) {
            msg["content"] = nullptr;
        } else {
            msg["content"] = "";
        }
        if (m.requires_reasoning_content)
            msg["reasoning_content"] = ToUtf8(m.reasoning_content);
        else if (!m.reasoning_content.empty())
            msg["reasoning_content"] = ToUtf8(m.reasoning_content);
        if (!m.tool_call_id.empty())
            msg["tool_call_id"] = ToUtf8(m.tool_call_id);
        if (!m.tool_calls.empty()) {
            json tcs = json::array();
            for (const auto& tc : m.tool_calls) {
                json func;
                func["name"] = ToUtf8(tc.name);
                func["arguments"] = ToUtf8(tc.arguments);
                json item;
                item["id"] = ToUtf8(tc.id);
                item["type"] = "function";
                item["function"] = func;
                tcs.push_back(item);
            }
            msg["tool_calls"] = tcs;
        } else if (!m.tool_name.empty()) {
            json func;
            func["name"] = ToUtf8(m.tool_name);
            if (!m.tool_args.empty())
                func["arguments"] = ToUtf8(m.tool_args);
            json tc;
            tc["id"] = "call_" + std::to_string(msgs.size());
            tc["type"] = "function";
            tc["function"] = func;
            msg["tool_calls"] = json::array({ tc });
        }
        msgs.push_back(msg);
    }
    req["messages"] = msgs;

    // 构建 tools 数组
    if (!tools_.empty()) {
        json toolsArray = json::array();
        for (const auto& t : tools_) {
            json tool;
            tool["type"] = "function";
            tool["function"]["name"] = ToUtf8(t.name);
            tool["function"]["description"] = ToUtf8(t.description);
            if (!t.parameters_json.empty()) {
                try {
                    tool["function"]["parameters"] = json::parse(ToUtf8(t.parameters_json));
                } catch (const json::parse_error&) {
                    // 参数 schema 解析失败时回退为最简 schema，确保工具仍能被发送
                    tool["function"]["parameters"] = json::object(
                        {{"type", "object"}, {"properties", json::object()}}
                    );
                }
            } else {
                tool["function"]["parameters"] = json::object(
                    {{"type", "object"}, {"properties", json::object()}}
                );
            }
            toolsArray.push_back(tool);
        }
        req["tools"] = toolsArray;
        // OpenAI/兼容网关：auto|required|none。未指定（auto）时不发字段——
        // 缺省即 auto，且 DeepSeek 深度思考模式会对 tool_choice=required 报 400。
        if (!toolChoice.empty() && toolChoice != "auto")
            req["tool_choice"] = toolChoice;
    }
    // DeepSeek V4 / 豆包 seed 等思考型模型：默认就会长思考（单轮可达数分钟，
    // 还受 max_tokens 预算放大），表现为「卡死」。助手流程的正确性由
    // Skill + 工具校验保证（实测关思考后 3s 内正确调出 readAgentSkill →
    // planScriptActions → createMacroScript），因此一律显式 disabled 走快速执行。
    // 原生推理模型（R1/Reasoner、o 系列）不接受该开关，不发送避免 400；
    // 其它网关（OpenAI 等）保持默认。
    if (ShouldForceFastThinking(config_.apiUrl, config_.model)
        && !ModelIsReasoningType(config_.model)) {
        req["thinking"] = json::object({{"type", "disabled"}});
    }

    return req;
}

// ── WinHTTP API 调用 ──────────────────────────────────────────────
std::wstring AgentCore::CallApi(const json& requestBody, std::wstring* errorOut,
                                const std::atomic_bool* cancelFlag,
                                AiHttpAbortSlot* httpAbort,
                                StatusCallback onStatus) {
    const std::wstring url = NormalizeChatCompletionsUrl(Trim(config_.apiUrl));
    const ParsedUrl parsed = ParseUrl(url);
    if (parsed.host.empty() || !HostLooksValid(parsed.host)) {
        if (errorOut) *errorOut = L"API 地址格式无效，请检查是否包含完整的 https:// 地址。";
        return L"";
    }
    if (Trim(config_.apiUrl).find(L"://") != Trim(config_.apiUrl).rfind(L"://")) {
        if (errorOut) *errorOut = L"API 地址格式无效（检测到重复协议），请重新填写。";
        return L"";
    }

    const std::string body = requestBody.dump();
    const int effectiveTimeoutMs = ComputeEffectiveRecvTimeoutMs(config_.recvTimeoutMs, config_.maxTokens);
    if (onStatus) {
        std::wstring st = L"正在连接 API（请求体 "
            + std::to_wstring((body.size() + 1023) / 1024) + L" KB）…";
        if (requestBody.contains("thinking")
            && requestBody["thinking"].is_object()
            && requestBody["thinking"].value("type", "") == "enabled") {
            st += L"（已开深度思考）";
        }
        onStatus(st);
    }

    WinHttpHandle hSession = WinHttpOpen(
        L"QuickScriptTool/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        if (errorOut) *errorOut = L"WinHttpOpen 失败：" + WinHttpErrorText(GetLastError());
        return L"";
    }

    DWORD connectTimeout = 30000;
    DWORD sendTimeout = body.size() > 300000 ? 180000u : 120000u;
    DWORD recvTimeout = static_cast<DWORD>(std::max(5000, effectiveTimeoutMs));
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &connectTimeout, sizeof(connectTimeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &sendTimeout, sizeof(sendTimeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &recvTimeout, sizeof(recvTimeout));

    WinHttpHandle hConnect = WinHttpConnect(hSession, parsed.host.c_str(), parsed.port, 0);
    if (!hConnect) {
        if (errorOut) *errorOut = L"无法连接服务器 " + parsed.host + L"：" + WinHttpErrorText(GetLastError());
        return L"";
    }

    WinHttpHandle hRequest = WinHttpOpenRequest(
        hConnect, L"POST", parsed.path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parsed.isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        if (errorOut) *errorOut = L"WinHttpOpenRequest 失败：" + WinHttpErrorText(GetLastError());
        return L"";
    }

    auto fail = [&](const std::wstring& msg) -> std::wstring {
        hRequest.Detach();
        if (errorOut) *errorOut = msg;
        return L"";
    };

    if (httpAbort) httpAbort->Set(hRequest);
    struct HttpRequestGuard {
        AiHttpAbortSlot* slot;
        ~HttpRequestGuard() { if (slot) slot->Clear(); }
    } httpGuard{httpAbort};

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!config_.apiKey.empty())
        headers += L"Authorization: Bearer " + config_.apiKey + L"\r\n";

    if (!HttpSendJsonBody(hRequest, headers, body, cancelFlag, errorOut, onStatus))
        return fail(errorOut ? *errorOut : L"发送请求失败");

    if (cancelFlag && cancelFlag->load()) return fail(L"已取消");

    if (onStatus) {
        onStatus(L"等待服务器响应（最长 "
            + std::to_wstring(std::max(30, effectiveTimeoutMs / 1000)) + L"s，推理模型可能较慢）…");
    }
    std::wstring recvErr;
    if (!ReceiveResponseWithPolling(hRequest, cancelFlag, effectiveTimeoutMs, &recvErr, onStatus))
        return fail(recvErr.empty() ? L"等待服务器响应失败" : recvErr);

    if (onStatus) onStatus(L"已收到响应头，读取响应体…");

    DWORD pollRecvTimeoutMs = kHttpBodyPollMs;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &pollRecvTimeoutMs, sizeof(pollRecvTimeoutMs));

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    // 读取响应体必须带总超时：推理模型生成可能长达数分钟，若服务器持续慢速吐数据，
    // 无 deadline 的循环会无限等待（表现为“半天没响应”）。
    const auto bodyStart = std::chrono::steady_clock::now();
    const auto bodyDeadline = bodyStart + std::chrono::milliseconds(effectiveTimeoutMs);
    std::string responseBody;
    DWORD bytesAvailable = 0;
    int lastBodyBeatSec = 0;
    for (;;) {
        if (cancelFlag && cancelFlag->load()) return fail(L"已取消");
        const auto now = std::chrono::steady_clock::now();
        if (now >= bodyDeadline) {
            if (onStatus)
                onStatus(L"读取响应体超时（已超过 "
                    + std::to_wstring(effectiveTimeoutMs / 1000)
                    + L"s），中止等待…");
            return fail(L"读取响应体超时（服务器长时间未完成响应）");
        }
        if (onStatus) {
            const int waitedSec = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(now - bodyStart).count());
            if (waitedSec >= lastBodyBeatSec + 5) {
                lastBodyBeatSec = waitedSec;
                onStatus(L"等待响应体 " + std::to_wstring(waitedSec)
                    + L"s（已接收 " + std::to_wstring(responseBody.size() / 1024)
                    + L" KB）…");
            }
        }
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
            const DWORD err = GetLastError();
            if (IsTransientHttpReceiveError(err)) {
                if (cancelFlag && cancelFlag->load()) return fail(L"已取消");
                continue;
            }
            break;
        }
        if (bytesAvailable == 0) break;
        std::vector<char> buffer(bytesAvailable);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) && bytesRead > 0)
            responseBody.append(buffer.data(), bytesRead);
        if (cancelFlag && cancelFlag->load()) return fail(L"已取消");
    }

    const std::wstring text = FromUtf8(responseBody);
    if (statusCode >= 400) {
        std::wstring detail = text.empty() ? L"(无响应正文)" : text;
        if (detail.size() > 400) detail = detail.substr(0, 400) + L"...";
        return fail(L"HTTP " + std::to_wstring(statusCode) + L" " + parsed.path + L"：" + detail
            + L"\n（实际请求: " + url + L"）");
    }
    if (text.empty())
        return fail(L"服务器返回空响应。");
    return text;
}

struct StreamAccumState {
    std::string reasoning;
    std::string content;
    std::vector<json> toolCallParts;
    std::string lineBuffer;
    std::string finishReason;
    bool done = false;
    bool hasToolCalls = false;
    bool contentDeltaSent = false;
};

bool StreamToolCallArgsComplete(const json& fn) {
    if (!fn.contains("arguments") || !fn["arguments"].is_string()) return false;
    const std::string args = fn["arguments"].get<std::string>();
    if (args.empty()) return false;
    try {
        (void)json::parse(args);
        return true;
    } catch (...) {
        return false;
    }
}

bool HasUsableStreamToolCalls(const StreamAccumState& state) {
    if (state.toolCallParts.empty()) return false;
    for (const json& tc : state.toolCallParts) {
        if (tc.is_null() || tc.empty()) continue;
        const json& fn = tc.value("function", json::object());
        if (!fn.contains("name") || !fn["name"].is_string()) return false;
        if (!StreamToolCallArgsComplete(fn)) return false;
    }
    return true;
}

void MaybeMarkStreamDone(StreamAccumState& state) {
    if (state.finishReason == "tool_calls") {
        if (HasUsableStreamToolCalls(state)) state.done = true;
        return;
    }
    if (state.finishReason == "stop" || state.finishReason == "length")
        state.done = true;
}

bool HasMeaningfulStreamPayload(const StreamAccumState& state) {
    return !state.reasoning.empty() || !state.content.empty() || HasUsableStreamToolCalls(state);
}

bool ShouldFinalizeStream(const StreamAccumState& state, bool expectTools) {
    if (state.done) return true;
    if (expectTools) return HasUsableStreamToolCalls(state);
    return !state.content.empty() || !state.reasoning.empty();
}

bool AssembleStreamMessage(const StreamAccumState& state, bool expectTools, ChatMessage& outMsg) {
    if (!ShouldFinalizeStream(state, expectTools)) return false;

    json assembled;
    assembled["role"] = "assistant";
    assembled["content"] = state.content.empty() ? json(nullptr) : json(state.content);
    if (!state.reasoning.empty())
        assembled["reasoning_content"] = state.reasoning;
    // 只挂载参数已完整的 tool_calls，避免半截 JSON 阻断「改走强制调工具」路径
    if (HasUsableStreamToolCalls(state)) {
        json tcs = json::array();
        for (const json& tc : state.toolCallParts) {
            if (!tc.is_null() && !tc.empty()) tcs.push_back(tc);
        }
        if (!tcs.empty()) assembled["tool_calls"] = tcs;
    }

    outMsg.role = L"assistant";
    FillAssistantFromApiMessage(outMsg, assembled);
    return true;
}

void MergeStreamToolCallDelta(std::vector<json>& parts, const json& deltaToolCalls) {
    if (!deltaToolCalls.is_array()) return;
    for (const json& tc : deltaToolCalls) {
        const int index = tc.value("index", static_cast<int>(parts.size()));
        while (static_cast<int>(parts.size()) <= index)
            parts.push_back(json::object());
        json& dst = parts[static_cast<size_t>(index)];
        if (tc.contains("id") && tc["id"].is_string())
            dst["id"] = tc["id"];
        if (tc.contains("type") && tc["type"].is_string())
            dst["type"] = tc["type"];
        if (tc.contains("function")) {
            if (!dst.contains("function")) dst["function"] = json::object();
            const json& fn = tc["function"];
            if (fn.contains("name") && fn["name"].is_string())
                dst["function"]["name"] = fn["name"];
            if (fn.contains("arguments") && fn["arguments"].is_string()) {
                std::string prev = dst["function"].value("arguments", "");
                prev += fn["arguments"].get<std::string>();
                dst["function"]["arguments"] = prev;
            }
        }
    }
}

bool ProcessStreamSseLine(const std::string& line, StreamAccumState& state,
    const AgentSendCallbacks& callbacks) {
    if (line.empty()) return true;
    if (line.rfind("data:", 0) != 0) return true;
    std::string payload = line.substr(5);
    while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\t'))
        payload.erase(payload.begin());
    if (payload.empty() || payload == "[DONE]") {
        state.done = true;
        return true;
    }
    json chunk;
    try {
        chunk = json::parse(payload);
    } catch (...) {
        return true;
    }
    if (chunk.contains("error")) {
        state.done = true;
        return false;
    }
    if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].empty())
        return true;
    const json& choice = chunk["choices"][0];
    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
        state.finishReason = choice["finish_reason"].get<std::string>();
        MaybeMarkStreamDone(state);
    }
    const json& delta = choice.value("delta", json::object());
    auto appendReasoning = [&](const json& obj, const char* key) {
        if (!obj.contains(key) || !obj[key].is_string()) return;
        const std::string piece = obj[key].get<std::string>();
        if (piece.empty()) return;
        state.reasoning += piece;
        if (callbacks.onReasoningDelta)
            callbacks.onReasoningDelta(FromUtf8(piece));
    };
    appendReasoning(delta, "reasoning_content");
    appendReasoning(delta, "reasoning");
    appendReasoning(delta, "thinking");
    appendReasoning(delta, "thought");
    if (delta.contains("content") && delta["content"].is_string()) {
        const std::string piece = delta["content"].get<std::string>();
        if (!piece.empty()) {
            state.content += piece;
            if (callbacks.onContentDelta) {
                state.contentDeltaSent = true;
                callbacks.onContentDelta(FromUtf8(piece));
            }
        }
    }
    if (delta.contains("tool_calls")) {
        state.hasToolCalls = true;
        MergeStreamToolCallDelta(state.toolCallParts, delta["tool_calls"]);
        if (state.finishReason == "tool_calls") MaybeMarkStreamDone(state);
    }
    if (choice.contains("message") && choice["message"].is_object()) {
        const json& msg = choice["message"];
        appendReasoning(msg, "reasoning_content");
        appendReasoning(msg, "reasoning");
        appendReasoning(msg, "thinking");
        appendReasoning(msg, "thought");
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            state.hasToolCalls = true;
            for (const json& tc : msg["tool_calls"]) state.toolCallParts.push_back(tc);
            if (state.finishReason.empty()) state.finishReason = "tool_calls";
            MaybeMarkStreamDone(state);
        }
        if (msg.contains("content") && msg["content"].is_string()) {
            const std::string piece = msg["content"].get<std::string>();
            if (!piece.empty()) {
                state.content += piece;
                if (callbacks.onContentDelta) {
                    state.contentDeltaSent = true;
                    callbacks.onContentDelta(FromUtf8(piece));
                }
            }
        }
    }
    return true;
}

void FeedStreamBytes(StreamAccumState& state, const char* data, size_t len,
    const AgentSendCallbacks& callbacks) {
    state.lineBuffer.append(data, len);
    for (;;) {
        const size_t pos = state.lineBuffer.find('\n');
        if (pos == std::string::npos) break;
        std::string line = state.lineBuffer.substr(0, pos);
        state.lineBuffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ProcessStreamSseLine(line, state, callbacks);
        if (state.done) break;
    }
}

AgentCore::StreamApiResult AgentCore::CallApiStream(const json& requestBodyIn,
    const AgentSendCallbacks& callbacks) {
    StreamApiResult result;
    auto fail = [&](const std::wstring& msg) {
        result.error = msg;
        return result;
    };

    json requestBody = requestBodyIn;
    requestBody["stream"] = true;

    const int effectiveTimeoutMs = ComputeEffectiveRecvTimeoutMs(config_.recvTimeoutMs, config_.maxTokens);
    // 长等待只发生在流式路径：模型边想边吐工具 JSON 极慢时会「工具调用组装中」空挂满 51s，
    // 再叠加完整响应重试共 ~100s。宿主对工具轮改用更短的组装/空闲上限，超时即走
    // 完整响应兜底（日志显示兜底响应通常很快），把单轮最坏等待压到 ~30s。
    constexpr int kStreamIdleTimeoutMs = 30000;
    constexpr int kToolCallAssemblyCapMs = 30000;
    // 兜底：仅在长时间空闲仍无 tool_calls 时收束（勿在数秒内打断，否则永远调不到工具）
    constexpr int kReasoningOnlyForceMs = 90000;
    constexpr int kReasoningPlateauIdleMs = 45000;
    // 工具轮：已连上/已有字节但迟迟无思考/正文/工具（常见于 SSE keepalive 空挂）→ 尽快改完整响应
    // 勿等满 API 超时（日志里会一直「流式等待 3s…42s」）
    constexpr int kEmptyStreamCapMs = 15000;

    const std::wstring url = NormalizeChatCompletionsUrl(Trim(config_.apiUrl));
    const ParsedUrl parsed = ParseUrl(url);
    if (parsed.host.empty() || !HostLooksValid(parsed.host))
        return fail(L"API 地址格式无效，请检查是否包含完整的 https:// 地址。");

    const std::string body = requestBody.dump();
    // 空流上限按请求体大小自适应：几十 KB 历史+工具 schema+截图时，
    // 模型预填充阶段没有首包是正常的（可达 30~60s）；固定 15s 会误杀流式，
    // 逼到更慢的完整响应（表现为「等待响应体 Ns（已接收 0 KB）」干等）。
    int emptyStreamCapMs = kEmptyStreamCapMs;
    if (body.size() > 20000) {
        const int extra = static_cast<int>((body.size() - 20000) / 1024) * 600;
        emptyStreamCapMs = std::min(60000, kEmptyStreamCapMs + extra);
    }
    if (callbacks.onStatus) {
        std::wstring st = L"正在连接 API（请求体 "
            + std::to_wstring((body.size() + 1023) / 1024) + L" KB）…";
        if (requestBody.contains("thinking")
            && requestBody["thinking"].is_object()
            && requestBody["thinking"].value("type", "") == "enabled") {
            st += L"（已开深度思考）";
        }
        callbacks.onStatus(st);
    }

    WinHttpHandle hSession = WinHttpOpen(
        L"QuickScriptTool/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return fail(L"WinHttpOpen 失败：" + WinHttpErrorText(GetLastError()));

    DWORD connectTimeout = 30000;
    DWORD sendTimeout = body.size() > 300000 ? 180000u : 120000u;
    DWORD recvTimeout = static_cast<DWORD>(std::max(5000, effectiveTimeoutMs));
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &connectTimeout, sizeof(connectTimeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &sendTimeout, sizeof(sendTimeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &recvTimeout, sizeof(recvTimeout));

    WinHttpHandle hConnect = WinHttpConnect(hSession, parsed.host.c_str(), parsed.port, 0);
    if (!hConnect)
        return fail(L"无法连接服务器 " + parsed.host + L"：" + WinHttpErrorText(GetLastError()));

    WinHttpHandle hRequest = WinHttpOpenRequest(
        hConnect, L"POST", parsed.path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parsed.isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest)
        return fail(L"WinHttpOpenRequest 失败：" + WinHttpErrorText(GetLastError()));

    if (callbacks.httpAbort) callbacks.httpAbort->Set(hRequest);
    struct HttpRequestGuard {
        AiHttpAbortSlot* slot;
        ~HttpRequestGuard() { if (slot) slot->Clear(); }
    } httpGuard{callbacks.httpAbort};

    std::wstring headers = L"Content-Type: application/json\r\nAccept: text/event-stream\r\n";
    if (!config_.apiKey.empty())
        headers += L"Authorization: Bearer " + config_.apiKey + L"\r\n";

    std::wstring sendErr;
    if (!HttpSendJsonBody(hRequest, headers, body, callbacks.cancelFlag, &sendErr, callbacks.onStatus))
        return fail(sendErr.empty() ? L"发送请求失败" : sendErr);

    if (callbacks.cancelFlag && callbacks.cancelFlag->load()) return fail(L"已取消");

    std::wstring recvErr;
    if (!ReceiveResponseWithPolling(hRequest, callbacks.cancelFlag, effectiveTimeoutMs,
            &recvErr, callbacks.onStatus))
        return fail(recvErr.empty() ? L"接收响应失败" : recvErr);

    if (callbacks.cancelFlag && callbacks.cancelFlag->load()) return fail(L"已取消");

    DWORD pollRecvTimeoutMs = kHttpBodyPollMs;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &pollRecvTimeoutMs, sizeof(pollRecvTimeoutMs));

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    StreamAccumState state;
    const bool expectTools = !tools_.empty();
    DWORD bytesAvailable = 0;
    const auto streamDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(effectiveTimeoutMs);
    const auto streamStart = std::chrono::steady_clock::now();
    auto lastByteTime = streamStart;
    bool receivedAnyByte = false;
    bool announcedStreamConnected = false;
    int lastBeatSec = 0;

    // 必须在「QueryDataAvailable 超时 continue」路径也会跑：否则无数据时永久跳过心跳/强制收束
    auto checkStreamProgress = [&]() -> bool {
        const auto now = std::chrono::steady_clock::now();
        const int waitedSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
            now - streamStart).count());
        if (callbacks.onStatus && waitedSec >= lastBeatSec + 3) {
            lastBeatSec = waitedSec;
            std::wstring beat = L"流式等待 " + std::to_wstring(waitedSec) + L"s";
            if (!state.reasoning.empty())
                beat += L"，思考 " + std::to_wstring(state.reasoning.size()) + L" 字节";
            if (!state.content.empty())
                beat += L"，回复 " + std::to_wstring(state.content.size()) + L" 字节";
            if (state.hasToolCalls || !state.toolCallParts.empty())
                beat += L"，工具调用组装中";
            callbacks.onStatus(beat + L"…");
        }
        if (expectTools
            && state.content.empty()
            && !HasUsableStreamToolCalls(state)
            && !state.reasoning.empty()) {
            const bool hitAbs = now - streamStart >= std::chrono::milliseconds(kReasoningOnlyForceMs);
            const bool hitPlateau = receivedAnyByte
                && now - lastByteTime >= std::chrono::milliseconds(kReasoningPlateauIdleMs);
            if (hitAbs || hitPlateau) {
                if (callbacks.onStatus) {
                    callbacks.onStatus(hitPlateau
                        ? L"思考已停顿仍未调用工具，结束流式并强制调工具…"
                        : L"思考过久未调用工具，结束流式并强制调工具…");
                }
                state.done = true;
                return true;
            }
        }
        // 工具轮：已在「工具调用组装中」但迟迟凑不齐完整 tool_calls → 放弃流式，
        // 改走完整响应兜底（比继续空挂快）。
        if (expectTools
            && (state.hasToolCalls || !state.toolCallParts.empty())
            && !HasUsableStreamToolCalls(state)
            && now - streamStart >= std::chrono::milliseconds(kToolCallAssemblyCapMs)) {
            if (callbacks.onStatus)
                callbacks.onStatus(L"工具调用组装超时，改用完整响应…");
            return false; // 调用方 fail → 上层改用完整响应
        }
        // 工具轮空流：无思考/正文/可用工具。含「已收到字节但只是 ping/空行」——原先只拦
        // !receivedAnyByte，keepalive 会空挂到整段 API 超时。
        if (expectTools
            && !HasMeaningfulStreamPayload(state)
            && now - streamStart >= std::chrono::milliseconds(emptyStreamCapMs)) {
            if (callbacks.onStatus)
                callbacks.onStatus(receivedAnyByte
                    ? L"流式空闲无内容（可能仅 keepalive），改用完整响应…"
                    : L"流式首包超时，改用完整响应…");
            return false;
        }
        // 非工具轮：仍要求尽快有首包，避免无限空等
        if (!expectTools && !receivedAnyByte
            && now - streamStart >= std::chrono::milliseconds(45000)) {
            if (callbacks.onStatus)
                callbacks.onStatus(L"流式首包超时，结束等待…");
            return false;
        }
        if (now >= streamDeadline) {
            if (!state.reasoning.empty() || !state.content.empty() || HasUsableStreamToolCalls(state)) {
                state.done = true;
                return true;
            }
            return false;
        }
        if (bytesAvailable == 0 && receivedAnyByte && HasMeaningfulStreamPayload(state)
            && now - lastByteTime >= std::chrono::milliseconds(kStreamIdleTimeoutMs)) {
            if (!state.reasoning.empty() || !state.content.empty() || HasUsableStreamToolCalls(state)) {
                state.done = true;
                return true;
            }
            return false;
        }
        return true; // 继续读
    };

    while (!state.done) {
        if (callbacks.cancelFlag && callbacks.cancelFlag->load())
            return fail(L"已取消");
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
            const DWORD err = GetLastError();
            // 热键 StopRun → Abort 关句柄：优先按取消退出，勿干等超时
            if (callbacks.cancelFlag && callbacks.cancelFlag->load())
                return fail(L"已取消");
            if (ShouldFinalizeStream(state, expectTools)) break;
            if (IsTransientHttpReceiveError(err)) {
                const bool ok = checkStreamProgress();
                if (state.done) break;
                if (!ok) {
                    return fail(L"流式接收超时（已超过 "
                        + std::to_wstring(effectiveTimeoutMs) + L" ms / 首包或空闲）");
                }
                Sleep(50);
                continue;
            }
            return fail(L"读取流失败：" + WinHttpErrorText(err)
                + L" (code=" + std::to_wstring(err) + L")");
        }
        {
            const bool ok = checkStreamProgress();
            if (state.done) break;
            if (!ok) {
                return fail(L"流式接收超时（已超过 "
                    + std::to_wstring(effectiveTimeoutMs) + L" ms）");
            }
        }
        if (bytesAvailable == 0) {
            if (ShouldFinalizeStream(state, expectTools)) break;
            if (callbacks.cancelFlag && callbacks.cancelFlag->load()) return fail(L"已取消");
            Sleep(50);
            continue;
        }
        receivedAnyByte = true;
        lastByteTime = std::chrono::steady_clock::now();
        if (callbacks.onStatus && !announcedStreamConnected) {
            announcedStreamConnected = true;
            callbacks.onStatus(L"已连接，接收流式响应…");
        }
        std::vector<char> buffer(bytesAvailable);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) || bytesRead == 0) {
            if (ShouldFinalizeStream(state, expectTools)) break;
            const bool ok = checkStreamProgress();
            if (state.done) break;
            if (!ok) {
                return fail(L"流式接收超时（已超过 "
                    + std::to_wstring(effectiveTimeoutMs) + L" ms）");
            }
            continue;
        }
        FeedStreamBytes(state, buffer.data(), bytesRead, callbacks);
    }
    if (!state.lineBuffer.empty())
        ProcessStreamSseLine(state.lineBuffer, state, callbacks);

    if (!state.done && HasUsableStreamToolCalls(state))
        state.done = true;

    if (AssembleStreamMessage(state, expectTools, result.message)) {
        result.ok = true;
        result.finishReason = state.finishReason;
        const bool hasTools = state.hasToolCalls || !state.toolCallParts.empty();
        if (!hasTools && !state.content.empty() && callbacks.onContentDelta && !state.contentDeltaSent)
            callbacks.onContentDelta(FromUtf8(state.content));
        return result;
    }

    if (statusCode >= 400) {
        std::wstring detail = FromUtf8(state.lineBuffer.empty() ? state.content : state.lineBuffer);
        if (detail.size() > 400) detail = detail.substr(0, 400) + L"...";
        return fail(L"HTTP " + std::to_wstring(statusCode) + L" " + parsed.path + L"：" + detail);
    }

    return fail(L"流式响应未包含可用内容");
}

// ── 发送消息（兼容旧接口）────────────────────────────────────────
std::wstring AgentCore::SendMessage(const std::wstring& userMessage,
                                    ChunkCallback onChunk,
                                    ToolCallCallback onToolCall) {
    AgentSendCallbacks cb;
    cb.onChunk = std::move(onChunk);
    cb.onToolCall = std::move(onToolCall);
    ChatMessage userMsg;
    userMsg.role = L"user";
    userMsg.content = userMessage;
    return SendMessage(userMsg, cb);
}

std::wstring AgentCore::SendMessage(const ChatMessage& userMessage,
                                    ChunkCallback onChunk,
                                    ToolCallCallback onToolCall) {
    AgentSendCallbacks cb;
    cb.onChunk = std::move(onChunk);
    cb.onToolCall = std::move(onToolCall);
    return SendMessage(userMessage, cb);
}

// ── 发送消息（含 tool-call 自动循环）──────────────────────────────
std::wstring AgentCore::SendMessage(const ChatMessage& userMessage,
                                    const AgentSendCallbacks& callbacks) {
    if (config_.apiUrl.empty())
        return L"[错误] 未配置 API 地址，请在「设置 → AI助手」中填写。";
    if (config_.apiKey.empty())
        return L"[错误] 未配置 API 密钥，请在「设置 → AI助手」中填写。";
    if (config_.model.empty())
        return L"[错误] 未配置模型名称，请在「设置 → AI助手」中填写。";

    ChatMessage userMsg = userMessage;
    userMsg.role = L"user";
    messages_.push_back(userMsg);

    static constexpr int kMaxToolLoops = 14;
    // 工具循环治理：
    // - lastToolName/lastToolResult/repeatToolCount：连续「同一工具+相同结果」计数；
    // - repeatedPairCount：全轮次内相同「工具+结果」对出现次数（隔轮绕圈 A→B→A→B 也能识别）。
    std::wstring lastToolName;
    std::wstring lastToolResult;
    int repeatToolCount = 0;
    std::map<std::wstring, int> repeatedPairCount;
    // 单轮内 readScriptReference 调用计数：超过阈值直接提示收束，防逐条翻参考
    int referenceReadCount = 0;
    // 单轮内「排查/探索」工具（搜索/浏览目录/读文件）计数：防止模型把轮次
    // 全花在找文件上，导致构建/创建没有余量。
    int reconToolCount = 0;

    for (int loop = 0; loop < kMaxToolLoops; ++loop) {
        if (callbacks.cancelFlag && callbacks.cancelFlag->load())
            return L"[错误] 用户取消";

        // 接近上限：注入引导（不限制思考，只提示下一步方向，给模型收束机会）
        if (loop >= kMaxToolLoops - 2) {
            ChatMessage limitMsg;
            limitMsg.role = L"user";
            limitMsg.internal_nudge = true;
            limitMsg.content = (loop == kMaxToolLoops - 1)
                ? L"这是本轮最后一次工具调用机会：请直接给出最终回答，"
                  L"或完成最后一个必要步骤后立即总结，不要再重复调用工具。"
                : L"已接近本轮工具调用上限。若脚本已创建或已保存，请直接总结；"
                  L"否则请在剩余轮次内完成必要步骤，不要重复调用工具。";
            messages_.push_back(std::move(limitMsg));
            if (callbacks.onStatus)
                callbacks.onStatus(loop == kMaxToolLoops - 1
                    ? L"最后一轮工具调用，已提示直接收尾…"
                    : L"接近工具调用上限，已提示收束…");
        }

        activeHttpAbort_ = callbacks.httpAbort;
        // 一律 auto：DeepSeek 深度思考模式不支持 tool_choice=required（会 400），
        // 且用户需要「先思考流程再实现」的分步规划，不该强制立刻调工具。
        const std::string choiceThisCall = "auto";
        // 工具续轮不再带截图（省大量 token），但仍走流式：
        // 推理模型在工具轮后还会长思考，非流式会整段生成完才返回、期间无任何反馈，
        // 前端表现像卡死；且「只思考不调工具」的纠偏只在流式路径生效。
        const bool stripImagesThisCall = (loop > 0);
        json requestBody = BuildRequest(stripImagesThisCall, choiceThisCall);
        if (callbacks.onStatus)
            callbacks.onStatus(loop == 0 ? L"正在连接…" : L"继续处理…");

        const bool useStream = !callbacks.preferNonStream;
        const bool expectTools = !tools_.empty();
        StreamApiResult streamed;
        ChatMessage assistantMsg;
        bool parsed = false;
        std::string finishReason;
        bool nonStreamRetryStripImages = stripImagesThisCall;

        auto needsNonStreamRetry = [&](const StreamApiResult& s, const ChatMessage& msg) {
            if (!useStream || !s.ok || !expectTools || !msg.tool_calls.empty()) return false;
            return s.finishReason == "length";
        };

        auto needsForceToolAfterReasoning = [&](const StreamApiResult& s, const ChatMessage& msg) {
            if (!useStream || !s.ok || !expectTools || !msg.tool_calls.empty()) return false;
            return msg.content.empty() && !msg.reasoning_content.empty();
        };

        if (useStream) {
            streamed = CallApiStream(requestBody, callbacks);
            if (streamed.ok) {
                assistantMsg = streamed.message;
                finishReason = streamed.finishReason;
                parsed = true;
                if (needsForceToolAfterReasoning(streamed, assistantMsg)) {
                    messages_.push_back(assistantMsg);
                    ChatMessage nudge;
                    nudge.role = L"user";
                    nudge.internal_nudge = true;
                    nudge.content =
                        L"请继续推进任务：基于你的分析调用下一步工具"
                        L"（如 buildScriptActions / createMacroScript）或给出最终回答；"
                        L"不要重复查询同一参考，也不要把思考内容当作最终回答。";
                    messages_.push_back(nudge);
                    if (callbacks.onStatus)
                        callbacks.onStatus(L"思考中未调工具，已提示继续推进…");
                    continue;
                } else if (needsNonStreamRetry(streamed, assistantMsg)) {
                    parsed = false;
                    nonStreamRetryStripImages = true;
                    if (callbacks.onStatus)
                        callbacks.onStatus(L"输出被截断，改用完整响应重试（省略截图）…");
                }
            }
        }

        if (!parsed) {
            if (callbacks.cancelFlag && callbacks.cancelFlag->load())
                return L"[错误] 用户取消";
            if (useStream && callbacks.onStatus) {
                if (!streamed.error.empty()) {
                    std::wstring hint = streamed.error;
                    if (hint.size() > 120) hint = hint.substr(0, 120) + L"...";
                    callbacks.onStatus(L"流式失败，改用完整响应（" + hint + L"）");
                } else if (!nonStreamRetryStripImages) {
                    callbacks.onStatus(L"正在等待完整响应…");
                }
            }
            // 流式失败/截断重试：一律省略截图，避免 80KB+ 再付一次
            json apiBody = (nonStreamRetryStripImages || useStream)
                ? BuildRequest(true, choiceThisCall)
                : requestBody;
            apiBody["stream"] = false;
            std::wstring apiError;
            const std::wstring responseText = CallApi(
                apiBody, &apiError, callbacks.cancelFlag, callbacks.httpAbort, callbacks.onStatus);
            if (!apiError.empty()) {
                if (callbacks.stopToolLoopAfterTools && callbacks.stopToolLoopAfterTools())
                    return L"";
                return L"[错误] API 请求失败：" + apiError;
            }
            if (responseText.empty())
                return L"[错误] API 请求失败：无响应。";

            json resp;
            try {
                resp = json::parse(ToUtf8(responseText));
            } catch (const json::parse_error& e) {
                return L"[错误] JSON 解析失败：" + FromUtf8(e.what());
            }
            if (resp.contains("error")) {
                std::string errMsg = resp["error"].value("message", "未知 API 错误");
                return L"[错误] API 返回错误：" + FromUtf8(errMsg);
            }
            if (!resp.contains("choices") || !resp["choices"].is_array() || resp["choices"].empty())
                return L"[错误] API 响应格式异常：缺少 choices。";

            const json& choice = resp["choices"][0];
            const json& message = choice.value("message", json::object());
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null())
                finishReason = choice["finish_reason"].get<std::string>();
            assistantMsg = ChatMessage{};
            assistantMsg.role = L"assistant";
            FillAssistantFromApiMessage(assistantMsg, message);
            parsed = true;
        }

        if (!parsed)
            return L"[错误] API 请求失败：" + (useStream ? streamed.error : L"无响应");

        activeHttpAbort_ = nullptr;

        const bool hasToolCalls = !assistantMsg.tool_calls.empty();

        if (hasToolCalls) {
            if (callbacks.onReasoning && !assistantMsg.reasoning_content.empty())
                callbacks.onReasoning(assistantMsg.reasoning_content);

            for (const auto& tc : assistantMsg.tool_calls) {
                if (callbacks.onToolCall)
                    callbacks.onToolCall(tc.name, tc.arguments);
            }
            messages_.push_back(assistantMsg);

            // 收集本轮工具结果中带出的脚本图片（readScript 的 [[AGENT_IMG:...]] 标记）
            std::vector<std::wstring> pendingImages;
            bool terminalSucceeded = false;
            std::wstring terminalResult;
            for (const auto& tc : assistantMsg.tool_calls) {
                std::wstring toolResult;
                bool found = false;
                // 工具层流程强制：构建/创建/手写保存脚本前必须先读脚本生成规范
                // （排除当前轮自身，避免同轮并行调用绕过约束）。
                const bool needsScriptSkill = (tc.name == L"buildScriptActions"
                    || tc.name == L"createMacroScript" || tc.name == L"writeScript"
                    || tc.name == L"planScriptActions");
                const bool scriptSkillRead = SkillScriptStrategyRead(
                    messages_, messages_.size() - 1);
                const bool isReconTool = tc.name == L"searchAgentFiles"
                    || tc.name == L"listDirectory" || tc.name == L"readAgentFile";
                if (needsScriptSkill && !scriptSkillRead) {
                    toolResult = L"[错误] 规划/构建/创建/保存脚本前必须先调用 "
                        L"readAgentSkill section=scriptStrategy 获取脚本生成规范；"
                        L"请先读取该 Skill，再调用 " + tc.name
                        + L"。不要用 readScriptReference 逐条翻阅代替。";
                    found = true;
                } else if (tc.name == L"readScriptReference"
                    && ++referenceReadCount > 3) {
                    toolResult = L"[提示] 你已多次翻阅脚本参考（超过 3 次），"
                        L"请停止逐条查阅。先调用 readAgentSkill section=scriptStrategy "
                        L"获取脚本生成规范，再 planScriptActions 核对动作树，"
                        L"然后 buildScriptActions / createMacroScript。";
                    found = true;
                } else if (isReconTool && ++reconToolCount > 4) {
                    toolResult = L"[提示] 你已多次浏览/搜索/读取文件（超过 4 次），"
                        L"请停止排查。创建/修改脚本请先调用 readAgentSkill "
                        L"section=scriptStrategy，再 planScriptActions → createMacroScript；"
                        L"需要定位文件时一次读取目录即可。";
                    found = true;
                } else {
                    for (const auto& tool : tools_) {
                        if (tool.name == tc.name) {
                            toolResult = tool.execute(tc.arguments);
                            found = true;
                            break;
                        }
                    }
                }
                if (!found)
                    toolResult = L"[错误] 未知工具：" + tc.name;

                // 重复检测：仅「同一工具 + 完全相同返回」连续出现才计数；
                // 同名但参数/结果不同（如 readScript 换脚本）不算重复。
                if (tc.name == lastToolName && toolResult == lastToolResult) {
                    ++repeatToolCount;
                } else {
                    lastToolName = tc.name;
                    lastToolResult = toolResult;
                    repeatToolCount = 1;
                }
                ++repeatedPairCount[tc.name + L"\x01" + toolResult];

                // 脚本图片标记：多模态模型把图片编码为下一轮 user 图片消息；
                // 非多模态模型降级为路径文本提示，不报错。
                std::wstring toolText;
                std::vector<std::wstring> imgPaths;
                if (AgentExtractImageMarkers(toolResult, toolText, imgPaths)
                    && !imgPaths.empty()) {
                    toolResult = toolText;
                    if (ModelSupportsVision(config_.model)) {
                        for (size_t i = 0; i < imgPaths.size(); ++i) {
                            toolResult += L"\n[脚本图片 " + std::to_wstring(i + 1)
                                + L"] 已作为参考嵌入下一轮请求（" + imgPaths[i] + L"）";
                            pendingImages.push_back(imgPaths[i]);
                        }
                    } else {
                        for (const auto& p : imgPaths) {
                            toolResult += L"\n[脚本图片: " + p
                                + L"]（当前模型不支持视觉，未读取图片内容，请基于路径/文件名推断）";
                        }
                    }
                }

                // 入历史按工具分层预算（只读台账略宽，lookup/执行摘要宜短）
                const size_t kMaxToolResultChars = AiToolResultBudgetChars(tc.name);
                if (toolResult.size() > kMaxToolResultChars) {
                    toolResult.resize(kMaxToolResultChars);
                    toolResult += L"\n…(工具结果已截断)";
                }

                // 终态工具成功标记：脚本/录制已保存、定时任务已创建/更新。
                // 放在截断之后捕获，保证返回文本与入历史的内容一致。
                if (!terminalSucceeded && IsTerminalToolName(tc.name)
                    && ToolResultIsSuccess(toolResult)) {
                    terminalSucceeded = true;
                    terminalResult = toolResult;
                }

                if (callbacks.onToolResult)
                    callbacks.onToolResult(tc.name, toolResult);

                ChatMessage toolMsg;
                toolMsg.role = L"tool";
                toolMsg.content = toolResult;
                toolMsg.tool_call_id = tc.id;
                messages_.push_back(toolMsg);
            }
            if (!pendingImages.empty()) {
                std::vector<ChatContentPart> imgParts;
                std::wstring skipped;
                if (AgentBuildImageParts(pendingImages, imgParts, skipped)) {
                    ChatMessage imgMsg;
                    imgMsg.role = L"user";
                    imgMsg.keep_images = true;
                    imgMsg.internal_nudge = true;
                    ChatContentPart textPart;
                    textPart.type = L"text";
                    textPart.text = L"以上为脚本引用的图片，请结合工具结果理解脚本意图。"
                        + (skipped.empty() ? L"" : (L"\n" + skipped));
                    imgMsg.parts.push_back(std::move(textPart));
                    for (auto& p : imgParts) imgMsg.parts.push_back(std::move(p));
                    messages_.push_back(std::move(imgMsg));
                    if (callbacks.onStatus) {
                        callbacks.onStatus(L"已嵌入 " + std::to_wstring(pendingImages.size())
                            + L" 张脚本图片供理解");
                    }
                } else if (callbacks.onStatus) {
                    callbacks.onStatus(L"脚本图片读取失败：" + skipped);
                }
            }
            // 同一工具 + 相同结果：连续 2 次，或全轮出现 2 次（隔轮绕圈），注入收束引导
            std::wstring repeatName = lastToolName;
            bool repeatedPairDetected = false;
            for (const auto& kv : repeatedPairCount) {
                if (kv.second >= 2) {
                    repeatedPairDetected = true;
                    const size_t sep = kv.first.find(L'\x01');
                    repeatName = (sep != std::wstring::npos)
                        ? kv.first.substr(0, sep) : kv.first;
                    break;
                }
            }
            if (repeatToolCount >= 2 || repeatedPairDetected) {
                ChatMessage nag;
                nag.role = L"user";
                nag.internal_nudge = true;
                nag.content = L"注意：你已多次用相同参数调用同一工具并得到相同结果（"
                    + repeatName + L"）。这通常说明在绕圈：请基于已有信息继续完成任务，"
                    L"必要时换用其它工具，"
                    + L"或直接给出最终脚本/回答。";
                messages_.push_back(std::move(nag));
                if (callbacks.onStatus)
                    callbacks.onStatus(L"检测到重复工具调用（" + repeatName
                        + L"），已提示停止…");
                repeatToolCount = 0;
                lastToolName.clear();
                lastToolResult.clear();
                repeatedPairCount.clear();
            }
            // 终态工具成功：脚本/录制已保存 → 直接收尾，不再发起下一轮 API
            if (terminalSucceeded) {
                if (callbacks.onContentDelta)
                    callbacks.onContentDelta(terminalResult);
                else if (callbacks.onChunk)
                    callbacks.onChunk(terminalResult);
                return terminalResult;
            }
            if (callbacks.stopToolLoopAfterTools && callbacks.stopToolLoopAfterTools())
                return L"";
            continue;
        }

        std::wstring finalContent = assistantMsg.content;
        if (finalContent.empty() && !assistantMsg.reasoning_content.empty())
            finalContent = (loop > 0
                ? L"[提示] 模型思考后未继续生成或调用工具，本次未产出结果。"
                    L"可重试，或换用非推理模型。\n\n"
                : L"") + assistantMsg.reasoning_content;
        messages_.push_back(assistantMsg);

        if (callbacks.onReasoning && !assistantMsg.reasoning_content.empty())
            callbacks.onReasoning(assistantMsg.reasoning_content);

        if (callbacks.onChunk && !callbacks.onContentDelta)
            callbacks.onChunk(finalContent);

        if (expectTools && finishReason == "length" && !hasToolCalls) {
            return L"[提示] 模型输出因 max_tokens（当前 "
                + std::to_wstring(config_.maxTokens)
                + L"）被截断，未完成脚本生成。请在「设置 → AI助手」中增大 max_tokens，"
                L"或换用非思考型模型后重试。\n\n" + finalContent;
        }

        return finalContent;
    }

    const std::wstring limitNote = L"本轮工具调用达到上限（" + std::to_wstring(kMaxToolLoops)
        + L" 轮），已停止继续调用以避免绕圈。前面已完成的工具结果与思考内容均已保留；"
        L"你可以直接重试，或将需求拆得更具体后再发一次。";
    if (callbacks.onStatus)
        callbacks.onStatus(L"已达到工具调用上限，本轮结束");
    return limitNote;
}
