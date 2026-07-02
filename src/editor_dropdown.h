#pragma once
// ── 编辑器下拉弹层（独立顶层窗口，避免被子控件遮挡） ──────────────

#include <windows.h>

LRESULT CALLBACK EditorDropPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK EditorTipPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void RegisterEditorDropPopupClass();
void RegisterEditorTipPopupClass();
