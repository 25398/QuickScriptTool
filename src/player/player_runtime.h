#pragma once
// ──────────────────────────────────────────────────────────────────
// player_runtime.h — 独立播放器（导出 EXE）的运行时依赖解析
//
// 用户视角只有两种模式（导出时选）：
//   自带    = 目标电脑没装本软件也能跑
//   走软件  = 目标电脑装了本软件，复用它的组件（体积小、行为完全一致）
//
// 本模块负责把「清单里的选择」翻译成「实际从哪儿加载」：
//   · 找图：自带 = 从释放目录 LoadLibrary；走软件 = 从软件目录 LoadLibrary
//   · OCR ：自带 = 系统 WinRT OCR；走软件 = 软件目录的 Python OCR
//
// 硬规则：**走软件模式不得硬失败**。找不到软件/组件时必须给出可读原因
// （或回退到自带），绝不能静默不跑 —— 那是用户最难诊断的失败方式。
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

#include "script_package.h"

namespace playerruntime {

/// 某个组件最终从哪来
enum class ComponentSource {
    None = 0,       // 本次不需要
    Bundled,        // 自带（从释放目录加载）
    InstalledApp,   // 走已装软件
    SystemOcr,      // 系统 WinRT OCR
    Unavailable,    // 需要但拿不到（进 fatalError 或 warnings）
};

const wchar_t* ComponentSourceName(ComponentSource s);

struct ResolveInput {
    scriptpkg::PlayerManifest manifest;
    /// 内嵌组件的释放目录（`%LOCALAPPDATA%\QstPlayer\rt\<hash>`）。
    /// 走软件模式下可能不存在。
    std::wstring bundledDir;
    /// 已装软件目录。调用方先调 FindInstalledAppDir() 填进来（可为空）。
    std::wstring installedAppDir;
};

struct ResolveOutput {
    /// false = 有致命缺失，player 应弹错误框后退出，而不是继续跑到一半才失败
    bool ok = true;
    ComponentSource openCv = ComponentSource::None;
    ComponentSource ocr = ComponentSource::None;
    std::wstring openCvPath;              // 实际加载成功的 dll 路径
    std::vector<std::wstring> warnings;   // 非致命（回退说明），player 记日志 + 首次运行提示
    std::wstring fatalError;              // 可读的致命原因
};

/// 查已装软件目录。回退链（按可靠性）：
///   1. HKCU\Software\QuickScriptTool\LastRunDir   （RecordLastRunAppDir 每次启动都写）
///   2. HKLM\Software\QuickScriptTool\InstallPath  （安装包写入）
///   3. HKCU\Software\QuickScriptTool\InstallPath
///   4. HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\QuickScriptTool.exe
/// 命中后校验目录里确实有 QuickScriptTool.exe，避免拿到过期路径。
std::wstring FindInstalledAppDir();

/// 运行时目录：`%LOCALAPPDATA%\QstPlayer\rt\<hash16>`。
/// 用 payload 哈希命名 ⇒ 同一台机器上多个导出的 exe 各自隔离，
/// 同一个 exe 反复运行命中同一目录（不重复解包）。
std::wstring RuntimeDirFor(uint64_t payloadHash);

/// 按清单准备依赖。
/// **OpenCV 在这里就 LoadLibrary 好** —— 因为 `TryInitOpenCv()`
/// （`src/opencv_runtime.cpp`）第一步是 `GetModuleHandleW("opencv_world4100.dll")`，
/// 只要提前加载好它就直接返回 Ready，`opencv_runtime.cpp` 一行都不用改。
ResolveOutput ResolveRuntime(const ResolveInput& in);

/// 在目录里找 opencv_world4100.dll 并加载。成功返回完整路径。
/// 失败填 reason（可读）。已加载过则直接返回其路径。
std::wstring LoadOpenCvFrom(const std::wstring& dir, std::wstring& reason);

}  // namespace playerruntime
