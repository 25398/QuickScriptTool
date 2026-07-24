#include "cdp_input.h"

#include "window_mode/background_window_input.h"
#include "window_mode/com_apartment.h"
#include "window_mode/virtual_desktop_accessor.h"
#include "window_mode/window_mode_log.h"
#include "window_mode/window_mode_types.h"
#include "window_mode/window_target.h"

#include <UIAutomation.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "UIAutomationCore.lib")

namespace windowmode {
namespace {

using Microsoft::WRL::ComPtr;

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()),
        nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), out.data(), n);
    return out;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

bool HttpGetLocal(int port, const wchar_t* path, std::string& body, std::wstring& err,
    DWORD timeoutMs = 2000) {
    body.clear();
    HINTERNET session = WinHttpOpen(L"QuickScriptTool-CDP/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        err = L"WinHttpOpen 失败";
        return false;
    }
    HINTERNET connect = WinHttpConnect(session, L"127.0.0.1",
        static_cast<INTERNET_PORT>(port), 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        err = L"无法连接 127.0.0.1:" + std::to_wstring(port);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        err = L"WinHttpOpenRequest 失败";
        return false;
    }
    DWORD timeout = timeoutMs > 0 ? timeoutMs : 2000;
    WinHttpSetOption(request, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(request, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(session, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(session, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(session, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        err = L"HTTP 请求失败（端口 " + std::to_wstring(port) + L"）";
        return false;
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) break;
        if (avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read) || read == 0) break;
        body.append(buf.data(), read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return !body.empty();
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

std::vector<int> CandidatePorts(int preferred) {
    std::vector<int> ports;
    auto add = [&](int p) {
        if (p <= 0 || p > 65535) return;
        if (std::find(ports.begin(), ports.end(), p) == ports.end()) ports.push_back(p);
    };
    add(preferred > 0 ? preferred : 9222);

    wchar_t localApp[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) > 0) {
        add(ReadDevToolsActivePort(std::wstring(localApp) + L"\\Microsoft\\Edge\\User Data"));
        add(ReadDevToolsActivePort(std::wstring(localApp) + L"\\Google\\Chrome\\User Data"));
    }
    for (int p = 9222; p <= 9230; ++p) add(p);
    return ports;
}

struct CdpTarget {
    std::string title;
    std::string type;
    std::string wsUrl;
};

std::string ExtractJsonStringValue(const std::string& obj, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) return {};
    pos = obj.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = obj.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos;
    std::string out;
    for (; pos < obj.size(); ++pos) {
        const char c = obj[pos];
        if (c == '\\' && pos + 1 < obj.size()) {
            out.push_back(obj[pos + 1]);
            ++pos;
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

std::vector<CdpTarget> ParseTargetList(const std::string& json) {
    std::vector<CdpTarget> targets;
    size_t i = 0;
    while (i < json.size()) {
        const size_t start = json.find('{', i);
        if (start == std::string::npos) break;
        int depth = 0;
        size_t end = start;
        for (; end < json.size(); ++end) {
            if (json[end] == '{') ++depth;
            else if (json[end] == '}') {
                --depth;
                if (depth == 0) break;
            }
        }
        if (end >= json.size()) break;
        const std::string obj = json.substr(start, end - start + 1);
        CdpTarget t;
        t.type = ExtractJsonStringValue(obj, "type");
        t.title = ExtractJsonStringValue(obj, "title");
        t.wsUrl = ExtractJsonStringValue(obj, "webSocketDebuggerUrl");
        if (!t.wsUrl.empty() && (t.type == "page" || t.type == "webview" || t.type.empty())) {
            targets.push_back(std::move(t));
        }
        i = end + 1;
    }
    return targets;
}

std::wstring WindowTitle(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd) return {};
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    return title;
}

bool TitleLooksMatch(const std::wstring& windowTitle, const std::string& targetTitleUtf8) {
    if (windowTitle.empty() || targetTitleUtf8.empty()) return false;
    const std::wstring target = Utf8ToWide(targetTitleUtf8);
    if (target.empty()) return false;
    // 窗口标题常带 " - Microsoft Edge" 后缀；target 多为页标题。
    if (windowTitle.find(target) != std::wstring::npos) return true;
    if (target.find(windowTitle) != std::wstring::npos) return true;
    // 取窗口标题去掉浏览器后缀再比
    std::wstring stem = windowTitle;
    const auto dash = stem.rfind(L" - ");
    if (dash != std::wstring::npos) stem = stem.substr(0, dash);
    if (!stem.empty() && target.find(stem) != std::wstring::npos) return true;
    if (!stem.empty() && stem.find(target) != std::wstring::npos) return true;
    return false;
}

bool IsInspectOrUtilityTarget(const CdpTarget& t) {
    const std::string& title = t.title;
    auto has = [&](const char* s) {
        return title.find(s) != std::string::npos;
    };
    if (has("edge://inspect") || has("chrome://inspect")) return true;
    if (has("Inspect with") || has("DevTools")) return true;
    if (has("远程调试") || has("检查")) return true;
    if (title == "about:blank") return true;
    return false;
}

const CdpTarget* PickBestTarget(const std::vector<CdpTarget>& targets,
    const std::wstring& windowTitle, const std::wstring& titleHint) {
    if (targets.empty()) return nullptr;

    auto preferMatch = [&](const std::wstring& hint, bool skipUtility) -> const CdpTarget* {
        if (hint.empty()) return nullptr;
        for (const auto& t : targets) {
            if (skipUtility && IsInspectOrUtilityTarget(t)) continue;
            if (TitleLooksMatch(hint, t.title)) return &t;
        }
        return nullptr;
    };

    if (const CdpTarget* hit = preferMatch(titleHint, true)) return hit;
    if (const CdpTarget* hit = preferMatch(windowTitle, true)) return hit;
    if (const CdpTarget* hit = preferMatch(titleHint, false)) return hit;
    if (const CdpTarget* hit = preferMatch(windowTitle, false)) return hit;

    // 单页时直接用
    if (targets.size() == 1) return &targets.front();
    // 否则取第一个非 inspect / 非空标题的 page
    for (const auto& t : targets) {
        if (IsInspectOrUtilityTarget(t)) continue;
        if (!t.title.empty()) return &t;
    }
    for (const auto& t : targets) {
        if (!t.title.empty() && t.title != "about:blank") return &t;
    }
    return &targets.front();
}

struct ParsedWsUrl {
    std::wstring host = L"127.0.0.1";
    INTERNET_PORT port = 9222;
    std::wstring path = L"/";
    bool secure = false;
};

bool ParseWsUrl(const std::string& url, ParsedWsUrl& out, std::wstring& err) {
    // ws://127.0.0.1:9222/devtools/page/xxx
    std::string u = url;
    out = {};
    if (u.rfind("wss://", 0) == 0) {
        out.secure = true;
        u = u.substr(6);
    } else if (u.rfind("ws://", 0) == 0) {
        u = u.substr(5);
    } else {
        err = L"无效的 webSocketDebuggerUrl";
        return false;
    }
    const size_t slash = u.find('/');
    std::string hostPort = slash == std::string::npos ? u : u.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : u.substr(slash);
    const size_t colon = hostPort.find(':');
    std::string host = colon == std::string::npos ? hostPort : hostPort.substr(0, colon);
    int port = out.secure ? 443 : 80;
    if (colon != std::string::npos) {
        port = std::atoi(hostPort.c_str() + colon + 1);
    }
    out.host = Utf8ToWide(host);
    out.port = static_cast<INTERNET_PORT>(port);
    out.path = Utf8ToWide(path);
    return true;
}

std::string MouseButtonName(MouseButtonType button) {
    switch (button) {
    case MouseButtonType::Right: return "right";
    case MouseButtonType::Middle: return "middle";
    case MouseButtonType::X1:
    case MouseButtonType::X2: return "none";
    default: return "left";
    }
}

struct KeyInfo {
    std::string key;
    std::string code;
    std::string text; // empty = non-printable
    int windowsVk = 0;
};

KeyInfo MapVk(UINT vk) {
    KeyInfo info;
    info.windowsVk = static_cast<int>(vk);
    switch (vk) {
    case VK_RETURN: info.key = "Enter"; info.code = "Enter"; info.text = "\r"; break;
    case VK_ESCAPE: info.key = "Escape"; info.code = "Escape"; break;
    case VK_TAB: info.key = "Tab"; info.code = "Tab"; info.text = "\t"; break;
    case VK_BACK: info.key = "Backspace"; info.code = "Backspace"; break;
    case VK_DELETE: info.key = "Delete"; info.code = "Delete"; break;
    case VK_SPACE: info.key = " "; info.code = "Space"; info.text = " "; break;
    case VK_LEFT: info.key = "ArrowLeft"; info.code = "ArrowLeft"; break;
    case VK_UP: info.key = "ArrowUp"; info.code = "ArrowUp"; break;
    case VK_RIGHT: info.key = "ArrowRight"; info.code = "ArrowRight"; break;
    case VK_DOWN: info.key = "ArrowDown"; info.code = "ArrowDown"; break;
    case VK_HOME: info.key = "Home"; info.code = "Home"; break;
    case VK_END: info.key = "End"; info.code = "End"; break;
    case VK_PRIOR: info.key = "PageUp"; info.code = "PageUp"; break;
    case VK_NEXT: info.key = "PageDown"; info.code = "PageDown"; break;
    case VK_SHIFT: case VK_LSHIFT: info.key = "Shift"; info.code = "ShiftLeft"; break;
    case VK_RSHIFT: info.key = "Shift"; info.code = "ShiftRight"; break;
    case VK_CONTROL: case VK_LCONTROL: info.key = "Control"; info.code = "ControlLeft"; break;
    case VK_RCONTROL: info.key = "Control"; info.code = "ControlRight"; break;
    case VK_MENU: case VK_LMENU: info.key = "Alt"; info.code = "AltLeft"; break;
    case VK_RMENU: info.key = "Alt"; info.code = "AltRight"; break;
    default:
        if (vk >= 'A' && vk <= 'Z') {
            info.key.assign(1, static_cast<char>(vk - 'A' + 'a'));
            info.code = std::string("Key") + static_cast<char>(vk);
            info.text = info.key;
        } else if (vk >= '0' && vk <= '9') {
            info.key.assign(1, static_cast<char>(vk));
            info.code = std::string("Digit") + static_cast<char>(vk);
            info.text = info.key;
        }
        break;
    }
    return info;
}

std::wstring ProcessImagePath(HWND hwnd) {
    hwnd = TopLevelTargetWindow(hwnd);
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    return ok ? std::wstring(path) : L"";
}

bool LooksLikeEdgeProcess(const std::wstring& imagePath) {
    if (imagePath.empty()) return false;
    const auto slash = imagePath.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos
        ? imagePath : imagePath.substr(slash + 1);
    return _wcsicmp(name.c_str(), L"msedge.exe") == 0
        || _wcsicmp(name.c_str(), L"msedge_proxy.exe") == 0;
}

bool LooksLikeChromeProcess(const std::wstring& imagePath) {
    if (imagePath.empty()) return false;
    const auto slash = imagePath.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos
        ? imagePath : imagePath.substr(slash + 1);
    return _wcsicmp(name.c_str(), L"chrome.exe") == 0;
}

std::wstring BrowserProcessLeafName(const std::wstring& imagePath) {
    if (LooksLikeChromeProcess(imagePath)) return L"chrome.exe";
    return L"msedge.exe";
}

void CloseChromiumTopWindows(const std::wstring& leafName) {
    struct Ctx {
        const wchar_t* leaf = nullptr;
    } ctx{leafName.c_str()};

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        const auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindow(hwnd)) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        wchar_t cls[256]{};
        GetClassNameW(hwnd, cls, 256);
        if (_wcsnicmp(cls, L"Chrome_WidgetWin_", 16) != 0) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return TRUE;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return TRUE;
        wchar_t path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
        CloseHandle(process);
        if (!ok) return TRUE;
        const auto slash = std::wstring(path).find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos
            ? std::wstring(path) : std::wstring(path).substr(slash + 1);
        if (_wcsicmp(name.c_str(), c->leaf) != 0) return TRUE;
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

bool AnyBrowserProcessRunning(const std::wstring& leafName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, leafName.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

void ForceKillBrowserProcesses(const std::wstring& leafName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, leafName.c_str()) != 0) continue;
            HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (process) {
                TerminateProcess(process, 0);
                CloseHandle(process);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

bool WaitForAnyCdpPort(int preferredPort, int timeoutMs, int* outPort);

bool RelaunchChromiumWithRemoteDebuggingImpl(const std::wstring& browserExe, int port,
    std::wstring& err) {
    if (browserExe.empty()
        || !(LooksLikeEdgeProcess(browserExe) || LooksLikeChromeProcess(browserExe))) {
        err = L"不是 Edge/Chrome，无法自动兼容重启";
        return false;
    }
    if (port <= 0) port = 9222;
    const std::wstring leaf = BrowserProcessLeafName(browserExe);

    WindowModeLogf(L"[窗口模式] 自动兼容重启浏览器（恢复标签 + remote-debugging-port=%d），无需手动调试",
        port);

    CloseChromiumTopWindows(leaf);
    const DWORD waitDeadline = GetTickCount() + 15000;
    while (GetTickCount() < waitDeadline && AnyBrowserProcessRunning(leaf)) {
        Sleep(300);
    }
    if (AnyBrowserProcessRunning(leaf)) {
        WindowModeLog(L"[窗口模式] 浏览器未完全退出，强制结束进程…");
        ForceKillBrowserProcesses(leaf);
        Sleep(800);
    }
    // 等用户数据目录锁释放
    Sleep(1200);

    std::wstring args = EnsureRemoteDebuggingLaunchArgs(L"--restore-last-session", port);
    std::wstring cmd = L"\"";
    cmd += browserExe;
    cmd += L"\" ";
    cmd += args;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
            0, nullptr, nullptr, &si, &pi)) {
        err = L"兼容模式启动浏览器失败";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    WindowModeLogf(L"[窗口模式] 已启动: %s", cmd.c_str());

    int readyPort = 0;
    if (!WaitForAnyCdpPort(port, 45000, &readyPort)) {
        err = L"浏览器已重启，但远程调试端口仍未就绪";
        return false;
    }
    WindowModeLogf(L"[窗口模式] 兼容重启完成，CDP 端口=%d", readyPort);
    err.clear();
    return true;
}

bool NameSuggestsRemoteDebugToggle(const std::wstring& name) {
    if (name.empty()) return false;
    if (name.find(L"远程调试") != std::wstring::npos) return true;
    if (name.find(L"Remote debugging") != std::wstring::npos) return true;
    if (name.find(L"remote debugging") != std::wstring::npos) return true;
    if (name.find(L"Allow remote debugging") != std::wstring::npos) return true;
    if (name.find(L"允许对此浏览器实例") != std::wstring::npos) return true;
    if (name.find(L"允许远程调试") != std::wstring::npos) return true;
    return false;
}

bool TryActivateRemoteDebugControl(IUIAutomationElement* el) {
    if (!el) return false;
    ComPtr<IUIAutomationTogglePattern> toggle;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) && toggle) {
        ToggleState state = ToggleState_Off;
        toggle->get_CurrentToggleState(&state);
        if (state == ToggleState_On) return true;
        if (SUCCEEDED(toggle->Toggle())) return true;
    }
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) && invoke) {
        if (SUCCEEDED(invoke->Invoke())) return true;
    }
    ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&legacy)))
        && legacy) {
        if (SUCCEEDED(legacy->DoDefaultAction())) return true;
    }
    return false;
}

bool TryToggleRemoteDebuggingOnHwnd(HWND edgeTop) {
    if (!edgeTop || !IsWindow(edgeTop)) return false;
    EnsureThreadComApartment();

    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&uia))) || !uia) {
        return false;
    }

    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(edgeTop, &root)) || !root) return false;

    ComPtr<IUIAutomationCondition> trueCond;
    if (FAILED(uia->CreateTrueCondition(&trueCond)) || !trueCond) return false;

    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(root->FindAll(TreeScope_Descendants, trueCond.Get(), &found)) || !found) {
        return false;
    }

    int count = 0;
    found->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(found->GetElement(i, &el)) || !el) continue;
        BSTR nameBstr = nullptr;
        el->get_CurrentName(&nameBstr);
        std::wstring name = nameBstr ? nameBstr : L"";
        if (nameBstr) SysFreeString(nameBstr);
        if (!NameSuggestsRemoteDebugToggle(name)) continue;
        if (TryActivateRemoteDebugControl(el.Get())) {
            WindowModeLogf(L"[窗口模式] 已启用远程调试控件: %s", name.c_str());
            return true;
        }
    }
    return false;
}

bool EnumEdgeTopWindows(std::vector<HWND>& out) {
    out.clear();
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* list = reinterpret_cast<std::vector<HWND>*>(lp);
        if (!IsWindow(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        wchar_t cls[256]{};
        GetClassNameW(hwnd, cls, 256);
        if (_wcsnicmp(cls, L"Chrome_WidgetWin_", 16) != 0) return TRUE;
        const std::wstring path = ProcessImagePath(hwnd);
        if (!LooksLikeEdgeProcess(path) && !LooksLikeChromeProcess(path)) return TRUE;
        list->push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return !out.empty();
}

bool TryToggleRemoteDebuggingCheckbox(HWND preferredTop) {
    std::vector<HWND> windows;
    EnumEdgeTopWindows(windows);
    if (preferredTop && IsWindow(preferredTop)) {
        windows.insert(windows.begin(), preferredTop);
    }
    // 去重
    std::vector<HWND> unique;
    for (HWND h : windows) {
        if (std::find(unique.begin(), unique.end(), h) == unique.end()) unique.push_back(h);
    }
    for (HWND h : unique) {
        if (TryToggleRemoteDebuggingOnHwnd(h)) return true;
    }
    return false;
}

bool OpenChromiumInspectRemoteDebuggingPage(HWND edgeTop, const std::wstring& browserExe) {
    const bool chrome = LooksLikeChromeProcess(browserExe);
    const wchar_t* kUrl = chrome
        ? L"chrome://inspect/#remote-debugging"
        : L"edge://inspect/#remote-debugging";
    if (!browserExe.empty()) {
        std::wstring params = L"\"";
        params += kUrl;
        params += L"\"";
        const HINSTANCE hi = ShellExecuteW(nullptr, L"open", browserExe.c_str(), params.c_str(),
            nullptr, SW_SHOWNOACTIVATE);
        if (reinterpret_cast<INT_PTR>(hi) > 32) {
            WindowModeLogf(L"[窗口模式] 已在现有浏览器新开标签: %s（游戏页保留，不重启）", kUrl);
            return true;
        }
    }
    if (!chrome) {
        const HINSTANCE hi2 = ShellExecuteW(nullptr, L"open",
            L"microsoft-edge:edge://inspect/#remote-debugging", nullptr, nullptr, SW_SHOWNOACTIVATE);
        if (reinterpret_cast<INT_PTR>(hi2) > 32) {
            WindowModeLog(L"[窗口模式] 已通过协议打开远程调试页（游戏页保留）");
            return true;
        }
    }
    (void)edgeTop;
    return false;
}

bool WaitForAnyCdpPort(int preferredPort, int timeoutMs, int* outPort) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(std::max(1000, timeoutMs));
    while (GetTickCount() < deadline) {
        const auto ports = CandidatePorts(preferredPort);
        for (int port : ports) {
            std::string body;
            std::wstring localErr;
            if (HttpGetLocal(port, L"/json/list", body, localErr)
                || HttpGetLocal(port, L"/json", body, localErr)) {
                if (outPort) *outPort = port;
                return true;
            }
        }
        Sleep(400);
    }
    if (outPort) *outPort = 0;
    return false;
}

/// 已打开的 Edge/Chrome：新开 inspect 标签并勾选远程调试。不重启进程，游戏页数据保留。
bool TryEnableChromiumRemoteDebuggingInPlace(HWND top, int preferredPort, std::wstring& err) {
    top = TopLevelTargetWindow(top);
    const std::wstring exe = ProcessImagePath(top);
    if (!LooksLikeEdgeProcess(exe) && !LooksLikeChromeProcess(exe)) {
        err = L"目标不是 Edge/Chrome";
        return false;
    }

    if (IsIconic(top)) {
        // 异桌面 Show 会切到宏桌面：先 Cloak，Show 后钉回原桌面。
        auto& vda = VirtualDesktopAccessor::Instance();
        const int viewBefore = vda.GetCurrentDesktopNumber();
        BOOL cloak = TRUE;
        DwmSetWindowAttribute(top, DWMWA_CLOAK, &cloak, sizeof(cloak));
        ShowWindow(top, SW_SHOWNOACTIVATE);
        vda.HoldView(viewBefore, 250);
        cloak = FALSE;
        DwmSetWindowAttribute(top, DWMWA_CLOAK, &cloak, sizeof(cloak));
    }

    if (!OpenChromiumInspectRemoteDebuggingPage(top, exe)) {
        err = L"无法打开远程调试设置页";
        return false;
    }

    bool toggled = false;
    for (int i = 0; i < 30; ++i) {
        Sleep(400);
        if (TryToggleRemoteDebuggingCheckbox(top)) {
            toggled = true;
            break;
        }
    }
    if (!toggled) {
        WindowModeLog(L"[窗口模式] 未自动勾选到远程调试开关，继续检测 DevToolsActivePort…");
    }

    int port = 0;
    if (!WaitForAnyCdpPort(preferredPort, 15000, &port)) {
        err = L"未能在现有浏览器实例启用远程调试（未重启，游戏页仍在）。"
              L"可在浏览器新标签打开 edge://inspect → 远程调试，勾选允许；"
              L"或后续使用配套扩展方案（无需每次勾选）。";
        return false;
    }
    WindowModeLogf(L"[窗口模式] 现有浏览器远程调试已可用 port=%d（未重启）", port);
    err.clear();
    return true;
}

bool TryHttpListOnPorts(int preferredPort, std::string& listBody, int& usedPort, std::wstring& err) {
    listBody.clear();
    usedPort = 0;
    // 短超时：端口未开时不要拖住脚本线程（否则热键中止要等扫完 9222~9230）。
    constexpr DWORD kProbeTimeoutMs = 250;
    const auto ports = CandidatePorts(preferredPort);
    std::wstring lastErr;
    for (int port : ports) {
        std::wstring localErr;
        if (HttpGetLocal(port, L"/json/list", listBody, localErr, kProbeTimeoutMs)
            || HttpGetLocal(port, L"/json", listBody, localErr, kProbeTimeoutMs)) {
            usedPort = port;
            err.clear();
            return true;
        }
        lastErr = localErr;
    }
    err = L"NEED_INPLACE";
    if (!lastErr.empty()) {
        err += L":";
        err += lastErr;
    }
    return false;
}

}  // namespace

bool IsChromiumBrowserExecutable(const std::wstring& imagePath) {
    return LooksLikeEdgeProcess(imagePath) || LooksLikeChromeProcess(imagePath);
}

std::wstring QueryHwndProcessImagePath(HWND hwnd) {
    return ProcessImagePath(hwnd);
}

bool EnableChromiumRemoteDebuggingWithoutRestart(HWND top, int preferredPort, std::wstring& err) {
    (void)top;
    (void)preferredPort;
    // 注入 / inspect 勾选已停用；未开调试端口时改走配套扩展桥。
    err = L"请安装配套扩展（extension\\edge），无需开启 remote-debugging-port";
    WindowModeLog(L"[窗口模式] 已停用 CDP 注入/inspect；请使用配套扩展桥");
    return false;
}

bool RelaunchChromiumWithRemoteDebugging(const std::wstring& browserExe, int port,
    std::wstring& err) {
    // 会丢页面内存状态；默认产品路径不要调用。
    return RelaunchChromiumWithRemoteDebuggingImpl(browserExe, port, err);
}

CdpInputSession::~CdpInputSession() {
    Disconnect();
}

void CdpInputSession::Disconnect() {
    connected_ = false;
    auto* ws = static_cast<HINTERNET>(hWebSocket_);
    auto* req = static_cast<HINTERNET>(hRequest_);
    auto* conn = static_cast<HINTERNET>(hConnect_);
    auto* sess = static_cast<HINTERNET>(hSession_);
    if (ws) {
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(ws);
        hWebSocket_ = nullptr;
    }
    if (req) {
        WinHttpCloseHandle(req);
        hRequest_ = nullptr;
    }
    if (conn) {
        WinHttpCloseHandle(conn);
        hConnect_ = nullptr;
    }
    if (sess) {
        WinHttpCloseHandle(sess);
        hSession_ = nullptr;
    }
}

bool CdpInputSession::UpgradeWebSocket(const std::wstring& wsUrlWide, std::wstring& err) {
    Disconnect();
    ParsedWsUrl parsed;
    if (!ParseWsUrl(WideToUtf8(wsUrlWide), parsed, err)) return false;

    HINTERNET session = WinHttpOpen(L"QuickScriptTool-CDP/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        err = L"WinHttpOpen 失败";
        return false;
    }
    hSession_ = session;
    HINTERNET connect = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
    if (!connect) {
        err = L"WinHttpConnect 失败";
        Disconnect();
        return false;
    }
    hConnect_ = connect;
    DWORD flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", parsed.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        err = L"WinHttpOpenRequest(WS) 失败";
        Disconnect();
        return false;
    }
    hRequest_ = request;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        err = L"无法升级到 WebSocket";
        Disconnect();
        return false;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request, nullptr)) {
        err = L"WebSocket 握手失败";
        Disconnect();
        return false;
    }
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (!ws) {
        err = L"WinHttpWebSocketCompleteUpgrade 失败";
        Disconnect();
        return false;
    }
    hWebSocket_ = ws;
    // Request handle ownership transfers; close the HTTP handle.
    WinHttpCloseHandle(request);
    hRequest_ = nullptr;
    connected_ = true;
    return true;
}

bool CdpInputSession::SendRaw(const std::string& utf8, std::wstring& err) {
    auto* ws = static_cast<HINTERNET>(hWebSocket_);
    if (!ws) {
        err = L"CDP 未连接";
        return false;
    }
    const DWORD hr = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (PVOID)utf8.data(), static_cast<DWORD>(utf8.size()));
    if (hr != ERROR_SUCCESS) {
        err = L"CDP WebSocket 发送失败";
        return false;
    }
    return true;
}

bool CdpInputSession::RecvUntilId(int id, std::wstring& err) {
    auto* ws = static_cast<HINTERNET>(hWebSocket_);
    if (!ws) {
        err = L"CDP 未连接";
        return false;
    }
    const std::string idNeedle = "\"id\":" + std::to_string(id);
    std::string acc;
    const DWORD deadline = GetTickCount() + 5000;
    while (GetTickCount() < deadline) {
        char buf[8192];
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD hr = WinHttpWebSocketReceive(ws, buf, sizeof(buf), &read, &type);
        if (hr != ERROR_SUCCESS) {
            err = L"CDP WebSocket 接收失败";
            return false;
        }
        if (read == 0) continue;
        acc.append(buf, read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
            || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            if (acc.find(idNeedle) != std::string::npos) {
                if (acc.find("\"error\"") != std::string::npos) {
                    err = L"CDP 命令返回 error: " + Utf8ToWide(acc.substr(0, std::min<size_t>(acc.size(), 200)));
                    return false;
                }
                return true;
            }
            acc.clear();
        }
    }
    err = L"CDP 命令超时";
    return false;
}

bool CdpInputSession::Call(const std::string& method, const std::string& paramsJson,
    std::wstring& err) {
    const int id = nextId_++;
    std::string msg = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + method + "\"";
    if (!paramsJson.empty()) {
        msg += ",\"params\":";
        msg += paramsJson;
    }
    msg += "}";
    if (!SendRaw(msg, err)) return false;
    return RecvUntilId(id, err);
}

bool CdpInputSession::ConnectForWindow(HWND top, int preferredPort,
    const std::wstring& titleHint, std::wstring& err) {
    Disconnect();
    top = TopLevelTargetWindow(top);
    const std::wstring windowTitleBefore = top && IsWindow(top) ? WindowTitle(top) : L"";
    const std::wstring matchHint = !titleHint.empty() ? titleHint : windowTitleBefore;

    std::string listBody;
    int usedPort = 0;
    if (!TryHttpListOnPorts(preferredPort, listBody, usedPort, err)) {
        // 端口未开：返回 NEED_INPLACE，由上层在不重启前提下启用调试。
        if (err.find(L"NEED_INPLACE") == std::wstring::npos) {
            err = L"NEED_INPLACE";
        }
        return false;
    }

    const auto targets = ParseTargetList(listBody);
    const CdpTarget* best = PickBestTarget(targets, windowTitleBefore, matchHint);
    if (!best) {
        err = L"调试端口 " + std::to_wstring(usedPort)
            + L" 上没有可用的 page target";
        return false;
    }

    const std::wstring wsUrl = Utf8ToWide(best->wsUrl);
    if (!UpgradeWebSocket(wsUrl, err)) return false;

    std::wstring callErr;
    if (!Call("Emulation.setFocusEmulationEnabled", "{\"enabled\":true}", callErr)) {
        WindowModeLogf(L"[窗口模式] CDP setFocusEmulationEnabled 失败（继续）: %s", callErr.c_str());
    }
    Call("Page.bringToFront", "{}", callErr);

    WindowModeLogf(L"[窗口模式] CDP 已连接 port=%d title=%s",
        usedPort, Utf8ToWide(best->title).c_str());
    return true;
}

bool CdpInputSession::KeyEvent(UINT vk, bool down, std::wstring& err) {
    if (!connected_) {
        err = L"CDP 未连接";
        return false;
    }
    const KeyInfo info = MapVk(vk);
    const char* type = "keyUp";
    if (down) type = info.text.empty() ? "rawKeyDown" : "keyDown";

    std::string params = "{";
    params += "\"type\":\"";
    params += type;
    params += "\",\"windowsVirtualKeyCode\":";
    params += std::to_string(info.windowsVk);
    params += ",\"nativeVirtualKeyCode\":";
    params += std::to_string(info.windowsVk);
    if (!info.key.empty()) {
        params += ",\"key\":\"";
        params += JsonEscape(info.key);
        params += "\"";
    }
    if (!info.code.empty()) {
        params += ",\"code\":\"";
        params += JsonEscape(info.code);
        params += "\"";
    }
    if (down && !info.text.empty()) {
        params += ",\"text\":\"";
        params += JsonEscape(info.text);
        params += "\",\"unmodifiedText\":\"";
        params += JsonEscape(info.text);
        params += "\"";
    }
    params += "}";
    return Call("Input.dispatchKeyEvent", params, err);
}

bool CdpInputSession::MouseMove(int cx, int cy, std::wstring& err) {
    lastCx_ = cx;
    lastCy_ = cy;
    std::string params = "{\"type\":\"mouseMoved\",\"x\":";
    params += std::to_string(cx);
    params += ",\"y\":";
    params += std::to_string(cy);
    params += ",\"button\":\"none\"}";
    return Call("Input.dispatchMouseEvent", params, err);
}

bool CdpInputSession::MouseButton(int cx, int cy, MouseButtonType button, bool down,
    std::wstring& err) {
    lastCx_ = cx;
    lastCy_ = cy;
    const char* type = down ? "mousePressed" : "mouseReleased";
    std::string params = "{\"type\":\"";
    params += type;
    params += "\",\"x\":";
    params += std::to_string(cx);
    params += ",\"y\":";
    params += std::to_string(cy);
    params += ",\"button\":\"";
    params += MouseButtonName(button);
    params += "\",\"clickCount\":1}";
    return Call("Input.dispatchMouseEvent", params, err);
}

bool CdpInputSession::MouseClick(int cx, int cy, MouseButtonType button, std::wstring& err) {
    if (!MouseButton(cx, cy, button, true, err)) return false;
    return MouseButton(cx, cy, button, false, err);
}

bool CdpInputSession::Scroll(int cx, int cy, int steps, bool vertical, bool positive,
    std::wstring& err) {
    lastCx_ = cx;
    lastCy_ = cy;
    const int delta = (positive ? 1 : -1) * std::max(1, steps) * 100;
    std::string params = "{\"type\":\"mouseWheel\",\"x\":";
    params += std::to_string(cx);
    params += ",\"y\":";
    params += std::to_string(cy);
    if (vertical) {
        params += ",\"deltaX\":0,\"deltaY\":";
        params += std::to_string(delta);
    } else {
        params += ",\"deltaX\":";
        params += std::to_string(delta);
        params += ",\"deltaY\":0";
    }
    params += "}";
    return Call("Input.dispatchMouseEvent", params, err);
}

bool CdpInputSession::InsertText(const std::wstring& text, std::wstring& err) {
    if (text.empty()) return true;
    const std::string utf8 = WideToUtf8(text);
    std::string params = "{\"text\":\"";
    params += JsonEscape(utf8);
    params += "\"}";
    return Call("Input.insertText", params, err);
}

}  // namespace windowmode
