# 最新内核（2026-04+）测试机部署：Custom Kernel Signers（CKS）

> 适用：Windows 11 24H2/25H2 + 2026 年 4 月非安全更新之后的机器。
> 这类机器内核已移除「旧版交叉签名根计划」信任，**过期 EV + 假时间戳无法再加载**（CI 3004 / 0xC0000428）。
> CKS 是微软官方为「无法/不想提交 WHQL 的自研驱动」提供的通道，Secure Boot 保持开启即可加载自签驱动，
> 并可覆盖 2026 的 Windows Driver Policy。

官方文档：[Custom kernel signers for App Control for Business](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers)

## 硬性要求（先核对）

- AMD64 的 Windows 11 24H2+（**Home 版不支持**；Server 按官方平台表确认）。
- UEFI 2.3+，Secure Boot 开启。
- **全新安装/重置**：CKS 必须在设备初始设置阶段启用；运行中的系统无法事后启用
  （“The feature won't enable on currently running systems”），部署策略后需执行一次
  Windows 重置（Push-button reset）才会生效。
- 自有 PKI（根 + 代码签名叶子证书），且能访问 UEFI 固件变量（PK/KEK/DB）。
- 装有 Windows SDK（signtool）+ App Control 工具（`New-CIPolicy` / `ConvertFrom-CIPolicy` /
  `Set-RuleOption` / `Set-CIPolicyIdInfo` / `Add-SignerRule`）。

## 信任链

```
Secure Boot PK/KEK（你或 OEM 持有）
  └─ 签名 CI 策略（App Control policy，OID 1.3.6.1.4.1.311.79.1）
       └─ 策略内 <Signers> 列出你的驱动签名证书（TBS hash）
            └─ Windows 内核 CI 按策略放行你的驱动
```

策略一旦部署，只能用同一授权签名更新；没有有效替换策略就移除会导致无法启动（需进 UEFI 关
Secure Boot 恢复）。先在小规模实验机上验证，密钥管理要有恢复预案。

## 部署步骤（实验机）

### 0. 准备测试 PKI

生成自己的根 CA + 策略签名证书 + 驱动代码签名证书（自签即可，供测试用）。证书建议：

- 策略签名证书：EKU 含 Code Signing；链到 PK/KEK。
- 驱动签名证书：EKU 含 Code Signing；链到你的根。

### 1. Secure Boot 密钥（二选一）

**A. 你持有该机 PK**：用 PK 签 KEK 追加内容，把策略签名证书加入 KEK：

```powershell
Format-SecureBootUEFI -Name KEK -SignatureOwner "<你的GUID>" `
  -ContentFilePath C:\KEK\KEK_SigList.bin -FormatWithCert `
  -Certificate C:\KEK\policy_signer.cer `
  -SignableFilePath C:\KEK\KEK_Signable_SigList.bin -AppendWrite:$true
signtool sign /fd sha256 /p7 .\ /p7co 1.2.840.113549.1.7.1 /p7ce DetachedSignedData `
  /a /f PK.pfx C:\KEK\KEK_Signable_SigList.bin
```

**B. 你不持有 PK（常见实验机）**：UEFI 里临时关 Secure Boot → 进入 Setup Mode →
按序写入 DB（微软 UEFI CA 2023 + OEM）、KEK（微软 KEK CA 2K 2023 + 你的策略签名证书）、
PK（你的平台密钥或保留 OEM PK）→ 重新开启 Secure Boot。
（密钥文件见微软《Secure Boot Key Creation and Management Guidance》。）

### 2. 生成 App Control 策略并加入你的签名者

> 以下 cmdlet（`New-CIPolicy` / `ConvertFrom-CIPolicy` / `Set-RuleOption` /
> `Set-CIPolicyIdInfo` / `Add-SignerRule`）来自 `ConfigCI` 模块，先用
> `Import-Module ConfigCI` 加载；管理员 PowerShell 中运行。

```powershell
# 以默认强制策略为模板
$base = "$env:windir\schemas\CodeIntegrity\ExamplePolicies\DefaultWindows_Enforced.xml"
# 扫描本机已信任的签名者（保留 Windows/硬件驱动信任）
New-CIPolicy -ScanPath 'C:\' -UserPEs -NoScript -FilePath .\ScannedPolicy.xml -Level PCACertificate -Fallback Hash
Merge-CIPolicy -PolicyPaths $base, .\ScannedPolicy.xml -OutputFilePath .\CksPolicy.xml

# 必须移除“未签名策略”选项；只做内核模式可再删 UMCI(Option 0)
Set-RuleOption -Option 6 -FilePath .\CksPolicy.xml -Delete
Set-RuleOption -Option 0 -FilePath .\CksPolicy.xml -Delete   # 可选

# 加入策略签名证书（<UpdateSigners>）与驱动签名证书（<Signers>）
Add-SignerRule -CertificatePath .\policy_signer.cer -FilePath .\CksPolicy.xml -Update
Add-SignerRule -CertificatePath .\driver_signer.cer -FilePath .\CksPolicy.xml -Kernel
```

### 3. 转换 + 签名策略

```powershell
Set-CIPolicyIdInfo -ResetPolicyID -FilePath .\CksPolicy.xml
$PolicyId = ([xml](Get-Content .\CksPolicy.xml)).SiPolicy.PolicyId
ConvertFrom-CIPolicy .\CksPolicy.xml -BinaryFilePath (".\" + $PolicyId + ".cip")
signtool sign /fd sha256 /p7 .\ /p7co 1.3.6.1.4.1.311.79.1 /p7ce Embedded `
  /a /f policy_signer.pfx (".\" + $PolicyId + ".cip")
```

### 4. 部署到 ESP

```powershell
mountvol s: /s
copy (".\" + $PolicyId + ".cip") s:\EFI\Microsoft\Boot\CiPolicies\Active\
mountvol s: /d
```

### 5. 重置 Windows（必须）

`Settings → System → Recovery → Reset this PC`（保留文件重置即可）。重启后 CKS 生效，
Code Integrity 事件日志（`Microsoft-Windows-CodeIntegrity/Operational`）可确认策略加载。

### 6. 签 QstVHid 并安装

用你的驱动签名证书（链到测试根）按常规方式签 `QstVHid.sys` + `qst_vhid.cat`：

```powershell
powershell -ExecutionPolicy Bypass -File .\sign_and_install.ps1 -SkipBuild -SignOnly `
  -PfxPath <驱动签名证书.pfx> -PfxPassword <pass>
```

然后管理员运行 `_elevate_install.ps1`（或设置页一键安装）。Secure Boot 保持开启；
2026 Driver Policy 由 CKS 策略覆盖，无需 `CiTool --remove-policy`。

## HVCI / 内存完整性注意

- 社区实测（2026-06，MS Q&A）：**CKS 自签驱动在 HVCI 关闭时可加载；HVCI 开启时即使策略已信任
  该证书，仍可能报 0xC0000428**。微软答复口径前后不一（先称需把签名者加入 HVCI 感知策略，
  后又称链需满足 KMCS 要求），目前没有稳定的“CKS + HVCI 同时开”的公开方案。
- MS Q&A 官方答复（2026-06）的正式口径是：要在 HVCI 下放行 CKS 驱动，需把签名者显式加入
  一份 **HVCI-aware 的 CI 策略**（`HVCIEnabled` 选项），但按该答复引用的实测结果，仅这样仍
  不足以保证成功，落地前务必在目标机型上验证。
- QstVHid 是 KMDF+VHF 驱动、无可执行内存分配，本身满足 HVCI 兼容性；若验证 AV/EDR 时
  HVCI 必须开着，建议用 WHQL 签名的 Interception 后端，或把驱动提交 WHCP/attestation。

## 与“找新漏洞”路线的区别

CKS 是官方功能，不是漏洞，稳定、可持续、EDR 视为正常签名加载。研究圈还有另外几条
非官方路线用于最新内核（BYOVD 漏洞驱动、开机期 DSE 补丁、UEFI bootkit），它们属于
漏洞利用类：EDR/块列表会识别、随补丁失效，且不适合作为 AV 检测验证的稳定基线。
