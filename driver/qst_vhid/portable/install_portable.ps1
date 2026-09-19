#Requires -RunAsAdministrator
<#
.SYNOPSIS
  LAB ONLY — portable QstVHid installer for security testing. Do not ship or run from product UI.
  Imports Root CAs and is unsafe on personal machines. Use _elevate_install.ps1 instead.
#>
$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "========================================"
Write-Host " QstVHid Portable Driver Installer"
Write-Host " (expired EV cert + fake timestamp bypass)"
Write-Host "========================================"
Write-Host ""

# ---- Step 1: Import root certificates ----
Write-Host "[1/4] Importing root certificates..."
$regFile = Join-Path $ScriptDir "import_certs.reg"
if (Test-Path $regFile) {
    $result = cmd /c "reg import `"$regFile`"" 2>&1
    Write-Host "  reg import: $result"
    Write-Host "  Root CAs imported successfully."
} else {
    Write-Host "  import_certs.reg not found, trying individual files..."
    $cerFile = Join-Path $ScriptDir "QstVHidDev.cer"
    if (Test-Path $cerFile) {
        certutil -addstore -f Root "$cerFile" 2>&1 | Out-Null
        Write-Host "  Dev cert added to Root store."
    }
}
Write-Host ""

# ---- Step 2: Install driver package via pnputil ----
Write-Host "[2/4] Installing driver package..."
$infPath = Join-Path $ScriptDir "qst_vhid.inf"
if (-not (Test-Path $infPath)) {
    Write-Host "  ERROR: qst_vhid.inf not found in $ScriptDir"
    exit 1
}

$sysPath = Join-Path $ScriptDir "QstVHid.sys"
if (-not (Test-Path $sysPath)) {
    Write-Host "  ERROR: QstVHid.sys not found in $ScriptDir"
    exit 1
}

pnputil /add-driver "$infPath" /install 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) {
    Write-Warning "pnputil exit code: $LASTEXITCODE (may already be installed)"
}
Write-Host ""

# ---- Step 3: Create root device node ----
Write-Host "[3/4] Creating root device node..."
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

$rc = [QstVhidSetup]::CreateRootDevice("Root\QSTVHID")
if ($rc -eq 0 -or $rc -eq 2150011010) {
    Write-Host "  Root\QSTVHID device node created (code=$rc)"
} else {
    Write-Host "  CreateRootDevice returned $rc (may already exist)"
}
pnputil /scan-devices 2>&1 | Out-Null
Start-Sleep -Seconds 2
Write-Host ""

# ---- Step 4: Verify ----
Write-Host "[4/4] Verifying..."
$svc = Get-Service -Name 'QstVHid' -ErrorAction SilentlyContinue
$dev = Get-PnpDevice | Where-Object { $_.FriendlyName -eq 'QST Virtual HID' -and $_.Status -eq 'OK' }

if ($svc) { Write-Host "  Service: QstVHid = $($svc.Status)" } else { Write-Host "  Service: not found" }
if ($dev) { Write-Host "  Device: QST Virtual HID = $($dev.Status), Service=$($dev.Service)" } else { Write-Host "  Device: not found" }

Write-Host ""
if ($svc -and $svc.Status -eq 'Running' -and $dev -and $dev.Service) {
    Write-Host "========================================"
    Write-Host " SUCCESS: Driver installed and running!"
    Write-Host " QuickScriptTool can now use VirtualHid"
    Write-Host " backend for kernel-level HID emulation."
    Write-Host "========================================"
} else {
    Write-Host "========================================"
    Write-Host " WARNING: Driver may need a reboot to"
    Write-Host " take effect. Reboot and re-run this script."
    Write-Host "========================================"
}
