# 重构验收报告（第 3 轮：发版工具链 + FakeFocus32 + A4/A5 收尾）

> 验收日期：2026-09-19
> 验收对象：本轮实际改动（`release.cmd`、`tools/package_with_version.ps1`、`src/window_mode/fake_focus/*`、
> `src/window_mode/window_mode_executor.cpp`、`window_mode_requirements.h`、版本号三处）
> 前序：`docs/refactor-acceptance.md`（第 1 轮）、`docs/refactor-acceptance-round2.md`（第 2 轮）
> 方法：逐项实机复跑；回归用 18 个 logic suite；产品路径用 MCP 端到端冒烟

---

## 一、总评

| 维度 | 结论 |
|------|------|
| 本轮实际内容 | **不是重构轮**，而是 ①**发版工具链**（`release.cmd` + `package_with_version.ps1` + 版本号）②**FakeFocus32 构建修复** ③window_mode 后台输入调整 |
| 发版工具链 | ✅ 质量高：DryRun 实跑通过、版本号三处一致、正则只替换一处且保留行尾、失败回滚、工具链探测正确、`release.cmd` 纯 ASCII 且 `cd /d "%~dp0"` |
| FakeFocus32 修复 | ✅ **真修好了**：实跑打印 `[FakeFocus32] OK:`（此前是 `vcvarsall.bat not found - skip 32-bit DLL`），PE 架构校验 32 位 = `0x014c` |
| window_mode 改动 | ✅ 代码与技能文档同步完整；但 reference.md 自述「诊断全零」仍未收敛（诚实） |
| **两份重构文档** | ❌ **本轮完全未更新**（仍停在 09-18 23:41），且 §5.1/§5.4/§七 等多处陈述已过时 |
| 本轮我闭合 | A4（残留 Base64 清零）、A5（`window_mode_common` STATIC 库，消除 1,246 行重复编译） |
| **验收新发现** | **1 项 P1（窗口模式自检 40% 抖动）** + 1 项 P1（文档未同步）+ 2 项 P2 |

**一句话**：这一轮的**工程质量是好的**（发版脚本写得比多数商业项目的发版脚本讲究），但**项目记忆断档了**——重构的两份权威文档没跟上，且里面已有过时陈述。文档不同步的代价在下一轮就会显现：接手者会按过时的 §七 去做已经做完的 C 段。

---

## 二、发版工具链验收

### 2.1 `tools/package_with_version.ps1`（新增，481 行）

| 检查点 | 实测 |
|--------|------|
| 版本号三处一致性 | ✅ `product_version.txt`=`1.3.3`、`.iss #define MyAppVersion "1.3.3"`、`app_branding.cpp L"v1.3.3"` 三处一致 |
| 正则是否真能匹配（**独立复核**） | ✅ 用 PowerShell 单独跑两个正则：`(#define\s+MyAppVersion\s+")[^"]*(")` 与 `(AppBranding::version_\s*=\s*L"v?)[^"]*(")` 均 `IsMatch=True`；替换后 **各只出现 1 次** `9.9.9`，行尾 `\n` 保留 |
| `-DryRun` 实跑 | ✅ exit 0；输出计划完整（写三处 → cmake 配置 + `--target QstWebViewShell --target QstUninstall` → `package_release.ps1 -SkipBuild` → ISCC → 再同步） |
| DryRun 是否真无副作用 | ✅ 跑完三处版本号仍为 `1.3.3` |
| 构建目标不写死 | ✅ 从 `package_release.ps1` 正则解析 `--target`，实测解析出 `QstWebViewShell, QstUninstall`（与该脚本里的两处 `--target` 完全一致） |
| 工具链探测 | ✅ 实跑到 `C:\Program Files (x86)\...\BuildTools\...\cmake.exe` 与 `%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe`（本机实际安装位置） |
| 失败回滚 | ✅ 有 `$backups` + `catch` 回滚，`-KeepVersionOnFailure` 可保留现场 |
| 受限宿主自适应 | ✅ 检测 `CODEBUDDY_SAFE_DELETE_BULK_GUARD` 自动加载兼容层（本会话实测加载了 `build\qst_pack_compat.ps1`）；并**提前探测**外部程序能否启动，避免"构建阶段才以配置失败告终" |
| 输出策略 | ✅ 子命令输出全量进 `build\release-<版本>.log`，控制台只放行关键行（cmake/ISCC 会刷几千行，不过滤看不到结果）—— 这是踩过坑才会有的设计 |
| 编码 | ✅ 含中文，文件头声明必须 UTF-8 with BOM |

### 2.2 `release.cmd`（新增，49 行）

| 检查点 | 实测 |
|--------|------|
| 纯 ASCII | ✅ 无中文字节（避免 GB2312 宿主读坏 .cmd） |
| 任意目录可调用 | ✅ `cd /d "%~dp0"` 在解析参数前执行 |
| 双击保持窗口 | ✅ 用 `echo %cmdcmdline% \| find /i "%~nx0"` 检测，仅双击时 `pause` |
| 退出码透传 | ✅ `exit /b %RC%` |
| 无参数时提示输入 | ✅ `set /p` 兜底 |

> 小结：**发版链路可以放心用**。唯一未验证的是完整跑一次（需 2–4 分钟构建 + 2–3 分钟 ISCC），本轮只做了 DryRun。

---

## 三、FakeFocus32 构建修复验收

这是本轮**最有实际价值**的修复（此前 32 位 DLL 被静默跳过，任何 DLL 侧修复都不生效）。

| 检查点 | 实测 |
|--------|------|
| 是否修掉括号打断 `if` 的老坑 | ✅ `build_fakefocus32.cmd:16` 改成先 `set "PF86=%ProgramFiles(x86)%"` 再 `if not defined PF86`，并写明「Do NOT test `if defined ProgramFiles(x86)` directly」 |
| vswhere 全路径兜底 | ✅ `:24-30` 用 vswhere `-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`，并校验 `vcvarsall.bat` 存在 |
| 已知路径回退链 | ✅ `:33-37` 覆盖 Community/Professional/Enterprise（64 位 PF）+ BuildTools/Community（32 位 PF86） |
| 只许 ASCII | ✅ 文件无中文字节 |
| **实跑是否打印 OK** | ✅ `cmake --build build --config Release --target FakeFocus32` → **`[FakeFocus32] OK: D:/other/software/build/Release\FakeFocus32.dll`**（修复前是 `[FakeFocus32] vcvarsall.bat not found - skip 32-bit DLL`） |
| **产物是否真是 32 位** | ✅ PE `Machine=0x014c`（i386）；`FakeFocus64.dll` = `0x8664`（x64）—— 证明不是"打印了 OK 但产物不对" |
| 陈旧实验 DLL 是否会被打进包 | ✅ 打包脚本按**文件名精确拷贝**（`package_release.ps1:292-293` 显式列 `FakeFocus64.dll` / `FakeFocus32.dll`），`build\Release` 下的 `FakeFocus32.next/safe/v3/v4.dll` 不会被带上 |

---

## 四、window_mode 改动验收

| 改动 | 代码位置 | 验收 |
|------|----------|------|
| `WakeMapleStoryInputPolling()` 真激活唤醒 | `window_mode_executor.cpp` | ✅ 存在；调用点 `:899-903` 有双重守卫（`UsesBackgroundWindow() && fakeFocus_.IsInjected() && LooksLikeMapleStoryTarget(...)`）——不会误伤普通窗口 |
| `TargetOwnsForegroundWindow()` 方向键兜底守卫 | `background_window_input.cpp/.h` | ✅ 存在；自检覆盖（`window_mode_selftest.cpp:2689-2692` 断言 `TargetOwnsForegroundWindow(fg)` 为真、`nullptr` 与后台 hwnd 为假） |
| `MaplePollHitSummary()` 诊断解码 | `window_mode_executor.cpp` | ✅ 存在 |
| 技能文档同步 | `.cursor/skills/window-mode-debug/{SKILL.md,reference.md}` | ✅ 完整：新增 `lca_nav_key_leaks_to_foreground` 用例条目、`pollHit=` 诊断契约、`build_fakefocus32.cmd` 构建前提、`WakeMapleStoryInputPolling` 根因分析 |
| 未收敛项 | reference.md 自述 | ✅ **诚实标注**「游戏已在前台、诊断却全零」为「第二轮，仍未收敛」，并明确「不要再盲目补钩子，先看 `pollHit=`」 |

---

## 五、本轮我闭合的上轮遗留项

### A4 —— 残留 Base64 收敛（评估表 P1-2 的漏网部分）

上轮验收发现 Base64 实际有 **5** 份、第 1 轮只收敛了 3 份。本轮补上剩余 2 处：

| 位置 | 处理 |
|------|------|
| `src/webview/qst_webview_shell.cpp:4053`（`readImageDataUrl` 的内联编码循环） | 删循环，改 `qst::base64::Encode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size())`；加 `#include "base64.h"` |
| `src/window_mode/ext_bridge/ext_bridge_server.cpp:110`（`Base64Encode`，给 WebSocket `Sec-WebSocket-Accept` 用） | 删函数，调用点改 `qst::base64::Encode(dig, sizeof(dig))`；加 `#include "base64.h"` |

**验证**：`grep -rn "ABCDEFGHIJKLMNOPQRSTUVWXYZabc...0123456789+/" src/` 现在**只命中 `src/base64.h`** —— 自研 Base64 清零。两处原实现与新实现位运算/填充分支逐行等价。

### A5 —— 消除 `window_mode_types/json.cpp` 重复编译

| 项 | 前 | 后 |
|----|----|----|
| `window_mode_types.cpp`（818 行） | 2 个 target（`window_mode_core` + `script_action_builder_core`） | **1**（`window_mode_common`） |
| `window_mode_json.cpp`（214 行） | 3 个 target（`qst_engine` + `script_action_builder_core` + `WindowModeSelfTest`） | **1**（`window_mode_common`） |
| 重复编译量 | — | **消除约 1,246 行** |

**做法**：新增 `add_library(window_mode_common STATIC src/window_mode/window_mode_types.cpp src/window_mode/window_mode_json.cpp)`，四个 target 改为链接它。**必须先确认这两个文件是纯函数**——实测只有匿名 namespace，无全局可变状态、无静态注册，所以不会像有静态注册的源那样被 STATIC 库裁剪掉初始化（这是 `qst_utils` 那条经验的延伸）。

**权威核对（生成的 vcxproj，不采信磁盘 `.obj`）**：`window_mode_types.cpp` / `window_mode_json.cpp` 现在**只出现在 `window_mode_common.vcxproj`**。（`QuickScriptTool.vcxproj` 里也有一份，但该文件是 **09-18 13:02 的陈旧工程**，不在当前配置的构建图里。）

### 本轮回归结果

| 项 | 结果 |
|----|------|
| 全量构建（`cmake --build build --config Release`） | ✅ exit 0，无 error |
| logic 档 18 个 suite | ✅ **665 用例 / 0 失败** |
| MCP 端到端冒烟（我改了产品路径两个文件） | ✅ **26 项全通过 / exit 0** |

---

## 六、验收新发现的问题

### F1（P1）两份重构文档本轮完全未更新，且多处陈述已过时

`docs/refactor-progress.md` 与 `docs/refactor-changes-summary.md` 的最后修改时间都是 **09-18 23:41**，本轮所有改动（发版工具链 + FakeFocus32 + window_mode）**一字未提**（`grep "package_with_version\|release.cmd"` = 0 命中）。

已过时的具体陈述：

| 位置 | 文档原文 | 实测 |
|------|----------|------|
| `refactor-progress.md` §5.1 | 「`run_all_selftests.ps1` 与两个打包脚本**未能实机执行**——编写环境的脚本执行被拦截」 | ❌ 第 2 轮验收已实跑通过（18/18）；根因是宿主 `PATHEXT` 缺 `.EXE`，非脚本缺陷 |
| `refactor-progress.md` §5.4 | 「`dist\` 仍有 **21 GB**：本轮只提供策略，**未执行清理**」 | ❌ 现为 **5.2 GB / 503 文件**。已按保留策略清到每系列约 5 版，且 `prune_dist.ps1 -PurgeArchive` **确已实跑**（归档 70 文件 / 15.23 GB → 回收后 dist 顶层 5.12 GB；`dist\archive` 现在不存在是因为 `-PurgeArchive` 会删掉该目录） |
| `refactor-progress.md` §5.5 | 「49 个文件与 6 个 `.obj` 只在工作区暂存」 | ⚠ 现为 **50 删除 + 32 修改 + 19 未跟踪**，规模已变 |
| `refactor-progress.md` §七「下一步」 | 「C 段（#10 桥接契约正式化）」 | ❌ C 段已在本文件 §八 记录完成 |
| `refactor-progress.md` §5.3 | 「`build\` 仍有历史陈旧 `.obj`（如 `QuickScriptTool.dir\Release\utils.obj`）」 | ⚠ 仍属实，但 `QuickScriptTool.vcxproj` 整个是陈旧工程，影响面比文档描述的大 |

**风险**：接手者按 §七 会去做已完成的 C 段；按 §5.1 会以为自检入口不可用；按 §5.4 会去"清理"一个已经清干净的目录。**文档是这类多轮重构的唯一记忆，断档的代价大于任何单个代码缺陷。**

### F2（P1）`WindowModeSelfTest / background_minimized_quiet_restore` 抖动 40%

| 项 | 数据 |
|----|------|
| 现象 | 同一二进制反复跑，`{"passed":70,"failed":1}` 与 `{"passed":71,"failed":0}` 交替 |
| 抖动率 | **10 次 4 失败（40%）**；另一次 5 次 3 失败 |
| 失败用例 | 恒为 `background_minimized_quiet_restore`（断言「后台绑定/点击不抢前台」） |
| 失败详情 | 全部含 `click stole FG`；其中一半还含 `bind stole FG` |
| 影响 | 该 suite 属 `full`（交互）档，**当前不 gate CI**（CI 只跑 logic 档）；但一旦启用 `-Tier full`，CI 会随机变红 |
| 根因判断 | 测试用**固定 sleep**（`BeginRun` 后 80ms、点击后 50ms、`EndRun` 后 50ms）读 `GetForegroundWindow()`，而前台切换是异步的 —— 属**测试时序脆弱**，未必是产品缺陷 |
| 无法定论的部分 | 全部改动未提交、无基线二进制，**无法判定是否本轮引入**（本轮 `fake_focus_dll.cpp` 改动使 DLL 首次真正重建，理论上可能影响注入路径；但该用例的探针类名 `QuickScriptWmMinProbe` 不匹配冒险岛守卫） |

**建议**：把断言从「固定 sleep 后读一次」改成「**轮询 + 超时**」（例如 500ms 内每 20ms 检查一次 `GetForegroundWindow()==decoy`），既消除抖动又保留语义。**在改好之前不要把 `-Tier full` 接进 CI**。

### F3（P2）发版脚本覆盖不到 4 处硬编码版本号

`package_with_version.ps1` 只写三处（`product_version.txt` / `.iss #define` / `app_branding.cpp`），但全仓还有 4 处会随发版漂移：

| 位置 | 内容 | 说明 |
|------|------|------|
| `installer/QuickScriptTool.iss:4` | `;   1. 确认 tools\product_version.txt 与下方 MyAppVersion 一致（当前 1.3.3）` | **自指的检查行**，每次发版后必然写错 |
| `tools/package_with_version.ps1:44` | `.EXAMPLE` 里的 `-Version 1.3.3 -SkipBuild` | 示例值，漂移影响小 |
| `website/downloads/README.md:7-9` | 列 `QuickScriptTool-1.3.3.exe` 等三个文件名 | 发版后文件名变了 |
| `website/README.md:16-18` | 目录树里同样三个文件名 | 同上 |

这是**同一类问题的第三次出现**（第 1 轮 `script_types.h` 的「28 种」、第 2 轮 suite 计数）。建议：把 `.iss:4` 那行改成不含版本号的措辞（如「确认与 `product_version.txt` 一致」），website 两份 README 的文件名改成 `<版本>` 占位或改为不列举具体版本。

### F4（P2）`-DryRun` 不校验版本号正则

`-DryRun` 的自述是「只做检查（版本号格式 / 工具链 / 构建目标）」，实测**确实**只做这三项——它在 `Set-VersionInFile` 之前就 `exit 0`，所以**不会验证两个正则能否匹配**。若 `.iss` 或 `app_branding.cpp` 的结构变了，要等正式跑（已写入 `product_version.txt` 之后）才失败，靠回滚兜底。

**建议**：DryRun 里对两个 `-Pattern` 做一次 `[regex]::IsMatch` 检查并报告，把"文件结构变了"提前到 DryRun 暴露（成本约 5 行）。

---

## 七、下一步方向

### 7.1 先做文档与提交（阻塞项，半天）

1. **补两份重构文档的本轮记录**（F1）：发版工具链、FakeFocus32 修复、window_mode 调整、A4/A5 闭合
2. **修正 5 处过时陈述**（F1 表格）：§5.1 改「已实跑，宿主 `PATHEXT` 是误判根因」；§5.4 改「现 5.2 GB，且不是 prune_dist 清的」；§5.5 更新规模；§七「下一步」删掉已完成的 C 段
3. **修 F3 的 4 处硬编码版本号**（尤其 `.iss:4` 的自指检查行）
4. **提交**：建议分 3 个提交 —— ①阶段 0 + #9 ②A/B/C 段 ③发版工具链 + FakeFocus32 + A4/A5。当前工作区 **50 删除 + 32 修改 + 19 未跟踪**，再不分批就难以回溯了
5. 提交后**盯首次 CI**（OpenCV 首次下载 ~250MB、`FakeFocus32` 的 x86 工具链）——这两项至今未在真实 runner 验证过

### 7.2 修 F2（半小时，解锁 `-Tier full`）

把 `background_minimized_quiet_restore` 的三个固定 sleep 改成轮询 + 超时。修好后 `WindowModeSelfTest` 才能作为 CI 门槛——它是本项目唯一能端到端验证「后台窗口模式不抢前台」的用例，价值高。

### 7.3 然后进 D 段（#4 统一序列化 + schema 版本）—— 本轮仍是最优先的重构项

三轮下来 D 段一直没动，而它是**评估表里唯一同时满足「高价值 + 独立可做 + 不依赖 #1/#2」的 L 级项**。

**硬要求（第 1 轮 §7.2 已定，第 2、3 轮重申）**：必须用**真实历史脚本**做往返验证（`dist\` 与用户脚本库），不能只跑 `ScriptIoSelfTest`——三轮里已经两次证明「用例全绿 ≠ 没坏」。

| 步 | 动作 | 验收口径 |
|----|------|----------|
| D1 | 建**往返基线**：扫描 `dist\` 与用户库的真实 `.qst` 脚本，记录三套序列化路径各自的 读→写→再读 结果 | 基线用例数 + 差异清单 |
| D2 | 以 nlohmann 为唯一实现，三处入口（`script_io.cpp` / `script_action_builder.cpp` / `webview_bridge_backend.cpp`）转发 | `ScriptIoSelfTest` 58 条不退；D1 基线零差异 |
| D3 | 加 `"v"` 字段 + `MigrateAction(json, fromVer)` | 旧脚本（无 `v`）按 v1 迁移，往返仍零差异 |
| D4 | 新增 `ScriptSerializationSelfTest`，含**事故形状回归** | **变异测试**：把某字段映射改错 → 必须变红 |

### 7.4 可并行的低风险项

| 项 | 说明 |
|----|------|
| `prune_dist.ps1` | ✅ **已实跑**（`-PurgeArchive`，归档 70 文件 / 15.23 GB 后回收，dist 顶层 5.12 GB）。第 2 轮验收的 `-DryRun` 清单与本次实际归档量完全吻合，策略按预期工作 |
| 残留陈旧工程 | `build\QuickScriptTool.vcxproj`（09-18 13:02）等陈旧 `.vcxproj` 会误导"权威核对"（本轮就差点被它干扰）；建议 `--target clean` 或重建 `build\` |
| #7 统一 HTTP 抽象 | 独立可做（`IHttpClient` + `MockHttpClient`） |
| #10 类型化余项 | 命令表已建，可补 `enum class BridgeCommand` + 请求/响应结构体 |
| #16 文档分册 | `ai-action-exec-optimization.md`（2,056 行） |

### 7.5 明确不建议现在做的

- **不要**为了消除 F2 抖动而放宽断言（把「不抢前台」改成「大概不抢」）——那是把真问题藏起来。抖动在测试侧，改测试的**等待方式**而不是**判定标准**。
- **不要**在 D 段之前动 #1（`ActionHandler`）——序列化格式稳定后 `ActionContext` 的字段映射才有意义，顺序反了要做两遍。
- **不要**把 A5 的手法（STATIC 库）无差别套到所有重复编译上——`window_mode_*` 能这么做是因为**实测确认是纯函数**；有静态注册的源放进 STATIC 库可能被链接器裁掉初始化，是静默故障。
- **不要**因为 F1 就重写文档体系——补齐 + 修正过时陈述即可；真正的解法是**让文档里的数字不写死**（指向单一真值），这条已经在第 2 轮对 suite 计数做过。

---

## 八、验收操作留痕

| 操作 | 方式 | 结果 |
|------|------|------|
| 发版 DryRun | `package_with_version.ps1 -Version 1.3.4 -DryRun` | exit 0；计划完整；三处版本号未变 |
| 版本号正则独立复核 | PowerShell `[regex]::IsMatch` + `Replace` | 两条正则均匹配；各只替换 1 处；行尾保留 |
| FakeFocus32 构建 | `cmake --build build --config Release --target FakeFocus32` | `[FakeFocus32] OK:` |
| PE 架构校验 | 读 PE 头 Machine 字段 | 32 位 `0x014c` / 64 位 `0x8664` |
| A5 去重核对 | 生成的 `.vcxproj`（不采信磁盘 `.obj`） | `types` 2→1、`json` 3→1 |
| 全量构建 | `cmake --build build --config Release` | exit 0，无 error |
| logic 档回归 | 18 个 suite `--json`，按 exit code 判定 | **665 用例 / 0 失败** |
| 窗口模式自检 | `WindowModeSelfTest --json` × 15 次 | **抖动 40%**（F2） |
| MCP 端到端 | `tools/test_mcp_server.ps1` | 26 项全通过 / exit 0 |
| 自研 Base64 清零 | `grep -rn "<Base64 字母表>" src/` | 仅命中 `src/base64.h` |
