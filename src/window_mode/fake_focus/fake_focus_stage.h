#pragma once

#include <string>

namespace windowmode {

/// 注入副本（暂存）目录的保留天数。超过这个年龄且**删得掉**的副本会在启动清扫时删掉。
inline constexpr int kFakeFocusStageKeepDays = 30;

/// 安装程序「让位改名」留下的残留后缀：`FakeFocus32.dll.locked-<tick>`。
/// 之所以带 `.dll.` 两段而不是 `FakeFocus32.<n>.dll`：后者的形状会命中
/// `PickPreferredFakeFocusDll` 的 `FakeFocus32*.dll` glob，被当成一个可注入的候选。
inline constexpr wchar_t kFakeFocusLockedLeftoverTag[] = L".dll.locked-";

struct FakeFocusSweepResult {
    int stageDirsRemoved = 0;      /// 删掉的整份旧副本目录
    int stageKeptLocked = 0;       /// 仍被进程映射（删不掉）而保留的副本目录
    int leftoversRemoved = 0;      /// 删掉的 `FakeFocus*.dll.locked-*` 残留
    int leftoversKeptLocked = 0;   /// 仍被占用而保留的残留
};

/// 真实的暂存根目录：`%LOCALAPPDATA%\QuickScriptTool\module_stage`。
/// 拿不到 LocalAppData 时退化为 `<exe 目录>\module_stage`（便携/受限环境）。
std::wstring FakeFocusStageRootDir();

/// 某个源 DLL 在暂存目录中的**稳定**路径（同一份文件 ⇒ 同一路径），纯计算不落盘。
/// 文件名一律归一成 `FakeFocus32.dll` / `FakeFocus64.dll`：`FakeFocus32.next.dll`
/// 这类旁路槽如果按原名注入，`MapleIsFakeFocusModulePath` 与模块枚举的按名判据都会漏掉它。
std::wstring FakeFocusStagedPathFor(const std::wstring& sourcePath);

/// 确保暂存副本存在，`outStagedPath` 回传**实际用于注入**的路径。
/// ⚠ 这一层存在的唯一理由见 fake_focus_stage.cpp 顶部（安装目录那份永远不被映射）。
bool StageFakeFocusDllForInjection(const std::wstring& sourcePath,
    std::wstring& outStagedPath, std::wstring& err);

/// 启动清扫：删掉 `keepDays` 天没被碰过的副本目录（被进程占用的一律跳过），
/// 以及 exe 旁 `FakeFocus*.dll.locked-*` 残留（装/卸载程序让位改名留下的）。
FakeFocusSweepResult SweepStaleFakeFocusArtifacts(const std::wstring& exeDir, int keepDays);

// ── 自检用：显式指定暂存根，不碰真实用户环境 ──────────────────────────────
std::wstring FakeFocusStagedPathIn(const std::wstring& stageRoot,
    const std::wstring& sourcePath);
bool StageFakeFocusDllInto(const std::wstring& stageRoot,
    const std::wstring& sourcePath, std::wstring& outStagedPath, std::wstring& err);
FakeFocusSweepResult SweepStaleFakeFocusArtifactsIn(const std::wstring& stageRoot,
    const std::wstring& exeDir, int keepDays);

}  // namespace windowmode
