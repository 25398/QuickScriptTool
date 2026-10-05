#pragma once
// ──────────────────────────────────────────────────────────────────
// ai_locate_verify.h — 定位结果的「多候选融合 + 本地校验」
//
// 三件事（都尽量做成纯函数，便于自检）：
//  1) 多候选投票：VLM 一次给出多个候选时，做**空间聚类取簇心**，而不是平均
//     （研究结论：聚类 61.7 vs 平均 46.6 vs 随机 55.7 —— 平均比随机还差）。
//  2) UIA + 视觉融合：候选框与 UIA 控件框按 IoU 去重后**统一编号**；命中 UIA 时
//     直接用该控件的精确矩形（UFO² 的做法，IoU 阈值 0.1 起）。
//  3) 本地校验：特征密度（低特征=可疑）+ 候选一致性 + UIA 命中，
//     给出 Accept / Suspect / Refine 三档判决。**不依赖 OCR**，装了 OCR 才另加文本核对。
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>
#include <vector>

#include "ocr_result.h"

/// VLM 给出的一个候选（坐标空间由调用方统一：通常先归一成上传图像素）
struct AiVisionCandidate {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    /// 只给了中心点（无框）：聚类时用点，不参与 IoU
    bool pointOnly = false;
};

/// 已确认的 UIA 控件（屏幕坐标）
struct AiUiAnchor {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    std::wstring name;
    /// ★★控件**角色**（「按钮」/「输入框」/「滑块」…）与**动作能力动词**
    /// （`click`/`fill`/`toggle`/`select`/`slide`/`scroll`/`focus`）。
    /// 由 UIA 类型算出、如实透传给模型 —— 它回答「这是什么、支持哪一类操作」。
    ///
    /// 为什么这条值得单列一段说明（它正是「半视觉」这一轮的核心）：
    /// 此前索引里只有「编号 + 名字 + 坐标」，于是模型**看不出**某个条目是能填值的输入框、
    /// 还是需要点一下的按钮、还是一个只能切换的开关 —— 而这三者要做的事完全不同。
    /// 结果它只能先截图看一眼、或先点一下试试，一次动作 + 一次观察就白烧掉。
    /// ⚠ 只描述**能力**，不描述**做法**：这里不出现「先点再输入」这类策略句，
    ///   也不出现任何禁令（本项目总原则见 docs §48：引擎只做感知 + 执行 + 如实回执）。
    std::wstring role;
    std::wstring action;
    /// ★可读状态事实（`focused` / `value:"…"` / `range:0-100` / `toggle:on` /
    /// `state:expanded` / `v:42%` / `readonly` / `password(值不回传)`）。
    /// 与 `role/action` 同理：这是**画面上读不出来**的事实，不是建议。
    std::vector<std::wstring> state;
};

struct AiLocateFusionResult {
    bool ok = false;
    /// 选定中心（与输入同坐标空间）
    int cx = 0;
    int cy = 0;
    int boxX1 = 0;
    int boxY1 = 0;
    int boxX2 = 0;
    int boxY2 = 0;
    /// 最大簇里包含的候选数（1 = 各说各话）
    int clusterSize = 0;
    int candidateCount = 0;
    /// 由 UIA 控件确认（矩形取 UIA 的精确框）
    bool uiaConfirmed = false;
    std::wstring uiaName;
    /// 诊断短说明（进日志）
    std::wstring note;
};

/// 视觉候选 + UIA 锚点 → 聚类选点。
/// 规则（与 GUI-Actor/MVP 一致的方向：选簇，不平均）：
///  0) targetText 非空时，只考虑**名字对得上**的锚点（避免候选框碰巧压在容器/别的控件上
///     就直接采信它的矩形——单候选时尤其致命）；
///  1) 候选若与某个 UIA 锚点 IoU ≥ iouMatch，或候选中心落在锚点内 → 采用该锚点精确矩形；
///  2) 否则对候选中心做单链聚类（合并阈值 = max(24, 0.6×较短边)），取最大簇；
///  3) 无候选 → ok=false。
AiLocateFusionResult FuseLocateCandidates(const std::vector<AiVisionCandidate>& cands,
    const std::vector<AiUiAnchor>& anchors, double iouMatch = 0.5,
    const std::wstring& targetText = std::wstring());

/// 匹配档位：2 = 完全相等，1 = 双向包含且长度接近（≤40% 长度差），0 = 不匹配。
/// **不做模糊匹配**（编辑距离之类）：OCR 模糊命中很容易点到隔壁相似按钮上。
/// 用途：文字直点（索引档 vs 就地复核档的比较，见 AiOcrProbeAgreesWithIndex）。
int AiOcrLabelMatchTier(const std::wstring& lineText, const std::wstring& wantText);

/// 「就地复核」是否足以采信索引里的那次命中所给的坐标。
///
/// 背景（实测踩过，别再退回严格相等）：文字直点会**裁一小块重新 OCR** 来确认画面
/// 没变。但游戏/小字标签二次识别常被裁切与重采样**读得更差** —— 例如索引里读到
/// 「键全选」(contains 命中「一键全选」)，复核又读成「键全选」。旧实现要求复核
/// **完全相等**，于是复核永远比索引更严、永远不通过，每次点击都白落回 VLM 识图
/// （实测一整个动作轮作废：一次识图 100KB+、15~45s，而本地 OCR 只要几十毫秒）。
///
/// 采信规则（四个条件都要满足）：
///   ① 复核确实读到了**与目标仍算同一个标签**的文字（不再要求完全相等）；
///   ② 索引那次命中的档位本身够好（>= minIndexTier，默认 contains 档即可）；
///   ③ 复核给出的位置离索引位置不远（<= maxDriftPx，默认 24px）—— 复核的价值就是
///      「顺便把坐标修正得更准」，所以坐标可以微调，但不能跳到别处；
///   ④ 复核位置必须与索引**同一处**（漂移限制已覆盖）。
bool AiOcrProbeAgreesWithIndex(const std::wstring& probeText,
    const std::wstring& indexHitText, const std::wstring& wantText,
    int probeX, int probeY, int indexX, int indexY,
    int minIndexTier = 1, int maxDriftPx = 24);

/// UIA 控件名与目标描述是否算「同一个东西」（双向包含 / 最长公共子串 ≥2 / 去 UI 类型词后相等）。
/// 用途：融合时过滤掉名字不相干的锚点。
bool UiNameMatchesTarget(const std::wstring& name, const std::wstring& target);

// ── 屏幕文字索引（给模型看的「可点清单」）──────────────────────────────
/// 一行里的一段文字（按横向顺序）。
struct OcrIndexSpan {
    std::wstring text;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int centerX() const { return (x1 + x2) / 2; }
    int centerY() const { return (y1 + y2) / 2; }
};

/// 把 OCR 行表按**视觉行**聚成几组，组内按 x 升序。
///
/// 为什么需要（实测：AI 在一局 PvZ 里连点错卡片，docs §25.4）：
/// 索引原本是一串 `文字(x,y)；文字(x,y)；…` 的平铺列表，模型拿到
/// 「`50(604,188)；600(2140,443)`」这样的东西**无法把它和画面上的卡片对上**
/// —— 它不知道哪个价格属于哪张卡、卡片从左到右是第几张，于是反复猜。
/// 按行分组 + 标出视觉行序，就把「找第几张卡」从**推理**变成**查表**。
///
/// 分组规则（都是宽严明确的几何判据，别凭感觉调）：
///   · 同一视觉行：垂直重叠 ≥ 较矮者的 50%，且两段水平间距 ≤ 较高者的 `maxGapFactor` 倍；
///   · 每组最多 maxSpans 段（够描述一条卡槽/工具栏即可，防整屏并成一行）。
///
/// ⚠⚠ `maxGapFactor`（默认 2）是**给模型看的索引**的口径：间距太远说明是两个不相干的区域。
///   但**版面推断（`PairCaptionRowWithIconBand`）不能吃这道闸** —— 实测代价（真机日志）：
///   一排**卡片宽、价签小**的卡槽（卡距 ≈43 upload px、价签字高 ≈10 upload px）里
///   `间距/字高 ≈ 4.3 > 2` ⇒ **每个价签各自成行** ⇒ `spans.size() >= 3` 永不成立 ⇒
///   配对一条不发布（`标签→图标槽推断 0 条`），而且连"未配对"的诊断都不会打
///   （那段诊断只在 spans≥3 时才走）。吃这道闸的后果是模型退回去**逐张放大手量像素**。
///   ⇒ 要推断版面就显式传一个大值：行内**等距/等高**由下游判据负责，不靠间距闸。
/// ⚠⚠ `maxNumericSpans` 是**给模型看的索引**的容量闸（默认 `kOcrIndexMaxNumericSpans`：
///   满屏价签会把「一键全选/确认」这类文字按钮挤掉，所以纯数字限量、文字优先）。
///   但**版面推断（见 PairCaptionRowWithIconBand）不能吃这道闸** —— 实测代价：
///   一个 8×10 的选卡网格有 ~50 个价签，限量后每行只剩 <3 段 ⇒ 配对**一条都不发布**，
///   而且日志里连一行原因都没有（`0 条` 就是这么来的）。要全量就显式传一个大值。
std::vector<std::vector<OcrIndexSpan>> CollectOcrIndexRows(
    const std::vector<OcrTextLine>& lines, size_t maxSpans = 12,
    int maxNumericSpans = -1,   ///< -1 = 用默认闸 `kOcrIndexMaxNumericSpans`
    int maxGapFactor = 2);      ///< 行内相邻两段的间距上限 = 较高者字高 × 本值

/// 生成给模型看的文字索引文本（含标题与坐标口径说明）。
/// `capX1/capY1/capX2/capY2` = 本次观察帧对应的屏幕区域（用于换算出**画面高度占比**，
/// 让模型知道某行在画面里靠上还是靠下 —— 只有绝对像素它推算不出这一点）。
/// `frameW/frameH` = 这一帧的 **upload 截图像素**尺寸（= 模型收到的那张图的大小）。
///   ★**必须给**：索引给出来的就是**可点坐标**，而 `mouseClick(x,y)` 收的正是这一套
///   ⇒ 两者必须同一个口径，且帧尺寸只该有**一个**来源（调用方与元素索引共用同一个值）。
///   给 0 时退回屏幕绝对像素，并在标题里**如实说明「这不是 mouseClick 那一套」** ——
///   ⚠ 绝不许像旧版那样发着屏幕像素、标题却写「与 mouseClick 同一套」：
///   2560×1440 截成 1024×576 时差 2.5 倍，模型照它点必然落空（docs §33.1/§60.5）。
/// 返回空串表示没有可用条目（调用方据此不要注入空索引）。
std::wstring FormatOcrTextIndex(const std::vector<OcrTextLine>& lines,
    int capX1, int capY1, int capX2, int capY2,
    int frameW = 0, int frameH = 0, int* outItemCount = nullptr);

/// ★★帧间**屏幕文字**差分（纯函数，可逐格自检）——「我的动作到底有没有发生」的硬证据。
///
/// 起因（落盘日志）：模型放了一个僵尸，本地只测得出「落点附近有变化」，**测不出放了什么、
/// 有没有放** ⇒ 它连着 5 轮截图 + zoom 去找那个僵尸，最后跑去搜网页，再没放第二个。
/// 而**答案就在引擎自己手里**：那一帧的 OCR 读到 `29900`，上一帧是 `30000` ——
/// 「屏幕上有个数字从 30000 变成 29900」就是"确实发生了一次操作"的硬证据。
///
/// 通用性：任何界面的可见状态变化都会在这里现形 —— 游戏计数器、对话框文字、单元格值、
/// 列表条数、错误提示。不需要任何领域知识，也**不多花一次识图或一次 API**：OCR 本来每帧都在跑。
/// （对标开源 GUI agent 的 state-diff 思路：那边比的是 a11y 树；游戏/画布类界面没有 a11y 树，
/// OCR 是同一角色的一等替代。）
///
/// 只比**多重集**（出现 / 消失），不按位置配对整行：OCR 的框每帧都在抖，按位置配对全是噪声。
/// 唯一的例外是**纯数字**：按最近的 x 位置配对成「数值 30000 → 29900」—— 计数器变化是最要紧
/// 也最常见的一种，而数字自带对齐语义。返回空串 = 没有值得报的变化。
std::wstring AiDescribeTextIndexDelta(const std::vector<OcrTextLine>& prev,
    const std::vector<OcrTextLine>& cur, size_t maxItems = 6);

/// 本地校验输入（不依赖 OCR）
struct AiLocateVerifyInput {
    int boxW = 0;
    int boxH = 0;
    /// 最大簇候选数 / 候选总数（1.0 = 全部一致；单候选记 1.0）
    double clusterAgreement = 1.0;
    bool uiaConfirmed = false;
    /// 框内裁图特征密度过低（纯色/空白区）——最典型的「框对了位置但没框到东西」
    bool lowFeature = false;
    /// UIA 点探测：该点确实是可交互控件
    bool pointOnInteractiveControl = false;
};

enum class AiLocateVerdict {
    Accept = 0,   // 直接可用
    Suspect,      // 可用但要提示模型「可能不准」
    Refine,       // 需要再放大精炼一轮
};

/// 判决：UIA 确认 → Accept；一致性好且非低特征 → Accept；
/// 低特征或候选分歧 → Refine；只有单候选且无其他证据 → Suspect。
AiLocateVerdict JudgeLocateConfidence(const AiLocateVerifyInput& in,
    std::wstring* outWhy = nullptr);

const wchar_t* AiLocateVerdictName(AiLocateVerdict v);

/// 把模型回复按行拆成候选文本（每行一个框/点），最多 maxLines 行。
std::vector<std::wstring> SplitVisionCandidateLines(const std::wstring& text,
    size_t maxLines = 3);

/// 框内裁图是否「低特征」（灰度方差低于阈值；纯色/空白区）。
/// 用于：可疑点提示 + 拒绝给这种点存定位模板（存了下次也会匹配到别处）。
bool BitmapRegionLooksLowFeature(HBITMAP bmp, double* outStdDev = nullptr,
    double minStdDev = 6.0);

// ── OCR 文本核对（可选：用户装了文字识别引擎才走）────────────────────
// 视觉定位的最后一层独立证据：在定位点附近裁一块做 OCR，看能不能读到目标文字。
//  · 没装引擎 / 识别失败 → checked=false，**什么都不改**（静默跳过，绝不因此失败）
//  · 读到目标文字 → checked=true, found=true（可把「可疑」升级为「可用」）
//  · 读到了别的文字但没读到目标 → checked=true, found=false（提示疑似未命中）
// 目标描述不是「纯短文本」时不做核对（颜色/方位/图标/序号类目标，OCR 对不上号）。

/// 从 locateAndClick 的目标描述里提取「要核对的屏幕文字」。
/// 例：「保存按钮」→「保存」；「点击登录」→「登录」。
/// 颜色/方位/图标/序号/长句/带标点 → 返回 false（不做 OCR 核对）。
bool AiLocateExtractOcrTarget(const std::wstring& targetDesc, std::wstring* textToFind);

struct AiLocateOcrOutcome {
    /// 真的做了核对（引擎可用且识别有结果）
    bool checked = false;
    /// 在区域内读到了目标文字
    bool found = false;
    /// 建议使用的点（found 且 moved 时有效）
    int cx = 0;
    int cy = 0;
    /// OCR 行中心相对原定位点的偏移
    int dx = 0;
    int dy = 0;
    /// 偏移够大 → 按 OCR 识别框修正定位点
    bool moved = false;
    std::wstring note;
};

/// OCR 核对判决（纯函数）。lineX1..lineY2 是命中的 OCR 行框（屏幕坐标）。
AiLocateOcrOutcome JudgeLocateOcrText(bool ocrUsable, bool found,
    int lineX1, int lineY1, int lineX2, int lineY2,
    int clickX, int clickY, int moveThresholdPx = 12);

// ── 「文字直点」：本地 OCR 索引 → 直接点击（省掉 DOM/UIA/两轮识图）──────
// 带文字标签的按钮/菜单项是**一次性**操作，不值得走三级定位阶梯：
// 实测一次「点个按钮」= 整屏识图 + Zoom 精炼两轮 VLM ≈ 20s，而观察帧里的本地 OCR
// 早就给出了「这段文字在屏幕哪个坐标」。命中唯一就直点，0 次 API。
//  · 目标不是「纯短文本」（图标/方位/序号/格子/长句）→ 调用方先用
//    AiLocateExtractOcrTarget 过滤，本函数只负责挑点。
//  · 同屏出现多个同名候选（两个「确定」）→ 歧义，拒绝（宁可回落识图）。
//  · 命中行框大到像「整块面板被并成一行」→ 拒绝（点中心会打到别的东西）。
struct AiOcrDirectHit {
    int screenX = 0;
    int screenY = 0;
    /// 命中文字框尺寸（屏幕像素）：调用方据此决定「就地复核」要裁多大
    int boxW = 0;
    int boxH = 0;
    std::wstring hitText;
    /// 与目标的匹配档位：exact=完全相等；contains=包含；fuzzy=模糊
    std::wstring matchKind;
    /// 未命中/被拒的原因（诊断日志用，成功时为空）
    std::wstring why;
};

/// 在 OCR 行里挑「可以直点的目标文字」。返回 true 时 out 填好屏幕坐标。
/// **要求唯一命中**（同屏多个同名候选 = 歧义 → 拒绝），用于「没识图、只靠文字」的快路径。
bool AiOcrPickDirectClickTarget(const std::vector<OcrTextLine>& lines,
    const std::wstring& wantText, int screenW, int screenH,
    AiOcrDirectHit* out);

/// 同上但**允许同屏多个候选**：取离 (nearX,nearY) 最近的那个（识图点通常已很接近目标，
/// 「最近的同名文字」比「全局唯一」实用得多）。maxDistPx > 0 时限制搜索半径。
/// 用于识图之后的**文字坐标覆盖**：本地 OCR 读到文字像素，比 VLM 的粗框准。
bool AiOcrPickNearestText(const std::vector<OcrTextLine>& lines,
    const std::wstring& wantText, int nearX, int nearY, int maxDistPx,
    int screenW, int screenH, AiOcrDirectHit* out);

// ── 统一元素索引：把「看得见的可点东西」编成一张表（所见即所得）──────────
//
// 由来（用户提问）：「找图-定位-点击这套流程能不能合并？让 AI 做到所见即所得，
// 用工具函数直接操作定位电脑上的界面，而不是用图片观察界面、再用图片定位到界面？」
//
// 现状是**两套不相干的感知**：UIA 控件表给 `invokeUiControl` 用（编号只在那一次调用里有效），
// OCR 文字索引给模型看（另一套坐标），而 `locateAndClick(target=文字)` 走的是**第三条**
// 独立阶梯（DOM → UIA → OCR → VLM），三套各自重新枚举、各自打分。代价不只是慢：
// 实测模型要关游戏内的卡牌面板，目标写「关闭」，UIA 阶梯把点定到了**窗口自己的「关闭」按钮**，
// 一击把整个游戏关掉（进度丢失，之后十几轮都在找游戏怎么重开）。
//
// 对齐开源实现的做法（UFO/UFO² 的 `merge_target_info_list` + OmniParser SoM、
// WindowsAgentArena/Navi 的元素 id 动作空间）：**一次枚举 → 一张带 id 的表 →
// 模型按 id 说话，宿主按 id 执行**。区别只在于我们把 id 也喂给 `locateAndClick`，
// 于是「按文字定位」不再需要 VLM —— 索引里有就直接解析出坐标，没有才回落识图。
enum class AiElementSource {
    UiAutomation = 0,   ///< UIA 控件（有精确矩形，最可信）
    OcrText,            ///< 本地 OCR 读到的文字行
    /// ★「短标签 → 它标注的图标槽」（引擎按版面推断，见 PairCaptionRowWithIconBand）。
    /// 由来（实测那一局）：界面上只有价格数字进了索引，模型想点「600 的那张卡」却只知道
    /// 「600」这几个字在哪 —— 它不知道**字正上方就是那张卡**，于是 8 轮里 6 轮在 zoom 猜卡，
    /// 最后只会点它唯一有坐标的那张（最便宜的 50）。
    LabeledIcon,
    Vision,             ///< VLM 识图结果（兜底，索引里没有时才有）
};

const wchar_t* AiElementSourceName(AiElementSource s);

/// 索引里的一条「看得见、点得到」的元素
struct AiElementEntry {    /// 1 起的编号：**模型说话用的唯一 id**（跨轮不稳定，只在本帧内有效）
    int id = 0;
    AiElementSource source = AiElementSource::OcrText;
    /// 显示名（UIA 控件名 / OCR 文字 / 目标描述）
    std::wstring name;
    /// 屏幕绝对像素矩形（与 mouseClick / OCR 索引同一口径）
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool enabled = true;
    /// 支持 UIA InvokePattern：可不打像素直接触发
    bool invokable = false;
    /// ★控件角色（「按钮」/「输入框」/「滑块」…；OCR/推断条目为空）
    std::wstring role;
    /// ★动作能力动词（`click`/`fill`/`toggle`/`select`/`slide`/`scroll`/`focus`）。
    /// 对 UIA 条目由控件类型算出；对 OCR/推断条目为空（**不猜**）——
    /// 读出来的一段文字到底能不能点，本地没有证据，宁可不给也不给错。
    std::wstring action;
    /// ★可读状态事实（`focused` / `value:"…"` / `range:0-100` / `toggle:on` / …）。
    /// 见 `AiUiAnchor::state` 的理由；同样只对 UIA 条目有值。
    std::vector<std::wstring> state;
    /// ★窗口自身标题栏按钮（关/最小化/最大化）。**绝不参与按名字解析** ——
    /// 它的名字（「关闭」）和应用内按钮完全一样，实测就是这样关掉整个游戏的。
    bool windowChrome = false;
    /// 该元素所在窗口的标题（多窗口/弹窗时让模型知道点的是哪一个）
    std::wstring windowTitle;
    /// 这条**是怎么来的**（引擎推断的依据 / 标签在哪儿）。空则不打印。
    /// ★存在的理由：`LabeledIcon` 是**推断**出来的坐标（不是读出来的），推断类事实必须
    /// 连同证据一起给 —— 模型才有机会自己判断该不该信（照 UFO²/OmniParser 的做法，
    /// 它们也把 icon 的框与它旁边的文本一起发布，而不是只发一个坐标）。
    std::wstring note;
    int centerX() const { return (x1 + x2) / 2; }
    int centerY() const { return (y1 + y2) / 2; }
    int width() const { return x2 - x1; }
    int height() const { return y2 - y1; }
    bool valid() const { return x2 > x1 && y2 > y1; }
};

/// 索引里的一条 OCR 行（同一视觉行的多段已拼接，见 CollectOcrIndexRows）
struct AiIndexRowInput {
    std::wstring text;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

/// ★★按**编号**在索引里直查一条（纯函数，自检可逐格断言）。
///
/// 起因（一次只读审计实测出来的缺口）：索引早就给模型编号与坐标了，但**没有任何工具
/// 吃得下编号** —— 模型「看得见编号、用不上编号」，只能退回写**名字**去匹配；
/// 而同屏多个同名条目（「确定」「600」）按名字解析会判歧义，于是直接回落一整轮 VLM 识图
/// （一次识图 100KB+/15~45s）。编号路径让「第 3 条」变成确定命中。
///
/// ⚠ 编号**只在本帧有效**（`AiElementEntry::id` 是每帧重建的阅读顺序号）：
///   查不到就返回 nullptr，调用方**必须如实回执**，绝不能退化成「按名字猜」——
///   猜错是点到别处，比回落识图糟得多。
/// ⚠ 窗口自身按钮（`windowChrome`）**永不入选**：它的名字（「关闭」）和应用内按钮完全一样，
///   实测模型想关游戏内面板却把整个游戏窗口关掉了（docs §31.1）。`enabled=false` 同理。
/// ⚠ 这两个拒绝**不给「近似编号」兜底**（id±1 之类）：编号错位必然点到隔壁条目。
const AiElementEntry* AiElementIndexById(const std::vector<AiElementEntry>& items, int id);

/// 把 UIA 控件与 OCR 行**合并成一张编号索引**。
///
/// 合并判据（有意保守，宁可两条也不要错并）：
///   · 同一处（矩形 IoU ≥ dedupIou **或** 一方中心落在另一方内）且**名字对得上**
///     （`UiNameMatchesTarget` 双向）→ 合成一条，保留 UIA 的矩形与能力位
///     （UIA 矩形是控件真框，OCR 框只是文字框），来源记 UiAutomation；
///   · 其余各成一条：UIA 在前（精确、可 Invoke），OCR 在后。
/// `maxItems` 上限按「给模型看的清单」控制（默认 80）。
/// 编号在**排序后**统一分配（上→下、左→右的阅读顺序），保证 id 与视觉扫描一致。
std::vector<AiElementEntry> BuildAiElementIndex(
    const std::vector<AiUiAnchor>& uiControls,
    const std::vector<AiIndexRowInput>& ocrRows,
    size_t maxItems = 80, double dedupIou = 0.35);

// ── ★「短标签 → 它标注的图标槽」：一条**通用**版面推断（可逐格自检）──────────
//
// 要解决的问题（实测那一局）：`植物大战僵尸融合版` 的卡槽里，OCR 只能读到价格数字
// （「50/75/100/600」），**读不到卡片图标是什么**。模型在索引里查到了「600」的坐标，
// 却不知道那只是**卡下方的价签**；它花 8 轮反复 `zoom` 猜「哪张卡是 600 的」，
// 最后只会点它唯一有把握的那张（最便宜的 50）—— 用户看到的就是「只会放普通僵尸」。
//
// 做法对齐开源结构化解析：OmniParser（微软，MIT）把界面拆成 **icon 框 + 文本**两部分再
// 按邻近关系配对；UFO/UFO² 在有控件树时用 `LabeledBy` 直接拿这层关系，没有控件树时退化到
// 几何配对；PaddleOCR/tesseract 的版面分析用**投影剖面**切出成排的块。这里用同一条思路的
// 最小实现：**从标记行往上取一条横带，按「列与同一行中位亮度的偏离」找等距块**，
// 全部判据都在纯函数里，不够整齐就**什么都不发布**（宁可没有，也不给错坐标）。
//
// ⚠ 它是**推断**，不是读出来的 ⇒ 发布时必须连证据一起给（`AiElementEntry::note`）。

/// 一条横带的**原始亮度**（屏幕坐标；由 `SampleIconBandFromBitmap` 从位图拷出来）。
/// ⚠ 这里**故意只装像素、不做分析**：所有判据都放进纯函数
/// （`PairCaptionRowWithIconBand`），于是自检能直接喂一段合成像素、逐格断言。
/// 第一版把「列墨迹剖面」预先算在这里，用的是「与同一行中位数亮度比」——
/// 实测它**会翻极性**：图标占了条带一半以上宽度时，中位数就变成图标自己的颜色，
/// 于是被检出的「块」变成了**缝**。判据放错层的代价就是这个。
struct AiIconBand {
    int x0 = 0;      ///< 第 0 列的屏幕 x
    int y0 = 0;      ///< 条带上沿的屏幕 y
    int width = 0;   ///< 列数
    int height = 0;  ///< 行数
    /// 灰度亮度，行优先：`lum[y * width + x]`（长度 = width * height）
    std::vector<uint8_t> lum;
    uint8_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        return lum[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
    }
    bool valid() const {
        return width > 0 && height > 0
            && lum.size() >= static_cast<size_t>(width) * static_cast<size_t>(height);
    }
};

/// 标签 ↔ 图标槽 的配对结果（坐标一律**屏幕绝对像素**，与 AiElementEntry 同一套）
struct AiLabelIconPair {
    AiIndexRowInput label;          ///< 标签原样（文字 + 它自己的框）
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;   ///< 推断出的图标槽
    int slotCount = 0;              ///< 本行检出几个槽（= 标签数，判据要求相等）
    int labelCount = 0;
    int iconHeight = 0;             ///< 槽高（像素）——比标签文字高才认
};

/// 从位图拷一条横带的亮度出来（**只拷像素，不做判断**）。`capX1/capY1` = 位图左上角
/// 对应的屏幕坐标（位图是捕获区域的**原生分辨率**副本，而 OCR 坐标是屏幕坐标 ⇒ 平移一次）。
/// 返回 false = 位图读不出来（调用方**如实跳过**这条推断，别猜）。
bool SampleIconBandFromBitmap(HBITMAP bmp, int capX1, int capY1,
    int sx1, int sy1, int sx2, int sy2, AiIconBand* out);

/// 纯函数：把「一行短标签 + 它正上方的横带」配成 标签→图标槽。
///
/// ★思路是**假设检验**，不是无监督分割：先假设「每段标签的正上方有一个图标、
///   标签与标签之间的正上方是背景」，再拿像素验下面四条 —— 全过才发布。
///   （第一版做的是「按行中位数找偏离列」：图标占条带一半以上宽度时中位数就变成图标
///   自己的颜色 ⇒ **极性翻转**、检出的"块"其实是缝。判据放错层的代价就是这个。）
///   ① 标签 ≥ 3 段、每段 ≤ 6 字、间距均匀（**中位间距 ±35% 内的最长等距段**，段内变异系数 ≤ 0.35）；
///      ⚠ 判据的宾语是「这一排标签是否落在同一个等距栅格上」，不是「行里每个间距都一样」——
///      实测顶栏价签本身等距，但同一视觉行混着太阳计数器（`30000`）⇒ 整行变异系数 0.36，
///      只比门槛高 0.01 就让**整排作废** ⇒ 模型拿不到卡坐标、反复 zoom（docs §63）。
///      行里混进一个**不同来源**的标签不该让整排作废；但**参与段仍须 ≥3 段**（只放宽"谁参与"）。
///   ② 背景由**标签之间**的窗口按行取中位亮度得出（这是本推断的假设，用它自己定背景才自洽）；
///   ③ 每段标签正上方确有东西：中心窗口的列偏离量中位数 ≥ max(3, 条带高/4)；
///   ④ 每块横向的**范围**：先按「相邻两标签的中点」划出这一格（相邻格不重叠），
///      再在格内取「脱离背景」的连续列 ⇒ 紧凑网格（卡片之间只隔一条深色描边）也能切对；
///      只有**顶到条带外沿**才算被切（格边界顶到不算，那正是"这一格被填满"）；
///   ⑤ 块宽 ≥ 0.3 × 标签间距；⑥ 块高 ≥ 1.6 × 标签文字高
///      （**上面那行是文字的话只有一行那么高，会被这条挡掉**）。
/// ⚠ 调用方给的条带**左右各留够一格**（否则最边上的卡片会被切）；网格界面还要保证
///   条带上沿**让开上一行标签**（见 `CollectOcrIndexRows` 的 `maxNumericSpans` 说明）。
std::vector<AiLabelIconPair> PairCaptionRowWithIconBand(
    const std::vector<AiIndexRowInput>& labels, const AiIconBand& band, std::wstring* why);

/// 纯函数：条带被拒时**如实说清是哪一条判据失败的** —— 诊断也是一份回执，不许说谎。
///
/// ⚠ 出过一次真事故：调用点把「上一行标签压过来」和「条带在本帧捕获区之外」**两种原因
///   写了一句话**（`条带出界（y …，捕获区 …）`）⇒ 排查时被指向「捕获区域配错了」这个
///   **错误方向**，而真实原因是网格里上一行价签顶到了本行条带。判据的宾语必须是
///   **当下真正失败的那一条**，所以原因文案从谓词本身算出来，而不是从现场的几个数字猜。
///
/// 入参就是调用点手里那几个数：条带申请到的上下沿 `bandY1/bandY2`、上一行标签底边
/// `prevRowBottom`（`INT_MIN` = 这是第一行）、本帧捕获区上沿 `capY1`、申请条带高 `bandH`。
/// **返回空 = 不该拒**（判据与调用点逐字相同：`bandY1 < capY1 || bandY2 <= bandY1`）；
/// 返回非空 = 该拒，且这就是原因。三种原因：①上一行标签压没 ②让开上一行后顶出捕获区
/// ③本行标签太靠上（离上沿不足一条条带高）。
std::wstring AiExplainIconBandSkip(int bandY1, int bandY2, int prevRowBottom,
    int capY1, int bandH);

/// 同 `BuildAiElementIndex`，另把 `iconSlots`（标签→图标槽）作为 `LabeledIcon` 条目并入，
/// 并**剔除已被配对的那几条 OCR 文字**（否则同一句话会同时是「文字」和「图标槽」两条，
/// `locateAndClick("600")` 命中两条同档 ⇒ 判歧义拒绝，等于白做）。
std::vector<AiElementEntry> BuildAiElementIndexWithIcons(
    const std::vector<AiUiAnchor>& uiControls,
    const std::vector<AiIndexRowInput>& ocrRows,
    const std::vector<AiLabelIconPair>& iconSlots,
    size_t maxItems = 80, double dedupIou = 0.35);

/// 给模型看的紧凑清单。
///
/// ★★坐标口径必须与 `mouseClick` **完全一致**：那是「当前 upload 截图像素」，
/// 不是屏幕像素，也不是 0~1000 归一化。这里踩过一个大坑 ——
/// 第一版把**屏幕像素**写进清单，还注了一句「与 mouseClick 同一套」，
/// 于是在 2560×1440 屏幕上截成 1024×576 的帧里，模型拿屏幕坐标去 mouseClick
/// 会**整体偏移 2.5 倍**。更要命的是它察觉了矛盾，然后花了 10~40KB 的思考反复
/// 推敲「这到底是屏幕坐标还是图像坐标」，一轮就烧掉 40 多秒（实测日志）。
///
/// 所以：调用方给捕获区域与帧尺寸，本函数**自己换算成 upload 像素**，
/// 并在标题里写明「这就是 mouseClick 要的那个像素」。
/// `mapCapX1/mapCapY1/mapCapX2/mapCapY2` + `apiW/apiH` 与 `MapApiPointToScreen` 同一套映射。
std::wstring FormatAiElementIndex(const std::vector<AiElementEntry>& items,
    int mapCapX1, int mapCapY1, int mapCapX2, int mapCapY2, int apiW, int apiH,
    size_t maxChars = 2600);

/// 宽容档匹配（CJK 短标签丢字/并字）：OCR 对中文小字常少读一个字
/// （「自选僵尸卡牌」读成「自选僵尸卡」），而 `AiOcrLabelMatchTier` 的长度比闸
/// 在**更短的目标**上会把它挡掉 —— 实测这条让模型在索引里查不到自己刚看到的文字，
/// 于是反复识图 + 反复推敲，一轮烧掉 40 多秒。
///
/// 只在两边都是「短 CJK 标签」（3~12 字，无空格标点）时启用；返回 1 = 宽容命中。
/// 档位**低于**精确/包含档：`AiElementIndexResolve` 里有精确命中时直接丢弃宽容候选，
/// 所以不会出现「模糊的抢走精确的」。
int AiElementIndexPartialMatch(const std::wstring& candidateName,
    const std::wstring& targetDesc);

/// 按模型给的目标描述**从索引里解析坐标**（0 次 VLM）。
struct AiIndexResolveResult {
    bool ok = false;
    int x = 0, y = 0;
    /// 命中的编号 / 名字 / 来源（回执里要说清「凭哪一条点的」）
    int id = 0;
    std::wstring name;
    AiElementSource source = AiElementSource::OcrText;
    /// 匹配档位：2 完全相等，1 双向包含（与 AiOcrLabelMatchTier 同尺）
    int tier = 0;
    /// 歧义：同名多项且无法用 `near` 区分 → 拒绝（宁可回落识图，也不猜着点）
    bool ambiguous = false;
    /// 未命中/被拒的原因（诊断用）
    std::wstring why;
};

/// 在索引里找 `targetDesc` 指向的元素。
///
/// 规则：
///   ① **窗口自身标题栏按钮永不入选**（`windowChrome`）——「关闭」这一类名字
///      和应用内按钮字面相同，靠名字挑必然出事（实测关掉了整个游戏窗口）；
///   ② 只考虑 `enabled` 的条目（灰控件点了也没用）；
///   ③ 档位：完全相等 > 双向包含（`AiOcrLabelMatchTier`）；
///   ④ 同档多条 → 用 `nearX/nearY`（**可选**，模型给了上下文就用）取最近那条；
///      没有 near 或仍然分不出 → `ambiguous=true` 拒绝；
///   ⑤ 纯数字/过短目标（≤1 字）**只允许精确档**（不许宽容档）—— 表格单元格、
///      卡片角标的价格本来就是纯数字，一律拒绝会让「按价格/编号选」不可用；
///      但满屏都是这种短标签，模糊匹配必然点错，所以按档位收紧而不是一刀切拒绝。
bool AiElementIndexResolve(const std::vector<AiElementEntry>& items,
    const std::wstring& targetDesc, bool hasNear, int nearX, int nearY,
    AiIndexResolveResult* out);
