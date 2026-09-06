#include "inject_peb_hide.h"

#include "inject_common.h"

#include <cstring>

namespace windowmode {
namespace inject {
namespace {

constexpr size_t kPtr32 = 4;
constexpr size_t kPtr64 = 8;

bool ReadPtr(HANDLE process, uintptr_t addr, size_t width,
             uintptr_t& out, std::wstring& err) {
    uint64_t raw = 0;
    const SIZE_T n = width;
    if (!detail::ReadRemoteBytes(process, reinterpret_cast<const void*>(addr),
                                 &raw, n, err)) {
        return false;
    }
    out = static_cast<uintptr_t>(raw);
    return true;
}

bool WritePtr(HANDLE process, uintptr_t addr, size_t width,
              uintptr_t value, std::wstring& err) {
    uint64_t raw = static_cast<uint64_t>(value);
    return detail::WriteRemoteBytes(process, reinterpret_cast<void*>(addr),
                                    &raw, width, err);
}

bool UnlinkNode(HANDLE process, uintptr_t nodeAddr, size_t width,
                uintptr_t* savedLinks, std::wstring& err) {
    uintptr_t flink = 0, blink = 0;
    if (!ReadPtr(process, nodeAddr, width, flink, err)) return false;
    if (!ReadPtr(process, nodeAddr + width, width, blink, err)) return false;
    // flink->Blink = blink ; blink->Flink = flink
    if (!WritePtr(process, flink + width, width, blink, err)) return false;
    if (!WritePtr(process, blink, width, flink, err)) return false;
    if (savedLinks) {
        savedLinks[0] = flink;
        savedLinks[1] = blink;
    }
    return true;
}

bool RelinkNode(HANDLE process, uintptr_t nodeAddr, size_t width,
                const uintptr_t* savedLinks, std::wstring& err) {
    const uintptr_t flink = savedLinks[0];
    const uintptr_t blink = savedLinks[1];
    if (!WritePtr(process, flink + width, width, nodeAddr, err)) return false;
    if (!WritePtr(process, blink, width, nodeAddr, err)) return false;
    if (!WritePtr(process, nodeAddr, width, flink, err)) return false;
    if (!WritePtr(process, nodeAddr + width, width, blink, err)) return false;
    return true;
}

}  // namespace

bool HideModuleFromPeb(HANDLE process, DWORD pid, HMODULE moduleBase,
                       HiddenModuleState& out, std::wstring& err) {
    out = HiddenModuleState{};
    (void)pid;
    if (!process || !moduleBase) {
        err = L"HideModuleFromPeb 参数无效";
        return false;
    }

    uintptr_t peb = 0;
    bool wow64 = false;
    if (!detail::QueryRemotePeb(process, peb, wow64, err)) return false;
    const size_t width = wow64 ? kPtr32 : kPtr64;

    // PEB->Ldr
    uintptr_t ldr = 0;
    if (!ReadPtr(process, peb + (wow64 ? 0x0C : 0x18), width, ldr, err)) return false;
    if (!ldr) {
        err = L"目标 PEB->Ldr 为空";
        return false;
    }

    // 三条链表头
    const uintptr_t loadHead = ldr + (wow64 ? 0x0C : 0x10);

    // 遍历 InLoadOrder 找到 DllBase == moduleBase 的条目
    const uintptr_t loadLinksOff = 0;
    const uintptr_t memLinksOff = wow64 ? 0x08 : 0x10;
    const uintptr_t initLinksOff = wow64 ? 0x10 : 0x20;
    const uintptr_t dllBaseOff = wow64 ? 0x18 : 0x30;

    uintptr_t entry = 0;
    uintptr_t node = 0;
    if (!ReadPtr(process, loadHead, width, node, err)) return false;
    for (int guard = 0; guard < 4096 && node != loadHead; ++guard) {
        if (node == 0) break;
        uintptr_t base = 0;
        if (!ReadPtr(process, node - loadLinksOff + dllBaseOff, width, base, err)) {
            return false;
        }
        if (base == reinterpret_cast<uintptr_t>(moduleBase)) {
            entry = node - loadLinksOff;
            break;
        }
        if (!ReadPtr(process, node, width, node, err)) return false;
    }
    if (!entry) {
        err = L"在 PEB 模块链表中未找到目标模块";
        return false;
    }

    const uintptr_t loadNode = entry + loadLinksOff;
    const uintptr_t memNode = entry + memLinksOff;
    const uintptr_t initNode = entry + initLinksOff;

    if (!UnlinkNode(process, loadNode, width, out.loadLinks, err)) return false;
    if (!UnlinkNode(process, memNode, width, out.memLinks, err)) return false;
    if (!UnlinkNode(process, initNode, width, out.initLinks, err)) return false;

    out.active = true;
    out.entry = entry;
    return true;
}

bool RestoreModuleFromPeb(HANDLE process, DWORD pid, HMODULE moduleBase,
                          const HiddenModuleState& st, std::wstring& err) {
    if (!st.active || !process || !moduleBase) return true;
    (void)pid;
    (void)moduleBase;
    bool wow64 = false;
    if (!detail::TargetIsWow64(process, wow64, err)) return false;
    const size_t width = wow64 ? kPtr32 : kPtr64;
    const uintptr_t entry = st.entry;
    const uintptr_t loadNode = entry;
    const uintptr_t memNode = entry + (wow64 ? 0x08 : 0x10);
    const uintptr_t initNode = entry + (wow64 ? 0x10 : 0x20);
    if (!RelinkNode(process, loadNode, width, st.loadLinks, err)) return false;
    if (!RelinkNode(process, memNode, width, st.memLinks, err)) return false;
    if (!RelinkNode(process, initNode, width, st.initLinks, err)) return false;
    return true;
}

}  // namespace inject
}  // namespace windowmode
