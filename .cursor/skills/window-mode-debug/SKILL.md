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
| `background_fake_focus_not_degraded` | **后台模式却是「假后台」：跑脚本时鼠标/键盘被抢走**（MC / GLFW30 实测）；日志有「假焦点未生效（未注入 / 仅时钟补丁 / 钩已拆），回退假前台 SendInput（绝对坐标；会占键鼠）」 | 用户关掉「启用假焦点注入」+ 开着变速 ⇒ 只注入时钟补丁 ⇒ 没有假焦点钩 ⇒ 回退假前台 SendInput。修法：`ShouldInjectTimeScaleOnly` 判据**不得**带「关了注入」；`BackgroundTargetRequiresFakeFocus` 让后台+3D 目标忽略该设置（同微信/冒险岛）。**边界**：仅 `BackgroundWindow`，`HiddenDesktop` 不变 |
| （诊断）`假焦点输入体检` / `rescan=N轮/+M槽` | 冒险岛后台只原地平A、走路时灵时不灵（全零计数却仍不走） | 体检行判路径：泵=0 ⇒ 消息都不经钩子（晚解析/缓存指针，靠周期补挂）；**WM_INPUT>0 ⇒ Raw Input（后台天生收不到，禁止灌假 WM_INPUT——闪退）**；WM_KEYDOWN>0 ⇒ 消息驱动 + 激活态门控；软键按下=0 ⇒ 宿主没喂键。规则 `window_mode_requirements.h` §10.1/§10.2，实现 `StartMapleRescanThread`/`MapleIatFillPollPairs` |
| （诊断）`目标主循环节拍：N 次/秒` | 变速「时钟改了、游戏却没变快」 | 节拍**不随倍率涨** ⇒ 帧率被 vsync/固定帧/Sleep 封顶，时钟补丁天生改不动（LESSONS §3）；**禁止**为此放宽 IAT 扫描（宽扫会虚拟化网络栈时钟 ⇒ 心跳错乱掉线，用户实测） |
| `fake_focus_uses_bound_hwnd_class` | **后台+软输入全程卡顿**：日志出现「回退假前台 SendInput」或每一步都付 `PrepareSoftInput()` 全套代价 | 判「要不要假焦点」**必须传 hwnd**：`UsesFakeFocus(config, hwnd = nullptr)`。配置类名常为空（拖拽拾取后类名**只在 HWND 上**），只看 `config.windowClassName` 会漏判 GLFW30/SDL_app ⇒ 假焦点不装 ⇒ `PreferHardwareInput()` 里 `if (FakeFocusActive()) return false;` 不成立 ⇒ 全程软输入。**纯类名判断放头里**（底层头不能依赖 cdp 的 `QueryHwndProcessImagePath`，进程路径那步留在 cpp） |
| `soft_input_fast_path` | 「后台 + 软输入」卡顿的结构性开销 = `PrepareSoftInput` **每拍一次全树枚举** | **所有「每拍一次」的输入路径都必须走 `PrepareSoftInputFast`**，判据是纯函数 `CanUseSoftInputFastPathClass`（`window_mode_types.h`）—— ① 顶层窗有效 ② 绑定**就是顶层自身**（子窗绑定不适用）③ 客户区几何未变 ④ **顶层窗类名与缓存一致**（HWND 会被系统复用：旧窗关了开新窗可能拿到同值句柄、尺寸还恰好相同 ⇒ 前三条全成立却投递到**错误目标**且不报错）。失效点：`RefreshTarget()`、`EndRun()`。⚠ **键盘 `PostKeyToTarget` 曾漏改**（第一轮只改相对移动 ⇒ 大尖峰紧跟键盘动作）；4 组调用点曾**各调两遍** `PrepareSoftInput`，已删 |
| `fakefocus_stale_module_crash` | **注入后游戏立刻闪退**；日志里 `pid`/`hwnd` 跨小时甚至跨天完全不变（用户一直没重启游戏）；`假焦点钩命中 … hitReady=1` 但 `iatPoll=0 diag=0x00000000 foundVt=0` | 同一进程反复注入 = 双重挂钩（IAT 被两套 detour 覆盖、DI 方法体 JMP 叠加）。`fake_focus_injector.cpp` `TargetHasStaleFakeFocusModule()` 注入前按模块名查残留并警告；`InstallMapleIatHooksGuarded()` 用 SEH 兜底保游戏；看 `stage=`/`fault=` 定位死亡点。**先完全退出 MapleStoryt.exe 再跑** |
| `background_post_wrapped_container` | **后台按键完全没反应**（按 A 打不进目标应用，用户原话「按 A 打不到其他应用的后台里面」），日志 `后台输入子窗 kind=renderSurface class=NotepadTextBox`（现代记事本 / WinUI3） | `PostMessage` **不向子窗转发** ⇒ 投给只作容器的父窗 = **完全没投**。实测树 `Notepad` └ `NotepadTextBox`(755x553) └ `RichEditD2DPT`(755x553)，父子客户区**一样大**，而 `EnumChildWindows` 父先于子 ⇒「严格大于」的最大子窗启发式取到**包装层**。修法：`FindBackgroundInputChild` 用 `TextInputInsideSurface`（判据 **`IsChild(surface, input)`**）让位给真控件；`ResolveSoftInputHwnd` 对**已绑子窗**不得再用无 config 重解析覆盖（唯一例外：绑到的就是包装层）。⚠ **别改成「窗口里有没有输入框」**——会把投递目标从主区域挪到别处的小搜索框 |
| `background_self_translate_double_char` | 修好上一条之后**一次按键进两个字**（按 A 出 `aa`）；`class=RichEditD2DPT` / WinUI 文本控件 | 该控件**自己**把 `WM_KEYDOWN` 译成字符，宿主再补 `WM_CHAR` 就双发（实测 `KEYDOWN(A)+WM_CHAR('a')` → `'aa'`）。修法：`ClassSelfTranslatesPostedKeys`（类名白名单）+ `SelfTranslateKeyUsesWmChar`（`ch >= 0x20`）——**可打印字符只发 `WM_CHAR`**（由宿主用软修饰键态译好：控件自译只看**真实键态**，看不见脚本按住的 Shift，否则 `Shift+A` 退化成 `a`）、**其余只发 `KEYDOWN`**（实测 `WM_CHAR` 对 `\r`/`\t` 不换行/不制表）。⚠ 只发过 `WM_CHAR` 的键**不得**再补 KEYUP |
| `recorded_window_title_locks_rebind` | **后台窗口模式「不操作后台」**：录制回放时按键/点击到不了目标（用户原话「窗口模式自动识别，录制回放，不操作后台」）。⚠ **不是投递坏了，是根本没绑上** | 录制保存把**录制瞬间的标题**写进 `windowName`（`wm.windowName = wmTgt.windowTitle`），回放端 `BuildTargetQuery` 把它按 `" - "` 截成 stem 当 `titleContains`，而 `EnumWindowsOnDesktopProc` 里标题匹配是**硬门**（在类名匹配**之前**）。标题易变（换文档/换标签页/游戏换场景/存档改名）⇒ 枚举**零命中** ⇒ 绑不到。修法：录制端置 `windowNameIsHintOnly = true`；`BuildTargetQuery` hint-only 下**不产生** `titleContains`；`DoesTopWindowMatchConfig` 也**不拿标题**判「没绑到」——身份退化为 **进程路径 + 顶层类名(+子窗类名)**。⚠ 字段默认 `false` ⇒ 旧脚本/用户手配（标题关键词是**真意图**）行为不变。复现：`tools/verify/probe_record_playback_bind.py`（同窗同类名，仅标题变 ⇒ 命中 0；摘掉标题过滤 ⇒ 命中 1）；用例 `background_recorded_title_is_hint_only` |
| `lca_arrow_key_lparam` | 后台空格有效、方向键没反应 | `BuildWindowKeyLParam` 扫描码 0x4B；`NormalizeScriptKeyVk`；冒险岛方向键 `SendKeyboardKey`（仅前台） |
| `lca_nav_key_leaks_to_foreground` | 后台窗口模式跑脚本时用户在前台干别的被打扰：浏览器视频 ←/→ 跳进度、↑/↓ 调音量 | `background_window_input.cpp` `PostKeyToWindow` 方向键兜底**按下**必须 `TargetOwnsForegroundWindow(send)` 才 `SendKeyboardKey`；目标在后台只写 SoftInput + PostMessage |
| `lca_nav_keyup_released_after_focus_loss` | **切走之后游戏朝「离开时那个方向」一直走**（用户原话：朝离开时候的那一个方向 瞬移）；切回任何程序都像在持续按方向键 | 方向键兜底真键**松开**要看 `ShouldMirrorNavKeySend(down, fg, mirroredDown)` —— 按「当初补过没有」而不是「此刻是否前台」；`ReleaseMirroredLcaNavKeys()` 在 BeginRun/EndRun 兜底松键 |
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

## 「扩展离线 / 又开了一个浏览器」三分钟判读（2026-09-27 真机）

用户报障原话往往是**「缩略图已经定位到目标窗口了，可是还是打开了新标签页」**。按下面顺序看日志，
不要一上来就怀疑绑窗 —— 绑窗那一步通常是好的（日志里明确写着「已找到匹配的目标窗口，跳过自动打开」）。

| 日志里看到 | 含义 | 怎么办 |
|---|---|---|
| `扩展桥 WS token 不匹配：扩展 ...xxxx(32) ≠ 桥 ...yyyy(32)` | 扩展手里是**陈旧 token**（宿主每次启动都换） | 到扩展**选项页**点「重新连接」。⚠ 长度恒为 32，**没有任何诊断价值** —— 看指纹 |
| 同一行里两侧**指纹相同** | 不是 token 问题 | 去看扩展 SW 控制台（`edge://extensions` → Service Worker） |
| `扩展离线 ⇒ 后台启动浏览器：…（已发起）` | 桥把一个**新浏览器实例**拉起来了 | 若浏览器本来就在跑，这就是用户看到的新窗口/新标签页 |
| `扩展离线但浏览器已在运行 ⇒ 不新开窗口` | 命中 2026-09-27 加的护栏（正解） | 只需让扩展重新连上；**这不是错误** |

⚠⚠ **旧版扩展侧的坑（已修，别照旧版推断）**：`offscreen.js` 一进发现流程就把
`nativeProbeWanted` 置 `true`，导致「读 `bridge_runtime.json`」那条**零进程开销**的直读路径
**从来没生效过**，每次都要走带 `NATIVE_PROBE_MIN_MS = 30s` 节流的 native 探测。
宿主换了 token 之后，扩展会拿着旧凭证**空转 30 秒**，而宿主的 `WaitForExtension` 等不到就判
「扩展离线」⇒ 去拉浏览器。判据：日志里 `WS token 不匹配` 之后有 30 秒左右**一条连接尝试都没有**。
修法：只有「这份凭证**真的被试过且失败**」才允许问 native（判据盯**被拒的那个 token**，
不盯「上一轮没连上」—— 后者在宿主没跑时会变成每 1.5s 拉一个完整宿主进程）。

### 「后台窗口模式直接让游戏崩溃」怎么取证

⚠⚠ **第一步不是看字段，是看「这一轮到底做过什么」**（2026-10-03 踩到）。

先找**动作证据行**：`假焦点已注入` / `假焦点注入技术=` / `输入策略=` / `目标窗口已消失 … exit=`。
**一条都没有** ⇒ 这一轮**压根没走到注入**，日志只是**崩溃后的善后轮**
（典型形态：`未找到目标窗口，自动打开` → `仅绑定已有标题窗` → `EndRun`，
因为微端已经没了所以「未找到」）。⇒ **它证明不了崩溃原因，别在里面找根因。**

真实前科：用户报「给微端用后台窗口模式后游戏微端崩溃」，给的日志正是上面那三行。
当时差点按「`ShouldAbortAutoLaunchOnBindFailure` 不含 `TargetNotFound` ⇒ 重复启动搞崩游戏」
写一个假修复 —— **读码核实后否定**：`LaunchTargetOnDefaultDesktop` 的
`editorNeedsIdentity && documentFile.empty()` 分支是 `if (BindTargetWindow(err)) return true;
else return false;`，**只绑窗、不启动**。
⚠ 元规则：**「注释说危险 + 判据看着像漏」≠「真有洞」，把那条路径读到 `return` 再下结论。**

看到字段之后再往下读。target-lost 那一行带三个关键字段：

- `本轮已跑=NNNms` —— **绑定后几十毫秒就没** vs **跑了几秒才没**是两种完全不同的故障
  （前者指向注入/绑定本身，后者指向某个动作把游戏搞崩）；
- `最后动作=[3]找图，…` —— 崩溃触发点（需要调试窗开着）；
- `proc=2 exit=0x00000000` —— ⚠ **`0x00000000` 表示进程自己正常退出了，不是崩溃**；
  真崩溃是 `0xC0000005` 之类。**先读这一位再谈"是不是我们把它搞崩的"**，
  否则会把「游戏自己退出」误判成注入事故。

AIR（`ApolloRuntimeContentWindow`，造梦西游 / 4399 微端）已知脆：只钩前景查询（禁 Phase2/子类化）。

⚠ **2026-10-03 已修两条「whitelist 泄漏」**（用户报障「给微端使用后台窗口模式后游戏微端崩溃」）：

1. **AIR 识别现在看子窗**（`HwndTreeLooksLikeAdobeAir`）。`ApolloRuntimeContentWindow`
   常是**内容子窗**，父窗是启动器/包装窗（4399 微端就是这个结构）；只看 `GA_ROOT`
   会漏判 ⇒ 走全量 Phase2 ⇒「一启动就卡死退出，鼠标原地抽」。
2. **变速时钟钩不再改 AIR 的代码页**：`SetIatOnlyMode(mapleSafe || airSafe)`。
   它原来装在 `airSafe` 早退**之前**，非 IAT 模式下会把所有模块的时钟函数做**内联钩**
   ⇒「开倍速过一会就闪退」。
3. `setwindowshook` 注入（在目标 UI 线程 `LoadLibrary`）也已把 AIR 列入禁止名单，
   判据收在 `ForbidsSetWindowsHookTechnique()`。

**若还要怀疑变速**：仍建议用设置里的「启用窗口变速」做 A/B（现在 AIR 只剩 IAT 补丁，
理论上不该再崩）；但**先确认日志里到底有没有 `假焦点已注入` 那一行** ——
用户给的那份日志停在「未找到目标窗口，自动打开」就 `EndRun` 了，
那是**崩溃后的善后轮**，不是崩溃现场。

### 「后台只剩平A、不能走A」怎么判读（2026-10-03，冒险岛）

⚠⚠ **症状指纹**：**消息驱动的那半活着、共享内存/DI 的那半死了**。

- **攻击键**（`MapleStoryClass` 技能键）走 **PostMessage** ⇒ **照打** ⇒ 用户看到「平A 有效」；
- **方向键**依赖 `FakeFocusSoftInput_SetKey` 写共享内存 `down[]` + DLL 内 **DirectInput 软键**
  ⇒ 一旦假焦点没装上就**全失效** ⇒ 用户看到「不能走A」。

⇒ 用户原文「**原地不动的平A，不能走A**」就是这条链的精确指纹。
根因不在「方向键代码」，在**假焦点注入没成功**。

**为什么注入失败后不会自动回退**：`PreferHardwareInput()` 在 `UsesBackgroundWindow()` 下
**永不回退**假前台 SendInput（红线 22「后台永不抢鼠标」）⇒ 键鼠只剩 PostMessage。
⚠ 这是**设计**，不是 bug —— 别去改它。

**第一步：看日志的「形态」，不是看「值」**（与上面「崩溃取证」同一族）：

```
BeginRun：窗口模式启用 kind=BackgroundWindow
BeginRun：生效 kind=BackgroundWindow targetExe=…\MapleStoryt.exe
构建指纹： QuickScriptTool.exe=… FakeFocus32.dll=… FakeFocus64.dll=…
EndRun：窗口模式会话结束
```

一轮里**只有这四行、没有任何 `假焦点已注入` / `假焦点钩命中` / `假焦点钩安装` /
`假焦点输入体检`** ⇒ **诊断根本没产生 = 注入没成功**。别去读注入之后的代码。

**为什么会「看不到失败原因」**（2026-10-03 已修，但旧版日志就是长这样）：
① 注入期失败原来只走 `WindowModeLogf`（**非 Event ⇒ 不落盘**），
而 `diagnose_report.txt` 只抓 `window_mode_debug.log` 的**末 120 行**；
② 宏调试窗 sink 用 `runningWindowMode_.enabled` 过滤，嵌套模式的 `publishRunningWm()`
原来排在 `beginWmCfg()` **成功之后** ⇒ 注入期日志全被丢；
③ DLL 早退路径 `if (!g_softView) OpenSoftInputView()` —— **非空 ≠ 有效**
（宿主每轮重建映射，旧 view 指向**布局已变**的旧 section，`kSoftInputVersion` 7→10）
⇒ `MaplePublishHits()` 静默 `return` ⇒ 宿主读全 0 + 软键态全失效。

**新版（10-03 晚之后）应该看到这些行**。先看**三分表**一眼定位：

| 看到什么 | 结论 | 下一步 |
|---|---|---|
| `假焦点决策：mapleStory=? …` + 某条早退行（`未注入假焦点，键鼠走 PostMessage` / `未登记游戏：跳过假焦点注入` / `仅注入时钟补丁` / `假焦点跳过：内核反作弊…`） | **没尝试注入** | 看 `mapleStory=` 是不是 0 ⇒ 目标没被识别（`targetExe`/类名/标题） |
| `⛔ 假焦点注入失败…: <原因>` | **尝试了但失败** | 看冒号后的原因 |
| `冒险岛假焦点已注入…` 有，但**没有**紧跟「假焦点钩命中/钩安装」 | **注入成功、进程内 DLL 没往共享内存写** | 进程里挂着**旧版** DLL ⇒ **完全退出游戏再运行** |

⚠ 那三行诊断（`假焦点钩命中`/`钩安装`/`输入体检`）在冒险岛目标上走 `lite && mapleStory`
分支，该分支前面有 **6 条会静默早退**的路径，所以「三行全消失」有 **三种**完全不同的原因。
2026-10-03 之前这些早退全是非 Event 日志 ⇒ 现场**区分不了**（这也是那次查很久的原因）。

⚠ **2026-10-04 起范围扩大**：这三行现在**也在 `native3d` 分支打**（Unity / UE / GLFW /
桌面模拟器）。之前它们**只在冒险岛分支打** ⇒ 非冒险岛目标（如 Unity 报障「后台窗口模式
鼠标不移动到指定位置」）日志里**一条命中数据都没有**，只能靠猜。配套改动：
- DLL 侧 `MapleNotePumpMessage` 去掉 `if (!g_mapleSafe) return;` 门闩，通用泵钩子
  （`HookPeekMessage`）现在也计数 ⇒ 非冒险岛目标的 `hitPump/msgInput/msgKey` 不再恒 0。
- 新增一行 `假焦点软光标 <when> 假光标=(x,y) cursorValid=N postKeyEvents=N`，回答
  「**宿主到底有没有把光标喂进去**」——`SyncFakeFocusCursor()` 在共享内存没挂时是
  **静默 return**，光看「假焦点已注入」永远发现不了。`cursorValid=0` ⇒ 宿主没喂光标
  ⇒ `GetCursorPos` 钩子会回退到真光标 ⇒ 后台移动必然无效。

详细行含义：

| 日志行 | 含义 / 下一步 |
|---|---|
| `假焦点决策：mapleStory=… fakeFocusNeeded=… timeScaleWanted=… timeScaleOnly=… class=… targetExe=…` | **决策输入**（每轮一条）。`mapleStory=0` ⇒ 目标没被识别，先查 `targetExe`/类名 |
| `坐标语义 窗口客户区相对 / 屏幕绝对（coordSpace=… 录制客户区=WxH）` | 脚本的**坐标系**（`BeginRun` 一条，2026-10-04 加）。选到「屏幕绝对」时紧跟一条 `⚠ 本脚本按屏幕绝对坐标回放…` ⇒ 回放用**当前**窗口位置换算，**窗口一动全部坐标动作整体偏移**。用户症状原话：「点不到指定位置 / 鼠标像没动」、「窗口移动后就不能用了」。修法：用后台窗口模式**重录**，或改「窗口模式」 |
| `假焦点软光标 <when> 假光标=(x,y) cursorValid=N postKeyEvents=N` | **宿主到底有没有把光标喂进去**（2026-10-04 加）。`cursorValid=0` ⇒ 宿主没喂 ⇒ `GetCursorPos` 钩子回退真光标 ⇒ **后台移动必然无效**（这是宿主侧缺陷，不是游戏的事） |
| `假焦点钩命中 … gfw=0 gaks=0 diState=0`（**Unity/UE/GLFW 目标**） | ⚠ **别读成「游戏不走这些入口」** —— 这几个位是**冒险岛专用**（`MapleBumpHit` 只在 `g_mapleSafe` 时递增，**故意没放开**：`gaks` 还被 `EvaluateKeyStatePhase` 当「客户端在不在查键态」的判据）。对通用目标恒 0 = **没人在数**。真正有效的是 `输入体检` 行的 `泵/WM_INPUT/WM_KEY` |
| `输入体检 … 泵=0` | ⚠ **先看「假焦点注入技术=… lite=?」**：`lite=1`（UE5 精简）**本就不钩 PeekMessage**（`InstallRawInputHooks` 里 `if (lite) return;`）⇒ 恒 0 属正常。`lite=0` 时泵=0 才是「游戏不走我们的钩子」 |
| `⛔ 假焦点注入失败（精简/…）: <原因>` | 注入失败 —— **看冒号后的原因**（旧版这一行不落盘） |
| `⛔ 假焦点注入中止：软输入共享内存创建失败 pid=…` | 共享内存 `CreateFileMapping`/`MapViewOfFile` 失败（权限/SDDL） |
| `⛔ 假焦点注入被拒：目标进程 pid=… 已加载另一份 <路径>` | 进程里有**另一个路径**的 FakeFocus ⇒ 双重挂钩会带走游戏，已拒绝。**重启游戏进程** |
| `⚠ 目标进程 <pid> 里挂的是**旧版** FakeFocus：<路径> 在进程启动之后被改写过` | **升级了软件但没重启游戏** ⇒ 共享内存布局可能不兼容。**完全退出游戏再跑** |

**判据速查**：`同路径 ≠ 同一份内容`（`LoadLibrary` 同路径只加引用计数、**不重跑 DllMain**）；
`InjectedModuleLooksStale(dllWrite, procStart)` 只是**启发式**（宁可漏报），
真正的兜底是**每次早退都无条件重建共享视图**。

⚠ **用户报「还是」时先核构建指纹**：本次用户跑的 `QuickScriptTool.exe=7519232@2026-10-02 23:41`
落后 HEAD 两个提交 ⇒ 「还是」**不能**读成「上一版修复无效」。
详见 `window_mode_requirements.h` §22、`LESSONS.md` §85、红线 68。

## 「滚动不动 / 拖拽抢鼠标」怎么判读（2026-09-30 真机）

### 滚动滚轮没反应 —— 先看这一行，别猜

日志里现在每次滚动都会有一行：

```text
[窗口/后台窗口模式] 滚轮投递 竖向 反向 步数=1 目标=0x…(类名) 客户区=(x,y) 屏幕=(x,y) 成功=1/1
```

| 现象 | 结论 |
|---|---|
| **完全没有这一行** | `PostScrollWheelAtClient` 没走到投递：看是不是 `scrollVertical`/`scrollHorizontal` 都是 0，或 `PrepareSoftInputFast` 失败（会另有一行「鼠标移动被跳过」） |
| `成功=0/1` | 投递失败：目标句柄失效 / 被 UIPI 拦 |
| **`成功=1/1` 但目标类名是容器** | ⚠⚠ **最坑的一格**：投递"成功"只是 `SendNotifyMessage` 接受了，而 `PostMessage` **不向子窗转发** ⇒ 给容器等于没投。**必须同时看目标类名，别只看成功数** |
| 目标类名是真输入控件、`成功=1/1`，目标仍不动 | 该程序不读 `WM_MOUSEWHEEL`（走 Raw Input / 自己处理），不是投递问题 |

**真机实例（2026-09-30，Win11 商店版记事本）**：

```text
后台输入子窗 kind=configChild class=RichEditD2DPT …         判据=配置的子窗类名命中   ← 绑定是对的
滚轮投递 竖向 反向 步数=1 目标=0x…1307AE(Microsoft.UI.Content.DesktopChildSiteBridge) … 成功=1/1
                                                                  ↑ 投给了容器
```

根因：绑定 `ResolveBindHwnd(top, config)` 是**带 config** 选的（`RichEditD2DPT`），
但**鼠标/滚轮**那条路调的是 `ResolveBackgroundPostTarget` → `FindBackgroundInputChild(root, nullptr)`
—— **config 传 nullptr**，于是把绑定丢掉、用「最大后代」重新猜，猜到容器。
**键盘不走这条路**（用 `ResolveSoftInputHwnd`，认得子控件绑定）——所以缺陷只在鼠标侧、
只在特定窗口树上显形。已按 `window_mode_requirements.h` **硬规则 ②** 补齐：
绑定窗自己就是真文本控件时，投递目标就用它（判据收得很窄，不动其它已调好的格子）。
自检 `wheel_targets_bound_child_not_wrapper`（已 A/B：还原启发式 ⇒ 目标变成容器 ⇒ 红）。

⚠ **写这类用例时别去数消息条数**：投出去之后落在哪由 `SendNotifyMessage` + `DefWindowProc`
决定，在我们自建的假 WndProc 上**不可观测**（上一版用例就是数消息 ⇒ 红绿都不代表产品行为，
白折腾了一轮）。要钉就钉**选择结果**（`ResolveMousePostTargetForTest`）。

### 滚动滚轮没反应 —— 另外三处各写了一份 `WHEEL_DELTA * steps`

| 检查 | 结论 |
|---|---|
| 日志有没有 `假焦点软滚轮 入队 …` | 有 = 宿主入队了；**没有** = 连入队都没发生 |
| `FakeFocusSoftInput_WheelCursors` 的 write 涨不涨 | 不涨 = 没入队（旧版 `PushWheel` 会因一个标志没置位直接 return） |
| write 涨、read 不涨 | 目标进程没取走 ⇒ DLL 没装 RawInput 钩 / 是上一版 DLL（重启游戏） |
| 步数 ≥ 274 | ⚠ **`SHORT` 回绕**：`120×274 = 32880 > 32767` ⇒ 正着滚变成倒着滚 |

⚠⚠ 三条坑都是"静默"的，历史上**全都踩过**：
1. `static_cast<SHORT>(WHEEL_DELTA * steps)` 回绕（274 格以上方向反过来）；
2. `notch = steps` 把**步数当增量**塞进 wParam 高位（填 300 步也只滚 1 格）；
3. `PushWheel` 前置 `kSoftFlagPostKeyEvents` —— 那个标志**只有 Chromium/Qt 壳会置**，
   普通游戏（GLFW/Unity/UE，正是假焦点存在的理由）走进程内软输入，
   于是滚轮请求被丢掉，而日志照样说"投递成功"。

⇒ 规则已收进 `src/window_mode/mouse_wheel_events.h`（**唯一事实来源**），
三处调用点（宿主 PostMessage / 假焦点 DLL / 硬件 SendInput）共用。
**别再在调用点自己写 `* steps`。** 自检：`mouse_wheel_step_events` / `fake_focus_wheel_enqueue`。

游戏侧落点：`FillPendingRawInput()` 把每一格滚轮编成**独占一条** `WM_INPUT`
（`RI_MOUSE_WHEEL/HWHEEL` + `usButtonData`）。⚠ 只发 `WM_MOUSEWHEEL` 是**驱动不了**
Raw Input 游戏的 —— 所以两条都要发（消息驱动的应用读后者）。

### 拖拽"抢占鼠标"

**判据**：日志里出现「本机绝对光标 客户区(…) → 屏幕(…)」或「假前台本机输入已激活」
⇒ 这一轮在抢用户的鼠标（`EnsureHardwareInputFocus` 切前台 + `SetCursorPos` 搬系统光标）。

成因：`PreferHardwareInput()` 为 true ⇒ **所有**鼠标动作（移动/按键/点击/滚轮）
都走 `SendHardwareCursorToClient`；拖拽一次要搬 100+ 次，用户的手被整只抢走。

⚠ 它**根本不会成功**：后台模式下目标不在前台，`SendInput` 打的是用户当时在看的那只窗。
⇒ 现在 `UsesBackgroundWindow()` 下一律 `return false`（与「模拟器注入失败 ⇒ 后台不回退
SendInput」同一把尺），并**每次运行告警一次**说清代价（键鼠只剩 PostMessage，
Raw Input 游戏可能无响应）。用户看到的是"没反应 + 一条解释"，而不是"鼠标被抢走"。

⚠ 同类还有一处：`GetRawInputData` 吞真鼠标时原来把 `usButtonFlags/usButtonData` **整条清零**，
连用户自己手动滚的轮也一起吞了。现在只清非滚轮位。

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
