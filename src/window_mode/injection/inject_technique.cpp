#include "inject_technique.h"

#include "inject_common.h"
#include "inject_peb_hide.h"
#include "inject_techniques_internal.h"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace windowmode {
namespace inject {
namespace {

struct TechniqueInfo {
    const wchar_t* name;
    const wchar_t* description;
};

const TechniqueInfo kTechniques[] = {
    { L"classic", L"CreateRemoteThread + LoadLibraryW（基线，反作弊最常见检测点）" },
    { L"ntcreatethreadex", L"NtCreateThreadEx + LoadLibraryW（绕过 CreateRemoteThread 用户态钩子）" },
    { L"apc", L"QueueUserAPC + LoadLibraryW（不新建线程，需目标线程可告警等待）" },
    { L"threadhijack", L"挂起线程 + SetThreadContext 执行 LoadLibraryW 桩，随后恢复原上下文" },
    { L"manualmap", L"手动映射 PE（不经 LoadLibrary，模块列表不可见；入口经远程线程调用）" },
    { L"manualmapxor", L"XOR 加密载荷文件，内存解密后手动映射（对抗静态文件扫描）" },
    { L"setwindowshook", L"SetWindowsHookEx(WH_GETMESSAGE) 窗口消息钩子注入（窗口模式经典路径）" },
    { L"manualmaphijack", L"复合：手动映射 + 线程劫持入口（不新建线程，模块列表不可见）" },
    { L"manualmaphijackxor", L"复合：XOR 载荷 + 手动映射 + 线程劫持入口（最隐蔽组合）" },
    { L"imagemap", L"SEC_IMAGE 映像节映射（MEM_IMAGE，内存扫描看起来像正常加载的镜像）" },
    { L"imagemaphijack", L"复合：映像节映射 + 线程劫持入口（镜像区域 + 不新建线程）" },
};
static_assert(sizeof(kTechniques) / sizeof(kTechniques[0]) ==
              static_cast<size_t>(Technique::Count),
              "technique table size mismatch");

bool IsLoadLibraryBased(Technique t) {
    return t == Technique::ClassicRemoteThread || t == Technique::NtCreateThreadEx ||
           t == Technique::ApcQueue || t == Technique::ThreadHijack ||
           t == Technique::SetWindowsHook;
}

}  // namespace

const wchar_t* TechniqueName(Technique t) {
    const int i = static_cast<int>(t);
    if (i < 0 || i >= TechniqueCount()) return L"unknown";
    return kTechniques[i].name;
}

const wchar_t* TechniqueDescription(Technique t) {
    const int i = static_cast<int>(t);
    if (i < 0 || i >= TechniqueCount()) return L"";
    return kTechniques[i].description;
}

int TechniqueCount() {
    return static_cast<int>(sizeof(kTechniques) / sizeof(kTechniques[0]));
}

bool ParseTechnique(const std::wstring& name, Technique& out) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    for (int i = 0; i < TechniqueCount(); ++i) {
        if (lower == kTechniques[i].name) {
            out = static_cast<Technique>(i);
            return true;
        }
    }
    return false;
}

Technique TechniqueFromInt(int v) {
    if (v < 0 || v >= TechniqueCount()) {
        return Technique::ClassicRemoteThread;
    }
    return static_cast<Technique>(v);
}

bool CallRemoteExport(HANDLE process, DWORD pid, HMODULE moduleBase,
                      const std::wstring& localDllPath, const char* exportName,
                      void* arg, DWORD timeoutMs, DWORD* exitCode,
                      std::wstring& err) {
    DWORD rva = 0;
    std::vector<uint8_t> pe;
    std::wstring peErr;
    if (detail::ReadFileBytes(localDllPath, pe, peErr)
        && detail::FindExportRva(pe, exportName, rva, peErr)) {
        void* remoteFn = reinterpret_cast<BYTE*>(moduleBase) + rva;
        return detail::RunRemoteThread(process, remoteFn, arg, timeoutMs,
                                       exitCode, err);
    }
    std::wstring mapErr;
    if (detail::FindExportRvaByLocalImage(localDllPath, exportName, rva, mapErr)) {
        void* remoteFn = reinterpret_cast<BYTE*>(moduleBase) + rva;
        return detail::RunRemoteThread(process, remoteFn, arg, timeoutMs,
                                       exitCode, err);
    }
    const std::wstring baseName = detail::BaseNameOnly(localDllPath);
    const uintptr_t remote = detail::ResolveRemoteProcAddress(
        process, pid, baseName.c_str(), exportName, err);
    if (remote != 0) {
        return detail::RunRemoteThread(process, reinterpret_cast<void*>(remote),
                                       arg, timeoutMs, exitCode, err);
    }
    err = peErr.empty() ? mapErr : peErr;
    if (err.empty()) err = L"未找到导出";
    return false;
}

bool InjectDll(DWORD pid, const std::wstring& dllPath, Technique tech,
               const InjectOptions& opts, InjectResult& out) {
    out = InjectResult{};
    if (pid == 0 || dllPath.empty()) {
        out.detail = L"参数无效";
        return false;
    }
    if (static_cast<int>(tech) < 0 || static_cast<int>(tech) >= TechniqueCount()) {
        out.detail = L"未知注入技术";
        return false;
    }
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        out.detail = L"DLL 文件不存在: " + dllPath;
        return false;
    }

    // 各技术所需进程访问权
    const bool needsNewThread = tech == Technique::ClassicRemoteThread
        || tech == Technique::NtCreateThreadEx
        || tech == Technique::ManualMap || tech == Technique::ManualMapXor
        || tech == Technique::ImageMap;
    const DWORD extra = needsNewThread ? PROCESS_CREATE_THREAD : 0;

    HANDLE process = nullptr;
    if (!detail::OpenTarget(pid, extra, process, out.detail)) return false;

    HMODULE remoteModule = nullptr;
    void* mappedBase = nullptr;
    void* entryRegion = nullptr;
    std::wstring techErr;
    bool ok = false;

    switch (tech) {
        case Technique::ClassicRemoteThread:
            ok = InjectLoadLibraryRemoteThread(process, pid, dllPath,
                                               remoteModule, techErr);
            break;
        case Technique::NtCreateThreadEx:
            ok = InjectLoadLibraryNtCreateThreadEx(process, pid, dllPath,
                                                   remoteModule, techErr);
            break;
        case Technique::ApcQueue:
            ok = InjectLoadLibraryApc(process, pid, dllPath,
                                      remoteModule, techErr);
            break;
        case Technique::ThreadHijack:
            ok = InjectLoadLibraryThreadHijack(process, pid, dllPath,
                                               remoteModule, techErr);
            break;
        case Technique::ManualMap:
            ok = InjectManualMap(process, pid, dllPath,
                                 mappedBase, entryRegion, techErr);
            break;
        case Technique::ManualMapXor: {
            std::vector<uint8_t> encrypted;
            if (detail::ReadFileBytes(dllPath, encrypted, techErr)) {
                std::vector<uint8_t> plain;
                detail::XorBytes(encrypted, opts.xorKey, plain);
                ok = InjectManualMapBytes(process, pid, plain,
                                          mappedBase, entryRegion, false, techErr);
            }
            break;
        }
        case Technique::ManualMapHijack:
            ok = InjectManualMapHijack(process, pid, dllPath,
                                       mappedBase, entryRegion, techErr);
            break;
        case Technique::ManualMapHijackXor: {
            std::vector<uint8_t> encrypted;
            if (detail::ReadFileBytes(dllPath, encrypted, techErr)) {
                std::vector<uint8_t> plain;
                detail::XorBytes(encrypted, opts.xorKey, plain);
                ok = InjectManualMapBytes(process, pid, plain,
                                          mappedBase, entryRegion, true, techErr);
            }
            break;
        }
        case Technique::ImageMap:
            ok = InjectImageMap(process, pid, dllPath,
                                mappedBase, entryRegion, techErr);
            break;
        case Technique::ImageMapHijack:
            ok = InjectImageMapHijack(process, pid, dllPath,
                                      mappedBase, entryRegion, techErr);
            break;
        case Technique::SetWindowsHook:
            ok = InjectSetWindowsHook(process, pid, dllPath, opts,
                                      remoteModule, techErr);
            break;
        default:
            techErr = L"未实现的技术";
            break;
    }

    if (!ok) {
        out.detail = techErr.empty()
            ? std::wstring(L"注入失败: ") + TechniqueName(tech)
            : techErr;
        CloseHandle(process);
        return false;
    }

    out.remoteModule = remoteModule;
    out.entryRegion = entryRegion;
    if (mappedBase) out.remoteModule = static_cast<HMODULE>(mappedBase);

    // LoadLibrary 系注入后按需从 PEB 模块链表摘除
    if (opts.hideModule && IsLoadLibraryBased(tech) && out.remoteModule) {
        HiddenModuleState hideState{};
        std::wstring hideErr;
        if (HideModuleFromPeb(process, pid, out.remoteModule,
                              hideState, hideErr)) {
            out.moduleHidden = true;
            out.hideState = hideState;
            out.detail = L"模块已从 PEB 链表摘除（Toolhelp 枚举不可见）";
        } else {
            out.detail = L"注入成功，但 PEB 隐藏失败: " + hideErr;
        }
    }
    if (out.detail.empty()) {
        out.detail = L"注入成功（" + std::wstring(TechniqueName(tech)) + L"）";
    }
    if (tech == Technique::ManualMap || tech == Technique::ManualMapXor ||
        tech == Technique::ManualMapHijack ||
        tech == Technique::ManualMapHijackXor ||
        tech == Technique::ImageMap || tech == Technique::ImageMapHijack) {
        wchar_t buf[160]{};
        swprintf_s(buf, L"；映射基址=0x%p 入口桩=0x%p",
            out.remoteModule, out.entryRegion);
        out.detail += buf;
    }
    CloseHandle(process);
    return true;
}

}  // namespace inject
}  // namespace windowmode
