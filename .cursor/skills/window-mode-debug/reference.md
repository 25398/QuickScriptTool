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

### `background_post_wrapped_container`

- **测什么**：自建「顶层 └ 包装层 `NotepadTextBox` └ 真文本控件 `RichEditD2DPT`」三层树，断言
  `FindBackgroundInputChild` 选出**真控件**（`kind=textInput`），且 `PostKeyToWindow` 投递后
  **包装层收到 0 条键消息**（`wrapperMsgs==0`）；另断言**显式绑定的子窗不得被最大兄弟表面顶掉**。
- **用户症状**（原话）：「**后台窗口模式按键点击不生效，按 A 打不到其他应用的后台里面**」（1.3.3 起存在）。
  日志特征：`后台输入子窗 kind=renderSurface class=NotepadTextBox hwnd=0x…`。
- **根因**：`PostMessage` **不向子窗转发** ⇒ 投给只作容器的父窗 = **完全没投**。
  实测 Win11 商店版记事本（WinUI3，2026-09-23 本机）树：
  ```
  Notepad(top) └ NotepadTextBox(755x553) └ RichEditD2DPT(755x553)
  ```
  父子客户区**一样大**，而 `EnumChildWindows` 是「父先于子」，最大值启发式又用**严格大于**
  ⇒「最大后代」取到**包装层** `NotepadTextBox`。受控实验：
  投 `WM_CHAR` 给 `NotepadTextBox` → 文档**一个字都不进**；投给 `RichEditD2DPT` → 正常进字；
  投给顶层 `Notepad` → 也不进。
- **修法**：`background_input_target.cpp` 新增 `TextInputInsideSurface(top, surface, config)`，
  判据是 **`IsChild(surface, input)`** —— 只在「真文本控件确实长在这个最大子窗**里面**」时让位；
  原 `FindKnownRenderSurfaceChild` 里的「最大后代」启发式**移出**，统一到 `FindBackgroundInputChild`
  （那里才有 config 能判「是否优先文本输入」）。
  `ResolveSoftInputHwnd`：**已绑到子控件**时不得再用「无 config 重解析」覆盖它（绑定是
  `ResolveBindHwnd` 带 config 选的，更可信）；唯一例外是绑到的只是包装层 ⇒ 让位给里面的真控件。
- ⚠ **判据必须是 `IsChild`，不能是「窗口里有没有输入框」**：后者会把投递目标从主区域挪到
  别处的小搜索框（既有选择被无理由改掉）。
- **取证脚本**（`tools/verify/`，可复现）：`probe_bg_notepad.py`
  `launch|hier|read|post|exp|close`（不激活起窗 / 打层级树 / 读文本 / 投递 / 对照实验 / 关闭）、
  `probe_richedit_matrix.py`（消息矩阵隔离）、`probe_richedit_keyonly.py`（只发 KEYDOWN 覆盖度）、
  `probe_richedit_backspace.py`（退格/删除/方向键）。
  ⚠ 探针用 `SW_SHOWNOACTIVATE` 起记事本，**不抢前台**；测完自己 `close`。
- **代码**：`background_input_target.cpp` `TextInputInsideSurface` / `FindBackgroundInputChild`；
  `background_window_input.cpp` `ResolveSoftInputHwnd`；
  用例 `background_input_wrapped_text_control` / `background_input_bound_child_respected`（均做过 A/B）。
- **硬规则**：`window_mode_requirements.h` 第 15 条。

### `background_self_translate_double_char`

- **测什么**：纯判据真值表 —— `ClassSelfTranslatesPostedKeys`（只认 `RichEditD2DPT`，大小写不敏感，
  对 `Edit`/`RICHEDIT50W`/空/null 为 false）+ `SelfTranslateKeyUsesWmChar(true, ch) == (ch >= 0x20)`
  （`'a'`/`' '`/`'A'` 为 true；`'\r'`/`'\t'`/`0` 为 false）；**非自译目标行为不变**（显式断言）。
- **用户症状**：**修好 `background_post_wrapped_container` 之后才显形** —— 按一次 A 出 `aa`。
- **根因**：`PostKeyToWindow` 对 `WM_KEYDOWN` **同时**发了 KEYDOWN + `WM_CHAR`；
  WinUI（XAML）文本控件**自己**把 `WM_KEYDOWN` 译成字符 ⇒ 双发。
  实测 `RichEditD2DPT`：只 `KEYDOWN(A)` → `'a'`；只 `WM_CHAR('a')` → `'a'`；
  **`KEYDOWN(A)+WM_CHAR('a')` → `'aa'`**。
- **修法**（两种键走两条路，**永不双发**）：
  - **可打印字符**（`ch >= 0x20`）⇒ **只发 `WM_CHAR`**。理由：该控件的自译只看**真实键态**，
    脚本按住的 Shift 它看不见 ⇒ 走 KEYDOWN 会把 `Shift+A` 退化成 `a`；改由宿主用**软修饰键态**
    译好字符（`SoftVkToChar` 已含 Shift/Ctrl 修正）。
  - **不可打印**（Enter/Tab/退格/删除/方向键/功能键）⇒ **只发 `KEYDOWN`**。
    实测 `WM_CHAR` 对 `'\r'`/`'\t'` **不换行/不制表**；`SoftVkToChar` 对它们返回 0。
  - ⚠ 只发过 `WM_CHAR` 的键**没有配对的 KEYDOWN** ⇒ 松手时**不得**再补 KEYUP
    （`g_softCharOnlyDown[256]` 记账，`ResetSoftKeyState()` 里一起清零）。
- **代码**：`window_mode_types.h` `ClassSelfTranslatesPostedKeys` / `SelfTranslateKeyUsesWmChar`；
  `background_window_input.cpp` `PostKeyToWindow`（`sendChar` / `selfTranslate` / `g_softCharOnlyDown`）；
  用例 `background_key_self_translate_policy`。
- **取证脚本**：`probe_richedit_keyonly.py`（只发 KEYDOWN 的覆盖度）、
  `probe_richedit_backspace.py`（退格/删除/方向键）。
- **硬规则**：`window_mode_requirements.h` 第 16 条。

### `recorded_window_title_locks_rebind`

- **测什么**：① 录制产物（`windowNameIsHintOnly=true`）经 `BuildTargetQuery` **不产生** `titleContains`；
  ② 用户手配（默认 `false`）**仍产生** `titleContains`（旧语义不许漂移）；
  ③ 同一真实窗口 + 标题已变 ⇒ `DoesTopWindowMatchConfig` 对 hint-only **放行**、对手配**拦截**；
  ④ **端到端**：直接用产品真查找接口 `FindMainWindowDefault` 走完整枚举 —— hint-only **找得到**、
  手配 **找不到**。A/B 全部在同一进程同一次运行内完成（排除环境差异）。
- **用户症状**：**后台窗口模式「不操作后台」** —— 用「后台窗口模式 + 自动识别」录制的脚本，
  回放时按键/点击到不了目标（用户原话「窗口模式自动识别，录制回放，不操作后台」）。
  ⚠ **关键判读：这不是投递坏了，是根本没绑上。** 先确认绑没绑到（日志
  `[窗口/后台窗口模式] 已绑定 hwnd=…`），**别一上来就去动注入 DLL**。
- **根因**：录制保存把**录制瞬间的窗口标题**写进 `windowName`
  （`SaveScriptFileData`：`wm.windowName = wmTgt.windowTitle` —— 那是「我当时在那个窗上录的」，
  **不是**用户表达的「我要绑标题含 XX 的窗」）。回放端 `BuildTargetQuery` 对
  `UseEditorWindowClass` 分支把它按 `" - "` 截成 stem 当 `titleContains`，而
  `EnumWindowsOnDesktopProc` 里标题匹配是**硬门**（排在**类名匹配之前**）。
  标题是**易变量**：换文档 / 换标签页 / 游戏换场景 / 存档改名 ⇒ 枚举**零命中** ⇒ 绑不到目标。
- **修法**：`windowNameIsHintOnly` 字段（`window_mode_types.h`）——
  - 录制端（`SaveScriptFileData`）置 `true`；
  - `BuildTargetQuery` 在 hint-only 下**不产生** `titleContains`（身份 = 进程路径 + 类名 + 子窗类名）；
  - `DoesTopWindowMatchConfig` 在 hint-only 下**不拿标题**判「没绑到」
    （否则已正确绑定的目标会被拒，照样回退成「不操作后台」）；
  - ⚠ 字段默认 `false`：**旧脚本 / 用户手配行为完全不变** —— 用户手配的标题关键词是**真意图**，
    必须继续当硬门（自检里显式断言两条并存）。
- **代码**：`window_mode/window_mode_types.h`（字段）、`window_target.cpp`
  （`BuildTargetQuery` / `DoesTopWindowMatchConfig`）、`window_mode_json.cpp`（读写）、
  `engine/engine_host_window.h`（录制保存端）。
- **取证/复现脚本**：`tools/verify/probe_record_playback_bind.py`
  —— 自建可控顶层窗（避开 Win11 记事本进程交接坑），复刻 `TitleMatches`/类名/`IsLikelyMainWindow`
  判据跑对照：**同窗同类名，标题未变 ⇒ 命中 1；标题变了 ⇒ 命中 0；把标题过滤摘掉 ⇒ 命中 1**
  （第三列是决定性反证）。
- **硬规则**：`window_mode_requirements.h` 第 17 条。

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

### `fake_focus_inject_copy`

- **测什么**：注入前必须把 DLL **复制成副本**再注入 —— 副本落盘、**源文件一字不动**、副本文件名归一成
  `FakeFocus32.dll`（`FakeFocus32.next.dll` 这类旁路槽也归一）、同一源同一路径、源变了换新目录。  
- **用户症状**：软件关闭后安装目录里的 `FakeFocus32.dll` 还在被某个游戏进程映射 ⇒ 删不掉/覆盖不了，
  安装包卡在覆盖这一步、用户被迫重启电脑才能装（见 [docs/fakefocus-dll-lock.md](../../docs/fakefocus-dll-lock.md)）。  
- **代码**：`fake_focus_stage.cpp` `StageFakeFocusDllInto` / `FakeFocusStagedPathIn`；
  接线在 `fake_focus_injector.cpp` 的 `ResolveDllPathForPid`  

### `fake_focus_stage_sweep`

- **测什么**：副本目录按年龄（30 天）回收、**被占用（独占打开）的跳过而不是报错**、放开占用后下一次能清掉；
  安装目录里让位改名残留 `FakeFocus*.dll.locked-*` 被清掉，**且不动用户自己的 `*.locked-*` 文件**。  
- **用户症状**：`%LOCALAPPDATA%\QuickScriptTool\module_stage` 只增不减；或安装包让位出来的
  `FakeFocus32.dll.locked-…` 永久留在安装目录里。  
- **代码**：`fake_focus_stage.cpp` `SweepStaleFakeFocusArtifacts{In}`（调用点：产品壳 `wWinMain`
  与播放器 `player_main.cpp`）  

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

### `fake_focus_air_child_iat_only`（2026-10-03 新增）

- **测什么**：**包装窗（顶层）+ `ApolloRuntimeContentWindow` 子窗**这一真实微端结构下，`InstallLite` 仍按 AIR 处理 —— ① 顶层 `GWLP_WNDPROC` **不变**（不子类化）；② `FakeFocus_TimeScaleDiag` **bit6（`g_iatOnly`）= 1**（变速只补 IAT 槽、不碰代码页）。负对照：普通 `STATIC` 窗必须 **bit6 = 0**。  
- **用户症状**：4399 微端（造梦西游）后台窗口模式崩溃 / 一启动就卡死退出 / 开倍速过一会闪退。  
- **代码**：`fake_focus_dll.cpp` `HwndTreeLooksLikeAdobeAir`（看子窗）、`SetIatOnlyMode(mapleSafe || airSafe)`  

### `setwindowshook_not_for_fragile_targets`（2026-10-03 新增）

- **测什么**：`ForbidsSetWindowsHookTechnique()` 的真值表 —— Chromium 壳 / 微信 Qt / Qt 安卓壳 / 原生 3D / 桌面模拟器 / **Adobe AIR** 逐一命中都必须禁止 `setwindowshook`；普通目标必须放行（负对照，防「恒真」）。  
- **用户症状**：把注入技术选成 `setwindowshook` 后，脆弱目标（含 AIR 微端）在目标 UI 线程里被 `LoadLibrary` ⇒ 当场崩/卡死退出。默认技术是 classic ⇒ 日常测不出来。  
- **代码**：`window_mode_types.h` `ForbidsSetWindowsHookTechnique`；`window_mode_executor.cpp` `TryInstallFakeFocus`  

### `injected_module_stale_detection`（2026-10-03 新增）

- **测什么**：`InjectedModuleLooksStale(dllWrite, procStart)` 的真值表（6 格）—— ① 磁盘 DLL 写入时间 **晚于**目标进程启动时间 ⇒ **必须判旧**（`newerIsStale`）；② 早于 ⇒ 不许报警；③ 相等 ⇒ 保守判「不旧」；④⑤⑥ 负对照：`dllWrite=0` / `procStart=0` / 两者皆 0 ⇒ **一律不报警**（宁可漏报，不误报）。  
- **用户症状**：冒险岛后台「**原地不动的平A，不能走A**」——升级了软件但**游戏进程没重启** ⇒ `LoadLibrary` 同路径**只加引用计数、不重跑 DllMain** ⇒ 进程里跑的还是旧 `FakeFocus32/64.dll` ⇒ 共享内存 `kSoftInputVersion`（7→10）不匹配 ⇒ `SoftInputStateLooksValid` 假 ⇒ 软键态 / DirectInput 全失效（只剩 PostMessage 的攻击键）。日志形态：一轮只有 `BeginRun`/`EndRun`，`假焦点已注入`/`假焦点钩命中` 整段消失。  
- **代码**：`window_mode_types.h` `InjectedModuleLooksStale`；`fake_focus_injector.cpp` `InjectAndInstall`（同路径分支打 `⚠ 目标进程 … 里挂的是**旧版** FakeFocus`，**只报警不阻断**）  

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
- **用户症状**：游戏必须前台才会走，切走后原地 A；或**在桌面/浏览器前台时点运行**，全程原地 A（切走只会 A 不会走）。技能键走 WndProc/PostMessage；走路走 DirectInput，失焦后停轮询。  
- **「点运行时游戏已不在前台」根因**：2009 dinput8 靠 WM_ACTIVATE 停轮询（不逐帧查前台），客户端早在注入前就停了 → DI 虚表钩子/软键态全不被读。IAT 吞失活只能拦「以后」的失活。修法：`WakeMapleStoryInputPolling()`（注入后真激活一次、等 `diState>0`、还前台）；**禁止**假 WM_ACTIVATE（会冻客户端）。同时方向键兜底 SendInput 只在目标为前台时补，否则会打进用户正在看的浏览器（B 站视频跳进度/调音量）。  
- **「游戏已在前台、诊断却全零」根因（第二轮，仍未收敛）**：日志里 `gaks=0 gfw=0 diState=0 lastCb=0` 且 `diag=0x00014FE3`（钩子装好了）——说明**客户端根本不调这些 API**，而不是没装上。此时不要盲目继续补钩子，先看 `pollHit=`（见下）。可能的真实入口：打包器手搓 PE 导出解析（绕过 `GetProcAddress`，只能方法体 JMP，而冒险岛**明令禁止** user32 方法体 JMP）；或客户端是纯消息驱动、走路靠别的键态源。  
- **诊断契约（本轮新增，务必先读再动手）**：`mapleDiag` 高位是运行期命中位，宿主在 `假焦点钩安装` 行尾解成 `pollHit=` + `gpaIat=` + `dinputIat=`：
  - `0x20000` GetKeyState 被调用过 / `0x40000` GetKeyboardState / `0x80000` GetCursorPos / `0x100000` GetProcAddress / `0x200000` GetProcAddress 的 IAT 槽已补 / `0x400000` dinput user32 IAT 补到过槽。
  - 判读：`pollHit=无` + `gaks=0 diState=0` ⇒ 客户端不走任何被拦 API（**不是**钩子没装上）；`pollHit=GetCursorPos` 但无键态项 ⇒ 客户端确实在轮询 Win32，键态走了别的入口。
  - **`iatPoll=2` 是异常值**：本地 dinput8 存在时应 ≥4。成因是 dinput8/dinput **懒加载**，PEB 那轮还没进进程；`InstallMapleIatHooks` 末尾已在 DI 虚表阶段之后补走一次 `MapleIatWalkGameDirDinputUser32()`。
  - `stage=`：安装阶段号（1=入口 2=指针就绪 3=PEB/IAT 扫完 4=DI 钩完 5=全完成）；`fault=1` = 安装期抛过异常（已被 SEH 兜住，游戏保住）。`stage<5` 说明安装中途没了，那个数字就是死亡点。
  - `pwPoll=`：进程级（堆 / 主模块映像之外）补到的**键态类**缓存指针数。0 = 那条路也没东西可补。
- **2026-09-19 夜 实测读数（`diag=0x05314FE3`，`stage=5 fault=0`）**：`pollHit=` 只有 **`GetProcAddress`**，`gpaIat=1`；`GetCursorPos`/`GetKeyState`/`GetKeyboardState` **全没被调用过**；`gaks=0 diState=0 lastCb=0`。  
  ⇒ 客户端确实会 `GetProcAddress`，但**不走**任何我们挂上的键态/光标入口；而 **真 `SendInput` 一打就灵**（前台启动能走）⇒ 它读的是**真实键盘状态**，入口在我们够不到的地方（最可能是堆上缓存的 `GetAsyncKeyState` 指针，故本轮加了 `MaplePatchPollPointersProcessWide`）。  
- **前台启动 vs 后台启动的区别**：`PostKeyToWindow` 对方向键会**兼写真键**（`SendKeyboardKey`），但只在「目标就是前台窗」时补。所以**前台启动 = 真键在动 = 游戏读到 = 会走**；后台启动 = 只有 PostMessage + 软键态 = 游戏读不到 = 只会原地 A。**这条差异本身就是判据**，不要再往「DI 虚表没挂上」方向查。
- **构建前提（踩过，别再踩）**：`src/window_mode/fake_focus/build_fakefocus32.cmd` 里 **禁止**写 `if defined ProgramFiles(x86)` —— 括号会打断 `if` 解析，BuildTools 装在「Program Files (x86)」时永远走到 `vcvarsall.bat not found - skip 32-bit DLL`，**32 位 DLL 被静默跳过**（症状：`FakeFocus32.dll` 时间戳远旧于 `FakeFocus64.dll`，任何 DLL 侧修复都没生效）。已改成先 `set "PF86=%ProgramFiles(x86)%"` 再判断，并加 vswhere 全路径兜底。验证：`cmake --build build --config Release --target FakeFocus32` 必须打印 `[FakeFocus32] OK:`。
- **代码**：`UsesFakeFocus` / `UsesFakeFocusForTarget` 仍 false（保持 PostMessage）；`TryInstallFakeFocus` 对冒险岛注入 mapleSafe `InstallLite`（吞失活 + DI 填键）。`UsesMapleStoryFakeFocusInput` 恒 false。方向键：已注入时写 SoftInput + PostMessage，`SendKeyboardKey` **只在目标为前台窗时**才补（`TargetOwnsForegroundWindow`）。`WakeMapleStoryInputPolling()`（`window_mode_executor.cpp`，后台窗口模式 BeginRun 内）负责把「注入前已失焦」的客户端叫醒。`fake_focus_maplestory_focus_only` 约束 DLL：禁止假 WM_INPUT / 18 方法表 / user32 JMP。`MaplePollHitSummary()`（`window_mode_executor.cpp`）负责把诊断位解成人话。  

### `background_fake_focus_not_degraded`

- **测什么**：`ShouldInjectTimeScaleOnly(true, true)` 必须为 **false**（需要假焦点时不得只装时钟补丁）；`BackgroundTargetRequiresFakeFocus(后台+GLFW30, 关注入)` 为 true、设置开着时为 false、`HiddenDesktop` 时为 false。  
- **用户症状**：**后台模式其实是「假后台」，跑脚本时鼠标/键盘被抢走**。实测 MC（`class=GLFW30`，Java 版）：脚本一动，用户就没法操作电脑。日志三连：
  ```
  [窗口模式] 已关闭假焦点注入，但启用了窗口变速：仅注入时钟补丁（不装假焦点钩，键鼠仍走软消息/必要时假前台）
  [窗口模式] 假焦点未生效（未注入 / 仅时钟补丁 / 钩已拆），回退假前台 SendInput（绝对坐标；会占键鼠）
  [窗口模式] 本机绝对光标 客户区(536,88) → 屏幕(1389,568)
  ```
- **根因**：`timeScaleOnlyInject` 的旧判据是 `timeScaleWanted && (!enableFakeFocusInjection_ || !fakeFocusNeeded)`。用户 `app_settings.json` 里 `enableFakeFocusInjection=false` + 开着变速 ⇒ 3D/GLFW 目标被带成「仅时钟补丁」⇒ DLL 里没有任何假焦点钩 ⇒ `fake_focus_active()` 为 false ⇒ `PreferHardwareInput()` 为 true ⇒ 回退假前台 SendInput。  
- **修法**：
  1. `ShouldInjectTimeScaleOnly(timeScaleWanted, fakeFocusNeeded) = timeScaleWanted && !fakeFocusNeeded` —— **判据里不得出现「用户关掉了假焦点注入」**。
  2. `BackgroundTargetRequiresFakeFocus(config, hwnd, enableFakeFocusInjection)` —— 后台窗口模式 + 需要假焦点的 3D/游戏目标**忽略**该设置（理由同微信/冒险岛：没有假焦点就没有真后台）。
  3. 边界：只对 `BackgroundWindow` 生效；`HiddenDesktop`（宏桌面）保持原行为 —— 那时用户本来就不在这台桌面上操作，占键鼠无所谓。
- **代码**：`window_mode_types.h` `ShouldInjectTimeScaleOnly` / `BackgroundTargetRequiresFakeFocus`；`window_mode_executor.cpp` `TryInstallFakeFocus`（`timeScaleOnlyInject` 计算处 + `!enableFakeFocusInjection_` 分支）。  
- **排查提示**：日志里出现「回退假前台 SendInput」＝后台承诺已破，先查这两条判据，别去动 DLL。  

### `lca_nav_keyup_released_after_focus_loss`

- **测什么**：`ShouldMirrorNavKeySend(down, targetOwnsForeground, mirroredDown)` —— 按下只在目标为前台时为 true；**松开只看 `mirroredDown`**，与此刻是否前台无关。  
- **用户症状**（原话）：「**在游戏前台启动，再去浏览器看视频，就会朝离开时候的那一个方向 瞬移**」；切回任何程序都像在持续按方向键。  
- **根因**：`PostKeyToWindow` 里 `SendKeyboardKey` 的判据原来每次调用都重算 `TargetOwnsForegroundWindow(send)`。按下时游戏在前台（补了真键 ↓）→ 用户切去浏览器 → 收到 KEYUP 时已不在前台 → 按「不在前台就不补」跳过 ⇒ **真键永久卡在按下状态**。游戏读真实键盘状态，于是朝那个方向一直走；整个系统也认为该键被按住。  
- **修法**：`ShouldMirrorNavKeySend(down, fg, mirroredDown) = down ? fg : mirroredDown`；`PostKeyToWindow` 用 `g_mirroredNavDown[256]` 记录本会话补过 KEYDOWN 的方向键；新增 `ReleaseMirroredLcaNavKeys()`，在 `BeginRun` 开头与 `EndRun` 各调一次兜底松键。  
- **注意与上一条的边界**：`lca_nav_key_leaks_to_foreground` 守的是**按下**（后台不得补，否则打进遮挡窗）；本条守的是**松开**（补过就必须补回来）。两条必须同时成立。  
- **代码**：`background_window_input.h` `ShouldMirrorNavKeySend` / `ReleaseMirroredLcaNavKeys`；`background_window_input.cpp` `PostKeyToWindow`；`window_mode_executor.cpp` `BeginRun`/`EndRun`。  

### `fakefocus_stale_module_crash`

- **测什么**：`TargetHasStaleFakeFocusModule(pid)` 能按模块名发现目标进程里已加载的 `FakeFocus32.dll`/`FakeFocus64.dll`；`InstallMapleIatHooksGuarded()` 的 SEH 兜底不改变正常路径行为。  
- **用户症状**：**注入后游戏立刻闪退**。日志特征：`假焦点钩命中 … hitReady=1` 但 `假焦点钩安装 … iatPoll=0 diag=0x00000000 foundVt=0 patchedSlot=0`（什么都没装上），紧接着 `目标窗口已消失（进程退出/闪退）`。  
- **根因**：**同一个游戏进程反复注入**。第二次注入时 IAT 槽被两套 detour 覆盖、DirectInput 方法体 JMP 叠加、卸载时各按自己保存的原始字节回写 → 访问违例。现场最好认的证据是**日志里 `pid`/`hwnd` 跨小时甚至跨天完全不变**（实测 `pid=2489124 hwnd=0x0D06E6` 从 09-18 20:32 一直用到 09-19 16:24），说明用户一直没重启游戏。  
- **修法**：注入前 `TargetHasStaleFakeFocusModule()`（Toolhelp，回传 **`szExePath` 完整路径**，只看模块名区分不出「两份不同路径」这种最危险的情况）警告「请先完全退出 MapleStoryt.exe」；`InstallMapleIatHooksGuarded()` 用 `__try/__except` 兜住安装期异常并置 `kMapleInstallFault`，先保游戏。  
- **诊断读数全 0 的另一种成因（2026-09-20 补）**：日志出现 `iatPoll=0 diag=0x00000000 stage=0 hitReady=0` 但注入本身报成功 ⇒ 目标进程里已有一份**同一路径**的 FakeFocus，`InstallCommon` 走**早退路径**（`g_installed` 已为真）。此时 DLL 若不再往宿主新建的共享内存里写，宿主读到的就全是 0 —— 看着像「注入没生效」。已修：早退路径重新 `OpenSoftInputView` + `MaplePublishHits()`。**看到全 0 先排这一条，别急着怀疑钩子。**
- **诊断契约补充**：`mapleDiag` 最高 8 位是安装阶段号（`MapleSetStage`，1=入口 2=指针就绪 3=PEB/IAT 扫完 4=DI 钩完 5=全部完成），宿主日志打 `stage=`/`fault=`。装到一半就没了 ⇒ `stage` 就是死亡点。  
- **禁止**：用「文件大小 == 魔术数」判 DLL 新旧。源码一改大小就漂移，曾把 163840 判成「旧」、把源码注释里明写「164352 闪退」的尺寸当成「现行」，把整轮排查带偏。以共享内存 `diag`/`hitReady`/`stage` 为准。  
- **代码**：`fake_focus_injector.cpp` `TargetHasStaleFakeFocusModule`；`fake_focus_dll.cpp` `InstallMapleIatHooksGuarded` / `MapleSetStage`；`window_mode_executor.cpp` `LogMapleHookHits`（解 `stage=`/`fault=`）。  

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
