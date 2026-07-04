# QuickScriptTool 安装包说明

## 为什么 Debug 压缩包在别人电脑上跑不起来？

你看到的 `MSVCP140**D**.dll`、`VCRUNTIME140**D**.dll` 等，文件名末尾的 **`D` 表示 Debug 版**运行库。

- Debug 版只在安装了 Visual Studio 的开发机上才有
- 普通用户电脑没有这些 DLL
- **不要把 `build\Debug` 发给别人**

请始终分发 **`Release` 构建**（`build\Release` 或本目录脚本生成的 `dist`）。

---

## 方式一：ZIP 绿色包（最简单）

在项目根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
```

会生成：

- `dist\QuickScriptTool\` — 可直接拷贝的文件夹
- `dist\QuickScriptTool-Release.zip` — 可发给他人的压缩包

若对方提示缺少 `VCRUNTIME140.dll`（注意没有 D），让对方安装微软官方运行库：

https://aka.ms/vs/17/release/vc_redist.x64.exe

---

## 方式二：Inno Setup 安装包（推荐）

1. 安装 [Inno Setup 6](https://jrsoftware.org/isdl.php)
2. 执行上面的 `package_release.ps1` 生成 `dist\QuickScriptTool`
3. （推荐）下载 [vc_redist.x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe) 放到 `installer\redist\`
4. 编译安装脚本：

```powershell
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\QuickScriptTool.iss
```

输出：`dist\QuickScriptTool-Setup-1.0.0.exe`

安装包会：

- 安装到 `C:\Program Files\QuickScriptTool`
- 创建开始菜单快捷方式（可选桌面图标）
- 若包含 `redist\vc_redist.x64.exe`，安装时自动安装 VC++ 运行库

---

## 发布目录应包含的文件

```
QuickScriptTool.exe
opencv_world4100.dll      （Release，不要带 d 的 Debug 版）
tools\
  paddle_ocr_helper.py
  requirements-ocr.txt
scripts\                    （用户数据，可为空）
recordings\                 （用户数据，可为空）
```

OCR 功能依赖 Python 3.12 与 PaddleOCR，安装到 `C:\paddle_env`。点击软件内「一键安装」即可自动完成（无需用户预先安装 Python）：

1. 自动下载并安装 Python 3.12（若 `tools\python-3.12.10-amd64.exe` 或 `tools\python312\` 已随包附带，则离线安装）
2. 创建虚拟环境并 pip 安装 PaddleOCR 等依赖

打包时可选执行 `tools\download_python312.ps1`，将 Python 安装包放入 `tools\`，便于无网络环境分发。
