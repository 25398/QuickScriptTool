// ── 编辑器下拉弹层实现 ────────────────────────────────────────
#include "editor_dropdown.h"
#include "main_window.h"
#include "config.h"

namespace {
constexpr wchar_t kEditorDropPopupClass[] = L"QSEditorDropPopup";
constexpr wchar_t kEditorTipPopupClass[] = L"QSEditorTipPopup";
constexpr wchar_t kClickerDropPopupClass[] = L"QSClickerDropPopup";
bool g_editorDropClassRegistered = false;
bool g_editorTipClassRegistered = false;
bool g_clickerDropClassRegistered = false;
}

void RegisterEditorDropPopupClass() {
    if (g_editorDropClassRegistered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = EditorDropPopupWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kEditorDropPopupClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    g_editorDropClassRegistered = true;
}

void RegisterEditorTipPopupClass() {
    if (g_editorTipClassRegistered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = EditorTipPopupWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kEditorTipPopupClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    g_editorTipClassRegistered = true;
}

void RegisterClickerDropPopupClass() {
    if (g_clickerDropClassRegistered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = ClickerDropPopupWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClickerDropPopupClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    g_clickerDropClassRegistered = true;
}

LRESULT CALLBACK EditorTipPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->RouteEditorTipPopup(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK EditorDropPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->RouteEditorDropPopup(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ClickerDropPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->RouteClickerDropPopup(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::RouteEditorDropPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintEditorDropPopupContent(hdc, hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        const int idx = HitEditorPopupItemLocal(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (idx != editorPopupHover_) {
            const int old = editorPopupHover_;
            editorPopupHover_ = idx;
            InvalidateEditorPopupRow(old);
            InvalidateEditorPopupRow(idx);
            if (editorPopupOpen_ == 7) {
                if (idx >= 0) BeginQuickInputVarTipHover(idx);
                else CancelQuickInputTip();
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (editorPopupHover_ != -1) {
            const int old = editorPopupHover_;
            editorPopupHover_ = -1;
            InvalidateEditorPopupRow(old);
            if (editorPopupOpen_ == 7) CancelQuickInputTip();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const int idx = HitEditorPopupItemLocal(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (idx >= 0) SelectEditorPopupItem(idx);
        else CloseEditorPopup();
        return 0;
    }
    case WM_MOUSEWHEEL:
        OnEditorDropPopupWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

LRESULT MainWindow::RouteClickerDropPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintClickerDropPopupContent(hdc, hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        const int idx = HitClickerPopupItemLocal(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (idx != clickerPopupHover_) {
            const int old = clickerPopupHover_;
            clickerPopupHover_ = idx;
            InvalidateClickerPopupRow(old);
            InvalidateClickerPopupRow(idx);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (clickerPopupHover_ != -1) {
            const int old = clickerPopupHover_;
            clickerPopupHover_ = -1;
            InvalidateClickerPopupRow(old);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const int idx = HitClickerPopupItemLocal(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (idx >= 0) SelectClickerPopupItem(idx);
        else CloseClickerDropPopup();
        return 0;
    }
    case WM_MOUSEWHEEL:
        OnClickerDropPopupWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

LRESULT MainWindow::RouteEditorTipPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp;
    (void)lp;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintEditorTipPopupContent(hdc, hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}
