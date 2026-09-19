#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Recovery for machines that can still reach Windows / Safe Mode / WinRE
  after an old QuickScriptTool driver install.

.DESCRIPTION
  Removes Interception from keyboard/mouse class LowerFilters (the usual
  "cannot boot / no keyboard" cause), deletes leftover boot tasks, and
  undoes test-signing if a previous installer left a marker.

  Does not disable Secure Boot, Memory Integrity, or import certificates.
  Copy this file to a USB stick if the installed copy is unreachable.

  Safe Mode: Shift+Restart → Troubleshoot → Advanced → Startup Settings → 4
  WinRE PowerShell: also works if you can map the Windows volume.
#>
$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$elevate = Join-Path $here '_elevate_install.ps1'
if (Test-Path -LiteralPath $elevate) {
    & $elevate -Kind interception -RepairOnly
    exit $LASTEXITCODE
}

# Standalone fallback (script folder missing the product installer)
$kb = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e96b-e325-11ce-bfc1-08002be10318}'
$ms = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}'
function Test-IcFilterName([string]$name) {
    if (-not $name) { return $false }
    if ($name -ieq 'Interception') { return $true }
    $img = [string](Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$name" -ErrorAction SilentlyContinue).ImagePath
    return ($img -match '(?i)interception\.sys')
}
function Strip-Ic([string]$key) {
    if (-not (Test-Path $key)) { return }
    $p = Get-ItemProperty $key -Name LowerFilters -ErrorAction SilentlyContinue
    if (-not $p -or -not $p.LowerFilters) { return }
    $keep = @($p.LowerFilters | Where-Object { $_ -and -not (Test-IcFilterName $_) })
    if ($keep.Count -eq 0) { Remove-ItemProperty $key -Name LowerFilters -ErrorAction SilentlyContinue }
    else { Set-ItemProperty $key -Name LowerFilters -Value ([string[]]$keep) -Type MultiString }
}
Strip-Ic $kb
Strip-Ic $ms
foreach ($n in @('QstVHidFinishInstall', 'QstVHidWaitSb', 'QstVHidOpenApp')) {
    Unregister-ScheduledTask -TaskName $n -Confirm:$false -ErrorAction SilentlyContinue
}
$marker = 'HKLM:\SOFTWARE\QuickScriptTool\QstVHid'
if ((Get-ItemProperty $marker -Name SetTestSigning -ErrorAction SilentlyContinue).SetTestSigning -eq 1) {
    bcdedit.exe /deletevalue testsigning 2>&1 | Out-Null
}
Write-Host 'Repair finished. Reboot normally (do not enter firmware).'
exit 0
