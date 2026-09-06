#pragma once

#include <windows.h>

#include <string>

namespace windowmode {

/// 屏幕某点上 UI 元素的可交互状态（UIAutomation 探测）。
/// 用途：点击前判断按钮是不是灰色不可用，避免 AI 对着禁用控件反复点。
struct UiElementState {
    /// UIA 是否成功取到该点的元素（false 时其余字段无意义，按「未知」处理）
    bool probed = false;
    bool enabled = true;
    /// 元素被标记为「只读/不可交互」（如禁用的编辑框）
    bool offscreen = false;
    std::wstring name;
    std::wstring controlType;
};

/// 探测屏幕坐标 (x,y) 处的控件状态。失败时返回 probed=false（调用方应放行，勿误拦）。
UiElementState ProbeUiElementAtPoint(int screenX, int screenY);

/// 在前台窗口里找「提交类」按钮（保存/确定/OK/Save…）并回报是否被禁用。
/// found=false 表示没找到可判定的按钮；disabledName 为被禁用按钮的名字。
bool FindDisabledSubmitButtonInForeground(std::wstring& disabledName);

/// 前台系统对话框（Win32 #32770）或 Office 迷你「保存此文件」面板。
/// 用途：保存链路里区分「经典另存为 / 迷你保存 / 覆盖确认」，
/// 避免在迷你框把目录路径写入文件名、或 Escape 取消整个保存。
struct ForegroundDialogInfo {
    bool present = false;
    /// confirmOverwrite | saveAs | saveAsMini | openFile | dialog
    std::wstring kind;
    std::wstring title;
    /// 可点按钮文本，以 | 分隔，如 "是(Y)|否(N)"
    std::wstring buttons;
    /// 对话框正文（已截断）
    std::wstring message;
};

ForegroundDialogInfo ProbeForegroundDialog();

/// 单行摘要（给模型看）；前台没有系统对话框时返回空串。
std::wstring FormatForegroundDialogProbe();

}  // namespace windowmode
