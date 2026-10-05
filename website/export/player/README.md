# website/export/player —— 在线导出用的播放器模板

这个目录里需要有一个 **`QstPlayer.exe`**，它是网页版导出的「EXE 模板」。

## 它是干什么的

「在线脚本工坊」和官网 Demo 的「导出为独立 EXE」都在**浏览器里**完成组装：

```text
[QstPlayer.exe] [脚本包 zip] [32 字节尾标]
```

也就是说：网页只是把「脚本数据」追加到这个现成的播放器后面（格式与产品的
`QuickScriptTool.exe --export-exe` 完全一致，由 `src/script_package.cpp`
的 `AppendPayloadToExe` 定义）。**浏览器里不编译任何代码**，也不需要服务端算力。

## ⚠ 部署前必须确认它存在

`.gitignore` 里 `*.exe` 被忽略（与 `website/downloads/*.exe` 同一策略：
二进制由发版脚本生成，不入库）。所以**全新克隆的仓库里这个文件是缺的**，
直接上传 `website/` 会导致网页导出报「播放器模板不是有效的 exe（HTTP 404）」。

补上它有三种方式，任选其一：

```powershell
# 1) 只同步模板（最快；需要先构建过一次 QstScriptPlayer）
powershell -ExecutionPolicy Bypass -File tools\sync_web_export_player.ps1

# 2) 完整发版（会顺带同步模板 + 安装包 + 便携包到 website\）
release.cmd 1.4.0

# 3) 手工拷贝
copy build\Release\tools\player\QstPlayer.exe website\export\player\QstPlayer.exe
```

## 上传后怎么确认

浏览器打开站点上的 `export/selftest.html`：

- 标题变成 `[PASS] 导出链路自检` ⇒ 模板在位、能下载、能拼出可用的 exe。
- 出现 `播放器模板可下载且是有效 PE ✖ HTTP 404` ⇒ 就是忘了传这个文件。

**本地也要这么测**：直接双击 `demo/index.html` 是 `file://`，浏览器禁止 `fetch` 本地文件，
导出**一定**失败（只报一句 `Failed to fetch`）。用仓库自带的本地服务器：

```powershell
powershell -ExecutionPolicy Bypass -File tools\serve_website.ps1
# → http://127.0.0.1:8090/export/selftest.html
```

## 体积

模板当前约 **5.5 MB**（纯输入脚本导出的 exe 也就是这个量级 + 几 KB 脚本数据）。
它随静态站点分发，占服务器磁盘 5.5 MB，**不需要任何服务端 CPU**。

> 找图类脚本要在目标电脑上跑，还需要 `opencv_world4100.dll`（约 64.6 MB）。
> 网页版**不**内嵌它，而是让播放器复用目标电脑上已装的「键鼠工坊」；
> 没装客户端时那一类动作会跳过（不会崩）。详见 `docs/web-online-export.md`。
