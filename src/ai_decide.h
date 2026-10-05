#pragma once
// ──────────────────────────────────────────────────────────────────
// ai_decide.h — AI 动作执行的**本地判断表**（System One 形态）
//
// 背景：这条链路上原本散着若干「阈值 + if-else」的裸判断（动态覆盖率 0.10/0.04/0.03、
// 页面类型、前台类名、是否有控件树…），阈值是逐份日志硬调出来的，判断理由不可见，
// 出问题只能翻代码猜，阈值本身也无法单测。
//
// 本模块把这几个判断统一成同一形态（对齐 TypeSafe Jev 的 System One「原语」：
// Noul=真假 / Choice=多选一 / Score=有序档位）：
//
//     输入 = 引擎**已有的本地状态向量**（AiDecideSignals）
//     输出 = AiDecisionRecord{ verdict 枚举, confidence 0~1, why 字符串 }
//
// 三条硬规则（改这个文件前先读）：
//   ① **纯函数**：不读线程局部、不查前台窗口、不碰文件/网络。所有宿主事实由
//      `AiDecideSignals` 传入 —— 这样每个判断都能被自检逐格断言（旧实现只能测外层布尔）。
//   ② **布尔结果必须与原实现逐字等价**：旧函数保留为薄封装（见 macro_execute_tools.cpp 的
//      `AiActionUiTooBusyForVisionLocate` 等），行为由既有自检钉死
//      （AiActionRouterSelfTest / game_foreground_vision_allowed、history_sidebar_and_busy_guards）。
//   ③ **confidence 不是准确率**，是「本次判断有多少本地证据」的度量，只用来分档
//      （高→直接执行 / 中→执行但附提示 / 低→回落保守路径）。它与 Jev 的 confidence
//      一样是分布集中度而非校准概率，**不要**当命中率使用。
//
// 为什么默认 0.5 而不是 0.0：阈值式判断的「未命中」不等于「确定不发生」，
// 只表示没有定量证据。回落到定性信号时给中性置信度，语义上更诚实。
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

// ── 一档判断的置信度分界（三档路由用）──────────────────────────────
// 高：有硬证据（页面类型 / 窗口类名 / 控件树 / DOM 钩子），直接执行，不加提示。
// 中：只有定量阈值（动态覆盖率）在撑，执行但附「可能不准」提示。
// 低：证据不足 → 走保守路径并注入提示。
constexpr double kAiDecideConfHigh = 0.70;
constexpr double kAiDecideConfLow  = 0.40;

/// 置信度分档（判断侧只暴露这个，具体阈值集中在上面两个常量里）
enum class AiConfTier {
    Low = 0,   // < kAiDecideConfLow      证据不足 → 保守路径 + 提示
    Medium,    // [Low, High)             执行 + 附「可能不准」提示
    High,      // >= kAiDecideConfHigh    直接执行，不打扰模型
};

// ── 思考失控闸（docs §39.2）────────────────────────────────────────
// 单轮「等模型」异常久 = 推理在绕圈（实测一轮能到 60s、流式吐 55KB）。处置与
// 「只想不干」一致：关掉接下来 N 轮思考，逼它直接出手。
/// 单轮**当场**处置的门槛：实测一轮就能 60s，「连续 2 轮」意味着先白等 120s。
constexpr unsigned long long kAiVerySlowRoundMs = 30000;
/// 「连续两轮偏慢」的门槛。
constexpr unsigned long long kAiSlowRoundMs = 12000;
/// 触发后关几轮思考。
constexpr int kAiThinkingSuppressRounds = 2;

/// 一次判决。⚠ `newStreak` **必须由调用方保存到能跨轮存活的地方** ——
/// 这条闸原来的状态是 `SendMessage` 的局部变量，而 AI 动作执行每外层轮重进一次
/// `SendMessage`，计数器每轮归零 ⇒「连续 2 轮」永远不成立 ⇒ 闸门形同不存在。
/// 把「跨轮状态」变成**显式入参/出参**，这条契约才可能被自检钉住。
struct AiSlowRoundVerdict {
    bool suppress = false;      ///< true → 调用方执行 SuppressThinkingForNextRounds(suppressRounds)
    int newStreak = 0;          ///< 调用方要保存的新连续计数
    int suppressRounds = 0;     ///< 关几轮
    std::wstring why;           ///< 给日志的一句话
};

/// `streakBefore` = 此前连续「偏慢」的轮数（调用方持有）；
/// `thinkingEnabledThisRound` = 本轮确实开着思考（关掉思考后模型本来就慢，
///   那时再计数会变成「永久关思考」的正反馈）；`inAiActionScope` = 在 AI 动作执行内
///   （聊天助手的长思考是用户要的，不替用户省时间）；
/// `actedThisRound` = 本轮**确实产出了工具调用**（出手了）。
///
/// ★★`actedThisRound` 是这条闸的宾语所在（实测踩过）：本条要判的事情是
///   「**想很久，而且没换来任何动作**」，不是「这一轮墙钟久」。
///   墙钟只是代理量，大请求体（250~340KB 含多张图）/低带宽/冷启动都会让它变长，
///   而「等模型久」在**已经出手**的那些轮里买到的是真进展。
///   实测事故（用户日志）：某局连续两轮 11.9s / 16.1s（都在放大+读图，属于正常工作），
///   闸门照旧关掉接下来 2 轮思考；关掉思考的下一轮模型**把两轮前那个工具批次原样重放**
///   （mouseClick+moveMove+screenshot，等模型只用 1.1s），在游戏里又点了一次同一处
///   —— 引擎的「省时间」制造了一次多余点击。出手了就不该按绕圈处置。
///   ⚠ 判据的对象不是「工具调用个数」而是「有没有动作产出」：1 个也算出手。
AiSlowRoundVerdict AiDecideSlowThinkingRound(int streakBefore, unsigned long long apiMs,
    bool thinkingEnabledThisRound, bool inAiActionScope, bool actedThisRound);

// ── 工具轮的「收束」判据：产出很多却不调工具（docs §42）──────────────
// 为什么需要：一个**期待工具调用**的轮次里，模型可能把整个输出预算写成散文
// （实测 45~55s、57 895 字节，而日志里那是「回复」不是「思考」）。原来只有两条闸
// 管这件事，两条的判据却都**绑在输出的形态上**：
//   · 看门狗「只想不干」闸要求 `contentBytes == 0`
//   · `needsForceToolAfterReasoning` 要求 `msg.content.empty()`
// ⇒「说了一大堆但不调工具」这个形态**两条都看不见**，只能一路烧到 `max_tokens` 耗尽
// （`finish_reason=length`），再赌一次非流式重试。判据必须与「文本落在哪个字段」无关 ——
// 本条闸的宾语是**有没有工具调用意向**。
/// 工具轮里「毫无工具意向」的产出上限。正常工具轮的输出是工具 JSON（几百字节~几 KB）
/// 加一小段前言；12KB ≈ 4000 汉字，任何合法工具轮都到不了这里。
///
/// ⚠⚠ **这个预算的宾语是「正文（`content`）」，不是「思考」**（docs §67）。
///   起因（真机日志）：一次 8 分钟的运行里，**3 次**触发
///   `本轮没有工具调用意向却已产出 14KB（以思考为主）→ 结束流式，逼它直接出手…`，
///   紧接着 `thinking=关闭（thinking.type=disabled 已下发）` 连续 2 轮。
///   而 14~15KB 的**推理**对这个模型是**正常工作量**（它每次都在推「这游戏怎么玩/我的僵尸呢」）。
///   ⇒ 引擎在思考**说到一半**时把它掐断，再拿关掉思考的请求重试
///   ⇒ 模型只能给出又短又浅的动作（实测：`zoom` 右下角一块毫无意义的区域），
///   并在之后几轮反复重放同一个鼠标批次 —— 用户看到的「呆呆的、一次只放一只普通僵尸」。
///   ⇒ 正文与思考**分两个预算**：正文还是 12KB（§42 那 57 895 字节的散文正是它抓的），
///   思考另给一个大得多的上限，且真正管住「只想不干」的是**时间/停顿**（见 ④⑤ 两条门槛），
///   不是字节数 —— 思考字节多本来就等于模型正在干活。
constexpr size_t kAiStreamProseBudgetBytes = 12288;
/// 工具轮里**纯思考**的字节上限（防死循环的兜底）。取正文预算的 3 倍：
/// 实测该模型正常推理 14~15KB，给到 36KB 才有余量，同时仍能拦住真正的失控。
constexpr size_t kAiStreamReasoningBudgetBytes = 3 * kAiStreamProseBudgetBytes;
/// 「只想不干」的绝对上限 / 停顿上限（原闸①的两条门槛，逐字保留）。
constexpr unsigned long long kAiStreamNoToolForceMs = 90000;
constexpr long long kAiStreamNoToolPlateauIdleMs = 45000;

enum class AiStreamBrakeKind {
    None = 0,
    OutputBudget,       ///< 产出已超过 kAiStreamProseBudgetBytes 且毫无工具意向
    NoToolCallTooLong,  ///< 吐了思考却既不调工具、又拖得太久/中途停顿
};

const wchar_t* AiStreamBrakeKindName(AiStreamBrakeKind k);

/// 看门狗**只能读原子量投影**（不许碰读线程独占的 StreamAccumState，那是数据竞争），
/// 这里的字段就是那几个投影的打包 —— 判据因此能被自检逐格断言。
struct AiStreamBrakeSignals {
    bool expectTools = false;          ///< 本轮期待工具调用（工具集非空）
    bool inAiActionScope = false;      ///< 在 AI 动作执行内（聊天助手的长篇回答是用户要的）
    bool sawUsableToolCalls = false;   ///< 已拿到可用的工具调用 → 一律不拦
    bool toolAssemblyStarted = false;  ///< 模型**已经开始**吐工具调用 JSON → 一律不拦
    bool gotAnyByte = false;           ///< 收到过任何字节
    unsigned long long elapsedMs = 0;  ///< 距流式开始
    long long idleMs = 0;              ///< 距最后一个字节
    size_t reasoningBytes = 0;
    size_t contentBytes = 0;
};

struct AiStreamBrakeVerdict {
    AiStreamBrakeKind kind = AiStreamBrakeKind::None;
    size_t producedBytes = 0;       ///< 思考 + 正文总字节（进日志 —— 不让日志猜是哪一边）
    bool reasoningDominated = false;
    std::string why;
};

AiStreamBrakeVerdict AiDecideStreamBrake(const AiStreamBrakeSignals& s);

/// 收束时给日志/用户的一句话，含**实测**的「思考 X KB / 回复 Y KB」拆分。
std::wstring FormatAiStreamBrakeStatus(const AiStreamBrakeVerdict& v);

// ── 「期待工具却只拿到散文」：这段文本不是回答（docs §42）────────────
// 工具轮里没有工具调用，而且这段文本是**被截断/被看门狗收束**的（不是模型自己收尾），
// 那它就是一段没写完的独白。旧实现直接把 `assistantMsg.content` 当本轮最终回答返回
// （最坏附一句「请在设置里增大 max_tokens」）⇒ AI 动作执行这一轮等于什么都没干，
// 而用户看到的就是「卡在那里不动」。
enum class AiNoToolAction {
    AcceptAnswer = 0,  ///< 就当回答：自然收尾 / 不在动作作用域 / 纠偏次数已用完
    ForceToolCall,     ///< 注入「只准调工具」纠偏，本轮重来
};

struct AiNoToolVerdict {
    AiNoToolAction action = AiNoToolAction::AcceptAnswer;
    std::wstring why;
};

/// `mustAct` = 「本轮必须有动作」（工具集非空 **且** 在 AI 动作执行作用域内 ——
/// 聊天助手里一大段文字回答就是正确答案，不能替它动手）；
/// `endedNaturally` = `finish_reason == "stop"`（模型自己收尾 → 尊重它）；
/// `nudgesUsed` / `maxNudges` 让纠偏**有界** —— 守卫不能把模型永久锁在外面（§36.6 / §40.2）。
AiNoToolVerdict AiDecideNoToolCallAnswer(bool mustAct, bool hasToolCalls, bool producedText,
    bool endedNaturally, int nudgesUsed, int maxNudges = 2);

// ── 「网关回了空体」怎么处置 ─────────────────────────────────────────
// 官方《Thinking Mode》写明：`max_tokens` 不够时**全部预算被推理吃光** ⇒
// `content` 为空、`finish_reason=length`，并且明确说这**不是** Thinking 失败。
// 而本仓旧实现把它当**致命 API 错误**（`[错误] API 请求失败：无响应。`）⇒
// 调用方判失败 ⇒ **整个宏当场结束** —— 用户看到的是「卡在那里然后自己停了」。
// 实测事故链（「我是僵尸」那一局）：模型只想不干 → 看门狗收束 → 用**还开着思考**的
// 请求重试 → 又只产思考 → 网关连回三次空体 → 宏结束。
// ⚠ 所以这条判据的**宾语是「网关回了空体」这件事本身**，不是「模型答得好不好」。
enum class AiEmptyResponseAction {
    GiveUp = 0,      ///< 不兜：非动作作用域 / 已兜满 → 交回既有错误路径
    NudgeAndRetry,   ///< 注入「直接调工具」纠偏 + 关思考，本轮重来
};

struct AiEmptyResponseVerdict {
    AiEmptyResponseAction action = AiEmptyResponseAction::GiveUp;
    std::wstring why;
};

/// `thinkingWasOn` = 本轮**确实开着思考**（空体最可能的成因就是这个）；
/// `afterCutOrTruncation` = 本轮是被看门狗收束 / 被 `length` 截断的；
/// `recoveriesUsed` / `maxRecoveries` 让兜底**有界** —— 守卫不能把模型永久锁在外面。
AiEmptyResponseVerdict AiDecideEmptyResponseRecovery(bool mustAct, bool thinkingWasOn,
    bool afterCutOrTruncation, int recoveriesUsed, int maxRecoveries = 2);

AiConfTier AiConfidenceTier(double confidence);

// ── 统一的判断记录 ────────────────────────────────────────────────
/// 所有判断共用的返回形态：枚举 + 置信度 + 一句可进日志的理由。
/// `why` 为英文/中文短标签均可，但必须是**稳定的诊断字符串**（用户报障时要一眼看懂），
/// 不要写「条件不满足」这种等于没说的理由。
struct AiDecisionRecord {
    double confidence = 0.5;
    std::wstring why;
    /// 仅 `AiDecideVisionGate` 会置位：**这次「允许视觉」是因为没有别的路可走**
    /// （画布页 / 非浏览器前台）。调用方据此**不要**再注入「改用控件树」类提示 ——
    /// 那正是当年把游戏锁死的那条错误指引（docs §20.1）。
    /// 放在末尾是为了让前面的聚合初始化（`{conf, why}`）继续可用。
    bool visionIsOnlyWay = false;
};

/// `"<decision> verdict=<name> conf=0.90 why=<...>"` —— 统一诊断行，直接进日志。
std::wstring FormatAiDecision(const wchar_t* decisionName, const wchar_t* verdictName,
    const AiDecisionRecord& rec);

// ── 置信度分档：现在只用于**留痕**（判断日志里的 why/conf）─────────────
// ⚠ 原先这里有一条「三档路由」提示函数，把 confidence 变成「往对话里塞一句提示」的行为；
//   连同它的薄封装与三个注入点**已整体删除（批 D，docs §47）**：引擎不再往对话里注入
//   任何提示 —— 判断表只负责**如实留痕**（`NoteAiDecisionLog` + `FormatAiSignals`，
//   可离线复算）。
//   顺带消灭一整类坑：`visionIsOnlyWay` 时误注入「改用控件树」（当年把游戏锁死的那条错指引）
//   —— **不注入就不会注入错**。

// ══════════════════════════════════════════════════════════════════
// 判断一：视觉定位闸（Noul）
// 实现：AiActionUiTooBusyForVisionLocate()（macro_execute_tools.cpp）
// ⚠ **它现在只是一个留痕器**（批 D，docs §47）：判断照做、`NoteAiDecisionLog` 照记
//   （判决 + why + conf + 输入信号，可离线复算），但**不再拦任何东西** ——
//   原先「命中即拒绝 locateAndClick」的两处 `return` 已删：引擎不决定模型该用哪种感知手段。
//
// 历史坑（改动前务必读）：这条闸过去只看动态覆盖率，而游戏画面逐帧重绘必然超标 →
// 真实游戏里 locateAndClick 被 100% 硬拦，用户实测表现是「游戏没反应、一直在思考」。
//（§20.1）那次的修法是「按前台类型分流」，仍然是在替模型决定用不用视觉；
// 批 D 的结论是**干脆不拦**。
// ══════════════════════════════════════════════════════════════════

enum class AiVisionGate {
    Allow = 0,        // 判断结论：放行
    DenyBusyFallback, // 判断结论：疑似「页面在动且有控件树可退」
};

/// 宿主侧**已有的本地事实**。全部由调用方填，本模块不自己取。
struct AiDecideSignals {
    /// 最近一次 observePage 的页型："dom" / "mixed" / "canvas" / 空=未知
    std::wstring pageKind;
    /// 前台窗口类名像浏览器（Chrome_WidgetWin_* / MozillaWindowClass；#32770 不算）
    bool foregroundBrowserClass = false;
    /// 前台是「没有控件树的动态画面」（桌面游戏 / 自绘应用 / 模拟器；控制台不算）
    bool foregroundSelfDrawn = false;
    /// 前台是表格/办公软件（有 Ctrl+Home、列标、UIA 等更准的路线）
    bool foregroundSpreadsheet = false;
    /// 本次上下文已进入网页会话
    bool webSessionActive = false;
    /// 最近观察的动态覆盖比例（<0 = 还没观察过）
    double busyCoverage = -1.0;
    /// 最近一次 settle 结束时画面仍在变
    bool settleStillChanging = false;
    /// 扩展已连接且宿主注册了 DOM 钩子 —— 影响**置信度**（有没有树可退），
    /// 不单独决定判决：macOS 式「把判决交给工具可用性」会让判决表失去可读性。
    bool domHooksAvailable = false;
};

/// 输入信号的**紧凑可解析**序列化（离线影子测试用，见 docs §23）。
///
/// 为什么必须记信号而不只记判决：判决是信号的函数，只记「判了什么」无法离线复算，
/// 也就无法回答「换个阈值会不会判得更准」—— 那是影子测试唯一要回答的问题。
///
/// 格式（**稳定契约，别随意改字段名/顺序**；解析方在 tools/verify/ai_decide_shadow.py）：
///     sig page=dom br=1 self=0 sheet=0 web=0 hooks=1 busy=0.421 changing=1
/// 布尔一律输出 0/1；`busy` 固定三位小数（-1 表示还没观察过）。
/// 空格分隔的 `键=值`，末尾无空格 —— 新增字段**只能追加在末尾**。
std::wstring FormatAiSignals(const AiDecideSignals& in);

/// 视觉定位闸的判决（`record.why` 写明分流依据）。
AiVisionGate AiDecideVisionGate(const AiDecideSignals& in, AiDecisionRecord* out);

/// 判决名字（诊断行用）
const wchar_t* AiVisionGateName(AiVisionGate v);

// ══════════════════════════════════════════════════════════════════
// 判断二：游戏前台（Noul）
// 原实现：AiActionGameForegroundLikely()（macro_execute_tools.cpp）
// 语义：true = 前台是「没有控件树 / 有 DOM 但没节点」的画面，**视觉是唯一手段** →
//       每轮必须注入正向指引（locateAndClick + 键鼠、本轮至少落一个动作）
//
// 历史坑：过去只认「动态覆盖率」，开局静止画面（覆盖率 <3%）连续 4 轮没被认成游戏，
// 游戏 Skill 与「每轮至少落一个动作」的指引全没注入 → 模型从零推玩法，光思考烧几十秒。
// 现在主判据是「前台没有控件树」（ForegroundWindowLooksSelfDrawn），见 §20.1。
// ══════════════════════════════════════════════════════════════════

enum class AiGameForeground {
    NotGame = 0,      // 有控件树 / 有 DOM / 有更准路线可退
    GameByNoTree,     // 自绘画面（子窗口≈0）：游戏/模拟器，判据最硬
    GameByCanvasPage, // 画布页：DOM 没有可用节点
    GameByMotion,     // 只有「画面在持续变」这条定量证据
};

AiGameForeground AiDecideGameForeground(const AiDecideSignals& in, AiDecisionRecord* out);

const wchar_t* AiGameForegroundName(AiGameForeground v);

/// 游戏前台判断的**判决 + 依据**（一起返回，别让调用方重算判决 —— 重算时
/// 桌面状态可能已经变了，会得到与记录不一致的结论）。
struct AiGameForegroundDecision {
    AiGameForeground verdict = AiGameForeground::NotGame;
    AiDecisionRecord record;
};

/// 「游戏前台」判断的可采信门槛（settle 节拍用，见 engine_script_run.cpp）。
///
/// 为什么游戏这条不能直接用通用的 `kAiDecideConfHigh`：游戏画面覆盖率是**抖的**，
/// 只有 0.55 的「画面持续重绘」兜底行只是疑似（视频播放器也是这个特征）。
/// 用 0.55 那条去跳掉整段 settle，会在**非游戏**前台跳过必要的稳定等待；
/// 而 0.88（自绘、无控件树）与 0.90（画布页）是结构性证据，可以放心跳。
///
/// 取值 = 「不要 settle」的最低把握：就落在 GameByMotion(0.55) 与 GameByNoTree(0.88)
/// 之间。改判定表里这两行的置信度时，**必须回来重新核对这个常量**。
constexpr double kAiGameConfDecisive = 0.80;

/// 这次「是游戏前台」的判断够不够把握，可以据此跳掉 UI settle 的长等待。
/// 语义绑在判定行上（画布页 / 无控件树），不只是一个裸阈值比较。
bool AiGameForegroundDecisionIsDecisive(const AiGameForegroundDecision& d);

// ══════════════════════════════════════════════════════════════════
// 判断三：规划轮是否附观察截图（Noul）
// 原实现：ShouldAttachObserveImageToPlanner(plannerModel, haveImage)（agent_ai_actions.cpp）
// 语义：true = 本轮请求带整帧图（大 token）
//
// 与另两条不同，这条的输入是**模型能力**而不是桌面状态，所以单独一个轻量入口。
// 原实现是 `return ModelSupportsVision(model)` —— 纯布尔、零置信度；这里补上
// 判决理由，供诊断与第 2 刀的置信度门控使用。
// ══════════════════════════════════════════════════════════════════

enum class AiAttachFrame {
    NoModelCannotSee = 0,  // 模型不支持视觉 → 附了也没用（会降级成路径提示）
    NoNoImage,             // 这轮根本没图可附
    YesFrame,              // 附整帧
};

// ⚠ 第 1 刀的行为契约：`YesFrame` ⇔ 旧 `ShouldAttachObserveImageToPlanner(...) == true`，
//   即仍然**只看「模型是否支持视觉」**。「本地文字索引/布局记忆够用就别附图」属于
//   第 2 刀的省轮次改动（那需要按「目标」为键查布局记忆，本模块刻意不依赖它）。
//   第 1 刀这里只暴露判决 + 理由，不改行为。

AiAttachFrame AiDecideAttachObserveFrame(bool modelSupportsVision, bool haveImage,
    AiDecisionRecord* out = nullptr);

const wchar_t* AiAttachFrameName(AiAttachFrame v);

// ══════════════════════════════════════════════════════════════════
// 诊断出口
// ══════════════════════════════════════════════════════════════════
// 判断行必须能被用户看到 —— 这条闸曾经把游戏 100% 拦死，而用户只能看到「游戏没反应」，
// 排查全靠翻代码猜条件。形状对齐既有的 `SetOcrDiagnosticSink`（src/ocr_backend.h）：
// 引擎只往 sink 推字符串，壳/播放器决定落到哪里（WebView 日志 / player.log）。
/// 设置诊断出口（nullptr = 只留在内存环形缓冲里）
void SetAiDecisionLogSink(void (*sink)(const std::wstring& line));

/// 记录一行判断诊断：推给 sink（若已设置），并存入环形缓冲。
/// 空串忽略。**不做节流** —— 调用点已经是每动作一次的粒度。
void NoteAiDecisionLog(const std::wstring& line);

/// 取最近的判断日志（越旧越靠前；自检与「把决策依据给模型看」用）。
std::vector<std::wstring> RecentAiDecisionLogs();

/// 清空环形缓冲（每个 AI 动作开始前调用，避免跨动作串台）。
void ClearAiDecisionLogs();
