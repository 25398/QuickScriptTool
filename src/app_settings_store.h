#pragma once

#include "app_settings.h"

#include <string>

bool LoadAppSettings(quickscript::AppSettings& out);
bool SaveAppSettings(const quickscript::AppSettings& settings);
std::wstring AppSettingsFilePath();
/// 自检用：改写盘路径。传空字符串恢复为 AppDir()\app_settings.json。
void SetAppSettingsFilePathForTest(const std::wstring& path);
/// 文件存在且非空并解析后写入 out。失败时 **不改** out（与 LoadAppSettings 失败会填默认值不同）。
bool TryLoadAppSettings(quickscript::AppSettings& out);
/// 以磁盘最新设置为底，只叠 inOut.home 的运行时选中/滚动等；写回并把 inOut 更新为合并结果。
/// includeGlobalHotkey 为 true 时同时叠全局启停热键（改热键保存）。
bool SaveAppSettingsPreserveUserSettings(quickscript::AppSettings& inOut, bool includeGlobalHotkey);
