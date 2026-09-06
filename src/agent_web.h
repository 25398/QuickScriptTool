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
