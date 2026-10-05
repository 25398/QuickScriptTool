#pragma once
// ──────────────────────────────────────────────────────────────────
// ai_action_router.h — AI 动作执行混合路由（方案 E）
// ──────────────────────────────────────────────────────────────────

#include <string>

enum class AiActionRouteKind {
    ToolExecute,       // 纯动作 / 文本：tools 单轮（AgentCore 内多轮 tool-call）
    VisionQuery,       // 识图问答：无 tools，直接文本
    CompositeClick,    // 简单「点击…」：宿主本地调用 locateAndClick 管线（默认 1 次识图+本地点击）
    MultiTurnTools,    // 复杂组合：tools + 可选共享上下文、多轮 user 消息
};

/// 截图区域与上传图之间的坐标映射
struct AiCaptureMapping {
    int capX1 = 0;
    int capY1 = 0;
    int capX2 = 0;
    int capY2 = 0;
    int srcWidth = 0;
    int srcHeight = 0;
    int apiWidth = 0;
    int apiHeight = 0;
};

std::wstring AiActionRouteLabel(AiActionRouteKind kind);

AiActionRouteKind ClassifyAiActionRoute(const std::wstring& prompt, bool withImage);

bool IsAiActionVisionQueryPrompt(const std::wstring& prompt);

bool IsAiActionCompositeClickPrompt(const std::wstring& prompt);

/// 「回答/回复对方消息」类：应走工具链输入+发送，勿当多步组合
bool IsAiActionReplyMessagePrompt(const std::wstring& prompt);

bool IsAiActionComplexCompositePrompt(const std::wstring& prompt);

/// 未勾选「带截图」时：任务是否仍明显依赖所见画面（点击/定位等）。
/// 纯变量分析+按键等应返回 false，避免无谓全屏截图开销。
bool AiActionPromptLikelyNeedsScreenCapture(const std::wstring& prompt);

/// ★「打开目标」解析：完整网址 / 裸域名（补 https://）/ 常见站点名（哔哩哔哩、百度…）
///   / 浏览器内置页 ⇒ 可直接打开的 URL；**解析不出来返回空**（调用方如实报错，不猜）。
std::wstring ResolveOpenWebpageTargetUrl(const std::wstring& raw);
std::wstring OpenWebpageTargetHint();

/// 从 AI 文本中解析 (x,y) / x,y
bool TryParseCoordinatePair(const std::wstring& text, int& outX, int& outY);

/// 从 AI 文本解析矩形 [x1,y1,x2,y2] / (x1,y1)-(x2,y2) / x1,y1,x2,y2
bool TryParseBoundingBox(const std::wstring& text, int& outX1, int& outY1, int& outX2, int& outY2);

/// 识图明确表示目标不在画面上（NOT_FOUND / 找不到 / 未检测到…）
bool IsVisionLocateNotFound(const std::wstring& text);

/// 从「点击/双击/右键点击…目标」类 prompt 中提取定位目标短语：
/// 剥动作前缀（点击/请帮我…）并截断到第一个逗号（「点击搜索图标，再输入…」→「搜索图标」）。
/// 本地 CompositeClick 快路径把该短语直接作为 locateAndClick 的 target，避免整段任务原文
/// 塞进识图 prompt 烧 token / 让模型误认多个目标。
std::wstring ExtractClickTargetPhrase(const std::wstring& prompt);

/// prompt 是否含「双击」意图（如「双击桌面图标」；打开图标/文件必须双击）
bool PromptIntendsDoubleClick(const std::wstring& prompt);

/// prompt 是否含「右键」意图（如「右键点击图标」；出上下文菜单用右键）
bool PromptIntendsRightClick(const std::wstring& prompt);

/// 粗框相对上传图是否过大（疑似乱框整屏/整列），areaRatio 默认 0.22；另拦过高/过宽单边
bool IsVisionApiBoxTooLarge(int apiX1, int apiY1, int apiX2, int apiY2,
    int apiW, int apiH, double maxAreaRatio = 0.22);

/// 识图回答给出了什么：一个框 / 一个点 / **什么都没给**。
enum class AiVisionAnswerKind {
    NoAnswer = 0,  ///< 解析不出位置，或给出的是**退化回答**（空框）
    Box,           ///< 有效矩形
    Point,         ///< 有效单点
};

/// 解析识图定位回答，并把「**模型其实没回答**」这件事显式说出来（docs §38.4）。
///
/// 由来（实测一局游戏日志）：模型对「左侧剩余植物」回了 `[0,0,0,0]`。
/// `TryParseBoundingBox` 要求 `x2>x1 && y2>y1`，所以它对空框**返回 false** ——
/// 调用方于是退回 `TryParseCoordinatePair`，从同一串里抓到了 `(0,0)`，
/// 把「一个空框」当成了「图左上角这个点」，然后：
///   ① 点屏幕 **(0,0)**（真正的桌面左上角）；
///   ② 因为落点局部「变了」而回执 **「[结果] 界面已经变化 → 这一步已经生效」**；
///   ③ 把这块垃圾存成定位模板并起了跟踪会话。
/// 一次幻觉 = 一次错点 + 一个**假成功**回执，比直接失败糟得多。
///
/// 判据：**括号里有 4 个数、但撑不出面积**（`x2<=x1` 或 `y2<=y1`）⇒ 退化回答。
/// 只有 2 个数才是「单点」，所以 `(500, 95)` 这类正常精点回答不受影响。
AiVisionAnswerKind ParseVisionLocateAnswer(
    const std::wstring& text, int& outX1, int& outY1, int& outX2, int& outY2);

void MapApiPointToScreen(const AiCaptureMapping& map, int apiX, int apiY, int& screenX, int& screenY);

/// 屏幕绝对像素 → **upload 截图像素**（`MapApiPointToScreen` 的逆运算）。
///
/// 为什么要有这个函数：给模型看的坐标**一律是 upload 像素**（`mouseClick`/
/// `locateAndClick` 要的是它，元素索引/文字索引给的也是它）。任何「本地拿到的是屏幕像素、
/// 却要说给模型听」的场合都得反着算一次 —— 各处自己写一遍必然漂移。
/// 实测事故（用户日志）：观察帧上的红叉画在 upload 空间，配的文字却写「屏幕(2155,255)」，
/// 模型拿着这句话去和 1024 宽的图对照，只能自己猜缩放比（"the red十字 at screen(2155,255)
/// → upload ≈ (2155 * 1024/2400?)"），白烧几千 token 在换算上，还可能算错。
/// 判据：**同一条回执里出现的每个坐标都要各自标名**（docs §33.1 / §50）。
void MapScreenPointToApi(const AiCaptureMapping& map, int screenX, int screenY, int& apiX, int& apiY);

/// 点是否明显落在上传图外（精炼幻觉坐标）
bool IsApiPointClearlyOutsideImage(int apiX, int apiY, int apiW, int apiH);

/// 将点钳到图内；若原本明显在外返回 false
bool TryClampApiPointToImage(int& apiX, int& apiY, int apiW, int apiH);

/// 把模型输出的点/框归一到「上传图像素」。
/// 顺序：0~1000 归一化（与图内像素重叠时优先）→ 已在上传图内 → 像截屏原图像素 → 失败。
/// outNote 可选，写入诊断用短说明（如 L"0~1000归一化"）。
bool ResolveVisionPointToApiImage(
    int& x, int& y,
    int apiW, int apiH, int srcW, int srcH,
    std::wstring* outNote = nullptr);

bool ResolveVisionRectToApiImage(
    int& x1, int& y1, int& x2, int& y2,
    int apiW, int apiH, int srcW, int srcH,
    std::wstring* outNote = nullptr);

/// Agent mouseClick/moveMouse：图内优先按上传像素；越界再试 0~1000 / 原图像素。
/// 与识图 ResolveVision*（0~1000 优先）不同，避免把合法像素点误当归一化放大飞点。
bool ResolveAgentPointerToApiImage(
    int& x, int& y,
    int apiW, int apiH, int srcW, int srcH,
    std::wstring* outNote = nullptr);

/// 精炼屏幕点相对上级是否漂移过大
bool IsRefineScreenDriftTooFar(int priorX, int priorY, int nextX, int nextY, int maxDistPx);

/// 粗定位后是否可跳过第二级 Zoom/纠偏（对齐 Midscene：deepLocate 仅在必要时开启）
struct CoarseLocateRefineGateInput {
    bool haveScreenBox = false;
    int boxW = 0;
    int boxH = 0;
    /// 当前观察区宽高（屏幕像素）；用于面积比，避免只看绝对边长
    int captureW = 0;
    int captureH = 0;
    bool coordsWereRemapped = false;
    /// 一级仅返回中心点（无 bbox）；归一化点在自适应下可跳过 Zoom
    bool pointOnly = false;
    /// 旧开关：像素空间紧凑框无面积/长宽比门槛时也可跳过
    bool acceptCompactBboxWithoutRefine = false;
    /// 自适应：紧凑且面积比/长宽比合格时跳过二级（默认建议开）
    bool adaptiveRefineDepth = true;
    int compactBboxMinSide = 20;
    int compactBboxMaxSide = 160;
    double compactAreaRatioMax = 0.03;
    double compactAspectMin = 0.35;
    double compactAspectMax = 3.0;
};

enum class CoarseLocateSkipReason {
    None = 0,
    CompactPixel,
    CompactRemapped,
    WideControlRemapped,
    /// 「小标签/小卡片」：单独看边长不「紧凑」（比观察区 8% 宽、或比 56 高），
    /// 但相对整屏面积很小（≤1%）→ 中心可点，不值得再烧一轮 Zoom。
    SmallLabel,
    /// 0~1000 / 原图像素换算后的单点（无框）：模型已给可点中心
    PointRemapped,
};

/// 返回 true 表示一级粗框已可直接点击，省一轮 API
bool ShouldAcceptCoarseLocateWithoutRefine(
    const CoarseLocateRefineGateInput& in,
    CoarseLocateSkipReason* outReason = nullptr);

/// 以屏幕点为中心生成 ROI（限制在 bounds 内）
void BuildZoomRoiAroundScreenPoint(
    int screenX, int screenY,
    int halfSide,
    int boundX1, int boundY1, int boundX2, int boundY2,
    int& outX1, int& outY1, int& outX2, int& outY2);

/// API 矩形 → 屏幕矩形
void MapApiRectToScreen(const AiCaptureMapping& map,
    int apiX1, int apiY1, int apiX2, int apiY2,
    int& screenX1, int& screenY1, int& screenX2, int& screenY2);

/// 构建 moveMouse + mouseClick (+ stopMacro) 动作 JSON
/// button=left|right|middle；clickCount=2 表示双击（间隔 40ms，用于打开图标/文件）
std::wstring BuildScreenClickActionsJson(int screenX, int screenY, bool includeStopMacro = true,
    const std::wstring& button = L"left", int clickCount = 1);

/// 组合任务 Skill（lookupMacroAction section=composite）
std::wstring MacroActionCompositeSkill();

/// 场景用法 Skill（lookupMacroAction section=usage）：何时调用何动作
std::wstring MacroActionUsageSkill();

/// 分步 Agent Skill（lookupMacroAction section=agent）：观察→规划→执行→验收
std::wstring MacroActionAgentSkill();

/// 命令行路线 Skill（lookupMacroAction section=command）：
/// 不需要看界面的活（写文件/批量/统计/注册表/取数）一律 runCommand 一步做完。
std::wstring MacroActionCommandSkill();

/// 办公文档 Skill（lookupMacroAction section=office）：readDocument 读、runCommand 写，
/// 覆盖 Excel/Word/PPT/PDF/CSV（产品文件 skills/agent/office.md）。
std::wstring MacroActionOfficeSkill();

/// 实时游戏/动态画面 Skill（lookupMacroAction section=game）：本地优先感知、
/// 颜色/找图跟手、keyDown 长按、相对鼠标、反作弊风险（产品文件 skills/agent/game.md）。
std::wstring MacroActionGameSkill();

std::wstring BuildAiActionHybridSystemPrompt(int imageWidth, int imageHeight);

/// ToolExecute 路由的短 system（与 Hybrid 共享硬规则，Skill 全文仍走 lookup）
std::wstring BuildAiActionToolExecuteSystemPrompt(
    int captureWidth, int captureHeight, bool withImage);

/// 简单「找目标并点击」的识图 prompt（粗定位：优先矩形）
std::wstring BuildCompositeLocatePrompt(const std::wstring& userTask,
    int imageWidth = 0, int imageHeight = 0);

/// 放大区精确点 prompt。prevPredictionMarked=true 时图上已画「上一轮预测点」红叉，
/// prompt 会说明这个参考物（但仍要求绝对坐标）。
std::wstring BuildCompositeRefinePointPrompt(const std::wstring& userTask, int levelIndex,
    int imageWidth = 0, int imageHeight = 0, bool prevPredictionMarked = false);

/// 全图纠偏：告知上一轮候选框，要求重新框选真正目标（避免 Zoom 围着错误粗点打转）
std::wstring BuildCompositeCorrectLocatePrompt(const std::wstring& userTask,
    int prevX1, int prevY1, int prevX2, int prevY2,
    int imageWidth = 0, int imageHeight = 0);

std::wstring BuildAiActionVisionQuerySystemPrompt(int imageWidth, int imageHeight);
