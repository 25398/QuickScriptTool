#pragma once

#include <windows.h>

#include <string>

namespace windowmode {

/// Loads VirtualDesktopAccessor.dll (bundled, version-selected) and exposes its API.
class VirtualDesktopAccessor {
public:
    static VirtualDesktopAccessor& Instance();

    bool EnsureLoaded(std::wstring& err);
    void Unload();
    bool IsLoaded() const { return module_ != nullptr; }
    const std::wstring& LoadedDllName() const { return loadedDllName_; }

    int GetDesktopCount() const;
    std::wstring GetDesktopName(int desktopNumber) const;
    bool SetDesktopName(int desktopNumber, const std::wstring& name) const;
    int CreateDesktop() const;
    int CreateDesktopPreservingView() const;
    bool MoveWindowToDesktopNumber(HWND hwnd, int desktopNumber) const;
    bool MoveWindowToDesktopNumberPreservingView(HWND hwnd, int desktopNumber) const;
    int GetCurrentDesktopNumber() const;
    bool GoToDesktopNumber(int desktopNumber) const;
    int GetWindowDesktopNumber(HWND hwnd) const;
    bool IsWindowOnDesktopNumber(HWND hwnd, int desktopNumber) const;
    /// 1=在当前桌面, 0=不在, -1=失败
    int IsWindowOnCurrentVirtualDesktop(HWND hwnd) const;
    bool GetDesktopIdByNumber(int desktopNumber, GUID& outId) const;
    bool GetWindowDesktopId(HWND hwnd, GUID& outId) const;

    /// Trim-aware name compare against a desktop index.
    bool DesktopNameMatches(int desktopNumber, const std::wstring& expectedName) const;
    /// Find first desktop whose name matches (VDA API + registry fallback). Returns -1 if not found.
    int FindDesktopIndexByName(const std::wstring& expectedName) const;
    bool FindDesktopIndexByGuid(const GUID& desktopId, int& outIndex) const;

    int IsPinnedWindow(HWND hwnd) const;
    bool PinWindow(HWND hwnd) const;
    bool UnPinWindow(HWND hwnd) const;

private:
    VirtualDesktopAccessor() = default;

    bool TryLoadDll(const std::wstring& path, std::wstring& err);
    void ResolveFunctions();
    std::wstring BuildCandidatePath(const wchar_t* fileName) const;

    HMODULE module_ = nullptr;
    std::wstring loadedDllName_;

    using FnGetDesktopCount = int(__stdcall*)();
    using FnGetDesktopName = int(__stdcall*)(int, char*, size_t);
    using FnSetDesktopName = int(__stdcall*)(int, const char*);
    using FnCreateDesktop = int(__stdcall*)();
    using FnMoveWindowToDesktopNumber = int(__stdcall*)(HWND, int);
    using FnGetWindowDesktopNumber = int(__stdcall*)(HWND);
    using FnIsWindowOnDesktopNumber = int(__stdcall*)(HWND, int);
    using FnIsWindowOnCurrentVirtualDesktop = int(__stdcall*)(HWND);
    using FnGetDesktopIdByNumber = GUID(__stdcall*)(int);
    using FnGetWindowDesktopId = GUID(__stdcall*)(HWND);
    using FnGetCurrentDesktopNumber = int(__stdcall*)();
    using FnGoToDesktopNumber = int(__stdcall*)(int);
    using FnIsPinnedWindow = int(__stdcall*)(HWND);
    using FnPinWindow = int(__stdcall*)(HWND);
    using FnUnPinWindow = int(__stdcall*)(HWND);

    FnGetDesktopCount getDesktopCount_ = nullptr;
    FnGetDesktopName getDesktopName_ = nullptr;
    FnSetDesktopName setDesktopName_ = nullptr;
    FnCreateDesktop createDesktop_ = nullptr;
    FnMoveWindowToDesktopNumber moveWindowToDesktopNumber_ = nullptr;
    FnGetWindowDesktopNumber getWindowDesktopNumber_ = nullptr;
    FnIsWindowOnDesktopNumber isWindowOnDesktopNumber_ = nullptr;
    FnIsWindowOnCurrentVirtualDesktop isWindowOnCurrentVirtualDesktop_ = nullptr;
    FnGetDesktopIdByNumber getDesktopIdByNumber_ = nullptr;
    FnGetWindowDesktopId getWindowDesktopId_ = nullptr;
    FnGetCurrentDesktopNumber getCurrentDesktopNumber_ = nullptr;
    FnGoToDesktopNumber goToDesktopNumber_ = nullptr;
    FnIsPinnedWindow isPinnedWindow_ = nullptr;
    FnPinWindow pinWindow_ = nullptr;
    FnUnPinWindow unPinWindow_ = nullptr;
};

}  // namespace windowmode
