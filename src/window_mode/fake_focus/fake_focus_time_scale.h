#pragma once

// =============================================================================
// fake_focus_time_scale.h — 目标进程内「变速齿轮」（speedhack）
// =============================================================================
// 目标：回放键鼠录制时，让**目标窗口自己的时钟**与脚本按同一倍率流逝，
//       这样游戏冷却 / 动画 / 帧间隔会跟着脚本一起加速，脚本才不会「跑在冷却前面」。
//
// 做法（与 Cheat Engine / GearNT 等 speedhack 同源）：**内联钩（改函数体）**。
// 把目标进程里几个「读时钟」的 API 入口换成我们的实现，返回
//     virtual = anchor + (real - anchor) * speed
// 而 **QueryPerformanceFrequency 保持原值** —— 目标看到的是「时间过得更快」，
// 而不是「时钟频率变了」。
//
// ⚠ 为什么不能只改 IAT 槽（本文件最重要的一条教训）：
//   改 IAT 槽是原子的、更安全，但对**在启动时就把函数地址缓存下来**的目标完全无效。
//   实测《植物大战僵尸融合版》（Unity）：IAT 补了 239 个槽、倍率也正确下发到 DLL、
//   诊断显示一切正常，游戏照样不变速 —— Unity 早就把 QPC 的地址存进自己的结构体了，
//   之后不再走导入表。改成改函数体后，无论调用方是走 IAT、还是用缓存指针、
//   还是 GetProcAddress 现取，都必然经过我们换掉的那条指令。
//
// 为什么不需要 trampoline / 指令长度解码器：
//   detour **从不调用原函数**。真实时间改从**未被挂钩**的 ntdll 原生调用取：
//       NtQueryPerformanceCounter  → 真实 QPC（且顺带给出频率）
//       NtQuerySystemTime          → 真实 FILETIME
//   所以被覆盖掉的那十几个字节可以永久丢弃，卸载时再写回即可。
//   （推论：**不能**对本文件的钩子用 CallThroughOriginal —— 被覆盖的字节里含半条
//     指令，把原字节恢复后直接调用会崩。这是刻意的设计，别「顺手补上」。）
//
// 已知取舍：改代码段有一个极短的撕裂窗口（写入跳转期间恰有线程执行到函数头）。
//   这是 CE / Detours / Frida 等所有内联钩方案的共同取舍；写入用 VirtualProtect +
//   一次性 memcpy + FlushInstructionCache，并且只在需要时安装（倍率 1.0 时完全不挂钩）。
//
// 开关语义（由宿主经共享内存下发 timeScaleMilli）：
//   0     → 完全卸载，目标进程里不留任何痕迹（功能关闭 / 回放结束）
//   1000  → 挂钩已装但原样转发（功能开启、当前原速）
//   >1000 → 按倍率缩放
// =============================================================================

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "window_mode/time_scale_clock.h"
#include "window_mode/fake_focus/fake_focus_unity_timescale.h"

namespace fakefocus {
namespace timescale {

// ── 真实时间源：全部走**未被挂钩**的 ntdll 原生调用 ──────────────────────
// 关键：一旦钩了 RtlQueryPerformanceCounter / GetTickCount64 /
// GetSystemTimeAsFileTime，就绝不能再拿它们当真实时间源（会无限递归）。
// ntdll 的 Nt* 是它们的下层，我们不碰，所以安全。
struct RealTimeApi {
    LONG(NTAPI* ntQpc)(LARGE_INTEGER* counter, LARGE_INTEGER* frequency) = nullptr;
    LONG(NTAPI* ntSysTime)(LARGE_INTEGER* systemTime) = nullptr;
    uint64_t freq = 0;
};

inline RealTimeApi g_real{};

/// 真实 QPC。返回 0 表示时间源不可用（此时 detour 会退化成返回 0）。
inline uint64_t RealQpcNow() {
    if (!g_real.ntQpc) return 0;
    LARGE_INTEGER li{};
    if (g_real.ntQpc(&li, nullptr) != 0) return 0;
    return static_cast<uint64_t>(li.QuadPart);
}

/// 轮询线程自己的计时**必须**走真实时间源（毫秒）。
/// 挂钩生效后 GetTickCount64() 返回的是虚拟时间 —— 若用它算「距上次重设锚点多久」，
/// 倍率越高间隔越短（2x → 1000ms，4x → 500ms），高倍率下会疯狂重设锚点。
inline uint64_t RealTick64() {
    if (!g_real.freq) return 0;
    return (RealQpcNow() * 1000ull) / g_real.freq;
}

/// 真实 FILETIME（100ns 单位，epoch 与 FILETIME 一致）。
inline uint64_t RealFileTimeNow() {
    if (!g_real.ntSysTime) return 0;
    LARGE_INTEGER li{};
    if (g_real.ntSysTime(&li) != 0) return 0;
    return static_cast<uint64_t>(li.QuadPart);
}

// ── 虚拟时钟状态（seqlock：写者只有轮询线程，读者是所有被挂钩的调用）──
struct Snapshot {
    TimeScaleAnchors anchors{};
    uint32_t passThrough = 1;
};

inline volatile LONG g_seq = 0;
inline Snapshot g_state{};

inline Snapshot ReadSnapshot() {
    Snapshot s;
    for (int i = 0; i < 128; ++i) {
        const LONG before = InterlockedCompareExchange(&g_seq, 0, 0);
        if (before & 1) {
            YieldProcessor();
            continue;
        }
        s = g_state;
        MemoryBarrier();
        if (InterlockedCompareExchange(&g_seq, 0, 0) == before) return s;
    }
    return g_state;  // 兜底：写者长时间持锁（几乎不可能）
}

inline void WriteSnapshot(const Snapshot& s) {
    InterlockedIncrement(&g_seq);  // 奇数 = 写入中
    g_state = s;
    MemoryBarrier();
    InterlockedIncrement(&g_seq);
}

// ── 调用计数 ──────────────────────────────────────────────────────────
// 为什么必须数这个：本项目「变速不生效」查了整整 8 轮，前 7 轮都在猜。
// 有了计数就能一眼区分两种完全不同的失败：
//   · 计数不涨  → 钩子根本不在目标的调用路径上（钩错地址 / 目标不走这个 API）；
//   · 计数猛涨但游戏速度不变 → 时钟确实被改了，但游戏的速度不由它决定。
enum HookId : int {
    kHookQpc = 0,
    kHookTick64,
    kHookTick,
    kHookFileTime,
    kHookTimeGetTime,
    kHookCount,
};

inline volatile LONG g_localCalls[kHookCount]{};
/// 宿主侧共享内存里的计数数组（DLL 安装时注入）。非空时 detour 直接累加到那里，
/// 宿主随时可读，不需要 CreateRemoteThread。
inline uint32_t* g_callSink = nullptr;

inline void SetCallSink(uint32_t* sink) { g_callSink = sink; }
inline uint32_t HookCallCount(int id) {
    if (id < 0 || id >= kHookCount) return 0;
    return static_cast<uint32_t>(g_localCalls[id]);
}

inline void NoteCall(int id) {
    if (id < 0 || id >= kHookCount) return;
    InterlockedIncrement(&g_localCalls[id]);
    if (g_callSink) InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_callSink[id]));
}

// ── detour 实现 ────────────────────────────────────────────────────────
// 全部只依赖「真实时间源 + 快照」，不调用任何被挂钩的函数。
inline BOOL WINAPI Hook_QueryPerformanceCounter(LARGE_INTEGER* out) {
    NoteCall(kHookQpc);
    if (!out) return FALSE;
    const Snapshot s = ReadSnapshot();
    const uint64_t real = RealQpcNow();
    out->QuadPart = s.passThrough
        ? static_cast<LONGLONG>(real)
        : static_cast<LONGLONG>(VirtualQpc(s.anchors, real));
    return TRUE;
}

inline ULONGLONG WINAPI Hook_GetTickCount64() {
    NoteCall(kHookTick64);
    const Snapshot s = ReadSnapshot();
    if (s.passThrough) return static_cast<ULONGLONG>(RealTick64());
    const uint64_t virt = VirtualQpc(s.anchors, RealQpcNow());
    return static_cast<ULONGLONG>(VirtualTickMs(s.anchors, virt, g_real.freq));
}

inline DWORD WINAPI Hook_GetTickCount() {
    NoteCall(kHookTick);
    return static_cast<DWORD>(Hook_GetTickCount64());
}

inline void WINAPI Hook_GetSystemTimeAsFileTime(LPFILETIME out) {
    NoteCall(kHookFileTime);
    if (!out) return;
    const Snapshot s = ReadSnapshot();
    uint64_t ft = 0;
    if (s.passThrough) {
        ft = RealFileTimeNow();
    } else {
        const uint64_t virt = VirtualQpc(s.anchors, RealQpcNow());
        ft = VirtualFileTime(s.anchors, virt, g_real.freq);
    }
    out->dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFFull);
    out->dwHighDateTime = static_cast<DWORD>(ft >> 32);
}

/// winmm!timeGetTime 与 GetTickCount 同源（都是「开机至今毫秒」）。
inline DWORD WINAPI Hook_timeGetTime() {
    NoteCall(kHookTimeGetTime);
    return static_cast<DWORD>(Hook_GetTickCount64());
}

inline void* DetourForImportName(const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "QueryPerformanceCounter") == 0
        || std::strcmp(name, "RtlQueryPerformanceCounter") == 0)
        return reinterpret_cast<void*>(&Hook_QueryPerformanceCounter);
    if (std::strcmp(name, "GetTickCount") == 0)
        return reinterpret_cast<void*>(&Hook_GetTickCount);
    if (std::strcmp(name, "GetTickCount64") == 0)
        return reinterpret_cast<void*>(&Hook_GetTickCount64);
    if (std::strcmp(name, "GetSystemTimeAsFileTime") == 0)
        return reinterpret_cast<void*>(&Hook_GetSystemTimeAsFileTime);
    if (std::strcmp(name, "timeGetTime") == 0)
        return reinterpret_cast<void*>(&Hook_timeGetTime);
    return nullptr;
}

// ── 已加载模块映像范围（用来判定「这个地址是不是真的落在某个模块的代码里」）──
// 为什么不写死 kernel32/kernelbase/winmm 白名单：现代 Windows 的导入会落到 API set，
// 加载器把转发链走到底 —— 实测 QPC 最终落在 **ntdll.dll** 里。
struct ModuleRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;
};

inline constexpr int kMaxModuleRanges = 512;
inline ModuleRange g_modRanges[kMaxModuleRanges]{};
inline int g_modRangeCount = 0;

inline void RefreshModuleRanges(HMODULE self) {
    g_modRangeCount = 0;
    HMODULE mods[kMaxModuleRanges]{};
    DWORD needed = 0;
    using EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    auto enumMods = reinterpret_cast<EnumProcessModulesFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules"));
    DWORD count = 0;
    if (enumMods && enumMods(GetCurrentProcess(), mods,
                        static_cast<DWORD>(sizeof(mods)), &needed)) {
        count = needed / sizeof(HMODULE);
        if (count > kMaxModuleRanges) count = kMaxModuleRanges;
    } else {
        mods[0] = GetModuleHandleW(nullptr);
        count = mods[0] ? 1 : 0;
    }
    for (DWORD i = 0; i < count; ++i) {
        HMODULE m = mods[i];
        if (!m || m == self) continue;
        auto* b = reinterpret_cast<const BYTE*>(m);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
        g_modRanges[g_modRangeCount].begin = reinterpret_cast<uintptr_t>(b);
        g_modRanges[g_modRangeCount].end =
            reinterpret_cast<uintptr_t>(b) + nt->OptionalHeader.SizeOfImage;
        ++g_modRangeCount;
    }
}

inline bool AddressInAnyModule(const void* p) {
    if (!p) return false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    for (int i = 0; i < g_modRangeCount; ++i) {
        if (a >= g_modRanges[i].begin && a < g_modRanges[i].end) return true;
    }
    return false;
}

// ── 内联钩 ─────────────────────────────────────────────────────────────
inline constexpr int kMaxTimeHooks = 32;

struct TimeHookEntry {
    void* target = nullptr;
    void* detour = nullptr;
    BYTE original[16]{};
    size_t patchSize = 0;
    bool installed = false;
};

inline TimeHookEntry g_hooks[kMaxTimeHooks]{};
inline int g_hookCount = 0;

/// 往 dst 写一条「绝对跳转」，返回字节数。
/// x64：mov rax, imm64; jmp rax（12 字节）—— 不依赖 ±2GB 相对位移。
/// x86：push imm32; ret（6 字节）。
inline size_t WriteAbsJump(BYTE* dst, void* to) {
#if defined(_WIN64)
    dst[0] = 0x48;
    dst[1] = 0xB8;
    const UINT64 addr = reinterpret_cast<UINT64>(to);
    std::memcpy(dst + 2, &addr, sizeof(addr));
    dst[10] = 0xFF;
    dst[11] = 0xE0;
    return 12;
#else
    dst[0] = 0x68;  // push imm32
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(to));
    std::memcpy(dst + 1, &addr, sizeof(addr));
    dst[5] = 0xC3;  // ret
    return 6;
#endif
}

/// 把 target 函数体开头换成跳往 detour。同一地址只钩一次（QPC 与
/// RtlQueryPerformanceCounter 常常就是同一个地址，靠去重避免重复改）。
inline bool HookAddress(void* target, void* detour) {
    if (!target || !detour) return false;
    for (int i = 0; i < g_hookCount; ++i) {
        if (g_hooks[i].target == target) return true;
    }
    if (g_hookCount >= kMaxTimeHooks) return false;
    // 只钩「落在某个已加载模块内的代码」。这条同时挡住了两类误伤：
    //   · 槽里存着 0 / 野指针；
    //   · 槽里已经是我们自己的 detour（self 模块不在 g_modRanges 里）。
    if (!AddressInAnyModule(target)) return false;

    TimeHookEntry& h = g_hooks[g_hookCount];
    BYTE jump[16]{};
    const size_t n = WriteAbsJump(jump, detour);
    if (n > sizeof(h.original)) return false;
    std::memcpy(h.original, target, n);

    DWORD old = 0;
    if (!VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(target, jump, n);
    FlushInstructionCache(GetCurrentProcess(), target, n);
    VirtualProtect(target, n, old, &old);

    h.target = target;
    h.detour = detour;
    h.patchSize = n;
    h.installed = true;
    ++g_hookCount;
    return true;
}

inline void RemoveIatPatches();   // 定义在下面（IAT 补丁区）

inline void UnhookAll() {
    // IAT 模式只有槽补丁；内联模式两者都清。
    RemoveIatPatches();
    for (int i = 0; i < g_hookCount; ++i) {
        TimeHookEntry& h = g_hooks[i];
        if (!h.installed || !h.target) continue;
        DWORD old = 0;
        if (VirtualProtect(h.target, h.patchSize, PAGE_EXECUTE_READWRITE, &old)) {
            std::memcpy(h.target, h.original, h.patchSize);
            FlushInstructionCache(GetCurrentProcess(), h.target, h.patchSize);
            VirtualProtect(h.target, h.patchSize, old, &old);
        }
        h.installed = false;
        h.target = nullptr;
    }
    g_hookCount = 0;
}

// ── IAT 补丁（仅用于「只许改 IAT」的目标，如冒险岛）────────────────────
// 为什么需要这一套：冒险岛（nProtect GameGuard）有硬约束
// 「只装 IAT，禁止改函数体 / 方法体 JMP」（曾闪退）。
// 而改 IAT 槽只动导入表（数据），不碰代码页 ——
// 与它已有的键鼠 IAT 补丁同一类修改，风险相当。
// 代价：只能拦「走导入表」的调用，拦不住已缓存的函数指针。
//   （其实也是当初为什么把变速改成内联钩的原因 —— 但对冒险岛安全优先。）
inline constexpr int kMaxIatPatches = 512;

struct IatPatch {
    void** slot = nullptr;
    void* original = nullptr;
};

inline IatPatch g_iat[kMaxIatPatches]{};
inline int g_iatCount = 0;
inline bool g_iatOnly = false;   // true = 只改 IAT 槽（冒险岛）

/// 仅 IAT 模式（冒险岛等反作弊目标）。必须在安装前设置。
inline void SetIatOnlyMode(bool only) { g_iatOnly = only; }
inline bool IatOnlyMode() { return g_iatOnly; }

/// 只补「当前值确实指向某个已加载模块内的代码」的槽。
/// 8 字节对齐指针写入是**原子**的，不会撕裂 —— 这正是 IAT 方案相对内联钩的优势。
inline bool PatchSlot(void** slot, void* detour) {
    if (!slot || !detour || g_iatCount >= kMaxIatPatches) return false;
    void* cur = *slot;
    if (!cur) return false;
    if (!AddressInAnyModule(cur)) return false;
    if (cur == detour) return true;  // 已补过

    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = detour;
    VirtualProtect(slot, sizeof(void*), old, &old);

    g_iat[g_iatCount].slot = slot;
    g_iat[g_iatCount].original = cur;
    ++g_iatCount;
    return true;
}

inline void RemoveIatPatches() {
    for (int i = 0; i < g_iatCount; ++i) {
        IatPatch& p = g_iat[i];
        if (!p.slot || !p.original) continue;
        DWORD old = 0;
        if (!VirtualProtect(p.slot, sizeof(void*), PAGE_READWRITE, &old)) continue;
        *p.slot = p.original;
        VirtualProtect(p.slot, sizeof(void*), old, &old);
        p.slot = nullptr;
        p.original = nullptr;
    }
    g_iatCount = 0;
}

/// 扫描某个模块的导入表，把「这些时间 API 的槽里**实际存着的地址**」钩掉。
/// 加载器解析出来的那个地址，就是目标真正会执行到的那条指令 —— 比我们自己
/// 去猜「应该是哪个导出」可靠得多（转发桩 / API set / 直连实现都能覆盖）。
inline void HookModuleIatTargets(HMODULE mod) {
    if (!mod) return;
    auto* base = reinterpret_cast<BYTE*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                          .VirtualAddress;
    if (!rva) return;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + rva); desc->Name;
         ++desc) {
        const char* dll = reinterpret_cast<const char*>(base + desc->Name);
        // 只看真正提供这些 API 的模块，避免同名导入误伤。
        const bool provider =
            _stricmp(dll, "kernel32.dll") == 0
            || _stricmp(dll, "kernelbase.dll") == 0
            || _stricmp(dll, "winmm.dll") == 0
            || _stricmp(dll, "ntdll.dll") == 0
            || _strnicmp(dll, "api-ms-win-core-", 16) == 0;
        if (!provider) continue;
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* imp = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            void* detour = DetourForImportName(reinterpret_cast<const char*>(imp->Name));
            if (!detour) continue;
            if (g_iatOnly) {
                // 冒险岛路径：只改槽（数据），不碰代码页。
                PatchSlot(reinterpret_cast<void**>(&iat->u1.Function), detour);
            } else {
                void* cur = reinterpret_cast<void*>(iat->u1.Function);
                if (cur) HookAddress(cur, detour);
            }
        }
    }
}

inline int InstallTimeHooks() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&InstallTimeHooks), &self)) {
        self = nullptr;
    }
    RefreshModuleRanges(self);

    const int before = g_hookCount;
    const int beforeIat = g_iatCount;

    // ① 各模块导入表里实际存着的地址（覆盖走 IAT 的调用，含转发桩/API set/直连实现）。
    HMODULE mods[kMaxModuleRanges]{};
    DWORD needed = 0;
    using EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    auto enumMods = reinterpret_cast<EnumProcessModulesFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules"));
    DWORD count = 0;
    if (enumMods && enumMods(GetCurrentProcess(), mods,
                        static_cast<DWORD>(sizeof(mods)), &needed)) {
        count = needed / sizeof(HMODULE);
        if (count > kMaxModuleRanges) count = kMaxModuleRanges;
    } else {
        mods[0] = GetModuleHandleW(nullptr);
        count = mods[0] ? 1 : 0;
    }
    // IAT-only（冒险岛）：**只补目标自己目录下的模块**。
    // 原来连系统 DLL 的 IAT 槽一起补（实测 255 个槽），等于把进程里所有模块的
    // QPC/GetTickCount 调用面全虚拟化 —— 面太大，且用户实测「开倍速过一会就闪退」。
    // 游戏自己的代码必然在自己的模块里，系统 DLL 内部的调用不是我们要影响的对象。
    // 注意：本头文件**不包含 <string>**，别用 std::wstring（会 C2039 连锁报一堆未声明）。
    wchar_t exeDir[MAX_PATH]{};
    if (g_iatOnly) {
        wchar_t exePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) && exePath[0]) {
            for (int k = 0; exePath[k]; ++k) {
                if (exePath[k] != L'\\' && exePath[k] != L'/') continue;
                const int n = k + 1;   // 含分隔符
                if (n > 0 && n < MAX_PATH) {
                    memcpy(exeDir, exePath, sizeof(wchar_t) * static_cast<size_t>(n));
                    exeDir[n] = 0;
                }
            }
        }
    }
    size_t exeDirLen = 0;
    while (exeDirLen < MAX_PATH && exeDir[exeDirLen]) ++exeDirLen;
    for (DWORD i = 0; i < count; ++i) {
        if (mods[i] == self) continue;
        if (exeDirLen > 0) {
            wchar_t modPath[MAX_PATH]{};
            if (!GetModuleFileNameW(mods[i], modPath, MAX_PATH) || !modPath[0]) continue;
            // 大小写不敏感前缀比较（盘符/目录名大小写不定）
            if (_wcsnicmp(modPath, exeDir, exeDirLen) != 0) continue;
        }
        HookModuleIatTargets(mods[i]);
    }

    // IAT-only 模式（冒险岛）：到此为止。第 ② 步是内联钩函数体，
    // 而该类目标有硬约束「禁止改函数体」。
    if (g_iatOnly) return g_iatCount - beforeIat;

    // ② 系统 DLL 的导出地址 —— 覆盖「目标用 GetProcAddress 自己解析并缓存」的情况。
    //    注意 GetProcAddress 可能返回转发桩（kernel32!QueryPerformanceCounter 就是个
    //    jmp 桩），那也是目标会跳到的入口，所以桩和真实实现两个都钩。
    //    **不要**把 NtQueryPerformanceCounter / NtQuerySystemTime 放进来 —— 那是我们的
    //    真实时间源，钩了就递归。
    static const struct {
        const wchar_t* mod;
        const char* fn;
    } kSystemExports[] = {
        {L"kernel32.dll", "QueryPerformanceCounter"},
        {L"kernel32.dll", "GetTickCount"},
        {L"kernel32.dll", "GetTickCount64"},
        {L"kernel32.dll", "GetSystemTimeAsFileTime"},
        {L"kernelbase.dll", "QueryPerformanceCounter"},
        {L"kernelbase.dll", "GetTickCount"},
        {L"kernelbase.dll", "GetTickCount64"},
        {L"kernelbase.dll", "GetSystemTimeAsFileTime"},
        {L"ntdll.dll", "RtlQueryPerformanceCounter"},
        {L"winmm.dll", "timeGetTime"},
    };
    for (const auto& e : kSystemExports) {
        HMODULE m = GetModuleHandleW(e.mod);
        if (!m) continue;
        void* detour = DetourForImportName(e.fn);
        void* fn = reinterpret_cast<void*>(GetProcAddress(m, e.fn));
        if (fn && detour) HookAddress(fn, detour);
    }
    return g_hookCount - before;
}

// ── 对外控制 ───────────────────────────────────────────────────────────
inline volatile LONG g_installed = 0;   // 1 = 时间钩已装
inline volatile LONG g_scaleNum = 0;    // 0 = 关闭
inline volatile LONG g_pollStop = 0;
inline HANDLE g_pollThread = nullptr;
inline uint64_t g_lastRebaseTick = 0;
inline uint64_t g_lastScanTick = 0;

using DesiredScaleFn = uint32_t (*)();

inline void ApplyScale(uint32_t num) {
    Snapshot s = ReadSnapshot();
    RebaseAnchors(s.anchors, RealQpcNow(), RealTick64(), RealFileTimeNow(), num, g_real.freq);
    s.passThrough = TimeScaleIsIdentity(num) ? 1u : 0u;
    WriteSnapshot(s);
    g_scaleNum = static_cast<LONG>(num);
    g_lastRebaseTick = RealTick64();
    // Unity 游戏还要另外设 Time.timeScale —— 它才是 Unity 里真正决定
    // 「游戏速度」的东西（只改系统时钟对固定 deltaTime 的游戏无效，实测过）。
    //
    // ⚠⚠ IL2CPP 只在这里**惰性**解析（2026-10-03 修，原在 InitRealTimeApi 里无条件调）：
    //   `Init()` 会 attach 当前线程并 invoke 托管方法，代价与风险都高，而
    //   `InstallCommon` 是无条件装变速的 —— 用户没开变速时必须一个字节都不碰 IL2CPP。
    //   本函数只由**轮询线程**在 `desired != 0` 时调用 ⇒ attach 落在常驻线程上
    //   （退出前会 detach，见 `PollThreadProc`），不再落在装完就退的注入线程上。
    //   原速（num == 1000）时不做任何 IL2CPP 操作。
    if (!TimeScaleIsIdentity(num)
        && (fakefocus::unity::Available() || fakefocus::unity::Init())) {
        fakefocus::unity::SetTimeScale(static_cast<float>(num) / 1000.0f);
    }
}

/// 轮询一次宿主下发的倍率。返回 true 表示状态有变化。
inline bool PollOnce(uint32_t desired) {
    const uint32_t cur = static_cast<uint32_t>(g_scaleNum);
    if (desired == cur) {
        if (desired == 0) return false;
        bool changed = false;
        // 目标进程常在启动后懒加载渲染 / 音频 / 反作弊 DLL，这些模块的导入表
        // 在注入那一刻还不存在。既然轮询线程本来就在跑，就顺手补一遍 ——
        // 否则「开脚本前游戏刚切了场景」这类时序会让变速时灵时不灵。
        if (RealTick64() - g_lastScanTick > 500) {
            g_lastScanTick = RealTick64();
            changed = InstallTimeHooks() > 0;
        }
        // 定期重设锚点：把 delta*num 的乘积压在 64 位安全范围内
        if (g_lastRebaseTick && RealTick64() - g_lastRebaseTick > 2000) {
            ApplyScale(desired);
            changed = true;
        }
        return changed;
    }

    if (desired == 0) {
        // Unity 的 timeScale 必须无条件恢复，否则会把游戏留在变速状态。
        fakefocus::unity::Restore();
        if (g_installed) {
            UnhookAll();
            g_installed = 0;
        }
        g_scaleNum = 0;
        g_lastScanTick = 0;
        return true;
    }

    if (!g_installed) {
        // 目标进程可能还没加载任何导入这些 API 的模块 —— 退避 500ms 再试，
        // 别每 20ms 全量走一遍模块表。
        const uint64_t now = RealTick64();
        if (g_lastScanTick && now - g_lastScanTick < 500) return false;
        if (InstallTimeHooks() <= 0) {
            g_lastScanTick = now;
            g_scaleNum = 0;
            return false;
        }
        g_installed = 1;
        g_lastScanTick = now;
    }
    ApplyScale(desired);
    return true;
}

inline DWORD WINAPI PollThreadProc(LPVOID param) {
    auto fn = reinterpret_cast<DesiredScaleFn>(param);
    while (InterlockedCompareExchange(&g_pollStop, 0, 0) == 0) {
        uint32_t desired = fn ? fn() : 0;
        if (desired == 1) desired = 0;  // 1 是「关闭」的另一种写法
        PollOnce(desired);
        Sleep(20);
    }
    // ⚠ 本线程可能 attach 过 IL2CPP（ApplyScale → unity::Init/SetTimeScale）。
    //   线程退出前必须 detach：否则 IL2CPP 线程表里留下**已终止线程**的记录，
    //   下一次 GC 扫栈就是一次访问违例 ⇒ 整个游戏进程消失、无日志。
    fakefocus::unity::DetachCurrentThread();
    return 0;
}

inline void StartPoll(DesiredScaleFn fn) {
    if (g_pollThread) return;
    InterlockedExchange(&g_pollStop, 0);
    g_pollThread = CreateThread(nullptr, 0, &PollThreadProc,
        reinterpret_cast<LPVOID>(fn), 0, nullptr);
}

inline void StopPoll() {
    if (!g_pollThread) return;
    InterlockedExchange(&g_pollStop, 1);
    WaitForSingleObject(g_pollThread, 2000);
    CloseHandle(g_pollThread);
    g_pollThread = nullptr;
    PollOnce(0);  // 卸载钩子 + 复位
}

/// 诊断位（宿主 / 自检用来判断「装没装上、钩了几个函数、当前倍率多少」）：
///   bit0        = 时间钩已装
///   bit1        = 轮询线程在跑
///   bits 8..15  = 已安装的内联钩数量（饱和到 255）
///   bits 16..31 = 当前倍率定点值（1000 = 1x，0 = 关闭）
/// 钩数量单列出来很关键：它能区分「钩子没装」和「装了但一个地址都没钩上」。
inline DWORD Diag() {
    const DWORD bits = (g_installed ? 1u : 0u) | (g_pollThread ? 2u : 0u)
        | (g_iatOnly ? 64u : 0u)
        | (fakefocus::unity::Available() ? 16u : 0u)
        | (fakefocus::unity::State() & (1u << 17) ? 32u : 0u);
    const int total = g_iatOnly ? g_iatCount : g_hookCount;
    const DWORD hooks = static_cast<DWORD>(total > 255 ? 255 : total) & 0xFFu;
    return (static_cast<DWORD>(static_cast<uint32_t>(g_scaleNum)) << 16)
        | (hooks << 8) | bits;
}

/// 解析真实时间源。拿不到 NtQueryPerformanceCounter 就返回 false（放弃变速）。
inline bool InitRealTimeApi() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    g_real.ntQpc = reinterpret_cast<decltype(g_real.ntQpc)>(
        GetProcAddress(ntdll, "NtQueryPerformanceCounter"));
    g_real.ntSysTime = reinterpret_cast<decltype(g_real.ntSysTime)>(
        GetProcAddress(ntdll, "NtQuerySystemTime"));
    if (!g_real.ntQpc) return false;

    LARGE_INTEGER counter{};
    LARGE_INTEGER freq{};
    if (g_real.ntQpc(&counter, &freq) != 0 || freq.QuadPart <= 0) return false;
    g_real.freq = static_cast<uint64_t>(freq.QuadPart);

    // ⚠⚠ **不要**在这里解析 Unity IL2CPP（2026-10-03 修）。
    //   `unity::Init()` 会 `il2cpp_thread_attach` 当前线程、遍历所有程序集、
    //   再 `il2cpp_runtime_invoke(Time.get_timeScale)` —— 而本函数是 `InstallCommon`
    //   **无条件**调的（用户根本没开变速也会走到），于是：
    //     ① 注入线程（`CreateRemoteThread` 出来的临时线程）被 attach 到 IL2CPP，
    //        执行完 `FakeFocus_Install` 就退出，**没人 detach**
    //        ⇒ IL2CPP 线程表留下悬挂记录 ⇒ 下一次 GC 扫栈 = 访问违例
    //        ⇒ 整个游戏进程消失、无日志（`exit=0xC0000005`）；
    //     ② 非 Unity 目标只是白付一次遍历程序集的开销。
    //   ⇒ 改成**只在真正需要变速时**惰性解析，见 `ApplyScale()`。

    Snapshot s{};
    InitAnchors(s.anchors, RealQpcNow(), RealTick64(), RealFileTimeNow());
    s.passThrough = 1;
    WriteSnapshot(s);
    return true;
}

}  // namespace timescale
}  // namespace fakefocus
