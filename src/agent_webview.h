#pragma once

#include <string>

// 用 App 自带的 WebView2 开一个隐藏页面渲染后取 DOM 文本。
// 用于 JS 动态渲染 / 反爬较重、普通 GET 抓不到的页面。
// 返回 document.title + document.body.innerText（受 timeoutMs 限制）。
bool FetchWebPageRendered(const std::wstring& url, std::wstring& outText,
                          std::wstring& err, int timeoutMs = 30000);
