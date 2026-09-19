# 重构验收报告（第 2 轮：A 段 + B 段 + C 段）

> 验收日期：2026-09-18（第 2 轮验收）
> 验收对象：`docs/refactor-changes-summary.md` §四/§五/§六、`docs/refactor-progress.md` §七/§八
> 验收方式：**逐项实机复跑 + 变异测试**（把声称"会被测出来"的缺陷真的打回去，看是否变红）
> 前序：`docs/refactor-acceptance.md`（第 1 轮验收）

---

## 一、总评

| 维度 | 结论 |
|------|------|
| A 段（闭合 P0） | ✅ 全部属实，且**回归锁强度超过声称**（变异后 7 条变红，声称 5 条） |
| B 段（依赖倒置 + 删桩 + 首个引擎测试） | ✅ 全部属实，**依赖倒置是真的**（引擎已能脱离壳独立链接） |
| C 段（桥接契约） | ✅ 全部属实，双向校验**经变异实测有效** |
| 自述数据 | ✅ **18 suite / 665 用例 / 0 失败** 完全复现；`ScriptRunnerSelfTest --engine` 11/11 复现 |
| 工程质量 | 高。B1 主动更正了第 1 轮验收报告的一处事实错误；B3 明确写出「断言只能用时序语义」的边界，不假装覆盖 |
| **验收新发现** | 1 项 P1（**文档被误删、12 处引用断链**，已恢复）+ 4 项 P2 + 1 项环境误判澄清 |

**一句话**：这一轮把第 1 轮暴露的"边界行为无测试"补上了，而且**补得是真的**——两个回归锁都通过了变异测试，不是摆设。当前最该处理的不再是代码，而是**文档完整性**。

---

## 二、A 段验收（闭合 saveSettings P0）

### A1 —— 回归锁

| 检查点 | 实测 |
|--------|------|
| `GetSubObjectText()` 存在且实现正确 | ✅ `src/json_util.h:189`，严格解析取**配对**子对象；`dump(..., error_handler_t::replace)` 顺带堵住非法 UTF-8 抛 `type_error.316` |
| 壳已改用（不再有脆弱的 `substr`） | ✅ `qst_webview_shell.cpp:3162` → `GetSubObjectText(json, "settings", payload, "saveSettings.payload")` |
| `BridgeJsonSelfTest` 存在 | ✅ 280 行，**9 条**断言，链 `qst_utils` |
| 断言内容是真契约（非空壳） | ✅ 含 `regression_trailing_outer_brace`（**正是本次事故形状**）、`subobject_not_last_key`、`subobject_brace_and_nested`、`subobject_invalid_json_no_throw`、`subobject_applyable_fields` |

**变异测试（关键）**：把 `GetSubObjectText` 改回事故前的 `substr(首个 '{')` 到末尾写法 → 重新构建 → 实测：

```
{"name":"subobject_last_key","ok":false,...}                 ← 红
{"name":"subobject_not_last_key","ok":false,...}             ← 红
{"name":"subobject_brace_and_nested","ok":false,...}         ← 红
{"name":"subobject_missing_or_null","ok":true,...}           ← 绿（该分支本就正确）
{"name":"subobject_invalid_json_no_throw","ok":false,...}    ← 红
{"name":"regression_trailing_outer_brace","ok":false,...}    ← 红
{"name":"subobject_applyable_fields","ok":false,...}         ← 红
{"name":"diagnostics_silent_on_valid","ok":true,...}         ← 绿
{"name":"diagnostics_reports_invalid","ok":false,...}        ← 红
{"passed":2,"failed":7,"ok":false}   exit=7
```

**7 条变红**（声称 5 条，实际更强）。还原后 9/9 绿、exit 0，且 `git diff` 确认工作区无残留变异。

### A2 —— 让静默失败可观测

| 检查点 | 实测 |
|--------|------|
| 诊断 API 存在 | ✅ `NoteParseFailure` / `ParseFailureCount` / `ResetParseFailureCount` / `SetParseFailureSink` / `IsParseableObject` |
| `ApplySaveSettingsJson` 入口前置校验 | ✅ `webview_bridge_backend.cpp:708` —— 一次校验，不是改 100 个 `if (GetX(...))` |
| 壳装 sink 写日志 | ✅ `qst_webview_shell.cpp:5082` `SetParseFailureSink(&JsonParseFailureToBootLog)` |
| 子对象缺失时**不再静默回落** | ✅ `:3165` 增加 `BootLogLine("BRIDGE: saveSettings 缺少 settings 子对象，回落整条消息（本次保存很可能不生效）")` |
| 未采纳明令禁止的两件事 | ✅ 无「宽容模式」开关、未回退到手写扫描（已 `grep` 确认 `JsonGet*` 无残留） |

> A2 的价值在「下次同样的事故会留下痕迹」——这正是第 1 轮 P0 难发现的根本原因（解析失败静默返回 false）。

---

## 三、B 段验收（消掉链接 hack）

### B1 —— engine → shell 依赖倒置

| 检查点 | 实测 |
|--------|------|
| `engine_ui_hooks.{h,cpp}` 存在 | ✅ 69 + 83 行；`UiBridgeHooks`（4 个 `std::function`）+ `SetUiBridgeHooks`（**只覆盖非空成员**，因壳的真实现分布在两个 TU）+ `UiHooks()` / `UiBridgeHooksInstalled()` |
| 4 个函数的**定义**确实搬到引擎侧 | ✅ `grep` 全仓：`PostToWebUi` / `HotkeyLogLine` / `NotifyWebDebugWindowSetting` / `SyncHomeSelectionCache` **只在 `src/engine/engine_ui_hooks.cpp` 有定义**（其余全是壳侧 `Shell*` 版本） |
| 引擎侧不再 include 壳的头 | ✅ `grep webview_bridge_backend.h src/engine/` → **只命中注释**，无实际 `#include` |
| `g_instance` 迁移 | ✅ 唯一定义在 `src/app_instance.cpp:16`；`taskbar_window.h` 与壳仍是 `extern` 声明；壳 `wWinMain:5081` 赋值；编入 `qst_utils`（`CMakeLists.txt:155`），无 ODR 风险 |
| **引擎可脱离壳独立链接（最关键）** | ✅ `ScriptRunnerSelfTest` 只链 `qst_engine` + `qst_desktop_tools` + `qst_engine_ui_stubs` + WebView2 + 公共库 —— **无任何链接桩、无壳源文件**，且实跑 11/11 通过 |

**B1 的事实更正属实**：第 1 轮验收报告称 4 个符号"只在 `qst_webview_shell.cpp` 定义"。实测 `ShellPostToWebUi` 在 `webview_bridge_backend.cpp:333`、`ShellSyncHomeSelectionCache` 在 `:358`、`ShellNotifyWebDebugWindowSetting` 在 `:372`，只有 `ShellHotkeyLogLine` 在 `qst_webview_shell.cpp:5049`。结论（引擎无法脱离壳链接）不变。**主动更正上游文档的错误，这是本轮最值得肯定的工程习惯。**

### B2 —— 删链接桩

✅ `tools/engine_link_stubs.cpp` 与 `tools/fetch_webpage_link_stub.cpp` **均已物理删除**；CMake 中不再列桩。

### B3 —— 首个引擎执行测试

| 检查点 | 实测 |
|--------|------|
| `ScriptRunnerSelfTest` 存在 | ✅ 448 行 |
| 默认 7 条（CI 安全） | ✅ 无 `--engine` 时 7/7，不依赖桌面/驱动 |
| `--engine` 追加 4 条 | ✅ 实测 **11/11 / exit 0** |
| 边界说明诚实 | ✅ 明确写出「只能用时序语义断言，逐动作级断言要等 #1 抽 `ActionContext`」，并列出未覆盖的 `If/Else` / 窗口模式 / 找图分支 |

**三个实测坑已写进 `module-selftest` 元技能**（已核对内容属实）：`:477` `Shutdown()` 会 `TerminateProcess`、`:480` 必须自己 `PeekMessage`、`:483-485` 循环体生产编码不生成 `endLoop`。

---

## 四、C 段验收（桥接契约正式化）

| 检查点 | 实测 |
|--------|------|
| `bridge_commands.h` 存在 | ✅ 206 行；正式表 **106 条** + 例外表 **7 条**（每条带理由） |
| `BridgeContractSelfTest` 存在 | ✅ 484 行，**8 条**断言，纯源码分析（不链库） |
| 双向校验完整 | ✅ 实测 detail：`表内 106 条全部有入站分支` / `表内每条命令 JS 侧都真的会发` / `C++ 分派 113 条全部已登记` / `JS 实际发的 106 条全部已登记（无孤儿命令）` |

**变异测试（关键）**：把 `ui/bridge.js:27` 的 `saveSettings` 改名为 `saveSettingsX` → 实测：

```
{"name":"table_all_sent_by_js","ok":false,
 "detail":"表里登记了但没有任何 JS 会发（要么该删，要么该移进例外表）：  saveSettings"}
{"name":"js_sends_all_declared","ok":false,
 "detail":"JS 发了但 C++ 不处理 / 未登记（会静默无响应）：  saveSettingsX"}
{"passed":6,"failed":2,"ok":false}   exit=2
```

**恰好 2 条、双向同时变红**，与声称一致。还原后 8/8 绿。诊断文案可直接定位到出问题的命令名 —— 这正是第 1 轮 P0 的形状，现在被锁住了。

**抽查 C1 的 7 条差异结论**：`agentWindow.setTopmost` 经 `grep` 复核 —— C++ 侧有分支（`qst_webview_shell.cpp:2635`）、`ui/` 下无任何发送方 → **「疑似死分支」判断准确**。

---

## 五、实测数据复现

| 声称 | 实测 | 判定 |
|------|------|------|
| 18 suite / 665 用例 / 0 失败 | **18 suite / 665 用例 / 0 失败** | ✅ 完全一致 |
| `ScriptRunnerSelfTest --engine` 11/11 | **11/11 / exit 0** | ✅ |
| `WindowModeSelfTest` 71/71 | **71/71 / exit 0** | ✅ |
| MCP 端到端 6/6 | **26 项全通过 / exit 0** | ⚠ 低报（脚本完整，非覆盖缺失） |
| 契约 A/B 双向变红 | **2 条变红，双向，exit=2** | ✅ |
| 回归锁 A/B 5 条变红 | **7 条变红，exit=7** | ✅ 保守（实际更强） |
| `run_all_selftests.ps1` 可用 | **18/18 通过 / exit 0**（见 §六） | ✅ 补上了第 1 轮 §5.1 的未验证项 |

### 5.1 统一入口实测（闭合第 1 轮 §5.1 的长期悬空项）

第 1 轮验收报告 §5.1 与 `refactor-progress.md` §5.1 都写着「`run_all_selftests.ps1` 未能实机执行——编写环境的脚本执行被拦截，只能静态校验」。

**本轮实测：该脚本完全可用**，`-SkipBuild -Tier logic` 输出：

```
[PASS] ScriptActionBuilderSelfTest  passed=53
[PASS] ScriptIoSelfTest             passed=58
...
[PASS] BridgeJsonSelfTest           passed=9
[PASS] ScriptRunnerSelfTest         passed=7
[PASS] BridgeContractSelfTest       passed=8

===== 汇总：18 个 suite，18 通过，0 失败（Tier=logic）=====
SCRIPT_EXIT=0
```

**"脚本执行被拦截"是个误判**，真实原因是环境变量 **`PATHEXT` 被削成只剩 `.CPL`**（本会话实测 `PATHEXT = .CPL`）。PowerShell 的命令发现依赖 `PATHEXT`，`.EXE` 不在其中时 `& $exe --json` 会被当成"文档"处理，报：

```
无法在管道中间运行文档: D:\other\software\build\Release\ScriptActionBuilderSelfTest.exe。
```

**这解释了上一轮的谜团**：不是沙箱拦脚本，是 `PATHEXT` 缺 `.EXE`。修正 `PATHEXT` 后同一脚本 18/18 全绿。**该脚本本身无缺陷**，无需修改（建议在排查同类"跑不起来"时先看 `$env:PATHEXT`）。

---

## 六、验收新发现的问题

### F1（P1）`docs/refactor-acceptance.md` 被误删，12 处引用断链 —— 已恢复

| 项 | 内容 |
|----|------|
| 现象 | 该文件（第 1 轮验收报告，约 24 KB）在 `docs/` 下**不存在**；`git log --all` 无记录（从未提交）、无 stash，**不可从版本库恢复** |
| 影响 | **12 处引用**指向它，分布在 **7 个文件**： |
| | `.cursor/skills/module-selftest/SKILL.md` ×3（`:436` / `:463` / `:492`） |
| | `docs/refactor-changes-summary.md:4`（自称"依据"） |
| | `docs/refactor-progress.md:140` / `:152`（自称"依据"与"§7.2 三处调整"） |
| | `src/webview/qst_webview_shell.cpp:3160`（**源码注释**） |
| | `tools/bridge_json_selftest.cpp:8` / `:242` |
| | `tools/bridge_contract_selftest.cpp:444` |
| | `tools/script_runner_selftest.cpp:9` / `:401` |
| 断链的实质内容 | ① P0 根因分析（§3）② 测试规格（§7.3，即 A1 的设计依据）③ **A/B/C 段的排序依据（§7.2 三处调整）**——整个第 2 轮的施工顺序都引用它 |
| 处理 | ✅ **已按原文恢复**，并在文件头加状态说明（标明"第 1 轮当时结论"+ 第 2 轮已闭合 A1/A2/A3/B 段，最新进度以另两份为准），保证 12 处 §引用可解析 |

> 若删除是有意为之（例如内容已并入 `refactor-changes-summary.md`），请再删一次并**同时清理这 12 处引用**——目前源码注释与 3 个测试文件头都在引用它。

### F2（P2）文档里的 suite 计数再次漂移 —— 已修正

| 位置 | 原文 | 实际 | 处理 |
|------|------|------|------|
| `AGENTS.md:80` | "构建产品壳 + **18 个** suite 目标" | CI 实际构建 **21 个** SelfTest 目标（逻辑档 18 + 交互类 3） | ✅ 改为 21，并注明"以 `run_all_selftests.ps1` 的 `$LogicSuites` 为准" |
| `.cursor/skills/module-selftest/SKILL.md:26` | "构建 + 跑 **16 个**逻辑 suite" | 逻辑档实际 **18 个** | ✅ 去掉硬编码数字（改为"全部逻辑 suite"） |

> 这是**第二次**出现同类漂移（第 1 轮是 `script_types.h` 的「28 种」）。硬编码计数会随每次新增 suite 失准，建议一律改为指向 `$LogicSuites` 这类单一真值。

### F3（P2）MCP 冒烟项数被低报

`refactor-changes-summary.md` §八 与 `refactor-progress.md` §七 均记为 **"MCP 6/6"**，实测 `tools/test_mcp_server.ps1` 有 **26 项**检查（13 条静态 + 12 条工具存在性循环 + 1），全部通过。**脚本完整未缩减**（本轮对它的唯一改动是第 1 轮的 +5 行编码修复），属低报，不影响结论。

### F4（P2）回归锁强度被低报

A1 声称"把旧 `substr` 打回去 → 5 条变红"，实测 **7 条**。属保守低报，无害。

### F5（P2）CI 编 21 个 target 只跑 18 个

3 个交互类（`WindowModeSelfTest` / `VirtualHidSelfTest` / `InjectionSelfTest`）只编不跑。`refactor-changes-summary.md` §十一.2 已声明为"有意保留编译覆盖"，**本轮改为有意决策，不再是遗漏**。但副作用是：`WindowModeSelfTest`（本机实测 **71/71 可跑**）永远不会 gate CI。可考虑在 self-hosted runner 上跑 `-Tier full`。

### F6（P3）上轮遗留项状态

| 项 | 状态 |
|----|------|
| A4 Base64 仍剩 2 份 | ❌ 未做 —— `qst_webview_shell.cpp:4054`、`ext_bridge_server.cpp:112` 仍是自研实现 |
| A5 `window_mode_types/json.cpp` 重复编译 | ❌ 未做 —— 仍分别 2 份 / 3 份（vcxproj 权威确认） |
| A9 `.research/word-reflow-test.ps1` 缺 BOM | ✅ 已随 `.research` 清理一并消失 |
| A10 `dist/setup-1.0.5-test.log` | ❌ 仍在，不被任何系列匹配 |
| #16 文档分册 | ❌ 未做（自述承认） |

---

## 七、验收操作留痕

| 操作 | 方式 | 结果 |
|------|------|------|
| 18 个 logic suite | `build\Release\<Target>.exe --json` 逐套跑，按 exit code 判定 | **665 用例 / 0 失败** |
| 统一入口 | `tools\run_all_selftests.ps1 -SkipBuild -Tier logic`（修正 `PATHEXT` 后） | **18/18 / exit 0** |
| 引擎自检 | `ScriptRunnerSelfTest.exe --engine --json` | 11/11 / exit 0 |
| 窗口模式 | `WindowModeSelfTest.exe --json` | 71/71 / exit 0 |
| MCP 端到端 | `tools\test_mcp_server.ps1` | 26 项全通过 / exit 0 |
| **变异测试 A1** | `GetSubObjectText` → 改回 `substr(首个 '{')` → 重建 `BridgeJsonSelfTest` | **7 条变红 / exit 7**；还原后 9/9 绿 |
| **变异测试 C1** | `ui/bridge.js` 的 `saveSettings` → `saveSettingsX` | **2 条变红 / exit 2**；还原后 8/8 绿 |
| 依赖倒置核验 | `grep -rn webview_bridge_backend.h src/engine/` | 仅注释命中（无实际 include） |
| 4 个符号定义唯一性 | `grep -rn "^void <fn>(" src/` | 只在 `engine_ui_hooks.cpp` |
| `g_instance` ODR | `grep -rn "^HINSTANCE g_instance" src/` | 唯一定义在 `app_instance.cpp` |
| 工作区洁净 | `git diff --stat src/json_util.h ui/bridge.js` | 空（变异无残留） |
| 构建 | `cmake --build build --config Release --target <T>` | exit 0 |

---

## 八、下一步方向

### 8.1 先清账：代码已不是瓶颈，文档是

本轮代码侧质量明显提升（两个回归锁都过了变异测试，依赖倒置是真的）。**当前最该做的是收口**：

1. **决定 `refactor-acceptance.md` 的去留**（已恢复，12 处引用依赖它）
2. **把 12 处引用改为"内容位置"而非"文档名"**，或明确该文档为长期保留件 —— 避免再次出现"文件没了但引用还在"
3. **消灭硬编码计数**（F2 已修两处，但同类风险仍在）
4. **提交**（见 8.4）

### 8.2 再进 D 段（#4 统一序列化 + schema 版本）—— 优先级最高

**硬要求（第 1 轮验收报告 §7.2 D 已定，本轮重申）**：必须用**真实历史脚本**做往返验证（`dist\` / 用户脚本库），不能只跑 `ScriptIoSelfTest`。理由已在本轮被再次验证——**用例全绿 ≠ 没坏**（A1 的事故正是 641 绿也照不到）。

**具体做法（建议）**：

| 步 | 动作 | 验收口径 |
|----|------|----------|
| D1 | 先建**往返基线**：扫描 `dist\` 与用户脚本库里的真实 `.qst` 脚本，记录 三套序列化路径各自的 读→写→再读 结果 | 基线用例数 + 差异清单 |
| D2 | 以 nlohmann 为唯一实现，`script_io.cpp` / `script_action_builder.cpp` / `webview_bridge_backend.cpp` 三处入口转发 | `ScriptIoSelfTest` 58 条不退；D1 基线零差异 |
| D3 | 加 `"v"` 字段 + `MigrateAction(json, fromVer)` | 旧脚本（无 `v`）读入按 v1 迁移，往返仍零差异 |
| D4 | 新套件 `ScriptSerializationSelfTest`，含**事故形状回归**（类似 A1 的 `regression_trailing_outer_brace`） | 变异测试：把某字段映射改错 → 必须变红 |

> D4 是本轮经验的直接产物：**每改一处边界契约，就配一个能被变异打红的锁**。

### 8.3 与 D 段并行可做的（低风险、独立）

| 项 | 说明 |
|----|------|
| A4 | 2 处残留 Base64 并入 `src/base64.h`（`qst_webview_shell.cpp:4054`、`ext_bridge_server.cpp:112`）—— 各约 15 行 |
| A5 | `window_mode_types.cpp` / `window_mode_json.cpp` 去重（参照 `qst_utils` 的 STATIC 手法） |
| F5 | CI 增加 `-Tier full` 的 self-hosted 作业，让 `WindowModeSelfTest` 真正 gate |
| #16 | `ai-action-exec-optimization.md`（2,056 行）分册 |
| #7 | 统一 HTTP 抽象（`IHttpClient` + `MockHttpClient`），独立于序列化 |
| #10 余项 | 契约表已建，可把 `enum class BridgeCommand` + 请求/响应结构体补齐（本轮只做了"表 + 双向校验"，未做类型化） |

### 8.4 提交（阻塞项）

`refactor-changes-summary.md` §十一.1 自述"全部改动尚未提交"。当前工作区有 **50 个删除 + 28 个修改 + 15 个未跟踪**（含 11 个新文件）。建议：

1. **分两个提交**：① 阶段 0 + #9（第 1 轮）② A/B/C 段（第 2 轮）—— 便于回溯与 `git bisect`
2. 提交前跑一次 `run_all_selftests.ps1`（**本机需先修 `PATHEXT`**，见 §5.1）
3. 首次 push 后**盯 CI**：OpenCV 首次下载（约 250 MB）、`FakeFocus32` 的 x86 工具链 —— 这两项至今未在真实 runner 验证过

### 8.5 明确不建议现在做的

- **不要**因为 F1 就重写文档体系 —— 恢复 + 清理引用即可
- **不要**把 `run_all_selftests.ps1` 改成不依赖 `PATHEXT` 的 `Process.Start` 形式 —— 脚本本身没问题，改它反而引入新风险（且 CI 上 `pwsh` 的 `PATHEXT` 正常）
- **不要**为了"让 CI 全绿"把 3 个交互类 suite 从构建列表删掉 —— 保留编译覆盖有价值，问题在"没跑"，应在 runner 侧解决
- **不要**在 D 段之前动 #1（`ActionHandler`）—— 序列化格式一旦稳定，`ActionContext` 的字段映射才有意义；顺序反了会做两遍
