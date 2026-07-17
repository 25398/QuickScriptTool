#pragma once
// ──────────────────────────────────────────────────────────────────
// modern_edit.h — 现代风格编辑框（灰边、剪贴板快捷键、IME 友好）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include "config.h"

constexpr int kModernEditLimitSingle = 8192;
constexpr int kModernEditLimitMulti = 262144;
constexpr int kModernEditLimitAgentInput = 262144;

/// 为已有 EDIT/RichEdit 控件启用现代输入行为（可重复调用，幂等）
void ApplyModernEditBehavior(HWND edit, bool multiline, int maxChars = 0, bool drawBorder = false);

/// 处理 Ctrl+A / Ctrl+Z 等编辑快捷键；若已处理返回 true
bool ModernEditHandleShortcutMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/// 选中编辑框全部文本
void ModernEditSelectAll(HWND edit);

/// 单行编辑框文字垂直居中（对齐外侧 SS_CENTERIMAGE / DT_VCENTER 标签）。
/// 依赖 ES_MULTILINE 伪单行；密码框等不支持时为 no-op。
void CenterModernSingleLineEditText(HWND edit);

/// 创建无边框单行编辑框（边框由父窗口绘制；文字与同行标签垂直对齐）
HWND MakeModernSingleLineEdit(HWND parent, const wchar_t* text, int id,
    int x, int y, int w, int h, DWORD extraStyle = 0);

/// 创建无边框多行编辑框
HWND MakeModernMultiLineEdit(HWND parent, const wchar_t* text, int id,
    int x, int y, int w, int h, bool wantReturn = true, DWORD extraStyle = 0);

/// 在 parent 的 HDC 上绘制编辑框灰色边框
void DrawModernEditBorder(HDC hdc, const RECT& rc);

/// 根据控件位置在对话框上绘制单个 EDIT 边框（应在 Paint 中调用）
void DrawEditControlBorder(HDC hdc, HWND dialog, HWND edit,
    COLORREF color = kComboBorderGray);

/// 将编辑框放入 outer 边框矩形内（编辑框内缩 1px，边框绘制在 outer 区域）
void PositionEditInBorderFrame(HWND edit, int outerX, int outerY, int outerW, int outerH);

/// 在编辑框外侧绘制 1px 边框（编辑框需已通过 PositionEditInBorderFrame 内缩）
void DrawEditOuterBorder(HDC hdc, HWND dialog, HWND edit,
    COLORREF color = kComboBorderGray);
