#Requires -RunAsAdministrator
<#
  LAB ONLY — do not run from the product settings UI.
  This script is a signing-bypass experiment. The product installer
  (_elevate_install.ps1) never uses this path.
#>
$ErrorActionPreference = "Stop"

$DriverPath = "D:\other\software\driver\qst_vhid\QstVHid_signed.sys"
if (-not (Test-Path $DriverPath)) {
    Write-Host "ERROR: $DriverPath not found. Run final.py first."
    exit 1
}

Write-Host "=== Step 1: Stop and remove old QstVHid service ==="
cmd /c "sc stop QstVHid 2>nul"
Start-Sleep -Seconds 1
cmd /c "sc delete QstVHid 2>nul"
Write-Host "Old service cleaned."

Write-Host ""
Write-Host "=== Step 2: Create kernel driver service ==="
$scResult = cmd /c "sc create QstVHid type=kernel binPath=`"$DriverPath`" 2>&1"
Write-Host $scResult
Start-Sleep -Seconds 1

Write-Host "=== Step 3: Start the driver ==="
$startResult = cmd /c "sc start QstVHid 2>&1"
Write-Host $startResult

if ($startResult -match "RUNNING") {
    Write-Host ""
    Write-Host "[SUCCESS] Driver is RUNNING! Fake signature bypass confirmed."
    Write-Host "Driver path: $DriverPath"
    Write-Host ""
    Write-Host "=== Removing old root devices ==="
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $oldDevs = Get-PnpDevice | Where-Object { $_.InstanceId -match "QSTVHID" -or $_.FriendlyName -match "QST Virtual HID" }
    foreach ($d in $oldDevs) {
        pnputil /remove-device $d.InstanceId 2>&1 | Out-Null
    }
    Start-Sleep -Seconds 2
    $ErrorActionPreference = $prev

    Write-Host "=== Creating root device node ==="
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
    Write-Host "CreateRootDevice: $rc (0=success)"

    pnputil /scan-devices 2>&1 | Out-Null
    Start-Sleep -Seconds 2

    Write-Host ""
    Write-Host "=== Device Status ==="
    Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -match "QST" -or $_.InstanceId -match "QST" } | Select-Object FriendlyName, Status, Problem, InstanceId | Format-Table -AutoSize

    Write-Host ""
    Write-Host "=== Driver Service Status ==="
    cmd /c "sc query QstVHid"

    Write-Host ""
    Write-Host "=== To test: open QuickScriptTool, click install HID ==="
    Write-Host "=== To uninstall: sc stop QstVHid; sc delete QstVHid ==="
} else {
    Write-Host ""
    Write-Host "[FAILED] Driver did not start."
    Write-Host "Output: $startResult"
}
