#include "crosshair_drag.h"

#include "config.h"
#include "drawing.h"
#include "process_utils.h"

#include <algorithm>

void CrosshairDragController::ClearButtons() {
    buttons_.clear();
}

void CrosshairDragController::RegisterButton(HWND button, CrosshairDragBinding binding) {
    if (!button) return;
    buttons_.push_back({button, binding});
}

bool CrosshairDragController::IsCrosshairButton(HWND hwnd) const {
    for (const auto& item : buttons_) {
        if (item.first == hwnd) return true;
    }
    return false;
}

bool CrosshairDragController::TryGetBinding(HWND button, CrosshairDragBinding& out) const {
    for (const auto& item : buttons_) {
        if (item.first == button) {
            out = item.second;
            return true;
        }
    }
    return false;
}

HWND CrosshairDragController::HitButton(int x, int y, const std::function<RECT(HWND)>& clientRect) const {
    for (const auto& item : buttons_) {
        if (!item.first) continue;
        if (!IsWindowVisible(item.first)) continue;
        const RECT rc = clientRect(item.first);
        if (PtInRect(&rc, POINT{x, y})) return item.first;
    }
    return nullptr;
}

void CrosshairDragController::Begin(const CrosshairDragBinding& binding) {
    if (!owner_ || !dragCursor_) return;
    mode_ = binding.mode;
    targetEdit_ = binding.targetEdit;
    onWindowTarget_ = binding.onWindowTarget;
    active_ = true;
    savedCursor_ = SetClassLongPtrW(owner_, GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(dragCursor_));
    SetCursor(dragCursor_);
    GetWindowRect(owner_, &savedWindowRect_);
    SetWindowPos(owner_, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    SetCapture(owner_);
    InvalidateButtons();
}

void CrosshairDragController::End() {
    if (!owner_) return;
    ReleaseCapture();
    SetClassLongPtrW(owner_, GCLP_HCURSOR, savedCursor_);
    SetWindowPos(owner_, nullptr, savedWindowRect_.left, savedWindowRect_.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    SetForegroundWindow(owner_);
    active_ = false;
    mode_ = CrosshairDragMode::Coordinates;
    targetEdit_ = nullptr;
    onWindowTarget_ = nullptr;
    InvalidateButtons();
}

bool CrosshairDragController::HandleMessage(UINT msg, WPARAM wp, LPARAM lp,
    CoordinateHandler onCoordinate, ProgramPathHandler onProgramPath) {
    if (!active_) return false;
    (void)lp;

    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(dragCursor_);
        return true;
    case WM_MOUSEMOVE: {
        POINT pt{};
        GetCursorPos(&pt);
        if (mode_ == CrosshairDragMode::Coordinates && onCoordinate) {
            onCoordinate(pt.x, pt.y);
        }
        SetCursor(dragCursor_);
        return true;
    }
    case WM_LBUTTONDOWN:
        return true;
    case WM_LBUTTONUP: {
        POINT pt{};
        GetCursorPos(&pt);
        if (mode_ == CrosshairDragMode::WindowTarget) {
            const WindowInfoFromPoint info = GetWindowInfoFromPoint(pt.x, pt.y);
            if (onWindowTarget_) onWindowTarget_(info);
            if (!info.processPath.empty() && targetEdit_) {
                SetWindowTextW(targetEdit_, info.processPath.c_str());
            }
        } else if (mode_ == CrosshairDragMode::ProgramPath) {
            const std::wstring path = GetProcessPathFromPoint(pt.x, pt.y);
            if (!path.empty()) {
                if (onProgramPath) onProgramPath(path);
                else if (targetEdit_) SetWindowTextW(targetEdit_, path.c_str());
            }
        }
        End();
        return true;
    }
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        End();
        return true;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            End();
            return true;
        }
        break;
    case WM_HOTKEY:
        break;
    default:
        break;
    }
    return false;
}

void CrosshairDragController::DrawButton(DRAWITEMSTRUCT* dis, HFONT font, HWND hoveredButton) const {
    if (!dis) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool active = active_;
    const bool hovered = !active && dis->hwndItem == hoveredButton;
    wchar_t label[64]{};
    GetWindowTextW(dis->hwndItem, label, 64);
    if (label[0] == L'\0') wcscpy_s(label, L"拖动准星");

    HBRUSH bgBrush = CreateSolidBrush(kWhite);
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    constexpr int kIconSize = 28;
    const int iconCx = rc.left + 2 + kIconSize / 2;
    const int iconCy = (rc.top + rc.bottom) / 2;
    const COLORREF iconBlue = active ? RGB(0, 100, 190)
        : (pressed ? RGB(0, 110, 200) : (hovered ? RGB(0, 130, 220) : kCrosshairBlue));
    DrawCrosshairGlyph(hdc, iconCx, iconCy, kIconSize, kWhite, iconBlue, true);

    RECT textRc{rc.left + 2 + kIconSize + 8, rc.top, rc.right, rc.bottom};
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, active ? RGB(20, 50, 180) : (pressed || hovered ? RGB(50, 90, 210) : RGB(50, 70, 180)));
    HGDIOBJ oldFont = SelectObject(hdc, font);
    DrawTextW(hdc, label, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}

void CrosshairDragController::InvalidateButtons() const {
    for (const auto& item : buttons_) {
        if (item.first) InvalidateRect(item.first, nullptr, FALSE);
    }
}
