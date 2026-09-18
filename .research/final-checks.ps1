# Final gap-closing checks:
#  (1) Is styles.xml actually REQUIRED for Word to open a .docx? (3-part vs 5-part)
#  (2) .xlsm: what does Excel COM SaveAs(52) produce, and is vbaProject.bin present?
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$dir = 'D:\other\software\.research\fixtures'
function Log($m) { "[{0:HH:mm:ss}] {1}" -f (Get-Date), $m }

# ---- (1) build a 3-part docx (no styles.xml, no document.xml.rels) ----
$three = Join-Path $dir 'docx-3part.docx'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'D:\other\software\.research\make-minimal-docx.ps1' -Path $three -OmitStyles | Out-Null
$z = [System.IO.Compression.ZipFile]::OpenRead($three)
Log "3-part docx parts: $(($z.Entries | ForEach-Object { $_.FullName }) -join ', ')"
$z.Dispose()

$wordWorker = {
    param($f)
    $w = New-Object -ComObject Word.Application
    $w.Visible = $false; $w.DisplayAlerts = 0
    try {
        $d = $w.Documents.Open($f, [ref]$false, [ref]$true)
        $t = $d.Content.Text
        "WORD OPENED 3-PART DOCX: paras=$($d.Paragraphs.Count) tables=$($d.Tables.Count) chars=$($t.Length)"
        $d.Close($false)
    } catch { "WORD REJECTED 3-PART DOCX: $($_.Exception.Message)" }
    finally { try { $w.Quit() } catch {}; Get-Process WINWORD -ErrorAction SilentlyContinue | Stop-Process -Force }
}
$j = Start-Job -ScriptBlock $wordWorker -ArgumentList $three
if ($j | Wait-Job -Timeout 90) { Log "$(Receive-Job $j)" } else { Log "TIMEOUT"; Stop-Job $j -EA SilentlyContinue; Get-Process WINWORD -EA SilentlyContinue | Stop-Process -Force }
Remove-Job $j -Force -EA SilentlyContinue

# ---- (2) .xlsm ----
$xl = New-Object -ComObject Excel.Application
$xl.Visible = $false; $xl.DisplayAlerts = $false
try {
    $wb = $xl.Workbooks.Add()
    $ws = $wb.Worksheets.Item(1)
    $ws.Cells.Item(1,1) = '城市'; $ws.Cells.Item(1,2) = '值'
    $ws.Cells.Item(2,1) = '北京'; $ws.Cells.Item(2,2) = 42
    $xlsm = Join-Path $dir 'test.xlsm'
    if (Test-Path $xlsm) { Remove-Item $xlsm -Force }
    $wb.SaveAs($xlsm, 52)    # 52 = xlOpenXMLWorkbookMacroEnabled
    $wb.Close($false)
    Log "xlsm written: $((Get-Item $xlsm).Length)B"
} catch { Log "xlsm ERROR: $($_.Exception.Message)" }
finally { $xl.Quit(); [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($xl); Get-Process EXCEL -EA SilentlyContinue | Stop-Process -Force }

Start-Sleep 1
$z = [System.IO.Compression.ZipFile]::OpenRead($xlsm)
Log "=== .xlsm parts ($($z.Entries.Count)) ==="
$z.Entries | Sort-Object FullName | ForEach-Object { "    {0,-42} {1,7}" -f $_.FullName, $_.Length }
$e = $z.GetEntry('[Content_Types].xml'); $s = $e.Open()
$sr = New-Object System.IO.StreamReader($s, [System.Text.Encoding]::UTF8)
$ct = $sr.ReadToEnd(); $sr.Dispose(); $z.Dispose()
Log "  workbook content-type override present: $($ct -match 'macroEnabled')"
Log "  vbaProject.bin present: $((Test-Path (Join-Path $dir 'x')) ) / in zip: $($ct -match 'vbaProject')"
# can the pure-ZipFile route read an .xlsm? (same sharedStrings layout)
$z2 = [System.IO.Compression.ZipFile]::OpenRead($xlsm)
Log "  has xl/sharedStrings.xml: $($null -ne $z2.GetEntry('xl/sharedStrings.xml'))"
Log "  has xl/worksheets/sheet1.xml: $($null -ne $z2.GetEntry('xl/worksheets/sheet1.xml'))"
$z2.Dispose()
Log done
