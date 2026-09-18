# Builds a minimal valid single-page PDF with a text object, using only .NET.
# Purpose: an Office-independent fixture for PDF text-extraction / render tests.
param([string]$Path = 'D:\other\software\.research\fixtures\handmade.pdf')
$ErrorActionPreference = 'Stop'

$content = "BT /F1 24 Tf 72 700 Td (Hello from a hand-built PDF) Tj ET`n" +
           "BT /F1 14 Tf 72 660 Td (Second line: cities Beijing Shanghai) Tj ET`n"

$objs = @()
$objs += '<</Type/Catalog/Pages 2 0 R>>'
$objs += '<</Type/Pages/Kids[3 0 R]/Count 1>>'
$objs += '<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Resources<</Font<</F1 4 0 R>>>>/Contents 5 0 R>>'
$objs += '<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>'
$objs += "<</Length $([System.Text.Encoding]::ASCII.GetByteCount($content))>>`nstream`n$content" + "endstream"

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("%PDF-1.4`n")
$offsets = @()
for ($i = 0; $i -lt $objs.Count; $i++) {
    $offsets += $sb.Length
    [void]$sb.Append("$($i+1) 0 obj`n$($objs[$i])`nendobj`n")
}
$xrefPos = $sb.Length
[void]$sb.Append("xref`n0 $($objs.Count+1)`n")
[void]$sb.Append("0000000000 65535 f `n")
foreach ($o in $offsets) { [void]$sb.Append(("{0:D10} 00000 n `n" -f $o)) }
[void]$sb.Append("trailer`n<</Size $($objs.Count+1)/Root 1 0 R>>`nstartxref`n$xrefPos`n%%EOF`n")

[System.IO.File]::WriteAllText($Path, $sb.ToString(), [System.Text.Encoding]::ASCII)
"wrote $Path ($((Get-Item $Path).Length) bytes)"
