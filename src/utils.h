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
#include <unordered_set>
#include <vector>

// ── 字符串处理 ────────────────────────────────────────────────────
std::wstring Trim(const std::wstring& value);

// ── 路径工具 ──────────────────────────────────────────────────────
std::wstring AppDir();
std::wstring ScriptsDir();
std::wstring RecordingsDir();
std::wstring FindImagesDir();
void        EnsureScriptsDir();
void        EnsureFindImagesDir();
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

// ── 文件操作 ──────────────────────────────────────────────────────
std::wstring ReadAll(const std::wstring& path);
std::wstring ExtractString(const std::wstring& src, const std::wstring& key);
double      ExtractNumber(const std::wstring& src, const std::wstring& key, double fallback);
/// 解析 JSON 布尔：支持 true/false 与 1/0（ExtractNumber 无法解析 true/false）
bool        ExtractBool(const std::wstring& src, const std::wstring& key, bool fallback);
int         CountActionsInJson(const std::wstring& content);
/// 更新 JSON 中某个字符串字段的值（字段必须已存在）
std::wstring UpdateJsonStringField(const std::wstring& content,
                                    const std::wstring& key,
                                    const std::wstring& value);

// ── 图片管理 ──────────────────────────────────────────────────────
/// 判断路径是否在 images 目录下（绝对路径）
bool IsPathInImageDir(const std::wstring& path);
/// 将 JSON 中存储的路径（相对或绝对）解析为运行时绝对路径
std::wstring ResolveImagePath(const std::wstring& stored);
/// 将绝对路径转为 JSON 存储用的相对路径（images\xxx.bmp）
std::wstring ImagePathForJson(const std::wstring& absolutePath);
/// 确保图片位于 scripts/images 下，必要时从外部路径复制进去
std::wstring EnsureImageInLibrary(const std::wstring& path);
/// 从 JSON 内容中收集所有引用的图片绝对路径
std::unordered_set<std::wstring> CollectImagePathsFromJson(const std::wstring& jsonContent);
/// 扫描所有脚本和录制文件，收集所有被引用的图片路径
std::unordered_set<std::wstring> CollectAllReferencedImages();
/// 删除 images 目录下没有被任何脚本引用的孤立图片
int CleanOrphanImages();
/// 删除指定 JSON 文件所引用的图片（如果这些图片不被其他脚本引用）
void DeleteUnreferencedImagesOfScript(const std::wstring& scriptPath);

// ── ZIP 压缩包操作（stored 方式，无压缩，保证兼容性）──────────────
struct CreateZipResult {
    bool success = false;
    std::vector<std::wstring> skippedFiles;  // 本地路径不存在而跳过的文件
};
/// 将多个文件打包为 ZIP，files 为 (压缩包内名称, 本地文件路径) 对
/// requiredLocalPath 指定的文件必须存在，否则导出失败；其余缺失文件会跳过
CreateZipResult CreateZipFile(const std::wstring& zipPath,
                              const std::vector<std::pair<std::wstring, std::wstring>>& files,
                              const std::wstring& requiredLocalPath = L"");
/// 将 ZIP 中的所有文件解压到目标目录，返回解压的文件数量，失败返回 -1
int ExtractZipFile(const std::wstring& zipPath, const std::wstring& destDir);
/// 从 ZIP 中读取指定文件的内容（UTF-8 文本），用于读取 JSON
std::string ReadTextFromZip(const std::wstring& zipPath, const std::string& archiveName);

// ── 热键与按键名称 ────────────────────────────────────────────────
std::wstring VkName(UINT vk);
std::wstring HotkeyText(UINT modifiers, UINT vk);
/// holdMode：按下/松开各触发一次（与鼠标左键启停同语义）
std::wstring HotkeyText(UINT modifiers, UINT vk, bool holdMode);

/// Hotkey 数据结构 — 存储热键组合、文本、开关状态
struct Hotkey {
    UINT modifiers = 0;
    UINT vk = VK_F8;
    std::wstring text = L"F8";
    bool enabled = true;
    /// 按住启停：按下达阈值后启动，松开停止（捕获时按住达「长按判定」设为 true）
    bool holdMode = false;
};

/// 规范化长按判定秒数（须 > 0，默认 0.2）
double NormalizeHoldThresholdSeconds(double seconds);
DWORD HoldThresholdMsFromSeconds(double seconds);
/// 格式化为提示文案用数字（如 0.2、1）
std::wstring FormatHoldThresholdLabel(double seconds);

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

/// 开机自动启动（HKCU\\...\\Run，值名「鼠大侠」）
bool SetAutoStartOnBoot(bool enabled);
bool IsAutoStartOnBootEnabled();
