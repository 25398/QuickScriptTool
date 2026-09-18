# read_doc.ps1 — 读取办公文档正文（Excel/Word/PPT/PDF/文本），输出单行 JSON
# 设计约束：只用 Windows 自带能力（PowerShell 5.1 + .NET + 可选 Office COM + WinRT），
# 不依赖任何 pip/第三方库；Office 不在时用 OOXML 直读（ZipFile + XML）兜底。
# 用法: powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File read_doc.ps1 -Path <文件> [-MaxChars 20000] [-PageLimit 5] [-ImageDir <目录>]
param(
    [Parameter(Mandatory=$true)][string]$Path,
    [int]$MaxChars = 20000,
    [int]$PageLimit = 5,
    [string]$ImageDir = ''
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)

function Out-Json($obj) {
    # 只输出一行 JSON（避免 PowerShell 的表格美化把 stdout 弄脏）。
    # ★必须自己写 UTF-8 字节：宿主用 CREATE_NO_WINDOW + 重定向管道启动本脚本时没有控制台，
    # [Console]::OutputEncoding 会被忽略，中文按 ANSI(GBK) 输出 → 宿主按 UTF-8 解析直接失败。
    $json = $obj | ConvertTo-Json -Depth 6 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $stdout = [System.Console]::OpenStandardOutput()
    $stdout.Write($bytes, 0, $bytes.Length)
    $stdout.Flush()
    exit 0
}
function Fail($msg) { Out-Json @{ ok = $false; error = [string]$msg } }

# 统一收尾：清洗控制字符 + 截断（截断必须显式告知，别让模型以为读全了）
function Finish-Result($res) {
    $t = [string]$res.text
    # Word COM 的表格结构实测：单元格结束 = CR+BEL(\x07)，**整行结束 = CR+BEL 再来一次**。
    # 先「双 CR+BEL → 换行」，再把剩下的单 CR+BEL 换成制表符（顺序不能反）。
    $t = $t -replace "(`r[\x07]){2,}", "`n"
    $t = $t -replace "`r[\x07]", "`t"
    $t = $t -replace "[\x07]", "`n"
    $t = $t -replace "`r`n", "`n" -replace "`r", "`n"
    $t = $t -replace "[\x0C]", "`n"
    $t = $t -replace "`t`n", "`n"
    $t = $t -replace "(`n){3,}", "`n`n"
    if ($t.Length -gt $MaxChars) {
        $t = $t.Substring(0, $MaxChars) + "`n…（内容超过 $MaxChars 字，已截断）"
        $res.truncated = $true
    }
    $res.text = $t.Trim()
    Out-Json $res
}

# ── PDF 文本：用 Windows 自带的 Search IFilter（不需要 Office / Python / 任何第三方）──
# Windows.Data.Pdf 只能**渲染**（没有文本 API），但 .pdf 的 persistent handler 在
# Windows.Data.Pdf.dll 里注册了一个 IFilter，可以几十毫秒抽出上千字（中文也正确）。
# 两个关键坑：必须用 IInitializeWithStream（不是 IPersistFile，QI 会 E_NOINTERFACE）；
# GetText 返回 0x00041709 FILTER_S_LAST_TEXT 是**成功**码，判 hr==0 会静默拿到空串。
$script:qstPdfFilterReady = $false
function Ensure-PdfFilterType() {
    if ($script:qstPdfFilterReady) { return $true }
    try {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

[ComImport, Guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IQstInitWithStream { [PreserveSig] int Initialize(IntPtr pstream, uint grfMode); }

[ComImport, Guid("89BCB740-6119-101A-BCB7-00DD010655AF"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IQstFilter {
    [PreserveSig] int Init(uint grfFlags, uint cAttributes, IntPtr aAttributes, out uint pdwFlags);
    [PreserveSig] int GetChunk(IntPtr pStat);
    [PreserveSig] int GetText(ref uint pcwcBuffer, IntPtr awcBuffer);
    [PreserveSig] int GetValue(out IntPtr ppPropValue);
    [PreserveSig] int BindRegion(int origPos, ref Guid riid, out IntPtr ppunk);
}

public static class QstPdfText {
    [DllImport("ole32.dll")]
    private static extern int CoCreateInstance(ref Guid rclsid, IntPtr pUnkOuter, uint dwClsContext, ref Guid riid, out IntPtr ppv);
    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int SHCreateStreamOnFileEx(string pszFile, uint grfMode, uint dwAttributes, bool fCreate, IntPtr pstmTemplate, out IntPtr ppstm);

    public static string Extract(string path, string clsid, int maxChars, out string diag) {
        var log = new StringBuilder();
        var sb = new StringBuilder();
        Guid cl = new Guid(clsid);
        Guid iidFilter = new Guid("89BCB740-6119-101A-BCB7-00DD010655AF");
        IntPtr pObj = IntPtr.Zero;
        int hr = CoCreateInstance(ref cl, IntPtr.Zero, 0x1, ref iidFilter, out pObj);
        if (hr != 0) { diag = "CoCreateInstance hr=0x" + hr.ToString("X8"); return ""; }
        IntPtr pStream = IntPtr.Zero;
        try {
            hr = SHCreateStreamOnFileEx(path, 0x20, 0, false, IntPtr.Zero, out pStream);
            if (hr != 0) { diag = "SHCreateStreamOnFileEx hr=0x" + hr.ToString("X8"); return ""; }
            var init = (IQstInitWithStream)Marshal.GetObjectForIUnknown(pObj);
            hr = init.Initialize(pStream, 0x0);
            if (hr != 0) { diag = "Initialize hr=0x" + hr.ToString("X8"); return ""; }
            var filter = (IQstFilter)init;
            uint flags = 0;
            hr = filter.Init(0, 0, IntPtr.Zero, out flags);
            if (hr != 0) { diag = "Init hr=0x" + hr.ToString("X8"); return ""; }
            IntPtr stat = Marshal.AllocCoTaskMem(256);
            int bufChars = 65536;
            IntPtr buf = Marshal.AllocCoTaskMem(bufChars * 2);
            try {
                while (sb.Length < maxChars) {
                    hr = filter.GetChunk(stat);
                    if (unchecked((uint)hr) == 0x8004170C) break;
                    if (hr != 0) { log.Append("GetChunk hr=0x" + hr.ToString("X8") + "; "); break; }
                    uint chunkFlags = (uint)Marshal.ReadInt32(stat, 8);
                    if ((chunkFlags & 0x1) == 0) continue;
                    while (true) {
                        uint cch = (uint)bufChars;
                        hr = filter.GetText(ref cch, buf);
                        if (hr >= 0 && cch > 0) {
                            sb.Append(Marshal.PtrToStringUni(buf, (int)cch));
                            if (sb.Length >= maxChars) break;
                            if ((uint)hr == 0x00041709) break;
                        } else break;
                    }
                }
            } finally { Marshal.FreeCoTaskMem(stat); Marshal.FreeCoTaskMem(buf); }
            diag = log.ToString();
        } finally {
            if (pStream != IntPtr.Zero) Marshal.Release(pStream);
            if (pObj != IntPtr.Zero) Marshal.Release(pObj);
        }
        return sb.ToString();
    }
}
'@ -ErrorAction Stop
        $script:qstPdfFilterReady = $true
        return $true
    } catch { return $false }
}

# 从注册表解析 .pdf 的 IFilter CLSID（不写死：硬编码值只在 Win11 26200 上验证过）
function Resolve-PdfFilterClsid() {
    $fallback = '6C337B26-3E38-4F98-813B-FBA18BAB64F5'
    try {
        $handler = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\Classes\.pdf' -Name 'PersistentHandler' -ErrorAction Stop).PersistentHandler
        if ($handler) {
            $key = "HKLM:\SOFTWARE\Classes\CLSID\$handler\PersistentAddinsRegistered\{89BCB740-6119-101A-BCB7-00DD010655AF}"
            $clsid = (Get-ItemProperty -Path $key -ErrorAction Stop).'(default)'
            if ($clsid) { return $clsid }
        }
    } catch { }
    return $fallback
}

$result = [ordered]@{ ok = $false; format = ''; engine = ''; text = ''; images = @(); note = ''; truncated = $false }

try {
    if (-not (Test-Path -LiteralPath $Path)) { Fail "文件不存在：$Path" }
    $full = (Resolve-Path -LiteralPath $Path).Path
    $ext = [System.IO.Path]::GetExtension($full).ToLowerInvariant()
    $result.format = $ext.TrimStart('.')

    Add-Type -AssemblyName System.IO.Compression | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

    # ── 纯文本类（含 Excel 导出的 CSV：可能是 GBK/UTF-8/带 BOM）──
    function Read-TextSmart([string]$p) {
        $bytes = [System.IO.File]::ReadAllBytes($p)
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            return (New-Object System.Text.UTF8Encoding($false)).GetString($bytes, 3, $bytes.Length - 3)
        }
        if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
            return [System.Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
        }
        # 严格 UTF-8 试解；失败则按系统 ANSI（中文机器=GBK）
        try {
            $strict = New-Object System.Text.UTF8Encoding($false, $true)
            return $strict.GetString($bytes)
        } catch {
            return [System.Text.Encoding]::Default.GetString($bytes)
        }
    }

    if ($ext -in @('.txt', '.csv', '.tsv', '.log', '.json', '.md', '.xml', '.ini', '.ps1', '.bat', '.sql', '.html', '.htm')) {
        $text = Read-TextSmart $full
        $result.ok = $true; $result.engine = 'text'
        $result.text = $text
        Finish-Result $result
    }

    # ── OOXML（xlsx/docx/pptx）直读：ZipFile + XML，不需要 Office ──
    function Read-ZipEntryText($zip, [string]$name) {
        $entry = $zip.GetEntry($name)
        if (-not $entry) { return $null }
        $sr = New-Object System.IO.StreamReader($entry.Open(), [System.Text.Encoding]::UTF8)
        try { return $sr.ReadToEnd() } finally { $sr.Close() }
    }
    function Get-NsManager($xmlDoc) {
        $ns = New-Object System.Xml.XmlNamespaceManager($xmlDoc.NameTable)
        $ns.AddNamespace('w', 'http://schemas.openxmlformats.org/wordprocessingml/2006/main')
        $ns.AddNamespace('a', 'http://schemas.openxmlformats.org/drawingml/2006/main')
        $ns.AddNamespace('p', 'http://schemas.openxmlformats.org/presentationml/2006/main')
        $ns.AddNamespace('m', 'http://schemas.openxmlformats.org/spreadsheetml/2006/main')
        return $ns
    }
    function Load-Xml([string]$text) {
        $d = New-Object System.Xml.XmlDocument
        $d.LoadXml($text)
        return $d
    }
    # Excel 列号 A→1, AA→27
    function Col-Index([string]$ref) {
        $n = 0
        foreach ($ch in $ref.ToCharArray()) {
            if ($ch -ge 'A' -and $ch -le 'Z') { $n = $n * 26 + ([int][char]$ch - 64) } else { break }
        }
        return $n
    }

    if ($ext -in @('.xlsx', '.xlsm')) {
        # ① 有 Office 就用 COM（值最准，含公式结果、日期格式）
        $comText = $null
        try {
            $xl = New-Object -ComObject Excel.Application
            $xl.Visible = $false
            $xl.DisplayAlerts = $false
            $wb = $xl.Workbooks.Open($full, 0, $true)
            $sb = New-Object System.Text.StringBuilder
            foreach ($ws in $wb.Worksheets) {
                [void]$sb.AppendLine("## 工作表: " + $ws.Name)
                $used = $ws.UsedRange
                $rows = [int]$used.Rows.Count
                $cols = [int]$used.Columns.Count
                if ($rows -gt 200) { $rows = 200 }
                if ($cols -gt 40) { $cols = 40 }
                if ($rows -ge 1 -and $cols -ge 1) {
                    $vals = $used.Resize($rows, $cols).Value2
                    for ($r = 1; $r -le $rows; $r++) {
                        $cells = @()
                        for ($c = 1; $c -le $cols; $c++) {
                            $v = if ($rows -eq 1 -and $cols -eq 1) { $vals } else { $vals[$r, $c] }
                            $cells += ([string]$v)
                        }
                        $line = ($cells -join "`t").TrimEnd("`t")
                        if ($line -ne '') { [void]$sb.AppendLine($line) }
                    }
                }
            }
            $comText = $sb.ToString()
            $wb.Close($false)
            $xl.Quit()
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($wb)
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($xl)
            [GC]::Collect(); [GC]::WaitForPendingFinalizers()
            $result.engine = 'excel-com'
        } catch {
            $comText = $null
        }
        if ($comText -ne $null -and $comText.Trim() -ne '') {
            $result.ok = $true; $result.text = $comText
            Finish-Result $result
        }
        # ② 无 Office：直读 OOXML
        $zip = [System.IO.Compression.ZipFile]::OpenRead($full)
        try {
            $shared = @()
            $ss = Read-ZipEntryText $zip 'xl/sharedStrings.xml'
            if ($ss) {
                $sx = Load-Xml $ss
                foreach ($si in $sx.SelectNodes('//*[local-name()="si"]')) {
                    $t = ''
                    foreach ($node in $si.SelectNodes('.//*[local-name()="t"]')) { $t += $node.InnerText }
                    $shared += $t
                }
            }
            $names = @{}
            $wbx = Read-ZipEntryText $zip 'xl/workbook.xml'
            if ($wbx) {
                $wx = Load-Xml $wbx
                $i = 1
                foreach ($sh in $wx.SelectNodes('//*[local-name()="sheet"]')) {
                    $names[$i] = [string]$sh.GetAttribute('name'); $i++
                }
            }
            $sheets = @($zip.Entries | Where-Object { $_.FullName -like 'xl/worksheets/sheet*.xml' } | Sort-Object FullName)
            $sb = New-Object System.Text.StringBuilder
            $idx = 0
            foreach ($entry in $sheets) {
                $idx++
                $label = if ($names.ContainsKey($idx)) { $names[$idx] } else { "sheet$idx" }
                [void]$sb.AppendLine("## 工作表: $label")
                $sr = New-Object System.IO.StreamReader($entry.Open(), [System.Text.Encoding]::UTF8)
                $sheetXml = $sr.ReadToEnd(); $sr.Close()
                $doc = Load-Xml $sheetXml
                $rowCount = 0
                foreach ($row in $doc.SelectNodes('//*[local-name()="row"]')) {
                    if (++$rowCount -gt 200) { [void]$sb.AppendLine('…（更多行已省略）'); break }
                    $cells = @{}
                    $maxCol = 0
                    foreach ($c in $row.SelectNodes('./*[local-name()="c"]')) {
                        $ref = [string]$c.GetAttribute('r')
                        $ci = Col-Index $ref; if ($ci -eq 0) { $ci = $maxCol + 1 }
                        $t = [string]$c.GetAttribute('t')
                        $vNode = $c.SelectSingleNode('./*[local-name()="v"]')
                        $isNode = $c.SelectSingleNode('./*[local-name()="is"]')
                        $val = ''
                        if ($t -eq 's' -and $vNode) {
                            $si = [int]$vNode.InnerText
                            if ($si -ge 0 -and $si -lt $shared.Count) { $val = $shared[$si] }
                        } elseif ($isNode) {
                            foreach ($node in $isNode.SelectNodes('.//*[local-name()="t"]')) { $val += $node.InnerText }
                        } elseif ($vNode) {
                            $val = $vNode.InnerText
                        }
                        $cells[$ci] = $val
                        if ($ci -gt $maxCol) { $maxCol = $ci }
                    }
                    if ($maxCol -gt 40) { $maxCol = 40 }
                    $line = @()
                    for ($c = 1; $c -le $maxCol; $c++) { $line += [string]$cells[$c] }
                    $joined = ($line -join "`t").TrimEnd("`t")
                    if ($joined -ne '') { [void]$sb.AppendLine($joined) }
                }
            }
            $result.ok = $true; $result.engine = 'ooxml'; $result.text = $sb.ToString()
            Finish-Result $result
        } finally { $zip.Dispose() }
    }

    if ($ext -eq '.docx') {
        $comText = $null
        try {
            $w = New-Object -ComObject Word.Application
            $w.Visible = $false
            $w.DisplayAlerts = 0
            $d = $w.Documents.Open($full, $false, $true)
            $comText = $d.Content.Text
            $d.Close($false)
            $w.Quit()
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($d)
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($w)
            [GC]::Collect(); [GC]::WaitForPendingFinalizers()
            $result.engine = 'word-com'
        } catch { $comText = $null }
        if ($comText -ne $null -and $comText.Trim() -ne '') {
            $result.ok = $true; $result.text = $comText
            Finish-Result $result
        }
        $zip = [System.IO.Compression.ZipFile]::OpenRead($full)
        try {
            $docXml = Read-ZipEntryText $zip 'word/document.xml'
            if (-not $docXml) { Fail 'docx 缺少 word/document.xml' }
            $doc = Load-Xml $docXml
            $sb = New-Object System.Text.StringBuilder
            $body = $doc.SelectSingleNode('//*[local-name()="body"]')
            if ($body) {
                foreach ($node in $body.ChildNodes) {
                    if ($node.LocalName -eq 'p') {
                        $line = ''
                        foreach ($t in $node.SelectNodes('.//*[local-name()="t"]')) { $line += $t.InnerText }
                        [void]$sb.AppendLine($line)
                    } elseif ($node.LocalName -eq 'tbl') {
                        foreach ($tr in $node.SelectNodes('./*[local-name()="tr"]')) {
                            $cells = @()
                            foreach ($tc in $tr.SelectNodes('./*[local-name()="tc"]')) {
                                $cell = ''
                                foreach ($t in $tc.SelectNodes('.//*[local-name()="t"]')) { $cell += $t.InnerText }
                                $cells += $cell
                            }
                            [void]$sb.AppendLine(($cells -join "`t"))
                        }
                    }
                }
            }
            $result.ok = $true; $result.engine = 'ooxml'; $result.text = $sb.ToString()
            Finish-Result $result
        } finally { $zip.Dispose() }
    }

    if ($ext -eq '.pptx') {
        $zip = [System.IO.Compression.ZipFile]::OpenRead($full)
        try {
            # ★页序必须按 presentation.xml 的 sldIdLst + rels 解析，**不能**按 slide1..N 文件名排：
            # 实测文件名顺序可以与放映顺序不同（改过页序的 PPT 尤其明显）。
            $slidePaths = New-Object System.Collections.Generic.List[string]
            $presXml = Read-ZipEntryText $zip 'ppt/presentation.xml'
            $relsXml = Read-ZipEntryText $zip 'ppt/_rels/presentation.xml.rels'
            if ($presXml -and $relsXml) {
                $relMap = @{}
                $rels = Load-Xml $relsXml
                foreach ($rel in $rels.SelectNodes('//*[local-name()="Relationship"]')) {
                    $id = [string]$rel.GetAttribute('Id')
                    $target = [string]$rel.GetAttribute('Target')
                    if (-not $id -or -not $target) { continue }
                    $target = $target -replace '\\', '/'
                    if ($target -match '^[a-zA-Z][a-zA-Z0-9+.\-]*:') { continue }   # 外部链接，跳过
                    if ($target.StartsWith('/')) {
                        $target = $target.Substring(1)
                    } else {
                        # rels 在 ppt/_rels/ 下，相对 Target 要按 ppt/ 解析
                        $target = 'ppt/' + $target
                    }
                    while ($target -match '[^/]+/\.\./') { $target = $target -replace '[^/]+/\.\./', '' }
                    $target = $target -replace '/\./', '/'
                    $relMap[$id] = $target
                }
                $pres = Load-Xml $presXml
                foreach ($sldId in $pres.SelectNodes('//*[local-name()="sldId"]')) {
                    $rid = ''
                    foreach ($attr in $sldId.Attributes) {
                        if ($attr.LocalName -eq 'id' -and $attr.NamespaceURI -like '*relationships*') { $rid = $attr.Value }
                    }
                    if (-not $rid) { continue }
                    if ($relMap.ContainsKey($rid)) {
                        $slidePaths.Add([string]$relMap[$rid])
                    }
                }
            }
            if ($slidePaths.Count -eq 0) {
                # 兜底：rels 缺失时退回文件名顺序
                $slidePaths = @($zip.Entries |
                    Where-Object { $_.FullName -match '^ppt/slides/slide\d+\.xml$' } |
                    Sort-Object { [int]([regex]::Match($_.FullName, '(\d+)').Groups[1].Value) } |
                    ForEach-Object { $_.FullName })
            }
            $sb = New-Object System.Text.StringBuilder
            $n = 0
            foreach ($slidePath in $slidePaths) {
                $n++
                if ($n -gt $PageLimit) { [void]$sb.AppendLine("…（还有更多页，已省略）"); break }
                $xml = Read-ZipEntryText $zip $slidePath
                if (-not $xml) { continue }
                $doc = Load-Xml $xml
                $ns = Get-NsManager $doc
                [void]$sb.AppendLine("## 第 $n 页")
                foreach ($p in $doc.SelectNodes('//*[local-name()="p"]')) {
                    $line = ''
                    foreach ($t in $p.SelectNodes('.//*[local-name()="t"]')) { $line += $t.InnerText }
                    if ($line.Trim() -ne '') { [void]$sb.AppendLine($line) }
                }
            }
            $result.ok = $true; $result.engine = 'ooxml'; $result.text = $sb.ToString()
            Finish-Result $result
        } finally { $zip.Dispose() }
    }

    # ── 旧格式（.xls/.doc/.ppt）：只能靠 Office COM ──
    if ($ext -in @('.xls', '.doc', '.ppt', '.docm', '.xlsb')) {
        if ($ext -in @('.xls', '.xlsb')) {
            $xl = New-Object -ComObject Excel.Application
            $xl.Visible = $false; $xl.DisplayAlerts = $false
            $wb = $xl.Workbooks.Open($full, 0, $true)
            $sb = New-Object System.Text.StringBuilder
            foreach ($ws in $wb.Worksheets) {
                [void]$sb.AppendLine("## 工作表: " + $ws.Name)
                $used = $ws.UsedRange
                $rows = [Math]::Min(200, [int]$used.Rows.Count); $cols = [Math]::Min(40, [int]$used.Columns.Count)
                if ($rows -ge 1 -and $cols -ge 1) {
                    $vals = $used.Resize($rows, $cols).Value2
                    for ($r = 1; $r -le $rows; $r++) {
                        $cells = @()
                        for ($c = 1; $c -le $cols; $c++) {
                            $v = if ($rows -eq 1 -and $cols -eq 1) { $vals } else { $vals[$r, $c] }
                            $cells += ([string]$v)
                        }
                        $line = ($cells -join "`t").TrimEnd("`t")
                        if ($line -ne '') { [void]$sb.AppendLine($line) }
                    }
                }
            }
            $wb.Close($false); $xl.Quit()
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($wb)
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($xl)
            [GC]::Collect(); [GC]::WaitForPendingFinalizers()
            $result.ok = $true; $result.engine = 'excel-com'; $result.text = $sb.ToString()
            Finish-Result $result
        } elseif ($ext -eq '.ppt') {
            $ppt = New-Object -ComObject PowerPoint.Application
            $pres = $ppt.Presentations.Open($full, $true, $false, $false)
            $sb = New-Object System.Text.StringBuilder
            $n = 0
            foreach ($slide in $pres.Slides) {
                $n++
                if ($n -gt $PageLimit) { [void]$sb.AppendLine('…（还有更多页，已省略）'); break }
                [void]$sb.AppendLine("## 第 $n 页")
                foreach ($shape in $slide.Shapes) {
                    try {
                        if ($shape.HasTextFrame -eq -1 -and $shape.TextFrame.HasText -eq -1) {
                            [void]$sb.AppendLine([string]$shape.TextFrame.TextRange.Text)
                        }
                    } catch { }
                }
            }
            $pres.Close(); $ppt.Quit()
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($pres)
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($ppt)
            [GC]::Collect(); [GC]::WaitForPendingFinalizers()
            $result.ok = $true; $result.engine = 'ppt-com'; $result.text = $sb.ToString()
            Finish-Result $result
        } else {
            $w = New-Object -ComObject Word.Application
            $w.Visible = $false; $w.DisplayAlerts = 0
            $d = $w.Documents.Open($full, $false, $true)
            $result.ok = $true; $result.engine = 'word-com'; $result.text = [string]$d.Content.Text
            $d.Close($false); $w.Quit()
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($d)
            [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($w)
            [GC]::Collect(); [GC]::WaitForPendingFinalizers()
            Finish-Result $result
        }
    }

    if ($ext -eq '.pdf') {
        # ① pdftotext（poppler，装了就用）：最快最准的文本提取，优先复用现成实现
        $pdftotext = Get-Command pdftotext.exe -ErrorAction SilentlyContinue
        if ($pdftotext) {
            $tmpTxt = Join-Path ([System.IO.Path]::GetTempPath()) ("qst_pdf_" + [Guid]::NewGuid().ToString('N') + ".txt")
            try {
                $psi = New-Object System.Diagnostics.ProcessStartInfo
                $psi.FileName = $pdftotext.Source
                $psi.Arguments = "-layout -enc UTF-8 -f 1 -l $PageLimit `"$full`" `"$tmpTxt`""
                $psi.UseShellExecute = $false
                $psi.CreateNoWindow = $true
                $p = [System.Diagnostics.Process]::Start($psi)
                $p.WaitForExit(20000) | Out-Null
                if (Test-Path -LiteralPath $tmpTxt) {
                    $txt = Read-TextSmart $tmpTxt
                    if ($txt.Trim().Length -ge 8) {
                        $result.ok = $true; $result.engine = 'pdftotext'; $result.text = $txt
                        $result.note = "PDF 文本由 pdftotext 提取（前 $PageLimit 页）"
                        Remove-Item -LiteralPath $tmpTxt -Force -ErrorAction SilentlyContinue
                        Finish-Result $result
                    }
                }
            } catch { }
            Remove-Item -LiteralPath $tmpTxt -Force -ErrorAction SilentlyContinue
        }
        # ② Windows 自带 IFilter（无 Office/无 Python 也能抽文本；这是最优先的「零依赖」路线）
        if (Ensure-PdfFilterType) {
            try {
                $diag = ''
                $txt = [QstPdfText]::Extract($full, (Resolve-PdfFilterClsid), 400000, [ref]$diag)
                if ($txt -and $txt.Trim().Length -ge 8) {
                    $result.ok = $true; $result.engine = 'pdf-ifilter'; $result.text = $txt
                    $result.note = 'PDF 文本由 Windows 自带搜索过滤器提取（无版式/无分页，'
                    $result.note += '扫描件会抽不到字）。'
                    Finish-Result $result
                }
            } catch { }
        }
        # ③ 扫描件（没有文本层）：把页面渲染成 PNG 交给视觉模型读。
        #    ★不要用 Word COM 转 PDF —— 实测会挂住（>2 分钟无对话框），还可能留下卡死的 WINWORD。
        if ([string]::IsNullOrEmpty($ImageDir)) { $ImageDir = [System.IO.Path]::GetTempPath() }
        if (-not (Test-Path -LiteralPath $ImageDir)) { New-Item -ItemType Directory -Force -Path $ImageDir | Out-Null }
        Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null
        $asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
            $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
        $asTaskAction = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
            $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' })[0]
        function Await($op, $type) {
            $m = $asTaskGeneric.MakeGenericMethod($type)
            $task = $m.Invoke($null, @($op))
            $task.Wait(-1) | Out-Null
            return $task.Result
        }
        function AwaitAction($op) {
            # IAsyncAction（RenderToStreamAsync）没有返回值：必须用同一个 AsTask 重载族，
            # 直接 $op.AsTask() 在 PowerShell 里看不到扩展方法（会静默不生效）
            $task = $asTaskAction.Invoke($null, @($op))
            $task.Wait(-1) | Out-Null
        }
        [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime] | Out-Null
        [Windows.Data.Pdf.PdfDocument, Windows.Data.Pdf, ContentType = WindowsRuntime] | Out-Null
        [Windows.Storage.StorageFolder, Windows.Storage, ContentType = WindowsRuntime] | Out-Null
        $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($full)) ([Windows.Storage.StorageFile])
        $pdf = Await ([Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($file)) ([Windows.Data.Pdf.PdfDocument])
        $folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($ImageDir)) ([Windows.Storage.StorageFolder])
        $pages = [int]$pdf.PageCount
        if ($pages -gt $PageLimit) { $pages = $PageLimit }
        $imgs = @()
        for ($i = 0; $i -lt $pages; $i++) {
            $page = $pdf.GetPage($i)
            $name = "qst_pdf_{0}_{1}.png" -f ([System.IO.Path]::GetFileNameWithoutExtension($full) -replace '[^\w\-]', '_'), ($i + 1)
            $outFile = Await ($folder.CreateFileAsync($name, [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])
            $stream = Await ($outFile.OpenAsync([Windows.Storage.FileAccessMode]::ReadWrite)) ([Windows.Storage.Streams.IRandomAccessStream])
            # 渲染尺寸钳住长边（太大既慢又费 token；VLM 看 1600 长边足够）
            $opts = New-Object Windows.Data.Pdf.PdfPageRenderOptions
            $w0 = [double]$page.Size.Width
            $h0 = [double]$page.Size.Height
            $long = [Math]::Max($w0, $h0)
            if ($long -gt 0) {
                $scale = [Math]::Min(2.0, 1600.0 / $long)
                if ($scale -lt 1.0) { $scale = 1.0 }
                $opts.DestinationWidth = [uint32][Math]::Round($w0 * $scale)
                $opts.DestinationHeight = [uint32][Math]::Round($h0 * $scale)
            }
            AwaitAction ($page.RenderToStreamAsync($stream, $opts))
            $stream.Dispose()
            $imgs += $outFile.Path
        }
        $result.ok = $true; $result.engine = 'pdf-render'; $result.images = $imgs
        $result.note = "PDF 未提取到文本层（本机无 pdftotext/IFilter 也读不到，或这是扫描件）：已渲染前 $pages 页为图片，请直接读图"
        Finish-Result $result
    }

    Fail "暂不支持的格式：$ext"
} catch {
    Fail ($_.Exception.Message + ' @ ' + $_.InvocationInfo.ScriptLineNumber)
}
