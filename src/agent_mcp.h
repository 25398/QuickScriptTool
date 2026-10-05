#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_mcp.h — MCP（Model Context Protocol）**客户端**
//
// 本产品**已经是 MCP server**：`src/mcp_server.cpp` 把原生能力
// （截图/点击/键盘/UIA/读文档）以 MCP 工具形式暴露给外部 agent
// （2026-09-16 与用户确认的「方向 B 反向集成」）。
//
// 这里补的是**对称的另一半** —— 让助手能挂上**用户自己配置的**外部 MCP server
// （这才是「主流 Agent 生态」的门票）。**产品不主动探测、不主动接入任何第三方软件**：
// 只有用户显式写进配置的 server 才会被启动，一个都不写就一个进程都不起。
// ⚠ 曾经有过「自动发现 GenOffice 并自动接入」的逻辑，**已删除** ——
//   那会让产品在用户不知情的情况下把能力外包给第三方，属于**隐式依赖**，见 .cpp 顶部注释。
//
// 传输：stdio，每行一个 JSON-RPC 2.0 消息（消息内不得含裸换行）。
// 协议层全部是**纯函数**（可逐格自检，不需要真的起进程）；
// 进程层用一个自建的假 server（本自检 exe 的 `--fake-mcp-server` 模式）做端到端验证，
// 不依赖网络、不依赖用户装任何东西。
//
// ⚠ 安全边界（S4）：外部 MCP server 就是**任意代码执行**。所以
//   · 只有用户在 `AppDir()\mcp_servers.json` 里显式写下的 server 才会被启动；
//   · 配置文件不存在 = 一个进程都不起（自检/无配置用户完全不受影响）；
//   · 不放行 HTTP 传输（本机 stdio 起进程即可，远程 server 面太大）。
// ──────────────────────────────────────────────────────────────────

#include "agent_core.h"

#include <string>
#include <vector>

namespace qst {
namespace agent {

/// 一个外部 MCP server 的配置
struct McpServerConfig {
    std::wstring name;                  ///< 展示名，也是工具名前缀
    /// 可执行文件。⚠ 实测（2026-09-23）：`CreateProcessW` 对**裸名只补 `.exe`**，
    /// **不会**按 PATHEXT 去找 `.cmd`/`.bat` ⇒ `"mytool"` 在只有 `mytool.cmd` 的机器上
    /// 报 WinError 2（提示是「启动 MCP server 失败（找不到程序？）」，很难联想到真因）。
    /// 所以：**`.exe` 可以写裸名，`.cmd`/`.bat` 必须写完整路径。**
    /// （GenOffice 那条路已由 `DetectGenOfficeCli` 解析成绝对路径，用户不用管。）
    std::wstring command;
    std::vector<std::wstring> args;
    std::wstring cwd;                   ///< 工作目录。很多 server 靠它找自己的依赖，建议填。
    bool enabled = true;                ///< 缺省视为启用
};

/// 配置文件：`AppDir()\mcp_servers.json`
std::wstring McpServersConfigPath();

/// 解析配置。**文件不存在不是错误**（返回 true 且 out 为空）—— 绝大多数用户没有外部
/// server，这条路必须静默。JSON 坏掉才是错误。
bool ParseMcpServersJson(const std::string& jsonText,
    std::vector<McpServerConfig>& out, std::wstring& err);

/// 序列化（写回配置；给未来的设置界面用）
std::string SerializeMcpServersJson(const std::vector<McpServerConfig>& servers);

/// 读配置（不存在 → 空列表 + true）
bool LoadMcpServers(std::vector<McpServerConfig>& out, std::wstring& err);

// ── 协议层（纯函数）──────────────────────────────────────────────

/// 造一行 JSON-RPC 请求。paramsJson 为空则不输出 params（用于无参方法）。
/// ⚠ 会**剥掉** JSON 里的裸换行 —— stdio 传输按行分帧，消息里混进换行会把流拆坏。
std::string McpBuildRequestLine(int id, const std::string& method, const std::string& paramsJson);

/// 造一行 JSON-RPC 通知（无 id，不需要响应）
std::string McpBuildNotificationLine(const std::string& method, const std::string& paramsJson);

enum class McpLineKind {
    Ignore = 0,  ///< 不是我们要的那条（别人的 id / 通知 / 日志噪声）—— **不算错误**
    Result,      ///< 匹配 id 的成功响应
    Error,       ///< 匹配 id 的错误响应
};

/// 解析一行响应。只认 id 匹配的那条；其余一律 Ignore（服务端可能先发日志或通知）。
McpLineKind McpParseResponseLine(const std::string& line, int id,
    std::string& resultJson, std::wstring& errOut);

/// 从 tools/list 结果里抽工具
struct McpRemoteTool {
    std::string name;
    std::string description;
    std::string inputSchema;   ///< JSON 字符串；空则由调用方兜底成 {"type":"object"}
};

bool McpParseToolsList(const std::string& resultJson,
    std::vector<McpRemoteTool>& out, std::wstring& err);

/// 助手工具表里的名字：`mcp__<server>__<tool>`。
/// 为什么要加前缀：模型一眼能看出这是**外部**工具（而不是我们自己的），
/// 出事时也一眼能定位到是哪个 server。
std::string McpQualifiedToolName(const std::wstring& serverName, const std::string& toolName);
bool McpSplitQualifiedToolName(const std::string& qualified,
    std::wstring& server, std::string& tool);

/// 一次 tools/call 结果的解析产物。
struct McpToolContent {
    std::wstring text;                      ///< content[] 里文本部分的拼接（含附图标记）
    std::vector<std::wstring> imageFiles;   ///< 已落盘的图片绝对路径（原顺序）
};

/// 解析 tools/call 结果。
///
/// imageOutDir 非空 ⇒ 把 content[] 里 `type=="image"` 的内容 base64 解码后落盘，
/// 路径收进 out.imageFiles，并在 out.text 末尾追加 `[[AGENT_IMG:<绝对路径>]]` 标记 ——
/// 宿主的多模态通路（`src/agent_attachment.*` 的 `AgentExtractImageMarkers` /
/// `AgentBuildImageParts`）会据此把图片作为 `image_url` 塞进**下一轮**请求。
/// 这就是「产物验收」的关键：文档渲染出的 PNG 只有走到模型眼前，
/// 「写完了吗」才不是模型自说自话。
/// imageOutDir 为空 ⇒ 不落盘，非文本内容只留一句占位说明（自检用，避免副作用）。
///
/// isError==true 时返回 false 并把说明写进 err，但 out.text 仍填好（错误正文有用）。
bool McpExtractToolResult(const std::string& resultJson, const std::wstring& imageOutDir,
    McpToolContent& out, std::wstring& err);

/// 附图落盘目录：`AppDir()\agent_mcp_images`
std::wstring McpImageSpoolDir();

/// ── 附图目录清理（**必须做，否则无限增长**）────────────────────────────────────
/// 每张图在**下一轮**请求里被消费一次后就不再被引用：`AgentExtractImageMarkers()`
/// 会把 `[[AGENT_IMG:…]]` 标记从文本里**删掉**，所以历史里不会留路径。
/// ⇒ 目录只增不减，必须按年龄清理。
/// 保留期给到 7 天：正常消费是秒级的，7 天远超任何活着的需求；
/// 万一真被删了，宿主侧也只是降级成一行 `[读取失败] …`（`AgentBuildImageParts`
/// 会跳过并记一行），**不会崩、不会中断对话**。
const int kMcpImageRetentionDays = 7;

/// 清理 rootDir 下**最后写入时间早于 retentionDays 天**的文件（递归含 `<server>\` 子目录；
/// 顺手删空目录）。返回删除条数；`removed` 非空时回填被删路径。
/// ⚠ 未来时间戳的文件一律不动（时钟不可信时宁可不删）。
int SweepStaleMcpImagesIn(const std::wstring& rootDir, int retentionDays,
    std::vector<std::wstring>* removed = nullptr);
/// 用 `McpImageSpoolDir()` 作为根目录的便捷重载。
int SweepStaleMcpImages(std::vector<std::wstring>* removed = nullptr);

// ── 进程层 ───────────────────────────────────────────────────────

/// 工具调用的默认超时。比握手宽得多 —— 渲染文档 / 转 PDF / 起 Excel 都可能几十秒。
const int kMcpCallTimeoutMs = 180000;

/// 握手 / 列工具的默认超时（MCP server 首次启动可能慢：要拉 Node 运行时）。
const int kMcpHandshakeTimeoutMs = 20000;

/// ★枚举阶段的总预算。`CollectMcpAgentTools()` 是从 **UI 线程**调的
/// （BuildDefaultAgentTools ← RebuildAgentCoreLocked），所以绝不能让一条坏配置
/// 把界面冻住：整个枚举（所有 server 的启动+握手+列工具）共享这个预算，
/// 用完就跳过剩下的并写进诊断。典型情况（没写配置）**0 次进程启动**，
/// 只花一次小文件读。
const int kMcpEnumerateBudgetMs = 10000;

/// stdio 传输的 MCP 客户端：起子进程、按行收发、带超时。
class McpStdioClient {
public:
    McpStdioClient();
    ~McpStdioClient();
    McpStdioClient(const McpStdioClient&) = delete;
    McpStdioClient& operator=(const McpStdioClient&) = delete;

    bool Start(const McpServerConfig& cfg, std::wstring& err);
    void Stop();
    bool IsRunning() const;

    /// initialize 握手（含 notifications/initialized）
    /// timeoutMs 可覆写：枚举阶段在 **UI 线程**上跑，必须能收紧（见 kMcpEnumerateBudgetMs）。
    bool Initialize(std::wstring& err, int timeoutMs = kMcpHandshakeTimeoutMs);
    bool ListTools(std::vector<McpRemoteTool>& out, std::wstring& err,
        int timeoutMs = kMcpHandshakeTimeoutMs);
    /// timeoutMs 可覆写：自检要能在 1 秒内验「服务端不回时按超时返回、不永久卡住」，
    /// 否则一条用例就得等满 180 秒。产品路径用默认值。
    bool CallTool(const std::string& toolName, const std::string& argsJson,
        std::wstring& outText, std::wstring& err, int timeoutMs = kMcpCallTimeoutMs);

    /// 最近一次启动/调用的诊断（自检与日志用）
    const std::wstring& LastError() const { return lastError_; }

    /// ── 孤儿防护状态（`Stop()` 管不了崩溃，所以子进程另有 Job 兜底）──────────────
    /// 宿主被强杀/崩溃时 `Stop()` 根本没机会跑 ⇒ 子进程会一直挂着（用户配的 server
    /// 可能是 Node/Python 进程，几百 MB + 持有管道）。`Start()` 会把子进程挂进一个
    /// `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job，本进程句柄一没（正常退出/崩溃/
    /// 被 TerminateProcess 都一样）子进程连同后代一起被杀。
    /// 返回 false 时 `diag` 给出原因 —— **「防护没生效」不该是静默的**。
    bool OrphanGuardActive(std::wstring& diag) const;

    /// 供自检用：建一个 `KILL_ON_JOB_CLOSE` 的 Job（`Start()` 内部用的就是它）。
    /// 调用方负责 `CloseHandle` —— 关掉它会**立刻杀掉** Job 内所有进程（这正是被测行为）。
    static HANDLE CreateKillOnCloseJobForTest();

private:
    bool SendLine(const std::string& line, std::wstring& err);
    /// 读到 id 匹配的响应（或超时）。kind 见 McpLineKind。
    McpLineKind ReadResponse(int id, std::string& resultJson, std::wstring& err, int timeoutMs);
    int NextId();

    struct Impl;
    Impl* impl_ = nullptr;
    std::wstring lastError_;
};

// ── 助手侧装配 ───────────────────────────────────────────────────

/// `CreateProcessW` 能不能直接启动这个路径？
/// ⚠ 实测事实：`.exe`/`.cmd`/`.bat` 可以（`.cmd` 由系统代起 cmd.exe），
///   **无扩展名的脚本不行**（WinError 2 系统找不到指定的文件）。
///   用户配置里写裸名时只有 `.exe` 能解析到，`.cmd`/`.bat` 必须写完整路径。
bool McpCommandIsRunnable(const std::wstring& path);

/// ⚠ 这里原来有 `DetectGenOfficeCli()` / `MakeGenOfficeMcpServerConfig()`（探测并自动接入
/// 第三方办公引擎）——**已删除**，理由见 `agent_mcp.cpp` 顶部「关于自动接入第三方办公引擎」
/// 的注释：本产品**不主动依赖、也不主动探测任何第三方软件**。
/// 用户要接什么，自己写进 `mcp_servers.json`（厂商中立的显式扩展点）。

/// 收集所有可用外部 server 的工具，并装配成助手工具。
/// 任何 server 起不来都**只跳过它**，不影响其它 server 与内置工具。
/// 结果被缓存（同一份配置不重复起进程）；配置变化或显式 Reset 才重建。
std::vector<AgentTool> CollectMcpAgentTools();

}  // namespace agent
}  // namespace qst
