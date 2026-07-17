#include "ui_scale.h"

#include "config.h"

#include <algorithm>

#ifndef ENUM_CURRENT_SETTINGS
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#endif

namespace {

// 设计基准分辨率：2560×1440 下首页 720×540、编辑器 1200×1080（100% 缩放）
constexpr int kUiRefScreenW = 2560;
constexpr int kUiRefScreenH = 1440;

int g_uiScalePercent = 100;

bool MonitorPixelSize(HMONITOR monitor, int* outW, int* outH) {
    if (!outW || !outH) return false;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!monitor || !GetMonitorInfoW(monitor, &mi)) return false;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
        && dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
        *outW = static_cast<int>(dm.dmPelsWidth);
        *outH = static_cast<int>(dm.dmPelsHeight);
        return true;
    }

    *outW = mi.rcMonitor.right - mi.rcMonitor.left;
    *outH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return *outW > 0 && *outH > 0;
}

int ResolutionScalePercent(HMONITOR monitor) {
    int screenW = 0;
    int screenH = 0;
    if (!MonitorPixelSize(monitor, &screenW, &screenH)) {
        return 100;
    }

    const int pctW = MulDiv(screenW, 100, kUiRefScreenW);
    const int pctH = MulDiv(screenH, 100, kUiRefScreenH);
    int pct = std::min(pctW, pctH);
    // 大于参考分辨率时不放大，保持设计尺寸
    pct = std::min(pct, 100);
    return std::max(50, pct);
}

int FitScalePercentToWorkArea(HMONITOR monitor, int scalePercent) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return scalePercent;
    }

    const int workW = info.rcWork.right - info.rcWork.left;
    const int workH = info.rcWork.bottom - info.rcWork.top;
    if (workW <= 0 || workH <= 0) return scalePercent;

    const int editorW = MulDiv(kEditorWidth, scalePercent, 100);
    const int editorH = MulDiv(kEditorHeight, scalePercent, 100);
    int fitPercent = scalePercent;
    if (editorW > workW) {
        fitPercent = std::min(fitPercent, MulDiv(workW, 100, kEditorWidth));
    }
    if (editorH > workH) {
        fitPercent = std::min(fitPercent, MulDiv(workH, 100, kEditorHeight));
    }
    return std::max(50, fitPercent);
}

void ApplyScaleFromMonitor(HMONITOR monitor) {
    g_uiScalePercent = FitScalePercentToWorkArea(monitor, ResolutionScalePercent(monitor));
}

}  // namespace

void UiScaleSetPercent(int percent) {
    g_uiScalePercent = std::max(50, std::min(percent, 100));
}

int UiScalePercent() {
    return g_uiScalePercent;
}

int UiLen(int designPx) {
    return MulDiv(designPx, g_uiScalePercent, 100);
}

int UiHomeWidth() {
    return UiLen(kHomeWidth);
}

int UiHomeHeight() {
    return UiLen(kHomeHeight);
}

int UiEditorWidth() {
    return UiLen(kEditorWidth);
}

int UiEditorHeight() {
    return UiLen(kEditorHeight);
}

RECT UiRect4(int left, int top, int right, int bottom) {
    return RECT{UiLen(left), UiLen(top), UiLen(right), UiLen(bottom)};
}

int UiFontHeight(int designHeight) {
    return std::max(8, UiLen(designHeight));
}

void UiResizeWindowClient(HWND hwnd, int clientW, int clientH, bool force) {
    if (!hwnd || !IsWindow(hwnd) || clientW <= 0 || clientH <= 0) return;
    RECT clientRc{};
    GetClientRect(hwnd, &clientRc);
    const int curW = clientRc.right - clientRc.left;
    const int curH = clientRc.bottom - clientRc.top;
    if (!force && curW == clientW && curH == clientH) return;
    RECT winRc{};
    GetWindowRect(hwnd, &winRc);
    const int borderW = (winRc.right - winRc.left) - curW;
    const int borderH = (winRc.bottom - winRc.top) - curH;
    SetWindowPos(hwnd, nullptr, 0, 0, clientW + borderW, clientH + borderH,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void UiResizeHwndToHome(HWND hwnd) {
    UiResizeWindowClient(hwnd, UiHomeWidth(), UiHomeHeight());
}

void UiResizeHwndToEditor(HWND hwnd) {
    UiResizeWindowClient(hwnd, UiEditorWidth(), UiEditorHeight());
}

void UiScaleInitFromPrimaryMonitor() {
    const POINT pt{0, 0};
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    ApplyScaleFromMonitor(monitor);
}

void UiScaleInitFromHwnd(HWND hwnd) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    ApplyScaleFromMonitor(monitor);
}

void UiScaleSetFromDpi(int /*dpi*/, HWND hwnd) {
    if (hwnd && IsWindow(hwnd)) {
        UiScaleInitFromHwnd(hwnd);
    } else {
        UiScaleInitFromPrimaryMonitor();
    }
}
