#pragma once

// =============================================================================
// 注入技术库（对抗性测试框架）
// -----------------------------------------------------------------------------
// 用途：为窗口/后台窗口模式 DLL 注入提供多种注入技术，用于验证游戏/反作弊系统
// 对各类注入路径的识别与拦截能力（授权对抗性测试）。
//
// 技术矩阵与检测面详见 docs/anticheat-injection-testing.md。
// =============================================================================

#include <windows.h>

#include <string>

#include "inject_peb_hide.h"

namespace windowmode {
namespace inject {

enum class Technique {
    ClassicRemoteThread = 0,  // CreateRemoteThread + LoadLibraryW（基线，最易检测）
    NtCreateThreadEx,         // NtCreateThreadEx + LoadLibraryW（绕过 CreateRemoteThread 用户态钩子）
    ApcQueue,                 // QueueUserAPC + LoadLibraryW（不新建线程）
    ThreadHijack,             // 挂起现有线程 + SetThreadContext -> LoadLibraryW（不新建线程）
    ManualMap,                // 手动映射 PE（不经 LoadLibrary，模块列表不可见）
    ManualMapXor,             // XOR 加密载荷文件 + 内存解密 + 手动映射
    SetWindowsHook,           // SetWindowsHookEx 窗口消息钩子注入（窗口/后台窗口模式经典路径）
    ManualMapHijack,          // 复合：手动映射 + 线程劫持入口（不新建线程、模块列表不可见）
    ManualMapHijackXor,       // 复合：XOR 载荷 + 手动映射 + 线程劫持入口（最隐蔽组合）
    ImageMap,                 // SEC_IMAGE 映像节映射（MEM_IMAGE，内存扫描看起来像正常镜像）
    ImageMapHijack,           // 复合：映像节映射 + 线程劫持入口（镜像区域 + 不新建线程）
    Count,
};

struct InjectOptions {
    bool hideModule = false;      // LoadLibrary 系注入后从 PEB 模块链表摘除（测试模块枚举检测）
    DWORD timeoutMs = 15000;      // 注入/等待模块出现超时
    HWND targetTop = nullptr;     // SetWindowsHook：目标窗口（取其属主线程）
    DWORD hookThreadId = 0;       // SetWindowsHook：显式指定线程（优先于 targetTop）
    std::string hookProcName;     // SetWindowsHook：DLL 导出的钩子过程名（必须）
    uint8_t xorKey = 0x5A;        // ManualMapXor 解密密钥
};

struct InjectResult {
    bool ok = false;
    HMODULE remoteModule = nullptr;  // LoadLibrary 系 = 模块基址；手动映射 = 映射基址
    void* entryRegion = nullptr;     // 手动映射：入口调用 stub 所在内存区域
    bool moduleHidden = false;       // 是否已从 PEB 链表摘除
    HiddenModuleState hideState;     // PEB 隐藏前快照（卸载前需 Restore）
    /// ⚠ SetWindowsHook 专用：本地 LoadLibrary 出来的模块 + 装上去的钩子句柄。
    /// 调用方**必须**在卸载时 `UnhookWindowsHookEx(hookHandle)` 再
    /// `FreeLibrary(hookModule)`，否则：
    ///   ① user32 会把这个 DLL **钉在目标进程里**（远程 FreeLibrary 归不了零）
    ///      ⇒ 目标进程活着，那个 DLL 文件就一直被锁（安装/卸载要不重启电脑就动不了它）；
    ///   ② 钩子过程会一直在目标线程里跑下去。
    /// 旧的实现把这两个句柄丢在函数局部变量里 ⇒ 泄漏 + 永久文件锁，就是上面那条事故。
    HMODULE hookModule = nullptr;
    void* hookHandle = nullptr;
    std::wstring detail;
};

// 注入 DLL 到目标进程（按所选技术）。失败时 out.ok=false，out.detail 含原因。
bool InjectDll(DWORD pid, const std::wstring& dllPath, Technique tech,
               const InjectOptions& opts, InjectResult& out);

// 便于在 UI/引擎层使用：注入后继续远程调用 FakeFocus 导出（Install/Update/Uninstall）。
// 内部复用 InjectDll；moduleBase 为 InjectDll 成功后的 remoteModule。
bool CallRemoteExport(HANDLE process, DWORD pid, HMODULE moduleBase,
                      const std::wstring& localDllPath, const char* exportName,
                      void* arg, DWORD timeoutMs, DWORD* exitCode, std::wstring& err);

// 工具：名称/描述/解析
const wchar_t* TechniqueName(Technique t);
const wchar_t* TechniqueDescription(Technique t);
bool ParseTechnique(const std::wstring& name, Technique& out);
int TechniqueCount();

/// 该技术是把 DLL 经 LoadLibrary 装进目标的（⇒ 目标进程模块引用计数 +1、可按名枚举、
/// 卸载必须远程 FreeLibrary）。手动映射/映像节映射**不**在这个集合里：它们不注册进 loader，
/// FreeLibrary 对它们没有意义（调了也只是返回 FALSE），文件本身也不会被映射锁住。
bool IsLoadLibraryBasedTechnique(Technique t);

/// 设置持久化索引 → 技术（越界时钳制到合法范围；0 = ClassicRemoteThread）。
Technique TechniqueFromInt(int v);

}  // namespace inject
}  // namespace windowmode
