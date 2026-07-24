#include "cdp_enable_injector.h"

#include "window_mode/window_mode_log.h"
#include "window_mode/window_target.h"

#include <psapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "psapi.lib")

namespace windowmode {
namespace {

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring full(path);
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return full.substr(0, slash + 1);
}

bool PathExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring FormatWinError(DWORD code) {
    wchar_t buf[256]{};
    swprintf_s(buf, L"Win32=%lu", static_cast<unsigned long>(code));
    return buf;
}

bool HttpGetLocal(int port, const wchar_t* path, std::string& body) {
    body.clear();
    HINTERNET session = WinHttpOpen(L"QuickScriptTool-CdpEnable/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    HINTERNET connect = WinHttpConnect(session, L"127.0.0.1",
        static_cast<INTERNET_PORT>(port), 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD timeout = 1500;
    WinHttpSetOption(request, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(request, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

    const bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(request, buf.data(), avail, &read) || read == 0) break;
            body.append(buf.data(), read);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok && !body.empty();
}

bool CdpListReachable(int port) {
    if (port <= 0 || port > 65535) return false;
    std::string body;
    return HttpGetLocal(port, L"/json/list", body) || HttpGetLocal(port, L"/json", body);
}

int ReadDevToolsActivePort(const std::wstring& userDataDir) {
    const std::wstring path = userDataDir + L"\\DevToolsActivePort";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[64]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);
    if (!ok || read == 0) return 0;
    int port = 0;
    for (DWORD i = 0; i < read; ++i) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            port = port * 10 + (buf[i] - '0');
        } else {
            break;
        }
    }
    return port;
}

std::vector<int> CandidatePorts(int preferredPort) {
    std::vector<int> ports;
    auto add = [&](int p) {
        if (p <= 0 || p > 65535) return;
        if (std::find(ports.begin(), ports.end(), p) == ports.end()) ports.push_back(p);
    };

    add(preferredPort > 0 ? preferredPort : 9222);
    add(9222);
    add(8181);
    add(8182);

    wchar_t localApp[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) > 0) {
        add(ReadDevToolsActivePort(std::wstring(localApp) + L"\\Microsoft\\Edge\\User Data"));
    }
    return ports;
}

bool RemoteProcessHasModule(HANDLE process, const wchar_t* moduleName) {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(process, mods, sizeof(mods), &needed)) return false;

    const DWORD count = needed / sizeof(HMODULE);
    std::wstring wantLower = moduleName;
    std::transform(wantLower.begin(), wantLower.end(), wantLower.begin(), ::towlower);

    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleBaseNameW(process, mods[i], name, MAX_PATH) == 0) continue;
        std::wstring base = name;
        std::transform(base.begin(), base.end(), base.begin(), ::towlower);
        if (base == wantLower) return true;
    }
    return false;
}

HMODULE FindRemoteModule(DWORD pid, const std::wstring& dllPath) {
    const auto slash = dllPath.find_last_of(L"\\/");
    const std::wstring want = slash == std::wstring::npos
        ? dllPath : dllPath.substr(slash + 1);
    std::wstring wantLower = want;
    std::transform(wantLower.begin(), wantLower.end(), wantLower.begin(), ::towlower);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    HMODULE found = nullptr;
    if (Module32FirstW(snap, &me)) {
        do {
            std::wstring name = me.szModule;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name == wantLower) {
                found = me.hModule;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

bool RemoteLoadLibrary(HANDLE process, DWORD pid, const std::wstring& dllPath, std::wstring& err) {
    if (FindRemoteModule(pid, dllPath)) return true;

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteBuf = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf) {
        err = L"VirtualAllocEx 失败: " + FormatWinError(GetLastError());
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remoteBuf, dllPath.c_str(), bytes, &written)) {
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        err = L"WriteProcessMemory 失败: " + FormatWinError(GetLastError());
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto* loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(k32, "LoadLibraryW"));
    if (!loadLib) {
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        err = L"GetProcAddress(LoadLibraryW) 失败";
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLib, remoteBuf, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        err = L"CreateRemoteThread(LoadLibraryW) 失败: " + FormatWinError(GetLastError());
        return false;
    }

    WaitForSingleObject(thread, 15000);
    CloseHandle(thread);
    VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);

    if (!FindRemoteModule(pid, dllPath)) {
        err = L"LoadLibraryW 注入后未找到 CdpEnable64.dll";
        return false;
    }
    return true;
}

bool WaitForCdpList(int preferredPort, int timeoutMs, int* outPort) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(std::max(1000, timeoutMs));
    while (GetTickCount() < deadline) {
        for (int port : CandidatePorts(preferredPort)) {
            if (CdpListReachable(port)) {
                if (outPort) *outPort = port;
                return true;
            }
        }
        Sleep(400);
    }
    if (outPort) *outPort = 0;
    return false;
}

}  // namespace

bool EnableCdpViaProcessInject(HWND edgeTop, int preferredPort, std::wstring& err) {
    err.clear();
    edgeTop = TopLevelTargetWindow(edgeTop);
    if (!edgeTop || !IsWindow(edgeTop)) {
        err = L"CDP 注入目标窗口无效";
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(edgeTop, &pid);
    if (pid == 0) {
        err = L"无法获取 Edge 窗口进程 ID";
        return false;
    }

    const std::wstring dir = ModuleDirectory();
    if (dir.empty()) {
        err = L"无法解析主程序目录";
        return false;
    }
    const std::wstring dllPath = dir + L"CdpEnable64.dll";
    if (!PathExists(dllPath)) {
        err = L"找不到 CdpEnable64.dll: " + dllPath;
        return false;
    }

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
            | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
        FALSE, pid);
    if (!process) {
        const DWORD code = GetLastError();
        err = L"OpenProcess 失败: " + FormatWinError(code);
        if (code == ERROR_ACCESS_DENIED) {
            err += L"（权限不足；若目标更高完整性级别请以管理员运行）";
        }
        return false;
    }

    if (!RemoteProcessHasModule(process, L"msedge.dll")) {
        CloseHandle(process);
        err = L"目标进程未加载 msedge.dll（可能为 GPU/渲染子进程，需注入浏览器主进程）";
        return false;
    }

    WindowModeLogf(L"[窗口模式] CDP 注入 pid=%lu dll=%s",
        static_cast<unsigned long>(pid), dllPath.c_str());

    if (!RemoteLoadLibrary(process, pid, dllPath, err)) {
        CloseHandle(process);
        WindowModeLogf(L"[窗口模式] CDP 注入失败: %s", err.c_str());
        return false;
    }
    CloseHandle(process);

    int readyPort = 0;
    // Edge 大体积 msedge.dll 签名扫描 + UI 线程回调可能超过 15s。
    if (!WaitForCdpList(preferredPort, 30000, &readyPort)) {
        err = L"CDP 注入完成，但 30 秒内未检测到 /json/list（详见 %TEMP%\\CdpEnable64.log；"
              L"签名可能仍不匹配当前 Edge 版本）";
        WindowModeLog(L"[窗口模式] CDP 端口探测超时");
        return false;
    }

    WindowModeLogf(L"[窗口模式] CDP 已就绪 port=%d", readyPort);
    err.clear();
    return true;
}

}  // namespace windowmode
