// =============================================================================
// AgentAssistantSelfTest - AI 助手增强能力自检
//   对话编辑截断 / 撤销日志 / 白名单命令 / 文件工具 / Skill 文件加载
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:AgentAssistantSelfTest
//   build\Release\AgentAssistantSelfTest.exe --json
// =============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "selftest_harness.h"

#include "agent_attachment.h"
#include "agent_ai_actions.h"
#include "agent_conversation_store.h"
#include "agent_core.h"
#include "agent_script_ops.h"
#include "agent_reference.h"
#include "agent_shell.h"
#include "agent_tools.h"
#include "agent_undo.h"
#include "agent_web.h"
#include "macro_execute_tools.h"
#include "recording_optimize_ops.h"
#include "script_io.h"
#include "action_utils.h"
#include "utils.h"

#include <opencv2/opencv.hpp>

#include <fstream>
#include <functional>
#include <cmath>
#include <string>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"truncate_history_keeps_prefix", L"default",
        L"TruncateHistoryToUserRound(1) 保留 system+第1条user"},
    {L"truncate_history_out_of_range_false", L"default",
        L"越界序号返回 false 且历史不变"},
    {L"truncate_history_skips_internal_nudge", L"default",
        L"内部引导消息不计入用户轮次序号（编辑重发不串位）"},
    {L"undo_begin_finish_list", L"default",
        L"AgentUndoBegin/Finish 后日志含 before/after"},
    {L"undo_revert_restores_content", L"default",
        L"revert 后文件内容恢复为 before"},
    {L"undo_revert_missing_file_deletes", L"default",
        L"修改前不存在的文件 revert 后被删除"},
    {L"undo_revert_twice_rejected", L"default",
        L"同一记录不能重复恢复"},
    {L"shell_reject_unknown_program", L"default",
        L"cmd/powershell 等不在白名单被拒"},
    {L"shell_reject_git_write_command", L"default",
        L"git 写子命令（commit）被拒"},
    {L"shell_where_readonly_ok", L"default",
        L"where.exe 只读命令可执行且返回 exit="},
    {L"shell_where_reject_recursive", L"default",
        L"where /R 与路径参数被拒"},
    {L"file_tools_reject_settings", L"default",
        L"readAgentFile 拒绝 app_settings.json"},
    {L"file_tools_reject_write_ui", L"default",
        L"writeAgentFile 拒绝写入仓库 ui/"},
    {L"fetch_url_blocks_rfc1918", L"default",
        L"AgentFetchUrlIsBlocked 拦截 10/172.16/127/v4mapped 且放行公网"},
    {L"file_tools_write_read_search_undo", L"default",
        L"writeAgentFile/readAgentFile/searchAgentFiles/撤销闭环"},
    {L"skill_catalog_lists_new_sections", L"default",
        L"catalog 包含 conversation/revert/shell"},
    {L"skill_file_override_loaded", L"default",
        L"AppDir()\\skills\\agent\\shell.md 覆盖内嵌 Skill"},
    {L"clipboard_roundtrip", L"optional",
        L"copyAgentTextToClipboard -> pasteAgentClipboardText"},
    {L"write_script_rejects_handwritten", L"default",
        L"writeScript 拒绝手写动作 JSON（未通过 buildScriptActions 规范化校验）"},
    {L"write_script_accepts_built_actions", L"default",
        L"writeScript 接受 buildScriptActions 生成的规范动作"},
    {L"write_script_rejects_empty_loop", L"default",
        L"writeScript 拒绝空循环（循环体写成同级）"},
    {L"read_script_emits_image_markers", L"default",
        L"readScript 输出脚本引用图片的 [[AGENT_IMG:...]] 标记"},
    {L"image_markers_and_parts", L"default",
        L"AgentExtractImageMarkers / AgentBuildImageParts 编码闭环"},
    {L"model_reasoning_type_detected", L"default",
        L"推理模型识别（deepseek-reasoner/o3 为 true，gpt-4o 为 false）"},
    {L"tool_result_budget_readscript", L"default",
        L"readScript 工具结果预算收窄到 16000 字符（防长输入卡死）"},
    {L"read_script_summary_not_raw", L"default",
        L"readScript 默认输出动作概览（动作名+备注，不含数值），不返回原始 JSON"},
    {L"read_script_raw_mode", L"default",
        L"readScript raw=true 才返回原始 JSON"},
    {L"read_script_paging", L"default",
        L"readScript 按 startIndex/maxActions 分页"},
    {L"describe_actions_covers_key_params", L"default",
        L"详细版翻译覆盖全部关键参数（缩放/限时/偏移/修饰键/容差/AI超时降级等）"},
    {L"describe_actions_quick_omits_values", L"default",
        L"概览版含语义属性（变量图/无限循环/修饰键）与完整备注，不含数值"},
    {L"recopt_merge_splits_on_keys", L"default",
        L"MergeAllKeySplit 以关键动作为分割点分别合并各段"},
    {L"recopt_merge_wait_per_segment", L"default",
        L"等待时间 first 按段取该段第一个 Wait，不串段"},
    {L"recopt_skip_relative_segment", L"default",
        L"含相对位移的段跳过，其它段照常合并"},
    {L"recopt_compress_wait_first", L"default",
        L"路径压缩 first：留下的移动点间隔取该间隔第一个 Wait"},
    {L"recopt_compress_wait_fixed", L"default",
        L"路径压缩 fixed：留下的移动点间隔使用指定等待"},
    {L"skill_optimize_directs_to_tools", L"default",
        L"optimize Skill 要求调用 optimizeRecording/optimizeScript，禁止 readScript 手改"},
    {L"skill_script_tree_and_plan", L"default",
        L"scriptStrategy Skill 要求 children 嵌套与 planScriptActions"},
    {L"conversation_draft_roundtrip", L"default",
        L"会话草稿（文本/附件/编辑态）持久化往返"},
    {L"empty_conversation_not_listed", L"default",
        L"无轮次空会话不进对话列表、不落盘"},
    {L"conversation_with_round_is_listed", L"default",
        L"有用户轮次的会话仍进入对话列表"},
    {L"conversation_title_rejects_api_error", L"default",
        L"API 错误原文不能当对话标题，加载时回退为首条用户消息"},
    {L"user_facing_tool_reply_strips_outline", L"default",
        L"终态工具回复去掉动作一览与内部约束，只留创建摘要"},
    {L"outline_header_has_no_user_constraints", L"default",
        L"动作一览标题不含对用户说明/禁止说英文等约束句"},
    {L"skill_reply_forbids_dumping_constraints", L"default",
        L"reply Skill 禁止逐步复述动作和把内部约束念给用户"},
    {L"create_macro_folder_support", L"default",
        L"createMacroScript 默认保存根目录/支持 folder，且新脚本不绑定热键"},
    {L"delete_script_via_tool", L"default",
        L"deleteScriptFile 工具删除脚本成功（文件消失 + 返回已删除）"},
    {L"build_actions_rejects_missing_required", L"default",
        L"构建器拒绝空参数（空文件路径/空按键/空输入/空找图/空等待），合法动作通过"},
    {L"loop_terminal_script_ends_immediately", L"default",
        L"createMacroScript 成功后立即收尾，不再发起第二轮请求"},
    {L"loop_repeat_guides_and_graceful_end", L"default",
        L"重复工具调用触发引导，达到轮次上限后以友好提示结束"},
    {L"tools_schema_size_bounded", L"default",
        L"工具 schema 总字节数有界（过大拖慢模型首 token）"},
    {L"script_skill_gate_enforced", L"default",
        L"未先读 scriptStrategy 时 buildScriptActions 被拒，读后放行"},
    {L"reference_read_capped", L"default",
        L"单轮 readScriptReference 超 3 次返回收束提示"},
    {L"runprogram_no_keytext_default", L"default",
        L"非按键动作（runProgram）不再残留默认键 keyText=7"},
    {L"recon_tools_capped", L"default",
        L"单轮搜索/浏览/读文件超 4 次返回收束提示"},
    {L"read_reference_system_section", L"default",
        L"readScriptReference section=system 覆盖系统动作参数（openFile/runProgram 等）"},
    {L"env_var_expansion", L"default",
        L"引擎展开 %USERPROFILE% 等环境变量，未定义变量保持原样"},
    {L"html_to_plain_text", L"default",
        L"HTML 转纯文本：去脚本/样式、实体解码、保留标题与正文"},
    {L"fetch_tool_guards", L"default",
        L"fetchWebPage 拒绝缺 url / 非 http(s) / 本地内网地址"},
    {L"ai_action_exec_has_fetch", L"default",
        L"AI 动作执行工具集包含 fetchWebPage（规划快捷键/操作时可联网查资料）"},
    {L"locate_fail_block", L"default",
        L"同一目标连续定位失败第 3 次被硬拦，换目标不误伤"},
    {L"fetch_then_runprogram_needs_confirm", L"default",
        L"fetchWebPage 之后 runProgram 无确认钩子则拒绝；未抓网页的 RPA 启动不弹"},
};

struct JournalGuard {
    std::wstring path;
    std::wstring backup;
    bool hadBackup = false;

    JournalGuard() {
        path = AgentChangeJournalPath();
        backup = path + L".selftest_bak";
        DeleteFileW(backup.c_str());
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!MoveFileW(path.c_str(), backup.c_str())) {
                if (CopyFileW(path.c_str(), backup.c_str(), FALSE)) hadBackup = true;
                DeleteFileW(path.c_str());
            } else {
                hadBackup = true;
            }
        }
    }
    ~JournalGuard() {
        DeleteFileW(path.c_str());
        if (hadBackup) MoveFileW(backup.c_str(), path.c_str());
        DeleteFileW(backup.c_str());
    }
};

std::wstring TestFilePath(const wchar_t* name) {
    return AppDir() + L"\\__agent_selftest_" + name;
}

void WriteUtf8File(const std::wstring& path, const std::wstring& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    const std::string bytes = ToUtf8(content);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// ── 本地 mock API 服务器（验证工具循环治理）────────────────────────
// 仅监听 127.0.0.1 随机端口，按请求序号返回预设的 chat/completions 响应。
struct MockCompletionsServer {
    SOCKET listenSock = INVALID_SOCKET;
    std::vector<std::string> bodies;  // 序号不足时重复最后一个
    std::atomic<int> requests{0};
    std::atomic<bool> running{false};
    std::thread worker;
    int port = 0;

    explicit MockCompletionsServer(std::vector<std::string> resp)
        : bodies(std::move(resp)) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR
            && listen(listenSock, 4) != SOCKET_ERROR) {
            int len = sizeof(addr);
            getsockname(listenSock, reinterpret_cast<sockaddr*>(&addr), &len);
            port = ntohs(addr.sin_port);
            running = true;
            worker = std::thread([this] { Serve(); });
        }
    }

    ~MockCompletionsServer() {
        running = false;
        if (listenSock != INVALID_SOCKET) {
            closesocket(listenSock);
            listenSock = INVALID_SOCKET;
        }
        if (worker.joinable()) worker.join();
        WSACleanup();
    }

    void Serve() {
        while (running) {
            SOCKET c = accept(listenSock, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            const int idx = requests.fetch_add(1);
            const std::string& body = bodies.empty() ? std::string()
                : bodies[static_cast<size_t>(idx) < bodies.size()
                    ? static_cast<size_t>(idx) : bodies.size() - 1];
            char buf[4096];
            std::string req;
            for (;;) {
                const int n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
                const size_t hdrEnd = req.find("\r\n\r\n");
                if (hdrEnd != std::string::npos) {
                    size_t cl = 0;
                    const size_t p = req.find("Content-Length:");
                    if (p != std::string::npos) {
                        const size_t e = req.find("\r\n", p);
                        if (e != std::string::npos) {
                            cl = static_cast<size_t>(std::atoll(
                                req.substr(p + 15, e - p - 15).c_str()));
                        }
                    }
                    while (req.size() < hdrEnd + 4 + cl) {
                        const int m = recv(c, buf, sizeof(buf), 0);
                        if (m <= 0) break;
                        req.append(buf, static_cast<size_t>(m));
                    }
                    break;
                }
            }
            std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size())
                + "\r\nConnection: close\r\n\r\n" + body;
            send(c, resp.data(), static_cast<int>(resp.size()), 0);
            closesocket(c);
        }
    }
};

std::string MockToolCallResponse(const char* toolName, const std::string& argsJson) {
    nlohmann::json tc = {
        {"id", "call_mock_1"},
        {"type", "function"},
        {"function", {{"name", toolName}, {"arguments", argsJson}}}
    };
    nlohmann::json message = {
        {"role", "assistant"},
        {"content", nullptr},
        {"tool_calls", nlohmann::json::array({tc})}
    };
    nlohmann::json body = {
        {"choices", nlohmann::json::array({
            {{"index", 0}, {"message", message}, {"finish_reason", "tool_calls"}}
        })},
        {"usage", {{"prompt_tokens", 5}, {"completion_tokens", 5}, {"total_tokens", 10}}}
    };
    return body.dump();
}

std::string MockTextResponse(const char* text) {
    nlohmann::json body = {
        {"choices", nlohmann::json::array({
            {{"index", 0},
             {"message", {{"role", "assistant"}, {"content", text}}},
             {"finish_reason", "stop"}}
        })},
        {"usage", {{"prompt_tokens", 5}, {"completion_tokens", 5}, {"total_tokens", 10}}}
    };
    return body.dump();
}

AgentCore MakeTestCore() {
    AgentConfig cfg;
    cfg.apiUrl = L"http://127.0.0.1:1/v1/chat/completions";
    cfg.apiKey = L"x";
    cfg.model = L"test";
    return AgentCore(cfg, L"system", {});
}

AgentCore MakeMockCore(int port, const std::vector<AgentTool>& tools) {
    AgentConfig cfg;
    cfg.apiUrl = L"http://127.0.0.1:" + std::to_wstring(port)
        + L"/v1/chat/completions";
    cfg.apiKey = L"x";
    cfg.model = L"test";
    return AgentCore(cfg, L"system", tools);
}

void CaseTruncatePrefix() {
    AgentCore core = MakeTestCore();
    std::vector<ChatMessage> history;
    ChatMessage sys; sys.role = L"system"; sys.content = L"s";
    ChatMessage u1; u1.role = L"user"; u1.content = L"a";
    ChatMessage a1; a1.role = L"assistant"; a1.content = L"b";
    ChatMessage u2; u2.role = L"user"; u2.content = L"c";
    history.push_back(sys); history.push_back(u1);
    history.push_back(a1); history.push_back(u2);
    core.SetFullHistory(std::move(history));
    const bool ok = core.TruncateHistoryToUserRound(1);
    const auto& h = core.GetHistory();
    const bool valid = ok && h.size() == 3
        && h[0].role == L"system" && h[1].role == L"user" && h[1].content == L"a"
        && h[2].role == L"assistant" && h[2].content == L"b";
    Emit(L"truncate_history_keeps_prefix", valid,
        ok ? L"" : L"TruncateHistoryToUserRound 返回 false");
}

void CaseTruncateOutOfRange() {
    AgentCore core = MakeTestCore();
    std::vector<ChatMessage> history;
    ChatMessage sys; sys.role = L"system"; sys.content = L"s";
    ChatMessage u1; u1.role = L"user"; u1.content = L"a";
    history.push_back(sys); history.push_back(u1);
    core.SetFullHistory(std::move(history));
    const bool before = core.TruncateHistoryToUserRound(0);
    const bool after = core.TruncateHistoryToUserRound(5);
    Emit(L"truncate_history_out_of_range_false",
        before && !after && core.GetHistory().size() == 1,
        L"期望首条截断成功、越界返回 false");
}

void CaseTruncateSkipsInternalNudge() {
    AgentCore core = MakeTestCore();
    std::vector<ChatMessage> history;
    ChatMessage sys; sys.role = L"system"; sys.content = L"s";
    ChatMessage u1; u1.role = L"user"; u1.content = L"第一问";
    ChatMessage a1; a1.role = L"assistant"; a1.content = L"答1";
    ChatMessage nudge; nudge.role = L"user"; nudge.internal_nudge = true;
    nudge.content = L"注意：重复调用工具";
    ChatMessage a2; a2.role = L"assistant"; a2.content = L"答2";
    ChatMessage u2; u2.role = L"user"; u2.content = L"第二问";
    history.push_back(sys); history.push_back(u1); history.push_back(a1);
    history.push_back(nudge); history.push_back(a2); history.push_back(u2);
    core.SetFullHistory(std::move(history));
    // 回退到第 1 条用户消息（第二问）之前：nudge 不算用户轮次
    const bool ok = core.TruncateHistoryToUserRound(1);
    const auto& h = core.GetHistory();
    const bool valid = ok && h.size() == 5
        && h[0].role == L"system"
        && h[1].content == L"第一问"
        && h[2].content == L"答1"
        && h[3].internal_nudge
        && h[4].content == L"答2";
    Emit(L"truncate_history_skips_internal_nudge", valid,
        ok ? (valid ? L"" : L"截断位置错误（nudge 被当成用户轮次）")
           : L"TruncateHistoryToUserRound 返回 false");
}

void CaseUndoRoundtrip(JournalGuard&) {
    const std::wstring path = TestFilePath(L"undo1.txt");
    DeleteFileW(path.c_str());
    WriteUtf8File(path, L"before-content");
    const std::wstring id = AgentUndoBegin(L"writeScript", L"自检写入",
        path, L"before-content", true);
    WriteUtf8File(path, L"after-content");
    AgentUndoFinish(id, L"after-content", true);

    std::vector<AgentChangeEntry> list;
    LoadAgentChanges(list);
    bool found = false;
    for (const auto& e : list) {
        if (e.id == id) {
            found = e.before == L"before-content" && e.after == L"after-content"
                && e.ok && !e.reverted && e.path == path;
            break;
        }
    }
    DeleteFileW(path.c_str());
    Emit(L"undo_begin_finish_list", found && !id.empty(),
        found ? L"" : L"日志记录缺失或字段不符");
}

void CaseRevertRestores(JournalGuard&) {
    const std::wstring path = TestFilePath(L"undo2.txt");
    DeleteFileW(path.c_str());
    WriteUtf8File(path, L"old");
    const std::wstring id = AgentUndoBegin(L"updateSettings", L"自检恢复",
        path, L"old", true);
    WriteUtf8File(path, L"new");
    AgentUndoFinish(id, L"new", true);
    std::wstring err;
    const bool ok = RevertAgentChange(id, err);
    const bool content = ReadAll(path) == L"old";
    DeleteFileW(path.c_str());
    Emit(L"undo_revert_restores_content", ok && content && err.empty(),
        ok && content ? L"" : (L"revert 后内容未恢复：" + err).c_str());
}

void CaseRevertMissingFile(JournalGuard&) {
    const std::wstring path = TestFilePath(L"undo3.txt");
    DeleteFileW(path.c_str());
    const std::wstring id = AgentUndoBegin(L"deleteScriptFile", L"自检删除",
        path, L"", false);
    WriteUtf8File(path, L"created-after-begin");
    AgentUndoFinish(id, L"created-after-begin", true);
    std::wstring err;
    const bool ok = RevertAgentChange(id, err);
    const bool gone = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    Emit(L"undo_revert_missing_file_deletes", ok && gone && err.empty(),
        ok && gone ? L"" : (L"删除失败：" + err).c_str());
}

void CaseRevertTwice(JournalGuard&) {
    const std::wstring path = TestFilePath(L"undo4.txt");
    DeleteFileW(path.c_str());
    WriteUtf8File(path, L"v1");
    const std::wstring id = AgentUndoBegin(L"writeAgentFile", L"自检重复", path, L"v1", true);
    WriteUtf8File(path, L"v2");
    AgentUndoFinish(id, L"v2", true);
    std::wstring err;
    const bool first = RevertAgentChange(id, err);
    const bool second = !RevertAgentChange(id, err);
    DeleteFileW(path.c_str());
    Emit(L"undo_revert_twice_rejected", first && second,
        first ? L"" : L"第一次恢复失败或重复恢复未拦截");
}

void CaseShellRejectUnknown() {
    const AgentTool tool = MakeRunAgentCommandTool();
    const std::wstring r = tool.execute(LR"({"command":"cmd /c echo hi"})");
    Emit(L"shell_reject_unknown_program",
        r.find(L"不在白名单") != std::wstring::npos, r.c_str());
}

void CaseShellRejectGitWrite() {
    const AgentTool tool = MakeRunAgentCommandTool();
    const std::wstring r = tool.execute(LR"({"command":"git commit -m x"})");
    Emit(L"shell_reject_git_write_command",
        r.find(L"只读") != std::wstring::npos, r.c_str());
}

void CaseShellWhereOk() {
    const AgentTool tool = MakeRunAgentCommandTool();
    const std::wstring r = tool.execute(LR"({"command":"where.exe cmd"})");
    Emit(L"shell_where_readonly_ok",
        r.find(L"exit=") != std::wstring::npos
            && r.find(L"[错误]") == std::wstring::npos, r.c_str());
}

void CaseShellWhereRejectRecursive() {
    const AgentTool tool = MakeRunAgentCommandTool();
    const std::wstring r = tool.execute(LR"({"command":"where /R C:\\ *.pem"})");
    Emit(L"shell_where_reject_recursive",
        r.find(L"[错误]") != std::wstring::npos, r.c_str());
}

void CaseFileRejectSettings() {
    const AgentTool reader = MakeReadAgentFileTool();
    const std::wstring r = reader.execute(LR"({"path":"app_settings.json"})");
    Emit(L"file_tools_reject_settings",
        r.find(L"受保护") != std::wstring::npos
            || r.find(L"[错误]") != std::wstring::npos, r.c_str());
}

void CaseFileRejectWriteUi() {
    std::wstring dir = AppDir();
    std::wstring repo;
    for (int up = 0; up < 6; ++up) {
        if (GetFileAttributesW((dir + L"\\CMakeLists.txt").c_str()) != INVALID_FILE_ATTRIBUTES
            && GetFileAttributesW((dir + L"\\ui").c_str()) != INVALID_FILE_ATTRIBUTES) {
            repo = dir;
            break;
        }
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        dir = dir.substr(0, slash);
    }
    if (repo.empty()) {
        Emit(L"file_tools_reject_write_ui", true, L"no repo ui (skipped)");
        return;
    }
    const std::wstring path = repo + L"\\ui\\__agent_selftest_ui.txt";
    std::wstring json = L"{\"path\":\"";
    for (wchar_t c : path) {
        if (c == L'\\') json += L"\\\\";
        else json.push_back(c);
    }
    json += L"\",\"content\":\"x\"}";
    const AgentTool writer = MakeWriteAgentFileTool();
    const std::wstring r = writer.execute(json);
    const bool rejected = r.find(L"[错误]") != std::wstring::npos;
    DeleteFileW(path.c_str());
    Emit(L"file_tools_reject_write_ui", rejected, r.c_str());
}

void CaseFetchUrlBlocksRfc1918() {
    std::wstring err;
    const bool ten = AgentFetchUrlIsBlocked(L"http://10.1.2.3/secret", err);
    const bool priv = AgentFetchUrlIsBlocked(L"http://172.16.0.9/", err);
    const bool loop = AgentFetchUrlIsBlocked(L"http://127.0.0.1/status", err);
    const bool mapped = AgentFetchUrlIsBlocked(L"http://[::ffff:10.1.2.3]/x", err);
    std::wstring okErr;
    const bool publicOk = !AgentFetchUrlIsBlocked(L"https://example.com/a", okErr);
    Emit(L"fetch_url_blocks_rfc1918", ten && priv && loop && publicOk && mapped,
        publicOk && mapped ? L"" : (mapped ? okErr.c_str() : L"v4mapped 未拦"));
}

void CaseFileTools(JournalGuard&) {
    const std::wstring rel = L"scripts\\__agent_selftest_file.txt";
    const std::wstring abs = ScriptsDir() + L"\\__agent_selftest_file.txt";
    DeleteFileW(abs.c_str());

    const AgentTool writer = MakeWriteAgentFileTool();
    const std::wstring w = writer.execute(
        LR"({"path":"scripts/__agent_selftest_file.txt","content":"QST_AGENT_SELFTEST_MARKER_12345\nline2"})");
    const bool wrote = w.find(L"已写入") != std::wstring::npos;

    const AgentTool reader = MakeReadAgentFileTool();
    const std::wstring rd = reader.execute(LR"({"path":"scripts/__agent_selftest_file.txt"})");
    const bool readOk = rd.find(L"QST_AGENT_SELFTEST_MARKER_12345") != std::wstring::npos;

    const AgentTool searcher = MakeSearchAgentFilesTool();
    const std::wstring sr = searcher.execute(
        LR"({"path":"scripts","pattern":"QST_AGENT_SELFTEST_MARKER_12345","glob":"*.txt"})");
    const bool searchOk = sr.find(L"QST_AGENT_SELFTEST_MARKER_12345") != std::wstring::npos;

    // 撤销闭环：找到该变更并恢复（文件恢复为不存在）
    std::vector<AgentChangeEntry> list;
    LoadAgentChanges(list);
    std::wstring changeId;
    for (const auto& e : list) {
        if (e.path == abs && e.tool == L"writeAgentFile" && !e.reverted) {
            changeId = e.id;
            break;
        }
    }
    std::wstring err;
    const bool reverted = !changeId.empty() && RevertAgentChange(changeId, err);
    const bool gone = GetFileAttributesW(abs.c_str()) == INVALID_FILE_ATTRIBUTES;
    DeleteFileW(abs.c_str());
    std::wstring detail;
    if (!wrote) detail += L"写失败: " + w + L" ";
    if (!readOk) detail += L"读失败: " + rd + L" ";
    if (!searchOk) detail += L"搜失败: " + sr + L" ";
    if (!(reverted && gone)) detail += L"撤销失败 " + err;
    Emit(L"file_tools_write_read_search_undo",
        wrote && readOk && searchOk && reverted && gone, detail.c_str());
}

void CaseSkillCatalog() {
    const std::wstring catalog = AgentSkillGet(L"catalog");
    const bool ok = catalog.find(L"conversation") != std::wstring::npos
        && catalog.find(L"revert") != std::wstring::npos
        && catalog.find(L"shell") != std::wstring::npos;
    Emit(L"skill_catalog_lists_new_sections", ok, L"catalog 缺新 section");
}

void CaseSkillFileOverride() {
    const std::wstring dir = AppDir() + L"\\skills\\agent";
    CreateDirectoryW((AppDir() + L"\\skills").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\selftest_override.md";
    WriteUtf8File(path, L"OVERRIDE_SHELL_SKILL_CONTENT");
    const std::wstring got = AgentSkillGet(L"selftest_override");
    DeleteFileW(path.c_str());
    Emit(L"skill_file_override_loaded",
        got.find(L"OVERRIDE_SHELL_SKILL_CONTENT") != std::wstring::npos,
        (L"got=" + got.substr(0, 120)).c_str());
}

void CaseClipboard() {
    const std::wstring marker = L"QST_CLIP_SELFTEST_α";
    // 剪贴板是全局资源：先保存原文本，测试结束后恢复，
    // 绝不把自检标记残留在用户剪贴板（否则下次粘贴会粘出 QST_CLIP_SELFTEST_α）。
    std::wstring originalText;
    bool hadText = false;
    if (OpenClipboard(nullptr)) {
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    originalText = p;
                    hadText = true;
                    GlobalUnlock(h);
                }
            }
        }
        CloseClipboard();
    }
    const AgentTool copier = MakeCopyAgentTextToClipboardTool();
    const AgentTool paster = MakePasteAgentClipboardTextTool();
    const std::wstring c = copier.execute(LR"({"text":"QST_CLIP_SELFTEST_α"})");
    const std::wstring p = paster.execute(L"{}");
    const bool ok = c.find(L"已复制") != std::wstring::npos
        && p.find(marker) != std::wstring::npos;
    // 恢复原剪贴板（尽力而为）：有原文本则写回，没有则清空，绝不留标记
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        if (hadText) {
            const size_t bytes = (originalText.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                if (void* dst = GlobalLock(hMem)) {
                    CopyMemory(dst, originalText.c_str(), bytes);
                    GlobalUnlock(hMem);
                    if (!SetClipboardData(CF_UNICODETEXT, hMem)) GlobalFree(hMem);
                } else {
                    GlobalFree(hMem);
                }
            }
        }
        CloseClipboard();
    }
    Emit(L"clipboard_roundtrip", ok,
        ok ? L"" : (c + L" | " + p).c_str());
}

void CaseWriteScriptRejectsHandwritten() {
    const AgentTool tool = MakeWriteScriptTool();
    nlohmann::json j;
    j["fileName"] = "__agent_bad.json";
    j["dir"] = "scripts";
    j["content"] =
        "{\"scriptName\":\"bad\",\"actions\":[{\"type\":\"findImage\",\"imagePath\":\"\"}]}";
    const std::wstring r = tool.execute(FromUtf8(j.dump()));
    const bool rejected = r.find(L"buildScriptActions") != std::wstring::npos
        || r.find(L"规范化") != std::wstring::npos;
    const std::wstring path = ScriptsDir() + L"\\__agent_bad.json";
    const bool notSaved = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    DeleteFileW(path.c_str());
    Emit(L"write_script_rejects_handwritten", rejected && notSaved,
        ((rejected ? L"" : L"手写动作未被拒绝: ") + r.substr(0, 200)).c_str());
}

void CaseWriteScriptAcceptsBuilt() {
    // 用 buildScriptActions 生成规范动作，再整体保存，应成功
    const AgentTool builder = MakeBuildScriptActionsTool();
    nlohmann::json action;
    action["type"] = "findImage";
    action["imagePath"] = "images\\selftest_agent.png";
    action["findImageFollowUp"] = 2;
    action["matchVarName"] = "hit";
    nlohmann::json params = nlohmann::json::object();
    params["actions"] = nlohmann::json::array({action});
    const std::wstring built = builder.execute(FromUtf8(params.dump()));
    const bool builtOk = built.find(L"已构建") != std::wstring::npos;

    // 从 built 文本中截取 actions 数组，组装为完整脚本 content
    const size_t lb = built.find(L'[');
    const size_t rb = built.rfind(L']');
    const std::wstring actionsJson =
        lb != std::wstring::npos && rb != std::wstring::npos && rb > lb
            ? built.substr(lb, rb - lb + 1) : L"";
    std::wstring content = L"{\"scriptName\":\"__agent_built\",\"actions\":"
        + actionsJson + L"}";
    nlohmann::json save;
    save["fileName"] = "__agent_built.json";
    save["dir"] = "scripts";
    save["content"] = ToUtf8(content);
    const AgentTool writer = MakeWriteScriptTool();
    const std::wstring r = writer.execute(FromUtf8(save.dump()));
    const std::wstring path = ScriptsDir() + L"\\__agent_built.json";
    const bool saved = r.find(L"已保存") != std::wstring::npos
        && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    DeleteFileW(path.c_str());
    Emit(L"write_script_accepts_built_actions", builtOk && saved,
        ((builtOk ? L"" : L"构建失败 ")
            + (saved ? L"" : (L"保存失败: " + r.substr(0, 200)))).c_str());
}

void CaseWriteScriptRejectsEmptyLoop() {
    const AgentTool tool = MakeWriteScriptTool();
    nlohmann::json j;
    j["fileName"] = "__agent_empty_loop.json";
    j["dir"] = "scripts";
    j["content"] =
        "{\"scriptName\":\"badloop\",\"actions\":["
        "{\"type\":\"loop\",\"loopCount\":-1,\"indent\":0},"
        "{\"type\":\"wait\",\"duration\":1,\"indent\":0}"
        "]}";
    const std::wstring r = tool.execute(FromUtf8(j.dump()));
    const std::wstring path = ScriptsDir() + L"\\__agent_empty_loop.json";
    const bool notSaved = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    DeleteFileW(path.c_str());
    const bool rejected = r.find(L"没有子动作") != std::wstring::npos
        || r.find(L"children") != std::wstring::npos;
    Emit(L"write_script_rejects_empty_loop", rejected && notSaved,
        ((rejected ? L"" : L"空循环未被拒绝: ") + r.substr(0, 200)).c_str());
}

void CaseReadScriptImageMarkers(JournalGuard&) {
    const std::wstring scriptPath = ScriptsDir() + L"\\__agent_imgtest.json";
    const std::wstring imgPath = FindImagesDir() + L"\\selftest_agent.png";
    CreateDirectoryW(ScriptsDir().c_str(), nullptr);
    CreateDirectoryW(FindImagesDir().c_str(), nullptr);
    DeleteFileW(scriptPath.c_str());
    DeleteFileW(imgPath.c_str());
    cv::Mat img(4, 4, CV_8UC3, cv::Scalar(0, 0, 255));
    cv::imwrite(ToUtf8(imgPath), img);
    WriteUtf8File(scriptPath,
        L"{\"scriptName\":\"imgtest\",\"actions\":[{\"type\":\"findImage\","
        L"\"imagePath\":\"images\\selftest_agent.png\",\"findImageFollowUp\":0}]}");
    const AgentTool tool = MakeReadScriptTool();
    const std::wstring r = tool.execute(
        LR"({"fileName":"__agent_imgtest.json","dir":"scripts"})");
    const bool ok = r.find(L"[[AGENT_IMG:") != std::wstring::npos
        && r.find(imgPath) != std::wstring::npos;
    DeleteFileW(scriptPath.c_str());
    DeleteFileW(imgPath.c_str());
    Emit(L"read_script_emits_image_markers", ok,
        ok ? L"" : r.substr(0, 240).c_str());
}

void CaseImageMarkersAndParts() {
    std::wstring text = L"a [[AGENT_IMG:D:\\x\\a.png]] b [[AGENT_IMG:D:\\y\\b.png]] c";
    std::wstring out;
    std::vector<std::wstring> paths;
    const bool extracted = AgentExtractImageMarkers(text, out, paths);
    const bool ok1 = extracted && paths.size() == 2
        && out.find(L"[[AGENT_IMG:") == std::wstring::npos
        && out == L"a  b  c";

    const std::wstring imgPath = AppDir() + L"\\__agent_selftest_part.png";
    DeleteFileW(imgPath.c_str());
    cv::Mat img(4, 4, CV_8UC3, cv::Scalar(0, 255, 0));
    cv::imwrite(ToUtf8(imgPath), img);
    std::vector<ChatContentPart> parts;
    std::wstring skipped;
    const bool built = AgentBuildImageParts({imgPath}, parts, skipped, 2);
    const bool ok2 = built && parts.size() == 1
        && parts[0].type == L"image_url"
        && parts[0].image_url.rfind(L"data:image/", 0) == 0;
    DeleteFileW(imgPath.c_str());
    Emit(L"image_markers_and_parts", ok1 && ok2,
        ((ok1 ? L"" : L"标记提取失败 ") + (ok2 ? L"" : skipped)).c_str());
}

void CaseModelReasoningType() {
    const bool ok = ModelIsReasoningType(L"deepseek-reasoner")
        && ModelIsReasoningType(L"o3-mini")
        && !ModelIsReasoningType(L"gpt-4o")
        && !ModelIsReasoningType(L"deepseek-chat");
    Emit(L"model_reasoning_type_detected", ok, L"推理模型识别不符预期");
}

void CaseToolResultBudget() {
    Emit(L"tool_result_budget_readscript",
        AiToolResultBudgetChars(L"readScript") == 16000,
        std::to_wstring(AiToolResultBudgetChars(L"readScript")).c_str());
}

void CaseReadScriptSummary() {
    const std::wstring scriptPath = ScriptsDir() + L"\\__agent_summary.json";
    DeleteFileW(scriptPath.c_str());
    WriteUtf8File(scriptPath,
        L"{\"scriptName\":\"summary\",\"actions\":["
        L"{\"type\":\"findImage\",\"imagePath\":\"images\\a.png\",\"findImageFollowUp\":0,"
        L"\"matchThreshold\":80,\"searchX1\":10,\"searchY1\":20,\"searchX2\":100,\"searchY2\":200,"
        L"\"remark\":\"检查登录按钮\"},"
        L"{\"type\":\"wait\",\"duration\":2.5,\"remark\":\"等待加载\"},"
        L"{\"type\":\"keyClick\",\"keyText\":\"F5\",\"clickCount\":3,\"duration\":0.2,"
        L"\"remark\":\"刷新页面\"}"
        L"]}");
    const AgentTool tool = MakeReadScriptTool();
    const std::wstring r = tool.execute(
        LR"({"fileName":"__agent_summary.json","dir":"scripts"})");
    // 概览：动作名 + 备注（意图关键），不含数值参数与原始 JSON 字段
    const bool hasSummary = r.find(L"动作概览") != std::wstring::npos
        && r.find(L"找图后点击") != std::wstring::npos
        && r.find(L"检查登录按钮") != std::wstring::npos
        && r.find(L"等待加载") != std::wstring::npos
        && r.find(L"按键点击") != std::wstring::npos
        && r.find(L"刷新页面") != std::wstring::npos;
    const bool noRaw = r.find(L"\"searchX1\"") == std::wstring::npos
        && r.find(L"\"imageScale\"") == std::wstring::npos
        && r.find(L"\"matchThreshold\"") == std::wstring::npos;
    const bool noValues = r.find(L"F5") == std::wstring::npos
        && r.find(L"2.5") == std::wstring::npos
        && r.find(L"×3") == std::wstring::npos;
    DeleteFileW(scriptPath.c_str());
    Emit(L"read_script_summary_not_raw", hasSummary && noRaw && noValues,
        (std::wstring(hasSummary ? L"" : L"缺概览/备注 ")
            + (noRaw ? L"" : L"仍含原始字段 ")
            + (noValues ? L"" : L"仍含数值 ") + r.substr(0, 200)).c_str());
}

void CaseReadScriptRaw() {
    const std::wstring scriptPath = ScriptsDir() + L"\\__agent_raw.json";
    DeleteFileW(scriptPath.c_str());
    WriteUtf8File(scriptPath,
        L"{\"scriptName\":\"rawtest\",\"actions\":[{\"type\":\"wait\",\"duration\":1}]}");
    const AgentTool tool = MakeReadScriptTool();
    const std::wstring r = tool.execute(
        LR"({"fileName":"__agent_raw.json","dir":"scripts","raw":true})");
    const bool ok = r.find(L"\"scriptName\"") != std::wstring::npos
        && r.find(L"rawtest") != std::wstring::npos;
    DeleteFileW(scriptPath.c_str());
    Emit(L"read_script_raw_mode", ok, ok ? L"" : r.substr(0, 200).c_str());
}

void CaseReadScriptPaging() {
    const std::wstring scriptPath = ScriptsDir() + L"\\__agent_page.json";
    DeleteFileW(scriptPath.c_str());
    std::wstring actions;
    for (int i = 0; i < 70; ++i) {
        if (i) actions += L",";
        actions += L"{\"type\":\"wait\",\"duration\":0.1}";
    }
    // 声明 inputTimingVersion=2（显式 wait），避免旧文件迁移逻辑合并连续 wait
    WriteUtf8File(scriptPath,
        L"{\"scriptName\":\"page\",\"inputTimingVersion\":2,\"actions\":["
        + actions + L"]}");
    const ScriptFileData dbg = LoadScriptFileData(scriptPath);
    const std::wstring dbgDetail = L"parsed=" + std::to_wstring(dbg.actions.size());
    const AgentTool tool = MakeReadScriptTool();
    const std::wstring page1 = tool.execute(
        LR"({"fileName":"__agent_page.json","dir":"scripts"})");
    const bool page1Ok = page1.find(L"第1步") != std::wstring::npos
        && page1.find(L"startIndex=60") != std::wstring::npos;
    const std::wstring page2 = tool.execute(
        LR"({"fileName":"__agent_page.json","dir":"scripts","startIndex":60,"maxActions":10})");
    const bool page2Ok = page2.find(L"第61步") != std::wstring::npos
        && page2.find(L"第70步") != std::wstring::npos;
    DeleteFileW(scriptPath.c_str());
    Emit(L"read_script_paging", page1Ok && page2Ok && dbg.actions.size() == 70,
        (std::wstring(page1Ok ? L"" : L"首页分页提示缺失 ")
            + (page2Ok ? L"" : L"第二页缺失 ") + dbgDetail + L" " + page2.substr(0, 200)).c_str());
}

void CaseDescribeCoversParams() {
    std::vector<ScriptAction> acts;
    auto push = [&](ActionType type, std::function<void(ScriptAction&)> fill) {
        ScriptAction a;
        a.type = type;
        fill(a);
        acts.push_back(std::move(a));
    };
    push(ActionType::FindImage, [](ScriptAction& a) {
        a.imagePath = L"images\\btn.png";
        a.matchThreshold = 80;
        a.remark = L"检查登录按钮";
        a.searchFullScreen = false;
        a.searchX1 = 1; a.searchY1 = 2; a.searchX2 = 100; a.searchY2 = 200;
        a.findImageFollowUp = 0;
        a.offsetX = 5; a.offsetY = 6;
        a.findTimeExpr = L"30";
        a.imageScaleMin = 0.9; a.imageScaleMax = 1.1;
    });
    push(ActionType::KeyClick, [](ScriptAction& a) {
        a.keyText = L"F5";
        a.holdLeftCtrl = true;
        a.holdLeftShift = true;
        a.clickCount = 3;
        a.duration = 0.2;
    });
    push(ActionType::FindColor, [](ScriptAction& a) {
        a.colorR = 255; a.colorG = 0; a.colorB = 0;
        a.colorTolerance = 32;
        a.searchFullScreen = false;
        a.searchX1 = 0; a.searchY1 = 0; a.searchX2 = 10; a.searchY2 = 10;
        a.findImageFollowUp = 2;
        a.matchVarName = L"col";
    });
    push(ActionType::ColorMatch, [](ScriptAction& a) {
        a.colorR = 0; a.colorG = 128; a.colorB = 255;
        a.x = 12; a.y = 34;
        a.matchVarName = L"cm";
    });
    push(ActionType::AiImageAnalysis, [](ScriptAction& a) {
        a.aiPrompt = L"描述画面";
        a.aiOutputVarName = L"out";
        a.aiTimeoutSec = 60;
        a.aiFallbackValue = L"x";
        a.aiRegionByImage = true;
        a.aiTargetImagePath = L"images\\anchor.png";
    });
    push(ActionType::AiActionExecute, [](ScriptAction& a) {
        a.aiPrompt = L"自动操作";
        a.aiWithImage = true;
        a.aiMaxSteps = 5;
        a.aiLogicConvert = true;
    });
    push(ActionType::TimerRecordTime, [](ScriptAction& a) {
        a.loopVarName = L"t";
    });
    push(ActionType::GetColor, [](ScriptAction& a) {
        a.x = 7; a.y = 8;
        a.matchVarName = L"c";
    });
    push(ActionType::QuickInput, [](ScriptAction& a) {
        a.inputText = L"hello";
        a.charInterval = 0.05;
    });
    push(ActionType::RunMacro, [](ScriptAction& a) {
        a.targetPath = L"scripts\\sub\\other.json";
    });
    push(ActionType::WatchImage, [](ScriptAction& a) {
        a.imagePath = L"images\\w.bmp";
        a.watchMode = 1;
        a.watchPollSeconds = 2.5;
        a.resumeAfterWatch = true;
    });

    size_t shown = 0;
    const std::wstring s = DescribeScriptActionsDetail(acts, 0, 60, shown);
    const bool ok = shown == acts.size()
        && s.find(L"区域") != std::wstring::npos
        && s.find(L"限时30s") != std::wstring::npos
        && s.find(L"缩放0.9-1.1") != std::wstring::npos
        && s.find(L"偏移") != std::wstring::npos
        && s.find(L"Ctrl+Shift+") != std::wstring::npos
        && s.find(L"按键点击 F5") != std::wstring::npos
        && s.find(L"×3") != std::wstring::npos
        && s.find(L"间隔0.2s") != std::wstring::npos
        && s.find(L"找色 ") != std::wstring::npos
        && s.find(L"#FF0000") != std::wstring::npos
        && s.find(L"容差32") != std::wstring::npos
        && s.find(L"保存到变量col") != std::wstring::npos
        && s.find(L"颜色匹配") != std::wstring::npos
        && s.find(L"AI 图片分析") != std::wstring::npos
        && s.find(L"图:images") != std::wstring::npos
        && s.find(L"超时60s") != std::wstring::npos
        && s.find(L"失败降级") != std::wstring::npos
        && s.find(L"按锚点图") != std::wstring::npos
        && s.find(L"AI 执行动作") != std::wstring::npos
        && s.find(L"带截图") != std::wstring::npos
        && s.find(L"最多5步") != std::wstring::npos
        && s.find(L"逻辑转化") != std::wstring::npos
        && s.find(L"计时器记录时间 → t") != std::wstring::npos
        && s.find(L"获取颜色") != std::wstring::npos
        && s.find(L"→ c") != std::wstring::npos
        && s.find(L"输入 \"hello\"") != std::wstring::npos
        && s.find(L"间隔0.05s") != std::wstring::npos
        && s.find(L"运行宏 scripts") != std::wstring::npos
        && s.find(L"找图监视") != std::wstring::npos
        && s.find(L"时间监视") != std::wstring::npos;
    Emit(L"describe_actions_covers_key_params", ok,
        ok ? L"" : s.substr(0, 400).c_str());
}

void CaseDescribeQuickOmitsValues() {
    std::vector<ScriptAction> acts;
    auto push = [&](ActionType type, std::function<void(ScriptAction&)> fill) {
        ScriptAction a;
        a.type = type;
        fill(a);
        acts.push_back(std::move(a));
    };
    push(ActionType::FindImage, [](ScriptAction& a) {
        a.imagePath = L"images\\btn.png";
        a.matchThreshold = 80;
        a.searchX1 = 1; a.searchY1 = 2; a.searchX2 = 100; a.searchY2 = 200;
        a.findTimeExpr = L"30";
        a.imageUseVar = true;
        a.remark = L"检查登录按钮";
    });
    push(ActionType::KeyClick, [](ScriptAction& a) {
        a.keyText = L"F5";
        a.holdLeftCtrl = true;
        a.clickCount = 3;
        a.duration = 0.2;
    });
    push(ActionType::Wait, [](ScriptAction& a) {
        a.duration = 2.5;
    });
    push(ActionType::FindColor, [](ScriptAction& a) {
        a.colorR = 255; a.colorG = 0; a.colorB = 0;
        a.colorTolerance = 32;
    });
    std::wstring longRemark;
    for (int i = 0; i < 40; ++i)
        longRemark += L"这是一条很长的备注，用于验证概览不截断备注内容。";
    push(ActionType::Loop, [&](ScriptAction& a) {
        a.loopCount = -1;
        a.remark = longRemark;
    });

    size_t shown = 0;
    const std::wstring s = DescribeScriptActionsBrief(acts, 0, 60, shown);
    const bool hasBrief = s.find(L"动作概览") != std::wstring::npos
        && s.find(L"找图（变量图）后点击") != std::wstring::npos
        && s.find(L"检查登录按钮") != std::wstring::npos
        && s.find(L"Ctrl+按键点击") != std::wstring::npos
        && s.find(L"等待") != std::wstring::npos
        && s.find(L"区域找色") != std::wstring::npos
        && s.find(L"无限循环") != std::wstring::npos
        && s.find(longRemark) != std::wstring::npos;
    const bool noValues = s.find(L"F5") == std::wstring::npos
        && s.find(L"2.5") == std::wstring::npos
        && s.find(L"#FF0000") == std::wstring::npos
        && s.find(L"容差") == std::wstring::npos
        && s.find(L"限时") == std::wstring::npos
        && s.find(L"×3") == std::wstring::npos;
    Emit(L"describe_actions_quick_omits_values", hasBrief && noValues,
        (std::wstring(hasBrief ? L"" : L"缺概览/备注 ")
            + (noValues ? L"" : L"概览仍含数值 ") + s.substr(0, 300)).c_str());
}

void CaseConversationDraftRoundtrip() {
    const std::wstring id = L"selftest_" + TimestampName();
    const std::wstring draft = L"未发送的草稿内容 α";
    const std::vector<std::wstring> atts = {L"C:\\pics\\a.png", L"D:\\x\\b.jpg"};
    const bool saved = SaveAgentConversationDraft(id, draft, atts, 2);
    AgentConversationRecord rec;
    const bool loaded = LoadAgentConversationRecord(id, rec);
    std::vector<AgentConversationMeta> listed;
    LoadAgentConversationList(listed);
    bool inList = false;
    for (const auto& m : listed) {
        if (m.id == id) inList = true;
    }
    const bool ok = saved && loaded
        && rec.draft == draft
        && rec.attachmentPaths.size() == 2
        && rec.attachmentPaths[0] == atts[0]
        && rec.attachmentPaths[1] == atts[1]
        && rec.editIndex == 2
        && !inList;
    DeleteAgentConversation(id);
    Emit(L"conversation_draft_roundtrip", ok,
        ok ? L"" : (std::wstring(saved ? L"" : L"保存失败 ")
            + (loaded ? L"" : L"读取失败 ")
            + (inList ? L"误进列表 " : L"")).c_str());
}

void CaseEmptyConversationNotListed() {
    const std::wstring id = L"selftest_empty_" + TimestampName();
    const bool saved = SaveAgentConversationDraft(id, L"", {}, -1);
    AgentConversationRecord rec;
    const bool fileExists = LoadAgentConversationRecord(id, rec);
    std::vector<AgentConversationMeta> listed;
    LoadAgentConversationList(listed);
    bool inList = false;
    for (const auto& m : listed) {
        if (m.id == id) inList = true;
    }
    DeleteAgentConversation(id);
    const bool ok = saved && !fileExists && !inList;
    Emit(L"empty_conversation_not_listed", ok,
        ok ? L"" : (std::wstring(saved ? L"" : L"空草稿应视为成功 ")
            + (fileExists ? L"不应落盘 " : L"")
            + (inList ? L"误进列表 " : L"")).c_str());
}

void CaseConversationWithRoundIsListed() {
    AgentConversationSavePayload payload;
    payload.shouldSave = true;
    payload.id = L"selftest_round_" + TimestampName();
    payload.name = L"有对话";
    payload.createdTime = L"2026-08-31 00:00:00";
    ChatMessage user;
    user.role = L"user";
    user.content = L"hello";
    payload.messages.push_back(user);
    const bool saved = SaveAgentConversation(payload);
    std::vector<AgentConversationMeta> listed;
    LoadAgentConversationList(listed);
    bool inList = false;
    int rounds = 0;
    for (const auto& m : listed) {
        if (m.id == payload.id) {
            inList = true;
            rounds = m.roundCount;
        }
    }
    DeleteAgentConversation(payload.id);
    const bool ok = saved && inList && rounds >= 1;
    Emit(L"conversation_with_round_is_listed", ok,
        ok ? L"" : (std::wstring(saved ? L"" : L"保存失败 ")
            + (inList ? L"" : L"未进列表 ")
            + (rounds >= 1 ? L"" : L"轮次未写入 ")).c_str());
}

void CaseConversationTitleRejectsApiError() {
    const bool rejectErr = !IsUsableConversationTitle(L"[错误] API 请求失败：读取")
        && !IsUsableConversationTitle(L"API请求失败")
        && !IsUsableConversationTitle(L"新对话")
        && !IsUsableConversationTitle(L"")
        && IsUsableConversationTitle(L"自动按QWER");
    AgentConversationSavePayload payload;
    payload.shouldSave = true;
    payload.id = L"selftest_title_" + TimestampName();
    payload.name = L"[错误] API 请求失败：读取";
    payload.createdTime = L"2026-09-12 00:00:00";
    ChatMessage user;
    user.role = L"user";
    user.content = L"帮我生成一个脚本，自动按QWER";
    payload.messages.push_back(user);
    const bool saved = SaveAgentConversation(payload);
    AgentConversationRecord rec;
    const bool loaded = LoadAgentConversationRecord(payload.id, rec);
    const bool repaired = loaded && IsUsableConversationTitle(rec.meta.name)
        && rec.meta.name.find(L"API") == std::wstring::npos
        && rec.meta.name.find(L"错误") == std::wstring::npos;
    DeleteAgentConversation(payload.id);
    Emit(L"conversation_title_rejects_api_error", rejectErr && saved && repaired,
        (std::wstring(rejectErr ? L"" : L"判定未拒错误标题 ")
            + (saved ? L"" : L"保存失败 ")
            + (repaired ? L"" : (L"加载未回退: " + rec.meta.name))).c_str());
}

void CaseUserFacingToolReplyStripsOutline() {
    const std::wstring raw =
        L"✓ 鼠标宏已创建：qwer.json\n  名称: 自动按QWER\n\n"
        L"【动作一览 — 缩进表示父子（循环/条件体内）；对用户说明时必须用下列名称（与编辑器动作列一致），禁止说英文 type】\n"
        L"第1步 按键点击Q\n";
    const std::wstring facing = AgentUserFacingToolReply(raw);
    const std::wstring modern =
        L"✓ 鼠标宏已创建：a.json\n  动作数: 2\n\n动作一览（缩进=循环/条件体内）：\n第1步 等待\n";
    const std::wstring facing2 = AgentUserFacingToolReply(modern);
    const bool ok = facing.find(L"已创建") != std::wstring::npos
        && facing.find(L"自动按QWER") != std::wstring::npos
        && facing.find(L"动作一览") == std::wstring::npos
        && facing.find(L"禁止说英文") == std::wstring::npos
        && facing.find(L"对用户说明") == std::wstring::npos
        && facing.find(L"第1步") == std::wstring::npos
        && facing2.find(L"已创建") != std::wstring::npos
        && facing2.find(L"动作一览") == std::wstring::npos
        && facing2.find(L"第1步") == std::wstring::npos;
    Emit(L"user_facing_tool_reply_strips_outline", ok,
        ok ? L"" : (L"facing=" + facing.substr(0, 160)).c_str());
}

void CaseOutlineHeaderHasNoUserConstraints() {
    std::vector<ScriptAction> acts(1);
    acts[0].type = ActionType::Wait;
    acts[0].duration = 1;
    acts[0].remark = L"等一秒";
    const std::wstring s = FormatScriptActionsOutline(acts);
    const bool ok = s.find(L"对用户说明") == std::wstring::npos
        && s.find(L"禁止说英文") == std::wstring::npos
        && s.find(L"必须用") == std::wstring::npos
        && s.find(L"等一秒") != std::wstring::npos;
    Emit(L"outline_header_has_no_user_constraints", ok,
        ok ? L"" : s.substr(0, 160).c_str());
}

void CaseSkillReplyForbidsDumping() {
    const std::wstring t = AgentSkillGet(L"reply");
    const bool ok = t.find(L"一两句") != std::wstring::npos
        && t.find(L"内部约束") != std::wstring::npos;
    Emit(L"skill_reply_forbids_dumping_constraints", ok,
        ok ? L"" : t.substr(0, 160).c_str());
}

void CaseCreateMacroFolder(JournalGuard&) {
    nlohmann::json action;
    action["type"] = "wait";
    action["duration"] = 1;
    const nlohmann::json acts = nlohmann::json::array({action});
    const std::wstring name = L"__agent_folder_test.json";

    nlohmann::json params;
    const auto r1 = AgentCreateMacroScript(name, L"t", acts, params);
    const std::wstring rootPath = ScriptsDir() + L"\\" + name;
    const bool rootOk = GetFileAttributesW(rootPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    const ScriptFileData loaded = LoadScriptFileData(rootPath);
    const bool noHotkey = loaded.hotkey.vk == 0
        && !loaded.hotkey.enabled
        && loaded.hotkey.text.empty();
    DeleteFileW(rootPath.c_str());

    params["folder"] = "__agent_sub";
    const auto r2 = AgentCreateMacroScript(name, L"t", acts, params);
    const std::wstring subPath = ScriptsDir() + L"\\__agent_sub\\" + name;
    const bool subOk =
        GetFileAttributesW(subPath.c_str()) != INVALID_FILE_ATTRIBUTES
        && r2.message.find(L"scripts\\__agent_sub\\") != std::wstring::npos;
    DeleteFileW(subPath.c_str());
    RemoveDirectoryW((ScriptsDir() + L"\\__agent_sub").c_str());
    Emit(L"create_macro_folder_support", rootOk && noHotkey && subOk,
        (std::wstring(rootOk ? L"" : L"根目录保存失败 ")
            + (noHotkey ? L"" : L"新脚本仍带热键 ")
            + (subOk ? L"" : r2.message.substr(0, 160))).c_str());
}

void CaseDeleteScriptViaTool(JournalGuard&) {
    const std::wstring name = L"__agent_del_test.json";
    const std::wstring path = ScriptsDir() + L"\\" + name;
    DeleteFileW(path.c_str());
    const auto created = AgentSaveScriptContent(name,
        L"{\"scriptName\":\"del\",\"actions\":[{\"type\":\"wait\",\"duration\":1}]}",
        L"scripts");
    const bool existed = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const auto r = AgentDeleteScriptFile(name, L"scripts");
    const bool gone = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    const bool msgOk = r.message.find(L"已删除") != std::wstring::npos;
    DeleteFileW(path.c_str());
    Emit(L"delete_script_via_tool", existed && gone && msgOk,
        (std::wstring(existed ? L"" : L"创建失败 ")
            + (gone ? L"" : L"删除后文件仍存在 ")
            + (msgOk ? L"" : r.message.substr(0, 160))).c_str());
}

void CaseBuildActionsRequired() {
    const AgentTool tool = MakeBuildScriptActionsTool();
    auto run = [&](const nlohmann::json& action) -> std::wstring {
        nlohmann::json p;
        p["actions"] = nlohmann::json::array({action});
        return tool.execute(FromUtf8(p.dump()));
    };
    const bool openFileRej =
        run(nlohmann::json{{"type", "openFile"}}).find(L"targetPath") != std::wstring::npos;
    const bool keyRej =
        run(nlohmann::json{{"type", "keyClick"}}).find(L"keyText") != std::wstring::npos;
    const bool quickRej =
        run(nlohmann::json{{"type", "quickInput"}, {"inputText", ""}})
            .find(L"inputText") != std::wstring::npos;
    const bool findRej =
        run(nlohmann::json{{"type", "findImage"}}).find(L"imagePath") != std::wstring::npos;
    const bool waitRej =
        run(nlohmann::json{{"type", "wait"}}).find(L"duration") != std::wstring::npos;
    const bool ifRej =
        run(nlohmann::json{{"type", "if"}}).find(L"conditionExpr") != std::wstring::npos;
    const bool okBuilt =
        run(nlohmann::json{{"type", "keyClick"}, {"keyText", "Enter"}})
                .find(L"已构建") != std::wstring::npos
        && run(nlohmann::json{{"type", "openFile"}, {"targetPath", "C:\\t.txt"}})
                .find(L"已构建") != std::wstring::npos
        && run(nlohmann::json{{"type", "quickInput"}, {"inputText", "hello"}})
                .find(L"已构建") != std::wstring::npos
        && run(nlohmann::json{{"type", "findImage"}, {"imagePath", "images\\a.png"}})
                .find(L"已构建") != std::wstring::npos
        && run(nlohmann::json{{"type", "wait"}, {"duration", 1}})
                .find(L"已构建") != std::wstring::npos;
    Emit(L"build_actions_rejects_missing_required",
        openFileRej && keyRej && quickRej && findRej && waitRej && ifRej && okBuilt,
        (std::wstring(openFileRej ? L"" : L"openFile 未拒 ")
            + (keyRej ? L"" : L"keyClick 未拒 ")
            + (quickRej ? L"" : L"quickInput 未拒 ")
            + (findRej ? L"" : L"findImage 未拒 ")
            + (waitRej ? L"" : L"wait 未拒 ")
            + (ifRej ? L"" : L"if 未拒 ")
            + (okBuilt ? L"" : L"合法动作被误拒 ")).c_str());
}

void CaseLoopTerminalScriptEndsImmediately() {
    // 门禁要求先读 scriptStrategy；服务器先返回 readAgentSkill，再返回
    // createMacroScript。创建成功后应直接收尾，不再发起下一轮请求。
    nlohmann::json skillArgs = {{"section", "scriptStrategy"}};
    nlohmann::json args = {
        {"fileName", "__agent_loop_terminal.json"},
        {"scriptName", "自检终态"},
        {"actions", nlohmann::json::array({
            {{"type", "wait"}, {"duration", 1}}
        })}
    };
    MockCompletionsServer server({
        MockToolCallResponse("readAgentSkill", skillArgs.dump()),
        MockToolCallResponse("createMacroScript", args.dump())
    });
    if (server.port == 0) {
        Emit(L"loop_terminal_script_ends_immediately", false, L"mock 服务器启动失败");
        return;
    }
    AgentCore core = MakeMockCore(server.port,
        {MakeReadAgentSkillTool(), MakeCreateMacroScriptTool()});
    AgentSendCallbacks cb;
    cb.preferNonStream = true;
    ChatMessage msg;
    msg.role = L"user";
    msg.content = L"创建一个测试脚本";
    const std::wstring reply = core.SendMessage(msg, cb);
    const std::wstring path = ScriptsDir() + L"\\__agent_loop_terminal.json";
    const bool created = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool twoRounds = server.requests.load() == 2;
    const bool replyOk = reply.find(L"已创建") != std::wstring::npos
        && reply.find(L"[错误]") == std::wstring::npos
        && reply.find(L"动作一览") == std::wstring::npos
        && reply.find(L"禁止说英文") == std::wstring::npos;
    bool historyHasVisible = false;
    for (const auto& m : core.GetHistory()) {
        if (m.role == L"assistant" && m.content.find(L"已创建") != std::wstring::npos
            && m.content.find(L"动作一览") == std::wstring::npos) {
            historyHasVisible = true;
            break;
        }
    }
    DeleteFileW(path.c_str());
    Emit(L"loop_terminal_script_ends_immediately", created && twoRounds && replyOk && historyHasVisible,
        ((created ? L"" : L"脚本未创建 ")
            + (twoRounds ? L"" : (L"请求数=" + std::to_wstring(server.requests.load()) + L" "))
            + (replyOk ? L"" : (L"回复异常: " + reply.substr(0, 120)))
            + (historyHasVisible ? L"" : L"历史缺可见回复 ")).c_str());
}

void CaseLoopRepeatGuidesAndGracefulEnd() {
    // 模型每轮都用相同参数调用同一工具（listScripts）→ 应触发重复引导、
    // 接近上限提示，并在 10 轮后以友好提示结束（不再返回粗硬错误）。
    MockCompletionsServer server({MockToolCallResponse("listScripts", "{}")});
    if (server.port == 0) {
        Emit(L"loop_repeat_guides_and_graceful_end", false, L"mock 服务器启动失败");
        return;
    }
    AgentCore core = MakeMockCore(server.port, {MakeListScriptsTool()});
    AgentSendCallbacks cb;
    cb.preferNonStream = true;
    ChatMessage msg;
    msg.role = L"user";
    msg.content = L"列出脚本";
    const std::wstring reply = core.SendMessage(msg, cb);
    bool hasNag = false;
    bool hasNearLimit = false;
    for (const auto& h : core.GetHistory()) {
        if (h.role == L"user" && h.content.find(L"相同结果") != std::wstring::npos)
            hasNag = true;
        if (h.role == L"user" && h.content.find(L"接近本轮工具调用上限") != std::wstring::npos)
            hasNearLimit = true;
    }
    const bool ok = server.requests.load() == 14
        && reply.find(L"达到上限") != std::wstring::npos
        && reply.find(L"[错误]") == std::wstring::npos
        && hasNag && hasNearLimit;
    Emit(L"loop_repeat_guides_and_graceful_end", ok,
        ((server.requests.load() == 14
                ? L"" : (L"请求数=" + std::to_wstring(server.requests.load()) + L" "))
            + (reply.find(L"达到上限") != std::wstring::npos ? L"" : L"回复未提示上限 ")
            + (reply.find(L"[错误]") == std::wstring::npos ? L"" : L"回复仍为错误 ")
            + (hasNag ? L"" : L"无重复引导 ")
            + (hasNearLimit ? L"" : L"无接近上限提示 ")
            + L"| reply: " + reply.substr(0, 100)).c_str());
}

void CaseScriptSkillGateEnforced() {
    // 未先读 scriptStrategy 就调用 buildScriptActions → 工具层拒绝并提示先读 Skill；
    // 读完后再构建才放行（防止模型逐条翻参考绕圈）。
    nlohmann::json buildArgs = {
        {"actions", nlohmann::json::array({
            {{"type", "wait"}, {"duration", 1}}
        })}
    };
    nlohmann::json skillArgs = {{"section", "scriptStrategy"}};
    MockCompletionsServer server({
        MockToolCallResponse("buildScriptActions", buildArgs.dump()),
        MockToolCallResponse("readAgentSkill", skillArgs.dump()),
        MockToolCallResponse("buildScriptActions", buildArgs.dump()),
        MockTextResponse("完成")
    });
    if (server.port == 0) {
        Emit(L"script_skill_gate_enforced", false, L"mock 服务器启动失败");
        return;
    }
    AgentCore core = MakeMockCore(server.port,
        {MakeBuildScriptActionsTool(), MakeReadAgentSkillTool()});
    AgentSendCallbacks cb;
    cb.preferNonStream = true;
    ChatMessage msg;
    msg.role = L"user";
    msg.content = L"创建一个测试脚本";
    const std::wstring reply = core.SendMessage(msg, cb);
    bool gateFired = false;
    bool buildOk = false;
    for (const auto& h : core.GetHistory()) {
        if (h.role == L"tool") {
            if (h.content.find(L"readAgentSkill section=scriptStrategy") != std::wstring::npos)
                gateFired = true;
            if (h.content.find(L"已构建") != std::wstring::npos)
                buildOk = true;
        }
    }
    const bool ok = gateFired && buildOk
        && reply.find(L"完成") != std::wstring::npos;
    std::wstring detail;
    if (!gateFired) detail += L"门禁未触发 ";
    if (!buildOk) detail += L"读 Skill 后构建仍被拒 ";
    if (reply.find(L"完成") == std::wstring::npos)
        detail += L"回复异常: " + reply.substr(0, 100);
    Emit(L"script_skill_gate_enforced", ok, detail.c_str());
}

void CaseReferenceReadCapped() {
    // 单轮内 readScriptReference 超过 3 次 → 第 4 次直接返回收束提示
    //（不同 section 的翻阅不会触发“同名同结果”重复检测，需单独兜底）。
    MockCompletionsServer server({
        MockToolCallResponse("readScriptReference", "{}"),
        MockToolCallResponse("readScriptReference", "{}"),
        MockToolCallResponse("readScriptReference", "{}"),
        MockToolCallResponse("readScriptReference", "{}"),
        MockTextResponse("结束")
    });
    if (server.port == 0) {
        Emit(L"reference_read_capped", false, L"mock 服务器启动失败");
        return;
    }
    AgentCore core = MakeMockCore(server.port, {MakeReadScriptReferenceTool()});
    AgentSendCallbacks cb;
    cb.preferNonStream = true;
    ChatMessage msg;
    msg.role = L"user";
    msg.content = L"查一下参考";
    const std::wstring reply = core.SendMessage(msg, cb);
    bool capped = false;
    for (const auto& h : core.GetHistory()) {
        if (h.role == L"tool"
            && h.content.find(L"已多次翻阅脚本参考") != std::wstring::npos) {
            capped = true;
            break;
        }
    }
    std::wstring detail;
    if (!capped) detail += L"第 4 次翻阅未被拦截 ";
    if (reply.find(L"结束") == std::wstring::npos)
        detail += L"回复异常: " + reply.substr(0, 100);
    Emit(L"reference_read_capped", capped && reply.find(L"结束") != std::wstring::npos,
        detail.c_str());
}

void CaseRunProgramNoKeyTextDefault() {
    // 非按键动作不应残留默认键 "7"（历史 bug：runProgram 被填 keyText="7"/keyVk=55）。
    const AgentTool builder = MakeBuildScriptActionsTool();
    const std::wstring r = builder.execute(FromUtf8(nlohmann::json{{"actions", nlohmann::json::array({
        {{"type", "runProgram"}, {"targetPath", "powershell.exe"}, {"inputText", "echo 1"}}
    })}}.dump()));
    const size_t lb = r.find(L'[');
    const size_t rb = r.rfind(L']');
    const std::wstring arr = (lb != std::wstring::npos && rb > lb && rb != std::wstring::npos)
        ? r.substr(lb, rb - lb + 1) : L"";
    const bool noSeven = arr.find(L"\"keyText\": \"7\"") == std::wstring::npos
        && arr.find(L"keyText\":\"7\"") == std::wstring::npos;
    std::wstring detail;
    if (arr.empty()) detail = L"未取到动作数组";
    else if (!noSeven) detail = L"runProgram 仍含 keyText=7";
    Emit(L"runprogram_no_keytext_default", !arr.empty() && noSeven, detail.c_str());
}

void CaseReconToolsCapped() {
    // 单轮内 searchAgentFiles/listDirectory/readAgentFile 合计超 4 次 →
    // 第 5 次起返回收束提示，防止模型把轮次全花在找文件上。
    nlohmann::json args = {
        {"path", "scripts"},
        {"pattern", "__agent_recon_nonexistent_xyz__"}
    };
    MockCompletionsServer server({
        MockToolCallResponse("searchAgentFiles", args.dump()),
        MockToolCallResponse("searchAgentFiles", args.dump()),
        MockToolCallResponse("searchAgentFiles", args.dump()),
        MockToolCallResponse("searchAgentFiles", args.dump()),
        MockToolCallResponse("searchAgentFiles", args.dump()),
        MockToolCallResponse("searchAgentFiles", args.dump()),
        MockTextResponse("结束")
    });
    if (server.port == 0) {
        Emit(L"recon_tools_capped", false, L"mock 服务器启动失败");
        return;
    }
    AgentCore core = MakeMockCore(server.port, {MakeSearchAgentFilesTool()});
    AgentSendCallbacks cb;
    cb.preferNonStream = true;
    ChatMessage msg;
    msg.role = L"user";
    msg.content = L"搜索文件";
    const std::wstring reply = core.SendMessage(msg, cb);
    bool capped = false;
    for (const auto& h : core.GetHistory()) {
        if (h.role == L"tool"
            && h.content.find(L"已多次浏览/搜索/读取文件") != std::wstring::npos) {
            capped = true;
            break;
        }
    }
    std::wstring detail;
    if (!capped) detail = L"第 5 次搜索未被拦截";
    if (reply.find(L"结束") == std::wstring::npos)
        detail += L" 回复异常: " + reply.substr(0, 100);
    Emit(L"recon_tools_capped", capped && reply.find(L"结束") != std::wstring::npos,
        detail.c_str());
}

void CaseReadReferenceSystemSection() {
    // 系统动作参数必须能在 section=system 一次查到，
    // 避免模型因找不到 openFile/runProgram 参数而反复翻阅其它 section。
    const AgentTool ref = MakeReadScriptReferenceTool();
    const std::wstring r = ref.execute(FromUtf8(
        nlohmann::json{{"section", "system"}}.dump()));
    const bool hasOpenFile = r.find(L"openFile") != std::wstring::npos
        && r.find(L"targetPath") != std::wstring::npos;
    const bool hasRunProgram = r.find(L"runProgram") != std::wstring::npos
        && r.find(L"inputText") != std::wstring::npos;
    const bool hasActivate = r.find(L"activateWindow") != std::wstring::npos;
    const bool hasTimer = r.find(L"timerRecordTime") != std::wstring::npos;
    // 路径与等待规范：避免 AI 猜 Public 桌面、用 0.5s 短等待
    const bool hasPathRule = r.find(L"USERPROFILE") != std::wstring::npos
        && r.find(L"Public") != std::wstring::npos;
    const bool hasWaitRule = r.find(L"3 秒以上") != std::wstring::npos;
    std::wstring detail;
    if (!hasOpenFile) detail += L"缺 openFile/targetPath ";
    if (!hasRunProgram) detail += L"缺 runProgram/inputText ";
    if (!hasActivate) detail += L"缺 activateWindow ";
    if (!hasTimer) detail += L"缺 timerRecordTime ";
    if (!hasPathRule) detail += L"缺路径规范 ";
    if (!hasWaitRule) detail += L"缺等待规范 ";
    Emit(L"read_reference_system_section",
        hasOpenFile && hasRunProgram && hasActivate && hasTimer
            && hasPathRule && hasWaitRule,
        detail.c_str());
}

void CaseEnvVarExpansion() {
    // 引擎展开 %环境变量%：脚本里写 %USERPROFILE%\Desktop\test.txt 应展开为真实用户目录
    const std::wstring home = ExpandEnvironmentVars(L"%USERPROFILE%");
    const std::wstring expanded = ExpandEnvironmentVars(
        L"%USERPROFILE%\\Desktop\\test.txt");
    const bool ok = !home.empty()
        && home.find(L'%') == std::wstring::npos
        && expanded.find(home) == 0
        && expanded.find(L"\\Desktop\\test.txt") != std::wstring::npos;
    // 未定义变量保持原样，避免误伤
    const std::wstring unknown = ExpandEnvironmentVars(
        L"C:\\%NO_SUCH_VAR_XYZ_12345%\\a.txt");
    const bool keepUnknown = unknown.find(L"%NO_SUCH_VAR_XYZ_12345%") != std::wstring::npos;
    std::wstring detail;
    if (!ok) detail = L"展开异常: " + home + L" -> " + expanded;
    if (!keepUnknown) detail += L" 未定义变量被改写";
    Emit(L"env_var_expansion", ok && keepUnknown, detail.c_str());
}

void CaseHtmlToPlainText() {
    const std::wstring html =
        L"<html><head><title>测试页面</title><style>a{color:red}</style></head>"
        L"<body><h1>标题一</h1><p>第一段&amp;内容</p>"
        L"<script>var x=1;</script><ul><li>项目1</li><li>项目2</li></ul></body></html>";
    const std::wstring html2 =
        L"<html><head><title>带属性样式</title>"
        L"<style data-for=\"result\" type=\"text/css\" >body{font-size:14px}</style >"
        L"<p>正文内容</p></head></html>";
    const std::wstring text = HtmlToPlainText(html);
    const std::wstring text2 = HtmlToPlainText(html2);
    const bool ok = text.find(L"测试页面") != std::wstring::npos
        && text.find(L"标题一") != std::wstring::npos
        && text.find(L"第一段&内容") != std::wstring::npos
        && text.find(L"var x=1") == std::wstring::npos
        && text.find(L"项目1") != std::wstring::npos
        && text.find(L"项目2") != std::wstring::npos
        && text.find(L"color:red") == std::wstring::npos
        && text2.find(L"font-size:14px") == std::wstring::npos
        && text2.find(L"正文内容") != std::wstring::npos;
    Emit(L"html_to_plain_text", ok,
        ok ? L"" : (L"text=" + text.substr(0, 200)
            + L" | text2=" + text2.substr(0, 200)).c_str());
}

void CaseFetchToolGuards() {
    const AgentTool tool = MakeFetchWebPageTool();
    const std::wstring r1 = tool.execute(L"{}");
    const bool missing = r1.find(L"缺少 url") != std::wstring::npos;
    const std::wstring r2 = tool.execute(LR"({"url":"ftp://example.com/a"})");
    const bool scheme = r2.find(L"仅支持 http/https") != std::wstring::npos;
    const std::wstring r3 = tool.execute(LR"({"url":"http://localhost:8080/x"})");
    const bool local = r3.find(L"禁止抓取本地") != std::wstring::npos;
    const std::wstring r4 = tool.execute(LR"({"url":"http://192.168.1.5/x"})");
    const bool lan = r4.find(L"禁止抓取本地") != std::wstring::npos;
    std::wstring detail;
    if (!missing) detail += L"缺 url 未拒 ";
    if (!scheme) detail += L"非 http(s) 未拒 ";
    if (!local) detail += L"localhost 未拒 ";
    if (!lan) detail += L"内网地址未拒 ";
    Emit(L"fetch_tool_guards", missing && scheme && local && lan, detail.c_str());
}

void CaseAiActionExecHasFetch() {
    AiActionToolOptions opts;
    const auto tools = BuildAiActionExecuteTools(nullptr, opts);
    bool hasFetch = false;
    for (const auto& t : tools) {
        if (t.name == L"fetchWebPage") hasFetch = true;
    }
    Emit(L"ai_action_exec_has_fetch", hasFetch,
        hasFetch ? L"" : L"AI 动作执行工具集缺少 fetchWebPage");
}

void CaseLocateFailBlock() {
    ResetAiActionSessionState();
    AiActionHostHooks hooks;
    hooks.onLocateAndClick = [](const std::wstring&, int, const std::wstring&, int)
        -> std::wstring {
        return L"[错误] 未找到目标（识图返回 NOT_FOUND）";
    };
    AiActionToolOptions opts;
    const auto tools = BuildAiActionExecuteTools(&hooks, opts);
    const AgentTool* locate = nullptr;
    for (const auto& t : tools) {
        if (t.name == L"locateAndClick") locate = &t;
    }
    if (!locate) {
        Emit(L"locate_fail_block", false, L"工具集中缺少 locateAndClick");
        return;
    }
    const std::wstring r1 = locate->execute(LR"({"target":"消息输入框"})");
    const bool fail1 = r1.find(L"未找到目标") != std::wstring::npos;
    const std::wstring r2 = locate->execute(LR"({"target":"消息输入框"})");
    const bool fail2 = r2.find(L"未找到目标") != std::wstring::npos;
    // 第 3 次同一目标：硬拦截，不再调用宿主
    const std::wstring r3 = locate->execute(LR"({"target":"消息输入框"})");
    const bool blocked = r3.find(L"已连续") != std::wstring::npos
        && r3.find(L"禁止继续换词重试") != std::wstring::npos;
    // 换一个完全不同目标：不误伤（继续走宿主 → 失败）
    const std::wstring r4 = locate->execute(LR"({"target":"发送按钮"})");
    const bool different = r4.find(L"已连续") == std::wstring::npos
        && r4.find(L"未找到目标") != std::wstring::npos;
    std::wstring detail;
    if (!fail1) detail += L"第1次未被放行 ";
    if (!fail2) detail += L"第2次未被放行 ";
    if (!blocked) detail += L"第3次同目标未拦截: " + r3.substr(0, 80);
    if (!different) detail += L"换目标被误拦: " + r4.substr(0, 80);
    Emit(L"locate_fail_block", fail1 && fail2 && blocked && different, detail.c_str());
    ResetAiActionSessionState();
}

void CaseFetchThenRunProgramNeedsConfirm() {
    ResetAiActionSessionState();
    AiActionToolOptions opts;
    const auto tools = BuildAiActionExecuteTools(nullptr, opts);
    const AgentTool* run = nullptr;
    for (const auto& t : tools) {
        if (t.name == L"runProgram") run = &t;
    }
    if (!run) {
        Emit(L"fetch_then_runprogram_needs_confirm", false, L"缺少 runProgram");
        return;
    }
    const std::wstring before = run->execute(LR"({"targetPath":"notepad"})");
    const bool rpaOk = before.find(L"刚抓取过网页") == std::wstring::npos;
    AiNoteWebFetchUntrusted();
    const std::wstring after = run->execute(LR"({"targetPath":"notepad"})");
    const bool denied = after.find(L"刚抓取过网页") != std::wstring::npos
        && after.find(L"确认框") != std::wstring::npos;
    std::wstring detail;
    if (!rpaOk) detail += L"未抓网页也被拦: " + before.substr(0, 80) + L" ";
    if (!denied) detail += L"抓网页后未要求确认: " + after.substr(0, 80);
    Emit(L"fetch_then_runprogram_needs_confirm", rpaOk && denied, detail.c_str());
    ResetAiActionSessionState();
}

void CaseToolsSchemaSize() {
    // 按 BuildRequest 的组装方式精确估算请求体中 tools 数组的 UTF-8 字节数：
    // 过大（几十 KB）会显著拖慢模型首 token，是「卡死」的候选根因之一。
    const auto tools = BuildDefaultAgentTools();
    nlohmann::json toolsArray = nlohmann::json::array();
    for (const auto& tool : tools) {
        nlohmann::json fn;
        fn["name"] = ToUtf8(tool.name);
        fn["description"] = ToUtf8(tool.description);
        if (!tool.parameters_json.empty()) {
            try {
                fn["parameters"] = nlohmann::json::parse(ToUtf8(tool.parameters_json));
            } catch (...) {
                fn["parameters"] = nlohmann::json::parse("{}");
            }
        }
        toolsArray.push_back(
            {{"type", "function"}, {"function", fn}});
    }
    const size_t bytes = toolsArray.dump().size();
    const bool ok = bytes < 100 * 1024;
    Emit(L"tools_schema_size_bounded", ok,
        (L"tools=" + std::to_wstring(tools.size()) + L" schema_bytes="
            + std::to_wstring(bytes)).c_str());
}

ScriptAction TestMove(int x, int y) {
    ScriptAction a{};
    a.type = ActionType::MoveMouse;
    a.x = x;
    a.y = y;
    return a;
}

ScriptAction TestWait(double sec) {
    ScriptAction a{};
    a.type = ActionType::Wait;
    a.duration = sec;
    if (sec > 0.0) {
        const long double us = static_cast<long double>(sec) * 1000000.0L;
        a.timingUs = static_cast<uint64_t>(std::llround(us));
    }
    return a;
}

ScriptAction TestKey() {
    ScriptAction a{};
    a.type = ActionType::MouseDown;
    return a;
}

ScriptAction TestRel(int dx, int dy) {
    ScriptAction a{};
    a.type = ActionType::MoveMouseRelative;
    a.x = dx;
    a.y = dy;
    return a;
}

void CaseRecoptMergeSplitsOnKeys() {
    // 1~3 移动, 4 关键, 5~6 移动, 7 关键, 8 移动, 9 关键
    std::vector<ScriptAction> acts = {
        TestMove(0, 0), TestMove(1, 1), TestMove(2, 2),
        TestKey(),
        TestMove(3, 3), TestMove(4, 4),
        TestKey(),
        TestMove(5, 5),
        TestKey()
    };
    const auto r = recopt::MergeAllKeySplit(acts, "sum", 0.0);
    int keys = 0, moves = 0;
    for (const auto& a : acts) {
        if (a.type == ActionType::MouseDown) ++keys;
        if (a.type == ActionType::MoveMouse) ++moves;
    }
    const bool ok = r.collectOk && r.applied == 3 && keys == 3 && moves == 3
        && acts.size() == 6;  // 3 次合并后的 move + 3 关键
    Emit(L"recopt_merge_splits_on_keys", ok,
        ok ? L"" : (L"applied=" + std::to_wstring(r.applied)
            + L" n=" + std::to_wstring(acts.size())
            + L" moves=" + std::to_wstring(moves)).c_str());
}

void CaseRecoptMergeWaitPerSegment() {
    // 段1: wait 0.10 + move + wait 0.30 + move → first=0.10 + last move
    // 关键
    // 段2: wait 0.50 + move → first=0.50 + move
    std::vector<ScriptAction> acts = {
        TestWait(0.10), TestMove(1, 1), TestWait(0.30), TestMove(2, 2),
        TestKey(),
        TestWait(0.50), TestMove(9, 9)
    };
    const auto r = recopt::MergeAllKeySplit(acts, "first", 0.0);
    std::vector<double> waits;
    for (const auto& a : acts) {
        if (a.type == ActionType::Wait) waits.push_back(a.duration);
    }
    const bool ok = r.collectOk && r.applied == 2 && waits.size() == 2
        && std::abs(waits[0] - 0.10) < 0.0005
        && std::abs(waits[1] - 0.50) < 0.0005;
    Emit(L"recopt_merge_wait_per_segment", ok,
        ok ? L"" : (L"applied=" + std::to_wstring(r.applied)
            + L" waits=" + std::to_wstring(waits.size())).c_str());
}

void CaseRecoptSkipRelative() {
    std::vector<ScriptAction> acts = {
        TestMove(0, 0), TestRel(1, 0), TestMove(2, 2),
        TestKey(),
        TestMove(3, 3), TestMove(4, 4)
    };
    const size_t before = acts.size();
    const auto r = recopt::MergeAllKeySplit(acts, "sum", 0.0);
    int rel = 0;
    for (const auto& a : acts)
        if (a.type == ActionType::MoveMouseRelative) ++rel;
    const bool ok = r.collectOk && r.applied == 1 && r.skippedRelative == 1 && rel == 1
        && acts.size() < before;
    Emit(L"recopt_skip_relative_segment", ok,
        ok ? L"" : (L"applied=" + std::to_wstring(r.applied)
            + L" skipRel=" + std::to_wstring(r.skippedRelative)).c_str());
}

std::vector<ScriptAction> CompressWaitSample() {
    // 中间两点距起点 < 5px，压缩后只留 (0,0) 与 (80,0)；间隔等待 0.10/0.30/0.50
    return {
        TestMove(0, 0), TestWait(0.10), TestMove(0, 1),
        TestWait(0.30), TestMove(0, 2), TestWait(0.50), TestMove(80, 0)
    };
}

void CaseRecoptCompressWaitFirst() {
    auto acts = CompressWaitSample();
    const auto r = recopt::CompressAllKeySplit(acts, 5.0, "first", 0.0);
    std::vector<double> waits;
    int moves = 0;
    for (const auto& a : acts) {
        if (a.type == ActionType::Wait) waits.push_back(a.duration);
        if (a.type == ActionType::MoveMouse) ++moves;
    }
    const bool ok = r.collectOk && r.applied == 1 && moves == 2 && waits.size() == 1
        && std::abs(waits[0] - 0.10) < 0.0005;
    Emit(L"recopt_compress_wait_first", ok,
        ok ? L"" : (L"applied=" + std::to_wstring(r.applied)
            + L" n=" + std::to_wstring(acts.size())
            + L" moves=" + std::to_wstring(moves)
            + L" waits=" + std::to_wstring(waits.size())).c_str());
}

void CaseRecoptCompressWaitFixed() {
    auto acts = CompressWaitSample();
    const auto r = recopt::CompressAllKeySplit(acts, 5.0, "fixed", 0.2);
    std::vector<double> waits;
    int moves = 0;
    for (const auto& a : acts) {
        if (a.type == ActionType::Wait) waits.push_back(a.duration);
        if (a.type == ActionType::MoveMouse) ++moves;
    }
    const bool ok = r.collectOk && r.applied == 1 && moves == 2 && waits.size() == 1
        && std::abs(waits[0] - 0.2) < 0.0005;
    Emit(L"recopt_compress_wait_fixed", ok,
        ok ? L"" : (L"applied=" + std::to_wstring(r.applied)
            + L" n=" + std::to_wstring(acts.size())
            + L" moves=" + std::to_wstring(moves)
            + L" waits=" + std::to_wstring(waits.size())).c_str());
}

void CaseSkillOptimizeDirectsToTools() {
    const std::wstring t = AgentSkillGet(L"optimize");
    const bool ok = t.find(L"optimizeRecording") != std::wstring::npos
        && t.find(L"optimizeScript") != std::wstring::npos
        && t.find(L"readScript") != std::wstring::npos
        && t.find(L"鼠标移动合并") != std::wstring::npos
        && t.find(L"compressPath") != std::wstring::npos;
    Emit(L"skill_optimize_directs_to_tools", ok,
        ok ? L"" : t.substr(0, 160).c_str());
}

void CaseSkillScriptTreeAndPlan() {
    const std::wstring t = AgentSkillGet(L"scriptStrategy");
    bool hasPlan = false;
    for (const auto& tool : BuildDefaultAgentTools()) {
        if (tool.name == L"planScriptActions") {
            hasPlan = true;
            break;
        }
    }
    const bool ok = t.find(L"planScriptActions") != std::wstring::npos
        && t.find(L"children") != std::wstring::npos
        && t.find(L"循环体") != std::wstring::npos
        && hasPlan;
    std::wstring detail;
    if (!ok) {
        detail = t.substr(0, 200);
        if (!hasPlan) detail += L" missing plan tool";
    }
    Emit(L"skill_script_tree_and_plan", ok, detail.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--json") == 0) {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
            setvbuf(stdout, nullptr, _IONBF, 0);
        } else if (_wcsicmp(argv[i], L"--list") == 0) {
            listOnly = true;
            selftest::InitUtf8Stdout();
            setvbuf(stdout, nullptr, _IONBF, 0);
        } else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            std::fwprintf(stderr,
                L"  AgentAssistantSelfTest.exe [--json] [--list] [--help]\n");
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"AgentAssistantSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    if (!selftest::gJson) {
        std::fwprintf(stderr, L"=== AgentAssistantSelfTest ===\n");
    }

    CaseTruncatePrefix();
    CaseTruncateOutOfRange();
    CaseTruncateSkipsInternalNudge();
    {
        JournalGuard guard;
        CaseUndoRoundtrip(guard);
        CaseRevertRestores(guard);
        CaseRevertMissingFile(guard);
        CaseRevertTwice(guard);
        CaseFileTools(guard);
    }
    CaseShellRejectUnknown();
    CaseShellRejectGitWrite();
    CaseShellWhereOk();
    CaseShellWhereRejectRecursive();
    CaseFileRejectSettings();
    CaseFileRejectWriteUi();
    CaseFetchUrlBlocksRfc1918();
    CaseSkillCatalog();
    CaseSkillFileOverride();
    CaseClipboard();
    CaseWriteScriptRejectsHandwritten();
    CaseWriteScriptAcceptsBuilt();
    CaseWriteScriptRejectsEmptyLoop();
    {
        JournalGuard guard;
        CaseReadScriptImageMarkers(guard);
    }
    CaseImageMarkersAndParts();
    CaseModelReasoningType();
    CaseToolResultBudget();
    CaseReadScriptSummary();
    CaseReadScriptRaw();
    CaseReadScriptPaging();
    const auto guarded = [](const wchar_t* name, std::function<void()> fn) {
        try {
            fn();
        } catch (const std::exception& e) {
            Emit(name, false, FromUtf8(e.what()).c_str());
        } catch (...) {
            Emit(name, false, L"unknown exception");
        }
    };
    guarded(L"describe_actions_covers_key_params", CaseDescribeCoversParams);
    guarded(L"describe_actions_quick_omits_values", CaseDescribeQuickOmitsValues);
    guarded(L"recopt_merge_splits_on_keys", CaseRecoptMergeSplitsOnKeys);
    guarded(L"recopt_merge_wait_per_segment", CaseRecoptMergeWaitPerSegment);
    guarded(L"recopt_skip_relative_segment", CaseRecoptSkipRelative);
    guarded(L"recopt_compress_wait_first", CaseRecoptCompressWaitFirst);
    guarded(L"recopt_compress_wait_fixed", CaseRecoptCompressWaitFixed);
    guarded(L"skill_optimize_directs_to_tools", CaseSkillOptimizeDirectsToTools);
    guarded(L"skill_script_tree_and_plan", CaseSkillScriptTreeAndPlan);
    guarded(L"conversation_draft_roundtrip", CaseConversationDraftRoundtrip);
    guarded(L"empty_conversation_not_listed", CaseEmptyConversationNotListed);
    guarded(L"conversation_with_round_is_listed", CaseConversationWithRoundIsListed);
    guarded(L"conversation_title_rejects_api_error", CaseConversationTitleRejectsApiError);
    guarded(L"user_facing_tool_reply_strips_outline", CaseUserFacingToolReplyStripsOutline);
    guarded(L"outline_header_has_no_user_constraints", CaseOutlineHeaderHasNoUserConstraints);
    guarded(L"skill_reply_forbids_dumping_constraints", CaseSkillReplyForbidsDumping);
    guarded(L"create_macro_folder_support",
        [] { JournalGuard guard; CaseCreateMacroFolder(guard); });
    guarded(L"delete_script_via_tool",
        [] { JournalGuard guard; CaseDeleteScriptViaTool(guard); });
    guarded(L"build_actions_rejects_missing_required", CaseBuildActionsRequired);
    guarded(L"loop_terminal_script_ends_immediately",
        CaseLoopTerminalScriptEndsImmediately);
    guarded(L"loop_repeat_guides_and_graceful_end",
        CaseLoopRepeatGuidesAndGracefulEnd);
    guarded(L"tools_schema_size_bounded", CaseToolsSchemaSize);
    guarded(L"script_skill_gate_enforced", CaseScriptSkillGateEnforced);
    guarded(L"reference_read_capped", CaseReferenceReadCapped);
    guarded(L"runprogram_no_keytext_default", CaseRunProgramNoKeyTextDefault);
    guarded(L"recon_tools_capped", CaseReconToolsCapped);
    guarded(L"read_reference_system_section", CaseReadReferenceSystemSection);
    guarded(L"env_var_expansion", CaseEnvVarExpansion);
    guarded(L"html_to_plain_text", CaseHtmlToPlainText);
    guarded(L"fetch_tool_guards", CaseFetchToolGuards);
    guarded(L"ai_action_exec_has_fetch", CaseAiActionExecHasFetch);
    guarded(L"locate_fail_block", CaseLocateFailBlock);
    guarded(L"fetch_then_runprogram_needs_confirm", CaseFetchThenRunProgramNeedsConfirm);

    selftest::EmitSummary();
    return selftest::ExitCode();
}
