#include "inject_common.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>

namespace windowmode {
namespace inject {
namespace detail {
namespace {

uint16_t ReadU16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
uint32_t ReadU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
uint64_t ReadU64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
void AppendByte(std::vector<uint8_t>& v, uint8_t b) { v.push_back(b); }
void AppendU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
void AppendU64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

}  // namespace

uint16_t PeU16(const uint8_t* p) { return ReadU16(p); }
uint32_t PeU32(const uint8_t* p) { return ReadU32(p); }
uint64_t PeU64(const uint8_t* p) { return ReadU64(p); }

std::wstring WinErrorText(DWORD code) {
    wchar_t buf[128]{};
    swprintf_s(buf, L"Win32=%lu", static_cast<unsigned long>(code));
    return buf;
}

bool OpenTarget(DWORD pid, DWORD extraAccess, HANDLE& out, std::wstring& err) {
    const DWORD access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
        | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | extraAccess | SYNCHRONIZE;
    HANDLE h = OpenProcess(access, FALSE, pid);
    if (!h) {
        err = L"OpenProcess 失败: " + WinErrorText(GetLastError());
        return false;
    }
    out = h;
    return true;
}

bool TargetIsWow64(HANDLE process, bool& outWow64, std::wstring& err) {
    outWow64 = false;
#if defined(_WIN64)
    BOOL wow = FALSE;
    if (!IsWow64Process(process, &wow)) {
        err = L"IsWow64Process 失败: " + WinErrorText(GetLastError());
        return false;
    }
    outWow64 = wow == TRUE;
    return true;
#else
    (void)process;
    outWow64 = true;
    return true;
#endif
}

std::wstring BaseNameOnly(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return path;
    return path.substr(slash + 1);
}

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring full(path);
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return full.substr(0, slash + 1);
}

bool ResolveArchDll(DWORD pid, const wchar_t* name64, const wchar_t* name32,
                    std::wstring& outPath, std::wstring& err) {
    HANDLE process = nullptr;
    if (!OpenTarget(pid, 0, process, err)) return false;
    bool wow64 = false;
    const bool ok = TargetIsWow64(process, wow64, err);
    CloseHandle(process);
    if (!ok) return false;

#if defined(_WIN64)
    const wchar_t* dllName = wow64 ? name32 : name64;
#else
    if (!wow64) {
        err = L"当前为 32 位主程序，无法向 64 位目标注入";
        return false;
    }
    dllName = name32;
#endif

    const std::wstring dir = ModuleDirectory();
    if (dir.empty()) {
        err = L"无法解析主程序目录";
        return false;
    }
    outPath = dir + dllName;
    return true;
}

namespace {

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* NtQueryInformationProcess_t)(
    HANDLE, ULONG, PVOID, ULONG, PULONG);

constexpr ULONG kProcessBasicInformation = 0;
constexpr ULONG kProcessWow64Information = 26;

void LowerInPlace(std::wstring& s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
}

bool ReadRemotePtr(HANDLE process, uintptr_t addr, size_t width,
                    uintptr_t& out, std::wstring& err) {
    uint64_t raw = 0;
    if (!ReadRemoteBytes(process, reinterpret_cast<const void*>(addr),
                          &raw, width, err)) {
        return false;
    }
    out = static_cast<uintptr_t>(raw);
    return true;
}

bool ReadRemoteUnicode(HANDLE process, uintptr_t strAddr, size_t width,
                        std::wstring& out, std::wstring& err) {
    out.clear();
    uint8_t raw[16]{};
    const SIZE_T n = width == 8 ? 16 : 8;
    if (!ReadRemoteBytes(process, reinterpret_cast<const void*>(strAddr),
                         raw, n, err)) {
        return false;
    }
    const uint16_t nbytes = PeU16(raw);
    if (nbytes == 0 || (nbytes % 2) != 0 || nbytes > 1024) return true;
    const uintptr_t buf = width == 8 ? PeU64(raw + 8) : PeU32(raw + 4);
    if (!buf) return true;
    out.assign(nbytes / 2, L'\0');
    return ReadRemoteBytes(process, reinterpret_cast<const void*>(buf),
                           out.data(), nbytes, err);
}

bool SameArchAsSelf(HANDLE process, std::wstring& err) {
    bool wow64 = false;
    if (!TargetIsWow64(process, wow64, err)) return false;
#if defined(_WIN64)
    return !wow64;
#else
    return wow64;
#endif
}

bool IsSystemModuleName(const std::wstring& lower) {
    return lower == L"kernel32.dll" || lower == L"kernelbase.dll"
        || lower == L"ntdll.dll";
}

HMODULE FindRemoteModuleViaToolhelp(DWORD pid, const std::wstring& wantLower) {
    const DWORD flagSets[] = {
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        TH32CS_SNAPMODULE,
        TH32CS_SNAPMODULE32,
    };
    for (DWORD flags : flagSets) {
        for (int attempt = 0; attempt < 4; ++attempt) {
            HANDLE snap = CreateToolhelp32Snapshot(flags, pid);
            if (snap == INVALID_HANDLE_VALUE) {
                const DWORD e = GetLastError();
                if ((e == ERROR_BAD_LENGTH || e == ERROR_PARTIAL_COPY) && attempt < 3) {
                    Sleep(20);
                    continue;
                }
                break;
            }
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            HMODULE found = nullptr;
            if (Module32FirstW(snap, &me)) {
                do {
                    std::wstring name = me.szModule;
                    LowerInPlace(name);
                    if (name == wantLower) {
                        found = me.hModule;
                        break;
                    }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
            if (found) return found;
            break;
        }
    }
    return nullptr;
}

HMODULE FindRemoteModuleViaPsapi(HANDLE process, const std::wstring& wantLower) {
    using EnumEx = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD, DWORD);
    using BaseNameFn = DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return nullptr;
    auto enumEx = reinterpret_cast<EnumEx>(
        GetProcAddress(k32, "K32EnumProcessModulesEx"));
    auto baseName = reinterpret_cast<BaseNameFn>(
        GetProcAddress(k32, "K32GetModuleBaseNameW"));
    if (!enumEx || !baseName) return nullptr;

    constexpr DWORD kListModulesAll = 0x03;
    std::vector<HMODULE> mods(512);
    DWORD needed = 0;
    bool listed = false;
    for (int grow = 0; grow < 4; ++grow) {
        const DWORD cb = static_cast<DWORD>(mods.size() * sizeof(HMODULE));
        if (enumEx(process, mods.data(), cb, &needed, kListModulesAll)) {
            if (needed > cb && needed > 0) {
                mods.resize(needed / sizeof(HMODULE) + 8);
                continue;
            }
            listed = true;
            break;
        }
        if (needed <= cb) return nullptr;
        mods.resize(needed / sizeof(HMODULE) + 8);
    }
    if (!listed) return nullptr;
    const size_t count = needed / sizeof(HMODULE);
    wchar_t name[MAX_PATH]{};
    for (size_t i = 0; i < count && i < mods.size(); ++i) {
        if (!mods[i]) continue;
        if (!baseName(process, mods[i], name, MAX_PATH)) continue;
        std::wstring n = name;
        LowerInPlace(n);
        if (n == wantLower) return mods[i];
    }
    return nullptr;
}

HANDLE OpenForModuleEnum(DWORD pid) {
    return OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
}

}  // namespace

bool QueryRemotePeb(HANDLE process, uintptr_t& peb, bool& wow64, std::wstring& err) {
    peb = 0;
    wow64 = false;
    if (!process) {
        err = L"QueryRemotePeb 进程句柄无效";
        return false;
    }
    if (!TargetIsWow64(process, wow64, err)) return false;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto* query = reinterpret_cast<NtQueryInformationProcess_t>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!query) {
        err = L"无法解析 NtQueryInformationProcess";
        return false;
    }

#if defined(_WIN64)
    if (wow64) {
        ULONG_PTR peb32 = 0;
        if (query(process, kProcessWow64Information, &peb32, sizeof(peb32),
                  nullptr) >= 0 && peb32 != 0) {
            peb = static_cast<uintptr_t>(peb32);
            return true;
        }
        err = L"无法读取 WOW64 PEB";
        return false;
    }
#endif

    struct BasicInfo {
        PVOID Reserved1;
        PVOID PebBaseAddress;
        PVOID Reserved2[2];
        ULONG_PTR UniqueProcessId;
        PVOID Reserved3;
    } bi{};
    if (query(process, kProcessBasicInformation, &bi, sizeof(bi), nullptr) < 0
        || !bi.PebBaseAddress) {
        err = L"NtQueryInformationProcess 失败";
        return false;
    }
    peb = reinterpret_cast<uintptr_t>(bi.PebBaseAddress);
    return true;
}

HMODULE FindRemoteModuleViaPeb(HANDLE process, const std::wstring& dllBaseNameLower) {
    if (!process) return nullptr;
    std::wstring want = dllBaseNameLower;
    LowerInPlace(want);

    std::wstring err;
    uintptr_t peb = 0;
    bool wow64 = false;
    if (!QueryRemotePeb(process, peb, wow64, err)) return nullptr;
    const size_t width = wow64 ? 4u : 8u;

    uintptr_t ldr = 0;
    if (!ReadRemotePtr(process, peb + (wow64 ? 0x0C : 0x18), width, ldr, err)
        || !ldr) {
        return nullptr;
    }
    const uintptr_t loadHead = ldr + (wow64 ? 0x0C : 0x10);
    const uintptr_t dllBaseOff = wow64 ? 0x18 : 0x30;
    const uintptr_t baseNameOff = wow64 ? 0x2C : 0x58;
    const uintptr_t fullNameOff = wow64 ? 0x24 : 0x48;

    uintptr_t node = 0;
    if (!ReadRemotePtr(process, loadHead, width, node, err)) return nullptr;
    for (int guard = 0; guard < 4096 && node != 0 && node != loadHead; ++guard) {
        uintptr_t base = 0;
        if (ReadRemotePtr(process, node + dllBaseOff, width, base, err) && base) {
            std::wstring name;
            if (!ReadRemoteUnicode(process, node + baseNameOff, width, name, err)
                || name.empty()) {
                ReadRemoteUnicode(process, node + fullNameOff, width, name, err);
            }
            std::wstring file = BaseNameOnly(name);
            LowerInPlace(file);
            if (file == want) {
                return reinterpret_cast<HMODULE>(base);
            }
        }
        if (!ReadRemotePtr(process, node, width, node, err)) break;
    }
    return nullptr;
}

HMODULE FindRemoteModule(DWORD pid, const std::wstring& dllBaseNameLower) {
    std::wstring want = dllBaseNameLower;
    LowerInPlace(want);
    HMODULE found = FindRemoteModuleViaToolhelp(pid, want);
    if (found) return found;

    // 枚举只需 QUERY+VM_READ。OpenTarget 还要 VM_WRITE，反作弊常拦第二次
    // OpenProcess(写)，导致 Toolhelp 失败后连 PEB 都走不成。
    HANDLE process = OpenForModuleEnum(pid);
    if (!process) return nullptr;
    found = FindRemoteModuleViaPsapi(process, want);
    if (!found) found = FindRemoteModuleViaPeb(process, want);
    CloseHandle(process);
    return found;
}

bool WaitForRemoteModule(DWORD pid, const std::wstring& dllBaseNameLower,
                         DWORD timeoutMs, HMODULE& out, std::wstring& err) {
    std::wstring want = dllBaseNameLower;
    std::transform(want.begin(), want.end(), want.begin(), ::towlower);
    const DWORD deadline = GetTickCount() + timeoutMs;
    do {
        HMODULE m = FindRemoteModule(pid, want);
        if (m) {
            out = m;
            return true;
        }
        if (GetTickCount() >= deadline) break;
        Sleep(80);
    } while (true);
    out = nullptr;
    err = L"等待远端模块超时: " + dllBaseNameLower;
    return false;
}

uintptr_t ResolveRemoteProcAddress(HANDLE process, DWORD pid,
                                   const wchar_t* moduleBaseName,
                                   const char* procName, std::wstring& err) {
    std::wstring want = moduleBaseName;
    std::transform(want.begin(), want.end(), want.begin(), ::towlower);
    HMODULE mod = FindRemoteModule(pid, want);
    if (!mod && process) {
        mod = FindRemoteModuleViaPsapi(process, want);
        if (!mod) mod = FindRemoteModuleViaPeb(process, want);
    }
    if (!mod) {
        std::wstring archErr;
        if (process && IsSystemModuleName(want) && SameArchAsSelf(process, archErr)) {
            HMODULE local = GetModuleHandleW(moduleBaseName);
            FARPROC fn = local ? GetProcAddress(local, procName) : nullptr;
            if (fn) return reinterpret_cast<uintptr_t>(fn);
        }
        err = std::wstring(L"目标进程未加载模块: ") + moduleBaseName;
        return 0;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(mod);

    // 读 PE 头（导出目录 RVA/大小）
    uint8_t header[0x1000]{};
    if (!ReadRemoteBytes(process, reinterpret_cast<const void*>(base),
                         header, sizeof(header), err)) {
        return 0;
    }
    if (ReadU16(header) != 0x5A4D) {
        err = std::wstring(L"远端模块头无效: ") + moduleBaseName;
        return 0;
    }
    const uint32_t peOff = ReadU32(header + 0x3C);
    if (peOff + 0x18 + 0x78 > sizeof(header)) {
        err = std::wstring(L"远端模块 NT 头越界: ") + moduleBaseName;
        return 0;
    }
    const uint16_t magic = ReadU16(header + peOff + 0x18);
    const bool x64 = magic == 0x20B;
    if (!x64 && magic != 0x10B) {
        err = std::wstring(L"远端模块魔数无效: ") + moduleBaseName;
        return 0;
    }
    const uint32_t dirBase = x64 ? 0x70 : 0x60;
    const uint32_t expRva = ReadU32(header + peOff + 0x18 + dirBase);
    const uint32_t expSize = ReadU32(header + peOff + 0x18 + dirBase + 4);
    if (expRva == 0 || expSize == 0) {
        err = std::wstring(L"远端模块无导出表: ") + moduleBaseName;
        return 0;
    }

    // 读导出目录
    const uint32_t dirBytes = expSize < 0x1000 ? expSize : 0x1000;
    std::vector<uint8_t> exp(dirBytes);
    if (!ReadRemoteBytes(process,
                         reinterpret_cast<const void*>(base + expRva),
                         exp.data(), exp.size(), err)) {
        return 0;
    }
    if (exp.size() < 40) {
        err = L"导出目录过小";
        return 0;
    }
    const uint32_t numNames = ReadU32(exp.data() + 24);
    const uint32_t addrFuncs = ReadU32(exp.data() + 28);
    const uint32_t addrNames = ReadU32(exp.data() + 32);
    const uint32_t addrOrdinals = ReadU32(exp.data() + 36);
    if (numNames == 0 || numNames > 0x10000) {
        err = L"导出名称数量异常";
        return 0;
    }

    std::vector<uint8_t> namesBuf(static_cast<size_t>(numNames) * 4);
    if (!ReadRemoteBytes(process,
                         reinterpret_cast<const void*>(base + addrNames),
                         namesBuf.data(), namesBuf.size(), err)) {
        return 0;
    }
    std::vector<uint8_t> ordBuf(static_cast<size_t>(numNames) * 2);
    if (!ReadRemoteBytes(process,
                         reinterpret_cast<const void*>(base + addrOrdinals),
                         ordBuf.data(), ordBuf.size(), err)) {
        return 0;
    }

    char name[256]{};
    for (uint32_t i = 0; i < numNames; ++i) {
        const uint32_t nameRva = ReadU32(namesBuf.data() + i * 4);
        if (!ReadRemoteBytes(process,
                             reinterpret_cast<const void*>(base + nameRva),
                             name, sizeof(name) - 1, err)) {
            return 0;
        }
        name[sizeof(name) - 1] = '\0';
        if (ExportNameMatches(name, procName)) {
            const uint16_t ord = ReadU16(ordBuf.data() + i * 2);
            uint8_t fn[4]{};
            if (!ReadRemoteBytes(process,
                                 reinterpret_cast<const void*>(
                                     base + addrFuncs + static_cast<uint32_t>(ord) * 4),
                                 fn, sizeof(fn), err)) {
                return 0;
            }
            return base + ReadU32(fn);
        }
    }
    wchar_t wide[128]{};
    MultiByteToWideChar(CP_ACP, 0, procName, -1, wide, 128);
    err = std::wstring(L"目标模块未导出: ") + moduleBaseName + L"!" + wide;
    return 0;
}

bool ReadRemoteBytes(HANDLE process, const void* addr, void* buf,
                     SIZE_T size, std::wstring& err) {
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, addr, buf, size, &read) || read != size) {
        err = L"ReadProcessMemory 失败: " + WinErrorText(GetLastError());
        return false;
    }
    return true;
}

bool WriteRemoteBytes(HANDLE process, void* addr, const void* buf,
                      SIZE_T size, std::wstring& err) {
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, addr, buf, size, &written) || written != size) {
        err = L"WriteProcessMemory 失败: " + WinErrorText(GetLastError());
        return false;
    }
    return true;
}

bool RunRemoteThread(HANDLE process, void* start, void* arg,
                     DWORD timeoutMs, DWORD* exitCode, std::wstring& err) {
    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(start), arg, 0, nullptr);
    if (!thread) {
        err = L"CreateRemoteThread 失败: " + WinErrorText(GetLastError());
        return false;
    }
    WaitForSingleObject(thread, timeoutMs);
    DWORD code = 0;
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    if (exitCode) *exitCode = code;
    return true;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out,
                   std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"打开文件失败: " + path + L" (" + WinErrorText(GetLastError()) + L")";
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart <= 0 || size.QuadPart > 256 * 1024 * 1024) {
        CloseHandle(h);
        err = L"文件大小异常: " + path;
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != out.size()) {
        err = L"读取文件失败: " + path;
        return false;
    }
    return true;
}

// ── PeFileView ───────────────────────────────────────────────────────────────

bool PeFileView::Init(const std::vector<uint8_t>& bytes, std::wstring& err) {
    data = bytes.data();
    size = bytes.size();
    if (size < 0x40 || ReadU16(data) != 0x5A4D) {
        err = L"无效 PE 文件（缺少 DOS 头）";
        return false;
    }
    peOff = ReadU32(data + 0x3C);
    if (peOff + 0x18 + 0x70 > size || std::memcmp(data + peOff, "PE\0\0", 4) != 0) {
        err = L"无效 PE 文件（缺少 NT 头）";
        return false;
    }
    magic = ReadU16(data + peOff + 0x18);
    x64 = magic == 0x20B;
    if (!x64 && magic != 0x10B) {
        err = L"不支持的 PE 魔数";
        return false;
    }
    numSections = ReadU16(data + peOff + 4 + 2);
    optSize = ReadU16(data + peOff + 4 + 16);
    sections = data + peOff + 4 + 20 + optSize;
    if (sections + static_cast<size_t>(numSections) * 40 > data + size) {
        err = L"节表越界";
        return false;
    }
    return true;
}

const uint8_t* PeFileView::RvaToPtr(uint32_t rva) const {
    for (uint16_t i = 0; i < numSections; ++i) {
        const uint8_t* s = sections + static_cast<size_t>(i) * 40;
        const uint32_t va = ReadU32(s + 12);
        const uint32_t vsize = ReadU32(s + 8);
        const uint32_t rawSize = ReadU32(s + 16);
        const uint32_t rawPtr = ReadU32(s + 20);
        const uint32_t span = (vsize > rawSize ? vsize : rawSize);
        if (rva >= va && rva < va + span) {
            const uint32_t off = rva - va;
            if (rawPtr + off >= size) return nullptr;
            return data + rawPtr + off;
        }
    }
    // 头内 RVA：文件偏移 == RVA。必须用 SizeOfHeaders（常见 0x200/0x400），
    // 不能用 SizeOfOptionalHeader（≈224）——小 DLL 的导出名常落在 0xE0..0x400，
    // 旧判断会让 FindExportRva 扫不到任何名字 →「未找到导出: FakeFocus_Install」。
    const uint32_t headers = OptSizeOfHeaders();
    const uint32_t headerSpan = headers > optSize ? headers : optSize;
    if (rva < headerSpan && static_cast<size_t>(rva) < size) return data + rva;
    return nullptr;
}

uint32_t PeFileView::OptU32(uint32_t optOff) const {
    return ReadU32(data + peOff + 0x18 + optOff);
}
uint64_t PeFileView::OptU64(uint32_t optOff) const {
    return ReadU64(data + peOff + 0x18 + optOff);
}
uint32_t PeFileView::OptEntryRva() const { return OptU32(16); }
uint64_t PeFileView::OptImageBase() const { return x64 ? OptU64(24) : OptU32(28); }
uint32_t PeFileView::OptSectionAlignment() const { return OptU32(32); }
uint32_t PeFileView::OptFileAlignment() const { return OptU32(36); }
uint32_t PeFileView::OptSizeOfImage() const { return OptU32(56); }
uint32_t PeFileView::OptSizeOfHeaders() const { return OptU32(60); }
uint32_t PeFileView::OptDirRva(int index) const {
    const uint32_t base = x64 ? 112 : 96;
    return ReadU32(data + peOff + 0x18 + base + static_cast<uint32_t>(index) * 8);
}
uint32_t PeFileView::OptDirSize(int index) const {
    const uint32_t base = x64 ? 112 : 96;
    return ReadU32(data + peOff + 0x18 + base + static_cast<uint32_t>(index) * 8 + 4);
}

bool XorBytes(const std::vector<uint8_t>& in, uint8_t key,
              std::vector<uint8_t>& out) {
    out.resize(in.size());
    for (size_t i = 0; i < in.size(); ++i) out[i] = in[i] ^ key;
    return true;
}

bool XorFile(const std::wstring& inPath, const std::wstring& outPath,
             uint8_t key, std::wstring& err) {
    std::vector<uint8_t> raw;
    if (!ReadFileBytes(inPath, raw, err)) return false;
    std::vector<uint8_t> xored;
    XorBytes(raw, key, xored);
    HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"创建输出文件失败: " + outPath + L" (" + WinErrorText(GetLastError()) + L")";
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, xored.data(), static_cast<DWORD>(xored.size()),
        &written, nullptr);
    CloseHandle(h);
    if (!ok || written != xored.size()) {
        err = L"写入输出文件失败: " + outPath;
        return false;
    }
    return true;
}

bool ExportNameMatches(const char* peName, const char* exportName) {
    if (!peName || !exportName || !*exportName) return false;
    if (std::strcmp(peName, exportName) == 0) return true;
    const size_t elen = std::strlen(exportName);
    auto stdcallSuffix = [](const char* afterName) -> bool {
        // @N，至少一位十进制栈字节数
        if (!afterName || *afterName != '@') return false;
        const char* p = afterName + 1;
        if (*p < '0' || *p > '9') return false;
        for (; *p; ++p) {
            if (*p < '0' || *p > '9') return false;
        }
        return true;
    };
    // MSVC x86 __stdcall：_FakeFocus_InstallLite@4
    if (peName[0] == '_' &&
        std::strncmp(peName + 1, exportName, elen) == 0 &&
        stdcallSuffix(peName + 1 + elen)) {
        return true;
    }
    // MinGW x86 __stdcall：FakeFocus_InstallLite@4
    if (std::strncmp(peName, exportName, elen) == 0 &&
        stdcallSuffix(peName + elen)) {
        return true;
    }
    return false;
}

bool FindExportRva(const std::vector<uint8_t>& pe, const char* exportName,
                   DWORD& outRva, std::wstring& err) {
    PeFileView view;
    if (!view.Init(pe, err)) return false;
    const uint32_t expRva = view.OptDirRva(0);
    if (expRva == 0) {
        err = L"DLL 没有导出表";
        return false;
    }
    const uint8_t* exp = view.RvaToPtr(expRva);
    if (!exp) {
        err = L"导出表越界";
        return false;
    }
    const uint32_t numNames = ReadU32(exp + 24);
    const uint32_t addrFuncs = ReadU32(exp + 28);
    const uint32_t addrNames = ReadU32(exp + 32);
    const uint32_t addrOrdinals = ReadU32(exp + 36);
    for (uint32_t i = 0; i < numNames; ++i) {
        const uint8_t* nameRvaPtr = view.RvaToPtr(addrNames + i * 4);
        if (!nameRvaPtr) continue;
        const uint32_t nameRva = ReadU32(nameRvaPtr);
        const uint8_t* name = view.RvaToPtr(nameRva);
        if (!name) continue;
        if (!ExportNameMatches(reinterpret_cast<const char*>(name), exportName)) continue;
        const uint8_t* ordPtr = view.RvaToPtr(addrOrdinals + i * 2);
        if (!ordPtr) continue;
        const uint16_t ord = ReadU16(ordPtr);
        const uint8_t* fnPtr = view.RvaToPtr(addrFuncs + ord * 4);
        if (!fnPtr) continue;
        const uint32_t fnRva = ReadU32(fnPtr);
        outRva = fnRva;
        return true;
    }
    wchar_t wide[128]{};
    MultiByteToWideChar(CP_ACP, 0, exportName, -1, wide, 128);
    err = std::wstring(L"未找到导出: ") + wide;
    std::wstring preview;
    const uint32_t previewN = numNames < 12 ? numNames : 12;
    for (uint32_t i = 0; i < previewN; ++i) {
        const uint8_t* nameRvaPtr = view.RvaToPtr(addrNames + i * 4);
        if (!nameRvaPtr) continue;
        const uint8_t* name = view.RvaToPtr(ReadU32(nameRvaPtr));
        if (!name) continue;
        wchar_t one[96]{};
        MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(name), -1, one, 96);
        if (!preview.empty()) preview += L",";
        preview += one;
    }
    if (numNames > previewN) preview += L",...";
    if (!preview.empty()) err += L" [" + preview + L"]";
    return false;
}

bool FindExportRvaByLocalImage(const std::wstring& dllPath, const char* exportName,
                               DWORD& outRva, std::wstring& err) {
    if (dllPath.empty() || !exportName || !*exportName) {
        err = L"本地映像导出参数无效";
        return false;
    }
    HMODULE local = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) {
        err = L"本地映射 DLL 失败: " + WinErrorText(GetLastError());
        return false;
    }
    auto tryName = [&](const char* name) -> FARPROC {
        return name && *name ? GetProcAddress(local, name) : nullptr;
    };
    FARPROC fn = tryName(exportName);
    if (!fn) {
        char decorated[160]{};
        static const int kStacks[] = {0, 4, 8, 12, 16, 20};
        for (int stack : kStacks) {
            if (sprintf_s(decorated, "_%s@%d", exportName, stack) > 0) {
                fn = tryName(decorated);
                if (fn) break;
            }
            if (sprintf_s(decorated, "%s@%d", exportName, stack) > 0) {
                fn = tryName(decorated);
                if (fn) break;
            }
        }
    }
    if (!fn) {
        FreeLibrary(local);
        wchar_t wide[128]{};
        MultiByteToWideChar(CP_ACP, 0, exportName, -1, wide, 128);
        err = std::wstring(L"未找到导出: ") + wide;
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(local);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(fn);
    if (addr < base) {
        FreeLibrary(local);
        err = L"本地导出地址低于映像基址";
        return false;
    }
    const uintptr_t rva = addr - base;
    if (rva > 0x7FFFFFFFu) {
        FreeLibrary(local);
        err = L"本地导出 RVA 异常";
        return false;
    }
    outRva = static_cast<DWORD>(rva);
    FreeLibrary(local);
    return true;
}

bool BuildCall3Stub(bool x64, uintptr_t fn, uintptr_t a1, uintptr_t a2,
                    uintptr_t a3, std::vector<uint8_t>& out, std::wstring& err) {
    (void)err;
    out.clear();
    if (x64) {
        // 线程入口桩：CreateRemoteThread 起始 RSP ≡ 8 (mod 16)，栈顶为返回地址。
        // sub 28h（40）→ 调用点 RSP ≡ 0 (mod 16)（ABI 正确），add 后 ret 仍能
        // 弹到原返回地址（不能 and rsp，否则返回地址被“抬”丢）。
        AppendByte(out, 0x48); AppendByte(out, 0x83); AppendByte(out, 0xEC);
        AppendByte(out, 0x28);
        // mov rcx, a1
        AppendByte(out, 0x48); AppendByte(out, 0xB9); AppendU64(out, a1);
        // mov rdx, a2
        AppendByte(out, 0x48); AppendByte(out, 0xBA); AppendU64(out, a2);
        // mov r8, a3
        AppendByte(out, 0x49); AppendByte(out, 0xB8); AppendU64(out, a3);
        // mov rax, fn
        AppendByte(out, 0x48); AppendByte(out, 0xB8); AppendU64(out, fn);
        // call rax
        AppendByte(out, 0xFF); AppendByte(out, 0xD0);
        // add rsp, 28h
        AppendByte(out, 0x48); AppendByte(out, 0x83); AppendByte(out, 0xC4);
        AppendByte(out, 0x28);
        // ret
        AppendByte(out, 0xC3);
    } else {
        // push a3 / a2 / a1
        AppendByte(out, 0x68); AppendU32(out, static_cast<uint32_t>(a3));
        AppendByte(out, 0x68); AppendU32(out, static_cast<uint32_t>(a2));
        AppendByte(out, 0x68); AppendU32(out, static_cast<uint32_t>(a1));
        // mov eax, fn
        AppendByte(out, 0xB8); AppendU32(out, static_cast<uint32_t>(fn));
        // call eax
        AppendByte(out, 0xFF); AppendByte(out, 0xD0);
        // ret（__stdcall 被调方清栈）
        AppendByte(out, 0xC3);
    }
    return true;
}

bool BuildHijackStub(bool x64, uintptr_t fn, uintptr_t a1, uintptr_t a2,
                     uintptr_t a3, uintptr_t contextPtr, uintptr_t setContextAddr,
                     uintptr_t originalIp, uintptr_t startedAddr, uintptr_t resultAddr,
                     uintptr_t errorAddr, uintptr_t getLastErrorAddr,
                     std::vector<uint8_t>& out, std::wstring& err) {
    out.clear();
    // 哨兵：一进入桩就写入 startedAddr（诊断：线程是否真的执行了桩）
    if (startedAddr != 0) {
        if (x64) {
            AppendByte(out, 0x48); AppendByte(out, 0xB8);
            AppendU64(out, 0x12345678ULL);
            AppendByte(out, 0x48); AppendByte(out, 0xA3);
            AppendU64(out, startedAddr);
        } else {
            AppendByte(out, 0xB8); AppendU32(out, 0x12345678u);
            AppendByte(out, 0xA3); AppendU32(out, static_cast<uint32_t>(startedAddr));
        }
    }
    if (x64) {
        // and rsp, -16
        AppendByte(out, 0x48); AppendByte(out, 0x83); AppendByte(out, 0xE4);
        AppendByte(out, 0xF0);
        // sub rsp, 20h（影子空间 32 字节；调用前 RSP 保持 16 对齐）
        AppendByte(out, 0x48); AppendByte(out, 0x83); AppendByte(out, 0xEC);
        AppendByte(out, 0x20);
        // mov rcx, a1
        AppendByte(out, 0x48); AppendByte(out, 0xB9); AppendU64(out, a1);
        // mov rdx, a2
        AppendByte(out, 0x48); AppendByte(out, 0xBA); AppendU64(out, a2);
        // mov r8, a3
        AppendByte(out, 0x49); AppendByte(out, 0xB8); AppendU64(out, a3);
        // mov rax, fn
        AppendByte(out, 0x48); AppendByte(out, 0xB8); AppendU64(out, fn);
        // call rax
        AppendByte(out, 0xFF); AppendByte(out, 0xD0);
        // add rsp, 20h
        AppendByte(out, 0x48); AppendByte(out, 0x83); AppendByte(out, 0xC4);
        AppendByte(out, 0x20);
    } else {
        // push a3 / a2 / a1
        AppendByte(out, 0x68); AppendU32(out, static_cast<uint32_t>(a3));
        AppendByte(out, 0x68); AppendU32(out, static_cast<uint32_t>(a2));
        AppendByte(out, 0x68); AppendU32(out, static_cast<uint32_t>(a1));
        // mov eax, fn
        AppendByte(out, 0xB8); AppendU32(out, static_cast<uint32_t>(fn));
        // call eax
        AppendByte(out, 0xFF); AppendByte(out, 0xD0);
    }
    // 保存 fn 的返回值到 resultAddr（诊断用；resultAddr=0 则跳过）
    if (resultAddr != 0) {
        if (x64) {
            // mov [imm64], rax
            AppendByte(out, 0x48); AppendByte(out, 0xA3);
            AppendU64(out, resultAddr);
        } else {
            // mov [imm32], eax
            AppendByte(out, 0xA3);
            AppendU32(out, static_cast<uint32_t>(resultAddr));
        }
    }
    if (errorAddr != 0 && getLastErrorAddr != 0) {
        if (x64) {
            // mov rax, GetLastError; call rax; mov [errorAddr], rax
            AppendByte(out, 0x48); AppendByte(out, 0xB8);
            AppendU64(out, getLastErrorAddr);
            AppendByte(out, 0xFF); AppendByte(out, 0xD0);
            AppendByte(out, 0x48); AppendByte(out, 0xA3);
            AppendU64(out, errorAddr);
        } else {
            // mov eax, GetLastError; call eax; mov [errorAddr], eax
            AppendByte(out, 0xB8); AppendU32(out, static_cast<uint32_t>(getLastErrorAddr));
            AppendByte(out, 0xFF); AppendByte(out, 0xD0);
            AppendByte(out, 0xA3); AppendU32(out, static_cast<uint32_t>(errorAddr));
        }
    }
    if (x64) {
        // mov rcx, -2 (GetCurrentThread)
        AppendByte(out, 0x48); AppendByte(out, 0xB9);
        AppendU64(out, static_cast<uint64_t>(-2));
        // mov rdx, contextPtr
        AppendByte(out, 0x48); AppendByte(out, 0xBA); AppendU64(out, contextPtr);
        // mov rax, NtSetContextThread
        AppendByte(out, 0x48); AppendByte(out, 0xB8); AppendU64(out, setContextAddr);
        // call rax
        AppendByte(out, 0xFF); AppendByte(out, 0xD0);
        // mov rax, originalIp; jmp rax
        AppendByte(out, 0x48); AppendByte(out, 0xB8); AppendU64(out, originalIp);
        AppendByte(out, 0xFF); AppendByte(out, 0xE0);
    } else {
        // push contextPtr; push -2
        AppendByte(out, 0x68); AppendU32(out, static_cast<uint32_t>(contextPtr));
        AppendByte(out, 0x6A); AppendByte(out, 0xFE);
        // mov eax, NtSetContextThread; call eax
        AppendByte(out, 0xB8); AppendU32(out, static_cast<uint32_t>(setContextAddr));
        AppendByte(out, 0xFF); AppendByte(out, 0xD0);
        // mov eax, originalIp; jmp eax
        AppendByte(out, 0xB8); AppendU32(out, static_cast<uint32_t>(originalIp));
        AppendByte(out, 0xFF); AppendByte(out, 0xE0);
    }
    return true;
}

bool SnapshotTarget(DWORD pid, const std::wstring& dllBaseNameLower,
                    TargetObservable& out, std::wstring& err) {
    out = TargetObservable{};
    out.moduleInList = FindRemoteModule(pid, dllBaseNameLower) != nullptr;

    HANDLE process = nullptr;
    if (!OpenTarget(pid, 0, process, err)) return false;

    // 用 VirtualQueryEx 扫描可执行且非映像映射的区域
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    int count = 0;
    wchar_t detail[512]{};
    int detailLen = 0;
    while (addr < maxAddr && count < 32) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQueryEx(process, reinterpret_cast<const void*>(addr), &mbi,
                           sizeof(mbi)) == 0) {
            addr += 0x10000;
            continue;
        }
        const bool executable =
            (mbi.Protect & 0xF0) != 0 ||
            (mbi.Protect == PAGE_EXECUTE) || (mbi.Protect == PAGE_EXECUTE_READ) ||
            (mbi.Protect == PAGE_EXECUTE_READWRITE) || (mbi.Protect == PAGE_EXECUTE_WRITECOPY);
        const bool imageBacked =
            (mbi.Type == MEM_IMAGE) || (mbi.State != MEM_COMMIT);
        if (mbi.State == MEM_COMMIT && executable && !imageBacked) {
            ++count;
            if (detailLen < 300) {
                detailLen += swprintf_s(detail + detailLen, 512 - detailLen,
                    L" [%p,%zu,%X]",
                    mbi.BaseAddress, static_cast<size_t>(mbi.RegionSize),
                    static_cast<unsigned>(mbi.Protect));
            }
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    out.execRegionCount = count;
    out.detail = detail;
    CloseHandle(process);
    return true;
}

}  // namespace detail
}  // namespace inject
}  // namespace windowmode
