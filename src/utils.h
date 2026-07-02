#pragma once
// ──────────────────────────────────────────────────────────────────
// utils.h — 通用工具函数集合（声明）
// 提供字符串操作、文件系统、JSON解析、键盘热键名称等基础工具函数
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <string>
#include <sstream>
#include <vector>

// ── 字符串处理 ────────────────────────────────────────────────────
std::wstring Trim(const std::wstring& value);

// ── 路径工具 ──────────────────────────────────────────────────────
std::wstring AppDir();
std::wstring ScriptsDir();
std::wstring RecordingsDir();
void        EnsureScriptsDir();
std::wstring NowText();
std::wstring TimestampName();

// ── 窗口文本操作 ──────────────────────────────────────────────────
std::wstring GetText(HWND hwnd);
void        SetText(HWND hwnd, const std::wstring& text);
std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const std::string& text);

// ── 数值转换 ──────────────────────────────────────────────────────
int         ToInt(HWND edit, int fallback = 0);
double      ToDouble(HWND edit, double fallback = 0.0);
std::wstring F3(double value);

// ── JSON 转义 ─────────────────────────────────────────────────────
std::wstring EscapeJson(const std::wstring& value);
std::wstring UnescapeJson(const std::wstring& value);
std::vector<std::wstring> ExtractJsonActionBlocks(const std::wstring& content);
std::wstring UnescapeJson(const std::wstring& value);

// ── 文件操作 ──────────────────────────────────────────────────────
std::wstring ReadAll(const std::wstring& path);
std::wstring ExtractString(const std::wstring& src, const std::wstring& key);
double      ExtractNumber(const std::wstring& src, const std::wstring& key, double fallback);
int         CountActionsInJson(const std::wstring& content);

// ── 热键与按键名称 ────────────────────────────────────────────────
std::wstring VkName(UINT vk);
std::wstring HotkeyText(UINT modifiers, UINT vk);

/// Hotkey 数据结构 — 存储热键组合、文本、开关状态
struct Hotkey {
    UINT modifiers = 0;
    UINT vk = VK_F8;
    std::wstring text = L"F8";
    bool enabled = true;
};

/// 脚本元数据结构 — 存储脚本文件的基本信息
struct ScriptMeta {
    std::wstring name;
    std::wstring path;
    std::wstring recordTime;
    int actionCount = 0;
    double durationSeconds = 0;
    Hotkey hotkey{};
};

/// 格式化秒数为 分'秒" 格式
std::wstring FormatDuration(double sec);
