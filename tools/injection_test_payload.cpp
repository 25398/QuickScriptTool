// =============================================================================
// InjectionTestPayload.dll —— 注入测试标记载荷
// -----------------------------------------------------------------------------
// 仅依赖 kernel32/user32（无 CRT），保证手动映射路径可解析导入。
// DLL_PROCESS_ATTACH 时在 %TEMP% 写标记文件：
//   QstInjectionTestPayload_<pid>.marker
// 同时导出 TestHookProc 供 SetWindowsHookEx 技术测试。
// =============================================================================

#include <windows.h>

// 无 CRT 环境下编译器为数组零初始化生成的 memset
#pragma function(memset)
extern "C" void* memset(void* dst, int c, size_t n) {
    unsigned char* p = static_cast<unsigned char*>(dst);
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(c);
    return dst;
}

namespace {

void UIntToAscii(unsigned long value, char* out, size_t cap) {
    char tmp[16]{};
    int n = 0;
    if (value == 0) tmp[n++] = '0';
    while (value > 0 && n < 15) {
        tmp[n++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    size_t len = 0;
    while (n > 0 && len + 1 < cap) out[len++] = tmp[--n];
    out[len] = '\0';
}

void UIntToWide(unsigned long value, wchar_t* out, size_t cap) {
    wchar_t tmp[16]{};
    int n = 0;
    if (value == 0) tmp[n++] = L'0';
    while (value > 0 && n < 15) {
        tmp[n++] = static_cast<wchar_t>(L'0' + (value % 10));
        value /= 10;
    }
    size_t len = 0;
    while (n > 0 && len + 1 < cap) out[len++] = tmp[--n];
    out[len] = L'\0';
}

void AppendW(wchar_t* dst, size_t cap, const wchar_t* src) {
    size_t n = 0;
    while (dst[n] && n + 1 < cap) ++n;
    for (size_t i = 0; src[i] && n + 1 < cap; ++i) dst[n++] = src[i];
    dst[n] = L'\0';
}

void WriteMarker(const char* reason) {
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp) == 0) return;
    wchar_t pidStr[16]{};
    UIntToWide(GetCurrentProcessId(), pidStr, 16);
    wchar_t path[MAX_PATH * 2]{};
    AppendW(path, MAX_PATH * 2, temp);
    AppendW(path, MAX_PATH * 2, L"QstInjectionTestPayload_");
    AppendW(path, MAX_PATH * 2, pidStr);
    AppendW(path, MAX_PATH * 2, L".marker");

    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[128]{};
    size_t len = 0;
    const char head[] = "attach pid=";
    for (size_t i = 0; head[i] && len + 1 < sizeof(buf); ++i) buf[len++] = head[i];
    char pidAscii[16]{};
    UIntToAscii(GetCurrentProcessId(), pidAscii, 16);
    for (size_t i = 0; pidAscii[i] && len + 1 < sizeof(buf); ++i) buf[len++] = pidAscii[i];
    const char tail[] = " reason=";
    for (size_t i = 0; tail[i] && len + 1 < sizeof(buf); ++i) buf[len++] = tail[i];
    const char* r = reason;
    for (size_t i = 0; r[i] && len + 1 < sizeof(buf); ++i) buf[len++] = r[i];
    buf[len++] = '\n';
    DWORD written = 0;
    WriteFile(h, buf, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
}

}  // namespace

extern "C" __declspec(dllexport) LRESULT CALLBACK TestHookProc(
    int nCode, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        WriteMarker("attach");
    } else if (reason == DLL_PROCESS_DETACH) {
        WriteMarker("detach");
    }
    return TRUE;
}
