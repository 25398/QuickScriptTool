# Skill: 命令行路线（section=command）

> 用途：**凡是不需要「看见界面」的活，一律用 `runCommand` 一条命令做完**，不要用定位/点击逐格模拟。
> 归属：这是**产品 Skill**——`skills/agent/` 就是打包后 `AppDir()\skills\agent\` 的来源，
> 不是编辑器（Cursor）的规则文件。运行时读取顺序：`AppDir()\skills\agent\command.md`
> → `AppDir()\skills\command.md` → 内嵌兜底（`MacroActionCommandSkill()`，供 AI 动作执行
> 走 `lookupMacroAction(section=command)`）。
> ★改本文件时必须同步 `src/ai_action_router.cpp` 的内嵌文本 `MacroActionCommandSkill()`
> （AI 动作执行那条链路读内嵌版，不读文件）。
> 实测依据：同一个「抄浏览器历史 → 桌面建 Excel」任务，GUI 路线要 17 轮 API + 约 99 个合成按键；
> 命令路线 2~3 轮（写 CSV/JSON → 完事）。

## 1. 先做路线判断（每次任务开头想一遍）

> ★这条规则放在 Skill 而不是系统提示词里：系统提示词每一轮都要付费，而路线判断只在一部分
> 任务里相关。宿主会在**相关时机**把指针塞进工具结果（要用配方逐格填表 / 打开表格软件 /
> 在表格里输入时各一次，见 `AiRouteNudgeOnce`），你看到指针就来查本节。
> 如果你在任务开头就判断出这是「写数据」类任务，直接照下表走即可。

| 任务性质 | 路线 | 工具 |
|---|---|---|
| 读写/生成文件（Excel/CSV/JSON/文本）、批量改名/整理、统计与转码、算数、查/写注册表、HTTP 取数 | **命令行** | `runCommand(shell=powershell\|cmd, command=...)` |
| 打开程序 / 网址 / 文件 | 既有动作 | `runProgram` / `openWebpage` / `openFile` |
| 浏览器内置页（历史/下载/设置/扩展） | 让浏览器自己带 URL 打开 | `openWebpage("edge://history")`（宿主展开为 `runProgram <浏览器> <url>`，**不合成键**） |
| 网页里的内容与按钮 | 扩展 DOM | `observePage` → `clickRef` / `typeByLabel` / `searchOnPage` |
| 浏览器外壳（地址栏/标签/「…」菜单）与桌面软件控件 | UIA | `listUiControls` → `invokeUiControl(name)` |
| 自绘界面 / 游戏 / UIA 枚举不到 | 视觉兜底 | `locateAndClick` |

判断口诀：**结果落在文件/数据里 → 命令；结果落在界面状态里 → 定位点击。**

> ★踩过的坑（务必避开）：内置页早期是用「Ctrl+L → 打字 → Enter」合成键打开的，
> 实测**首字母会被吞**：`edge://history` 打成 `dge://history` → 浏览器当成搜索词 →
> 进了必应搜索页，而模型以为历史记录已经打开（「本应输到地址栏却输到搜索栏」）。
> 现在宿主改为让浏览器自己带 URL 打开；如果你在日志里看到标题变成「xxx - 搜索」，
> 说明 URL 又被打进了搜索栏，别重复 Ctrl+L/Ctrl+H，用 `runProgram(edge, <url>)` 重来。

## 2. 工具调用形态（照抄即可）

```jsonc
// 基本形态（默认 powershell、隐藏窗口、启动后等 1500ms）
{ "command": "Get-Date" }

// 指定 cmd / 等待时间 / 是否隐藏窗口
{ "shell": "cmd", "command": "dir /b", "waitMs": 3000 }
{ "command": "Start-Process notepad", "hidden": false }
```

- `shell`：`powershell`（默认）或 `cmd`。
- `waitMs`：启动后等待毫秒（默认 1500，上限 60000）。命令很快时可传 0。
- `hidden`：默认 true，用隐藏窗口执行，**不抢前台焦点**（后面还要点界面时很重要）。
- 命令正文里 **有换行或双引号时必须转义**（`\n`、`\"`）；宿主对非法 JSON 有兜底恢复，
  但不要依赖它——能写单行就写单行。

## 3. 三条硬约定（踩过的坑）

1. **路径必须绝对**。桌面路径先 `resolveSystemPath(folder="desktop")` 拿，别写 `~/Desktop`。
   中文路径用引号包住：`'C:\Users\x\Desktop\记录.csv'`（PowerShell 单引号最稳）。
2. **`runCommand` 不回显 stdout**。需要结果就自己写文件，再用 `readAgentFile` / `readTaskData` 读：
   ```powershell
   Get-ChildItem 'C:\Users\x\Desktop' | Select-Object Name | Out-File -Encoding utf8 'C:\temp\ls.txt'
   ```
3. **不要用它开 GUI 程序去等界面**（那是 `runProgram` + 观察的事）；
   也不要用它做需要人工确认/管理员权限的操作（宿主不提权，UAC 会弹窗打断脚本）。

## 4. 典型配方（本产品场景）

```powershell
# A. 桌面写一份 CSV（Excel 可直接打开；避免装 Office 依赖）
$p = 'C:\Users\x\Desktop\记录.csv'
'序号,标题,网址,时间' | Out-File -Encoding utf8 $p
'1,示例,example.com,22:18' | Out-File -Encoding utf8 -Append $p

# B. 真 xlsx（机器上装了 Excel/COM 时）
$xl = New-Object -ComObject Excel.Application
$wb = $xl.Workbooks.Add(); $ws = $wb.Worksheets.Item(1)
$ws.Cells.Item(1,1) = '序号'; $ws.Cells.Item(1,2) = '标题'
$wb.SaveAs('C:\Users\x\Desktop\记录.xlsx'); $wb.Close(); $xl.Quit()

# C. 批量改名（前缀统一）
Get-ChildItem 'C:\dir\*.png' | Rename-Item -NewName { 'img_' + $_.Name }

# D. 统计 / 筛选（结果写文件再读）
(Get-Content 'C:\temp\raw.txt' | Select-String 'keyword').Count | Out-File 'C:\temp\n.txt'

# E. 读取剪贴板 / 写入剪贴板
Get-Clipboard | Out-File -Encoding utf8 'C:\temp\clip.txt'
'hello' | Set-Clipboard

# F. HTTP 取数（不要用浏览器打开 api 接口）
Invoke-RestMethod 'https://example.com/api' | ConvertTo-Json | Out-File -Encoding utf8 'C:\temp\a.json'
```

```bat
:: cmd 侧：简单文件操作 / 老式工具
dir /b > "%TEMP%\ls.txt"
copy /y "a.txt" "b.txt"
```

## 5. 与脚本、逻辑转化的关系

- `runCommand` **落到既有的「运行程序」动作**（`runProgram` + `inputText` 运行参数，`shortcutPreset=0`），
  所以它在录制/逻辑转化里就是一个普通动作，可回放、可在编辑器里改程序路径与参数。
- 因此**能用 `runCommand` 完成的步骤，都不要用「逐格输入 + 保存对话框」去模拟**：
  前者回放稳定、不受分辨率/皮肤影响；后者依赖界面，容易点错。
- 需要人工检查的半自动脚本：把命令写进脚本动作（运行程序），参数留成宏变量即可。

## 6. 安全边界（当前实现，务必知道）

- 命令以**当前用户权限**运行，无沙箱、不提权；
- 不会有确认弹窗（除非任务本身来自「刚抓网页再启动程序」那条链路）；
- 因此：**不要生成删除/覆盖用户数据、改系统设置、下载并执行未知程序的命令**；
  涉及这些动作时先 `completeTask` 说明需要用户确认，或改用界面操作让用户看得见。
