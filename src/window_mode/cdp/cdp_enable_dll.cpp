#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#define TARGET_DLL "msedge.dll"
#define CDP_PORT 9222
#define WM_START_CDP (WM_USER + 0x1337)

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#pragma comment(lib, "user32.lib")

typedef struct {
    void* vtable;
    uint16_t port;
    uint8_t padding[6];
} TCPServerSocketFactory;

typedef struct {
    uint8_t data[24];
} FilePath;

typedef void (*StartFn)(void** factory_ptr, FilePath* out, FilePath* front, int mode);
typedef void* (*ChromeNewFn)(size_t size);
typedef void* (*GetDevToolsManagerFn)(void);

static GetDevToolsManagerFn g_get_devtools_manager = nullptr;
static HWND g_target_hwnd = nullptr;
static WNDPROC g_original_wndproc = nullptr;
static StartFn g_start_server = nullptr;
static void* g_factory_vtable = nullptr;
static ChromeNewFn g_chrome_new = nullptr;
static volatile LONG g_cdp_started = 0;

// Mid-function: mov ecx, SIZE; call operator new; mov r15, rax;
// mov rax, [rsi]; xor r12d, r12d; mov [rsi], r12
// Edge 138: SIZE=0x80, backtrack=51
// Edge/Chrome ~143: SIZE=0x88, backtrack=55/63
static const uint8_t SIG_START_MID_TAIL[] = {
    0xE8, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x89, 0xC7,
    0x48, 0x8B, 0x06,
    0x45, 0x31, 0xE4,
    0x4C, 0x89, 0x26
};
static const uint8_t SIG_START_MID_TAIL_MASK[] = {
    1, 0, 0, 0, 0,
    1, 1, 1,
    1, 1, 1,
    1, 1, 1,
    1, 1, 1
};
#define SIG_START_MID_TAIL_LEN 17

static const uint32_t SIG_START_ALLOC_SIZES[] = { 0x80, 0x88, 0x90, 0x98 };
static const int SIG_START_BACKTRACKS[] = { 51, 55, 63, 48, 50, 52, 56, 64 };

static const uint8_t SIG_START_SERVER_PROLOGUE[] = {
    0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57,
    0x53, 0x48, 0x83, 0xEC, 0x48
};
#define SIG_START_SERVER_PROLOGUE_LEN 13

static const uint8_t SIG_OPERATOR_NEW[] = {
    0x40, 0x53,
    0x48, 0x83, 0xEC, 0x20,
    0x48, 0x8B, 0xD9,
    0xEB, 0x00,
    0x48, 0x8B, 0xCB,
    0xE8, 0x00, 0x00, 0x00, 0x00,
    0x85, 0xC0,
    0x74, 0x00,
    0x48, 0x8B, 0xCB
};
static const uint8_t SIG_OPERATOR_NEW_MASK[] = {
    1, 1,
    1, 1, 1, 1,
    1, 1, 1,
    1, 0,
    1, 1, 1,
    1, 0, 0, 0, 0,
    1, 1,
    1, 0,
    1, 1, 1
};
#define SIG_OPERATOR_NEW_LEN 26

// Edge 138 TCPServerSocketFactory scalar deleting dtor: test edx,edx (not test dl,1)
static const uint8_t SIG_VTABLE_ENTRY0_E138[] = {
    0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCE,
    0x85, 0xD2, 0x74, 0x08, 0x48, 0x89, 0xF1,
    0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xF0,
    0x48, 0x83, 0xC4, 0x20, 0x5E, 0xC3
};
static const uint8_t SIG_VTABLE_ENTRY0_E138_MASK[] = {
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,
    1, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1
};
#define SIG_VTABLE_ENTRY0_E138_LEN 29

// Older Chrome/Edge: test dl, 1
static const uint8_t SIG_VTABLE_ENTRY0_OLD[] = {
    0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCE,
    0xF6, 0xC2, 0x01, 0x74, 0x08, 0x48, 0x89, 0xF1,
    0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xF0,
    0x48, 0x83, 0xC4, 0x20, 0x5E, 0xC3
};
static const uint8_t SIG_VTABLE_ENTRY0_OLD_MASK[] = {
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1
};
#define SIG_VTABLE_ENTRY0_OLD_LEN 30

// Edge 138 CreateForHttpServer
static const uint8_t SIG_VTABLE_ENTRY1_E138[] = {
    0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xD6,
    0x0F, 0xB7, 0x51, 0x08, 0x48, 0x89, 0xF1,
    0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xF0,
    0x48, 0x83, 0xC4, 0x20, 0x5E, 0xC3
};
static const uint8_t SIG_VTABLE_ENTRY1_E138_MASK[] = {
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,
    1, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1
};
#define SIG_VTABLE_ENTRY1_E138_LEN 29

// Older CreateForHttpServer (tail-call jmp)
static const uint8_t SIG_VTABLE_ENTRY1_OLD[] = {
    0x48, 0x89, 0xD0,
    0x0F, 0xB7, 0x51, 0x08,
    0x48, 0x89, 0xC1,
    0xE9, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t SIG_VTABLE_ENTRY1_OLD_MASK[] = {
    1, 1, 1,
    1, 1, 1, 1,
    1, 1, 1,
    1, 0, 0, 0, 0
};
#define SIG_VTABLE_ENTRY1_OLD_LEN 15

static void DebugLog(const char* fmt, ...) {
    char buf[512];
    char line[576];
    va_list args;
    va_start(args, fmt);
    wvsprintfA(buf, fmt, args);
    va_end(args);
    wsprintfA(line, "[CdpEnable64] %s", buf);
    OutputDebugStringA(line);
    OutputDebugStringA("\n");

    char tempPath[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tempPath);
    if (n == 0 || n >= MAX_PATH) return;
    char logPath[MAX_PATH];
    wsprintfA(logPath, "%sCdpEnable64.log", tempPath);
    HANDLE h = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line, (DWORD)lstrlenA(line), &written, nullptr);
    WriteFile(h, "\r\n", 2, &written, nullptr);
    CloseHandle(h);
}

static void InitEmptyFilePath(FilePath* fp) {
    memset(fp, 0, sizeof(FilePath));
}

static void* ScanForSignature(const uint8_t* start, size_t size,
    const uint8_t* sig, const uint8_t* mask, size_t sig_len) {
    if (size < sig_len || sig_len == 0) return nullptr;
    const uint8_t first = sig[0];
    const uint8_t* p = start;
    const uint8_t* end = start + size - sig_len;
    while (p <= end) {
        p = (const uint8_t*)memchr(p, first, (size_t)(end - p) + 1);
        if (!p) return nullptr;
        BOOL match = TRUE;
        for (size_t i = 1; i < sig_len && match; ++i) {
            if (mask == nullptr || mask[i]) {
                if (p[i] != sig[i]) match = FALSE;
            }
        }
        if (match && (mask == nullptr || mask[0] == 0 || p[0] == sig[0]))
            return (void*)p;
        ++p;
    }
    return nullptr;
}

static BOOL GetSectionInfo(HMODULE mod, const char* section_name,
    uint8_t** out_start, size_t* out_size) {
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;

    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;

    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (strncmp((char*)section->Name, section_name, 8) == 0) {
            *out_start = (uint8_t*)mod + section->VirtualAddress;
            *out_size = section->Misc.VirtualSize;
            return TRUE;
        }
    }
    return FALSE;
}

static int ScanForAllSignatures(const uint8_t* start, size_t size,
    const uint8_t* sig, const uint8_t* mask, size_t sig_len,
    void** out_matches, int max_matches) {
    int count = 0;
    if (size < sig_len || sig_len == 0) return 0;
    const uint8_t first = sig[0];
    const uint8_t* p = start;
    const uint8_t* end = start + size - sig_len;
    while (p <= end && count < max_matches) {
        p = (const uint8_t*)memchr(p, first, (size_t)(end - p) + 1);
        if (!p) break;
        BOOL match = TRUE;
        for (size_t i = 1; i < sig_len && match; ++i) {
            if (mask == nullptr || mask[i]) {
                if (p[i] != sig[i]) match = FALSE;
            }
        }
        if (match) out_matches[count++] = (void*)p;
        ++p;
    }
    return count;
}

static BOOL IsKnownAllocSize(uint32_t size) {
    for (size_t i = 0; i < sizeof(SIG_START_ALLOC_SIZES) / sizeof(SIG_START_ALLOC_SIZES[0]); ++i) {
        if (SIG_START_ALLOC_SIZES[i] == size) return TRUE;
    }
    return FALSE;
}

static void* ResolveStartServer(uint8_t* text_start, size_t text_size) {
    // Single pass: find unique_ptr-steal mid pattern, then backtrack to prologue.
    // Pattern: B9 xx xx 00 00 E8 ?? ?? ?? ?? 49 89 C7 48 8B 06 45 31 E4 4C 89 26
    const uint8_t* p = text_start;
    const uint8_t* end = text_start + text_size;
    if (text_size < 22) return nullptr;

    while (p + 22 <= end) {
        p = (const uint8_t*)memchr(p, 0xB9, (size_t)(end - p));
        if (!p || p + 22 > end) break;
        if (p[2] == 0x00 && p[3] == 0x00 && p[4] == 0x00 && p[5] == 0xE8
            && p[10] == 0x49 && p[11] == 0x89 && p[12] == 0xC7
            && p[13] == 0x48 && p[14] == 0x8B && p[15] == 0x06
            && p[16] == 0x45 && p[17] == 0x31 && p[18] == 0xE4
            && p[19] == 0x4C && p[20] == 0x89 && p[21] == 0x26) {
            const uint32_t allocSize = (uint32_t)p[1];
            if (IsKnownAllocSize(allocSize)) {
                for (size_t bi = 0; bi < sizeof(SIG_START_BACKTRACKS) / sizeof(SIG_START_BACKTRACKS[0]); ++bi) {
                    const int bt = SIG_START_BACKTRACKS[bi];
                    if (p - bt < text_start) continue;
                    uint8_t* candidate = (uint8_t*)(p - bt);
                    if (memcmp(candidate, SIG_START_SERVER_PROLOGUE, SIG_START_SERVER_PROLOGUE_LEN) == 0) {
                        DebugLog("ResolveStartServer: size=0x%X backtrack=%d at %p",
                            allocSize, bt, candidate);
                        return candidate;
                    }
                }
            }
        }
        ++p;
    }
    return nullptr;
}

static BOOL LooksLikeScalarDeletingDtor(const uint8_t* fn) {
    // push rsi; sub rsp,20; mov rsi,rcx; ...
    if (fn[0] != 0x56 || fn[1] != 0x48 || fn[2] != 0x83 || fn[3] != 0xEC || fn[4] != 0x20)
        return FALSE;
    if (fn[5] != 0x48 || fn[6] != 0x89 || fn[7] != 0xCE)
        return FALSE;
    return TRUE;
}

static void* FindVtable(uint8_t* text_start, size_t text_size,
    uint8_t* rdata_start, size_t rdata_size) {
    void* entry1_fn = ScanForSignature(text_start, text_size,
        SIG_VTABLE_ENTRY1_E138, SIG_VTABLE_ENTRY1_E138_MASK, SIG_VTABLE_ENTRY1_E138_LEN);
    if (!entry1_fn) {
        entry1_fn = ScanForSignature(text_start, text_size,
            SIG_VTABLE_ENTRY1_OLD, SIG_VTABLE_ENTRY1_OLD_MASK, SIG_VTABLE_ENTRY1_OLD_LEN);
    }
    DebugLog("FindVtable: entry1=%p", entry1_fn);
    if (!entry1_fn) return nullptr;

    const uint64_t http_addr = (uint64_t)entry1_fn;
    const uint64_t text_lo = (uint64_t)text_start;
    const uint64_t text_hi = (uint64_t)text_start + text_size;
    const uint64_t* p = (const uint64_t*)rdata_start;
    const uint64_t* end = (const uint64_t*)(rdata_start + rdata_size - 16);

    // Anchor on CreateForHttpServer (slot1); accept any plausible dtor in slot0.
    for (const uint64_t* q = p; q < end; ++q) {
        if (q[1] != http_addr) continue;
        if (q[0] < text_lo || q[0] >= text_hi) continue;
        if (!LooksLikeScalarDeletingDtor((const uint8_t*)q[0])) continue;
        DebugLog("FindVtable: vtable=%p dtor=%p", q, (void*)q[0]);
        return (void*)q;
    }

    // Fallback: pair classic dtor signatures with entry1 (older Edge/Chrome).
    void* destr_candidates[64];
    int destr_count = ScanForAllSignatures(text_start, text_size,
        SIG_VTABLE_ENTRY0_E138, SIG_VTABLE_ENTRY0_E138_MASK, SIG_VTABLE_ENTRY0_E138_LEN,
        destr_candidates, 64);
    if (destr_count == 0) {
        destr_count = ScanForAllSignatures(text_start, text_size,
            SIG_VTABLE_ENTRY0_OLD, SIG_VTABLE_ENTRY0_OLD_MASK, SIG_VTABLE_ENTRY0_OLD_LEN,
            destr_candidates, 64);
    }
    for (int i = 0; i < destr_count; ++i) {
        const uint64_t destr_addr = (uint64_t)destr_candidates[i];
        for (const uint64_t* q = p; q < end; ++q) {
            if (q[0] == destr_addr && q[1] == http_addr) {
                DebugLog("FindVtable: vtable=%p (paired)", q);
                return (void*)q;
            }
        }
    }
    return nullptr;
}

static BOOL ResolveSymbolsBySig(void** out_start_server,
    void** out_chrome_new, void** out_vtable) {
    HMODULE browser_dll = GetModuleHandleA(TARGET_DLL);
    if (!browser_dll) {
        DebugLog("ResolveSymbolsBySig: %s not loaded", TARGET_DLL);
        return FALSE;
    }

    uint8_t* text_start = nullptr;
    uint8_t* rdata_start = nullptr;
    size_t text_size = 0;
    size_t rdata_size = 0;
    if (!GetSectionInfo(browser_dll, ".text", &text_start, &text_size)) return FALSE;
    if (!GetSectionInfo(browser_dll, ".rdata", &rdata_start, &rdata_size)) return FALSE;

    int found = 0;
    void* start = ResolveStartServer(text_start, text_size);
    if (start) {
        *out_start_server = start;
        ++found;
    } else {
        DebugLog("ResolveSymbolsBySig: StartRemoteDebuggingServer not found");
    }

    void* addr = ScanForSignature(text_start, text_size, SIG_OPERATOR_NEW,
        SIG_OPERATOR_NEW_MASK, SIG_OPERATOR_NEW_LEN);
    if (addr) {
        *out_chrome_new = addr;
        ++found;
        DebugLog("ResolveSymbolsBySig: operator_new=%p", addr);
    } else {
        DebugLog("ResolveSymbolsBySig: operator_new not found");
    }

    void* vtable = FindVtable(text_start, text_size, rdata_start, rdata_size);
    if (vtable) {
        *out_vtable = vtable;
        ++found;
    } else {
        DebugLog("ResolveSymbolsBySig: TCPServerSocketFactory vtable not found");
    }

    return found == 3;
}

static void* DeriveGetInstance(void* start_server) {
    uint8_t* func = (uint8_t*)start_server;
    // Prefer known offset used by Chromium MSVC builds; else scan first call.
    uint8_t* check_addr = func + 0x25;
    if (check_addr[0] == 0xE8) {
        int32_t rel_offset = 0;
        memcpy(&rel_offset, check_addr + 1, sizeof(int32_t));
        return check_addr + 5 + rel_offset;
    }
    for (int off = 0x10; off < 0x60; ++off) {
        if (func[off] != 0xE8) continue;
        int32_t rel_offset = 0;
        memcpy(&rel_offset, func + off + 1, sizeof(int32_t));
        return func + off + 5 + rel_offset;
    }
    return nullptr;
}

static BOOL ResolveSymbols(void) {
    void* start_server = nullptr;
    void* chrome_new = nullptr;
    void* vtable = nullptr;

    if (!ResolveSymbolsBySig(&start_server, &chrome_new, &vtable)) {
        DebugLog("ResolveSymbols: signature resolution failed");
        return FALSE;
    }

    g_start_server = (StartFn)start_server;
    g_chrome_new = (ChromeNewFn)chrome_new;
    g_factory_vtable = vtable;
    g_get_devtools_manager = (GetDevToolsManagerFn)DeriveGetInstance(start_server);

    DebugLog("ResolveSymbols: start=0x%p new=0x%p vtable=0x%p getInstance=0x%p",
        g_start_server, g_chrome_new, g_factory_vtable, g_get_devtools_manager);
    return TRUE;
}

static int CallStartServerSafe(StartFn fn, void** factory_ptr,
    FilePath* out, FilePath* front, int mode) {
    __try {
        fn(factory_ptr, out, front, mode);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DebugLog("StartRemoteDebuggingServer exception: 0x%08X", GetExceptionCode());
        return 0;
    }
}

static void StartCDPServer(void) {
    TCPServerSocketFactory* factory = nullptr;
    void* factory_ptr = nullptr;
    FilePath output_dir;
    FilePath frontend_dir;

    InitEmptyFilePath(&output_dir);
    InitEmptyFilePath(&frontend_dir);

    if (InterlockedCompareExchange(&g_cdp_started, 1, 0) != 0) {
        DebugLog("StartCDPServer: already started");
        return;
    }

    if (!g_start_server || !g_factory_vtable) {
        DebugLog("StartCDPServer: missing symbols");
        InterlockedExchange(&g_cdp_started, 0);
        return;
    }

    if (g_get_devtools_manager) {
        void* manager = g_get_devtools_manager();
        if (manager) {
            void* delegate = *(void**)((char*)manager + 8);
            if (!delegate) {
                DebugLog("StartCDPServer: DevToolsManager delegate is NULL");
            }
        }
    }

    if (g_chrome_new) {
        factory = (TCPServerSocketFactory*)g_chrome_new(sizeof(TCPServerSocketFactory));
    } else {
        factory = (TCPServerSocketFactory*)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TCPServerSocketFactory));
    }
    if (!factory) {
        InterlockedExchange(&g_cdp_started, 0);
        return;
    }
    if (g_chrome_new) {
        memset(factory, 0, sizeof(TCPServerSocketFactory));
    }

    factory->vtable = g_factory_vtable;
    factory->port = CDP_PORT;
    factory_ptr = factory;

    DebugLog("StartCDPServer: calling StartRemoteDebuggingServer port=%u", (unsigned)CDP_PORT);
    if (!CallStartServerSafe(g_start_server, &factory_ptr, &output_dir, &frontend_dir, 0)) {
        InterlockedExchange(&g_cdp_started, 0);
        return;
    }
    DebugLog("StartCDPServer: CDP server started on port %u", (unsigned)CDP_PORT);
}

static LRESULT CALLBACK SubclassWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_START_CDP) {
        DebugLog("SubclassWndProc: WM_START_CDP on thread %lu", GetCurrentThreadId());
        StartCDPServer();
        if (g_original_wndproc) {
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)g_original_wndproc);
            g_original_wndproc = nullptr;
        }
        return 0;
    }
    return CallWindowProcA(g_original_wndproc, hwnd, msg, wParam, lParam);
}

struct EnumBrowserCtx {
    DWORD pid;
    HWND best;
    HWND anyTop;
};

static BOOL CALLBACK EnumBrowserWindowsProc(HWND hwnd, LPARAM lParam) {
    EnumBrowserCtx* ctx = (EnumBrowserCtx*)lParam;
    DWORD window_pid = 0;
    GetWindowThreadProcessId(hwnd, &window_pid);
    if (window_pid != ctx->pid) return TRUE;
    if (GetParent(hwnd) != nullptr) return TRUE;

    char cls[64] = {};
    if (GetClassNameA(hwnd, cls, sizeof(cls)) == 0) return TRUE;
    if (lstrcmpA(cls, "Chrome_WidgetWin_1") != 0) return TRUE;

    if (!ctx->anyTop) ctx->anyTop = hwnd;
    if (IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        ctx->best = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND FindBrowserWindow(void) {
    EnumBrowserCtx ctx = {};
    ctx.pid = GetCurrentProcessId();
    EnumWindows(EnumBrowserWindowsProc, (LPARAM)&ctx);
    if (ctx.best) return ctx.best;
    if (ctx.anyTop) {
        DebugLog("FindBrowserWindow: using non-visible top hwnd=%p", ctx.anyTop);
        return ctx.anyTop;
    }
    return nullptr;
}

static DWORD WINAPI InjectionThread(LPVOID param) {
    (void)param;
    DebugLog("InjectionThread: begin pid=%lu", GetCurrentProcessId());

    if (!ResolveSymbols()) return 1;

    g_target_hwnd = FindBrowserWindow();
    if (!g_target_hwnd) {
        DebugLog("InjectionThread: Chrome_WidgetWin_1 not found");
        return 1;
    }
    DebugLog("InjectionThread: target hwnd=%p", g_target_hwnd);

    g_original_wndproc = (WNDPROC)SetWindowLongPtrA(
        g_target_hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassWndProc);
    if (!g_original_wndproc) {
        DebugLog("InjectionThread: SetWindowLongPtr failed: %lu", GetLastError());
        return 1;
    }

    if (!PostMessageA(g_target_hwnd, WM_START_CDP, 0, 0)) {
        SetWindowLongPtrA(g_target_hwnd, GWLP_WNDPROC, (LONG_PTR)g_original_wndproc);
        g_original_wndproc = nullptr;
        DebugLog("InjectionThread: PostMessage failed: %lu", GetLastError());
        return 1;
    }

    DebugLog("InjectionThread: posted WM_START_CDP to 0x%p", g_target_hwnd);
    return 0;
}

extern "C" __declspec(dllexport) int CdpEnable_IsReady(void) {
    return g_cdp_started != 0 ? 1 : 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)hModule;
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        {
            HANDLE hThread = CreateThread(nullptr, 0, InjectionThread, nullptr, 0, nullptr);
            if (hThread) CloseHandle(hThread);
        }
        break;
    case DLL_PROCESS_DETACH:
        if (g_target_hwnd && g_original_wndproc) {
            SetWindowLongPtrA(g_target_hwnd, GWLP_WNDPROC, (LONG_PTR)g_original_wndproc);
            g_original_wndproc = nullptr;
        }
        break;
    }
    return TRUE;
}
