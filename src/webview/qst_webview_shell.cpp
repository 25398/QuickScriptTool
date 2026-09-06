// ──────────────────────────────────────────────────────────────────
// qst_webview_shell.cpp — WebView2 UI shell (Fixed Runtime portable)
// Icons/tray reuse taskbar_window.h + product resources; engine via bridge.
// ──────────────────────────────────────────────────────────────────

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wrl.h>

#include "agent_ui_notify.h"
#include "config.h"
#include "desktop_tools/desktop_tools.h"
#include "ocr_engine.h"
#include "process_utils.h"
#include "taskbar_window.h"
#include "tray_menu.h"
#include "utils.h"
#include "engine/qst_engine.h"
#include "webview/webview_bridge_backend.h"
#include "window_mode/window_mode_json.h"
#include "window_mode/window_mode_preview.h"
#include "input/hid_interception.h"
#include "input/input_emergency_teardown.h"
#include "input/virtual_hid.h"

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <commdlg.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <aclapi.h>
#include <sddl.h>
#include <delayimp.h>
#include <mutex>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "comdlg32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

#ifndef INTERNET_MAX_URL_LENGTH
#define INTERNET_MAX_URL_LENGTH 2084
#endif

#ifndef QST_WEBVIEW_USE_FIXED
#define QST_WEBVIEW_USE_FIXED 1
#endif
#ifndef QST_WEBVIEW_ALLOW_EVERGREEN
#define QST_WEBVIEW_ALLOW_EVERGREEN 0
#endif

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif
#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif

HINSTANCE g_instance = nullptr;

namespace {

constexpr wchar_t kWndClass[] = L"QstWebViewShellWindow";
constexpr wchar_t kDebugWndClass[] = L"KeyMouseDebugWebWindow";
constexpr wchar_t kAgentWndClass[] = L"QstAgentWebWindow";
// 用户指定客户区：主页 1552×960（黄金分割）；鼠标宏编辑器 1800×1230；录制优化 1640×1140（1:1，不按 DPI 放大）
// 前端组件倍率见 ui/index.html --qst-u / --qst-opt-u，勿在此做 DPI 放大。
constexpr int kHomeClientW = 1552;
constexpr int kHomeClientH = 960;
constexpr int kProHomeClientW = 1552;
constexpr int kProHomeClientH = 960;
int g_homeClientW = kHomeClientW;
int g_homeClientH = kHomeClientH;
constexpr int kEditorClientW = 1800;
constexpr int kEditorClientH = 1230;
constexpr int kOptClientW = 1640;
constexpr int kOptClientH = 1140;
constexpr int kDebugClientW = 480;
constexpr int kDebugClientH = 300;
// 对齐 index.html .dlg.home-size（约主界面 0.85）：1170×820
constexpr int kAgentClientW = 1170;
constexpr int kAgentClientH = 820;
constexpr wchar_t kFixedDirName[] = L"WebView2Fixed";
constexpr wchar_t kUserDataDirName[] = L"WebView2UserData";
// 空参数走 GPU。曾强制 SwiftShader 软件光栅：1800×1230 编辑器叠在游戏上会把
// UI 线程拖死，表现为「编辑时鼠标一顿一顿」。空页已由 cloak + contentReady 兜底。
constexpr wchar_t kWebViewBrowserArgs[] = L"";
constexpr UINT kTrayId = 1;
constexpr UINT WM_BRIDGE_POST_JS = WM_APP + 91;
constexpr UINT WM_APP_BROWSE_PATH = WM_APP + 92; // 延后弹出文件框，避开 WebMessageReceived 嵌套
constexpr UINT WM_APPLY_UI_MODE = WM_APP + 93; // wParam: 0=home 1=editor 2=opt
constexpr UINT WM_DEBUG_POST_JS = WM_APP + 94;
constexpr UINT WM_AGENT_POST_JS = WM_APP + 95;
constexpr UINT WM_APP_CAPTURE_PREVIEW = WM_APP + 96; // 异步抓预览首帧（lParam=PreviewCaptureReq*）
constexpr UINT WM_APP_EVERGREEN_FALLBACK = WM_APP + 97; // 固定运行时失败 -> 回退系统 WebView2
constexpr UINT WM_APP_LAUNCH_WEBVIEW = WM_APP + 98;     // 后台 ACL 完成后启动 WebView

enum class UiMode { Home, Editor, Optimize };

constexpr UINT_PTR kStatusTimerId = 9101;
constexpr UINT_PTR kPrewarmTimerId = 9102;
// 注意：勿与 kWebHotkeyHoldTimerId(9103) 冲突
constexpr UINT_PTR kMainRevealTimerId = 9105;
constexpr UINT_PTR kMainRevealSettleTimerId = 9106; // contentReady 后再等合成，避免揭开空 sky
constexpr UINT_PTR kEvergreenFallbackTimerId = 9107; // 固定运行时迟迟没画出页面 -> 回退系统 WebView2
constexpr UINT_PTR kBootVisibleTimerId = 9108; // 首屏未就绪时先露出 sky 窗，避免「点了没反应」
constexpr UINT_PTR kAclWaitTimerId = 9109;      // 后台 ACL 过久则不等，直接起 WebView

HWND g_hwnd = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
bool g_shellCloaked = false;
ComPtr<ICoreWebView2Environment> g_webviewEnv;
HWND g_debugHwnd = nullptr;
ComPtr<ICoreWebView2Controller> g_debugController;
ComPtr<ICoreWebView2> g_debugWebview;
bool g_debugPinned = true;
bool g_debugCreating = false;
bool g_debugReady = false;
bool g_debugContentReady = false; // 真 chrome 已绘，才允许 Show，避免黑屏
std::vector<std::string> g_debugPending;

HWND g_agentHwnd = nullptr;
ComPtr<ICoreWebView2Controller> g_agentController;
ComPtr<ICoreWebView2> g_agentWebview;
bool g_agentCreating = false;
bool g_agentReady = false;
bool g_agentContentReady = false; // 骨架已绘出（contentReady），才允许 Show，避免 navy 黑屏
bool g_agentNavFallbackTried = false;
std::vector<std::string> g_agentPending;
std::string g_agentOpenPending; // agentWindow.open JSON 等就绪后投递
bool g_agentPinned = false;
bool g_agentWantShow = false;
bool g_debugWantShow = false;
bool g_secondaryPrewarmScheduled = false;
bool g_handlingAgentMsg = false;
HBRUSH g_agentBgBrush = nullptr;

// 助手/调试 WebView 未就绪时暂存消息；卡住创建时防止无限堆积
constexpr size_t kMaxPendingWebMessages = 64;

void CapPendingWebMessages(std::vector<std::string>& q) {
    if (q.size() <= kMaxPendingWebMessages) return;
    q.erase(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(q.size() - kMaxPendingWebMessages));
}

bool g_mainWantShow = true;
bool g_mainContentReady = false; // Web 首屏 chrome 已绘，可显示 WebView
bool g_mainWebVisible = false;   // 窗体可见前 WebView 已绘真 DOM；揭窗时一并 IsVisible
bool g_mainNavSucceeded = false; // 主 UI 首次导航是否成功（NavigationCompleted ok）
bool g_mainNavDoneOnce = false;  // 首次导航是否已回调（避免把后续重载当失败）
int g_webviewAttempt = 0;        // 0=固定运行时 1=系统 WebView2 回退
bool g_evergreenFallbackPending = false;
bool g_revealErrorShown = false;
bool g_webviewLaunchStarted = false; // ACL 异步完成后只 Launch 一次
bool g_bootPlaceholderShown = false;
int g_mainShowCmd = SW_SHOW;

void RegisterSyncHostObject(); // 定义在 EscapeJsonUtf8 之后

std::wstring g_browserFolder;
std::wstring g_userDataFolder;
HANDLE g_mutex = nullptr;

UiMode g_mode = UiMode::Home;
bool g_trayActive = false;
bool g_trayRunning = false;
UINT g_wmTaskbarCreated = 0;
bool g_applyingModeResize = false;
// Web 动作按键捕获：LL 钩子把 LWin 等键回传给 #ov-action-key（不弹原生对话框）
HHOOK g_webActionKeyLl = nullptr;
bool g_webActionKeyLlActive = false;
// Web 脚本/全局热键捕获：键盘+鼠标 LL，支持长按判定（不弹原生 HotkeyCapture）
HHOOK g_webHotkeyKbLl = nullptr;
HHOOK g_webHotkeyMouseLl = nullptr;
bool g_webHotkeyLlActive = false;
DWORD g_webHotkeyHoldMs = 200;
bool g_webHotkeyPending = false;
UINT g_webHotkeyPendingVk = 0;
UINT g_webHotkeyPendingMods = 0;
DWORD g_webHotkeyPendingTick = 0;
constexpr UINT_PTR kWebHotkeyHoldTimerId = 9103;
constexpr UINT_PTR kWebCaptureWatchdogTimerId = 9104;
DWORD g_webHotkeyKbLastEventTick = 0;
DWORD g_webActionKeyLastEventTick = 0;
windowmode::WindowModePreview g_wmPreview;
HWND g_wmPreviewTarget = nullptr;
std::atomic<bool> g_webPreviewVisible{false};
std::atomic<bool> g_previewCapturing{false};

struct PreviewCaptureReq {
    HWND target = nullptr;
    std::wstring label;
};

struct CotaskStr {
    LPWSTR p = nullptr;
    ~CotaskStr() { if (p) CoTaskMemFree(p); }
    LPWSTR* put() { return &p; }
    const wchar_t* get() const { return p ? p : L""; }
};

/// 虚拟主机映射了整个 AppDir：拦截 settings/密钥等敏感文件，防页面 fetch 读出 apiKey。
void InstallQstLocalAccessGuard(ICoreWebView2* webview) {
    if (!webview || !g_webviewEnv) return;
    webview->AddWebResourceRequestedFilter(
        L"https://qst.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    EventRegistrationToken token{};
    webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (!args || !g_webviewEnv) return S_OK;
                ComPtr<ICoreWebView2WebResourceRequest> req;
                if (FAILED(args->get_Request(&req)) || !req) return S_OK;
                CotaskStr uri;
                if (FAILED(req->get_Uri(uri.put())) || !uri.p) return S_OK;
                std::wstring u = uri.p;
                for (auto& ch : u) {
                    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
                const bool deny = (u.find(L"app_settings.json") != std::wstring::npos)
                    || (u.find(L"/driver/") != std::wstring::npos)
                    || (u.find(L"ext_bridge.json") != std::wstring::npos)
                    || (u.find(L"/agent_conversations/") != std::wstring::npos)
                    || (u.size() >= 4 && u.compare(u.size() - 4, 4, L".exe") == 0)
                    || (u.size() >= 4 && u.compare(u.size() - 4, 4, L".dll") == 0)
                    || (u.size() >= 4 && u.compare(u.size() - 4, 4, L".pdb") == 0)
                    || (u.size() >= 4 && u.compare(u.size() - 4, 4, L".key") == 0)
                    || (u.size() >= 4 && u.compare(u.size() - 4, 4, L".pem") == 0)
                    || (u.size() >= 5 && u.compare(u.size() - 5, 5, L".pfx") == 0)
                    || (u.size() >= 5 && u.compare(u.size() - 5, 5, L".p12") == 0)
                    || (u.size() >= 5 && u.compare(u.size() - 5, 5, L".jks") == 0);
                if (!deny) return S_OK;
                ComPtr<ICoreWebView2WebResourceResponse> resp;
                if (SUCCEEDED(g_webviewEnv->CreateWebResourceResponse(
                        nullptr, 403, L"Forbidden", L"Content-Type: text/plain", &resp))
                    && resp) {
                    args->put_Response(resp.Get());
                }
                return S_OK;
            }).Get(),
        &token);
}

DWORD WindowStyle() {
    // No WS_THICKFRAME: size locked like GDI EngineHost (config.h kHome*/kEditor*).
    return WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN;
}

DWORD WindowExStyle() { return WS_EX_APPWINDOW; }

UINT WindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForWindow"));
    const UINT dpi = (fn && hwnd) ? fn(hwnd) : 96;
    return dpi ? dpi : 96;
}

void ClientToOuterSize(int clientW, int clientH, int& outW, int& outH) {
    // WS_POPUP has no non-client frame for our chrome; outer == client.
    outW = (std::max)(1, clientW);
    outH = (std::max)(1, clientH);
}

// 与产品窗体同步：HWND 客户区 = 设计像素（主页 1380×960 / 编辑器 1800×1230）。
// 不按 DPI 放大窗口；WebView RasterizationScale=1 → CSS 与客户区一致。
void ApplyWebViewRasterScale1() {
    if (!g_controller) return;
    g_controller->put_ZoomFactor(1.0);
    ComPtr<ICoreWebView2Controller3> c3;
    if (SUCCEEDED(g_controller.As(&c3)) && c3) {
        c3->put_ShouldDetectMonitorScaleChanges(FALSE);
        c3->put_RasterizationScale(1.0);
    }
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path().wstring();
}

std::wstring UiIndexPath() {
    const std::wstring beside = ExeDir() + L"\\ui\\index.html";
    if (std::filesystem::exists(beside)) return beside;
    std::filesystem::path cur = std::filesystem::current_path();
    for (int i = 0; i < 6; ++i) {
        auto cand = cur / "ui" / "index.html";
        if (std::filesystem::exists(cand)) return cand.wstring();
        if (!cur.has_parent_path()) break;
        cur = cur.parent_path();
    }
    return beside;
}

std::wstring UiAgentPath() {
    const std::wstring beside = ExeDir() + L"\\ui\\agent.html";
    if (std::filesystem::exists(beside)) return beside;
    const std::wstring index = UiIndexPath();
    const auto slash = index.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        return index.substr(0, slash + 1) + L"agent.html";
    return beside;
}

std::wstring UiDebugPath() {
    const std::wstring beside = ExeDir() + L"\\ui\\debug.html";
    if (std::filesystem::exists(beside)) return beside;
    const std::wstring index = UiIndexPath();
    const auto slash = index.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        return index.substr(0, slash + 1) + L"debug.html";
    return beside;
}

std::wstring PathToFileUrl(const std::wstring& path) {
    std::wstring full = std::filesystem::absolute(path).wstring();
    // UrlCreateFromPathW 会对空格/中文等做百分号编码，避免 file:/// 导航失败
    DWORD cch = INTERNET_MAX_URL_LENGTH;
    std::wstring url(cch, L'\0');
    const HRESULT hr = UrlCreateFromPathW(full.c_str(), url.data(), &cch, 0);
    if (SUCCEEDED(hr) && cch > 0) {
        url.resize(cch);
        while (!url.empty() && url.back() == L'\0') url.pop_back();
        if (!url.empty()) return url;
    }
    // 回退：UTF-8 百分号编码（保留 / : 与未保留 ASCII）
    for (auto& ch : full) {
        if (ch == L'\\') ch = L'/';
    }
    std::wstring enc;
    enc.reserve(full.size() * 3 + 8);
    for (wchar_t ch : full) {
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'/' || ch == L':' ||
            ch == L'-' || ch == L'_' || ch == L'.' || ch == L'~') {
            enc.push_back(ch);
            continue;
        }
        char utf8[8]{};
        const int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, static_cast<int>(sizeof(utf8)),
            nullptr, nullptr);
        for (int i = 0; i < n; ++i) {
            wchar_t buf[8]{};
            swprintf_s(buf, L"%%%02X", static_cast<unsigned char>(utf8[i]));
            enc += buf;
        }
    }
    return L"file:///" + enc;
}

constexpr wchar_t kAclMarkerName[] = L".qst_acl_ok";
constexpr wchar_t kBootLogName[] = L"webview_boot.log";

std::string NarrowUtf8(const std::wstring& w); // 定义在下方

std::wstring BootLogPath() {
    return (std::filesystem::path(ExeDir()) / kBootLogName).wstring();
}

void BootLogReset() {
    std::ofstream f(BootLogPath(), std::ios::trunc | std::ios::binary);
    if (!f) return;
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char ts[64]{};
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    f << "=== QstWebView boot " << ts << " ===\n";
}

void BootLogLine(const std::string& line) {
    std::ofstream f(BootLogPath(), std::ios::app | std::ios::binary);
    if (!f) return;
    f << line << '\n';
}

void BootLogLineW(const std::wstring& line) {
    BootLogLine(NarrowUtf8(line));
}

void ShowShellError(const wchar_t* detail) {
    // 禁止用 cloak/透明主窗做 owner：部分 Win11/杀软环境下对话框会完全看不见
    MessageBoxW(nullptr, detail ? detail : L"启动失败。", L"键鼠工坊",
        MB_ICONERROR | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
}

std::mutex& StartupLogMutex() {
    static std::mutex m;
    return m;
}

void StartupTrace(const char* line) {
    // 极早路径写盘：exe 旁 + %LOCALAPPDATA%\QuickScriptTool（安装到 Program Files 时旁路可能无写权限）
    std::lock_guard<std::mutex> lock(StartupLogMutex());
    auto appendOne = [&](const std::filesystem::path& path) {
        try {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream f(path, std::ios::app | std::ios::binary);
            if (!f) return;
            const auto now = std::chrono::system_clock::now();
            const std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
            localtime_s(&tm, &t);
            char ts[64]{};
            std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
            f << ts << " " << (line ? line : "") << '\n';
        } catch (...) {
        }
    };
    appendOne(std::filesystem::path(ExeDir()) / L"shell_startup.log");
    wchar_t localApp[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) {
        appendOne(std::filesystem::path(localApp) / L"QuickScriptTool" / L"shell_startup.log");
    }
}

// 静态初始化打点：若只有这一行没有 wWinMain，说明卡在其它静态构造/依赖加载
struct QstEarlyBootMarker {
    QstEarlyBootMarker() { StartupTrace("crt_static_init"); }
} g_qstEarlyBootMarker;

static bool MediaFoundationPresent() {
    static const wchar_t* kMf[] = { L"MFPlat.DLL", L"MF.dll", L"MFReadWrite.dll" };
    for (const wchar_t* name : kMf) {
        HMODULE m = GetModuleHandleW(name);
        if (!m) m = LoadLibraryW(name);
        if (!m) {
            char buf[96]{};
            sprintf_s(buf, "MF missing");
            StartupTrace(buf);
            return false;
        }
    }
    return true;
}

// OpenCV 延迟加载失败时弹窗并退出（含静态构造阶段触发的失败）
FARPROC WINAPI QstDelayLoadFailureHook(unsigned dliNotify, PDelayLoadInfo pdli) {
    if (dliNotify == dliFailLoadLib || dliNotify == dliFailGetProc) {
        const char* name = (pdli && pdli->szDll) ? pdli->szDll : "dependency";
        StartupTrace((std::string("DELAYLOAD fail: ") + name).c_str());
        ShowShellError(
            L"无法加载 opencv_world4100.dll（或其系统依赖）。\n\n"
            L"请检查：\n"
            L"1) 软件目录是否有 opencv_world4100.dll（勿只拷 exe）\n"
            L"2) 杀软隔离区是否隔离了该文件\n"
            L"3) Win N/KN 是否已安装「媒体功能包」（MFPlat/MF/MFReadWrite）\n\n"
            L"日志：软件目录 shell_startup.log\n"
            L"或 %LOCALAPPDATA%\\QuickScriptTool\\shell_startup.log");
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return nullptr;
}

bool EnsureOpenCvLoadable() {
    // opencv_world 导入 MFPlat/MF/MFReadWrite；Win N 精简版常缺
    if (!MediaFoundationPresent()) {
        ShowShellError(
            L"无法启动：系统缺少 Media Foundation 组件。\n\n"
            L"常见于 Windows N/KN 精简版。请安装「媒体功能包」后重试。\n\n"
            L"日志：软件目录 shell_startup.log\n"
            L"或 %LOCALAPPDATA%\\QuickScriptTool\\shell_startup.log");
        return false;
    }
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::filesystem::path beside =
        std::filesystem::path(exePath).parent_path() / L"opencv_world4100.dll";
    if (GetFileAttributesW(beside.c_str()) == INVALID_FILE_ATTRIBUTES) {
        StartupTrace("opencv_world4100.dll missing beside exe");
        ShowShellError(
            L"无法启动：软件目录缺少 opencv_world4100.dll。\n\n"
            L"请使用完整安装包/便携包（不要只拷贝 exe）。\n"
            L"若文件曾在，请检查杀软隔离区并恢复后加入白名单。\n\n"
            L"日志：软件目录或 %LOCALAPPDATA%\\QuickScriptTool\\shell_startup.log");
        return false;
    }
    // 绝对路径加载，避免工作目录不是 exe 目录时找不到
    HMODULE cv = LoadLibraryW(beside.c_str());
    if (!cv) {
        const DWORD err = GetLastError();
        char buf[96]{};
        sprintf_s(buf, "LoadLibrary opencv_world4100.dll failed err=%lu", err);
        StartupTrace(buf);
        ShowShellError(
            L"无法加载 opencv_world4100.dll。\n\n"
            L"可能原因：\n"
            L"1) 杀软隔离/删除了该 DLL\n"
            L"2) 安装包不完整\n"
            L"3) 系统缺少 Media Foundation（Win N 请装媒体功能包）\n\n"
            L"日志：软件目录或 %LOCALAPPDATA%\\QuickScriptTool\\shell_startup.log");
        return false;
    }
    StartupTrace("opencv_world4100.dll loaded");
    return true;
}

// 静默 icacls /T：解压后已有文件也需 AppContainer RX（仅改目录 ACE 不够）
bool RunIcaclsGrantTree(const std::wstring& folder, const wchar_t* sid) {
    if (folder.empty() || !sid) return false;
    wchar_t sysDir[MAX_PATH]{};
    if (!GetSystemDirectoryW(sysDir, MAX_PATH)) return false;
    const std::wstring exe = std::wstring(sysDir) + L"\\icacls.exe";
    std::wstring cmd = L"\"" + exe + L"\" \"" + folder + L"\" /grant *" + sid
        + L":(OI)(CI)(RX) /T /C /Q";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}

// 直接以 Win32 API 给单个文件/目录追加 AppContainer RX ACE，
// 不依赖外部 icacls.exe（部分机器杀软/组策略会拦截 icacls，导致固定运行时
// 子进程读不到文件 -> 渲染进程崩溃 -> 淡蓝空白页）。
static bool GrantAclWin32One(const std::wstring& path, const wchar_t* sidStr, bool isDir) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(sidStr, &sid) || !sid) return false;
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = isDir ? (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)
                              : NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);

    PACL newAcl = nullptr;
    const DWORD err1 = SetEntriesInAclW(1, &ea, nullptr, &newAcl);
    LocalFree(sid);
    if (err1 != ERROR_SUCCESS) return false;

    const DWORD err2 = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, newAcl, nullptr);
    LocalFree(newAcl);
    return err2 == ERROR_SUCCESS;
}

// 递归给目录树追加 AppContainer RX（S-1-15-2-1 / S-1-15-2-2）。
// 返回失败条目数；0 表示全部成功。
static int GrantAclTreeWin32(const std::wstring& root) {
    int failed = 0;
    auto grantOne = [&failed](const std::wstring& p, bool isDir) {
        if (!GrantAclWin32One(p, L"S-1-15-2-1", isDir)) ++failed;
        if (!GrantAclWin32One(p, L"S-1-15-2-2", isDir)) ++failed;
    };
    grantOne(root, true);
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const bool isDir = it->is_directory(ec);
        grantOne(it->path().wstring(), isDir);
    }
    return failed;
}

void EnsureFixedRuntimeAppContainerAcl(const std::wstring& folder) {
    if (folder.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec)) {
        BootLogLine("ACL: folder missing");
        return;
    }
    const auto marker = std::filesystem::path(folder) / kAclMarkerName;
    if (std::filesystem::exists(marker, ec)) {
        BootLogLine("ACL: skip (marker .qst_acl_ok present)");
        return;
    }
    BootLogLineW(L"ACL: running grant on " + folder);
    StartupTrace("ACL begin");
    const int win32Failed = GrantAclTreeWin32(folder);
    BootLogLine("ACL: win32 grant failed=" + std::to_string(win32Failed));
    bool ok1 = win32Failed == 0;
    bool ok2 = ok1;
    if (!ok1) {
        // 兜底：外部 icacls.exe（旧逻辑保留）；缩短等待，避免杀软下卡死数分钟
        ok1 = RunIcaclsGrantTree(folder, L"S-1-15-2-1");
        ok2 = RunIcaclsGrantTree(folder, L"S-1-15-2-2");
        BootLogLine(std::string("ACL: icacls fallback S-1-15-2-1=") + (ok1 ? "ok" : "fail")
            + " S-1-15-2-2=" + (ok2 ? "ok" : "fail"));
    }
    if (ok1 && ok2) {
        std::ofstream mf(marker, std::ios::trunc | std::ios::binary);
        if (mf) mf << "ok\n";
        BootLogLine("ACL: wrote .qst_acl_ok");
    } else {
        BootLogLine("ACL: FAILED - fixed runtime may crash (will try system WebView2 fallback)");
    }
    StartupTrace("ACL end");
}

void RequestLaunchWebView();

void EnsureFixedRuntimeAppContainerAclAsync(const std::wstring& folder) {
    if (!g_hwnd) {
        EnsureFixedRuntimeAppContainerAcl(folder);
        RequestLaunchWebView();
        return;
    }
    BootLogLine("ACL: defer to background thread");
    StartupTrace("ACL defer background");
    SetTimer(g_hwnd, kAclWaitTimerId, 12000, nullptr);
    std::thread([folder]() {
        EnsureFixedRuntimeAppContainerAcl(folder);
        if (g_hwnd && IsWindow(g_hwnd)) {
            PostMessageW(g_hwnd, WM_APP_LAUNCH_WEBVIEW, 0, 0);
        }
    }).detach();
}

bool ResolveFixedBrowserFolder(std::wstring& outFolder) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(ExeDir()) / kFixedDirName;
    if (!fs::exists(root)) return false;
    const fs::path direct = root / L"msedgewebview2.exe";
    if (fs::exists(direct)) {
        outFolder = root.wstring();
        return true;
    }
    try {
        for (const auto& ent : fs::directory_iterator(root)) {
            if (!ent.is_directory()) continue;
            const fs::path cand = ent.path() / L"msedgewebview2.exe";
            if (fs::exists(cand)) {
                outFolder = ent.path().wstring();
                return true;
            }
        }
        for (const auto& ent : fs::recursive_directory_iterator(root)) {
            if (!ent.is_regular_file()) continue;
            if (_wcsicmp(ent.path().filename().c_str(), L"msedgewebview2.exe") == 0) {
                outFolder = ent.path().parent_path().wstring();
                return true;
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool JsonGetString(const std::string& json, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = json.find(pat);
    if (k == std::string::npos) return false;
    const auto colon = json.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    const auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    // 须解开 \\ \" 等转义：否则 Windows 路径 D:\\a\\b 会带着双反斜杠，_wcsicmp 匹配失败
    std::string val;
    for (size_t i = q1 + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            val.push_back(json[i + 1]);
            ++i;
            continue;
        }
        if (json[i] == '"') {
            out = std::move(val);
            return true;
        }
        val.push_back(json[i]);
    }
    return false;
}

bool JsonGetInt(const std::string& json, const char* key, int& out) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = json.find(pat);
    if (k == std::string::npos) return false;
    const auto colon = json.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i >= json.size()) return false;
    char* end = nullptr;
    const long v = strtol(json.c_str() + static_cast<ptrdiff_t>(i), &end, 10);
    if (end == json.c_str() + static_cast<ptrdiff_t>(i)) return false;
    out = static_cast<int>(v);
    return true;
}

bool JsonGetDouble(const std::string& json, const char* key, double& out) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = json.find(pat);
    if (k == std::string::npos) return false;
    const auto colon = json.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i >= json.size()) return false;
    char* end = nullptr;
    const double v = strtod(json.c_str() + static_cast<ptrdiff_t>(i), &end);
    if (end == json.c_str() + static_cast<ptrdiff_t>(i)) return false;
    out = v;
    return true;
}

void PostToJs(const std::string& jsonUtf8);

void PostToJsAsync(std::string jsonUtf8) {
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    PostMessageW(g_hwnd, WM_BRIDGE_POST_JS, 0,
        reinterpret_cast<LPARAM>(new std::string(std::move(jsonUtf8))));
}

UINT ReadAsyncModifierMask(bool excludeWin) {
    UINT mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if (!excludeWin && ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)))
        mods |= MOD_WIN;
    return mods;
}

bool IsPureModifierVk(UINT vk) {
    return vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT
        || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL
        || vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU
        || vk == VK_LWIN || vk == VK_RWIN;
}

LRESULT CALLBACK WebActionKeyLlProc(int code, WPARAM wp, LPARAM lp);
void StartWebActionKeyLlHook();
void StopWebActionKeyLlHook();
bool StartWebHotkeyLlHook(double holdSec);
void StopWebHotkeyLlHook();
void OnWebHotkeyHoldTimer();
void TickWebCaptureWatchdog();
void EmergencyUnhookWebCaptureLl();

LRESULT CALLBACK WebActionKeyLlProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_webActionKeyLlActive
        && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        g_webActionKeyLastEventTick = GetTickCount();
        const auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (ks && !(ks->flags & LLKHF_INJECTED)) {
            const UINT vk = static_cast<UINT>(ks->vkCode);
            const bool winAsKey = (vk == VK_LWIN || vk == VK_RWIN);
            // 单独修饰键忽略；Win 可作为动作主键
            if (IsPureModifierVk(vk) && !winAsKey) {
                return CallNextHookEx(g_webActionKeyLl, code, wp, lp);
            }
            if (vk == VK_SNAPSHOT) {
                return CallNextHookEx(g_webActionKeyLl, code, wp, lp);
            }
            const UINT mods = winAsKey ? 0 : ReadAsyncModifierMask(true);
            PostToJsAsync(std::string("{\"type\":\"actionKey.llKey\",\"ok\":true,\"keyVk\":")
                + std::to_string(vk)
                + ",\"hotkeyModifiers\":" + std::to_string(mods) + "}");
        }
    }
    return CallNextHookEx(g_webActionKeyLl, code, wp, lp);
}

void StartWebActionKeyLlHook() {
    StopWebHotkeyLlHook();
    StopWebActionKeyLlHook();
    g_webActionKeyLlActive = true;
    g_webActionKeyLastEventTick = GetTickCount();
    g_webActionKeyLl = SetWindowsHookExW(
        WH_KEYBOARD_LL, WebActionKeyLlProc, GetModuleHandleW(nullptr), 0);
    if (!g_webActionKeyLl) g_webActionKeyLlActive = false;
    if (g_hwnd && IsWindow(g_hwnd))
        SetTimer(g_hwnd, kWebCaptureWatchdogTimerId, 2000, nullptr);
}

void StopWebActionKeyLlHook() {
    g_webActionKeyLlActive = false;
    if (g_hwnd && IsWindow(g_hwnd)) KillTimer(g_hwnd, kWebCaptureWatchdogTimerId);
    if (g_webActionKeyLl) {
        UnhookWindowsHookEx(g_webActionKeyLl);
        g_webActionKeyLl = nullptr;
    }
}

void PostWebHotkeyUpdate(UINT vk, UINT mods, bool hold, const char* edge) {
    PostToJsAsync(std::string("{\"type\":\"hotkey.llUpdate\",\"ok\":true,\"keyVk\":")
        + std::to_string(vk)
        + ",\"hotkeyModifiers\":" + std::to_string(mods)
        + ",\"hotkeyHold\":" + (hold ? "true" : "false")
        + ",\"hotkeyEdge\":\"" + (edge ? edge : "down") + "\"}");
}

void CancelWebHotkeyPending() {
    g_webHotkeyPending = false;
    g_webHotkeyPendingVk = 0;
    g_webHotkeyPendingMods = 0;
    g_webHotkeyPendingTick = 0;
    if (g_hwnd && IsWindow(g_hwnd)) KillTimer(g_hwnd, kWebHotkeyHoldTimerId);
}

void BeginWebHotkeyPending(UINT vk, UINT mods) {
    CancelWebHotkeyPending();
    g_webHotkeyPending = true;
    g_webHotkeyPendingVk = vk;
    g_webHotkeyPendingMods = mods;
    g_webHotkeyPendingTick = GetTickCount();
    PostWebHotkeyUpdate(vk, mods, false, "down");
    if (g_hwnd && IsWindow(g_hwnd) && g_webHotkeyHoldMs > 0) {
        SetTimer(g_hwnd, kWebHotkeyHoldTimerId, g_webHotkeyHoldMs, nullptr);
    }
}

void FinishWebHotkeyPending(UINT vk) {
    if (!g_webHotkeyPending || vk != g_webHotkeyPendingVk) return;
    const bool hold = (GetTickCount() - g_webHotkeyPendingTick) >= g_webHotkeyHoldMs;
    const UINT mods = g_webHotkeyPendingMods;
    CancelWebHotkeyPending();
    PostWebHotkeyUpdate(vk, mods, hold, "up");
}

void OnWebHotkeyHoldTimer() {
    if (!g_webHotkeyLlActive || !g_webHotkeyPending) {
        CancelWebHotkeyPending();
        return;
    }
    if ((GetTickCount() - g_webHotkeyPendingTick) < g_webHotkeyHoldMs) return;
    if (g_hwnd && IsWindow(g_hwnd)) KillTimer(g_hwnd, kWebHotkeyHoldTimerId);
    PostWebHotkeyUpdate(g_webHotkeyPendingVk, g_webHotkeyPendingMods, true, "hold");
}

LRESULT CALLBACK WebHotkeyKbLlProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_webHotkeyLlActive) {
        g_webHotkeyKbLastEventTick = GetTickCount();
        const auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (ks && !(ks->flags & LLKHF_INJECTED)) {
            const UINT vk = static_cast<UINT>(ks->vkCode);
            if (IsPureModifierVk(vk) || vk == VK_SNAPSHOT) {
                return CallNextHookEx(g_webHotkeyKbLl, code, wp, lp);
            }
            if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
                if (g_webHotkeyPending && g_webHotkeyPendingVk == vk) {
                    return CallNextHookEx(g_webHotkeyKbLl, code, wp, lp);
                }
                BeginWebHotkeyPending(vk, ReadAsyncModifierMask(false));
            } else if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
                FinishWebHotkeyPending(vk);
            }
        }
    }
    return CallNextHookEx(g_webHotkeyKbLl, code, wp, lp);
}

bool StartWebHotkeyLlHook(double holdSec) {
    // 始终卸旧钩再装：Windows 可能因超时静默卸掉 LL，若仅看 g_webHotkeyLlActive
    // 会误以为仍在捕获，弹层一直停在「捕获中…」
    StopWebHotkeyLlHook();
    StopWebActionKeyLlHook();
    g_webHotkeyHoldMs = static_cast<DWORD>(
        (std::max)(50.0, (std::min)(5000.0, holdSec * 1000.0)));
    CancelWebHotkeyPending();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    // 仅键盘 LL：鼠标热键走设置里的预设，避免点「确定」被当成「鼠标左键」
    g_webHotkeyKbLastEventTick = GetTickCount();
    g_webHotkeyKbLl = SetWindowsHookExW(WH_KEYBOARD_LL, WebHotkeyKbLlProc, inst, 0);
    g_webHotkeyMouseLl = nullptr;
    g_webHotkeyLlActive = (g_webHotkeyKbLl != nullptr);
    if (g_hwnd && IsWindow(g_hwnd) && g_webHotkeyLlActive)
        SetTimer(g_hwnd, kWebCaptureWatchdogTimerId, 2000, nullptr);
    return g_webHotkeyLlActive;
}

void StopWebHotkeyLlHook() {
    CancelWebHotkeyPending();
    g_webHotkeyLlActive = false;
    if (g_hwnd && IsWindow(g_hwnd)) KillTimer(g_hwnd, kWebCaptureWatchdogTimerId);
    if (g_webHotkeyKbLl) {
        UnhookWindowsHookEx(g_webHotkeyKbLl);
        g_webHotkeyKbLl = nullptr;
    }
    if (g_webHotkeyMouseLl) {
        UnhookWindowsHookEx(g_webHotkeyMouseLl);
        g_webHotkeyMouseLl = nullptr;
    }
}

void EmergencyUnhookWebCaptureLl() {
    StopWebHotkeyLlHook();
    StopWebActionKeyLlHook();
}

/// 捕获期间 LL 钩子存活看门狗（2s 间隔）：Windows 超时会静默卸载 LL，钩子句柄
/// 仍非空。捕获弹层表现为「一直捕获中/按了没反应」。这里以系统输入活动做参照，
/// 检测到 LL 无事件即卸旧重装；重装失败则提示用户关闭弹窗重试。
void TickWebCaptureWatchdog() {
    if (!g_webHotkeyLlActive && !g_webActionKeyLlActive) {
        if (g_hwnd && IsWindow(g_hwnd)) KillTimer(g_hwnd, kWebCaptureWatchdogTimerId);
        return;
    }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const DWORD now = GetTickCount();
    LASTINPUTINFO lii{};
    lii.cbSize = sizeof(lii);
    DWORD lastInput = 0;
    if (GetLastInputInfo(&lii)) lastInput = lii.dwTime;
    const bool userActive = (now - lastInput) < 5000u;

    if (g_webHotkeyLlActive) {
        const bool stale = (now - g_webHotkeyKbLastEventTick) > 5000u;
        if (!g_webHotkeyKbLl || (userActive && stale)) {
            if (g_webHotkeyKbLl) UnhookWindowsHookEx(g_webHotkeyKbLl);
            g_webHotkeyKbLl = SetWindowsHookExW(
                WH_KEYBOARD_LL, WebHotkeyKbLlProc, inst, 0);
            if (!g_webHotkeyKbLl) {
                BootLogLine("HOTKEY: capture LLKB watchdog reinstall FAILED");
                g_webHotkeyLlActive = false;
                PostToJsAsync(
                    "{\"type\":\"engine.toast\",\"ok\":true,"
                    "\"text\":\"热键捕获钩子失效，请关闭弹窗重新进入捕获\"}");
            } else {
                g_webHotkeyLlActive = true;
                g_webHotkeyKbLastEventTick = GetTickCount();
                BootLogLine("HOTKEY: capture LLKB watchdog reinstall ok");
            }
        }
    }
    if (g_webActionKeyLlActive) {
        const bool stale = (now - g_webActionKeyLastEventTick) > 5000u;
        if (!g_webActionKeyLl || (userActive && stale)) {
            if (g_webActionKeyLl) UnhookWindowsHookEx(g_webActionKeyLl);
            g_webActionKeyLl = SetWindowsHookExW(
                WH_KEYBOARD_LL, WebActionKeyLlProc, inst, 0);
            if (!g_webActionKeyLl) {
                BootLogLine("HOTKEY: capture action LLKB watchdog reinstall FAILED");
                g_webActionKeyLlActive = false;
            } else {
                g_webActionKeyLlActive = true;
                g_webActionKeyLastEventTick = GetTickCount();
                BootLogLine("HOTKEY: capture action LLKB watchdog reinstall ok");
            }
        }
    }
}

void PostToJs(const std::string& jsonUtf8) {
    auto isAgentUiMsg = [](const std::string& j) -> bool {
        return j.find("\"sendAgentMessage.") != std::string::npos
            || j.find("\"openAgentConversation.result\"") != std::string::npos
            || j.find("\"cancelAgentMessage.result\"") != std::string::npos
            || j.find("\"pasteAgentClipboard.result\"") != std::string::npos
            || j.find("\"saveClipboardImage.result\"") != std::string::npos
            || j.find("\"saveImageAs.result\"") != std::string::npos
            || j.find("\"browsePath.result\"") != std::string::npos
            || j.find("\"readImageDataUrl.result\"") != std::string::npos
            || j.find("\"agentWindow.") != std::string::npos
            || j.find("\"openSettingsData.result\"") != std::string::npos
            || j.find("\"listAgentConversations.result\"") != std::string::npos
            || j.find("\"listScripts.result\"") != std::string::npos
            || j.find("\"listRecordings.result\"") != std::string::npos
            || j.find("\"themeCatalog.result\"") != std::string::npos
            || j.find("\"deleteAgentConversation.result\"") != std::string::npos
            || j.find("\"renameAgentConversation.result\"") != std::string::npos
            || j.find("\"listAgentChanges.result\"") != std::string::npos
            || j.find("\"revertAgentChange.result\"") != std::string::npos
            || j.find("\"agentSaveDraft.result\"") != std::string::npos
            || j.find("\"agentConversation.title\"") != std::string::npos;
    };
    // 仅「用户要看的助手窗」才阻断主壳回包。预热窗在 -32000 也 IsWindowVisible，
    // 若误判会吞掉主壳消息，且关进程再开后像「助手打不开 / 主界面假死」。
    auto agentWindowUp = []() -> bool {
        if (!g_agentWantShow || !g_agentHwnd || !IsWindow(g_agentHwnd)) return false;
        if (!IsWindowVisible(g_agentHwnd) || IsIconic(g_agentHwnd)) return false;
        RECT rc{};
        GetWindowRect(g_agentHwnd, &rc);
        if (rc.right < -1000 || rc.bottom < -1000) return false;
        return true;
    };
    const bool agentUp = agentWindowUp();
    const bool agentUi = isAgentUiMsg(jsonUtf8);
    const bool agentChrome = jsonUtf8.find("\"agentWindow.") != std::string::npos;
    // 按键/热键捕获类消息永远属于主壳：即使助手窗正打开也照常回主 WebView，
    // 否则 #ov-action-key / #ov-hotkey 弹层会「前几次按键无反应」
    const bool mainShellCaptureUi =
        jsonUtf8.find("\"actionKey.llKey\"") != std::string::npos
        || jsonUtf8.find("\"hotkey.llUpdate\"") != std::string::npos
        || jsonUtf8.find("\"formatHotkey.result\"") != std::string::npos
        || jsonUtf8.find("\"beginHotkeyCapture.result\"") != std::string::npos
        || jsonUtf8.find("\"endHotkeyCapture.result\"") != std::string::npos;
    if (agentUi && g_agentWebview) {
        g_agentWebview->PostWebMessageAsJson(WidenUtf8(jsonUtf8).c_str());
        // 结果回包后再钉一次前台，防止其它路径偶发抢焦点
        if (agentUp && g_agentHwnd
            && jsonUtf8.find("\"sendAgentMessage.result\"") != std::string::npos) {
            if (IsIconic(g_agentHwnd)) ShowWindow(g_agentHwnd, SW_RESTORE);
            SetForegroundWindow(g_agentHwnd);
        }
    } else if (agentUi && g_agentHwnd) {
        g_agentPending.push_back(jsonUtf8);
        CapPendingWebMessages(g_agentPending);
    }
    if (g_webview) {
        // 助手独立窗打开时：agentWindow.* 只给助手；助手会话回包给助手（上面已投）。
        // 主壳业务回包（setMode / openEditor / loadOpt / listScripts 等）必须仍回主 WebView。
        // 旧逻辑用 agentUp 阻断「一切非热键回包」，主窗会 cloak 不揭、尺寸不变、动作列表一直加载。
        if (mainShellCaptureUi || !agentChrome)
            g_webview->PostWebMessageAsJson(WidenUtf8(jsonUtf8).c_str());
    }
}

void PostToDebugJs(const std::string& jsonUtf8) {
    if (!g_debugWebview) return;
    g_debugWebview->PostWebMessageAsJson(WidenUtf8(jsonUtf8).c_str());
}

void PostToAgentJs(const std::string& jsonUtf8) {
    if (!g_agentWebview) return;
    g_agentWebview->PostWebMessageAsJson(WidenUtf8(jsonUtf8).c_str());
}

// Win11 圆角 HWND 缝隙默认白底会露出顶边白线；跟标题栏同色
void SetControllerChromeBg(ICoreWebView2Controller* controller, BYTE r, BYTE g, BYTE b) {
    if (!controller) return;
    ComPtr<ICoreWebView2Controller2> c2;
    if (FAILED(controller->QueryInterface(IID_PPV_ARGS(&c2))) || !c2) return;
    COREWEBVIEW2_COLOR c{};
    c.A = 255;
    c.R = r;
    c.G = g;
    c.B = b;
    c2->put_DefaultBackgroundColor(c);
}

// 预热/启动：HWND 保持「可导航」但不被人眼看见（避免 -32000 闪浅色窗）
void SetHwndClickThroughInvisible(HWND hwnd, bool invisible) {
    if (!hwnd || !IsWindow(hwnd)) return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (invisible) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_NOACTIVATE);
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        // 让 WS_EX_LAYERED 立即生效：否则 ShowWindow 仍按不透明样式显示，启动期会闪一帧背景色
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    } else {
        if (ex & WS_EX_LAYERED)
            SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        ex &= ~static_cast<LONG_PTR>(WS_EX_LAYERED);
        ex &= ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

// 启动延迟揭窗后常丢前台；AttachThreadInput 抢回最顶层
void ForceForegroundWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    const DWORD curTid = GetCurrentThreadId();
    const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const BOOL attached = (fgTid && fgTid != curTid)
        ? AttachThreadInput(fgTid, curTid, TRUE) : FALSE;
    BringWindowToTop(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    if (attached) AttachThreadInput(fgTid, curTid, FALSE);
}

void ApplyControllerRaster1(ICoreWebView2Controller* controller) {
    if (!controller) return;
    controller->put_ZoomFactor(1.0);
    ComPtr<ICoreWebView2Controller3> c3;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&c3))) && c3) {
        c3->put_ShouldDetectMonitorScaleChanges(FALSE);
        c3->put_RasterizationScale(1.0);
    }
}

void DisableWebViewContextMenu(ICoreWebView2* webview) {
    if (!webview) return;
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview->get_Settings(&settings)) && settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
    }
    // 再挡一层：部分运行时仍可能弹出
    ComPtr<ICoreWebView2_11> wv11;
    if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&wv11))) && wv11) {
        wv11->add_ContextMenuRequested(
            Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                    if (args) args->put_Handled(TRUE);
                    return S_OK;
                }).Get(),
            nullptr);
    }
}

/// 导航封锁：只允许本程序本地 UI（file:// 指向 AppDir\ui 下的 .html，或 https://qst.local/*），
/// 其余导航一律取消，防止外部页面进入 WebView 后调用 bridge（跑脚本/截图/读写设置等）。
// 不同机器/系统上 get_Uri 可能返回百分号编码或原始字符的 file:// URL，
// 统一先解码再与真实文件系统路径比较，避免把自己页面 Cancel 掉导致淡蓝。
static std::wstring PercentDecodeFileUri(const std::wstring& in) {
    std::string bytes;
    bytes.reserve(in.size());
    auto hexVal = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'%' && i + 2 < in.size()) {
            const int h = hexVal(in[i + 1]);
            const int l = hexVal(in[i + 2]);
            if (h >= 0 && l >= 0) {
                bytes.push_back(static_cast<char>((h << 4) | l));
                i += 2;
                continue;
            }
        }
        char u8[4]{};
        const int n = WideCharToMultiByte(CP_UTF8, 0, &in[i], 1, u8,
            static_cast<int>(sizeof(u8)), nullptr, nullptr);
        if (n > 0) bytes.append(u8, static_cast<size_t>(n));
        else bytes.push_back(static_cast<char>(in[i] & 0x7F));
    }
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (wlen <= 0) return in;
    std::wstring out(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
        static_cast<int>(bytes.size()), out.data(), wlen);
    return out;
}

bool IsLocalUiNavigation(const std::wstring& uriLower) {
    if (uriLower.rfind(L"about:blank", 0) == 0) return true;
    if (uriLower.rfind(L"https://qst.local/", 0) == 0) return true;
    if (uriLower.rfind(L"file:///", 0) != 0) return false;

    // file:///D:/... → D:\...（剥掉查询/锚点后规范化，防 .. 逃逸）
    std::wstring path = uriLower.substr(8);
    const auto q = path.find_first_of(L"?#");
    if (q != std::wstring::npos) path = path.substr(0, q);
    path = PercentDecodeFileUri(path);
    for (auto& ch : path) {
        if (ch == L'/') ch = L'\\';
    }
    std::error_code ec;
    const auto canon = std::filesystem::weakly_canonical(path, ec);
    if (ec || canon.empty()) return false;

    const auto uiDir = std::filesystem::weakly_canonical(
        std::filesystem::path(AppDir()) / L"ui", ec);
    if (ec || uiDir.empty()) return false;

    std::wstring canonStr = canon.wstring();
    std::wstring uiDirStr = uiDir.wstring();
    for (auto& ch : canonStr) {
        if (ch == L'/') ch = L'\\';
    }
    for (auto& ch : uiDirStr) {
        if (ch == L'/') ch = L'\\';
    }
    if (canonStr.size() <= uiDirStr.size()
        || _wcsnicmp(canonStr.c_str(), uiDirStr.c_str(), uiDirStr.size()) != 0) {
        return false;
    }
    if (canonStr[uiDirStr.size()] != L'\\') return false;

    const auto dot = canonStr.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = canonStr.substr(dot);
    return ext == L".html" || ext == L".htm";
}

/// 三个 WebView 共用：拦截一切非本地导航 + 把 target=_blank 交给默认浏览器。
void InstallWebViewNavigationGuard(ICoreWebView2* webview) {
    if (!webview) return;
    webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if (!args) return S_OK;
                CotaskStr uri;
                if (FAILED(args->get_Uri(uri.put())) || !uri.p) return S_OK;
                std::wstring u = uri.p;
                for (auto& ch : u) {
                    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
                if (!IsLocalUiNavigation(u)) {
                    args->put_Cancel(TRUE);
                }
                return S_OK;
            }).Get(),
        nullptr);

    webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                if (!args) return S_OK;
                CotaskStr uri;
                if (SUCCEEDED(args->get_Uri(uri.put())) && uri.p && uri.p[0]) {
                    std::wstring u = uri.p;
                    for (auto& ch : u) {
                        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
                    }
                    const bool isWebLink = u.rfind(L"http://", 0) == 0
                        || u.rfind(L"https://", 0) == 0;
                    // 仅把 http/https 外链交给默认浏览器，拦截 ms-msdt/search-ms/file 等危险 scheme
                    if (isWebLink) {
                        ShellExecuteW(nullptr, L"open", uri.p, nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(),
        nullptr);
}

void ApplyWindowCornerRadius(HWND hwnd);

void ApplyPopupChrome(HWND hwnd, ICoreWebView2Controller* controller, BYTE r, BYTE g, BYTE b) {
    if (hwnd && IsWindow(hwnd)) {
        ApplyWindowCornerRadius(hwnd);
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif
        // 去掉 Win11 默认浅色窗口描边（顶边白线常见原因）
        const COLORREF border = static_cast<COLORREF>(DWMWA_COLOR_NONE);
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }
    SetControllerChromeBg(controller, r, g, b);
}

// WebView 已捕获鼠标时必须先 ReleaseCapture，否则 HTCAPTION 拖不动
void BeginCaptionDrag(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    ReleaseCapture();
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

void ParseCssHexRgb(const std::string& hex, BYTE& r, BYTE& g, BYTE& b) {
    r = 0x06;
    g = 0x10;
    b = 0x18;
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() >= 6) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        r = static_cast<BYTE>((nibble(h[0]) << 4) | nibble(h[1]));
        g = static_cast<BYTE>((nibble(h[2]) << 4) | nibble(h[3]));
        b = static_cast<BYTE>((nibble(h[4]) << 4) | nibble(h[5]));
    }
}

void ResizeDebugWebView() {
    if (!g_debugController || !g_debugHwnd) return;
    RECT rc{};
    GetClientRect(g_debugHwnd, &rc);
    g_debugController->put_Bounds(rc);
}

void ApplyDebugTopmost() {
    if (!g_debugHwnd || !IsWindow(g_debugHwnd)) return;
    LONG_PTR ex = GetWindowLongPtrW(g_debugHwnd, GWL_EXSTYLE);
    if (g_debugPinned) ex |= WS_EX_TOPMOST;
    else ex &= ~static_cast<LONG_PTR>(WS_EX_TOPMOST);
    SetWindowLongPtrW(g_debugHwnd, GWL_EXSTYLE, ex);
    // 置顶时同时提到前台，避免「标志位开了但仍被挡住」
    SetWindowPos(g_debugHwnd, g_debugPinned ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | (g_debugPinned ? 0 : SWP_NOACTIVATE));
    if (g_debugPinned) {
        BringWindowToTop(g_debugHwnd);
        SetForegroundWindow(g_debugHwnd);
    }
}

void DestroyDebugWebWindow() {
    if (g_debugController) {
        g_debugController->Close();
        g_debugController.Reset();
    }
    g_debugWebview.Reset();
    if (g_debugHwnd && IsWindow(g_debugHwnd)) {
        DestroyWindow(g_debugHwnd);
    }
    g_debugHwnd = nullptr;
    g_debugCreating = false;
    g_debugReady = false;
    g_debugContentReady = false;
    g_debugPending.clear();
}

void HideDebugWebWindow() {
    g_debugWantShow = false;
    if (g_debugHwnd && IsWindow(g_debugHwnd)) ShowWindow(g_debugHwnd, SW_HIDE);
}

LRESULT CALLBACK DebugWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
bool RegisterDebugWndClass(HINSTANCE inst);
void EnsureDebugWebWindow(bool show = true);
void HandleDebugBridgeMessage(const std::string& json);
void DispatchDebugUiMessage(const std::string& jsonUtf8);

void EnsureTrayIcon();
void HandleBridgeMessage(const std::string& json);
void EnsureAgentWebWindow(bool show = true);
void OpenAgentWebWindow(const std::string& openJson);
void DestroyAgentWebWindow();
void ScheduleSecondaryWebViewPrewarm();
void PrewarmSecondaryWebViews();
void RevealMainWindowIfReady();
void ShowBootPlaceholderIfNeeded();
void GetCenteredPopupPos(int clientW, int clientH, int& outX, int& outY);

std::string EscapeJsonUtf8(const std::string& s);

void PushEngineStatusIfChanged(bool force, bool emitRecordingStopped = true) {
    static int lastPacked = -1;
    static int lastSteps = -1;
    static int lastRunningMode = -1;
    static bool lastDebugging = false;
    static bool lastDebugPaused = false;
    static bool lastDebugStepMode = false;
    static std::string lastScript;
    static std::string lastWmSummary;
    static bool wasRecording = false;
    static DWORD lastStepsPushTick = 0;
    const bool clicking = qst::engine::IsClicking();
    const bool running = qst::engine::IsRunning();
    const bool recording = qst::engine::IsRecording();
    const bool breakoutPaused = running && qst::engine::IsBreakoutPaused();
    const bool debugging = running && qst::engine::IsDebugging();
    const bool debugPaused = debugging && qst::engine::DebugPaused();
    const bool debugStepMode = debugging && qst::engine::DebugStepMode();
    const int steps = running ? qst::engine::ExecutedSteps() : 0;
    const std::string script = running ? qst::engine::RunningScriptNameUtf8() : std::string();
    const int runningMode = running ? qst::engine::RunningMode() : 0;
    std::string wmSummary = "null";
    if (running) {
        windowmode::WindowModeScriptConfig cfg{};
        if (qst::engine::RunningWindowMode(cfg)) {
            std::wstring wmJson;
            windowmode::WriteWindowModeJson(wmJson, cfg, false);
            // WriteWindowModeJson → `  "windowMode": { ... }` — 取对象本体
            const auto brace = wmJson.find(L'{');
            const auto end = wmJson.rfind(L'}');
            if (brace != std::wstring::npos && end != std::wstring::npos && end > brace) {
                wmSummary = ToUtf8(wmJson.substr(brace, end - brace + 1));
            }
        }
    }
    const int packed = (clicking ? 1 : 0) | (running ? 2 : 0) | (recording ? 4 : 0)
        | (breakoutPaused ? 8 : 0);
    const bool recordingEnded = wasRecording && !recording;
    wasRecording = recording;
    const bool flagsChanged = packed != lastPacked || script != lastScript
        || runningMode != lastRunningMode || wmSummary != lastWmSummary
        || debugging != lastDebugging
        || debugPaused != lastDebugPaused
        || debugStepMode != lastDebugStepMode;
    const bool stepsChanged = steps != lastSteps;
    // 运行中 executedSteps 高频变化：至少 200ms 才推一次，避免桥接洪水拖慢 UI
    const DWORD nowTick = GetTickCount();
    const bool stepsDue = stepsChanged && (force || flagsChanged
        || (nowTick - lastStepsPushTick) >= 200u || !running);
    if (!force && !flagsChanged && !stepsDue
        && !(recordingEnded && emitRecordingStopped)) return;
    lastPacked = packed;
    lastSteps = steps;
    lastScript = script;
    lastRunningMode = runningMode;
    lastDebugging = debugging;
    lastDebugPaused = debugPaused;
    lastDebugStepMode = debugStepMode;
    lastWmSummary = wmSummary;
    if (stepsChanged) lastStepsPushTick = nowTick;
    PostToJs(std::string("{\"type\":\"getEngineStatus.result\",\"ok\":true,\"clicking\":")
        + (clicking ? "true" : "false")
        + ",\"running\":" + (running ? "true" : "false")
        + ",\"recording\":" + (recording ? "true" : "false")
        + ",\"breakoutPaused\":" + (breakoutPaused ? "true" : "false")
        + ",\"debugging\":" + (debugging ? "true" : "false")
        + ",\"debugPaused\":" + (debugPaused ? "true" : "false")
        + ",\"debugStepMode\":" + (debugStepMode ? "true" : "false")
        + ",\"executedSteps\":" + std::to_string(steps)
        + ",\"currentScript\":\"" + EscapeJsonUtf8(script) + "\""
        + ",\"runningMode\":" + std::to_string(runningMode)
        + ",\"windowMode\":" + wmSummary + "}");
    // 托盘 tip/icon + 可选壳标题对齐原生脱离态
    EnsureTrayIcon();
    if (g_hwnd && IsWindow(g_hwnd)) {
        if (breakoutPaused) {
            SetWindowTextW(g_hwnd, L"键鼠工坊-脱离中");
        } else if (g_mode == UiMode::Editor) {
            SetWindowTextW(g_hwnd, L"键鼠工坊 — 编辑器");
        } else if (g_mode == UiMode::Optimize) {
            SetWindowTextW(g_hwnd, L"键鼠工坊 — 录制优化");
        } else {
            SetWindowTextW(g_hwnd, L"键鼠工坊");
        }
    }
    if (recordingEnded && emitRecordingStopped) {
        qst::engine::ReloadScriptsAndHotkeys();
        PostToJs(std::string("{\"type\":\"recording.stopped\",\"ok\":true,\"recordings\":")
            + qst::webview::JsonListRecordings() + "}");
        EnsureTrayIcon();
    }
}

std::string EscapeJsonUtf8(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            } else {
                o.push_back(static_cast<char>(c));
            }
        }
    }
    return o;
}

/// 同步 HostObject：准星必须在 pointerdown 时阻塞启动，才能按住拖（postMessage 异步会错过按住态）。
class QstSyncHost final : public IDispatch {
public:
    static HRESULT Create(IDispatch** out) {
        if (!out) return E_POINTER;
        *out = new (std::nothrow) QstSyncHost();
        return *out ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch) {
            *ppv = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) override {
        if (pctinfo) *pctinfo = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(
        REFIID, LPOLESTR* rgszNames, UINT cNames, LCID, DISPID* rgDispId) override {
        if (!rgszNames || !rgDispId || cNames == 0) return E_INVALIDARG;
        for (UINT i = 0; i < cNames; ++i) {
            if (_wcsicmp(rgszNames[i], L"crosshairPick") == 0) {
                rgDispId[i] = 1;
            } else {
                rgDispId[i] = DISPID_UNKNOWN;
                return DISP_E_UNKNOWNNAME;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(
        DISPID dispIdMember, REFIID, LCID, WORD wFlags, DISPPARAMS* pDispParams,
        VARIANT* pVarResult, EXCEPINFO*, UINT*) override {
        if (dispIdMember != 1 || (wFlags & DISPATCH_METHOD) == 0) {
            return DISP_E_MEMBERNOTFOUND;
        }
        std::string mode = "coordinates";
        if (pDispParams && pDispParams->cArgs >= 1) {
            VARIANT arg{};
            VariantInit(&arg);
            const VARIANT& in = pDispParams->rgvarg[pDispParams->cArgs - 1];
            if (SUCCEEDED(VariantChangeType(&arg, const_cast<VARIANT*>(&in), 0, VT_BSTR))
                && arg.bstrVal) {
                mode = NarrowUtf8(arg.bstrVal);
            }
            VariantClear(&arg);
        }
        std::string outJson;
        std::string err;
        bool ok = false;
        if (g_hwnd) {
            const auto r = qst::desktop_tools::CrosshairPick(g_hwnd, mode);
            if (r.ok) {
                ok = true;
                outJson = r.pickJson;
            } else {
                err = r.detail.empty() ? "cancelled" : r.detail;
            }
        } else {
            err = "no hwnd";
        }
        if (pVarResult) {
            VariantInit(pVarResult);
            pVarResult->vt = VT_BSTR;
            std::ostringstream oss;
            if (ok) {
                oss << "{\"ok\":true,\"mode\":\"" << EscapeJsonUtf8(mode)
                    << "\",\"pick\":" << outJson << "}";
            } else {
                oss << "{\"ok\":false,\"detail\":\""
                    << EscapeJsonUtf8(err.empty() ? "cancelled" : err) << "\"}";
            }
            pVarResult->bstrVal = SysAllocString(FromUtf8(oss.str()).c_str());
        }
        return S_OK;
    }

private:
    LONG ref_ = 1;
};

void RegisterSyncHostObject() {
    if (!g_webview) return;
    IDispatch* host = nullptr;
    if (FAILED(QstSyncHost::Create(&host)) || !host) return;
    VARIANT obj{};
    obj.vt = VT_DISPATCH;
    obj.pdispVal = host;
    if (FAILED(g_webview->AddHostObjectToScript(L"qst", &obj))) {
        host->Release();
    }
}

void ApplyWindowCornerRadius(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
    DWORD pref = DWMWCP_ROUND;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref)))) {
        SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    // Fallback: outer window region in window coords
    GetWindowRect(hwnd, &rc);
    const int w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
    constexpr int kRadius = 14;
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, kRadius * 2, kRadius * 2);
    if (rgn) SetWindowRgn(hwnd, rgn, TRUE);
}

void ResizeWebView() {
    if (!g_controller || !g_hwnd) return;
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
    ApplyWindowCornerRadius(g_hwnd);
}

/// DWM cloak：改窗体尺寸时整窗不可见，避免「主页被拉大 + 右侧空白底」露脸
void SetShellCloaked(bool cloak) {
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    if (cloak) {
        BOOL disableTransitions = TRUE;
        DwmSetWindowAttribute(g_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
            &disableTransitions, sizeof(disableTransitions));
    }
    BOOL v = cloak ? TRUE : FALSE;
    if (SUCCEEDED(DwmSetWindowAttribute(g_hwnd, DWMWA_CLOAK, &v, sizeof(v)))) {
        g_shellCloaked = cloak;
    }
    if (!cloak) {
        BOOL disableTransitions = FALSE;
        DwmSetWindowAttribute(g_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
            &disableTransitions, sizeof(disableTransitions));
        // 揭开后强制一帧，避免残留旧尺寸合成
        if (g_controller) {
            ResizeWebView();
            g_controller->put_IsVisible(TRUE);
        }
        InvalidateRect(g_hwnd, nullptr, TRUE);
    }
}

void RemoveTrayIcon() {
    if (!g_trayActive || !g_hwnd) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayActive = false;
}

void EnsureTrayIcon() {
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    if (!g_wmTaskbarCreated) {
        g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    }
    // tip/icon 优先级：录制 > 脱离 > 宏运行 > 连点 > 空闲（对齐原生 EnsureTrayIcon 脱离态）
    const bool recording = qst::engine::IsRecording();
    const bool breakoutPaused = qst::engine::IsBreakoutPaused();
    const bool macroRunning = qst::engine::IsRunning();
    const bool clicking = qst::engine::IsClicking() || g_trayRunning;
    HICON icon = breakoutPaused
        ? LoadBreakoutPauseIconSmall()
        : ((recording || macroRunning || clicking) ? LoadTrayRunningIconSmall() : LoadAppIconSmall());
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!icon) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayId;
    // 首次 NIM_ADD 勿带 NIF_SHOWTIP：部分 Explorer/杀软环境下会直接失败，导致无托盘 + cloak 窗 =「完全没反应」
    nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (recording) wcscpy_s(nid.szTip, L"键鼠工坊-录制中");
    else if (breakoutPaused) wcscpy_s(nid.szTip, L"键鼠工坊-脱离中");
    else if (macroRunning) wcscpy_s(nid.szTip, L"键鼠工坊-运行中");
    else if (clicking) wcscpy_s(nid.szTip, L"键鼠工坊-连点运行中");
    else wcscpy_s(nid.szTip, L"键鼠工坊");

    // 状态未变则跳过 NIM_MODIFY（500ms 定时器）；图标已缓存，仍避免无扰 Explorer
    static HICON s_lastIcon = nullptr;
    static wchar_t s_lastTip[128]{};
    const bool sameVisual = g_trayActive && icon == s_lastIcon
        && wcscmp(nid.szTip, s_lastTip) == 0;
    if (sameVisual) return;

    bool ok = false;
    if (g_trayActive) {
        if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            ok = true;
        } else {
            g_trayActive = false;
            if (Shell_NotifyIconW(NIM_ADD, &nid)) {
                ok = true;
                Shell_NotifyIconW(NIM_SETVERSION, &nid);
            }
        }
    } else if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        ok = true;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        ok = true;
    }
    if (ok) {
        g_trayActive = true;
        s_lastIcon = icon;
        wcscpy_s(s_lastTip, nid.szTip);
        // SETVERSION 后再开 NIF_SHOWTIP（失败可忽略）
        nid.uFlags |= NIF_SHOWTIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    } else {
        BootLogLine("tray: NIM_ADD/MODIFY failed");
        StartupTrace("tray add failed");
    }
}

void ShowBootPlaceholderIfNeeded() {
    if (g_bootPlaceholderShown || g_mainContentReady) return;
    if (!g_mainWantShow || !g_hwnd || !IsWindow(g_hwnd)) return;
    g_bootPlaceholderShown = true;
    BootLogLine("boot placeholder: uncloak before contentReady");
    StartupTrace("boot placeholder");
    SetShellCloaked(false);
    SetHwndClickThroughInvisible(g_hwnd, false);
    ShowWindow(g_hwnd, (g_mainShowCmd == SW_HIDE) ? SW_SHOW : g_mainShowCmd);
    UpdateWindow(g_hwnd);
    ForceForegroundWindow(g_hwnd);
}

void RestoreMainWindow() {
    if (!g_hwnd) return;
    // 二次启动/托盘恢复必须解开 cloak 与 alpha=0，否则用户会看到「点了完全没反应」
    SetShellCloaked(false);
    SetHwndClickThroughInvisible(g_hwnd, false);
    if (g_controller && g_mainContentReady && !g_mainWebVisible) {
        g_controller->put_IsVisible(TRUE);
        g_mainWebVisible = true;
    }
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    else ShowWindow(g_hwnd, SW_SHOW);
    ForceForegroundWindow(g_hwnd);
}

void SetTrayRunning(bool running) {
    g_trayRunning = running;
    EnsureTrayIcon();
}

void ShowTrayContextMenuAt(POINT pt) {
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    // 托盘菜单必须先抢前台，否则 TrackPopupMenu 会粘住/点不中
    SetForegroundWindow(g_hwnd);
    TrayMenuOptions opts;
    opts.enableStop = qst::engine::IsRunning() || qst::engine::IsClicking()
        || qst::engine::IsRecording();
    const TrayMenuAction action = TrayMenu::Show(g_hwnd, pt, opts);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    if (action == TrayMenuAction::ShowWindow) {
        RestoreMainWindow();
    } else if (action == TrayMenuAction::StopRunning) {
        if (qst::engine::IsRunning()) qst::engine::StopScript();
        if (qst::engine::IsClicking()) {
            qst::engine::StopClicker();
            qst::webview::StopClicker();
        }
        if (qst::engine::IsRecording()) {
            std::string path, err;
            int count = 0;
            qst::engine::StopRecordingEx(path, count, err);
        }
        SetTrayRunning(false);
        EnsureTrayIcon();
        PushEngineStatusIfChanged(true);
    } else if (action == TrayMenuAction::Exit) {
        qst::webview::StopClicker();
        SetTrayRunning(false);
        RemoveTrayIcon();
        DestroyWindow(g_hwnd);
    }
}

void ShowTrayContextMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    ShowTrayContextMenuAt(pt);
}

void SetWindowClientSizeCentered(int designW, int designH, bool center = true) {
    if (!g_hwnd) return;
    // HTML mockup 同步：客户区直接用设计像素，WebView RasterizationScale=1 → CSS=客户区。
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workW = (std::max)(1, static_cast<int>(work.right - work.left));
    const int workH = (std::max)(1, static_cast<int>(work.bottom - work.top));
    int wantW = (std::min)(designW, workW);
    int wantH = (std::min)(designH, workH);

    int x = 0, y = 0;
    if (center) {
        // 相对工作区居中（仅启动主界面等显式要求时）
        x = work.left + (workW - wantW) / 2;
        y = work.top + (workH - wantH) / 2;
    } else {
        // 模式切换等：保持当前位置，仅改尺寸
        RECT wr{};
        GetWindowRect(g_hwnd, &wr);
        x = wr.left;
        y = wr.top;
    }
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    if (x + wantW > work.right) x = (std::max)(work.left, work.right - wantW);
    if (y + wantH > work.bottom) y = (std::max)(work.top, work.bottom - wantH);

    // 尺寸未变且不强制居中：跳过，避免无意义挪窗
    if (!center) {
        RECT cur{};
        GetWindowRect(g_hwnd, &cur);
        if ((cur.right - cur.left) == wantW && (cur.bottom - cur.top) == wantH
            && cur.left == x && cur.top == y) {
            return;
        }
    }

    g_applyingModeResize = true;
    for (int i = 0; i < 2; ++i) {
        SetWindowPos(g_hwnd, HWND_TOP, x, y, wantW, wantH,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED | SWP_NOSENDCHANGING);
        MoveWindow(g_hwnd, x, y, wantW, wantH, TRUE);
    }
    g_applyingModeResize = false;
    ResizeWebView();
    ApplyWebViewRasterScale1();
    ApplyWindowCornerRadius(g_hwnd);

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    const UINT dpi = WindowDpi(g_hwnd);
    SetWindowTextW(g_hwnd,
        g_mode == UiMode::Editor ? L"键鼠工坊 — 编辑器"
            : (g_mode == UiMode::Optimize ? L"键鼠工坊 — 录制优化" : L"键鼠工坊"));

    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"window.clientSize\",\"ok\":true,\"clientW\":%d,\"clientH\":%d,"
        "\"expectW\":%d,\"expectH\":%d,\"designW\":%d,\"designH\":%d,\"dpi\":%u,\"match\":%s}",
        cw, ch, wantW, wantH, designW, designH, dpi,
        (cw == wantW && ch == wantH) ? "true" : "false");
    PostToJs(buf);

    // 仅尺寸不符时追加，并限制文件大小，避免长跑占磁盘
    if (cw != wantW || ch != wantH) {
        const std::wstring logPath = ExeDir() + L"\\shell_resize.log";
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER sz{};
            sz.LowPart = fad.nFileSizeLow;
            sz.HighPart = fad.nFileSizeHigh;
            if (sz.QuadPart > 256ull * 1024ull) DeleteFileW(logPath.c_str());
        }
        FILE* f = nullptr;
        if (_wfopen_s(&f, logPath.c_str(), L"a") == 0 && f) {
            const char* modeName = "home";
            if (g_mode == UiMode::Editor) modeName = "editor";
            else if (g_mode == UiMode::Optimize) modeName = "opt";
            fprintf(f, "mode=%s design=%dx%d dpi=%u client=%dx%d expect=%dx%d\n",
                modeName, designW, designH, dpi, cw, ch, wantW, wantH);
            fclose(f);
        }
    }
}

void ApplyUiMode(UiMode mode) {
    if (!g_hwnd) return;
    const UiMode prev = g_mode;
    g_mode = mode;
    // 宏编辑 / 录制优化界面打开时让引擎静默全部启停热键，回主页恢复并强制重注册
    qst::engine::SetUiMode(static_cast<int>(mode));
    if (mode == UiMode::Home) {
        qst::engine::EnsureHotkeysArmed();
    }
    if (mode == UiMode::Home && (prev == UiMode::Editor || prev == UiMode::Optimize)) {
        CleanOrphanImages();
    }
    int cw = g_homeClientW;
    int ch = g_homeClientH;
    const char* modeName = "home";
    if (mode == UiMode::Editor) {
        cw = kEditorClientW;
        ch = kEditorClientH;
        modeName = "editor";
    } else if (mode == UiMode::Optimize) {
        cw = kOptClientW;
        ch = kOptClientH;
        modeName = "opt";
    }
    // 尺寸变化时 cloak：禁止「主页 UI + 大窗空白底」闪帧；由前端 modeReady 揭开
    const bool sizeChanging = (prev != mode);
    if (sizeChanging) SetShellCloaked(true);
    SetWindowClientSizeCentered(cw, ch);
    PostToJs(std::string("{\"type\":\"window.setMode.result\",\"ok\":true,\"mode\":\"")
        + modeName
        + "\",\"clientW\":"
        + std::to_string(cw)
        + ",\"clientH\":"
        + std::to_string(ch)
        + ",\"cloaked\":"
        + (g_shellCloaked ? "true" : "false")
        + "}");
}

void RequestUiMode(UiMode mode) {
    // WebView 回调线程必须切到 UI 线程；同线程则直接 Apply（避免仅 Post 被积压）
    if (!g_hwnd) return;
    const DWORD uiTid = GetWindowThreadProcessId(g_hwnd, nullptr);
    WPARAM wp = 0;
    if (mode == UiMode::Editor) wp = 1;
    else if (mode == UiMode::Optimize) wp = 2;
    if (uiTid == GetCurrentThreadId()) {
        ApplyUiMode(mode);
        return;
    }
    // 异步投递而非 SendMessage 同步等待：运行后 UI 线程若短暂忙碌（如收尾/预览抓帧），
    // 同步 SendMessage 会让 WebView 回调线程阻塞，表现为「打开编辑器卡住、尺寸不变、动作列表空白」。
    PostMessageW(g_hwnd, WM_APPLY_UI_MODE, wp, 0);
}

LRESULT HitTestBorder(HWND /*hwnd*/, LPARAM /*lp*/) {
    // No resize borders (WS_THICKFRAME removed). Move via titlebar → window.drag.
    return HTCLIENT;
}

LRESULT CALLBACK DebugWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        ResizeDebugWebView();
        return 0;
    case WM_SHOWWINDOW:
        // 截屏藏窗/置顶揭开后，强制重绑 Bounds + 控制器可见，避免 WebView2 残留半张旧表面
        if (wp && g_debugController) {
            ResizeDebugWebView();
            g_debugController->put_IsVisible(TRUE);
            ApplyControllerRaster1(g_debugController.Get());
        } else if (!wp && g_debugController) {
            g_debugController->put_IsVisible(FALSE);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_CLOSE:
        // 关闭 = 关掉调试功能（对齐原生 MacroDebug 关窗清设置）
        qst::engine::DebugWindowClosedByUser();
        HideDebugWebWindow();
        return 0;
    case WM_DESTROY:
        if (g_debugController) {
            g_debugController->Close();
            g_debugController.Reset();
        }
        g_debugWebview.Reset();
        if (g_debugHwnd == hwnd) g_debugHwnd = nullptr;
        g_debugCreating = false;
        g_debugReady = false;
        g_debugContentReady = false;
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool RegisterDebugWndClass(HINSTANCE inst) {
    // 跟内容区 sky，勿 BLACK_BRUSH（加载瞬间黑块）
    static HBRUSH s_debugBg = CreateSolidBrush(RGB(0xe8, 0xf4, 0xfc));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DebugWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kDebugWndClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = s_debugBg ? s_debugBg : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.hIcon = LoadAppIcon();
    wc.hIconSm = LoadAppIconSmall();
    return RegisterClassExW(&wc) != 0;
}

void HandleDebugBridgeMessage(const std::string& json) {
    std::string type;
    if (!JsonGetString(json, "type", type)) return;
    if (type == "debugWindowClosed") {
        qst::engine::DebugWindowClosedByUser();
        HideDebugWebWindow();
        return;
    }
    if (type == "debugWindowSetTopmost") {
        int topmost = 1;
        JsonGetInt(json, "topmost", topmost);
        g_debugPinned = topmost != 0;
        ApplyDebugTopmost();
        // 回传实际状态，避免前端与 HWND 不一致
        PostToDebugJs(std::string("{\"type\":\"debugWindowSetTopmost.result\",\"ok\":true,\"topmost\":")
            + (g_debugPinned ? "1" : "0") + "}");
        return;
    }
    if (type == "debugWindow.minimize") {
        // 对齐原生 MacroDebugWindow：整窗最小化到任务栏（非压成标题条）
        if (!g_debugHwnd || !IsWindow(g_debugHwnd)) return;
        ShowWindow(g_debugHwnd, SW_MINIMIZE);
        return;
    }
    if (type == "debugWindow.drag") {
        BeginCaptionDrag(g_debugHwnd);
        return;
    }
    if (type == "debugWindow.ready" || type == "debugWindow.needTheme") {
        // 向主壳要当前主题 CSS 变量，再转发到调试窗
        PostToJs("{\"type\":\"debugWindow.needTheme\"}");
        return;
    }
    if (type == "debugWindow.contentReady") {
        g_debugContentReady = true;
        if (g_debugController) {
            ResizeDebugWebView();
            g_debugController->put_IsVisible(TRUE);
        }
        if (g_debugWantShow) {
            if (IsIconic(g_debugHwnd)) ShowWindow(g_debugHwnd, SW_RESTORE);
            else ShowWindow(g_debugHwnd, SW_SHOW);
            ApplyDebugTopmost();
            ForceForegroundWindow(g_debugHwnd);
        }
        return;
    }
}

void EnsureDebugWebWindow(bool show) {
    if (!g_webviewEnv) return;
    if (show) g_debugWantShow = true;
    auto reveal = []() {
        if (!g_debugWantShow || !g_debugHwnd || !IsWindow(g_debugHwnd)) return;
        // 等真 DOM chrome，禁止先露 navy/黑块
        if (!g_debugContentReady) return;
        if (g_debugController) {
            ResizeDebugWebView();
            g_debugController->put_IsVisible(TRUE);
            ApplyControllerRaster1(g_debugController.Get());
        }
        const bool alreadyUp = IsWindowVisible(g_debugHwnd) && !IsIconic(g_debugHwnd);
        if (IsIconic(g_debugHwnd)) ShowWindow(g_debugHwnd, SW_RESTORE);
        else if (!alreadyUp) ShowWindow(g_debugHwnd, SW_SHOW);
        ApplyDebugTopmost();
        if (!alreadyUp) ForceForegroundWindow(g_debugHwnd);
    };
    if (g_debugHwnd && IsWindow(g_debugHwnd) && g_debugWebview) {
        reveal();
        return;
    }
    if (g_debugCreating) return;
    if (!g_debugHwnd || !IsWindow(g_debugHwnd)) {
        const int x = 120, y = 120;
        g_debugHwnd = CreateWindowExW(
            0,
            kDebugWndClass, L"调试信息输出窗口",
            WS_POPUP | WS_MINIMIZEBOX,
            x, y, kDebugClientW, kDebugClientH,
            nullptr, nullptr, g_instance, nullptr);
        if (!g_debugHwnd) return;
        ApplyTaskbarWindowStyle(g_debugHwnd, L"调试信息输出窗口", true);
        g_debugPinned = true;
        // 未绘页用 sky，勿 navy 黑屏
        ApplyPopupChrome(g_debugHwnd, nullptr, 0xe8, 0xf4, 0xfc);
    }
    if (g_debugController && g_debugWebview) {
        reveal();
        return;
    }
    g_debugCreating = true;
    g_debugContentReady = false;
    g_webviewEnv->CreateCoreWebView2Controller(g_debugHwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                g_debugCreating = false;
                if (FAILED(result) || !controller || !g_debugHwnd) return result;
                g_debugController = controller;
                g_debugController->get_CoreWebView2(&g_debugWebview);
                if (!g_debugWebview) return E_FAIL;
                ComPtr<ICoreWebView2Settings> settings;
                if (SUCCEEDED(g_debugWebview->get_Settings(&settings)) && settings) {
                    settings->put_IsStatusBarEnabled(FALSE);
                    settings->put_IsZoomControlEnabled(FALSE);
                }
                DisableWebViewContextMenu(g_debugWebview.Get());
                ApplyControllerRaster1(g_debugController.Get());
                g_debugController->put_IsVisible(TRUE);
                ApplyPopupChrome(g_debugHwnd, g_debugController.Get(), 0xe8, 0xf4, 0xfc);
                SetControllerChromeBg(g_debugController.Get(), 0xe8, 0xf4, 0xfc);
                ComPtr<ICoreWebView2_3> debugWv3;
                                if (SUCCEEDED(g_debugWebview.As(&debugWv3)) && debugWv3) {
                                    debugWv3->SetVirtualHostNameToFolderMapping(
                                        L"qst.local",
                                        AppDir().c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                    InstallQstLocalAccessGuard(g_debugWebview.Get());
                                }
                                InstallWebViewNavigationGuard(g_debugWebview.Get());
                                g_debugWebview->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            BOOL ok = FALSE;
                            if (args) args->get_IsSuccess(&ok);
                            if (!ok) {
                                static bool s_fallbackTried = false;
                                if (!s_fallbackTried && g_debugWebview) {
                                    s_fallbackTried = true;
                                    g_debugWebview->Navigate(PathToFileUrl(UiDebugPath()).c_str());
                                }
                                return S_OK;
                            }
                            g_debugReady = true;
                            for (const auto& j : g_debugPending) PostToDebugJs(j);
                            g_debugPending.clear();
                            PostToJs("{\"type\":\"debugWindow.needTheme\"}");
                            // 勿在此 Show：等 debugWindow.contentReady
                            if (!g_debugWantShow) ShowWindow(g_debugHwnd, SW_HIDE);
                            else if (g_debugContentReady) {
                                ShowWindow(g_debugHwnd, SW_SHOW);
                                ApplyDebugTopmost();
                            }
                            return S_OK;
                        }).Get(), nullptr);
                g_debugWebview->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            CotaskStr asStr;
                            if (SUCCEEDED(args->TryGetWebMessageAsString(asStr.put())) && asStr.p) {
                                HandleDebugBridgeMessage(NarrowUtf8(asStr.get()));
                                return S_OK;
                            }
                            CotaskStr raw;
                            if (SUCCEEDED(args->get_WebMessageAsJson(raw.put())) && raw.p) {
                                HandleDebugBridgeMessage(NarrowUtf8(raw.get()));
                            }
                            return S_OK;
                        }).Get(), nullptr);
                ResizeDebugWebView();
                g_debugReady = false;
                g_debugContentReady = false;
                ShowWindow(g_debugHwnd, SW_HIDE);
                g_debugWebview->Navigate(L"https://qst.local/ui/debug.html");
                return S_OK;
            }).Get());
}

void DispatchDebugUiMessage(const std::string& jsonUtf8) {
    // 仅 debugWindow.show 才创建/显示顶层窗。
    // 旧逻辑在 !g_debugReady 时对 hide/append 也 Ensure(...true)，
    // 导致切 Tab → saveSettings → ReloadSettings → Hide 反而弹出宏调试窗。
    const bool isShow = jsonUtf8.find("\"debugWindow.show\"") != std::string::npos;
    const bool isHide = jsonUtf8.find("\"debugWindow.hide\"") != std::string::npos;

    if (isShow) {
        EnsureDebugWebWindow(true);
    } else if (isHide) {
        HideDebugWebWindow();
        // 丢弃尚未冲刷的 show/日志，避免预热完成后又被 show 拉起
        g_debugPending.erase(
            std::remove_if(g_debugPending.begin(), g_debugPending.end(),
                [](const std::string& j) {
                    return j.find("\"debugWindow.show\"") != std::string::npos
                        || j.find("\"debugWindow.append\"") != std::string::npos
                        || j.find("\"debugWindow.appendBatch\"") != std::string::npos
                        || j.find("\"debugWindow.clear\"") != std::string::npos;
                }),
            g_debugPending.end());
    }

    if (g_debugReady && g_debugWebview) {
        PostToDebugJs(jsonUtf8);
        return;
    }
    if (!isHide) {
        g_debugPending.push_back(jsonUtf8);
        CapPendingWebMessages(g_debugPending);
    }
    // 未就绪：只为显式 show 去创建；hide/append 不得 Ensure(true)
    if (isShow) EnsureDebugWebWindow(true);
}

void ResizeAgentWebView() {
    if (!g_agentController || !g_agentHwnd) return;
    RECT rc{};
    GetClientRect(g_agentHwnd, &rc);
    g_agentController->put_Bounds(rc);
}

void ApplyAgentTopmost() {
    if (!g_agentHwnd || !IsWindow(g_agentHwnd)) return;
    LONG_PTR ex = GetWindowLongPtrW(g_agentHwnd, GWL_EXSTYLE);
    if (g_agentPinned) ex |= WS_EX_TOPMOST;
    else ex &= ~static_cast<LONG_PTR>(WS_EX_TOPMOST);
    SetWindowLongPtrW(g_agentHwnd, GWL_EXSTYLE, ex);
    SetWindowPos(g_agentHwnd, g_agentPinned ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void GetCenteredPopupPos(int clientW, int clientH, int& outX, int& outY) {
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    outX = wa.left + (std::max)(0L, ((wa.right - wa.left) - clientW) / 2);
    outY = wa.top + (std::max)(0L, ((wa.bottom - wa.top) - clientH) / 2);
}

void RevealMainWindowIfReady() {
    if (!g_mainWantShow || !g_hwnd || !IsWindow(g_hwnd)) return;
    if (!g_mainContentReady) return;
    if (g_controller) {
        g_controller->put_IsVisible(TRUE);
        g_mainWebVisible = true;
        ResizeWebView();
    }
    // 已在工作区坐标、alpha=0 下完成首绘；此处只揭透明度，避免屏外移入时闪空蓝
    if (g_mode == UiMode::Editor)
        SetWindowClientSizeCentered(kEditorClientW, kEditorClientH);
    else if (g_mode == UiMode::Optimize)
        SetWindowClientSizeCentered(kOptClientW, kOptClientH);
    else
        SetWindowClientSizeCentered(g_homeClientW, g_homeClientH);
    SetShellCloaked(false);
    SetHwndClickThroughInvisible(g_hwnd, false);
    int cmd = g_mainShowCmd;
    if (cmd == SW_HIDE) cmd = SW_SHOW;
    ShowWindow(g_hwnd, cmd);
    UpdateWindow(g_hwnd);
    ForceForegroundWindow(g_hwnd);
    ScheduleSecondaryWebViewPrewarm();
}

void RevealAgentWindowIfReady() {
    if (!g_agentWantShow || !g_agentHwnd || !IsWindow(g_agentHwnd)) return;
    // 必须等真 DOM chrome（contentReady），禁止先露空 sky 窗
    if (!g_agentContentReady) return;
    SetHwndClickThroughInvisible(g_agentHwnd, false);
    // 用户打开：独立任务栏按钮；预热阶段保持 TOOLWINDOW 不占栏
    ApplyTaskbarWindowStyle(g_agentHwnd, L"AI 助手", true);
    int x = 0, y = 0;
    GetCenteredPopupPos(kAgentClientW, kAgentClientH, x, y);
    SetWindowPos(g_agentHwnd, HWND_TOP, x, y, kAgentClientW, kAgentClientH,
        SWP_SHOWWINDOW);
    if (g_agentController) {
        g_agentController->put_IsVisible(TRUE);
        ResizeAgentWebView();
    }
    if (IsIconic(g_agentHwnd)) ShowWindow(g_agentHwnd, SW_RESTORE);
    else ShowWindow(g_agentHwnd, SW_SHOW);
    ApplyAgentTopmost();
    ForceForegroundWindow(g_agentHwnd);
}

void HideAgentWebWindow() {
    g_agentWantShow = false;
    qst::engine::SetTypingHotkeysMuted(false, 1);
    if (g_agentHwnd && IsWindow(g_agentHwnd)) {
        ShowWindow(g_agentHwnd, SW_HIDE);
        // 关掉后恢复 TOOLWINDOW，避免残留独立任务栏项
        LONG_PTR ex = GetWindowLongPtrW(g_agentHwnd, GWL_EXSTYLE);
        ex |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        ex &= ~WS_EX_APPWINDOW;
        SetWindowLongPtrW(g_agentHwnd, GWL_EXSTYLE, ex);
        if (g_hwnd && IsWindow(g_hwnd))
            SetWindowLongPtrW(g_agentHwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(g_hwnd));
        SetWindowPos(g_agentHwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    // 仅刷主壳「AI 对话列表」；勿顺带 listScripts/listRecordings ——
    // 会触发主壳 maybeRestoreHomeSelection / 选中脚本强制切到「鼠标宏」
    if (g_webview) {
        PostToJs(std::string("{\"type\":\"listAgentConversations.result\",\"ok\":true,\"conversations\":")
            + qst::webview::JsonListAgentConversations() + "}");
    }
}

void DestroyAgentWebWindow() {
    qst::engine::SetTypingHotkeysMuted(false, 1);
    if (g_agentController) {
        g_agentController->Close();
        g_agentController.Reset();
    }
    g_agentWebview.Reset();
    if (g_agentHwnd && IsWindow(g_agentHwnd)) DestroyWindow(g_agentHwnd);
    g_agentHwnd = nullptr;
    g_agentCreating = false;
    g_agentReady = false;
    g_agentContentReady = false;
    g_agentWantShow = false;
    g_agentPending.clear();
    g_agentOpenPending.clear();
}

LRESULT CALLBACK AgentWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        ResizeAgentWebView();
        return 0;
    case WM_CLOSE:
        HideAgentWebWindow();
        return 0;
    case WM_DESTROY:
        qst::engine::SetTypingHotkeysMuted(false, 1);
        if (g_agentController) {
            g_agentController->Close();
            g_agentController.Reset();
        }
        g_agentWebview.Reset();
        if (g_agentHwnd == hwnd) g_agentHwnd = nullptr;
        g_agentCreating = false;
        g_agentReady = false;
        g_agentContentReady = false;
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool RegisterAgentWndClass(HINSTANCE inst) {
    if (!g_agentBgBrush) {
        // 跟内容区 sky，避免 Show 瞬间 navy 黑屏
        g_agentBgBrush = CreateSolidBrush(RGB(0xe8, 0xf4, 0xfc));
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AgentWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kAgentWndClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_agentBgBrush ? g_agentBgBrush
                                      : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.hIcon = LoadAppIcon();
    wc.hIconSm = LoadAppIconSmall();
    return RegisterClassExW(&wc) != 0;
}

void HandleAgentChromeMessage(const std::string& json) {
    std::string type;
    if (!JsonGetString(json, "type", type)) return;
    if (type == "agentWindow.close") {
        g_agentWantShow = false;
        HideAgentWebWindow();
        return;
    }
    if (type == "agentWindow.minimize") {
        if (g_agentHwnd && IsWindow(g_agentHwnd))
            ShowWindow(g_agentHwnd, SW_MINIMIZE);
        return;
    }
    if (type == "agentWindow.drag") {
        BeginCaptionDrag(g_agentHwnd);
        return;
    }
    if (type == "agentWindow.setTopmost") {
        int topmost = 0;
        JsonGetInt(json, "topmost", topmost);
        g_agentPinned = topmost != 0;
        ApplyAgentTopmost();
        return;
    }
    if (type == "agentWindow.contentReady") {
        g_agentContentReady = true;
        if (g_agentController && g_agentWantShow)
            g_agentController->put_IsVisible(TRUE);
        RevealAgentWindowIfReady();
        return;
    }
    // 其余 bridge 与主壳相同（标记来源，便于 browse 父窗与回包路由）
    g_handlingAgentMsg = true;
    HandleBridgeMessage(json);
    g_handlingAgentMsg = false;
}

void FlushAgentPending() {
    if (!g_agentReady || !g_agentWebview) return;
    for (const auto& j : g_agentPending) PostToAgentJs(j);
    g_agentPending.clear();
    if (!g_agentOpenPending.empty()) {
        PostToAgentJs(g_agentOpenPending);
        g_agentOpenPending.clear();
    }
}

void EnsureAgentWebWindow(bool show) {
    if (!g_webviewEnv) return;
    if (show) g_agentWantShow = true;
    auto reveal = []() { RevealAgentWindowIfReady(); };
    if (g_agentHwnd && IsWindow(g_agentHwnd) && g_agentWebview) {
        if (!g_agentCreating) reveal();
        return;
    }
    if (g_agentCreating) return; // wantShow 已置，创建完成后立刻 Reveal
    if (!g_agentHwnd || !IsWindow(g_agentHwnd)) {
        int ax = 0, ay = 0;
        GetCenteredPopupPos(kAgentClientW, kAgentClientH, ax, ay);
        // 预热：TOOLWINDOW + 主窗 owner，不进任务栏；用户打开时再挂 APPWINDOW
        g_agentHwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kAgentWndClass, L"AI 助手",
            WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
            ax, ay, kAgentClientW, kAgentClientH,
            g_hwnd, nullptr, g_instance, nullptr);
        if (!g_agentHwnd) return;
        ApplyPopupChrome(g_agentHwnd, nullptr, 0x06, 0x10, 0x18);
    }
    if (g_agentController && g_agentWebview) {
        reveal();
        return;
    }
    g_agentCreating = true;
    g_agentNavFallbackTried = false;
    g_webviewEnv->CreateCoreWebView2Controller(g_agentHwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                g_agentCreating = false;
                if (FAILED(result) || !controller || !g_agentHwnd) return result;
                g_agentController = controller;
                g_agentController->get_CoreWebView2(&g_agentWebview);
                if (!g_agentWebview) return E_FAIL;
                ComPtr<ICoreWebView2Settings> settings;
                if (SUCCEEDED(g_agentWebview->get_Settings(&settings)) && settings) {
                    settings->put_IsStatusBarEnabled(FALSE);
                    settings->put_IsZoomControlEnabled(FALSE);
                    settings->put_AreHostObjectsAllowed(TRUE);
                }
                DisableWebViewContextMenu(g_agentWebview.Get());
                ApplyControllerRaster1(g_agentController.Get());
                ApplyPopupChrome(g_agentHwnd, g_agentController.Get(), 0xe8, 0xf4, 0xfc);
                // 隐藏 HWND 时仍让 WebView 绘页；等 agentWindow.contentReady 再 Show
                SetControllerChromeBg(g_agentController.Get(), 0xe8, 0xf4, 0xfc);
                g_agentController->put_IsVisible(TRUE);
                ComPtr<ICoreWebView2_3> webview3;
                                if (SUCCEEDED(g_agentWebview.As(&webview3)) && webview3) {
                                    webview3->SetVirtualHostNameToFolderMapping(
                                        L"qst.local",
                                        AppDir().c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                    InstallQstLocalAccessGuard(g_agentWebview.Get());
                                }
                                InstallWebViewNavigationGuard(g_agentWebview.Get());
                                g_agentWebview->AddScriptToExecuteOnDocumentCreated(
                                    L"(function(){"
                                    L"document.documentElement.classList.add('qst-webview','agent-shell');"
                    L"document.addEventListener('contextmenu',function(e){e.preventDefault();},true);"
                    L"window.qstBridge={post:function(o){"
                    L"var s=(typeof o==='string')?o:JSON.stringify(o);"
                    L"if(window.chrome&&chrome.webview)chrome.webview.postMessage(s);"
                    L"}};"
                    // 尽早挂拖拽（不必等 app.js），与主壳同用 mousedown+ReleaseCapture 路径
                    L"document.addEventListener('mousedown',function(e){"
                    L"if(e.button!==0)return;"
                    L"var t=e.target&&e.target.closest?e.target.closest('.dlg-title'):null;"
                    L"if(!t)return;"
                    L"if(e.target.closest&&e.target.closest('.win-btn'))return;"
                    L"if(window.chrome&&chrome.webview)chrome.webview.postMessage("
                    L"JSON.stringify({type:'agentWindow.drag'}));"
                    L"},true);"
                    L"})();",
                    nullptr);
                g_agentWebview->add_NavigationCompleted(
                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            BOOL ok = FALSE;
                            if (args) args->get_IsSuccess(&ok);
                            if (!ok) {
                                if (!g_agentNavFallbackTried && g_agentWebview) {
                                    g_agentNavFallbackTried = true;
                                    const std::wstring agentPath = UiAgentPath();
                                    if (std::filesystem::exists(agentPath)) {
                                        g_agentWebview->Navigate(PathToFileUrl(agentPath).c_str());
                                    } else {
                                        const std::wstring fallback =
                                            PathToFileUrl(UiIndexPath()) + L"?mode=agent";
                                        g_agentWebview->Navigate(fallback.c_str());
                                    }
                                } else if (!g_agentWantShow && g_agentHwnd) {
                                    SetHwndClickThroughInvisible(g_agentHwnd, false);
                                    SetWindowPos(g_agentHwnd, nullptr, 160, 100, kAgentClientW, kAgentClientH,
                                        SWP_NOZORDER | SWP_NOACTIVATE);
                                    ShowWindow(g_agentHwnd, SW_HIDE);
                                }
                                return S_OK;
                            }
                            g_agentReady = true;
                            FlushAgentPending();
                            // 预热绘完后 HIDE（页已在内存）；首次打开只 Show
                            if (!g_agentWantShow) {
                                ShowWindow(g_agentHwnd, SW_HIDE);
                            } else {
                                RevealAgentWindowIfReady();
                            }
                            return S_OK;
                        }).Get(), nullptr);
                g_agentWebview->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            CotaskStr asStr;
                            if (SUCCEEDED(args->TryGetWebMessageAsString(asStr.put())) && asStr.p) {
                                HandleAgentChromeMessage(NarrowUtf8(asStr.get()));
                                return S_OK;
                            }
                            CotaskStr raw;
                            if (SUCCEEDED(args->get_WebMessageAsJson(raw.put())) && raw.p) {
                                HandleAgentChromeMessage(NarrowUtf8(raw.get()));
                            }
                            return S_OK;
                        }).Get(), nullptr);
                ResizeAgentWebView();
                g_agentReady = false;
                g_agentContentReady = false;
                // 预热只 HIDE：禁止 SHOWNOACTIVATE，否则任务栏会出现助手缩略图
                ShowWindow(g_agentHwnd, SW_HIDE);
                g_agentWebview->Navigate(L"https://qst.local/ui/agent.html");
                return S_OK;
            }).Get());
}

void OpenAgentWebWindow(const std::string& openJson) {
    g_agentOpenPending = openJson;
    EnsureAgentWebWindow(true);
    if (g_agentReady) FlushAgentPending();
}

void ScheduleSecondaryWebViewPrewarm() {
    if (g_secondaryPrewarmScheduled || !g_hwnd) return;
    g_secondaryPrewarmScheduled = true;
    // 尽早预热助手（alpha=0），首次打开接近即时
    SetTimer(g_hwnd, kPrewarmTimerId, 50, nullptr);
}

void PrewarmSecondaryWebViews() {
    if (!g_webviewEnv) return;
    if (!g_agentWebview && !g_agentCreating) EnsureAgentWebWindow(false);
    if (!g_debugWebview && !g_debugCreating) EnsureDebugWebWindow(false);
}

void HandleBridgeMessage(const std::string& json) {
    std::string type;
    if (!JsonGetString(json, "type", type) && !JsonGetString(json, "method", type)) {
        PostToJs("{\"type\":\"error\",\"detail\":\"missing type\"}");
        return;
    }

    if (type == "shell.contentReady") {
        // 真 DOM 已就绪：再等一拍合成后再揭 alpha，避免空 DefaultBackground 闪一下
        BootLogLine("shell.contentReady");
        g_mainContentReady = true;
        if (g_hwnd) {
            KillTimer(g_hwnd, kMainRevealTimerId);
            KillTimer(g_hwnd, kMainRevealSettleTimerId);
            KillTimer(g_hwnd, kEvergreenFallbackTimerId);
            SetTimer(g_hwnd, kMainRevealSettleTimerId, 50, nullptr);
        }
        return;
    }

    if (type == "themeCss.apply") {
        // 主壳推送 CSS 变量 → 宏调试独立窗；并同步顶栏底色消白边
        if (g_debugReady && g_debugWebview) PostToDebugJs(json);
        else if (g_debugHwnd) {
            g_debugPending.push_back(json);
            CapPendingWebMessages(g_debugPending);
        }
        auto parseCssVar = [&](const char* key) -> std::string {
            const std::string needle = std::string("\"") + key + "\"";
            const auto at = json.find(needle);
            if (at == std::string::npos) return {};
            const auto colon = json.find(':', at + needle.size());
            const auto q1 = json.find('"', colon + 1);
            const auto q2 = (q1 == std::string::npos) ? std::string::npos : json.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos || q2 <= q1 + 1) return {};
            return json.substr(q1 + 1, q2 - q1 - 1);
        };
        // 助手窗跟 navy；调试窗必须跟 sky——误设成 navy 时，WebView 未绘满会露出「深蓝块+浅蓝底」
        const std::string navy = parseCssVar("--navy-950");
        const std::string sky = parseCssVar("--sky-100");
        if (!navy.empty() && g_agentController) {
            BYTE r = 0, g = 0, b = 0;
            ParseCssHexRgb(navy, r, g, b);
            ApplyPopupChrome(g_agentHwnd, g_agentController.Get(), r, g, b);
        }
        if (g_debugController) {
            BYTE r = 0xe8, g = 0xf4, b = 0xfc;
            if (!sky.empty()) ParseCssHexRgb(sky, r, g, b);
            ApplyPopupChrome(g_debugHwnd, g_debugController.Get(), r, g, b);
        }
        return;
    }

    if (type == "window.minimize" || type == "minimize") {
        ShowWindow(g_hwnd, SW_MINIMIZE);
        return;
    }
    if (type == "window.close" || type == "close") {
        // 对齐原生：closeToTray → 藏托盘；否则退出进程
        const bool toTray = qst::webview::Ctx().settings.other.closeToTray;
        if (toTray) {
            ShowWindow(g_hwnd, SW_HIDE);
            EnsureTrayIcon();
            return;
        }
        qst::webview::StopClicker();
        SetTrayRunning(false);
        RemoveTrayIcon();
        DestroyWindow(g_hwnd);
        return;
    }
    if (type == "window.drag" || type == "drag") {
        BeginCaptionDrag(g_hwnd);
        return;
    }
    if (type == "window.setMode" || type == "setMode") {
        std::string mode;
        JsonGetString(json, "mode", mode);
        UiMode next = UiMode::Home;
        if (mode == "editor") next = UiMode::Editor;
        else if (mode == "opt" || mode == "optimize") next = UiMode::Optimize;
        RequestUiMode(next);
        return;
    }
    if (type == "window.setHomeSize") {
        // 极简 / 专业主窗统一黄金分割；center=0 时不挪到屏幕中央（模式切换）
        int w = kHomeClientW, h = kHomeClientH;
        int center = 1;
        JsonGetInt(json, "w", w);
        JsonGetInt(json, "h", h);
        JsonGetInt(json, "center", center);
        if (w < 800) w = 800;
        if (h < 600) h = 600;
        if (w > 2400) w = 2400;
        if (h > 1600) h = 1600;
        g_homeClientW = w;
        g_homeClientH = h;
        if (g_mode == UiMode::Home) {
            SetWindowClientSizeCentered(g_homeClientW, g_homeClientH, center != 0);
        }
        PostToJs(std::string("{\"type\":\"window.setHomeSize.result\",\"ok\":true,\"clientW\":")
            + std::to_string(g_homeClientW) + ",\"clientH\":" + std::to_string(g_homeClientH) + "}");
        return;
    }
    if (type == "listLibraryFolders") {
        std::string kind;
        JsonGetString(json, "kind", kind);
        PostToJs(std::string("{\"type\":\"listLibraryFolders.result\",\"ok\":true,\"kind\":\"")
            + EscapeJsonUtf8(kind) + "\",\"folders\":"
            + qst::webview::JsonListLibraryFolders(kind) + "}");
        return;
    }
    if (type == "createLibraryFolder") {
        std::string kind, folder, err;
        JsonGetString(json, "kind", kind);
        JsonGetString(json, "folder", folder);
        if (!qst::webview::CreateLibraryFolder(kind, folder, err)) {
            PostToJs(std::string("{\"type\":\"createLibraryFolder.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        PostToJs("{\"type\":\"createLibraryFolder.result\",\"ok\":true}");
        return;
    }
    if (type == "renameLibraryFolder") {
        std::string kind, folder, name, err;
        JsonGetString(json, "kind", kind);
        JsonGetString(json, "folder", folder);
        JsonGetString(json, "name", name);
        if (!qst::webview::RenameLibraryFolder(kind, folder, name, err)) {
            PostToJs(std::string("{\"type\":\"renameLibraryFolder.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        qst::engine::ReloadScriptsAndHotkeys();
        PostToJs(std::string("{\"type\":\"renameLibraryFolder.result\",\"ok\":true,\"scripts\":")
            + qst::webview::JsonListScripts()
            + ",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
        return;
    }
    if (type == "deleteLibraryFolder") {
        std::string kind, folder, err;
        JsonGetString(json, "kind", kind);
        JsonGetString(json, "folder", folder);
        if (!qst::webview::DeleteLibraryFolder(kind, folder, err)) {
            PostToJs(std::string("{\"type\":\"deleteLibraryFolder.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        qst::engine::ReloadScriptsAndHotkeys();
        PostToJs(std::string("{\"type\":\"deleteLibraryFolder.result\",\"ok\":true,\"scripts\":")
            + qst::webview::JsonListScripts()
            + ",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
        return;
    }
    if (type == "moveScriptToFolder") {
        std::string path, folder, err, newPath;
        JsonGetString(json, "path", path);
        JsonGetString(json, "folder", folder);
        if (!qst::webview::MoveScriptToFolder(path, folder, newPath, err)) {
            PostToJs(std::string("{\"type\":\"moveScriptToFolder.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        qst::engine::ReloadScriptsAndHotkeys();
        PostToJs(std::string("{\"type\":\"moveScriptToFolder.result\",\"ok\":true,\"path\":\"")
            + EscapeJsonUtf8(newPath)
            + "\",\"scripts\":" + qst::webview::JsonListScripts()
            + ",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
        return;
    }
    if (type == "setItemLibraryFolder") {
        std::string kind, id, folder, err;
        JsonGetString(json, "kind", kind);
        JsonGetString(json, "id", id);
        JsonGetString(json, "folder", folder);
        if (!qst::webview::SetItemLibraryFolder(kind, id, folder, err)) {
            PostToJs(std::string("{\"type\":\"setItemLibraryFolder.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        PostToJs("{\"type\":\"setItemLibraryFolder.result\",\"ok\":true}");
        return;
    }
    if (type == "window.modeReady" || type == "modeReady" || type == "window.uncloak") {
        // 前端列表/参数已画完：揭开 DWM cloak
        SetShellCloaked(false);
        PostToJs("{\"type\":\"window.modeReady.result\",\"ok\":true}");
        return;
    }

    if (type == "listScripts") {
        PostToJs(std::string("{\"type\":\"listScripts.result\",\"ok\":true,\"scripts\":")
            + qst::webview::JsonListScripts() + "}");
        return;
    }
    if (type == "listRecordings") {
        PostToJs(std::string("{\"type\":\"listRecordings.result\",\"ok\":true,\"recordings\":")
            + qst::webview::JsonListRecordings() + "}");
        return;
    }
    if (type == "listAgentConversations") {
        PostToJs(std::string("{\"type\":\"listAgentConversations.result\",\"ok\":true,\"conversations\":")
            + qst::webview::JsonListAgentConversations() + "}");
        return;
    }
    if (type == "runScript") {
        std::string id;
        JsonGetString(json, "id", id);
        std::wstring path;
        std::string err;
        if (!qst::webview::ResolveScriptPath(id, path, err)) {
            PostToJs(std::string("{\"type\":\"runScript.result\",\"ok\":false,\"detail\":\"")
                + err + "\"}");
            return;
        }
        if (!qst::engine::RunScriptPath(path, err)) {
            PostToJs(std::string("{\"type\":\"runScript.result\",\"ok\":false,\"detail\":\"")
                + err + "\"}");
            return;
        }
        EnsureTrayIcon();
        PostToJs("{\"type\":\"runScript.result\",\"ok\":true}");
        PushEngineStatusIfChanged(true, false);
        return;
    }
    if (type == "stopScript") {
        qst::engine::StopScript();
        EnsureTrayIcon();
        PostToJs("{\"type\":\"stopScript.result\",\"ok\":true}");
        PushEngineStatusIfChanged(true, false);
        return;
    }
    if (type == "debugScript") {
        std::string err;
        std::vector<ScriptAction> actions;
        int startIndex = 0;
        bool stepMode = false;
        std::vector<int> breakpoints;
        Hotkey debugHotkey{};
        std::wstring displayName;
        windowmode::WindowModeScriptConfig wmCfg{};
        if (!qst::webview::ParseDebugScriptJson(json, actions, startIndex, stepMode,
                breakpoints, debugHotkey, displayName, wmCfg, err)) {
            PostToJs(std::string("{\"type\":\"debugScript.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        std::string engineErr;
        if (!qst::engine::DebugRunActions(actions, startIndex, stepMode, breakpoints,
                debugHotkey, displayName, wmCfg, engineErr)) {
            PostToJs(std::string("{\"type\":\"debugScript.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(engineErr) + "\"}");
            return;
        }
        EnsureTrayIcon();
        PostToJs("{\"type\":\"debugScript.result\",\"ok\":true}");
        PushEngineStatusIfChanged(true, false);
        return;
    }
    if (type == "startClicker") {
        std::string err;
        // Persist interval/button only — never start webview ClickerLoop/SendInput.
        if (!qst::webview::StartClickerFromOpts(json, err)) {
            PostToJs(std::string("{\"type\":\"startClicker.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        qst::engine::ReloadSettings();
        qst::engine::ApplyClickerSettings(qst::webview::Ctx().clicker);
        qst::engine::StartClicker();
        EnsureTrayIcon();
        PushEngineStatusIfChanged(true, false);
        PostToJs(std::string("{\"type\":\"startClicker.result\",\"ok\":true,\"status\":{")
            + "\"running\":" + (qst::engine::IsClicking() ? "true" : "false") + "}}");
        return;
    }
    if (type == "stopClicker") {
        qst::engine::StopClicker();
        qst::webview::StopClicker();
        EnsureTrayIcon();
        PushEngineStatusIfChanged(true, false);
        PostToJs(std::string("{\"type\":\"stopClicker.result\",\"ok\":true,\"status\":{")
            + "\"running\":" + (qst::engine::IsClicking() ? "true" : "false") + "}}");
        return;
    }
    if (type == "getClickerStatus") {
        std::string st = qst::webview::JsonClickerStatus();
        if (qst::engine::IsClicking()) {
            // force running true in a small status object
            PostToJs("{\"type\":\"getClickerStatus.result\",\"ok\":true,\"status\":{"
                "\"running\":true}}");
        } else {
            PostToJs(std::string("{\"type\":\"getClickerStatus.result\",\"ok\":true,\"status\":")
                + st + "}");
        }
        return;
    }
    if (type == "openSettingsData") {
        PostToJs(std::string("{\"type\":\"openSettingsData.result\",\"ok\":true,\"settings\":")
            + qst::webview::JsonOpenSettings() + "}");
        return;
    }
    if (type == "saveSettings") {
        std::string payload = json;
        const auto pos = json.find("\"settings\"");
        if (pos != std::string::npos) {
            const auto brace = json.find('{', pos);
            if (brace != std::string::npos) payload = json.substr(brace);
        }
        std::string err;
        if (!qst::webview::ApplySaveSettingsJson(payload, err)) {
            PostToJs(std::string("{\"type\":\"saveSettings.result\",\"ok\":false,\"detail\":\"")
                + err + "\"}");
            return;
        }
        qst::engine::ReloadSettings();
        PostToJs("{\"type\":\"saveSettings.result\",\"ok\":true}");
        // 主壳保存后推一份最新设置到独立助手窗，避免仍提示「未配置 API 密钥」
        if (g_agentWebview) {
            PostToAgentJs(std::string("{\"type\":\"openSettingsData.result\",\"ok\":true,\"settings\":")
                + qst::webview::JsonOpenSettings() + "}");
        }
        return;
    }
    if (type == "restoreSettingsDefaults") {
        std::string err;
        if (!qst::webview::RestoreSettingsDefaults(err)) {
            PostToJs(std::string("{\"type\":\"restoreSettingsDefaults.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        qst::engine::ReloadSettings();
        PostToJs(std::string("{\"type\":\"restoreSettingsDefaults.result\",\"ok\":true,\"settings\":")
            + qst::webview::JsonOpenSettings() + "}");
        return;
    }
    if (type == "openEditor") {
        std::string pathUtf8;
        JsonGetString(json, "path", pathUtf8);
        std::wstring wpath;
        bool isNew = pathUtf8.empty();
        if (!isNew) {
            std::string err;
            if (!qst::webview::ResolveScriptPath(pathUtf8, wpath, err)) {
                PostToJs(std::string("{\"type\":\"openEditor.result\",\"ok\":false,\"detail\":\"")
                    + err + "\"}");
                return;
            }
        }
        // 不在此 RequestUiMode：等前端写完动作列表后再 setMode，避免主页被拉大露出空白底
        const std::string script = qst::webview::JsonLoadScriptEditor(wpath, isNew);
        PostToJs(std::string("{\"type\":\"openEditor.result\",\"ok\":true,\"script\":")
            + script + "}");
        return;
    }
    if (type == "peekScriptActions") {
        std::string pathUtf8, reqId, err;
        JsonGetString(json, "path", pathUtf8);
        JsonGetString(json, "reqId", reqId);
        std::wstring wpath;
        if (!qst::webview::ResolveScriptPath(pathUtf8, wpath, err)) {
            PostToJs(std::string("{\"type\":\"peekScriptActions.result\",\"ok\":false,\"reqId\":\"")
                + EscapeJsonUtf8(reqId) + "\",\"detail\":\"" + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        const std::string actions = qst::webview::JsonPeekScriptActions(wpath);
        PostToJs(std::string("{\"type\":\"peekScriptActions.result\",\"ok\":true,\"reqId\":\"")
            + EscapeJsonUtf8(reqId) + "\",\"actions\":" + actions + "}");
        return;
    }
    if (type == "previewScriptActions") {
        std::string pathUtf8, reqId, err;
        JsonGetString(json, "path", pathUtf8);
        JsonGetString(json, "reqId", reqId);
        int maxNames = 16;
        JsonGetInt(json, "max", maxNames);
        std::wstring wpath;
        if (!qst::webview::ResolveScriptPath(pathUtf8, wpath, err)) {
            PostToJs(std::string("{\"type\":\"previewScriptActions.result\",\"ok\":false,\"reqId\":\"")
                + EscapeJsonUtf8(reqId) + "\",\"detail\":\"" + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        const std::string preview = qst::webview::JsonPreviewScriptActions(wpath, maxNames);
        if (preview.size() < 2 || preview.front() != '{') {
            PostToJs(std::string("{\"type\":\"previewScriptActions.result\",\"ok\":true,\"reqId\":\"")
                + EscapeJsonUtf8(reqId) + "\",\"actionCount\":0,\"names\":[]}");
            return;
        }
        PostToJs(std::string("{\"type\":\"previewScriptActions.result\",\"ok\":true,\"reqId\":\"")
            + EscapeJsonUtf8(reqId) + "\"," + preview.substr(1));
        return;
    }
    if (type == "saveEditor") {
        std::string outPath, err;
        if (!qst::webview::SaveEditorFromJson(json, outPath, err)) {
            PostToJs(std::string("{\"type\":\"saveEditor.result\",\"ok\":false,\"detail\":\"")
                + err + "\"}");
            return;
        }
        qst::engine::ReloadScriptsAndHotkeys();
        PostToJs(std::string("{\"type\":\"saveEditor.result\",\"ok\":true,\"path\":\"")
            + EscapeJsonUtf8(outPath) + "\",\"scripts\":" + qst::webview::JsonListScripts() + "}");
        return;
    }
    if (type == "deleteScript") {
        std::string pathUtf8, err;
        JsonGetString(json, "path", pathUtf8);
        const bool deleted = qst::webview::DeleteScriptFile(pathUtf8, err);
        // 无论成败都刷新列表：文件已不存在时按成功处理，幽灵条目随之消失
        qst::engine::ReloadScriptsAndHotkeys();
        if (!deleted) {
            PostToJs(std::string("{\"type\":\"deleteScript.result\",\"ok\":false,\"detail\":\"")
                + err + "\",\"scripts\":" + qst::webview::JsonListScripts()
                + ",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
            return;
        }
        PostToJs(std::string("{\"type\":\"deleteScript.result\",\"ok\":true,\"scripts\":")
            + qst::webview::JsonListScripts()
            + ",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
        return;
    }
    if (type == "renameScript") {
        std::string pathUtf8, nameUtf8, err, newPath;
        JsonGetString(json, "path", pathUtf8);
        JsonGetString(json, "name", nameUtf8);
        if (!qst::webview::RenameScriptFile(pathUtf8, nameUtf8, newPath, err)) {
            PostToJs(std::string("{\"type\":\"renameScript.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        qst::engine::ReloadScriptsAndHotkeys();
        PostToJs(std::string("{\"type\":\"renameScript.result\",\"ok\":true,\"path\":\"")
            + EscapeJsonUtf8(newPath)
            + "\",\"scripts\":" + qst::webview::JsonListScripts()
            + ",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
        return;
    }
    if (type == "importScript") {
        std::string kind;
        JsonGetString(json, "kind", kind);
        std::wstring outPath;
        std::string err;
        if (!qst::webview::ImportScriptFile(kind == "recording" || kind == "rec", outPath, err)) {
            if (err == "cancelled") {
                PostToJs("{\"type\":\"importScript.result\",\"ok\":false,\"detail\":\"cancelled\"}");
            } else {
                PostToJs(std::string("{\"type\":\"importScript.result\",\"ok\":false,\"detail\":\"")
                    + err + "\"}");
            }
            return;
        }
        qst::engine::ReloadScriptsAndHotkeys();
        PostToJs(std::string("{\"type\":\"importScript.result\",\"ok\":true,\"path\":\"")
            + EscapeJsonUtf8(ToUtf8(outPath))
            + "\",\"scripts\":" + qst::webview::JsonListScripts()
            + ",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
        return;
    }
    if (type == "exportScript") {
        std::string pathUtf8, err, skippedFilesJson;
        int skipped = 0;
        JsonGetString(json, "path", pathUtf8);
        if (!qst::webview::ExportScriptFile(pathUtf8, err, &skipped, &skippedFilesJson)) {
            if (err == "cancelled") {
                PostToJs("{\"type\":\"exportScript.result\",\"ok\":false,\"detail\":\"cancelled\"}");
            } else {
                PostToJs(std::string("{\"type\":\"exportScript.result\",\"ok\":false,\"detail\":\"")
                    + EscapeJsonUtf8(err) + "\"}");
            }
            return;
        }
        std::ostringstream oss;
        oss << "{\"type\":\"exportScript.result\",\"ok\":true,\"skipped\":" << skipped;
        if (!skippedFilesJson.empty()) oss << ",\"skippedFiles\":" << skippedFilesJson;
        oss << "}";
        PostToJs(oss.str());
        return;
    }
    if (type == "openRecordingOptimize") {
        std::string pathUtf8, nameUtf8;
        JsonGetString(json, "path", pathUtf8);
        JsonGetString(json, "name", nameUtf8);
        int forceNative = 0;
        JsonGetInt(json, "forceNative", forceNative);
        if (forceNative) {
            PostToJs("{\"type\":\"openRecordingOptimize.result\",\"ok\":false,"
                     "\"detail\":\"legacy_unavailable\",\"code\":\"legacy_unavailable\"}");
            return;
        }
        PostToJs(std::string("{\"type\":\"openRecordingOptimize.result\",\"ok\":true,\"useHtml\":true,\"path\":\"")
            + EscapeJsonUtf8(pathUtf8) + "\",\"name\":\"" + EscapeJsonUtf8(nameUtf8) + "\"}");
        return;
    }
    if (type == "setScriptHotkey") {
        std::string pathUtf8, textUtf8, err;
        JsonGetString(json, "path", pathUtf8);
        JsonGetString(json, "hotkeyText", textUtf8);
        int vk = 0, modifiers = 0, hold = 0;
        JsonGetInt(json, "hotkeyVk", vk);
        JsonGetInt(json, "hotkeyModifiers", modifiers);
        JsonGetInt(json, "hotkeyHold", hold);
        if (vk != 0) {
            std::wstring conflict;
            if (qst::engine::HotkeyChordConflicts(static_cast<UINT>(vk), static_cast<UINT>(modifiers),
                    FromUtf8(pathUtf8), false, conflict)) {
                PostToJs(std::string("{\"type\":\"setScriptHotkey.result\",\"ok\":false,\"detail\":\"")
                    + EscapeJsonUtf8(ToUtf8(conflict)) + "\"}");
                return;
            }
        }
        if (!qst::webview::SetScriptHotkeyJson(pathUtf8, textUtf8,
                static_cast<UINT>(vk), static_cast<UINT>(modifiers), hold != 0, err)) {
            PostToJs(std::string("{\"type\":\"setScriptHotkey.result\",\"ok\":false,\"detail\":\"")
                + err + "\"}");
            return;
        }
        qst::engine::ReloadScriptsAndHotkeys();
        PostToJs(std::string("{\"type\":\"setScriptHotkey.result\",\"ok\":true,\"scripts\":")
            + qst::webview::JsonListScripts()
            + ",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
        return;
    }
    if (type == "setRecorderMode") {
        int mode = 0;
        JsonGetInt(json, "mode", mode);
        std::string err;
        if (!qst::webview::SetRecorderInputMode(mode, err)) {
            PostToJs(std::string("{\"type\":\"setRecorderMode.result\",\"ok\":false,\"detail\":\"")
                + err + "\"}");
            return;
        }
        qst::engine::ReloadSettings();
        PostToJs(std::string("{\"type\":\"setRecorderMode.result\",\"ok\":true,\"mode\":")
            + std::to_string(mode) + "}");
        return;
    }
    if (type == "openAgentConversation") {
        std::string id;
        JsonGetString(json, "id", id);
        int createNew = 0;
        JsonGetInt(json, "createNew", createNew);
        int peek = 0;
        JsonGetInt(json, "peek", peek);
        std::string panelKey;
        JsonGetString(json, "panelKey", panelKey);
        const std::string body = qst::webview::JsonOpenAgentConversation(
            id, createNew != 0 || id.empty(), peek != 0);
        if (body.find("\"error\"") != std::string::npos || body.find("not found") != std::string::npos
            || body.find("助手忙碌中") != std::string::npos) {
            std::string detail = "not found";
            if (body.find("助手忙碌中") != std::string::npos) detail = "助手忙碌中";
            PostToJs(std::string("{\"type\":\"openAgentConversation.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(detail) + "\""
                + (panelKey.empty() ? "" : (",\"panelKey\":\"" + EscapeJsonUtf8(panelKey) + "\""))
                + "}");
            return;
        }
        PostToJs(std::string("{\"type\":\"openAgentConversation.result\",\"ok\":true")
            + (panelKey.empty() ? "" : (",\"panelKey\":\"" + EscapeJsonUtf8(panelKey) + "\""))
            + ",\"conversation\":" + body + "}");
        return;
    }
    if (type == "sendAgentMessage") {
        std::string err;
        std::string panelKey;
        JsonGetString(json, "panelKey", panelKey);
        if (!qst::webview::BeginSendAgentMessage(json, err)) {
            PostToJs(std::string("{\"type\":\"sendAgentMessage.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\""
                + (panelKey.empty() ? "" : (",\"panelKey\":\"" + EscapeJsonUtf8(panelKey) + "\""))
                + "}");
        }
        // async success arrives via PostToWebUi
        return;
    }
    if (type == "deleteAgentConversation") {
        std::string id, err;
        JsonGetString(json, "id", id);
        if (!qst::webview::DeleteAgentConversationById(id, err)) {
            PostToJs(std::string("{\"type\":\"deleteAgentConversation.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        PostToJs(std::string("{\"type\":\"deleteAgentConversation.result\",\"ok\":true,\"conversations\":")
            + qst::webview::JsonListAgentConversations() + "}");
        return;
    }
    if (type == "listAgentChanges") {
        PostToJs(std::string("{\"type\":\"listAgentChanges.result\",\"ok\":true,\"changes\":")
            + qst::webview::JsonListAgentChanges() + "}");
        return;
    }
    if (type == "revertAgentChange") {
        std::string id, err;
        JsonGetString(json, "id", id);
        if (!qst::webview::RevertAgentChangeById(id, err)) {
            PostToJs(std::string("{\"type\":\"revertAgentChange.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        PostToJs(std::string("{\"type\":\"revertAgentChange.result\",\"ok\":true,\"changes\":")
            + qst::webview::JsonListAgentChanges()
            + ",\"scripts\":" + qst::webview::JsonListScripts()
            + ",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
        return;
    }
    if (type == "agentSaveDraft") {
        std::string err;
        if (!qst::webview::SaveAgentDraft(json, err)) {
            PostToJs(std::string("{\"type\":\"agentSaveDraft.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        PostToJs("{\"type\":\"agentSaveDraft.result\",\"ok\":true}");
        return;
    }
    if (type == "getGlobalHotkey") {
        const Hotkey hk = qst::engine::GetGlobalHotkey();
        PostToJs(std::string("{\"type\":\"getGlobalHotkey.result\",\"ok\":true,\"hotkeyText\":\"")
            + EscapeJsonUtf8(ToUtf8(hk.text.empty() ? L"F8" : hk.text))
            + "\",\"hotkeyVk\":" + std::to_string(hk.vk)
            + ",\"hotkeyModifiers\":" + std::to_string(hk.modifiers)
            + ",\"hotkeyHold\":" + (hk.holdMode ? "true" : "false") + "}");
        return;
    }
    if (type == "setGlobalHotkey") {
        Hotkey hk{};
        std::string text;
        JsonGetString(json, "hotkeyText", text);
        int vk = 0, mods = 0, hold = 0;
        JsonGetInt(json, "hotkeyVk", vk);
        JsonGetInt(json, "hotkeyModifiers", mods);
        JsonGetInt(json, "hotkeyHold", hold);
        hk.text = FromUtf8(text);
        hk.vk = static_cast<UINT>(vk);
        hk.modifiers = static_cast<UINT>(mods);
        hk.holdMode = hold != 0;
        hk.enabled = hk.vk != 0;
        if (hk.enabled) {
            std::wstring conflict;
            if (qst::engine::HotkeyChordConflicts(hk.vk, hk.modifiers, L"", true, conflict)) {
                PostToJs(std::string("{\"type\":\"setGlobalHotkey.result\",\"ok\":false,\"detail\":\"")
                    + EscapeJsonUtf8(ToUtf8(conflict)) + "\"}");
                return;
            }
        }
        qst::engine::SetGlobalHotkey(hk);
        PostToJs(std::string("{\"type\":\"setGlobalHotkey.result\",\"ok\":true,\"hotkeyText\":\"")
            + EscapeJsonUtf8(ToUtf8(hk.text))
            + "\",\"hotkeyVk\":" + std::to_string(hk.vk)
            + ",\"hotkeyModifiers\":" + std::to_string(hk.modifiers)
            + ",\"hotkeyHold\":" + (hk.holdMode ? "true" : "false") + "}");
        return;
    }
    if (type == "startRecord") {
        int recWm = -1;
        JsonGetInt(json, "recorderWindowMode", recWm);
        std::string err;
        if (!qst::engine::StartRecordingEx(err, recWm)) {
            PostToJs(std::string("{\"type\":\"startRecord.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        EnsureTrayIcon();
        PushEngineStatusIfChanged(true, false);
        PostToJs("{\"type\":\"startRecord.result\",\"ok\":true}");
        return;
    }
    if (type == "stopRecord") {
        std::string path, err;
        int actionCount = 0;
        const bool ok = qst::engine::StopRecordingEx(path, actionCount, err);
        EnsureTrayIcon();
        PushEngineStatusIfChanged(true, false);
        if (!ok) {
            PostToJs(std::string("{\"type\":\"stopRecord.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err.empty() ? "stop recording failed" : err)
                + "\",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
            return;
        }
        qst::engine::ReloadScriptsAndHotkeys();
        PostToJs(std::string("{\"type\":\"stopRecord.result\",\"ok\":true,\"actionCount\":")
            + std::to_string(actionCount)
            + ",\"path\":\"" + EscapeJsonUtf8(path)
            + "\",\"recordings\":" + qst::webview::JsonListRecordings() + "}");
        return;
    }
    if (type == "setActiveHomeTab") {
        int tab = 0;
        JsonGetInt(json, "tab", tab);
        qst::engine::SetActiveHomeTab(tab);
        // 主页 Tab 切换时武装热键：修复调试结束后误静音 / RegisterHotKey 丢失后 F8、录制热键全哑
        if (g_mode == UiMode::Home) {
            qst::engine::EnsureHotkeysArmed();
        }
        PostToJs("{\"type\":\"setActiveHomeTab.result\",\"ok\":true}");
        return;
    }
    if (type == "setHomeSelection") {
        int tab = 0;
        std::string path;
        JsonGetInt(json, "tab", tab);
        JsonGetString(json, "path", path);
        qst::engine::SelectHomeItem(tab, FromUtf8(path));
        PostToJs("{\"type\":\"setHomeSelection.result\",\"ok\":true}");
        return;
    }
    if (type == "getHomeState") {
        PostToJs(std::string("{\"type\":\"getHomeState.result\",\"ok\":true,")
            + qst::engine::GetHomeStateJson().substr(1)); // strip leading '{'
        return;
    }
    if (type == "cancelAgentMessage") {
        std::string err;
        if (!qst::webview::CancelAgentMessage(err)) {
            PostToJs(std::string("{\"type\":\"cancelAgentMessage.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err.empty() ? "not busy" : err) + "\"}");
            return;
        }
        PostToJs("{\"type\":\"cancelAgentMessage.result\",\"ok\":true}");
        return;
    }
    if (type == "pasteAgentClipboard") {
        std::string pathsJson, err;
        if (!qst::webview::PasteAgentClipboardAttachments(g_hwnd, pathsJson, err)) {
            PostToJs(std::string("{\"type\":\"pasteAgentClipboard.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        PostToJs(std::string("{\"type\":\"pasteAgentClipboard.result\",\"ok\":true,\"paths\":")
            + pathsJson + "}");
        return;
    }
    if (type == "saveClipboardImage") {
        std::string dataUrl, ext, outPath, err;
        JsonGetString(json, "dataUrl", dataUrl);
        JsonGetString(json, "ext", ext);
        if (!qst::webview::SaveClipboardImageDataUrl(dataUrl, ext, outPath, err)) {
            PostToJs(std::string("{\"type\":\"saveClipboardImage.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        PostToJs(std::string("{\"type\":\"saveClipboardImage.result\",\"ok\":true,\"path\":\"")
            + EscapeJsonUtf8(outPath) + "\"}");
        return;
    }
    if (type == "saveImageAs") {
        std::string path, dataUrl, err;
        JsonGetString(json, "path", path);
        JsonGetString(json, "dataUrl", dataUrl);
        HWND owner = (g_handlingAgentMsg && g_agentHwnd && IsWindow(g_agentHwnd))
            ? g_agentHwnd
            : g_hwnd;
        if (!qst::webview::SaveAgentImageAsDialog(owner, path, dataUrl, err)) {
            const bool cancelled = (err == "cancelled");
            PostToJs(std::string("{\"type\":\"saveImageAs.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(cancelled ? "cancelled" : err) + "\"}");
            return;
        }
        PostToJs("{\"type\":\"saveImageAs.result\",\"ok\":true}");
        return;
    }
    if (type == "openScheduledTasks") {
        int forceNative = 0;
        JsonGetInt(json, "forceNative", forceNative);
        if (forceNative) {
            PostToJs("{\"type\":\"openScheduledTasks.result\",\"ok\":false,"
                     "\"detail\":\"legacy_unavailable\",\"code\":\"legacy_unavailable\"}");
            return;
        }
        PostToJs("{\"type\":\"openScheduledTasks.result\",\"ok\":true,\"useHtml\":true}");
        return;
    }
    if (type == "listScheduledTasks") {
        const std::string body = qst::webview::JsonListScheduledTasks();
        PostToJs(std::string("{\"type\":\"listScheduledTasks.result\",\"ok\":true,\"data\":")
            + body + "}");
        return;
    }
    if (type == "saveScheduledTask") {
        int isUpdate = 0;
        int statusOnly = 0;
        JsonGetInt(json, "update", isUpdate);
        JsonGetInt(json, "statusOnly", statusOnly);
        std::string err;
        if (!qst::webview::SaveScheduledTaskFromJson(json, isUpdate != 0, err)) {
            PostToJs(std::string("{\"type\":\"saveScheduledTask.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        qst::engine::ReloadScheduledTasks();
        if (isUpdate) {
            std::string id;
            JsonGetString(json, "id", id);
            if (!id.empty()) qst::engine::TouchScheduledIntervalClock(FromUtf8(id));
        }
        PostToJs(std::string("{\"type\":\"saveScheduledTask.result\",\"ok\":true,\"statusOnly\":")
            + std::string(statusOnly != 0 ? "true" : "false")
            + ",\"data\":" + qst::webview::JsonListScheduledTasks() + "}");
        return;
    }
    if (type == "deleteScheduledTask") {
        std::string id, err;
        JsonGetString(json, "id", id);
        if (!qst::webview::DeleteScheduledTaskById(id, err)) {
            PostToJs(std::string("{\"type\":\"deleteScheduledTask.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        qst::engine::ReloadScheduledTasks();
        PostToJs(std::string("{\"type\":\"deleteScheduledTask.result\",\"ok\":true,\"data\":")
            + qst::webview::JsonListScheduledTasks() + "}");
        return;
    }
    if (type == "setScheduledTasksGlobalDisabled") {
        int disabled = 0;
        JsonGetInt(json, "disabled", disabled);
        std::string err;
        if (!qst::webview::SetScheduledTasksGlobalDisabled(disabled != 0, err)) {
            PostToJs(std::string("{\"type\":\"setScheduledTasksGlobalDisabled.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        qst::engine::ReloadScheduledTasks();
        PostToJs(std::string("{\"type\":\"setScheduledTasksGlobalDisabled.result\",\"ok\":true,\"data\":")
            + qst::webview::JsonListScheduledTasks() + "}");
        return;
    }
    if (type == "loadOptimizeRecording") {
        std::string path, err;
        JsonGetString(json, "path", path);
        const std::string body = qst::webview::JsonLoadOptimizeRecording(path, err);
        if (!err.empty()) {
            PostToJs(std::string("{\"type\":\"loadOptimizeRecording.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        PostToJs(std::string("{\"type\":\"loadOptimizeRecording.result\",\"ok\":true,\"recording\":")
            + body + "}");
        return;
    }
    if (type == "applyOptimizeRecording") {
        std::string err, extra;
        if (!qst::webview::ApplyOptimizeAndSave(json, err, extra)) {
            PostToJs(std::string("{\"type\":\"applyOptimizeRecording.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        // 对齐原生 LoadRecordings：写回后刷新热键表
        qst::engine::ReloadScriptsAndHotkeys();
        std::string body = "{\"type\":\"applyOptimizeRecording.result\",\"ok\":true";
        if (!extra.empty()) body += "," + extra;
        body += ",\"recordings\":" + qst::webview::JsonListRecordings() + "}";
        PostToJs(body);
        return;
    }
    if (type == "pickScreenRegion") {
        const auto r = qst::desktop_tools::PickScreenRegion(g_hwnd, L"选取区域");
        if (!r.ok) {
            PostToJs("{\"type\":\"pickScreenRegion.result\",\"ok\":false,\"detail\":\"cancelled\"}");
            return;
        }
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"pickScreenRegion.result\",\"ok\":true,\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}",
            r.x1, r.y1, r.x2, r.y2);
        PostToJs(buf);
        return;
    }
    if (type == "getEngineStatus") {
        PushEngineStatusIfChanged(true);
        return;
    }
    if (type == "themeCatalog") {
        PostToJs(std::string("{\"type\":\"themeCatalog.result\",\"ok\":true,\"themes\":")
            + qst::webview::JsonThemeCatalog() + "}");
        return;
    }
    if (type == "applyTheme") {
        int themeId = 0;
        int useCustom = 0;
        int main = 0, accent = 0;
        JsonGetInt(json, "themeId", themeId);
        JsonGetInt(json, "useCustomTheme", useCustom);
        JsonGetInt(json, "customMainColor", main);
        JsonGetInt(json, "customAccentColor", accent);
        std::string err;
        if (!qst::webview::ApplyThemeSettings(themeId, useCustom != 0,
                static_cast<COLORREF>(main), static_cast<COLORREF>(accent), err)) {
            PostToJs(std::string("{\"type\":\"applyTheme.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        PostToJs(std::string("{\"type\":\"applyTheme.result\",\"ok\":true,\"settings\":")
            + qst::webview::JsonOpenSettings() + "}");
        return;
    }
    if (type == "openThemeCustom") {
        // 已迁移：用 Web #ov-theme（HTML color），不再弹原生 ThemeCustomDialog
        PostToJs("{\"type\":\"openThemeCustom.useWeb\",\"ok\":true}");
        return;
    }
    if (type == "installDriver") {
        std::string kind, err;
        JsonGetString(json, "kind", kind);
        int uninstall = 0;
        JsonGetInt(json, "uninstall", uninstall);
        const auto inst = qst::desktop_tools::InstallDriver(g_hwnd, kind,
            [&](int percent, int step, const char* status) {
                char buf[320];
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"installDriver.progress\",\"ok\":true,\"kind\":\"%s\",\"percent\":%d,\"step\":%d,\"status\":\"%s\"}",
                    EscapeJsonUtf8(kind).c_str(), percent, step, EscapeJsonUtf8(status).c_str());
                PostToJs(buf);
            }, uninstall != 0);
        if (inst.rebootRequired) {
        PostToJs(std::string("{\"type\":\"installDriver.result\",\"ok\":false,\"rebootRequired\":true,\"fwReboot\":false,\"kind\":\"")
            + EscapeJsonUtf8(kind) + "\",\"detail\":\"" + EscapeJsonUtf8(inst.detail) + "\"}");
            return;
        }
        if (!inst.ok) {
            PostToJs(std::string("{\"type\":\"installDriver.result\",\"ok\":false,\"kind\":\"")
                + EscapeJsonUtf8(kind) + "\",\"uninstalled\":"
                + (inst.uninstalled ? "true" : "false")
                + ",\"detail\":\"" + EscapeJsonUtf8(inst.detail) + "\"}");
            return;
        }
        int suggestedBackend = 0;
        bool probed = false;
        if (!inst.uninstalled) {
            if (kind == "interception" || kind == "ic") {
                suggestedBackend = 1;
                probed = HidInterceptionBackend::Instance().ProbeAvailable(nullptr);
            } else if (kind == "vhid" || kind == "virtualHid") {
                suggestedBackend = 2;
                probed = VirtualHidBackend::Instance().ProbeAvailable(nullptr);
            }
        }
        PostToJs(std::string("{\"type\":\"installDriver.result\",\"ok\":true,\"kind\":\"")
            + EscapeJsonUtf8(kind)
            + "\",\"uninstalled\":" + (inst.uninstalled ? "true" : "false")
            + ",\"probed\":" + (probed ? "true" : "false")
            + ",\"suggestedBackend\":" + std::to_string(suggestedBackend)
            + ",\"hint\":\"" + (inst.uninstalled
                ? "已卸载并清除过滤驱动残留"
                : "请在设置中选择注入后端并保存") + "\"}");
        return;
    }
    if (type == "queryVhidStatus") {
        const auto st = qst::desktop_tools::QueryVhidInstallStatus();
        char buf[384];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"queryVhidStatus.result\",\"ok\":true,"
            "\"hvciEnabled\":%s,\"rebootPending\":%s,\"driverReady\":%s,"
            "\"installScriptPresent\":%s,\"pendingSb\":%s,\"lastExitCode\":%d}",
            st.hvciEnabled ? "true" : "false",
            st.rebootPending ? "true" : "false",
            st.driverReady ? "true" : "false",
            st.installScriptPresent ? "true" : "false",
            st.pendingSb ? "true" : "false",
            st.lastExitCode);
        PostToJs(buf);
        return;
    }
    if (type == "systemReboot") {
        // 禁止为装驱动改 BCD 或重启进固件。旧路径会把机器留在无法启动状态。
        PostToJs("{\"type\":\"systemReboot.result\",\"ok\":false,"
                 "\"detail\":\"已禁止一键重启进 BIOS 或为装驱动修改启动配置\"}");
        return;
    }
    if (type == "pickImageFile") {
        const auto r = qst::desktop_tools::PickImageFile(g_hwnd);
        if (!r.ok) {
            PostToJs("{\"type\":\"pickImageFile.result\",\"ok\":false,\"detail\":\"cancelled\"}");
            return;
        }
        PostToJs(std::string("{\"type\":\"pickImageFile.result\",\"ok\":true,\"path\":\"")
            + EscapeJsonUtf8(ToUtf8(r.path)) + "\"}");
        return;
    }
    if (type == "findImageCrop") {
        std::string pathUtf8;
        JsonGetString(json, "imagePath", pathUtf8);
        int ox = 0, oy = 0;
        JsonGetInt(json, "offsetX", ox);
        JsonGetInt(json, "offsetY", oy);
        int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
        const bool hasRect = JsonGetInt(json, "cropX", cropX)
            && JsonGetInt(json, "cropY", cropY)
            && JsonGetInt(json, "cropW", cropW)
            && JsonGetInt(json, "cropH", cropH)
            && cropW > 0 && cropH > 0;
        if (!hasRect) {
            PostToJs("{\"type\":\"findImageCrop.result\",\"ok\":false,"
                     "\"detail\":\"crop_rect_required\",\"code\":\"crop_rect_required\"}");
            return;
        }
        const auto r = qst::desktop_tools::FindImageCropRect(
            FromUtf8(pathUtf8), ox, oy, cropX, cropY, cropW, cropH);
        if (!r.ok) {
            PostToJs(std::string("{\"type\":\"findImageCrop.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(r.detail) + "\"}");
            return;
        }
        if (r.unchanged) {
            PostToJs("{\"type\":\"findImageCrop.result\",\"ok\":true,\"unchanged\":true}");
            return;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), ",\"offsetX\":%d,\"offsetY\":%d", r.offsetX, r.offsetY);
        PostToJs(std::string("{\"type\":\"findImageCrop.result\",\"ok\":true,\"imagePath\":\"")
            + EscapeJsonUtf8(ToUtf8(r.imagePath)) + "\"" + buf + "}");
        return;
    }
    if (type == "findImageMatch") {
        std::string pathUtf8, modeUtf8;
        JsonGetString(json, "imagePath", pathUtf8);
        JsonGetString(json, "mode", modeUtf8);
        qst::desktop_tools::FindImageMatchParams params;
        params.imagePath = FromUtf8(pathUtf8);
        params.modeUtf8 = modeUtf8;
        JsonGetInt(json, "searchX1", params.searchX1);
        JsonGetInt(json, "searchY1", params.searchY1);
        JsonGetInt(json, "searchX2", params.searchX2);
        JsonGetInt(json, "searchY2", params.searchY2);
        JsonGetInt(json, "searchFullScreen", params.searchFullScreen);
        JsonGetDouble(json, "matchThreshold", params.matchThreshold);
        JsonGetInt(json, "perfectMatch", params.perfectMatch);
        JsonGetDouble(json, "imageScaleMin", params.imageScaleMin);
        JsonGetDouble(json, "imageScaleMax", params.imageScaleMax);
        JsonGetInt(json, "syntheticW", params.syntheticW);
        JsonGetInt(json, "syntheticH", params.syntheticH);
        const auto r = qst::desktop_tools::FindImageMatch(g_hwnd, params);
        if (!r.ok) {
            PostToJs(std::string("{\"type\":\"findImageMatch.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(r.detail) + "\"}");
            return;
        }
        std::ostringstream oss;
        oss << "{\"type\":\"findImageMatch.result\",\"ok\":true,\"mode\":\"" << EscapeJsonUtf8(r.modeUtf8)
            << "\",\"resolvedPath\":\"" << EscapeJsonUtf8(ToUtf8(r.resolvedPath))
            << "\",\"offsetX\":" << r.offsetX << ",\"offsetY\":" << r.offsetY
            << ",\"regionValid\":" << (r.regionValid ? "true" : "false")
            << ",\"searchX1\":" << r.regionX1 << ",\"searchY1\":" << r.regionY1
            << ",\"searchX2\":" << r.regionX2 << ",\"searchY2\":" << r.regionY2
            << ",\"matchTopLeftX\":" << r.matchTopLeftX << ",\"matchTopLeftY\":" << r.matchTopLeftY
            << ",\"found\":" << (r.found ? "true" : "false")
            << ",\"matchCount\":" << r.matchCount
            << ",\"bestScore\":" << r.bestScore
            << "}";
        PostToJs(oss.str());
        return;
    }
    if (type == "captureTemplateScreenshot") {
        const auto r = qst::desktop_tools::CaptureTemplateScreenshot(g_hwnd, L"屏幕截图");
        if (!r.ok) {
            PostToJs(std::string("{\"type\":\"captureTemplateScreenshot.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(r.detail) + "\"}");
            return;
        }
        PostToJs(std::string("{\"type\":\"captureTemplateScreenshot.result\",\"ok\":true,\"imagePath\":\"")
            + EscapeJsonUtf8(ToUtf8(r.imagePath))
            + "\",\"resolvedPath\":\"" + EscapeJsonUtf8(ToUtf8(r.resolvedPath)) + "\"}");
        return;
    }
    if (type == "resolveImagePath") {
        std::string pathUtf8;
        JsonGetString(json, "path", pathUtf8);
        const std::wstring resolved = ResolveImagePath(FromUtf8(pathUtf8));
        const bool ok = !resolved.empty() && IsPathInImageDir(resolved)
            && GetFileAttributesW(resolved.c_str()) != INVALID_FILE_ATTRIBUTES;
        PostToJs(std::string("{\"type\":\"resolveImagePath.result\",\"ok\":")
            + (ok ? "true" : "false") + ",\"path\":\"" + EscapeJsonUtf8(pathUtf8)
            + "\",\"resolvedPath\":\"" + EscapeJsonUtf8(ToUtf8(ok ? resolved : L"")) + "\"}");
        return;
    }
    if (type == "readImageDataUrl") {
        std::string pathUtf8;
        JsonGetString(json, "path", pathUtf8);
        std::string reqId;
        JsonGetString(json, "reqId", reqId);
        const std::wstring resolved = ResolveImagePath(FromUtf8(pathUtf8));
        if (resolved.empty() || !IsPathInImageDir(resolved)
            || GetFileAttributesW(resolved.c_str()) == INVALID_FILE_ATTRIBUTES) {
            PostToJs(std::string("{\"type\":\"readImageDataUrl.result\",\"ok\":false,\"reqId\":\"")
                + EscapeJsonUtf8(reqId) + "\",\"detail\":\"找不到路径: "
                + EscapeJsonUtf8(ToUtf8(resolved.empty() ? FromUtf8(pathUtf8) : resolved)) + "\"}");
            return;
        }
        // 优先用虚拟主机 URL，避免大 BMP base64 撑爆 PostWebMessage
        const std::wstring appDir = AppDir();
        std::wstring absResolved = resolved;
        wchar_t fullBuf[MAX_PATH]{};
        if (GetFullPathNameW(resolved.c_str(), MAX_PATH, fullBuf, nullptr)) {
            absResolved = fullBuf;
        }
        if (absResolved.size() >= appDir.size()
            && _wcsnicmp(absResolved.c_str(), appDir.c_str(), appDir.size()) == 0) {
            std::wstring rel = absResolved.substr(appDir.size());
            while (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/')) rel.erase(0, 1);
            for (auto& ch : rel) {
                if (ch == L'\\') ch = L'/';
            }
            PostToJs(std::string("{\"type\":\"readImageDataUrl.result\",\"ok\":true,\"reqId\":\"")
                + EscapeJsonUtf8(reqId) + "\",\"path\":\"" + EscapeJsonUtf8(pathUtf8)
                + "\",\"resolvedPath\":\"" + EscapeJsonUtf8(ToUtf8(absResolved))
                + "\",\"assetUrl\":\"https://qst.local/" + EscapeJsonUtf8(ToUtf8(rel)) + "\"}");
            return;
        }
        HANDLE h = CreateFileW(resolved.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            PostToJs(std::string("{\"type\":\"readImageDataUrl.result\",\"ok\":false,\"reqId\":\"")
                + EscapeJsonUtf8(reqId) + "\",\"detail\":\"无法读取图片\"}");
            return;
        }
        LARGE_INTEGER sz{};
        // base64 回退仅允许小图；大图应落在 AppDir 下走 assetUrl
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 256 * 1024) {
            CloseHandle(h);
            PostToJs(std::string("{\"type\":\"readImageDataUrl.result\",\"ok\":false,\"reqId\":\"")
                + EscapeJsonUtf8(reqId)
                + "\",\"detail\":\"图片不在应用目录且过大，无法预览\"}");
            return;
        }
        std::string bytes(static_cast<size_t>(sz.QuadPart), '\0');
        DWORD read = 0;
        const BOOL okRead = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
        CloseHandle(h);
        if (!okRead || read == 0) {
            PostToJs(std::string("{\"type\":\"readImageDataUrl.result\",\"ok\":false,\"reqId\":\"")
                + EscapeJsonUtf8(reqId) + "\",\"detail\":\"读取失败\"}");
            return;
        }
        bytes.resize(read);
        static const char* kB64 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        b64.reserve(((bytes.size() + 2) / 3) * 4);
        for (size_t i = 0; i < bytes.size(); i += 3) {
            const unsigned a = static_cast<unsigned char>(bytes[i]);
            const unsigned b = (i + 1 < bytes.size()) ? static_cast<unsigned char>(bytes[i + 1]) : 0;
            const unsigned c = (i + 2 < bytes.size()) ? static_cast<unsigned char>(bytes[i + 2]) : 0;
            const unsigned n = (a << 16) | (b << 8) | c;
            b64.push_back(kB64[(n >> 18) & 63]);
            b64.push_back(kB64[(n >> 12) & 63]);
            b64.push_back((i + 1 < bytes.size()) ? kB64[(n >> 6) & 63] : '=');
            b64.push_back((i + 2 < bytes.size()) ? kB64[n & 63] : '=');
        }
        std::string mime = "image/bmp";
        const auto dot = resolved.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            std::wstring ext = resolved.substr(dot + 1);
            for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
            if (ext == L"png") mime = "image/png";
            else if (ext == L"jpg" || ext == L"jpeg") mime = "image/jpeg";
            else if (ext == L"gif") mime = "image/gif";
            else if (ext == L"webp") mime = "image/webp";
        }
        PostToJs(std::string("{\"type\":\"readImageDataUrl.result\",\"ok\":true,\"reqId\":\"")
            + EscapeJsonUtf8(reqId) + "\",\"path\":\""
            + EscapeJsonUtf8(pathUtf8) + "\",\"resolvedPath\":\""
            + EscapeJsonUtf8(ToUtf8(resolved)) + "\",\"dataUrl\":\"data:" + mime
            + ";base64," + b64 + "\"}");
        return;
    }
    // Web #ov-hotkey / #ov-action-key：LL 回传按键，不弹原生 HotkeyCapture。
    if (type == "beginHotkeyCapture") {
        Hotkey editing{};
        std::string keyText;
        int keyVk = 0, mods = 0, hold = 0, passAll = 0, hotkeyUi = 0;
        JsonGetString(json, "hotkeyText", keyText);
        JsonGetInt(json, "hotkeyVk", keyVk);
        JsonGetInt(json, "hotkeyModifiers", mods);
        JsonGetInt(json, "hotkeyHold", hold);
        JsonGetInt(json, "passAll", passAll);
        JsonGetInt(json, "hotkeyUi", hotkeyUi);
        editing.text = FromUtf8(keyText);
        editing.vk = static_cast<UINT>(keyVk);
        editing.modifiers = static_cast<UINT>(mods);
        editing.holdMode = hold != 0;
        editing.enabled = editing.vk != 0;
        double holdSec = 0.2;
        {
            const auto& s = qst::webview::Ctx().settings;
            if (s.other.holdThresholdSeconds > 0) holdSec = s.other.holdThresholdSeconds;
        }
        if (hotkeyUi) {
            qst::engine::BeginHotkeyCaptureRelease(editing);
            if (!StartWebHotkeyLlHook(holdSec)) {
                qst::engine::EndHotkeyCaptureRelease();
                PostToJs("{\"type\":\"beginHotkeyCapture.result\",\"ok\":false,"
                         "\"detail\":\"无法安装热键捕获钩子\"}");
                return;
            }
        } else if (passAll) {
            qst::engine::BeginActionKeyCaptureRelease();
            StartWebActionKeyLlHook();
        } else {
            StopWebHotkeyLlHook();
            StopWebActionKeyLlHook();
            qst::engine::BeginHotkeyCaptureRelease(editing);
        }
        PostToJs(std::string("{\"type\":\"beginHotkeyCapture.result\",\"ok\":true,\"holdThresholdSeconds\":")
            + std::to_string(holdSec) + "}");
        return;
    }
    if (type == "endHotkeyCapture") {
        StopWebHotkeyLlHook();
        StopWebActionKeyLlHook();
        qst::engine::EndHotkeyCaptureRelease();
        // 捕获结束后巩固主页热键武装，避免注销后 RegisterHotKey 未恢复
        if (g_mode == UiMode::Home) {
            qst::engine::EnsureHotkeysArmed();
        }
        PostToJs("{\"type\":\"endHotkeyCapture.result\",\"ok\":true}");
        return;
    }
    if (type == "setTypingHotkeysMuted") {
        int muted = 0;
        JsonGetInt(json, "muted", muted);
        qst::engine::SetTypingHotkeysMuted(muted != 0, g_handlingAgentMsg ? 1 : 0);
        return;
    }
    // 已废弃：产品走 Web #ov-action-key / #ov-hotkey
    if (type == "captureActionKey" || type == "captureScriptHotkey"
        || type == "captureGlobalHotkey") {
        PostToJs(std::string("{\"type\":\"") + type + ".result\",\"ok\":false,\"detail\":"
                 "\"use web overlay\"}");
        return;
    }
    if (type == "formatHotkey") {
        int keyVk = 0, mods = 0, hold = 0;
        JsonGetInt(json, "hotkeyVk", keyVk);
        JsonGetInt(json, "hotkeyModifiers", mods);
        JsonGetInt(json, "hotkeyHold", hold);
        const bool holdMode = hold != 0;
        std::wstring text = HotkeyText(static_cast<UINT>(mods), static_cast<UINT>(keyVk), holdMode);
        PostToJs(std::string("{\"type\":\"formatHotkey.result\",\"ok\":true,\"hotkeyText\":\"")
            + EscapeJsonUtf8(ToUtf8(text))
            + "\",\"hotkeyVk\":" + std::to_string(keyVk)
            + ",\"hotkeyModifiers\":" + std::to_string(mods)
            + ",\"hotkeyHold\":" + (holdMode ? "true" : "false") + "}");
        return;
    }
    if (type == "testOcr") {
        qst::desktop_tools::TestOcrParams params;
        std::string imagePathUtf8, searchTextUtf8;
        JsonGetString(json, "mode", params.modeUtf8);
        JsonGetInt(json, "ocrRegionByImage", params.ocrRegionByImage);
        JsonGetInt(json, "ocrDigitsOnly", params.ocrDigitsOnly);
        JsonGetInt(json, "searchFullScreen", params.searchFullScreen);
        JsonGetInt(json, "ocrResultMode", params.ocrResultMode);
        JsonGetInt(json, "searchX1", params.searchX1);
        JsonGetInt(json, "searchY1", params.searchY1);
        JsonGetInt(json, "searchX2", params.searchX2);
        JsonGetInt(json, "searchY2", params.searchY2);
        JsonGetInt(json, "imageRegionX1", params.imageRegionX1);
        JsonGetInt(json, "imageRegionY1", params.imageRegionY1);
        JsonGetInt(json, "imageRegionX2", params.imageRegionX2);
        JsonGetInt(json, "imageRegionY2", params.imageRegionY2);
        JsonGetDouble(json, "matchThreshold", params.matchThreshold);
        JsonGetInt(json, "perfectMatch", params.perfectMatch);
        JsonGetDouble(json, "imageScaleMin", params.imageScaleMin);
        JsonGetDouble(json, "imageScaleMax", params.imageScaleMax);
        JsonGetString(json, "imagePath", imagePathUtf8);
        JsonGetString(json, "ocrSearchText", searchTextUtf8);
        params.imagePath = FromUtf8(imagePathUtf8);
        params.ocrSearchText = FromUtf8(searchTextUtf8);
        const auto r = qst::desktop_tools::TestOcr(g_hwnd, params);
        if (!r.ok) {
            if (r.needInstall) {
                PostToJs("{\"type\":\"testOcr.result\",\"ok\":false,\"detail\":\"OCR 未就绪\",\"needInstall\":true}");
            } else {
                PostToJs(std::string("{\"type\":\"testOcr.result\",\"ok\":false,\"detail\":\"")
                    + EscapeJsonUtf8(r.detail) + "\"}");
            }
            return;
        }
        if (r.modeUtf8 == "offset") {
            PostToJs(std::string("{\"type\":\"testOcr.result\",\"ok\":true,\"mode\":\"offset\",\"offsetX\":")
                + std::to_string(r.offsetX) + ",\"offsetY\":" + std::to_string(r.offsetY) + "}");
            return;
        }
        PostToJs(std::string("{\"type\":\"testOcr.result\",\"ok\":true,\"mode\":\"test\",\"text\":\"")
            + EscapeJsonUtf8(ToUtf8(r.text)) + "\"}");
        return;
    }
    if (type == "crosshairPick") {
        std::string mode;
        JsonGetString(json, "mode", mode);
        if (mode.empty()) mode = "coordinates";
        const auto r = qst::desktop_tools::CrosshairPick(g_hwnd, mode);
        if (!r.ok) {
            PostToJs(std::string("{\"type\":\"crosshairPick.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(r.detail.empty() ? "cancelled" : r.detail) + "\"}");
            return;
        }
        PostToJs(std::string("{\"type\":\"crosshairPick.result\",\"ok\":true,\"mode\":\"")
            + EscapeJsonUtf8(mode) + "\",\"pick\":" + r.pickJson + "}");
        return;
    }
    if (type == "browsePath") {
        int exeOnly = 0;
        JsonGetInt(json, "executableOnly", exeOnly);
        HWND browseParent = (g_handlingAgentMsg && g_agentHwnd && IsWindow(g_agentHwnd))
            ? g_agentHwnd
            : g_hwnd;
        // 勿在 WebMessageReceived 里同步弹模态框：会卡在回调里半天才出对话框
        auto* req = new std::pair<HWND, bool>(browseParent, exeOnly != 0);
        if (!PostMessageW(g_hwnd, WM_APP_BROWSE_PATH, 0, reinterpret_cast<LPARAM>(req))) {
            delete req;
            PostToJs("{\"type\":\"browsePath.result\",\"ok\":false,\"detail\":\"无法打开文件选择器\"}");
        }
        return;
    }
    if (type == "installOcr") {
        static std::atomic<bool> s_ocrInstallRunning{false};
        if (s_ocrInstallRunning.exchange(true)) {
            PostToJs("{\"type\":\"installOcr.result\",\"ok\":false,\"detail\":\"正在安装中，请稍候\"}");
            return;
        }
        int repair = 0;
        JsonGetInt(json, "repair", repair);
        (void)repair;
        PostToJs("{\"type\":\"installOcr.progress\",\"ok\":true,\"indeterminate\":false,\"percent\":0,"
                 "\"status\":\"已就绪，点击安装开始下载安装…\"}");
        // 在后台线程跑 RunOcrInstall，进度经 WM_BRIDGE_POST_JS 回 Web（不再弹原生 OcrInstallDialog）
        std::thread([]() {
            auto postJs = [](std::string json) {
                if (!g_hwnd || !IsWindow(g_hwnd)) return;
                PostMessageW(g_hwnd, WM_BRIDGE_POST_JS, 0,
                    reinterpret_cast<LPARAM>(new std::string(std::move(json))));
            };
            std::wstring message;
            const bool ok = RunOcrInstall(message, [&](int percent, const std::wstring& status) {
                std::ostringstream oss;
                oss << "{\"type\":\"installOcr.progress\",\"ok\":true,\"indeterminate\":false,\"percent\":"
                    << std::clamp(percent, 0, 100)
                    << ",\"status\":\"" << EscapeJsonUtf8(ToUtf8(status)) << "\"}";
                postJs(oss.str());
            });
            {
                std::ostringstream oss;
                oss << "{\"type\":\"installOcr.progress\",\"ok\":true,\"indeterminate\":false,\"percent\":"
                    << (ok ? 100 : 0)
                    << ",\"status\":\""
                    << EscapeJsonUtf8(ToUtf8(message.empty()
                        ? (ok ? L"安装完成" : L"安装失败，请重试")
                        : message))
                    << "\"}";
                postJs(oss.str());
            }
            {
                std::ostringstream oss;
                oss << "{\"type\":\"installOcr.result\",\"ok\":" << (ok ? "true" : "false")
                    << ",\"detail\":\""
                    << EscapeJsonUtf8(ToUtf8(message.empty()
                        ? (ok ? L"installed" : L"install_failed")
                        : message))
                    << "\"}";
                postJs(oss.str());
            }
            s_ocrInstallRunning = false;
        }).detach();
        return;
    }
    if (type == "getAppBranding") {
        PostToJs(std::string("{\"type\":\"getAppBranding.result\",\"ok\":true,\"branding\":")
            + qst::webview::JsonAppBranding() + "}");
        return;
    }
    if (type == "checkUpgrade") {
        // 诚实 stub：未接入在线升级通道，勿提示「已是最新」冒充检查成功
        PostToJs("{\"type\":\"checkUpgrade.result\",\"ok\":true,\"stub\":true,"
                 "\"detail\":\"当前版本未接入在线升级检查。\"}");
        return;
    }
    if (type == "showDebugWindow") {
        // 产品：独立调试 WebView 顶层窗（可拖出主壳）
        qst::desktop_tools::RequestShowDebugWindow([]() { qst::engine::ReloadSettings(); });
        PostToJs("{\"type\":\"showDebugWindow.result\",\"ok\":true}");
        return;
    }
    if (type == "openAgentWindow") {
        std::string id;
        int createNew = 0;
        JsonGetString(json, "id", id);
        JsonGetInt(json, "createNew", createNew);
        std::ostringstream oss;
        oss << "{\"type\":\"agentWindow.open\",\"ok\":true,\"id\":\""
            << EscapeJsonUtf8(id) << "\",\"createNew\":" << (createNew ? 1 : 0) << "}";
        OpenAgentWebWindow(oss.str());
        PostToJs("{\"type\":\"openAgentWindow.result\",\"ok\":true}");
        return;
    }
    if (type == "debugWindowClosed") {
        qst::engine::DebugWindowClosedByUser();
        HideDebugWebWindow();
        PostToJs("{\"type\":\"debugWindowClosed.result\",\"ok\":true}");
        return;
    }
    if (type == "debugWindowSetTopmost") {
        int topmost = 1;
        JsonGetInt(json, "topmost", topmost);
        g_debugPinned = topmost != 0;
        ApplyDebugTopmost();
        PostToDebugJs(std::string("{\"type\":\"debugWindowSetTopmost.result\",\"ok\":true,\"topmost\":")
            + (g_debugPinned ? "1" : "0") + "}");
        PostToJs("{\"type\":\"debugWindowSetTopmost.result\",\"ok\":true}");
        return;
    }
    if (type == "showWindowModePreview") {
        windowmode::WindowModeScriptConfig cfg{};
        std::string err;
        int fromRunning = 0;
        JsonGetInt(json, "fromRunning", fromRunning);
        if (fromRunning) {
            if (!qst::engine::IsRunning() || !qst::engine::RunningWindowMode(cfg)) {
                PostToJs("{\"type\":\"showWindowModePreview.result\",\"ok\":false,\"detail\":\"当前没有正在运行的脚本\"}");
                return;
            }
            if (!cfg.enabled) {
                PostToJs("{\"type\":\"showWindowModePreview.result\",\"ok\":false,\"detail\":\"正在运行的脚本未启用窗口模式\"}");
                return;
            }
        } else if (!qst::webview::ParseWindowModePreviewRequest(json, cfg, err)) {
            PostToJs(std::string("{\"type\":\"showWindowModePreview.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        HWND target = nullptr;
        std::wstring label;
        if (!qst::webview::ResolveWindowModeTargetHwnd(cfg, target, label, err)) {
            PostToJs(std::string("{\"type\":\"showWindowModePreview.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(err) + "\"}");
            return;
        }
        // 产品路径：只回 Web #wmPreview（dataUrl）；不再弹原生 WindowModePreview 窗
        KillTimer(g_hwnd, kWindowModePreviewTimerId);
        g_wmPreview.Hide();
        g_wmPreviewTarget = nullptr;
        g_webPreviewVisible.store(false, std::memory_order_relaxed);
        g_previewCapturing.store(false, std::memory_order_relaxed);
        // 首帧抓取放后台线程，避免 PrintWindow（可达上百毫秒）卡住 UI 线程
        g_wmPreviewTarget = target;
        if (fromRunning) {
            // 运行中预览：按设置间隔持续刷新（默认 500ms，可调 200~5000ms）
            const int refreshMs = std::clamp(static_cast<int>(
                qst::webview::Ctx().settings.windowMode.previewRefreshMs), 200, 5000);
            SetTimer(g_hwnd, kWindowModePreviewTimerId, static_cast<UINT>(refreshMs), nullptr);
        }
        auto* req = new PreviewCaptureReq{target, std::move(label)};
        if (!PostMessageW(g_hwnd, WM_APP_CAPTURE_PREVIEW, 0, reinterpret_cast<LPARAM>(req))) {
            delete req;
            PostToJs("{\"type\":\"showWindowModePreview.result\",\"ok\":false,\"detail\":\"预览启动失败\"}");
        }
        return;
    }
    if (type == "hideWindowModePreview") {
        KillTimer(g_hwnd, kWindowModePreviewTimerId);
        g_wmPreview.Hide();
        g_wmPreviewTarget = nullptr;
        g_webPreviewVisible.store(false, std::memory_order_relaxed);
        g_previewCapturing.store(false, std::memory_order_relaxed);
        PostToJs("{\"type\":\"hideWindowModePreview.result\",\"ok\":true}");
        return;
    }

    PostToJs(std::string("{\"type\":\"error\",\"detail\":\"unknown method:") + type + "\"}");
}

void ShowIncompletePackageError() {
    const std::wstring detail =
        L"安装包不完整或 WebView2 未能启动。\n\n"
        L"请使用完整便携包（含 WebView2Fixed 文件夹），"
        L"整夹拷贝后再运行，不要只复制 exe。\n\n"
        L"本产品不要求也不引导安装系统 WebView2 Runtime。\n\n"
        L"详见同目录 webview_boot.log / shell_startup.log。";
    ShowShellError(detail.c_str());
}

static void LaunchWebView(const wchar_t* browserFolder, int attempt);

void RequestLaunchWebView() {
    if (g_webviewLaunchStarted) return;
    g_webviewLaunchStarted = true;
    if (g_hwnd) {
        KillTimer(g_hwnd, kAclWaitTimerId);
    }
    BootLogLine("RequestLaunchWebView");
    StartupTrace("launch webview");
    LaunchWebView(g_browserFolder.empty() ? nullptr : g_browserFolder.c_str(), 0);
}

// 固定运行时在该机器上起不来（环境/控制器创建失败、浏览器进程退出、
// 首次导航失败、页面 7s 仍未画出）时，自动回退到系统已装的 WebView2
// （Win10 通常随 Edge 自带），避免整窗淡蓝空白。仅在 attempt=0 时触发一次。
static void RequestEvergreenFallback(int reasonIdx) {
    if (g_evergreenFallbackPending || g_webviewAttempt != 0 || !g_hwnd) return;
    PostMessageW(g_hwnd, WM_APP_EVERGREEN_FALLBACK, static_cast<WPARAM>(reasonIdx), 0);
}

static void ScheduleEvergreenFallback(const char* reason) {
    if (g_evergreenFallbackPending || g_webviewAttempt != 0 || !g_hwnd) return;
    g_evergreenFallbackPending = true;
    BootLogLine(std::string("FALLBACK: ") + reason + " -> system WebView2 (attempt 1)");

    // 释放固定运行时相关的一切 WebView（含预热的 agent/debug 窗）
    g_agentController.Reset();
    g_agentWebview.Reset();
    g_debugController.Reset();
    g_debugWebview.Reset();
    g_agentReady = false;
    g_agentContentReady = false;
    g_agentCreating = false;
    g_debugReady = false;
    g_debugContentReady = false;
    g_debugCreating = false;
    g_secondaryPrewarmScheduled = false;

    g_controller.Reset();
    g_webview.Reset();
    g_webviewEnv.Reset();
    g_mainWebVisible = false;
    g_mainContentReady = false;
    g_mainNavSucceeded = false;
    g_mainNavDoneOnce = false;
    g_webviewAttempt = 1;
    KillTimer(g_hwnd, kEvergreenFallbackTimerId);
    KillTimer(g_hwnd, kMainRevealSettleTimerId);
    LaunchWebView(nullptr, 1);
}

void InitWebView() {
    BootLogReset();
    BootLogLineW(L"exeDir=" + ExeDir());
    BootLogLineW(L"appDir=" + AppDir());

    qst::webview::SetJsPoster([](std::string json) {
        if (!g_hwnd) return;
        PostMessageW(g_hwnd, WM_BRIDGE_POST_JS, 0,
            reinterpret_cast<LPARAM>(new std::string(std::move(json))));
    });
    qst::desktop_tools::SetMacroDebugWebPoster([](std::string json) {
        if (!g_hwnd) return;
        PostMessageW(g_hwnd, WM_DEBUG_POST_JS, 0,
            reinterpret_cast<LPARAM>(new std::string(std::move(json))));
    });
    SetLogicConvertUiNotify([](const std::wstring& path, const std::wstring& summary) {
        std::string j = "{\"type\":\"logicConvert.updated\",\"ok\":true,\"path\":\"";
        // 简易转义
        for (char c : ToUtf8(path)) {
            if (c == '\\') j += "\\\\";
            else if (c == '"') j += "\\\"";
            else j += c;
        }
        j += "\",\"summary\":\"";
        for (char c : ToUtf8(summary)) {
            if (c == '\\') j += "\\\\";
            else if (c == '"') j += "\\\"";
            else if (c == '\n') j += "\\n";
            else j += c;
        }
        j += "\"}";
        qst::webview::PostToWebUi(std::move(j));
    });
    g_browserFolder.clear();
    g_userDataFolder = (std::filesystem::path(ExeDir()) / kUserDataDirName).wstring();
    std::error_code ec;
    std::filesystem::create_directories(g_userDataFolder, ec);
    BootLogLineW(L"userData=" + g_userDataFolder);

#if QST_WEBVIEW_USE_FIXED
    if (!ResolveFixedBrowserFolder(g_browserFolder)) {
        BootLogLine("ResolveFixedBrowserFolder: FAILED");
#if !QST_WEBVIEW_ALLOW_EVERGREEN
        ShowIncompletePackageError();
        return;
#endif
    } else {
        BootLogLineW(L"fixedBrowser=" + g_browserFolder);
        const std::wstring fixedRoot =
            (std::filesystem::path(ExeDir()) / kFixedDirName).wstring();
        const auto marker = std::filesystem::path(fixedRoot) / kAclMarkerName;
        std::error_code markerEc;
        if (std::filesystem::exists(marker, markerEc)) {
            EnsureFixedRuntimeAppContainerAcl(fixedRoot); // 有 marker：秒退
            RequestLaunchWebView();
        } else {
            // 解压后 ACL 常丢失；递归授 RX 可能在杀软下阻塞数十秒——必须后台做，否则无消息循环/无托盘刷新
            EnsureFixedRuntimeAppContainerAclAsync(fixedRoot);
        }
        return;
    }
#endif

    RequestLaunchWebView();
}

static void LaunchWebView(const wchar_t* browserFolder, int attempt) {
    const wchar_t* browserArg = browserFolder;
    const wchar_t* userDataArg = g_userDataFolder.c_str();
    BootLogLine(attempt == 0 ? "launch attempt=0 (fixed runtime)"
                             : "launch attempt=1 (system WebView2)");
    if (attempt == 0) {
        BootLogLineW(L"launch folder=" +
            (g_browserFolder.empty() ? std::wstring(L"<none>") : g_browserFolder));
    }

    // 浏览器参数见 kWebViewBrowserArgs（默认 GPU；勿再强制 SwiftShader）
    auto options = Make<CoreWebView2EnvironmentOptions>();
    if (options && kWebViewBrowserArgs[0]) {
        options->put_AdditionalBrowserArguments(kWebViewBrowserArgs);
    }
    BootLogLineW(std::wstring(L"browserArgs=") + kWebViewBrowserArgs);

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        browserArg, userDataArg, options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                char buf[96]{};
                sprintf_s(buf, "CreateEnvironment hr=0x%08lX env=%s",
                    static_cast<unsigned long>(result), env ? "ok" : "null");
                BootLogLine(buf);
                if (FAILED(result) || !env) {
                    if (g_webviewAttempt == 0) {
                        RequestEvergreenFallback(0);
                        return result;
                    }
                    ShowIncompletePackageError();
                    return result;
                }
                g_webviewEnv = env;
                return env->CreateCoreWebView2Controller(g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {
                            char buf2[96]{};
                            sprintf_s(buf2, "CreateController hr=0x%08lX ctl=%s",
                                static_cast<unsigned long>(result2),
                                controller ? "ok" : "null");
                            BootLogLine(buf2);
                            if (FAILED(result2) || !controller) {
                                if (g_webviewAttempt == 0) {
                                    RequestEvergreenFallback(1);
                                    return result2;
                                }
                                ShowIncompletePackageError();
                                return result2;
                            }
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);
                            if (!g_webview) {
                                BootLogLine("get_CoreWebView2 failed");
                                if (g_webviewAttempt == 0) {
                                    RequestEvergreenFallback(1);
                                    return E_FAIL;
                                }
                                ShowIncompletePackageError();
                                return E_FAIL;
                            }

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                                settings->put_AreHostObjectsAllowed(TRUE);
                            }
                            DisableWebViewContextMenu(g_webview.Get());
                            g_controller->put_ZoomFactor(1.0);
                            ApplyWebViewRasterScale1();
                            // 父窗先 HIDE：WebView 仍可见以便绘出与终局同结构的 DOM，禁止空 sky / GDI 假壳
                            g_controller->put_IsVisible(TRUE);
                            g_mainWebVisible = true;
                            SetControllerChromeBg(g_controller.Get(), 0xe8, 0xf4, 0xfc);

                            // 映射 AppDir → https://qst.local/…（主 UI 与找图预览）
                            ComPtr<ICoreWebView2_3> webview3;
                            if (SUCCEEDED(g_webview.As(&webview3)) && webview3) {
                                webview3->SetVirtualHostNameToFolderMapping(
                                    L"qst.local",
                                    AppDir().c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                InstallQstLocalAccessGuard(g_webview.Get());
                                BootLogLine("virtualHost qst.local mapped");
                            } else {
                                BootLogLine("virtualHost mapping FAILED (no ICoreWebView2_3)");
                            }
                            InstallWebViewNavigationGuard(g_webview.Get());

                            RegisterSyncHostObject();

                            g_webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        BOOL ok = FALSE;
                                        COREWEBVIEW2_WEB_ERROR_STATUS err =
                                            COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                        if (args) {
                                            args->get_IsSuccess(&ok);
                                            args->get_WebErrorStatus(&err);
                                        }
                                        char nb[160]{};
                                        sprintf_s(nb, "NavigationCompleted attempt=%d ok=%d err=%d",
                                            g_webviewAttempt, static_cast<int>(ok),
                                            static_cast<int>(err));
                                        BootLogLine(nb);
                                        if (!g_mainNavDoneOnce) {
                                            g_mainNavDoneOnce = true;
                                            g_mainNavSucceeded = (ok == TRUE);
                                            if (!ok && g_webviewAttempt == 0) {
                                                RequestEvergreenFallback(2);
                                                return S_OK;
                                            }
                                        }
                                        // 页面重载：结束热键捕获 + 停壳侧 LL，避免钩子泄漏/启停全哑
                                        StopWebHotkeyLlHook();
                                        StopWebActionKeyLlHook();
                                        qst::engine::EndHotkeyCaptureRelease();
                                        RegisterSyncHostObject();
                                        // 勿在 NavigationCompleted 强行揭窗：须等 shell.contentReady（真 chrome）
                                        // 若 ping 丢失，kMainRevealTimerId 兜底
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            g_webview->add_ProcessFailed(
                                Callback<ICoreWebView2ProcessFailedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                                        COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                                            COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                                        if (args) args->get_ProcessFailedKind(&kind);
                                        char pb[96]{};
                                        sprintf_s(pb, "ProcessFailed attempt=%d kind=%d",
                                            g_webviewAttempt, static_cast<int>(kind));
                                        BootLogLine(pb);
                                        if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED
                                            && g_webviewAttempt == 0) {
                                            RequestEvergreenFallback(3);
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        CotaskStr asStr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(asStr.put())) && asStr.p) {
                                            HandleBridgeMessage(NarrowUtf8(asStr.get()));
                                            return S_OK;
                                        }
                                        CotaskStr raw;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(raw.put())) && raw.p) {
                                            HandleBridgeMessage(NarrowUtf8(raw.get()));
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            g_webview->AddScriptToExecuteOnDocumentCreated(
                                L"(function(){"
                                L"document.documentElement.classList.add('qst-webview','shell-booting');"
                                L"document.addEventListener('contextmenu',function(e){e.preventDefault();},true);"
                                L"window.qstBridge={post:function(o){"
                                L"var s=(typeof o==='string')?o:JSON.stringify(o);"
                                L"if(window.chrome&&chrome.webview)chrome.webview.postMessage(s);"
                                L"}};"
                                L"})();",
                                nullptr);

                            ResizeWebView();
                            // 主壳仍用本地 file://（PathToFileUrl 已做百分号编码）；qst.local 仅给资源/预览映射
                            const std::wstring mainUrl = PathToFileUrl(UiIndexPath());
                            BootLogLineW(L"Navigate " + mainUrl);
                            g_webview->Navigate(mainUrl.c_str());
                            // 主页还在加载时就开始预热 AI 窗，缩短首次打开等待
                            ScheduleSecondaryWebViewPrewarm();
                            if (g_webviewAttempt == 0) {
                                // 固定运行时 7s 内既无 NavigationCompleted ok 也无 contentReady -> 回退系统
                                SetTimer(g_hwnd, kEvergreenFallbackTimerId, 7000, nullptr);
                            }
                            return S_OK;
                        }).Get());
            }).Get());

    {
        char hrb[64]{};
        sprintf_s(hrb, "CreateEnvironmentWithOptions sync hr=0x%08lX",
            static_cast<unsigned long>(hr));
        BootLogLine(hrb);
    }
    if (FAILED(hr)) {
        if (g_webviewAttempt == 0) {
            RequestEvergreenFallback(0);
        } else {
            ShowIncompletePackageError();
        }
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_wmTaskbarCreated && msg == g_wmTaskbarCreated) {
        g_trayActive = false;
        EnsureTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        ApplyWindowMainTaskbarPresentation(hwnd, L"键鼠工坊");
        ApplyWindowCornerRadius(hwnd);
        SetTimer(hwnd, kStatusTimerId, 1000, nullptr);
        // AI 工具修改脚本/录制后经此 WM 通知主壳刷新列表
        SetAgentUiNotifyHwnd(hwnd);
        return 0;
    case WM_AGENT_SCRIPT_LIBRARY_CHANGED:
        if (g_webview) {
            g_webview->PostWebMessageAsJson(WidenUtf8(
                std::string("{\"type\":\"listScripts.result\",\"ok\":true,\"scripts\":")
                + qst::webview::JsonListScripts() + "}").c_str());
            g_webview->PostWebMessageAsJson(WidenUtf8(
                std::string("{\"type\":\"listRecordings.result\",\"ok\":true,\"recordings\":")
                + qst::webview::JsonListRecordings() + "}").c_str());
        }
        return 0;
    case WM_TIMER:
        if (wp == kStatusTimerId) {
            EnsureTrayIcon();
            PushEngineStatusIfChanged(false);
            return 0;
        }
        if (wp == kPrewarmTimerId) {
            KillTimer(hwnd, kPrewarmTimerId);
            PrewarmSecondaryWebViews();
            return 0;
        }
        if (wp == kMainRevealTimerId) {
            KillTimer(hwnd, kMainRevealTimerId);
            if (!g_mainContentReady && !g_mainNavSucceeded && !g_revealErrorShown) {
                g_revealErrorShown = true;
                BootLogLine(std::string("REVEAL_FALLBACK contentReady=0 navOk=0 attempt=")
                    + std::to_string(g_webviewAttempt));
                if (g_webviewAttempt >= 1) {
                    ShowIncompletePackageError();
                } else {
                    RequestEvergreenFallback(4);
                }
            }
            g_mainContentReady = true;
            RevealMainWindowIfReady();
            return 0;
        }
        if (wp == kBootVisibleTimerId) {
            KillTimer(hwnd, kBootVisibleTimerId);
            ShowBootPlaceholderIfNeeded();
            return 0;
        }
        if (wp == kAclWaitTimerId) {
            KillTimer(hwnd, kAclWaitTimerId);
            BootLogLine("ACL: wait timeout -> launch WebView anyway");
            StartupTrace("ACL timeout launch");
            RequestLaunchWebView();
            return 0;
        }
        if (wp == kMainRevealSettleTimerId) {
            KillTimer(hwnd, kMainRevealSettleTimerId);
            RevealMainWindowIfReady();
            return 0;
        }
        if (wp == kEvergreenFallbackTimerId) {
            KillTimer(hwnd, kEvergreenFallbackTimerId);
            if (g_webviewAttempt == 0 && !g_mainContentReady && !g_mainNavSucceeded) {
                RequestEvergreenFallback(4);
            }
            return 0;
        }
        if (wp == kWebHotkeyHoldTimerId) {
            OnWebHotkeyHoldTimer();
            return 0;
        }
        if (wp == kWebCaptureWatchdogTimerId) {
            TickWebCaptureWatchdog();
            return 0;
        }
        if (wp == kWindowModePreviewTimerId) {
            // Web 预览刷新：抓帧放到后台线程，避免 PrintWindow+JPEG 编码卡住 UI 线程
            if (g_webPreviewVisible.load(std::memory_order_relaxed)
                && g_wmPreviewTarget && IsWindow(g_wmPreviewTarget)
                && !g_previewCapturing.exchange(true, std::memory_order_relaxed)) {
                const HWND target = g_wmPreviewTarget;
                std::thread([target]() {
                    const std::string frameUrl =
                        qst::webview::CaptureHwndPreviewDataUrl(target);
                    g_previewCapturing.store(false, std::memory_order_relaxed);
                    if (!frameUrl.empty()) {
                        PostToJsAsync(std::string(
                            "{\"type\":\"windowModePreview.frame\",\"dataUrl\":\"")
                            + EscapeJsonUtf8(frameUrl) + "\"}");
                    }
                }).detach();
            }
            return 0;
        }
        break;
    case WM_SIZE:
        ResizeWebView();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        if (g_applyingModeResize) {
            mmi->ptMinTrackSize.x = 200;
            mmi->ptMinTrackSize.y = 200;
            mmi->ptMaxTrackSize.x = 100000;
            mmi->ptMaxTrackSize.y = 100000;
            return 0;
        }
        // 主页（极简 1380 / 专业 1552）×960 / 编辑器 1800×1230 / 优化 1640×1140
        int designW = g_homeClientW;
        int designH = g_homeClientH;
        if (g_mode == UiMode::Editor) {
            designW = kEditorClientW;
            designH = kEditorClientH;
        } else if (g_mode == UiMode::Optimize) {
            designW = kOptClientW;
            designH = kOptClientH;
        }
        int outerW = 0, outerH = 0;
        ClientToOuterSize(designW, designH, outerW, outerH);
        mmi->ptMinTrackSize.x = outerW;
        mmi->ptMinTrackSize.y = outerH;
        mmi->ptMaxTrackSize.x = outerW;
        mmi->ptMaxTrackSize.y = outerH;
        return 0;
    }
    case WM_DPICHANGED: {
        // 尺寸跟 mockup 锁定，DPI 变化时只重套当前模式客户区，不跟系统建议矩形
        ApplyUiMode(g_mode);
        return 0;
    }
    case WM_APPLY_UI_MODE: {
        UiMode mode = UiMode::Home;
        if (wp == 1) mode = UiMode::Editor;
        else if (wp == 2) mode = UiMode::Optimize;
        ApplyUiMode(mode);
        return 0;
    }
    case WM_BRIDGE_POST_JS: {
        auto* payload = reinterpret_cast<std::string*>(lp);
        if (payload) {
            PostToJs(*payload);
            delete payload;
        }
        return 0;
    }
    case WM_APP_BROWSE_PATH: {
        auto* req = reinterpret_cast<std::pair<HWND, bool>*>(lp);
        if (!req) return 0;
        const HWND browseParent = req->first;
        const bool exeOnly = req->second;
        delete req;
        const auto r = qst::desktop_tools::BrowsePath(browseParent, exeOnly);
        if (!r.ok) {
            PostToJs(std::string("{\"type\":\"browsePath.result\",\"ok\":false,\"detail\":\"")
                + EscapeJsonUtf8(r.detail.empty() ? "cancelled" : r.detail) + "\"}");
            return 0;
        }
        PostToJs(std::string("{\"type\":\"browsePath.result\",\"ok\":true,\"path\":\"")
            + EscapeJsonUtf8(ToUtf8(r.path)) + "\"}");
        return 0;
    }
    case WM_APP_CAPTURE_PREVIEW: {
        auto* req = reinterpret_cast<PreviewCaptureReq*>(lp);
        if (!req) return 0;
        const HWND target = req->target;
        std::wstring label = std::move(req->label);
        delete req;
        std::thread([target, label = std::move(label)]() {
            const std::string dataUrl = qst::webview::CaptureHwndPreviewDataUrl(target);
            g_webPreviewVisible.store(!dataUrl.empty(), std::memory_order_relaxed);
            std::string json;
            if (dataUrl.empty()) {
                json = "{\"type\":\"showWindowModePreview.result\",\"ok\":false,\"detail\":\"预览截图失败\"}";
            } else {
                json = std::string("{\"type\":\"showWindowModePreview.result\",\"ok\":true,\"native\":false,\"label\":\"")
                    + EscapeJsonUtf8(ToUtf8(label))
                    + "\",\"dataUrl\":\"" + EscapeJsonUtf8(dataUrl) + "\"}";
            }
            PostToJsAsync(std::move(json));
        }).detach();
        return 0;
    }
    case WM_DEBUG_POST_JS: {
        auto* payload = reinterpret_cast<std::string*>(lp);
        if (payload) {
            DispatchDebugUiMessage(*payload);
            delete payload;
        }
        return 0;
    }
    case WM_APP_EVERGREEN_FALLBACK: {
        static const char* kReasons[] = {
            "CreateEnvironment failed",
            "CreateController failed",
            "main navigation failed",
            "browser process exited",
            "page not painted within timeout",
        };
        const size_t idx = wp < 5 ? wp : 0;
        ScheduleEvergreenFallback(kReasons[idx]);
        return 0;
    }
    case WM_APP_LAUNCH_WEBVIEW:
        RequestLaunchWebView();
        return 0;
    case WM_NCHITTEST: {
        return HTCLIENT;
    }
    case WM_TRAY: {
        // NOTIFYICON_VERSION_4：lParam 低字为事件（WM_CONTEXTMENU / NIN_SELECT…）
        const UINT trayEvent = LOWORD(lp);
        if (trayEvent == WM_CONTEXTMENU || trayEvent == WM_RBUTTONUP) {
            POINT pt{ GET_X_LPARAM(wp), GET_Y_LPARAM(wp) };
            if (pt.x == 0 && pt.y == 0) GetCursorPos(&pt);
            ShowTrayContextMenuAt(pt);
        } else if (trayEvent == NIN_SELECT || trayEvent == NIN_KEYSELECT
            || trayEvent == WM_LBUTTONUP || trayEvent == WM_LBUTTONDBLCLK) {
            RestoreMainWindow();
        } else if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
            // 兼容未 SETVERSION 的旧回调（lParam 直接是鼠标消息）
            RestoreMainWindow();
        } else if (lp == WM_RBUTTONUP) {
            ShowTrayContextMenu();
        }
        return 0;
    }
    case WM_APP_RESTORE_INSTANCE:
        RestoreMainWindow();
        return 0;
    case WM_CLOSE:
        qst::engine::StopClicker();
        qst::engine::StopScript();
        qst::webview::StopClicker();
        KillTimer(hwnd, kStatusTimerId);
        KillTimer(hwnd, kWindowModePreviewTimerId);
        g_wmPreview.Destroy();
        g_wmPreviewTarget = nullptr;
        DestroyDebugWebWindow();
        DestroyAgentWebWindow();
        RemoveTrayIcon();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        qst::engine::StopClicker();
        qst::webview::StopClicker();
        RemoveTrayIcon();
        DestroyDebugWebWindow();
        DestroyAgentWebWindow();
        g_webview.Reset();
        g_controller.Reset();
        g_webviewEnv.Reset();
        qst::engine::Shutdown();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterWndClass(HINSTANCE inst) {
    // 跟内容区 sky-100，避免启动期 navy 被看成「黑屏」
    static HBRUSH s_mainBg = CreateSolidBrush(RGB(0xe8, 0xf4, 0xfc));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = s_mainBg ? s_mainBg : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = LoadAppIcon();
    wc.hIconSm = LoadAppIconSmall();
    if (RegisterClassExW(&wc) != 0) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

// delayimp 需要全局可见的 hook 符号（不可放进匿名命名空间）
extern "C" const PfnDliHook __pfnDliFailureHook2 = QstDelayLoadFailureHook;

namespace qst::webview {
void HotkeyLogLine(const std::string& line) {
    BootLogLine("HOTKEY: " + line);
}
}  // namespace qst::webview

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    g_instance = inst;
    StartupTrace("wWinMain enter");
    if (!EnsureOpenCvLoadable()) {
        return 1;
    }
    StartupTrace("after opencv");

    using SetDpi = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto fn = reinterpret_cast<SetDpi>(
            GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext"))) {
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    StartupTrace("after dpi");

    // Single-instance（只认壳窗；勿把 headless KeyMouseEngineWindow 当可恢复 UI）
    for (int attempt = 0; attempt < 40; ++attempt) {
        g_mutex = CreateMutexW(nullptr, FALSE, L"KeyMouse_SingleInstance");
        if (g_mutex && GetLastError() != ERROR_ALREADY_EXISTS) break;
        HWND existing = FindWindowW(L"QstWebViewShellWindow", nullptr);
        if (existing && IsWindow(existing)) {
            PostMessageW(existing, WM_APP_RESTORE_INSTANCE, 0, 0);
            StartupTrace("handoff to existing shell");
            if (g_mutex) CloseHandle(g_mutex);
            return 0;
        }
        if (g_mutex) CloseHandle(g_mutex);
        g_mutex = nullptr;
        if (attempt == 8 || attempt == 20) {
            TerminateOtherInstancesOfCurrentExe();
        }
        Sleep(100);
    }
    if (!g_mutex) {
        TerminateOtherInstancesOfCurrentExe();
        Sleep(300);
        g_mutex = CreateMutexW(nullptr, FALSE, L"KeyMouse_SingleInstance");
        if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (g_mutex) CloseHandle(g_mutex);
            StartupTrace("single-instance acquire failed");
            ShowShellError(L"键鼠工坊已在运行，但未能唤起窗口。\n\n"
                L"请打开任务管理器结束「键鼠工坊 / QuickScriptTool」进程后重试。");
            return 1;
        }
    }
    StartupTrace("single-instance ok");

    SetCurrentProcessExplicitAppUserModelID(L"ShuDaXia.KeyMouse");

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        StartupTrace("CoInitializeEx failed");
        ShowShellError(L"系统组件初始化失败（COM）。");
        if (g_mutex) CloseHandle(g_mutex);
        return 1;
    }
    StartupTrace("com ok");

    if (!qst::engine::Start(inst)) {
        StartupTrace("engine Start failed");
        ShowShellError(L"引擎初始化失败。");
        CoUninitialize();
        if (g_mutex) CloseHandle(g_mutex);
        return 1;
    }
    StartupTrace("engine Start ok");
    input_emergency::RegisterExtraTeardown(EmergencyUnhookWebCaptureLl);
    CleanOrphanImages();
    StartupTrace("after CleanOrphanImages");

    if (!RegisterWndClass(inst)) {
        StartupTrace("RegisterWndClass failed");
        ShowShellError(L"窗口类注册失败，无法创建主界面。");
        qst::engine::Shutdown();
        CoUninitialize();
        if (g_mutex) CloseHandle(g_mutex);
        return 1;
    }
    RegisterDebugWndClass(inst);
    RegisterAgentWndClass(inst);
    StartupTrace("wndclass ok");

    int ww = 0, wh = 0;
    ClientToOuterSize(g_homeClientW, g_homeClientH, ww, wh);

    g_hwnd = CreateWindowExW(
        WindowExStyle(),
        kWndClass,
        L"键鼠工坊",
        WindowStyle(),
        CW_USEDEFAULT, CW_USEDEFAULT, ww, wh,
        nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) {
        StartupTrace("CreateWindowEx failed");
        ShowShellError(L"主窗口创建失败。");
        qst::engine::Shutdown();
        CoUninitialize();
        if (g_mutex) CloseHandle(g_mutex);
        return 1;
    }
    StartupTrace("CreateWindowEx ok");

    qst::engine::SetUiHost(g_hwnd);
    qst::engine::EnsureHotkeysArmed();
    StartupTrace("hotkeys armed");
    ApplyWindowMainTaskbarPresentation(g_hwnd, L"键鼠工坊");
    SetWindowClientSizeCentered(g_homeClientW, g_homeClientH);
    g_mainShowCmd = show;
    g_mainWantShow = (show != SW_HIDE);
    g_mainContentReady = false;
    g_mainWebVisible = false;
    g_mainNavSucceeded = false;
    g_mainNavDoneOnce = false;
    g_webviewAttempt = 0;
    g_evergreenFallbackPending = false;
    g_revealErrorShown = false;
    g_webviewLaunchStarted = false;
    g_bootPlaceholderShown = false;
    // 工作区就位 + alpha=0：WebView 在不可见状态下绘真 DOM；超时仍未见则强制露出 sky 占位
    if (g_mainWantShow) {
        SetShellCloaked(true);
        SetHwndClickThroughInvisible(g_hwnd, true);
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_hwnd);
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }
    StartupTrace("window shown/cloaked");
    EnsureTrayIcon();
    StartupTrace(g_trayActive ? "tray ok" : "tray failed");
    if (!g_trayActive) {
        // 托盘失败时立刻露出主窗，避免整进程「隐身」
        ShowBootPlaceholderIfNeeded();
    }
    if (g_mainWantShow) {
        SetTimer(g_hwnd, kBootVisibleTimerId, 2500, nullptr);
        SetTimer(g_hwnd, kMainRevealTimerId, 12000, nullptr);
    }
    InitWebView();
    StartupTrace("enter message loop");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_DISPLAYCHANGE) {
            if (HWND eng = qst::engine::Hwnd()) {
                PostMessageW(eng, WM_APP_UI_SCALE_SYNC, 0, 1);
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RemoveTrayIcon();
    qst::engine::Shutdown();
    CoUninitialize();
    if (g_mutex) CloseHandle(g_mutex);
    // TerminateProcess 会跳过 atexit；HID/LL 收尾已在 engine::Shutdown→TeardownNow 完成。
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(msg.wParam));
    return static_cast<int>(msg.wParam);
}
