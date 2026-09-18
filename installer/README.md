# QuickScriptTool 安装包说明

## 为什么 Debug 压缩包在别人电脑上跑不起来？

你看到的 `MSVCP140**D**.dll`、`VCRUNTIME140**D**.dll` 等，文件名末尾的 **`D` 表示 Debug 版**运行库。

- Debug 版只在安装了 Visual Studio 的开发机上才有
- 普通用户电脑没有这些 DLL
- **不要把 `build\Debug` 发给别人**

请始终分发 **`Release` 构建**（`tools\package_release.ps1` 生成的 `dist`）。

---

## 发版依赖策略

`tools\package_release.ps1` 会把**运行主程序所需依赖**全部打进 `dist\QuickScriptTool`（再打 zip / 供 Inno 安装）：

| 随包（解压/安装即可用） | 软件内按钮 / 运行时再装 |
|-------------------------|-------------------------|
| `WebView2Fixed\` | Interception / 虚拟 HID（设置里下载 `QuickScriptTool-HidDriver.zip` 再 UAC 安装） |
| MSVC x64 CRT（`vcruntime140*.dll` / `msvcp140*.dll` 旁路） | Python + PaddleOCR（OCR 一键安装） |
| OpenCV / FakeFocus / VirtualDesktopAccessor | 缺 OpenCV 仍可进壳，找图禁用 |
| `extension\edge`、`ui\`、VHID **安装脚本**（不含 `.sys` / `interception.dll`） | |

版本号以 `tools\product_version.txt` 为准，须与 `installer\QuickScriptTool.iss` 的 `MyAppVersion`、`src\app_branding.cpp` 一致。

---

## 方式一：ZIP 绿色包

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
```

产物：

- `dist\QuickScriptTool\` — 可直接拷贝的文件夹
- `dist\QuickScriptTool-Release-<ver>.zip`（及同内容别名 `QuickScriptTool-Release.zip`）

用户**无需**再安装 `vc_redist.x64.exe`（CRT 已在 exe 旁）。

---

## 方式二：Inno Setup 安装包

1. 安装 [Inno Setup 6](https://jrsoftware.org/isdl.php)
2. 确认 `tools\product_version.txt` == `installer\QuickScriptTool.iss` 里 `MyAppVersion`
3. 执行 `package_release.ps1` 生成 `dist\QuickScriptTool`
4. 编译安装脚本：

```powershell
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\QuickScriptTool.iss
# 产物：dist\QuickScriptTool-<ver>.exe（官网主下载名；另同步 QuickScriptTool-Setup-<ver>.exe 别名）
```

5. 再跑一次同步官网下载目录：

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -SkipBuild
```

输出：`dist\QuickScriptTool-Setup-<ver>.exe`（当前 **1.1.2**）

安装包会：

- 安装到 `C:\Program Files\QuickScriptTool`
- 递归装入整个 dist（含 MSVC CRT 旁路 DLL 与 WebView2Fixed）
- 创建开始菜单快捷方式（可选桌面图标）

---

## 发布目录应包含的文件（摘要）

```
QuickScriptTool.exe
vcruntime140.dll / vcruntime140_1.dll / msvcp140*.dll   （MSVC CRT 旁路）
opencv_world4100.dll
FakeFocus64.dll / FakeFocus32.dll
VirtualDesktopAccessor10.dll / VirtualDesktopAccessor11.dll / VirtualDesktopAccessor11_23h2.dll
interception.dll
WebView2Fixed\
ui\
extension\edge\
driver\qst_vhid\
skills\agent\
tools\                      （OCR helper；可选 python 离线包）
scripts\ / recordings\      （用户数据，可为空）
```

OCR：点击软件内「一键安装」即可（无需用户预先装 Python）。打包机可先跑 `tools\download_python312.ps1` 预置离线安装包。
