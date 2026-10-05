#pragma once
// ──────────────────────────────────────────────────────────────────
// script_package.h — 脚本包（导出 / 导入）依赖收集
//
// 为什么有这个模块（2026-09-19 修复）：
//   导出脚本时只收集**根脚本自己**引用的图片，没有递归收集
//   RunMacro / MousePlayback 的 targetPath 指向的嵌套脚本 —— 于是
//   「脚本包」在嵌套场景下本身就是不完整的：对方导入后嵌套动作必然失败。
//   导出为独立 EXE 会把这个缺口放大（EXE 里没有补救的机会）。
//
// 设计要点：
//   1. **纯逻辑**，不碰 UI / COM，可直接单测（tools/script_package_selftest.cpp）。
//   2. 动作数组是**扁平 + indent** 结构（不是 children 嵌套），所以「递归」指的是
//      **脚本之间**的引用递归，不是动作树递归。
//   3. 包内布局：
//        script.json              根脚本
//        scripts/<名字>.json      嵌套脚本（可多层）
//        <图片基名>                模板图（保持既有扁平命名，兼容旧导入）
//        package.json             清单（v/root/scripts/images）
//   4. 嵌套脚本的 targetPath 在**打包时改写为裸文件名**（可移植引用），
//      这样对方机器上 ResolveLibraryScriptPath 能在 ScriptsDir()/RecordingsDir()
//      里按文件名找到它（EnumerateScriptJsonFiles 是递归的，子目录也能命中）。
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <utility>
#include <vector>

namespace scriptpkg {

/// 清单条目名（zip 根下）
inline constexpr wchar_t kManifestEntry[] = L"package.json";
/// 根脚本条目名
inline constexpr wchar_t kRootEntry[] = L"script.json";
/// 嵌套脚本子目录
inline constexpr wchar_t kNestedDir[] = L"scripts";
/// 清单格式版本
inline constexpr int kManifestVersion = 1;

/// 包内一个脚本条目
struct ScriptEntry {
    std::wstring srcPath;      // 源文件绝对路径
    std::wstring entryName;    // zip 内条目名
    std::wstring portableRef;  // 打包时写回 targetPath 的可移植引用（根脚本为空）
    std::wstring displayName;  // 用户可见名（错误提示用）
    /// 所有**解析到本条目**的原始引用字符串（targetPath / blockName）。
    /// 打包改写 targetPath 时按这些原值匹配 —— 不能拿 srcPath 当 key，
    /// 因为引用可能来自 blockName 回退（此时原值与 srcPath 并不相等）。
    std::vector<std::wstring> originalRefs;
    bool isRoot = false;
};

/// 一次导出的完整依赖计划
struct PackagePlan {
    std::vector<ScriptEntry> scripts;      // [0] 恒为根脚本
    std::vector<std::wstring> images;      // 去重后的图片绝对路径
    std::vector<std::wstring> missingRefs; // 解析不到的嵌套引用（原样字符串）
    bool hasNested = false;                // 是否含嵌套脚本（决定要不要打 zip）
};

/// 一个嵌套动作的引用候选。
/// 为什么是「一对」而不是一个值：targetPath 存的是**源机器上的绝对路径**
/// （编辑器用 script.path 填的），对方机器上必然失效；blockName 存的是脚本显示名，
/// 换机器反而能按文件名解析到。所以两个都要留，由解析方**先 targetPath 后 blockName**
/// 取第一个命中的 —— 不能两个都收，否则名字撞车时会多收一个不相干的脚本。
struct NestedRef {
    std::wstring targetPath;
    std::wstring blockName;
};

/// 收集单个脚本 JSON 里的嵌套引用候选。
/// **只认 runMacro / mousePlayback** —— runProgram / openFile 的 targetPath 不是脚本。
std::vector<NestedRef> CollectNestedRefsFromJson(const std::wstring& json);

/// 把脚本 JSON 里 runMacro / mousePlayback 的 targetPath 改写成可移植引用。
/// remap：旧值(原 targetPath) → 新值。匹配大小写不敏感。
/// **逐个出现位置推进**：同一个脚本里多个 runMacro 都会被改写
/// （不能用「只替换第一处」的实现，那正是历史 bug 的成因之一）。
/// 返回改写次数。
int RewriteNestedTargetPaths(std::wstring& json,
    const std::vector<std::pair<std::wstring, std::wstring>>& remap);

/// 把任意名字净化成 zip 条目可用的文件名（去路径分隔符与 Windows 非法字符）。
std::wstring SanitizeEntryFileName(const std::wstring& name);

/// 从根脚本出发递归收集嵌套脚本与其引用的图片。
/// 带访问集，**循环引用安全**（A→B→A 不会死循环）。
/// 失败（根脚本读不到）返回 false 并填 err；嵌套引用解析不到不算失败，
/// 记进 missingRefs 由调用方提示用户。
bool BuildPackagePlan(const std::wstring& rootPath, PackagePlan& out, std::wstring& err);

/// 清单里的一个条目
struct ManifestEntry {
    std::wstring entryName;    // zip 内条目名
    std::wstring portableRef;  // 打包时写进 targetPath 的可移植引用（导入侧按它建改写表）
    std::wstring displayName;  // 显示名 / 还原时的文件名主干
};

/// 解析 package.json 清单。旧包没有清单时返回 false（调用方走兼容路径）。
bool ParsePackageManifestJson(const std::wstring& json, std::vector<ManifestEntry>& out);

// ──────────────────────────────────────────────────────────────────
// 能力体检（导出为独立 EXE 前的「这个脚本要什么」）
// ──────────────────────────────────────────────────────────────────

/// 一个脚本（含其嵌套脚本）用到的能力统计。
/// 用途：导出对话框据此决定要不要让用户选「自带 / 走软件」，以及估算体积。
struct CapabilityScan {
    int totalActions = 0;
    /// 找图 / 找色：findImage / multiMatch / watchImage / findColor / colorMatch /
    /// getColor(imageLocate) / mouseDrag(imageLocate) —— 都需要 OpenCV
    int imageActions = 0;
    /// 文字识别：textRecognition —— 需要 OCR 后端
    int ocrActions = 0;
    /// AI 动作：aiTextAnalysis / aiImageAnalysis / aiActionExecute —— 需要 API Key
    int aiActions = 0;
    /// 会拉起外部程序/文件的动作（RunProgram / OpenFile / OpenWebpage）
    int externalActions = 0;
    /// 后台窗口模式（需要 FakeFocus 注入 DLL）
    bool windowMode = false;
    /// 脚本自带热键（player 会注册它作为停止通道之一）
    bool hasHotkey = false;

    bool NeedsOpenCv() const { return imageActions > 0; }
    bool NeedsOcr() const { return ocrActions > 0; }
    bool NeedsAi() const { return aiActions > 0; }
    bool NeedsFakeFocus() const { return windowMode; }
    /// 完全不依赖外部组件 ⇒ 导出产物最小、最省心
    bool IsSelfContainedOnly() const {
        return !NeedsOpenCv() && !NeedsOcr() && !NeedsAi() && !NeedsFakeFocus();
    }
};

/// 累加单个脚本 JSON 的能力（不会清零，便于跨脚本累加）。
void ScanActionCapabilities(const std::wstring& json, CapabilityScan& acc);

/// 递归扫整个包（根 + 全部嵌套脚本）。读不到的脚本跳过，不失败。
CapabilityScan ScanPackageCapabilities(const PackagePlan& plan);

// ──────────────────────────────────────────────────────────────────
// player 运行配置（写进清单，导出时由用户选择）
// ──────────────────────────────────────────────────────────────────

/// 「找图 / OCR 用哪一份」的选择。用户视角只有两种：
///   自带 = 目标电脑没装软件也能跑（体积大 / OCR 走系统 WinRT）
///   走软件 = 目标电脑装了软件，复用它的组件（体积小 / 行为完全一致）
struct PlayerMode {
    /// 找图：true = exe 内嵌 OpenCV；false = 用已装软件的 OpenCV
    bool bundledOpenCv = true;
    /// OCR：true = 系统 WinRT OCR；false = 用已装软件的 Python OCR
    bool bundledOcr = true;
    /// 后台窗口模式所需的 FakeFocus DLL 是否内嵌（这份没有"走软件"模式）
    bool bundledFakeFocus = true;
};

/// 清单里的 player 段（player 启动时据此决定依赖从哪来）
struct PlayerManifest {
    int version = 1;
    bool needOpenCv = false;
    bool needOcr = false;
    bool needFakeFocus = false;
    PlayerMode mode;
    std::wstring createdBy;  // 导出它的软件版本，便于排查
};

/// 生成清单文本（含 player 段）。
std::wstring BuildPackageManifestJson(const PackagePlan& plan, const PlayerManifest& player);

/// 从清单解析 player 段。旧包没有该段时返回默认值并返回 false。
bool ParsePlayerManifest(const std::wstring& json, PlayerManifest& out);

// ──────────────────────────────────────────────────────────────────
// payload 尾部：把一个 zip 追加到 player.exe 后面
// ──────────────────────────────────────────────────────────────────
//
// 布局： [player.exe][zip 字节][Tail 32 字节]
//   Tail = magic "QSTPKG01" (8) + payloadOffset (u64) + payloadSize (u64) + payloadHash (u64)
//
// 为什么不做运行时编译（rc.exe/link.exe）：用户机器上没有 MSVC，路径版本不可控。
// 尾部追加是唯一同时满足「零工具链 + 单文件」的做法。
// ⚠ 追加数据会让已有的数字签名失效 —— 本项目 exe 当前未签名，所以没有额外损失。

struct PayloadInfo {
    uint64_t offset = 0;
    uint64_t size = 0;
    /// payload 的 FNV-1a 64 哈希。用作「同一脚本的运行时目录名」与单实例互斥体名，
    /// 让同一台机器上多个导出的 exe 各自隔离、且重复运行命中同一目录。
    /// 直接存在尾标里 —— payload 可能 70MB，每次启动重算太贵。
    uint64_t hash = 0;
};

/// 把 payloadZipPath 的字节追加到 targetExePath 末尾并写尾标。
/// ⚠ targetExePath 必须是**已经被复制出来的副本**，不要直接改模板 exe。
bool AppendPayloadToExe(const std::wstring& targetExePath, const std::wstring& payloadZipPath,
    PayloadInfo* outInfo = nullptr);

/// 读自身（或任意 exe）尾部的 payload 信息。没有 payload 时返回 false。
bool ReadPayloadInfo(const std::wstring& exePath, PayloadInfo& out);

/// 把 payload 原样导出成一个独立 zip 文件（便于复用现有 ExtractZipFile）。
bool DumpPayloadToZip(const std::wstring& exePath, const PayloadInfo& info,
    const std::wstring& zipOutPath);

/// 便捷组合：读尾部 + 落成 zip。没有 payload 时返回 false。
bool ExtractEmbeddedPayload(const std::wstring& exePath, const std::wstring& zipOutPath,
    PayloadInfo* outInfo = nullptr);

}  // namespace scriptpkg
