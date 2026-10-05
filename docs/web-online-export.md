# 网页在线导出独立 EXE —— 可行性评估与落地记录

> 2026-09-28。回答的问题是：「网站 Demo 能不能让用户在线写脚本、直接下载一个能跑的 exe？」
> 结论：**能，而且已经落地**。但可行的那条路与"在服务器上编译 exe"完全不是一回事。
> 本文记下判据、实测数据、踩到的坑，以及**明确不做**的部分。

---

## 0. 一句话结论

**把"组装"放到浏览器里，服务器只发一个 5.5 MB 的静态播放器模板。**

导出产物 = `[QstPlayer.exe 模板][脚本包 zip][32 字节尾标]` ——
这与产品自己的 `QuickScriptTool.exe --export-exe` 是**同一套格式**
（`src/script_package.cpp` 的 `AppendPayloadToExe` 定义），
区别只在"谁来拼"：产品在本地拼，网页在浏览器里拼。

于是：

| 资源 | 服务端成本 |
|---|---|
| CPU | **0**（nginx 发静态文件） |
| 内存 | **0**（无每请求缓冲） |
| 磁盘 | 模板 5.5 MB，**一次性**；用户脚本/图片**一个字节都不落盘** |
| 临时文件 | **0** |
| 用户数据 | **0**（不上传，也不经过服务器） |

---

## 1. 需求拆解与逐条判定

| 需求 | 判定 | 依据 |
|---|---|---|
| 在线写脚本 | ✅ 已做 | `website/export/export.html` 工坊页；Demo 的宏编辑器也能直接用 |
| 导出可双击运行的独立 EXE | ✅ 已做 | 浏览器端拼装，实测跑通（见 §6） |
| 只保留主要动作（按键点击 / 按键按下 / 鼠标点击…） | ✅ 已做 | 白名单见 §4；不支持的**逐条报原因**，不静默丢 |
| AI 相关动作不要 | ✅ 不收 | 需要 API Key，且端口在客户端；网页版一律拒绝并说明 |
| 找图动作（看服务器吃不吃得消上传图片） | ✅ 支持，**服务器不受影响** | 图片进 payload，在浏览器里打包；见 §5 |
| 优先鼠标宏 | ✅ 已做 | 预设第一个就是"每隔 N 秒鼠标左键点一下屏幕某个坐标" |
| **键鼠录制用户网页上的操作** | ❌ **不做** | 原理上不成立，见 §7 |
| 动作数限制 128 以内 | ✅ 已做 | `MAX_ACTIONS = 128`，超了直接拒绝并提示 |
| 脚本数据缓存在用户这边、导出时才处理 | ✅ 已做 | 数据从头到尾只在浏览器内存里 |

---

## 2. 为什么不能在服务器上"生成 exe"

三种"生成 exe"的路线，只有一条成立：

| 路线 | 判定 | 原因 |
|---|---|---|
| 服务端装 MSVC / clang 真编译 | ❌ | 4C/4G 上跑编译 farm 不成立；工具链版本与路径不可控；产品的 exe 本身就是这么造的 —— **导出**要是也这么干，等于把构建系统搬上公网 |
| 浏览器里真编译出原生 PE | ❌ | 没有可用工具链。TCC→WASM 只有一个研究性 demo（[lupyuen/tcc-riscv32-wasm](https://github.com/lupyuen/tcc-riscv32-wasm)），只产出 **RISC-V ELF**、POSIX 函数大量是 `@panic("TODO")`，**没有 PE 后端**；[binji/wasm-clang](https://github.com/binji/wasm-clang) 停更于 2023-12-07，且要产 PE 还得带 Windows SDK 导入库 |
| **模板 + 尾部追加** | ✅ **唯一现实解** | PE 规范明确写了 attribute certificate / debug 目录"**must be placed at the very end of an image file… because the loader does not map these into memory"**（[MS PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)）⇒ overlay 区天然可以塞数据。Ahk2Exe / Aut2Exe / Node.js SEA / 7-Zip SFX 全是这个模式 |

Node.js 官方 SEA 文档把同一件事写得很清楚：*"During start up, the program checks if anything has been injected. If the blob is found, it executes the script in the blob."*
（<https://nodejs.org/api/single-executable-applications.html>）

最接近的**商业**先例是 [RTILA X 的 Standalone Bot Export](https://rtila.com/blog/standalone-bot-export-compile-and-sell/)：
它把一个 Deno 运行时 + 浏览器引擎整包塞进产物，**在桌面编辑器里**导出，**没有**把编译搬到网页端。
⇒ "纯网页里生成 exe"没有商业先例可抄，我们这条路是自己定的，但依据是硬的。

---

## 3. 服务器容量：用户给的那台 4C / 4GB / 40GB

宝塔官方运维在自家论坛给的下限是 **2H2G + 40G 磁盘**（<https://www.bt.cn/bbs/archiver/tid-140972.html>），
而这台机器还同时跑着营销站、nginx，以及两个 200 MB 级安装包的下载分发。

**如果按"服务端组装"做，账是这样的**：

- 一个自带 OpenCV 的产物 ≈ **66 MB**（`opencv_world4100.dll` 64.6 MB，本仓实测）。
  单次请求 = 读 66 MB + 拼 payload + 写回 ≈ **132 MB 磁盘 IO + 66 MB 常驻缓冲**。
- 4 GB 内存下 **20~30 个并发就把内存打满**（还没算 nginx/PHP/MySQL）。
- `/tmp` 通常与根分区同盘：20 并发 × 2 份 66 MB ≈ **2.6 GB**，失败请求的残留会累积到 40 GB 盘满。
- 签名更别想：CA/B Forum [Code Signing BRs v3.11.0](https://cabforum.org/working-groups/code-signing/requirements/)
  自 **2023-06-01** 起要求订户私钥生成/存储/使用都在**硬件密码模块**内 ⇒ 私钥不能以文件形式放在这台 VPS 上。
  真要做签名只能走云签名服务，那就是每请求一次网络往返 —— 又是一个"别放进请求路径"的理由。

**按现在的做法**：并发 100 个用户同时导出，服务器只是在 `sendfile` 一个 5.5 MB 的静态文件 ——
这正是 nginx 最擅长、也最便宜的事。**这正是"适应性调整"的落点。**

---

## 4. 动作白名单（"仅保留主要功能"落到代码里）

`website/export/qst-web-export.js` 的 `ACTIONS` 表：

| 分组 | 动作 | 能重复（`clickCount` + `duration`） |
|---|---|---|
| 鼠标 | `mouseClick` / `moveMouse` / `mouseDown` / `mouseUp` / `scrollWheel` | 点击与滚轮可 |
| 键盘 | `keyClick` / `keyDown` / `keyUp` / `quickInput` | 按键点击可 |
| 流程 | `wait` / `stopMacro` | — |
| 识别 | `findImage`（找图；命中后点击或移动） | 用 `loop` + `wait` 表达 |

Demo 的"导出当前编辑的脚本"那条路（`buildExeFromEditor`）额外接受 `loop` / `endLoop` /
`if` / `else` / `hotkeyShortcut` —— 因为编辑器里本来就能写这些结构。

**明确拒绝**（会逐条列出类型 + 原因，**不静默丢弃**）：
AI 三个动作（要 API Key）、OCR、找色、拖拽、嵌套宏 / 自定义块、录制回放、
启动程序 / 打开文件 / 打开网页 / 关闭程序 / 激活窗口、变量计算 / 跳转、截图锁定、后台窗口模式。

> 拒绝而不是忽略，是因为"我明明写了这个动作、导出来却没有"是最难查的一类问题。

**行动作数上限 128**：用户明确要求的产品约束，同时也是"网页版 ≠ 完整版"的天然边界。
扫描结果里直接显示 `动作 N / 128`。

---

## 5. 找图与"用户上传图片"：服务器到底吃不吃得消

**吃得消，而且是零成本** —— 因为图片根本不经过服务器：

1. 用户在工坊页选图 → `File.arrayBuffer()` 读进浏览器内存；
2. 打包时作为 **stored（不压缩）条目**写进 payload；
3. 播放器解包后由 `MoveImagesIntoPlace()` 挪进 `scripts\images\`，
   `ResolveImagePath()` 按**裸文件名**在 `FindImagesDir()` 里找到它（`src/utils.cpp:1116`）。

⇔ **脚本与图片都是"从浏览器直接进 exe"，服务器只发了那个 5.5 MB 的模板。**

⚠ 运行时还需要**图像识别组件**（OpenCV）。两种模式：

| 模式 | 体积 | 前提 |
|---|---|---|
| **走软件**（网页版默认） | 不加 | 目标电脑装了「键鼠工坊」⇒ 复用它的 `opencv_world4100.dll` |
| 自带 | **+64.6 MB** | 把 DLL 也塞进 payload ⇒ 目标机零依赖 |

网页版默认走**前者**：多下 64 MB 的带宽是用户的、不是服务器的，但对"只是想写个连点宏"的用户来说
没必要；而且 64 MB 的 exe 过浏览器下载本身就是负担。
`playerruntime::ResolveRuntime()` 在"走软件"拿不到组件时**不硬失败**，只记 warning ⇒ 那一类动作跳过，不会崩。

> 如果将来要做"自带"，只需把 `opencv_world4100.dll` 作为静态资源放上去、在 payload 里加一条 `rt/` 条目。
> 代码路径已经预留（`buildManifest` 的 `bundledOpenCv`、`buildPayload` 的 `runtimeFiles`）。

**不引 OpenCV.js**：`@techstark/opencv-js` 的 `dist/opencv.js` 是 **13,298,869 B（≈12.7 MiB）**
（<https://data.jsdelivr.com/v1/packages/npm/@techstark/opencv-js@5.0.0-release.1>，许可证 Apache-2.0）。
在**已经要下载 5.5 MB 播放器**的前提下再让浏览器多下 12.7 MB 去做一件"播放器那边本来就会做"的事，不划算。

---

## 6. 实测证据（不是"看着对"）

### 6.1 离线：`node tools/verify/web_export_roundtrip.js` → **47 passed / 0 failed**

其中与 C++ 参考实现**定值对齐**的两条最关键：

- `fnv1a64`：拿**真实导出产物**的 payload 对哈希，必须等于产物尾标里的
  `1850181869054415507`。⚠ 本仓 FNV-1a 的 offset basis 是
  **`1469598103934665603`**（`src/script_package.cpp:471`），
  **不是**标准 FNV 的 `14695981039346656037`（少一位）。写错不致命但极难察觉 —— 运行时目录名会变。
- `crc32`：定值向量 `"123456789"` → `0xCBF43926`。

### 6.2 真机：网页生成的 exe 真的驱动了键鼠

用探针脚本（都经由 `buildExe` 真实代码路径产出）实测：

| 探针 | 观察 | 结果 |
|---|---|---|
| `moveMouse` → 25%/25% | 从 PowerShell 读 `GetCursorPos` | **(427,240)** = `(0.25×1707, 0.25×960)` —— **完全吻合** |
| `loop(-1){moveMouse 75%; wait 1}` | 每轮前把光标挪走，看它是否回来 | **8/8 次回到 (1280,720)** = `(0.75×1707, 0.75×960)` ⇒ 循环真的在重复执行 |
| `keyClick(F24) ×3，间隔 1s` | 进程总时长 | **3.68 s**（≈1.2 s 迁移 + 2.0 s 两次间隔）⇒ `clickCount + duration` 的"重复间隔"语义正确，且**首前/末后不插等待** |
| 全流程 | `player.log` | `脚本已开始运行` → `脚本已结束（跑完或已停止），播放器退出` —— **双击就跑、跑完自退** |

### 6.3 真实浏览器：`website/export/selftest.html` → **8 passed / 0 failed**

headless Edge（`--dump-dom`）实测：CRC 定值、FNV 定值、stored ZIP 自洽读回、
**模板可下载且是有效 PE（5.50 MB）**、工坊路径产物尾标自洽、Demo 路径产物尾标自洽、
设置快照正确。

### 6.4 页面无白屏

Demo 主界面在 headless Edge 下渲染出 **239 KB DOM**，标题 / 体验版标识 / 四个主 Tab 全部在位。

---

## 7. 键鼠录制：为什么网页做不到（明确不做）

**结论：纯网页拿不到全局鼠标键盘。这不是权限问题，是捕获管道的问题。**

- `getDisplayMedia()` 给的是**像素流**，页面**收不到任何鼠标/键盘事件**
  （[MDN](https://developer.mozilla.org/en-US/docs/Web/API/MediaDevices/getDisplayMedia)、
  [Using Screen Capture](https://developer.mozilla.org/en-US/docs/Web/API/Screen_Capture_API/Using_Screen_Capture)）。
  共享屏幕上"用户点了什么"对页面**不可观测**。
- 而且它**必然弹选择器**、源列表**不可枚举**（规范明写 *"screen sharing sources are not enumerable"*）。
- WebHID / WebUSB 标着 *Limited availability + Experimental*，只能与用户逐个授权的**那一个设备**通信，
  既读不到全局输入流，也不能注入。
- Pointer Lock / Keyboard Lock / Fullscreen **都不是全局钩子**（只作用于页面自己）。

⇒ **录制必须留在原生侧**（本仓 `extension/edge/` + native messaging host、或产品自己的壳）。
网页适合的是"编辑 + 组装 + 下载"，不是"采集"。

> 顺带一条对**产品**有用的发现：WebView2 有
> [`ScreenCaptureStarting`](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/overview-features-apis)
> 事件，宿主可以*"block the UI from being displayed, or allow the UI to be displayed"* ⇒
> **自家壳里可以零摩擦抓桌面画面**。但即使这样也**拿不到输入** —— 输入只能走原生钩子。

---

## 8. 落地时踩到并修掉的坑（都做了 A/B 验证）

### 8.1 ★★ 不带设置快照 ⇒ 导出的 exe **永远不退出**

`PlaybackTabSettings::enablePlaybackCount` 的默认值是 **`false` = 无限循环**
（`src/app_settings.h:48`；引擎在 `engine_script_run.cpp:8000` 按它决定要不要 break），
而播放器读的是 `AppDir()\app_settings.json` —— **包里不带，它就用这个默认值**。

后果：**任何"线性、跑完就该退出"的脚本都会一直循环下去**。
我第一个探针就是这样：网页说导出成功，双击以后每 2 秒按一次 F24，一直按到手动结束 ——
**而这在浏览器里完全看不出来**（产物能跑、字节也对）。

产品的导出会 `SerializeAppSettings()` 打整份快照，所以它没这个问题；网页版没有"用户设置"可快照，
于是在 `buildSettingsJson()` 里**显式钉死** `enablePlaybackCount: true, playbackCount: 1`
（只覆盖这两个键是安全的：`LoadAppSettings` 先取 `DefaultAppSettings()` 再按字段覆盖，
`app_settings_store.cpp:677-701`）。

> 教训：**"产物能跑"不等于"产物行为对"**。涉及默认值的开关，必须像坐标一样被显式写进产物。

### 8.2 ★★ `ExtractZipFile` 会认错 EOCD（产品既有缺陷，python 一次就中）

`FindEocdInBuffer` 原来从 `searchStart` **向前**扫描、撞见第一个 `50 4B 05 06` 就返回。
而这个签名只有 4 字节，**完全可能自然出现在条目数据里** ——
网页导出会把**用户上传的模板图**原样放进 payload，这条路径第一次被真实暴露。
命中假签名 ⇒ `centralDirOffset`/`totalEntries` 全是垃圾 ⇒ **解包 0 个条目** ⇒
用户看到的是「脚本数据解包失败（**可能被杀毒软件拦截**）」—— 真因与杀软毫无关系，
这条文案会把排查方向整个带偏。

修法：优先采信**自洽**的 EOCD（`centralDirOffset + centralDirSize == EOCD 位置`
且算上 comment 正好收尾），一个都不自洽时才回落到旧的"第一个"。
新用例 `zip_entry_data_forging_eocd`；**A/B 验证**：关掉修复 ⇒ `extracted=0` 转红。

### 8.3 ★★ 中文条目名会变乱码（产品既有缺陷）

条目名在 zip 里是 **UTF-8 字节**（`CreateZipFile` 走 `ArchiveNameUtf8`），
而读取侧写的是 `std::wstring(seg.begin(), seg.end())` —— **逐字节加宽**。
中文名于是变成一串乱码宽字符，`CreateFileW` 生成一个**名字不对**的文件，
而按真名去找的一方（`ResolveLibraryScriptPath` / `ResolveImagePath` / `FindImagesDir`）
**永远找不到它**。纯英文名一直是好的，所以这个缺陷长期没暴露
—— 而产品的清单测试里就写着 `scripts\子脚本.json`。

修法：`FromUtf8(seg)`。新用例 `zip_non_ascii_entry_name`；
**A/B 验证**：关掉修复 ⇒ `extracted=1 fileExists=0`（解出来了，但名字不对）转红。

### 8.4 模板路径不能相对"当前页面"

第一版写死 `export/player/QstPlayer.exe`。工坊页在 `/export/export.html`，
于是解析成 `/export/export/player/...` ⇒ **404**；而官网首页下的 Demo 又是另一个基准。
浏览器自检页第一次运行就把它抓出来了。现在改为**相对脚本自身 URL**（`document.currentScript.src`），
站点部署在任意子目录都成立。

### 8.5 `count: 0`（一直重复）被展开成"只执行一次"

工坊的重复模型里 `count === 0` 表示"一直重复"。第一版的分支顺序是
`if (count <= 1 || canRepeatInline)` ⇒ `0 <= 1` 成立 ⇒ 直接输出裸动作 ⇒
「一直重复的鼠标点击」变成"点一次就结束"。离线用例 `count=0 展开成 loop 容器` 抓住并钉住了它。

### 8.6 `tools/package_release.ps1` 的 BOM 被编辑工具吃掉了

按本仓规矩「含中文的 `.ps1` 必须 UTF-8 **带 BOM**」（本机 ANSI 是 GB2312）。
改文件时 BOM 被剥掉 ⇒ PowerShell 按 GBK 解析 ⇒ 解析器报 7 处语法错。
已补回，并顺手审计了 `tools/ driver/ scripts/ website/ installer/` 下全部 `.ps1`（只这一个中招）。

### 8.7 ★ 「我在本地打开不能导出 exe」—— 双击 html 是 `file://`

用户实测：本地直接双击 `demo/index.html`，点导出只得到一句 **`Failed to fetch`**。

根因：`file://` 协议下浏览器**禁止 `fetch` 读本地文件**（XHR 同样被拦，没有绕法）。
所以问题不是"必须装到服务器上"，而是"必须用 http 打开"。本机没装 Python，
于是新增 `tools/serve_website.ps1`（.NET `HttpListener`，零依赖、不需要管理员、
只监听 127.0.0.1、只读文件）作为本地预览入口。

同时改掉"干巴巴一句 Failed to fetch"：

- `describePlayerError()` 把三种成因**分开说**：
  ① `file://` ⇒ 告诉用户去跑 `tools\serve_website.ps1` 并用 http 访问；
  ② HTTP 404 ⇒ 明确说"模板没上传，它被 gitignore 忽略、不跟着代码走"；
  ③ 网络层失败 / 拿到的不是 PE ⇒ 各自的处置。
- 工坊页**一进来**就检测 `file://` 并直接把提示写在扫描区（不等用户点导出）；
  Demo 的导出分支同样提前拦。
- 判据是 `location.protocol === "file:"` —— 不看 UA、不看端口，没有猜测成分。

> 教训：**"操作失败"和"环境不对"是两类信息**。把后者翻译成前者（或反过来）
> 会让用户朝错的方向修 —— 与 §8.2 那条"报成杀软拦截"是同一个毛病。

### 8.8 ★ Demo 的脚本显示名多了一个 `.json`

用户实测：网页版编辑器「宏名称」显示 `表格录入.json`，而软件里不是这样。

产品口径（`webview_bridge_backend.cpp`）：
- 列表入口 `name` = `data.scriptName`（存盘时已 `StripJsonExtension`，`:1589`），
  兜底用 `std::filesystem::path(path).stem()`（`:3796-3797`）—— **两者都不带扩展名**；
- 重命名 `RenameScriptFile` 收的是**裸名**，`name` 存裸名、`path` 才补 `.json`（`:1776-1786`）。

Demo 的桩给 `name` 拼了 `.json`（示例数据 4 条 + `upsertMacro` + `renameScript` 三处），
于是比软件多一个后缀。已按产品口径统一，并用 `listScripts` / `openEditor` / `renameScript`
三个真实桥接调用探针验证（`name=我的宏`、`path=我的宏.json`）。

### 8.9 Demo 示例数据太多太杂

用户原话：「例子太多太杂了，用户根本没有看下去的欲望」。
从 **4 条宏 + 2 条录制 + 2 条对话** 砍到 **1 + 1 + 1**，示例动作从 13 条重写成 11 条
一条能读完的脚本：基本动作 + 循环（带循环变量 `i`）+ 如果 / 否则 + 结束宏运行。
`i` 与条件 `{i} >= 2` 是成对的 —— 顺手把"循环变量能拿到条件里用"演示清楚，
而不是随手写一个不存在的变量（旧版就是 `{done} == 1`，而 `done` 从没被赋值过）。

### 8.10 ★★ 极简 / 专业模式的脚本库对不上（桩的目录树写死了）

用户原话：「网页版的极简模式和专业模式的脚本库没有统一」。

根因在**桩**，不在前端：`libraryFoldersFor(kind)` 返回写死的
`["示例宏","游戏辅助","办公自动化"]`，而 `macros` 里只有一条**无目录**的脚本。
于是同一次会话里：

- 极简模式读 `macros` ⇒ 列出 1 条「示例宏」；
- 专业模式读 `folders` 建树 ⇒ 显示 3 个**空目录**，脚本一条都不在里面。

**同一份数据、两种视图、互相矛盾**。修法只有一个方向：目录列表必须从条目数据
**算出来**（`folder` 字段 ∪ 用户新建的目录），不能另抄一份。
这正是 AGENTS.md §43「静态清单 × 动态数据必须同一份事实」。

顺带发现同一类问题还有 9 个 —— 用 `tools/verify/demo_bridge_coverage.js` 比对
`ui/bridge.js` 的请求类型与桩的 `case` 得到：

| 缺失的桥接方法 | 用户看到的现象 |
|---|---|
| `previewScriptActions` | 专业模式右侧「动作预览」**永远空着** |
| `windowAgentList` / `windowAgentBind` | 设置页「窗口 Agents」整块失效 |
| `queryVhidStatus` | 驱动安装区读不到状态 |
| `setRecorderMode` | 录制模式切换无效 |
| `window.setHomeSize` | 模式切换后窗口尺寸对不上 |
| `pickScreenDrag` / `pickTemplateDrag` | 要原生截图，浏览器做不到 |
| `systemReboot` | 要重启电脑 —— **绝不能"演示"** |

**桩没实现的方法不会报错**：调用发出去、没有 `.result` 回来，界面就静默少一块。
这类缺口在浏览器里完全看不出来，只能靠比对 API 面。

已修：9 个全部补上（前 6 个**真做**，后 3 个给出明确说明）；
目录管理（新建 / 改名 / 删除 / 移动）也从 `fail(DEMO_HINT)` 改成**内存态真做**。
守卫脚本进了仓库，改 `ui/` 或桩之后跑一次即可。

### 8.11 ★ 导出的 exe 没有继承「设置」里的设置项

用户原话：「网页版导出的exe没有像客户端那样继承设置里面的设置项，要跟设置里面保证一致」。

客户端导出会把 `SerializeAppSettings(settings, true)` 的**整份** `app_settings.json`
打进 `rt\`（播放器按 `AppDir()\app_settings.json` 读）⇒「导出时软件里是什么设置，
exe 里就是什么设置」。网页版此前只带了 `playback` 里的两个键，于是"设置里选了什么"
和"导出的 exe 怎么跑"是两回事。

修法：Demo 路径把**用户当前那一份设置对象**原样 `JSON.stringify` 进包。
安全性来自 `LoadAppSettings` 的形状（`app_settings_store.cpp:677-701`）：
先取 `DefaultAppSettings()` 再**逐字段覆盖** ⇒ 多带的键被忽略、少带的键保持默认，
两个方向都不会把别的设置清零。

**真机验证**（不是"字节里多了一个文件"）：同一个单动作脚本（`wait 0.6`），
用 `playbackCount=1` 与 `=3` 分别导出并真跑：

| 设置 | 子进程存活 |
|---|---|
| `playbackCount: 1` | 1240 ms |
| `playbackCount: 3` | 2302 ms |

差 **1062 ms** ≈ 多跑的 2 遍 × 600 ms ⇒ 设置确实驱动了运行行为。

> ⚠ 这条与 §8.1 是同一枚硬币的两面：**设置快照不是可选项**。
> 有真设置就原样带（Demo）；没有设置 UI 的工坊页则显式钉住"播放次数"，
> 否则 `enablePlaybackCount` 取默认 `false` = 无限循环。

### 8.12 ★ 导出"不如客户端迅速" —— 加进度条 + 可中止 + 修缓存策略

用户原话：「网页版导出时组装不如客户端那样迅速，加个进度条UI，能实时展示效果，
用户也能通过UI界面提前中止」。

先量清楚**慢在哪**：组装（ZIP + 追加）是**毫秒级**，5.5 MB 的模板下载才是全部观感来源。
所以：

- `fetchPlayer` 从 `res.arrayBuffer()` 改成 **`res.body.getReader()` 流式读** ——
  `arrayBuffer()` 期间拿不到任何进度，用户只看到一个卡住的按钮；
- 进度事件分阶段（`player` 下载 0~90% → `pack` → `assemble` → `done`），
  下载阶段显示 `已下载 / 总量` 的实际字节；
- `AbortController` 贯穿 `fetch` + 各阶段边界，UI 上「取消导出」/「取消」随时可中止；
  取消**不是错误**（`isAbort()` 统一判据，UI 用中性语气报"已取消"，不弹红色失败）。

⚠ **顺带修掉一个真 bug**：原来用 `fetch(url, { cache: "force-cache" })`。
`force-cache` 会无视 HTTP 新鲜度直接用本地副本 ⇒ **发版换了 `QstPlayer.exe` 之后，
老用户仍然拿旧模板导出** —— 产物照样能跑，只是跑的是旧引擎，完全看不出来
（与 §8.1、§8.11 同一类"静默"事故）。改成 `default`，让 nginx 的 ETag/Last-Modified
正常生效；页面内再缓存一份，保证"一次页面加载只下一次"。

> 测试环境提醒：本机 localhost 下 5.5 MB 是毫秒级，进度条一帧跳到底 ——
> 用 CDP 的 `Network.emulateNetworkConditions` 限速到 2.5 MB/s 才能测出中间态。
> **"测不出来"和"没有实现"是两件事**，别把前者当后者（也别反过来）。

### 8.13 ★★ 「导出还是没走设置」—— 扁平键全被桩丢掉了（连带挖出 2 个产品缺陷）

用户第二次反馈：「网页 demo，导出还是没走设置，要跟软件导出的 exe 一致」。

§8.11 修的是"设置快照没进包"，但**更里面一层**才是真因：
产品前端的设置往返是**不对称**的 ——

| 方向 | 形状 | 出处 |
|---|---|---|
| 前端 → 后端 | **扁平**：`{enablePlaybackCount, playbackCount, lowPerformanceMode, clickCountLimit, …}` | `ui/app.js` 的 `collectSettings()` |
| 后端 → 前端 | **嵌套**：`{playback:{…}, click:{…}, other:{…}}` | `fillSettings()` 读 `s.playback.playbackCount` |

也就是后端契约是「**收扁平、回嵌套**」（`webview_bridge_backend.cpp` 的 SaveSettings
里有 93 条 `GetX(json,"key") → s.section.field`）。

而桩早先只认嵌套 + 一份**手写的 24 项 `otherFlat` 白名单** ⇒ `playbackCount`、
`enablePlaybackCount`、`lowPerformanceMode`、`clickCountLimit` 这些扁平键
**全被静默丢掉**：界面上改了、`settings` 对象没变、导出的 exe 用旧值。
浏览器里一个字都看不出来。

**修法**：把路由表**从 C++ 抽出来**（93 条），桩照表路由；并加守卫
`tools/verify/demo_settings_parity.js` 做**双向**校验 —— C++ 加了新设置而桩没跟，直接报红。
不再手写白名单（这个教训一天内吃了两次）。

**守卫顺带揪出 2 个真正的产品缺陷**（不是 Demo 的，是客户端的）：
`collectSettings()` 一直在发这两个扁平键，但 `SaveSettings` **从来没映射过** ⇒
勾选保存后静默失效：

| 键 | 界面入口 | 症状 |
|---|---|---|
| `exportScriptAsZip` | 设置 → 其他 → 「导出脚本默认为 zip 格式」 | 勾了保存，下次打开又变回去 |
| `enableWindowTimeScale` | 设置 → 窗口模式 → 「窗口时间缩放」 | 同上；**更隐蔽**：`fillSettings` 读到 `null` 时**默认显示为勾选**（`app.js:3269`），界面看起来"一直是开的"，用户根本不会怀疑是保存坏了 |

已在 `webview_bridge_backend.cpp` 的 SaveSettings 里补上映射。

### 8.14 ★ 默认改成「回放次数限制：勾选 + 1 次」

用户要求：「软件和网页 demo 的设置要改成默认勾选『回放次数为 1』」。

改的是 `src/app_settings.h` 的 `PlaybackTabSettings::enablePlaybackCount`
（`false` → `true`，`playbackCount` 本来就是 1），Demo 桩的两处默认设置同步。

为什么这个默认值值得改：关掉 = **无限循环**（`engine_script_run.cpp:8000` 按它 break），
而"线性脚本跑完就该退出"是最自然的预期 —— 关着的时候一条 `mouseClick` 会一直点下去，
只能靠热键停。更要命的是它**同时决定导出的 exe 行为**：播放器读
`AppDir()\app_settings.json`，包里不带该键就用这个默认值。
默认 1 次让"双击就跑、跑完自退"成立；挂机循环改为**显式**把次数调大或关掉勾选
（脚本里也能用 `loop(-1)` 表达）。

⚠ 只影响**没有该键**的旧配置（`LoadAppSettings` 先取默认再逐字段覆盖），
已存在的 `app_settings.json` 保持用户自己的值。

### 8.15 验证方式

| 层 | 命令 | 结果 |
|---|---|---|
| 离线逻辑 | `node tools/verify/web_export_roundtrip.js` | **54 passed / 0 failed** |
| 桥接覆盖 | `node tools/verify/demo_bridge_coverage.js` | 缺口 **0**（前端 99 个调用全有处理） |
| 设置字段对齐 | `node tools/verify/demo_settings_parity.js` | 93 条 C++ 映射逐条一致 / 73 个前端扁平键全部有归宿 |
| 真机运行 | `.tmp/webexport-probe/` 的探针 exe | 光标落点 / 循环 / 按键时序 / 设置继承全部吻合 |
| 真浏览器 | Edge + CDP（限速 2.5 MB/s 驱动真实 Demo 页） | **导出 12/12 + 设置 12/12** 全通过 |

设置用例里最关键的三条是「用**扁平**键保存 → 读回是**嵌套**且值对得上 →
**导出时交给打包器的那份设置就是它**」—— 一条链从界面一直验到 exe 的字节。

---

## 9. 与产品侧的一致性（这是能长期维护的关键）

- **格式同源**：payload 尾标、清单 `package.json`、脚本 `script.json` 全部对齐 C++ 端字段名与语义；
  网页导出的 exe 能被本软件**导入**（`src/webview/webview_bridge_backend.cpp:1884` 的
  `DumpPayloadToZip` 路径），与产品 export 的产物等价。
- **产物同源**：模板就是 `build\Release\tools\player\QstPlayer.exe`（与产品分发的是同一个文件）。
  `tools\package_release.ps1` 发版时会把它同步到 `website\export\player\`，
  否则网页版会跑一个**旧引擎** —— 而这也是"看不出来"的一类问题。
- **一份实现两个入口**：工坊页与 Demo 的导出对话框共用 `website/export/qst-web-export.js`。

---

## 10. 没做 / 下一步

| 项 | 说明 |
|---|---|
| 找图"自带 OpenCV"模式 | 代码路径已留（`bundledOpenCv` / `runtimeFiles`），只差把 64.6 MB 的 DLL 放上静态站 |
| 断点续传 / 大产物 | 当前产物 5.5 MB 级，流式进度够用；真要 66 MB 再考虑 `showSaveFilePicker` 流式写 |
| 代码签名 | 未签名 ⇒ SmartScreen 会提示"更多信息 → 仍要运行"。真要签：**先追加 payload 再签**（顺序反了签名必失效），且私钥必须放硬件模块 / 云签名服务 |
| 服务端零参与这条约束的守卫 | 目前靠本文与 README；以后若有人想加"帮我编译"接口，先回来读 §2 与 §3 |
| 录制 | 明确不做（§7）；要做就在原生侧做 |
| 网页版脚本 JSON 的 `hotkeyVk` 配置 | 工坊页暂未暴露"给 exe 配一个控制热键"，当前统一 F9（单击=暂停/继续，长按=停止） |
| Demo 与产品前端的**逐字节**一致 | 已由 `tools/sync_website_demo.ps1` + 发版链保证（`ui/` 是唯一事实来源） |

---

## 11. 开源参考与许可证（闭源商业分发的硬约束）

| 项目 | 许可证 | 能否用 |
|---|---|---|
| AutoHotkey v2 | **GPLv2** | ❌ bundling 解释器即传染 |
| Pulover's Macro Creator | **GPLv3**（1 人例外） | ❌ |
| n8n | Sustainable Use License（非 OSI） | ❌ 明禁商业 |
| AutoIt | 专有 EULA（允许商业分发编译产物） | ⚠️ 源码不可得、禁反向工程 |
| TagUI / SikuliX / RobotJS / Selenium IDE | Apache-2.0 / MIT / MIT / Apache-2.0 | ✅ |
| OpenCV / OpenCV.js | Apache-2.0 | ✅（但 12.7 MiB 体积劝退，见 §5） |
| fflate | MIT（约 8 kB） | ✅ **当前不需要** —— payload 必须 stored，连压缩器都不用 |

**本项目一个第三方库都没引**：stored ZIP（约 60 行）、CRC32（约 10 行）、FNV-1a64（约 6 行）都是自研，
既省体积也省许可证审查。

---

## 12. 相关文件

| 文件 | 作用 |
|---|---|
| `website/export/qst-web-export.js` | ★ 全部核心逻辑（脚本 JSON / ZIP / 尾标 / 校验 / 预设） |
| `website/export/export.html` `export.js` `export.css` | 「在线脚本工坊」页面 |
| `website/export/selftest.html` | 部署后自检（打开即跑，标题给出 PASS/FAIL） |
| `website/export/player/README.md` | 模板为什么不在 git 里、怎么补、怎么验证 |
| `website/demo/bridge.stub.js` | Demo 的 `scanScriptForExport` / `exportScriptAsExe` 两个分支 |
| `tools/verify/web_export_roundtrip.js` | 离线校验（54 条断言） |
| `tools/verify/demo_bridge_coverage.js` | 桥接桩覆盖守卫（桩缺方法 = 界面静默少一块） |
| `tools/serve_website.ps1` | 本地预览服务器（零依赖；`file://` 下导出用不了的解法） |
| `tools/sync_web_export_player.ps1` | 同步模板 |
| `tools/sync_website_demo.ps1` | `ui/` → `website/demo/` 同步（含 `-Check`） |
| `src/script_package.h/.cpp` | payload 尾标格式的**定义方**（改格式两边一起改） |
| `docs/script-to-exe-design.md` | 产品侧"导出为独立 EXE"的完整设计（本文是它的网页版） |
