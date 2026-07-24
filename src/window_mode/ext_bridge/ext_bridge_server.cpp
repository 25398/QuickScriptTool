#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include "ext_bridge_server.h"
#include "window_mode/window_mode_log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace windowmode {
namespace {

constexpr int kPortLo = 19228;
constexpr int kPortHi = 19240;
const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

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

std::string Base64Encode(const unsigned char* data, size_t len) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (data[i] << 16)
            | ((i + 1 < len ? data[i + 1] : 0) << 8)
            | (i + 2 < len ? data[i + 2] : 0);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? tbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? tbl[v & 63] : '=');
    }
    return out;
}

std::string MakeWsAccept(const std::string& key) {
    const std::string src = key + kWsGuid;
    unsigned char dig[20]{};
    if (!Sha1(src.data(), src.size(), dig)) return {};
    return Base64Encode(dig, 20);
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

bool ExtBridgeServer::TokenMatches(const std::string& got) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mu_));
    return !token_.empty() && got == token_;
}

void ExtBridgeServer::SendHttpJson(uintptr_t clientSock, int status, const std::string& body) {
    SOCKET s = static_cast<SOCKET>(clientSock);
    const char* reason = "OK";
    if (status == 400) reason = "Bad Request";
    else if (status == 401) reason = "Unauthorized";
    else if (status == 404) reason = "Not Found";
    else if (status == 409) reason = "Conflict";
    else if (status == 500) reason = "Internal Server Error";
    else if (status == 503) reason = "Service Unavailable";
    char hdr[256];
    std::snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, X-Qst-Token\r\n"
        "Connection: close\r\nContent-Length: %u\r\n\r\n",
        status, reason, static_cast<unsigned>(body.size()));
    SendAll(s, hdr, static_cast<int>(strlen(hdr)));
    if (!body.empty()) {
        SendAll(s, body.data(), static_cast<int>(body.size()));
    }
}

bool ExtBridgeServer::BindPort(std::wstring& err) {
    for (int port = kPortLo; port <= kPortHi; ++port) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        BOOL reuse = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closesocket(s);
            continue;
        }
        if (listen(s, 4) != 0) {
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

void ExtBridgeServer::WriteConfigFile() const {
    wchar_t localApp[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) == 0) return;
    std::wstring dir = std::wstring(localApp) + L"\\QuickScriptTool";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\ext_bridge.json";
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
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, body, static_cast<DWORD>(strlen(body)), &written, nullptr);
    CloseHandle(h);
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
        token_ = RandomTokenHex(16);
    }

    if (!BindPort(err)) {
        WSACleanup();
        return false;
    }

    stop_.store(false);
    ClearAbort();
    running_.store(true);
    WriteConfigFile();
    WindowModeLogf(L"[窗口模式] 扩展桥已监听 127.0.0.1:%d", port_.load());

    thread_ = std::thread([this]() { ThreadMain(); });
    err.clear();
    return true;
}

void ExtBridgeServer::ClearAbort() {
    abort_.store(false);
}

void ExtBridgeServer::AbortPending() {
    abort_.store(true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        waitingDone_ = true;
        if (waitingResult_.empty()) {
            waitingResult_ = "{\"ok\":false,\"error\":\"ABORTED\"}";
        }
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
        waitingDone_ = true;
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
    if (stop_.load() || abort_.load()) return false;
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

    if (method == "OPTIONS") {
        SendHttpJson(clientSock, 200, "{\"ok\":true}");
        return false;
    }

    const bool isStatus = (method == "GET" && path.rfind("/qst/status", 0) == 0);
    const bool isScripts = (method == "GET" && path.rfind("/qst/scripts", 0) == 0);
    const bool isRun = (method == "POST" && path.rfind("/qst/run", 0) == 0);
    const bool isStop = (method == "POST" && path.rfind("/qst/stop", 0) == 0);
    const bool isShot = (method == "POST" && path.rfind("/qst/shot", 0) == 0);
    const bool isWs = (method == "GET" && path.rfind("/qst/ws", 0) == 0);
    if (!isStatus && !isScripts && !isRun && !isStop && !isShot && !isWs) {
        SendHttpJson(clientSock, 404, "{\"ok\":false,\"error\":\"not_found\"}");
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

    if (isShot) {
        const std::string qToken = QueryParam(path, "token");
        const std::string hToken = ExtractHeader(headers, "X-Qst-Token");
        if (!TokenMatches(qToken) && !TokenMatches(hToken)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}");
            return false;
        }
        // JPEG 可达数 MB；禁止走 WebSocket 大包（会弄死 MV3 SW）。
        constexpr int kMaxShot = 12 * 1024 * 1024;
        std::string body = readBody(kMaxShot, 15000);
        if (body.size() < 64) {
            SendHttpJson(clientSock, 400, "{\"ok\":false,\"error\":\"empty_shot\"}");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            lastShotJpeg_.assign(body.begin(), body.end());
        }
        cv_.notify_all();
        SendHttpJson(clientSock, 200,
            "{\"ok\":true,\"bytes\":" + std::to_string(body.size()) + "}");
        return false;
    }

    if (isStatus) {
        const int port = port_.load();
        std::string tok;
        ExtScriptApiHandlers api;
        {
            std::lock_guard<std::mutex> lock(mu_);
            tok = token_;
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
            + ",\"token\":\"" + JsonEscape(tok) + "\""
            + ",\"ws\":\"ws://127.0.0.1:" + std::to_string(port) + "/qst/ws\""
            + ",\"running\":" + (running ? "true" : "false")
            + ",\"currentScript\":\"" + JsonEscape(cur) + "\"}";
        SendHttpJson(clientSock, 200, body);
        static std::atomic<int> statusHits{0};
        const int n = ++statusHits;
        if (n == 1 || (n % 8) == 0) {
            WindowModeLogf(L"[窗口模式] 扩展桥被探测 status×%d（扩展进程活着）", n);
        }
        return false;
    }

    if (isScripts) {
        const std::string qToken = QueryParam(path, "token");
        if (!TokenMatches(qToken)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}");
            return false;
        }
        ExtScriptApiHandlers api;
        {
            std::lock_guard<std::mutex> lock(mu_);
            api = scriptApi_;
        }
        if (!api.listScripts) {
            SendHttpJson(clientSock, 503, "{\"ok\":false,\"error\":\"no_handler\"}");
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
        SendHttpJson(clientSock, 200, body);
        return false;
    }

    if (isRun) {
        const std::string bodyIn = readBody(65536, 3000);
        const std::string tok = ExtractJsonString(bodyIn, "token");
        if (!TokenMatches(tok)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}");
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
            SendHttpJson(clientSock, 503, "{\"ok\":false,\"error\":\"no_handler\"}");
            return false;
        }
        std::string err;
        if (!api.runScript(pathOrFile, err)) {
            const int code = (err == "busy") ? 409 : 400;
            SendHttpJson(clientSock, code,
                "{\"ok\":false,\"error\":\"" + JsonEscape(err.empty() ? "run_failed" : err) + "\"}");
            return false;
        }
        SendHttpJson(clientSock, 200, "{\"ok\":true}");
        return false;
    }

    if (isStop) {
        const std::string bodyIn = readBody(65536, 3000);
        const std::string tok = ExtractJsonString(bodyIn, "token");
        if (!TokenMatches(tok)) {
            SendHttpJson(clientSock, 401, "{\"ok\":false,\"error\":\"bad_token\"}");
            return false;
        }
        ExtScriptApiHandlers api;
        {
            std::lock_guard<std::mutex> lock(mu_);
            api = scriptApi_;
        }
        if (api.stopScript) api.stopScript();
        SendHttpJson(clientSock, 200, "{\"ok\":true}");
        return false;
    }

    if (!stop_.load() && !abort_.load()) {
        WindowModeLog(L"[窗口模式] 扩展桥收到 WebSocket 升级请求");
    }
    const std::string upgrade = ExtractHeader(headers, "Upgrade");
    const std::string wsKey = ExtractHeader(headers, "Sec-WebSocket-Key");
    const std::string qToken = QueryParam(path, "token");
    std::string expectTok;
    {
        std::lock_guard<std::mutex> lock(mu_);
        expectTok = token_;
    }
    // Upgrade 头可能是 "websocket"，也可能混在 Connection 里；部分环境 token 校验稍后放宽。
    const bool upgradeOk = _stricmp(upgrade.c_str(), "websocket") == 0
        || headers.find("websocket") != std::string::npos
        || headers.find("WebSocket") != std::string::npos;
    if (!upgradeOk || wsKey.empty()) {
        WindowModeLogf(L"[窗口模式] 扩展桥 WS 握手失败: upgradeLen=%u keyLen=%u",
            static_cast<unsigned>(upgrade.size()), static_cast<unsigned>(wsKey.size()));
        const char* resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        SendAll(s, resp, static_cast<int>(strlen(resp)));
        return false;
    }
    if (qToken != expectTok) {
        WindowModeLogf(L"[窗口模式] 扩展桥 WS token 不匹配 (qLen=%u expectLen=%u)，仍继续握手并依赖 hello 校验",
            static_cast<unsigned>(qToken.size()), static_cast<unsigned>(expectTok.size()));
    }

    const std::string accept = MakeWsAccept(wsKey);
    if (accept.empty()) return false;
    char resp[512];
    std::snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept.c_str());
    if (!SendAll(s, resp, static_cast<int>(strlen(resp)))) return false;

    int clientCount = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        extSocks_.push_back(clientSock);
        extConnected_.store(true);
        clientCount = static_cast<int>(extSocks_.size());
        cv_.notify_all();
    }
    if (!stop_.load() && !abort_.load()) {
        WindowModeLogf(L"[窗口模式] 配套扩展已连接本机桥（当前 %d 路）", clientCount);
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
            if (waitingId_ != 0 && id == waitingId_) {
                waitingResult_ = msg;
                waitingDone_ = true;
                cv_.notify_all();
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        RemoveSockLocked(clientSock);
        waitingDone_ = true;
        cv_.notify_all();
        clientCount = static_cast<int>(extSocks_.size());
    }
    if (!stop_.load() && !abort_.load()) {
        WindowModeLogf(L"[窗口模式] 配套扩展已断开本机桥（剩余 %d 路）", clientCount);
    }
    return true;
}

void ExtBridgeServer::ThreadMain() {
    SOCKET listen = static_cast<SOCKET>(listenSock_);
    while (!stop_.load()) {
        TIMEVAL tv{0, 200000};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen, &fds);
        const int sel = select(0, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        SOCKET client = accept(listen, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (client == INVALID_SOCKET) continue;

        // status 请求很快结束；WS 连接占用线程 —— 用短线程处理
        std::thread([this, client]() {
            const bool keep = HandleClient(static_cast<uintptr_t>(client));
            if (!keep) {
                closesocket(client);
            } else {
                closesocket(client);
            }
        }).detach();
    }
}

bool ExtBridgeServer::WaitForExtension(int timeoutMs, std::wstring& err) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!extSocks_.empty()) {
        err.clear();
        return true;
    }
    const auto ok = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
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
    {
        std::unique_lock<std::mutex> lock(mu_);
        id = nextId_++;
        waitingId_ = id;
        waitingResult_.clear();
        waitingDone_ = false;
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
        waitingId_ = 0;
        return false;
    }

    std::unique_lock<std::mutex> lock(mu_);
    // 分片等待：热键 Abort 后最多约 50ms 退出，避免单次 wait_for(20s) 漏唤醒时卡死。
    bool ok = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        if (waitingDone_ || abort_.load() || stop_.load()) {
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
            return waitingDone_ || abort_.load() || stop_.load();
        })) {
            ok = true;
            break;
        }
    }
    waitingId_ = 0;
    if (abort_.load() || stop_.load()) {
        err = L"已取消";
        return false;
    }
    if (!ok || !waitingDone_) {
        err = L"扩展桥响应超时";
        return false;
    }
    resultJson = waitingResult_;
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
        WindowModeLogf(L"[窗口模式] 扩展桥 attach：向 %d 路扩展逐个尝试",
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
                WindowModeLogf(L"[窗口模式] 扩展桥跳过错误标签: %s（version=%s）",
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
