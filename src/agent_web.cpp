// ──────────────────────────────────────────────────────────────────
// agent_web.cpp — AI 智能体网页抓取工具
// 零外部依赖的网页抓取：WinHTTP GET → 字符集识别 → HTML 转纯文本。
// 覆盖「文章/文档/新闻/天气页」等静态内容；JS 渲染与强反爬站点不保证。
// ──────────────────────────────────────────────────────────────────
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "agent_web.h"

#include "agent_webview.h"
#include "macro_execute_tools.h"
#include "page_snapshot.h"
#include "utils.h"

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr size_t kMaxRawBody = 512 * 1024;   // 原始 HTML 读取上限
constexpr int kDefaultMaxChars = 20000;      // 返回纯文本默认上限

/// 找标签真正的 `>` —— **跳过引号里的内容**。
/// ⚠ 实测踩过（抽出来的「正文」里出现 `t" href="data:text/css;base64,…`）：属性值里可以有 `>`，
///   不跳引号就会把标签截成两半，后半截（一堆 data URI/base64）被当成正文输出。
///   ⇒ HTML 相关的每一处「找标签结尾」都必须用这一个函数，别各处自己 `find(L'>')`。
size_t FindTagEnd(const std::wstring& s, size_t lt) {
    wchar_t quote = 0;
    for (size_t i = lt + 1; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == L'"' || c == L'\'') { quote = c; continue; }
        if (c == L'>') return i;
    }
    return std::wstring::npos;
}

struct WebHttpHandle {
    HINTERNET h = nullptr;
    WebHttpHandle(HINTERNET hh = nullptr) : h(hh) {}
    ~WebHttpHandle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

struct WebUrl {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
    bool isHttps = true;
};

bool ParseWebUrl(const std::wstring& url, WebUrl& out, std::wstring& err) {
    std::wstring lower;
    lower.reserve(url.size());
    for (wchar_t c : url) lower.push_back(static_cast<wchar_t>(std::towlower(c)));
    const size_t schemeEnd = lower.find(L"://");
    if (schemeEnd == std::wstring::npos) {
        err = L"URL 缺少协议，仅支持 http/https";
        return false;
    }
    const std::wstring scheme = lower.substr(0, schemeEnd);
    if (scheme != L"http" && scheme != L"https") {
        err = L"仅支持 http/https 协议";
        return false;
    }
    out.isHttps = (scheme == L"https");
    out.port = out.isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    const size_t hostStart = schemeEnd + 3;
    const size_t pathStart = url.find(L'/', hostStart);
    std::wstring hostPort;
    if (pathStart == std::wstring::npos) {
        hostPort = url.substr(hostStart);
        out.path = L"/";
    } else {
        hostPort = url.substr(hostStart, pathStart - hostStart);
        out.path = url.substr(pathStart);
    }
    const size_t at = hostPort.rfind(L'@');
    if (at != std::wstring::npos) hostPort = hostPort.substr(at + 1);
    if (hostPort.empty()) {
        err = L"URL 缺少主机名";
        return false;
    }
    if (hostPort.front() == L'[') {
        const size_t rb = hostPort.find(L']');
        if (rb == std::wstring::npos) {
            err = L"URL IPv6 主机无效";
            return false;
        }
        out.host = hostPort.substr(1, rb - 1);
        if (rb + 1 < hostPort.size()) {
            if (hostPort[rb + 1] != L':') {
                err = L"URL 端口无效";
                return false;
            }
            try {
                out.port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(rb + 2)));
            } catch (...) {
                err = L"URL 端口无效";
                return false;
            }
        }
    } else {
        const size_t colon = hostPort.rfind(L':');
        if (colon != std::wstring::npos && hostPort.find(L':') == colon) {
            out.host = hostPort.substr(0, colon);
            try {
                out.port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(colon + 1)));
            } catch (...) {
                err = L"URL 端口无效";
                return false;
            }
        } else {
            out.host = hostPort;
        }
    }
    if (out.host.empty()) {
        err = L"URL 缺少主机名";
        return false;
    }
    return true;
}

bool ParseIpv4(const std::wstring& host, unsigned& a, unsigned& b, unsigned& c, unsigned& d) {
    unsigned vals[4]{};
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t dot = (i < 3) ? host.find(L'.', start) : std::wstring::npos;
        const std::wstring part = (i < 3)
            ? host.substr(start, dot == std::wstring::npos ? std::wstring::npos : dot - start)
            : host.substr(start);
        if (part.empty() || (i < 3 && dot == std::wstring::npos)) return false;
        if (part.size() > 3) return false;
        int v = 0;
        for (wchar_t ch : part) {
            if (ch < L'0' || ch > L'9') return false;
            v = v * 10 + (ch - L'0');
        }
        if (v > 255) return false;
        vals[i] = static_cast<unsigned>(v);
        if (i < 3) start = dot + 1;
    }
    a = vals[0]; b = vals[1]; c = vals[2]; d = vals[3];
    return true;
}

bool Ipv4IsBlocked(unsigned a, unsigned b, unsigned /*c*/, unsigned /*d*/) {
    if (a == 10 || a == 127 || a == 0) return true;
    if (a == 169 && b == 254) return true;
    if (a == 192 && b == 168) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 100 && b >= 64 && b <= 127) return true;
    return false;
}

bool In6AddrIsBlocked(const IN6_ADDR& addr) {
    if (IN6_IS_ADDR_UNSPECIFIED(&addr) || IN6_IS_ADDR_LOOPBACK(&addr)
        || IN6_IS_ADDR_LINKLOCAL(&addr) || IN6_IS_ADDR_SITELOCAL(&addr)) {
        return true;
    }
    if (IN6_IS_ADDR_V4MAPPED(&addr) || IN6_IS_ADDR_V4COMPAT(&addr)) {
        const unsigned char* b = addr.s6_addr;
        return Ipv4IsBlocked(b[12], b[13], b[14], b[15]);
    }
    if ((addr.s6_addr[0] & 0xFE) == 0xFC) return true;
    return false;
}

bool EnsureWsaStarted() {
    static int state = 0;
    if (state == 1) return true;
    if (state == -1) return false;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        state = -1;
        return false;
    }
    state = 1;
    return true;
}

bool HostIsBlocked(std::wstring host) {
    for (auto& ch : host) ch = static_cast<wchar_t>(std::towlower(ch));
    if (host.empty()) return true;
    if (host.front() == L'[' && host.back() == L']' && host.size() >= 2)
        host = host.substr(1, host.size() - 2);
    const size_t zone = host.find(L'%');
    if (zone != std::wstring::npos) host = host.substr(0, zone);
    if (host == L"localhost" || host == L"metadata.google.internal"
        || host == L"metadata"
        || (host.size() >= 6 && host.compare(host.size() - 6, 6, L".local") == 0)) {
        return true;
    }
    std::wstring mapped;
    if (host.rfind(L"::ffff:", 0) == 0) mapped = host.substr(7);
    else if (host.rfind(L"0:0:0:0:0:ffff:", 0) == 0) mapped = host.substr(15);
    if (!mapped.empty()) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (ParseIpv4(mapped, a, b, c, d) && Ipv4IsBlocked(a, b, c, d)) return true;
    }
    if (host.find(L':') != std::wstring::npos) {
        IN6_ADDR v6{};
        if (InetPtonW(AF_INET6, host.c_str(), &v6) == 1 && In6AddrIsBlocked(v6))
            return true;
        if (host == L"::1" || host == L"0:0:0:0:0:0:0:1"
            || host.rfind(L"fe80:", 0) == 0
            || host.rfind(L"fc", 0) == 0 || host.rfind(L"fd", 0) == 0) {
            return true;
        }
    }
    if (host == L"0.0.0.0") return true;
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (ParseIpv4(host, a, b, c, d) && Ipv4IsBlocked(a, b, c, d)) return true;
    return false;
}

std::wstring OriginFromParsed(const WebUrl& u) {
    std::wstring o = u.isHttps ? L"https://" : L"http://";
    const bool ipv6 = u.host.find(L':') != std::wstring::npos;
    if (ipv6) o += L"[" + u.host + L"]";
    else o += u.host;
    const INTERNET_PORT def = u.isHttps ? INTERNET_DEFAULT_HTTPS_PORT
        : INTERNET_DEFAULT_HTTP_PORT;
    if (u.port != def) o += L":" + std::to_wstring(u.port);
    return o;
}

std::wstring ResolveHttpRedirect(const WebUrl& from, std::wstring location) {
    while (!location.empty() && (location.front() == L' ' || location.front() == L'\t'))
        location.erase(location.begin());
    while (!location.empty() && (location.back() == L' ' || location.back() == L'\t'
        || location.back() == L'\r' || location.back() == L'\n')) {
        location.pop_back();
    }
    if (location.size() >= 7) {
        std::wstring low = location.substr(0, 8);
        for (auto& ch : low) ch = static_cast<wchar_t>(std::towlower(ch));
        if (low.rfind(L"http://", 0) == 0 || low.rfind(L"https://", 0) == 0) return location;
    }
    if (location.rfind(L"//", 0) == 0)
        return (from.isHttps ? L"https:" : L"http:") + location;
    const std::wstring origin = OriginFromParsed(from);
    if (!location.empty() && location.front() == L'/') return origin + location;
    std::wstring dir = from.path;
    const size_t slash = dir.find_last_of(L'/');
    if (slash == std::wstring::npos) dir = L"/";
    else dir = dir.substr(0, slash + 1);
    return origin + dir + location;
}

// 从 Content-Type / meta 中提取字符集提示；小写返回。
std::wstring LowerCharsetHint(const std::wstring& contentType,
                              const std::string& rawBodyPrefix) {
    std::wstring hint = contentType;
    for (auto& c : hint) c = static_cast<wchar_t>(std::towlower(c));
    if (hint.find(L"charset=") != std::wstring::npos) {
        const size_t p = hint.find(L"charset=") + 8;
        const size_t e = hint.find_first_of(L"; \"", p);
        return hint.substr(p, e == std::wstring::npos ? hint.size() : e - p);
    }
    // meta charset 探测（只看前 8KB）
    std::string head = rawBodyPrefix.substr(0, 8192);
    for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string kMeta = "charset=";
    const size_t p = head.find(kMeta);
    if (p != std::string::npos) {
        const size_t start = p + kMeta.size();
        const size_t end = head.find_first_of("\"' >", start);
        std::string cs = head.substr(start, end == std::string::npos ? 16 : end - start);
        if (cs.size() > 16) cs.resize(16);
        std::wstring wcs = FromUtf8(cs);
        for (auto& c : wcs) c = static_cast<wchar_t>(std::towlower(c));
        return wcs;
    }
    return L"";
}

std::wstring DecodeHtmlBody(const std::string& bytes, const std::wstring& charsetHint) {
    if (bytes.size() >= 3
        && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return FromUtf8(bytes.substr(3));
    }
    if (charsetHint.find(L"gb") != std::wstring::npos
        || charsetHint.find(L"936") != std::wstring::npos) {
        int len = MultiByteToWideChar(936, 0, bytes.data(),
            static_cast<int>(bytes.size()), nullptr, 0);
        if (len > 0) {
            std::wstring out(static_cast<size_t>(len), L'\0');
            MultiByteToWideChar(936, 0, bytes.data(),
                static_cast<int>(bytes.size()), out.data(), len);
            return out;
        }
    }
    // 默认按 UTF-8 严格解码；失败则回退 GBK
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (len > 0) {
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
            static_cast<int>(bytes.size()), out.data(), len);
        return out;
    }
    len = MultiByteToWideChar(936, 0, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (len > 0) {
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(936, 0, bytes.data(),
            static_cast<int>(bytes.size()), out.data(), len);
        return out;
    }
    return FromUtf8(bytes);
}

}  // namespace

bool AgentFetchUrlIsBlocked(const std::wstring& url, std::wstring& err) {
    WebUrl parsed;
    if (!ParseWebUrl(url, parsed, err)) return true;
    if (HostIsBlocked(parsed.host)) {
        err = L"禁止抓取本地/内网地址。";
        return true;
    }
    return false;
}

bool AgentFetchResolvedHostBlocked(const std::wstring& hostIn, std::wstring& err) {
    std::wstring host = hostIn;
    if (host.empty()) {
        err = L"URL 缺少主机名";
        return true;
    }
    if (host.front() == L'[' && host.back() == L']' && host.size() >= 2)
        host = host.substr(1, host.size() - 2);
    const size_t zone = host.find(L'%');
    if (zone != std::wstring::npos) host = host.substr(0, zone);
    if (HostIsBlocked(host)) {
        err = L"禁止抓取本地/内网地址。";
        return true;
    }
    if (!EnsureWsaStarted()) {
        err = L"网络初始化失败，已拒绝抓取。";
        return true;
    }
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = nullptr;
    const int rc = GetAddrInfoW(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) {
        err = L"无法解析主机，已拒绝抓取。";
        return true;
    }
    bool blocked = false;
    for (ADDRINFOW* p = res; p; p = p->ai_next) {
        if (!p->ai_addr) continue;
        if (p->ai_family == AF_INET && p->ai_addrlen >= sizeof(sockaddr_in)) {
            const auto* v4 = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
            const unsigned long ip = ntohl(v4->sin_addr.s_addr);
            const unsigned a = (ip >> 24) & 0xFFu;
            const unsigned b = (ip >> 16) & 0xFFu;
            const unsigned c = (ip >> 8) & 0xFFu;
            const unsigned d = ip & 0xFFu;
            if (Ipv4IsBlocked(a, b, c, d)) { blocked = true; break; }
        } else if (p->ai_family == AF_INET6 && p->ai_addrlen >= sizeof(sockaddr_in6)) {
            const auto* v6 = reinterpret_cast<const sockaddr_in6*>(p->ai_addr);
            if (In6AddrIsBlocked(v6->sin6_addr)) { blocked = true; break; }
        }
    }
    FreeAddrInfoW(res);
    if (blocked) {
        err = L"禁止抓取本地/内网地址。";
        return true;
    }
    return false;
}

bool AgentFetchUrlDestinationBlocked(const std::wstring& url, std::wstring& err) {
    if (AgentFetchUrlIsBlocked(url, err)) return true;
    WebUrl parsed;
    if (!ParseWebUrl(url, parsed, err)) return true;
    return AgentFetchResolvedHostBlocked(parsed.host, err);
}

bool FetchWebPage(const std::wstring& url, std::wstring& outBody,
                  std::wstring& err, int timeoutMs) {
    outBody.clear();
    std::wstring current = url;
    const DWORD timeout = static_cast<DWORD>(std::max(5000, timeoutMs));
    WebHttpHandle session = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        L"(KHTML, like Gecko) Chrome/120.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        err = L"WinHttpOpen 失败";
        return false;
    }
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

    for (int hop = 0; hop < 6; ++hop) {
        if (AgentFetchUrlDestinationBlocked(current, err)) return false;
        WebUrl parsed;
        if (!ParseWebUrl(current, parsed, err)) return false;

        WebHttpHandle connect = WinHttpConnect(
            session, parsed.host.c_str(), parsed.port, 0);
        if (!connect) {
            err = L"无法连接 " + parsed.host;
            return false;
        }
        WebHttpHandle request = WinHttpOpenRequest(
            connect, L"GET", parsed.path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, parsed.isHttps ? WINHTTP_FLAG_SECURE : 0);
        if (!request) {
            err = L"WinHttpOpenRequest 失败";
            return false;
        }
        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
        const std::wstring referer = parsed.isHttps ? L"https://" : L"http://";
        std::wstring headers = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            L"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
            L"Accept: text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.8\r\n"
            L"Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n"
            L"Accept-Encoding: identity\r\n";
        headers += L"Referer: " + referer + parsed.host + L"/\r\n";
        if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1),
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            err = L"发送请求失败（" + parsed.host + L"）";
            return false;
        }
        if (!WinHttpReceiveResponse(request, nullptr)) {
            err = L"接收响应失败（" + parsed.host + L"）";
            return false;
        }
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
            WINHTTP_NO_HEADER_INDEX);
        if (statusCode >= 300 && statusCode < 400) {
            wchar_t loc[2048]{};
            DWORD n = sizeof(loc);
            if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                    WINHTTP_HEADER_NAME_BY_INDEX, loc, &n, WINHTTP_NO_HEADER_INDEX)
                || loc[0] == 0) {
                err = L"重定向缺少 Location（" + parsed.host + L"）";
                return false;
            }
            current = ResolveHttpRedirect(parsed, loc);
            continue;
        }
        if (statusCode >= 400) {
            err = L"HTTP " + std::to_wstring(statusCode) + L"（" + parsed.host + L"）";
            return false;
        }
        std::wstring contentType;
        {
            wchar_t buf[512]{};
            DWORD n = 512;
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE,
                    WINHTTP_HEADER_NAME_BY_INDEX, buf, &n, WINHTTP_NO_HEADER_INDEX)) {
                contentType.assign(buf, n / sizeof(wchar_t));
            }
        }
        std::string body;
        body.reserve(65536);
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline) {
                err = L"读取超时（" + parsed.host + L"）";
                return false;
            }
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                err = L"读取数据失败（" + parsed.host + L"）";
                return false;
            }
            if (available == 0) break;
            std::vector<char> buf(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, buf.data(), available, &read) || read == 0) break;
            body.append(buf.data(), read);
            if (body.size() >= kMaxRawBody) {
                body.resize(kMaxRawBody);
                break;
            }
        }
        if (body.empty()) {
            err = L"页面为空（" + parsed.host + L"）";
            return false;
        }
        outBody = DecodeHtmlBody(body, LowerCharsetHint(contentType, body));
        return true;
    }
    err = L"重定向次数过多";
    return false;
}

std::wstring HtmlToPlainText(const std::wstring& html) {
    std::wstring s = html;
    // 去 script / style / 注释
    auto stripBlock = [&](const wchar_t* open, const wchar_t* close) {
        const std::wstring o(open), c(close);
        for (;;) {
            const size_t a = s.find(o);
            if (a == std::wstring::npos) break;
            const size_t tagEnd = s.find(L'>', a);
            if (tagEnd == std::wstring::npos) { s.erase(a); break; }
            const size_t b = s.find(c, tagEnd + 1);
            if (b != std::wstring::npos) {
                s.erase(a, b + c.size() - a);
                continue;
            }
            // 容错：闭合标签可能带空格（如 </style >），按标签名宽松匹配
            const std::wstring closeName = c.size() > 1 ? c.substr(0, c.size() - 1) : c;
            const size_t closeStart = s.find(closeName, tagEnd + 1);
            if (closeStart == std::wstring::npos) {
                // 没有闭合标签：只移除开标签本身，保留后续内容
                s.erase(a, tagEnd - a + 1);
                continue;
            }
            const size_t closeEnd = s.find(L'>', closeStart);
            if (closeEnd == std::wstring::npos) { s.erase(closeStart); break; }
            s.erase(a, closeEnd - a + 1);
        }
    };
    stripBlock(L"<script", L"</script>");
    stripBlock(L"<style", L"</style>");
    stripBlock(L"<!--", L"-->");

    std::wstring title;
    {
        const size_t a = s.find(L"<title");
        if (a != std::wstring::npos) {
            const size_t b = s.find(L'>', a);
            const size_t c = s.find(L"</title>", b == std::wstring::npos ? a : b);
            if (b != std::wstring::npos && c != std::wstring::npos && c > b)
                title = Trim(s.substr(b + 1, c - b - 1));
        }
    }

    // 去标签 + 块级标签后换行（**整只标签都替掉**：只替 `<br` 这种前缀会把 `/>` 留在正文里 —— 实测）
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == L'<') {
            const size_t te = FindTagEnd(s, i);
            if (te == std::wstring::npos) break;   // 残缺标签：后面不再是可信正文
            std::wstring nm;
            for (size_t k = i + 1; k < te; ++k) {
                const wchar_t c = s[k];
                if (c == L'/' || iswspace(c)) continue;
                if (!iswalnum(c) && c != L'-') break;
                nm.push_back(static_cast<wchar_t>(towlower(c)));
            }
            static const wchar_t* kBlockTags[] = { L"br", L"p", L"div", L"h1", L"h2", L"h3",
                L"h4", L"h5", L"h6", L"li", L"tr", L"table", L"ul", L"ol", L"section",
                L"article", L"blockquote", L"dd", L"dt", L"td" };
            bool block = false;
            for (const wchar_t* bt : kBlockTags) if (nm == bt) { block = true; break; }
            out.push_back(block ? L'\n' : L' ');
            i = te + 1;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }

    // 实体解码（`&amp;` **放最后**：先解它会把 `&amp;lt;` 变成真的 `&lt;` 再解一次）
    auto replaceAll = [&](const std::wstring& from, const std::wstring& to) {
        for (;;) {
            const size_t p = out.find(from);
            if (p == std::wstring::npos) break;
            out.replace(p, from.size(), to);
        }
    };
    // 常见具名实体（实测搜索结果摘要里就有 `&ensp;·&ensp;` 这种，不解码直接喂给模型很难看）
    replaceAll(L"&ensp;", L" ");
    replaceAll(L"&emsp;", L" ");
    replaceAll(L"&thinsp;", L" ");
    replaceAll(L"&middot;", L"·");
    replaceAll(L"&hellip;", L"…");
    replaceAll(L"&mdash;", L"—");
    replaceAll(L"&ndash;", L"–");
    replaceAll(L"&ldquo;", L"“");
    replaceAll(L"&rdquo;", L"”");
    replaceAll(L"&lsquo;", L"‘");
    replaceAll(L"&rsquo;", L"’");
    replaceAll(L"&laquo;", L"《");
    replaceAll(L"&raquo;", L"》");
    replaceAll(L"&times;", L"×");
    replaceAll(L"&lt;", L"<");
    replaceAll(L"&gt;", L">");
    replaceAll(L"&quot;", L"\"");
    replaceAll(L"&apos;", L"'");
    replaceAll(L"&#39;", L"'");
    replaceAll(L"&nbsp;", L" ");
    replaceAll(L"&amp;", L"&");
    // 数字实体 &#NN; / &#xHH;
    for (;;) {
        const size_t p = out.find(L"&#");
        if (p == std::wstring::npos) break;
        const size_t e = out.find(L';', p);
        if (e == std::wstring::npos || e - p > 10) break;
        const std::wstring num = out.substr(p + 2, e - p - 2);
        wchar_t decoded = 0;
        try {
            if (!num.empty() && (num[0] == L'x' || num[0] == L'X'))
                decoded = static_cast<wchar_t>(std::stoi(num.substr(1), nullptr, 16));
            else
                decoded = static_cast<wchar_t>(std::stoi(num));
        } catch (...) {}
        out.replace(p, e - p + 1, decoded ? std::wstring(1, decoded) : L"");
    }

    // 空白折叠
    std::wstring clean;
    clean.reserve(out.size());
    bool lastSpace = false;
    for (wchar_t ch : out) {
        if (ch == L'\n' || ch == L'\r' || ch == L'\t' || ch == L' ') {
            if (!lastSpace) clean.push_back(ch);
            lastSpace = true;
        } else {
            clean.push_back(ch);
            lastSpace = false;
        }
    }
    // 按行修整（去行首尾空格、合并空行）
    std::wstring result;
    if (!title.empty()) result = L"标题：" + title + L"\n\n";
    size_t lineStart = 0;
    while (lineStart <= clean.size()) {
        size_t nl = clean.find(L'\n', lineStart);
        std::wstring line = Trim(clean.substr(lineStart,
            nl == std::wstring::npos ? clean.size() - lineStart : nl - lineStart));
        if (!line.empty()) {
            result += line;
            result += L"\n";
        }
        if (nl == std::wstring::npos) break;
        lineStart = nl + 1;
    }
    return Trim(result);
}

namespace {

/// 「把一个 URL 读成**给模型看的文字**」——`fetchWebPage` 与 `webSearch(带读正文)` 共用**同一份实现**。
/// ⚠ 一份实现是硬要求：两处各写一遍必然漂移（本仓 §33.1/§38.5 的教训），
///   而漂移的表现是「同一句话在两个工具里口径不同」，模型只能自己猜哪个对。
struct WebReadOutcome {
    bool ok = false;
    std::wstring text;      // 已抽取的正文（或整页纯文本/JSON）
    std::wstring method;    // 如实回执用的路径名
    std::wstring err;
};

WebReadOutcome ReadUrlAsText(const std::wstring& url, bool forceRender, int timeoutMs) {
    WebReadOutcome out;
    std::wstring lower;
    lower.reserve(url.size());
    for (const wchar_t c : url) lower.push_back(static_cast<wchar_t>(std::towlower(c)));

    if (forceRender) {
        out.ok = FetchWebPageRendered(url, out.text, out.err, timeoutMs);
        out.method = L"渲染";
        return out;
    }
    std::wstring body;
    std::wstring err;
    const bool got = FetchWebPage(url, body, err, timeoutMs);
    const std::wstring trimmed = Trim(body);
    const bool looksJson = (trimmed.size() >= 1 && (trimmed[0] == L'{' || trimmed[0] == L'}'
            || trimmed[0] == L'['))
        || lower.find(L"/api/") != std::wstring::npos
        || lower.find(L".json") != std::wstring::npos;
    if (got) {
        if (looksJson) {
            out.text = body;
            out.method = L"JSON 接口";
        } else {
            // ★先做 readability 式主内容抽取；抽不出来**如实回退**整页纯文本
            std::wstring title;
            const std::wstring main = HtmlExtractMainText(body, &title);
            if (!main.empty()) {
                out.text = (title.empty() ? std::wstring() : (L"标题：" + title + L"\n\n")) + main;
                out.method = L"正文抽取（readability 式）";
            } else {
                out.text = HtmlToPlainText(body);
                out.method = L"整页纯文本（未抽到正文，已如实回退）";
            }
        }
        out.ok = true;
    }
    // GET 失败，或页面疑似 JS 空壳 → WebView2 渲染兜底
    if (!out.ok || (!looksJson && out.text.size() < 80)) {
        std::wstring rendered;
        std::wstring renderErr;
        if (FetchWebPageRendered(url, rendered, renderErr, timeoutMs)) {
            std::wstring title;
            const std::wstring main = HtmlExtractMainText(rendered, &title);
            out.text = main.empty() ? HtmlToPlainText(rendered) : main;
            out.method = L"渲染";
            out.ok = true;
            out.err.clear();
        } else if (!out.ok) {
            out.err = renderErr;
        }
    }
    return out;
}

}  // namespace

AgentTool MakeFetchWebPageTool() {
    AgentTool tool;
    tool.name = L"fetchWebPage";
    tool.description =
        L"抓取一个网页并**抽成正文**（已做 readability 式主内容抽取：剔导航/侧栏/页脚/相关阅读）。"
        L"url 必填 http/https。"
        // ★★这段措辞是**实测事故**的修复：旧文案写「禁止抓站点搜索/用户/动态 JSON API」，
        //   模型把它读成了「禁止抓搜索」，于是**放弃 fetchWebPage**、去 openWebpage 开真浏览器
        //   搜必应（抢走游戏前台 + 3 轮 + 拿到的是别的游戏的摘要）。实测本工具**能抓搜索页**
        //   （只挡站点内部 API），而且找资料本来就该先 webSearch。措辞不能再有歧义。
        L"**找资料请优先用 `webSearch`**（一步拿到 标题/URL/摘要，不用自己拼搜索地址）；"
        L"已知具体网址时用本工具读它的正文。"
        L"只禁**站点内部 API**（api.*、graphql.*、gateway.*、/api/、/ajax/、/x/web-interface、…json）"
        L"—— 那类地址会触发风控；**搜索引擎的结果页不属于此列，可以抓**。"
        L"需要**点击/登录/翻页**的交互站点才用 openWebpage + observePage"
        L"（⚠ 它会抢走前台，一次性读资料不要用它）。"
        L"返回正文（默认最多 20000 字符，maxChars 上限 50000）。"
        L"默认先走轻量 GET；GET 失败或页面疑似 JS 空壳时，自动用 App 内置 WebView2 "
        L"隐藏渲染后再取 DOM。也可传 render=true 强制走渲染。"
        L"禁止抓取本地地址/内部服务（localhost、127.0.0.1、内网 IP）。"
        L"抓取结果只用于阅读。若随后要启动程序，宿主会弹出确认框（脚本里直接启动程序不受影响）。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "url": {
                "type": "string",
                "description": "要抓取的完整 URL，如 https://example.com/article"
            },
            "maxChars": {
                "type": "integer",
                "description": "返回文本上限（默认 20000，最大 50000）"
            },
            "render": {
                "type": "boolean",
                "description": "true=强制用 WebView2 渲染后取 DOM（JS 页面）；默认自动回退"
            }
        },
        "required": ["url"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring url;
        int maxChars = kDefaultMaxChars;
        bool forceRender = false;
        try {
            const json p = json::parse(ToUtf8(paramsJson));
            if (p.contains("url") && p["url"].is_string())
                url = Trim(FromUtf8(p["url"].get<std::string>()));
            if (p.contains("maxChars") && p["maxChars"].is_number_integer())
                maxChars = p["maxChars"].get<int>();
            if (p.contains("render") && p["render"].is_boolean())
                forceRender = p["render"].get<bool>();
        } catch (...) {
            return L"[错误] 参数 JSON 解析失败。";
        }
        if (url.empty()) return L"[错误] 缺少 url 参数。";
        maxChars = std::clamp(maxChars, 1000, 50000);

        std::wstring blockErr;
        if (AgentFetchUrlDestinationBlocked(url, blockErr))
            return L"[错误] " + blockErr;
        if (LooksLikeNonUserFacingWebUrl(url)) {
            return L"[错误] 这是**站点内部 API**（会触发风控，如 B 站 412），不是给人看的页面。"
                L"要找资料用 webSearch；要**点击/登录**某个站点用 openWebpage + observePage"
                L"（⚠ 它会抢走前台）。";
        }
        const WebReadOutcome read = ReadUrlAsText(url, forceRender, 30000);
        if (!read.ok) {
            return L"[错误] 抓取失败：" + read.err
                + L"。请确认 URL 可访问，或让用户提供内容。";
        }
        AiNoteWebFetchUntrusted();
        std::wstring text = read.text;
        if (text.size() > static_cast<size_t>(maxChars)) {
            text.resize(static_cast<size_t>(maxChars));
            text += L"\n…(内容过长已截断)";
        }
        std::wstring result = L"✓ 已抓取（" + std::to_wstring(text.size())
            + L" 字符，" + read.method + L"）\n来源：" + url + L"\n\n" + text;
        return result;
    };
    return tool;
}

// ══════════════════════════════════════════════════════════════════════════
// ★正文抽取（readability 式）—— 让「抓下来的东西」是**正文**而不是整页导航
//
// 开源依据（详见 docs §52）：Mozilla Readability（Apache-2.0）、trafilatura（Apache-2.0）、
// go-readability、postlight/parser、boilerpipe（Apache-2.0）都是同一条思路 ——
// 先剔样板容器，再给候选块按「文本量 + 标点/段落数 − 链接密度」打分，取最高分容器。
// 这里是**紧凑本地重写**（不引入依赖、不复制代码），关键词表只影响「抽哪块」，
// 不影响任何动作决策。
// ══════════════════════════════════════════════════════════════════════════
namespace {

struct HtmlElemSpan {
    std::wstring tag;
    std::wstring attrs;   // class/id 原文（小写），用于加减分
    size_t start = 0;     // '<' 的位置
    size_t end = 0;       // 闭合标签之后
    int depth = 0;
};

bool HtmlTagIsVoid(const std::wstring& t) {
    static const wchar_t* kVoid[] = { L"br", L"img", L"input", L"meta", L"link", L"hr",
        L"source", L"area", L"base", L"col", L"embed", L"param", L"track", L"wbr" };
    for (const wchar_t* v : kVoid) if (t == v) return true;
    return false;
}

/// 扫描出块级元素的区间（含嵌套深度）。
/// ⚠ 必须处理 HTML 的**隐式结束标签**：`<p>` 常常不写 `</p>`（合法 HTML），
///   早期版本不处理 ⇒ 一个 `<p>` 的区间会一路延伸到文档末尾，把后面所有
///   `<style>`/base64 内容都算进「这一段正文」（实测：抽出来的正文开头是
///   `WIKI_BWIKI_哔哩哔哩"/ .smw.style%7Cext…` 和一串 base64 CSS）。
std::vector<HtmlElemSpan> ScanBlockElements(const std::wstring& html) {
    struct Open { std::wstring tag; std::wstring attrs; size_t start; int depth; };
    std::vector<Open> stack;
    std::vector<HtmlElemSpan> out;
    /// 关闭栈顶最近的 tag（含其上方所有未闭合者），并记录区间。
    auto closeUpTo = [&](const std::wstring& tag, size_t endPos) {
        for (size_t s = stack.size(); s > 0; --s) {
            if (stack[s - 1].tag != tag) continue;
            HtmlElemSpan e;
            e.tag = tag;
            e.attrs = stack[s - 1].attrs;
            e.start = stack[s - 1].start;
            e.end = endPos;
            e.depth = stack[s - 1].depth;
            out.push_back(e);
            stack.resize(s - 1);
            return true;
        }
        return false;
    };
    size_t i = 0;
    const size_t n = html.size();
    while (i < n) {
        const size_t lt = html.find(L'<', i);
        if (lt == std::wstring::npos) break;
        if (html.compare(lt, 4, L"<!--") == 0) {
            const size_t e = html.find(L"-->", lt + 4);
            i = (e == std::wstring::npos) ? n : e + 3;
            continue;
        }
        const size_t gt = FindTagEnd(html, lt);
        if (gt == std::wstring::npos) break;
        std::wstring inner = html.substr(lt + 1, gt - lt - 1);
        i = gt + 1;
        if (inner.empty()) continue;
        const bool closing = (inner[0] == L'/');
        if (closing) inner = inner.substr(1);
        size_t p = 0;
        while (p < inner.size() && (iswspace(inner[p]) || inner[p] == L'/')) ++p;
        size_t q = p;
        while (q < inner.size() && (iswalnum(inner[q]) || inner[q] == L'-')) ++q;
        if (q == p) continue;
        std::wstring tag = inner.substr(p, q - p);
        for (auto& c : tag) c = static_cast<wchar_t>(towlower(c));
        // 只要块级候选（其余标签不影响打分）
        const bool interesting = (tag == L"p" || tag == L"div" || tag == L"article"
            || tag == L"section" || tag == L"td" || tag == L"li" || tag == L"main"
            || tag == L"blockquote");
        if (closing) {
            closeUpTo(tag, gt + 1);
            continue;
        }
        if (HtmlTagIsVoid(tag)) continue;
        // 隐式结束标签（HTML5 的 implied end tag 规则里最常用的几条）
        if (tag == L"p") {
            closeUpTo(L"p", lt);
        } else if (tag == L"li") {
            closeUpTo(L"li", lt);
        } else if (tag == L"td" || tag == L"th") {
            closeUpTo(L"td", lt);
            closeUpTo(L"th", lt);
        } else if (tag == L"tr") {
            closeUpTo(L"td", lt);
            closeUpTo(L"th", lt);
            closeUpTo(L"tr", lt);
        } else if (tag == L"dd" || tag == L"dt") {
            closeUpTo(L"dd", lt);
            closeUpTo(L"dt", lt);
        } else if (tag == L"div" || tag == L"section" || tag == L"article"
            || tag == L"main" || tag == L"blockquote" || tag == L"table"
            || tag == L"ul" || tag == L"ol" || tag == L"h1" || tag == L"h2"
            || tag == L"h3" || tag == L"h4") {
            closeUpTo(L"p", lt);        // <div> 会隐式结束 <p>
            closeUpTo(L"li", lt);       // 列表项在遇到块级兄弟时也结束
        }
        if (!interesting) {
            // 仍需跟踪嵌套（闭合时按名字弹栈），但只给少数常见容器记栈，避免栈无界增长
            if (tag == L"table" || tag == L"ul" || tag == L"ol" || tag == L"a"
                || tag == L"span" || tag == L"h1" || tag == L"h2" || tag == L"h3"
                || tag == L"h4" || tag == L"dd" || tag == L"dt" || tag == L"th"
                || tag == L"tr" || tag == L"nav" || tag == L"header" || tag == L"footer"
                || tag == L"aside" || tag == L"form" || tag == L"script"
                || tag == L"style") {
                stack.push_back(Open{ tag, std::wstring(), lt, static_cast<int>(stack.size()) });
            }
            continue;
        }
        // 记 class/id（小写，限长）
        std::wstring attrs;
        {
            std::wstring low = inner;
            for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
            for (const wchar_t* key : { L"class=", L"id=" }) {
                const size_t k = low.find(key);
                if (k == std::wstring::npos) continue;
                size_t v = k + wcslen(key);
                wchar_t quote = (v < low.size() && (low[v] == L'"' || low[v] == L'\''))
                    ? low[v] : 0;
                if (quote) ++v;
                const size_t ve = quote ? low.find(quote, v) : low.find_first_of(L" \t\r\n>", v);
                if (ve != std::wstring::npos && ve > v) {
                    attrs += low.substr(v, (std::min)(ve - v, static_cast<size_t>(120)));
                    attrs += L" ";
                }
            }
        }
        stack.push_back(Open{ tag, attrs, lt, static_cast<int>(stack.size()) });
        if (stack.size() > 400) stack.erase(stack.begin());   // 病态页面防爆
    }
    // 没收尾的（HTML 截断）按文档末补上
    for (const auto& o : stack) {
        if (o.tag == L"p" || o.tag == L"div" || o.tag == L"article" || o.tag == L"section"
            || o.tag == L"td" || o.tag == L"li" || o.tag == L"main") {
            HtmlElemSpan e;
            e.tag = o.tag; e.attrs = o.attrs; e.start = o.start; e.end = html.size();
            e.depth = o.depth;
            out.push_back(e);
        }
    }
    return out;
}

struct HtmlBlockStat {
    size_t textChars = 0;
    size_t linkChars = 0;
    size_t punct = 0;
    size_t paragraphs = 0;
};

HtmlBlockStat MeasureHtmlRange(const std::wstring& html, size_t from, size_t to) {
    HtmlBlockStat st;
    bool inTag = false;
    int linkDepth = 0;
    size_t i = from;
    const size_t end = (std::min)(to, html.size());
    while (i < end) {
        const wchar_t c = html[i];
        if (c == L'<') {
            const size_t gt = FindTagEnd(html, i);
            if (gt == std::wstring::npos || gt >= end) break;
            // 只关心 <a> / </a> / <p
            std::wstring t = html.substr(i + 1, (std::min)(gt - i - 1, static_cast<size_t>(12)));
            for (auto& ch : t) ch = static_cast<wchar_t>(towlower(ch));
            if (t.rfind(L"a", 0) == 0 && (t.size() == 1 || t[1] == L' ' || t[1] == L'>'))
                ++linkDepth;
            else if (t.rfind(L"/a", 0) == 0 && linkDepth > 0)
                --linkDepth;
            else if (t.rfind(L"p", 0) == 0 && (t.size() == 1 || t[1] == L' ' || t[1] == L'>'))
                ++st.paragraphs;
            i = gt + 1;
            inTag = false;
            continue;
        }
        if (!inTag) {
            if (!iswspace(c)) {
                ++st.textChars;
                if (linkDepth > 0) ++st.linkChars;
                if (c == L'，' || c == L'。' || c == L'！' || c == L'？' || c == L'；'
                    || c == L',' || c == L'.' || c == L'!' || c == L'?' || c == L';')
                    ++st.punct;
            }
        }
        ++i;
    }
    return st;
}

/// 样板容器：整段删掉（导航/页脚/侧栏/表单…）。这些**不该**进入正文候选。
bool HtmlTagIsBoilerplate(const std::wstring& tag) {
    return tag == L"nav" || tag == L"header" || tag == L"footer" || tag == L"aside"
        || tag == L"form" || tag == L"script" || tag == L"style" || tag == L"noscript"
        || tag == L"svg" || tag == L"iframe";
}

/// 正文「文本块」标签：这些才是要输出给模型的最小单位（标题/段落/列表项/表格单元）。
bool HtmlTagIsTextBlock(const std::wstring& t) {
    return t == L"p" || t == L"h1" || t == L"h2" || t == L"h3" || t == L"h4"
        || t == L"li" || t == L"td" || t == L"dd" || t == L"blockquote";
}

/// 文本块的轻量 Markdown 前缀（保留层级，几乎不花 token —— 这是 html→md 那一派的做法）。
std::wstring HtmlBlockPrefix(const std::wstring& t) {
    if (t == L"h1") return L"# ";
    if (t == L"h2") return L"## ";
    if (t == L"h3" || t == L"h4") return L"### ";
    if (t == L"li") return L"- ";
    return L"";
}

double HtmlRangeLinkDensity(const std::wstring& s, size_t from, size_t to) {
    const HtmlBlockStat st = MeasureHtmlRange(s, from, to);
    if (st.textChars == 0) return 1.0;
    return static_cast<double>(st.linkChars) / static_cast<double>(st.textChars);
}


/// ⚠ 这一条**只用于「整块删」的判据**，而且要再 AND 上「块里没有 `<p>`」——
///   理由：链接列表/导航栏**从来不会有 `<p>`**，而正文段落容器一定有。
///   这是 Readability 的 unlikely-candidate 规则里最稳的那一半（另一半按长度阈值，
///   在中文页面上阈值不好定，所以只留结构判据）。
bool HtmlAttrLooksBoilerplate(const std::wstring& attrs) {
    if (attrs.empty()) return false;
    static const wchar_t* kNeg[] = { L"nav", L"menu", L"sidebar", L"side-bar", L"footer",
        L"header", L"comment", L"share", L"related", L"recommend", L"advert", L"banner",
        L"promo", L"cookie", L"login", L"subscribe", L"breadcrumb", L"toolbar",
        L"pagination", L"copyright", L"search", L"tag-list", L"hot-list", L"catalog",
        L"toc", L"mw-navigation", L"vector", L"portal", L"footer-info" };
    for (const wchar_t* k : kNeg)
        if (attrs.find(k) != std::wstring::npos) return true;
    return false;
}

/// class/id 关键词打分（Readability 同款思路：正向是正文容器，负向是周边样板）。
int HtmlAttrKeywordWeight(const std::wstring& attrs) {
    if (attrs.empty()) return 0;
    static const wchar_t* kPositive[] = { L"article", L"content", L"main", L"post",
        L"entry", L"body", L"text", L"detail", L"markdown", L"mw-parser", L"rich_media" };
    static const wchar_t* kNegative[] = { L"nav", L"menu", L"sidebar", L"side-bar",
        L"footer", L"header", L"comment", L"share", L"related", L"recommend", L"advert",
        L"banner", L"promo", L"cookie", L"login", L"subscribe", L"breadcrumb", L"toolbar",
        L"pagination", L"copyright", L"search", L"meta", L"tag-list", L"hot-list" };
    int w = 0;
    for (const wchar_t* k : kPositive)
        if (attrs.find(k) != std::wstring::npos) { w += 25; break; }
    for (const wchar_t* k : kNegative)
        if (attrs.find(k) != std::wstring::npos) { w -= 25; break; }
    return w;
}

}  // namespace

std::wstring HtmlExtractMainText(const std::wstring& html, std::wstring* outTitle) {
    if (outTitle) outTitle->clear();
    if (html.empty()) return {};
    // ① 先剔掉不该进正文的整块（含嵌套内容）—— 直接在副本上删除标签区间
    std::wstring s = html;
    {
        std::wstring low = s;
        for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        size_t i = 0;
        while (i < low.size()) {
            const size_t lt = low.find(L'<', i);
            if (lt == std::wstring::npos) break;
            size_t p = lt + 1;
            if (p < low.size() && low[p] == L'/') ++p;
            size_t q = p;
            while (q < low.size() && (iswalnum(low[q]) || low[q] == L'-')) ++q;
            const std::wstring tag = low.substr(p, q - p);
            const size_t gt = FindTagEnd(low, lt);
            if (gt == std::wstring::npos) break;
            const bool closing = (lt + 1 < low.size() && low[lt + 1] == L'/');
            if (!closing && HtmlTagIsBoilerplate(tag)) {
                const std::wstring closeTag = L"</" + tag;
                const size_t ce = low.find(closeTag, gt);
                // 找不到闭合标签时**删到文档末**（`<style>` 没闭合时后面全是 CSS，
                // 留着它就会把 CSS/base64 当正文 —— 实测踩过）。
                const size_t cut = (ce == std::wstring::npos)
                    ? low.size()
                    : ((FindTagEnd(low, ce) == std::wstring::npos)
                        ? low.size() : (FindTagEnd(low, ce) + 1));
                s.erase(lt, cut - lt);
                low.erase(lt, cut - lt);
                i = lt;
                continue;
            }
            i = gt + 1;
        }
    }
    // ② 再删一遍「样板容器」：class/id 像导航/评论/相关阅读/搜索 **且块里没有 `<p>`** 的整块。
    //    为什么必要（实测）：中文 Wiki 首页这类页面，正文很少而导航链接列表极长，
    //    纯按文本量/链接密度打分时导航会赢；而「没有 <p> 的链接列表」这条结构判据能把它们清掉。
    {
        const std::vector<HtmlElemSpan> elems = ScanBlockElements(s);
        std::vector<std::pair<size_t, size_t>> spans;
        for (const auto& e : elems) {
            if (e.end <= e.start) continue;
            const size_t span = e.end - e.start;
            if (span < 40 || span > 200 * 1024) continue;
            if (!HtmlAttrLooksBoilerplate(e.attrs)) continue;
            if (s.compare(e.start, (std::min)(span, static_cast<size_t>(2)), L"<p") == 0) continue;
            if (s.find(L"<p", e.start) != std::wstring::npos
                && s.find(L"<p", e.start) < e.end) continue;   // 有段落 ⇒ 可能是正文，留给打分
            if (MeasureHtmlRange(s, e.start, e.end).textChars > 1200) continue;   // 太长不敢乱删
            spans.push_back({ e.start, e.end });
        }
        // 去掉被别的区间包住的那些（父块删掉就够了，避免用过期偏移二次删除）
        std::vector<std::pair<size_t, size_t>> keep;
        for (size_t i = 0; i < spans.size(); ++i) {
            bool contained = false;
            for (size_t j = 0; j < spans.size() && !contained; ++j) {
                if (i == j) continue;
                const bool covers = spans[j].first <= spans[i].first
                    && spans[j].second >= spans[i].second
                    && (spans[j].first < spans[i].first || spans[j].second > spans[i].second);
                if (covers) contained = true;
            }
            if (!contained) keep.push_back(spans[i]);
        }
        std::sort(keep.begin(), keep.end(), [](const std::pair<size_t, size_t>& a,
                                                const std::pair<size_t, size_t>& b) {
            return a.first > b.first;   // 从后往前删，偏移才不失效
        });
        for (const auto& k : keep) s.erase(k.first, k.second - k.first);
    }
    // ③ 候选块打分
    const std::vector<HtmlElemSpan> elems = ScanBlockElements(s);
    double bestScore = 0.0;
    HtmlElemSpan best;
    bool hasBest = false;
    for (const auto& e : elems) {
        if (e.end <= e.start) continue;
        const size_t span = e.end - e.start;
        if (span < 40 || span > 400 * 1024) continue;
        const HtmlBlockStat st = MeasureHtmlRange(s, e.start, e.end);
        if (st.textChars < 100) continue;   // 太小，不可能是正文
        const double linkDensity = static_cast<double>(st.linkChars)
            / static_cast<double>((std::max)(static_cast<size_t>(1), st.textChars));
        double score = static_cast<double>((std::min)(st.textChars, static_cast<size_t>(3000))) / 100.0;
        score += 3.0 * static_cast<double>((std::min)(st.punct, static_cast<size_t>(50)));
        score += 3.0 * static_cast<double>((std::min)(st.paragraphs, static_cast<size_t>(30)));
        score += HtmlAttrKeywordWeight(e.attrs);
        score += (std::min)(e.depth, 8) * 1.0;
        if (linkDensity > 0.25) score *= (1.0 - (std::min)(linkDensity, 0.95));
        // 同分时更深的那个赢（更贴近正文），所以这里用严格大于
        if (score > bestScore) { bestScore = score; best = e; hasBest = true; }
    }
    // ④ 在选中的容器里**只输出优质文本块**（段落 / 标题 / 列表项 / 表格单元），
    //    跳过落在负向子容器（导航/侧栏/相关阅读/公告）里的块。
    //    为什么不能直接 dump 整个容器的文本（实测）：选中的容器常常是「正文 + 站点横幅」的
    //    大 wrapper，直接输出会让模型先读一屏公告/导航（本机实测 wiki 页面开头就是
    //    「本站为民间玩家交流站…」一串站点通知）。块级输出才是 trafilatura / go-readability
    //    那一派真正干净的原因。
    std::wstring main;
    if (hasBest && bestScore >= 3.0) {
        const size_t from = best.start;
        const size_t to = best.end;
        const std::vector<HtmlElemSpan> subs = ScanBlockElements(s.substr(from, to - from));
        std::vector<std::pair<size_t, size_t>> skip;      // 要跳过的负向子容器（绝对偏移）
        for (const auto& e : subs) {
            if (e.end <= e.start) continue;
            if (!HtmlAttrLooksBoilerplate(e.attrs)) continue;
            const size_t span = e.end - e.start;
            if (span > 120 * 1024) continue;              // 太大不敢整块跳（可能是外层壳）
            skip.push_back({ from + e.start, from + e.end });
        }
        auto inSkip = [&](size_t a, size_t b) {
            for (const auto& k : skip)
                if (a >= k.first && b <= k.second) return true;
            return false;
        };
        // 只留「叶子」文本块：包含别的文本块的块（如 <li><p>…</p></li>）交给内层输出，
        // 否则同一段话会被打印两遍。
        std::vector<size_t> blocks;
        for (size_t i = 0; i < subs.size(); ++i) {
            if (!HtmlTagIsTextBlock(subs[i].tag)) continue;
            if (subs[i].end <= subs[i].start) continue;
            // ⚠ subs 的偏移是**相对子串**的，必须先换算成 s 的绝对偏移再切片 ——
            //   直接拿 subs[i].start 去切 s 会切到文档中间（实测抽出来的「正文」是
            //   `ile-height="505" />` 这种属性残渣）。
            const size_t a = from + subs[i].start;
            const size_t b = from + subs[i].end;
            // 块里混着 <style>/<script>/<link> ⇒ 这不是干净的文本块（多半是坏标签边界），跳过。
            {
                const std::wstring frag = s.substr(a, b - a);
                bool dirty = false;
                for (const wchar_t* bad : { L"<style", L"<script", L"<link", L"<meta" })
                    if (frag.find(bad) != std::wstring::npos) { dirty = true; break; }
                if (dirty) continue;
            }
            bool containsInner = false;
            for (size_t j = 0; j < subs.size() && !containsInner; ++j) {
                if (i == j) continue;
                if (!HtmlTagIsTextBlock(subs[j].tag)) continue;
                if (subs[j].start >= subs[i].start && subs[j].end <= subs[i].end
                    && (subs[j].start > subs[i].start || subs[j].end < subs[i].end))
                    containsInner = true;
            }
            if (containsInner) continue;
            if (inSkip(a, b)) continue;
            const std::wstring text = Trim(HtmlToPlainText(s.substr(a, b - a)));
            const bool heading = subs[i].tag[0] == L'h';
            if (text.size() < (heading ? 2u : 12u)) continue;
            if (HtmlRangeLinkDensity(s, a, b) > 0.6) continue;   // 整块都是链接 ⇒ 导航
            const std::wstring prefix = HtmlBlockPrefix(subs[i].tag);
            if (!main.empty()) main += L"\n";
            main += prefix + text;
        }
    }
    if (main.size() < 80) return {};
    if (outTitle) {
        // 标题取**页面自己的 `<title>`**（比「正文第一行」可靠得多：正文第一行常常是横幅/公告，
        // 长度还不一定落进窗口）。
        const size_t a = s.find(L"<title");
        if (a != std::wstring::npos) {
            const size_t b = s.find(L'>', a);
            const size_t c = (b == std::wstring::npos) ? std::wstring::npos
                                                       : s.find(L"</title>", b);
            if (b != std::wstring::npos && c != std::wstring::npos && c > b) {
                *outTitle = Trim(HtmlToPlainText(s.substr(b + 1, c - b - 1)));
                // HtmlToPlainText 会带上「标题：」前缀（它自己也提取了一次），这里剥掉
                const std::wstring kPrefix = L"标题：";
                if (outTitle->rfind(kPrefix, 0) == 0) *outTitle = outTitle->substr(kPrefix.size());
                *outTitle = Trim(*outTitle);
            }
        }
    }
    return Trim(main);
}

// ══════════════════════════════════════════════════════════════════════════
// ★webSearch：零 API key 的联网搜索（宿主 HTTP，不开浏览器、不抢前台）
// ══════════════════════════════════════════════════════════════════════════
namespace {

/// Bing 的跳转包装（`/ck/a?...&u=a1<base64url>`）→ 还原真实 URL。
/// 实测本机 cn 结果页给的是**直链**，这条只是兜底（别让模型看到一串 bing.com/ck/a）。
std::wstring DecodeBingRedirect(const std::wstring& href) {
    const size_t u = href.find(L"u=a1");
    if (u == std::wstring::npos) return href;
    std::wstring b64 = href.substr(u + 4);
    const size_t amp = b64.find(L'&');
    if (amp != std::wstring::npos) b64 = b64.substr(0, amp);
    for (auto& c : b64) {
        if (c == L'-') c = L'+';
        else if (c == L'_') c = L'/';
    }
    while (b64.size() % 4 != 0) b64.push_back(L'=');
    static const wchar_t* kTbl =
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string bytes;
    int acc = 0, bits = 0;
    for (const wchar_t c : b64) {
        const wchar_t* pos = wcschr(kTbl, c);
        if (!pos) continue;
        acc = (acc << 6) | static_cast<int>(pos - kTbl);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    if (bytes.empty()) return href;
    return FromUtf8(bytes);
}

std::wstring FirstHrefInBlock(const std::wstring& block, size_t from) {
    size_t i = from;
    for (;;) {
        const size_t a = block.find(L"<a", i);
        if (a == std::wstring::npos) return {};
        const size_t gt = block.find(L'>', a);
        if (gt == std::wstring::npos) return {};
        std::wstring tag = block.substr(a, gt - a);
        std::wstring low = tag;
        for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        const size_t h = low.find(L"href=");
        if (h != std::wstring::npos) {
            size_t v = a + h + 5;
            const wchar_t quote = (v < block.size() && (block[v] == L'"' || block[v] == L'\''))
                ? block[v] : 0;
            if (quote) ++v;
            const size_t ve = quote ? block.find(quote, v)
                                    : block.find_first_of(L" \t\r\n>", v);
            if (ve != std::wstring::npos && ve > v) return block.substr(v, ve - v);
        }
        i = gt + 1;
    }
}

std::wstring TagText(const std::wstring& frag) {
    return Trim(HtmlToPlainText(frag));
}

}  // namespace

std::vector<WebSearchResult> ParseSearchResultsHtml(const std::wstring& html) {
    std::vector<WebSearchResult> out;
    if (html.empty()) return out;
    // 结果块分隔：Bing 用 <li class="b_algo">；也容忍属性顺序不同（class 不是第一个）
    std::vector<size_t> starts;
    {
        size_t i = 0;
        for (;;) {
            const size_t p = html.find(L"b_algo", i);
            if (p == std::wstring::npos) break;
            const size_t lt = html.rfind(L'<', p);
            starts.push_back(lt == std::wstring::npos ? p : lt);
            i = p + 6;
        }
    }
    for (size_t k = 0; k < starts.size() && out.size() < 20; ++k) {
        const size_t from = starts[k];
        const size_t to = (k + 1 < starts.size()) ? starts[k + 1]
            : (std::min)(html.size(), from + 64 * 1024);
        const std::wstring block = html.substr(from, to - from);
        // 标题：第一个 <h2> 里的第一个 <a href>
        WebSearchResult r;
        size_t h2 = block.find(L"<h2");
        if (h2 == std::wstring::npos) h2 = block.find(L"<H2");
        if (h2 != std::wstring::npos) {
            const std::wstring href = FirstHrefInBlock(block, h2);
            const size_t gt = block.find(L'>', h2);
            const size_t he = block.find(L"</h2", gt == std::wstring::npos ? h2 : gt);
            if (gt != std::wstring::npos && he != std::wstring::npos && he > gt)
                r.title = TagText(block.substr(gt + 1, he - gt - 1));
            if (!href.empty()) {
                r.url = (href.rfind(L"http", 0) == 0) ? DecodeBingRedirect(href) : std::wstring();
            }
        }
        // 摘要：h2 之后的第一个 <p>
        {
            const size_t base = (h2 == std::wstring::npos) ? 0 : h2;
            const size_t p = block.find(L"<p", base);
            const size_t pe = (p == std::wstring::npos) ? std::wstring::npos
                                                        : block.find(L"</p", p);
            if (p != std::wstring::npos && pe != std::wstring::npos && pe > p) {
                const size_t gt = block.find(L'>', p);
                if (gt != std::wstring::npos && gt < pe)
                    r.snippet = TagText(block.substr(gt + 1, pe - gt - 1));
            }
        }
        if (r.title.empty() || r.url.empty()) continue;
        if (r.snippet.size() > 400) r.snippet.resize(400);
        out.push_back(std::move(r));
    }
    return out;
}

std::wstring BuildSearchUrl(const std::wstring& query) {
    // 只保留可搜索的字符（去控制字符/换行），其余按 UTF-8 百分号转义
    std::wstring q;
    for (const wchar_t c : query) {
        if (c == L'\r' || c == L'\n' || c == L'\t') { q.push_back(L' '); continue; }
        if (c < 0x20) continue;
        q.push_back(c);
    }
    q = Trim(q);
    std::string utf8 = ToUtf8(q);
    std::string enc;
    static const char* kHex = "0123456789ABCDEF";
    for (const unsigned char c : utf8) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            enc.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            enc += "%20";
        } else {
            enc.push_back('%');
            enc.push_back(kHex[(c >> 4) & 0xF]);
            enc.push_back(kHex[c & 0xF]);
        }
    }
    return L"https://www.bing.com/search?q=" + FromUtf8(enc);
}

AgentTool MakeWebSearchTool() {
    AgentTool tool;
    tool.name = L"webSearch";
    tool.description =
        L"联网搜索（宿主直连搜索结果页，**不开浏览器、不抢前台**），返回 标题 / URL / 摘要 清单。"
        L"查资料的第一步就该用它（游戏玩法、软件用法、报错原因、名词是什么…），"
        L"拿到 URL 后再 fetchWebPage 读正文；也可以直接传 readTop=1~3 让它**同一轮**把前几条正文读回来。"
        // 实测事实（本机 2026-09）：中文查询**用空格分词会显著变差** ——
        // 「植物大战僵尸融合版 我是僵尸 攻略」只匹配到「植物」，
        // 而「植物大战僵尸融合版」第一条就是对的 Wiki。这是搜索引擎的分词行为，不是我们的策略。
        L"⚠ 实测：中文查询**别用空格堆多个词**（会把结果带偏），"
        L"把关键短语连写（如 `植物大战僵尸融合版WIKI`）；要多个概念就分几次搜。"
        L"只读公开网页；不碰需要登录的内容。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "query": {"type": "string", "description": "搜索词。中文建议连写关键短语，别用空格堆词"},
            "maxResults": {"type": "integer", "description": "返回几条结果（默认 8，最多 15）"},
            "readTop": {"type": "integer", "description": "同一轮顺带读前 N 条的正文（0=只给清单，默认 0，最多 3）"},
            "maxChars": {"type": "integer", "description": "每条正文的字符上限（默认 8000，最多 20000）"}
        },
        "required": ["query"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring query;
        int maxResults = 8;
        int readTop = 0;
        int maxChars = 8000;
        try {
            const json p = json::parse(ToUtf8(paramsJson));
            if (p.contains("query") && p["query"].is_string())
                query = Trim(FromUtf8(p["query"].get<std::string>()));
            if (p.contains("maxResults") && p["maxResults"].is_number_integer())
                maxResults = p["maxResults"].get<int>();
            if (p.contains("readTop") && p["readTop"].is_number_integer())
                readTop = p["readTop"].get<int>();
            if (p.contains("maxChars") && p["maxChars"].is_number_integer())
                maxChars = p["maxChars"].get<int>();
        } catch (...) {
            return L"[错误] 参数 JSON 解析失败。";
        }
        if (query.empty()) return L"[错误] 缺少 query 参数（搜索词）。";
        maxResults = std::clamp(maxResults, 1, 15);
        readTop = std::clamp(readTop, 0, 3);
        maxChars = std::clamp(maxChars, 1000, 20000);

        const std::wstring searchUrl = BuildSearchUrl(query);
        std::wstring blockErr;
        if (AgentFetchUrlDestinationBlocked(searchUrl, blockErr))
            return L"[错误] " + blockErr;

        std::wstring html;
        std::wstring err;
        if (!FetchWebPage(searchUrl, html, err, 25000))
            return L"[错误] 搜索请求失败：" + err + L"（URL：" + searchUrl + L"）";
        const std::vector<WebSearchResult> results = ParseSearchResultsHtml(html);
        if (results.empty()) {
            // 如实说明：解析不到 ≠ 没有结果（可能是反爬/改版）。绝不编造。
            return L"[错误] 搜索结果页没能解析出条目（页面 "
                + std::to_wstring(html.size()) + L" 字符）——可能是反爬或改版。"
                L"可改用 openWebpage + observePage 看结果页（⚠ 会抢前台），"
                L"或直接把已知网址交给 fetchWebPage。";
        }

        std::wstring out = L"✓ webSearch「" + query + L"」→ 解析到 "
            + std::to_wstring(results.size()) + L" 条（来源：搜索结果页 "
            + std::to_wstring(html.size()) + L" 字符，宿主 HTTP，未开浏览器）\n";
        const int shown = (std::min)(static_cast<int>(results.size()), maxResults);
        for (int i = 0; i < shown; ++i) {
            const WebSearchResult& r = results[static_cast<size_t>(i)];
            out += L"\n[" + std::to_wstring(i + 1) + L"] " + r.title + L"\n    " + r.url;
            if (!r.snippet.empty()) out += L"\n    " + r.snippet;
            out += L"\n";
        }
        if (readTop > 0) {
            const int n = (std::min)(readTop, shown);
            for (int i = 0; i < n; ++i) {
                const WebSearchResult& r = results[static_cast<size_t>(i)];
                std::wstring bErr;
                if (AgentFetchUrlDestinationBlocked(r.url, bErr)) {
                    out += L"\n── 正文[" + std::to_wstring(i + 1) + L"] 跳过：" + bErr + L"\n";
                    continue;
                }
                const WebReadOutcome read = ReadUrlAsText(r.url, false, 25000);
                if (!read.ok) {
                    out += L"\n── 正文[" + std::to_wstring(i + 1) + L"] 读取失败：" + read.err
                        + L"\n";
                    continue;
                }
                std::wstring body = read.text;
                if (body.size() > static_cast<size_t>(maxChars)) {
                    body.resize(static_cast<size_t>(maxChars));
                    body += L"\n…(已截断)";
                }
                out += L"\n──── 正文[" + std::to_wstring(i + 1) + L"] " + r.title
                    + L"（" + read.method + L"，" + std::to_wstring(body.size())
                    + L" 字符）\n" + body + L"\n";
            }
        }
        AiNoteWebFetchUntrusted();
        return out;
    };
    return tool;
}

