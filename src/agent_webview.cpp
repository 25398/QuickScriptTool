// ──────────────────────────────────────────────────────────────────
// agent_webview.cpp — WebView2 隐藏渲染抓取
// 用 App 自带的 WebView2 打开隐藏页面（1x1 屏幕外窗口），等 JS 渲染完成后
// 取 document.title + document.body.innerText，覆盖普通 GET 抓不到的动态页。
// 线程模型：调用线程做 STA + 消息泵（工具在后台线程执行，自带消息循环）。
// ──────────────────────────────────────────────────────────────────
#include "agent_webview.h"

#include "agent_web.h"
#include "utils.h"

#include <windows.h>
#include <objbase.h>
#include <wrl.h>
#include <WebView2.h>

#include <chrono>
#include <memory>
#include <string>

#include <chrono>
#include <memory>
#include <string>

using namespace Microsoft::WRL;

namespace {

HWND CreateHiddenHostWindow() {
    const wchar_t kClass[] = L"QstWebFetchHiddenHost";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    return CreateWindowExW(0, kClass, L"", WS_POPUP,
        -32000, -32000, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
}

struct FetchState {
    std::wstring result;
    std::wstring error;
    bool done = false;
    ComPtr<ICoreWebView2Controller> controller;
};

// ExecuteScriptAsync 返回的是 JSON 字符串字面量（带引号与转义），解码为真实文本。
std::wstring DecodeJsonString(const std::wstring& json) {
    std::wstring s = json;
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"')
        s = s.substr(1, s.size() - 2);
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        const wchar_t c = s[++i];
        switch (c) {
            case L'n': out.push_back(L'\n'); break;
            case L'r': out.push_back(L'\r'); break;
            case L't': out.push_back(L'\t'); break;
            case L'"': out.push_back(L'"'); break;
            case L'\\': out.push_back(L'\\'); break;
            case L'/': out.push_back(L'/'); break;
            case L'u': {
                if (i + 4 < s.size()) {
                    unsigned code = 0;
                    for (int k = 1; k <= 4; ++k) {
                        const wchar_t h = s[i + k];
                        code <<= 4;
                        if (h >= L'0' && h <= L'9') code |= (h - L'0');
                        else if (h >= L'a' && h <= L'f') code |= (h - L'a' + 10);
                        else if (h >= L'A' && h <= L'F') code |= (h - L'A' + 10);
                    }
                    i += 4;
                    out.push_back(static_cast<wchar_t>(code));
                }
                break;
            }
            default: out.push_back(c); break;
        }
    }
    return out;
}

}  // namespace

bool FetchWebPageRendered(const std::wstring& url, std::wstring& outText,
                          std::wstring& err, int timeoutMs) {
    outText.clear();
    if (AgentFetchUrlDestinationBlocked(url, err)) return false;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) {
        err = L"COM 初始化失败";
        return false;
    }
    const bool coOwned = SUCCEEDED(co);

    HWND host = CreateHiddenHostWindow();
    if (!host) {
        err = L"创建隐藏窗口失败";
        if (coOwned) CoUninitialize();
        return false;
    }

    // 固定运行时优先；缺失则退回系统 Evergreen
    const std::wstring fixedFolder = AppDir() + L"\\WebView2Fixed";
    std::wstring browserFolder;
    if (GetFileAttributesW((fixedFolder + L"\\msedgewebview2.exe").c_str())
            != INVALID_FILE_ATTRIBUTES) {
        browserFolder = fixedFolder;
    }
    // 独立用户数据目录，避免与主壳 WebView2UserData 冲突
    // Program Files 下必须用 LocalAppData，否则 Edge 沙箱无法写盘
    const std::wstring userData = WebView2FetchDataDir();

    auto state = std::make_shared<FetchState>();
    const HRESULT envHr = CreateCoreWebView2EnvironmentWithOptions(
        browserFolder.empty() ? nullptr : browserFolder.c_str(),
        userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [host, url, state](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    state->error = L"WebView2 环境创建失败";
                    state->done = true;
                    return result;
                }
                return env->CreateCoreWebView2Controller(host,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [url, state](HRESULT result2,
                                     ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result2) || !controller) {
                                state->error = L"WebView2 控制器创建失败";
                                state->done = true;
                                return result2;
                            }
                            state->controller = controller;
                            ComPtr<ICoreWebView2> webview;
                            if (FAILED(controller->get_CoreWebView2(&webview)) || !webview) {
                                state->error = L"获取 CoreWebView2 失败";
                                state->done = true;
                                return E_FAIL;
                            }
                            RECT r{0, 0, 1, 1};
                            controller->put_Bounds(r);
                            controller->put_IsVisible(TRUE);
                            EventRegistrationToken navStartToken{};
                            webview->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [state](ICoreWebView2*,
                                        ICoreWebView2NavigationStartingEventArgs* args)
                                        -> HRESULT {
                                        if (!args) return S_OK;
                                        LPWSTR uri = nullptr;
                                        if (FAILED(args->get_Uri(&uri)) || !uri) return S_OK;
                                        std::wstring blockErr;
                                        const bool blocked =
                                            AgentFetchUrlDestinationBlocked(uri, blockErr);
                                        CoTaskMemFree(uri);
                                        if (blocked) {
                                            args->put_Cancel(TRUE);
                                            state->error = blockErr;
                                            state->done = true;
                                        }
                                        return S_OK;
                                    }).Get(), &navStartToken);
                            EventRegistrationToken navToken{};
                            webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [webview, state](
                                        ICoreWebView2*,
                                        ICoreWebView2NavigationCompletedEventArgs* args)
                                        -> HRESULT {
                                        BOOL ok = FALSE;
                                        if (args) args->get_IsSuccess(&ok);
                                        if (!ok) {
                                            state->error = L"页面加载失败（渲染器）";
                                            state->done = true;
                                            return S_OK;
                                        }
                                        webview->ExecuteScript(
                                            L"(()=>{const t=document.title||'';"
                                            L"const b=document.body?document.body.innerText:'';"
                                            L"return '标题：'+t+'\\n\\n'+b;})()",
                                            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                                                [state](HRESULT hr, LPCWSTR json) -> HRESULT {
                                                    if (FAILED(hr) || !json) {
                                                        state->error = L"提取页面文本失败";
                                                    } else {
                                                        state->result =
                                                            DecodeJsonString(std::wstring(json));
                                                    }
                                                    state->done = true;
                                                    return S_OK;
                                                }).Get());
                                        return S_OK;
                                    }).Get(), &navToken);
                            return webview->Navigate(url.c_str());
                        }).Get());
            }).Get());

    if (FAILED(envHr)) {
        err = L"WebView2 环境创建失败";
        DestroyWindow(host);
        if (coOwned) CoUninitialize();
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max(5000, timeoutMs));
    while (!state->done) {
        if (std::chrono::steady_clock::now() >= deadline) {
            err = L"渲染超时（" + std::to_wstring(std::max(5, timeoutMs / 1000)) + L"s）";
            if (state->controller) state->controller->Close();
            DestroyWindow(host);
            if (coOwned) CoUninitialize();
            return false;
        }
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }

    if (state->controller) state->controller->Close();
    DestroyWindow(host);
    if (coOwned) CoUninitialize();
    if (!state->error.empty()) {
        err = state->error;
        return false;
    }
    outText = state->result;
    return !outText.empty();
}
