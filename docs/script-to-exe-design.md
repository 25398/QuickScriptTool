# 脚本导出独立 EXE —— 产品设计方案

> 日期：2026-09-19
> 上游评估：[`script-to-exe-feasibility.md`](script-to-exe-feasibility.md)
> 本次已落地的前置修复：`ExportScriptFile` 递归收集嵌套脚本（见 §1.2）

---

## 0. 目标、非目标与一句话方案

**目标**：把脚本导出成一个 exe，发到**没装本软件**的电脑上，对方双击就能用，完成**大部分基本操作**。

**非目标**：100% 功能对等。以下三类天然做不到「零依赖自包含」，设计上选择**明确降级 + 导出前告知**，而不是假装支持：
1. 高精度 OCR（产品内是 RapidOCR/PaddleOCR + Python venv）
2. AI 动作（需要用户自己的 API Key）
3. 虚拟 HID / 内核驱动类功能（装驱动必须管理员权限）

**一句话方案**：

> 预编译一个 **player.exe 模板**（引擎 + 桌面工具，无 WebView2、无 ui/），导出时把「脚本包」追加到模板 exe 尾部，
> 得到**单文件 exe**；双击即跑，托盘 + 停止热键保证可控；**找图与 OCR 各提供「自带」和「走软件」两种模式，
> 由用户在导出时按目标电脑是否装了本软件来选**。

### 0.1 已拍板的决策（2026-09-19）

| 决策点 | 结论 |
|--------|------|
| 代码签名 | **不做，保持现状**（未签名）。因此产物必须靠"不写注册表、不自启、不驻留"降低启发式评分，并在首次运行给一次性说明 |
| 默认行为 | **双击即跑**（常驻托盘，可随时停止） |
| OCR 后端 | **由导出模式决定**：脚本含 OCR 动作时弹窗让用户选「自带（系统 WinRT OCR）」或「走软件（用目标机已装软件的 OCR）」 |
| 找图 | **同上**：含找图/找色动作时让用户选「自带 OpenCV」或「走软件（用目标机已装软件的 OpenCV）」 |
| 产物形态 | **单文件**（默认且唯一） |

> 关键推论：**「自带」= 给没装软件的人用；「走软件」= 给已装软件的人用，产物更小、行为与软件内完全一致。**
> 两种模式可以在同一个导出流程里分别对「找图」和「OCR」独立选择，因为它们的成本与行为差异是正交的。

---

## 1. 事实基础（本次实测，不是推测）

### 1.1 引擎可以脱离 WebView 壳独立运行

```
build\Release\ScriptRunnerSelfTest.exe --json --engine   → {"passed":11,"failed":0,"ok":true}
```
最小运行时集合实测为 `exe + opencv_world4100.dll`；没有 `ui/`、没有 558MB 的 `WebView2Fixed/`、
没有 `WebView2Loader.dll`，引擎照常启动并执行动作。

### 1.2 前置缺口已修（本轮）

`ExportScriptFile` 原来只收集**根脚本自己**引用的图片，嵌套脚本（`runMacro` / `mousePlayback` 的
`targetPath`）整份丢包 —— 导出的 zip 在嵌套场景下本身就是残缺的。已修并加了 `ScriptPackageSelfTest`
（13 个用例）。**这是导出 exe 的硬前置**：exe 里没有"事后补文件"的机会。

改动：
- 新增 `src/script_package.h/.cpp`（纯逻辑：递归收集嵌套脚本 + 图片、改写 `targetPath`、包清单）
- `ExportScriptFile` 改为按依赖计划打包：`script.json` + `scripts/<名>.json` + 图片 + `package.json` 清单
- `ImportScriptFile` 支持还原嵌套脚本并把 `targetPath` 改写成还原后的真实文件名
- `ExtractZipFile` 补上「自建中间目录」（原先 `scripts\x.json` 会被静默丢弃）
- 导出结果新增 `missingRefs`，UI 会提示"引用的子脚本找不到，未打包"

验证：`run_all_selftests.ps1 -Tier logic` → **21 个 suite / 21 通过 / 0 失败 / exit 0**。

### 1.3 三个难点的现状（决定了设计取舍）

| 难点 | 现状（源码事实） | 能否自包含 |
|------|-----------------|-----------|
| **找图** | `image_match.cpp` 全部入口都有 `if (!OpenCvAvailable()) return;`，**没有非 OpenCV 的匹配引擎**（`image_match_engines.h` 直接 `#include <opencv2/core.hpp>`） | ⚠ 必须外带 `opencv_world4100.dll`（64.6 MB） |
| **OCR** | `ocr_engine.cpp` 100% 依赖 Python：固定 venv `C:\paddle_env\venv` + RapidOCR/PaddleOCR，还有自动下载安装 Python 3.12 的流程 | ❌ 原实现无法自包含 |
| **热键启停** | `hotkey_stop.h`、`input_emergency_teardown`、`TrayMenu` 全在引擎 / `qst_desktop_tools` 层，**不依赖壳** | ✅ 可直接复用 |

### 1.4 关键发现：系统自带 OCR 可用（**本方案的支点**）

项目**已经在用 C++/WinRT**（`src/window_mode/window_capture_wgc.cpp` include `winrt/Windows.Graphics.Capture.h`），
并且链接了 `windowsapp.lib`（`QST_COMMON_LINK_LIBS`）。Windows SDK 自带
`cppwinrt\winrt\windows.media.ocr.h`（已在本机 `10.0.26100.0` 确认存在）。

**实测（PowerShell 调 WinRT）**：

```
[Windows.Media.Ocr.OcrEngine]::AvailableRecognizerLanguages
  → OCR 可用语言数: 1
    zh-Hans-CN / 简体中文(中国大陆)
[Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages() → True
```

**结论**：`Windows.Media.Ocr` 可以在**零安装、零体积、离线**的前提下做中文 OCR。
这是把 OCR 从"无法自包含"变成"可自包含"的关键，也是本方案与"必须让用户装 Python"路线的分水岭。

---

## 2. 产品形态

### 2.1 入口：默认走 EXE，设置里可切回 zip

导出入口**保持唯一**（用户点一次「导出」），**不再弹方案选择**：

```
选中脚本 → 点「导出」
   ├─ 默认（设置未勾选）→ 直接弹「导出为独立 EXE」（见 §2.2）
   └─ 勾了「其他设置 → 导出脚本默认为 zip 格式」→ 走原来的 zip 脚本包路径
```

设置项：`AppSettings.other.exportScriptAsZip`（**默认 false**），
UI 在「设置 → 其他设置 → 行为与外观」里，与「脚本启动时播放声音」同一组复选框。

> 为什么不弹选择框：exe 形态已经**能免安装运行、也能被本软件导入**（§2.5），
> 也就是"软件能解析出来，AI 脚本助手照样读得懂"——那 zip 的唯一优势只剩体积。
> 所以把它降级成一个设置开关，而不是每次都拦一道。

「导出」按钮（极简模式的宏/录制列表、专业模式右键菜单）都走这一套；
专业模式通过 `window.QstExport.open(path, name)` 复用同一流程。

### 2.2 导出为独立 EXE 的对话框

UI 全部用项目既有的主题变量（`--text` / `--text-2` / `--text-3` / `--line` / `--frost` /
`--ice-*` / `--mint-*` / `--rose`）与既有组件（`.overlay` / `.dlg` / `.btn` / `.radio` / `.chk`），
**换主题时自动跟随**，不需要额外适配。

对话框内容（脚本含哪些能力，就只显示哪几行）：

```
┌─ 导出为独立 EXE ────────────────────────────────────────┐
│ 脚本：龙女下风口      动作 187 个                        │
│                                                          │
│ 本脚本用到的能力：                                        │
│   找图 / 找色        9 处                                │
│   文字识别           1 处                                │
│   AI 动作            1 处  ⚠ 需要目标电脑配置 API Key     │
│   后台窗口模式       否                                  │
│   引用的子脚本       2 个（已打包）                       │
│   找不到的子脚本     无                                  │
│                                                          │
│ 目标电脑是否装了「键鼠工坊」？                             │
│   ○ 没装 / 不确定 → 全部自带（推荐，发给任何人都能用）     │
│   ● 装了         → 找图与 OCR 复用软件里的组件（体积更小） │
│                                                          │
│   [展开高级]  找图：○自带 ●走软件    OCR：●自带 ○走软件   │
│                                                          │
│ 预计体积：约 6 MB                                        │
│                              [取消]  [导出]              │
└──────────────────────────────────────────────────────────┘
```

- 一个总开关（"目标电脑装没装"）联动两项子选项，避免让用户面对两个技术名词；
  但**展开高级**可以分别指定 —— 因为确实存在"装了软件但 OCR 环境没装好"这种组合。
- 脚本**不含**找图/OCR 时，对应那一行不显示，也不参与体积估算。
- 体积估算实时更新，让"自带 vs 走软件"的代价一眼可见。

选保存路径（原生保存框，默认文件名 = 脚本名 + ".exe"）→ 写出单文件 exe。

### 2.3 导出模式：自带 vs 走软件

这是本方案的核心机制。**同一份脚本，两种模式下 player 的行为完全不同。**

| | **自带模式**（目标机没装软件） | **走软件模式**（目标机装了软件） |
|---|---|---|
| **找图 / 找色** | OpenCV 内嵌进 exe，首启释放到 `%LOCALAPPDATA%\QstPlayer\rt\<hash>\` 并缓存 | 从已装软件目录 `LoadLibrary` 它的 `opencv_world4100.dll`；找不到则回退到"自带"并提示 |
| **OCR** | **系统自带 WinRT OCR**（零安装，见 §1.4）；语言包缺失时明确提示 | 调软件目录的 `tools\paddle_ocr_helper.py` + `C:\paddle_env\venv`；环境缺失时回退到系统 OCR 并提示 |
| **体积** | 6 MB（纯输入）～ 71 MB（含 OpenCV） | **恒定约 6 MB** |
| **行为一致性** | 找图与软件内**完全一致**（同一份 OpenCV）；OCR 与软件内**可能有差异** | 与软件内**完全一致** |
| **目标机要求** | 无 | 已安装键鼠工坊；OCR 还要求装过 OCR 运行环境 |

**软件安装目录怎么找**（按可靠性排序，全部命中即用）：

1. `HKCU\Software\QuickScriptTool` → `LastRunDir`（`RecordLastRunAppDir()` 每次启动都会写，最可靠）
2. `HKLM\Software\QuickScriptTool` → `InstallPath`（安装包写入，`installer/QuickScriptTool.iss:125`）
3. `HKCU\Software\QuickScriptTool` → `InstallPath`
4. `HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\QuickScriptTool.exe`
5. `%ProgramFiles%\键鼠工坊`

**回退规则（重要）**：走软件模式**不得硬失败**。找不到软件/组件时，player 必须
① 回退到自带路径（若 exe 内嵌了的话），或 ② 给出**可读的一句话原因 + 下载入口**，
绝不能静默不跑。这也是导出时要把两种模式的依赖都记录进脚本包的原因。

### 2.4 运行行为（双击即跑 → 跑完自退；**不常驻托盘**）

```
双击 exe
  ├─ 自我迁移到 %LOCALAPPDATA%\QstPlayer\rt\<payload哈希>\（见 §6.1）
  ├─ 解包脚本到 <运行时目录>\scripts\；rt\ 前缀的组件（OpenCV / FakeFocus）放到运行时目录根
  ├─ 按导出模式准备依赖（找图 / OCR）
  ├─ 启动 headless 引擎 → 按脚本自身流程执行（含默认模式的"中断脱离"）
  ├─ 等到 running_ 转 false（跑完 / 被停止）→ **进程自己退出**
  └─ 控制热键：**用脚本自己配的那个热键**（导出时设置的，`hotkeyVk`/`hotkeyModifiers`）
        · 单击（<600ms）→ 暂停 / 继续
        · 长按（≥600ms）→ 停止并退出
        · 脚本没配热键 → 回退 F9（并在日志里说明）
```

**热键为什么要"先读再抹"**（`player_main.cpp`，两处都实测踩过）：

1. 引擎 `Start()` 时会扫描脚本目录，把「脚本热键」注册成**回放钩子**
   （`ghPlaybackScriptHooks`）——运行中按它走的是**紧急停止**分支，
   会抢在播放器的「单击/长按」判定之前把脚本停掉。实测：单击一下脚本直接结束。
2. 引擎加载脚本时还会把 `script.json` **规范化重写**（补 `coordMeta` / 动作默认字段），
   那一步会把 `hotkeyVk` 清成 0 —— 读晚了什么也读不到。

所以：在 `engine::Start()` **之前**读出热键，然后把脚本里的热键字段抹成 0
（`StripScriptHotkey`），让引擎彻底看不见这个键。

**三条硬语义（2026-09-19 按用户要求定稿）**：

1. **不常驻托盘、不留后台进程**。脚本结束（`stopMacro` / 跑完）或被停止后立刻退出。
   - 界面只剩一个隐藏的 0 尺寸 `WS_EX_TOOLWINDOW` 消息窗（只为收 `WM_HOTKEY`），
     不出现在任务栏 / Alt-Tab。
2. **默认模式的「中断脱离」必须按原逻辑恢复执行** —— 这是唯一的例外。
   - 引擎 `waitBreakoutCooldown()` 期间只把 `breakoutPaused_` 置真，`running_` **仍为 true**，
     所以播放器的等待循环天然会继续等，脱离结束引擎自己恢复。
     **不要**因为"看到 `breakoutPaused_` 就去判断是否该退出"。
3. **脚本必须以 `stopMacro` 收尾**才会结束。
   - 引擎的 worker 是 `while (!StopRequested()) { runRange(...) }`，只有 `stopMacro`
     会把 `stopFlag_` 置真。这是**产品既有语义**，播放器刻意不改 ——
     同一脚本在软件里和在 exe 里行为一致，比"播放器自作聪明跑一遍"重要得多。
     没写 `stopMacro` 的脚本在 exe 里会一直循环，按 F9 停。

> 首次运行说明改为**写 README.txt**（在运行时目录里），不弹窗 —— 用户要的是"双击即跑"。
> 代价是"对方不知道怎么停"这件事只能靠 README 传达；如果后续觉得不够，可以加一次性的
> 托盘气泡或确认框（代价是打断"即跑"）。

---

### 2.4.1 提示音：项目音优先，缺失才回退系统音

| 时机 | 音效 | 实现 |
|------|------|------|
| 脚本启动 | `startup.wav` | 引擎按 `appSettings.other.playSoundOnStart` 自动播（默认开） |
| 暂停 | `pause.wav` | 播放器在 `TogglePlaybackPause()` 里播 |
| 继续 | `startup.wav` | 同上（复用启动音，表示"又跑起来了"） |
| 脚本结束 | `finish.wav` | 引擎按 `playSoundOnEnd` 自动播 |

- 三者都走 `PlayWavOrFallbackBeep()`（`src/utils.cpp`）：**wav 存在且 RIFF/WAVE 头合法就播它，
  否则 `MessageBeep(MB_OK)` 走系统音** —— 正好是"项目音优先，否则系统音"。
- 引擎的 `PlayAppStartupSound()` 按 **`AppDir()\startup.wav`** 找文件，
  所以导出时必须把三个 wav 打进 payload 的 `rt\`，播放器释放到运行时目录根（= AppDir()）。
  三个加起来不到 100KB，**无条件带**（脚本是否"用得到"取决于运行时分支，导出时判不出来）。
- `pause.wav` 是**本项目生成**的（两音下行短促提示，mono/16bit/44100Hz/0.25s，
  与 startup/finish 同格式）。要换音效直接覆盖 `resources/pause.wav` 即可。

**⚠ 结束音必须等它放完再退进程。** 提示音是 `SND_ASYNC` 播的，
播放器跑完脚本就立刻退出的话，声音会被硬截断 —— 实测"结束音只响了一半"，听感很怪。
修法：退出前用 `WavDurationMs(AppFinishSoundFilePath())` 读出 wav 时长并 `Sleep`
（上限 3 秒；读不到时长时给 400ms 短缓冲）。

### 2.5 「导出的 exe 能不能被本软件导入」—— 可以，而且是**天然成立**的

**结论：可行，已实现。** 而且不需要为它设计任何新格式。

原因很简单：**payload 本身就是一个标准 zip**（store 模式，含 `script.json` /
`scripts\*.json` / 图片 / `package.json` 清单），只是被追加到了 exe 末尾。
所以导入侧只要：

```
检测文件尾部是否有 QSTPKG01 尾标
  → 有：把 payload 原样 dump 成一个临时 zip（DumpPayloadToZip）
        → 交给**既有的 zip 导入路径**（含嵌套脚本还原、图片重映射）
  → 无：按原来的 .zip / .json 处理
```

实现就这么多：`ImportScriptFile` 里加约 25 行（`ReadPayloadInfo` → `DumpPayloadToZip` →
把 `src` 换成临时 zip），文件过滤器加 `*.exe`。**导入端一行都不需要知道 exe 的存在。**

已验证可复用的既有能力：
- `ReadPayloadInfo` / `DumpPayloadToZip`（`src/script_package.*`，`ScriptPackageSelfTest` 已覆盖
  「dump 出的字节与原始 zip 逐字节相同」）
- zip 导入路径本身已支持嵌套脚本还原 + 图片重映射（见 §1.2）

⚠ 两个细节：
- 导入时 `rt\` 前缀的运行时组件（OpenCV / FakeFocus）**会被忽略** ——
  它们只在运行时目录里有意义，进软件库反而会污染。
  `ExtractZipFile` 会把它们解到临时目录，导入逻辑只挑 `script.json` / `scripts\` / 图片，
  临时目录最后整体删掉。
- 导入后的脚本**不带** `player` 段的选择（那是 exe 运行时的配置），
  在软件里跑就按软件自己的设置来 —— 符合直觉。

> 顺带的好处：用户拿到别人发来的 exe，即使不运行也能"导入到软件里看看它做了什么"。
> 这比"必须双击跑起来才知道"安全得多，也便于排查。

## 3. 三个难点的具体解法

### 3.1 找图 → 两种模式，但不做自研匹配器

**决策：`image_match.cpp` 那一份代码原样复用，只在"OpenCV 从哪来"上分两种模式。**
**不自研简化匹配器** —— 阈值语义、金字塔、像素终审、NMS 都不一样，
会让"同一个脚本在产品里和 exe 里结果不一致"，这比"exe 大 65MB"糟糕得多。

| 模式 | 做法 |
|------|------|
| **自带** | `opencv_world4100.dll` 内嵌进 exe（zip 存储），首启释放到 `%LOCALAPPDATA%\QstPlayer\rt\<sha256前16位>\`，之后命中缓存不再释放。**同一台机器上多个导出的 exe 共享这一份缓存** |
| **走软件** | 找到软件目录后 `LoadLibraryW(<软件目录>\opencv_world4100.dll)`；失败则回退自带（若包内带了）或给出可读提示 |

**实现要点（很关键，能省掉一次改造）**：`TryInitOpenCv()`（`src/opencv_runtime.cpp`）
第一步就是 `GetModuleHandleW(L"opencv_world4100.dll")`，**只要提前 `LoadLibrary` 好，
它就直接返回 Ready** —— 所以 player 只要在 `qst::engine::Start()` **之前**
把 OpenCV 按模式加载好即可，`opencv_runtime.cpp` **一行都不用改**。

player 目标必须带 `/DELAYLOAD:opencv_world4100.dll`（照 `QstWebViewShell` 的做法，
`CMakeLists.txt:839`），否则 exe 在缺 DLL 时**根本起不来**，谈不上"回退 + 提示"。

### 3.2 OCR → 两种模式（自带 = 系统 WinRT OCR）

**新增一个 OCR 后端**，与现有 Python 后端同层：

```
src/player/ocr_winrt.cpp
  ├─ Windows.Media.Ocr.OcrEngine::TryCreateFromUserProfileLanguages()
  │    （失败则遍历 AvailableRecognizerLanguages 找 zh-Hans-CN）
  ├─ HBITMAP → SoftwareBitmap（BGRA8, Premultiplied）
  ├─ RecognizeAsync → OcrResult.Lines / Words（含 BoundingRect，可直接映射回屏幕坐标）
  └─ 输出成现有 OcrEngineOutput（复用 ocr_result.h 的结构，上层无感）
```

| 模式 | 后端 | 说明 |
|------|------|------|
| **自带** | 系统 WinRT OCR | 零安装、零体积、离线。**不保证与软件内逐字一致** |
| **走软件** | 软件目录的 `tools\paddle_ocr_helper.py` + `C:\paddle_env\venv` | 与软件内完全一致；环境缺失时回退到系统 OCR 并提示 |

产品内（装了软件时）**保持现有 RapidOCR/PaddleOCR 不变** —— 零回归风险。
只有 player 才走后端选择逻辑。

必须向用户交代的差异（写进导出对话框 + player 的 README）：
- 系统 OCR 对**小字号 / 密集数字**的准确率低于 RapidOCR。若脚本用 `ocrDigitsOnly`，
  player 端加一层数字白名单后处理来补。
- 系统需要装对应 OCR 语言包。`AvailableRecognizerLanguages` 为空时 player 明确提示
  "目标电脑缺少中文 OCR 语言包"，而不是静默失败。
- **不保证与软件内逐字一致** —— 所以"走软件"模式的卖点正是"结果完全一致"。

### 3.3 热键启停与可控性 → player 自带，不依赖壳

- **托盘**：`TrayMenu`（`src/tray_menu.cpp`）**无壳依赖**，直接用；图标资源已在 `resources/`。
- **停止热键**：复用 `hotkey_stop.h` 的策略（含"启动键未抬起时不得误停"、"远控 SendInput 视为用户键"、
  UIPI/独占全屏下的轮询兜底）——这套踩坑已经踩完了，**不要另写一套**。
- **紧急停止**：`input_emergency_teardown` 已覆盖异常退出时的钩子/键态清理。
- ⚠ **单实例互斥体必须换名字**：壳用的是 `KeyMouse_SingleInstance`。player 用
  `QstScriptPlayer_<脚本包 hash>`，否则会和已安装的产品互踢（用户装了两个就更乱）。
- ⚠ **脚本 schema 迁移必须带上**：`kScriptSchemaVersion` 的 `MigrateScriptJson` /
  `MigrateScriptFileData` 要在 player 里可用，否则老脚本在新 exe 里跑不了。
  由于 player 直接链 `qst_engine`，这一点天然满足——**这是复用整个引擎而非抽子集的重要理由**。

---

## 4. 自包含清单（打包什么、不打包什么）

| 文件 | 必需性 | 说明 |
|------|--------|------|
| `player.exe` | ✅ 必需 | 引擎 + 桌面工具 + stubs，约 5–6 MB |
| CRT 9 个 dll（`vcruntime140*.dll` / `msvcp140*.dll` …） | ✅ 必需 | **旁路部署**，目标机无需装 VC Redist。照 `tools\package_release.ps1` 的清单。单文件形态下内嵌 + 首启释放 |
| `opencv_world4100.dll` | ⚠ 按模式 | **自带模式**内嵌；**走软件模式**从软件目录加载，不进包 |
| `FakeFocus32.dll` / `FakeFocus64.dll` | ⚠ 条件 | 仅后台窗口模式。32 位目标要 32 位那份。**这份没有"走软件"模式**（必须注入到目标进程，路径必须可控） |
| 驱动安装脚本 + `interception.dll` | ⚠ 条件 | 仅虚拟 HID；**装驱动需管理员**，只能提示不能自动 |
| `app_settings.json` | ⚠ 条件 | 仅 AI 动作需要（见 §5） |
| `ui/`（1.9 MB） | ❌ 不需要 | player 没有 WebView UI |
| `WebView2Fixed/`（**558 MB**） | ❌ 不需要 | 只有 AI 网页抓取才用 |
| `extension/` `skills/` `office/` | ❌ 不需要 | |
| `tools/paddle_ocr_helper.py` | ❌ 不需要 | 走软件模式下从软件目录读，不进包 |

**体积预算**

| 场景 | 产物 |
|------|------|
| 纯输入/逻辑 | **约 6 MB** 单文件 |
| + 找图（自带） | 约 71 MB 单文件 |
| + 找图（走软件） | **约 6 MB** 单文件（不涨） |
| + 后台窗口模式 | 再 +0.4 MB |
| + OCR | 两种模式都**不增加体积** |

---

## 4.5 设置快照：**导出时是什么设置，exe 里就是什么设置**

这是"效果一模一样"的地基。做法很简单也很关键：
**把导出那一刻的整份 `app_settings.json` 打进 payload**，播放器释放到运行时目录，
引擎照常按 `AppDir()\app_settings.json` 读它 —— 于是 exe 里的行为和软件内完全一致。

实现只用了一个函数：`SerializeAppSettings(const AppSettings&, bool portableSecrets)`
（`src/app_settings_store.h/.cpp`）。

**为什么必须复用同一个序列化器**（而不是另写一份"便携版"）：
漏掉任何一个字段，就是一处行为差异，而且是**静默**的 —— 用户只会觉得"exe 跑得不对"。
所以 `SaveAppSettings` 和导出侧都调它，只有 `portableSecrets` 一个参数不同。

### 被快照覆盖的、会影响回放效果的设置

| 分类 | 字段 | 效果 |
|------|------|------|
| **回放次数** | `enablePlaybackCount` / `playbackCount` | 与 `stopMacro` **共同约束**：谁先命中谁生效（见下） |
| **回放间隔** | `enablePlaybackInterval` / `playbackIntervalMin/MaxSeconds` | 每轮回放之间的随机间隔 |
| **倍速** | `enablePlaybackSpeed` / `playbackSpeed` | 录制回放的倍速（导出时选 2 倍速 ⇒ exe 也 2 倍速） |
| **性能** | `lowPerformanceMode` / `aiFastPaths` / `findImageGpuAccel` | 低性能模式、AI 高级加速、找图 GPU 加速 |
| **窗口模式** | `enableFakeFocusInjection` / `injectionTechnique` / `hideInjectedModule` / `allowForegroundInputFallback` / **`enableWindowTimeScale`** | 后台注入方式、假焦点、模块隐藏、前台回退、**目标窗口变速** |
| **前台注入后端** | `foregroundInputBackend` | Software / Interception / VirtualHid |
| **AI** | `apiUrl` / `apiKey` / `modelName` / `temperature` / `maxTokens` / `savedModels` | 密钥**明文**（见 §5） |
| 其它 | `click` / `other` / `home` | 一并带上（`home.uiMode` 会影响倍速换算，不能漏） |

### 回放次数 与 stopMacro 是**共同约束**，不是互相替代

引擎的 worker 长这样（`engine_script_run.cpp`）：

```cpp
while (!StopRequested()) {                       // ← stopMacro 置 stopFlag_ ⇒ 这里退出
    runRange(0, actions.size());                 // ← 动作里遇到 stopMacro 就置 stopFlag_
    releaseHeldKeys();
    if (ps.enablePlaybackCount && ps.playbackCount > 0 && curLoops_ >= ps.playbackCount) break;
    if (StopRequested()) break;                  // ← 次数到了也走这里
    ...
}
```

两个约束**独立并存，谁先命中谁生效**：

| 脚本有 `stopMacro` | `enablePlaybackCount` | 实际行为 |
|---|---|---|
| 有 | 否 | 跑到 `stopMacro` 就停（1 轮） |
| 有 | 是，=5 | **1 轮就停** —— `stopMacro` 先命中，不会硬跑满 5 轮 |
| 无 | 是，=3 | 跑 3 轮后停 |
| 无 | 否 | 无限循环，直到 `stopMacro` / 热键 / 停止 |

> **`stopMacro` 没有被绕过、也没有被删掉。** 导出侧只是把设置快照带过去，
> 动作序列原样保留 —— 导入回来也还是原来那份脚本。

**这个矩阵可以随时重跑**（改引擎 worker / 播放器 / 设置快照之后尤其应该跑）：

```
python tools\verify\player_behavior_matrix.py
```

它会现场构造四个导出的 exe 并观察真实行为 —— 这些行为横跨
「引擎 worker 循环 / 设置快照 / 播放器生命周期」，没有单元测试能覆盖。

### 边界与约定

- **导入回来后仍是原脚本**：设置放在 exe 的 payload 里，**不写进脚本 JSON** ——
  所以别人把 exe 导入到软件里，拿到的是干净脚本，跑起来走**他自己的**设置。
- **`autoStartOnBoot` 之类的开关在 exe 里是惰性的**：只有 UI 保存设置时才会
  `SetAutoStartOnBoot`，引擎启动时不读它 —— 不会让产物偷偷写注册表。
- 拿不到设置（`LoadAppSettings` 失败）时**不写这份文件**，引擎走默认值 ——
  宁可"按默认跑"，也不要写一份半截的设置进去。

## 4.6 嵌套宏（引用的其他宏 / 录制回放）：多层递归 + 名字引用

**结论：导出的 exe 发到别人电脑上能正常运行，包括多层嵌套和按名字引用。**
这一节把"最底层"讲清楚，免得以后再怀疑。

### 收集（导出侧，`src/script_package.cpp`）

`BuildPackagePlan` 是 **BFS**，边遍历边入队，所以**任意深度**的嵌套都会收到：

```
root.json ──runMacro──▶ child.json ──runMacro──▶ grand.json
```

| 能力 | 实现 | 用例 |
|------|------|------|
| 只认跨文件引用 | 只有 `runMacro` / `mousePlayback` 收；`runProgram` / `openFile` / `activateWindow` 的 `targetPath` **不收** | `collect_only_nested_types` |
| **多层递归** | BFS，不是只收一层 | `plan_multilevel_recursion`（根→A→B→C 四份全收） |
| 循环引用安全 | `visited` 集合去重，A→B→A 不死循环 | `plan_cycle_safe` |
| **名字引用** | `targetPath` 空、只有 `blockName` 时也收（`blockName` 就是库内脚本文件名，见 `ResolveNestedLibraryTarget`） | `plan_blockname_only_ref` |
| 同名去重 | 两个不同脚本同名时条目名变 `x.json` / `x-2.json`，不会互相覆盖 | `plan_unique_names_on_collision` |
| 引用改写 | 全量 remap 应用到**每一份**脚本（含子脚本）—— 漏掉子脚本 = 孙脚本在目标机找不到 | `plan_rewrites_refs_inside_children` |
| 图片 | 每一层脚本引用的图片都收（findImage / watchImage / multiMatch / OCR 按图取区 / AI 目标图 …） | `plan_collects_nested_images` |

### 目标机解析（播放器侧）

导出侧把引用改写成**裸文件名**（`child.json`），播放器把它们解到 `scripts\scripts\`，
而引擎的 `ResolveLibraryScriptPath` 会**递归**搜 `ScriptsDir()`（= `AppDir()\scripts`），
所以 `scripts\scripts\child.json` 能被找到 ✓。

图片统一挪进 `scripts\images\`（引擎的 `FindImagesDir()`），扁平基名因此也能对上 ✓。

### ⚠ 一个必须知道的既有语义：嵌套宏里的 `stopMacro` 会终止**整次运行**

`StopMacroShouldEndEntireRun()` 在非定时场景恒为真 ⇒ `stopMacro` 置全局 `stopFlag_`，
**不管嵌套多深，都会把父脚本一起结束**。软件里同样如此，**不是导出引入的差异**。

写脚本时子宏不要放 `stopMacro`（要提前返回请用 `goto` / `if` 控制流程）。
这一点我第一次做验证时也踩了 —— 四层标记只出来两层，一度以为是导出的 bug。

### 可复现验证

```
python tools\verify\nested_macro_export.py
```

造一棵三层嵌套 + 一个「只用 blockName 引用」的脚本树，按导出侧真实布局打包成 exe，
在**全新的 `LOCALAPPDATA`**（模拟别人电脑）下跑，断言四个标记文件全部出现。
实测结果：

```
child    -> 执行了        grand    -> 执行了        ← 第 3 层
sibling  -> 执行了        ← 只用 blockName 引用
root     -> 执行了
嵌套脚本落在 scripts\scripts\ ：True
嵌套脚本引用的图片落在 scripts\images\ ：True
```

## 5. AI 动作：把密钥随 exe 打包（明文，跨机可用）

**已实现（2026-09-19 按用户决定）**：脚本含 AI 动作时，导出会把 AI 设置
**连同密钥一起打进 exe**，目标机不用配置就能跑。

实现：`BuildPortableAiSettingsJson()`（`src/portable_ai_settings.h/.cpp`，放在 **`qst_utils`**）
生成只含 `ai` 段的 `app_settings.json`，作为 `rt\app_settings.json` 进 payload，
播放器释放到运行时目录根 —— 引擎按 `AppDir()\app_settings.json` 读它。

**⚠ 关键坑：绝对不能用 `SaveAppSettings()` 走一遍。**
它内部用 `CryptProtectData`（**用户级 DPAPI**）加密 `apiKey`，
换一台机器 / 换一个用户就解不开 —— 打进 exe 等于把 AI 动作弄坏（而且是静默失败）。
而 `UnprotectSecret` 对**不以 `"dpapi:"` 开头**的值是原样返回的，
所以这里直接写明文，目标机就能直接读出来用。`AppSettingsStoreSelfTest` 有
`portable_ai_json_plaintext_key` / `_roundtrip` / `_escapes` 三条用例锁住这个行为。

**安全提示（已接受）**：这等于把用户的 API Key 明文分发给拿到 exe 的任何人。
导出成功时会额外弹一条 toast：「注意：AI 密钥已随 exe 打包，请只发给可信的人」。
后续要改成分发式密钥 / 首启引导填 Key，都只需要替换这一个函数的产物。

**其它约定**：
- 只写 `ai` 段 —— 主题 / 界面 / 选中项这些不该跟着 exe 跑到别人机器上。
- 只在脚本**确实含 AI 动作**时才带（`scan.NeedsAi()`），不含就不把密钥塞进去。

---

## 6. 关键技术实现

### 6.1 模板 exe + 尾部追加资源

```
┌──────────────────────────────┐
│  player.exe（PE 映像）        │
├──────────────────────────────┤
│  脚本包（zip，可能含 OpenCV）  │  ← 导出时追加
├──────────────────────────────┤
│  尾标 16 字节                  │
│  magic "QSTPKG\0\0"           │
│  payloadOffset (u64)          │
│  payloadSize   (u64)          │
└──────────────────────────────┘
```

- **导出 = 复制模板 + 追加数据**，纯文件 IO，秒级完成，**目标机/本机都不需要编译器**。
- 读尾部 → `SetFilePointer` 到 payload → 解包。
- ⚠ 当前 `ExtractZipFile(const std::wstring& zipPath, ...)` **只接受文件路径**，
  需要加一个「从文件 + 偏移解压」的重载（`ReadTextFromZip` 同理）。
  P0 可先用"解到临时文件再走现有接口"的简化版，P1 再补偏移重载。
- 追加数据不影响 PE 执行；但**会破坏数字签名**——本项目 exe 当前未签名（PE Security Directory 为空），
  所以短期内无额外损失，长期要配合 §8 的签名方案。

### 6.2 为什么不走"运行时编译"

用户机器上不会有 MSVC / `rc.exe` / `link.exe`。即使有，版本与路径不可控。
7-Zip SFX 也需要外带 SFX 模块，且 SFX 特征更招杀软。**尾部追加是唯一同时满足"零工具链 + 单文件"的做法。**

### 6.3 需要新增 / 改动的文件

| 类型 | 文件 | 说明 |
|------|------|------|
| 新增 | `src/player/script_player_main.cpp` | player 入口：读尾部解包 → `qst::engine::Start()` → `RunScriptPath()` → 托盘 / 停止热键 |
| 新增 | `src/player/payload_append.h/.cpp` | 尾部追加与读取（magic + 偏移 + 长度），**纯逻辑可单测** |
| 新增 | `src/player/export_pack.cpp` | 导出侧：能力体检 → 依赖计划 → 组装 payload |
| 新增 | `src/player/ocr_winrt.cpp` | WinRT OCR 后端（输出 `OcrEngineOutput`，上层无感） |
| 新增 | `src/player/player_runtime.h/.cpp` | **导出模式与依赖解析**：定位已装软件目录、按模式准备 OpenCV / OCR、回退链与可读错误 |
| 新增 | `src/player/player_settings.cpp` | AI 模型配置（与 `app_settings.json` 的 `savedModels` 兼容） |
| 新增 | `tools/script_player_selftest.cpp` | 追加/解析往返、能力体检、模式解析（含注册表回退链）、错误路径 |
| 改动 | `CMakeLists.txt` | 新增 `QstScriptPlayer` 目标（`WIN32`，`/DELAYLOAD:opencv_world4100.dll`，链 `qst_engine` + `qst_desktop_tools` + `qst_engine_ui_stubs`） |
| 改动 | `src/utils.cpp/.h` | `ExtractZipFile` / `ReadTextFromZip` 增加"从偏移"重载 |
| 改动 | `src/ocr_engine.cpp/.h` | 抽出后端选择点（产品内保持 Python，player 用 WinRT） |
| 改动 | `src/webview/bridge_commands.h`、`webview_bridge_backend.cpp`、`ui/bridge.js`、`ui/app.js`、`ui/index.html` | 新命令 `exportScriptAsExe` + 导出报告弹窗 + 导出菜单 |
| 改动 | `tools/package_release.ps1`、`tools/package_with_version.ps1`、`installer/QuickScriptTool.iss` | 把 `player.exe` 模板随产品分发（放 `AppDir()\tools\player\`），保证 player 版本与主程序一致 |

> 模板放 `AppDir()\tools\player\`：导出时 `CopyFile` 再追加，不依赖网络、不需要编译器，
> 且 player 与主程序永远同版本。

---

## 7. 分期与验收

| 阶段 | 范围 | 验收判据 |
|------|------|---------|
| **P0**<br>纯逻辑脚本跑通 | 尾部追加 + player 骨架；**能力体检 + 导出设置对话框（含模式选择）**；只支持不依赖任何外部组件的脚本；托盘 + 停止热键 + 错误提示 | ① 用现有 12 个脚本中不含 `findImage`/`textRecognition`/`ai*` 的导出，在**另一台干净机器**（无本软件、无 VC Redist）双击跑通；② 无限循环脚本能按热键停止；③ `ScriptPlayerSelfTest` 覆盖追加/解析往返 + 能力体检 + 模式解析，进 CI 逻辑档 |
| **P1**<br>找图两模式 | 自带模式：OpenCV 内嵌 + 首启释放缓存；走软件模式：定位软件目录 + `LoadLibrary` + 回退链；窗口模式按需打包 `FakeFocus32/64.dll`；窗口模式导出前校验 `selectMethod` | ① 含 9 张模板图的脚本在**没装软件**的机器上（自带模式）跨机可跑；② 同一个 exe 在**装了软件**的机器上走软件模式体积不涨且结果一致；③ 找不到软件时给出可读原因而不是静默失败；④ 无 OpenCV 时不崩溃 |
| **P2**<br>OCR 两模式 + AI | WinRT OCR 后端；走软件模式接 `paddle_ocr_helper.py`；player 内 AI Key 配置引导 | ① `textRecognition` 脚本在干净机器上靠系统 OCR 跑通；② 装了 OCR 环境的机器上走软件模式结果与产品一致；③ 语言包缺失时提示可读；④ 未配 Key 的 AI 动作给出可读失败原因 |

**P0 的一个额外前置验证**：在干净 Windows 上确认 `Windows.Media.Ocr` 的语言包情况
（本机实测 `zh-Hans-CN` 可用；英文系统可能只有 `en-US`，需要在 player 里做语言回退）。

---

## 8. 风险与对策

| 风险 | 严重度 | 对策 |
|------|-------|------|
| **SmartScreen / 杀软拦截** | 🔴 高（**已接受**） | 已决定不做签名，因此只能靠行为降分：① 产物**不写注册表**（只读查询）、**不自启**、**不驻留服务**；② 首次运行给一次性说明；③ `README.txt` 写清"这是键鼠模拟工具，可能被安全软件提示，选择'仍要运行'即可"；④ 导出时提示用户"建议通过微信/QQ 直接发文件，别用网盘转发链接（网盘常被拦）"；⑤ 提前在 VirusTotal 过一遍，把误报反馈给厂商 |
| **反作弊 / 游戏封号** | 🔴 高 | 导出报告里明确列出"本脚本会注入目标进程（后台窗口模式）"；对已知内核反作弊目标直接**禁止导出**（复用 `LooksLikeKernelAntiCheatProtectedTarget`） |
| OCR 结果与软件内不一致 | 🟡 中 | 导出报告 + player 内的后端开关明示；`ocrDigitsOnly` 加白名单后处理 |
| 目标机缺 OCR 语言包 | 🟡 中 | `AvailableRecognizerLanguages` 探测 + 明确提示 + 语言回退链 |
| 分辨率 / 坐标错位 | 🟡 中 | 归一化坐标（`coordsAreNormalized`）已支持；**窗口相对坐标**在目标窗口大小不同时会错位 → 导出报告提示 |
| 老脚本格式不兼容 | 🟢 低 | player 直接链 `qst_engine`，自带 `MigrateScriptJson` / `MigrateScriptFileData` |
| player 与主程序版本漂移 | 🟢 低 | 模板随产品分发（`AppDir()\tools\player\`），且导出报告里记录 player 版本 |
| 单实例互斥体冲突 | 🟢 低 | 用 `QstScriptPlayer_<hash>`，不复用 `KeyMouse_SingleInstance` |

---

## 9. 已定决策（2026-09-19，不再需要讨论）

| 决策点 | 结论 |
|--------|------|
| 代码签名 | **不做，保持现状**。→ 产物必须：不写注册表（只读查询）、不自启、不驻留服务；首次运行给一次性说明；`README.txt` 里写清楚"这是键鼠模拟工具，可能被安全软件提示" |
| 默认行为 | **双击即跑**（常驻托盘 + 停止热键） |
| OCR 后端 | **导出时由用户选**：脚本含 `textRecognition` 时弹「自带（系统 WinRT OCR）／走软件（目标机已装软件的 OCR）」 |
| 找图 | **导出时由用户选**：脚本含找图/找色时弹「自带 OpenCV／走软件」 |
| 产物形态 | **单文件**，唯一形态 |

> 唯一还悬着的是"目标电脑装没装软件"这个总开关的**默认值**。建议默认「没装 / 不确定」= 全部自带
> —— 因为"发给别人"这个场景里，对方没装是常态，猜错了代价是"对方跑不起来"。

---

## 10. 实施状态（2026-09-19）

### 已落地

| 能力 | 状态 | 位置 |
|------|------|------|
| 脚本包依赖递归收集（嵌套脚本 + 图片 + 清单） | ✅ 完成 | `src/script_package.h/.cpp` |
| payload 尾部追加 / 读取（magic + offset + size + **FNV 哈希**） | ✅ 完成 | 同上 |
| 能力体检（找图 / OCR / AI / 窗口模式 / 外部程序 / 热键） | ✅ 完成 | `ScanActionCapabilities` / `ScanPackageCapabilities` |
| player 清单段（`needOpenCv` / `bundledOpenCv` / `needOcr` / `bundledOcr`） | ✅ 完成 | `PlayerManifest` |
| 已装软件目录定位（4 级注册表回退链 + 目录校验） | ✅ 完成 | `src/player/player_runtime.cpp` |
| OpenCV 两模式解析（自带 / 走软件）+ 可读失败原因 | ✅ 完成 | `ResolveRuntime` |
| 播放器目标（`QstPlayer.exe`，**4.79 MB**） | ✅ 完成 | `src/player/player_main.cpp` + CMake `QstScriptPlayer` |
| 自我迁移到 `%LOCALAPPDATA%\QstPlayer\rt\<hash>` | ✅ 完成 | `RelocateAndRelaunch` |
| 托盘 + F9 停止 + 双击托盘重跑 | ✅ 完成 | `TrayMenu` 复用 |
| `player.log` + `README.txt`（无界面程序的可诊断面） | ✅ 完成 | `PlayerLog` |
| 模板随产品分发到 `tools/player/` | ✅ 完成 | CMake POST_BUILD |
| 端到端实测 | ✅ **通过** | 见下 |

**端到端实测结论**：把测试脚本包追加到 `QstPlayer.exe` 尾部 → 双击 →
迁移、解包、起引擎、**脚本动作真实执行**（用 `runProgram` 写出的标记文件验证）、
托盘与日志正常。`QSTPKG01` 尾标的 FNV 哈希与 Python 侧实现逐位一致。

### P0 实施中踩到的三个真坑（都已修，且值得写进 skill）

1. **`CreateDirectoryW` 不建中间目录** —— `%LOCALAPPDATA%\QstPlayer\rt\<hash>` 的父级
   `QstPlayer\rt` 不存在时整条失败，播放器直接死在"无法创建运行时目录"。
   **与 `ExtractZipFile` 那条是同一类 bug**。已抽出公共的 `EnsureDirectoryTree()`（`src/utils.h`），
   两处都改用它 —— **以后不要再用裸 `CreateDirectoryW`**。

2. **路径未归一化 → 无限迁移循环**。`LOCALAPPDATA` 里可能出现 `//` 或尾斜杠，
   而 `GetModuleFileNameW` 返回规范形式；直接 `_wcsicmp` 会判定"不是同一个目录"，
   于是每次重启都认为还需要迁移 → **进程无限自繁殖**。
   修法：`NormalizePath()`（统一斜杠 + 折叠重复 + `GetFullPathNameW`）+ `SameDir()`；
   另外给子进程带 `--qst-no-relocate` 做**硬断路**，即使比较判错也不会失控。

3. **引擎把播放器注册成了浏览器扩展的原生消息宿主** —— `StartExtBridgeAlwaysOn()`
   → `RegisterExtNativeMessagingHost()` 用 `ExePath()` 写清单，于是播放器把自己写进了
   `HKCU\...\Edge\NativeMessagingHosts\com.quickscripttool.bridge`。
   浏览器随后以
   `QstPlayer.exe chrome-extension://<id>/ --parent-window=0` 反复拉起它当宿主，
   而播放器既不认这个调用、又去解析 payload → **实测 45 个实例同时存活**。
   修法两层：
   - 新增 `windowmode::SetExtNativeHostAllowed(false)`，播放器在 `engine::Start()` 前关掉，
     `StartExtBridgeAlwaysOn()` 与 `RegisterExtNativeMessagingHost()` 都早退
     （顺带满足"产物不写注册表"这条约束）；
   - 播放器在 `wWinMain` 最前面检测**管道 stdio / `--ext-native-host`** 并立刻 `return 0`。

   > 这条对"产物要干净"尤其重要：**播放器绝不能把自己注册成任何系统级宿主**。

### P1 已落地（2026-09-19 第二轮）

| 能力 | 状态 | 位置 |
|------|------|------|
| **导出为独立 EXE 的桥接命令**（能力体检 + 打包） | ✅ 完成 | `ScanScriptForExportJson` / `ExportScriptAsExe`（`webview_bridge_backend.cpp`） |
| 导出侧打包助手（zip / exe 共用一份） | ✅ 完成 | `BuildPackageZip` + `AddRuntimeEntry` |
| **导出对话框 UI**（先选方案 → 再看体检 + 选模式） | ✅ 完成 | `ov-export-kind` / `ov-export-exe`（`ui/index.html` + `ui/app.js`），**全部用既有主题变量与组件** |
| 专业模式右键导出走同一流程 | ✅ 完成 | `window.QstExport.open()`（`ui/pro-mode.js`） |
| **找图「自带」模式**：OpenCV 打进 payload 的 `rt\`，播放器释放到运行时目录根并加载 | ✅ 完成并实测 | `ExportScriptAsExe` + `MoveRuntimeDllsToRoot` + `LoadOpenCvFrom` |
| **OCR「自带」模式：系统 WinRT OCR 后端** | ✅ 完成并实测 | `src/ocr_winrt.cpp` + `src/ocr_backend.h`，接在 `RequestOcrOnHbitmap` 顶部 |
| OCR「走软件」模式（用已装软件的 Python 环境，失败回退系统 OCR） | ✅ 完成 | `OcrBackend::Auto` |
| **导出的 exe 可被本软件导入** | ✅ 完成 | `ImportScriptFile` 检测 `QSTPKG01` 尾标 → dump 成临时 zip → 复用既有 zip 导入（见 §2.5） |
| 播放器无托盘 / 跑完自退 / 中断脱离继续等 | ✅ 完成并实测 | `player_main.cpp` 的 `WaitUntilFinished` |
| 窗口模式兼容：`FakeFocus32/64.dll` 从 `rt\` 释放到运行时目录根 | ✅ 完成 | 注入器按 `ModuleDirectory()` 找 DLL，所以必须放根 |

**P1 实测记录**：

- 自带 OpenCV：导出的 exe **66.3 MB**，运行时目录根出现 `opencv_world4100.dll`，
  日志 `找图组件=自带；OpenCV=<运行时目录>\opencv_world4100.dll`，
  脚本跑完播放器自退。
- 系统 OCR：`OcrSelfTest` 新增 `winrt_ocr_reads_rendered_text` —— 用 GDI 渲染 `1234`，
  系统 OCR 识别结果为 `1234`（`{"ok":true,"detail":"识别到：1234"}`）。
- 自动退出：脚本以 `stopMacro` 收尾 → 日志 `脚本已结束（跑完或已停止），播放器退出`，
  进程 0 残留。
- 全量逻辑档自检 **21 suite / 21 通过 / 0 失败**（`OcrSelfTest` 11→15，`ScriptPackageSelfTest` 13→20）。

**P1 又踩到的两个坑（都已修）**：

1. **`LoadLibrary` 加载刚释放出来的 60+ MB DLL 会撞 `ERROR_SHARING_VIOLATION(32)`** ——
   杀软在扫描新写入的文件。一次性 `LoadLibrary` 会让"首次运行"**必失败**。
   已加退避重试（8 次 × 400ms，仅对 32/5 重试）。
2. **payload 里的 `rt\` 前缀不能直接解到 `scripts\`** —— 会变成 `scripts\rt\`，
   而注入器按 `ModuleDirectory()` 找 DLL、OpenCV 也从 exe 旁边加载，两者都找不到。
   已改成"解到 `_stage` 暂存目录 → 按前缀分发（`rt\` → 运行时根，其余 → `scripts\`）→ 删暂存"。

### P2 已落地（2026-09-19 第三轮）

| 能力 | 状态 | 位置 |
|------|------|------|
| **提示音**：启动 / 暂停 / 继续 / 结束，项目音优先、缺失回退系统音 | ✅ 完成并实测 | `resources/pause.wav`（新增）+ `PlayAppPauseSound()`；三个 wav 随 payload 的 `rt\` 分发 |
| **热键启停**：改用脚本自己配的热键（不再是写死的固定键） | ✅ 完成并实测 | `ReadScriptHotkey` / `StripScriptHotkey` / `HotkeyDown`（`player_main.cpp`） |
| 单击 = 暂停/继续；长按 ≥600ms = 停止退出 | ✅ 完成并实测 | `WaitUntilFinished` 的轮询式热键处理 |
| **引擎暂停能力** | ✅ 完成 | `playbackPaused_` + `EngineSetPlaybackPaused/EngineIsPlaybackPaused`，照 `debugPaused_` 的既有模式在动作边界等待；**产品内恒 false，零行为变化** |
| **导出默认走 EXE**，设置里可切回 zip | ✅ 完成 | `OtherTabSettings.exportScriptAsZip`（默认 false）+ 设置界面复选框；去掉导出方案选择弹窗 |
| **AI 动作可用**：密钥随 exe 明文打包 | ✅ 完成并实测 | `BuildPortableAiSettingsJson`（`qst_utils`）+ 3 条自检用例 |

**P2 实测记录（模拟按键端到端）**：

脚本配 `hotkeyVk = F6`（**不能用 F8 —— 那是产品默认的全局启停键，会先把脚本停掉**）：

```
[21:08:21] 脚本热键已生效（单击=暂停/继续，长按=停止）
[21:08:27] 已暂停（再按一次热键继续；长按热键则停止）      ← 单击
[21:08:29] 已继续                                          ← 再单击
[21:08:31] 热键长按：停止脚本                              ← 长按 1.2s
[21:08:31] 脚本已结束（跑完或已停止），播放器退出
```
三个提示音都出现在运行时目录根 ✓。

**P2 又踩到的两个坑（都已修）**：

1. **引擎会自己注册脚本热键**（`ghPlaybackScriptHooks`），运行中按它走**紧急停止**分支，
   抢在播放器的单击/长按判定之前把脚本停掉。修法：在 `engine::Start()` **之前**读出热键，
   然后把脚本里的热键字段抹成 0（`StripScriptHotkey`）。
2. **`CreateProcessW` 拉起刚 `CopyFileW` 出来的 exe 会撞杀软占用**
   （`ERROR_SHARING_VIOLATION` / `ERROR_ACCESS_DENIED`）——首次运行必失败。
   已加退避重试（10 次 × 500ms），与 `LoadLibrary` 那处同一类问题。

### P3 已落地（2026-09-19 第四轮）：设置快照 + 结束音修复

| 能力 | 状态 | 位置 |
|------|------|------|
| **导出时的全套设置打进 exe**（回放次数/间隔/倍速/性能/窗口模式/AI） | ✅ 完成并实测 | `SerializeAppSettings(settings, portableSecrets)`；导出侧写 `rt\app_settings.json` |
| **结束音不再被截断** | ✅ 完成 | `WavDurationMs()`（`src/utils.cpp`）+ 播放器退出前等待 |
| 密钥明文打包改为复用同一序列化器（不再单写 AI 段） | ✅ 完成 | `WriteAiApiSettings(file, s, protectKey)` |

**实测：约束矩阵**（每轮都往标记文件追加一行，数行数即轮数）

```
[A 有stopMacro+次数关]   实际轮数=1  自行退出=True  用时=2.7s  -> OK
[B 有stopMacro+次数=5]   实际轮数=1  自行退出=True  用时=2.6s  -> OK   ← stopMacro 先命中
[C 无stopMacro+次数=3]   实际轮数=3  自行退出=True  用时=3.4s  -> OK
[D 无stopMacro+次数关]   实际轮数=26 自行退出=False 用时=12.4s -> OK   ← 无限循环
```

**B 是关键**：`stopMacro` 与「次数 = 5」同时存在时只跑 1 轮就停 ——
证明两个约束是共同生效、`stopMacro` 没有被设置绕过。

**`AppSettingsStoreSelfTest` 新增 4 条用例**（`portable_settings_plaintext_key` /
`_roundtrip` / `_playback_roundtrip` / `_escapes`），其中 `_playback_roundtrip`
逐字段锁住回放次数、间隔区间、倍速、低性能模式、AI 加速、找图 GPU、
窗口模式四个开关与注入技术 —— 漏任何一个字段都会在这里变红。

### P4 已落地（2026-09-19 第五轮）：嵌套宏深度验证

| 能力 | 状态 | 位置 |
|------|------|------|
| 多层递归收集（≥3 层） | ✅ 已验证 | `ScriptPackageSelfTest::plan_multilevel_recursion` |
| **子脚本里**的引用也被改写 | ✅ 已验证 | `plan_rewrites_refs_inside_children` |
| 只用 `blockName` 引用 | ✅ 已验证 | `plan_blockname_only_ref` |
| 同名脚本条目去重 | ✅ 已验证 | `plan_unique_names_on_collision` |
| 导出的 exe 里多层嵌套 + 名字引用真能跑通 | ✅ 端到端实测 | `tools/verify/nested_macro_export.py` |

**本轮没有改动产品代码** —— 读代码 + 补测试 + 端到端验证，结论是**原本就是对的**，
但此前**没有测试锁住**，属于"随时可能被改坏而无人发现"的状态，现在锁住了。

### P5 已落地（2026-09-19 第六轮）：修掉一个会让进程当场死的真 bug

**用户报告**：`文字识别应用示例.json` 用软件跑正常，导出成 exe **两种模式都不能执行**；
且**没有结束提示音**，按热键也没有。而「等 1 秒按 A」的脚本两种模式都正常。

#### 根因链（四步，缺一不可）

```
① ActionNeedsOpenCv 漏了 textRecognition + ocrRegionByImage
      ⇒ 能力体检认为「不需要找图」（日志：找图组件=不需要）
② 于是 ResolveRuntime 两种模式都**不去加载 OpenCV**（走软件模式也跳过）
      ⇒ OpenCV 从未 LoadLibrary，但延迟加载桩仍然挂着
③ 脚本执行到「按图取 OCR 区域」→ resolveOcrRegion → LoadBitmapFromFile
      ⇒ 该函数内部有 cv::Mat（构造/析构 = OpenCV 符号）
④ 延迟加载桩找不到 opencv_world4100.dll
      ⇒ 抛 0xC06D007E ⇒ **进程当场死**
```

**为什么"没有结束提示音"**：进程是被硬杀的，引擎 worker 的收尾代码（播放结束音的地方）
**根本没跑到**，播放器的退出日志也没写。用户看到的现象就是"什么都没发生，也没有结束音"。

**为什么简单脚本正常**：它不碰 OpenCV，延迟加载桩永远不被调用。

**退出码 0xC06D007E** 是 MSVC delay-load 助手的 `VcppException(ERROR_MOD_NOT_FOUND)` ——
**只要看到这个码，就是"某个 /DELAYLOAD 的 DLL 不在，而代码仍然调了它的符号"**。

#### 三处修复

| # | 位置 | 改动 |
|---|------|------|
| 1 | `script_package.cpp::ActionNeedsOpenCv` | 补上 `textRecognition && ocrRegionByImage != 0` ⇒ **自带模式会真的打包 OpenCV**（产物 4.9MB → 69.6MB） |
| 2 | `image_match.cpp::LoadBitmapFromFile` | **在任何 `cv::Mat` 出现之前**加 `OpenCvAvailable()` 早退（退回 GDI `LoadImageW`）—— 没有 OpenCV 时不再触发延迟加载桩 |
| 3 | `engine_script_run.cpp::resolveOcrRegion` | 「按图取区域」前加 `OpenCvAvailable()` 守卫，没有 OpenCV 就老实失败，不硬着头皮往下走 |

另外给**播放器**补了两样主程序一直有、它却没有的东西：
- `__pfnDliFailureHook2` 延迟加载失败钩子（把失败原因写进 `player.log`）
- 启动期 `TryInitOpenCv()` 探测（把 OpenCV 状态定下来，日志打印「OpenCV 已就绪 / 不可用」）

#### 验证

```
自带：找图组件=自带；OCR=系统OCR；OpenCV=<运行时目录>\opencv_world4100.dll
      OpenCV 已就绪（找图可用） → 脚本已结束（跑完或已停止），播放器退出   ✓
走软件：找图组件=走软件；OCR=走软件；OpenCV=<软件目录>\opencv_world4100.dll
      OpenCV 已就绪（找图可用） → 脚本已结束（跑完或已停止），播放器退出   ✓
```

`ScriptPackageSelfTest` 新增 `scan_ocr_region_by_image_needs_opencv` 锁住第 ① 步。

#### 新增：命令行导出（排查 / 批处理用）

```
QuickScriptTool.exe --export-exe <脚本.json> <输出.exe> [--bundled]
```

`--bundled` = 「目标电脑没装软件」模式。结果同时写 `export_cli_result.txt`（GUI 子系统拿不到 stdout）。
这个入口是这次排查的产物 —— **没有它就没法在不点 UI 的情况下复现导出问题**。

#### 排查手法（值得复用）

1. **看退出码**：绕过启动器直接跑 `<runtimeDir>\QstPlayer.exe --qst-no-relocate`，
   `0xC06D007E` 一眼定位到 delay-load。
2. **看"有没有写退出日志"**：没有 ⇒ 进程被硬杀（不是正常结束）。
3. **加步骤日志**：`ocr_winrt.cpp` 的步骤日志改成 `QST_OCR_DEBUG=1` 才写，排查时打开即可。

### P5 已落地（2026-09-19 第六轮）：修掉一个会让进程当场死的真 bug

**用户报告**：`文字识别应用示例.json` 用软件跑正常，导出成 exe **两种模式都不能执行**；
且**没有结束提示音**，按热键也没有。而「等 1 秒按 A」的脚本两种模式都正常。

#### 根因链（四步，缺一不可）

```
① ActionNeedsOpenCv 漏了 textRecognition + ocrRegionByImage
      ⇒ 能力体检认为「不需要找图」（日志：找图组件=不需要）
② 于是 ResolveRuntime 两种模式都**不去加载 OpenCV**（走软件模式也跳过）
      ⇒ OpenCV 从未 LoadLibrary，但延迟加载桩仍然挂着
③ 脚本执行到「按图取 OCR 区域」→ resolveOcrRegion → LoadBitmapFromFile
      ⇒ 该函数内部有 cv::Mat（构造/析构 = OpenCV 符号）
④ 延迟加载桩找不到 opencv_world4100.dll
      ⇒ 抛 0xC06D007E ⇒ **进程当场死**
```

**为什么"没有结束提示音"**：进程是被硬杀的，引擎 worker 的收尾代码（播放结束音的地方）
**根本没跑到**，播放器的退出日志也没写。用户看到的现象就是"什么都没发生，也没有结束音"。

**为什么简单脚本正常**：它不碰 OpenCV，延迟加载桩永远不被调用。

**退出码 0xC06D007E** 是 MSVC delay-load 助手的 `VcppException(ERROR_MOD_NOT_FOUND)` ——
**只要看到这个码，就是"某个 /DELAYLOAD 的 DLL 不在，而代码仍然调了它的符号"**。

#### 三处修复

| # | 位置 | 改动 |
|---|------|------|
| 1 | `script_package.cpp::ActionNeedsOpenCv` | 补上 `textRecognition && ocrRegionByImage != 0` ⇒ **自带模式会真的打包 OpenCV**（产物 4.9MB → 69.6MB） |
| 2 | `image_match.cpp::LoadBitmapFromFile` | **在任何 `cv::Mat` 出现之前**加 `OpenCvAvailable()` 早退（退回 GDI `LoadImageW`）—— 没有 OpenCV 时不再触发延迟加载桩 |
| 3 | `engine_script_run.cpp::resolveOcrRegion` | 「按图取区域」前加 `OpenCvAvailable()` 守卫，没有 OpenCV 就老实失败，不硬着头皮往下走 |

另外给**播放器**补了两样主程序一直有、它却没有的东西：
- `__pfnDliFailureHook2` 延迟加载失败钩子（把失败原因写进 `player.log`）
- 启动期 `TryInitOpenCv()` 探测（把 OpenCV 状态定下来，日志打印「OpenCV 已就绪 / 不可用」）

#### 验证

```
自带：找图组件=自带；OCR=系统OCR；OpenCV=<运行时目录>\opencv_world4100.dll
      OpenCV 已就绪（找图可用） → 脚本已结束（跑完或已停止），播放器退出   ✓
走软件：找图组件=走软件；OCR=走软件；OpenCV=<软件目录>\opencv_world4100.dll
      OpenCV 已就绪（找图可用） → 脚本已结束（跑完或已停止），播放器退出   ✓
```

`ScriptPackageSelfTest` 新增 `scan_ocr_region_by_image_needs_opencv` 锁住第 ① 步。

#### 新增：命令行导出（排查 / 批处理用）

```
QuickScriptTool.exe --export-exe <脚本.json> <输出.exe> [--bundled]
```

`--bundled` = 「目标电脑没装软件」模式。结果同时写 `export_cli_result.txt`（GUI 子系统拿不到 stdout）。
这个入口是这次排查的产物 —— **没有它就没法在不点 UI 的情况下复现导出问题**。

#### 排查手法（值得复用）

1. **看退出码**：绕过启动器直接跑 `<runtimeDir>\QstPlayer.exe --qst-no-relocate`，
   `0xC06D007E` 一眼定位到 delay-load。
2. **看"有没有写退出日志"**：没有 ⇒ 进程被硬杀（不是正常结束）。
3. **加步骤日志**：`ocr_winrt.cpp` 的步骤日志改成 `QST_OCR_DEBUG=1` 才写，排查时打开即可。

### P6 已落地（2026-09-19 第七轮）：全量依赖矩阵审计

用户要求：「逐个对一遍，确认体检规则和实际依赖完全对齐」。
做法：把引擎回放路径上**所有** OpenCV / OCR / AI / 外部程序触点逐个追到动作类型上，
再与 `ActionNeedsOpenCv` / `ActionNeedsOcr` / `ActionNeedsAi` / `ActionIsExternal` 逐格比对。

#### 审计方法

1. 在 `engine_script_run.cpp` 里 grep 出所有 OpenCV 触点
   （`LoadBitmapFromFile` / `FindTemplate*` / `ImReadW` / `MatToHBitmap` / `cv::`），
   再用「往上找最近的 `ActionType::`」把每个触点归到动作类型；
2. 对每个有疑问的类型，追到实际实现（如 `BitmapToBase64Jpeg` 里有没有 `cv::imencode`）；
3. 把结论固化成 `ScriptPackageSelfTest::scan_dependency_matrix` —— **44 种动作 + 5 个条件分支**
   逐格断言，任一格对不上就变红并打印具体差异。

#### 本轮又查出的两处漏判（都已修）

| 漏判 | 后果 | 依据 |
|------|------|------|
| **`aiImageAnalysis` / `aiActionExecute`** 没算需要 OpenCV | ① `aiRegionByImage`+`aiTargetImagePath` 会走 `LoadBitmapFromFile`；② **发截图给模型**走 `BitmapToBase64Jpeg` → `cv::MatFromBitmap` / `cv::resize` / `cv::imencode`（**无条件**）。没 OpenCV 时它 `return {}`，AI **拿不到任何图**，动作以"截图编码失败"告终 | `engine_script_run.cpp:1313/6804`、`ai_action_service.cpp:561-585` |

> `BitmapToBase64Jpeg` 本身有 `OpenCvAvailable()` 守卫，所以不会崩 ——
> 但**功能必然失败**，而且失败信息是"截图编码失败"，很难联想到"导出时没带 OpenCV"。
> 这类"不崩但静默失效"比崩溃更难查，所以一样要算进体检。

#### 全量依赖矩阵（审计结论）

| 分类 | 动作 | 找图 | OCR | AI | 外部 |
|------|------|:---:|:---:|:--:|:----:|
| 纯输入 | moveMouse / moveMouseRelative / mouseDown / mouseUp / mouseClick / keyDown / keyUp / keyClick / hotkeyShortcut / quickInput / scrollWheel / wait / customText | | | | |
| 流程控制 | loop / endLoop / defineBlock / runBlock / if / else / goto / stopMacro / varCompute / timerRecordTime / getCursorPos / lockScreenshot / unlockScreenshot / activateWindow | | | | |
| 嵌套 | runMacro / mousePlayback（本体不依赖，靠**递归收集子脚本**的能力） | | | | |
| 找图族 | findImage / watchImage / multiMatch / findColor / colorMatch | ✅ | | | |
| 条件找图 | mouseDrag（`imageLocate=1`）/ getColor（`imageLocate=1`） | ✅ | | | |
| OCR | textRecognition（`ocrRegionByImage=0`） | | ✅ | | |
| OCR+找图 | textRecognition（`ocrRegionByImage=1`） | ✅ | ✅ | | |
| AI | aiTextAnalysis（纯文本，不碰图） | | | ✅ | |
| AI+找图 | aiImageAnalysis / aiActionExecute | ✅ | | ✅ | |
| 外部程序 | runProgram / openFile / openWebpage | | | | ✅ |
| 其它 | closeProgram（只按进程名关窗口，不依赖随包文件） | | | | |

#### 验证

- 矩阵用例先跑通；再做 **A/B**（故意把 `aiImageAnalysis` 的期望改成 0）确认它会变红并打印
  `aiImageAnalysis 期望(找图=0 …) 实际(找图=1 …)` —— **测试是活的，不是静默通过**。
- AI 脚本实测：`AI动作执行测试.json` 自带模式 `usedOpenCv:1`（69.6MB）、
  `OpenCV 已就绪`、脚本正常运行不崩；走软件模式 `usedOpenCv:0`（4.96MB）。
- **21 suite / 21 通过 / 0 失败（720 用例）**。

#### 维护约定

**加新动作、或给动作加新的图片字段时，必须同时改两处**：
`ActionNeedsOpenCv`（或对应判定）+ `scan_dependency_matrix` 的表。
只改一处的话，矩阵会立刻变红 —— 这正是它存在的意义。

### P7 已落地（2026-09-20）：OCR 类脚本「软件里好、exe 里不行」的两个真因

用户反馈：声音正常了、exe 体积也变大了（OpenCV 已打进），但 `文字识别应用示例.json`
**两种模式仍然不行**；而「等 1 秒按 A」的简单脚本一直正常。

脚本逻辑是「按图取区域 → OCR 读血量数字 → 小于阈值就按键」，**结果完全取决于 OCR 读出什么**。

#### 真因 ①：「走软件」模式的 OCR 后端顺序搞反了

`OcrBackend::Auto` 原本是「**先试系统 OCR**，失败才回退 Python」。
但 `Auto` 对应的是导出的**「走软件」模式** —— 用户期望和软件里跑出**一样**的结果，
而软件里用的就是 PaddleOCR。系统 OCR 一旦成功，就**永远不会用软件里的引擎**。

于是两种模式实际上都在用 Windows OCR：
- 自带模式 → `WinRt`
- 走软件模式 → `Auto` → 先 WinRT → 成功 → **也变成 WinRT**

**修法**：`Auto` 改成 **Python 优先**，Python 不可用才回退系统 OCR。
并把「尝试顺序」抽成纯函数 `OcrBackendAttemptOrder(pref)`，自检直接断言：

| 偏好 | 尝试顺序 | 用途 |
|------|---------|------|
| `Python` | `{Python}` | 产品内默认（**与引入本层之前完全一致**） |
| `WinRt` | `{WinRt}` | 导出「自带」模式 |
| `Auto` | `{Python, WinRt}` | 导出「走软件」模式（**Python 优先**） |

#### 真因 ②：WinRT OCR 对中文会**逐字加空格**

实测日志：`囗 Q 关 于 （ A 〕 WorkBuddy AI 5 ． 5 ． 2 ． 新 建 任 务 助 理 …`

WinRT 把**每个汉字**当成一个 word，`OcrLine.Text()` 于是用空格把它们拼起来。
这会让下游的文本匹配、`numbers()` 取数、找字**全乱** ——
表现就是"软件里能识别、导出的 exe 就是不行"。

**修法**：不用 `OcrLine.Text()`，改成自己按词拼（`JoinOcrWords`）：
**只要有一侧是 CJK 就不加空格**，两侧都不是才加。这样
`新建任务` 紧凑、`HP 45` 保留空格、`生命HP45` 也紧凑（找字/取数都更稳）。

修后实测：`思考列车已出站，请系好安全带` ✓（不再逐字带空格）。

#### 附带：OCR 诊断日志

新增 `SetOcrDiagnosticSink()`，播放器把它接到 `player.log`。每次 OCR 完成写一行：

```
OCR[系统OCR] 成功，读到 71 行：囗Q 7关于（A〕 思考列车已出站，请系好安全带 …
OCR[软件引擎] 失败：Python 运行环境不可用
```

**这类脚本"不生效"时，最难判断的就是到底有没有识别到、识别成了什么** ——
没有这行日志只能靠猜。现在用户自己就能看到。

#### 验证

- `OcrSelfTest` 新增 3 条：`backend_attempt_order`、`join_ocr_words_cjk_no_spaces`、
  `join_ocr_words_latin_keeps_spaces`（15→18）。
- 端到端：构造「全屏 OCR → 写标记 → 停止」的 exe，`QST_OCR_DEBUG=1` 跑：
  WinRT OCR 在**引擎工作线程**上步骤全绿、脚本跑完、标记文件出现、进程正常退出。
- **21 suite / 21 通过 / 0 失败（723 用例）**。

### 仍未落地

- **虚拟 HID**：需要内核驱动，导出时只能给前置条件说明。
- **端到端真机验证**：把产物拿到**另一台干净机器**上跑（当前验证都在本机，靠
  `LOCALAPPDATA` 重定向模拟隔离环境）。
- **`ExtractZipFile` / `ReadTextFromZip` 的"从偏移"重载**：现在多一次 dump 到临时 zip 的拷贝。
  对 66 MB 的产物是可感知的（约几百毫秒）。
- **暂停只在动作边界生效**：单个长 `wait` 动作不会被打断（与调试单步同一取舍）。
  要"立刻暂停"得让 `SleepInterruptible` 也看 `playbackPaused_`。
- **`foregroundInputBackend = VirtualHid` 在目标机不可用**（需内核驱动）——
  快照会如实带上这个值，但目标机没装驱动时该后端会失败并回退。

## 附：本轮已完成的代码改动

| 文件 | 改动 |
|------|------|
| `src/script_package.h` / `.cpp` | **新增**。纯逻辑：`CollectNestedRefsFromJson` / `RewriteNestedTargetPaths` / `BuildPackagePlan` / `BuildPackageManifestJson` / `ParsePackageManifestJson` / `SanitizeEntryFileName` |
| `src/webview/webview_bridge_backend.cpp` | `ExportScriptFile` 改为按依赖计划打包（含嵌套脚本 + 清单 + `missingRefs`）；`ImportScriptFile` 还原嵌套脚本并改写 `targetPath`；图片映射统一收集后应用到所有脚本 |
| `src/webview/webview_bridge_backend.h` | `ExportScriptFile` 增加 `outMissingRefsJson` |
| `src/webview/qst_webview_shell.cpp` | `exportScript` 结果 JSON 带上 `missingRefs` |
| `src/utils.cpp` | 新增 `EnsureParentDirectory`，`ExtractZipFile` 自建中间目录 |
| `ui/app.js` | 导出结果提示补上"引用的子脚本找不到，未打包" |
| `tools/script_package_selftest.cpp` | **新增** 13 个用例 |
| `CMakeLists.txt` | `qst_utils` 加入 `script_package.cpp`；新增 `ScriptPackageSelfTest` |
| `tools/run_all_selftests.ps1`、`.github/workflows/build.yml`、`AGENTS.md`、`.cursor/skills/module-selftest/SKILL.md` | 注册新 suite；顺带修掉 CI 从未构建 `TimeScaleSelfTest` 的既有缺口 |

**验证**：`run_all_selftests.ps1 -SkipBuild -Tier logic` → **21 个 suite / 21 通过 / 0 失败 / exit 0**；
`QstWebViewShell` 构建通过，无新增警告。

### P1 新增 / 改动的文件

| 文件 | 改动 |
|------|------|
| `src/ocr_backend.h` | **新增**。`OcrBackend` 枚举 + `SetOcrBackendPreference` / `WinRtOcrAvailable` / `RunWinRtOcr` |
| `src/ocr_winrt.cpp` | **新增**。C++/WinRT 调 `Windows.Media.Ocr`：HBITMAP → BGRA8 `SoftwareBitmap` → `RecognizeAsync` → 行框（词框并集）。**alpha 必须补成 255**（截图 alpha 常为 0，否则识别不出任何东西） |
| `src/ocr_engine.cpp` | `RequestOcrOnHbitmap` 顶部接后端选择：`Python` 保持原样；`Auto` 先试系统 OCR 再回退；`WinRt` 失败即返回可读原因 |
| `src/player/player_runtime.cpp` | `LoadOpenCvFrom` 加**退避重试**（杀软占用刚释放的 DLL 会报 32）；`ResolveRuntime` 支持 FakeFocus 与两种 OCR 来源 |
| `src/player/player_main.cpp` | 去托盘、`WaitUntilFinished` 跑完自退、`rt\` 前缀分发到运行时根、`--qst-no-relocate` 断路、`player.log` |
| `src/window_mode/ext_bridge/ext_bridge_server.h` / `ext_native_host.cpp` / `engine_host_window.h` | 新增 `SetExtNativeHostAllowed` 开关（播放器必须关掉，否则被浏览器反复拉起） |
| `src/webview/webview_bridge_backend.cpp/.h` | `BuildPackageZip` 抽出共用；新增 `ScanScriptForExportJson` / `ExportScriptAsExe`；`ImportScriptFile` 支持 `.exe` |
| `src/webview/bridge_commands.h` / `qst_webview_shell.cpp` | 新命令 `scanScriptForExport` / `exportScriptAsExe` |
| `ui/index.html` / `ui/app.js` / `ui/bridge.js` / `ui/pro-mode.js` | 导出方案选择 + EXE 设置对话框（沿用既有主题变量与组件） |
| `tools/ocr_selftest.cpp` | 新增 4 个用例（后端偏好 + WinRT 探测 + **渲染文字实测**） |
| `CMakeLists.txt` | 新增 `QstScriptPlayer` 目标（产物 `QstPlayer.exe`，POST_BUILD 复制到 `tools/player/`）；`script_core_common` 加 `ocr_winrt.cpp` 与 `windowsapp`（PUBLIC） |

### 播放器体积

| 形态 | 体积 |
|------|------|
| `QstPlayer.exe` 模板 | **4.79 MB** |
| 纯输入/逻辑脚本产物 | ≈ 4.8 MB |
| + 自带 OpenCV | **66.3 MB**（实测） |
| 主程序 `QuickScriptTool.exe` 对照 | 6.18 MB |



### P1 新增 / 改动的文件

| 文件 | 改动 |
|------|------|
| `src/ocr_backend.h` | **新增**。`OcrBackend` 枚举 + `SetOcrBackendPreference` / `WinRtOcrAvailable` / `RunWinRtOcr` |
| `src/ocr_winrt.cpp` | **新增**。C++/WinRT 调 `Windows.Media.Ocr`：HBITMAP → BGRA8 `SoftwareBitmap` → `RecognizeAsync` → 行框（词框并集）。**alpha 必须补成 255**（截图 alpha 常为 0，否则识别不出任何东西） |
| `src/ocr_engine.cpp` | `RequestOcrOnHbitmap` 顶部接后端选择：`Python` 保持原样；`Auto` 先试系统 OCR 再回退；`WinRt` 失败即返回可读原因 |
| `src/player/player_runtime.cpp` | `LoadOpenCvFrom` 加**退避重试**（杀软占用刚释放的 DLL 会报 32）；`ResolveRuntime` 支持 FakeFocus 与两种 OCR 来源 |
| `src/player/player_main.cpp` | 去托盘、`WaitUntilFinished` 跑完自退、`rt\` 前缀分发到运行时根、`--qst-no-relocate` 断路、`player.log` |
| `src/window_mode/ext_bridge/ext_bridge_server.h` / `ext_native_host.cpp` / `engine_host_window.h` | 新增 `SetExtNativeHostAllowed` 开关（播放器必须关掉，否则被浏览器反复拉起） |
| `src/webview/webview_bridge_backend.cpp/.h` | `BuildPackageZip` 抽出共用；新增 `ScanScriptForExportJson` / `ExportScriptAsExe`；`ImportScriptFile` 支持 `.exe` |
| `src/webview/bridge_commands.h` / `qst_webview_shell.cpp` | 新命令 `scanScriptForExport` / `exportScriptAsExe` |
| `ui/index.html` / `ui/app.js` / `ui/bridge.js` / `ui/pro-mode.js` | 导出方案选择 + EXE 设置对话框（沿用既有主题变量与组件） |
| `tools/ocr_selftest.cpp` | 新增 4 个用例（后端偏好 + WinRT 探测 + **渲染文字实测**） |
| `CMakeLists.txt` | 新增 `QstScriptPlayer` 目标（产物 `QstPlayer.exe`，POST_BUILD 复制到 `tools/player/`）；`script_core_common` 加 `ocr_winrt.cpp` 与 `windowsapp`（PUBLIC） |

### 播放器体积

| 形态 | 体积 |
|------|------|
| `QstPlayer.exe` 模板 | **4.79 MB** |
| 纯输入/逻辑脚本产物 | ≈ 4.8 MB |
| + 自带 OpenCV | **66.3 MB**（实测） |
| 主程序 `QuickScriptTool.exe` 对照 | 6.18 MB |


