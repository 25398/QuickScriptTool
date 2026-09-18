# Test Word's PDF -> docx "reflow" path (pure-Office PDF text route) on several PDFs.
# Runs each attempt in its own process with a hard timeout, because Word COM has been
# observed to hang indefinitely on some calls.
$ErrorActionPreference = 'Stop'
$dir = 'D:\other\software\.research\fixtures'
function Log($m) { "[{0:HH:mm:ss}] {1}" -f (Get-Date), $m }

$pdfs = @(
    (Join-Path $dir 'wp-saveas-OneNoteDesktop.pdf'),
    (Join-Path $dir 'handmade.pdf'),
    'C:\Users\冯思乾\Downloads\2021概率论与数理统计A(试题).pdf'
)

$worker = {
    param($pdf, $outDocx)
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    try {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        # ConfirmConversions=false; Word reflows the PDF into an editable document.
        $doc = $word.Documents.Open($pdf, [ref]$false, [ref]$false, [ref]$false)
        $sw.Stop()
        $open = [math]::Round($sw.Elapsed.TotalSeconds, 1)
        $t = $doc.Content.Text
        $res = "opened in ${open}s paras=$($doc.Paragraphs.Count) tables=$($doc.Tables.Count) chars=$($t.Length)"
        $snip = ($t -replace "[\r\n\v\f]+", ' / ').Trim()
        if ($snip.Length -gt 220) { $snip = $snip.Substring(0, 220) + '...' }
        $doc.SaveAs2($outDocx, 16)
        $doc.Close($false)
        "$res`n      SNIPPET: $snip"
    } catch { "ERROR: $($_.Exception.Message)" }
    finally {
        try { $word.Quit() } catch {}
        Get-Process WINWORD -ErrorAction SilentlyContinue | Stop-Process -Force
    }
}

foreach ($pdf in $pdfs) {
    if (-not (Test-Path $pdf)) { Log "MISSING $pdf"; continue }
    $name = [System.IO.Path]::GetFileNameWithoutExtension($pdf)
    $out  = Join-Path $dir "reflow-$name.docx"
    if (Test-Path $out) { Remove-Item $out -Force }
    Log "--- $([System.IO.Path]::GetFileName($pdf)) ($((Get-Item $pdf).Length)B) ---"
    $j = Start-Job -ScriptBlock $worker -ArgumentList $pdf, $out
    if ($j | Wait-Job -Timeout 90) {
        Log "  $(Receive-Job $j)"
        Log "  out docx: $(if (Test-Path $out) { "$((Get-Item $out).Length)B" } else { 'NOT WRITTEN' })"
    } else {
        Log "  *** TIMED OUT after 90s ***"
        Stop-Job $j -ErrorAction SilentlyContinue
        Get-Process WINWORD -ErrorAction SilentlyContinue | Stop-Process -Force
    }
    Remove-Job $j -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}
Log done
