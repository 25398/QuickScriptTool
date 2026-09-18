# Copy extension\edge into a destination dir, WITHOUT the runtime bridge token file.
#
# Usage (repo root):
#   powershell -ExecutionPolicy Bypass -File tools\copy_extension_edge.ps1 -Source <dir> -Dest <dir>
#
# Why this script exists (do not go back to Copy-Item -Recurse / cmake -E copy_directory):
#   bridge_runtime.json is written by the host on every start
#   (src\window_mode\ext_bridge\ext_bridge_server.cpp -> ExtBridgeServer::WriteConfigFile)
#   into BOTH the packaged extension dir and the repo source dir extension\edge.
#   That writer uses WriteUtf8AclFile(), which hardens the file DACL. Any token the DACL
#   does not list - e.g. a restricted/sandboxed build account - gets "Access is denied"
#   even for a plain read. Consequences seen in practice:
#     - `Copy-Item -Recurse` and `cmake -E copy_directory` over extension\edge FAIL
#       ("Permission denied"), which breaks the Release POST_BUILD copy and packaging.
#     - The content is stale runtime state (local port + bridge token), never a release
#       artifact: the host rewrites it next to the installed extension on first run.
#   So every copy of extension\edge goes through here and skips it.
#
# NOTE: keep this file ASCII-only. It has no BOM, and Windows PowerShell 5.1 would parse
# non-ASCII text as GB2312 on this machine (see AGENTS.md).

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Dest,
    [string[]]$Exclude = @("bridge_runtime.json")
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Source)) {
    throw "FATAL: extension source dir not found: $Source"
}
if (-not (Test-Path -LiteralPath (Join-Path $Source "manifest.json"))) {
    throw "FATAL: $Source has no manifest.json - expected the extension\edge source dir"
}

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

$copied = 0
$skipped = @()
foreach ($item in (Get-ChildItem -LiteralPath $Source -Force)) {
    if ($Exclude -contains $item.Name) {
        $skipped += $item.Name
        continue
    }
    Copy-Item -LiteralPath $item.FullName -Destination $Dest -Recurse -Force
    $copied++
}

# Best effort: a previous run may have left runtime state in the destination.
# Failing to delete it must not fail the build, but it must never be shipped.
foreach ($name in $Exclude) {
    $stale = Join-Path $Dest $name
    if (Test-Path -LiteralPath $stale) {
        try {
            Remove-Item -LiteralPath $stale -Force -ErrorAction Stop
            Write-Host "  - removed stale runtime state from dest: $name"
        } catch {
            Write-Warning "  ! could not remove stale $name from $Dest ($($_.Exception.Message)); excluded from this copy, check the package"
        }
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $Dest "manifest.json"))) {
    throw "FATAL: $Dest\manifest.json missing after copy - extension package incomplete"
}

foreach ($name in $skipped) { Write-Host "  - skipped runtime state: $name" }
Write-Host "  + extension\edge -> $Dest ($copied entries copied)"
