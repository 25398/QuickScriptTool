#pragma once

#include <windows.h>

#include <string>
#include <vector>

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

// ── UIA 控件枚举 / 调用（桌面侧「UIA 优先定位」）────────────────────
// 为什么需要：Vision 定位一次要截屏 + 上传 + VLM（1~2 轮 API），而 UIA 树是本地、免费、
/// 且能给「按钮文字」这种语义完全一致的目标。对齐微软 UFO 的做法：
/// 枚举控件 → 编号 → 选中 → 校验 name 与 id 一致 → Invoke（不打像素）。
struct UiControlInfo {
    /// 1 起，给模型看的编号（与列表顺序一致）
    int id = 0;
    std::wstring name;
    /// 中文类型标签（按钮/输入框/菜单项…）
    std::wstring controlType;
    /// 原始 UIA 控件类型 id（CONTROLTYPEID，避免在头文件里拉 COM 头）
    int controlTypeId = 0;
    RECT rect{};
    bool enabled = true;
    bool offscreen = false;
    /// 支持 InvokePattern（可不打像素直接触发）
    bool invokable = false;
    /// 支持 ValuePattern（可直接填值）
    bool valuePattern = false;
    std::wstring automationId;
};

/// 枚举窗口（hwnd=nullptr → 前台窗口）里可交互的 UIA 控件，最多 maxCount 个。
/// 只收有名字的可交互类型；按屏幕阅读顺序排序后编号。
std::vector<UiControlInfo> ListInteractiveUiControls(HWND hwnd = nullptr, int maxCount = 60);

/// 把控件列表格式化成给模型看的紧凑文本（编号 + 类型 + 名字 + 状态），超预算截断。
std::wstring FormatUiControlListForAgent(const std::vector<UiControlInfo>& items,
    size_t maxChars = 1800);

/// 在给定列表里按名字挑一个控件：
/// 完全同名 > 前缀 > 包含（大小写不敏感）；同分时取阅读顺序最前。
/// 返回命中的下标；ambiguous=true 表示存在近似竞争项（调用方应回退 Vision）。
/// outIndex 越界/无命中时返回 -1。
int PickUiControlByName(const std::vector<UiControlInfo>& items, const std::wstring& name,
    bool* ambiguous = nullptr);

/// 按名字（而不是编号）重新枚举并触发控件：name 是模型真正推理过的语义，
/// 编号可能因界面变化而错位——以 name 定位、以 id 校验，两者不一致时写入 outWarn。
/// 优先 InvokePattern；不可 Invoke 时返回 false 并给出矩形（调用方点中心）。
bool InvokeUiControlByName(const std::wstring& name, int expectedId,
    std::wstring& outActualName, int& outActualId, RECT& outRect, bool& outInvoked,
    std::wstring& outWarn);

/// 遮挡校验：屏幕点是否落在「前台顶层窗口本身 / 其后代 / 其拥有的弹窗」上。
/// false = 该点属于别的程序（或被子窗口挡住），此时点击会打错目标，应拦截。
/// 用 GA_ROOT + 同进程 + owner 链判定，避免把菜单/下拉这类弹窗误判为遮挡。
bool IsScreenPointOnForegroundWindow(int screenX, int screenY);

}  // namespace windowmode
