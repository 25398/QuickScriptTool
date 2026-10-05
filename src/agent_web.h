#pragma once

#include <string>
#include <vector>

#include "agent_tools.h"

// 抓取网页并转纯文本，供 AI 智能体联网获取信息。
// 零外部依赖：直接走 WinHTTP，不要求用户安装 Python/Rust 等爬虫运行时。
AgentTool MakeFetchWebPageTool();

// ★`webSearch`：零 API key 的联网搜索（宿主直连搜索结果页，**不抢前台、不开浏览器**）。
// 由来（用户主诉「他不能直接抓取浏览器的内容吗？还要手动去打开浏览器搜索」+ 实测日志）：
// 模型需要查玩法时有两条路 —— `fetchWebPage`（宿主 HTTP）与 `openWebpage`+`observePage`
// （真开一个 Edge 并抢走前台）。它选了后者：3 轮、游戏丢前台还要 activateWindow 切回来，
// 拿到的还是必应对**另一个游戏**的 AI 摘要。而它本可以一条 HTTP GET 解决。
AgentTool MakeWebSearchTool();

// 一次搜索的结果项。
struct WebSearchResult {
    std::wstring title;
    std::wstring url;
    std::wstring snippet;
};

// 纯函数：从搜索结果页 HTML 里解析出结果清单（可单测，**不发网络请求**）。
/// ⚠ 只认「给人看的结果页」结构（`<li class="b_algo">` 块里的 `<h2><a href>标题</a></h2>`
///   + 首个 `<p>` 摘要）。实测（本机 2026-09 抓 `https://www.bing.com/search?q=…`）：
///   结果页 97KB、10 个 `b_algo` 块，标题/链接/摘要都能按这个形状取到；
///   而 `&format=rss`（看着更好解析）**查询会被降级**（中文单概念查询也只回「植物」这种泛结果）
///   ⇒ 宁可用 HTML。解析不到就**返回空并如实说明**，不许编造结果。
std::vector<WebSearchResult> ParseSearchResultsHtml(const std::wstring& html);

// 纯函数：HTML → **正文**（readability 式主内容抽取）。
//
// 为什么不能只「去标签」（原 `HtmlToPlainText` 的做法）：搜索结果页/文档站/百科页的
// 正文外面裹着导航、侧栏、页脚、相关阅读、登录提示……朴素去标签会把它们全塞进结果，
// 模型要在噪声里找答案，token 也白烧（开源侧的共识与做法见 docs §52：
// Mozilla Readability（Apache-2.0）、trafilatura（Apache-2.0）、go-readability、
// postlight/parser 都是「先剔样板、再按文本密度/链接密度给候选容器打分、取最高分」）。
//
// 做法（本地紧凑重写，不引入依赖）：①剔 script/style/noscript/svg/iframe/注释与
// nav/header/footer/aside/form 容器；②按 class/id 关键词给候选块加减分（article/content/
// main/post/entry 加分；nav/menu/sidebar/comment/footer/share/ad/related 减分）；
// ③候选块（p/div/article/section/td/li）累加「文本长度 + 逗号句号数 + 段落数」，
// 再按**链接密度**扣分（链接文字占比高的块是导航，不是正文）；④取最高分容器的文本。
// ⚠ 抽不出来时**如实回退**到整页纯文本（由调用方标注路径），绝不返回空让模型以为页面没内容。
std::wstring HtmlExtractMainText(const std::wstring& html, std::wstring* outTitle = nullptr);

// 纯函数：HTML → 纯文本（去 script/style、实体解码、空白折叠），可单测。
std::wstring HtmlToPlainText(const std::wstring& html);

// 抓取 URL，返回解码后的响应体文本（UTF-8/GBK 自动识别）。
bool FetchWebPage(const std::wstring& url, std::wstring& outBody,
                  std::wstring& err, int timeoutMs = 25000);

// 拼一个搜索结果页 URL（纯函数：只做转义与拼接，可单测）。
std::wstring BuildSearchUrl(const std::wstring& query);

// true = 禁止抓取（本机/内网/元数据）。parse 失败也视为禁止。
// 只检查 URL 里的主机名/字面量 IP，不解析 DNS（离线自检可用）。
bool AgentFetchUrlIsBlocked(const std::wstring& url, std::wstring& err);

// 对已解析主机名做 DNS，任一 A/AAAA 落在私网/回环则禁止。解析失败也禁止。
bool AgentFetchResolvedHostBlocked(const std::wstring& host, std::wstring& err);

// 主机名检查 + DNS 再查。实际抓取 / WebView 导航用这个。
bool AgentFetchUrlDestinationBlocked(const std::wstring& url, std::wstring& err);
