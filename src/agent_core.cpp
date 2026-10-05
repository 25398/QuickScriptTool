// ──────────────────────────────────────────────────────────────────
// agent_core.cpp — AI Agent 核心通信层实现
// WinHTTP 通信、JSON 请求构建、tool-call 自动循环
// ──────────────────────────────────────────────────────────────────

#include "agent_core.h"
#include "agent_attachment.h"
#include "agent_ai_actions.h"
#include "ai_decide.h"          // 判断表（显式依赖：别靠 macro_execute_tools.h 的间接包含）
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

/// 本机回环地址？（127.0.0.0/8 / localhost / ::1）
///
/// ⚠⚠ 回环请求**绝不能走代理**。本机设了系统代理时（实测 127.0.0.1:49755），
///   代理会替 127.0.0.1 的请求回 **502 Bad Gateway** ⇒ 「服务没连上」被报成
///   「服务器故障」，排查方向直接跑偏。
///   而 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` 是否绕过回环，取决于系统代理里
///   「对本地地址不使用代理服务器」那个勾选项 —— **不能靠运气**。
///   网页版 AI 后端的端点正是回环（`127.0.0.1:<桥端口>/v1/chat/completions`），
///   所以这里必须显式用 NO_PROXY。
bool HostIsLoopback(const std::wstring& host) {
    std::wstring h;
    h.reserve(host.size());
    for (wchar_t c : host) h.push_back(static_cast<wchar_t>(towlower(c)));
    if (h == L"localhost" || h == L"::1" || h == L"[::1]") return true;
    if (h.rfind(L"127.", 0) == 0) return true;
    return false;
}

// ── 回环连接失败的「自证」三件套（2026-10-02） ─────────────────────────────
//
// 起因：连不上本机桥时只报一句「无法与服务器建立连接」，排查只能让用户开 netstat 反推
// （已经为此往返好几轮：先怀疑 token、再怀疑端口、最后才靠截图看出"监听着却没 accept"）。
// ⇒ 连接失败且目标是回环时，把**四件事实**一次写进错误里：
//   ① 目标 host:port；② 档案 URL；③ 桥运行时文件里记的**当前实例端口**；④ **真探一次**那个端口。
//   ⚠ ④ 是"实测"不是"猜"：探到"无监听" ⇒ 没人在听；探到"有监听"却连不上 ⇒ 被拦/握手被拒。
//     两种情形的处置完全不同，所以必须区分开报。

/// 读桥自己写的运行时文件里的端口（`%LOCALAPPDATA%\QuickScriptTool\ext_bridge.json`）。
/// 用文件读而不是进程内访问器：一是**零跨层依赖**，二是它恰好证明"当前实例绑的是哪个端口"。
int ReadBridgeRuntimePort() {
    wchar_t localApp[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) == 0) return 0;
    const std::wstring path = std::wstring(localApp) + L"\\QuickScriptTool\\ext_bridge.json";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[1024]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);
    if (!ok) return 0;
    const std::string body(buf, buf + read);
    const size_t p = body.find("\"port\"");
    if (p == std::string::npos) return 0;
    size_t c = body.find(':', p);
    if (c == std::string::npos) return 0;
    ++c;
    while (c < body.size() && (body[c] == ' ' || body[c] == '\t')) ++c;
    int port = 0;
    while (c < body.size() && body[c] >= '0' && body[c] <= '9') {
        port = port * 10 + (body[c] - '0');
        ++c;
    }
    return port;
}

/// **实测**回环端口上有没有监听（不是猜）。只用于失败路径，代价可忽略。
bool TcpLoopbackListening(int port, int timeoutMs = 400) {
    if (port <= 0 || port > 65535) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 不依赖 inet_pton（本 TU 没带 ws2tcpip.h）
    bool listening = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        listening = true;
    } else {
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            fd_set w, e;
            FD_ZERO(&w);
            FD_SET(s, &w);
            FD_ZERO(&e);
            FD_SET(s, &e);
            TIMEVAL tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            if (select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w)) {
                int soErr = 0;
                int len = sizeof(soErr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
                listening = (soErr == 0);
            }
        }
    }
    closesocket(s);
    return listening;
}

/// 拼给模型/用户看的那段"为什么连不上"（只在回环 + 失败时调）。
std::wstring LoopbackConnectHint(const std::wstring& host, int port, const std::wstring& url) {
    const bool listening = TcpLoopbackListening(port);
    const int curPort = ReadBridgeRuntimePort();
    std::wstring s = L"（本机桥 " + host + L":" + std::to_wstring(port) + L" 连接失败";
    s += L"；档案 URL=" + (url.empty() ? std::wstring(L"(空)") : url);
    s += L"；桥运行时端口=" + (curPort > 0 ? std::to_wstring(curPort) : std::wstring(L"(读不到)"));
    s += listening
        ? L"；**探测该端口有监听** ⇒ 不是没人听，而是连接被拦/握手被拒（安全软件、防火墙、或接受线程已死）"
        : L"；**探测该端口无监听** ⇒ 没有进程在听它（实例已退出/端口漂移）";
    if (curPort > 0 && curPort != port) {
        s += L"；⚠ 端口不一致：档案写 " + std::to_wstring(port) + L"，当前实例是 "
            + std::to_wstring(curPort) + L" ⇒ 请求发到了错的端口";
    }
    s += L"）";
    return s;
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

/// 「思考失控闸」的连续慢轮计数。
///
/// ⚠⚠ **它必须跨 `SendMessage` 存活**（docs §39.2）。原来是 `SendMessage` 里的一个
///   `int consecutiveSlowThinkingRounds`，而 AI 动作执行是**外层每轮调一次
///   `SendMessage`**（日志里外层打 `Agent 第 N 轮`，内层打 `第 1/2 轮耗时` 并从 1 重数）
///   ⇒ 计数器每轮归零 ⇒「连续 2 轮慢才关思考」**永远不可能成立**。
///   实测第十八份日志：单轮等模型 16.2s → 62.3s，闸门一次都没触发，
///   用户看到的就是「卡在那里半天没反应」。
///   与 §26.1 那个「函数声明了却没人调」是同一类缺陷：
///   **判据活着，但它的状态活不过一轮。**
std::atomic<int> g_slowThinkingStreak{0};

/// AI 动作执行作用域计数（可重入：嵌套 aiActionExecute 会进出多次）。
/// ⚠ 它**不再**用于思考降档（那条批 D 已删）；现在是 **§42 两条协议闸**的入参 ——
///   `AiDecideStreamBrake` / `AiDecideSlowThinkingRound` 靠它区分「AI 动作执行」
///   与「聊天助手」（聊天助手的长回答是用户要的，不该被看门狗收束）。
std::atomic<int> g_aiActionExecDepth{0};

AiActionExecThinkingScope::AiActionExecThinkingScope() {
    g_aiActionExecDepth.fetch_add(1, std::memory_order_relaxed);
}
AiActionExecThinkingScope::~AiActionExecThinkingScope() {
    g_aiActionExecDepth.fetch_sub(1, std::memory_order_relaxed);
}
bool InAiActionExecScope() {
    return g_aiActionExecDepth.load(std::memory_order_relaxed) > 0;
}

void SuppressThinkingForNextRounds(int rounds) {
    if (rounds < 1) return;
    g_thinkingSuppressedRounds.store(std::clamp(rounds, 1, 8));
}

void NoteThinkingRoundConsumed() {
    int cur = g_thinkingSuppressedRounds.load();
    while (cur > 0 && !g_thinkingSuppressedRounds.compare_exchange_weak(cur, cur - 1)) {
    }
}

/// 自检用：把「抑制 N 轮」计数清零，让每个用例从确定状态开始。
/// 没有它就没法测「消耗 N 次之后一定会放开」这条不变量
/// （残留计数会让断言随用例顺序漂 —— 实测踩过）。
void ResetThinkingSuppressionForTest() {
    g_thinkingSuppressedRounds.store(0);
    g_slowThinkingStreak.store(0);
}

// ── 本轮后续工具调用数（docs §72）──────────────────────────────────────────
// 写：`SendMessage` 的工具循环（每次调用工具前）。读：宿主侧 settle 前瞻。
// 用文件级 atomic 而不是会话字段：这是**协议闸的入参**（跨 agent_core / 引擎两半），
// 状态必须活得过单次调用，且与 §39.2「闸门的状态必须活得过它要跨的那一轮」同一形状。
namespace {
std::atomic<int> g_aiToolCallsRemainingInRound{0};
}  // namespace

void NoteAiToolCallsRemainingInRound(int remaining) {
    g_aiToolCallsRemainingInRound.store(
        remaining > 0 ? remaining : 0, std::memory_order_relaxed);
}

int AiToolCallsRemainingInRound() {
    return g_aiToolCallsRemainingInRound.load(std::memory_order_relaxed);
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
    // ★原 ③「游戏前台降档」**已整条删除（批 D，docs §47.4）**：自绘画面/画布页（结构性证据、
    //   conf ≥ 0.80）曾在这里直接下发 `thinking.type=disabled`。撤销理由：
    //   **引擎不替模型决定「这一轮要不要想」** —— 想什么、想多久由模型自己定。
    //   ⚠⚠ 同函数里的 ①（抑制 N 轮）与 ②（QST_FAST_THINKING 逃生阀）**保留**：
    //      ①属**协议层**收束（与 §42 流式看门狗同族），②是外部开关、不是引擎决策。
    //   ⚠⚠ `AiActionExecThinkingScope` / `InAiActionExecScope()` 也**保留** —— 名字里带"思考"，
    //      但它是 §42 两条协议闸的入参（见 agent_core.cpp 里 inActionScopeForBrake /
    //      AiDecideSlowThinkingRound 的调用点）：删它会连带弄坏明确要保留的协议级收束。
    //   ⚠ §39.3 的「`length` 截断重试必须关思考」同样是协议修复，不在这里、也保留。
    // 只有靠 thinking 出活的网关才发这个字段（原生推理模型不接受 → 由调用点拦住）
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

// ── 历史附图的保留窗口（纯函数，供 BuildRequest 调用与自检逐格钉住）────────────
bool ShouldStripMessageImages(int msgUserRound, int lastUserRound, bool isLastUserMessage,
    int imageKeepRounds, bool stripLastUserImages) {
    // -1 = 参考资料，永不剥
    if (imageKeepRounds < 0) return false;
    // 刻意不传图的那一轮：只对「默认窗口（0）」生效 —— 观察类附图（>0）是刚刚发生的事，
    // 不是历史快照，不该被这一轮的省 token 决定顺手抹掉。
    if (stripLastUserImages && isLastUserMessage && imageKeepRounds == 0) return true;
    if (isLastUserMessage) return false;
    if (msgUserRound < 0 || lastUserRound < 0) return true;
    return (lastUserRound - msgUserRound) > imageKeepRounds;
}

// ── 构建 API 请求 ─────────────────────────────────────────────────
namespace {
/// 本线程的会话 key（见 `agent_core.h` 的说明）
thread_local std::string t_agentSessionKey;
/// ★ 见 `agent_core.h`：后台小请求（生成标题）**不该**在网页端开新对话
thread_local bool t_agentNoNewChat = false;
}  // namespace

void SetAgentSessionKeyForThisThread(const std::string& keyUtf8) {
    t_agentSessionKey = keyUtf8;
}

std::string AgentSessionKeyForThisThread() {
    return t_agentSessionKey;
}

namespace {
std::mutex g_engineKeyMu;
std::string g_engineSessionKey;
}  // namespace

void SetEngineSessionKeyOverride(const std::string& keyUtf8) {
    std::lock_guard<std::mutex> lk(g_engineKeyMu);
    g_engineSessionKey = keyUtf8;
}

std::string EngineSessionKeyOverride() {
    std::lock_guard<std::mutex> lk(g_engineKeyMu);
    return g_engineSessionKey;
}

void SetAgentNoNewChatForThisThread(bool on) {
    t_agentNoNewChat = on;
}

json AgentCore::BuildRequest(bool stripLastUserImages,
                             const std::string& toolChoice) {
    json req;
    req["model"] = ToUtf8(config_.model);
    // ★ 会话 key 走 OpenAI 标准的 `user` 字段（对真 API 无害：它只是"终端用户标识"）。
    //   网页版 AI 后端读它来**按会话分状态** —— 不然同站点的多个会话会串上下文。
    if (!t_agentSessionKey.empty()) req["user"] = t_agentSessionKey;
    // ★ 后台小请求显式声明"别开新对话"（见 `agent_core.h` 的说明）
    if (t_agentNoNewChat) req["qst_no_new_chat"] = true;
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
    // 每条消息所属的 user 轮序号（附图保留窗口按「轮」算，不按消息条数 —— 一个轮里
    // 可能插了好几条工具/引导消息）。
    std::vector<int> userRoundOf(messages_.size(), -1);
    {
        int r = -1;
        for (size_t mi = 0; mi < messages_.size(); ++mi) {
            if (messages_[mi].role == L"user") ++r;
            userRoundOf[mi] = r;
        }
    }
    const int lastUserRound = (lastUserIdx < userRoundOf.size())
        ? userRoundOf[lastUserIdx] : -1;

    // ★★**请求级**附图总预算（docs §62）：`AgentBuildImageParts` 的预算是**按一次调用**
    //   算的，而 `image_keep_rounds = 2` 让**上一轮**那条附图消息继续留在请求里
    //   ⇒ 两条消息各自合规，加起来就是两倍。实测（真机日志，3500 行那份）：
    //   `图 890 KB（4 张）`、请求体 **1009 KB**，而且随轮次单调上涨
    //   （54→122→…→1009 KB）——每一轮都在为**上一轮的旧快照**重付一次上传时间。
    //   做法：从**最新**一条带图消息往回数，累计超过预算就把更早的图**剥掉**（保新弃旧），
    //   并在原位留一句**说清原因**的话（回执不许说谎：剥了就要说剥了）。
    //   ⚠ 只剥图、不动文字 —— 历史里的工具结果与思考仍然完整。
    std::vector<char> stripByRequestBudget(messages_.size(), 0);
    {
        unsigned long long imgBytes = 0;
        for (size_t mi = messages_.size(); mi-- > 0;) {
            const auto& m = messages_[mi];
            if (m.role != L"user" || m.parts.empty()) continue;
            if (mi != 0 && mi < startIdx) continue;
            unsigned long long thisMsg = 0;
            bool hasImage = false;
            for (const auto& p : m.parts) {
                if (p.type != L"image_url") continue;
                hasImage = true;
                // data URL 的字节数 ≈ base64 长度 × 3/4；这里只用它**比较大小**，
                // 所以比例常数一致即可（不需要精确字节数）。
                thisMsg += static_cast<unsigned long long>(p.image_url.size());
            }
            if (!hasImage) continue;
            if (imgBytes > 0 && imgBytes + thisMsg > kAgentImageBudgetBytes) {
                stripByRequestBudget[mi] = 1;   // 更早的整条图剥掉（最新那条永远留）
                continue;
            }
            imgBytes += thisMsg;
        }
    }

    // 构建 messages 数组（system 恒保留；裁剪轮次跳过）
    json msgs = json::array();
    for (size_t mi = 0; mi < messages_.size(); ++mi) {
        if (mi != 0 && mi < startIdx) continue;
        const auto& m = messages_[mi];
        const bool isLastUser = (m.role == L"user" && mi == lastUserIdx);
        const bool stripThis = m.role == L"user"
            && (stripByRequestBudget[mi]
                || ShouldStripMessageImages(userRoundOf[mi], lastUserRound, isLastUser,
                       m.image_keep_rounds, stripLastUserImages));
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
                        const char* hint = stripByRequestBudget[mi]
                            ? "(这一轮的附图总量已超预算，更早的截图按「保新弃旧」省略；"
                              "**最新那一张仍在**，当前画面以它为准)"
                            : ((stripLastUserImages && mi == lastUserIdx)
                                ? "(本轮未附截图以省 token；需要看当前画面请调用 computer(action=screenshot)，"
                                  "或直接用 locateAndClick 让宿主识图定位)"
                                : "(历史截图已省略)");
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
        if (!m.reasoning_content.empty()) {
            // ★★**带 `tools` 的请求必须完整回传 `reasoning_content`**（提供方契约）。
            //   官方 DeepSeek《Thinking Mode》原话：带 `tools` 参数时，
            //   「`reasoning_content` must be **fully passed back** to the API in all
            //   subsequent requests -- even for turns where the model did not perform a
            //   tool call」，否则「the API will return a 400 error」。
            //   （不带 tools 时它被忽略，完整回传也无害 ⇒ 一律完整回传，
            //     不需要在组装消息时判断「这次带不带 tools」。）
            //
            //   历史：这里曾经有一个「通用提速」优化，把每条的推理**截成 400 字尾巴**
            //   （理由是实测请求体涨到 314~330KB、且模型读到旧推理会接着纠结）。
            //   撤销它的理由不是那两条测量错了，而是**半回传是违约的**：
            //   ① 契约明确要求「完整」，截断属于「没有正确回传」；
            //   ② 更要紧的是**它可能就是"反复重推同一件事"的成因** —— 推理被截掉后，
            //      模型每轮都只能从头再想一遍（实测那一局它把「我是僵尸怎么算赢」
            //      推了 4 遍以上）。
            //   ⇒ 体量问题改用**源头**手段治：动作执行下发 `reasoning_effort=low`
            //     （见本函数末尾），而不是事后截断。
            //   ⚠ 请求体里这块有多大，`请求体拆解` 那行一直如实报着「思考回灌 NKB」。
            msg["reasoning_content"] = ToUtf8(m.reasoning_content);
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
    // 判据（策略）与「字段真的发出去了」是两件事，分开记 —— 见 RequestBreakdown 注释。
    const bool wantDisableThinking = ShouldDisableThinking(config_.apiUrl, config_.model);
    const bool thinkingFieldSent = wantDisableThinking && !ModelIsReasoningType(config_.model);
    if (thinkingFieldSent) {
        req["thinking"] = json::object({{"type", "disabled"}});
    }

    // ★动作执行的**思考档位**降到 low。同一份官方文档里 `reasoning_effort` 取
    //   low/high/max，而**默认就是 high** —— 实测那一局每轮等模型 7~22s、单轮吐 15KB
    //   思考，8 轮下来一个僵尸都没放（它把「我是僵尸怎么算赢」推了 4 遍以上）。
    //   动作执行是**紧回路**：一轮多想 20 秒的收益，远小于多做出一次**可验证的动作**。
    //   降档 = 少想、多看回执，与「引擎只报事实、决策交给模型」是同一方向。
    //   ⚠ 三条约束：
    //     ① 只在**确实开着思考**时发（没发 thinking 字段 = 走模型默认，即开启）；
    //     ② 只在**模型官方支持**该字段时发（ModelSupportsReasoningEffort），否则可能 400；
    //     ③ **只在 AI 动作执行作用域**发 —— 聊天助手的长思考是用户要的答案质量，
    //        不能替它降档（与 §26.1③/§39.2 同一条边界）。
    const bool actionLowEffort = InAiActionExecScope() && !thinkingFieldSent
        && ModelSupportsReasoningEffort(config_.model);
    if (actionLowEffort) req["reasoning_effort"] = "low";

    // ── 请求体成分拆解（诊断用，见 agent_core.h 的 RequestBreakdown）──────────
    // 长任务里请求体一路涨；光看总 KB 判不出「是谁涨的」，于是没法决定该改哪条。
    {
        RequestBreakdown bd;
        auto kb = [](size_t bytes) { return (bytes + 1023) / 1024; };
        bd.totalKb = kb(req.dump().size());
        bd.messageCount = static_cast<int>(msgs.size());
        for (const auto& m : msgs) {
            const std::string role = m.value("role", std::string());
            const std::string text = m.contains("content") && m["content"].is_string()
                ? m["content"].get<std::string>() : std::string();
            size_t bytes = text.size();
            if (role == "system") { bd.systemKb += kb(bytes); continue; }
            if (role == "tool") { bd.toolResultsKb += kb(bytes); continue; }
            if (m.contains("reasoning_content") && m["reasoning_content"].is_string())
                bd.reasoningKb += kb(m["reasoning_content"].get<std::string>().size());
            if (m.contains("tool_calls")) bd.assistantKb += kb(m["tool_calls"].dump().size());
            if (m.contains("content") && m["content"].is_array()) {
                for (const auto& p : m["content"]) {
                    if (p.value("type", std::string()) == "image_url") {
                        ++bd.imagesKept;
                        if (p.contains("image_url"))
                            bd.imagesKb += kb(p["image_url"].dump().size());
                    } else if (p.contains("text") && p["text"].is_string()) {
                        const size_t n = p["text"].get<std::string>().size();
                        if (role == "user") bd.userTextKb += kb(n);
                        else bd.assistantKb += kb(n);
                    }
                }
            } else if (role == "user") {
                bd.userTextKb += kb(bytes);
            } else {
                bd.assistantKb += kb(bytes);
            }
        }
        if (req.contains("tools")) bd.toolsSchemaKb = kb(req["tools"].dump().size());
        bd.thinkingFieldSent = thinkingFieldSent;
        bd.reasoningEffortSent = actionLowEffort;
        bd.thinkingWhy = thinkingFieldSent
            ? L"thinking.type=disabled 已下发"
            : (ModelIsReasoningType(config_.model) ? L"原生推理模型，不发 thinking 字段"
                                                   : L"策略未要求关闭思考");
        lastBreakdown_ = bd;
    }

    return req;
}

std::wstring AgentCore::FormatLastRequestBreakdown() const {
    const RequestBreakdown& b = lastBreakdown_;
    if (b.totalKb == 0) return {};
    // ★★把**真正发出去的** max_tokens 写进这一行（docs §55）。
    //   背景：设置界面上写着 393216，而两处代码各自把它私自钳成 8192 / 16384，
    //   **任何地方都不说** ⇒ 用户看到的是「我设的数字根本不起作用」，排查时无从下手
    //   （原话：「这个预算是哪里来的？不是设置界面设置的吗？」）。
    //   口径：`ClampApiMaxTokens` 是**唯一**那份模型上限（协议要求，超了 API 直接 400），
    //   这里如实报「设置值 → 实发值」，两者不同就必须让用户看见。
    const int effectiveMaxTokens = ClampApiMaxTokens(config_.maxTokens, config_.model);
    const std::wstring maxTokensNote = effectiveMaxTokens == config_.maxTokens
        ? (L"；max_tokens=" + std::to_wstring(effectiveMaxTokens) + L"（取自设置）")
        : (L"；max_tokens=" + std::to_wstring(effectiveMaxTokens)
            + L"（设置里是 " + std::to_wstring(config_.maxTokens)
            + L"，按该模型输出上限收敛；思考也吃这份预算）");
    return L"请求体拆解 " + std::to_wstring(b.totalKb) + L"KB = system "
        + std::to_wstring(b.systemKb) + L" + 图 " + std::to_wstring(b.imagesKb)
        + L"（" + std::to_wstring(b.imagesKept) + L" 张） + 思考回灌 "
        + std::to_wstring(b.reasoningKb) + L" + 工具结果 " + std::to_wstring(b.toolResultsKb)
        + L" + assistant " + std::to_wstring(b.assistantKb)
        + L" + user 文本 " + std::to_wstring(b.userTextKb)
        + L" + 工具定义 " + std::to_wstring(b.toolsSchemaKb)
        + L"（" + std::to_wstring(b.messageCount) + L" 条消息）"
        + (b.thinkingFieldSent ? L"；thinking=关闭（" : L"；thinking=允许（")
        + b.thinkingWhy + L"）"
        + (b.reasoningEffortSent ? L"；reasoning_effort=low 已下发（动作执行紧回路）"
                                 : L"；reasoning_effort 未下发（走网关默认）")
        + maxTokensNote;
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
        // 回环（网页版 AI 的本机端点）显式免代理，见 HostIsLoopback 的长注释
        HostIsLoopback(parsed.host) ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                    : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
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

    if (!HttpSendJsonBody(hRequest, headers, body, cancelFlag, errorOut, onStatus)) {
        // ★ 回环（网页版 AI 的本机桥）失败时**自证**：host:port / 档案 URL / 桥运行时端口 /
        //   **实测探测结果** 一次写清（见 LoopbackConnectHint 的注释）。
        std::wstring msg = errorOut ? *errorOut : L"发送请求失败";
        if (HostIsLoopback(parsed.host)) {
            msg += L" " + LoopbackConnectHint(parsed.host,
                static_cast<int>(parsed.port), config_.apiUrl);
        }
        return fail(msg);
    }

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
    // 门槛与判据都搬进 ai_decide.h（kAiStreamNoToolForceMs / kAiStreamNoToolPlateauIdleMs +
    // AiDecideStreamBrake）—— 写在 lambda 里的阈值没人测得着，§29.1 的教训。
    // 工具轮：已连上/已有字节但迟迟无思考/正文/工具（常见于 SSE keepalive 空挂）→ 尽快改完整响应
    // 勿等满 API 超时（日志里会一直「流式等待 3s…42s」）
    constexpr int kEmptyStreamCapMs = 15000;
    /// AI 动作执行作用域内的「零字节等待」上限（docs §59）。
    /// 实测（真机日志）：一轮 `请求体拆解 127KB` 之后是 `流式等待 5s…10s…55s…`，
    /// **一个字节都没有** —— 用户看到的就是「放了一个僵尸就卡住了」。
    /// 之所以能等到 60s：下面那条「大请求体预填充可以很慢」的自适应（每 KB +600ms）
    /// 把 127KB 顶到了它的上限 60000ms。
    /// ⚠ 但**动作执行这一侧的实测分布不是那样**：同一局每一轮都在几秒内吐出思考字节
    /// （心跳行都带「思考 N 字节」）⇒ 60s 零字节是**离群**，不是「预填充慢」。
    /// ⚠ 而且这是**回退路径**：超时就换完整响应（另有显式的 `API 超时: 90s` 兜底），
    /// 所以收紧它不会把「慢但正常」的请求判死，只是不让用户干等一分钟。
    /// ⚠ 只收紧动作作用域 —— 聊天助手的长上下文照旧（那里的 60s 是有依据的）。
    constexpr int kAiActionEmptyStreamCapMs = 20000;

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
    // ★★动作执行侧收紧零字节等待（见 kAiActionEmptyStreamCapMs 的注释）：
    //   实测 127KB 请求体那一轮静默 55s+，用户就是「卡住了」。这里把它压到 20s，
    //   超时即走既有的完整响应回退（那条路另有 90s 显式超时 + §56 的空体恢复）。
    if (InAiActionExecScope()) {
        emptyStreamCapMs = std::min(emptyStreamCapMs, kAiActionEmptyStreamCapMs);
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
        // 回环（网页版 AI 的本机端点）显式免代理，见 HostIsLoopback 的长注释
        HostIsLoopback(parsed.host) ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                    : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
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
    if (!HttpSendJsonBody(hRequest, headers, body, callbacks.cancelFlag, &sendErr, callbacks.onStatus)) {
        // ★ 同上：回环失败要**自证**（这条是流式/工具轮的发送点）
        std::wstring msg = sendErr.empty() ? L"发送请求失败" : sendErr;
        if (HostIsLoopback(parsed.host)) {
            msg += L" " + LoopbackConnectHint(parsed.host,
                static_cast<int>(parsed.port), config_.apiUrl);
        }
        return fail(msg);
    }

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
        // 收束判据要用的作用域事实：**流开始前取一次**（前端作用域不该在流中途变），
        // 看门狗只读捕获到的常量与原子量。
        const bool inActionScopeForBrake = InAiActionExecScope();
        watchdog = std::thread([&, inActionScopeForBrake]() {
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

                // ① 期待工具、却没有任何工具调用意向 → 收束（采用已收内容，交给上层催工具）。
                //    ★★判据的宾语是「**有没有工具调用意向**」，与文本落在
                //    `reasoning_content` 还是 `content` **无关** —— 旧写法绑死 `cBytes == 0`，
                //    于是「长篇大论却不调工具」这个形态**结构上就看不见**：
                //    实测两轮各 45~55s、57 895 / 54 732 字节，全在**回复**里，
                //    只能一路烧到 max_tokens 耗尽（`finish_reason=length`），
                //    再看一次非流式重试的运气 —— 用户看到的就是「选了两张卡然后卡住不动」。
                //    判决表见 `AiDecideStreamBrake`（纯函数，可逐格自检）。
                {
                    AiStreamBrakeSignals bs;
                    bs.expectTools = expectTools;
                    bs.inAiActionScope = inActionScopeForBrake;
                    bs.sawUsableToolCalls = sawUsableToolCalls.load();
                    bs.toolAssemblyStarted = toolAssemblyStarted.load();
                    bs.gotAnyByte = gotAny;
                    bs.elapsedMs = static_cast<unsigned long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - streamStart).count());
                    bs.idleMs = idleMs;
                    bs.reasoningBytes = rBytes;
                    bs.contentBytes = cBytes;
                    const AiStreamBrakeVerdict brake = AiDecideStreamBrake(bs);
                    if (brake.kind != AiStreamBrakeKind::None) {
                        // kind=1 = 采用已收内容：上层那条「没有工具调用就不算回答」的闸接手。
                        setStop(1, FormatAiStreamBrakeStatus(brake));
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

    // ★收束与失败必须分开说：工具轮里「按判据收束」时 `AssembleStreamMessage` 会失败
    //   （`ShouldFinalizeStream` 在 expectTools 下只认工具调用），若照旧报「流式响应未包含
    //   可用内容」，日志看起来像网关故障 —— 实际是我们主动止损（docs §42）。
    if (stopKind.load() == 1) {
        result.watchdogCut = true;
        const std::wstring reason = currentStopReason();
        return fail(reason.empty() ? L"看门狗收束（本轮没有工具调用）" : reason);
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
    /// 「期待工具却只拿到散文」的纠偏次数（docs §42）：**有界**，用完就认输 ——
    /// 否则纠正与重生成会互相喂成新的死循环（§36.6/§40.2：守卫不能把模型永久锁在外面）。
    int noToolCallNudges = 0;
    /// 「网关回了空体」已兜底的次数（跨轮，有界 —— 见 AiDecideEmptyResponseRecovery）。
    int emptyResponseRecoveries = 0;
    /// 「思考失控闸」的状态见文件级 `g_slowThinkingStreak`（**不能放这里**：
    /// 这个函数每外层轮都被重新调用一次，局部变量活不过一轮，闸门就永远不触发）

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
        // 「抑制思考 N 轮」是**按请求计次**的：每发一轮就消耗一轮，否则这个计数器
        // 会永久停在 >0 —— `ShouldDisableThinking` 从此恒为 true，思考再也回不来。
        // ★这个漏调用是真出现过的：`NoteThinkingRoundConsumed()` 声明了却没人调，
        //   于是「只想不干」触发过一次之后，整个进程的后续请求都关着思考（且无日志）。
        NoteThinkingRoundConsumed();
        // 一律 auto：DeepSeek 深度思考模式不支持 tool_choice=required（会 400），
        // 且用户需要「先思考流程再实现」的分步规划，不该强制立刻调工具。
        const std::string choiceThisCall = "auto";
        // 工具续轮不再带截图（省大量 token），但仍走流式：
        // 推理模型在工具轮后还会长思考，非流式会整段生成完才返回、期间无任何反馈，
        // 前端表现像卡死；且「只思考不调工具」的纠偏只在流式路径生效。
        const bool stripImagesThisCall = (loop > 0);
        // 本轮墙钟起点：用来把「慢」拆成「等模型」与「本地动作」两段（见下）。
        const ULONGLONG roundStartTick = GetTickCount64();
        // ★本轮是否开着思考（在 `NoteThinkingRoundConsumed()` 之前取）——
        // 「思考失控闸」只在确实开着思考时计数，否则关掉思考后的正常慢响应会被
        // 当成思考失控，形成「永久关思考」的正反馈。见下面的闸门注释。
        const bool thinkingEnabledThisRound =
            !ShouldDisableThinking(config_.apiUrl, config_.model);
        // 模型已返回、开始跑工具的时点 + 本轮工具个数；收尾时算两段耗时。
        ULONGLONG toolsStartTick = 0;
        int toolCallCount = 0;
        json requestBody = BuildRequest(stripImagesThisCall, choiceThisCall);
        if (callbacks.onStatus) {
            callbacks.onStatus(loop == 0 ? L"正在连接…" : L"继续处理…");
            // 每轮打一行成分拆解：请求体涨了要能立刻看出是谁涨的
            // （历史图没剥掉 / 思考回灌 / 工具结果堆积）。
            const std::wstring bd = FormatLastRequestBreakdown();
            if (!bd.empty()) callbacks.onStatus(L"  [诊断] " + bd);
        }

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

        // ★本轮**发出去的时候**是不是开着思考：必须在任何「关思考」处置**之前**取。
        //   判据不许用「已经被改掉的当下」去解释「已经发生的事」（§38② 同形）——
        //   否则收束后刚关掉思考，回头归因就会得出「没开思考」，判断全错。
        const bool thinkingWasOnThisCall =
            !(ShouldDisableThinking(config_.apiUrl, config_.model)
              && !ModelIsReasoningType(config_.model));
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
                    // ★★被 `length` 截断 = **输出预算用光了**（docs §39.3）：重试必须一并
                    //   关闭思考，否则同一段 prompt 会**再炸一次**，白烧第二个 60s。
                    //   ⚠ 但**不许**断言是「思考」吃掉的：实测那次 57 895 字节全在
                    //   `content` 里（日志写的是「回复」），把归因写死就正好把处置瞄错对象
                    //   ——「关闭思考」对这种形态本来就无效（§41.3：日志说谎和回执说谎一样糟）。
                    //   这里只报**实测**拆分。
                    const size_t rOutBytes = ToUtf8(streamed.message.reasoning_content).size();
                    const size_t cOutBytes = ToUtf8(streamed.message.content).size();
                    auto kbOf = [](size_t n) { return (n + 1023) / 1024; };
                    SuppressThinkingForNextRounds(kAiThinkingSuppressRounds);
                    if (callbacks.onStatus)
                        callbacks.onStatus(L"输出被截断（思考 " + std::to_wstring(kbOf(rOutBytes))
                            + L"KB / 回复 " + std::to_wstring(kbOf(cOutBytes))
                            + L"KB，没有工具调用），改用完整响应重试（省略截图；并请求关闭思考 "
                            + std::to_wstring(kAiThinkingSuppressRounds) + L" 轮）…");
                }
            }
        }

        if (!parsed) {
            if (callbacks.cancelFlag && callbacks.cancelFlag->load())
                return L"[错误] 用户取消";
            // ★★看门狗收束之后的**重试必须一并关思考**（与上面 `length` 截断那条对齐）。
            //   实测事故（「我是僵尸」那一局，2026-09-22）：第 8 轮模型又吐 ~15KB 思考
            //   → 看门狗按判据收束 → 用**还开着思考**的请求重试 → 模型再次只产思考、
            //   正文为空 → 网关**回空体**（官方《Thinking Mode》写明：「`max_tokens` 不够时
            //   全部预算被推理吃光 ⇒ `content` 为空、`finish_reason=length`，
            //   这**不是** Thinking 失败」）→ 连回三次空体 → 判致命 API 失败
            //   → **整个宏当场结束**（用户看到的就是「卡在那里然后自己停了」）。
            //   ⇒ 那个专门用来救「只想不干」的闸，反过来把运行杀死了。
            //   判据很直白：**收束的含义就是「这一轮想太多了」**，所以重试必须少想。
            if (streamed.watchdogCut) {
                nonStreamRetryStripImages = true;
                SuppressThinkingForNextRounds(kAiThinkingSuppressRounds);
            }
            if (useStream && callbacks.onStatus) {
                if (!streamed.error.empty()) {
                    std::wstring hint = streamed.error;
                    if (hint.size() > 120) hint = hint.substr(0, 120) + L"...";
                    callbacks.onStatus((streamed.watchdogCut
                        ? L"已按判据收束本轮（不是故障），改用完整响应；"
                          L"本轮想太多 ⇒ 已请求关闭思考 "
                            + std::to_wstring(kAiThinkingSuppressRounds) + L" 轮（"
                        : L"流式失败，改用完整响应（") + hint + L"）");
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
            bool rebuiltForThinkingOff = false;
            for (int attempt = 0; attempt <= kEmptyRetry; ++attempt) {
                apiError.clear();
                responseText = CallApi(
                    apiBody, &apiError, callbacks.cancelFlag, callbacks.httpAbort,
                    callbacks.onStatus);
                const bool emptyResp = apiError.empty() && responseText.empty();
                if (apiError.empty() && !responseText.empty()) break;
                // ★★「空体」的判据不许绑在**报错形式**上（docs §56；§42 同一形状第四次）：
                //   旧代码只在 `apiError.empty() && responseText.empty()` 时才重建请求关思考，
                //   可传输层报空体时**会带一句错误文本**（`服务器返回空响应。`）⇒
                //   `emptyResp` 恒 false ⇒ 那条**唯一有用**的处置（关思考重建请求体）
                //   **从来没执行过**，只剩「重发同一份请求」两次。
                //   实测代价（真机日志）：`locateAndClick(target="600")` 落进 VLM 那一级后
                //   连吃 3 次空体（21.7s），工具报错、模型整轮白烧 —— 而下面这个 retryable
                //   判据**本来就认得出** `空响应`。两处必须是同一个判据。
                const bool emptyLike = emptyResp
                    || apiError.find(L"空响应") != std::wstring::npos;
                const bool retryable = emptyResp
                    || apiError.find(L"空响应") != std::wstring::npos
                    || apiError.find(L"连接") != std::wstring::npos
                    || apiError.find(L"超时") != std::wstring::npos
                    || apiError.find(L"timeout") != std::wstring::npos;
                if (!retryable || attempt == kEmptyRetry) break;
                if (callbacks.cancelFlag && callbacks.cancelFlag->load()) break;
                // ★空体且**本轮开着思考** ⇒ 下一次重试必须**真的关掉思考**。
                //   重发同一份请求没有意义：官方记载空体就是「预算被推理吃光」造成的。
                //   关思考是**改请求体**（`thinking.type=disabled`），所以必须**重建** apiBody，
                //   只翻一个标志位是自欺（字段没变，网关看到的是同一份请求）。
                if (emptyLike && thinkingWasOnThisCall && !rebuiltForThinkingOff) {
                    rebuiltForThinkingOff = true;
                    SuppressThinkingForNextRounds(kAiThinkingSuppressRounds);
                    apiBody = BuildRequest(true, choiceThisCall);
                    apiBody["stream"] = false;
                    if (callbacks.onStatus) {
                        callbacks.onStatus(L"空体且本轮开着思考 ⇒ 重建请求并关闭思考 "
                            + std::to_wstring(kAiThinkingSuppressRounds) + L" 轮后重试…");
                    }
                } else if (callbacks.onStatus) {
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
            if (responseText.empty()) {
                // ★★空体**不再判致命**。官方《Thinking Mode》写明：`max_tokens` 不够时
                //   全部预算被推理吃光 ⇒ `content` 为空、`finish_reason=length`，
                //   并明确说这**不是** Thinking 失败。
                //   旧实现 `return L"[错误] API 请求失败：无响应。"` ⇒ 调用方判失败 ⇒
                //   **整个宏当场结束**（用户看到的是「卡在那里然后自己停了」）。
                //   现在按判据表处置（`AiDecideEmptyResponseRecovery`，纯函数、可逐格自检）：
                //   动作作用域内且（开着思考 或 本轮被收束/截断）⇒ 注入一次「直接出手」
                //   纠偏 + 关思考，**有界**重来；否则才交回错误路径如实报。
                const bool afterCutOrTruncation = streamed.watchdogCut
                    || finishReason == "length";
                const AiEmptyResponseVerdict er = AiDecideEmptyResponseRecovery(
                    /*mustAct=*/expectTools && InAiActionExecScope(),
                    /*thinkingWasOn=*/thinkingWasOnThisCall,
                    /*afterCutOrTruncation=*/afterCutOrTruncation,
                    emptyResponseRecoveries);
                if (er.action == AiEmptyResponseAction::NudgeAndRetry) {
                    ++emptyResponseRecoveries;
                    SuppressThinkingForNextRounds(kAiThinkingSuppressRounds);
                    ChatMessage nudge;
                    nudge.role = L"user";
                    nudge.internal_nudge = true;
                    nudge.content =
                        L"你上一轮**没有产出任何内容**（网关回了空体：输出预算被思考吃光了）。"
                        L"这一轮请**直接调用工具**推进任务 —— 不要写解释、计划或总结；"
                        L"若确实无法推进，调用 completeTask 说明卡在哪里。";
                    messages_.push_back(std::move(nudge));
                    if (callbacks.onStatus) {
                        callbacks.onStatus(L"  [诊断] " + er.why + L"（空体兜底第 "
                            + std::to_wstring(emptyResponseRecoveries) + L" 次）");
                    }
                    continue;
                }
                return L"[错误] API 请求失败：无响应。（" + er.why + L"）";
            }

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
            std::vector<std::wstring> pendingImageLabels;
            bool terminalSucceeded = false;
            std::wstring terminalResult;
            // 模型已返回、即将开始跑工具 → 记下时点与个数（收尾时算两段耗时）
            toolsStartTick = GetTickCount64();
            toolCallCount = static_cast<int>(assistantMsg.tool_calls.size());
            int toolCallIdx = 0;
            for (const auto& tc : assistantMsg.tool_calls) {
                // ★★把「本轮后面还有几个工具调用」报给宿主（docs §72）：引擎判「这一步
                //   要不要等界面稳定」时用它。模型一次并行发 N 个点击时，每个工具调用
                //   都是**独立的一批**，不报这个数就每个都成了「最后一个交互步」
                //   ⇒ 每击一次白等一次 settle（实测一批 20 击白等 ~22s）。
                NoteAiToolCallsRemainingInRound(
                    static_cast<int>(assistantMsg.tool_calls.size()) - toolCallIdx - 1);
                ++toolCallIdx;
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
                if (!found) {
                    // ★★ **`loadTools` 是"协议里的示例"，不是真工具**（2026-09-30 实测事故）。
                    //
                    //   实测：网页端模型**照抄提示词里的示例块**发出 `loadTools`，
                    //   而工具表里没有这个名字 ⇒ 每次都得到同一句 `[错误] 未知工具：loadTools`
                    //   ⇒ 它以为是"工具路由没加载"，**连续重试到放弃**，最后写了一大段拒绝
                    //   （用户看到的"桥链路没跑通、请你自己贴题目"就是这么来的）。
                    //   `loadTools` 的真实归属：**网页协议层**（`TakeLoadToolNames` 在
                    //   `web_ai_backend` 里消化它，把目录/定义发回页面）—— 走到这里说明
                    //   它是被当作**原生工具调用**发出来的，那条路没有它的实现。
                    //   ⇒ 不报"未知工具"，直接告诉模型**别要清单、照名字直接干**（可执行）。
                    if (tc.name == L"loadTools") {
                        // ★★★ **真的回传工具清单**（2026-10-02 用户要求 + 真机事故）
                        //
                        //   ⚠⚠ 原来回的是「不需要 loadTools：工具名与用途已经在你的系统提示里」——
                        //     那是**旧时代**的事实（当时 system 里内联了紧凑清单）。
                        //     用户后来要求"system 精简、一律走查询的路子" ⇒ **清单已从 system 删掉**
                        //     ⇒ 这句话变成**谎言**：模型拿不到任何工具名，只能乱猜。
                        //     （日志实证：`findImage` 未知 → 猜 `runProgram` + 拼路径
                        //       `%USERPROFILE%\\Desktop\\绿色图标.exe` —— 把"双击图标"做成了
                        //       "运行一个猜出来的 exe"，**完全不通用**：游戏界面根本没有这种替代方案。）
                        //   ⇒ 模型主动来要，就**把清单给它**（这正是"查询的路子"的入口）。
                        std::wstring list;
                        for (const auto& t : tools_) {
                            if (t.name.empty()) continue;
                            list += L"- " + t.name;
                            if (!t.description.empty()) {
                                std::wstring d = t.description;
                                if (d.size() > 60) d = d.substr(0, 60) + L"…";
                                list += L"：" + d;
                            }
                            list += L"\n";
                        }
                        // ⚠⚠ **必须区分「工具」与「脚本动作」**（2026-10-02 真机）：
                        //   用户说"点击桌面的绿色图标" ⇒ 模型去找 `findImage` / `mouseClick`
                        //   ⇒ 清单里**没有它们**（它们是**脚本动作**，不是工具）⇒ 它反复试、
                        //   最后卡住不动（日志实证：`未知工具：findImage` → `loadTools` →
                        //   `readScriptReference` → 然后没了）。
                        //   ⇒ 清单开头必须点明这个区别，并给出"要生成脚本该走哪条路"。
                        toolResult = L"可用**工具**清单（这些才能用 [{\"action\":\"名\"}] 调用）：\n"
                                     + list
                                     + L"\n⚠ **注意区分**：`findImage` / `mouseClick` / `keyClick` / `wait` 这类"
                                       L"是**脚本动作**，**不是工具**（清单里找不到它们是对的）——"
                                       L"要生成/修改脚本，请先 [{\"action\":\"readAgentSkill\",\"section\":\"scriptStrategy\"}] "
                                       L"拿规范，再用 planScriptActions / createMacroScript 生成，"
                                       L"**不要**把动作名当工具名去调。\n"
                                       L"（工具的参数不确定就按名字调用一次，回执会告诉你缺什么）";
                    } else {
                        toolResult = L"[错误] 未知工具：" + tc.name;
                    }
                }

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

                // 工具结果带出的图片标记：多模态模型把图片编码为下一轮 user 图片消息；
                // 非多模态模型降级为路径文本提示，不报错。
                // ⚠ 措辞是**中性**的「附图」而不是「脚本图片」：同一条路现在既送
                //   `readScript` 的脚本引用图，也送 `zoom` 的放大图 —— 旧文案对着
                //   zoom 的裁剪图说「以上为脚本引用的图片，请结合工具结果理解脚本意图」，
                //   模型拿着一局游戏的放大图去「理解脚本意图」，纯属误导。
                std::wstring toolText;
                std::vector<std::wstring> imgPaths;
                if (AgentExtractImageMarkers(toolResult, toolText, imgPaths)
                    && !imgPaths.empty()) {
                    toolResult = toolText;
                    if (ModelSupportsVision(config_.model)) {
                        for (const auto& p : imgPaths) {
                            // 序号在**本轮**内连续编号（不是每条工具结果各自从 1 开始）：
                            // 一轮两张 zoom 图时，「附图 1/2」才是模型能对上的编号。
                            const size_t n = pendingImages.size() + 1;
                            const auto slash = p.find_last_of(L"\\/");
                            const std::wstring base = (slash == std::wstring::npos)
                                ? p : p.substr(slash + 1);
                            toolResult += L"\n[附图 " + std::to_wstring(n)
                                + L"] 将随下一轮请求一起送去（" + p + L"）";
                            pendingImages.push_back(p);
                            pendingImageLabels.push_back(
                                L"附图 " + std::to_wstring(n) + L"（" + base + L"）");
                        }
                    } else {
                        for (const auto& p : imgPaths) {
                            toolResult += L"\n[附图: " + p
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
                if (AgentBuildImageParts(pendingImages, imgParts, skipped, 8,
                        &pendingImageLabels)) {
                    ChatMessage imgMsg;
                    imgMsg.role = L"user";
                    // ★★ 观察类附图只保留**最近 1 个 user 轮**（值 = 2 时是「最近 2 轮」，见
                    //    `ShouldStripMessageImages`：剥掉的判据是 `lastRound - msgRound > keep`，
                    //    所以 keep=2 实际让图活到第 3 轮）。
                    //    实测教训（超星 50 题作业页那一局，51 轮 / 9.5MB 上传）：
                    //    `zoom` 每次交付 2 张长边 1280 的裁剪图，单张 22~96 KB。
                    //    同样的「图 96（2 张）」在日志里**连续出现 5 轮** —— 一次交付被计费 5 次，
                    //    10 次 zoom 交付的 232 KB 被放大成 1737 KB（占全部上传量 17.8%）。
                    //    这些图 90% 的用途是「下一轮照着点一下」，之后再重发就是纯付钱：
                    //    模型不点也不会回头翻 5 轮前的裁剪图，它会重新 zoom（那正是它一直在做的）。
                    //    ⇒ keep=1 = 只活到下一轮；要再看画面它会自己重新取，代价可控且新鲜。
                    //    ⚠ 别改成 0：那样本轮刚给的图下一轮立刻就没了，模型会陷入
                    //      「看不到图 → 再截图 → 又没了」的死循环（有前科，见下方 strip 提示文案）。
                    imgMsg.image_keep_rounds = 1;
                    imgMsg.internal_nudge = true;
                    ChatContentPart textPart;
                    textPart.type = L"text";
                    // 每张图前面都带了 `附图 N（文件名）` —— 模型据此把图和**那一次**
                    // 工具调用对上（回执里的 `[附图 N]` 是同一个编号）。
                    textPart.text = L"以下是上面工具结果附带的图片，每张前面标了它的编号与文件名"
                        L"（与工具结果里的 `[附图 N]` 一一对应；坐标口径看那条工具结果的回执）。"
                        + (skipped.empty() ? L"" : (L"\n" + skipped));
                    imgMsg.parts.push_back(std::move(textPart));
                    for (auto& p : imgParts) imgMsg.parts.push_back(std::move(p));
                    messages_.push_back(std::move(imgMsg));
                    if (callbacks.onStatus) {
                        callbacks.onStatus(L"已嵌入 " + std::to_wstring(pendingImages.size())
                            + L" 张附图（随本轮工具结果）");
                    }
                } else if (callbacks.onStatus) {
                    callbacks.onStatus(L"附图读取失败：" + skipped);
                }
            }
            // 本轮墙钟收尾：把「慢」拆成两段报出来。
            // 「等模型」= 本轮 API 往返（含思考）；「本地执行」= 工具跑完（识图/OCR/点击/settle）。
            // 实测游戏场景里前者是 5~30s、后者是秒级 —— 谁是瓶颈看这一行就够，
            // 不用再凭感觉猜该优化哪一条。
            if (callbacks.onStatus) {
                const ULONGLONG nowTick = GetTickCount64();
                const ULONGLONG roundMs = nowTick - roundStartTick;
                const ULONGLONG toolMs = nowTick - toolsStartTick;
                const ULONGLONG apiMs = (toolsStartTick >= roundStartTick)
                    ? (toolsStartTick - roundStartTick) : roundMs;
                callbacks.onStatus(L"  [诊断] 第 " + std::to_wstring(loop + 1)
                    + L" 轮耗时 " + std::to_wstring(roundMs) + L"ms = 等模型 "
                    + std::to_wstring(apiMs) + L"ms + 本地执行 " + std::to_wstring(toolMs)
                    + L"ms（" + std::to_wstring(toolCallCount) + L" 个工具）");
                // ★★思考失控闸（通用，不是游戏专属）：
                // 实测一局游戏里单轮「等模型」冲到 45s+、思考流 43KB —— 内容全是把
                // 界面常识和坐标口径反复推敲（含我们自己给的矛盾说明），零决策产出；
                // 而且**跨轮重复**：越想越不肯动手，下一轮接着想。
                // 处置与「只想不干」一致（那条路径已有成熟机制）：连读两轮异常慢就
                // 关思考 2 轮，逼它直接出手。
                // ⚠ 只在 AI 动作执行作用域内动手：聊天助手那边长思考是用户想要的东西，
                //   别去替用户「省时间」。
                // ⚠ 只在**本轮确实开着思考**时计数：关掉思考之后模型本来就慢（大 prompt
                //   的首 token 时间），那时再计数会变成永久关思考的正反馈。
                if (thinkingEnabledThisRound && InAiActionExecScope()) {
                    // 判据抽成纯函数（AiDecideSlowThinkingRound）：这样「单轮 ≥30s 当场处置」
                    // 与「连续 2 轮 ≥12s」两条门槛、以及**跨轮计数**这条契约都能被自检钉住。
                    // ★★并且把「本轮有没有出手」也传进去：这条闸的宾语是
                    //   「想很久**且**没换来动作」，而墙钟只是代理量（大请求/多图/低带宽都会长）。
                    //   实测事故：连续两轮 11.9s/16.1s 都在放大读图（正常工作）却照旧被关思考，
                    //   关掉思考的下一轮模型把两轮前那个工具批次**原样重放**、又在游戏里点了一次。
                    const AiSlowRoundVerdict slow = AiDecideSlowThinkingRound(
                        g_slowThinkingStreak.load(), apiMs, thinkingEnabledThisRound,
                        InAiActionExecScope(), /*actedThisRound=*/toolCallCount > 0);
                    g_slowThinkingStreak.store(slow.newStreak);
                    if (slow.suppress) {
                        SuppressThinkingForNextRounds(slow.suppressRounds);
                        if (callbacks.onStatus && !slow.why.empty())
                            callbacks.onStatus(L"  [诊断] " + slow.why);
                    }
                }
            }
            // 同一工具 + 相同结果：连续 2 次，或全轮出现 2 次（隔轮绕圈），注入收束引导。
            //
            // ★★但 **AI 动作执行里不注入这条**（实测事故）。判据的宾语是「有没有绕圈」，
            //   而它的观测是「同一工具 + **逐字相同**的回执」—— 在动作执行里这个形态
            //   **恰恰是正常操作**：连续点同一张卡放 5 个单位、连按同一个键、同一处补点，
            //   每一条都是模型该做的事，回执自然逐字相同。
            //   实测一局 PvZ（用户日志）：模型一轮里发了 5 组
            //   `mouseClick(卡位) + mouseClick(格子)`（**正确**打法）⇒ 第 2 组就命中判据
            //   ⇒ 引擎对正在打游戏的模型说「你已多次用相同参数调用同一工具并得到相同结果…
            //   请基于已有信息继续完成任务，或直接给出最终脚本/回答」+ 状态行
            //   「检测到重复工具调用（mouseClick），已提示停止…」。
            //   在一条正在推进任务的链路里，这句话就是在**教模型放弃**
            //   （用户看到的现象：「刚放两个僵尸就自己停了」）。
            //   ⚠ 跳过它不等于撤掉防线：动作执行那一侧本来就有**如实的事实**
            //   （`★本地观测：同一批工具已连续 M 轮完全相同（含参数）` +
            //   界面差分计数，见 ai_action_service.cpp），那是给模型自己判断的输入，
            //   而不是引擎替它下的结论 —— 与「引擎只报事实、模型自己决定怎么办」一致。
            //   聊天助手/写脚本那条路照旧保留（`listScripts` 死循环正是它要管的）。
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
            const bool repeatedToolPattern = (repeatToolCount >= 2 || repeatedPairDetected);
            if (repeatedToolPattern && InAiActionExecScope()) {
                // 只留痕：让诊断看得出「这里本来会注入引导、但按作用域跳过了」。
                if (callbacks.onStatus)
                    callbacks.onStatus(L"  [诊断] 本轮有工具被重复调用（" + repeatName
                        + L"）；动作执行里重复同一动作属正常操作 → 不注入收束引导"
                          L"（只把「同一批工具已连续 N 轮相同」作为事实报给模型）");
            } else if (repeatedToolPattern) {
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
            }
            if (repeatedToolPattern) {
                // 计数窗口重新开始（两个分支都要清，否则动作执行里每轮都会重报同一条留痕）
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

        // ★★「期待工具，却只拿到一段散文」——这段文本**不是回答**（docs §42）。
        //   旧实现到这一行就把 `assistantMsg.content` 当本轮最终回答返回（最坏再附一句
        //   「请在设置里增大 max_tokens」）。可这一轮是**被截断/被看门狗收束**的
        //   （不是模型自己 `finish_reason=stop` 收尾），那段文本是没写完的独白
        //   ⇒ AI 动作执行这一轮等于**什么都没干**，而用户看到的就是「卡在那里不动」。
        //   判据抽成纯函数 `AiDecideNoToolCallAnswer`（有界纠偏也在表里）。
        {
            const bool producedText = !assistantMsg.content.empty()
                || !assistantMsg.reasoning_content.empty();
            // 只有 AI 动作执行作用域才要求「必须有动作」：聊天助手里一大段文字回答
            // 就是正确答案，不能替它动手（与 §26.1③ / §39.2 同一条边界）。
            const bool mustAct = expectTools && InAiActionExecScope();
            const AiNoToolVerdict noTool = AiDecideNoToolCallAnswer(
                mustAct, /*hasToolCalls=*/false, producedText,
                /*endedNaturally=*/finishReason == "stop", noToolCallNudges);
            if (noTool.action == AiNoToolAction::ForceToolCall) {
                ++noToolCallNudges;
                messages_.push_back(assistantMsg);
                ChatMessage forceTool;
                forceTool.role = L"user";
                forceTool.internal_nudge = true;
                forceTool.content = L"你上一轮只输出了文字，**没有调用任何工具** —— "
                    L"在动作执行里，这段文字不算完成任何事。请立刻**直接调用**下一个工具，"
                    L"不要再写解释、计划或总结；若确实无法继续推进，"
                    L"调用 completeTask 说明卡在哪里。";
                messages_.push_back(std::move(forceTool));
                // 同一处置口径：接下来两轮关掉思考，逼它出手而不是继续推敲。
                SuppressThinkingForNextRounds(kAiThinkingSuppressRounds);
                if (callbacks.onStatus) {
                    callbacks.onStatus(L"  [诊断] " + noTool.why);
                    callbacks.onStatus(L"本轮只有文字没有工具调用，已要求它直接调工具"
                        L"（第 " + std::to_wstring(noToolCallNudges) + L" 次纠偏）…");
                }
                continue;
            }
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
