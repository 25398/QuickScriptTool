# 「录制演示 → AI 技能」可行性评估

> 起因：用户提出 —— 用本项目的**键鼠录制** + **图片定位**，把点击时的界面抽象成"帧"，
> 配合录制时的动作逻辑，让 AI 助手理解脚本流程，抽象出操作方式与图片，
> 再生成带 `aiActionExecute` 的脚本，让脚本里的 AI 按这份理解做更高效/更准的自动化。
> 用户同时给了参考项目 `github.com/ai0859/CoraFlow`，问**可行性**与**有没有可复用的开源实现**。
>
> 本文全部结论均给出**本仓文件/行号证据**或**外部项目一手信息**；未验证的一律标注「未验证」。

---

## 0. 一句话结论

| 问题 | 答案 |
|---|---|
| 方案可行吗？ | **可行，而且比预期更可行** —— 这套链路你已经做掉了约 **70~80%**，缺的不是引擎能力，是**「离线分析」这一层**（录制后把扁平动作流编译成"步骤 + 语义 + 锚点"）。 |
| 网上有可复用的开源实现吗？ | **有，而且两个都是 MIT、都直接对口**：**`microsoft/skill-recorder`**（录制→LLM 重建"意图 + 有序步骤"→SKILL.md）与 **`OpenAdaptAI/OpenAdapt`**（演示→**编译**成不含生成式模型的确定性程序→**验证通过才报成功**）。 |
| ⚠ 但 CoraFlow 不是你以为的那个东西 | 它是 **Vue 3 可视化工作流编辑器 + AI 对话生成 JSON 节点图**，**没有录屏解析、没有帧抽象**（详见 §1）。它的 README 原话是"需搭配 python 自动化脚本工具使用"。**把它当参考，只能参考它的 SKILL.md 约束写法，不能参考"AI 解析录屏"。** |
| 最该先做的是什么？ | **不是接 VLM**，是**录制时补采"每步上下文"**（整帧快照 + 前台窗口 + 可选 UIA 路径）。现在录制只存了点击点周围 ≤120px 的小图，AI 根本"看不到界面"（§3）。 |
| 有什么必须守住的红线？ | **抽象层必须落在"编译期/脚本产物"，绝不能落在引擎运行期。** 引擎"只做感知 + 执行 + 如实回执、绝不保留跨帧世界状态"——这条已经用撤销 `ai_locate_track`/`ai_locate_cache`/`ai_ui_layout` 三个模块的代价确立过（§5）。 |

---

## 1. 先纠正一个前提：CoraFlow 不是「AI 解析录屏」

用户看到的那句"借助 AI 解析游戏录屏画面，拆解画面识别、坐标捕捉、逻辑判断全流程"，
**与该仓库的实际代码不符**。已核实的事实：

| 项 | 实际情况 |
|---|---|
| 定位（README 原文） | "一个基于 Vue 3 的可视化工作流编辑器，**需搭配 python 自动化脚本工具使用**，实现自动化脚本工作流" |
| 技术栈 | Vue 3 + Vite + TypeScript（纯前端） |
| 目录 | `src/` `public/` `scripts/` `skill/SKILL.md`，**没有任何视觉/OCR/录屏模块** |
| AI 干什么 | `skill/SKILL.md` 是给**外部 AI（Claude 等）**看的说明书：让它按规则生成**工作流 JSON**（`nodes` + `edges`） |
| 节点类型 | `is_image_exist` / `recognize_number` / `check_region_change` / `click_if_image_exist` / `left_click` / `ocr` / `wait_color` / `loop` … |
| 图片从哪来 | **节点里的 `image` 字段留空 `""`，由用户手动填**（SKILL.md 明写"图片路径留空，由用户手动替换"） |
| 许可证 | MIT |

**⇒ CoraFlow 证明的是"AI 能生成节点图 JSON"这件事，而这件事你的项目已经做了**
（`planScriptActions` / `buildScriptActions` / `buildAiActionExecuteAction`）。
它的节点体系与你的 `ActionType` 高度重合，但**覆盖度远不如你**（44 种动作 vs 它的 13 种）。

**唯一值得借鉴的一点**：它的 `SKILL.md` 是一份**写得非常狠的生成约束**——
把"字段顺序、坐标间隔 210/240、双出口节点必须 2 条边、8 类常见错误"逐条钉死。
你仓库的 `skills/agent/*.md` + `src/ai_action_router.cpp` 内嵌文本已经是同款机制，
**如果要提升 AI 生成脚本的合规率，可以照它的"❌ 错误示例 + ✅ 正确示例"成对写法补几条**。仅此而已。

---

## 2. 现状盘点：这套链路你已经做掉了 70~80%

这是本文最重要的一节。用户以为要"从零搭建"，实际上**零件基本都在**：

| 方案要的能力 | 本仓现状 | 证据 |
|---|---|---|
| 录制键鼠事件 | ✅ 已成熟 | `src/recorder.h` — `RecordedEvent{timeOffsetUs, msg, vkOrButton, x, y, wheelDelta, source}` |
| **录制时自动截图** | ✅ **已经在做** | `RecordedEvent::capturePath` — 注释："BUTTON DOWN **异步截图**完成后回填"；`SetRecordingClickCaptureConfig(enabled, halfSize)`；`ComputeClickCaptureRect()` |
| 截图边缘裁剪补偿 | ✅ | `captureOffsetX/Y` — "边缘裁剪补偿 → FindImage.offset" |
| **点击 → 图片锚点** | ✅ **已落地** | `src/recording_to_findimage.h` — `ConvertActionsToFindImage()` / `ConvertOneClickUnitToFindImage()` |
| 点击单元配对（含拖拽/多击排除） | ✅ 且判据很细 | `ProbeClickUnitFromDown()`；`ClickUnitRejectReason{DragDistance, DragAbsMoveCount, ModifierHeld, MultiClick…}` |
| 录制轨迹压缩 | ✅ | `recopt::MergeSelected()` / `CompressSelected()`（`src/recording_optimize_ops.h`） |
| **轨迹 → 指令块「编译」** | ✅ **已有雏形** | `CompileAiLogicConvert()` — "编译轨迹 → DefineBlock：每个找图独立 timer + findImage(限时) + if/else → AI；步间插入短 Wait" |
| **步骤 + 锚点 + 时序**的数据结构 | ✅ **已存在** | `AiLogicTranscriptEntry{action, locateTarget, templatePath, screenX/Y, fromLocate, fromActivate, settleSec}` |
| **AI 回退成功 → 固化为快路径** | ✅ **已有 heal/promote** | `AiLogicConvertCompileInput::promoteExisting`；`AiLogicConvertSessionBegin(..., healPromote)`；备注 "逻辑转化回退（界面漂移时自愈并提升快路径）" |
| **「验证通过才固化」** | ✅ **已在做** | `AiLogicConvertShouldWriteback(apiOk, actionsAlreadyExecuted, anyActionsExecuted, completeReason, textResult)` — 代码注释原文写着 **"综合成功信号：是否允许段末写回（OpenAdapt：验证后再固化）"** |
| AI observe→plan→act→verify 闭环 | ✅ 相当成熟 | `ExecuteAiActionExecute()` @ `src/ai_action_service.cpp:2453`；hooks 接线 @ `engine_script_run.cpp` ~4395 |
| 感知原语 | ✅ 四条 | 模板匹配（毫秒级）/ OCR 文字坐标索引 / UIA 控件树 / 周期网格推断 |
| 助手侧"动手"入口 | ✅ | `runDesktopTask`（`src/agent_desktop_task.*`，走正常回放链路） |

**★ 特别值得指出**：`src/ai_logic_convert.h` 的接口注释里已经出现
**"OpenAdapt：验证后再固化"** —— 说明**上一轮已经对标过 OpenAdapt 了**，
只是对标的是它的"验证"那半，**没有对标它的"离线编译"那半**。

---

## 3. 真正缺的四层（这才是本次要做的事）

| # | 缺什么 | 现状 | 为什么这是关键 |
|---|---|---|---|
| **①** | **整帧快照** | ❌ 只存了点击点周围 `halfSize ∈ [16,120]` px 的**小图**（`kClickCaptureHalfSizeMin/Max`） | AI 要"理解界面"，得看见**整个窗口/屏幕**。一张 120px 的小图，模型只能知道"这里有个东西"，不知道"我在哪个界面、这一步在干什么"。**这是最大的缺口。** |
| **②** | **每步上下文** | ❌ 录制事件里**没有**"当时前台窗口是谁 / 窗口标题 / 类名 / UIA 路径" | `RecordingWindowTarget` 只记了**录制目标窗**（整场一个），不是**每步一个**。缺了它，抽象出来的步骤无法回答"这一步是在哪个窗口做的"。 |
| **③** | **离线语义分析** | ❌ 「逻辑转化」是**运行时渐进固化**（AI 现场跑一遍、成功才写回），**不是录制后一次性分析** | 这是与 `skill-recorder` 的**核心差距**：它在你点完 Analyze 之后，把**整条时间线 + 抽帧图 + 旁白**交给 LLM，一次重建出"总体意图 + 有序步骤列表"。你现在是"边跑边学"，代价是**每次都要先付一次 AI 的探索成本**。 |
| **④** | **步骤切分 + 意图归纳** | ❌ 录制产物是**扁平动作流**（MoveMouse/Down/Up/KeyDown…），没有"步骤"概念 | `recopt::CollectMoveWaitRanges()` 已经有"**关键动作作分割点**"的思想，`ProbeClickUnitFromDown()` 已经有"点击单元"的概念 —— **切分逻辑可以复用这两处，不用新造。** |

---

## 4. 可复用的开源实现（按对口度排序）

### 4.1 ★★★★★ `microsoft/skill-recorder` — **MIT**，2026-07 开源

**它做什么**：录一次你的真实操作 → 点 Analyze → **GitHub Copilot 重建出「一个总体意图 + 有序步骤列表」**
→ 你审阅编辑 → 生成可复用的 **Skill（`SKILL.md`）** 和/或带调度的 **Automation**。

**它的设计决定里，有四条直接可以抄**：

| # | 它的决定 | 对你的意义 |
|---|---|---|
| 1 | **"最大化廉价的非视频 OS 信号作为主来源，屏幕视频仅作机会性补充（不做逐帧分析）"** | 直接否定了"逐帧视频分析"这条贵路。你的录制**本来就是事件流**，天然符合这个原则 —— 只需补"锚定事件的抽帧"（每次点击/每次窗口切换抽一帧）。 |
| 2 | **采集多路信号**：窗口跟踪（活动应用/标题/**窗口边界**）、浏览器 URL、剪贴板短预览、**可选旁白（本地 Whisper 转写）**、**可选录制终端**（命令/cwd/退出码/完整输出） | 你的缺口②正是"窗口跟踪"这一路。另外 **"录制终端"这个思路很值得抄** —— 对应到你的场景就是**记录脚本自己跑了哪些宏/切了哪些窗**。 |
| 3 | **分析在本地完成，只有点 Analyze 才上传** | 与你"引擎只做感知"的隐私姿态一致，也是个可宣传的卖点。 |
| 4 | **偏好让 Agent 使用原生工具（`gh` CLI、`web_fetch`）而非重放 UI 点击**，并从**单个示例泛化**（"录一次填表单 → 学会填所有表单"） | 这正是你要的"**抽象出操作方式**"而不是"照抄坐标"。 |

**不可直接复用的部分**：它是 Electron + TypeScript，内嵌 Copilot CLI（固定 1.0.78），
产出面向 Microsoft Scout / Copilot Cowork / Copilot Studio。
**你只能抄它的"信号采集清单"与"分析产物格式"，代码一行都用不上。**

**它的产物格式值得照抄**：`SKILL.md`（程序性文档，带 frontmatter），
**且提供"Review SKILL.md"只读预览** —— 用户能看见 AI 到底生成了什么指令再决定是否安装。
⚠ 它自己也声明：Review 提供的是**可见性，不是安全保证**。这个姿态很诚实，值得学。

### 4.2 ★★★★★ `OpenAdaptAI/OpenAdapt` — **MIT**（open-core，本地安全关键验证**不设付费墙**）

**它做什么**：把一次有人监督的 GUI 演示，**编译**成**不含生成式模型的确定性程序**，
在真实系统边界上独立校验声明的效果，只有独立检查一致时才报 `VERIFIED`。

**它比 skill-recorder 更对口的地方**：它是**"录制 → 编译 → 验证"全链路**，而且明确区分了：

| 概念 | 含义 | 对应用户诉求 |
|---|---|---|
| **bundle** | 编译产物（目录），可用 `visualize` 生成 HTML 图、`lint` 查覆盖缺口 | "抽象出操作方式和图片"的**落地形态** |
| **effect contract（效果契约）** | 从"演示所导致的记录增量（observed delta）"**反推**出来的声明效果 + 所需证据层级 | 让每一步都有"预期结果"，而不是只看"点没点上" |
| **`VERIFIED` 是唯一的生产成功态** | 还有 `HALTED_BEFORE_EFFECT` / `RECONCILIATION_REQUIRED`（**绝不盲目重试**）/ `FAILED_PLATFORM` / `CANCELED` / `REJECTED_POLICY` / `COMPLETED_UNVERIFIED` / `ROLLED_BACK` | 你现在只有"成功/失败"两态；这套**七态语义**能直接治"AI 自以为做完了"的毛病 |
| **"点击落地 ≠ 事务提交"** | 官方 `--break-it` 演示：即使界面成功横幅已显示、所有屏幕检查都通过，**独立读记录库不一致时运行仍会 halt** | 这是"胡乱操作"这类问题的**真正解药**：不是让 AI 更聪明，是**让成功判定不靠 AI 自述** |
| **编译产物 0 model calls** | 教程实测 `VERIFIED in 4.1s; 0 model calls` | **运行期不烧 token**，这才是"更高效率"的来源 |
| **多表面证据分层** | 浏览器：DOM / 可访问性 / 视觉 / OCR / 字段几何；原生桌面：视觉 / OCR / 窗口作用域 + UIA/Accessibility/AT-SPI；RDP：外部像素 / OCR / **锚点** / 键盘鼠标 / 新帧验证 | 与你的"DOM 优先 → UIA 优先 → 视觉兜底"路线**同构**，可以照它的表补齐你缺的证据层 |

**它明确的边界**（对你很重要）：仓库只是安装器 + `openadapt` CLI 入口，
**真正的编译器与运行时在 `openadapt-flow` 仓库**；另有研究包
`openadapt-grounding`（**UI 元素定位**）、`openadapt-capture`（屏幕/鼠标/键盘/时序/**窗口作用域**采集）、
`openadapt-retrieval`（**多模态演示检索**）、`openadapt-ml`。
官方强调：**这些研究包不是录制/编译/回放/验证所必需的** —— 也就是说，**核心思路可以只借不装**。

⚠ **"一个 bundle 只用一个执行表面"这条约束值得抄**：跨 surface 的任务必须分别录制再 `compose` 串联。
对应到你的场景 = **一个脚本只绑一个目标窗口**（你已有 `RecordingWindowTarget` + `windowmode::WindowModeScriptConfig`）。

### 4.3 ★★★ `bytedance/UI-TARS-desktop` — **Apache-2.0**，37.1K star

端到端 GUI Agent（VLM 直接看屏出动作），是"计算机使用"这条路线的标杆。
**能借的**：交互范式、"每步截图 + 动作 + 观察"的循环设计、它对坐标归一化的处理。
**不能借的**：它是 Electron 应用 + 它自己的 VLM 权重。**你的产品是 C++ 原生引擎 + 闭源商业分发**，
引 VLM 权重等于把"每一次识图"都押在模型上 —— 而你的 `docs/local-detection-feasibility.md` 已经实测
**VLM 识图 ≈10 800 ms/次**，这条路当**主力**是走不通的，只能当**兜底**。

### 4.4 ★★ `microsoft/OmniParser` — 纯视觉屏幕解析（Set-of-Mark）

把截图解析成"结构化可交互元素列表"再交给 LLM。**你的项目已经评估过并明确"现在不做"**：
`docs/local-detection-feasibility.md` §4 Tier 2 的结论是 ——
它要解决的场景（大量纯图标、无文字、无控件树）**至今没有真实案例**，
而你已经有 **UIA 控件树（桌面）+ OCR 文字索引 + 周期网格推断**三条更便宜的替代路径。
**本次维持这个结论**（游戏界面是个例外，见 §6 风险）。

### 4.5 ★★ `ai0859/CoraFlow` — MIT

见 §1。**只能借 SKILL.md 的"错误示例成对写法"**。

---

## 5. 架构红线（必须守住，否则会重蹈覆辙）

这一条比任何技术选型都重要。

### 5.1 引擎的职责边界是**已经用代价确立过的**

`docs/local-detection-feasibility.md` 开头的**撤销声明**原文：

> `TrackerVit` 跟踪（`src/ai_locate_track.*`）连同定位模板缓存（`src/ai_locate_cache.*`）、
> 布局记忆（`src/ai_ui_layout.*`）已按「**引擎只做感知 + 执行 + 如实回执，绝不替模型做决定，
> 也绝不保留跨帧世界状态**」的原则**全部撤销**（见 `docs/ai-action-exec-optimization.md` §45/§46）。
> 撤销理由不是性能，而是**正确性**：拿一次带噪的感知去写「目标还在老地方」这种世界状态，
> 出错的形态是**静默点到错的地方**，而它省下的只是识图钱。

### 5.2 你的方案**不违反**这条红线 —— 前提是分层放对

| 层 | 能不能放"世界状态" | 理由 |
|---|---|---|
| **录制期**（采集） | ✅ 可以 | 它记的是**已经发生的事实**（第 3 步时前台是记事本、点的是 (523,417)），不是猜测 |
| **编译期**（离线分析） | ✅ 可以，而且**应该** | 产物是**脚本文件 + 技能文档**，落盘、可审阅、可回滚。它是"知识"，不是"运行期状态" |
| **运行期**（引擎） | ❌ **绝对不行** | 引擎不得保留跨帧世界状态、不得替模型判断"目标还在不在老地方"。运行期只有"这一帧我看到了什么" |

**一句话**：**把"演示理解"做成编译器，不要做成引擎的记忆。**
`CompileAiLogicConvert()` 的位置是对的（它产出 `DefineBlock` 动作，落回脚本文件）；
如果做成"引擎在录制时自己维护一份布局，运行期拿出来用"，就是 §10 撤销的复现。

### 5.3 第二条红线：AI 的成功判定不能只听 AI 自己说

`src/ai_action_service.cpp:3596` 已有一处（原文）：
"逻辑转化会话：失败措辞的 `completeTask` 不当成成功（**验收对齐 OpenAdapt halt**）"。
同文件紧接的两行还写明了对照边界：普通（非逻辑转化）AI 动作执行**保持既有语义**
（"只要 AI 验收就按成功计"）—— 即**目前只有逻辑转化这条路是对齐 OpenAdapt 的**。
OpenAdapt 的七态语义是这条红线的完整版 —— **"界面显示了成功横幅"不等于"业务效果达成"**。

---

## 6. 分期建议与验收判据

### P0 —— 录制补采"每步上下文"（低风险，最该先做）

**做什么**：
1. `RecordedEvent` 增加：`foregroundHwnd` / `windowTitle` / `windowClass`（每次事件或状态变化时采样）；
2. **锚定事件的抽帧**：在**每次点击 / 每次窗口切换 / 每 N 秒**存一张**窗口级或全屏快照**，
   落盘到 `recordings/<session>/frames/`，文件名带 `sequence`（与 `RecordedEvent::sequence` 对齐）；
3. 抽帧必须**异步 + 限流**（照抄 `SetRecordingClickCaptureConfig` 那套"钩子内截像素、worker 只写盘"的架构）。

**⚠ 关键约束**：抽帧**绝不能拖慢钩子**。现有 `capturePath` 的注释写得很清楚 ——
"钩子内在 `CallNextHookEx` 前截像素，worker 只写盘"。**新抽帧必须走同一条路**，
否则会毁掉 §11 那套录制精度（8ms 是硬件物理上限，别再往里塞开销）。

**验收判据**：
- `RecorderSelfTest` 新增断言：**帧与事件的 `sequence` 时间对齐**（纯逻辑，可自检）；
- 录制 60 秒操作后，`frames/` 里的帧数在**预期区间**（不是"越多越好"）；
- 录制期间的 `recorder_diag.log` 里，**事件间隔分布与改动前一致**（证明抽帧没拖慢钩子）。

### P1 —— 离线「演示 → 技能」编译器（核心，收益最大）

**做什么**：新增 `src/demo_compile.*`（**纯逻辑，可自检**），四步：

```
① 切分  扁平动作流 → 步骤单元
        复用 ProbeClickUnitFromDown()（点击配对 + 拖拽/多击排除）
        复用 recopt::CollectMoveWaitRanges()（关键动作作分割点）
② 锚点  每步产出多级锚点：{模板图, OCR 文字, UIA 路径, 窗口内相对坐标}
        复用 CaptureAiLogicConvertTemplateAt() / OCR 索引 / UIA 台账
        ★ 锚点必须带"证据层级"（照 OpenAdapt 的表），运行期按层级降级
③ 归纳  步骤序列 + 抽帧图 + 窗口标题 → LLM → "总体意图 + 有序步骤列表"
        ★ 照 skill-recorder 的 describer：一次分析整条时间线，不是边跑边学
④ 产出  产物 A：ScriptAction 序列（findImage/OCR/UIA 快路径）
        复用 CompileAiLogicConvert() 的 "timer + findImage(限时) + if/else → AI" 结构
        产物 B：技能文档（Markdown），可进 skills/agent/ 或与脚本同目录
```

**验收判据**：
- `DemoCompileSelfTest` 新增：① 切分逐格断言（给定动作序列 → 期望步骤数/边界）；
  ② 锚点优先级（模板图命中时不用 OCR、OCR 命中时不用 VLM）；
  ③ **无帧时优雅降级**（缺 frames 目录 ⇒ 退回现状行为，不报错不崩）；
- **A/B 实测**：同一段录制，**带演示技能文档** vs **不带**，跑同一任务，
  对比 **AI 轮次 / 耗时 / token**（这三项是唯一有说服力的指标）；
- 产物 B 可被 `readAgentSkill` 读到（复用既有 section 机制）。

### P2 —— 技能文档接入 `aiActionExecute`

**做什么**：把 P1 产物 B 作为**先验知识**送进 `aiActionExecute` 的 prompt
（或走 `readAgentSkill` 的 section 机制让宏侧 AI 主动读）。

**⚠ 注意**：`src/agent_reference.cpp:703` 现在写着
"`aiActionExecute` — ★权重极低★：除非用户明确要求…"，且
`src/agent_tools.cpp:1976` 有硬闸"用户未明确要求「AI动作执行」或「逻辑转化」，禁止生成 `aiActionExecute`"。
**接入时要走"逻辑转化/自愈脚本"这条已授权的路径**，不要绕过硬闸。
⚠ 改 `skills/agent/*.md` **必须同步 `src/ai_action_router.cpp` 的内嵌文本**（`AGENTS.md` 硬规则）。

**验收判据**：P1 的 A/B 指标在 P2 后**进一步下降**，且**没有**新增"AI 乱点"案例。

### P3 —— 效果验证闭环（对标 OpenAdapt 的 effect contract）

**做什么**：每步声明"预期效果"（画面变化 / OCR 文本出现 / 文件生成），执行后独立验证。
`AiActionHostHooks` 的"观察比对"（"没变就不上传新图"）**已经是雏形** ——
要补的是**从"画面变没变"升级到"该发生的事发生了没"**。

**验收判据**：能复现 OpenAdapt 的 `--break-it` 场景 ——
**界面显示成功但效果没达成时，运行必须 halt 而不是报成功。**

---

## 7. 风险清单

| 风险 | 等级 | 缓解 |
|---|---|---|
| **抽帧拖慢录制钩子** ⇒ 毁掉既有录制精度 | **高** | 走 `SetRecordingClickCaptureConfig` 同款"钩子内截像素、worker 写盘"架构；P0 验收里**必须**核对事件间隔分布 |
| **把"抽象层"做成引擎运行期状态** ⇒ 复现 §10 撤销 | **高** | §5.2 的分层表写进代码注释；抽象只产出**脚本文件**，引擎不持有 |
| **游戏界面无 UIA/DOM，OCR 也可能无文字** ⇒ 锚点退化到只剩模板匹配 | **高** | 模板匹配是毫秒级且精度更高（有像素终审）；**不要为此引 YOLO/OmniParser**（`local-detection-feasibility.md` 已论证更慢且要训练）。锚点层级表里"纯视觉"那档**必须标成最弱证据** |
| 抽帧图**含隐私内容**（密码框、聊天记录）落盘 | 中 | 照 skill-recorder 的做法：**默认本地、只在用户点"分析"时才外传**；并在 UI 明示"不要把密码/Token 录进去" |
| 技能文档**过时**（界面改版后 AI 照着旧文档做） | 中 | 锚点带"证据层级"，模板失配时**必须**降级到重新观察，**不许**照文档硬做；文档里显式标注"生成时间 + 来源录制会话" |
| 生成的动作序列**膨胀**（一次演示 → 上百步） | 中 | 复用 `recopt` 的合并/压缩；P1 切分阶段就做"步骤归并"（连续同质动作合一） |
| 录制与回放**分辨率/DPI 不一致** ⇒ 锚点错位 | 中 | 锚点一律存**窗口内相对坐标 + 模板图**（不是屏幕绝对坐标）；你已有 `MapRecordingPointToClientIfWindowRelative()` 可复用 |
| 产物 B 被当成"安全保证" | 低 | 照 skill-recorder 明示：**提供的是可见性，不是安全保证** |

---

## 8. 明确不建议做的

| 不做 | 原因 |
|---|---|
| **逐帧视频分析** | skill-recorder 明确否定了这条路（成本高、收益低）；你的录制**本来就是事件流**，锚定事件抽帧足够 |
| **引入 YOLO / OmniParser 做屏幕解析** | `docs/local-detection-feasibility.md` 已实测：YOLO 比模板匹配**更慢**（65~356ms vs 毫秒级）且要训练；OmniParser 的场景"至今没有真实案例"。维持原结论 |
| **照搬 CoraFlow 的节点图 UI** | 你是 WebView2 壳 + 原生引擎，动作体系（44 种）比它（13 种）完整得多；抄它等于降级 |
| **把抽象层做成引擎的运行期记忆** | 见 §5。这是已经付过学费的坑 |
| **引 UI-TARS 的 VLM 权重当主力** | VLM 识图 10.8s/次；且闭源商业分发要单独核权重许可。它只能当**兜底**，不能当**主力** |
| **让 AI 自述"我做完了"就当作成功** | 见 §5.3。成功判定必须来自**独立证据**（OpenAdapt 的 `VERIFIED` 语义） |

---

## 9. 核查方式（本文结论的取证路径）

本文所有"本仓现状"结论均可按下列方式复核：

```bash
# ① 录制已在截图（不是猜测）
grep -n "capturePath\|SetRecordingClickCaptureConfig" src/recorder.h

# ② 点击 → 找图 已落地
grep -n "ConvertActionsToFindImage\|ConvertOneClickUnitToFindImage" src/recording_to_findimage.h

# ③ 轨迹 → 指令块编译器已存在 + 已对标 OpenAdapt
grep -n "CompileAiLogicConvert\|OpenAdapt" src/ai_logic_convert.h src/ai_logic_convert.cpp

# ④ 轨迹条目结构（步骤 + 锚点 + settleSec）
grep -n "struct AiLogicTranscriptEntry" -A 15 src/ai_logic_convert.h

# ⑤ 引擎职责边界（撤销声明的出处）
head -20 docs/local-detection-feasibility.md
```

外部项目的一手信息（许可、产物格式、设计原则）来自各自 GitHub 仓库 README 与 LICENSE，
**未在本机构建或运行过任何一个** —— 引用的是**文档声明**，不是实测。⚠ 落地前建议先
`pip install openadapt` 跑一遍它的 tutorial（5 步，产出 `run/REPORT.md`）以验证"0 model calls"这类关键声明。

---

## 10. 一句话回答用户

**方案可行，方向正确，而且你的项目已经走在正确路上了**（`ai_logic_convert` 就是对标 OpenAdapt 的产物）。
真正要补的不是"AI 能力"，是**录制侧的上下文采集**与**离线编译这一层**；
真正要防的不是"AI 不够聪明"，是**把世界状态放错层**。

参考项目里，**`microsoft/skill-recorder`（MIT）抄"信号采集清单 + 分析产物格式"**，
**`OpenAdaptAI/OpenAdapt`（MIT）抄"编译范式 + 七态验证语义 + 多表面证据分层"**，
**CoraFlow 只抄 SKILL.md 的错误示例写法**。三个都用不上代码，但思路都能省掉大量试错。
