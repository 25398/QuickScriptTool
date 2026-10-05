// =============================================================================
// AgentMcpSelfTest — MCP 客户端自检（协议层纯函数 + 进程层端到端）
// =============================================================================
// 被测：src/agent_mcp.cpp
//
// 端到端怎么测而不依赖网络/第三方：**本 exe 自己当假 server**。
//   带 --fake-mcp-server 启动时，它就是一个最小的 stdio JSON-RPC 服务端
//   （initialize / tools/list / tools/call）。用例用当前 exe 的路径起一个子进程，
//   于是握手、分帧、超时、关闭这一整条真链路都被覆盖 —— 不装 GenOffice、不联网也能跑。
//
//   MSBuild ... /t:AgentMcpSelfTest
//   build\Release\AgentMcpSelfTest.exe --json
//   build\Release\AgentMcpSelfTest.exe --fake-mcp-server   （被用例内部调用）
// =============================================================================
#include "selftest_harness.h"

#include "agent_mcp.h"
#include "utils.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"build_request_line_shape", L"default",
        L"请求行含 jsonrpc/id/method/params，且是单行"},
    {L"build_request_line_no_bare_newline", L"default",
        L"★params 里带字面换行时输出仍不含裸换行（stdio 按行分帧，混进换行会拆坏流）"},
    {L"build_notification_has_no_id", L"default",
        L"通知不带 id（服务端不该为它回响应）"},
    {L"parse_result_matched_id", L"default",
        L"id 匹配的成功响应解析出 result"},
    {L"parse_ignores_other_id", L"default",
        L"别的 id 的响应一律忽略，不当错误"},
    {L"parse_ignores_non_json_log_line", L"default",
        L"★服务端往 stdout 打日志行（非 JSON）必须忽略 —— 当错误会把能用的 server 判成坏的"},
    {L"parse_error_response_reported", L"default",
        L"id 匹配的 error 响应报出服务端消息"},
    {L"parse_tools_list", L"default",
        L"从 tools/list 抽出名字/描述/schema；无名条目跳过"},
    {L"qualified_name_roundtrip", L"default",
        L"mcp__server__tool 命名可往返反解"},
    {L"split_rejects_foreign_name", L"default",
        L"非 mcp__ 前缀的名字不被误认"},
    {L"extract_tool_text_concatenates", L"default",
        L"多段 text content 拼接"},
    {L"extract_tool_text_flags_non_text", L"default",
        L"★没给落盘目录时，图片等非文本内容不被静默丢弃（否则模型以为「什么都没返回」）"},
    {L"extract_tool_image_persists", L"default",
        L"★★图片落盘 + 打 [[AGENT_IMG:…]] 标记（render 出的 PNG 只有走到模型眼前，验收才成立）"},
    {L"extract_tool_unknown_image_mime_is_honest", L"default",
        L"★宿主认不出的图片格式（svg）：不假装能看图、也不悄悄丢，留一句说得清的话"},
    {L"extract_tool_error_is_error", L"default",
        L"isError=true 判为失败并带上说明"},
    {L"config_missing_file_is_ok", L"default",
        L"配置文件不存在 = 没有外部 server，不是错误（绝大多数用户如此）"},
    {L"config_bad_json_is_error", L"default",
        L"JSON 坏掉才是错误"},
    {L"config_skips_incomplete_entries", L"default",
        L"缺 name/command 的条目跳过（否则会变成一条每次都失败的工具）"},
    {L"config_roundtrip", L"default",
        L"序列化再解析得到同样的 server 列表"},
    {L"runnable_image_by_extension", L"default",
        L"★CreateProcess 只能起 .exe/.cmd/.bat；无扩展名的脚本（Git Bash 那种）判为不可用"},
    {L"sweep_stale_mcp_images", L"default",
        L"★附图落盘目录按年龄清理（它**只增不减**，不清会无限长）：太老的删、新的留、"
        L"**未来时间戳的留**（时钟不可信时宁可不删）；真建文件 + 真改写入时间"},
    {L"orphan_guard_active_after_start", L"macro",
        L"★`Start()` 必须把 MCP 子进程挂进 KILL_ON_JOB_CLOSE 的 Job —— `Stop()` 管不了崩溃，"
        L"没有 Job 就会在用户机器上攒出一串孤儿 node.exe"},
    {L"kill_on_close_job_kills_child", L"macro",
        L"★机制本身：真起一个 60s 长命子进程 → 挂 Job → 关 Job 句柄 → 它必须**立刻**死"
        L"（不能只看「句柄非空」就当防护生效了）"},
    {L"concurrent_call_tool_is_serialized", L"macro",
        L"★同一 client 上并发 CallTool 必须被串行化：`ReadResponse` 会把别人的响应 pop 掉丢弃"
        L"⇒ 不串行就会互相偷响应、输的一方等到 180s 超时（产品当前串行调用工具，属预防性地雷）"},
    {L"e2e_initialize_and_list_tools", L"macro",
        L"★端到端：起假 server 子进程 → 握手 → 列工具（真进程、真管道）"},
    {L"e2e_call_tool_roundtrip", L"macro",
        L"★端到端：tools/call 往返，参数与结果都对"},
    {L"e2e_tool_error_surfaces", L"macro",
        L"★端到端：isError=true 的工具失败要如实上报"},
    {L"e2e_bad_command_fails_gracefully", L"macro",
        L"★起不来的 server 只报错、不崩、不挂"},
    {L"e2e_timeout_does_not_hang", L"macro",
        L"★服务端不回响应时按超时返回，不许永久卡住"},
};

// ── 假 MCP server（本 exe 以 --fake-mcp-server 启动时）────────────────

std::string FakeToolsList() {
    nlohmann::json tools = nlohmann::json::array();
    tools.push_back({ {"name", "echo"},
        {"description", "回显传入的 text"},
        {"inputSchema", {{"type", "object"},
            {"properties", {{"text", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"text"})}}} });
    tools.push_back({ {"name", "fail"},
        {"description", "总是失败（测 isError 通路）"},
        {"inputSchema", {{"type", "object"}, {"properties", nlohmann::json::object()}}} });
    tools.push_back({ {"name", "image"},
        {"description", "只返回非文本内容"},
        {"inputSchema", {{"type", "object"}, {"properties", nlohmann::json::object()}}} });
    tools.push_back({ {"description", "没有 name 的条目（应被跳过）"},
        {"inputSchema", {{"type", "object"}}} });
    nlohmann::json result;
    result["tools"] = tools;
    return result.dump();
}

void FakeServerWriteLine(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int RunFakeMcpServer() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        nlohmann::json req;
        try {
            req = nlohmann::json::parse(line);
        } catch (...) {
            continue;   // 忽略坏行，和真 server 一样
        }
        if (!req.is_object()) continue;
        const std::string method = req.contains("method") && req["method"].is_string()
            ? req["method"].get<std::string>() : std::string();
        const bool hasId = req.contains("id") && req["id"].is_number_integer();
        const int id = hasId ? req["id"].get<int>() : 0;

        if (method == "initialize") {
            nlohmann::json result;
            result["protocolVersion"] = "2024-11-05";
            result["capabilities"] = {{"tools", nlohmann::json::object()}};
            result["serverInfo"] = {{"name", "fake"}, {"version", "1"}};
            nlohmann::json resp;
            resp["jsonrpc"] = "2.0";
            resp["id"] = id;
            resp["result"] = result;
            FakeServerWriteLine(resp.dump());
            continue;
        }
        if (method == "notifications/initialized") continue;   // 通知不回
        if (method == "tools/list") {
            nlohmann::json resp;
            resp["jsonrpc"] = "2.0";
            resp["id"] = id;
            resp["result"] = nlohmann::json::parse(FakeToolsList());
            FakeServerWriteLine(resp.dump());
            continue;
        }
        if (method == "tools/call") {
            const std::string name = req["params"].contains("name")
                && req["params"]["name"].is_string()
                ? req["params"]["name"].get<std::string>() : std::string();
            nlohmann::json content = nlohmann::json::array();
            bool isError = false;
            if (name == "echo") {
                std::string text = req["params"].contains("arguments")
                    && req["params"]["arguments"].contains("text")
                    && req["params"]["arguments"]["text"].is_string()
                    ? req["params"]["arguments"]["text"].get<std::string>() : std::string();
                content.push_back({{"type", "text"}, {"text", "echo:" + text}});
            } else if (name == "fail") {
                content.push_back({{"type", "text"}, {"text", "boom"}});
                isError = true;
            } else if (name == "image") {
                content.push_back({{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}});
            } else if (name == "slow") {
                // 故意不回：测超时路径
                continue;
            } else {
                nlohmann::json resp;
                resp["jsonrpc"] = "2.0";
                resp["id"] = id;
                resp["error"] = {{"code", -32601}, {"message", "unknown tool"}};
                FakeServerWriteLine(resp.dump());
                continue;
            }
            nlohmann::json result;
            result["content"] = content;
            result["isError"] = isError;
            nlohmann::json resp;
            resp["jsonrpc"] = "2.0";
            resp["id"] = id;
            resp["result"] = result;
            FakeServerWriteLine(resp.dump());
            continue;
        }
        // 未实现的方法
        if (hasId) {
            nlohmann::json resp;
            resp["jsonrpc"] = "2.0";
            resp["id"] = id;
            resp["error"] = {{"code", -32601}, {"message", "method not found"}};
            FakeServerWriteLine(resp.dump());
        }
    }
    return 0;
}

// ── 协议层用例 ───────────────────────────────────────────────────

void CaseBuildRequestLineShape() {
    const std::string line = qst::agent::McpBuildRequestLine(7, "tools/list", R"({"a":1})");
    nlohmann::json j;
    bool parsed = true;
    try { j = nlohmann::json::parse(line); } catch (...) { parsed = false; }
    const bool good = parsed && j.is_object()
        && j["jsonrpc"] == "2.0" && j["id"] == 7 && j["method"] == "tools/list"
        && j["params"]["a"] == 1
        && line.find('\n') == std::string::npos
        && line.find('\r') == std::string::npos;
    Emit(L"build_request_line_shape", good, selftest::JsonEscape(
        std::wstring(L"line=" + FromUtf8(line)).c_str()).c_str());
}

void CaseBuildRequestLineNoBareNewline() {
    // params 里带**字面换行**（多行提示词/文档内容很常见）
    const std::string line = qst::agent::McpBuildRequestLine(1, "tools/call",
        "{\"text\":\"line1\nline2\r\nline3\"}");
    const bool single = line.find('\n') == std::string::npos
        && line.find('\r') == std::string::npos;
    bool parsed = true;
    try { (void)nlohmann::json::parse(line); } catch (...) { parsed = false; }
    Emit(L"build_request_line_no_bare_newline", single && parsed,
        single ? L"" : L"输出里出现了裸换行（会把 stdio 分帧拆坏）");
}

void CaseBuildNotificationHasNoId() {
    const std::string line = qst::agent::McpBuildNotificationLine(
        "notifications/initialized", "");
    nlohmann::json j;
    bool parsed = true;
    try { j = nlohmann::json::parse(line); } catch (...) { parsed = false; }
    const bool good = parsed && j.is_object() && !j.contains("id")
        && j["method"] == "notifications/initialized";
    Emit(L"build_notification_has_no_id", good, L"");
}

void CaseParseResultMatchedId() {
    std::string result;
    std::wstring err;
    const auto k = qst::agent::McpParseResponseLine(
        R"({"jsonrpc":"2.0","id":3,"result":{"ok":true}})", 3, result, err);
    const bool good = k == qst::agent::McpLineKind::Result
        && result.find("\"ok\":true") != std::string::npos;
    Emit(L"parse_result_matched_id", good, L"");
}

void CaseParseIgnoresOtherId() {
    std::string result;
    std::wstring err;
    const auto k = qst::agent::McpParseResponseLine(
        R"({"jsonrpc":"2.0","id":99,"result":{}})", 3, result, err);
    const bool good = k == qst::agent::McpLineKind::Ignore && err.empty();
    Emit(L"parse_ignores_other_id", good, err.c_str());
}

void CaseParseIgnoresNonJsonLogLine() {
    std::string result;
    std::wstring err;
    const auto k = qst::agent::McpParseResponseLine(
        "[genoffice] starting up, node v22 ...", 3, result, err);
    const auto k2 = qst::agent::McpParseResponseLine(
        R"({"jsonrpc":"2.0","method":"notifications/progress","params":{}})", 3, result, err);
    const bool good = k == qst::agent::McpLineKind::Ignore
        && k2 == qst::agent::McpLineKind::Ignore && err.empty();
    Emit(L"parse_ignores_non_json_log_line", good,
        err.empty() ? L"" : (L"日志行被当成错误：" + err).c_str());
}

void CaseParseErrorResponseReported() {
    std::string result;
    std::wstring err;
    const auto k = qst::agent::McpParseResponseLine(
        R"({"jsonrpc":"2.0","id":3,"error":{"code":-32601,"message":"method not found"}})",
        3, result, err);
    const bool good = k == qst::agent::McpLineKind::Error
        && err.find(L"method not found") != std::wstring::npos;
    Emit(L"parse_error_response_reported", good, err.c_str());
}

void CaseParseToolsList() {
    std::vector<qst::agent::McpRemoteTool> tools;
    std::wstring err;
    const bool ok = qst::agent::McpParseToolsList(FakeToolsList(), tools, err);
    // 4 条里第 4 条没有 name，必须被跳过
    const bool good = ok && tools.size() == 3
        && tools[0].name == "echo"
        && tools[0].description == "回显传入的 text"
        && !tools[0].inputSchema.empty()
        && tools[1].name == "fail"
        && tools[2].name == "image";
    Emit(L"parse_tools_list", good,
        (L"ok=" + std::to_wstring(ok ? 1 : 0) + L" n=" + std::to_wstring(tools.size())
            + L" err=" + err).c_str());
}

void CaseQualifiedNameRoundtrip() {
    const std::string q = qst::agent::McpQualifiedToolName(L"genoffice", "sheet_read");
    std::wstring server;
    std::string tool;
    const bool split = qst::agent::McpSplitQualifiedToolName(q, server, tool);
    const bool good = q == "mcp__genoffice__sheet_read" && split
        && server == L"genoffice" && tool == "sheet_read";
    Emit(L"qualified_name_roundtrip", good,
        (L"q=" + FromUtf8(q) + L" server=" + server + L" tool=" + FromUtf8(tool)).c_str());
}

void CaseSplitRejectsForeignName() {
    std::wstring server;
    std::string tool;
    const bool a = qst::agent::McpSplitQualifiedToolName("readDocument", server, tool);
    const bool b = qst::agent::McpSplitQualifiedToolName("mcp__onlyserver", server, tool);
    const bool c = qst::agent::McpSplitQualifiedToolName("mcp____tool", server, tool);
    Emit(L"split_rejects_foreign_name", !a && !b && !c,
        (L"a=" + std::to_wstring(a ? 1 : 0) + L" b=" + std::to_wstring(b ? 1 : 0)
            + L" c=" + std::to_wstring(c ? 1 : 0)).c_str());
}

void CaseExtractToolTextConcatenates() {
    qst::agent::McpToolContent content;
    std::wstring err;
    const bool ok = qst::agent::McpExtractToolResult(
        R"({"content":[{"type":"text","text":"第一段"},{"type":"text","text":"第二段"}]})",
        L"", content, err);
    const bool good = ok && content.text == L"第一段\n第二段" && content.imageFiles.empty();
    Emit(L"extract_tool_text_concatenates", good, (L"text=" + content.text).c_str());
}

void CaseExtractToolTextFlagsNonText() {
    qst::agent::McpToolContent content;
    std::wstring err;
    // 没给落盘目录 ⇒ 图片只能留占位说明（自检路径，不产生副作用）
    const bool ok = qst::agent::McpExtractToolResult(
        R"({"content":[{"type":"image","data":"UE5HIQ==","mimeType":"image/png"}]})",
        L"", content, err);
    const bool good = ok && content.text.find(L"非文本内容") != std::wstring::npos
        && content.text.find(L"image") != std::wstring::npos
        && content.imageFiles.empty();
    Emit(L"extract_tool_text_flags_non_text", good, (L"text=" + content.text).c_str());
}

/// 落盘目录：临时目录下建一个唯一子目录，用例自己收尾。
bool MakeTempSpoolDir(std::wstring& out) {
    wchar_t base[MAX_PATH * 2] = {};
    const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(base)), base);
    if (n == 0 || n >= std::size(base)) return false;
    out = std::wstring(base) + L"qst_mcp_selftest_"
        + std::to_wstring(GetTickCount64());
    return EnsureDirectoryTree(out);   // 统一走 utils 的逐级建目录
}

void RemoveTempSpoolDir(const std::wstring& dir) {
    // 目录里只有本用例写下的图片；逐个删掉再删目录
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

void CaseExtractToolImagePersists() {
    std::wstring dir;
    if (!MakeTempSpoolDir(dir)) {
        Emit(L"extract_tool_image_persists", false, L"建临时目录失败");
        return;
    }
    qst::agent::McpToolContent content;
    std::wstring err;
    // base64 "UE5HIQ==" = 4 字节 50 4E 47 21（"PNG!"）—— 用于逐字节校验落盘内容
    const bool ok = qst::agent::McpExtractToolResult(
        R"({"content":[{"type":"text","text":"已渲染 2 页"},
            {"type":"image","data":"UE5HIQ==","mimeType":"image/png"}]})",
        dir, content, err);

    bool good = ok && content.imageFiles.size() == 1;
    std::wstring detail = L"n=" + std::to_wstring(content.imageFiles.size());
    if (good) {
        const std::wstring& p = content.imageFiles[0];
        // ① 扩展名必须是宿主认得的（否则图送不进请求）
        const bool extOk = p.size() > 4 && p.substr(p.size() - 4) == L".png";
        // ② 文本里必须有附图标记，且标记里的路径就是落盘路径
        const bool markerOk = content.text.find(L"[[AGENT_IMG:" + p + L"]]")
            != std::wstring::npos;
        // ③ 文件真的存在，且内容 = 解码后的字节
        std::ifstream f(p.c_str(), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)),
            std::istreambuf_iterator<char>());
        const bool bytesOk = bytes.size() == 4
            && static_cast<unsigned char>(bytes[0]) == 0x50
            && static_cast<unsigned char>(bytes[1]) == 0x4E
            && static_cast<unsigned char>(bytes[2]) == 0x47
            && static_cast<unsigned char>(bytes[3]) == 0x21;
        // ④ 原有文本不能丢
        const bool textKept = content.text.find(L"已渲染 2 页") != std::wstring::npos;
        good = extOk && markerOk && bytesOk && textKept;
        detail += L" ext=" + std::to_wstring(extOk ? 1 : 0)
            + L" marker=" + std::to_wstring(markerOk ? 1 : 0)
            + L" bytes=" + std::to_wstring(bytesOk ? 1 : 0)
            + L" textKept=" + std::to_wstring(textKept ? 1 : 0)
            + L" path=" + p;
    }
    RemoveTempSpoolDir(dir);
    Emit(L"extract_tool_image_persists", good, detail.c_str());
}

void CaseExtractToolUnknownImageMimeIsHonest() {
    std::wstring dir;
    if (!MakeTempSpoolDir(dir)) {
        Emit(L"extract_tool_unknown_image_mime_is_honest", false, L"建临时目录失败");
        return;
    }
    qst::agent::McpToolContent content;
    std::wstring err;
    // svg 不是宿主认得的位图格式：**不许**落盘成 .png 假装能看图，
    // 也不许悄悄丢掉 —— 要留一句说得清的话。
    const bool ok = qst::agent::McpExtractToolResult(
        R"({"content":[{"type":"image","data":"UE5HIQ==","mimeType":"image/svg+xml"}]})",
        dir, content, err);
    const bool good = ok && content.imageFiles.empty()
        && content.text.find(L"[[AGENT_IMG:") == std::wstring::npos
        && content.text.find(L"svg") != std::wstring::npos;
    RemoveTempSpoolDir(dir);
    Emit(L"extract_tool_unknown_image_mime_is_honest", good,
        (L"text=" + content.text).c_str());
}

void CaseExtractToolErrorIsError() {
    qst::agent::McpToolContent content;
    std::wstring err;
    const bool ok = qst::agent::McpExtractToolResult(
        R"({"content":[{"type":"text","text":"boom"}],"isError":true})",
        L"", content, err);
    const bool good = !ok && content.text == L"boom" && !err.empty();
    Emit(L"extract_tool_error_is_error", good,
        (L"text=" + content.text + L" err=" + err).c_str());
}

// ── 配置用例 ─────────────────────────────────────────────────────

void CaseConfigMissingFileIsOk() {
    std::vector<qst::agent::McpServerConfig> servers;
    std::wstring err;
    const bool ok = qst::agent::ParseMcpServersJson("", servers, err);
    const bool good = ok && servers.empty() && err.empty();
    Emit(L"config_missing_file_is_ok", good, err.c_str());
}

void CaseConfigBadJsonIsError() {
    std::vector<qst::agent::McpServerConfig> servers;
    std::wstring err;
    const bool ok = qst::agent::ParseMcpServersJson("{not json", servers, err);
    const bool good = !ok && !err.empty();
    Emit(L"config_bad_json_is_error", good, err.c_str());
}

void CaseConfigSkipsIncompleteEntries() {
    const char* text = R"({"servers":[
        {"name":"good","command":"genoffice","args":["mcp"]},
        {"name":"noCommand"},
        {"command":"orphan"},
        {"name":"disabled","command":"x","enabled":false}
    ]})";
    std::vector<qst::agent::McpServerConfig> servers;
    std::wstring err;
    const bool ok = qst::agent::ParseMcpServersJson(text, servers, err);
    const bool good = ok && servers.size() == 2
        && servers[0].name == L"good"
        && servers[0].args.size() == 1 && servers[0].args[0] == L"mcp"
        && servers[1].name == L"disabled" && !servers[1].enabled;
    Emit(L"config_skips_incomplete_entries", good,
        (L"n=" + std::to_wstring(servers.size())).c_str());
}

void CaseConfigRoundtrip() {
    std::vector<qst::agent::McpServerConfig> in;
    qst::agent::McpServerConfig a;
    a.name = L"genoffice";
    a.command = L"C:\\Program Files\\GenOffice\\genoffice.cmd";
    a.args = { L"mcp" };
    a.cwd = L"C:\\temp\\带 空格";
    in.push_back(a);
    const std::string json = qst::agent::SerializeMcpServersJson(in);

    std::vector<qst::agent::McpServerConfig> out;
    std::wstring err;
    const bool ok = qst::agent::ParseMcpServersJson(json, out, err);
    const bool good = ok && out.size() == 1
        && out[0].name == a.name && out[0].command == a.command
        && out[0].args == a.args && out[0].cwd == a.cwd && out[0].enabled;
    Emit(L"config_roundtrip", good, err.c_str());
}

// ⚠ 这里原来有 `CaseGenOfficeConfigShape()`（验「自动接入 GenOffice」的配置形状）——
//   **已随功能一起删除**（2026-09-23）：产品不主动依赖、也不主动探测任何第三方软件。
//   通用 MCP 客户端的配置形状由 `config_*` 几个用例覆盖。

void CaseRunnableImageByExtension() {
    using qst::agent::McpCommandIsRunnable;
    // 期望值**逐条手写**，不复用被测表达式（否则等于自证）
    struct Row { const wchar_t* path; bool want; };
    const Row rows[] = {
        { L"genoffice.cmd", true },
        { L"C:\\Program Files\\GenOffice\\resources\\cli\\genoffice.cmd", true },
        { L"genoffice.exe", true },
        { L"tool.bat", true },
        { L"genoffice", false },                                    // ← Git Bash 那个
        { L"C:\\Program Files\\GenOffice\\resources\\cli\\genoffice", false },
        { L"C:\\some.dir\\genoffice", false },                      // 点在目录名里，不算扩展名
        { L"C:\\some.dir\\genoffice.cmd", true },
        { L"genoffice.ps1", false },
        { L"genoffice.cmd.bak", false },
        { L"", false },
    };
    std::wstring bad;
    for (const auto& r : rows) {
        const bool got = McpCommandIsRunnable(r.path);
        if (got != r.want) {
            bad += L"[";
            bad += r.path;
            bad += got ? L"→true " : L"→false ";
            bad += L"] ";
        }
    }
    Emit(L"runnable_image_by_extension", bad.empty(),
        bad.empty() ? L"" : (L"不符期望：" + bad).c_str());
}

// ── 附图目录清理 ──────────────────────────────────────────────────────────────
// 为什么必须测：这个目录**只增不减**（标记被消费时会从文本里删掉，历史不留路径），
// 不清就会在用户 AppDir 里无限长。而「按年龄删」很容易删错 ⇒ 真建文件、真改写入
// 时间、真跑清扫，三种边界都要看：太老的删、新的留、**未来时间戳的留**。
void CaseSweepStaleMcpImages() {
    using qst::agent::SweepStaleMcpImagesIn;
    std::wstring dir;
    if (!MakeTempSpoolDir(dir)) {
        Emit(L"sweep_stale_mcp_images", false, L"建临时目录失败");
        return;
    }
    // 真目录结构是 `<spool>\<server>\*.png`（CallTool 懒建），所以子目录也要覆盖。
    const std::wstring serverDir = dir + L"\\genoffice";
    EnsureDirectoryTree(serverDir);

    // daysAgo > 0 = 往回拨；daysAgo < 0 = 拨到未来（测「时钟不可信时宁可不删」）
    auto writeAged = [](const std::wstring& p, int daysAgo) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const char payload[] = "PNG";
        DWORD written = 0;
        const bool wrote = WriteFile(h, payload, 3, &written, nullptr) != 0;
        CloseHandle(h);
        if (!wrote) return false;
        if (daysAgo == 0) return true;
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        const ULONGLONG delta = static_cast<ULONGLONG>(daysAgo < 0 ? -daysAgo : daysAgo)
            * 24ULL * 3600ULL * 10000000ULL;
        u.QuadPart = (daysAgo > 0) ? (u.QuadPart - delta) : (u.QuadPart + delta);
        FILETIME aged{};
        aged.dwLowDateTime = u.LowPart;
        aged.dwHighDateTime = u.HighPart;
        HANDLE hw = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hw == INVALID_HANDLE_VALUE) return false;
        const bool ok = SetFileTime(hw, nullptr, nullptr, &aged) != 0;
        CloseHandle(hw);
        return ok;
    };

    const std::wstring fOld = serverDir + L"\\old.png";
    const std::wstring fFresh = serverDir + L"\\fresh.png";
    const std::wstring fFuture = serverDir + L"\\future.png";
    if (!writeAged(fOld, 30) || !writeAged(fFresh, 0) || !writeAged(fFuture, -3)) {
        Emit(L"sweep_stale_mcp_images", false, (L"造 fixture 失败：" + dir).c_str());
        return;
    }
    auto exists = [](const std::wstring& p) {
        return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    };

    std::vector<std::wstring> removed;
    const int n = SweepStaleMcpImagesIn(dir, 7, &removed);
    const bool oldGone = !exists(fOld);
    const bool freshKept = exists(fFresh);
    const bool futureKept = exists(fFuture);
    const int again = SweepStaleMcpImagesIn(dir, 7, nullptr);
    const bool idempotent = (again == 0);

    const bool ok = (n == 1) && (removed.size() == 1) && oldGone && freshKept
        && futureKept && idempotent;
    Emit(L"sweep_stale_mcp_images", ok,
        (L"n=" + std::to_wstring(n) + L" again=" + std::to_wstring(again)
            + L" oldGone=" + (oldGone ? L"1" : L"0")
            + L" freshKept=" + (freshKept ? L"1" : L"0")
            + L" futureKept=" + (futureKept ? L"1" : L"0")).c_str());

    // 用例自建自删（本仓约定：自检不许留垃圾）
    DeleteFileW(fOld.c_str());
    DeleteFileW(fFresh.c_str());
    DeleteFileW(fFuture.c_str());
    RemoveDirectoryW(serverDir.c_str());
    RemoveDirectoryW(dir.c_str());
}

// ── 孤儿防护 ──────────────────────────────────────────────────────────────────
// 为什么必须测：`Stop()` 是**优雅**路径，宿主被强杀/崩溃时它根本没机会跑
// ⇒ 起出来的 MCP server 子进程会一直挂着（`genoffice mcp` 是个 Node 进程，
// 几百 MB + 持有管道），用户任务管理器里会攒出一串看不懂的 node.exe。
// 两个用例分工：① 证明 `Start()` 真的把子进程挂上了 Job；② 证明那个 Job
// **确实会在句柄关闭时杀掉子进程**（机制本身，不是只看句柄非空）。

bool SelfPath(std::wstring& out);   // 定义在下面「端到端」段，这里先用

// ── 单通道串行 ────────────────────────────────────────────────────────────────
// 为什么必须有：`ReadResponse` 会把「别人的 id」的响应从队列里 **pop 掉并丢弃**
// （`Ignore → continue`）⇒ 同一个 client 上两个在途请求会互相偷响应，
// 输的那一方一直等到**超时**（产品默认 180s）才返回。
// ⚠ 产品当前是**串行**调用工具的（`agent_core.cpp` 的 `tool_calls` 循环），
//   所以这个 bug 今天打不到 —— 但别把「调用方串行」当隐含前提。
//
// 用例怎么做到**确定性**（不靠线程调度赌博）：
//   A 线程先发一个**永远不回**的 `slow`（服务器 `continue`，不响应），占住 1200ms；
//   200ms 后 B 线程发一个**秒回**的 `echo`。
//   · 有串行锁：B 根本发不出去，要等 A 释放 ⇒ B 自己耗时 ≈1000ms；
//   · 没串行锁：B 立刻发出、立刻拿到响应 ⇒ B 耗时只有几毫秒。
//   ⇒ 断言 `B 耗时 >= 800ms` 就能把两种实现分开，且与「谁先 pop」无关。
void CaseConcurrentCallToolIsSerialized() {
    std::wstring self;
    if (!SelfPath(self)) {
        Emit(L"concurrent_call_tool_is_serialized", false, L"取自身路径失败");
        return;
    }
    qst::agent::McpServerConfig cfg;
    cfg.name = L"selftest_serial";
    cfg.command = self;
    cfg.args = { L"--fake-mcp-server" };

    qst::agent::McpStdioClient client;
    std::wstring err;
    if (!client.Start(cfg, err) || !client.Initialize(err, 5000)) {
        client.Stop();
        Emit(L"concurrent_call_tool_is_serialized", false, (L"启动/握手失败：" + err).c_str());
        return;
    }

    std::atomic<ULONGLONG> aStart{0}, bStart{0}, bEnd{0};
    std::atomic_bool bOk{false};
    std::wstring aErr, bErr;

    std::thread ta([&]() {
        std::wstring text;
        aStart.store(GetTickCount64());
        client.CallTool("slow", "{}", text, aErr, 1200);   // 服务器永不响应
    });
    Sleep(200);
    std::thread tb([&]() {
        std::wstring text;
        bStart.store(GetTickCount64());
        bOk.store(client.CallTool("echo", R"({"text":"B"})", text, bErr, 1200));
        bEnd.store(GetTickCount64());
    });
    ta.join();
    tb.join();

    const ULONGLONG bDur = bEnd.load() - bStart.load();
    const bool aTimedOut = aErr.find(L"超时") != std::wstring::npos;
    const bool ok = bOk.load() && aTimedOut && bDur >= 800;
    client.Stop();
    Emit(L"concurrent_call_tool_is_serialized", ok,
        (L"bOk=" + std::to_wstring(bOk.load() ? 1 : 0)
            + L" aTimedOut=" + std::to_wstring(aTimedOut ? 1 : 0)
            + L" bDurMs=" + std::to_wstring(bDur)
            + L"（串行时 B 要等 A 的 1200ms ⇒ 应 >=800；不串行则只有几毫秒）").c_str());
}

void CaseOrphanGuardActiveAfterStart() {
    std::wstring self;
    if (!SelfPath(self)) {
        Emit(L"orphan_guard_active_after_start", false, L"取自身路径失败");
        return;
    }
    qst::agent::McpServerConfig cfg;
    cfg.name = L"selftest_guard";
    cfg.command = self;
    cfg.args = { L"--fake-mcp-server" };

    qst::agent::McpStdioClient client;
    std::wstring err;
    if (!client.Start(cfg, err)) {
        Emit(L"orphan_guard_active_after_start", false, (L"启动失败：" + err).c_str());
        return;
    }
    std::wstring diag;
    const bool active = client.OrphanGuardActive(diag);
    client.Stop();
    Emit(L"orphan_guard_active_after_start", active,
        (active ? std::wstring(L"guard=active") : (L"guard=INACTIVE " + diag)).c_str());
}

void CaseKillOnCloseJobKillsChild() {
    std::wstring self;
    if (!SelfPath(self)) {
        Emit(L"kill_on_close_job_kills_child", false, L"取自身路径失败");
        return;
    }
    HANDLE job = qst::agent::McpStdioClient::CreateKillOnCloseJobForTest();
    if (!job) {
        Emit(L"kill_on_close_job_kills_child", false, L"CreateJobObject 失败");
        return;
    }

    // 长命子进程 = 本 exe 的 --sleep-ms 模式（60s，远长于用例等待）。
    std::wstring cmd = L"\"" + self + L"\" --sleep-ms 60000";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // 与产品同款顺序：**先挂 Job 再放行**（CREATE_SUSPENDED 避免抢跑出不受管的孙进程）
    const BOOL spawned = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    if (!spawned) {
        CloseHandle(job);
        Emit(L"kill_on_close_job_kills_child", false, L"CreateProcess 失败");
        return;
    }

    const BOOL assigned = AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // ① 挂上之后它应该还活着（否则下面的「被杀」证明不了任何事）
    const DWORD aliveBefore = WaitForSingleObject(pi.hProcess, 1500);

    // ② 关掉 Job 句柄 —— KILL_ON_JOB_CLOSE 的语义就是「最后一个句柄没了 ⇒ 成员全杀」
    CloseHandle(job);
    const DWORD aliveAfter = WaitForSingleObject(pi.hProcess, 5000);

    const bool ok = assigned && (aliveBefore == WAIT_TIMEOUT) && (aliveAfter == WAIT_OBJECT_0);
    Emit(L"kill_on_close_job_kills_child", ok,
        (L"assigned=" + std::to_wstring(assigned ? 1 : 0)
            + L" before=" + std::to_wstring(aliveBefore)
            + L" after=" + std::to_wstring(aliveAfter)).c_str());

    if (aliveAfter == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 0);   // 兜底，别留孤儿
    CloseHandle(pi.hProcess);
}

// ── 端到端（真起子进程）──────────────────────────────────────────

bool SelfPath(std::wstring& out) {
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return false;
    out.assign(buf, n);
    return true;
}

qst::agent::McpServerConfig FakeServerConfig() {
    qst::agent::McpServerConfig cfg;
    cfg.name = L"fake";
    std::wstring self;
    SelfPath(self);
    cfg.command = self;
    cfg.args = { L"--fake-mcp-server" };
    cfg.enabled = true;
    return cfg;
}

void CaseE2eInitializeAndListTools() {
    qst::agent::McpStdioClient client;
    std::wstring err;
    if (!client.Start(FakeServerConfig(), err)) {
        Emit(L"e2e_initialize_and_list_tools", false, (L"启动失败：" + err).c_str());
        return;
    }
    const bool initOk = client.Initialize(err);
    std::vector<qst::agent::McpRemoteTool> tools;
    const bool listOk = initOk && client.ListTools(tools, err);
    const bool good = initOk && listOk && tools.size() == 3 && tools[0].name == "echo";
    Emit(L"e2e_initialize_and_list_tools", good,
        (L"init=" + std::to_wstring(initOk ? 1 : 0) + L" n="
            + std::to_wstring(tools.size()) + L" err=" + err).c_str());
    client.Stop();
}

void CaseE2eCallToolRoundtrip() {
    qst::agent::McpStdioClient client;
    std::wstring err;
    if (!client.Start(FakeServerConfig(), err) || !client.Initialize(err)) {
        Emit(L"e2e_call_tool_roundtrip", false, (L"握手失败：" + err).c_str());
        return;
    }
    std::wstring text;
    // ⚠ 自检里**不许**用产品默认的 180s 调用超时：一个 bug 就能把套件挂满 3 分钟，
    //   而且失败信息也拿不到。给 5s，超时即 FAIL 并带上诊断。
    const bool ok = client.CallTool("echo", R"({"text":"你好"})", text, err, 5000);
    const bool good = ok && text == L"echo:你好";
    Emit(L"e2e_call_tool_roundtrip", good,
        (L"ok=" + std::to_wstring(ok ? 1 : 0) + L" text=" + text + L" err=" + err
            + L" last=" + client.LastError()).c_str());
    client.Stop();
}

void CaseE2eToolErrorSurfaces() {
    qst::agent::McpStdioClient client;
    std::wstring err;
    if (!client.Start(FakeServerConfig(), err) || !client.Initialize(err)) {
        Emit(L"e2e_tool_error_surfaces", false, (L"握手失败：" + err).c_str());
        return;
    }
    std::wstring text;
    const bool ok = client.CallTool("fail", "{}", text, err, 5000);
    // 服务端报错：必须**如实上报**，不能当成成功
    const bool good = !ok && !err.empty() && text == L"boom";
    Emit(L"e2e_tool_error_surfaces", good, (L"ok=" + std::to_wstring(ok ? 1 : 0)
        + L" text=" + text + L" err=" + err).c_str());
    client.Stop();
}

void CaseE2eBadCommandFailsGracefully() {
    qst::agent::McpStdioClient client;
    qst::agent::McpServerConfig cfg;
    cfg.name = L"nope";
    cfg.command = L"qst_definitely_not_a_real_program_xyz.exe";
    std::wstring err;
    const bool started = client.Start(cfg, err);
    const bool good = !started && !err.empty() && !client.IsRunning();
    Emit(L"e2e_bad_command_fails_gracefully", good, err.c_str());
    client.Stop();
}

void CaseE2eTimeoutDoesNotHang() {
    qst::agent::McpStdioClient client;
    std::wstring err;
    if (!client.Start(FakeServerConfig(), err) || !client.Initialize(err)) {
        Emit(L"e2e_timeout_does_not_hang", false, (L"握手失败：" + err).c_str());
        return;
    }
    std::wstring text;
    const ULONGLONG t0 = GetTickCount64();
    // 假 server 对 "slow" 故意不回 → 必须超时返回，而不是永久卡住。
    // 超时覆写成 800ms：产品默认是 180s，真等满就没法进逻辑档了。
    const bool ok = client.CallTool("slow", "{}", text, err, 800);
    const ULONGLONG dt = GetTickCount64() - t0;
    // 要求：① 返回了（没卡住）② 报错了（不是假成功）③ 确实等过（不是立刻返回）
    const bool good = !ok && !err.empty() && dt >= 700 && dt < 30000;
    Emit(L"e2e_timeout_does_not_hang", good,
        (L"ok=" + std::to_wstring(ok ? 1 : 0) + L" dtMs=" + std::to_wstring(dt)
            + L" err=" + err).c_str());
    client.Stop();
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"AgentMcpSelfTest — MCP 客户端自检（协议层 + 端到端）\n"
        L"  --json              每行一个用例结果 + 末行汇总\n"
        L"  --list              列出用例名/分类/含义\n"
        L"  --fake-mcp-server   以最小 MCP stdio 服务端运行（供用例内部起子进程用）\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i] ? argv[i] : L"";
        // ★假 server 模式：必须在任何 selftest 输出之前分流，
        //   否则它往 stdout 写的 JSON-RPC 会和用例输出混在一起。
        if (a == L"--fake-mcp-server") return RunFakeMcpServer();
        // ★孤儿防护用例的「长命子进程」：本 exe 以这个模式起就只是睡够再退。
        //   用例把它挂进 KILL_ON_JOB_CLOSE 的 Job，关掉 Job 句柄后它必须**立刻**死。
        if (a == L"--sleep-ms" && i + 1 < argc) {
            const long ms = wcstol(argv[i + 1] ? argv[i + 1] : L"0", nullptr, 10);
            Sleep(ms > 0 ? static_cast<DWORD>(ms) : 0);
            return 0;
        }
        if (a == L"--json") {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--list") {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--help" || a == L"-h") {
            PrintHelp();
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"AgentMcpSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseBuildRequestLineShape();
    CaseBuildRequestLineNoBareNewline();
    CaseBuildNotificationHasNoId();
    CaseParseResultMatchedId();
    CaseParseIgnoresOtherId();
    CaseParseIgnoresNonJsonLogLine();
    CaseParseErrorResponseReported();
    CaseParseToolsList();
    CaseQualifiedNameRoundtrip();
    CaseSplitRejectsForeignName();
    CaseExtractToolTextConcatenates();
    CaseExtractToolTextFlagsNonText();
    CaseExtractToolImagePersists();
    CaseExtractToolUnknownImageMimeIsHonest();
    CaseExtractToolErrorIsError();
    CaseConfigMissingFileIsOk();
    CaseConfigBadJsonIsError();
    CaseConfigSkipsIncompleteEntries();
    CaseConfigRoundtrip();
    CaseRunnableImageByExtension();
    CaseSweepStaleMcpImages();
    CaseKillOnCloseJobKillsChild();   // 纯机制，不依赖 MCP
    CaseOrphanGuardActiveAfterStart();
    CaseConcurrentCallToolIsSerialized();

    CaseE2eInitializeAndListTools();
    CaseE2eCallToolRoundtrip();
    CaseE2eToolErrorSurfaces();
    CaseE2eBadCommandFailsGracefully();
    CaseE2eTimeoutDoesNotHang();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
