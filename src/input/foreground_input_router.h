#pragma once
// foreground_input_router.h — 前台回放键鼠注入分流（Software / Interception / VirtualHid）

#include "app_settings.h"
#include "script_types.h"

#include <atomic>
#include <string>

/// 回放会话级路由：BeginSession 时按后端打开；失败则整会话回退 SendInput。
class ForegroundInputRouter {
public:
    static ForegroundInputRouter& Instance();

    /// 窗口模式回放应传 Software。
    void BeginSession(quickscript::ForegroundInputBackend backend);
    /// 兼容旧调用：wantHid=true → Interception。
    void BeginSession(bool wantHid);
    void EndSession();

    bool IsHidActive() const;
    quickscript::ForegroundInputBackend ActiveBackend() const;
    bool DidFallback() const;
    std::wstring FallbackReason() const;

    bool SendKey(unsigned short scanCode, bool down, bool extended);
    bool MoveRelative(int dx, int dy);
    bool Button(MouseButtonType button, bool down);
    bool Wheel(int delta, bool horizontal);
    /// 绝对屏坐标：VirtualHid 仅走 VHF 绝对报告；Interception 为 absolute stroke + SetCursorPos；否则 SetCursorPos。
    bool SetCursorScreen(int x, int y);

private:
    enum class Active : int { None = 0, Interception = 1, VirtualHid = 2 };

    std::atomic<bool> sessionActive_{false};
    std::atomic<int> active_{static_cast<int>(Active::None)};
    std::atomic<bool> didFallback_{false};
    std::wstring fallbackReason_;
};
