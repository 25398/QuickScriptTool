# 视觉 Agent / 逻辑转化 — 对照审计（差距表）

> 状态：**仅审计，未改代码**。对齐参考：Midscene.js（规划/定位分离、observe→act）、Ui.Vision / SikuliX（找图门闩）、影刀·实在（另存为/封闭 UI 恢复）、本仓既有实现。  
> 已落地：定位管线 Midscene 化（整图 VLM → 按需 Zoom deepLocate；短引号标签）。Florence/本地找图（local_grounding）已移除，纯云端 VLM 定位。  
> **请确认下表优先级后再开实现 PR。**

---

## 0. 总览

| 域 | 对标 | 本仓现状（摘要） | 差距等级 |
|----|------|------------------|----------|
| 定位管线 | Midscene deepLocate | 一级整图 VLM + 按需 Zoom；短标签 | **已对齐（本轮）** |
| Agent 闭环 | Midscene aiAct observe→act | 有 settle/observe/lookahead；locate 失败多为软提示 | **中** |
| 另存为恢复 | 影刀 CV / UiPath 对话框策略 | 有 UIA/对话框探测与 Escape 守卫；失败后仍可 F12 等乱键 | **中高** |
| 逻辑转化回放 | Ui.Vision 录制+找图 | 找图阈值 85、OCR 前 activate、只填表 prompt 已有 | **中**（回放假匹配/错屏 OCR 仍可能） |
| 本地 OCR 定位 | Midscene：动作定位不用全屏 OCR | 已从 locate 热路径移除 | **已对齐** |

---

## 1. Agent 闭环：observe → act

### 1.1 Midscene / 主流做法

| 能力 | Midscene 等 |
|------|-------------|
| 节奏 | 规划一步 → 执行 → **强制观察**（新截图）→ 再规划 |
| 定位失败 | 换描述 / deepLocate / 滚动露出；**不**用盲快捷键碰运气 |
| 规划 vs 定位 | Planning 与 Locate 可分模型；定位失败不污染整任务 goal |
| 死循环 | 相同动作重复 → 换策略或结束 |

### 1.2 本仓已有

| 落点 | 行为 |
|------|------|
| [`ai_action_service.cpp`](../src/ai_action_service.cpp) Agent 环 | `onObserveScreen` + settle；`SKIP_OBSERVE` 省 token；lookahead |
| `lastLocateFailed` | 下一轮注入「换描述再 locate 最多 1 次，或 completeTask」 |
| locate 错误串 | [`engine_script_run.cpp`](../src/engine/engine_script_run.cpp) 文案禁止猜坐标/乱快捷键；另存为「浏览」提示偏软（「键盘兜底…勿当首选」） |
| 重复批次 | `AiNoteToolBatchSignature` → 劝阻 Escape/乱快捷键 |
| 盲键盘 | `historyIsBlindKeyOnly` 统计；非硬拦全部 keyClick |
| 对话框 | [`GuardEscapeInSaveDialog`](../src/macro_execute_tools.cpp)、`ProbeForegroundDialogKind`、灰控件 UIA 拦截 |

### 1.3 差距表（Agent）

| # | 差距 | 现象 / 风险 | 建议改法（待确认） | 优先级 |
|---|------|-------------|-------------------|--------|
| A1 | locate 失败后 **无硬拦** 乱快捷键 | 日志：桌面 NOT_FOUND 后仍 `keyClick(F12)` | 在 `lastLocateFailed` 为真的下一轮：`keyClick`/`hotkeyShortcut` 拒绝非白名单（Enter/Tab/Escape×1/PageDown/Backspace）；放行须 `confirmBlindKey=true` 或先成功 observe | P0 |
| A2 | 另存为恢复策略散落在 prompt，无状态机 | Agent 猜「浏览」/F12；迷你保存框 vs 经典另存为未分支 | 检测 `kind=saveAs` 时注入 **固定恢复菜单**：① locate「桌面」/导航树短标签 ② 文件名 `quickInput(clearFirst)` ③ locate「保存」；禁止 F12/Ctrl+S 重开；可选 `runSaveAsRecipe` 工具 | P0 |
| A3 | observe→act 可被 SKIP_OBSERVE 跳过过多 | 键盘连打多轮无新图 | locate 失败后 **强制** 下一轮附观察图；另存为探测到前台时禁止 SKIP_OBSERVE | P1 |
| A4 | 「换描述最多 1 次」仅文案 | 模型可无限换 target 字符串 | 宿主计数 `locateRetryAfterFail`；超限只允许 scroll/activate/completeTask | P1 |
| A5 | 规划模型无图时仍鼓励 Escape/Alt+F4 | 纯文本规划轮提示与「禁止乱快捷键」冲突 | 另存为上下文改「仅 locate / quickInput」，去掉默认 Escape 推荐 | P2 |

---

## 2. 逻辑转化对齐 RPA 回放

### 2.1 Ui.Vision / SikuliX / 影刀 做法

| 能力 | 典型做法 |
|------|----------|
| 找图门闩 | 高阈值 + 失败分支（重试/人工/AI）；低特征模板拒绝 |
| 动态数据 | OCR/表格抽取在 **正确窗口**；填写步骤不重跑「新建/保存」 |
| 回放漂移 | 锚点图更新；区域搜索；缩放容差有限 |

### 2.2 本仓已有（近期已改）

| 落点 | 行为 |
|------|------|
| `MakeFindImageClick` | `matchThreshold = 85` |
| `CollapseInstanceDataEntries` | ≥4 实例 QI + 列表窗才折叠；OCR 前 `activateWindow` 列表窗 |
| `MakeDynamicFillAi` | 「只填表 / 勿重做」硬约束；背景说明弱化原任务 |
| 找图失败 else | `MakeFallbackAi` 含已完成/下一步/禁止从零重做 |

### 2.3 差距表（逻辑转化）

| # | 差距 | 现象 / 风险 | 建议改法（待确认） | 优先级 |
|---|------|-------------|-------------------|--------|
| L1 | 找图 85 仍可能假 100% 命中 | 低纹理模板点错坐标 | 编译时估模板特征（方差/边缘密度）；过低则 **仅门闩不点击** 或强制走 else AI；可选缩小 `search` 到录制点邻域 | P0 |
| L2 | OCR 全屏 + 列表 activate 仍可能错屏 | Edge 费用中心进 `logicOcr` | OCR 后校验：变量文本须含列表启发（多行/时间/域名）；失败则再 activate+OCR 一次，仍失败跳过折叠填表改走 fallback AI | P0 |
| L3 | 动态填表 AI 仍可嵌套工具开浏览器 | prompt 约束非硬拦 | 动态填表 `AiActionExecute`：`aiMaxSteps` 收紧；工具白名单去掉 `runProgram`/`openWebpage`/`openAppViaSearch`（或 hooks 拒绝） | P1 |
| L4 | 回退 AI 与动态填表两套 prompt | 维护成本、偶发重开 goal | 统一「进度上下文」模板（已完成/下一步/禁止重置 goal） | P2 |
| L5 | 无回放自检 suite 覆盖错屏 OCR | 回归靠人工跑 Excel 任务 | `AiActionRouterSelfTest` 增：假 OCR 脏文本 → 不应直接填表；低特征模板 → 不点 | P1 |

---

## 3. 与「已对齐定位」的边界（勿回退）

| 不要做 | 原因 |
|--------|------|
| locate 热路径全屏 Paddle OCR | 慢（~2s+）且长描述子串易 miss |
| Florence → CorrectLocate 先验 | 毒化云端 → NOT_FOUND |
| Edge/Excel 控件词典 / 右上角空间 hint | 已明确拒绝；另存为用 **对话框 kind + 通用短标签** |
| Florence Zoom 直接点击 | 粗框偏则裁掉真目标 |

---

## 4. 建议实现分期（确认后执行）

```text
Phase A（Agent P0）
  A1  locate 失败后键盘硬拦
  A2  另存为面板恢复菜单 / 可选 recipe 工具

Phase B（逻辑转化 P0–P1）
  L1  低特征模板不点
  L2  OCR 列表内容校验
  L3  动态填表工具白名单
  L5  自检用例

Phase C（体验）
  A3–A5、L4
```

验收建议：

1. 复现「桌面新建 Excel→另存为点桌面」：locate 失败后 **不得** 出现 F12；应出现换描述 locate 或 completeTask / 保存恢复提示。  
2. 逻辑块回放：错屏 OCR 不得驱动填表；找图假匹配应进 else 而非点错。  
3. `AiActionRouterSelfTest` / 既有 logic_convert_* 全绿后再编壳。

---

## 5. 请你确认

回复时请标明，例如：

- `确认 Phase A` — 只做 Agent A1+A2  
- `确认 Phase A+B` — Agent + 逻辑转化 P0/P1  
- `调整：…` — 改优先级或砍项  

确认前 **不改业务代码**（本文档除外）。

---

## 6. 落地记录（Phase A+B 已确认并实现）

| 项 | 状态 |
|----|------|
| A1 locate 失败键盘硬拦 | `AiNoteLocateFailed` + keyClick/hotkey 守卫 |
| A2 另存为恢复菜单 | Agent 轮注入固定恢复步骤 |
| L1 低特征模板 | 抬阈值 + 锚点邻域搜索 |
| L2 OCR 列表校验 | `LooksLikeListOcrText` + listActivate 再 OCR |
| L3 填表工具白名单 | `fillTableOnly` 过滤 runProgram/网页/hotkey |
| L5 自检 | `locate_fail_key_block` / `fill_table_only_tools` / `list_ocr_heuristic` 等 |

---

## 7. Phase D（Midscene ROI，已实现）

| 项 | 改动 |
|----|------|
| 自适应 refine | 归一化单点 / 紧凑框 / 宽控件 → `skippedRefine`，省一轮 Zoom API；诊断日志带「省一轮 API」 |
| ~~本地 grounding 预热~~ | **已移除**（Florence/local_grounding 模块删除；定位纯云端 VLM） |

### Phase D 续（本轮加固，已实现）

| 项 | 改动 |
|----|------|
| 紧凑门禁按观察区宽度缩放 | `ShouldAcceptCoarseLocateWithoutRefine` 的 `maxSide` 随 `captureW` 8% 缩放并钳到 [160,320]：4K 下 200px 按钮不再被强迫 Zoom |
| lazy 补级 | `locateAndClick`/CompositeClick 默认 `refineLevels=1`（简单目标 1 轮 API 即点）；粗框未达「紧凑即点」时 `ExecuteZoomRefineLocate` 自动补一级 Zoom（`lazyEscalateMaxLevels`） |
| ~~预热缓存消费~~ | **已移除**（随 local_grounding 删除） |
| ~~热目标 TTL + 实时让路~~ | **已移除**（随 local_grounding 删除） |

## Phase C（部分已实现）

| 项 | 状态 |
|----|------|
| A3 locate 失败后强制刷新观察 | `lastLocateFailed` 下一轮禁 SKIP_OBSERVE，`forceRefreshObserve` 重截；另存为在前台同样禁 SKIP_OBSERVE |
| A4 连败硬拦计数 | `AiLocateRetryCount`：locate 失败递增、成功清零；≥2 次后指令只允许 scrollWheel/activateWindow/completeTask |

仍待 Phase C：A5、L4。
