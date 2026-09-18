#pragma once
// ──────────────────────────────────────────────────────────────────
// ai_action_service.h — AI 宏动作服务层
// 封装宏执行中的 AI 调用、截图、桥接定位等逻辑
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "agent_core.h"
#include "ai_action_router.h"
#include "ai_locate_verify.h"
#include "app_settings.h"
#include "macro_execute_tools.h"
#include "macro_variables.h"
#include "script_types.h"

// ── 将 HBITMAP 编码为 JPEG base64 字符串 ──────────────────────────
std::string BitmapToBase64Jpeg(HBITMAP hBitmap, int quality = 80, double scale = 1.0);

/// 大图自动限边（maxLongEdge，默认 1024），降低上传与识图耗时
double ComputeEffectiveAiImageScale(int width, int height, double userScale, int maxLongEdge = 1024);

struct AiImageEncodeResult {
    std::string base64;
    int srcWidth = 0;
    int srcHeight = 0;
    int outWidth = 0;
    int outHeight = 0;
    double effectiveScale = 1.0;
};

/// 宏 AI 图片分析专用编码（自动缩放 + 适中 JPEG 质量；scale≤1，不下采样以外的放大）
/// maxLongEdge：观察帧可用 1024；locateAndClick 建议 1280 以保留小控件细节
/// imeStatusText：非空则编码前把输入法状态文字画到图左上角（TSF 输入法候选框
/// BitBlt/CAPTUREBLT 物理截不到，画上去让 Agent 从截图直接看到输入法状态）
AiImageEncodeResult EncodeBitmapForAiAnalysis(HBITMAP hBitmap, double userScale = 0.5,
    int maxLongEdge = 1024, const std::wstring* imeStatusText = nullptr);

/// 将剪贴板位图与其中的图片文件编成 JPEG base64，供 AI 图片分析/动作额外附图。
std::vector<std::string> EncodeClipboardSnapshotImages(const MacroClipboardSnapshot& snap);

/// Zoom/ReGround 裁剪图上传：可将小图上采样到 targetLongEdge（对齐 ZoomClick in_min_crop）
AiImageEncodeResult EncodeBitmapForAiZoomUpload(HBITMAP hBitmap, int targetLongEdge = 768);

/// 在图上标注「上一轮预测点」——红色十字（本机 GDI 绘制，<1ms）。
/// 依据：PrecisionCUA（arXiv 2604.13019）在**干净图**上于上一轮预测点画红叉，
/// 并要求模型「看红叉与目标的相对位置，仍输出**绝对坐标**」——前沿模型命中率
/// T1→T5 翻倍（Claude Opus 4.7 21%→45.4%，GPT-5.4-Pro 13.5%→41.0%）。
/// 两个反面教训（同一论文/同一批开源项目）：
///  ① 别改成「回答偏移量(dx,dy)」：`visual_anchor` 变体把 GPT-5.4-Pro 从 41.0% 打到 18.5%；
///  ② 别用**真鼠标光标**当锚点：开源界零实现，且 32px 光标在 960 宽上传图里只剩 ~12px。
/// 返回 false 表示没画上（调用方就不能在 prompt 里提红叉）。
bool DrawPredictionCrossOnBitmap(HBITMAP bmp, int cx, int cy, int armPx, int lineWidth = 3);

using AiMacroLogFn = std::function<void(const std::wstring& line)>;

// ── AiActiveContext ───────────────────────────────────────────────
struct AiActiveContext {
    int contextMode = 0;            // 0=无, 1=宏, 2=循环, 3=块
    int depth = 0;                  // 嵌套深度
    std::wstring modelName;
    std::unique_ptr<AgentCore> core;
};

// ── AiActionResult ────────────────────────────────────────────────
struct AiActionResult {
    bool ok = false;
    /// true 表示 textResult 为识图问答纯文本，不是动作 JSON
    bool visionQueryText = false;
    /// true 表示动作已在 Agent 闭环中由宿主即时执行，调用方勿再解析执行 textResult
    bool actionsAlreadyExecuted = false;
    /// Agent 是否至少执行过一轮动作工具（含 locate/activate 等）
    bool anyActionsExecuted = false;
    AiActionRouteKind routeKind = AiActionRouteKind::ToolExecute;
    std::wstring textResult;
    std::wstring errorMessage;
    /// completeTask 的 reason（可含失败措辞；逻辑转化写回前需甄别）
    std::wstring completeReason;
};

// ── 工具函数 ──────────────────────────────────────────────────────
bool IsAgentErrorResponse(const std::wstring& text);

AgentSendCallbacks MakeAiMacroSendCallbacks(
    AiMacroLogFn logFn,
    const std::atomic_bool* cancelFlag = nullptr,
    AiHttpAbortSlot* httpAbort = nullptr);

// 从 savedModels 中按 modelName 查找配置，构建 AgentCore（无工具）
std::unique_ptr<AgentCore> CreateAiActionCore(
    const std::wstring& modelName,
    const std::vector<quickscript::AiModelProfile>& savedModels,
    const std::wstring& fallbackApiUrl,
    const std::wstring& fallbackApiKey,
    const std::wstring& extraSystemPrompt = L"",
    int recvTimeoutMs = 120000,
    double temperatureOverride = -1.0,
    int maxTokensOverride = -1);

// 构建 AI 动作执行专用 AgentCore（注册 macro* 工具供 API 调用）
std::unique_ptr<AgentCore> CreateAiActionExecuteCore(
    const std::wstring& modelName,
    const std::vector<quickscript::AiModelProfile>& savedModels,
    const std::wstring& fallbackApiUrl,
    const std::wstring& fallbackApiKey,
    const std::wstring& extraSystemPrompt,
    int recvTimeoutMs = 120000,
    double temperatureOverride = -1.0);

// 执行 AI 文字分析（contextMode=0 时清空历史；≠0 时复用会话保留上下文）

/// 一次性识图问答：自建 core（无工具、无历史），发**一张图 + 一条 prompt**，返回模型原文。
/// 供「错点自纠」这类单发子任务用：调用方自己裁剪/标注图片，这里只负责发一次请求。
AiActionResult RunAiOneShotVisionQuery(
    const std::wstring& modelName,
    const std::vector<quickscript::AiModelProfile>& savedModels,
    const std::wstring& fallbackApiUrl,
    const std::wstring& fallbackApiKey,
    const std::wstring& systemPrompt,
    const std::wstring& userPrompt,
    const std::string& imageBase64,
    int recvTimeoutMs,
    const std::atomic_bool& stopFlag,
    AiHttpAbortSlot* httpAbort = nullptr);

AiActionResult ExecuteAiTextAnalysis(
    AgentCore* core,
    const std::wstring& resolvedPrompt,
    int outputType,
    const std::atomic_bool& stopFlag,
    int timeoutSec,
    AiMacroLogFn logFn = nullptr,
    AiHttpAbortSlot* httpAbort = nullptr,
    int contextMode = 0);

// 执行 AI 图片分析
AiActionResult ExecuteAiImageAnalysis(
    AgentCore* core,
    const std::wstring& resolvedPrompt,
    const std::string& screenshotBase64,
    int outputType,
    const std::atomic_bool& stopFlag,
    int timeoutSec,
    AiMacroLogFn logFn = nullptr,
    AiHttpAbortSlot* httpAbort = nullptr,
    int contextMode = 0,
    const std::vector<std::string>* extraImageJpegBase64 = nullptr);

// 构建 AI 动作执行的 system prompt（带截图）
std::wstring BuildAiActionExecuteSystemPrompt(
    int captureWidth,
    int captureHeight);

// 构建 AI 动作执行的 system prompt（纯文本）
std::wstring BuildAiActionExecuteTextSystemPrompt();

// 视觉/工具请求需要更长等待；返回 effective 超时秒数
int ResolveAiActionExecuteTimeoutSec(int userTimeoutSec, bool withImage);

/// 单次识图定位（locateAndClick）超时：默认更短，避免找不到时长时间卡住
int ResolveAiLocateVisionTimeoutSec(int userTimeoutSec);

// AI 图片分析专用超时（比动作执行更短，默认 90s 起；长 prompt 自动加长）
int ResolveAiImageAnalysisTimeoutSec(int userTimeoutSec, size_t promptChars = 0);

// 构建写入 user 消息的动作执行说明（与图片分析同路径，不用 system prompt）
std::wstring BuildAiActionExecuteUserInstruction(
    const std::wstring& taskPrompt,
    int captureWidth,
    int captureHeight,
    bool withImage);

struct ZoomRefineLocateOptions {
    int maxLevels = 2;
    /// 裁剪方边下限（屏幕像素）；ZoomClick 常用 in_min_crop≈768 作「上传边」，此处是裁屏边
    int minRoiSide = 160;
    int maxRoiSide = 720;
    /// 相对上一视野的收缩比（ZoomClick --in_ratio，默认 0.5）
    double shrinkRatio = 0.5;
    /// 相对粗框长边的放大倍数（有框时优先于 shrinkRatio）
    double bboxPadFactor = 3.0;
    /// 裁剪图上传目标长边；小于此则 INTER_LINEAR 上采样（ZoomClick --in_min_crop）
    int uploadLongEdge = 768;
    /// 精炼点相对上级中心漂移超过此像素则回退上级
    int maxRefineDriftPx = 220;
    /// AI 粗定位后找图精修（默认关：同帧自匹配常 100%/0px，会固化错误点）
    bool snapByFindImage = false;
    /// 已废弃：自匹配 100%/0px 时不得跳过 AI 精炼（会固化错误粗点）
    bool findImageSkipsAiRefine = false;
    double findImageThreshold = 72.0;
    /// 找图命中点相对识图中心允许的最大距离（像素）
    int findImageMaxSnapDist = 280;
    /// 旧开关：像素空间紧凑框无面积比门槛也跳过二级（默认关，易点邻近图标）
    bool acceptCompactBboxWithoutRefine = false;
    /// 自适应 refine 深度：一级粗框紧凑且面积比/长宽比合格时跳过二级（省一轮 API；对齐 Midscene deepLocate 按需）
    bool adaptiveRefineDepth = true;
    /// 懒补级：调用方只请求 1 级，但一级粗框未过紧凑门禁时自动补一级 Zoom 精炼。
    /// 简单目标（紧凑粗框）仍只花 1 轮 API；偏大/模糊目标保持 2 级精度。
    bool lazyEscalateRefine = true;
    int lazyEscalateMaxLevels = 2;
    int compactBboxMinSide = 20;
    int compactBboxMaxSide = 160;
    /// 粗框占观察区面积上限（自适应跳过二级）
    double compactAreaRatioMax = 0.03;
    double compactAspectMin = 0.35;
    double compactAspectMax = 3.0;
    /// 已少用：二级默认一律 Zoom deepLocate；保留供旧调用方强制语义
    bool forceZoomCropRefine = false;
};

struct ZoomRefineLocateResult {
    bool ok = false;
    int screenX = 0;
    int screenY = 0;
    int levelsUsed = 0;
    bool usedFindImageSnap = false;
    double findImageScore = -1.0;
    /// 一级粗框判定可点、跳过二级
    bool skippedRefine = false;
    /// 本地校验定级（可用/可疑/需精炼）；由 ExecuteZoomRefineLocate 填入
    AiLocateVerdict verdict = AiLocateVerdict::Suspect;
    std::wstring errorMessage;
};

/// 多级放大定位：粗框/粗点 → 裁剪放大 → 精点 → 屏幕坐标
/// uiAnchors 可选：调用方（引擎）传入的 UIA 控件框（屏幕坐标），用于「UIA+视觉融合」——
/// 视觉候选与控件框重合时直接用控件的精确矩形。窗口模式下前台往往不是目标窗口，
/// 调用方应传 nullptr（由引擎判断），避免张冠李戴。
ZoomRefineLocateResult ExecuteZoomRefineLocate(
    AgentCore* core,
    const std::wstring& userTask,
    const std::string& initialScreenshotBase64,
    int initialApiW,
    int initialApiH,
    const AiCaptureMapping& rootMap,
    const std::atomic_bool& stopFlag,
    AiMacroLogFn logFn = nullptr,
    AiHttpAbortSlot* httpAbort = nullptr,
    ZoomRefineLocateOptions opts = {},
    const std::vector<AiUiAnchor>* uiAnchors = nullptr,
    /// 出参：定位判决（可用/可疑/需精炼）与说明；调用方可据此提示模型或调整策略
    AiLocateVerdict* outVerdict = nullptr,
    std::wstring* outVerdictWhy = nullptr);

/// 本地校验定级（快路径与末级共用，不依赖 OCR）：
/// UIA 命中 / 框内特征密度 / 候选一致性 / 落点是否压在可交互控件上 →
/// 可用（Accept）/ 可疑（Suspect）/ 需精炼（Refine）。
/// ★「紧凑即点」「UIA 确认」这类提前返回的快路径也必须调用它：否则 verdict 会停在默认
/// 的 Suspect，把已经确认的点击也标成「可疑」，模型会为此白烧一轮重新确认。
AiLocateVerdict ComputeLocateVerdictAtPoint(int screenX, int screenY,
    int boxW, int boxH, double clusterAgreement,
    const std::vector<AiUiAnchor>& anchors,
    AiLocateVerdict* outVerdict, std::wstring* outWhy);

/// 点击后 ROI 取色校验：采样点颜色相对点击前变化则认为可能生效
bool VerifyClickEffectByColorSample(
    int screenX, int screenY,
    int beforeR, int beforeG, int beforeB,
    int tolerance = 12);

// 执行 AI 动作执行（混合路由：识图问答 / 组合点击 / 工具 / 多轮 / Agent 闭环）
// hooks 非空且含 onExecuteActions + onObserveScreen/onCaptureScreen 时：
// 边执行边观察；onObserveScreen 可用 aiObs 图片变量本地比对，未变则不上传新图
AiActionResult ExecuteAiActionExecute(
    AgentCore* core,
    const std::wstring& resolvedPrompt,
    const std::string& screenshotBase64,
    int captureWidth,
    int captureHeight,
    int contextMode,
    const std::atomic_bool& stopFlag,
    int timeoutSec,
    AiMacroLogFn logFn = nullptr,
    AiHttpAbortSlot* httpAbort = nullptr,
    const AiCaptureMapping* captureMapping = nullptr,
    AiActionHostHooks* hostHooks = nullptr,
    int maxAgentRounds = 10,
    const std::vector<std::string>* extraImageJpegBase64 = nullptr,
    /// 宿主已判定路由时传入（如：本地定位点击自带截屏，首帧截图纯属白烧，
    /// 但路由分类依赖 withImage，必须显式覆盖）：非空则跳过 ClassifyAiActionRoute。
    const AiActionRouteKind* routeOverride = nullptr);
