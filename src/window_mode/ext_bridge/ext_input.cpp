#include "ext_input.h"

#include "ext_bridge_server.h"
#include "window_mode/background_window_input.h"
#include "window_mode/virtual_desktop_accessor.h"
#include "window_mode/window_capture.h"
#include "window_mode/window_mode_log.h"
#include "window_mode/window_mode_types.h"
#include "window_mode/window_target.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

// strcmp used for vision/screenshot command fallback

namespace windowmode {
namespace {

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
    std::string text;
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

}  // namespace

ExtInputSession::~ExtInputSession() {
    Disconnect();
}

void ExtInputSession::Disconnect() {
    // 脚本结束必须主动 detach，否则 Edge 会一直显示「已开始调试此浏览器」。
    if (attached_) {
        auto& bridge = ExtBridgeServer::Instance();
        // Abort 中勿再 Request：会被「已取消」挡住；EndRun 已 ClearAbort 后再 Disconnect。
        if (bridge.IsExtensionConnected() && !bridge.IsAborted()) {
            std::string ignore;
            std::wstring ignoreErr;
            bridge.Request("detach", "", ignore, ignoreErr, 800);
        }
    }
    attached_ = false;
    attachedTabId_ = 0;
    boundTopHwnd_ = nullptr;
    // 用赋值重置，避免对半销毁/布局错位对象调用 clear() 时空指针写入。
    titleHint_ = {};
    extVersion_ = {};
    lastShotVia_ = {};
    lastVisionJpegHash_ = 0;
    iframeCssX_ = iframeCssY_ = iframeCssW_ = iframeCssH_ = 0;
    pageCssW_ = pageCssH_ = 0;
    contentW_ = contentH_ = 0;
    surfaceW_ = surfaceH_ = 0;
    dpr_ = 1.5;
}

namespace {

int ExtVersionCode(const std::string& ver) {
    int a = 0, b = 0, c = 0;
    if (sscanf_s(ver.c_str(), "%d.%d.%d", &a, &b, &c) < 1) return 0;
    return a * 10000 + b * 100 + c;
}

}  // namespace

bool ExtInputSession::SupportsSafeExtScreenshot() const {
    // 1.0.17 screenshot 会断 WS；1.0.19+ 为 iframe canvas 路径。
    // 版本尚未写入时按新扩展处理（attach 校验截图）。
    if (extVersion_.empty()) return true;
    return ExtVersionCode(extVersion_) >= ExtVersionCode("1.0.19");
}

bool ExtInputSession::SupportsExtVision() const {
    // 1.0.21+：vision 协议 + lifecycle wake；宿主 CDP 找图只走扩展、禁止 Win32 展开。
    if (extVersion_.empty()) return true;
    return ExtVersionCode(extVersion_) >= ExtVersionCode("1.0.21");
}

bool ExtInputSession::SupportsStableBridgeApi() const {
    // 1.1.15+：vision=CDP 截图经 HTTP /qst/shot；1.1.16+ 含 lifecycle wake。
    if (extVersion_.empty()) return false;
    return ExtVersionCode(extVersion_) >= ExtVersionCode("1.1.15");
}

bool ExtInputSession::HasValidIframeLayout() const {
    return iframeCssW_ >= 64 && iframeCssH_ >= 64;
}

void ExtInputSession::ApplyLayoutFields(int ix, int iy, int iw, int ih, int cw, int ch, int pw, int ph) {
    if (iw > 0 && ih > 0) {
        iframeCssX_ = ix;
        iframeCssY_ = iy;
        iframeCssW_ = iw;
        iframeCssH_ = ih;
    }
    if (cw > 0 && ch > 0) {
        contentW_ = cw;
        contentH_ = ch;
    }
    if (pw > 0 && ph > 0) {
        pageCssW_ = pw;
        pageCssH_ = ph;
    }
    // attach 漏 pageCss 或旧扩展：用 iframe 外接矩形兜底，避免 MapCanvas 整窗拉伸点偏。
    if ((pageCssW_ <= 0 || pageCssH_ <= 0) && iframeCssW_ >= 64 && iframeCssH_ >= 64) {
        pageCssW_ = std::max(pageCssW_, iframeCssX_ + iframeCssW_);
        pageCssH_ = std::max(pageCssH_, iframeCssY_ + iframeCssH_);
    }
}

std::string ExtInputSession::AppendSurfaceFields(std::string paramsJson) const {
    if (surfaceW_ <= 0 || surfaceH_ <= 0) return paramsJson;
    if (paramsJson.empty()) paramsJson = "{}";
    if (paramsJson.back() != '}') return paramsJson;
    paramsJson.pop_back();
    if (paramsJson.size() > 1) paramsJson.push_back(',');
    paramsJson += "\"surfaceW\":";
    paramsJson += std::to_string(surfaceW_);
    paramsJson += ",\"surfaceH\":";
    paramsJson += std::to_string(surfaceH_);
    paramsJson.push_back('}');
    return paramsJson;
}

/// 只看顶层第一个 "ok"（勿被 dom.ok 等嵌套字段干扰）。
bool ResultLooksOk(const std::string& result) {
    const size_t p = result.find("\"ok\"");
    if (p == std::string::npos) return false;
    const size_t colon = result.find(':', p + 4);
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < result.size() && (result[i] == ' ' || result[i] == '\t')) ++i;
    return result.compare(i, 4, "true") == 0;
}

double ExtractJsonDouble(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t p = json.find(needle);
    if (p == std::string::npos) return 0.0;
    const size_t colon = json.find(':', p + needle.size());
    if (colon == std::string::npos) return 0.0;
    return atof(json.c_str() + colon + 1);
}

std::string ExtractJsonStringLocal(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t p = json.find(needle);
    if (p == std::string::npos) return {};
    const size_t colon = json.find(':', p + needle.size());
    if (colon == std::string::npos) return {};
    size_t a = json.find('"', colon + 1);
    if (a == std::string::npos) return {};
    ++a;
    std::string out;
    for (; a < json.size(); ++a) {
        if (json[a] == '\\' && a + 1 < json.size()) {
            out.push_back(json[a + 1]);
            ++a;
            continue;
        }
        if (json[a] == '"') break;
        out.push_back(json[a]);
    }
    return out;
}

std::wstring Utf8SnippetToWide(const std::string& u);

bool ExtInputSession::IsDetachError(const std::string& result, const std::wstring& err) {
    if (err.find(L"尚未 attach") != std::wstring::npos) return true;
    if (err.find(L"NO_TAB") != std::wstring::npos) return true;
    if (err.find(L"扩展桥未 attach") != std::wstring::npos) return true;
    const std::string code = ExtractJsonStringLocal(result, "error");
    if (code == "NO_TAB") return true;
    const std::string msg = ExtractJsonStringLocal(result, "message");
    if (msg.find("尚未 attach") != std::string::npos) return true;
    if (msg.find("Debugger is not attached") != std::string::npos) return true;
    if (msg.find("Detached while") != std::string::npos) return true;
    return false;
}

bool ExtInputSession::RecoverAttach(std::wstring& err) {
    attached_ = false;
    if (titleHint_.empty() && attachedTabId_ <= 0) {
        err = L"扩展 debugger 已脱落且无标题可重绑";
        return false;
    }
    WindowModeLog(L"[窗口模式] 扩展 debugger 已脱落，正在重新 attach…");
    return EnsureReady(titleHint_, boundTopHwnd_, err);
}

bool ExtInputSession::CallCdp(const std::string& method, const std::string& paramsJson,
    std::wstring& err, bool allowReattach) {
    if (!attached_) {
        if (!allowReattach || !RecoverAttach(err)) {
            if (err.empty()) err = L"扩展桥未 attach";
            return false;
        }
    }
    if (ExtBridgeServer::Instance().IsAborted()) {
        err = L"已取消";
        return false;
    }
    std::string extra = "\"method\":\"" + method + "\",\"params\":";
    extra += paramsJson.empty() ? "{}" : paramsJson;
    std::string result;
    if (!ExtBridgeServer::Instance().Request("cdp", extra, result, err, 3000)) {
        if (allowReattach && IsDetachError(result, err) && RecoverAttach(err)) {
            return CallCdp(method, paramsJson, err, false);
        }
        const std::wstring methodW(method.begin(), method.end());
        WindowModeLogf(L"[窗口模式] 扩展 CDP 失败 %s: %s", methodW.c_str(), err.c_str());
        return false;
    }
    if (!ResultLooksOk(result)) {
        const std::string msg = ExtractJsonStringLocal(result, "message");
        err = msg.empty() ? L"扩展 CDP 返回失败" : Utf8SnippetToWide(msg);
        if (allowReattach && IsDetachError(result, err) && RecoverAttach(err)) {
            return CallCdp(method, paramsJson, err, false);
        }
        const std::wstring methodW(method.begin(), method.end());
        WindowModeLogf(L"[窗口模式] 扩展 CDP 失败 %s: %s", methodW.c_str(), err.c_str());
        return false;
    }
    return true;
}

std::wstring CleanBrowserTitleHint(std::wstring title) {
    for (;;) {
        const size_t pos = title.rfind(L" - ");
        if (pos == std::wstring::npos) break;
        const std::wstring tail = title.substr(pos + 3);
        const bool drop =
            tail.find(L"Microsoft") != std::wstring::npos
            || tail.find(L"Edge") != std::wstring::npos
            || tail.find(L"Chrome") != std::wstring::npos
            || tail.find(L"用户配置") != std::wstring::npos
            || tail.find(L"Profile") != std::wstring::npos
            || tail.find(L"InPrivate") != std::wstring::npos;
        if (!drop) break;
        title.resize(pos);
        while (!title.empty() && (title.back() == L' ' || title.back() == L'\t')) title.pop_back();
    }
    return title;
}

std::wstring Utf8SnippetToWide(const std::string& u) {
    if (u.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()),
        nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), out.data(), n);
    return out;
}

cv::Mat HbitmapToGrayMatLocal(HBITMAP bmp) {
    if (!bmp) return {};
    BITMAP bm{};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return {};
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> px(static_cast<size_t>(bm.bmWidth) * bm.bmHeight * 4);
    HDC dc = GetDC(nullptr);
    if (!dc) return {};
    const int lines = GetDIBits(dc, bmp, 0, bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines <= 0) return {};
    cv::Mat bgra(bm.bmHeight, bm.bmWidth, CV_8UC4, px.data());
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    return gray.clone();
}

/// Win32 客户区（含浏览器壳）与扩展 canvas 比对：模板匹配峰值 0~1。
double CorrelateWin32ToExtShot(HBITMAP win32Bmp, HBITMAP extBmp) {
    cv::Mat screen = HbitmapToGrayMatLocal(win32Bmp);
    cv::Mat templ = HbitmapToGrayMatLocal(extBmp);
    if (screen.empty() || templ.empty()) return -1.0;
    // 去掉顶部标签栏/地址栏，保留游戏区。
    const int topCrop = std::max(8, screen.rows / 8);
    if (screen.rows > topCrop + 40) {
        screen = screen(cv::Rect(0, topCrop, screen.cols, screen.rows - topCrop)).clone();
    }
    const int tw = std::clamp(screen.cols * 2 / 3, 96, 480);
    const int th = std::clamp(screen.rows * 2 / 3, 64, 320);
    cv::Mat tSmall;
    cv::resize(templ, tSmall, cv::Size(tw, th), 0, 0, cv::INTER_AREA);
    if (tSmall.cols >= screen.cols || tSmall.rows >= screen.rows) {
        cv::Mat a, b;
        cv::resize(screen, a, cv::Size(160, 90), 0, 0, cv::INTER_AREA);
        cv::resize(templ, b, cv::Size(160, 90), 0, 0, cv::INTER_AREA);
        cv::Mat aa, bb;
        a.convertTo(aa, CV_32F);
        b.convertTo(bb, CV_32F);
        const double n1 = cv::norm(aa);
        const double n2 = cv::norm(bb);
        if (n1 < 1e-3 || n2 < 1e-3) return 0.0;
        return aa.dot(bb) / (n1 * n2);
    }
    cv::Mat result;
    cv::matchTemplate(screen, tSmall, result, cv::TM_CCOEFF_NORMED);
    double minV = 0, maxV = 0;
    cv::minMaxLoc(result, &minV, &maxV);
    return maxV;
}

std::vector<int> ParseJsonIntArray(const std::string& json, const char* key) {
    std::vector<int> out;
    const std::string needle = std::string("\"") + key + "\"";
    const size_t p = json.find(needle);
    if (p == std::string::npos) return out;
    const size_t lb = json.find('[', p + needle.size());
    if (lb == std::string::npos) return out;
    const size_t rb = json.find(']', lb + 1);
    if (rb == std::string::npos) return out;
    size_t i = lb + 1;
    while (i < rb) {
        while (i < rb && (json[i] == ' ' || json[i] == ',' || json[i] == '\n' || json[i] == '\r')) {
            ++i;
        }
        if (i >= rb) break;
        if (json[i] == '-' || (json[i] >= '0' && json[i] <= '9')) {
            out.push_back(atoi(json.c_str() + i));
            while (i < rb && json[i] != ',' && json[i] != ']') ++i;
            continue;
        }
        // 跳过字符串元素
        if (json[i] == '"') {
            ++i;
            while (i < rb && json[i] != '"') {
                if (json[i] == '\\' && i + 1 < rb) i += 2;
                else ++i;
            }
            if (i < rb && json[i] == '"') ++i;
            continue;
        }
        ++i;
    }
    return out;
}

bool ExtInputSession::EnsureReady(const std::wstring& titleHint, std::wstring& err) {
    return EnsureReady(titleHint, nullptr, err);
}

bool ExtInputSession::EnsureReady(const std::wstring& titleHint, HWND boundTop,
    std::wstring& err) {
    titleHint_ = CleanBrowserTitleHint(titleHint);
    boundTop = TopLevelTargetWindow(boundTop);
    boundTopHwnd_ = (boundTop && IsWindow(boundTop)) ? boundTop : nullptr;

    auto& bridge = ExtBridgeServer::Instance();
    if (!bridge.IsRunning() && !bridge.Start(err)) {
        return false;
    }

    const std::string hintUtf8 = WideToUtf8(titleHint_);
    std::string baseExtra = "\"titleHint\":\"" + JsonEscape(hintUtf8) + "\"";
    if (boundTopHwnd_) {
        RECT rc{};
        if (GetWindowRect(boundTopHwnd_, &rc)) {
            char geom[160];
            std::snprintf(geom, sizeof(geom),
                ",\"boundLeft\":%ld,\"boundTop\":%ld,\"boundRight\":%ld,\"boundBottom\":%ld",
                rc.left, rc.top, rc.right, rc.bottom);
            baseExtra += geom;
        }
        // 截图校验为主；不再用 preferUnfocused（多桌面同坐标时会绑错同类窗）。
        baseExtra += ",\"preferUnfocused\":false";
    }

    HBITMAP refBmp = nullptr;
    if (boundTopHwnd_) {
        bool skipWin32Ref = true;
        // 脚本猫模型：永远禁止 Win32 PrintWindow 参考帧（会切屏）；只用标题戳记对页。
        WindowModeLog(
            L"[窗口模式] 扩展 attach：跳过 Win32 PrintWindow 参考帧（宏桌面停放+扩展操作）");
        (void)skipWin32Ref;
    }

    auto finishFromResult = [&](const std::string& result, int tabId) -> bool {
        return FinishAttachFromResult(result, tabId, err);
    };

    std::wstring lastErr;
    constexpr int kRounds = 12;
    for (int round = 0; round < kRounds; ++round) {
        if (bridge.IsAborted()) {
            if (refBmp) DeleteObject(refBmp);
            err = L"已取消";
            return false;
        }
        if (!bridge.IsExtensionConnected()) {
            if (round == 0) {
                WindowModeLog(L"[窗口模式] 等待配套扩展连接本机桥…");
            }
            std::wstring waitErr;
            if (!bridge.WaitForExtension(2500, waitErr)) {
                if (bridge.IsAborted() || waitErr == L"已取消") {
                    if (refBmp) DeleteObject(refBmp);
                    err = L"已取消";
                    return false;
                }
                lastErr = L"NO_EXTENSION";
                continue;
            }
        }

        WindowModeLogf(L"[窗口模式] 扩展桥 attach 标题提示: %s（已连接扩展 %d 路，第 %d 次）",
            titleHint_.c_str(), bridge.ExtensionClientCount(), round + 1);

        std::vector<int> tabIds;
        if (attachedTabId_ > 0) {
            tabIds.push_back(attachedTabId_);
        } else {
            std::string listResult;
            std::wstring listErr;
            if (bridge.Request("listPages", baseExtra, listResult, listErr, 4000)) {
                tabIds = ParseJsonIntArray(listResult, "tabIds");
                WindowModeLogf(L"[窗口模式] 扩展桥 listPages: %d 个标题候选",
                    static_cast<int>(tabIds.size()));
            } else if (listErr.find(L"UNKNOWN") == std::wstring::npos) {
                WindowModeLogf(L"[窗口模式] listPages 不可用: %s，回退标题 attach", listErr.c_str());
            }
        }

        if (tabIds.empty()) {
            tabIds.push_back(0); // 0 = 仅按标题
        }

        // 只要有绑定 HWND，就必须用「标题戳记」校验（截图在 Cloak/它桌常失败，不能当依据）。
        const bool needVerify = boundTopHwnd_ != nullptr;
        double bestScore = -1.0;
        int bestTabId = 0;
        std::string bestResult;
        int tried = 0;

        for (int tabId : tabIds) {
            if (tried >= 6) break;
            if (bridge.IsAborted()) break;
            ++tried;

            std::string extra = baseExtra;
            if (tabId > 0) {
                extra += ",\"tabId\":" + std::to_string(tabId);
            }

            // 标题戳记：在扩展 attach 壳页阶段写入，再读绑定 HWND 标题核对。
            const unsigned stamp = GetTickCount() ^ (static_cast<unsigned>(tabId) * 2654435761u);
            char marker[48];
            std::snprintf(marker, sizeof(marker), "QST%08X:", stamp);
            const std::wstring markerW = Utf8SnippetToWide(marker);
            if (needVerify) {
                extra += ",\"stampMarker\":\"";
                extra += JsonEscape(marker);
                extra += "\"";
            }

            std::string result;
            std::wstring reqErr;
            if (!bridge.Request("attach", extra, result, reqErr, 8000)) {
                lastErr = reqErr.empty() ? L"attach 失败" : reqErr;
                if (bridge.IsAborted() || reqErr == L"已取消") {
                    if (refBmp) DeleteObject(refBmp);
                    err = L"已取消";
                    return false;
                }
                WindowModeLogf(L"[窗口模式] 扩展桥 attach 失败(tab=%d): %s", tabId, lastErr.c_str());
                continue;
            }

            attached_ = true;
            {
                const std::string needle = "\"version\"";
                const size_t p = result.find(needle);
                if (p != std::string::npos) {
                    const size_t colon = result.find(':', p + needle.size());
                    size_t a = (colon == std::string::npos) ? std::string::npos
                        : result.find('"', colon + 1);
                    if (a != std::string::npos) {
                        ++a;
                        std::string ver;
                        for (; a < result.size() && result[a] != '"'; ++a) {
                            if (result[a] == '\\' && a + 1 < result.size()) {
                                ver.push_back(result[++a]);
                            } else {
                                ver.push_back(result[a]);
                            }
                        }
                        if (!ver.empty()) extVersion_ = ver;
                    }
                }
                if (extVersion_.empty()) extVersion_ = "1.1.5";
            }

            if (!needVerify) {
                if (refBmp) DeleteObject(refBmp);
                return finishFromResult(result, tabId);
            }

            Sleep(180);
            wchar_t hwndTitle[512]{};
            GetWindowTextW(boundTopHwnd_, hwndTitle, 512);
            const bool stampedFlag = result.find("\"stamped\":true") != std::string::npos
                || result.find("\"stamped\": true") != std::string::npos;
            const bool pinOk = stampedFlag && wcsstr(hwndTitle, markerW.c_str()) != nullptr;

            // 尽量清掉戳记（挂在 iframe 时可能失败，无伤大雅）。
            {
                std::string undo =
                    "(()=>{try{const t=String(document.title||\"\");"
                    "const m=t.match(/^QST[0-9A-Fa-f]{8}:/);"
                    "if(m)document.title=t.slice(m[0].length);"
                    "}catch(e){}return true;})()";
                std::string undoParams = "{\"expression\":\"";
                for (char c : undo) {
                    if (c == '\\' || c == '"') undoParams.push_back('\\');
                    undoParams.push_back(c);
                }
                undoParams += "\",\"returnByValue\":true}";
                std::wstring ignoreErr;
                CallCdp("Runtime.evaluate", undoParams, ignoreErr, false);
            }

            double score = pinOk ? 1.0 : -1.0;
            if (!pinOk && refBmp) {
                HBITMAP shot = nullptr;
                int sw = 0, sh = 0;
                std::wstring shotErr;
                if (CaptureScreenshot(&shot, &sw, &sh, shotErr) && shot) {
                    score = CorrelateWin32ToExtShot(refBmp, shot);
                    DeleteObject(shot);
                } else {
                    WindowModeLogf(L"[窗口模式] 候选 tab=%d 扩展截图失败: %s",
                        tabId, shotErr.c_str());
                }
            }
            WindowModeLogf(L"[窗口模式] 候选 tab=%d 标题钉窗=%d stamped=%d 相似度=%.3f",
                tabId, pinOk ? 1 : 0, stampedFlag ? 1 : 0, score);

            if (pinOk) {
                if (refBmp) DeleteObject(refBmp);
                WindowModeLogf(L"[窗口模式] 扩展桥选定 tab=%d（标题戳记命中绑定窗）", tabId);
                return finishFromResult(result, tabId);
            }

            if (bestResult.empty() || score > bestScore) {
                bestScore = score;
                bestTabId = tabId;
                bestResult = result;
            }

            attached_ = false;
            std::string ignore;
            std::wstring ignoreErr;
            bridge.Request("detach", "", ignore, ignoreErr, 2000);
        }

        if (needVerify) {
            if (refBmp) DeleteObject(refBmp);
            err = L"无法将扩展标签对应到绑定窗口（标题戳记未命中）。"
                  L"请确认目标游戏标签在绑定的 Edge 窗内，并关掉其它同名游戏窗后重试。";
            WindowModeLog(L"[窗口模式] 扩展桥 attach 失败: 标题戳记未命中绑定 HWND");
            if (bestTabId > 0) {
                WindowModeLogf(L"[窗口模式] 曾尝试最佳 tab=%d 相似度=%.3f（已拒绝）",
                    bestTabId, bestScore);
            }
            return false;
        }

        std::string result;
        std::wstring reqErr;
        if (bridge.Request("attach", baseExtra, result, reqErr, 8000)) {
            if (refBmp) DeleteObject(refBmp);
            return finishFromResult(result, 0);
        }
        lastErr = reqErr.empty() ? L"attach 失败" : reqErr;
        Sleep(200);
    }

    if (refBmp) DeleteObject(refBmp);
    err = lastErr.empty() ? L"NO_EXTENSION" : lastErr;
    WindowModeLog(L"[窗口模式] 提示: 请重载扩展到 v1.1.4，打开选项页点「重新连接」。");
    return false;
}

bool ExtInputSession::FinishAttachFromResult(const std::string& result, int tabId, std::wstring& err) {
    attached_ = true;
    if (tabId > 0) attachedTabId_ = tabId;
    auto extractStr = [&](const char* key) -> std::string {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t p = result.find(needle);
        if (p == std::string::npos) return {};
        const size_t colon = result.find(':', p + needle.size());
        if (colon == std::string::npos) return {};
        size_t a = result.find('"', colon + 1);
        if (a == std::string::npos) return {};
        ++a;
        std::string out;
        for (; a < result.size(); ++a) {
            if (result[a] == '\\' && a + 1 < result.size()) {
                out.push_back(result[a + 1]);
                ++a;
                continue;
            }
            if (result[a] == '"') break;
            out.push_back(result[a]);
        }
        return out;
    };
    auto extractNum = [&](const char* key) -> int {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t p = result.find(needle);
        if (p == std::string::npos) return 0;
        const size_t colon = result.find(':', p + needle.size());
        if (colon == std::string::npos) return 0;
        return atoi(result.c_str() + colon + 1);
    };
    if (attachedTabId_ <= 0) {
        const int tid = extractNum("tabId");
        if (tid > 0) attachedTabId_ = tid;
    }
    const std::string attachedTitle = extractStr("title");
    const std::string ver = extractStr("version");
    extVersion_ = ver;
    if (!attachedTitle.empty()) {
        const std::wstring titleW = Utf8SnippetToWide(attachedTitle);
        const std::wstring verW = ver.empty() ? L"?" : Utf8SnippetToWide(ver);
        WindowModeLogf(L"[窗口模式] 扩展桥已 attach 标签: %s（扩展 v%s）",
            titleW.c_str(), verW.c_str());
    } else {
        WindowModeLog(L"[窗口模式] 扩展桥已连接（chrome.debugger）");
    }
    {
        const std::string pickNote = extractStr("pickNote");
        if (!pickNote.empty()) {
            WindowModeLogf(L"[窗口模式] 扩展桥选页: %s",
                Utf8SnippetToWide(pickNote).c_str());
        }
    }
    if (!SupportsSafeExtScreenshot()) {
        WindowModeLog(L"[窗口模式] 警告: 扩展需 v1.0.21+ 才能扩展视觉找图；"
            L"请在 edge://extensions 对「鼠大侠」点「重新加载」"
            L"（目录 extension\\\\edge 或 build\\\\Release\\\\extension\\\\edge）");
    }
    if (!SupportsStableBridgeApi()) {
        // 仅日志：勿 MessageBox 抢焦点打断键鼠；找图需 1.1.15，输入仍可继续。
        WindowModeLog(L"[窗口模式] ★请重载扩展到 v1.1.43+★（HTTP 截图找图；轻量保活防卡帧）"
            L" edge://extensions → 鼠大侠 → 重新加载（build\\\\Release\\\\extension\\\\edge）");
    }
    const std::string via = extractStr("via");
    const std::string focus = extractStr("focus");
    const std::string url = extractStr("url");
    const std::string note = extractStr("note");
    if (!via.empty() || !focus.empty()) {
        const std::wstring viaW = Utf8SnippetToWide(via.empty() ? "?" : via);
        const std::wstring focusW = Utf8SnippetToWide(focus.empty() ? "?" : focus);
        const std::wstring urlW = Utf8SnippetToWide(url.substr(0, 160));
        WindowModeLogf(L"[窗口模式] 扩展输入目标 via=%s focus=%s url=%s",
            viaW.c_str(), focusW.c_str(), urlW.c_str());
    }
    if (!note.empty()) {
        const std::wstring noteW = Utf8SnippetToWide(note.substr(0, 360));
        WindowModeLogf(L"[窗口模式] 扩展 iframe 探测: %s", noteW.c_str());
    }
    ApplyLayoutFields(
        extractNum("iframeCssX"), extractNum("iframeCssY"),
        extractNum("iframeCssW"), extractNum("iframeCssH"),
        extractNum("contentW"), extractNum("contentH"),
        extractNum("pageCssW"), extractNum("pageCssH"));
    {
        const double dpr = ExtractJsonDouble(result, "dpr");
        if (dpr > 0.1 && dpr < 8.0) dpr_ = dpr;
    }
    if (via == "tab") {
        WindowModeLog(L"[窗口模式] 警告: 仍挂在壳页(tab)。跨域游戏 iframe 未挂上时角色通常无反应。"
            L"请把上面「iframe 探测」整行发我。");
    }
    // 禁止 HoldPreferred：用户进「鼠标宏」不得被踢回。
    err.clear();
    return true;
}

bool ExtInputSession::RefreshLayout(std::wstring& err) {
    if (!attached_) {
        err = L"扩展未 attach";
        return false;
    }
    auto& bridge = ExtBridgeServer::Instance();
    if (bridge.IsAborted()) {
        err = L"已取消";
        return false;
    }
    std::string result;
    if (!bridge.Request("layout", "", result, err, 4000)) {
        return false;
    }
    auto extractNum = [&](const char* key) -> int {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t p = result.find(needle);
        if (p == std::string::npos) return 0;
        const size_t colon = result.find(':', p + needle.size());
        if (colon == std::string::npos) return 0;
        return atoi(result.c_str() + colon + 1);
    };
    const int ix = extractNum("iframeCssX");
    const int iy = extractNum("iframeCssY");
    const int iw = extractNum("iframeCssW");
    const int ih = extractNum("iframeCssH");
    const int cw = extractNum("contentW");
    const int ch = extractNum("contentH");
    const int pw = extractNum("pageCssW");
    const int ph = extractNum("pageCssH");
    ApplyLayoutFields(ix, iy, iw, ih, cw, ch, pw, ph);
    {
        const double dpr = ExtractJsonDouble(result, "dpr");
        if (dpr > 0.1 && dpr < 8.0) dpr_ = dpr;
    }
    WindowModeLogf(
        L"[窗口模式] 扩展布局 iframeCss=(%d,%d) %dx%d content=%dx%d pageCss=%dx%d surface=%dx%d scale≈%.3fx%.3f",
        iframeCssX_, iframeCssY_, iframeCssW_, iframeCssH_, contentW_, contentH_, pageCssW_, pageCssH_,
        surfaceW_, surfaceH_,
        (pageCssW_ > 0 && surfaceW_ > 0) ? (surfaceW_ / static_cast<double>(pageCssW_)) : 0.0,
        (pageCssH_ > 0 && surfaceH_ > 0) ? (surfaceH_ / static_cast<double>(pageCssH_)) : 0.0);
    err.clear();
    return true;
}

bool ExtInputSession::DispatchKeyEventOnly(UINT vk, bool down, std::wstring& err) {
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
    return CallCdp("Input.dispatchKeyEvent", params, err);
}

bool ExtInputSession::KeyEvent(UINT vk, bool down, std::wstring& err) {
    // 勿对 iframe 调 setWebLifecycleState；勿在找图后「重发按住键」（会卡人，已证伪）。
    if (down) {
        std::wstring focusErr;
        CallCdp("Emulation.setFocusEmulationEnabled", "{\"enabled\":true}", focusErr);
    }
    return DispatchKeyEventOnly(vk, down, err);
}

bool ExtInputSession::CallMouse(const char* action, int cx, int cy, MouseButtonType button,
    std::wstring& err, bool allowReattach) {
    if (!attached_) {
        if (!allowReattach || !RecoverAttach(err)) {
            if (err.empty()) err = L"扩展桥未 attach";
            return false;
        }
    }
    auto& bridge = ExtBridgeServer::Instance();
    if (bridge.IsAborted()) {
        err = L"已取消";
        return false;
    }

    std::string extra = "\"action\":\"";
    extra += action ? action : "click";
    extra += "\",\"x\":";
    extra += std::to_string(cx);
    extra += ",\"y\":";
    extra += std::to_string(cy);
    extra += ",\"button\":\"";
    extra += MouseButtonName(button);
    extra += "\"";
    if (surfaceW_ > 0 && surfaceH_ > 0) {
        extra += ",\"surfaceW\":";
        extra += std::to_string(surfaceW_);
        extra += ",\"surfaceH\":";
        extra += std::to_string(surfaceH_);
    }

    std::string result;
    if (!bridge.Request("mouse", extra, result, err, 2500)) {
        if (allowReattach && IsDetachError(result, err) && RecoverAttach(err)) {
            return CallMouse(action, cx, cy, button, err, false);
        }
        WindowModeLogf(L"[窗口模式] 扩展鼠标 %hs 失败 host=(%d,%d): %s",
            action ? action : "?", cx, cy, err.c_str());
        return false;
    }

    // 旧扩展无 mouse 命令时回退到逐条 CDP。
    const std::string errCode = ExtractJsonStringLocal(result, "error");
    if (errCode == "UNKNOWN") {
        WindowModeLog(L"[窗口模式] 扩展无 mouse 命令，回退 CDP（请重载扩展到 v1.0.15）");
        if (std::strcmp(action, "move") == 0) {
            std::string params = "{\"type\":\"mouseMoved\",\"x\":";
            params += std::to_string(cx);
            params += ",\"y\":";
            params += std::to_string(cy);
            params += ",\"button\":\"none\"}";
            return CallCdp("Input.dispatchMouseEvent", AppendSurfaceFields(std::move(params)), err);
        }
        if (std::strcmp(action, "click") == 0) {
            std::string down = "{\"type\":\"mousePressed\",\"x\":";
            down += std::to_string(cx);
            down += ",\"y\":";
            down += std::to_string(cy);
            down += ",\"button\":\"";
            down += MouseButtonName(button);
            down += "\",\"clickCount\":1,\"pointerType\":\"mouse\"}";
            if (!CallCdp("Input.dispatchMouseEvent", AppendSurfaceFields(std::move(down)), err)) {
                return false;
            }
            Sleep(45);
            std::string up = "{\"type\":\"mouseReleased\",\"x\":";
            up += std::to_string(cx);
            up += ",\"y\":";
            up += std::to_string(cy);
            up += ",\"button\":\"";
            up += MouseButtonName(button);
            up += "\",\"clickCount\":1,\"pointerType\":\"mouse\"}";
            return CallCdp("Input.dispatchMouseEvent", AppendSurfaceFields(std::move(up)), err);
        }
        const char* type = (std::strcmp(action, "down") == 0) ? "mousePressed" : "mouseReleased";
        std::string params = "{\"type\":\"";
        params += type;
        params += "\",\"x\":";
        params += std::to_string(cx);
        params += ",\"y\":";
        params += std::to_string(cy);
        params += ",\"button\":\"";
        params += MouseButtonName(button);
        params += "\",\"clickCount\":1,\"pointerType\":\"mouse\"}";
        return CallCdp("Input.dispatchMouseEvent", AppendSurfaceFields(std::move(params)), err);
    }

    if (!ResultLooksOk(result)) {
        const std::string msg = ExtractJsonStringLocal(result, "message");
        err = msg.empty() ? L"扩展鼠标失败" : Utf8SnippetToWide(msg);
        if (allowReattach && IsDetachError(result, err) && RecoverAttach(err)) {
            return CallMouse(action, cx, cy, button, err, false);
        }
        WindowModeLogf(L"[窗口模式] 扩展鼠标 %hs 失败 host=(%d,%d): %s",
            action ? action : "?", cx, cy, err.c_str());
        return false;
    }

    const int mappedX = static_cast<int>(ExtractJsonDouble(result, "mappedX") + 0.5);
    const int mappedY = static_cast<int>(ExtractJsonDouble(result, "mappedY") + 0.5);
    const double scaleX = ExtractJsonDouble(result, "scaleX");
    const double scaleY = ExtractJsonDouble(result, "scaleY");
    std::wstring domHint;
    const size_t domPos = result.find("\"dom\"");
    if (domPos != std::string::npos) {
        const size_t domOk = result.find("\"ok\"", domPos);
        bool domOkTrue = false;
        if (domOk != std::string::npos) {
            const size_t colon = result.find(':', domOk);
            if (colon != std::string::npos) {
                size_t i = colon + 1;
                while (i < result.size() && (result[i] == ' ' || result[i] == '\t')) ++i;
                domOkTrue = result.compare(i, 4, "true") == 0;
            }
        }
        if (!domOkTrue) {
            domHint = L" dom=fail";
        } else {
            const std::string tag = ExtractJsonStringLocal(result, "tag");
            if (!tag.empty()) {
                domHint = L" dom=";
                domHint += Utf8SnippetToWide(tag);
            } else {
                domHint = L" dom=ok";
            }
        }
    }
    const bool touchOk = result.find("\"touch\":true") != std::string::npos
        || result.find("\"touch\": true") != std::string::npos;
    WindowModeLogf(
        L"[窗口模式] 扩展鼠标 %hs surface=(%d,%d) -> iframe=(%d,%d) scale=%.3fx%.3f touch=%d%s",
        action ? action : "?", cx, cy, mappedX, mappedY, scaleX, scaleY,
        touchOk ? 1 : 0, domHint.c_str());
    err.clear();
    return true;
}

bool ExtInputSession::MouseMove(int cx, int cy, std::wstring& err) {
    lastCx_ = cx;
    lastCy_ = cy;
    // 软光标由 Executor 按「输入窗客户区」记录；此处 cx/cy 已是截图表面坐标。
    return CallMouse("move", cx, cy, MouseButtonType::Left, err);
}

bool ExtInputSession::MouseButton(int cx, int cy, MouseButtonType button, bool down,
    std::wstring& err) {
    lastCx_ = cx;
    lastCy_ = cy;
    return CallMouse(down ? "down" : "up", cx, cy, button, err);
}

bool ExtInputSession::MouseClick(int cx, int cy, MouseButtonType button, std::wstring& err) {
    lastCx_ = cx;
    lastCy_ = cy;
    return CallMouse("click", cx, cy, button, err);
}

bool ExtInputSession::Scroll(int cx, int cy, int steps, bool vertical, bool positive,
    std::wstring& err) {
    lastCx_ = cx;
    lastCy_ = cy;
    const int delta = (positive ? 1 : -1) * (std::max)(1, steps) * 100;
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
    return CallCdp("Input.dispatchMouseEvent", params, err);
}

bool ExtInputSession::InsertText(const std::wstring& text, std::wstring& err) {
    if (text.empty()) return true;
    const std::string utf8 = WideToUtf8(text);
    std::string params = "{\"text\":\"";
    params += JsonEscape(utf8);
    params += "\"}";
    return CallCdp("Input.insertText", params, err);
}

namespace {

bool Base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    static const int kDec[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };
    out.clear();
    out.reserve(in.size() * 3 / 4 + 4);
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || c > 127 || kDec[c] < 0) {
            if (c == '=') break;
            continue;
        }
        val = (val << 6) + kDec[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return !out.empty();
}

HBITMAP BgrMatToHBitmapLocal(const cv::Mat& bgr) {
    if (bgr.empty()) return nullptr;
    cv::Mat bgra;
    cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bgra.cols;
    bi.bmiHeader.biHeight = -bgra.rows;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!bmp || !bits) return nullptr;
    std::memcpy(bits, bgra.data, static_cast<size_t>(bgra.cols) * bgra.rows * 4);
    return bmp;
}

}  // namespace

bool ExtInputSession::CaptureScreenshot(HBITMAP* outBmp, int* outW, int* outH, std::wstring& err) {
    if (!outBmp) {
        err = L"无效输出";
        return false;
    }
    *outBmp = nullptr;
    if (outW) *outW = 0;
    if (outH) *outH = 0;

    // 禁止找图热路径 ShowWindow 展开：同进程 Edge 会切到「鼠标宏」（1.1.34 已证伪）。
    // 出帧只靠扩展 mirror + 可见性伪装/指纹；Win32 只停放。

    if (!attached_) {
        if (!RecoverAttach(err)) {
            if (err.empty()) err = L"扩展桥未 attach";
            return false;
        }
    }
    if (!SupportsSafeExtScreenshot()) {
        err = L"扩展版本过旧(需 v1.0.21+)，跳过 screenshot 以免断桥";
        return false;
    }
    if (ExtBridgeServer::Instance().IsAborted()) {
        err = L"已取消";
        return false;
    }

    auto decodeJpegBytesToBmp = [&](const std::vector<uint8_t>& bytes) -> bool {
        if (bytes.size() < 64) {
            err = L"扩展截图无 data";
            return false;
        }
        const cv::Mat img = cv::imdecode(bytes, cv::IMREAD_COLOR);
        if (img.empty()) {
            err = L"扩展截图解码失败";
            return false;
        }
        HBITMAP bmp = BgrMatToHBitmapLocal(img);
        if (!bmp) {
            err = L"扩展截图转位图失败";
            return false;
        }
        *outBmp = bmp;
        if (outW) *outW = img.cols;
        if (outH) *outH = img.rows;
        err.clear();
        return true;
    };

    auto decodeB64ToBmp = [&](const std::string& b64) -> bool {
        if (b64.empty()) {
            err = L"扩展截图无 data";
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!Base64Decode(b64, bytes)) {
            err = L"扩展截图 base64 解码失败";
            return false;
        }
        return decodeJpegBytesToBmp(bytes);
    };

    auto& bridge = ExtBridgeServer::Instance();
    std::string result;
    std::wstring reqErr;

    // 1.1.15：vision 经 HTTP /qst/shot 回传 JPEG，WS 仅小 JSON（不断桥）。
    const char* cmd = SupportsExtVision() ? "vision" : "screenshot";
    if (bridge.IsAborted()) {
        err = L"已取消";
        return false;
    }
    if (!bridge.IsExtensionConnected()) {
        err = L"NO_EXTENSION";
        return false;
    }

    bridge.ClearLastShotJpeg();
    result.clear();
    reqErr.clear();
    // 脚本猫模型：找图纯扩展 HTTP，禁止 Cloak / pin 线程 / HoldView / Correct（零 VDA）。
    // 1.1.33+：mirror-only（双路径 viewerHidden/pageclip 已证伪）。
    // iconic/屏外停放时 canvas 指纹不变会标 :static；跨次找图若 JPEG 字节完全相同
    // 也视为静帧（假 via=http-mirror 已踩坑）——最多再拍 2 次。
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            if (bridge.IsAborted()) {
                err = L"已取消";
                return false;
            }
            WindowModeLogf(L"[窗口模式] 扩展视觉: 静帧/复用旧图 丢弃，重试截图 (%d/2)", attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(80 * attempt));
            bridge.ClearLastShotJpeg();
            result.clear();
            reqErr.clear();
        }
        const bool okReq = bridge.Request(cmd, "", result, reqErr, 20000) && ResultLooksOk(result);
        if (!okReq) break;

        const std::string via = ExtractJsonStringLocal(result, "via");
        lastShotVia_ = via;
        const bool isStatic = via.find(":static") != std::string::npos;

        const bool httpShot = result.find("\"shotHttp\":true") != std::string::npos
            || via.find("cdp:http") != std::string::npos;
        if (httpShot) {
            std::vector<uint8_t> jpeg;
            if (!bridge.TakeLastShotJpeg(jpeg) || jpeg.size() < 64) {
                if (err.empty()) err = L"HTTP 截图未收到/解码失败";
                return false;
            }
            uint64_t hash = 14695981039346656037ull;
            for (uint8_t b : jpeg) {
                hash ^= b;
                hash *= 1099511628211ull;
            }
            const bool sameAsPrev = lastVisionJpegHash_ != 0 && hash == lastVisionJpegHash_;
            if ((isStatic || sameAsPrev) && attempt < 2) {
                if (sameAsPrev && !isStatic) {
                    WindowModeLog(L"[窗口模式] 扩展视觉: JPEG 与上次完全相同（假新帧）");
                }
                continue;
            }
            if (isStatic || sameAsPrev) {
                WindowModeLog(L"[窗口模式] 扩展视觉: 仍为静帧/旧图（屏外可能冻 WebGL）；匹配可能不准");
            }
            if (!decodeJpegBytesToBmp(jpeg)) {
                return false;
            }
            lastVisionJpegHash_ = hash;
            WindowModeLogf(L"[窗口模式] 扩展视觉截图成功 via=%s (HTTP)",
                via.empty() ? L"cdp:http" : std::wstring(via.begin(), via.end()).c_str());
            const std::string visionPath = ExtractJsonStringLocal(result, "visionPath");
            if (!visionPath.empty()) {
                WindowModeLogf(L"[窗口模式] 扩展视觉路径=%s",
                    Utf8SnippetToWide(visionPath).c_str());
            }
            if (via.find("pageclip") != std::string::npos) {
                const std::string mirrorErr = ExtractJsonStringLocal(result, "mirrorErr");
                WindowModeLog(L"[窗口模式] ★回归：热路径出现 pageclip（1.1.33+ 应为 mirror-only）");
                if (!mirrorErr.empty()) {
                    WindowModeLogf(L"[窗口模式] 扩展视觉 mirror 未用（已回退 pageclip，可能闪白）: %s",
                        Utf8SnippetToWide(mirrorErr).c_str());
                }
            }
            return true;
        }
        if (isStatic && attempt < 2) {
            std::vector<uint8_t> discard;
            bridge.TakeLastShotJpeg(discard);
            continue;
        }
        if (isStatic) {
            WindowModeLog(L"[窗口模式] 扩展视觉: 仍为静帧（最小化冻 WebGL）；匹配可能不准");
        }
        if (decodeB64ToBmp(ExtractJsonStringLocal(result, "data"))) {
            if (!via.empty()) {
                WindowModeLogf(L"[窗口模式] 扩展视觉截图成功 via=%s",
                    std::wstring(via.begin(), via.end()).c_str());
            } else {
                WindowModeLog(L"[窗口模式] 扩展视觉截图成功");
            }
            return true;
        }
        err = L"扩展截图解码失败";
        return false;
    }

    if (result.find("SHOT_UNAVAILABLE") != std::string::npos) {
        const std::string detail = ExtractJsonStringLocal(result, "message");
        err = detail.empty()
            ? L"扩展视觉截图不可用"
            : (L"扩展视觉截图不可用: " + Utf8SnippetToWide(detail));
        WindowModeLogf(L"[窗口模式] %s", err.c_str());
        return false;
    }
    if (!reqErr.empty()) {
        WindowModeLogf(L"[窗口模式] 扩展视觉截图失败: %s", reqErr.c_str());
    }
    err = reqErr.empty() ? L"扩展截图失败" : reqErr;
    return false;
}

namespace {

HBITMAP CreateSolidDib(int w, int h, COLORREF bgr) {
    if (w <= 0 || h <= 0) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!bmp || !bits) return nullptr;
    const DWORD pixel = 0xFF000000u
        | (GetRValue(bgr))
        | (static_cast<DWORD>(GetGValue(bgr)) << 8)
        | (static_cast<DWORD>(GetBValue(bgr)) << 16);
    auto* p = static_cast<DWORD*>(bits);
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < n; ++i) p[i] = pixel;
    return bmp;
}

HBITMAP CompositeCanvasOntoClient(HBITMAP canvas, int clientW, int clientH,
    int dx, int dy, int dw, int dh) {
    if (!canvas || clientW <= 0 || clientH <= 0) return nullptr;
    BITMAP bm{};
    if (!GetObjectW(canvas, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return nullptr;

    HBITMAP dest = CreateSolidDib(clientW, clientH, RGB(0, 0, 0));
    if (!dest) return nullptr;

    dx = std::clamp(dx, 0, clientW - 1);
    dy = std::clamp(dy, 0, clientH - 1);
    dw = std::clamp(dw, 1, clientW - dx);
    dh = std::clamp(dh, 1, clientH - dy);

    HDC screen = GetDC(nullptr);
    HDC hdcSrc = CreateCompatibleDC(screen);
    HDC hdcDst = CreateCompatibleDC(screen);
    HGDIOBJ oldSrc = SelectObject(hdcSrc, canvas);
    HGDIOBJ oldDst = SelectObject(hdcDst, dest);
    SetStretchBltMode(hdcDst, HALFTONE);
    SetBrushOrgEx(hdcDst, 0, 0, nullptr);
    const BOOL ok = StretchBlt(hdcDst, dx, dy, dw, dh,
        hdcSrc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
    SelectObject(hdcSrc, oldSrc);
    SelectObject(hdcDst, oldDst);
    DeleteDC(hdcSrc);
    DeleteDC(hdcDst);
    ReleaseDC(nullptr, screen);
    if (!ok) {
        DeleteObject(dest);
        return nullptr;
    }
    return dest;
}

}  // namespace

bool ExtInputSession::CaptureScreenshotForClientMatch(int clientW, int clientH,
    HBITMAP* outBmp, int* outW, int* outH, std::wstring& err) {
    if (!outBmp || clientW < 400 || clientH < 300) {
        err = L"客户区尺寸异常(疑似最小化缩略图)";
        return false;
    }
    *outBmp = nullptr;
    if (outW) *outW = 0;
    if (outH) *outH = 0;

    // attach 已有 iframe 时禁止再打 layout：旧路径会与 vision 叠在一起弄死 MV3 桥。
    // pageCss 缺失时用 iframe 外框推算，不够再走 canvas 空间匹配。
    if (iframeCssW_ <= 0 || iframeCssH_ <= 0) {
        if (SupportsStableBridgeApi()) {
            std::wstring layoutErr;
            RefreshLayout(layoutErr);
        }
    } else if (pageCssW_ <= 0 || pageCssH_ <= 0) {
        pageCssW_ = std::max(pageCssW_, iframeCssX_ + iframeCssW_);
        pageCssH_ = std::max(pageCssH_, iframeCssY_ + iframeCssH_);
        if (contentW_ > 0 && contentH_ > 0 && (pageCssW_ < 64 || pageCssH_ < 64)) {
            pageCssW_ = contentW_;
            pageCssH_ = contentH_;
        }
    }

    HBITMAP canvas = nullptr;
    int sw = 0, sh = 0;
    if (!CaptureScreenshot(&canvas, &sw, &sh, err) || !canvas) {
        return false;
    }
    if (IsCaptureLikelyBlank(canvas)) {
        DeleteObject(canvas);
        err = L"扩展截图像素黑帧(游戏未绘制/WebGL缓冲已清空)";
        return false;
    }

        if (lastShotVia_.find("page:full") != std::string::npos
            || lastShotVia_.find("cdp:iframe") != std::string::npos
            || lastShotVia_.find("cdp:http") != std::string::npos) {
        // 整页/iframe CDP/HTTP 截图：交给 VisionMatch canvas 映射。
        DeleteObject(canvas);
        err = L"page-surface-skip-composite";
        return false;
    }

int dx = 0, dy = 0, dw = clientW, dh = clientH;
    if (iframeCssW_ > 0 && iframeCssH_ > 0 && pageCssW_ > 0 && pageCssH_ > 0) {
        // 用统一缩放，避免 pageCss 与客户区比例不一致时把 iframe 拉成竖向畸变。
        const double sx = clientW / static_cast<double>(pageCssW_);
        const double sy = clientH / static_cast<double>(pageCssH_);
        const double s = std::min(sx, sy);
        dx = static_cast<int>(std::lround(iframeCssX_ * s));
        dy = static_cast<int>(std::lround(iframeCssY_ * s));
        dw = std::max(1, static_cast<int>(std::lround(iframeCssW_ * s)));
        dh = std::max(1, static_cast<int>(std::lround(iframeCssH_ * s)));
        // 若布局异常导致贴片过小，改为按 canvas 像素居中铺到客户区。
        if (dw < clientW / 3 || dh < clientH / 3) {
            dw = clientW;
            dh = clientH;
            dx = 0;
            dy = 0;
        }
        WindowModeLogf(
            L"[窗口模式] 扩展截图合成: canvas=%dx%d → client=%dx%d iframe@(%d,%d) %dx%d (pageCss=%dx%d s=%.3f)",
            sw, sh, clientW, clientH, dx, dy, dw, dh, pageCssW_, pageCssH_, s);
    } else {
        WindowModeLogf(
            L"[窗口模式] 扩展截图合成: 无 iframe 布局，canvas %dx%d 拉伸铺满客户区 %dx%d",
            sw, sh, clientW, clientH);
    }

    HBITMAP composed = CompositeCanvasOntoClient(canvas, clientW, clientH, dx, dy, dw, dh);
    DeleteObject(canvas);
    if (!composed) {
        err = L"扩展截图合成失败";
        return false;
    }
    if (IsCaptureLikelyBlank(composed)) {
        DeleteObject(composed);
        err = L"扩展合成图黑帧";
        return false;
    }
    *outBmp = composed;
    if (outW) *outW = clientW;
    if (outH) *outH = clientH;
    err.clear();
    return true;
}

bool ExtInputSession::CaptureScreenshotForVisionMatch(int clientW, int clientH,
    HBITMAP* outBmp, int* outW, int* outH, bool* outCanvasSpace, std::wstring& err) {
    if (outCanvasSpace) *outCanvasSpace = false;
    if (!outBmp) {
        err = L"无效输出";
        return false;
    }
    *outBmp = nullptr;
    if (outW) *outW = 0;
    if (outH) *outH = 0;

    // 只截一次：HTTP 帧已被 Take，禁止 ClientMatch 失败后再截第二次。
    HBITMAP canvas = nullptr;
    int sw = 0, sh = 0;
    if (!CaptureScreenshot(&canvas, &sw, &sh, err) || !canvas) {
        if (err.empty()) err = L"扩展视觉截图失败";
        return false;
    }
    if (sw < 64 || sh < 64 || IsCaptureLikelyBlank(canvas)) {
        DeleteObject(canvas);
        err = L"扩展截图黑帧/过小";
        return false;
    }

    const bool iframeOrHttp = lastShotVia_.find("iframe") != std::string::npos
        || lastShotVia_.find("cdp:http") != std::string::npos
        || lastShotVia_.find("page:full") != std::string::npos;
    if (iframeOrHttp || clientW < 400 || clientH < 300) {
        *outBmp = canvas;
        if (outW) *outW = sw;
        if (outH) *outH = sh;
        if (outCanvasSpace) *outCanvasSpace = true;
        err.clear();
        WindowModeLogf(L"[窗口模式] 扩展视觉: canvas/HTTP 空间 %dx%d via=%s",
            sw, sh,
            lastShotVia_.empty() ? L"?" : std::wstring(lastShotVia_.begin(), lastShotVia_.end()).c_str());
        return true;
    }

    // 壳页整图：尝试合成到客户区（再拍一次代价高，这里用已有 canvas 本地合成）。
    int dx = 0, dy = 0, dw = clientW, dh = clientH;
    if (iframeCssW_ > 0 && iframeCssH_ > 0 && pageCssW_ > 0 && pageCssH_ > 0) {
        const double sx = clientW / static_cast<double>(pageCssW_);
        const double sy = clientH / static_cast<double>(pageCssH_);
        const double s = std::min(sx, sy);
        dx = static_cast<int>(std::lround(iframeCssX_ * s));
        dy = static_cast<int>(std::lround(iframeCssY_ * s));
        dw = std::max(1, static_cast<int>(std::lround(iframeCssW_ * s)));
        dh = std::max(1, static_cast<int>(std::lround(iframeCssH_ * s)));
    }
    HBITMAP composed = CompositeCanvasOntoClient(canvas, clientW, clientH, dx, dy, dw, dh);
    DeleteObject(canvas);
    if (!composed || IsCaptureLikelyBlank(composed)) {
        if (composed) DeleteObject(composed);
        err = L"扩展截图合成失败";
        return false;
    }
    *outBmp = composed;
    if (outW) *outW = clientW;
    if (outH) *outH = clientH;
    if (outCanvasSpace) *outCanvasSpace = false;
    err.clear();
    return true;
}

}  // namespace windowmode
