---
name: agent-script
description: >-
  脚本生成规范（scriptStrategy）：先 planScriptActions 规划动作树（children 嵌套，像写代码），
  再 buildScriptActions + createMacroScript 补齐细节；杜绝手写 JSON；循环体必须在循环内。
---

# 脚本生成规范（scriptStrategy）

## 铁律

1. **必须用工具生成，杜绝手写 JSON**：含循环/条件时先 `planScriptActions` 核对动作树，
   再 `buildScriptActions` 校验（自动补序号、标准字段、末尾 stopMacro），
   再 `createMacroScript` 保存到 scripts（默认不分类，用户指明目录时传 `folder`）。
   禁止直接在 `writeScript` 或回复里手写动作 JSON。
2. **动作是树，不是平铺列表**：`loop` / `if` / `else` / `defineBlock` 像代码块。
   块内语句必须放在该动作的 `children` 数组里（工具会展开为 indent=父级+1）。
   **禁止**把循环体写成循环后面的同级动作——那会变成「空循环 + 循环外只跑一次」。
3. **必填参数**（缺了会构建失败）：
   keyClick/keyDown/keyUp→`keyText`；quickInput→`inputText`；wait→`duration`；
   findImage→`imagePath`；textRecognition→`imagePath` 或 `ocrSearchText`；
   if→`conditionExpr`；goto→`gotoStepExpr`；defineBlock/runBlock→`blockName`；
   runMacro/mousePlayback→`targetPath`；openFile/runProgram/openWebpage/closeProgram/
   activateWindow→`targetPath`；AI 动作→`aiPrompt`。
   runMacro/runBlock/mousePlayback 可选 `clickCount`/`duration`/`randomDuration`
   （整段目标重复；间隔是两遍之间；count=1 不等待）。mousePlayback 另可选 `playbackSpeed`
   （0.25~4，缺省 1；嵌套录制只用此字段，不叠加设置全局倍速）。mousePlayback 界面名是「运行录制回放」。
4. **支持魔法变量**（引擎在**运行时**自动求值，文本 `inputText`、`aiPrompt`、
   条件、goto 等所有变量位置可用）：
   - 剪贴板：界面下拉只有 `{ctrl:Clipboard()}`（条件里非空为 1、空为 0；
     快捷输入展开为文本或全部文件路径；AI 图片分析/动作执行可附图）。
     手写 `{clipboard}` 仍只取纯文本（要比对原文时用）。
   - 时间魔法（不下拉，手写才引用）：`{Now}`、`{time:格式}`、`{date:格式}`；
     条件里小时/分钟用 `ctrl:Hour()` / `ctrl:Minute()`（这两项在下拉里）；
   - 随机数：`{random:1,100}`（闭区间整数，适合输入/坐标的人性化变化）；
   - 宏运行次数：`{ctrl:CurLoops()}`（固定变量，当前宏从头执行的第几次，从 1 开始）；
   - 随机固定变量：`{ctrl:Random()}`（固定变量，每次引用随机取 1~100 整数；
     配合「条件-如果」可做随机分支，如 `ctrl:Random() > 50`）；
   - 鼠标：`{cursor.x}` / `{cursor.y}`（当前鼠标屏幕坐标）；
   - 屏幕：`{screen.w}` / `{screen.h}`（主屏宽高）；
   - 用户名：`{username}`（当前 Windows 用户名，可拼 `C:\Users\{username}\Desktop`）。
   需要当前时间/剪贴板内容时优先用上述固定/魔法变量，不要再留占位文本让用户替换。
   脚本变量（如 `{var}`、`{var.matchData}`）照常可用。
5. 只有确认 `createMacroScript` 返回「✓ 鼠标宏已创建」才能说脚本已创建；
   未调用工具或工具失败时声称完成属于错误。
6. 工具已执行成功后，用一两句话告诉用户结果即可，不要继续长篇思考或重复调用工具。
7. 生成的脚本与用户手动编辑完全一致：动作名用编辑器中文名，说明写 `remark`，
   `no`/`text` 由工具自动分配，不要手写。
8. 自定义快捷键组合（如 Ctrl+End、Ctrl+S）用 `keyClick` + `modifiers:["ctrl","shift","alt","win"]`；
   `hotkeyShortcut` 仅用于预设快捷键（如复制/粘贴），不要用它表达任意组合。
9. 不要编造当前墙钟时间。脚本运行时需要时间，用 `{Now}` / `{time:格式}` / `ctrl:Hour()` / `ctrl:Minute()`；
   用户要你「现在几点」而不是写脚本时，说明你看不到实时时钟。

## 像写代码一样写动作

把脚本当成程序：容器 = `{ ... }`，`children` = 花括号里的语句。

```
// 错误：循环是空的，A/B 只执行一次
loop
wait     // 同级，在循环外
keyClick

// 正确：A/B 每轮都执行
loop {
  wait
  keyClick
}
```

JSON（推荐，忽略子项 indent）：

```json
{
  "type": "loop",
  "loopCount": -1,
  "remark": "无限循环",
  "children": [
    { "type": "wait", "duration": 1, "remark": "循环体内等待" },
    { "type": "keyClick", "keyText": "a", "remark": "循环体内按键" }
  ]
}
```

`if` / `else` 是**兄弟**（同一层 `children` 里紧挨着），各自再嵌套：

```json
{
  "type": "loop",
  "loopCount": -1,
  "children": [
    { "type": "findImage", "followUp": "saveVar", "matchVarName": "btn", "imagePath": "images\\btn.bmp" },
    {
      "type": "if",
      "conditionExpr": "btn.matchData > 0",
      "children": [ { "type": "mouseClick", "remark": "找到则点" } ]
    },
    {
      "type": "else",
      "children": [ { "type": "wait", "duration": 0.5, "remark": "没找到则等" } ]
    }
  ]
}
```

`endLoop` 必须放在某个 `loop` 的 `children` 里（提前跳出），不能与 `loop` 同级。

空 `children`、或循环后面全是同级动作 → 工具报错，按报错改树，不要硬保存。

## 两阶段（必须走工具）

**阶段 1 — 规划树**（`planScriptActions`）

- 先列出整条动作链：准备 → 主循环 → 循环内观察/判断 → 行动 → 失败等待/兜底。
- 只传 `type`、`remark`、`children`、`loopCount` / `conditionExpr`。图片路径等可缺。
- 看返回的缩进树：循环体必须缩在循环下面。不对就改 `children` 再规划，不要进入阶段 2。

**阶段 2 — 补齐并保存**

- 同一棵树补上必填字段，调用 `createMacroScript`（或先 `buildScriptActions` 再创建）。
- 不要把阶段 1 的树拍扁成同级数组。

线性脚本（打开文件 → 等待 → 输入，无循环/条件）可跳过阶段 1，直接构建。

## 挂机 / 刷任务模板

用户要「一直刷 / 无限循环 / 挂机 / 全自动打怪」时，主流程必须是**顶层无限循环**，业务全在 `children` 里：

```
loop(-1) {                    // 整段挂机
  findImage(saveVar)          // 观察
  if 找到 {
    点击 / 放技能 / 拾取
  } else {
    wait                      // 没目标就等，别空转点
  }
}
```

要点：先观察再行动；失败走 `else` 等待，不要在循环外再写一遍同样的点击；
不要用 `aiActionExecute` 代替这棵树，除非用户明确要求 AI 动作执行。

## 简单任务

打开文件/输入文字/按键/运行程序直接用对应动作：`openFile` / `runProgram` /
`quickInput` / `keyClick` / `hotkeyShortcut` / `wait`。
`quickInput.parseEscapes` 缺省 0（按字面输入）。只有要把文本里的 `\n` `\t` `\\`
转成换行/Tab/反斜杠时才写 `parseEscapes: 1`。
先充分理解任务与动作语义再生成；
不确定的动作类型先查对应 section（勿猜）；同一 section 不要重复查。
系统动作（openFile/runProgram/openWebpage/closeProgram/activateWindow/runMacro/
mousePlayback/timerRecordTime/getCursorPos）的参数统一在 `readScriptReference section=system`，
需要时一次查完再构建；不要连环翻阅多个 section 找同一信息。
允许充分思考，正确性优先于速度。

## 路径与等待

1. **桌面路径**：用户桌面是 `%USERPROFILE%\Desktop`
   （如 `C:\Users\当前用户名\Desktop`），**不是** `C:\Users\Public\Desktop`（那是公共桌面）。
   引擎会自动展开 `%环境变量%`（如 `%USERPROFILE%`、`%TEMP%`），
   所以 `targetPath` 可以直接写 `%USERPROFILE%\Desktop\test.txt`，无需先查用户名。
   需要确认文件是否存在时用 `runAgentCommand`（如 `dir` / `where`）查询，或询问用户；
   禁止猜 `C:\Users\Public\Desktop`。
2. **慢操作等待**：打开文件 / 运行程序 / 打开网页 / 双击打开等操作之后，
   必须等窗口或程序起来再继续，`wait` 建议 **3 秒以上**（冷启动更久，可用 3~5 秒），
   不要用 0.5~1.5 秒这种短等待。

## 获取关键信息

**网页抓取（可用工具）**：需要网页内容（文章/文档/新闻/天气/官方说明等）时，
用 `fetchWebPage` 工具抓取，url 传完整 http/https 地址，返回纯文本并注明来源。
JSON 接口（如热搜榜 `/api/` 或 `.json`）返回原始 JSON，可直接提取结构化数据。
默认先走轻量 GET，失败或疑似 JS 空壳时自动用 App 内置 WebView2 隐藏渲染后再取 DOM
（覆盖 JS 动态页面）；强反爬仍可能失败，失败时如实说明，不要编造内容。
禁止抓取 localhost/127.0.0.1/内网地址。**不要假装已联网搜索、不要编造来源**；
抓不到或用户要的并非网页时，询问用户。

- **当前时间/日期**：不要编造时间。脚本里需要在**运行时**写入时间时，用：
  用 `runProgram` 调用 PowerShell 在**运行时**获取真实时间。模板：
  `targetPath=powershell.exe`，`inputText=-Command "Get-Date -Format 'yyyy-MM-dd HH:mm:ss' | Out-File -Append -FilePath $env:USERPROFILE\Desktop\test.txt -Encoding UTF8"`
  （PowerShell 会自行展开 `$env:USERPROFILE`）。
- **文件/路径确认**：用 `runAgentCommand`（白名单命令）查询，或询问用户。
- **需要外部信息（网页、天气等）**：询问用户，不要假装联网搜索。

## 效率 / 准确度

默认「效率优先」：动作链优先 findImage/OCR/wait/键鼠/`if`/`goto`，少用 AI 分析；
禁止 `aiActionExecute`，除非用户明确要求「AI动作执行/让AI自动操作」。
找图：`followUp=saveVar` + `if(matchData>0)` 即可，不必加计时器兜底。
`createMacroScript` 须写 `windowMode`（默认 `enabled=0`）；默认模式 `breakoutTimeSeconds` 未写视为 0。

用户说「准确度优先/要稳/要兜底」时切换准确度模式：仍禁止 `aiActionExecute`；
可在兜底分支用 `aiImageAnalysis` 诊断界面。关键找图前加计时器，失败走 `else.children`，
不要把兜底摊到容器外面：

```
timerRecordTime(findTimer)
findImage(saveVar, findTimeExpr=30)
if (findTimer < 30 and target.matchData > 0) {
  正常后续
} else {
  aiImageAnalysis → 1等待 / 2关弹窗 / 3刷新
  goto 主流程起点
}
```

「快点/效率」→ 效率优先；「稳/容错/兜底」→ 准确度优先并套用上面的树。
