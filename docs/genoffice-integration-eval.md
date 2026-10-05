# GenOffice 接入评估 — 能否用于「AI 动作执行」的办公逻辑

> 状态：**仅评估，未改代码**。评估对象：`github.com/genspark-ai/genoffice`（官网 `genoffice.ai`）。
> 关联文档：`docs/agent-capability-expansion.md`（助手能力扩展）。

## ⚠⚠ 结论修正（2026-09-23，用户指出后）

**本文档最初的结论「能派上用场，用法是『当外部工具调』」被采纳并落地成了产品代码
（自动探测 GenOffice、自动接入 MCP、给它的 CLI 开命令白名单）—— 这个方向是错的，已全部回滚。**

用户的原话：「**我们的软件怎么能依赖别人的软件运行呢？我是让你参考 GenOffice 的这个
AI+办公的操作方式和思路**」。

复盘：我把「**参考它的思路**」做成了「**依赖它的产物**」。区别在于——

| | 依赖（做错了） | 借鉴（正确） |
|---|---|---|
| 形态 | 探测到就自动接入，没装就能力缺失 | 自己实现，不探测任何第三方 |
| 后果 | 版本不同则行为不同；它的子命令清单要跟着对方发版维护；用户不知情 | 能力始终在，行为可预期 |
| 许可 | 仍是外部依赖，只是「免费」 | 只借**设计思想**，不沾代码与二进制 |

**已经删掉的**：`DetectGenOfficeCli()`、`MakeGenOfficeMcpServerConfig()`、
`EnsureMcpLoadedLocked()` 里的自动补配置、`runAgentCommand` 的 `genoffice` 白名单特例
（连同 `kOfficeCommandTimeoutMs`）、Skill/内嵌文本里的「GenOffice 路线」推荐。
**保留的**：通用 MCP 客户端（`mcp_servers.json`）—— 那是**厂商中立、用户显式配置**的扩展点，
不是依赖：一个都不写就一个进程都不起。

**真正要借的三条思路**（与具体项目无关，已落到 `src/ooxml/`）：
1. **工具层不调模型** —— 文档读写是确定性的，模型只负责编排与决策。
2. **字节保留式编辑** —— 只重写改动的 part，其余原样搬运 ⇒ 公式/图表/样式存活。
3. **产物可验收** —— 读回做确定性验证；必要时渲染成图给模型看。

下文 §1~§7 保留作为**技术调研记录**（GenOffice 的实现思路、许可证边界、命令面、
以及落地时实测出的那些坑仍然有参考价值），但**不要**再据此把 GenOffice 接进产品。

---

> 原始结论（**已被上面推翻，仅存档**）：
> ~~能派上用场，但用法是「当外部工具调」，不是「拿代码来改」。~~

---

## 0. 先核实：这个仓库是真的，信息也对得上

| 项 | 核实结果 | 来源 |
|---|---|---|
| 仓库存在 | ✅ `genspark-ai/genoffice` | 仓库页面 |
| 许可证 | ✅ **Apache-2.0**（根 `LICENSE`）；⚠ **`ee/` 目录例外**，走 GenOffice Enterprise License | 根 LICENSE + 仓库结构 |
| 定位 | 「Free, open-source AI Office suite: Docs、Sheets、Slides、PDF、Markdown、HTML + 内置 AI agent」 | 仓库描述 |
| 活跃度 | ✅ **2026-09-22** 仍有提交（`Sync snapshot (2026-09-22) (#757)`）；首次公开发布 2026-08-02 | 提交历史 |
| 技术栈 | 7 个 Electron 应用共享**纯 TypeScript 引擎** + 处理 `.xlsx` 的 **Rust sidecar**；编辑方式为**字节保留**（只对改动的块打窄补丁） | README |
| CLI 包 | `@genoffice/cli` **v0.10.0**，`"license": "Apache-2.0"`，**`"private": true`** | `packages/cli/package.json` |
| 是否发 npm | ❌ **不发**（`private: true`），**随桌面应用打包** | 同上 + CLI README |

> **`"private": true` 这一条是决定性事实**：它不是可以 `npm i` 的库，
> 没有可供版本锁定的发布产物。**「拿代码来集成」这条路从许可与工程两个角度看都不通。**

---

## 1. 它能给我们什么：逐项对照我们现在的办公链路

我们现在的办公读写（`skills/agent/office.md` + `src/office_doc.h`，已核实）：

- **读**：`ReadOfficeDocument()`，引擎 `excel-com` / `ooxml` / `pdftotext` / `pdf-render` / `text`
- **写**：**Excel / Word / PowerPoint COM**（`New-Object -ComObject Excel.Application` 那套配方 A–D）
- **硬约束**：COM 路线**必须装 Office**；没装 Office 时**只能读、不能写**

对照表：

| 要做什么 | 我们现状 | GenOffice 能给 | 增量 |
|---|---|---|---|
| 读 xlsx/docx/pptx/pdf | `readDocument` ✅（纯文本/制表符） | `docs read --json` / `sheet read --json`（带 `--range` / `--formats`） | **结构化数据**，不是文本糊 |
| 写真 xlsx | Excel COM（**必须装 Office**） | `create --type xlsx --from data.csv` / `sheet apply --cells` | **不装 Office 也能写** |
| 写 docx | Word COM（必须装 Office） | `create --type docx --from report.md` | 同上 |
| 生成 pptx | PowerPoint COM（必须装 Office） | `create --type pptx`（含 `deck_start/page/build` 流程） | 同上 |
| **PDF → docx/pptx** | ❌ 不支持 | `convert scan.pdf --to docx`（**设备本地**） | **新能力** |
| docx → md / html | ❌ | `convert report.docx --to md` | **新能力** |
| xlsx → csv | 只能 COM 另存 | `convert book.xlsx --to csv` | 更轻 |
| 改既有 xlsx 单元格 | COM 打开→改→存（**整文件重写**） | `sheet apply --cells`（**字节保留**，公式/图表/透视表原样存活） | **不破坏文件** |
| **把文档渲染成 PNG** | ❌ | `render report.docx --out shots/ --scale 2` | **★ 最关键** |

### ★ 最关键的一条：`render` → PNG 补上了办公任务的**验收闭环**

我们的 AI 动作执行是靠**截屏观察**来验证「这一步生效了没有」（`onObserveScreen`）。
但一个办公任务的产物是**磁盘上的 .docx**，截屏看不见它 —— 于是
「写完了吗？写对了吗？」**只能靠模型自己声称**。

`genoffice render <file> --out shots/` 把文档渲成图片 ⇒ **可以喂给识图模型** ⇒
办公任务第一次有了和「点击后截图比对」同等级别的验收手段。

这条比「不装 Office 也能写」价值更高：**写错而不知道，比写不出来更糟**（与
`docs/vision-agent-gap-audit.md` 里「假成功回执比直接失败糟得多」同一个道理）。

#### ⚠ 纠正（2026-09-23，落地时实测后修正）

上面这段在写的时候把闭环当成了**自动成立**的，**不对**。落地时查了一遍通路，实际是：

1. 助手侧**没有任何**「把任意图片文件塞进对话」的工具：
   - `aiImageAnalysis` 只吃**实时截屏**（可选 `aiTargetImagePath` 是「找图锚点」，
     不是「要分析的那张图」）⇒ 喂不了磁盘上的 PNG；
   - `computer(action=screenshot)` / `zoom` / `locateAndClick` 全部基于**当前观察帧**；
   - `observePage` 是 Edge 可访问性树，跟图片无关。
2. **但宿主本来就有一条多模态通路**：工具结果文本里的 `[[AGENT_IMG:<绝对路径>]]`
   会被 `AgentExtractImageMarkers()` 摘出来，再由 `AgentBuildImageParts()`
   编码成 `image_url` parts 塞进**下一轮**请求（`src/agent_core.cpp:1966/2018`）。
   `zoom` 就是走这条路的。
3. 缺口在**没人把 MCP 工具结果里的图片接到这条路上**：`content[].type=="image"`
   原本只留一句「非文本内容」占位说明 ⇒ **图被丢掉了**。

**所以闭环是「接上」的，不是「天然有」的**：`McpExtractToolResult()` 现在把
image content 的 base64 解码落盘到 `AppDir()\agent_mcp_images\<server>\`，
并在文本里补 `[[AGENT_IMG:…]]` 标记。已由 `AgentMcpSelfTest` 的
`extract_tool_image_persists` 钉住（扩展名必须是宿主认得的位图格式、
标记路径必须等于落盘路径、文件字节必须等于解码结果、原文本不许丢）。

**同时补了一条「诚实降级」**：宿主认不出的图片格式（如 `image/svg+xml`）**不许**
落盘成 `.png` 假装能看图，也**不许**悄悄丢 —— 要留一句说清格式的话
（`extract_tool_unknown_image_mime_is_honest`）。

**仍未闭环的部分**：走命令行入口（`runAgentCommand("genoffice render …")`）时 PNG
只落在 `--out` 目录里，模型**看不到** —— 那是给用户看的。要模型看图必须走 MCP 工具入口。
Skill 里已把这条差异写明（`skills/agent/office.md` §2.5.4）。

### 其余可借的设计（比代码更值钱）

| 设计 | 它怎么做 | 我们的对应问题 |
|---|---|---|
| **`--dry-run` + `check` + `warnings[]`** | `docs check` / `slides check` / `sheet check` 先校验；`warnings[]` 对「成功但有风险」的结果给告警，不判失败 | 我们的 `AiLocateVerdict`（Accept/Suspect/Refine）已有同款思路，但**办公写操作完全没有「先校验」这一步** |
| **单一注册表、三个面（CLI / MCP / 应用内）** | `src/registry.ts` 是唯一分发点，参数从命令自己的选项表取 ⇒ **两面不可能漂移** | 我们的 `AGENTS.md` 明写「抄两份必然漂移」（Skill 文本要同步 `ai_action_router.cpp` 内嵌副本）—— **这正是它解决的问题** |
| **`guide <domain> --json`** | 自描述的操作目录（含 schema 与 fingerprint），模型按需取，不是把全部说明塞进 prompt | 我们的 `readAgentSkill` 分 section 是同一思路，但内容是**手写文本**，会与实现漂移 |
| **审计日志** | 每条命令追加一行 JSON 到 `~/.genoffice/cli-audit.jsonl`，2MB 轮转 | 我们的助手缺「执行审计」；Phase 1 的 S2/S6 可以照抄这个形态 |
| **`No model call happens inside genoffice`** | 它**不含任何模型调用** —— 是纯文档引擎，云端命令（`search`/`image`/`media`）才出网 | 定位清晰：它**不抢我们的 AI**，只当**文档的手**。不会出现「两套 Agent 打架」 |

---

## 2. 许可证边界（本项目闭源商业分发，必须逐条看）

| 情形 | 义务 | 结论 |
|---|---|---|
| **只调 `genoffice` 可执行文件**（进程边界） | 无分发 ⇒ **无任何义务**（连 NOTICE 都不用带） | ✅ **推荐** |
| 把 CLI 打进我们的安装包 | 需带 Apache-2.0 NOTICE、保留许可文本；且要捆 Electron/Node 运行时 | ❌ 不推荐 |
| 复制/移植其源码（`packages/` 下） | 需保留版权与许可声明；衍生作品同样 Apache-2.0 | ⚠ 可行但没必要 |
| **复制 `ee/` 下任何内容** | **禁止** —— `ee/` 是 GenOffice Enterprise License，不是 Apache-2.0 | ⛔ **红线** |

补充：

- Apache-2.0 是**宽松许可**，无 copyleft ⇒ 与闭源商业分发**兼容**（对照
  `docs/local-detection-feasibility.md` §11 里 AGPL/GPL 全出局的结论，GenOffice 不在那一类）。
- ⚠ 它的运行时依赖是 MIT / Apache-2.0 / BSD-3-Clause / OFL —— **仅在我们捆绑时才需要关心**。
- ⚠ **不要**因为它叫「开源 AI Office」就以为可以整仓搬运：`ee/` 边界 + `private: true`
  两条加起来，**最省事也最安全的用法就是「不碰代码，只调命令」**。

---

## 3. 三种接入方式对比

| 方案 | 做法 | 优点 | 缺点 | 建议 |
|---|---|---|---|---|
| **A. 调 CLI** | 检测到 `genoffice` 就把它加进 `runAgentCommand` 白名单；`skills/agent/office.md` 增一节「GenOffice 配方」；检测不到就回退现有 COM | 零新运行时依赖、零许可义务、改动最小 | 只有文本输出；要自己读 PNG 文件 | ✅ **Phase 2.3 先做** |
| **B. 走 MCP client** | 助手的 MCP client 挂 `genoffice mcp`（stdio），工具并入助手工具表 | **`render` 直接以 image content 返回 PNG** ⇒ 助手当场能「看」文档；工具 schema 自动同步（它一个动词一个工具） | 要先有 MCP client（Phase 2.1），且引入外部进程 | ✅ **Phase 2.2 做，收益最大** |
| **C. 只借设计** | 采纳：字节保留式改动、`dry-run + check + warnings[]`、单一注册表三面同步、`guide` 式自描述目录 | 零风险、立刻可做、不依赖用户装任何东西 | 不直接增加功能 | ✅ **立即采纳** |
| D. 捆绑 / 移植 | 把 CLI 或源码打进我们的包 | — | Electron 体积、`private: true` 无发布产物可锁版本、`ee/` 边界、长期维护成本 | ⛔ **不做** |

**推荐组合：A + B + C，永不捆绑。**

---

## 4. 落地清单（对应 `agent-capability-expansion.md` 的 Phase 2）

```text
2.2  MCP client + 接入 genoffice mcp
     · 检测顺序：PATH 上的 genoffice → 注册表/默认安装目录 \resources\cli\genoffice.cmd
       （⚠ 它的 Windows 安装器写的是**用户 PATH**，我们进程内的 PATH 可能没刷新 ⇒ 必须直接探目录）
     · 能力探测只用 `genoffice capabilities --json` 与 `genoffice guide <domain> --json`，
       **绝不解析人类可读输出**（它年轻，输出格式会变）
     · 调用一律带 --json，按 {status, command, summary, output_path?, warnings?, detail?} 读
     · warnings[] 非空 ⇒ 在助手面板显式展示（不要静默吞掉）
     · 退出码 4（app not available）⇒ 降级到现有 COM 路线并告知用户

2.3  CLI 白名单 + office skill 增节
     · runAgentCommand 放行 genoffice 的**只读/生成**子命令（info/read/check/render/capabilities/guide）
     · 写操作（docs apply / sheet apply / slides apply / convert --out / create）走 Phase 1 的审批

C    设计采纳（不依赖用户装 GenOffice）
     · 办公写操作加「先 check 再 apply」两段式，失败/告警显式回报
     · 产物校验：写完文档后用 render（有 GenOffice）或回读（无）确认，不靠模型自称
     · 单一注册表：助手工具表 / MCP server 工具表 / Skill 文本**同一来源生成**，禁止手抄第二份
```

### 验收建议

1. **不装 Office 的机器**上，让助手生成一份真 `.xlsx`（带公式）与一份 `.pptx`，用 Excel/WPS 打开无告警。
2. 改一份**既有**带图表与公式的 xlsx 的某个单元格 ⇒ **图表与公式必须原样存活**（这是字节保留的判据；
   COM 路线做不到，A/B 回退到 COM 应能看出差别）。
3. 办公任务跑完后，**助手能指出产物哪里不对**，而不是无条件说「已完成」。
   ⚠ 两条手段不要混为一谈：
   - **文本验收（可靠、便宜、已可用）**：`sheet check` / `docs check` / `slides audit --json`
     ⇒ 公式错误、引用不存在的表、TOC 过期、标题跳级、出界/溢出/重叠，且 `detail` 带可重试的事实。
     **这条不依赖识图模型，也不依赖「图能不能送到模型眼前」。**
   - **视觉验收（走 MCP 工具入口才成立）**：`render` / `slides render` 的 PNG 作为附图送进下一轮。
     前提：① 走 **MCP 工具**入口（命令行入口的 PNG 只落盘，模型看不到）；
     ② 当前模型**支持识图**（`ModelSupportsVision`，否则降级成一行路径文字）。
4. 把 `genoffice` 从 PATH 移除后，全部路线**自动降级到 COM 且不报错**（GenOffice 必须是纯可选依赖）。

---

## 5. 风险与未核实项（诚实标注）

| 项 | 说明 |
|---|---|
| ⚠ 它是**独立应用**，用户得自己装（Electron，体积不小） | 必须**严格可选**、自动探测、检测不到静默降级。**不能变成我们的硬依赖** |
| ⚠ 项目很年轻 | 首次公开发布 2026-08-02，到本文档 2026-09-23 仅约 7 周；**CLI 接口会变** ⇒ 只吃 `--json`，靠 `capabilities`/`guide` 做能力探测 |
| ⚠ 无发布产物可锁版本 | `private: true` ⇒ 无法在 package.json 里钉版本；只能「探测 + 按能力用」 |
| ❓ 未实测 | Windows 默认安装目录的确切路径；`--json` envelope 的字段是否跨版本稳定；`render` 是否在**没装 Office** 的机器上也能工作（README 称本地优先，**但我没实测，不当事实**） |
| ✅ 已核实（2026-09-23 落地时） | `genoffice mcp` 的 stdio 握手细节 —— **不需要真机跑 GenOffice 也能定**：它就是一个标准的 MCP stdio server（每行一个 JSON-RPC 2.0，`initialize` → `notifications/initialized` → `tools/list` → `tools/call`，协议版本 `2024-11-05`）。客户端已实现并用自建假 server 端到端验证（`AgentMcpSelfTest`）。 |
| ✅ 已核实（2026-09-23 落地时） | **CLI 命令面**（权威来源 `packages/cli/src/cli.ts` 的 `defaultRegistry()` + 各 `commands/*.ts` 的 switch 分支）：顶层 `info/convert/create/slides/sheet/docs/render/guide/search/image/media/open/selection/capabilities/install/mcp/skill`；`docs`=read\|check\|apply、`sheet`=read\|check\|apply、`slides`=read\|check\|audit\|apply\|replace\|render、`skill`=install\|list\|path。⚠ 之前我写过一个不存在的 `docs selection` —— `selection` 是**顶层**命令，已修正。 |
| ✅ 已核实（2026-09-23 落地时） | 退出码 `0` ok / `1` 用法 / `2` 文件 / `3` 转换失败 / `4` 应用不可用；`--json` 信封 `{status, command, summary, output_path?, warnings?, detail?}`；错误 `{status:"error", code, error, message, suggestion?, detail?}`，且 `detail` 里带**可重试的事实**（合法范围、可用 id、表名、用法行） |
| ⚠ 本机实测（Win32） | **`CreateProcessW` 能直接跑 `.cmd`，跑不了无扩展名脚本**（WinError 2）。它的 Windows 目录里 `genoffice.cmd`（给 cmd/PowerShell）与无扩展名 `genoffice`（给 Git Bash）**并存** ⇒ 探测必须按扩展名过滤 |
| ⚠ `ee/` 目录 | **禁止复制**。若将来有人提议「顺便把企业版功能抄过来」，这条是红线 |

---

## 6. 一句话结论

**GenOffice 对我们的价值不是「一套代码」，而是「一只不用装 Office 的手 + 一双能看文档产物的眼睛」** ——
用 **MCP / CLI 当外部工具调**（方案 A+B），**顺便把它的三处工程范式借过来**（方案 C，零风险），
**永远不捆绑、永远不碰 `ee/`**。

其中**收益最高、最容易被忽略的一条是 `render` → PNG**：它把办公任务从
「模型声称做完了」变成「模型真的看到了做出来的东西」——
⚠ 但这条**必须自己把图接到多模态通路上**才成立（见 §1 的「⚠ 纠正」）。

---

## 7. 落地实现的两条坑（2026-09-23 实测，写下来免得下次重踩）

### 7.1 「裸名」不能直接交给 `CreateProcessW`

白名单只放行 PATH 上的**裸名**（`genoffice` / `genoffice.cmd`）—— 这是对的安全姿态
（带路径就等于「任意 exe 都能跑」）。但**裸名不能直接拿去 CreateProcess**：

| 命令行 | 结果 |
|---|---|
| `go_tool`（PATH 上只有 `go_tool.cmd`） | ❌ `WinError 2 系统找不到指定的文件` |
| `go_tool.cmd` | ✅ 正常执行，参数与退出码都正确传递 |
| `C:\...\Program Files\...\go_tool.cmd`（带空格） | ✅ 正常（`CreateProcessW` 对 `.cmd` 有特殊处理，会代起 cmd.exe） |

原因：`CreateProcessW` 对**无扩展名**的名字只会补 `.exe`，**不会**像 cmd/PowerShell 那样按
`PATHEXT` 去找 `.cmd`。而 GenOffice 在 Windows 上提供的正是 **`genoffice.cmd`**。

⇒ 实现：白名单仍然只接受裸名，但**由我们自己在 PATH 上解析出绝对路径**再交给 CreateProcess
（`qst::agent::DetectGenOfficeCli`）。路径来自**我们的搜索**、不是模型给的字符串 ⇒ 安全性不变。

⚠ 报错形态很难联想：「进程启动失败，错误码 2」看起来像环境问题。

### 7.2 `runAgentCommand` 的 60s 硬超时对文档生成太紧

`kCommandTimeoutMs = 60000` 是「别让助手线程被卡死的命令拖住」的上限，对 `git status` 够用；
但 GenOffice 的 `convert` / `render` / `create` 要**启动它自己的渲染器**
（其 CLI README：转换会让 app 以 hidden 方式起来）⇒ 60s 实测不够。

⇒ 实现：genoffice 单独走 `kOfficeCommandTimeoutMs = 300000`。
顺手修了一个既有隐患：原来用 32 位 `GetTickCount()` + `deadline > now` 比较，
在 **49.7 天回绕点附近会立刻误判超时**、命令被无故杀掉；已改 `GetTickCount64()` + 显式 startTick。

### 7.3 图片落盘目录**只增不减** —— 必须自己清理

`render` 的 PNG 会被落盘到 `AppDir()\agent_mcp_images\<server>\`（§1 的闭环机制）。
这个目录**没有任何自然回收**：标记被消费时 `AgentExtractImageMarkers()` 会把
`[[AGENT_IMG:…]]` 从文本里**删掉**，所以历史里不留路径、也就永远没人再来引用它。

⇒ 落地时补了 `SweepStaleMcpImages()`（保留 `kMcpImageRetentionDays = 7` 天，
在 `wWinMain` 里跑并打 `StartupTrace("mcp image sweep removed=N")`）。
保留期给到 7 天是因为消费是**秒级**的；真删错了也只是降级成一行 `[读取失败] …`
（`AgentBuildImageParts` 会跳过并记账），不会崩、不会中断对话。

⚠ 判据里 **`now > 文件时间戳` 这个前置条件不能省**：时间戳被拨到未来时 `now - w`
按 `ULONGLONG` 回绕成天文数字 ⇒ 新图被当老图删掉。
**A/B 验证过**：去掉它 ⇒ `AgentMcpSelfTest` 的 `sweep_stale_mcp_images` 转红
（`futureKept=0, n=2`）。

### 7.4 崩溃会留下**孤儿 MCP server 进程**

`McpStdioClient::Stop()` 是**优雅**路径（关 stdin → 等/杀子进程 → join）。宿主被强杀或崩溃时
它根本没机会跑 ⇒ 子进程一直挂着。`genoffice mcp` 是个 **Node 进程**（几百 MB 常驻 + 持有管道），
崩一次留一个 —— 用户会在任务管理器里看到一串看不懂的 `node.exe`。

⇒ 实现：`CREATE_SUSPENDED` 起进程 → 挂进 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job →
再 `ResumeThread`。本进程持有的**最后一个 Job 句柄**消失时（正常退出 / 崩溃 /
`TerminateProcess` 都一样，内核会回收句柄），Job 内所有进程连同后代一起被杀。
**必须先挂后放行**：否则子进程可能在 Assign 之前就 spawn 出不受管的孙进程。

⚠ 自检判据踩过一个坑：`OrphanGuardActive()` 第一版只看「Job 句柄非空 + 无诊断」，
A/B（短路掉 `AssignProcessToJobObject` 的调用）**照样返回 true** —— 假绿。
改成 `IsProcessInJob(hProc, hJob, &in)` 真查**子进程**在不在 Job 里，A/B 才转红。
⇒ 教训：**判据要盯被保护的对象，不是保护者的句柄。**

### 7.5 同一个 client 上并发发请求会**互相偷响应**

`ReadResponse(id, …)` 的实现是「从队列头部取一行 → id 不匹配就**丢掉**再取下一行」
（`McpLineKind::Ignore → continue`）。所以同一个 client 上只要有两个在途请求：

1. 两个请求都写进 stdin（`SendLine` 无锁）；
2. 服务端的响应按某种顺序回来；
3. 等 id=N 的线程可能先 pop 到 id=N+1 的响应 → **丢掉**；
4. 等 id=N+1 的线程永远等不到自己的响应 → **干等到超时**（产品默认 **180s**）。

⇒ 实现：`Impl::callMu` 把 `Initialize` / `ListTools` / `CallTool` 三个入口串行化
（MCP stdio 本来就是「一问一答」协议，这也顺带把 `nextId++` 的竞态收掉）。
`Stop()` **不拿**这把锁（那正是要打断的调用持有的锁 ⇒ 死锁），改为置 `readErr` +
`notify_all()` 让在途请求立刻返回，而不是让它干等 180s。

⚠ **可达性（诚实标注）**：产品当前是**串行**调用工具的
（`src/agent_core.cpp` 的 `for (const auto& tc : assistantMsg.tool_calls)` 逐个 `tool.execute`）
⇒ **今天打不到**。加锁是**预防性地雷**，代价极小。

自检 `concurrent_call_tool_is_serialized` 做到**确定性**（不靠线程调度赌博）：
A 线程先发一个永远不回的 `slow`（占住 1200ms），200ms 后 B 线程发秒回的 `echo` ——
有锁时 B 要等 A 释放（`bDurMs≈1000`），没锁时 B 立刻返回（`bDurMs=0`）。
断言 `bDurMs >= 800` 就能把两种实现分开，且与「谁先 pop」无关。
**A/B 验证过**：注释掉那把锁 ⇒ 本用例转红（`bDurMs=0`）。

