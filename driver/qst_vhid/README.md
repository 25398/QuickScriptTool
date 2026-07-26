# QstVHid — Virtual HID（安全测试样本）

目标机安装走 **过期 EV 签名样本 + 导入根证**，不依赖 testsigning / WDK。

## 发版包内一键安装（推荐）

发版目录需包含：

```
driver\qst_vhid\_elevate_install.ps1
driver\qst_vhid\import_certs.reg
driver\qst_vhid\package\QstVHid.sys
driver\qst_vhid\package\qst_vhid.inf
driver\qst_vhid\package\qst_vhid.cat
```

用法：打开 QuickScriptTool → 设置 → **安装虚拟 HID 驱动**（管理员）。  
若未就绪，重启后再点一次。日志：`driver\qst_vhid\install_log.txt`。

也可管理员直接跑：

```powershell
cd <解压目录>\driver\qst_vhid
powershell -ExecutionPolicy Bypass -File .\_elevate_install.ps1
```

## 安全软件测试 vs 装好驱动注入

| 目的 | 做法 |
|------|------|
| 扫过期签名样本 | 扫 `package\QstVHid.sys`（或仓库 `bin\Release\QstVHid.sys`） |
| 用过期 PFX 构造样本 | 开发机：`sign_and_install.ps1 -SkipBuild -SignOnly` |
| 装上驱动做键鼠注入 | `_elevate_install.ps1`（会导入 `import_certs.reg`） |

**只改应用代码、不改 `.sys`：不必重签驱动。**  
只有重编/替换了 `QstVHid.sys`（或改 INF 导致目录哈希变）才需要重新构造 `package\`。

## 开发机：重签 / 编驱动（需 WDK + SDK）

```powershell
cd <repo>\driver\qst_vhid
powershell -ExecutionPolicy Bypass -File .\sign_and_install.ps1 -SkipBuild -SkipResign
# 或强制本机自签：
powershell -ExecutionPolicy Bypass -File .\sign_and_install.ps1 -SkipBuild -ForceLocalDevPfx
```

卸载：

```powershell
powershell -ExecutionPolicy Bypass -File .\sign_and_install.ps1 -Uninstall
```

## 验收

```powershell
build\Release\VirtualHidSelfTest.exe --json
```

`device_open` 通过即可。
