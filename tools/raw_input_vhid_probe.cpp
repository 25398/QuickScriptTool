// Minimal Raw Input logger for verifying QstVHid / VHF injection.
// Usage: raw_input_vhid_probe.exe <logPath> <seconds>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

FILE* gLog = nullptr;
volatile long gEvents = 0;
volatile long gVhfEvents = 0;

void LogLine(const char* line) {
    if (!gLog) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(gLog, "[%02u:%02u:%02u.%03u] %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    fflush(gLog);
}

bool NameLooksVhf(const std::wstring& name) {
    if (name.find(L"HID_DEVICE_SYSTEM_VHF") != std::wstring::npos) return true;
    if (name.find(L"VID_5153") != std::wstring::npos) return true;
    if (name.find(L"PID_5648") != std::wstring::npos) return true;
    return false;
}

std::wstring DeviceName(HANDLE hDevice) {
    UINT size = 0;
    GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, nullptr, &size);
    if (size == 0 || size > 4096) return L"";
    std::vector<wchar_t> name(size);
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, name.data(), &size) == (UINT)-1) {
        return L"";
    }
    return std::wstring(name.data());
}

void HandleRawInput(HRAWINPUT hRaw) {
    UINT size = 0;
    GetRawInputData(hRaw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0 || size > 4096) return;
    std::vector<BYTE> buf(size);
    if (GetRawInputData(hRaw, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        return;
    }
    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buf.data());
    std::wstring name = DeviceName(raw->header.hDevice);
    const bool vhf = NameLooksVhf(name);
    char nameUtf8[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nameUtf8, 255, nullptr, nullptr);

    char line[384];
    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        InterlockedIncrement(&gEvents);
        if (vhf) InterlockedIncrement(&gVhfEvents);
        _snprintf_s(line, sizeof(line), _TRUNCATE,
            "%s KEY vk=0x%02X scan=0x%02X flags=0x%04X name=%s",
            vhf ? "VHF" : "OTH",
            (unsigned)raw->data.keyboard.VKey,
            (unsigned)raw->data.keyboard.MakeCode,
            (unsigned)raw->data.keyboard.Flags,
            nameUtf8);
        LogLine(line);
    } else if (raw->header.dwType == RIM_TYPEMOUSE) {
        const unsigned flags = raw->data.mouse.usButtonFlags;
        const long dx = raw->data.mouse.lLastX;
        const long dy = raw->data.mouse.lLastY;
        if (flags == 0 && dx == 0 && dy == 0) return;
        InterlockedIncrement(&gEvents);
        if (vhf) InterlockedIncrement(&gVhfEvents);
        _snprintf_s(line, sizeof(line), _TRUNCATE,
            "%s MOUSE btnFlags=0x%04X d=(%ld,%ld) name=%s",
            vhf ? "VHF" : "OTH",
            flags, dx, dy, nameUtf8);
        LogLine(line);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INPUT:
        HandleRawInput((HRAWINPUT)lp);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr, L"Usage: raw_input_vhid_probe.exe <logPath> <seconds>\n");
        return 2;
    }
    const wchar_t* logPath = argv[1];
    const int seconds = _wtoi(argv[2]);
    if (seconds <= 0) return 2;

    gLog = _wfopen(logPath, L"wb");
    if (!gLog) {
        fwprintf(stderr, L"Cannot open log\n");
        return 3;
    }
    LogLine("probe_open_ok");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QstRawInputVhidProbe";
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (!RegisterClassW(&wc)) {
        LogLine("RegisterClass failed");
        fclose(gLog);
        return 4;
    }
    LogLine("RegisterClass_ok");

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"probe", WS_OVERLAPPED,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        char b[64];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "CreateWindow failed gle=%lu", GetLastError());
        LogLine(b);
        fclose(gLog);
        return 5;
    }
    // Keep invisible but not HWND_MESSAGE — some Raw Input sinks behave better with a real HWND.
    ShowWindow(hwnd, SW_HIDE);
    LogLine("CreateWindow_ok");

    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x06;
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x02;
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        char b[64];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "RegisterRawInputDevices failed gle=%lu", GetLastError());
        LogLine(b);
        DestroyWindow(hwnd);
        fclose(gLog);
        return 6;
    }
    LogLine("probe_started");

    const DWORD until = GetTickCount() + (DWORD)seconds * 1000u;
    MSG msg = {};
    while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
done:
    char summary[128];
    _snprintf_s(summary, sizeof(summary), _TRUNCATE,
        "probe_done events=%ld vhf_events=%ld", gEvents, gVhfEvents);
    LogLine(summary);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    fclose(gLog);
    gLog = nullptr;
    printf("%s\n", summary);
    return (gVhfEvents > 0) ? 0 : 1;
}
