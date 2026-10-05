#pragma once
// ──────────────────────────────────────────────────────────────────
// window_ai_driver.h — 窗口反代的**驱动层**：把一个 GUI 客户端窗口当成 AI 后端
//
// 与网页端驱动（`web_ai_driver.h`）**同形**：都是「写进去 → 提交 → 等新回答 → 读回来」，
// 只是把「扩展 + CDP + CSS selector」换成「UIA 控件属性 + 键鼠 + 本进程」。
//
// ⚠⚠ 三条与网页端不同的、必须显式交代的事实：
//   ① **要抢前台**：SendInput 只打前台窗口 ⇒ 本层会把目标客户端切到前台
//      （复用 `windowmode::ActivateWindow`，它会处理前台锁）。网页端走扩展、不动用户前台。
//   ② **没有"回读确认"的天然保证**：Electron 的 contenteditable 在 UIA 里可能既不支持
//      ValuePattern 也读不回文本 ⇒ 写入结果分三档如实回传：
//      `verified`（回读一致）/ `unavailable`（读不回，只能靠"粘贴成功"推断）/ `mismatch`（回读不一致 ⇒ 换下一条通道）。
//      **不许**把 `unavailable` 说成"成功"。
//   ③ **无 selector 稳定性**：客户端改版就会失配 ⇒ 配置化（`window_ai_providers.json`）+ 探针 dump。
//
// ⚠ 本层**不做** A 类·流量反代（拦 HTTP、绕额度）——见 docs/ai-proxy-roadmap.md §3.1。
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "web_ai/window_ai_profile.h"

namespace quickscript::webai {

/// 目标窗口（找到了什么）
struct WindowAiTarget {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring processName;
    bool foreground = false;
    bool minimized = false;
    /// ★这个窗口是**我们从"被隐藏"状态拉回来的**吗？
    ///   实测：Cursor 与豆包的主窗都会处于 `IsWindowVisible=false`（多半是本软件的窗口模式/
    ///   虚拟桌面留下的状态），而 `ListSwitchableWindows`（Alt+Tab 口径）看不到它们。
    ///   ⇒ 走 `EnumWindows` 的专用枚举把隐藏窗找出来并 `ShowWindow` 恢复；
    ///   这一项如实标出"曾恢复过"，别让回执看着像"本来就在那儿"。
    bool restoredFromHidden = false;
    /// ★这个窗口是从**别的虚拟桌面**搬回当前桌面的吗？
    ///   ⚠ 实测：窗口在别的桌面时，它的渲染被系统节流 ⇒ **UIA 树是空的**
    ///   （独立工具与产品内查询都返回 0 个节点，标题也读成空）。所以"树空"有时
    ///   不是客户端没开无障碍，而是**它不在当前桌面**。搬回来是唯一能让它可用的办法。
    bool movedToCurrentDesktop = false;
    RECT windowRect{};
    RECT clientRectScreen{};
};

/// 写入结果的**可信度**（不许把"读不回"说成"成功"）
enum class WindowAiWriteProof {
    kFailed = 0,       ///< 三条通道都没写进去
    kMismatch = 1,     ///< 写下去了但**回读对不上**（可能打进了别的地方 ⇒ 换下一条通道并如实记账）
    kUnavailable = 2,  ///< 写下去了但**读不回**（只能靠通道语义推断）
    kVerified = 3,     ///< 回读一致（最硬）
};

const wchar_t* WindowAiWriteProofName(WindowAiWriteProof p);

struct WindowAiWriteOutcome {
    WindowAiWriteProof proof = WindowAiWriteProof::kFailed;
    std::wstring channel;      ///< value-pattern | clipboard-paste | unicode-typing
    std::wstring readBack;     ///< 回读到的文本（截断，诊断用）
    std::wstring error;
    std::vector<std::wstring> tried;   ///< 依次试过哪些通道（失败原因也记）
};

/// 干跑写入（探针用）：聚焦 → 写入 marker → 回读 → **用退格撤销 marker**，不提交。
/// ⚠ 为什么用退格而不是 Ctrl+A+Del：在"编辑器 + 聊天面板"共存的客户端里，
///   Ctrl+A 可能选中**整个文件**，随后的 Delete 会**删掉用户的代码**（真·破坏性）。
///   退格只删我们自己刚打进去的那些字符，最坏情况是少删几个。
struct WindowAiDryRun {
    bool focused = false;
    WindowAiWriteOutcome write;
    std::wstring inputMatchedBy;
    std::wstring preExistingText;   ///< 写之前输入框里已有的内容（非空则**不写**，如实回报）
    bool clearedAfter = false;
    /// ★写入**之后**整窗 OCR 的开头片段（诊断用）：
    ///   回读区域读空时，靠它判断"字到底打进去了没有、落到哪去了" ——
    ///   否则只能看到一句"回读为空"，无法区分「粘贴没生效」和「回读区域量错了」。
    std::wstring postWriteOcrHead;
    /// ★回读走的 OCR 通道报的原始错误（不许吞掉）
    std::string ocrError;
    std::string error;
    std::string message;
};

/// 一轮「写进去 + 提交 + 等新回答」的结果
struct WindowAiSendOutcome {
    bool ok = false;
    std::string error;    ///< NO_CLIENT_WINDOW / NO_INPUT / FOCUS_FAILED / WRITE_FAILED /
                          ///< SUBMIT_FAILED / NO_REPLY / REPLY_NOT_STABLE / ...
    std::string message;
    WindowAiTarget target;
    /// ★ 这一轮走的是哪条路（**如实标出，不许把几何兜底说成 UIA 定位**）：
    ///   `uia`          = UIA 控件树里定位到输入框
    ///   `geometry-ocr` = UIA 树为空（Electron 默认）⇒ 按配置的相对矩形点下去 + 系统 OCR 读
    std::wstring mode;
    /// ★ 会话文本是从哪读到的：`uia` / `ocr`。
    ///   ⚠ 必须如实带出去：Chromium 客户端的 UIA 只给**标题级**文本（实测豆包 26~43 字），
    ///     两条路读到的**不是同一个东西**，排障时第一眼就要看这一项。
    std::string replySource;
    std::wstring inputMatchedBy;   ///< 命中的输入框（类型 + 名字片段）
    WindowAiWriteOutcome write;
    std::wstring baselineText;
    std::wstring replyText;
    int findMs = 0;
    int activateMs = 0;
    int focusMs = 0;
    int writeMs = 0;
    int submitMs = 0;
    int waitMs = 0;
    int baselineChars = 0;
    int replyChars = 0;
    bool truncated = false;
};

class WindowAiDriver {
public:
    static WindowAiDriver& Instance();

    /// 配置里的客户端档案（内置 + `AppDir()\window_ai_providers.json` 覆盖）
    const std::vector<WindowAiClientProfile>& Clients();
    /// 强制重读配置（改完 json 不用重启，探针会调）
    void ReloadClients();

    /// 列出当前**可切换的**窗口（探针/诊断用：先看有没有目标进程在跑）
    std::vector<WindowAiTarget> ListCandidateWindows();

    /// 按档案找窗口。找不到时 err 说清"进程没跑 / 标题不匹配 / 被排除项挡了"
    bool FindClientWindow(const WindowAiClientProfile& profile, WindowAiTarget& out,
        std::string& err);

    /// ★抓取窗口的 UIA 控件快照（探针/调参的主力：把真实树 dump 出来）
    /// @param maxNodes 上限（默认 400；Electron 的树很大）
    bool SnapshotControls(HWND hwnd, std::vector<WindowAiUiNode>& out, int maxNodes = 400);

    /// 探针报告：候选窗口 + 控件 dump + 输入框候选打分
    nlohmann::json ProbeReport(const std::string& clientId, bool includeControls);

    /// 定位输入框（返回快照下标；-1 = 没找到）
    int FindInput(const WindowAiClientProfile& profile, HWND hwnd,
        std::vector<WindowAiUiNode>& nodes, std::string& err);

    /// ★端到端一次：找窗 → （**后台灌键，不抢前台**）→ 定位输入框 → 写入 → 提交 → 等新回答 → 读回
    /// @param newConversation 这次要不要**先在客户端开一个新对话** —— 对应网页端的
    ///        `plan.newConversation`。⚠ 不实现它就会出现用户实测的那个 bug：
    ///        新对话没生效、多轮全打进同一个历史对话、甚至把别的会话的回答读回来。
    WindowAiSendOutcome SendAndRead(const std::string& clientId, const std::wstring& text,
        int idleTimeoutMs = 0, int maxTotalMs = 0, bool newConversation = false);

    /// ★ 本轮的后台**软悬停**回调（客户区坐标 → 是否已悬停）。
    ///   后台模式下由 `SendAndRead` 装上（走窗口模式会话的软光标：进程内、不动物理鼠标、
    ///   不抢前台）；前台模式为空。用途：**悬停让「消息操作栏」出现**，再 Invoke 复制按钮
    ///   —— 操作栏是 hover 才渲染的，不悬停就永远找不到它。
    std::function<bool(int clientX, int clientY)> softHover_;

    /// ★在客户端**开一个新对话**（找「新对话」按钮 → Invoke；不需要前台）。
    /// 找不到按钮 ⇒ false + 可读原因（**不硬着头皮往当前对话里写** —— 那正是串台的来源）。
    bool StartNewConversation(const WindowAiClientProfile& profile, HWND hwnd, std::string& err);

    /// ★干跑写入（探针用，**不提交**）：用来确认"到底能不能把字写进这个客户端的输入框"。
    WindowAiDryRun DryRunWrite(const std::string& clientId, const std::wstring& marker);

    /// ★★剪贴板通道（**首选读法**）：点回答下方的「复制」按钮 → 读剪贴板。
    ///
    /// 为什么它排在最前：剪贴板里是客户端自己给的原文（准确），一次点击 + 一次读取（快），
    /// 且不受可视区域限制（完整）—— 见 `WindowAiReplySpec` 里那段说明。
    /// 步骤：① 记下当前剪贴板 → ② 找「复制」按钮（UIA 名字/ id，找不到就用几何提示）
    /// → ③ 点它 → ④ 轮询等剪贴板变化（`clipboardWaitMs`）→ ⑤ **再读一次确认稳定**
    /// （生成中复制会拿到半截）→ ⑥ 按配置把用户原来的剪贴板还回去。
    ///
    /// @param why 拿不到时的可读原因（进回执/日志，**不许吞**）
    /// @param overrideX / overrideY **标定用**：≥0 时直接点这个屏幕坐标
    ///        （跳过"按锚点+偏移算落点"那一步）。标定扫描靠它遍历候选点。
    bool TryClipboardReply(const WindowAiClientProfile& profile, HWND hwnd,
        const std::wstring& prompt, std::wstring& out, std::string& why,
        int overrideX = -1, int overrideY = -1);

    /// ★标定扫描：在锚点附近的**小范围**里试几个落点，第一个让剪贴板变化的即为复制图标。
    ///   返回是否找到；`outPoint` 是命中的屏幕坐标，`outOffset` 是换算回
    ///   `[dx, dy]`（相对客户区宽/高）的结果 —— 直接写进 json 即可，不用再算。
    ///
    /// ⚠ 扫描范围**刻意很窄**（默认 x ±14px、y 锚点下方 20~80px）：
    ///   消息操作栏从左到右是 复制/赞/踩/分享/重新生成，往右扫会误触这些按钮
    ///   （"重新生成"会真的重跑一轮）。宁可不中，也不乱点。
    /// 标定扫描命中的一个候选落点（内容 + 落点 + 换算出的偏移）
    struct CopyScanHit {
        POINT point{};
        std::wstring text;
        std::vector<double> offset;   ///< [dx, dy] 相对客户区宽/高
    };

    /// @param outHits **所有**让剪贴板变化的落点（按扫描顺序）。
    ///        ⚠ 刻意"只收集、不判对错"：哪些内容是回答、哪些是别的卡片里的字，
    ///        要人看内容才知道（工具负责把地图打出来，判断留给调用方/用户）。
    bool ScanCopyButton(WindowAiClientProfile profile, HWND hwnd, const std::wstring& prompt,
        std::wstring& outReply, POINT& outPoint, std::vector<double>& outOffset, std::string& why,
        std::vector<CopyScanHit>* outHits = nullptr);

    /// ★上一次 `TryClipboardReply` 的**落点诊断**（哪条路、锚点矩形、点了哪里）。
    ///   为什么要暴露它：几何点击失败时，"点没点中"无法从结果反推 ——
    ///   必须把**实际落点**回报出来，用户才能对着截图量出正确的偏移。
    std::string LastClipboardDiag() const;

    /// 上一次剪贴板通道的落点诊断（`LastClipboardDiag` 的后备字段）
    std::string clipboardDiag_;

    /// ★我们**发给这个客户端**的历史 prompt（最近 8 条）。
    ///
    /// 用途：识别"输入框里那坨字是不是我们自己上一轮留下的"。
    /// ⚠ 为什么不能靠"和会话正文比对"（第一版就是这么写的，现场失效）：会话读回**本身不可靠**
    ///   —— 实测同一次调用里窗口的 UIA 树可能只有 25 字（树在 6~308 节点之间反复），
    ///   拿它当参照等于把两件不可靠的事串起来。自己记一份 = 判据不依赖任何窗口状态。
    std::vector<std::wstring> sentPrompts_;
    bool sentPromptsLoaded_ = false;
    void RememberSentPrompt(const std::wstring& text);
    /// 从 `AppDir()\window_ai_sent_prompts.json` 载入（懒加载；跨重启保留）
    void LoadSentPrompts();

    /// 只读一次会话文本（探针用）    /// @param sourceOut 出参：这次文本是从哪读到的（`uia` / `ocr` / `uia-echo` / `ocr-echo`）——
    ///        ⚠ 必须如实带出去：Chromium 客户端的 UIA 只给标题级文本，
    ///          两条路读到的**不是同一个东西**，排障时要看这一项。
    /// @param echoHint 我们刚发出去的那句话。**非空时按"哪份文本里有它的回显"择优**
    ///        （UIA 那份含会话正文但混着侧栏；OCR 那份会把图标读成 `0`）
    ///        —— 实测：按长度择优会选到更脏的 OCR 那份，回答直接变成 `0`。
    bool ReadConversation(const WindowAiClientProfile& profile, HWND hwnd, std::wstring& text,
        std::string& err, std::string* sourceOut = nullptr,
        const std::wstring& echoHint = std::wstring());
};

}  // namespace quickscript::webai
