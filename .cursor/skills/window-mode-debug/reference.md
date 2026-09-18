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

### `background_click_keeps_foreground`

- **测什么**：后台窗口模式对 Win32 目标 `PostMessage` 点击后，前台窗口不变；不走 UIA Invoke。最小化目标允许置底安静还原，但不得抢前台。  
- **用户症状**：录制回放 / 鼠标宏把游戏或目标窗切到前台。  
- **代码**：`background_window_input.cpp`、`window_mode_executor.cpp`（禁止 SetForegroundWindow 目标）、`background_uia_input.cpp`（UIA 仅 UWP）  

### `window_client_scale`

- **测什么**：`ScaleWindowClientPoint` 把录制客户区坐标映射到更小的当前客户区；`recordClientWidth/Height` JSON 往返；找图模板 `ComputeTemplateScale(800x600→400x300)==0.5`。  
- **用户症状**：最大化时录制，窗口缩小后回放点偏 / 找图 0%。  
- **代码**：`window_coords.cpp`、`window_mode_json.cpp`、`MapScriptPointToClient`；找图 `FindImageSurfaceScale` / `BuildExecutionFindImageOptions`  

### `window_findimage_full_client`

- **测什么**：窗口/后台窗口模式 `ResolveClientSearchRect` 忽略绝对选取区域（含整屏 3840×2160），始终返回 `0,0,clientW,clientH`。`MapClientRect` 在默认 `screenAbsolute` 下仍把客户区映射为屏幕坐标（禁止原样拷贝）。纯函数 `EffectiveWindowModeClientSearchRect` 同步校验。  
- **用户症状**：窗口模式找图搜到全屏/其它窗口；或「选取区域」把搜索框裁到屏幕坐标；或 OCR/AI/保存图片截到虚拟屏左上角。  
- **代码**：`window_coords.cpp` `EffectiveWindowModeClientSearchRect`；`window_mode_executor.cpp` `ResolveClientSearchRect` / `MapClientRect`  

### `window_relative_playback_enables_wm`

- **测什么**：`FinalizeWindowModeForPlayback(..., reviveEnabled=true)` 仅在录制回放把 `enabled=0` + 身份复活为后台窗口模式；`reviveEnabled=false` 时编辑器默认模式保持关闭；写盘 `enabled=0` 仍保留类名/路径。  
- **用户症状**：窗口模式+图片定位回放出现 `找图调试 ref=2560x1440`、`bestNcc≈50%`、匹配度 0%；或鼠标宏改成默认模式保存后再开仍是后台窗口模式。  
- **代码**：`window_mode_json.cpp` `FinalizeWindowModeForPlayback` / `WriteWindowModeJson` / `ParseWindowModeConfigObject` / `SaveEditorFromJson`  

### `background_minimized_quiet_restore`

- **测什么**：目标最小化时 `BeginRun` 置底还原且不抢前台；`EndRun` 再最小化。  
- **用户症状**：目标最小化后脚本完全不执行。  
- **代码**：`window_mode_executor.cpp` `EnsurePlaybackGeometry`  

### `screen_point_to_client_within`

- **测什么**：`ScreenPointToClientWithin` 客户区内映射成功，区外拒绝。  
- **代码**：`window_coords.cpp`  

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

- **测什么**：`UsesFakeFocus` 在 HiddenDesktop **或** BackgroundWindow 下，开关打开 **或** Unity / 传奇 `TFrmMain`/`TPlayScene` / Adobe AIR `ApolloRuntimeContentWindow` 等游戏类名自动启用；`TForm1` 本身不启用，但顶层下有 `TDXDraw` 子窗时按直播 HWND 启用且禁止绑后最小化。普通 Win32（Notepad）后台仍不启用。`ThunderRT6FormDC` 不得误判（VB6 默认类）。`TscShellContainerClass` / `mstsc.exe`：**禁止**假焦点目标、禁止绑后最小化。  
- **代码**：`src/window_mode/window_mode_types.h` / `window_mode_types.cpp` `LooksLikeGameWindowClass` / `LooksLikeAdobeAirWindowClass` / `LooksLikeRemoteDesktop*`  

### `soft_message_exe_gates`

- **测什么**：Notepad 等 Win32 → softMessage、绑后可最小化；Unity / 传奇 `TFrmMain` 类名自动假焦点并保持还原；显式 softMessage 不走 CDP。Discord/`Weixin.exe`+`Chrome_WidgetWin` 仍是 Chromium 壳；`Weixin.exe`+`Qt51514QWindowIcon` 不是壳、不是 MuMu。  
- **代码**：`window_mode_types.h`、绑定分支 `window_mode_session.cpp`（勿对 exe 调 `PrepareMacroDesktopForExtVision`）  

### `weixin_qt_fake_focus`

- **测什么**：`Weixin.exe` / 标题「微信」+ `Qt*QWindowIcon` → `NeedsFakeFocusInjection` / `UsesFakeFocus`，不是 Electron 壳、不是 `ConfigLooksLikeEmulatorTarget`、不走 LCA；输入子窗绑顶层。纯 Qt 类名仍当 MuMu 模拟器。`微信开发者工具` 不是微信客户端。探针窗：`PostKeyToWindow` 只到 `KEYDOWN`/`KEYUP`，**零** `WM_CHAR`/`WM_ACTIVATE`/`WM_SETFOCUS`/`WM_PASTE`；快捷输入「h」走 KEY*；左键消息到达且不激活。  
- **用户症状**：后台窗口模式对微信无效；日志「安卓模拟器…未注入假焦点」；或「设置未启用假焦点注入」后按键把微信切到前台、一次变两次；或按键 OK 但找图点击/快捷输入没反应。  
- **代码**：`LooksLikeWeixin*`；`TryInstallFakeFocus` 微信忽略关闭注入设置；`PrimeWindowSoftFocus` / `PostKeyToWindow` 不发激活/CHAR；快捷输入 `SendQuickInputViaPostedKeys`；`fake_focus_dll.cpp` `weixinSafe` 前景查询+软光标/键态；`FindBackgroundInputChild` 顶层。  

### `weixin_qt_mouse_hooks`

- **测什么**：标题「微信」+ `Qt*QWindowIcon` 上 `FakeFocus_InstallLite`：`GetCursorPos` 读软光标、`SetCursorPos` 不挪真光标、`GetAsyncKeyState(VK_LBUTTON)` 跟共享内存、不改 WndProc。  
- **代码**：`InstallWeixinMouseStateHooks`；禁止 `MaybePostFakeWmInput` / `SoftRefreshFocusMessages`。  

### `quick_input_skips_paste_non_edit`

- **测什么**：非 Edit 探针（Qt `QWindowIcon`、AIR、`MapleStoryClass`、`Chrome_WidgetWin`、普通自定义类）`PostQuickInputToWindow("h")` **零** `WM_PASTE`。Qt/冒险岛走 KEY* 且不附带宿主 `WM_CHAR`；普通自定义类走 `WM_CHAR`。跨进程记事本仍可 `WM_PASTE`（`post_quick_input`）。
- **用户症状**：后台快捷输入在微信/Qt/AIR/游戏里没反应，记事本却正常；或 Chromium 壳一次打出两个字。
- **代码**：`background_window_input.cpp` `WindowAcceptsWmPaste` / `WindowPrefersPostedQuickKeys` / `SendQuickInputViaPostedKeys`；Chromium 进程内灌键时只 `SetKey`，禁止再宿主 `PostMessage`。

### `posted_quick_keys_timing`

- **测什么**：后台逐字投递（LCA / 游戏类名）必须按真实键盘时序落地：`WM_KEYDOWN` →（键已按下且它的 `WM_CHAR` 已排在 UP 之前）→ `WM_KEYUP`。探针窗口按**每帧一次**的节奏取消息，模拟「按帧取键 + 只在键仍按下时接受目标自己 `TranslateMessage` 出的 `WM_CHAR`」的游戏：文本 `"11"` 必须两字都进、`丢=0`、`同键重叠=0`。`QST_LCA_NO_BARRIER=1`（关队列屏障，回落到固定按住）与 `QST_LCA_KEY_MS=0~500`（兜底按住/间隔）可现场 A/B。
- **用户症状**：后台窗口模式下快捷输入**吞字**（实测 `"11"` 只进一个 `1`，前台 SendInput 正常）：DOWN/UP 零间隔连发 → 整串挤进目标同一帧，且目标自己 `TranslateMessage` 出的 `WM_CHAR` 全被排到所有 `KEYUP` 之后，按「键仍按下」判定的游戏整串吞掉。
- **代码**：`background_window_input.cpp` `PostedKeyStepMs` / `PostedKeyBarrierEnabled` / `SendQuickInputViaPostedKeys`（`queueBarrier()` = 跨线程同步 `SendMessageTimeoutW(top, WM_NULL)`；Ctrl+V 分支同样先按住修饰键）。日志：`[窗口模式] 快捷输入逐字投递 N 字 按住=队列屏障/24ms 间隔=… 文本="…"`。

### `soft_key_combo_state_race`

- **测什么**：软键（假焦点灌键队列：Chromium 壳/Qt/微信）投递组合键时，目标在**处理字符键 KEYDOWN 的那一刻**读 `GetKeyState(VK_CONTROL)` 必须仍是「按下」。探针窗口按真实链路跑 `PostQuickInputToWindow(中文)`（非 Edit → Ctrl+V），断言 `V-DOWN=1 且其中读到 Ctrl 仍按下=1`。`QST_NO_SOFT_KEY_BARRIER=1` 关屏障做 A/B（用例即变红：读到 Ctrl 仍按下=0）。
- **用户症状**：Chromium 壳/CEF（含 WinForms 宿主）里脚本 **Ctrl+V 异常**：**只出 v 不粘贴**，或组合键被当成普通字符。
- **根因**：软键的**状态**（共享内存 `down[]`，宿主一次写完 DOWN/UP）与**事件**（DLL 灌键线程稍后 `PostMessage`）是两条路 → 宿主早已把 Ctrl 写回「抬起」，目标才处理 `WM_KEYDOWN(V)` → Chromium 的 `IsKeyDown(GetKeyboardState(), modifiers)` 判定 Ctrl 不在 → 快捷键退化成字符。
- **代码**：`background_window_input.cpp` `WaitSoftKeyPostTurn`（跨线程 `WM_NULL` 队列屏障，等目标处理完这一笔再写下一步键态）；调用点 `SendQuickInputViaPostedKeys`（Ctrl+V 分支）与 `window_mode_executor.cpp` `SendKey`（`UsesInProcFakeFocusSoftInput` 分支，冒险岛 DirectInput 除外）。日志：`[窗口模式] 软键屏障无应答：本会话改为不等待…`。

### `restore_prefer_maximized`

- **测什么**：最大化→最小化后安静铺满工作区（`WindowFillsWorkArea`），且**不得** `IsZoomed`（禁止 Maximize API 切桌面）；普通窗不得被铺满。  
- **代码**：`window_target.cpp` `RestoreMinimizedQuietPreferMax`  

### `monitor_covering_fullscreen`

- **测什么**：`WS_POPUP` 铺满监视器 → `LooksLikeMonitorCoveringFullscreen`；带标题框的重叠窗仍可 `WindowCoversNearestMonitor`，但不得命中 covering（避免误伤最大化窗）；非游戏类名不得当成 `LooksLikeFullscreenGameTarget`。`UnrealWindow` **任意尺寸**都算 DXGI 敏感（假焦点会冻 Present；`PreferHardwareInput` 恒真）。窗口化 UE5（国王大道）**不是** covering：回放须假前台 + 绝对 `SetCursorPos`，禁止相对鼠标、禁止 `ActivateWindow` 走独占弱路径。Unity 等仍仅在铺满监视器时走本机输入。传奇 `TFrmMain` 铺满**不是** DXGI 独占：仍自动假焦点（钩 `GetCursorPos`），后台不抢鼠标。  
- **代码**：`window_target.cpp`；绑定 `window_mode_session.cpp` `moveCandidateToMacro`；执行路径 `window_mode_executor.cpp` `PreferHardwareInput` / `EnsureHardwareInputFocus` / `UsesRelativeHardwareCursor`；`window_list.cpp` `ActivateWindow`  

### `game_hardware_without_inject`

- **测什么**：窗口/后台窗口 + `UnrealWindow` / `LaunchUnrealUWindowsClient` 且未注入假焦点 → `GameTargetNeedsHardwareWithoutFakeFocus`；不得 `CanParkHardwareInputTargetOffscreen`（玩游戏时不能把窗挪到屏外）。普通 Notepad 类窗不走这条，仍可屏外停放。  
- **用户症状**：SCUM/UE5/枪神纪窗口模式日志 `SendInput ok=0`、脚本点击不跳转；或回放时游戏窗消失。**调试 F9 能点、主页/热键不能点**：旧调试不传 `windowMode`（走前台 SendInput 假绿）；且窗口模式假前台须在 UI 线程先隐藏 Web 壳，工作线程 `SetForegroundWindow` 抢不到游戏。  
- **代码**：`window_mode_types.cpp` `GameTargetNeedsHardwareWithoutFakeFocus` / `LooksLikeUnrealEngineWindowClass`；`window_mode_executor.cpp` `PreferHardwareInput`（后台窗口未注入时假前台，找图不抢前台）；`window_target.cpp` `CanParkHardwareInputTargetOffscreen`；`ui/app.js` `startDebugFrom`；`EngineDebugRunActions`；`ResolveWindowModeSelectMethod`（SelectOnStartup 若前台是本进程则先隐藏）；`StartActionsWorker` 窗口模式隐藏壳。  

### `hardware_offscreen_park`

- **测什么**：窗口化目标可 `ParkHardwareInputTargetOffscreen`（屏外 + `HWND_TOPMOST`，尺寸不缩小），再 `Restore` 回原位置。铺满监视器不得 park。系统把 `-32000` 钳回可见区时必须还原原位置，禁止留在左上角。  
- **用户症状**：国王大道 / 远程桌面回放挡住屏幕；或结束后窗口消失在屏外；挂机时桌面上其它窗口被挪到左上角。  
- **代码**：`window_target.cpp` `ParkHardwareInputTargetOffscreen`；`window_mode_executor.cpp` BeginRun/EndRun  

### `clamp_rect_keeps_bottom_right`

- **测什么**：`ClampRectToContainingWorkArea` 保持工作区右下角小窗的位置，不得改写到主屏原点。  
- **用户症状**：脚本运行后无关窗口从右下角飞到左上角。  
- **代码**：`window_target.cpp` `ClampRectToContainingWorkArea` / `FitRectIntoWorkArea`

### `clamp_rect_shrinks_into_work`

- **测什么**：超大矩形只缩小进所在监视器工作区，不把窗口铺成整块工作区。  
- **代码**：`ClampRectToContainingWorkArea`

### `fake_focus_json_roundtrip`

- **测什么**：`fakeFocusEnabled` 缺省为 false；JSON 读写往返。  
- **代码**：`window_mode_json.cpp`

### `kernel_anticheat_blocks_background`

- **测什么**：`RiotWindowClass` / 联盟客户端标题与 exe 判定为内核反作弊目标；Unity/Unreal 普通窗不得误伤；拒绝文案含「后台窗口」。  
- **用户症状**：后台窗口绑联盟后 `VirtualAllocEx` 拒绝访问，Ctrl+键 PostMessage 无效果但宏仍在空跑。  
- **代码**：`window_mode_types.cpp` `LooksLikeKernelAntiCheatProtectedTarget`；`window_mode_executor.cpp` `BeginRun` 在注入前失败返回  

### `fake_focus_hook_local`

- **测什么**：本进程 `LoadLibrary(FakeFocus64/32.dll)` 后 `GetForegroundWindow` 返回目标 HWND；卸载后恢复。  
- **代码**：`src/window_mode/fake_focus/**`  

### `fake_focus_lite_unreal`

- **测什么**：`FakeFocus_InstallLite` 导出存在，GetForegroundWindow 指向目标，且不钩 PeekMessage 注入 WM_INPUT。  
- **代码**：`fake_focus_dll.cpp` `FakeFocus_InstallLite`

### `fake_focus32_export_rva`

- **测什么**：`FindExportRva` 能解析 `FakeFocus32.dll` 的 `FakeFocus_InstallLite` / `Install` / `Uninstall` / `UpdateTarget` / `HookProc`；`ExportNameMatches` 认 x86 stdcall `_Name@N`，且不把 `Install` 误当成 `InstallLite`。  
- **用户症状**：传奇/Delphi 后台「未找到导出: FakeFocus_InstallLite」，鼠标原地点击。  
- **代码**：`inject_common.cpp` `ExportNameMatches` / `FindExportRva`；`fake_focus.def`

### `remote_module_kernel32`

- **测什么**：对本进程 `FindRemoteModule("kernel32.dll")` 与 PEB 遍历得到的基址等于 `GetModuleHandle`；`ResolveRemoteProcAddress(..., LoadLibraryW)` 等于 `GetProcAddress`。  
- **用户症状**：后台窗口模式「假焦点注入失败: 目标进程未加载模块: kernel32.dll」，Unity 只剩 PostMessage，键鼠无效。  
- **代码**：`inject_common.cpp` `FindRemoteModule` / `FindRemoteModuleViaPeb` / `QueryRemotePeb`

### `fake_focus_header_export_rva`

- **测什么**：合成 PE32 把 `FakeFocus_Install` 导出名放在 `SizeOfHeaders` 内（RVA 0x250 > SizeOfOptionalHeader），`FindExportRva` 仍能解析。  
- **用户症状**：造梦西游 / Adobe AIR 后台「未找到导出: FakeFocus_Install」，随后只走 PostMessage。  
- **代码**：`inject_common.cpp` `PeFileView::RvaToPtr`

### `fake_focus_air_focus_only`

- **测什么**：`ApolloRuntimeContentWindow` 上 `InstallLite` 后 `GetForegroundWindow` 指向目标，但 **不改 WndProc**、**不吞 SetCursorPos**、不往队列塞 `WM_INPUT`。  
- **用户症状**：后台造梦西游一启动就卡死退出，鼠标原地抽。  
- **代码**：`fake_focus_dll.cpp` `LooksLikeAdobeAirClassName` / `InstallCommon` airSafe 早退  

### `fake_focus_maplestory_focus_only`

- **测什么**：`MapleStoryClass` 上 `InstallLite` 后 `GetForegroundWindow` 指向目标，**不改 WndProc**、不往队列塞 `WM_INPUT`；写入软输入后 **`GetCursorPos` / `GetAsyncKeyState` 反映软状态**（IAT 导出名 / 可写节指针扫描，**禁止 user32 方法体 JMP**）；`FakeFocus_MapleIatCount` 低 16 位 ≥ 1，且 **`u32jmp`/`diData` 位必须为 0**（`diag & 0x3000 == 0`）；`FakeFocus_MapleHookHits` 在自检里 GetAsyncKeyState 后低字节 > 0。  
- **用户症状**：后台窗口模式一点运行冒险岛无响应；或注入成功、`slots≥1` 但角色不动、只有边框闪；或跑 2–3 秒后游戏消失、软件自动停止（`目标窗口已消失`）。`slots=0` = 没补到 GetCursorPos；`setFg=1 flash=0` 仍边框闪 = 缓存的 SetForegroundWindow（禁止再打 user32 JMP 修这个）；`u32jmp=1` 或 `diData=1` 后闪退换 pid = 又打了 user32/GetDeviceData 方法体；**18 方法表槽/JMP**（`155136`/`21:25:14`）会等几秒闪退。`0xCFE3`/`slots=2` 且首键后钩命中全 0：本地 dinput8 失焦后靠 `WM_ACTIVATE` 停轮询——IAT 吞失活也救不了键（失焦后不轮询）。**禁止**再灌假 `WM_INPUT`：`164352` / `FakeFocus32.raw.dll` 注入后立刻闪退（`目标窗口已消失` + `CreateRemoteThread Win32=5`）。**禁止** GetGUIThreadInfo IAT / 游戏目录上一级（`156160`/`00:04:24` 立刻闪退）。**禁止**改 dinput8 的 GetProcAddress/DirectInputCreate* IAT 或可写节（`155648`/`23:00:38` 立刻闪退）。
- **代码**：`fake_focus_dll.cpp` `InstallMapleIatHooks`：主程序/本地 dinput8 IAT 钩 Peek/Dispatch/CallWindowProc **只吞失活**（禁止塞 `WM_INPUT`）。27–36 方法表槽（Acquire/GetDeviceState/GetDeviceData/SetCooperativeLevel）；**禁止**因 foundN>4 整表放弃（2009 dinput8 常有 5 张键盘/鼠标表）。虚表在 `.rdata` 即可，**不要**再要求模块内第二份指针（设备对象只在堆上）。相邻 Device8 表会把方法计数顶过 32，**不要**用 n>32/36 丢掉真表（仍拒绝 <27 的 18 方法跳转表）。找不到镜像表时才扫堆上的现有设备（仅 32 位）。辅助 DLL 可写节只补前景/闪框。**禁止**代理 dinput8、禁止假前台、禁止改 dinput8 的 GetProcAddress/DirectInputCreate* IAT。**禁止 Unacquire 槽**。禁止 18 方法表、user32/win32u 方法体 JMP、假 WM_INPUT、注入线程 `RegisterRawInputDevices` / heap `SetCooperativeLevel`。宿主方向键 **SetKey + SendKeyboardKey**（虚表没挂上时前台仍能走；`lastCb=256` 且 diState>0 后停 SendInput，免得打进当前前台窗）。禁止运行中 `CreateRemoteThread` 查 IAT 计数；安装诊断走共享内存 `mapleDiag/mapleIatPoll/mapleDiVt`。键盘 `GetDeviceState(256)` 的 lastCb 必须记 256，禁止再截成 255。
- **用户症状**：后台窗口模式一点运行，冒险岛客户端无响应，只能任务管理器强制结束（软件本身正常）。日志里曾出现「精简假焦点已注入（GetAsyncKeyState/光标/RawInput）」。  
- **代码**：`fake_focus_dll.cpp` `LooksLikeMapleStoryClassName` / `g_mapleSafe`；与 AIR 一样禁止 Phase2。禁止再给冒险岛开光标 inline / RawInput / 子类化。  

### `fake_focus_glfw_lite_cursor`

- **测什么**：类名 `GLFW30` 的 lite 安装仍钩 `GetCursorPos`（暂停菜单点击坐标），且 `SetCursorPos` 被吞掉不挪真光标。  
- **用户症状**：后台《我的世界》窗口有反应但点不中；前台真光标被夹走/跟着飞。  
- **代码**：`fake_focus_dll.cpp` `ShouldSkipGetCursorPosHook` / `Hook_GetCursorPos` / `Hook_SetCursorPos`

### `fake_focus_soft_input`

- **测什么**：共享内存写入光标/按键后，目标进程内 `GetCursorPos` / `GetAsyncKeyState` / `GetKeyboardState` / Raw Input（`PeekMessage(WM_INPUT)` + `GetRawInputData`）反映软状态。  
- **代码**：`fake_focus_soft_input.h`、`fake_focus_soft_input_host.*`、`fake_focus_dll.cpp`  

### `anjuzhen_script_wm_config`

- **测什么**：样例 JSON 含 Chrome 类名时解析为 CDP 策略（`UsesCdpInput`，假焦点注入关闭）。  
- **代码**：`window_mode_json.cpp`、`window_mode_types.h`  

### `permission_match_uipi`

- **测什么**：`CheckPermissionMatch(self)` / `pid=0` 为 true；对 explorer（通常 Medium IL）即使本进程已提权也必须允许。  
- **用户症状**：已用管理员运行本工具，点运行仍提示「请以相同权限运行本工具与目标程序」（窗口模式 / Chrome 游戏页）。  
- **代码**：`window_mode_permission.cpp` `CheckPermissionMatch`；`window_mode_executor.cpp` CDP 路径跳过 UIPI 硬拦  

### `permission_mismatch_no_autolaunch`

- **测什么**：`PermissionMismatch` / `DesktopNotReady` 必须 `ShouldAbortAutoLaunchOnBindFailure`；`TargetNotFound` 仍允许自动打开。提示文案须含「管理员」。  
- **用户症状**：冒险岛已在跑、本工具未提权时，日志先「目标完整性更高」再「未找到目标窗口，自动打开 MapleStoryt.exe」，已开着的游戏闪退。  
- **代码**：`window_mode_types.h` `ShouldAbortAutoLaunchOnBindFailure`；`window_mode_executor.cpp` `BeginRun` / `CheckRunHealth`  

### `maplestory_bg_fake_focus`

- **测什么**：`MapleStoryClass` / `MapleStory.exe` / 标题含冒险岛 识别为游戏；后台 **不** `UsesFakeFocus`、绑后不最小化；`MapleNeedsSafeFakeFocusLite` 为 true。  
- **用户症状**：游戏必须前台才会走，切走后原地 A。技能键走 WndProc/PostMessage；走路走 DirectInput，失焦后停轮询。  
- **代码**：`UsesFakeFocus` / `UsesFakeFocusForTarget` 仍 false（保持 PostMessage）；`TryInstallFakeFocus` 对冒险岛注入 mapleSafe `InstallLite`（吞失活 + DI 填键）。`UsesMapleStoryFakeFocusInput` 恒 false。方向键**始终** `SendKeyboardKey`（防 DI 虚表未挂上时前台也不走）+ 已注入时写 SoftInput。`fake_focus_maplestory_focus_only` 约束 DLL：禁止假 WM_INPUT / 18 方法表 / user32 JMP。  

### `lca_arrow_key_lparam`

- **测什么**：`BuildWindowKeyLParam(VK_LEFT)` 扫描码 0x4B + KF_EXTENDED；`←` / U+2190 规整为 `VK_LEFT`；LCA PostMessage 探针窗收到同样 lParam。  
- **用户症状**：后台空格/C 有效，方向键没反应。  
- **代码**：`background_window_input.cpp` `BuildWindowKeyLParam` / `PostKeyToWindow`；`NormalizeScriptKeyVk`。  

### `lca_bg_unknown_game`

- **测什么**：未登记类名（如 `IWWindowClass`）即使打开假焦点开关也走 LCA 窗口消息、不最小化；Unity/UE/GLFW 仍 `NeedsFakeFocusInjection`；记事本不走 LCA。  
- **用户症状**：未知 2D/DirectX 客户端被注入后闪退，或假焦点失败后改抢前台。  
- **代码**：`PrefersLcaBackgroundMessages`；注入失败时 `SetLcaBackgroundMessageMode`。方向键兼写本机键态（`ShouldMirrorLcaNavKeyState`，TOOLWINDOW 探针除外）。**禁止**给未登记游戏注入 mapleSafe。  

### `tianlong_bg_fake_focus`

- **测什么**：`TianLongBaBuHJ WndClass` / 路径含「开心天龙」/ 标题「天龙八部」→ `NeedsFakeFocusInjection`、`UsesFakeFocus`，**不是** `PrefersLcaBackgroundMessages`。`IWWindowClass` 仍走 LCA。  
- **用户症状**：后台窗口模式找图 60%+ 并打出「点击 → 客户区」但 NPC 没反应；日志「未登记游戏：跳过假焦点注入」。  
- **代码**：`LooksLikeTianLongBaBu*`；`TryInstallFakeFocus` 精简注入 + 软光标播种；DLL 钩 `GetCursorPos`/`GetAsyncKeyState`，禁止子类化/假 `WM_INPUT`/RawInput。  

### `window_mode_target_lost_stops`

- **测什么**：后台 `BeginRun` 后 `DestroyWindow`，`TargetStillAlive` 必须为 false。  
- **用户症状**：冒险岛已闪退，软件仍显示「宏运行中」且步数继续涨（精密时间轴对失效 HWND 静默 PostMessage）。  
- **代码**：`window_mode_executor.cpp` `TargetStillAlive`；`engine_script_run.cpp` 每步/精密等待检测目标，置 `stopFlag_`。  

### `uwp_frame_bind_pid_still_alive`

- **测什么**：`TargetBindPidStillMatches`：UWP 顶层 ApplicationFrameHost PID ≠ CoreWindow PID 仍算存活；HWND 被其它进程复用必须判死。  
- **用户症状**：窗口模式录制计算器后回放立刻「目标窗口已消失」，时间轴 SendInput ok=0。  
- **代码**：`window_mode_types.h` `TargetBindPidStillMatches`；`ApplyBoundTargetState` 记录 `bindPid`；`TargetStillAlive` 按输入窗进程判定。  

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
- **依赖**：exe 同目录按 OS 放 `VirtualDesktopAccessor11.dll`（24H2+）、`VirtualDesktopAccessor11_23h2.dll`（22H2/23H2）或 `VirtualDesktopAccessor10.dll`（Win10）。禁止交叉加载。  
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
