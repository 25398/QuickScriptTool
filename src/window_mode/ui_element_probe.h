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
    /// ★该点落在**窗口自身**的标题栏按钮上（关闭/最小化/最大化）。
    ///
    /// 为什么必须单独标出来：这些按钮的名字就是「关闭」「最小化」，
    /// 和应用里的「关闭」按钮**字面完全相同**。实测事故 —— 模型要关游戏内的
    /// 卡牌面板，UIA 命中窗口自己的「关闭」按钮（`关闭 [按钮 id=3] InvokePattern`），
    /// 一击把整个游戏窗口关掉，任务随即崩盘（游戏进度丢失）。
    /// 窗口生命周期操作必须走**明确意图**的通道，不能靠「找个叫『关闭』的东西点一下」。
    bool titleBarControl = false;
    /// ★★**桌面/资源管理器里的图标**（shell 视图项）：对它"打开"= **双击**。
    /// shell 的 InvokePattern **只做选中**（实测：回执说"已触发"，浏览器却没起来）。
    bool shellIconItem = false;
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
    /// ★支持 UIA 键盘焦点（`UIA_IsKeyboardFocusablePropertyId`）。
    /// 存在的理由：有些控件（文档/画布）没有按钮语义，但**可以聚焦后直接打字** ——
    /// 「能不能打字」是模型规划「输入文字」这一步的事实前提，而它看不见。
    bool focusable = false;
    /// ★该控件是**密码框**。存在的唯一目的是**拦住值回传**：
    /// UIA 在部分应用里会把密码明文交出来（不是所有都返回圆点），照抄就进了 API 请求。
    bool password = false;
    std::wstring automationId;
    /// ★★给模型看的**动作能力动词**（`click` / `fill` / `toggle` / `select` / `slide` /
    /// `scroll` / `focus`）。由控件角色算出，是**事实**而非建议：
    /// 它回答的是「这个控件支持哪一类操作」，而不是「你该怎么做」。
    /// 对齐 Windows-MCP 的做法（其 UI 树每行都带 `[action: click|fill|toggle|…]`）——
    /// 模型据此**不必靠截图猜**「这是按钮还是输入框、要不要先点一下才能输入」。
    std::wstring action;
    /// ★★**可读状态事实**（短串，全部如实观测得到，取不到就不给）：
    /// `focused` / `password` / `value:"…"` / `range:0-100` / `toggle:on` / `state:expanded` /
    /// `shortcut:Ctrl+S` / `readonly` / `required`。
    /// 为什么要这层：这些状态**在画面上很难或根本读不出来** ——
    ///   开关当前是开还是关、滑块当前位置、输入框里已经有什么、
    ///   这个输入框是不是密码框（截图上是一片圆点）、当前焦点在谁身上。
    /// 没有它们，模型只能「先点一下看看反应」，一次动作 + 一次观察就这么白烧掉。
    /// ⚠ 只放**观测到的事实**，不放任何祈使句/建议（见 docs §45~§48 的判据）。
    std::vector<std::wstring> state;
    /// ★窗口自身标题栏上的按钮（关闭/最小化/最大化）——见 `UiElementState::titleBarControl`。
    /// 枚举时**仍然列出**（模型有知情权，也能解释「为什么不能用」），
    /// 但按名字选中它时会被拒；需要关窗请用 switchWindow(action=close) 这类明确意图的通道。
    bool titleBarControl = false;
    /// ★★**桌面/资源管理器里的图标**（shell 视图项）—— 见 `ClassNameIsShellIconHost`。
    /// ⚠ 为什么要这个标志：这类项在 UIA 里是 `ListItem` + `InvokePattern`，而 shell 的
    ///   **Invoke 只做"选中"**，不是"打开"。实测（2026-09-29 用户日志）：点桌面「Edge」、
    ///   点列表项「Microsoft Edge」后回执说"已触发（InvokePattern）"，但**浏览器根本没起来**，
    ///   模型只能继续瞎试。⇒ 宿主对这类目标必须**双击**（`clickCount=2`）才是"打开"。
    bool shellIconItem = false;
};

/// ★★ shell 图标宿主类名判据（**纯函数**，可逐格自检）。
///
/// 判据只看**结构**不看名字：`SHELLDLL_DefView` 是 Windows 外壳视图的宿主窗口类 ——
/// 「桌面图标」（Progman/WorkerW 下）与「资源管理器文件列表」（CabinetWClass 下）的项，
/// 父链上都会经过它；别的应用里的普通 ListView 不会。
/// ⚠ 故意**不**收 `CabinetWClass`/`Progman` 单独出现的情况：资源管理器窗口里嵌的
///   WebView/其它控件父链上也有它们，那样会把「网页里的按钮」误判成桌面图标。
bool ClassNameIsShellIconHost(const wchar_t* className);

/// 枚举窗口（hwnd=nullptr → 前台窗口）里可交互的 UIA 控件
/// 只收有名字的可交互类型；按屏幕阅读顺序排序后编号。
/// offscreenSkipped（可选出参）：被「在视口外」滤掉的可交互控件条数。滚动列表
/// （历史记录/书签/文件列表）常走这条 —— 调用方应把它带回去告诉模型「还要滚动」。
std::vector<UiControlInfo> ListInteractiveUiControls(HWND hwnd = nullptr, int maxCount = 60,
    int* offscreenSkipped = nullptr);

/// ★控件类型 → **动作能力动词**（纯函数，与 UIA 无关 ⇒ 自检可逐格钉住）。
/// 取值：`click` / `fill` / `toggle` / `select` / `slide` / `scroll` / `focus`。
/// 它对标 Windows-MCP UI 树每行的 `[action: …]`，回答的是「这个控件支持哪一类操作」。
///
/// ⚠ **只描述能力，不描述做法**：不返回「先点一下再输入」这类策略，也不返回禁令
///   （本项目总原则见 docs §48：「引擎只做感知 + 执行 + 如实回执」）。
/// ⚠ 未知类型返回 `focus` 而不是 `click`：点一个说不清是什么的东西**会打错目标**，
///   而「可聚焦」是几乎任何可交互控件的共同下限，不构成任何误导性的能力承诺。
const wchar_t* UiActionVerbForControl(int controlTypeId);

/// ★在册控件类型的**类型表**（`ui_element_probe.cpp` 的单一事实来源）。
///
/// 为什么要有这张表、而不是让自检自己抄一份常量：
/// ① 自检若自己硬编码 `UIA_*ControlTypeId`，它就必须拉 COM 头（只为几个整数）；
/// ② 更要紧的是**两份常量必然漂移** —— 加了类型而自检表没跟上，测试依然全绿，
///    而那一类控件在模型眼里会悄悄退回「说不清是什么」。
/// ⇒ 让自检遍历**实现用的同一张表**（本仓 §43「静态文案 × 动态工具表必须同一份事实」的同一条纪律）。
struct UiControlTypeRow {
    int controlTypeId;
    const wchar_t* label;    ///< 中文角色标签（按钮/输入框/滑块…）
    const wchar_t* action;   ///< 动作能力动词
    bool needsFocusable;     ///< 是否必须「可聚焦」才收（容器/文档类）
};
/// 返回在册类型的数组（长度写入 `outCount`）。
const UiControlTypeRow* UiControlTypeTable(int* outCount);

/// 把控件列表格式化成给模型看的紧凑文本（编号 + 类型 + 名字 + 状态），超预算截断。
/// ★ 截断时**必须**回传「共 N 条 / 还剩几条」，只回一句「…(截断)」会让模型
///   误判列表长度，转而去 zoom 读像素（实测白烧数轮）。
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
    std::wstring& outWarn, bool* outShellIconItem = nullptr);

/// 遮挡校验：屏幕点是否落在「前台顶层窗口本身 / 其后代 / 其拥有的弹窗」上。
/// false = 该点属于别的程序（或被子窗口挡住），此时点击会打错目标，应拦截。
/// 用 GA_ROOT + 同进程 + owner 链判定，避免把菜单/下拉这类弹窗误判为遮挡。
bool IsScreenPointOnForegroundWindow(int screenX, int screenY);

// ── 窗口**非客户区**（标题栏/边框）：识图落点绝不允许落在这里 ──────────────
//
// 由来（真实事故，docs §41.1）：模型要关「游戏内的卡牌面板」，识图补点落在窗口右上角
// `(2522,16)` —— 距顶 16px、距右边 51px，正是**窗口自己的关闭按钮**，一击把整个游戏
// 窗口关掉、进度丢失。
//
// ⚠⚠ 为什么**不能只靠上一条 UIA 守卫**（`UiElementState::titleBarControl`）：
//   实测游戏窗口 **`UIA 控件 0 条`**（`ElementFromPoint` 拿不到元素）⇒ 那道守卫
//   **根本不会触发**；而 `IsScreenPointOnForegroundWindow` 只回答「是不是这个窗口的」
//   ⇒ 标题栏照样放行。**结构事实（几何）比控件树可靠**：非客户区是纯 Win32 信息，
//   任何窗口都有，游戏/自绘程序一样量得到。

/// 屏幕点是否落在「窗口矩形内、客户区矩形外」= 标题栏 / 边框 / 系统菜单区。
/// 纯几何，两个矩形都必须是**屏幕坐标**。
/// ⚠ **头文件内联**（同 `low_power_mode.h` / `findimage_gpu.h` 的做法）：这是零依赖的纯判据，
///   内联之后任何自检目标都能直接断言它，**不必把整个 UIA 探测层链进去** ——
///   守门员越容易写，越不会没人守。
inline bool IsPointInWindowNonClientStrip(int x, int y, const RECT& windowRect,
    const RECT& clientRectScreen) {
    const POINT pt{ x, y };
    if (!PtInRect(&windowRect, pt)) return false;        // 压根不在这个窗口上
    if (PtInRect(&clientRectScreen, pt)) return false;   // 在客户区里 = 内容区，放行
    return true;                                         // 窗口内、客户区外 = 标题栏/边框
}

struct NonClientPointInfo {
    /// 是否成功量到窗口/客户区矩形。false = 判不了 ⇒ 调用方**放行**（勿误拦）。
    bool probed = false;
    bool nonClient = false;
    /// 该点 UIA 元素名（诊断用，可能为空 —— 游戏窗口实测取不到）
    std::wstring hint;
    RECT windowRect{};
    RECT clientRect{};
};

/// 探测屏幕点是否落在窗口的非客户区。`hwnd=nullptr` → 前台顶层窗口。
/// ⚠ 窗口/后台窗口模式跑脚本时应传**目标窗口**（点击坐标是相对它的）。
NonClientPointInfo ProbeWindowNonClientAtPoint(HWND hwnd, int screenX, int screenY);

}  // namespace windowmode
