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

    /// 仅变速注入：DLL 只装时钟 IAT 补丁，不装任何假焦点钩。
    /// 「启用窗口变速」开而「启用假焦点注入」关时用，键鼠路径保持不变。
    void SetTimeScaleOnly(bool only) { timeScaleOnly_ = only; }
    bool time_scale_only() const { return timeScaleOnly_; }

    /// 只拆假焦点钩、保留 DLL 与时钟轮询（全屏游戏拆钩但继续变速用）。
    /// 成功后会置 `fake_focus_disabled()`，让输入路径判定立刻回到「假焦点不可用」。
    bool DisableFakeFocusKeepTimeScale(std::wstring& err);

    /// 假焦点钩是否**真的**可用。仅变速注入、或钩已被单独拆掉，都返回 false。
    /// 输入路径判定一律用它，别用 IsInjected()。
    bool fake_focus_active() const {
        return IsInjected() && !timeScaleOnly_ && !fakeFocusDisabled_;
    }

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

    /// 读取目标进程内 FakeFocus_TimeScaleDiag（远程调用导出）。
    /// 这是唯一能证明「**跨进程**注入后变速真的在靶进程里生效」的手段：
    /// 自检进程内 LoadLibrary 的用例覆盖不到远程注入 + 远程调用这条产品真实路径。
    bool QueryTimeScaleDiag(DWORD& value, std::wstring& err);

private:
    struct Target {
        DWORD pid = 0;
        HANDLE process = nullptr;
        HMODULE remoteModule = nullptr;
        std::wstring dllPath;
        bool installed = false;
        bool moduleHidden = false;
        inject::HiddenModuleState hideState;
        /// SetWindowsHook 路径专用（其余技术为 null）：本地钩子模块 + HHOOK。
        /// 卸载时必须 `UnhookWindowsHookEx` + `FreeLibrary` —— 钩子在，user32 就把
        /// DLL 钉在目标进程里，远程 FreeLibrary 归不了零 ⇒ 文件锁到目标退出。
        HMODULE hookModule = nullptr;
        void* hookHandle = nullptr;
        /// 模块是经 LoadLibrary 装进目标的（⇒ 卸载要按模块存在性循环 FreeLibrary）。
        /// 手动映射/映像节映射不注册进 loader，FreeLibrary 对它们没有意义。
        bool loaderLoaded = false;
    };

    bool InjectOne(DWORD pid, HWND targetTop, const std::wstring& dllPath, std::wstring& err,
        bool lite);
    bool ResolveDllPathForPid(DWORD pid, std::wstring& outPath, std::wstring& err) const;
    bool RemoteCallExport(Target& t, const char* exportName, HWND arg, std::wstring& err);
    /// 远程 `FreeLibrary(t.remoteModule)` 一次。返回 false = 这次没卸下
    /// （引用计数没到 0 / 线程起不来），调用方据此停止循环。
    bool FreeRemoteModuleOnce(Target& t);
    void UnloadOne(Target& t);
    static std::vector<DWORD> CollectInjectPids(DWORD windowPid, HWND targetTop);

    std::vector<Target> targets_;
    DWORD windowPid_ = 0;
    std::wstring lastError_;
    std::wstring dllPath_;
    inject::Technique technique_ = inject::Technique::ClassicRemoteThread;
    bool hideModule_ = false;
    bool timeScaleOnly_ = false;
    /// 假焦点钩已被单独拆掉（DLL 还在，时钟还在）。
    bool fakeFocusDisabled_ = false;
};

}  // namespace windowmode
