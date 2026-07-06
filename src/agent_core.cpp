// ──────────────────────────────────────────────────────────────────
// agent_core.cpp — AI Agent 核心通信层实现
// WinHTTP 通信、JSON 请求构建、tool-call 自动循环
// ──────────────────────────────────────────────────────────────────

#include "agent_core.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
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
constexpr DWORD kHttpReceivePollMs = 30000;  // 30s，避免大图片请求触发 12019

bool HttpSendJsonBody(HINTERNET hRequest, const std::wstring& headers, const std::string& body,
                      const std::atomic_bool* cancelFlag, std::wstring* errorOut) {
    const DWORD total = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(
            hRequest, headers.c_str(), static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, total, 0)) {
        if (errorOut) *errorOut = L"发送请求失败：" + WinHttpErrorText(GetLastError());
        return false;
    }
    size_t sent = 0;
    constexpr size_t kChunk = 65536;
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
            lastReportSec = waitedSec;
            onStatus(L"等待响应 " + std::to_wstring(waitedSec) + L"s… (err=" + std::to_wstring(lastErr) + L")");
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
json AgentCore::BuildRequest() {
    json req;
    req["model"] = ToUtf8(config_.model);
    req["temperature"] = config_.temperature;
    req["max_tokens"] = config_.maxTokens;

    size_t lastUserIdx = messages_.size();
    for (size_t i = messages_.size(); i > 0; --i) {
        if (messages_[i - 1].role == L"user") {
            lastUserIdx = i - 1;
            break;
        }
    }

    // 构建 messages 数组
    json msgs = json::array();
    for (size_t mi = 0; mi < messages_.size(); ++mi) {
        const auto& m = messages_[mi];
        const bool stripImages = m.role == L"user" && mi != lastUserIdx;
        json msg;
        msg["role"] = ToUtf8(m.role);
        if (!m.parts.empty()) {
            json parts = json::array();
            for (const auto& p : m.parts) {
                if (p.type == L"text") {
                    parts.push_back({{"type", "text"}, {"text", ToUtf8(p.text)}});
                } else if (p.type == L"image_url") {
                    if (stripImages) {
                        parts.push_back({{"type", "text"}, {"text", "(历史截图已省略)"}});
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
        req["tool_choice"] = "auto";
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
    if (onStatus) {
        onStatus(L"请求体 " + std::to_wstring((body.size() + 1023) / 1024)
            + L" KB，上传中…");
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
    DWORD recvTimeout = static_cast<DWORD>(std::max(5000, config_.recvTimeoutMs));
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

    if (!HttpSendJsonBody(hRequest, headers, body, cancelFlag, errorOut))
        return fail(errorOut ? *errorOut : L"发送请求失败");

    if (cancelFlag && cancelFlag->load()) return fail(L"已取消");

    if (onStatus) onStatus(L"等待服务器响应…");

    // 使用较长的每次轮询超时（30s），避免大图片请求触发 12019
    DWORD pollTimeoutMs = 30000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &pollTimeoutMs, sizeof(pollTimeoutMs));

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max(30000, config_.recvTimeoutMs));
    const auto waitStart = std::chrono::steady_clock::now();
    int lastReportSec = 0;
    DWORD lastErr = 0;
    bool received = false;
    for (;;) {
        if (cancelFlag && cancelFlag->load())
            return fail(L"已取消");
        const int waitedSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - waitStart).count());
        if (onStatus && waitedSec >= lastReportSec + 5) {
            lastReportSec = waitedSec;
            onStatus(L"等待响应 " + std::to_wstring(waitedSec) + L"s…");
        }
        if (WinHttpReceiveResponse(hRequest, nullptr)) {
            received = true;
            break;
        }
        lastErr = GetLastError();
        if (cancelFlag && cancelFlag->load())
            return fail(L"已取消");
        if (std::chrono::steady_clock::now() >= deadline)
            return fail(L"等待服务器响应超时（已超过 "
                + std::to_wstring(std::max(30000, config_.recvTimeoutMs)) + L"ms，"
                + WinHttpErrorText(lastErr) + L" code=" + std::to_wstring(lastErr) + L"）");
    }

    if (!received)
        return fail(L"接收响应失败：" + WinHttpErrorText(lastErr) + L" (code=" + std::to_wstring(lastErr) + L")");

    if (onStatus) onStatus(L"已收到响应头，读取响应体…");

    DWORD pollRecvTimeoutMs = kHttpBodyPollMs;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &pollRecvTimeoutMs, sizeof(pollRecvTimeoutMs));

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    std::string responseBody;
    DWORD bytesAvailable = 0;
    for (;;) {
        if (cancelFlag && cancelFlag->load()) return fail(L"已取消");
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
    bool done = false;
    bool hasToolCalls = false;
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
    if (!state.toolCallParts.empty()) {
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
        const std::string fr = choice["finish_reason"].get<std::string>();
        if (fr == "stop" || fr == "tool_calls" || fr == "length") state.done = true;
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
            if (callbacks.onContentDelta)
                callbacks.onContentDelta(FromUtf8(piece));
        }
    }
    if (delta.contains("tool_calls")) {
        state.hasToolCalls = true;
        MergeStreamToolCallDelta(state.toolCallParts, delta["tool_calls"]);
    }
    if (choice.contains("message") && choice["message"].is_object()) {
        const json& msg = choice["message"];
        appendReasoning(msg, "reasoning_content");
        appendReasoning(msg, "reasoning");
        appendReasoning(msg, "thinking");
        appendReasoning(msg, "thought");
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            state.hasToolCalls = true;
            state.done = true;
            for (const json& tc : msg["tool_calls"]) state.toolCallParts.push_back(tc);
        }
        if (msg.contains("content") && msg["content"].is_string()) {
            const std::string piece = msg["content"].get<std::string>();
            if (!piece.empty()) {
                state.content += piece;
                if (callbacks.onContentDelta)
                    callbacks.onContentDelta(FromUtf8(piece));
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

    const std::wstring url = NormalizeChatCompletionsUrl(Trim(config_.apiUrl));
    const ParsedUrl parsed = ParseUrl(url);
    if (parsed.host.empty() || !HostLooksValid(parsed.host))
        return fail(L"API 地址格式无效，请检查是否包含完整的 https:// 地址。");

    const std::string body = requestBody.dump();

    WinHttpHandle hSession = WinHttpOpen(
        L"QuickScriptTool/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return fail(L"WinHttpOpen 失败：" + WinHttpErrorText(GetLastError()));

    DWORD connectTimeout = 30000;
    DWORD sendTimeout = body.size() > 300000 ? 180000u : 120000u;
    DWORD recvTimeout = static_cast<DWORD>(std::max(5000, config_.recvTimeoutMs));
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
    if (!HttpSendJsonBody(hRequest, headers, body, callbacks.cancelFlag, &sendErr))
        return fail(sendErr.empty() ? L"发送请求失败" : sendErr);

    if (callbacks.cancelFlag && callbacks.cancelFlag->load()) return fail(L"已取消");

    std::wstring recvErr;
    if (!ReceiveResponseWithPolling(hRequest, callbacks.cancelFlag, config_.recvTimeoutMs,
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
        + std::chrono::milliseconds(std::max(5000, config_.recvTimeoutMs));
    const auto streamStart = std::chrono::steady_clock::now();
    int lastBeatSec = 0;
    while (!state.done) {
        if (callbacks.cancelFlag && callbacks.cancelFlag->load())
            return fail(L"已取消");
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
            const DWORD err = GetLastError();
            if (ShouldFinalizeStream(state, expectTools)) break;
            if (IsTransientHttpReceiveError(err)) {
                if (callbacks.cancelFlag && callbacks.cancelFlag->load()) return fail(L"已取消");
                continue;
            }
            return fail(L"读取流失败：" + WinHttpErrorText(err)
                + L" (code=" + std::to_wstring(err) + L")");
        }
        if (bytesAvailable == 0) {
            if (ShouldFinalizeStream(state, expectTools)) break;
            if (callbacks.cancelFlag && callbacks.cancelFlag->load()) return fail(L"已取消");
            if (std::chrono::steady_clock::now() >= streamDeadline) {
                if (!state.reasoning.empty() || !state.content.empty()) {
                    state.done = true;
                    break;
                }
                return fail(L"流式接收超时（已超过 "
                    + std::to_wstring(config_.recvTimeoutMs) + L" ms）");
            }
            const int waitedSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - streamStart).count());
            if (callbacks.onStatus && waitedSec >= lastBeatSec + 3) {
                lastBeatSec = waitedSec;
                std::wstring beat = L"流式等待 " + std::to_wstring(waitedSec) + L"s";
                if (!state.reasoning.empty())
                    beat += L"，思考 " + std::to_wstring(state.reasoning.size()) + L" 字节";
                if (!state.content.empty())
                    beat += L"，回复 " + std::to_wstring(state.content.size()) + L" 字节";
                callbacks.onStatus(beat + L"…");
            }
            Sleep(50);
            continue;
        }
        std::vector<char> buffer(bytesAvailable);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) || bytesRead == 0) {
            if (ShouldFinalizeStream(state, expectTools)) break;
            continue;
        }
        FeedStreamBytes(state, buffer.data(), bytesRead, callbacks);
    }
    if (!state.lineBuffer.empty())
        ProcessStreamSseLine(state.lineBuffer, state, callbacks);

    if (AssembleStreamMessage(state, expectTools, result.message)) {
        result.ok = true;
        const bool hasTools = state.hasToolCalls || !state.toolCallParts.empty();
        if (!hasTools && !state.content.empty() && callbacks.onContentDelta)
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

    static constexpr int kMaxToolLoops = 10;

    for (int loop = 0; loop < kMaxToolLoops; ++loop) {
        if (callbacks.cancelFlag && callbacks.cancelFlag->load())
            return L"[错误] 用户取消";

        activeHttpAbort_ = callbacks.httpAbort;
        json requestBody = BuildRequest();
        if (callbacks.onStatus)
            callbacks.onStatus(loop == 0 ? L"Connecting..." : L"继续处理…");

        const bool useStream = !callbacks.preferNonStream;
        StreamApiResult streamed;
        ChatMessage assistantMsg;
        bool parsed = false;

        if (useStream) {
            streamed = CallApiStream(requestBody, callbacks);
            if (streamed.ok) {
                assistantMsg = streamed.message;
                parsed = true;
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
                } else {
                    callbacks.onStatus(L"正在等待完整响应…");
                }
            }
            json apiBody = requestBody;
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

            const json& message = resp["choices"][0].value("message", json::object());
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

            for (const auto& tc : assistantMsg.tool_calls) {
                std::wstring toolResult;
                bool found = false;
                for (const auto& tool : tools_) {
                    if (tool.name == tc.name) {
                        toolResult = tool.execute(tc.arguments);
                        found = true;
                        break;
                    }
                }
                if (!found)
                    toolResult = L"[错误] 未知工具：" + tc.name;

                if (callbacks.onToolResult)
                    callbacks.onToolResult(tc.name, toolResult);

                ChatMessage toolMsg;
                toolMsg.role = L"tool";
                toolMsg.content = toolResult;
                toolMsg.tool_call_id = tc.id;
                messages_.push_back(toolMsg);
            }
            if (callbacks.stopToolLoopAfterTools && callbacks.stopToolLoopAfterTools())
                return L"";
            continue;
        }

        std::wstring finalContent = assistantMsg.content;
        if (finalContent.empty() && !assistantMsg.reasoning_content.empty())
            finalContent = assistantMsg.reasoning_content;
        messages_.push_back(assistantMsg);

        if (callbacks.onReasoning && !assistantMsg.reasoning_content.empty())
            callbacks.onReasoning(assistantMsg.reasoning_content);

        if (callbacks.onChunk && !callbacks.onContentDelta)
            callbacks.onChunk(finalContent);

        return finalContent;
    }

    return L"[错误] 工具调用超过最大循环次数（" + std::to_wstring(kMaxToolLoops) + L"）。";
}
