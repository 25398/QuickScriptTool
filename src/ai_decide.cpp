// ──────────────────────────────────────────────────────────────────
// ai_decide.cpp — 本地判断表的实现（纯函数，无宿主依赖）
// 设计说明与硬规则见 ai_decide.h 顶部注释。
// ──────────────────────────────────────────────────────────────────
#include "ai_decide.h"

#include <cstdio>
#include <cwchar>

namespace {

/// 动态覆盖率阈值（判断一用）：原值，逐份日志硬调出来的，勿凭感觉改。
constexpr double kBusyDeny = 0.10;              // 浏览器前台 + 高动态 → 拦
constexpr double kBusyDenyWhileChanging = 0.04; // 且还没稳定 → 更低门槛也拦
/// 动态覆盖率阈值（判断二用）：实测同一局游戏覆盖率在 0.05~0.22 之间跳。
constexpr double kGameBusy = 0.03;
constexpr double kGameBusyWhileChanging = 0.02;

}  // namespace

AiConfTier AiConfidenceTier(double confidence) {
    if (confidence >= kAiDecideConfHigh) return AiConfTier::High;
    if (confidence >= kAiDecideConfLow) return AiConfTier::Medium;
    return AiConfTier::Low;
}

std::wstring FormatAiDecision(const wchar_t* decisionName, const wchar_t* verdictName,
    const AiDecisionRecord& rec) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"%.2f", rec.confidence);
    std::wstring s = L"[判断] ";
    s += (decisionName ? decisionName : L"?");
    s += L" verdict=";
    s += (verdictName ? verdictName : L"?");
    s += L" conf=";
    s += buf;
    s += L" why=";
    s += rec.why;
    return s;
}

std::wstring FormatAiSignals(const AiDecideSignals& in) {
    wchar_t busy[32]{};
    swprintf_s(busy, L"%.3f", in.busyCoverage);
    std::wstring s = L"sig page=";
    // 空页型写成 `-`：解析方要能区分「没观察过」与「页型是空串」
    s += in.pageKind.empty() ? L"-" : in.pageKind;
    s += L" br=";      s += in.foregroundBrowserClass ? L"1" : L"0";
    s += L" self=";    s += in.foregroundSelfDrawn ? L"1" : L"0";
    s += L" sheet=";   s += in.foregroundSpreadsheet ? L"1" : L"0";
    s += L" web=";     s += in.webSessionActive ? L"1" : L"0";
    s += L" hooks=";   s += in.domHooksAvailable ? L"1" : L"0";
    s += L" busy=";    s += busy;
    s += L" changing="; s += in.settleStillChanging ? L"1" : L"0";
    return s;
}

// ══════════════════════════════════════════════════════════════════
// 判断一：视觉定位闸
//
// 表驱动（每一行 = 一条分流规则），顺序即优先级，与旧实现逐条对应：
//   dom/mixed → canvas → 网页会话 → 非浏览器前台 → 浏览器+高动态拦 → 浏览器兜底
// 「返回第一条命中的行」而不是嵌套 if，是为了让**优先级一眼可见**、
// 并且能对着自检逐格核对。改判据时请连同行号注释一起改，并补自检。
// ══════════════════════════════════════════════════════════════════
const wchar_t* AiVisionGateName(AiVisionGate v) {
    switch (v) {
    case AiVisionGate::Allow: return L"allow";
    case AiVisionGate::DenyBusyFallback: return L"deny_busy_fallback";
    }
    return L"?";
}

AiVisionGate AiDecideVisionGate(const AiDecideSignals& in, AiDecisionRecord* out) {
    const bool kindDom = (in.pageKind == L"dom" || in.pageKind == L"mixed");
    const bool kindCanvas = (in.pageKind == L"canvas");
    const bool pageBusy = in.busyCoverage >= kBusyDeny
        || (in.settleStillChanging && in.busyCoverage >= kBusyDenyWhileChanging);

    // 「视觉是唯一手段」的两种情况（行②画布页 / 行④非浏览器前台）：没有网页树可退。
    // 用途：① 第 2 刀据此**不注入**「改用控件树」提示；② 三档路由的语义基础。
    const bool visionIsOnlyWay = kindCanvas || !in.foregroundBrowserClass;

    struct Row {
        bool hit;
        AiVisionGate verdict;
        double confidence;
        const wchar_t* why;
    };
    const Row rows[] = {
        // ① 网页有可访问性树：树上能点就别烧识图 token（本闸的原始用意）
        { kindDom, AiVisionGate::Allow, 0.90,
          L"页型是 dom/mixed：有控件树可点，视觉仍可用（优先树）" },
        // ② 画布页 = 网页游戏/绘图/播放器外壳：本来就没有可用节点，只能靠视觉
        { kindCanvas, AiVisionGate::Allow, 0.90,
          L"画布页：DOM 没有可用节点，视觉是唯一手段" },
        // ③ 已是网页会话：视觉兜底仍可用（扩展只是优化，未装扩展/树上没有时照旧识图）
        { in.webSessionActive, AiVisionGate::Allow, 0.75,
          L"本次已是网页会话：视觉兜底可用" },
        // ④ 前台不是浏览器窗口（桌面游戏/自绘应用/模拟器）→ 根本没有控件树可退。
        //    ★这一条是「游戏没反应」那个坑的修复点：视觉是唯一手段，必须放行。
        { !in.foregroundBrowserClass, AiVisionGate::Allow, 0.90,
          L"前台不是浏览器窗口：没有网页树可退，视觉是唯一手段" },
        // ⑤ 浏览器前台但画面高动态、页型未知：拦住识图，让模型先 observePage 拿树。
        //    ⚠ 必须显式要求 foregroundBrowserClass，否则会误伤行④已放行的情况。
        { in.foregroundBrowserClass && pageBusy,
          AiVisionGate::DenyBusyFallback, 0.55,
          L"浏览器前台页型未知且画面高动态：先 observePage 拿树，别在动图上烧识图" },
        // ⑥ 浏览器前台 + 页型未知 + 画面安静：没有更好的路，视觉照常。
        //    ⚠ 置信度必须**低于 kAiDecideConfHigh**：这是「没有证据、只能照常」
        //    的兜底行，不是硬证据。写成 0.70 会正好压在 High 边界上，
        //    于是三档路由判定「有硬证据」→ 不加任何提示，与表格语义自相矛盾。
        { in.foregroundBrowserClass, AiVisionGate::Allow, 0.68,
          L"浏览器前台页型未知但画面安静：识图代价可控（无硬证据，属兜底放行）" },
    };
    static_assert(sizeof(rows) / sizeof(rows[0]) == 6, "判断一行数变了：请同步自检");

    AiVisionGate verdict = AiVisionGate::Allow;
    AiDecisionRecord rec;
    bool found = false;
    for (const Row& r : rows) {
        if (!r.hit) continue;
        verdict = r.verdict;
        rec.confidence = r.confidence;
        rec.why = r.why ? r.why : L"";
        found = true;
        break;
    }
    if (!found) {
        // 表必须穷尽（行⑥以「是浏览器前台」收尾，覆盖了剩余所有情况）。
        // 走到这里说明有人加行时漏了兜底 —— 明确报出来，别静默放行。
        verdict = AiVisionGate::DenyBusyFallback;
        rec.confidence = 0.0;
        rec.why = L"判断表未覆盖（实现缺陷：请补齐 AiDecideVisionGate 的兜底行）";
    }

    // 置信度修正：没有扩展 DOM 钩子时，「先拿树」这条退路其实不存在，
    // 拦住识图的把握要降一档（判决不变，第 1 刀只让诊断更诚实）。
    if (verdict == AiVisionGate::DenyBusyFallback && !in.domHooksAvailable) {
        if (rec.confidence > 0.50) rec.confidence = 0.50;
        rec.why += L"；且宿主没有 DOM 钩子，「先拿树」这条退路可能不存在";
    }

    // 「视觉是唯一手段」只对放行有意义（行②画布页 / 行④非浏览器前台）：
    // 调用方据此**不要**再注入「改用控件树」——那正是把游戏锁死的那条错误指引。
    rec.visionIsOnlyWay = (verdict == AiVisionGate::Allow && visionIsOnlyWay);

    if (out) *out = rec;
    return verdict;
}

// ══════════════════════════════════════════════════════════════════
// 判断二：游戏前台
// ══════════════════════════════════════════════════════════════════
const wchar_t* AiGameForegroundName(AiGameForeground v) {
    switch (v) {
    case AiGameForeground::NotGame: return L"not_game";
    case AiGameForeground::GameByNoTree: return L"game_no_tree";
    case AiGameForeground::GameByCanvasPage: return L"game_canvas_page";
    case AiGameForeground::GameByMotion: return L"game_by_motion";
    }
    return L"?";
}

AiGameForeground AiDecideGameForeground(const AiDecideSignals& in, AiDecisionRecord* out) {
    const bool kindCanvas = (in.pageKind == L"canvas");
    const bool busyMotion = in.busyCoverage >= kGameBusy
        || (in.settleStillChanging && in.busyCoverage >= kGameBusyWhileChanging);

    struct Row {
        bool hit;
        AiGameForeground verdict;
        double confidence;
        const wchar_t* why;
    };
    const Row rows[] = {
        // ① 画布页（网页游戏/绘图）= 没有可用节点，必然走视觉
        { kindCanvas, AiGameForeground::GameByCanvasPage, 0.90,
          L"画布页：DOM 无可用节点，视觉是唯一手段" },
        // ② 浏览器前台：有 DOM 可退，不算「只能靠视觉」（视频页也是）
        { in.foregroundBrowserClass, AiGameForeground::NotGame, 0.85,
          L"前台是浏览器：有 DOM 可退，不判游戏" },
        // ③ 表格/办公软件有各自更精确的定位路线（Ctrl+Home、UIA），不算游戏
        { in.foregroundSpreadsheet, AiGameForeground::NotGame, 0.85,
          L"前台是表格/办公软件：走 UIA/键盘路线，不判游戏" },
        // ④ ★主判据：没有控件树 = 自绘画面（游戏/模拟器/自绘应用）。
        //   比「动态覆盖率」可靠得多——开局静止画面覆盖率 <3%，旧判据会漏判
        //   （第十三份日志：前 4 轮没认成游戏 → 玩法指引全没注入 → 光思考烧几十秒）。
        { in.foregroundSelfDrawn, AiGameForeground::GameByNoTree, 0.88,
          L"前台自绘（子窗口≈0）：没有控件树，视觉是唯一手段" },
        // ⑤ 兜底判据：画面在持续变（静态桌面软件覆盖率很低）
        { busyMotion, AiGameForeground::GameByMotion, 0.55,
          L"画面持续重绘：疑似游戏/模拟器/视频前台（只有这条定量证据）" },
    };
    static_assert(sizeof(rows) / sizeof(rows[0]) == 5, "判断二行数变了：请同步自检");

    for (const Row& r : rows) {
        if (!r.hit) continue;
        AiDecisionRecord rec;
        rec.confidence = r.confidence;
        rec.why = r.why ? r.why : L"";
        if (out) *out = rec;
        return r.verdict;
    }

    // 没有定量证据：按非游戏处理（保守 —— 不注入游戏指引，只说明为什么）
    AiDecisionRecord rec;
    rec.confidence = 0.50;
    rec.why = L"无动/静态证据（画面覆盖率未达阈值且前台有控件树）：按非游戏处理";
    if (out) *out = rec;
    return AiGameForeground::NotGame;
}

bool AiGameForegroundDecisionIsDecisive(const AiGameForegroundDecision& d) {
    // 只有这两行是结构性证据（DOM 里没有可用节点 / 前台没有控件树）。
    // 「只有画面在动」（GameByMotion）不够 —— 视频、动画广告都是这个特征。
    if (d.verdict != AiGameForeground::GameByNoTree
        && d.verdict != AiGameForeground::GameByCanvasPage)
        return false;
    return d.record.confidence >= kAiGameConfDecisive;
}

// ══════════════════════════════════════════════════════════════════
// 判断三：规划轮是否附观察截图
// ══════════════════════════════════════════════════════════════════
const wchar_t* AiAttachFrameName(AiAttachFrame v) {
    switch (v) {
    case AiAttachFrame::NoModelCannotSee: return L"no_model_cannot_see";
    case AiAttachFrame::NoNoImage: return L"no_no_image";
    case AiAttachFrame::YesFrame: return L"yes_frame";
    }
    return L"?";
}

AiAttachFrame AiDecideAttachObserveFrame(bool modelSupportsVision, bool haveImage,
    AiDecisionRecord* out) {
    if (!modelSupportsVision) {
        AiDecisionRecord rec;
        rec.confidence = 0.90;
        rec.why = L"模型不支持视觉：附了也会降级成路径提示，不必花这份 token";
        if (out) *out = rec;
        return AiAttachFrame::NoModelCannotSee;
    }
    if (!haveImage) {
        AiDecisionRecord rec;
        rec.confidence = 0.90;
        rec.why = L"本轮没有可附的观察帧";
        if (out) *out = rec;
        return AiAttachFrame::NoNoImage;
    }
    AiDecisionRecord rec;
    rec.confidence = 0.80;
    rec.why = L"模型可看图且有帧可附";
    if (out) *out = rec;
    return AiAttachFrame::YesFrame;
}

// ══════════════════════════════════════════════════════════════════
// 诊断出口（环形缓冲 + 可选 sink）
// ══════════════════════════════════════════════════════════════════
namespace {

/// 最近若干条判断日志（够看一个动作的完整决策链即可）
constexpr size_t kAiDecisionLogMax = 32;
std::vector<std::wstring>& DecisionLogBuf() {
    static std::vector<std::wstring> buf;
    return buf;
}
void (*&DecisionLogSink())(const std::wstring&) {
    static void (*sink)(const std::wstring&) = nullptr;
    return sink;
}

}  // namespace

void SetAiDecisionLogSink(void (*sink)(const std::wstring& line)) {
    DecisionLogSink() = sink;
}

void NoteAiDecisionLog(const std::wstring& line) {
    if (line.empty()) return;
    auto& buf = DecisionLogBuf();
    buf.push_back(line);
    if (buf.size() > kAiDecisionLogMax) buf.erase(buf.begin());
    if (auto sink = DecisionLogSink()) sink(line);
}

std::vector<std::wstring> RecentAiDecisionLogs() {
    return DecisionLogBuf();
}

void ClearAiDecisionLogs() {
    DecisionLogBuf().clear();
}

AiSlowRoundVerdict AiDecideSlowThinkingRound(int streakBefore, unsigned long long apiMs,
    bool thinkingEnabledThisRound, bool inAiActionScope, bool actedThisRound) {
    AiSlowRoundVerdict v;
    if (!thinkingEnabledThisRound || !inAiActionScope) {
        // 不计数，但也不清 0：调用方本来就只在满足条件时才该更新 streak。
        v.newStreak = streakBefore;
        return v;
    }
    // ★出手了就不算「绕圈」（宾语是「想很久**且**没换来动作」，见头文件）。
    //   顺手把 streak 清零：一次真出手足以证明它不是在原地打转，
    //   留着旧计数会让「慢—快—慢」这种正常节奏凑出一次误伤。
    if (actedThisRound) {
        v.newStreak = 0;
        return v;
    }
    if (apiMs >= kAiVerySlowRoundMs) {
        v.suppress = true;
        v.suppressRounds = kAiThinkingSuppressRounds;
        v.newStreak = 0;
        v.why = L"本轮等模型 " + std::to_wstring(apiMs)
            + L"ms（>=" + std::to_wstring(kAiVerySlowRoundMs)
            + L"ms）**且这一轮没有任何工具调用**（想很久没换来动作）→ 接下来 "
            + std::to_wstring(kAiThinkingSuppressRounds) + L" 轮关闭思考，逼它直接出手";
        return v;
    }
    if (apiMs >= kAiSlowRoundMs) {
        v.newStreak = streakBefore + 1;
        if (v.newStreak >= 2) {
            v.suppress = true;
            v.suppressRounds = kAiThinkingSuppressRounds;
            v.newStreak = 0;
            v.why = L"连续 2 轮「等模型 >=" + std::to_wstring(kAiSlowRoundMs)
                + L"ms 且没有工具调用」（只想不干）→ 接下来 "
                + std::to_wstring(kAiThinkingSuppressRounds) + L" 轮关闭思考";
        }
        return v;
    }
    v.newStreak = 0;   // 快轮 → 连续中断
    return v;
}

// ── 工具轮的「收束」判据（docs §42）──────────────────────────────────
// 判决表（穷尽；走不到兜底 = 实现缺陷，与 AiDecideVisionGate 同一口径）：
//   ① 不期待工具（非工具轮）                      → None
//   ② 已有工具调用 / 工具 JSON 已在组装           → None（正在调工具）
//   ③ AI 动作作用域 + 产出 ≥ 预算 且毫无工具意向  → OutputBudget   ← 新增的形态
//   ④ 只有思考、没有任何正文，且已拖满绝对上限    → NoToolCallTooLong（原闸①逐字）
//   ⑤ 只有思考、没有任何正文，且已停顿满上限      → NoToolCallTooLong（原闸①逐字）
//   ⑥ 兜底                                        → None
// ⚠ ③ 只在 AI 动作作用域内生效：聊天助手里「回答得很长」正是用户要的东西，
//   在动作作用域内则**任何**合法的工具轮输出都远小于 12KB。
// ⚠ ④⑤ 保留 `contentBytes == 0` 的原判据（它们管的是「只想不干」，不是本条要修的形态），
//   这样聊天助手那边的既有行为逐字不变。
const wchar_t* AiStreamBrakeKindName(AiStreamBrakeKind k) {
    switch (k) {
    case AiStreamBrakeKind::OutputBudget: return L"产出超预算";
    case AiStreamBrakeKind::NoToolCallTooLong: return L"只思考不调工具";
    case AiStreamBrakeKind::None: break;
    }
    return L"不拦";
}

AiStreamBrakeVerdict AiDecideStreamBrake(const AiStreamBrakeSignals& s) {
    AiStreamBrakeVerdict v;
    v.producedBytes = s.reasoningBytes + s.contentBytes;
    v.reasoningDominated = s.reasoningBytes > s.contentBytes;
    if (!s.expectTools) return v;                                   // ①
    if (s.sawUsableToolCalls || s.toolAssemblyStarted) return v;     // ②
    // ③ 正文（**不是**思考）超预算且毫无工具意向 —— 这正是 §42 那条形状
    //    （实测 57 895 字节**全在 content 里**的散文）。
    //    ⚠ 旧写法用的是 `producedBytes`（思考+正文）⇒ 把**正常的推理**也当成"只想不干"掐掉
    //    （真机：14/14/15KB 的思考 ×3 次，每次都掐在思考说一半，再关掉思考重试
    //    ⇒ 模型只能给又短又浅的动作 = 用户说的「呆呆的」，docs §67）。
    if (s.inAiActionScope && s.contentBytes >= kAiStreamProseBudgetBytes) {
        v.kind = AiStreamBrakeKind::OutputBudget;                    // ③
        v.why = "no_tool_call_intent_after_12kb_content";
        return v;
    }
    // ③-b 纯思考也有个**大得多**的兜底上限（防死循环）；正常推理 14~15KB 不该被它碰到。
    if (s.inAiActionScope && s.reasoningBytes >= kAiStreamReasoningBudgetBytes) {
        v.kind = AiStreamBrakeKind::OutputBudget;
        v.why = "reasoning_runaway";
        return v;
    }
    const bool reasoningOnly = (s.contentBytes == 0 && s.reasoningBytes > 0);
    if (reasoningOnly && s.elapsedMs >= kAiStreamNoToolForceMs) {     // ④
        v.kind = AiStreamBrakeKind::NoToolCallTooLong;
        v.why = "reasoning_only_and_too_long";
        return v;
    }
    if (reasoningOnly && s.gotAnyByte && s.idleMs >= kAiStreamNoToolPlateauIdleMs) {  // ⑤
        v.kind = AiStreamBrakeKind::NoToolCallTooLong;
        v.why = "reasoning_only_and_plateaued";
        return v;
    }
    return v;                                                       // ⑥
}

std::wstring FormatAiStreamBrakeStatus(const AiStreamBrakeVerdict& v) {
    // 日志必须说**实测**的拆分：旧文案写死「预算基本被推理吃光了」，
    // 而那一次 57 895 字节全在「回复」里 —— 于是「关闭思考」这个处置瞄错了东西，
    // 而且日志本身在说谎（§41.3）。这里只报数，不猜归因。
    const size_t rkb = (v.producedBytes > 0) ? (v.producedBytes + 1023) / 1024 : 0;
    std::wstring who = v.reasoningDominated ? L"以思考为主" : L"以正文为主";
    std::wstring head = (v.kind == AiStreamBrakeKind::OutputBudget)
        ? (L"本轮没有工具调用意向却已产出 " + std::to_wstring(rkb) + L"KB（"
            + who + L"）")
        : (L"本轮只在思考、始终不调工具（" + std::to_wstring(rkb) + L"KB）");
    return head + L"→ 结束流式，逼它直接出手…";
}

AiNoToolVerdict AiDecideNoToolCallAnswer(bool mustAct, bool hasToolCalls, bool producedText,
    bool endedNaturally, int nudgesUsed, int maxNudges) {
    AiNoToolVerdict v;
    if (!mustAct) {                                                  // ① 不要求动手
        v.why = L"本轮不要求动手（聊天回答/非动作作用域）";
        return v;
    }
    if (hasToolCalls) {                                              // ② 已经动手了
        v.why = L"本轮已有工具调用";
        return v;
    }
    if (!producedText) {                                             // ③ 空回答交给既有错误路径
        v.why = L"本轮没有产出任何文本";
        return v;
    }
    if (endedNaturally) {                                            // ④ 模型自己收尾
        v.why = L"模型自己收尾（finish_reason=stop），尊重它";
        return v;
    }
    if (nudgesUsed >= maxNudges) {                                   // ⑤ 纠偏有界
        v.why = L"已纠偏 " + std::to_wstring(nudgesUsed)
            + L" 次仍不调工具，不再纠缠（守卫不能把模型永久锁在外面）";
        return v;
    }
    v.action = AiNoToolAction::ForceToolCall;                        // ⑥
    v.why = L"这段文本是被截断/被收束的独白，不是回答 → 要求它只调工具、别写解释";
    return v;
}

AiEmptyResponseVerdict AiDecideEmptyResponseRecovery(bool mustAct, bool thinkingWasOn,
    bool afterCutOrTruncation, int recoveriesUsed, int maxRecoveries) {
    AiEmptyResponseVerdict v;
    if (!mustAct) {                                              // ① 不要求动手
        v.why = L"不在动作执行作用域：空回答不在这里兜圈";
        return v;
    }
    if (recoveriesUsed >= maxRecoveries) {                       // ② 兜底有界
        v.why = L"已兜 " + std::to_wstring(recoveriesUsed)
            + L" 次仍是空体，不再纠缠（守卫不能把模型永久锁在外面）";
        return v;
    }
    if (afterCutOrTruncation) {                                  // ③ 被收束/截断过
        v.action = AiEmptyResponseAction::NudgeAndRetry;
        v.why = L"本轮被收束/截断后网关回空体（官方记载：预算被推理吃光时正文为空，不是故障）";
        return v;
    }
    if (thinkingWasOn) {                                         // ④ 开着思考
        v.action = AiEmptyResponseAction::NudgeAndRetry;
        v.why = L"本轮开着思考且网关回空体（最可能是预算被推理吃光）";
        return v;
    }
    v.why = L"没开思考也不是收束/截断：更像传输层问题，不兜（交给错误路径如实报）";
    return v;
}
