$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'install_log.txt'

Remove-Item $log -ErrorAction SilentlyContinue

function Write-Log($msg) {
  Write-Host $msg
  Add-Content -Path $log -Value $msg
}

# ---- Step 1: Quick check — already installed? ----
$svc = Get-Service -Name 'QstVHid' -ErrorAction SilentlyContinue
$dev = Get-PnpDevice | Where-Object { $_.FriendlyName -eq 'QST Virtual HID' -and $_.Status -eq 'OK' }
if ($svc -and $svc.Status -eq 'Running' -and $dev -and $dev.Service) {
  Write-Log "QstVHid already RUNNING and bound to device. Nothing to do."
  Add-Content -Path $log -Value 'EXIT_CODE=0'
  exit 0
}

# ---- Step 2: Import root certificates ----
Write-Log "Importing root certificates..."
$regFile = Join-Path $PSScriptRoot 'import_certs.reg'
if (Test-Path $regFile) {
  reg import "$regFile" 2>&1 | Out-Null
  Write-Log "  Root certificates imported."
} else {
  Write-Log "  WARNING: import_certs.reg not found, trying .cer fallback..."
  $cerFile = Join-Path $PSScriptRoot 'QstVHidDev.cer'
  if (Test-Path $cerFile) {
    certutil -addstore -f Root "$cerFile" 2>&1 | Out-Null
    Write-Log "  Dev cert imported from .cer"
  }
}

# ---- Step 3: Check package directory ----
$pkgDir = Join-Path $PSScriptRoot 'package'
$infPath = Join-Path $pkgDir 'qst_vhid.inf'
$sysPath = Join-Path $pkgDir 'QstVHid.sys'
$catPath = Join-Path $pkgDir 'qst_vhid.cat'

$pkgReady = $true
if (-not (Test-Path $infPath)) {
  Write-Log "  ERROR: Missing qst_vhid.inf in package/"
  $pkgReady = $false
}
if (-not (Test-Path $sysPath)) {
  Write-Log "  ERROR: Missing QstVHid.sys in package/"
  $pkgReady = $false
}
if (-not (Test-Path $catPath)) {
  Write-Log "  ERROR: Missing qst_vhid.cat in package/"
  $pkgReady = $false
}
if (-not $pkgReady) {
  Add-Content -Path $log -Value 'EXIT_CODE=99'
  exit 99
}
Write-Log "Driver package files present."

# ---- Step 4: Clean stale state ----
Write-Log "Cleaning stale services/devices..."
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'

# Stop + delete legacy sc service
sc.exe stop QstVHid 2>&1 | Out-Null
sc.exe delete QstVHid 2>&1 | Out-Null

# Remove ALL root nodes (FriendlyName or InstanceId), including unbound 0001 leftovers
Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
  $_.FriendlyName -eq 'QST Virtual HID' -or $_.InstanceId -like 'ROOT\QSTVHID*'
} | ForEach-Object {
  Write-Log "  Removing device $($_.InstanceId) Status=$($_.Status)"
  pnputil /remove-device $_.InstanceId 2>&1 | Out-Null
}

Start-Sleep -Seconds 1

# Remove old DriverStore packages
$pubNames = @()
$enum = pnputil /enum-drivers 2>&1 | Out-String
foreach ($block in ($enum -split '(?=Published Name)')) {
  if ($block -notmatch '(?im)qst_vhid') { continue }
  if ($block -match '(?im)Published Name\s*:\s*(\S+)') {
    $pubNames += $Matches[1]
  }
}
foreach ($pn in ($pubNames | Select-Object -Unique)) {
  Write-Log "  Removing old driver store: $pn"
  pnputil /delete-driver $pn /uninstall /force 2>&1 | Out-Null
}

$ErrorActionPreference = $prev
Start-Sleep -Seconds 2

# ---- Step 5: Install driver ----
Write-Log "Installing driver package..."
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
pnputil /add-driver "$infPath" /install 2>&1 | Out-Null
$pnpRc = $LASTEXITCODE
$ErrorActionPreference = $prev
Write-Log "  pnputil /add-driver exit=$pnpRc"

# ---- Step 6: Create root device node ----
Write-Log "Creating root device node..."
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
    Write-Log "  Root device node created (code=$rc)"
} else {
    Write-Log "  CreateRootDevice returned $rc (may already exist)"
}

pnputil /scan-devices 2>&1 | Out-Null
Start-Sleep -Seconds 3

# Drop unbound duplicate root nodes (keep one OK+Service)
$bound = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
  ($_.FriendlyName -eq 'QST Virtual HID' -or $_.InstanceId -like 'ROOT\QSTVHID*') -and
  $_.Status -eq 'OK' -and $_.Service
} | Select-Object -First 1
Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
  ($_.FriendlyName -eq 'QST Virtual HID' -or $_.InstanceId -like 'ROOT\QSTVHID*') -and
  (-not $bound -or $_.InstanceId -ne $bound.InstanceId) -and
  (-not $_.Service -or $_.Status -ne 'OK')
} | ForEach-Object {
  Write-Log "  Pruning leftover node $($_.InstanceId)"
  pnputil /remove-device $_.InstanceId 2>&1 | Out-Null
}

# ---- Step 7: Verify ----
$svcFinal = Get-Service -Name 'QstVHid' -ErrorAction SilentlyContinue
$devFinal = Get-PnpDevice | Where-Object {
  $_.FriendlyName -eq 'QST Virtual HID' -and $_.Status -eq 'OK' -and $_.Service
} | Select-Object -First 1

Write-Log "--- Result ---"
if ($svcFinal) { Write-Log "  Service: $($svcFinal.Status)" } else { Write-Log "  Service: not found" }
if ($devFinal) {
  Write-Log "  Device: $($devFinal.Status), Service=$($devFinal.Service), Id=$($devFinal.InstanceId)"
} else {
  Write-Log "  Device: not found (OK + bound)"
}
$extra = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
  $_.InstanceId -like 'ROOT\QSTVHID*' -and (-not $devFinal -or $_.InstanceId -ne $devFinal.InstanceId)
})
if ($extra.Count -gt 0) {
  Write-Log "  NOTE: $($extra.Count) extra ROOT\QSTVHID node(s) remain after prune"
}

if ($svcFinal -and $svcFinal.Status -eq 'Running' -and $devFinal) {
  Write-Log "SUCCESS: Driver installed and running."
  Add-Content -Path $log -Value 'EXIT_CODE=0'
  exit 0
} else {
  Write-Log "WARNING: Driver package/certs installed but device not Running/OK yet."
  Write-Log "  Reboot once, then re-run this script or click Install again in Settings."
  # Non-zero so Settings UI does not claim success when ProbeAvailable still fails.
  Add-Content -Path $log -Value 'EXIT_CODE=2'
  exit 2
}
