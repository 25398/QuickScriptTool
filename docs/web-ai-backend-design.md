# 网页版 AI 后端（Web AI Backend）设计

> 起因：用户问「用户下载好软件就能直接用 AI 功能，无需配置 API Key」。经评估（2026-09-24）选定
> **C 方案：驱动用户本机已登录的 Edge/Chrome 里的网页版 AI**，不做云端中转、不内置无头浏览器。
>
> **验收口径（用户原话）**：「这只是给用户体验一下我们这些 AI 功能能做些什么……
> AI 脚本助手 + AI 动作执行 + AI 图片分析 + AI 文字分析，能保证最低实现就可以了。
> 比如 AI 动作执行，能让豆包通过我们的软件理解图形界面，然后定位图形界面上的某个地方，并点击，这就足够了。
> 不需要规划过长的任务，比如打游戏之类的。」
>
> ⚠ **本文数字分三类，不许混用**：① **本仓实测**（注明文件:行）；② **外部项目文档**
> （注明仓库/文章）；③ **推算**（**必须标「推算」**）。**未实测项一律显式标注。**

---

## 0. 一句话结论

| 问题 | 答案 |
|---|---|
| 有没有现成开源可抄？ | **有，而且架构高度吻合**：`chen-squared/browser-ai-bridge`（MIT）与 `ChatGPT-Bridge + ChatGPT-Executor`（插件 + 本地服务）与我们的形态几乎 1:1（§2）。 |
| 抄哪一层？ | **抄「架构分层 + 配置化 selector + 提交确认/忙闲判据」**（§2.3）。**不抄实现**（它们是 TS/Playwright，我们是 C++/`chrome.debugger`）。 |
| 我们比它们强在哪？ | 我们**已经有** `chrome.debugger`(CDP) + 页面快照 + `clickRef`/`typeRef` 全套原语（`extension/edge/background.js`），**不需要 Playwright、不需要另起浏览器**（§3）。 |
| 核心接入点？ | **不改 `agent_core.cpp` 的 HTTP 层**。在 `AiApiSettings.savedModels` 里加一条「`modelName` 含 `doubao`、`apiUrl` 指向本机回环」的档案 ⇒ **模型能力判定按名字、与 URL 解耦** ⇒ 全链路零核心改动（§4）。 |
| 最大的技术风险？ | ⚠⚠ **豆包输入框是富文本（contenteditable），不是 `textarea`** —— 现有 `qstFillRef` 的「设 value + 派发 input 事件」对富文本**大概率失效**（§5.1）。这是全案第一优先要实测的事。 |
| 验证顺序？ | 见 §7：四步冒烟，**第 1 步失败则整案需重估**。 |

---

## 1. 问题重述与边界

### 1.1 要什么

| # | 功能 | 最低验收 |
|---|---|---|
| 1 | **AI 文字分析** | 输入一段文字 → 得到一段回答 |
| 2 | **AI 图片分析** | 给一张图 + 一句问题 → 得到一段回答 |
| 3 | **AI 脚本助手** | 对话式生成/优化脚本（走现有 `BuildDefaultAgentTools()` 36 个工具） |
| 4 | **AI 动作执行** | 「理解图形界面 → 定位某个地方 → 点击」（**不需要长任务规划**） |

### 1.2 不要什么（明确排除）

- ❌ 云端中转服务器（违反「零服务器」前提，且违反 `MEMORY.md` §14）
- ❌ 内置无头浏览器（+250MB 装机体积，且**仍需用户登录**，违反 §14）
- ❌ 长时间自主规划（用户明确说「不需要规划过长的任务」）
- ❌ **产品默认依赖它**：必须是**用户显式开启的可选项**（§6）

---

## 2. 外部开源实现调研（可抄什么、不抄什么）

### 2.1 三个最相关的项目

| 项目 | 形态 | 驱动方式 | 优点 | 与我们的差距 |
|---|---|---|---|---|
| **`chen-squared/browser-ai-bridge`**（MIT） | 本地 HTTP 服务 + Playwright | Playwright 持久化 profile | **架构最接近我们**：本地 `POST /v1/chat/completions` → DOM 操作 → OpenAI 兼容 JSON。**README 把坑全写出来了** | 用 Playwright **另起** Chromium ⇒ 用户要**重新登录**；我们**不需要** |
| **`ChatGPT-Bridge` + `ChatGPT-Executor`** | 浏览器插件 + 本地服务（WS :8181） | 扩展注入 + DOM 操作 | **形态与我们 1:1**：插件在浏览器内操作页面，本地服务对外出 API | 只支持 ChatGPT；WS 是**插件当服务端**（我们是宿主当服务端，更安全） |
| **`clear2x/openbrowserharness`**（MIT） | 纯扩展 MV3，harness 跑在扩展里 | **`chrome.debugger`(CDP)**，明确「no OS-level automation」 | **驱动方式与我们完全一致**；声明快照**可穿透 Shadow DOM 与 iframe** | 它是**消费者侧 agent**，不做「网页 AI → API」转换 |

**结论**：**架构抄 `browser-ai-bridge`，通信形态我们对齐 `ChatGPT-Bridge` 的「本地服务为主、扩展为从」，驱动层用我们已有的 `chrome.debugger`。**

### 2.2 `browser-ai-bridge` 的分层（建议照搬）

```
外部程序
  │  POST /v1/chat/completions
  ▼
本地 HTTP 服务        ← 路由 / 会话 / 格式转换
  │  Provider 驱动
  ▼
浏览器（持久化会话）
  │  DOM 操作
  ▼
网页版 AI
```

它源码的分层（README「开发」节原文）：

```
src/
├── server.ts                    # HTTP 服务入口（路由、会话同步、SSE）
├── prompt.ts                    # 消息规范化逻辑
├── browser/
│   ├── browser-manager.ts       # 浏览器生命周期管理（启动、页面复用）
│   ├── provider-client.ts       # DOM 交互（定位输入框、发送、提取回复）
│   └── markdown-restoration.ts  # Markdown token 还原
└── providers/registry.ts        # 各 provider 的 selector 配置与覆盖逻辑
```

**这四层可以直接映射到我们的 C++ 侧**（§3.2）。

### 2.3 ★ 最值得抄的四件事（都是它 README 里写明的踩坑结论）

#### (1) **Provider 配置化 selector**（最高价值）

它的 selector 配置结构（原文引用 `selectors.overrides.example.json`）：

```json
{
  "deepseek": {
    "inputSelectors":    ["textarea", "div[contenteditable=\"true\"][role=\"textbox\"]"],
    "sendButtonSelectors": ["button[type=\"submit\"]", "button[aria-label*=\"发送\"]"],
    "responseSelectors": ["dummy"],
    "busySelectors":     ["button[aria-label*=\"停止\"]"]
  }
}
```

**四个字段的语义（照抄）**：

| 字段 | 语义 | 关键点 |
|---|---|---|
| `inputSelectors` | 输入框候选**列表** | **按顺序取第一个可见的** ⇒ 天然兼容「富文本 vs textarea」两种情况 |
| `sendButtonSelectors` | 发送按钮候选 | **全部不可点击时退化为按 Enter** |
| `responseSelectors` | 回复容器候选 | 应指向**完整 assistant 回复节点** |
| `busySelectors` | **是否仍在生成** | ⚠ README 明确写「**必须配准**，否则活跃检测失效」 |

**selector 编写优先级（照抄）**：
> `aria-label` / `data-testid` / `role` > `placeholder` / `name` / `type` > `class`
> ❌ 避免编译产物类名（`.c3f91a._ab12.xYz9`，随版本变化）

**并且它把 selector 做成「可热重载的覆盖文件」**（`POST /providers/reload`，不用重启服务）。
⇒ **网页改版是这类方案的固有成本，必须设计成「改配置不改代码」。这条我们要照抄。**

#### (2) **三个 timeout 的分工**（直接解决我此前「怎么判回答完成」的疑问）

| 参数 | 默认 | 语义 |
|---|---|---|
| `submissionSignalTimeoutMs` | 8000 | 点击发送后，等待「**Stop 按钮出现 / 输入框清空 / 新响应出现 / URL 变化**」**任一信号**；超时视为该次点击失效。⚠ **一旦某次点击无异常地触发过，后续不再叠加尝试，避免重复发送耗尽 quota** |
| `progressIdleTimeoutMs` | 30000 | **空闲超时**：内容无变化 **且** 不处于忙碌状态，持续超过此值则放弃等待。只要内容仍在更新**或** `busySelectors` 命中，计时器持续重置 ⇒ **不会因响应过慢而误截断** |
| `maxGenerationTimeoutMs` | 600000 | 总时长上限（防真正卡死时永久阻塞） |

⇒ **我之前设计的「文本连续 2 次相同判完成」太弱**（豆包末尾会追加「相关推荐」等非正文，会误判）。
**改用 `busySelectors`（停止生成按钮）+ 内容稳定双条件**，这是它们踩过坑的结论。

#### (3) **System prompt 默认不注入**

> 原文：「默认情况下，`system` 消息**不会**被注入到网页输入框。这是有意为之的设计选择——将 system 消息拼入聊天框只能以普通文本形式注入，**既无法等同于模型原生的 system role**，也容易造成网页视觉混乱。」

⇒ 但**我们必须注入**（要带 36 个工具定义）。所以要走它的 `injectSystemOnFirstTurn: true` 路径：
**只在首轮把 system 作为文本前缀一次性拼进去**，后续轮次依赖网页自身维持上下文。

#### (4) **会话复用策略**

- 只把**最后一条 user 消息**写进输入框（避免 API 历史与网页历史**叠加导致上下文重复**）
- `conversationId` 是**本地页签复用键**，不传给模型
- 会话**仅存内存**，重启失效；页签被关会自动导航回入口页重试
- **串行处理**：同一 provider 的请求排队，**不支持并发**

#### (5) 已知限制（原文，接受即可）

> 串行处理 / **无流式响应** / System 消息默认不注入 / Selector 脆弱性 / 登录状态需人工介入 / 会话非持久化

⚠ **「无流式响应」我们要正视**：`AgentCore::CallApiStream` 期望 SSE。但 `CallApi`（非流式）路径是现成的，
且 `AgentSendCallbacks` 里有 **`preferNonStream`** 开关（`agent_core.h:131`）⇒ **走非流式即可，不必伪造 SSE**（§3.3）。

### 2.4 明确**不抄**的

| 不抄 | 原因 |
|---|---|
| Playwright 另起浏览器 | 会让用户**重新登录**；我们已有扩展 + 用户的浏览器 |
| TypeScript/Node 实现 | 我们是 C++ 单体产品，不引入 Node 运行时 |
| 「会议编排」「多 provider 并发」 | 超出「最低实现」范围 |
| 它的 SSH/远程部署章节 | 不适用 |

### 2.5 我们相对它们的**结构优势**

| 维度 | `browser-ai-bridge` | **我们** |
|---|---|---|
| 浏览器 | Playwright **新起** Chromium | **用户现有 Edge/Chrome 标签页**，登录态直接复用 |
| 驱动 | Playwright API | **`chrome.debugger`(CDP)**，已在 `background.js` 里跑通 |
| 页面理解 | selector 取回复容器 | **额外有**「可交互节点枚举 + ref 引用」快照（`qstBuildPageSnapshot`） |
| 装机增量 | Node + Playwright + Chromium ≈ 数百 MB | **0**（扩展已随包分发） |

⇒ **我们的驱动层已经比它强**，只缺「网页 AI → OpenAI 格式」这一层转换。

---

## 3. 我们的架构（对齐它们的分层）

### 3.1 总体

```
 ┌─ AgentCore（现有，只当它是 OpenAI 客户端，不改 HTTP 层）
 │    POST http://127.0.0.1:<port>/v1/chat/completions
 ▼
 ┌─ 新增：src/web_ai/web_ai_backend.{h,cpp}      ← 对标它的 server.ts
 │    ① HTTP 端点（只绑 127.0.0.1 + token）
 │    ② messages → prompt 规范化                 ← 对标 prompt.ts
 │    ③ 调驱动层，拿到回答
 │    ④ 回答 → OpenAI 兼容 JSON（含 tool_calls 反解析）
 ▼
 ┌─ 新增：src/web_ai/web_ai_driver.{h,cpp}       ← 对标 provider-client.ts
 │    读 provider 配置 → 走 ExtBridgeServer::Request() 发指令
 ▼
 ┌─ 扩展（现有 + 新增 2 个消息类型）              ← 对标它内嵌在 provider-client 里的 DOM 操作
 │    background.js: navigatePage / observePage / typeRef / clickRef（已有）
 │                 + readAssistantReply / attachImageToChat（新增）
 ▼
 ┌─ 网页版豆包 / DeepSeek / 元宝
```

### 3.2 分层映射表

| `browser-ai-bridge` | 我们的对应物 | 状态 |
|---|---|---|
| `server.ts`（路由/会话） | `src/web_ai/web_ai_backend.cpp` | **新增** |
| `prompt.ts`（消息规范化） | `src/web_ai/web_ai_prompt.cpp`（纯函数，可单测） | **新增** |
| `browser-manager.ts`（浏览器生命周期） | `ExtBridgeServer` + 扩展 `navigatePage` | ✅ **已有** |
| `provider-client.ts`（DOM 交互） | `background.js` 的 `qstBuildPageSnapshot` / `qstFillRef` / `qstClickRef` | ⚠ **大部分已有，缺「读回复」与「传图」** |
| `providers/registry.ts`（selector 配置） | `src/web_ai/web_ai_providers.json`（随包分发） | **新增** |
| `selectors.overrides.json`（热重载覆盖） | 同上的用户覆盖层（`AppDir()\web_ai_selectors.json`） | **新增** |
| `markdown-restoration.ts` | **不需要**（我们直接取 `textContent`） | — |

### 3.3 关键决策：走**非流式**

`AgentSendCallbacks` 已有 **`preferNonStream`**（`agent_core.h:131`）⇒ 网页 AI 档案强制走
`CallApi`（非流式，`agent_core.cpp:881`），**不需要伪造 SSE 分片**。

⇒ 规避了我此前判断的最大坑（伪造 SSE 时 `tool_calls` 增量拼接出错 ⇒ **静默、偶发**）。

### 3.4 线程与超时（已取证）

| 事项 | 结论 | 依据 |
|---|---|---|
| 桥调用是否阻塞 | **是**，`cv_.wait_for` 分片等待 | `ext_bridge_server.cpp:1039-1057` |
| 执行线程 | **引擎 worker 线程**（非 UI、非桥线程） | `engine_script_run.cpp:651` `StartActionsWorker` → `:792` `worker_ = std::thread(...)` |
| 会不会死锁 | **不会**：桥线程只 `PostMessageW` 后立即返回 | `engine_host_window.h:3073` |
| 重入保护 | **无**（`Request` 全文无 `GetCurrentThreadId`）⇒ 我们**必须**保证不在桥线程里调它 | `ext_bridge_server.cpp:1093` |
| `recvTimeoutMs` | 默认 **120 s**，且按 maxTokens 放大 | `agent_core.h:80`、`ComputeEffectiveRecvTimeoutMs` |

⚠ **超时要对齐**：网页一轮可能 5~60 s。桥的 `Request` 默认 8 s，**必须显式传大超时**（建议 ≤ 180 s，
且要能被 `AbortPending()` 打断 —— 已有该机制）。

---

## 4. 接入点：为什么 `agent_core.cpp` 零改动

三条**已取证**的事实让「零核心改动」成立：

### 4.1 模型能力判定**按名字、与 URL 解耦**

`agent_ai_actions.cpp:23` `ModelExistsInSettings` **只查 `savedModels` + `ai.modelName`，不校验 URL**；
`ModelSupportsVision`（`:62`）**纯按模型名字符串**推断，白名单含 **`doubao`/`seed`/`volces`/`ark.cn`**。

⇒ 在 `savedModels` 加一条：

```jsonc
{
  "modelName": "doubao-web",          // 含 doubao ⇒ 自动被判「支持视觉」
  "apiUrl": "http://127.0.0.1:19260/v1/chat/completions",  // ← 本机回环
  "apiKey": "<本机桥 token>",          // 必须非空（§4.3）
  "temperature": 0.3,
  "maxTokens": 8192
}
```

⇒ **视觉能力判定、`ClampApiMaxTokens`、thinking 策略全链路自动按「支持识图的豆包」处理。**

### 4.2 本地回环被封**只影响 `fetchWebPage`，不影响 `CallApi`**

`agent_web.cpp:166` `Ipv4IsBlocked` 把 `127.0.0.0/8` 全判禁 —— 但那是 **`fetchWebPage` 工具自己的守卫**。
`CallApi` 走 `WinHttpConnect`（`agent_core.cpp:924`），**没有这层黑名单**。

⇒ **回环接入点可行，不会撞墙。** ✅

### 4.3 `apiKey` 必须非空

`agent_core.cpp:1651-1666` 三连检查：`apiUrl`/`apiKey`/`model` 任一为空 → 直接返回 `[错误] 未配置…`
⇒ 填**桥 token**（到了本地后端就被吞掉，不外发）。

### 4.4 三条 `AgentCore` 构造路径都要覆盖

| 路径 | 位置 |
|---|---|
| `AiSessionStore` slot 复用 | `ai_action_runtime.cpp:48` |
| `CreateAiAnalyzeCore` | `ai_action_service.cpp:~930` |
| `CreateAiActionExecuteCore` | `ai_action_service.cpp:989` |

统一加一层 `ResolveEffectiveApiConfig(profile)`：若是「网页 AI」档案，改写 `apiUrl`/`apiKey` 指向本机。
**这样才保证四个功能全覆盖。**

### 4.5 附图链路**完全现成**（AI 图片分析）

`AgentExtractImageMarkers`（`agent_core.cpp:2041`）解析工具结果里的 `[[AGENT_IMG:路径]]`
→ `ModelSupportsVision` 为真 → `AgentBuildImageParts`（`agent_attachment.cpp:398`）编码成 `image_url` parts。

⇒ **只要桥接工具回传 `[[AGENT_IMG:<绝对路径>]]`，就把图喂给模型，`agent_core.cpp` 零改动。**

⚠ 但**图片最终要塞进网页版豆包的输入框** —— 这是 §5.2 的未验证风险。

---

## 5. ⚠⚠ 两个必须实测的风险（不许当结论）

### 5.1 ★★★ 最高优先级：豆包输入框是**富文本，不是 `textarea`**

**外部证据**（知乎《别再用 textarea 了：看看豆包是怎么做输入的》，2025-12）：

> 传统的文本输入组件（`input` / `textarea`）已无法满足 AI 项目需求。
> **豆包**的做法是**富文本编辑器**，本质流程是「用户输入 → 文档结构（JSON/AST）→ Prompt 生成」。

**这与我们的现有实现直接冲突**：

- `qstFillRef`（`background.js:3638`）走的是「**设 `value` + 派发 `input` 事件**」路线
- 富文本（`contenteditable`）**没有 `value` 属性** ⇒ **大概率静默失效**（表现为「填进去了但发不出去」或「输入框仍是空的」）

**影响面**：`typeRef` 三重兜底里的第 ①层失效 ⇒ 只剩「合成 KeyboardEvent」与「CDP `Input.dispatchKeyEvent`」。
后者（CDP 真实按键）**理论上能进富文本**，但**逐字符输入与 IME 提交仍存疑**。

**必须实测**（§7 第 1 步）：对 `https://www.doubao.com/chat/` 的输入框：
1. 用 `observePage` 看它报的 `role`/`tagName` 是什么
2. 试 `qstFillRef` → 看是否真的填进去
3. 试 `dispatchTrustedEnter`（CDP）→ 看是否发出

**若①成功**：整案顺，改动小。
**若①失败但 CDP 成功**：`readAssistantReply` 之外还要**新增「富文本输入」注入函数**，工作量 +1~2 天。
**若都失败**：C 方案在豆包上**不成立**，需回退到 `DeepSeek`（它的配置里有 `textarea` 候选）或重估。

### 5.2 传图（AI 图片分析）

豆包上传控件是 `<input type="file">`。CDP 有 `DOM.setFileInputFiles` **理论可行**，
但我**没有实测过豆包页面的 DOM 结构** ⇒ **「理论可行、未取证」**。

### 5.3 其他未验证项

- 引擎的 system prompt + 36 工具定义约 **30~50 KB 字符**，豆包输入框是否有限制、会不会截断 —— **未实测**
- 网页 AI 一轮 5~60 s，**超时要实测标定**（桥 `Request` 默认 8 s 明显不够）

---

## 6. 合规与产品边界（`MEMORY.md` §14 红线）

§14：**产品不依赖别人的软件运行；接外部软件必须用户显式配置。**

⇒ 落实为：

1. **默认关闭**。设置页加「使用本机浏览器里的 AI（实验性）」开关，**用户点了才写 `savedModels` 那条档案**
2. 文案明说「**借用你已登录的网页版 AI，非官方 API**」
3. 文档说明「网页改版可能导致失效，需更新选择器配置」
4. **不把它设成任何功能的唯一路径** —— 用户随时可切回正规 API

---

## 7. 落地路线：四步冒烟（**失败即止损**）

| 步 | 目标 | 成功判据 | 失败处置 |
|---|---|---|---|
| **1** | **豆包输入框可写可发** | `observePage` 拿到输入框 ref → 填入「你好」→ 消息真的发出、页面出现回答 | ⚠ **整案重估**（见 §5.1） |
| **2** | **能读到回答文本** | 新增 `readAssistantReply` 返回非空且与页面一致 | 换站点或改用 CDP `Runtime.evaluate` |
| **3** | **打通本机 HTTP 端点** | `curl` 打 `http://127.0.0.1:<port>/v1/chat/completions` 拿到正确 OpenAI JSON | 纯工程问题，可控 |
| **4** | **AgentCore 接上** | 日志显示「回答来自网页版豆包」，AI 文字分析可用 | 检查三条构造路径改写 |

**第 1~2 步只动 `extension/edge/`，不碰 C++** ⇒ **可以立刻做，且零风险回退**。

---

## 8. 改动点清单（按优先级）

### P0（冒烟必需）

| 文件 | 改动 |
|---|---|
| `extension/edge/background.js` | 新增 `readAssistantReply` 消息类型 + 对应注入函数（**注意：不复用 `qstBuildPageSnapshot`，它的 `isVisible()` 只收视口内元素 ⇒ 回答滚出视口就取不到**，见 `MEMORY.md` §22 同类前科） |
| `extension/edge/manifest.json` | **不需要改**（`host_permissions` 已含 `<all_urls>` + `http://127.0.0.1/*` + `ws://127.0.0.1/*`）✅ |
| `extension/edge/web_ai_providers.json` | **新增**：豆包/DeepSeek/元宝的 selector 配置（**照抄 §2.3(1) 的四字段结构**） |

### P1（接上 AgentCore）

| 文件 | 改动 |
|---|---|
| `src/web_ai/web_ai_backend.{h,cpp}` | **新增**：本机 HTTP 端点 + 格式转换 |
| `src/web_ai/web_ai_driver.{h,cpp}` | **新增**：走 `ExtBridgeServer::Request()` 驱动 |
| `src/web_ai/web_ai_prompt.{h,cpp}` | **新增**：messages → prompt（纯函数，可单测） |
| `src/app_settings.h` | 加「网页 AI 档案」的标记字段（或在 `AiModelProfile` 复用 `apiUrl` 前缀判定） |
| `src/ai_action_runtime.cpp` / `ai_action_service.cpp` | 三条构造路径加 `ResolveEffectiveApiConfig()` |
| `CMakeLists.txt` | 新源加进 `QST_ENGINE_SOURCES`；⚠ **`agent_core.cpp` 在 `qst_engine`，已链 `window_mode_core`（含 `ExtBridgeServer`）⇒ 无需新增库依赖** ✅ |

### P2（体验与健壮）

| 项 | 说明 |
|---|---|
| 传图 | §5.2，需实测 |
| selector 热重载 | 照抄 `browser-ai-bridge` 的覆盖文件机制 |
| 设置页开关 | §6 |
| 单测 | `WebAiPromptSelfTest`：messages→prompt、回答→OpenAI JSON、`tool_calls` 反解析 |

---

## 9. 与既有 `MEMORY.md` 条款的交叉

| 条款 | 关系 |
|---|---|
| §14 产品不依赖别人软件 | ✅ §6 落实为「默认关闭 + 用户显式开启」 |
| §17 `PostMessage` 不向子窗转发 | ⚠ 网页 AI 是浏览器，**走扩展桥不走 PostMessage**，不受影响 |
| §20 枚举回执必须带覆盖度 | ⚠ `readAssistantReply` 若可能返回超长文本，**必须给 `truncated` 标记与 `total`** |
| §22 「在不在可视区」不是可用性判据 | ⚠⚠ **直接命中**：读回答**不能**用 `isVisible` 过滤，要主动滚动到回答区 |
| §27 取证断言期望值须从源码推出 | 开发期所有 selector 断言同上 |

---

## 10. 待办

- [ ] **P0-1** 实测豆包输入框能否用现有 `typeRef` 写入并发送（§5.1）—— **阻塞项**
- [ ] **P0-2** 新增 `readAssistantReply` + `web_ai_providers.json`
- [ ] P1 本机端点 + 三条构造路径改写
- [ ] P2 传图、热重载、设置开关、自检

---

## 11. 实测进展（2026-09-24，**已取证**）

### 11.1 已落地的代码

| 文件 | 状态 |
|---|---|
| `extension/edge/web_ai_providers.json` | ✅ 新增（豆包/DeepSeek/元宝，含 `busySelectors` 与三个 timeout） |
| `extension/edge/background.js` | ✅ 新增 `readAssistantReply` / `webAiProbe` / `webAiProviders` 三个消息类型 + 两个注入函数；`BRIDGE_VERSION` → `1.0.21` |
| `extension/edge/manifest.json` | ✅ `version` → `1.0.21`（与 `BRIDGE_VERSION` 同步） |
| `src/window_mode/ext_bridge/ext_bridge_server.cpp` | ✅ `/qst/status` 新增 `extConnected` / `extClients` 两个只读字段（见 §11.3） |
| `tools/verify/web_ai_backend_smoke.py` | ✅ 29 项纯逻辑冒烟 **ALL PASS** |
| `tools/verify/web_ai_live_probe.py` + 仓库根 `web_ai_probe.cmd` | ✅ 真机探针（手写最小 WS 客户端，零第三方依赖） |

### 11.2 ★★ 桥的 WS 口**不是给第三方探针用的**（重要，别再走弯路）

想「从外部连 WS 发 ping 判扩展在不在」是**错的路子**。取证：

| 行 | 事实 | 后果 |
|---|---|---|
| `:893-901` | 握手后**第一件事**是 `RecvWsText(hello, 5000)`，要求 `{"type":"hello","token":...}`；不合就 `return false` | **不发 hello ⇒ 5 秒被关**，症状 =「连上后静等 5s 断开」，**极像「扩展没连」** |
| `:906` | 任何过了 hello 的连接都被 `extSocks_.push_back()` | 探针连接会被**登记成一路「扩展」** |
| `:1018-1022` | 等回复用**全局单一** `waitingId_` | 桥可能把请求发给探针自己 ⇒ 真扩展**收不到**。**探针在污染被测系统** |
| `:929` | 只把 `result` / `ready` 当扩展回复；`hello` 无回执 | 从 WS **探不到**扩展在不在 |

**两次对照实验（实跑）**：不带 hello → 5.0s 被关；带 hello → 静等 10s 干净超时
⇒ 证明 hello+token 校验**通过**，但拿不到任何扩展信息。

### 11.3 因此新增 `extConnected` / `extClients`（唯一的可靠判据）

`ExtBridgeServer` 内部本就有 `IsExtensionConnected()` / `ExtensionClientCount()`
（`ext_bridge_server.h:53/58`），只是未对外暴露。现已在 `/qst/status` 补上：

```json
{"ok":true,"port":19228,"ws":"...","running":false,"currentScript":"",
 "extConnected":true,"extClients":1}
```

**实测验证**（17:04 重建 exe → 启动 → 探测）：

- exe 6779904 → **6780928** 字节；`"extClients":` **带引号的 JSON 字面量**在 exe 里找到
- **负对照**：两个必然不存在的串均未命中（证明取证方法有效）
- 运行时实测返回 `extConnected: true, extClients: 1` ✅

### 11.4 ⚠ 本机系统代理会让「软件在不在」的探测静默失真

- 软件没跑时，`urllib` 打 `127.0.0.1:<port>` 拿到的是 **`502 Bad Gateway`** ——
  **那是本机系统代理回的，不是软件回的**。
- **修法**：探测本机服务**必须绕代理** —— `build_opener(ProxyHandler({}))`。
  绕开后错误码从误导性的 `502` 变成如实的 `WinError 10061`（积极拒绝）。
- ⚠ 端口扫描找服务时**不能只认「HTTP 成功」**（代理对不存在端口也可能回 502，
  且响应**不稳定**，一会儿 502 一会儿超时）⇒ 要**校验响应体里的服务特有字段**（此处认 `ws`）。
- ⚠⚠ **一码多因**：`12029` / `502` / `10061` 在「端口关着」与「代理拦截」两种场景**都出现**。
  **看到错误码别立刻归因**，先用独立手段（裸 socket / netstat）确认端口状态。

### 11.5 扩展侧行为（实测）

- **扩展自动重连有效**：软件一启动，扩展在 30 秒内自己回来（`alarms` 每 30s 一轮），
  **不需要手动点图标**。此前「SW 在睡觉」的判断只对「刚重载扩展」那一刻成立。
- **扩展唯一正确加载路径是 `...\extension\edge`**；少一层（`...\extension`）会报
  「清单文件丢失或不可读」。
- ⚠ **「两份版本一致」这个自检会骗人**：它比的是「源码 vs build 副本」，
  而浏览器加载后有自己的缓存实例 ⇒ 版本一致**不等于**浏览器跑的是新版。
  要确证看 `edge://extensions/` 卡片显示的实际版本号。

### 11.6 ★★ 已定位并修复：「停过一次脚本 ⇒ 常开桥永久变聋」

> **这是本项目此前反复误判的原始现象**（`扩展没应答 ping / WS_RECV 连接被关闭`）的**真根因**。
> 结论：**不是扩展的问题，是桥自己的闩锁作用域错了。**

**症状（三段式，极易误判）**

| 观测 | 值 | 容易被误读成 |
|---|---|---|
| TCP 端口 | `LISTEN 127.0.0.1:19228` 在 | 「桥在，应该是扩展的问题」 |
| 进程健康度 | 11 线程 / 4.7% CPU / 44 MB | 「进程没死，不是崩溃」 |
| HTTP 请求 | **TCP 连得上，但请求被立即重置**（10054 / 10053） | 「软件没开」或「桥坏了」 |

⇒ 三档判据要分开：`连不上（拒绝/超时/代理502）` / **`连上但被重置`** / `连上有响应`。
**中间那档**才是本例，表示「服务在听，但受理路径被门闩挡住」。

**定位手法（可复用）**

1. **对称性反证**：同一端口上，**扩展的连接被记日志**（`配套扩展已连接本机桥`，记到 17:22:38），
   **我的连接不被记**（`扩展桥被探测 status×N` 停在 17:14:37）⇒ 二者走了不同分支
   ⇒ 直接锁定「读请求**之前**的那个提前返回」。
2. **找日志时间分水岭**：`17:23:16 本次运行解析后配置：enabled=0` 之后，`配套扩展已连接`
   **再不出现** ⇒ 那次「窗口模式关闭」的脚本运行就是触发点。
   **别从代码猜，从日志时间轴切。**

**两处缺陷（缺一不成，属「相乘」型）**

| | 位置 | 缺陷 |
|---|---|---|
| **A** | `EngineHost::StopRun()`（`engine_script_run.cpp`） | 调 `NotifyCancel()` ⇒ `AbortPending()` ⇒ `abort_ = true`。而清它的 `EndRun()` **只在窗口模式会话开着时**才跑 ⇒ 跑「窗口模式关闭」的脚本时，**置位了没人清**。 |
| **B** | `ExtBridgeServer::HandleClient()`（`ext_bridge_server.cpp:677` 原样） | `if (stop_.load() \|\| abort_.load()) return false;` —— 把「打断某次在途等待」的闩锁，当成了「拒绝新连接」的门闩。桥是**常开监听**（`Start()` 全进程只调一次）⇒ A 一置位就**永久**拒客。 |

**为什么极难定位**：B 的早退在**读请求之前** ⇒ 不读请求、不回响应、**连日志都不留**。
症状与「软件没开」「扩展没连上」「扩展版本旧」**完全同形**。

**修复（两道防线）**

1. `StopRun()`：在 `NotifyCancel()` **之后**（顺序不能反，反了就打断不了本次在途等待）
   立刻 `windowmode::ExtBridgeServer::Instance().ClearAbort();`
2. `HandleClient()`：判据去掉 `abort_`，**只留 `stop_`**（真关服务）。

**验收实验（必须真跑，不许只看代码）**：
启动软件 → 跑一次脚本 → 停止 → 再读 `/qst/status` ⇒ 应正常返回且扩展仍在在线。

---

## 12. P1 落地进展（2026-09-24 深夜，**已构建 / 已自检 / 待真机**）

### 12.1 ★★ 先纠正一条**方向性错误**：外部探针根本驱动不了扩展

`tools/verify/web_ai_live_probe.py` 原版想「从外部用 WebSocket 连上桥，再发
`ping` / `webAiProviders` / `webAiRichType` 去驱动浏览器」。**那条路在原理上不成立**，
不是选择器或超时的问题。取证（`src/window_mode/ext_bridge/ext_bridge_server.cpp`）：

| 事实 | 位置 | 后果 |
|---|---|---|
| 读循环**只**认 `type=="result"/"ready"` 且 id 命中 `waitingId_` 的**回执** | `HandleClient` 的读循环 | 外部发进来的业务请求**没有任何代码路径**转发给扩展 |
| 真正发给扩展的请求只由桥自己 `Request()` 发出；`Request()` 只发给**连在本进程监听器上**的扩展 | `RequestOnSock` / `Request` | **只有持有那条 WS 连接的进程**能驱动它 |
| 任何过了 `hello` 的连接都会被 `extSocks_.push_back()` 登记成"一路扩展"；非 attach 类型取 `socks.back()`（**最新那条**） | 同上 | 探针一旦连上就**顶替**真扩展收请求，而它自己不会应答 ⇒ **探针在污染被测系统** |

⇒ 所以原版 `[0.5]` 之后每一步都在白等 `--timeout 30s`（合计 5 分钟以上），
表现成"卡住"。**「ping 无回执属正常，继续往下跑」这句解释是错的**——
它掩盖了「后面每一步都不可能出结果」。

**纪律**：判据必须落在被测对象上。要验证"能不能驱动扩展"，就得在**能驱动它的那个进程里**跑。
本仓的实现是产品进程内的原生探针（下条），外部只能通过**产品自己开的口**问。

### 12.2 端点挂在哪里：**复用桥的监听器**，不新起服务

| 决策 | 理由（都是取证过的） |
|---|---|
| `POST /v1/chat/completions` + `POST /qst/web-ai/probe` **挂在 `ExtBridgeServer` 的 127.0.0.1 监听器上** | 扩展只能被持有那条 WS 的进程驱动 ⇒ 另起端口/另起进程都得让扩展重新连一次，等于把同一能力实现两遍；挂同一监听器 = **零新端口 / 零新 token / 零新生命周期**，且仍然「不动 `agent_core.cpp` 的 HTTP 层」 |
| token 复用桥 token（`Authorization: Bearer` / `X-Qst-Token` / `?token=` 三种都给） | `AgentCore::CallApi` 本来就发 `Authorization: Bearer <apiKey>`（`agent_core.cpp:970-972`）⇒ 档案里填桥 token 即可，不用新凭证 |
| 桥**不直接依赖** `src/web_ai/*`，改由 `ExtWebAiHandlers` 回调注入（与既有 `SetScriptApiHandlers` 同惯例） | `window_mode_core` 还被自检/播放器等多个目标链接；直接调会把 web_ai 的链接依赖扩散到每个目标。未注册时两个路由回 **501** 并说明原因（不静默 404） |
| 回环请求**显式免代理** | 本机系统代理会给 `127.0.0.1` 回 **502**（实测），而 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` 是否绕过回环取决于系统代理的勾选项 ⇒ `HostIsLoopback()` 命中时用 `WINHTTP_ACCESS_TYPE_NO_PROXY`（`agent_core.cpp` 两处 session） |

### 12.3 新增/改动清单

| 文件 | 内容 |
|---|---|
| `src/web_ai/web_ai_prompt.{h,cpp}` | **纯逻辑**：`messages`+`tools` → 这一轮写什么（`PlanTurn`）、工具调用协议反解析（`ParseReply`）、OpenAI 非流式/SSE 响应构造 |
| `src/web_ai/web_ai_config.{h,cpp}` + `web_ai_config_parse.cpp` | 开关文件 `AppDir()\web_ai_config.json`（**不存在 = 功能不存在**）、模型档案注入/清理、构造期改写 `ApplyProfileOverride`（端口与 token 每次启动都变） |
| `src/web_ai/web_ai_driver.{h,cpp}` | 经 `ExtBridgeServer::Request()` 驱动扩展：`listProviders/listPages/attach/webAiProbe/webAiRichType/webAiSubmit/readAssistantReply`。★ 含**防"读到旧回答"**的基线判据 |
| `src/web_ai/web_ai_backend.{h,cpp}` | 一次 chat completion 的编排（含会话状态机）+ 原生真机探针 `RunLiveProbeReport` |
| `src/window_mode/ext_bridge/ext_bridge_server.{h,cpp}` | 两个新路由 + `SendHttpWithType`（SSE 要 `text/event-stream`）+ `ExtWebAiHandlers` 回调 |
| `src/engine/engine_host_window.h` | 注册 web AI 处理器；4 处设置加载后注入档案（AI 三动作的硬闸看 `appSettings_.ai.enabled`） |
| `src/ai_action_service.cpp` ×2 / `src/ai_action_runtime.cpp` ×1 | 三条 `AgentCore` 构造路径统一 `ApplyProfileOverride(profile)` |
| `src/agent_ai_actions.cpp` | `LoadAgentAppSettings()` 注入档案；`ResolveAiModelName()` 加**防御纵深**兜底（只在用户**没有任何可用 API Key** 时接管） |
| `src/agent_core.cpp` | `HostIsLoopback()` ⇒ 回环显式免代理（两处 session） |
| `tools/web_ai_selftest.cpp` + CMake/`run_all_selftests.ps1` | `WebAiSelfTest`（24 例纯逻辑，进逻辑档） |
| `tools/verify/web_ai_live_probe.py` | **整体重写**：不再自己造通道，改为调产品内的 `/qst/web-ai/probe` 并把人话结论打出来；顺带修掉 GB2312 控制台下 `⚠` 直接崩的问题 |

### 12.4 自检证据（不是"跑绿了"）

`build\Release\WebAiSelfTest.exe --json` ⇒ **24 passed / 0 failed**（exit 0）。

★ 它是**红过一轮**的：首轮 21/3，其中
`parse_arguments_accepts_json_string` 抓到一个**真 bug** ——
模型给出 `{"name":"mouseClick","function":{"arguments":"{...}"}}` 时，
原实现「name 非空就整段跳过 `function`」⇒ 嵌套里的 `arguments` 被丢掉，
然后**被静默替换成 `{}`** ⇒ **用默认参数执行动作**（在错的位置点一下）。
已改为 name 与 arguments **各自独立**解析。另两条是我把期望写错了（追加消息≠重写历史；
重复提交同一请求应重发全量而不是报错——否则同一个动作跑第二次就废了），已在用例里写明理由。

`SSE` 那条用例逐字钉住消费端（`agent_core.cpp`）的硬要求：每行 `data:` 开头、
`function.name` 非空、`arguments` 是**字符串形式的合法 JSON**、`finish_reason` 是字符串、末尾 `[DONE]`。

### 12.5 还没做（如实列出）

| # | 项 | 说明 |
|---|---|---|
| 1 | **真机验收** | 豆包输入框能不能写进去 / 能不能读回回答 —— 必须由**你在重启软件后**跑 `web_ai_probe.cmd`（原生探针）。这是 §7 第 1~2 步，**没过就要重估整案** |
| 2 | 设置页勾选框 | 目前**靠 `web_ai_config.json` 显式开启**（与 `mcp_servers.json` 同惯例）。UI 勾选框的 8 处改动点已取证（见本文 §6 与 LESSONS 记录），待真机验收通过后再做 |
| 3 | **传图**（AI 图片分析的关键路径） | 请求里的 `image_url` data URL 需要落盘成临时文件 → 扩展用 CDP `DOM.setFileInputFiles` 挂到网页的 `<input type=file>`。**未实现**：目前图片只会在提示词里留 `[图片:image/png]` 占位 |
| 4 | 会话起点的干净度 | 复用用户当前网页会话（不新开对话）。若用户那个标签页里有无关长对话，会污染上下文。新开对话需要 provider 选择器 + 导航，留 P2 |
| 5 | 站点的真实选择器 | 豆包/DeepSeek/元宝的 selector 来自公开页面结构**推**出来的，**未真机验证**；探针 `[4]` 段就是干这个的 |

---

## 13. ★★★ 真机验收结果（2026-09-25 00:0x，**豆包全链路打通**）

### 13.1 结论：§7 的第 1~2 步（最关键的阻塞项）**过了**

`web_ai_probe.cmd`（实测输出，节选）：

```
[1] 已知站点（webAiProviders）
        浏览器里跑的扩展版本 = v1.0.23   磁盘上的 manifest = v1.0.23
        · 豆包 (doubao)  inputKind=richtext
[3] attach 到该站点
  [OK]  已 attach（1326ms）
[4] ★ 输入框长什么样（webAiProbe）
  [OK]  输入框命中：div[contenteditable='true']
        tag=div role='' 960x24 → 富文本(contenteditable)
[5] ★ 能不能把字写进输入框（webAiRichType，写完即清空）
        命中输入框：div[contenteditable='true']  可编辑=True  焦点在自己身上=True
        **回读长度=5**  内容='QST探测'
  [OK]  ★★ 写入成功且已回读确认 —— 富文本这一关过了！
[7] 真发一条消息
  [OK]  ★★★ 端到端打通！用时 6022ms，读回 43 字
        你好😊，有什么可以帮你的吗？
```

⇒ 三条**实测事实**（不是推断）：
1. **豆包输入框确实是富文本**（`div[contenteditable='true']`，960x24）—— §5.1 的预判成立；
2. **我们的 CDP 通道能写进去**，而且**是回读确认的**（`回读长度=5`，内容与写入一致）；
3. **提交（CDP Enter）+ 读回答**都能用：真发一条后 6.0 秒读回豆包的回答正文，
   说明 `responseSelectors` 与真实豆包 DOM 是对上的。

⚠ `发送按钮没命中`（`button[data-testid='chat_input_send_button']` 等都不匹配）：
**不阻塞** —— 提交走的是 CDP `Enter`（`webAiSubmit`），实测有效。选择器仍留在配置里，
将来若要"点按钮提交"再修。

### 13.2 这一轮真机暴露的 3 个 bug（都已修，都在"扩展边界"上）

| # | 症状 | 根因 | 判据/守卫 |
|---|---|---|---|
| 1 | `listPages` 报 `BAD_PAGES`（看着像"拿不到页面"） | 扩展返回的是**列式数组** `{count, tabIds[], titles[], urls[]}`，而驱动按**对象数组**解析 | 两种形状都认；驱动里写明理由 |
| 2 | `attach` 成功，紧接着 `webAiProbe`/`webAiRichType` 却回 `NO_TAB: 未 attach 任何标签页` | 4 个新处理器读了 `attachMeta.tabId`，而 `attachTab` 存的是 `meta.pageTabId`（`background.js:1148-1149`）—— **该字段根本不存在** ⇒ 恒为 0 | `web_ai_backend_smoke.py` 新增静态守卫：`attachMeta.tabId` 出现即 FAIL（既有 11 处都用 `pageTabId`） |
| 3 | `webAiRichType` 报 `NO_COMPOSER`（指向"选择器没命中"，而 `webAiProbe` 用同一份选择器刚命中过） | `executeScript({func: qstPrepareComposerForCdp})` 只注入该函数**自身源码**，而它内部调用两个兄弟函数 ⇒ 页面里 `ReferenceError`；⚠ MV3 此时是 **resolve 成 `{result:undefined, error:{...}}`**（不是 reject）⇒ 只看 `result` 就把"函数崩了"读成"元素没找到" | 改为只注入**自包含**函数（`injectPageFn` 统一入口，**并把 `error` 带出来**）；smoke 新增守卫：组合函数不得直接当 `func:` |

⚠ 三个 bug 的共同形状：**都在"我们以为传过去/拿回来"的边界上，且报错信息都指向了错误的方向**。
⇒ 纪律：跨进程/跨语言边界（C++ ↔ 桥 ↔ 扩展 ↔ 页面）每一处都要有**形状断言**，
且失败时必须把**对方的原始错误**带回来（不许用 `undefined` 顶替）。

### 13.3 ★ 一个必须记住的取证手法：扩展版本要**问扩展自己**

`[1]` 段那行 `浏览器里跑的扩展版本 = v1.0.23` 是**扩展在回执里自报的
`BRIDGE_VERSION`**，不是比两个文件的版本号。

为什么必须这样：浏览器加载扩展后有**自己的一份内存实例**，
「源码版本 == build 副本版本」**证明不了**浏览器跑的是新版 ——
本轮就出现了「文件已经是 1.0.22、浏览器还在跑 1.0.21」的情况（没点 ⟳）。
⇒ 判据要盯**被测对象本身**（扩展进程），不是它的文件。

### 13.4 ★★★ P1 也通了：端点本体 + **真实产品功能**端到端

**(a) 端点本体**（直接 POST `/v1/chat/completions`，两条都在 9 秒内）：

| 请求 | 实测结果 |
|---|---|
| 文字轮 `stream:false` | HTTP 200，`finish_reason=stop`，`content="1+1 等于 2。"` |
| 工具轮 `stream:true`（SSE） | HTTP 200，分片含 `function.name="mouseClick"`、`arguments="{\"button\":\"left\",\"x\":640,\"y\":360}"`（**字符串形式的合法 JSON**，正是 `agent_core` 的硬判据）、`finish_reason="tool_calls"`、`data: [DONE]` |

⇒ **豆包读懂了 `<<<QST_TOOL_CALLS>>>` 协议**并给出正确参数 —— 「AI 动作执行」的核心机制成立。

**(b) 真实产品功能**（`aiTextAnalysis` 动作，脚本里 `aiModelName = "doubao-web"`）：

```
[网页AI] provider=doubao 轮次=1 新对话=1 提示词=1462 字（工具裁剪=0）
[网页AI] 已 attach：豆包 - 字节跳动旗下 AI 智能助手（tabId=…）
[网页AI] 收到新回答：2 字，块 9→11，用时 5272ms
[网页AI] 成功：正文 2 字、工具调用 0 个、总耗时 8916ms
AI文字分析 [doubao-web]：完成 → aiResult = 15          ← 问的是 7+8
```

⇒ 完整链路：**产品 AI 动作 → AgentCore → 回环端点 → 桥 → 扩展 → 豆包 → 回答 → 输出变量**。
（`窗口模式`那两行 `enabled=0` 是同一脚本的运行配置，与 AI 无关。）

**(c) ★ 顺带证实了一条产品边界（重要，别误判成 bug）**：
先跑了一次 `aiModelName=""`（留空）的同一脚本，结果是
`AI文字分析 [deepseek-v4-flash]：完成 → aiResult = 5` ——
**它走了用户自己配置的 DeepSeek API**，而不是网页 AI。
这正是 §6 要的行为：**用户配了真 Key 就一律以用户配置为准**，
网页 AI 只是 `savedModels` 里多出来的一条可选档案。
要让某个动作走网页 AI，**显式选 `doubao-web` 这个模型**（或用户清掉自己的 Key）。

### 13.5 本轮补掉的两个"静默失败"缺口

| 缺口 | 处理 |
|---|---|
| 配置文件的 **UTF-8 BOM**：记事本「另存为 UTF-8」与 PowerShell `Set-Content -Encoding UTF8` **都会写 BOM**，而 nlohmann 遇 BOM 直接判失败 ⇒ 配置看着完全正确、功能却静默不开启（本轮实测就是这个情形） | `ParseUserConfigJson` 先剥 BOM；自检 `config_tolerates_utf8_bom` 钉住 |
| 请求里带图时本通道**传不了图** | `PlanTurn` 剥图并**在提示词里明说**「本通道无法回传图像，请改用文字/元素索引定位」+ `x_web_ai.note` 记账。⚠ 不硬失败的理由：`aiActionExecute` 默认每轮附截图，硬失败会让整条功能不可用；而它用文字标签定位时**本来就不需要图**。自检 `stripped_images_are_announced` |

### 13.6 已知未做 / 下一步（按优先级）

1. ~~**传图**~~ ⇒ **已完成，见 §14**。
2. ~~**设置页勾选框**~~ ⇒ **已改成更好的形态，见 §14.2**：不再需要 `web_ai_config.json`，
   档案**始终**出现在「设置 → AI助手」的模型下拉里，**选中即启用**（免 API Key）。
3. ⚠ **早退路由的 RST**：`/v1/chat/completions` 在 401/400 提前返回时**没读完请求体**
   就关连接 ⇒ 客户端可能看到 `ConnectionReset` 而读不到错误体（实测 `/qst/status` 被
   POST 时就是这个现象）。修法：早退前把 body 读掉再关。**未修**。
4. 会话起点干净度（新开对话而非复用用户当前长对话）、发送按钮选择器。

---

## 14. 传图 + 默认模型选项（2026-09-25）

### 14.1 传图通道（AI 图片分析的关键路径）

**链路**：请求里的 `data:image/...;base64,...` → 宿主解码 → 落临时文件 →
扩展用 CDP `DOM.setFileInputFiles` 挂到网页 `<input type=file>` → 页面回读确认。

**为什么必须走 CDP**（不是"实现选择"，是浏览器安全模型决定的）：
`input.files` 是**只读** FileList；用 `DataTransfer` 造 File 塞进去，页面大多不认
（那不是"用户选择"的结果，且部分框架会校验事件来源）。
CDP 走的是浏览器进程的**真实文件选择管线**，派发的事件与真人选文件完全一致。

**新文件**：

| 文件 | 职责 |
|---|---|
| `src/web_ai/web_ai_image.{h,cpp}` | `Base64Decode` / `ExtractImagesFromMessages`（**纯逻辑**）+ 临时文件落盘与清理（碰盘） |
| 扩展 `handleWebAiUploadImages` | 两条路：**A** 直接找 `input[type=file]`（含 open shadow root）；**B** 找不到就 `Page.setInterceptFileChooserDialog` 拦住文件选择器 + 点附件按钮，从 `Page.fileChooserOpened` 取 `backendNodeId` 再挂 |

**两条硬规则**（都有自检钉住）：

1. **话术必须按差值说**：`plan.imagesAttached`（真送到几张）vs `plan.imagesStripped`
   （请求里几张）。全部送到 ⇒ 说「已附上 N 张图」；一张没送到 ⇒ 才说「没能送过去」。
   **说反了就是静默错误**：模型会凭空猜坐标，或对着真存在的图说"我看不到"。
2. **回读校验是必须的**：`DOM.setFileInputFiles` 不报错 ≠ 页面认了
   （站点可能在 `change` 里校验类型/大小并静默清空 input）⇒ 必须读 `files.length`。

**降级**：上传失败**不**硬失败（`aiActionExecute` 每轮都附截图，硬失败会让整条功能不可用），
而是退回"剥图 + 在提示词里明说"的旧行为，并把失败原因写进 `meta.note`。

**上限**（保守值，网页端超限往往是**静默失败**）：最多 4 张 / 解码后合计 6 MB，**保新弃旧**。

### 14.2 模型下拉里的默认选项（"无需配置任何 API 就能用"）

**问题**：此前档案只在 `AppDir()\web_ai_config.json` 存在且 `enabled=true` 时才注入
⇒ 用户"在设置里根本看不到这个选项"。

**改法**（`ApplyProfileToSettings`，**纯函数**，在 `web_ai_config_parse.cpp`）：

1. **始终**把档案注入 `savedModels` ⇒ 下拉里**总能看到**它。
2. **追加在末尾**，不插队 —— `ResolveAiModelName` 在没有 `ai.modelName` 时会取
   `savedModels` 的**第一个**，插队会把用户自己的模型挤掉。
3. **选中才启用**：`ai.modelName == 档案名` ⇒ 打开 `ai.enabled` 总闸、顶层指向本机端点。
   **没选中就什么都不动**（不删档案、不动总闸、不碰用户的 Key/URL）。

`Enabled()` 因此有两条入口：**用户选中了该模型**（主路径，免 API Key）
**或** 配置文件开启（向后兼容 + 高级用户改参数）。

**为什么抽成纯函数**：`EnsureProfileInSettings` 要问桥拿端口/token（依赖 `ExtBridgeServer`），
而这三条硬规则必须能在**无桥、无桌面**的环境里逐格断言。
现在 `WebAiSelfTest` 直接覆盖 `ApplyProfileToSettings`（4 条用例，含"绝不自动选中"）。

### 14.2.1 ⚠⚠ 注入点漏了 —— "设置里看不到这个选项"的**第二条成因**

改完 §14.2 的逻辑之后，`grep` 才发现**光改判据还不够**：

- `JsonOpenSettings()`（前端取设置的那个函数）第一步是 `ReloadSettingsFromDisk()`；
- 而 `ReloadSettingsFromDisk()` 会 `g_ctx.settings = std::move(loaded)`
  —— **用磁盘那份整体覆盖**，把注入的档案**冲掉**；
- 本文件里有 **10+ 处** `LoadAppSettings(g_ctx.settings)` / `TryLoadAppSettings(g_ctx.settings)`
  ⇒ **逐条补注入必然会漏**（以后新增一条还会漏）。

**修法（两层）**：

1. **咽喉点兜底**：`JsonOpenSettings()` 拿到 `g_mu` 之后、序列化之前注入一次。
   它是"前端能看到的设置"的**唯一出口** ⇒ 不管上游怎么加载，前端一定看得到。
2. `ReloadSettingsFromDisk()` 里也注一次 —— 那是给**非序列化**的消费者
   （引擎侧直接读 `g_ctx.settings.ai` 的地方）用的。

⚠ 锁序：两处都在 webview 的 `g_mu` 内调 `EnsureProfileInSettings`，
它会拿 `web_ai_config` 的 `g_mu` 与桥的 `mu_`。
**保存路径（`ApplySaveSettingsJson`）早就是同一个锁序** ⇒ 不是新引入的耦合。
已确认桥的 `mu_` 全部是**短临界区**（`{ lock(mu_); ... }`），
**没有** `mu_ → g_mu` 的反向持有 ⇒ 无死锁。

### 14.3 选中即自动连接（页面没开就自动开）

用户不需要自己先去打开豆包页面。`SendAndRead` 的 attach 失败时：

1. `EnsurePageOpen()`：先 `ListPages` 按站点 `urlHint` 找 —— **已经开着就复用**
   （不重复开、不导航，用户可能正在那个页面上）。
2. 一个都没开 ⇒ 发 `webAiOpenPage`，扩展 `chrome.tabs.create({url, active:true})`。
   ⚠ 默认 `active:true`：这是"第一次用"的场景，用户往往还需要登录，开在后台会让他看不到、
   也不知道为什么失败。一旦页面存在就会走 ①，不会再有第二次抢焦点。
3. 睡 1.8 s（新建的标签页要时间才能 attach）后**重试一次** `EnsureAttached`。

⚠ **只重试一次**：失败就如实报错，绝不循环（会变成抢焦点 + 拖时间）。

### 14.4 新增自检（`WebAiSelfTest` 27 → 41 条）

传图 10 条：base64 向量（含 URL-safe / 折行 / 非法拒绝）、mime→扩展名、
抽取（保序 / 跳远程 / 跳坏数据并记账）、张数与体量上限（保新弃旧）、三种 part 形状、
无图时完全安静、以及 `PlanTurn` 的四条话术用例。

档案 4 条：始终注入且追加末尾（不抢 `savedModels[0]`、不改用户模型/Key/总闸）、
选中即开总闸并指向本机、重复注入幂等且刷新端口、**绝不自动选中**。

**反向验证（§30 铁律）**：把这 4 处断言对应的实现临时改坏（插队 / 去掉守卫 /
静默跳过非法 base64 / 去掉夹取），确认**红的正是预期那几条**，再还原。
两轮实测都只红了预期项，其余不动 ⇒ 断言指向精确，不是永真。


---

## 15. ★★★ 三个真机事故的根因与修复（2026-09-25 晚）

用户在真机上用出三个问题，前两个都是**事故级**的。逐条记根因，别再犯。

### 15.1 ★★★ 事故一：我们在**用户自己的对话**里发消息

**现象**：用户问「这个图片里都有什么」，回答里混进了用户私人对话的内容，
并且页面上的推荐内容被点开 —— 因为我们的请求**被追加进了用户正在聊的那个豆包对话**。

**根因**：`EnsureAttached` 挑标签页用的是「**第一个 URL 匹配的页面**」。
用户自己正开着的那个对话，URL 当然匹配 ⇒ **我们就直接用了它**。后果两条：
1. 模型能看到用户的私人历史（隐私 + 上下文污染）；
2. 我们的请求追加进用户主对话，用户看到的"回答"混着两边内容。

**修法**：**每个站点有且只有一个属于本软件的标签页**。

| 层 | 做了什么 |
|---|---|
| 扩展 | 新增 `webAiEnsureTab`：tabId 持久化在 `chrome.storage.local`（键 `qstWebAiOwnTab`）。复用前**同时校验 URL 仍属于该站点**（用户可能把那个标签页导航走了）。用户的标签页**一个都不碰**。 |
| 扩展 | 新增 `webAiNewChat`：判定「这一轮是新对话」时，把**我们自己的**标签页导航到站点的 `newChatUrl`，拿干净上下文。等 `status === "complete"` 再返回（不等的话后面的 attach/insertText 会打在半截页面上）。 |
| C++ | `EnsureAttached` → `EnsureOwnTab`；`SendAndRead` 增加 `newConversation` 入参（由 `TurnPlan::newConversation` 提供）。 |
| 配置 | `web_ai_providers.json` 新增 `newChatUrl`（留空则退回 `url`）。 |

⚠ **`EnsurePageOpen()` 已删除** —— 留两条"开页面"的路只会再踩坑。

⚠ 我们的对话仍会出现在豆包的侧边栏历史里（那是它的 UI，躲不掉），
但**与用户的对话是分开的**，互不污染。

### 15.2 ★★ 事故二：页面明明开着，却报 `NO_TAB:doubao`

**现象**：没开豆包页面时，软件自动开了页面，但立刻报
`NO_TAB:doubao` / 「页面已自动打开，但仍 attach 不上」；
用户还看到「扩展桥自动关闭调试了」。

**根因 A（页面找不到）**：`EnsureAttached` 用 `ListPages` 找页面，而 `listPages` 是给
**挑游戏窗**用的 ——
- 按 `gameUrlScore()` 打分（豆包 URL 得 **0** 分，游戏页得 18~40 分）；
- `bestPerWin`：**每个 windowId 只保留分最高的一个**；
- 最后 `slice(0, 8)`。

⇒ 豆包那个标签页被游戏页挤掉，**根本不出现在回执里**。`EnsurePageOpen` 步骤②也用它
⇒ 还会重复新建标签页。

**根因 B（调试会话被拆）**：`attachTab()` 开头就 `await detachDebugger()` ——
它会拆掉**全局**的 `attachedDebuggee`，也就是**正在跑的脚本**的调试会话。
用户看到的「自动关闭调试」就是这个。

**修法**：

1. **新增 `webAiFindPage`**：只做域名匹配，**不排序、不截断、不丢弃**（穷举 `chrome.tabs.query`）。
2. **网页 AI 用自己独立的调试会话**（`webAiDebuggee`）：`webAiEnsureTab` 单独
   `chrome.debugger.attach({tabId})`，**不碰**全局那条。
   `webAiRichType` / `webAiSubmit` / `webAiUploadImages` 的 CDP 目标改走 `webAiCdpTarget()`。
3. **网页 AI 不再走全局 `attach` 消息**（那正是拆会话的入口）。
4. 所有 `webAi*` handler 的 tabId 解析统一走 `webAiTargetTabId()`。

⚠ 判据：`listPages` 的契约是「**挑游戏窗**」，**不能**用来判「某个站点的页面在不在」。

### 15.3 事故三：只有豆包一个网页模型 + 地址/密钥看不懂

**现象**：模型下拉里只有 `doubao-web`；用户问「元宝/千问/deepseek 呢」，
以及「API 地址是 `127.0.0.1:19228`、密钥是一串 hex，机制不明，换台电脑还能生效么？」

**修法 A（多站点）**：`ApplyProfileToSettings` 改成**按站点列表循环注入**
（`BuiltinWebAiProviderIds()` = doubao / deepseek / yuanbao），每个站点一条 `<id>-web` 档案。
`ProviderFromModelName` 加 qwen 分支；`IsWebAiModelName` 认任一内置名。

⚠⚠ **加站点必须同时改三处**，漏一处症状不同：
| 漏哪 | 症状 |
|---|---|
| `BuiltinWebAiProviderIds()` | 设置页里**看不到**那个模型（用户以为没做） |
| `ProviderFromModelName()` | 选了它被判成「认不出站点」 |
| `web_ai_providers.json` | 请求报 `NO_PROVIDER` |

⇒ 已加自检 `builtin_provider_names_round_trip` 钉住前两处的一致性。

⚠ **千问（qwen）故意没加**：核实过 `www.qianwen.com` 与 `tongyi.aliyun.com/qianwen/`
都是阿里官方落地页，但**对话页 URL 与输入框 selector 没核实**；
而搜索「通义千问 网页版」前排混着 `cn.qianwen-ail.com.cn` 这类**仿冒域名**
⇒ **绝不猜**（猜错就是把用户导到钓鱼站）。**需要用户确认入口 URL 后再加。**

**修法 B（地址/密钥机制）**：机制本身是**对的**，缺的是**没告诉用户**：
- 端口：每次软件启动时**顺序占位**（从 19228 起找空闲端口）
- 密钥：每次启动**随机生成**（32 位 hex）
- ⇒ 换电脑/重启软件后两个值都会变，但
  `EnsureProfileInSettings`（每次加载设置）与 `ApplyProfileOverride`（每次构造 AI 请求）
  会**自动刷新** ⇒ **用户不用管，也不会失效**。

UI 上加了 `#aiWebHint`：选中网页版模型时显示一段说明（不需要填 Key、值是自动生成的、
换环境照样能用、前提是扩展已连 + 站点已登录）。

---

## 16. 统一 `web` 模型 + 站点自动挑选 + 后台标签页保活（2026-09-25 深夜）

用户第二轮反馈（7 项）。本节记 **1 / 2 / 3 / 5** 的落地；**4 / 6 / 7** 是工程，另见
[`docs/ai-proxy-roadmap.md`](ai-proxy-roadmap.md)。

### 16.1 模型下拉统一成 `web`（用户原话：站点多了"眼花缭乱"）

**旧形态**：每个站点一条档案（`doubao-web` / `deepseek-web` / …），用户得先决定用哪个。
**新形态**：**只留一个 `web`**，站点由运行时自动挑。

| 规则（用户指定） | 实现 |
|---|---|
| 已打开页面的站点优先 | 扩展 `webAiPickProvider`：遍历内置站点 → `findProviderTabs` → 谁有页面用谁 |
| 都没开 ⇒ 用**已登录**的 | 逐个打开探测，判据 = **页面上能找到可见输入框**（≈ 已登录，且这正是后续要用的东西，不用去猜各家 cookie 名） |
| 探测失败的标签页 | **由我们关掉**（不给用户留一堆登录页） |
| 全都不行 | 报 `NO_WEB_AI_LOGIN` ⇒ 界面提示"请至少登录一个网页 AI" |

⚠ **迁移**：`MigrateAndApplyProfiles`（**纯函数**）会清掉旧的 `<站点>-web` 档案
（判据必须**同时**满足「旧名」+「apiUrl 是本机回环」），并把选中过旧档案的用户改选 `web`；
**用户自己起名/自己填 URL 的档案一律不动**（判严不判松）。

⚠⚠ **为什么把迁移逻辑抽成纯函数**：第一版留在 `EnsureProfileInSettings`（要问桥，自检调不到），
于是用例只能自己传 `kWebAiModelName` 去调 `ApplyProfileToSettings` ——
**"到底注入了哪个名字"根本没人测**。实测：把实现改回 `doubao-web`，测试**照样全绿**（永真断言）。
抽出来之后同一变异立刻报 `web=0`。

### 16.2 后台标签页保活（问题 2 的根因）

**用户现象**：deepseek 调用"成功"了，但**必须手动把网页切到前台**，软件才能抓到回复。

**根因**：后台标签页 `document.visibilityState === 'hidden'` ⇒ Chrome 冻结/节流渲染
⇒ 站点的**流式回答不往 DOM 写** ⇒ 我们等半天读不到。扩展原来的
`Emulation.setFocusEmulationEnabled` + `Page.setWebLifecycleState("active")`
**只能缓解，改不了 `visibilityState`**。

**修法（三层）**：

1. **保活**：`webAiEnsureTab` attach 后调 `keepTargetAlive()`（focus 模拟 + lifecycle active）。
2. **可观测**：`webAiEnsureTab` 回执里带 `pageState`（`visibility` / `hidden` / `hasFocus`），
   宿主写进日志 ⇒ 这类问题**下次一眼能看出来**。
3. **临时前台化**（默认开）：`SendAndRead` 开头 `webAiActivateTab`（记住原标签页），
   结束用 **RAII 守卫**还回去 —— 保证**任何** return 路径（含中途报错）都会还。
   ⚠ 还的时候只在我们那个仍是活动标签页时才还（用户中途自己切走了就别抢回来）。

### 16.3 另一个根因：新建标签页没等加载

**现象**（探针实测）：`[4] 输入框命中：div[contenteditable='true']` 但
`[5] 写不进：NO_COMPOSER 没找到可见的输入框` —— **同一页面上两条判据打架**。

**根因**：`chrome.tabs.create` 返回时页面**还在加载**：DOM 有了但**布局没算**，
`getBoundingClientRect()` 返回 `0x24` ⇒ `qstFocusComposer` 的可见性判据
（`width<=0 || height<=0` 跳过）判它"不可见"。

**修法**：抽出 `waitTabComplete(tabId, ms)`，`webAiEnsureTab`（新建时）与 `webAiNewChat`
**都必须等 `status === "complete"`** 再往下走。

### 16.4 千问（用户提供入口）

`https://www.qianwen.com/` —— 已核实与 `tongyi.aliyun.com/qianwen/` 返回**同一个阿里官方落地页**。
已加进 `web_ai_providers.json`（`urlHint: qianwen.com`）。
⚠ **对话页路径与输入框 selector 仍是通用候选，未真机标定**
（搜索"通义千问 网页版"前排混着 `cn.qianwen-ail.com.cn` 这类**仿冒域名**，所以不猜）。
跑法：`web_ai_probe.cmd --provider qwen`。

### 16.5 自检

`WebAiSelfTest` **47 条**。本轮新增/改写：
`profile_for_every_builtin_provider`（走真实入口 `MigrateAndApplyProfiles`）、
`legacy_profiles_are_migrated`（旧档案迁移 + 用户自建档案不动）、
`legacy_provider_names_recognized`（旧名仍认得，真模型名不误判）。
**反向验证**：把注入名改回 `doubao-web` ⇒ 立刻红 2 条（`web=0`）。

---

## 17. ★★★ 闪退的**真根因**：扩展 JS 的**同名函数覆盖**（2026-09-25 深夜）

### 17.1 症状与取证

用户：无 web 页面、但已登录，提问**不回答**且软件**闪退**。
`web_ai_log.cmd` 给出的日志（**这就是 §50/§51 那两条加固的价值 —— 把静默崩溃变成了可读日志**）：

```
[23:09:01.894] 桥请求 ← webAiNewChat（回执 124 字节）
[23:09:01.895] ★★ 处理器抛出异常（已兜住，未崩溃）：[json.exception.type_error.302] type must be string, but is boolean
```

### 17.2 真根因

`extension/edge/background.js` 里 **`waitTabComplete` 被定义了两次**：

| 位置 | 返回 |
|---|---|
| 我新加的（靠前） | **字符串**（`"complete"` / `""`） |
| **原有的**（靠后，1460 行附近） | **`true` / `false`** |

⚠⚠ **JS 的函数声明会提升，后定义的赢** ⇒ **我新加的那个从来没生效过**。
调用点拿到 `true` ⇒ 回执变成 `{"status":true}` ⇒
宿主 `r.value("status", "")` 期望 string ⇒ **抛 `type_error.302`** ⇒
异常逃出桥的 HTTP 线程 ⇒ `std::terminate` ⇒ **整个软件瞬间消失**。

⚠ **`node --check` 抓不到这个** —— 重复声明在 JS 里是**合法的**（静默覆盖）。
这类 bug 的特征：**你的新代码一次都没跑过，但语法检查全绿。**

### 17.3 修法（三层）

1. **删掉重名函数**，调用点按原有语义改（`const ready = await waitTabComplete(...)`，
   回执里 `status: ready ? "complete" : "timeout"`）。
2. **回执字段显式定型**：`webAiEnsureTab` / `webAiNewChat` 的每个字段都用
   `String(...)` / `Number(...)` / `!!` 包一层。
   ⚠ **回执是跨进程协议，类型必须确定** —— 别把 JS 的宽松类型直接塞过去。
3. **宿主侧类型安全读**（见 §17.4）。

### 17.4 ★★ 宿主侧加固：`JsonStr` / `JsonBool` / `JsonInt`

`nlohmann::json::value(key, default)` 在**类型不符**时**抛异常**。
而 web_ai 模块的读法大量出现在**桥的 HTTP 线程**里 ⇒ 一抛就是整个进程。
⇒ 新增三个纯函数（`web_ai_prompt.{h,cpp}`，**可自检**）：类型不符返回默认值 +
把**实际类型**写进 `mismatchOut`（调用方记账）。

**web_ai 模块 48 处 `.value(` 全部替换**，并加了两条自检：
- `json_safe_read_never_throws`：8 个键 × 3 种读法 + 非对象输入，**全程不许抛**；
- `json_safe_read_semantics`：类型不符给默认值并记账；**缺字段不记账**（否则日志全是噪音）。

**`RequestJson` 里再加一层**：字段类型异常时**把回执原文（截断 400 字）写进日志** ——
否则下次还是要靠猜。

### 17.5 静态守卫：`tools/verify/ext_no_dup_functions.py`

扫 `extension/edge/*.js` 的**顶层同名声明**（`function` / `const f = ...`），
出现 >1 次即报错并打印行号。

⚠ 立刻又抓到一处：`sleepMs` 也有两份（内容完全相同，无害但会让守卫永远报红）
⇒ 已删掉后一份，**守卫现在干净**（永远报红的守卫没人看）。

**跑法**：`python tools/verify/ext_no_dup_functions.py`（改扩展 JS 后必跑）。

---

## 18. ★★★ 传图判据修正：`files.length` 不是「文件挂上了」的判据（2026-09-26）

### 18.1 现象

```
上传失败（UPLOAD_NOT_ACCEPTED: CDP 已把文件挂上 input，但页面把它清掉了（files.length=0））
```
⇒ 提示词里说「1 张图没能上传」⇒ 模型答「无法接收解析这张图片」。

### 18.2 根因：**判据错了，不是实现错了**

站点的附件流程是「读走 `files[0]` → 上传/本地预览 → **主动清空 input**」
（清空是为了让用户能再选同一个文件）⇒ **`files.length === 0` 完全可能是成功的**。
我拿它当失败 ⇒ **把成功判成失败**。

**与类名无关的正确信号**：页面里出现了新的 **`blob:` / `data:image` 图片元素（缩略图）**。
⇒ `qstCountThumbs()` 数它，挂载前后对比。

### 18.3 修法（4 处）

1. **逐个候选 input 尝试**（`qstPickFileInput(cfg, idx)`）——
   按「`accept` 含 image 优先 → 不限类型 → 可见优先 → **离输入框近**优先」排序。
   ⚠ 原来"只试第一个 + 只在最后回读一次"⇒ 选错 input 就直接失败，
   而且**分不清"选错了"和"读走就清空"**。
2. **成功判据**（任一成立）：① input 里还留着文件；② **缩略图变多**。
3. **路 A 全失败 ⇒ 自动降级走路 B**（拦文件选择器 + 点附件按钮）。
4. **诊断随回执回来 + 落日志 + 探针显示**：
   `listBefore/After`（页面上**所有** file input 的 `accept/multiple/visible/尺寸/fileCount`）、
   `tried`（逐个尝试的回读与缩略图）、`thumbsBefore/After`。

### 18.4 仍未确认

**豆包到底是"读走就清空"（成功）还是"选错 input"（失败）** —— 这次改法**两种都能处理**，
但确凿答案要等 `web_ai_probe.cmd --upload` 的输出（看「逐个尝试」里哪个 input 的缩略图变多了）。
**诊断已经埋在回执里，不用再猜。**
