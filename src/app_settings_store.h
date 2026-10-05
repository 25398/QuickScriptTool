#pragma once

#include "app_settings.h"

#include <string>

bool LoadAppSettings(quickscript::AppSettings& out);
bool SaveAppSettings(const quickscript::AppSettings& settings);
std::wstring AppSettingsFilePath();
/// 把设置序列化成 app_settings.json 的文本。
///
/// `portableSecrets = true` 时 **apiKey 写明文**（不 DPAPI 加密）——用于随导出的
/// exe 一起分发：DPAPI 是**用户级**的，换一台机器 / 换一个用户就解不开，
/// 打进 exe 等于把 AI 动作静默弄坏。而 `UnprotectSecret` 对不以 "dpapi:" 开头的值
/// 是**原样返回**的，所以明文在目标机照样能读出来。
///
/// 刻意复用**同一个序列化器**（而不是另写一份"便携版"）：导出的 exe 要和
/// 软件内跑同一个脚本效果一模一样，任何字段漏掉都会变成行为差异。
std::wstring SerializeAppSettings(const quickscript::AppSettings& settings,
    bool portableSecrets);

/// 自检用：改写盘路径。传空字符串恢复为 AppDir()\app_settings.json。
void SetAppSettingsFilePathForTest(const std::wstring& path);
/// 文件存在且非空并解析后写入 out。失败时 **不改** out（与 LoadAppSettings 失败会填默认值不同）。
bool TryLoadAppSettings(quickscript::AppSettings& out);
/// 以磁盘最新设置为底，只叠 inOut.home 的运行时选中/滚动等；写回并把 inOut 更新为合并结果。
/// includeGlobalHotkey 为 true 时同时叠全局启停热键（改热键保存）。
bool SaveAppSettingsPreserveUserSettings(quickscript::AppSettings& inOut, bool includeGlobalHotkey);
