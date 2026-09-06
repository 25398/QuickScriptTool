<#
.SYNOPSIS
  Product driver installer for QstVHid and Interception.

.DESCRIPTION
  Safe product path (settings buttons call this):
    - Never changes boot policy (no BCD, no firmware reboot).
    - Never disables Memory Integrity.
    - Never imports fake/expired CAs into the machine Root store.
    - Never deletes driver-store repository folders.
    - Interception class filters (keyboard/mouse LowerFilters) are written
      ONLY after the kernel service is confirmed Running in this session.
      If the .sys is rejected by CI, files/service are rolled back and
      LowerFilters are left untouched (or leftover orphan filters are removed).

  Exit codes:
    0  = installed and running (or uninstall/repair done)
    2  = package applied but device not ready (VHID); no boot-critical filters
    3  = signature not trusted by this Windows; refused, system unchanged
    5  = kernel service failed to start; rolled back; no class filters written
    90 = not elevated
    99 = missing package files
#>
param(
    [ValidateSet('vhid', 'interception')]
    [string]$Kind = 'vhid',
    [switch]$Uninstall,
    [switch]$RepairOnly
)

$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'install_log.txt'
$scriptVersion = '2026-08-19.1'

# Class GUIDs: Keyboard {4d36e96b-...}  Mouse {4d36e97b-...}
$script:KeyboardClassKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e96b-e325-11ce-bfc1-08002be10318}'
$script:MouseClassKey    = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}'
$script:IcSysDst         = 'C:\Windows\System32\drivers\interception.sys'
$script:MarkerKey        = 'HKLM:\SOFTWARE\QuickScriptTool\QstVHid'

Remove-Item $log -ErrorAction SilentlyContinue

function Write-Log($msg) {
    Write-Host $msg
    Add-Content -Path $log -Value $msg -Encoding UTF8
}

function Exit-Logged([int]$Code) {
    Add-Content -Path $log -Value "EXIT_CODE=$Code" -Encoding UTF8 -ErrorAction SilentlyContinue
    exit $Code
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Add-Content -Path $log -Value "ERROR: not elevated (script=$scriptVersion)" -Encoding UTF8 -ErrorAction SilentlyContinue
    Add-Content -Path $log -Value 'EXIT_CODE=90' -Encoding UTF8 -ErrorAction SilentlyContinue
    exit 90
}
Write-Log "Admin check OK (script=$scriptVersion kind=$Kind uninstall=$Uninstall repairOnly=$RepairOnly)"

function Unregister-LeftoverBootTasks {
    foreach ($n in @('QstVHidFinishInstall', 'QstVHidWaitSb', 'QstVHidOpenApp')) {
        Unregister-ScheduledTask -TaskName $n -Confirm:$false -ErrorAction SilentlyContinue
        & schtasks.exe /delete /tn $n /f 2>&1 | Out-Null
    }
    Write-Log '  Cleared leftover boot/logon tasks from old installer (if any).'
}

function Undo-OldTestSigningMarker {
    if (-not (Test-Path -LiteralPath $script:MarkerKey)) { return }
    $setByUs = (Get-ItemProperty $script:MarkerKey -Name 'SetTestSigning' -ErrorAction SilentlyContinue).SetTestSigning
    if ($setByUs -ne 1) { return }
    Write-Log '  Old installer had flipped boot signing; restoring default (off).'
    # Recovery only: undo a previous product change. Never turn signing bypass ON.
    & bcdedit.exe /deletevalue testsigning 2>&1 | Out-Null
    Remove-ItemProperty -Path $script:MarkerKey -Name 'SetTestSigning' -ErrorAction SilentlyContinue
}

function Get-ServiceImagePath([string]$Name) {
    $key = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name"
    if (-not (Test-Path -LiteralPath $key)) { return '' }
    return [string](Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue).ImagePath
}

function Test-IsInterceptionFilterName([string]$Name) {
    if (-not $Name) { return $false }
    if ($Name -ieq 'Interception') { return $true }
    $img = Get-ServiceImagePath $Name
    return ($img -match '(?i)interception\.sys')
}

function Remove-InterceptionClassFilters {
    foreach ($classKey in @($script:KeyboardClassKey, $script:MouseClassKey)) {
        if (-not (Test-Path -LiteralPath $classKey)) { continue }
        $prop = Get-ItemProperty -LiteralPath $classKey -Name LowerFilters -ErrorAction SilentlyContinue
        if (-not $prop -or -not $prop.LowerFilters) { continue }
        $keep = @($prop.LowerFilters | Where-Object { $_ -and -not (Test-IsInterceptionFilterName $_) })
        if ($keep.Count -eq 0) {
            Remove-ItemProperty -LiteralPath $classKey -Name LowerFilters -ErrorAction SilentlyContinue
            Write-Log "  Removed LowerFilters on $classKey"
        } elseif ($keep.Count -ne @($prop.LowerFilters).Count) {
            Set-ItemProperty -LiteralPath $classKey -Name LowerFilters -Value ([string[]]$keep) -Type MultiString
            Write-Log "  Stripped Interception from LowerFilters on $classKey"
        }
    }
}

function Repair-OrphanClassFilters {
    # Only strip a class-filter name when that service is not Running.
    # A stopped-but-startable driver is started once; if CI rejects it, drop the filter
    # so the next boot still has keyboard/mouse.
    foreach ($classKey in @($script:KeyboardClassKey, $script:MouseClassKey)) {
        if (-not (Test-Path -LiteralPath $classKey)) { continue }
        $prop = Get-ItemProperty -LiteralPath $classKey -Name LowerFilters -ErrorAction SilentlyContinue
        if (-not $prop -or -not $prop.LowerFilters) { continue }
        $keep = @()
        foreach ($name in @($prop.LowerFilters)) {
            if (-not $name) { continue }
            if (-not (Test-IsInterceptionFilterName $name)) {
                $keep += $name
                continue
            }
            $svc = Get-Service -Name $name -ErrorAction SilentlyContinue
            if (-not $svc -or $svc.Status -ne 'Running') {
                sc.exe start $name 2>&1 | Out-Null
                Start-Sleep -Milliseconds 800
                $svc = Get-Service -Name $name -ErrorAction SilentlyContinue
            }
            if ($svc -and $svc.Status -eq 'Running') {
                $keep += $name
            } else {
                Write-Log "  Dropping orphan class filter '$name' (service not running; would block boot)"
            }
        }
        if ($keep.Count -eq 0) {
            Remove-ItemProperty -LiteralPath $classKey -Name LowerFilters -ErrorAction SilentlyContinue
        } elseif ($keep.Count -ne @($prop.LowerFilters).Count) {
            Set-ItemProperty -LiteralPath $classKey -Name LowerFilters -Value ([string[]]$keep) -Type MultiString
        }
    }
}

function Repair-LeftoverBootChanges {
    Unregister-LeftoverBootTasks
    Undo-OldTestSigningMarker
    Repair-OrphanClassFilters
}

function Get-InterceptionServiceRunning {
    $svc = Get-Service -Name 'Interception' -ErrorAction SilentlyContinue
    return ($svc -and $svc.Status -eq 'Running')
}

function Undo-InterceptionFiles {
    sc.exe stop Interception 2>&1 | Out-Null
    Start-Sleep -Milliseconds 400
    sc.exe delete Interception 2>&1 | Out-Null
    if (Test-Path -LiteralPath $script:IcSysDst) {
        Remove-Item -LiteralPath $script:IcSysDst -Force -ErrorAction SilentlyContinue
    }
}

function Add-InterceptionClassFilters {
    foreach ($classKey in @($script:KeyboardClassKey, $script:MouseClassKey)) {
        if (-not (Test-Path -LiteralPath $classKey)) { continue }
        $cur = @()
        $prop = Get-ItemProperty -LiteralPath $classKey -Name LowerFilters -ErrorAction SilentlyContinue
        if ($prop -and $prop.LowerFilters) {
            $cur = @($prop.LowerFilters | Where-Object { $_ -and -not (Test-IsInterceptionFilterName $_) })
        }
        $cur += 'Interception'
        Set-ItemProperty -LiteralPath $classKey -Name LowerFilters -Value ([string[]]$cur) -Type MultiString
        Write-Log "  LowerFilters += Interception on $classKey"
    }
}

function Uninstall-Interception {
    Write-Log 'Uninstall Interception: drop class filters FIRST, then service/file.'
    Remove-InterceptionClassFilters
    Undo-InterceptionFiles
    Write-Log 'Interception removed. Keyboard/mouse class filters restored.'
}

function Install-Interception {
    $sysSrc = Join-Path $PSScriptRoot 'package\interception.sys'
    if (-not (Test-Path -LiteralPath $sysSrc)) {
        Write-Log 'ERROR: missing package\interception.sys'
        Exit-Logged 99
    }

    if (Get-InterceptionServiceRunning) {
        Write-Log 'Interception already Running. Nothing to do.'
        return
    }

    Write-Log 'Staging interception.sys (service only; no class filters yet)...'
    sc.exe stop Interception 2>&1 | Out-Null
    sc.exe delete Interception 2>&1 | Out-Null
    Copy-Item -LiteralPath $sysSrc -Destination $script:IcSysDst -Force
    if (-not (Test-Path -LiteralPath $script:IcSysDst)) {
        Write-Log 'ERROR: failed to write interception.sys'
        Exit-Logged 99
    }

    sc.exe create Interception type= kernel start= demand error= normal `
        binPath= '\SystemRoot\System32\drivers\interception.sys' `
        DisplayName= 'Interception Driver' 2>&1 | Out-Null
    sc.exe description Interception 'Optional keyboard/mouse filter (demand start until verified)' 2>&1 | Out-Null

    Write-Log 'Starting Interception service (CI gate — must succeed before filters)...'
    sc.exe start Interception 2>&1 | Out-Null
    Start-Sleep -Seconds 2

    # FILTERS_ONLY_AFTER_RUNNING
    if (-not (Get-InterceptionServiceRunning)) {
        Write-Log 'REFUSED_FILTERS: driver did not start (signature/CI). Rolling back; class filters unchanged.'
        Undo-InterceptionFiles
        Exit-Logged 5
    }

    sc.exe config Interception start= system 2>&1 | Out-Null
    Add-InterceptionClassFilters
    Write-Log 'SUCCESS: Interception running; class filters registered.'
}

function Add-TypeRootDeviceHelper {
    if ('QstVhidSetup' -as [type]) { return }
    Add-Type -TypeDefinition @'
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
}

function Get-VhidReady {
    $svc = Get-Service -Name 'QstVHid' -ErrorAction SilentlyContinue
    $dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
        $_.FriendlyName -eq 'QST Virtual HID' -and $_.Status -eq 'OK' -and $_.Service
    } | Select-Object -First 1
    return ($svc -and $svc.Status -eq 'Running' -and $dev)
}

function Remove-VhidDevicesAndPackage {
    Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
        $_.FriendlyName -eq 'QST Virtual HID' -or $_.InstanceId -like 'ROOT\QSTVHID*'
    } | ForEach-Object {
        Write-Log "  Removing device $($_.InstanceId)"
        pnputil /remove-device $_.InstanceId 2>&1 | Out-Null
    }
    sc.exe stop QstVHid 2>&1 | Out-Null
    sc.exe delete QstVHid 2>&1 | Out-Null

    $enum = pnputil /enum-drivers 2>&1 | Out-String
    $nameLabel = '(?:Published Name|发布名称)'
    $pubNames = @()
    foreach ($block in ($enum -split "(?=$nameLabel)")) {
        if ($block -notmatch '(?im)qst_vhid') { continue }
        if ($block -match "(?im)$nameLabel\s*:\s*(\S+)") { $pubNames += $Matches[1] }
    }
    foreach ($pn in ($pubNames | Select-Object -Unique)) {
        Write-Log "  pnputil /delete-driver $pn"
        pnputil /delete-driver $pn /uninstall 2>&1 | Out-Null
    }
}

function Uninstall-Vhid {
    Write-Log 'Uninstall QstVHid via pnputil (no DriverStore folder wipe)...'
    Remove-VhidDevicesAndPackage
    Write-Log 'QstVHid removed.'
}

function Test-UserWantsUnsignedAnyway {
    # Read-only probe. We never enable this ourselves.
    $out = & bcdedit.exe /enum '{current}' 2>&1 | Out-String
    return ($out -match '(?im)^\s*testsigning\s+Yes')
}

function Install-Vhid {
    $pkgDir = Join-Path $PSScriptRoot 'package'
    $infPath = Join-Path $pkgDir 'qst_vhid.inf'
    $sysPath = Join-Path $pkgDir 'QstVHid.sys'
    $catPath = Join-Path $pkgDir 'qst_vhid.cat'
    foreach ($f in @($infPath, $sysPath, $catPath)) {
        if (-not (Test-Path -LiteralPath $f)) {
            Write-Log "ERROR: missing $f"
            Exit-Logged 99
        }
    }

    if (Get-VhidReady) {
        Write-Log 'QstVHid already RUNNING. Nothing to do.'
        return
    }

    $catSig = Get-AuthenticodeSignature -FilePath $catPath
    $trusted = ($catSig.Status -eq 'Valid')
    $unsignedOk = Test-UserWantsUnsignedAnyway
    if (-not $trusted -and -not $unsignedOk) {
        Write-Log "ERROR: catalog not trusted (status=$($catSig.Status)). Refusing to weaken boot policy."
        Exit-Logged 3
    }
    if ($trusted) {
        Write-Log "Catalog signature VALID — $($catSig.SignerCertificate.Subject)"
    } else {
        Write-Log 'Catalog not trusted, but this machine already has test signing ON (user-configured). Continuing.'
    }

    Write-Log 'Adding driver package (software device only; not a class filter)...'
    pnputil /add-driver "$infPath" /install 2>&1 | Out-Null
    Write-Log "  pnputil /add-driver exit=$LASTEXITCODE"

    Add-TypeRootDeviceHelper
    $rc = [QstVhidSetup]::CreateRootDevice('Root\QSTVHID')
    Write-Log "  CreateRootDevice code=$rc"
    pnputil /scan-devices 2>&1 | Out-Null
    Start-Sleep -Seconds 3

    if (Get-VhidReady) {
        Write-Log 'SUCCESS: QstVHid installed and running.'
        return
    }
    Write-Log 'WARNING: package staged but device not Running/OK (kernel likely rejected the image).'
    Write-Log 'Leaving the software device; it is not a keyboard/mouse class filter and will not block boot.'
    Exit-Logged 2
}

# ---- always heal leftovers from the old installer, then dispatch ----
Repair-LeftoverBootChanges

if ($RepairOnly) {
    Write-Log 'Repair-only complete (tasks, orphan filters, old signing marker).'
    Exit-Logged 0
}

if ($Uninstall) {
    if ($Kind -eq 'vhid') { Uninstall-Vhid } else { Uninstall-Interception }
    Repair-LeftoverBootChanges
    Write-Log 'UNINSTALL done.'
    Exit-Logged 0
}

if ($Kind -eq 'vhid') { Install-Vhid } else { Install-Interception }
Write-Log 'DONE.'
Exit-Logged 0
