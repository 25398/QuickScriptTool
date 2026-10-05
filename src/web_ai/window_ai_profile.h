#pragma once
// ──────────────────────────────────────────────────────────────────
// window_ai_profile.h — 窗口反代（B 类·界面反代）的**纯逻辑层**
//
// 「窗口反代」= 把一个 GUI 客户端窗口（豆包客户端 / Cursor / …）当成 AI 后端：
//   找到窗口 → 定位输入框 → 打字 → 提交 → 等回答稳定 → 读回文本。
//   与网页端（`web_ai_*`）**共用**提示词规划/工具协议/OpenAI 响应那一层，
//   差别只在「怎么把字送进去、怎么把回答读出来」：
//     网页端 = CSS selector + CDP（扩展）
//     窗口端 = UIA 控件属性 + 键鼠（本进程）
//
// ⚠⚠ 为什么这一层必须与 UIA 分离成纯函数：
//   判据错一次 = 把话打进**别的程序**的输入框、或把**上一次的回答**当成新回答
//   （与网页端 §5.1 同类，但更难发现：窗口那边没有"回读长度"这种现成证据）。
//   所以：打分/挑选/提取全部做成吃「节点快照」的纯函数，由 `WindowAiSelfTest` 逐格钉住。
//
// ⚠ 命名歧义（见 docs/ai-proxy-roadmap.md §3.1）：
//   这里做的是 **B 类·界面反代**（驱动 GUI），**不是** A 类·流量反代
//   （拦 CLI 的 HTTP 请求、绕过额度）—— 后者违反服务条款，产品明确不做。
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace quickscript::webai {

/// UIA 控件类型 id ↔ 稳定英文名（配置里写英文名，不写数字、不写中文标签）
///
/// ⚠ 与 `windowmode::UiControlTypeTable`（`ui_element_probe.h`）是**两份表**，
///   但 id 必须一致 —— 自检里遍历那张表断言「每个在册 id 都有英文名」，
///   否则新加的类型会在这边悄悄变成"认不出来"（配置写 `Switch` 却永远匹配不上）。
const wchar_t* WindowAiControlTypeName(int controlTypeId);

/// 英文名 → 类型 id（大小写不敏感）；不认识返回 0
int WindowAiControlTypeIdFromName(const std::wstring& name);

/// 一个 UIA 节点的**纯数据快照**（驱动层抓取，本层只做判断）
struct WindowAiUiNode {
    int controlTypeId = 0;
    std::wstring controlType;    ///< 英文名（如 Edit/Document/Button）
    std::wstring name;           ///< UIA Name
    std::wstring automationId;   ///< UIA AutomationId
    RECT rect{};                 ///< 屏幕坐标
    bool focusable = false;
    bool valuePattern = false;
    bool textPattern = false;
    bool enabled = true;
    bool offscreen = false;
    bool keyboardFocus = false;  ///< 当前是否**已经**有键盘焦点（判断"要不要先点一下"）
    /// UIA ValuePattern 当前值（可能为空；⚠ 密码框的值**绝不回传**）
    std::wstring value;

    int Width() const { return rect.right - rect.left; }
    int Height() const { return rect.bottom - rect.top; }
    long long Area() const {
        const long long w = Width() > 0 ? Width() : 0;
        const long long h = Height() > 0 ? Height() : 0;
        return w * h;
    }
};

/// 输入框判据（配置里写这些；与网页端 `inputSelectors` 同地位）
struct WindowAiInputSpec {
    std::vector<std::wstring> controlTypes{L"Edit", L"Document"};  ///< 英文名
    std::vector<std::wstring> nameContains;        ///< 任一命中即可（空 = 不限）
    std::vector<std::wstring> automationIdContains;///< 任一命中即可（空 = 不限）
    int minWidth = 120;
    int minHeight = 18;
    /// 是否要求「可聚焦」（能打字的下限）
    bool requireFocusable = true;
    /// ★★**防"打进编辑器"的安全闸**：为 true 时，`nameContains` / `automationIdContains`
    ///   必须至少命中一个，否则整条判据**一个候选都不给**（宁可不工作，也不乱打）。
    ///
    /// 为什么需要它：像 Cursor / VS Code 这类"编辑器 + 聊天面板"共存的客户端里，
    /// **主编辑器也是 Document**，而且面积最大、也在窗口下半部 ——
    /// 光靠类型+面积打分，很可能把提示词打进**代码文件**里（真·破坏性）。
    /// ⇒ 这类客户端的默认档案把本项置 true 且**留空两张名单**（= 显式"未校准"状态）：
    ///   探针 dump 出真实 AutomationId 后再填进去，功能才启用。
    ///   这是「宁可 NO_INPUT，也不打错地方」的落地方式。
    bool requireHints = false;
    /// ★★**几何兜底**（UIA 拿不到控件树时的唯一出路）。
    ///
    /// ⚠⚠ 为什么必须有：**Electron / Chromium 客户端默认不向 UIA 暴露控件树**
    ///   （2026-09-26 在 Cursor 上实测：`FindAll(Descendants)` 返回 **0 个节点**，
    ///   等 6 秒仍是 0）—— 不是我们读法不对，是它压根没开无障碍。
    ///   这类客户端只有两条路：① 让用户在客户端里打开无障碍；② 走这条几何+OCR 路径。
    ///
    /// 取值：相对**客户区**的 4 个数（l,t,r,b，0..1），如 `[0.55, 0.88, 0.98, 0.97]`。
    /// 非空 ⇒ UIA 一个候选都没有时，改走「点它中心 → 粘贴 → 回车」，
    /// 回执里如实标 `mode=geometry-ocr`（**不假装是 UIA 定位的**）。
    std::vector<double> rectHint;
    /// ★**占位符提示词**（空输入框里会显示的那句话的开头）。
    ///
    /// ⚠ 为什么必需：几何+OCR 模式下，"输入框里有没有用户草稿"只能靠读那块矩形判断，
    ///   而**空输入框也会显示占位符**（豆包是「发消息或按住空格说话．．」）
    ///   ⇒ 没有这条判据就会把"空框"误判成"有草稿"从而**永远拒绝工作**（实测踩到）。
    std::vector<std::wstring> placeholderHints{
        L"发消息", L"输入消息", L"说点什么", L"有问题", L"问我",
        L"Ask", L"Send a message", L"Type a message", L"Message",
    };
    /// ★**永不 Ctrl+A 清空**（见驱动层注释）：输入框里已有草稿时**中止**而不是覆盖。
    ///   这一项存在即是为了"不破坏用户已有的输入"，不要改成自动清空。
    bool abortIfInputNotEmpty = true;
    /// ★选哪个（同一份判据常命中多个）：
    ///   `largest`      面积最大（聊天客户端输入框通常是内容区最宽的输入控件）
    ///   `lowest`       底部最靠下（输入框一般在窗口下方）
    ///   `largest-lowest` 先按「在客户区下半部」筛，再取面积最大（默认）
    std::wstring pick = L"largest-lowest";
};

/// 回复读取判据
struct WindowAiReplySpec {
    /// 容器候选（英文类型名）；空 = 用整窗文本
    std::vector<std::wstring> containerTypes{L"Document", L"List", L"Pane"};
    int maxChars = 12000;
    /// 用 UIA TextPattern 读整段（拿不到时退回"拼接各节点 Name"）
    bool preferTextPattern = true;
    /// ★ UIA 读不到任何文本时，用**系统自带 OCR**（`RunWinRtOcr`，零安装）读一块区域。
    ///   这是 Electron/Chromium 客户端（Cursor 等）**唯一**的读回答办法。
    bool ocrFallback = true;
    /// OCR 区域（相对客户区 0..1 的 l,t,r,b）；空 = 整个客户区。
    /// ⚠ 整窗 OCR 会把侧边栏/标题栏的字也读进来（噪声），能收敛就收敛。
    std::vector<double> ocrRegion;

    // ─────────────────────────────────────────────────────────────────
    // ★★ 剪贴板通道（**首选**读法）：点回答下方的「复制」按钮 → 读剪贴板
    //
    // 为什么它优先于 OCR/UIA（用户 2026-09-27 指出，方向正确）：
    //   · **准**：剪贴板里是客户端自己给的原文（Markdown/纯文本），不是识别结果 ——
    //     实测 OCR 把消息操作图标读成 `0`、把 `11+12` 读成 `1 1 + 1 2`；
    //   · **快**：一次点击 + 一次剪贴板读取（毫秒级）；OCR 每轮都要截图 + 识别（数百毫秒起），
    //     而等回答期间是**反复轮询**的，这个成本要乘轮数；
    //   · **全**：不受可视区域限制（长回答不用滚动、不会被截断）。
    // ⇒ 顺序固定为 **剪贴板 → UIA 文本 → OCR**，后两者只是兜底。
    // ─────────────────────────────────────────────────────────────────
    /// 启用剪贴板通道（关掉就退回 UIA/OCR）
    bool useClipboard = true;
    /// 「复制」按钮的**可访问名**（UIA name；大小写不敏感、子串匹配）
    std::vector<std::wstring> copyButtonNames{
        L"复制", L"复制回答", L"复制内容", L"Copy", L"Copy response", L"Copy message",
    };
    /// 复制按钮的 automationId 子串（有些客户端只给 id 不给名字）
    std::vector<std::wstring> copyButtonIds;
    /// 复制按钮的几何兜底（相对客户区 0..1 的 l,t,r,b）：UIA 里找不到「复制」时点这里。
    /// ⚠ 回答位置随长度变化 ⇒ 这条通常留空；优先用下面那条"相对最新回答底部"的推算。
    std::vector<double> copyButtonHint;
    /// 点完复制按钮后等剪贴板变化的上限（毫秒）
    int clipboardWaitMs = 2500;
    /// ★读完**把用户原来的剪贴板还回去**（默认是）。借了别人的东西要还。
    bool restoreClipboard = true;
    /// ★★**几何锚点兜底**：UIA 里找不到「复制」按钮时（Chromium 客户端实测就是这样 ——
    ///   308 个节点里一个「复制」都没有），用"**回答最后一行文本**"的矩形推它的位置。
    ///
    /// 依据（实测树结构）：消息正文是带矩形的 `Text` 节点，而消息下方的操作栏
    /// （复制/点赞/踩/分享/重新生成）**不在树里**，只能按位置推：
    ///   锚点 = 会话列里**最靠下的、可见的**文本节点（聊天是自上而下追加的 ⇒ 最后一条最新）；
    ///   复制按钮 ≈ 锚点左下角 + 偏移（`copyButtonOffset`，相对客户区宽高的比例）。
    /// ⚠ 偏移量是**每客户端一配**的值（默认值按豆包客户端的实测布局给），
    ///   探针的 `copyProbe` 会把它算出来的落点回报出来，可据此校准。
    bool copyAnchorFallback = true;
    /// `[dx, dy]`：相对客户区宽/高的比例。`dy` 作用在锚点**底边**上；`dx` 只是微调
    /// （横向基准是 `copyButtonXRel`，**不是**锚点文本的左缘 —— 见那里的说明）。
    std::vector<double> copyButtonOffset{ 0.0, 0.034 };
    /// ★★复制图标的**横向基准**（相对客户区 0..1）。
    ///
    /// ⚠⚠ 为什么不能用锚点文本的左缘（实测踩到）：消息操作栏是**贴会话列左缘**排的，
    ///   而消息正文有自己的缩进/居中 ⇒ 按文本左缘点会偏出几百像素
    ///   （实测锚点左缘在 x_rel 0.63、图标实际在 0.277 ⇒ 必然点空）。
    ///   实测（OCR 行框）：图标行在 x_rel≈0.277 与 0.335（复制/点赞），消息正文在 0.313。
    double copyButtonXRel = 0.28;
    /// 会话列的相对横向范围（默认 0.24~0.86：排除左侧会话列表与右侧摘要栏）
    /// 操作栏按钮的尺寸区间 / 同排最少几颗（见 PickWindowAiCopyRowButton 的判据）
    int copyRowMinSize = 20;
    int copyRowMaxSize = 56;
    int copyRowMinCount = 3;
    double copyColumnLeftRel = 0.24;
    double copyColumnRightRel = 0.86;
};

/// 一个客户端的适配档案（形态对标 `web_ai_providers.json` 的一个 provider）
struct WindowAiClientProfile {
    std::string id;                    ///< 模型名里写的 id（如 doubao-app / cursor）
    std::string label;                 ///< 人看的名字
    std::vector<std::wstring> aliases; ///< 模型名别名（如 豆包客户端 / cursor-app）
    std::vector<std::wstring> processNames;  ///< 进程 exe 名（不含路径，如 Doubao.exe）
    std::vector<std::wstring> titleContains; ///< 标题子串（空 = 不限）
    /// 标题里出现这些就**排除**（如 Cursor 的「Welcome」空窗口）
    std::vector<std::wstring> titleExcludes;
    /// ★窗口尺寸下限：小于这个尺寸的"匹配窗口"**不算数**。
    ///
    /// ⚠⚠ 为什么必须有（实测踩到）：客户端常有多个同进程的窗口 ——
    ///   Cursor 就有个 `158x26` 的离屏小窗（挂在 `-21333,-21333`），标题同样是
    ///   "Cursor Agents"，而**真正的主窗是 1920x997**。
    ///   按"首个匹配"选 ⇒ 探针会对着那个 26 像素高的窗口报告"成功"，
    ///   之后所有打字都进了虚空（而且回执看着一切正常）。
    ///   ⇒ 先按尺寸把不可能当聊天窗的排除掉，再在剩下的里挑**面积最大**的。
    int minWindowWidth = 320;
    int minWindowHeight = 220;
    WindowAiInputSpec input;
    WindowAiReplySpec reply;
    /// 提交方式：`enter`（直接回车）| `button`（按名字点发送按钮）
    std::wstring submitMethod = L"enter";
    std::vector<std::wstring> sendButtonNames;
    /// ★★**输入方式**（默认 `background`：**不抢前台**）。
    ///
    /// ⚠⚠ 为什么默认后台（用户 2026-09-27 明确要求）：窗口反代原先走"抢前台 + SendInput"，
    ///   用户原话「问个问题电脑都用不了了」—— 这个代价**不可接受**。
    ///   仓库里本来就有后台输入能力（窗口模式那套）：`windowmode::PostQuickInputToWindow`
    ///   + `PostKeyToWindow`，注释写明"假焦点灌键（Chromium 壳/Qt/微信）"，
    ///   正是给这类客户端准备的。
    ///   · `background` = 后台灌键（**不激活窗口、不动鼠标**）；
    ///   · `foreground` = 老行为（抢前台 + SendInput），只在后台灌键确实不行时由用户开启。
    std::wstring inputMethod = L"background";
    /// 后台输入失败时**是否允许回退到抢前台**。默认 false —— 回退就等于"偷偷抢前台"，
    /// 正是用户不接受的那件事；失败要**如实报**，让用户自己决定（改成 foreground）。
    bool allowForegroundFallback = false;
    /// ★「新对话」按钮的可访问名：开新一轮会话时 **Invoke** 它（不需要前台）。
    /// ⚠ 这是"新对话"语义的落地 —— 没有它，多轮请求会全部打进**同一个历史对话**，
    ///   甚至把别的会话的回答读回来（用户实测报障：新开对话没生效、回答串台）。
    std::vector<std::wstring> newConversationButtonNames{
        L"新对话", L"新建对话", L"新会话", L"New chat", L"New Chat",
    };
    int idleTimeoutMs = 8000;    ///< 内容稳定多久算生成结束
    int maxTotalMs = 180000;     ///< 一轮上限
};

/// 内置默认档案（用户在 `AppDir()\window_ai_providers.json` 里可覆盖/新增）
std::vector<WindowAiClientProfile> BuiltinWindowAiClients();

/// 解析用户覆盖文件并**合并**到默认档案上（按 id 覆盖；新增 id 直接追加）。
/// ⚠ 坏 JSON **不抛异常**：返回 base 并把原因写进 err（功能退化为"用默认档案"，
///   而不是整个窗口反代不可用 —— 这与 `web_ai_config.json` 的取舍一致）。
std::vector<WindowAiClientProfile> MergeWindowAiProfiles(const std::string& jsonUtf8,
    const std::vector<WindowAiClientProfile>& base, std::string& err);

/// 模型名/客户端 id → 档案下标（-1 = 没有匹配）。
/// 匹配顺序：id 精确 > 别名精确 > id/别名出现在模型名里（子串）。
int FindWindowAiClient(const std::vector<WindowAiClientProfile>& clients,
    const std::string& modelOrIdUtf8);

/// 窗口是否属于该客户端（进程名不分大小写精确匹配；标题按子串；排除项优先）
bool WindowAiWindowMatches(const WindowAiClientProfile& profile,
    const std::wstring& processName, const std::wstring& title);

/// ★给一个 UIA 节点打分：< 0 = 不合格；分数越大越像"这个客户端的输入框"。
/// @param clientArea 目标窗口的**客户区**矩形（屏幕坐标），用于「在不在下半部」判断。
///        ⚠ 必须传窗口自己的客户区，不能用整窗矩形：标题栏会让"下半部"判断偏移。
int ScoreWindowAiInput(const WindowAiInputSpec& spec, const WindowAiUiNode& node,
    const RECT& clientArea);

/// 在候选里挑一个（返回下标；-1 = 没有合格项）。同分时：面积大的先、再按 y 靠后。
int PickWindowAiInput(const WindowAiInputSpec& spec,
    const std::vector<WindowAiUiNode>& nodes, const RECT& clientArea);

/// ★ 相对矩形（0..1 的 4 个数 l,t,r,b）→ 客户区**像素**矩形。
///
/// ⚠ 必须做**合法性**判断而不是"能用就用"：几何兜底会**真的点下去并打字**，
///   一个写错的 `rectHint`（比如把 0.5 写成 5）会点到窗口外面/别的程序上。
///   判据：4 个数、都在 [0,1]、右下严格大于左上。
bool ResolveRelativeRect(const std::vector<double>& rel, const RECT& clientArea, RECT& out);

/// ★这段文本看起来是**占位符**（空输入框的提示语）而不是用户草稿吗？
///
/// ⚠ 判据刻意**判严**（宁可真把草稿当草稿 → 中止一次，也不要把草稿当占位符 → 覆盖用户输入）：
///   · 命中提示词**必须在开头附近**（前 6 个字符内，空白不算）；且
///   · 整段**很短**（≤ 40 字符）。
///   反例：用户草稿「发消息给张三，内容是……」—— 开头命中但很长 ⇒ **不算占位符** ✓
bool WindowAiLooksLikePlaceholder(const std::vector<std::wstring>& hints,
    const std::wstring& text);

/// ★最长公共子串长度（纯逻辑）。用来判断"两段文本是不是同源"。
///
/// ⚠ 为什么不能用子串包含/片段比对（实测踩到）：**OCR 会认错字** ——
///   「**请**只回答一个数字：7+8 等于几？」被读成「**清**只回答一个数字：7 + 8等于几？」，
///   一个字之差就让 `find` 全部落空 ⇒ 判据失效。最长公共子串对**局部错字/插噪免疫**：
///   上面两段的 LCS = 「只回答一个数字：7+8等于几？」（去掉空白后 >10 字）⇒ 判为同源 ✓
size_t LongestCommonSubstringLen(const std::wstring& a, const std::wstring& b);

/// ★★挑「复制」按钮（返回节点下标；-1 = 没找到）。判据（纯逻辑，可逐格自检）：
///   · 只认 `Button`（或 `Hyperlink`：有些客户端把消息操作做成链接）；
///   · 名字命中 `copyButtonNames`（子串、大小写不敏感）**或** id 命中 `copyButtonIds`；
///   · 有可见面积、不在屏外；
///   · 多个命中时取 **y 最大者**（= 屏幕上最靠下 = 最新那条回答的操作栏）。
///
/// ⚠⚠ 为什么必须"取最靠下"：**用户自己发的那条消息下面也有复制按钮**。
///   点错了就把我们的提问复制回来，表现为"回答 = 提问"（而且回执一切正常，
///   因为剪贴板确实变了）。最靠下的那个才是最新回答的。
int PickWindowAiCopyButton(const WindowAiReplySpec& spec,
    const std::vector<WindowAiUiNode>& nodes, const RECT& clientArea);

/// ★剪贴板里读到的这段文本，能当"回答"用吗？（纯逻辑）
///
/// 三条都过才算可用：
///   ① **变了**（`now != prev`）—— 没变说明复制按钮没点中，或它复制的是同一段；
///   ② 非空、且**不是我们刚发出去的那句提问**（忽略空白比对）—— 见 PickWindowAiCopyButton
///      的"最靠下"说明：点错了就会把提问复制回来；
///   ③ 长度合理（≤ maxChars；超过 maxChars 的按上限截断由调用方处理，这里只管"大到离谱"）。
///
/// 返回不可用时填 `why`（可读原因，进回执/日志）。
bool WindowAiClipboardReplyUsable(const std::wstring& prev, const std::wstring& now,
    const std::wstring& prompt, int maxChars, std::string& why);

/// ★★挑「复制按钮」的**几何锚点**（返回 false = 没找到可用锚点）。
///
/// 判据（纯逻辑）：
///   ① 只在**文本类**节点里找（`Text` / `Document` / `Hyperlink` 的 name 也常是消息文本）；
///   ② 有可见面积、**在客户区内可见**（`rect.bottom <= clientArea.bottom` ——
///      实测会话很长时，DOM 里未渲染的部分 y 会超出窗口，取"最靠下"会锚到看不见的地方）；
///   ③ 横向落在会话列内（`columnLeftRel`..`columnRightRel`，默认 0.24~0.86；
///      排除左侧会话列表与右侧摘要栏）；
///   ④ 取**最靠下**的那个（聊天自上而下追加 ⇒ 最后一条最新）。
///   ⑤ `echoHint` 非空时，**必须在它下面**（回答在提问下面）—— 找不到回显就不加这条限制。
/// ★★挑「复制按钮」的**几何锚点**（返回 false = 没找到可用锚点）。
///
/// 判据（纯逻辑，可逐格自检）：
///   ① 只在**文本类**节点里找（`Text` / `Document` / `Hyperlink` / `Group`；消息正文就是 `Text`）；
///   ② 有可见面积、**在客户区内可见**（`rect.bottom <= clientArea.bottom` ——
///      实测会话很长时 DOM 里未渲染的部分 y 会超出窗口，只看"最靠下"会锚到看不见的地方）；
///   ③ 横向落在会话列内（`copyColumnLeftRel`..`copyColumnRightRel`）；
///   ④ **必须在 `bottomLimitY` 之上**（调用方传输入框顶边）：
///      ⚠⚠ 不加这条会锚到**输入区自己的东西** —— 实测第一版就锚到了输入框左侧「+」
///      弹出的附件菜单项（「上传文件或图片」），于是点下去打开的是附件菜单（真机上踩到）。
///   ⑤ 取**最靠下**的那个（聊天自上而下追加 ⇒ 最后一条最新）；
///   ⑥ `echoHint` 非空时，**必须在它下面**（回答在提问下面）；找不到回显就不加这条限制。
bool PickWindowAiCopyAnchor(const WindowAiReplySpec& spec,
    const std::vector<WindowAiUiNode>& nodes, const RECT& clientArea,
    const std::wstring& echoHint, int bottomLimitY, RECT& outAnchor);

/// ★★**首选**定位法：直接在 UIA 树里找「消息操作栏那一排按钮」，取**最靠下**那一排的
/// **最左一颗**（= 复制）。返回节点下标；-1 = 没找到。
///
/// 为什么它比"猜哪段文本是回答"可靠（实测对比）：
///   · 豆包客户端的操作栏就是**一排等间距的无名方形按钮** ——
///     实测 `x=851,902,953,1004,1055,1106,1157,1208`，全部 `36x36`，间距 51px，
///     最左那颗是「复制」、第二颗是「朗读」（带名字）⇒ **最左 = 复制** 这条很稳；
///   · 而"哪段文本是回答"极难判：实测被**推荐问题**（"10+10等于几？"）、
///     被**用户消息的拆分片段**反复抢走，两次都把落点带偏。
///
/// 判据（纯逻辑）：
///   ① 只认 `Button`；尺寸在 [minSize, maxSize] 方形区间内（默认 20~56）；
///   ② 横向落在会话列内（`copyColumnLeftRel`..`copyColumnRightRel`）；
///   ③ 在输入框上方（`bottomLimitY` 之上）；
///   ④ 按 y 分组（±6px 同排），**同一排至少 `minRowCount`（默认 3）颗**才算操作栏；
///   ⑤ 取**最靠下**那一排；返回其中**最左**的那颗。
///   ⚠ 不要求按钮有名字：豆包这排按钮**全是无名**的（只有「朗读」例外）。
int PickWindowAiCopyRowButton(const WindowAiReplySpec& spec,
    const std::vector<WindowAiUiNode>& nodes, const RECT& clientArea, int bottomLimitY);

/// ★在 `hay` 里找 `needle`，**忽略两边所有的空白字符**；命中时回填它在 hay 里的
/// **起止下标**（原串坐标，`outEnd` 指向最后一个非空白字符之后）。多次命中取**最后一次**。
///
/// ⚠⚠ 为什么必须有这个函数（实测踩了两次）：OCR 会在数字/字母之间**插空格** ——
///   我们发出去的「请只回答一个数字：11+12 等于几？」在屏幕上被识别成
///   「请只回答一个数字：1 1 + 1 2等于几？」⇒ 精确 `rfind` 必然落空，
///   于是退回"公共前缀剥离"，把整段会话（含推荐问题、右侧栏）都当成回答。
bool FindWindowAiEchoIgnoringSpaces(const std::wstring& hay, const std::wstring& needle,
    size_t& outBegin, size_t& outEnd);

/// ★从「发送后的整窗文本」里**提取回答**（纯函数，最容易静默出错的一步）。
///
/// 难点（真机上必踩）：
///   · 客户端会把**我们刚发出去的话**也显示在会话里 ⇒ 直接取尾部 => 读到自己的提问；
///   · 会话历史里可能有**上一次的相同提问**（同一个问题问两遍）⇒ 按内容定位会取错那一段；
///   · 有些客户端把输入框内容也算进整窗文本 ⇒ 末尾挂着还没发出去的字。
///
/// 判据（按优先级）：
///   ① 若 `after` 里能找到 `prompt` 的**最后一次**出现 ⇒ 取它之后的内容；
///   ② 否则与 `baseline` 做**公共前缀剥离**（会话是追加式的）⇒ 取新增部分；
///   ③ 再剥掉尾部若与 `prompt` 尾部重合的部分（输入框回显）。
/// 全程**不猜**：提取不出就返回空串，由调用方如实报"读不到回答"。
std::wstring ExtractWindowAiReply(const std::wstring& baseline, const std::wstring& after,
    const std::wstring& prompt, int maxChars);

/// 「内容稳定」判据（纯函数）：连续两次读数相同即视为生成结束。
/// ⚠ 与网页端同一条纪律：**不能只看"变了没"** —— 流式渲染每帧都在变，
///   必须"变了之后又连续相同"才算完；否则会把半截回答当最终答案。
struct WindowAiStability {
    std::wstring last;
    int sameCount = 0;
};
/// 喂一次读数；返回 true 表示"已稳定"（可收工）
bool FeedWindowAiStability(WindowAiStability& st, const std::wstring& current,
    int requiredSame = 2);

/// 把档案排版成给模型/人看的简短说明（诊断与探针报告用）
std::string FormatWindowAiProfile(const WindowAiClientProfile& p);

}  // namespace quickscript::webai
