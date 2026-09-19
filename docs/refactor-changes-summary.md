# 重构改动总清单（阶段 0 → #9 → A/B 段 → C 段）

> 汇总日期：2026-09-18（含两轮实施 + 一次独立验收）
> 依据：`docs/architecture-review.md`（16 项评估）、`docs/refactor-acceptance.md`（逐项实机验收 + §7.2 调整后的顺序）
> 过程记录：`docs/refactor-progress.md`（含每轮的踩坑与验证留痕）
>
> **口径**：本清单只列**实际改动**与**实测结论**；未做的项单独列在 §7。

---

## 一、一句话总览

| 轮次 | 内容 | 结果 |
|------|------|------|
| 第 1 轮 | 阶段 0 全部（#6/#13/#14/#15/#16）+ 阶段 1 的 #9 构建去重 | ✅ 641 用例通过；**但 #13 引入 1 项 P0**（见 §3） |
| 验收 | 独立逐项实机复跑（不采信自述） | 5/6 项属实；抓到 P0 并修复；另修 1 项 P1 工具缺陷 |
| 第 2 轮 | A 段（闭合 P0）+ B 段（依赖倒置、删链接桩、首个引擎执行测试）+ C 段（桥接契约） | ✅ **665 用例 / 0 失败**；引擎可独立链接；桥接命令面双向锁定 |

---

## 二、第 1 轮：阶段 0 + 构建去重

### #6 引入 CI（评估表 S）

| 动作 | 文件 |
|------|------|
| 新增 CI workflow：windows-2022 + OpenCV 缓存 → 构建产品壳与全部 SelfTest 目标 → 跑自检，失败即红 | `.github/workflows/build.yml`（新增） |
| 新增 CI 与本地**共用**的自检入口（构建 + 跑 + 按 exit code 判定 + 失败回显） | `tools/run_all_selftests.ps1`（新增，204 行） |
| 补「一键跑全部自检」与 4 条构建陷阱 | `AGENTS.md` |

### #13 消灭重复工具函数（评估表 S）→ 引入 P0

| 动作 | 文件 |
|------|------|
| Base64 三份实现（`agent_attachment.cpp:29` / `ai_action_service.cpp:96,128` / `ocr_engine.cpp:482`）收敛为唯一 header-only 实现 | `src/base64.h`（新增，74 行） |
| 手写 JSON 扫描（`webview_bridge_backend.cpp` 的 `JsonGetString*/Number/Bool/Int`、`ai_action_service.cpp` 的 `ExtractJsonArrayFromText`）收敛到 nlohmann | `src/json_util.h`（新增，250 行） |
| 三处旧实现物理删除，改 `using` | `src/agent_attachment.cpp` / `src/ai_action_service.cpp` / `src/ocr_engine.cpp` |

> ⚠ 这一项**改变了边界行为**（严格解析 vs 容忍残破 JSON），并触发 §3 的 P0。

### #14 清理遗留物（评估表 S）

| 动作 | 证据 |
|------|------|
| 删根目录 6 个 `.obj` 残留 | `_melon_test/action_utils/conv_test/debug_trace_test/findimage_template_crop/remap_collapsed_test` |
| `git rm query`（9 字节 CLI 残留） | 已暂存删除 + `.gitignore` 覆盖 |
| `.research/` + `.dsh-research/` 取消追踪（49 文件） | `.gitignore` 用 `.research/*` + `!.research/office-document-io-report.md`（该报告被 AGENTS.md 引用，保留） |
| 删空目录 `archive/` | — |

### #15 dist 保留策略（评估表 S）

| 动作 | 文件 |
|------|------|
| 新增保留策略：每系列留最近 5 版（语义版本排序），旧的移入 `dist\archive\`，清理 `_edge_pack_stage_*` | `tools/prune_dist.ps1`（新增，148 行） |
| 接入两个打包脚本尾部 | `tools/package_release.ps1` / `tools/package_webview_portable.ps1` |

实测（验收会话真机跑通）：installer 27→留 5 归档 22；setup 28→留 5 归档 24；zip 29→留 5 归档 24；共 70 文件 / 15.23 GB 待归档；`dist\` 总占用 20.35 GB。无版本号固定别名与大小写异常文件名均正确保护。

### #16 文档校对（评估表 M，部分）

| 动作 | 文件 |
|------|------|
| 类型注释「共 28 种」→ 实测 **44 种**；补「新增动作必须同步的四处」清单 | `src/script_types.h` |
| 标注 `ScriptAction` 为 **126 字段**扁平结构及副作用 | `src/script_types.h` |
| 🟡 `ai-action-exec-optimization.md`（2,056 行）分册**未做** | — |

### #9 构建去重（评估表 M）—— 消除约 34,400 行重复编译

| 动作 | 效果 |
|------|------|
| `utils.cpp`（1,814 行）原被 4 个 target 各编一份 → 抽 `qst_utils` **STATIC** 库 | 工程文件权威确认：`utils.cpp` 4 → 1（只在 `qst_utils.vcxproj`） |
| `AgentAssistantSelfTest` 从直接列 24 个被测源（22,595 行）→ 改链 `qst_engine` | 少编 22,595 行，用例数不变（61） |
| `AiActionRouterSelfTest` 从直接列 9 个被测源（11,791 行）→ 改链 `qst_engine` | 少编 11,791 行，用例数不变（171） |
| 删死文件 | `tools/fetch_webpage_link_stub.cpp` |

> 为什么必须 STATIC：**OBJECT 库的 `.obj` 不会经由中间 OBJECT 库传递给消费者**（实测：只链 `qst_engine` 会缺 `qst_desktop_tools` 的符号）。这条已写进 `CMakeLists.txt` 注释。

---

## 三、验收会话发现并修复的缺陷

| 项 | 类型 | 根因 | 修复 |
|----|------|------|------|
| **saveSettings 静默失效** | **P0** | ① `bridge.js` 发 `{"type":"saveSettings","settings":{…}}` ② 壳 `qst_webview_shell.cpp:3154` 用 `json.substr(brace)` 截到**末尾** → payload 多带外层 `}` ③ 换 nlohmann 严格解析后所有键取不到 → 各字段保持旧值 → 末尾写回盘 → 仍报「保存成功」 | 改用 `FindMatchingJsonBrace` 截**配对**括号（第 2 轮 A1 进一步上收为 `GetSubObjectText`） |
| `test_mcp_server.ps1` 编码缺陷 | P1 | `ProcessStartInfo` 未设 `StandardOutputEncoding`，.NET Framework 默认取控制台代码页（936）→ 子进程 UTF-8 中文被解坏 → `ConvertFrom-Json` 抛错。**该冒烟自建立起从未真正通过** | 显式指定 UTF8；修复后 22 项全通过 |

> 641 用例全绿也没拦住 P0 —— 因为 `saveSettings` 全链路在**桥接层**，而桥接层零自检覆盖。这正是后续 A/C 段的动因。

---

## 四、第 2 轮 A 段：闭合 P0，把边界行为固化

### A1 — 回归锁（验收报告 §7.3 规格）

| 动作 | 文件 |
|------|------|
| 新增 `GetSubObjectText()`：严格解析取**配对**子对象；`dump(..., error_handler_t::replace)` 顺带堵住「非法 UTF-8 抛 `type_error.316`」 | `src/json_util.h` |
| 壳的 `saveSettings` 改用它，删掉脆弱的 `substr(首个 '{')` | `src/webview/qst_webview_shell.cpp` |
| 新增 `BridgeJsonSelfTest`：**9 条**断言（规格 5 条 + 事故原始形状 + 诊断静默/上报 + 子对象可被取值函数消费） | `tools/bridge_json_selftest.cpp`（新增，280 行） |

**A/B 实证**：把旧 `substr` 写法打回去 → **5 条立刻变红**；还原后 9/9 绿。锁是真的。

### A2 — 让「静默失败」变可观测

| 动作 | 文件 |
|------|------|
| 新增 `NoteParseFailure` / `ParseFailureCount` / `ResetParseFailureCount` / `SetParseFailureSink` / `IsParseableObject` | `src/json_util.h` |
| `ApplySaveSettingsJson` 入口做**一次**前置校验并上报（不是改 100 个 `if (GetX(...))`） | `src/webview/webview_bridge_backend.cpp` |
| 壳在 `wWinMain` 装 sink 写 `webview_boot.log`（`JSON: 解析失败 where=saveSettings.payload …`） | `src/webview/qst_webview_shell.cpp` |

未采纳验收报告 §7.4 明令禁止的两件事：**没有**加「宽容模式」开关、**没有**回退到手写字符扫描。

---

## 五、第 2 轮 B 段：消掉链接 hack，让引擎可测

### B1 — engine → shell 依赖倒置

| 动作 | 文件 |
|------|------|
| 新增 `UiBridgeHooks`（4 个 `std::function`）+ `SetUiBridgeHooks`（**只覆盖非空成员**）+ `UiHooks()` / `UiBridgeHooksInstalled()` | `src/engine/engine_ui_hooks.h`（新增，69 行） |
| **4 个函数的定义搬到引擎侧**（默认 no-op 转发）：`qst::webview::PostToWebUi` / `HotkeyLogLine` / `NotifyWebDebugWindowSetting` / `SyncHomeSelectionCache` | `src/engine/engine_ui_hooks.cpp`（新增，83 行） |
| 壳侧真实现改名 `Shell*` 并注入：`InstallBridgeUiHooks()` + `InstallShellUiHooks()` | `src/webview/webview_bridge_backend.cpp` / `.h`、`src/webview/qst_webview_shell.cpp` |
| `g_instance` 定义搬到共享基础库（原在壳里，被 `taskbar_window.h` inline 函数、`engine_gdi_editor.cpp`、`engine_host_window.h` 引用） | `src/app_instance.cpp`（新增，16 行）+ 编入 `qst_utils` |
| **引擎侧不再 include 壳的头文件**：`webview/webview_bridge_backend.h` → `engine/engine_ui_hooks.h` | `src/engine/engine_host_window.h` / `src/engine/engine_settings_reload.cpp` |

**事实更正**：验收报告称这 4 个符号「只在 `qst_webview_shell.cpp` 定义」。实测 `PostToWebUi` / `SyncHomeSelectionCache` / `NotifyWebDebugWindowSetting` 定义在 **`webview_bridge_backend.cpp`**，只有 `HotkeyLogLine` 在 shell。结论（引擎无法脱离壳链接）不变。

### B2 — 删链接桩

删 `tools/engine_link_stubs.cpp`（被依赖倒置取代）与 `tools/fetch_webpage_link_stub.cpp`（死文件）；两个 SelfTest 的 CMake 配置不再列桩。

### B3 — 首个引擎执行测试

| 动作 | 文件 |
|------|------|
| 新增 `ScriptRunnerSelfTest`：默认 **7 条** UI 钩子契约（CI 安全）；`--engine` 追加 **4 条** headless 引擎真实跑 Wait/Loop/Goto/VarCompute | `tools/script_runner_selftest.cpp`（新增，448 行） |

**边界（诚实说明）**：Wait/Loop/Goto/VarCompute 的*执行*覆盖到了，但断言只能用**时序语义**（循环重复→耗时成倍、goto 跳转→耗时骤减）。逐动作级断言要等 #1 抽出 `ActionContext`。`If/Else`、窗口模式、找图分支同样未覆盖。

**三个实测坑**（已写进 `module-selftest` 元技能）：
1. 测试里**不能调** `qst::engine::Shutdown()` —— headless 窗 `WM_DESTROY` 走 `TerminateProcess`，杀掉测试进程；stdout 是块缓冲 → 输出全丢、exit 还是 0。
2. headless 跑动作**必须自己 `PeekMessage`**，否则收尾信号不派发、`IsRunning()` 永远为真。
3. 循环体**别手搓 indent** —— 生产编码是 `[loop(indent=0), body(indent=1)]`、**构建器不生成 endLoop**；显式塞 `indent=1` 的 endLoop 会把循环体切断。

---

## 六、第 2 轮 C 段：桥接契约正式化（#10，提前到 #4 之前）

### C1 — 命令表 + 双向一致性校验

| 动作 | 文件 |
|------|------|
| 新增命令契约表：**106 条** JS→C++ 命令（含 sender 文件）+ **7 条** C++ 独有命令（含理由） | `src/webview/bridge_commands.h`（新增，206 行） |
| 新增 `BridgeContractSelfTest`：**8 条**断言，双向校验「表 ↔ C++ 入站分派 ↔ JS 发送侧」 | `tools/bridge_contract_selftest.cpp`（新增，484 行） |
| 壳的未知命令处理补日志：区分「JS 发了未登记命令」与「表与实现漂移」，写 `webview_boot.log` | `src/webview/qst_webview_shell.cpp` |

**8 条断言**：表无重名/无交集、表内每条在 C++ 分派里有分支、**表内每条 JS 侧真的会发**、sender 文件存在、C++ 分派全部已登记、例外表有理由且确有分派、例外当前无人发、JS 发的无孤儿。

**A/B 实证**：把 `bridge.js` 的 `saveSettings` 改名 → 两个方向同时变红（`table_all_sent_by_js` + `js_sends_all_declared`），正是 P0 的形状。

### C1 顺带查清的 7 条差异（全部有据可查，非 bug）

| 命令 | 性质 |
|------|------|
| `setMode` / `modeReady` / `close` / `drag` / `minimize` | 历史别名：C++ 用 `\|\|` 与 `window.*` 并列接受，JS 只发 `window.*` 形式 |
| `agentWindow.setTopmost` | 疑似死分支：无任何 JS 发它；JS 侧只有 `debugWindowSetTopmost` |
| `debugWindow.needTheme` | C++→JS 消息名（`PostToJs`），入站分派把它当 `debugWindow.ready` 的别名接受 |

### 提取规则（避免误报，实测踩过）

- **C++**：`type == "X"`。
- **JS 专用发送方**（`bridge.js` / `debug.html` / `agent.html`）→ 取全部 `type: "X"`。
- **JS 混合文件**（`app.js` / `pro-mode.js` / `visual_editor.js` / `index.html`）→ **只取出现在 `post(...)` / `postMessage(...)` / `qst.post(...)` 实参里**的字面量。
  > 不加这个区分会出现 7 个假阳性：`loop` / `else` / `macro` / `ai` / `rec` / `sched` / `paint`（脚本动作类型与标签页 kind，不是桥接命令）。

---

## 七、新增 / 修改文件总表

### 新增（11 个）

| 文件 | 行数 | 作用 |
|------|------|------|
| `src/base64.h` | 74 | Base64 唯一实现（header-only） |
| `src/json_util.h` | 250 | JSON 读写唯一实现 + `GetSubObjectText` + 解析失败诊断 |
| `src/app_instance.cpp` | 16 | `g_instance` 定义（原在壳里） |
| `src/engine/engine_ui_hooks.h` | 69 | 引擎→壳唯一出口（依赖倒置） |
| `src/engine/engine_ui_hooks.cpp` | 83 | 钩子存储 + 4 个转发定义 |
| `src/webview/bridge_commands.h` | 206 | 桥接命令契约表（106 + 7） |
| `tools/bridge_json_selftest.cpp` | 280 | 桥接 JSON 边界契约（9 断言） |
| `tools/script_runner_selftest.cpp` | 448 | 引擎执行侧（7 + 4 断言） |
| `tools/bridge_contract_selftest.cpp` | 484 | 桥接命令契约（8 断言） |
| `tools/run_all_selftests.ps1` | 204 | 自检统一入口（CI 与本地共用） |
| `tools/prune_dist.ps1` | 148 | dist 保留策略 |
| `.github/workflows/build.yml` | — | CI |

### 修改（19 个，按主题）

| 主题 | 文件 |
|------|------|
| 构建配置 | `CMakeLists.txt`、`cmake/QstProductLibs.cmake` |
| 去重改动点 | `src/agent_attachment.cpp`、`src/ai_action_service.cpp`、`src/ocr_engine.cpp` |
| 依赖倒置 | `src/engine/engine_host_window.h`、`src/engine/engine_settings_reload.cpp`、`src/webview/webview_bridge_backend.h`、`src/webview/webview_bridge_backend.cpp`、`src/webview/qst_webview_shell.cpp` |
| 文档校对 | `src/script_types.h` |
| 打包脚本 | `tools/package_release.ps1`、`tools/package_webview_portable.ps1` |
| 编码修复（补 BOM） | `driver/qst_vhid/install_fake_driver.ps1`、`driver/qst_vhid/repair_boot.ps1`、`driver/qst_vhid/portable/install_portable.ps1`、`tools/fetch_webview2_fixed.ps1`、`tools/package_webview_portable.ps1` |
| 仓库卫生 | `.gitignore` |
| 约定与索引 | `AGENTS.md`、`.cursor/skills/module-selftest/SKILL.md` |

> 另有 `src/window_mode/*`、`.cursor/skills/window-mode-debug/*`、`tools/window_mode_selftest.cpp`、`tools/test_mcp_server.ps1` 是**前序会话**的未提交改动，不在本次两轮范围内。

---

## 八、验证矩阵

| 轮次 | 构建 | 逻辑层自检 | 其它 |
|------|------|-----------|------|
| 第 1 轮 | 全绿 | 15 suite / **641** 用例 / 0 失败 | VirtualHid 驱动脚本断言 8/8 |
| 验收 | 全绿 | 复现 **641**（580+61），一个不差 | MCP 端到端 22 项；`prune_dist -DryRun` 跑通 |
| 第 2 轮 A+B | 全绿 | 17 suite / **657** 用例 / 0 失败 | `ScriptRunnerSelfTest --engine` 11/11；MCP 6/6；`WindowModeSelfTest` 71/71 |
| 第 2 轮 C | 全绿 | 18 suite / **665** 用例 / 0 失败 | MCP 6/6；契约 A/B 双向变红验证 |

---

## 九、过程中发现的、原评估表未记录的问题

1. **引擎对壳有隐式符号依赖** —— `qst_engine` 无法脱离壳链接（缺 4 个函数 + `g_instance`）。这才是 P1-5「引擎主循环不可测」的**机制性根因**（不是「没人写测试」）。已在 B 段修复。
2. **OBJECT 库的 `.obj` 不跨中间 OBJECT 库传递** —— 想让源只编一次必须用 STATIC 库。
3. **5 个 `.ps1` 含中文却缺 BOM** —— 含 `package_webview_portable.ps1`（261 个中文字符），会让便携包内的中文 README 整个乱码并进 zip。
4. **MSBuild 遇 `https_proxy`/`HTTPS_PROXY` 并存直接 `MSB6001` 崩** —— 报错完全指不到原因（看起来像编译开关问题）。
5. **`qst::engine::Shutdown()` 会 `TerminateProcess`** —— 在测试进程里调用会静默杀掉自己并丢掉缓冲输出（exit 仍是 0）。
6. **headless 引擎跑动作需自抽消息** —— 否则收尾信号不派发。
7. **循环体生产编码不生成 `endLoop`** —— 手搓 `indent` 会把循环体切断。
8. **桥接层 7 条命令差异** —— 5 条历史别名 + 1 条疑似死分支 + 1 条 C++→JS 消息名被当入站别名。

---

## 十、未做项与下一步

| 顺序 | 项 | 状态 |
|------|----|------|
| **D** | **#4 统一序列化 + schema 版本** | 下一步。硬要求：**必须用真实历史脚本做往返验证**（`dist\` 与用户脚本库），不能只跑 `ScriptIoSelfTest` —— 本次事故的教训是「用例全绿 ≠ 没坏」 |
| E | #1 ActionHandler 注册表 | 需要先抽 `ActionContext`（分发链每个分支都依赖执行期局部状态）。做完才能给 B3 补上逐动作级断言 |
| E | #3 灭 `gh*` 全局态 → #2 拆 `EngineHost` | 依赖 #1 |
| F | #5 动作模型结构化 / #7 HTTP 抽象 / #8 引擎可测化（深度） / #11 前端模块化 / #12 AI 编排拆分 | 独立可做 |
| F | #16 文档分册（`ai-action-exec-optimization.md` 2,056 行） | 未做 |

---

## 十一、风险与注意事项

1. **全部改动尚未提交**：`git rm --cached` 的 49 个文件、删除的 6 个 `.obj`、以及上述 11 新增 + 19 修改都在工作区暂存/未跟踪状态。
2. **CI 仍未在真实 runner 跑过**：OpenCV 首次下载（约 250MB）、`FakeFocus32` 的 x86 工具链、3 个交互类 suite 只编不跑（有意保留编译覆盖）都需首次 push 验证。
3. **`prune_dist.ps1` 未执行过真实清理**：`dist\` 仍是 20.35 GB。要回收：先 `-DryRun` 确认，再 `-PurgeArchive`。
4. **`build\` 仍有历史陈旧 `.obj`**：不影响正确性，但占空间；需要时 `cmake --build build --target clean` 或重建目录。
5. **桥接契约测试依赖源码可读**：从拷贝出去的 build 目录单独跑会跳过（判 ok=true），不会假红；但也意味着 CI 之外要保证源码在场。

---

## 十二、第 3 轮（发版工具链 + FakeFocus32 + A4/A5）

> 验收全文：[`docs/refactor-acceptance-round3.md`](refactor-acceptance-round3.md)；过程记录：[`docs/refactor-progress.md`](refactor-progress.md) §九。
> 本轮**不是重构轮**：主线是发版工具链与 FakeFocus32 构建修复；A4/A5 由验收会话闭合。

| 类别 | 内容 | 文件 |
|------|------|------|
| **发版工具链** | 一条命令走完「写版本号三处 → 构建 → 组装 dist + 便携 zip → 编安装包 → 同步官网」；版本号用正则写入、构建目标从 `package_release.ps1` 解析 `--target`、失败默认回滚 | `tools/package_with_version.ps1`（新增 481 行）、`release.cmd`（新增 49 行）、`AGENTS.md` |
| **FakeFocus32 修复** | 修掉「`if defined ProgramFiles(x86)` 括号打断 `if` 解析 → BuildTools 装在 `Program Files (x86)` 时永远静默跳过 32 位 DLL」。改先 `set "PF86=..."` 再判断 + vswhere 兜底 | `src/window_mode/fake_focus/build_fakefocus32.cmd` |
| **window_mode 调整** | `WakeMapleStoryInputPolling()` 注入后真激活唤醒轮询（禁假 `WM_ACTIVATE`）；`TargetOwnsForegroundWindow()` 让方向键兜底只在目标为前台时补；`MaplePollHitSummary()` 诊断解码 | `window_mode_executor.cpp`、`background_window_input.{cpp,h}`、`window_mode_requirements.h`、`fake_focus_dll.cpp` |
| **A4 闭合** | 残留 2 处自研 Base64 → `qst::base64::Encode`；全仓字母表只剩 `src/base64.h` | `qst_webview_shell.cpp`、`ext_bridge_server.cpp` |
| **A5 闭合** | 新增 `window_mode_common`（STATIC）承载 `window_mode_types.cpp` + `window_mode_json.cpp`；`types` 2→1、`json` 3→1，**消除约 1,246 行重复编译** | `CMakeLists.txt`、`cmake/QstProductLibs.cmake` |

### 验证

| 项 | 结果 |
|----|------|
| 发版 `-DryRun` | exit 0；计划完整；三处版本号未变；两条正则独立复核各只替换 1 处 |
| `FakeFocus32` 构建 | **`[FakeFocus32] OK:`**；PE `Machine=0x014c`（真 32 位） |
| 全量构建 | exit 0，无 error |
| logic 档 | **18 suite / 665 用例 / 0 失败** |
| MCP 端到端 | **26 项全通过 / exit 0** |

### 待处理（第 3 轮验收新发现）

| # | 项 | 级别 |
|---|----|------|
| F1 | 两份重构文档本轮未同步 + 5 处过时陈述（已在 `refactor-progress.md` §9.6 更正） | P1 |
| F2 | `WindowModeSelfTest / background_minimized_quiet_restore` **抖动 40%**（固定 sleep 读前台，前台切换是异步的）。该 suite 在 `full` 档，**修好前不要把 `-Tier full` 接进 CI** | P1 |
| F3 | 发版脚本覆盖不到的 4 处硬编码版本号（`.iss:4` 自指检查行每次必错、`website` 两份 README） | P2 |
| F4 | `-DryRun` 不校验版本号正则（在写文件之前就 `exit 0`） | P2 |

### 下一步

**D 段（#4 统一序列化 + schema 版本）** —— 三轮下来始终未动，且是评估表里唯一同时满足「高价值 + 独立可做 + 不依赖 #1/#2」的 L 级项。硬要求：**必须用真实历史脚本做往返验证**（`dist\` 与用户脚本库），不能只跑 `ScriptIoSelfTest`。
先清账：补文档（F1）、修 F3、修 F2 的抖动、**分批提交**（当前 50 删除 + 32 修改 + 19 未跟踪）。

