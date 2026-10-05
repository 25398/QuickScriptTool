# Agent notes (QuickScriptTool)

## UI 方向（WebView2 壳）

产品 UI 目标改为与 `docs/ui-redesign-mockup.html` 1:1，采用 **WebView2 壳 + 现有 C++ 引擎**（禁止 Electron 整仓重写；禁止 WebView 内 SendInput）。

- **产品 = Web**：CMake 目标 `QstWebViewShell` 输出 **`QuickScriptTool.exe`**（`ui/` + `src/engine/qst_engine.h` + `src/desktop_tools/`）；本地入口 `build\Release\QuickScriptTool.exe`
- **永久原生（DesktopTools）**：选区 / 找图叠层 / OCR 叠层 / 准星 / 启停·脚本热键捕获 / 托盘 — **禁止**塞进 WebView 冒充
- Phase-1 壳：`src/webview/qst_webview_shell.cpp`；引擎：`src/engine/engine_runtime.cpp` + `engine_host_window.h`（headless；无 GDI 产品 exe）
- 分层库：`qst_desktop_tools` / `qst_engine`（OBJECT，`cmake/QstProductLibs.cmake`）
- 前端：`ui/index.html` + `shell.css` + `bridge.js`
- **Fixed Version 便携**：[`docs/webview-fixed-runtime.md`](docs/webview-fixed-runtime.md)
- Bridge / 分期：[`docs/webview-bridge-api.md`](docs/webview-bridge-api.md)
- 原生分层盘点：[`docs/webview-native-layering.md`](docs/webview-native-layering.md)
- ⚠⚠ **桥消息处理跑在 UI 线程上**：`HandleBridgeMessage` 是在 WebView2 的 `WebMessageReceived`
  回调里**同步**执行的 ⇒ 里面做几百毫秒的重活（读大文件 / 解析 / 编解码）会把**整个界面冻住**，
  连骨架屏的 CSS 动画都停摆（用户看到的就是「卡住不动」）。**重活一律挪后台线程**，
  照 `installOcr` 的套路：`std::thread` + `PostToJsAsync`（`WM_BRIDGE_POST_JS`）。
  ⚠ 后台线程要碰全局状态时必须 `std::mutex` 串行化，并用 `std::atomic` 序号**丢弃过期结果**
  （用户连开两次时，先发起的可能后返回）。⚠ 配套的 JS 侧要能处理「结果晚到」——
  若面板/页面已被关掉，直接丢弃，别对着关掉的界面继续跑收尾逻辑。
  前科与实测：录制优化列表打开 [`docs/recording-optimize-performance.md`](docs/recording-optimize-performance.md)。
- ⚠ **别凭直觉优化「桥载荷太大」**：录制优化列表实测过——列式编码能把载荷 14.4MB 压到 0.95MB，
  但 JS 侧 `JSON.parse` 省的 21ms 会被「物化 5881×54 个属性」的 20ms 抵消（打平），
  还要搭上字段同步风险。**先分段量化再动手**：`build\Release\ScriptIoSelfTest.exe --bench <脚本.json>`。
- ⚠ **列表斑马纹的选择器必须限定在自己的容器内**（如 `.action-list .arow:nth-child(even)`）。
  写成全局 `.arow:nth-child(even)` 特异性 (0,4,0) 会压过别的列表的 tone 类 (0,3,0)，
  虚拟滚动下行的底色就由「它在窗口 DOM 里的位置」决定 ⇒ **同一行滚动时不断换色**。
- ⚠⚠ **已经解析好的 DOM 必须复用；`ExtractNamedJsonObject` / `GetSubObjectText` 是「整份重解析」。**
  它们内部是 `nlohmann::json::parse(ToUtf8(整份))` ⇒ **键存不存在都要先付一次全量解析**
  （14.4MB 实测 **185~190ms**）。前科：`LoadScriptFileData` 已解析过 `JC`，却又为 `visualLayout`
  整份重解析一次，而**该键压根不存在**。⇒ 根已解析时一律走 `SubObjectFromParsedRoot(root, key)`
  （只 `find` + `dump` 子树，输出与 `GetSubObjectText` 严格一致）。
  ⚠ 见到这两个函数就**先问「根是不是已经解析过了」**。
  ⚠ `nestedWindowMode` 那处只对 `RunMacro`/`MousePlayback` 触发 ⇒ **基准数据里一次都不跑**，
  **基准全绿 ≠ 这类重复劳动不存在**；要按「模式」搜代码，别只信基准。
  ⚠ `webview_bridge_backend.cpp` 的 `SaveScript` 有**同类**问题（**未修**，写用户数据的路径
  风险等级不同，复用 DOM 需块与下标严格对应，静默错位就是存错脚本）。
  ⚠ **完整剖析 + 复现命令见 [`docs/recording-optimize-performance.md`](docs/recording-optimize-performance.md)**；
  改了任何解析/序列化路径都顺手跑一次 `build\Release\ScriptIoSelfTest.exe --bench <脚本.json>`。
- 打包：`powershell -File tools\package_webview_portable.ps1` → `dist\QstWebViewShell-Portable.zip`；完整发版 `tools\package_release.ps1`（主产物 Shell）
- **一键发版**：仓库根 `release.cmd 1.3.4 [-Mode zip]`（纯 ASCII，自带 `cd /d "%~dp0"`），
  等价于 `tools\package_with_version.ps1 -Version 1.3.4` —— 写版本号（**7 处**）→ 构建 →
  组装 dist + 便携 zip → 编安装包 → 同步 `website\downloads` → **同步 website 前端（见下）**。
  `-Mode all|zip|setup`（默认 `all`；`zip` 跳过 ISCC）；`-DryRun` / `-SkipBuild` / `-SkipInstaller`。
  版本号用正则写入、构建目标从 `package_release.ps1` 解析、打包逻辑复用该脚本 ⇒ 新增源码/UI/Skill 都不用改它。
  **需在普通 PowerShell / CMD 终端运行**（受限宿主禁止启动 cmake/ISCC）；
  直接调 PowerShell 时**必须给绝对路径**（相对路径报「`-File` 形式参数不存在」）。
- **官网前端一键同步**：仓库根 `sync_web.cmd`（可直接双击）→ `tools\sync_website.ps1`，
  把 `ui\` 同步进 `website\demo\`、把 `QstPlayer.exe` 同步进 `website\export\player\`，
  再对 demo JS 跑 `node --check` 守卫；`-Check` 只校验（不一致 exit 1）、`-Serve` 顺带起本地预览。
  ⚠ **发版链里已经强制调用它**（`package_release.ps1` 同步 `website\downloads` 之后），
  失败即中止发版；**手动同步与发版同步是同一条代码路径**，别在发版脚本里另写一份拷贝逻辑。
  ⚠ 同步会**覆盖** `website\demo\` 下所有 js/css/html，所以 Demo 侧只允许改两个官网独有文件
  （`bridge.stub.js` / `agent-inline.js`）—— 其余手改下次同步就丢。
- 发版版本：`tools\product_version.txt`（须同步 `installer\QuickScriptTool.iss` 的 `MyAppVersion`、
  `src\app_branding.cpp`、以及 `resources\QuickScriptTool.rc` 的 FILEVERSION / PRODUCTVERSION / FileVersion / ProductVersion）
- 发版依赖：运行必需项（MSVC CRT 旁路、`WebView2Fixed`、`ui`、OpenCV、扩展、驱动**安装脚本**）必须进 zip/安装包；`interception.dll` / `.sys` 不进默认包（设置里下载 `QuickScriptTool-HidDriver.zip`）；OCR Python 可运行时再装

## 导出为独立 EXE（脚本播放器）

把脚本导出成「发给别人就能双击跑」的单文件 exe。设计见
**[`docs/script-to-exe-design.md`](docs/script-to-exe-design.md)**，可行性评估见
[`docs/script-to-exe-feasibility.md`](docs/script-to-exe-feasibility.md)**。

- **网页版（在线导出）** 见 **[`docs/web-online-export.md`](docs/web-online-export.md)** ——
  官网 `website/export/` 与 Demo 的「导出为独立 EXE」都在**浏览器里**拼同一个格式，
  服务器只发一个 5.5 MB 静态模板（0 CPU / 0 临时文件 / 用户数据不落盘）。
  ⚠ 模板 `website\export\player\QstPlayer.exe` **不入 git**（`*.exe` 规则，与 `downloads/` 同策略），
  靠 `tools\sync_web_export_player.ps1`（或发版）补；部署后开 `export/selftest.html` 验证。
  ⚠ 改 payload/清单/坐标语义时**两边一起改**：格式定义方是 `src/script_package.*` 与
  `src/coord_space.cpp`；离线校验 `node tools\verify\web_export_roundtrip.js`。

- 运行时模板：CMake **`QstScriptPlayer`** → `QstPlayer.exe`（**不含 WebView2 / ui/**），
  POST_BUILD 复制到 `build\Release\tools\player\` 随产品分发
- 格式：`[player.exe][脚本包 zip][32 字节尾标]`，尾标 = magic `QSTPKG01` + offset + size + FNV 哈希；
  **导出 = 复制模板 + 追加数据**，纯文件 IO，目标机不需要编译器。依赖收集/体检/尾标读写
  `src/script_package.h/.cpp`（有 `ScriptPackageSelfTest`）；运行端 `src/player/`。
  **导出的 exe 可被本软件导入**（payload 是标准 zip：检测尾部 `QSTPKG01` → dump 成临时 zip
  → 复用既有导入路径，`rt\` 忽略）。
- **导出模式**：找图/OCR 各可选「自带」或「走软件」。找图自带 = OpenCV 进 payload 的 `rt\`
  （约 66 MB）；走软件 = 从已装目录 `LoadLibrary`。OCR 自带 = **系统 WinRT OCR**
  （`src/ocr_winrt.cpp`，零安装）；走软件 = 已装 Python。
  ⚠ **`Auto`（走软件）必须 Python 优先**，不可用才回退系统 OCR（`OcrBackendAttemptOrder()`）
  —— 用户期望"走软件"和软件内一致，反了就用不上软件里的引擎。
  ⚠ **WinRT 对中文逐字给 word**（「新 建 任 务」）⇒ 必须 `JoinOcrWords()` 拼（有 CJK 不加空格）。
- 播放器跑完/被停止后**自己退出**，不常驻托盘；**唯一例外**是「中断脱离」（那时 `running_` 仍 true）。
- **控制热键 = 脚本自己配的那个**（`hotkeyVk`/`hotkeyModifiers`）：单击=暂停/继续，长按≥600ms=停止，
  没配就回退 F9。⚠ **必须在 `engine::Start()` 之前读出并抹掉**（`StripScriptHotkey`），
  否则引擎会把它注册成回放钩子 ⇒ 运行中按它走**紧急停止**，抢在播放器判定之前停掉脚本。
- **提示音**：`startup/pause/finish.wav` 走 `PlayWavOrFallbackBeep`（随 `rt\` 分发）；⚠ `SND_ASYNC`
  播的，**退出前要等它放完**（`WavDurationMs()` + Sleep）—— 实测"结束音只响一半"。
- **设置快照**：导出时把**整份** `app_settings.json` 打进 `rt\`（播放器释放到运行时目录 ⇒ 引擎按
  `AppDir()\app_settings.json` 读）⇒ **exe 与软件内一致**。⚠ **必须复用
  `SerializeAppSettings(settings, portableSecrets)`**，别另写"便携版"（漏一个字段就是**静默**差异）；
  `portableSecrets=true` 让 apiKey 不 DPAPI 加密（DPAPI 是**用户级**的，换机器解不开 ⇒ AI 静默失败）。
- **OCR 诊断**：`SetOcrDiagnosticSink()` 把每次 OCR 的后端/成败/内容写进 `player.log`（这类脚本
  "不生效"先看这行）。⚠ **`stopMacro` 与「回放次数」是共同约束**（谁先命中谁生效），**不要**把
  `stopMacro` 当多余的删掉。改完 worker/播放器/设置快照后跑 `tools\verify\player_behavior_matrix.py`。
- ⚠⚠ **`/DELAYLOAD` 的 DLL 缺失时，只要代码碰到它的符号就抛 `0xC06D007E` 把进程当场带走**
  （没日志、没结束音）⇒ **任何调用 OpenCV 的地方前面必须有 `OpenCvAvailable()` 守卫**，
  且守卫要在**任何 `cv::Mat` 出现之前**（`cv::Mat` 的构造/析构本身就是 OpenCV 符号）。
  **看到退出码 `0xC06D007E` 就是这一类问题。**
- **能力体检漏判 = 导出缺组件 = 运行期崩/静默失效**（已漏过两处：`textRecognition +
  ocrRegionByImage`；`aiImageAnalysis`/`aiActionExecute` —— 后者**无条件**要 OpenCV）。
  ⚠ **加新动作/给动作加图片字段时必须同时改 `ActionNeedsOpenCv` 与
  `ScriptPackageSelfTest::scan_dependency_matrix` 的表**（44 动作 + 5 条件分支逐格断言），
  并对照 `CollectImagePathsFromJson`，别只改一处。
- **嵌套宏导出后能在别人电脑上跑通**：`BuildPackagePlan` 是 BFS，引用改写成裸文件名，引擎
  `ResolveLibraryScriptPath` 递归搜 `ScriptsDir()`。⚠ **嵌套宏里不要放 `stopMacro`**（它终止
  **整次运行**）。改完跑 `tools\verify\nested_macro_export.py`。
- **命令行导出**：`QuickScriptTool.exe --export-exe <脚本.json> <输出.exe> [--bundled]`。
  ⚠ 播放器**不得**注册成浏览器扩展原生宿主、**不得**写注册表；路径比较前必须归一化。

## 输入接管红线（叠层 / 捕获 / 屏外停放）

**总原则：任何「我们接管了用户输入」的状态都必须能被我们主动收尾 —— 绝不把用户的键鼠
控制权留在自己手里。** 本地还能靠 Esc / 点一下自救，**远控（QQ 远程协助 / ToDesk /
向日葵…）下几乎没有别的入口**，所以这类缺陷只在远控时被报上来。

事故形态（用户报障原文：「用 QQ 远程协助他人的电脑使用我们这个软件时，如果软件在对方
电脑的最上层，那我会失去对方电脑的控制权（只能看到屏幕变化，但无法操作）」）：
选区 / 找图 / OCR / 拖拽取点 / 拖动准星这些叠层都是同一个套路 ——
`SetCapture(自己或宿主主窗口)` + 把窗口挪到屏外（-10000 / -32000）+ **阻塞的模态消息循环**，
只在收到「按钮抬起 / Esc」时才退出。终止事件一旦丢了（远控走 SendInput，注入的抬起本来
就可能丢），叠层就永远停在「等 UP」⇒ 捕获被我们扣着而窗口在屏外 ⇒ **看得见屏幕、点不动**。

- **硬规则**：这类循环**禁止裸 `GetMessage`**，一律用 `overlay_guard::WaitMessageWithTimeout()`
  （`src/overlay_input_guard.h`）超时醒来，超时后调 `ShouldAbortStuckCaptureNow()`
  自检；判定丢了就自己收尾（`ReleaseCapture` + 隐藏 + 结束循环）。
  宿主非模态路径（`engine_host_window.h` 的 `crosshairDrag_`）挂 `SetTimer` 做同一件事。
- 判据三条同时成立才动手（避免误伤正常拖拽）：① 捕获在我们手上 ② **没有**任何鼠标键按下
  ③ 距最后一次系统输入已超过 `kStuckCaptureIdleMs`(1200ms)。
- 应用点（加新叠层时照抄）：`src/screenshot_overlay.cpp`、`src/match_overlay.cpp`、
  `src/ocr_overlay.cpp`、`src/drag_pick_overlay.cpp`、`src/desktop_tools/desktop_tools.cpp`
  （`CrosshairPick` / `PickWindowTarget`）、`src/desktop_tools/float_ball.cpp`（悬浮球拖拽：
  **顶置常显窗**扣住捕获 = 整桌面点不动）、`src/engine/engine_host_window.h`
  （`crosshairDrag_` 与主窗自身的列表排序/三种滚动条拖拽）。
- ⚠ 判据**只可能在「已经坏了」的状态下触发**（捕获在手 && 无键按下 && 已停手 ≥1.2s）⇒
  正常拖拽（键按着）永远不满足，所以加这个兜底**不会**改变任何正常行为。
- 回归：`build\Release\OverlayInputGuardSelfTest.exe --json`（逻辑档，已进
  `run_all_selftests.ps1` 的 `$LogicSuites`）。⚠ 其中 `wait_times_out_when_queue_quiet`
  钉的是「兜底通道真能超时醒来」—— 判据写得再对，循环醒不过来也是死的。
- ⚠ **屏外停放的窗口是别人的进程**（`ParkHardwareInputTargetOffscreen` 停的是目标窗）：
  本进程被强杀时**不会**被还原，只能靠用户手动拉回。改这条路径时先想好收尾。

## 注入白名单红线（脆弱目标 / Adobe AIR）

**总原则：「只对某类目标生效的白名单」必须只有一个来源。** 前科（2026-10-03，用户报
「给微端使用后台窗口模式后游戏微端崩溃」，4399 微端 = 造梦西游 OL = Adobe AIR）——
AIR 在本项目里是**已知脆**目标（**禁**子类化 / 禁光标钩 / 禁 RawInput / 禁假 `WM_INPUT`），
但当时有三条路径**绕过了白名单**，共同形态是「**早退在安装之后**」或「**宿主认了、DLL 没认**」：

| 入口 | 缺陷 | 现状 |
|---|---|---|
| AIR 识别 | `InstallCommon` 只看**顶层窗类名**（`cls` 来自 `GA_ROOT`），而 `ApolloRuntimeContentWindow` 常是**内容子窗**（父窗是启动器/包装窗，4399 微端就是这个结构）⇒ 漏判 ⇒ 走**全量 Phase2** ⇒「一启动就卡死退出，鼠标原地抽」 | 已修：`HwndTreeLooksLikeAdobeAir(top)`（顶层**或任一子窗**命中即算 AIR） |
| 变速时钟钩 | 它是**改函数体**的内联钩，却装在 `if (airSafe) return TRUE;` **之前**，且非 IAT-only 模式下会把**所有模块**解析到的时钟函数都钩掉 ⇒「开倍速过一会就闪退」 | 已修：`SetIatOnlyMode(mapleSafe \|\| airSafe)`（AIR 与冒险岛同待遇，只补 IAT 槽） |
| `setwindowshook` | 该技术把 DLL 装进**目标的 UI 线程**（在它自己的消息线程里 `LoadLibrary`），闸门原来漏了 AIR。默认技术是 classic ⇒ **日常测不出来**，但设置里能选 ⇒ 一选就带走 | 已修：判据收进 `ForbidsSetWindowsHookTechnique()`（`window_mode_types.h`） |

- ⚠ **新增目标类型时先问：它在每一条白名单里都登记了吗？** 这类缺陷只在「用户恰好选了
  非默认选项 / 恰好是多层窗口结构」时复现 ⇒ **日常回归全绿也照样存在**。
- 回归（`WindowModeSelfTest`，两条都带**负对照**）：`fake_focus_air_child_iat_only`
  （包装窗 + AIR 子窗：不子类化 + 变速诊断 `bit6=1`；普通 `STATIC` 窗必须 `bit6=0`）、
  `setwindowshook_not_for_fragile_targets`（逐一命中 + 普通目标必须放行）。
- ⚠ **判「注入把游戏搞崩」之前，先看日志里有没有「动作证据行」**
  （`假焦点已注入` / `目标窗口已消失 … exit=` / `输入策略=`）。停在「未找到目标窗口，自动打开」
  →「仅绑定已有标题窗」→ `EndRun` 且**没有任何假焦点行**，那是**崩溃后的善后轮**，
  证明不了根因（详见 `window_mode_requirements.h` §19 / `LESSONS.md` §81）。

## 注入安全红线（调用原函数 / 线程生命周期）

**总原则：注入绝不允许把目标带走。** 2026-10-03 用户报障「当前项目，后台窗口模式/窗口模式
会导致一些窗口闪退」—— 现场是 Unity 游戏（`UnityWndClass`，`CreatureCurios.exe`），
假焦点注入完成后**第一次执行鼠标宏**目标即消失，`exit=0xC0000005`。两条独立入口，
细节收在 `src/window_mode/window_mode_requirements.h` §20/§21：

- **调用原函数不许改写目标函数头**。`CallThroughOriginal()` 原用「还原 N 字节 → 调用原函数 →
  重写绝对跳转」，而 x86-64 上 **12 字节写入不是原子的**（只有对齐的 8 字节以内才是）
  ⇒ 目标多线程调用被钩函数时取到「半个跳转」（`mov rax,<垃圾>; jmp rax`）⇒ 目标闪退。
  ⇒ 原函数一律走 **trampoline**（MinHook，`third_party/minhook/`，BSD-2）：
  函数头**只在安装时写一次**（`MH_EnableHook` 还会先挂起其他线程再落补丁），之后永远只读。
  回归 `python tools/verify/hook_race/run_hook_race.py`（1/2/8 线程全绿）+
  负对照 `negcontrol_hook_race.py`（**修复前 2/8 线程必红**）。
  ⚠ 负对照落在 `tools/verify/hook_race/legacy/` **副本**上，**别在 `src/` 上做变异**（有并行会话同步产物）。
- **不许在「装完就退的临时线程」上 attach 第三方运行时**。`unity::Init()` 曾在**注入线程**
  （`CreateRemoteThread` 出来、执行完 `FakeFocus_Install` 就退）`il2cpp_thread_attach`
  后**没人 detach** ⇒ IL2CPP 线程表留悬挂记录 ⇒ 下次 GC 扫栈访问违例 ⇒ 整个游戏进程消失。
  ⇒ 这类初始化必须**惰性**（只在真正需要时，如 `ApplyScale` 且非原速）+ 只落在**常驻线程**上
  （轮询线程退出前 `il2cpp_thread_detach`）；「用完就还」的路径（`Restore()`）要判
  `t_attachedThread` 决定是否 detach。
- ⚠ **现场特征**：**单线程目标不崩、多线程 3D/游戏目标崩**；崩溃码 `0xC0000005`；
  日志停在「假焦点注入完成」之后的第一次动作。同一竞态还会**静默漏钩**
  ⇒「后台输入时灵时不灵」是同一个病，不是两个。

## 窗口模式诊断可见性红线（失败原因必须能被用户导出）

**总原则：「诊断日志的可见性」也是功能的一部分 —— 判据是「用户能导出」，不是「代码里搜得到」。**
2026-10-03 用户**第三次**报「冒险岛后台：原地不动的平A，不能走A」。现场形态是
**诊断行整段消失**（`window_mode_debug.log` 里一轮只剩 `BeginRun` / `BeginRun：生效` /
`构建指纹` / `EndRun` 四行），而不是「读数不对」—— 那等于「注入根本没成功」。
后果链：假焦点没装上 ⇒ `PreferHardwareInput()` 在后台模式下**永不回退**假前台 SendInput
⇒ 键鼠只剩 PostMessage ⇒ **消息驱动的攻击键照打、方向键（软键态 / DirectInput 软键）全失效**。
细节收在 `src/window_mode/window_mode_requirements.h` §22：

- **失败点必须走 `WindowModeLogEvent*`（落盘），不能只走 `WindowModeLogf`**。
  `diagnose_report.txt` 只抓 `window_mode_debug.log` 的**最后 120 行**，而该文件只收
  Event 级日志 ⇒ 非 Event 的失败原因**写了等于没写**。`WindowModeLogf` 只配打**正常路径的细节**。
- **注入期的日志不能被 sink 过滤掉**。宏调试窗那条 sink 是
  `if (!runningWindowMode_.enabled) return;`，而嵌套模式的 `publishRunningWm()` 必须排在
  `beginWmCfg()` **之前**（原来排在成功之后 ⇒ 绑窗/注入/失败全被丢）。
- **`g_softView` 非空 ≠ 视图有效**。`InstallCommon` 的复用分支**禁止**加 `if (!g_softView)` 守卫：
  宿主每轮 `BeginRun` 都会重建映射，旧 view 可能指向**布局已变**的旧 section
  （`kSoftInputVersion` 从 7 抬到 10）⇒ `SoftInputStateLooksValid` 假 ⇒ `MaplePublishHits()`
  开头就 `return` ⇒ 宿主读全 0 + 软键态全失效。`OpenSoftInputView` 内部已处理「非空但无效 ⇒
  关掉重开」，**无条件调用**即可。
- **同路径 ≠ 同一份内容**。`LoadLibrary` 同路径**只加引用计数、不重跑 DllMain** ⇒
  用户升级软件但游戏进程没重启时，进程里跑的还是旧代码。已加
  `InjectedModuleLooksStale(dllWrite, procStart)` 告警（`window_mode_types.h`）：
  ⚠ **宁可漏报不误报**（任一时间为 0 即返回 false），且**只报警不阻断**。
- 回归：`WindowModeSelfTest` 的 `injected_module_stale_detection`（6 格，含 3 个负对照）。
- ⚠ 元规则（与「AI 工具描述红线」是同一族）：**「我写了」≠「对方收到了」**——
  那次的「对方」是模型（卡在 48 字节描述预算），这次的「对方」是用户导出的日志。

## 坐标语义红线（后台窗口模式 / 窗口模式）

**总原则：脚本的坐标系必须三端一致；不一致时**必须提示**，不能静默偏移。**

- 两套坐标系（`windowMode.coordSpace`）：`windowClient`（动作坐标**就是**客户区像素）|
  `screenAbsolute`（动作坐标是屏幕像素，回放时按**当前**窗口位置换算成客户区）。
- ★ 症状：`screenAbsolute` 脚本用后台窗口模式回放 ⇒ **偏差 = 窗口位移**。
  窗口不动时一切正常 ⇒ **只在移动后暴露**（用户原话：「窗口移动后就不能使用了」、
  「编辑宏的时候，后台窗口模式下鼠标不会移动到指定位置」）。
- ★ 日志指纹：每次移动的「脚本值 − 客户区值」**恒定**；若是客户区相对，应为原值直传或
  **等比例**缩放（比例不等即排除缩放路径）。
- 三端现状（2026-10-04 调查结论，**别重复查**）：录制端已自洽（`ConvertRecordedToActions`
  的开关与保存时写 `windowRelativeCoordinates` 的条件同源，且事件入队时就经
  `MapRecordingPointToClientIfWindowRelative` 转成客户区）；编辑端
  `SyncScriptWindowModeFromEditor()` **不碰** coordSpace / windowRelativeCoordinates /
  recordClientWidth/Height；回放端按 `windowRelativeCoordinates` 分流本身没错
  ⇒ 缺口在「语义不匹配时没有任何提示」。
- ⚠ **不做自动迁移**：旧脚本没有「录制时窗口位置」，信息已丢失，自动换算只是拿当前窗口
  位置去猜 ⇒ 会把「看起来能跑」变成「静默错位」。**只提示**（回放端 Event 告警 +
  编辑端 `promptModal_.ShowInfo` 一次）。
- ⚠ **日志里的「冒险岛」是历史名称**（`冒险岛钩命中` / `输入体检` / `钩安装` / `软光标`），
  实际已是**通用假焦点诊断**（Unity / UE / GLFW / 模拟器同样适用）。被 skills / LESSONS /
  历次日志共 10+ 处引用 ⇒ **不要改字面**，要消歧义就加前缀说明。
- 详见 `window_mode_requirements.h` §23（含诊断判读表）。

## AI 工具描述红线（写 description 前必读）

**模型看到的 description 是被截断的** —— 网页 AI 路径下有两道预算，**都是按 UTF-8 字节算**：

| 位置 | 渲染器 | 每条描述预算 | 何时发给模型 |
|---|---|---|---|
| 工具**目录** | `RenderToolCatalog`（`catalogDescChars`） | **48 字节 ≈ 16 个汉字** | 模型调 `loadTools` 时 |
| 扁平工具清单 | `RenderToolList`（`maxToolListChars` 只管总量） | **160 字节** | `toolCatalogMode=false` 时 |
| **完整定义** | `RenderToolDefinitions` | **不截断** | 模型**点名索取**该工具定义时 |

⇒ **硬规则：`tool.description` 的第一句必须是「这条工具最容易被误用的那件事」**
（现状里多数工具就是这么写的，句首带 `★`）。**排在后面的文字在目录/清单里根本到不了模型**。

- 前科（2026-10-02）：把「做完就收尾」判据加在 `completeTask` 描述的**末尾**
  ⇒ 源码里搜得到、自检也绿，但**模型一个字都没看到** ⇒ 用户报「改完没变化」。
- 自查工具：`python tools/verify/probe_tool_desc_budget.py --catalog`（看模型实际看到的那一行）、
  `--all`（按超预算幅度列出全部工具）、`--budget N <工具名>`（单条明细）。
- 回归：`AiActionRouterSelfTest/complete_task_wrapup_visible` 钉住
  「收尾判据必须出现在前 16 字内」（16 汉字 = 48 字节，与具体字节数无关）。

### 取值（枚举）走 `enum`，**不要写在 description 里**

目录渲染器会把**短枚举**印在参数名后面：`readScriptReference(section:all|format|…)`。
**这是"这个参数能填什么值"到达模型眼前的唯一通道** —— 写进 description 的取值会被 48 字节截掉。

⇒ **硬规则：凡是「有限取值集合」的参数，必须写成 JSON Schema 的 `enum` 数组。**

- 上限（`src/web_ai/web_ai_prompt.cpp`，两道，超了**整组丢弃**、不截一半）：
  单个取值 ≤ `kMaxEnumValue`(24) 字符；整组总长 ≤ `kMaxEnumChars`(160) 字符。
- 前科（2026-10-02）：旧判据是「**任一**取值 > 12 字符 ⇒ 整组丢」⇒ 静默丢掉 3 组
  （`switchWindow` 的 `default|window|backgroundWindow`、`computer` 的 13 个 action、
  `runActionRecipe` 的 4 个）⇒ 模型在目录里一个取值都看不到，只能自己编。
- 自查：`python tools/verify/probe_tool_desc_budget.py --enum`
  （列出全部取值组 + 哪些会被丢弃；`VERDICT=ALL_ENUMS_VISIBLE` 才算干净）。
- 回归：`AgentAssistantSelfTest/ref_tools_section_visible_in_catalog` 钉住
  「`readScriptReference` / `readAgentSkill` 的 section 取值必须出现在**真实目录渲染器**的输出里」。

### ⚠⚠ `parameters_json` 的 raw string 里**不能写 `//` 注释**

`tool.parameters_json = LR"({ ... })"` 里的一切都是**字面内容** —— `//` 不是 C++ 注释。
`agent_core.cpp` 会对它做 `json::parse`，**解析失败时静默回退成空 schema**
（工具还在、参数没了，不报错不崩溃、编译全绿）。

- 自查（含负对照）：`python tools/verify/probe_raw_json_comments.py` / `--selftest`。
- 说明写在 raw string **外面**（紧邻的 `//` 注释）。

## Module self-tests（必读入口）

统一约定与 suite 索引：

**[`.cursor/skills/module-selftest/SKILL.md`](.cursor/skills/module-selftest/SKILL.md)**

动作语义注意：`mouseClick`/`keyClick` 等 `duration` 是**重复间隔**（两次之间），不是执行前等待——细节见元 Skill「重复间隔语义」。

流程：选 suite → `MSBuild` Release `/t:<Target>` → `build\Release\<Target>.exe --json` → exit `0` 才宣称修好。

**Suite → Target 全表见上面那份 SKILL**（判读口径 / FAIL 对照也在那里）—— **勿在此再抄一份**
（抄两份必然漂移，这正是 §43 那条规则的由来）。

共享 harness：`tools/selftest_harness.h`。各 exe：`tools/*_selftest.cpp`。

窗口模式可选烟雾：`--macro`。定时任务硬规则见 scheduled-task-debug skill。

PowerShell 不要用 `/t:A;B` 拼多个 target（分号会拆命令）；分开跑。`--list` / `--json` 结果均在 stdout。

### 一键跑全部自检（推荐入口）

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1   # 构建 + 跑全部逻辑 suite
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -SkipBuild -LogPath build\selftest.log
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -Tier full   # 加交互类（需桌面会话+驱动）
```

CI：`.github/workflows/build.yml`（构建产品壳 + 全部 SelfTest 目标，只跑其中的逻辑档；
窗口模式 / 虚拟 HID / 注入 3 个只编不跑，需桌面会话与已装驱动）。**数量以
`tools/run_all_selftests.ps1` 的 `$LogicSuites` / `$InteractiveSuites` 为准** ——
别写死数字（本仓长期有并行会话往里加 suite，写死的会静默过期）。
2026-09-23 22:24 实测：CI **29** 个 SelfTest 目标（逻辑 26 + 交互 3）/ **858 用例 / 0 失败**。
⚠ 同一入口当天多次测量都出现过红，全在 `AiActionRouterSelfTest` / `WindowModeSelfTest`
（并行会话在途的重构）—— **别把别人的在途红当成自己的回归**。

### 构建陷阱（踩过，别再踩）

- **MSBuild + `https_proxy`/`HTTPS_PROXY` 并存 → 直接失败**。报错是
  `MSB6001: "CL.exe"的命令行开关无效。System.ArgumentException: 已添加项。字典中的关键字:"https_proxy"…`，
  看起来像编译开关问题，实际是 MSBuild 用大小写不敏感的字典构造子进程环境。
  绕法：构建前在本进程内 `Remove-Item Env:HTTPS_PROXY`（`run_all_selftests.ps1` 已内置护栏）。
- **OBJECT 库的 `.obj` 不会经由中间 OBJECT 库传递**：只链 `qst_engine` 会缺 `qst_desktop_tools` 的符号
  （`RenderBatchScope` / `UiEditorWidth` / `ResolveProgramLaunchPath`），链接列表要像 `QstWebViewShell`
  那样逐个列出；想让某个源只编一次**必须用 STATIC 库**（`qst_utils` 即此例）。
- **`qst_engine` 目前无法脱离壳链接**：`qst::webview::PostToWebUi` / `HotkeyLogLine` /
  `NotifyWebDebugWindowSetting` / `SyncHomeSelectionCache` 与 `g_instance` 只在
  `src/webview/qst_webview_shell.cpp` 定义；SelfTest 链引擎需带 `tools/engine_link_stubs.cpp`
  （空实现）。修法见 `docs/refactor-progress.md` §3.1。
- **含中文的 `.ps1` 必须 UTF-8 带 BOM**（写法详见「核心约定」；`tools/` 与 `driver/qst_vhid/`
  下曾各有一批漏网的，含 `package_webview_portable.ps1` ⇒ 便携包内中文 README 整个乱码）。

### 假焦点注入：**永远不要注入安装目录里那一份**（2026-09-30 用户报障后定规）

注入 = 让目标进程（游戏/模拟器）长期 `LoadLibrary` 这个 DLL。宿主被强杀 / 桌面钩没拆干净 /
引用计数残留，都会让这个映射活到目标进程退出（用户体感就是"软件关了文件还在、不重启电脑删不掉"）
⇒ **安装目录里的 `FakeFocus32/64.dll` 被锁住，覆盖安装与卸载全卡在这里**。

- 注入一律走**副本**：`src\window_mode\fake_focus\fake_focus_stage.{h,cpp}` 复制到
  `%LOCALAPPDATA%\QuickScriptTool\module_stage\<源名>.<大小>_<时间戳>\FakeFocus32.dll`。
  ⚠ **别改回**「直接注入 exe 旁那份」，也**别**「注入后删掉副本」（delete-pending 更难查）。
- 副本文件名必须归一成 `FakeFocus32.dll` / `FakeFocus64.dll`
  （`MapleIsFakeFocusModulePath`、`TargetHasStaleFakeFocusModule` 都按名认）。
- 桌面钩路径的 `HHOOK` + 本地 `HMODULE` **必须**交回 `UnloadOne` 去 Unhook + FreeLibrary；
  卸载还要**循环**远程 `FreeLibrary`（历史残留引用计数可能 >1，只拆一次归不了零）。
- 安装包兜底（`installer\QuickScriptTool.iss`）：`PrepareToInstall` 里 `ReleaseLockedInjectModules`
  给锁住的 DLL 让位改名，`[Files]` 加 `restartreplace` / `uninsrestartdelete`。
  ⚠ `CloseApplicationsFilter` 是**文件名通配符**（默认 `*.exe,*.dll`），**不是**"允许关闭哪些程序"
  —— 只写两个 exe 名就等于这两份 DLL 不参与占用检测，Inno 覆盖时重试 4 次后弹错误框。
- 自检：`WindowModeSelfTest` 的 `fake_focus_inject_copy` / `fake_focus_stage_sweep`；
  机制、装机复现口径、安装包改动全文见 [docs/fakefocus-dll-lock.md](docs/fakefocus-dll-lock.md)。

### 改完扩展 JS **必须**跑两条静态检查（2026-09-26 加）

`node --check` **只查语法**，而下面三类事故**语法上完全合法**，只在运行时炸，
且症状离原因极远（我一天内各栽一次，各白烧一轮）：

| 命令 | 抓什么 |
|---|---|
| `python tools/verify/ext_no_dup_functions.py` | **同名函数覆盖**（函数声明提升、后定义的赢 ⇒ 你新加的从没跑过） |
| `python tools/verify/ext_lint.py` | **未定义变量**（`ReferenceError`）、重复声明、**替换范围吃掉下一个函数头** |

⚠ `ext_lint.py` 依赖 `eslint@8`（装在 `~/.workbuddy-ai/binaries/node/workspace`，
**不污染用户环境**）。缺了会返回 3 并提示装法 —— 那是**环境问题**，不是代码问题。
⚠ **两条守卫都要保持干净**：永远报红的守卫没人看，等于没有。

## Edge 配套扩展（发版必带）

源码 `extension/edge/`（非编译产物），规范全文见 **[extension/PACKAGING.md](extension/PACKAGING.md)**。
命令：校验+打侧载 zip → `tools\pack_edge_extension.ps1`（出 `dist\QstEdgeBridge-<ver>.zip`）；
打 Release/安装包素材 → `tools\package_release.ps1`（内部先跑上一步）；Inno 安装包 → 先跑上一步
再编 `installer\QuickScriptTool.iss`。硬规则：改扩展版本须同步 `manifest.json` 与 `background.js`
的 `BRIDGE_VERSION`；`manifest` 必须是合法 JSON（`version` 行用逗号不是分号）；**禁止**发不含
`extension\edge` 的安装包/zip。

## AI 助手 Skill（内嵌助手能力，非 Prompt）

内嵌 AI 助手（`ui/agent.html` + `src/agent_*`）的能力以 Skill 文件定义，随包分发到 `skills/agent/`：

| section（9 个）：`conversation`（`rewindUserIndex`/`TruncateHistoryToUserRound`）· `revert`
（`listAgentChanges`/`revertAgentChange`，快照存 `AppDir()\agent_changes`）· `shell`（白名单
`runAgentCommand` + 文件工具 + 剪贴板，`src/agent_shell.cpp`）· `scriptStrategy`（`planScriptActions`
→ `buildScriptActions`+`createMacroScript`）· `optimize`（`optimizeRecording`/`optimizeScript`）·
**`command`**（产品自有：何时用命令行、`runCommand`/`resolveSystemPath`、不回显 stdout 要重定向后
`readAgentFile`、Excel COM/CSV、落到「运行程序」可回放）· **`office`**（产品自有：读用
`readDocument`，写用 `runCommand` 的 COM/CSV；中文 BOM、COM 收尾挂起、公式重算、`~$` 独占锁；
**§2.5 我们不依赖第三方办公引擎**）·
**`game`**（产品自有：VLM 不进每步路径、颜色/找图优先、`keyDown/keyUp` 长按、节奏用 `duration`、
反作弊）· **`desktop`**（产品自有：助手**动手**层 —— `runDesktopTask` 何时用/何时**不许**用、
危险目标确认闸 S1、观察与验收、外部 MCP 工具 `mcp__<server>__<tool>`） |

助手按 `AppDir()\skills\agent\<section>.md` → `AppDir()\skills\<section>.md` → 内嵌兜底读取。
改动助手 Skill / 优化算法后必须跑 `AgentAssistantSelfTest --json`（exit 0）并同步 `.cursor/skills/agent-*` 与打包脚本的拷贝列表。

**文档能力（readDocument）**：`tools/office/read_doc.ps1` 是**运行必需项**，CMake POST_BUILD
与两个打包脚本都已拷到 `<exe目录>\office\`。⚠ 改它之后必须重新构建（否则自检测到旧副本）。
支持 xlsx/xls/xlsb、csv/tsv、docx/doc、pptx/ppt、pdf、txt/md/json/log/xml/ini/sql；
PDF 文本走 **Windows 自带 IFilter**（无 Office 无 Python，实测 1147 字中文 0.03s），
扫描件渲染 PNG 交给多模态模型；`.pptx` 页序按 `sldIdLst`+rels 解析（**不能**按文件名排），
且**不能**手搓最小 OOXML（真实 pptx 44 个 part），生成用 PowerPoint COM。
研究依据：`.research/office-document-io-report.md`；游戏方向见
[docs/realtime-game-vision-loop-research.md](docs/realtime-game-vision-loop-research.md)。

**Skill 归属**：`.cursor/skills/agent-*` 是给 Cursor/开发者的；**产品功能相关的 Skill 放 `skills/agent/`**
（`command.md` 即此例，打包脚本优先拷贝 `skills/agent/<section>.md`，缺失时才回落到
`.cursor/skills/agent-<name>/SKILL.md`）。改 Skill 的读取/内嵌兜底要同步 `ai_action_router.cpp`
的同名 section 函数（如 `MacroActionCommandSkill()`）。

### 助手「动手」层（runDesktopTask）

助手原本 **34 个工具全是「产出物」型**（改文件/配置/剪贴板、生成脚本 JSON），**没有一个能操作桌面**
—— 这就是「功能有限」的根因。`src/agent_desktop_task.*` 补上了这一层（差距审计见
[docs/agent-capability-expansion.md](docs/agent-capability-expansion.md)）：

- **不复刻执行闭环，走正常回放链路**：`runDesktopTask` 把目标写成**一个临时脚本**
  （`ScriptsDir()\_agent_task_<pid>_<tick>_<n>.json`，内含单个 `aiActionExecute` 动作），
  再用 `engine::RequestRunScriptAsync()` 投给引擎跑。**不要**改成直接调
  `ExecuteAiActionExecute()` —— agentHooks 是在 `engine_script_run.cpp` 的回放上下文里组装的，
  在别处重组必然漂移；走回放能白拿断点/紧急停止/窗口模式/超时/AI 调试日志/嵌套熔断。
- ⚠ **临时脚本必须落在 `ScriptsDir()`**：引擎的 `ResolveLibraryScriptPath` **只认
  `scripts/` 与 `recordings/` 内的文件**，放别处会解析失败。
- ⚠ **等待不能用 `IsRunning()`**：投递到真正开跑之间有**空档**，只轮询它会把「还没开始」当成
  「已经结束」。要用 `engine::IsBusy()`（= `running_ || extRunPending_`），且**先等它变 true
  再等它变 false**。
- ⚠ 引擎的 `aiActionExecute` **原本不写 `aiVars_`**（`aiTextAnalysis`/`aiImageAnalysis` 写）。
  已在 `engine_script_run.cpp` 两个分支补上 `aiOutputVarName` 落点 —— 这是助手能回报
  「**具体发生了什么**」而不是只说「完成」的前提。
- **安全联锁 S1**：`DesktopTaskNeedsConfirm(goal)` 命中危险词就**拒绝执行**并返回确认提示，
  必须用户明确同意后带 `confirmed=true` 重调。危险词**中英分治**：中文按**子串**，
  英文按**词边界**（`ContainsEnglishWord`）—— 否则 `send` 会命中 `sender`。
  ⚠ 别加「只填/only fill」这类**局部**否定词到 `kNegations`：那是整句级否定表，
  混进局部否定会让闸被问废。
- 结果回读：`engine::GetMacroVariable(L"__qstAgentTaskResult")` → `SummarizeDesktopTaskResult()`
  剥掉 `[EXECUTED]`/`[OBSERVE]`/`[结果]` 等回放控制标记再给模型。
- ⚠⚠ **临时脚本「不是用户脚本」这件事必须自己保证**（`scripts\` 的枚举**不做任何名字过滤**）：
  正常路径跑完即删，但**崩溃/被强杀**会留残留 ⇒ 用户脚本库里会出现一个跑不了的鬼影条目。
  两道一起才叫干净：
  1. **列表出口过滤**：`IsAgentTaskTempFileName()`（判据 + 前缀常量都在 `src/utils.cpp`，
     与生成点 `DesktopTaskTempScriptPath()` 共用 `kAgentTaskTempScriptPrefix()`，**别另写字面量**）。
     过滤点：`ListJsonFiles()`（UI 脚本库）与 `ListScriptsInDir()`（助手 `listScripts`）。
  2. **启动清扫**：`SweepStaleAgentTaskTempScripts(ScriptsDir())`（`qst_webview_shell.cpp` 的
     `wWinMain`，紧挨 `CleanOrphanImages()`），**只删创建它的进程已不在的**——
     进程还活着的一律不动（可能正有另一个实例在跑任务）。
     ⚠ **不能**去改 `EnumerateScriptJsonFiles` 或 `ResolveLibraryScriptPath`：引擎正是靠后者
     才找得到这个临时脚本，过滤掉就跑不起来。
  判据**判严不判松**：判严只漏过滤一个残留，判松会**吞掉用户自己的脚本**。
- 自检：`AgentDesktopTaskSelfTest`（纯逻辑，19 例，含名字判据 + 清扫的真建文件测试）；
  `AgentAssistantSelfTest` 的 `list_scripts_hides_desktop_task_temp` 真调 `listScripts` 断言
  用户可见行为（**A/B 验证过**：摘掉过滤 ⇒ 该用例转红）。改了参数/闸/结果清洗/列表过滤必须跑。

### MCP 客户端（外部工具接入）

产品**早就是 MCP server**（`src/mcp_server.cpp`，方向 B）。`src/agent_mcp.*` 补的是**对称的另一半**
—— 让助手挂上**用户自己配置的**外部 MCP server。

- ⚠⚠ **产品不主动探测、不主动接入任何第三方软件**（2026-09-23 用户指出后纠正）。
  曾经有过「自动发现 GenOffice 并自动补一条配置」的逻辑，**已删除** —— 那会让产品在用户
  不知情的情况下把某项能力外包给第三方（对方没装就能力缺失、版本不同就行为不同），
  属于**隐式依赖**。要接第三方工具，用户自己写配置，产品不替任何人做决定。
- **配置**：`AppDir()\mcp_servers.json`（`{"servers":[{name,command,args,cwd,enabled}]}`）。
  **文件不存在 = 一个进程都不起**（自检/无配置用户零影响）；只有用户显式写下的 server 才会启动。
- **懒加载 + 缓存**：`CollectMcpAgentTools()` 由 `BuildDefaultAgentTools()` 调用，而它在
  **UI 线程**上（`RebuildAgentCoreLocked`）⇒ 整个枚举共享 `kMcpEnumerateBudgetMs`（10s）预算，
  用完就跳过剩下的并写诊断。**改这里别把预算去掉**，一条坏配置能冻住界面。
- 工具名 `mcp__<server>__<tool>`；`server` 名进文件名/路径前必须净化（它来自用户配置）。
- ★★ **图片闭环**：`tools/call` 结果里 `content[].type=="image"` 的 base64 会被解码落盘到
  `AppDir()\agent_mcp_images\<server>\`，并在文本里补 `[[AGENT_IMG:<绝对路径>]]` ——
  宿主的 `AgentExtractImageMarkers()` / `AgentBuildImageParts()` 据此把图作为 `image_url`
  塞进**下一轮**请求（`zoom` 走的就是这条路）。**这是「办公任务验收」唯一能成立的机制**：
  助手侧没有任何「把盘上图片塞进对话」的工具（`aiImageAnalysis` 只吃**实时截屏**）。
  ⚠ 新增任何「返回图片」的工具，都必须照这个契约打标记，否则图等于丢了。
  ⚠ 宿主认不出的格式（`image/svg+xml` 等）**不许**落盘成 `.png` 假装能看图，要留一句说清格式的话。
- ⚠⚠ **这个目录只增不减 ⇒ 必须按年龄清理**：标记被消费时 `AgentExtractImageMarkers()` 会把
  `[[AGENT_IMG:…]]` 从文本里**删掉**，历史里不留路径 ⇒ 没有任何「自然回收」的机会。
  `SweepStaleMcpImages()`（保留 `kMcpImageRetentionDays` = 7 天）在 `wWinMain` 里跑，
  打一行 `StartupTrace("mcp image sweep removed=N")`。
  ⚠ 判据里 **`now > w.QuadPart` 这个前置条件不能省**：文件时间戳被拨到未来时 `now - w`
  会按 ULONG_LONG 回绕成天文数字 ⇒ 把新图当老图删掉（**A/B 验证过**：去掉它 ⇒
  `AgentMcpSelfTest` 的 `sweep_stale_mcp_images` 转红，`futureKept=0`）。
  真删错了也只是降级成一行 `[读取失败] …`（`AgentBuildImageParts` 会跳过并记账），不会崩。
- ⚠ **`CreateProcessW` 能直接跑 `.cmd`，但跑不了无扩展名的脚本**（实测 WinError 2）；
  裸名也只补 `.exe`，**不按 PATHEXT 找 `.cmd`/`.bat`** ⇒ 用户配置里 `.cmd`/`.bat` 必须写完整路径
  （`McpCommandIsRunnable` 只做扩展名判据）。
- ⚠ `CollectMcpAgentTools()` 返回的工具**不是内置工具**：出错时要把对方进程的错误原样上报，
  别猜；`mcp_servers.json` 坏 JSON 才算错误，文件缺失不算。
- ⚠⚠ **子进程必须有孤儿防护**：`Stop()` 是**优雅**路径，宿主被强杀/崩溃时它根本没机会跑
  ⇒ 起出来的 server 子进程会一直挂着（用户配的可能是 Node/Python 进程，几百 MB + 持有管道），
  用户任务管理器里会攒出一串看不懂的 `node.exe`。
  做法：`CREATE_SUSPENDED` 起进程 → 挂进 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job
  （`CreateKillOnCloseJob()`）→ 再 `ResumeThread`。**必须先挂后放行**，否则子进程可能在
  Assign 之前就 spawn 出不受管的孙进程（Node 起渲染器）。Job 句柄在 `Impl` 生命周期内
  **一直持有**，`Stop()` 里最后才关。
  ⚠ 判据 `OrphanGuardActive()` **必须用 `IsProcessInJob` 真查子进程在不在 Job 里**：
  第一版只看「`hJob` 非空 + 无诊断」，A/B（短路掉 `AssignProcessToJobObject`）
  **照样返回 true** —— 假绿。**判据要盯被保护的对象，不是保护者的句柄。**
- ⚠⚠ **同一个 client 上必须串行发请求**（`Impl::callMu`）：MCP stdio 是「一问一答」协议，
  而 `ReadResponse` 会把「别人的 id」的响应从队列里 **pop 掉并丢弃**（`Ignore → continue`）
  ⇒ 两个在途请求会**互相偷响应**，输的那一方一直等到**超时**（产品默认 180s）才返回。
  `Initialize` / `ListTools` / `CallTool` 三个入口都持这把锁（`nextId++` 也靠它串行化）。
  ⚠ 产品当前是**串行**调用工具的（`agent_core.cpp` 的 `tool_calls` 循环）⇒ 今天打不到；
  但**别把「调用方串行」当隐含前提** —— 一旦有人并行化工具调用，它就会变成「莫名卡 3 分钟」。
  ⚠ `Stop()` 里**绝不能**去拿 `callMu`（那正是要打断的调用持有的锁 ⇒ 死锁）；
  它是靠置 `readErr` + `notify_all()` 让在途请求**立刻**返回的。
- 自检：`AgentMcpSelfTest`（19 纯协议 + 1 可执行性判据 + 4 图片/错误 + 1 附图目录清理
  + 2 孤儿防护 + 1 单通道串行 + 5 `macro` 端到端 = 30）。
  端到端**不依赖任何外部程序**：本 exe 带 `--fake-mcp-server` 启动即成为最小 stdio JSON-RPC 服务端，
  用例拿自己的路径起子进程，真管道真进程地验握手/列工具/调用/错误/超时。
- **`runAgentCommand` 白名单**：只放行 `git`（只读子命令）、`MSBuild`（自检 Target）、
  `build\Release\*SelfTest.exe`、`where`。**没有任何第三方软件特例** ——
  曾经给 `genoffice` 开过一条（含 300s 专用超时 `kOfficeCommandTimeoutMs`），
  **已删除**（理由同上面那条：产品不依赖别人的软件运行）。
  ⚠ 用户想跑白名单外的程序（含第三方办公工具）**走 MCP**（`mcp_servers.json`），
  不要在这里开口子；`AgentAssistantSelfTest` 的 `shell_rejects_third_party` 会拦住回归。

### OOXML 容器层（`src/ooxml/`）—— 自研，不依赖 Office 与第三方库

「不装 Office 也能读写 xlsx/docx/pptx」的**地基**。为什么要自己写：本仓原有 zip 代码
（`utils.cpp` 的 `ExtractZipFile` / `ReadTextFromZip`）**只按 `compSize` 原样拷贝字节、
根本不解压** —— 它读不了任何真实 Office 文件（真文件的条目是 deflate/method 8）。
而现有 `ooxml` 读取引擎其实是**起 PowerShell 脚本**（`tools/office/read_doc.ps1`，用 .NET
`ZipFile`），能读但**不能写**，且每次都要起进程。

| 文件 | 职责 |
|---|---|
| `src/ooxml/inflate.h/.cpp` | **RFC 1951 解压**（stored / 固定 Huffman / 动态 Huffman）。纯逻辑，不碰文件系统 |
| `src/ooxml/zip_archive.h/.cpp` | zip 读写。★核心是**「未改动条目原样搬运」**：只重写改动的 part，其余把**原始压缩字节**连 method/crc 一起拷过去 |
| `src/ooxml/xlsx_doc.h/.cpp` | xlsx 读写（表名解析 / 单元格读 / **外科手术式**单元格写 / TSV 导出） |

- ⚠⚠ **「字节保留」是这一层的存在理由**：改一个 .xlsx 的单元格时，`xl/charts/*`、
  `xl/styles.xml`、嵌入图片等几十个 part **必须一字不动地活下来**。一旦走「解析成对象模型
  再重新生成」，图表/公式/透视表/样式全丢。`SaveToMemory` 里那句
  `if (e.modified)` 就是这条性质的全部实现 —— **改它之前先看自检**。
- ⚠⚠ **xlsx 写单元格是「文本外科手术」，不是模型重建**：只替换那一个 `<c>` 元素，
  **连同一个 worksheet 里**的 `<mergeCells>` / `<cols>` / 行高属性 / `<conditionalFormatting>`
  都一字不动（自检 `xlsx_set_cell_preserves_sheet_extras` 逐条钉住）。
  · 替换时必须**保留原 `<c>` 上的 `s`（样式）等属性** —— 否则数字格式/填充全丢
    （`xlsx_set_cell_keeps_style_attr`）。
  · 新文本写 **`t="inlineStr"`**（+ `xml:space="preserve"`）：OOXML 允许逐格选，
    于是**完全不用碰 `sharedStrings.xml`**，改动面又小一块。
  · ⚠⚠ **`<sheetData/>` 自闭合时必须先展开再插**：否则新行落到元素**外面**，
    文件看着写成功了、Excel 却当它不存在。空表是常见形态，这条必踩
    （**A/B 验证过**：不展开 ⇒ `xlsx_roundtrip_after_save` + `xlsx_set_cell_inserts_in_column_order`
    双双转红）。
- ⚠ **Zip64 明确拒绝**（>4GB 或 >65535 条目）并给出可读原因，不按 32 位错读出一堆垃圾。
- ⚠ 写的时候用 **method 0（stored）**：Excel/Word 完全接受，省掉一整个压缩器。
- 已知边界（写进头文件了）：**不更新 `<dimension>`**（提示字段，Excel 会重算；保持不动是为了
  手术面最小）；**公式不算值**（`<v>` 缓存值由调用方给，没给就留空 ⇒ Excel 打开时重算，
  但直读拿不到值 —— 这是 OOXML 既有语义，不是 bug）；不支持 .xls 与加密文件。
- 自检 `OoxmlSelfTest`（**26 例**，只链 `qst_utils`）：夹具是**硬编码的真 deflate 流 + 真 zip**
  （Python zlib/zipfile 生成后内联）+ 用例自己用 `ZipArchive` 拼的**结构真实 xlsx**（两张表 /
  富文本 sharedString / 带样式单元格 / 合并单元格 / 一个必须原样存活的 chart part）。
  ⇒ 不依赖 Python、不依赖 Office、不依赖任何外部程序。
  诊断开关 `--emit-sample <path>`：把 `CreateNew` 的产物写盘，交给**别的工具**独立验证
  （实测用 Python `zipfile` + `etree` 验过：6 个 part 齐全、无格式错误、content-type 正确、
  值能独立读回）。**自检只能证明「我们自己读得回来」，那不够。**

#### 接进助手：`readDocument` 的 xlsx 原生快路径 + `writeSpreadsheet`

**读**：`ReadOfficeDocument` 对 `.xlsx`/`.xlsm` **优先走原生路径**（`engine=ooxml-native`），
失败才静默回退到老的 PowerShell 脚本。为什么值得抢在脚本前面：
① **不起进程**（脚本每次拉 `powershell.exe`，60s 超时，纯开销）；
② PowerShell 被限制的环境也能读；
③ ★**日期能正确还原** —— Excel 把日期存成序列号（2024-01-01 = 45292），而脚本的 OOXML
回退分支只取 `<v>` 原文 ⇒ **那条路线上的日期一直是坏的**（只有装了 Office 的 COM 才对）。
⚠ 数字格式只对**日期/时间**生效（还原成 ISO）；**百分比等保持原始数值**（0.12 就是真值）。
⚠ 1900 假闰日（序列 60 = 1900-02-29）与 `<workbookPr date1904>` **两种基准都要对**
（同一序列号差 1462 天）。

**写**：`writeSpreadsheet`（`src/agent_shell.cpp`）—— 在此之前写 .xlsx **只有 Excel COM 一条路**
（必须装 Office）。两种模式：

- `mode="create"`：`tsv` → 新文件。**默认拒绝覆盖**（要覆盖显式 `overwrite=true`）。
  撤销走撤销日志（`existed=false` ⇒ 恢复=删文件）—— **二进制无关**，所以成立。
- `mode="setCells"`：改已有文件的指定单元格，**字节保留**（只动你点名的 `<c>`）。
  ⚠⚠ **改前必须落字节级备份**到 `AppDir()\agent_changes\xlsx_backup_*.xlsx`：
  撤销日志存的是 **UTF-8 文本**，装不下二进制 —— 拿它当撤销会**清空用户文件**。
  所以走备份，并把路径告诉用户（**不假装它进了「撤销」列表**）。
- ⚠⚠ **可写范围只对这一个工具放宽**：`SpreadsheetWriteRoots()` = `AgentWriteRoots()`
  **+ 桌面 / 文档 / 下载**（「做成 xlsx 放桌面」是主用例，不放宽这工具就没法用）。
  **没去动 `AgentWriteRoots()`** —— 那是 `writeAgentFile` 的边界，别顺手放宽。
  `IsSensitiveAgentPath()` 仍拦 `app_settings.json` / `.exe` / 仓库内部文件。
- 数字自动识别（`SUM` 才算得动），但**带前导 0 的 `007` 保持文本**；
  公式**不计算**（`cached` 是直读用缓存值）；写完读回核对后一起返回。
- 自检：`AgentAssistantSelfTest` 的 `write_spreadsheet_create_and_set` +
  `read_document_native_xlsx`（都真落盘真读回）。

### 只读 SQLite + 浏览器历史（`src/sqlite/`，`readBrowserHistory`）

**起因**：实测「统计 Edge 最近 10 条浏览记录」走 UI 路线要 **8+ 轮、多次截图、23.6 秒**，
而且 `observePage` 在 `edge://history` 上**必然失败** —— Chrome/Edge 的
`edge://` / `chrome://` **内置页禁止扩展注入 content script**，重试没有任何意义。
模型撞墙后会去 `zoom` 读被省略号截断的标题（每次 zoom = 一次图片上传），最后还是抄错。
（注意：**不是**本产品没做好 UIA —— 是那条路从原理上就不该走。）

**正路**：直接读浏览器自己的 SQLite 库。一条查询拿到「完整标题 + 完整 URL + 精确时间」。

| 文件 | 职责 |
|---|---|
| `src/sqlite/sqlite_read.{h,cpp}` | **纯逻辑**只读 SQLite 解析器（无 Win32 依赖，也编进 `SqliteSelfTest`） |
| `src/sqlite/sqlite_file.cpp` | Win32 那半边：按共享方式打开被占用的库、整体读入 |
| `src/sqlite/browser_history.{h,cpp}` | 探测 Edge/Chrome profile、读 History 库 / Bookmarks JSON、渲染成表格文本 |

**为什么自己写解析器**：用户的纪律 —— **产品不依赖别人的软件运行**（见 `LESSONS.md` §18）。
所以**不调** `sqlite3.exe`、**不调** Python，格式解析自己做。
`sqlite_read.h` 里**逐条列明不支持的能力**（写入 / WAL / 索引 / JOIN / 加密），
避免「偷偷假装支持」——遇到 WAL 主库**明确拒绝**而不是硬读出一堆垃圾。

**格式要点**（每条都在真实 Edge History 库上验证过，详见 `sqlite_read.cpp` 顶部）：
- 单元指针是**页内绝对偏移（相对页首）**，第 1 页页头在 `raw+100` 但指针**仍从 raw 算起**。
- serial type 要**按 header 字节边界循环**（`while (q < hdrEnd)`）—— 它本身是 varint，
  **可占 2 字节**，按「个数」循环会多数出一列。
- `rowLimit == 0` 的语义是**不限**。

**⚠⚠ 本仓最隐蔽的 bug：int64 转 double 比大小。**
排序比较器原本把所有数值转 `double` 再比，看代码完全正常，自检却报「降序第 2 条给错」。
根因：Chromium 的 `last_visit_time` 是「1601 年起微秒数」，量级 **~1.34e16**，
**超过 double 精确表示整数的上限 `2^53 ≈ 9.0e15`** ⇒ `...000` 与 `...001` 塌成同一个值
⇒ 比较恒等于 0 ⇒ 排序「大体对、相邻项错」。**在真实数据上会静默搞乱顺序。**
⇒ **纪律：比较两个整数永远直接用 `int64`**；只有真牵涉 `Real` 才降级到 `double`。

**接进助手**：`readBrowserHistory`（`src/macro_execute_tools.cpp`），
排在 `openWebpage` **之前**（模型看工具顺序会优先选更直接的读法）。
`kind=history|bookmarks|browsers`，`limit` 默认 20。
返回**制表符分隔、一行一条** ⇒ 直接配 `writeSpreadsheet` 的 `tsv` 或 CSV 配方。
**浏览器开着也能读**（`FILE_SHARE_READ|WRITE|DELETE`，不需要 CopyFile —— SQLite 原子提交
保证一致快照）。**只读，绝不写浏览器库**。

**自检**：`SqliteSelfTest`（27 suite 中的第 27 个，**7 passed / 0 failed**）。
⭐ 核心方法：夹具与期望值**都由官方 `sqlite3` 模块生成**
（`build/_tmp/mk_sqlite_fixture.py`），**不从实现反推** ——
这套做法一次就抓出上面 4 个真 bug。「看起来对」和「真对」差得很远。
⚠ 它只链 `qst_utils`（纯逻辑档），**不**链 `sqlite_file.cpp` / `browser_history.cpp`（那俩要 Win32）。

### 核心约定（改动前必读）

- **含中文的 `.ps1` 必须存成 UTF-8 带 BOM**。本机 ANSI 代码页是 GB2312：无 BOM 时 PowerShell 5.1
  按 GBK 解析 → 中文乱码、甚至把引号/花括号吃成语法错误（`read_doc.ps1`、`test_mcp_server.ps1`、
  调研脚本各中过一次）。写文件用
  `[System.IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding($true)))`。
- **MCP server 模式**：`QuickScriptTool.exe --mcp` 走 stdio MCP（每行一个 JSON-RPC 2.0），
  在 WebView2/单实例初始化**之前**分流（`src/mcp_server.cpp`；入口在 `qst_webview_shell.cpp` 的
  `wWinMain` 顶部）。暴露 12 个工具（清单见 `docs/ai-action-exec-optimization.md` §18），
  全部复用既有原生链路。
  冒烟：`powershell -File tools\test_mcp_server.ps1`（真拉子进程走管道，exit 0 才算过）。
  改工具清单要同步该脚本与 `docs/ai-action-exec-optimization.md` §18。
- **思考开关策略**：默认**允许思考**（准确率优先）；降档两条见「思考降档两条」
  （`ShouldDisableThinking()`，声明在 `agent_core.h`）。
- **computer-use 别名**：`computer` 工具（`MakeComputerTool`）是别名入口，映射到既有动作；
  其上下文守卫与 `mouseClick` 共用 `GuardPointerClickContext()`——**改守卫必须同时覆盖两者**，
  自检 `computer_alias_tool` 会断言这一点。

- **低性能模式**：设置 → 宏回放设置的勾选框，用户抱怨「跑脚本时电脑烫/风扇响」时的取舍开关。
  开关是**进程级原子量，只有一份**：`src/low_power_mode.h` 的 `SetLowPerformanceMode()` /
  `LowPerformanceMode()`（头文件内联）。`LoadAppSettings`/`SaveAppSettings` 各同步一次 → 立即生效。
  生效点：`input_timeline_scheduler.cpp`（忙自旋 12ms→0.8ms）、`image_match.cpp` 的
  `SyncImageMatchThreadBudget()`（CV 线程 →1）、`engine_script_run.cpp` 三个 playback guard
  （不提优先级/不绑核/不抬定时器分辨率）、找图监视轮询下限（50→200ms）。
  **加新的降耗点必须读同一个 `LowPerformanceMode()`，不要另建开关**；用例**结束必须复位**开关，
  否则污染同进程后续用例（`ImageMatchSelfTest/low_power_limits_cv_threads` 等）。

- **找图 GPU 加速（OpenCL）**：设置 → 宏回放设置，默认**关**。开关本体在 `src/findimage_gpu.h`
  （头文件内联原子量，避免设置层依赖 OpenCV）；生效点在 `image_match_internal.h` 的
  `MatchSingleScale` 非金字塔分支 → `TryMatchTemplateOnGpu()`。三条实测（docs §19.4）：
  ① 只 `setUseOpenCL(true)` 对 **Mat 输入无效甚至更慢**（OpenCV 只对 `UMat` 走 OpenCL），必须显式 UMat；
  ② 面积 <500k 像素一律 CPU；③ 低性能模式优先，GPU 抛异常就本进程永久回落 CPU。

- **游戏前台「硬指令注入」：已撤销（批 C）**。原实现：`AiActionGameForegroundLikely()` 命中时
  每轮往指令里追加「用视觉推进、**本轮必须至少落一个动作**」，并附一段「每任务只提示一次」的
  玩法要点（`AiGameNudgeOnce`）。撤销理由：那是**引擎替模型下命令**（祈使句 + 禁令），
  而它本来只是「游戏没有控件树、视觉是唯一手段」这个**事实**的推论 —— 归属地是 Skill
  （`lookupMacroAction(section=game)`，模型按需自己去查）。`AiActionGameForegroundLikely()`
  **保留**：它仍是感知分档的输入（settle 短节拍 / 判断日志），只是不再塞命令。
  ⚠ **事故教训仍有效（而「守卫」本身已删）**：`AiActionUiTooBusyForVisionLocate()` 曾经
  **只看动态覆盖率** ⇒ 游戏逐帧重绘必然超标 ⇒ `locateAndClick` 被 100% 硬拦 ⇒
  「游戏没反应、一直在思考」；后来改成按前台分流（dom/mixed、canvas、非游戏前台放行），
  **批 D 更进一步：拦与劝两个消费点全部删除** —— 现在它**只做如实留痕**
  （`vision_gate` 一行判决 + 输入信号，供离线影子测试复算）。判据留下的**事实**仍然对：
  「画布页/非浏览器前台没有控件树可用」——但那不许再变成对模型的手段禁令。
  自检 `vision_gate_trace_only`（反转钉「判断照记、动作路径不拦不劝」）。

- **本地判断表**（`src/ai_decide.h/.cpp`，System One 形态，见 docs §22）：
  AI 动作执行的「阈值式判断」统一成 **输入本地状态向量 `AiDecideSignals` → 输出
  `枚举 + confidence + why`**。三条硬规则：① **纯函数**（宿主事实一律由信号传入，
  所以能**逐格自检**）；② **布尔结果与原实现逐字等价**（旧函数保留为薄封装，由
  `decide_wrapper_equivalence` 钉死）；③ `confidence` **不是准确率**，是「有多少本地证据」，
  现在**只用于如实留痕**（判断日志里的 why/conf + `FormatAiSignals` 离线复算）。
  ★ **confidence 不再驱动任何行为**：把它变成「往对话里塞提示」的那条三档路由
  （`AiVisionLocateHintFor`）与它拦识图定位的两个消费点**已整体删除（批 D，docs §47）**
  —— 引擎不决定模型该用哪种感知手段，`visionIsOnlyWay` 时更没有「改用控件树」的错指引。
  ⚠ **High 档 = 有硬证据**：`kAiDecideConfHigh` 是**闭区间下界**，所以「兜底放行」的行
  **不能贴线写 0.70**（会被判成有硬证据），行⑥ 就是为此用的 0.68。
  **加判据 = 加一行表 + 在 `decide_table` 里补一格**；表必须穷尽，走不到兜底会明确报
  「判断表未覆盖（实现缺陷）」而不是静默放行。判断理由进 `NoteAiDecisionLog(...)`。
  自检 `decide_table` / `decide_wrapper_equivalence` / `decide_diagnostic_log` /
  `vision_gate_trace_only` / `slow_thinking_round_gate`。
  ⚠ `ai_decide.cpp` 同时编进 `qst_engine` 与 `script_action_builder_core` —— 不可同时链。

- **判断行必须可离线复算**（`FormatAiSignals`，见 docs §22.8/§23）：判断日志不只记「判了什么」，
  还带一段紧凑信号（`| sig page=- br=1 self=0 ... changing=1`）。格式是**稳定契约**
  （解析方 `tools/verify/ai_decide_shadow.py`），**新增字段只能追加在末尾**。
  落盘接线：播放器 → `PlayerLog`、产品壳 → `BootLogLineW`（都装 `SetAiDecisionLogSink`）。
  ⚠ 不加这条接线，AI 判断在**导出的 exe 里完全没有痕迹**（原先只写宏调试窗，窗口没建就丢弃）。

- **settle 短节拍按置信度分档**（`kAiGameConfDecisive = 0.80`，见 docs §22.7）：
  游戏前台的 0.9s 短 settle 只有「结构性证据」才配用 —— `game_no_tree`(0.88) /
  `game_canvas_page`(0.90) 可以，`game_by_motion`(0.55) **不行**
  （视频/动画广告也是「画面在持续变」）。门槛用 0.80 而不是 `kAiDecideConfHigh`，
  必须落在疑似与结构性证据之间；改那两行的置信度要回来核对常量。
  取记录用 `AiLastGameForegroundDecision()`（判决 + 依据一起返回，
  **别自己重算判决** —— 重算时桌面状态可能已变）。

- **离线影子测试 / 置信度校准**：`tools/verify/ai_decide_shadow.py`（docs §23）—— 拿真实日志回答
  「判断表说 0.68 把握时实际对了几成」「换阈值会不会更准」，也是「要不要接本地 Jev/小模型当判断
  后端」的**前置条件**。先 `--selftest`（固定用例）再 `--log <player.log>`。
  **复算闸**（Python 重实现判定表 vs 日志判决）不一致就 exit 1 ⇒ C++ 改了而 Python 没同步；
  改阈值必须两边一起改（常量在脚本顶部 `K_*` / `CONF_*` / `GAME_CONF_DECISIVE`）。
  ⚠ 归因口径两种错法都踩过（固定行数窗口 → 比率 >1；段末截窗 → 拦下组永远 0 样本），现在是
  **按信号段**归因、比率按**判断条数**算；改 `attach_truth` 前先读 §23.3 并跑 `--selftest`。

- **文字直点（本地 OCR 直点）两条硬规则**（docs §24；实测一次带文字按钮点击白烧 2 次 API ≈ 249 KB）：
  ① **索引档与就地复核档必须同一把尺**：索引级用 `AiOcrLabelMatchTier`（双向包含），复核若要求
  **完全相等**就永远比索引更严 ⇒ 每次点击都回落一整轮 VLM 识图。改用 `AiOcrProbeAgreesWithIndex()`：
  「仍算同一标签 + 索引档够好 + 漂移 ≤24px」即采信，并接受复核把坐标修得更准。
  ② **同一行多段要能拼**：OCR 常把按钮文字拆段（「自选」+「僵尸卡牌」），单段既过不了长度比
  （差 >40% → 档位 0）也过不了框合理性 ⇒ 整个标签进不了索引。`OcrMergedRowCandidates()` 把同一行
  横向相邻的 2~4 段拼成候选；歧义判定里要**放过「一个框基本套住另一个」**（交集 ≥ 较小框 80%）
  ——那是同一处的两种读法，不是两个按钮。真·两处同名仍判歧义。自检 `ocr_direct_click_pick`
  / `vision_gate_trace_only`（⚠「不该拼」的反例要选**单段都够不着目标**的分片）。

- **执行后要不要回传观察帧**（`AiDecideNeedObserveAfterExec`，见 docs §24.3）：
  三档合取 —— ①模型明确要 → 看；②结果不确定（`[事实] settle无反应`/灰钮/`[UIA]`）→ 看；
  ③**本任务第一次动手** → 看（防「抢跑」：首帧往往还没生成，模型只能靠猜）。
  `actionSeq` 按「一次 AI 动作」复位，所以是一次动作一帧。
  ⚠ `computer(action=hold_key)` 分支原本**直接返回、绕过这三条守卫**，而 computer 是最常用的
  别名入口 —— 加守卫时必须一起覆盖（已修，自检 `ai_exec_observe_guards` 钉死）。
  该函数与 `AiExecResultLooksUncertain` **必须文件作用域**（匿名 namespace → LNK2019）。

- **每轮耗时拆解**（`agent_core.cpp` SendMessage 循环收尾，docs §25.3）：每轮打一行
  `第 N 轮耗时 Xms = 等模型 Yms + 本地执行 Zms（k 个工具）`，另一行
  `请求体拆解 NKB = system + 图(n张) + 思考回灌 + 工具结果 + assistant + user文本 + 工具定义
  （…条消息）；thinking=关闭/允许（为什么）；reasoning_effort=low 已下发／未下发`。
  ⚠ **说「慢」之前先看这两行**：实测一局游戏里等模型 5~25s、本地执行秒级 —— 瓶颈在模型的**思考**，
  不在识图/OCR/点击链路。第一杠杆是档位：动作执行已下发 `reasoning_effort=low`（官方默认 `high`）。
  ⚠ 心跳行 `流式等待 Ns，思考 X 字节，回复 Y 字节` 里「思考」与「回复」是**两个字段**，**别读错**
  （§42 就是读错了才把处置瞄错对象）。⚠ **在看到「请求体拆解」这行数据之前不要再动历史压缩**：
  `compactAgentHistory` 已剥历史整帧图，再加一层很可能只是在压一个不是瓶颈的东西。
  ⚠ 历史上这里还写过「`BuildRequest` 已把 `reasoning_content` 截到 400 字尾巴」—— **那条已撤销**
  （半回传违反提供方契约，见决策链三条 ③），别再按它推断请求体成分。

- **思考降档两条**（`ShouldDisableThinking`，docs §26.1 / §47.4）：①「只想不干」抑制 N 轮；
  ② `QST_FAST_THINKING=1` 逃生阀。③「游戏前台降档」（`AiGameForegroundDecisionIsDecisive` →
  `thinking.type=disabled`）**已整体删除（批 D）** —— 引擎不替模型决定「这一轮要不要想」。
  ⚠⚠ **`AiActionExecThinkingScope` / `InAiActionExecScope()` 保留**：名字里带"思考"，但它同时是
  §42 协议闸的入参（`AiDecideStreamBrake` / `AiDecideSlowThinkingRound` 靠它区分「AI 动作执行」
  与「聊天助手」）—— 删它会连带弄坏明确要保留的协议级收束。
  ⚠⚠ **「抑制 N 轮」靠 `NoteThinkingRoundConsumed()` 递减，每发一轮请求必须调一次** ——
  漏调 = 计数器永久 >0 = 此后整个进程都关着思考且**无任何日志**（该缺陷真实出现过）。
  ⚠ §39.3 的「`length` 截断重试**必须**关思考」是协议修复，不是策略，保留。
  ⚠ 自检 `thinking_downgrade_plumbing` 只钉**计数契约**，**测不到调用点是否还在**。

- **屏幕文字索引 = 按画面行分组的「可点清单」**（`FormatOcrTextIndex` / `CollectOcrIndexRows`，
  `src/ai_locate_verify.*`，docs §26.2）：平铺的 `文字(x,y)；…` 让模型无法把文字和画面上的卡片
  对上（不知道哪个价格属于哪张卡、第几张）⇒ 反复猜。现在标 `第N行[纵向%]` + 行内从左到右即
  卡片先后顺序。分组判据：同一视觉行 = 垂直重叠 ≥ 较矮者 50% **且**水平间距 ≤ 较高者 2 倍；每组 ≤12 段。

- **游戏前台裁工具 schema：已整体撤销（批 C）**（`AiActionToolOptions` 裁表开关 + 名单 +
  `lookupMacroAction` 的「本轮已裁掉 X」服务点 + 用例 `game_tool_trim`/`skill_text_matches_toolset`
  全删）：那是**引擎写死「游戏里用不到这些工具」**。**工具表现在永远给全**。
  ⚠ 最贵的教训（换个领域照样成立）：被裁掉的 `fetchWebPage` 恰恰是游戏里查玩法的唯一出口，而两处
  文案还在教模型用它 ⇒ 模型只好拿 `runCommand` 去拼必应搜索，**它不回显输出** ⇒ 连烧两轮什么也没
  查到（用户：「怎么调必应啊」）。**别让静态文案教模型用一个不在能力表里的东西。**
- **★清单必须可执行：产出物的「对象标识」要能落地**（docs §44）：`planSpend` 的 `costs` 简写会
  **伪造名字**（旧写法 `选项3(花费500)` 看着就像个可点目标），描述又写着「照它执行、逐个点名单里
  的卡」⇒ 模型真去 `locateAndClick("600")`；而价格**不唯一**（计划里就有 500×3、300×4）、**未必
  进索引**、**问 VLM 每次框都不同** ⇒ 点空 → 死点表/近点重复连环触发 → 改用**不带目标语义**的
  手算坐标 `mouseClick` → 在 toggle 界面（选卡）上把**已选中的卡又点掉**（用户：「把选了的卡收回
  去又重新选」）。现在占位符 `#3(cost500,noname)`、回执说清「不能用来定位」+ 替代（items 的 name
  = **屏幕上真实存在的短标签**）。⚠ **每份产出都问：照它做，第一个动作做得到吗？**
- **纯数字索引两遍收集**（`kOcrIndexMaxNumericSpans`=16，docs §44）：文字**先占位**、数字有余量
  再补 —— 「满屏价格挤掉确认」靠"文字先占位"保住，不靠**按 OCR 顺序抽签截断**。自检
  `ocr_text_index_rows` / `plan_spend_target_contract`。
- **★静态文案 × 动态工具表必须同一份事实**（docs §43；机制已随批 C 撤销，规则保留）：
  「静态文案 + 动态能力」是两处事实时，**让其中一处从另一处算出来**。
- **「格」感知 / 格表：已整体撤销（批 C）**（`AiInferGridFromSpans` / `BindOcrSpansToGrid` /
  `AiGridLabelLookup` / 网格规格结构体 + `AiElementSource::GridCell` + 用例
  `grid_label_binding` / `grid_both_axes_required` 全删）。撤销理由：引擎从文字自己推「格」、
  再发布带编号的格子，就是**替模型判断「哪些文字属于同一个目标」**；而且实测从未稳定发布过
  （日志里恒为「只推出行周期」）。保留：OCR 文字索引（按行分组）与元素索引（UIA ∪ OCR 合并 + 编号）。
  ⚠ **唯一要记住的教训**：那次事故的根因是**单方向网格也照发**（列周期没推出来），
  ⇒ **判据的「一致性」闸只能加在真正需要周期的那一维**：给行方向也加「等距」闸时，
  「名字一行 + 价格一行」这种天然「近-远-近-远」的行心会被误拒。收紧前先问：这形状是不是就该长这样。
  ⚠⚠ **同名陷阱**：`kClickColorGridN` / `SampleClickColorGrid` / `ClickColorGridChanged`
  是「点击前后采样 RGB 判有没有反应」的**感知**代码（settle 链路，保留项），与「格感知」无关。

- **批 B 撤销的四样（都是「跨帧世界状态」）**（细则见 docs §45~§48）：找图「上一帧命中」快路径、
  定位模板缓存 + 多帧采样（`src/ai_locate_cache.*`）、本地跟住会话（`src/ai_locate_track.*` +
  TrackerVit 接线）、布局记忆 + 相对网格（`src/ai_ui_layout.*`、`locateAndClick(grid=...)`），
  连用例 `find_image_fastpath_gate`/`ui_layout_memory_and_grid`/`locate_track_*` **全删**。
  理由：出错形态都是**静默点到错的地方/静默不点** ⇒ 同一目标**每次都重新识图**（**宁可慢，不许拿
  旧坐标**）。**保留**：`TemplateImageCache*`（纯解码备忘，**不参与判断**）、`vit.onnx` 与
  `docs/local-detection-feasibility.md` §0~§9（**§10 接线作废**；重做要**做成工具**而不是会话）。
  四条实测教训：① **命中就提前 return 的捷径走不到链路内部的记账**（后果：同一坐标连点 11 次、
  最后撞上限）⇒ **加捷径前先答「它的判据错了谁负责作废它」**；② **判据不许用「从未被赋值」的变量**
  （恒 0 ⇒ 命中 ≥2 帧就永远点 (0,0)，极难查）；③ **断言要断坐标值本身**，不是「是否命中」；
  ④ **坐标空间契约错了自检也会全绿**，只有真机暴露 ⇒ 契约类断言必须钉**坐标空间**；
  ⚠ **定位噪声底 84~100px** ⇒ 跳变阈值不能卡在噪声底之下。
  **仍在用的两个同帧快路径**：文字直点与元素索引（`aiOcrDirectIndexThisBatch`/`aiElementIndexThisBatch`）。

- **YOLO / 视觉模型：许可证是硬约束，先查 LICENSE 再谈技术**（docs §11）：本项目**闭源商业分发**
  ⇒ **AGPL/GPL 一律不能用**（Ultralytics 系含 v8 —— 2024 年中从 Apache 改成 AGPL、YOLOv6/7/9、
  YOLOv10/YOLO-World、YOLO-NAS 全排除）。⚠ 三个陷阱：「自己从头训」不算；「只本地跑不联网」不算
  （**分发**才是触发路径）；**只看仓库根 LICENSE，不看项目出身**。⇒ 只能用 **Apache-2.0**（YOLOX 首选、
  DAMO-YOLO、NanoDet、PP-YOLOE/RT-DETR）。⚠ **别用 YOLO 替代找图**（模板匹配毫秒级，YOLO 更慢）；
  唯一落点是替代 **VLM 识图**，且**检测器没有时间维度**（「在往哪走」要靠跟踪 + 运动模型）。

- **OCR 覆盖识图点要有依据**（docs §29.2）：
  「探针在识图点附近读到的同名文字」**不能**无条件覆盖识图坐标 ——
  当 OCR 索引自己都没有该目标时（`文字直点不可用：OCR 索引里没有「X」`），
  那个坐标只是附近**另一个**同名元素。只有索引给出了最近命中（`srcNote` 非空）才允许覆盖。

- **窗口自身的标题栏按钮绝不能被「按名字点」**（`IsWindowChromeButton`，docs §31.1）：
  UIA 里窗口自己的「关闭/最小化/最大化」就是普通的 `Button`，**名字和应用内按钮字面相同**
  （本地化为「关闭」），而且因为它支持 `InvokePattern` 还会在 `PickUiControlByName` 里
  **多拿 120 分**。实测事故：模型要关游戏内的卡牌面板，目标写「卡牌面板右上角关闭X」，
  UIA 一档命中了窗口自己的「关闭」→ **一击把整个游戏窗口关掉、进度丢失**。
  判据（不看名字，名字会本地化且重名）：沿父链上溯，祖先里有 **TitleBar** 控件类型，
  或**直接**挂在 `ControlType=Window` 下 → `titleBarControl = true`。
  ⚠ 四个收紧点要**同时**改，少一个就漏：
  ① `PickUiControlByName` 直接出局（不是降权）；② `InvokeUiControlByName` 只匹配到它时
  给可执行解释；③ `locateAndClick` 的 UIA 档回落识图；④ **`ProbeUiElementAtPoint`**——
  识图路径的落点若落在标题栏按钮上也要拦（模型说「右上角」时识图照样会定位到它）；
  另外 `FormatUiControlListForAgent` 要标「（窗口自身按钮·勿按名字点击）」。
  元素索引（下面那条）同样排除它，且「只匹配到它」时 why 里要说明原因。
  ⚠ 开源对照：UFO / WAA **都没做**这条结构判据（UFO 只把它列为需 CONFIRM 的敏感动作）。
  自检：`element_index_and_resolve` 覆盖索引侧的拒选；`IsWindowChromeButton` 本身
  要真实 UIA 树，**无单测**。

- **按显示名启动程序必须走本地解析，禁止 Win+S 搜索**（`ResolveAppLaunchTarget`，docs §31.2）：
  原 `openAppViaSearch` 是「Win+S → 输入 → Enter」，而 Win10/11 搜索**默认带网页结果** ——
  输入「植物大战僵尸融合版」回车打开的是**浏览器里的 Bing 搜索**（用户实测报障）。
  现在纯本地解析：存在的路径/`.lnk` → 桌面快捷方式 → 开始菜单快捷方式（深度 2）→ `App Paths`
  → `PATH`；命中后复用既有 `openFile` 动作（`ShellExecute` 解释 `.lnk`），**不另写启动逻辑**。
  解析不到就**明确失败**并回近似名字/窗口台账，**绝不退回搜索**。
  ⚠ `runProgram` 解析不出 exe 时也走同一套；⚠ 把全仓「改用 openAppViaSearch」的提示文案一起改掉
  ——留着旧文案等于还在教模型走那条死路。⚠ 装饰后缀（「XX快捷方式」「XX 程序」）要剥掉再匹配，
  且**每剥一次都要 Trim**（`notepad 程序` 就是这么漏的）。自检 `resolve_app_launch_target`。

- **★`zoom`：给模型一个放大镜**（docs §50 / §51）：整帧降到 1024 后小字只剩几十像素（实测 15 张卡
  每张 ≈40 upload px）⇒ `zoom(x1,y1,x2,y2)` 按 **upload 截图像素**给区域、从**原始分辨率**裁剪回传；
  也可 `target="屏幕上的短标签"`（走**本帧元素索引**，与 locateAndClick **同一张表**）。
  ⚠ 四条别拆：① 坐标口径与元素索引**完全相同**（零换算）；② 回执写清「区域、倍率 N、
  **图内坐标 → upload = x1 + 图x/N**」；③ 超上限（`maxEdge` 默认 = 上限 = **1280** =
  `kAgentAttachmentMaxLongEdge`）**收窄区域并如实说明**；④ **倍率按「模型真正收到的那张图」算**
  （附件链路还会缩到 1280；实测回执报 ×2.5、真值 ×2.0 ⇒ 换算出的 upload 坐标**右端偏 128px**）。
  ⚠⚠ **落地文件名必须每次不同**（`FormatAiZoomTempPath(dir,pid,seq)`）：`[[AGENT_IMG:…]]` 是
  **整批工具跑完之后**才读盘编码的，固定名时同轮第二张覆盖第一张 ⇒ 模型收到**两张一样的图**并
  **弃用了这个工具**。旧图由 `PruneAiZoomTempDir` 按年龄清。⚠ 落盘用 `SaveHbitmapPng`（`SaveBitmapToFile`
  是未压缩 BMP）；它内部经 OpenCV `imencode` ⇒ 无 OpenCV 时**如实报失败**。
- **★一组坐标必须每一处都说清是哪一个**（docs §50 / §33.1 / §51）：本仓同时有 **upload 截图像素**
  （`mouseClick`/`locateAndClick` 的目标、两个索引给的坐标）、**屏幕绝对像素**、**`computer` 的 0~1000**。
  ⚠ 回执曾写「（0~1000，与 computer/mouseClick 同一套）」——**后半句是错的**；观察帧落点标注也**一句话
  里混过两套**（红叉画在 upload 空间，文字只写「屏幕(2155,255)」）⇒ 模型自己去除 1024/2400。
  现在三套**各自标名**，逆运算统一走 `MapScreenPointToApi`（**别各处自己写一遍**）。

- **可点元素索引 = 所见即所得的主通道**（`BuildAiElementIndex` / `AiElementIndexResolve`，
  docs §32）：把 **UIA 控件 ∪ OCR 文字**合成**一张带编号的表**，`locateAndClick(target)`
  **先查表拿坐标（0 次识图）**，表里没有才回落识图（对齐 UFO/UFO² 的 UIA+OmniParser merge、
  WAA/Navi 的**元素 id 动作空间**）。四条别拆：① **合并要保守**（IoU ≥ 0.35 或一方中心在
  另一方内，**且**名字对得上）—— 并错 = 点错东西，宁可清单长一点。② **编号在排序后分配**
  （上→下、左→右，行高差 12px 内同一行），否则「第 3 个」会对不上。③ **窗口自身按钮不进表**。
  ④ 解析：精确 > 双向包含 > 宽容档；同档多条取最近，两者几乎一样近（<24px）或无线索 →
  **判歧义拒绝**；**纯数字/单字只允许精确档**（角标价格要能按价格选，但满屏短标签模糊匹配必错）。
  ⚠ **它是快路径，必须登记作废**（硬规则第 4 个实例）：settle 判「无反应」→
  `aiElementIndexThisBatch` → 整表作废；不登记则错坐标被反复查表复用，而这条捷径**绕过了
  识图链路里的记账**（§27.1 同一口坑）。⚠ 索引**只在重建路径里覆盖**：「界面未变」提前 return
  时不重跑 OCR，保留旧索引是**对的**（坐标仍成立）；每帧清空反而让未变帧之后永远查不到索引。
  ⚠ 命中索引后**点击链路一个守卫都不少**（遮挡/灰化/窗口按钮/落点标注/settle/近点守卫），
  回执要明写「0 次识图」。自检 `element_index_and_resolve`。

- **★「半视觉」：UIA 清单要带角色/能力/状态，且编号必须能真的用**（docs §73）：
  对标 Windows-MCP（其 `Snapshot` 默认 `use_vision=False` + `use_ui_tree=True`）。三件别拆：
  ① **类型表是单一事实来源**（`window_mode::UiControlTypeTable`：类型 id / 角色标签 / 动作能力 /
  是否需可聚焦闸**写在一张表里**）——类型清单、中文角色、动词是三份必须同步的事实，
  分散在三处 switch 必然漂移；**自检遍历这张表本身**，不许自己再抄一份常量（抄了必然漂移且漂移时仍全绿）。
  ② **`action` 与 `state` 是事实不是建议**：`click/fill/toggle/select/slide/scroll/focus` +
  `focused`/`value:"…"`/`range:0-100`/`toggle:on`/`state:expanded`/`v:42%`/`readonly`。
  ⚠ **未知类型给 `focus`，绝不冒充 `click`**（点说不清的东西会打错目标）；
  ⚠ **OCR 条目不许有 action/state**（本地没证据 ⇒ 不猜）；
  ⚠⚠ **密码框的值一个字都不回传**（UIA 在部分应用给明文！`get_CurrentIsPassword` 要在读
  ValuePattern**之前**判），自检断言台账里不得出现 `value:"`。
  ③ **编号能被工具用**：`locateAndClick(elementId=N)` → `AiElementIndexById` 直查同一张表；
  查不到一律 `nullptr` **绝不兜底成近似编号**（错位必然点到隔壁），且窗口自身按钮/灰控件
  **永不入选**（与按名字同一把尺）。⚠ 清单标题里**必须写明 `elementId` 可用** ——
  不说，模型就永远不知道有这条路（能力存在 ≠ 能力可用）。
  自检 `uia_action_verb_table` / `uia_control_list_carries_action_and_state`（WindowModeSelfTest）、
  `element_index_facts_and_id_binding`（AiActionRouterSelfTest）。
  ⚠ 改这里必须同时看 `ui_element_probe.h` 的 `UiControlTypeRow`（加类型=加一行，别只改 switch）。

- **工具参数的口径必须在给模型的每一处说同一句话**（docs §33.1）：
  元素索引第一版写了「屏幕绝对像素，与 mouseClick 同一套」——**这句话是错的**，
  `mouseClick` 要的是 **upload 截图像素**，2560×1440 截成 1024×576 时差 2.5 倍；
  模型察觉了矛盾，花 10~40KB 思考在两个说法之间反复权衡（单轮 45s+）。
  现在 `FormatAiElementIndex(...)` **自己换算成 upload 像素**（与 `MapApiPointToScreen` 互为逆运算），
  标题写明图像尺寸与口径，自检**断言换算值本身**（屏幕 (600,450) 必须显示 (240,180)）。
  ⚠ 加任何「给模型看的坐标」时先问：**这个字段的消费方是谁**（mouseClick？locateAndClick？
  computer 的 0~1000？），照它的口径写，别凭印象写「同一套」。

- **OCR 丢字要有宽容档**（`AiElementIndexPartialMatch`，docs §33.2）：
  `AiOcrLabelMatchTier` 的长度比闸（≤40%）会把「自选僵尸卡牌」vs OCR 读到的「自选僵尸卡」挡掉 ⇒
  模型在索引里**查不到自己刚看到的文字**，回去识图再困惑一轮。宽容档只在两边都是 3~12 字、
  无空格标点的 CJK 短标签时启用：①一方是另一方的子串 → 命中；②LCS ≥ 较短者 67% 且首字相同 → 命中。
  ⚠ 档位**低于**精确/包含档：`AiElementIndexResolve` 里有精确命中就整批丢弃宽容候选（`sawTier2`）。
  ⚠ 反例要选**真的不像**的：第一版拿「请点击这里确认」vs「请点击那里确认」，但两者 LCS 85%、
  宽容档命中是**对的** —— 测的是错误预期。
- **遮挡拦截之前先尝试自愈**（`engine_script_run.cpp` 的 locateAndClick，docs §33.3）：
  前台被本软件自己的调试窗/浮窗抢走时，定位点其实仍属于目标程序。
  旧逻辑只回「请先 activateWindow」，模型照做要再花 2~3 轮，而**那一轮识图已经付过钱**。
  现在：`WindowFromPoint` + `GA_ROOT` 取该点所属顶层窗口 → 进程名 → `ActivateByProcessName`
  拉回前台 → `Sleep(120)` 后**重新**做遮挡校验 → 通过就继续点，不通过才拦截。
  ⚠ **只重激活「该点所属的那一个窗口」**，绝不激活别的程序（那是打错目标）。
  「按进程激活」抽成具名 lambda（`activateByProcessFn`），与 `agentHooks.onActivateByProcess` 共用。

- **闸门的状态必须活得过「它要跨的那一轮」**（docs §39.2；与 §26.1 同一形状，又踩一次）：
  「思考失控闸」的 `consecutiveSlowThinkingRounds` 原来是 `SendMessage` 的**局部变量**，
  而 AI 动作执行是**外层每轮调一次 `SendMessage`**（日志里内层「第 N 轮耗时」反复从 1 重数）
  ⇒ 计数器每轮归零 ⇒「连续 2 轮慢」**永远不成立** ⇒ 闸门形同不存在
  （实测单轮等模型 16.2s → 62.3s 一次没触发 = 用户说的「半天没反应」）。
  现在状态是文件级 atomic；判据抽成纯函数 `AiDecideSlowThinkingRound(streakBefore, apiMs,
  thinkingEnabled, inActionScope, **actedThisRound**)` —— 跨轮状态变成**显式入参/出参**才测得着；
  并新增「**单轮 ≥30s 当场处置**」（一轮就能 60s，等第二轮的 120s 等不起）。
  ⚠ 两条否定条件保留：只在**本轮确实开着思考**时计数（否则变「永久关思考」正反馈）、
  只在 **AI 动作执行作用域**内动手。
  ⚠⚠ **`actedThisRound` 是这条闸的宾语**（docs §51，实测误伤）：要判的是「**想很久，而且没换来任何
  动作**」，墙钟只是代理量（大请求体 250~340KB 含多图/低带宽都会变长）。实测某局连续两轮
  11.9s / 16.1s **都在 zoom 放大读图**（正常工作）却照旧被关思考，关掉思考的下一轮模型把**两轮前
  那个工具批次原样重放**（等模型只用 1.1s），在游戏里又点了一次同一处 —— 引擎的「省时间」制造了
  一次多余点击。⇒ 出手了（≥1 个工具调用）就**不计数、不抑制**，并把旧 streak 归零。
  自检 `slow_thinking_round_gate`。
- **`length` 截断 = 输出预算用光了**（docs §39.3）：重试**必须一并关思考**，否则同一段 prompt
  会再炸一次（实测白烧第二个 60s）。⚠ 但**别断言是「推理」吃掉的** —— 实测两轮 57 895 /
  54 732 字节**全在 `content` 里**（心跳行印的是「回复」）⇒ 关思考对它毫无约束力。
- **★★决策链三条（P0~P3，docs §49）**：①**看门狗收束 / `length` 截断之后的重试必须一并关思考**
  （收束的含义就是「这一轮想太多了」；实测事故：收束后用**还开着思考**的请求重试 ⇒ 模型又只产思考
  ⇒ 网关回空体 ⇒ 判致命 ⇒ **整个宏当场结束** —— 救火闸自己变成了火源）；
  ②**「服务器返回空响应」不再判致命**：官方《Thinking Mode》写明「预算被推理吃光时 `content` 为空、
  `finish_reason=length`，这**不是**故障」；按 `AiDecideEmptyResponseRecovery`（纯函数、逐格自检、
  **有界 2 次**）注入「直接调工具」纠偏 + 关思考后重来。
  ⚠ 重试必须**重建请求体**才算真的关了思考（只翻标志位 = 同一份请求，网关看到的一样）；
  ③**带 `tools` 时 `reasoning_content` 必须完整回传**（官方契约，否则 400）——
  曾经的「截成 400 字尾巴」**已撤销**：半回传违约，且**推理被截掉会让模型每轮从头重推**
  （实测同一件事推了 4 遍以上）。体量改用**源头**手段：动作执行下发 `reasoning_effort=low`
  （`ModelSupportsReasoningEffort` 只放官方写过的 DeepSeek v4/v5 与 o 系列；**聊天助手不动**）。
  ⚠ 「连续 N 轮无变化」必须**分开报两个事实**（界面帧差分 / 工具批次指纹）——
  取 `max` 合成一句是**说谎**（N 可能来自界面计数）。
  ⚠ 索引里的「文字」条目**只表示屏幕上有这几个字**（模式标签/标题/计数器都会出现）：
  索引标题与回执都已如实标注**可点性未经验证**，别改回「可点元素索引」那种越界措辞。

- **★判据的宾语必须是「事情本身」，不是「事情碰巧长成的样子」**（`AiDecideStreamBrake` /
  `AiDecideNoToolCallAnswer`，docs §42 —— §39.4/§40.1 同一形状第三次）：两条闸原来**都绑在输出
  形态上**（看门狗要 `contentBytes == 0`、纠偏要 `content.empty()`）⇒「**说了一大堆**却不调工具」
  两条都够不着 ⇒ 只能烧到 `max_tokens`；更糟的是截断后那段散文会被**当成本轮「最终回答」返回**
  ⇒ 这一轮**什么都没干**（用户：「选了两张卡然后卡在那里不动了」，两轮各 ~50s、零进展）。
  现在：① 看门狗按 `思考+正文 ≥ 12KB` 且**毫无工具意向**收束（**只在 AI 动作作用域**——聊天助手的
  长回答是用户要的；工具 JSON 一开始组装就不拦）；② 被截断/被收束且无工具调用 → 注入「只准调工具、
  别写解释」纠偏，且**有界**（2 次后认输，§36.6/§40.2）。自检 `stream_prose_brake` /
  `no_tool_call_answer_gate`；**看门狗是线程内 lambda，引擎侧接线无自检覆盖**（只静态核对
  `ExecuteAiActionExecute` 把动作执行期的 `SendMessage` 都包在 `AiActionExecThinkingScope` 内）。
- **恒真的判据不是判据**（`AiJudgeUiReaction`，docs §39.4/§40.1，同一个形状踩了两次）：
  ① `reacted` = 「与 baseline 的**任意**差异」⇒ 游戏每帧重绘 ⇒ 必真；② 三档后「**局部**变化在别处
  也算反应」在动态画面上**同样恒真** ⇒ 每次点完都告诉模型「已生效」。现在动态前台**只有「落点附近
  有局部变化」算反应**，拆两个字段：`reacted`（有无正证据 → 决定 settle 结局与回执措辞）/
  `conclusive`（判得确不确定 → 决定要不要做**惩罚性**动作）。⚠⚠ 「无法归因」**不许**记死点表/
  连败表/作废缓存：画面自己在动而已，记了会把**正确的**点击永久拦下（§36.6）。
  **只有「一个变化区都没有」才是确定的没反应**；无落点动作（键盘）也没有可归因的点。
  ⚠⚠ **`!dynamicForeground` 那条分支不能删**：静态界面「一大片全变了」是**真重绘**
  （点链接整页跳转就是一个巨框），它保证 Web/Office 逐字等价。⚠ 「局部 vs 大面积」看**占画面比例**
  （`AiRoiIsLocalMotion`：≥6% 面积或横纵都过 55%）—— 绝对像素阈值会把 `263×104`（0.74%）的卡片
  高亮判成「大面积」，真反应被丢掉。settle / 逐步校验 / 回执**共用这一把尺**；落点要**换算到位图
  局部坐标**再传。自检 `ui_reaction_locality`；引擎侧接线无自检覆盖。

- **识图落点守卫必须是「一份实现」**（`blockVisionLanding`，docs §41.1）：§31.1 那条「窗口自身
  标题栏按钮」守卫只加在**主识图链路**上，而（当时还有的）「错点自纠」是后来的备用项 ⇒ 同类事故从
  **另一条路**又进来一次：自纠补点落到 `(2522,16)`（距顶 16、距右 51）—— **一击把整个游戏窗口关掉**
  （用户：「咋把游戏关了」）。⚠⚠ **不能只依赖 UIA**：这局窗口 **`UIA 控件 0 条`** ⇒
  `titleBarControl` 恒 false ⇒ 那道守卫**根本不会触发**。现加**纯 Win32 几何**判据
  `IsPointInWindowNonClientStrip`（窗口矩形内 + 客户区矩形外 = 标题栏/边框），头文件内联 ⇒
  自检不必链 UIA 层就能钉。**结构事实比控件树可靠**。
  ⚠ 客户区 == 窗口矩形（**无边框全屏**）时**不许拦** —— 游戏内的 X 就在右上角，拦了是减少能力。
- **判断日志不能「结局与判据各说各话」**（docs §41.3）：`reactionWhy` 把判据结论当**充分**条件写进日志，
  而最终判决还要 AND 上「整帧差分过阈」⇒ 出现 `UI settle：无反应 …；有局部变化落在落点附近 → 算反应`
  这种自相矛盾的一行。两半写进同一句话 —— **日志说谎和回执说谎一样糟**。

- **★★跨轮记账三条铁律**（docs §38，必读）：跑偏**不是**模型笨，是引擎把**陈旧的目标名**配上了
  **别人的落点**，再反复告诉它「已生效」（语义复用已删，但下面三条照旧适用）。
  ① **「当前正在处理的对象」要用作用域**（RAII），别用「记得清空」——一定会漏。
  ② **判据不许用「上一次」的值**：`AiUiLayoutRemember(layoutKey, lastLocateScreenRect)` 写在那个
  变量被更新**之前** ⇒ 布局记忆整条落后一次定位（与 §29.1 同类：**用了不属于本次的值**）。
  ③ **回执必须描述「这一批」**：`lastBatchOutcome` 跨批泄漏 ⇒ `本批完成 0 步` 却回执「已生效」。
  **回执说谎比没有回执更糟**；`executeActionsJsonNow` 进门先清零。
- **识图回答要分「格式不对」和「答了个空」**（`ParseVisionLocateAnswer`，docs §38.4）：
  模型答不出来会回**空框** `[0,0,0,0]`，旧解析退回坐标对抓到 `(0,0)` ⇒ 点屏幕 **(0,0)**、
  报「已生效」、还存成定位模板。空框/零宽零高一律 `NoAnswer`（按未找到处理，绝不猜坐标）；

- **动作回执必须带「成没成」，且不能靠解析文本**（`AiBatchOutcome`，docs §36.1/§36.2）：
  用户主诉「操作已经完成了，但还在重复选卡」—— 日志显示同一位置连点 3 次，**第 1 次其实成功了**
  （settle「仍在变化 差分1083bp」），但回执只有「已点击屏幕(x,y)」，**一个结果字都没有**，模型只能
  靠猜 → 「再点一次确认」→ 这次没反应 → 记入死点 → 手算坐标再点。这是**决策路径**问题：缺字段，
  不是缺守卫。现在回执写 `[结果] 界面已经变化 → 这一步已经生效，不要重做` /
  `[结果] 界面没有变化 → 这一击很可能没生效`。⚠ 判据必须用**枚举**
  （`AiBatchOutcome{Unknown,Reacted,NoReaction}`）由 `executeActionsJsonNow` 记录后传给调用方。
  **第一版写成 `execMsg.find(L"settle无反应")` —— 措辞一改就静默失效**，而且同一个词在多个分支都会
  出现。别再用文本匹配做判据。

- **★★撤销总账（第 1~5 批）与总原则**（docs §45~§48；**先读这一段，再看下面各条**）：
  **永远不要写死策略，把决策留给模型本身；Skill 只提建议** ⇒ 引擎只做**感知 + 执行 + 如实回执**。
  判据是**句子级**的：**事实留 / 策略删 / 谎必删**（三者常贴在同一段代码里，**别按「块」删**）。
  已撤：①否决型点击闸（死点/近点/重复预算/连败/错点自纠）；②三张跨帧状态表（布局记忆 / 定位缓存 /
  跟踪会话）+ 找图「上一帧命中」快路径；③格感知 · 游戏硬指令注入 · 游戏裁工具（含 §43 整套机制）；
  ④站点专属启发式（B 站家族）· 引擎代导航 · ★视觉闸 · 思考降档 · lookahead 预规划 · 识图提示注入；
  ⑤按**提示词子串**裁工具表（`fillTableOnly`）。**保留**：文字直点 / 元素索引（同帧感知）、
  本地判断表（只**留痕**不注入）、§42 两条协议闸、`planSpend` 与纯数字两遍收集。
  ⏸ **破坏性保护族暂缓（用户裁定）**：拦 Escape / 盲 Tab / 藏窗连按 / `blockVisionLanding` /
  `GuardPointerClickContext` / 「抓过网页后启动程序」的 **fail-safe 安全联锁** —— **不要顺手删**
  （理由见 docs §48.5：它们保护用户的**已有状态**；安全联锁拒绝 ≠ 引擎替用户做决定）。
  ⚠ 改完**必须**跑 `tools\verify\rollback_audit.ps1 -Strict`（A~E 全 GONE + 四个 keep 组 INTACT + 21/21）；
  ⚠ 它是**子串**扫描（**改名也能变绿**、注释里的符号名也算命中）⇒ 真正的保证是**语义核查**（那道闸在不在），
  不是名单变绿；遇到「改个名字就绿」要停下来报告。

- **★决策链第二轮：七处「引擎替模型下结论/下命令/给错数字」（docs §51）**：① `zoom` 同轮两张图
  **共用一个文件名**互相覆盖 ⇒ 模型收到两张一样的图并**弃用了该工具**（`[[AGENT_IMG]]` 是**整批工具
  跑完之后**才读盘编码的 ⇒ 旧注释「提取时当场编 base64」是**错的**；现在唯一名 + 按龄清理）；
  ② `zoom` 回执按**裁剪图**算倍率，而模型拿到的是缩到 1280 之后的图 ⇒ 换算出的 upload 坐标
  **右端偏 128px ≈ 3 张卡**（现在上限与附件管线**共用** `kAgentAttachmentMaxLongEdge`，回执按
  **交付尺寸**再算一遍）；③ **动作执行里不再注入「重复工具调用 → 去给最终回答」引导** ——
  「同一动作 + 逐字相同回执」在那里**恒真**（重复同一动作本来就是正常操作），一句话等于教正在打
  游戏的模型放弃；**事实照报**，聊天助手/写脚本那条路保留；④ 落点标注**同时**给帧内(upload)与
  屏幕两个口径；⑤ 工具附图**不再谎称「脚本引用的图片」**，每张图**前面**插同序号标题（与回执
  `[附图 N]` 对齐）；⑥ **归因不了时引擎只报事实**（「先 screenshot / 换个落点」这类祈使句归
  Skill，`game.md` §0.7 本来写得更具体）；⑦ 慢思考闸补上宾语 `actedThisRound`。
  ⚠ 可迁移三条：**同一批里会被多次产出的资源必须天然唯一**（别赌「它马上被消费掉」）；
  **交出去的和消费者拿到的不是同一个东西时，回执必须按后者写**；**恒真的判据不是判据**。

- **★联网查资料 = `webSearch` → `fetchWebPage`（docs §52）**：查玩法/文档/报错**先 `webSearch`**
  （零 API key、宿主 WinHTTP 直连**搜索结果页**解析成 标题/URL/摘要，`readTop=1~3` **同一轮**连正文
  一起读回），再 `fetchWebPage` 读选中的 URL。⚠ **`openWebpage`+`observePage` 只给需要点击/登录/翻页
  的交互网页**（它**抢前台**；实测模型为查玩法开真浏览器 → 3 轮 + 丢前台 + 拿回别的游戏摘要）。
  ⚠ 旧描述「禁止抓**搜索**/用户/JSON API」被读成「禁止抓搜索」⇒ **它没试就绕道**；现在写明只禁
  **站点内部 API**（api.\*/graphql.\*），**搜索页能抓**。⚠ 实测（2026-09）：DDG Lite/html **本机超时**；
  Bing `format=rss` **查询被降级**（不可用）；Bing HTML `li.b_algo` 可用；**中文别用空格堆词**。
- **★正文抽取要 readability 式，不是去标签**（`HtmlExtractMainText`，docs §52；Readability /
  trafilatura / go-readability 同思路，Apache-2.0）：剔整块样板（script/style/nav/header/footer/
  aside/form）→ 按 class/id **剔无 `<p>` 的样板容器** → 候选按「文本量+标点+段落数+关键词+深度−
  链接密度」打分 → 在最优容器里**只输出叶子文本块**（p/h1-h4/li/td…，带轻量 `#`/`-` 前缀）。
  ⚠ 必须走 `ReadUrlAsText`（`fetchWebPage`/`webSearch` **共用一份**）；抽不到就**如实回退**整页纯文本。
  ⚠⚠ HTML 三个坑（实测）：**标签结尾要跳引号**（属性里的 `>` 会截断标签，漏出 `href="data:…base64"`）、
  **隐式结束标签**（`<p>` 常不闭合 ⇒ 区间吃到文档末）、**`<br` 只替前缀会留下 `/>`**。
  ⇒ 改抽取算法**必须**用 `--extract-smoke <url>` 对着**真页面**量（合成 fixture 抓不到这三类）。
- **★历史附图要有时限**（docs §52）：`ChatMessage::image_keep_rounds`（0＝只在它还是最后一条 user
  消息时留／N>0＝再留 N 轮／-1＝永不剥），判据 `ShouldStripMessageImages` 逐格自检。由来：观察类附图
  原为「永不剥」⇒ 实测 `请求体拆解 835KB = 图 704（6 张）`，每轮都在为**过期快照**付钱。

- **★「只会放普通僵尸」= 感知只说了一半（docs §53）**：引擎只给了**文字坐标**（价签
  `50/75/600` 在哪），**没给「这个价签标注的是哪张卡」** ⇒ 模型只能一轮轮 `zoom` 看图猜卡
  （实测一轮 3 张图、请求体 477~564KB、单轮等模型 19s），猜不出来就退化成只点唯一有把握的那张
  （最便宜的）。⇒ 新增**通用版面推断**「短标签 → 它标注的图标槽」
  （`PairCaptionRowWithIconBand` + `SampleIconBandFromBitmap` → `AiElementSource::LabeledIcon`，
  证据写进 `AiElementEntry::note` 一起发）。出处：OmniParser（图标框+邻近文本配对）/
  UFO² 的 `LabeledBy` / PaddleOCR·tesseract 的投影剖面。
  ⚠ **采样层只拷像素、判据全放纯函数**：第一版把「列墨迹」预算在采样层、与**行中位数**比，
  图标占条带一半以上时中位数变成图标自己的颜色 ⇒ **极性翻转**，4 张卡被读成 4 条缝（自检抓到的）。
  ⚠ 配对成功必须**吃掉那条价签文字**（否则同档两条 ⇒ `locateAndClick("600")` 判歧义 ⇒ 白做）。
  ⚠ 判据一律往紧里收：配错 = 点到别的卡，不发布只是回到旧行为。
- **★`zoom`/点击回执只许描述「消费者手里的那个东西」（docs §53）**：① 请求区域超上限**绝不静默
  居中裁剪**（实测模型要整条卡槽、只拿到中间一段，却据此认定「卡槽是空的」白烧两轮）⇒ 改成
  **分块**（`PlanAiZoomTiles`，≤2 块、每块原生分辨率，回执**逐块**给区域+倍率+换算基准），
  块数超限才整块降采样并如实说「放大倍数已丢失」；② 落盘就用**送图链路的编码**
  （`SaveHbitmapJpeg` + `kAgentAttachmentJpegQuality`：JPEG q82/长边 ≤1280）⇒ 磁盘字节数**就是**
  模型拿到的字节数（旧实现写 PNG，实测 1365 KB，而模型手里是 ≈200 KB 的 JPEG）；
  ③ `DescribeClickPointForModel` **第一套就给 upload 截图像素**（旧文案把屏幕像素和 0~1000 摆前面、
  还补一句「这两个都不是你要的」，**却始终没给**那个真的）。
  ⚠ 加导出函数前先看自己在不在 `namespace {` 里（同日两次：`SaveHbitmapJpeg` 与并行会话的
  `MakeReadDocumentTool`）—— 报错是 `LNK2019`，或全局声明+匿名定义并存时的 `C2668`。
- **★「卡在一个界面不停思考」= 感知给不出宾语 + 唯一想换的手段是死的（docs §54）**：
  实测选卡界面停 13+ 轮，`标签→图标槽推断 0 条` 且**一行诊断都没有** ⇒ 根因是配对复用了
  **给模型看的**那份行表（纯数字限量 16 条，而网格有 ~50 个价签 ⇒ 每行 <3 段 ⇒ 前置条件静默跳过）。
  ⇒ **限量是展示层的事，不许回流成感知层的输入**（`CollectOcrIndexRows(ocr.lines, 64, 4096)` 只给配对用）；
  条带上沿让开上一行标签（`prevRowBottom+2`）；块边界改「邻居标签中点」；**每条被拒的路都打一行
  `[诊断] 图标槽未配对：<原因>`**（发布 0 条时原因必须能看见）。
  ⚠⚠ **`mouseDrag` 曾经是个不可能成功的工具**：`mouseDown`/`mouseUp` 没写坐标 ⇒ 计划恒为
  `(0,0)→(0,0)`，而报错文案是**为 `mouseClick` 写的**（「点击输入框须给截图坐标…」）⇒
  模型以为是自己点输入框的方式不对，换个用法再烧一轮。现在起点写在 `mouseDown`、终点写 `mouseUp`
  的 `endX/endY`，报错文案按动作类型分。⇒ **一个只会报错的工具比没有这个工具更贵**；
  **为动作 A 写的提示被动作 B 复用就是假信息**。自检 `caption_icon_pairing` / `mouse_drag_executes`。
  ⚠ 配对**仍未在真机帧上验证**：下一局先收那几行诊断，按它点名的闸再调阈值。
- **★「标签→图标槽」在真机上被一道几何闸挡在判据**外面**（docs §56）**：真机卡槽 ≈112 native px
  宽而价签只有 ≈30 px 宽 / 26 px 高 ⇒ 相邻价签间距 ≈82 > `hRef×2 = 52`
  ⇒ `CollectOcrIndexRows` **每个价签各自成行** ⇒ 配对的 `spans.size() >= 3` **永不成立**
  ⇒ 一行诊断都不打（那段诊断**长在门槛之内**），模型只能逐张 `zoom` 手量像素（实测 8+ 轮，
  同一张卡的 x 在它笔下从 671→690→601→555→696 反复变）。
  ⇒ 行分组的 `maxGapFactor` 拆成**两个消费者**：给模型看的索引仍是 **2**（那是阅读顺序口径），
  **版面推断传 64**（行内等距/等高由 `PairCaptionRowWithIconBand` 自己判，不需要间距闸）。
  ⚠⚠ **夹具必须用真机几何**：旧的 `numeric_cap_split` 价签宽 28 / 槽距 43 ⇒ 间距 15 ≤ 20，
  **永远测不出**真机的 82 > 52。现在夹具里写死「槽 112 / 签 30×26」并断言
  **默认闸切 1 段、宽闸并 1 行 14 段**（两边都不一样，才算真的把两个消费者分开了）。
- **★空体的判据不许绑在「报错形式」上（docs §56，§42 同一形状第四次）**：重试循环里
  「空体 ⇒ **重建请求体 + 关思考**」那条**唯一有用**的处置挂在 `apiError.empty() && text.empty()`
  上，而传输层报空体时**会带一句错误文本**（`服务器返回空响应。`）⇒ 条件恒 false
  ⇒ 三次全是「重发同一份请求」（实测 `locateAndClick(target="600")` 白烧 **21.7s** 后报错）。
  现在提取 `emptyLike = emptyResp || apiError 含「空响应」` —— 与同函数里的 `retryable`
  **用同一个判据**。⚠ 顺带：`zr.errorMessage` 自带 `[错误] `，外面又加一次 ⇒
  `[错误] [错误] API 请求失败：…`（回执说谎），已只留一层。
- **★批量多步只回报「首次点击」（docs §56，未改）**：模型一轮塞 10 个「选卡+落点」对
  （`计划执行 39 个`，这是省轮次的正解），拿回的却只有 `批量逐步校验：点击 20 次，首次点击附近
  无变化` ⇒ 它不知道哪几个落成了，只能回头一轮轮猜。**修法**：每次点击分别报「落点附近有没有
  局部变化」这条**测量**（措辞仍是「无法确认是否生效」），别替模型合并成一个结论。
- **★「选完卡不会放僵尸」= 模型看不见自己的落点与结果（docs §55）**：卡片**没有文字**、
  进不了元素索引 ⇒ 模型只能手算坐标用 `mouseClick`；而 `SummarizeExecutedActionsJson`
  **没有坐标动作的分支** ⇒ 回执只有「已执行:mouseClick」，**连个数字都没有**；
  而 `[结果] 界面已经变化 → 这一步已经生效，不要重做` 只长在 `locateAndClick` 的回执里
  ⇒ 模型无法判断卡选没选中，只能反复点、反复开合同一个面板。
  现在：坐标动作回执**回显落点**（upload 原值，零换算）；`[结果]` 上移到**批次**那一层
  （不管走哪个入口**恰好说一次**，判据是 `lastBatchOutcome` 枚举）。
  ⚠⚠ **`computer(left_click_drag)` 发的是没人读的字段名**：`mouseDrag` 在脚本 schema 里是
  **`x/y` 起点 + `endX/endY` 终点**（`script_action_builder.cpp` / `script_io.cpp` 只读这两个），
  别名却发工具层的 `fromX/toX` ⇒ 退化成 **(0,0)→(0,0)** —— 上一轮 `mouseDown/mouseUp`
  缺坐标那个事故**长在另一条入口上**。自检 `coord_action_receipt_names_landing`。
- **★设置里的 `maxTokens` 不许被引擎偷偷改（docs §55）**：设置界面写 393216，两处代码
  各自静默钳成 **8192**（`BuildCfgFromSettings`）/ **16384**（`ai_action_service.cpp`），
  **日志一个字都不提**（用户原话：「这个预算是哪里来的？不是设置界面设置的吗？」）。
  现在两处都**不钳**（只保下限），模型相关的上限只剩 `ClampApiMaxTokens` **一份**（协议要求），
  并且**实发值写进「请求体拆解」那行**：`；max_tokens=N（取自设置）` /
  `（设置里是 M，按该模型输出上限收敛）`。跑飞由 §42 看门狗 / §39.2 慢思考闸管，
  **不靠偷偷调小输出预算兜底** —— 那只会把工具调用截断在半路。
  ⚠ 可迁移：**「替用户调小一个他自己设的数」和「替模型下命令」是同一类错**，
  数字类的尤其隐蔽：不报错、不写日志，只是安静地不生效。

- **★第一批：否决型闸已整体撤销 —— 引擎不做决策**（**细则与事故见 docs §45~§48，此处只留索引**）：
  已删：`AiSkipDeadClick`（死点表）/`AiSkipRepeatSuccess`/`AiDecideNearDupClick`/`AiDecideRepeatRefusal`/
  `AiDecideLocateRetry`/`NormalizeLocateTargetKey`+`LocateTargetsEquivalent`+`aiLocateMemos`/
  `AiDecideSelfCorrectClick`（错点自纠**整条链路**）—— 判据、记账、「跳过…」回执文案**一并删除**；
  **模型发来的点击一律如实执行**（只剩落点守卫与坐标越界拒绝）。
  ⚠ 勿据已删自检名重建：`degenerate_loop_gates`/`near_dup_guard_release`/`repeat_refusal_shared_budget`/
  `self_correct_shift_bound`/`locate_target_key_normalize`。五条可迁移教训（§45~§48 有事故细节）：
  ① **判据写在哪一层，决定它能不能拦住事情**；② 「点过且有用」≠「不该再点」——判据的宾语是
  **动作身份**不是**位置**；③ **守卫不能把模型永久锁在外面**；④ 先问这是**能力问题**还是**开销问题**；
  ⑤ **出口只有真的执行了才算数**（两道闸各自「拒满放行」⇒ 闭合 livelock）。

- **观察帧落点标注：让规划模型顺手验收「上一次点在哪」**（`AiFrameClickMark`，docs §30）：
  红叉过去**只画在 Zoom 放大图上**（识图子模型的读者），每轮真正决策的规划模型拿到的观察帧上
  没有任何落点信息 ⇒ 只能靠「界面变没变」反推 ⇒「点偏了三格还在继续点」。现在真点下去时记坐标、
  进入定位时记目标，组帧时 `EncodeBitmapForAiAnalysis(..., &clickMark)` 画上。
  ⚠ 四条别拆：① **只画在 `CopyImage` 副本上**（原图是 diff baseline）；② 落点在捕获区域外不画；
  ③ 一次点击只标一帧（去重按**屏幕坐标**判）；④ `ResetAiActionClickMarks()` 放在
  `RunAiActionExecuteForAction` 入口。⚠ 提示词注入是三项合取
  （`frameHasClickMark && !curB64.empty() && AiFrameClickMarkDrawnInFrame()`）——
  **图上没有的叉绝不能说有**。自检 `frame_click_mark_lifecycle`；引擎侧接线无自检覆盖。

- **游戏效率三条**（用户实测「每步都要确认、中间隔好几秒、不懂机制」后落的）：
  ① **两步操作一次做完**：`locateAndClick(targets=["A","B"])`（2~6 个不同目标）→ 宿主
  `onLocateMulti` 逐个识图定位并**立即连点**，中间不插观察/验收，只做一次 settle；失败即停。
  ② **游戏前台不等界面稳定**：settle 用 80/350/150/900ms 短节拍，且不提「建议刷新/重开」。
  ③ **先懂机制再动手**：`skills/agent/game.md` §0.5 + `MacroActionGameSkill()` 要求抽象目标
  先用一轮确认胜负条件 / **冷却是全局还是每单位独立** / 操作机制 / 节奏，写进 memo 再动手
  （PvZ「我是僵尸」冷却按卡牌各自算 → 应批量放僵尸）。自检 `locate_multi_targets`。

- **流式读取两条硬规则**（`agent_core.cpp`；用户实测「每轮都白付一次完整请求」）：
  ① **不许用短超时轮询读 body**（250ms/1500ms 都试过，thinking 模型「看图+想」的间隙有几秒，
  WinHTTP 的 RECEIVE_TIMEOUT 一触发句柄即废 → 后续报 12019）。现在是
  **读给足超时 + 看门狗线程**：心跳/收束/组装超时/空流/整体超时/空闲超时全在看门狗里，
  要打断阻塞读就 `Abort()`（与停止热键同一条路径）；看门狗**只能读 `std::atomic` 投影**，
  不许碰读线程独占的 `StreamAccumState`（数据竞争）。② 读循环出错时**先判断「已攒到可用
  内容」**（`ShouldFinalizeStream`）再决定 fail —— SSE 正常收尾时 `QueryDataAvailable`
  也会报 12019，那不是故障。`Abort()` 关过句柄后 `AbortAwareRequestGuard` 不再重复关。

- **模型能力判定**（`agent_ai_actions.cpp`；用户报障「我选 deepseek-v4.1-flash 却跑成豆包」）：
  `ModelSupportsVision` 里 **DeepSeek v4/v5 与 vl/vision 都是多模态**，只有
  `chat`/`r1`/`reasoner`/`v3.x` 是纯文本 —— 旧规则「带 deepseek 且无 vl/vision 一律纯文本」
  会把用户选的 v4.1-flash 判成不能识图，然后**静默**换成列表里的识图模型并存进脚本。
  另外：**AI 动作执行不许在写库时改写动作模型**（`EnsureAiModelOnAction` /
  `ApplyResolvedAiModelToActionParams` 对 `aiActionExecute` 直接返回），识图由运行时
  `RunAiActionExecuteForAction` 按能力决定并打诊断；编辑器模型下拉默认选**设置里的当前模型**。
  自检：`model_supports_vision` / `action_model_not_silently_swapped` / `planner_observe_image_attach`。

- **规范化脚本**：生成/修改脚本必须走 `planScriptActions`（含循环/条件时）→ `buildScriptActions` + `createMacroScript`；
  `loop`/`if`/`else`/`defineBlock` 用 `children` 嵌套子动作（像写代码），禁止把循环体写成同级；
  `writeScript` 只接受规范化产物（逐动作重建校验 + 关键字段完整性校验，残缺 JSON 直接拒）。
- **脚本理解（两级泛化翻译，防长输入卡死）**：`readScript` 默认输出**动作概览**（动作名 +
  类型级语义 + **完整备注**，不含坐标/时长/阈值），让 AI 先做模糊理解；要具体参数传
  `detail=true`（`DescribeScriptActionsDetail`），要原文传 `raw=true`（单次 ≤32KB）。
  均按 `startIndex`/`maxActions` 分页（每页 ≤60 步）。脚本图片输出 `[[AGENT_IMG:路径]]`：
  `AgentCore` 对多模态模型自动编码为图片消息（保留窗口看 `ChatMessage::image_keep_rounds`：
  0＝只在它还是最后一条 user 消息时留、N>0＝再留 N 轮、-1＝永不剥），非多模态降级为路径提示。
  工具结果预算见 `AiToolResultBudgetChars`（`readScript` 16000 / `readAgentFile` 48000，
  勿盲目调大——长输入是卡死主因）。
- **模型自适应**：请求层按 `ModelIsReasoningType` 识别推理模型（o1/o3/o4、DeepSeek R1/Reasoner、
  Kimi K2 Thinking、Grok Reasoning），不发送 `temperature`；`max_tokens` 按模型 clamp；
  上下文滑动窗口保留最近 10 个 user 轮。`thinking.type=disabled` 由 `ShouldDisableThinking`
  **两条判据**决定（抑制 N 轮 / `QST_FAST_THINKING=1`，见「思考降档两条」）——
  **不是**按网关；旧函数 `ThinkingDisabledByGateway`（含「deepseek 域名一律关」）如今只在
  `QST_FAST_THINKING=1` 下可达。⚠ 字段**发出去 ≠ 网关认**，所以 `请求体拆解` 行会写明发没发。
