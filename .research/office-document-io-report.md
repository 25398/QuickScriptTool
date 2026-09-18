# Office document I/O for QuickScriptTool — reuse/port survey

**Scope:** reusable/portable open source + verified native routes for reading/writing `.xlsx/.xlsm/.csv`, `.docx`, `.pptx`, `.pdf` from an agent whose only primitive is *run a command line* + file I/O + screenshots. No hard Office dep, no assumed pip installs, no C++ deps.

**Evidence convention.** ✔︎ = **I executed it on this machine** and observed the result. ⚠︎ = verified to fail/hang. 📄 = read from upstream source/docs, not executed. Else **UNVERIFIED**. Test artifacts in `.research/`.

**Test machine (measured):** Windows 11 build 26200 · PowerShell **5.1**.26100.9444 Desktop, 64-bit · **ANSI codepage gb2312** · Office **16.0** (Word/Excel/PowerPoint all COM-registered 64-bit) · `System.IO.Compression.FileSystem` present. **Absent:** LibreOffice/`soffice`, `pdftotext`, `pandoc`, working Python (`python`/`python3` are WindowsApps stubs → exit 9009). **Execution policy `Restricted`.**

---

## 1. Verdict — recommended routes

| Format | Primary | Fallback | Why |
|---|---|---|---|
| **.xlsx/.xlsm read** | Pure ZIP+XML ✔︎ | Excel COM ✔︎ | No Office needed; ms-fast. Verified on Chinese, formulas, mixed types, space-in-sheet-name. |
| **.xlsx/.xlsm write** | Hand-built OOXML, **5 parts** ✔︎ (Excel opened it) | Excel COM `SaveAs` ✔︎ | 1769 B file Excel accepts. Pure route can't evaluate formulas. |
| **.csv** | `Import-Csv`/manual; **write UTF-8 *with BOM*** ✔︎ | Excel COM | BOM mandatory or Excel mojibakes Chinese. `Export-Csv` w/o `-Encoding` = irreversible loss. |
| **.docx read** | Pure ZIP+XML, **namespace-aware** ✔︎ | Word COM | Paragraphs **and** tables; must join `<w:t>` per paragraph. |
| **.docx write** | Hand-built OOXML, **3 parts** ✔︎ (Word opened it) | Word COM | Styles/rels optional. Real formatting → template or COM. |
| **.docx → PDF** | Word COM `SaveAs2($out,17)` ✔︎ 0.3–0.6 s | `ExportAsFixedFormat($out,17)` ✔︎ | Both verified. **Always timeout + kill `WINWORD`** (F7). |
| **.pptx read** | Pure ZIP+XML, order from `p:sldIdLst` ✔︎ | PowerPoint COM ✔︎ | Never trust `slide1..N` filename order. |
| **.pptx write** | **PowerPoint COM + a template** ✔︎ | — | ⚠︎ Minimal hand-built pptx **rejected**; real deck is **44 parts**. |
| **.pptx → PDF** | PowerPoint COM `SaveAs($out,32)` ✔︎ | — | Verified: 21133 B PDF from a 2-slide deck. |
| **.pdf text** | **Windows Search IFilter** (no Office/Python) ✔︎ | ⚠︎ Word reflow (hangs) | 1147 chars Chinese from a real exam PDF, 65 from a 635 B PDF, **0.03 s**. |
| **.pdf → images** | `Windows.Data.Pdf` WinRT render → PNG ✔︎ | — | 43 KB PNG, text visually confirmed; feed to vision/RapidOCR. |

---

## 2. Concrete recipes

### 2.1 `.xlsx`/`.xlsm` read — pure PowerShell ✔︎
`Add-Type -AssemblyName System.IO.Compression.FileSystem` — `ZipFile` is in the **`.FileSystem`** assembly; `System.IO.Compression` alone gives `Unable to find type [ZipFile]` ⚠︎. Then:
1. `xl/workbook.xml` → `<sheet name= sheetId= r:id=>`. 2. `xl/_rels/workbook.xml.rels` → `r:id` → `worksheets/sheetN.xml` (**never assume sheetN order matches visible order**). 3. `xl/sharedStrings.xml` → index-ordered array; an `<si>` may hold **several `<t>`** (rich-text runs) — concatenate them all. 4. `xl/worksheets/sheetN.xml` → per `<c r="B2" t="…">`: `t="s"` → `<v>` is an **index into sharedStrings**; `t="inlineStr"` → `<is><t>`; `t="b"` → 1/0; `t="e"` → error; `t="str"` → formula string; no `t` → numeric `<v>`. **Absent cell = empty** (sparse grid). Parse `r=` for row/col (A→1, Z→26, AA→27). Formula cells carry `<f>` **plus cached `<v>`** (verified); a tool-written file may omit the cache → `null`. 5. `.xlsm` is layout-identical: verified 10 parts with `sharedStrings.xml` + `sheet1.xml`, so **the same reader works unchanged**; only the `macroEnabled` content-type override and (if macros exist) `xl/vbaProject.bin` differ — **never rewrite an .xlsm zip without carrying `vbaProject.bin`**, or the VBA is destroyed permanently.

**Minimum viable `.xlsx`, Excel opened it ✔︎** — 5 parts, 1769 B, Chinese sheet name and content: `[Content_Types].xml`, `_rels/.rels`, `xl/workbook.xml`, `xl/_rels/workbook.xml.rels`, `xl/worksheets/sheet1.xml`. **`styles.xml` NOT required.** Use `t="inlineStr"` to skip `sharedStrings.xml`. Write UTF-8 **no BOM**, declare `encoding="UTF-8"`, set `xml:space="preserve"`. Escape `& < > "` yourself (a bare `&` makes the file unopenable). Numbers must use `InvariantCulture` (a comma decimal separator corrupts the cell).

### 2.2 `.docx` read — pure PowerShell ✔︎
`word/document.xml`, walk `w:body` children **in order**: `w:p` → join **all** `.//w:t`; `w:tbl` → per `w:tr`, per `w:tc`, join `.//w:t`, then join cells. Verified: paragraphs, Chinese, a 3×3 table as `城市 | 销售额 | 增长`, and the paragraph *after* the table.

**Fragmented runs (verified):** Word splits a sentence across many `<w:r>`. My one-sentence fixture was 2 runs: `'This sentence is split across two runs '` + `'to model fragmented w:r elements.'`. A regex/`InnerText` search for the whole phrase **fails** — always join `w:t` per paragraph before searching or replacing (Anthropic ships `merge_runs.py` for exactly this). Headings: read `w:pStyle` to emit markdown levels instead of flat text.

**Namespace trap (verified, silent):** `GetElementsByTagName('sldId')` → **0 nodes**, because prefixed OOXML matches on *qualified* name (`p:sldId`). Use `SelectNodes` + `XmlNamespaceManager` or `GetElementsByTagName(local, nsUri)`. It "works" on `.xlsx` only because those parts use default namespaces.

**Minimum viable `.docx`, Word opened it ✔︎** — **3 parts** (194 chars, 16 paras, 1 table): `[Content_Types].xml`, `_rels/.rels`, `word/document.xml` (with `w:sectPr`). Adding `word/styles.xml` + `word/_rels/document.xml.rels` (5 parts) also opened ✔︎ and is needed for named styles. Legacy `.doc` is **not** ZIP → Word COM only.

### 2.3 `.pptx` read — pure PowerShell ✔︎
`ppt/presentation.xml` → `p:sldIdLst/p:sldId` **in document order** (= real slide order), each with `r:id`; resolve via `ppt/_rels/presentation.xml.rels` → `slides/slideN.xml`; collect `//a:t` (namespace-aware). Verified: `slide1: 幻灯片标题 / 要点一 / 要点二`, `slide2: Second Slide / Only one line`. Speaker notes live in `ppt/notesSlides/notesSlideN.xml` (absent unless PowerPoint wrote them). Slide tables/charts are **not** in `a:t` — walk `a:tbl`/`c:chart` separately.

### 2.4 `.pptx` write — ⚠︎ do not hand-generate
My valid-by-reading 6-part pptx was **rejected by PowerPoint**: `0x80070570 文件或目录损坏且无法读取`. A deck PowerPoint itself authored has **44 parts / 39812 B**: `slideMasters/slideMaster1.xml`, **11** `slideLayouts/*.xml` + rels, `theme1.xml`+`theme2.xml`, `notesMasters/notesMaster1.xml`, `presProps.xml`, `viewProps.xml`, `tableStyles.xml`, `docProps/thumbnail.jpeg`. → Ship a **template `.pptx`**: use PowerPoint COM, or unzip → edit `ppt/slides/slideN.xml` → rezip preserving every part and relationship. This is why Anthropic's pptx skill is template/`add_slide.py`-centric.

### 2.5 `.pdf` text — Windows Search IFilter, no Office, no Python ✔︎
The highest-value find here. `Windows.Data.Pdf` is **render-only** (reflection-verified members: `LoadFromFileAsync/LoadFromStreamAsync/GetPage/PageCount/IsPasswordProtected` and `RenderToStreamAsync/PreparePageAsync/Dimensions/Size/Rotation` — **no text API**). But `.pdf`'s persistent handler `{1AA9BF05-9A97-48c1-BA28-D9DCE795E93C}` registers an **IFilter** `{6C337B26-3E38-4F98-813B-FBA18BAB64F5}` ("Reader Search Handler") **inside `C:\WINDOWS\system32\Windows.Data.Pdf.dll`** — so PDF text extraction **does** ship with Windows. Working sequence (`.research/pdf-text-win.ps1`, 0.03 s/file):
1. `CoCreateInstance(CLSID {6C337B26-…}, IID_IFilter {89BCB740-6119-101A-BCB7-00DD010655AF})`. 2. `SHCreateStreamOnFileEx(path, STGM_READ|STGM_SHARE_DENY_WRITE, 0, false, 0, out stm)`. 3. `IInitializeWithStream::Initialize(stm, STGM_READ)` — **not** `IPersistFile`. QI probe: `IInitializeWithStream` ✔︎ `IPersistStream` ✔︎ `IFilter` ✔︎ · `IPersistFile` ✗ `E_NOINTERFACE` · `IInitializeWithFile` ✗. `query.dll!LoadIFilter()` **fails** `0x80004002` — use `CoCreateInstance`. 4. `IFilter::Init(0,0,0,out flags)`, then loop `GetChunk`/`GetText`. Chunk flags `0x1`=TEXT, `0x2`=VALUE (metadata, skip). **Accept `hr >= 0`, not `hr == 0`** — `GetText` returns **`0x00041709` `FILTER_S_LAST_TEXT`**, a *success* code that still delivered my 65 chars; testing `== S_OK` silently returns `""` (cost me two attempts). `GetChunk == 0x80041700` ends iteration.

Verified: 65 chars (635 B PDF) · **1147 chars correct Chinese** (141 KB exam PDF) · 415 chars (81 KB diagram). **Limits (observed):** flat unspaced text — **no page boundaries, no layout/coordinates**; some glyphs drop (`P(A)`→`AP`, fraction bars lost). **Scanned PDFs → 0 chars**: my 339 KB test file had `/Image: 4, /Font: 0` (image-only) and returned `GetText`→`0x80041701` with the count untouched → render + OCR/vision instead.

### 2.6 `.pdf` → PNG — `Windows.Data.Pdf` WinRT ✔︎
PS 5.1 needs the async bridge (`.research/pdf-render-winrt.ps1`):
```powershell
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$g = ([System.WindowsRuntimeSystemExtensions].GetMethods() | ? { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
$a = ([System.WindowsRuntimeSystemExtensions].GetMethods() | ? { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' })[0]
function Await($op,$t){ $k=$g.MakeGenericMethod($t).Invoke($null,@($op)); $k.Wait(-1)|Out-Null; $k.Result }
function AwaitAction($op){ $k=$a.Invoke($null,@($op)); $k.Wait(-1)|Out-Null }
[Windows.Storage.StorageFile,Windows.Storage,ContentType=WindowsRuntime] | Out-Null          # project types FIRST
[Windows.Data.Pdf.PdfDocument,Windows.Data.Pdf,ContentType=WindowsRuntime] | Out-Null
$sf   = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($full)) ([Windows.Storage.StorageFile])
$pdf  = Await ([Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($sf))     ([Windows.Data.Pdf.PdfDocument])
$page = $pdf.GetPage(0)
$st = New-Object Windows.Storage.Streams.InMemoryRandomAccessStream
$o  = New-Object Windows.Data.Pdf.PdfPageRenderOptions; $o.DestinationWidth = 1240
AwaitAction ($page.RenderToStreamAsync($st, $o))      # IAsyncAction -> the other AsTask overload
$st.Seek(0); $net = [System.IO.WindowsRuntimeStreamExtensions]::AsStreamForRead($st)   # copy to FileStream
```
Verified: 635 B PDF → **43155 B PNG**; I viewed it and it shows the PDF's exact text. Without options it renders at native size (mine: 816×1056).

### 2.7 COM specifics
**Excel** ✔︎ `New-Object -ComObject Excel.Application`; `Visible=$false`, `DisplayAlerts=$false`; `SaveAs($p,51)`=.xlsx, **`52`=.xlsm** ✔︎; `$ws.Cells.Item($r,$c).Text`. Wrote/read a real 10088 B .xlsx incl. `=SUM(B2:B3)` + Chinese. **Word** ✔︎ `Documents.Open($p,[ref]$false,[ref]$true)` (ConfirmConversions, ReadOnly); `SaveAs2($out,16)`=.docx, `17`=PDF; `ExportAsFixedFormat($out,17)`=PDF. **PowerPoint** ✔︎ `Presentations.Add(0)` (0 = no window) works headless; `Slides.Add(1,12)` (ppLayoutBlank); `SaveAs($p,24)`=.pptx, `32`=PDF; notes via `slide.NotesPage.Shapes.Item(2).TextFrame.TextRange.Text`. **Cleanup mandatory** ⚠︎: failed/killed calls leave orphan `WINWORD`/`EXCEL`/`POWERPNT` processes (**I accumulated 4 orphaned WINWORD**), and the next automation attempt blocked. Kill before and after; `Quit()`+`ReleaseComObject` in `finally`.

---

## 3. Anthropic `skills` — what upstream actually says

Repo `anthropics/skills`; skills live at **`skills/<name>/SKILL.md`** — there is **no** `document-skills/` path (every `document-skills/docx/SKILL.md`-style URL 404s ⚠︎). README calls them *"source-available, not open source"*; shipped as the `document-skills` plugin. Skills: `docx`/`pptx`/`xlsx`/`pdf` (plus `pdf/forms.md`, `pdf/reference.md`).

**Their routing tables:** `.docx` read → `pandoc -t markdown`; **edit → unzip → edit `word/document.xml` → zip** ("docx-js cannot open existing files"). `.xlsx` quick look → `markitdown`; create/edit → `openpyxl`; bulk → `pandas`; **mandatory LibreOffice recalc**. `.pptx` read → `markitdown`; edit → unzip → `ppt/slides/slideN.xml` → zip. `.pdf` → `pypdf`/`pdfplumber`.

**Library-dependent (useless without Python):** `openpyxl`, `pandas`, `python-pptx`, `pypdf`, `pdfplumber`, `reportlab`, `pytesseract`/`pdf2image`, `mammoth`, `markitdown`, `pptxgenjs`/`docx` (npm), `soffice`, `pdftoppm`. **Format-level, directly portable to our ZIP+XML engine:** the whole unzip→edit-XML→rezip workflow; `<w:ins>`/`<w:del>` tracked-change structure; `<w:delText>` vs `<w:t>`; deleted paragraph-mark semantics; `<a:p>`/`<a:pPr>`/`<a:buChar>` bullet inheritance; `xml:space="preserve"`; the six cross-linked comment parts; XSD validation (the repo vendors the full ISO/IEC 29500 XSD set — a reusable spec artifact for our own validator).

**Their `xlsx` recalc rule is the key lesson for us:** openpyxl — like *any* generator, including ours — writes formulas as strings with **no cached value**, so every formula cell reads back `None` to anything reading cached values (`pandas`, `data_only=True`, previewers); they force a LibreOffice recalc pass. **We have no LibreOffice**, so our equivalent is: write **computed values**, or write `<f>` **and** a cached `<v>` ourselves, or drive Excel COM (`$ws.Calculate()`) when Office exists. A pure-ZipFile writer can never evaluate a formula.

**"Do NOT" warnings worth copying into our skill text** (each encodes a hard-won failure):
1. Never a literal `•` — use `numbering`/`<a:buChar>`; a literal renders doubled. · 2. Never `\n` inside a run — use separate `<w:p>`/`<a:p>`. · 3. Never reorder `<p:presentation>` children — the order is load-bearing; PowerPoint then calls the file corrupt. · 4. Never reorder `<w:del/>` among other `rPr` children — schema-enforced order. · 5. Never hardcode a computed result where a formula belongs — the sheet must recalc. · 6. Never treat a clean recalc as proof of correctness — it proves formulas *evaluate*, not that they're *right* (off-by-one ranges pass silently); write 2–3 formulas and verify before building the grid. · 7. Never write non-anchor cells of a merged range — read-only. · 8. `.xlsm` loses macros unless explicitly preserved — one wrong save destroys VBA. · 9. Never leave a space-containing sheet name unquoted in a cross-sheet ref → `#VALUE!`. · 10. Never read a zero exit code as success — their `recalc.py` exits 0 *with* errors found. · 11. Verify by rendering and **actually looking** — convert → rasterize → view the images; and distrust text-fit QA for fonts whose substitute has different metrics. · 12. Never default outside the safe font list (Arial/Calibri/Cambria/Times New Roman/Courier New/Bookman Old Style/Century Schoolbook); **never default to Aptos** (missing from older Office, no metric-compatible substitute). · 13. External links die on re-save — `='[1]Returns Analysis'!$B$2` points at a *separate file on disk*; re-saving strips the cached value and recalc then writes `#NAME?`. · 14. Do pptx structural work (add/delete/reorder) **before** editing slide content, and never hand-copy a slide file — the registration bookkeeping is easy to miss. · 15. Treat third-party documents as untrusted — strip symlink entries on unpack, and parse OOXML with `defusedxml`: round-tripping through `xml.etree` rewrites namespace prefixes and corrupts the deck.

---

## 4. MarkItDown and other agents (secondary — mostly not reusable as-is)

**Microsoft MarkItDown** 📄: MIT, Python **≥3.10**, actively maintained (stable **0.1.7**, 2026-07-29; prerelease 0.1.8b2). Every converter is a Python library: `.docx`→**mammoth** (not python-docx), `.xlsx`→**pandas+openpyxl**, `.xls`→pandas+xlrd, `.pptx`→**python-pptx**, `.pdf`→**pdfminer.six+pdfplumber**, `.csv`→stdlib, `.html`→bs4+markdownify, images→external **exiftool**, audio→Google Web Speech. CLI exists (`markitdown <f> -o out.md`, `-p/--use-plugins`, `--list-plugins`; `python -m markitdown` equivalent; prefer `-o` — stdout re-encodes with `errors="replace"` and is lossy for non-ASCII). **Critical: NO standalone executable** — all **21** GitHub releases have `"assets": []`; no winget/Chocolatey/Homebrew/Scoop formula; no Microsoft Docker image (GHCR 404); **pip-only**. So it **cannot run** on our target without installing Python (here `python` is a WindowsApps stub). Third-party ports exist but none executed: `managedcode/markitdown` (C#/.NET 9, NuGet `ManagedCode.MarkItDown`; you'd build your own self-contained exe), `kelter-antunes/MarkItDownSharp`, `markitdown-ts`, `markitdown-rs`, plus GUI wrappers bundling CPython. **Worth stealing conceptually, not as code:** its mammoth→HTML→Markdown path yields real structure (headings/lists/tables) vs flat text — our ZIP+XML reader can mimic the useful part via `w:pStyle`. ⚠︎ Its README bullet "Images (EXIF metadata and OCR)" is misleading: the built-in image converter does **no OCR** (needs the separate `markitdown-ocr` plugin).

**OpenAI Codex: ships nothing for office documents** 📄 — full recursive tree (**9087 paths, `truncated: false`**) has zero pdf/docx/xlsx/pptx/office hits, and all five base-instruction/prompt files never mention binary or non-text files. Its skills are `imagegen`, `openai-docs`, `plugin-creator`, `review-agent`, `skill-creator`, `skill-installer`; the only binary-file tool is `view_image` (images only). **No prior art to copy — and no expectation a coding agent can read these formats unaided**, which is exactly the gap our skill text must fill.

**Approach taxonomy worth copying:** `goose` is the only agent with first-class document tools, implemented **in-process in Rust** (`umya_spreadsheet`, `docx_rs`, `lopdf`) with usage guidance embedded in the MCP **tool descriptions** — a good model for documenting our `runProgram` helpers. `cline` pre-extracts in the *host* (`pdf-parse`/`mammoth`/`ExcelJS`, 400 KB truncation) so the model never sees binary. `aider` passes PDFs as native multimodal attachments and has no docx/xlsx support at all. `OpenHands`' agent moved to `OpenHands/software-agent-sdk` (the old `All-Hands-AI/OpenHands` is now "Agent Canvas") and consumes Anthropic's document skills verbatim via a plugin (`github:anthropics/skills`, asserting `pptx`/`xlsx`/`docx`/`pdf` SKILL.md exist); `NousResearch/hermes-agent` vendors them in-tree.

---

## 5. Reliable vs fragile — every failure mode below was observed

**Solid** ✔︎: ZIP+XML read for xlsx/xlsm/docx/pptx · minimal xlsx (5-part) and docx (3-part) generation · CSV with BOM · IFilter PDF text · WinRT PDF→PNG · Excel/Word/PowerPoint COM *when Office is present and you clean up*.

| # | Failure mode | Detail / mitigation |
|---|---|---|
| F1 | **Excel holds an exclusive lock** ⚠︎ | With the file open in Excel, `ZipFile.OpenRead` **and** `File.Open(...,Read,None)` both fail *"being used by another process"*; a `~$name.xlsx` lock file appears. Mitigate: detect `~$*` and read a **copy**, or attach via `[Runtime.InteropServices.Marshal]::GetActiveObject('Excel.Application')`. |
| F2 | **PS 5.1 + non-ASCII without a BOM** ⚠︎ | **Three of my own scripts hit this.** With ACP `gb2312`, a UTF-8-**no-BOM** `.ps1` containing Chinese is decoded as ANSI: the parser dies with *"The '\<' operator is reserved for future use"* / *"string is missing the terminator"*, and `'北京'` becomes `'鍖椾含'`. Write any generated `.ps1` with non-ASCII as **UTF-8 *with* BOM** (or pure ASCII). Same content + BOM → exit 0. **A latent landmine for QuickScriptTool, which will emit Chinese scripts.** |
| F3 | **Execution policy `Restricted`** ⚠︎ | All scopes `Undefined` ⇒ effective `Restricted`; `& script.ps1` fails `UnauthorizedAccess` even from a working shell. Use `powershell.exe -NoProfile -ExecutionPolicy Bypass -File x.ps1` ✔︎, or pipe the body in; `-Command` bypasses `-File` policy entirely (how our `runProgram` should invoke). |
| F4 | **`Export-Csv` without `-Encoding` = data loss** ⚠︎ | Writes **ASCII**: `城市` → `??` (bytes `22 3F 3F 22`), irreversible. Always `-Encoding UTF8` (PS 5.1 then emits a BOM — which you want). |
| F5 | **CSV without BOM mojibakes in Excel** ⚠︎ | Excel read UTF-8-**no-BOM** Chinese as `鍩庡競`/`閲戦` (UTF-8 bytes as GBK); the **BOM** version showed `城市`/`金额`. Same content, same Excel. |
| F6 | **Namespace-qualified XML lookups silently return nothing** ⚠︎ | `GetElementsByTagName('sldId')` → 0 nodes for `p:sldId`. No exception, just empty. Always namespace-qualify. |
| F7 | **Word COM can hang indefinitely** ⚠︎ | `ExportAsFixedFormat` hung **>2 min** in one invocation pattern; `Documents.Open(<pdf>)` (reflow) hung **>2 min** in another. During the hang I enumerated its windows: **no dialog** (`OpusApp` present, no modal child), so it is not a suppressed prompt. Both needed `Stop-Process WINWORD`. Yet the *same* docx→PDF calls succeeded **4/4 in 0.3–0.6 s** when Word was created inside a `Start-Job` child. **Root cause NOT established** → treat all Word COM as hang-capable: child process + hard timeout + kill `WINWORD`. **The printer theory is disproven** ✔︎ — both paths work with the default printer set to `Microsoft Print to PDF` (port `PORTPROMPT:`) *and* to `OneNote (Desktop)`. **Consequence: do not rely on Word for PDF→docx reflow; the IFilter route (2.5) removes the need.** |
| F8 | **Localized Office object model** ⚠︎ | `$doc.Styles.Item('Heading 1')` threw `COMException` on Chinese Office (`标题 1`). Never index styles/bookmarks by English display name — use `WdBuiltinStyle` numeric constants (`-1` Normal, `-2` Heading1). |
| F9 | **Orphaned Office processes block later automation** ⚠︎ | 4 orphaned `WINWORD` accumulated; the next attempt blocked. Kill before/after; `Quit()`+`ReleaseComObject` in `finally`. Word also left `~$` lock files. |
| F10 | **Scanned PDFs yield zero text** ⚠︎ | Confirmed on `/Font: 0, /Image: 4` → 0 chars. Detect by "0 chars extracted", not file size; route to WinRT render + RapidOCR/vision. |
| F11 | **IFilter success codes** ⚠︎ | `FILTER_S_LAST_TEXT (0x00041709)` carries real text; `hr == S_OK` silently yields `""`. Accept `hr >= 0`; `GetChunk == 0x80041700` ends. |
| F12 | **32/64-bit COM + Office version differences** | Verified 64-bit host + 64-bit `InprocServer32` for all three apps; **`WOW6432Node` had no Office registration here**, so an x86 host gets no Office COM → keep the agent host 64-bit or fall back to ZIP+XML. `ExportAsFixedFormat` is 2007+, `SaveAs2` 2010+ (else `SaveAs`); legacy `.doc/.xls/.ppt` are OLE compound files with **no pure-PowerShell route**. Office versions other than 16.0, and 32-bit Office, are **UNVERIFIED**. |
| F13 | **Minimal pptx is not valid pptx** ⚠︎ | PowerPoint: `0x80070570` corrupt. Needs master+layouts+themes (44 parts measured). Template-based authoring only. |
| F14 | **OOXML byte-encoding traps** | Write parts as UTF-8 **no BOM** via `[System.IO.File]::WriteAllText($p,$s,(New-Object System.Text.UTF8Encoding($false)))` and declare `encoding="UTF-8"`; PS 5.1's `Set-Content -Encoding UTF8` adds a BOM that some parsers reject. |

---

## 6. Not verified (do not treat as fact)

- The IFilter CLSID `{6C337B26-…}` is verified **only** on Windows 11 build 26200. **Resolve it at runtime** from `HKLM:\SOFTWARE\Classes\.pdf\PersistentHandler` → `PersistentAddinsRegistered\{89BCB740-6119-101A-BCB7-00DD010655AB}` rather than hardcoding.
- Whether `.docx`/`.xlsx` also register usable IFilters (the same registry pattern exists) — not tested.
- Office 2007/2010/2013/2019/365 and 32-bit Office; all COM results are Office 16.0 64-bit.
- Legacy `.doc`/`.xls`/`.ppt` (OLE compound files) — not attempted; no pure route identified.
- Encrypted/password-protected PDFs (only `IsPasswordProtected` was read).
- `Windows.Data.Pdf` on Windows Server / N editions without the Media Feature Pack.
- PDF **writing/editing** (merge/split/watermark): no pure-Windows route found. I hand-built a valid 635 B PDF in pure PowerShell (`.research/make-minimal-pdf.ps1`) — fine for trivial output, but `Windows.Data.Pdf` cannot author PDFs and there is **no** general pure-Windows PDF writer.
- MarkItDown ports (none executed); those claims are raw repo/README reads.

---

## 7. Sources

**Anthropic skills:** [repo](https://github.com/anthropics/skills) · [README](https://raw.githubusercontent.com/anthropics/skills/main/README.md) · [docx](https://raw.githubusercontent.com/anthropics/skills/main/skills/docx/SKILL.md) · [pdf](https://raw.githubusercontent.com/anthropics/skills/main/skills/pdf/SKILL.md) · [pdf/reference](https://raw.githubusercontent.com/anthropics/skills/main/skills/pdf/reference.md) · [pdf/forms](https://raw.githubusercontent.com/anthropics/skills/main/skills/pdf/forms.md) · [pptx](https://raw.githubusercontent.com/anthropics/skills/main/skills/pptx/SKILL.md) · [xlsx](https://raw.githubusercontent.com/anthropics/skills/main/skills/xlsx/SKILL.md) · [tree API](https://api.github.com/repos/anthropics/skills/git/trees/main?recursive=1)

**MarkItDown:** [repo](https://github.com/microsoft/markitdown) · [README](https://raw.githubusercontent.com/microsoft/markitdown/main/README.md) · [pyproject](https://raw.githubusercontent.com/microsoft/markitdown/main/packages/markitdown/pyproject.toml) · [releases](https://api.github.com/repos/microsoft/markitdown/releases) · [PyPI](https://pypi.org/pypi/markitdown/json) · [managedcode port](https://github.com/managedcode/markitdown) · [MarkItDownSharp](https://github.com/kelter-antunes/MarkItDownSharp)

**Other agents:** [codex tree](https://api.github.com/repos/openai/codex/git/trees/main?recursive=1) · [aaif-goose/goose](https://github.com/aaif-goose/goose) · [cline](https://github.com/cline/cline) · [OpenHands/software-agent-sdk](https://github.com/OpenHands/software-agent-sdk) · [NousResearch/hermes-agent](https://github.com/NousResearch/hermes-agent) · [kolega-ai/kolega-skills](https://github.com/kolega-ai/kolega-skills) · [lovesickness111/AI-office-skill](https://github.com/lovesickness111/AI-office-skill)

**Windows APIs:** [Windows.Data.Pdf](https://learn.microsoft.com/en-us/uwp/api/windows.data.pdf) · [RenderToStreamAsync](https://learn.microsoft.com/en-us/uwp/api/windows.data.pdf.pdfpage.rendertostreamasync) · [IFilter](https://learn.microsoft.com/en-us/windows/win32/api/filter/nn-filter-ifilter) · [IFilter::GetText](https://learn.microsoft.com/en-us/windows/win32/api/filter/nf-filter-ifilter-gettext) · [LoadIFilter](https://learn.microsoft.com/en-us/windows/win32/api/query/nf-query-loadifilter) · [IInitializeWithStream](https://learn.microsoft.com/en-us/windows/win32/api/propsys/nn-propsys-iinitializewithstream) · [SHCreateStreamOnFileEx](https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shcreatestreamonfileex) · [ZipFile](https://learn.microsoft.com/en-us/dotnet/api/system.io.compression.zipfile) · [about_Execution_Policies](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_execution_policies)

**Working code from this survey** (all executed, all in `.research/`): `pdf-text-win.ps1` (IFilter PDF text) · `pdf-render-winrt.ps1` (PDF→PNG) · `make-minimal-docx.ps1` (3-/5-part docx) · `make-minimal-pdf.ps1` · `word-printer-hypothesis.ps1`, `ppt-author-test.ps1`, `final-checks.ps1` (COM matrix) · fixtures in `.research/fixtures/`.
