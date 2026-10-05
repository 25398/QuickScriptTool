# 脚本导出为独立 EXE —— 可行性评估

> 评估日期：2026-09-19
> 评估对象：QuickScriptTool（WebView2 壳 + `qst_engine` 引擎）
> 结论：**技术上可行，且项目现有架构比预期更适合这件事；真正的成本不在"能不能编译出 exe"，而在资源重定向、无 UI 下的可运维性、以及签名/杀软三块。**
>
> **后续**：产品方案见 [`script-to-exe-design.md`](script-to-exe-design.md)（含 OCR 走系统 WinRT、找图按需外带
> OpenCV、AI Key 引导配置三条落地路线）。§7 提到的「`ExportScriptFile` 不递归收集嵌套脚本」已在
> 2026-09-19 修复（新增 `src/script_package.*` + `ScriptPackageSelfTest`）。

---

## 0. 结论摘要

| 维度 | 判断 |
|------|------|
| 技术上是否可行 | ✅ **可行**，无需引入编译器/新语言/新运行时 |
| 引擎能否脱离 WebView 壳独立跑脚本 | ✅ **已具备**，并已实测通过（见 §2.2） |
| 现有代码可复用度 | ✅ 高：脚本包（zip）读写、图片收集、headless 引擎、托盘、紧急停止全都已存在 |
| 是否需要在用户机器上装编译器 | ❌ **不需要**（关键：走"模板 exe + 尾部追加资源"，不做运行时编译） |
| 主要工作量 | 新增一个 `QstScriptPlayer` 目标 + 一个"打包器" + 资源路径重定向层 |
| 主要风险 | 图片/嵌套脚本资源、AI 密钥与 OCR 运行时无法自包含、未签名 exe 的杀软与 SmartScreen 拦截 |
| 建议 | **分三阶段做**：P0 只支持"纯输入/逻辑脚本"（覆盖现有脚本约 94% 的动作），P1 加图片与窗口模式，P2 再谈 AI/OCR |

---

## 1. 需求拆解

"把脚本文件直接导出为 exe" 可以拆成三个互不相同的子问题，成本差异极大：

1. **A. 能不能生成一个 exe 文件** —— 成本最低，见 §3。
2. **B. 这个 exe 能不能在没有安装本产品的机器上跑起来** —— 取决于脚本用到的动作类型，见 §4。
3. **C. 这个 exe 跑起来之后，用户能不能管得住它**（停止、看错误、改配置）—— 最容易被忽略，见 §5。

多数同类产品的失败点都在 B 和 C，而不是 A。

---

## 2. 现状盘点

### 2.1 现有"导出"功能只做了文件搬运

- UI 入口：`ui/index.html` 的 `#btnMacroExport` / `#btnRecExport` → `ui/app.js:15921` → `qst.exportScript(path)`
- 桥接：`ui/bridge.js:49` → 命令 `exportScript`
- 实现：`src/webview/qst_webview_shell.cpp:3327` → `qst::webview::ExportScriptFile()`（`src/webview/webview_bridge_backend.cpp:1957`）
- 行为：
  - 脚本**不引用图片** → 直接另存为 `.json`
  - 脚本**引用图片** → 弹保存框，打成 **zip**（`script.json` + 模板图），走 `CreateZipFile()`

**这条现状非常关键**：项目里已经存在一套"**脚本 + 资源 = 一个自包含包**"的格式与实现，导出 exe 完全可以建立在它之上，而不是另起炉灶。

### 2.2 引擎已经能脱离 WebView 壳独立运行（已实测）

架构上的依赖倒置（`src/engine/engine_ui_hooks.h`，验收报告 §7.2 B1）已经把引擎对壳的依赖全部改成回调注入，
所以 `qst_engine` 可以独立链接。实测（本次评估现场跑的）：

```
build\Release\ScriptRunnerSelfTest.exe --json --engine
→ {"passed":11,"failed":0,"ok":true}   EXIT=0
```

其中 `engine_headless_start` / `engine_run_varcompute_wait` / `engine_run_loop_body` / `engine_run_goto_skips`
全部通过 —— 也就是说 **在一个纯 console 进程里，没有 WebView2 UI、没有 `ui/`、没有 558MB 的 `WebView2Fixed/`，
引擎可以正常启动并执行脚本动作**。

进一步做"干净目录"实测，确定最小运行时集合：

| 目录内容 | 结果 |
|---------|------|
| 只有 `ScriptRunnerSelfTest.exe` | ❌ 缺 `opencv_world4100.dll`（静态导入，非 delayload） |
| `ScriptRunnerSelfTest.exe` + `opencv_world4100.dll` | ✅ **11/11 通过，exit 0** |

> ⚠ 复现时的坑：刚 `cp` 完立刻执行会撞杀软扫描，返回 `126 Device or resource busy`，**等几秒重跑即可**。
> 另外注意当前产品壳 `QuickScriptTool.exe` 是 `/DELAYLOAD:opencv_world4100.dll`，缺 OpenCV 也能进壳、只是禁用找图；
> 而 `qst_add_selftest` 出来的自检 exe **没有** delayload。做 player 时应**照壳的做法加 DELAYLOAD**。

### 2.3 其它已存在的可复用资产

| 资产 | 位置 | 对导出 exe 的价值 |
|------|------|------------------|
| `CreateZipFile` / `ExtractZipFile` | `src/utils.cpp:1419` / `:1516` | 脚本包打包 / 自解压读取，**不用引第三方 zip 库** |
| `CollectImagePathsFromJson` | `webview_bridge_backend.cpp` 内 | 自动找出脚本引用了哪些模板图 |
| 托盘菜单 `TrayMenu` | `src/tray_menu.cpp` | **无壳依赖**，player 可直接用（含"停止当前运行 / 退出"） |
| 紧急停止 | `engine_host_window.h` 的 `RequestEmergencyStop()` / `input_emergency_teardown` | 无限循环脚本的保命机制 |
| headless 引擎 | `qst::engine::Start()` → `EngineHost::SetHeadlessUi(true)` | 隐藏窗口、无 UI 运行 |
| `ResolveImagePath` | `src/utils.cpp:996` | **资源重定向的天然收口点**（见 §5.1） |

---

## 3. 三种"生成 exe"路线对比

| 路线 | 做法 | 是否需在用户机器装工具链 | 评价 |
|------|------|------------------------|------|
| **① 模板 exe + 尾部追加资源** | 预编译好 `player.exe`，导出时把「脚本包 zip」追加到文件末尾，player 启动时读自身尾部 | ❌ 不需要 | ✅ **推荐**。导出 = 纯文件 IO，秒级完成 |
| ② 运行时编译 | 用 `rc.exe` + `link.exe` 生成带 RCDATA 的新 exe | ✅ 需要 MSVC | ❌ 不可行。用户机器不会有 VS；即使有，路径/版本不可控 |
| ③ 自解压包 | 7z SFX / Inno 把 player + dll + 脚本打成单 exe | ⚠ 需要外部工具 | ⚠ 可作补充。但 SFX 模块本身会被杀软重点关照 |

**路线 ① 的技术要点**（都是常规做法，无技术风险）：

- 尾部结构：`[player.exe][zip 数据][16 字节尾标：magic + zip 长度 + 版本]`
- player 启动时：`GetModuleFileNameW` → 打开自身 → 读最后 16 字节校验 magic → `SetFilePointer` 到 zip 起点 →
  用现有 `ExtractZipFile`（需要支持从"文件 + 偏移"解压；当前是纯路径接口，**需要加一个带偏移的重载**）→
  解到 `%TEMP%` 或 `AppDir()\scripts\` → `qst::engine::RunScriptPath()`
- 追加数据不破坏 PE 签名校验：exe 仍可正常执行；只是**数字签名会失效**（本来也没签名，见 §5.6）

> 补充：也可以不做"追加"，直接让 player 读**同目录**的 `script.zip`。
> 但那就不是"一个 exe"，用户体验差一档。建议默认追加，同时支持 `--pack <zip>` 参数便于调试。

---

## 4. 脚本内容对"自包含"的影响（决定性因素）

按 `src/script_types.h` 的 44 种 `ActionType` 分类：

### 4.1 完全自包含（纯 Win32，无外部依赖）

`MoveMouse` `MoveMouseRelative` `Wait` `MouseDown/Up/Click/Drag` `KeyDown/Up/Click`
`Loop` `EndLoop` `DefineBlock` `RunBlock` `HotkeyShortcut` `QuickInput` `ScrollWheel`
`If` `Else` `Goto` `StopMacro` `VarCompute` `TimerRecordTime` `GetCursorPos`
`CustomText` `LockScreenshot` `UnlockScreenshot` `RunProgram` `CloseProgram`
`OpenWebpage` `OpenFile` `ActivateWindow`

### 4.2 需要额外资源（必须一起打包或降级）

| 动作 | 依赖 | 处理方式 |
|------|------|---------|
| `FindImage` / `MultiMatch` / `FindColor` / `ColorMatch` / `GetColor` | **模板图片** + OpenCV | 图片进 zip（已有实现）；OpenCV 见 §5.2 |
| `TextRecognition`（OCR） | **Python 运行时** + paddle 依赖 | ❌ 无法自包含，只能"运行环境要求"或首启引导安装 |
| `AiTextAnalysis` / `AiImageAnalysis` / `AiActionExecute` | **API Key + 模型配置**（存在 `AppDir()\app_settings.json` 的 `savedModels[].apiKey`） | ⚠ 见 §5.3，是**安全与体验的双难点** |
| `RunMacro` / `MousePlayback` | 被引用的**其它脚本文件**（`targetPath`） | 需递归收集进 zip（当前 `ExportScriptFile` **没做递归**，是个缺口） |
| `WatchImage` | 同 `FindImage` | 同上 |

### 4.3 用你自己的脚本实测

拿 `build\Release\scripts\` 下 12 个真实脚本统计（共 ~180 个动作）：

| 动作 | 次数 | 类别 |
|------|-----:|------|
| wait / keyClick / keyDown / keyUp / moveMouse / mouseClick / scrollWheel / quickInput | 122 | ✅ 自包含 |
| if / else / loop / endLoop / runBlock / defineBlock / stopMacro / varCompute / timerRecordTime / activateWindow | 45 | ✅ 自包含 |
| findImage | 9 | ⚠ 需 9 张模板图 |
| textRecognition | 1 | ⚠ 需 Python OCR |
| aiActionExecute | 1 | ⚠ 需 API Key |

**约 93% 的动作天然自包含**；需要外部资源的只有 11 个动作，且其中 9 个只是图片（已有打包实现）。
被引用的 9 张图都在 `scripts/images/template_*.bmp`，无嵌套脚本。

这个分布说明：**先做"纯输入/逻辑 + 找图"的导出，就能覆盖绝大多数实际脚本**，不必一开始就啃 AI/OCR。

---

## 5. 阻碍与风险清单（按严重度排序）

### 5.1 🟡 图片路径重定向（中等，但必须做）

- 脚本里存的是**相对路径**（实测样本：`images/template_174709500.bmp` 与 `images\template_1332458796.bmp`
  两种斜杠混用），解析规则在 `ResolveImagePath()`（`src/utils.cpp:996`）：
  - 绝对路径 → 原样
  - `images\...` 前缀 → `ScriptsDir()\...`
  - 不含 `\` → `FindImagesDir()\...`（= `AppDir()\scripts\images`）
  - 其它含 `\` → `ScriptsDir()\...`
- **推论**：只要 player 把脚本包解压到 `playerAppDir\scripts\`，**现有解析逻辑就能直接工作，零改动**。
  这是最省事的做法，建议优先采用。
- 备选：若想解到 `%TEMP%`（不污染目录），则需要在 `ResolveImagePath` 增加一个"包内资源根"分支，
  或在解压后**改写 JSON 里的图片路径为绝对路径**（导入流程已有类似做法：
  `RemapImportedImagePathInScriptJson`，可直接复用）。

### 5.2 🟡 OpenCV 体积与可选性

- `opencv_world4100.dll` = **64.6 MB**，是导出体积的绝对大头（player.exe 本体仅约 5–6 MB）。
- 好消息：项目已支持 **DELAYLOAD + 优雅降级**（`src/opencv_runtime.h` 的 `OpenCvAvailable()` /
  `MarkOpenCvUnavailable()`），壳就是这么做的（`CMakeLists.txt:839`）。
- 建议策略（**按脚本内容自适应**）：
  - 脚本不含找图/找色 → **只导出 ~6MB 单文件**，不带 OpenCV
  - 脚本含找图 → 导出"exe + opencv_world4100.dll"两个文件，或把 dll 也追加进 exe、
    首启释放到 `%LOCALAPPDATA%\QstPlayer\cache\<hash>\`（首次慢，之后命中缓存）
- ⚠ 不建议静态链接 OpenCV：会得到一个 40–60MB 的 exe，且需重做一套 OpenCV 构建，收益不划算。

### 5.3 🔴 AI 动作的密钥问题（**设计难点，非技术难点**）

- AI 动作依赖 `AppDir()\app_settings.json` 的 `savedModels[].apiKey`（`src/app_settings.h:195/204/208`）。
- 三种做法各有代价：
  1. **打包密钥** → ❌ **强烈不建议**：等于把用户的 API Key 明文分发给任何人，反编译即得。
  2. **player 读同目录 `app_settings.json`** → ⚠ 可行，但要求导出时一并复制配置文件，同样泄露密钥。
  3. **player 首次运行时提示用户自己填** → ✅ **推荐**，但需要给 player 加一个最小配置界面
     （或一个 `--set-model` 命令行 / 一个可编辑的 `player_settings.json`）。
- 无论哪种，**导出时若检测到脚本含 AI 动作，都应明确提示用户**"此脚本需要 API 配置才能运行"。

### 5.4 🟡 窗口模式的交互依赖

- `EngineHost::ResolveWindowModeSelectMethod()`（`src/engine/engine_window_mode_hooks.cpp:211`）在
  `selectMethod == SelectOnStartup / MousePositionOnStartup` 时会：
  - 取当前前台窗口作为目标；若前台是自己则先隐藏再取
  - **失败时弹 `promptModal_.ShowInfo(...)`** —— 在无 UI 的 player 里这个提示**无处显示**，
    表现为"脚本静默不跑"，用户完全无法诊断。
- 另外 `EngineRunFromPath` 里 `if (!ResolveWindowModeSelectMethod(wmCfg))` 直接返回失败，
  player 必须把这条错误**显示出来**（托盘气泡 / MessageBox）。
- **建议**：导出时校验 —— 若 `windowMode.enabled` 且 `selectMethod` 是"启动时选择"，
  强制要求用户改为"指定窗口类"（`UseEditorWindowClass`，绑类名/exe 路径）或 `NoSelect`，
  否则给出明确警告。否则导出的 exe 在别人机器上必然失效。

### 5.5 🟡 假焦点注入与虚拟 HID

- 后台窗口模式需要 `FakeFocus32.dll` / `FakeFocus64.dll`（176KB / 211KB）与 player 同目录，
  **32 位目标要 32 位 DLL**（两条独立构建，见项目约定）。
- 虚拟 HID 需要 `interception.dll` + 内核驱动 `.sys`。按当前发版约定，
  **驱动不进默认包**（设置里按需下载）。导出 exe **不可能**把驱动也自包含（安装驱动需管理员权限）。
  → 用了虚拟 HID 的脚本，导出时只能提示"目标机器需先安装驱动"。
- 建议：导出时扫描动作，按需把 FakeFocus dll 一并带上；对虚拟 HID 给出明确的前置条件说明。

### 5.6 🔴 未签名 exe 的杀软 / SmartScreen 拦截

- 实测：`build\Release\QuickScriptTool.exe` 的 **PE Security Directory 为空 ⇒ 当前产品未做数字签名**。
- 导出的 exe 会更敏感：
  - "自解压 + 释放 DLL + 注入目标进程"的行为组合，是启发式引擎的经典特征
  - 未签名 exe 首次运行必被 SmartScreen 拦（"Windows 已保护你的电脑"）
  - 部分反作弊对"陌生 exe 注入自身"的判定比"知名工具"更激进
- **这是本项目最需要用户拍板的一条**：要么接受"首次运行需点"仍要运行"+"可能被误报"，
  要么投入代码签名证书（OV 证书约需年费，且需要公司主体；EV 证书才能直接过 SmartScreen）。

### 5.7 🟡 无 UI 下的可运维性（体验关键）

player 必须至少提供：

| 能力 | 现状 | 说明 |
|------|------|------|
| 托盘图标 | `TrayMenu` 可复用（无壳依赖） | 提供"停止当前运行 / 退出" |
| 停止热键 | `hotkey_stop.h` + `input_emergency_teardown` 已具备 | **必须**，否则无限循环脚本跑起来无法停止 |
| 错误显示 | 需新增 | 脚本加载失败 / 窗口模式未就绪 / 无有效动作，都要能看见 |
| 单实例 | ⚠ 需注意 | 壳用 `KeyMouse_SingleInstance` 互斥体；**player 必须换一个独立名字**，否则会和已安装的产品互踢 |
| 脚本 schema 兼容 | `kScriptSchemaVersion = 2` | player 必须带同一份 `MigrateScriptJson` / `MigrateScriptFileData`，否则老脚本跑不了 |

### 5.8 🟢 其它小项

- **坐标归一化**：脚本可能带 `coordsAreNormalized` / `coordMeta`（绑定分辨率）。
  引擎已有 denormalize 逻辑，跨分辨率应可用，但**窗口相对坐标（`windowRelative`）在目标机窗口大小不同时仍会错位**，导出时值得提示。
- **`ResolveLibraryScriptPath` 只认 `ScriptsDir()` / `RecordingsDir()`**（`src/utils.cpp:421`）。
  所以解压目录要么放进 player 自己的 `scripts\`，要么给 player 加一条"直接绝对路径"分支。
- **录制回放脚本**（`recordings/`）与宏脚本走同一套引擎，导出逻辑可共用。
- **导出体积**（按需自适应）：

  | 场景 | 产物 |
  |------|------|
  | 纯输入/逻辑 | `~6 MB` 单文件 |
  | + 找图（内嵌 OpenCV） | `~71 MB` 单文件（首启解压缓存） |
  | + 找图（外置 OpenCV） | `~6 MB` exe + `64.6 MB` dll |
  | + 后台窗口模式 | 再 `+0.4 MB`（FakeFocus32/64） |

---

## 6. 推荐方案与落地分期

### 6.1 架构

```
                    ┌──────────────────────────────┐
   导出时（主程序内）  │  1. 解析脚本，收集依赖        │
                    │     - CollectImagePathsFromJson
                    │     - 递归收集 RunMacro/MousePlayback 的 targetPath
                    │  2. CreateZipFile → 脚本包 zip │
                    │  3. player.exe + zip 追加成单文件
                    └──────────────┬───────────────┘
                                   ↓
   运行时的 player   ┌──────────────────────────────┐
   (新目标 QstScriptPlayer)                          │
                    │  读自身尾部 → 校验 magic        │
                    │  解 zip 到 playerDir\scripts\  │
                    │  qst::engine::Start()  (headless)
                    │  qst::engine::RunScriptPath()  │
                    │  托盘 + 停止热键 + 错误提示     │
                    └──────────────────────────────┘
```

### 6.2 需要新增/改动的文件

| 类型 | 文件 | 说明 |
|------|------|------|
| 新增 | `src/player/script_player_main.cpp` | player 入口：解包 + 起引擎 + 托盘 + 停止热键 |
| 新增 | `src/player/payload_append.h/.cpp` | 尾部追加/读取（magic + 长度 + 版本），可单测 |
| 新增 | `src/player/export_pack.cpp` | 导出侧：依赖收集 + zip 组装 + 追加 |
| 新增 | `tools/script_player_selftest.cpp` | 追加/解析往返、依赖收集、错误路径 |
| 改动 | `CMakeLists.txt` | 新增 `QstScriptPlayer` 目标（`WIN32`，DELAYLOAD OpenCV，复用 `qst_engine` / `qst_desktop_tools` / `qst_engine_ui_stubs`） |
| 改动 | `src/utils.cpp` / `utils.h` | `ExtractZipFile` 增加"从文件偏移解压"重载 |
| 改动 | `src/webview/bridge_commands.h` + `webview_bridge_backend.cpp` + `ui/bridge.js` + `ui/app.js` + `ui/index.html` | 新命令 `exportScriptAsExe`，UI 增加"导出为 EXE"入口与前置检查提示 |
| 改动 | `tools/package_release.ps1` / `package_with_version.ps1` | 把 `player.exe`（模板）随产品分发到 `AppDir()\tools\` 或内嵌 |
| 改动 | `installer/QuickScriptTool.iss` | 同上，模板必须进安装包 |

> 模板 `player.exe` 的存放：建议放在 `AppDir()\tools\player\`，导出时直接 `CopyFile` 再追加数据。
> 这样导出不依赖网络、不需要编译器，且 player 版本始终与主程序一致。

### 6.3 分期建议

| 阶段 | 范围 | 交付判据 |
|------|------|---------|
| **P0** | 纯输入/逻辑脚本导出（不含找图、窗口模式、AI、OCR）；单文件 exe；托盘 + 停止热键 + 错误弹窗；导出前做能力检查并列出"本脚本包含 N 个需要额外环境支持的动作" | 用你自己的 12 个脚本中不含 findImage/OCR/AI 的，导出后在**另一台干净机器**上能跑通；新增 SelfTest 覆盖"追加/解析往返"与"依赖收集" |
| **P1** | 图片打包（复用现有 zip 逻辑）+ 递归收集 `RunMacro` 依赖；含找图时自动附带 OpenCV（外置或内嵌）；窗口模式导出前校验 `selectMethod` 并给出修正建议 | 含 9 张模板图的脚本导出后跨机可跑；窗口模式脚本要么导出成功要么给出明确阻断原因 |
| **P2** | AI 动作的配置引导（player 首启引导填 Key，或读同目录配置）；OCR 的运行环境检查；代码签名 | 明确密钥不落盘的方案；签名证书到位 |

### 6.4 顺带建议（不阻塞本需求，但值得一起做）

1. **`ExportScriptFile` 目前不递归收集 `RunMacro` / `MousePlayback` 的 `targetPath`** ——
   现在导出的 zip 在"嵌套脚本"场景下是**不完整的**。这是既有缺口，导出 exe 会把它放大。
2. 图片路径两种斜杠混写（`images/xxx.bmp` 与 `images\xxx.bmp`）说明历史上存在写入不一致，
   `ResolveImagePath` 里那段 `recoverEatenTemplateName`（修 `imagestemplate_` 被吃掉前缀）也在暗示
   曾经有过序列化污染。导出功能会把路径**原样**带到别人机器上，建议顺手统一。

---

## 7. 结论

**可行，且推荐做。**

- **技术风险低**：引擎已能 headless 独立运行（已实测 11/11 通过），
  脚本包 zip 读写、托盘、紧急停止、图片收集这些基础件全都现成，
  连"最小运行时集合"都已实测清楚（player.exe + opencv_world4100.dll）。
- **不需要在用户机器上装任何工具链** —— 走"预编译模板 exe + 尾部追加资源"，导出是纯文件 IO。
- **真正的成本在工程细节**：图片路径重定向、递归依赖收集、无 UI 下的错误可见性、
  以及窗口模式/AI/OCR 这三类"无法自包含"动作的**前置校验与明确提示**。
- **需要用户拍板的一条**：**代码签名**。未签名的"自解压 + 注入"类 exe 必然被 SmartScreen 拦、
  且有一定误报率。这不是技术问题，是发布策略问题。
- **建议节奏**：先做 P0（纯输入/逻辑），它已覆盖现有脚本 ~93% 的动作；
  确认体验后再决定要不要为 AI/OCR 投入。

---

## 附：本次评估的实测命令与结果

```bash
# 1) 引擎 headless 能力（在 build\Release 下）
./ScriptRunnerSelfTest.exe --json --engine
# → {"passed":11,"failed":0,"ok":true}   EXIT=0

# 2) 最小运行时集合（干净目录）
mkdir <empty> && cp ScriptRunnerSelfTest.exe <empty>/ && ./ScriptRunnerSelfTest.exe --json --engine
# → 126 Device or resource busy（杀软扫描，等几秒重跑）
cp opencv_world4100.dll <empty>/ && ./ScriptRunnerSelfTest.exe --json --engine
# → 11/11 通过，EXIT=0   ⇒ 最小集合 = exe + opencv_world4100.dll
```

```python
# 3) PE 导入表（确认 OpenCV 为静态导入、无 WebView2Loader.dll 依赖）
#    4) 签名检查：Security Directory RVA=0x0 Size=0 ⇒ 未签名
# 5) 现有脚本动作分布：12 个文件 / ~180 动作，findImage 9 / textRecognition 1 / aiActionExecute 1
```
