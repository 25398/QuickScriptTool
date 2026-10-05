#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace windowmode {

struct ExtScriptInfo {
    std::string name;
    std::string file;
    std::string path;
    /// windowMode 元数据，供扩展弹窗按当前标签过滤「同类」脚本。
    bool windowModeEnabled = false;
    std::string windowName;
    std::string windowClassName;
    std::string inputStrategy; // auto | cdp | softMessage
};

struct ExtRunState {
    bool running = false;
    std::string currentScript;
};

/// UI 线程注册：桥线程只调用这些回调，不直接依赖 EngineHost。
struct ExtScriptApiHandlers {
    std::function<std::vector<ExtScriptInfo>()> listScripts;
    /// 投递运行请求；busy/非法路径时返回 false 并写 err（英文短码：busy/bad_path/post_failed）。
    std::function<bool(const std::string& pathOrFileUtf8, std::string& err)> runScript;
    std::function<void()> stopScript;
    std::function<ExtRunState()> getRunState;
};

/// 网页版 AI 后端的处理器（**由 qst_engine 注册**，理由同 ExtScriptApiHandlers）：
/// 桥这一层（window_mode_core）**不能**直接依赖 `src/web_ai/*` ——
/// 那个模块被编进 qst_engine，而 window_mode_core 还被自检/播放器等多个目标链接，
/// 直接调它会给每个目标都添一份链接依赖。回调注入是本文件既有惯例
/// （`SetScriptApiHandlers` 就是这么做的）。
///
/// ⚠ 未注册时两个路由都回 **501** 并说明原因，不会静默 404。
struct ExtWebAiHandlers {
    /// 处理 `POST /v1/chat/completions`：body → 响应体 + 是否 SSE + HTTP 状态码
    std::function<bool(const std::string& requestBody, std::string& responseBody,
        bool& isSse, int& httpStatus)> chat;
    /// 处理 `POST /qst/web-ai/probe`：body（JSON）→ 报告 JSON 文本
    std::function<std::string(const std::string& requestBody)> probe;
    /// ★ 处理 `POST /qst/window-ai/probe`：**窗口反代**（GUI 客户端）探针。
    /// 与网页端探针分开，是因为它做的是**另一条链**（UIA + 键鼠 + 抢前台），
    /// 出问题时要看的东西完全不同（控件 dump vs 站点 selector）。
    std::function<std::string(const std::string& requestBody)> windowProbe;
    /// ★ 窗口 Agents 诊断路由（`/qst/window-agents`、`/qst/window-agent-bind`）：
    /// 设置页「准星绑定窗口」用的那对只读/绑定接口。
    ///
    /// ⚠⚠ 这两个**必须**和上面三个一样走注入，不能像 2026-09-27 之前那样在
    ///   `ext_bridge_server.cpp` 里直接 `quickscript::webai::RunWindowAgentsListJson()`
    ///   —— 那让桥多了一条指向 `src/web_ai/*` 的链接依赖，而本文件顶部的注释
    ///   （以及本结构体的存在理由）恰恰是「桥这一层不能依赖 src/web_ai/*」。
    ///   代价不是理论上的：`window_mode_core` 还被 `WindowModeSelfTest` / `WindowModeDiag`
    ///   链接，而那两条链接列表里**没有** `qst_engine` ⇒ 直接调用会让它们
    ///   **LNK2019 链不过**（实测：`RunWindowAgentsListJson` / `RunWindowAgentBindJson`
    ///   两个无法解析的外部符号），自检连编都编不出来。
    ///   未注册时这两条路由回一句明确的 not_registered（见路由实现），**不静默 404**。
    std::function<std::string()> windowAgentsList;
    std::function<std::string(const std::string& requestBody)> windowAgentBind;
};

/// 本机 HTTP+WebSocket 桥：扩展 CDP 输入 + 脚本列表/运行/停止。
/// 允许多个扩展实例同时连接（多用户配置）；attach 时逐个尝试并校验标题。
class ExtBridgeServer {
public:
    static ExtBridgeServer& Instance();

    bool Start(std::wstring& err);
    void Stop();

    /// 热键停止：打断正在等待的 Request / WaitForExtension（不关监听）。
    void AbortPending();
    void ClearAbort();
    bool IsAborted() const { return abort_.load(); }

    bool IsRunning() const { return running_.load(); }
    bool IsExtensionConnected() const { return extConnected_.load(); }
    int Port() const { return port_.load(); }
    int HttpProbeCount() const { return httpProbeCount_.load(); }
    int WsHandshakeFailCount() const { return wsHandshakeFailCount_.load(); }
    std::string Token() const;
    int ExtensionClientCount() const;

    void SetScriptApiHandlers(ExtScriptApiHandlers handlers);
    /// 注册网页版 AI 端点（见 ExtWebAiHandlers；未注册时路由回 501）
    void SetWebAiHandlers(ExtWebAiHandlers handlers);

    /// 发送一条 JSON 请求，等待 type=result 且同 id。timeoutMs 超时返回 false。
    /// type=="attach" 时会对所有已连接扩展逐个尝试，直到标题与 titleHint 匹配。
    bool Request(const std::string& type, const std::string& extraJsonFields,
        std::string& resultJson, std::wstring& err, int timeoutMs = 8000);

    bool WaitForExtension(int timeoutMs, std::wstring& err);

    /// 重写 bridge_runtime.json / Native Messaging 清单，便于休眠的扩展重新发现桥。
    void RefreshDiscovery();

    /// 扩展 POST /qst/shot 写入的最近一帧 JPEG（找图用，避免 WS 大包弄死 MV3）。
    bool TakeLastShotJpeg(std::vector<uint8_t>& out);
    void ClearLastShotJpeg();

private:
    ExtBridgeServer() = default;
    ~ExtBridgeServer();
    ExtBridgeServer(const ExtBridgeServer&) = delete;
    ExtBridgeServer& operator=(const ExtBridgeServer&) = delete;

    void ThreadMain();
    bool BindPort(std::wstring& err);
    void WriteConfigFile() const;
    bool HandleClient(uintptr_t clientSock);
    bool SendWsText(uintptr_t sock, const std::string& utf8);
    bool RecvWsText(uintptr_t sock, std::string& out, int timeoutMs);
    bool RequestOnSock(uintptr_t sock, const std::string& type,
        const std::string& extraJsonFields, std::string& resultJson,
        std::wstring& err, int timeoutMs);
    void RemoveSockLocked(uintptr_t sock);
    bool TokenMatches(const std::string& got) const;
    /// 与 `TokenMatches` 同一把尺，但**要求调用方已持 `mu_`**。
    ///
    /// ⚠⚠ 为什么必须有这个重载（2026-09-27）：`TokenMatches` 自己 `lock(mu_)`，
    ///   所以像 WS 握手那种「先取 expectTok 再连续比两次」的写法**不能**直接复用它；
    ///   上一版是手写 `!TokenMatches(qToken) || qToken != expectTok` 绕过去的，结果是
    ///   ① 同一件事两条判据（`TokenMatches` 将来被加强，这里悄悄还是旧的）；
    ///   ② 想顺手比一下 **hello 里的 token 与 query 里的 token 是否同一个**都没法比
    ///      （再调一次 `TokenMatches` 就是二次加锁 ⇒ 未定义行为）。
    bool TokenMatchesLocked(const std::string& got) const;
    void SendHttpJson(uintptr_t clientSock, int status, const std::string& body,
        const std::string& origin, bool publicOk = false);
    /// 与 SendHttpJson 同，但可指定 Content-Type（网页 AI 端点的 SSE 要用
    /// `text/event-stream`；其余路由仍走 JSON）。
    void SendHttpWithType(uintptr_t clientSock, int status, const std::string& body,
        const std::string& origin, const std::string& contentType, bool publicOk = false);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> extConnected_{false};
    std::atomic<int> port_{0};
    std::atomic<int> httpProbeCount_{0};
    std::atomic<int> wsHandshakeFailCount_{0};
    std::string token_;
    uintptr_t listenSock_ = 0;

    std::mutex mu_;
    std::condition_variable cv_;
    int nextId_ = 1;

    /// ★★ 在途请求的等待槽：**按请求 id 关联**，不是全局单槽。
    ///
    /// ⚠⚠ 为什么必须改成 map（2026-09-25，为了「多会话并行」）：
    ///   原来只有**一个** `waitingId_` + 一份 `waitingResult_`/`waitingDone_` ⇒
    ///   两条并发请求会**互相吞掉对方的回执**（后发的那条把 `waitingId_` 覆盖掉，
    ///   先发的那条永远等不到 ⇒ 表现成"莫名超时"）。
    ///   这也是为什么 `web_ai_driver.cpp` 不得不用一把全局锁把调用串起来。
    ///
    ///   ⚠ 扩展侧**不需要改**：它本来就按 `id` 回执（`nextId_++` 生成的那个 id）。
    ///     缺的只是宿主这边"按 id 找等待者"。
    struct WaitSlot {
        std::string result;
        bool done = false;
    };
    std::map<int, std::shared_ptr<WaitSlot>> pending_;

    /// 把**所有**在途等待标记完成（带给定回执）。断开 / 中止 / 关服务时用。
    /// ⚠ 必须在持有 `mu_` 时调用。
    void FailAllPendingLocked(const std::string& result);

    /// ★★ **一键式**：扩展不在时，软件自己把浏览器拉起来（用户不该为此手动开浏览器）。
    ///
    /// 设计（2026-09-26 用户要求）：
    ///   · 默认启动**上次连接过桥的那个浏览器**（`lastBrowserExe_`）；
    ///   · 没有记录 ⇒ 回退到 `FindMsEdgeExe()`（Edge）；
    ///   · ⚠⚠ 用 `--no-startup-window` **后台启动、不弹窗** —— 用户明确抱怨过"占用界面前台"；
    ///     扩展的 service worker 照样会起来并连上桥，之后要开标签页时由扩展侧的
    ///     `windows.create({focused:false})` 兜底（同样不抢焦点）。
    ///   · ⚠ **限流 60 秒**：`WaitForExtension` 每次请求都会调，不限流会狂启浏览器。
    ///   · ⚠⚠ **自检/探针/播放器进程一律不启动**（否则跑一次自检就弹一个浏览器）。
    /// @return 这次是否**真的发起了启动**（false = 被限流 / 没找到浏览器 / 不该启动）
    bool EnsureBrowserRunning();

    /// 上次连上桥的浏览器 exe（扩展在 hello 里自报浏览器名，桥据此记下）
    std::wstring lastBrowserExe_;
    /// 上次真正发起启动的时刻（毫秒，steady_clock）——限流用
    std::atomic<long long> lastLaunchAtMs_{0};
    std::vector<uintptr_t> extSocks_;
    uintptr_t activeExtSock_ = 0;

    ExtScriptApiHandlers scriptApi_;
    ExtWebAiHandlers webAi_;

    std::vector<uint8_t> lastShotJpeg_;
};

/// 引导用户侧载 extension/edge。
void OpenExtensionInstallGuide();

/// 扩展目录（exe 旁 extension\\edge）。
std::wstring ExtensionEdgeDirectory();

/// Chrome/Edge Native Messaging：把本机桥 token 交给打包 CRX（读不到 bridge_runtime.json）。
int RunExtNativeMessagingHost();
void RegisterExtNativeMessagingHost();

/// 是否允许把**当前 exe** 注册为浏览器扩展的原生消息宿主（默认允许）。
/// 独立播放器（导出的脚本 EXE）必须关掉，理由：
///   注册清单里的 path 取的是 ExePath()，播放器会把自己写进去；浏览器随后会反复
///   拉起它当宿主（`QstPlayer.exe chrome-extension://.../ --parent-window=0`），
///   而播放器既不认这个调用、又要去解析 payload 并迁移目录 —— 实测形成**进程风暴**
///   （几十个实例互相拉起）。同时这也是"产物不写注册表"这条产品约束的要求。
void SetExtNativeHostAllowed(bool allowed);
bool ExtNativeHostAllowed();

/// 解析桥配置 JSON（自检用）。
bool ParseExtBridgeConfigJson(const std::string& json, int& port, std::string& token);

/// 自检用：进程名判据「这个进程算不算浏览器」。
///
/// 与 `ExtBridgeServer::EnsureBrowserRunning` 里那道「浏览器已经在跑就别再拉一个」
/// 的护栏**共用同一个实现** —— 2026-09-27 真机事故：扩展离线时无条件
/// `ShellExecute(msedge.exe)`，用户明明开着浏览器（脚本目标窗口就在里面），
/// 结果又冒出一个窗口/新标签页。护栏的判据是纯函数，所以能被自检逐个钉死
/// （含 `msedgewebview2.exe` / 自家 exe **不算**浏览器这两条反例）。
bool ExtBrowserLeafNameIsBrowser(const wchar_t* leafName);

/// 原生消息宿主 `path` 的文件名合法性判据（纯函数，自检钉住规则本身）：
/// 只有产品 exe（`QuickScriptTool.exe` / `QstPlayer.exe`）配当宿主。
/// 2026-09-24 事故：自测 exe 也会起扩展桥、每 15s 重注册，用「当前 exe」就把它自己写成了宿主
/// ⇒ 浏览器每次 `connectNative` 拉起**自测 exe** ⇒ 它忽略参数跑整套自测 ⇒ 无限弹 notepad 窗口。
/// 与上面播放器那条是**同一类**事故（都源于 path 取「当前 exe」），所以判据收在注册函数里，
/// 任何自测/诊断/播放器 exe 都不可能再把自己写进去。
bool NativeHostExeNameIsProduct(const std::wstring& exePath);

}  // namespace windowmode
