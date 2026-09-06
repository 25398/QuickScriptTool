// Shared Win32 UI helpers kept on the product Engine path (headless still ApplyFont / Scale*).
#include "controls.h"
#include "config.h"
#include "ui_component.h"
#include "ui_scale.h"

HINSTANCE GetThisModule() {
    return GetModuleHandleW(nullptr);
}

void ApplyFont(HWND parent, HFONT font) {
    if (!parent || !font) return;
    SendMessageW(parent, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    for (HWND child = GetWindow(parent, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        ApplyFont(child, font);
    }
}

int ScaleX(int baseX) {
    return MulDiv(baseX, UiEditorWidth(), kEditorBaseWidth);
}
int ScaleY(int baseY) {
    return MulDiv(baseY, UiEditorHeight(), kEditorBaseHeight);
}
int ScaleW(int baseW) {
    return MulDiv(baseW, UiEditorWidth(), kEditorBaseWidth);
}
int ScaleH(int baseH) {
    return MulDiv(baseH, UiEditorHeight(), kEditorBaseHeight);
}
