#pragma once
// ──────────────────────────────────────────────────────────────────
// controls.h — 窗口控件工厂函数集合（声明）
// 封装 Win32 控件创建 API，提供统一的控件创建和配置接口
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

/// 获取当前模块实例句柄
HINSTANCE GetThisModule();

/// 创建左侧对齐、垂直居中的文本标签控件
HWND MakeLabel(HWND parent, const wchar_t* text, int id,
               int x, int y, int w, int h);

/// 创建自绘下拉显示标签控件
HWND MakeComboLabel(HWND parent, const wchar_t* text, int id,
                    int x, int y, int w, int h);

/// 创建左对齐文本标签（顶部对齐，用于编辑器参数区标题）
HWND MakeEditorLabel(HWND parent, const wchar_t* text, int id,
                     int x, int y, int w, int h);

/// 创建左对齐提示文本控件（支持多行）
HWND MakeHint(HWND parent, const wchar_t* text,
              int x, int y, int w, int h);

/// 创建单行文本编辑框控件
HWND MakeEdit(HWND parent, const wchar_t* text, int id,
              int x, int y, int w, int h);

/// 创建由父窗口绘制边框的单行编辑框（无 WS_BORDER）
HWND MakeFieldEdit(HWND parent, const wchar_t* text, int id,
                   int x, int y, int w, int h);

/// 创建多行文本编辑框控件
HWND MakeMultilineEdit(HWND parent, const wchar_t* text, int id,
                       int x, int y, int w, int h);

/// 创建标准按钮控件
HWND MakeButton(HWND parent, const wchar_t* text, int id,
                int x, int y, int w, int h);

/// 创建自绘绿色按钮控件（用于脚本编辑器操作按钮）
HWND MakeGreenButton(HWND parent, const wchar_t* text, int id,
                     int x, int y, int w, int h);

/// 创建自绘灰色按钮控件（用于找图等辅助操作）
HWND MakeGrayButton(HWND parent, const wchar_t* text, int id,
                    int x, int y, int w, int h);

/// 创建带凹陷边框的捕获字段控件
HWND MakeCaptureField(HWND parent, const wchar_t* text, int id,
                      int x, int y, int w, int h);

/// 创建复选框控件
HWND MakeCheckBox(HWND parent, const wchar_t* text, int id,
                  int x, int y, int w, int h);

/// 递归地将字体应用到指定窗口及其所有子窗口
void ApplyFont(HWND parent, HFONT font);
