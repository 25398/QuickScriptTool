# Verify PowerPoint opens the hand-built minimal .pptx, and test PPTX -> PDF export.
$ErrorActionPreference = 'Stop'
$dir = 'D:\other\software\.research\fixtures'
function Log($m) { "[{0:HH:mm:ss}] {1}" -f (Get-Date), $m }

$worker = {
    param($pptx, $pdf)
    $ppt = New-Object -ComObject PowerPoint.Application
    try {
        # PowerPoint COM historically REFUSES Presentations.Open with WithWindow:=msoFalse
        # for some operations; it also needs the app to be able to create a window.
        $pres = $ppt.Presentations.Open($pptx, [Microsoft.Office.Core.MsoTriState]::msoTrue,
                                               [Microsoft.Office.Core.MsoTriState]::msoFalse,
                                               [Microsoft.Office.Core.MsoTriState]::msoFalse)
        $out = "slides=$($pres.Slides.Count)"
        foreach ($s in $pres.Slides) {
            $txt = @()
            foreach ($sh in $s.Shapes) {
                if ($sh.HasTextFrame -eq -1 -and $sh.TextFrame.HasText -eq -1) { $txt += $sh.TextFrame.TextRange.Text }
            }
            $out += " | slide$($s.SlideIndex): " + (($txt -join ' / ') -replace "[\r\n\v]+", ' ')
        }
        $pres.SaveAs($pdf, 32)   # ppSaveAsPDF
        $pres.Close()
        "$out`n      PDF: $(if (Test-Path $pdf) { "$((Get-Item $pdf).Length)B" } else { 'NOT WRITTEN' })"
    } catch { "ERROR: $($_.Exception.Message)" }
    finally {
        try { $ppt.Quit() } catch {}
        Get-Process POWERPNT -ErrorAction SilentlyContinue | Stop-Process -Force
    }
}

$pptx = Join-Path $dir 'generated.pptx'
$pdf  = Join-Path $dir 'generated-slides.pdf'
if (Test-Path $pdf) { Remove-Item $pdf -Force }
Log "opening $pptx ($((Get-Item $pptx).Length)B) in PowerPoint..."
$j = Start-Job -ScriptBlock $worker -ArgumentList $pptx, $pdf
if ($j | Wait-Job -Timeout 120) { Log "$(Receive-Job $j)" }
else {
    Log "*** TIMED OUT after 120s ***"
    Stop-Job $j -ErrorAction SilentlyContinue
    Get-Process POWERPNT -ErrorAction SilentlyContinue | Stop-Process -Force
}
Remove-Job $j -Force -ErrorAction SilentlyContinue
Log done
