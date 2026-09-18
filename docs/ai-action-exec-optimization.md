# AI 动作执行 — 审计与优化（效率 + 准确度）

> 范围：脚本里「AI 动作执行」这一条链路——路由分类 → 截图编码 → 定位/精炼 → Agent 工具闭环 →
> 观察/回看 → 浏览器扩展 DOM 路径 → 逻辑转化回放。
> 本文记录**本轮已落地**的优化、**开放代码库对标结论**、以及**尚未落地的路线图**。

---

## 0. 一句话结论

现有实现已经是「DOM 优先 + 视觉兜底 + 本地 settle + 自适应精炼」的成熟形态，本轮不再做架构级重写，
只打四个**可验证的漏点**：一次白烧的整屏截图、一次会阻塞主链路的预规划、一条绕开 DOM 的点击快路径、
一份坐标口径不够硬的定位 prompt。

| 优化 | 类型 | 预期收益 | 风险 |
|------|------|----------|------|
| A 浏览器 DOM 优先点击 | 效率 + 准确度 | 网页点击 **省 1~2 轮 VLM 识图 + 一次整屏上传**；坐标误差归零 | 低（多重门闩 + 失败回落识图） |
| B 省掉 CompositeClick 的无用首帧截图 | 效率 | 每次「点击 X」**省一次整屏截图 + JPEG 编码（约 0.2–0.4s）** | 低（仅无「带截图」勾选且宿主有本地定位时生效） |
| C 预规划（lookahead）不再阻塞 | 效率 | 去掉每轮 **最多 6s 空等 + join 最长 25s** 的隐藏延迟；预取额度 4→2 | 低（提示变少，但提示本来不减少轮次） |
| D 定位 prompt 坐标契约加固 | 准确度 | 降低「归一化/像素」混用导致的系统性偏移 | 低（仅文案，解析层早已两者兼容） |
| E 浏览器 DOM 优先填写 `typeByLabel` | 效率 + 准确度 | 表单/登录框每格 **省一轮**（observePage 读树 → typeRef 变一次调用） | 中（新增工具面；无命中/多命中时明确报错并指路） |
| G 删除扩展版本门禁 | 修复 | 恢复被误关的扩展截图/找图链路（`preferExtShot` 由 false 变 true） | 低 |
| H 桌面 UIA 优先点击 | 效率 + 准确度 | 桌面软件「点击 X」**零 API 调用**（枚举控件→按名命中→Invoke） | 低（多重门闩 + 失败回落识图） |
| I 点击前遮挡校验 | 准确度 | 拦掉「窗口被盖住/已切走 → 点击打到别的程序」这一类误点 | 低（窗口模式下自动跳过） |
| J UIA 台账/触发工具 | 效率 + 准确度 | 模型先看编号台账再按 (id,name) 触发；比识图快且可校验 | 中（新增工具面；名字冲突/歧义时明确报错） |

---

## 1. 本轮落地改动

### A. 浏览器场景：点击类目标先走扩展 DOM（配套插件针对性优化）

**问题**：路由 `CompositeClick`（prompt 含「点击/双击/单击/点一下/按下」且带图）在
`ai_action_service.cpp` 里直接调用宿主 `onLocateAndClick` → **整屏截图 → VLM 识图 → 坐标点击**，
完全没看扩展已经抓到的控件树，于是：

- 语义上「点击登录按钮」在 DOM 页只要 `clickRef` 一次精确点击；
- 实测路径却要付一次 960 长边整屏上传 + 1~2 轮 VLM，且按像素落点，天然有邻点风险。

**改法**（`src/engine/engine_script_run.cpp`）：在宿主 `onLocateAndClick` 最前面插一层
`tryDomClickViaExtension`，满足全部条件才走 DOM：

1. 扩展已连接（`ExtBridgeServer::IsExtensionConnected`）；
2. 前台窗口标题像浏览器（`LooksLikeBrowserWindowTitle`）或本次已是网页会话；
3. 不是 `canvas` 页（canvas 无 DOM 可点）；
4. 左键（右键/双击的语义差异交给识图，避免误触上下文菜单）。

命中判定用新增的纯函数（`src/page_snapshot.cpp`）：

- `PageSnapshotTargetKeyword()`：把模型写的短标签压成扩展查询关键字。
  扩展 `observePage(query)` 是「控件名**包含**关键字」过滤，整句「点击登录按钮」必然 0 命中，
  压成「登录」才有召回；动作前缀（请帮我点击…）与通用后缀（按钮/图标/链接…）都在这里剥掉。
- `PickPageSnapshotRefForText()`：在返回的控件树里按
  「完全同名 > 前缀 > 包含 > 反向包含」+ 可点击 role 加权 + 超大面积降权 + 阅读顺序靠前
  打分选 `ref`。**只有部分命中且存在近似竞争项时才判 `ambiguous` 并回退识图**；
  同名多项不算歧义（扩展已按相关性排序，取最前 = 列表第 1 项）。

命中即 `clickRef`，返回**紧凑结果**（新 URL/标题 + 树上前 6 项），不把整棵树塞进历史；
任一环节不满足都打印一条诊断后**原样回落**到原有识图管线。

顺带修掉一处「白烧一轮」：`MakeLocateAndClickTool` 原先在「树上已有匹配 ref」时**直接报错**
让模型下一轮改调 `clickRef`。现在宿主有 DOM 钩子时不报错——宿主自己会走 DOM 精确点击；
只有无宿主（离线规划）才保留原来的指路错误。

### B. 省掉 CompositeClick 的无用首帧截图

`execution` 前 `runWithOptionalAutoCapture(needScreen)` 对点击类任务必定截一整屏（1280 长边）
并 JPEG 编码，但 `CompositeClick` 走宿主 `onLocateAndClick` 时**根本不用这张图**——
钩子内部会按 960 长边以满分辨率重新截一次。等于每次简单点击白付一次截屏 + 编码 + `CommitSavedImage`。

**改法**：`ExecuteAiActionExecute` 新增可选参数 `routeOverride`（默认 `nullptr`，不影响既有调用），
宿主在「未勾选带截图 + 任务需看屏 + 宿主有本地定位 + 路由本会是 CompositeClick」时跳过首帧截图，
并把路由显式覆盖成 `CompositeClick`（否则 `withImage=false` 会被重新分类成 `ToolExecute`，
误进规划轮）。日志会打印「本地定位点击自带截屏 → 跳过首帧整屏截图」。

### C. 预规划（lookahead）不再阻塞主链路

`AiActionLookahead` 原本是「每轮多发一次完整 API 请求猜下一步」，本身**不减少轮次**，只往下一轮
指令里塞一句提示。但它有两处隐藏成本：

- `TakeHintAfterObserve(..., 6000)` / `TakeHintSkipObserve(4000)` 是**观察完成之后**才等的，
  已经错过了「与 settle/截图重叠」的窗口，纯加延迟；
- 超时后走 `Cancel() → JoinWorker_()`，而 worker 里的 `SendMessage` 只检查 `stopFlag`、
  不检查 `cancelWorker_`，于是 join 会一直等到那次在途 HTTP 自己结束（recvTimeout 上限 25s）。
  也就是说「最多等 6s」实际可能是「最多等 6s + 25s」。

**改法**（`src/ai_action_lookahead.{h,cpp}`）：

- 预取使用**自己的 `AiHttpAbortSlot`**（绝不复用主链路那个槽，否则丢弃预取会把主请求一起掐掉），
  `Cancel()`/丢弃时真正 `Abort()` 掉在途请求，join 立即返回；
- 等待上限收到 **观察后 400ms / SKIP_OBSERVE 150ms**，跑不完就以当前画面为准丢弃（诊断会写明）；
- 单次 AI 动作最多预规划 **2** 次（原 4），把额度留给真需要方向的复杂任务。

### D. 定位 prompt 的坐标契约加固

原 prompt 只说「输出 [x1,y1,x2,y2]（0~1000）」。多模态模型混用「0~1000 归一化」与
「截图像素」是「点了隔壁」最常见的系统性来源（4K/缩图尤其明显）。现在四处定位/识图 prompt
（整图 locate、Zoom 精点、全图纠偏、识图问答 system）统一写明：

> 一律相对整图 0~1000 归一化，**❌禁止输出像素坐标或图宽图高**

解析层无需改动——`ResolveVisionPointToApiImage` / `ResolveVisionRectToApiImage` 早已同时兼容
两种口径，并优先按归一化解释。

### E. 浏览器场景：按标签一次性填写（`typeByLabel`）

**问题**：`typeRef` 必须先 `observePage` 拿到 `ref`，于是「填用户名 → 填密码 → 点登录」这种最常见的
登录/表单流程，每格都要多花一整轮 API（观察 → 读树 → 填写）。扩展侧 `observePage(query)` 本来就会
按「控件名命中」排序，宿主完全可以自己挑 ref。

**改法**（`src/macro_execute_tools.cpp` 新增 `MakeTypeByLabelTool`）：

- 一次调用内部完成 `onObservePage(query=PageSnapshotTargetKeyword(label))` → 选 ref → `onTypeRef`；
- 选 ref 用**填写口径**（`PageSnapshotPickKind::Input`）：textbox/searchbox/combobox/textarea 加权，
  链接/按钮降权。这个区分是必要的——「密码」当点击目标与当填写目标会选到完全不同的控件
  （「忘记密码」链接 vs 密码输入框），同一套权重必然选错；
- 命中多个输入框 → **不替模型猜**，明确报错并指路 `observePage(query) + typeRef`；
- 未装扩展 / canvas 页 / 搜索结果页 → 直接拒绝并说明替代路径；
- 结果只回紧凑摘要（新 URL + 标题），不把整棵树塞进历史。

### F. 把 DOM 优先门禁抽成可自检的纯函数

A 的门禁判定（扩展是否连接、宿主是否有钩子、是否 canvas、前台是否浏览器、是否右键）原本散在
宿主 lambda 里，**而门禁判错会点到别的窗口**——这是准确度关键路径，不能只靠人眼 review。
现在抽成 `ShouldUseDomFirstAction(DomFirstActionGateInput)`（`src/macro_execute_tools.{h,cpp}`），
宿主只负责采集事实，策略集中一处并被自检覆盖（含「网页会话还在但前台已切到别的程序必须拦」这条）。

### G. 删除配套扩展版本门禁

**问题**：扩展版本号曾从 1.1.x 重置为 1.0.0 重新计数，但宿主仍按
`SupportsExtVision ≥1.0.21` / `SupportsStableBridgeApi ≥1.1.15` / `SupportsSafeExtScreenshot ≥1.0.19`
判断能力，于是随包扩展（1.0.19）被判成「能力不足」：`preferExtShot == false`、窗口模式 CDP 找图的
扩展截图链路整体关闭，还提示用户「请重载扩展到 v1.1.43+」。

**改法**：删掉三处版本门禁（`ext_input.{h,cpp}` 的三个 `Supports*` 与 `ExtVersionCode`），
窗口模式执行器改为**只以「扩展是否已连接」为准**（`window_mode_executor.cpp`）；能力不足时由截图
命令自身的错误路径兜底，并保留明确日志。同时去掉「读不到版本号就猜一个 1.1.5」的兜底——
猜出来的版本号只会让日志和诊断骗人。`ExtensionVersion()` 保留，但注释明确**禁止再用它做门禁**。

### H. 桌面 UIA 优先点击（自动路径，不烧 vision token）

**问题**：桌面软件上「点击保存/下一步」这类目标，原先必须走整屏截图 → VLM → 坐标点击。
可 UIA 树里本来就有「保存」这个按钮，语义完全一致，而且本地枚举免费。

**改法**（`src/engine/engine_script_run.cpp` 的 `onLocateAndClick` + `src/window_mode/ui_element_probe.cpp`）：
新增 `ListInteractiveUiControls` / `PickUiControlByName` / `InvokeUiControlByName`，
点击顺序变为 **浏览器 DOM → 桌面 UIA → 视觉兜底**（对齐 Anthropic 明确写出的阶梯：
有 API 用 API，有 DOM 用 DOM，都没有才用 computer use）。门闩：左键单击、非窗口模式、
前台不是浏览器、不是本软件窗口。命中即 `InvokePattern` 触发（不移动鼠标）；控件没有
InvokePattern（列表项/树项）时改点 UIA 矩形中心，仍然不打像素猜测。
**灰控件在这一步就被拦下**——原先要识图定位完才发现禁用，白烧一轮。

### I. 点击前遮挡校验

**问题**：视觉定位完成到真正点击之间，窗口可能被覆盖、被抢前台或已经切换。此时照旧截图坐标点击，
就会把点击打到别的程序上——这是「点了没反应/点错东西」最典型的一类成因。

**改法**：`windowmode::IsScreenPointOnForegroundWindow(x,y)`（`GetAncestor(GA_ROOT)` + 同进程 +
`GW_OWNER` 链判定，避免把菜单/下拉/工具提示这类独立顶层弹窗误判成遮挡），在识图点击前校验；
不通过就返回明确错误（含当前前台窗口标题）并让 Agent 重新 activateWindow/observePage。
窗口模式下目标窗口本就不一定是前台，故自动跳过。

### J. UIA 台账 / 按编号触发工具（模型回 id + name）

对齐微软 UFO 的「枚举 → 模型回 (id, name) → 宿主校验」：

- `listUiControls(maxCount)`：前台窗口可交互控件的编号台账（纯文本、不烧图），
  形如 `[3] 按钮 "保存" @812,64`；灰控件标 `（灰）`、能力位标 `[可填]`/`[需点击]`。
- `invokeUiControl(id, name)`：**按 name 重新定位、按 id 交叉校验**——两者不一致时
  照样按 name 执行，但把差异写进返回文本当警告（UFO 的 verify-vs-warn 思路）；
  目标灰掉直接拦下；有 InvokePattern 直接触发，否则点控件中心。
- 两个工具都通过宿主钩子（`onListUiControls` / `onInvokeUiControl`）实现，工具层不依赖
  window_mode，因此可以用假钩子自检（见 `ui_control_tools`）。

顺带修掉一个打分缺陷：部分命中时**名字越长越接近目标**（「保存并关闭」应优于「保存」）；
歧义判定也收紧为「同一匹配档 + 名字长度几乎相同」才算歧义——否则「保存并关闭窗口」会因为
「保存」多拿几分而被判歧义、白退回识图。同样的规则同步到了浏览器侧 `PickPageSnapshotRefForText`。

### K. 定位结果模板缓存 + 「点击无变化即作废」闭环

**问题**：同一个 AI 动作在循环里反复点同一个目标（「下一题」「保存」…），每轮都整屏截图 + VLM。
屏幕没动时这是纯浪费。

**改法**（新模块 `src/ai_locate_cache.{h,cpp}`）：识图成功即把定位点周围 80×80 截图存成模板，
键 = **归一化短标签 + 窗口标题 + 窗口类名 + 观察区尺寸**；下一次同键先做模板匹配，命中就跳过
截图与 VLM。安全设计（宁丢优化、不固化错点）：

- **命中门槛四重**：最佳分 ≥ 92%；有次佳时必须「次佳明显更低」或「次佳明显更远」
  （同屏两个都像 → 判为不唯一，回落识图）；只在缓存点 ±200px 邻域内搜；
- **键含窗口身份**：窗口标题/类名或分辨率一变，坐标含义就变了 → 直接查不到，重新识图；
- **点击后无变化即作废**：本次是照缓存点点的，且点击处颜色没变也没有小范围变化 →
  立刻删掉这条缓存（下次老实识图）；日志写明「定位缓存已作废」；
- 生命周期按**脚本运行**（`ResetAiActionSessionState` 的 runId 变化时整批清理并释放位图），
  上限 8 条 LRU，位图所有权在缓存内，替换/清理都 `DeleteObject`；
- 全部决策都打诊断日志（命中/未过门槛/未命中模板/作废），便于真实环境核对。

### 竞品对标（本轮补充）

调研了 Codex CLI、Cursor、腾讯 WorkBuddy、DeepSeek Harness（本机安装包内 README，一手）、
Claude Code / computer use、OpenAI CUA 示例、微软 Copilot Studio 与 UFO²。结论与取舍：

| 竞品机制 | 本仓现状 |
|---|---|
| **控制面阶梯**（Claude Code 原话：有 API 用 API，有 shell 用 shell，有 DOM 用 DOM，都没有才 computer use） | ✅ 本轮落地为 **浏览器 DOM → 桌面 UIA → 视觉兜底**，并集中成可自检门禁 |
| **选 id 而不是回归坐标**（UFO 的「枚举编号 → 模型回 (id,name) → 宿主按 name 校验」；GUI-Actor 实测同 backbone +17~20 分） | ✅ 本轮落地（`listUiControls` / `invokeUiControl` + 自动 UIA 优先） |
| **`verify-vs-warn`**（UFO：id 与 name 不一致照样按 id 执行但明确警告） | ✅ 本仓反过来：**按 name 执行、把 id 差异写进警告**（name 才是模型推理过的语义） |
| **观察-再-行动门闩**（DSH 的 fs-observation-policy：没读过不许改，版本变了必须重读） | ✅ UIA 路径每次触发都重新枚举（天然「重探」）；缓存路径用「窗口键 + 模板高分 + 唯一性」等价约束 |
| **输出有界 + 显式省略标记 + 溢出落盘**（DSH spill：`(Omitted N bytes…stored at: …)`；Codex 头尾对半 + `... N bytes omitted ...`） | ⬜ 本仓只有 `…(工具结果已截断)`，**未带字节数/落盘路径**——路线图 |
| **浏览器日志落文件、只回行数+预览**（Cursor 明确说这是省 token 的关键） | ⬜ 未做——路线图 |
| **一步一批 + 用实测态校验**（UFO² 声称少 51% LLM 调用；Copilot Studio 按「步」计费） | 部分：本仓已有 `runActionRecipe` / `submitMacroActions` 批量提交，但未做「先预测一批 → 校验 → 执行」 |
| **UIA + 视觉混合并按 IoU 去重**（UFO²：保留全部 UIA 控件，视觉框与 UIA 框 IoU > 0.1 丢弃） | ⬜ 本轮只做了「UIA 优先」，未做融合与编号叠加（SoM）——路线图 |
| 批准策略两轴化 + 策略在模型之外不可绕过（Codex sandbox×approval、Claude Code PreToolUse deny 优先于一切模式） | ⬜ 本仓是「审批开/关」单轴——路线图 |
| 快照-再-改（WorkBuddy 改文件前备份） | ✅ 本仓已有 `agent_changes` 快照机制 |

### 新增自检（`tools/ai_action_router_selftest.cpp`，140 项全绿）

| 用例 | 覆盖 |
|------|------|
| `pick_snapshot_ref_for_text` | 完全同名优先；树上没有 → 交回识图 |
| `pick_snapshot_ref_ambiguous` | 近似竞争 → `ambiguous` 不许 DOM 直点；同名多项取列表第 1 项 |
| `pick_snapshot_ref_for_input` | 填写口径选输入框；同名同位置时点击/填写口径必须给出不同答案 |
| `snapshot_target_keyword` | 剥动作前缀/引号/通用后缀，压成扩展查询关键字 |
| `vision_prompt_normalized_contract` | 四个 prompt 都带 0~1000 + 禁止像素坐标 |
| `locate_tool_defers_to_host_dom` | 有宿主钩子不再报错浪费一轮；无宿主保留指路错误 |
| `dom_first_action_gate` | 门禁七种组合：扩展/钩子/canvas/右键/前台非浏览器全部拦下 |
| `type_by_label_tool` | 一次调用按标签填写；无命中/无钩子/空参报错并指路；在两个工具名单里 |
| `ui_control_tools` | UIA 台账/触发：台账透传、执行标记、报错透传、只读工具不被计划门闩拦 |
| `locate_cache` | 定位缓存：键归一、窗口隔离、高分且唯一才命中、无效果作废、清理释放位图 |
| `lookahead_wait_budget` | 等待上限收紧、次数上限 2、无在途时 `Cancel` 立刻返回 |

`tools/window_mode_selftest.cpp` 新增（链路在 window_mode_core，故放这里）：

| 用例 | 覆盖 |
|------|------|
| `uia_control_pick_by_name` | UIA 选控件：完全同名 > 前缀；部分命中取更具体的名字；灰控件降权；同档近似竞争判歧义 |
| `uia_control_list_format` | 台账文本：编号、类型、名字、灰态、能力位、坐标 |
| `screen_point_occlusion_check` | 遮挡校验：屏幕外点判「不属于前台」；自己窗口内点判「属于前台」 |

---

## 2. 对标开放代码库：结论与取舍

主要参考（均为可查证的原始来源）：

| 项目 | 借鉴点 | 本仓现状 |
|------|--------|----------|
| [Midscene.js](https://github.com/web-infra-dev/midscene)（MIT） | Planning/Locate 分工、`deepLocate` 粗→精 ROI 裁剪 + 放大、**归一化 0~1000 坐标契约**、locate 缓存 | 已对齐：自适应精炼、`adaptiveRefineDepth`、本轮加固坐标契约 |
| [browser-use](https://github.com/browser-use/browser-use)（MIT） | **indexed element 点击代替坐标回归**、bbox 包含剪枝、paint-order 遮挡剪枝、`ActionLoopDetector` | 本轮 A 落地「按控件名选 ref 再点」，与 indexed click 同构 |
| [Stagehand](https://github.com/browserbase/stagehand)（MIT） | `observe → act` 一次枚举多次确定性回放、`%variable%` 间接引用、快照裁剪 | 部分：本仓 `observePage`/`clickRef` 已分离；回放未做 |
| [微软 UFO](https://github.com/microsoft/UFO)（MIT） | **UIA 控件树 → 编号 id，模型回 id + name 再校验**、`CacheRequest` 批量取属性、多动作批量 + 实测态校验（最多省 51% LLM 调用） | **未做**，见路线图 P2 |
| [GUI-Actor](https://github.com/microsoft/GUI-Actor) | 「选区域/id」优于「回归坐标」：同 7B backbone ScreenSpot-Pro 27.6 → 47.7；**独立 verifier 免费 +3~5 分** | 部分：浏览器侧已用「选 ref」；桌面侧仍是坐标回归 |
| [UI-TARS](https://github.com/bytedance/UI-TARS)（Apache-2.0） | `smart_resize`（28 对齐）+ 反归一化必须用**模型实际看到的尺寸** | 本仓上传尺寸与映射一起记录，未踩该坑；已记入路线图 |
| OmniParser / Set-of-Mark | 编号标注把坐标回归变成 id 选择（ScreenSpot-Pro 0.8 → 39.5） | **暂不做**：需本地检测器 + captioner，且权重许可 AGPL/GPL 有污染风险 |

### 关键负面结论（避免走弯路）

- **不要指望模型自己说「找不到」**：开源模型的存在性拒答准确率≈0（OSWorld-G 上各模型接近 0）。
  「NOT_FOUND」只能作为**升级信号**，必须配确定性存在性判定（UIA/OCR/模板）才算数。
- **不要用「平均多个候选」合并坐标**：MVP 论文实测聚类 61.7 vs 平均 46.6 vs 随机 55.7——
  平均比随机还差。要合并就做**空间聚类取最大簇质心**。
- **裁剪放大要「在原坐标系里固定比例裁剪、边缘平移而非缩放」**，且多视图不要问模型「该裁哪里」
  （固定比例框 43.2 vs 模型预测框 28.1）。

---

## 3. 路线图（按 收益/风险 排序，尚未落地）

### P0 — 低风险高收益

1. **桌面 UIA 优先定位（第三基底）**：本仓已有 `windowmode::ProbeUiElementAtPoint` /
   `FindDisabledSubmitButtonInForeground`。把「复选框已探测到的控件」升级为
   「**枚举 + 编号**，让模型回 id，宿主校验 id 对应 name」——UFO 式。命中即 `InvokePattern`，
   不打像素；不命中再回落现有 VLM 管线。**收益**：UIA 可见的应用（Win32/WinForms/WPF/WinUI3）
   点击准确率与延迟同时改善，且几乎不烧 token。
2. **点击前遮挡校验**：定位出 (x,y) 后用 `WindowFromPoint` + `GetAncestor` 确认该点确实属于目标
   前台窗口（或 UIA 祖先链一致），否则拒绝点击并报「target_moved」。这是 browser-use 里性价比最高的
   防误点机制，桌面等价物本仓还没有。
3. **截图尺寸/模型的显式契约**：把「上传尺寸 `(w̄,h̄)`」与「反归一化基准」显式记录并打日志
   （UI-TARS `smart_resize` 的 28 对齐坑：用自身 W 而非 w̄ 反算是 4K 下约 28px 系统偏移；
   忘记 letterbox padding 偏移是约 107px）。

### P1 — 中等改造

4. **定位结果缓存（对应 Midscene cache / Stagehand ActCache）**：
   键 = 短标签 + 窗口标题/类 + 屏幕尺寸；值 = 模板图 + 点位。
   命中用模板匹配（高阈值 + 唯一性）直接点，落空/窗口变化/点击无效果即失效。
   本仓已有全部零件：`CaptureAiLogicConvertTemplateAt`、`BitmapLooksLowFeature`、
   `FindTemplateOnScreenMulti`、`SampleClickColorGrid`/`ClickColorGridChanged`。
   **落地前提**：必须有「点后无变化就作废该条缓存」的闭环，否则会固化错点。
5. **桌面历史预算**：现在只有 `role==tool && >720 字` 才压缩。可对齐 Midscene
   `ConversationHistory.snapshot(maxImages)` / `compressHistory`：只保留最近 N 轮图，较早轮图替换为
   `(历史截图已省略)`，并对每类工具给独立预算（本仓 `AiToolResultBudgetChars` 已有分层，可延伸到历史）。
6. **观察的 settle 由固定帧改事件驱动**：现在是「3 帧 + 2×90ms 睡眠 + 差分」。可先用
   UIA/窗口事件或 `WaitForInputIdle` 作为静默信号，超时才退化到固定采样。

### P2 — 明确待确认

7. **配套扩展版本门禁不一致（疑似缺陷）**：宿主门禁要求
   `SupportsExtVision ≥1.0.21`、`SupportsStableBridgeApi ≥1.1.15`
   （`src/window_mode/ext_bridge/ext_input.cpp`），而随包扩展自报 **1.0.19**
   （`extension/edge/manifest.json` + `background.js` 的 `BRIDGE_VERSION`）。
   结果是 `preferExtShot == false`，窗口模式 CDP 找图的扩展截图链路形同关闭，
   同时提示用户「请重载扩展到 v1.1.43+」。**请确认是有意收紧门禁还是版本号回退**——
   两个方向（抬扩展版本号 / 降门禁）都会改变行为，需要你拍板。
8. **浏览器整页文本读取**：扩展已有 `cdp` 透传命令（`sendCdp(method,params)`），
   可零扩展改动加一个「读取可视文本」能力，供「读出页面数据 → saveTaskData」类任务使用，
   避免只能从 64 个节点的树里猜。
9. **一次调用解决「搜索 + 点击第 1 项」**：`searchOnPage` 已在名字命中用户卡片时直接打开主页，
   可把同样的「query → ref → click」合并成扩展侧单次命令，省一次 WS 往返。

---

## 5. 非浏览器链路：按实测日志的修复（2026-09-14 第二轮）

用户提供了一段「统计 Edge 最近 10 条历史记录 → 桌面建 Excel」的失败日志（14 轮 API、任务未完成）。
逐条定位到 5 个根因，全部修复：

| # | 日志现象 | 根因 | 修复 |
|---|---|---|---|
| L1 | 目标「浏览器右上角三个点的菜单按钮」`locateAndClick` → DOM 未命中 → **UIA 被跳过**（"前台是浏览器（交给 DOM 路径）"）→ 走整屏识图 | 我的 UIA 门禁把「前台是浏览器」当成「DOM 能覆盖一切」。**但扩展的可访问性树只覆盖网页内容，覆盖不到浏览器外壳**（地址栏/标签/「…」菜单）——这些恰恰在 UIA 里有准确名字 | 去掉该跳过；顺序固定为 DOM（网页内容）→ UIA（外壳 + 桌面）→ 视觉。`listUiControls` 也不再对浏览器直接拒答，而是返回外壳台账并注明「网页内容请用 observePage/clickRef」 |
| L2 | 菜单项「历史记录」识图回 533×40 的宽框 → `一级可点(宽控件 533×40)，跳过二级 refine` → 直接点中心 → **点开了「设置」** | 「宽控件中心可点」的判定把**菜单行**也算进去了，而模型给的框常比真行更宽 | `wideControl` 收紧：高度 14~56px、宽度 ≤420px、面积比 ≤2%。真正很扁很长的地址栏/公式栏仍由 `thinWideBar` 单独放行，不受影响 |
| L3 | `Ctrl+L → quickInput(edge://history) → keyClick(Enter)`，Enter 被拦两次（"已有网页控件树：…勿 keyClick(Enter)"），随后触发重复调用告警 | 该守卫本意是拦「在站内搜索框按 Enter」，却把**地址栏导航**一起拦了 | 记住最近一次 quickInput 的正文（含动作序号），若它像 URL（`http(s)://`/`edge://`/`www.`/裸域名）且在 3 步内 → 放行 Enter；输入普通搜索词时仍拦（站内搜索请走 `searchOnPage`） |
| L4 | 前台窗口标题已是「设置 和另外 4 个页面 - 个人 - Microsoft Edge」，但扩展 `observePage` **始终返回 4399 游戏页的树**（连续 8 轮） | 宿主把**窗口标题**整串当 `titleHint` 传给扩展，扩展按标签标题匹配 → 匹配不上就沿用旧标签 | 新增 `StripBrowserWindowTitleDecorations()`：剥掉「和另外 N 个页面 / and N more pages」「- 个人/- Profile N - Microsoft Edge」等装饰，只把当前标签标题交给扩展；日志会打印替换前后 |
| L5 | 浏览器的「…」菜单项在 UIA 里也找不到（只有把菜单算进来才行） | 菜单/下拉是**独立顶层窗口**，不在前台主窗口的 UIA 子树里 | `ListInteractiveUiControls` 改为多根枚举：前台窗口 + `GetLastActivePopup` + 同线程可见的 owned 弹窗，按「同名 + 同矩形」去重后统一编号 |

效果（预期日志变化）：

- 「点击浏览器右上角三个点」→ `UIA 优先：命中「设置及其他」[按钮 id=N] → InvokePattern`，**零识图**；
- 菜单打开后「点击历史记录」→ UIA 台账里有该菜单项（L5），按名字命中，不再靠像素猜；
- `Ctrl+H` 或 `Ctrl+L + 输入 edge://history + Enter` 均可导航，观察帧能跟上当前标签（L4）；
- 桌面软件的「点击保存」依旧走 UIA（第二轮已落地），只有**自绘界面/游戏**才回落到整屏识图。

仍未解决、记为路线图的日志问题：历史记录这类「长列表读取」目前仍靠 DOM 树（20 节点），
若要稳定抄 10 条以上，建议加「扩展侧整页文本读取」（见 §3 P2 第 8 条）。

## 6. 视觉兜底为什么会「失效」——按第二份日志的通用修复

第二份日志的现象是：**历史记录页明明已经打开，AI 却始终没意识到**（用户手动把同一张截图发给
豆包网页 Agent，对方能正确读出最近 10 条记录）。根因不在模型，而在宿主/扩展这条链路上：

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| V1 | 日志里 `观察 hint 去掉窗口装饰：「历史记录 和另外 5 个页面 - 个人 - Microsoft Edge」→「历史记录」`（我的上一轮修复生效了），**但紧接着 `observePage` 仍然回 `url=https://console.volcengine.com/...`** —— 树永远是旧标签的 | 扩展 `handleObservePage` 只在「没附着」或「iframe 附着」时才会 `attachTab`；**一旦附着就再也不重新选标签**，`titleHint/urlHint` 被完全忽略。Ctrl+H 切到历史记录页后，扩展仍一直快照旧标签 | `extension/edge/background.js`：新增「跟随前台标签」——传入的 `titleHint`（前台标签标题）与已附着标签标题/URL 明显不符时**重新 attach**，并按同一 hint 去重防止抖动；跟随失败保留原附着（返回可能是旧树，而不是直接报错）。扩展版本 1.0.19 → **1.0.20**（manifest 与 BRIDGE_VERSION 同步） |
| V2 | 树是旧页面（volcengine），而 `pageKind=dom` 判定会把**截图从规划轮里剥掉**（`网页 DOM：不上传截图…省 token`）→ 模型**既看不到新画面、又拿着旧树决策**，连续 10 轮都在错页面里打转 | 剥离截图的依据只有「pageKind=dom」，没有任何「这棵树是不是当前前台」的校验。视觉兜底被 DOM 优化吃掉了 | 新增**控件树可信度**：`AiNotePageTreeTrusted()`。observePage 解析出树后，用「前台标签标题（已剥装饰）」与「树上 title」比对，明显不符即标记不可信；`ai_action_service.cpp` 三处剥图点全部改为 `AiPageKindIsDom() && AiPageTreeTrusted()`，不可信时**保留截图**并向模型注入事实「上一轮控件树与前台不一致，本轮以截图为准」。navigatePage 成功时重置为可信 |

这两条合起来就是用户要的「通用兜底」：**只要控件树不能证明自己属于当前前台，就必须让模型看图**——
而不是让 DOM 优化把视觉能力关掉。

### 顺带修掉的低效点

- `invokeUiControl` 的 `id` 改为**可选**：UIA 编号会随界面变化整体错位（实测模型拿 30 条台账的
  id 去对 80 条台账，必然对不上，白烧一轮告警）。名称才是稳定标识，`id` 只在提供时用于交叉校验。
- `ListInteractiveUiControls` 扫描上限固定为 120（与 `maxCount` 解耦），编号在任何 `maxCount` 下
  都取同一前缀，避免「换个 maxCount 编号就变」。

### 仍未做（等你定优先级）

- **扩展侧整页文本读取**：历史记录这类长列表目前只能靠 DOM 树（本次 20 节点）+ 截图，抄 10 条以上不稳。
- **让模型调用 PowerShell/命令行**（Cursor/Codex 式）：本仓已有白名单命令工具 `runAgentCommand`
  （助手侧），但**AI 动作执行**这条链路没有可直接执行命令的工具；若要引入，必须同时给它
  沙箱边界与审批策略（参考 Codex 的 sandbox×approval 两轴、Claude Code 的 deny 优先于一切模式）。
- **UIA + 视觉融合**（UFO² 式 IoU 去重 + 统一编号），目前是「UIA 优先、失败回落」，不是融合。

## 8. 按第三份日志：路线分流 + 三个误拦/误判修复

第三份日志里任务**已经跑通**（历史记录已抄、Excel 已建、已保存），但暴露了四件事：

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| R1 | 桌面建表走了 **17 轮 API + 约 99 个合成按键**（runActionRecipe 逐格 quickInput + Tab + Enter/Home） | 提示词里没有「先选路线」这一步，模型默认用 GUI 模拟一切 | 新增 **`runCommand(shell, command)`** 工具 + 在 Skill 顶部加「路线选择」段：不需要看界面的活（写文件/批量改名/统计转码/注册表/取数据）一律一条命令做完。工具内部**落到既有的「运行程序」动作**（`runProgram` + `inputText` 参数、`shortcutPreset=0`），所以它天然进逻辑转化/脚本回放，不引入新的回放语义 |
| R2 | 另存为对话框里按 **Enter 被拦**两次（"已有网页控件树：…勿 keyClick(Enter)"） | 网页控件树的守卫只看 `pageKind`，不看**前台到底是不是浏览器**；焦点早跑到 Excel 对话框了，却仍拿上一次的网页 kind 拦桌面操作 | 新增 `AiForegroundIsBrowserWindow()`（前台是 `#32770` 对话框或非浏览器 → 网页守卫不生效）；`keyClick(Enter)`、`mouseClick` 拒绝、新标签页拦截全部加上这道门 |
| R3 | 另存为里点「桌面」被**遮挡校验误拦**（"定位点不属于前台窗口"，可点明明在对话框内） | 现代文件对话框用 DirectComposition 绘制，`WindowFromPoint` 可能返回别的顶层窗口 | 遮挡校验加 **UIA 命中测试交叉校验**：该点的 UIA 元素属于前台进程即认定不遮挡（UIA 更接近「用户实际点到的元素」） |
| R4 | `invokeUiControl` 两次失败（「文件名:」「保存(&S)」）各烧一轮，还要再补一轮 `listUiControls` | 失败只回「找不到」，且模型抄的是**对话框探测文本**（Win32 按钮文本）而不是 UIA 名 | 失败时**直接带回最接近的候选名字**（按首字/前两字匹配，最多 6 个）+ 明确提示「台账只列按钮/菜单/输入类控件，填内容直接 quickInput」 |

两个可复用的实现约定（本轮踩到）：

- **nlohmann 只认 UTF-8 `std::string`**：把 `std::wstring` 赋给 `json` 会变成整数数组，
  动作校验随即报「缺 targetPath」。所有动作字段都要 `ToUtf8(...)`。
- 动作层的运行参数键是 **`inputText`**（不是 `args`/`runArgs`/`launchArgs`；`launchArgs` 是
  窗口模式配置里的字段）——补工具时以 `agent_reference.cpp` 的 `runProgram` 行为准。

### 路线分流（本轮确立，后续按此扩展）

| 任务性质 | 走哪条路 | 依据 |
|---|---|---|
| 文件/数据/批量/注册表/取数据 | **`runCommand`（powershell / cmd）** | 一条命令一步到位，可回放；实测同任务 17 轮 → 2~3 轮 |
| 打开程序/网址/文件 | `runProgram` / `openWebpage` / `openFile` | 既有动作，直接进脚本 |
| 网页内容与网页按钮 | 扩展 DOM：`observePage` / `clickRef` / `typeByLabel` / `searchOnPage` | 可访问性树 + 稳定引用 |
| 浏览器外壳（地址栏/标签/「…」菜单）与桌面软件 | **UIA**：`listUiControls` → `invokeUiControl(name)` | 有准确名字，零识图 token |
| 自绘界面 / 游戏 / UIA 枚举不到 | **视觉**：`locateAndClick`（+ Zoom 精炼/缓存） | 最后兜底，必须始终可用 |

## 10. 按第四份日志：为什么「卡在历史记录界面反复打开」

这份日志里任务**卡死**在同一处（反复开历史记录、反复 locate 失败），根因是我上一轮修复里的一个**顺序错误**：

```cpp
hint = StripBrowserWindowTitleDecorations(hint);   // "历史记录 和另外 7 个页面 - … Edge" → "历史记录"
if (!LooksLikeBrowserWindowTitle(hint)) hint.clear();   // ← "历史记录" 不含 edge → 清空！
```

剥掉窗口装饰后，`titleHint` 变成了「历史记录」，**但下一行又用「像不像浏览器标题」去判断并把它清空了**。
后果是连锁的：

1. `titleHint` 根本没传给扩展 → 扩展无法跟随前台标签 → 一直返回旧标签（deepseek 页面）的树；
2. 宿主侧「树与前台是否一致」的可信度校验因为 `hint` 为空 → 永远判「可信」→ **截图被剥掉**；
3. 于是模型既看不到历史记录页，又拿着别的页面的树 → 反复试 Ctrl+H / 菜单 / 地址栏，全部落空。

**修复**：记住「原 hint 是浏览器窗口标题」这一事实，剥完装饰后**直接采用**剥出来的标签标题，
不再做第二次浏览器判定（`engine_script_run.cpp` 观察钩子）。

同一份日志里另外三处问题与修复：

| # | 现象 | 修复 |
|---|---|---|
| G1 | `runCommand` 连续两轮「JSON 解析失败」，随后被判重复调用 → 路线直接死掉 | 命令正文含换行/引号时模型极易给出非法 JSON。现在**容错恢复**：从原始参数里抠出 `command`（还原 `\n`/`\"` 转义），抠不到就把整段当命令；只有空对象之类才报错。另加测试 `run_command_salvage` |
| G2 | `openWebpage(edge://history)` 被「仅允许 http/https」拒（模型试了两轮） | 扩展 `tabs.update` 无法导航 `edge://`/`chrome://`，这是浏览器限制。工具层改为**展开成地址栏动作批**：Ctrl+L → quickInput(URL) → Enter（仍是可回放动作），并提示 `observePage` 看该页。测试 `open_webpage_internal_page` |
| G3 | locate 失败后 `Ctrl+L` 被「禁止乱按快捷键」拦下 | 该守卫只放行 Enter/Tab/Escape/方向键。现在额外放行**跨软件通用编辑/导航键**（Ctrl+L/A/C/V/X/Z/Y/F、Ctrl+Home/End/Tab），其它键仍需 `confirmBlindKey` |
| G4 | `mouseClick` 在浏览器里被拒（合理），但模型没有更快的路 | 已在 §8 的路线分流里给出：网页按钮 clickRef、外壳 UIA、其余视觉 |

### 命令行 Skill（本轮新增，因为提示词不够，模型不会自己想到用命令）

- **`skills/agent/command.md`（产品自有，随包分发到 `AppDir()\skills\agent\command.md`）**
  ——注意**不是** `.cursor/skills/agent-command/SKILL.md`：`.cursor/` 下的 skill 是给 Cursor/开发者看的，
  这个 skill 是给**产品内嵌助手/AI 动作执行**用的能力定义，必须放在 `skills/agent/`。
  打包脚本优先拷贝 `skills/agent/<section>.md`，缺失时才回落到 `.cursor/skills/agent-<name>/SKILL.md`
- 内嵌同名 section：`lookupMacroAction(section=command)` → `MacroActionCommandSkill()`
- `ai_action_router.cpp` 的 usage/agent 两个 Skill 顶部加了**路线选择**段，指向该 Skill
- 打包脚本（portable + release）的 Skill 拷贝列表已加 `command`

Skill 内容结合本产品实际：工具签名与参数、`resolveSystemPath` 取桌面绝对路径、
「不回显 stdout → 重定向到文件再 readAgentFile」、Excel COM 与 CSV 的取舍、
命令如何落到「运行程序」动作从而可回放、以及**当前无沙箱不提权**的安全边界。

## 11. 验证方式

```powershell
# CMake 源列表改动后需先重新生成工程
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build

& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:AiActionRouterSelfTest /m /v:minimal
build\Release\AiActionRouterSelfTest.exe --json     # 期望 passed=152 failed=0 exit=0
```

本轮实测（Release，2026-09-14，§12 三项视觉加固之后）：

| Suite | 结果 |
|-------|------|
| AiActionRouterSelfTest | **154 / 154**（exit 0，连跑稳定） |
| AgentAssistantSelfTest | **61 / 61**（exit 0，连跑稳定） |
| WindowModeSelfTest | **64 / 64**（本轮干净；本机**存在既有抖动**，见下） |
| ScriptActionBuilderSelfTest | 53 / 53 |
| ScriptIoSelfTest | 58 / 58 |
| ImageMatchSelfTest | 32 / 32 |
| CoordSpaceSelfTest | 27 / 27 |
| MacroVariablesSelfTest | 27 / 27 |
| 合计 | **476 断言 / 0 失败** |
| QstWebViewShell（`QuickScriptTool.exe`） | 构建通过（exit 0） |

WindowModeSelfTest 的抖动与本轮改动无关：多轮实测里偶发失败的用例是
`fake_focus_air_focus_only`（真实光标落点）、`weixin_qt_fake_focus`（剪贴板/前台）、
`no_select_ignores_doc`（目标窗口最小化状态）——都依赖当前桌面状态，且失败用例在不同轮次里
轮换。本轮新增的三个用例（`uia_control_pick_by_name` / `uia_control_list_format` /
`screen_point_occlusion_check`）在任何一轮都没有失败；其中遮挡校验那条已改成「抢到前台才验证
窗口内点」，避免自己引入环境相关的假失败。

回归重点（人工，需要真实桌面）：

1. 网页上「点击登录按钮」类任务：日志应出现
   `DOM 优先：树上命中「登录」[button e1] … → clickRef，省整屏截图+识图`，
   且**不应**出现整屏识图 locate。
2. 桌面软件（记事本/Excel）上「点击保存」：应出现 `UIA 优先：命中「保存」[按钮 id=N] → InvokePattern`
   或 `listUiControls → invokeUiControl`，**且没有任何识图请求**。
3. 循环里反复点同一个目标：第二轮起应出现
   `定位缓存命中：屏幕(x,y) 匹配 NN% 偏差 Npx（省一次识图）`；
   若某次点下去界面没变，应出现 `定位缓存已作废`。
4. 手动把目标窗口切到后台再让脚本点：应看到
   `遮挡校验失败：定位点不属于前台窗口，已拦截点击`，而不是点错程序。
5. 窗口模式（指定窗口后台运行）下，遮挡校验应自动跳过（不误拦）。
6. 脚本里含大量简单点击时：应打印
   `本地定位点击自带截屏 → 跳过首帧整屏截图`，且整步耗时下降。
7. 失败/取消时不应出现长时间卡住（lookahead 已改成可中断）。

## 12. 视觉核心三项加固（本轮：多候选聚类 + 本地校验 + OCR 文本核对）

用户明确的优先级：**「真正核心的还是基于 AI 视觉理解」**——效率路线（DOM/UIA/命令行）都是省钱的旁路，
**视觉兜底必须始终可用且准**。参考已跑通的 OSS 方案（GUI-Actor / MVP / UI-Zoomer 的
「聚类 > 平均 > 随机」、UFO² 的 UIA+视觉 IoU 融合、Midscene 的坐标契约），本轮把视觉这一层
从「一次识图 → 直接点」升级为「一次识图 → 多变证据 → 定级 → 才点」。

### 12.1 多候选 → 空间聚类取簇心（不是平均）

新模块 `src/ai_locate_verify.{h,cpp}`：

- 提示词要求模型**每行一个候选框、最多 3 行、最可能的放第一行**；
  `SplitVisionCandidateLines()` 按行拆（去 `-`/`*`/`1.`/`1)`/`1、` 列表前缀，最多 3 行）。
- 每个候选各自归一到上传图坐标 → 屏幕坐标（沿用既有 `ResolveVisionRectToApiImage` 契约），
  再与 UIA 控件框做 IoU 融合：**IoU ≥ 0.5 或候选中心落在控件框内 → 直接采用控件的精确矩形**
  （UFO² 做法，VLM 的框永远比 UIA 的框粗）。
- 否则对候选中心做单链聚类（合并阈值 `max(24, 0.6×较短边)`），**取最大簇的簇心**作为落点。
  研究数据（GUI-Actor/MVP 复现）：聚类 61.7 > 随机 55.7 > **平均 46.6**——把互相矛盾的候选
  平均掉比随机猜还差，所以这里**只平均「已经聚成一簇」的候选**，分歧时老老实实选一簇。
- 簇内样本数 / 候选总数 = `clusterAgreement`，喂给下面的定级；UIA 命中时直接跳过后续 Zoom 精炼。

### 12.2 本地校验定级：可用 / 可疑 / 需精炼（不依赖 OCR）

`JudgeLocateConfidence()` 用三样本地证据给定位定级（顺序即优先级）：

| 证据 | 判定 |
|---|---|
| UIA 控件确认（落点在已枚举的可交互控件内） | **可用** |
| 框内特征密度过低（灰度 stdDev < 6，纯色/空白区） | **需精炼** |
| 多候选没聚成一簇（`clusterAgreement < 0.5`） | **需精炼** |
| 落点确实压在可交互控件上（UIA 点探测） | **可用** |
| 框过小（< 12px，多半是文字噪点） | 可疑 |
| 单候选、无其它证据 | 可疑 |

配套的两处「别把可疑固化成正确」：

- **低特征不留定位模板**：`BitmapRegionLooksLowFeature()` 判为纯色/空白区的点不写进定位缓存
  （存下来下次必然匹配到别处），日志 `定位框内特征过低，跳过缓存模板`。
- **定级进结果文本**：非「可用」时结果里追加
  `；定位校验=需精炼（…）★该点可疑：先看截图确认再继续；若没点中，换更具体的短标签或加 refineLevels=2，勿连点同坐标`
  ——模型下一轮就知道该换描述，而不是对同一坐标连点。

### 12.3 OCR 文本核对（**装了识别引擎才走**）

用户要求：OCR 核对必须看用户有没有装文字识别引擎，**没装就完全不校验**。实现：

- 只在「定级不是可用」且**目标是纯短文本**时才尝试；预算**一次运行最多 8 次**。
- `AiLocateExtractOcrTarget()` 提取要核对的文字：剥「点击/打开」动词前缀与「按钮/链接/输入框」
  等 UI 类型后缀（`保存按钮` → `保存`）；颜色/方位/图标/序号/长句/带标点/纯数字 → **不做核对**
  （OCR 对不上号，宁可跳过）。
- `CheckOcrEnvironment(false).state != Ready`（用户没装）→ **整段静默跳过**，不改判定、不写噪音日志。
- 装了引擎：在定位点 ±28px 裁一块 → `RunOcrOnBitmap` → `FindTextInOcrLines` 模糊匹配：
  - 读到目标文字 → 定级**升级为可用**（省掉模型再确认一轮）；若 OCR 行中心与落点差 > 12px，
    **按识别框中心修正落点**（文字的真实位置比 VLM 的框准），并夹在识别行框内；
  - 读到了别的文字但没读到目标 → 定级改为**需精炼**并提示
    `OCR 未在该点附近读到「xxx」，疑似未命中`（**只提示不阻断**，OCR 有漏检，绝不因此拒点）。

### 12.4 修掉的两个「校验/通道自身」缺陷

| # | 缺陷 | 后果 | 修复 |
|---|---|---|---|
| X1 | `SplitVisionCandidateLines` 用「数字 + `.`/`)`/`、` 一路吃到非分隔符」剥列表前缀 | 把首行候选 `500,600` 剥成 `,600`、`2、800,900` 剥成 `,900`——**直接吃掉坐标**，多候选反而更不准 | 只在「数字后**确实跟着**列表分隔符」时剥，且 `.` 必须后接空白/行尾（`1.5` 不会被当序号）。新增用例 `split_vision_candidate_lines` 覆盖 |
| X2 | 视觉这一层的 `Refine` 判决只写日志，没进结果文本 | 模型看不到「这个点可疑」，继续对同一坐标连点 | 定级 + 原因进 `locateAndClick` 结果文本（见 12.2 末） |

### 12.5 本轮自检

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build      # ai_locate_verify.cpp 已进源列表
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:AiActionRouterSelfTest /m /v:minimal
build\Release\AiActionRouterSelfTest.exe --json               # 152 / 152，exit 0
```

新增用例：`fuse_locate_candidates`、`judge_locate_confidence`、`split_vision_candidate_lines`、
`bitmap_low_feature`、`ocr_text_verify`（含「没装引擎必须静默跳过」这一条断言）。

## 13. 按第五份日志：runCommand 真的坏了 + 「拿空树还没图」的盲循环

第五份日志（统计 Edge 最近 10 条历史 → 桌面建 Excel）现象正是用户反馈的
「慢 / 不准 / 来回重复」，而且**命令行路线也报错**。逐条根因如下。

### 13.1 `runCommand` 第一次调用就死：「JSON 解析失败」的真凶

日志：

```
调用工具：runCommand（规范动作）
  计划执行 1 个：· runCommand（未解析：未知动作类型：runCommand）
AI动作执行 [doubao-…]：JSON 解析失败
工具返回错误：[错误] JSON 解析失败
```

三个独立缺陷叠在一起：

| # | 缺陷 | 说明 |
|---|---|---|
| B1 | `BuildScriptActionsJsonArray` 在数组后追加 **`[提示] 已自动在末尾追加 stopMacro…`**，而引擎取数组用的是 `find('[')` + **`rfind(']')`** | 提示正文自带 `[提示]`，`rfind(']')` 取到的是**提示里的 `]`** → 截出来的串不是合法 JSON → `json::parse` 抛异常。`runCommand` 的单动作批次必然被自动追加 stopMacro，于是**第一次调用必挂**；其它工具走 `GuardedExecuteActions`（早就用括号配对取法）所以没事 |
| B2 | 引擎的兜底 `catch (...)` 把**任何**异常都报成「JSON 解析失败」 | 执行期异常（例如真正的参数问题）也被误报成 JSON 问题，模型和排查都被带偏 |
| B3 | 日志预览把工具名当动作类型：`j["type"]=name` | `runCommand` 不是动作类型（动作层是 `runProgram`），日志出现「未知动作类型：runCommand」，看着像工具坏了 |

修复：

- 新增导出函数 `ExtractActionJsonArrayText()`（括号配对 + 跳过字符串，**引号里的 `[`/`]` 不参与配对**）。
- `runCommand` 只把**数组正文**交给宿主；引擎执行前也统一用它取数组（双保险）。
- 引擎把「解析」和「执行」分成两个 try/catch：解析失败带 `what()` 原文；执行异常报
  `执行动作时异常：<原因>` 并提示可换的路线。
- 日志预览对 `runCommand` 单独打印 `powershell/cmd 命令：…`。
- 新用例 `extract_action_json_array`（`[提示]` 尾巴 / 命令里含 `]` 都要取对）、
  并给 `run_command_tool` 加了「交宿主的必须是合法 JSON 数组」断言（这类回归靠肉眼看不出来）。

### 13.2 「拿着 0 节点的树 + 没有截图」——为什么来回重复

日志里模型连续 4 轮在 `observePage` → 换措辞 `locateAndClick` 之间打转，关键两行：

```
[诊断] 扩展 observePage pageKind=dom nodes=0 url=http://127.0.0.1:3080/
[诊断] 网页 DOM：不上传截图（用 searchOnPage/observePage，省 token）
```

**剥截图的条件只看「kind=dom 且树可信」，而「可信」当时只比对标题**：树标题与前台标签一致
（都是那个 DeepSeek 页面），于是判可信 → 截图被剥掉。可是这棵树 **0 个可交互节点**
（Ctrl+H 打开的是浏览器**侧边栏**，不在网页 DOM 里），模型等于「拿着空树、又没有图」盲决策——
它当然只能反复重开历史记录、换措辞重复点。

本轮把「树有没有用」也纳入判定，任一成立即**保留截图**并注入事实：

| 判据 | 含义 |
|---|---|
| 标题与前台标签不一致 | 旧树（原有） |
| **0 个可交互节点** | canvas / 内置页 / 侧边栏，DOM 看不到内容 |
| **带 query 却 `queryHits=0`** | 树上没有模型要找的东西 |
| **内置页导航没生效** | 见 13.3 |

### 13.3 内置页（`edge://history`）导航没生效要说出来

`openWebpage(edge://…)` 展开成 `Ctrl+L → quickInput → Enter`（扩展不能导航 inner pages）。
这条路线依赖焦点与 IME，**可能静默失败**：日志里模型以为历史记录已经打开，实际窗口标题一直没变。
现在 `openWebpage` 会记下「待核对的内置页」，下一次 `observePage` 拿树上的 URL 核对：

- 没到 → 树标记不可信（保留截图）+ 明确事实：
  `★openWebpage(edge://history) 没生效：控件树还停在 …。不要再用 Ctrl+H / Ctrl+L 重试。`
  并给出可靠替代 `runProgram(targetPath="edge", inputText="edge://history", forceNew=true)`
  （直接让浏览器打开该内置页），以及「侧边栏 DOM 看不到 → listUiControls/invokeUiControl 或看截图定位」。
- 新增纯函数 `IsBrowserInternalUrl()`（edge/chrome/about/view-source）+ 一次性核对接口，
  测试见 `extract_action_json_array` 里的 `内置页=` 断言。

### 13.4 定位「换措辞反复点同一处」的闸门

失败重试本来就有红线（同目标最多 1 次），但**成功的点击没解决问题时**模型会换说法继续点
（日志：右上角三个点 → 三个水平点 → 设置菜单按钮）。新增本次动作的定位总计数：

- **第 4 次起**在结果里追加：`★本次动作已定位 N 次。若这一击没达到目的，不要换措辞再定位同一目标：
  先 observePage/看截图确认状态，再按优先级换路线 —— clickRef → invokeUiControl → runCommand。`
- **第 12 次**硬停（避免真·多字段任务被误伤），提示改用 `submitMacroActions`/`runActionRecipe` 批量提交。

同时收紧提示词契约（`locateAndClick` 工具描述 + composite/agent 两个 Skill）：

> target 最准是 **2~10 字**、屏幕上真实存在的**文字或控件名**（「历史记录」「保存」「更多」）；
> 方位/颜色只作最短限定。把方位/形状/用途堆成长句会让 VLM 被带偏、UIA 名称匹配变歧义——
> **长描述不是更准，是更不准**。

### 13.6 顺带把「小图标必烧两轮识图」和「确认了还报可疑」修掉

第五份日志里每个定位都是 **2 轮 API**（15s×2），而且**成功了还标「定位校验=可疑」**：

```
[诊断] 第1级矩形 api[904,52,918,62] → 屏幕中心(2429,151)
[诊断] 一级粗框未达「紧凑即点」，lazy 补一级 Zoom 精炼（简单目标仍仅 1 轮 API）
…（第二轮 API）…
[诊断] 第2级点 api(296,373) → 屏幕(2419,149)      ← 只挪了 10px
工具返回：locateAndClick 已点击屏幕(2419,149) 识图轮次=2；定位校验=可疑
```

两个根因，各自都是一次「白烧」：

| # | 根因 | 修复 |
|---|---|---|
| C1 | **UIA 只在「≥2 个候选」时才融合**。单候选（绝大多数情况）即使正好压在 UIA 控件上，也照旧走 Zoom 精炼——而控件矩形本来就比放大精点更准 | 融合改为**单候选也做**，并带上目标描述做名字核对：`IoU ≥ 0.5`（几何即强证据）或 `IoU ≥ 0.3 且名字吻合` 或 `中心落在锚点内且名字吻合` → 直接采用控件精确框，**跳过 Zoom 精炼**（省一整轮 API，且更准）。新增 `UiNameMatchesTarget()`（去掉 `(&S)` 访问键与「按钮/链接」类后缀后比对）+ 单候选/名字过滤用例 |
| C2 | 快路径（「紧凑即点」、现在的「UIA 确认」）**提前 return，跳过了定级**，于是 `verdict` 停在默认 `Suspect` → 结果里出现「定位校验=可疑」，模型据此又去确认一轮 | 抽出 `ComputeLocateVerdictAtPoint()`，快路径与末级共用；UIA 确认直接是**可用**，紧凑框按真实证据定级 |

顺带把每次定位的 `stdDev` 诊断日志从末级移到统一入口（避免快路径没有特征日志）。

### 13.7 顺带修掉自检的「假红」：前台窗口依赖

排查本轮修复时发现 `AiActionRouterSelfTest` **同一个二进制连跑三次会 153/153、148/5 交替**。
根因：`AiForegroundIsBrowserWindow()` 读的是**运行时真实前台窗口**，而 5 个用例断言的是
「网页守卫生效」（网页树拦 Enter、网页里禁开新标签、mouseClick 拒绝…）——测试机上当前前台
是不是浏览器，决定了这些断言红还是绿。

修复：加自检专用覆盖 `SetAiForegroundBrowserOverrideForTest(0|±1)`（0=按真实前台），
在这 5 个用例里显式固定为「是浏览器」。现在连跑三次都稳定 153/153。

### 13.8 自检

```powershell
build\Release\AiActionRouterSelfTest.exe --json     # 153 / 153 × 3 次，exit 0
```

新增/加强用例：`extract_action_json_array`（新增）、`run_command_tool`（加 JSON 合法性 + 含 `]` 命令断言）、
`fuse_locate_candidates`（加单候选 UIA 采信 / 名字过滤 / 名字匹配断言）。

## 14. 按第六份日志：URL 打进搜索栏、配方把已写的行弄没、还是不走 runCommand

第六份日志（同一个「抄 Edge 历史 → 建 Excel」任务）暴露三个**用户可感知**的硬问题，逐条定位如下。

### 14.1 「本应输到地址栏的内容输到搜索栏」——首字符被吞

日志铁证（这行是本轮刚加的核对逻辑抓到的）：

```
[诊断] 观察 hint 去掉窗口装饰：「dge://history - 搜索 和另外 5 个页面 - 个人 - Microsoft Edge」
                                     ^^ 少了 'e'
[诊断] 扩展 observePage pageKind=dom nodes=64 url=https://cn.bing.com/search?q=dge%3A%2F%2Fhistory&…
[诊断] ★内置页导航未生效：期望 edge://history，树仍在 https://cn.bing.com/…
```

`edge://history` 被打成了 `dge://history` → Edge 当搜索词 → 进了必应搜索页；模型却以为历史记录已打开
（而且它确实把这一条搜索记录当成「第 1 条历史」抄进了 Excel）。

**根因（两层）**：

1. `SendUnicodeChar()` 注入的是 `KEYEVENTF_UNICODE + wVk=0` 的**纯字符**事件。若此刻物理上还按着
   Ctrl/Alt/Win（上一条 `keyClick(Ctrl+L)` 的 keyup 还没被目标处理完），目标会把首字符当
   **控制字符/快捷键**吞掉 —— `'e'` 被 `Ctrl+E` 吃掉，剩下 `dge://history` 被当搜索词。
2. 内置页打开路线本身依赖「合成键 + 焦点 + IME」三个变量，天生脆弱。

**修复**：

| # | 修复 | 说明 |
|---|---|---|
| D1 | `SendQuickInputText()` 注入文本前**显式抬起物理按住的修饰键**（Ctrl/Alt/Win/Shift，按 `GetAsyncKeyState` 判断）并等 20ms | 根治「首字符被当控制字符吃掉」这一类问题，**所有** quickInput 都受益（不只是 URL） |
| D2 | 内置页改为**首选让浏览器自己带 URL 打开**：`openWebpage("edge://history")` → `runProgram <浏览器> <url>`（浏览器从**前台进程名**推断：msedge→edge / chrome / firefox / brave） | 一条动作、无合成键、无焦点/IME 依赖，仍可回放；落地页是真正的 `edge://history` 页（DOM 看不到但 UIA/截图看得到） |
| D3 | 识别不出浏览器才退回地址栏路线，并在 `Ctrl+L` 与输入之间、输入与 Enter 之间插入**真等待**（动作层 wait，250/180ms） | 缓解「刚聚焦就打字」的时序问题 |
| D4 | 两条路线都提示「务必核对是否真到了该页」，核对失败时的事实文案点名症状：`若 URL/标题变成「… - 搜索」，说明这个 URL 被打进搜索栏了` | 模型能据此立刻换路线，而不是继续 Ctrl+H |

### 14.2 「复用逻辑反复输入，最后前两行被删」——模板里塞了 Ctrl+Home + Ctrl+A 清表

日志里的动作序列（这正是用户看到的「前两排被复用的内容全被删了」）：

```
第7轮 runActionRecipe 模板9步×11组 → 执行18步（表头+第1行）
第8轮 runActionRecipe 模板10步×11组 → 模板里多了 Ctrl+Home → 又从 A1 写一遍（覆盖）
第9轮 Ctrl+A → Delete → Ctrl+Home（整表清空！）→ 再试跑 18 步（只写回表头+第1行）
第10轮 runActionRecipe → 这次才接着写 2..10 行
```

**根因**：`runActionRecipe` 的模板是**每一组都执行一遍**的，模型把 `Ctrl+Home` 放进了模板 →
每组都跳回 A1 → 反复覆盖表头/前几行；接着为了「重来」按了 `Ctrl+A + Delete`，把已写好的内容整片删掉。
宿主明明知道「已经写进去 2 组」，却没有任何护栏。

**修复（按「Skill 讲清道理，工具在恰当时机提醒」的原则，不做硬拦）**：

| # | 修复 | 说明 |
|---|---|---|
| E1 | **不拦，只讲清后果**：模板含 Ctrl+Home 且 rows ≥2 时，结果里追加说明（逐行追加数据会被反复覆盖表头/前几行；该把起点单独发一次）并**明确写出合法例外**：`若你本来就要「每组回到固定区域覆盖填写」（例如同一张表反复填新值），那这条忽略即可` | 硬拦会让这类合法复用抓瞎（用户明确要求）；判断依据交给模型 + 截图。Skill 里也写清了两种用法 |
| E2 | 新增**写入计数** `recipeRowsWritten`；本次调用前已有写入时，试跑结果追加：`★本表已经用配方写入约 N 组…这次试跑是从当前光标继续写，不是从 A1…禁止用 Ctrl+A + Delete 清表重来` | 让模型知道「光标在哪」这件事，别再假设从 A1 开始 |
| E3 | **Ctrl+A 清表护栏**：已写入 ≥2 组后按 `Ctrl+A` → 返回提示（要修单个错格请用方向键/名称框定位，别全选；确实要清空整表重来请 `confirmShortcut=true`） | 这是**不可逆的数据损失**动作，沿用产品既有的「危险键要确认」模式（和 Escape 关保存框同一套），且确认后放行，不是硬拦 |
| E4 | 配方 memo 增加 `written=N`；试跑结果里也报「累计写入 N 组」 | 状态可追溯 |

### 14.3 「还是不走 runCommand，纯手点」——路线规则只写在 Skill，但没人去查

日志里 **一次 `lookupMacroAction` 都没有**，模型从头到尾没读过任何 Skill；于是它把「写 10 行数据进 Excel」
当成了逐格输入任务（99 个合成按键 + 4 轮配方），`runCommand` 一次都没调。

**根因**：路线规则只写在 Skill 里，而模型在本任务里从未主动 lookup，于是根本不知道有命令行路线。

**修复（Skill 承载知识，工具函数按需提醒——不进系统提示词）**：

- 系统提示词**保持原样**（一个字不加）。系统提示词每轮付费，而路线判断只在一部分任务里相关；
  写进去等于给所有任务加固定开销（用户明确否掉了这个方案）。
- 新增 `AiRouteNudgeOnce()`：**每个任务只提示一次**，只在真的走到「GUI 数据录入」时塞进工具结果，
  且只给**指针**不搬知识：`★路线提示…runCommand 一条命令就能做完…用法见 lookupMacroAction(section=command)`。
  三个触发点：
  1. `runActionRecipe` 首次试跑且模板是纯逐格录入；
  2. `runProgram`/`openFile` 打开的是表格/文档软件（Excel/WPS/Word/Calc…）；
  3. 在**表格软件前台**里 `quickInput` 逐格输入。
- 知识本体留在 Skill：`skills/agent/command.md`（产品自有）＋ 内嵌 `MacroActionCommandSkill()`，
  两者本轮都补了「宿主会在相关时机给指针」的说明与内置页踩坑。

### 14.4 自检

```powershell
build\Release\AiActionRouterSelfTest.exe --json     # 154 / 154，exit 0
```

新增用例 `recipe_reuse_guards`（Ctrl+Home 只提醒不拦 / 试跑带 Skill 指针且每任务只一次 / 已写入后 Ctrl+A 要确认 /
确认后放行 / 表格里逐格输入也给指针）、`open_webpage_internal_page` 重写为两条路线（launch 走 runProgram、
fallback 带等待）。自检还需固定「前台是不是浏览器 / 是不是表格软件」——见 13.7 的覆盖开关（本轮又加了
`SetAiBrowserLaunchTargetOverrideForTest`、`SetAiSpreadsheetForegroundOverrideForTest`，
避免用例依赖测试机当前前台与装了哪个浏览器）。

## 15. 本轮的取舍原则（用户拍板，写下来别再走回头路）

1. **视觉是核心兜底，必须始终可用**；DOM/UIA/命令行都是省钱旁路。
2. **命令行安全边界**：`runCommand` 保持用户权限、无沙箱、无审批（用户明确确认）。
3. **OCR 文本核对只在用户装了识别引擎时生效**；没装就静默跳过（用户明确确认）。
4. **能用 Skill 讲清的道理，不要写成硬约束**：硬拦会在合法场景下把模型卡死（例：配方模板里的
   Ctrl+Home 在某些复用语义下是对的）。做法 = Skill 写清两种用法 + 工具结果里给出后果与判断依据。
5. **知识不进系统提示词**：系统提示词每一轮都付费，只在一部分任务里相关的规则（如路线选择）放在
   Skill 里，由工具函数在**恰当时机**把指针塞进工具结果（`AiRouteNudgeOnce`，每任务一次）。
   危险/不可逆动作（清表、关保存框、乱按快捷键）仍走「要求 confirmXxx=true」的既有确认模式。
6. **每次定位/识图都要有独立证据**（多候选一致性、UIA 命中、特征密度、可选 OCR），
   快路径也必须定级，不能让已确认的点击被标成「可疑」而白烧一轮。
7. **状态判断类缺陷一律按「通用」修**：不要只补当前场景（见 §16.5）。

## 16. 按第七份日志：卡在保存界面反复切窗（通用性修复）

第七份日志：视觉/UIA 这条链路明显快了（宿主直接 `UIA 优先：命中「空白工作簿」→ InvokePattern`、
`DOM 精确点击`，识图轮次大幅减少），但暴露两个问题：**仍然没走 runCommand**、
**Ctrl+S 弹出另存为框后卡死**（locateAndClick 打到浏览器、activateWindow 连续失败、Alt+Tab、F12 乱试）。

### 16.1 「卡在保存界面反复切窗」——四个叠在一起的通用缺陷

日志关键片段：

```
第8轮 hotkeyShortcut → Ctrl+S（新工作簿 → 弹「另存为」）
第9轮 locateAndClick(更多选项) → [诊断] DOM 优先：树上命中「更多操作」[button e1] → clickRef
      （注意：此时前台是另存为模态框，扩展打的是**浏览器标签页**的 DOM！）
第10轮 activateWindow(Excel) → 失败：系统未把该窗口切到前台
      activateWindow(工作簿1 - Excel) → 又失败 → 检测到重复工具调用
第10轮 switchWindow(openPreview) → Alt 预览 → move×1 → 落地
第12轮 keyClick(F12) → 无反应
第13轮 listUiControls「前台「」」→ invokeUiControl(文件名) → 点了一下，仍无进展
```

四个根因（每一个单独都能造成这次卡死）：

| # | 根因 | 修复 |
|---|---|---|
| F1 | **DOM 优先门禁留了「标题读不出来就放行」的后门**：另存为这类 Win32 模态框（类名 `#32770`）标题常常是**空**的，于是门禁一边看到「前台标题空」一边以为「那就是浏览器」，把 `clickRef` 打进浏览器 DOM | 门禁改为看**窗口类**：`Chrome_WidgetWin_*` / `MozillaWindowClass` 才算浏览器，`#32770` 一律不算；标题读不出来**不再**放行（`ShouldUseDomFirstAction` + 新 `ForegroundWindowIsBrowserClass()`）。自检 `dom_first_action_gate` 同步改成新策略 |
| F2 | **控件树可信度把「前台标题空」当可信**：旧逻辑 `if (!wantTitle.empty() && !treeTitle.empty())` 才比对，标题空 → 跳过比对 → 判可信 → **截图被剥掉**；模型因此完全看不到那个保存框 | 两条新判据：① 前台**不是浏览器窗口**（对话框/桌面软件）→ 树必然过期 → 不可信；② 前台标题读不出来 → 不可信。都保留截图（`AiPageTreeTrusted` 现有机制自动生效） |
| F3 | `activateWindow` 失败只说「可能被全屏独占或 UAC 挡住」，并建议 Alt+Tab —— 而**模态框存在时系统根本不允许把别的窗口切到前面**，Alt+Tab 也切不走 | `window_list.cpp`：失败时探测前台是否 `#32770`，是则明确说「前台有模态对话框「另存为」，它挡着时切窗必然失败（不是你的错）→ 先 quickInput 文件名 → Enter 处理掉」，并明说**不要**反复切窗/Alt+Tab |
| F4 | 模型完全不知道「现在有个保存框」：整轮没有任何信息告诉它 | `observeScreenForAgent` 每轮探测前台对话框（复用 `FormatForegroundDialogProbe()`），把 `foregroundFact` 注入本轮指令（**优先级最高**，覆盖其它提示）：saveAs → 「quickInput 文件名/完整路径 → Enter 提交；放弃才 Escape；不要点更多选项、不要切窗」；openFile → 「quickInput 文件名 → Enter，或 Escape 后 openFile(路径)」；confirmOverwrite → 「看图点是/否」 |

另外把 `switchWindow`（Alt+Tab 兜底）也加上同一道闸：前台有模态对话框时直接回「先处理这个框」，
免得模型在「切窗→失败→再切」上继续打转。

### 16.2 「还是没走 runCommand」——只给指针不够，要给**可照抄的命令**

上一轮加的路线指针只写了「用法见 `lookupMacroAction(section=command)`」。日志显示模型**根本不去查**：
它收到指针时已经决定好了计划（`thinking=disabled` 下更不会回头改计划），照旧开了 Excel、逐格填、Ctrl+S。
而且最强信号点（第 4 轮 `saveTaskData` 一次性存下 10 条 `序号|标题|网址|时间`）当时**没有任何触发点**。

修复（仍然不进系统提示词）：

| # | 修复 | 说明 |
|---|---|---|
| G1 | 指针自带**可照抄的命令**：`runCommand{command:"$rows=@('序号,标题,网址,时间','1,标题A,example.com,23:54');$p=Join-Path ([Environment]::GetFolderPath('Desktop')) '记录.csv';$rows \| Out-File -Encoding utf8 $p"}` + 「要真 xlsx 就 `$ws.Cells.Item($r,$c)=…` + `$wb.SaveAs`」 | 不需要再花一轮 lookup，也就不存在「知道有这条路但不查」的漏点 |
| G2 | 新增**最早、最强**的触发点：`saveTaskData` 的内容含换行 + `\|`（成批表格数据）时立刻提示 | 这次任务第 4 轮就会命中——**在打开 Excel 之前**，模型还来得及改路线 |
| G3 | 另三个触发点保留：`runProgram/openFile` 打开表格软件、表格前台里 `quickInput`、`runActionRecipe` 首次试跑 | 每个任务只提示一次（`routeNudgeShown`） |

### 16.3 顺带修掉的两个「日志骗人」缺陷

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| H1 | 日志里 `计划执行 快捷按键[Ctrl+C(拷贝)]` 但紧接着 `执行 快捷按键[Ctrl+S(保存)]` | `ActionName` 对 HotkeyShortcut **只按 `shortcutPreset` 索引取标签**；AI 直接给组合键时 preset 留在默认 0（=Ctrl+C）。**编辑器/脚本备注也会跟着显示错** | `ActionName`：实键与预设不一致时按**实键**渲染（`Ctrl+S`），一致时仍用预设标签；用例 `hotkey_label_real_keys` |
| H2 | `invokeUiControl（未解析：未知动作类型：invokeUiControl）` | 日志预览把「本地工具名」当动作类型解析（`invokeUiControl` 在动作层并不存在） | 预览给这批本地工具（`invokeUiControl`/`clickRef`/`typeRef`/`typeByLabel`/`locateAndClick`/`searchOnPage`/`observePage`/`listUiControls`）单独一行，不再叫「未知动作类型」 |

### 16.4 自检

```powershell
build\Release\AiActionRouterSelfTest.exe --json     # 157 / 157，exit 0
```

新增用例：`dom_first_dialog_gate`（模态框不放行 DOM、按窗口类判浏览器）、
`route_nudge`（表格数据触发、非表格不打扰、每任务一次）、`hotkey_label_real_keys`；
`dom_first_action_gate` 改为新策略（标题读不出来不再放行）。

### 16.5 通用性补充：为什么这些修复不限于「Excel 保存」

F1–F4 描述的都是**状态判断**类缺陷，换任何场景都成立，所以是通用修复而不是补丁：

- 「前台是不是浏览器」必须按**窗口类 + 排除对话框**判断，不能只看标题（标题可为空/被改写）；
- 「控件树是不是当前画面」必须同时看**前台窗口身份**，前台不是浏览器就直接判过期；
- 任何「切换前台窗口」失败时，都要先问一句**是不是有模态框挡着**，再决定给什么建议；
- 宿主已知的前台状态（对话框类型）要**主动告诉模型**，不能指望它从截图里猜（截图还可能被省掉）。

同类隐患已一并处理：`switchWindow`（Alt+Tab）、`pageTreeTrusted`（截图剥离）、
`ShouldUseDomFirstAction`（DOM 直点）三处现在共用同一套「前台身份」判据。

## 17. 办公文档能力（Excel / Word / PPT / PDF）+ 游戏实时链路优化

本轮两个方向：**拓宽任务复杂度**（像 Codex 那样能直接读懂/生成办公文件）与**继续加固视觉链路**
（重点：实时游戏这类画面一直在动的场景）。先做了两轮开源调研（结论与出处见
`.research/office-document-io-report.md` 与 `docs/realtime-game-vision-loop-research.md`），
再按本项目特点落地。

### 17.1 读：`readDocument` 工具 + `office/read_doc.ps1`

新增一个工具（不再只是「开软件用视觉抄」）：

```
readDocument(path, maxChars=6000, pages=3)
→ 已读取 Excel 工作簿「Edge历史记录.xlsx」（引擎 excel-com）
   ---- 正文 ----
   序号	标题	网址	时间
   1	费用中心-火山引擎	console.volcengine.com	18:24
```

提取脚本 `tools/office/read_doc.ps1`（随包分发到 `<exe>\office\`）按格式走**最省事且最稳**的路线：

| 格式 | 主路线 | 兜底 | 实测要点 |
|---|---|---|---|
| xlsx/xlsm | Excel COM（值最准：公式结果/日期） | OOXML 直读（zip+XML：`sharedStrings` + `sheetN`） | 无 Office 也能读；公式只有缓存值 |
| xls/xlsb | Excel COM | — | 老格式只能 COM |
| docx | Word COM | OOXML 直读（`word/document.xml`，段落 + 表格） | Word 表格结构是「单元格 CR+BEL、整行再来一次 CR+BEL」→ 已按此还原成「一行一记录」 |
| pptx | OOXML 直读 | PowerPoint COM（`.ppt`） | **页序必须按 `sldIdLst`+rels 解析**，不能按 `slide1..N` 文件名排（实测可不同） |
| pdf | **Windows 自带 Search IFilter** | pdftotext（装了 poppler 时优先）/ **渲染成 PNG 给多模态模型** | IFilter：无 Office 无 Python，实测 1147 字中文 **0.03s**；扫描件无文本层 → 渲染图片 |
| csv/tsv/txt… | 智能编码识别（UTF-8 BOM / UTF-16 / 系统 ANSI） | — | 中文 CSV 常是 GBK，按 UTF-8 直读会乱码 |

**PDF 文本零依赖路线**（调研里最有价值的发现，已落地）：`Windows.Data.Pdf` 只能渲染（没有文本 API），
但 `.pdf` 的 persistent handler 在 `Windows.Data.Pdf.dll` 里注册了一个 IFilter ——
`CoCreateInstance` + **`IInitializeWithStream`**（不是 `IPersistFile`，QI 会 `E_NOINTERFACE`）
+ `GetChunk`/`GetText` 即可抽文本，CLSID 从注册表解析。**关键坑**：`GetText` 返回
`0x00041709 FILTER_S_LAST_TEXT` 是**成功**码，判 `hr == 0` 会静默拿到空串。

踩坑记录（都写进了 Skill / 代码注释）：
- `powershell -File '<路径>'` **不认单引号**（报 "The given path's format is not supported"，
  还会在 stdout 打出 PowerShell 横幅 → 宿主 JSON 解析失败）。必须用双引号；
  解析时也从后往前找第一行 `{…}`，对横幅/告警免疫。
- **重定向管道下没有控制台**，`[Console]::OutputEncoding` 被忽略 → 中文按 GBK 输出、宿主按 UTF-8
  解析失败。脚本改为**自己写 UTF-8 字节到标准输出**（`OpenStandardOutput()`）。
- 自检必须加载**当前**脚本：`build\Release\office\read_doc.ps1` 是旧副本时测试会假通过/假失败
  → 给 `AiActionRouterSelfTest` 加了 POST_BUILD 拷贝。
- 中文脚本一律 **UTF-8 带 BOM**（本机 ANSI=GB2312；无 BOM 的 .ps1 会被按 ANSI 解析，中文乱码甚至语法错）。

### 17.2 写：`section=office` Skill（产品自有）

新增 `skills/agent/office.md` + 内嵌 `MacroActionOfficeSkill()`，`lookupMacroAction(section=office)`
（excel/word/ppt/pdf/xlsx 等别名也可触发）。内容全是**可照抄的配方**：xlsx（Excel COM，`SaveAs($p,51)`）、
CSV（必须 UTF-8 **带 BOM**）、Word（`Documents.Add` + 可选 `ExportAsFixedFormat($pdf,17)`）、
PPT（`Presentations.Add($false)`；**不能手搓最小 OOXML** —— 真实 pptx 44 个 part，实测 6-part
手搓件被 PowerPoint 拒绝 `0x80070570`）、既有 xlsx→PDF。

以及调研出来的硬约定：COM 必须 `Close`+`Quit`+`ReleaseComObject`（否则孤儿进程锁文件）、
Word/Excel COM **可能挂住**（实测 >2 分钟且无任何对话框 → 一律超时+兜底；禁止用 Word 打开 PDF 转换）、
Excel 独占锁与 `~$` 文件、公式需重算、XML 查询必须带命名空间（`GetElementsByTagName('sldId')`
对 `p:sldId` 返回 0 且不报错）、中文版 Office 对象模型本地化（用 `WdBuiltinStyle` 数字常量）、
`Export-Csv` 不写 `-Encoding UTF8` 会输出 ASCII 把中文变 `??`。

### 17.3 游戏/实时画面：视觉链路继续优化

调研结论先说重点：**已发表的「VLM 每步都问」方案没有一个能实时跑游戏**（要暂停或减速：
Cradle 打 RDR2 暂停等 GPT-4o；grounding 0.7~6.9s 而游戏回路 ~15Hz）。所以优化方向是
「VLM 找一次 → 本地跟住 → 本地判失败才再叫 VLM」。本轮落地三件事：

| # | 改动 | 依据 / 效果 |
|---|---|---|
| I1 | **找图引擎拒绝零方差模板**（`kFlatTemplateMinStdDev = 4.0`），结果里给出原因与替代（findColor/getColor） | OpenCV `templmatch.cpp` 在零方差模板 + `TM_CCOEFF_NORMED` 时直接 `result = all(1)`、平方差侧处处 0 → **每个位置都「满分」**（常落 (0,0)）。最隐蔽的假匹配来源：纯色条/纯色球心当模板必然误点。自检 `frozen_bitmap_find_template` 改为「纯色模板必须被拒 + 有纹理模板必须定位准」 |
| I2 | **定位缓存加 N-of-M 迟滞**：3 次快速采样（间隔 45ms）里 ≥2 次通过才算命中；抖动时作废并回落识图 | 实测单帧检测在 60~70% 摆动（最低 40%），「保留多数帧都出现的目标」后稳定率 80~100%。动态画面单帧就点 = 点在残影上 |
| I3 | **定位缓存按窗口位移重锚**：存/取都带前台窗口矩形，窗口被拖动后把期望点整体平移 | 窗口化游戏/用户拖窗后模板仍在窗口内同一相对位置；旧逻辑会因「离缓存点太远」白回一次识图 |

同时新增 `section=game` Skill（`skills/agent/game.md` + 内嵌 `MacroActionGameSkill()`）：
本地优先感知（颜色 > 找图；UIA 对 DirectX 无效）、`keyDown/keyUp` 长按而不是 `keyClick` 连点、
`moveMouseRelative` 与系统指针加速（同一份代码在不同机器落点差 400~1600px，精确点要用绝对坐标）、
节奏用动作 `duration`（`Sleep` 被量化到 ~15ms，`wait 0.016` 得不到 60Hz）、
连续多步用 `submitMacroActions` 一次提交（等价 speculative multi-action）、
反作弊提示（SendInput 必带 `LLMHF_INJECTED` 标记且无法清除，在线竞技游戏不要跑）。

**未采纳但已记录**（避免以后重复研究）：
- 本机 OpenCV 4.10 无 `opencv_contrib` → `TrackerCSRT/KCF/MOSSE` **根本不可用**；
  可用的 `TrackerVit/Nano/DaSiamRPN` 需要额外模型文件。当前「小窗口 NCC + 迟滞」已拿到主要收益。
- DXGI 桌面复制在双显卡（Optimus）笔记本上**必然失败**（`DXGI_ERROR_UNSUPPORTED`，MS 明确 by design）；
  本产品采集链本来就是 WGC → BitBlt 兜底，**不要**自己去找 DXGI 路径。
- 「整屏缩小再让 VLM 看」会显著掉准确率（目标检测 F1 从 1280×720 的 0.68 掉到 160×210 的 0.31），
  正确做法是**裁小区域**（本产品的 Zoom 精炼 / `refineLevels=2` 就是这个方向）。

### 17.4 自检

```powershell
build\Release\AiActionRouterSelfTest.exe --json     # 159 / 159，exit 0
build\Release\ImageMatchSelfTest.exe --json         # 32 / 32（含纯色模板拒绝）
```

新增用例：`read_document_tool`（CSV/txt 中文读得对、缺文件/不支持扩展名报可执行错误）、
`locate_cache_hysteresis`（窗口位移重锚 + N-of-M 迟滞 + 原门槛不变）；
`frozen_bitmap_find_template` 改为「纯色模板拒绝 + 纹理模板定位准」。

## 18. 与 DeepSeek Harness 的 Computer Use：反向集成（我们做 MCP provider）+ computer-use 别名层

用户提到 DSH 近期加了 Computer Use。查证（2026-09-16）：**确实有，但只在新版本里**——
`@deepseek-ai/dsh` npm `latest` 仍是 `0.1.5-rc.1`（本机装的就是这个，所以本地树里搜不到），
Computer Use 在 **`alpha` = `0.1.6-alpha.1`**：
[release notes](https://github.com/deepseek-ai/deepseek-harness/releases/tag/dsh-v0.1.6-alpha.1)
写着「新增实验性 Computer Use 支持，可通过 Cua Driver MCP 或原生驱动操作本机、获取截图」。

它到底是什么（读了官方子系统文档与 provider README）：

| 组成 | 事实 |
|---|---|
| `@deepseek-ai/dsh-computer-use` | **只是个注册位**：`ctx.computerUse.register(name)`，*"no common desktop-operation methods or model-controlled selector"*（一个 profile 只允许一个 provider） |
| `...-cua-driver-native` | 依赖 **`@trycua/cua-driver@0.28.0`**（第三方），**进程内**加载；工具目录/参数全部来自上游（`cua_driver_native__*`）；需要**宿主桌面权限**；native 崩溃会带走宿主进程；明确「experimental、无稳定性承诺」 |
| `...-cua-driver-mcp` | 连一个已安装的 `cua-driver` 可执行程序（MCP），由那个程序持有权限与执行 |

**结论：不建议把 Cua Driver 当我们的执行器接进来**，理由按重要性排序：
1. **动作不可回放**：我们产品的核心资产是「每个动作都落到可回放的宏动作」（`runProgram`/`mouseClick`/
   `keyClick`…，录制/逻辑转化/脚本/定时任务全依赖它）。Cua 风格在 SDK 内直接注入，不经过我们的动作层
   → 不录制、不转化、不能固化成脚本。
2. **架构倒挂**：DSH 那层只是注册位，真正的能力来自第三方驱动；接它 = 在 C++ 产品里塞 Node + 原生 SDK +
   另一套权限模型，而我们的 Windows 采集/注入链路（WGC、SendInput、UIA、扩展 DOM、中文 IME）更深。
3. 体积（便携包 = 单 exe + WebView2）、alpha 稳定性、native provider 崩溃连带宿主。

因此按用户拍板走**两条正向路线**：

### 18.1 方案 B：让 QuickScriptTool 成为 MCP server（反向集成）

`QuickScriptTool.exe --mcp` 进入 **MCP stdio 模式**（每行一个 JSON-RPC 2.0；在 WebView2/单实例初始化
**之前**分流，因此反复拉起不会弹窗、不抢单实例锁）。实现：`src/mcp_server.{h,cpp}`。

暴露 **12 个工具**（都直接复用产品里已验证的原生链路，没有第二条代码路径）：

| 工具 | 内部实现 |
|---|---|
| `screenshot` | `CaptureScreenRegion` + `BitmapToBase64Jpeg` → MCP image 内容（实测返回真 JPEG） |
| `click` / `move` | `SendMouseClickAtScreen` / `SendMouseMoveAbsoluteScreen`（SendInput） |
| `type_text` | `PrepareImeForTextInput`（先切英文，避免中文组字污染）+ `SendQuickInputText` |
| `key` | `ParseKeySpec`（`Enter`/`ctrl+s`/`alt+tab`/`F5`…）+ `SendKeyboardKey` |
| `scroll` | `MouseInputRouter::Wheel` |
| `cursor_position` | `GetCursorPos` |
| `list_windows` / `activate_window` | `windowmode::ListSwitchableWindows` / `MatchWindows` / `ActivateWindow` |
| `list_ui_controls` / `invoke_ui_control` | `windowmode::ListInteractiveUiControls` / `InvokeUiControlByName`（InvokePattern 优先，否则点控件中心） |
| `read_document` | §17 的 `ReadOfficeDocument`（PDF 无文本层时把渲染出的 PNG 作为 image 内容一并返回） |

**凭什么值得对方挂我们**：UIA 精确控件名/id、浏览器扩展 DOM、WGC 合成采集、中文 IME 处理、
模态框/保存框状态判断、以及「动作可固化成脚本」——这些都是通用 computer-use driver 没有的。

挂载方式（DSH 侧，MCP 客户端配置一个 stdio server 即可）：

```jsonc
// 举例：把 QuickScriptTool 作为一个 MCP server 接进去
{ "mcpServers": { "quickscripttool": {
    "command": "C:\\path\\QuickScriptTool.exe", "args": ["--mcp"] } } }
```

**验证**：`powershell -File tools\test_mcp_server.ps1` → 20 项全过（initialize 协商、
12 个工具带 schema、ping、真实光标坐标、真实截图图片、未知工具 isError、未知方法 -32601、
非法 JSON -32700；通知不产生响应）。这个脚本是**真把 exe 当子进程拉起来走管道**的端到端冒烟。

### 18.2 方案 C：computer-use 兼容别名层（`computer` 工具）

模型侧（Anthropic/OpenAI/Gemini 的 computer tool，本机 `node_modules` 里都能看到）越来越习惯
**单一 `computer` 工具 + action 枚举**的调用形态。新增 `computer` 工具把它映射到我们的既有动作：

| computer action | 落到的动作 |
|---|---|
| `screenshot` | 返回 `[EXECUTED][OBSERVE]`（宿主刷新观察帧，图片随下一轮给出） |
| `left_click` / `right_click` / `middle_click` / `double_click` | `mouseClick`（button / clickCount） |
| `mouse_move` | `moveMouse` |
| `left_click_drag` | `mouseDrag`（from/to） |
| `type` | `quickInput`（`clearFirst=false`，符合「在光标处输入」语义） |
| `key` / `hold_key` | `keyClick`（解析 `ctrl+s` 组合键）/ `keyDown`+`wait`+`keyUp` |
| `scroll` | `scrollWheel`（方向 + 步数） |
| `wait` | `wait` |
| `cursor_position` | 直接返回 `GetCursorPos` 屏幕坐标 |

约定：`coordinate` 用 **0~1000 归一化**（也接受像素，>1000 按像素解释）——与既有契约一致，
引擎侧本来就会做归一化/像素判定。

**关键：别名层不绕过守卫。** 把 `mouseClick` 里那两条踩过坑的拦截抽成 `GuardPointerClickContext()`，
`mouseClick` 与 `computer` 共用：网页里禁止坐标猜点击（指回 `clickRef`）、表格前台禁止点网格
（指回 `Ctrl+Home`/`locateAndClick`）。自检 `computer_alias_tool` 里专门断言这两个守卫在别名入口同样生效。

代价：工具表多一个 `computer`（约 0.5KB schema）。如果哪天觉得不值，删掉 `MakeComputerTool` 的注册即可
（协议层无耦合）。

### 18.3 顺带落地：thinking 改为「允许思考 + 保留工具催促」

早期为了治「只想不调工具 / 单轮数分钟」，对 DeepSeek/方舟网关一律下发 `thinking.type=disabled`。
用户拍板改为**允许思考**（复杂任务——办公文档、Office COM 编排、多步规划、动态画面判断——准确率优先），
「只想不调工具」继续由既有的 `toolNudgePending`（上轮没调工具就催一轮）兜底，
诊断日志同步打印实际策略：

```
[诊断] 工具轮: thinking=允许（复杂任务准确率优先；上轮没调工具会被催促）
```

需要回到旧行为时设环境变量 **`QST_FAST_THINKING=1`**（仅对原本支持该开关的网关生效）。
实现：`ShouldDisableThinking(apiUrl, model)`（`agent_core.cpp`，已导出给诊断用）。

### 18.4 自检

```powershell
build\Release\AiActionRouterSelfTest.exe --json            # 160 / 160，exit 0
powershell -File tools\test_mcp_server.ps1                # 20 项全过，exit 0
```

新增用例 `computer_alias_tool`（12 项断言：截图/点击/双击/右键/组合键/输入/滚动/长按/光标 +
两个守卫 + 错误路径）；MCP 端到端冒烟脚本见上。

### 18.5 踩坑（第 4 次同一类问题，已升级为硬规则）

**含中文的 `.ps1` 必须存成 UTF-8 带 BOM**。本机 ANSI 代码页是 GB2312：无 BOM 时 PowerShell 5.1
按 GBK 解析，中文变乱码、甚至把引号/花括号吃成语法错误（`test_mcp_server.ps1`、`read_doc.ps1`、
调研脚本各中过一次）。规则：**新写/改写的 .ps1 一律 `[System.IO.File]::WriteAllText(path, text,
(New-Object System.Text.UTF8Encoding($true)))`**。

## 19. 性能/发热专项：找图热路径 + 「低性能模式」设计开关

用户反馈：**挂机跑脚本时电脑急剧升温**（i7-7700 / ASUS B250M-PLUS / 8G×2 2666；空闲 50~60°C，
跑脚本满窗口到 80°C）。用户自己把「全屏找图」改成「区域找图」后降到 ~60°C。
本轮做两件事：**把找图热路径里明显的浪费去掉**，再加一个**用户可选的「低性能模式」**，
把「占资源 vs 速度/精度」这个取舍交给用户（用户原话：勾选后以降低性能占用为主要取舍方向）。

### 19.1 先量化：全屏找图到底花在哪

自检里已有可复现的基准（`ImageMatchSelfTest` 的 `fullscreen_locate_perf_and_accuracy`，
合成 2560×1440 屏幕 + 96×96 模板，走 `FindTemplateInFrozenScreenMulti`，不含截屏成本）：

| 版本 | 全屏单次找图 | 说明 |
|---|---|---|
| 旧（救援也全分辨率） | **91ms** | 每次调用都跑一遍全屏 `TM_SQDIFF` 直接卷积 |
| 现在（降采样粗搜 + 全分辨率复算） | **64ms** | 省 30%，位置仍精确（自检断言 `位置准=1`） |

两处根因（都是「无条件做最贵的那件事」）：

1. **`RestrictFindImageToSingleAnchor()` 顺手把金字塔关了**。它的本意是「单命中不要多锚点」，
   实现上却设了 `disablePyramid=true` → 全屏搜索永远不会粗细分层，每帧都全分辨率扫。
   现在改成**按面积自适应**（≥400k 像素 + 有纹理 + 模板 ≥12px 且 ≤ 源图一半才开金字塔），
   单命中不再等于关金字塔。
2. **精确匹配救援（`TM_SQDIFF`）无条件全分辨率**。它是直接卷积（不像 `TM_SQDIFF_NORMED` 走 DFT），
   2560×1440 + 96×96 一次约 3×10¹⁰ 次运算 ≈ 85ms。现在按面积降采样粗搜（取粗层前 6 个极小值，
   抑制半径 = 粗层模板短边/2），再**回到全分辨率在候选点周围的小窗口复算同一个 sumSq** ——
   分数口径与语义完全不变，只是不再全屏卷积。
   新增 `ImageMatchOptions::rescueMaxDownscale`（默认 4）作为该救援的上限旋钮。

用户脚本（`loop` 里两次 `findImage` + `if`）本身已经改成归一化 ROI，所以剩下的成本主要是
**找图线程 fan-out**：`cv::getNumThreads()` 在本机报 **16**，4 核 8 线程的 i7-7700 被超订，
`matchTemplate`/DFT 一启动就是全核满载 + turbo → 温度尖峰。

### 19.2 「低性能模式」勾选框（设置 → 宏回放设置）

| 生效点 | 正常模式 | 低性能模式 | 为什么能降温 |
|---|---|---|---|
| 输入时间轴忙自旋 | 12ms | **0.8ms**（其余交给 `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`） | 回放期间不再长时间占满一个核 |
| OpenCV 线程预算 | 基线（本机 16） | **1** | 一次找图不再顶满所有物理核、不触发 turbo |
| 回放进程优先级 | `HIGH_PRIORITY_CLASS` | 不提（保持 NORMAL） | 不跟系统/游戏抢核 |
| 回放线程绑核 | 绑到单个逻辑核 | 不绑 | 减少「必须占住某核」的调度压力 |
| 系统定时器分辨率 | `timeBeginPeriod(1)` + `NtSetTimerResolution(0.5ms)` | **只留 `timeBeginPeriod(1)`** | 0.5ms 是**全系统**分辨率，会让整机无法进深度 C-state；长挂机时这是隐形大头 |
| 找图监视（WatchImage 时间模式）轮询下限 | 50ms | **200ms** | 内部轮询，占用与频率成正比 |

**不勾选时行为与之前完全一致**（默认 `false`，所有分支都是「加一个开关」，不改默认路径）。

实现要点：

- 开关是**进程级原子量**，头文件内联（`src/low_power_mode.h` 的 `SetLowPerformanceMode()` /
  `LowPerformanceMode()`）。选内联实现是为了让 `app_settings_store_core` 这类轻量自检目标也能用，
  不必为了一个 bool 把引擎依赖拖进来。
- **只留一份开关**：时间轴 / 找图 / 回放守卫 / 监视轮询都读同一个 `LowPerformanceMode()`，
  不再各留一个变量（否则必然出现「设置里勾了但某条链路没生效」）。
- 同步时机：`LoadAppSettings()` 与 `SaveAppSettings()` 各调一次 → **保存后立即生效**，
  不需要重启、也不依赖某个 UI 是否已初始化。
- `SyncImageMatchThreadBudget()`（`image_match.cpp` 两个公开入口内部调用）负责把 OpenCV 线程预算
  按当前开关同步成 `1` 或**基线**（基线在首次调用时用 `cv::getNumThreads()` 记下来，不是硬编码核数）。

### 19.3 自检（本轮新增）

| 用例 | 断言 | 实测 |
|---|---|---|
| `ImageMatchSelfTest / low_power_limits_cv_threads` | 低性能=1，关闭后**回到基线** | `基线=16 低性能=1 恢复=16` |
| `RecorderSelfTest / timeline_low_power_keeps_deadlines` | 自旋压到 800µs 后仍按 deadline 命中，p99 迟到 ≤3ms | `wall=32ms p99迟到=269us 事件=13` |
| `RecorderSelfTest / low_power_flag_toggles` | 开关可开可关（用完必须复位，否则污染后续用例） | ok |
| `AppSettingsStoreSelfTest / save_load_low_performance_mode` | JSON 往返 + 保存/载入后**立即**置位进程开关 | ok |

`timeline_low_power_keeps_deadlines` 的结果顺带纠正了一个旧假设：忙自旋 12ms 带来的精度**远低于**预期
（关到 800µs 后 p99 迟到仍只有 ~0.3ms），说明这段自旋过去主要是白烧 CPU。因此低性能模式在时间轴上的
代价很小 —— 真正需要用户接受的是「找图变慢」（单线程）。

| `template_bitmap_cache_reuse_and_invalidate`（ImageMatch） | 同路径复用（hits/lookups）；改写文件 + 改时间戳后必须重新解码 | `冷=417us 热=130us 命中=2/3 换图后生效=1` |
| `opencl_matchtemplate_bench`（ImageMatch） | 三档面积 CPU vs GPU；只要求位置一致（无 OpenCL 设备直接过） | `480x360 CPU2/GPU1ms · 1280x720 CPU11/GPU7ms · 2560x1440 CPU53/GPU23ms` |
| `gpu_accel_same_result_and_area_gate`（ImageMatch） | 公开入口对比 CPU/GPU 命中位置与分数；小区域不受开关影响 | `CPU分=99 GPU分=99 小区域=1` |
| `find_image_fastpath_gate`（ImageMatch） | 9 组守卫断言（窗口包住漂移带 / 快搜索不规划 / 陈旧不规划 / 贴边不规划 / 窗口≈搜索区不规划 / 漂移与分数余量） | 正常窗口 `976,676-1120,820`（比全屏小 178 倍） |

全套自检（本轮全绿）：AiActionRouter 160 / AgentAssistant 61 / WindowMode 66 / ScriptActionBuilder 53 /
ImageMatch 38 / CoordSpace 27 / MacroVariables 27 / ScriptIo 58 / AppSettingsStore 42 / RecorderSelfTest 61，
`tools\test_mcp_server.ps1` 20 项 exit 0。

### 19.5 还没做（按收益/风险排序，别再盲猜）

1. **截屏成本**：`BitBlt` + BGRA→gray 在 2560×1440 下也是毫秒级。WGC 复用帧是备选，
   但会改变「所见即所得」语义（拿到的是上一帧），要单独评估。
2. **GPU 路径扩到金字塔/跨分辨率分支**：现在只有「非金字塔单尺度」分支走 GPU
   （引擎单命中找图正好走这条）。金字塔粗层本来就在小图上跑，收益有限，先不做。
3. **模板缓存收益太小**：如果哪天想再压，方向不是解码而是**整块 `BitmapToBgrAndMask` + gray 转换**
   一起缓存（现在每次调用仍要把 HBITMAP 转回 Mat + 转灰度）；小模板下这点开销同样可忽略。

### 19.4 第二轮：模板缓存 / 上一帧复核 / GPU 实测

上一轮列的「还没做」三项，这一轮全做了（并附实测结论）。

#### ① 模板解码缓存（`image_match.cpp`）

`findImage` 每步都走 `PrepareFindImageMatch` → `LoadBitmapFromFile` → `imread` + 解码；
循环里每秒十几次纯属白给。现在 `LoadBitmapFromFile` 内部按 **路径 + 文件大小 + 修改时间**
缓存 `ImReadW` 的解码结果（`cv::Mat`），命中时只做一次 `CreateDIBSection` + `memcpy`。
返回给调用方的仍是**新建的 HBITMAP** → 句柄所有权语义与之前完全一致（调用方照旧 `DeleteBitmapHandle`）。
上限 24 项 / 24MB，LRU 淘汰；`TryTemplateBitmapSize` 也走这条缓存（不再为了拿宽高造 HBITMAP）。

| | 冷（读盘+解码） | 热（缓存） |
|---|---|---|
| 32×32 BMP | 417~430µs | 130~135µs |

**诚实结论：收益不大**（每步省 ~0.3ms，循环 20 步/秒 ≈ 6ms/s），它的真正价值是去掉磁盘 I/O 抖动。
失效判据用「大小 + 修改时间」：注意 Windows 文件时间戳粒度约 15.6ms，同一 tick 内用**同尺寸**内容
覆盖同名文件理论上可能漏判 —— 人改图/编辑器另存不可能落在同一 tick，自检里用 `SetFileTime`
显式改时间戳来覆盖这条路径（`template_bitmap_cache_reuse_and_invalidate`）。

#### ② 找图「上一帧命中」本地复核（快速路径）

循环里反复找同一个东西时画面基本没变。做法：先在**上一帧命中点周围的小窗口**里，用**同一套
阈值与像素校验**复算一次；过了就直接用，**没过就当场回退全屏搜索**（不是等下次重试）。
因此「漏检」不可能发生 —— 最坏只是多花一次小窗口的时间。

判据是纯函数（`PlanFindImageFastPath` / `AcceptFindImageFastPathHit`，`image_match.h`），便于穷举自检：

| 守卫 | 值 | 为什么 |
|---|---|---|
| 请求指纹 | 模板路径 + 搜索区 + 阈值/尺度/完美匹配 + 模板宽高 + 目标分辨率 | 任一变化即视为新请求（脚本改了、分辨率换了都不能用旧命中） |
| 上次全屏耗时 | ≥ 12ms | 区域找图本来几毫秒，白付调度开销 |
| 命中新鲜度 | ≤ 3000ms | 太旧就重新全屏搜 |
| 漂移 | ≤ 8px（切比雪夫距离） | 超过就当目标移动了 → 回退全屏，避免点到「同款图标」的另一个实例 |
| 分数余量 | ≥ 阈值 + 3 个百分点 | 防止「本地复核刚过线、全屏搜却不过线」让 `matchData >= X` 分支判断变样 |
| 窗口覆盖漂移带 | 必须完整包含 `[prev-8, prev+8]` | 命中贴搜索区边缘时窗口放不下 → 直接不规划（宁可回退） |
| 窗口 vs 搜索区 | 窗口 < 搜索区 70% | 否则是「假快速路径」 |
| 失效 | 全屏也没找到 → 立刻删掉该请求的旧命中 | 别让下一步再白试小窗口 |
| 清空时机 | **每次开始跑脚本**（`ResetFindImageFastPath()`） | 绝不让上一次运行的命中影响这一次 |

实测（自检 `find_image_fastpath_gate`）：2560×1440 全屏、命中 (1000,700)、96×96 模板 →
规划出的复核窗口是 **976,676-1120,820 = 144×144**，比全屏小 **178 倍**。
即一次 60~100ms 的全屏找图，在画面没动的情况下变成亚毫秒级的小窗口搜索。

#### ③ GPU（OpenCL）实测：能用，但只在「大区域」划算

自检 `opencl_matchtemplate_bench` 直接量（TM_SQDIFF_NORMED，含上传 + 结果回传）：

| 搜索区 @ 模板 | CPU | GPU(OpenCL/UMat) | 结论 |
|---|---|---|---|
| 480×360 @48×48 | 2ms | 1~2ms | **打平**（纯付传输开销） |
| 1280×720 @96×96 | 11~24ms | 7~11ms | GPU 约 2x |
| 2560×1440 @96×96 | 46~100ms | 23~35ms | GPU 约 2~2.9x |

三个必须写下来的事实：

1. **只调 `cv::ocl::setUseOpenCL(true)` 没有用**：OpenCV 的 `matchTemplate` 只对 `UMat` 输入走
   OpenCL；对普通 `Mat` 输入反而**更慢**（实测 2560×1440 从 46ms 变成 89ms，因为它要隐式上传再回传）。
   所以真正生效的必须是显式 UMat（`TryMatchTemplateOnGpu`）。
2. **我们的 locate 需要整张相似度图落在 CPU**（`FindPeaks`/多峰/阈值筛选），上表 GPU 列已经把这笔
   「回传」算进去了 —— 即使按最坏情况量，大区域仍是 2 倍以上。
3. 面积门槛取 **500k 像素（≈900×560）**：低于它一律走 CPU。

落地方式：设置 → 宏回放设置 → **「找图 GPU 加速」**（默认关）。机器没有 OpenCL 设备时自动回落并只写
一条调试日志；GPU 计算抛异常就**本进程永久回落 CPU**（绝不因为加速把找图搞坏）。
与「低性能模式」冲突时**低性能模式优先**（笔记本 dGPU 算一次的功耗/发热比 CPU 更凶，
与「省电降温」目标相反）—— 这条规则由 `FindImageGpuAccelActive()` 单点表达。

精度验证：`gpu_accel_same_result_and_area_gate` 走**公开入口** `FindTemplateInFrozenScreenMulti`
对比 —— CPU 分数 99 / GPU 分数 99、位置一致；小区域（<500k 像素）开不开 GPU 开关结果都不变。

结论：**iGPU/独显不是「一定更慢」**，但收益严格依赖面积 —— 所以它是开关而非常开，
且区域找图（用户脚本现在的写法）根本不会走到 GPU 路径。

## 20. 按第八份日志：AI 动作执行在游戏里「一直思考、没反应」

用户反馈：用 AI 动作执行打《植物大战僵尸融合版》"我是僵尸"，**图片识别功能缺陷很大**，
而且游戏**没什么反应、一直在思考**。日志里两个现象各自对应一个根因。

### 20.1 自己人把唯一能推进的手段锁死了

`locateAndClick` 开头有条硬闸 `AiActionUiTooBusyForVisionLocate()`：前台动态覆盖率 ≥10%
就拒绝识图点击，报「前台页面动态干扰过大（游戏/广告/视频区在变），禁止在页面内容上 locateAndClick」。
游戏画面**每帧都在重绘**，覆盖率必然超标 → **真实游戏里 locateAndClick 100% 被拦**。
同一轮指令还写着「禁止在页面内容上 locateAndClick。切窗用 activateWindow」，
`observePage` 的 canvas 文案更直接：「**进游戏后人手或脚本接**」。
三条叠加 = 模型只剩 listWindows/activateWindow/看图，鼠标点不了画面 → 表现就是「一直思考、没反应」。

这条闸的**本意**是「浏览器页面在动时别烧识图 token，树上能点就用树」，
对「没有树可退」的前台根本不成立。判据改成按前台类型分流：

| 前台 | 旧行为 | 新行为 |
|---|---|---|
| 浏览器 + dom/mixed | 放行（树优先） | 不变 |
| 浏览器 + canvas（网页游戏/绘图） | 超高动态时拦 | **放行**（画布本来没有节点） |
| 浏览器 + 页型未知 + 高动态 | 拦 | 不变（先 `observePage` 拿树），文案改成「先拿树」而非「禁止」 |
| **桌面游戏 / 自绘应用 / 模拟器** | **拦**（覆盖率必然超标） | **放行** —— 视觉是唯一手段 |
| 表格软件 | 拦 | 不变（Ctrl+Home / 列标等路线更准） |

配套（`macro_execute_tools.cpp` / `ai_action_service.cpp`）：

- 新增 `AiActionGameForegroundLikely()`：画布页 / 非浏览器前台 / 非表格 / **画面在持续变**
  （静态桌面软件覆盖率很低，只有游戏/视频/模拟器这类逐帧重绘的前台才过阈值）。
- 命中时**每轮**在指令里加正向指引：locateAndClick 点画面 + keyClick/keyDown，
  **本轮至少落一个动作**，不要只观察、不要反复 activateWindow。
- `AiGameNudgeOnce()`（仿 `AiRouteNudgeOnce`，每任务一次）把可照抄的玩法直接塞进指令：
  先看清→记住位置→同目标别反复识图→长按用 keyDown/keyUp→血条用 findColor→每轮必须有动作。
  （实测「只给指针」时模型不会去 lookup，所以要点要内联。）
- canvas 与 `observePage` 两处「人手或脚本接」文案改成游戏路线指引。

### 20.2 每轮都白付一次完整请求（流式读取中断）

日志每轮都是：

```
思考中… 流式等待 8s，思考 231 字节…
流式失败，改用完整响应（读取流失败：要求操作句柄的状态错误 (code=12019)）
正在连接 API（请求体 42 KB）… 等待响应 10s… 15s…
```

`12019 = ERROR_WINHTTP_INCORRECT_HANDLE_STATE`。根因是**短超时轮询读 body**：
WinHTTP 的 `RECEIVE_TIMEOUT` 一旦触发，这次请求句柄就废了（后续 `QueryDataAvailable` 报 12019），
而 thinking 模型「看图 + 想」的间隙常有几秒 —— 拿 250ms/1500ms 的短切片去轮询，等于每轮都在赌。
实测规律：**带图的请求（163KB/171KB）死在 4~9s，纯文本请求（43~47KB）一路正常** ——
正是因为看图那几秒没有新字节。（第一版只把 250ms 抬到 1500ms，日志里照旧死，说明短超时这条路本身不成立。）

**最终方案：读给足超时 + 看门狗线程**（`agent_core.cpp` `CallApiStream`）：

- body 的 `RECEIVE_TIMEOUT` = `max(30s, 本段超时)`，**不再用短切片**，所以正常间隙不会再毁句柄；
- 另起一个**看门狗线程**负责原来 `checkStreamProgress` 的全部策略：每 3s 心跳（`流式等待 Ns，思考 N 字节`）、
  思考过久/停顿未调工具→收束、工具调用组装超时→改完整响应、空流/首包超时、整体超时、空闲超时；
  需要打断阻塞读时用 `Abort()`（与「停止热键」同一条已验证路径），
  看门狗只读 `std::atomic` 投影（`reasoningBytes`/`contentBytes`/`toolAssemblyStarted`/…），
  不跨线程碰读线程独占的 `state`（否则是数据竞争）；
- 读循环遇到错误时**先判断「已攒到可用内容」**（`ShouldFinalizeStream`）再决定 fail ——
  SSE 正常收尾时 `QueryDataAvailable` 本来也可能报 12019，那不是故障。
  旧实现一律 fail，等于把整段流式结果丢掉再付一次 ~90s 的完整请求。

顺带修掉一个潜在雷：`Abort()` 关句柄后 RAII `WinHttpHandle` 会**再关一次**（句柄值可能已被系统复用）。
现在 `AiHttpAbortSlot` 带 `closed` 标记 + `AbortAwareRequestGuard`，谁先关谁负责，不再重复关。

### 20.3 模型被静默换成豆包（用户报障：我选的是 deepseek-v4.1-flash）

`ModelSupportsVision()` 里有一条「名字带 `deepseek` 且不含 `vl`/`vision` → 一律纯文本」的硬规则，
而 **DeepSeek v4 起是原生多模态**（v4-flash / v4.1-flash 都能收图，
见 [apidog: V4.1-Flash Vision API](https://apidog.com/blog/deepseek-v4-1-flash-vision-api/)、
[IT之家](https://www.ithome.com/0/992/755.htm)）。于是连锁反应：

1. `ActionRequiresVisionModel(aiActionExecute + 带图)` = true；
2. `EnsureAiModelOnAction` 写库时把动作的模型换成列表里第一个识图模型（豆包）→ **存进脚本，永久生效**；
3. `ResolveActionAiModelName` 运行时同样先挑识图模型；
4. 编辑器 `RefreshAiModelCombo` 默认选**列表第 1 个**（添加顺序），新建动作也不是用户当前选的模型。

四处一起改：

| 改动 | 说明 |
|---|---|
| `ModelSupportsVision` | DeepSeek `v4`/`v5`/`vl`/`vision` 判为多模态；`chat`/`r1`/`reasoner`/`v3.x` 仍是纯文本 |
| `EnsureAiModelOnAction` / `ApplyResolvedAiModelToActionParams` | **AI 动作执行不再改写动作里的模型**（识图需要由运行时按能力决定），纯识图动作 `aiImageAnalysis` 保留兜底 |
| `RefreshAiModelCombo` | 默认选「设置→AI助手」里的当前模型，而不是列表第 1 个 |
| `RunAiActionExecuteForAction` | 动作模型与设置默认模型不一致时**打诊断**（模型来源必须可见，不再静默） |

效果：用户选 deepseek-v4.1-flash → 判定为多模态 → 规划轮直接用它并附观察截图（`ShouldAttachObserveImageToPlanner`
现在对它返回 true），不再换成豆包。真·纯文本模型 + 需要识图时仍会兜底换识图模型，但会在日志里明确写出来。

⚠️ **已有脚本里的动作**如果之前被写成了豆包，那个名字已经存在动作里 —— 需要在编辑器 AI 动作的模型下拉里
重新选一次（新建动作现在默认跟随设置里的模型）。

### 20.4 自检

新增/更新用例：`model_supports_vision`（DeepSeek v4 系判多模态、v3/chat/reasoner 仍纯文本）、
`action_model_not_silently_swapped`（选了多模态就原样用；写库不改写；纯文本+识图才兜底）、
`planner_observe_image_attach`（v4 系必须附观察截图）、
`game_foreground_vision_allowed`（4 组前台钉死）、
`history_sidebar_and_busy_guards`（补前台浏览器覆盖）。
AiActionRouterSelfTest 162/162，其余 9 套全绿，MCP 冒烟 exit 0。

### 20.4 游戏观察帧抬到 1024 长边

观察帧长边上限固定 768 → 2560×1440 缩成 768×432，一株植物/一个僵尸只剩十几个像素，
识图基本靠猜（这就是「图片识别缺陷很大」的一部分）。现在**前台不是浏览器窗口**时抬到 1024
（只有这类前台付这份 token），浏览器/办公仍是 768。

### 20.5 自检

新增/更新用例：`model_supports_vision`（DeepSeek v4 系判多模态、v3/chat/reasoner 仍纯文本）、
`action_model_not_silently_swapped`（选了多模态就原样用；写库不改写；纯文本+识图才兜底）、
`planner_observe_image_attach`（v4 系必须附观察截图）、
`game_foreground_vision_allowed`（4 组前台钉死：桌面游戏高动态放行且被判游戏前台、canvas 放行、
浏览器未知页型仍拦且不算游戏、表格软件不算游戏）、
`history_sidebar_and_busy_guards`（补「前台是浏览器」前置覆盖 —— 原断言默认命中真实前台）。
AiActionRouterSelfTest 162/162，其余 9 套全绿，MCP 冒烟 exit 0。

### 20.6 第四轮：游戏里「每步都要确认」「不理解机制」

用户实测（deepseek-v4-flash，流式与模型替换都已生效）后指出三件事：**每步都要反复确认**、
**两步操作中间隔好几秒**、**AI 不懂游戏机制**（PvZ「我是僵尸」里以为冷却只能放一个僵尸，
于是葫芦娃救爷爷式一个个送）。逐个处理：

#### ① 两步操作一次做完：`locateAndClick(targets=[…])`

以前「拿僵尸卡 → 放到草坪」要发两次 tool call，每次都走一遍：
主模型一轮（thinking 10~40s）+ 执行 + **UI settle 1.5~2.6s**。
新增 `targets` 参数（2~6 个不同目标）：宿主**逐个 VLM 定位并立即连续点击**，
中间不插观察/验收，一次工具调用完成、只做一次 settle。
任一目标定位失败**立即停**（不做「猜着点」），并明确告诉模型后面的没点。
实现：`AiActionHostHooks::onLocateMulti`（宿主把原来那条单目标链路包成
`std::function` 再复用，守卫/缓存/日志全部照旧），工具侧只做参数校验与计数。

#### ② 游戏前台不再等「界面稳定」

`WaitUiReactThenSettle` 的交互 settle 预算是 2.2s（点击后等界面画完）。
游戏画面**每帧都在变**，既等不到「稳定」也没意义 —— 日志里每个动作白等 1.5~2.6s。
现在 `AiActionGameForegroundLikely()` 命中时改用短节拍
（80ms 轮询 / 350ms 反应 / 150ms 稳定 / 900ms 上限），并且**不再提示「建议刷新/重开」**
（对游戏是噪音，还会诱发模型重新观察一轮）。

#### ③ 先懂机制再动手（写进 Skill + 每轮指令）

`skills/agent/game.md` 与内嵌 `MacroActionGameSkill()` 新增 **§0.5「先搞懂机制再动手」**：
抽象目标（「通关这关」）先用一轮确认 ①胜负条件 ②**资源/冷却是全局共享还是每个单位各自一份**
③操作机制（先选再点？一次能放几个？）④节奏，把一句可执行结论写进 `updateTaskMemo`；
操作已明确（点这个按钮）就直接做，别为省事而查。
并把这次的实例写进 Skill：**PvZ「我是僵尸」冷却按卡牌各自计算、阳光共享 →
选够卡后应批量/连续放多个形成波次**，推广规则是「凡是选单位放到场上的游戏，
先确认冷却是全局还是每单位独立，再决定要不要批量放」。

同时 `AiGameNudgeOnce()` 重写为 ⓪~⑥ 条（含「两步操作一次做完」「批量推进」
「同一步骤失败 2 次就换策略」），每轮游戏指令也点名 `targets` 用法。

#### ④ 顺手修掉一句会误导模型的话

`agent_core.cpp` 在省 token 剥图时写的提示是「截图已在上一轮流式请求中发送，
**请根据文字描述直接调用工具**」—— 模型把它理解成「我看不到画面，只能瞎猜」，
于是反复 `screenshot`、反复自问「我是不是看不到图」。改成
「本轮未附截图以省 token；需要看当前画面请调用 computer(action=screenshot)，
或直接用 locateAndClick 让宿主识图定位」。

#### 自检

新增 `locate_multi_targets`：多目标走 `onLocateMulti`（断言收到 2 个目标、没走单目标路径、
结果含 `[1/2]`）、单目标仍走原路径、只给一个 target 又不写 `target` 时明确报错。
AiActionRouterSelfTest 163/163，其余 9 套全绿，MCP 冒烟 exit 0。

### 20.7 第五轮：位置只找一次 —— 布局记忆 + 相对网格（通用能力）

用户提出把「成功逻辑复用」再抽象一层：**固定界面上的元素位置不该反复识图**，
并且规则排列的东西（草坪格子 / 卡槽 / 图标阵列 / 工具栏）**定位一格就该推出整片**，
要求做成**通用优化**而不是游戏专项。

先查了业界实现，用户这套思路正是三条成熟做法的组合：

| 做法 | 已有开源/产品实现 | 我们取哪条 |
|---|---|---|
| **元素定位缓存 / UI 对象仓库**：记住「怎么找到它」或「它在哪」，跨步骤/跨轮复用 | Midscene 的 caching（缓存 AI 规划**与定位**结果，[文档](https://v0.midscenejs.com/zh/caching)）、UiPath/Power Automate 的 UI Object Repository（[专利 11809846](https://patents.justia.com/patent/11809846)） | ✅ 本轮落地 |
| **相对定位**：由一个锚点推导邻居，而不是对每个目标重新匹配 | Sikuli 的 `Region.right()/below()/nearby()`（[邮件列表](https://lists.launchpad.net/sikuli-driver/msg55523.html)） | ✅ 本轮落地（升级成「整片网格」） |
| 一次解析整屏、模型只引用 ID | 微软 **OmniParser** / Set-of-Mark（[介绍](https://cloud.tencent.com.cn/developer/article/2502979)） | 已有等价物（UIA 树 / DOM 树 / `targets=[…]`），本轮不动 |

#### 新模块 `src/ai_ui_layout.{h,cpp}`

- **布局记忆**：键 = 归一化目标描述 + **窗口身份**（标题|类名|进程名|**客户区尺寸**）+ 屏幕尺寸。
  定位成功一次即记住屏幕坐标；之后同键直接点，**0 次识图**（日志打「布局记忆命中」）。
  失效规则（宁可重新识图也不能乱点）：换窗口/挪窗口/改客户区/改分辨率 → 键不匹配即失效；
  **点下去画面没变化 → 立刻作废**（沿用定位缓存那套「错点不固化」）；**每次开始跑脚本整表清空**
  （绝不让上一次运行的坐标影响这一次）。
- **相对网格**：`AiUiGridSpec{origin, stepX, stepY, cols, rows}` + `AiUiGridCellCenter(row,col)`。
  网格从哪来？用**画面自身的周期**推断：`AiUiDetectGridPeriod()` 对锚点所在行带/列带做
  **自位移平均绝对差** `d(lag)=mean|I(x)−I(x+lag)|`，取显著极小值当周期（谷底再做抛物线亚像素修正，
  否则 10 格后会累积十几像素误差）。显著性不足（不规则界面）就**明确拒绝**，让调用方老实逐个识图 ——
  这点很关键：硬套网格会点到不相干的地方。

#### 工具侧：`locateAndClick(grid={anchor, cells})`

```
① locateAndClick(grid={anchor:"草坪", cells:[[0,0]]})   ← 唯一一次识图定位 + 建立网格
② locateAndClick(target="僵尸卡")                        ← 卡槽各自定位一次（之后走记忆）
③ 之后每次放置：locateAndClick(grid={anchor:"草坪", cells:[[行,列],…]})
   —— 一次可点 ≤24 格，全程 0 次识图
```

宿主 `onLocateGrid`：锚点先查布局记忆（命中则 0 识图），未命中才走一次真识图；
网格同理（记忆 → 否则抓一帧做周期检测）；然后算出所有格心、**一个批次连续点击**
（中间不插观察/验收），越界/周期未知的格子明确回报「跳过哪些」，绝不静默乱点。

#### 通用性

键里没有任何游戏概念 —— 草坪格子、Excel 工具按钮、桌面图标阵列、播放器控制条
都是「同一窗口 + 同一描述 + 规则排列」，走的是同一条路径。桌面图标（间距不规则的）
会因周期不显著而**自动回落**到逐个 `targets`，不会误伤。

#### 自检

- `ui_layout_memory_and_grid`：记住/取回；窗口身份、分辨率、目标描述变化即失效；
  点击无变化（`Forget`）连网格一起清；`(行,列)`→坐标含负偏移；离谱越界被挡。
- `ui_grid_period_detect`：合成 9×3 网格（列距 120/行距 100）→ 检出 120（±4）；
  **随机噪声画面 → 不报周期**（不硬套网格）。
- `locate_multi_targets`（上一轮）：`targets=[…]` 一次定位连点。

AiActionRouterSelfTest 165/165，其余 9 套全绿，MCP 冒烟 exit 0。

### 20.8 第六轮：按第九份日志 —— 记忆误删 / 假网格 / 资源分配

用户给了新日志，并纠正了一条设计。三个问题全部来自日志实证：

#### ① 「点下去没变化就作废」是我判错了（用户纠正）

日志里每个 `locateAndClick` 后面都跟着
`[诊断] 布局记忆作废：照记忆坐标点击后画面无变化` —— 包括**刚识图成功、界面确实变了**的那些。
根因：`locateAndClick` 自己就要花 2~4 秒识图，等它点下去时 settle 的基线早就过期了，
`UI settle：已稳定 差分0bp` 是**误报**；小范围颜色采样同样会误报。于是布局记忆每次都被删掉，
等于白做。

按用户意见改成「**留着，用更多数据把界面模型建准**」：

- 记一笔 miss，**单次不删条目**；连续 2 次才让 `Recall` 回退真识图（条目仍在，下次
  `Remember` 刷新它），此时网格也一并作废重新推断；
- `Remember` 改为**多次定位取平均**（偏差 ≤24px 时按样本数加权平均，越用越准；
  偏差 >24px 说明界面真动过 → 直接换新值）；
- 新增 `AiUiLayoutStaleCount()` 便于诊断「有几条待刷新」。

#### ② 网格假阳性：暂停菜单上推出「79 列」

日志：`[诊断] 网格推断：返回游戏 周期 16×16px（右侧 79 列 / 下方 19 行）` ——
那是暂停菜单的石纹背景，16px 是**纹理周期**，照它点就是乱点。两道闸：

- `AiUiDetectGridPeriod`：新增「**网格间距不会远小于元素自身**」判据
  （`minPitch = max(28, min(锚点宽,锚点高) × 3/5)`；宿主的锚点框是定位点 ±24px）；
- 宿主侧再加**合理性闸**：推出来的 `cols > 24 || rows > 12` 一律拒绝并打日志
  （真实界面不会有几十列格子）。

#### ③ 资源分配：新增 `planSpend` 工具（先算账）

用户指出的两处策略缺陷都是**算术问题**：选卡点「一键全选」让弱卡占满卡槽（强卡进不来，
阳光明明够）；放僵尸只放 2~3 只就停手（阳光够放几十只）。这类「预算 + 槽位 + 一批候选」
是有确定解的组合问题，**不该让模型心算**——这是 plan-and-solve / tool-augmented reasoning
的标准做法（智能体用显式工具做算术）。

`planSpend(budget, slots, distinctOnly, items[{name,cost,value?,maxCount?}])`：
按性价比贪心给出「选哪些卡 / 这一波放哪些各几个」，并回报花了多少、剩多少；
`distinctOnly=true` 用于选卡（每种最多 1 张），`false` 用于放单位（预算内尽量多）。
纯函数 `PlanSpendBudget()` 在 `src/ai_plan_util.{h,cpp}`，与游戏无关
（商店购物、塔防放塔、卡牌对局、RTS 造兵同一套）。Skill 与 nudge 里写明
**禁止一键全选 / 禁止只放一两个**，并把 `planSpend` 列为选卡与放单位的前置步骤。

#### ④ 顺带修掉两处「逼模型反复确认」的噪音

- `定位校验=可疑` 的警告在**点击确实生效**（颜色/ROI 有变化）时**收回**，
  不再让模型去多截一次图确认；
- `locateAndClick` 的定位次数上限 12 → **24**：日志里 6 目标的 `targets=[…]` 调用
  被这条闸拒了（`本次动作定位次数将超上限`），而多目标调用恰恰是省轮次的关键路径。

#### 自检

`ui_layout_memory_and_grid` 扩到覆盖「单次 miss 不删 / 连续两次才回退 / 重新识图即恢复 /
多次取平均 / 偏差大就换新值」；`ui_grid_period_detect` 增加「周期 8px vs 60px 元素 → 必须拒绝」；
新增 `plan_spend_budget`（选卡按性价比挑强卡 / 阳光 3000 单只 600 → 一次 5 只 /
买不起要明说 / maxCount 限量生效）。AiActionRouterSelfTest **166/166**，其余 9 套全绿。

### 20.9 第七轮：文字坐标索引（通用「不必看图也能点」）+ 算账工具入口简化

第十份日志暴露的问题**全都不是 PvZ 特有的**，按通用能力修：

#### ① 「在界面呆着半天没反应」的真因：模型在盲回合里靠猜坐标

日志里 `电脑截图`/`computer(screenshot)` 反复出现，但模型自己写着
「images are being omitted to save tokens … I'm working blind」。它靠**目测缩图猜坐标**
（`mouseClick(196,42)` / `(215,43)` / `(200,45)`），点不中就再猜 → 触发近点连点守卫 →
`[错误] 已连续在相近位置左键点击` → 彻底卡住。**这是所有图形界面的通病，不是这一个游戏。**

**通用解药：本地 OCR 出「屏幕上有哪些文字、各自在屏幕的哪个像素」，作为纯文本注入每轮指令。**
不烧图片 token，却给出**可点元素的真实坐标**：

```
屏幕文字索引（本地 OCR，坐标为屏幕绝对像素；可直接 locateAndClick(target=其中任一文字) 或按坐标点）：
自选僵尸卡牌(537,107)；一键全选(2272,255)；返回游戏(1284,1136)；暂停(180,980)；…
```

实现：`AiObserveCaptureResult::textIndex`（`macro_execute_tools.h`）+
`observeScreenForAgent` 抓完当前帧后调 `RunOcrOnBitmap`（引擎未装则整段静默跳过，
沿用既有 OCR 环境判定）→ `ai_action_service` 每轮把索引拼在指令**最后**（最显眼位置）。
桌面软件/浏览器/游戏一视同仁：只要屏幕上有字，模型就能点到字，不必先看图。
（这条正是下面 §21 对 PP-OCR 评估的落地形式。）

#### ② `planSpend` 没人用：参数太难填

日志里模型明确写了「The guidance says use planSpend. But planSpend needs item names and
costs and values. I don't have values.」——然后**放弃算账、直接点了一键全选**。
工具没错，入口太重。新增 `costs` 简写：只给价格数组即可
（`planSpend(budget=30000, slots=16, distinctOnly=true, costs=[600,400,175,75,50,25,…])`），
`items` 仍是可选的高级用法；`required` 从 `["budget","items"]` 放宽为 `["budget"]`。

#### ③ 通用「换策略」硬约束（不限游戏）

新增：连续 2 轮重复工具批次或界面无变化时，指令里强制写一段
「**换策略**：换目标描述/换落点/换单位或加量（预算够就多上），或先 planSpend 算账；
禁止对同一位置反复重试」。用户的意见（「僵尸打不动就该换僵尸或加量」）在通用层面就是这句话。

#### ④ 顺带

- 文字索引让「盲回合」变成可执行回合，配合 §20.6 的布局记忆/网格，模型不必每轮重新推导。

### 20.10 第八轮：布局记忆「错命中」把选卡面板点关了（第十一份日志）

现象：**打开选卡界面后没选卡就回到了游戏界面**。日志给出完整链路：

```
locateAndClick(自选僵尸卡牌) → 屏幕(265,122)   ← 面板打开（正确）
locateAndClick(一键全选)     → 屏幕(2140,215)  ← 定位校验=可疑、settle 无反应（点偏了，但被记住了）
locateAndClick(一键全选)     → 布局记忆命中：一键全选 → 屏幕(265,122)  ← ★错命中
                             点的是「自选僵尸卡牌」的位置 → 面板被 toggle 关掉
```

两个**通用**缺陷（与具体游戏无关）：

1. **键不够保真**：布局记忆的键用了 `AiLocateNormalizeTarget()`（剥「按钮/图标」后缀、折叠空白）
   —— 那是给「定位缓存」做语义去重用的，拿来做**坐标记忆**的键会让不同目标互相污染。
   → 改为**按原始目标文本建键**（只做去空白+小写）。措辞不同就算新目标：miss 只是多识图一次
   （安全），错命中会点错按钮（危险）。
2. **把可疑定位也记了**：那次 `定位校验=可疑` + settle 无反应说明**很可能没点中**，却仍写进记忆。
   → 只有 `verdict == Accept`（且左键单击）才写入记忆与网格；可疑/需精炼一律不记，下次照旧真识图。

日志同时证明上几轮改动**已生效**：`[诊断] 文字索引 30 条（本地 OCR…）`、
`布局记忆记一笔未生效（连续 2 次才回退识图…）`（不再一有风吹草动就删条目），
而且模型确实在用 OCR 坐标校正自己（思考里写「OCR says screen (2157,257)」），
只是那次被错命中的记忆抢先点掉了。

### 20.11 第九轮：「卡在选卡界面思考半天」——两处通用问题

第十二份日志：AI 进了选卡面板后**没有选卡也没有点确认，只是不停思考**（单轮思考 3~7.5KB，
时间轴拉到 60s+）。日志给出两个**通用**原因：

1. **「界面未变 → 跳过上传」把帧也省掉了，而模型还没看过这一帧**：
   `[诊断] 本地观察：界面未变…跳过上传` 紧接着模型自问「I need to actually SEE the image」。
   通用规则：**只有上一轮确实执行过动作**，才敢用「界面未变」省这一帧；
   整轮一个动作都没做（纯查看/读取）说明模型还没消费当前画面 → 必须继续回传。
2. **文字索引被数字塞满**：`文字索引 33 条` 里绝大多数是价格（100/75/6666…），
   真正可点的**文字按钮**（一键全选/上一页/确认）反而被挤掉。通用修法：
   索引里「含字母或汉字」的条目优先、纯数字最多留 6 条并明确标注
   「带文字的才是按钮，纯数字是价格不用点」。

两条都写进 `observeScreenForAgent` / 观察处理路径，与游戏无关。

### 20.11 第十轮：高级加速总开关 + 记忆命中硬校验 + 批量选择硬门槛

连续三轮出现过「新机制自己变成故障源」（记忆误删 → 错命中点错按钮 → 省帧导致盲想），
所以这一轮按 1→2→3 补齐**可关、可校验、可拦截**三件事。

#### ① 「AI 高级加速」总开关（设置 → 宏回放设置，默认开）

`src/ai_fast_paths.h`（进程级原子量）。关掉即退回保守链路：**每步真识图 + 每轮回传整帧**。
已挂闸**四处**：布局记忆命中、相对网格推断、文字索引构建、观察帧省上传。
（OCR 文本核对是安全检查，不属于加速项，**不受此开关影响**。）
设置界面勾选框 `setAiFastPaths`；`AppSettingsStoreSelfTest` 断言 JSON 往返 + 立即置位/复位。

#### ② 布局记忆命中前的**外观签名硬校验**

记住坐标时同时留一份「外观签名」= 目标框外扩 20% 后的 8×8 格子平均亮度（64 字节）。
命中时用**当前画面**重算并比对（平均绝对差 ≤20 判为同一个）：不符 → 记一次 miss 并
**回退真识图**，绝不盲点。这正是「点错按钮」那类最危险故障的闸：面板被关掉/换页/内容变了，
同一坐标上的像素必然不同。无签名的旧条目**不拦**（避免误伤）。
自检 `ui_layout_signature_gate`：同帧通过、目标区被换掉必须拒绝。

#### ③ 选卡/批量选择**硬门槛**（通用关键词，不绑定某个游戏）

`locateAndClick` 的目标含 `全选/全選/批量选/全部选择/select all` 时，若**本次动作还没算过账**
（没调 `planSpend`）→ 直接拦下，并把可照抄的调用示例写进错误里：

```
planSpend(budget=预算, slots=卡槽数, distinctOnly=true, costs=[各卡价格…])
```

标记按**动作**复位（`ResetAiActionSessionState` → `AiResetPlanSpendGate()`），
不是「一次算账永久放行」。自检 `plan_spend_gate`：未算账被拦（且提示含 planSpend）、
算过账放行、普通目标不受影响。

#### 踩坑（第 2 次同一类问题）：文件作用域

`AiPlanSpendCalledThisAction()` 等三个函数第一次写在了文件上方的**匿名 namespace 内**，
与头文件的外部声明同时可见 → 调用处 `C2668 对重载函数的调用不明确`。
**改法：这类「供其它 TU 使用」的函数必须定义在文件作用域**（同 `AiGameNudgeOnce` 的注释）。
另外用脚本插代码时把门槛块插进了 `if (Trim(target).empty())` 与它的 `return` 之间，
形成「悬空 if」→ 所有调用都返回「target 不能为空」（10 个用例同时红）。
**教训：多行插入必须插在语句边界，插完立刻编译**。

### 20.12 第十一轮：用户问「咋选个卡牌和点个按钮都这么久」

第十二份日志里「点一次『自选僵尸卡牌』」的账：模型轮次 ×N + `locateAndClick` 里
**整屏 VLM 粗定位 + lazy Zoom 精炼两轮识图**（每轮 5~15s）+ 点击后 1.5~2.6s 界面稳定等待
+ 观察/Ocr/上传。两轮识图是纯浪费——**观察帧的本地 OCR 早就给出了这段文字的坐标**。

四条通用改动（都可关/可测）：

#### ① 文字直点：本地 OCR 索引命中就点，0 次识图

观察时本来就跑了一次本地 OCR（§20.9 的文字索引），现在**留一份带坐标的原始行表**
（`StoreOcrScreenIndex`，随观察帧更新，有效期 6s，每次跑脚本清空）。`locateAndClick` 在
DOM/UIA 之后、识图之前先查这张表：目标能从描述里剥成**纯短文本**（`AiLocateExtractOcrTarget`：
图标/方位/序号/格子/长句/纯数字一律不算）且**唯一命中** → 直接点该行中心。
过期的三种情形都静默回落识图：超过 6s、**前台窗口换了**（索引记了 `HWND`，窗口一换坐标必错）、
观察确认「画面结构未变」时反而**续期**（静态菜单上连点几个按钮不会 6s 后掉回识图）。
取点判据是纯函数 `AiOcrPickDirectClickTarget`（`ai_locate_verify`）：

- 三档匹配：完全相等 > 双向包含（长度差 ≤40%）；**不做模糊匹配**（OCR 模糊命中容易点到隔壁按钮）；
- **同屏多个同名候选**（两个「确定」相距 >16px）= 歧义 → 拒绝，回落识图；
- 命中行框过大（宽 >观察区 1/6 或高 >1/8、面积 >3%，即「整块面板被并成一行」）→ 拒绝；
- 低置信度（<0.5）行不参与。

命中即 `verdict=Accept`（OCR 是真读到这段文字才点的），不再让模型看到「可疑」回头确认一轮。
未装 OCR 引擎 / 帧过期 / 目标不是文字 → 整段静默回落原阶梯（与 §20.9 的「装了才走」一致）。
日志：`文字直点：本地 OCR 索引命中「自选僵尸卡牌」(exact) → 屏幕(254,103)，未截屏未识图（省 1~2 轮 API）`。
自检 `ocr_direct_click_pick`。

#### ② 小标签/小卡片不再白烧一轮 Zoom（`smallLabel` 放行）

`ShouldAcceptCoarseLocateWithoutRefine` 原来只有两条放行：`compactBox`（边长 ≤观察区 8%）
与 `wideControl`（高 14~56、宽 ≤420、面积 ≤2%）。实测「正常模式（要过关点这个）」的粗框是
**245×93**：宽超 8% 被 compactBox 拒、高超 56 被 wideControl 拒 → 白烧一轮 15s 的二级 Zoom，
而那一轮还答错了（模型回「编辑模式」，漂移被拒，最后仍点一级框）。现在加第三条按**相对面积**
判：面积 ≤1%、宽 ≤观察区 1/6、高 ≤1/8、长宽比 0.3~6、且 ≥32×28 并有一边 ≥56 → 直接点。
下界与宽度上界都必须留：前者挡任务栏邻近小图标，后者挡「比真行更宽的菜单行」
（实测 533×40 点中心会点开「设置」）。自检 `wide_row_still_needs_refine`（含 245×93 用例）。

#### ③ 一次性按钮不进复用缓存（用户原话：「不是每个地方的按钮都需要抽象出来」）

判据只有一个：**这个位置后面还会不会再点？**
`NoteLocateTargetSeen` 按目标文本计数（每次跑脚本清空），**第二次**才允许写缓存：

- 首次定位 → 不存定位模板、不写布局记忆、不算外观签名、不推断网格；
- 顺带把「布局签名」与「网格推断」原本**各自的整区截屏 + 转灰度**合并成一次（省一次全窗转换）；
- 网格锚点用 `MarkLocateTargetReusable()` 显式标记为可复用（网格锚点必然反复用；
  否则第 2 次 `grid` 调用会又识图一次并**再点一次锚点格**——那是一次多余点击）。

收益有两层：省掉一次性点击的整窗截屏/灰度/签名开销；更重要的是**消灭了一类故障** ——
一次性坐标被记忆固化后，界面一变就点到别处（§20.10 的「一键全选」就是这么点错的）。

#### ④ 模型明确要截图 → 必须回传这一帧

日志里模型调用 `computer(action=screenshot)`，偏偏那一轮「界面未变 → 跳过上传」，
于是它**根本没有图**（历史图已被剥成「(历史截图已省略)」），当场自问「我看不到图」并空转两轮。
现在 `computer(action=screenshot)` 会置 `NoteAiExplicitScreenshotRequest()`（消费一次即清），
宿主观察时 `skipUnchangedCheck` 一并成立 → 强制回传。自检 `computer_alias_tool` 加了
「截图强制回传 / 无残留标记」两条断言。

#### 顺带修掉一个假象：`ai_ui_layout` 的跨库依赖

`SCRIPT_CORE_COMMON` 是轻量库，而 `ai_ui_layout.cpp` 调了 `process_utils.h` 的
`ProcessImageNameByPid` → 只链 `script_core_common` 的自检目标（`ImageMatchSelfTest`、
`WindowModeSelfTest`、`ScriptActionBuilderSelfTest`）**全部 LNK2019 链接失败**，
目录里留着上一次成功构建的旧 exe ——「自检全绿」其实是跑在过期二进制上。
改成文件内自带的 `LocalProcessImageName()`（`QueryFullProcessImageNameW`），
并重建整个解决方案 + 全量跑 15 个 suite（见 §20.4 同款清单）。

### 20.13 第十三轮：定位精度（用户问「能不能用光标辅助定位」+ 让参考开源方案）

第十三份日志暴露了一次**射失**：

```
[诊断] 文字直点：OCR 索引已过期(7969ms) → 回落识图          ← 模型「看图→想 8s→才动手」，6s TTL 必然过期
[诊断] 第1级矩形 api[49,54,121,73] → 屏幕中心(226,169)      ← VLM 粗框（真实文字在 OCR 的 (245,106)，低了 63px）
[诊断] 一级可点(归一化紧凑框 192×50)，跳过二级 refine         ← 带着 63px 误差直接点了
```

#### 调研结论（三份子代理报告，逐条核实过原始论文/源码）

| 设想 | 开源界/学术界的实际情况 | 判决 |
|---|---|---|
| **移动真光标 + 截图含光标 + 问偏移 + 迭代** | 有一个真实实现（[GUI-Cursor, ICML 2026](https://arxiv.org/abs/2509.21552) + [`LufeMC/gui-g2-3b-ccf`](https://github.com/LufeMC/gui-g2-3b-ccf)），但它问的是**绝对坐标**不是偏移；而且**实测退步 15pp**（桌面 91.3%→74.7%，`mean_steps` 恒为 4.0「学不会停」）。**问偏移这一形态至今零先例** | ❌ 不做（先例失败 + 结构性劣势：32px 光标在 960 宽上传图里只剩 ~12px） |
| **网格/标尺叠加** | 强**模型门控**：[2509.11548](https://arxiv.org/abs/2509.11548) 里 Axis-Grid 让 Gemini-2.5-Flash 5.5→56.9、GPT-4o 20.8→45.8，但让**原生 grounding 模型** Qwen2-VL-7B 50.2→12.9 | ⚠️ 只能当**可关的实验**，绝不能默认 |
| **SoM 编号框** | [UGround Table 4](https://arxiv.org/abs/2410.05243)：SoM 25.6 **低于**坐标回归 46.8、也低于纯文本元素列表 42.3 | ❌ 不做 |
| **裁切放大二次通过（zoom-in）** | 证据最强：#1 与 #19 的 ScreenSpot-Pro 方法都叫 "Zoom In"，**2B 模型靠 zoom-in 打赢所有单次通过的大模型**；MEGA-GUI ROI 缩放边际增益 **+28.47pp** | ✅ **做**（本轮已做） |
| **本地 OCR 精定位** | 全 benchmark 一致：**text ≫ icon**（榜首 90.4 vs 70.4）；且 OCR **0 额外 API**。GUI-Cursor 项目唯一打赢基线的成果是零训练的「裁切再问」 | ✅ **做**（本轮已做，且**只对有文字的目标**做 = 天然的类型门控） |
| **红叉闭环（PrecisionCUA）** | [arXiv 2604.13019](https://arxiv.org/abs/2604.13019)：在**干净图**上于上一轮预测点画红叉 + 要求**绝对坐标** → Claude 21%→45.4%、GPT 13.5%→41.0%。**但同一论文里「让模型用锚点估算相对位置」的提示词把 GPT 从 41.0% 打到 18.5%** | ✅ **做**（本轮已做，只标上一轮落点、仍要绝对坐标） |

UI-TARS-desktop 侧可借鉴的（[repo](https://github.com/bytedance/UI-TARS-desktop)）：动作空间用 **bbox 取框心**（我们已是）、**一轮多动作**（我们已有 `targets=[…]`）、**图片滑动窗口 + 预算**、**归一化 box 与像素坐标分两个字段**、`Reflection` 段写回历史前剥掉；但它**没有任何** zoom/crop/二次放大、**不用** UIA 补强、**不用**光标位置——所以我们的 Zoom refine + 本地 OCR 校验**比它强，不要为了「对齐官方」而删掉**。

#### 本轮落地（都带自检）

1. **文字直点 TTL 6s → 90s，且用之前**就地复核**（`ocrProbeText`）
   6s 的 TTL 在「模型想 8~40s 才动手」的现实里几乎必然过期（日志逐字为证）。
   现在放宽到 90s，但命中索引后**裁一小块现场重新 OCR**（几十毫秒），文字还在原位才点，
   并顺手用**新识别到的框中心**修正落点。时间长短说明不了画面有没有变，实测一次才是。
2. **文字坐标覆盖识图点（P3，本轮最有价值的一条）**
   识图之后，只要目标是纯文本，一律再用 OCR 覆核：① 索引里离识图点最近的同名文字（≤300px）
   → 就地复核；② 索引没有 → 直接在识图点周边 ±120px 重新识别。
   拿到文字框中心就**改用 OCR 坐标**（差 >6px 才动），并记「可用」。
   这正是那次射失的解药：VLM 说 (226,169)，OCR 说 (245,106)。
   匹配只吃 **完全相等 / 双向包含**（长度差 ≤40%），**不做模糊匹配**；同屏多个只取**最近**的。
3. **小数坐标解析（P0，真 bug）**
   `TryParseCoordinatePair`/`TryParseBoundingBox` 原来用 `wcstol` 逐个抓整数：
   `(88.5,117.3)` → y 从 `.5` 解析失败 → 整条判「无法解析」（白烧一轮）；
   `[52.4,100.2,…]` → **静默错框**（抓 52，再把小数点后的 5 当 y1）。
   现改为 `wcstod` + 四舍五入，自检 `locate_decimal_coord_parse`。
4. **二级精炼真的放大（P1）**
   原来 ROI = 粗框 × 3.0 再钳到 720 → 上传 768 只有 **×1.07**（等于没放大，白花一轮 API）。
   现按粗框自适应：`side = 粗框 + 两侧各 max(60, 0.6×粗框)`（典型小按钮 → **×1.8**），
   容错仍覆盖 40~60px 的粗框误差。
5. **放大图上标「上一轮预测点」红叉（P2）**
   `DrawPredictionCrossOnBitmap()`（本机 GDI，<1ms，臂长≈图宽 5%），prompt 说明这是上一轮的落点、
   **仍要绝对坐标**。只标上一轮、不累积；`QST_NO_PRED_CROSS=1` 可关掉做 A/B
   （叠加类改动的收益是**按模型分档**的，必须能一键回退）。
6. **工具返回同时给两套坐标**
   `屏幕像素(226,169)＝归一化(88,117)（0~1000，与 computer/mouseClick 同一套）`
   —— 日志里模型为「这是屏幕还是图上的坐标」自问自答烧掉好几轮。
7. **游戏前台判定阈值 0.06 → 0.03**
   同一局游戏里覆盖率在 0.05~0.22 之间跳，认不出就要等 2.2s 界面稳定 + 误报「建议刷新/重开」。

#### 自检抓到的真 bug（值得记一笔）

重构把 `AiOcrPickDirectClickTarget` 的分档抽成公共函数时，索引写成 `tiers[tier-1]`
（tier 2=完全相等、1=包含）→ **两档颠倒**，「exact」用例当场变红（`kind=contains`）。
正确是 `tiers[2-tier]`。**没有这条自检，这个 bug 会静默降低文字直点/文字覆盖的可靠性。**

### 20.14 错点自纠（用户提的「光标辅助定位」的**备用项**形态）

用户的观察很准：**「点错了」这件事本身就是一个已知信息 —— 那个错点坐标在我们手里**
（就是刚 `locateAndClick` 点下去的位置）。这正好是 PrecisionCUA 红叉闭环需要的锚点，
而且**不必动真光标**：把红叉画在**放大图**上（本机 GDI，<1ms），让识图子模型对着红叉
重新给出目标的**绝对坐标**，然后补点一次。

触发条件（都在 `locateAndClick` 内，一次调用最多一次）：

- 左键单击、不是照定位缓存点的；
- 点下去**没有任何变化证据**（邻域颜色未变 + 变化 ROI 里没有小范围命中 + 不是大范围动态画面）；
- 预算没超（`missSelfCorrectBudget = 3`/次运行）；`QST_NO_MISS_SELFCORRECT=1` 可整体关掉。

实现要点：

1. 裁「错点 ±260px」的放大图 → 在**错点位置**画红叉 → `EncodeBitmapForAiZoomUpload(768)`
   → `RunAiOneShotVisionQuery()`（新增的一次性识图入口：自建 core、无工具、无历史）。
2. prompt 由 `BuildMissSelfCorrectPrompt()` 生成：说清「刚才点了它、没有任何反应」+
   「红叉=刚才点的位置」+ **只要绝对坐标**（`❌不要输出偏移量`）。
   自检 `miss_self_correct_prompt` 把这条契约钉住。
3. 拒绝补点的兜底：模型说 NOT_FOUND、没给坐标、自纠点与错点**重合（<24px，没有新信息）**、
   不在前台窗口上、命中重复点击守卫 → 全部放弃，只把原因写进工具结果。
4. 补点成功后，工具结果里同时给出**两套坐标**与差值，并明确「不要再重复这两个坐标」。

**为什么不直接用真光标伺服**（调研结论，见 §20.13 表）：唯一开源实现（[GUI-Cursor](https://arxiv.org/abs/2509.21552)
/ [`LufeMC/gui-g2-3b-ccf`](https://github.com/LufeMC/gui-g2-3b-ccf)）实测**退步 15pp**，
且它问的也是绝对坐标；真光标还有「32px sprite 在 960 宽上传图里只剩 ~12px」「移动光标会触发 hover」
「拖影/动画光标/`ClipCursor` 各自出错」这些结构性坑。**红叉是它的合成替代品，且零副作用。**

#### 又一次踩到同一个坑：匿名 namespace

`RunAiOneShotVisionQuery()` 第一次写进了 `ai_action_service.cpp` 的**匿名 namespace 内**
（该 namespace 从 1346 行开到 1642 行），于是变成**内部链接**，`engine_script_run.cpp`
引用时报 `LNK2019 无法解析的外部符号`。**这是本仓库第二次栽在这条上**（第一次是
`AiPlanSpendCalledThisAction`）。规矩：**供其它 TU 使用的函数一律定义在文件作用域。**

### 20.15 第十三次日志：整个宏被「空响应」打死（思考预算不够是根因）

失败链（逐行可对）：

```
生成回复中…                                        ← 流式回复
输出被截断，改用完整响应重试（省略截图）…            ← finish_reason=length，落进非流式重试
已收到响应头，读取响应体…
[错误] API 请求失败：服务器返回空响应。              ← HTTP 200 但**正文为空** → 整个动作判失败
[2]结束宏运行                                      ← 宏当场结束
```

两处都要修，而且要分开修：

1. **根因：AI 动作执行的 max_tokens 被硬钳在 2048。** 思考型模型（我们默认**允许思考**）
   的思考 token 也算在 max_tokens 里 —— 一轮较长的思考就会在**中途**被截断
   （`finish_reason=length`），于是掉进「非流式重试」；重试若同样被截断，网关给出的就是
   空正文。`CreateAiActionExecuteCore` 与 `ai_action_runtime.cpp` 的复用分支都钳在 2048，
   现改为工具轮**下限 8192**、上限 16384（再高会把单轮思考拖成「卡死」，
   见 `webview_bridge_backend.cpp` 的既有注释）。
2. **韧性：空响应/连接中断不再一次就判死。** 非流式回退路径加**静默重试 2 次**（间隔 1.2s，
   仅对「空响应/连接/超时」这类可重试错误；HTTP 4xx 与用户取消不重试），仍拿不到正文才报错。
   网关侧空正文本来就是间歇性故障，一次就把整个宏打死代价太大。

### 20.16 第十四次日志：**我加的硬门槛把人带沟里了**（用户：「规划了半天，结果是个错误的方向，选卡只选了一张」）

复盘第 3 轮，逐字：

```
调用工具：locateAndClick 目标：一键全选
工具返回错误：[错误] 这是一次性批量选择（「一键全选」）。**先算账再选**：调
             planSpend(budget=预算, slots=卡槽数, distinctOnly=true, costs=[各卡价格…])
```

这个门槛（§20.11 ③）要的是**模型根本拿不到的数据**：54 张僵尸卡的价格是卡面上的小数字
（OCR 只零星读到几个），卡槽数也未知。后果是灾难性的连锁：

| 轮次 | 模型做了什么 | 代价 |
|---|---|---|
| 3 | 被拦 → 放弃整个选卡面板 | 一轮 |
| 3 | `listUiControls`（游戏只有 4 个窗口控件） | 一轮 |
| 4 | 重新截图、又推了一遍玩法 | 一轮（思考 3.7KB） |
| 4-5 | 点单张卡（只加进去 1 张） | 一轮 |
| 5-6 | **按 Escape** 开暂停菜单、在菜单里分析两轮 | 两轮（思考 3.9KB+3.1KB） |
| 7-8 | 回到游戏、乱点草地「放僵尸」 | 两轮 |

**根因不在模型**：它拿不到算账需要的数据，而门又只对它关着。

#### 修复 ①：`planSpend` 门槛从**硬拦**降级为**软提示**（动作照做）

关键词命中且没算账时，**照常执行点击**，只在工具结果里追加一段提醒
（「若卡槽/预算有限，先 planSpend 拿名单；若确认该全选就忽略本条」）。
自检 `plan_spend_gate` 同步改成断言「照常执行 + 带提醒 + 算过账不重复提醒」。

**沉淀成规矩**：**挡住路又不给可行替代，比不拦更糟。** 任何硬门槛都必须同时满足
「模型有能力满足它」；不能满足的，只能给提示。用户最初说的「不要写硬约束，写进 skill 让 AI 自己理解」，
这次用日志证实是对的。

#### 修复 ②：自绘前台**不再依赖「画面在动」**才被认出来

同一份日志里，前 4 轮**一次都没注入**游戏 Skill 与「每轮至少落一个动作」的指引
（日志里没有任何「游戏/自绘前台」诊断）—— 因为开局画面几乎静止，动态覆盖率低于阈值，
`AiActionGameForegroundLikely()` 判 false。于是模型只能自己从零推 PvZ 玩法，
光思考就烧掉几十秒还推错方向（「规划了半天」）。

新增判据：**前台没有控件树 = 自绘画面**（`ForegroundWindowLooksSelfDrawn()`：
`EnumChildWindows` 数子窗口 ≤2，且排除控制台/终端类）。游戏把整屏画在自己窗口上，
子窗口≈0；Notepad/资源管理器/对话框都有一排子控件。这条比「动态覆盖率」可靠得多，
而且代价极低（微秒级）。控制台/终端显式排除，交给 UIA 路线。

### 20.17 第十五次日志：**错点自纠把开关按钮点了两下**（「拿下来一张卡之后就点不动了」）

日志逐字：

```
执行 鼠标点击左键@330,123   UI settle：仍在变化 耗时1589ms 差分127bp     ← 画面明明变了 1.27%
执行 鼠标点击左键@252,98    [诊断] 错点自纠：红叉=330,123 → 识图给出 252,98，已补点一次
...
执行 鼠标点击左键@505,142   UI settle：无反应
执行 鼠标点击左键@592,139   [诊断] 错点自纠：红叉=505,142 → 识图给出 592,139，已补点一次
```

`自选僵尸卡牌` 是**开关按钮**：第一下开面板，宿主补的第二下又把它关回去 → 面板进入不一致状态，
之后点什么都没反应（用户报障「拿下来一张卡之后就点不动了一直在那个界面」）。

**根因**：`tryMissSelfCorrect` 的触发条件只看了「邻域颜色没变 + 变化 ROI 里没有小范围命中 +
不是大范围动态」，**唯独没看 settle 的权威判定**（那两行明明写着 `仍在变化 差分127bp`）。

**修复**：`NoteAiUiSettleReacted()` / `AiLastUiSettleReacted()`（settle 判定投影进 `AiSession`），
自纠触发条件加 `&& !AiLastUiSettleReacted()`。
**规矩：任何「点没中就补一下」的逻辑，必须先问 settle 的权威判定，不能靠局部采样推断。**
开关类控件上多点一下不是「多花一次点击」，而是**把界面搞成不一致状态**。

### 20.18 删除「批量选择」关键词门槛（用户：「不要做针对性的优化，要做通用的」）

§20.11 ③ 加的「全选/批量选/select all → 要求先 planSpend」正式**整段删除**，
连带 §20.16 的软提示版本也删掉。两条理由都写进代码注释：

1. **针对性**：拿关键词去认某个界面的按钮，本质是给个别游戏打的补丁；
2. **挡路又无替代**：门槛要的是模型拿不到的数据，被拦后它放弃整面板绕了 4 轮（§20.16）。

宿主只做**能力**（能点、能定位、能验证、失败能自纠），不做**领域规则**（该选几张卡、该不该全选）。
后者交给 Skill 与模型判断。自检改为断言「批量选择完全无特判」。

### 20.19 第十六次日志：提速（用户：「规划路径还是太慢，选完卡停在那里半天思考」）

先看这一轮的账（日志逐字）：

```
流式等待 5s，思考 4329 字节…    流式等待 10s，思考 8407 字节…
流式等待 15s，思考 12541 字节…  流式等待 20s，思考 16623 字节…
流式等待 25s，思考 20900 字节…  流式等待 30s，思考 24821 字节…
流式等待 35s，思考 28550 字节…  流式等待 40s，思考 32227 字节…
流式等待 45s，思考 36430 字节…
```

**一轮持续思考 45s、吐出 36KB 推理**，而且内容是在反复重推同一个决定（「要不要点一键全选」）。
请求体也一路涨到 **314~330KB**（每轮重发一遍它自己的旧推理）。三处通用修复：

#### ① 不回灌模型自己的长推理（`BuildRequest`）

`reasoning_content` 原来整段回灌进后续每一轮请求 —— 请求体因此线性膨胀，
而且**模型读到自己上一轮的纠结会接着纠结**（这正是「停在那里半天思考」的机制）。
现在：网关明确要求回放的（`requires_reasoning_content`，如方舟思考模型）保持原样；
其余**只留最后 400 字**尾巴。省下的是每轮的传输与 prefill，也切断了「自我复读」的正反馈。

#### ② 「只想不干」→ 接下来两轮**关掉思考**（通用）

`ShouldDisableThinking()` 原来恒返回 false（全放开思考）。新增动态抑制：
`SuppressThinkingForNextRounds(n)`，在**「思考完却没有调任何工具」那一刻**触发（两轮）。
理由：那一轮已经把该想的想完了，下一轮再让它想只会重复；关掉思考直接逼它出手。
逃生阀 `QST_FAST_THINKING=1` 的旧语义保留。

#### ③ 去掉自相矛盾的绝对禁令

`MacroActionGameSkill()` 里写着「**禁止**一键全选」，而宿主早就不拦这个动作了 ——
模型于是卡在「doc 说禁止 / 但我需要」之间反复权衡（上面 36KB 推理的主因）。
改成按条件判断的说法，不再用绝对禁令。

**通用性**：三条都不是游戏特判 —— 分别属于「上下文工程」「循环控制」「提示词一致性」，
对任何任务都成立。

### 20.20 第十七次日志：**依赖链的第一步没生效，后面全在空转**（用户：「没选成功就放僵尸，结果什么都没做」）

日志逐字：

```
submitMacroActions 计划 4 个：①(122,47)卡片 ②(700,115) ③(700,200) ④(700,285)
执行完 → 太阳仍 29925，与操作前**完全相同** ⇒ 4 次点击一次都没生效
（下一轮换成 (170,45)+5 个格子，又重复一遍）
```

模型把「选卡 → 放」整条链当成一次盲批量提交了。`submitMacroActions` 一批只结算一次，
而**动态画面上结算只会说「仍在变化 218bp」**——「第一步其实没点上」在结算里根本看不出来。

#### 修复 ①（宿主侧，通用）：盲批量的**首次点击生效校验**

`executeOne` 的批量路径里，每步执行后记下**第一次点击的落点**（点完光标就在那儿，
不需要坐标换算），结算后拿 settle 的变化区判断它有没有在附近引起**小范围结构变化**
（>220×180 或面积 >48000 的算动态区，不算），于是给出事实：

```
[事实] 本批第一次点击屏幕像素(305,117)＝归一化(119,81) 附近没有任何结构变化（变化都发生在别处）：
该步很可能没生效（点错了/没选中/不可点）。★后面的步骤依赖它，可能全在空转 ——
先单独确认这一步的状态（看截图、或再点一次这一步），再继续后续。
批量做「先选中/先切换 → 再作用于目标」这类链条时应改用 locateAndClick(targets=[…])（逐步校验、失败即停）。
```

整屏都没有可归因变化时另给一条更短的结论。**判据全是通用信号**（点击点 + 结构变化 ROI），
不带任何游戏/软件知识：选工具再画、选卡再放、切标签再填都是同一条链。
日志另有 `[诊断] 批量逐步校验：点击 N 次，首次点击附近已变/无变化`。

#### 修复 ②（提示词侧）：依赖链不许用盲批量

动作指引里补上判据：**批量第一步是「选中/切换」类动作时，先单独做它并确认状态变了再做后续，
或整条链都走 `targets`**（`targets` 逐步校验、失败即停）。

#### 踩坑记录：变量名撞上 Windows 历史宏

`const bool small = …` 报 `C2187 此处出现意外的"char"` —— windows.h 的旧兼容宏里
`small` 就是 `char`（同类的还有 `near`/`far`/`pascal`）。**改名为 `roiSmall` / `nearHit` 即通过。**
这类宏只在标识符**整个 token** 相等时展开，所以 `sawSmallRoi` 不受影响。


## 21. 「本地识别」能带来什么：YOLO 与 PP-OCR 的评估结论

用户问：网上看到的 **YOLO 检测**与最新的 **PP-OCRv6**，对本品有没有搞头。结论分两半：

### 21.1 PP-OCR / OCR 文本坐标索引 —— **已落地，价值最大**

OCR 是**通用**的（任何界面上的文字都能被读出来），而且**不需要图片 token**。
本轮已按这个思路落地「屏幕文字索引」（§20.9）：本地 OCR → 文字 + 屏幕像素坐标 → 纯文本注入。
收益：

- 模型不必靠看图猜坐标（这是第十份日志里「呆着没反应」的根因）；
- 每轮请求体不再必须带 127KB 大图（配合 §20.6 的布局记忆，多数轮次可只发文本）；
- OCR 对中文 UI 文字（按钮名、卡牌花费、菜单项）本来就比 VLM grounding 稳得多，
  而且 `locateAndClick(target=<文字>)` 的既有链路本来就用 OCR/UIA 做校验，天生长在一起。

PP-OCRv6 相对现有 RapidOCR(ONNX)：中文小字/竖排/低对比度更稳，但要**多带一套模型与运行时**。
本品已有「OCR 引擎可选安装」的现成管线（`CheckOcrEnvironment` / `RunOcrInstall`），
所以正确做法是**升级可选的 OCR 引擎版本**（把 PP-OCRv6 的检测+识别 ONNX 作为可选后端），
而不是把 OCR 变成硬依赖。**下一步**：把 `RunOcrOnBitmap` 换成/并联 PP-OCRv6 后端，对比
「花括号按钮名 + 数字」的识别率与耗时（目标：整屏 ≤120ms）。

### 21.2 YOLO 通用目标检测 —— **暂不引入，理由如下**

- **通用 YOLO 不认识本品目标**：PvZ 的僵尸、Excel 的按钮都不在 COCO 类别里，
  要它可用就得**逐游戏/逐软件训练**——那正是用户反对的「特化」，且每个新场景都要重训。
- 业界真正用 YOLO 的地方是 **OmniParser 式「图标候选框检测」**：用 YOLO 找出「像可交互元素」的
  方框，再交给模型/OCR 命名（Set-of-Mark，模型只引用编号）。这条路对我们**确实有价值**，
  但收益点在于「未知界面、图标多、没有文字」的场景 —— 而这类场景我们已有两条更便宜的替代：
  ① **UIA 控件树**（桌面软件，精确、免费）；② **周期网格推断**（卡槽/草坪/图标阵列，§20.7）。
- 成本：ONNX Runtime + 模型文件（数十 MB）+ 每帧一次推理（CPU 上 20~80ms）× 维护。
- **结论**：先做 OCR 文本索引（已做）与 UIA/网格（已有）；等遇到「大量纯图标、无文字、
  无控件树」的真实场景（例如某些游戏 HUD）再考虑引入轻量图标检测器，
  并把它做成**可选插件**（缺模型自动回落现链路）。

### 21.3 下一步（按用户已批准的两条）

1. **观察帧降采样 + 只在必要时上传**：布局记忆/文字索引建起来后，多数轮次用 640 长边低清帧
   （或干脆只用文字索引），把每轮请求体从 ~230KB 压到 ~80KB。
   【部分已具备：文字索引已在每轮注入；降采样阈值待接】
2. **把已确认的机制与坐标固化成一句清单**：宿主主动回报
   「卡槽坐标已记住：第1槽(537,107)…；草坪网格已建立：5×9，周期 240×180」，
   把模型从「每轮重新分析」推向「直接下单」。【文字索引与布局记忆已各自可用，
   合并成一句「已确认清单」待接】

### 21.4 还没做

1. **游戏画面理解仍靠整帧 VLM**：一帧 1024 长边 + 多轮观察仍是「每轮都问模型」。
   游戏技能里写的正解是「VLM 找一次 → 本地找图/颜色跟住」（`skills/agent/game.md`），
   模型目前只是被**提示**这么做，没有强制。要真正省轮次，得让宿主在游戏前台时
   把「刚定位到的目标」自动转成 `findImage`/`findColor` 跟随后续动作（脚本化），
   这是下一轮的正经活。
2. **停止/取消延迟**：轮询切片 250→1500ms 后，按停止最多多等 1.5s 才中断流式读。
   要更跟手就得把流式改成 WinHTTP 异步回调（`WinHttpSetStatusCallback`），
   属于结构性改造，先记账。
