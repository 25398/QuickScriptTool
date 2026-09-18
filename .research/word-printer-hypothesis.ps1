# Hypothesis: Word COM PDF export hangs when the DEFAULT PRINTER is
# "Microsoft Print to PDF" (port PORTPROMPT:), because Word queries the default
# printer DC for page layout. Each job creates its OWN Word instance (COM objects
# cannot cross a Start-Job process boundary).
$ErrorActionPreference = 'Stop'
$dir  = 'D:\other\software\.research\fixtures'
$docx = Join-Path $dir 'generated.docx'
function Log($m) { "[{0:HH:mm:ss}] {1}" -f (Get-Date), $m }

$net = New-Object -ComObject WScript.Network
$before = (Get-CimInstance Win32_Printer | Where-Object Default).Name
Log "default BEFORE: '$before'"

$worker = {
    param($docx, $pdf, $mode, $timeoutSec)
    $ErrorActionPreference = 'Stop'
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    try {
        $active = $null
        try { $active = $word.ActivePrinter } catch {}
        $doc = $word.Documents.Open($docx, [ref]$false, [ref]$true)
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        if ($mode -eq 'saveas') { $doc.SaveAs2($pdf, 17) } else { $doc.ExportAsFixedFormat($pdf, 17) }
        $sw.Stop()
        $doc.Close($false)
        "OK mode=$mode activePrinter='$active' seconds=$([math]::Round($sw.Elapsed.TotalSeconds,1))"
    } catch { "ERR mode=$mode $($_.Exception.Message)" }
    finally {
        try { $word.Quit() } catch {}
        Get-Process WINWORD -ErrorAction SilentlyContinue | Stop-Process -Force
    }
}

foreach ($printer in @('OneNote (Desktop)', $before)) {
    Log "================ default printer := '$printer' ================"
    $net.SetDefaultPrinter($printer)
    Start-Sleep -Milliseconds 800
    Log "  confirmed default: '$((Get-CimInstance Win32_Printer | Where-Object Default).Name)'"
    foreach ($mode in @('saveas','export')) {
        $pdf = Join-Path $dir "wp-$mode-$([regex]::Replace($printer,'[^A-Za-z]','')).pdf"
        if (Test-Path $pdf) { Remove-Item $pdf -Force }
        $j = Start-Job -ScriptBlock $worker -ArgumentList $docx, $pdf, $mode, 60
        if ($j | Wait-Job -Timeout 75) {
            Log "  [$mode] $(Receive-Job $j)  file=$(Test-Path $pdf) $(if(Test-Path $pdf){"$((Get-Item $pdf).Length)B"})"
        } else {
            Log "  [$mode] *** TIMED OUT (>75s) - HANG REPRODUCED ***"
            Stop-Job $j -ErrorAction SilentlyContinue
            Get-Process WINWORD -ErrorAction SilentlyContinue | Stop-Process -Force
        }
        Remove-Job $j -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
    }
}
$net.SetDefaultPrinter($before)
Log "restored default: '$((Get-CimInstance Win32_Printer | Where-Object Default).Name)'"
Log done
