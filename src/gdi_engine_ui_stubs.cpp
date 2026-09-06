// gdi_engine_ui_stubs.cpp — 旧 GDI 编辑器控件空实现（产品 UI 在 Web；头仍在 src/）

#include "controls.h"
#include "editor_dropdown.h"
#include "panel_popup_combo.h"
#include "popup_combo.h"
#include "prompt_modal.h"
#include "ui_component.h"

#include "engine/engine_host_window.h"

#include <commctrl.h>
#include <unordered_map>
#include <unordered_set>

namespace {

HWND NullHwnd() { return nullptr; }

}  // namespace

// ── controls ──────────────────────────────────────────────────────
HWND MakeLabel(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }
HWND MakeComboLabel(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }
HWND MakeEditorLabel(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }
HWND MakeHint(HWND, const wchar_t*, int, int, int, int) { return NullHwnd(); }
HWND MakeEdit(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }
HWND MakeFieldEdit(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }
HWND MakeMultilineEdit(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }
HWND MakeButton(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }
HWND MakeGreenButton(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }
HWND MakeGrayButton(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }
HWND MakeCaptureField(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }
HWND MakeCheckBox(HWND, const wchar_t*, int, int, int, int, int) { return NullHwnd(); }

void MarkParamCheckbox(HWND) {}
bool IsMarkedParamCheckbox(HWND) { return false; }
bool IsParamCheckboxChecked(HWND) { return false; }
void SetParamCheckboxChecked(HWND, bool, bool) {}

// ── ui_component（布局创建；Scale* 在 win32_ui_util）──────────────
UILayoutResult CalculateLayout(const UILayout&, bool) { return {}; }
HWND CreateComponent(HWND, const UIComponent&, int, int) { return NullHwnd(); }
UILayoutResult BuildLayout(HWND, const UILayout&, bool) { return {}; }
void ShowLayoutGroup(const UILayoutResult&, bool) {}

// ── popup_combo ───────────────────────────────────────────────────
std::unordered_map<HWND, int> g_comboListHover;
std::unordered_set<HWND> g_comboHover;

void PaintComboChrome(HDC, const RECT&, bool, bool) {}
void StyleComboDropdownList(HWND) {}
LRESULT CALLBACK ComboListSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    return DefSubclassProc(hwnd, msg, wp, lp);
}
LRESULT CALLBACK ComboSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    return DefSubclassProc(hwnd, msg, wp, lp);
}
HWND MakeCombo(HWND, int, int, int, int, int) { return NullHwnd(); }
void ConfigureComboDropdown(HWND, int) {}
void MeasureComboOwnerItem(MEASUREITEMSTRUCT*) {}
void DrawComboOwnerItem(DRAWITEMSTRUCT*, HFONT) {}

// ── editor_dropdown ───────────────────────────────────────────────
void RegisterEditorDropPopupClass() {}
void RegisterEditorTipPopupClass() {}
void RegisterClickerDropPopupClass() {}
LRESULT CALLBACK EditorDropPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT CALLBACK EditorTipPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT CALLBACK ClickerDropPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── panel_popup_combo ─────────────────────────────────────────────
void PanelPopupCombo::Init(HWND, HFONT) {}
void PanelPopupCombo::Destroy() {}
void PanelPopupCombo::SetItems(std::vector<std::wstring>) {}
void PanelPopupCombo::SetSelectedIndex(int) {}
std::wstring PanelPopupCombo::DisplayText() const { return placeholder_; }
void PanelPopupCombo::Toggle(const RECT&) {}
void PanelPopupCombo::Close() {}
void PanelPopupCombo::DrawField(HDC, const RECT&, bool) const {}
bool PanelPopupCombo::HitField(const RECT&, int, int) const { return false; }
bool PanelPopupCombo::IsPopupVisible() const { return false; }
bool PanelPopupCombo::HitPopupScreen(int, int) const { return false; }
void PanelPopupCombo::SyncPopupPosition(const RECT&) {}
LRESULT CALLBACK PanelPopupCombo::PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT PanelPopupCombo::HandlePopupMessage(UINT, WPARAM, LPARAM) { return 0; }
void PanelPopupCombo::PaintPopup(HDC) {}
int PanelPopupCombo::VisibleCount(int) const { return 0; }
int PanelPopupCombo::HitItemIndex(int, int) const { return -1; }
void PanelPopupCombo::SelectIndex(int) {}

// ── prompt_modal ──────────────────────────────────────────────────
PromptModalLayout ComputePromptModalLayout(const RECT&, PromptModalMode,
    const std::wstring&, HFONT) {
    return {};
}
void PaintPromptModal(HDC, const RECT&, const std::wstring&, PromptModalMode, bool, bool, HFONT) {}
PromptModalButton PromptModalHitTest(int, int, const RECT&, PromptModalMode,
    const std::wstring&, HFONT) {
    return PromptModalButton::None;
}

void PromptModal::Bind(HWND, HFONT, std::function<void()>) {}
void PromptModal::OnOwnerResize() {}
void PromptModal::ShowInfo(const std::wstring&, std::function<void()>) {}
void PromptModal::ShowConfirm(const std::wstring&, std::function<void(bool)>) {}
void PromptModal::Close(PromptModalButton) {}
void PromptModal::EnsureShield() {}
void PromptModal::SyncShield() {}
void PromptModal::InvalidateButtonRegion() {}
bool PromptModal::UpdateHover(int, int, const RECT&) { return false; }
void PromptModal::TrackMouseLeave() {}
RECT PromptModal::ClientRect() const { return {}; }
RECT PromptModal::OwnerScreenRect() const { return {}; }
void PromptModal::RefreshOwnerAfterClose() {}
void PromptModal::ReleaseMessageFont() {}
void PromptModal::EnsureMessageFont() {}
LRESULT CALLBACK PromptModal::ShieldProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR) {
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ── editor_paint（EngineHost 成员）────────────────────────────────
void EngineHost::PaintActionListLocal(HDC, int, int) {}
void EngineHost::PaintEditorScrollbarLocal(HDC, int, int) {}
void EngineHost::DrawNavTab(HDC, RECT, quickscript::MainTab, const std::wstring&, int) {}
void EngineHost::DrawRadio(HDC, RECT, bool) {}
void EngineHost::DrawClickerPopupMenuItem(HDC, const RECT&, const wchar_t*, const wchar_t*, bool, bool) {}
void EngineHost::PaintClickerDropPopupContent(HDC, HWND) {}
void EngineHost::DrawEditorCombo(HDC, HWND, RECT, bool) {}
void EngineHost::PaintEditorDropPopupContent(HDC, HWND) {}
void EngineHost::PaintEditorTipPopupContent(HDC, HWND) {}
void EngineHost::PaintEditorListHeaderChrome(HDC) {}
void EngineHost::PaintEditorParamChrome(HDC, HWND) {}
void EngineHost::PaintEditor(HDC) {}
void EngineHost::PaintParamScrollScrollbar(HDC) {}
void EngineHost::DrawEditorFieldBorder(HDC, HWND, HWND) {}
void EngineHost::PaintActionList(HDC) {}
void EngineHost::PaintDragMarker(HDC) {}
void EngineHost::DrawTextIn(HDC, const std::wstring&, RECT, COLORREF, UINT) {}
SIZE EngineHost::MeasureQuickInputTipSize() const { return SIZE{0, 0}; }
