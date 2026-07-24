#pragma once

#include "script_types.h"

#include <windows.h>

#include <string>

namespace windowmode {

bool IsChromiumBrowserExecutable(const std::wstring& imagePath);
std::wstring QueryHwndProcessImagePath(HWND hwnd);

/// 不重启浏览器：新开 inspect 标签并尝试启用远程调试（游戏页数据保留）。
bool EnableChromiumRemoteDebuggingWithoutRestart(HWND top, int preferredPort, std::wstring& err);

/// 会重启浏览器并丢失页面内存状态——产品默认路径不要调用。
bool RelaunchChromiumWithRemoteDebugging(const std::wstring& browserExe, int port,
    std::wstring& err);

/// Chromium/Edge 键鼠：经 DevTools Protocol（不抢系统前台）。
class CdpInputSession {
public:
    CdpInputSession() = default;
    ~CdpInputSession();

    CdpInputSession(const CdpInputSession&) = delete;
    CdpInputSession& operator=(const CdpInputSession&) = delete;

    /// 连接本机 remote-debugging-port，按窗口标题匹配 page target。
    /// 端口未开时返回 NEED_INPLACE，由上层在不重启前提下启用调试。
    bool ConnectForWindow(HWND top, int preferredPort, const std::wstring& titleHint,
        std::wstring& err);
    void Disconnect();
    bool IsConnected() const { return connected_; }

    bool KeyEvent(UINT vk, bool down, std::wstring& err);
    bool MouseMove(int cx, int cy, std::wstring& err);
    bool MouseButton(int cx, int cy, MouseButtonType button, bool down, std::wstring& err);
    bool MouseClick(int cx, int cy, MouseButtonType button, std::wstring& err);
    bool Scroll(int cx, int cy, int steps, bool vertical, bool positive, std::wstring& err);
    bool InsertText(const std::wstring& text, std::wstring& err);

private:
    bool Call(const std::string& method, const std::string& paramsJson, std::wstring& err);
    bool UpgradeWebSocket(const std::wstring& wsUrl, std::wstring& err);
    bool SendRaw(const std::string& utf8, std::wstring& err);
    bool RecvUntilId(int id, std::wstring& err);

    void* hSession_ = nullptr;
    void* hConnect_ = nullptr;
    void* hRequest_ = nullptr;
    void* hWebSocket_ = nullptr;
    bool connected_ = false;
    int nextId_ = 1;
    int lastCx_ = 0;
    int lastCy_ = 0;
};

}  // namespace windowmode
