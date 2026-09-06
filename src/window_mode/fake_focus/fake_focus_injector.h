#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "window_mode/injection/inject_peb_hide.h"
#include "window_mode/injection/inject_technique.h"

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
    bool InjectAndInstall(DWORD windowPid, HWND targetTop, std::wstring& err,
        bool lite = false, bool windowPidOnly = false);

    /// 选择注入技术（默认 ClassicRemoteThread，保持原有行为）。
    void SetInjectionTechnique(inject::Technique t) { technique_ = t; }
    inject::Technique injection_technique() const { return technique_; }

    /// LoadLibrary 系注入后是否从 PEB 模块链表摘除（测试模块枚举检测）。
    void SetHideModule(bool hide) { hideModule_ = hide; }
    bool hide_module() const { return hideModule_; }

    /// Call FakeFocus_UpdateTarget in every injected process when HWND changes.
    bool UpdateTarget(HWND targetTop, std::wstring& err);

    /// Uninstall hooks and FreeLibrary in all injected processes. Idempotent.
    void Unload();

    bool IsInjected() const { return !targets_.empty(); }
    const std::wstring& LastError() const { return lastError_; }

    /// 读取目标进程内 FakeFocus_MapleIatCount。低 16 位=轮询槽，高 16 位=diag；远程失败则 false。
    bool QueryMapleIatCount(DWORD& count, std::wstring& err);

    /// 读取目标进程内 FakeFocus_MapleHookHits（按键后再查：gaks/diState/diData/lastCb）。
    bool QueryMapleHookHits(DWORD& hits, std::wstring& err);

private:
    struct Target {
        DWORD pid = 0;
        HANDLE process = nullptr;
        HMODULE remoteModule = nullptr;
        std::wstring dllPath;
        bool installed = false;
        bool moduleHidden = false;
        inject::HiddenModuleState hideState;
    };

    bool InjectOne(DWORD pid, HWND targetTop, const std::wstring& dllPath, std::wstring& err,
        bool lite);
    bool ResolveDllPathForPid(DWORD pid, std::wstring& outPath, std::wstring& err) const;
    bool RemoteCallExport(Target& t, const char* exportName, HWND arg, std::wstring& err);
    void UnloadOne(Target& t);
    static std::vector<DWORD> CollectInjectPids(DWORD windowPid, HWND targetTop);

    std::vector<Target> targets_;
    DWORD windowPid_ = 0;
    std::wstring lastError_;
    std::wstring dllPath_;
    inject::Technique technique_ = inject::Technique::ClassicRemoteThread;
    bool hideModule_ = false;
};

}  // namespace windowmode
