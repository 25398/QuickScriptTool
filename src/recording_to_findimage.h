#pragma once
// ──────────────────────────────────────────────────────────────────
// recording_to_findimage.h — 录制点击单元识别与「转为找图点击」纯逻辑
// ──────────────────────────────────────────────────────────────────

#include "script_types.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

/// 模板半边长 clamp（禁止进 settings JSON）
constexpr int kClickCaptureHalfSizeMin = 16;
constexpr int kClickCaptureHalfSizeMax = 120;

constexpr int kDragRejectPx = 25;
constexpr int kDragRejectMaxAbsMoves = 8;
constexpr int kDragRejectRelativeSum = 40;
constexpr int kSnapPx = 5;
constexpr int kClickCaptureMinSide = 8;

inline int ClampClickCaptureHalfSize(int half) {
    if (half < kClickCaptureHalfSizeMin) return kClickCaptureHalfSizeMin;
    if (half > kClickCaptureHalfSizeMax) return kClickCaptureHalfSizeMax;
    return half;
}

struct ClickCaptureRectResult {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;  // exclusive
    int y2 = 0;  // exclusive
    int offsetX = 0;
    int offsetY = 0;
    bool valid = false;
};

/// 点击周围截模板矩形：右下开区间；与虚拟屏相交；过小则 invalid。
/// vsRight/vsBottom 为 exclusive（= left+width / top+height）。
ClickCaptureRectResult ComputeClickCaptureRect(
    int cx, int cy, int half,
    int vsLeft, int vsTop, int vsRight, int vsBottom);

/// 悬停缓存是否覆盖本次点击（光标几乎没动，可用按下前的画面）。
inline bool HoverPatchCoversClick(int patchCx, int patchCy, int clickCx, int clickCy, int maxDeltaPx) {
    if (maxDeltaPx < 0) maxDeltaPx = 0;
    const int dx = patchCx - clickCx;
    const int dy = patchCy - clickCy;
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    return adx <= maxDeltaPx && ady <= maxDeltaPx;
}

/// 模板文件名：rec_{sessionId}_{sequence}.bmp
std::wstring MakeRecordingClickCaptureFileName(uint64_t sessionId, uint64_t sequence);

struct ClickUnit {
    int downIndex = -1;
    int upIndex = -1;
    /// 点击前连续绝对 MoveMouse 段的起始下标（含）；无则为 -1。
    /// 段为 [snapMoveIndex, downIndex)；找图点击会删掉整段，避免仍移到录制坐标。
    int snapMoveIndex = -1;
    bool ok = false;
};

enum class ClickUnitRejectReason {
    None = 0,
    NoPair,
    ForbiddenBetween,
    DragDistance,
    DragAbsMoveCount,
    DragRelativeSum,
    ModifierHeld,
    NoCapturePath,
    NoCoordinate,
    MultiClick,
    NotApplicable,
};

struct ClickUnitProbe {
    ClickUnit unit{};
    ClickUnitRejectReason reject = ClickUnitRejectReason::NotApplicable;
};

/// 在 [begin,end) 内从 downIndex 向后配对同 button 的 Up。
ClickUnitProbe ProbeClickUnitFromDown(
    const std::vector<ScriptAction>& actions, int downIndex,
    int begin, int end);

/// 编辑器选中规则：返回可升级 unit；否则 reject。
ClickUnitProbe ProbeClickUnitFromSelection(
    const std::vector<ScriptAction>& actions, int selectedIndex);

struct ConvertToFindImageOptions {
    bool requireCapturePath = true;   // 批量 true；单步可先补图再 false
    bool preferLocalSearch = false;   // 本迭代默认全屏
    /// 写入 FindImage.findTimeExpr：0=只找一次；-1=直到找到；正数=轮询最多找该秒数。
    std::wstring findTimeExpr = L"0";
    /// true：仅删除 originalNo 落在 erasableAbsMoveNos 内的绝对 Move（优化勾选 / 编辑器单选）。
    /// false：删除可删的连续前置 / Down–Up 间全部绝对 Move（无勾选＝全量转换）。
    bool restrictAbsMoveErase = false;
    std::set<int> erasableAbsMoveNos;
};

struct ConvertToFindImageResult {
    int converted = 0;
    int skipped = 0;
    std::wstring detail;  // 简短中文说明
};

/// 将单个 click unit 替换为 FindImage（稀疏删除顺序见设计稿 C5）。
/// 成功返回 true；失败不修改 actions。
bool ConvertOneClickUnitToFindImage(
    std::vector<ScriptAction>& actions,
    const ClickUnit& unit,
    const ConvertToFindImageOptions& options,
    ClickUnitRejectReason* outReject = nullptr);

/// 批量：从后往前处理 [begin,end) 内所有可升级 unit。
ConvertToFindImageResult ConvertActionsToFindImage(
    std::vector<ScriptAction>& actions,
    int begin, int end,
    const ConvertToFindImageOptions& options);

/// 有勾选：只转与勾选相交的 unit，且只删勾选中的绝对 Move；无勾选：不转换。
ConvertToFindImageResult ConvertActionsToFindImageSelected(
    std::vector<ScriptAction>& actions,
    const std::vector<char>& selected,  // size 须==actions；全 0 / 空 = 不转
    const ConvertToFindImageOptions& options);

void RenumberScriptActions(std::vector<ScriptAction>& actions);

/// 解析点击屏幕坐标：动作自身 x/y → 前置绝对 Move → unit 内最后绝对 Move。
/// 禁止把 (0,0) 当无效；无任何来源时返回 false。
bool ResolveClickScreenPoint(
    const std::vector<ScriptAction>& actions,
    const ClickUnit& unit,
    int& outX, int& outY);
