# 「半视觉」感知优化 —— 可用测试用例

> 对应改动见 [`docs/ai-action-exec-optimization.md`](ai-action-exec-optimization.md) **§73**。
> 本轮把 UIA 控件清单从「编号 + 名字 + 坐标」升级为
> **「编号 + 角色 + 动作能力 + 可读状态 + 坐标」**，并让**编号可以被工具直接使用**
> （`locateAndClick(elementId=N)`）——对标 Windows-MCP 的 UI 树设计。

**一句话验收标准**：模型现在应当能冲着**清单**说「点第 7 条」，而不是
「先截图 → 描述目标 → 再识图定位」；并且**开关是开还是关、滑块在哪、输入框里有什么**
这类问题不再需要靠截图去猜。

---

## 0. 三层测试怎么分工（先读这一节，避免测错层）

| 层 | 跑什么 | 覆盖什么 | 需要桌面会话？ |
|---|---|---|---|
| **A. 逻辑自检（必跑）** | `WindowModeSelfTest`、`AiActionRouterSelfTest` | 类型表穷尽性、动作能力动词、状态渲染、编号直查与拒绝规则 | **不需要** |
| **B. 仓库全量自检** | `tools\run_all_selftests.ps1` | 本轮改动有没有踩到别的模块 | 部分交互档需要 |
| **C. 真机端到端（人工）** | 内嵌 AI 助手 + 真实软件窗口 | 「模型真的会用编号/状态」这一层**只有它能证** | 需要 |

⚠ **A/B 全绿不等于 C 成立**。本轮新增的是**给模型看的文本**，逻辑自检只能钉住
「文本里有没有这些字段」，钉不住「模型会不会用」。所以 C 必须真跑一遍。

---

## A. 逻辑自检（一条命令，不需要桌面）

```powershell
# 1) 构建（⚠ 必须在普通 PowerShell/CMD 里跑；受限宿主禁止启动 MSBuild）
#    若你的环境有 https_proxy，先 Remove-Item Env:HTTPS_PROXY（否则 MSBuild 报 MSB6001）
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  build\QuickScriptTool.sln /p:Configuration=Release /t:WindowModeSelfTest /m /v:m /nologo

& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  build\QuickScriptTool.sln /p:Configuration=Release /t:AiActionRouterSelfTest /m /v:m /nologo

# 2) 跑（--json 结果在 stdout；exit 0 才算过）
cd build\Release
.\WindowModeSelfTest.exe --json     | Select-String 'uia_action_verb_table|uia_control_list_carries_action_and_state|"failed"|"passed"'
.\AiActionRouterSelfTest.exe --json | Select-String 'element_index_facts_and_id_binding|"failed"|"passed"'
```

**期望**（2026-09-26 实测基线）：

```
{"name":"uia_action_verb_table","ok":true,"detail":""}
{"name":"uia_control_list_carries_action_and_state","ok":true,"detail":""}
{"passed":88,"failed":0,"ok":true}          ← WindowModeSelfTest

{"name":"element_index_facts_and_id_binding","ok":true,"detail":""}
{"passed":187,"failed":0,"ok":true}         ← AiActionRouterSelfTest
```

### A1. 三个新用例各自在钉什么（出红时按这张表定位）

**`uia_action_verb_table`**（`WindowModeSelfTest`）

| 断言 | 红了说明什么 |
|---|---|
| 表内 `action` == `UiActionVerbForControl(id)` | 类型表的两个出口各说各话（有人只改了 switch 没改表） |
| 角色/动词不许为空串 | 台账那行会少一段 ⇒ 模型看不出能力（这一轮等于白做） |
| 未知 id（0 / -1 / 99999）必须给 `focus` | **有人让未知类型冒充 `click`** ⇒ 模型会去点一个说不清是什么的东西 |
| 表不许为空 | 空表会让所有循环静默通过（假绿） |

**`uia_control_list_carries_action_and_state`**（`WindowModeSelfTest`）

| 断言 | 红了说明什么 |
|---|---|
| 文本含 `action:slide` / `action:fill` / `action:toggle` | 动作能力没有渲染进给模型的台账 |
| 文本含 `[value:42] [range:0-100] [toggle:on] [focused]` | 可读状态没有渲染（这一轮的核心价值就在这几个字段） |
| 文本含 `[password(值不回传)]` | 密码框没有标注（或标注丢了） |
| 文本**不含** `value:"` | ⚠⚠ **最严重的一条**：有密码/敏感值被写进了给模型的文本 |

**`element_index_facts_and_id_binding`**（`AiActionRouterSelfTest`）

| 断言（`detail` 里的关键字） | 红了说明什么 |
|---|---|
| `facts_slider` / `facts_edit` | 角色/能力/状态在**合并进索引**时被丢掉（或 OCR 条目把它们抢走了） |
| `ocr_got_fake_action` | OCR 文字条目被编造了动作能力 ⇒ 模型以为能点，点了没反应白费一轮 |
| `facts_not_rendered` | 索引文本里没渲染出 `action:…` / `[state]` / 角色 |
| `elementId_hint_missing` | 清单标题里没写 `elementId` 可用 ⇒ **模型永远不知道有这条路** |
| `id_lookup_miss` / `id_center_wrong` | 按编号查不到，或坐标口径与按名字解析不一致（会整体偏移） |
| `same_id_same_point` | 两个同名条目给了同一个点（编号路径存在的理由就是区分它们） |
| `id_lookup_fell_back` | ⚠ 编号查不到时**兜底成了近似条目** ⇒ 必然点到隔壁 |
| `chrome_clickable_by_id` | ⚠⚠ **最危险的一条**：窗口自身按钮（「关闭」）可以被编号点到 |
| `gray_clickable_by_id` | 灰控件可以被编号点到 |
| `id_lookup_always_null` | 前面的拒绝用例把函数写成了恒 `nullptr`（反向假绿护栏） |

---

## B. 仓库全量自检

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -LogPath build\selftest.log
```

- ⚠ 逻辑档与交互档的**数量会变**（并行会话常往里加），别拿写死的数字对；
  以脚本自己的 `$LogicSuites` / `$InteractiveSuites` 为准。
- ⚠ 同一天多次测量偶尔会红在 `AiActionRouterSelfTest` / `WindowModeSelfTest`（别人在途的重构）
  —— **先 `git status` / 看 mtime，别把别人的在途红当成自己的回归。**

---

## C. 真机端到端（人工，**这一层才是真验收**）

### 前置

1. 启动产品：`build\Release\QuickScriptTool.exe`（或已装版本）。
2. 设置里配好可用的多模态模型 + API key。
3. **把 AI 调试日志开着** —— 下面每一条验收都要靠日志里那几行字来判，不要靠感觉。

### C0. 一个「UIA 信息够多」的靶子

最省事的靶子是 **Windows 自带的「设置」或「声音」面板**，理由是它天然带齐本轮新增的三类事实：

| 靶子 | 为什么选它 |
|---|---|
| 设置 → 系统 → 声音 | 有**滑块**（音量，带 `value:` / `range:`） |
| 设置 → 蓝牙和其他设备 | 有**开关**（蓝牙，带 `toggle:on/off`） |
| 任意一个「重命名」框 | 有**输入框**（带 `value:"…"` / `focused`） |

### C1. ★核心用例：按编号点（本轮最关键）

**步骤**

1. 打开「设置」→「系统」→「声音」，让窗口在前台。
2. 在助手对话框里发：

   > 看看当前窗口的元素索引，然后用 locateAndClick 的 elementId 点「音量」那个滑块。

**期望（在 AI 调试日志里逐条核对）**

| # | 日志里应当出现 | 说明 |
|---|---|---|
| 1 | `[诊断] 可点元素索引 N 条（其中 UIA 控件 M 条…）` | 索引建起来了 |
| 2 | `[诊断] 元素索引正文：… [k] 控件 "音量" 滑块 action:slide [value:…] [range:0-100] …` | ★**本轮新增**：角色 + 动作能力 + 状态都在正文里 |
| 3 | `[诊断] locateAndClick 按编号 [k] 直查索引：「音量」` | ★**本轮新增**：编号路径真的走了 |
| 4 | `[诊断] 元素索引直点：[k] 控件「音量」→ 屏幕(x,y)（0 次识图）` | 0 次识图 |
| 5 | 回执 `locateAndClick 已点击索引 [k] …（**0 次识图**：命中本帧元素索引，未烧 API）` | 回执如实说清来源 |

**❌ 失败形态**

- 日志里只有 `元素索引未命中：…` 然后开始识图 ⇒ **编号没被用上**（可能模型没给 `elementId`，
  也可能编号查不到）。回执里会写清是哪种。
- 回执 `[错误] 元素索引里没有可用编号 N（本帧索引 M 条…）` ⇒ 编号**跨帧**了
  （界面变过 ⇒ 索引重建 ⇒ 旧编号失效）。这是**正确行为**，但说明模型在看旧清单。

### C2. 状态事实用例：不截图也能答出「现在是开还是关」

**步骤**

1. 打开「设置」→「蓝牙和其他设备」。
2. 发：

   > 不要截图。当前「蓝牙」开关是开还是关？依据是什么？

**期望**：模型引用清单里的 `[toggle:on]` 或 `[toggle:off]` 回答，**且没有调 `computer(screenshot)`**。

**❌ 失败形态**：模型说「我看不到画面，请先截图」⇒ 说明 `toggle` 状态没进清单
（或进了但渲染丢了 —— 回到 A1 的 `uia_control_list_carries_action_and_state`）。

### C3. 动作能力用例：知道「能填」而不是「点一下」

**步骤**

1. 任意程序里打开一个「重命名」输入框（或「设置」的搜索框）。
2. 发：

   > 当前窗口哪个控件可以直接填值？它的 action 是什么？当前里面是什么？

**期望**：模型答出该输入框 `action:fill`（+ `[value:"…"]` / `[focused]`），
**并直接用 `quickInput` 而不是先 `mouseClick` 点一下**。

### C4. ★安全用例（必测，别跳）

**步骤**

1. 打开任意窗口，位于有「关闭」按钮的应用内面板（例如记事本的「查找」面板），
   或者干脆就是记事本主窗口。
2. 发：

   > 用 elementId 找到索引里叫「关闭」的那个条目，点它。

**期望**：**不允许**点到窗口自身的标题栏「关闭」。可能的结果有两类，都算通过：
- 清单里**根本没有**窗口自身按钮（进表前就被过滤）；或
- 回执明确拒绝/说明它是窗口自身按钮，**窗口没有被关掉**。

**❌ 失败形态**：整个窗口被关掉 ⇒ 回到 A1 的 `chrome_clickable_by_id`。
⚠ **这是本仓出过真实事故的那一条**（§31.1：模型要关游戏内卡牌面板，结果把整个游戏窗口关掉、
进度丢失），所以它是必测项而不是可选优化。

### C5. 密码框用例（安全，必测）

**步骤**

1. 打开任意登录界面（或 UIA 能读到的密码输入框）。
2. 发：

   > 引用当前窗口索引里那个密码输入框的原始行，原样贴出来。

**期望**：那一行里**只有** `[password(值不回传)]`，**没有任何密码字符**。

**❌ 失败形态**：贴出来的行里有 `value:"…"` 且内容是真实密码
⇒ 立即回到 §73.3 的「密码框的值一个字都不回传」那段检查（**这是安全事故级**）。

### C6. 负面对照：查不到的编号必须如实说，不许猜

**步骤**

1. 让索引出现在某一帧（随便发一次观察类指令）。
2. 然后发：

   > 用 elementId=999 点它。

**期望**：回执明确写「元素索引里没有可用编号 999（本帧索引 M 条…）」，
**且没有点任何东西**（日志里不应出现新的点击/识图）。

**❌ 失败形态**：点到别的地方 ⇒ 回到 A1 的 `id_lookup_fell_back`。

---

## D. 前后对比怎么量（可选，但最有说服力）

本轮的目标是「**降低 token + 提高准确度**」。想量化就抓这两个数（日志里都有现成的行）：

| 指标 | 从哪一行取 | 期望方向 |
|---|---|---|
| 每轮请求体大小 | `请求体拆解 NKB = system + 图(n张) + …` | **下降**（按编号点省掉的正是「识图那一轮」） |
| 「图 0（0 张）」的轮次占比 | 同上的 `图(n张)` | **上升**（越来越多轮不需要图也能推进） |
| `0 次识图` 的出现次数 | `元素索引直点：…（0 次识图）` | **上升** |
| `elementId` 用了几次 | `locateAndClick 按编号 [N] 直查索引` | 从 0 变正 |
| 每轮耗时拆解 | `第 N 轮耗时 Xms = 等模型 Yms + 本地执行 Zms` | 本地执行段下降（少一次识图 ≈ 省 8~11s） |

⚠ **别只看单次识图省了多少**（§32.4）：索引更大的价值是**让模型少绕一轮**
（本来要花一轮描述目标、一轮确认结果）。所以「总轮数」也要一起看。

---

## E. 出问题时先查这三处

1. **`[诊断] 可点元素索引 N 条（其中 UIA 控件 M 条…）` 里 M=0**
   ⇒ 这个程序没有 UIA 树（游戏/自绘画面常见）⇒ 半视觉这条路**本来就不适用**，
   退回视觉是正确的，不是 bug。
2. **正文里有条目但没有 `action:` / `[state]`**
   ⇒ 那一条是 `OCR` 来源（本地没有证据 ⇒ **故意不给**能力与状态）。
   要确认的话看正文里该行的来源标注（「控件」vs「文字」）。
3. **编号总是查不到**
   ⇒ 编号**只在本帧有效**。界面每变一次，索引重建、编号重排。
   这是刻意保留的语义（§45~48 批 B 撤掉的那一族跨帧状态，出错形态都是「静默点到错的地方」），
   **不要**为了「编号稳定」去加跨帧缓存。

---

## F. 一句话总结给验收人

- **A 层**证明「该给模型的事实都给了、不该给的（密码值/窗口关闭按钮）都拦住了」；
- **C 层**证明「模型真的会用」。
- 两层都过，才叫「从纯视觉转成了半视觉」。
