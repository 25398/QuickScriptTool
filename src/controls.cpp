// ── 窗口控件工厂函数实现 ────────────────────────────────────
#include "controls.h"

HINSTANCE GetThisModule() {
    return GetModuleHandleW(nullptr);
}

HWND MakeLabel(HWND parent, const wchar_t* text, int id,
               int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetThisModule(), nullptr);
}

HWND MakeEditorLabel(HWND parent, const wchar_t* text, int id,
                     int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetThisModule(), nullptr);
}

HWND MakeHint(HWND parent, const wchar_t* text,
              int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, nullptr,
        GetThisModule(), nullptr);
}

HWND MakeEdit(HWND parent, const wchar_t* text, int id,
              int x, int y, int w, int h) {
    return CreateWindowExW(0, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetThisModule(), nullptr);
}

HWND MakeFieldEdit(HWND parent, const wchar_t* text, int id,
                   int x, int y, int w, int h) {
    return CreateWindowExW(0, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetThisModule(), nullptr);
}

HWND MakeMultilineEdit(HWND parent, const wchar_t* text, int id,
                       int x, int y, int w, int h) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetThisModule(), nullptr);
}

HWND MakeButton(HWND parent, const wchar_t* text, int id,
                int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetThisModule(), nullptr);
}

HWND MakeGreenButton(HWND parent, const wchar_t* text, int id,
                     int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetThisModule(), nullptr);
}

HWND MakeGrayButton(HWND parent, const wchar_t* text, int id,
                    int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetThisModule(), nullptr);
}

HWND MakeCaptureField(HWND parent, const wchar_t* text, int id,
                      int x, int y, int w, int h) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetThisModule(), nullptr);
}

HWND MakeCheckBox(HWND parent, const wchar_t* text, int id,
                  int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetThisModule(), nullptr);
}

void ApplyFont(HWND parent, HFONT font) {
    SendMessageW(parent, WM_SETFONT,
        reinterpret_cast<WPARAM>(font), TRUE);
    for (HWND child = GetWindow(parent, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        ApplyFont(child, font);
    }
}
