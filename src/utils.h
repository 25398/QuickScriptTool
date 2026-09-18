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
/// 路径是否位于某根目录下（大小写不敏感；根目录本身也算）
bool PathIsUnderRoot(const std::wstring& path, const std::wstring& root);
/// 安装在 Program Files 时，WebView2 沙箱无法写 exe 旁目录，须漫游到 LocalAppData
bool InstallDirNeedsRoamingWebView2Data(const std::wstring& exeDir,
    const std::wstring& programFiles, const std::wstring& programFilesX86);
/// 纯函数：解析 WebView2 用户数据目录（自检用）
std::wstring ResolveWebView2UserDataDir(const std::wstring& exeDir,
    const std::wstring& programFiles, const std::wstring& programFilesX86,
    const std::wstring& localAppData);
std::wstring ResolveWebView2FetchDataDir(const std::wstring& exeDir,
    const std::wstring& programFiles, const std::wstring& programFilesX86,
    const std::wstring& localAppData);
/// 运行时用户数据：便携/自定义目录用 exe 旁；Program Files 用 %LOCALAPPDATA%\QuickScriptTool\...
std::wstring WebView2UserDataDir();
std::wstring WebView2FetchDataDir();
/// 写入 HKCU\Software\QuickScriptTool\LastRunDir，供安装包定位正在使用的副本
void RecordLastRunAppDir();
/// 启动提示音路径：exe 旁 startup.wav
std::wstring AppStartupSoundFilePath();
/// 结束提示音路径：exe 旁 finish.wav
std::wstring AppFinishSoundFilePath();
/// 存在且含 RIFF/WAVE 头则视为可播放（损坏/空文件返回 false）
bool IsPlayableWavFile(const std::wstring& path);
/// 播放自定义启动提示音；文件缺失或 PlaySound 失败时回退 MessageBeep(MB_OK)
void PlayAppStartupSound();
/// 播放自定义结束提示音；文件缺失或 PlaySound 失败时回退 MessageBeep(MB_OK)
void PlayAppFinishSound();
std::wstring ScriptsDir();
std::wstring RecordingsDir();
std::wstring FindImagesDir();
/// 专业模式逻辑目录根：sched / ai 的空文件夹落在 AppDir()/library/<kind>
std::wstring LibraryKindDir(const std::wstring& kind);
void        EnsureScriptsDir();
void        EnsureFindImagesDir();
void        EnsureLibraryKindDir(const std::wstring& kind);

/// 脚本/录制 JSON 枚举项（支持子目录；folder 用 / 分隔的相对路径）
struct ScriptFileEntry {
    std::wstring path;
    std::wstring fileName;
    std::wstring folder;
};
/// 递归枚举 rootDir 下所有 .json（跳过 images 等保留名）
void EnumerateScriptJsonFiles(const std::wstring& rootDir, std::vector<ScriptFileEntry>& out);
/// 在 rootDir 子树中按文件名查找 .json（缺扩展名时补 .json）。多个同名时取先遇到的。
bool FindScriptJsonByFileName(const std::wstring& rootDir, const std::wstring& fileName,
    std::wstring& outPath);
/// 解析脚本/录制路径：现存完整路径优先；拖进专业模式子文件夹后仍按文件名找回。
/// 只返回 scripts/ 或 recordings/ 目录内的文件，不会打开目录外路径。
bool ResolveLibraryScriptPath(const std::wstring& pathOrName, std::wstring& outPath);
/// 规范化后大小写不敏感比较（缺失文件仍可比较弱规范路径）。
bool LibraryPathsEqual(const std::wstring& a, const std::wstring& b);
/// Win32 完整路径前缀判断（去 \\?\；缺失文件仍可判断是否落在目录树下）。
bool PathIsUnderDir(const std::wstring& path, const std::wstring& dir);
/// 若 path 在 oldDir 子树内，改写为 newDir 下的对应路径。用于文件夹重命名后重定向引用。
bool RelocatePathUnderDir(std::wstring& path, const std::wstring& oldDir, const std::wstring& newDir);
/// 递归枚举 rootDir 下所有子目录（相对路径，/ 分隔；不含 root 自身）
void EnumerateRelativeFolders(const std::wstring& rootDir, std::vector<std::wstring>& out);
/// 校验相对文件夹路径：禁止 ..、绝对路径、非法字符
bool IsSafeRelativeFolder(const std::wstring& folder);
std::wstring NormalizeRelativeFolder(std::wstring folder);
/// 确保 rootDir\\folder 整条路径存在
bool EnsureRelativeFolder(const std::wstring& rootDir, const std::wstring& folder);
std::wstring NowText();
/// 当前 Unix 秒时间戳字符串（无业务前缀；前缀由调用方拼接）
std::wstring TimestampName();
/// 去掉末尾 `.json`（大小写不敏感），用于列表显示名 / 无 HWND 时从文件名回退
std::wstring StripJsonExtension(std::wstring name);

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
/// 字符串感知的配对括号查找（跳过引号内内容与转义）；openPos 须指向开括号。
size_t FindMatchingJsonBrace(const std::wstring& src, size_t openPos);
size_t FindMatchingJsonBrace(const std::string& src, size_t openPos);
size_t FindMatchingJsonBracket(const std::string& src, size_t openPos);
size_t FindMatchingJsonBracket(const std::wstring& src, size_t openPos);
/// 解析 JSON 字符串数组字段（如 imagePaths）；找不到返回空
std::vector<std::wstring> ExtractJsonStringArray(const std::wstring& src, const std::wstring& key);
/// 解析 JSON 整数数组（如 imageUseVars）；支持 0/1 与 true/false
std::vector<int> ExtractJsonIntArray(const std::wstring& src, const std::wstring& key);

// ── 文件操作 ──────────────────────────────────────────────────────
std::wstring ReadAll(const std::wstring& path);
/// 当前 JSON 对象顶层键的冒号位置（跳过嵌套对象/数组；同名键 last-wins）。
size_t FindTopLevelJsonKeyColon(const std::wstring& src, const std::wstring& key);
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
/// 将绝对路径转为 JSON 存储用的相对路径（images/xxx.bmp，正斜杠避免 \t 被当转义）
std::wstring ImagePathForJson(const std::wstring& absolutePath);
/// 确保图片位于 scripts/images 下，必要时从外部路径复制进去
std::wstring EnsureImageInLibrary(const std::wstring& path);
/// 从 JSON 内容中收集所有引用的图片绝对路径
std::unordered_set<std::wstring> CollectImagePathsFromJson(const std::wstring& jsonContent);
/// 扫描所有脚本和录制文件，收集所有被引用的图片路径
std::unordered_set<std::wstring> CollectAllReferencedImages();
/// ZIP 导入后：按条目文件名重写 script JSON 中的 imagePath / aiTargetImagePath / recordedCapturePath。
void RemapImportedImagePathInScriptJson(std::wstring& content,
    const std::wstring& zipEntryFileName,
    const std::wstring& newRelPath);
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
/// 脚本 keyText（←/空格键 等）→ 虚拟键；无法识别返回 0。
UINT VirtualKeyFromKeyText(const std::wstring& keyText);
/// 把误存的 Unicode 箭头（U+2190 等）或缺 keyVk 的动作纠正为真正的 VK。
UINT NormalizeScriptKeyVk(UINT vk, const std::wstring& keyText);
std::wstring HotkeyText(UINT modifiers, UINT vk);
/// 展开文本中的 %环境变量%（如 %USERPROFILE%）；未定义变量保持原样。
/// 用于动作路径等字段，让脚本可用 %USERPROFILE%\Desktop 这类写法。
std::wstring ExpandEnvironmentVars(const std::wstring& text);
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
    std::wstring folder;  // 相对 ScriptsDir/RecordingsDir，空=根
    std::wstring recordTime;
    int actionCount = 0;
    double durationSeconds = 0;
    Hotkey hotkey{};
};

/// 格式化秒数为 分'秒" 格式
std::wstring FormatDuration(double sec);

/// 开机自动启动（HKCU\\...\\Run，值名「键鼠工坊」）
bool SetAutoStartOnBoot(bool enabled);
bool IsAutoStartOnBootEnabled();
