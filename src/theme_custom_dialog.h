#pragma once
// ──────────────────────────────────────────────────────────────────
// theme_custom_dialog.h — 自定义主题 / 取色弹窗（与主界面主题风格一致）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

namespace quickscript {

/// 自定义主题弹窗：主色/点缀色预览、主题风取色、随机。确认返回 true。
bool ShowCustomThemePicker(HWND owner, COLORREF& mainInOut, COLORREF& accentInOut);

}  // namespace quickscript
