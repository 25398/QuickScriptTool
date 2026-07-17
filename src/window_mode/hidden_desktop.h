#pragma once

#include "macro_virtual_desktop.h"
#include "window_mode_types.h"

#include <string>

namespace windowmode {

/// Window-mode execution surface: a visible Windows virtual desktop named「鼠标宏」.
class HiddenDesktop {
public:
    HiddenDesktop() = default;
    ~HiddenDesktop();

    HiddenDesktop(const HiddenDesktop&) = delete;
    HiddenDesktop& operator=(const HiddenDesktop&) = delete;

    bool OpenOrCreate();
    void Close();
    bool IsValid() const;
    const GUID& DesktopId() const { return virtualDesktop_.DesktopId(); }
    int DesktopIndex() const { return virtualDesktop_.DesktopIndex(); }

    bool LaunchProcess(const std::wstring& exe, const std::wstring& args, PROCESS_INFORMATION& outPi,
        const std::wstring& titleContains = L"");
    bool MoveWindowToMacroDesktop(HWND hwnd) { return virtualDesktop_.MoveWindowToMacroDesktop(hwnd); }

    void SetCancelFlag(const std::atomic_bool* flag) { virtualDesktop_.SetCancelFlag(flag); }

    /// No-op: virtual desktop mode does not bind threads to a Win32 desktop.
    bool AttachCurrentThread() { return IsValid(); }
    bool DetachCurrentThread() { return true; }
    bool IsThreadAttached() const { return false; }

    const std::wstring& LastError() const { return virtualDesktop_.LastError(); }

private:
    MacroVirtualDesktop virtualDesktop_;
};

}  // namespace windowmode
