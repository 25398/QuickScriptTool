#pragma once

// 各注入技术实现（仅 inject_technique.cpp 调用）

#include <windows.h>

#include <string>
#include <vector>

#include "inject_technique.h"

namespace windowmode {
namespace inject {

bool InjectLoadLibraryRemoteThread(HANDLE process, DWORD pid,
    const std::wstring& dllPath, HMODULE& outModule, std::wstring& err);

bool InjectLoadLibraryNtCreateThreadEx(HANDLE process, DWORD pid,
    const std::wstring& dllPath, HMODULE& outModule, std::wstring& err);

bool InjectLoadLibraryApc(HANDLE process, DWORD pid,
    const std::wstring& dllPath, HMODULE& outModule, std::wstring& err);

bool InjectLoadLibraryThreadHijack(HANDLE process, DWORD pid,
    const std::wstring& dllPath, HMODULE& outModule, std::wstring& err);

// 让目标一个工作线程执行 fn(a1,a2,a3)，桩自动恢复原上下文（手动映射入口复用）。
bool HijackThreadCall(HANDLE process, DWORD pid, uintptr_t fn,
    uintptr_t a1, uintptr_t a2, uintptr_t a3, DWORD waitMs,
    uintptr_t& outCodeBuf, std::wstring& err);

bool InjectManualMap(HANDLE process, DWORD pid, const std::wstring& dllPath,
    void*& outBase, void*& outEntryRegion, std::wstring& err);

bool InjectManualMapHijack(HANDLE process, DWORD pid, const std::wstring& dllPath,
    void*& outBase, void*& outEntryRegion, std::wstring& err);

bool InjectImageMap(HANDLE process, DWORD pid, const std::wstring& dllPath,
    void*& outBase, void*& outEntryRegion, std::wstring& err);

bool InjectImageMapHijack(HANDLE process, DWORD pid, const std::wstring& dllPath,
    void*& outBase, void*& outEntryRegion, std::wstring& err);

// useHijack=true 时入口经线程劫持调用（不新建线程）。
bool InjectManualMapBytes(HANDLE process, DWORD pid,
    const std::vector<uint8_t>& peBytes, void*& outBase,
    void*& outEntryRegion, bool useHijack, std::wstring& err);

bool InjectSetWindowsHook(HANDLE process, DWORD pid,
    const std::wstring& dllPath, const InjectOptions& opts,
    HMODULE& outModule, std::wstring& err);

}  // namespace inject
}  // namespace windowmode
