#pragma once

#include <string>

#include "agent_tools.h"

// 抓取网页并转纯文本，供 AI 智能体联网获取信息。
// 零外部依赖：直接走 WinHTTP，不要求用户安装 Python/Rust 等爬虫运行时。
AgentTool MakeFetchWebPageTool();

// 纯函数：HTML → 纯文本（去 script/style、实体解码、空白折叠），可单测。
std::wstring HtmlToPlainText(const std::wstring& html);

// 抓取 URL，返回解码后的响应体文本（UTF-8/GBK 自动识别）。
bool FetchWebPage(const std::wstring& url, std::wstring& outBody,
                  std::wstring& err, int timeoutMs = 25000);

// true = 禁止抓取（本机/内网/元数据）。parse 失败也视为禁止。
// 只检查 URL 里的主机名/字面量 IP，不解析 DNS（离线自检可用）。
bool AgentFetchUrlIsBlocked(const std::wstring& url, std::wstring& err);

// 对已解析主机名做 DNS，任一 A/AAAA 落在私网/回环则禁止。解析失败也禁止。
bool AgentFetchResolvedHostBlocked(const std::wstring& host, std::wstring& err);

// 主机名检查 + DNS 再查。实际抓取 / WebView 导航用这个。
bool AgentFetchUrlDestinationBlocked(const std::wstring& url, std::wstring& err);
