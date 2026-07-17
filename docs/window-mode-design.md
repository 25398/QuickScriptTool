# 窗口模式（隐藏桌面）设计方案

> 版本：v0.1 框架稿  
> 目标：优先满足 **P0（光标/焦点零打扰）** 与 **P1（窗口可被遮挡仍可找图）**，P2 尽量覆盖大部分桌面程序。  
> **硬性需求（优化禁改坏）**：见 `src/window_mode/window_mode_requirements.h`（指定窗口类按身份打开、启动不切宏桌面视图、等待要短）。

---

## 1. 背景与约束

### 1.1 现状

当前宏引擎为**整机前台自动化**：

| 能力 | 实现 | 文件 |
|------|------|------|
| 鼠标 | `SetCursorPos` + `SendInput` | `action_utils.cpp`, `main_window.h` (`executeOne`) |
| 键盘 | `SendKeyboardKey` / `SendInput` | `action_utils.cpp` |
| 找图 | `CaptureScreenRegion`（屏幕 BitBlt） | `image_match.cpp` |
| 坐标 | 虚拟屏幕绝对坐标 | `script_types.h` (`ScriptAction::x/y`) |
| 录制 | 全局 LL 钩子，`MSLLHOOKSTRUCT.pt` | `recorder.cpp`, `main_window.h` |

### 1.2 目标（用户优先级）

| 级别 | 要求 |
|------|------|
| **P0** | 用户当前桌面的光标、焦点完全不动 |
| **P1** | 目标窗口可被其他窗口遮挡；找图/操作仍尽量可用 |
| **P2** | 尽量覆盖大部分 Win32/Office 类程序；游戏/强 GPU 界面不保证 |

### 1.3 核心策略

采用 **隐藏桌面（Hidden Desktop）隔离执行**：

```
用户默认桌面                         宏桌面 (QuickScriptMacro)
────────────────                     ─────────────────────────
用户正常使用浏览器/IDE               目标程序 + 宏 worker 线程
光标/键盘/焦点不受影响        ←→      标准 SetCursorPos + SendInput
                                     找图：PrintWindow 窗口截图
```

**不采用**同桌面 `PostMessage` 作为主路径（无法满足 P0+P2）。

---

## 2. 总体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        MainWindow (UI)                       │
│  窗口模式开关 / 目标绑定 / 预览缩略图 / 健康状态指示          │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│                   WindowModeSession (会话)                   │
│  绑定目标进程、维护 HWND、桌面句柄、坐标系、健康检查           │
└───────┬──────────────────────────────┬──────────────────────┘
        │                              │
┌───────▼────────┐            ┌────────▼─────────┐
│ HiddenDesktop  │            │ WindowCapture    │
│ 创建/切换桌面   │            │ PrintWindow 截图  │
│ 进程启动到桌面  │            │ 区域/全窗口匹配   │
└───────┬────────┘            └────────┬─────────┘
        │                              │
┌───────▼──────────────────────────────▼──────────────────────┐
│              WindowModeExecutor (执行路由)                     │
│  包装 executeOne：坐标变换、桌面线程、输入代理、找图代理        │
└──────────────────────────┬──────────────────────────────────┘
                           │
              ┌────────────┴────────────┐
              │  复用现有能力            │
              │  action_utils           │
              │  image_match engines    │
              │  ocr_engine             │
              └─────────────────────────┘
```

**设计原则：**

1. **少改 `executeOne` 主体**：通过 `WindowModeExecutor` 在外层做坐标/截图/桌面切换适配。
2. **会话级窗口绑定**：第一版不做「每条动作不同窗口」，降低复杂度。
3. **接口先行、实现分阶段**：框架文件先落地，内部可先返回 stub/降级。

---

## 3. 新增模块与文件

| 文件 | 职责 | Phase |
|------|------|-------|
| `src/window_mode/hidden_desktop.h/.cpp` | 创建/打开宏桌面，`SetThreadDesktop`，在该桌面 `CreateProcess` | **1** |
| `src/window_mode/window_target.h/.cpp` | 目标解析：进程名/路径 → 主窗口 HWND；健康检查 | **1** |
| `src/window_mode/window_capture.h/.cpp` | `PrintWindow` 截图、黑屏检测、窗口客户区矩形 | **1** |
| `src/window_mode/window_coords.h/.cpp` | 客户区 ↔ 屏幕坐标变换（绑定 HWND） | **1** |
| `src/window_mode/window_mode_session.h/.cpp` | 会话生命周期：启动/停止/状态 | **1** |
| `src/window_mode/window_mode_executor.h/.cpp` | 执行期适配层，对接 `executeOne` | **2** |
| `src/window_mode/window_mode_preview.h/.cpp` | 缩略图预览（可选定时刷新） | **3** |
| `src/window_mode/window_mode_types.h` | 枚举、错误码、配置结构 | **1** |

`CMakeLists.txt` 增加上述源文件。

---

## 4. 数据模型

### 4.1 脚本级配置（第一版）

在脚本 JSON / 内存中增加**脚本头**字段（不污染每条 `ScriptAction`）：

```cpp
// window_mode_types.h
enum class WindowModeCoordinateSpace {
    ScreenAbsolute,   // 兼容旧脚本（默认）
    WindowClient      // 窗口模式：相对目标窗口客户区
};

struct WindowModeScriptConfig {
    bool enabled = false;
    std::wstring targetExePath;      // 如 C:\...\EXCEL.EXE 或 excel
    std::wstring targetWindowTitle;  // 可选，进一步过滤
    WindowModeCoordinateSpace coordSpace = WindowModeCoordinateSpace::WindowClient;
    bool autoLaunchTarget = false;   // 宏桌面内自动启动目标程序
    std::wstring launchArgs;
};
```

**序列化**（`script_io.cpp` Phase 3）：

```json
{
  "windowMode": {
    "enabled": 1,
    "targetExePath": "excel.exe",
    "targetWindowTitle": "",
    "coordSpace": "windowClient",
    "autoLaunchTarget": 1,
    "launchArgs": ""
  },
  "actions": [ ... ]
}
```

### 4.2 运行时会话

```cpp
struct WindowModeSession {
    HDESK macroDesktop = nullptr;
    HWND  targetHwnd = nullptr;
    DWORD targetPid = 0;

    // 缓存的窗口几何（执行前刷新）
    RECT clientRectScreen = {};  // 客户区映射到屏幕的包围盒
    int  clientW = 0;
    int  clientH = 0;

    WindowModeHealth health = WindowModeHealth::Unknown;
    std::wstring lastError;
};
```

### 4.3 暂不修改 `ScriptAction` 主体

第一版保持 `ScriptAction::x/y/searchX1..Y2` 字段不变，仅改变**解释方式**：

- 窗口模式 OFF：`x/y` = 虚拟屏幕绝对坐标（现状）
- 窗口模式 ON：`x/y` = 目标窗口**客户区**坐标；执行前 `ClientToScreen`

后续如需「单脚本多窗口」再扩展 `targetWindowId` 字段。

---

## 5. 核心 API 草案

### 5.1 HiddenDesktop

```cpp
namespace windowmode {

constexpr wchar_t kMacroDesktopName[] = L"QuickScriptMacroDesktop";

class HiddenDesktop {
public:
    bool OpenOrCreate();
    void Close();
    bool IsValid() const;
    HDESK Handle() const;

    // 在宏桌面启动进程（CreateProcess + lpDesktop）
    bool LaunchProcess(const std::wstring& exe, const std::wstring& args, PROCESS_INFORMATION& outPi);

    // 将当前线程绑定到宏桌面（worker 线程入口调用）
    bool AttachCurrentThread();
    bool DetachCurrentThread();  // 回到 Input desktop
};

}  // namespace windowmode
```

### 5.2 WindowTarget

```cpp
enum class WindowModeHealth {
    Ok,
    Unknown,
    DesktopNotReady,
    TargetNotFound,
    TargetMinimized,      // IsIconic
    TargetNoRender,       // PrintWindow 黑屏/空
    CaptureFailed,
    PermissionMismatch,   // UIPI / 管理员
};

struct WindowTargetQuery {
    std::wstring exePath;
    std::wstring titleContains;  // 可选
    DWORD pid = 0;               // 可选，优先
};

HWND FindMainWindowOnDesktop(const WindowTargetQuery& q, HDESK desktop);
WindowModeHealth EvaluateTargetHealth(HWND hwnd, HDC probeDc = nullptr);
```

### 5.3 WindowCapture

```cpp
struct WindowCaptureResult {
    HBITMAP bitmap = nullptr;
    int x = 0;  // 与 image_match 一致：截图左上角屏幕坐标
    int y = 0;
    int w = 0;
    int h = 0;
    bool fromPrintWindow = true;
};

WindowCaptureResult CaptureWindowClient(HWND hwnd);
WindowCaptureResult CaptureWindowRegion(HWND hwnd, int cx1, int cy1, int cx2, int cy2);

// 黑屏/无效检测（均值像素、全零等）
bool IsCaptureLikelyBlank(HBITMAP bmp);
```

### 5.4 WindowCoords

```cpp
bool ClientToScreenPoint(HWND hwnd, int cx, int cy, int& sx, int& sy);
bool ScreenToClientPoint(HWND hwnd, int sx, int sy, int& cx, int& cy);
void MapClientRectToScreen(HWND hwnd, int cx1, int cy1, int cx2, int cy2,
                           int& sx1, int& sy1, int& sx2, int& sy2);
```

### 5.5 WindowModeExecutor（适配层）

```cpp
class WindowModeExecutor {
public:
    explicit WindowModeExecutor(WindowModeSession& session);

    // 执行前：刷新 HWND、健康检查、线程绑定桌面
    bool BeginRun(std::wstring& err);

    // 包装现有原语
    void MoveMouseClient(int cx, int cy, int rx, int ry);
    void MouseClickClient(int cx, int cy, ...);
    void SendKey(...);  // 直接复用 action_utils，桌面线程内 SendInput

    // 找图：窗口截图 + 现有匹配引擎
    ImageMatchOutput FindImageClientRect(int cx1, int cy1, int cx2, int cy2, HBITMAP templ, ...);

    void EndRun();
};
```

---

## 6. 与现有代码的集成点

### 6.1 执行入口（`main_window.h`）

在 `StartActionsWorker` / `executeOne` 外层：

```cpp
WindowModeScriptConfig wmCfg = LoadWindowModeFromScript(currentScript);
WindowModeSession wmSession;
WindowModeExecutor wmExec(wmSession);

if (wmCfg.enabled) {
    if (!wmExec.BeginRun(err)) { /* 弹窗 + 中止 */ }
}

// executeOne 内部：
if (wmCfg.enabled) {
    wmExec.MoveMouseClient(a.x, a.y, a.randomX, a.randomY);
} else {
    SetCursorPos(...);  // 现状
}
```

**Phase 1** 只接好分支骨架，`BeginRun` 可仅完成桌面创建 + HWND 查找日志。

### 6.2 找图（`executeOne` 中 `FindImage` 分支）

| 模式 | 截图来源 | 搜索坐标 |
|------|----------|----------|
| 普通 | `CaptureScreenRegion` / frozen screen | 屏幕绝对 |
| 窗口 | `CaptureWindowClient` + 裁切 | `searchX1..Y2` 为客户区坐标 |

匹配仍调用 `FindTemplateInFrozenScreenMulti`（传入窗口截图 + 偏移）。

### 6.3 录制（`recorder` / `ConvertRecordedToActions`）

窗口模式录制必须在**宏桌面**进行（Phase 2）：

1. UI 按钮「在窗口模式开始录制」
2. 录制线程 `AttachCurrentThread` 到宏桌面
3. 钩子采集的 `pt` 经 `ScreenToClientPoint(targetHwnd)` 存入 `x/y`

Phase 1 可先用**手动坐标** + 十字准星在预览窗上选点。

### 6.4 十字准星（`crosshair_drag.cpp`）

Phase 2：增加「窗口模式拾取」——在预览缩略图上拾取，返回客户区坐标。

### 6.5 设置（`app_settings.h`）

```cpp
struct WindowModeSettings {
    bool enabledByDefault = false;
    bool showPreviewThumbnail = true;
    int previewRefreshMs = 500;
    bool blockRunWhenUnhealthy = true;
};
```

---

## 7. 分阶段实施计划

### Phase 1 — 框架骨架（最先落地，约 1 周）

**目标：** 模块存在、可编译、可手动验证桌面/HWND/截图；不改变默认宏行为。

| 任务 | 产出 |
|------|------|
| 新建 `window_mode/*` 模块 | 空实现 + 日志 |
| `HiddenDesktop::OpenOrCreate` | 能创建桌面名 `QuickScriptMacroDesktop` |
| `LaunchProcess` on desktop | 能在宏桌面启动记事本 |
| `FindMainWindowOnDesktop` | 枚举桌面窗口找 HWND |
| `CaptureWindowClient` | PrintWindow 返回 HBITMAP |
| `EvaluateTargetHealth` | 检测最小化、黑屏 |
| `WindowModeScriptConfig` 内存结构 | 暂不接 JSON |
| MainWindow 菜单/开关 | 「窗口模式（实验）」勾选 + 目标 exe 路径 |
| 调试按钮 | 「测试：启动目标到宏桌面」「测试：截图保存」 |

**验收：**

- 用户桌面光标不动
- 宏桌面内记事本可启动
- PrintWindow 能保存一张 bmp
- 最小化时 `TargetMinimized` 有明确提示

### Phase 2 — 执行闭环（约 1–2 周）

| 任务 | 产出 |
|------|------|
| `WindowModeExecutor` 接入 `executeOne` | MoveMouse / Click / Key / Scroll |
| FindImage 窗口路径 | 客户区坐标找图 |
| OCR / AI 截图区域 | 复用 `MapClientRectToScreen` |
| 运行前健康检查 gate | unhealthy 则阻止运行 |
| 坐标变换单测/日志 | 客户区 ↔ 屏幕 |

**验收：**

- 宏桌面内 Excel/记事本：点击、输入、找图跑通简单脚本
- 用户桌面全程无焦点变化

### Phase 3 — 体验补全（持续）

| 任务 | 产出 |
|------|------|
| 宏桌面内录制 | 客户区坐标录制 |
| 预览缩略图控件 | 可选刷新 |
| `script_io` / `script_action_builder` 序列化 | `windowMode` 头 |
| Agent 文档 `agent_reference.cpp` | 窗口模式字段说明 |
| LockScreenshot 窗口版 | 冻结窗口截图而非全屏 |
| 权限检测 | 管理员提示 |

### Phase 4 — 增强（可选）

- 多窗口 profile（单脚本多目标）
- 窗口模式 + 计划任务
- 同屏 `PostMessage` 实验通道（明确标注不保证 P0）
- 失败自动重试 / 重新绑定 HWND

---

## 8. UI 草案（最小）

```
┌─ 设置 / 脚本工具栏 ─────────────────────────────────────┐
│ [x] 窗口模式（隐藏桌面）   目标程序: [excel.exe ▼] […]  │
│ 状态: ● 就绪  |  HWND: 0x...  |  [预览] [测试截图]      │
└────────────────────────────────────────────────────────┘
```

- **目标程序**：复用 `crosshair_drag` / `GetProcessPathFromPoint` 拾取 exe
- **状态灯**：绿/黄/红 对应 `WindowModeHealth`
- **预览**：小窗显示 `CaptureWindowClient` 缩略图（Phase 3）

运行宏时若 `blockRunWhenUnhealthy` 且 health != Ok，弹窗列出原因与建议。

---

## 9. 健康检查与错误提示

| Health | 检测方式 | 用户提示 |
|--------|----------|----------|
| `TargetMinimized` | `IsIconic(hwnd)` | 目标窗口在宏桌面中最小化，请还原 |
| `TargetNotFound` | 枚举无匹配 HWND | 请先在宏桌面启动目标程序，或勾选自动启动 |
| `TargetNoRender` | PrintWindow 黑屏检测 | 该程序可能不支持后台截图（游戏/GPU 界面） |
| `PermissionMismatch` | OpenProcess 失败 / UIPI | 请以相同权限运行本工具与目标程序 |
| `CaptureFailed` | PrintWindow 返回 false | 截图失败，稍后重试 |

---

## 10. 不支持场景（产品声明）

1. 目标窗口在宏桌面内**最小化**
2. 游戏、全屏 3D、部分 Electron/Chromium 页面（截图或输入不可靠）
3. 目标与本工具**权限不一致**（管理员 vs 普通）
4. 锁屏、UAC 安全桌面、休眠中执行
5. 远程桌面会话断开

**明确不是缺陷：** 用户在**自己桌面**上用其他窗口遮挡 → 不影响宏桌面执行。

---

## 11. 风险与对策

| 风险 | 对策 |
|------|------|
| `CreateDesktop` 权限/泄漏 | RAII 包装，`CloseHandle`；进程退出时清理桌面 |
| HWND 运行中变化 | 每步/每 N 步 `FindMainWindowOnDesktop` 重绑 |
| DPI 缩放 | `GetDpiForWindow` + `WM_DPICHANGED` 后刷新坐标 |
| 多显示器 | 宏桌面内仍用 `ClientToScreen`，不依赖用户当前显示器 |
| PrintWindow 失败 | 降级 `CaptureScreenRegion(GetWindowRect)` 并打日志（仅宏桌面内有效） |
| 与现有 `LockScreenshot` 冲突 | 窗口模式改用 `LockWindowCapture` 冻结窗口位图 |

---

## 12. Phase 1 具体任务清单（开发可直接开工）

```
[ ] CMakeLists.txt 添加 window_mode 源文件
[ ] window_mode_types.h — 枚举与配置结构
[ ] hidden_desktop.cpp — OpenOrCreate / LaunchProcess / AttachCurrentThread
[ ] window_target.cpp — FindMainWindowOnDesktop / EvaluateTargetHealth
[ ] window_capture.cpp — CaptureWindowClient / IsCaptureLikelyBlank
[ ] window_coords.cpp — ClientToScreen 封装
[ ] window_mode_session.cpp — Start / Stop / RefreshTarget
[ ] main_window.h — 实验性 UI 开关 + 「测试宏桌面」按钮
[ ] 手动测试脚本：宏桌面启动 notepad → 截图 → 用户桌面焦点不变
```

---

## 13. 后续扩展预留

- `WindowModeExecutor` 保持薄适配层，便于加「同屏 PostMessage 后端」
- `WindowCapture` 可插 `WGC`（Windows Graphics Capture）引擎
- `WindowModeScriptConfig` 可扩展 `desktopName`、`captureEngine`
- 与 AI Agent 工具链对接：脚本头声明窗口模式参数

---

*文档维护：实现 Phase 1 后回填「实测程序兼容表」。*
