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
    std::wstring scriptName;
    std::wstring recordTime;
    double durationSeconds = 0;
    Hotkey hotkey{};
    windowmode::WindowModeScriptConfig windowMode;
    CoordMeta coordMeta;
    bool coordsNormalized = false;  // JSON 中坐标是否为归一化格式
    double breakoutTimeSeconds = 0; // 默认模式脱离时间（秒），0 表示禁用
    int recordingCaptureMode = -1;  // -1=旧文件/未知，0=自动，1=桌面绝对，2=FPS相对
    int inputTimingVersion = 0;      // 1=整数微秒绝对时间轴语义
    std::vector<ScriptAction> actions;
};

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
