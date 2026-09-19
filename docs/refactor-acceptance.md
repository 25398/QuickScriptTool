# 重构验收报告（阶段 0 + 阶段 1 · #9）

> **状态（2026-09-18 补记）**：本文件曾被误删（未被 git 跟踪，无法从历史恢复），
> 由第 2 轮验收会话按原文恢复。它被 **12 处**引用（含 `src/webview/qst_webview_shell.cpp`
> 源码注释、3 个测试文件头、`module-selftest` 技能、两份重构文档）。
>
> 本文件是**当时（第 1 轮结束后）**的验收结论，保留原貌以便 §编号可解析。
> 其中 §六 的遗留项 A1/A2/A3/B 段已在第 2 轮闭合，**最新进度以
> [`docs/refactor-progress.md`](refactor-progress.md) 与
> [`docs/refactor-changes-summary.md`](refactor-changes-summary.md) 为准**。

> 验收日期：2026-09-18
> 验收对象：`docs/refactor-progress.md` 声称完成的 #6 / #13 / #14 / #15 / #16 / #9
> 验收方式：**逐项实机复跑**（构建、自检、脚本、探针），不采信自述
> 相关文档：`docs/architecture-review.md`（评估）、`docs/refactor-progress.md`（实施记录）

---

## 一、总评

| 维度 | 结论 |
|------|------|
| 声称完成度 | 6 项中 **5 项属实**，#16 自述为部分完成（属实） |
| 自述数据准确性 | **全部可复现**：641 用例 / 0 失败、utils.cpp 4→1、prune 策略、清理项 —— 无一虚报 |
| 工程质量 | 高。注释写清了「为什么」与踩坑，风险自述诚实（`refactor-progress.md` §5 已预告 4 项遗留风险） |
| **验收新发现** | **1 项 P0 回归（已修）** + 1 项 P1 工具缺陷（已修）+ 6 项遗留/偏差 |

**一句话**：本轮重构的**纪律**是合格的，但**"安全重构"这个前提被打破了两次**——两处都是"改了边界行为、没有测试能看见"。这恰好印证了评估里 P1-5/P1-3 的判断：引擎与桥接层零测试覆盖，是当前最大的风险敞口。

---

## 二、逐项验收

### #6 CI —— ✅ 属实（附 2 项偏差）

| 检查点 | 实测 |
|--------|------|
| `.github/workflows/build.yml` 存在、YAML 合法 | ✅ PyYAML 解析通过；8 个 step；runner `windows-2022` |
| `tools/run_all_selftests.ps1` 存在、含 BOM | ✅ 201 行，BOM `efbbbf` |
| 依赖的 `scripts/setup_opencv.ps1` 存在 | ✅ 存在，逻辑正确（下载 SFX → 解压 → `third_party/opencv`） |
| 依赖的 `-DQST_WEBVIEW_BUNDLE_FIXED=OFF` 选项存在 | ✅ `CMakeLists.txt:12` |
| 本地与 CI 共用同一入口 | ✅ `AGENTS.md` 已同步「一键跑全部自检」与 4 条构建陷阱 |

**偏差 1：CI 编 18 个 target 但只跑 15 个。**
workflow 的 `Build selftest targets` 列了 `WindowModeSelfTest` / `VirtualHidSelfTest` / `InjectionSelfTest`，但 `Run self-tests` 用 `-Tier logic`（15 个），这 3 个只编不跑。要么从构建列表去掉，要么在 self-hosted runner 上跑 `-Tier full`。

**偏差 2：本机 OpenCV 路径与 CI 假设不一致，`setup_opencv.ps1` 从未在本机跑完。**
- 本机实际：`CMakeCache` → `FIND_PACKAGE_MESSAGE_DETAILS_OpenCV=[D:/OpenCV/opencv/build]`
- `third_party/` 下只剩 `opencv-4.10.0-windows.exe`（115 MB SFX）+ `opencv-src/`，**`third_party/opencv` 不存在**
- 即该脚本的"下载→解压→Move-Item"链路在本机中途失败过，CI 上是**首次真正执行**

> 结论：#6 的**判定口径**是可靠的（`run_all_selftests.ps1` 本地实跑通过），但 CI 本身仍属"纸面通过"，首次 push 必须盯着 OpenCV 那一步。

### #13 消灭重复工具函数 —— ✅ 属实（但**引入 1 项 P0 回归**，见第三节）

| 检查点 | 实测 |
|--------|------|
| `src/base64.h` 存在 | ✅ 74 行，header-only，带「为什么 header-only」的理由 |
| 三处旧实现已删（非仅新增头文件） | ✅ `agent_attachment.cpp` / `ai_action_service.cpp` / `ocr_engine.cpp` 内的 `Base64Encode`/`Base64Decode` 已物理删除，改为 `using` |
| **语义等价性**（逐行比对） | ✅ 三份删除实现的位运算、填充分支、`'='` 截断语义与 `qst::base64::Encode/Decode` **完全一致**（含空输入、1/2 字节尾部的边界） |
| `src/json_util.h` 存在 | ✅ 157 行，nlohmann 非抛异常解析 |
| 旧手写扫描已删 | ✅ `webview_bridge_backend.cpp` 的 `JsonGetNumber/Bool/Int/StringArray`、`IsEditorActionTypeTokenUtf8` 已删；`ai_action_service.cpp` 的 `ExtractJsonArrayFromText` 已删 |
| `ExtractFirstJsonArray` 语义等价 | ✅ 与旧 `ExtractJsonArrayFromText` 逐行一致（跳过非 `{`/`[` 的方括号、按深度配对、字符串内括号不计） |

**⚠ 偏差：Base64 实际有 5 份，本轮只收敛了 3 份。** 评估表漏了 2 处，本轮也漏了：

| 位置 | 说明 |
|------|------|
| `src/webview/qst_webview_shell.cpp:4042` | `readImageDataUrl` 的内联编码循环，自带 `kB64` 字母表 |
| `src/window_mode/ext_bridge/ext_bridge_server.cpp:111` | `Base64Encode()` 函数（给 WebSocket `Sec-WebSocket-Accept` 用） |

### #14 清理遗留物 —— ✅ 属实

| 检查点 | 实测 |
|--------|------|
| 根目录 6 个 `.obj` | ✅ 已无残留 |
| `git rm query` | ✅ 已暂存删除；文件仍在磁盘但已被 `.gitignore:45` 覆盖（`git check-ignore` 确认） |
| `.research/` / `.dsh-research/` 取消追踪 | ✅ 共 49 个 `D`；`.gitignore` 用 `.research/*` + `!.research/office-document-io-report.md` 白名单 |
| 被 AGENTS.md 引用的报告仍保留且 tracked | ✅ `git ls-files .research/` → 仅 `office-document-io-report.md` |
| `archive/` | ✅ 已删（空目录） |
| 删除项只在暂存区、未提交 | ✅ 与 §5.5 自述一致 |

### #15 dist 保留策略 —— ✅ 属实（**已实机跑通**，补上 §5.1 的未验证项）

`prune_dist.ps1 -DryRun` 实测输出：

```
[DryRun] 删除临时目录 _edge_pack_stage_81f433bcb196478d95bde6e9462d593c
安装包(QuickScriptTool-<ver>.exe)：保留 5 个，归档 22 个
安装包(Setup-<ver>)：             保留 5 个，归档 24 个
绿色版 zip(Release-<ver>)：       保留 5 个，归档 24 个
保留策略：归档 70 个文件，共 15.23 GB
dist\ 当前总占用：20.35 GB
```

| 检查点 | 实测 |
|--------|------|
| 脚本可执行（补 §5.1） | ✅ 真机跑通（原报告称"编写环境被拦截，只能静态校验"） |
| 版本号排序（非文件名序） | ✅ 用 `[version]` 转型，`1.0.10` 正确排在 `1.0.9` 之后 |
| 无版本号的固定别名不被误移 | ✅ `QuickScriptTool-Setup.exe` / `-Release.zip` 正则不匹配，安全 |
| 大小写异常文件名 | ✅ `quickscripttool-setup-1.2.0.exe`（磁盘上确为小写）被 `-match` 大小写不敏感正确识别 |
| 归档而非删除（默认非破坏） | ✅ 默认 `Move-Item` 到 `dist\archive\`，需显式 `-PurgeArchive` 才回收 |
| 已接入两个打包脚本 | ✅ `package_release.ps1:572`、`package_webview_portable.ps1:146` |

> 唯一遗留：`dist\setup-1.0.5-test.log` 之类散落文件不在任何系列内，不会被清理。影响可忽略。

### #16 文档校对 —— 🟡 部分（自述属实）

✅ `script_types.h:45` 已改为「共 44 种」（枚举成员实测 44 个），并补上「新增动作必须同步的四处」（`script_action_builder.cpp` / `script_io.cpp` / `ui/` / `engine_script_run.cpp`）——这条比原评估的表述更有用，因为它把 P0-1 的耦合显式写成了检查清单。

🟡 `docs/ai-action-exec-optimization.md`（2,056 行）分册未做，自述已在 §4 承认。

### #9 构建去重 —— ✅ 属实（权威口径复核通过）

不采信磁盘上的 `utils.obj` 计数（中间构建会留残留），改用**生成的工程文件**判定：

```
$ grep -E 'Include="[^"]*[\\/]utils\.cpp"' build/*.vcxproj
  qst_utils.vcxproj  →  1 处          ← 唯一编译点
```

| 检查点 | 实测 |
|--------|------|
| `utils.cpp` 4 → 1 | ✅ **权威确认**，只在 `qst_utils.vcxproj` |
| `qst_utils` 用 STATIC（非 OBJECT） | ✅ 理由写进 CMakeLists 注释：OBJECT 库的 `.obj` 不跨中间 OBJECT 库传递 |
| `AgentAssistantSelfTest` 改链 `qst_engine` | ✅ 原先 24 个源 → 现仅 `tools/agent_assistant_selftest.cpp` + `engine_link_stubs.cpp` + 4 个库 |
| `AiActionRouterSelfTest` 改链 `qst_engine` | ✅ 原先 9 个源 → 现 2 个源 + 4 个库 |
| 用例数未因改链接而缩水 | ✅ 171 / 61，与改前一致 |
| 旧 stub 处理 | ⚠ `window_mode_link_stubs.cpp` 仍在用（`ScriptActionBuilderSelfTest` / `ScriptIoSelfTest`）✅；`fetch_webpage_link_stub.cpp` **已成死文件**（CMake 只剩注释引用），未删 |

**⚠ 偏差：P1-8 的另两项未做**（原评估提到 3 处重复编译，本轮只做了 `utils.cpp`）：

| 源文件 | 仍在几个 target 编译 |
|--------|---------------------|
| `window_mode_types.cpp` | 2（`window_mode_core` + `script_action_builder_core`） |
| `window_mode_json.cpp` | 3（`qst_engine` + `script_action_builder_core` + `WindowModeSelfTest`） |

---

## 三、验收发现的 P0 回归（已修复并验证）

### 3.1 现象：保存设置完全静默失效

**根因链**（三步，缺一不可）：

1. `ui/bridge.js:27` 发的消息是 `{"type":"saveSettings","settings":{...}}`
2. `src/webview/qst_webview_shell.cpp:3154`（**本轮未改**）用 `payload = json.substr(brace)` 取子对象 → 取到 `{...}}`，**多带一个外层 `}`**
3. 本轮把 `JsonGetBool/Number/Int` 从**手写字符扫描**换成 **nlohmann 严格解析**（`json_util.h`）→ 尾随 `}` 使 `json::parse` 直接判 `is_discarded()` → 所有键取不到

**后果**：`ApplySaveSettingsJson` 里全部 `if (GetX(...))` 为假 → 各字段保持**从磁盘加载的旧值** → 末尾 `SaveAppSettings(s)` **把旧值写回** → `return true` → UI 提示「保存成功」。
用户视角：**改任何设置都提示成功，实际全部回滚，重启依旧，且无任何报错。**

**为什么没被 641 个用例拦住**：`saveSettings` 全链路在桥接层，而桥接层**零自检覆盖**（评估 P1-3 已指出）。用例全绿是真实结论，只是照不到这里。

### 3.2 实证（用**生产头文件**编译的探针，非模拟）

```
修复后 payload = {"enableRandomInterval":true,"themeId":7,"jitterX":3,"uiScaleFactor":1.25}
修复前 payload = {"enableRandomInterval":true,"themeId":7,"jitterX":3,"uiScaleFactor":1.25}}

-- 修复后（真实 json_util.h）--
  enableRandomInterval = true
  themeId              = 7
  jitterX              = 3
  uiScaleFactor        = 1.250000

-- 修复前（同一 json_util.h）--
  themeId              = 取不到
```

探针直接 `#include "json_util.h"` + 链接真实 `src/utils.cpp` 的 `FindMatchingJsonBrace`，即走的是产品同一份代码。

### 3.3 修复

`src/webview/qst_webview_shell.cpp` —— 用已有的 `FindMatchingJsonBrace()` 截到**配对的** `}`：

```cpp
const auto end = FindMatchingJsonBrace(json, brace);
payload = (end == std::string::npos) ? json : json.substr(brace, end - brace + 1);
```

**影响面排查**：`qst_webview_shell.cpp` 里只有这一处 `json.substr(brace)` 取子对象（另两处 `substr` 一处是 `substr(brace, end - brace + 1)` 已正确、一处是取引号内字符串）。其余 7 个桥接入口（`ParseDebugScriptJson` / `StartClickerFromOpts` / `SaveEditorFromJson` / `BeginSendAgentMessage` / `SaveAgentDraft` / `SaveScheduledTaskFromJson` / `ApplyOptimizeAndSave`）传的都是**整条 `json`**（合法 JSON），不受影响。

### 3.4 验证

| 项 | 结果 |
|----|------|
| 重新构建 `QstWebViewShell` | ✅ exit 0，`qst_webview_shell.cpp` 已重编、`QuickScriptTool.exe` 已重链 |
| 探针（真实头文件） | ✅ 修复后 4 个字段全部正确读出 |
| 15 个 logic suite 回归 | ✅ **641 用例 / 0 失败**（与修复前一致） |
| MCP 端到端冒烟（真拉子进程） | ✅ 全通过 / exit 0（见 3.5） |

---

## 四、验收发现的 P1 工具缺陷（已修复并验证）

### 4.1 `tools/test_mcp_server.ps1` 的编码缺陷 —— 该冒烟从未绿过

`AGENTS.md` 把 `test_mcp_server.ps1`（exit 0 才算过）写成 MCP 能力的验收关口。实测首次运行即抛异常：

```
EXCEPTION: 传入的对象无效，应为":"或"}"。 (206):
{"id":1,...,"serverInfo":{"name":"quickscripttool","title":"QuickScriptTool 鍘熺敓妗岄潰鎺у埗锛圵GC 鎴獥 / SendInput / UIA / 鏂囨。璇诲彇锛?,"version":"1.0"}}}
```

`鍘熺敓妗岄潰` 是典型的 **UTF-8 被当 GBK 解码**。根因：`ProcessStartInfo` 未设 `StandardOutputEncoding`，.NET Framework 下默认取控制台代码页（本机 936）→ 子进程输出的 UTF-8 中文（`serverInfo.title`）被解坏 → `ConvertFrom-Json` 直接失败。

**这是脚本缺陷，不是产品缺陷**（MCP 的 JSON-RPC 输出本身是正确的 UTF-8）。

**修复**：显式指定编码（BOM 已确认保留）。

```powershell
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
```

**修复后实测（全通过 / exit 0）**：真拉子进程走管道，`initialize` 返回 `serverInfo`（中文 UTF-8 无乱码）、协商协议版本 `2025-06-18`、`tools/list` 12 个工具且都有 `inputSchema`、`ping` 空结果、`cursor_position` 返回坐标、`screenshot` 返回 `image/jpeg` base64 且附尺寸说明、未知工具 `isError`、未知方法 `-32601`、非法 JSON `-32700`。

> 附带收益：这条同时是**产品壳的端到端验收**——`QuickScriptTool.exe` 作为子进程被真拉起，走真管道。

---

## 五、验收结论：声称 vs 实测

| # | 声称 | 实测 | 判定 |
|---|------|------|------|
| #6 CI | ✅ | 文件与依赖齐备，YAML 合法；**未在真实 runner 跑过**；编 18 跑 15 | ✅ 属实（2 偏差） |
| #13 去重 | ✅ | 三份 Base64 + JSON 扫描确已收敛、语义等价；**但引入 saveSettings P0 回归**（已修）；另有 2 处 Base64 漏网 | ⚠ 属实但引入回归 |
| #14 卫生 | ✅ | 6 个 obj / query / 49 文件取消追踪 / archive 全部落实 | ✅ 属实 |
| #15 dist 策略 | ✅ | 脚本真机跑通（补上原 §5.1 未验证项）：70 文件 / 15.23 GB，排序与别名保护正确 | ✅ 属实（超出预期） |
| #16 文档 | 🟡 部分 | 注释已改 44 种 + 补四处同步清单；分册未做（自述已承认） | ✅ 自述准确 |
| #9 构建去重 | ✅ | 工程文件权威确认 `utils.cpp` 4→1；两个 SelfTest 改链引擎且用例数未缩水 | ✅ 属实（2 项未做完） |

**自述可信度**：641 用例这个数字实测复现（580 + 61），一个不差。`refactor-progress.md` §5 自述的 4 项遗留风险**全部属实**，且 §3 记录的 4 个踩坑（引擎隐式符号依赖 / BOM / MSBuild 代理 / OBJECT 库不传递）经复核全部准确。这份记录的诚实度是本次验收里最值得肯定的部分。

---

## 六、遗留清单（按处理顺序）

> **第 2 轮补记**：A1 / A2 / A3 / B 段已闭合；A4 / A5 / A6 / A7 / A8 / A10 见第 2 轮验收报告。

| # | 项 | 类型 | 位置 | 说明 |
|---|----|------|------|------|
| A1 | **saveSettings 回归无测试保护** | P0 | 新增测试 | 本次 P0 已修但无回归用例；下次同类改动会再犯 |
| A2 | 解析失败静默 | P0 | `src/json_util.h` | `TryParse` 失败时所有键静默返回 false，正是本次事故难发现的根本原因；应至少打一条诊断 |
| A3 | `fetch_webpage_link_stub.cpp` 死文件 | P2 | `tools/` | CMake 只剩注释引用，可删 |
| A4 | Base64 仍剩 2 份 | P2 | `qst_webview_shell.cpp:4042`、`ext_bridge_server.cpp:111` | 可并入 `src/base64.h` |
| A5 | `window_mode_types.cpp` / `window_mode_json.cpp` 重复编译 | P2 | `CMakeLists.txt` | 分别 2 份 / 3 份，P1-8 未做完 |
| A6 | CI 编 18 跑 15 | P2 | `.github/workflows/build.yml` | 3 个 target 只编不跑 |
| A7 | `build/` 陈旧 `.obj` | P2 | `build/` | 自述 §5.3 已承认；`--target clean` 或重建目录 |
| A8 | 元技能未同步新入口 | P2 | `.cursor/skills/module-selftest/SKILL.md` | AGENTS.md 称其为「必读入口」，却未提 `run_all_selftests.ps1`（417 行仍只讲单 suite 手工流程） |
| A9 | `.research/word-reflow-test.ps1` 缺 BOM | P3 | `.research/` | 该目录已 gitignore，影响可忽略 |
| A10 | `dist/setup-1.0.5-test.log` 等散落文件 | P3 | `dist/` | 不属于任何系列，prune 不处理 |

---

## 七、下一步重构方向

### 7.1 先修正一个认知：本轮暴露的是「边界行为无测试」

本轮两个"安全重构"都改变了行为，且都没有测试能看见：

- **#13**：换 JSON 解析器 → 解析**严格度**变了（旧的手写扫描容忍残破 JSON）
- **#6**：加冒烟脚本 → 才发现它自己一直因编码问题失败

两次都不是"写错了代码"，而是**改了边界契约却没人守**。所以下一步的第一优先级不是继续拆大文件，而是**把边界行为固化**。

### 7.2 建议顺序（在原 `#8 → #4 → #1 → #3 → #2` 基础上做三处调整）

```
A. 立即（半天，闭合本次事故）
   A1  把 saveSettings 回归固化成测试（见 7.3 规格）
   A2  json_util.h 加解析失败诊断

B. #8 的前置（1–2 天，消掉链接 hack）
   B1  把 engine → shell 的 5 个符号倒置为回调注入
       （PostToWebUi / HotkeyLogLine / NotifyWebDebugWindowSetting /
         SyncHomeSelectionCache / g_instance）
   B2  删掉 tools/engine_link_stubs.cpp
   B3  再写 ScriptRunnerSelfTest（先覆盖 Wait/Loop/Goto/VarCompute）

C. #10 桥接契约 —— 优先级上调到 #4 之前
   理由：本次 P0 就长在这条缝里。39 个 type 字符串 → 命令表 +
   请求/响应结构 + BridgeContractSelfTest（校验 C++ 命令表与 bridge.js 一致）

D. #4 统一序列化 + schema 版本
   硬要求：必须用**真实历史脚本**做往返验证（dist/ 与用户库），
   而不是只跑 ScriptIoSelfTest —— 本次事故的教训是"用例全绿≠没坏"

E. #1 ActionHandler 注册表 → #3 灭 gh* → #2 拆 EngineHost → 其余
```

**三处调整及理由**：

| 调整 | 原方案 | 建议 | 理由 |
|------|--------|------|------|
| ① 加 A 段 | 无 | 立即做 A1/A2 | 已知 P0 无回归保护；静默失败必须变可观测 |
| ② `#10` 提前 | `#8 → #4 → #1 …` | `#8 → #10 → #4 → #1 …` | P0 落在桥接缝上，先建契约测试再改序列化，避免第二次"静默行为变更" |
| ③ `#8` 拆成两步 | 直接写 `ScriptRunnerSelfTest` | 先倒置依赖、删 stub，再写测试 | `engine_link_stubs.cpp` 是链接期空实现，测试跑在它上面是"假绿"；5 个符号的倒置是**小工作量**（远小于 #1/#2），先做收益最大 |

### 7.3 A1 的测试规格（可直接落地）

**目标**：锁住「JS 消息形状 ↔ C++ 解析严格度」这条缝。

**为什么不能直接测 `ApplySaveSettingsJson`**：它末尾无条件 `SaveAppSettings(s)`，会写真实设置文件，污染用户环境。

**推荐做法**——测"取子对象"这一步（即本次修的那一步），把逻辑上收到 `json_util.h`：

```cpp
// src/json_util.h 新增
/// 取顶层对象字段并还原成 JSON 文本（供只接受子对象 JSON 的旧接口使用）。
/// 必须用严格解析取**配对**的对象；早期实现用 substr(首个 '{') 到末尾，
/// 会带上外层消息的尾随 '}' → 非法 JSON → 调用方静默拿到全默认值。
inline bool GetSubObjectText(const std::string& json, const char* key, std::string& out);
```

然后新增 `BridgeJsonSelfTest`（链 `qst_engine`），断言：

1. `{"type":"saveSettings","settings":{"themeId":7}}` → `GetSubObjectText` 得到可严格解析的对象，`GetInt(...,"themeId")==7`
2. `settings` 不是最后一个键（`{"settings":{...},"x":1}`）→ 仍正确
3. `settings` 的值里含字符串 `"}"`、嵌套对象 → 仍正确
4. `settings` 缺失 / 为 `null` → 返回 false，调用方回落整条消息
5. 非法 JSON → 返回 false 且**不抛异常**

这样既修掉 `substr` 这个脆弱写法，又把回归钉死在测试里。

### 7.4 不建议现在做的事

- **不要**因为这次 P0 就回退到手写字符扫描 —— 严格解析是对的，错的是调用方构造了非法 JSON。
- **不要**在 `json_util.h` 里加"宽容模式"开关 —— 那会让"残破 JSON 静默取到半个值"重新变成默认行为。
- **不要**跳过 B 段直接进 #1 —— `#1` 要抽 `ActionContext`，而它需要能单测；在 stub 上写的测试无法验证 `ActionContext` 的真实行为。

---

## 八、验收操作留痕

| 操作 | 命令 / 方式 | 结果 |
|------|-------------|------|
| 构建产品壳 | `cmake --build build --config Release --target QstWebViewShell` | exit 0 |
| 15 个 logic suite | `build\Release\<Target>.exe --json`（逐套跑，按 exit code 判定） | 641 用例 / 0 失败 |
| 交互类 suite | `WindowModeSelfTest` / `VirtualHidSelfTest` / `InjectionSelfTest` | 71 pass/exit 0；9+1 fail（无驱动，符合预期）；12+1 fail（exe 为 00:48 旧产物，本轮未重建） |
| MCP 端到端 | `tools\test_mcp_server.ps1` | 修复编码后全通过 / exit 0 |
| dist 策略 | `tools\prune_dist.ps1 -DryRun` | 跑通，70 文件 / 15.23 GB 待归档 |
| 工程文件权威核对 | `grep -E 'Include="[^"]*[\\/]utils\.cpp"' build/*.vcxproj` | 仅 `qst_utils.vcxproj` 1 处 |
| 语义探针 | `cl` 编译探针 + 真实 `json_util.h` + 真实 `utils.cpp` | 复现回归并验证修复 |
| YAML 校验 | PyYAML `safe_load` | 语法合法（`on:` 被 YAML 1.1 解为 bool，GitHub Actions 不受影响） |
