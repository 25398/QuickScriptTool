#include "recording_optimize_dialog.h"
#include "drawing.h"

#include "action_utils.h"
#include "drawing.h"
#include "modern_edit.h"
#include "script_io.h"
#include "taskbar_window.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr COLORREF kKeyboardRow = RGB(204, 229, 255);
constexpr COLORREF kMouseRow = RGB(230, 255, 204);
constexpr COLORREF kWaitAltRow = RGB(246, 246, 246);
constexpr COLORREF kSearchHighlight = RGB(255, 241, 122);

std::wstring SafeScriptFileName(std::wstring name) {
    if (Trim(name).empty()) name = TimestampName();
    for (wchar_t& ch : name) {
        if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    }
    return name;
}

double ParseEditDouble(HWND edit, double fallback) {
    if (!edit) return fallback;
    wchar_t buf[64]{};
    GetWindowTextW(edit, buf, 63);
    if (buf[0] == L'\0') return fallback;
    return std::wcstod(buf, nullptr);
}

constexpr const wchar_t* kSchemeItems[] = {
    L"批量删除", L"等待时间调整", L"鼠标移动合并", L"鼠标移动压缩"};
constexpr const wchar_t* kFilterItems[] = {
    L"全部", L"小于", L"小于等于", L"大于", L"大于等于", L"等于", L"不等于"};

std::vector<std::wstring> SchemeItemStrings() {
    return {kSchemeItems[0], kSchemeItems[1], kSchemeItems[2], kSchemeItems[3]};
}

std::vector<std::wstring> FilterItemStrings() {
    return {kFilterItems[0], kFilterItems[1], kFilterItems[2], kFilterItems[3],
        kFilterItems[4], kFilterItems[5], kFilterItems[6]};
}

}  // namespace

RecordingOptimizeDialog::Result RecordingOptimizeDialog::Show(HWND owner, const ScriptMeta& recording) {
    owner_ = owner;
    done_ = false;
    saved_ = false;
    savedPath_.clear();
    sourcePath_ = recording.path;

    ScriptFileData fileData = LoadScriptFileData(recording.path);
    actions_ = std::move(fileData.actions);
    hotkey_ = fileData.hotkey;
    originalActionCount_ = static_cast<int>(actions_.size());
    originalDuration_ = fileData.durationSeconds > 0
        ? fileData.durationSeconds
        : ComputeDuration(actions_);
    currentDuration_ = originalDuration_;
    selected_.assign(actions_.size(), false);
    anchorIndex_ = 0;

    static bool registered = false;
    const wchar_t* clsName = L"QuickScriptRecordingOptimizeDlg";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &RecordingOptimizeDialog::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = clsName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    RECT ownerRc{};
    GetWindowRect(owner, &ownerRc);
    const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - kDialogW) / 2;
    const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - kDialogH) / 2;

    hwnd_ = CreateWindowExW(
        0, clsName, L"", WS_POPUP | WS_CLIPCHILDREN,
        x, y, kDialogW, kDialogH,
        owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return {};

    ApplyTaskbarWindowStyle(hwnd_, L"鼠大侠-录制优化");
    outerShadow_.Attach(hwnd_);

    const std::wstring defaultName = L"优化-" + (fileData.scriptName.empty() ? recording.name : fileData.scriptName);
    SetWindowTextW(nameEdit_, defaultName.c_str());
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);

    MSG msg{};
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    Result result{};
    result.saved = saved_;
    result.savedPath = savedPath_;
    return result;
}

LRESULT CALLBACK RecordingOptimizeDialog::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    RecordingOptimizeDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<RecordingOptimizeDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<RecordingOptimizeDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT RecordingOptimizeDialog::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        titleFont_ = CreateFontW(28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei");
        bodyFont_ = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei");
        smallFont_ = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei");
        btnFont_ = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei");
        closeFont_ = CreateFontW(38, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        const RECT nameRc = NameEditRect();
        nameEdit_ = MakeModernSingleLineEdit(hwnd_, L"", 100,
            nameRc.left, nameRc.top,
            nameRc.right - nameRc.left, nameRc.bottom - nameRc.top);
        SendMessageW(nameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        CenterEditTextVertically(nameEdit_);
        valueEdit_ = MakeModernSingleLineEdit(hwnd_, L"0.1", 101, 0, 0, kEditW, kEditH, ES_CENTER);
        thresholdEdit_ = MakeModernSingleLineEdit(hwnd_, L"1.0", 102, 0, 0, kEditW, kEditH, ES_CENTER);
        mergeWaitEdit_ = MakeModernSingleLineEdit(hwnd_, L"0.1", 103, 0, 0, kEditW, kEditH, ES_CENTER);
        SendMessageW(valueEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        SendMessageW(thresholdEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        SendMessageW(mergeWaitEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        CreateDropPopup();
        UpdatePanelControls();
        UpdateStats();
        promptModal_.Bind(hwnd_, bodyFont_);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, kWhite);
        SetTextColor(hdc, kText);
        static HBRUSH editBrush = CreateSolidBrush(kWhite);
        return reinterpret_cast<LRESULT>(editBrush);
    }
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (HitCloseButton(pt.x, pt.y)) return HTCLIENT;
        if (HitTitleBar(pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_MOUSEWHEEL:
        if (promptModal_.visible()) return 0;
        if (!DropPopupVisible()) {
            scrollTop_ -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            ClampScroll();
            InvalidateListArea();
        }
        return 0;
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (promptModal_.visible()) return 0;
        SetHoverFlag(hoverClose_, HitCloseButton(x, y), CloseRect());
        SetHoverFlag(hoverCancel_, PtInRect(CancelBtnRect(), x, y), CancelBtnRect());
        SetHoverFlag(hoverSave_, PtInRect(SaveBtnRect(), x, y), SaveBtnRect());
        SetHoverFlag(hoverApply_, PtInRect(ApplyBtnRect(), x, y), ApplyBtnRect());
        SetHoverFlag(hoverPrevKey_, PtInRect(PrevKeyBtnRect(), x, y), PrevKeyBtnRect());
        SetHoverFlag(hoverNextKey_, PtInRect(NextKeyBtnRect(), x, y), NextKeyBtnRect());
        SetHoverFlag(hoverKeySearch_, PtInRect(KeySearchBtnRect(), x, y), KeySearchBtnRect());
        SetHoverFlag(hoverQuickSelect_, PtInRect(QuickSelectBtnRect(), x, y), QuickSelectBtnRect());
        if (draggingScroll_) {
            const int thumbH = std::max(32, kListH * VisibleRowCount() / std::max(1, static_cast<int>(actions_.size())));
            const int trackH = std::max(1, kListH - thumbH);
            const int rel = std::clamp(y - scrollDragOffset_ - kListTop, 0, trackH);
            scrollTop_ = (rel * MaxScrollTop()) / trackH;
            ClampScroll();
            InvalidateListArea();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (promptModal_.visible()) return 0;
        POINT pt{x, y};
        HWND child = ChildWindowFromPointEx(hwnd_, pt, CWP_SKIPINVISIBLE);
        if (child && child != hwnd_) return DefWindowProcW(hwnd_, msg, wp, lp);
        PopupKind toggledPopup = PopupKind::None;
        if (DropPopupVisible()) {
            POINT screenPt{x, y};
            ClientToScreen(hwnd_, &screenPt);
            RECT popupRc{};
            GetWindowRect(dropPopup_, &popupRc);
            if (screenPt.x >= popupRc.left && screenPt.x <= popupRc.right
                && screenPt.y >= popupRc.top && screenPt.y <= popupRc.bottom) {
                return 0;
            }
            toggledPopup = openPopup_;
            CloseMenuPopup();
        }
        if (HitCloseButton(x, y) || PtInRect(CancelBtnRect(), x, y)) {
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        }
        if (PtInRect(SaveBtnRect(), x, y)) {
            if (SaveToNewRecording()) {
                saved_ = true;
                done_ = true;
                DestroyWindow(hwnd_);
            }
            return 0;
        }
        const RECT applyBtn = ApplyBtnRect();
        if (PtInRect(applyBtn, x, y)) {
            if (optimizeScheme_ == 0) ApplyBatchDelete();
            else if (optimizeScheme_ == 1) ApplyWaitAdjust();
            else if (optimizeScheme_ == 2) ApplyMoveMerge();
            else ApplyMoveCompress();
            return 0;
        }
        const RECT prevBtn = PrevKeyBtnRect();
        const RECT nextBtn = NextKeyBtnRect();
        const RECT keyBtn = KeySearchBtnRect();
        const RECT quickBtn = QuickSelectBtnRect();
        if (PtInRect(prevBtn, x, y)) {
            const int idx = FindKeyIndexFrom(keySearchHighlight_ >= 0 ? keySearchHighlight_ - 1 : anchorIndex_, false);
            if (idx >= 0) { keySearchHighlight_ = lastKeySearchIndex_ = anchorIndex_ = idx; ScrollToIndex(idx); }
            InvalidateListArea();
            return 0;
        }
        if (PtInRect(nextBtn, x, y)) {
            const int idx = FindKeyIndexFrom(keySearchHighlight_ >= 0 ? keySearchHighlight_ + 1 : anchorIndex_, true);
            if (idx >= 0) { keySearchHighlight_ = lastKeySearchIndex_ = anchorIndex_ = idx; ScrollToIndex(idx); }
            InvalidateListArea();
            return 0;
        }
        if (PtInRect(keyBtn, x, y)) {
            OpenMenuPopup(PopupKind::KeySearch, keyBtn, {
                L"查找第一个关键操作", L"查找最后一个关键操作", L"查找下一个关键操作",
                L"查找上一个关键操作", L"查找当前页后关键操作", L"查找当前页前关键操作",
                L"回到最后查找处", L"重置查找"});
            return 0;
        }
        if (PtInRect(quickBtn, x, y)) {
            OpenMenuPopup(PopupKind::QuickSelect, quickBtn, {
                L"选择从当前选择项到最前", L"选择从当前选择项到最后",
                L"选择从当前选择项到下一关键操作间操作",
                L"选择从当前选择项到上一关键操作间操作",
                L"选择从当前已选两项间操作", L"清空选择", L"全选"});
            return 0;
        }
        const RECT panel = RightPanelRect();
        const RECT schemeRc = SchemeComboRect();
        if (PtInRect(schemeRc, x, y)) {
            if (toggledPopup == PopupKind::OptimizeScheme) {
                InvalidatePanelArea();
                return 0;
            }
            OpenMenuPopup(PopupKind::OptimizeScheme, schemeRc, SchemeItemStrings());
            InvalidatePanelArea();
            return 0;
        }
        if (optimizeScheme_ == 1) {
            const RECT filterRc = WaitFilterComboRect();
            if (PtInRect(filterRc, x, y)) {
                if (toggledPopup == PopupKind::WaitFilter) {
                    InvalidatePanelArea();
                    return 0;
                }
                OpenMenuPopup(PopupKind::WaitFilter, filterRc, FilterItemStrings());
                InvalidatePanelArea();
                return 0;
            }
        }
        if (optimizeScheme_ == 0) {
            const RECT protectCheck{panel.left + kMargin, panel.top + kPanelContentTop,
                panel.left + kMargin + kCheckboxSize, panel.top + kPanelContentTop + kCheckboxSize};
            if (PtInRect(protectCheck, x, y)) {
                protectKeyOps_ = !protectKeyOps_;
                InvalidatePanelArea();
                return 0;
            }
        }
        if (optimizeScheme_ == 2) {
            for (int i = 0; i < 5; ++i) {
                const int rowTop = panel.top + kPanelContentTop + 36 + i * kRadioRowH;
                const RECT radio{panel.left + kMargin, rowTop + (kRadioRowH - kRadioSize) / 2,
                    panel.left + kMargin + kRadioSize, rowTop + (kRadioRowH + kRadioSize) / 2};
                if (PtInRect(radio, x, y)) {
                    mergeWaitMode_ = i;
                    UpdatePanelControls();
                    InvalidatePanelArea();
                    return 0;
                }
            }
        }
        const int row = HitListRow(x, y);
        if (row >= 0) {
            if (PtInRect(CheckboxRect(row), x, y)) selected_[static_cast<size_t>(row)] = !selected_[static_cast<size_t>(row)];
            else anchorIndex_ = row;
            InvalidateListArea();
            return 0;
        }
        const RECT scrollTrack{kListLeft + kListW - kScrollBarW, kListTop, kListLeft + kListW, kFooterTop};
        if (MaxScrollTop() > 0 && PtInRect(scrollTrack, x, y)) {
            draggingScroll_ = true;
            SetCapture(hwnd_);
            const int thumbH = std::max(20, kListH * VisibleRowCount() / std::max(1, static_cast<int>(actions_.size())));
            const int thumbTop = kListTop + (std::max(1, kListH - thumbH) * scrollTop_) / std::max(1, MaxScrollTop());
            scrollDragOffset_ = y - thumbTop;
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (draggingScroll_) { draggingScroll_ = false; ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED:
        draggingScroll_ = false;
        return 0;
    case WM_SHOWWINDOW:
        if (!wp) CloseMenuPopup();
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            HWND fg = GetForegroundWindow();
            if (fg != hwnd_ && fg != dropPopup_) CloseMenuPopup();
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_ENTERSIZEMOVE:
        CloseMenuPopup();
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (DropPopupVisible()) SyncDropPopup();
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_CLOSE:
        done_ = true;
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        done_ = true;
        CleanupGdi();
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

void RecordingOptimizeDialog::CleanupGdi() {
    if (dropPopup_) {
        DestroyWindow(dropPopup_);
        dropPopup_ = nullptr;
    }
    if (titleFont_) DeleteObject(titleFont_);
    if (bodyFont_) DeleteObject(bodyFont_);
    if (smallFont_) DeleteObject(smallFont_);
    if (btnFont_) DeleteObject(btnFont_);
    if (closeFont_) DeleteObject(closeFont_);
    titleFont_ = bodyFont_ = smallFont_ = btnFont_ = closeFont_ = nullptr;
}

bool RecordingOptimizeDialog::PtInRect(const RECT& rc, int x, int y) const {
    return x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom;
}

bool RecordingOptimizeDialog::HitCloseButton(int x, int y) const { return PtInRect(CloseRect(), x, y); }
bool RecordingOptimizeDialog::HitTitleBar(int x, int y) const {
    return y >= 0 && y < kOptTitleH && x >= 0 && x < kDialogW - kCloseBtnW;
}

int RecordingOptimizeDialog::MaxScrollTop() const {
    return std::max(0, static_cast<int>(actions_.size()) - VisibleRowCount());
}

void RecordingOptimizeDialog::ClampScroll() { scrollTop_ = std::clamp(scrollTop_, 0, MaxScrollTop()); }

void RecordingOptimizeDialog::ScrollToIndex(int index) {
    if (index < scrollTop_) scrollTop_ = index;
    else if (index >= scrollTop_ + VisibleRowCount()) scrollTop_ = index - VisibleRowCount() + 1;
    ClampScroll();
}

int RecordingOptimizeDialog::HitListRow(int x, int y) const {
    const RECT list = ListContentRect();
    if (!PtInRect(list, x, y) || x >= list.right - kScrollBarW) return -1;
    const int row = scrollTop_ + (y - list.top) / kRowH;
    return (row >= 0 && row < static_cast<int>(actions_.size())) ? row : -1;
}

RECT RecordingOptimizeDialog::RowRect(int index) const {
    return RECT{kListLeft, kListTop + (index - scrollTop_) * kRowH,
        kListLeft + kListW - kScrollBarW, kListTop + (index - scrollTop_ + 1) * kRowH};
}

RECT RecordingOptimizeDialog::CheckboxRect(int index) const {
    RECT row = RowRect(index);
    return RECT{row.left + 52, row.top + (kRowH - kCheckboxSize) / 2,
        row.left + 52 + kCheckboxSize, row.top + (kRowH + kCheckboxSize) / 2};
}

bool RecordingOptimizeDialog::IsKeyOperation(const ScriptAction& action) const {
    return action.type != ActionType::MoveMouse && action.type != ActionType::Wait;
}

bool RecordingOptimizeDialog::IsMoveOrWait(const ScriptAction& action) const {
    return action.type == ActionType::MoveMouse || action.type == ActionType::Wait;
}

COLORREF RecordingOptimizeDialog::RowBackground(const ScriptAction& action) const {
    switch (action.type) {
    case ActionType::KeyDown: case ActionType::KeyUp: case ActionType::KeyClick:
    case ActionType::HotkeyShortcut: case ActionType::QuickInput: return kKeyboardRow;
    case ActionType::MouseDown: case ActionType::MouseUp: case ActionType::MouseClick:
    case ActionType::ScrollWheel: return kMouseRow;
    default: return kWhite;
    }
}

COLORREF RecordingOptimizeDialog::RowBackgroundAt(int index) const {
    const auto& action = actions_[static_cast<size_t>(index)];
    if (action.type == ActionType::Wait || action.type == ActionType::MoveMouse) {
        return (index % 2 == 0) ? kWhite : kWaitAltRow;
    }
    return RowBackground(action);
}

std::wstring RecordingOptimizeDialog::ActionDisplayText(const ScriptAction& action) const {
    switch (action.type) {
    case ActionType::Wait: {
        const int ms = static_cast<int>(action.duration * 1000.0 + 0.5);
        return L"等待:" + F3(action.duration) + L"秒(" + std::to_wstring(ms) + L"毫秒)";
    }
    case ActionType::MoveMouse:
        return L"鼠标移动到位置(" + std::to_wstring(action.x) + L"," + std::to_wstring(action.y) + L")";
    case ActionType::KeyDown: return L"键盘按下" + action.keyText;
    case ActionType::KeyUp: return L"键盘抬起" + action.keyText;
    case ActionType::MouseDown: return ButtonText(action.button) + L"按下";
    case ActionType::MouseUp: return ButtonText(action.button) + L"抬起";
    case ActionType::ScrollWheel: return L"滚轮" + std::to_wstring(action.scrollSteps) + L"步";
    default: return ActionName(action);
    }
}

double RecordingOptimizeDialog::ComputeDuration(const std::vector<ScriptAction>& actions) const {
    double total = 0;
    for (const auto& a : actions) if (a.type == ActionType::Wait) total += a.duration;
    return total;
}

void RecordingOptimizeDialog::UpdateStats() {
    currentDuration_ = ComputeDuration(actions_);
    RECT stats = StatsRect();
    RECT header = ListHeaderTextRect();
    InvalidateRect(hwnd_, &stats, FALSE);
    InvalidateRect(hwnd_, &header, FALSE);
}

void RecordingOptimizeDialog::SyncSelectionSize() {
    if (selected_.size() != actions_.size())
        selected_.assign(actions_.size(), false);
}

void RecordingOptimizeDialog::ApplyActionChange() {
    SyncSelectionSize();
    UpdateStats();
    ClampScroll();
    InvalidateListArea();
    InvalidatePanelArea();
}

void RecordingOptimizeDialog::InvalidateListArea() {
    RECT rc = ListContentRect();
    InvalidateRect(hwnd_, &rc, FALSE);
}

void RecordingOptimizeDialog::InvalidatePanelArea() {
    RECT rc = RightPanelRect();
    InvalidateRect(hwnd_, &rc, FALSE);
}

void RecordingOptimizeDialog::SetHoverFlag(bool& flag, bool value, const RECT& rc) {
    if (flag == value) return;
    flag = value;
    InvalidateRect(hwnd_, &rc, FALSE);
}

bool RecordingOptimizeDialog::DropPopupVisible() const {
    return dropPopup_ && IsWindowVisible(dropPopup_) == TRUE;
}

int RecordingOptimizeDialog::PopupVisibleCount() const {
    if (popupItems_.empty()) return 0;
    const int maxVisible = kEditorPopupMaxHeight / kPopupItemH;
    return std::min(maxVisible, static_cast<int>(popupItems_.size()));
}

int RecordingOptimizeDialog::HitPopupItemLocal(int /*x*/, int y) const {
    if (popupItems_.empty()) return -1;
    const int visible = PopupVisibleCount();
    const int rel = (y - 1) / kPopupItemH;
    if (rel < 0 || rel >= visible) return -1;
    const int idx = rel + popupScroll_;
    return idx < static_cast<int>(popupItems_.size()) ? idx : -1;
}

void RecordingOptimizeDialog::CreateDropPopup() {
    static bool registered = false;
    const wchar_t* cls = L"QSRecordingOptDropPopup";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &RecordingOptimizeDialog::DropPopupWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }
    dropPopup_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        cls, L"", WS_POPUP,
        0, 0, 0, 0,
        hwnd_, nullptr, GetModuleHandleW(nullptr), this);
    if (dropPopup_) ShowWindow(dropPopup_, SW_HIDE);
}

void RecordingOptimizeDialog::SyncDropPopup() {
    if (!dropPopup_) return;
    if (openPopup_ == PopupKind::None || popupItems_.empty()) {
        ShowWindow(dropPopup_, SW_HIDE);
        return;
    }
    const int visible = PopupVisibleCount();
    const int scrollMax = std::max(0, static_cast<int>(popupItems_.size()) - visible);
    popupScroll_ = std::clamp(popupScroll_, 0, scrollMax);
    int w = popupAnchor_.right - popupAnchor_.left;
    if (openPopup_ == PopupKind::KeySearch || openPopup_ == PopupKind::QuickSelect) w += 80;
    const int h = visible * kPopupItemH + 2;
    POINT screenTop{popupAnchor_.left, popupAnchor_.bottom + 2};
    ClientToScreen(hwnd_, &screenTop);
    int x = static_cast<int>(screenTop.x);
    int y = static_cast<int>(screenTop.y);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (y + h > work.bottom) {
        POINT screenAnchor{popupAnchor_.left, popupAnchor_.top};
        ClientToScreen(hwnd_, &screenAnchor);
        y = static_cast<int>(screenAnchor.y) - h - 2;
    }
    y = std::max(static_cast<int>(work.top), std::min(y, static_cast<int>(work.bottom) - h));
    SetWindowPos(dropPopup_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
    InvalidateRect(dropPopup_, nullptr, FALSE);
}

void RecordingOptimizeDialog::CloseMenuPopup() {
    const bool wasPanelPopup = openPopup_ == PopupKind::OptimizeScheme
        || openPopup_ == PopupKind::WaitFilter;
    openPopup_ = PopupKind::None;
    popupItems_.clear();
    popupHover_ = -1;
    popupScroll_ = 0;
    if (dropPopup_) ShowWindow(dropPopup_, SW_HIDE);
    if (wasPanelPopup) InvalidatePanelArea();
}

void RecordingOptimizeDialog::OpenMenuPopup(PopupKind kind, const RECT& anchor,
                                             const std::vector<std::wstring>& items) {
    if (openPopup_ == kind) {
        CloseMenuPopup();
        return;
    }
    openPopup_ = kind;
    popupAnchor_ = anchor;
    popupItems_ = items;
    popupHover_ = -1;
    popupScroll_ = 0;
    if (kind == PopupKind::OptimizeScheme) popupScroll_ = std::max(0, optimizeScheme_);
    else if (kind == PopupKind::WaitFilter) popupScroll_ = std::max(0, waitFilterOp_);
    SyncDropPopup();
}

void RecordingOptimizeDialog::InvalidatePopupRow(int idx) {
    if (!dropPopup_ || idx < 0) return;
    const int vis = idx - popupScroll_;
    if (vis < 0 || vis >= PopupVisibleCount()) return;
    RECT row{1, 1 + vis * kPopupItemH, 0, 1 + (vis + 1) * kPopupItemH};
    RECT client{};
    GetClientRect(dropPopup_, &client);
    row.right = client.right - 1;
    InvalidateRect(dropPopup_, &row, FALSE);
}

void RecordingOptimizeDialog::UpdatePanelControls() {
    const RECT panel = RightPanelRect();
    ShowWindow(valueEdit_, SW_HIDE);
    ShowWindow(thresholdEdit_, SW_HIDE);
    ShowWindow(mergeWaitEdit_, SW_HIDE);
    if (optimizeScheme_ == 1) {
        const int adjustTop = WaitAdjustRowTop(panel);
        MoveWindow(valueEdit_, panel.left + 88, adjustTop, kEditW, kEditH, TRUE);
        ShowWindow(valueEdit_, SW_SHOW);
        SetWindowPos(valueEdit_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        if (waitFilterOp_ != 0) {
            const int compareTop = WaitCompareEditTop(panel);
            MoveWindow(thresholdEdit_, panel.left + kMargin, compareTop, kEditW, kEditH, TRUE);
            ShowWindow(thresholdEdit_, SW_SHOW);
            SetWindowPos(thresholdEdit_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
    } else if (optimizeScheme_ == 2) {
        if (mergeWaitMode_ == 4) {
            MoveWindow(mergeWaitEdit_, panel.left + kMargin, panel.top + kMergeAdjustEditTop, kEditW, kEditH, TRUE);
            ShowWindow(mergeWaitEdit_, SW_SHOW);
            SetWindowPos(mergeWaitEdit_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
    } else if (optimizeScheme_ == 3) {
        MoveWindow(valueEdit_, panel.left + kMargin, panel.top + kCompressWaitEditTop, kEditW, kEditH, TRUE);
        MoveWindow(thresholdEdit_, panel.left + kMargin, panel.top + kCompressThresholdEditTop, kEditW, kEditH, TRUE);
        ShowWindow(valueEdit_, SW_SHOW);
        ShowWindow(thresholdEdit_, SW_SHOW);
        SetWindowPos(valueEdit_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(thresholdEdit_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
}

void RecordingOptimizeDialog::CenterEditTextVertically(HWND edit) {
    if (!edit) return;
    RECT rc{};
    GetClientRect(edit, &rc);
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HDC hdc = GetDC(edit);
    HFONT oldFont = font ? reinterpret_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(edit, hdc);
    const int textH = tm.tmHeight;
    const int pad = std::max(0, static_cast<int>((rc.bottom - rc.top - textH) / 2));
    rc.top = pad;
    rc.bottom = rc.top + textH + 2;
    SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&rc));
}

LRESULT CALLBACK RecordingOptimizeDialog::DropPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    RecordingOptimizeDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<RecordingOptimizeDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    self = reinterpret_cast<RecordingOptimizeDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleDropPopup(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT RecordingOptimizeDialog::HandleDropPopup(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(dropPopup_, &ps);
        PaintDropPopupContent(hdc);
        EndPaint(dropPopup_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, dropPopup_, 0};
        TrackMouseEvent(&tme);
        const int idx = HitPopupItemLocal(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (idx != popupHover_) {
            const int old = popupHover_;
            popupHover_ = idx;
            InvalidatePopupRow(old);
            InvalidatePopupRow(idx);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (popupHover_ != -1) {
            const int old = popupHover_;
            popupHover_ = -1;
            InvalidatePopupRow(old);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const int idx = HitPopupItemLocal(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        const PopupKind kind = openPopup_;
        CloseMenuPopup();
        if (idx >= 0) {
            if (kind == PopupKind::KeySearch) FindKeyOperation(idx);
            else if (kind == PopupKind::QuickSelect) QuickSelect(idx);
            else if (kind == PopupKind::OptimizeScheme) {
                optimizeScheme_ = idx;
                UpdatePanelControls();
                InvalidatePanelArea();
            } else if (kind == PopupKind::WaitFilter) {
                waitFilterOp_ = idx;
                UpdatePanelControls();
                InvalidatePanelArea();
            }
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int visible = PopupVisibleCount();
        const int scrollMax = std::max(0, static_cast<int>(popupItems_.size()) - visible);
        if (scrollMax <= 0) return 0;
        const int oldScroll = popupScroll_;
        popupScroll_ = std::clamp(popupScroll_ + (GET_WHEEL_DELTA_WPARAM(wp) < 0 ? 1 : -1), 0, scrollMax);
        if (oldScroll != popupScroll_) InvalidateRect(dropPopup_, nullptr, FALSE);
        return 0;
    }
    default:
        return DefWindowProcW(dropPopup_, msg, wp, lp);
    }
}

void RecordingOptimizeDialog::PaintDropPopupContent(HDC hdc) {
    if (popupItems_.empty()) return;
    RECT client{};
    GetClientRect(dropPopup_, &client);
    FillRectColor(hdc, client, kWhite);
    DrawBorderRect(hdc, client, kComboPopupBorderGray);
    const int visible = PopupVisibleCount();
    const int scrollMax = std::max(0, static_cast<int>(popupItems_.size()) - visible);
    popupScroll_ = std::clamp(popupScroll_, 0, scrollMax);
    int selectedIdx = -1;
    if (openPopup_ == PopupKind::OptimizeScheme) selectedIdx = optimizeScheme_;
    else if (openPopup_ == PopupKind::WaitFilter) selectedIdx = waitFilterOp_;
    SelectObject(hdc, bodyFont_);
    SetBkMode(hdc, TRANSPARENT);
    const int contentRight = client.right - 1 - (scrollMax > 0 ? 12 : 0);
    for (int vis = 0; vis < visible; ++vis) {
        const int i = vis + popupScroll_;
        if (i >= static_cast<int>(popupItems_.size())) break;
        RECT row{client.left + 1, client.top + 1 + vis * kPopupItemH, contentRight, client.top + 1 + (vis + 1) * kPopupItemH};
        const bool selected = i == selectedIdx;
        const bool hovered = !selected && popupHover_ == i;
        const COLORREF rowBg = selected ? kComboMenuSelectBlue : (hovered ? kComboMenuHoverBlue : kWhite);
        FillRectColor(hdc, row, rowBg);
        SetTextColor(hdc, selected ? kComboMenuSelectText : kText);
        RECT textRc{row.left + 10, row.top, row.right - 6, row.bottom};
        DrawTextW(hdc, popupItems_[static_cast<size_t>(i)].c_str(), -1, &textRc,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (scrollMax > 0) {
        RECT track{client.right - 10, client.top + 1, client.right - 1, client.bottom - 1};
        FillRectColor(hdc, track, kComboScrollTrackGray);
        const int trackH = track.bottom - track.top;
        const int thumbH = std::max(18, trackH * visible / static_cast<int>(popupItems_.size()));
        const int thumbTop = track.top + (trackH - thumbH) * popupScroll_ / scrollMax;
        FillRectColor(hdc, RECT{track.left, thumbTop, track.right, thumbTop + thumbH}, kComboScrollThumbGray);
    }
}

int RecordingOptimizeDialog::SelectedCount() const {
    return static_cast<int>(std::count(selected_.begin(), selected_.end(), true));
}

std::vector<int> RecordingOptimizeDialog::SelectedIndices() const {
    std::vector<int> indices;
    for (int i = 0; i < static_cast<int>(selected_.size()); ++i)
        if (selected_[static_cast<size_t>(i)]) indices.push_back(i);
    return indices;
}

std::vector<int> RecordingOptimizeDialog::ContiguousSelectedRange() const {
    auto indices = SelectedIndices();
    if (indices.empty()) return {};
    std::sort(indices.begin(), indices.end());
    for (size_t i = 1; i < indices.size(); ++i)
        if (indices[i] != indices[i - 1] + 1) return {};
    return indices;
}

bool RecordingOptimizeDialog::SelectionIsContiguousMoveWait() const {
    const auto range = ContiguousSelectedRange();
    if (range.empty()) return false;
    for (int idx : range) if (!IsMoveOrWait(actions_[static_cast<size_t>(idx)])) return false;
    return true;
}

int RecordingOptimizeDialog::FindKeyIndexFrom(int start, bool forward) const {
    if (actions_.empty()) return -1;
    if (forward) {
        for (int i = std::max(0, start); i < static_cast<int>(actions_.size()); ++i)
            if (IsKeyOperation(actions_[static_cast<size_t>(i)])) return i;
    } else {
        for (int i = std::min(static_cast<int>(actions_.size()) - 1, start); i >= 0; --i)
            if (IsKeyOperation(actions_[static_cast<size_t>(i)])) return i;
    }
    return -1;
}

void RecordingOptimizeDialog::FindKeyOperation(int mode) {
    int idx = -1;
    switch (mode) {
    case 0: idx = FindKeyIndexFrom(0, true); break;
    case 1: idx = FindKeyIndexFrom(static_cast<int>(actions_.size()) - 1, false); break;
    case 2: idx = FindKeyIndexFrom(keySearchHighlight_ >= 0 ? keySearchHighlight_ + 1 : anchorIndex_ + 1, true); break;
    case 3: idx = FindKeyIndexFrom(keySearchHighlight_ >= 0 ? keySearchHighlight_ - 1 : anchorIndex_ - 1, false); break;
    case 4: idx = FindKeyIndexFrom(scrollTop_ + VisibleRowCount(), true); break;
    case 5: idx = FindKeyIndexFrom(scrollTop_ - 1, false); break;
    case 6: idx = lastKeySearchIndex_; break;
    case 7: keySearchHighlight_ = -1; InvalidateRect(hwnd_, nullptr, FALSE); return;
    default: break;
    }
    if (idx >= 0) {
        keySearchHighlight_ = lastKeySearchIndex_ = anchorIndex_ = idx;
        ScrollToIndex(idx);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void RecordingOptimizeDialog::QuickSelect(int mode) {
    if (actions_.empty()) return;
    const int anchor = std::clamp(anchorIndex_, 0, static_cast<int>(actions_.size()) - 1);
    switch (mode) {
    case 0: for (int i = 0; i <= anchor; ++i) selected_[static_cast<size_t>(i)] = true; break;
    case 1: for (int i = anchor; i < static_cast<int>(actions_.size()); ++i) selected_[static_cast<size_t>(i)] = true; break;
    case 2: {
        int end = static_cast<int>(actions_.size()) - 1;
        for (int i = anchor + 1; i < static_cast<int>(actions_.size()); ++i) {
            if (IsKeyOperation(actions_[static_cast<size_t>(i)])) { end = i - 1; break; }
        }
        for (int i = anchor; i <= end; ++i) selected_[static_cast<size_t>(i)] = true;
        break;
    }
    case 3: {
        int start = 0;
        for (int i = anchor - 1; i >= 0; --i) {
            if (IsKeyOperation(actions_[static_cast<size_t>(i)])) { start = i + 1; break; }
        }
        for (int i = start; i <= anchor; ++i) selected_[static_cast<size_t>(i)] = true;
        break;
    }
    case 4: {
        auto indices = SelectedIndices();
        if (indices.size() < 2) return;
        std::sort(indices.begin(), indices.end());
        for (int i = indices.front(); i <= indices.back(); ++i) selected_[static_cast<size_t>(i)] = true;
        break;
    }
    case 5: std::fill(selected_.begin(), selected_.end(), false); break;
    case 6: std::fill(selected_.begin(), selected_.end(), true); break;
    default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool RecordingOptimizeDialog::WaitMatchesFilter(double duration, double compareValue) const {
    switch (waitFilterOp_) {
    case 0: return true;
    case 1: return duration < compareValue;
    case 2: return duration <= compareValue;
    case 3: return duration > compareValue;
    case 4: return duration >= compareValue;
    case 5: return std::abs(duration - compareValue) < 0.0005;
    case 6: return std::abs(duration - compareValue) >= 0.0005;
    default: return true;
    }
}

void RecordingOptimizeDialog::ApplyBatchDelete() {
    if (SelectedCount() == 0) { ShowAlert(L"请先选择要删除的动作。"); return; }
    std::vector<ScriptAction> kept;
    kept.reserve(actions_.size());
    for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
        if (!selected_[static_cast<size_t>(i)]) { kept.push_back(actions_[static_cast<size_t>(i)]); continue; }
        if (protectKeyOps_ && IsKeyOperation(actions_[static_cast<size_t>(i)])) kept.push_back(actions_[static_cast<size_t>(i)]);
    }
    actions_ = std::move(kept);
    selected_.assign(actions_.size(), false);
    ApplyActionChange();
}

void RecordingOptimizeDialog::ApplyWaitAdjust() {
    waitAdjustValue_ = ParseEditDouble(valueEdit_, waitAdjustValue_);
    const double compareVal = waitFilterOp_ == 0
        ? 0.0
        : ParseEditDouble(thresholdEdit_, filterCompareValue_);
    int changed = 0;
    for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
        if (!selected_[static_cast<size_t>(i)]) continue;
        auto& a = actions_[static_cast<size_t>(i)];
        if (a.type != ActionType::Wait || !WaitMatchesFilter(a.duration, compareVal)) continue;
        a.duration = waitAdjustValue_;
        ++changed;
    }
    if (changed == 0) ShowAlert(L"没有符合条件的等待动作被调整。");
    ApplyActionChange();
}

void RecordingOptimizeDialog::ApplyMoveMerge() {
    if (!SelectionIsContiguousMoveWait()) {
        ShowAlert(L"鼠标移动合并时只能选择连续的移动和等待操作，其中不允许夹杂其他操作。");
        return;
    }
    const auto range = ContiguousSelectedRange();
    std::vector<double> waits;
    ScriptAction lastMove{};
    bool hasMove = false;
    for (int idx : range) {
        const auto& a = actions_[static_cast<size_t>(idx)];
        if (a.type == ActionType::Wait) waits.push_back(a.duration);
        else if (a.type == ActionType::MoveMouse) { lastMove = a; hasMove = true; }
    }
    if (!hasMove) { ShowAlert(L"选中范围内没有鼠标移动动作。"); return; }
    mergeWaitValue_ = ParseEditDouble(mergeWaitEdit_, mergeWaitValue_);
    double mergedWait = 0;
    if (mergeWaitMode_ == 0) for (double w : waits) mergedWait += w;
    else if (mergeWaitMode_ == 1) { for (double w : waits) mergedWait += w; mergedWait = waits.empty() ? 0 : mergedWait / waits.size(); }
    else if (mergeWaitMode_ == 2) mergedWait = waits.empty() ? 0 : waits.front();
    else if (mergeWaitMode_ == 3) mergedWait = waits.empty() ? 0 : waits.back();
    else mergedWait = mergeWaitValue_;
    std::vector<ScriptAction> merged;
    if (mergedWait > 0.0005) { ScriptAction wa{}; wa.type = ActionType::Wait; wa.duration = mergedWait; merged.push_back(wa); }
    merged.push_back(lastMove);
    std::vector<ScriptAction> result;
    for (int i = 0; i < range.front(); ++i) result.push_back(actions_[static_cast<size_t>(i)]);
    for (const auto& a : merged) result.push_back(a);
    for (int i = range.back() + 1; i < static_cast<int>(actions_.size()); ++i) result.push_back(actions_[static_cast<size_t>(i)]);
    actions_ = std::move(result);
    selected_.assign(actions_.size(), false);
    for (int i = 0; i < static_cast<int>(merged.size()); ++i)
        selected_[static_cast<size_t>(range.front() + i)] = true;
    ApplyActionChange();
}

void RecordingOptimizeDialog::ApplyMoveCompress() {
    if (!SelectionIsContiguousMoveWait()) {
        ShowAlert(L"鼠标移动压缩时只能选择连续的移动和等待操作，其中不允许夹杂其他操作。");
        return;
    }
    compressWait_ = ParseEditDouble(valueEdit_, compressWait_);
    compressThreshold_ = ParseEditDouble(thresholdEdit_, compressThreshold_);
    const auto range = ContiguousSelectedRange();
    struct Point { int x; int y; };
    std::vector<Point> points;
    for (int idx : range) {
        const auto& a = actions_[static_cast<size_t>(idx)];
        if (a.type == ActionType::MoveMouse) points.push_back({a.x, a.y});
    }
    if (points.size() < 2) { ShowAlert(L"选中范围内至少需要两个鼠标移动点。"); return; }
    auto dist = [](const Point& a, const Point& b) {
        const double dx = static_cast<double>(a.x - b.x), dy = static_cast<double>(a.y - b.y);
        return std::sqrt(dx * dx + dy * dy);
    };
    std::vector<Point> compressed{points.front()};
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        if (dist(compressed.back(), points[i]) >= compressThreshold_) compressed.push_back(points[i]);
    }
    if (compressed.back().x != points.back().x || compressed.back().y != points.back().y) compressed.push_back(points.back());
    std::vector<ScriptAction> replacement;
    for (size_t i = 0; i < compressed.size(); ++i) {
        if (i > 0) { ScriptAction wa{}; wa.type = ActionType::Wait; wa.duration = compressWait_; replacement.push_back(wa); }
        ScriptAction mv{}; mv.type = ActionType::MoveMouse; mv.x = compressed[i].x; mv.y = compressed[i].y;
        replacement.push_back(mv);
    }
    std::vector<ScriptAction> result;
    for (int i = 0; i < range.front(); ++i) result.push_back(actions_[static_cast<size_t>(i)]);
    for (const auto& a : replacement) result.push_back(a);
    for (int i = range.back() + 1; i < static_cast<int>(actions_.size()); ++i) result.push_back(actions_[static_cast<size_t>(i)]);
    actions_ = std::move(result);
    selected_.assign(actions_.size(), false);
    for (int i = 0; i < static_cast<int>(replacement.size()); ++i)
        selected_[static_cast<size_t>(range.front() + i)] = true;
    ApplyActionChange();
}

void RecordingOptimizeDialog::ShowAlert(const wchar_t* message) {
    CloseMenuPopup();
    promptModal_.ShowInfo(message ? message : L"");
}

bool RecordingOptimizeDialog::SaveToNewRecording() {
    wchar_t nameBuf[256]{};
    GetWindowTextW(nameEdit_, nameBuf, 255);
    const std::wstring name = Trim(nameBuf);
    if (name.empty()) { ShowAlert(L"请输入录制名称。"); return false; }
    EnsureScriptsDir();
    std::wstring path = RecordingsDir() + L"\\" + SafeScriptFileName(name) + L".json";
    for (int suffix = 1; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++suffix)
        path = RecordingsDir() + L"\\" + SafeScriptFileName(name) + L"-" + std::to_wstring(suffix) + L".json";
    ScriptFileData data{};
    data.scriptName = name;
    data.recordTime = NowText();
    data.durationSeconds = ComputeDuration(actions_);
    data.hotkey = hotkey_;
    data.actions = actions_;
    if (!SaveScriptFileData(path, data)) { ShowAlert(L"保存失败：无法写入文件，请检查磁盘空间和权限。"); return false; }
    savedPath_ = path;
    return true;
}

void RecordingOptimizeDialog::DrawGreenButton(HDC hdc, const RECT& rc, const wchar_t* text,
                                               bool hover, bool enabled) {
    const COLORREF fill = enabled ? (hover ? kButtonGreenHover : kButtonGreen) : kButtonDisabledGreen;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, enabled ? kWhite : kButtonDisabledText);
    SelectObject(hdc, btnFont_);
    DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void RecordingOptimizeDialog::DrawOutlineButton(HDC hdc, const RECT& rc, const wchar_t* text, bool hover) {
    DrawBorderRoundRect(hdc, rc, hover ? kMainGreen : kComboBorderGray, 6);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kMainGreen);
    SelectObject(hdc, btnFont_);
    DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void RecordingOptimizeDialog::DrawPanelCombo(HDC hdc, const RECT& rc, const wchar_t* text, bool open) {
    FillRectColor(hdc, rc, kWhite);
    const int arrowW = 26;
    SelectObject(hdc, bodyFont_);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kMainGreen);
    RECT textRc{rc.left + 10, rc.top, rc.right - arrowW, rc.bottom};
    DrawTextW(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int arrowCenterX = rc.right - arrowW / 2;
    const int arrowCenterY = (rc.top + rc.bottom) / 2;
    POINT arrow[3] = {
        {arrowCenterX - 5, arrowCenterY - 3},
        {arrowCenterX + 5, arrowCenterY - 3},
        {arrowCenterX, arrowCenterY + 4}};
    HBRUSH arrowBrush = CreateSolidBrush(kMainGreen);
    HGDIOBJ oldBrush = SelectObject(hdc, arrowBrush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, arrow, 3);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(arrowBrush);
    DrawBorderRect(hdc, rc, open ? kMainGreen : kComboBorderGray);
}

void RecordingOptimizeDialog::DrawRadio(HDC hdc, const RECT& rc, bool checked) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int cx = rc.left + w / 2;
    const int cy = rc.top + h / 2;
    const int inset = 1;
    HPEN pen = CreatePen(PS_SOLID, 2, checked ? kMainGreen : kComboBorderGray);
    HBRUSH brush = CreateSolidBrush(kWhite);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    Ellipse(hdc, rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    if (checked) {
        const int dotR = std::max(4, std::min(w, h) / 2 - 6);
        HBRUSH dot = CreateSolidBrush(kMainGreen);
        oldBrush = SelectObject(hdc, dot);
        oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, cx - dotR, cy - dotR, cx + dotR, cy + dotR);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(dot);
    }
}

void RecordingOptimizeDialog::PaintList(HDC hdc) {
    const RECT list = ListContentRect();
    FillRectColor(hdc, list, kWhite);
    DrawBorderRect(hdc, list, kLineGreen);
    SelectObject(hdc, bodyFont_);
    SetBkMode(hdc, TRANSPARENT);
    HPEN linePen = CreatePen(PS_SOLID, 1, kLineGreen);
    HGDIOBJ oldPen = SelectObject(hdc, linePen);
    for (int i = scrollTop_; i < static_cast<int>(actions_.size()) && i < scrollTop_ + VisibleRowCount(); ++i) {
        RECT row = RowRect(i);
        const COLORREF bg = i == keySearchHighlight_ ? kSearchHighlight : RowBackgroundAt(i);
        HBRUSH rowBrush = CreateSolidBrush(bg);
        FillRect(hdc, &row, rowBrush);
        DeleteObject(rowBrush);
        MoveToEx(hdc, row.left, row.bottom - 1, nullptr);
        LineTo(hdc, row.right, row.bottom - 1);
        SetTextColor(hdc, kIndexGreen);
        wchar_t indexText[16]{};
        swprintf_s(indexText, L"%d", i + 1);
        RECT indexRc{row.left + 8, row.top, row.left + 42, row.bottom};
        DrawTextW(hdc, indexText, -1, &indexRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawCheckbox(hdc, CheckboxRect(i), selected_[static_cast<size_t>(i)]);
        SetTextColor(hdc, kText);
        RECT textRc{row.left + 82, row.top, row.right - 6, row.bottom};
        DrawTextW(hdc, ActionDisplayText(actions_[static_cast<size_t>(i)]).c_str(), -1, &textRc,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(linePen);
    if (MaxScrollTop() > 0) {
        const RECT track{kListLeft + kListW - kScrollBarW + 2, kListTop + 4,
            kListLeft + kListW - 2, kFooterTop - 4};
        FillRectColor(hdc, track, kScrollTrackGray);
        const int thumbH = std::max(32, kListH * VisibleRowCount() / std::max(1, static_cast<int>(actions_.size())));
        const int trackHeight = static_cast<int>(track.bottom - track.top);
        const int trackH = std::max(1, trackHeight - thumbH);
        const int thumbTop = track.top + (trackH * scrollTop_) / std::max(1, MaxScrollTop());
        RECT thumbRc{track.left, thumbTop, track.right, thumbTop + thumbH};
        FillRectColor(hdc, thumbRc, kScrollThumbGray);
    }
}

void RecordingOptimizeDialog::PaintRightPanel(HDC hdc) {
    const RECT panel = RightPanelRect();
    FillRectColor(hdc, panel, kPanel);
    DrawBorderRect(hdc, panel, kLineGreen);
    SelectObject(hdc, bodyFont_);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kText);
    wchar_t selText[64]{};
    swprintf_s(selText, L"已选择%d个", SelectedCount());
    RECT selRc{panel.left + kMargin, panel.top + kPanelSelectedTop,
        panel.right - kMargin, panel.top + kPanelSelectedTop + 28};
    DrawTextW(hdc, selText, -1, &selRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT schemeLabel{panel.left + kMargin, panel.top + kSchemeLabelTop,
        panel.right - kMargin, panel.top + kSchemeLabelTop + 28};
    DrawTextW(hdc, L"优化方案", -1, &schemeLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const RECT schemeRc = SchemeComboRect();
    DrawPanelCombo(hdc, schemeRc, kSchemeItems[optimizeScheme_],
        openPopup_ == PopupKind::OptimizeScheme);
    const RECT applyBtn = ApplyBtnRect();
    if (optimizeScheme_ == 0) {
        RECT protectCheck{panel.left + kMargin, panel.top + kPanelContentTop,
            panel.left + kMargin + kCheckboxSize, panel.top + kPanelContentTop + kCheckboxSize};
        DrawCheckbox(hdc, protectCheck, protectKeyOps_);
        RECT protectText{panel.left + kMargin + kCheckboxSize + 8, panel.top + kPanelContentTop - 4,
            panel.right - kMargin, panel.top + kPanelContentTop + 28};
        DrawTextW(hdc, L"不删除关键操作", -1, &protectText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawGreenButton(hdc, applyBtn, L"删除选择项", hoverApply_);
    } else if (optimizeScheme_ == 1) {
        RECT waitLabel{panel.left + kMargin, panel.top + kPanelContentTop, panel.right - kMargin, panel.top + kPanelContentTop + 28};
        DrawTextW(hdc, L"等待时间(已选择的):", -1, &waitLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const RECT filterRc = WaitFilterComboRect();
        DrawPanelCombo(hdc, filterRc, kFilterItems[waitFilterOp_],
            openPopup_ == PopupKind::WaitFilter);
        if (waitFilterOp_ != 0) {
            const int compareTop = WaitCompareEditTop(panel);
            RECT secCompare{panel.left + kMargin + kEditW + 8, compareTop,
                panel.left + kMargin + kEditW + 36, compareTop + kEditH};
            DrawTextW(hdc, L"秒", -1, &secCompare, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        const int adjustTop = WaitAdjustRowTop(panel);
        RECT adjustLabel{panel.left + kMargin, adjustTop, panel.left + 80, adjustTop + kEditH};
        DrawTextW(hdc, L"调整为", -1, &adjustLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT secLabel{panel.left + 88 + kEditW + 8, adjustTop,
            panel.left + 88 + kEditW + 36, adjustTop + kEditH};
        DrawTextW(hdc, L"秒", -1, &secLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawGreenButton(hdc, applyBtn, L"调整选择", hoverApply_);
    } else if (optimizeScheme_ == 2) {
        static const wchar_t* kMergeModes[] = {
            L"等待时间累加", L"使用平均时间", L"使用已选的第一个", L"使用已选的最后一个", L"使用指定的时间"};
        RECT mergeLabel{panel.left + kMargin, panel.top + kPanelContentTop, panel.right - kMargin, panel.top + kPanelContentTop + 28};
        DrawTextW(hdc, L"合并后等待时间处理:", -1, &mergeLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (int i = 0; i < 5; ++i) {
            const int rowTop = panel.top + kPanelContentTop + 36 + i * kRadioRowH;
            RECT radio{panel.left + kMargin, rowTop + (kRadioRowH - kRadioSize) / 2,
                panel.left + kMargin + kRadioSize, rowTop + (kRadioRowH + kRadioSize) / 2};
            DrawRadio(hdc, radio, mergeWaitMode_ == i);
            RECT radioText{panel.left + kMargin + kRadioSize + 8, rowTop,
                panel.right - kMargin, rowTop + kRadioRowH};
            DrawTextW(hdc, kMergeModes[i], -1, &radioText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        if (mergeWaitMode_ == 4) {
            RECT adjustLabel{panel.left + kMargin, panel.top + kMergeAdjustLabelTop,
                panel.right - kMargin, panel.top + kMergeAdjustLabelTop + 26};
            DrawTextW(hdc, L"调整等待时间为", -1, &adjustLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT secLabel{panel.left + kMargin + kEditW + 8, panel.top + kMergeAdjustEditTop,
                panel.left + kMargin + kEditW + 36, panel.top + kMergeAdjustEditTop + kEditH};
            DrawTextW(hdc, L"秒", -1, &secLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(hdc, smallFont_);
        SetTextColor(hdc, kHint);
        RECT hint{panel.left + kMargin, panel.bottom - 108, panel.right - kMargin, panel.bottom - 72};
        DrawTextW(hdc, L"*本操作将合并已选择的等待移动操作为1个。并用选择的合并等待时间方式设置这个等待时间。", -1, &hint, DT_LEFT | DT_WORDBREAK);
        DrawGreenButton(hdc, applyBtn, L"开始合并", hoverApply_);
    } else {
        RECT waitLabel{panel.left + kMargin, panel.top + kPanelContentTop,
            panel.right - kMargin, panel.top + kPanelContentTop + 26};
        DrawTextW(hdc, L"压缩后等待时间", -1, &waitLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT sec1{panel.left + kMargin + kEditW + 8, panel.top + kCompressWaitEditTop,
            panel.left + kMargin + kEditW + 36, panel.top + kCompressWaitEditTop + kEditH};
        DrawTextW(hdc, L"秒", -1, &sec1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT thLabel{panel.left + kMargin, panel.top + kCompressThresholdLabelTop,
            panel.right - kMargin, panel.top + kCompressThresholdLabelTop + 26};
        DrawTextW(hdc, L"压缩阈值", -1, &thLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawGreenButton(hdc, applyBtn, L"开始压缩", hoverApply_);
    }
    DrawEditControlBorder(hdc, hwnd_, valueEdit_);
    DrawEditControlBorder(hdc, hwnd_, thresholdEdit_);
    DrawEditControlBorder(hdc, hwnd_, mergeWaitEdit_);
}

void RecordingOptimizeDialog::Paint() {
    PAINTSTRUCT ps{};
    HDC windowDc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    HDC hdc = CreateCompatibleDC(windowDc);
    HBITMAP bmp = CreateCompatibleBitmap(windowDc, w, h);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);
    FillRectColor(hdc, client, kWhite);
    RECT titleBar{0, 0, kDialogW, kOptTitleH};
    FillRectColor(hdc, titleBar, kMainGreen);
    SelectObject(hdc, titleFont_);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kWhite);
    RECT titleRc{kMargin, 0, kDialogW - kCloseBtnW, kOptTitleH};
    DrawTextW(hdc, L"鼠大侠-录制优化", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hoverClose_) FillRectColor(hdc, CloseRect(), RGB(90, 190, 125));
    SelectObject(hdc, closeFont_);
    RECT closeRc = CloseRect();
    DrawTextW(hdc, L"×", -1, &closeRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, bodyFont_);
    SetTextColor(hdc, kText);
    const RECT nameLabel = NameLabelRect();
    DrawTextW(hdc, L"录制名称:", -1, const_cast<RECT*>(&nameLabel), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawBorderRect(hdc, NameEditRect(), kComboBorderGray);
    wchar_t stats[128]{};
    swprintf_s(stats, L"当前时长:%.3f  原时长:%.3f", currentDuration_, originalDuration_);
    const RECT statsRc = StatsRect();
    DrawTextW(hdc, stats, -1, const_cast<RECT*>(&statsRc), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    wchar_t listHeader[128]{};
    swprintf_s(listHeader, L"动作列表  当前动作数:%d  原动作数:%d",
        static_cast<int>(actions_.size()), originalActionCount_);
    const RECT headerRc = ListHeaderTextRect();
    DrawTextW(hdc, listHeader, -1, const_cast<RECT*>(&headerRc), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawGreenButton(hdc, PrevKeyBtnRect(), L"<<", hoverPrevKey_);
    DrawGreenButton(hdc, NextKeyBtnRect(), L">>", hoverNextKey_);
    DrawGreenButton(hdc, KeySearchBtnRect(), L"关键操作查找", hoverKeySearch_);
    DrawGreenButton(hdc, QuickSelectBtnRect(), L"快速选择", hoverQuickSelect_);
    PaintList(hdc);
    PaintRightPanel(hdc);
    RECT footer{0, kFooterTop, kDialogW, kDialogH};
    FillRectColor(hdc, footer, kPanel);
    DrawOutlineButton(hdc, CancelBtnRect(), L"取消", hoverCancel_);
    DrawGreenButton(hdc, SaveBtnRect(), L"保存到新录制", hoverSave_);
    const int blitW = ps.rcPaint.right - ps.rcPaint.left;
    const int blitH = ps.rcPaint.bottom - ps.rcPaint.top;
    BitBlt(windowDc, ps.rcPaint.left, ps.rcPaint.top, blitW, blitH, hdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
    EndPaint(hwnd_, &ps);
}
