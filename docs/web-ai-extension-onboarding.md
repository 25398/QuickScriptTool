# 网页版 AI 的接入与扩展：三项评估（3.1 / 3.2 / 3.3）

> 配套：`docs/web-ai-backend-design.md`（C 方案主体）、`extension/PACKAGING.md`（扩展打包规范）
> 日期：2026-09-25　状态：**评估稿**（结论里有「已核实」与「未实测」两种标注，别混）

---

## 0. 结论先行

| 项 | 结论 | 能不能"一键" |
|---|---|---|
| **3.1 免开发者模式装扩展** | 在**没加域**的普通 Windows 上，**没有**任何"完全静默、零点击"的合法路径（官方文档明确限制，见 §1.2） | ❌ 完全静默做不到；**✅ 但能做到"一次点 3 下 + 全程有指引"**，见 §1.3 的 A 方案 |
| | 唯一能真正静默的是「**先上架 Edge 加载项商店，再用用户级策略强制安装**」——不需要管理员、不需要加域 | ✅ 但**前置是上架**（审核 + 开发者账号），见 §1.3 的 B 方案 |
| **3.2 给豆包做一个 Skill** | **值得做，但它的作用是"让我们少发提示词、让协议更稳"，不是"让豆包能连我们的桥"** —— 豆包的服务端碰不到用户机器的 `127.0.0.1` | — |
| **3.3 桥接 DeepSeek / 千问 / 元宝** | 三家 selector **已经配好并进了扩展**（`web_ai_providers.json`），缺的是**逐家真机标定**；开源参考 `browser-ai-bridge`(MIT) 的**架构我们不能照抄**（它自带一个 Playwright 浏览器，用户得重新登录） | — |

---

## 1. 3.1 免开发者模式安装扩展

### 1.1 现状（痛点是真的）

现在用户必须：`edge://extensions/` → 打开「开发人员模式」→ 「加载解压缩的扩展」→ 选目录。
四个动作、一个开关，而且**开关本身会让浏览器顶部常驻一条黄色警告**（"请停用以开发者模式运行的扩展"）。
对非技术用户来说这一步就是劝退点。

### 1.2 候选路径逐条核实

#### ① 组策略 / 注册表强制安装（`ExtensionInstallForcelist`）

微软官方文档原文（**已核实**，2026-09-25 抓取
<https://learn.microsoft.com/zh-cn/deployedge/microsoft-edge-policies/ExtensionInstallForcelist>）：

> 设置此策略以指定**无需用户交互、无提示安装**的应用和扩展的列表。用户无法卸载或关闭此设置。
> …
> **注意: 对于未加入 Microsoft Active Directory 域的 Windows 实例，强制安装仅限于
> Microsoft Edge 外接程序网站中列出的应用和扩展。**

> 策略的每个列表项都是一个字符串，其中包含一个扩展 ID（可选）以及一个可选的"更新"URL（用分号 (;) 分隔）。
> …"更新"URL 应指向更新清单 XML 文档…**更新 URL 应使用以下方案之一：http、https 或文件。**

注册表位置：`SOFTWARE\Policies\Microsoft\Edge\ExtensionInstallForcelist`，值名 `1`、`2`、…

**⇒ 关键结论**：那句话把自托管路堵死了。
普通用户机器**没有加域** ⇒ 策略只认**商店里已上架的**扩展。
"自己写个 update_url 指向我们的服务器"这条路，**在非域机器上不生效**（这是微软的硬限制，不是我们配置的问题）。

⚠ 补充：Chromium 的策略加载器同时读 `HKLM\...\Policies` 与 `HKCU\...\Policies`
（所以**理论上不需要管理员权限**）。但"非域 ⇒ 只认商店"这条限制与权限无关，换 hive 也绕不过。
（HKCU 是否被 Edge 采纳，本机**未实测**；写 HKCU\Software\Policies 也可能被安全软件告警。）

#### ② 命令行 `--load-extension`

**已核实**（来源：`mozilla/web-ext` issue #3388 的逐版本实测记录）：

| 版本 | 变化 |
|---|---|
| Chrome 116+ | **开启「增强型安全浏览」时**，`--load-extension` 被禁用 |
| Chrome 120+ | 由**企业策略**设置时被禁用 |
| Chrome 134+ | **开发者模式关闭时**，用该参数装的扩展在**重载后被停用** |
| **Chrome 137+** | **官方构建里默认禁用**（非官方 Chromium 构建暂时还能用） |

⚠ 这些是 **Chrome** 的实测数据。Edge 同为 Chromium 系、`extension_service.cc` 是共用代码，
**大概率同形，但本机 Edge 版本上未实测**（要测就一条命令：带 `--load-extension=<目录>` 起 Edge，看扩展在不在）。

**⇒ 结论**：不能把产品赌在这条路上。它是"当年能用、现在被围剿"的通道。

#### ③ CDP `Extensions.loadUnpacked`

**已核实**（同源）：Chrome 126+ 引入，是 `--load-extension` 的官方替代品。要求：

- 浏览器必须以 **`--remote-debugging-pipe`** 启动（**不是** `--remote-debugging-port`）
- 且必须加 **`--enable-unsafe-extension-debugging`**；不加则回
  `{"error":{"code":-32000,"message":"Method not available."}}`
- ⚠ 用 `--remote-debugging-pipe` 会让 `navigator.webdriver === true`
  ⇒ **站点可以据此判定"这是自动化"**，可能影响登录/风控

**⇒ 结论**：技术上可行，但要求我们**控制浏览器的启动**（用户得先关掉自己的 Edge，再由我们带参数重启）。
代价：抢走用户的浏览器会话 + 可能触发站点风控。**不做主路径，最多作为"高级选项"。**

#### ④ 拖入 `.crx`

未签名 `.crx` 拖进 `edge://extensions/` 仍要求开发者模式（实测经验，未做本轮核实）。
即便可行，**开发者模式那条黄色警告**还在 —— 痛点没解决。

#### ⑤ 上架 Edge 加载项商店

- 用户侧：一次「获取」点击（无需开发者模式、无警告条）
- **叠加 ① 的用户级策略 ⇒ 连这一次点击都能省掉**（因为已上架，非域限制不再适用）

**⇒ 这是唯一能同时满足"零警告 + 可真静默"的路。**

### 1.3 建议：分两层做，先做今天就能落地的

#### A 方案（**今天就能做，零外部依赖**）：引导式安装

我们**已经有全部零件**，缺的只是把它们串起来：

1. **入口**：设置 →「AI助手」加一行状态 + 一个按钮「安装浏览器扩展」。
2. **检测**：读桥的 `/qst/status` 的 `extConnected` / `extClients`
   （**这是唯一可靠判据**，见设计文档 §11.3）。
3. **引导**（未连接时）：
   - `ShellExecuteW` 打开 `edge://extensions/`
   - 用**我们自己的原生叠层窗口**（`src/desktop_tools/` 里那套选区/准星叠层是同款机制，
     它**不依赖扩展**，所以"扩展没装"时它照样能用）盖在 Edge 窗口上，标出
     「开发人员模式」开关与「加载解压缩的扩展」按钮的位置 + 三步文字。
   - 同时把**扩展目录绝对路径复制到剪贴板**，并给一个「打开扩展目录」按钮
     —— 用户在文件选择框里直接 `Ctrl+V` 回车即可。
4. **确认**：轮询 `extConnected`（2 秒一次、最多 3 分钟），一旦为真立刻提示「安装成功」并自动关闭引导。
5. **兜底文案**：失败时给出 `build\Release\extension\edge` 的确切路径与手动步骤截图链接。

**为什么这是对的**：不碰系统策略、不要管理员、不要上架、不要开发者模式以外的任何东西，
而且**把"找目录、找按钮"这两件最容易出错的事替用户做了**。用户实际只剩"点开关 + 点按钮 + 粘贴路径"。

#### B 方案（**需要先上架**）：商店 + 用户级策略静默安装

1. 上架 Edge 加载项商店（开发者账号 → 提交 → 审核）。
2. 安装包里带一个**可选**的「免点击安装」开关（默认**关**）：
   写 `HKCU\Software\Policies\Microsoft\Edge\ExtensionInstallForcelist\1 = <商店扩展ID>`
3. 卸载时**必须**删掉这个键（否则用户卸载软件后扩展还在、还卸不掉 —— 这是产品事故）。

⚠ **合规提醒**：写 `HKCU\Software\Policies` 是"改浏览器策略"，杀软可能告警，
而且它会让 Edge 显示「由你的组织管理」。**必须显式征求用户同意，不能默认开**。
（`MEMORY.md` §14：接外部软件必须用户显式配置。）

### 1.4 一句话建议

**先做 A（成本低、覆盖全部用户），把 B 当"上架之后再说"的增强。**
别在 `--load-extension` / `Extensions.loadUnpacked` 上花时间 —— 前者正在被移除，
后者要抢用户的浏览器会话且会置 `navigator.webdriver`。

---

## 2. 3.2 给豆包做一个 Skill

### 2.1 豆包 Skill 是什么形态（**已核实**，来自用户提供的界面截图）

`https://www.doubao.com/chat/skills` → 「上传技能」→ 拖入**压缩包或文件夹**，要求：

> · 压缩包或文件夹需要包含 `SKILL.md` 文件
> · `SKILL.md` 需包含 **YAML 格式的技能名称和描述**

⇒ 就是 Anthropic 那套 **Agent Skills** 格式：一个 zip，根目录有 `SKILL.md`，
顶部是 YAML frontmatter（`name` / `description`），正文是给模型看的操作说明，
可以再带 `scripts/`、`references/` 等资源。

### 2.2 ⚠⚠ 先纠一个方向性误解：Skill **连不上**我们的桥

一个必须说清的前提：**Skill 是"给模型的说明书"，不是"给模型的网络能力"。**

- 豆包跑在**字节的服务器**上，它的沙箱**碰不到用户机器的 `127.0.0.1`**。
- 所以"让 Skill 去调我们的 `/v1/chat/completions`" —— **不成立**。
- 我们的桥的方向是**反的**：**我们**（用户机器上的 QuickScriptTool）主动去驱动**浏览器里的豆包页面**。
  数据流是 `我们的进程 → 扩展桥 → CDP → 豆包页面`，全程不经过豆包的服务端。

**所以 Skill 能帮上忙的地方是另外两件**（而且这两件价值都不小）：

### 2.3 Skill 真正能解决的两个问题

#### ① 让"工具调用协议"从**每轮重发**变成**常驻指令**

现状（设计文档 §5.1 / §5.3）：我们没有原生 function calling，全靠把
`<<<QST_TOOL_CALLS>>>` 协议说明**塞进每一轮的提示词前缀**里。
代价：
- 系统提示词 + 36 个工具定义约 **30~50 KB**，每轮都要写进输入框；
- 输入框有长度上限（**未实测**），超了会被截断；
- 模型偶尔会漏掉格式（我们靠 `ParseReply` 的容错去兜）。

**把协议写进 Skill** ⇒ 豆包把它当**长期指令**，我们就不用每轮重发那一大段。
省的是 token，降的是截断风险，提的是格式稳定性。**这是实打实的收益。**

#### ② 给用户一个"显式开关"

用户能在豆包侧看到「键鼠工坊」这个技能、能自己开/关它。
这比"程序偷偷往输入框里塞一段协议说明"更符合
`MEMORY.md` §14「接外部软件必须用户显式配置」的精神。

### 2.4 建议的 Skill 结构（草案）

```
qst-doubao-bridge/
├── SKILL.md                 # 必需：YAML frontmatter + 协议说明
├── references/
│   └── tool-protocol.md     # 完整协议（工具名/参数/示例），SKILL.md 里只留摘要
└── scripts/                 # 暂不需要（Skill 里跑不了我们的东西）
```

`SKILL.md` 草案要点（**内容必须与我们 `web_ai_prompt.cpp` 的协议严格同源**）：

```markdown
---
name: 键鼠工坊
description: 与「键鼠工坊」桌面自动化软件配合：当用户要求操作本机软件/桌面时，
             按本技能定义的协议输出工具调用块，由该软件在用户电脑上执行。
---

# 何时用
用户提到「操作电脑 / 点击某个按钮 / 帮我自动填表 / 键鼠工坊 / QST」时。

# 输出协议
需要调用工具时，**只**输出下面这种块（可多个），块外不要写多余解释：
<<<QST_TOOL_CALLS>>>
[{"name":"mouseClick","arguments":{"x":640,"y":360}}]
<<<END_QST_TOOL_CALLS>>>

不需要调用工具时，直接用自然语言回答。

# 铁律
- 协议块的 JSON **必须是合法 JSON**（半截 JSON 会被整块丢弃）
- 一次可以给多个调用，但**不要**重复调用同一个
- 需要点击时优先用**屏幕上的文字标签**作为目标，坐标尽量由软件侧解析
```

### 2.5 ⚠ 未实测 / 风险（别当结论）

1. **Skill 是否作用于普通对话**：豆包 Skill 的触发机制（是"用户 @ 技能"还是"自动匹配描述"）
   **未实测**。如果只在特定入口生效，那我们的主链路（`/v1/chat/completions` 直接写输入框）
   未必吃得到这份指令 ⇒ **收益可能为零**。**必须先做一次真机验证再决定投入。**
2. **维护成本**：协议一改，Skill 与 `web_ai_prompt.cpp` 两处都要改 ⇒ 必须加一条
   **同源守卫**（比如自检里断言 Skill 文件里的协议标记 == `kToolCallsBegin/kToolCallsEnd`）。
   本仓对"两处抄同一份必然漂移"已有明确教训（`AGENTS.md` 里 Suite 表就因此只留一处）。
3. **上架/分发**：Skill 是给用户上传的 zip ⇒ 要随安装包分发，并在引导里教用户上传。

**⇒ 建议**：**先做一次真机验证**（花 30 分钟确认 Skill 的指令能不能影响我们写进去的那一轮），
能影响再投入做；不能影响就**不做**，把精力放回 3.3。

---

## 3. 3.3 桥接 DeepSeek / 千问 / 元宝

### 3.1 现状：配置已经就位，缺的是真机标定

`extension/edge/web_ai_providers.json` **已经有三家**（不是待做，是待验）：

| 站点 | `inputKind` | 输入框候选 | 状态 |
|---|---|---|---|
| `doubao` 豆包 | richtext | `textarea[data-testid='chat_input_input']` … | **已真机打通**（2026-09-25） |
| `deepseek` DeepSeek | textarea | `textarea`、`div[contenteditable]` | 配置就位，**未真机标定** |
| `yuanbao` 腾讯元宝 | richtext | `div[contenteditable][role=textbox]` … | 配置就位，**未真机标定** |

⚠ 所有 selector 都是**推测值**（除豆包外）。这正是本项目的老问题：
"selector 是猜的"必须在文档里标出来，不能当已知。

**千问（Qwen）目前没有配置** —— 要新增一个 provider 段。

### 3.2 与开源方案对比：`chen-squared/browser-ai-bridge`（MIT）

**已核实**（抓取其 README）：

| | browser-ai-bridge | 我们（C 方案） |
|---|---|---|
| 驱动方式 | **Playwright** 起一个**独立的** Chromium（持久化 profile） | 扩展桥 + CDP，驱动**用户自己已经登录的** Edge |
| 登录 | **用户要在它弹出的浏览器里重新登录** | **白拿用户的登录态**（这是我们的核心优势） |
| 支持的站点 | DeepSeek / ChatGPT / Gemini / Claude / Grok / Qwen | 豆包 ✅ / DeepSeek、元宝（待验） |
| 配置 | `selectors.overrides.json` + `POST /providers/reload` **热重载** | `web_ai_providers.json`，改完要**重载扩展** |
| 已知限制 | 串行、无流式、system 不注入、selector 脆弱、会话不持久 | 同样串行（桥的 `waitingId_` 全局唯一）、**已有** SSE 构造 |
| 许可 | MIT | — |

**两个可直接借鉴的点**（都已核实存在）：

1. **selector 热重载**：它 `POST /providers/reload` 就能重读配置、**不用重启**。
   我们目前改 `web_ai_providers.json` 必须去 `edge://extensions/` 点 ⟳。
   ⇒ **值得抄**：让扩展监听 `chrome.storage.onChanged` 或加一个 `webAiReloadProviders` 消息，
   宿主侧做一个「站点适配更新」按钮。
2. **`promptMode` 的三种策略**（`latest-user` / `trailing-users` / `full-messages`）：
   我们只有"续轮只发新增"一种。遇到"网页自己丢了上下文"时，它这套更灵活。
   ⇒ 可作为**兜底策略**参考，不必照搬。

**⚠ 明确不借鉴的**：它自带浏览器这件事。我们的产品边界是
"用用户**已经登录**的浏览器"（`MEMORY.md` §14 + 设计 §6），
让用户为了用 AI 再登录一遍，等于把最大优势丢掉。

### 3.3 落地清单（按优先级）

1. **逐家真机标定**（DeepSeek 优先，因为它 `inputKind=textarea`，最可能一次就通）：
   跑 `POST /qst/web-ai/probe`（产品进程内那个真探针），看 `[4] 写入回读` / `[6] 读回回答` 两段。
   - 失败就按 `webAiProbe` 的回报更新 selector（这是**维护成本**，不是 bug）
2. **新增千问**：`providers.qwen = { url: "https://chat.qwen.ai/", inputKind: "richtext", … }`
   ⚠ 千问的 selector 需要现场用 DevTools 取，**不能抄 browser-ai-bridge 的**
   （它跑在 Playwright 里，注入上下文与扩展不同，但 selector 本身可以交叉验证）。
3. **传图通道**（已完成，见设计文档 §13.6 更新）：三家都已加 `fileInputSelectors` /
   `fileButtonSelectors`，同样需要真机标定。
4. **（可选）selector 热重载**，见 §3.2 第 1 点。
5. **模型名 → 站点映射**已就绪（`ProviderFromModelName` 认
   `doubao/豆包/seed`、`deepseek/深度求索`、`yuanbao/元宝/hunyuan`）
   ⇒ 新增千问要**同时**加 `qwen/千问/通义` 分支，否则用户选 `qwen-web` 会被判成"认不出站点"。

### 3.4 风险（如实列出）

- **风控**：网页端 AI 普遍有反自动化。我们是"在用户自己的浏览器、用用户自己的账号、走真实按键"，
  风险显著低于无头浏览器，但**不是零**。应在 UI 里如实告知。
- **改版**：selector 一定会失效。这是这类方案的**固有成本**，必须把"更新适配"做成一个用户可点的动作，
  而不是等用户报障。
- **合规**：驱动第三方网页服务属于"借用用户已有会话"，**不代用户注册、不绕过风控、不批量刷量**。
  `MEMORY.md` §14 与设计 §6 的边界不变。

---

## 4. 明确不做的事

- ❌ 不把 `--load-extension` / `Extensions.loadUnpacked` 作为安装主路径（前者正被移除，后者要抢会话 + 置 `navigator.webdriver`）
- ❌ 不默认写 `HKCU\Software\Policies`（改浏览器策略必须用户显式同意）
- ❌ 不抄 browser-ai-bridge 的"自带浏览器"架构（会丢掉"白拿登录态"这个核心优势）
- ❌ 不在没有真机验证前宣称 Skill 方案有效
