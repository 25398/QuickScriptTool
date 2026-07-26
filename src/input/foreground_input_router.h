#pragma once
// foreground_input_router.h — 前台键鼠注入分流（连点 / 录制回放 / 鼠标宏；Software / Interception / VirtualHid）

#include "app_settings.h"
#include "script_types.h"

#include <atomic>
#include <string>

/// 会话级路由：BeginSession 时按后端打开；失败则整会话回退 SendInput。
class ForegroundInputRouter {
public:
    static ForegroundInputRouter& Instance();

    /// 窗口模式回放应传 Software；连点/前台录制回放/鼠标宏传设置里的 foregroundInputBackend。
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
    /// 绝对屏坐标：VirtualHid 只 SetCursorPos（不发绝对 HID，避免多屏乱漂）；
    /// Interception 发绝对报告后再 SetCursorPos；Software 仅 SetCursorPos。
    bool SetCursorScreen(int x, int y);

private:
    enum class Active : int { None = 0, Interception = 1, VirtualHid = 2 };

    std::atomic<bool> sessionActive_{false};
    std::atomic<int> active_{static_cast<int>(Active::None)};
    std::atomic<bool> didFallback_{false};
    std::wstring fallbackReason_;
};
