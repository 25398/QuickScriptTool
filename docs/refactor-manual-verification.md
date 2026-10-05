# 重构人工验证手册

> 用途：把四轮重构（阶段 0/#9 · A/B/C 段 · D 段 · 发版工具链）的成果**逐项手工验证**一遍。
> 每项给：**怎么验**（可直接复制的命令或 UI 操作）+ **期望结果** + **失败意味着什么**。
> 相关：`docs/architecture-review.md`（评估）· `refactor-acceptance*.md`（三轮验收）· `refactor-progress.md`（实施记录）

---

## 0. 准备

### 0.1 构建（全量）

```bash
cd /d/other/software
unset https_proxy http_proxy HTTPS_PROXY HTTP_PROXY      # 否则 MSBuild 崩 MSB6001
"/c/Program Files/CMake/bin/cmake.exe" --build build --config Release -j 16
```

**期望**：`exit 0`，**零 error、零 warning**，38 个目标全部产出（产品壳 `build/Release/QuickScriptTool.exe` + 19 个 logic suite + 3 个交互 suite + 诊断/注入工具 + 卸载程序）。

### 0.2 一键自检（CI 同款口径）

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -SkipBuild -Tier logic
```

**期望**：`19 个 suite，19 通过，0 失败`，`exit 0`。

> ⚠ **本机跑不起来的唯一已知原因**：`$env:PATHEXT` 被削成只剩 `.CPL` 时，PowerShell 命令发现不认 `.EXE`，会对每个 suite 报
> 「无法在管道中间运行文档」。先设一次即可：
> ```powershell
> $env:PATHEXT = '.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.CPL'
> ```
> 这不是脚本缺陷，也不是沙箱限制。

### 0.3 逐套跑（排查单套时用）

```bash
cd build/Release
./<Suite>.exe --json          # 每行一条用例；末行 {"passed":N,"failed":M,"ok":...}
echo $?                       # exit code = 失败用例数（0 = 全过）
./<Suite>.exe --list          # 只列用例名，不跑
```

---

## 1. 阶段 0（零风险项）

### 1.1 #13 重复实现已收敛（Base64 / JSON 手写扫描）

```bash
# Base64 字母表在整个 src/ 里应只出现一次
grep -rn "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" src/
# 手写 JSON 扫描函数应已不存在
grep -rn "JsonGetStringField\|JsonGetStringArray\|ExtractJsonArrayFromText" src/ | grep -v json_util.h
```

**期望**：第一条**只命中 `src/base64.h`**（原 5 份 → 1 份）；第二条**无输出**。
**失败意味着**：又有人新写了一份 Base64/JSON 扫描（应改为 `using qst::base64::Encode` / `qst::json_util`）。

### 1.2 #14 仓库卫生

```bash
ls *.obj query archive 2>/dev/null          # 期望：全部 "No such file"
git ls-files .research/                      # 期望：只有 office-document-io-report.md
git check-ignore -v query .research/foo.md   # 期望：被 .gitignore 命中
```

### 1.3 #15 dist 保留策略（**有破坏性，先 DryRun**）

```powershell
powershell -ExecutionPolicy Bypass -File tools\prune_dist.ps1 -DryRun
```

**期望**：打印每个产物系列「保留 5 个，归档 N 个」+ 待归档文件清单 + `dist\ 当前总占用`；**不改任何文件**。
确认无误后再决定是否加 `-PurgeArchive`（会真删 `dist\archive\`）。

### 1.4 #16 文档数字不再漂移

```bash
grep -n "共 44 种动作类型" src/script_types.h          # 期望命中
grep -nE "[0-9]+ 个 suite" AGENTS.md                   # 期望：不写死（指向 $LogicSuites）
```

---

## 2. #9 构建去重（**必须用生成的 vcxproj 判定，不能数磁盘上的 .obj**）

```bash
cd /d/other/software
# 权威口径：谁在「编译」这个源
for f in utils window_mode_types window_mode_json; do
  echo "--- $f.cpp ---"
  for p in build/*.vcxproj; do
    n=$(grep -c "$f\.cpp" "$p" 2>/dev/null); [ "$n" -gt 0 ] && echo "  $(basename $p)"
  done
done
```

**期望**：
| 源 | 期望编译它的工程 |
|----|------------------|
| `utils.cpp` | 只有 `qst_utils.vcxproj` |
| `window_mode_types.cpp` | 只有 `window_mode_common.vcxproj` |
| `window_mode_json.cpp` | 只有 `window_mode_common.vcxproj` |

**注意**：`build/` 下可能残留**陈旧工程**（如 `QuickScriptTool.vcxproj`，mtime 远早于本次配置）。判定时先看该 vcxproj 的时间戳；不确定就 `cmake -S . -B build` 重新生成后再看。

**为什么不能数 `utils.obj`**：中间构建会留下旧的 `.obj`，数量对不上会误判（第 3 轮验收差点因此误报 A5 未生效）。

**再验一条**：两个 SelfTest 改链引擎后用例数不得缩水 —— `AiActionRouterSelfTest` 171、`AgentAssistantSelfTest` 61。

---

## 3. A 段 —— saveSettings 事故（P0）的回归锁

### 3.1 自动化：`BridgeJsonSelfTest`（9 条）

```bash
./build/Release/BridgeJsonSelfTest.exe --json
```

**期望**：`{"passed":9,"failed":0,"ok":true}`，且含用例 `regression_trailing_outer_brace`（本次事故形状）。

### 3.2 **变异测试（关键）**：证明这个锁不是摆设

把 `src/json_util.h` 的 `GetSubObjectText` 改成事故前的写法（`substr(首个 '{')` 到末尾），重建 `BridgeJsonSelfTest`。

**期望**：**7 条变红**（`subobject_*` 6 条 + `regression_trailing_outer_brace`），exit=7。还原后 9/9 绿。
**失败意味着**：回归锁失效 —— 同类事故可以再次静默发生。

### 3.3 **端到端手工验证（唯一能验证真实产品行为的）**

1. 启动 `build\Release\QuickScriptTool.exe`
2. 打开 **设置**，改一个显眼的项（例如「界面缩放倍率」或「开机自启」）
3. 点保存 → 应提示成功
4. **关掉程序，重新打开，再看设置页**

**期望**：改的值**还在**。
**失败意味着**：正是本次 P0 的症状 —— 「提示保存成功、实际写回旧值」。此时看 `build\Release\webview_boot.log`：
```
JSON: 解析失败 where=saveSettings.payload …
BRIDGE: saveSettings 缺少 settings 子对象，回落整条消息（本次保存很可能不生效）
```
A2 的诊断就是为这条设计的：**让静默失败留下痕迹**。

---

## 4. B 段 —— 引擎脱离壳可独立链接（依赖倒置）

### 4.1 结构验证（不需要跑）

```bash
# 引擎侧不应再 include 壳的头文件（只允许注释里出现）
grep -rn "webview_bridge_backend.h" src/engine/

# 4 个 UI 钩子函数应只在引擎侧定义
grep -rn "^void PostToWebUi\|^void HotkeyLogLine\|^void NotifyWebDebugWindowSetting\|^void SyncHomeSelectionCache" src/

# g_instance 应只有一个定义
grep -rn "^HINSTANCE g_instance" src/
```

**期望**：第一条**只命中注释**；第二条只命中 `src/engine/engine_ui_hooks.cpp`；第三条只命中 `src/app_instance.cpp`。

### 4.2 链接验证（**这是"引擎可独立链接"的硬证据**）

```bash
grep -n "ScriptRunnerSelfTest" -A 10 CMakeLists.txt
```

**期望**：`LIBS` 里是 `qst_engine qst_desktop_tools qst_engine_ui_stubs Qst::WebView2 ...` —— **没有任何链接桩文件、没有壳的源文件**。

```bash
./build/Release/ScriptRunnerSelfTest.exe --json        # 7 条（UI 钩子契约，CI 安全）
./build/Release/ScriptRunnerSelfTest.exe --engine --json   # 11 条（+4 条 headless 真跑动作）
```

**期望**：7/7 与 11/11，exit 0。

### 4.3 UI 钩子回归（手工）

依赖倒置把「引擎→壳」的调用改成注入，**壳没装上钩子时会静默 no-op**，所以必须手工确认这几条链路还活着：

| 操作 | 期望 |
|------|------|
| 改一个脚本的启停热键 | 提示保存成功；`webview_boot.log` 有 `HOTKEY:` 行 |
| 脚本运行中，主页状态跟着刷新 | Web UI 状态与引擎一致（`postToWebUi` 通路） |
| 打开宏调试窗再关掉 | 设置里「调试输出窗」勾选状态同步（`notifyWebDebugWindowSetting`） |
| 在主页切到另一个脚本 | 重启后仍停在刚切的那个（`syncHomeSelectionCache`） |

**失败意味着**：`InstallShellUiHooks()` / `InstallBridgeUiHooks()` 没在 `wWinMain` 顶部调用，或注入的成员为空。

---

## 5. C 段 —— 桥接命令契约

### 5.1 自动化：`BridgeContractSelfTest`（8 条，纯源码分析）

```bash
./build/Release/BridgeContractSelfTest.exe --json
```

**期望**：`{"passed":8,"failed":0,"ok":true}`，detail 里能看到 `正式表 106 条 / 例外表 7 条`、`C++ 分派 113 条全部已登记`。

> 该套件读源码文件（`src/webview/bridge_commands.h` + `ui/*.js`），**从拷贝出去的 build 目录单独跑会跳过**（判 ok=true，不假红）。

### 5.2 **变异测试（关键）**

把 `ui/bridge.js` 里 `saveSettings: (settings) => post({ type: "saveSettings", … })` 改成 `"saveSettingsX"`，重跑。

**期望**：**恰好 2 条变红**，双向报错：
```
table_all_sent_by_js : 表里登记了但没有任何 JS 会发：saveSettings
js_sends_all_declared: JS 发了但 C++ 不处理 / 未登记：saveSettingsX
```
还原后 8/8 绿。
**失败意味着**：契约表与实现已漂移，新的桥接命令会静默无响应。

---

## 6. D 段 —— 脚本序列化 / schema 版本

### 6.1 自动化：`ScriptSerializationSelfTest`（11 条内置）

```bash
./build/Release/ScriptSerializationSelfTest.exe --json
```

**期望**：11/11，含 `field_mapping_anchors`（11 种动作类型）、`schema_version_chain`（含 `JSON钩子保真=1`）、`save_is_deterministic`、`roundtrip_invalid_escape_path`。

### 6.2 真实脚本往返（**D 段的核心口径**）

```bash
./build/Release/ScriptSerializationSelfTest.exe --corpus build/Release/scripts --json
```

**期望**：`corpus_roundtrip_clean` 绿，detail 显示「脚本 N 个，零差异 N」。
**唯一允许的系统性差异**：`coordMeta.captureWidth/Height` 会被盖上**本机屏幕尺寸**（设计如此，用于找图模板缩放基准）。它会被单独计数上报，不参与严格比对。
**失败意味着**：真实脚本在 读→写→再读 过程中丢了字段 —— 这是最严重的回归，必须立刻停手排查。

### 6.3 **变异测试（关键，且是 D4 发现的盲点）**

把 `src/script_io.cpp` 里 `MoveMouseRelative` 分支的 `a.x = J.GetNumber(L"x", 0)` 改成读 `L"y"`，重建。

**期望**：`field_mapping_anchors` 变红，诊断形如
```
[5].x（dx 应原值 -17，不参与归一化）
```
**注意**：**只做往返测试抓不到这个变异**（A=parse(content) 与 B=parse(save(A)) 都经过同一个被变异的解析器，错得一样 → 零差异）。所以 `field_mapping_anchors` 这类**锚定断言**是必需的。
**覆盖面规则**：锚定断言的覆盖面 = 它列举的动作类型。**新加动作类型时要同步补一条**，否则那类映射错误只有 `--corpus` 才可能碰到。

### 6.4 schema 版本手工验证

1. 找一个**旧脚本**（文件里没有 `"v"` 字段，例如 `build\Release\scripts\` 里的老文件；可先复制一份改名为 `_v1test.json`）
2. 用产品打开它 → 随便改一下 → 保存
3. 打开该 JSON 文件看顶层

**期望**：出现 `"v": 2`，且**动作内容与保存前一致**（旧脚本迁移不得改动内容）。
**失败意味着**：`MigrateScriptFileData` / `MigrateScriptJson` 在迁移时动了不该动的东西。

### 6.5 JSON 层钩子的位置（架构约定，人工核对）

`script_io.h` 的 `ScriptFileData` 注释里有 4 条「改字段时必须做」的清单。**第 ④ 条是硬约定**：

> 字段**改名/结构变化**必须用 `MigrateScriptJson`（解析**之前**）；值语义变化才用 `MigrateScriptFileData`（模型层）。
> 原因：解析器只认当前字段名，老文件里的旧名字在解析阶段就被丢成默认值，模型层无从恢复。

**怎么验**：若未来新增一个 schema 版本且涉及改名，检查 `LoadScriptFileData` / `ParseScriptContent` 里
`ParseWithJsonMigration` 的调用**在构造解析器之前**，且迁移逻辑写在 `MigrateScriptJson` 里。

---

## 7. 发版工具链

### 7.1 版本号单源化

```bash
grep -n "MyAppVersion" installer/QuickScriptTool.iss | head -3
cat tools/product_version.txt
grep -n "version_ = " src/app_branding.cpp
grep -n "FILEVERSION\|PRODUCTVERSION" resources/QuickScriptTool.rc
```

**期望**：四处**版本号一致**，且 `.iss:4` 那行注释里**不含具体版本号**（曾经每次发版必写错）。

### 7.2 DryRun（不写任何文件）

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_with_version.ps1 -Version 1.3.4 -DryRun
```

**期望**：exit 0；打印 5 步计划；**三处版本号仍为原值**；DryRun 阶段就会校验「文件存在 + 正则可匹配」（结构变了当场报错，而不是写到一半再回滚）。

### 7.3 完整发版（可选，约 6–10 分钟）

```powershell
# 最省事：仓库根的包装器（任意目录可用，纯 ASCII）
release.cmd 1.3.4
```

**期望**：结尾绿色横幅 `发版成功 | QuickScriptTool 1.3.4`；`dist\QuickScriptTool-Release-1.3.4.zip` 与 `QuickScriptTool-1.3.4.exe` 存在；`website\downloads\` 的固定别名已同步；日志在 `build\release-1.3.4.log`（控制台只显示关键行）。
**失败**：红色横幅 + 原因 + 日志末尾 20 行，且**版本号自动回滚**（除非加 `-KeepVersionOnFailure`）。

---

## 8. 通用方法：**变异自检**

四轮下来最重要的一条经验 —— **用例全绿 ≠ 没坏**。所以每个回归锁都应该做一次变异验证：

| 步骤 | 做法 |
|------|------|
| 1 | 备份要改的文件 |
| 2 | 注入一个**语义错误**（读错字段、去掉兜底、改错映射） |
| 3 | 重建受影响的目标 |
| 4 | 跑对应 suite → **必须变红**，且诊断能指出问题 |
| 5 | 还原 + 重建 → 必须恢复全绿 |
| 6 | `git diff --stat <文件>` 确认为空（无残留） |

**已验证有效的三处**：`BridgeJsonSelfTest`（7 条变红）、`BridgeContractSelfTest`（2 条双向变红）、`ScriptSerializationSelfTest / field_mapping_anchors`（1 条精确变红）。

**没做变异验证的锁 = 不知道它有没有用。**

---

## 9. 当前已知的、验不过去的项（不是回归）

| 项 | 现象 | 原因 |
|----|------|------|
| `VirtualHidSelfTest` | 9 通过 / 1 失败（`device_open`） | 未安装虚拟 HID 驱动。属 `full` 档，不影响 CI |
| `InjectionSelfTest` | 12 通过 / 1 失败 | 需管理员 + 驱动 |
| `WindowModeSelfTest / background_minimized_quiet_restore` | 偶发失败（本机实测约 40%） | 前台切换是异步的，而该用例用固定 sleep 观察。**属测试时序脆弱**，已把命名用例改成轮询+超时（15 次 14 次全绿），但同族的 `background_click_keeps_foreground` 因改造后恒失败已完整还原。**在隔离桌面会话里才能稳定测量**，故 `-Tier full` 仍不建议接进 CI |
| CI `Run self-tests` | 干净 runner 上失败（#5） | 见下方「待办」 |
