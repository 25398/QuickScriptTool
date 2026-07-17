#pragma once

#include "window_mode_types.h"

#include <string>

namespace windowmode {

/// Manages the visible Windows Task View virtual desktop named「鼠标宏」.
class MacroVirtualDesktop {
public:
    MacroVirtualDesktop() = default;
    ~MacroVirtualDesktop();

    MacroVirtualDesktop(const MacroVirtualDesktop&) = delete;
    MacroVirtualDesktop& operator=(const MacroVirtualDesktop&) = delete;

    /// Find「鼠标宏」by name/cache; create it if missing.
    /// Must not leave the user's view on the macro desktop (Create/Move restore via GoTo).
    bool OpenOrCreate();
    void Close();
    bool IsValid() const;
    const GUID& DesktopId() const { return desktopId_; }
    int DesktopIndex() const { return desktopIndex_; }

    bool LaunchProcess(const std::wstring& exe, const std::wstring& args, PROCESS_INFORMATION& outPi,
        const std::wstring& titleContains = L"");
    bool MoveWindowToMacroDesktop(HWND hwnd);

    void SetCancelFlag(const std::atomic_bool* flag) { cancelFlag_ = flag; }

    const std::wstring& LastError() const { return lastError_; }

private:
    bool RefreshDesktopList();
    bool IsCancelled() const { return WindowModeCancelled(cancelFlag_); }

    GUID desktopId_{};
    int desktopIndex_ = -1;
    bool ready_ = false;
    std::wstring lastError_;
    const std::atomic_bool* cancelFlag_ = nullptr;
};

bool IsWindowOnVirtualDesktop(HWND hwnd, const GUID& desktopId);
bool IsWindowOnVirtualDesktopIndex(HWND hwnd, int desktopIndex);
bool GetWindowVirtualDesktopId(HWND hwnd, GUID& outId);

}  // namespace windowmode
