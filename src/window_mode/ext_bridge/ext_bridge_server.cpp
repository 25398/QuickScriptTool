#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// ⚠ `PROCESSENTRY32W` / `CreateToolhelp32Snapshot` 在 **tlhelp32.h** 里，
//   `windows.h` 与上面两个 winsock 头都不带它（`ext_bridge_server.cpp` 原先没有
//   任何进程枚举代码，所以从来没 include 过）。
#include <tlhelp32.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>

#include "base64.h"
#include "ext_bridge_server.h"
#include "window_mode/window_mode_log.h"

#include <algorithm>
#include <chrono>
#include <exception>   // std::exception（网页 AI 处理器兜异常用）
#include <string>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace windowmode {
// ★★ 前置声明：这几个"找浏览器 / 解析 UA"的函数定义在本文件**后面**，
//   而 `EnsureBrowserRunning`、WS hello 处理在它们之前 —— 不声明编译不过。
//   ⚠ 放在**文件顶部**（而不是靠近使用点）：使用点分散在多处，放中间必然漏。
std::wstring FindMsEdgeExe();
std::wstring FindChromeExe();
std::wstring BrowserExeFromUserAgent(const std::string& ua);
namespace {

constexpr int kPortLo = 19228;
constexpr int kPortHi = 19240;
const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// UTF-8 → 宽字符串（本文件不 include `utils.h`，且**不能**逐字节窄转宽：
/// 中文会变成乱码，日志就白写了）。仅用于异常消息这类低频诊断文本。
std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                        nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        out.data(), n, nullptr, nullptr);
    return out;
}

std::string RandomTokenHex(size_t bytes = 16) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.resize(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        const int v = dist(gen);
        out[i * 2] = hex[(v >> 4) & 0xF];
        out[i * 2 + 1] = hex[v & 0xF];
    }
    return out;
}

/// token 的**非敏感指纹**：只露最后 4 位 Hex（本机 127.0.0.1 闭环，回显它不构成泄露，
/// 但足够让用户把「扩展手里那个 token」和「桥当前这个 token」对上看）。
///
/// ⚠⚠ 存在的理由是日志本身（2026-09-27 真机）：原先的失配日志只打 `qLen/expectLen`，
///   而两端**永远是 32**（token 恒为 `RandomTokenHex(16)` ⇒ 16 字节 = 32 个十六进制字符）
///   ⇒ 那行日志的信息量恒等于零，用户拿着它只能看到「不匹配」三个字，
///   既不能判断是陈旧凭证还是根本没写进文件，也无法自证"该点重新连接"。
std::string TokenFingerprint(const std::string& tok) {
    if (tok.empty()) return "(empty)";
    constexpr size_t kTail = 4;
    const size_t n = tok.size() < kTail ? tok.size() : kTail;
    std::string out = "...";
    out += tok.substr(tok.size() - n);
    out += "(" + std::to_string(tok.size()) + ")";
    return out;
}

bool Sha1(const void* data, size_t len, unsigned char out[20]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objLen = 0, cb = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
            sizeof(objLen), &cb, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::vector<UCHAR> obj(objLen);
    bool ok = false;
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) == 0
        && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
            static_cast<ULONG>(len), 0) == 0
        && BCryptFinishHash(hash, out, 20, 0) == 0) {
        ok = true;
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string MakeWsAccept(const std::string& key) {
    const std::string src = key + kWsGuid;
    unsigned char dig[20]{};
    if (!Sha1(src.data(), src.size(), dig)) return {};
    // Base64 唯一实现见 src/base64.h（原自研实现已收敛；语义完全一致）
    return qst::base64::Encode(dig, sizeof(dig));
}

bool SendAll(SOCKET s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        const int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool RecvLine(SOCKET s, std::string& line, int timeoutMs) {
    line.clear();
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
    char ch = 0;
    while (GetTickCount() < deadline) {
        TIMEVAL tv{0, 100000};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        const int sel = select(0, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        const int n = recv(s, &ch, 1, 0);
        if (n <= 0) return false;
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line.push_back(ch);
        if (line.size() > 8192) return false;
    }
    return false;
}

bool RecvUntil(SOCKET s, std::string& out, size_t n, int timeoutMs) {
    out.clear();
    out.reserve(n);
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
    while (out.size() < n && GetTickCount() < deadline) {
        char buf[1024];
        const size_t need = n - out.size();
        const int chunk = static_cast<int>((std::min)(need, sizeof(buf)));
        TIMEVAL tv{0, 100000};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        const int r = recv(s, buf, chunk, 0);
        if (r <= 0) return false;
        out.append(buf, r);
    }
    return out.size() == n;
}

std::string ExtractHeader(const std::string& headers, const char* name) {
    const std::string key = std::string(name) + ":";
    size_t pos = 0;
    while (pos < headers.size()) {
        const size_t lineEnd = headers.find("\r\n", pos);
        const std::string line = headers.substr(pos,
            lineEnd == std::string::npos ? std::string::npos : lineEnd - pos);
        if (line.size() >= key.size()) {
            bool match = true;
            for (size_t i = 0; i < key.size(); ++i) {
                const char a = static_cast<char>(tolower(static_cast<unsigned char>(line[i])));
                const char b = static_cast<char>(tolower(static_cast<unsigned char>(key[i])));
                if (a != b) { match = false; break; }
            }
            if (match) {
                size_t v = key.size();
                while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
                return line.substr(v);
            }
        }
        if (lineEnd == std::string::npos) break;
        pos = lineEnd + 2;
    }
    return {};
}

std::string UrlDecode(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+' ) {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(s[i]);
            }
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string QueryParam(const std::string& path, const char* key) {
    const size_t q = path.find('?');
    if (q == std::string::npos) return {};
    std::string query = path.substr(q + 1);
    const std::string prefix = std::string(key) + "=";
    size_t pos = 0;
    while (pos < query.size()) {
        const size_t amp = query.find('&', pos);
        const std::string part = query.substr(pos,
            amp == std::string::npos ? std::string::npos : amp - pos);
        if (part.rfind(prefix, 0) == 0) {
            return UrlDecode(part.substr(prefix.size()));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

int ExtractJsonInt(const std::string& json, const char* key, int def = 0) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return def;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    return std::atoi(json.c_str() + pos);
}

std::string ExtractJsonString(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos;
    std::string out;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (c == '\\' && pos + 1 < json.size()) {
            out.push_back(json[pos + 1]);
            ++pos;
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

bool JsonOkTrue(const std::string& json) {
    const size_t pos = json.find("\"ok\"");
    if (pos == std::string::npos) return false;
    const size_t t = json.find("true", pos);
    const size_t f = json.find("false", pos);
    if (t == std::string::npos) return false;
    if (f != std::string::npos && f < t) return false;
    return true;
}

std::string Utf8CoreTitle(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    const char* seps[] = {" - ", " – ", " — ", " | ", "-", "|"};
    size_t cut = s.size();
    for (const char* sep : seps) {
        const size_t p = s.find(sep);
        if (p != std::string::npos && p > 0 && p < cut) cut = p;
    }
    s.resize(cut);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

/// attach 结果标题是否与 hint 对应（防止旧扩展兜底连到当前活动标签）。
bool ExtTitleMatchesHint(const std::string& hintRaw, const std::string& titleRaw) {
    if (hintRaw.empty()) return true;
    if (titleRaw.empty()) return false;
    if (titleRaw.find(hintRaw) != std::string::npos) return true;
    if (hintRaw.find(titleRaw) != std::string::npos && titleRaw.size() >= 4) return true;
    const std::string hc = Utf8CoreTitle(hintRaw);
    const std::string tc = Utf8CoreTitle(titleRaw);
    if (hc.empty() || tc.empty()) return false;
    if (tc.find(hc) != std::string::npos || hc.find(tc) != std::string::npos) return true;
    // 至少要求核心标题前 6 字节（约 2 个汉字）命中，避免「知乎」误过。
    if (hc.size() >= 6) {
        const std::string prefix = hc.substr(0, 6);
        if (tc.find(prefix) != std::string::npos) return true;
    }
    return false;
}

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return {};
    std::wstring full(path);
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return full.substr(0, slash);
}

}  // namespace

ExtBridgeServer& ExtBridgeServer::Instance() {
    static ExtBridgeServer inst;
    return inst;
}

ExtBridgeServer::~ExtBridgeServer() {
    Stop();
}

std::string ExtBridgeServer::Token() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mu_));
    return token_;
}

void ExtBridgeServer::SetScriptApiHandlers(ExtScriptApiHandlers handlers) {
    std::lock_guard<std::mutex> lock(mu_);
    scriptApi_ = std::move(handlers);
}

void ExtBridgeServer::SetWebAiHandlers(ExtWebAiHandlers handlers) {
    std::lock_guard<std::mutex> lock(mu_);
    webAi_ = std::move(handlers);
}

bool ExtBridgeServer::TokenMatchesLocked(const std::string& got) const {
    if (token_.empty()) return false;
    const size_t n = token_.size();
    unsigned char diff = (got.size() == n) ? 0 : 1;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char g = (i < got.size())
            ? static_cast<unsigned char>(got[i]) : 0;
        diff |= static_cast<unsigned char>(token_[i]) ^ g;
    }
    return diff == 0 && got.size() == n;
}

bool ExtBridgeServer::TokenMatches(const std::string& got) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mu_));
    return TokenMatchesLocked(got);
}

std::string CorsHeadersForOrigin(const std::string& origin) {
    if (origin.size() < 19 || origin.rfind("chrome-extension://", 0) != 0) return {};
    for (unsigned char c : origin) {
        if (c < 0x20 || c == 0x7F) return {};
    }
    return "Access-Control-Allow-Origin: " + origin + "\r\n"
        "Vary: Origin\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, X-Qst-Token\r\n";
}

void ExtBridgeServer::SendHttpJson(uintptr_t clientSock, int status, const std::string& body,
    const std::string& origin, bool publicOk) {
    SendHttpWithType(clientSock, status, body, origin, "application/json; charset=utf-8", publicOk);
}

void ExtBridgeServer::SendHttpWithType(uintptr_t clientSock, int status, const std::string& body,
    const std::string& origin, const std::string& contentType, bool publicOk) {
    SOCKET s = static_cast<SOCKET>(clientSock);
    const char* reason = "OK";
    if (status == 400) reason = "Bad Request";
    else if (status == 401) reason = "Unauthorized";
    else if (status == 404) reason = "Not Found";
    else if (status == 409) reason = "Conflict";
    else if (status == 500) reason = "Internal Server Error";
    else if (status == 502) reason = "Bad Gateway";
    else if (status == 503) reason = "Service Unavailable";
    std::string cors;
    if (publicOk) {
        cors = "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Private-Network: true\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Authorization, X-Qst-Token, "
            "Access-Control-Request-Private-Network\r\n";
    } else {
        cors = CorsHeadersForOrigin(origin);
        cors += "Access-Control-Allow-Private-Network: true\r\n";
    }
    std::string hdr = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
        "Content-Type: " + contentType + "\r\n"
        + cors
        + "Connection: close\r\nContent-Length: "
        + std::to_string(body.size()) + "\r\n\r\n";
    SendAll(s, hdr.data(), static_cast<int>(hdr.size()));
    if (!body.empty()) {
        SendAll(s, body.data(), static_cast<int>(body.size()));
    }
}

bool ExtBridgeServer::BindPort(std::wstring& err) {
    for (int port = kPortLo; port <= kPortHi; ++port) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closesocket(s);
            continue;
        }
        if (listen(s, 16) != 0) {
            closesocket(s);
            continue;
        }
        listenSock_ = static_cast<uintptr_t>(s);
        port_.store(port);
        err.clear();
        return true;
    }
    err = L"无法绑定本机桥端口 19228-19240";
    return false;
}


bool WriteUtf8AclFile(const std::wstring& path, const std::string& body) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)(A;;FR;;;AU)",
            SDDL_REVISION_1, &sd, nullptr)) {
        sd = nullptr;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, sd ? &sa : nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sd) LocalFree(sd);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, body.data(), static_cast<DWORD>(body.size()),
        &written, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}
/// ★★ **桥 token 跨启动保持稳定**（2026-10-02 用户报障"每次重启软件都要手点重新连接"）。
///
/// 事故链：原来每次 `Start()` 都 `token_ = RandomTokenHex(16)` ⇒ 宿主一重启，扩展侧
/// 缓存的旧 token 立刻失效（日志：`扩展桥 WS token 不匹配…请点扩展选项页「重新连接」`），
/// 而扩展**不会自动重新握手** ⇒ 用户不手点就连不上 —— 实测症状是
/// `[错误] API 请求失败：发送请求失败：无法与服务器建立连接`（整条网页 AI 链路断掉）。
///
/// 修法：把 token 落到 `%LOCALAPPDATA%\QuickScriptTool\ext_bridge_token.txt`，能读回就用它。
/// ⚠ **判据要严**（32 位十六进制，与 `RandomTokenHex(16)` 同口径）：读回一个坏 token
///   比换 token 更糟 —— 那会让扩展**永远**连不上，而且看不出原因。
std::wstring BridgeTokenStatePath() {
    wchar_t localApp[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) == 0) return std::wstring();
    std::wstring dir = std::wstring(localApp) + L"\\QuickScriptTool";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\ext_bridge_token.txt";
}

bool BridgeTokenLooksValid(const std::string& t) {
    if (t.size() != 32) return false;
    for (char c : t) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

std::string LoadPersistedBridgeToken() {
    const std::wstring path = BridgeTokenStatePath();
    if (path.empty()) return std::string();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    char buf[128]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);
    if (!ok) return std::string();
    std::string t(buf, buf + read);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ' || t.back() == '\t'))
        t.pop_back();
    size_t b = 0;
    while (b < t.size() && (t[b] == ' ' || t[b] == '\t')) ++b;
    t = t.substr(b);
    return BridgeTokenLooksValid(t) ? t : std::string();
}

std::string LoadOrCreateBridgeToken() {
    std::string t = LoadPersistedBridgeToken();
    if (!t.empty()) return t;
    t = RandomTokenHex(16);
    const std::wstring path = BridgeTokenStatePath();
    if (!path.empty() && BridgeTokenLooksValid(t)) WriteUtf8AclFile(path, t + "\n");
    return t;
}

std::wstring FindSourceExtensionEdge() {
    std::wstring dir = ModuleDir();
    for (int i = 0; i < 6; ++i) {
        const std::wstring cand = dir + L"\\extension\\edge";
        if (GetFileAttributesW((cand + L"\\background.js").c_str()) != INVALID_FILE_ATTRIBUTES
            && GetFileAttributesW((dir + L"\\CMakeLists.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
            return cand;
        }
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        dir = dir.substr(0, slash);
    }
    return {};
}

void ExtBridgeServer::WriteConfigFile() const {
    const int port = port_.load();
    std::string tok;
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mu_));
        tok = token_;
    }
    char body[512];
    std::snprintf(body, sizeof(body),
        "{\n  \"ok\": true,\n  \"port\": %d,\n  \"token\": \"%s\",\n"
        "  \"ws\": \"ws://127.0.0.1:%d/qst/ws\",\n"
        "  \"status\": \"http://127.0.0.1:%d/qst/status\"\n}\n",
        port, tok.c_str(), port, port);
    wchar_t localApp[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) != 0) {
        std::wstring dir = std::wstring(localApp) + L"\\QuickScriptTool";
        CreateDirectoryW(dir.c_str(), nullptr);
        WriteUtf8AclFile(dir + L"\\ext_bridge.json", body);
    }
    const std::wstring packed = ExtensionEdgeDirectory();
    if (!packed.empty()
        && GetFileAttributesW((packed + L"\\background.js").c_str()) != INVALID_FILE_ATTRIBUTES) {
        WriteUtf8AclFile(packed + L"\\bridge_runtime.json", body);
    }
    const std::wstring src = FindSourceExtensionEdge();
    if (!src.empty() && src != packed) {
        WriteUtf8AclFile(src + L"\\bridge_runtime.json", body);
    }
    RegisterExtNativeMessagingHost();
}

bool ExtBridgeServer::Start(std::wstring& err) {
    if (running_.load()) {
        ClearAbort();
        return true;
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = L"WSAStartup 失败";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        token_ = LoadOrCreateBridgeToken();   // ★ 跨启动稳定（见 LoadOrCreateBridgeToken 的注释）
    }

    if (!BindPort(err)) {
        WSACleanup();
        return false;
    }

    stop_.store(false);
    ClearAbort();
    running_.store(true);
    WriteConfigFile();
    WindowModeLogEventf(L"[窗口/后台窗口模式] 扩展桥已监听 127.0.0.1:%d", port_.load());

    thread_ = std::thread([this]() {
        // ★★ **看门狗**（2026-10-02）：`ThreadMain` 只在 `stop_` 置位后才该返回；
        //   若它因**任何**原因退出（异常、监听套接字被关、将来某条路径 return），
        //   这里必须把它拉起来 —— "接受线程死了但套接字还开着"正是那次
        //   "netstat 显示 LISTENING、客户端却报无法与服务器建立连接"的形态
        //   （套接字没关 ⇒ 内核照样报 LISTENING；新连接只能堆进 backlog 到失败）。
        //   ⚠ 顺带把监听套接字也补回来：只要 `stop_` 没置位，桥就必须处于"有人在听"的状态。
        while (!stop_.load()) {
            ThreadMain();
            if (stop_.load()) break;
            WindowModeLogEventf(L"[扩展桥] ★★ 接受循环意外退出 ⇒ 看门狗 500ms 后重启");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (listenSock_ == 0) {
                std::wstring err;
                if (!BindPort(err)) {
                    WindowModeLogEventf(L"[扩展桥] 看门狗重绑失败：%s", err.c_str());
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                } else {
                    WindowModeLogEventf(L"[扩展桥] 看门狗已重绑 127.0.0.1:%d", port_.load());
                    WriteConfigFile();
                }
            }
        }
    });
    err.clear();
    return true;
}

void ExtBridgeServer::FailAllPendingLocked(const std::string& result) {
    for (auto& kv : pending_) {
        if (!kv.second || kv.second->done) continue;
        kv.second->done = true;
        if (kv.second->result.empty()) kv.second->result = result;
    }
}

void ExtBridgeServer::ClearAbort() {
    abort_.store(false);
}

void ExtBridgeServer::AbortPending() {
    abort_.store(true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        FailAllPendingLocked("{\"ok\":false,\"error\":\"ABORTED\"}");
        // 打断桥线程上可能卡住的 recv，让热键中止不必等满 CDP/WS 超时。
        for (uintptr_t s : extSocks_) {
            if (s) shutdown(static_cast<SOCKET>(s), SD_BOTH);
        }
        cv_.notify_all();
    }
}

void ExtBridgeServer::RemoveSockLocked(uintptr_t sock) {
    extSocks_.erase(std::remove(extSocks_.begin(), extSocks_.end(), sock), extSocks_.end());
    if (activeExtSock_ == sock) activeExtSock_ = 0;
    extConnected_.store(!extSocks_.empty());
}

int ExtBridgeServer::ExtensionClientCount() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mu_));
    return static_cast<int>(extSocks_.size());
}

bool ExtBridgeServer::TakeLastShotJpeg(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (lastShotJpeg_.size() < 64) {
        out.clear();
        return false;
    }
    out.swap(lastShotJpeg_);
    lastShotJpeg_.clear();
    return true;
}

void ExtBridgeServer::ClearLastShotJpeg() {
    std::lock_guard<std::mutex> lock(mu_);
    lastShotJpeg_.clear();
}

void ExtBridgeServer::Stop() {
    stop_.store(true);
    abort_.store(true);
    if (listenSock_) {
        closesocket(static_cast<SOCKET>(listenSock_));
        listenSock_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (uintptr_t s : extSocks_) {
            closesocket(static_cast<SOCKET>(s));
        }
        extSocks_.clear();
        activeExtSock_ = 0;
        FailAllPendingLocked("{\"ok\":false,\"error\":\"STOPPED\"}");
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    extConnected_.store(false);
    // 脚本结束后不再接受扩展重连，避免调试窗持续刷「已连接/已断开」
}

bool ExtBridgeServer::SendWsText(uintptr_t sock, const std::string& utf8) {
    SOCKET s = static_cast<SOCKET>(sock);
    const size_t n = utf8.size();
    std::vector<unsigned char> frame;
    frame.push_back(0x81);
    if (n < 126) {
        frame.push_back(static_cast<unsigned char>(n));
    } else if (n <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<unsigned char>((n >> 8) & 0xFF));
        frame.push_back(static_cast<unsigned char>(n & 0xFF));
    } else {
        return false;
    }
    frame.insert(frame.end(), utf8.begin(), utf8.end());
    return SendAll(s, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()));
}

bool ExtBridgeServer::RecvWsText(uintptr_t sock, std::string& out, int timeoutMs) {
    out.clear();
    SOCKET s = static_cast<SOCKET>(sock);
    std::string hdr;
    if (!RecvUntil(s, hdr, 2, timeoutMs)) return false;
    const unsigned char b0 = static_cast<unsigned char>(hdr[0]);
    const unsigned char b1 = static_cast<unsigned char>(hdr[1]);
    const bool masked = (b1 & 0x80) != 0;
    uint64_t payloadLen = b1 & 0x7F;
    if (payloadLen == 126) {
        std::string ext;
        if (!RecvUntil(s, ext, 2, timeoutMs)) return false;
        payloadLen = (static_cast<unsigned char>(ext[0]) << 8)
            | static_cast<unsigned char>(ext[1]);
    } else if (payloadLen == 127) {
        return false;
    }
    if (payloadLen > 1024 * 1024) return false;
    unsigned char mask[4]{};
    if (masked) {
        std::string m;
        if (!RecvUntil(s, m, 4, timeoutMs)) return false;
        for (int i = 0; i < 4; ++i) mask[i] = static_cast<unsigned char>(m[i]);
    }
    std::string payload;
    if (payloadLen > 0 && !RecvUntil(s, payload, static_cast<size_t>(payloadLen), timeoutMs)) {
        return false;
    }
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }
    }
    const int opcode = b0 & 0x0F;
    if (opcode == 0x8) return false; // close
    if (opcode == 0x9) { // ping -> pong
        std::vector<unsigned char> pong;
        pong.push_back(0x8A);
        pong.push_back(static_cast<unsigned char>(payload.size()));
        pong.insert(pong.end(), payload.begin(), payload.end());
        SendAll(s, reinterpret_cast<const char*>(pong.data()), static_cast<int>(pong.size()));
        return RecvWsText(sock, out, timeoutMs);
    }
    if (opcode != 0x1 && opcode != 0x0) return false;
    out = std::move(payload);
    return true;
}

bool ExtBridgeServer::HandleClient(uintptr_t clientSock) {
    SOCKET s = static_cast<SOCKET>(clientSock);
    // ⚠⚠ 这里**只能**看 stop_（真关服务），**不能**看 abort_。
    //   abort_ 是「中断某次在途等待」的闩锁：StopRun() ⇒ NotifyCancel() ⇒ AbortPending()
    //   会把它置 true，而**只有 Start() 才会清**（本文件的 ClearAbort）。
    //   本桥是「常开监听」：进程启动后 Start() 只调一次，之后每次「停止脚本」都会置 abort_
    //   ⇒ 若这里也看 abort_，那么**只要用户停过一次脚本，桥就永久变聋**：
    //     新连接 TCP 能连上（内核 backlog），但 HandleClient 立刻 return false ⇒
    //     不读请求、不回任何响应 ⇒ 客户端看到的是「连接被重置」，
    //     且因为上面那行提前返回，**连日志都不会留**，进程看起来完全健康。
    //   症状与「扩展没连上 / 桥坏了」一模一样，极难定位（本项目已因此误判多次）。
    //   abort_ 的语义只该作用在「等回执」上：RequestOnSock / WaitForExtension 已经各自检查它。
    if (stop_.load()) return false;
    std::string reqLine;
    if (!RecvLine(s, reqLine, 3000)) return false;
    std::string headers;
    for (;;) {
        std::string line;
        if (!RecvLine(s, line, 3000)) return false;
        if (line.empty()) break;
        headers += line;
        headers += "\r\n";
    }

    std::string method;
    std::string path;
    {
        std::istringstream iss(reqLine);
        iss >> method >> path;
    }
    const std::string origin = ExtractHeader(headers, "Origin");

    if (method == "OPTIONS") {
        httpProbeCount_.fetch_add(1);
        SendHttpJson(clientSock, 200, "{\"ok\":true}", origin, true);
        return false;
    }

    const bool isStatus = (method == "GET" && path.rfind("/qst/status", 0) == 0);
    const bool isScripts = (method == "GET" && path.rfind("/qst/scripts", 0) == 0);
    const bool isRun = (method == "POST" && path.rfind("/qst/run", 0) == 0);
    const bool isStop = (method == "POST" && path.rfind("/qst/stop", 0) == 0);
    const bool isShot = (method == "POST" && path.rfind("/qst/shot", 0) == 0);
    const bool isWs = (method == "GET" && path.rfind("/qst/ws", 0) == 0);
    // ── 网页版 AI 后端（见 docs/web-ai-backend-design.md）────────────────
    // ⚠ 为什么挂在这个监听器上：扩展**只能被持有那条 WS 连接的进程**驱动
    //   （`Request()` 只发给连在本进程监听器上的扩展）⇒ 另起端口/另起进程都得让
    //   扩展重新连一次，等于把同一个能力实现两遍。挂同一监听器 = 零新端口/零新 token。
    const bool isChat = (method == "POST" && path.rfind("/v1/chat/completions", 0) == 0);
    const bool isWebAiProbe = (method == "POST" && path.rfind("/qst/web-ai/probe", 0) == 0);
    const bool isWindowAiProbe = (method == "POST" && path.rfind("/qst/window-ai/probe", 0) == 0);
    // ★ 窗口 Agents（设置页「准星绑定窗口」的**诊断入口**）：与桥命令同名同实现，
    //   只是为了能在没有 WebView 的情况下用 curl/脚本验证（桥命令那条链要开界面）。
    const bool isWindowAgentList = (method == "POST" && path.rfind("/qst/window-agent/list", 0) == 0);
    const bool isWindowAgentBind = (method == "POST" && path.rfind("/qst/window-agent/bind", 0) == 0);
    if (!isStatus && !isScripts && !isRun && !isStop && !isShot && !isWs
        && !isChat && !isWebAiProbe && !isWindowAiProbe && !isWindowAgentList && !isWindowAgentBind) {
        SendHttpJson(clientSock, 404, "{\"ok\":false,\"error\":\"not_found\"}", origin);
        return false;
    }

    auto readBody = [&](int maxBytes, int timeoutMs) -> std::string {
        const std::string cl = ExtractHeader(headers, "Content-Length");
        const int n = cl.empty() ? 0 : std::atoi(cl.c_str());
        if (n <= 0 || n > maxBytes) return {};
        std::string body;
        if (!RecvUntil(s, body, static_cast<size_t>(n), timeoutMs)) return {};
        return body;
    };

    // ── 网页版 AI：OpenAI 兼容端点 ─────────────────────────────────
    //   调用方是**本进程内的 AgentCore**（它以为自己在打一个 OpenAI 兼容服务）。
    //   token 三种给法都接受：Authorization: Bearer（AgentCore 走这条）、
    //   X-Qst-Token（扩展/脚本）、?token=（手工 curl）。判据与其它路由同一把尺。
    if (isChat) {
        std::string bearer = ExtractHeader(headers, "Authorization");
        if (bearer.size() > 7 && (bearer.rfind("Bearer ", 0) == 0 || bearer.rfind("bearer ", 0) == 0)) {
            bearer = bearer.substr(7);
        }
        const std::string qToken = QueryParam(path, "token");
        const std::string hToken = ExtractHeader(headers, "X-Qst-Token");
        if (!TokenMatches(bearer) && !TokenMatches(qToken) && !TokenMatches(hToken)) {
            // ★★ **不匹配就记下双方**（2026-10-02 真机"HTTP 401"）：这条原来不记日志，
            //   排查时只能猜"发来的是旧 token / 空 token / 别的实例的 token"。
            //   与 WS 那条（`扩展桥 WS token 不匹配：扩展 %s ≠ 桥 %s`）同一把尺。
            {
                std::string expect;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    expect = token_;
                }
                auto abbrev = [](const std::string& t) {
                    return t.empty() ? std::string("(空)") : t.substr(0, 8) + "...";
                };
                WindowModeLogEventf(
                    L"[扩展桥] HTTP token 不匹配：发来 bearer=%s q=%s x-token=%s ≠ 期望 %s"
                    L"（旧实例的 token / 未解密 / 档案未刷新 三者之一）",
                    Utf8ToWideLocal(abbrev(bearer)).c_str(),
                    Utf8ToWideLocal(abbrev(qToken)).c_str(),
                    Utf8ToWideLocal(abbrev(hToken)).c_str(),
                    Utf8ToWideLocal(abbrev(expect)).c_str());
            }
            SendHttpJson(clientSock, 401,
                "{\"error\":{\"type\":\"web_ai_backend_error\",\"code\":\"BAD_TOKEN\","
                "\"message\":\"token 不对：应从桥配置 ext_bridge.json 取当前 token\"}}", origin);
            return false;
        }
        // 8 MB：AI 动作执行带图 + 36 个工具定义时实测 250~340 KB，留一个数量级余量。
        // ⚠ 超限**直接拒绝**而不是截断 —— 截断的 JSON 会变成半截指令（比报错危险得多）。
        std::string body = readBody(8 * 1024 * 1024, 60000);
        if (body.empty()) {
            SendHttpJson(clientSock, 400,
                "{\"error\":{\"type\":\"web_ai_backend_error\",\"code\":\"EMPTY_BODY\","
                "\"message\":\"请求体为空，或超过 8MB 上限\"}}", origin);
            return false;
        }
        ExtWebAiHandlers h;
        {
            std::lock_guard<std::mutex> lock(mu_);
            h = webAi_;
        }
        if (!h.chat) {
            // 未注册 ⇒ 如实报「这一层没接上」，不要静默 404（排查会跑偏）
            SendHttpJson(clientSock, 501,
                "{\"error\":{\"type\":\"web_ai_backend_error\",\"code\":\"NOT_REGISTERED\","
                "\"message\":\"本进程没有注册网页版 AI 后端（SetWebAiHandlers）\"}}", origin);
            return false;
        }
        std::string resp;
        bool isSse = false;
        int status = 500;
        // ★★ 必须**兜住**处理器抛出的异常（2026-09-25 真机闪退事故）。
        //   此前这里是裸调用：处理器里任何 `throw`（典型是 `nlohmann::json::value()`
        //   遇到类型不匹配抛的 `type_error`）都会一路穿过 HandleClient 逃出线程函数
        //   ⇒ **`std::terminate` ⇒ 整个软件瞬间消失、没有任何日志、没有退出码**。
        //   ⚠ 一个 HTTP 请求的失败**绝不该**杀掉整个进程。
        //   ⚠ 捕获后必须**把原因写进日志**：否则只是把"崩溃"换成"看不懂的 500"。
        try {
            h.chat(body, resp, isSse, status);
        } catch (const std::exception& e) {
            status = 500;
            isSse = false;
            resp = std::string("{\"error\":{\"type\":\"web_ai_backend_error\",")
                 + "\"code\":\"HANDLER_EXCEPTION\","
                 + "\"message\":\"网页 AI 处理器抛出异常（已兜住，未崩溃）："
                 + JsonEscape(e.what()) + "\"}}";
            windowmode::WindowModeLogEventf(
                L"[网页AI] ★★ 处理器抛出异常（已兜住，未崩溃）：%s",
                Utf8ToWideLocal(e.what()).c_str());
        } catch (...) {
            status = 500;
            isSse = false;
            resp = "{\"error\":{\"type\":\"web_ai_backend_error\","
                   "\"code\":\"HANDLER_EXCEPTION\","
                   "\"message\":\"网页 AI 处理器抛出未知异常（已兜住，未崩溃）\"}}";
            windowmode::WindowModeLogEvent(L"[网页AI] ★★ 处理器抛出未知异常（已兜住，未崩溃）");
        }
        SendHttpWithType(clientSock, status, resp, origin,
            isSse ? "text/event-stream; charset=utf-8" : "application/json; charset=utf-8");
        return false;
    }

    // ── 网页版 AI：真机探针 ────────────────────────────────────────
    //   ⚠⚠ 这是**唯一**能真正驱动扩展的探针：外部 WS 探针进不了业务链路
    //      （桥的 HandleClient 只把入站消息当 result/ready 回执，从不转发给扩展）。
    //   默认只读（列站点/列页面/attach/探输入框/干跑写入/读回答）；
    //   带 `"send": true` 才真的发一条消息。
    if (isWebAiProbe) {
        const std::string qToken = QueryParam(path, "token");
        const std::string hToken = ExtractHeader(headers, "X-Qst-Token");
        if (!TokenMatches(qToken) && !TokenMatches(hToken)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}", origin);
            return false;
        }
        const std::string body = readBody(64 * 1024, 10000);
        ExtWebAiHandlers h;
        {
            std::lock_guard<std::mutex> lock(mu_);
            h = webAi_;
        }
        if (!h.probe) {
            SendHttpJson(clientSock, 501,
                "{\"ok\":false,\"error\":\"not_registered\","
                "\"message\":\"本进程没有注册网页版 AI 后端（SetWebAiHandlers）\"}", origin);
            return false;
        }
        // ★ 探针同样要兜异常（它是调试工具，更不能把进程带走）
        std::string rep;
        try {
            rep = h.probe(body);
        } catch (const std::exception& e) {
            rep = std::string("{\"ok\":false,\"error\":\"HANDLER_EXCEPTION\",\"message\":\"")
                + JsonEscape(e.what()) + "\"}";
            windowmode::WindowModeLogEventf(
                L"[网页AI] ★★ 探针抛出异常（已兜住，未崩溃）：%s", Utf8ToWideLocal(e.what()).c_str());
        } catch (...) {
            rep = "{\"ok\":false,\"error\":\"HANDLER_EXCEPTION\"}";
            windowmode::WindowModeLogEvent(L"[网页AI] ★★ 探针抛出未知异常（已兜住，未崩溃）");
        }
        SendHttpJson(clientSock, 200, rep, origin, true);
        return false;
    }

    // ── ★ 窗口反代探针（GUI 客户端：豆包客户端 / Cursor / 终端 TUI）──────
    //   与网页端探针**分开**：那条链是"扩展 + CDP + CSS selector"，
    //   这条是"UIA + 键鼠 + 抢前台"，出问题时要看的东西完全不同。
    //   ⚠ 它会把目标客户端**切到前台**并可能真发一条消息（仅在 send=true 时）。
    if (isWindowAiProbe) {
        const std::string qToken = QueryParam(path, "token");
        const std::string hToken = ExtractHeader(headers, "X-Qst-Token");
        if (!TokenMatches(qToken) && !TokenMatches(hToken)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}", origin);
            return false;
        }
        const std::string body = readBody(64 * 1024, 10000);
        ExtWebAiHandlers h;
        {
            std::lock_guard<std::mutex> lock(mu_);
            h = webAi_;
        }
        if (!h.windowProbe) {
            SendHttpJson(clientSock, 501,
                "{\"ok\":false,\"error\":\"not_registered\","
                "\"message\":\"本进程没有注册窗口反代探针（SetWebAiHandlers.windowProbe）\"}", origin);
            return false;
        }
        std::string rep;
        try {
            rep = h.windowProbe(body);
        } catch (const std::exception& e) {
            rep = std::string("{\"ok\":false,\"error\":\"HANDLER_EXCEPTION\",\"message\":\"")
                + JsonEscape(e.what()) + "\"}";
            windowmode::WindowModeLogEventf(
                L"[窗口AI] ★★ 探针抛出异常（已兜住，未崩溃）：%s", Utf8ToWideLocal(e.what()).c_str());
        } catch (...) {
            rep = "{\"ok\":false,\"error\":\"HANDLER_EXCEPTION\"}";
            windowmode::WindowModeLogEvent(L"[窗口AI] ★★ 探针抛出未知异常（已兜住，未崩溃）");
        }
        SendHttpJson(clientSock, 200, rep, origin, true);
        return false;
    }

    // ── ★ 窗口 Agents 诊断路由（token 同桥；实现与桥命令**共用同一函数**）──
    //   ⚠ 实现由**宿主注入**（`ExtWebAiHandlers::windowAgentsList/windowAgentBind`）：
    //     桥这一层不得直接依赖 `src/web_ai/*`（理由见头文件里那两个字段的注释）。
    if (isWindowAgentList || isWindowAgentBind) {
        const std::string qToken = QueryParam(path, "token");
        const std::string hToken = ExtractHeader(headers, "X-Qst-Token");
        if (!TokenMatches(qToken) && !TokenMatches(hToken)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}", origin);
            return false;
        }
        ExtWebAiHandlers api;
        {
            std::lock_guard<std::mutex> lock(mu_);
            api = webAi_;
        }
        const std::string body = readBody(64 * 1024, 10000);
        std::string rep;
        const bool haveHandler = isWindowAgentList
            ? static_cast<bool>(api.windowAgentsList)
            : static_cast<bool>(api.windowAgentBind);
        if (!haveHandler) {
            // 明确说清"这个进程没注册"，别静默 404 —— 与上面三条路由同一口径。
            rep = "{\"ok\":false,\"error\":\"not_registered\",\"message\":\""
                  "本进程没有注册窗口 Agents 处理器（SetWebAiHandlers）\"}";
        } else {
            try {
                rep = isWindowAgentList
                    ? api.windowAgentsList()
                    : api.windowAgentBind(body);
            } catch (const std::exception& e) {
                rep = std::string("{\"ok\":false,\"error\":\"HANDLER_EXCEPTION\",\"message\":\"")
                    + JsonEscape(e.what()) + "\"}";
            } catch (...) {
                rep = "{\"ok\":false,\"error\":\"HANDLER_EXCEPTION\"}";
            }
        }
        SendHttpJson(clientSock, 200, rep, origin, true);
        return false;
    }

    if (isShot) {
        const std::string qToken = QueryParam(path, "token");
        const std::string hToken = ExtractHeader(headers, "X-Qst-Token");
        if (!TokenMatches(qToken) && !TokenMatches(hToken)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}", origin);
            return false;
        }
        // JPEG 可达数 MB；禁止走 WebSocket 大包（会弄死 MV3 SW）。
        constexpr int kMaxShot = 12 * 1024 * 1024;
        std::string body = readBody(kMaxShot, 15000);
        if (body.size() < 64) {
            SendHttpJson(clientSock, 400, "{\"ok\":false,\"error\":\"empty_shot\"}", origin);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            lastShotJpeg_.assign(body.begin(), body.end());
        }
        cv_.notify_all();
        SendHttpJson(clientSock, 200,
            "{\"ok\":true,\"bytes\":" + std::to_string(body.size()) + "}", origin);
        return false;
    }

    if (isStatus) {
        httpProbeCount_.fetch_add(1);
        const int port = port_.load();
        ExtScriptApiHandlers api;
        {
            std::lock_guard<std::mutex> lock(mu_);
            api = scriptApi_;
        }
        bool running = false;
        std::string cur;
        if (api.getRunState) {
            const ExtRunState st = api.getRunState();
            running = st.running;
            cur = st.currentScript;
        }
        std::string body = "{\"ok\":true,\"port\":" + std::to_string(port)
            + ",\"ws\":\"ws://127.0.0.1:" + std::to_string(port) + "/qst/ws\""
            + ",\"running\":" + (running ? "true" : "false")
            + ",\"currentScript\":\"" + JsonEscape(cur) + "\""
            // ⚠ 这里补的是「扩展在不在」的唯一可外判信号。
            // 此前 status 不报扩展连接状态，而 WS 口不是给第三方探针用的（没有 ping/pong；
            // 外部连进去发 hello 会被登记成一路“扩展”，反而污染 waitingId_ 的单次等待），
            // 导致「排查扩展连没连」只能靠猜或翻日志。两个字段都只是读原子量，零副作用。
            + ",\"extConnected\":" + (extConnected_.load() ? "true" : "false")
            + ",\"extClients\":" + std::to_string(ExtensionClientCount()) + "}";
        SendHttpJson(clientSock, 200, body, origin, true);
        const int n = httpProbeCount_.load();
        if (n == 1 || (n % 8) == 0) {
            WindowModeLogEventf(L"[窗口/后台窗口模式] 扩展桥被探测 status×%d（扩展 %s，%d 路）",
                n, extConnected_.load() ? L"在线" : L"离线", ExtensionClientCount());
        }
        RegisterExtNativeMessagingHost();
        return false;
    }

    if (isScripts) {
        const std::string qToken = QueryParam(path, "token");
        if (!TokenMatches(qToken)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}", origin);
            return false;
        }
        ExtScriptApiHandlers api;
        {
            std::lock_guard<std::mutex> lock(mu_);
            api = scriptApi_;
        }
        if (!api.listScripts) {
            SendHttpJson(clientSock, 503, "{\"ok\":false,\"error\":\"no_handler\"}", origin);
            return false;
        }
        const auto scripts = api.listScripts();
        std::string body = "{\"ok\":true,\"scripts\":[";
        for (size_t i = 0; i < scripts.size(); ++i) {
            if (i) body += ',';
            body += "{\"name\":\"" + JsonEscape(scripts[i].name) + "\""
                + ",\"file\":\"" + JsonEscape(scripts[i].file) + "\""
                + ",\"path\":\"" + JsonEscape(scripts[i].path) + "\""
                + ",\"windowModeEnabled\":" + (scripts[i].windowModeEnabled ? "true" : "false")
                + ",\"windowName\":\"" + JsonEscape(scripts[i].windowName) + "\""
                + ",\"windowClassName\":\"" + JsonEscape(scripts[i].windowClassName) + "\""
                + ",\"inputStrategy\":\"" + JsonEscape(scripts[i].inputStrategy) + "\"}";
        }
        body += "]}";
        SendHttpJson(clientSock, 200, body, origin);
        return false;
    }

    if (isRun) {
        const std::string bodyIn = readBody(65536, 3000);
        const std::string tok = ExtractJsonString(bodyIn, "token");
        if (!TokenMatches(tok)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}", origin);
            return false;
        }
        std::string pathOrFile = ExtractJsonString(bodyIn, "path");
        if (pathOrFile.empty()) pathOrFile = ExtractJsonString(bodyIn, "file");
        ExtScriptApiHandlers api;
        {
            std::lock_guard<std::mutex> lock(mu_);
            api = scriptApi_;
        }
        if (!api.runScript) {
            SendHttpJson(clientSock, 503, "{\"ok\":false,\"error\":\"no_handler\"}", origin);
            return false;
        }
        std::string err;
        if (!api.runScript(pathOrFile, err)) {
            const int code = (err == "busy") ? 409 : 400;
            SendHttpJson(clientSock, code,
                "{\"ok\":false,\"error\":\"" + JsonEscape(err.empty() ? "run_failed" : err) + "\"}",
                origin);
            return false;
        }
        SendHttpJson(clientSock, 200, "{\"ok\":true}", origin);
        return false;
    }

    if (isStop) {
        const std::string bodyIn = readBody(65536, 3000);
        const std::string tok = ExtractJsonString(bodyIn, "token");
        if (!TokenMatches(tok)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}", origin);
            return false;
        }
        ExtScriptApiHandlers api;
        {
            std::lock_guard<std::mutex> lock(mu_);
            api = scriptApi_;
        }
        if (api.stopScript) api.stopScript();
        SendHttpJson(clientSock, 200, "{\"ok\":true}", origin);
        return false;
    }

    if (!stop_.load() && !abort_.load()) {
        WindowModeLogVerbose(L"[窗口/后台窗口模式] 扩展桥收到 WebSocket 升级请求");
    }
    const std::string upgrade = ExtractHeader(headers, "Upgrade");
    const std::string wsKey = ExtractHeader(headers, "Sec-WebSocket-Key");
    const std::string qToken = QueryParam(path, "token");
    // Upgrade 头可能是 "websocket"，也可能混在 Connection 里；部分环境 token 校验稍后放宽。
    const bool upgradeOk = _stricmp(upgrade.c_str(), "websocket") == 0
        || headers.find("websocket") != std::string::npos
        || headers.find("WebSocket") != std::string::npos;
    if (!upgradeOk || wsKey.empty()) {
        wsHandshakeFailCount_.fetch_add(1);
        WindowModeLogEventf(L"[窗口/后台窗口模式] 扩展桥 WS 握手失败: upgradeLen=%u keyLen=%u",
            static_cast<unsigned>(upgrade.size()), static_cast<unsigned>(wsKey.size()));
        SendHttpJson(clientSock, 400, "{\"ok\":false,\"error\":\"bad_upgrade\"}", origin);
        return false;
    }
    // ★ 判定与诊断都在**同一把锁**里（`TokenMatchesLocked`），且**只判一次**：
    //   上一版是 `!TokenMatches(qToken) || qToken != expectTok` —— 同一件事两条判据，
    //   其中一条（`TokenMatches`）自己还会再拿一次 `mu_`。
    //   ⚠ `expectTok` 仍在锁内拷出来：401 回执要把它（的指纹）写回去。
    bool tokenOk = false;
    std::string expectTok;
    {
        std::lock_guard<std::mutex> lock(mu_);
        expectTok = token_;
        tokenOk = TokenMatchesLocked(qToken);
    }
    if (!tokenOk) {
        wsHandshakeFailCount_.fetch_add(1);
        const std::string qFp = TokenFingerprint(qToken);
        const std::string eFp = TokenFingerprint(expectTok);
        WindowModeLogEventf(L"[窗口/后台窗口模式] 扩展桥 WS token 不匹配：扩展 %s ≠ 桥 %s"
            L"（宿主每次启动都会换 token；扩展侧陈旧凭证 ⇒ 请点扩展选项页「重新连接」）",
            Utf8ToWideLocal(qFp).c_str(), Utf8ToWideLocal(eFp).c_str());
        // ⚠ 回执里带上「桥当前 token 的指纹」：扩展/用户据此能立刻分清
        //   「陈旧凭证」与「文件根本没被重写」——没这条，两边都只能看到 bad_token。
        SendHttpJson(clientSock, 401,
            "{\"ok\":false,\"error\":\"bad_token\",\"gotToken\":" + JsonEscape(qFp)
                + ",\"expectToken\":" + JsonEscape(eFp) + "}", origin);
        return false;
    }

    const std::string accept = MakeWsAccept(wsKey);
    if (accept.empty()) return false;
    char resp[512];
    std::snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept.c_str());
    if (!SendAll(s, resp, static_cast<int>(strlen(resp)))) return false;

    std::string hello;
    if (!RecvWsText(clientSock, hello, 5000)) return false;
    const std::string helloType = ExtractJsonString(hello, "type");
    const std::string helloTok = ExtractJsonString(hello, "token");
    // ★ token 判据与 query 参数**同一把尺**（`TokenMatchesLocked`），并且**要求二者一致**：
    //   上一版是「hello 的 token 只要匹配桥当前 token 就收」——
    //   于是带着**陈旧 query token** 的连接、只要 hello 里塞了正确 token 也能进（反之亦然），
    //   两条通道各判一半，出问题时分不清是哪一条在拒。
    bool helloOk = false;
    std::string bridgeTok;
    {
        std::lock_guard<std::mutex> lock(mu_);
        bridgeTok = token_;
        helloOk = helloType == "hello" && TokenMatchesLocked(helloTok)
            && helloTok == qToken;
    }
    if (!helloOk) {
        wsHandshakeFailCount_.fetch_add(1);
        // ⚠ 指纹统一从**上面锁内拷出来的** `bridgeTok` 算：
        //   `Token()` 自己要拿 `mu_`，它**不是**可重入的 —— 在锁内调它就是自锁。
        WindowModeLogEventf(L"[窗口/后台窗口模式] 扩展桥 WS hello 校验失败：type=%s "
            L"helloToken=%s queryToken=%s 桥=%s",
            Utf8ToWideLocal(helloType).c_str(),
            Utf8ToWideLocal(TokenFingerprint(helloTok)).c_str(),
            Utf8ToWideLocal(TokenFingerprint(qToken)).c_str(),
            Utf8ToWideLocal(TokenFingerprint(bridgeTok)).c_str());
        return false;
    }

    // ★★ 记下"**上次连过桥的浏览器**"（2026-09-26 用户要求：一键式启动时优先用它）。
    //   扩展拿不到自己的 exe 路径（浏览器安全限制）⇒ 只能报 `userAgent`，桥据此推断。
    //   ⚠ 推断不出来就**保持原值**（不要清空：上次的记录仍然有价值）。
    {
        const std::string ua = ExtractJsonString(hello, "browser");
        if (!ua.empty()) {
            const std::wstring exe = BrowserExeFromUserAgent(ua);
            if (!exe.empty()) {
                std::lock_guard<std::mutex> lock(mu_);
                if (lastBrowserExe_ != exe) {
                    lastBrowserExe_ = exe;
                    WindowModeLogEventf(L"[扩展桥] 记下上次连过桥的浏览器：%s", exe.c_str());
                }
            }
        }
    }

    int clientCount = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        extSocks_.push_back(clientSock);
        extConnected_.store(true);
        clientCount = static_cast<int>(extSocks_.size());
        cv_.notify_all();
    }
    if (!stop_.load() && !abort_.load()) {
        WindowModeLogEventf(L"[窗口/后台窗口模式] 配套扩展已连接本机桥（当前 %d 路）", clientCount);
    }

    while (!stop_.load()) {
        TIMEVAL tv{0, 200000};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        const int sel = select(0, &fds, nullptr, nullptr, &tv);
        if (sel < 0) break;
        if (sel == 0) continue;

        std::string msg;
        if (!RecvWsText(clientSock, msg, 5000)) break;

        const int id = ExtractJsonInt(msg, "id", -1);
        const std::string type = ExtractJsonString(msg, "type");
        if (type == "result" || type == "ready") {
            std::lock_guard<std::mutex> lock(mu_);
            // ★ 按 **id** 找等待者（不是"当前那个全局等待"）⇒ 并发请求各拿各的回执
            auto it = pending_.find(id);
            if (it != pending_.end() && it->second) {
                it->second->result = msg;
                it->second->done = true;
            }
            cv_.notify_all();
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        RemoveSockLocked(clientSock);
        FailAllPendingLocked("{\"ok\":false,\"error\":\"DISCONNECTED\"}");
        cv_.notify_all();
        clientCount = static_cast<int>(extSocks_.size());
    }
    if (!stop_.load() && !abort_.load()) {
        WindowModeLogVerbosef(L"[窗口/后台窗口模式] 配套扩展已断开本机桥（剩余 %d 路）", clientCount);
    }
    return true;
}

void ExtBridgeServer::ThreadMain() {
    SOCKET listen = static_cast<SOCKET>(listenSock_);
    auto lastNativeReg = std::chrono::steady_clock::now();
    while (!stop_.load()) {
        // ★★ **接受循环绝不允许被异常打死**（2026-10-02 真机实测"监听着却连不上"）。
        //
        //   事故形态：`netstat` 显示 `127.0.0.1:19228 LISTENING`、进程也在，
        //   但客户端三次都是 `[错误] API 请求失败：发送请求失败：无法与服务器建立连接`
        //   —— 因为**没人 accept**：套接字还开着（内核照样显示 LISTENING），
        //   新连接只能堆在 backlog（`listen(s, 16)`）里，堆满即失败。
        //   而这里原来每来一个连接就 `std::thread(...).detach()`：
        //   ① `std::thread` 构造可能抛（线程/句柄耗尽、CRT 收尾期）⇒ 异常**逃出本循环**
        //      ⇒ 接受线程当场结束，**从此再没有任何人 accept**（症状与上面完全一致）；
        //   ② 分离线程里的 `HandleClient` 若抛异常，未被捕获就是 `std::terminate`
        //      ⇒ 整个进程消失（本文件 845 行那段注释早写过 `type_error` 会逃出 HandleClient）。
        //   ⇒ 两道 try/catch 一起加：外层保"接受循环永不退出"，内层保"单连接异常不杀进程"。
        try {
            TIMEVAL tv{0, 200000};
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(listen, &fds);
            const int sel = select(0, &fds, nullptr, nullptr, &tv);
            if (sel <= 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastNativeReg > std::chrono::seconds(15)) {
                    lastNativeReg = now;
                    RegisterExtNativeMessagingHost();
                }
                continue;
            }
            sockaddr_in peer{};
            int peerLen = sizeof(peer);
            SOCKET client = accept(listen, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (client == INVALID_SOCKET) continue;

            // status 请求很快结束；WS 连接占用线程 —— 用短线程处理
            auto serve = [this, client]() {
                try {
                    HandleClient(static_cast<uintptr_t>(client));
                } catch (const std::exception& e) {
                    WindowModeLogEventf(L"[扩展桥] 连接处理异常（已隔离，不影响后续连接）：%s",
                        Utf8ToWideLocal(e.what()).c_str());
                } catch (...) {
                    WindowModeLogEventf(L"[扩展桥] 连接处理未知异常（已隔离）");
                }
                closesocket(client);
            };
            try {
                std::thread(serve).detach();
            } catch (const std::exception& e) {
                // 起不了线程就**就地处理**：宁可这一条连接慢一点，也不能让接受循环死掉
                WindowModeLogEventf(L"[扩展桥] 起线程失败（%s）⇒ 就地处理该连接",
                    Utf8ToWideLocal(e.what()).c_str());
                serve();
            }
        } catch (const std::exception& e) {
            WindowModeLogEventf(L"[扩展桥] 接受循环异常（已隔离，继续监听）：%s",
                Utf8ToWideLocal(e.what()).c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        } catch (...) {
            WindowModeLogEventf(L"[扩展桥] 接受循环未知异常（已隔离，继续监听）");
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    }
}

void ExtBridgeServer::RefreshDiscovery() {
    if (!running_.load()) return;
    WriteConfigFile();
}

namespace {

/// 自检 / 探针 / 播放器进程**绝不允许**去启动浏览器
/// —— 否则跑一次自检就弹一个浏览器窗口（那会让 CI 和本地回归都变成灾难）。
bool ShouldNotLaunchBrowser() {
    wchar_t path[MAX_PATH]{};
    if (!::GetModuleFileNameW(nullptr, path, MAX_PATH)) return true;
    std::wstring p(path);
    for (const wchar_t* s : {L"SelfTest", L"selftest", L"Probe", L"probe",
                             L"QstPlayer", L"player"}) {
        if (p.find(s) != std::wstring::npos) return true;
    }
    return false;
}

/// 毫秒时钟（steady，避免受系统时间调整影响）
long long BridgeNowMs() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// 这些进程名任一在跑 ⇒ 「浏览器已经开着」。**按整名精确比**（不做子串），
/// 避免 `msedgewebview2.exe`（WebView2 宿主）被误算成浏览器。
/// ⚠ 大小写无关：`PROCESSENTRY32W` 给的名字大小写不保证（实测有 `MSEDGE.EXE`）。
bool IsKnownBrowserLeafName(const wchar_t* rawName) {
    if (!rawName || !rawName[0]) return false;
    std::wstring leaf(rawName);
    for (auto& ch : leaf) ch = static_cast<wchar_t>(towlower(ch));
    static const wchar_t* kNames[] = {
        L"msedge.exe", L"chrome.exe", L"firefox.exe", L"brave.exe",
        L"opera.exe", L"vivaldi.exe", L"360se.exe", L"360chrome.exe",
        L"qqbrowser.exe", L"sogouexplorer.exe",
    };
    for (const wchar_t* n : kNames) {
        if (leaf == n) return true;
    }
    return false;
}

/// 本机**是否已经有浏览器在跑**。
///
/// ★★ 为什么 `EnsureBrowserRunning` 必须先问这一句（2026-09-27 真机）：
///   原先它无条件 `ShellExecute(msedge.exe)`，而 Edge 已经在跑时这一下**不会**复用旧窗 ——
///   实测表现为用户看到「又开了一个浏览器窗口 / 新标签页」，即使脚本的目标窗口
///   （缩略图里那个游戏页）就在旁边好好的。
///   用户原话：「缩略图已经定位到目标窗口了，可是还是打开了新标签页」。
///   浏览器**已经在跑**时，唯一缺的东西是「扩展拿到当前 token」——
///   那是扩展侧 2s 轮询自己会解决的事（配合 `WriteConfigFile` 重写凭证），
///   再拉一个浏览器实例既帮不上忙，又正好制造了用户看到的那一幕。
///
/// ⚠ 判据取「任一已知浏览器进程在跑」而不是「目标 exe 在跑」：
///   `lastBrowserExe_` 只在扩展连过桥之后才有值，而这里恰恰是**没连上**的场景
///   ⇒ 拿它当判据会在最需要它的那一刻恒为空。
bool AnyBrowserProcessRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        // ⚠ 查不到 ≠ 没有：宁可当作"有浏览器"（= 不新开窗口），也不要因为一次
        //   快照失败就退回"无条件拉一个"那条会制造重复窗口的老路。
        return true;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (IsKnownBrowserLeafName(pe.szExeFile)) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

}  // namespace

/// 自检用：把「怎么判浏览器已经在跑」暴露成**纯函数**。
///
/// ★ 为什么要有它：`AnyBrowserProcessRunning()` 读的是**本机真实进程表**，
///   自检没法在不真的开一个 Edge 的前提下验它 —— 而"没法验"的判据正是过去
///   吃亏最多的地方（写死一个恒真/恒假的谓词，没人发现）。
///   拆出纯函数后，用例可以逐个钉：`msedge.exe` 算、`MSEDGE.EXE`（大小写）算、
///   `msedgewebview2.exe` **不算**（它是 WebView2 宿主，把它算成浏览器会让
///   产品在用户根本没开浏览器时拒绝拉起浏览器 —— 静默失效）、
///   自家 `QuickScriptTool.exe` 不算、空名不算。
bool ExtBrowserLeafNameIsBrowser(const wchar_t* leafName) {
    return IsKnownBrowserLeafName(leafName);
}

bool ExtBridgeServer::EnsureBrowserRunning() {
    if (ShouldNotLaunchBrowser()) return false;

    // ⚠ 限流 60 秒：这个函数在**每次** `WaitForExtension` 失败时都会被调到
    const long long now = BridgeNowMs();
    const long long prev = lastLaunchAtMs_.load();
    if (prev > 0 && now - prev < 60000) return false;

    // ★★ 浏览器本来就开着 ⇒ **一个进程都不要拉**（见 `AnyBrowserProcessRunning` 的注释）。
    //   这里**不更新限流时间戳**：我们什么都没启动，限流管的是"启动"，不该被空跑吃掉配额。
    //   缺的只是 token ⇒ 把凭证文件重写一遍（扩展 2s 轮询会读到），然后回去等它自己连上。
    if (AnyBrowserProcessRunning()) {
        WriteConfigFile();
        // 打包 CRX 读不到 extension\edge\bridge_runtime.json，只能靠 native host 拿凭证
        // ⇒ 顺手把清单也重注册一遍（与 `WriteConfigFile` 里那条同一入口，不另写逻辑）。
        RegisterExtNativeMessagingHost();
        // ⚠ 指纹从锁内拷一份再算：`Token()` 自己要拿 `mu_`，这里没有持有它，
        //   但把"读 token"与"打日志"混在一句话里最容易在下次改动时踩到重入。
        std::string tok;
        {
            std::lock_guard<std::mutex> lock(mu_);
            tok = token_;
        }
        WindowModeLogEvent(
            L"[扩展桥] 扩展离线但浏览器已在运行 ⇒ 不新开窗口（避免重复标签页）；"
            L"已重写桥凭证，扩展轮询会自行重连");
        WindowModeLogEventf(L"[扩展桥] 桥端口=%d token=%s；若 10 秒内仍不连，"
            L"请到扩展选项页点「重新连接」",
            port_.load(), Utf8ToWideLocal(TokenFingerprint(tok)).c_str());
        return false;
    }
    lastLaunchAtMs_.store(now);

    std::wstring exe;
    {
        std::lock_guard<std::mutex> lock(mu_);
        exe = lastBrowserExe_;
    }
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        exe = FindMsEdgeExe();          // 没有记录 ⇒ 回退到 Edge
    }
    if (exe.empty()) {
        WindowModeLogEventf(L"[扩展桥] 扩展离线，但没找到可启动的浏览器（Edge）");
        return false;
    }

    // ⚠⚠⚠ **绝对不要用 `--no-startup-window`**（2026-09-26 实测踩坑）：
    //   无窗口的浏览器实例**不会唤醒扩展的 service worker** ⇒
    //   浏览器确实起来了（日志里 `已发起`），但桥这边**永远等不到扩展**
    //   （实证：19:40:06 发起 ⇒ 19:40:29 仍是 `NO_EXTENSION`）⇒ 必须**带窗口**启动。
    //
    //   ⚠ 与「不抢前台」的取舍：用户抱怨的是**每次请求都抢**（那是 activateTabDuringRun）；
    //     这里是「浏览器本来就没开、软件帮你开一次」⇒ 开一次可接受，
    //     用 SW_SHOWMINNOACTIVE（最小化、不激活）尽量少打扰。
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(),
                                      nullptr, nullptr, SW_SHOWMINNOACTIVE);
    const bool ok = reinterpret_cast<INT_PTR>(r) > 32;
    WindowModeLogEventf(L"[扩展桥] 扩展离线 ⇒ 后台启动浏览器：%s（%s）",
        exe.c_str(), ok ? L"已发起" : L"失败");
    return ok;
}

bool ExtBridgeServer::WaitForExtension(int timeoutMs, std::wstring& err) {
    {
        std::unique_lock<std::mutex> lock(mu_);
        if (!extSocks_.empty()) {
            err.clear();
            return true;
        }
    }

    // ★★ 一键式（2026-09-26 用户要求）：扩展不在 ⇒ **软件自己把浏览器拉起来**，
    //   再给它一次机会。用户不该为了"浏览器没开"去看错误提示、自己去开。
    //   ⚠ 限流在 `EnsureBrowserRunning` 里（60s）；自检进程直接返回 false。
    const bool launched = EnsureBrowserRunning();
    // 冷启动浏览器 + 扩展 SW 起来 + 连桥，实测要几秒到十几秒 ⇒ 多给 20 秒
    const int waitMs = timeoutMs + (launched ? 35000 : 0);

    std::unique_lock<std::mutex> lock(mu_);
    const auto ok = cv_.wait_for(lock, std::chrono::milliseconds(waitMs), [&]() {
        return !extSocks_.empty() || abort_.load() || stop_.load();
    });
    if (abort_.load() || stop_.load()) {
        err = L"已取消";
        return false;
    }
    if (!ok || extSocks_.empty()) {
        err = L"NO_EXTENSION";
        return false;
    }
    err.clear();
    return true;
}

bool ExtBridgeServer::RequestOnSock(uintptr_t sock, const std::string& type,
    const std::string& extraJsonFields, std::string& resultJson,
    std::wstring& err, int timeoutMs) {
    resultJson.clear();
    int id = 0;
    // ★ 自己的等待槽（**按 id 关联**）—— 并发调用各持一个，互不干扰
    auto slot = std::make_shared<WaitSlot>();
    {
        std::unique_lock<std::mutex> lock(mu_);
        id = nextId_++;
        pending_[id] = slot;
    }

    std::string msg = "{\"id\":" + std::to_string(id) + ",\"type\":\"" + type + "\"";
    if (!extraJsonFields.empty()) {
        msg += ",";
        msg += extraJsonFields;
    }
    msg += "}";

    if (!SendWsText(sock, msg)) {
        err = L"扩展桥发送失败";
        std::lock_guard<std::mutex> lock(mu_);
        pending_.erase(id);
        return false;
    }

    std::unique_lock<std::mutex> lock(mu_);
    // 分片等待：热键 Abort 后最多约 50ms 退出，避免单次 wait_for(20s) 漏唤醒时卡死。
    bool ok = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        if (slot->done || abort_.load() || stop_.load()) {
            ok = true;
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        auto slice = deadline - now;
        if (slice > std::chrono::milliseconds(50)) {
            slice = std::chrono::milliseconds(50);
        }
        if (cv_.wait_for(lock, slice, [&]() {
            return slot->done || abort_.load() || stop_.load();
        })) {
            ok = true;
            break;
        }
    }
    pending_.erase(id);
    if (abort_.load() || stop_.load()) {
        err = L"已取消";
        return false;
    }
    if (!ok || !slot->done) {
        err = L"扩展桥响应超时";
        return false;
    }
    resultJson = slot->result;
    if (!JsonOkTrue(resultJson)) {
        const std::string code = ExtractJsonString(resultJson, "error");
        const std::string message = ExtractJsonString(resultJson, "message");
        err = code.empty() ? L"扩展桥命令失败" : std::wstring(code.begin(), code.end());
        if (!message.empty()) {
            // message 可能是 UTF-8；错误码用 ASCII，附加原文供日志。
            err += L": ";
            const int n = MultiByteToWideChar(CP_UTF8, 0, message.c_str(),
                static_cast<int>(message.size()), nullptr, 0);
            if (n > 0) {
                std::wstring w(static_cast<size_t>(n), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, message.c_str(),
                    static_cast<int>(message.size()), w.data(), n);
                err += w;
            }
        }
        return false;
    }
    err.clear();
    return true;
}

bool ExtBridgeServer::Request(const std::string& type, const std::string& extraJsonFields,
    std::string& resultJson, std::wstring& err, int timeoutMs) {
    resultJson.clear();
    std::vector<uintptr_t> socks;
    uintptr_t preferred = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (extSocks_.empty()) {
            err = L"NO_EXTENSION";
            return false;
        }
        socks = extSocks_;
        preferred = activeExtSock_;
    }

    if (type == "attach") {
        const std::string hint = ExtractJsonString(
            std::string("{") + extraJsonFields + "}", "titleHint");
        std::string lastFail;
        std::string mergedCandidates;
        WindowModeLogf(L"[窗口/后台窗口模式] 扩展桥 attach：向 %d 路扩展逐个尝试",
            static_cast<int>(socks.size()));

        // 先试上次成功的连接，再试其余（倒序：较新连接优先）。
        std::vector<uintptr_t> order;
        if (preferred) order.push_back(preferred);
        for (auto it = socks.rbegin(); it != socks.rend(); ++it) {
            if (*it != preferred) order.push_back(*it);
        }

        for (uintptr_t sock : order) {
            std::string one;
            std::wstring oneErr;
            if (!RequestOnSock(sock, "attach", extraJsonFields, one, oneErr, timeoutMs)) {
                lastFail = one.empty() ? WideToUtf8(oneErr) : one;
                if (one.find("\"candidates\"") != std::string::npos) {
                    mergedCandidates = one;
                }
                continue;
            }
            const std::string title = ExtractJsonString(one, "title");
            const std::string ver = ExtractJsonString(one, "version");
            if (!ExtTitleMatchesHint(hint, title)) {
                auto u8 = [](const std::string& u) -> std::wstring {
                    if (u.empty()) return L"?";
                    const int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(),
                        static_cast<int>(u.size()), nullptr, 0);
                    if (n <= 0) return L"?";
                    std::wstring w(static_cast<size_t>(n), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, u.c_str(),
                        static_cast<int>(u.size()), w.data(), n);
                    return w;
                };
                const std::wstring titleW = u8(title);
                const std::wstring verW = u8(ver);
                WindowModeLogf(L"[窗口/后台窗口模式] 扩展桥跳过错误标签: %s（version=%s）",
                    titleW.c_str(), verW.c_str());
                std::string ignore;
                std::wstring ignoreErr;
                RequestOnSock(sock, "detach", "", ignore, ignoreErr, 2000);
                lastFail = one;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                activeExtSock_ = sock;
            }
            resultJson = one;
            err.clear();
            return true;
        }

        resultJson = mergedCandidates.empty() ? lastFail : mergedCandidates;
        err = L"NO_TAB";
        if (!hint.empty()) {
            err += L": 所有已连接扩展都未能 attach 到匹配标签";
        }
        return false;
    }

    uintptr_t sock = preferred;
    if (!sock && !socks.empty()) sock = socks.back();
    if (!sock) {
        err = L"NO_EXTENSION";
        return false;
    }
    return RequestOnSock(sock, type, extraJsonFields, resultJson, err, timeoutMs);
}

std::wstring ExtensionEdgeDirectory() {
    return ModuleDir() + L"\\extension\\edge";
}

std::wstring FindMsEdgeExe() {
    wchar_t buf[MAX_PATH]{};
    const wchar_t* keys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe",
    };
    for (const wchar_t* key : keys) {
        HKEY hkey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
            continue;
        }
        DWORD typ = 0;
        DWORD cb = sizeof(buf);
        const LONG ok = RegQueryValueExW(hkey, nullptr, nullptr, &typ,
            reinterpret_cast<LPBYTE>(buf), &cb);
        RegCloseKey(hkey);
        if (ok == ERROR_SUCCESS && (typ == REG_SZ || typ == REG_EXPAND_SZ) && buf[0]) {
            return buf;
        }
    }
    const wchar_t* fallbacks[] = {
        L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    };
    for (const wchar_t* p : fallbacks) {
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return {};
}

/// 找 Chrome 的 exe（注册表 App Paths + 常见安装路径兜底）。
///
/// ⚠ 与 `FindMsEdgeExe()` 同一套做法：**别只硬编码路径** —— 用户可能装在
/// 非默认盘 / 用户级安装（`%LOCALAPPDATA%\Google\Chrome\Application`）。
std::wstring FindChromeExe() {
    wchar_t buf[MAX_PATH]{};
    const wchar_t* keys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe",
    };
    for (const wchar_t* key : keys) {
        HKEY hkey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hkey) != ERROR_SUCCESS) continue;
        DWORD typ = 0;
        DWORD cb = sizeof(buf);
        const LONG r = RegQueryValueExW(hkey, nullptr, nullptr, &typ,
            reinterpret_cast<LPBYTE>(buf), &cb);
        RegCloseKey(hkey);
        if (r == ERROR_SUCCESS && (typ == REG_SZ || typ == REG_EXPAND_SZ) && buf[0]) {
            return buf;
        }
    }
    // 用户级安装（Chrome 默认就装在这儿）
    wchar_t localApp[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) != 0) {
        const std::wstring p1 = std::wstring(localApp)
            + L"\\Google\\Chrome\\Application\\chrome.exe";
        if (GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES) return p1;
    }
    const wchar_t* fallbacks[] = {
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
    };
    for (const wchar_t* q : fallbacks) {
        if (GetFileAttributesW(q) != INVALID_FILE_ATTRIBUTES) return q;
    }
    return {};
}

/// 从扩展自报的 `userAgent` 推断用哪个浏览器启动。
/// ⚠ 扩展**拿不到自己的 exe 路径**（浏览器安全限制）⇒ 只能靠 UA 里的品牌串。
///   返回空串 = 认不出（调用方回退到 Edge）。
std::wstring BrowserExeFromUserAgent(const std::string& ua) {
    if (ua.find("Edg/") != std::string::npos || ua.find("EdgA/") != std::string::npos
        || ua.find("EdgiOS/") != std::string::npos) {
        return FindMsEdgeExe();
    }
    if (ua.find("Chrome/") != std::string::npos) {
        return FindChromeExe();
    }
    return {};
}

void OpenExtensionInstallGuide() {
    const std::wstring dir = ExtensionEdgeDirectory();
    const std::wstring guide = dir + L"\\guide.html";

    // 本地引导页（file://），不要 ShellExecute edge:// —— 系统会弹「获取打开此 edge 链接的应用」。
    std::wstring fileUrl = L"file:///";
    for (wchar_t ch : guide) {
        if (ch == L'\\') fileUrl.push_back(L'/');
        else fileUrl.push_back(ch);
    }
    fileUrl += L"?path=";
    for (wchar_t ch : dir) {
        if (ch == L' ') fileUrl += L"%20";
        else if (ch == L'\\') fileUrl += L"%5C";
        else fileUrl.push_back(ch);
    }
    ShellExecuteW(nullptr, L"open", fileUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ShellExecuteW(nullptr, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    const std::wstring edge = FindMsEdgeExe();
    if (!edge.empty()) {
        // 用 msedge.exe 打开扩展页，避免 edge:// 协议未注册。
        ShellExecuteW(nullptr, L"open", edge.c_str(), L"edge://extensions",
            nullptr, SW_SHOWNORMAL);
    }
}

bool ParseExtBridgeConfigJson(const std::string& json, int& port, std::string& token) {
    port = ExtractJsonInt(json, "port", 0);
    token = ExtractJsonString(json, "token");
    return port >= kPortLo && port <= kPortHi && token.size() >= 8;
}

}  // namespace windowmode
