#Requires -RunAsAdministrator
<#
.SYNOPSIS
  LAB ONLY — build/sign QstVHid on a developer machine. Never ship, never call from settings UI.
  Product install is _elevate_install.ps1 (no BCD / no Root CA import / no clock rollback).
#>
param(
    [string]$PfxPath = "",
    [string]$PfxPassword = "Abc123456",
    [switch]$SkipBuild,
    [switch]$SkipResign,   # use package\ (or stage from bin) without re-signing; must already verify
    [switch]$ForceLocalDevPfx,  # if Preferred PFX missing OR expired, create/use certs\QstVHidDev.pfx
    [switch]$AllowExpiredPfx,   # keep expired Preferred PFX (default behavior now; switch retained for clarity)
    [switch]$SignOnly,          # sign package\ then exit; skip verify gate + pnputil install
    [switch]$Uninstall,
    [string]$StageDir = "",     # 签名暂存目录（默认 driver\qst_vhid\package；受环境写入干扰时可改到仓库外）
    [switch]$SkipStage,         # 不重建暂存目录/不拷贝（文件已由外部准备好）
    # 过期 EV + 自制时间戳（旧版跨签兼容路径）：Secure Boot / HVCI 保持开启也能加载
    [switch]$FakeTimestamp,
    [string]$TsUrl = "http://timers.524228.xyz/",   # 假时间戳服务器（RFC 3161）
    [string]$TsTime = "",                           # 空=按 PFX 有效期自动选（UTC，如 2015-06-20T08:00:00）
    [string]$DriverVer = "",                        # 空=按 TsTime 自动回推（MM/DD/YYYY）
    [string]$CrossCertPath = ""                     # 微软 Code Verification Root 给 VeriSign G5 的交叉证书
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PfxPath) {
    if ($FakeTimestamp) {
        # 默认用 SHA-256 叶子、有效期覆盖 2015 兼容窗口的 EV 证书
        $ev2015 = "D:\other\EV\证书\上海明盛密码2015.pfx"
        if (Test-Path -LiteralPath $ev2015) { $PfxPath = $ev2015; $PfxPassword = "2015" }
        else { $PfxPath = Join-Path $Root "certs\Beijing_Changlang.pfx" }
    } else {
        $localPfx = Join-Path $Root "certs\Beijing_Changlang.pfx"
        $legacyPfx = "D:\other\EV\证书\Beijing_Changlang_Abc123456.pfx"
        if (Test-Path -LiteralPath $localPfx) { $PfxPath = $localPfx }
        elseif (Test-Path -LiteralPath $legacyPfx) { $PfxPath = $legacyPfx }
        else { $PfxPath = $localPfx }
    }
}
$Cfg = "Release"
$OutDir = Join-Path $Root "bin\$Cfg"
$SysPath = Join-Path $OutDir "QstVHid.sys"
$InfPath = Join-Path $Root "qst_vhid.inf"
$PackageDir = Join-Path $Root "package"
if ($StageDir) { $PackageDir = $StageDir }

function Find-Tool([string]$Name) {
    $kits = "C:\Program Files (x86)\Windows Kits\10\bin"
    $hit = Get-ChildItem $kits -Recurse -Filter $Name -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' -or $_.FullName -match '\\x86\\' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if ($hit) { return $hit.FullName }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Test-SysSignature([string]$File, [string]$Signtool) {
    if (-not (Test-Path -LiteralPath $File)) { return $false }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Signtool verify /pa /q $File 2>&1 | Out-Null
    $ok = ($LASTEXITCODE -eq 0)
    $ErrorActionPreference = $prev
    return $ok
}

function Test-IsOrphanQstVHidService {
    $svcKey = "HKLM:\SYSTEM\CurrentControlSet\Services\QstVHid"
    if (-not (Test-Path $svcKey)) { return $false }
    $img = [string](Get-ItemProperty $svcKey -ErrorAction SilentlyContinue).ImagePath
    $start = (Get-ItemProperty $svcKey -ErrorAction SilentlyContinue).Start
    # Good INF install: ImagePath under DriverStore / System32\drivers
    $normalized = $img -replace '^\?\?\\', '' -replace '^\\??\\', ''
    $inStore = $normalized -match '(?i)DriverStore\\FileRepository\\qst_vhid' `
        -or $normalized -match '(?i)\\System32\\drivers\\QstVHid\.sys' `
        -or $normalized -match '(?i)%13%\\QstVHid\.sys'
    if (-not $inStore) { return $true }
    if ($start -eq 4) { return $true }  # DISABLED
    return $false
}

function Remove-OrphanQstVHidService {
    <#
      Old manual installs left HKLM\...\Services\QstVHid with:
        ImagePath = \??\<repo>\bin\Release\QstVHid.sys
        Start = 4 (DISABLED)
      That causes PnP devices to sit at Code 32 (CM_PROB_DISABLED_SERVICE)
      even after a successful pnputil /add-driver.
    #>
    $svcKey = "HKLM:\SYSTEM\CurrentControlSet\Services\QstVHid"
    if (-not (Test-Path $svcKey)) {
        Write-Host "No QstVHid service key."
        return
    }
    if (-not (Test-IsOrphanQstVHidService)) {
        Write-Host "QstVHid service looks like a normal DriverStore install — leaving it."
        return
    }
    $img = (Get-ItemProperty $svcKey -ErrorAction SilentlyContinue).ImagePath
    $start = (Get-ItemProperty $svcKey -ErrorAction SilentlyContinue).Start
    Write-Host "Orphan QstVHid service ImagePath=$img Start=$start — removing"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    sc.exe stop QstVHid 2>&1 | Out-Host
    Start-Sleep -Seconds 1
    sc.exe delete QstVHid 2>&1 | Out-Host
    $ErrorActionPreference = $prev
    # If still present (DeleteFlag while RUNNING), force-clear for this boot
    if (Test-Path $svcKey) {
        Write-Warning "Service key still present (reboot may be needed). Forcing Start=DEMAND(3) and DriverStore ImagePath if available."
        Set-ItemProperty -Path $svcKey -Name Start -Value 3 -Type DWord -ErrorAction SilentlyContinue
        $repo = Get-ChildItem "C:\Windows\System32\DriverStore\FileRepository" -Directory -Filter "qst_vhid.inf_*" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($repo) {
            $sysInStore = Get-ChildItem $repo.FullName -Filter "*.sys" | Select-Object -First 1
            if ($sysInStore) {
                $storePath = "\??\" + $sysInStore.FullName
                Set-ItemProperty -Path $svcKey -Name ImagePath -Value $storePath -ErrorAction SilentlyContinue
                Write-Host "Repointed ImagePath to DriverStore: $storePath"
            }
        }
    } else {
        Write-Host "Orphan QstVHid service removed."
    }
}

function Remove-StaleQstVHidDevices {
    $devs = Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match 'QSTVHID' -or $_.FriendlyName -match 'QST Virtual HID' }
    foreach ($d in $devs) {
        Write-Host "Removing device $($d.InstanceId) status=$($d.Status)"
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        pnputil /remove-device $d.InstanceId 2>&1 | Out-Host
        $ErrorActionPreference = $prev
    }
}

function Assert-QstVHidReady {
    $ok = Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object {
            ($_.FriendlyName -eq 'QST Virtual HID' -or $_.InstanceId -match '^ROOT\\QSTVHID\\') -and
            $_.Status -eq 'OK'
        }
    if (-not $ok) {
        $bad = Get-PnpDevice -ErrorAction SilentlyContinue |
            Where-Object { $_.FriendlyName -eq 'QST Virtual HID' -or $_.InstanceId -match 'QSTVHID' } |
            ForEach-Object { "$($_.InstanceId) Status=$($_.Status) Problem=$($_.Problem)" }
        $detail = if ($bad) { ($bad -join "; ") } else { "no QSTVHID device node" }
        throw "QST Virtual HID not OK after install ($detail). Reboot and retry, or check Device Manager for Code 32 orphan service."
    }

    # PnP Status=OK is not enough — interface must exist for usermode OpenByInterface
    $ifaceKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\{a7c3e9f1-4b2d-4e8a-9c1f-6d5e8a7b3c2d}'
    $ifaceOk = (Test-Path $ifaceKey) -and (@(Get-ChildItem $ifaceKey -ErrorAction SilentlyContinue).Count -gt 0)
    if (-not $ifaceOk) {
        $svc = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\QstVHid' -ErrorAction SilentlyContinue
        $hint = "Device node OK but GUID_DEVINTERFACE_QSTVHID not registered (driver EvtDeviceAdd likely never succeeded)."
        if ($svc -and $svc.DeleteFlag -eq 1) {
            $hint += " Service is marked for delete — REBOOT, then run this script again."
        } else {
            $hint += " Ensure import_certs.reg was applied and package\ signature sample is intact, then reboot and retry."
        }
        throw $hint
    }
    Write-Host "Device ready: $($ok.InstanceId -join ', ')"
    Write-Host "Device interface GUID present."
}

function Ensure-SigningPfx {
    param(
        [string]$PreferredPath,
        [string]$Password,
        [bool]$ForceLocalOnExpiredOrMissing
    )

    $certsDir = Join-Path $Root "certs"
    if (-not (Test-Path $certsDir)) { New-Item -ItemType Directory -Path $certsDir | Out-Null }

    $usePath = $PreferredPath
    $usePass = $Password
    $needDev = $false
    $expired = $false

    if (-not (Test-Path -LiteralPath $usePath)) {
        Write-Warning "PFX missing: $usePath"
        if ($ForceLocalOnExpiredOrMissing) {
            Write-Warning "ForceLocalDevPfx: will create local code-signing cert"
            $needDev = $true
        } else {
            throw "PFX not found: $usePath (pass -ForceLocalDevPfx to generate certs\QstVHidDev.pfx)"
        }
    } else {
        $secure = ConvertTo-SecureString -String $usePass -AsPlainText -Force
        $probe = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2
        $probe.Import((Resolve-Path -LiteralPath $usePath).Path, $secure,
            [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable)
        Write-Host "PFX Subject=$($probe.Subject)"
        Write-Host "PFX NotAfter=$($probe.NotAfter) HasKey=$($probe.HasPrivateKey)"
        if ($probe.NotAfter -lt (Get-Date)) {
            $expired = $true
            Write-Warning "PFX expired ($($probe.NotAfter)). Keeping it as signing source (no auto-fallback)."
            Write-Warning "SignTool / Windows may still reject; use -ForceLocalDevPfx only if you want the old local-dev path."
            if ($ForceLocalOnExpiredOrMissing) {
                Write-Warning "ForceLocalDevPfx: falling back to local code-signing cert."
                $needDev = $true
            }
        }
    }

    if ($needDev) {
        $usePath = Join-Path $certsDir "QstVHidDev.pfx"
        $usePass = "QstVHid-Dev-Only"
        if (-not (Test-Path -LiteralPath $usePath)) {
            Write-Host "Creating self-signed code signing cert: $usePath"
            $cert = New-SelfSignedCertificate -Type CodeSigningCert `
                -Subject "CN=QST Virtual HID Dev" `
                -CertStoreLocation Cert:\LocalMachine\My `
                -NotAfter (Get-Date).AddYears(5)
            $secureOut = ConvertTo-SecureString -String $usePass -AsPlainText -Force
            Export-PfxCertificate -Cert $cert -FilePath $usePath -Password $secureOut | Out-Null
        }
        $expired = $false
    }

    return @{ Path = $usePath; Password = $usePass; Expired = $expired }
}

function Sign-File {
    param(
        [string]$Signtool,
        [string]$File,
        [string]$Pfx,
        [string]$Password,
        [bool]$SoftFail = $false,
        [string]$TsUrl = ""   # 非空=用该 RFC3161 时间戳 URL（FakeTimestamp 路径）
    )
    # Native stderr must not abort under $ErrorActionPreference=Stop
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    if ($TsUrl) {
        & $Signtool sign /fd sha256 /f $Pfx /p $Password /tr "$TsUrl" /td sha256 $File 2>&1 | Out-Host
    } else {
        & $Signtool sign /fd sha256 /f $Pfx /p $Password /tr http://timestamp.digicert.com /td sha256 $File 2>&1 | Out-Host
    }
    $rc = $LASTEXITCODE
    if ($rc -ne 0) {
        Write-Warning "Timestamp sign failed (rc=$rc); retry without timestamp"
        & $Signtool sign /fd sha256 /f $Pfx /p $Password $File 2>&1 | Out-Host
        $rc = $LASTEXITCODE
    }
    $ErrorActionPreference = $prev
    if ($rc -ne 0) {
        if ($SoftFail) {
            Write-Warning "signtool failed for $File (rc=$rc) — SoftFail keeps going (lab sample path)."
            return $false
        }
        throw "signtool failed for $File (rc=$rc)"
    }
    return $true
}

function Get-CrossCertPath {
    param([string]$Explicit)
    if ($Explicit -and (Test-Path -LiteralPath $Explicit)) { return $Explicit }
    $hit = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\CrossCertificates' -Recurse `
        -Filter 'VRSN_C3_PCA_G5_Root_CA_Cross.cer' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

function Resolve-FakeTsTime {
    param(
        [System.Security.Cryptography.X509Certificates.X509Certificate2]$Cert,
        [string]$Wanted
    )
    if ($Wanted) { return $Wanted }
    # 首选 2015-06-20（位于 2015-07-29 兼容窗口内、且在大多数 EV 证书有效期内）
    $candidate = [datetime]::ParseExact('2015-06-20T08:00:00', 'yyyy-MM-ddTHH:mm:ss', $null,
        [System.Globalization.DateTimeStyles]::AssumeUniversal)
    if ($candidate -ge $Cert.NotBefore -and $candidate -le $Cert.NotAfter) { return '2015-06-20T08:00:00' }
    # 更老的证书：取有效期内的第 30 天（仍有足够余量）
    $fallback = $Cert.NotBefore.AddDays(30)
    if ($fallback -ge $Cert.NotAfter) { $fallback = $Cert.NotBefore.AddDays(1) }
    return $fallback.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss')
}

function Save-ClockState { return @{ Time = Get-Date } }

function Set-ClockBack {
    param([string]$IsoUtc)
    $target = [datetime]::ParseExact($IsoUtc, 'yyyy-MM-ddTHH:mm:ss', $null,
        [System.Globalization.DateTimeStyles]::AssumeUniversal).ToLocalTime()
    try { & w32tm /config /syncfromflags:manual 2>&1 | Out-Null } catch {}
    try { Stop-Service w32time -Force -ErrorAction SilentlyContinue } catch {}
    Set-Date $target
    Write-Host "System clock set to $target for legacy-timestamp signing (restored afterwards)."
}

function Restore-Clock {
    param($State)
    try {
        Set-Date $State.Time
        Write-Host "System clock restored to $($State.Time)"
    } catch { Write-Warning "Clock restore failed: $_" }
    try { Start-Service w32time -ErrorAction SilentlyContinue } catch {}
    try { & w32tm /config /syncfromflags:domhier 2>&1 | Out-Null } catch {}
    try { & w32tm /resync 2>&1 | Out-Null } catch {}
}

function Export-LegacyChainCerts {
    param(
        [string]$Pfx,
        [string]$Password,
        [string]$CrossCert,
        [string]$OutDir
    )
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $col = [System.Security.Cryptography.X509Certificates.X509Certificate2Collection]::new()
    $col.Import($Pfx, $Password, 'DefaultKeySet')
    foreach ($c in $col) {
        $name = $null
        if ($c.HasPrivateKey) { $name = 'leaf' }
        elseif ($c.Subject -match 'Class 3 Code Signing 2010 CA') { $name = 'intermediate' }
        elseif ($c.Subject -match 'Public Primary Certification Authority - G5') { $name = 'g5_root' }
        if ($name) {
            $out = Join-Path $OutDir "$name.cer"
            [System.IO.File]::WriteAllBytes($out, $c.Export('Cert'))
            Write-Host "  exported $name.cer ($($c.Subject))"
        }
    }
    if ($CrossCert) {
        Copy-Item -LiteralPath $CrossCert -Destination (Join-Path $OutDir 'ms_codeverification_g5_cross.cer') -Force
        # 签名机本地也装上，方便 signtool verify /kp 通过
        certutil -addstore -f Root $CrossCert 2>&1 | Out-Null
        Write-Host "  exported ms_codeverification_g5_cross.cer + imported into LocalMachine\Root"
    }
    $int = Join-Path $OutDir 'intermediate.cer'
    if (Test-Path $int) { certutil -addstore -f Root $int 2>&1 | Out-Null }
    # 假时间戳服务器的 TSA 根链：签名机与目标机都必须信任，否则内核验时间戳失败（0x80096005）
    $srcCerts = Join-Path $Root 'certs'
    foreach ($n in @('ROOTCA-CRT.crt', 'TIMECA-CRT.crt', 'ROOT-CA-G1.crl', 'ROOT-CA-G2.crl',
                     'TIME-CA-G1.crl', 'TIME-CA-G2.crl')) {
        $src = Join-Path $srcCerts $n
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $OutDir $n) -Force
            if ($n -like '*.crt') {
                certutil -addstore -f Root $src 2>&1 | Out-Null
                certutil -addstore -f AuthRoot $src 2>&1 | Out-Null
            } else {
                certutil -addstore -f AuthRoot $src 2>&1 | Out-Null
            }
            Write-Host "  exported + trusted $n"
        } else {
            Write-Warning "  missing TSA cert asset: $src"
        }
    }
}

function Import-PfxToStores {
    param([string]$Path, [string]$Password)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "PFX not found: $Path"
    }
    $secure = ConvertTo-SecureString -String $Password -AsPlainText -Force
    $cert = Import-PfxCertificate -FilePath $Path -CertStoreLocation Cert:\LocalMachine\My -Password $secure -Exportable
    Write-Host "Imported to LocalMachine\My: $($cert.Thumbprint)"
    foreach ($storeName in @("Root", "TrustedPublisher")) {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, "LocalMachine")
        $store.Open("ReadWrite")
        $store.Add($cert)
        $store.Close()
        Write-Host "Added to LocalMachine\$storeName"
    }
    return $cert
}

function Ensure-RootDevice {
    $code = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class QstVhidSetup {
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr hwndParent);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiCreateDeviceInfo(IntPtr DeviceInfoSet, string DeviceName,
        ref Guid ClassGuid, string DeviceDescription, IntPtr hwndParent, uint CreationFlags,
        ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiSetDeviceRegistryProperty(IntPtr DeviceInfoSet,
        ref SP_DEVINFO_DATA DeviceInfoData, uint Property, byte[] PropertyBuffer, uint PropertyBufferSize);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiCallClassInstaller(uint InstallFunction, IntPtr DeviceInfoSet,
        ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA {
        public uint cbSize;
        public Guid ClassGuid;
        public uint DevInst;
        public IntPtr Reserved;
    }

    const uint DICD_GENERATE_ID = 0x00000001;
    const uint SPDRP_HARDWAREID = 0x00000001;
    const uint DIF_REGISTERDEVICE = 0x00000019;
    // System class
    static readonly Guid GUID_DEVCLASS_SYSTEM = new Guid("4d36e97d-e325-11ce-bfc1-08002be10318");

    public static int CreateRootDevice(string hwid) {
        Guid cls = GUID_DEVCLASS_SYSTEM;
        IntPtr set = SetupDiCreateDeviceInfoList(ref cls, IntPtr.Zero);
        if (set == IntPtr.Zero || set == (IntPtr)(-1)) return Marshal.GetLastWin32Error();

        SP_DEVINFO_DATA data = new SP_DEVINFO_DATA();
        data.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
        if (!SetupDiCreateDeviceInfo(set, "QSTVHID", ref cls, "QST Virtual HID",
                IntPtr.Zero, DICD_GENERATE_ID, ref data)) {
            int err = Marshal.GetLastWin32Error();
            SetupDiDestroyDeviceInfoList(set);
            return err;
        }

        byte[] ids = Encoding.Unicode.GetBytes(hwid + "\0\0");
        if (!SetupDiSetDeviceRegistryProperty(set, ref data, SPDRP_HARDWAREID, ids, (uint)ids.Length)) {
            int err = Marshal.GetLastWin32Error();
            SetupDiDestroyDeviceInfoList(set);
            return err;
        }

        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref data)) {
            int err = Marshal.GetLastWin32Error();
            SetupDiDestroyDeviceInfoList(set);
            return err;
        }

        SetupDiDestroyDeviceInfoList(set);
        return 0;
    }
}
'@
    if (-not ("QstVhidSetup" -as [type])) {
        Add-Type -TypeDefinition $code -Language CSharp
    }
    $rc = [QstVhidSetup]::CreateRootDevice("Root\QSTVHID")
    if ($rc -ne 0 -and $rc -ne 2150011010) { # ignore already exists-ish
        Write-Host "CreateRootDevice returned $rc (may already exist)"
    } else {
        Write-Host "Root\QSTVHID device node ensured (code=$rc)"
    }
}

if ($Uninstall) {
    Write-Host "Removing QstVHid packages..."
    Remove-StaleQstVHidDevices
    # Force-remove service regardless of orphan detection
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    sc.exe stop QstVHid 2>&1 | Out-Host
    Start-Sleep -Seconds 1
    sc.exe delete QstVHid 2>&1 | Out-Host
    $ErrorActionPreference = $prev
    $block = pnputil /enum-drivers | Out-String
    $oemNames = [System.Collections.Generic.HashSet[string]]::new()
    # 中文系统 pnputil 输出本地化（发布名称/原始名称），两套标签都要匹配
    $nameLabel = '(?:Published Name|发布名称)'
    $origLabel = '(?:Original Name|原始名称)'
    foreach ($m in [regex]::Matches($block, "(?is)$nameLabel\s*:\s*(oem\d+\.inf).*?$origLabel\s*:\s*qst_vhid\.inf")) {
        [void]$oemNames.Add($m.Groups[1].Value)
    }
    # Fallback: any block mentioning qst_vhid
    if ($oemNames.Count -eq 0) {
        $chunks = $block -split "(?=$nameLabel\s*:)"
        foreach ($c in $chunks) {
            if ($c -match 'qst_vhid\.inf' -and $c -match "$nameLabel\s*:\s*(oem\d+\.inf)") {
                [void]$oemNames.Add($Matches[1])
            }
        }
    }
    if ($oemNames.Count -eq 0) {
        Write-Host "No published qst_vhid.inf found (already removed?)"
    } else {
        foreach ($name in $oemNames) {
            Write-Host "Deleting $name"
            $prev = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            pnputil /delete-driver $name /uninstall /force 2>&1 | Out-Host
            $ErrorActionPreference = $prev
        }
    }
    $ErrorActionPreference = "Continue"
    sc.exe delete QstVHid 2>&1 | Out-Host
    # 还原我们开启的测试签名（仅当安装脚本记录过 SetTestSigning=1）
    $marker = 'HKLM:\SOFTWARE\QuickScriptTool\QstVHid'
    $setByUs = (Get-ItemProperty $marker -Name 'SetTestSigning' -ErrorAction SilentlyContinue).SetTestSigning
    if ($setByUs -eq 1) {
        Write-Host "Disabling test signing (enabled by QuickScriptTool)..."
        bcdedit /set testsigning off 2>&1 | Out-Host
        Remove-ItemProperty -Path $marker -Name 'SetTestSigning' -ErrorAction SilentlyContinue
    }
    $ErrorActionPreference = "Stop"
    exit 0
}

# --- Build ---
if (-not $SkipBuild) {
    $msbuild = "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
    if (-not (Test-Path $msbuild)) {
        throw "MSBuild not found: $msbuild"
    }
    & $msbuild (Join-Path $Root "QstVHid.vcxproj") /p:Configuration=$Cfg /p:Platform=x64 /m /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "Driver build failed" }
}

if (-not (Test-Path $SysPath)) {
    throw "Missing $SysPath — build first or drop Release binary there"
}

$signtool = Find-Tool "signtool.exe"
$inf2cat = Find-Tool "Inf2Cat.exe"
if (-not $signtool) { throw "signtool.exe not found (install Windows SDK)" }
if (-not $inf2cat) { throw "Inf2Cat.exe not found (install WDK)" }

# Warn if the "already signed" bin copy is expired / invalid
if (-not (Test-SysSignature -File $SysPath -Signtool $signtool)) {
    Write-Warning "bin\Release\QstVHid.sys signature is NOT trusted (often expired commercial cert)."
    Write-Warning "Cannot load that file as-is. Will stage + re-sign into package\ (unless -SkipResign)."
}

# --- Preflight (before mutating services) ---
$svcPre = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\QstVHid' -ErrorAction SilentlyContinue
if ($svcPre -and $svcPre.DeleteFlag -eq 1) {
    if ($SignOnly) {
        Write-Warning "QstVHid service DeleteFlag=1 — ignored for SignOnly."
    } else {
        throw "QstVHid service DeleteFlag=1 (marked for delete). Reboot first, then run this script again."
    }
}

# Clean leftovers that make Code 32 (skip when only constructing signed samples)
if (-not $SignOnly) {
    Write-Host "Cleaning orphan service / stale devices..."
    Remove-StaleQstVHidDevices
    Remove-OrphanQstVHidService
    $svcAfterClean = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\QstVHid' -ErrorAction SilentlyContinue
    if ($svcAfterClean -and $svcAfterClean.DeleteFlag -eq 1) {
        throw "Orphan service cleaned but still marked DeleteFlag=1 while running. Reboot, then run this script once more."
    }
} else {
    Write-Host "SignOnly: skipping service/device cleanup."
}

# Stage package: INF + SYS side by side（-SkipStage 时假定文件已在 PackageDir）
if (-not $SkipStage) {
    if (Test-Path $PackageDir) { Remove-Item $PackageDir -Recurse -Force }
    New-Item -ItemType Directory -Path $PackageDir | Out-Null
    Copy-Item $SysPath (Join-Path $PackageDir 'QstVHid.sys')
    Copy-Item $InfPath (Join-Path $PackageDir 'qst_vhid.inf')
} else {
    if (-not (Test-Path (Join-Path $PackageDir 'QstVHid.sys'))) {
        throw "SkipStage: missing $(Join-Path $PackageDir 'QstVHid.sys')"
    }
}

Push-Location $PackageDir
$usedExpiredPfx = $false
try {
    if ($SkipResign) {
        # 本地开发证书（certs\QstVHidDev.pfx）重签 .sys + .cat。
        # 踩坑：这里曾硬编码旧指纹 274B9D2A...，那把钥匙不存在后 .cat 变成
        # “不受信任根”签名——pnputil 装包成功但设备永不绑定，设置页 exit 2。
        # 必须从 pfx 动态取当前指纹，且先嵌签 .sys 再 inf2cat 再签 .cat。
        $devPfxPath = Join-Path $Root "certs\QstVHidDev.pfx"
        $devPfxPass = "QstVHid-Dev-Only"
        if (-not (Test-Path -LiteralPath $devPfxPath)) {
            throw "SkipResign requires certs\QstVHidDev.pfx — recreate via: sign_and_install.ps1 -SkipBuild -ForceLocalDevPfx"
        }
        $devCert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2
        $secureDev = ConvertTo-SecureString $devPfxPass -AsPlainText -Force
        $devCert.Import((Resolve-Path -LiteralPath $devPfxPath).Path, $secureDev,
            [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable)
        Write-Host "SkipResign: signing package with dev cert thumb=$($devCert.Thumbprint) ($($devCert.Subject))"
        $ErrorActionPreference = "Continue"
        & $Signtool sign /fd sha256 /f $devPfxPath /p $devPfxPass (Join-Path $PackageDir 'QstVHid.sys') 2>&1 | Out-Host
        & $inf2cat /driver:. /os:10_X64 /verbose 2>&1 | Out-Host
        $ErrorActionPreference = "Stop"
        $cat = Get-ChildItem $PackageDir -Filter '*.cat' | Select-Object -First 1 -ExpandProperty FullName
        # Try to sign catalog with dev cert so pnputil accepts the package
        Write-Host "Signing catalog with dev cert for pnputil acceptance..."
        $ErrorActionPreference = "Continue"
        & $Signtool sign /fd sha256 /f $devPfxPath /p $devPfxPass $cat 2>&1 | Out-Host
        $ErrorActionPreference = "Stop"
        # 同步导出与 pfx 匹配的公钥证书，供目标机导入 Root/TrustedPublisher
        Export-Certificate -Cert $devCert -FilePath (Join-Path $Root 'QstVHidDev.cer') -Type CERT -Force | Out-Null
        Write-Host "Exported matching cert to driver\qst_vhid\QstVHidDev.cer"
    } elseif ($FakeTimestamp) {
        # ---- 过期 EV + 自制时间戳（旧版跨签兼容路径，Secure Boot/HVCI 无需关闭）----
        $forceLocal = [bool]$ForceLocalDevPfx
        $resolved = Ensure-SigningPfx -PreferredPath $PfxPath -Password $PfxPassword `
            -ForceLocalOnExpiredOrMissing $forceLocal
        if ($resolved.Expired) {
            Write-Warning "Expired EV PFX in use — will backdate RFC3161 timestamp (legacy cross-signed path)."
        }
        $PfxPath = $resolved.Path
        $PfxPassword = $resolved.Password
        $usedExpiredPfx = $true
        Import-PfxToStores -Path $PfxPath -Password $PfxPassword | Out-Null

        $cross = Get-CrossCertPath -Explicit $CrossCertPath
        if (-not $cross) {
            throw "VeriSign G5 的微软交叉证书未找到（需 WDK CrossCertificates 或 -CrossCertPath）"
        }

        # 1) 叶证书 + 解析安全时间戳（UTC）
        $leafProbe = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($PfxPath, $PfxPassword)
        $ts = Resolve-FakeTsTime -Cert $leafProbe -Wanted $TsTime
        $dv = if ($DriverVer) { $DriverVer } else {
            ([datetime]::ParseExact($ts, 'yyyy-MM-ddTHH:mm:ss', $null,
                [System.Globalization.DateTimeStyles]::AssumeUniversal).AddDays(-30).ToString('MM/dd/yyyy'))
        }
        # 时间顺序：DriverVer <= SYS签名TS <= CAT创建(系统时钟) <= CAT签名TS
        $tsBase = [datetime]::ParseExact($ts, 'yyyy-MM-ddTHH:mm:ss', $null,
            [System.Globalization.DateTimeStyles]::AssumeUniversal)
        $catTs = $tsBase.AddDays(2).ToString('yyyy-MM-ddTHH:mm:ss')
        $clockTarget = $tsBase.AddDays(1).ToString('yyyy-MM-ddTHH:mm:ss')
        Write-Host "Legacy EV signing: DriverVer=$dv, SYS-TS=$($TsUrl)$ts, CAT-TS=$($TsUrl)$catTs"

        # 2) 改 package INF 的 DriverVer 到兼容窗口（<= 签名时间）
        $infCopy = Join-Path $PackageDir 'qst_vhid.inf'
        $infText = [System.IO.File]::ReadAllText($infCopy)
        $infText = $infText -replace '(?im)^\s*DriverVer\s*=.*$', "DriverVer   = $dv,1.0.0.1"
        [System.IO.File]::WriteAllText($infCopy, $infText)

        # 3) 回拨系统时钟到 SYS 签名时间之后一天，按顺序：先嵌签 .sys → 再 inf2cat 生成 CAT
        #    （catalog 哈希必须取自“签名后的 .sys”，否则 CI 3004 hash not found）→ 再签 CAT。
        $clockState = Save-ClockState
        try {
            Set-ClockBack $clockTarget
            # 让 staged .sys 的文件时间与假时钟一致（尽力而为；mtime 不影响 /kp 校验）
            try {
                (Get-Item -LiteralPath (Join-Path $PackageDir 'QstVHid.sys')).LastWriteTime = Get-Date
            } catch {
                Write-Warning "  (could not align QstVHid.sys LastWriteTime: $($_.Exception.Message))"
            }
            # 1) 先嵌签 .sys（时间戳 = ts）
            [void](Sign-File -Signtool $signtool -File (Join-Path $PackageDir 'QstVHid.sys') `
                -Pfx $PfxPath -Password $PfxPassword -SoftFail $false -TsUrl ($TsUrl + $ts))
            # 2) 再生成 CAT（系统时钟已回拨到 ts+1d，catalog 哈希来自已签名的 .sys）
            $ErrorActionPreference = "Continue"
            & $inf2cat /driver:. /os:10_X64 /verbose 2>&1 | Out-Host
            $ErrorActionPreference = "Stop"
            if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed" }
            $cat = Join-Path $PackageDir 'qst_vhid.cat'
            if (-not (Test-Path $cat)) {
                $cat = Get-ChildItem $PackageDir -Filter '*.cat' | Select-Object -First 1 -ExpandProperty FullName
            }
            # 3) 最后签 CAT（时间戳 = ts+2d，满足 cat创建 <= cat签名）
            if ($cat) {
                [void](Sign-File -Signtool $signtool -File $cat -Pfx $PfxPath -Password $PfxPassword `
                    -SoftFail $false -TsUrl ($TsUrl + $catTs))
            }
        } finally {
            Restore-Clock $clockState
        }

        # 4) 导出链证书（leaf/intermediate/g5 + 微软交叉证书）供目标机导入
        Export-LegacyChainCerts -Pfx $PfxPath -Password $PfxPassword -CrossCert $cross `
            -OutDir (Join-Path $PackageDir 'certs')
    } else {
        $forceLocal = [bool]$ForceLocalDevPfx
        # AllowExpiredPfx is default; ForceLocalDevPfx opts back into old fallback.
        $resolved = Ensure-SigningPfx -PreferredPath $PfxPath -Password $PfxPassword `
            -ForceLocalOnExpiredOrMissing $forceLocal
        $PfxPath = $resolved.Path
        $PfxPassword = $resolved.Password
        $usedExpiredPfx = [bool]$resolved.Expired
        if ($AllowExpiredPfx -and $usedExpiredPfx) {
            Write-Host "AllowExpiredPfx: proceeding with expired PFX as signing source."
        }
        Import-PfxToStores -Path $PfxPath -Password $PfxPassword | Out-Null

        $ErrorActionPreference = "Continue"
        & $inf2cat /driver:. /os:10_X64 /verbose 2>&1 | Out-Host
        $ErrorActionPreference = "Stop"
        if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed" }
        $cat = Join-Path $PackageDir 'qst_vhid.cat'
        if (-not (Test-Path $cat)) {
            $cat = Get-ChildItem $PackageDir -Filter '*.cat' | Select-Object -First 1 -ExpandProperty FullName
        }
        $soft = [bool]($SignOnly -or $usedExpiredPfx)
        [void](Sign-File -Signtool $signtool -File (Join-Path $PackageDir 'QstVHid.sys') -Pfx $PfxPath -Password $PfxPassword -SoftFail $soft)
        if ($cat) {
            [void](Sign-File -Signtool $signtool -File $cat -Pfx $PfxPath -Password $PfxPassword -SoftFail $soft)
        }
    }
} finally {
    Pop-Location
}

$pkgSys = Join-Path $PackageDir 'QstVHid.sys'
$verifyOk = Test-SysSignature -File $pkgSys -Signtool $signtool
if (-not $verifyOk) {
    $hint = 'package\QstVHid.sys still fails signature verify'
    if ($SignOnly -or $usedExpiredPfx -or $SkipResign) {
        Write-Warning "$hint — continuing (SignOnly / expired-PFX / SkipResign lab path). Sample left at: $pkgSys"
    } else {
        throw "$hint - aborting install"
    }
}

# FakeTimestamp 路径必须额外通过内核策略（/kp）校验，否则 Secure Boot 下内核必拒载
if ($FakeTimestamp) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $signtool verify /kp /v $pkgSys 2>&1 | Out-Host
    $kpOk = ($LASTEXITCODE -eq 0)
    $ErrorActionPreference = $prev
    if (-not $kpOk) {
        throw "Legacy EV package failed signtool verify /kp — cross-cert chain not accepted. Aborting."
    }
    Write-Host "signtool verify /kp OK: kernel policy chain accepted (Microsoft Code Verification Root)."
}

if ($SignOnly) {
    Write-Host ""
    Write-Host "SignOnly done. Staged/signed files under: $PackageDir"
    Write-Host "No pnputil install performed."
    exit 0
}

Ensure-RootDevice

Write-Host "Installing driver package..."
$prev = $ErrorActionPreference
$ErrorActionPreference = "Continue"
pnputil /add-driver (Join-Path $PackageDir 'qst_vhid.inf') /install 2>&1 | Out-Host
$pnpRc = $LASTEXITCODE
$ErrorActionPreference = $prev
if ($pnpRc -ne 0) {
    Write-Warning "pnputil /install exit $pnpRc - check Device Manager"
}

Remove-OrphanQstVHidService
Ensure-RootDevice
$ErrorActionPreference = "Continue"
pnputil /scan-devices 2>&1 | Out-Host
$ErrorActionPreference = "Stop"

Assert-QstVHidReady

Write-Host ""
Write-Host "Done. QST Virtual HID is OK."
Write-Host ("Uninstall: powershell -File `"" + $MyInvocation.MyCommand.Path + "`" -Uninstall")
exit 0
