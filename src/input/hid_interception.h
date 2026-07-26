#pragma once
// ──────────────────────────────────────────────────────────────────
// hid_interception.h — Interception 动态加载 + 键鼠 stroke 注入
// ──────────────────────────────────────────────────────────────────

#include "script_types.h"

#include <mutex>
#include <string>

/// 用户态封装：动态加载 interception.dll，不静态链接（LGPL）。
class HidInterceptionBackend {
public:
    static HidInterceptionBackend& Instance();

    /// 探测 dll + 驱动是否可用（会短暂 create/destroy context）。
    bool ProbeAvailable(std::wstring* errorOut = nullptr);

    bool Open(std::wstring* errorOut = nullptr);
    void Close();
    bool IsOpen() const;

    bool SendKey(unsigned short scanCode, bool down, bool extended);
    bool MoveRelative(int dx, int dy);
    bool MoveAbsoluteScreen(int screenX, int screenY);
    bool Button(MouseButtonType button, bool down);
    bool Wheel(int delta, bool horizontal);

    std::wstring LastError() const;

private:
    HidInterceptionBackend() = default;
    bool EnsureApiLoaded(std::wstring* errorOut);
    bool ResolveDevicesLocked(std::wstring* errorOut);
    bool SendMouseStrokeLocked(unsigned short state, unsigned short flags,
        short rolling, int x, int y);

    mutable std::mutex mutex_;
    void* context_ = nullptr;  // InterceptionContext
    int keyboardDevice_ = 0;
    int mouseDevice_ = 0;
    std::wstring lastError_;
    bool apiLoaded_ = false;
};
