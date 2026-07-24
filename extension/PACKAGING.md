# Edge / Chrome 配套扩展 — 打包与安装规范

源码目录（唯一真相）：`extension/edge/`  
**不是** C++ 编译产物；构建时复制到程序旁，发版时必须一并带走。

## 必带文件清单

| 路径 | 说明 |
|------|------|
| `manifest.json` | MV3；`version` 行末必须是 **逗号** `,`，禁止 `;` |
| `background.js` | `BRIDGE_VERSION` 必须与 manifest `version` **完全一致** |
| `popup.html` / `popup.js` | 工具栏弹窗（脚本列表 / 运行停止） |
| `options.html` / `options.js` | 选项页（重连本机桥） |
| `guide.html` | 安装引导 |
| `README.txt` | 给人看的短说明 |
| `icons/icon16.png` `icon48.png` `icon128.png` | 图标 |

缺任一文件 = 安装包不合格。

## 一键校验 + 打侧载 zip

在仓库根目录：

```powershell
powershell -ExecutionPolicy Bypass -File tools\pack_edge_extension.ps1
```

成功时：

- 校验 JSON / 版本对齐 / 文件齐全
- 产出 `dist\QstEdgeBridge-<version>.zip`（**zip 根目录就是 manifest.json**）

仅校验不打 zip：

```powershell
powershell -ExecutionPolicy Bypass -File tools\pack_edge_extension.ps1 -SkipZip
```

## 跟软件一起发版（必走）

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
```

该脚本会：

1. 编 Release `QuickScriptTool`
2. **调用** `pack_edge_extension.ps1`（失败则整包失败）
3. 把 `extension\edge` 拷进 `dist\QuickScriptTool\extension\edge`
4. 再打 `dist\QuickScriptTool-Release.zip`

然后用 Inno Setup 编 `installer\QuickScriptTool.iss`（源目录为 `dist\QuickScriptTool`）。  
ISS 要求 `{app}\extension\...` **必须存在**，缺扩展会编不过。

## 用户怎么装扩展

### A. 开发 / 本机调试（推荐）

加载解压缩目录：

- 仓库：`extension\edge`
- 或构建输出：`build\Release\extension\edge` / `build\Debug\extension\edge`

### B. 发给别人（侧载 zip）

1. 发 `dist\QstEdgeBridge-<ver>.zip`
2. 解压到文件夹（打开后应直接看到 `manifest.json`）
3. Edge（**游戏同一用户配置**）→ `edge://extensions` → 开发人员模式 →「加载解压缩的扩展」

### C. 安装包用户

安装后目录：`{安装目录}\extension\edge`  
同样用「加载解压缩的扩展」指向该文件夹。

## 视觉协议（v1.0.21+）

宿主 CDP 窗口模式找图走扩展，**不再**为找图 Win32 展开宏桌面窗。

| 桥命令 | 作用 |
|--------|------|
| `hello` | 回 `vision:true`、`capabilities:["vision","mouse","keys","layout"]` |
| `vision` / `screenshot` | `Page.captureScreenshot` → HTTP `POST /qst/shot`；WS 只回小 JSON（`shotHttp:true`）；禁止 WS 大 base64 |
| `mouse` / `cdp` | 键鼠（surface→iframe） |
| `layout` | iframe/pageCss 几何 |

版本：`manifest.json` == `BRIDGE_VERSION` == **1.1.35**（或更新）。


## 顶栏弹窗与脚本 API（v1.0.22+）

鼠大侠进程启动后本机桥常开（端口 19228–19240）。工具栏弹窗通过 HTTP：

- `GET /qst/status` — 发现桥与运行状态
- `GET /qst/scripts?token=` — 列出 `scripts\*.json`
- `POST /qst/run` / `POST /qst/stop` — 运行/停止（需 token）

`manifest.json` 的 `action.default_popup` 指向 `popup.html`。

## Agent / 打包时硬规则

1. 改扩展版本：同时改 `manifest.json` 的 `version` 与 `background.js` 的 `BRIDGE_VERSION`
2. 改完跑 `tools\pack_edge_extension.ps1`（或至少 `-SkipZip`）通过再宣称可用
3. 打安装包 / Release zip：**禁止**跳过 `extension\edge`；`package_release.ps1` 失败即停
4. 不要手改 `build\*\extension\edge` 当唯一源——改 `extension\edge`，构建会复制
5. CMake `POST_BUILD` 已 `copy_directory` 源到 exe 旁；ISS 从 `dist` 再拷一次

## 常见翻车

| 现象 | 原因 |
|------|------|
| Manifest is not valid JSON … line 4 | `version` 行写成了分号 `;` |
| 日志仍显示旧版本 | Edge 未点「重新加载」或装错用户配置 |
| 安装包里没有 extension | 没跑 `package_release.ps1` 或拷错 dist |
| zip 加载失败 | zip 里多包了一层目录，选错了文件夹 |
