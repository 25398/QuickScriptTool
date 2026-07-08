#include "find_image_ui_debug.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#if FIND_IMAGE_UI_DEBUG

namespace {

std::mutex g_logMu;
HWND g_mainWnd = nullptr;
HWND g_overlayWnd = nullptr;
std::wstring g_logPath;
std::wstring g_overlayLine1;
std::wstring g_overlayLine2;
int g_grayDrawLogBudget = 0;

std::wstring NowText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64]{};
    swprintf_s(buf, L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void AppendLine(const std::wstring& line) {
    std::lock_guard<std::mutex> lock(g_logMu);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"a+, ccs=UTF-8") != 0 || !f) return;
    fputws(line.c_str(), f);
    fputwc(L'\n', f);
    fclose(f);
    OutputDebugStringW((line + L"\n").c_str());
}

void RefreshOverlay() {
    if (!g_overlayWnd) return;
    std::wstring text = L"[DBG] " + g_overlayLine1;
    if (!g_overlayLine2.empty()) text += L"\r\n" + g_overlayLine2;
    SetWindowTextW(g_overlayWnd, text.c_str());
    InvalidateRect(g_overlayWnd, nullptr, TRUE);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(255, 249, 196));
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void EnsureOverlay() {
    if (!g_mainWnd || g_overlayWnd) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FiDbgOverlay";
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    RECT rc{};
    GetClientRect(g_mainWnd, &rc);
    const int h = 44;
    g_overlayWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"FiDbgOverlay", L"[DBG]",
        WS_CHILD | WS_VISIBLE,
        8, rc.bottom - h - 8, rc.right - 16, h,
        g_mainWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_overlayWnd) return;
    HFONT font = CreateFontW(
        14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    SendMessageW(g_overlayWnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    RefreshOverlay();
}

void SetOverlay(const std::wstring& line1, const std::wstring& line2 = L"") {
    g_overlayLine1 = line1;
    g_overlayLine2 = line2;
    EnsureOverlay();
    RefreshOverlay();
}

std::wstring ForegroundSummary() {
    HWND fg = GetForegroundWindow();
    wchar_t title[128]{};
    GetWindowTextW(fg, title, 128);
    wchar_t cls[32]{};
    GetClassNameW(fg, cls, 32);
    wchar_t buf[256]{};
    swprintf_s(buf, L"fg=%p cls=%s title=\"%s\"", fg, cls, title);
    return buf;
}

} // namespace

void FiDbgInit(HWND mainWnd) {
    g_mainWnd = mainWnd;
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path = exePath;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash + 1);
    path += L"find_image_ui_debug.log";
    g_logPath = path;

    AppendLine(L"========== FIND_IMAGE_UI_DEBUG session start ==========");
    AppendLine(NowText() + L" log=" + g_logPath);
    SetOverlay(L"诊断已启动", g_logPath);

    if (mainWnd) {
        wchar_t title[256]{};
        GetWindowTextW(mainWnd, title, 256);
        if (wcsstr(title, L" [DBG]") == nullptr) {
            SetWindowTextW(mainWnd, (std::wstring(title) + L" [DBG]").c_str());
        }
    }
}

void FiDbgShutdown() {
    if (g_overlayWnd) {
        DestroyWindow(g_overlayWnd);
        g_overlayWnd = nullptr;
    }
    AppendLine(L"========== FIND_IMAGE_UI_DEBUG session end ==========");
}

void FiDbgSetMainWnd(HWND mainWnd) {
    g_mainWnd = mainWnd;
}

void FiDbgLog(const wchar_t* event, const std::wstring& detail) {
    if (!event) return;
    std::wstring line = NowText() + L" [" + event + L"]";
    if (!detail.empty()) line += L" " + detail;
    AppendLine(line);
    SetOverlay(std::wstring(event) + (detail.empty() ? L"" : (L": " + detail)));
}

void FiDbgLogFmt(const wchar_t* event, const wchar_t* fmt, ...) {
    wchar_t buf[1024]{};
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    FiDbgLog(event, buf);
}

std::wstring FiDbgHwndTag(HWND hwnd) {
    if (!hwnd) return L"null";
    wchar_t cls[32]{};
    GetClassNameW(hwnd, cls, 32);
    wchar_t title[96]{};
    GetWindowTextW(hwnd, title, 96);
    RECT rc = RECT{};
    GetWindowRect(hwnd, &rc);
    const bool vis = IsWindowVisible(hwnd) != FALSE;
    const bool en = IsWindowEnabled(hwnd) != FALSE;
    wchar_t buf[512]{};
    swprintf_s(buf, L"hwnd=%p cls=%s vis=%d en=%d rc=(%ld,%ld,%ld,%ld) title=\"%s\"",
        hwnd, cls, vis ? 1 : 0, en ? 1 : 0, rc.left, rc.top, rc.right, rc.bottom, title);
    return buf;
}

std::wstring FiDbgGrayButtonTag(HWND hwnd) {
    static const std::unordered_map<HWND, const wchar_t*> kNames;
    (void)kNames;
    // 名称在 main_window 侧传入更可靠；此处仅输出句柄信息
    return FiDbgHwndTag(hwnd);
}

std::wstring FiDbgItemState(UINT state) {
    wchar_t buf[128]{};
    swprintf_s(buf, L"0x%X sel=%d focus=%d dis=%d",
        state,
        (state & ODS_SELECTED) ? 1 : 0,
        (state & ODS_FOCUS) ? 1 : 0,
        (state & ODS_DISABLED) ? 1 : 0);
    return buf;
}

std::wstring FiDbgWindowChain(HWND hwnd, int maxDepth) {
    std::wstring out;
    for (int i = 0; hwnd && i < maxDepth; ++i) {
        if (i) out += L" -> ";
        out += FiDbgHwndTag(hwnd);
        hwnd = GetParent(hwnd);
    }
    return out;
}

void FiDbgLogGrayButtonLayout(const wchar_t* group, HWND btn) {
    if (!group || !btn) return;
    RECT rc{};
    GetWindowRect(btn, &rc);
    const LONG style = GetWindowLongW(btn, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(btn, GWL_EXSTYLE);
    const HWND parent = GetParent(btn);
    wchar_t text[64]{};
    GetWindowTextW(btn, text, 64);
    FiDbgLogFmt(L"BTN_LAYOUT",
        L"%s btn=%p parent=%p text=\"%s\" client=(%ld,%ld,%ld,%ld) style=0x%lX ex=0x%lX",
        group, btn, parent, text,
        rc.left, rc.top, rc.right, rc.bottom,
        style, exStyle);
}

void FiDbgOnGrayClick(HWND btn) {
    g_grayDrawLogBudget = 8;
    wchar_t text[64]{};
    if (btn) GetWindowTextW(btn, text, 64);
    FiDbgLogFmt(L"GRAY_CLICK", L"btn=%p text=\"%s\"", btn, text);
}

void FiDbgOnGrayHoverChanged(HWND oldBtn, HWND newBtn) {
    wchar_t oldText[64]{};
    wchar_t newText[64]{};
    if (oldBtn) GetWindowTextW(oldBtn, oldText, 64);
    if (newBtn) GetWindowTextW(newBtn, newText, 64);
    FiDbgLogFmt(L"GRAY_HOVER",
        L"old=%p(\"%s\") -> new=%p(\"%s\")",
        oldBtn, oldText, newBtn, newText);
}

void FiDbgBumpGrayDraw(HWND btn, UINT itemState, HWND hoverBtn, bool force) {
    const bool selected = (itemState & ODS_SELECTED) != 0;
    const bool focused = (itemState & ODS_FOCUS) != 0;
    if (!force && !selected && !focused && g_grayDrawLogBudget <= 0) return;
    if (g_grayDrawLogBudget > 0) --g_grayDrawLogBudget;

    wchar_t text[64]{};
    GetWindowTextW(btn, text, 64);
    FiDbgLogFmt(L"GRAY_DRAW",
        L"btn=%p text=\"%s\" hoverBtn=%p match=%d state=%s",
        btn, text, hoverBtn, (btn == hoverBtn) ? 1 : 0, FiDbgItemState(itemState).c_str());
}

#endif
