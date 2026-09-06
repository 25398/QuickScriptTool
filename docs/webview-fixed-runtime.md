# WebView2 Fixed Version 便携运行时

## 用户一句话

**便携版：解压后运行 exe 即可，无需安装 WebView2 / Edge；请整夹一起拷贝，不要只拷 exe。**

## 版本钉扎

| 项 | 值 |
|----|-----|
| 文件 | `tools/webview2_fixed_version.txt` |
| 当前版本 | `133.0.3065.92`（x64） |
| 获取脚本 | `tools/fetch_webview2_fixed.ps1` |
| 缓存目录 | `third_party/webview2_fixed/<ver>/WebView2Fixed/`（**不入 git**） |
| 发版 zip | `tools/package_webview_portable.ps1` → `dist/QstWebViewShell-Portable.zip` |
| 参考体积 | zip ≈ **244 MB**；解压后 `WebView2Fixed` ≈ **557 MB**（随版本变化） |

CAB 来源：官方 Fixed Version 包（与 Microsoft 网站同名 cab）。因官网直链会轮换，脚本默认使用社区归档中的同名 cab URL（见 version 文件第 3 行），可自行改成你下载的官方 cab 路径或 URL。

## 运行时定位

`QstWebViewShell` 调用：

```text
CreateCoreWebView2EnvironmentWithOptions(
  <exe>\WebView2Fixed,      // browserExecutableFolder（内含 msedgewebview2.exe）
  <exe>\WebView2UserData,   // userDataFolder（可写）
  ...
)
```

- Fixed 目录缺失 → 提示「安装包不完整」，**不**引导安装系统 Runtime。
- Fixed 目录存在但**起不来**（环境/控制器创建失败、浏览器进程退出、首次导航失败、7s 未画出页面）→ 自动回退系统已装 WebView2（Edge 自带），仅在两者都失败时才弹「安装包不完整或 WebView2 未能启动」。
- 编译开关：`QST_WEBVIEW_ALLOW_EVERGREEN=ON` 额外允许 Fixed 目录缺失时直接回退系统 Evergreen（开发用）。
- Win10：应用启动时用 Win32 API（`SetNamedSecurityInfo`，不依赖外部 icacls）对 Fixed 目录递归授予 AppContainer RX（Fixed ≥120 要求）；首次成功写 `.qst_acl_ok` 跳过。
- 诊断：每次启动在 exe 同目录写 `webview_boot.log`（环境创建 / ACL / 导航 / ProcessFailed / contentReady / 回退原因）。仍出现淡蓝空白时，把该 log 发回即可定位。

## 本地命令

```powershell
# 仅下载/展开 Fixed Runtime 到 third_party 缓存
powershell -ExecutionPolicy Bypass -File tools\fetch_webview2_fixed.ps1

# 编壳 + 打便携 zip
powershell -ExecutionPolicy Bypass -File tools\package_webview_portable.ps1
```

产物：

```text
dist/QstWebViewShell-Portable/
  QstWebViewShell.exe
  ui\...
  WebView2Fixed\...
  WebView2UserData\
  README-portable.txt
dist/QstWebViewShell-Portable.zip
```
