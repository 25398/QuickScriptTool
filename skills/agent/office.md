# Skill: 办公文档（section=office）

> 用途：**读写 Excel / Word / PPT / PDF 这些办公文件**。原则和 `section=command` 一样——
> 能用一条命令或一个工具做完的，绝不开软件用鼠标逐格模拟。
> 归属：产品 Skill（`skills/agent/office.md` → 打包后 `AppDir()\skills\agent\office.md`）。
> ★改本文件必须同步 `src/ai_action_router.cpp` 的内嵌文本 `MacroActionOfficeSkill()`。

## 0. 先想一遍路线（这张表就是全部）

| 要做什么 | 走哪条 | 说明 |
|---|---|---|
| **读** xlsx/xlsm/xls/csv/docx/doc/pptx/ppt/pdf/txt | **`readDocument(path)`** | 直接返回正文；表格是「制表符分隔、一行一记录」；PDF 扫描件自动渲染成图片给你看 |
| **写/建** xlsx（真 Excel 文件） | `runCommand` + Excel COM | 见下面 §2 配方 A |
| **写** CSV（Excel 能直接打开） | `runCommand` + PowerShell | 见配方 B，最快、无 Office 也行的路线 |
| 改既有 xlsx 的某个单元格 | `runCommand` + Excel COM | 打开 → 改 cells → Save（**不要**用界面点） |
| 生成 Word 文档 | `runCommand` + Word COM | 配方 C（可顺便 `ExportAsFixedFormat` 出 PDF） |
| 生成 PPT | `runCommand` + PowerPoint COM | 配方 D |
| 读页面截图里的数据（网页/软件界面） | `locateAndClick` / `observePage` + `saveTaskData` | 这是视觉路线，见 section=agent |
| 用户明确要求「看着界面操作某个 Office 软件」 | 界面路线 | 只有这种时候才逐格输入（见 section=agent 的配方复用） |

**判断口诀**：数据的**来源和去处都是文件** → 命令行/readDocument；**来源在屏幕上** → 视觉读一次
（`saveTaskData` 存下来）→ 之后全部走命令行。

## 1. readDocument —— 先读，别抄

```jsonc
{ "path": "C:\\Users\\x\\Desktop\\历史记录.xlsx" }              // 默认最多 6000 字
{ "path": "C:\\temp\\report.pdf", "pages": 5, "maxChars": 12000 } // PDF/PPT 多页
```

- 返回：`已读取 Excel 工作簿「…」（引擎 excel-com）` + `---- 正文 ----` + 内容。
- 表格格式：**第一行常见是表头**，列之间是 `\t`，一行一条记录 → 可直接喂给 CSV/配方。
- 引擎含义：`excel-com`/`word-com`（装了 Office，值最准）、`ooxml`（没装 Office，直读
  zip+XML，公式只看到缓存值/可能缺失）、`pdftotext`（有 poppler 时最快）、
  `pdf-render`（PDF 无文本层 → 已渲染成图片，宿主会把图发给你）、`text`（纯文本/CSV）。
- 读不到内容时不要死磕：`openFile` 打开后用截图+视觉读，或先 `runCommand` 转成文本。
- **禁止**为了「看看里面有什么」去 `openFile` + 截图 + 识图抄数字：慢几十倍而且会抄错。

## 2. 写文件的配方（照抄即可，路径先 `resolveSystemPath` 取绝对路径）

```powershell
# A. 真 xlsx（装了 Office）：值最准、能设格式
$p = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Edge历史记录.xlsx'
$xl = New-Object -ComObject Excel.Application
$xl.Visible = $false; $xl.DisplayAlerts = $false
$wb = $xl.Workbooks.Add(); $ws = $wb.Worksheets.Item(1)
$ws.Cells.Item(1,1) = '序号'; $ws.Cells.Item(1,2) = '标题'
$rows = @(@(1,'费用中心-火山引擎'), @(2,'哔哩哔哩'))
$r = 2
foreach ($row in $rows) { $ws.Cells.Item($r,1) = $row[0]; $ws.Cells.Item($r,2) = $row[1]; $r++ }
$wb.SaveAs($p, 51)          # 51 = xlsx
$wb.Close($false); $xl.Quit()
[void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($wb)
[void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($xl)

# B. CSV（没装 Office 也能用；Excel 双击就能开）
$p = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Edge历史记录.csv'
$rows = @('序号,标题,网址,时间', '1,费用中心-火山引擎,console.volcengine.com,18:24')
$rows | Out-File -Encoding utf8 $p        # ★必须 utf8，否则中文在 Excel 里乱码

# C. Word 文档（可选：顺便导出 PDF）
$w = New-Object -ComObject Word.Application
$w.Visible = $false; $w.DisplayAlerts = 0
$d = $w.Documents.Add()
$d.Content.Text = "标题`r`n正文……"
$docx = Join-Path ([Environment]::GetFolderPath('Desktop')) '报告.docx'
$d.SaveAs($docx)
$d.ExportAsFixedFormat(($docx -replace '\.docx$', '.pdf'), 17)   # 17 = PDF
$d.Close($false); $w.Quit()

# D. PPT（WithWindow=$false 才能无窗口跑）
$ppt = New-Object -ComObject PowerPoint.Application
$pres = $ppt.Presentations.Add($false)
$slide = $pres.Slides.Add(1, 12)          # 12 = ppLayoutBlank
$slide.Shapes.AddTextbox(1, 40, 40, 600, 80).TextFrame.TextRange.Text = '第一页标题'
$pres.SaveAs((Join-Path ([Environment]::GetFolderPath('Desktop')) '演示.pptx'))
$pres.Close(); $ppt.Quit()

# E. 把已有 xlsx/docx 转 PDF
$wb = (New-Object -ComObject Excel.Application).Workbooks.Open($xlsxPath)
$wb.ExportAsFixedFormat(0, $pdfPath)      # 0 = xlTypePDF
$wb.Close($false); $wb.Parent.Quit()
```

## 3. 硬约定（踩过的坑，务必遵守）

> 下面每一条都在本机实测过（依据：`.research/office-document-io-report.md`）。

1. **中文 CSV 必须写 BOM**。实测同一个文件：无 BOM 时 Excel 显示 `鍩庡競`/`閲戦`（乱码），
   带 BOM 才显示 `城市`/`金额`。`Out-File -Encoding utf8`（PowerShell 5.1）会写 BOM ✓；
   若在 PowerShell 7 下跑则不写 BOM，稳妥写法：
   `[System.IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding($true)))`。
   **`Export-Csv`/`ConvertTo-Csv` 不写 `-Encoding UTF8` 会输出 ASCII** → 中文直接变成 `??`（不可逆）。
2. **中文脚本必须存成「UTF-8 带 BOM」**。本机 ANSI 代码页是 GB2312：UTF-8 无 BOM 的 .ps1
   会被按 ANSI 解析，中文变 `鍖椾含`，甚至报「'<' 运算符保留给将来使用」。
   宿主生成的临时脚本已按 BOM 写；你自己造 .ps1 时注意这一点。
3. **COM 必须收尾**：`$wb.Close($false)` + `$xl.Quit()` + `ReleaseComObject`。
   实测漏掉会累积孤儿 WINWORD/EXCEL 进程，随后锁住文件、后续自动化全部失败。
4. **Word/Excel COM 可能挂住**（实测 `ExportAsFixedFormat` 与 `Documents.Open(pdf)` 各挂过 >2 分钟，
   枚举窗口看不到任何对话框 —— 不是被抑制的提示框）。所以：
   - 任何 COM 调用都要有超时与兜底（宿主 `readDocument` 内置 60s 超时并会杀进程）；
   - **不要用 Word 打开 PDF 来转换**（挂 + 可能留卡死 WINWORD）；要 PDF 用
     `ExportAsFixedFormat(路径, 17)`，要读 PDF 用 `readDocument`（走 Windows 自带 IFilter / 渲染图片）。
5. **文件被打开时读不了**：Excel 会独占锁，`ZipFile.OpenRead` 和 `File.Open(Read)` 都会报
   「being used by another process」（同目录还会出现 `~$` 锁文件）。要么先关闭，要么把文件复制一份再读，
   要么附着到已开实例（`[Runtime.InteropServices.Marshal]::GetActiveObject('Excel.Application')`）。
6. **公式需要重算才有值**：程序生成的 xlsx 里公式单元格没有缓存值，直读会得到空。
   要么写**计算好的值**，要么 `<f>` 与 `<v>` 都写，要么用 COM `$ws.Calculate()`。
7. **XML 必须按命名空间查**：`GetElementsByTagName('sldId')` 对 `p:sldId` 返回 **0 个节点且不报错**。
   （宿主脚本统一用 `local-name()` 查询规避。）
8. **Word 会把一句话拆成多个 `<w:r>`**（实测一句拆 2 段）。读文本要按段落合并 `w:t` 再判断/替换。
9. **老格式 `.xls/.doc/.ppt` 只能靠 COM**（OOXML 只覆盖新格式）；`readDocument` 已处理。
10. **中文版 Office 的对象模型是本地化的**：`Styles.Item('Heading 1')` 在中文 Office 上会抛错
    （要用 `标题 1`）。样式请用 `WdBuiltinStyle` 的数字常量。
11. **`.pptx` 不能用「最小 OOXML」手搓**：实测 6 个 part 的 pptx 被 PowerPoint 拒绝（0x80070570），
    真实 pptx 有 44 个 part（slideMaster/版式/主题/notesMaster…）。要生成 PPT 就用
    PowerPoint COM（见配方 D），或从模板复制。
12. **`.xlsx` / `.docx` 可以手搓最小 OOXML**（实测各 5 个 / 3 个 part，Excel/Word 都能打开）——
    但只在没有 Office 的机器上才值得这么做；有 Office 时优先 COM。
13. **读取的引擎与局限**（`readDocument` 会在结果里写明引擎）：
    - `pdf-ifilter`：Windows 自带搜索过滤器，**无 Office 无 Python** 也能抽文本（实测 1147 字中文 0.03s），
      但**没有版式与分页**，扫描件抽不到字；
    - `pdf-render`：扫描件 → 渲染成 PNG 交给你看图（多模态）；
    - `ooxml`：直读 zip+XML，快、无 Office 依赖，但公式只有缓存值；
    - `excel-com`/`word-com`：装了 Office 时值最准（公式结果、日期）；
    - 大文件先 `pages`/`maxChars` 限量；几十万行的表要先用命令行聚合再写结果。

## 4. 与脚本/回放的关系

所有配方都通过 `runCommand` 落到既有的「运行程序」动作（`runProgram` + `inputText`），
所以在录制、逻辑转化、脚本回放里都是**同一条可编辑的动作**：路径与参数都能在编辑器里改，
不需要用户重新点一遍界面。
