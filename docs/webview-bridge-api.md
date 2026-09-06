# WebView2 壳 + C++ 引擎 — Bridge API

> 产品主入口：`QstWebViewShell.exe`（发版时复制为 `QuickScriptTool.exe`）= Web UI + Engine（`src/engine/qst_engine.h`）+ DesktopTools  
> **禁止** Electron；**禁止** WebView 内 `SendInput`；不破坏 Fixed `WebView2Fixed` 便携。
> 旧 GDI 产品 exe / `archive/gdi_legacy` **已移除**；`forceNative:1` 一律 `legacy_unavailable`。

设计真值：`docs/ui-redesign-mockup.html`（仅 `.app` + overlays）。

---

## 迁 Web / 留原生（架构硬边界）

| 层 | 职责 |
|----|------|
| **WebView** | 列表 · 表单 · 方案 UI（主页四 Tab、设置、宏编辑参数、定时、录制优化面板、主题自定义） |
| **C++** | 桌面级手势 · 匹配 · 执行引擎（截屏选区、冻屏匹配、准星、回放/录制/热键/窗口模式、驱动安装） |
| **Bridge** | 参数进、结果出；需要桌面时 `ScopedHideShell` 藏壳 → 原生窗 → JSON 回传 |
| **DesktopTools** | 桌面能力唯一 C++ 门面：`src/desktop_tools/desktop_tools.h`（归属见 [`webview-native-layering.md`](webview-native-layering.md)） |

### 迁 Web（产品主路径）

- 主页四 Tab、设置读写、宏编辑参数面板、定时任务 UI、录制优化方案面板
- 主题预设 / HTML 自定义主色点缀色（`#ov-theme`）
- 找图模板裁切 UI（`#ov-crop` canvas 选区）；写盘/偏移仍走 DesktopTools `FindImageCropRect`

### 必须留原生 + bridge（禁止塞进 WebView 假装全屏）

| bridge `type` | 原生实现 | 注意 |
|---------------|----------|------|
| `pickScreenRegion` | `ScreenshotOverlay`（标题「选取区域」） | `ScopedHideShell`；回传屏幕坐标 |
| `captureTemplateScreenshot` | 同上（标题「屏幕截图」） | 保存模板到 `images/`，回写 `imagePath` |
| `findImageMatch` | `MatchOverlay` | `ResolveImagePath`；`mode`: `test` / `offset` / `region` |
| `findImageCrop` | **Web `#ov-crop` +** `FindImageCropRect`（**必须** `cropX/Y/W/H`） | 缺 rect → `crop_rect_required`；输出 `imagePath` + offset；`unchanged`=全图 |
| `readImageDataUrl` | 读文件 → data URL | 找图预览 `<img>`，避免 `file://` 拦截 |
| `resolveImagePath` | `ResolveImagePath` | 相对→ScriptsDir/FindImagesDir |
| `crosshairPick` | `CrosshairDragController` | `ScopedHideShell`；`coordinates` \| `programPath` \| `windowTarget` |
| `installDriver` | `ShellExecute` 提权 | `GetExitCodeProcess`；成功/失败/取消 |
| `queryVhidStatus` | 只读探测（无 UAC） | HVCI / 待重启续装 / 设备可用 / 上次退出码 |

Web 侧按钮只发消息、填回表单；**禁止**用浏览器 Pointer Lock / 全屏 div 冒充桌面选区或准星。

### `forceNative`

- 默认 **0**（省略即 HTML 主路径）。**产品 UI 不提供**「原生对话框 / forceNative」入口。
- 手写 `forceNative:1`：**一律**回 `ok:false`，`detail`/`code`=`legacy_unavailable`（旧 GDI 对话框已移除）。

---

## 窗体尺寸与圆角

| 模式 | 客户区 | 说明 |
|------|--------|------|
| Home | **1380×960** | 对齐 mockup `.app`；设计稿=客户区 1:1 |
| Editor | **1800×1230** | 对齐 mockup `.app.editor-mode` |
| 缩放 | **禁止** | 无 `WS_THICKFRAME`；`WM_GETMINMAXINFO` 固定外框 |
| Zoom | **等比** | Web `SHELL_DESIGN` = 客户区；`Math.min(zw,zh)`，禁止非等比拉伸 |
| DPI | Per-Monitor V2 | `AdjustWindowRectExForDpi` |
| 圆角 | **≈14px** | CSS `.app { border-radius:14px }` + Win11 `DWMWCP_ROUND` / 否则 `SetWindowRgn` |
| 断言 | `window.clientSize` | setMode/启动后 `GetClientRect` 与 expectW/H；不匹配再强制一次 |

产品 `ui/index.html` 的 `.app` **禁止** mockup 的 `min(…vw)` 缩小；须 `width/height:100%` 填满客户区。发版/本地请跑 `build\Release\QstWebViewShell.exe`（旁路 `ui/` 由 MSBuild 复制），勿用陈旧 `dist`。

**headless 引擎窗**：凡 `RestoreMainWindowAfterRun` / `RestoreBreakout*` / `ForceEndBreakoutUiState` / 录制·连点恢复 / 对引擎 `hwnd_` 的 `SW_SHOW*|SW_RESTORE`，在 `headlessUi_` 下一律 `SW_HIDE`。用户可见窗只用 `webViewHostHwnd_`（`RestoreMainWindowForUser`）。

---

## Bridge（已实接）

| type | 状态 |
|------|------|
| `window.*` / `window.setMode` | home↔editor 居中改客户区 |
| `listScripts` / `listRecordings` / `listAgentConversations` | 已接 |
| `openSettingsData` / `saveSettings` | 已接 |
| `runScript` / `stopScript` / 连点 / 录制 | 引擎 |
| `openEditor` / `saveEditor` | 已接 |
| `getGlobalHotkey` / `setGlobalHotkey` | HomeState 持久化 |
| `getEngineStatus` | 定时推送（含 `executedSteps`/`currentScript`/`runningMode`） |
| `getAppBranding` / `checkUpgrade` | 关于页 / 升级诚实 stub（`stub:true`） |
| `themeCatalog` / `applyTheme` | HTML 主题；自定义走 COLORREF |
| `openThemeCustom` | 可选原生拾色器（默认用 HTML `#ov-theme`） |
| `installDriver` | exit code + `detail` |
| `queryVhidStatus` | 设置弹窗打开 / 选「虚拟 HID」后端时即时刷新状态 |
| `installOcr` | 后台 `RunOcrInstall` + Web `#ov-ocr` 进度（不再弹 `OcrInstallDialog`）；`repair`:0\|1 |
| `captureGlobalHotkey` | `HotkeyCapture` + `Begin/EndHotkeyCaptureRelease` → HomeState |
| `captureScriptHotkey` | 同上（scriptHotkey）；避免重捕获已注册键误触发 |
| `setActiveHomeTab` / `setHomeSelection` / `getHomeState` | 引擎主页 Tab/选中；启动与列表变更后 Web 必须对齐 |
| `setRecorderCaptureScope` | 持久化 HomeState + 录制钩子过滤（0=当前窗口 / 1=全局） |
| `showDebugWindow` | ReloadSettings → 独立调试 WebView 窗（`ui/debug.html`）；Gdi 仍原生 MacroDebugWindow |
| `openAgentWindow` | 独立 AI 助手 WebView 顶层窗（`index.html?mode=agent`，可拖出主壳） |
| `cancelAgentMessage` | 置 `g_agent.cancelFlag` + Abort HTTP |
| `showWindowModePreview` / `hideWindowModePreview` | 查找目标 HWND → JPEG dataUrl → **仅 Web `#wmPreview`** |
| `pickImageFile` / `findImageMatch` / `pickScreenRegion` | 原生 overlay；路径经 `ResolveImagePath` |
| `findImageCrop` | **产品**：Web `#ov-crop` + `cropX/Y/W/H` → 原生写盘（缺 rect → `crop_rect_required`） |
| `captureTemplateScreenshot` | ScreenshotOverlay 标题「屏幕截图」→ 保存模板 → `imagePath` |
| `readImageDataUrl` / `resolveImagePath` | 预览缩略图 / 路径解析 |
| `crosshairPick` / `browsePath` | 准星 / 文件对话框 |
| `openScheduledTasks` | **HTML**；`forceNative:1` → GDI |
| `listScheduledTasks` / `saveScheduledTask` / `deleteScheduledTask` / `setScheduledTasksGlobalDisabled` | `scheduled_tasks.json` + `ReloadScheduledTasks` |
| `openRecordingOptimize` | **HTML**；`forceNative:1` → GDI |
| `loadOptimizeRecording` / `applyOptimizeRecording` | C++ 算法；scheme 0–4；result 含 `recordings` |
| `importScript` / `exportScript` | JSON 或 ZIP（有找图资源时 ZIP：`script.json`+图片）；导入后 Reload + 选中同步 |

Web 编辑器动作列表：`Loop`/`If`/`Else`/`DefineBlock` 支持 chevron 折叠（仅 UI，保存仍含完整 actions+indent）。设置 → AI：`savedModels` 增删后立即 `saveSettings` 并刷新 `#agentModelCombo` / `#aiModelCombo`。

### 窗口模式预览

```json
{ "type":"showWindowModePreview", "editorMode":1|2, "windowMode":{ /* 同 saveEditor */ } }
→ { "type":"showWindowModePreview.result", "ok":true, "native":false, "label":"…", "dataUrl":"data:image/jpeg;base64,…" }

{ "type":"showWindowModePreview", "fromRunning":1 }
→ 使用引擎「正在运行脚本」的 windowMode（非 Web 编辑器缓存）

{ "type":"hideWindowModePreview" }
→ { "type":"hideWindowModePreview.result", "ok":true }
```

宏运行 HUD「窗口预览」按钮在 `runningMode>0` 时可见；请求一律 `fromRunning:1`。**产品路径只刷新 Web `#wmPreview`**（`native:false` + `dataUrl`）；不再同步弹出原生 `WindowModePreview` 窗。

### Agent 附件 / 流式状态 / 取消

`sendAgentMessage` 支持 `attachments: ["C:\\path\\file.png", …]`；后端 `AgentLoadAttachmentFromPath` + `AgentBuildUserMessage`（与原生 Agent 一致）。`text` 可空（仅附件时）。附件区在输入框上方；`+` 可选文件或剪贴板附件。

新增剪贴板桥（对齐原生 `AgentTryReadClipboardAttachments`）：

```text
{ "type":"pasteAgentClipboard" }
→ { "type":"pasteAgentClipboard.result", "ok":true, "paths":["…"] }

{ "type":"saveClipboardImage", "dataUrl":"data:image/png;base64,…", "ext":"png" }
→ { "type":"saveClipboardImage.result", "ok":true, "path":"…\\images\\agent_clip_….png" }
```

图片落盘到 `images/`，再作为普通路径附件发送。

流式推送（对齐原生 Agent 面板；含推理模型 thinking）：

```json
{ "type":"sendAgentMessage.delta", "ok":true, "id":"…", "delta":"…" }
{ "type":"sendAgentMessage.reasoningDelta", "ok":true, "id":"…", "delta":"…" }
{ "type":"sendAgentMessage.reasoning", "ok":true, "id":"…", "reasoning":"…" }
{ "type":"sendAgentMessage.status", "ok":true, "id":"…", "status":"…" }
{ "type":"sendAgentMessage.tool", "ok":true, "id":"…", "name":"writeScript", "args":"…" }
{ "type":"sendAgentMessage.result", "ok":true, "id":"…", "reply":"…",
  "conversations":[…], "scripts":[…], "recordings":[…] }

{ "type":"cancelAgentMessage" }
→ { "type":"cancelAgentMessage.result", "ok":true }
```

- `reasoning` / `reasoningDelta` 对应引擎 `AgentSendCallbacks.onReasoning` / `onReasoningDelta`；Web `#chatLog` 渲染可折叠「思考过程」块（对齐原生 thinking 区块）。
- 推送均带会话 `id`，供多 Tab 路由。
- **多会话**：Web `#ov-agent` 支持多 Tab（各自 `conversationId` / 草稿 / 附件 / busy 展示）；后端同一时刻仅一条流式发送（另一会话发送时返回「助手忙碌中」）。打开其它会话且当前 busy 时，`openAgentConversation` 自动 `peek`（只读磁盘，不切换活跃 core）。
- 标题栏 **─** = 最小化到右下角 dock（`#agentMinDock`），**×** = 关闭当前 Tab（末 Tab 关窗）；二者行为不同。
- busy 时发送按钮变为「取消」并调用 `cancelAgentMessage`（置 `cancelFlag` + Abort HTTP）。Web `#chatLog` 显示 status/tool 行（可选勾选显示 `args`）；若 `scripts` 相对发送前新增路径，toast 脚本名并 confirm 后 `openEditor(path)`；`recordings` 变更则刷新录制列表。

```text
{ "type":"openAgentConversation", "id":"…", "createNew":0|1, "peek":0|1, "panelKey":"…" }
→ { "type":"openAgentConversation.result", "ok":true, "panelKey":"…", "conversation":{…} }
```

`peek:1`（或 busy 时打开其它 id）只读返回历史，`conversation.peek:true`，不替换后端活跃会话。

### 对话编辑重发 / 变更撤销 / 白名单命令（Skill 化）

助手能力以 Skill 文件形式分发（`skills/agent/*.md`，见 `.cursor/skills/agent-*`），
助手通过 `readAgentSkill(section=conversation|revert|shell|optimize|scriptStrategy)` 读取。

**编辑已发送消息并重发**：`sendAgentMessage` 增加 `rewindUserIndex`（0 起，user 消息序号）。
后端按 `AgentCore::TruncateHistoryToUserRound` 截断到该条消息之前再发送；前端 user 气泡提供「编辑」按钮，
输入框上方出现编辑提示条，回车重发、Esc 取消。编辑第 0 条消息时对话标题会重置。

```json
{ "type":"sendAgentMessage", "id":"…", "text":"新文本", "rewindUserIndex":1, "panelKey":"…" }
```

**变更撤销**：所有会落盘的助手工具（writeScript / createMacroScript / optimizeScript /
optimizeRecording / deleteScriptFile / 定时任务 CRUD / updateSettings / writeAgentFile）先写快照
到 `AppDir()\agent_changes\journal.json`。

```text
{ "type":"listAgentChanges" }
→ { "type":"listAgentChanges.result", "ok":true,
    "changes":[ { "id":"…", "time":"…", "tool":"writeScript", "title":"…", "path":"…", "ok":true, "reverted":false } ] }

{ "type":"revertAgentChange", "id":"…" }
→ { "type":"revertAgentChange.result", "ok":true, "changes":[…],
    "scripts":[…], "recordings":[…] }
```

**白名单命令与文件操作**：`runAgentCommand` 仅允许 git 只读子命令、MSBuild 自检 Target、
`build\Release\*SelfTest.exe --json`、`where`；参数经 `CommandLineToArgvW` 解析后逐条校验，
不经 cmd/powershell。另有 `listDirectory` / `readAgentFile` / `searchAgentFiles` /
`writeAgentFile`（自动撤销）/ `copyAgentTextToClipboard` / `pasteAgentClipboardText`，
路径限制在 scripts / recordings / images / library / AppDir（开发仓库另含 docs、.cursor/skills、tools、ui）。

### 脚本理解 / 规范化生成 / 模型自适应

**规范化脚本**：`writeScript` 只接受规范化产物——保存前逐动作执行
`BuildScriptActionFromJson` 重建校验 + 关键字段完整性校验（findImage 缺 imagePath、if 缺
conditionExpr 等手写残缺动作直接拒绝）。AI 生成脚本必须走
`buildScriptActions`（schema 补全 + 序号/stopMacro 自动处理）+ `createMacroScript`。

**脚本泛化翻译（两级，防长输入）**：`readScript` 默认输出**动作概览**——每步只有
动作名 + 类型级语义（按钮/跟随动作/循环方向/变量图/无限循环/修饰键/限定区域/带截图等）
+ **完整备注**（备注必传、不截断，是意图关键），不含坐标/时长/阈值等数值参数，
让 AI 先做模糊理解；需要具体参数分析时传 `detail=true`
查详细说明（`DescribeScriptActionsDetail`：图片/坐标/条件/间隔/修饰键/容差/AI 超时等），
或 `raw=true` 看原始 JSON（单次 ≤32KB）。均按 `startIndex`/`maxActions` 分页（每页 ≤60 步）。
实现：`DescribeScriptActionsBrief`（概览）/ `DescribeScriptActionsDetail`（详细），
位于 `src/agent_script_ops.cpp`。

**脚本图片理解**：`readScript` 的 `includeImages`（默认 true）输出脚本引用图片的
`[[AGENT_IMG:<绝对路径>]]` 标记；`AgentCore` 工具循环解析标记：

- 多模态模型（`ModelSupportsVision`）→ 图片编码为 `data:image/...;base64` 并以
  `keep_images` user 消息嵌入下一轮（不被历史图片剥离逻辑删掉）；
- 非多模态模型 → 替换为「当前模型不支持视觉，未读取图片内容」的路径提示，不报错；
- 每轮最多嵌入 8 张，超出部分输出跳过说明；读取失败的图片输出失败原因。
- 工具结果预算：`readScript` 16000 / `readAgentFile` 48000（`AiToolResultBudgetChars`，
  勿盲目调大——长输入是 AI 卡死主因）。

**模型差异自适应**（`AgentCore::BuildRequest`）：

- 推理模型（`ModelIsReasoningType`：o1/o3/o4、DeepSeek R1/Reasoner、Kimi K2 Thinking、
  Grok Reasoning）不发送 `temperature`（否则 400）；
- `thinking.disabled` 仅对火山/方舟/豆包网关（URL 含 volces/ark/doubao）发送；
- `max_tokens` 按模型 clamp（deepseek 32768 / gpt-4o·4.1 16384 / o 系列 100000）；
- 上下文滑动窗口：只保留 system + 最近 10 个 user 轮（含随后的 tool/assistant），
  以 user 消息为轮次边界裁剪，保证 tool_call_id 配对完整。

### 主页 Tab / 选中同步（热键目标）

引擎 `RestoreHomeState` 为真值；Web 启动默认 tab/sel 不可信。约定：

```json
{ "type":"getHomeState" }
→ { "ok":true, "activeTab":0|1|2|3,
    "selectedScriptPath":"…", "selectedRecordingPath":"…" }

{ "type":"setActiveHomeTab", "tab":0|1|2|3 }
{ "type":"setHomeSelection", "tab":1|2, "path":"…" }  // path 空=清空选中
```

- **启动**：`listScripts` + `listRecordings` + `openSettingsData` 完成后调 `getHomeState`，再 `setActiveHomeTab` + `setHomeSelection`（Web UI 同步索引）。
- **列表变更**：`applyListsFromMsg`、delete/import/rename、`list*.result` 更新 sel 后一律 `setHomeSelection(2|1, path|"")`。
- **导入/保存/停录**：`importScript.result` / `saveEditor.result` / `stopRecord.result` 选中 `msg.path` 并同步引擎。
- **设置**：`home.activeTab`（及可选 selected*Path）经 `fillSettings`/`collectSettings`/`setTab` 读写并 `saveSettings` 持久化。

F8 / CTA 目标 = 引擎当前 `selectedScript_` / `selectedRecording_` + `activeHomeTab_`。

### 定时任务

```json
{ "type":"openScheduledTasks" }
{ "type":"openScheduledTasks", "forceNative":1 }
→ { "type":"openScheduledTasks.result", "ok":true, "useHtml":true }
   或 { "ok":true, "native":true }

{ "type":"listScheduledTasks" }
{ "type":"saveScheduledTask", "update":0, "name":"…", "kind":0|1, "filePath":"…",
  "frequency":0-4, "status":0|1, "hour":8, "minute":0, "weekDays":31, "year":0, "month":0, "day":0 }
{ "type":"deleteScheduledTask", "id":"…" }
{ "type":"setScheduledTasksGlobalDisabled", "disabled":1 }
```

`frequency`：0 每小时（只比分/秒） / 1 每日 / 2 每周 / 3 自定义（一次） / 4 间隔（时分秒毫秒为**时长**；从本次保存或软件启动起计时，到期执行，关闭软件后重置）。

### 设置

```json
{ "type":"openSettingsData" }
→ { "ok":true, "settings":{ /* click/playback/other/ai/home */ } }

{ "type":"saveSettings", "settings":{ "closeToTray":true, "autoHideMainWindow":true, … } }
→ { "ok":true }
```

`autoHideMainWindow`：跑宏 / 连点 / 录制开始时隐藏**用户可见主窗**（headless 产品壳 = `webViewHostHwnd_` / `g_hwnd`；GDI = 引擎主窗）。结束或托盘「显示主窗口」走 `RestoreMainWindowForUser` / `RestoreMainWindowAfterRun`。headless 下引擎宿主窗始终 `SW_HIDE`，勿误 Show。已关闭到托盘（开始前不可见）时，结束后保持隐藏，避免与 `closeToTray` 互踩。

```json
{ "type":"restoreSettingsDefaults" }
→ { "ok":true, "settings":{ /* DefaultAppSettings 并已 SaveAppSettings */ } }
```

`window.close`：读 `other.closeToTray`——`true` 藏托盘；`false` 退出进程（对齐原生 GDI）。

### 录制优化

```json
{ "type":"openRecordingOptimize", "path":"…", "name":"…" }
→ { "ok":true, "useHtml":true, "path":"…", "name":"…" }

{ "type":"loadOptimizeRecording", "path":"…" }
→ { "ok":true, "recording":{ "path","name","durationSeconds","actions":[…] } }

{ "type":"applyOptimizeRecording", "path":"…", "scheme":0,
  "selected":[0,1], "protectKeyOps":1 }
{ "type":"applyOptimizeRecording", "path":"…", "scheme":1,
  "selected":[2,5], "waitValue":0.2,
  "waitFilter":0, "compareValue":0.5 }
{ "type":"applyOptimizeRecording", "path":"…", "scheme":2,
  "selected":[…], "waitCalculation":"sum|average|first|last|fixed",
  "mergeWaitValue":0.1 }
{ "type":"applyOptimizeRecording", "path":"…", "scheme":3,
  "selected":[…], "distanceThreshold":3, "compressWait":0.05 }
{ "type":"applyOptimizeRecording", "path":"…", "scheme":4,
  "selected":[…], "findTimeExpr":"0|-1|3" }
{ "type":"applyOptimizeRecording", "path":"…", "scheme":-1, "saveAsNew":1 }
→ { "ok":true, "recordings":[ /* listRecordings 同构 */ ],
    "converted":2, "skipped":1, "detail":"…" /* scheme4 可选 */ }
→ { "ok":false, "detail":"…" /* 非法选中 / 无匹配 wait / 相对位移保护 */ }
```

scheme：0 批量删除 / 1 等待调整 / 2 移动合并 / 3 移动压缩 / 4 转为找图 / **-1 无变换**（`saveAsNew:1` 另存）。

- **scheme0**：`protectKeyOps:1` 时保护「关键操作」（对齐原生 `IsKeyOperation`：除绝对/相对移动与等待外均保留）；未选中任何项返回错误。
- **scheme1**：仅改 `selected` 中的 Wait；`waitFilter` 0=全部 / 1&lt; / 2≤ / 3&gt; / 4≥ / 5= / 6≠（亦可传中文标签）；非「全部」时用 `compareValue`（秒）过滤，对齐原生 `ApplyWaitAdjust` + `WaitMatchesFilter`。
- **scheme2/3**：`selected` **未含关键操作**时，仍须一整段连续的 move/wait（原 `SelectionIsContiguousMoveWait`）。**含关键操作**时以已选关键动作为分割点，对每段已选的 move/wait 分别合并/压缩（等待时间按段计算）；未勾选的空洞也分段；含相对位移的段跳过、其它段照常。算法在 C++ `recopt`（对话框 `ApplyOptimizeAndSave` 与助手 `optimizeRecording` / `optimizeScript` 共用 `MergeAllKeySplit`），禁止 JS 重写变换。
- **scheme4**：result 含 `converted` / `skipped` / `detail`，UI `#optFindResult` 写转换统计。

`waitCalculation` 禁止后端 hardcode 为 sum。点「开始合并」等只改对话框工作副本，**不写原录制**；`saveAsNew:1` 才写入 `recordings/` 新文件。转找图后裁切/测试再调 `findImageCrop` / `findImageMatch`。

### 脚本包 IO（JSON / ZIP）

```json
{ "type":"importScript", "kind":"macro"|"recording" }
→ 文件对话框：*.zip;*.json
→ { "ok":true, "path":"…", "scripts":[…], "recordings":[…] }

{ "type":"exportScript", "path":"…" }
→ 无找图资源：另存 *.json（CopyFile）
→ 有找图/OCR 图等：另存 *.zip（`script.json` + 图片文件名，对齐原生 CreateZipFile）
→ { "ok":true, "skipped":0, "skippedFiles":["…"] }  // skipped>0 时 UI：导出成功，但有 N 个文件被跳过

{ "type":"renameScript", "path":"…", "name":"新名" }
→ 写回 scriptName 后若路径变化则 MoveFileW 到同目录「新名.json」；冲突返回明确错误
→ { "ok":true, "path":"…\\新名.json", "scripts":[…], "recordings":[…] }
```

ZIP 导入：读 `script.json` → 解压图片到 `images/` → 重映射 `imagePath` → `SaveScriptFileData`；录制导入会清窗口模式/`breakoutTimeSeconds`。导入后 shell `ReloadScriptsAndHotkeys`，Web 选中 `msg.path`。

### 引擎状态 / 关于 / 升级

```json
{ "type":"getEngineStatus" }
→ { "ok":true, "clicking":false, "running":true, "recording":false,
    "breakoutPaused":false,
    "executedSteps":12, "currentScript":"日常挂机", "runningMode":0|1|2 }
```

`runningMode`：0=默认 / 1=窗口模式 / 2=后台窗口；由引擎当前运行脚本的 `windowMode` 推导（非 Web 缓存）。停宏后复位为 0。

`breakoutPaused`：默认模式脱离暂停（真人键鼠打断后等待恢复）。Web `#statusPill` / `#runHud` 优先显示「脱离中…」；壳托盘 tip/icon 对齐原生「键鼠工坊-鼠标宏脱离中」；可选壳标题同步。

壳每 500ms `PushEngineStatusIfChanged`；运行中步数/`runningMode`/`breakoutPaused` 变化也会推送。HUD `#runHudMeta` 显示宏名 + 模式 + 已执行步数。

引擎用户可见提示（headless，E1.3）：

```json
{ "type":"engine.toast", "ok":true, "text":"…" }
```

Native → Web 推送；`ui/app.js` 调现有 `toast()`。无 Web 桥时回落 `MessageBoxW`（非 `QST_WEBVIEW_WITH_ENGINE`）。产品路径不再依赖 `prompt_modal` 显示失败信息。

```json
{ "type":"getAppBranding" }
→ { "ok":true, "branding":{
    "name","version","tagline","website","contact","qq","copyright" } }

{ "type":"checkUpgrade" }
→ { "ok":true, "stub":true, "detail":"当前版本未接入在线升级检查。" }
```

关于页用 branding 替换占位；**检查升级为诚实 stub**（`stub:true`），勿冒充「已是最新」。

驱动安装过程推送 `installDriver.progress`（`percent`/`step`/`status`）。OCR：**产品壳不再弹原生 `OcrInstallDialog`**；`qst_webview_shell` 后台 `RunOcrInstall`，经 `installOcr.progress` 推真实百分比到 Web `#ov-ocr`。

### OCR / 全局热键 / 录制范围

```json
{ "type":"installOcr", "repair":0 }
→ { "type":"installOcr.progress", "ok":true, "indeterminate":false, "percent":0, "status":"已就绪，点击安装开始下载安装…" }
→ { "type":"installOcr.progress", "ok":true, "indeterminate":false, "percent":0-100, "status":"…" }
→ { "type":"installOcr.result", "ok":true|false, "detail":"…" }
```

```json
{ "type":"captureGlobalHotkey" }
→ { "ok":true, "hotkeyText":"F8", "hotkeyVk":119, "hotkeyModifiers":0, "hotkeyHold":false }
```

捕获前后引擎调用 `BeginHotkeyCaptureRelease` / `EndHotkeyCaptureRelease`，临时注销正在编辑的键，避免重捕获已注册键误触发启停。

```json
{ "type":"setTypingHotkeysMuted", "muted":0|1 }
```

主壳 / 助手窗输入框（`input` / `textarea` / `contenteditable`）聚焦时静默**空闲启动**热键（卸掉键盘 `RegisterHotKey`，LL 放行不触发），避免搜索/对话时按到脚本热键。运行中仍可用热键停止。主壳与助手窗各一 bit，关助手窗会清助手 bit。

### 热键入口定案（B4）

| 角色 | UI | 捕获 |
|------|-----|------|
| 启停热键 | 主页 `#comboHotkey` | 自定义 → 原生 `HotkeyCapture`（`captureGlobalHotkey`） |
| 脚本热键 | `#ov-hotkey`（确认/清除）；**捕获中隐藏**，避免与原生模态双叠 | `captureScriptHotkey` → `HotkeyCapture` |
| 动作按键 | `#ov-action-key`（产品主路径） | Web `keydown` + `begin/endHotkeyCapture` |

主页 `#comboHotkey` 预设含 **鼠标左/中/右/侧键1/侧键2/空格键**（侧键与空格无长按）；自定义捕获走原生 `HotkeyCapture`。**禁止** Web `keydown` 覆盖全局/脚本热键并把 `hotkeyHold` 强制为 false。

{ "type":"captureScriptHotkey", "hotkeyText":"…", "hotkeyVk":0, "hotkeyModifiers":0, "hotkeyHold":0 }
{ "type":"testOcr", "mode":"test|offset", "ocrRegionByImage":0|1, "imagePath":"…", "searchX1":0, "searchY1":0, "searchX2":0, "searchY2":0,
  "matchThreshold":65, "imageScaleMin":1, "imageScaleMax":1, "ocrDigitsOnly":0, "searchFullScreen":0, "ocrSearchText":"", "ocrResultMode":0 }
→ test: { "ok":true, "mode":"test", "text":"识别到的文字" }
→ offset: { "ok":true, "mode":"offset", "offsetX":0, "offsetY":0 }
→ 或 `{ "ok":false, "needInstall":true }`
{ "type":"setRecorderCaptureScope", "scope":0|1 }
→ 持久化 `home.recorderCaptureScope` 并 `ReloadSettings` → `recorderSettings_.captureScope`
  录制时 `BeginRecordingScopeSession`：`0` 仅记录目标窗口树（从本软件点开始则锁定首个外部窗口）；`1` 全局。Web 录制页 `#recScope`。
{ "type":"showDebugWindow" }
→ { "type":"showDebugWindow.result", "ok":true }
```

启用「宏调试窗口」后：勾选即时 `showDebugWindow` → `ReloadSettings` → 壳打开**独立 WebView 顶层窗**（`ui/debug.html`，可拖出主壳、置顶/最小化/关闭）；引擎推送 `debugWindow.show` / `append` / `appendBatch` / `clear` / `hide`。主壳内 `#debugFloat` 仅无 bridge 的 HTML 预览兜底。Gdi.exe 仍用原生 `MacroDebugWindow`。

`applyOptimizeRecording` 成功后引擎 `ReloadScriptsAndHotkeys`（对齐原生 `LoadRecordings`）。

设置 `openSettingsData` / `saveSettings` 的 `playback` 含：`enablePlaybackCount`、`playbackCount`、`enablePlaybackInterval`、间隔秒、`enablePlaybackSpeed`、`playbackSpeed`(0.25~4)。**全局倍速只作用于录制页直接播放**：极简模式无视勾选（视为已启用），专业模式看勾选；与极简录制工具栏共用 `playbackSpeed`，专业录制页不再另放滑条。鼠标宏顶层不缩放。嵌套「运行录制回放」(`mousePlayback`) 只用动作自身 `playbackSpeed`（缺省 1），不叠加全局。`enableDebugOutputWindow`、`autoOutputKeyFunctionDebug`、`recordingClickCaptureEnabled`、`recordingClickCaptureHalfSize`、`foregroundInputBackend`(0/1/2)、`scheduledTaskConflictPolicy`(0=执行脚本优先/跳过 1=定时脚本优先/打断)、`scheduledTaskAutoResume`（false=上述跳过/打断不恢复；true 时 0=结束后再跑、1=插入后从原步骤继续）。`other` 另含 `closeToTray`、`playSoundOnStart`、`hideBottomRightTip`、`resolveImeConflict` 等（**`preferDirect2D` 在 Web 壳设置页隐藏**，磁盘值保留）。`home` 含 `activeTab`、`uiMode`(`simple`/`pro`)、可选 `selectedScriptPath` / `selectedRecordingPath`。

headless 下原生右下角 tip **禁用**（仅用 Web `#statusPill`，由 `hideBottomRightTip` 控制），避免双显。

### 仍强制原生的窗口

| 场景 | 原生 |
|------|------|
| 选区 / 截模板 / 找图匹配 | ScreenshotOverlay / MatchOverlay |
| 找图裁切写盘 | `FindImageCropRect`（UI 在 Web `#ov-crop`） |
| 准星 | CrosshairDragController |
| OCR 安装进度 | Web `#ov-ocr`（壳内 `RunOcrInstall`；非 OcrInstallDialog） |
| 全局/脚本热键捕获 | HotkeyCapture |
| 宏调试输出（enableDebugOutputWindow） | **独立 WebView 顶层窗**（产品）；Gdi 逃生舱仍 MacroDebugWindow |
| 驱动安装 | ShellExecute 提权 installer |

### 桌面工具 result

```json
{ "type":"window.clientSize", "ok":true, "clientW":1380, "clientH":960, "expectW":1380, "expectH":960, "match":true }

{ "type":"pickScreenRegion.result", "ok":true, "x1":…, "y1":…, "x2":…, "y2":… }
{ "type":"captureTemplateScreenshot.result", "ok":true, "imagePath":"images/template_….bmp", "resolvedPath":"…" }
{ "type":"readImageDataUrl.result", "ok":true, "path":"…", "dataUrl":"data:image/bmp;base64,…" }
{ "type":"resolveImagePath.result", "ok":true, "path":"…", "resolvedPath":"…" }

{ "type":"findImageMatch.result", "ok":true, "mode":"test|offset|region", "resolvedPath":"…",
  "offsetX":0, "offsetY":0, "regionValid":true, "searchX1":…, "found":true }
{ "type":"findImageCrop.result", "ok":true, "imagePath":"…", "offsetX":0, "offsetY":0 }
{ "type":"findImageCrop", "imagePath":"…", "offsetX":0, "offsetY":0,
  "cropX":10, "cropY":20, "cropW":100, "cropH":80 }
{ "type":"crosshairPick.result", "ok":true, "mode":"…", "pick":{ "x":0, "y":0,
  "windowTitle":"…", "windowClassName":"…", "processPath":"…" } }
{ "type":"installDriver.result", "ok":true|false, "kind":"interception|vhid",
  "uninstalled":true|false, "detail":"…", "probed":true|false, "suggestedBackend":0|1|2 }
{ "type":"queryVhidStatus.result", "ok":true, "hvciEnabled":true|false,
  "rebootPending":true|false, "driverReady":true|false,
  "installScriptPresent":true|false, "lastExitCode":-1|0|2|10|99 }
```

---

## 发版

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
```

禁止主路径 toast「待并入/演示」。
