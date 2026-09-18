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
#include <atomic>
#include <chrono>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
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

/// 请求句柄守卫：Abort（用户停止 / 看门狗策略超时）可能已经在别的线程把它关了，
/// 那种情况下这里不能再关一次。
struct AbortAwareRequestGuard {
    WinHttpHandle req;
    AiHttpAbortSlot* slot = nullptr;
    AbortAwareRequestGuard(HINTERNET h, AiHttpAbortSlot* s) : req(h), slot(s) {}
    ~AbortAwareRequestGuard() {
        if (slot && slot->ConsumeClosed()) req.Detach();  // 所有权已归 Abort，勿重复关闭
    }
    operator HINTERNET() const { return req.handle; }
    HINTERNET operator->() const { return req.handle; }
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

// 读 body 的轮询切片。**别调小**：WinHTTP 的 RECEIVE_TIMEOUT 一旦触发，
// 这次请求基本就废了（后续 QueryDataAvailable 报 12019 句柄状态错误，
// 见 IsTransientHttpReceiveError 的注释），而 thinking 模型吐字间隙、SSE keepalive
// 间隔都可能到几百毫秒 —— 250ms 切片等于每轮都在赌命，实测表现为
// 「思考 8s → 流式失败 → 再发一次完整请求（又多 90s）」。
// 1500ms 兼顾两件事：正常间隙不再误杀；停止/取消最多多等 1.5s。
constexpr DWORD kHttpBodyPollMs = 1500;

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
    // 读 body / QueryDataAvailable 的短轮询：仅超时/连接闪断可在同一句柄上继续等。
    // 12019（句柄状态错误）等不可重试：WinHTTP 超时后句柄已作废。
    // ★调用方必须先试「已攒到可用内容就直接采用」（见 CallApiStream 读循环），
    //   因为 SSE 正常收尾时 QueryDataAvailable 也可能报 12019 —— 那不是故障。
    if (err == 0) return true;
    return err == ERROR_WINHTTP_TIMEOUT
        || err == ERROR_WINHTTP_CONNECTION_ERROR
        || err == ERROR_OPERATION_ABORTED;
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
    // WinHTTP：ReceiveResponse 一旦超时，句柄进入不确定状态，必须关掉重开，
    // 不能在同一句柄上把 5s 超时当「临时错误」空转（表现为 12019 空等到满超时）。
    const int waitMs = std::max(5000, maxWaitMs);
    DWORD recvMs = static_cast<DWORD>(waitMs);
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &recvMs, sizeof(recvMs));
    const auto waitStart = std::chrono::steady_clock::now();

    std::atomic<bool> stopBeat{false};
    std::thread beat;
    if (onStatus) {
        beat = std::thread([&, waitMs]() {
            int lastReportSec = 0;
            while (!stopBeat.load()) {
                for (int i = 0; i < 10 && !stopBeat.load(); ++i)
                    Sleep(500);
                if (stopBeat.load()) break;
                const int waitedSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - waitStart).count());
                if (waitedSec >= lastReportSec + 5) {
                    lastReportSec = waitedSec - (waitedSec % 5);
                    const int remainSec = std::max(0, waitMs / 1000 - waitedSec);
                    onStatus(L"等待响应 " + std::to_wstring(waitedSec) + L"s（剩余约 "
                        + std::to_wstring(remainSec) + L"s）…");
                }
            }
        });
    }

    const bool ok = WinHttpReceiveResponse(hRequest, nullptr);
    const DWORD lastErr = ok ? 0 : GetLastError();
    stopBeat.store(true);
    if (beat.joinable()) beat.join();

    if (cancelFlag && cancelFlag->load()) {
        if (errorOut) *errorOut = L"已取消";
        return false;
    }
    if (ok) return true;
    if (errorOut) {
        *errorOut = L"等待服务器响应超时（已超过 "
            + std::to_wstring(waitMs) + L" ms，最后错误="
            + WinHttpErrorText(lastErr)
            + L" code=" + std::to_wstring(lastErr) + L"）";
    }
    if (onStatus)
        onStatus(L"接收超时: " + WinHttpErrorText(lastErr) + L" (code=" + std::to_wstring(lastErr) + L")");
    return false;
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

// 思考开关策略（2026-09-16 用户拍板改为「允许思考 + 保留工具催促」）。
//
// 背景：早期为了治「只想不调工具 / 单轮数分钟」一律下发 thinking.type=disabled，
// 单轮能压到 1~3s。但任务复杂度上来后（读办公文档、Office COM 编排、多步规划、
// 游戏/动态画面判断），关思考会明显压低准确率——日志里模型经常选错路线。
//
// 现在：**默认允许思考**（不下发 thinking 字段，交给网关/模型自己决定），
// 「只想不调工具」继续由既有的 toolNudgePending 机制兜（上轮没调工具就催一轮），
// 那才是这个问题的正解。
// 若某台机器/某次演示需要回到快速执行：设环境变量 QST_FAST_THINKING=1。
// 「上一轮只想不干」→ 接下来 N 轮**关掉思考**，强制直接动手。
// ★通用提速：实测一轮可以连续思考 45s、吐 36KB 推理（第十五/十六次日志），
//   而它想的东西上一轮已经想过一遍了 —— 越读自己的旧推理越不肯动手。
//   关掉思考只省时间、不损正确性（需要回放思考的网关走 requires_reasoning_content 另一条路）。
std::atomic<int> g_thinkingSuppressedRounds{0};

void SuppressThinkingForNextRounds(int rounds) {
    if (rounds < 1) return;
    g_thinkingSuppressedRounds.store(std::clamp(rounds, 1, 8));
}

void NoteThinkingRoundConsumed() {
    int cur = g_thinkingSuppressedRounds.load();
    while (cur > 0 && !g_thinkingSuppressedRounds.compare_exchange_weak(cur, cur - 1)) {
    }
}

namespace {
/// 旧行为（保留给 QST_FAST_THINKING=1 的逃生阀）：按网关/模型判定是否下发 thinking。
bool ThinkingDisabledByGateway(const std::wstring& apiUrl, const std::wstring& model) {
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
}  // namespace

bool ShouldDisableThinking(const std::wstring& apiUrl, const std::wstring& model) {
    // ① 动态抑制：上一轮「只想不干」→ 接下来几轮直接关思考（通用提速主力）
    if (g_thinkingSuppressedRounds.load() > 0) return true;
    // ② 逃生阀：QST_FAST_THINKING=1 恢复旧的「按网关判定」行为
    wchar_t envBuf[8]{};
    if (GetEnvironmentVariableW(L"QST_FAST_THINKING", envBuf, 8) > 0 && envBuf[0] == L'1')
        return ThinkingDisabledByGateway(apiUrl, model);
    return false;
}

std::wstring AgentUserFacingToolReply(const std::wstring& toolResult) {
    std::wstring out = toolResult;
    auto cutAt = [&](const wchar_t* marker) {
        const size_t pos = out.find(marker);
        if (pos != std::wstring::npos) out.resize(pos);
    };
    cutAt(L"【动作一览");
    cutAt(L"【动作 type");
    cutAt(L"动作一览（缩进");
    cutAt(L"动作一览：");
    cutAt(L"对用户说明时必须");
    cutAt(L"禁止说英文");
    while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r'
        || out.back() == L' ' || out.back() == L'\t')) {
        out.pop_back();
    }
    return out;
}

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
                        // ★别写「请根据文字描述直接调用工具」——模型会理解成「我没有图，只能瞎猜」，
                        // 实测它因此反复 screenshot / 反复自问「我看不到画面吗」，白烧好几轮。
                        // 明确告诉它：这是省 token 的历史图，要看当前画面就主动取。
                        const char* hint = (stripLastUserImages && mi == lastUserIdx)
                            ? "(本轮未附截图以省 token；需要看当前画面请调用 computer(action=screenshot)，"
                              "或直接用 locateAndClick 让宿主识图定位)"
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
        else if (!m.reasoning_content.empty()) {
            // ★通用提速：**不要把模型自己的长推理整段回灌**。
            //   实测一轮能吐 36KB 推理，回灌后每一轮的请求体都把它重发一遍
            //   （日志里请求体一路涨到 314~330KB），而且模型读到自己的旧推理会**接着纠结**
            //   （第十六次日志里它把同一个决定反复推了十几遍）。
            //   网关明确要求回放的，走上面 requires_reasoning_content 那条不变；
            //   其余只留一小截尾巴（有些网关要看到 non-empty 才肯走同一分支）。
            constexpr size_t kReasoningEchoMaxChars = 400;
            std::wstring tail = m.reasoning_content;
            if (tail.size() > kReasoningEchoMaxChars) {
                tail = L"…(前文思考已省略)…"
                    + tail.substr(tail.size() - kReasoningEchoMaxChars);
            }
            msg["reasoning_content"] = ToUtf8(tail);
        }
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
    // DeepSeek V4 / 豆包 seed 等思考型模型：默认就会长思考。现在**允许思考**
    // （准确率优先，见 ShouldDisableThinking 注释）；只在显式设了
    // QST_FAST_THINKING=1 时才恢复旧的「强制快速执行」。
    // 原生推理模型（R1/Reasoner、o 系列）不接受该开关，不发送避免 400；
    // 其它网关（OpenAI 等）保持默认。
    if (ShouldDisableThinking(config_.apiUrl, config_.model)
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

    // ★读 body 的超时**必须给足**：WinHTTP 的 RECEIVE_TIMEOUT 一旦触发，这次请求句柄就废了
    //（后续 QueryDataAvailable 报 12019），而 thinking 模型在图片请求上「看图 + 想」的间隙
    // 常常好几秒 —— 之前用 250ms/1500ms 短切片轮询，等于每一轮都在赌，实测就是
    // 「思考 4s → 流式失败 → 再发一次完整请求（又多 90s）」。
    // 现在改成：读给足超时，**由看门狗线程**负责心跳、策略超时与取消；
    // 需要打断阻塞读时用 Abort()（与「停止热键」同一条已验证的路径）。
    DWORD bodyRecvTimeoutMs = static_cast<DWORD>((std::max)(30000, effectiveTimeoutMs));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT,
        &bodyRecvTimeoutMs, sizeof(bodyRecvTimeoutMs));

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
    bool announcedStreamConnected = false;
    bool receivedAnyByte = false;

    // 看门狗只能读原子量（state 归读线程独占，跨线程读 std::wstring 是数据竞争）
    std::atomic<long long> lastByteTickMs{0};
    std::atomic<size_t> reasoningBytes{0};
    std::atomic<size_t> contentBytes{0};
    std::atomic<bool> toolAssemblyStarted{false};
    std::atomic<bool> sawUsableToolCalls{false};
    std::atomic<bool> streamDone{false};
    // 0=继续 1=收束（采用已收内容）2=放弃（改完整响应）
    std::atomic<int> stopKind{0};
    std::atomic<bool> watchdogStop{false};
    std::wstring stopReason;
    std::mutex stopReasonMu;

    auto TicksMs = []() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    lastByteTickMs.store(TicksMs());

    auto setStop = [&](int kind, const std::wstring& reason) {
        int expected = 0;
        if (!stopKind.compare_exchange_strong(expected, kind)) return;
        {
            std::lock_guard<std::mutex> lock(stopReasonMu);
            stopReason = reason;
        }
        if (callbacks.onStatus && !reason.empty()) callbacks.onStatus(reason);
        // 解除阻塞读：只有 Abort 关句柄才能让 WinHttpQueryDataAvailable 立刻返回
        if (callbacks.httpAbort) callbacks.httpAbort->Abort();
    };

    std::thread watchdog;
    if (callbacks.onStatus || callbacks.httpAbort) {
        watchdog = std::thread([&]() {
            int lastBeatSec = -3;
            while (!watchdogStop.load()) {
                for (int i = 0; i < 25 && !watchdogStop.load(); ++i) Sleep(200);
                if (watchdogStop.load()) break;
                const auto now = std::chrono::steady_clock::now();
                const int waitedSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                    now - streamStart).count());
                const bool gotAny = lastByteTickMs.load() > 0;
                const long long idleMs = TicksMs() - lastByteTickMs.load();
                const size_t rBytes = reasoningBytes.load();
                const size_t cBytes = contentBytes.load();
                if (callbacks.onStatus && waitedSec >= lastBeatSec + 3) {
                    lastBeatSec = waitedSec;
                    std::wstring beat = L"流式等待 " + std::to_wstring(waitedSec) + L"s";
                    if (rBytes > 0) beat += L"，思考 " + std::to_wstring(rBytes) + L" 字节";
                    if (cBytes > 0) beat += L"，回复 " + std::to_wstring(cBytes) + L" 字节";
                    if (toolAssemblyStarted.load()) beat += L"，工具调用组装中";
                    callbacks.onStatus(beat + L"…");
                }
                if (stopKind.load() != 0) break;

                // ① 思考很久却始终不调工具 → 收束（采用已有内容，交给上层催工具）
                if (expectTools && cBytes == 0 && !sawUsableToolCalls.load() && rBytes > 0) {
                    const bool hitAbs = now - streamStart >= std::chrono::milliseconds(kReasoningOnlyForceMs);
                    const bool hitPlateau = gotAny
                        && idleMs >= kReasoningPlateauIdleMs;
                    if (hitAbs || hitPlateau) {
                        setStop(1, hitPlateau
                            ? L"思考已停顿仍未调用工具，结束流式并强制调工具…"
                            : L"思考过久未调用工具，结束流式并强制调工具…");
                        break;
                    }
                }
                // ② 工具调用组装超时 → 改完整响应
                if (expectTools && toolAssemblyStarted.load()
                    && !sawUsableToolCalls.load()
                    && now - streamStart >= std::chrono::milliseconds(kToolCallAssemblyCapMs)) {
                    setStop(2, L"工具调用组装超时，改用完整响应…");
                    break;
                }
                // ③ 工具轮空流（只有 keepalive / 没有首包）→ 改完整响应
                if (expectTools && rBytes == 0 && cBytes == 0 && !sawUsableToolCalls.load()
                    && now - streamStart >= std::chrono::milliseconds(emptyStreamCapMs)) {
                    setStop(2, gotAny
                        ? L"流式空闲无内容（可能仅 keepalive），改用完整响应…"
                        : L"流式首包超时，改用完整响应…");
                    break;
                }
                // ④ 非工具轮首包
                if (!expectTools && !gotAny
                    && now - streamStart >= std::chrono::milliseconds(45000)) {
                    setStop(2, L"流式首包超时，结束等待…");
                    break;
                }
                // ⑤ 整体超时
                if (now >= streamDeadline) {
                    setStop((rBytes > 0 || cBytes > 0 || sawUsableToolCalls.load()) ? 1 : 2,
                        L"流式接收超时（已超过 " + std::to_wstring(effectiveTimeoutMs) + L" ms）");
                    break;
                }
                // ⑥ 有内容但长时间没有新字节 → 收束
                if (gotAny && (rBytes > 0 || cBytes > 0 || sawUsableToolCalls.load())
                    && idleMs >= kStreamIdleTimeoutMs) {
                    setStop(1, L"流式空闲超时，采用已收内容");
                    break;
                }
            }
        });
    }
    struct WatchdogGuard {
        std::thread* t;
        std::atomic<bool>* stop;
        ~WatchdogGuard() {
            if (stop) stop->store(true);
            if (t && t->joinable()) t->join();
        }
    } watchdogGuard{&watchdog, &watchdogStop};

    auto currentStopReason = [&]() -> std::wstring {
        std::lock_guard<std::mutex> lock(stopReasonMu);
        return stopReason;
    };

    while (!state.done) {
        if (callbacks.cancelFlag && callbacks.cancelFlag->load())
            return fail(L"已取消");
        if (stopKind.load() == 2) {
            const std::wstring reason = currentStopReason();
            return fail(reason.empty() ? L"流式接收中断" : reason);
        }
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
            const DWORD err = GetLastError();
            // 热键 StopRun / 看门狗 → Abort 关句柄：优先按取消/策略退出，勿干等超时
            if (callbacks.cancelFlag && callbacks.cancelFlag->load())
                return fail(L"已取消");
            if (stopKind.load() == 1) break;
            if (stopKind.load() == 2) {
                const std::wstring reason = currentStopReason();
                return fail(reason.empty() ? L"流式接收中断" : reason);
            }
            // ★句柄已作废 / 流已结束（12019 等）：只要已经攒到可用内容就**直接采用**。
            // 旧实现一律 fail → 上层丢掉整段流式结果、再发一次完整请求
            //（思考模型一次 ~90s，日志里每轮都翻倍）。SSE 在最后一个 chunk 之后
            // QueryDataAvailable 本来就可能返回句柄状态错误，那是正常收尾，不是故障。
            if (ShouldFinalizeStream(state, expectTools)) {
                if (callbacks.onStatus)
                    callbacks.onStatus(L"流式已结束（句柄收尾），直接采用已收内容…");
                break;
            }
            if (IsTransientHttpReceiveError(err)) {
                // 超时只是「这一窗没数据」：长超时下极少发生；句柄若真废了，下一轮
                // 会以 12019 走到上面的「已攒到内容就直接采用」分支。
                if (stopKind.load() == 1) break;
                if (stopKind.load() == 2) {
                    const std::wstring reason = currentStopReason();
                    return fail(reason.empty() ? L"流式接收中断" : reason);
                }
                Sleep(50);
                continue;
            }
            return fail(L"读取流失败：" + WinHttpErrorText(err)
                + L" (code=" + std::to_wstring(err) + L")");
        }
        if (stopKind.load() == 1) {
            if (callbacks.onStatus) callbacks.onStatus(L"按看门狗策略收束流式，采用已收内容…");
            break;
        }
        if (stopKind.load() == 2) {
            const std::wstring reason = currentStopReason();
            return fail(reason.empty() ? L"流式接收中断" : reason);
        }
        if (bytesAvailable == 0) {
            if (ShouldFinalizeStream(state, expectTools)) break;
            if (callbacks.cancelFlag && callbacks.cancelFlag->load()) return fail(L"已取消");
            if (stopKind.load() == 1) break;
            if (stopKind.load() == 2) {
                const std::wstring reason = currentStopReason();
                return fail(reason.empty() ? L"流式接收中断" : reason);
            }
            Sleep(50);
            continue;
        }
        receivedAnyByte = true;
        lastByteTickMs.store(TicksMs());
        if (callbacks.onStatus && !announcedStreamConnected) {
            announcedStreamConnected = true;
            callbacks.onStatus(L"已连接，接收流式响应…");
        }
        std::vector<char> buffer(bytesAvailable);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) || bytesRead == 0) {
            if (ShouldFinalizeStream(state, expectTools)) break;
            if (stopKind.load() == 1) break;
            if (stopKind.load() == 2) {
                const std::wstring reason = currentStopReason();
                return fail(reason.empty() ? L"流式接收中断" : reason);
            }
            continue;
        }
        FeedStreamBytes(state, buffer.data(), bytesRead, callbacks);
        // 看门狗只读原子量：这里把读线程独占的 state 投影出去
        reasoningBytes.store(state.reasoning.size());
        contentBytes.store(state.content.size());
        if (!toolAssemblyStarted.load() && (state.hasToolCalls || !state.toolCallParts.empty()))
            toolAssemblyStarted.store(true);
        if (!sawUsableToolCalls.load() && HasUsableStreamToolCalls(state))
            sawUsableToolCalls.store(true);
        if (state.done) streamDone.store(true);
    }
    watchdogStop.store(true);
    if (watchdog.joinable()) watchdog.join();
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
                    // ★只想不干 → 接下来两轮**关掉思考**：实测「空转思考」会连着来
                    //   （一轮 45s、36KB 推理，内容还都是上一轮已经想过的），
                    //   越读自己的旧推理越不肯动手。关掉思考直接逼它出手。
                    SuppressThinkingForNextRounds(2);
                    if (callbacks.onStatus)
                        callbacks.onStatus(L"思考中未调工具，已提示继续推进（下两轮关闭思考）…");
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
            std::wstring responseText;
            // ★「服务器返回空响应」是网关侧的间歇故障（思考型模型尤其常见：整段回复都是
            //   思考、正文为空时网关会回空体）。原来一次空响应就直接 return 错误 →
            //   AI 动作判失败 → **整个宏当场结束**（用户实测第十三次日志）。
            //   这里对「空响应/可重试传输错误」静默重试两次，仍然拿不到内容才报错。
            constexpr int kEmptyRetry = 2;
            for (int attempt = 0; attempt <= kEmptyRetry; ++attempt) {
                apiError.clear();
                responseText = CallApi(
                    apiBody, &apiError, callbacks.cancelFlag, callbacks.httpAbort,
                    callbacks.onStatus);
                const bool emptyResp = apiError.empty() && responseText.empty();
                if (apiError.empty() && !responseText.empty()) break;
                const bool retryable = emptyResp
                    || apiError.find(L"空响应") != std::wstring::npos
                    || apiError.find(L"连接") != std::wstring::npos
                    || apiError.find(L"超时") != std::wstring::npos
                    || apiError.find(L"timeout") != std::wstring::npos;
                if (!retryable || attempt == kEmptyRetry) break;
                if (callbacks.cancelFlag && callbacks.cancelFlag->load()) break;
                if (callbacks.onStatus) {
                    callbacks.onStatus(L"空响应/连接中断，静默重试 " + std::to_wstring(attempt + 1)
                        + L"/" + std::to_wstring(kEmptyRetry) + L"…");
                }
                Sleep(1200);
            }
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
            // 终态工具成功：脚本/录制已保存 → 直接收尾，不再发起下一轮 API。
            // 对用户只给短摘要（去掉动作一览/内部约束），并写入历史，关窗重开才看得到。
            if (terminalSucceeded) {
                std::wstring userReply = AgentUserFacingToolReply(terminalResult);
                if (userReply.empty()) userReply = L"已完成。";
                ChatMessage visible;
                visible.role = L"assistant";
                visible.content = userReply;
                messages_.push_back(std::move(visible));
                if (callbacks.onContentDelta)
                    callbacks.onContentDelta(userReply);
                else if (callbacks.onChunk)
                    callbacks.onChunk(userReply);
                return userReply;
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
