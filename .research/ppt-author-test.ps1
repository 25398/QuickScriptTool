# Author a real .pptx with PowerPoint COM, list its OOXML parts, then export to PDF.
# Purpose: (a) prove PowerPoint COM save works, (b) show the real minimal-ish part set
# (my hand-built 6-part pptx was REJECTED by PowerPoint with 0x80070570).
$ErrorActionPreference = 'Stop'
$dir = 'D:\other\software\.research\fixtures'
function Log($m) { "[{0:HH:mm:ss}] {1}" -f (Get-Date), $m }

$worker = {
    param($pptx, $pdf)
    $ppt = New-Object -ComObject PowerPoint.Application
    try {
        # Add() with msoFalse = no visible window
        $pres = $ppt.Presentations.Add(0)
        $s1 = $pres.Slides.Add(1, 12)   # ppLayoutBlank
        $null = $s1.Shapes.AddTextbox(1, 50, 50, 600, 80)
        $s1.Shapes.Item($s1.Shapes.Count).TextFrame.TextRange.Text = 'Slide One Title'
        $s2 = $pres.Slides.Add(2, 12)
        $null = $s2.Shapes.AddTextbox(1, 50, 50, 600, 80)
        $s2.Shapes.Item($s2.Shapes.Count).TextFrame.TextRange.Text = "Bullet A`rBullet B"
        $s2.NotesPage.Shapes.Item(2).TextFrame.TextRange.Text = 'speaker note here'
        $pres.SaveAs($pptx, 24)   # ppSaveAsOpenXMLPresentation
        $n = $pres.Slides.Count
        $pres.SaveAs($pdf, 32)    # ppSaveAsPDF
        $pres.Close()
        "made pptx slides=$n ; pdf=$(if (Test-Path $pdf) { "$((Get-Item $pdf).Length)B" } else { 'NOT WRITTEN' })"
    } catch { "ERROR: $($_.Exception.Message)" }
    finally {
        try { $ppt.Quit() } catch {}
        Get-Process POWERPNT -ErrorAction SilentlyContinue | Stop-Process -Force
    }
}

$pptx = Join-Path $dir 'ppt-authored.pptx'
$pdf  = Join-Path $dir 'ppt-authored.pdf'
foreach ($f in @($pptx,$pdf)) { if (Test-Path $f) { Remove-Item $f -Force } }

$j = Start-Job -ScriptBlock $worker -ArgumentList $pptx, $pdf
if ($j | Wait-Job -Timeout 120) { Log "$(Receive-Job $j)" }
else { Log "*** TIMED OUT ***"; Stop-Job $j -ErrorAction SilentlyContinue; Get-Process POWERPNT -ErrorAction SilentlyContinue | Stop-Process -Force }
Remove-Job $j -Force -ErrorAction SilentlyContinue

if (Test-Path $pptx) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [System.IO.Compression.ZipFile]::OpenRead($pptx)
    "=== parts in a PowerPoint-authored .pptx ($((Get-Item $pptx).Length)B, $($z.Entries.Count) entries) ==="
    $z.Entries | Sort-Object FullName | ForEach-Object { "  {0,-48} {1,7}" -f $_.FullName, $_.Length }
    $z.Dispose()
}
Log done
