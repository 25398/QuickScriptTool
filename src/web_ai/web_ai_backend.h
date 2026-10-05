#pragma once
// ──────────────────────────────────────────────────────────────────
// web_ai_backend.h — 网页版 AI 后端的**编排层**
//
// 一次 `/v1/chat/completions` 的完整链路：
//   请求 JSON → 解析 → PlanTurn（写什么）→ 驱动扩展（写进去/提交/读回）
//   → ParseReply（反解析 tool_calls）→ OpenAI 响应（JSON 或 SSE）
//
// 入口由 `ExtBridgeServer` 的 HTTP 监听器调用（挂在同一个 127.0.0.1 端口上）：
//   `POST /v1/chat/completions`  →  HandleChatCompletion()
//   `GET  /qst/web-ai/probe`     →  RunLiveProbeReport()
//
// ⚠⚠ 为什么端点必须挂在本进程已有的桥监听器上（而不是另起一个 HTTP 服务）：
//   扩展**只能被持有那条 WS 连接的进程**驱动（`ExtBridgeServer::Request`），
//   另起端口 / 另起进程都得重新让扩展连过来 —— 那是把「同一个能力」实现两遍，
//   还要额外管端口、token、生命周期。挂同一个监听器 ⇒ 这些全都白拿。
// ──────────────────────────────────────────────────────────────────

#include <string>

namespace quickscript::webai {

/// 处理一次 chat completion。
/// @param requestJsonUtf8 请求体（UTF-8）
/// @param responseBody 出参：JSON 响应体，或 SSE 文本
/// @param isSse       出参：true = responseBody 为 SSE（请求里 `stream:true`）
/// @param httpStatus  出参：200 成功；4xx/5xx 为错误（body 是 OpenAI 形状的 error）
/// @return httpStatus == 200
bool HandleChatCompletion(const std::string& requestJsonUtf8, std::string& responseBody,
                          bool& isSse, int& httpStatus);

/// 原生真机探针（**这是唯一能真正驱动扩展的探针**，见 web_ai_driver.h 约束①）。
/// 全程只读 + 可选真发一条；任何异常都收敛成 JSON 报告，绝不抛出。
/// @param providerId 站点 id（默认取配置，再默认 doubao）
/// @param doSend     是否真的写一条并提交（会真的发消息！）
/// @param sendText   要发送的文本
/// @param doUpload   是否试一次**传图**（会真的往页面上挂一张 8×8 的测试图 ⇒
///                   输入框里会留下一个附件，**默认关**：探针不该在默认路径上改页面状态）
std::string RunLiveProbeReport(const std::string& providerId, bool doSend,
                               const std::string& sendText, bool doUpload = false);

/// 同上的 HTTP 入口形态：直接吃请求体（`{"provider":..,"send":bool,"text":..}`）。
/// 桥那边不便解析 JSON（window_mode_core 不该依赖 nlohmann）⇒ 原始 body 传进来，
/// 由本模块用 nlohmann 解析（字段提取统一用 nlohmann 是本仓的硬规则）。
std::string RunLiveProbeReportJson(const std::string& requestBodyUtf8);

/// ★ 窗口反代探针（`POST /qst/window-ai/probe`）。
/// 请求体：`{"client":"doubao-app","controls":true,"dryRun":true,"send":false,"text":"…"}`
/// 返回：候选窗口 + UIA 控件 dump + 输入框候选打分（+ 可选干跑写入 / 真发一条）。
///
/// ⚠ 探针在这里是**必需**的，不是顺手加的调试口：窗口端没有 CSS selector 那种
///   可离线推断的判据 —— 真实 UIA 树（类型 / Name / AutomationId / 在不在下半部）
///   只有**跑起来**才看得到。`window_ai_providers.json` 就是靠这份 dump 调出来的。
std::string RunWindowAiProbeReportJson(const std::string& requestBodyUtf8);
/// ★设置页「窗口 Agents」入口：列出可绑定的窗口客户端（内置 + 用户档案），
/// 每一项带 `bound`（是否已写进 `windowClients`）/`running`/`calibrated`。
std::string RunWindowAgentsListJson();

/// ★把某个客户端窗口**登记成模型**（准星绑定的落点）。
/// 请求体二选一：`{"client":"doubao-app"}`（已知档案）
/// 或 `{"process":"Doubao.exe","title":"..."}`（准星拾取到的窗口 ⇒ 自动匹配档案，
/// 匹配不到就按进程名建一条**骨架**档案写进 window_ai_providers.json）。
/// `{"client":"...","unbind":true}` 解除绑定。
std::string RunWindowAgentBindJson(const std::string& requestBodyUtf8);

}  // namespace quickscript::webai
