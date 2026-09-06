# QstVHid — Virtual HID（安全测试样本）

目标机安装由 **`_elevate_install.ps1` 一键完成**（虚拟 HID 与 Interception）。
**产品安装器禁止修改启动配置**：不关安全启动、不开测试签名、不关内存完整性、不向 Root 证书库灌入伪造 CA、
不把未加载成功的 Interception 写进键盘/鼠标 `LowerFilters`（那是旧版导致无法开机的根因）。

内核拒载该签名时安装会失败并回滚，请继续用「系统模拟」。需要驱动级注入时，须使用当前 Windows 真正信任的签名（WHQL / attestation），
或仅在**你已自行**开启测试签名的实验机上安装。

## 一键安装（推荐）

发版目录需包含：

```
driver\qst_vhid\_elevate_install.ps1
driver\qst_vhid\repair_boot.ps1
driver\qst_vhid\package\QstVHid.sys
driver\qst_vhid\package\qst_vhid.inf
driver\qst_vhid\package\qst_vhid.cat
```

用法：打开 QuickScriptTool → 设置 → **安装虚拟 HID / Interception 驱动**（管理员）。
「卸载并修复」会先拆掉键盘/鼠标类过滤残留，并删除旧版开机任务。

安全规则：

- Interception 的 class `LowerFilters` **仅在本会话 `sc start` 已 Running 之后**才写入；失败则删服务/文件，过滤表不动。
- 虚拟 HID 是 ROOT 软件设备，不是键盘/鼠标类过滤驱动。
- 每次运行都会清理旧版留下的 `QstVHidFinishInstall` / `QstVHidWaitSb` 开机任务。

日志：`driver\qst_vhid\install_log.txt`。也可管理员直接跑：

```powershell
cd <解压目录>\driver\qst_vhid
powershell -ExecutionPolicy Bypass -File .\_elevate_install.ps1 -Kind vhid
powershell -ExecutionPolicy Bypass -File .\_elevate_install.ps1 -Kind interception -Uninstall
powershell -ExecutionPolicy Bypass -File .\repair_boot.ps1
```

## 已经无法开机时（旧版安装）

1. 进安全模式或 WinRE（Shift+重启 → 疑难解答 → 高级选项 → 启动设置 → 4）。
2. 管理员运行 `repair_boot.ps1`（可先拷到 U 盘）。
3. 普通重启，**不要**再进固件关安全启动。
4. 若仍无键鼠：在注册表删除键盘/鼠标类 `LowerFilters` 里的 `Interception`（以及 ImagePath 指向 `interception.sys` 的项）。

## 兼容性（2026 年内核策略变化，重要）

微软 2026 年 4 月起逐步移除对「旧版交叉签名根计划」驱动签名的信任（Windows Driver Policy，
先 audit 后 enforce；2026-07 更新后可用 `CiTool.exe --remove-policy` 关闭，见
[The Windows Driver Policy](https://support.microsoft.com/en-us/Windows/Hardware/Drivers/the-windows-driver-policy)）。
更关键的是：**带 2026 年 4 月后累积更新的 Windows 11 24H2/25H2，内核 ci.dll 已不再信任
过期交叉证书链**——即使签名/时间戳完全合法（`signtool verify /kp` 通过、catalog 哈希一致），
加载时仍报 CI 3004 `file hash not found` + 0xC0000428（本机 25H2 26200.9168 实测）。

因此实测结论：

| 目标机器 | 过期 EV + 假时间戳 | 说明 |
|----------|-------------------|------|
| Windows 10 22H2 / 老版本 Win11（无 2026-04 后更新） | ✅ 可加载 | 产品安装器不再改启动策略 |
| Win11 24H2/25H2 + 2026-04 后更新（策略 audit/enforce） | ❌ 一键安装会拒绝/回滚 | **禁止**靠关安全启动/开测试签名绕过；请用系统模拟或 WHQL 包 |
| 任何机器 | 需 WHQL/attestation | 微软唯一继续信任的生产路径（需未过期 EV + Partner Center） |

Interception 官方驱动同样是 2011 年过期的 GlobalSign 普通代码签名（非 WHQL），
在新内核上同样会被拒载；产品安装器不会再把它提前写进 class 过滤表。

日志：`driver\qst_vhid\install_log.txt`（与安装脚本同目录，运行后生成）。

## 安全软件测试 vs 装好驱动注入

| 目的 | 做法 |
|------|------|
| 扫过期签名样本 | 扫 `package\QstVHid.sys`（或仓库 `bin\Release\QstVHid.sys`） |
| 用过期 EV + 假时间戳构造可加载样本 | 开发机：`sign_and_install.ps1 -SkipBuild -SignOnly -FakeTimestamp` |
| 装上驱动做键鼠注入（旧版系统，签名仍被信任） | `_elevate_install.ps1`（不改启动配置；拒载则回滚） |

**只改应用代码、不改 `.sys`：不必重签驱动。**  
只有重编/替换了 `QstVHid.sys`（或改 INF 导致目录哈希变）才需要重新构造 `package\`。

## 开发机：过期 EV + 假时间戳重签（需 WDK + SDK + 网络）

```powershell
cd <repo>\driver\qst_vhid
# 默认用 D:\other\EV\证书\上海明盛密码2015.pfx（密码 2015），时间戳回拨到 2015-06-20
powershell -ExecutionPolicy Bypass -File .\sign_and_install.ps1 -SkipBuild -SignOnly -FakeTimestamp

# 指定其他过期 EV 证书 / 时间：
# -PfxPath <pfx> -PfxPassword <pass> -TsTime 2012-06-01T08:00:00 -DriverVer 05/01/2012
# 自签开发包（仅测试签名环境）：
powershell -ExecutionPolicy Bypass -File .\sign_and_install.ps1 -SkipBuild -ForceLocalDevPfx
```

要点（脚本已自动处理）：

1. **时间顺序**：DriverVer ≤ SYS 签名时间戳 ≤ CAT 创建时间 ≤ CAT 签名时间戳
   （先嵌签 .sys → inf2cat → 签 .cat；签名期间自动回拨系统时钟，结束自动恢复）。
2. **catalog 必须含签名后 .sys 的哈希**，否则内核报 CI 3004。
3. 自动导出 `package\certs\`：EV 叶证书 + VeriSign 中间 CA + G5 + 微软交叉证书
   + 假时间戳 TSA 根（Pikachu），并写入签名机信任库供 `signtool verify /kp` 把关。
4. 默认 TSA 为 FakeSign 公共服务 `http://timers.524228.xyz/<UTC时间>`；
   可自建 TSA（FakeSign 项目）后换 `-TsUrl`。

> 注意：提权进程在部分环境读不到中文路径的 pfx（沙箱视图差异），建议把 pfx 复制到
> ASCII 路径（如 `D:\other\EV\_signing_assets\shanghai_2015.pfx`）再传 `-PfxPath`。

卸载：

```powershell
powershell -ExecutionPolicy Bypass -File .\sign_and_install.ps1 -Uninstall
```

## 安全收尾（真实鼠标假死）

杀毒软件若在注入会话中杀掉进程/删文件，旧版驱动可能留下虚拟键鼠「按下」状态，或 LL 钩子卡死导致**真实鼠标不动、触摸屏仍可用**（重启可恢复）。

当前缓解：

1. **驱动**：最后一个用户态句柄关闭时（含 `TerminateProcess`）自动提交空键盘/空鼠标报告  
2. **用户态**：`Close` 先抬起；异常/退出走 `input_emergency`；LL 回调卡死约 1.5s 看门狗卸钩  

改过 `driver.c` 后需重签并重装：

```powershell
powershell -ExecutionPolicy Bypass -File .\sign_and_install.ps1 -SkipBuild
# 或发版：_elevate_install.ps1
```

建议把发版目录加入杀毒信任区，避免误删 `QstVHid.sys` / `interception.dll`。

## 验收

```powershell
build\Release\VirtualHidSelfTest.exe --json
```

`device_open` 通过即可。
