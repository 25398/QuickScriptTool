# QuickScriptTool 架构评估与重构清单

> 评估日期：2026-09-18
> 评估范围：`src/`（282 文件 / 135,483 行）、`ui/`、`CMakeLists.txt`、`tools/`、发版链路
> 评估角度：分层合理性、模块边界、可测性、可演进性、与同类软件的架构差距
> 本文件为**评估结论**，不含任何代码改动。

---

## 一、项目现状速览

### 1.1 代码规模

| 区域 | 文件数 | 行数 | 说明 |
|------|--------|------|------|
| `src/` 总计 | 282 | 135,483 | 平均 480 行/文件 |
| `src/window_mode/` | ~45 | ~29,000 | 最大子系统（后台窗口输入 / CDP / 注入 / 虚拟桌面） |
| `src/engine/` | 9 | ~25,956 | 单文件最大 13,885 行 |
| AI 子系统（`agent_*` + `ai_*`） | 40 | ~20,300 | |
| `ui/` 前端 | 10 | ~30,700 | `app.js` 单文件 17,273 行 / 650KB |
| `tools/` 自检 | 25 | — | 21 个 SelfTest 目标 |

### 1.2 当前分层（设计意图）

项目已有明确的五层划分，并在 `docs/webview-native-layering.md` 中声明为"归属真值"：

```
┌─────────────────────────────────────────────────────┐
│ WebShell     WebView2 宿主窗 + bridge 路由 + ui/     │  ← 唯一产品壳
├─────────────────────────────────────────────────────┤
│ Engine       qst::engine 门面（headless）            │  ← 跑宏/录制/热键/设置
│              engine_host_window.h / engine_script_run│
├─────────────────────────────────────────────────────┤
│ DesktopTools 选区/叠层/准星/托盘/悬浮球（原生 Win32） │  ← 永久原生
│              qst::desktop_tools 门面                 │
├─────────────────────────────────────────────────────┤
│ GdiLegacy    旧 GDI 对话框（QST_GDI_LEGACY=OFF 时桩） │  ← 可删
├─────────────────────────────────────────────────────┤
│ Shared       找图/OCR/输入后端/脚本 IO/坐标          │  ← 多目标共用
└─────────────────────────────────────────────────────┘
```

### 1.3 已经做对的事（不要动）

这些是项目的架构资产，重构时应保护而非推翻：

1. **WebView2 壳 + 保留 C++ 引擎**（未走 Electron 整仓重写）—— 决策正确，规避了 Electron 的体积、内存与 `SendInput` 权限问题。
2. **门面模式**：`qst::engine` / `qst::desktop_tools` / `qst::webview` 三个命名空间是清晰的单入口，CMake 依赖单向无环。
3. **21 个 SelfTest + 统一 harness**（`tools/selftest_harness.h`，`--json` / exit code = 失败数）—— 这个基建质量超过绝大多数同类国产工具，是后续重构的安全网。
4. **`window_mode/` 子系统分层最规范**：`types → json → permission/log → session → window_target → executor → 输入后端`，是本项目唯一做到了"一个模块一件事"的部分。
5. **AI 能力以 Skill 文件定义**（`skills/agent/*.md`），随包分发、可热更新，而非硬编码进 Prompt —— 数据驱动的正确做法。
6. **低性能模式 = 单一进程级原子量**（`low_power_mode.h`），并明确约定"加新降耗点必须读同一个开关"。
7. **动作语义有文档约束**（如 `duration` = 重复间隔），且有自检断言保护。

---

## 二、与同类软件的架构对比

### 2.1 对比表

| 软件 | 技术栈 | 脚本模型 | 执行模型 | 扩展机制 | 可借鉴的架构点 |
|------|--------|----------|----------|----------|----------------|
| **AutoHotkey v2** | C++ 单体 | **自有语言**：`scanner.cpp` 词法 → AST → 解释执行 | 单线程消息循环 + 全局 hook（`hook.cpp`/`hotkey.cpp`） | `DllCall` / COM / 库文件 | 真正的语言引擎分层；命令与表达式分离；`ObjectBase` 引用计数对象系统；解释器可嵌入（DLL 模式） |
| **KeymouseGo** | Python + Electron | JSON 事件流 | 顺序回放（pyautogui） | 无 | 极简：录一遍→回放；跨平台；代价是无流程控制 |
| **SikuliX** | Java + OpenCV | 脚本 + `Region`/`Pattern` 对象 | 图像驱动，`find()` 返回 `Match` 可链式 | 插件 | **视觉抽象是第一公民**；相对定位（`Region.right()`/`below()`） |
| **UiPath / Power Automate Desktop** | .NET / WPF | 可视化工作流（XAML） | **设计器与 Runner 分进程** | NuGet 包 / 自定义 Activity | **Activity 是可插拔单元**；Object Repository 统一元素定位与缓存 |
| **Quicker** | .NET | 「动作」= 步骤串 + C#/JS 脚本步骤 | 顺序执行 | 动作库（社区分享分发） | 「动作」作为**分发单元**；步骤式低代码 |
| **按键精灵 / 触动精灵** | 自有 | Q 语言（类 VBScript）/ JS | 解释执行 | 插件 SDK | 语言 + 插件生态双轮 |
| **Playwright / Selenium** | 多语言客户端 | 代码 | **协议驱动**（CDP / WebDriver） | 协议 | 客户端/驱动分离，协议优先 |

### 2.2 三条路线与项目的定位

同类软件本质上是三条架构路线：

- **A. 语言引擎路线**（AutoHotkey、按键精灵）——脚本是一门**语言**，有词法/语法/AST/VM。
- **B. 可视化工作流路线**（UiPath、PAD、Quicker）——动作是**可插拔单元**（Activity / Action），设计器与执行器分离。
- **C. 协议驱动路线**（Playwright、Selenium）——协议优先，客户端与驱动解耦。

**QuickScriptTool 的定位是 A + B 的混合体**：既有录制回放 + 可视化动作列表（B），又自研了类 C 变量解释器 `var_compute.cpp`（1230 行，A 的雏形），还做了 Edge 扩展 + CDP（C 的元素）。

但**关键差距在于：项目缺少 B 路线最核心的那层抽象——Action Handler / Activity 注册表**。同类软件里，动作是「一个可独立注册、独立测试、独立文档化的单元」；本项目里，动作是「一个 126 字段的扁平 struct + 一条 6,677 行 if-else 链里的一段」。这正是下面所有结构性问题的总根源。

### 2.3 具体差距

| 维度 | 同类最佳实践 | 本项目现状 | 差距 |
|------|--------------|------------|------|
| 动作抽象 | Activity/Action 可插拔注册 | 126 字段扁平 struct + `if(type==...)` 链 | **大** |
| 执行器 | 设计器/执行器分离，执行器可单测 | `StartActionsWorker` 6,677 行，无单测 | **大** |
| 视觉定位 | SikuliX：`Region`/`Match` 一等公民 | 有 `coord_space` + 布局记忆 + 相对网格（已接近） | 小 |
| 元素定位缓存 | UiPath Object Repository | `ai_ui_layout` 布局记忆（已实现，且规则更细） | 小（持平） |
| 脚本语言 | AHK 完整语言 + AST | 只有变量运算解释器，无 AST、无流程编译 | 中（定位差异，非缺陷） |
| 插件生态 | NuGet / 动作库 / 插件 SDK | 无对外扩展点（仅内置 Skill 文件） | 中 |
| 可测试性 | 执行器可脱离 UI 单测 | 引擎主循环零单测 | **大** |
| CI | 几乎所有同类项目都有 | **无任何 CI** | **大** |

---

## 三、核心架构问题（附证据）

### P0 — 结构性问题（决定项目能否继续演进）

#### P0-1　`StartActionsWorker` 是 6,677 行的上帝函数

- **证据**：`src/engine/engine_script_run.cpp:660` 起，到 `:7336` 结束，跨度 **6,677 行**（文件总长 7,488 行）。
- 内部是一条约 1,900 行的 `if (action.type == ...) else if (...)` 长链（`:5152–7100`），44 种 ActionType 靠顺序比较分发，无 `switch`、无分派表。
- **影响**：任何新增动作类型都要在这条链里找位置插入；任何一处改动都可能影响全部动作；无法单测单个动作；合并冲突高发。
- **对比**：UiPath 的每个 Activity 是独立类；AHK 的每条命令是 `lib/` 下独立实现。

#### P0-2　`EngineHost` 是 12,044 行的上帝对象

- **证据**：`src/engine/engine_host_window.h:1842` 定义 `class EngineHost`，到 `:13885` 结束 = **12,044 行**；含 **56 个 `_` 后缀成员变量**、约 1,088 个方法声明。
- 职责横跨：窗口消息循环、GDI 编辑器绘制、热键注册/捕获、脚本执行、录制、连点、AI 编排、定时任务、窗口模式、设置持久化。
- **影响**：单头文件 13,885 行导致全量重编译代价极高；任何一处成员变更都触发大面积重编；无法用 mock 替换其中任一子系统。
- **附带**：该头文件里还有 **60 个 `inline gh*` 全局可变状态**（钩子句柄 / 计时 / latch，如 `ghHotkeySessionBusy`、`ghWorkerCancelFlag`）。

#### P0-3　动作数据模型是"126 字段扁平 struct"

- **证据**：`src/script_types.h` 的 `struct ScriptAction` 有 **126 个字段**，覆盖全部 **44 种** `ActionType`；未使用的字段保持默认值。
- 树形结构仅靠 `int indent` 表示，无真正的 AST/树节点。
- **额外发现（文档漂移）**：`script_types.h:45` 注释写"共 28 种动作类型"，实际枚举已有 **44 个成员**。
- **影响**：新增动作 = 往 struct 加字段（126 → 更多）；序列化必须处理所有字段；静态分析无法发现"用了不属于该类型的字段"。

#### P0-4　脚本序列化有 3 套并行实现

- **证据**：
  1. `src/script_io.cpp`（`WriteActionJson` / `ScriptActionToJsonString` / `ParseScriptActionBlock`）
  2. `src/script_action_builder.cpp`（`BuildScriptActionFromJson`，走 nlohmann）
  3. `src/webview/webview_bridge_backend.cpp:1212/1401/1427`（`SerializeEditorActionsJsonArray` / `ParseEditorActionsJson` / `ParseDebugScriptJson`）
  4. 另有 `src/window_mode/window_mode_json.cpp` 子配置
- **影响**：三处对同一 struct 的字段理解必须手工保持同步，是字段漂移与"脚本存了读不回来"类 Bug 的高发区；没有 schema 版本号与迁移机制。

#### P0-5　全局可变状态散落

- **证据**：`engine_host_window.h` 60 个 `inline gh*`；`engine_runtime.cpp:62` 的 `g_engine`；`webview_bridge_backend.cpp:2633` 的 `g_agent`（单例 `shared_ptr<AgentCore>` + `g_mu`）；`synthetic_input_filter.cpp:34–53` 十余个 `g_` 原子量；`recorder.cpp:33–55` 又一批。
- **影响**：多 Tab 会话（Agent 已设计支持）与全局单例直接冲突（代码里已在加锁绕）；无法并行测试；状态泄漏导致"用例污染"（AGENTS.md 里已要求"用例结束必须把开关复位"——这是症状）。

### P1 — 可维护性与质量

#### P1-1　两套独立的 WinHTTP 实现

- **证据**：`agent_core.cpp:26 WinHttpHandle` + `CallApi/CallApiStream`（46 处 `WinHttp*` 调用）；`agent_web.cpp:42 WebHttpHandle` + `FetchWebPage:387`（自带重定向/编码/读循环，15 处）。
- 两者各自 Open/Connect/OpenRequest/SendRequest/ReceiveResponse，**无共享抽象、无注入点**，导致 `agent_core` 的 HTTP/流式解析无法 mock 测试。

#### P1-2　重复的基础设施实现

- **Base64 有 3 份**：`agent_attachment.cpp:29`、`ai_action_service.cpp:96/128`、`ocr_engine.cpp:482`。
- **手写 JSON 字符串扫描**：项目已统一用 nlohmann（`agent_core.h:16`），但 `webview_bridge_backend.cpp:350/2547` 仍有 `JsonGetStringArray` / `JsonGetStringField` 手写扫描；`ai_action_service.cpp:210 ExtractJsonArrayFromText` 是第三套手动抽取。

#### P1-3　桥接契约靠字符串约定，无 schema 保护

- **证据**：`src/webview/qst_webview_shell.cpp`（5,222 行）用 `if (type == "agentWindow.close")` 这样的字符串比较分发约 **39 个消息类型**；`webview_bridge_backend.h` 是扁平自由函数集合，无命令枚举、无请求/响应结构体。
- **影响**：JS 侧与 C++ 侧靠 `docs/webview-bridge-api.md`（481 行）人工对齐，无编译期或运行期校验，改名/漏改只能靠运行时才发现；该层**零自检覆盖**。

#### P1-4　AI 编排层是超级枢纽

- **证据**：`ai_action_service.cpp` **3,556 行**，单函数 `ExecuteAiActionExecute`（`:2356–3556`）约 **1,200 行**，`RunAiOneShotVisionQuery`（`:1618–2311`）约 693 行。
- 该文件同时 include `agent_ai_actions` / `agent_web` / `ai_action_lookahead` / `ai_action_router` / `ai_locate_verify` / `ai_logic_convert` / `ai_fast_paths`，形成反向依赖，成为全项目耦合度最高的节点。
- `ai_action_runtime.cpp` 反向依赖 `ai_action_service.h`（运行时 → 服务层依赖倒置）。

#### P1-5　引擎主循环不可测

- **证据**：`CMakeLists.txt:192` 定义 `qst_engine`，仅在 `:763` 被 `QstWebViewShell` 链接——**没有任何 SelfTest 目标链接引擎**。
- `engine_runtime.cpp:99–111` 的"headless"只是 `SetHeadlessUi()` + `SW_HIDE`（隐藏窗口），不是可注入依赖。
- 结果：`StartActionsWorker` 内的窗口模式/找图/变量执行分支无单元测试，只能靠 `window_mode_selftest` 间接覆盖子模块。

#### P1-6　前端是 650KB 单文件，无构建、无类型

- **证据**：`ui/app.js` **17,273 行 / 650KB**，无 `import`/`export`，纯全局脚本；`ui/index.html` 3,959 行 / 210KB（含大量内联）；`ui/visual_editor.js` 5,131 行；`ui/pro-mode.js` 1,787 行。
- 无 `package.json` / vite / esbuild / TypeScript。CMake 只做 `copy_if_different`。
- 唯一模块化的是 `bridge.js`（261 行）。

#### P1-7　无 CI，21 个 SelfTest 全靠手动跑

- **证据**：无 `.github/`，无 `.gitlab-ci.yml` / `azure-pipelines.yml` / `.appveyor.yml`。
- 已有统一 harness 与 21 个测试目标，却没有任何自动回归——这是"投入已花、收益未取"的典型。

#### P1-8　构建重复编译

- **证据**：`src/utils.cpp` 同时编入 `script_core_common` / `scheduled_task_core` / `app_settings_store_core`（**3 次**）；`window_mode_types.cpp` 进 2 个 target；`window_mode_json.cpp` 进 2 个 target。
- 更严重：21 个 SelfTest **各自重新编译被测源**（`AiActionRouterSelfTest` / `AgentAssistantSelfTest` 各直接列 10+ 个 `.cpp`），而非链接库。
- 后果：`build/` 达 **4.3 GB**、564 个 `.obj`。

### P2 — 工程卫生

| 项 | 证据 | 状态 |
|----|------|------|
| 根目录残留 `.obj` | `_melon_test.obj`、`action_utils.obj`、`conv_test.obj`、`debug_trace_test.obj`、`findimage_template_crop.obj`、`remap_collapsed_test.obj` | 已被 .gitignore 忽略，但物理残留 |
| `query` 文件 | 根目录 9 字节，内容 `QstVHid`，误提交（commit `09eb790`） | **已 tracked**，未被忽略 |
| `gdi_*_stubs` | `src/gdi_legacy_stubs.cpp`、`src/gdi_engine_ui_stubs.cpp` 全空实现 | **仍在编译**（`QstProductLibs.cmake:29/34`），过渡期技术债 |
| `archive/` | 空目录 | CMake 无引用 |
| `QuickScript/src/main.cpp` | 旧单体 stub | CMake 无引用 |
| `flameshot_analysis/` | 549 文件第三方参考 | CMake 无引用（已忽略） |
| `.research/` `.dsh-research/` | 48 + 1 文件 | **未忽略且已提交** |
| `dist/` | **21 GB / 573 文件**，含 56 个历史版本 exe、35 个历史 zip、`_edge_pack_stage_*` 临时目录 | 已忽略，但无保留策略 |
| 文档漂移 | `script_types.h:45` 注释"28 种"vs 实际 44 种；`docs/ai-action-exec-optimization.md` 单文件 2,056 行 | 需校对/分册 |

---

## 四、重构清单（按优先级）

> 工作量：S = 1 天内，M = 数天，L = 1–2 周，XL = 2 周以上

### 第一优先级：打通"动作抽象层"（其余一切的地基）

| # | 动作 | 位置 | 具体做法 | 收益 | 风险 | 工作量 |
|---|------|------|----------|------|------|--------|
| **1** | **建立 ActionHandler 注册表** | 新增 `src/actions/`，改 `engine_script_run.cpp` | 定义 `IActionHandler { bool Validate(const ScriptAction&); ExecResult Execute(ActionContext&); std::string Describe(...); }`，每种 ActionType 一个 handler 文件，启动时注册进表；`StartActionsWorker` 改为查表分发 | 新动作=新增一个文件；单动作可独立单测；消灭 1,900 行 if-else 链 | 中（需覆盖 44 种动作，可分批迁移，旧链保留为 fallback） | **XL** |
| **2** | **拆 `EngineHost`** | `engine_host_window.h` → 多个 `.h/.cpp` | 按职责抽出：`ScriptRunner`（跑宏）、`HotkeyManager`、`RecorderController`、`ClickerController`、`SchedulerRunner`、`AiActionCoordinator`；`EngineHost` 只保留窗口生命周期 + 消息泵 | 单头 13,885 → 数个 <1,500 行的头；重编译范围大幅缩小；子系统可 mock | 中高（头文件被 8 个 cpp 反向 include，需按现有"F2 slice"手法分批切） | **XL** |
| **3** | **消灭 60 个 `gh*` 全局态** | `engine_host_window.h:179–285` | 迁入 `ScriptRunner` / `HotkeyManager` 的成员；跨线程状态统一用 `std::atomic` 投影 + 明确所有权注释 | 消除隐式耦合；为并行测试铺路 | 中 | **L** |
| **4** | **统一序列化到 1 套 + schema 版本** | `script_io.cpp` / `script_action_builder.cpp` / `webview_bridge_backend.cpp` | 以 nlohmann 为准收敛；引入 `"v"` 版本字段与 `MigrateAction(json, fromVer)`；三处入口全部转发到同一实现 | 消除字段漂移；脚本格式可演进 | 中（需兼容存量脚本，先写回归用例） | **L** |
| **5** | **动作模型结构化** | `src/script_types.h` | 保留 `ScriptAction` 作为线格式，新增按类型分组的参数子结构（如 `MouseParams` / `ImageParams` / `FlowParams`），由 handler 负责映射；树形用显式 `children` 索引替代裸 `indent` | 消除 126 字段"薛定谔字段"；静态可校验 | 中高 | **L** |

### 第二优先级：可测性与基础设施

| # | 动作 | 位置 | 具体做法 | 收益 | 风险 | 工作量 |
|---|------|------|----------|------|------|--------|
| **6** | **引入 CI** | 新增 `.github/workflows/build.yml` | Windows runner + MSBuild Release → 跑全部 21 个 SelfTest（`--json`，exit code 判定）→ 失败即红 | 已有投入立即变现；回归可发现 | 低 | **S** |
| **7** | **统一 HTTP 抽象** | `agent_core.cpp` + `agent_web.cpp` | 抽 `IHttpClient { Send(req, onChunk) }`，两个 WinHTTP 栈合并为一；提供 `MockHttpClient` 供测试 | `CallApi`/`CallApiStream`（各 ~350 行）可测；消除重复重定向/编码逻辑 | 中 | **M** |
| **8** | **引擎可测化** | `qst_engine` + `tools/` | 抽 `IActionContext`（提供截图/输入/时钟/日志接口），新增 `ScriptRunnerSelfTest` 链接 `qst_engine`；先覆盖无副作用动作（Wait/Loop/VarCompute） | 主循环首次获得单测；P0-1 重构有了安全网 | 中 | **L** |
| **9** | **构建去重** | `CMakeLists.txt` + `cmake/QstProductLibs.cmake` | `utils.cpp` 等改为编一次进共享 OBJECT 库；21 个 SelfTest 改为**链接库**而非重编源 | build 体积与编译时间显著下降 | 低中 | **M** |
| **10** | **桥接契约正式化** | `qst_webview_shell.cpp` + `webview_bridge_backend.h` | 定义 `enum class BridgeCommand` + 请求/响应结构体 + 分派表；加一个 `BridgeContractSelfTest` 校验 C++ 命令表与 `bridge.js` 声明一致 | 消灭 39 处字符串比较；改名漏改编译期报错 | 中 | **L** |

### 第三优先级：前端与清理

| # | 动作 | 位置 | 具体做法 | 收益 | 风险 | 工作量 |
|---|------|------|----------|------|------|--------|
| **11** | **前端模块化** | `ui/app.js` | 拆为 ESM 模块（按 `#page-*` 页面切），引入 esbuild/vite 单步打包（产物仍是静态文件，CMake 拷 `dist`）；可选加 JSDoc + `checkJs` | 17,273 行单文件 → 可维护；增量构建；类型提示 | 中（需保持"无构建也能跑"的降级路径） | **L** |
| **12** | **AI 编排层拆分** | `ai_action_service.cpp` | 3,556 行 → 按职责拆 `AiVisionQuery` / `AiActionDispatch` / `AiLocatePipeline` / `AiBudget`；`ExecuteAiActionExecute`（1,200 行）按动作类型再分 | 消除超级枢纽；反向依赖可解 | 中 | **L** |
| **13** | **消灭重复工具函数** | 全局 | Base64 3 份 → `src/base64.h`；`ExtractJsonArrayFromText` / `JsonGetString*` 手写扫描 → nlohmann | 减少行为不一致 | 低 | **S** |
| **14** | **清理遗留物** | 根目录 + `src/` | 删根 `.obj`；`git rm query`；补 `.gitignore`（`.research/`、`.dsh-research/`）；`archive/` 与 `QuickScript/` 从仓库移除；评估 `gdi_*_stubs` 是否可随 `QST_GDI_LEGACY` 彻底删除 | 仓库干净，clone 体积下降 | 低 | **S** |
| **15** | **dist 产物保留策略** | `tools/package_release.ps1` | 只保留最近 N 个版本，旧的移到 `dist/archive/` 或清理；`_edge_pack_stage_*` 打包后自动删 | 21 GB → 可控 | 低 | **S** |
| **16** | **文档校对与分册** | `docs/` | 修 `script_types.h:45` 的"28 种"→ 44 种；`ai-action-exec-optimization.md`（2,056 行）拆为"动作执行" / "AI 定位" / "找图性能" / "窗口模式"四册 | 降低新人上手成本 | 低 | **M** |

---

## 五、建议路线图

```
阶段 0（1 周内，零风险，先拿收益）
  ├─ #6  引入 CI（21 个 SelfTest 立刻自动回归）
  ├─ #13 消灭重复工具函数
  ├─ #14 清理遗留物 + 补 .gitignore
  └─ #15 dist 保留策略

阶段 1（2–3 周，为大地基铺路）
  ├─ #7  统一 HTTP 抽象（AI 侧首次可测）
  ├─ #9  构建去重（编译时间下降，改代码更快）
  ├─ #8  引擎可测化（抽 IActionContext，先测无副作用动作）
  └─ #16 文档校对分册

阶段 2（4–6 周，核心重构，必须与阶段 1 的测试网配合）
  ├─ #1  建立 ActionHandler 注册表（分批迁移 44 种动作）
  ├─ #4  统一序列化 + schema 版本
  └─ #3  消灭 gh* 全局态

阶段 3（后续，按需推进）
  ├─ #2  拆 EngineHost（依赖 #1/#3 完成后再动，否则冲突巨大）
  ├─ #5  动作模型结构化
  ├─ #10 桥接契约正式化
  ├─ #11 前端模块化
  └─ #12 AI 编排层拆分
```

**排序理由**：`#1 ActionHandler 注册表` 是所有结构性问题的总开关，但它风险最高，所以必须先用阶段 0/1 把 CI 与引擎测试网建起来；`#2 拆 EngineHost` 放在 `#1`/`#3` 之后，否则 13,885 行头文件的切割会和执行链重构互相踩踏。

---

## 六、结论

**总体判断**：这是一个**功能密度远超架构成熟度**的项目。它的功能覆盖（录制回放 / 找图 / OCR / 窗口模式 / 后台输入 / AI Agent / MCP server / Edge 扩展 / 定时任务）已经接近甚至超过多数商业同类产品，五层分层设计、门面模式、21 个 SelfTest、Skill 文件驱动 AI 能力这几件事做得相当专业。

**但它的架构已经到了一条分水岭**：

- 表现层（WebShell）与集成层（DesktopTools / 门面）是健康的；
- **中间的执行核心（`EngineHost` 12,044 行 + `StartActionsWorker` 6,677 行 + 126 字段扁平动作模型）是单体的**，且没有 Action Handler 这层抽象；
- 因为核心不可测、无 CI，任何对核心的改动都是"改完靠手点"；
- 后果是**功能迭代速度会持续衰减**——新增第 45 种动作的成本，已经远高于新增第 1 种时的成本。

**最该做的一件事**：不是先拆那 6,677 行，而是**先建 CI + 引擎测试网**（阶段 0/1），再动 `ActionHandler 注册表`（#1）。有了注册表，`StartActionsWorker`、`EngineHost`、序列化三套、`gh*` 全局态这四个 P0 问题会自然被逐个瓦解——因为它们本质上是同一个问题的四个侧面：**缺少动作级抽象层**。

**不建议做的**：不要重写引擎、不要换 Electron、不要推翻五层分层、不要动 `window_mode/` 的分层（它是全项目最好的部分）、不要为了"架构好看"引入重型框架（DI 容器 / 反射 / 插件系统）——这个项目的复杂度不足以支撑这些抽象的成本。
