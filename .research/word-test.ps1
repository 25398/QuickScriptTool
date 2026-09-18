# ASCII-only on purpose: PS 5.1 on a gb2312 box misparses UTF-8-no-BOM non-ASCII .ps1 files.
# Verifies: (1) Word opens our hand-built docx, (2) docx -> PDF via ExportAsFixedFormat,
# (3) PDF -> docx via Word's PDF reflow (the pure-Office PDF text route).
$ErrorActionPreference = 'Stop'
$dir  = 'D:\other\software\.research\fixtures'
$docx = Join-Path $dir 'generated.docx'
$pdf  = Join-Path $dir 'from-word.pdf'
$back = Join-Path $dir 'pdf-reflowed.docx'

foreach ($f in @($pdf, $back)) { if (Test-Path $f) { Remove-Item $f -Force } }

$word = New-Object -ComObject Word.Application
$word.Visible = $false
$word.DisplayAlerts = 0
$doc = $null
try {
    # ---- 1. open our hand-built docx ----
    $doc = $word.Documents.Open($docx, [ref]$false, [ref]$true)   # ConfirmConversions=false, ReadOnly=true
    "OPEN OK   paragraphs=$($doc.Paragraphs.Count) tables=$($doc.Tables.Count) words=$($doc.Words.Count) pages=$($doc.ComputeStatistics(2))"
    $i = 0
    foreach ($p in $doc.Paragraphs) {
        $i++
        if ($i -le 5) { "  p${i}: style='$($p.Style.NameLocal)' text='$($p.Range.Text.Trim())'" }
    }
    if ($doc.Tables.Count -ge 1) {
        $t = $doc.Tables.Item(1)
        "  table1 rows=$($t.Rows.Count) cols=$($t.Columns.Count) cell(2,1)='$($t.Cell(2,1).Range.Text.Trim())'"
    }

    # ---- 2. docx -> PDF (ExportAsFixedFormat, 17 = wdExportFormatPDF) ----
    $doc.ExportAsFixedFormat($pdf, 17)
    "PDF OK    $pdf $((Get-Item $pdf).Length) bytes"

    # ---- 3. PDF -> docx (Word reflows the PDF into an editable document) ----
    $doc2 = $word.Documents.Open($pdf, [ref]$false, [ref]$false, [ref]$false)
    "PDF OPEN  paragraphs=$($doc2.Paragraphs.Count) tables=$($doc2.Tables.Count) pages=$($doc2.ComputeStatistics(2))"
    $txt = $doc2.Content.Text
    "PDF TEXT  length=$($txt.Length)"
    "PDF SNIP  '" + ($txt.Substring(0, [Math]::Min(160, $txt.Length)) -replace "[\r\n\v\f]+", ' / ') + "'"
    $doc2.SaveAs2($back, 16)   # 16 = wdFormatDocumentDefault (.docx)
    "REFLOW OK $back $((Get-Item $back).Length) bytes"
    $doc2.Close($false)
}
catch { "WORD ERROR: $($_.Exception.Message)" }
finally {
    if ($doc) { try { $doc.Close($false) } catch {} }
    try { $word.Quit() } catch {}
    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($word)
    Start-Sleep -Seconds 1
    Get-Process WINWORD -ErrorAction SilentlyContinue | Stop-Process -Force
}
"done"
