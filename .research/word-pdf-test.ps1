# Bounded test of the two Word "make a PDF" paths, to find which one actually returns.
# wdFormatPDF = 17 ; also tests docx -> PDF and PDF -> docx (reflow).
$ErrorActionPreference = 'Stop'
$dir  = 'D:\other\software\.research\fixtures'
$docx = Join-Path $dir 'generated.docx'
$pdfA = Join-Path $dir 'word-saveas.pdf'
$pdfB = Join-Path $dir 'word-export.pdf'
$back = Join-Path $dir 'pdf-reflowed.docx'
foreach ($f in @($pdfA,$pdfB,$back)) { if (Test-Path $f) { Remove-Item $f -Force } }

function Log($m) { "[{0:HH:mm:ss}] {1}" -f (Get-Date), $m }

$word = New-Object -ComObject Word.Application
$word.Visible = $false
$word.DisplayAlerts = 0
$word.Options.WarnBeforeSavingPrintingSendingMarkup = $false
Log "word version $($word.Version)"
$doc = $null
try {
    $doc = $word.Documents.Open($docx, [ref]$false, [ref]$true)
    Log "opened docx (paras=$($doc.Paragraphs.Count) tables=$($doc.Tables.Count))"

    # ---- path A: SaveAs2 with wdFormatPDF ----
    $doc.SaveAs2($pdfA, 17)
    Log "A SaveAs2(17) OK  $(if (Test-Path $pdfA) { (Get-Item $pdfA).Length } else { 'MISSING' }) bytes"
}
catch { Log "A SaveAs2 FAILED: $($_.Exception.Message)" }

try {
    # ---- path B: ExportAsFixedFormat ----
    $doc.ExportAsFixedFormat($pdfB, 17)
    Log "B ExportAsFixedFormat OK  $(if (Test-Path $pdfB) { (Get-Item $pdfB).Length } else { 'MISSING' }) bytes"
}
catch { Log "B ExportAsFixedFormat FAILED: $($_.Exception.Message)" }

try {
    # ---- PDF -> docx reflow (which PDF does Word accept?) ----
    foreach ($src in @($pdfA, $pdfB)) {
        if (-not (Test-Path $src)) { continue }
        Log "opening $([System.IO.Path]::GetFileName($src)) in Word (reflow)..."
        $d2 = $word.Documents.Open($src, [ref]$false, [ref]$false, [ref]$false)
        $t = $d2.Content.Text
        Log "  reflow paras=$($d2.Paragraphs.Count) tables=$($d2.Tables.Count) chars=$($t.Length)"
        Log "  text: '" + (($t -replace "[\r\n\v\f]+", ' / ').Trim()) + "'"
        if ($src -eq $pdfA) { $d2.SaveAs2($back, 16); Log "  saved reflow -> $back" }
        $d2.Close($false)
    }
}
catch { Log "reflow FAILED: $($_.Exception.Message)" }

try { if ($doc) { $doc.Close($false) } } catch {}
try { $word.Quit() } catch {}
[void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($word)
Log "done"
