# WindowModeSelfTest — 用例与源码对照

给 Agent 读 FAIL 的 `name` 后直接跳文件。

## 默认套件（无需 `--macro`）

### `quote_args_strip`

- **测什么**：`"C:\路径\检查.txt"` 剥引号后应等于无引号路径；剥两次仍正确。  
- **用户症状**：记事本弹「文件名无效」；窗口模式找图 0%。  
- **代码**：  
  - `src/window_mode/macro_virtual_desktop.cpp` → `FormatLaunchArgs`  
  - `src/window_mode/window_mode_session.cpp` → `TryCreateProcessLaunch`、文档启动分支  

### `ime_filter_null`

- **测什么**：`IsLikelyImeOrToolWindow(nullptr)` 必须为 true。  
- **用户症状**：绑到输入法条 `SoPY_Status` 等。  
- **代码**：`src/window_mode/window_target.cpp` → `IsLikelyImeOrToolWindow`  

### `find_main_window`

- **测什么**：自建测试顶层窗可被 `FindMainWindowDefault` 按类名找到。  
- **代码**：`src/window_mode/window_target.cpp` → Enum / `ProcessMatchesQuery` / `IsLikelyMainWindow`  

### `post_quick_input`

- **测什么**：向自建 `EDIT` `PostQuickInputToWindow` 写入标记串。  
- **代码**：`src/window_mode/background_window_input.cpp`  

### `background_bind_child`

- **测什么**：后台窗口模式 `BeginRun` 后 `TargetHwnd` 类名为 `Edit`（子控件），不是仅顶层。  
- **用户症状**：截图是整个外框/菜单；找图模板对不齐。  
- **代码**：`src/window_mode/window_mode_session.cpp` → `WaitForBindHwnd` / `ResolveBindHwnd`  

### `background_quick_input`

- **测什么**：`WindowModeExecutor` 后台快捷输入写入 Edit。  
- **代码**：`src/window_mode/window_mode_executor.cpp`  

### `quick_input_cancel`

- **测什么**：带字符间隔的 `PostQuickInputToWindow` 在 cancel 后提前结束，写入长度小于全文。  
- **用户症状**：宏运行中按热键无法立刻终止，尤其卡在「快捷输入」。  
- **代码**：`background_window_input.cpp`；主程序路径见 `main_window.h` QuickInput + `stopFlag_` / `SendQuickInputText`  

### `desktop_quick_input_cancel`

- **测什么**：桌面模式 `SendQuickInputText(..., cancelFlag)` 在取消后迅速返回。  
- **代码**：`src/action_utils.cpp`  

### `fake_focus_json_roundtrip`

- **测什么**：缺省无字段 → `fakeFocusEnabled=false`；写入/再解析为 1。  
- **代码**：`src/window_mode/window_mode_json.cpp`  

### `input_strategy_cdp_auto`

- **测什么**：`Chrome_WidgetWin_*` → `ResolveInputStrategy=Cdp` / 保存注明 `inputStrategy:cdp`；独立游戏类名保持 softMessage；`--remote-debugging-port` 附加。  
- **代码**：`window_mode_types.*`、`window_mode_json.cpp`、`cdp/cdp_input.*`  

### `fake_focus_minimize_gate`

- **测什么**：仅 `HiddenDesktop + fakeFocusEnabled`（且非 CDP）时 `UsesFakeFocus`；此时禁止绑窗后最小化。  
- **代码**：`src/window_mode/window_mode_types.h`  

### `soft_message_exe_gates`

- **测什么**：Unity 等游戏类名 → softMessage、绑后可最小化；假焦点时保持还原；显式 softMessage 不走 CDP。  
- **代码**：`window_mode_types.h`、绑定分支 `window_mode_session.cpp`（勿对 exe 调 `PrepareMacroDesktopForExtVision`）  

### `restore_prefer_maximized`

- **测什么**：最大化→最小化后安静铺满工作区（`WindowFillsWorkArea`），且**不得** `IsZoomed`（禁止 Maximize API 切桌面）；普通窗不得被铺满。  
- **代码**：`window_target.cpp` `RestoreMinimizedQuietPreferMax`  

### `fake_focus_hook_local`

- **测什么**：本进程 `LoadLibrary(FakeFocus64/32.dll)` 后 `GetForegroundWindow` 返回目标 HWND；卸载后恢复。  
- **代码**：`src/window_mode/fake_focus/**`  

### `fake_focus_soft_input`

- **测什么**：共享内存写入光标/按键后，目标进程内 `GetCursorPos` / `GetAsyncKeyState` / `GetKeyboardState` 反映软状态。  
- **代码**：`fake_focus_soft_input.h`、`fake_focus_soft_input_host.*`、`fake_focus_dll.cpp`  

### `anjuzhen_script_wm_config`

- **测什么**：样例 JSON 含 Chrome 类名时解析为 CDP 策略（`UsesCdpInput`，假焦点注入关闭）。  
- **代码**：`window_mode_json.cpp`、`window_mode_types.h`  

### `invisible_child_class_bind`

- **测什么**：无 `WS_VISIBLE` 的子窗仍能被 `FindChildWindowByClass` 找到。  
- **用户症状**：宏桌面绑 Edge 仍停在 `Chrome_WidgetWin_1`，软点击/按键无效。  
- **代码**：`window_target.cpp` → `FindLargestChildByClass` / `FindChildWindowByClass`  

### `browser_render_skips_d3d`

- **测什么**：`FindBrowserRenderWidget` 不返回 `Intermediate D3D Window`；截图可用 `FindBrowserCaptureSurface`。  
- **用户症状**：绑到合成层后按键/点击全无响应。  
- **代码**：`window_target.cpp`、`ResolveBindHwnd`、`background_window_input.cpp`  

### `cdp_park_expandable`

- **测什么**：`PrepareMacroDesktopForCdpBind` 刮掉遗留 Cloak/α=1、窗非最小化、Peek 抑制、不 Cloak（≥1.1.39 移除 Cloak/α=1）。窗在另一桌面天然不可见，用户切换可正常展开。
- **用户症状**：宏桌面打不开幽灵窗；执行中切屏到宏桌面。  
- **代码**：`window_target.cpp` → `PrepareMacroDesktopForCdpBind`；见 [cdp-lessons.md](cdp-lessons.md)  

## 可选：`--macro`

### `macro_desktop_launch_bind`

- **测什么**：在「鼠标宏」虚拟桌面启动经典 `System32\notepad.exe` 并绑定到窗口。  
- **依赖**：`build\Release\VirtualDesktopAccessor11.dll`（或 10）在 exe 同目录。  
- **代码**：`macro_virtual_desktop.cpp`、`LaunchTargetOnDesktop`、`BindTargetWindow`  
- **说明**：不覆盖商店 Notepad / 找图模板；只做桌面+启动+绑定烟雾。真找图仍需主程序 UI 或后续加用例。  

### `macro_classic_with_store_open`

- **测什么**：用户已打开商店记事本时，仍用 System32 路径启动/交接并成功绑定（防排除快照把目标也踢掉）。  
- **代码**：`FindLaunchResultMainWindow`、启动等待复用回退  

### `macro_store_path_class_bind`

- **测什么**：WindowsApps Notepad 路径 + `UseEditorWindowClass` + `RichEditD2DPT` 绑定。  
- **代码**：`LaunchStoreAppFromPath` / `BindTargetWindow` / `WaitForBindHwnd`  

## 尚未自动化（需主程序 + 用户脚本）

- 真实找图模板匹配率、偏移点击、Caret、商店 AppsFolder 启动压底观感  
- CDP 同进程 Edge：扩展 v1.1.6+ 禁止挂浏览器级其它窗 iframe（仅本 tab / autoAttach / 精确 DOM src）；标题戳记只证明壳页  
- CDP 找图：优先扩展截图；**禁止** debugger soft-swap（detach iframe↔壳页），否则 MV3 断桥。布局/清戳记用 `chrome.scripting`。canvas/双挂/visibleTab 失败才 Win32。  
- Agent 绿了之后再用 `QuickScriptTool.exe` + 用户宏做最终确认  

## JSON 输出约定

- stdout：UTF-16/宽字符 `fwprintf`（控制台可能显示为乱码，但 JSON 字段名稳定 ASCII）。  
- Agent 解析时按行读：含 `"name"` 的是 case；含 `"passed"` 的是汇总。  
- 不要依赖 stderr 人类格式做自动化。  
