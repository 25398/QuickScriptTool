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
| `hardware_offscreen_park` | 窗口化本机输入目标未能屏外+顶置，或结束未还原 | `window_target.cpp` `ParkHardwareInputTargetOffscreen` |
| `vda_selects_os_dll` | 宏桌面 DLL 选错代（Win11 加载了 Win10 VDA，GetDesktopCount 崩溃） | `virtual_desktop_accessor.cpp` `kDllCandidates` |
| `fake_focus_hook_local` | FakeFocus DLL hook/卸载失败 | `src/window_mode/fake_focus/**` |
| `fake_focus_lite_unreal` | UE5 精简假焦点导出缺失或仍钩 PeekMessage 注入 WM_INPUT | `fake_focus_dll.cpp` `FakeFocus_InstallLite` |
| `fake_focus32_export_rva` | 32 位 FakeFocus 导出为 `_Name@N` 时远程 InstallLite 找不到 | `inject_common.cpp` `FindExportRva` |
| `remote_module_kernel32` | Unity 等「目标进程未加载模块: kernel32.dll」，假焦点失败只剩 PostMessage | `inject_common.cpp` `FindRemoteModule`（Toolhelp 重试 + PEB） |
| `fake_focus_header_export_rva` | 导出名在 PE 头（SizeOfHeaders）内时 FindExportRva 报「未找到导出」 | `inject_common.cpp` `PeFileView::RvaToPtr` |
| `fake_focus_glfw_lite_cursor` | GLFW30 lite 未钩 GetCursorPos，或 SetCursorPos 仍挪真光标 | `fake_focus_dll.cpp` `ShouldSkipGetCursorPosHook` / `Hook_SetCursorPos` |
| `fake_focus_air_focus_only` | AIR/造梦注入后卡死退出或真光标原地抽 | `fake_focus_dll.cpp` AIR 只钩前景查询，禁止子类化/光标/RawInput |
| `fake_focus_maplestory_focus_only` | 冒险岛点运行后游戏无响应，或脚本在跑但角色不动 / 边框闪 / 2–3 秒闪退 | `fake_focus_dll.cpp` maple IAT + **仅 27–32 方法**设备虚表槽 + Acquire/GetDeviceState 方法体 JMP。**禁止 18 方法表（槽/JMP 都会几秒闪退）**。命中看共享内存 `钩命中`，禁止运行中 CreateRemoteThread |
| `window_mode_target_lost_stops` | 游戏已闪退但软件仍显示宏运行中、步数继续涨 | `TargetStillAlive` + `engine_script_run.cpp` 目标消失立即 stopFlag |
| `fake_focus_soft_input` | Phase2 软输入同步失败 | `fake_focus_soft_input*` / DLL GetCursorPos hooks |
| `anjuzhen_script_wm_config` | 安居镇.json windowMode 字段不符 | `build/*/scripts/安居镇.json` + `window_mode_json` |
| `permission_match_uipi` | 管理员运行仍提示「请以相同权限」 | `window_mode_permission.cpp` `CheckPermissionMatch` |
| `maplestory_bg_fake_focus` | 冒险岛后台绑上后立刻 EndRun（分类/UsesFakeFocus） | `window_mode_types.cpp` `LooksLikeMapleStory*` |
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
10. **AIR 禁止 Phase2；冒险岛禁止任何 user32/win32u 方法体 JMP**：`ApolloRuntimeContentWindow` 只钩前景查询。`MapleStoryClass` 前景/键鼠必须走 **IAT 导出名**（含 stdcall 修饰名与旧 delay-load VA）、主程序可写节指针扫描（GetProcAddress 缓存），并 **始终** PEB 扫**游戏目录**模块 IAT（即使 poll 已>0，也要补 SetForegroundWindow/FlashWindow；VirtualQuery 后再改；跳过系统目录/FakeFocus/NGS/opencv 等）。辅助 DLL **可写节只补前景/闪框**（GetForegroundWindow/GetFocus/SetFG/Flash，≤48 处；不要因 SizeOfImage>4MB 跳过）。**禁止**把 GetAsyncKeyState/GetKeyState/GetCursorPos 写进辅助 DLL 的 .data：`slots` 从 5 涨到 17 的那版（diag `0xCFA7`）会让星辰冒险岛**立刻闪退**。已加载的 `dinput8.dll`/`dinput.dll`：对确认过的 **27–32 方法**设备 vtable 改槽（Acquire/GetDeviceState/GetDeviceData），并对这些表上**唯一的** Acquire/GetDeviceState **方法体**打 JMP（最多各 4 个；同映像 E9 一跳可跟随，禁止 FF25、禁止跟进 user32/win32u/ntdll）。**禁止**改 18 方法 `IDirectInputDevice` 的槽或 JMP：`155136`/`21:25:14` 那版对 ≤2 张 18 方法表动手后，星辰冒险岛**等几秒闪退**（与乱 JMP 跳转表同一指纹）。GetDeviceState 只填 **cb==256** 键盘 / 16或20 鼠标。IAT 可钩 `DirectInputCreateA/W/Ex`（不是注入线程 CreateDevice）。游戏目录模块可写节可替换 **DirectInput 方法原指针**（不是 user32 poll 地址）。**禁止** GetDeviceData/Poll **方法体或 Poll 虚表**。**禁止**对 user32/win32u 做方法体 JMP。禁止注入线程 `DirectInput8Create`/`CreateDevice`、禁止从钩子里再调 `GetDeviceInfo`/`CreateDevice`、禁止 Toolhelp 扫全模块、禁止 CallThroughOriginal 逐帧拆补丁、禁止子类化 WndProc、禁止 RawInput/WM_INPUT。已注入时宿主**禁止**再 PostMessage。**禁止**运行中 `CreateRemoteThread` 查命中（改共享内存 `hitGaks/hitDiState/hitDiData/lastDiStateCb`）。日志 `u32jmp=1`/`diData=1` 后闪退 = 又打了 user32 或 GetDeviceData 方法体。日志 `slots≥10` 后立刻闪退 = 又扫了辅助 DLL 的 poll 指针。日志 `0xCFA3`/`slots=2` + `liveJmp=1` + `softOk=1` 仍无键：看 `钩命中 hitReady/gfw/focus/gaks/diState/lastCb`。`hitReady=0` = 共享内存只读（命中不可信）。`gfw=0` 且 `hitReady=1` = 游戏没走我们钩的 GetForegroundWindow。禁止再改 18 方法表。Device8 **Unacquire 槽**可改成返回 OK（不 JMP 方法体）。   

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
