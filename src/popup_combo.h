// ── 弹出式下拉组合框系统 ──────────────────────────────────────
// 提供自定义绘制的 Win32 组合框，包括悬停高亮、下拉箭头、
// 边框样式和下拉列表滚动条样式。
#pragma once
#include <windows.h>
#include <unordered_map>
#include <unordered_set>

// 全局组合框状态
extern std::unordered_map<HWND, int> g_comboListHover; // 列表项悬停索引
extern std::unordered_set<HWND> g_comboHover;           // 组合框悬停状态

// 绘制组合框右侧的下拉箭头外框
void PaintComboChrome(HDC hdc, const RECT& rc, bool hovered, bool dropped);

// 样式化下拉列表的滚动条
void StyleComboDropdownList(HWND combo);

// 组合框下拉列表的子类回调
LRESULT CALLBACK ComboListSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

// 组合框控件的子类回调（处理悬停和绘制）
LRESULT CALLBACK ComboSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

// 创建自定义样式的组合框控件
HWND MakeCombo(HWND parent, int id, int x, int y, int w, int h);

// 配置组合框下拉特性的辅助函数
void ConfigureComboDropdown(HWND combo, int visibleRows);
