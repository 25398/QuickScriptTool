# 键鼠工坊（QuickScriptTool）官网

单仓静态官网：纯 HTML / CSS / JS，无 React / Vue 构建链，可直接上传宝塔站点目录运行。
包含营销首页、在线体验 Demo（复用产品前端 + 桥接桩）、下载页。

## 目录结构

```text
website/
├── index.html        # 官网首页（Hero / 在线体验 / 功能 / 架构声明 / 下载 / 页脚）
├── demo.html         # 全屏在线体验页（产品窗口 + iframe 等比缩放）
├── download.html     # 下载与系统要求
├── downloads/        # 安装包 / 便携 zip（package_release 后同步；部署时一并上传）
│   ├── QuickScriptTool-Setup.exe       # 官网固定链接（安装包最新）
│   ├── QuickScriptTool-Release.zip     # 官网固定链接（便携最新）
│   ├── QuickScriptTool-<版本>.exe       # 带版本号归档
│   ├── QuickScriptTool-Setup-<版本>.exe
│   └── QuickScriptTool-Release-<版本>.zip
├── favicon.ico       # 站点图标（复用产品图标）
├── css/site.css      # 官网样式（极光 Arctic：海军蓝 + 冰蓝 + 薄荷）
├── js/site.js        # 导航 / Demo iframe 缩放 / 页脚年份
├── demo/             # 从 ui/ 拷贝并适配后的可运行产品前端
│   ├── index.html    # 主界面（已注入 bridge.stub.js + 网页体验版标识）
│   ├── bridge.stub.js# 核心：WebView2 桥接桩（假数据 / 假状态机 / **真·导出 EXE**）
│   ├── agent-inline.js# 主壳 AI 对话改为页面内联弹窗（不开新页面）
│   ├── bridge.js     # 原产品桥接脚本（原样保留）
│   ├── app.js / pro-mode.js / shell.css / pro-mode.css / agent.css
│   ├── app_icon.ico
│   └── vendor/katex/ # 原产品依赖（数学公式渲染，未删减以保证功能一致）
└── export/           # 在线脚本工坊：浏览器里写宏 → 下载独立 EXE
    ├── export.html   # 「在线脚本工坊」页面
    ├── export.js     # 该页交互（动作列表 / 校验 / 导出）
    ├── export.css
    ├── qst-web-export.js  # ★ 核心：脚本 JSON + stored ZIP + 尾部追加（Demo 也用它）
    ├── selftest.html # 部署后打开它，确认导出链路可用（模板在位 / 能拼出 exe）
    └── player/       # QstPlayer.exe 模板（二进制不入库，见其中 README.md）
```

## 当前备案号

页脚已填 **鄂ICP备2026041557号-1**（链接到 `https://beian.miit.gov.cn/`，工信部要求）。
位置：`index.html` / `download.html` 的 `.f-bottom`；样式在 `css/site.css` 的 `.site-footer .f-icp`。
新增页面若带页脚，照抄这一段即可。

## 本地预览

⚠ **不要直接双击 `index.html`**（那是 `file://`）。在线导出要 `fetch` 播放器模板，
而浏览器**禁止 `file://` 读本地文件**，只会弹一句 `Failed to fetch`。必须走 http。

本机没装 Python，所以仓库自带一个零依赖的本地服务器（.NET `HttpListener`，不需要管理员）：

```powershell
# 在仓库根目录
powershell -ExecutionPolicy Bypass -File tools\serve_website.ps1
# → 自动打开 http://127.0.0.1:8090/export/selftest.html（导出链路自检）
#   官网首页     http://127.0.0.1:8090/index.html
#   在线体验     http://127.0.0.1:8090/demo.html
#   在线脚本工坊 http://127.0.0.1:8090/export/export.html
# 换端口：-Port 9001；不开浏览器：-NoBrowser
```

也可以用自己的静态服务器（`python -m http.server` / 宝塔 / nginx 都行），只要
**根目录指向 `website/`**、且 `export/player/QstPlayer.exe` 在位。

> 打开后重点看：Demo 是否白屏、四个主 Tab 能否切换、设置弹窗 / 宏编辑器能否打开、
> **在线导出能否真的下载出 exe**（用 `export/selftest.html` 一键判断）。

## 上传到宝塔

1. 宝塔面板 → 网站 → 找到 `https://www.quickscripttool.cloud/` 对应的站点（默认根目录形如
   `/www/wwwroot/QuickScriptTool/`）。
2. 删除站点根目录里宝塔自带的 `index.html`、`404.html` 等默认欢迎页。
3. 把本目录下所有文件整体上传到站点根目录，保持 `index.html` 在最外层：

```text
/www/wwwroot/QuickScriptTool/
├── index.html
├── demo.html
├── download.html
├── css/…
├── js/…
└── demo/…
```

4. 在宝塔「网站 → 设置 → 伪静态 / 默认文档」确认默认文档包含 `index.html`。
5. 官网页面不依赖外部字体/脚本，断网或大陆网络环境也能完整渲染。
6. **备案号**已填好（`鄂ICP备2026041557号-1`，链接 beian.miit.gov.cn），无需再改。
7. **下载文件**：官网按钮链到固定名 `downloads/QuickScriptTool-Setup.exe` 与
   `downloads/QuickScriptTool-Release.zip`（发版覆盖即可，不必改 HTML）。带版本号文件仍会同步到同目录作归档。
   由 `tools\package_release.ps1` + Inno Setup 生成。体积约数百 MB，上传时注意超时。
8. ⚠ **在线导出用的播放器模板**：`export/player/QstPlayer.exe`（约 5.5 MB）。
   它不进 git（`*.exe` 被忽略，与 `downloads/` 同一策略），**但必须上传到服务器**，
   否则网页导出会报「播放器模板不是有效的 exe」。补它与自检：

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\sync_web_export_player.ps1
   # 上传后浏览器打开 https://<域名>/export/selftest.html，标题应为 [PASS] 导出链路自检
   ```

## Demo 哪些是「演示态」

网页体验版由 `demo/bridge.stub.js` 接管所有原生桥接调用，**不会注入真实键鼠、不录制、
不找图、不 OCR、不捕获全局热键、不操作窗口**：

| 操作 | 表现 |
|------|------|
| 切换四个主 Tab | 真实前端逻辑，正常可用 |
| 设置弹窗、主题弹窗 | 正常可用；主题与正式版一致（7 经典 + 极光 Arctic 共 8 款），预设切换与自定义颜色真实生效 |
| 极简 / 专业模式 | 与产品一致，在「设置 → 其他 → 界面模式」切换保存 |
| 宏编辑器（打开 / 编辑 / 保存 / 删除 / 重命名 / 设热键） | 正常可用，数据保存在内存 |
| 开始 / 停止连点 | 模拟运行（按钮与状态会切换），并弹出「演示模式」提示 |
| 录制开始 / 停止 | 模拟录制，停止后生成一条示例录制 |
| 录制优化窗口 | 正常打开（示例录制数据），批量删除 / 等待调整 / 合并压缩 / 转找图点击等方案可浏览与模拟应用 |
| 运行宏 / 停止宏 | 模拟运行（顶部 HUD 出现），并弹出提示 |
| 找图 / OCR / 选区 / 截图 | 拦截，弹出「网页体验版仅演示界面」提示 |
| 热键捕获 | 拦截并提示（预设热键选项仍可点击体验） |
| 定时任务 | 预置示例任务，可浏览 / 新建 / 删除（内存态） |
| AI Agent 对话 | 主壳页面内联弹窗（不新开页面），假对话流：发送消息后流式返回预设回复，不调用真实模型 |
| **导出为独立 EXE** | **是真的**：在浏览器里把当前编辑的脚本拼成一个可双击运行的 exe 并下载（见下节） |
| 导入 / 安装驱动 / 检查更新 | 拦截并提示 |
| 下载客户端按钮 | 跳转 `downloads/` 下安装包 / 便携 zip |

### 示例数据的口径（改假数据前先读）

- **只有一条示例宏**（`示例宏`，11 个动作）。用户 2026-09-28 反馈："例子太多太杂，没人看得下去"
  ⇒ 从 4 条宏 + 2 条录制砍到 1 + 1，动作也重写成**一眼能读完**的一条：
  基本动作（移动鼠标 / 鼠标点击 / 等待 / 按键点击）+ 循环（带循环变量 `i`）+ 如果 / 否则 + 结束宏运行。
  循环变量 `i` 与后面的条件 `{i} >= 2` 是**成对**的，用来演示"循环次数能拿到条件里用"。
- ⚠ **脚本的 `name` 不带 `.json`**，`path` 才带。这是产品的口径：
  列表入口用 `data.scriptName`（存盘时已 `StripJsonExtension`），兜底用 `path.stem()`
  —— 见 `webview_bridge_backend.cpp:3796`；重命名同口径见 `:1776`。
  Demo 早期给 `name` 加了 `.json`，于是编辑器「宏名称」显示成 `表格录入.json`，比软件多一个后缀。
- ⚠⚠ **专业模式的目录树必须从条目数据推导，不能写死**（`libraryFoldersFor()`）。
  旧实现返回写死的 `["示例宏","游戏辅助","办公自动化"]`，而 `macros` 里只有一条无目录的脚本
  ⇒ 专业模式显示 3 个空目录、脚本一条都不在里面，极简模式却正常列出 1 条 ——
  **同一份数据两种视图对不上**，用户看到的就是"极简和专业模式的脚本库不一样"
  （2026-09-28 用户反馈）。这就是 AGENTS.md §43「静态清单 × 动态数据必须同一份事实」。
- **目录管理在网页版是真做的**（内存态）：新建 / 改名 / 删除 / 移动脚本。
  旧实现对这五个调用一律 `fail(DEMO_HINT)` ⇒ 专业模式里那套库管理全是死的。

### 桥接桩的覆盖守卫（改 `ui/` 或桩之后必跑）

```powershell
node tools\verify\demo_bridge_coverage.js
```

比对 `ui/bridge.js` 暴露的请求类型 vs `bridge.stub.js` 处理的 `case`。
**桩没实现的方法不会报错** —— 调用发出去、没有任何 `.result` 回来，界面就静默少一块，
在浏览器里完全看不出来。2026-09-28 实测就是靠它一次找出 9 个缺口
（`previewScriptActions` 让专业模式的动作预览永远空着、`windowAgentList/Bind` 让窗口 Agents 页失效…）。

浏览器里**真做不到**的三件事会在界面上明确说明，而不是假装成功：
`pickScreenDrag` / `pickTemplateDrag`（要原生截图）、`systemReboot`（**绝不**真重启用户的电脑）。

### 设置的「扁平→嵌套」守卫（改设置字段之后必跑）

```powershell
node tools\verify\demo_settings_parity.js
```

产品前端的设置往返是**不对称**的：

| 方向 | 形状 |
|---|---|
| `collectSettings()` 发出去 | **扁平**：`{playbackCount, enablePlaybackCount, lowPerformanceMode, …}` |
| `fillSettings()` 读回来 | **嵌套**：`{playback:{playbackCount}, click:{…}, other:{…}}` |

也就是后端契约是「**收扁平、回嵌套**」。桩里那张 `FLAT_FIELD_SECTION`（93 条）
就是从 `webview_bridge_backend.cpp` 的 SaveSettings **抽出来的**，守卫做双向校验。

⚠ **别再手写白名单**：旧版是一份手写的 24 项 `otherFlat`，把 `playbackCount`、
`enablePlaybackCount`、`lowPerformanceMode` 这些键**静默丢掉**了大半年 ——
表现就是用户说的"导出没走设置"（界面上改了、`settings` 对象没变、exe 用旧值），
而且**浏览器里一个字都看不出来**。

⚠ 这条守卫在写出来的当天就揪出 **2 个客户端缺陷**：`exportScriptAsZip` 与
`enableWindowTimeScale` 这两个扁平键，界面一直在发、`SaveSettings` 从来没接过
⇒ 勾选保存后静默失效（其中"窗口时间缩放"更隐蔽：`fillSettings` 读到 `null` 时
默认显示为**勾选**，界面看起来一直是开的）。已在后端补上映射。

## 在线导出独立 EXE（网页版唯一"真做"的功能）

这是 Demo 里唯一不靠假数据的能力，也是最值得维护的一条：

```text
[QstPlayer.exe 模板] [脚本包 zip] [32 字节尾标]
                      ↑ 全部在浏览器里生成
```

- **零服务端算力**：服务器只发一个 5.5 MB 的静态模板；脚本 JSON、模板图、
  最终 exe 全部在用户浏览器里组装，**不上传任何用户数据**、不产生临时文件。
- **格式与产品完全一致**：同一套 payload 尾标（`QSTPKG01` + offset + size + FNV-1a64），
  所以网页导出的 exe 与 `QuickScriptTool.exe --export-exe` 的产物等价，也能被本软件导入。
- **继承当前设置**（2026-09-28 用户要求）：导出时把**用户在「设置」里改的那一份**
  原样打进 exe 的 `rt\app_settings.json` —— 与客户端 `SerializeAppSettings(...)` 行为一致。
  回放次数限制 / 随机间隔 / 点击次数上限 / 低性能模式 / 倍速… 都会跟着走。
  真机验证：同一脚本 `playbackCount=1` 与 `=3`，后者多跑 2 遍（实测差 ≈1.06 s）。
  ⚠ 前提是**桩认得设置页发来的扁平键**（见下方「设置的扁平→嵌套守卫」）。
  ⚠ 包里必须带设置快照：`enablePlaybackCount` 决定线性脚本跑完退不退出。
  **默认值已按用户要求改成「勾选 + 1 次」**（`src/app_settings.h`），与 Demo 的默认设置一致
  ⇒ 导出的 exe 默认"双击就跑、跑完自退"；要挂机循环就显式把次数调大或关掉勾选。
- **进度条 + 可中止**：取模板要下 5.5 MB（慢网络上好几秒），组装本身是毫秒级，
  所以进度条盯的就是下载那一段（显示 `已下载 / 总量`），期间可随时点「取消导出」中止
  （`AbortController`，取消不是错误，不会用红色语气报）。
- **两个入口，同一份代码**（`export/qst-web-export.js`）：
  1. `export/export.html`「在线脚本工坊」：从零写脚本 + 预设 + 上传模板图 + 导出。
  2. Demo 的宏编辑器 →「导出为独立 EXE」对话框：把**编辑器里当前脚本**原样导出
     （`bridge.stub.js` 里 `scanScriptForExport` / `exportScriptAsExe` 两个分支）。
     进度条由桩**注入**到对话框里（产品前端 `ui/` 不动 —— 客户端导出是本机 IO，不需要它）。
- **动作白名单**：鼠标（点击/移动/按下/松开/滚轮）、键盘（点击/按下/松开/输入）、
  等待、循环、找图、结束。AI / OCR / 启动程序 / 嵌套宏 / 后台窗口模式**不在网页版导出范围** ——
  被拒绝的动作会**逐条列名并说明原因**，不静默丢弃。
- **上限 128 个动作**（用户可感知的产品约束，写在扫描结果里）。
- ⚠ **缓存策略是 `default` 不是 `force-cache`**：后者会让老用户在发版后继续用**旧播放器模板**
  导出（产物照样能跑、只是跑旧引擎，完全看不出来）。用 `default` 让 nginx 的
  ETag/Last-Modified 正常生效，页面内再缓存一份保证"一次加载只下一次"。
- 离线校验：`node tools/verify/web_export_roundtrip.js`（54 条断言，含与 C++ 参考实现的哈希定值对齐、
  设置继承、进度事件、中止）。

## 从上游 ui/ 同步

**不要手改 `demo/` 里的任何 js/css/html** —— 它是 `ui/` 的拷贝 + 两处注入，
手改下次同步就丢。**一条命令同步**（仓库根，可直接双击）：

```cmd
sync_web.cmd                 :: ui\ → website\demo\ + 播放器模板 + 语法守卫
sync_web.cmd -Check          :: 只校验，不改文件（不一致 exit 1）
sync_web.cmd -Serve          :: 同步完顺手起本地预览
```

等价于 `powershell -ExecutionPolicy Bypass -File tools\sync_website.ps1`。它做三件事：

| 步 | 内容 |
|---|---|
| 1 | `ui\` → `website\demo\`（含「网页体验版」补丁），调 `tools\sync_website_demo.ps1` |
| 2 | `QstPlayer.exe` → `website\export\player\`，调 `tools\sync_web_export_player.ps1` |
| 3 | 对同步后的 demo JS 跑 `node --check` —— 这段 JS 会被直接推到公网，语法错必须在这里拦住 |

> **发版会自动做这件事**：`tools\package_release.ps1` 在同步 `website\downloads\` 之后
> **强制调用** `tools\sync_website.ps1`，失败即中止发版（`release.cmd` → `package_with_version.ps1`
> → `package_release.ps1` 这条链同样如此）。手动同步与发版同步走的是**同一份代码**，
> 不会出现两套逻辑各改一半。

`sync_website_demo.ps1` 具体做的事（`website/demo/` 相对 `ui/` 的**全部**差异）：

1. 逐字节拷贝 `ui/` 的前端文件（`app.js` / `bridge.js` / `visual_editor.js` /
   `pro-mode.js` / `shell.css` / `agent.css` / `pro-mode.css` / `app_icon.ico` /
   `vendor/`），**不含** `ui/debug.html`（开发用独立页，`index.html` 里只有一句注释提到它）。
2. `demo/index.html`：`<title>` 加「· 网页体验版」；在 `vendor/katex` 之前注入
   「网页体验版标识 + 小屏提示」块；按序注入
   `../export/qst-web-export.js` → `bridge.stub.js` → `bridge.js` → `agent-inline.js`。
3. `demo/agent.html`：`bridge.stub.js` 插到 `bridge.js` 之前。

> 为什么改成脚本：2026-09-28 发现 demo 已经**漂移了一整轮 UI 重构** ——
> 它还停在 `--qst-u` 变量化之前，`bridge.js` 里缺 `exportScriptAsExe` /
> `previewScriptActions` / `pickScreenDrag` 等一票桥接方法。
> 手工三步必然漂移，所以固化成脚本 + `-Check` + 进发版路径。

`demo/bridge.stub.js` 与 `demo/agent-inline.js` 是**官网独有**的，不在同步范围内。
