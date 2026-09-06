#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace windowmode {
namespace inject {

// 记录摘除前三条链表的 Flink/Blink，便于恢复后安全 FreeLibrary。
struct HiddenModuleState {
    bool active = false;
    uintptr_t entry = 0;          // LDR_DATA_TABLE_ENTRY 地址
    uintptr_t loadLinks[2] = {};  // [Flink, Blink]
    uintptr_t memLinks[2] = {};
    uintptr_t initLinks[2] = {};
};

// 将 moduleBase 从目标进程 PEB 的 LoadOrder/MemoryOrder/InitializationOrder
// 三条链表摘除（Toolhelp/EnumProcessModules 将不再枚举到该模块）。
// 完全在注入方进程内完成（NtQueryInformationProcess + Read/WriteProcessMemory）。
bool HideModuleFromPeb(HANDLE process, DWORD pid, HMODULE moduleBase,
                       HiddenModuleState& out, std::wstring& err);

// 恢复三条链表（Unload 前调用，使 FreeLibrary 安全）。
bool RestoreModuleFromPeb(HANDLE process, DWORD pid, HMODULE moduleBase,
                          const HiddenModuleState& st, std::wstring& err);

}  // namespace inject
}  // namespace windowmode
