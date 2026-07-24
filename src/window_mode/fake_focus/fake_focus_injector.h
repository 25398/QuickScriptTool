#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace windowmode {

/// Injects FakeFocus32/64.dll into the target process tree and drives Install/Update/Uninstall.
/// Browser games (Edge/Chrome/Flash) often poll GetAsyncKeyState in child processes —
/// we inject the window PID plus its direct child processes.
class FakeFocusInjector {
public:
    FakeFocusInjector() = default;
    ~FakeFocusInjector();

    FakeFocusInjector(const FakeFocusInjector&) = delete;
    FakeFocusInjector& operator=(const FakeFocusInjector&) = delete;

    /// Inject + Install into windowPid and related child processes.
    /// Soft-input mapping is keyed by windowPid. On failure, lastError() is set;
    /// never falls back to foreground steal.
    bool InjectAndInstall(DWORD windowPid, HWND targetTop, std::wstring& err);

    /// Call FakeFocus_UpdateTarget in every injected process when HWND changes.
    bool UpdateTarget(HWND targetTop, std::wstring& err);

    /// Uninstall hooks and FreeLibrary in all injected processes. Idempotent.
    void Unload();

    bool IsInjected() const { return !targets_.empty(); }
    const std::wstring& LastError() const { return lastError_; }

private:
    struct Target {
        DWORD pid = 0;
        HANDLE process = nullptr;
        HMODULE remoteModule = nullptr;
        bool installed = false;
    };

    bool InjectOne(DWORD pid, HWND targetTop, const std::wstring& dllPath, std::wstring& err);
    bool ResolveDllPathForPid(DWORD pid, std::wstring& outPath, std::wstring& err) const;
    bool RemoteLoadLibrary(Target& t, const std::wstring& dllPath, std::wstring& err);
    bool RemoteCallExport(Target& t, const char* exportName, HWND arg, std::wstring& err);
    HMODULE FindRemoteModule(DWORD pid, HANDLE process, const std::wstring& dllPath) const;
    void UnloadOne(Target& t);
    static std::vector<DWORD> CollectInjectPids(DWORD windowPid, HWND targetTop);

    std::vector<Target> targets_;
    DWORD windowPid_ = 0;
    std::wstring lastError_;
    std::wstring dllPath_;
};

}  // namespace windowmode
