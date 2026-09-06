#include "inject_common.h"
#include "inject_techniques_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <vector>

namespace windowmode {
namespace inject {
namespace {

// IMAGE_THUNK_DATA 读取（宽 4/8），返回值及是否序号导入
struct ThunkValue {
    uint64_t raw = 0;
    bool byOrdinal = false;
    uint16_t ordinal = 0;
    uint32_t nameRva = 0;
};

ThunkValue ReadThunk(const uint8_t* p, bool x64) {
    ThunkValue v;
    if (x64) {
        std::memcpy(&v.raw, p, 8);
        v.byOrdinal = (v.raw & 0x8000000000000000ULL) != 0;
        v.ordinal = static_cast<uint16_t>(v.raw & 0xFFFF);
        v.nameRva = static_cast<uint32_t>(v.raw & 0x7FFFFFFF);
    } else {
        uint32_t raw = 0;
        std::memcpy(&raw, p, 4);
        v.raw = raw;
        v.byOrdinal = (raw & 0x80000000u) != 0;
        v.ordinal = static_cast<uint16_t>(raw & 0xFFFF);
        v.nameRva = raw & 0x7FFFFFFFu;
    }
    return v;
}

DWORD SectionProtect(DWORD chars) {
    const bool exec = (chars & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool read = (chars & IMAGE_SCN_MEM_READ) != 0;
    const bool write = (chars & IMAGE_SCN_MEM_WRITE) != 0;
    if (exec) {
        if (write) return PAGE_EXECUTE_READWRITE;
        if (read) return PAGE_EXECUTE_READ;
        return PAGE_EXECUTE;
    }
    if (write) return PAGE_READWRITE;
    if (read) return PAGE_READONLY;
    return PAGE_READONLY;
}

// 在本地镜像中应用重定位
bool ApplyRelocations(std::vector<uint8_t>& image, const detail::PeFileView& view,
                      uintptr_t delta, std::wstring& err) {
    if (delta == 0) return true;
    const uint32_t relocRva = view.OptDirRva(5);
    const uint32_t relocSize = view.OptDirSize(5);
    if (relocRva == 0 || relocSize == 0) return true;  // 无重定位
    if (relocRva >= image.size()) {
        err = L"重定位表越界";
        return false;
    }
    const size_t end = std::min<size_t>(
        static_cast<size_t>(relocRva) + relocSize, image.size());
    size_t off = relocRva;
    while (off + 8 <= end) {
        const uint32_t pageRva = *reinterpret_cast<const uint32_t*>(image.data() + off);
        const uint32_t blockSize = *reinterpret_cast<const uint32_t*>(image.data() + off + 4);
        if (blockSize < 8 || off + blockSize > end) {
            err = L"重定位块大小异常";
            return false;
        }
        const size_t entries = (blockSize - 8) / 2;
        for (size_t i = 0; i < entries; ++i) {
            const uint16_t entry = *reinterpret_cast<const uint16_t*>(
                image.data() + off + 8 + i * 2);
            const uint16_t type = static_cast<uint16_t>(entry >> 12);
            const uint16_t offset = static_cast<uint16_t>(entry & 0xFFF);
            if (type == 0) continue;  // IMAGE_REL_BASED_ABSOLUTE
            const size_t addr = static_cast<size_t>(pageRva) + offset;
            if (addr + 8 > image.size()) {
                err = L"重定位目标越界";
                return false;
            }
            if (type == 10) {  // DIR64
                *reinterpret_cast<uint64_t*>(image.data() + addr) += delta;
            } else if (type == 3) {  // HIGHLOW
                *reinterpret_cast<uint32_t*>(image.data() + addr) += static_cast<uint32_t>(delta);
            } else {
                wchar_t buf[96]{};
                swprintf_s(buf, L"不支持的重定位类型 %u", static_cast<unsigned>(type));
                err = buf;
                return false;
            }
        }
        off += blockSize;
    }
    return true;
}

struct IatPatch {
    uint32_t ftRva = 0;
    size_t index = 0;
    uintptr_t value = 0;
};

// 构建 IAT 补丁列表（从文件字节解析导入，本地解析函数地址）。
// 要求依赖模块在目标进程中已加载且基址与本机一致（同架构）。
bool BuildImportPatches(const detail::PeFileView& view, HANDLE process, DWORD pid,
                        std::vector<IatPatch>& patches, std::wstring& err) {
    (void)process;
    patches.clear();
    const uint32_t importRva = view.OptDirRva(1);
    const uint32_t importSize = view.OptDirSize(1);
    if (importRva == 0 || importSize == 0) return true;
    const size_t ptrSize = view.x64 ? 8 : 4;

    size_t off = 0;
    while (off < importSize) {
        const uint8_t* desc = view.RvaToPtr(importRva + static_cast<uint32_t>(off));
        if (!desc) break;
        const uint32_t oftRva = detail::PeU32(desc);
        const uint32_t nameRva = detail::PeU32(desc + 12);
        const uint32_t ftRva = detail::PeU32(desc + 16);
        if (oftRva == 0 && nameRva == 0 && ftRva == 0) break;  // 结束项
        if (nameRva == 0 || ftRva == 0) {
            off += 20;
            continue;
        }
        const uint8_t* namePtr = view.RvaToPtr(nameRva);
        if (!namePtr) {
            err = L"导入 DLL 名 RVA 无效";
            return false;
        }
        wchar_t wide[128]{};
        MultiByteToWideChar(CP_ACP, 0,
            reinterpret_cast<const char*>(namePtr), -1, wide, 128);
        if (!wide[0]) {
            err = L"导入 DLL 名为空";
            return false;
        }

        // api-set 虚拟模块（api-ms-win-*）不进 Toolhelp 模块列表，但同一引导下
        // 宿主 DLL（ucrtbase/kernelbase）在本机与目标基址一致，直接用本机地址。
        const bool isApiSet = _wcsnicmp(wide, L"api-ms-win-", 11) == 0;
        std::wstring lower = wide;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        HMODULE localMod = GetModuleHandleW(wide);
        if (!localMod && !isApiSet) {
            localMod = LoadLibraryW(wide);
        }
        if (!localMod) {
            err = std::wstring(L"导入依赖无法解析（本地未加载）: ") + wide;
            return false;
        }
        if (!isApiSet) {
            HMODULE targetMod = detail::FindRemoteModule(pid, lower);
            if (!targetMod) {
                err = std::wstring(L"导入依赖未在目标进程加载: ") + wide;
                return false;
            }
            if (reinterpret_cast<uintptr_t>(targetMod) !=
                reinterpret_cast<uintptr_t>(localMod)) {
                err = std::wstring(L"导入 DLL 基址不一致（目标已加载但基址不同）: ") + wide;
                return false;
            }
        } else {
            // CRT api-set：要求宿主 ucrtbase 已在目标加载且基址一致
            if (lower.find(L"-crt-") != std::wstring::npos) {
                HMODULE localUcrt = GetModuleHandleW(L"ucrtbase.dll");
                HMODULE targetUcrt = localUcrt
                    ? detail::FindRemoteModule(pid, L"ucrtbase.dll") : nullptr;
                if (!targetUcrt ||
                    (localUcrt &&
                     reinterpret_cast<uintptr_t>(targetUcrt) !=
                         reinterpret_cast<uintptr_t>(localUcrt))) {
                    err = L"CRT api-set 导入需要目标进程已加载 ucrtbase.dll（基址一致）";
                    return false;
                }
            }
        }

        const uint32_t thunkRva = oftRva != 0 ? oftRva : ftRva;
        size_t i = 0;
        for (;;) {
            const uint8_t* cellPtr = view.RvaToPtr(
                thunkRva + static_cast<uint32_t>(i * ptrSize));
            if (!cellPtr) {
                err = L"导入 Thunk 越界";
                return false;
            }
            const ThunkValue tv = ReadThunk(cellPtr, view.x64);
            if (tv.raw == 0) break;
            FARPROC proc = nullptr;
            if (tv.byOrdinal) {
                proc = GetProcAddress(localMod,
                    reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(tv.ordinal)));
            } else {
                const uint8_t* fnName = view.RvaToPtr(tv.nameRva);
                if (!fnName) {
                    err = L"导入函数名 RVA 无效";
                    return false;
                }
                proc = GetProcAddress(localMod,
                    reinterpret_cast<LPCSTR>(fnName + 2));  // 跳过 Hint
            }
            if (!proc) {
                wchar_t buf[160]{};
                swprintf_s(buf, L"导入函数解析失败: %s (索引 %zu)", wide, i);
                err = buf;
                return false;
            }
            patches.push_back({ftRva, i,
                static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(proc))});
            ++i;
        }
        off += 20;
    }
    return true;
}

// 本地手动映射：把补丁写进本地镜像缓冲。
bool ResolveImports(std::vector<uint8_t>& image, const detail::PeFileView& view,
                    HANDLE process, DWORD pid, std::wstring& err) {
    std::vector<IatPatch> patches;
    if (!BuildImportPatches(view, process, pid, patches, err)) return false;
    const size_t ptrSize = view.x64 ? 8 : 4;
    for (const auto& p : patches) {
        const size_t at = static_cast<size_t>(p.ftRva) + p.index * ptrSize;
        if (at + ptrSize > image.size()) {
            err = L"导入 Thunk 越界";
            return false;
        }
        if (view.x64) {
            *reinterpret_cast<uint64_t*>(image.data() + at) =
                static_cast<uint64_t>(p.value);
        } else {
            *reinterpret_cast<uint32_t*>(image.data() + at) =
                static_cast<uint32_t>(p.value);
        }
    }
    return true;
}

// 映像节映射：把补丁写进目标进程已映射的镜像（临时可写后恢复）。
bool PatchRemoteImports(HANDLE process, uintptr_t base,
                        const detail::PeFileView& view, DWORD pid,
                        std::wstring& err) {
    std::vector<IatPatch> patches;
    if (!BuildImportPatches(view, process, pid, patches, err)) return false;
    const size_t ptrSize = view.x64 ? 8 : 4;
    for (const auto& p : patches) {
        const uintptr_t at = base + p.ftRva + p.index * ptrSize;
        const uintptr_t page = at & ~static_cast<uintptr_t>(0xFFF);
        DWORD old = 0;
        VirtualProtectEx(process, reinterpret_cast<void*>(page), 0x1000,
                         PAGE_READWRITE, &old);
        if (view.x64) {
            if (!detail::WriteRemoteBytes(process, reinterpret_cast<void*>(at),
                                          &p.value, 8, err)) {
                return false;
            }
        } else {
            const uint32_t v = static_cast<uint32_t>(p.value);
            if (!detail::WriteRemoteBytes(process, reinterpret_cast<void*>(at),
                                          &v, 4, err)) {
                return false;
            }
        }
        VirtualProtectEx(process, reinterpret_cast<void*>(page), 0x1000, old, &old);
    }
    return true;
}

}  // namespace

bool InvokeMappedEntry(HANDLE process, DWORD pid, uintptr_t base,
                       uint32_t entryRva, bool useHijack,
                       void*& outEntryRegion, std::wstring& err);

bool InjectManualMapBytes(HANDLE process, DWORD pid,
                          const std::vector<uint8_t>& peBytes,
                          void*& outBase, void*& outEntryRegion,
                          bool useHijack, std::wstring& err) {
    outBase = nullptr;
    outEntryRegion = nullptr;

    bool wow64 = false;
    if (!detail::TargetIsWow64(process, wow64, err)) return false;
    if (wow64) {
        err = L"手动映射暂仅支持本机架构（x64->x64）；WOW64 目标请使用 APC/经典注入";
        return false;
    }

    detail::PeFileView view;
    if (!view.Init(peBytes, err)) return false;
#if defined(_WIN64)
    if (!view.x64) {
        err = L"DLL 为 32 位，但当前注入器为 64 位（手动映射不支持跨架构）";
        return false;
    }
#else
    if (view.x64) {
        err = L"DLL 为 64 位，但当前注入器为 32 位";
        return false;
    }
#endif

    const uint32_t sizeOfImage = view.OptSizeOfImage();
    const uint32_t sectionAlign = view.OptSectionAlignment();
    if (sizeOfImage == 0 || sizeOfImage > 512 * 1024 * 1024) {
        err = L"SizeOfImage 异常";
        return false;
    }
    const uint32_t align = sectionAlign ? sectionAlign : 0x1000;
    const size_t imageSize = (static_cast<size_t>(sizeOfImage) + align - 1)
        & ~static_cast<size_t>(align - 1);

    std::vector<uint8_t> image(imageSize, 0);
    const uint32_t headersSize = std::min(view.OptSizeOfHeaders(), sizeOfImage);
    std::memcpy(image.data(), peBytes.data(), std::min<size_t>(
        headersSize, peBytes.size()));

    for (uint16_t i = 0; i < view.numSections; ++i) {
        const uint8_t* s = view.sections + static_cast<size_t>(i) * 40;
        const uint32_t va = detail::PeU32(s + 12);
        const uint32_t rawSize = detail::PeU32(s + 16);
        const uint32_t rawPtr = detail::PeU32(s + 20);
        if (va >= imageSize) continue;
        if (rawPtr + rawSize <= peBytes.size() && rawSize > 0) {
            std::memcpy(image.data() + va, peBytes.data() + rawPtr, rawSize);
        }
    }

    if (!ResolveImports(image, view, process, pid, err)) return false;

    {
        char v[8]{};
        bool dumpIat = false;
        if (GetEnvironmentVariableA("QST_MM_DUMP_IAT", v, 8) > 0 && v[0] == '1') {
            dumpIat = true;
        }
        if (dumpIat) {
            for (uint32_t off : {0x4000u, 0x4008u, 0x4010u, 0x4018u, 0x4020u,
                                 0x4028u, 0x4030u, 0x4038u, 0x4040u, 0x40B0u}) {
                uint64_t val = 0;
                if (off + 8 <= image.size()) {
                    std::memcpy(&val, image.data() + off, 8);
                }
                std::fwprintf(stderr, L"[MM] IAT[0x%X]=0x%llX\n",
                    off, static_cast<unsigned long long>(val));
            }
        }
    }

    const uintptr_t preferredBase = static_cast<uintptr_t>(view.OptImageBase());
    void* base = VirtualAllocEx(process,
        reinterpret_cast<LPVOID>(preferredBase), imageSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!base) {
        base = VirtualAllocEx(process, nullptr, imageSize,
                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (!base) {
        err = L"VirtualAllocEx(手动映射) 失败: " + detail::WinErrorText(GetLastError());
        return false;
    }
    const uintptr_t delta = reinterpret_cast<uintptr_t>(base) - preferredBase;
    if (!ApplyRelocations(image, view, delta, err)) {
        VirtualFreeEx(process, base, 0, MEM_RELEASE);
        return false;
    }
    if (!detail::WriteRemoteBytes(process, base, image.data(), imageSize, err)) {
        VirtualFreeEx(process, base, 0, MEM_RELEASE);
        return false;
    }

    // 设置节保护（精确到各节，避免把可写属性扩散到相邻代码节）
    for (uint16_t i = 0; i < view.numSections; ++i) {
        const uint8_t* s = view.sections + static_cast<size_t>(i) * 40;
        const uint32_t va = detail::PeU32(s + 12);
        const uint32_t vsize = detail::PeU32(s + 8);
        const uint32_t chars = detail::PeU32(s + 36);
        if (va >= imageSize) continue;
        const DWORD prot = SectionProtect(chars);
        SIZE_T region = vsize;
        region = (region + 0xFFF) & ~static_cast<SIZE_T>(0xFFF);
        if (region == 0) region = 0x1000;
        if (va + region > imageSize) region = imageSize - va;
        DWORD old = 0;
        VirtualProtectEx(process, reinterpret_cast<BYTE*>(base) + va,
                         region, prot, &old);
    }
    DWORD old = 0;
    VirtualProtectEx(process, base, headersSize, PAGE_READONLY, &old);

    void* entryRegion = nullptr;
    if (!InvokeMappedEntry(process, pid, reinterpret_cast<uintptr_t>(base),
                           view.OptEntryRva(), useHijack,
                           entryRegion, err)) {
        VirtualFreeEx(process, base, 0, MEM_RELEASE);
        return false;
    }
    outBase = base;
    outEntryRegion = entryRegion;
    return true;
}

bool InvokeMappedEntry(HANDLE process, DWORD pid, uintptr_t base,
                       uint32_t entryRva, bool useHijack,
                       void*& outEntryRegion, std::wstring& err) {
    outEntryRegion = nullptr;
    // 调试隔离：QST_MM_SKIP_ENTRY=1 时只映射不调用入口
    bool skipEntry = false;
    {
        char v[8]{};
        if (GetEnvironmentVariableA("QST_MM_SKIP_ENTRY", v, 8) > 0 && v[0] == '1') {
            skipEntry = true;
        }
    }
    if (skipEntry) return true;

    const uintptr_t entry = base + entryRva;
    constexpr size_t kCodeBytes = 0x1000;

    if (useHijack) {
        // 复合：借目标现有工作线程执行 DllMain，不新建线程
        uintptr_t codeBuf = 0;
        if (!HijackThreadCall(process, pid, entry, base, 1, 0, 15000,
                              codeBuf, err)) {
            return false;
        }
        outEntryRegion = reinterpret_cast<void*>(codeBuf);
        return true;
    }

    // 远程线程入口：代码区 RW 写入后转 PAGE_EXECUTE_READ，执行完立即释放
    std::vector<uint8_t> stub;
    if (!detail::BuildCall3Stub(true, entry, base, 1, 0, stub, err)) {
        return false;
    }
    void* codeBuf = VirtualAllocEx(process, nullptr, kCodeBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!codeBuf) {
        err = L"VirtualAllocEx(入口桩) 失败: " + detail::WinErrorText(GetLastError());
        return false;
    }
    if (!detail::WriteRemoteBytes(process, codeBuf, stub.data(), stub.size(), err)) {
        VirtualFreeEx(process, codeBuf, 0, MEM_RELEASE);
        return false;
    }
    DWORD old = 0;
    VirtualProtectEx(process, codeBuf, kCodeBytes, PAGE_EXECUTE_READ, &old);
    DWORD exitCode = 0;
    if (!detail::RunRemoteThread(process, codeBuf, nullptr, 15000,
                                 &exitCode, err)) {
        VirtualFreeEx(process, codeBuf, 0, MEM_RELEASE);
        return false;
    }
    VirtualFreeEx(process, codeBuf, 0, MEM_RELEASE);
    return true;
}

bool InjectManualMap(HANDLE process, DWORD pid, const std::wstring& dllPath,
                     void*& outBase, void*& outEntryRegion, std::wstring& err) {
    std::vector<uint8_t> bytes;
    if (!detail::ReadFileBytes(dllPath, bytes, err)) return false;
    return InjectManualMapBytes(process, pid, bytes, outBase, outEntryRegion,
                                false, err);
}

bool InjectManualMapHijack(HANDLE process, DWORD pid,
                           const std::wstring& dllPath,
                           void*& outBase, void*& outEntryRegion,
                           std::wstring& err) {
    std::vector<uint8_t> bytes;
    if (!detail::ReadFileBytes(dllPath, bytes, err)) return false;
    return InjectManualMapBytes(process, pid, bytes, outBase, outEntryRegion,
                                true, err);
}

// SEC_IMAGE 映像节映射：NtCreateSection + NtMapViewOfSection 把 DLL 作为映像
// 映射进目标，区域类型为 MEM_IMAGE（内存扫描看起来像正常加载的镜像），
// 节权限由映像自身决定（无 RWX）。重定位由内核完成。
bool InjectImageMapBytes(HANDLE process, DWORD pid, const std::wstring& dllPath,
                         bool useHijack, void*& outBase,
                         void*& outEntryRegion, std::wstring& err) {
    outBase = nullptr;
    outEntryRegion = nullptr;
    bool wow64 = false;
    if (!detail::TargetIsWow64(process, wow64, err)) return false;
    if (wow64) {
        err = L"映像节映射暂仅支持本机架构（x64->x64）";
        return false;
    }

    std::vector<uint8_t> peBytes;
    if (!detail::ReadFileBytes(dllPath, peBytes, err)) return false;
    detail::PeFileView view;
    if (!view.Init(peBytes, err)) return false;
#if defined(_WIN64)
    if (!view.x64) {
        err = L"DLL 为 32 位，但当前注入器为 64 位（映像节映射不支持跨架构）";
        return false;
    }
#else
    if (view.x64) {
        err = L"DLL 为 64 位，但当前注入器为 32 位";
        return false;
    }
#endif

    HANDLE hFile = CreateFileW(dllPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        err = L"打开 DLL 文件失败: " + detail::WinErrorText(GetLastError());
        return false;
    }

    typedef LONG(NTAPI* NtCreateSection_t)(PHANDLE, ACCESS_MASK, PVOID,
        PLARGE_INTEGER, ULONG, ULONG, HANDLE);
    typedef LONG(NTAPI* NtMapViewOfSection_t)(HANDLE, HANDLE, PVOID*, ULONG_PTR,
        SIZE_T, PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);
    typedef LONG(NTAPI* NtClose_t)(HANDLE);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto* ntCreateSection = reinterpret_cast<NtCreateSection_t>(
        GetProcAddress(ntdll, "NtCreateSection"));
    auto* ntMapViewOfSection = reinterpret_cast<NtMapViewOfSection_t>(
        GetProcAddress(ntdll, "NtMapViewOfSection"));
    auto* ntClose = reinterpret_cast<NtClose_t>(
        GetProcAddress(ntdll, "NtClose"));
    if (!ntCreateSection || !ntMapViewOfSection || !ntClose) {
        CloseHandle(hFile);
        err = L"无法解析 NtCreateSection/NtMapViewOfSection/NtClose";
        return false;
    }

    HANDLE hSection = nullptr;
    LONG status = ntCreateSection(&hSection,
        SECTION_MAP_READ | SECTION_MAP_EXECUTE, nullptr, nullptr,
        PAGE_READONLY, SEC_IMAGE, hFile);
    CloseHandle(hFile);
    if (status < 0 || !hSection) {
        wchar_t buf[128]{};
        swprintf_s(buf, L"NtCreateSection(SEC_IMAGE) 失败: 0x%08X",
            static_cast<unsigned>(status));
        err = buf;
        return false;
    }

    SIZE_T viewSize = view.OptSizeOfImage();
    PVOID baseAddr = reinterpret_cast<PVOID>(
        static_cast<uintptr_t>(view.OptImageBase()));
    status = ntMapViewOfSection(hSection, process, &baseAddr, 0, 0, nullptr,
        &viewSize, 1 /*ViewShare*/, 0, PAGE_EXECUTE_READ);
    if (status < 0) {
        // 首选基址被占/镜像已加载 → 任意基址重试（内核自动重定位）
        baseAddr = nullptr;
        status = ntMapViewOfSection(hSection, process, &baseAddr, 0, 0, nullptr,
            &viewSize, 1 /*ViewShare*/, 0, PAGE_EXECUTE_READ);
    }
    ntClose(hSection);
    if (status < 0 || !baseAddr) {
        wchar_t buf[128]{};
        swprintf_s(buf, L"NtMapViewOfSection 失败: 0x%08X",
            static_cast<unsigned>(status));
        err = buf;
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(baseAddr);
    if (!PatchRemoteImports(process, base, view, pid, err)) {
        return false;  // 保留映射便于诊断
    }
    void* entryRegion = nullptr;
    if (!InvokeMappedEntry(process, pid, base, view.OptEntryRva(), useHijack,
                           entryRegion, err)) {
        return false;
    }
    outBase = reinterpret_cast<void*>(base);
    outEntryRegion = entryRegion;
    return true;
}

bool InjectImageMap(HANDLE process, DWORD pid, const std::wstring& dllPath,
                    void*& outBase, void*& outEntryRegion, std::wstring& err) {
    return InjectImageMapBytes(process, pid, dllPath, false,
                               outBase, outEntryRegion, err);
}

bool InjectImageMapHijack(HANDLE process, DWORD pid, const std::wstring& dllPath,
                          void*& outBase, void*& outEntryRegion,
                          std::wstring& err) {
    return InjectImageMapBytes(process, pid, dllPath, true,
                               outBase, outEntryRegion, err);
}

}  // namespace inject
}  // namespace windowmode
