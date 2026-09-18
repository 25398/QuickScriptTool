# Generates a minimal, valid .docx using only OOXML parts + System.IO.Compression.
# Deliberately splits one sentence across two <w:r> runs to model the "fragmented runs" gotcha.
param(
    [string]$Path = 'D:\other\software\.research\fixtures\generated.docx',
    [switch]$OmitStyles
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression | Out-Null

$enc = New-Object System.Text.UTF8Encoding($false)   # UTF-8, no BOM
function XmlEsc([string]$s) { $s -replace '&','&amp;' -replace '<','&lt;' -replace '>','&gt;' -replace '"','&quot;' }

# a paragraph of text
function P([string]$text, [string]$style = '') {
    $ppr = if ($style) { '<w:pPr><w:pStyle w:val="' + $style + '"/></w:pPr>' } else { '' }
    '<w:p>' + $ppr + '<w:r><w:t xml:space="preserve">' + (XmlEsc $text) + '</w:t></w:r></w:p>'
}
# a paragraph whose text is split across 2 runs (models Word's run fragmentation)
function PSplit([string]$a, [string]$b) {
    '<w:p><w:r><w:t xml:space="preserve">' + (XmlEsc $a) + '</w:t></w:r>' +
    '<w:r><w:rPr><w:b/></w:rPr><w:t xml:space="preserve">' + (XmlEsc $b) + '</w:t></w:r></w:p>'
}
function Cell([string]$text) {
    '<w:tc><w:tcPr><w:tcW w:w="2400" w:type="dxa"/></w:tcPr>' +
    '<w:p><w:r><w:t xml:space="preserve">' + (XmlEsc $text) + '</w:t></w:r></w:p></w:tc>'
}
function Row([string[]]$cells) {
    '<w:tr>' + (($cells | ForEach-Object { Cell $_ }) -join '') + '</w:tr>'
}

$body = @()
$body += P 'Generated Report Title' 'Heading1'
$body += PSplit 'This sentence is split across two runs ' 'to model fragmented w:r elements.'
$body += P 'Chinese text: 城市、销售额、增长'
$body += '<w:tbl><w:tblPr><w:tblW w:w="0" w:type="auto"/><w:tblBorders>' +
         '<w:top w:val="single" w:sz="4"/><w:left w:val="single" w:sz="4"/>' +
         '<w:bottom w:val="single" w:sz="4"/><w:right w:val="single" w:sz="4"/>' +
         '<w:insideH w:val="single" w:sz="4"/><w:insideV w:val="single" w:sz="4"/>' +
         '</w:tblBorders></w:tblPr>' +
         '<w:tblGrid><w:gridCol w:w="2400"/><w:gridCol w:w="2400"/><w:gridCol w:w="2400"/></w:tblGrid>'
$body += Row @('城市','销售额','增长')
$body += Row @('北京','1200','5%')
$body += Row @('上海','3400','8%')
$body += '</w:tbl>'
$body += P 'Paragraph after the table.'

$documentXml = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>' +
  '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">' +
  '<w:body>' + ($body -join '') +
  '<w:sectPr><w:pgSz w:w="11906" w:h="16838"/>' +
  '<w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440"/></w:sectPr>' +
  '</w:body></w:document>'

$parts = [ordered]@{
  '[Content_Types].xml' =
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>' +
    '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">' +
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>' +
    '<Default Extension="xml" ContentType="application/xml"/>' +
    '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>' +
    $(if (-not $OmitStyles) { '<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>' }) +
    '</Types>'
  '_rels/.rels' =
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>' +
    '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' +
    '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>' +
    '</Relationships>'
  'word/document.xml' = $documentXml
}
if (-not $OmitStyles) {
    # styleId="Heading1" must match w:pStyle val; Word names built-ins Heading1/Normal
    $parts['word/styles.xml'] =
      '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>' +
      '<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">' +
      '<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/></w:style>' +
      '<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/>' +
      '<w:basedOn w:val="Normal"/><w:pPr><w:outlineLvl w:val="0"/></w:pPr>' +
      '<w:rPr><w:b/><w:sz w:val="32"/></w:rPr></w:style>' +
      '</w:styles>'
    $parts['word/_rels/document.xml.rels'] =
      '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>' +
      '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' +
      '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>' +
      '</Relationships>'
}

if (Test-Path $Path) { Remove-Item $Path -Force }
$fs = [System.IO.File]::Open($Path, 'CreateNew', 'Write')
try {
    $zip = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($k in $parts.Keys) {
            $e = $zip.CreateEntry($k, [System.IO.Compression.CompressionLevel]::Optimal)
            $st = $e.Open()
            try { $b = $enc.GetBytes($parts[$k]); $st.Write($b, 0, $b.Length) } finally { $st.Dispose() }
        }
    } finally { $zip.Dispose() }
} finally { $fs.Dispose() }
"wrote $Path ($((Get-Item $Path).Length) bytes), parts: $($parts.Keys -join ', ')"
