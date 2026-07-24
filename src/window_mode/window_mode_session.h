#pragma once

#include "hidden_desktop.h"
#include "window_mode_types.h"

#include <atomic>
#include <vector>

namespace windowmode {

class WindowModeSession {
public:
    ~WindowModeSession();

    bool Start(const WindowModeScriptConfig& config, std::wstring& err);
    void Stop();
    /// 仅清目标绑定，不关闭宏桌面（用于“找不到再启动”时避免虚拟桌面重建延迟）。
    void ClearTargetBinding();
    void SetCancelFlag(const std::atomic_bool* flag);
    void SetLaunchSearchDir(const std::wstring& dir) { launchSearchDir_ = dir; }
    bool RefreshTarget(std::wstring& err);

    bool LaunchTargetOnDesktop(std::wstring& err);
    bool LaunchTargetOnDefaultDesktop(std::wstring& err);
    bool ValidateTargetExe(std::wstring& err) const;
    bool CaptureTargetToFile(const std::wstring& path, std::wstring& err);

    const WindowModeSessionState& State() const { return state_; }
    const WindowModeScriptConfig& Config() const { return config_; }
    HiddenDesktop& Desktop() { return desktop_; }
    bool RefreshInputBinding(std::wstring& err);
    /// CDP：宏桌面被删则重建；目标偏离则再 Park（PreservingView）。
    bool EnsureMacroDesktopReady(std::wstring& err);

private:
    bool EnsureDesktop(std::wstring& err);
    bool BindTargetWindow(std::wstring& err);
    void ReleaseLaunchedProcess();
    void SaveTargetTopPlacementIfNeeded(HWND top);
    void RestoreSavedTargetTopPlacement();
    bool IsCancelled() const { return WindowModeCancelled(cancelFlag_); }

    HiddenDesktop desktop_;
    WindowModeScriptConfig config_;
    WindowModeSessionState state_;
    PROCESS_INFORMATION launchedProcess_{};
    bool ownsLaunchedProcess_ = false;
    /// 启动 CreateProcess 前已存在的同名主窗，绑定阶段禁止挪这些窗。
    std::vector<HWND> preLaunchWindows_;
    FILETIME launchTimeUtc_{};
    bool hasLaunchTimeUtc_ = false;
    const std::atomic_bool* cancelFlag_ = nullptr;
    std::wstring launchSearchDir_;
    HWND savedTargetTopHwnd_ = nullptr;
    WINDOWPLACEMENT savedTargetTopWp_{};
    bool hasSavedTargetTopPlacement_ = false;
};

}  // namespace windowmode
