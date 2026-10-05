# Skill: 办公文档（section=office）

> 用途：**读写 Excel / Word / PPT / PDF 这些办公文件**。原则和 `section=command` 一样——
> 能用一条命令或一个工具做完的，绝不开软件用鼠标逐格模拟。
> 归属：产品 Skill（`skills/agent/office.md` → 打包后 `AppDir()\skills\agent\office.md`）。
> ★改本文件必须同步 `src/ai_action_router.cpp` 的内嵌文本 `MacroActionOfficeSkill()`。

## 0. 先想一遍路线（这张表就是全部）

| 要做什么 | 走哪条 | 说明 |
|---|---|---|
| **读** xlsx/xlsm/xls/csv/docx/doc/pptx/ppt/pdf/txt | **`readDocument(path)`** | 直接返回正文；表格是「制表符分隔、一行一记录」；PDF 扫描件自动渲染成图片给你看 |
| ★**写/新建 xlsx**（**不需要装 Office**） | **`writeSpreadsheet`** `mode=create` | 见 §2.0。给 `tsv`（制表符分隔）即可；数字自动识别 |
| ★**改已有 xlsx 的单元格**（**图表/公式/样式原样保留**） | **`writeSpreadsheet`** `mode=setCells` | 见 §2.0。只动你点名的格子；改前自动备份 |
| **写** CSV（Excel 能直接打开） | `runCommand` + PowerShell | 见配方 B，最快、无 Office 也行的路线 |
| 需要 Excel 的**本地化格式化**（千分位、货币符号、按区域显示的日期） | `runCommand` + Excel COM | 见配方 A。⚠ COM 要求装了 Office；另存会丢图表。**日期本身不用走 COM 了** —— `readDocument` 现在会把日期还原成 `2024-01-01` 这种 ISO 串 |
| 生成 Word 文档 | `runCommand` + Word COM | 配方 C（可顺便 `ExportAsFixedFormat` 出 PDF） |
| 生成 PPT | `runCommand` + PowerPoint COM | 配方 D |
| ★**拿浏览器历史/书签**（最近访问了什么、导出浏览记录、查某网址访问时间） | **`readBrowserHistory`** | 见 §1.5。**一步到位**：不开浏览器、不截图、不抢前台，返回**完整**标题+URL+精确时间 |
| 读页面截图里的数据（网页/软件界面，且**不是**浏览历史） | `locateAndClick` / `observePage` + `saveTaskData` | 这是视觉路线，见 section=agent |
| 用户明确要求「看着界面操作某个 Office 软件」 | 界面路线 | 只有这种时候才逐格输入（见 section=agent 的配方复用） |
| 用户**自己**配了外部 MCP server（`mcp_servers.json`） | 用它（`mcp__<server>__*`） | 工具表里有就用；**没有就说明用户没配** —— 直接回上面的配方，不要试图去启动任何第三方程序 |

**判断口诀**：数据的**来源和去处都是文件** → 命令行/readDocument；**来源在屏幕上** → 视觉读一次
（`saveTaskData` 存下来）→ 之后全部走命令行。

## 1. readDocument —— 先读，别抄

```jsonc
{ "path": "C:\\Users\\x\\Desktop\\历史记录.xlsx" }              // 默认最多 6000 字
{ "path": "C:\\temp\\report.pdf", "pages": 5, "maxChars": 12000 } // PDF/PPT 多页
```

- 返回：`已读取 Excel 工作簿「…」（引擎 excel-com）` + `---- 正文 ----` + 内容。
- 表格格式：**第一行常见是表头**，列之间是 `\t`，一行一条记录 → 可直接喂给 CSV/配方。
- 引擎含义：`excel-com`/`word-com`（装了 Office，值最准）、**`ooxml-native`**（我们自己的引擎直读
  zip+XML：**不起外部进程、不需要 Office**，xlsx 默认走它）、`ooxml`（老的脚本回退路线，
  只在原生路径读不了时才落到这里）、`pdftotext`（有 poppler 时最快）、
  `pdf-render`（PDF 无文本层 → 已渲染成图片，宿主会把图发给你）、`text`（纯文本/CSV）。
- ★**日期已经能正确读了**：Excel 把日期存成序列号（2024-01-01 = 45292），
  原生引擎会按单元格的数字格式还原成 `2024-01-01` / `12:00:00` / `2024-01-01 18:00:00`（ISO）。
  ⚠ **百分比等其它格式给的是原始数值**（`0.12` 就是 12%）—— 那是真值，算数用它对。
  ⚠ 公式单元格：有缓存值就给值，**没有缓存值就写成 `=公式原文`**（告诉你「这里有个公式」，
  而不是给你一个空格子）。
- 读不到内容时不要死磕：`openFile` 打开后用截图+视觉读，或先 `runCommand` 转成文本。
- **禁止**为了「看看里面有什么」去 `openFile` + 截图 + 识图抄数字：慢几十倍而且会抄错。

## 1.5 readBrowserHistory —— 拿浏览记录/书签（★别去 edge://history 抄）

```jsonc
{ "kind": "history", "limit": 10 }                 // 最近 10 条（默认 20）
{ "kind": "history", "browser": "edge", "limit": 50 }  // 指定浏览器
{ "kind": "bookmarks", "limit": 100 }              // 书签
{ "kind": "browsers" }                             // 先看有哪些浏览器/profile
```

- 返回：**制表符分隔、一行一条**（`序号 时间 访问次数 标题 URL`）→ 可直接配 `writeSpreadsheet`
  的 `tsv` 或 CSV 配方，**不需要任何中间转换**。
- 时间已转成**本地时间** `2026-09-23 21:25:27`（库里存的是「1601 年起微秒数」，别自己换算）。
- ★★**为什么不能用别的方式**（实测数据，不是理论）：
  - `observePage` 在 `edge://` / `chrome://` 上**必然失败** —— 内置页**禁止扩展注入
    content script**。重试没有任何意义（扩展会直接回一句「只能走 UIA」）。
  - 内置页上的**列表内容截图必然被截断**：标题会变成省略号，`zoom` 放大只是
    把省略号放大。所以「截图 + 抄」这条路**从原理上就会抄错**。
  - 实测 UI 路线（observePage → 失败 → listUiControls → zoom × 3 → 抄）：**8+ 轮、23.6 秒**；
    `readBrowserHistory`：**1 轮、亚秒级**。
- ★返回已是完整数据，**不要再 zoom / 截图「核对」**（核对对象本身是残缺的，纯烧 token）。
- **浏览器开着也能读**（按只读共享方式打开，不用先关浏览器）。
- 边界：只支持 Edge / Chrome（Chromium 系）。Firefox / 360 / QQ 浏览器库格式不同，不支持 ——
  那种情况下才回退到 UIA（`listUiControls(typeFilter=ListItem)`），**仍然不要截图**。

## 2.0 writeSpreadsheet —— 写 xlsx（**不需要装 Office**）

这是**首选**的写 xlsx 路线：我们自己的引擎直接读写 OOXML，不依赖 Office、不依赖任何第三方程序。

```jsonc
// 新建（默认拒绝覆盖已存在文件；要覆盖显式传 overwrite=true）
{ "path": "C:\\Users\\<你>\\Desktop\\报表.xlsx", "mode": "create",
  "sheet": "销售", "tsv": "品名\t金额\n键盘\t199.5\n鼠标\t89" }

// 改已有文件的指定单元格（**只动这几个格子**）
{ "path": "C:\\Users\\<你>\\Desktop\\报表.xlsx", "mode": "setCells",
  "cells": [ {"ref":"B2","number":250},
             {"ref":"C1","formula":"SUM(B2:B3)","cached":"339"},
             {"ref":"D1","text":"备注"} ] }
```

要点：

- 路径先用 `resolveSystemPath(folder=desktop)` 取**真实**桌面路径再拼文件名。
  可写范围：脚本/录制/图片/文档/技能目录 **+ 桌面 / 文档 / 下载**。
- **`mode=setCells` 是字节保留式的**：只改你点名的 `<c>` 元素，文件里的
  **图表 / 公式 / 条件格式 / 样式 / 别的表一字不动**。这正是 COM「另存一份」做不到的。
  ⚠ 改前会自动**备份**原始文件到 `AppDir()\agent_changes\xlsx_backup_*.xlsx`，
  返回里会给路径 —— 要还原就把它复制回去。（xlsx 是二进制，撤销日志装不下，所以走备份。）
- **`tsv` 的数字识别**：整串看起来是数字就当数字（这样 `SUM` 才算得动）；
  **带前导 0 的（`007`）保持文本**（否则编号/手机号会被吃掉前导零）。
  想强制文本就别让它看起来像数字（或改用 `setCells` + `text`）。
- **公式不会被计算**：`formula` 只写公式本身。`cached` 是给「直读」用的缓存值 ——
  **不传的话 Excel 打开时会自己重算，但直读（含 `readDocument`）在打开前拿不到值**。
  要立刻有值就自己算好塞进 `cached`。
- 写完工具会把该表**读回来**给你核对（不用再调一次读工具）。
- 老格式 `.xls` **不支持**（二进制老格式）；先让用户用 Excel 另存为 `.xlsx`。

**什么时候不用它**：需要 Excel 的**格式化显示值**（日期显示成 `2024-01-01`、千分位、货币符号）
—— 我们直读给的是原始值（日期是序列号）。这种需求走 §2 配方 A 的 COM。

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

## 2.5 关于第三方办公引擎（**我们不依赖**）

> 这里原来有一整节「GenOffice 路线」（推荐助手优先用它来读写 xlsx/docx/pptx）。**已删除**。

**为什么不走这条路**（产品原则）：本产品**不依赖任何别人的软件运行**。把「写 Office 文件」
这项能力外包给一个第三方 CLI，会带来三个后果：对方没装就能力缺失；对方版本不同就行为不同；
它的子命令清单还得跟着对方发版维护。这是**隐式依赖**，不是「可选的扩展点」。

**正确做法**：借鉴它的**做法与思路**，在**我们自己的引擎里实现**。值得借鉴的三条（与具体项目无关）：

1. **工具层不调模型** —— 文档的读写是确定性的，模型只负责编排与决策。
2. **字节保留式编辑** —— 改既有文件时只重写被改动的部分，其余原样搬运 ⇒
   公式/图表/透视表/样式**原样存活**（而不是「另存一份，功能全丢」）。
3. **产物可验收** —— 每次操作后能用**确定性**方式验证（读回单元格）+ 必要时渲染成图给模型看。

自研实现见 `src/ooxml/`（容器层：inflate + 逐条目原样搬运的 zip 读写；文档层在其上）。
**在自研层可用之前**，写文件就走 §2 的 COM / CSV 配方；不要去找任何第三方程序代劳。

**如果用户自己配了**外部 MCP server（`mcp_servers.json`），工具表里会出现 `mcp__<server>__*` ——
那是**用户显式配置**的、厂商中立的扩展点，可以用；但**不要**去探测、启动或推荐任何具体软件。

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
