#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
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

/// UI 线程注册：桥线程只调用这些回调，不直接依赖 MainWindow。
struct ExtScriptApiHandlers {
    std::function<std::vector<ExtScriptInfo>()> listScripts;
    /// 投递运行请求；busy/非法路径时返回 false 并写 err（英文短码：busy/bad_path/post_failed）。
    std::function<bool(const std::string& pathOrFileUtf8, std::string& err)> runScript;
    std::function<void()> stopScript;
    std::function<ExtRunState()> getRunState;
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
    std::string Token() const;
    int ExtensionClientCount() const;

    void SetScriptApiHandlers(ExtScriptApiHandlers handlers);

    /// 发送一条 JSON 请求，等待 type=result 且同 id。timeoutMs 超时返回 false。
    /// type=="attach" 时会对所有已连接扩展逐个尝试，直到标题与 titleHint 匹配。
    bool Request(const std::string& type, const std::string& extraJsonFields,
        std::string& resultJson, std::wstring& err, int timeoutMs = 8000);

    bool WaitForExtension(int timeoutMs, std::wstring& err);

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
    void SendHttpJson(uintptr_t clientSock, int status, const std::string& body);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> extConnected_{false};
    std::atomic<int> port_{0};
    std::string token_;
    uintptr_t listenSock_ = 0;

    std::mutex mu_;
    std::condition_variable cv_;
    int nextId_ = 1;
    int waitingId_ = 0;
    std::string waitingResult_;
    bool waitingDone_ = false;
    std::vector<uintptr_t> extSocks_;
    uintptr_t activeExtSock_ = 0;

    ExtScriptApiHandlers scriptApi_;

    std::vector<uint8_t> lastShotJpeg_;
};

/// 引导用户侧载 extension/edge。
void OpenExtensionInstallGuide();

/// 扩展目录（exe 旁 extension\\edge）。
std::wstring ExtensionEdgeDirectory();

/// 解析桥配置 JSON（自检用）。
bool ParseExtBridgeConfigJson(const std::string& json, int& port, std::string& token);

}  // namespace windowmode
