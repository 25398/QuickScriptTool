# 官网下载产物（部署时整目录上传到站点根下的 downloads/）

| 文件 | 说明 |
|------|------|
| `QuickScriptTool-Setup.exe` | **官网固定链接**（安装包最新版；发版覆盖即可） |
| `QuickScriptTool-Release.zip` | **官网固定链接**（便携版最新版；发版覆盖即可） |
| `QuickScriptTool-<版本>.exe` | 带版本号安装包（归档 / 外链可用） |
| `QuickScriptTool-Setup-<版本>.exe` | 同上（旧 Setup- 命名别名） |
| `QuickScriptTool-Release-<版本>.zip` | 带版本号便携包 |
| `QuickScriptTool-HidDriver.zip` | 可选：Interception.dll + 内核驱动样本（设置里按需下载；不进默认安装包） |

官网 HTML 请链到无版本号的固定名，避免每次发版改网页。

重新发版（推荐用仓库根的一键脚本，版本号会自动写进全部 **7 处**）：

```cmd
release.cmd 1.3.4                 :: 便携 zip + 安装包，跑完自动同步进本目录
release.cmd 1.3.4 -Mode zip       :: 过渡版本只发便携 zip（跳过安装包，省 2-3 分钟）
release.cmd 1.3.4 -Mode setup     :: 只发安装包
```

也可以手工分步（等价流程）：

```powershell
# 1) 改版本：tools\product_version.txt / installer\QuickScriptTool.iss 的 MyAppVersion /
#    src\app_branding.cpp / resources\QuickScriptTool.rc 的四处版本号
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\QuickScriptTool.iss
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -SkipBuild
```

最后一步会把 Setup / Zip（含固定别名）同步进本目录。

⚠️ ISCC 那步要 2-3 分钟且中间**没有输出**，不要中途关窗口 —— 中断会留下半截 exe
（体积远小于正常值，如 80 MB vs 200 MB），官网的安装包就会停留在上一版，
而 zip 却是新的，出现「新旧混装」。
