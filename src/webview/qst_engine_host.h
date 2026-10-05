#pragma once
// Headless engine API for QstWebViewShell（实现：src/engine/engine_runtime.cpp）。

#include <windows.h>

#include "main_features.h"
#include "script_types.h"
#include "utils.h"
#include "window_mode/window_mode_types.h"

#include <string>
#include <vector>

namespace qst::engine {

bool Start(HINSTANCE inst);
void Shutdown();

HWND Hwnd();

bool RunScriptPath(const std::wstring& path, std::string& err);
void StopScript();
bool IsRunning();
/// 引擎是否忙（含「已投递但还没开始」的窗口期）。
/// AI 助手投递任务后必须用这个判断起止 —— 只轮询 IsRunning() 会在投递空档里
/// 误判「已经跑完」（见 EngineIsBusy 的注释）。
bool IsBusy();
/// **异步**请求运行脚本：投递到引擎窗口线程，立刻返回。
/// 与 RunScriptPath 的区别：后者只能在 UI 线程同步调，助手工具跑在工作线程上。
bool RequestRunScriptAsync(const std::wstring& path, std::string& err);
/// 取宏变量终值（助手用它取回 runDesktopTask 的结果）。
/// 调用约定：只能在 IsBusy() 为 false 时读。
bool GetMacroVariable(const std::wstring& name, std::wstring& out);
/// 编辑器调试：从指定动作开始单次执行（不受宏执行次数设置影响）
bool DebugRunActions(const std::vector<ScriptAction>& actions, int startIndex,
    bool stepMode, const std::vector<int>& breakpoints,
    const Hotkey& debugHotkey, const std::wstring& displayName,
    const windowmode::WindowModeScriptConfig& wmCfg, std::string& err);
bool IsDebugging();
bool DebugPaused();
bool DebugStepMode();
int ExecutedSteps();
void PlaybackProgress(int& current, int& total);
std::string RunningScriptNameUtf8();
/// 正在运行脚本的模式 0/1/2（非运行时 0）；真值来自引擎当前脚本，非 Web 缓存。
int RunningMode();
bool RunningWindowMode(windowmode::WindowModeScriptConfig& out);
/// 默认模式脱离暂停（真人键鼠打断后等待恢复）
bool IsBreakoutPaused();

/// 暂停 / 继续当前回放。**独立播放器专用**：产品内从不调用，
/// `playbackPaused_` 恒为 false，行为与以前完全一致。
/// 暂停只在**动作边界**生效 —— 单个长 wait 动作不会被打断（与调试单步同一套机制）。
void SetPlaybackPaused(bool paused);
bool IsPlaybackPaused();

void StartClicker();
void StopClicker();
void ToggleClicker();
bool IsClicking();
void ApplyClickerSettings(const quickscript::ClickerSettings& s);

void StartRecording();
void StopRecording();
bool IsRecording();
/// recorderWindowModeOverride: -1=用当前设置；0=全屏录制；1=窗口相对录制。
bool StartRecordingEx(std::string& errUtf8, int recorderWindowModeOverride = -1);
bool StopRecordingEx(std::string& savedPathUtf8, int& actionCount, std::string& errUtf8);
void SetActiveHomeTab(int tab); // 0 clicker 1 recorder 2 macro 3 ai
void SelectHomeItem(int tab, const std::wstring& path);
/// 壳 UI 模式：0=主页 1=宏编辑 2=录制优化。编辑/优化界面打开时静默全部启停热键。
void SetUiMode(int mode);
/// 主页强制重新武装全局/脚本热键（清 mute + RegisterHotKey）
void EnsureHotkeysArmed();
/// 输入框聚焦静默启停热键。source: 0=主壳 1=助手窗。运行中仍可热键停止。
void SetTypingHotkeysMuted(bool muted, int source);
/// 引擎当前主页 Tab/选中路径（RestoreHomeState 后真值；供 Web 启动对齐）
std::string GetHomeStateJson();
void BeginHotkeyCaptureRelease(const Hotkey& editing);
/// 动作按键捕获：放行全部键并临时注销 RegisterHotKey（可录入与启停热键相同的键）
void BeginActionKeyCaptureRelease();
void DebugWindowClosedByUser();
void EndHotkeyCaptureRelease();
void ReloadSettings();
/// 磁盘脚本/录制热键表 → RegisterHotKey（改热键/增删后必须调；ReloadSettings 不含此项）
void ReloadScriptsAndHotkeys();
void OpenScheduledTasks();
/// headless / 无 GDI legacy 时为 false（定时走 Web HTML）
bool CanOpenNativeScheduledTasks();
void ReloadScheduledTasks();
/// 间隔任务刚保存：重锚该任务的运行时计时（不写盘）。
void TouchScheduledIntervalClock(const std::wstring& id);
void TickScheduledTasks();
void SetUiHost(HWND hwnd);
std::wstring GlobalHotkeyText();
void SetGlobalHotkey(const Hotkey& hk);
Hotkey GetGlobalHotkey();
/// 热键撞车：excludePath 跳过该文件；excludeGlobal 跳过全局。冲突返回 true。
bool HotkeyChordConflicts(UINT vk, UINT modifiers, const std::wstring& excludePath,
    bool excludeGlobal, std::wstring& errOut);

/// 专属热键作用域：true = 当前页面列出全部内容（专业模式脚本库/定时/设置）。
void SetDedicatedHotkeyScopeAll(bool all);

/// 全局启停热键是否占了这个和弦。
bool GlobalHotkeyConflicts(UINT vk, UINT modifiers);
/// 收集与该和弦重复的专属热键持有者路径（脚本/录制）。
void CollectDedicatedHotkeyOwners(UINT vk, UINT modifiers,
    const std::wstring& excludePath, std::vector<std::wstring>& out);

}  // namespace qst::engine
