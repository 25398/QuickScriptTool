# QuickScriptTool 重构进度与执行记录

> 依据：`docs/architecture-review.md`（2026-09-18 架构评估）
> 记录日期：2026-09-18
> 原则：**每一项都先构建、再跑自检，exit 0 才算完成**。本文件只记录已验证的事实。

---

## 一、本轮完成项

### 阶段 0（评估表「零风险，先拿收益」）

| # | 项 | 状态 | 证据 / 产出 |
|---|----|------|-------------|
| **#6** | 引入 CI | ✅ | 新增 `.github/workflows/build.yml`：windows-2022 + OpenCV 缓存 + 构建产品壳与 18 个 SelfTest 目标 + 跑自检，失败即红。新增 `tools/run_all_selftests.ps1` 作为 CI 与本地**共用**的唯一入口 |
| **#13** | 消灭重复工具函数 | ✅ | Base64 三份（`agent_attachment.cpp:29` / `ai_action_service.cpp:96,128` / `ocr_engine.cpp:482`）→ 唯一实现 `src/base64.h`；JSON 手写扫描（`webview_bridge_backend.cpp` 的 `JsonGetString*` / `JsonGetNumber/Bool/Int`、`ai_action_service.cpp` 的 `ExtractJsonArrayFromText`）→ 唯一实现 `src/json_util.h`（nlohmann） |
| **#14** | 清理遗留物 | ✅ | 删根目录 6 个 `.obj` 残留；`git rm query`；`.gitignore` 补 `.research/*`（保留被 AGENTS.md 引用的 `office-document-io-report.md`）、`.dsh-research/`、`query`、`archive/`，并 `git rm --cached` 共 49 个文件；删空目录 `archive/` |
| **#15** | dist 保留策略 | ✅ | 新增 `tools/prune_dist.ps1`：每个产物系列保留最近 5 个版本（语义版本排序），旧的移入 `dist\archive\`，清理 `_edge_pack_stage_*`；已接入 `package_release.ps1` 与 `package_webview_portable.ps1` 尾部 |
| **#16** | 文档校对 | 🟡 部分 | 已修 `script_types.h` 的类型注释「28 种」→ **44 种**（实测枚举成员 44 个），并补上「新增动作必须同步的四处」；`ai-action-exec-optimization.md`（2,056 行）分册未做，见第四节 |

### 阶段 1（部分）

| # | 项 | 状态 | 证据 / 产出 |
|---|----|------|-------------|
| **#9** | 构建去重 | ✅ | 三处改动，合计**消除约 34,400 行重复编译**：<br>① `utils.cpp`（1,814 行）原被 4 个 target 各编一份 → 抽 `qst_utils` STATIC 库，实测 `utils.obj` 由 4 份变 1 份；<br>② `AgentAssistantSelfTest` 原直接列 24 个被测源（**22,595 行**）→ 改为链 `qst_engine`；<br>③ `AiActionRouterSelfTest` 原直接列 9 个被测源（**11,791 行**）→ 改为链 `qst_engine`，同时删掉不再需要的 `fetch_webpage_link_stub.cpp` / `window_mode_link_stubs.cpp` |

---

## 二、验证结果

构建：`cmake --build build --config Release` 全部目标 exit 0，无 error、无新增 warning。

自检（`build\Release\<Target>.exe --json`，exit code = 失败数）：

| Suite | 用例数 | 结果 |
|-------|--------|------|
| ScriptActionBuilderSelfTest | 53 | ✅ |
| ScriptIoSelfTest | 58 | ✅ |
| CoordSpaceSelfTest | 27 | ✅ |
| MacroVariablesSelfTest | 27 | ✅ |
| ImageMatchSelfTest | 38 | ✅ |
| OcrSelfTest | 11 | ✅ |
| AiActionRouterSelfTest | 171 | ✅ |
| AgentAssistantSelfTest | 61 | ✅ |
| AppSettingsStoreSelfTest | 42 | ✅ |
| ThemeUiSelfTest | 22 | ✅ |
| BreakoutCooldownSelfTest | 7 | ✅ |
| HotkeyStopSelfTest | 33 | ✅ |
| ClickerTimingSelfTest | 5 | ✅ |
| ScheduledTaskSelfTest | 25 | ✅ |
| RecorderSelfTest（`QstRecorderLogicTest.exe`） | 61 | ✅ |
| **合计** | **641** | **0 失败** |

另外单独验证了 `VirtualHidSelfTest` 中依赖驱动安装脚本的 8 个静态断言（`install_script_*` / `product_no_*` / `release_pack_*`）——因为本轮给 5 个 `.ps1` 补了 BOM，必须确认不破坏这些断言：全部 ✅。

---

## 三、过程中发现的、评估表未记录的问题

这几条是本轮动代码时**实测撞到**的，比评估表更具体：

### 3.1 引擎对「壳」有隐式符号依赖（P1-5 的真正根因）

`qst_engine` 引用了下面这些符号，但它们**只在 `src/webview/qst_webview_shell.cpp` 里定义**：

| 符号 | 声明位置 | 定义位置 |
|------|----------|----------|
| `qst::webview::PostToWebUi(std::string)` | `webview_bridge_backend.h:30` | 壳 |
| `qst::webview::HotkeyLogLine(const std::string&)` | `webview_bridge_backend.h:32` | 壳 |
| `qst::webview::NotifyWebDebugWindowSetting(bool)` | `webview_bridge_backend.h:40` | 壳 |
| `qst::webview::SyncHomeSelectionCache(...)` | `webview_bridge_backend.h:37` | 壳 |
| `HINSTANCE g_instance` | `taskbar_window.h:25` | 壳 |

后果：**`qst_engine` 无法脱离壳单独链接**，所以此前没有任何 SelfTest 能链引擎——这正是评估表 P1-5「引擎主循环不可测」的机制性原因（不是「没写测试」，是「链不起来」）。

本轮的处理：新增 `tools/engine_link_stubs.cpp` 提供空实现，**先解锁**自检链引擎。彻底修法属于 #2/#10（把「引擎 → 壳」的调用倒置为回调注入），见第四节。

### 3.2 五个 `.ps1` 缺 UTF-8 BOM（会真实产出乱码）

AGENTS.md 已把「含中文的 `.ps1` 必须存成 UTF-8 带 BOM」列为硬规则，但实测仍有 5 个文件违反：

| 文件 | 中文字符数 | 影响 |
|------|-----------|------|
| `tools/package_webview_portable.ps1` | **261** | 生成便携包内的中文 README；GB2312 解析下**整个 README 乱码并进 zip** |
| `driver/qst_vhid/repair_boot.ps1` | 4 | 提示文本乱码 |
| `driver/qst_vhid/install_fake_driver.ps1` | 1 | 同上 |
| `driver/qst_vhid/portable/install_portable.ps1` | 1 | 同上 |
| `tools/fetch_webview2_fixed.ps1` | 1 | 同上 |

已全部补 BOM。

### 3.3 MSBuild 遇到 `https_proxy` 与 `HTTPS_PROXY` 并存会直接失败

报错是 `MSB6001: "CL.exe"的命令行开关无效。System.ArgumentException: 已添加项。字典中的关键字:"https_proxy"所添加的关键字:"HTTPS_PROXY"`——MSBuild 用大小写不敏感的字典构造子进程环境，两个变量同时存在就抛异常，且报错完全指不到真正原因（看起来像编译开关问题）。

`tools/run_all_selftests.ps1` 里已加护栏：检测到重复时在本进程内移除大写那份。

### 3.4 OBJECT 库的 `.obj` 不会经由中间 OBJECT 库传递

给 `AgentAssistantSelfTest` 只链 `qst_engine` 会缺 `qst_desktop_tools` 的符号（`RenderBatchScope` / `UiEditorWidth` / `ResolveProgramLaunchPath`）。所以 `QstWebViewShell` 的链接列表才必须逐个列出 `qst_engine` / `qst_desktop_tools` / `qst_engine_ui_stubs`。

推论：想让 `utils.cpp` 这类源只编一次，必须用 **STATIC** 库（可正常传递），不能用 OBJECT 库。这条已写进 CMakeLists 的注释，避免后人重踩。

---

## 四、未完成项与执行方案

以下项的**依赖关系**决定了不能先做，评估表的排序是对的：

| # | 项 | 工作量 | 为什么本轮没做 | 下一步具体做法 |
|---|----|--------|----------------|----------------|
| **#1** | ActionHandler 注册表 | XL | 分发链的每个分支都深度依赖执行期局部状态（`inputTimeline` / `wmExecPtr` / `pendingGoto` / `keepCursorAtFind` / `wmSetPos` 等 lambda），**必须先有 `ActionContext` 才能切**。硬切会把执行语义改错 | ① 先定义 `IActionContext`（截图/输入/时钟/日志/变量/窗口模式执行器/循环跳转状态）；② 用 `ActionContext` 包住 `StartActionsWorker` 的现有局部变量，**不改行为**；③ 从纯逻辑动作（`Wait` / `Loop` / `EndLoop` / `Goto` / `VarCompute` / `StopMacro`）开始，逐个把分支搬进 `src/actions/<name>_handler.cpp`；④ 每搬一个跑 `ScriptActionBuilderSelfTest` + `ScriptIoSelfTest` 回归 |
| **#2** | 拆 `EngineHost` | XL | 依赖 #1/#3 完成，否则 13,885 行头文件的切割会与执行链重构互相踩踏 | 按现有「F2 slice」手法分批：先抽 `HotkeyManager`（与执行链无关），再 `RecorderController` / `ClickerController` / `SchedulerRunner`，最后 `ScriptRunner` |
| **#3** | 消灭 60 个 `gh*` 全局态 | L | 目标宿主（`ScriptRunner`/`HotkeyManager`）还不存在 | 与 #2 的切片同步做：每抽出一个子系统就把属于它的 `gh*` 迁成成员 |
| **#4** | 统一序列化 + schema 版本 | L | 三套实现各有存量兼容分支，改动风险集中在脚本格式 | ① 先补回归：用现有 `ScriptIoSelfTest`（58 用例）做基线；② 以 nlohmann 为唯一实现，三处入口转发；③ 加 `"v"` 字段 + `MigrateAction(json, fromVer)`；④ 关键：用**真实历史脚本**（`dist\` / 用户库）做往返验证 |
| **#5** | 动作模型结构化 | L | 依赖 #1（由 handler 负责字段映射） | 保留 `ScriptAction` 作线格式，新增按类型分组的参数子结构（`MouseParams` / `ImageParams` / `FlowParams`），由 handler 映射；`indent` → 显式 `children` |
| **#7** | 统一 HTTP 抽象 | M | 独立可做，本轮预算优先给了 #9 | 抽 `IHttpClient { Send(req, onChunk) }`，合并 `agent_core.cpp:26 WinHttpHandle` 与 `agent_web.cpp:42 WebHttpHandle`；提供 `MockHttpClient` |
| **#8** | 引擎可测化 | L | 前置（引擎可链接）本轮已打通，但 `IActionContext` 未抽 | 现在可以新增 `ScriptRunnerSelfTest` 直接链 `qst_engine`（用 `tools/engine_link_stubs.cpp`），先覆盖无副作用动作 |
| **#10** | 桥接契约正式化 | L | 独立可做 | `enum class BridgeCommand` + 请求/响应结构体 + 分派表；新增 `BridgeContractSelfTest` 校验 C++ 命令表与 `bridge.js` 一致 |
| **#11** | 前端模块化 | L | 独立可做；需保持「无构建也能跑」的降级路径 | 按 `#page-*` 切 ESM，esbuild 单步打包到 `dist`，CMake 拷产物 |
| **#12** | AI 编排层拆分 | L | 独立可做 | `ai_action_service.cpp`（3,556 行）按职责拆 `AiVisionQuery` / `AiActionDispatch` / `AiLocatePipeline` / `AiBudget` |
| **#16** | 文档分册 | M | 只做了注释校对 | `ai-action-exec-optimization.md`（2,056 行）拆为「动作执行」/「AI 定位」/「找图性能」/「窗口模式」四册 |

**建议顺序**：`#8`（现在就能做，且给 #1 铺安全网）→ `#4` → `#1` → `#3` → `#2` → 其余。

---

## 五、遗留风险

1. **`tools/run_all_selftests.ps1` 与两个打包脚本未能实机执行**——编写环境的脚本执行被拦截，只能静态校验（解析无错、正则与版本排序逻辑用等价 Python 在真实 `dist\` 上验证过：installer 27 个 → 保留 5 归档 22，edge 4 个不动）。首次在真机跑时请先加 `-DryRun` 看 `prune_dist.ps1` 的清单。
2. **CI workflow 未在真实 runner 上跑过**——OpenCV 下载（约 250MB，已加缓存）、`FakeFocus32` 的 x86 工具链、GUI 类 suite 在 runner 上是否可跑，都需要首次 push 验证。`WindowModeSelfTest` / `VirtualHidSelfTest` / `InjectionSelfTest` 默认不在 CI 跑（需桌面会话与已装驱动）。
3. **`build\` 目录仍有历史陈旧 `.obj`**（如已不再编译的 `QuickScriptTool.dir\Release\utils.obj`）——不影响构建正确性，但占空间。需要时 `cmake --build build --target clean` 或删掉 `build\` 重新配置。
4. **`dist\` 仍有 21 GB**：本轮只提供策略，未执行清理。要真正回收：`powershell -File tools\prune_dist.ps1 -PurgeArchive`（先 `-DryRun` 确认）。
5. `git rm --cached` 的 49 个文件与删除的 6 个 `.obj` 目前**只在工作区暂存**，尚未提交。

---

## 六、验收结论（补记）

> 全文见 [`docs/refactor-acceptance.md`](refactor-acceptance.md)。此处只记与本文件**结论冲突**的部分。

- 本文件 §一 把 **#13** 标为 ✅。**验收发现该改动引入 1 项 P0 回归**：`saveSettings` 静默失效（改设置提示成功、实际写回旧值）。根因是本文件 §3.1 记录的"引擎对壳隐式依赖"之外的**同类问题**——`qst_webview_shell.cpp:3154` 的 `json.substr(brace)` 取到带尾随 `}` 的非法 JSON，旧手写扫描能容忍、换 nlohmann 严格解析后取不到任何键。已修复（改用 `FindMatchingJsonBrace` 截配对括号）并实证。
- 本文件 §5.1 称 `run_all_selftests.ps1` 与打包脚本"未能实机执行"。**验收已补跑**：`prune_dist.ps1 -DryRun` 跑通（70 文件 / 15.23 GB），`run_all_selftests.ps1` 的等价判定口径已逐套复现（641 用例 / 0 失败）。
- 本文件 §5.2 称 CI 未在真实 runner 跑过。**验收确认仍属未验证**；另发现 workflow 编 18 个 target 但只跑 15 个（3 个交互类只编不跑），以及本机 `third_party/opencv` 从未生成（`setup_opencv.ps1` 在本机中途失败过，CI 上是首次真正执行）。
- 本文件 §二 的 **641 用例 / 0 失败**经复现**完全属实**（580 + 61），一个不差。
- **验收另修 1 项 P1 工具缺陷**：`tools/test_mcp_server.ps1` 缺 `StandardOutputEncoding=UTF8`，导致该冒烟自建立起从未真正通过（`ConvertFrom-Json` 在 UTF-8 中文上抛错）。修复后 22 项全通过。

---

## 七、A 段 + B 段执行记录（按验收报告 §7.2 调整后的顺序）

> 依据：`docs/refactor-acceptance.md` §7.2 的三处调整（加 A 段 / #10 提前 / #8 拆两步）。
> 本轮完成 **A 段全部** + **B 段全部**（B3 见下）。

### A 段 —— 闭合 saveSettings 事故（已完成）

| 项 | 做法 | 证据 |
|----|------|------|
| **A1** 固化回归 | `json_util.h` 新增 `GetSubObjectText()`（严格解析取**配对**对象，`dump(..., error_handler_t::replace)` 防非法 UTF-8 抛异常）；壳的 `saveSettings` 改用它，删掉脆弱的 `substr(首个 '{')` 写法 | 新增 `tools/bridge_json_selftest.cpp`（**9 条**断言，链 `qst_utils`）；**A/B 实测**：把旧 `substr` 写法打回去 → **5 条立刻变红**，还原后 9/9 绿 |
| **A2** 解析失败可观测 | `json_util.h` 新增 `NoteParseFailure` / `ParseFailureCount` / `SetParseFailureSink` / `IsParseableObject`；`ApplySaveSettingsJson` 入口做**一次**前置校验并上报；壳在 `wWinMain` 装 sink 写 `webview_boot.log`（`JSON: 解析失败 where=…`） | `diagnostics_silent_on_valid`（合法输入 0 条诊断）+ `diagnostics_reports_invalid`（非法输入 1 条、标签正确） |

未采纳验收报告 §7.4 明确禁止的两件事：不加「宽容模式」开关、不回退到手写字符扫描。

### B 段 —— 消掉链接 hack（已完成）

| 项 | 做法 |
|----|------|
| **B1** 依赖倒置 | 新增 `src/engine/engine_ui_hooks.{h,cpp}`：`UiBridgeHooks` 结构体（4 个 `std::function`）+ `SetUiBridgeHooks`（**只覆盖非空成员**，因为壳的真实现分布在两个 TU）+ `UiHooks()` / `UiBridgeHooksInstalled()`。**4 个函数（`PostToWebUi` / `HotkeyLogLine` / `NotifyWebDebugWindowSetting` / `SyncHomeSelectionCache`）的定义搬到引擎侧**（默认 no-op 转发），壳改名 `Shell*` 并在 `wWinMain` 顶部注入真实现。`g_instance` 定义搬到 `src/app_instance.cpp`（编进 `qst_utils`）。引擎侧 `engine_host_window.h` / `engine_settings_reload.cpp` 的 `#include "webview/webview_bridge_backend.h"` 换成 `engine/engine_ui_hooks.h` —— **引擎不再 include 壳的头文件** |
| **B2** 删链接桩 | 删 `tools/engine_link_stubs.cpp`（B1 后不再需要）与 `tools/fetch_webpage_link_stub.cpp`（验收报告 A3 的死文件）；CMake 中两个 SelfTest 不再列桩 |
| **B3** `ScriptRunnerSelfTest` | 新增 `tools/script_runner_selftest.cpp`：默认 **7 条** UI 钩子契约断言（CI 安全）；`--engine` 追加 **4 条** headless 引擎真实跑动作 |

**B1 的一处事实更正**：验收报告 §7.2 称这 4 个符号"只在 `qst_webview_shell.cpp` 定义"。实测 `PostToWebUi` / `SyncHomeSelectionCache` / `NotifyWebDebugWindowSetting` 定义在 **`webview_bridge_backend.cpp`**（同属壳目标，不在 `qst_engine` 内），只有 `HotkeyLogLine` 在 `qst_webview_shell.cpp`。结论（引擎无法脱离壳链接）不变，只是位置需更正。

### 验证（本轮）

| 项 | 结果 |
|----|------|
| 构建 | 产品壳 + 全部 SelfTest 目标 exit 0，无 error / 无新增 warning |
| 逻辑层自检 | **17 suite / 657 用例 / 0 失败**（641 + BridgeJson 9 + ScriptRunner 7） |
| `ScriptRunnerSelfTest --engine` | **11/11 通过**（7 钩子 + 4 引擎） |
| MCP 端到端冒烟 | 6/6（真拉子进程走管道：initialize / tools/list 12 工具 / ping / cursor_position / 未知工具 isError / 未知方法 -32601），中文 UTF-8 无乱码 |
| `WindowModeSelfTest` | 71/71 |
| `VirtualHidSelfTest` 驱动脚本静态断言 | 8/8 |

### B3 的边界（诚实说明）

验收报告 §7.2 B3 写的是"先覆盖 Wait/Loop/Goto/VarCompute"。**Wait / Loop / Goto / VarCompute 的*执行*确实覆盖了**，但断言只能用**时序语义**（循环重复 → 耗时成倍；goto 跳转 → 耗时骤减），因为：

- `ExecutedSteps()` 在收尾后被清零，只能在运行中采样，会漏步；
- 没有对外暴露"单步推进"接口（`IsDebugging` / `DebugPaused` / `DebugStepMode` 只读），步进模式无法从测试驱动；
- 逐动作级断言要等 **#1 抽出 `ActionContext`** 之后才能做——这正是 #1 的价值所在，B3 不假装覆盖了它。

仍未覆盖：`If/Else` 分支、窗口模式分支、找图分支（同因）。

### B3 过程中踩到的三个坑（已写进 `module-selftest` 元技能）

1. **不要调 `qst::engine::Shutdown()`**：headless 引擎窗的 `WM_DESTROY` 会走 `TerminateProcess`，把测试进程直接杀掉；stdout 还是块缓冲 → **输出全丢、exit code 还是 0**，现象是"什么都没发生"，极难定位。
2. **必须自己 `PeekMessage` 抽消息**：收尾信号经窗口消息派发（产品里由壳的消息循环负责），否则 `IsRunning()` 永远为真（实测跑完 2 步后 10s 仍不收尾）。
3. **循环体不要手搓 indent**：手搓 `[loop(indent=0), wait(indent=1), endLoop(indent=1)]` 时循环体只跑**一遍**。用 `FlattenNestedActionParamList` 才发现生产编码是 `[loop(indent=0), body(indent=1)]`，**构建器不生成 endLoop**，显式塞 `indent=1` 的 endLoop 会把循环体提前切断。

### 下一步

按验收报告 §7.2：**C 段（#10 桥接契约正式化）提前到 #4 之前** —— 本次 P0 就长在桥接缝上；然后 D 段（#4 统一序列化 + schema 版本，**必须用真实历史脚本做往返验证**），再进 #1 → #3 → #2。

---

## 八、C 段执行记录（#10 桥接契约正式化）

### C1 —— 命令表 + 双向一致性校验（已完成）

| 动作 | 文件 |
|------|------|
| 新增命令契约表：**106 条** JS→C++ 命令（每条登记发它的 JS 文件）+ **7 条** C++ 独有命令（每条写清理由） | `src/webview/bridge_commands.h`（新增，206 行） |
| 新增 `BridgeContractSelfTest`：**8 条**断言，双向校验「表 ↔ C++ 入站分派 ↔ JS 发送侧」 | `tools/bridge_contract_selftest.cpp`（新增，484 行） |
| 壳的未知命令处理补日志：用命令表区分「JS 发了未登记命令」与「表与实现漂移」，写 `webview_boot.log` | `src/webview/qst_webview_shell.cpp` |

**8 条断言**：表无重名/无交集 · 表内每条在 C++ 分派里有分支 · **表内每条 JS 侧真的会发** · sender 文件存在 · C++ 分派全部已登记 · 例外表有理由且确有分派 · 例外当前无人发 · JS 发的无孤儿。

**A/B 实证**：把 `bridge.js` 的 `saveSettings` 改成 `saveSettingsX` → **两个方向同时变红**（`table_all_sent_by_js` 报「表里登记了但没人发」、`js_sends_all_declared` 报「JS 发了但 C++ 不处理」），正是 P0 的形状。还原后 8/8 绿。

### C1 顺带查清的 7 条差异（全部有据可查，非 bug）

| 命令 | 性质 |
|------|------|
| `setMode` / `modeReady` / `close` / `drag` / `minimize` | **历史别名**：C++ 用 `\|\|` 与 `window.*` 并列接受（`:2917` / `:3036` / `:2898` / `:2913` / `:2894`），JS 只发 `window.*` 形式 |
| `agentWindow.setTopmost` | **疑似死分支**：无任何 JS 发它；JS 侧只有 `debugWindowSetTopmost`（`:2634`） |
| `debugWindow.needTheme` | **C++→JS 消息名**（`PostToJs` 在 `:2218`/`:2323`，`app.js:12659` 接收）；入站分派在 `:2216` 把它当 `debugWindow.ready` 的别名接受 |

### 提取规则（避免误报，实测踩过）

- **C++**：`type == "X"`。
- **JS 专用发送方**（`bridge.js` / `debug.html` / `agent.html`）→ 取全部 `type: "X"`（这些文件整文件只发桥接命令）。
- **JS 混合文件**（`app.js` / `pro-mode.js` / `visual_editor.js` / `index.html`）→ **只取出现在 `post(...)` / `postMessage(...)` / `qst.post(...)` 实参里**的字面量。
  > 不加这个区分会出现 **7 个假阳性**：`loop` / `else` / `macro` / `ai` / `rec` / `sched` / `paint`（脚本动作类型与标签页 kind，不是桥接命令）。另有 `crosshairPick.result` 是 `Object.assign({type:...}, msg)` 本地合成的消息。

### 验证

| 项 | 结果 |
|----|------|
| 构建 | 产品壳 + 全部目标 exit 0 |
| 逻辑层 | **18 suite / 665 用例 / 0 失败**（657 + BridgeContract 8） |
| 契约 A/B | 双向变红验证通过 |
| MCP 端到端 | 6/6 |

### 下一步

**D 段（#4 统一序列化 + schema 版本）**。硬要求（验收报告 §7.2 D）：必须用**真实历史脚本**做往返验证（`dist\` 与用户脚本库），不能只跑 `ScriptIoSelfTest` —— 本次事故的教训是「用例全绿 ≠ 没坏」。

两轮改动的完整清单见 [`docs/refactor-changes-summary.md`](refactor-changes-summary.md)。

---

## 九、第 3 轮记录（发版工具链 + FakeFocus32 + A4/A5 收尾）

> 验收全文：[`docs/refactor-acceptance-round3.md`](refactor-acceptance-round3.md)
> 本轮**不是重构轮**：主线是发版工具链与 FakeFocus32 构建修复；另由验收会话闭合了上轮的 A4/A5。

### 9.1 发版工具链（新增）

| 文件 | 作用 |
|------|------|
| `tools/package_with_version.ps1`（新增，481 行） | 一条命令走完：写版本号三处 → 构建 → 组装 dist + 便携 zip → 编安装包 → 同步 `website\downloads` |
| `release.cmd`（新增，49 行） | 纯 ASCII 包装器；`cd /d "%~dp0"` 所以任意目录可调用；双击保持窗口 |
| `AGENTS.md` | 新增「一键发版」小节 |

**刻意不写死**（后续改代码不用动这个脚本）：版本号用正则写入（不依赖行号）；构建目标从 `package_release.ps1` 解析 `--target`；打包逻辑完全复用该脚本。失败默认回滚版本号；`-DryRun` / `-SkipBuild` / `-SkipInstaller` 可裁剪。

**验收实测**：`-DryRun` exit 0、计划完整、三处版本号未变；两条版本号正则独立复核均匹配且各只替换 1 处、行尾保留；构建目标正确解析出 `QstWebViewShell, QstUninstall`；工具链探测到本机实际安装位置（BuildTools 的 cmake、`%LOCALAPPDATA%` 的 ISCC）。

### 9.2 FakeFocus32 构建修复

`src/window_mode/fake_focus/build_fakefocus32.cmd` 的括号打断 `if` 老坑已修：改成先 `set "PF86=%ProgramFiles(x86)%"` 再 `if not defined`，并加 vswhere 全路径兜底 + 已知路径回退链。

**验收实测**：`cmake --build build --config Release --target FakeFocus32` 现在打印 **`[FakeFocus32] OK:`**（修复前是 `vcvarsall.bat not found - skip 32-bit DLL`）；PE 头校验 `Machine=0x014c`（真 32 位），`FakeFocus64.dll` = `0x8664`。打包脚本按文件名精确拷贝，`build\Release` 下的 `FakeFocus32.next/safe/v3/v4.dll` 陈旧实验产物不会进包。

### 9.3 window_mode 后台输入调整

| 改动 | 位置 |
|------|------|
| `WakeMapleStoryInputPolling()`：注入后补一次**真激活**唤醒客户端轮询（禁用假 `WM_ACTIVATE`） | `window_mode_executor.cpp`（调用点有 `UsesBackgroundWindow && IsInjected && LooksLikeMapleStoryTarget` 三重守卫） |
| `TargetOwnsForegroundWindow()`：方向键兜底 `SendKeyboardKey` **只在目标为前台窗时**才补 | `background_window_input.cpp/.h`（自检 `lca_nav_key_leaks_to_foreground` 覆盖） |
| `MaplePollHitSummary()`：`mapleDiag` 高位解成 `pollHit=` 人话 | `window_mode_executor.cpp` |
| 技能同步 | `.cursor/skills/window-mode-debug/{SKILL.md,reference.md}`（含 `build_fakefocus32.cmd` 构建前提、诊断契约） |

**仍未收敛**（reference.md 自述）：冒险岛「游戏已在前台、诊断却全零」——`gaks=0 gfw=0 diState=0 lastCb=0` 但钩子已装好，说明客户端根本不调这些 API。**不要再盲目补钩子，先看 `pollHit=`。**

### 9.4 上轮遗留项 A4 / A5 已闭合

| 项 | 前 | 后 |
|----|----|----|
| **A4** 残留自研 Base64 | 5 份（第 1 轮只收敛 3 份） | **1 份**（`src/base64.h`）。`qst_webview_shell.cpp` 的 `readImageDataUrl` 内联循环、`ext_bridge_server.cpp` 的 `Base64Encode` 均改为 `qst::base64::Encode`；`grep` 全仓字母表只剩 `base64.h` |
| **A5** 重复编译 | `window_mode_types.cpp` 2 份 / `window_mode_json.cpp` 3 份 | **各 1 份**。新增 `window_mode_common`（STATIC）承载两文件，四个 target 改链接；**消除约 1,246 行重复编译** |

A5 能这么做的前提是**先确认两文件是纯函数**（只有匿名 namespace，无全局可变状态、无静态注册）——有静态注册的源放进 STATIC 库可能被链接器裁掉初始化，属静默故障。

### 9.5 本轮回归

| 项 | 结果 |
|----|------|
| 全量构建 | exit 0，无 error |
| logic 档 18 个 suite | **665 用例 / 0 失败** |
| MCP 端到端 | **26 项全通过 / exit 0** |

### 9.6 本文件过时陈述更正（重要）

本文件以下陈述在本轮已不成立，**请以本节为准**：

| 位置 | 原文 | 实际 |
|------|------|------|
| §5.1 | 「`run_all_selftests.ps1` 与两个打包脚本**未能实机执行**——编写环境的脚本执行被拦截」 | ❌ **已实跑通过**（第 2 轮验收 18/18 / exit 0）。「被拦截」是**误判**：真实原因是宿主 `PATHEXT` 被削成只剩 `.CPL`，PowerShell 命令发现不认 `.EXE`，报「无法在管道中间运行文档」。脚本本身无缺陷 |
| §5.4 | 「`dist\` 仍有 **21 GB**：本轮只提供策略，**未执行清理**」 | ❌ 现为 **5.2 GB / 503 文件**。且 `prune_dist.ps1 -PurgeArchive` **确已实跑**（归档 70 文件 / 15.23 GB 后回收，dist 顶层 5.12 GB）——`dist\archive\` 现在不存在，是因为 `-PurgeArchive` 会删掉该目录，**不代表脚本没跑过** |
| §5.5 | 「49 个文件与 6 个 `.obj` 只在工作区暂存」 | ⚠ 现为 **50 删除 + 32 修改 + 19 未跟踪** |
| §5.3 | 「`build\` 仍有历史陈旧 `.obj`（如 `QuickScriptTool.dir\Release\utils.obj`）」 | ⚠ 仍属实，且影响面更大：`build\QuickScriptTool.vcxproj` 整个是 **09-18 13:02 的陈旧工程**，做「权威核对」时会被它干扰（第 3 轮验收差点误判 A5 未生效）。建议 `--target clean` 或重建 `build\` |
| §七「下一步」 | 「C 段（#10 桥接契约正式化）」 | ❌ C 段已在本文件 §八 记录完成；下一步是 **D 段（#4 统一序列化）** |

### 9.7 第 3 轮验收新发现（待处理）

| # | 项 | 级别 | 说明 |
|---|----|------|------|
| F2 | `WindowModeSelfTest / background_minimized_quiet_restore` **抖动 40%**（10 次 4 失败） | P1 | 测试用固定 sleep 读 `GetForegroundWindow()`，而前台切换是异步的 → 属测试时序脆弱。该 suite 在 `full` 档，**当前不 gate CI**；修好前**不要**把 `-Tier full` 接进 CI。建议改成**轮询 + 超时**（改等待方式，不改判定标准） |
| F3 | 发版脚本覆盖不到的 4 处硬编码版本号 | P2 | `installer/QuickScriptTool.iss:4`（自指检查行，每次发版必错）、`package_with_version.ps1:44`（示例）、`website/downloads/README.md:7-9`、`website/README.md:16-18` |
| F4 | `-DryRun` 不校验版本号正则 | P2 | 它在 `Set-VersionInFile` 之前 `exit 0`，不会验证两个 `-Pattern` 能否匹配；建议加 5 行 `IsMatch` 检查 |
| F1 | 本文件与 `refactor-changes-summary.md` 本轮未同步 | P1 | 已由本节 + §9.6 更正 |


