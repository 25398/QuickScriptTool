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
└── demo/             # 从 ui/ 拷贝并适配后的可运行产品前端
    ├── index.html    # 主界面（已注入 bridge.stub.js + 网页体验版标识）
    ├── bridge.stub.js# 核心：WebView2 桥接桩（假数据 / 假状态机 / 体验版提示）
    ├── agent-inline.js# 主壳 AI 对话改为页面内联弹窗（不开新页面）
    ├── bridge.js     # 原产品桥接脚本（原样保留）
    ├── app.js / pro-mode.js / shell.css / pro-mode.css / agent.css
    ├── app_icon.ico
    └── vendor/katex/ # 原产品依赖（数学公式渲染，未删减以保证功能一致）
```

## 本地预览

任意静态服务器均可（路径必须用相对路径，直接双击打开 index.html 也能看，但 iframe
页面在部分浏览器下建议用 http 方式访问）：

```powershell
# 方式一：Python
cd D:\other\software\website
python -m http.server 8080
# 打开 http://localhost:8080/

# 方式二：PowerShell（无需安装）
cd D:\other\software\website
Start-Process -WindowStyle Hidden powershell -ArgumentList '-NoProfile','-Command','$l=[System.Net.HttpListener]::new();$l.Prefixes.Add("http://localhost:8080/");$l.Start();while($l.IsListening){$c=$l.GetContext();$p=[System.IO.Path]::Combine((Get-Location).Path,$c.Request.Url.AbsolutePath.TrimStart("/"));if([System.IO.File]::Exists($p)){$c.Response.ContentType="text/html";[System.IO.File]::ReadAllBytes($p)|ForEach-Object{$c.Response.OutputStream.Write($_,0,$_.Length)}};$c.Response.Close()}'
```

> 推荐用 Python / Node / 宝塔自带静态服务器预览；打开后重点看：Demo 是否白屏、
> 四个主 Tab 能否切换、设置弹窗 / 宏编辑器能否打开、控制台是否有致命报错。

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
6. 备案号：把页脚 `沪ICP备XXXXXXXX号` 占位替换为真实备案号。
7. **下载文件**：官网按钮链到固定名 `downloads/QuickScriptTool-Setup.exe` 与
   `downloads/QuickScriptTool-Release.zip`（发版覆盖即可，不必改 HTML）。带版本号文件仍会同步到同目录作归档。
   由 `tools\package_release.ps1` + Inno Setup 生成。体积约数百 MB，上传时注意超时。

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
| 找图 / OCR / 选区 / 截图 | 全部拦截，弹出「网页体验版仅演示界面」提示 |
| 热键捕获 | 拦截并提示（预设热键选项仍可点击体验） |
| 定时任务 | 预置示例任务，可浏览 / 新建 / 删除（内存态） |
| AI Agent 对话 | 主壳页面内联弹窗（不新开页面），假对话流：发送消息后流式返回预设回复，不调用真实模型 |
| 导入 / 导出 / 安装驱动 / 检查更新 | 拦截并提示 |
| 下载客户端按钮 | 跳转 `downloads/` 下安装包 / 便携 zip |

## 从上游 ui/ 同步

本目录 `demo/` 从仓库 `ui/` 拷贝而来，改动点：

1. `demo/index.html`：`<title>` 加「网页体验版」；在 `bridge.js` 前注入
   `bridge.stub.js`；追加网页体验版 badge（右下角）与小屏提示。
2. `demo/index.html`：在 `bridge.js` 之后、`app.js` 之前注入 `agent-inline.js`
   （主壳 AI 对话走内联弹窗，不再请求原生打开独立窗口）。
3. `demo/agent.html`：在 `bridge.js` 前注入 `bridge.stub.js`（独立页仍可单独访问）。
4. 其余文件与 `ui/` 保持一致；上游 UI 更新时重新拷贝后重做上述三步即可。
3. 其余文件与 `ui/` 保持一致；上游 UI 更新时重新拷贝后重做上述两步即可。
