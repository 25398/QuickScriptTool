#pragma once
// ──────────────────────────────────────────────────────────────────
// script_io.h — 脚本 JSON 读写（与鼠标宏/录制导入导出格式一致）
// ──────────────────────────────────────────────────────────────────

#include "script_types.h"
#include "coord_space.h"
#include "utils.h"
#include "window_mode/window_mode_json.h"

#include <sstream>
#include <string>
#include <vector>

bool IsRecordingScriptPath(const std::wstring& path);

struct ScriptFileData {
    /// 脚本文件格式版本（JSON 顶层 "v"）。
    /// 1 = 历史文件（无 "v" 字段）；2 = 显式记录版本的当前格式。
    /// **改字段语义/名字时必须**：① 把 kScriptSchemaVersion +1；
    /// ② 在 MigrateScriptFileData 里加一条 fromVer→fromVer+1 的分支；
    /// ③ 在 ScriptSerializationSelfTest 里补一条「老版本文件仍能正确迁移」的用例。
    ///
    /// ④ **若变更涉及「字段改名 / 结构变化」（不是值语义变化），必须改用
    /// `MigrateScriptJson`（解析**之前**的 JSON 层钩子），模型层救不了它** ——
    /// 因为解析器只认当前字段名，老文件里的旧名字在解析阶段就被丢成默认值了，
    /// 等拿到 ScriptFileData 再迁移已经无从恢复。实测语义：值语义迁移（枚举取值、
    /// 单位换算、字段拆并）走 MigrateScriptFileData；名字/结构变化走 MigrateScriptJson。
    int schemaVersion = 1;
    std::wstring scriptName;
    std::wstring recordTime;
    double durationSeconds = 0;
    Hotkey hotkey{};
    windowmode::WindowModeScriptConfig windowMode;
    CoordMeta coordMeta;
    bool coordsNormalized = false;  // JSON 中坐标是否为归一化格式
    double breakoutTimeSeconds = 0; // 默认模式脱离时间（秒），0 表示禁用
    int recordingCaptureMode = -1;  // -1=旧文件/未知，0=自动，1=绝对坐标，2=相对坐标，3=图片定位
    // 0/缺省=旧文件；1=微秒轴但仍可把前延迟挂在动作上；2=时间只在显式 Wait（及重复间隔）
    int inputTimingVersion = 0;
    std::vector<ScriptAction> actions;
    /// 旧脚本可能带 visualLayout；新保存不再写入脚本文件，打开时迁到缓存。
    std::wstring visualLayoutJson;
};

/// AppDir()\cache\visual\<hash>.json — 卡片坐标，不进脚本文件
std::wstring VisualLayoutCachePathForScript(const std::wstring& scriptPath);
bool SaveVisualLayoutCache(const std::wstring& scriptPath, const std::wstring& json);
bool LoadVisualLayoutCache(const std::wstring& scriptPath, std::wstring& jsonOut);
void DeleteVisualLayoutCache(const std::wstring& scriptPath);
void MoveVisualLayoutCache(const std::wstring& oldScriptPath, const std::wstring& newScriptPath);

/// 提取 JSON 对象字段（key 后的 {...}），找不到返回空
std::wstring ExtractNamedJsonObject(const std::wstring& content, const wchar_t* key);

/// 当前脚本文件格式版本。保存时写入 JSON 顶层 "v"；读取时缺省视为 1。
inline constexpr int kScriptSchemaVersion = 2;

/// 把 fromVer 版本的**原始 JSON 文本**迁移到当前版本，返回迁移后的文本。
/// 由 Load/Parse 在**构造解析器之前**调用（见 MigrateScriptFileData 上方说明）。
///
/// 为什么需要这一层（而不是只做模型层迁移）：
/// 字段**改名/结构变化**时，解析器只认当前字段名，老文件里的旧名字在解析阶段就被
/// 丢成默认值，拿到 ScriptFileData 再迁移已经无从恢复。所以名字/结构类迁移必须在
/// 解析之前改文本。
///
/// 今天 v1→v2 是**空迁移**，本函数原样返回（调用方据此复用同一次解析，零额外开销）；
/// 它的存在是为了让下一个形状变更有一个**位置正确**的落点。
std::wstring MigrateScriptJson(const std::wstring& content, int fromVer);

/// 把 fromVer 版本的已解析脚本迁移到当前版本（就地改 data）。
/// 只在 fromVer < kScriptSchemaVersion 时由 Load/Parse 调用。
///
/// 为什么迁移**已解析的模型**而不是原始 JSON：
/// 解析已统一到 nlohmann（json_util::WideObjectView），原始 JSON 到这一步已被消费成
/// ScriptAction / ScriptFileData；对模型做迁移比再改一遍 JSON 更简单、也更不容易漏字段。
/// 逐动作的迁移逻辑写在本函数里（遍历 data.actions）。
/// **限制**：只适用于「值语义」变化；字段改名/结构变化必须用 MigrateScriptJson。
void MigrateScriptFileData(ScriptFileData& data, int fromVer);

/// 若 version<2：安全 Expand 前延迟为 Wait，合并相邻 Wait，升为 version=2。
void NormalizeInputTiming(ScriptFileData& data, const std::wstring& path,
    bool forceRecordingExpand = false);

/// 规范化脱离时间：空值/负数/非数字均视为 0
inline double NormalizeBreakoutTimeSeconds(double seconds) {
    return seconds > 0.0 ? seconds : 0.0;
}

/// 默认模式下生效的脱离时间；窗口类模式恒为 0
inline double EffectiveBreakoutTimeSeconds(const ScriptFileData& data) {
    if (data.windowMode.enabled) return 0.0;
    return NormalizeBreakoutTimeSeconds(data.breakoutTimeSeconds);
}

/// 已解析对象的字段视图（**前向声明**：本头文件刻意不引入 nlohmann，
/// 只有 .cpp 需要完整定义。见 src/json_util.h 的 `WideObjectView`）。
namespace qst {
namespace jsonutil {
class WideObjectView;
}  // namespace jsonutil
}  // namespace qst

ScriptAction ParseScriptActionBlock(const std::wstring& block, size_t fallbackNo,
    bool coordsNormalized = false);

/// 与 `ParseScriptActionBlock` 完全同义，但字段从**已解析好的对象** `J` 取，
/// 不再对 `block` 做第二次 nlohmann 解析。`block` 仍要传：函数体内有几处
/// 刻意保留的**原文扫描**（useMode 的引号判定 / nestedWindowMode / imagePaths /
/// imageUseVars / watchMode / imageRegionX1 存在性），这些行为必须原样保留。
///
/// 用途：`LoadScriptFileData` 已经为顶层字段解析过整份文件，逐动作再解析一遍
/// 是纯重复劳动（2026-10-03 实测 355ms / 793ms）。调用方须保证 `J` 对应的就是
/// `block` 那一条动作，且 `J` 的生命周期覆盖本调用。
ScriptAction ParseScriptActionBlockWithView(const std::wstring& block,
    const qst::jsonutil::WideObjectView& J, size_t fallbackNo,
    bool coordsNormalized = false);

void WriteActionJson(std::wstringstream& file, const ScriptAction& a, bool last);
std::wstring ScriptActionToJsonString(const ScriptAction& a);
ScriptFileData LoadScriptFileData(const std::wstring& path, bool denormForDisplay = true);
ScriptFileData ParseScriptContent(const std::wstring& content);
bool SaveScriptFileData(const std::wstring& path, const ScriptFileData& data);
/// 把其它脚本里 runMacro/mousePlayback 的 targetPath 从 oldPath 改到 newPath。返回改写文件数。
int RetargetNestedLibraryScriptPaths(const std::wstring& oldPath, const std::wstring& newPath);
/// 文件夹重命名：改写 targetPath 落在 oldDir 子树内的嵌套引用。
int RetargetNestedLibraryScriptPathPrefix(const std::wstring& oldDir, const std::wstring& newDir);
