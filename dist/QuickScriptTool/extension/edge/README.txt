鼠大侠（Edge / Chrome MV3 扩展）

════════════════════════════════════
安装（加载解压缩的扩展）
════════════════════════════════════
1. 打开【游戏所用】的同一 Edge「用户配置」窗口
2. 地址栏进入 edge://extensions ，打开「开发人员模式」
3. 「加载解压缩的扩展」→ 选本文件夹
   （本文件夹内应直接能看到 manifest.json）
4. 改代码后务必点「重新加载」——版本须与鼠大侠日志一致（当前 1.0.12）
5. 打开扩展「选项」页；跑宏时点「重新连接」

若你拿到的是 QstEdgeBridge-*.zip：
  先解压到文件夹，再对【解压后的文件夹】做第 3 步（不要选 zip 本身）

════════════════════════════════════
给打包/发版的人（Agent）
════════════════════════════════════
规范文档（仓库根相对路径）：
  extension\PACKAGING.md

一键校验 + 打侧载 zip：
  powershell -File tools\pack_edge_extension.ps1

跟软件一起打 Release / 安装包素材：
  powershell -File tools\package_release.ps1
  （缺扩展会失败；再编 installer\QuickScriptTool.iss）

════════════════════════════════════
成功标志（鼠大侠日志）
════════════════════════════════════
- 「扩展桥已 attach 标签: …（扩展 v1.0.12）」
- 「扩展输入目标 via=iframe focus=iframe-target」
