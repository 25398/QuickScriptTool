// =============================================================================
// InjectionTestTarget.exe —— 注入测试靶进程
// -----------------------------------------------------------------------------
// - 创建隐藏窗口线程（消息循环，供 SetWindowsHook / 经典注入）
// - 创建可告警等待线程（供 APC 注入）
// - 创建普通工作线程（供线程劫持）
// stdout 输出 "READY <pid>"（ASCII，WriteFile 直写，避免编码问题）。
// =============================================================================

#include <windows.h>

#include <cstdio>

namespace {

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    wchar_t path[MAX_PATH * 2]{};
    wcscat_s(path, temp);
    wcscat_s(path, L"qst_itt_crash_");
    wchar_t pid[32]{};
    swprintf_s(pid, L"%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    wcscat_s(path, pid);
    wcscat_s(path, L".log");
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[160]{};
        const int n = sprintf_s(buf, sizeof(buf),
            "exception=0x%08X addr=0x%p rip=0x%p\n",
            static_cast<unsigned>(ep->ExceptionRecord->ExceptionCode),
            ep->ExceptionRecord->ExceptionAddress,
            reinterpret_cast<void*>(static_cast<uintptr_t>(ep->ContextRecord->Rip)));
        DWORD written = 0;
        WriteFile(h, buf, static_cast<DWORD>(n), &written, nullptr);
        CloseHandle(h);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI AlertableWorker(LPVOID) {
    // 短可告警等待：APC 可在 50ms 内执行，同时避免“长系统调用中劫持”的卡死
    for (;;) SleepEx(50, TRUE);
    __assume(0);
}

DWORD WINAPI HijackWorker(LPVOID) {
    for (;;) Sleep(25);
    __assume(0);
}

void WriteReady(DWORD pid) {
    char buf[64]{};
    int n = 0;
    const char head[] = "READY ";
    for (int i = 0; head[i]; ++i) buf[n++] = head[i];
    char tmp[16]{};
    int m = 0;
    if (pid == 0) tmp[m++] = '0';
    while (pid > 0 && m < 15) {
        tmp[m++] = static_cast<char>('0' + (pid % 10));
        pid /= 10;
    }
    while (m > 0 && n < 60) buf[n++] = tmp[--m];
    buf[n++] = '\n';
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteFile(out, buf, static_cast<DWORD>(n), &written, nullptr);
}

}  // namespace

int wmain() {
    SetUnhandledExceptionFilter(CrashFilter);
    // 真实 GUI 进程通常已加载 dwmapi；手动映射要求导入依赖已存在于目标进程
    LoadLibraryW(L"dwmapi.dll");
    const wchar_t kClass[] = L"QstInjectionTestTargetClass";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);

    // 隐藏顶层窗口（不显示、不出现在任务栏）；定时器持续产生消息，供钩子注入触发
    HWND hwnd = CreateWindowExW(0, kClass, L"QstInjectionTestTarget", WS_OVERLAPPED,
                                0, 0, 320, 200, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (hwnd) SetTimer(hwnd, 1, 250, nullptr);

    WriteReady(GetCurrentProcessId());

    HANDLE a = CreateThread(nullptr, 0, AlertableWorker, nullptr, 0, nullptr);
    HANDLE h = CreateThread(nullptr, 0, HijackWorker, nullptr, 0, nullptr);
    if (a) CloseHandle(a);
    if (h) CloseHandle(h);

    // PeekMessage 循环：持续触发 WH_GETMESSAGE 钩子路径
    for (;;) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
    __assume(0);
}
