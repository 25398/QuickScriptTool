// ──────────────────────────────────────────────────────────────────
// agent_web.cpp — AI 智能体网页抓取工具
// 零外部依赖的网页抓取：WinHTTP GET → 字符集识别 → HTML 转纯文本。
// 覆盖「文章/文档/新闻/天气页」等静态内容；JS 渲染与强反爬站点不保证。
// ──────────────────────────────────────────────────────────────────
#include "agent_web.h"

#include "agent_webview.h"
#include "utils.h"

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <string>
#include <vector>

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
    const size_t colon = hostPort.find(L':');
    if (colon != std::wstring::npos) {
        out.host = hostPort.substr(0, colon);
        const std::wstring portStr = hostPort.substr(colon + 1);
        try {
            out.port = static_cast<INTERNET_PORT>(std::stoi(portStr));
        } catch (...) {
            err = L"URL 端口无效";
            return false;
        }
    } else {
        out.host = hostPort;
    }
    if (out.host.empty()) {
        err = L"URL 缺少主机名";
        return false;
    }
    return true;
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

bool FetchWebPage(const std::wstring& url, std::wstring& outBody,
                  std::wstring& err, int timeoutMs) {
    outBody.clear();
    WebUrl parsed;
    if (!ParseWebUrl(url, parsed, err)) return false;

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
    // 浏览器化请求头：降低简单反爬 403（如微博接口），并支持 JSON API
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
        L"抓取网页内容并转为纯文本（AI 智能体联网获取信息用）。"
        L"url 必填（http/https）。返回页面标题与正文文本（默认最多 20000 字符，"
        L"可用 maxChars 调整，上限 50000）。"
        L"JSON API（Content-Type=application/json 或响应以 {/[ 开头）返回原始 JSON 文本，"
        L"可直接从中提取结构化数据（如热搜榜）。"
        L"默认先走轻量 GET；GET 失败或页面疑似 JS 空壳时，自动用 App 内置 WebView2 "
        L"隐藏渲染后再取 DOM（覆盖 JS 动态页面）。也可传 render=true 强制走渲染。"
        L"禁止抓取本地地址/内部服务（如 localhost、127.0.0.1、内网 IP）。";
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

        std::wstring lower;
        lower.reserve(url.size());
        for (wchar_t c : url) lower.push_back(static_cast<wchar_t>(std::towlower(c)));
        if (lower.find(L"http://") != 0 && lower.find(L"https://") != 0)
            return L"[错误] 仅支持 http/https URL。";
        if (lower.find(L"localhost") != std::wstring::npos
            || lower.find(L"127.0.0.1") != std::wstring::npos
            || lower.find(L"::1") != std::wstring::npos
            || lower.find(L"192.168.") != std::wstring::npos
            || lower.find(L"10.") == 0
            || lower.find(L"172.") == 0)
            return L"[错误] 禁止抓取本地/内网地址。";

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
