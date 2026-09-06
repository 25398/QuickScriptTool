# 官网下载产物（部署时整目录上传到站点根下的 downloads/）

| 文件 | 说明 |
|------|------|
| `QuickScriptTool-Setup.exe` | **官网固定链接**（安装包最新版；发版覆盖即可） |
| `QuickScriptTool-Release.zip` | **官网固定链接**（便携版最新版；发版覆盖即可） |
| `QuickScriptTool-1.1.10.exe` | 带版本号安装包（归档 / 外链可用） |
| `QuickScriptTool-Setup-1.1.10.exe` | 同上（旧 Setup- 命名别名） |
| `QuickScriptTool-Release-1.1.10.zip` | 带版本号便携包 |

官网 HTML 请链到无版本号的固定名，避免每次发版改网页。

重新发版后执行：

```powershell
# 1) 改版本：tools\product_version.txt + installer\QuickScriptTool.iss MyAppVersion + src\app_branding.cpp
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\QuickScriptTool.iss
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -SkipBuild
```

最后一步会把 Setup / Zip（含固定别名）同步进本目录。
