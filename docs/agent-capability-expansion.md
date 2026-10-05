# AI 脚本助手能力扩展 — 差距审计与分期方案

> 状态：**仅审计，未改代码**（本文档除外）。对齐参考：Claude Code / Cursor Agent / Midscene.js /
> OpenAI Agents SDK / MCP。**本仓既有可复用件**（本次扩展的全部本钱）：
>
> - `ExecuteAiActionExecute()` —— `src/ai_action_service.cpp:2453`，宏侧 AI 动作执行的
>   observe → plan → act → verify 闭环（含 locate 管线、findImage 快路径、任务备忘、完成判定）
> - `AiActionHostHooks` —— 引擎在 `src/engine/engine_script_run.cpp`（约 4395 行起）接好的
>   截图 / 执行动作 / 观察比对 / 前视 回调
> - `src/mcp_server.*` —— 本产品**作为 MCP server** 的协议层（`McpHandleRequestLine` 纯函数，可自检）
> - `src/office_doc.*` —— `ReadOfficeDocument()` 只读办公文档
>
> **请确认 §6 分期后再开实现 PR**（与 `docs/vision-agent-gap-audit.md` 同一约定）。

---

## 0. 一个硬事实：助手的 34 个工具全是「产出物」型，没有一个是「动手」型

`BuildDefaultAgentTools()`（`src/agent_tools.cpp`）+ shell 组（`src/agent_shell.cpp`）实际注册：

| 组 | 工具 | 性质 |
|---|---|---|
| 脚本读写 | `listScripts` `readScript` `writeScript` `getScriptStats` `optimizeScript` `createMacroScript` `optimizeRecording` `deleteScriptFile` | 改**文件** |
| 动作构建 | `planScriptActions` `buildScriptActions` `submitMacroActions` `buildGetCursorPosAction` `buildAiTextAnalysisAction` `buildAiImageAnalysisAction` **`buildAiActionExecuteAction`** | 生成**JSON** |
| 定时任务 | `listScheduledTasks` `createScheduledTask` `updateScheduledTask` `deleteScheduledTask` | 改**配置** |
| 设置 | `listSettings` `updateSettings` | 改**配置** |
| 命令行 | `runAgentCommand` | 白名单**只读**（`git` 只读 / `MSBuild` 自检 / `SelfTest` / `where`） |
| 文件 | `listDirectory` `readAgentFile` `searchAgentFiles` `writeAgentFile` | 改**文件** |
| 剪贴板 | `copyAgentTextToClipboard` `pasteAgentClipboardText` | 改**剪贴板** |
| 网络 | `fetchWebPage` `webSearch` | 读**网** |
| 撤销 | `listAgentChanges` `revertAgentChange` | 回滚 |
| 知识 | `readScriptReference` `readAgentSkill` | 读**文档** |

**结论**：用户说「帮我把这件事做了」，助手能做的只有——**生成一个脚本**、改设置、写文件。
**它不能自己动手。** 最接近的 `buildAiActionExecuteAction` 名字有误导性：它只把 `aiActionExecute`
动作**构建进脚本 JSON**（`src/agent_tools.cpp:1931` 建 tool、`:1995` 填 `params["type"]`），
**并不执行**。

而宏侧的桌面执行闭环**早就存在且相当成熟**，只是**没有任何一条路径通向聊天助手**。
这就是「功能比较有限」的根因，也是本次扩展的全部空间 —— **不是重写一个 Agent，是把已有的那个接过来。**

---

## 1. 与主流 Agent 的差距表

| 域 | 主流做法（Claude Code / Cursor / Midscene） | 本仓助手现状 | 差距 |
|---|---|---|---|
| **动手能力** | 直接改文件 / 跑命令 / 操作界面 | 只能产出脚本 JSON | **P0** |
| **观察闭环** | 每步执行后强制观察新状态再规划 | 无（助手不看屏幕） | **P0** |
| 计划 / 待办 | `TodoWrite` 式显式清单，跨轮保活、可勾选 | `planScriptActions` 只规划**脚本动作树**，不是任务待办 | **P1** |
| 长任务 | 后台任务 + 可中断 + 进度回传 | 单轮同步，`AgentCore` 工具循环上限 10 轮 | **P1** |
| 子代理 | 派 subagent 干一件独立事，只回结论 | 无 | **P1** |
| **外部工具生态** | MCP client（接一切 server） | **已是 MCP server，但没有 client** | **P0** |
| 通用命令行 | 全权 shell（沙箱内） | 白名单 4 个程序、**只读** | P1 |
| 办公文档 | 直接读写 docx/xlsx/pptx | 只有 `readDocument`（只读，且**没注册给助手**） | **P0** |
| 记忆 | 跨会话项目记忆文件 | 有对话存储（`agent_conversation_store`），无「长期事实」层 | P2 |
| 权限 / 审批 | 危险动作前显式确认 | 无（因为没有危险动作） | P0（随动手能力一起上） |

---

## 2. 核心缺口：把执行闭环「接过来」，而不是重写

### 2.1 已有的东西长什么样

宏侧一次 `aiActionExecute` 的调用链（已核实）：

```
engine_script_run.cpp（约 4395 行起）
  ├─ 截屏 → capMap（屏幕↔上传图像素映射）
  ├─ 主模型非多模态时：ResolveVisionSubtaskModelName() 切识图子模型
  ├─ PrepareAiActionExecuteCore() → AgentCore（带全套工具）
  └─ ExecuteAiActionExecute(core, prompt, b64, w, h, …, &capMap, &agentHooks, rounds)
        └─ agentHooks: 截图 / 执行动作 / 观察比对 / 前视
```

`agentHooks` 里那套「执行完动作再截图比对、没变就不上传新图」的逻辑，
**正是主流 Agent 的 observe→act 环**，而且已经针对我们的场景调过（省 token、防死循环、
locate 失败硬拦乱快捷键 —— 见 `docs/vision-agent-gap-audit.md` §6–7）。

### 2.2 缺的只有「接口」，不是「引擎」

把 `ExecuteAiActionExecute` 暴露成助手的**一个工具**，助手立刻获得全部桌面能力：

```jsonc
// 提案：runDesktopTask —— 助手侧的「去做这件事」
{ "goal": "打开记事本写一份今日待办，保存到桌面",
  "withImage": true,
  "maxRounds": 12,
  "contextMode": 0 }
```

返回：`{ ok, routeKind, steps, lastObservation, error }`。

**关键设计决定（必须先定，否则会踩坑）：**

1. **模型不要换。** 助手当前模型（如 deepseek 文本模型）继续当**规划模型**；
   识图子任务走 `ResolveVisionSubtaskModelName()`（这条链路宏侧已经跑通，
   助手侧的 `ShouldAttachObserveImageToPlanner()` 也已经存在）——
   纯文本主模型收图会让网关挂起/超时，这条规则别推翻。
2. **`AiActionExecThinkingScope` 必须包住调用**（`src/agent_core.h:264`）。
   它现在不是「思考降档」开关，而是 §42 两条协议闸的入参 ——
   它区分「AI 动作执行」与「聊天助手」：**聊天助手的长回答是用户要的，不该被看门狗收束**。
   助手调 `runDesktopTask` 期间必须进这个作用域，否则看门狗会把长任务当闲聊收束掉。
3. **嵌套深度共用宏侧计数器**（`AiActionExecuteCanNestMore()` / `AiActionExecuteNestDepth()`）。
   助手 → runDesktopTask → 内部又调 aiActionExecute 是**两层**，别让熔断器各算一套。
4. **不能从 WebView 直接 SendInput**（`AGENTS.md` 硬规则）。执行必须回到引擎线程，
   走既有的原生链路。

### 2.3 配套要补的两个「低垂果实」

| 项 | 现状 | 做法 |
|---|---|---|
| `readDocument` 没给助手 | 实现于 `src/macro_execute_tools.cpp:3172`，**只注册给宏侧 AI 动作执行** | 直接 `tools.push_back(MakeReadDocumentTool())` —— 一行级改动，立刻让助手能读 xlsx/docx/pptx/pdf |
| `runAgentCommand` 太窄 | 白名单只有 `git`(只读) / `MSBuild` / `SelfTest` / `where` | 扩到「**办公/脚本类只读或幂等**」白名单（见 §4），写操作单独走审批 |

---

## 3. MCP：本产品**已经是 server**，缺的是 client

`src/mcp_server.h` 开头写得很清楚（2026-09-16 与用户确认的「方向 B 反向集成」）：

> 让**我们的原生链路成为 MCP 工具**，DSH / Claude / 其它 MCP 客户端挂上就能用。

现有 server 工具（`src/mcp_server.cpp`，已核实）：
`screenshot` `click` `move` `type_text` `key` `scroll` `cursor_position`
`list_windows` `activate_window` `list_ui_controls` `invoke_ui_control` `read_document`。

**对称的另一半是空的**：助手不能挂别人的 MCP server。补上 client 后解决两件事：

1. **消费任意第三方 MCP server**（**用户自己配**）—— 这才是「主流 Agent 生态」的门票；
2. 复用已有协议层：`McpHandleRequestLine` 是纯函数、有自检 —— **client 侧照同一套 JSON-RPC 写**，
   协议层可共用同一个自检 suite。

> ⚠ **原方案里的第 1 条是「消费 GenOffice MCP」——已删除**（2026-09-23 用户指出方向错了：
> 产品不能依赖别人的软件运行）。要接第三方工具就**用户自己写配置**，产品不主动探测。
> 办公文档能力改为**自研**，见 §6 的 Phase 2.2'。

> 注意：MCP client 会引入**外部进程**与**用户可配的命令行**，安全面比现在的白名单大得多。
> 见 §4。

---

## 4. 安全边界（Phase 1 的前置条件，不能后补）

现在助手之所以「安全」，只是因为**它没有手脚**。一旦接上执行闭环，以下必须同时到位：

| # | 规则 | 理由 |
|---|---|---|
| S1 | **危险动作前显式确认**（删除文件、发消息、提交表单、执行下载的 exe、注册表写） | 与 `SOUL.md`「对外动作谨慎」一致；主流 Agent 都有 |
| S2 | **执行期间可见**：助手面板常驻显示「正在做第 N 步 / 当前动作」，可一键中断 | 复用引擎既有 `stopFlag_` / `aiHttpAbort_` |
| S3 | **作用域隔离**：助手触发的桌面执行**不得**改用户正在编辑的脚本/设置，除非用户明确要求 | 防「聊个天把我脚本改了」 |
| S4 | **MCP 外部进程白名单**：用户显式添加的 server 才启，且默认 `--host 127.0.0.1`、不自动信任 | MCP 就是任意代码执行 |
| S5 | **撤销覆盖新增动作**：`listAgentChanges` / `revertAgentChange` 必须能回滚**桌面执行造成的文件改动** | 现有快照存 `AppDir()\agent_changes`，扩到执行产物 |
| S6 | **审批粒度可配**：保守（每步确认）/ 平衡（仅危险动作）/ 放手 | 让用户自己选风险 |

**落地现状（2026-09-23）**：

| # | 状态 | 实现 |
|---|---|---|
| S1 | ✅ | `DesktopTaskNeedsConfirm(goal)` + `confirmed` 二次调用；闸排在**写临时文件之前**（`AgentAssistantSelfTest::desktop_task_tool_wired` 钉住位置）。危险词中英分治：中文子串、英文**词边界** |
| S2 | ✅ | 执行期间每 2s 推一行 `sendAgentMessage.status`（JS 按 `live-status` 原地更新）；用户停止 → `CancelAgentMessage` → `RequestDesktopTaskCancel` → 工具轮询看到标志 → `StopScript()` |
| S3 | ✅ | 临时脚本落 `ScriptsDir()` 且**跑完即删**，不入用户脚本库；不动用户正在编辑的脚本/设置 |
| S4 | ✅ | 只有 `AppDir()\mcp_servers.json` 里**显式写下**的 server 才启动（文件不存在 ⇒ 一个进程都不起）；**不放行 HTTP 传输**（本机 stdio 即可，远程面太大）；工具名带 `mcp__<server>__` 前缀，出事一眼定位；server 名进路径前净化 |
| S5 | ⬜ | **未做**。`listAgentChanges`/`revertAgentChange` 目前覆盖助手自己的文件工具改动；桌面执行**通过脚本产物间接造成**的文件改动（如生成/改写 xlsx）**不在快照里**。要做需要「执行前后对目标目录取快照 + 差异」，是独立一件活 |
| S6 | ⬜ | **未做**。目前只有「危险动作确认」这一档（≈平衡模式）。保守/放手两档需要设置项 + 每步确认的 UI |

⇒ **S5/S6 是已知缺口**，别当成已实现。

---

## 5. 复用点清单（实现时的落点，避免另起炉灶）

| 要做的事 | 复用什么 | 落点 |
|---|---|---|
| 助手执行桌面任务 | `ExecuteAiActionExecute` | `src/ai_action_service.h:370` |
| 截图/执行/观察回调 | `AiActionHostHooks` | 引擎 `engine_script_run.cpp` 已有的 `agentHooks` 组装段 |
| 识图子模型路由 | `ResolveVisionSubtaskModelName` / `HasUsableVisionModel` | `src/agent_ai_actions.h:52` |
| 协议闸作用域 | `AiActionExecThinkingScope` | `src/agent_core.h:264` |
| 嵌套熔断 | `AiActionExecuteCanNestMore` / `AiActionExecuteNestDepth` | 宏侧 |
| 读办公文档 | `MakeReadDocumentTool` / `ReadOfficeDocument` | `src/macro_execute_tools.cpp:3172`、`src/office_doc.h:42` |
| MCP 协议 | `McpHandleRequestLine` 同款 JSON-RPC | `src/mcp_server.cpp` |
| 工具注册 | `BuildDefaultAgentTools()` | `src/agent_tools.cpp` |
| 工具说明书（给模型看） | `readAgentSkill` section 机制 | `src/agent_reference.cpp`（已有 scriptStrategy/reply/optimize/scheduledTasks/settings/conversation 六节） |

⚠ **跨编译单元取内部工具函数时的坑（踩过，C2668）**：`MakeReadDocumentTool()` 定义在
`macro_execute_tools.cpp` 的**匿名命名空间**里（内部链接）。在头文件里声明**同名**函数
会造出两个重载 ⇒ 调用点报 `C2668 对重载函数的调用不明确`。
正确做法是在 `macro_execute_tools.cpp` 末尾加**名字不同**的公开包装
（`MakeAgentReadDocumentTool()`），头文件声明与调用点同步改名。**在本仓加同类包装时勿再用同名。**

⚠ **改 `skills/agent/*.md` 必须同步 `src/ai_action_router.cpp` 的内嵌文本**（`AGENTS.md` 硬规则）。
⚠ 助手新增 section（如 `desktop`）要同时进 `agent_reference.cpp` 与产品 `skills/agent/`。

---

## 6. 分期建议

```text
Phase 1（动手能力，P0）                              —— 用户确认，已落地（1.2 未做）
  1.1  ✅ runDesktopTask 工具（复用 ExecuteAiActionExecute + agentHooks + 作用域/熔断）
       实现：src/agent_desktop_task.*（纯逻辑）+ agent_tools.cpp（工具装配）。
       ★走**正常回放链路**（临时脚本 + RequestRunScriptAsync），不直调执行函数 ——
         agentHooks 只在回放上下文里组装，别处重组必然漂移。
       自检：AgentDesktopTaskSelfTest 19/19。
  1.2  ⬜ observeScreen 工具（只观察不动作：截图 + 可选识图结论）
       —— **未做**。理由：`zoom`/`computer(action=screenshot)`/`locateAndClick` 已经是
          「基于当前观察帧」的观察工具，且都进了助手工具表；再加一个「只截图」的入口
          与它们职责重叠。真正缺的是「看**盘上**的图」，那条已由 MCP 的
          `[[AGENT_IMG:…]]` 通路补上（见 §6.1）。如仍要独立入口，按 P1 排。
  1.3  ✅ readDocument 注册给助手（`MakeAgentReadDocumentTool()`）
  1.4  ✅ S1 安全边界（危险目标确认闸：`DesktopTaskNeedsConfirm` + `confirmed` 二次调用）；
       ✅ S2 步骤可见可中断（执行期间每 2s 推一行 `sendAgentMessage.status`；用户停止 →
          `RequestDesktopTaskCancel`）；✅ S3 作用域隔离：临时脚本落 `scripts\`（引擎只认那里）、
          **不进脚本库列表**（`ListJsonFiles` / `ListScriptsInDir` 两处出口用
          `IsAgentTaskTempFileName` 过滤）、跑完即删、启动清扫残留
          （`SweepStaleAgentTaskTempScripts`，只删「创建它的进程已不在」的）。
          ⚠ **落地后自查发现原写「不入用户脚本库」是未经验证的、且当时并不成立**：
          `EnumerateScriptJsonFiles` / `ListScriptsInDir` 都不做任何名字过滤，
          所以崩溃/强杀留下的 `_agent_task_*.json` 会出现在用户脚本库里。
          已按「列表出口过滤 + 启动清扫」两处补齐，并加了 2 条自检钉住
          （`temp_script_name_predicate` / `sweep_stale_temp_scripts`）。
          ⚠ 注意**不能**去改通用枚举或 `ResolveLibraryScriptPath` ——
          引擎正是靠后者才找得到这个临时脚本。
  验收：⚠ **需真机人工验**（「打开记事本写待办并保存」这类）。自动化只能覆盖到
        「工具已注册 + 危险目标在写临时文件之前被拦」（AgentAssistantSelfTest 已钉）。

Phase 2（生态，P0–P1）                               —— 用户确认；**2.2/2.3 已推翻重做**
  2.1  ✅ MCP client（stdio）：src/agent_mcp.*，配置 `AppDir()\mcp_servers.json`，
       进程管理 + 懒加载 + 工具并入助手工具表（`mcp__<server>__<tool>`）。
       自检：AgentMcpSelfTest 29/29（含 5 条真子进程端到端，exe 自带 `--fake-mcp-server`）。
  2.2  ❌→ 原「自动接入 GenOffice MCP」**已删除**（用户 2026-09-23 指出方向错了）。
       原方案：`DetectGenOfficeCli` 探测 → 自动补一条 MCP 配置；`runAgentCommand` 给
       `genoffice` 开白名单特例。**问题**：那会让产品在用户不知情的情况下把「写 Office 文件」
       这项能力**外包**给第三方 —— 对方没装就能力缺失、版本不同就行为不同、它的子命令清单
       还要跟着对方发版维护。用户要的是**借鉴它的做法与思路**，不是依赖它的产物。
       已删：`DetectGenOfficeCli` / `MakeGenOfficeMcpServerConfig` / 自动补配置 /
       `genoffice` 白名单特例 + `kOfficeCommandTimeoutMs` / Skill 与内嵌文本里的推荐路线。
       保留：通用 MCP 客户端（厂商中立、用户显式配置的扩展点，不是依赖）。
       回归哨兵：`AgentAssistantSelfTest` 的 `shell_rejects_third_party`（钉「白名单里没有
       任何第三方特例」）。
  2.2' ⬜ **自研办公文档能力**（正确路线，进行中）
       —— 借鉴 GenOffice 的三条思路，在**我们自己**的引擎里实现：
       ① 工具层不调模型；② **字节保留式编辑**（只重写改动的 part ⇒ 公式/图表/样式存活）；
       ③ 产物可验收（读回做确定性验证）。
       已落地地基：`src/ooxml/`
         · `inflate` —— 自研 **RFC 1951** 解压（stored / 固定 / **动态** Huffman）
         · `zip_archive` —— zip 读写，★**未改动条目原样搬运**（字节保留的落点）
         · `xlsx_doc` —— xlsx 读写：表名解析 / 单元格读 / **外科手术式**单元格写 / TSV 导出
       自检 `OoxmlSelfTest` **24/24**，只链 `qst_utils`（不依赖 Office / 第三方库 / Python）。
       待做：docx/pptx；以及把 xlsx 层接进助手工具表（`readDocument` 原生化 / 新增写工具）。
       ⚠ 与现有 `ooxml` 读取引擎的关系：那个是**起 PowerShell 脚本**（`.NET ZipFile`），
         能读不能写、每次起进程；新层是原生的，读更快、且**补上了写入**这个真缺口。
       ⚠ 已知边界（写在 `xlsx_doc.h` 里）：不更新 `<dimension>`；公式不算值（缓存值由调用方给）。
  2.3  ❌ 已随 2.2 一起删除（白名单不再有任何第三方特例）。
  验收：⚠ **需真机人工验**：自研文档层可用后，改一个**带图表/公式**的 xlsx 并确认图表公式原样存活。
        自动化覆盖：协议与契约（工具解析、白名单拒绝路径、图片落盘+标记、进程生命周期）
        + 容器层（deflate 解压、字节保留、Zip64 拒绝）。

Phase 3（长任务，P1）
  3.1  ⬜ 任务待办层（显式 todo 列表，跨轮保活，UI 可见）
  3.2  ⬜ 后台长任务（不阻塞对话，进度回传）
  3.3  ⬜ 子代理（派一件事、只回结论）
  验收：一个跨 20+ 步的任务能跑完且不丢上下文；关掉面板任务仍在跑。

Phase 4（体验，P2）
  4.1  ⬜ 项目记忆层（跨会话长期事实）
  4.2  ✅ 助手侧 Skill 目录 —— `readAgentSkill` 的 catalog + `section=desktop`
       （产品文件 `skills/agent/desktop.md`，内嵌兜底 `kSkillDesktop`）。
```

### 6.1 落地时发现并修掉的一个前提错误

原方案把「`genoffice render` → PNG → 模型看图」当成**天然成立**的验收闭环。
落地时查通路才发现**不成立**：助手侧没有任何「把盘上图片塞进对话」的工具
（`aiImageAnalysis` 只吃**实时截屏**；`zoom`/`computer`/`locateAndClick` 都基于当前观察帧；
`observePage` 是 Edge 可访问性树）。

但宿主**本来就有**一条多模态通路：工具结果文本里的 `[[AGENT_IMG:<绝对路径>]]`
会被 `AgentExtractImageMarkers()` 摘出、再由 `AgentBuildImageParts()` 编码成
`image_url` parts 送进**下一轮**请求（`zoom` 走的就是它）。
缺口只在「没人把 MCP 工具结果里的图片接到这条路上」——原本只留一句占位说明。

⇒ 已在 `McpExtractToolResult()` 补上：image content 的 base64 解码落盘到
`AppDir()\agent_mcp_images\<server>\` + 文本里补标记。闭环是**接上**的，不是天然有的。
详见 `docs/genoffice-integration-eval.md` 的「⚠ 纠正」小节。

**建议先做 Phase 1 + 2.2**：这两块是用户感知最强、复用度最高、且几乎不引入新架构的。

---

## 7. 确认状态

- 2026-09-23：用户回复 **`确认 Phase 1+2`** ⇒ 已按上面的分期落地（Phase 1 除 1.2 外全部 ✅、
  Phase 2 全部 ✅）。落地后的实测结论与偏差见 §6.1 与
  `docs/genoffice-integration-eval.md` §7。
- Phase 3 / Phase 4.1 仍未做（P1–P2），需要时再确认。

---

## 附：本次审计已核实的事实（供复核）

| 结论 | 证据 |
|---|---|
| 助手工具共 34 个，全为「产出物」型 | `BuildDefaultAgentTools()` 函数体；`src/agent_shell.cpp` 8 个 shell 工具 |
| `buildAiActionExecuteAction` 只构建不执行 | `src/agent_tools.cpp:1931`（建 tool）、`:1995`（`params["type"] = "aiActionExecute"`） |
| 执行闭环在宏侧已完整 | `ExecuteAiActionExecute` @ `src/ai_action_service.cpp:2453`；hooks 接线 @ `src/engine/engine_script_run.cpp` ~4395 |
| 助手**无**任何桌面执行工具 | 34 个工具清单里无执行类；`runAgentCommand` 白名单仅 git 只读/MSBuild/SelfTest/where |
| `readDocument` 存在但未注册给助手 | 实现 @ `src/macro_execute_tools.cpp:3172`；`BuildDefaultAgentTools()` 里没有 |
| 本产品已是 MCP server | `src/mcp_server.h` 定位注释；`src/mcp_server.cpp` 12 个工具 |
| 本产品**无** MCP client | `src/` 下无 client 实现 |
| 助手代码规模 | `src/agent_*.cpp/h` 合计约 12,400 行 |
