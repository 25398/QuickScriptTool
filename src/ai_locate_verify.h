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

/// UIA 控件名与目标描述是否算「同一个东西」（双向包含 / 最长公共子串 ≥2 / 去 UI 类型词后相等）。
/// 用途：融合时过滤掉名字不相干的锚点。
bool UiNameMatchesTarget(const std::wstring& name, const std::wstring& target);

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
