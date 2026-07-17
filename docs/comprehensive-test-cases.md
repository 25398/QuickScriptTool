# QuickScriptTool 全面测试用例

> 版本对齐仓库模块：连点 / 录制 / 宏编辑执行 / 窗口模式 / 后台窗口模式 / 找图 OCR / 定时任务 / Agent·AI / 设置托盘。  
> 判定约定：自动化 suite **exit 0** 才算该模块机测通过；手工项打勾 `[x]`。  
> 建议顺序：先 **A 自动化门禁** → **B P0 冒烟** → **C 按模块深测** → **D 回归交叉**。

---

## 0. 环境与前置

| ID | 检查项 | 步骤 | 预期 |
|----|--------|------|------|
| ENV-01 | 构建产物 | Release 编出 `QuickScriptTool.exe` 及四个 SelfTest | `build\Release\` 存在 exe，OpenCV/VDA DLL 旁置 |
| ENV-02 | 权限 | 普通用户启动主程序；可选管理员对比窗口模式权限提示 | 能正常打开四 Tab |
| ENV-03 | 分辨率 | 记录当前分辨率（建议另测一档，如 1920×1080 / 2560×1440） | 坐标/找图用例需标注分辨率 |
| ENV-04 | OCR | 设置或首次文字识别时安装 OCR；确认环境 Ready | 安装后识别可用 |
| ENV-05 | AI（可选） | 配置可用 API Key / 模型 | 仅 AI 相关用例依赖 |

### A. 自动化门禁（每次全面检查必跑）

在仓库根目录：

```powershell
$msb = "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
foreach ($t in @(
  'WindowModeSelfTest','ScheduledTaskSelfTest',
  'MacroVariablesSelfTest','ScriptActionBuilderSelfTest',
  'CoordSpaceSelfTest','ScriptIoSelfTest','ImageMatchSelfTest',
  'AiActionRouterSelfTest','AppSettingsStoreSelfTest'
)) {
  & $msb ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:$t /m /v:minimal
  & ".\build\Release\$t.exe" --json
  if ($LASTEXITCODE -ne 0) { throw "$t failed exit=$LASTEXITCODE" }
}
# 窗口模式可选加深（需 VDA DLL）
.\build\Release\WindowModeSelfTest.exe --json --macro
```

| Suite | Target | 用例数 | 覆盖摘要 |
|-------|--------|--------|----------|
| 窗口模式 | `WindowModeSelfTest` | 9（含 1 个 `--macro`） | 引号剥除、IME 过滤、找窗、后台绑子控件、快捷输入与取消、宏桌面启动绑定 |
| 定时任务 | `ScheduledTaskSelfTest` | 11 | 秒级匹配、小时/周、门禁、自定义一次性、Tick 批量、解析 |
| 宏变量 | `MacroVariablesSelfTest` | 8 | 匹配变量、`CurLoops`、转义、找图时限、条件、goto/loop、未知变量 |
| 动作构建 | `ScriptActionBuilderSelfTest` | 10 | wait 构建、禁 customText、编号、补 stopMacro、endLoop、数组 JSON、相对移动、重复间隔语义 |
| 坐标 | `CoordSpaceSelfTest` | 12 | 标准 meta、存读 capture、JSON roundtrip、n* 归一化/迁移、找图 scale 选项、落点 |
| 脚本 IO | `ScriptIoSelfTest` | 12 | 录制路径、脱离时间、动作解析、存读 roundtrip、坏 meta/半截 JSON |
| 找图引擎 | `ImageMatchSelfTest` | 10 | 阈值清零、中心/偏移、金字塔与分数量化、NMS、冷冻位图匹配 |
| AI 路由 | `AiActionRouterSelfTest` | 14 | 四分类路由、坐标解析与映射、点击 JSON、识图 system prompt |
| 设置库 | `AppSettingsStoreSelfTest` | 10 | 默认值、路径、缺文件、jitter/playback/AI/home roundtrip、clamp、坏 JSON |

**通过标准**：九个默认 suite `--json` 均为 `ok:true` / exit `0`；有条件时再跑 `--macro`。

---

## 1. 主界面与导航（P0）

| ID | 优先级 | 模块 | 步骤 | 预期 |
|----|--------|------|------|------|
| UI-01 | P0 | 主窗 | 冷启动 | 四 Tab：连点 / 录制 / 宏 / 脚本定制可切换；上次 Tab/选中脚本恢复（`HomeState`） |
| UI-02 | P0 | 主题 | 设置中切换 themeId，重启 | 主题一致应用，无控件叠色/裁切 |
| UI-03 | P1 | DPI | 125%/150% 缩放各开一次 | 列表、编辑器、弹窗布局未严重错位 |
| UI-04 | P0 | 托盘 | `closeToTray=true` 点关闭 | 进托盘；托盘菜单可显示/退出；运行中托盘图标变化 |
| UI-05 | P1 | 托盘 | `closeToTray=false` 点关闭 | 真正退出进程 |
| UI-06 | P1 | 自动隐藏 | `autoHideMainWindow=true`，启动宏/连点 | 主窗隐藏行为符合设置；可从托盘恢复 |

---

## 2. 连点器（Clicker）

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| CLK-01 | P0 | 高效/极限/自定义间隔，对记事本空白区连点左键 | 按设定间隔点击；可热键启停 |
| CLK-02 | P1 | 中键/右键 | 对应按键生效 |
| CLK-03 | P1 | 开启坐标抖动 (jitterX/Y) | 点击落点在设定范围内浮动 |
| CLK-04 | P1 | 固定坐标 + 随机间隔上限 | 只点固定点；间隔 ≤ 设定 |
| CLK-05 | P1 | 按下抬起间隔 | 按下与抬起有可见间隔（慢速观察） |
| CLK-06 | P2 | 点击次数上限 | 达到上限后自动停止 |
| CLK-07 | P0 | 运行中热键停止 | 立即停止，无「失控连点」 |

---

## 3. 录制（Recorder）

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| REC-01 | P0 | 范围=当前窗口，启停热键录制鼠标键盘 | 生成录制脚本；路径可在录制列表中看到 |
| REC-02 | P0 | 范围=全局 | 跨窗操作均被记录 |
| REC-03 | P1 | 录制时按「停止录制」热键 | 该热键本身不被记入或按产品约定忽略 |
| REC-04 | P0 | 对录制结果「优化」 | 优化对话框可合并/精简；保存后动作数合理减少且可回放 |
| REC-05 | P0 | 回放刚录制脚本 | 路径/按键大体复现；可 stop 热键打断 |
| REC-06 | P1 | Agent / 工具 `optimizeRecording`（若走 Agent） | 文件被更新且主界面列表刷新 |

---

## 4. 宏脚本 I/O 与坐标（script_io / coord_space）

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| IO-01 | P0 | 新建宏 → 加若干动作 → 保存 → 重启加载 | name/hotkey/actions/windowMode/coordMeta/breakoutTime 不丢 |
| IO-02 | P0 | 改名、另存、删除脚本 | 列表与磁盘文件一致；删后热键失效 |
| IO-03 | P1 | 不同分辨率下保存的含坐标脚本，换分辨率回放 | 相对 `coordMeta` 自适应；关键点击大致落在目标（允许小误差） |
| IO-04 | P1 | 含 `windowMode` 的脚本导出再导入 | JSON 可解析；绑窗参数保留 |
| IO-05 | P2 | 损坏/半截 JSON 打开 | 提示失败，不崩溃 |
| IO-06 | P1 | 录制脚本路径识别（`IsRecordingScriptPath` 相关 UI） | 录制与宏列表归类正确 |

---

## 5. 脚本动作矩阵（编辑器 + 桌面执行）

对每个类型建议最小用例：**能添加 → 参数可编辑 → 执行一次 → 热键可停**。高风险动作为 P0。

### 5.1 鼠标 / 键盘 / 时序

| ID | 类型 (JSON) | 优先级 | 最小步骤 | 预期 |
|----|-------------|--------|----------|------|
| ACT-MM | `moveMouse` | P0 | 移到已知屏点 | 光标到达 |
| ACT-W | `wait` | P0 | wait 1000ms | 约 1s 延迟 |
| ACT-MD/U/C | `mouseDown/Up/Click` | P0 | 左/右各点一次；另测 Click 重复 2 次 + 间隔 | 单次无间隔等待；重复时仅两次之间有间隔 |
| ACT-KD/U/C | `keyDown/Up/Click` | P0 | A、Enter、组合键；KeyClick 重复同左 | 同上 |
| ACT-HK | `hotkeyShortcut` | P1 | Ctrl+C / 系统快捷；可测重复间隔 | count=1 立即执行；count>1 仅中间隔 |
| ACT-QI | `quickInput` | P0 | 含 `\\n` `\\t` 的文本进记事本；重复间隔≠字间 | 转义正确；字间用 charInterval；整段重复用 duration |
| ACT-SW | `scrollWheel` | P1 | 对可滚区域上下滚；可选重复 | 滚动方向合理；重复间隔语义同点击 |

### 5.2 流程控制

| ID | 类型 | 优先级 | 步骤 | 预期 |
|----|------|--------|------|------|
| ACT-LP | `loop` / `endLoop` | P0 | 有限次数 loop 内点一次 | 次数正确；非法孤立 endLoop 编辑器/构建拒绝 |
| ACT-LP∞ | 无限 loop | P0 | 无限循环 + stop 热键 | 可打断；**不**被自动塞多余 stopMacro 挡逻辑（与 builder 规则一致） |
| ACT-IF | `if` / `else` | P0 | 条件真/假各走一支（含 `==` `>` 与 and/or） | 分支正确 |
| ACT-GT | `goto` | P1 | goto 步号跳转 | 跳到目标步，不越界崩溃 |
| ACT-BLK | `defineBlock` / `runBlock` | P1 | 定义块后多次 runBlock | 执行块体；主流程不「双执行」define |
| ACT-SM | `stopMacro` | P0 | 脚本末尾或中途 stop | 宏结束；缺省时 Agent 构建应自动补（有限脚本） |

### 5.3 嵌套脚本

| ID | 类型 | 优先级 | 步骤 | 预期 |
|----|------|--------|------|------|
| ACT-MP | `mousePlayback` | P0 | 回放一段录制；可选重复+间隔 | 完整播完或可中断；重复间隔仅在两次回放之间 |
| ACT-RM | `runMacro` | P0 | A 调用 B；B 再短宏 | 嵌套执行正确；停热能穿透 |

### 5.4 视觉：找图 / 锁图 / OCR

| ID | 类型 | 优先级 | 步骤 | 预期 |
|----|------|--------|------|------|
| ACT-FI1 | `findImage` 后续=点击 | P0 | 静态模板在屏上 | 命中后点击中心或偏移点 |
| ACT-FI2 | `findImage` 后续=移动 | P1 | 同上 | 光标移到匹配点 |
| ACT-FI3 | `findImage` 后续=存变量 | P0 | 存后用 `{match*.x}` 等条件/移动 | 变量可展开（对齐全屏变量用例） |
| ACT-FI4 | 找图超时 `findTime` -1/0/正数 | P0 | 模板故意不存在 | 超时行为符合设定；非数字按 0 |
| ACT-LS | `lockScreenshot` / `unlock` | P1 | 锁屏后找图 | 在冻结层匹配；解锁后恢复 |
| ACT-OCR1 | `textRecognition` 取文本 | P0（需 OCR） | 对清晰文字区 | 文本写入变量可用 |
| ACT-OCR2 | OCR 搜索模式 + 点击 | P1 | 区域内搜关键词 | 找到并点击/移动 |
| ACT-OCR3 | 仅数字 / 相对图像区域 | P2 | 边界参数 | 结果稳定或合理失败提示 |

### 5.5 系统动作

| ID | 类型 | 优先级 | 步骤 | 预期 |
|----|------|--------|------|------|
| ACT-RP | `runProgram` | P0 | 启动 notepad / 带参数程序 | 进程起来；异常路径提示 |
| ACT-CP | `closeProgram` | P1 | 关闭刚启动进程 | 目标退出 |
| ACT-OW | `openWebpage` | P1 | 打开 https URL | 默认浏览器打开 |
| ACT-OF | `openFile` | P1 | 打开本地 txt | 关联程序打开；路径含空格/中文 |
| ACT-GCP | `getCursorPos` | P1 | 取坐标写变量 | 与实际光标一致 |
| ACT-TMR | `timerRecordTime` | P2 | 记时相关 | 按产品语义工作 |
| ACT-CT | `customText` | P0（负例） | Agent/`buildScriptActions` 传入 | **必须拒绝**；编辑器侧不产生脏数据 |

### 5.6 AI 动作（需 API）

| ID | 类型 | 优先级 | 步骤 | 预期 |
|----|------|--------|------|------|
| ACT-AIT | `aiTextAnalysis` | P1 | 简单提示词 | 返回文本/整型写入变量 |
| ACT-AII | `aiImageAnalysis` | P1 | 带截图分析 | 返回合理内容 |
| ACT-AIE | `aiActionExecute` | P1 | 要求「点某处」类指令 | 路由正确（工具/视觉/复合点击）；屏幕点击坐标映射合理 |
| ACT-AIR | `ai_action_router`（隐式） | P1 | 纯视觉问句 vs「点按钮」vs 多步 | 分类符合：`VisionQuery` / `CompositeClick` / `ToolExecute` / `MultiTurnTools` |

---

## 6. 宏变量与条件（macro_variables）

> 自动化已覆盖核芯；以下为 UI 联调补充。

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| VAR-01 | P0 | 跑过自检后，在宏里用 `{matchRet.x}` `{matchRet.cx}` | 与自检一致 |
| VAR-02 | P0 | loop 内 `{ctrl:CurLoops()}` 显示/参与条件 | 次数递增正确 |
| VAR-03 | P0 | 未知 `{no_such}` | 变空串，不卡死/递归爆栈 |
| VAR-04 | P1 | `loop` 最大次数来自变量表达式 | 与 `ResolveLoopMaxCount` 行为一致 |
| VAR-05 | P0 | 条件 `a == b` 与 `>`，多行 and/or | 分支正确 |
| VAR-06 | P1 | 调试输出窗（playback.debug） | 变量与步骤日志可读 |

---

## 7. 窗口模式（HiddenDesktop / 鼠标宏桌面）

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| WM-01 | P0 | 跑 `WindowModeSelfTest --json` | 默认 8 项全绿 |
| WM-02 | P0 | `--json --macro` | `macro_desktop_launch_bind` 绿（有 VDA） |
| WM-03 | P0 | 编辑器配置窗口模式：启动 notepad + 文档路径带引号与中文 | **不出现**「文件名无效」；成功绑到目标主窗 |
| WM-04 | P0 | 选择方式：启动时选择 / 启动时鼠标位置 / 编辑器类名 / 不选择 | 各模式行为符合文案 |
| WM-05 | P0 | 宏内找图（客户区坐标） | 匹配率正常（非长期 0%）；偏移点击落在客户区正确点 |
| WM-06 | P0 | 快捷输入到目标 | 文字进入目标；运行中热键停止可取消未打完字符 |
| WM-07 | P1 | IME/工具条干扰 | 不绑到 `SoPY` 等输入法条 |
| WM-08 | P1 | 目标最小化 / 无渲染 / 权限不匹配 | `WindowModeHealth` 提示；`blockRunWhenUnhealthy` 时阻止运行 |
| WM-09 | P1 | 预览缩略图开关与刷新间隔 | 设置生效；不影响绑窗 |
| WM-10 | P2 | 托盘/Z-order 窗口模式运行中 | 主程序托盘与目标桌面不互相「抢错层」导致用户找不到窗 |
| WM-11 | P2 | 商店版 Notepad / AppsFolder 启动 | 能绑则绿；不能则记录已知限制（设计文档已列缺口） |

---

## 8. 后台窗口模式（BackgroundWindow）

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| BG-01 | P0 | 自检 `background_bind_child` / `background_quick_input` | 绿 |
| BG-02 | P0 | 绑定已存在含 Edit 的窗；类名指定到子控件 | `TargetHwnd` 为子 Edit，截图非整框错位 |
| BG-03 | P0 | 后台找图 + 点击 | 目标窗不必前台；点击落在客户区正确位置 |
| BG-04 | P0 | 后台快捷输入 | 文本进入；前台焦点可保持在其他窗（无 fallback） |
| BG-05 | P1 | `allowForegroundInputFallback=false` 时输入失败场景 | 不抢焦点；有明确失败/健康状态 |
| BG-06 | P1 | fallback=true | 仅「后台全失败」时短暂前台输入 |
| BG-07 | P1 | 取消/停止 | 与桌面同样迅速响应 |

---

## 9. 找图引擎与叠加层（image_match / overlays）

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| IMG-01 | P0 | 编辑器截图选区保存模板 | 文件生成；列表可选 |
| IMG-02 | P0 | 测试匹配叠加层（阈值滑条） | 可见匹配框；阈值过高变 0 匹配 |
| IMG-03 | P1 | 多尺度 / 多引擎切换（若 UI 暴露） | 相似图仍可命中或明确失败 |
| IMG-04 | P1 | 虚拟屏幕多显示器 | 在副屏模板能找到 |
| IMG-05 | P1 | 准星拖拽绑坐标 / 程序路径 / 窗口 | 三种模式写入正确字段 |
| IMG-06 | P2 | NMS 多目标 | 取第一目标符合产品规则 |

---

## 10. OCR

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| OCR-01 | P0 | 未安装时打开识别 | 安装对话框；状态 NotInstalled→Ready |
| OCR-02 | P0 | Ready 后识别静态 UI 文字 | 高正确率；变量可引用 |
| OCR-03 | P1 | 缺 helper/依赖 | 状态与提示清晰，不崩溃 |
| OCR-04 | P1 | OCR 叠加选区 / 相对找图区域 | 区域与结果一致 |

---

## 11. 定时任务（scheduled）

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| SCH-01 | P0 | `ScheduledTaskSelfTest --json` | 11/11 绿 |
| SCH-02 | P0 | UI 建「每小时」任务，对准下一分钟秒 | 到秒触发；不因毫秒错过 |
| SCH-03 | P0 | 「每天」「每周」勾选星期（周日 bit 尤其） | 仅勾选日触发 |
| SCH-04 | P0 | 「自定义」一次性 | 触发一次，`customFired`，同秒不连发 |
| SCH-05 | P0 | 全局禁用 / 暂停 / 单条 Disabled / 空路径 | 均不触发 |
| SCH-06 | P0 | 同一秒多条到期 | **全部**触发（Tick 批量） |
| SCH-07 | P0 | 任务类型 Recording vs Macro | 启动对应文件 |
| SCH-08 | P1 | 日期时间选择器 | 格式含年；保存再开正确 |
| SCH-09 | P0 | Agent：`create/update/deleteScheduledTask` | 磁盘 JSON 更新且主窗列表 **Reload** |
| SCH-10 | P2 | 空 filePath 任务出现在坏文件中 | 加载时丢弃 |

---

## 12. Agent 与工具面

| ID | 优先级 | 工具/场景 | 步骤 | 预期 |
|----|--------|-----------|------|------|
| AG-01 | P0 | 打开 Agent 对话框 | 会话可发消息 | UI 正常；历史可存（conversation store） |
| AG-02 | P0 | `listScripts` / `readScript` / `writeScript` | 读写现有宏 | 内容一致；写后列表刷新 |
| AG-03 | P0 | `buildScriptActions` + `createMacroScript` | 要求「等待+点击+结束」 | 合法动作；自动补 stopMacro；**无** customText |
| AG-04 | P0 | `submitMacroActions` | 提交坏 endLoop | 校验失败有说明 |
| AG-05 | P0 | `deleteScriptFile` | 删除测试宏 | 文件与 UI 同步 |
| AG-06 | P1 | `getScriptStats` / `optimizeScript` | 对长脚本 | 统计合理；优化可撤销或可再读 |
| AG-07 | P0 | 定时任务 CRUD 工具 | 增删改 | 持久化 + Reload（见 SCH-09） |
| AG-08 | P1 | `listSettings` / `updateSettings` | 改一项回读 | 与 `app_settings.json` 一致 |
| AG-09 | P1 | `listAiModels` + AI builder 工具 | 配置多模型 | 列表正确；构建的 AI 动作可进宏 |
| AG-10 | P1 | 附件 / 引用脚本 | 带附件对话 | 不炸；引用内容进上下文 |
| AG-11 | P2 | 中断生成 / 关对话框 | 进行中关窗 | 资源释放，无残留线程捣乱 |

---

## 13. 设置持久化（app_settings_store）

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| SET-01 | P0 | 改连点/回放/其它/窗口模式/AI 各项 → 保存 → 杀进程再开 | 全部恢复 |
| SET-02 | P1 | `playbackCount` / `playbackInterval` | 宏按次数与间隔重播 |
| SET-03 | P1 | 播放开始音效 / 右下角提示开关 | 行为符合布尔项 |
| SET-04 | P1 | 窗口模式 `showPreviewThumbnail` / `previewRefreshMs` / `blockRunWhenUnhealthy` | 行为符合 |
| SET-05 | P2 | 手改坏 `app_settings.json` | 回退默认或可读部分，不崩溃 |
| SET-06 | P1 | AI enabled=false 时跑 AI 动作 | 明确提示，不静默挂起 |

---

## 14. 热键

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| HK-01 | P0 | 全局热键：开始/停止宏、连点、录制 | 冲突检测或后注册策略明确；能启停 |
| HK-02 | P0 | 脚本专属热键 | 仅该脚本响应；删脚本后释放 |
| HK-03 | P1 | 热键录制对话框捕获组合键 | UI 显示与保存一致；含 Win/Alt 边角 |
| HK-04 | P1 | 宏运行中停止热键 | 打断 wait、quickInput、找图等待、loop |

---

## 15. 调试与其它壳层

| ID | 优先级 | 步骤 | 预期 |
|----|--------|------|------|
| DBG-01 | P1 | 打开宏调试输出窗 | 步骤日志与错误可见 |
| DBG-02 | P2 | 任务栏缩略预览 | 有预览时不闪退 |
| DBG-03 | P2 | `WindowModeDiag`（可选） | 诊断绑窗/视觉问题有输出 |
| DBG-04 | P1 | 异常退出后再开 | 托盘图标不残留双重实例策略符合预期 |

---

## 16. 交叉回归（建议每日/发版前）

| ID | 场景 | 关注点 |
|----|------|--------|
| X-01 | 窗口模式宏内：找图 → 存变量 → if → quickInput → stop | 变量+窗口客户区+可中断 |
| X-02 | 后台模式宏运行时前台操作其他软件 | 不被抢焦点；目标仍执行 |
| X-03 | 定时任务触发时刻刚好在手动跑宏 | 排队/并行策略可接受，不崩溃 |
| X-04 | Agent 改脚本后立即热键运行 | 读到的是新文件 |
| X-05 | 录制 → 优化 → 嵌套进宏 → 换分辨率 | 坐标/时间轴仍可用 |
| X-06 | OCR + 找图 + AI 动作混合短脚本 | 失败有日志；成功链路通 |

---

## 17. 执行清单（可打印勾选）

### 门禁

- [ ] A：九个 SelfTest `--json` exit 0
- [ ] A+：`WindowModeSelfTest --json --macro`（有 VDA 时）

### P0 冒烟（约 30–60 分钟）

- [ ] UI-01, UI-04
- [ ] CLK-01, CLK-07
- [ ] REC-01, REC-04, REC-05
- [ ] IO-01, IO-02
- [ ] ACT-W, ACT-MM, ACT-QI, ACT-LP, ACT-FI1, ACT-FI3, ACT-SM, ACT-RP
- [ ] VAR-01, VAR-03, VAR-05
- [ ] WM-03, WM-05, WM-06
- [ ] BG-02, BG-03, BG-04
- [ ] SCH-02, SCH-04, SCH-05, SCH-06, SCH-09
- [ ] AG-02, AG-03, AG-07
- [ ] SET-01, HK-01, HK-04

### P1 加深（半日～一日）

- [ ] 第 5 节剩余动作类型扫一遍
- [ ] OCR-01～02；IMG-02～04
- [ ] WM-07～09；BG-05～06
- [ ] AI 三项（有 Key 时）
- [ ] SET-02～04；AG-08～09；X-01～X-05

### P2 / 已知缺口（时间允许）

- [ ] 商店 Notepad、多显示器找图、损坏 JSON、Diag、任务栏预览
- [ ] 仍偏手工：换分辨率实机点击观感、OCR Ready/正确率、连点与热键 UI、Agent 对话与附件

---

## 18. 缺陷记录模板

```text
ID:
模块: (window|background|scheduled|macro_var|builder|script_io|coord|image|ocr|agent|clicker|recorder|settings|hotkey|ui)
优先级: P0/P1/P2
复现步骤:
期望:
实际:
自动化: (SelfTest case name / 无)
环境: OS / 分辨率 / DPI / 是否管理员
```

---

## 19. 与自动化的对应关系

| 手工大类 | 已有自动化 | 仍偏手工 |
|----------|------------|----------|
| 窗口/后台 | `WindowModeSelfTest` | 真找图、商店 App、托盘观感 |
| 定时 | `ScheduledTaskSelfTest` | UI 选择器、Agent Reload 侧效 |
| 变量/条件 | `MacroVariablesSelfTest` | 调试窗联调 |
| Agent 写动作 | `ScriptActionBuilderSelfTest` | 对话、附件、CRUD Reload |
| 坐标 / 脚本 IO / 找图引擎 / AI 路由 / 设置库 | `CoordSpaceSelfTest` · `ScriptIoSelfTest` · `ImageMatchSelfTest` · `AiActionRouterSelfTest` · `AppSettingsStoreSelfTest` | 换分辨率实机观感、OCR 安装与正确率、连点/热键 UI |
| UI / 连点 / OCR / Agent 壳 | （无 exe） | 本文件第 1–3 / 10 / 12 / 14 节 |

Agent 改代码后：优先 `--json` 对应 suite，再用本文件 P0 做 UI 确认；**勿在 suite 未绿时宣称模块已修好**。
)
