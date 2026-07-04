#pragma once

#include "app_settings.h"

#include <string>

bool LoadAppSettings(quickscript::AppSettings& out);
bool SaveAppSettings(const quickscript::AppSettings& settings);
std::wstring AppSettingsFilePath();
