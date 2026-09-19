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

/// 把 fromVer 版本的已解析脚本迁移到当前版本（就地改 data）。
/// 只在 fromVer < kScriptSchemaVersion 时由 Load/Parse 调用。
///
/// 为什么迁移**已解析的模型**而不是原始 JSON：
/// 解析已统一到 nlohmann（json_util::WideObjectView），原始 JSON 到这一步已被消费成
/// ScriptAction / ScriptFileData；对模型做迁移比再改一遍 JSON 更简单、也更不容易漏字段。
/// 逐动作的迁移逻辑写在本函数里（遍历 data.actions）。
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

ScriptAction ParseScriptActionBlock(const std::wstring& block, size_t fallbackNo,
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
