#pragma once
// ──────────────────────────────────────────────────────────────────
// coord_space.h — 归一化坐标（0.0–1.0）跨分辨率适配
// 方案 C：JSON 存 0~1 归一化坐标 + 固定标准 coordMeta（2560×1440）。
// 保存：编辑器像素（当前屏幕）→ n*；coordMeta 恒为 2560×1440。
// 运行：PrepareScriptActionsForExecution 按当前屏幕生成执行副本（像素坐标）。
// ──────────────────────────────────────────────────────────────────

#include "script_types.h"
#include "image_match.h"
#include "window_mode/window_mode_types.h"

#include <string>
#include <vector>

// ── 坐标元数据：记录脚本录制/保存时的参考分辨率和坐标空间 ─────
struct CoordMeta {
    int version = 1;
    enum class Space { ScreenVirtual, WindowClient } space = Space::ScreenVirtual;
    int refOriginX = 0; // 录制/保存时虚拟桌面原点（SM_XVIRTUALSCREEN）
    int refOriginY = 0;
    int refWidth = 0;   // 标准参考宽（固定 2560，用于 n* 语义）
    int refHeight = 0;  // 标准参考高（固定 1440）
    int refDpi = 96;    // 标准 DPI 标记（找图不按 DPI 二次缩放）
    int captureWidth = 0;  // 保存/截图时实际屏幕宽（找图模板缩放基准，0=同 refWidth）
    int captureHeight = 0;
};

// ── 模板缩放比例 ──────────────────────────────────────────────────
struct TemplateScale {
    double sx = 1.0;
    double sy = 1.0;
};

// ── 核心 API ─────────────────────────────────────────────────────

/// 获取当前环境的参考尺寸
CoordMeta CaptureCurrentCoordMeta(
    const windowmode::WindowModeScriptConfig* wmCfg = nullptr);

/// 像素 → 归一化（保存时调用）
void NormalizeActionCoords(ScriptAction& a, const CoordMeta& meta);
void NormalizeScriptCoords(std::vector<ScriptAction>& actions, const CoordMeta& meta);

/// 归一化 → 像素（加载到编辑器 / 执行时调用）
void DenormalizeActionCoords(ScriptAction& a, const CoordMeta& meta, int targetW, int targetH);
void DenormalizeScriptCoords(std::vector<ScriptAction>& actions, const CoordMeta& meta,
    int targetW, int targetH);

/// 旧脚本迁移：无 coordMeta 时，假设坐标已是 refWidth×refHeight 下的像素，转为 norm
void MigrateLegacyScriptToNormalized(std::vector<ScriptAction>& actions,
    const CoordMeta& assumedRef);

/// 计算模板缩放比例（当前屏幕 / 保存时 capture 屏幕，不含 DPI 修正）
TemplateScale ComputeTemplateScale(const CoordMeta& meta, int currentW, int currentH);

/// 根据分辨率比例调整找图 scaleMin/Max（宽范围搜索，预览等场景）
ImageMatchOptions BuildResolutionAwareMatchOptions(const ScriptAction& action,
    const TemplateScale& resolutionScale);

/// 执行/测试统一选项：跨分辨率时单尺度 ratio×用户缩放，同分辨率时与用户 scale 一致
ImageMatchOptions BuildExecutionFindImageOptions(const ScriptAction& action,
    const TemplateScale& resolutionScale);

HBITMAP LoadResolutionMatchedTemplate(const std::wstring& path, const TemplateScale& ts);

/// 脚本 canonical 参考分辨率（保存/执行缩放基准，与当前屏幕无关）
CoordMeta StandardScriptCoordMeta();

/// 执行/找图用的 coordMeta（标准 ref + 文件中的 capture 尺寸）
CoordMeta ScriptCoordMetaForExecution(const CoordMeta& fromFile);

/// 保存 JSON：ref 固定 2560×1440，capture 记录当前实际屏幕（找图用）
CoordMeta BuildScriptCoordMetaForSave(const CoordMeta& pixelMeta);

/// 获取当前虚拟桌面边界（x,y 可为负；与 GDI 截屏坐标系一致）
void GetVirtualScreenBounds(int& x, int& y, int& w, int& h);

/// 编辑器加载：将 n* 反算为当前屏幕像素（仅改显示，不写回 JSON）
void DenormalizeScriptToCurrentScreen(std::vector<ScriptAction>& actions);

/// 执行前：用内存中的像素字段刷新 n*（编辑器未保存的改动）
void SyncNormFieldsFromPixels(std::vector<ScriptAction>& actions, const CoordMeta& meta);

/// 执行前：从 n* 反算到目标分辨率像素（深拷贝 actions 后调用，即运行副本）
std::vector<ScriptAction> PrepareScriptActionsForExecution(
    const std::vector<ScriptAction>& actions, const CoordMeta& scriptMeta);

/// 缩放模板加载（保留供预览等场景）
HBITMAP LoadScaledTemplateBitmap(const std::wstring& path, double sx, double sy);

/// 找图执行资源
struct PreparedFindImageMatch {
    HBITMAP bitmap = nullptr;
    ImageMatchOptions options{};
    int templateW = 0;   // 原图文件宽（偏移比例基准）
    int templateH = 0;
    double effScaleX = 1.0;
    double effScaleY = 1.0;
    bool templatePreScaled = false;
};
PreparedFindImageMatch PrepareFindImageMatch(const ScriptAction& action, const TemplateScale& ts);

/// 找图后点击/移动落点
void ResolveFindImageClickPoint(const ImageMatchResult& match,
    int origTplW, int origTplH, double nOffsetX, double nOffsetY,
    const TemplateScale& tmplScale, bool templatePreScaled, int& tx, int& ty);

// ── JSON 中 coordMeta 的序列化/反序列化 ──────────────────────────
void WriteCoordMetaJson(std::wstring& out, const CoordMeta& meta, bool trailingComma);
CoordMeta ParseCoordMetaJson(const std::wstring& content);
bool HasCoordMetaJson(const std::wstring& content);
