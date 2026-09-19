#pragma once
// Lightweight services for QstWebViewShell (no EngineHost).

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "app_settings.h"
#include "main_features.h"
#include "script_types.h"
#include "utils.h"
#include "window_mode/window_mode_types.h"

#include <vector>

namespace qst::webview {

struct BridgeContext {
    quickscript::AppSettings settings{};
    quickscript::ClickerSettings clicker{};
};

BridgeContext& Ctx();

/// Thread-safe post to WebView (shell installs poster via WM_BRIDGE_POST_JS).
void SetJsPoster(std::function<void(std::string)> poster);

/// 把壳侧的 UI 能力注入引擎钩子（依赖倒置，架构评估 B1）。
/// 壳必须在任何引擎代码调用 qst::webview::PostToWebUi /
/// NotifyWebDebugWindowSetting / SyncHomeSelectionCache **之前**调用一次
/// （wWinMain 顶部即可）。
///
/// 注意：PostToWebUi / HotkeyLogLine / NotifyWebDebugWindowSetting /
/// SyncHomeSelectionCache 这 4 个函数现在**定义在引擎侧**
/// （src/engine/engine_ui_hooks.cpp，默认 no-op 转发），声明见
/// src/engine/engine_ui_hooks.h。壳不再定义它们 —— 否则 qst_engine
/// 链接时会依赖壳符号，自检无法只链库。
void InstallBridgeUiHooks();

/// 强制从磁盘刷新 g_ctx.settings（openSettings / 外部写盘后调用）。
void ReloadSettingsFromDisk();
/// 悬浮球拖拽后只写位置，不碰其它设置。
void PersistFloatBallPlacement(bool docked, int edge, double xRatio, double yRatio,
    const std::wstring& monitorId);
/// 右键隐藏悬浮球：写盘并推 settings.changed。
void PersistShowFloatBall(bool show);

std::string JsonListScripts();
std::string JsonListRecordings();
std::string JsonListAgentConversations();
/// 专业模式：列出 kind(macro|rec|sched|ai) 下真实/逻辑文件夹（相对路径数组）
std::string JsonListLibraryFolders(const std::string& kindUtf8);
bool CreateLibraryFolder(const std::string& kindUtf8, const std::string& folderUtf8, std::string& err);
bool RenameLibraryFolder(const std::string& kindUtf8, const std::string& folderUtf8,
    const std::string& newNameUtf8, std::string& err);
bool DeleteLibraryFolder(const std::string& kindUtf8, const std::string& folderUtf8, std::string& err);
/// 将脚本/录制移到目标相对文件夹（空=根）；更新 path
bool MoveScriptToFolder(const std::string& pathUtf8, const std::string& destFolderUtf8,
    std::string& outNewPathUtf8, std::string& err);
/// 设置定时任务 / AI 对话的逻辑文件夹
bool SetItemLibraryFolder(const std::string& kindUtf8, const std::string& idUtf8,
    const std::string& folderUtf8, std::string& err);
void InvalidateScriptListCache();
std::string JsonOpenSettings();
std::string JsonAppBranding();
bool ApplySaveSettingsJson(const std::string& settingsObjJson, std::string& err);
/// 恢复出厂默认并持久化；成功后可用 JsonOpenSettings() 刷新 UI。
bool RestoreSettingsDefaults(std::string& err);
bool StartClickerFromOpts(const std::string& msgJson, std::string& err);
void StopClicker();
bool ResolveScriptPath(const std::string& idOrPathUtf8, std::wstring& outPath, std::string& err);
std::string JsonClickerStatus();
std::string JsonLoadScriptEditor(const std::wstring& path, bool isNew);
std::string JsonPeekScriptActions(const std::wstring& path);
/// 脚本库详情「动作预览」：ActionName 列表（不含完整动作 JSON）。
std::string JsonPreviewScriptActions(const std::wstring& path, int maxNames);
bool SaveEditorFromJson(const std::string& msgJson, std::string& outPathUtf8, std::string& err);
/// 解析编辑器调试请求：actions / startIndex / mode(step|run) / breakpoints / 调试热键
bool ParseDebugScriptJson(const std::string& msgJson, std::vector<ScriptAction>& actions,
    int& startIndex, bool& stepMode, std::vector<int>& breakpoints,
    Hotkey& debugHotkey, std::wstring& displayName,
    windowmode::WindowModeScriptConfig& wmCfg, std::string& err);
bool DeleteScriptFile(const std::string& pathUtf8, std::string& err);
bool RenameScriptFile(const std::string& pathUtf8, const std::string& newNameUtf8,
    std::string& outNewPathUtf8, std::string& err);
bool ImportScriptFile(bool toRecordings, std::wstring& outPath, std::string& err);
/// ZIP 导出时 skipped 为跳过的缺失图片数；skippedFilesJson 为 UTF-8 路径 JSON 数组（可空）。
bool ExportScriptFile(const std::string& pathUtf8, std::string& err,
    int* outSkipped = nullptr, std::string* outSkippedFilesJson = nullptr);
bool SetScriptHotkeyJson(const std::string& pathUtf8, const std::string& hotkeyTextUtf8,
    UINT vk, UINT modifiers, bool hold, std::string& err);
bool SetRecorderInputMode(int mode, std::string& err);

/// peekOnly=true：只读磁盘历史，不切换 g_agent（多 Tab / busy 时打开其它会话）。
std::string JsonOpenAgentConversation(const std::string& idUtf8, bool createNew, bool peekOnly = false);
bool DeleteAgentConversationById(const std::string& idUtf8, std::string& err);
bool BeginSendAgentMessage(const std::string& msgJson, std::string& err);
/// 持久化会话草稿（未发送文本/附件/编辑态），随会话文件保存。
bool SaveAgentDraft(const std::string& msgJson, std::string& err);
/// 最近助手修改的 JSON 数组（id/time/tool/title/path/ok/reverted）
std::string JsonListAgentChanges();
bool RevertAgentChangeById(const std::string& idUtf8, std::string& err);
/// 置 cancelFlag + Abort HTTP，对齐原生 Agent「取消」
bool CancelAgentMessage(std::string& err);
/// 从系统剪贴板读取路径/图片；图片落盘到 images/，返回路径 UTF-8 JSON 数组。
bool PasteAgentClipboardAttachments(HWND owner, std::string& outPathsJson, std::string& err);
/// 将 data:image/...;base64,... 落盘到 images/，outPathUtf8 为绝对路径。
bool SaveClipboardImageDataUrl(const std::string& dataUrlUtf8, const std::string& extHint,
    std::string& outPathUtf8, std::string& err);
/// 弹出系统「另存为」导出图片：优先复制 path，否则写 dataUrl。
bool SaveAgentImageAsDialog(HWND owner, const std::string& pathUtf8, const std::string& dataUrlUtf8,
    std::string& err);

std::string JsonEngineStatus();
std::string JsonThemeCatalog();
bool ApplyThemeSettings(int themeId, bool useCustom, COLORREF main, COLORREF accent, std::string& err);

std::string JsonListScheduledTasks();
bool SaveScheduledTaskFromJson(const std::string& msgJson, bool isUpdate, std::string& err);
bool DeleteScheduledTaskById(const std::string& idUtf8, std::string& err);
bool SetScheduledTasksGlobalDisabled(bool disabled, std::string& err);

std::string JsonLoadOptimizeRecording(const std::string& pathUtf8, std::string& err);
/// 在优化工作副本上应用方案；默认不写原文件。saveAsNew 时另存 recordings/ 新文件。
/// outExtraJson：成功时可选附加字段（无花括号），如 converted/skipped/detail、recording。
bool ApplyOptimizeAndSave(const std::string& msgJson, std::string& err, std::string& outExtraJson);

/// 窗口模式预览：从 bridge JSON 解析配置并查找目标 HWND。
bool ParseWindowModePreviewRequest(const std::string& json,
    windowmode::WindowModeScriptConfig& outCfg, std::string& err);
bool ResolveWindowModeTargetHwnd(const windowmode::WindowModeScriptConfig& cfg,
    HWND& outHwnd, std::wstring& outLabel, std::string& err);
std::string CaptureHwndPreviewDataUrl(HWND hwnd);

}  // namespace qst::webview
