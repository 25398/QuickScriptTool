#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Build (optional), sign, and install QstVHid.
.NOTES
  Security-lab path: prefer _elevate_install.ps1 + package\ + import_certs.reg (no WDK on target).
  Expired/missing Preferred PFX is NOT auto-replaced — use -ForceLocalDevPfx for local self-signed.
  -SignOnly: stage+sign into package\ then exit (sample construction; no install).
#>
param(
    [string]$PfxPath = "",
    [string]$PfxPassword = "Abc123456",
    [switch]$SkipBuild,
    [switch]$SkipResign,   # use package\ (or stage from bin) without re-signing; must already verify
    [switch]$ForceLocalDevPfx,  # if Preferred PFX missing OR expired, create/use certs\QstVHidDev.pfx
    [switch]$AllowExpiredPfx,   # keep expired Preferred PFX (default behavior now; switch retained for clarity)
    [switch]$SignOnly,          # sign package\ then exit; skip verify gate + pnputil install
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PfxPath) {
    $localPfx = Join-Path $Root "certs\Beijing_Changlang.pfx"
    $legacyPfx = "D:\other\EV\证书\Beijing_Changlang_Abc123456.pfx"
    if (Test-Path -LiteralPath $localPfx) { $PfxPath = $localPfx }
    elseif (Test-Path -LiteralPath $legacyPfx) { $PfxPath = $legacyPfx }
    else { $PfxPath = $localPfx }
}
$Cfg = "Release"
$OutDir = Join-Path $Root "bin\$Cfg"
$SysPath = Join-Path $OutDir "QstVHid.sys"
$InfPath = Join-Path $Root "qst_vhid.inf"
$PackageDir = Join-Path $Root "package"

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
        [bool]$SoftFail = $false
    )
    # Native stderr must not abort under $ErrorActionPreference=Stop
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Signtool sign /fd sha256 /f $Pfx /p $Password /tr http://timestamp.digicert.com /td sha256 $File 2>&1 | Out-Host
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
    foreach ($m in [regex]::Matches($block, '(?is)Published Name\s*:\s*(oem\d+\.inf).*?Original Name\s*:\s*qst_vhid\.inf')) {
        [void]$oemNames.Add($m.Groups[1].Value)
    }
    # Fallback: any block mentioning qst_vhid
    if ($oemNames.Count -eq 0) {
        $chunks = $block -split '(?=Published Name\s*:)'
        foreach ($c in $chunks) {
            if ($c -match 'qst_vhid\.inf' -and $c -match 'Published Name\s*:\s*(oem\d+\.inf)') {
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

# Stage package: INF + SYS side by side
if (Test-Path $PackageDir) { Remove-Item $PackageDir -Recurse -Force }
New-Item -ItemType Directory -Path $PackageDir | Out-Null
Copy-Item $SysPath (Join-Path $PackageDir 'QstVHid.sys')
Copy-Item $InfPath (Join-Path $PackageDir 'qst_vhid.inf')

Push-Location $PackageDir
$usedExpiredPfx = $false
try {
    if ($SkipResign) {
        if (-not (Test-SysSignature -File (Join-Path $PackageDir 'QstVHid.sys') -Signtool $signtool)) {
            Write-Warning 'SkipResign: package\QstVHid.sys does not pass signtool verify /pa — keeping as-is (lab sample / fake-timestamp path).'
        } else {
            Write-Host 'SkipResign: using existing signature on package\QstVHid.sys'
        }
        $ErrorActionPreference = "Continue"
        & $inf2cat /driver:. /os:10_X64 /verbose 2>&1 | Out-Host
        $ErrorActionPreference = "Stop"
        $cat = Get-ChildItem $PackageDir -Filter '*.cat' | Select-Object -First 1 -ExpandProperty FullName
        # Try to sign catalog with dev cert so pnputil accepts the package
        $devThumb = '274B9D2A0DE3FD0E86B68E78DFB482A702B5F91D'
        Write-Host "Signing catalog with dev cert for pnputil acceptance..."
        $ErrorActionPreference = "Continue"
        & $Signtool sign /fd sha256 /sha1 $devThumb /a $cat 2>&1 | Out-Host
        $ErrorActionPreference = "Stop"
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