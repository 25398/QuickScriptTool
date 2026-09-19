# Agent notes (QuickScriptTool)

## UI 方向（WebView2 壳）

产品 UI 目标改为与 `docs/ui-redesign-mockup.html` 1:1，采用 **WebView2 壳 + 现有 C++ 引擎**（禁止 Electron 整仓重写；禁止 WebView 内 SendInput）。

- **产品 = Web**：CMake 目标 `QstWebViewShell` 输出 **`QuickScriptTool.exe`**（`ui/` + `src/engine/qst_engine.h` + `src/desktop_tools/`）；本地入口 `build\Release\QuickScriptTool.exe`
- **永久原生（DesktopTools）**：选区 / 找图叠层 / OCR 叠层 / 准星 / 启停·脚本热键捕获 / 托盘 — **禁止**塞进 WebView 冒充
- Phase-1 壳：`src/webview/qst_webview_shell.cpp`；引擎：`src/engine/engine_runtime.cpp` + `engine_host_window.h`（headless；无 GDI 产品 exe）
- 分层库：`qst_desktop_tools` / `qst_engine`（OBJECT，`cmake/QstProductLibs.cmake`）
- 前端：`ui/index.html` + `shell.css` + `bridge.js`
- **Fixed Version 便携**：[`docs/webview-fixed-runtime.md`](docs/webview-fixed-runtime.md)
- Bridge / 分期：[`docs/webview-bridge-api.md`](docs/webview-bridge-api.md)
- 原生分层盘点：[`docs/webview-native-layering.md`](docs/webview-native-layering.md)
- 打包：`powershell -File tools\package_webview_portable.ps1` → `dist\QstWebViewShell-Portable.zip`；完整发版 `tools\package_release.ps1`（主产物 Shell）
- **一键发版**：`powershell -ExecutionPolicy Bypass -File tools\package_with_version.ps1 -Version 1.3.4`
  —— 自动写版本号三处 → 构建 → 组装 dist + 便携 zip → 编安装包 → 同步 `website\downloads`。
  刻意不依赖写死的东西：版本号用正则写入（不依赖行号）、构建目标从 `package_release.ps1` 解析 `--target`、
  打包逻辑完全复用该脚本，所以后续新增源码 / UI / 扩展 / Skill 都不用改它。失败默认回滚版本号；
  `-DryRun` 只看计划，`-SkipBuild` / `-SkipInstaller` 可裁剪步骤。
  **需在普通 PowerShell / CMD 终端运行** —— 受限宿主（某些 AI 终端 / 沙箱）禁止启动 cmake/ISCC，脚本会提前提示。
  最省事的入口是仓库根目录的 **`release.cmd`**（纯 ASCII，会先 `cd /d "%~dp0"`，所以在任何目录下调用都行；
  双击时结束会 pause）：`release.cmd 1.3.4`、`release.cmd 1.3.4 -DryRun`，不带参数则提示输入版本号。
  直接用 PowerShell 调用时**必须给绝对路径**（在别的目录下用相对路径会报「`-File` 形式参数不存在」）。
- 发版版本：`tools\product_version.txt`（须同步 `installer\QuickScriptTool.iss` 的 `MyAppVersion` 与 `src\app_branding.cpp`）
- 发版依赖：运行必需项（MSVC CRT 旁路、`WebView2Fixed`、`ui`、OpenCV、扩展、驱动**安装脚本**）必须进 zip/安装包；`interception.dll` / `.sys` 不进默认包（设置里下载 `QuickScriptTool-HidDriver.zip`）；OCR Python 可运行时再装

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_webview_portable.ps1
```

## Module self-tests（必读入口）

统一约定与 suite 索引：

**[`.cursor/skills/module-selftest/SKILL.md`](.cursor/skills/module-selftest/SKILL.md)**

动作语义注意：`mouseClick`/`keyClick` 等 `duration` 是**重复间隔**（两次之间），不是执行前等待——细节见元 Skill「重复间隔语义」。

流程：选 suite → `MSBuild` Release `/t:<Target>` → `build\Release\<Target>.exe --json` → exit `0` 才宣称修好。

| Suite | Target | Skill |
|-------|--------|-------|
| 窗口模式 | `WindowModeSelfTest` | [window-mode-debug](.cursor/skills/window-mode-debug/SKILL.md)（需求：[cdp-requirements.md](.cursor/skills/window-mode-debug/cdp-requirements.md)；踩坑：[cdp-lessons.md](.cursor/skills/window-mode-debug/cdp-lessons.md)） |
| 定时任务 | `ScheduledTaskSelfTest` | [scheduled-task-debug](.cursor/skills/scheduled-task-debug/SKILL.md) |
| 宏变量 | `MacroVariablesSelfTest` | 见元 Skill FAIL 表 |
| 动作构建 | `ScriptActionBuilderSelfTest` | 见元 Skill FAIL 表 |
| 坐标 | `CoordSpaceSelfTest` | 见元 Skill FAIL 表 |
| 脚本 IO | `ScriptIoSelfTest` | 见元 Skill FAIL 表 |
| 找图引擎 | `ImageMatchSelfTest` | 见元 Skill FAIL 表 |
| AI 路由 | `AiActionRouterSelfTest` | 见元 Skill FAIL 表 |
| 设置库 | `AppSettingsStoreSelfTest` | 见元 Skill FAIL 表 |
| 主题 UI | `ThemeUiSelfTest` | 见元 Skill FAIL 表 |
| 脱离冷却 | `BreakoutCooldownSelfTest` | 见元 Skill FAIL 表 |
| 连点时序 | `ClickerTimingSelfTest` | 见元 Skill FAIL 表 |
| 录制回放 | `RecorderSelfTest` → `QstRecorderLogicTest.exe` | 见元 Skill FAIL 表 |
| 虚拟 HID | `VirtualHidSelfTest` | `driver/qst_vhid/` + `src/input/virtual_hid.*` |
| 桥接 JSON | `BridgeJsonSelfTest` | `src/json_util.h`（`GetSubObjectText` / 解析失败诊断） |
| 桥接命令契约 | `BridgeContractSelfTest` | `src/webview/bridge_commands.h` ↔ C++ 分派 ↔ `ui/*.js` |
| 引擎执行 | `ScriptRunnerSelfTest` | `src/engine/engine_ui_hooks.*` + 执行核心；`--engine` 跑 headless 动作 |

共享 harness：`tools/selftest_harness.h`。各 exe：`tools/*_selftest.cpp`。

### 构建模板

```powershell
& "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:<Target> /m /v:minimal
build\Release\<Target>.exe --json
```

窗口模式可选烟雾：`--macro`。定时任务硬规则见 scheduled-task-debug skill。

PowerShell 不要用 `/t:A;B` 拼多个 target（分号会拆命令）；分开跑。`--list` / `--json` 结果均在 stdout。

### 一键跑全部自检（推荐入口）

```powershell
# 构建 + 跑 15 个纯逻辑 suite（CI 同款口径，失败即 exit != 0）
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1

# 已构建过，只跑不编；同时落盘日志
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -SkipBuild -LogPath build\selftest.log

# 加跑交互类（窗口模式 / 虚拟 HID / 注入，需桌面会话与已装驱动）
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -Tier full
```

CI：`.github/workflows/build.yml`（push/PR 自动构建产品壳 + **21 个** SelfTest 目标，跑其中的逻辑档
—— 逻辑档当前 **18 个** suite；窗口模式 / 虚拟 HID / 注入 3 个只编不跑，需桌面会话与已装驱动）。
> 数量会随新增 suite 变化，以 `tools/run_all_selftests.ps1` 的 `$LogicSuites` / `$InteractiveSuites` 为准。

### 构建陷阱（踩过，别再踩）

- **MSBuild + `https_proxy`/`HTTPS_PROXY` 并存 → 直接失败**。报错是
  `MSB6001: "CL.exe"的命令行开关无效。System.ArgumentException: 已添加项。字典中的关键字:"https_proxy"…`，
  看起来像编译开关问题，实际是 MSBuild 用大小写不敏感的字典构造子进程环境。
  绕法：构建前在本进程内 `Remove-Item Env:HTTPS_PROXY`（`run_all_selftests.ps1` 已内置护栏）。
- **OBJECT 库的 `.obj` 不会经由中间 OBJECT 库传递**。只链 `qst_engine` 会缺
  `qst_desktop_tools` 的符号（`RenderBatchScope` / `UiEditorWidth` / `ResolveProgramLaunchPath`），
  所以链接列表要像 `QstWebViewShell` 那样逐个列出。想让某个源只编一次，**必须用 STATIC 库**
  （`qst_utils` 即此例），OBJECT 库做不到。
- **`qst_engine` 目前无法脱离壳链接**：`qst::webview::PostToWebUi` / `HotkeyLogLine` /
  `NotifyWebDebugWindowSetting` / `SyncHomeSelectionCache` 与 `g_instance` 只在
  `src/webview/qst_webview_shell.cpp` 定义。SelfTest 链引擎需带上 `tools/engine_link_stubs.cpp`
  （空实现）。彻底修法见 `docs/refactor-progress.md` §3.1。
- **含中文的 `.ps1` 必须 UTF-8 带 BOM**（ANSI 代码页是 GB2312）。写文件用
  `[System.IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding($true)))`。
  全仓自查：`tools/` 与 `driver/qst_vhid/` 下曾各有一批漏网的（含
  `package_webview_portable.ps1`，会让便携包内的中文 README 整个乱码）。

## Edge 配套扩展（发版必带）

源码：`extension/edge/`（非编译产物）。规范全文：**[extension/PACKAGING.md](extension/PACKAGING.md)**。

| 动作 | 命令 / 路径 |
|------|-------------|
| 校验 + 打侧载 zip | `powershell -File tools\pack_edge_extension.ps1` → `dist\QstEdgeBridge-<ver>.zip` |
| 打软件 Release / 安装包素材 | `powershell -File tools\package_release.ps1`（内部会先跑上一步） |
| Inno 安装包 | 先 `package_release.ps1`，再编 `installer\QuickScriptTool.iss` |

硬规则：改扩展版本须同步 `manifest.json` 与 `background.js` 的 `BRIDGE_VERSION`；`manifest` 必须是合法 JSON（`version` 行用逗号不是分号）；**禁止**发不含 `extension\edge` 的安装包/zip。

## AI 助手 Skill（内嵌助手能力，非 Prompt）

内嵌 AI 助手（`ui/agent.html` + `src/agent_*`）的能力以 Skill 文件定义，随包分发到 `skills/agent/`：

| Skill | section | 内容 |
|-------|---------|------|
| [agent-conversation](.cursor/skills/agent-conversation/SKILL.md) | `conversation` | 编辑已发送消息并重发（`rewindUserIndex` + `TruncateHistoryToUserRound`） |
| [agent-revert](.cursor/skills/agent-revert/SKILL.md) | `revert` | 变更撤销（`listAgentChanges` / `revertAgentChange`，快照存 `AppDir()\agent_changes`） |
| [agent-shell](.cursor/skills/agent-shell/SKILL.md) | `shell` | 白名单命令 `runAgentCommand` + 文件工具 + 剪贴板（`src/agent_shell.cpp`） |
| [agent-script](.cursor/skills/agent-script/SKILL.md) | `scriptStrategy` | 脚本生成：先 `planScriptActions` 规划动作树（`children` 嵌套），再 `buildScriptActions` + `createMacroScript` |
| [agent-optimize](.cursor/skills/agent-optimize/SKILL.md) | `optimize` | 录制/宏优化：`optimizeRecording` / `optimizeScript`（与产品对话框同一套按关键动作分段合并/压缩） |
| [agent-command](skills/agent/command.md) | `command` | **产品自有**（不在 `.cursor/`）：AI 动作执行里什么时候该用命令行（powershell/cmd）——`runCommand` 签名、`resolveSystemPath`、不回显 stdout 要重定向后 `readAgentFile`、Excel COM/CSV、命令如何落到「运行程序」动作可回放、当前无沙箱的安全边界 |
| [agent-office](skills/agent/office.md) | `office` | **产品自有**：Excel/Word/PPT/PDF 读写路线——**读**用 `readDocument`（`tools/office/read_doc.ps1`：COM / OOXML 直读 / Windows 自带 PDF IFilter / 扫描件渲染成图），**写**用 `runCommand` 的 COM/CSV 配方；含中文 BOM、COM 收尾与挂起、公式重算、`~$` 独占锁等实测坑 |
| [agent-game](skills/agent/game.md) | `game` | **产品自有**：实时游戏/动态画面——VLM 不进每步路径（找一次→本地跟住）、颜色/找图优先、`keyDown/keyUp` 长按、`moveMouseRelative` 与指针加速、节奏用 `duration` 不用 `wait`、反作弊风险提示 |

助手按 `AppDir()\skills\agent\<section>.md` → `AppDir()\skills\<section>.md` → 内嵌兜底读取。
改动助手 Skill / 优化算法后必须跑 `AgentAssistantSelfTest --json`（exit 0）并同步 `.cursor/skills/agent-*` 与打包脚本的拷贝列表。

**文档能力（readDocument）**：`tools/office/read_doc.ps1` 是**运行必需项**，随包分发到
`<exe目录>\office\read_doc.ps1`（CMake POST_BUILD 已拷；两个打包脚本也已加）。
改它之后必须重新构建（否则 `build\Release\office\` 下仍是旧副本，自检会测到旧脚本）。
支持 xlsx/xlsm/xls/xlsb、csv/tsv、docx/doc、pptx/ppt、pdf、txt/md/json/log/xml/ini/sql。
PDF 文本走 **Windows 自带 IFilter**（无 Office 无 Python，实测 1147 字中文 0.03s）；
扫描件自动渲染 PNG 交给多模态模型；`.pptx` 页序按 `sldIdLst`+rels 解析（**不能**按文件名排）；
`.pptx` **不能**手搓最小 OOXML 生成（真实 pptx 44 个 part），生成 PPT 用 PowerPoint COM。
研究依据：`.research/office-document-io-report.md`；游戏方向见
[docs/realtime-game-vision-loop-research.md](docs/realtime-game-vision-loop-research.md)。

**Skill 归属**：`.cursor/skills/agent-*` 是给 Cursor/开发者的；**产品功能相关的 Skill 放 `skills/agent/`**
（`command.md` 即此例，打包脚本优先拷贝 `skills/agent/<section>.md`，缺失时才回落到
`.cursor/skills/agent-<name>/SKILL.md`）。改 Skill 的读取/内嵌兜底要同步 `ai_action_router.cpp`
的同名 section 函数（如 `MacroActionCommandSkill()`）。

### 核心约定（改动前必读）

- **含中文的 `.ps1` 必须存成 UTF-8 带 BOM**。本机 ANSI 代码页是 GB2312：无 BOM 时 PowerShell 5.1
  按 GBK 解析 → 中文乱码、甚至把引号/花括号吃成语法错误（`read_doc.ps1`、`test_mcp_server.ps1`、
  调研脚本各中过一次）。写文件用
  `[System.IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding($true)))`。
- **MCP server 模式**：`QuickScriptTool.exe --mcp` 走 stdio MCP（每行一个 JSON-RPC 2.0），
  在 WebView2/单实例初始化**之前**分流（`src/mcp_server.cpp`；入口在 `qst_webview_shell.cpp` 的
  `wWinMain` 顶部）。暴露 12 个工具（screenshot/click/move/type_text/key/scroll/cursor_position/
  list_windows/activate_window/list_ui_controls/invoke_ui_control/read_document），全部复用既有原生链路。
  冒烟：`powershell -File tools\test_mcp_server.ps1`（真拉子进程走管道，exit 0 才算过）。
  改工具清单要同步该脚本与 `docs/ai-action-exec-optimization.md` §18。
- **思考开关策略**：默认**允许思考**（准确率优先），诊断日志打印 `thinking=允许`；
  「只想不调工具」由「上轮没调工具就催」兜底。需要旧的快速执行：环境变量 `QST_FAST_THINKING=1`。
  实现 `ShouldDisableThinking()`（`agent_core.cpp`，声明在 `agent_core.h`）。
- **computer-use 别名**：`computer` 工具（`MakeComputerTool`）是别名入口，映射到既有动作；
  其上下文守卫与 `mouseClick` 共用 `GuardPointerClickContext()`——**改守卫必须同时覆盖两者**，
  自检 `computer_alias_tool` 会断言这一点。

- **低性能模式**：设置 → 宏回放设置的勾选框，用户抱怨「跑脚本时电脑烫/风扇响」时的取舍开关。
  开关是**进程级原子量，只有一份**：`src/low_power_mode.h` 的 `SetLowPerformanceMode()` /
  `LowPerformanceMode()`（头文件内联，轻量自检目标也能用）。`LoadAppSettings`/`SaveAppSettings`
  各同步一次 → 保存后立即生效。生效点：`input_timeline_scheduler.cpp`（忙自旋 12ms→0.8ms）、
  `image_match.cpp` 入口的 `SyncImageMatchThreadBudget()`（OpenCV 线程预算 →1）、
  `engine_script_run.cpp` 的三个 playback guard（不提优先级/不绑核/不抬全系统定时器分辨率）、
  找图监视轮询下限（50→200ms）。**加新的降耗点必须读同一个 `LowPerformanceMode()`，不要另建开关**；
  对应用例：`ImageMatchSelfTest/low_power_limits_cv_threads`、`RecorderSelfTest/timeline_low_power_keeps_deadlines`、
  `AppSettingsStoreSelfTest/save_load_low_performance_mode`（用例结束必须把开关复位，否则污染同进程后续用例）。

- **找图 GPU 加速（OpenCL）**：设置 → 宏回放设置，默认**关**。开关本体在 `src/findimage_gpu.h`
  （同低性能模式的头文件内联原子量，避免设置层依赖 OpenCV）。生效点在
  `image_match_internal.h` 的 `MatchSingleScale` 非金字塔分支 → `TryMatchTemplateOnGpu()`。
  三条硬事实（实测，见 docs §19.4）：① 只 `setUseOpenCL(true)` 对 **Mat 输入无效甚至更慢**
  （OpenCV 只对 `UMat` 走 OpenCL），必须显式 UMat；② 面积 <500k 像素一律 CPU（480×360 打平）；
  ③ 低性能模式优先（`FindImageGpuAccelActive()`），GPU 抛异常就本进程永久回落 CPU。
  对应用例：`ImageMatchSelfTest/opencl_matchtemplate_bench`、`gpu_accel_same_result_and_area_gate`。

- **找图「上一帧命中」本地复核**：循环里同一请求反复找图时的快速路径（自动，无开关）。
  判据是 `image_match.h` 的纯函数 `PlanFindImageFastPath` / `AcceptFindImageFastPathHit`，
  调度/存状态在 `engine_script_run.cpp`（`g_findImageFastPath` + `ResetFindImageFastPath()`，
  **每次开始跑脚本清空**）。**改动这几条守卫前先读 docs §19.4 的表**：请求指纹、上次全屏耗时 ≥12ms、
  新鲜度 ≤3s、漂移 ≤8px、分数余量 ≥阈值+3、窗口必须包住漂移带、窗口 <搜索区 70%；
  命中失败必须**当场**回退全屏搜索（不能等下次重试，否则等于漏检）。
  对应用例：`ImageMatchSelfTest/find_image_fastpath_gate`。
  模板解码缓存（`image_match.cpp` 的 `TemplateImageCache*`，按 路径+大小+修改时间 失效）同理：
  返回的仍是新建 HBITMAP，句柄所有权语义不变。

- **游戏前台 = 视觉唯一手段**（用户实测踩过）：`AiActionUiTooBusyForVisionLocate()` 过去只看
  动态覆盖率，游戏画面逐帧重绘必然超标 → `locateAndClick` 被 100% 硬拦；加上指令里
  「禁止在页面内容上 locateAndClick」与 canvas 文案「进游戏后人手或脚本接」，
  模型只剩切窗/看图 → 表现就是「游戏没反应、一直在思考」。判据已按前台分流：
  dom/mixed 与 canvas 放行、非浏览器前台（游戏/自绘/模拟器）放行、表格软件与
  「浏览器+页型未知」仍拦（后者文案是「先 observePage 拿树」，不是「禁止」）。
  `AiActionGameForegroundLikely()` 命中时每轮注入「用视觉推进、本轮至少落一个动作」，
  `AiGameNudgeOnce()`（每任务一次，仿 `AiRouteNudgeOnce`）内联玩法要点。
  **改这几条先读 docs §20**；自检 `AiActionRouterSelfTest / game_foreground_vision_allowed`。

- **游戏效率三条**（用户实测「每步都要确认、中间隔好几秒、不懂机制」后落的）：
  ① **两步操作一次做完**：`locateAndClick(targets=["A","B"])`（2~6 个不同目标）→
  宿主 `onLocateMulti` 逐个识图定位并**立即连点**，中间不插观察/验收，只做一次 settle；
  失败即停（不猜着点）。宿主侧是把原单目标链路包成 `std::function` 再复用，
  **改守卫/缓存/日志只需改那一条**。
  ② **游戏前台不等界面稳定**：`AiActionGameForegroundLikely()` 命中时 settle 用
  80/350/150/900ms 的短节拍，且不提「建议刷新/重开」（对游戏是噪音，会诱发多观察一轮）。
  ③ **先懂机制再动手**：`skills/agent/game.md` §0.5 + `MacroActionGameSkill()` 要求抽象目标
  （「通关这关」）先用一轮确认胜负条件 / **冷却是全局还是每单位独立** / 操作机制 / 节奏，
  写进 `updateTaskMemo` 再动手。实例：PvZ「我是僵尸」冷却按卡牌各自算 → 应批量放僵尸。
  自检 `locate_multi_targets`。

- **位置只找一次：布局记忆 + 相对网格**（`src/ai_ui_layout.{h,cpp}`，通用能力，见 docs §20.7）：
  ① **布局记忆**：键 = 归一化目标描述 + 窗口身份（标题|类名|进程|**客户区尺寸**）+ 屏幕尺寸；
  定位成功一次即记住坐标，之后同键 0 识图。失效规则：换窗/挪窗/改分辨率 → 键不匹配；
  **点击无变化 → 立刻 `AiUiLayoutForget`**；**每次开始跑脚本 `AiUiLayoutClear()`**。
  ② **相对网格**：`locateAndClick(grid={anchor,cells})` —— 锚点只识图一次，整片格子坐标由
  `AiUiDetectGridPeriod()`（自位移平均绝对差 + 抛物线亚像素修正）推断；**周期不显著必须拒绝**
  （硬套网格会点到不相干的地方）。宿主 `onLocateGrid` 一个批次连点 ≤24 格。
  行业出处（实施时已对照）：Midscene caching / UiPath Object Repository（元素定位缓存）、
  Sikuli `Region.right()/below()`（相对定位）、OmniParser/Set-of-Mark（一次解析整屏 → 我们已有
  UIA/DOM 树 + `targets=[…]`）。自检 `ui_layout_memory_and_grid`、`ui_grid_period_detect`。

- **流式读取两条硬规则**（`agent_core.cpp`；用户实测「每轮都白付一次完整请求」）：
  ① **不许用短超时轮询读 body**（250ms/1500ms 都试过，thinking 模型「看图+想」的间隙有几秒，
  WinHTTP 的 RECEIVE_TIMEOUT 一触发句柄即废 → 后续报 12019）。现在的结构是
  **读给足超时 + 看门狗线程**：心跳/收束/组装超时/空流/整体超时/空闲超时全在看门狗里，
  要打断阻塞读就 `Abort()`（与停止热键同一条路径）；看门狗**只能读 `std::atomic` 投影**，
  不许碰读线程独占的 `StreamAccumState`（数据竞争）。
  ② 读循环出错时**先判断「已攒到可用内容」**（`ShouldFinalizeStream`）再决定 fail ——
  SSE 正常收尾时 `QueryDataAvailable` 也会报 12019，那不是故障。
  `Abort()` 关过句柄后 `AbortAwareRequestGuard` 不再重复关（`AiHttpAbortSlot::closed`）。

- **模型能力判定**（`agent_ai_actions.cpp`；用户报障「我选 deepseek-v4.1-flash 却跑成豆包」）：
  `ModelSupportsVision` 里 **DeepSeek v4/v5 与 vl/vision 都是多模态**，只有
  `chat`/`r1`/`reasoner`/`v3.x` 是纯文本 —— 旧规则「带 deepseek 且无 vl/vision 一律纯文本」
  会把用户选的 v4.1-flash 判成不能识图，然后**静默**换成列表里的识图模型并存进脚本。
  另外：**AI 动作执行不许在写库时改写动作模型**（`EnsureAiModelOnAction` /
  `ApplyResolvedAiModelToActionParams` 对 `aiActionExecute` 直接返回），识图需要由运行时
  `RunAiActionExecuteForAction` 按能力决定并打诊断；编辑器模型下拉默认选**设置里的当前模型**
  而不是列表第 1 个。自检：`model_supports_vision`、`action_model_not_silently_swapped`、
  `planner_observe_image_attach`。

- **规范化脚本**：生成/修改脚本必须走 `planScriptActions`（含循环/条件时）→ `buildScriptActions` + `createMacroScript`；
  `loop`/`if`/`else`/`defineBlock` 用 `children` 嵌套子动作（像写代码），禁止把循环体写成同级；
  `writeScript` 只接受规范化产物（逐动作重建校验 + 关键字段完整性校验，手写残缺 JSON 直接拒绝）。
- **脚本理解（两级泛化翻译，防长输入卡死）**：`readScript` 默认输出**动作概览**——
  每步只有动作名 + 类型级语义（按钮/跟随动作/循环方向/变量图/无限循环/修饰键/限定区域/
  带截图等）+ **完整备注**（备注必传、不截断，是意图关键），不含坐标/时长/阈值等数值，
  让 AI 先做模糊理解；需要具体参数分析时传 `detail=true`
  查详细说明（`DescribeScriptActionsDetail`，含图片/坐标/条件/间隔/修饰键/容差/AI 超时等），
  或 `raw=true` 看原始 JSON（单次 ≤32KB）。均按 `startIndex`/`maxActions` 分页（每页 ≤60 步）。
  脚本引用图片输出 `[[AGENT_IMG:路径]]` 标记：`AgentCore` 工具循环对多模态模型自动编码为
  图片消息（`keep_images` 防剥离），非多模态降级为路径提示，不报错。
  工具结果预算见 `AiToolResultBudgetChars`（`readScript` 16000 / `readAgentFile` 48000，
  勿盲目调大——长输入是卡死主因）。
- **模型自适应**：请求层按 `ModelIsReasoningType` 识别推理模型（o1/o3/o4、DeepSeek R1/Reasoner、
  Kimi K2 Thinking、Grok Reasoning），不发送 `temperature`；`thinking.disabled` 仅对
  火山/方舟/豆包网关发送；`max_tokens` 按模型 clamp；上下文滑动窗口保留最近 10 个 user 轮。
