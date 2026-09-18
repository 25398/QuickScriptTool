# Windows.Data.Pdf (WinRT) -> render PDF pages to PNG from PowerShell 5.1.
# No Office, no Python, no third-party DLLs.
param(
    [Parameter(Mandatory=$true)][string]$PdfPath,
    [string]$OutDir = 'D:\other\software\.research\fixtures\pages',
    [int]$Width = 1240,          # target pixel width per page
    [int]$MaxPages = 0           # 0 = all
)
$ErrorActionPreference = 'Stop'

# ---- 1. the WinRT async bridge (required in PS 5.1) ----
Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
$asTaskAction = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' })[0]

function Await($op, $type) {
    $m = $asTaskGeneric.MakeGenericMethod($type)
    $t = $m.Invoke($null, @($op))
    $t.Wait(-1) | Out-Null
    $t.Result
}
function AwaitAction($op) {
    $t = $asTaskAction.Invoke($null, @($op))
    $t.Wait(-1) | Out-Null
}

# ---- 2. load the WinRT type projections BEFORE using them ----
[Windows.Storage.StorageFile,      Windows.Storage,      ContentType=WindowsRuntime] | Out-Null
[Windows.Data.Pdf.PdfDocument,     Windows.Data.Pdf,     ContentType=WindowsRuntime] | Out-Null
[Windows.Data.Pdf.PdfPageRenderOptions, Windows.Data.Pdf, ContentType=WindowsRuntime] | Out-Null
[Windows.Storage.Streams.InMemoryRandomAccessStream, Windows.Storage.Streams, ContentType=WindowsRuntime] | Out-Null

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$full = (Resolve-Path $PdfPath).Path

$sf  = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($full)) ([Windows.Storage.StorageFile])
$pdf = Await ([Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($sf))    ([Windows.Data.Pdf.PdfDocument])
"pages: $($pdf.PageCount)  (IsPasswordProtected=$($pdf.IsPasswordProtected))"

$n = $pdf.PageCount
if ($MaxPages -gt 0 -and $MaxPages -lt $n) { $n = $MaxPages }

for ($i = 0; $i -lt $n; $i++) {
    $page = $pdf.GetPage($i)
    try {
        $stream = New-Object Windows.Storage.Streams.InMemoryRandomAccessStream
        try {
            $opts = New-Object Windows.Data.Pdf.PdfPageRenderOptions
            $opts.DestinationWidth = $Width
            AwaitAction ($page.RenderToStreamAsync($stream, $opts))
            $stream.Seek(0)
            $net = [System.IO.WindowsRuntimeStreamExtensions]::AsStreamForRead($stream)
            $out = Join-Path $OutDir ("page-{0:D3}.png" -f ($i + 1))
            $fs = [System.IO.File]::Create($out)
            try { $net.CopyTo($fs) } finally { $fs.Dispose() }
            "page $($i+1): $out ($((Get-Item $out).Length) bytes)  size=$($page.Size.Width)x$($page.Size.Height)"
        } finally { $stream.Dispose() }
    } finally { $page.Dispose() }
}
"RENDER OK"
