#include "agent_web.h"

#include <string>

// AiActionRouterSelfTest 不链 agent_web.cpp（WinHTTP/WebView2）。产品与
// AgentAssistantSelfTest 仍用真实现。
AgentTool MakeFetchWebPageTool() {
    AgentTool tool;
    tool.name = L"fetchWebPage";
    tool.description = L"stub";
    tool.parameters_json = LR"({"type":"object","properties":{"url":{"type":"string"}},"required":["url"]})";
    tool.execute = [](const std::wstring&) -> std::wstring {
        return L"[错误] fetchWebPage stub";
    };
    return tool;
}

std::wstring HtmlToPlainText(const std::wstring&) { return {}; }

bool FetchWebPage(const std::wstring&, std::wstring&, std::wstring& err, int) {
    err = L"stub";
    return false;
}

bool AgentFetchUrlIsBlocked(const std::wstring&, std::wstring&) { return false; }
bool AgentFetchResolvedHostBlocked(const std::wstring&, std::wstring&) { return false; }
bool AgentFetchUrlDestinationBlocked(const std::wstring&, std::wstring&) { return false; }
