# Agent notes (QuickScriptTool)

## UI 方向（WebView2 壳）

产品 UI 目标改为与 `docs/ui-redesign-mockup.html` 1:1，采用 **WebView2 壳 + 现有 C++ 引擎**（禁止 Electron 整仓重写；禁止 WebView 内 SendInput）。

- **产品 = Web**：`QstWebViewShell`（`ui/` + `src/engine/qst_engine.h` + `src/desktop_tools/`）；发版主 exe 为壳复制为 `QuickScriptTool.exe`
- **永久原生（DesktopTools）**：选区 / 找图叠层 / OCR 叠层 / 准星 / 启停·脚本热键捕获 / 托盘 — **禁止**塞进 WebView 冒充
- Phase-1 壳：`src/webview/qst_webview_shell.cpp`；引擎：`src/engine/engine_runtime.cpp` + `engine_host_window.h`（headless；无 GDI 产品 exe）
- 分层库：`qst_desktop_tools` / `qst_engine`（OBJECT，`cmake/QstProductLibs.cmake`）
- 前端：`ui/index.html` + `shell.css` + `bridge.js`
- **Fixed Version 便携**：[`docs/webview-fixed-runtime.md`](docs/webview-fixed-runtime.md)
- Bridge / 分期：[`docs/webview-bridge-api.md`](docs/webview-bridge-api.md)
- 原生分层盘点：[`docs/webview-native-layering.md`](docs/webview-native-layering.md)
- 打包：`powershell -File tools\package_webview_portable.ps1` → `dist\QstWebViewShell-Portable.zip`；完整发版 `tools\package_release.ps1`（主产物 Shell）
- 发版版本：`tools\product_version.txt`（须同步 `installer\QuickScriptTool.iss` 的 `MyAppVersion` 与 `src\app_branding.cpp`）
- 发版依赖：运行必需项（MSVC CRT 旁路、`WebView2Fixed`、`ui`、OpenCV、扩展、驱动**安装链**等）必须进 zip/安装包；仅设置里点装的内核驱动 / OCR Python 可运行时再装（见 `package_release.ps1` 头注释）

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
| 录制回放 | `RecorderSelfTest` → `QstRecorderLogicTest.exe` | 见元 Skill FAIL 表 |
| 虚拟 HID | `VirtualHidSelfTest` | `driver/qst_vhid/` + `src/input/virtual_hid.*` |

共享 harness：`tools/selftest_harness.h`。各 exe：`tools/*_selftest.cpp`。

### 构建模板

```powershell
& "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:<Target> /m /v:minimal
build\Release\<Target>.exe --json
```

窗口模式可选烟雾：`--macro`。定时任务硬规则见 scheduled-task-debug skill。

PowerShell 不要用 `/t:A;B` 拼多个 target（分号会拆命令）；分开跑。`--list` / `--json` 结果均在 stdout。

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

助手按 `AppDir()\skills\agent\<section>.md` → `AppDir()\skills\<section>.md` → 内嵌兜底读取。
改动助手 Skill / 优化算法后必须跑 `AgentAssistantSelfTest --json`（exit 0）并同步 `.cursor/skills/agent-*` 与打包脚本的拷贝列表。

### 核心约定（改动前必读）

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
