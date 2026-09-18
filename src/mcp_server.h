#pragma once
// ──────────────────────────────────────────────────────────────────
// mcp_server.h — 把 QuickScriptTool 的原生能力（截图/鼠标/键盘/UIA/读文档）
//                以 MCP（Model Context Protocol）工具形式暴露给外部 agent。
//
// 定位（2026-09-16 与用户确认的方向 B「反向集成」）：
//   · 别人把 computer-use 驱动接进自己的 agent；我们反过来——
//     让**我们的原生链路成为 MCP 工具**，DSH / Claude / 其它 MCP 客户端挂上就能用。
//   · 为什么不让外部 driver 驱动我们：我们的价值就在这套 Windows 原生能力
//     （WGC 合成采集、SendInput、UIA 精确控件名、浏览器扩展 DOM、中文 IME、
//      可回放的宏动作）——通用 driver 没有这些。
//
// 传输：MCP stdio —— 每行一个 JSON-RPC 2.0 消息（消息内不得含裸换行）。
// 入口：QuickScriptTool.exe --mcp（在启动 WebView2 之前分流，headless 运行）。
// ──────────────────────────────────────────────────────────────────

#include <string>

/// 处理一行 JSON-RPC 请求，返回一行响应（通知类消息返回空串）。
/// 抽成纯函数便于自检：协议层（initialize/tools/list/tools/call/错误码）
/// 不需要真的截屏或注入输入就能测。
std::string McpHandleRequestLine(const std::string& line);

/// 当前暴露的工具数量（自检/诊断）
int McpToolCount();

/// 跑 stdio 主循环（读 stdin 一行 → 处理 → 写 stdout 一行），EOF 时返回。
/// 只在 --mcp 模式下调用。
int RunMcpStdioServer();
