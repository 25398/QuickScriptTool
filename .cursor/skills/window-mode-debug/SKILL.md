---
name: window-mode-debug
description: >-
  Debug and regression-test QuickScriptTool 窗口模式 / 后台窗口模式 via
  WindowModeSelfTest.exe. Use when: finding/binding target windows fails,
  find-image is 0%, launch/open-document errors (文件名无效), tray/Z-order issues
  during window mode, or any agent needs a green/red loop before UI reproduce.
  Prefer this skill over guessing — run --json, map FAIL name to source, fix, re-run.
---

# Window Mode Debug（Agent 必读）

总索引：[module-selftest](../module-selftest/SKILL.md)（选 suite / 统一约定）

本仓库给 Cursor Agent 准备的**可执行自检**：`WindowModeSelfTest.exe`。  
目标：不依赖用户手工点宏，也能验证启动/绑定/子控件/快捷输入等核心逻辑，并根据 FAIL 名称定位改代码。

**CDP/扩展需求与踩坑（禁止重复证伪方向）**：  
- 需求：[cdp-requirements.md](cdp-requirements.md)  
- 错误方向 / 定论：[cdp-lessons.md](cdp-lessons.md)

## Agent 取日志（免用户粘贴）

窗口模式日志自动追加到 **`QuickScriptTool.exe` 同目录** 的 `window_mode_debug.log`（Release/Debug 各自一份）。  
排障时读该文件即可，不必让用户从「宏调试信息」窗复制。

```powershell
Get-Content ".\build\Release\window_mode_debug.log" -Tail 80
```

完整游戏脚本无法在 Agent 环境可靠复现（需本机 Edge+扩展+游戏页）；以自检 `--json` + 上述日志为准。

## 何时必须用本 Skill

- 用户反馈：找图 0%、绑错窗（IME/`SoPY`）、「文件名无效」、启动后卡在等待窗口、后台 OK 但「鼠标宏」桌面挂了  
- 你改了 `src/window_mode/**`、启动参数、FindImage 窗口模式路径  
- 准备提「已修好」之前：至少默认用例全绿  

找图偏移 / 找图后连点（双屏、窗口模式、远程桌面常见）：偏移必须跟命中框同一坐标系。「找图移动」后的「鼠标点击」会重新落到找图坐标（不能点 GetCursorPos，否则远程桌面/真人鼠标会把光标拖走）。调试日志里 `鼠标点击左键@x,y` 是脚本残留坐标，不是当时光标；真正落点看后面的「重新落到」。找图后续=点击时可用循环次数连点。遥控用户电脑时请勿在对端画面上移动鼠标。

默认模式（非窗口模式）点游戏：走系统光标 + 绝对 SendInput + 按下至少 1ms，并尝试激活落点窗口。不是 FakeFocus。游戏读 `GetCursorPos` 时这一套才够；全屏独占若激活失败，日志会写「未能激活窗口」。后台点天龙仍须窗口模式精简假焦点。

## 构建（Windows / MSBuild）

在仓库根目录 `d:\other\software`（或当前 clone 根）：

```powershell
& "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:WindowModeSelfTest /m /v:minimal
```

需要连带主程序时：

```powershell
# PowerShell 里不要用分号拼两个 /t：会当成两个命令。分开跑或：
& ...\MSBuild.exe ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:QuickScriptTool /m /v:minimal
& ...\MSBuild.exe ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:WindowModeSelfTest /m /v:minimal
```

产物：`build\Release\WindowModeSelfTest.exe`  
（OpenCV / VDA DLL 一般已在 `build\Release\`，若缺 `opencv_world*.dll` 从同目录 QuickScriptTool 旁复制。）

## 运行

| 命令 | 作用 |
|------|------|
| `build\Release\WindowModeSelfTest.exe --help` | 用法 |
| `build\Release\WindowModeSelfTest.exe --list` | 只列用例名与说明（不跑） |
| `build\Release\WindowModeSelfTest.exe --json` | **Agent 默认**：机器可读，exit `0`=全过 |
| `build\Release\WindowModeSelfTest.exe` | 人类可读 PASS/FAIL |
| `... --json --macro` | 额外跑「鼠标宏」桌面烟雾（需 `VirtualDesktopAccessor*.dll`） |

**判读：**

- 进程退出码 `0` → 默认套件绿，可继续 UI 复现或收工  
- 退出码 `N>0` → `N` 个失败用例；stdout 每行一个 case JSON，最后一行 summary  

`--json` 单行 schema：

```json
{"name":"case_id","ok":true|false,"detail":"可选说明/实际值"}
{"passed":6,"failed":0,"ok":true}
```

## 用例 → 含义 → 优先改哪里

详见同目录 [reference.md](reference.md)。速查：

| `name` | 失败通常表示 | 优先查看 |
|--------|--------------|----------|
| `quote_args_strip` | 文档路径引号被二次转义 | `macro_virtual_desktop.cpp` `FormatLaunchArgs`；`window_mode_session.cpp` CreateProcess/文档启动 |
| `no_select_ignores_doc` | 不选择窗口仍打开残留文档/标题 | `ResolveDocumentFileToOpen` / `EffectiveLaunchArgs`（NoSelect 应空参启 exe） |
| `ime_filter_null` | IME 过滤 API 异常 | `window_target.cpp` `IsLikelyImeOrToolWindow` |
| `find_main_window` | 按类找顶层窗失败 | `window_target.cpp` Find/匹配 |
| `post_quick_input` | 后台 PostMessage 输入失败 | `background_window_input.cpp` |
| `background_bind_child` | 未绑到子 `EDIT` | `WaitForBindHwnd` / `ResolveBindHwnd`（`window_mode_session.cpp`） |
| `background_quick_input` | Executor 后台输入不通 | `window_mode_executor.cpp` BeginRun/SendQuickInput |
| `background_click_keeps_foreground` | 后台点击抢前台 | `background_window_input.cpp`；executor 禁止抢前台；最小化可置底还原；UIA 仅 UWP |
| `window_client_scale` | 窗口相对坐标未按客户区缩放 | `window_coords.cpp` `ScaleWindowClientPoint`；`recordClientWidth/Height`；找图模板 `ComputeTemplateScale` |
| `window_findimage_full_client` | 窗口模式找图仍走屏幕选取区/整屏；OCR/AI 截到屏左上角 | `window_coords.cpp` `EffectiveWindowModeClientSearchRect`；`ResolveClientSearchRect` / `MapClientRect` |
| `window_relative_playback_enables_wm` | 窗口相对找图走全屏桌面、匹配度约 50%；或鼠标宏默认模式保存后再开仍是后台窗口 | `FinalizeWindowModeForPlayback(reviveEnabled)`：仅录制路径复活；编辑器 `enabled=0` 不得强制开启 |
| `background_minimized_quiet_restore` | 最小化目标无法回放，或还原时抢前台 | `window_mode_executor.cpp` `EnsurePlaybackGeometry` |
| `quick_input_cancel` | 热键/取消无法打断快捷输入 | `background_window_input.cpp`；`main_window.h` QuickInput |
| `desktop_quick_input_cancel` | 桌面快捷输入不响应取消 | `action_utils.cpp` `SendQuickInputText` |
| `macro_desktop_launch_bind` | 宏桌面启动或绑定失败（仅 `--macro`） | `macro_virtual_desktop.cpp` + `LaunchTargetOnDesktop` |
| `macro_classic_with_store_open` | 已有商店记事本时经典路径绑不上 | `FindLaunchResultMainWindow` / 启动复用回退 |
| `macro_store_path_class_bind` | 商店路径+指定窗口类/RichEdit 失败 | `LaunchStoreAppFromPath` / `WaitForBindHwnd` |
| `macro_editor_open_named_doc` | 指定窗口类未按标题打开文档、绑到空白窗 | `BuildTargetQuery` 标题 / `ResolveDocumentFileToOpen` / `BeginRun` 身份校验 |
| `fake_focus_json_roundtrip` | fakeFocusEnabled 缺省/读写错 | `window_mode_json.cpp` |
| `kernel_anticheat_blocks_background` | 未识别 RiotWindowClass/联盟客户端，或误伤 Unity | `window_mode_types.cpp` `LooksLikeKernelAntiCheatProtectedTarget`；`BeginRun` 须拒绝后台 |
| `fake_focus_minimize_gate` | UsesFakeFocus / 最小化门控错 | `window_mode_types.h` / `LooksLikeGameWindowClass` / `LooksLikeDelphiVclGameWindowClass` |
| `monitor_covering_fullscreen` | 铺满监视器判定错（误伤最大化窗或漏掉独占全屏）；或窗口化 UE5 被当成独占（回放不生效） | `window_target.cpp` `LooksLikeMonitorCoveringFullscreen`；假前台见 `EnsureHardwareInputFocus` / `ActivateWindow` |
| `game_hardware_without_inject` | 窗口化 UE5 未注入时仍走 PostMessage（脚本点击无效）；或把游戏窗屏外停放。另：调试能点、主页/热键不能点 = 调试未走窗口模式或壳仍占前台 | `GameTargetNeedsHardwareWithoutFakeFocus`；`PreferHardwareInput`；`StartActionsWorker` 隐藏壳；`ParseDebugScriptJson` 传 windowMode |
| `hardware_offscreen_park` | 窗口化本机输入目标未能屏外+顶置，结束未还原，或停放失败把窗口留在左上角 | `window_target.cpp` `ParkHardwareInputTargetOffscreen`（失败必须还原原位置） |
| `clamp_rect_keeps_bottom_right` | 钳工作区把右下角窗口吸到主屏左上角 | `window_target.cpp` `ClampRectToContainingWorkArea` |
| `clamp_rect_shrinks_into_work` | 超大框应收拢但仍被铺满整块工作区 | `ClampRectToContainingWorkArea` |
| `vda_selects_os_dll` | 宏桌面 DLL 选错代（Win11 加载了 Win10 VDA，GetDesktopCount 崩溃） | `virtual_desktop_accessor.cpp` `kDllCandidates` |
| `fake_focus_hook_local` | FakeFocus DLL hook/卸载失败 | `src/window_mode/fake_focus/**` |
| `fake_focus_lite_unreal` | UE5 精简假焦点导出缺失或仍钩 PeekMessage 注入 WM_INPUT | `fake_focus_dll.cpp` `FakeFocus_InstallLite` |
| `fake_focus32_export_rva` | 32 位 FakeFocus 导出为 `_Name@N` 时远程 InstallLite 找不到 | `inject_common.cpp` `FindExportRva` |
| `remote_module_kernel32` | Unity 等「目标进程未加载模块: kernel32.dll」，假焦点失败只剩 PostMessage | `inject_common.cpp` `FindRemoteModule`（Toolhelp 重试 + PEB） |
| `fake_focus_header_export_rva` | 导出名在 PE 头（SizeOfHeaders）内时 FindExportRva 报「未找到导出」 | `inject_common.cpp` `PeFileView::RvaToPtr` |
| `fake_focus_glfw_lite_cursor` | GLFW30 lite 未钩 GetCursorPos，或 SetCursorPos 仍挪真光标 | `fake_focus_dll.cpp` `ShouldSkipGetCursorPosHook` / `Hook_SetCursorPos` |
| `fake_focus_air_focus_only` | AIR/造梦注入后卡死退出或真光标原地抽 | `fake_focus_dll.cpp` AIR 只钩前景查询，禁止子类化/光标/RawInput |
| `weixin_qt_fake_focus` | 微信 4.x 后台键鼠无效、日志当安卓模拟器/「未启用假焦点注入」、抢前台、一次按键两次、找图点击/快捷输入没反应 | `LooksLikeWeixinTarget`；勿当 MuMu；忽略关闭注入设置仍精简 FakeFocus；禁止 `WM_ACTIVATE`/`WM_CHAR`/`WM_PASTE`；钩软光标+键态 |
| `weixin_qt_mouse_hooks` | 微信找图点击没反应（按键已 OK） | `InstallWeixinMouseStateHooks`；`GetCursorPos`/`GetAsyncKeyState`；禁 RawInput/子类化 |
| `chromium_shell_soft_input` | Electron/CEF 壳里脚本 Ctrl+V **只出 v 不粘贴**；鼠标移到某处不动、点了没反应 | `fake_focus_dll.cpp` electronSafe 分支必须调 `InstallWeixinMouseStateHooks()`（软光标 + `GetKeyboardState`/`GetKeyState`/`GetAsyncKeyState`）；`Hook_GetCursorPos` 在 `g_electronSafe` 下必须是静默钩（禁假 WM_INPUT）；灌键线程停止位须在 `DrainSoftKeyEventsPost` 循环里查 |
| `quick_input_skips_paste_non_edit` | 快捷输入对 Qt/AIR/游戏/Chromium 壳没反应（记事本 OK） | `SendQuickInputViaClipboard` 仅 Edit 类才算粘贴成功；其余走 KEY*/Ctrl+V，禁止假成功 `WM_PASTE` |
| `posted_quick_keys_timing` | 后台窗口模式快捷输入**吞字**（`"11"` 只进一个 `1`；前台正常） | `SendQuickInputViaPostedKeys` 必须保证「DOWN 已被目标处理（其 `TranslateMessage` 的 `WM_CHAR` 排在 UP 之前）」：默认用跨线程 `WM_NULL` **队列屏障**，失败才回落 `DOWN→按住→UP→间隔`；禁止零间隔连发。旋钮 `QST_LCA_NO_BARRIER` / `QST_LCA_KEY_MS` |
| `soft_key_combo_state_race` | Chromium 壳/CEF 里 **Ctrl+V 异常**（只出 v 不粘贴）；Qt/微信同理 | 软键**状态**（`down[]`）不能抢在**事件**前面：每投递一笔必须 `WaitSoftKeyPostTurn`（跨线程 `WM_NULL` 屏障）等目标处理完再写下一步键态 —— 调用点 `SendQuickInputViaPostedKeys` Ctrl+V 分支 + `window_mode_executor.cpp` `SendKey`。旋钮 `QST_NO_SOFT_KEY_BARRIER` |
| `fake_focus_maplestory_focus_only` | 冒险岛点运行后游戏无响应，或脚本在跑但角色不动 / 边框闪 / 立刻闪退 | `fake_focus_dll.cpp` maple IAT + **≥27 方法**设备虚表槽 + Acquire/GetDeviceState 方法体 JMP。**禁止 18 方法表、foundN>4 整表放弃、n>32/36 丢掉相邻真表、模块内第二份虚表指针硬性要求**。**禁止假 WM_INPUT / `164352` raw.dll**。命中/安装诊断看共享内存，禁止运行中 CreateRemoteThread |
| `window_mode_target_lost_stops` | 游戏已闪退但软件仍显示宏运行中、步数继续涨 | `TargetStillAlive` + `engine_script_run.cpp` 目标消失立即 stopFlag |
| `uwp_frame_bind_pid_still_alive` | UWP 计算器绑上立刻报「目标已闪退」 | `TargetBindPidStillMatches` / `bindPid`；禁止用 ApplicationFrameHost PID 对 CoreWindow |
| `fake_focus_soft_input` | Phase2 软输入同步失败 | `fake_focus_soft_input*` / DLL GetCursorPos hooks |
| `anjuzhen_script_wm_config` | 安居镇.json windowMode 字段不符 | `build/*/scripts/安居镇.json` + `window_mode_json` |
| `permission_match_uipi` | 管理员运行仍提示「请以相同权限」 | `window_mode_permission.cpp` `CheckPermissionMatch` |
| `permission_mismatch_no_autolaunch` | 目标完整性更高时仍自动打开 MapleStoryt.exe，已开着的游戏闪退 | `ShouldAbortAutoLaunchOnBindFailure`；`BeginRun` 禁止把 PermissionMismatch 当成未找到 |
| `maplestory_bg_fake_focus` | 冒险岛后台绑上后立刻 EndRun；或注入后字母键也不动；或切走前台只会 A 不会走；或**点运行时游戏不在前台 → 全程原地 A**；或**游戏已在前台、`gaks=0 gfw=0 diState=0 lastCb=0` 全零却仍不走** | `LooksLikeMapleStory*`；`UsesFakeFocus`=0 仍 PostMessage；**须 mapleSafe InstallLite** 吞失活+DI；`UsesMapleStoryFakeFocusInput` 恒 false；`WakeMapleStoryInputPolling()` 注入后真激活一次（**禁止**假 WM_ACTIVATE，会冻客户端）。全零时先看 `pollHit=` 判读（见 [reference.md](reference.md) `maplestory_bg_fake_focus`），**别再盲补钩子** |
| `lca_arrow_key_lparam` | 后台空格有效、方向键没反应 | `BuildWindowKeyLParam` 扫描码 0x4B；`NormalizeScriptKeyVk`；冒险岛方向键 `SendKeyboardKey`（仅前台） |
| `lca_nav_key_leaks_to_foreground` | 后台窗口模式跑脚本时用户在前台干别的被打扰：浏览器视频 ←/→ 跳进度、↑/↓ 调音量 | `background_window_input.cpp` `PostKeyToWindow` 方向键兜底必须 `TargetOwnsForegroundWindow(send)` 才 `SendKeyboardKey`；目标在后台只写 SoftInput + PostMessage |
| `lca_bg_unknown_game` | 未登记游戏仍注入假焦点，或 Unity 被误判成 LCA | `PrefersLcaBackgroundMessages` / `NeedsFakeFocusInjection` |
| `tianlong_bg_fake_focus` | 天龙八部后台找图命中但点不上 | `LooksLikeTianLongBaBu*`；勿当 LCA 纯 PostMessage；精简假焦点钩 `GetCursorPos`/`GetAsyncKeyState` |
| `cdp_park_expandable` | CDP 停放后仍 Cloak/Peek 或二次最小化 | `PrepareMacroDesktopForCdpBind`；[cdp-lessons.md](cdp-lessons.md) |
| `window_list_enumerates_self` | AI 切窗台账枚举不到窗口，或没排除本进程窗口 | `src/window_mode/window_list.cpp` → `IsSwitchableWindow` |
| `window_list_match_and_format` | `activateWindow` 关键词匹配/台账排版坏 | `window_list.cpp` → `MatchWindows` / `FormatWindowList` |
| `window_activate_foreground` | 切窗切不过去（前台锁/最小化没还原） | `window_list.cpp` → `ActivateWindow`（AttachThreadInput 那段） |
| `screen_point_to_client_within` | 窗口相对坐标屏幕→客户区映射错 | `window_coords.cpp` `ScreenPointToClientWithin` |

## 硬性约定（改窗口模式时遵守）

完整条文：`src/window_mode/window_mode_requirements.h`（优化时禁止改坏）。

1. **「指定窗口类」≠「不选择窗口」**：指定类按身份找/开文档；不选择窗口只启 exe，忽略残留 launchArgs/标题。  
2. **输入策略分流**：  
   - **CDP/扩展**：Win32 **只** Move 到「鼠标宏」停放；键鼠/找图/保活走扩展。禁 Cloak/PreferMax/pin/Correct/Raise。见 [cdp-lessons.md](cdp-lessons.md)。  
   - **softMessage/假焦点**：宏桌面 + Win32。  
   - 切屏/无法展开 → 多半又在用 Cloak/PreferMax（错误方向）。  
3. **响应要快**：短轮询、找到即返回（无标题约 ≤2s，带文档约 ≤3s）。  
4. **自动打开文档（仅指定窗口类）**：目标程序带文件参数；禁止裸 `ShellExecute(文档)` 当主路径；禁止 `GetFullPathName(cwd)` 误开无关文件。  
5. **命令行参数**：先剥外层 `"`；工作线程不要 WinEvent 狂睡。  
6. **有 `childWindowClassName` 时**：后台与宏桌面都能绑子控件再找图。
7. **VirtualDesktopAccessor**：只加载与 OS build 匹配的那一颗 DLL（24H2+ / 23H2 / Win10）；禁止 Win11 回退加载 `VirtualDesktopAccessor10.dll`（GetDesktopCount 会 AV）。  
8. **32 位 FakeFocus 导出名**：远程安装查 `FakeFocus_Install` / `InstallLite`（无装饰）。x86 stdcall 默认导出 `_Name@N`，必须用 `.def` 或 `ExportNameMatches`；`RvaToPtr` 必须认 SizeOfHeaders 内的导出名（禁止只用 SizeOfOptionalHeader）。传奇/Delphi/AIR（`ApolloRuntimeContentWindow`，造梦西游）注入失败会退回 PostMessage，鼠标原地点击。  
9. **远端模块枚举**：`FindRemoteModule` 必须对 Toolhelp `ERROR_BAD_LENGTH`/`ERROR_PARTIAL_COPY` 重试，并回退 PEB（WOW64 用 32 位 PEB）。Unity 等模块极多时只调一次 `CreateToolhelp32Snapshot` 会误报「未加载 kernel32.dll」，假焦点失败。  
10. **AIR 禁止 Phase2；星辰冒险岛只允许 mapleSafe 精简注入；未登记游戏走 LCA 窗口消息**：`ApolloRuntimeContentWindow` 只钩前景查询。`MapleStoryClass` **技能键**走窗口消息（PostMessage `WM_KEY*`、`KF_EXTENDED`、不发 `WM_CHAR`/不发 `WM_ACTIVATE`）。**走路不走 WndProc**：2009 dinput8 收到真 `WM_ACTIVATE` 失活就停轮询，切走前台后只会 A 不会走——须注入 mapleSafe `InstallLite`（IAT 吞失活 + DI ≥27 方法虚表，共享内存填键）。`UsesFakeFocus` / `UsesMapleStoryFakeFocusInput` 仍为 false（禁止跳过 PostMessage）。方向键只在**目标就是前台窗**时补 `SendKeyboardKey`（钩未挂上时前台仍能走；`TargetOwnsForegroundWindow()` 判据）——目标在后台时 SendInput 打的是遮挡窗（浏览器视频 ←/→/↑/↓ 跳进度、调音量），而目标自己失焦早就停轮询照样不走，所以后台一律不补真键。`lastCb=256` 且 diState>0（DI 钩活着）时前台也不补。**冒险岛失焦即停轮询**（2009 dinput8 靠 WM_ACTIVATE，不逐帧查前台）：IAT 吞失活只拦得住注入之后的失活，点运行时游戏若已不在前台就永远不轮询（诊断 `diState=0 lastCb=0` + `gaks=0 gfw=0`）→ `WakeMapleStoryInputPolling()` 在注入后补一次**真激活**（**禁止**假 WM_ACTIVATE，会冻客户端），等 `diState>0` 再还前台。**未登记游戏**仍只 LCA、不注入，但方向键同样兼写本机键态（仅前台走路；自检 TOOLWINDOW 探针除外）。Unity/UE/GLFW/SDL/Godot/AIR/传奇/天龙八部（`TianLongBaBuHJ WndClass`）/桌面模拟器仍注入假焦点；注入失败时**后台窗口模式回退 LCA 窗口消息**（不抢前台 SendInput）。**禁止**给冒险岛注入 crashy 包（假 WM_INPUT/`164352`、18 方法表、user32 JMP）。**禁止**把天龙八部当未登记游戏只 PostMessage。**微信 4.x**（`Weixin.exe` / `Qt*QWindowIcon`）走精简假焦点：前景查询 + **软光标/GetAsyncKeyState**（找图点击）；宿主 PostMessage `KEY*`/`MOUSE*`；禁止当成 MuMu/安卓 Qt 壳，禁止 QQ 那套 Chromium 灌键线程、假 WM_INPUT、子类化、ClipCursor；**即使设置关闭假焦点注入也仍注入**；禁止 `WM_ACTIVATE`（会切到前台）、`WM_CHAR`（与 Qt 转字叠成两次）、快捷输入 `WM_PASTE`（仅 Edit/RichEdit 才算粘贴成功；QWindow/AIR/游戏/Chromium 壳须 KEY*/Ctrl+V）；注入失败仍只 `KEY*`、不回退假前台 SendInput。若仍改 FakeFocus32.dll，下列注入禁令仍有效：禁止 user32/win32u 方法体 JMP、禁止 18 方法表、禁止假 WM_INPUT（`164352`）、禁止 GetGUIThreadInfo / 扫游戏目录上一级（`156160`）、禁止改 dinput8 GetProcAddress IAT（`155648`）、禁止 Unacquire 槽、禁止辅助 DLL 可写节 poll（`slots≥10`/`0xCFA7`）。 

## 推荐迭代循环（复制即用）

```text
loop:
  MSBuild ... /t:WindowModeSelfTest
  run: build\Release\WindowModeSelfTest.exe --json
  if exit==0: break
  read FAIL name + detail → open file from table → patch → continue
optional:
  WindowModeSelfTest.exe --json --macro
  then manual: build\Release\QuickScriptTool.exe
```

## 相关路径

- 自检源码：`tools/window_mode_selftest.cpp`  
- 窗口模式核心：`src/window_mode/`  
- 主程序找图入口：`src/main_window.h`（FindImage + `wmUsesTarget`）  
- 用例详解：`reference.md`  
