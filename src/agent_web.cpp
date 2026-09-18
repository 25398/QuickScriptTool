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

    // 块级标签后换行
    for (const wchar_t* tag : {L"<br", L"</p>", L"</div>", L"</h1>", L"</h2>",
            L"</h3>", L"</h4>", L"</h5>", L"</h6>", L"</li>", L"</tr>", L"</table>"}) {
        std::wstring t(tag);
        for (;;) {
            const size_t p = s.find(t);
            if (p == std::wstring::npos) break;
            s.replace(p, t.size(), L"\n");
        }
    }

    // 去标签
    std::wstring out;
    out.reserve(s.size());
    bool inTag = false;
    for (wchar_t ch : s) {
        if (ch == L'<') { inTag = true; continue; }
        if (ch == L'>') { inTag = false; out.push_back(L' '); continue; }
        if (!inTag) out.push_back(ch);
    }

    // 实体解码
    auto replaceAll = [&](const std::wstring& from, const std::wstring& to) {
        for (;;) {
            const size_t p = out.find(from);
            if (p == std::wstring::npos) break;
            out.replace(p, from.size(), to);
        }
    };
    replaceAll(L"&amp;", L"&");
    replaceAll(L"&lt;", L"<");
    replaceAll(L"&gt;", L">");
    replaceAll(L"&quot;", L"\"");
    replaceAll(L"&#39;", L"'");
    replaceAll(L"&nbsp;", L" ");
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

AgentTool MakeFetchWebPageTool() {
    AgentTool tool;
    tool.name = L"fetchWebPage";
    tool.description =
        L"抓取给人阅读的网页正文（官方文档/帮助/文章）。url 必填 http/https。"
        L"禁止抓站点搜索/用户/动态 JSON API（api.*、/x/web-interface、/api/）——会触发风控。"
        L"浏览站点请 openWebpage + observePage，不要用本工具代替点击。"
        L"返回标题与正文（默认最多 20000 字符，maxChars 上限 50000）。"
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
            return L"[错误] 禁止抓站点 API（会触发风控，如 B 站 412）。"
                L"请 openWebpage 打开给人看的页面，再用 observePage + clickRef/typeRef。"
                L"fetchWebPage 只用于官方文档/帮助页。";
        }
        std::wstring lower;
        lower.reserve(url.size());
        for (wchar_t c : url) lower.push_back(static_cast<wchar_t>(std::towlower(c)));

        std::wstring text;
        std::wstring err;
        bool ok = false;
        std::wstring method;
        if (forceRender) {
            ok = FetchWebPageRendered(url, text, err, 30000);
            method = L"渲染";
        } else {
            std::wstring body;
            ok = FetchWebPage(url, body, err, 25000);
            const std::wstring trimmed = Trim(body);
            const bool looksJson = (trimmed.size() >= 1
                    && (trimmed[0] == L'{' || trimmed[0] == L'['))
                || lower.find(L"/api/") != std::wstring::npos
                || lower.find(L".json") != std::wstring::npos;
            if (ok) {
                text = looksJson ? body : HtmlToPlainText(body);
                method = looksJson ? L"JSON 接口" : L"网页正文";
            }
            // GET 失败，或页面疑似 JS 空壳 → WebView2 渲染兜底
            if (!ok || (!looksJson && text.size() < 80)) {
                std::wstring renderErr;
                if (FetchWebPageRendered(url, text, renderErr, 30000)) {
                    ok = true;
                    method = L"渲染";
                } else if (!ok) {
                    err = renderErr;
                }
            }
        }
        if (!ok) {
            return L"[错误] 抓取失败：" + err
                + L"。请确认 URL 可访问，或让用户提供内容。";
        }
        AiNoteWebFetchUntrusted();
        if (text.size() > static_cast<size_t>(maxChars)) {
            text.resize(static_cast<size_t>(maxChars));
            text += L"\n…(内容过长已截断)";
        }
        std::wstring result = L"✓ 已抓取（" + std::to_wstring(text.size())
            + L" 字符，" + method + L"）\n来源：" + url + L"\n\n" + text;
        return result;
    };
    return tool;
}
