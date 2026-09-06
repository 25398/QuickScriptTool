# WebView 原生分层盘点（阶段 0）

> 产品主路径：`QstWebViewShell` = **WebShell**（`ui/`）+ **Engine**（跑宏/录制/热键/设置）+ **DesktopTools**（选区/叠层/准星/托盘等）。  
> **GdiLegacy** 仅 `forceNative` / `QuickScriptTool.Gdi.exe`；发版默认应可关闭。  
> 设计真值：`docs/ui-redesign-mockup.html`；桥接契约：`docs/webview-bridge-api.md`。  
> 本文件是归属真值；改目录/CMake/调用边时同步更新。

---

## 分层定义

| 层 | 职责 | 用户可见？ |
|----|------|------------|
| **WebShell** | WebView2 宿主窗、bridge 路由、窗体尺寸/圆角、藏壳协调 | 是（唯一产品壳） |
| **Engine** | 跑宏/停宏/录制/连点/热键注册/设置持久化/窗口模式运行时；无产品 GDI 主页 | 否（headless） |
| **DesktopTools** | Win32 顶层手势/叠层/提权/托盘/独立调试窗；参数进、结果出 | 瞬时原生窗 |
| **GdiLegacy** | 旧对话框 + MainWindow 产品绘制 | 仅 Gdi.exe / forceNative |
| **Shared** | 脚本 IO、找图引擎、OCR 引擎、输入后端、工具函数；多目标共用 | 否 |

**永久原生 ≠ 永久保留 `main_window` 产品壳。** 永久原生 = 必须是 Win32 顶层窗/系统 API，由 DesktopTools 提供。

归类口诀：用户可见表单 → Web；桌面像素/跨进程手势/提权/托盘/壳藏后仍要存活的窗 → DesktopTools；跑脚本状态机 → Engine；仅 Gdi/forceNative → Legacy。

---

## 源文件归属

### WebShell

| 路径 | 说明 |
|------|------|
| `src/webview/qst_webview_shell.cpp` | WebView2 壳、托盘 NotifyIcon、`HandleBridgeMessage` |
| `ui/index.html` / `ui/shell.css` / `ui/bridge.js` / `ui/app.js` | 产品 UI |
| `docs/ui-redesign-mockup.html` | 设计真值（非运行时） |

### Engine（目标：无可见产品 UI）

| 路径 | 说明 | 现状备注 |
|------|------|----------|
| `src/engine/qst_engine.h` + `webview/qst_engine_host.h` | 产品 Engine API | 实现：`engine_runtime.cpp` |
| `src/engine/engine_runtime.cpp` | Start/跑停/热键/录制/连点/Reload | E4.1 |
| `src/engine/engine_host_window.h` | 引擎宿主窗类（原 main_window.h）→ **`EngineHost`** | 产品 TU 含此头；**不含** `main_window.h`；**F1.1**：GDI 对话框头经 `engine_host_window_gdi.h`；headless **不**自建托盘（仅 Web 壳 NIM_ADD） |
| `src/engine/engine_host_window_gdi.h` | settings/agent/sched/opt/ocr_install/window_pick 对话框 include 束 | 仅 GdiLegacy / `QST_GDI_LEGACY=1` |
| `src/webview/webview_bridge_backend.h/.cpp` | 设置/脚本 IO/Agent/优化等 JSON 服务 | 部分桌面能力已迁出至 DesktopTools |
| `src/clicker.cpp` | 连点 | 含 `engine_host_window.h` |
| `src/recorder.cpp` / `recorder_timeline.cpp` / `input_timeline_scheduler.cpp` | 录制回放 | |
| `src/macro_variables.cpp` / `macro_execute_tools.cpp` | 宏变量 / 执行工具 | |
| `src/script_io.*` / `script_action_builder.*` | 脚本读写与动作构建 | Shared 亦可 |
| `src/app_settings_store.*` / `app_settings.h` | 设置持久化 | |
| `src/scheduled_task_store.*` / `scheduled_task_scheduler.*` / `scheduled_task_types.*` | 定时任务运行时 | |
| `src/window_mode/*`（除 `window_pick_dialog`） | 窗口模式运行时 / CDP / 捕获 / 预览逻辑 | `window_mode_preview` 可被 Web `#wmPreview` 消费 |
| `src/ai_action_*.cpp` / `agent_*.cpp`（除 `agent_dialog`） | Agent / AI 动作 | 逻辑属 Engine；对话框属 Legacy |

### DesktopTools（永久原生能力；门面：`src/desktop_tools/`）

| 路径 | bridge `type` / 用途 |
|------|----------------------|
| `src/desktop_tools/desktop_tools.h/.cpp` | **唯一门面入口**（A1+） |
| `src/screenshot_overlay.*` | `pickScreenRegion` / `captureTemplateScreenshot` |
| `src/match_overlay.*` | `findImageMatch` |
| `src/findimage_crop_editor.*` / `findimage_template_crop.*` | `findImageCrop`：B3 后产品 UI 为 Web `#ov-crop`；写盘走 `FindImageCropRect`；GDI 编辑器后备 |
| `src/ocr_overlay.*` | `testOcr`（offset 模式叠层） |
| `src/crosshair_drag.*` | `crosshairPick` |
| `src/hotkey_dialog.*`（`HotkeyCapture`） | `captureGlobalHotkey` / `captureScriptHotkey`；动作键产品走 Web `#ov-action-key` |
| `src/macro_debug_window.*` | Gdi 原生后端；**产品**另开 WebView 顶层窗 `ui/debug.html`（`SetMacroDebugWebPoster`） |
| `src/tray_menu.*` / `themed_popup_menu.*` | 系统托盘菜单 |
| `src/process_utils.*` | 准星取窗/进程路径 |
| `src/ocr_install_dialog.*` | 历史 GDI 安装 UI；Web 主路径已走 `RunOcrInstall` 进度桥 | 安装逻辑可 Shared；对话框可 Legacy |
| UAC `ShellExecute`（门面内） | `installDriver` |

### GdiLegacy（可删 / `QST_BUILD_GDI_LEGACY`；实现归档于 `archive/gdi_legacy/`）

| 路径 | 说明 |
|------|------|
| `src/main.cpp` | GDI 产品入口 |
| `src/main_window.h` | **仅 Gdi shim** → `engine/engine_host_window.h`；产品禁止 include |
| `src/*.h` + `archive/gdi_legacy/*.cpp` | 旧对话框：settings/agent/sched/opt/theme/ocr_install/window_pick（头仍在 src，实现归档） |
| `src/editor_paint.*` / `editor_dropdown.*` | GDI 编辑器绘制（**OFF：stubs**；ON：真实 TU） |
| `src/controls.*` / `modern_edit.*` / `popup_combo.*` / `panel_popup_combo.*` / `prompt_modal.*` / `ui_component.*` | GDI 控件栈（`modern_edit` 在 DesktopTools；其余 OFF→`gdi_engine_ui_stubs`；`ApplyFont`/`Scale*`→`win32_ui_util`） |
| `src/drawing.*` / `render_*` / `ui_scale.*` | **DesktopTools / Shared**，禁止当 Legacy 删 |
| `src/main_window.h` / `engine_host_window.h` 中产品 UI / 对话框 Show 路径 | Gdi 绘制仍缠在宿主窗类；**禁止一次 PR 删除宿主窗头** |
| ~~`src/taskbar_iconic_preview.*`~~ | 已移出 engine（D2.3） |
| `src/gdi_legacy_stubs.cpp` / `src/gdi_engine_ui_stubs.cpp` | `QST_BUILD_GDI_LEGACY=OFF` 空实现 |

### Shared（引擎 + Tools + 自检共用；删 Legacy 时保留）

| 路径 | 说明 |
|------|------|
| `src/image_match.*` / `image_match_engines.*` | 找图引擎 |
| `src/ocr_engine.*` / `ocr_result.h` | OCR 引擎（非叠层） |
| `src/coord_space.*` | 坐标 / `BuildExecutionFindImageOptions` |
| `src/input/*` | 注入后端（禁止 WebView 内 SendInput） |
| `src/utils.*` / `config.h` / `script_types.h` / `app_theme.*` / `app_branding.*` | 通用 |
| `src/find_image_ui_debug.*` | 找图调试辅助 |
| `src/recording_to_findimage.*` | 录制→找图转换 |
| `driver/qst_vhid/*` | 虚拟 HID 驱动与安装脚本 |
| `extension/edge/*` | Edge 配套扩展（发版必带） |

---

## 删除 GdiLegacy 时允许断的符号

发版 `QST_BUILD_GDI_LEGACY=OFF` 且不链下列对话框/主页绘制时，**允许未定义 / 不链接**：

- `SettingsDialog::*`、`AgentDialog::*`、`ScheduledTaskDialog::*`、`RecordingOptimizeDialog::Show*`（若算法已抽到 Shared，保留算法符号）
- `ThemeCustomDialog::*`、`WindowPickDialog::*`
- `MainWindow` 上所有「画主页 / 开 GDI 编辑器 / ShowXxxDialog」路径
- `forceNative:1` 分支：bridge 应返回明确错误（如 `legacy_unavailable`），**不得**静默失败成空操作冒充成功
- GDI `main.cpp` / `QuickScriptTool.Gdi.exe` 目标整体可不编

**禁止断**（误删即产品残废）：

- `ScreenshotOverlay` / `MatchOverlay` / `OcrOverlay` / `CrosshairDragController` / `FindImageCropEditor`（或 B3 Web 裁切替代）
- `HotkeyCapture`、`MacroDebugWindow`、托盘 NotifyIcon
- `image_match` / `ocr_engine` / `script_io` / `window_mode` 运行时 / `qst_engine_host` API
- `qst::desktop_tools::*` 门面

---

## DesktopTools 对外 C++ API 草表（↔ bridge）

命名空间：`qst::desktop_tools`。入口头：`src/desktop_tools/desktop_tools.h`。  
约定：`HWND owner` = Web 壳窗（或合法顶层 owner）；内部需要时用 `ScopedHideShell` 藏壳；**准星禁止 SW_HIDE**（靠移出屏外）。

| C++ API | bridge `type` | 结果要点（JSON 兼容） |
|---------|---------------|------------------------|
| `ScopedHideShell(HWND)` | （辅助） | RAII 藏/还壳 |
| `PickScreenRegion(owner)` | `pickScreenRegion` | `x1,y1,x2,y2` 屏幕坐标 |
| `CaptureTemplateScreenshot(owner)` | `captureTemplateScreenshot` | `imagePath` + `resolvedPath` |
| `FindImageMatch(owner, params)` | `findImageMatch` | `mode,offset*,region*,found,matchCount,bestScore` |
| `FindImageCrop(owner, path, ox, oy, follow)` | `findImageCrop` | `imagePath,offsetX/Y` 或 `unchanged` |
| `PickWindowTarget(owner)` | （引擎 SelectOnStartup） | 准星点选窗口 → 填 WindowMode 配置 |
| `CaptureActionKey(owner, old)` | （DesktopTools API；产品桥已移除，动作键走 Web） | `keyVk,keyText,holdLeft*` |
| `CaptureHotkey(owner, params)` | `captureGlobalHotkey` / `captureScriptHotkey` | `hotkeyText/Vk/Modifiers/Hold`；捕获放行由调用方 `begin/end` 回调接 Engine |
| `TestOcr(owner, params)` | `testOcr` | `mode=test\|offset` + text/offset |
| `InstallDriver(owner, kind, onProgress)` | `installDriver` | ok + probed/suggestedBackend（探测可留 Shell） |
| `RequestShowDebugWindow(fn)` | `showDebugWindow` | headless → Web `#debugFloat`；Gdi → `MacroDebug` 原生窗；`fn` 通常为 `ReloadSettings`/Apply |
| `BrowsePath` / `PickImageFile` | `browsePath` / `pickImageFile` | 系统文件对话框（可归 Tools） |

Tray：`TrayMenu::Show` 仍属 DesktopTools；菜单项扩展（停宏等）在阶段 A3。

热键定案（B4）：捕获机制留 DesktopTools（`HotkeyCapture`）用于启停/脚本；动作按键产品走 Web `#ov-action-key`；统一入口文案「启停热键 / 脚本热键 / 动作按键」；脚本捕获中隐藏 `#ov-hotkey`。

---

## 分期完成度（滚动更新）

| 阶段 | 状态 | 说明 |
|------|------|------|
| **0 盘点** | **done** | 本文 |
| **A1 门面** | **done** | `src/desktop_tools/desktop_tools.h/.cpp` |
| **A2 bridge 改调门面** | **done** | Shell / `webview_bridge_backend` 桌面 type 经门面；JSON 兼容 |
| **A3 托盘/调试窗** | **done** | 托盘「停止当前运行」；产品调试 → Web `#debugFloat`；Gdi 仍 MacroDebugWindow |
| **A4 双通道噪音** | **done** | `#wmPreview` only；隐藏 preferDirect2D；诚实 checkUpgrade |
| **B1 Engine 瘦身** | **done** | headless：`EngineOpenScheduledTasks` no-op；`Start` 强化 `SW_HIDE`+TOOLWINDOW；`CanOpenNativeScheduledTasks()`；Shell 不经 `Get()` 开产品窗 |
| **B2 CMake 拆目标** | **done** | OBJECT：`qst_desktop_tools` / `qst_engine`；`QST_GDI_LEGACY`；`forceNative`→`legacy_unavailable` |
| **B3 裁切迁 Web** | **done** | `#ov-crop` + `FindImageCropRect` |
| **B4 热键文案定案** | **done** | 三入口文案统一；脚本捕获中隐藏 Web |
| **C 对话框归档** | **done** | 默认 OFF；`archive/gdi_legacy/` + stubs；`PickWindowTarget` |
| **D1.1 captureScope** | **done** | 钩子按窗口/全局过滤；Web `#recScope`；`EvaluateRecordingScopeFilter` selftest |
| **D1.2 调用边** | **done** | Shell 直接 `desktop_tools`；删除 backend `InstallDriverKind`/`RunCrosshairPick`/`BrowsePathDialog` 包装 |
| **D1.3 / D2.1 清单** | **done** | 见下文「D2 迁出候选」与 Engine API 表 |
| **D2.2 Engine API** | **done** | 产品门面固化为 `src/engine/qst_engine.h`（→ `qst_engine_host`）；新增代码禁止扩 EngineHost 产品 UI |
| **D2.3 移源** | **done**（E2+F） | `taskbar_iconic_preview` 已移出；控件栈 OFF 仅 `gdi_engine_ui_stubs`（真实 TU 仅 ON） |
| D2.4 MacroDebug 归属 | **done**（E3.1） | `desktop_tools::MacroDebug()` 持有；MainWindow 仅 Apply/Emit |
| **D3 发版文档** | **done** | AGENTS / package：产品=Web；Gdi 仅 ON 可选 |
| D4 拆穿 MainWindow | **done**（E4） | 产品 `#include "main_window.h"` = 0；宿主 → `engine/engine_host_window.h` |
| **E1.1 headless 跳过编辑器 HWND** | **done** | `Init` 不 `CreateEditorControls`/popup；`Paint` 早退 |
| **E1.2 静态引用审计** | **done** | 见下文「E1.2 审计结果」 |
| **E1.3 prompt_modal 产品提示** | **done** | headless → `engine.toast` / MessageBox；跑宏健康检查走 `ShowPromptInfo` |
| **E2 移源出产品 engine** | **done**（链接） | OFF 链 stubs；真实 controls/editor_*/popup_*/prompt_modal/ui_component 仅 ON；物理归档见 F3 |
| E3.1 MacroDebug 拥有权 | **done** | `qst::desktop_tools::MacroDebug`；显隐仍经 Engine ApplyDebugWindowSetting |
| **E4.1 引擎职责搬家** | **done** | 热键/跑停/录制/连点/Reload → `src/engine/engine_runtime.cpp` |
| **E4.2 产品 main_window.h include** | **done** | 产品 OFF TU = 0；仅 Gdi `main.cpp`/`editor_*` 经 shim |
| **E5 forceNative/发版文案** | **done** | UI 无 forceNative 入口；发版 README 标明产品=Web |
| **F1.1 host 对话框 include 瘦身** | **done** | OFF 不拉 settings/agent/… 完整头；`engine_host_window_gdi.h` |
| **F1.2 文档对齐** | **done** | D2.3/E2 与 stub 叙述一致 |
| **F1.3 死文件** | **done** | 无 `qst_engine_host.cpp`；实现仅 `engine_runtime.cpp` |
| **F2 拆巨石 .h→多 cpp** | **done**（核心） | `engine_hotkeys` / `script_run` / `record_click` / `settings_reload` / `window_mode_hooks` / `gdi_editor`；头 ~17.5k→~12.1k |
| **F3.1 GDI 源物理归档** | **done** | `archive/gdi_legacy/engine_ui/*`；OFF 仅 stubs |
| **F3.2 Gdi 目标默认不建** | **done** | `QST_BUILD_GDI_LEGACY` 默认 OFF；选项标注 DEPRECATED；不删目标 |
| **F4 Web 收尾** | **partial** | 驱动安装去 `alert`；找图缩略图/Agent 取消失败补 toast；升级检查仍 stub（文档） |
| **F5.1 MainWindow→EngineHost** | **done** | 类名全仓替换；`FindMainWindow*` / `RestoreMainWindow*` 保留 |
| **产品路径死代码清理** | **done** | 删未调用 `OpenThemeCustomPicker`；移除产品 `captureActionKey` 桥；CMake 去掉未用 DuiLib include |
| **拆除 GDI 逃生舱** | **done** | 删 `archive/gdi_legacy/`、`QuickScriptTool.Gdi`、`main.cpp`、`FindImageCropEditor`；裁切仅 `FindImageCropRect`；`forceNative`→`legacy_unavailable`；删 `DuiLib_Ultimate` 与探测杂物。**保留** DesktopTools 叠层/准星/热键捕获/托盘；EngineHost 头内 GDI 绘制仍编译但 headless 不执行（链 stubs） |

### E4.2 — 剩余仅历史遗留（无 Gdi.exe）

| 文件 | 说明 |
|------|------|
| `src/gdi_legacy_stubs.cpp` / `gdi_engine_ui_stubs.cpp` | 满足 EngineHost 头符号链接 |
| `src/engine/engine_gdi_editor.cpp` | headless 下多数路径不跑；后续可再 stub 化 |

---

## 验证约定

每完成一块：Web 按钮点通对应桌面工具 + 相关 `module-selftest --json` exit 0（见 `AGENTS.md` / `.cursor/skills/module-selftest`）。  
Bridge JSON 字段保持兼容；契约变更必须改 `docs/webview-bridge-api.md`。

### B2 构建目标（MSBuild）

```powershell
# 产品壳（默认 QST_GDI_LEGACY=0）
& "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:QstWebViewShell /m /v:minimal

# 可选 GDI 逃生舱 → build\Release\QuickScriptTool.Gdi.exe
& ...\MSBuild.exe ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:QuickScriptToolGdi /m /v:minimal
```

CMake：`option(QST_BUILD_GDI_LEGACY … OFF)` / `option(QST_BUILD_WEBVIEW_SHELL …)`；源列表在 `cmake/QstProductLibs.cmake`。  
发版默认 Legacy OFF。需要 Gdi 逃生舱：`cmake -B build -DQST_BUILD_GDI_LEGACY=ON` 再 `/t:QuickScriptToolGdi`。

---

## D2 — Engine API 与迁出候选（滚动）

### 产品 Engine 门面（WebShell / bridge 只应依赖此层）

头：[`src/engine/qst_engine.h`](../src/engine/qst_engine.h) → [`src/webview/qst_engine_host.h`](../src/webview/qst_engine_host.h)。  
实现：[`src/engine/engine_runtime.cpp`](../src/engine/engine_runtime.cpp)（委托 `engine_host_window.h` 宿主窗；**无**公开 `Get()`）。

| API | 用途 | 调用方 |
|-----|------|--------|
| `Start` / `Shutdown` / `Hwnd` / `SetUiHost` | 生命周期 | Shell |
| `RunScriptPath` / `StopScript` / `IsRunning` / `ExecutedSteps` / `Running*` / `IsBreakoutPaused` | 跑宏 | Shell / status |
| `Start/Stop/ToggleClicker` / `IsClicking` / `ApplyClickerSettings` | 连点 | Shell |
| `Start/StopRecording` / `*RecordingEx` / `IsRecording` | 录制 | Shell |
| `SetActiveHomeTab` / `SelectHomeItem` / `GetHomeStateJson` | 主页态 | Shell |
| `Begin/EndHotkeyCaptureRelease` / `BeginActionKeyCaptureRelease` | 热键捕获放行 | Shell |
| `ReloadSettings` / `ReloadScriptsAndHotkeys` / `ReloadScheduledTasks` | 磁盘→运行时 | Shell / bridge |
| `GlobalHotkey*` / `SetGlobalHotkey` | 启停热键 | Shell |
| `OpenScheduledTasks` / `CanOpenNativeScheduledTasks` | 仅 GDI；headless no-op | Shell forceNative |
| `DebugWindowClosedByUser` | 调试窗 | Shell / Tools |
| ~~`Get()`~~ | 已移除 | — |

### D2 迁出候选（控件栈：OFF 已 stubs；物理归档 → F3）

| 文件 | 为何还在 | 迁出条件 |
|------|----------|----------|
| ~~`controls/editor_*/ui_component/popup_*/prompt_modal`~~ | — | **E2 done（链接）**：OFF=`gdi_engine_ui_stubs`；ON=真实 TU；F3 再物理归档 |
| `src/win32_ui_util.cpp` | `ApplyFont` / `GetThisModule` / `Scale*` | **保留**（headless 布局缩放仍用） |
| ~~`src/taskbar_iconic_preview.cpp`~~ | — | **已移出** |


**永不迁出（DesktopTools / Shared）**：`drawing` / `render_*` / `ui_scale` / `modern_edit` / overlays / `hotkey_dialog` / `macro_debug_window` / `tray_menu`。

**仅 GDI 产品 UI（勿从 Engine API 再扩）**：画主页、`ShowSettings/Agent/Sched/Optimize`、GDI 编辑器命中测试等 — 只在 `QuickScriptTool.Gdi` / `main_window` 内联路径。

---

## E1.2 审计结果（静态引用；非 main_window 内联 / 非 GdiLegacy）

口径：**外部引用** = `src/` 下除 `main_window.h`、除 `archive/gdi_legacy/**`、除该模块自洽 `.cpp/.h` 之外的调用方。  
DesktopTools / selftest / tools：**均无**引用下列七文件实现符号（除过渡 `#include` 残留）。

| 文件 | 外部引用（产品路径） | 可移？ | 原因 / 前置 |
|------|----------------------|--------|-------------|
| `editor_paint.cpp` | **无**（`EngineHost::Paint*`；调用在 host `Paint()`） | **E2 done** | OFF=stubs；ON=真实 TU；F3 物理归档 |
| `editor_dropdown.cpp` | **无**（仅 Create*Popup 路径） | **E2 done** | 同 paint |
| `ui_component.cpp` | **无**外部（仅 GDI 编辑器） | **E2 done** | 同 paint |
| `controls.cpp` | Make* 仅 GDI；`ApplyFont`/`Scale*` → `win32_ui_util` | **E2 done** | OFF 不链真实 controls |
| `popup_combo.cpp` | **几乎无** | **E2 done** | OFF=stubs |
| `panel_popup_combo.cpp` | **无**产品运行时 | **E2 done** | ON 随 Gdi 对话框 |
| `prompt_modal.cpp` | 产品提示 → `engine.toast`；GDI Show* 仅 ON | **E2 done** | OFF=stubs |

### E1 验收要点

- **E1.1**：headless 不创建编辑器子 HWND / 不下拉 popup；`Paint` 不画 Home/Editor；对话框 include 仅 `QST_GDI_LEGACY=1`（`engine_host_window_gdi.h`）；产品 `SelectOnStartup` 走 `PickWindowTarget`。
- **E1.3**：产品失败提示 → `engine.toast`；窗口模式健康检查等改 `ShowPromptInfo`；不再 `promptModal_.Bind`。
- **E2 链接**：OFF 已 stubs；物理归档见 F3；`ApplyFont`/`Scale*` 在 `win32_ui_util`。

### 误报清理（非阻塞）

- ~~`qst_engine_host.cpp`~~ 已删除（E4.1：`engine_runtime.cpp` 为唯一实现 TU）。
