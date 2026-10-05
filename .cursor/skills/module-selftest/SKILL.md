---
name: module-selftest
description: >-
  QuickScriptTool 模块自检总索引。改窗口模式/定时任务/宏变量/脚本构建/坐标/脚本IO/
  找图引擎/OCR 找字解析/AI 路由/设置库/主题弹窗布局/脱离冷却/停止热键/连点间隔，或用户报 找图/绑窗/定时不触发/宏条件错/Agent 写坏动作/
  主题窗裁切字号/按住键仍恢复宏/快捷键停止失灵/连点过短点不上时：读本 skill，选 suite，MSBuild Release 目标，跑 --json，exit 0 前不宣称修好。
  Prefer over guessing.
---

# Module SelfTest（Agent 总索引）

每模块一个独立 console exe。统一约定见下；专项逻辑见各 suite skill / reference。

## 一句话循环

```text
1) 选 suite  2) MSBuild /t:<Target> Release  3) 跑 exe --json
4) FAIL 的 name → 对照表改源码  5) 再编再跑直到 exit 0
```

**批量回归（推荐先跑这个）**：`tools\run_all_selftests.ps1` —— 构建 + 跑全部纯逻辑 suite，
按 exit code 判定，失败回显原始输出。CI（`.github/workflows/build.yml`）用的就是它，
所以本地与 CI 判定口径一致。

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1                 # 构建 + 跑全部逻辑 suite
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -SkipBuild      # 只跑不编
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -Tier full      # 加交互类（需桌面会话/驱动）
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -LogPath build\selftest.log
```

> 本机构建前必须 `unset HTTPS_PROXY HTTP_PROXY https_proxy http_proxy`，否则 MSBuild 抛
> `MSB6001 … 字典中的关键字:"https_proxy"`（脚本已内置该护栏）。详见 AGENTS.md「构建陷阱」。

## 硬性约定

| 项 | 约定 |
|----|------|
| CLI | `--json` / `--list` / `--help`（`WindowModeSelfTest` 另有 `--macro`） |
| stdout（`--json`） | UTF-8；每行 `{"name","ok","detail"}`；末行 `{"passed","failed","ok"}` |
| stdout（`--list`） | `name\twhen\tmeaning` 行 + 末行 `{"listed":N,"ok":true}` |
| exit | `0` = 全过；`N>0` = 失败数 |
| 产物 | `build\Release\<Target>.exe`（`RecorderSelfTest` 例外：`QstRecorderLogicTest.exe`，降低 360 HEUR 误删） |
| 宣称修好前 | 对应 suite `exit 0` |

构建模板（仓库根）：

```powershell
& "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:<Target> /m /v:minimal
# RecorderSelfTest 请跑：build\Release\QstRecorderLogicTest.exe --json
build\Release\<Target>.exe --json
```

PowerShell 不要用分号拼多个 `/t:A;B`（会拆成多条命令）；多个 target 请分开跑。
`RecorderSelfTest` 勿链 `script_core_common`（含 SendInput）；被 360 误删时先加信任区 `build\Release\`，再重编目标 `RecorderSelfTest`。

> ⚠ **主程序目标叫 `QstWebViewShell`，不叫 `QuickScriptTool`**（2026-09-20 重构后改名，产物仍是
> `QuickScriptTool.exe`）。`build/QuickScriptTool.vcxproj` 是 Sep 18 遗留的**孤儿文件**，
> CMake 已不再生成它，里面残留 16 个**已删除**的 GDI 旧 UI 源文件（`main.cpp` / `controls.cpp` /
> `settings_dialog.cpp` / `ui_component.cpp` / `popup_combo.cpp` / `dialog_*.cpp` …）。
> 去构建 `--target QuickScriptTool` 会得到**一整屏 `C1083 无法打开源文件`**，
> **那全是这个孤儿文件造成的假象，不是工作区坏了**。
> 正确：`cmake --build build --config Release --target QstWebViewShell`。
> 判据：`ls -la build/*.vcxproj` 看目标名；`grep -c ui_component build/QstWebViewShell.vcxproj` 应为 0。
>
> 那 16 个文件也**不在 git 里**（`git ls-files src/main.cpp` → unknown），`CMakeLists.txt` 里无引用
> ⇒ **别去 `git checkout` 恢复它们**（会把已废弃的 GDI UI 拉回来）。产品当前走 WebView2 壳
> （`QST_BUILD_WEBVIEW_SHELL=ON`），UI 在 `ui/*.js`。

### 宿主陷阱（AI 终端 / 受限宿主里跑 `run_all_selftests.ps1`）

| 症状 | 真因 | 处理 |
|------|------|------|
| `无法将参数绑定到参数"Path"，因为该参数是空值`，报错行号指向**调用方** | 宿主把命令拼成字符串执行 ⇒ 脚本内 `$PSScriptRoot` **为空**，`Split-Path -Parent $null` 抛错 | 脚本已加三级兜底（`$PSScriptRoot` → `$MyInvocation.MyCommand.Path` → CWD 含 `CMakeLists.txt`），并显式 `throw` 可读原因。**别再退回裸 `Split-Path -Parent $PSScriptRoot`** |
| 命令 `exit 0` 但**没有任何输出**、`-ListOnly` 抓不到东西 | 脚本末尾 `exit N` 会终结**整个包装会话**，重定向与后续 `Set-Content` 都不执行 | 要拿结果就用脚本自带的 `-LogPath`（它自己 `Add-Content` 落盘），或别用 `-ListOnly`；不要依赖 `& script *>&1 \| Set-Content` |
| `exit 1` 且**日志文件根本没生成**（偶发，7 次里 2 次） | 宿主把脚本异常吞掉了，磁盘上零痕迹 | 脚本已加 `trap`：任何未捕获异常写 stdout + `-LogPath` 并以 `exit 3` 结束。**再遇到先看日志里的「脚本异常终止：…」**，别直接当抖动重跑 |
| `禁止运行脚本`（PSSecurityException） | 每个宿主会话的执行策略独立 | 同一命令块内先 `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force` |
| 从 Bash 调 `powershell.exe -File` 被拦 | 安全策略禁止绕过 PowerShell 工具 | 用 PowerShell 工具，并在同块内设 `$env:PATHEXT`（见 AGENTS.md） |

偶发 `exit 1` 但日志文件根本没生成 ⇒ 先复跑 2~3 次再下结论（抖动，非回归）。

共享 Emit：`tools/selftest_harness.h`。CMake：`qst_add_selftest(...)`。

## 环境依赖断言 —— 假红的头号来源（写新用例前必读）

**判据：一条断言只要读了「真实桌面 / 真实输入状态 / 全局资源」，它就在赌运行环境。**
赌赢了看不出来，赌输了就是**假红** —— 假红的代价比漏测更高（会让人去改没坏的代码）。
本仓已为此吃过两次（⚠ 那两个用例本身**已随批 A~D 的撤销一起删除**，教训留下）：
曾读真实前台窗口的旧三档路由用例、曾直接抓实时屏幕的旧跟踪首帧用例。

### 三类，处理方式完全不同

| 类别 | 例子 | 正确做法 |
|---|---|---|
| ① **可钉住**（有测试接缝） | 设置/任务库路径、AI 抓帧来源、前台窗口判定 | **钉住**：用 `SetXxxForTest` 换掉来源。**不是**放宽判定，**不是**改 `SKIP`（`SKIP` 会让守门员在坏环境下**永远不生效** = 没断言） |
| ② **天生容忍**（只验契约，不验内容） | `vision_not_found_variants`（认得哪种「未找到」都行，只验判据本身） | 保留，但**注释写清为什么可以任意**，否则后人会「顺手收紧」把它变成假红 |
| ③ **必须真实环境** | 注入 / 假焦点 / HID 驱动 / 剪贴板 | 归入**交互档**，不进 CI；并把失败形态写进「假失败清单」 |

### 本仓已有的钉住手段（照抄这些形状，别自己发明）

- `SetAppSettingsFilePathForTest` / `SetScheduledTasksFilePathForTest` —— 全局存储重定向到
  `*.selftest.json`，**避免覆盖用户真数据**
- `SetAiForegroundBrowserOverrideForTest` / `SetAiSpreadsheetForegroundOverrideForTest` —— 钉死前台判定轴
- `SetAiBrowserLaunchTargetOverrideForTest` —— 钉死「按显示名启动程序」解析出的目标
- `SetNowMsForTest` —— 时钟

### 三个反模式（见到就要改）

1. **「命中 ≥1 即过」这类弱条件** —— 系统性 bug 与偶然环境差异混在一起，分不清是修好了还是碰巧过了。
   要么**全部要过**，要么**把环境钉住**。（已删的旧跟踪首帧用例原来就是 ≥1，实测长期只过 2/3。）
2. **改 `SKIP` 了事** —— 环境不对就 SKIP 看似稳妥，实则让守门员在**最需要它的环境**里静默失效。
   只在「模块/模型/驱动缺失」这类**真前提**下才 SKIP。
3. **靠「等一会儿画面就变了」触发分支** —— 要用**几何/构造**确定性地踩。
   例：`ui_reaction_locality` 直接构造变化区矩形（`ScreenChangeRoi` 列表）+ 落点坐标，
   与屏幕内容无关，不靠等、不靠真实截图。

### 审计现状（2026-09-21 全量扫过 `tools/*selftest*.cpp`）

- CI 逻辑档**已无未钉住的环境依赖**。写盘全部带 `__`/`selftest` 前缀，或走 `SetXxxFilePathForTest`。
- `image_match` 的整屏自匹配用的是 `FindTemplateInFrozenScreenMulti`（**同一张位图**当冻结屏）⇒ 内容无关。
- `Sleep(...)` 只出现在交互档与轮询等待里，**没有**紧界时序断言。
- ⚠ 已知残留（低概率、暂不修）：`agent_assistant` 的剪贴板用例读**真实全局剪贴板**。
  它已保存并恢复原文，但若运行期间别的进程占着剪贴板，`OpenClipboard` 会失败 ⇒ 可能假红。
  这类是③类，只能靠「跑之前别开剪贴板管理器」规避。

## 新增一条「纯逻辑判据」suite 的套路（本仓标准形状，照抄别发明）

适用：要钉死的是一条**判定规则**（不是某条业务流程）。已按此形状落了 4 条：
`hotkey_stop.h` / `ime_hotkey_pass.h` / `desktop_tools/float_ball_geom.h` / `engine/hotkey_scope.h`。

1. **判据抽成纯逻辑头**：`inline` 函数、只吃标量或简单结构体，**不碰 HWND / 全局状态 / 真实时间**。
   零依赖 ⇒ 自检目标可以**不链任何库**（`qst_add_selftest(XxxSelfTest tools/xxx_selftest.cpp)`，
   不写 `LIBS`），编得快，也**不会被 `qst_engine` 的编译问题连坐**（补证据时优先靠它）。
2. **产品必须真的调它** —— 最关键的一步。判据留在原处、测试再抄一份 ⇒ **测的是副本 = 没测**。
   做法是把原实现改成委托（例：`EngineHost::DedicatedHotkeyInScope` → `DedicatedInScope`；
   `syncOne` 的 `outOfScope` → `!AllowSystemRegister(...)`）。改完 `grep` 确认调用点真的接上了。
3. **真值表逐行手写期望**，**不要**在测试里重算被测表达式（那是自证，永远绿）。
   例：`hotkey_scope_selftest.cpp` 的 `kScopeTable[16]` / `kRegTable[4]`，`expect` 全是手写的。
4. **同步四处清单**：`CMakeLists.txt` 的 `qst_add_selftest`、`tools/run_all_selftests.ps1` 的
   `$LogicSuites`、`.github/workflows/build.yml` 的 `--target`、`AGENTS.md` 的目标/档位计数；
   再加本文件「Suite 索引」一行 + 一个小节表。
5. **必须做 A/B 反向验证**：把判据临时改成**历史 bug 的形态**，确认用例**真的变红**，再改回确认全绿。
   没做这一步的断言视为橡皮图章。（本轮实例：`AllowSystemRegister` 改恒 `true` ⇒ 3 红；恢复 ⇒ 12 绿。）
6. **要真实桌面才成立的判据别塞进纯逻辑档**。另做一个**探针 exe**（`tools/float_ball_probe.cpp`，
   与产品共用同一个判据头）跑实机、打印判定与新旧口径对照，**不进任何清单**。

## Suite 索引

| id | Target / exe | 何时用 | Skill / reference | 核心源码 |
|----|--------------|--------|-------------------|----------|
| `window` | `WindowModeSelfTest` | 绑窗/后台输入/启动参数/IME/找图挂窗口模式；**AI 切窗台账**（`window_list_*` / `window_activate_foreground`）；**每拍输入快速路径**（`soft_input_fast_path`：改 `PrepareSoftInputFast` / `CanUseSoftInputFastPathClass` / 热路径调用点时必跑） | [window-mode-debug](../window-mode-debug/SKILL.md) · [reference](../window-mode-debug/reference.md) | `src/window_mode/**`（切窗台账：`window_list.cpp`） |
| `scheduled` | `ScheduledTaskSelfTest` | 定时不触发、Tick、weekDays、Agent 定时 CRUD | [scheduled-task-debug](../scheduled-task-debug/SKILL.md) · [reference](../scheduled-task-debug/reference.md) | `scheduled_task_*.cpp`, `agent_tools.cpp` |
| `macro_variables` | `MacroVariablesSelfTest` | `{var}` 条件、找图时限、转义、loop/goto | 下表 FAIL→文件 | `src/macro_variables.cpp` |
| `script_action_builder` | `ScriptActionBuilderSelfTest` | Agent 写宏坏、endLoop、缺 stopMacro、type 拒识 | 下表 FAIL→文件 | `src/script_action_builder.cpp`, `action_tree.h` |
| `coord_space` | `CoordSpaceSelfTest` | 换分辨率、coordMeta、n* 归一化、找图 scale | 下表 | `src/coord_space.cpp` |
| `script_io` | `ScriptIoSelfTest` | 脚本存读丢字段、录制路径归类、坏 JSON | 下表 | `src/script_io.cpp` |
| `script_package` | `ScriptPackageSelfTest` | 导出/导入脚本包：嵌套脚本（RunMacro/MousePlayback）没被打包、同一脚本多个 runMacro 只改了一处、循环引用、包内子目录解不出来 | 下表 | `src/script_package.h/.cpp`, `webview_bridge_backend.cpp`（`ExportScriptFile`/`ImportScriptFile`）, `utils.cpp`（`ExtractZipFile`） |
| `image_match` | `ImageMatchSelfTest` | 阈值清零、NMS、金字塔/分数量化、冷冻位图找模板 | 下表 | `src/image_match*.cpp` / `image_match_internal.h` |
| `ocr` | `OcrSelfTest` | OCR JSON 解析、找字（子串/全半角/邻行/模糊）、变量打包 | 下表 | `src/ocr_engine.cpp` |
| `ai_action_router` | `AiActionRouterSelfTest` | Vision/Click/Tool/多轮分类、坐标映射、点击 JSON、元素索引/文字直点、本地判断表留痕 | 下表 | `src/ai_action_router.cpp` + `src/ai_decide.cpp` + `src/ai_locate_verify.cpp` |
| `agent_assistant` | `AgentAssistantSelfTest` | 对话编辑截断、撤销日志、白名单命令、文件工具、Skill 文件加载、`runDesktopTask` 已注册且在**写临时脚本之前**拦危险目标、**`listScripts` 不列出助手临时任务脚本**（用户可见行为，A/B 验证过） | 下表 | `src/agent_undo.cpp`, `src/agent_shell.cpp`, `src/agent_core.cpp`, `src/agent_tools.cpp`, `src/utils.cpp`, `tools/agent_assistant_selftest.cpp` |
| `agent_desktop_task` | `AgentDesktopTaskSelfTest` | AI 助手「动手」层**纯逻辑**：`runDesktopTask` 参数解析/夹紧、危险目标确认闸（中英分治 + 词边界）、动作构造、结果清洗剥控制标记、中断标志、**临时脚本名判据 + 启动清扫**（真建文件） | 下表 | `src/agent_desktop_task.h/.cpp`, `src/utils.cpp`（前缀/判据/清扫） |
| `agent_mcp` | `AgentMcpSelfTest` | MCP **客户端**：JSON-RPC 行构造/解析（裸换行、别人的 id、非 JSON 日志行）、`tools/list` 解析、`mcp__<server>__<tool>` 命名往返、配置读写、**图片落盘 + `[[AGENT_IMG:…]]` 标记**、`CreateProcess` 可执行性判据、**附图目录按年龄清理**、**子进程孤儿防护（Job Object）**、**单通道串行**；`macro` 类 5 条**真起子进程**端到端（本 exe 自带 `--fake-mcp-server`） | 下表 | `src/agent_mcp.h/.cpp` |
| `ooxml` | `OoxmlSelfTest` | **OOXML 层**（自研，不依赖 Office/第三方库）：RFC 1951 解压（stored / 固定 / **动态** Huffman）、截断与长度不符必须失败、zip 读（deflate+stored 混用）、★★**跨 part 字节保留**（未改动条目的原始压缩字节逐字节相同）、method/crc 原样保留、Zip64 明确拒绝、**xlsx 读写**（表名解析 / sharedString 富文本 run / 数字·布尔·公式 / TSV 导出 / ★★**外科手术式写单元格**：保留样式属性、同 part 内 `<mergeCells>`/`<cols>`/行高一字不动、按列序插入、`<sheetData/>` 自闭合要先展开 / XML 转义） | 下表 | `src/ooxml/inflate.h/.cpp`, `src/ooxml/zip_archive.h/.cpp`, `src/ooxml/xlsx_doc.h/.cpp` |
| `app_settings_store` | `AppSettingsStoreSelfTest` | 设置存读、theme/preview clamp、坏文件 | 下表 | `src/app_settings_store.cpp` |
| `theme_ui` | `ThemeUiSelfTest` | 自定义主题/取色弹窗裁切、字号、随机色可用性 | 下表 | `src/theme_ui_layout.h`, `src/app_theme.cpp` |
| `breakout_cooldown` | `BreakoutCooldownSelfTest` | 默认模式脱离：按住不计冷却、松开后 idle、新输入重置 | 下表 | `src/breakout_cooldown.h`, `src/breakout_input.h` |
| `hotkey_stop` | `HotkeyStopSelfTest` | 运行中停止热键被吞/失灵、宏回放停不下来、键鼠假死、中文输入法仍触发热键、专属热键跨 TAB 吞键（按 P 打不出 P） | 下表 | `src/hotkey_stop.h`, `src/ime_hotkey_pass.h`, `src/engine/engine_hotkeys.cpp` |
| `float_ball_geom` | `FloatBallGeomSelfTest` | 悬浮球全屏隐藏判据：非全屏页面（最大化窗口 / 任务栏自动隐藏 / UWP 宿主）不得隐藏 | 下表 | `src/desktop_tools/float_ball_geom.h`, `src/desktop_tools/float_ball.cpp` |
| `overlay_input_guard` | `OverlayInputGuardSelfTest` | **输入接管兜底**：叠层/悬浮球/拖动准星扣住鼠标捕获后「抬起丢了」必须能自己收尾（本地还能按 Esc，**远控下几乎没有别的入口** ⇒ 只在远控被报上来）；钉「兜底通道真能超时醒来」 | 下表 | `src/overlay_input_guard.h`；应用点 `src/screenshot_overlay.cpp`、`src/match_overlay.cpp`、`src/ocr_overlay.cpp`、`src/drag_pick_overlay.cpp`、`src/desktop_tools/{desktop_tools,float_ball}.cpp`、`src/engine/engine_host_window.h` |
| `hotkey_scope` | `HotkeyScopeSelfTest` | 专属热键作用域：作用域外必须**彻底释放**（不启动 + 不吞键），切 TAB 后「按 P 打不出 P」 | 下表 | `src/engine/hotkey_scope.h`, `src/engine/engine_hotkeys.cpp` |
| `clicker_timing` | `ClickerTimingSelfTest` | 连点自定义 0.001s 截成 0ms、按下抬起粘连、间隔毫秒量化 | 下表 | `src/clicker_timing.h`, `src/engine/engine_record_click.cpp` |
| `time_scale` | `TimeScaleSelfTest`（纯逻辑）<br>`WindowModeSelfTest` 的 `window_time_scale_iat`（端到端，交互档）<br>`WindowModeSelfTest` 的 `window_time_scale_only_iat`（仅变速注入，交互档）<br>`InjectionSelfTest` 的 `fakefocus_timescale_only`（**跨进程真实路径**，交互档） | 窗口变速（变速齿轮）：虚拟时钟整数运算、换倍率不倒流、长跑不溢出；**改 `fake_focus_time_scale.h` 的 IAT 补丁 / `InstallCommon` / `InstallTimeScaleOnly` / 共享内存倍率下发 / `PreferHardwareInput` / 注入器导出选择时必跑** | 下表 | `src/window_mode/time_scale_clock.h`, `src/window_mode/fake_focus/fake_focus_time_scale.h`, `src/window_mode/fake_focus/fake_focus_dll.cpp` |
| `recorder` | `RecorderSelfTest`（产物 `QstRecorderLogicTest.exe`） | 录制排序/转换/时间轴/调度器；**回放偏移、高轮询率鼠标时间轴被拉伸、报告周期** | [recorder-precision](../recorder-precision/SKILL.md) · [docs/recorder-precision.md](../../../docs/recorder-precision.md) | `src/recorder*.cpp`, `src/recorder_report_interval.h`, `src/input_timeline_scheduler.cpp` |
| `virtual_hid` | `VirtualHidSelfTest` | VirtualHid 键/相对/绝对/滚轮注入；进程被杀须抬起（驱动 FileCleanup） | 下表 | `src/input/virtual_hid.*`, `input_emergency_teardown.*`, `driver/qst_vhid/` |
| `injection` | `InjectionSelfTest` | 注入对抗测试：7 种注入技术 + PEB 隐藏 + XOR；`--inject <pid> <dll> <technique>` 驱动真实目标测试 | 见 `docs/anticheat-injection-testing.md` | `src/window_mode/injection/**`, `tools/injection_selftest.cpp`, `tools/injection_test_*.cpp` |
| ↳ ⚠ **`fakefocus_injector_technique` 是既有稳定失败**（2026-09-21 复核仍是），**不是回归**。报 `线程劫持调用未完成（… resumeRet=1 rip=0x0 started=0x0 …）`。**⚠ `resumeRet=1` 是正常的**（`ResumeThread` 返回**之前的**挂起计数：线程原在跑 0→挂起 1→返回 1→减回 0，线程确实恢复了），**别按它推故障**。**隔离法**：`QST_HIJACK_NOOP=1 ./InjectionSelfTest.exe --json` —— 它让 stub 只调 `GetCurrentProcessId`，**该用例会通过** ⇒ 证明**线程劫持机制本身正常**，失败点在「调用真实 `DllMain`」那一步（任意工作线程上跑 DllMain 的 Loader Lock 风险；`VirtualProtectEx` 返回值未检查）。 | | |
| `bridge_json` | `BridgeJsonSelfTest` | **改桥接 JSON / `json_util.h` / `saveSettings` 全链路时必跑**：JS 消息形状 ↔ C++ 解析严格度、取子对象、解析失败诊断 | 见下 | `src/json_util.h`, `tools/bridge_json_selftest.cpp` |
| `script_runner` | `ScriptRunnerSelfTest` | **改引擎→壳调用、`engine_ui_hooks.*`、或执行核心时必跑**：UI 钩子契约（默认）+ headless 跑 Wait/Loop/Goto/VarCompute（`--engine`） | 见下 | `src/engine/engine_ui_hooks.*`, `src/engine/engine_script_run.cpp` |
| `bridge_contract` | `BridgeContractSelfTest` | **改桥接命令（加/改/删 `type`）时必跑**：命令表 ↔ C++ 入站分派 ↔ `ui/*.js` 发送侧 双向校验 | 见下 | `src/webview/bridge_commands.h`, `tools/bridge_contract_selftest.cpp` |

### 仍偏手工（无 exe）

连点注入观感 / Agent 对话·附件·热键观感、以及「拖拽闪烁」等时序观感 — 见 `docs/comprehensive-test-cases.md`。OCR 解析与找字已由 `OcrSelfTest` 覆盖；安装/实机正确率仍偏手工。停止热键策略已由 `HotkeyStopSelfTest` 覆盖；布局裁切/字号已由 `ThemeUiSelfTest` 覆盖。连点间隔/按下抬起换算已由 `ClickerTimingSelfTest` 覆盖。

## FAIL → 源码

### MacroVariablesSelfTest

| name | 优先查看 |
|------|----------|
| `resolve_match_var_brace` | `ResolveMacroVariables` / `LookupMatchVarProperty` |
| `resolve_cur_loops` | `ResolveMacroVariables` (`ctrl:CurLoops()`) |
| `decode_quick_input_escapes` | `DecodeQuickInputEscapes` |
| `resolve_quick_input_var_escapes` | `ResolveQuickInputText`（未勾选丢掉 `{var}` 中的换行/Tab） |
| `find_image_time_sec` | `ResolveFindImageTimeSec` |
| `condition_compare_and_or` | `EvaluateConditionExpr` / `ParseConditionParts` |
| `goto_step_from_literal` | `TryResolveGotoStepNo` |
| `loop_max_from_var` | `ResolveLoopMaxCount` |
| `unknown_var_no_recurse` | `ResolveMacroOperandImpl`（未知标识须返回空，禁止 `{t}` 递归） |
| `resolve_image_var_path` | `MacroVariableContext.imageVars` / `ResolveMacroOperandImpl` |
| `build_quick_input_image_var` | `BuildQuickInputVarItems`（findImage followUp=3） |
| `build_quick_input_fixed_vars` | `BuildQuickInputVarItems`（下拉仅 CurLoops/Random/Hour/Minute/Clipboard；`{Now}` 等魔法变量不进列表） |
| `resolve_ctrl_random` | `ResolveMacroVariables` / `ResolveMacroOperandImpl`（`ctrl:Random()`） |
| `resolve_ctrl_hour_minute` | `ResolveMacroOperandImpl`（`ctrl:Hour()` / `ctrl:Minute()`） |
| `resolve_ctrl_clipboard` | `ResolveMacroOperandImpl` / `ClipboardExpandMode`（`ctrl:Clipboard()`） |
| `user_var_beats_magic_name` | `ResolveMacroOperandImpl`（用户变量优先于无前缀魔法名） |
| `var_compute_return_exports` | `RunVarCompute`（`return` 导出；未 return 局部丢弃） |
| `var_compute_clipboard_string` | `ResolveClipboardVarCompute`（文本或文件路径，不是 0/1） |
| `var_compute_user_var_roundtrip` | `userVars` + `RunVarCompute` + 条件读取 |
| `var_compute_optional_semi_newline` | `RunVarCompute` 换行/下一语句可省略 `;`，return 仍导出 |
| `var_compute_string_compare_sign` | `RunVarCompute`（OCR 符号 `'+'`/`"+"` 比较；裸写 `+` 报运算符） |
| `var_compute_split_string` | `RunVarCompute`（`split` / `[i]` / `.count` / `toInt`） |
| `var_compute_ocr_paren_percent` | `RunVarCompute`（OCR「127723(74.27%)」→ `split(hp,"(")[1]`；空串/变量缺失时的失败语义） |
| `var_compute_ocr_numbers_replace` | `RunVarCompute`（`numbers()` / `replace()`；`count` 兜底 + 数值比较 vs 字典序） |
| `collect_varcompute_return_names` | `CollectVarComputeReturnNames` / `BuildQuickInputVarItems`（return 导出名进变量下拉） |
| `resolve_match_list_index` | `ParseIndexedVarRef` / `LookupMatchListVar`（`matchRet[0].x` / `count` / `[n]` 为空） |
| `build_quick_input_multimatch` | `BuildQuickInputVarItems`（MultiMatch 注册 `[n]` / `[0].x` / `count`） |

#### 变量运算（`src/var_compute.cpp`）三条硬规则

现场事故：用户「血量检测」脚本 OCR 得到 `127723(74.27%)` 存到 `hp`，下一步
`split(hp,"(")[1]` 报「变量运算失败：下标越界」——**表达式没错，是数据**。

1. **裸标识符取不到 ⇒ 数字 `0`（不是空串）。** 于是 `split(hp,"(")` 得到 `["0"]`，
   取 `[1]` 必越界，而报错完全不提变量，极容易把排查带偏到表达式上。
   现在：`AsIndex` 报错带长度 + **转义后**的源文本；`FromContext` 对「未定义/为空」记去重警告
   （`VarComputeResult.warnings` → 引擎 `AppendDebugLog`）。
   **排「下标越界」先看这两条日志，别先改表达式。**
2. **`split(...)[i]` 是字符串 ⇒ 与数字比较走字典序。** `a[0] <= 60` 里 `"9.5" > "60"`（漏判）、
   `"100" < "60"`（满血误触发）。要数值比较必须 `toInt/toDouble`，或直接用 `numbers()`（返回数字数组）。
   ⚠ 这是**既有语义**，不要顺手改成「混合类型自动数值比较」（`-`/`*` 对字符串是**报错**不是强转，
   改成 `missing → ""` 会把 `result <= 60` 整体变成字典序比较，反而更糟）。用例里的
   `numCmp` / `strCmp` 两条断言把这对语义钉住了。
3. **OCR 文本优先用 `numbers()`，不要按字符 split。** 识别引擎换版本后括号/百分号可能是全角
   （U+FF08（ / U+FF05％）—— **在调试日志里和半角长得一模一样**，`split(hp,"(")` 直接拆不开。
   `FormatOcrDebug` 打印的是原文、不转义，所以只能靠变量运算的报错来分辨。推荐写法：

   ```
   nums = numbers(hp)
   result = 100
   if (nums.count >= 2) { result = nums[-1] }
   return result
   ```

   阈值写 `>= 2` 的理由：变量缺失时 `hp` 是数字 `0` ⇒ `numbers(0)` = `[0]`，count 只有 1；
   写 `>= 1` 会把假数据 `0` 当成读数，于是 `result = 0` ⇒ 每轮都误按回血键。

### ScriptActionBuilderSelfTest

| name | 优先查看 |
|------|----------|
| `build_wait_ok` | `BuildScriptActionFromJson` |
| `reject_custom_text` | `BuildScriptActionFromJson`（禁 customText） |
| `normalize_renumber` | `NormalizeScriptActionList` |
| `ensure_stop_macro_appends` | `EnsureStopMacroOnActions` |
| `ensure_stop_macro_skip_infinite` | `EnsureStopMacroOnActions` / `HasTopLevelInfiniteLoop` |
| `endloop_needs_parent` | `ValidateEndLoopPlacements` (`action_tree.h`) |
| `endloop_inside_loop_ok` | 同上 |
| `build_array_json` | `BuildScriptActionsJsonArray` |
| `flatten_children_loop_body` | `FlattenNestedActionParamList` / `BuildScriptActionsJsonArray` |
| `empty_loop_rejected` | `ValidateContainerBodies` (`action_tree.h`) |
| `flatten_if_else_inside_loop` | 同上（if/else 为 loop 子节点，分支再嵌套） |
| `plan_outline_shows_tree` | `PlanScriptActionsOutline` |
| `non_container_children_rejected` | `FlattenNestedActionParamList` |
| `fragment_loop_skips_tree_check` | `BuildScriptActionsJsonArray(..., validateContainerBodies=false)` |
| `indent_siblings_form_loop_body` | `ValidateContainerBodies`（indent=父级+1） |
| `build_move_mouse_relative` | `BuildScriptActionFromJson` (`moveMouseRelative`) |
| `inter_repeat_interval_*` | `ShouldWaitAfterRepeat` / `ActionUsesInterRepeatInterval`（`action_utils`；含 runMacro/runBlock；mouseDrag 否） |
| `build_mousedrag_abs` | `BuildScriptActionFromJson` mouseDrag 绝对坐标；duration 为拖拽时长 |
| `build_mousedrag_image_locate_requires_path` | mouseDrag `imageLocate=1` 缺 `imagePath` 失败 |
| `lookup_mousedrag` | `LookupMacroActionSchema`（imageLocate/endX；脚注拖拽时长） |
| `build_color_image_locate_requires_path` | getColor/colorMatch/findColor `imageLocate=1` 缺 `imagePath` 失败 |
| `build_findcolor_followup_clamped` | findColor `followUp=saveImage` 钳到 2 |
| `lookup_getcolor` | `LookupMacroActionSchema`（getColor 含 imageLocate） |
| `build_runblock_repeat_fields` | `BuildScriptActionFromJson` runBlock/runMacro 重复字段 |
| `build_mouseplayback_speed` | `BuildScriptActionFromJson` mousePlayback `playbackSpeed` |
| `disassemble_*` | `DisassembleActionAt`（`action_disassemble.h`） |
| `merge_*` | `MergeSelectedIntoContainer`（`action_assemble.h`） |
| `flatten_watchimage_children` | `FlattenNestedActionParamList`（watchImage children） |
| `flatten_watchimage_nested_rejected` | `FlattenNestedActionParamList`（watchImage 禁止写在 loop/if children 里） |
| `lookup_watchimage_resume` | `LookupMacroActionSchema`（watchImage 续行含 resumeAfterWatch / watchMode） |
| `build_watchimage_time_mode` | `BuildScriptActionFromJson` `watchMode=time` + `watchPollSeconds` |
| `build_varcompute_code` | `BuildScriptActionFromJson` `varCompute` / `computeCode` |
| `build_findimage_save_image` | `ParseFollowUpValue` / FindImage `saveImage`→3；有模板保留 `findTimeExpr` |
| `build_multimatch_ok` | `BuildTypedAction` MultiMatch：缺 `imagePaths` 失败；mode/duration/followUp 钳 2；保留 `findTimeExpr` |
| `build_multimatch_image_use_var` | MultiMatch 每槽 `imageUseVars` 不被强制关掉 |
| `build_multimatch_hole_use_vars` | `imagePaths` 中间空槽不得把后面的 `imageUseVars` 错位 |
| `lookup_multimatch` | `LookupMacroActionSchema(L"multiMatch")` |
| `build_quickinput_parse_escapes` | `BuildScriptActionFromJson` QuickInput `parseEscapes` |
| `resolve_key_nav_names` | `ResolveKeyVk`（Home/Up/Right/Delete/`←` 等须映射真 VK，禁止首字母或 U+2190 回落） |
| `reject_unknown_keytext` | `ResolveKeyVk` / `BuildScriptActionFromJson`（未知多字符键名须拒绝） |

### 重复间隔语义（脚本动作，非连点器）

`mouseClick` / `keyClick` / `hotkeyShortcut` / `quickInput` / `scrollWheel` / `mousePlayback` / `runMacro` / `runBlock`：

- `clickCount` = 重复次数
- `duration` / `randomDuration` = **相邻两次之间**的间隔
- `clickCount=1`：完全不等待；不在第一次之前、最后一次之后插入等待
- 与 `wait` 动作的 `duration`（整段阻塞等待）不同；`mouseDrag.duration` 是按下到松开的拖拽时长，不是重复间隔；`quickInput.charInterval` 是字间间隔

Agent / `buildScriptActions` schema、`agent_reference`、工具描述须与此一致。

### CoordSpaceSelfTest

| name | 优先查看 |
|------|----------|
| `standard_meta_*` / `save_meta_*` / `exec_meta_*` | `StandardScriptCoordMeta` / `BuildScriptCoordMetaForSave` / `ScriptCoordMetaForExecution` |
| `coordmeta_json_*` / `has_coordmeta_*` | `WriteCoordMetaJson` / `ParseCoordMetaJson` / `HasCoordMetaJson`（`space`/`ref*` 在 `coordMeta` 对象内，须先取出该对象再 Extract） |
| `normalize_move_*` / `migrate_*` | `NormalizeActionCoords` / `MigrateLegacyScriptToNormalized` |
| `normalize_relative_skip` | `NormalizeActionCoords`（相对移动不得归一化） |
| `normalize_mousedrag_end_nstar` | `NormalizeActionCoords`（mouseDrag `endX/endY` → `nEndX/nEndY`；`imageLocate` 按模板尺寸） |
| `getcolor_imagelocate_xy_roundtrip` | `SyncMouseDragNorm` / `DenormMouseDragPixels`（getColor 找图定位 x/y 相对模板） |
| `var_image_offset_norm_from_producer` | `SyncNormFieldsFromPixels`（变量图 offset 跟前序「保存图片」搜索区/相对区，不跟屏幕） |
| `template_scale_*` / `exec_find_opts_*` | `ComputeTemplateScale` / `BuildExecutionFindImageOptions` |
| `resolve_click_point_*` | `ResolveFindImageClickPoint`（含窗口模式命中框映射后偏移跟框缩放） |

### ScriptPackageSelfTest

脚本包（导出/导入）依赖收集。**改 `ExportScriptFile` / `ImportScriptFile` / `script_package.*` /
`ExtractZipFile` 后必跑。**

| name | 优先查看 |
|------|----------|
| `collect_only_nested_types` | `CollectNestedRefsFromJson`：只认 `runMacro`/`mousePlayback`；`runProgram`/`openFile`/`activateWindow` 的 `targetPath` 不是脚本，收了就会把 exe/网址/窗口名当脚本去找 |
| `collect_keeps_targetpath_and_blockname` | `NestedRef` 两个字段都要留：`targetPath` 是**源机器绝对路径**（换机器必失效），`blockName` 是显示名（换机器反而解析得到） |
| `rewrite_all_nested_targetpaths` | `RewriteNestedTargetPaths` 必须**倒序逐个块替换**。用「只替换第一处」的写法时，一个脚本里多个 `runMacro` 只有第一个被改写 |
| `rewrite_skips_non_nested` | 同上：同一值出现在 `runProgram` 里时不得被改（按块判 `type`，不能全局 `find/replace`） |
| `rewrite_case_insensitive` | Windows 路径大小写不敏感，匹配要 `towlower` |
| `sanitize_entry_file_name` | `SanitizeEntryFileName`：去路径分隔符 / `<>:"|?*` / 结尾点与空格 |
| `plan_includes_nested_script` | `BuildPackagePlan`：嵌套条目名 `scripts\<名>.json`，`portableRef` 是**裸文件名** |
| `plan_cycle_safe` | A→B→A 循环引用不能死循环（访问集按**全路径小写**） |
| `plan_collects_nested_images` | 嵌套脚本自己引用的图片也要进 `images`（旧实现只看根脚本） |
| `plan_reports_missing_ref` | 解析不到的引用进 `missingRefs` 并提示用户；**不要**因为一个坏引用就让整个导出失败 |
| `zip_subdir_roundtrip` | `ExtractZipFile` 必须自建中间目录 —— `CreateFileW` 不建，缺了会把整条 `scripts\x.json` 静默丢弃 |
| `manifest_roundtrip` | `package.json` 的 `entry`/`ref`/`name` 三字段往返 |
| `portable_ref_resolves_on_target` | 改写后的裸文件名在目标机器上必须能被 `ResolveLibraryScriptPath` 解析到（靠 `EnumerateScriptJsonFiles` 递归） |
| `scan_dependency_matrix` | **全量依赖矩阵**：44 种动作 + 5 个条件分支逐格比对「体检判定」vs「引擎实际依赖」。加新动作 / 加图片字段后**必须同步这张表**，否则立刻变红 |
| `scan_ocr_region_by_image_needs_opencv` | `textRecognition + ocrRegionByImage` 必须算「需要找图」—— 漏了就不打包 OpenCV，执行时 `0xC06D007E` 当场死 |
| `scan_ocr_region_by_image_needs_opencv` | `textRecognition + ocrRegionByImage` 必须算「需要找图」—— 漏了就不打包 OpenCV，执行时 `0xC06D007E` 当场死 |
| `plan_multilevel_recursion` | 根→A→B→C **四份全收** —— "只收一层"的实现会在这里变红 |
| `plan_rewrites_refs_inside_children` | **子脚本里**指向孙脚本的引用也要被改写成裸文件名（只改根脚本 = 孙脚本在目标机找不到） |
| `plan_blockname_only_ref` | `targetPath` 空、只有 `blockName` 时也收（blockName 是库内脚本文件名） |
| `plan_unique_names_on_collision` | 两个不同脚本同名时条目名自动去重（`x.json` / `x-2.json`） |
| `manifest_player_section` | `BuildPackageManifestJson(plan, player)` / `ParsePlayerManifest`：**旧包没有 player 段必须返回 false**，不能崩或给脏值 |
| `scan_self_contained` | `CapabilityScan::IsSelfContainedOnly`：纯输入脚本应判定为"零外部依赖" |
| `scan_image_ocr_ai` | `ActionNeedsOpenCv` 的边界：`getColor`/`mouseDrag` **只在 `imageLocate=1` 时**才算找图；`findColor`/`colorMatch` 恒算 |
| `scan_includes_nested` | `ScanPackageCapabilities` 必须覆盖嵌套脚本（否则导出对话框会漏报依赖） |
| `payload_append_read_roundtrip` | 尾标 32 字节 = magic + offset + size + hash；断言 `offset+size+32 == 文件大小`，否则尾标写错位 |
| `payload_reject_plain_file` | 普通文件（哪怕末尾有 `QSTPKG0` 前缀）不得被当成有 payload —— magic 必须全 8 字节比对 |
| `payload_dump_bytes_equal` | dump 出的 payload 与原始 zip **逐字节相同**（偏移算错会在这里露出来） |

**播放器（导出 EXE 运行端）相关硬规则**（`src/player/player_main.cpp`，2026-09-19 实测踩过）：

- **不要用裸 `CreateDirectoryW`** —— 它只建最后一级，父级不存在时整条失败。
  用 `EnsureDirectoryTree()`（`src/utils.h`）。`%LOCALAPPDATA%\QstPlayer\rt\<hash>` 就栽在这上面。
- **路径必须归一化后再比较**（`NormalizePath` + `SameDir`）：`LOCALAPPDATA` 里可能出现 `//` 或尾斜杠，
  而 `GetModuleFileNameW` 返回规范形式，直接 `_wcsicmp` 会判定"不是同一个目录" →
  **无限迁移循环 / 进程自繁殖**。另加 `--qst-no-relocate` 做硬断路。
- **播放器绝不能把自己注册成浏览器扩展的原生消息宿主**：引擎的 `StartExtBridgeAlwaysOn()`
  会用 `ExePath()` 写清单，播放器必须先 `windowmode::SetExtNativeHostAllowed(false)`，
  并在 `wWinMain` 最前面检测**管道 stdio / `--ext-native-host`** 立刻返回。
  否则 Edge 会以 `QstPlayer.exe chrome-extension://<id>/ --parent-window=0` 反复拉起它
  —— 实测 45 个实例同时存活。
- **不调 `qst::engine::Shutdown()`**：headless 窗 `WM_DESTROY` 走 `TerminateProcess`。
- 排障入口是 `<runtimeDir>\player.log`（启动早期落在 `%TEMP%\QstPlayer-boot.log`）。
- **payload 里的 `rt\` 前缀不能直接解到 `scripts\`**：注入器按 `ModuleDirectory()` 找
  `FakeFocus32/64.dll`、OpenCV 也从 exe 旁边加载。必须"解到 `_stage` → 按前缀分发
  （`rt\` → 运行时根，其余 → `scripts\`）→ 删暂存"。
- **`LoadLibrary` 加载刚释放出来的 60+ MB DLL 会撞 `ERROR_SHARING_VIOLATION(32)`**
  （杀软在扫新写入的文件）。一次性 LoadLibrary 会让"首次运行"必失败 → 必须退避重试。
- **跑完自退的判据只能是 `IsRunning()` 转 false**。默认模式的「中断脱离」期间
  `breakoutPaused_=true` 但 `running_` **仍为 true**，等待循环天然继续等 ——
  别去判断 `breakoutPaused_` 决定是否退出，那会打断"脱离后恢复执行"。
- **脚本要以 `stopMacro` 收尾才会结束**：引擎 worker 是
  `while (!StopRequested()) { runRange(...) }`。这是产品既有语义，播放器不改它
  （同一脚本在软件里和 exe 里行为必须一致）。
- **控制热键必须在 `engine::Start()` 之前读出并抹掉**：① 引擎 `Start()` 会扫描脚本目录，
  把脚本热键注册成**回放钩子**（`ghPlaybackScriptHooks`），运行中按它走**紧急停止**分支，
  抢在播放器的单击/长按判定之前停掉脚本；② 引擎加载脚本时会把 `script.json` 规范化重写，
  那一步把 `hotkeyVk` 清成 0 —— 读晚了什么也读不到。
- **`CreateProcessW` 拉起刚复制出来的 exe 会撞杀软占用**（32/5），与 `LoadLibrary` 同一类问题，
  必须退避重试。
- **`PlayAppStartupSound()` 按 `AppDir()` 找 wav**，所以提示音必须随 payload 的 `rt\` 分发。
- **AI 密钥不能走默认的 `SaveAppSettings`**（用户级 DPAPI，换机器解不开）——
  用 `SerializeAppSettings(settings, /*portableSecrets=*/true)` 写明文。
- **导出的 exe 要"效果与软件内一致"，靠的是整份设置快照**：
  导出侧把 `SerializeAppSettings(cur, true)` 写成 payload 的 `rt\app_settings.json`，
  播放器释放到运行时目录根 ⇒ 引擎按 `AppDir()\app_settings.json` 读。
  ⚠ **别另写"便携版"序列化器** —— 漏字段 = 静默行为差异。
- **OCR「走软件」模式必须 Python 优先**：`Auto` 的语义是"复用已装软件里的引擎"，
  反过来先试系统 OCR 会让它永远用不上软件里的引擎 → "软件里好、exe 里不行"。
- **WinRT OCR 对中文逐字加空格**：`OcrLine.Text()` 返回 `新 建 任 务`，
  下游文本匹配/`numbers()`/找字全乱 → 用 `JoinOcrWords()` 自己拼。
- **OCR 类脚本排查第一步**：看 `player.log` 里的 `OCR[...] 成功/失败，读到 …` 那行
  （由 `SetOcrDiagnosticSink()` 写入）—— 能直接看出识别到没有、识别成什么。
- **提示音是 `SND_ASYNC`**：进程立刻退出会把声音截断（实测结束音只响一半）。
  退出前用 `WavDurationMs()` 读时长再 Sleep。
- **`stopMacro` 与「回放次数」共同约束脚本**（worker 里是两个独立的 break 条件，谁先命中谁生效）。
  单测覆盖不到 —— 用 `python tools\verify\player_behavior_matrix.py` 跑四象限矩阵
  （A 有stopMacro+次数关=1轮 / B 有stopMacro+次数5=**仍 1 轮** / C 无+次数3=3轮 / D 无+次数关=无限）。
- **AI 图片分析 / 动作执行也要 OpenCV**：发截图给模型走 `BitmapToBase64Jpeg` → `cv::imencode`，
  **无条件**需要。没 OpenCV 时它 `return {}` —— **不崩，但 AI 拿不到图、动作必然失败**。
  这种"不崩但静默失效"比崩溃更难查，所以体检一样要算。`aiTextAnalysis` 是纯文本，不用算。
- ⚠⚠ **`0xC06D007E` = delay-load 找不到 DLL**。exe 用 `/DELAYLOAD:opencv_world4100.dll` 链 OpenCV，
  缺 DLL 时只要代码碰到 OpenCV 符号（**包括 `cv::Mat` 的构造/析构**）就会抛异常把进程带走 ——
  症状是"没日志、没结束音、什么都没发生"。守卫必须放在**任何 `cv::Mat` 之前**。
  排查手法：绕过启动器直接跑 `<runtimeDir>\QstPlayer.exe --qst-no-relocate` 看退出码。
- ⚠⚠ **`0xC06D007E` = delay-load 找不到 DLL**。exe 用 `/DELAYLOAD:opencv_world4100.dll` 链 OpenCV，
  缺 DLL 时只要代码碰到 OpenCV 符号（**包括 `cv::Mat` 的构造/析构**）就会抛异常把进程带走 ——
  症状是"没日志、没结束音、什么都没发生"。守卫必须放在**任何 `cv::Mat` 之前**。
  排查手法：绕过启动器直接跑 `<runtimeDir>\QstPlayer.exe --qst-no-relocate` 看退出码。
- **嵌套宏里不要放 `stopMacro`**：`StopMacroShouldEndEntireRun()` 恒真 ⇒ 置全局 `stopFlag_`，
  **不管嵌套多深都会把父脚本一起结束**（软件内同样如此，不是导出差异）。
- 导出侧的嵌套宏链路单测覆盖不到"发到别人电脑能不能跑"，
  用 `python tools\verify\nested_macro_export.py`（三层嵌套 + blockName 引用 + 图片）。
- **控制热键必须在 `engine::Start()` 之前读出并抹掉**：① 引擎 `Start()` 会扫描脚本目录，
  把脚本热键注册成**回放钩子**（`ghPlaybackScriptHooks`），运行中按它走**紧急停止**分支，
  抢在播放器的单击/长按判定之前停掉脚本；② 引擎加载脚本时会把 `script.json` 规范化重写，
  那一步把 `hotkeyVk` 清成 0 —— 读晚了什么也读不到。
- **`CreateProcessW` 拉起刚复制出来的 exe 会撞杀软占用**（32/5），与 `LoadLibrary` 同一类问题，
  必须退避重试。
- **`PlayAppStartupSound()` 按 `AppDir()` 找 wav**，所以提示音必须随 payload 的 `rt\` 分发。
- **AI 密钥不能走 `SaveAppSettings`**（用户级 DPAPI，换机器解不开）——
  用 `BuildPortableAiSettingsJson` 写明文。
- **payload 里的 `rt\` 前缀不能直接解到 `scripts\`**：注入器按 `ModuleDirectory()` 找
  `FakeFocus32/64.dll`、OpenCV 也从 exe 旁边加载。必须"解到 `_stage` → 按前缀分发
  （`rt\` → 运行时根，其余 → `scripts\`）→ 删暂存"。
- **`LoadLibrary` 加载刚释放出来的 60+ MB DLL 会撞 `ERROR_SHARING_VIOLATION(32)`**
  （杀软在扫新写入的文件）。一次性 LoadLibrary 会让"首次运行"必失败 → 必须退避重试。
- **跑完自退的判据只能是 `IsRunning()` 转 false**。默认模式的「中断脱离」期间
  `breakoutPaused_=true` 但 `running_` **仍为 true**，等待循环天然继续等 ——
  别去判断 `breakoutPaused_` 决定是否退出，那会打断"脱离后恢复执行"。
- **脚本要以 `stopMacro` 收尾才会结束**：引擎 worker 是
  `while (!StopRequested()) { runRange(...) }`。这是产品既有语义，播放器不改它
  （同一脚本在软件里和 exe 里行为必须一致）。

**设计要点（别改回去）**：
- 导出时 `targetPath` 改写为**裸文件名**，导入时再改写为**还原后的真实文件名** —— 两端都要改，
  只改一端等于把绝对路径带到别人机器上。
- 图片映射要**统一收集后应用到所有脚本**（含嵌套），不能只改根脚本。
- 旧包（无 `package.json`）必须继续能导入：清单缺失时走原路径。

### ScriptIoSelfTest

| name | 优先查看 |
|------|----------|
| `recording_path_*` | `IsRecordingScriptPath` / `RecordingsDir`（含正斜杠规范化） |
| `breakout_*` | `NormalizeBreakoutTimeSeconds` / `EffectiveBreakoutTimeSeconds` |
| `parse_action_*` | `ParseScriptActionBlock` |
| `parse_move_mouse_relative_*` / `write_move_mouse_relative_*` | `ParseScriptActionBlock` / `WriteActionJson` |
| `save_load_*` / `load_*` / `parse_truncated_*` | `SaveScriptFileData` / `LoadScriptFileData` / `ParseScriptContent` |
| `write_action_json_*` | `ScriptActionToJsonString` |
| `mouse_playback_speed_roundtrip` | `ParseScriptActionBlock` / `WriteActionJson` `playbackSpeed` |
| `write_norm_xy_keeps_pixel` | `WriteActionJson`（n* 须 max_digits10，禁默认 precision=6） |
| `write_norm_xy_keeps_pixel` | `WriteActionJson`（n* 须 max_digits10，禁默认 precision=6） |
| `parse_findimage_save_image` | `findImageFollowUp=3` / `imageUseVar` 读写；有模板保留 `findTimeExpr` |
| `parse_multimatch_roundtrip` | `multiMatch` `imagePaths` / mode / sort；`followUp=2` 保留 `findTimeExpr` |
| `parse_multimatch_template_t_path` | `imagePaths` 含 `template_` 不得把 `\t` 吃成 `imagestemplate_` |
| `parse_multimatch_image_use_vars` | `imageUseVars` 与变量图槽位 roundtrip |
| `parse_multimatch_hole_use_vars` | `imagePaths` 中间空槽不得把后面的 `imageUseVars` 错位 |
| `resolve_imagestemplate_typo` | 旧 `\t` 吃路径后 `imagestemplate_` 应找回 `template_` |
| `parse_watchimage_resume` | `watchImage` / `resumeAfterWatch` 读写 |
| `parse_watchimage_time_mode` | `watchImage` `watchMode` / `watchPollSeconds` 读写 |
| `parse_mousedrag_abs` | `mouseDrag` `endX/endY`/`duration`/`imageLocate=0` 读写 |
| `parse_mousedrag_image_locate` | `mouseDrag` 找图定位 `imagePath` + 相对偏移读写 |
| `parse_color_actions_image_locate` | getColor/colorMatch/findColor `imageLocate` + `imagePath` 读写 |
| `parse_findcolor_keeps_find_time` | findColor `followUp=2` 保留 `findTimeExpr`；`followUp=3` 钳到 2；findImage `followUp=2` 同样保留 |
| `save_load_color_image_locate` | Save/Load 找图定位按模板归一化；zip 收集图路径 |
| `load_imagelocate_norm_xy_not_pixels` | getColor `imageLocate` `nx>1.5` 不得当像素重解析 |
| `parse_varcompute_code` | `varCompute` / `computeCode` 读写 |
| `parse_quickinput_escapes` | `ParseScriptActionBlock` / `WriteActionJson` `parseEscapes` |
| `looks_like_file_path` | `LooksLikeFilePath` (`image_var_util`) |
| `window_relative_pixel_xy` | `ParseScriptActionBlock` `windowRelative` 像素路径 |
| `recording_keeps_window_relative_mode` | `SaveScriptFileData`：窗口相对录制不得清 `windowMode` |
| `recording_recovers_wm_from_rel_actions` | `SaveScriptFileData`：录制路径 `windowRelative` 动作 + 身份即使 `enabled=0` 也要复活 |
| `script_default_mode_not_revived` | `SaveScriptFileData`/`LoadScriptFileData`：`scripts` 路径明确 `enabled=0` 不得复活窗口模式 |
| `recording_wipes_editor_window_mode` | `SaveScriptFileData`：普通录制仍强制关残留 `windowMode` |
| `save_after_load_false_keeps_xy` | `SaveScriptFileData`：`Load(..., false)` 后改 wait 再存不得冲掉 n* |
| `find_json_by_filename_nested` | `FindScriptJsonByFileName` 递归子目录 |
| `resolve_library_stale_root_path` | `ResolveLibraryScriptPath`：根路径失效后按文件名找回子文件夹 |
| `resolve_library_outside_rejected` | `ResolveLibraryScriptPath` 拒绝目录外文件 |
| `retarget_nested_runmacro_path` | `RetargetNestedLibraryScriptPaths` 改写 `runMacro` `targetPath` |
| `resolve_library_chinese_folder` | `ResolveLibraryScriptPath` 中文子文件夹：旧根路径 + 真实路径 |
| `save_omits_visual_layout` / `load_legacy_visual_layout_ok` / `load_missing_visual_layout_ok` | `visualLayout` **不再写进脚本文件**；旧脚本带/不带该字段都要能正常载入动作 |
| `visual_layout_cache_roundtrip` | 画布缓存落在 `AppDir()\cache\visual`，按脚本 key 删除 |
| `parse_arrow_keytext_vk` | `ParseScriptActionBlock`：`←` / `keyVk=8592` → `VK_LEFT` |

### ImageMatchSelfTest

| name | 优先查看 |
|------|----------|
| `normalize_match_*` | `NormalizeMatchVarResult` |
| `match_center_*` / `click_point_*` | `FindImageMatchCenter` / `FindImageClickPoint` / `FindImageRelativeClickOffset` |
| `var_offset_anchor_screen_center` | `SynthesizeSearchRectCenterMatch`（选偏移：变量图锚框画在屏幕中心） |
| `pyramid_*` / `score_*` / `threshold01_*` | `image_match_internal.h` |
| `nms_*` | `GlobalNms` |
| `findpeaks_sqdiff_keeps_two_minima` | `FindPeaks` / `SuppressPeak`（SQDIFF 抑制填值） |
| `frozen_bitmap_*` | `FindTemplateInFrozenScreenMulti` |
| `similar_buttons_only_true_match` / `unrelated_scene_rejected` | `MatchInGrayMatsMultiVerify` 像素容差验收（禁 NCC 裸兜底） |
| `repeated_template_finds_all` | 定位峰抑制过松会只剩 1 框；`kLocatePeakMaxOverlap` |
| `offset_pick_max_matches_one` | 选偏移点 `maxMatches=1` 只留最佳一处 |
| `nearest_match_offset_not_first` | `FindNearestImageMatch`：相对偏移不得永远用第一框 |
| `alpha_mask_ignores_transparent` | PNG alpha mask（透明像素不得当黑边） |
| `alpha_padded_near_edge` | 透明边伸出搜索区时不得误拒贴边不透明内容 |
| `perfect_match_*` | `MatchPerfectPixel`（完美匹配像素终审） |

### AppSettingsStoreSelfTest（导出用的设置快照）

| name | 优先查看 |
|------|----------|
| `portable_settings_plaintext_key` | `SerializeAppSettings(s, true)`：`apiKey` **必须是明文**（不能出现 `dpapi:`），否则换机器解不开 |
| `portable_settings_roundtrip` | 写盘 → `SetAppSettingsFilePathForTest` → `LoadAppSettings`，`apiKey` / `savedModels` 原样读回 |
| `portable_settings_playback_roundtrip` | **逐字段**锁住回放次数/间隔区间/倍速/低性能/AI加速/找图GPU/窗口模式四开关+注入技术 —— 漏一个字段就是静默行为差异 |
| `portable_settings_escapes` | 密钥里的引号 / 反斜杠 / **换行**必须被转义（原始换行会截断 JSON） |

### OcrSelfTest

| name | 优先查看 |
|------|----------|
| `parse_success_lines` / `parse_unicode_escape` / `parse_failure_error` | `ParseOcrEngineJson` / `ParseOcrJson` |
| `find_exact_substring` / `find_fullwidth_digits` / `find_ignores_spaces` | `FindTextInOcrLines` / `NormalizeOcrSearchText` |
| `find_adjacent_lines` | `FindTextInOcrLines` 邻行拼接 |
| `find_fuzzy_close` | `FindTextInOcrLines` Levenshtein |
| `find_empty_or_miss` | 空目标 / 无关文本不得命中 |
| `concat_lines_newline` | `ConcatOcrLines` |
| `make_ocr_vars` | `MakeOcrTextVarResult` / `MakeOcrSearchVarResult` |

| `backend_default_is_python` | `OcrBackendPreference()` 默认必须是 `Python` —— 产品内 OCR 行为不能被播放器那套改动影响 |
| `backend_preference_roundtrip` | `SetOcrBackendPreference` 往返；**越界枚举值必须回落 Python**，不能是随机行为 |
| `winrt_ocr_probe` | `WinRtOcrAvailable()` 探测不崩；不可用时 `reason` 必须可读（语言包缺失要说清） |
| `winrt_ocr_reads_rendered_text` | **真实功能验证**：GDI 渲染 `1234` → `RunWinRtOcr` 认出数字。无语言包的机器上按"跳过"计（仍算通过），避免 CI 因环境差异变红 |
| `backend_attempt_order` | `Auto` 必须 **Python 优先**（走软件模式要和软件内结果一致）；`Python`/`WinRt` 各自单跑 |
| `join_ocr_words_cjk_no_spaces` | WinRT 逐字给 word，直接拼会成「新 建 任 务」→ 必须自己按词拼 |
| `join_ocr_words_latin_keeps_spaces` | 拉丁词之间保留空格；有一侧是 CJK 就不加 |

| `backend_default_is_python` | `OcrBackendPreference()` 默认必须是 `Python` —— 产品内 OCR 行为不能被播放器那套改动影响 |
| `backend_preference_roundtrip` | `SetOcrBackendPreference` 往返；**越界枚举值必须回落 Python**，不能是随机行为 |
| `winrt_ocr_probe` | `WinRtOcrAvailable()` 探测不崩；不可用时 `reason` 必须可读（语言包缺失要说清） |
| `winrt_ocr_reads_rendered_text` | **真实功能验证**：GDI 渲染 `1234` → `RunWinRtOcr` 认出数字。无语言包的机器上按"跳过"计（仍算通过），避免 CI 因环境差异变红 |

### AiActionRouterSelfTest

| name | 优先查看 |
|------|----------|
| `route_*` | `ClassifyAiActionRoute` / `IsAiAction*Prompt` / `HasActionVerb` |
| `route_composite_click` | 「单击」须归 CompositeClick（勿落识图问答） |
| `need_screen_scroll_yes` | `AiActionPromptLikelyNeedsScreenCapture`（滑动/滚轮/拖拽/移鼠要首帧截图） |
| `click_intent_*` | `PromptIntendsDoubleClick` / `PromptIntendsRightClick` |
| `composite_target_phrase_stripped` | `ExtractClickTargetPhrase`（剥动作前缀、截逗号，喂 `locateAndClick`） |
| `parse_bbox_leading_number` / `parse_bbox_parens` | `TryParseBoundingBox`（优先解析方括号/圆括号数字组） |
| `vision_not_found_variants` | `IsVisionLocateNotFound` |
| `usage_skill_*` | `MacroActionUsageSkill` / `LookupMacroActionSchema(usage)` |
| `agent_skill_*` | `MacroActionAgentSkill` / `LookupMacroActionSchema(agent)` |
| `recipe_reuse_guards` | `MakeRunActionRecipeTool`：模板复用前的提示一次、`confirmShortcut` 软闸、表格软件的路线指针（⚠ ⏸ 推迟族：这套「试跑前 N 组」预检属破坏性保护邻域，**别顺手删**） |
| `logic_convert_collapse_instance_data` | `CompileAiLogicConvert` / `CollapseInstanceDataEntries`（动态列表勿写死） |
| `logic_convert_per_locate_timers` | `CompileAiLogicConvert`（每找图独立 timer+if，禁整段共用计时器） |
| `logic_convert_compile_gate` | `CompileAiLogicConvert`（限时 findImage + 步间 Wait） |
| `reply_actions_buildable` | `BuildScriptActionsJsonArray`（quickInput+keyClick Enter） |
| `parse_coord_*` | `TryParseCoordinatePair` |
| `map_api_point_*` | `MapApiPointToScreen` |
| `build_click_json_*` | `BuildScreenClickActionsJson` |
| `vision_system_prompt_*` | `BuildAiActionVisionQuerySystemPrompt` |
| `route_label_*` | `AiActionRouteLabel` |
| `resolve_action_vision_fallback` | `ResolveActionAiModelName`（指定文本模型+需识图 → 自动换识图模型） |
| `resolve_vision_subtask_model` | `ResolveVisionSubtaskModelName`（非多模态主模型 → savedModels 识图模型） |
| `vision_route_model_routed` | `ModelSupportsVision` + `ResolveVisionSubtaskModelName`（带图路由判定） |
| `model_supports_vision` | `ModelSupportsVision`（多模态/文本模型名判定） |
| `adaptive_refine_gate` | `ShouldAcceptCoarseLocateWithoutRefine`（紧凑跳过二级 / 过小过大不跳） |
| `planner_observe_image_attach` | `ShouldAttachObserveImageToPlanner`（纯文本规划不附观察图） |
| `model_supports_vision` / `resolve_vision_subtask_*` | `ModelSupportsVision` / `ResolveVisionSubtaskModelName` |
| `page_snapshot_format_and_ref` | `FormatPageSnapshotForAgent` 可视/屏外覆盖；列表第1项=重复卡片最上最左 |
| `click_ref_navigates_space_href` | 身份卡 href 才直接打开；列表内容卡点元素 |
| `web_browse_allows_vision_fallback` | 扩展是优化层：树上没有或未装扩展时允许 `locateAndClick` 识图兜底 |
| `vision_gate_trace_only` | ★**批 D 后的反转守门员**：视觉闸**只留痕不拦不劝**。它同时钉住「判决照记 + 信号齐全（可离线复算）」与「动作路径不被它拦」两半 —— 只验前半会漏掉「拦在别处」的回归。⚠ 它**依赖真实前台窗口**（`ForegroundWindowLooksSelfDrawn()` 读真实前台的子窗口数，无覆盖钩子）⇒ 断言必须用会话状态把判决钉住（`AiNotePageKind` 一类），否则就是环境依赖的假红 |

### AppSettingsStoreSelfTest

| name | 优先查看 |
|------|----------|
| `default_*` / `settings_path_*` | `DefaultAppSettings` / `AppSettingsFilePath` |
| `load_missing_*` / `load_garbage_*` | `LoadAppSettings` |
| `save_load_*` / `theme_id_*` / `custom_theme_*` / `wm_preview_*` / `save_load_playback_speed` / `playback_speed_scale_math` / `save_load_home_ui_mode` / `save_load_other_os_flags` / `save_load_float_ball` / `float_ball_geom_dock_expand` / `save_load_ui_scale_factor` / `save_load_editor_default_view` / `save_load_editor_visual_flags` / `save_load_editor_action_catalog` / `save_load_editor_var_filters` | `SaveAppSettings` / clamp in load / `ScalePlaybackTimeSeconds` / `RecordingPlaybackTimeScale` / `playSoundOnEnd` / `NormalizeUiScaleFactor` / `NormalizeEditorDefaultView` / `other.editorActionOrder` / `other.editorHiddenActions` / `other.editorCatalogPreset` / `other.editorSearchAllActions`（缺 key/junk→`all` / 搜索默认关） / `editorHideFixedVars` / `editorHideCoordVars` / `editorMultiResultPlaceholderOnly`（默认关） / `other.showFloatBall` / `src/desktop_tools/float_ball_geom.h` |
| `save_load_scheduled_conflict_policy` | `scheduledTaskConflictPolicy` 存读 + clamp 0..1；`scheduledTaskAutoResume` 存读 |
| `try_load_missing_leaves_out` | `TryLoadAppSettings` 缺文件失败且不改 out |
| `load_omits_auto_hide_keeps_default` | `LoadOtherSettings` 缺 `autoHideMainWindow` 保持默认 true；助手窗/部分保存不得把「运行后自动隐藏主窗口」写成 false |
| `home_runtime_save_preserves_playback` | `SaveAppSettingsPreserveUserSettings`：home 选中叠上去，playback/ai/uiMode 不被引擎过期内存覆盖 |
| `startup_wav_*` / `finish_wav_path_sidecar` | `IsPlayableWavFile` / `AppFinishSoundFilePath`。启动/结束音缺文件或 PlaySound 失败均回退 `MessageBeep(MB_OK)` |
| `path_is_under_root` / `webview_userdata_*` / `webview_fetchdata_*` | `PathIsUnderRoot` / `ResolveWebView2UserDataDir`：Program Files 漫游 LocalAppData，便携目录旁路 |

### AgentAssistantSelfTest

| name | 优先查看 |
|------|----------|
| `truncate_history_*` | `AgentCore::TruncateHistoryToUserRound` |
| `undo_*` / `file_tools_*` | `src/agent_undo.cpp` |
| `shell_reject_*` / `shell_where_*` | `src/agent_shell.cpp` 白名单校验 |
| `skill_optimize_directs_to_tools` | `.cursor/skills/agent-optimize/SKILL.md` |
| `skill_script_tree_and_plan` | `.cursor/skills/agent-script/SKILL.md` / `MakePlanScriptActionsTool` |
| `write_script_rejects_empty_loop` | `ValidateContainerBodies`（writeScript 不得绕过空循环） |
| `recopt_*` | `src/recording_optimize_ops.cpp` 按关键动作分段合并/压缩（与产品对话框共用） |
| `clipboard_roundtrip` | `copyAgentTextToClipboard` / `pasteAgentClipboardText` |
| `conversation_draft_roundtrip` | `SaveAgentConversationDraft`（无轮次草稿不进列表） |
| `empty_conversation_not_listed` | 空会话关窗不得写入 `index.json` |
| `conversation_with_round_is_listed` | 有用户轮次仍进列表（防误伤） |
| `conversation_title_rejects_api_error` | `IsUsableConversationTitle` / 加载时回退首条用户消息 |
| `user_facing_tool_reply_strips_outline` | `AgentUserFacingToolReply`（终态回复去动作一览） |
| `outline_header_has_no_user_constraints` | `FormatScriptActionsOutline` 标题不得含对用户约束 |
| `skill_reply_forbids_dumping_constraints` | `kSkillReply` / `AgentSkillGet(reply)` |
| `desktop_task_tool_wired` | `MakeRunDesktopTaskTool` 注册 + **危险目标必须在写临时脚本之前**被拦（`DesktopTaskNeedsConfirm`） |
| `list_scripts_hides_desktop_task_temp` | `ListScriptsInDir` 的 `IsAgentTaskTempFileName` 过滤（详见 `utils.h`）。⚠ 摘掉过滤 ⇒ 本用例转红 |
| `write_spreadsheet_create_and_set` | `MakeWriteSpreadsheetTool`（`src/agent_shell.cpp`）+ `src/ooxml/xlsx_doc.*`。真落盘真读回：create（TSV→文件、默认拒绝覆盖、前导 0 保文本）、setCells（其它 part 字节不变、有备份）、扩展名与可写目录守卫 |
| `read_document_native_xlsx` | `ReadOfficeDocument`（`src/office_doc.cpp`）的 xlsx 原生快路径：`engine=ooxml-native`、不起外部进程 |
| `browser_history_read_and_format` | `MakeReadBrowserHistoryTool`（`src/macro_execute_tools.cpp`）+ `src/sqlite/browser_history.*`。★真落盘一个 **Chromium 形状**的历史库 → 真读回 → 钉住「按时间倒序 / 完整标题无省略号 / 制表符表头」。夹具由**官方 sqlite3** 生成（`build/_tmp/mk_browser_history_fixture.py`），时间戳用**真实量级**（`~1.3e16 > 2^53`）⇒ 专门防「int64 转 double 比大小」那个**静默错序** bug 复发 |
| `sqlite_*`（`SqliteSelfTest`，7 例） | `src/sqlite/sqlite_read.*` 手写只读 SQLite 解析器。⚠ 单元指针是**页内绝对偏移**（第 1 页页头在 `raw+100` 但指针仍从 raw 算起）；serial type 要**按字节边界**循环（varint 可占 2 字节）；`rowLimit==0` 是**不限**。夹具由官方 sqlite3 生成 ⇒「看起来对 ≠ 真对」 |
| `shell_genoffice_whitelist` | `src/agent_shell.cpp` genoffice 子命令白名单（`mcp`/`install-cli` 等必须拒绝） |
| `skill_catalog_lists_new_sections` | `AgentSkillCatalog()` 与内嵌兜底文本（`desktop` 一节） |

### AgentDesktopTaskSelfTest

| name | 优先查看 |
|------|----------|
| `parse_*` | `ParseDesktopTaskOptions`（夹紧范围：超时 10~600、步数默认 12 / 上限 60） |
| `danger_verbs_flagged` / `benign_goals_not_flagged` | `DesktopTaskNeedsConfirm` 的中文子串表 |
| `negation_suppresses_false_positive` / `partial_limiter_does_not_suppress` | `kNegations` 只收**整句级**否定（混进「只填/only fill」会让闸被问废） |
| `confirm_prompt_demands_explicit_consent` | `DesktopTaskConfirmPrompt`（必须要求先问用户 + `confirmed=true`） |
| `action_carries_result_var` / `script_is_single_ai_action` | `BuildDesktopTaskAction` / `BuildDesktopTaskScript`（恰好一条 `AiActionExecute`） |
| `temp_script_in_scripts_dir` / `temp_script_paths_unique` | `DesktopTaskTempScriptPath`（必须落 `ScriptsDir()`，引擎只认那里） |
| `temp_script_name_predicate` | `IsAgentTaskTempFileName`（`src/utils.cpp`）。**判严不判松**：判松会吞掉用户脚本 |
| `sweep_stale_temp_scripts` | `SweepStaleAgentTaskTempScripts`（只删「创建它的进程已不在」的；真建文件测） |
| `summarize_*` | `SummarizeDesktopTaskResult`（剥回放控制标记 / 压平 / 截断 / 空结果显式） |
| `cancel_flag_roundtrip` | `RequestDesktopTaskCancel` / `ClearDesktopTaskCancel` |

### AgentMcpSelfTest

| name | 优先查看 |
|------|----------|
| `build_request_line_*` / `build_notification_has_no_id` | `McpBuildRequestLine`（每行一个 JSON-RPC、无裸换行；通知不带 id） |
| `parse_result_matched_id` / `parse_ignores_*` | `McpParseResultLine`（别人的 id 与日志行必须忽略） |
| `parse_error_response_reported` | JSON-RPC `error` 必须如实上报，别当成功 |
| `qualified_name_roundtrip` / `split_rejects_foreign_name` | `McpSplitQualifiedToolName`（`mcp__<server>__<tool>`） |
| `extract_tool_text_*` | `McpExtractToolResult` 的文本拼接与非文本占位说明 |
| `extract_tool_image_persists` | 图片 base64 落盘 + `[[AGENT_IMG:…]]` 标记（**办公验收的唯一通路**） |
| `extract_tool_unknown_image_mime_is_honest` | 宿主认不出的 mime **不许**假装能看图，要留一句说清格式 |
| `extract_tool_error_is_error` | `isError=true` ⇒ 返回 false 但 `out.text` 仍填好 |
| `config_*` / `genoffice_config_shape` | `mcp_servers.json` 读写（**文件缺失不算错误**、坏 JSON 才算） |
| `runnable_image_by_extension` | `McpCommandIsRunnable`：`CreateProcessW` 只能起 `.exe/.cmd/.bat` |
| `sweep_stale_mcp_images` | `SweepStaleMcpImagesIn`。⚠ 去掉 `now > 时间戳` 守卫 ⇒ 本用例转红（`futureKept=0`） |
| `orphan_guard_active_after_start` | `OrphanGuardActive`（**必须用 `IsProcessInJob` 查子进程**，只看句柄非空是假绿）+ `Start()` 里的 `AssignProcessToJobObject` |
| `kill_on_close_job_kills_child` | `CreateKillOnCloseJob`：关 Job 句柄必须**立刻**杀掉子进程（`before=258` 仍活 / `after=0` 已死） |
| `concurrent_call_tool_is_serialized` | `Impl::callMu`（`Initialize`/`ListTools`/`CallTool` 三入口）。⚠ 注释掉锁 ⇒ 本用例转红（`bDurMs=0`） |
| `e2e_*` | 真起子进程走 `--fake-mcp-server`。⚠ **用例里不许用产品默认的 180s 超时**，否则一个 bug 能把套件挂满 3 分钟且拿不到失败信息 |

### OoxmlSelfTest

| name | 优先查看 |
|------|----------|
| `inflate_stored_block` | `InflateRaw` 的 BTYPE=00 分支（LEN/NLEN 校验） |
| `inflate_fixed_huffman` | `BuildFixedTables`（RFC 1951 §3.2.6 的码长分配） |
| `inflate_dynamic_huffman` | ★`BuildHuffman` + 码长表的 `kCodeLenOrder`（**不是** 0..18 的顺序，写错必炸） |
| `inflate_rejects_truncated` | 流提前结束必须返回 false（不许崩、不许悄悄少给字节） |
| `inflate_rejects_length_mismatch` | `expectedSize` 校验 —— 把「解压器有 bug」和「文件本来就坏」分开 |
| `zip_reads_deflate_entries` / `zip_reads_stored_entry` | `ZipArchive::LoadFromMemory` + `GetData`（method 0/8 都要能读） |
| `zip_byte_preserving_save` | ★★`ZipArchive::SaveToMemory` 里的 `if (e.modified)`。⚠ 改成无条件重编码 ⇒ 本用例与下一条一起转红 |
| `zip_preserves_method_and_crc` | 未改动条目的 method/crc/compSize 必须原样（夹具里 `keep.xml` 是 **deflate(8)**） |
| `zip_roundtrip_added_entry` | `AddEntry` + 重新解析 |
| `zip_rejects_zip64` | Zip64 必须明确拒绝（把 EOCD 的 totalEntries 打成 0xFFFF 来构造） |
| `xlsx_lists_sheets` | `LoadFromMemory` 的 workbook.xml + rels 解析（rId 映射 + 约定回退） |
| `xlsx_reads_shared_string` | `GetCell` 的 `t="s"` 分支 + `CollectTextRuns`（**富文本多个 run 要拼起来**） |
| `xlsx_reads_number_bool_formula` | `t` 属性分派（缺 `t` = 数字 / `b` / `<f>`）；**不存在的格子 = 空，不是错误** |
| `xlsx_dump_tsv` | `DumpSheetAsTsv`。⚠ 只导出**有单元格的行**（不按 `<dimension>` 铺满） |
| `xlsx_set_cell_keeps_style_attr` | 替换时必须保留原 `<c>` 的 `s`。⚠ 断言**别按固定属性顺序搜字符串**（属性顺序无关） |
| `xlsx_set_cell_preserves_other_parts` | ★★`ZipArchive::SaveToMemory` 的 `if (e.modified)`（跨 part 字节保留） |
| `xlsx_set_cell_preserves_sheet_extras` | ★★同 part 内的 `<mergeCells>`/`<cols>`/行高/原有单元格都必须一字不动 |
| `xlsx_set_cell_inserts_in_column_order` | `SetCell` 的插入位置（插在已有之前 / 追加到行内） |
| `xlsx_set_cell_creates_row_in_order` | 行不存在时按行号升序建新行 |
| `xlsx_roundtrip_after_save` | `SaveToMemory` + 重新载入。⚠ 含 `<sheetData/>` 自闭合场景（**必踩**） |
| `xlsx_escapes_xml_text` | `XmlEncode`（一个裸 `&` 就能把 XML 写坏） |
| `xlsx_rejects_non_xlsx` | 缺 `xl/workbook.xml` 要给可读错误 |
| `xlsx_formats_date_and_time` | `EnsureStyleTableLoaded` / `DisplayForStyle` / `FormatSerial`。★日期还原成 ISO；**无样式的数字保持原始值** |
| `xlsx_formats_custom_percent_and_leap` | `ClassifyCustomFormat`（自定义 `yyyy-mm-dd`）；百分比**不给格式化值**；1900 假闰日（序列 60） |
| `xlsx_formats_1904_system` | `workbookPr date1904` —— 同一序列号两种系统**差 1462 天**（45292→2024-01-01 / 2028-01-02） |

### ThemeUiSelfTest

| name | 优先查看 |
|------|----------|
| `font_design_*` | `theme_ui_layout.h`（须与 `settings_dialog` 字号一致） |
| `custom_layout_*` / `custom_footer_*` / `custom_action_*` | `MakeCustomThemeLayout` |
| `custom_label_width_*` | 标签 RECT 宽度 / `UiFontHeight(26)` |
| `color_picker_layout_*` / `color_picker_parts_*` | `MakeColorPickerLayout` |
| `random_main_color_*` | `RandomAttractiveThemeColors` / `MainColorLooksUsable` |
| `build_custom_theme_*` / `apply_custom_theme_*` | `BuildTheme` / `ApplyThemeFromSettings` |
| `theme_catalog_*` | `ThemeCatalog` / `kThemeCount` |

### BreakoutCooldownSelfTest

| name | 优先查看 |
|------|----------|
| `hold_blocks_cooldown` | `BreakoutCooldownStillWaiting`（按住不得 armed/结束） |
| `release_starts_idle` | 松开后才开始 idle，满时限才恢复 |
| `fresh_input_resets_idle` | 松开后的新输入重置倒计时 |
| `hold_tracker_*` / `reconcile_drops_released` | `BreakoutHoldTracker` |

### HotkeyStopSelfTest

| name | 优先查看 |
|------|----------|
| `busy_physical_down_stops` | `ShouldStopOnToggleKeyDown`（忙碌物理键必须停） |
| `busy_needkeyup_does_not_stop` | 启动键仍按着（自动连发）不得停 |
| `busy_tagged_down_ignored` | ExtraInfo 脚本注入不得当停止 |
| `busy_remote_injected_down_stops` | 远控 LLKHF_INJECTED（无 ExtraInfo）忙碌必须停 |
| `idle_needkeyup_blocks_start` / `idle_physical_can_start` | `ShouldStartOnToggleKeyDown` |
| `idle_remote_injected_can_start` / `idle_tagged_blocks_start` | 远控 INJECTED 可启动；ExtraInfo 不得启动 |
| `physical_up_clears_latch` / `tagged_up_does_not_clear` / `remote_injected_up_clears` | `ShouldClearToggleLatchOnKeyUp` |
| `poller_*` | `TickPoller`（松手后再按才紧急停止） |
| `idle_fallback_*` / `toggle_consume_*` / `ll_owns_*` | `TickIdleStart` / `TryConsumeTogglePress`（全屏游戏 RegisterHotKey/LL 哑火时的空闲启动兜底） |
| `busy_stop_honors_emergency_if_consume_stuck` / `busy_stop_blocks_duplicate_without_emergency` | `AllowBusyToggleStop`（非管理员+提权游戏：启动次消费闩粘死时紧急停止仍须停） |
| `ime_pass_idle_native_blocks` / `ime_pass_composing_blocks` | `ime_hotkey_pass.h`：中文转换模式/组字必须挡 |
| `ime_pass_tsf_english_hkl_blocks` | Win11 TSF：英文 HKL 但 native 仍须挡；仅 OpenStatus 不得挡 |
| `ime_pass_english_mode_allows` / `ime_pass_us_keyboard_allows` | 拼音 Shift 英文（中文 HKL+IME 开、非 native）必须放行 |

### FloatBallGeomSelfTest

| name | 优先查看 |
|------|----------|
| `maximized_window_not_fullscreen` | `FloatBallWindowCoversMonitor` —— **历史 bug 回归**：1920x1080 屏 + 40px 任务栏 ⇒ 最大化窗口 1920x1040 高度 96.3% ≥ 旧阈值 95% ⇒ 被判全屏 ⇒ 悬浮球在普通窗口上直接消失 |
| `framed_full_rect_not_fullscreen` | `FloatBallForegroundIsFullscreen` 的 `hasCaption && thickFrame` 排除（任务栏自动隐藏时的最大化窗口、UWP 宿主 `ApplicationFrameWindow` 的 `GetWindowRect` 就是整屏） |
| `f11_fullscreen_hides` / `borderless_windowed_fullscreen_hides` / `caption_only_still_fullscreen` | 真全屏必须隐藏；无边框窗口化全屏也算；保留 CAPTION 但无 THICKFRAME 的播放器仍算 |
| `minimized_not_fullscreen` / `small_window_not_fullscreen` / `side_taskbar_not_covered` | 最小化、普通窗口、竖排任务栏留边都不得判全屏 |
| `one_pixel_tolerance_covers` / `two_pixel_gap_does_not_cover` | `kFloatBallFullscreenTolerancePx`（1px 容差吸收 DWM 取整误差） |
| `degenerate_monitor_not_fullscreen` / `secondary_monitor_fullscreen` | rcMonitor 非法（0 尺寸）/ 副屏按该屏 rcMonitor 判定 |

> 实机侧补充（在 `float_ball.cpp`，不属纯逻辑层）：`SHQueryUserNotificationState` 官方兜底
> （`QUNS_BUSY` / `QUNS_RUNNING_D3D_FULL_SCREEN` / `QUNS_PRESENTATION_MODE`）、排除自身进程与
> 桌面（`Progman` / `WorkerW` / `Shell_TrayWnd` / `Shell_SecondaryTrayWnd`）、`IsIconic`；
> 退出全屏带 400ms 滞回（`kFsRestoreDelayMs`，`fsExitTick_`），避免 Alt+Tab 瞬间闪烁。
>
> **实机探针**：`build\Release\FloatBallProbe.exe`（`--watch` 持续观察）。判据统一在
> `float_ball_geom.h` 的 `FloatBallIsForegroundFullscreen()`，**产品与探针共用同一份** ⇒
> 探针打印的就是悬浮球此刻的行为，并附**新旧口径对照**。它**不进**任何自检清单（需真实桌面会话）。
> 实测样例（本机 Edge 最大化）：窗口 1721x927 / 屏幕 1707x960 ⇒
> 旧口径（1721≥1621 且 927≥912）判**全屏 ⇒ 误隐藏**，新口径判**非全屏 ⇒ 显示**。

### OverlayInputGuardSelfTest

钉死「我们扣住了用户鼠标捕获时必须能自己还回去」这条红线（细节见 `AGENTS.md`「输入接管红线」）。
事故形态：选区 / 找图 / OCR / 拖拽取点 / 拖动准星 / 悬浮球拖拽都是
`SetCapture` + 把窗口挪到屏外 + **阻塞的模态消息循环**，只在「按钮抬起 / Esc」时退出；
终止事件丢了（远控走 `SendInput`，注入的抬起本来就可能丢）就永久扣住 ⇒ 用户**看得见屏幕、点不动**。

| name | 优先查看 |
|------|----------|
| `no_capture_never_aborts` | `ShouldAbortStuckCapture` 第 1 条门：没扣着捕获 ⇒ 一律不动手 |
| `holding_button_never_aborts` | 第 2 条门（**最容易漏**）：还按着鼠标键就是正常拖拽，**绝不许收尾**；删掉这条门会误杀真实拖拽 |
| `lost_button_up_aborts` | 第 3 条门：捕获在手 + 全抬起 + 已停手 ⇒ 判定抬起丢了，必须收尾 |
| `not_idle_yet_keeps_waiting` / `idle_threshold_is_inclusive` | `kStuckCaptureIdleMs`(1200ms) 的边界：未到不抢跑、恰好等于要触发（`>=` 不是 `>`） |
| `zero_threshold_aborts_when_idle` | 宿主自检节拍用阈值 0 的形态 |
| `wait_times_out_when_queue_quiet` | ★ **`WaitMessageWithTimeout` 真能按时醒** —— 判据写得再对，循环醒不过来（裸 `GetMessage`）也是死的；把唤醒推迟到 5s 即转红 |
| `wait_returns_posted_message` / `wait_flushes_existing_backlog` | 不吞消息、不改 `msg`；有积压时**立即**返回，不许先睡一个周期 |
| `live_capture_predicate_matches_button_state` | 真窗口 `SetCapture` 后：阈值 0 的判据 == 「无鼠标键按下」；`ReleaseCapture` 后恒 false（期望值按**真实键态**算 ⇒ 测试对「跑测时是否按着鼠标」免疫） |

> 实机侧（不属纯逻辑层）：叠层模态循环一律改 `WaitMessageWithTimeout` + `ShouldAbortStuckCaptureNow`；
> 宿主非模态路径（`crosshairDrag_`、主窗捕获、悬浮球拖拽）挂 `SetTimer` 做同一件事。

### HotkeyScopeSelfTest

| name | 优先查看 |
|------|----------|
| `out_of_scope_never_registers` | `AllowSystemRegister` —— **历史 bug 回归**：作用域外仍 `RegisterHotKey(P)` ⇒ 系统吞键 ⇒ 用户「按 P 既不起脚本、又打不出 P」 |
| `cross_tab_switch_releases_letter` / `switch_back_reacquires_letter` | 宏页设 P → 切键鼠录制页必须**注销**（P 能打出来）；切回必须**补注册**（按 P 能启动） |
| `running_session_keeps_registration` / `stop_path_ignores_scope` | `keepForStop` 例外：宏在跑时即使切到作用域外也要保留注册，否则停不下来 |
| `macro_tab_hides_recording_entries` / `recorder_tab_hides_macro_entries` | `DedicatedInScope` 的按 TAB 限定（两类条目互不越界） |
| `other_tabs_have_no_scope` | 设置 / AI 等页面两类条目都不在作用域内（一律放行） |
| `scope_all_pages_keeps_everything` | 「全部页面生效」开关打开 ⇒ 任意页面都在作用域内并保持注册 |
| `scope_truth_table_16` / `register_truth_table_4` | 真值表逐行手写期望（**不复用被测表达式**，否则等于自证） |

> 判据统一在 `src/engine/hotkey_scope.h`，**产品与自检共用同一份** ⇒ 改口径只改一处。
> 产品侧调用点：`engine_script_run.cpp` 的 `DedicatedHotkeyInScope()`（→ `DedicatedInScope`）、
> `engine_hotkeys.cpp` 的 `RefreshDedicatedHotkeyScope()`（→ `AllowSystemRegister`）。
> LL 钩子侧契约「作用域外不吞键」由 `engine_host_window.h` 的 `HotkeyKbProcBody` 里
> `if (!h.inScope) continue;` 实现，`h.inScope` 正是 `DedicatedInScope(...)` 的值。
>
> **实机核对入口**（无探针，靠日志）：切 TAB 后看 `webview_boot.log` 的 `HOTKEY:` 行 ——
> 切到作用域外的 TAB 应出现 `scope refresh ... unregistered>=1`，切回来应出现 `registered>=1`。
> 日志只在**真的发生注册/注销**时打行，所以「无行」= 无变化（正常）。
>
> ⚠ **改作用域前必须复核的三条**（漏一条就是静默失效，不会报错）：
> ① **`inScope` 只在 `RegisterAllHotkeys` 里赋值** ⇒ 任何「重建热键表 / 重新装配」的路径都必须走它。
>    全仓 `ghPlaybackScriptHookCount = 0` 只有 3 处（`RegisterAllHotkeys`、
>    `ResumeHotkeysAfterPlayback` 的预置位、`Cleanup` 的拆解），前两处之后都调 `RegisterAllHotkeys`；
>    `InstallGlobalHotkeyHooks` **不重建表**（只装钩子）⇒ 表里的 `inScope` 保持有效。
> ② **启动顺序是硬约束**：`RestoreHomeState()`（恢复 `activeHomeTab_`）必须排在
>    `RegisterAllHotkeys()` **之前**（`engine_host_window.h` 的 Init 序列，已有显式注释）。
>    对调 ⇒ 启动时 `activeHomeTab_` 还是默认值（`Clicker`）⇒ 作用域全为假
>    ⇒ **一个专属热键都不注册**，要切一次 TAB 才恢复。
> ③ **停止路径不得加作用域判断**（`kHotHoldStop` / `running_` 分支一律放行），
>    否则切页后停不掉正在跑的宏。

### ClickerTimingSelfTest

| name | 优先查看 |
|------|----------|
| `custom_1ms_is_1000us` | `ClickerGapUs` / `ClickerSecondsToUs`（禁止 `int(seconds*1000)`） |
| `hold_always_at_least_1ms` | `ClickerHoldUs`（未启用按下抬起仍保留 1ms 脉冲） |
| `hold_uses_press_release_when_longer` | `ClickerHoldUs` |
| `gap_extreme_10ms` | `ClickerGapUs` |
| `old_ms_truncation_drops_sub_ms` | `ClickerSecondsToUs`（亚毫秒不得截成 0） |

### TimeScaleSelfTest / `window_time_scale_iat`

**先记住这条：`window_time_scale_iat` 是端到端用例，失败时它自己会打印诊断，照着读即可。**

| 现象 | 优先查看 |
|------|----------|
| **变速对某类目标完全不生效，日志里连「窗口变速已下发」都没有** | 被 `TryInstallFakeFocus` 的「不需要假焦点就早退」挡住了。`PrefersLcaBackgroundMessages`（未登记游戏）/ `!UsesFakeFocusForTarget`（铺满全屏/传奇 Delphi）两处必须是 `if (!timeScaleWanted) return;` —— **变速只需要目标进程里有代码，与要不要假焦点无关**。只有远程桌面与内核反作弊仍硬 return。 |
| **诊断显示一切正常（倍率=2000、时间钩>0），目标游戏就是不变速** | 先看下一行的**调用计数**，别急着改钩法。**计数不涨** ⇒ 钩子不在目标调用路径上（钩错地址 / 目标不用这些 API）；**计数猛涨** ⇒ 时钟确实被改了，但**游戏速度不由它决定** —— 要换思路，不是换钩法。 |
| **计数猛涨但游戏逻辑完全不动** | 目标大概率是 **Unity**：Unity 的速度由 `Time.timeScale` 决定，`deltaTime = unscaledDeltaTime * timeScale`；2D 游戏常把 deltaTime 固定成 1/60 ⇒ **根本不从系统时钟来**。看 `Unity可用=` / `Unity已设=` 两位：非 Unity 目标是 0/0；Unity 目标应出现 `Unity可用=1 Unity已设=1`。实现见 `fake_focus_unity_timescale.h`。 |
| `Unity可用=1` 但 `Unity已设=0` | 看下一行 `[窗口模式] Unity 变速：进入前 timeScale=? 已设=? get失败=? set失败=? 后台运行已设=?`：<br>· `get失败=1` / `进入前 timeScale=读不到` ⇒ **`il2cpp_runtime_invoke` 的值类型返回值是装箱的**，必须 `il2cpp_object_unbox` 再读，直接当 `float*` 读会拿到对象头垃圾；<br>· `set失败=1` ⇒ invoke 抛异常；<br>· 两个都是 0 ⇒ 游戏在我们设完之后又把它改回去了。<br>**设值不要依赖 get 的结果** —— 保护逻辑不该建立在「读取一定成功」的假设上（第一版就栽在这）。 |
| 后台窗口模式下游戏整个「不动」（不是慢，是停） | Unity 独立版 `Application.runInBackground` **默认 false**，窗口失焦就停止更新游戏循环。已在 `unity::Init()` 里强制设 true。 |
| `调用计数` 全是 0 且目标确实用这些 API | 用 PE 静态分析确认：收集时间 API 的 IAT 槽 RVA，扫可执行节的 `FF 15 rel32`（`call qword ptr [rip+rel32]`）比对。⚠ `IMAGE_IMPORT_DESCRIPTOR` 第 3 字段是 ForwarderChain（恒 0）、**第 5 个才是 FirstThunk** —— 弄反会得到「0 次调用」的假结论。 |
| `调用计数` 读不到（`有=0`） | 宿主视图没挂载。**必须在 `FakeFocusSoftInput_Detach()` 之前读** —— Detach 会关掉视图，读到 null。自检里就踩过这个坑。 |
| `缓存指针` 那组耗时没跟着变速 | 内联钩没装到「目标缓存的地址」上。检查 `InstallTimeHooks` 的 ①② 两路（导入表实际地址 + 系统 DLL 导出地址）是否都跑了。 |
| `时间钩=0` | 一个候选地址都没钩上 → `AddressInAnyModule` 判定把候选全拒了（self 模块不在 `g_modRanges` 里，所以「槽里已是我们的 detour」会被正确跳过）。 |
| 冒险岛（或其它「只许改 IAT」的目标）拿不到变速 | 它必须走 **IAT-only 模式**（`SetIatOnlyMode(true)`，诊断 `仅IAT=1`）—— 只改导入表槽、不碰代码页。⚠ 该模式下安装数看 `g_iatCount` 而不是 `g_hookCount`（`Diag()` 已按模式取）。 |
| **变速对某类目标完全不生效，日志里连「窗口变速已下发」都没有** | 被 `TryInstallFakeFocus` 的「不需要假焦点就早退」挡住了。`PrefersLcaBackgroundMessages`（未登记游戏）/ `!UsesFakeFocusForTarget`（铺满全屏/传奇 Delphi）两处必须是 `if (!timeScaleWanted) return;` —— **变速只需要目标进程里有代码，与要不要假焦点无关**。只有远程桌面与内核反作弊仍硬 return。 |
| 倍率对、`已装` 真，但走 IAT 的耗时不变 | 钩打偏了 → 看导入表扫描是否漏了提供方（`kernel32`/`kernelbase`/`winmm`/`ntdll`/`api-ms-win-core-*`）。 |
| 2 倍速下重设锚点过频 | 轮询线程**必须用真实时间源**（`RealTick64()`，走未挂钩的 `NtQueryPerformanceCounter`）。**钩了 `kernel32!GetTickCount` 之后，DLL 里任何 `GetTickCount()` 都返回虚拟时间** —— 包括重试节流。 |
| 卸载后时钟没回到真实值 | `UnhookAll`（写回 `original`）或 `StopPoll` 的收尾 `PollOnce(0)`；Unity 目标还要 `unity::Restore()`。 |
| 目标进程崩 / 闪退 | 内联钩的撕裂窗口（写入 12 字节期间恰有线程执行到函数头）。先确认**没有**对本文件的钩子调用 `CallThroughOriginal` —— 被覆盖的字节含半条指令，恢复后执行必崩。Unity 侧还要确认**调用线程已 `il2cpp_thread_attach`**。 |

#### `window_time_scale_only_iat`（关掉假焦点注入时的路径）

**背景（重要）**：变速**不依赖**「启用假焦点注入」。用户关掉注入但开着变速时，执行器走
`timeScaleOnlyInject` → 注入器调 `FakeFocus_InstallTimeScaleOnly` → DLL 在 `InstallCommon` 里
`StartPoll` 之后**早退**，一个假焦点钩都不装。这条路径一旦回归，用户特意关掉的注入会**悄悄生效**且无报错。

| 现象 | 优先查看 |
|------|----------|
| `仅变速bit=0`（诊断 bit2 没置位） | DLL 没走 `timeScaleOnly` 分支 → `InstallCommon` 的早退位置 / 导出名 `FakeFocus_InstallTimeScaleOnly` 是否与 `.def`、`api.h`、注入器的 `primary` 三处一致。 |
| 装了但用户键鼠全丢（**灾难性**） | `PreferHardwareInput()` 等**输入路径**判定误用了 `fakeFocus_.IsInjected()`。必须用 `WindowModeExecutor::FakeFocusActive()`（= `IsInjected() && !time_scale_only()`）；`IsInjected()` 只该出现在 `CanWindowTimeScale()` 与卸载路径。 |
| 注入失败且报「目标 DLL 不支持变速专用注入」 | exe 与 `FakeFocus64.dll` 不同版本。**仅变速模式故意不 fallback** 到 `FakeFocus_Install`（回退会把用户关掉的注入打开）—— 报错是对的，别改成回退。 |
| 变速在用户机器上「完全没反应、连日志都没有」 | 先确认他跑的 exe 与 `app_settings.json` 在哪：`build\Release\app_settings.json` 的 `enableFakeFocusInjection` / `enableWindowTimeScale`。历史上就是 `enableFakeFocusInjection=false` 导致 `CanWindowTimeScale()` 恒 false 而**静默**失效。 |
| **注入成功了、日志也对了，但游戏就是不加速** | 查日志里有没有 `已卸载假焦点`。回放中途有 4 处会卸载（全屏游戏 / 相对鼠标 / 本机点击 / 快捷输入），`Unload()` 会 `FreeLibrary` 把时钟轮询一起带走。必须走 `WindowModeExecutor::DropFakeFocusForHardwareInput()`（先试 `FakeFocus_DisableFakeFocus` 只拆钩、保时钟）。**新增「回退本机输入」分支时一律用它，别直接 `fakeFocus_.Unload()`。** |

**注意**：`TimeScaleSelfTest` 只覆盖纯整数运算（`time_scale_clock.h`）。真正的「补丁装上没有、时钟真的快了没有」只有交互档的 `window_time_scale_iat` 能验 —— 它把 FakeFocus 装到自检进程自己身上，用 `Sleep(300)`（内核定时器，不受变速影响）当真实基准，同时测「走 IAT 的 QPC」与「从 kernelbase 直接解析出的真实 QPC」，断言前者 ≈ 2 × 后者。

#### 三个用例的层次（别互相替代）

| 用例 | 覆盖到 | 覆盖不到 |
|------|--------|----------|
| `TimeScaleSelfTest` | `time_scale_clock.h` 纯整数运算 | 任何 DLL / 注入 |
| `window_time_scale_iat` / `window_time_scale_only_iat` | DLL 的安装分支、时间钩真的装上、**走 IAT 与走「缓存指针」两条路都真的走快 2 倍** | **远程注入**（它们在自检进程内 `LoadLibrary` 后直接调导出） |
| `InjectionSelfTest / fakefocus_timescale_only` | **产品真实路径**：远程注入 + 远程调导出 + 宿主写倍率 + 靶进程时间钩被装 | 靶进程时钟的**数值**（只查诊断，不测耗时） |

> **「缓存指针」断言是必测项**：在装钩子**之前** `GetProcAddress` 拿到 QPC 指针，挂钩后走它也必须看到倍率。
> 这正是当初缺的那条 —— 只测「走 IAT 的调用」时，IAT-only 实现能全绿通过，而真机（Unity）完全无效。

**改注入器 / 执行器链路（导出选择、`timeScaleOnlyInject`、`PreferHardwareInput`）时，必须跑第三行那个** —— 前两个用例全绿也照样可能漏掉跨进程的问题。

| 现象 | 优先查看 |
|------|----------|
| `fakefocus_timescale_only` 报「仅变速注入失败」 | 靶进程里没有 `FakeFocus_InstallTimeScaleOnly` 导出（DLL 版本旧）→ 注入器**故意不 fallback**，报错是对的，别改成回退到 `FakeFocus_Install`。 |
| `fakefocus_timescale_only` 里 `槽=0` | 靶进程里补丁打偏了 → 回到上面 `PatchSlot` 那两行。 |
| `fakefocus_timescale_only` 里 `仅变速=0` | DLL 没走 `timeScaleOnly` 分支 → `InstallCommon` 的早退位置。 |

**改了 `fake_focus_time_scale.h` / `time_scale_clock.h` 之后，`FakeFocus32.dll` 与 `FakeFocus64.dll` 必须一起重建**（32 位那份由 `build_fakefocus32.cmd` 单独编，`--target FakeFocus` 不会带上它，要额外跑 `--target FakeFocus32`）。

**32 位目标「改了没反应」先看目录里的变体**：`PickPreferredFakeFocusDll` 过去**无条件优先 `FakeFocus32.next.dll`**，
而 `build/Release` 里躺着一个 9/7 的旧副本 ⇒ 任何 32 位目标都注入那份旧 DLL。现已改为
**仅当 `.next` 不比主 `FakeFocus32.dll` 旧时才优先**。排查同类问题：`ls build/Release/FakeFocus32*.dll` 看时间戳。

### RecorderSelfTest

| name | 优先查看 |
|------|----------|
| `event_sort_timestamp_sequence` | `SortRecordedEvents` |
| `relative_delta_conserved` / `mixed_capture_channels` | `ConvertRecordedEventsToActions` |
| `same_timestamp_button_order` / `stop_hotkey_tail_trimmed` | `ConvertRecordedEventsToActions` |
| `same_timestamp_relative_keep` | `ConvertRecordedEventsToActions`（同戳相对包不合并） |
| `repair_copy_leaves_source` | `RepairCompressedRelativeGaps`（只改执行副本） |
| `timeline_catchup_skips_stall` | `PrecisionInputTimeline::WaitDeltaUs`（小抖动过点追赶） |
| `timeline_injection_overrun_eats_short_wait` | `PrecisionInputTimeline::WaitDeltaUs`（上一拍注入超支吃掉本拍短等待：立刻返回、`late` 如实记录、4ms 量级**不** rebase）。**「短等待全带 late」别去改 scheduler** |
| `timeline_large_stall_rebases` | `PrecisionInputTimeline::WaitDeltaUs`（大 stall 平移原点，禁止短等待连发） |
| `timeline_wait_until_rebases` | `WaitUntilElapsedUs` 过点后同样 rebase，避免只修入口路径 |
| `timeline_long_wait_wall` | 长等待用自适应切片，墙钟不得被压成瞬时 |
| `timeline_gap_from_now_no_catchup` | `WaitGapUs` 从此刻睡满（竞品 Delay 语义） |
| `wheel_gap_becomes_wait` | `ConvertRecordedEventsToActions`（滚轮前等待） |
| `compile_integer_timeline` / `legacy_wait_timeline` | `CompileInputTimeline` |
| `random_duration_rejects_timeline` | `CompileInputTimeline`（`randomDuration` 禁用精密轴） |
| `timing_us_prefers_over_duration` | `CompileInputTimeline` / `ActionStepUs` |
| `convert_gaps_become_waits` / `same_timestamp_no_wait` | `ConvertRecordedEventsToActions`（显式 Wait） |
| `timeline_fold_vs_explicit_equiv` | `CompileInputTimeline` 折叠 vs Wait 等价 |
| `expand_recording_keeps_gaps` / `expand_script_default_duration_no_wait` / `expand_idempotent` | `ExpandRecordingPreDelaysToExplicitWaits` |
| `wait_stats_use_timing_us` | `ActionStepUs` |
| `snap_timing_to_wait` / `convert_drops_approach_moves` | `recording_to_findimage`（前置 Wait） |
| `click_capture_rect_clamped` | `ComputeClickCaptureRect` |
| `hover_patch_covers_click` | `HoverPatchCoversClick`（图片定位按下前模板） |
| `convert_window_relative_findimage` | `MakeFindImageFromDown` 保留 `windowRelative` + 全客户区搜索 |
| `scheduler_cancel_interrupts` / `scheduler_wait_until_elapsed` | `PrecisionInputTimeline` |

### VirtualHidSelfTest

| name | 优先查看 |
|------|----------|
| `install_script_present` | `driver/qst_vhid/_elevate_install.ps1`（相对 exe 上溯仓库/发版目录） |
| `install_script_no_boot_policy` | 禁止 `bcdedit /set`、测试签名 on、HVCI、`shutdown`、固件重启、注册开机任务 |
| `install_script_no_root_certs` | 禁止 `import_certs.reg` / `certutil` 灌 Root |
| `install_script_no_driverstore_rm` | 禁止直接删 DriverStore 仓库目录 |
| `install_script_filter_after_start` | `FILTERS_ONLY_AFTER_RUNNING`：LowerFilters 仅在服务 Running 之后 |
| `product_no_firmware_reboot` | `qst_webview_shell.cpp` 禁止 `shutdown /r /fw` |
| `product_no_official_ic_exe_msg` | `hid_interception.cpp` 禁止引导用户跑 `install-interception.exe` |
| `release_pack_no_lab_installers` | `package_release.ps1` 禁止随包官方 IC 安装器 / `sign_and_install.ps1` |
| `device_open` | 驱动未装 / `_elevate_install.ps1` / 设备接口 GUID |
| `key_press_release` | `VirtualHidBackend::SendKey` / Report ID 1 / scan→HID |
| `mouse_rel_move` | Report ID 2 / `MoveRelative` |
| `mouse_abs_jump` | 仅更新内部状态（不发 Report ID 3）；桌面绝对由 SetCursorPos |
| `mouse_wheel` | Report ID 2 wheel/hwheel |
| `release_all_no_stick` | `ReleaseAll` / EndSession |

### BridgeJsonSelfTest

> 背景：2026-09-18 验收发现的 P0 —— `saveSettings` 静默失效（改设置提示成功、实际写回旧值）。
> 根因是壳用 `substr(首个 '{')` 截到末尾，payload 多带外层 `}` → 非法 JSON；旧的手写字符
> 扫描容忍它，换 nlohmann 严格解析后所有键取不到。641 个用例全绿也照不到，因为桥接层零覆盖。
> 全文：`docs/refactor-acceptance.md` §3；测试规格：§7.3。

| name | 优先查看 |
|------|----------|
| `subobject_last_key` / `subobject_not_last_key` | `jsonutil::GetSubObjectText`（严格解析取**配对**对象，禁止括号扫描） |
| `subobject_brace_and_nested` | 同上（值里含 `}` 字符 / 嵌套对象不得被截断） |
| `subobject_missing_or_null` | 同上（键缺失 / null / 非对象 → false） |
| `subobject_invalid_json_no_throw` | `TryParse`（非抛异常）；`GetSubObjectText` 的 `dump(..., error_handler_t::replace)` |
| `regression_trailing_outer_brace` | **本次事故的回归锁**：残破 payload 必须被 `IsParseableObject` 拒绝 |
| `subobject_applyable_fields` | 子对象文本可被 `GetInt/GetBool/GetNumber/GetActionTokenArray` 直接消费 |
| `diagnostics_silent_on_valid` | 合法输入**不得**产生诊断（否则日志噪音） |
| `diagnostics_reports_invalid` | `NoteParseFailure` / `ParseFailureCount` / `SetParseFailureSink`；产品侧 sink 在 `qst_webview_shell.cpp` 写 `webview_boot.log`（`JSON: 解析失败 where=…`） |

**改动本域时的硬规则**：

- 取子对象**只能**用 `GetSubObjectText`；不要写 `json.find('{')` + `substr`（A/B 实测：
  打回这种写法，本 suite 立刻 5 条变红）。
- 不要把解析失败做成「宽容模式」开关 —— 那会让「残破 JSON 静默取到半个值」重新成为默认
  （验收报告 §7.4）。严格解析是对的，错的是调用方构造了非法 JSON。
- 新增桥接命令时同步 `ui/bridge.js`（见 #10 桥接契约正式化，`BridgeContractSelfTest`）。

### ScriptRunnerSelfTest

> 背景：此前 `qst_engine` **无法脱离壳链接**（缺 `PostToWebUi` / `HotkeyLogLine` /
> `NotifyWebDebugWindowSetting` / `SyncHomeSelectionCache` / `g_instance`），所以没有任何
> SelfTest 能链引擎 —— 这才是「引擎主循环零单测」的机制性根因。B 段把 4 个函数倒置成
> 回调注入（`src/engine/engine_ui_hooks.*`）、`g_instance` 定义搬到 `qst_utils` 之后，
> 引擎可独立链接。详见 `docs/refactor-acceptance.md` §7.2 B1/B3。

| name | 优先查看 |
|------|----------|
| `hooks_default_noop` | 未装钩子时 4 个转发必须安全 no-op（引擎要能在无 UI 进程里跑） |
| `hooks_installed_flag` / `hooks_forward_*` | `qst::engine::SetUiBridgeHooks` 与 4 个转发函数（`engine_ui_hooks.cpp`） |
| `hooks_merge_semantics` | `SetUiBridgeHooks` **只覆盖非空成员** —— 壳的真实现分布在两个 TU（bridge backend 三个 + shell 热键日志一个），改成整体覆盖会互相清空 |
| `engine_headless_start`（`--engine`） | `qst::engine::Start` / 测试进程需自抽消息 |
| `engine_run_varcompute_wait` / `engine_run_loop_body` / `engine_run_goto_skips`（`--engine`） | `DebugRunActions` + 执行核心；断言用**时序语义**（循环重复→耗时成倍、goto→耗时骤减） |
| `engine_run_varcompute_logs_reason`（`--engine`） | 变量运算失败时运行日志必须给出**变量名 + 转义后的源文本**；断言走真实链路 —— `MacroDebug().SetWebUiEnabled(true)` + `Create(nullptr,nullptr,nullptr)` + `SetMacroDebugWebPoster` 抓 JSON |

**改动本域时的硬规则（都是实测踩出来的）**：

- 引擎调壳**只能**走 `engine_ui_hooks.h`。不要让引擎再 `#include "webview/webview_bridge_backend.h"`
  ——那会把「引擎无法脱离壳链接」重新钉回去。
- 测试里**不要**调 `qst::engine::Shutdown()`：headless 引擎窗的 `WM_DESTROY` 会走
  `TerminateProcess`，直接把测试进程杀掉，且 stdout 是块缓冲 → **输出全丢、exit 还是 0**
  （现象是「什么都没发生」，极难定位）。进程退出时由 OS 回收即可。
- headless 跑动作时**必须自己 `PeekMessage` 抽消息**，否则收尾信号不派发、
  `IsRunning()` 永远为真（实测跑完 2 步后 10s 仍不收尾）。
- `ExecutedSteps()` 在收尾后被清零 → 只能在运行中采样取最大值；不要用「收尾后读步数」做断言。
- 循环体编码：用 `FlattenNestedActionParamList`（生产构建器）生成，**不要手搓 indent**。
  生产编码是 `[loop(indent=0), body(indent=1)]`，**不生成 endLoop**；显式塞一个
  `indent=1` 的 endLoop 会把循环体提前切断（实测循环体只跑一遍）。
- 要断言「运行日志内容」：`AppendDebugLog` 有两道闸 —— `KeyFunctionDebugActive()`
  （= `playback.autoOutputKeyFunctionDebug` **&&** `MacroDebug().IsCreated()`）与 `deferPlaybackDebugUi_`。
  测试里 `MacroDebug().SetWebUiEnabled(true)` + `Create(nullptr,nullptr,nullptr)` 只置 `webCreated_`
  （不建 GDI 窗），再 `SetMacroDebugWebPoster` 抓 JSON。⚠ `AppendLog` 是进 `webPendingLogs_`
  队列，**必须再调 `FlushWebPendingLogs()` 才推出去**；`JsonEscapeUtf8` 不转义非 ASCII，
  所以断言里可以直接 find 中文。用完记得把 poster 清空、`SetWebUiEnabled(false)` 复位。

### BridgeContractSelfTest

> 背景：桥接层是「JS 发的消息形状」与「C++ 解析的严格度」之间的**隐式契约**，两侧各写各的
> 字符串字面量，没有任何东西校验它们对得上——2026-09-18 的 `saveSettings` P0 就长在这条缝上。
> 现在 C++ 入站命令面显式声明在 `src/webview/bridge_commands.h`，本 suite 双向校验。
> 详见 `docs/refactor-acceptance.md` §7.2 C 段与 `docs/refactor-progress.md` §八。

| name | 优先查看 |
|------|----------|
| `table_no_duplicates` | `kBridgeJsCommands` / `kBridgeCppOnlyCommands` 无重名且不交集 |
| `table_all_handled_in_shell` | 表里每条，`qst_webview_shell.cpp` 的 `type == "..."` 分派里必须真有分支 |
| `table_all_sent_by_js` | **表里每条，JS 侧必须真的会发**（防「加了 C++ 分支但没人调」） |
| `table_sender_files_exist` | 表里登记的 sender 文件存在于 `ui/` 下 |
| `shell_dispatch_all_declared` | C++ 分派全部已登记（防「加了 JS 调用忘加 C++ 分支」→ 静默无响应） |
| `cpp_only_all_handled_and_reasoned` | 例外表每条有理由且确有分派 |
| `cpp_only_not_sent_by_js` | 例外当前无人发；一旦被发就该升级进正式表 |
| `js_sends_all_declared` | JS 实际发的每条都在表里（无孤儿命令） |

**改动桥接时的硬规则**：

- 新增 JS→C++ 命令的顺序：① 在 `bridge.js`（或 `debug.html` / `agent.html`）加发送方 →
  ② 在 `kBridgeJsCommands` 登记（含 sender）→ ③ 在 `qst_webview_shell.cpp` 加入站分派。
  三步缺一，本 suite 会红。
- C++ 分支先落地、JS 侧还没接：登记到 `kBridgeCppOnlyCommands` 并写清理由。
- **不要**把脚本动作 JSON 的 `type`（`loop`/`if`/`wait`/`mouseClick`…）当桥接命令。
- 测试提取 JS 侧时必须区分**专用发送方**（`bridge.js`/`debug.html`/`agent.html`，整文件取 `type:`）
  与**混合文件**（`app.js`/`pro-mode.js`/`visual_editor.js`/`index.html`，只取 `post(...)` 实参里的）；
  不加区分会出现 `loop`/`else`/`macro`/`ai`/`rec`/`sched`/`paint` 等 7 个假阳性。
- 壳收到未知命令会回 `{"type":"error"}`（`app.js:14403` 弹 toast）**并写 `webview_boot.log`**
  —— 桥接层不允许「无痕失败」。

## 相关

- 仓库短路由：[`AGENTS.md`](../../../AGENTS.md)
- 自检源码：`tools/*_selftest.cpp`
