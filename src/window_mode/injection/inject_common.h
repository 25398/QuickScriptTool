#pragma once

// 注入技术库内部公共工具（不对外部模块暴露）

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace windowmode {
namespace inject {
namespace detail {

std::wstring WinErrorText(DWORD code);

// 打开目标进程并请求指定访问权。失败时写入 err。
bool OpenTarget(DWORD pid, DWORD extraAccess, HANDLE& out, std::wstring& err);

// 目标是否为 WOW64（32 位）进程。
bool TargetIsWow64(HANDLE process, bool& outWow64, std::wstring& err);

// 按目标架构选择 64/32 位 DLL 文件名并拼接主程序目录。
bool ResolveArchDll(DWORD pid, const wchar_t* name64, const wchar_t* name32,
                    std::wstring& outPath, std::wstring& err);

std::wstring BaseNameOnly(const std::wstring& path);
std::wstring ModuleDirectory();

// 查找远端已加载模块（大小写不敏感，按文件名匹配）。
// Toolhelp 对 Unity 等模块极多的进程常返回 ERROR_BAD_LENGTH；失败后走
// EnumProcessModulesEx，再走 PEB InLoadOrder（WOW64 用 32 位 PEB）。
HMODULE FindRemoteModule(DWORD pid, const std::wstring& dllBaseNameLower);
// 仅走 PEB（自检用；与 Toolhelp 应对上同一 kernel32 基址）。
HMODULE FindRemoteModuleViaPeb(HANDLE process, const std::wstring& dllBaseNameLower);
bool WaitForRemoteModule(DWORD pid, const std::wstring& dllBaseNameLower,
                         DWORD timeoutMs, HMODULE& out, std::wstring& err);

// 目标「本机架构」PEB：WOW64 返回 32 位 PEB（含 kernel32），64 位返回 64 位 PEB。
bool QueryRemotePeb(HANDLE process, uintptr_t& peb, bool& wow64, std::wstring& err);

// 在目标进程内解析某个已加载模块的导出函数地址（WOW64 目标时从远端读取导出表，
// 避免本机 64 位 kernel32 地址被写入 32 位进程）。返回 0 表示失败。
uintptr_t ResolveRemoteProcAddress(HANDLE process, DWORD pid,
                                   const wchar_t* moduleBaseName,
                                   const char* procName, std::wstring& err);

bool ReadRemoteBytes(HANDLE process, const void* addr, void* buf,
                     SIZE_T size, std::wstring& err);
bool WriteRemoteBytes(HANDLE process, void* addr, const void* buf,
                      SIZE_T size, std::wstring& err);

// 在远端运行一个函数（CreateRemoteThread 封装），等待完成并取退出码。
bool RunRemoteThread(HANDLE process, void* start, void* arg,
                     DWORD timeoutMs, DWORD* exitCode, std::wstring& err);

// PE 导出名是否等于 exportName，或 x86 stdcall 装饰名 _Name@N / Name@N。
bool ExportNameMatches(const char* peName, const char* exportName);

// 从 PE 文件字节解析导出函数 RVA（不加载 DLL，支持 32/64 位）。
bool FindExportRva(const std::vector<uint8_t>& pe, const char* exportName,
                   DWORD& outRva, std::wstring& err);

// LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES) + GetProcAddress，含 x86 stdcall 装饰名。
bool FindExportRvaByLocalImage(const std::wstring& dllPath, const char* exportName,
                               DWORD& outRva, std::wstring& err);

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out,
                   std::wstring& err);
bool XorBytes(const std::vector<uint8_t>& in, uint8_t key,
              std::vector<uint8_t>& out);
bool XorFile(const std::wstring& inPath, const std::wstring& outPath,
             uint8_t key, std::wstring& err);

// 只读 PE 文件视图：按节表做 RVA<->文件偏移换算，支持 PE32/PE32+。
struct PeFileView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t peOff = 0;
    bool x64 = false;
    uint16_t magic = 0;
    uint16_t numSections = 0;
    uint32_t optSize = 0;
    const uint8_t* sections = nullptr;  // IMAGE_SECTION_HEADER 数组

    bool Init(const std::vector<uint8_t>& bytes, std::wstring& err);
    const uint8_t* RvaToPtr(uint32_t rva) const;

    uint32_t OptU32(uint32_t optOff) const;   // OptionalHeader 字段（4 字节）
    uint64_t OptU64(uint32_t optOff) const;   // OptionalHeader 字段（8 字节）
    uint32_t OptEntryRva() const;
    uint64_t OptImageBase() const;
    uint32_t OptSectionAlignment() const;
    uint32_t OptFileAlignment() const;
    uint32_t OptSizeOfImage() const;
    uint32_t OptSizeOfHeaders() const;
    uint32_t OptDirRva(int index) const;      // DataDirectory[index].VirtualAddress
    uint32_t OptDirSize(int index) const;     // DataDirectory[index].Size
};

// 小端读取工具（注入库内部共用）
uint16_t PeU16(const uint8_t* p);
uint32_t PeU32(const uint8_t* p);
uint64_t PeU64(const uint8_t* p);

// 生成“调用 3 参函数”的桩字节（x64 / x86），用于线程劫持与手动映射入口。
// 入口约定：call3(fn, a1, a2, a3)。
bool BuildCall3Stub(bool x64, uintptr_t fn, uintptr_t a1, uintptr_t a2,
                    uintptr_t a3, std::vector<uint8_t>& out, std::wstring& err);

// 生成“调用 3 参函数 -> NtSetContextThread 恢复原上下文 -> 跳回原 RIP/EIP”的桩。
bool BuildHijackStub(bool x64, uintptr_t fn, uintptr_t a1, uintptr_t a2,
                     uintptr_t a3, uintptr_t contextPtr, uintptr_t setContextAddr,
                     uintptr_t originalIp, uintptr_t startedAddr, uintptr_t resultAddr,
                     uintptr_t errorAddr, uintptr_t getLastErrorAddr,
                     std::vector<uint8_t>& out, std::wstring& err);

// 目标可观测状态快照（对抗测试驱动用）：模块列表可见性 + 非映像可执行区域。
struct TargetObservable {
    bool moduleInList = false;
    int execRegionCount = 0;
    std::wstring detail;
};
bool SnapshotTarget(DWORD pid, const std::wstring& dllBaseNameLower,
                    TargetObservable& out, std::wstring& err);

}  // namespace detail
}  // namespace inject
}  // namespace windowmode
