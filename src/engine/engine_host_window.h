#pragma once
// ──────────────────────────────────────────────────────────────────
// engine_host_window.h — 引擎宿主窗（原 main_window.h 主体）
// 产品 WebView 路径：headless EngineHost + Engine* 门面；GDI 主页仅 QST_GDI_LEGACY=1
// ──────────────────────────────────────────────────────────────────

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "action_tree.h"
#include "action_utils.h"
#include "app_settings.h"
#include "app_settings_store.h"
#include "app_theme.h"
#include "config.h"
#include "controls.h"
#include "drawing.h"
#include "hotkey_dialog.h"
#include "image_match.h"
#include "input/foreground_input_router.h"
#include "input/mouse_input_backend.h"
#include "input/synthetic_input_filter.h"
#include "hotkey_stop.h"
#include "ime_hotkey_pass.h"
#include "input_timeline_scheduler.h"
#include "macro_variables.h"
#include "macro_debug_window.h"
#include "modern_edit.h"
#include "main_features.h"
#include "popup_combo.h"
#include "prompt_modal.h"
#include "crosshair_drag.h"
#include "ocr_engine.h"
#include "process_utils.h"
#include "recorder.h"
#include "recorder_timeline.h"
#include "recording_to_findimage.h"
#include "scheduled_task_scheduler.h"
#include "scheduled_task_ui.h"
#include "tray_menu.h"
#include "themed_popup_menu.h"
#include "match_overlay.h"
#include "ocr_overlay.h"
#include "screenshot_overlay.h"
#include "editor_dropdown.h"
#include "script_types.h"
#include "script_io.h"
#include "coord_space.h"
#include "script_action_builder.h"
#include "utils.h"
#include "agent_conversation_store.h"
#include "agent_ui_notify.h"
#include "ai_action_service.h"
#include "agent_ai_actions.h"
#include "ai_action_runtime.h"
#include "ui_component.h"
#include "ui_scale.h"
#include "editor_param_layout.h"
#include "find_image_ui_debug.h"
#include "taskbar_window.h"
#include "breakout_input.h"
#include "desktop_tools/desktop_tools.h"
#include "window_mode/window_pick_result.h"
#include "window_mode/window_mode_json.h"
#if defined(QST_WEBVIEW_WITH_ENGINE) && QST_WEBVIEW_WITH_ENGINE
// 引擎→壳的唯一出口（依赖倒置）。**不要**改回
// #include "webview/webview_bridge_backend.h" —— 那是壳的头文件，
// 引它会让 qst_engine 依赖壳符号、自检无法只链库（架构评估 B1）。
#include "engine/engine_ui_hooks.h"
#endif

/// 热键诊断日志：WebView 构建写入 exe 目录 webview_boot.log（HOTKEY: 前缀），
/// GDI 构建为空操作，便于用户机器回传日志定位「热键无反应」。
#if defined(QST_WEBVIEW_WITH_ENGINE) && QST_WEBVIEW_WITH_ENGINE
inline void HotkeyDiagLog(const std::string& line) { qst::webview::HotkeyLogLine(line); }
#else
inline void HotkeyDiagLog(const std::string&) {}
#endif

/// LL 钩子 __except 内专用：禁止 C++ 对象；路径写失败则跳过，绝不打开原 exe。
inline void HotkeySehBreadcrumb(const char* tag, DWORD code) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[160]{};
    sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u %s seh=0x%08lX\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        tag ? tag : "LL", code);
    const DWORD lineLen = static_cast<DWORD>(strlen(line));

    wchar_t logPath[MAX_PATH]{};
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)
        && wcsncpy_s(logPath, exePath, _TRUNCATE) == 0) {
        wchar_t* slash = wcsrchr(logPath, L'\\');
        if (slash
            && wcscpy_s(slash + 1,
                   static_cast<size_t>(MAX_PATH - (slash + 1 - logPath)),
                   L"shell_startup.log") == 0) {
            HANDLE h = CreateFileW(logPath, FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(h, line, lineLen, &written, nullptr);
                FlushFileBuffers(h);
                CloseHandle(h);
            }
        }
    }

    wchar_t localApp[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) {
        wchar_t dir[MAX_PATH]{};
        if (swprintf_s(dir, L"%s\\QuickScriptTool", localApp) > 0) {
            CreateDirectoryW(dir, nullptr);
            if (swprintf_s(logPath, L"%s\\shell_startup.log", dir) > 0) {
                HANDLE h = CreateFileW(logPath, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(h, line, lineLen, &written, nullptr);
                    FlushFileBuffers(h);
                    CloseHandle(h);
                }
            }
        }
    }
}
#ifndef QST_GDI_LEGACY
#define QST_GDI_LEGACY 0
#endif
#if (QST_GDI_LEGACY == 1)
#include "engine/engine_host_window_gdi.h"
#else
class SettingsDialog;
class AgentDialog;
class RecordingOptimizeDialog {
public:
    static HWND ActiveHwnd();
};
void NotifyActiveSettingsDialogSync();
void NotifyActiveSettingsDialogRelayout();
#endif

#include <imm.h>
#pragma comment(lib, "imm32.lib")
#include "window_mode/window_mode_session.h"
#include "window_mode/window_mode_executor.h"
#include "window_mode/window_mode_log.h"
#include "window_mode/window_mode_json.h"
#include "window_mode/window_coords.h"
#include "window_mode/ext_bridge/ext_bridge_server.h"
#include "process_utils.h"

// ── Global-hotkey low-level hook (fallback when RegisterHotKey is unavailable) ──
inline HHOOK ghHotkeyKbHook = nullptr;
inline HHOOK ghHotkeyMouseHook = nullptr;
inline HWND ghHotkeyHwnd = nullptr;
inline bool ghHotkeyPending = false;
/// 已处理本次按下：到 KEYUP 前忽略另一通道（RegisterHotKey + LL 钩子双投递）。
inline bool ghHotkeyNeedKeyUp = false;
/// 正在处理启停热键：防止 StopRecording 保存期间排队的第二通道再次启动录制。
inline bool ghHotkeyHandling = false;
inline UINT ghHotkeyVk = 0;
inline UINT ghHotkeyMods = 0;
inline bool ghHotkeyEnabled = false;
/// 回放/录制/连点进行中：热键停止时放宽修饰键判定。
inline std::atomic<bool> ghHotkeySessionBusy{false};
/// 钩子/独立轮询线程置位：工作线程立即停注入，不坐等 UI 处理 WM_HOTKEY。
inline std::atomic<bool> ghEmergencyStop{false};
/// 脚本工作线程的取消闩锁（指向 EngineHost::stopFlag_）。紧急停时一并置位，
/// 让只认 stopFlag_ 的找图/OCR/AI/快输也能立刻退出，不坐等 UI 调 StopRun。
inline std::atomic<bool>* ghWorkerCancelFlag = nullptr;
inline std::atomic<bool> ghStopPollerExit{false};
inline std::atomic<HANDLE> ghStopPollerThread{nullptr};
constexpr UINT_PTR kHotkeyLatchSyncTimerId = 0x48534B31u; // 'HSK1'
constexpr UINT_PTR kHotkeyHookWatchdogTimerId = 0x48534B32u; // 'HSK2'
constexpr UINT_PTR kImeHotkeyPassTimerId = 0x48534B33u; // 'HSK3'
/// UI 线程远程查询的中文模式缓存。LL 钩子禁止 SendMessage，只读这个标志。
inline std::atomic<bool> ghImeHotkeyPassCache{false};
/// LL 钩子最后一次收到事件的时间戳（GetTickCount）；配套 Raw 输入 tick 作为
/// 「系统确实有键盘/鼠标输入」的参照，用于识别 Windows 按 LowLevelHooksTimeout
/// 静默卸载 LL 钩子后自动重装（引擎看门狗，见 engine_hotkeys.cpp）。
inline DWORD ghHotkeyKbLastEventTick = 0;
inline DWORD ghHotkeyMouseLastEventTick = 0;
inline DWORD ghRawKbLastInputTick = 0;
inline DWORD ghRawMouseLastInputTick = 0;
/// 钩子安装失败已提示（避免每 10s 看门狗重复 toast）
inline bool ghHotkeyHookWarned = false;

/// RegisterHotKey 注册失败的热键 id（全局/脚本/录制）。这些热键由系统判定被其它
/// 程序占用（或跨提权注册失败），改由 LL 钩子兜底触发并吞键，实现头注释承诺的
/// 「LL 钩子是 RegisterHotKey 不可用时的 fallback」。
inline int ghRegFailIds[256] = {};
inline int ghRegFailCount = 0;
inline bool IsRegFailId(int id) {
    if (!id) return false;
    for (int i = 0; i < ghRegFailCount; ++i) if (ghRegFailIds[i] == id) return true;
    return false;
}
inline void AddRegFailId(int id) {
    if (!id || IsRegFailId(id)) return;
    if (ghRegFailCount < static_cast<int>(sizeof(ghRegFailIds) / sizeof(ghRegFailIds[0])))
        ghRegFailIds[ghRegFailCount++] = id;
}
inline void RemoveRegFailId(int id) {
    for (int i = 0; i < ghRegFailCount; ++i) {
        if (ghRegFailIds[i] != id) continue;
        ghRegFailIds[i] = ghRegFailIds[ghRegFailCount - 1];
        --ghRegFailCount;
        return;
    }
}
inline void ClearRegFailIds() { ghRegFailCount = 0; }
/// 主页选中变更后延迟写入 app_settings.json，避免连点时同步盘 I/O 卡死 UI/整机
constexpr UINT_PTR kHomeStatePersistTimerId = 0x48534B33u; // 'HSK3'
/// 仅左键 / holdMode：按住超过阈值后开始，松开停止；右键等仍为单击切换
/// 阈值由「其他设置 → 长按判定」写入 ghHoldThresholdMs（默认 200ms）
inline std::atomic<DWORD> ghHoldThresholdMs{200};
inline DWORD CurrentHoldThresholdMs() {
    const DWORD ms = ghHoldThresholdMs.load(std::memory_order_relaxed);
    return ms < 1 ? 200 : ms;
}

inline LONGLONG HoldQpcFreq() {
    static LONGLONG freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart > 0 ? f.QuadPart : 1;
    }
    return freq;
}
inline LONGLONG HoldQpcNow() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
inline DWORD HoldElapsedMs(LONGLONG startQpc) {
    if (startQpc <= 0) return 0;
    const LONGLONG dt = HoldQpcNow() - startQpc;
    if (dt <= 0) return 0;
    return static_cast<DWORD>((dt * 1000) / HoldQpcFreq());
}

inline DWORD ghHotkeyMouseDownTick = 0;
inline LONGLONG ghHotkeyMouseDownQpc = 0;
inline bool ghHotkeyMouseHoldArmed = false;  // 已按下、等待达到长按阈值
inline bool ghHotkeyMouseHoldDown = false;   // 已触发开始，等待松开停止
/// 全局热键为「按住启停」模式（键盘捕获达判定时间，或鼠标左键）
inline bool ghHotkeyHoldMode = false;
/// WM_GLOBAL_HOTKEY_DETECTED 的 wParam：按住启停命令；lParam 非 0 时为脚本热键 id
constexpr WPARAM kHotHoldStart = 1;
constexpr WPARAM kHotHoldStop = 2;
/// 松手确认窗口：Interception 同键下过长会「松开半天不停」；0=立刻投递 Stop
constexpr DWORD kHoldRawUpConfirmMs = 0;

/// 长按阈值定时器（Timer Queue，不受隐藏窗 WM_TIMER coalescing 影响）
inline std::atomic<HANDLE> ghHoldFireTimer{nullptr};
inline std::atomic<HANDLE> ghHoldReleaseTimer{nullptr};
/// 定时任务 Tick（headless 引擎窗隐藏后 SetTimer 常被饿死，间隔任务会一直不跑）
inline std::atomic<HANDLE> ghScheduledTaskQueueTimer{nullptr};

inline void StopScheduledTaskQueueTimer() {
    HANDLE t = ghScheduledTaskQueueTimer.exchange(nullptr, std::memory_order_acq_rel);
    if (t) DeleteTimerQueueTimer(nullptr, t, INVALID_HANDLE_VALUE);
}

inline void CALLBACK ScheduledTaskQueueTimerCb(PVOID, BOOLEAN) {
    const HWND hwnd = ghHotkeyHwnd;
    if (hwnd) PostMessageW(hwnd, WM_APP_SCHEDULED_TICK, 0, 0);
}

inline void StartScheduledTaskQueueTimer() {
    StopScheduledTaskQueueTimer();
    HANDLE t = nullptr;
    if (CreateTimerQueueTimer(&t, nullptr, ScheduledTaskQueueTimerCb, nullptr, 1000, 1000,
            WT_EXECUTEDEFAULT)) {
        ghScheduledTaskQueueTimer.store(t, std::memory_order_relaxed);
    }
}

inline void CancelHoldThresholdTimer() {
    HANDLE t = ghHoldFireTimer.exchange(nullptr, std::memory_order_acq_rel);
    if (t) DeleteTimerQueueTimer(nullptr, t, nullptr);
}

inline void CancelHoldReleaseTimer() {
    HANDLE t = ghHoldReleaseTimer.exchange(nullptr, std::memory_order_acq_rel);
    if (t) DeleteTimerQueueTimer(nullptr, t, nullptr);
}

inline void CALLBACK HoldThresholdTimerCb(PVOID, BOOLEAN) {
    HANDLE t = ghHoldFireTimer.exchange(nullptr, std::memory_order_acq_rel);
    // 回调内删除自身须用 INVALID_HANDLE_VALUE，否则会死锁
    if (t) DeleteTimerQueueTimer(nullptr, t, INVALID_HANDLE_VALUE);
    const HWND hwnd = ghHotkeyHwnd;
    if (hwnd) PostMessageW(hwnd, WM_HOLD_THRESHOLD_FIRE, 0, 0);
}

inline void CALLBACK HoldReleaseTimerCb(PVOID, BOOLEAN) {
    HANDLE t = ghHoldReleaseTimer.exchange(nullptr, std::memory_order_acq_rel);
    if (t) DeleteTimerQueueTimer(nullptr, t, INVALID_HANDLE_VALUE);
    const HWND hwnd = ghHotkeyHwnd;
    if (hwnd) PostMessageW(hwnd, WM_HOLD_THRESHOLD_FIRE, 0, 0);
}

inline void ScheduleHoldThresholdTimer(DWORD delayMs) {
    CancelHoldThresholdTimer();
    if (!ghHotkeyHwnd) return;
    if (delayMs < 1) delayMs = 1;
    HANDLE t = nullptr;
    // WT_EXECUTEINTIMERTHREAD：回调更快投递，减轻「刚满阈值却慢半拍才启」
    if (!CreateTimerQueueTimer(&t, nullptr, HoldThresholdTimerCb, nullptr, delayMs, 0,
            WT_EXECUTEINTIMERTHREAD | WT_EXECUTEONLYONCE)) {
        return;
    }
    HANDLE old = ghHoldFireTimer.exchange(t, std::memory_order_acq_rel);
    if (old) DeleteTimerQueueTimer(nullptr, old, nullptr);
}

/// 松手确认：勿依赖隐藏窗 WM_TIMER（易被 coalescing 推迟导致松手不停）
inline void ScheduleHoldReleaseConfirmTimer() {
    CancelHoldReleaseTimer();
    if (!ghHotkeyHwnd) return;
    HANDLE t = nullptr;
    if (!CreateTimerQueueTimer(&t, nullptr, HoldReleaseTimerCb, nullptr, kHoldRawUpConfirmMs, 0,
            WT_EXECUTEDEFAULT | WT_EXECUTEONLYONCE)) {
        PostMessageW(ghHotkeyHwnd, WM_HOLD_THRESHOLD_FIRE, 0, 0);
        return;
    }
    HANDLE old = ghHoldReleaseTimer.exchange(t, std::memory_order_acq_rel);
    if (old) DeleteTimerQueueTimer(nullptr, old, nullptr);
}

/// 当前由「按住启停」拉起的热键 id（脚本 id 或 HOTKEY_GLOBAL_ID）。
/// 必须独立于 hook 表：SuspendHotkeys/RegisterAllHotkeys 会重建表并清掉 holdActive，
/// 否则松开无法停止。
inline std::atomic<int> ghActiveHoldHotkeyId{0};

inline bool IsActiveHoldHotkeyId(int id) {
    return id != 0 && ghActiveHoldHotkeyId.load(std::memory_order_relaxed) == id;
}

inline bool DedicatedHoldSessionActive() {
    return hotkey_stop::DedicatedHoldOwnsRun(
        ghActiveHoldHotkeyId.load(std::memory_order_relaxed), HOTKEY_GLOBAL_ID);
}

/// 长按停止后短暂禁止同键再武装，避免 VHID 注入 KEYUP→停→KEYDOWN→再开 自激。
inline std::atomic<DWORD> ghHoldRearmBlockUntil{0};
inline std::atomic<UINT> ghHoldRearmBlockVk{0};
/// Raw 非 VHID：当前仍按下的长按热键 VK；0=按 Raw 已抬起。
inline std::atomic<UINT> ghPhysicalHoldDownVk{0};
/// Raw 报告抬起后的起始 tick；连续抬起超过阈值才 Stop（滤掉注入/焦点假抬起）。
inline DWORD ghHoldRawUpSinceTick = 0;
/// 已由 Raw 非注入设备确认用户松手（与脚本同键注入解耦）。
inline std::atomic_bool ghHoldUserReleased{false};
/// 最近一次「真人」长按键 KEYDOWN（含连发）tick；用于松手看门狗（不依赖 KEYUP）。
inline std::atomic<DWORD> ghHoldLastPhysDownTick{0};
/// 本会长按是否已见过阈值后的连发 KEYDOWN（决定看门狗宽限）。
inline std::atomic_bool ghHoldRepeatSeen{false};
/// 长按会话开始 tick（holdActive 置位时）。
inline std::atomic<DWORD> ghHoldSessionStartTick{0};
/// 长按会话中刚注入同键的时间戳（QPC），用于 LL 无 INJECTED 时识别假抬起。
inline std::atomic<LONGLONG> ghHoldLastInjectUpQpc{0};
inline std::atomic<LONGLONG> ghHoldLastInjectDownQpc{0};

inline void BlockHoldRearm(UINT vk, DWORD ms = 280) {
    if (!vk) return;
    ghHoldRearmBlockVk.store(vk, std::memory_order_relaxed);
    ghHoldRearmBlockUntil.store(GetTickCount() + ms, std::memory_order_relaxed);
}

inline bool IsHoldRearmBlocked(UINT vk) {
    if (!vk) return false;
    if (ghHoldRearmBlockVk.load(std::memory_order_relaxed) != vk) return false;
    return GetTickCount() < ghHoldRearmBlockUntil.load(std::memory_order_relaxed);
}

inline void SetPhysicalHoldDownVk(UINT vk) {
    if (!vk) return;
    ghPhysicalHoldDownVk.store(vk, std::memory_order_relaxed);
    ghHoldRawUpSinceTick = 0;
    ghHoldUserReleased.store(false, std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    ghHoldLastPhysDownTick.store(now, std::memory_order_relaxed);
    const DWORD start = ghHoldSessionStartTick.load(std::memory_order_relaxed);
    if (start != 0 && now - start > 80u) {
        ghHoldRepeatSeen.store(true, std::memory_order_relaxed);
    }
}

inline void ClearPhysicalHoldDownVk(UINT vk = 0) {
    if (vk == 0) {
        ghPhysicalHoldDownVk.store(0, std::memory_order_relaxed);
        return;
    }
    UINT expected = vk;
    ghPhysicalHoldDownVk.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
}

inline void MarkHoldSessionStarted() {
    const DWORD now = GetTickCount();
    ghHoldSessionStartTick.store(now, std::memory_order_relaxed);
    ghHoldLastPhysDownTick.store(now, std::memory_order_relaxed);
    ghHoldRepeatSeen.store(false, std::memory_order_relaxed);
    ghHoldUserReleased.store(false, std::memory_order_relaxed);
    ghHoldRawUpSinceTick = 0;
    ghHoldLastInjectUpQpc.store(0, std::memory_order_relaxed);
    ghHoldLastInjectDownQpc.store(0, std::memory_order_relaxed);
}

inline void ClearHoldPhysDownWatchdog() {
    ghHoldSessionStartTick.store(0, std::memory_order_relaxed);
    ghHoldLastPhysDownTick.store(0, std::memory_order_relaxed);
    ghHoldRepeatSeen.store(false, std::memory_order_relaxed);
}

inline void SetActiveHoldHotkeyId(int id) {
    ghActiveHoldHotkeyId.store(id, std::memory_order_relaxed);
    if (id != 0) MarkHoldSessionStarted();
}
inline void ClearActiveHoldHotkeyId() {
    ghActiveHoldHotkeyId.store(0, std::memory_order_relaxed);
    ClearHoldPhysDownWatchdog();
}

inline bool IsPhysicalHoldDown(UINT vk) {
    return vk != 0 && ghPhysicalHoldDownVk.load(std::memory_order_relaxed) == vk;
}

/// 设置热键弹窗打开时：放行「正在编辑」的那枚键（便于同键改短按↔长按），其它占用键仍拦截。
/// 动作「按键点击/按下」捕获时 passAll：放行全部键，避免与启停热键撞车导致无法录入。
inline bool ghHotkeyCaptureOpen = false;
inline bool ghHotkeyCapturePassAll = false;
inline UINT ghHotkeyCaptureIgnoreVk = 0;

inline bool IsHotkeyCapturePassThroughVk(UINT vk) {
    if (!ghHotkeyCaptureOpen) return false;
    if (ghHotkeyCapturePassAll) return true;
    return ghHotkeyCaptureIgnoreVk != 0 && vk == ghHotkeyCaptureIgnoreVk;
}

/// 宏回放中：已注销 RegisterHotKey（避免吞掉 SendInput），脚本热键改由 LL 钩子识别物理键。
inline bool ghPlaybackHotkeySuspended = false;
/// 「中文输入法不触发热键」：键盘热键不走 RegisterHotKey（否则系统会吞键），改由 LL 决定放行/触发。
inline std::atomic<bool> ghPassThroughTypingHotkeys{false};
/// 壳 UI 处于宏编辑 / 录制优化界面：所有启停热键（专属/通用、短按/长按、键盘/鼠标）全部静默，
/// 物理按键原样放行（不吞键、不触发），RegisterHotKey 注销，回主页后全量重注册。
inline std::atomic<bool> ghUiModeHotkeysMuted{false};
/// 壳/助手输入框聚焦：bit0=主壳，bit1=助手窗。仅挡空闲启动，运行中仍可热键停止。
inline std::atomic<uint32_t> ghUiTypingMuteBits{0};
inline bool UiTypingHotkeysMuted() {
    return ghUiTypingMuteBits.load(std::memory_order_relaxed) != 0;
}
inline bool UiStartHotkeysMuted() {
    return ghUiModeHotkeysMuted.load(std::memory_order_relaxed) || UiTypingHotkeysMuted();
}
/// 脚本+录制各最多 100，合计 200；不足时靠后的录制热键在 IME/回放挂起/长按下会哑火。
constexpr int kMaxPlaybackScriptHooks = 200;
struct PlaybackScriptHook {
    UINT vk = 0;
    UINT mods = 0;
    int hotkeyId = 0;
    bool holdMode = false;
    bool holdArmed = false;
    bool holdActive = false;
    bool holdExtended = false;
    DWORD holdDownTick = 0;
    LONGLONG holdDownQpc = 0;
    uint32_t holdSeq = 0;         // 本次按下序号
    uint32_t holdInvalidSeq = 0;  // 短按抬起作废的序号（禁止随后误启）
    DWORD lastToggleTick = 0;     // 单击切换去抖（按热键各自独立）
    bool toggleNeedKeyUp = false; // 启动键尚未抬起：挡住自动连发误停
    bool toggleConsumed = false;  // 本按下已被任一通道处理（防 RegisterHotKey+轮询先开后停）
};
inline std::atomic<bool> ghGlobalToggleConsumed{false};
inline DWORD ghLastWatchdogPokeTick = 0;
inline std::atomic<uint32_t> ghRawVkDownBits[8]{};
inline DWORD ghRawVkDownTick[256]{};
inline PlaybackScriptHook ghPlaybackScriptHooks[kMaxPlaybackScriptHooks]{};
inline int ghPlaybackScriptHookCount = 0;

/// 清空长按会话闩锁（结束运行 / 捕获热键 / Cleanup），避免僵尸 holdDown 导致无法再武装或松手不停。
inline void ClearAllHoldSessionLatches() {
    CancelHoldThresholdTimer();
    CancelHoldReleaseTimer();
    ghHotkeyMouseHoldArmed = false;
    ghHotkeyMouseHoldDown = false;
    ghHotkeyMouseDownTick = 0;
    ghHotkeyMouseDownQpc = 0;
    ghHotkeyPending = false;
    ghHoldRawUpSinceTick = 0;
    ghHoldUserReleased.store(false, std::memory_order_relaxed);
    ClearHoldPhysDownWatchdog();
    ClearActiveHoldHotkeyId();
    ClearPhysicalHoldDownVk();
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        ghPlaybackScriptHooks[i].holdArmed = false;
        ghPlaybackScriptHooks[i].holdActive = false;
        ghPlaybackScriptHooks[i].holdDownTick = 0;
        ghPlaybackScriptHooks[i].holdDownQpc = 0;
    }
}

/// 全局按住：按下序号 / 短按作废序号（不用 GetAsyncKeyState，吞键后异步键态不可信）
inline std::atomic<uint32_t> ghHoldSeqCounter{1};
inline uint32_t ghHotkeyHoldSeq = 0;
inline uint32_t ghHotkeyHoldInvalidSeq = 0;
inline uint32_t ghHotkeyHoldPostedSeq = 0;

inline uint32_t NextHoldSeq() {
    return ghHoldSeqCounter.fetch_add(1, std::memory_order_relaxed);
}

inline void InvalidateHoldSeq(uint32_t seq) {
    if (seq != 0) ghHotkeyHoldInvalidSeq = seq;
}

inline bool IsHoldSeqInvalid(uint32_t seq) {
    return seq == 0 || seq == ghHotkeyHoldInvalidSeq;
}

inline bool NeedsMouseHoldHotkey(UINT vk) {
    return vk == VK_LBUTTON;
}

inline bool NeedsHoldHotkey() {
    return ghHotkeyHoldMode || NeedsMouseHoldHotkey(ghHotkeyVk);
}

inline bool IsMouseVk(UINT vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON
        || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

inline bool NeedHotkeyMouseHook() {
    if (IsMouseVk(ghHotkeyVk)) return true;
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        if (IsMouseVk(ghPlaybackScriptHooks[i].vk)) return true;
    }
    return false;
}

inline void RequestEmergencyStop() {
    if (auto* f = ghWorkerCancelFlag) f->store(true, std::memory_order_release);
    const bool already = ghEmergencyStop.exchange(true, std::memory_order_acq_rel);
    if (already) return;
    HWND hwnd = ghHotkeyHwnd;
    if (hwnd) PostMessageW(hwnd, WM_GLOBAL_HOTKEY_DETECTED, 0, 0);
}

inline void ClearEmergencyStopIfIdle(bool clicking, bool recording, bool running) {
    if (!clicking && !recording && !running) {
        ghEmergencyStop.store(false, std::memory_order_release);
    }
}

inline void SetScriptToggleNeedKeyUp(int hotkeyId, bool v) {
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        if (ghPlaybackScriptHooks[i].hotkeyId == hotkeyId) {
            ghPlaybackScriptHooks[i].toggleNeedKeyUp = v;
            return;
        }
    }
}

inline void ClearScriptToggleNeedKeyUpForVk(UINT vk) {
    if (!vk) return;
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        auto& h = ghPlaybackScriptHooks[i];
        if (!h.holdMode && h.vk == vk) h.toggleNeedKeyUp = false;
    }
}

inline bool TryConsumeGlobalTogglePress() {
    return !ghGlobalToggleConsumed.exchange(true, std::memory_order_acq_rel);
}

inline bool TryConsumeScriptTogglePress(int hotkeyId) {
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        auto& h = ghPlaybackScriptHooks[i];
        if (h.hotkeyId != hotkeyId) continue;
        return hotkey_stop::TryConsumeTogglePress(h.toggleConsumed);
    }
    return true;
}

inline void ClearToggleConsumedForVk(UINT vk) {
    if (!vk) return;
    if (ghHotkeyVk == vk) {
        ghGlobalToggleConsumed.store(false, std::memory_order_release);
    }
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        auto& h = ghPlaybackScriptHooks[i];
        if (!h.holdMode && h.vk == vk) h.toggleConsumed = false;
    }
}

inline void NoteRawHotkeyVk(UINT vk, bool down) {
    if (!vk || vk > 255) return;
    const uint32_t bit = 1u << (vk & 31);
    auto& word = ghRawVkDownBits[vk >> 5];
    if (down) {
        word.fetch_or(bit, std::memory_order_relaxed);
        ghRawVkDownTick[vk] = GetTickCount();
    } else {
        word.fetch_and(~bit, std::memory_order_relaxed);
        ghRawVkDownTick[vk] = 0;
    }
}

inline bool IsRawHotkeyVkDown(UINT vk, DWORD now) {
    if (!vk || vk > 255) return false;
    const uint32_t bit = 1u << (vk & 31);
    if ((ghRawVkDownBits[vk >> 5].load(std::memory_order_relaxed) & bit) == 0) {
        return false;
    }
    const DWORD tick = ghRawVkDownTick[vk];
    if (tick == 0) return false;
    // 漏 KEYUP 时 800ms 内无新 Raw 按下则丢弃，避免粘死
    return (now - tick) < 800u;
}

inline bool HotkeyVkLooksDown(UINT vk) {
    if (!vk) return false;
    if ((GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0) return true;
    return IsRawHotkeyVkDown(vk, GetTickCount());
}

inline bool HotkeyKbLlLooksFresh(DWORD now) {
    if (ghHotkeyKbLastEventTick == 0) return false;
    return (now - ghHotkeyKbLastEventTick) < 200u;
}

inline bool ShouldPokeHotkeyWatchdog(DWORD now) {
    if (!ghHotkeyHwnd) return false;
    if (HotkeyKbLlLooksFresh(now)) return false;
    if (now - ghLastWatchdogPokeTick < 1000u) return false;
    ghLastWatchdogPokeTick = now;
    return true;
}

inline bool CheckHotkeyModifiers(UINT required, bool requireNoExtras);
inline bool IsChineseImeActiveForHotkeyPass();

/// 空闲单击热键：RegisterHotKey / LL 被全屏游戏吃掉时由轮询投递。
inline void PollIdleToggleFallback(DWORD now) {
    if (ghHotkeyCaptureOpen) return;
    if (UiStartHotkeysMuted()) return;
    if (ghHotkeyHandling) return;

    const bool busy = ghHotkeySessionBusy.load(std::memory_order_acquire);
    if (busy) return;

    const bool passMode = ghPassThroughTypingHotkeys.load(std::memory_order_relaxed);
    const bool llFresh = HotkeyKbLlLooksFresh(now);
    const HWND hwnd = ghHotkeyHwnd;
    if (!hwnd) return;

    auto maybeFire = [&](UINT vk, UINT mods, int hotkeyId, bool holdMode, bool consumed,
            hotkey_stop::IdleStartState& st) {
        if (!vk || holdMode) {
            hotkey_stop::TickIdleStart(st, false, false, false, false, now, 0);
            return;
        }
        const bool down = HotkeyVkLooksDown(vk);
        if (!down) {
            ClearToggleConsumedForVk(vk);
            hotkey_stop::TickIdleStart(st, false, false, false, false, now, 0);
            return;
        }
        const bool synth = IsMouseVk(vk)
            ? synthetic_input::MatchesMouseButton(vk, true)
            : synthetic_input::MatchesKey(vk, 0, false, true);
        const bool llOwns = hotkey_stop::LlOwnsToggleHotkey(
            !IsMouseVk(vk) && llFresh, passMode, ghPlaybackHotkeySuspended,
            IsRegFailId(hotkeyId));
        const auto tick = hotkey_stop::TickIdleStart(st, true, synth, consumed, llOwns,
            now, hotkey_stop::kIdleFallbackConfirmMs);
        if (tick != hotkey_stop::IdleTick::FireStart) return;
        if (!CheckHotkeyModifiers(mods, true)) {
            st.fired = false;
            return;
        }
        if (passMode && IsChineseImeActiveForHotkeyPass()) {
            st.fired = false;
            return;
        }
        HotkeyDiagLog("idle fallback fire id=" + std::to_string(hotkeyId)
            + " vk=" + std::to_string(vk));
        if (hotkeyId == HOTKEY_GLOBAL_ID) {
            PostMessageW(hwnd, WM_GLOBAL_HOTKEY_DETECTED, 0, 0);
        } else {
            PostMessageW(hwnd, WM_HOTKEY, static_cast<WPARAM>(hotkeyId), 0);
        }
    };

    static hotkey_stop::IdleStartState idleGlobal;
    static hotkey_stop::IdleStartState idleScripts[kMaxPlaybackScriptHooks];

    if (ghHotkeyEnabled && ghHotkeyVk && !NeedsHoldHotkey()) {
        maybeFire(ghHotkeyVk, ghHotkeyMods, HOTKEY_GLOBAL_ID, false,
            ghGlobalToggleConsumed.load(std::memory_order_acquire), idleGlobal);
    } else {
        hotkey_stop::TickIdleStart(idleGlobal, false, false, false, false, now, 0);
    }

    const int n = (ghPlaybackScriptHookCount < kMaxPlaybackScriptHooks)
        ? ghPlaybackScriptHookCount : kMaxPlaybackScriptHooks;
    for (int i = 0; i < n; ++i) {
        const auto& h = ghPlaybackScriptHooks[i];
        maybeFire(h.vk, h.mods, h.hotkeyId, h.holdMode, h.toggleConsumed, idleScripts[i]);
    }
    for (int i = n; i < kMaxPlaybackScriptHooks; ++i) {
        hotkey_stop::TickIdleStart(idleScripts[i], false, false, false, false, now, 0);
    }
}

/// 长按：仅当 LL 已死（全屏游戏卸钩）时用异步键态武装，避免吞键后的假抬起误启。
inline void PollIdleHoldFallback(DWORD now) {
    if (ghHotkeyCaptureOpen) return;
    if (UiStartHotkeysMuted()) return;
    if (ghHotkeySessionBusy.load(std::memory_order_acquire)) return;
    if (HotkeyKbLlLooksFresh(now)) return;
    const HWND hwnd = ghHotkeyHwnd;
    if (!hwnd) return;

    const DWORD thr = CurrentHoldThresholdMs();
    auto tickHold = [&](UINT vk, UINT mods, int hotkeyId, bool& armed, DWORD& downTick) {
        if (!vk || IsMouseVk(vk)) {
            armed = false;
            downTick = 0;
            return;
        }
        const bool down = HotkeyVkLooksDown(vk);
        if (!down) {
            armed = false;
            downTick = 0;
            return;
        }
        if (synthetic_input::MatchesKey(vk, 0, false, true)) return;
        if (!CheckHotkeyModifiers(mods, true)) return;
        if (IsHoldRearmBlocked(vk)) return;
        if (IsActiveHoldHotkeyId(hotkeyId)) return;
        if (!armed) {
            armed = true;
            downTick = now;
            return;
        }
        if (now - downTick < thr) return;
        armed = false;
        downTick = 0;
        HotkeyDiagLog("idle hold fallback fire id=" + std::to_string(hotkeyId));
        PostMessageW(hwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStart,
            hotkeyId == HOTKEY_GLOBAL_ID ? 0 : static_cast<LPARAM>(hotkeyId));
    };

    static bool gArmed = false;
    static DWORD gDown = 0;
    if (ghHotkeyEnabled && NeedsHoldHotkey() && !IsMouseVk(ghHotkeyVk)) {
        tickHold(ghHotkeyVk, ghHotkeyMods, HOTKEY_GLOBAL_ID, gArmed, gDown);
    } else {
        gArmed = false;
        gDown = 0;
    }

    static bool sArmed[kMaxPlaybackScriptHooks]{};
    static DWORD sDown[kMaxPlaybackScriptHooks]{};
    const int n = (ghPlaybackScriptHookCount < kMaxPlaybackScriptHooks)
        ? ghPlaybackScriptHookCount : kMaxPlaybackScriptHooks;
    for (int i = 0; i < n; ++i) {
        auto& h = ghPlaybackScriptHooks[i];
        if (!h.holdMode) {
            sArmed[i] = false;
            sDown[i] = 0;
            continue;
        }
        tickHold(h.vk, h.mods, h.hotkeyId, sArmed[i], sDown[i]);
    }
}

inline DWORD WINAPI HotkeyStopPollerProc(LPVOID) {
    hotkey_stop::PollerState st;
    while (!ghStopPollerExit.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        const bool busy = ghHotkeySessionBusy.load(std::memory_order_acquire);
        const bool canPoll = busy && ghHotkeyEnabled && ghHotkeyVk
            && !IsMouseVk(ghHotkeyVk) && !NeedsHoldHotkey();
        if (canPoll) {
            // 独占全屏 / DirectInput 下 GetAsyncKeyState 常过时；Raw INPUTSINK 仍可能看到 F8。
            const bool down = HotkeyVkLooksDown(ghHotkeyVk);
            if (!down) {
                // 忙碌时 idle 轮询不跑；UIPI 也丢 LL KEYUP。必须在此清消费闩，
                // 否则 FireStop 的 PostMessage 会被 OnHotkey TryConsume 丢掉。
                ClearToggleConsumedForVk(ghHotkeyVk);
            }
            const bool synth = synthetic_input::MatchesKey(ghHotkeyVk, 0, false, true);
            if (hotkey_stop::TickPoller(st, true, down, synth)
                == hotkey_stop::PollTick::FireStop) {
                RequestEmergencyStop();
            }
        } else {
            hotkey_stop::TickPoller(st, false, false, false);
            PollIdleToggleFallback(now);
            PollIdleHoldFallback(now);
        }
        if (ShouldPokeHotkeyWatchdog(now)) {
            bool anyDown = HotkeyVkLooksDown(ghHotkeyVk);
            if (!anyDown) {
                const int n = (ghPlaybackScriptHookCount < kMaxPlaybackScriptHooks)
                    ? ghPlaybackScriptHookCount : kMaxPlaybackScriptHooks;
                for (int i = 0; i < n && !anyDown; ++i) {
                    anyDown = HotkeyVkLooksDown(ghPlaybackScriptHooks[i].vk);
                }
            }
            if (anyDown) PostMessageW(ghHotkeyHwnd, WM_APP_HOTKEY_WATCHDOG, 0, 0);
        }
        Sleep(10);
    }
    return 0;
}

inline void StartHotkeyStopPoller() {
    if (ghStopPollerThread.load(std::memory_order_acquire)) return;
    ghStopPollerExit.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, HotkeyStopPollerProc, nullptr, 0, nullptr);
    if (!th) return;
    SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
    ghStopPollerThread.store(th, std::memory_order_release);
}

inline void StopHotkeyStopPoller() {
    ghStopPollerExit.store(true, std::memory_order_release);
    HANDLE th = ghStopPollerThread.exchange(nullptr, std::memory_order_acq_rel);
    if (!th) return;
    WaitForSingleObject(th, 500);
    CloseHandle(th);
}

inline void EnsureHotkeyAuxTimers() {
    HWND hwnd = ghHotkeyHwnd;
    if (!hwnd || !IsWindow(hwnd)) return;
    bool anyHold = NeedsHoldHotkey();
    if (!anyHold) {
        for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
            if (ghPlaybackScriptHooks[i].holdMode) { anyHold = true; break; }
        }
    }
    const bool busy = ghHotkeySessionBusy.load(std::memory_order_relaxed);
    if (anyHold || busy) SetTimer(hwnd, kHotkeyLatchSyncTimerId, 16, nullptr);
    else KillTimer(hwnd, kHotkeyLatchSyncTimerId);
    SetTimer(hwnd, kHotkeyHookWatchdogTimerId, busy ? 1000u : 10000u, nullptr);
    if (ghPassThroughTypingHotkeys.load(std::memory_order_relaxed)) {
        SetTimer(hwnd, kImeHotkeyPassTimerId, 250, nullptr);
    } else {
        ghImeHotkeyPassCache.store(false, std::memory_order_relaxed);
        KillTimer(hwnd, kImeHotkeyPassTimerId);
    }
}

/// 长按从首击起吞键（避免「放行 KEYDOWN + 达阈值再注入 KEYUP」与同键脚本抢键态导致一卡一卡）。
/// 短按取消时补发一次完整按键，保证仍能打出字母。
inline void ReplayShortPressHoldKey(UINT vk) {
    if (!vk || IsMouseVk(vk)) return;
    SendKeyboardKey(vk, true);
    SendKeyboardKey(vk, false);
}

/// 长按已武装、尚未达阈值：若用户开始敲其它键，立刻短按补发并取消武装。
/// 否则会等到热键 KEYUP 才 Replay，快打时字母错位（bian→bani）。
inline void FlushArmedHoldForInterleavedTyping(UINT incomingVk) {
    if (!incomingVk || IsMouseVk(incomingVk)) return;

    if (ghHotkeyEnabled && NeedsHoldHotkey() && !IsMouseVk(ghHotkeyVk)
        && ghHotkeyMouseHoldArmed && !ghHotkeyMouseHoldDown
        && ghHotkeyVk != 0 && ghHotkeyVk != incomingVk) {
        CancelHoldThresholdTimer();
        InvalidateHoldSeq(ghHotkeyHoldSeq);
        ghHotkeyMouseHoldArmed = false;
        ghHotkeyMouseDownTick = 0;
        ghHotkeyMouseDownQpc = 0;
        ClearPhysicalHoldDownVk(ghHotkeyVk);
        ghHotkeyNeedKeyUp = false;
        ghHotkeyPending = false;
        const UINT replayVk = ghHotkeyVk;
        ReplayShortPressHoldKey(replayVk);
    }

    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        auto& h = ghPlaybackScriptHooks[i];
        if (!h.holdMode || !h.holdArmed || h.holdActive || !h.vk) continue;
        if (IsMouseVk(h.vk) || h.vk == incomingVk) continue;
        CancelHoldThresholdTimer();
        h.holdInvalidSeq = h.holdSeq;
        h.holdArmed = false;
        h.holdDownTick = 0;
        h.holdDownQpc = 0;
        ClearPhysicalHoldDownVk(h.vk);
        ReplayShortPressHoldKey(h.vk);
    }
}

/// 标记真人按下了某个长按热键（仅跟踪 hold 模式的键）。
inline void NotePhysicalHoldKeyDown(UINT vk) {
    if (!vk || IsMouseVk(vk)) return;
    if (NeedsHoldHotkey() && !IsMouseVk(ghHotkeyVk) && ghHotkeyVk == vk) {
        SetPhysicalHoldDownVk(vk);
        return;
    }
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        const auto& h = ghPlaybackScriptHooks[i];
        if (h.holdMode && h.vk == vk) {
            SetPhysicalHoldDownVk(vk);
            return;
        }
    }
}

/// 当前是否存在「按住启停」活动会话，以及对应 VK。
inline bool GetActiveHoldSessionVk(UINT& outVk, int& outHotkeyId) {
    outVk = 0;
    outHotkeyId = 0;
    // 含鼠标左键全局按住：连点「按住即停」要靠 Raw 真人抬起，不能把鼠标会话排除掉
    if (NeedsHoldHotkey()
        && !DedicatedHoldSessionActive()
        && (ghHotkeyMouseHoldDown || IsActiveHoldHotkeyId(HOTKEY_GLOBAL_ID))) {
        outVk = ghHotkeyVk;
        outHotkeyId = HOTKEY_GLOBAL_ID;
        return true;
    }
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        const auto& h = ghPlaybackScriptHooks[i];
        if (!h.holdMode || !h.vk) continue;
        if (h.holdActive || IsActiveHoldHotkeyId(h.hotkeyId)) {
            outVk = h.vk;
            outHotkeyId = h.hotkeyId;
            return true;
        }
    }
    return false;
}

inline bool MouseHoldSessionActiveForVk(UINT btnVk) {
    if (!btnVk || !IsMouseVk(btnVk)) return false;
    UINT holdVk = 0;
    int holdId = 0;
    return GetActiveHoldSessionVk(holdVk, holdId) && holdVk == btnVk;
}

/// 指纹命中的鼠标抬起是否仍应交给 LL 热键处理。
/// 活动会话：连点「按住即停」。武装中：宏刚结束时短按抬起也必须能撤掉阈值定时器，
/// 否则 MatchesMouseButton 会把 UP 放行到游戏，200ms 后仍会误启动。
inline bool MouseHoldShouldAcceptPhysicalUp(UINT btnVk) {
    if (!btnVk || !IsMouseVk(btnVk)) return false;
    if (MouseHoldSessionActiveForVk(btnVk)) return true;
    if (ghHotkeyEnabled && ghHotkeyVk == btnVk
        && (ghHotkeyMouseHoldArmed || ghHotkeyMouseHoldDown
            || IsActiveHoldHotkeyId(HOTKEY_GLOBAL_ID))) {
        return true;
    }
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        const auto& h = ghPlaybackScriptHooks[i];
        if (!h.holdMode || h.vk != btnVk) continue;
        if (h.holdArmed || h.holdActive || IsActiveHoldHotkeyId(h.hotkeyId)) return true;
    }
    return false;
}

/// 当前长按会话热键 VK。脚本可注入同键；松手靠 LL + Expect/ExtraInfo/INJECTED。
inline UINT ActiveHoldSessionVk() {
    UINT vk = 0;
    int id = 0;
    return GetActiveHoldSessionVk(vk, id) ? vk : 0;
}

inline void NoteHoldSessionKeyInject(UINT vk, bool down) {
    if (!vk || vk != ActiveHoldSessionVk()) return;
    const LONGLONG now = HoldQpcNow();
    if (down) ghHoldLastInjectDownQpc.store(now, std::memory_order_relaxed);
    else ghHoldLastInjectUpQpc.store(now, std::memory_order_relaxed);
}

inline bool IsRecentHoldSessionInject(UINT vk, bool down, DWORD windowMs = 48) {
    if (!vk || vk != ActiveHoldSessionVk()) return false;
    const LONGLONG stamp = down
        ? ghHoldLastInjectDownQpc.load(std::memory_order_relaxed)
        : ghHoldLastInjectUpQpc.load(std::memory_order_relaxed);
    if (stamp == 0) return false;
    const LONGLONG dt = HoldQpcNow() - stamp;
    if (dt < 0) return true;
    const LONGLONG lim = (HoldQpcFreq() * static_cast<LONGLONG>(windowMs)) / 1000;
    return dt <= lim;
}

/// Raw/LL 真人抬起：立刻请求 Stop（不再人为拖延确认窗）。
/// 注入抬起须在调用前剔除：ExtraInfo / INJECTED / Expect(TTL)。
inline void PostHoldStopNow(UINT vk, int hotkeyId) {
    if (!ghHotkeyHwnd || !hotkeyId) return;
    CancelHoldReleaseTimer();
    ClearHoldPhysDownWatchdog();
    ghHoldRawUpSinceTick = 0;
    ghHoldUserReleased.store(false, std::memory_order_relaxed);
    if (hotkeyId == HOTKEY_GLOBAL_ID) {
        ghHotkeyMouseHoldArmed = false;
        ghHotkeyMouseHoldDown = false;
        ghHotkeyMouseDownTick = 0;
        ClearActiveHoldHotkeyId();
        BlockHoldRearm(vk, 80);
        PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStop, 0);
        return;
    }
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        auto& h = ghPlaybackScriptHooks[i];
        if (h.hotkeyId != hotkeyId) continue;
        h.holdArmed = false;
        h.holdActive = false;
        h.holdDownTick = 0;
        ClearActiveHoldHotkeyId();
        BlockHoldRearm(h.vk, 80);
        PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStop,
            static_cast<LPARAM>(h.hotkeyId));
        return;
    }
}

inline void NotePhysicalHoldKeyUp(UINT vk) {
    if (!vk) return;
    UINT holdVk = 0;
    int holdId = 0;
    if (GetActiveHoldSessionVk(holdVk, holdId) && holdVk == vk) {
        ClearPhysicalHoldDownVk(vk);
        PostHoldStopNow(vk, holdId);
        return;
    }
    if (IsPhysicalHoldDown(vk)) ClearPhysicalHoldDownVk(vk);
}

/// 兼容旧轮询路径（主路径已在 NotePhysicalHoldKeyUp 里立刻 Stop）。
inline void PollHoldReleaseConfirm() {
    if (!ghHotkeyHwnd) return;
    UINT vk = 0;
    int hotkeyId = 0;
    if (!GetActiveHoldSessionVk(vk, hotkeyId)) {
        ghHoldRawUpSinceTick = 0;
        ghHoldUserReleased.store(false, std::memory_order_relaxed);
        return;
    }
    if (!ghHoldUserReleased.load(std::memory_order_relaxed)) return;
    PostHoldStopNow(vk, hotkeyId);
}

/// 不依赖 KEYUP 的松手看门狗已停用：同键注入会吃掉真人连发信用/指纹，
/// 导致 lastPhysDown 不刷新 → 误停 → 仍按住再武装 → 反复启停。
/// 现改为禁止注入 ActiveHoldSessionVk，KEYUP 即可可靠停。
inline void PollHoldPhysDownWatchdog() {}

inline void RefreshImeHotkeyPassCache() {
    if (!ghPassThroughTypingHotkeys.load(std::memory_order_relaxed)) {
        ghImeHotkeyPassCache.store(false, std::memory_order_relaxed);
        return;
    }
    const ime_hotkey_pass::NativeProbe probe =
        ime_hotkey_pass::ProbeForegroundNativeMode(GetForegroundWindow());
    if (probe == ime_hotkey_pass::NativeProbe::Unknown) return;
    const bool block = (probe == ime_hotkey_pass::NativeProbe::Chinese);
    const bool prev = ghImeHotkeyPassCache.exchange(block, std::memory_order_relaxed);
    if (prev != block) {
        HotkeyDiagLog(std::string("IME pass cache=") + (block ? "1" : "0"));
    }
}

/// LL 钩子 / 轮询只读缓存。禁止在热路径里 Imm/SendMessage。
inline bool IsChineseImeActiveForHotkeyPass() {
    return ghImeHotkeyPassCache.load(std::memory_order_relaxed);
}

inline bool CheckHotkeyModifiers(UINT required, bool requireNoExtras) {
    const bool alt   = (GetAsyncKeyState(VK_LMENU)    & 0x8000) || (GetAsyncKeyState(VK_RMENU)    & 0x8000);
    const bool ctrl  = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
    const bool shift = (GetAsyncKeyState(VK_LSHIFT)   & 0x8000) || (GetAsyncKeyState(VK_RSHIFT)   & 0x8000);
    const bool win   = (GetAsyncKeyState(VK_LWIN)     & 0x8000) || (GetAsyncKeyState(VK_RWIN)     & 0x8000);
    if ((required & MOD_ALT)     && !alt)   return false;
    if ((required & MOD_CONTROL) && !ctrl)  return false;
    if ((required & MOD_SHIFT)   && !shift) return false;
    if ((required & MOD_WIN)     && !win)   return false;
    // 启动时要求没有“额外”修饰键，避免误触；停止/切换忙碌态时放宽——
    // 回放可能残留 Shift/Ctrl，或用户正按着 WASD 旁的修饰键。
    if (requireNoExtras && required == 0 && (alt || ctrl || shift || win)) return false;
    if (requireNoExtras) {
        if (!(required & MOD_ALT) && alt) return false;
        if (!(required & MOD_CONTROL) && ctrl) return false;
        if (!(required & MOD_SHIFT) && shift) return false;
        if (!(required & MOD_WIN) && win) return false;
    }
    return true;
}

/// 钩子丢 KEYUP 时，用异步键态清掉闩锁，避免启停热键永久哑火。
/// 只用于「清除」：异步键态不可靠时不得据此把 NeedKeyUp 强制保持为 true。
/// 注意：按住模式禁止用 GetAsyncKeyState——连点 SendInput 的 LEFTUP 会污染键态。
inline void SyncHotkeyLatches() {
    // 只用于「清除」：异步键态不可靠时不得据此把 NeedKeyUp 强制保持为 true。
    // 按住模式禁止用 GetAsyncKeyState——连点 SendInput 的 LEFTUP 会污染键态。
    if (ghHotkeyEnabled && ghHotkeyVk != 0 && !NeedsHoldHotkey()) {
        if ((GetAsyncKeyState(static_cast<int>(ghHotkeyVk)) & 0x8000) == 0
            && !IsRawHotkeyVkDown(ghHotkeyVk, GetTickCount())) {
            ghHotkeyNeedKeyUp = false;
            ghHotkeyPending = false;
            ghGlobalToggleConsumed.store(false, std::memory_order_release);
        }
    }
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        auto& h = ghPlaybackScriptHooks[i];
        if (h.holdMode || !h.vk || !h.toggleNeedKeyUp) continue;
        if ((GetAsyncKeyState(static_cast<int>(h.vk)) & 0x8000) == 0
            && !IsRawHotkeyVkDown(h.vk, GetTickCount())) {
            h.toggleNeedKeyUp = false;
            h.toggleConsumed = false;
        }
    }
}

inline void ClearToggleHotkeyLatches() {
    ghHotkeyNeedKeyUp = false;
    ghHotkeyPending = false;
    ghHotkeyHandling = false;
}

/// 按住热键：按下后计时，仍按住且达到阈值才开始（抬起仅由钩子非注入 UP 取消/停止）
/// 键盘勿用 GetAsyncKeyState 判是否仍按下：LL 钩子吞掉连发后异步键态常误报已抬起，长按会永不起。
/// 短按与 Poll 竞态：用 holdSeq / holdInvalidSeq 作废已排队的 Start。
inline void PollMouseHoldHotkey() {
    if (!ghHotkeyEnabled || !ghHotkeyHwnd || !ghHotkeyMouseHoldArmed || ghHotkeyMouseHoldDown) return;
    if (!NeedsHoldHotkey()) return;
    if (IsHoldRearmBlocked(ghHotkeyVk)) {
        CancelHoldThresholdTimer();
        ghHotkeyMouseHoldArmed = false;
        ghHotkeyMouseDownTick = 0;
        ghHotkeyMouseDownQpc = 0;
        return;
    }
    if (HoldElapsedMs(ghHotkeyMouseDownQpc) < CurrentHoldThresholdMs()) return;
    if (ghHotkeyPending || ghHotkeyHandling) return;
    // 仅鼠标左键可用异步键态（按下全程放行）；键盘必须跳过
    if (NeedsMouseHoldHotkey(ghHotkeyVk)
        && (GetAsyncKeyState(static_cast<int>(ghHotkeyVk)) & 0x8000) == 0) {
        CancelHoldThresholdTimer();
        ghHotkeyMouseHoldArmed = false;
        ghHotkeyMouseDownTick = 0;
        ghHotkeyMouseDownQpc = 0;
        InvalidateHoldSeq(ghHotkeyHoldSeq);
        return;
    }
    if (IsHoldSeqInvalid(ghHotkeyHoldSeq) || !ghHotkeyMouseHoldArmed) return;
    const bool busy = ghHotkeySessionBusy.load(std::memory_order_relaxed);
    if (!CheckHotkeyModifiers(ghHotkeyMods, !busy)) {
        // 修饰键不对：保持武装，等用户松修饰或继续按住后再试（不直接取消，避免误判）
        return;
    }
    const uint32_t seq = ghHotkeyHoldSeq;
    if (IsHoldSeqInvalid(seq) || !ghHotkeyMouseHoldArmed) return;
    if (ghHotkeySessionBusy.load(std::memory_order_relaxed) && DedicatedHoldSessionActive()) {
        // 专属长按已在跑：不要把会话改成全局，否则脚本鼠标抬起会被当成「全局松手即停」
        CancelHoldThresholdTimer();
        ghHotkeyMouseHoldArmed = false;
        ghHotkeyMouseDownTick = 0;
        ghHotkeyMouseDownQpc = 0;
        InvalidateHoldSeq(seq);
        return;
    }
    CancelHoldThresholdTimer();
    ghHotkeyNeedKeyUp = false;
    ghHotkeyMouseHoldArmed = false;
    ghHotkeyMouseHoldDown = true;
    ghHotkeyPending = true;
    ghHotkeyHoldPostedSeq = seq;
    SetActiveHoldHotkeyId(HOTKEY_GLOBAL_ID);
    PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStart, 0);
}

inline void PollScriptHoldHotkeys() {
    if (!ghHotkeyHwnd) return;
    const bool busy = ghHotkeySessionBusy.load(std::memory_order_relaxed);
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        auto& h = ghPlaybackScriptHooks[i];
        if (!h.holdMode || !h.holdArmed || h.holdActive || !h.vk) continue;
        if (IsHoldRearmBlocked(h.vk)) {
            h.holdArmed = false;
            h.holdDownTick = 0;
            h.holdDownQpc = 0;
            continue;
        }
        // busy 时勿清武装：否则阈值将到时被误取消，表现为「按够了却不触发」
        if (busy) continue;
        if (HoldElapsedMs(h.holdDownQpc) < CurrentHoldThresholdMs()) continue;
        if (h.holdSeq == 0 || h.holdSeq == h.holdInvalidSeq) {
            h.holdArmed = false;
            h.holdDownTick = 0;
            h.holdDownQpc = 0;
            continue;
        }
        if (!CheckHotkeyModifiers(h.mods, !busy)) continue;
        const uint32_t seq = h.holdSeq;
        if (!h.holdArmed || seq == h.holdInvalidSeq) continue;
        CancelHoldThresholdTimer();
        h.holdArmed = false;
        h.holdActive = true;
        SetActiveHoldHotkeyId(h.hotkeyId);
        PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStart,
            static_cast<LPARAM>(h.hotkeyId));
    }
}

inline void PollHoldHotkeys() {
    PollMouseHoldHotkey();
    PollScriptHoldHotkeys();
    PollHoldReleaseConfirm();
}

inline void FlushQueuedGlobalHotkeys(HWND hwnd) {
    if (!hwnd) return;
    MSG msg{};
    // 丢掉处理期间排队的第二通道启停，避免「刚停又开」。
    while (PeekMessageW(&msg, hwnd, WM_GLOBAL_HOTKEY_DETECTED, WM_GLOBAL_HOTKEY_DETECTED, PM_REMOVE)) {
    }
    while (PeekMessageW(&msg, hwnd, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
        if (static_cast<int>(msg.wParam) != HOTKEY_GLOBAL_ID) {
            // 脚本热键：塞回队列末尾会乱序，这里用 Post 还原
            PostMessageW(hwnd, WM_HOTKEY, msg.wParam, msg.lParam);
        }
    }
}

/// 是否为「按住启停」类键盘热键 VK（全局或脚本）。
inline bool IsAnyHoldHotkeyVk(UINT vk) {
    if (!vk || IsMouseVk(vk)) return false;
    if (NeedsHoldHotkey() && !IsMouseVk(ghHotkeyVk) && ghHotkeyVk == vk) return true;
    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
        if (ghPlaybackScriptHooks[i].holdMode && ghPlaybackScriptHooks[i].vk == vk) return true;
    }
    return false;
}

/// 长按热键上的脚本注入：可放行到目标。
/// 仅 ExtraInfo 标签视为本进程注入。远控 SendInput 也带 LLKHF_INJECTED，
/// 不得据此 Consume Expect，否则真人/远控松手会被当成脚本回声。
inline bool IsHoldHotkeySyntheticPass(UINT vk, bool down, bool injected, bool taggedSynthetic) {
    if (hotkey_stop::IsScriptTaggedInjection(taggedSynthetic)) {
        if (down) (void)synthetic_input::ConsumeSyntheticKeyDown(vk);
        else (void)synthetic_input::ConsumeSyntheticKeyUp(vk);
        return true;
    }
    if (injected) return false;
    return down ? synthetic_input::ConsumeSyntheticKeyDown(vk)
                : synthetic_input::ConsumeSyntheticKeyUp(vk);
}

// 实现体与 CALLBACK 分离：LL 钩子内未处理异常 → 进程以 0xC000041D
// (STATUS_FATAL_USER_CALLBACK_EXCEPTION) 直接消失；调试器下常被吞掉，
// 表现为「开发机正常、用户点开无进程」。__try 不能与 C++ 析构混在同一函数。
inline LRESULT HotkeyKbProcBody(int code, WPARAM wp, LPARAM lp) {
    input_emergency::LlHookGuard llGuard;
    if (code >= 0) {
        if (!lp) return CallNextHookEx(nullptr, code, wp, lp);
        ghHotkeyKbLastEventTick = GetTickCount();
        auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        const bool injected = (ks->flags & LLKHF_INJECTED) != 0;
        const bool extended = (ks->flags & LLKHF_EXTENDED) != 0;
        const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);
        const bool taggedSynthetic = synthetic_input::IsSyntheticExtraInfo(ks->dwExtraInfo);
        const UINT vkCode = static_cast<UINT>(ks->vkCode);
        const bool busy = ghHotkeySessionBusy.load(std::memory_order_relaxed);

        if (ghHotkeyCaptureOpen) {
            // 设置/捕获热键期间不触发启停
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        if (UiStartHotkeysMuted() && !busy) {
            // 宏编辑/录制优化/输入框聚焦：不启动；运行中仍允许停止
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        const bool passMode = ghPassThroughTypingHotkeys.load(std::memory_order_relaxed);
        const bool scriptViaHook = ghPlaybackHotkeySuspended || passMode;
        // 中文输入法：未运行时放行热键字符；运行中仍可停
        const bool passToApp = passMode && !busy && IsChineseImeActiveForHotkeyPass();

        // 快打：长按武装中插入其它键 → 先补发被吞热键，避免字母错位
        if (down && !injected && !taggedSynthetic) {
            FlushArmedHoldForInterleavedTyping(vkCode);
        }

        // ── 长按热键专用路径（与 MatchesKey 解耦）──────────────────────────
        // 注意：仅 VK 命中不够——带修饰键的长按不得吞掉裸键（否则 Ctrl+字母 会让字母打不出）。
        if (IsAnyHoldHotkeyVk(vkCode)) {
            UINT sessionVk = 0;
            int sessionId = 0;
            const bool sessionActive = GetActiveHoldSessionVk(sessionVk, sessionId)
                && sessionVk == vkCode;

            // 中文输入法且脚本未在跑：不吞键、不武装，当作普通输入
            if (passToApp && !sessionActive) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }

            // ── 首击起吞键 + 同键可注入 + LL 立刻停 ──────────────────────────
            // Interception：注入常无 LLKHF_INJECTED；靠 ExtraInfo + Expect(100ms TTL)。
            // 松手：脚本 ExtraInfo 滤掉后立刻停；远控 INJECTED 当作用户松手。
            if (up && sessionActive) {
                // 仅 ExtraInfo 视为脚本抬起。软件路径已不再 Expect；
                // VHID 仍用 Expect，但真人/远控松手以 Raw physicalKeyUp 或未打标签的 UP 为主。
                if (hotkey_stop::IsScriptTaggedInjection(taggedSynthetic)) {
                    (void)synthetic_input::ConsumeSyntheticKeyUp(vkCode);
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                // 远控 INJECTED 不得去 Consume Expect：否则 VHID 同键信用会把 ToDesk 松手吃掉。
                if (!injected
                    && ForegroundInputRouter::Instance().IsHidActive()
                    && ForegroundInputRouter::Instance().ActiveBackend()
                        == quickscript::ForegroundInputBackend::VirtualHid
                    && IsHoldHotkeySyntheticPass(vkCode, false, false, false)) {
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                while (synthetic_input::ConsumeSyntheticKeyUp(vkCode)) {}
                NotePhysicalHoldKeyUp(sessionVk);
                return 1;
            }

            if (IsHoldHotkeySyntheticPass(vkCode, down, injected, taggedSynthetic)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }

            if (ghHotkeyEnabled && NeedsHoldHotkey() && !IsMouseVk(ghHotkeyVk)
                && vkCode == ghHotkeyVk) {
                const bool globalArmed = ghHotkeyMouseHoldArmed;
                const bool globalActive = ghHotkeyMouseHoldDown
                    || IsActiveHoldHotkeyId(HOTKEY_GLOBAL_ID);
                if (up) {
                    if (globalArmed) {
                        CancelHoldThresholdTimer();
                        InvalidateHoldSeq(ghHotkeyHoldSeq);
                        ghHotkeyMouseHoldArmed = false;
                        ghHotkeyMouseDownTick = 0;
                        ghHotkeyMouseDownQpc = 0;
                        ClearPhysicalHoldDownVk(ghHotkeyVk);
                        ghHotkeyNeedKeyUp = false;
                        ghHotkeyPending = false;
                        ReplayShortPressHoldKey(ghHotkeyVk);
                        return 1;
                    }
                } else if (down) {
                    if (globalActive) {
                        if (IsHoldRearmBlocked(ghHotkeyVk)) return 1;
                        SetPhysicalHoldDownVk(ghHotkeyVk);
                        return 1;
                    }
                    if (globalArmed) {
                        if (IsHoldRearmBlocked(ghHotkeyVk)) return 1;
                        SetPhysicalHoldDownVk(ghHotkeyVk);
                        if (ghHotkeyPending || ghHotkeyHandling) return 1;
                        if (HoldElapsedMs(ghHotkeyMouseDownQpc) >= CurrentHoldThresholdMs()
                            && CheckHotkeyModifiers(ghHotkeyMods, !busy)
                            && !IsHoldSeqInvalid(ghHotkeyHoldSeq)) {
                            if (DedicatedHoldSessionActive()) {
                                return 1;
                            }
                            const uint32_t seq = ghHotkeyHoldSeq;
                            CancelHoldThresholdTimer();
                            ghHotkeyNeedKeyUp = false;
                            ghHotkeyMouseHoldArmed = false;
                            ghHotkeyMouseHoldDown = true;
                            ghHotkeyPending = true;
                            ghHotkeyHoldPostedSeq = seq;
                            SetActiveHoldHotkeyId(HOTKEY_GLOBAL_ID);
                            PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStart, 0);
                        }
                        return 1;
                    }
                    if (!CheckHotkeyModifiers(ghHotkeyMods, !busy)) {
                        // 修饰键不符：原样放行
                    } else if (IsHoldRearmBlocked(ghHotkeyVk)) {
                        // 刚松手禁止再武装：不吞键，放行给目标程序，避免打字丢键
                    } else if (ghHotkeyPending || ghHotkeyHandling) {
                        return 1;
                    } else {
                        ghHotkeyNeedKeyUp = false;
                        ghHotkeyMouseDownTick = GetTickCount();
                        ghHotkeyMouseDownQpc = HoldQpcNow();
                        ghHotkeyMouseHoldArmed = true;
                        ghHotkeyMouseHoldDown = false;
                        ghHotkeyHoldSeq = NextHoldSeq();
                        SetPhysicalHoldDownVk(ghHotkeyVk);
                        ScheduleHoldThresholdTimer(CurrentHoldThresholdMs());
                        return 1; // 首击起吞键
                    }
                }
            }

            if (ghHotkeyHwnd && (down || up)) {
                for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                    auto& h = ghPlaybackScriptHooks[i];
                    if (!h.holdMode || !h.vk || h.vk != vkCode || IsMouseVk(h.vk)) continue;
                    if (up) {
                        if (h.holdArmed) {
                            CancelHoldThresholdTimer();
                            h.holdInvalidSeq = h.holdSeq;
                            h.holdArmed = false;
                            h.holdDownTick = 0;
                            h.holdDownQpc = 0;
                            ClearPhysicalHoldDownVk(h.vk);
                            ReplayShortPressHoldKey(h.vk);
                            return 1;
                        }
                        continue;
                    }
                    if (!down) break;
                    if (h.holdActive || IsActiveHoldHotkeyId(h.hotkeyId)) {
                        if (IsHoldRearmBlocked(h.vk)) return 1;
                        SetPhysicalHoldDownVk(h.vk);
                        return 1;
                    }
                    if (!CheckHotkeyModifiers(h.mods, !busy)) continue;
                    // 忙碌 / 再武装冷却期间不再吞键：原样放行，避免长按脚本结束后
                    // 打字（尤其同键）被吞成「bian→ban/bani」、输入变迟缓。
                    if (busy) continue;
                    if (IsHoldRearmBlocked(h.vk)) continue;
                    SetPhysicalHoldDownVk(h.vk);
                    if (h.holdArmed) {
                        if (HoldElapsedMs(h.holdDownQpc) >= CurrentHoldThresholdMs()
                            && h.holdSeq != 0 && h.holdSeq != h.holdInvalidSeq) {
                            CancelHoldThresholdTimer();
                            h.holdArmed = false;
                            h.holdActive = true;
                            SetActiveHoldHotkeyId(h.hotkeyId);
                            PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStart,
                                static_cast<LPARAM>(h.hotkeyId));
                        }
                        return 1;
                    }
                    h.holdDownTick = GetTickCount();
                    h.holdDownQpc = HoldQpcNow();
                    h.holdArmed = true;
                    h.holdActive = false;
                    h.holdExtended = (ks->flags & LLKHF_EXTENDED) != 0;
                    h.holdSeq = NextHoldSeq();
                    ScheduleHoldThresholdTimer(CurrentHoldThresholdMs());
                    return 1;
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        // ── 非长按热键：原逻辑（单击启停等）────────────────────────────────
        // 忙碌时用户按下必须能停：MatchesKey 会把「脚本刚注入的同 VK」当成回声。
        // 启动键仍按着（NeedKeyUp）时 Windows 会自动连发 KEYDOWN，不得当成停止；
        // 真丢 KEYUP 由 16ms SyncHotkeyLatches + 松手后再按的轮询兜底。
        // 远控 SendInput 带 LLKHF_INJECTED 但无 ExtraInfo，必须当作用户键，否则能开不能停。
        const bool userToggle = !hotkey_stop::IsScriptTaggedInjection(taggedSynthetic);
        if (userToggle && up) {
            ClearScriptToggleNeedKeyUpForVk(vkCode);
            ClearToggleConsumedForVk(vkCode);
        }
        if (userToggle && ghHotkeyEnabled && !IsMouseVk(ghHotkeyVk)
            && vkCode == ghHotkeyVk) {
            if (up && hotkey_stop::ShouldClearToggleLatchOnKeyUp(injected, taggedSynthetic)) {
                ghHotkeyPending = false;
                ghHotkeyNeedKeyUp = false;
            }
            if (down && hotkey_stop::ShouldStopOnToggleKeyDown(
                    busy, ghHotkeyNeedKeyUp, injected, taggedSynthetic)) {
                if (!passToApp && CheckHotkeyModifiers(ghHotkeyMods, false)) {
                    ghHotkeyPending = true;
                    ghHotkeyNeedKeyUp = true;
                    RequestEmergencyStop();
                    return 1;
                }
            }
        }

        // 脚本/录制单击热键：忙碌停也必须在 MatchesKey 之外，否则同 VK 注入回声会吞掉停止。
        if (userToggle && busy && down && ghHotkeyHwnd && !passToApp) {
            for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                auto& h = ghPlaybackScriptHooks[i];
                if (!h.vk || IsMouseVk(h.vk) || vkCode != h.vk || h.holdMode) continue;
                if (!hotkey_stop::ShouldStopOnToggleKeyDown(
                        busy, h.toggleNeedKeyUp, injected, taggedSynthetic)) continue;
                if (!CheckHotkeyModifiers(h.mods, false)) continue;
                h.toggleNeedKeyUp = true;
                RequestEmergencyStop();
                return 1;
            }
        }

        if (userToggle && !synthetic_input::MatchesKey(
                vkCode,
                static_cast<unsigned short>(ks->scanCode),
                extended,
                down)) {
            if (ghHotkeyEnabled && !IsMouseVk(ghHotkeyVk) && vkCode == ghHotkeyVk) {
                // 单击切换（非按住启停）
                if (up) {
                    ghHotkeyPending = false;
                    ghHotkeyNeedKeyUp = false;
                }
                if (down) {
                    if (passToApp) {
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    if (busy) {
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    if (!hotkey_stop::ShouldStartOnToggleKeyDown(
                            busy, ghHotkeyNeedKeyUp, ghHotkeyPending, ghHotkeyHandling,
                            injected, taggedSynthetic)) {
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    // RegisterHotKey 注册失败（被其它程序占用等）时不再放行：
                    // 由本钩子兜底触发（吞键，避免组合键落到前台窗口）。
                    const bool registerHotkeyActive = !passMode && !ghPlaybackHotkeySuspended
                        && !IsRegFailId(HOTKEY_GLOBAL_ID);
                    if (registerHotkeyActive) {
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    if (CheckHotkeyModifiers(ghHotkeyMods, !busy)) {
                        ghHotkeyPending = true;
                        PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, 0, 0);
                        return 1;
                    }
                }
            }

            if (ghHotkeyHwnd && (down || up)) {
                for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                    auto& h = ghPlaybackScriptHooks[i];
                    if (!h.vk || IsMouseVk(h.vk) || vkCode != h.vk) continue;
                    if (h.holdMode) continue; // 长按已在上方处理
                    if (!CheckHotkeyModifiers(h.mods, !busy)) continue;
                    const bool viaHook = scriptViaHook || IsRegFailId(h.hotkeyId) || busy;
                    if (!viaHook || !down) continue;
                    if (passToApp) {
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    if (busy) {
                        // 忙碌停已在 MatchesKey 外处理；此处不再投递 WM_HOTKEY，避免停完又开。
                        continue;
                    }
                    h.toggleNeedKeyUp = true;
                    PostMessageW(ghHotkeyHwnd, WM_HOTKEY, static_cast<WPARAM>(h.hotkeyId), 0);
                    if (passMode || ghPlaybackHotkeySuspended || IsRegFailId(h.hotkeyId)) return 1;
                    break;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

inline LRESULT HotkeyKbProcCatchCpp(int code, WPARAM wp, LPARAM lp) {
    try {
        return HotkeyKbProcBody(code, wp, lp);
    } catch (...) {
        HotkeySehBreadcrumb("LLKB_cxx", 0xE06D7363u);
        return CallNextHookEx(nullptr, code, wp, lp);
    }
}

inline LRESULT CALLBACK HotkeyKbProc(int code, WPARAM wp, LPARAM lp) {
    __try {
        return HotkeyKbProcCatchCpp(code, wp, lp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        HotkeySehBreadcrumb("LLKB", GetExceptionCode());
        return CallNextHookEx(nullptr, code, wp, lp);
    }
}

inline LRESULT HotkeyMouseProcBody(int code, WPARAM wp, LPARAM lp) {
    input_emergency::LlHookGuard llGuard;
    if (code < 0) return CallNextHookEx(nullptr, code, wp, lp);
    if (!lp) return CallNextHookEx(nullptr, code, wp, lp);
    ghHotkeyMouseLastEventTick = GetTickCount();

    auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
    bool down = false, up = false; UINT btnVk = 0;
    if      (wp == WM_LBUTTONDOWN) { down = true; btnVk = VK_LBUTTON; }
    else if (wp == WM_LBUTTONUP)   { up   = true; btnVk = VK_LBUTTON; }
    else if (wp == WM_RBUTTONDOWN) { down = true; btnVk = VK_RBUTTON; }
    else if (wp == WM_RBUTTONUP)   { up   = true; btnVk = VK_RBUTTON; }
    else if (wp == WM_MBUTTONDOWN) { down = true; btnVk = VK_MBUTTON; }
    else if (wp == WM_MBUTTONUP)   { up   = true; btnVk = VK_MBUTTON; }
    else if (wp == WM_XBUTTONDOWN || wp == WM_XBUTTONUP) {
        btnVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        if (wp == WM_XBUTTONDOWN) down = true; else up = true;
    }
    if (!btnVk) return CallNextHookEx(nullptr, code, wp, lp);

    const bool busy = ghHotkeySessionBusy.load(std::memory_order_relaxed);
    if (ghHotkeyCaptureOpen) {
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (UiStartHotkeysMuted() && !busy) {
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    const bool injected = (ms->flags & LLMHF_INJECTED) != 0;
    const bool taggedSynthetic = synthetic_input::IsSyntheticExtraInfo(ms->dwExtraInfo);
    const bool syntheticBtn = synthetic_input::MatchesMouseButton(btnVk, down);

    // 忙碌时用户鼠标热键必须能停，不被指纹回声挡住；启动键未抬起则忽略。
    // 远控 SendInput 带 LLMHF_INJECTED 但无 ExtraInfo，与键盘同一条规则。
    if (down && ghHotkeyEnabled && IsMouseVk(ghHotkeyVk) && btnVk == ghHotkeyVk
        && !NeedsHoldHotkey()
        && hotkey_stop::ShouldStopOnToggleKeyDown(
            busy, ghHotkeyNeedKeyUp, injected, taggedSynthetic)) {
        if (CheckHotkeyModifiers(ghHotkeyMods, false)) {
            ghHotkeyPending = true;
            ghHotkeyNeedKeyUp = true;
            RequestEmergencyStop();
            return 1;
        }
    }

    // 仅 ExtraInfo 标签一律放行（本进程脚本注入）。MatchesMouseButton 在连点时会持续命中同键，
    // 不能单独挡住「按住即停」的真人/远控抬起；VHID 注入无 ExtraInfo，指纹仍要挡，真人改走 Raw。
    const bool vhidActive = ForegroundInputRouter::Instance().IsHidActive()
        && ForegroundInputRouter::Instance().ActiveBackend()
            == quickscript::ForegroundInputBackend::VirtualHid;
    if (hotkey_stop::IsScriptTaggedInjection(taggedSynthetic)) {
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (syntheticBtn && !(up && MouseHoldShouldAcceptPhysicalUp(btnVk) && !vhidActive)) {
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (up) ClearToggleConsumedForVk(btnVk);
    const bool passMode = ghPassThroughTypingHotkeys.load(std::memory_order_relaxed);
    const bool passToApp = passMode && !busy && IsChineseImeActiveForHotkeyPass();
    const bool scriptViaHook = ghPlaybackHotkeySuspended || passMode;

    // ── 全局鼠标热键 ──────────────────────────────────────────────
    if (ghHotkeyEnabled && IsMouseVk(ghHotkeyVk) && btnVk == ghHotkeyVk) {
            const bool globalMouseHold = NeedsHoldHotkey();
            if (up && btnVk == ghHotkeyVk) {
                if (globalMouseHold) {
                    const bool wasArmed = ghHotkeyMouseHoldArmed;
                    const bool wasActive = ghHotkeyMouseHoldDown
                        || IsActiveHoldHotkeyId(HOTKEY_GLOBAL_ID);
                    const uint32_t seq = ghHotkeyHoldSeq;
                    if (wasArmed && !wasActive) CancelHoldThresholdTimer();
                    ghHotkeyMouseHoldArmed = false;
                    ghHotkeyMouseDownTick = 0;
                    ghHotkeyMouseDownQpc = 0;
                    ghHotkeyMouseHoldDown = false;
                    // 未达长按阈值的短按：取消，不启停
                    if (wasArmed && !wasActive) {
                        InvalidateHoldSeq(seq);
                        ghHotkeyNeedKeyUp = false;
                        ghHotkeyPending = false;
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    // 已触发连点：松开即停（仅信任非注入物理抬起）
                    if (wasActive) {
                        ghHotkeyNeedKeyUp = false;
                        if (!DedicatedHoldSessionActive()) {
                            ClearActiveHoldHotkeyId();
                            BlockHoldRearm(ghHotkeyVk);
                            if (ghHotkeyHandling || ghHotkeyPending) {
                                PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStop, 0);
                            } else {
                                ghHotkeyPending = true;
                                PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStop, 0);
                            }
                            return 1;
                        }
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    ghHotkeyNeedKeyUp = false;
                    ghHotkeyPending = false;
                } else {
                    ghHotkeyPending = false;
                    ghHotkeyNeedKeyUp = false;
                }
            }
            if (down && btnVk == ghHotkeyVk) {
                if (ghHotkeyPending || ghHotkeyHandling) {
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                if (globalMouseHold) {
                    // 武装长按，达阈值后由 Timer Queue / Poll 启动
                    if (IsHoldRearmBlocked(btnVk)) {
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    if (ghHotkeyNeedKeyUp) {
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    if (ghHotkeyMouseHoldDown || IsActiveHoldHotkeyId(HOTKEY_GLOBAL_ID)) {
                        return 1;
                    }
                    if (!CheckHotkeyModifiers(ghHotkeyMods, !busy)) {
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    ghHotkeyMouseDownTick = GetTickCount();
                    ghHotkeyMouseDownQpc = HoldQpcNow();
                    ghHotkeyMouseHoldArmed = true;
                    ghHotkeyMouseHoldDown = false;
                    ghHotkeyHoldSeq = NextHoldSeq();
                    ScheduleHoldThresholdTimer(CurrentHoldThresholdMs());
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                if (ghHotkeyNeedKeyUp) {
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                if (CheckHotkeyModifiers(ghHotkeyMods, !busy)) {
                    ghHotkeyPending = true;
                    PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, 0, 0);
                }
            }
    }

    // ── 脚本/录制鼠标热键 ─────────────────────────────────────────
    // 长按：始终走 LL（不注册 RegisterHotKey）。单击：回放挂起 / IME 放行时走 LL。
    if (ghHotkeyHwnd && (down || up)) {
        for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
            auto& h = ghPlaybackScriptHooks[i];
            if (!h.vk || !IsMouseVk(h.vk) || h.vk != btnVk) continue;
            if (up && !h.holdMode) {
                h.toggleNeedKeyUp = false;
                continue;
            }
            if (h.holdMode) {
                if (up) {
                    if (h.holdActive || IsActiveHoldHotkeyId(h.hotkeyId)) {
                        h.holdArmed = false;
                        h.holdActive = false;
                        h.holdDownTick = 0;
                        h.holdDownQpc = 0;
                        ClearActiveHoldHotkeyId();
                        BlockHoldRearm(h.vk);
                        PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStop,
                            static_cast<LPARAM>(h.hotkeyId));
                        return 1;
                    }
                    if (h.holdArmed) {
                        CancelHoldThresholdTimer();
                        h.holdInvalidSeq = h.holdSeq;
                        h.holdArmed = false;
                        h.holdDownTick = 0;
                        h.holdDownQpc = 0;
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    continue;
                }
                if (!down) continue;
                if (IsHoldRearmBlocked(h.vk)) return CallNextHookEx(nullptr, code, wp, lp);
                if (h.holdActive || IsActiveHoldHotkeyId(h.hotkeyId)) return 1;
                if (busy) return CallNextHookEx(nullptr, code, wp, lp);
                if (!CheckHotkeyModifiers(h.mods, !busy)) continue;
                if (passToApp) return CallNextHookEx(nullptr, code, wp, lp);
                SetPhysicalHoldDownVk(h.vk);
                if (h.holdArmed) {
                    if (HoldElapsedMs(h.holdDownQpc) >= CurrentHoldThresholdMs()
                        && h.holdSeq != 0 && h.holdSeq != h.holdInvalidSeq) {
                        CancelHoldThresholdTimer();
                        h.holdArmed = false;
                        h.holdActive = true;
                        SetActiveHoldHotkeyId(h.hotkeyId);
                        PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStart,
                            static_cast<LPARAM>(h.hotkeyId));
                    }
                    return 1;
                }
                h.holdDownTick = GetTickCount();
                h.holdDownQpc = HoldQpcNow();
                h.holdArmed = true;
                h.holdActive = false;
                h.holdSeq = NextHoldSeq();
                ScheduleHoldThresholdTimer(CurrentHoldThresholdMs());
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            if (!scriptViaHook || !down) continue;
            if (passToApp) return CallNextHookEx(nullptr, code, wp, lp);
            if (!CheckHotkeyModifiers(h.mods, !busy)) continue;
            if (busy) {
                if (!hotkey_stop::ShouldStopOnToggleKeyDown(
                        busy, h.toggleNeedKeyUp, injected, taggedSynthetic)) {
                    continue;
                }
                h.toggleNeedKeyUp = true;
                RequestEmergencyStop();
                return 1;
            }
            h.toggleNeedKeyUp = true;
            PostMessageW(ghHotkeyHwnd, WM_HOTKEY, static_cast<WPARAM>(h.hotkeyId), 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

inline LRESULT HotkeyMouseProcCatchCpp(int code, WPARAM wp, LPARAM lp) {
    try {
        return HotkeyMouseProcBody(code, wp, lp);
    } catch (...) {
        HotkeySehBreadcrumb("LLMS_cxx", 0xE06D7363u);
        return CallNextHookEx(nullptr, code, wp, lp);
    }
}

inline LRESULT CALLBACK HotkeyMouseProc(int code, WPARAM wp, LPARAM lp) {
    __try {
        return HotkeyMouseProcCatchCpp(code, wp, lp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        HotkeySehBreadcrumb("LLMS", GetExceptionCode());
        return CallNextHookEx(nullptr, code, wp, lp);
    }
}

class EngineHost {
public:
    /// WebView 壳宿主：隐藏 GDI 主窗，托盘/可见 UI 由壳负责；热键/执行/录制仍走本窗。
    void SetHeadlessUi(bool enabled) { headlessUi_ = enabled; }
    bool IsHeadlessUi() const { return headlessUi_; }
    void SetWebViewHostHwnd(HWND hwnd) { webViewHostHwnd_ = hwnd; }
    HWND Hwnd() const { return hwnd_; }

    void EngineStopRun() { StopRun(); }
    bool EngineIsRunning() const { return running_.load() || extRunPending_.load(); }
    bool EngineIsBreakoutPaused() const {
        return breakoutPaused_.load(std::memory_order_relaxed);
    }
    /// 编辑器调试：从指定动作开始单次执行（不受宏执行次数设置影响）。
    bool EngineDebugRunActions(const std::vector<ScriptAction>& actions, int startIndex,
        bool stepMode, const std::vector<int>& breakpoints,
        const Hotkey& debugHotkey, const std::wstring& displayName,
        const windowmode::WindowModeScriptConfig& wmCfg, std::wstring& err);
    bool EngineIsDebugging() const { return debugMode_.load(std::memory_order_relaxed); }
    bool EngineDebugPaused() const {
        return debugMode_.load(std::memory_order_relaxed)
            && debugPaused_.load(std::memory_order_relaxed);
    }
    bool EngineDebugStepMode() const {
        return debugMode_.load(std::memory_order_relaxed)
            && debugStepMode_.load(std::memory_order_relaxed);
    }
    int EngineExecutedSteps() const { return executedSteps_.load(std::memory_order_relaxed); }
    void EnginePlaybackProgress(int& current, int& total) const {
        current = playbackActionIndex_.load(std::memory_order_relaxed);
        total = playbackActionTotal_.load(std::memory_order_relaxed);
    }
    std::wstring EngineRunningScriptName() const {
        std::lock_guard<std::mutex> lock(extScriptStateMu_);
        return runningScriptName_;
    }
    /// 正在运行脚本的模式：0=默认 / 1=窗口模式 / 2=后台窗口（非运行时为 0）。
    int EngineRunningMode() const {
        std::lock_guard<std::mutex> lock(extScriptStateMu_);
        if (!running_.load(std::memory_order_relaxed) && !extRunPending_.load(std::memory_order_relaxed)) {
            return 0;
        }
        if (!runningWindowMode_.enabled) return 0;
        return runningWindowMode_.executionKind
                == windowmode::WindowModeExecutionKind::BackgroundWindow
            ? 2
            : 1;
    }
    bool EngineRunningWindowMode(windowmode::WindowModeScriptConfig& out) const {
        std::lock_guard<std::mutex> lock(extScriptStateMu_);
        if (!running_.load(std::memory_order_relaxed) && !extRunPending_.load(std::memory_order_relaxed)) {
            return false;
        }
        out = runningWindowMode_;
        return true;
    }

    void EngineStartRecording() { StartRecording(); }
    void EngineStopRecording() { StopRecording(); }
    bool EngineIsRecording() const { return recording_; }
    void EngineApplyRecorderWindowMode(bool on) {
        recorderWindowMode_ = on;
        appSettings_.home.recorderWindowMode = on ? 1 : 0;
    }
    bool EngineStartRecordingEx(std::wstring& err) {
        if (recording_) return true;
        StartRecording();
        if (!recording_) {
            err = lastRecordingError_.empty() ? L"录制启动失败" : lastRecordingError_;
            return false;
        }
        return true;
    }
    bool EngineStopRecordingEx(std::wstring& savedPath, int& actionCount, std::wstring& err) {
        savedPath.clear();
        actionCount = 0;
        if (!recording_) {
            err = L"当前未在录制";
            return false;
        }
        StopRecording();
        actionCount = static_cast<int>(actions_.size());
        if (!lastSavedRecordingPath_.empty()) {
            savedPath = lastSavedRecordingPath_;
            return true;
        }
        if (actionCount <= 0) {
            err = L"未录制到有效动作或未保存文件";
            return false;
        }
        err = L"录制结束但保存失败";
        return false;
    }

    void EngineStartClicking() { StartClicking(); }
    void EngineStopClicking() { StopClicking(); }
    void EngineToggleClicker() { ToggleClicker(); }
    bool EngineIsClicking() const { return clicking_; }
    void EngineApplyClickerSettings(const quickscript::ClickerSettings& s) { clickerSettings_ = s; }

    bool EngineRunFromPath(const std::wstring& path, std::wstring& err) {
        err.clear();
        if (running_) {
            err = L"busy";
            return false;
        }
        std::wstring resolved;
        if (!ResolveLibraryScriptPath(path, resolved)) {
            err = L"script not found";
            return false;
        }
        const ScriptFileData data = LoadScriptFileData(resolved, false);
        if (data.actions.empty()) {
            err = L"脚本无有效动作";
            return false;
        }
        windowmode::WindowModeScriptConfig wmCfg = data.windowMode;
        bool anyRel = wmCfg.windowRelativeCoordinates;
        for (const auto& a : data.actions) {
            if (a.windowRelative) { anyRel = true; break; }
        }
        windowmode::FinalizeWindowModeForPlayback(wmCfg, anyRel, IsRecordingScriptPath(resolved));
        if (!ResolveWindowModeSelectMethod(wmCfg)) {
            err = L"窗口模式未就绪或已取消";
            return false;
        }
        CoordMeta execMeta = ScriptCoordMetaForExecution(data.coordMeta);
        std::vector<ScriptAction> execActions =
            PrepareScriptActionsForExecution(data.actions, execMeta);
        if (IsRecordingScriptPath(resolved) || ScriptIsTimedInputSequence(execActions))
            RepairCompressedRelativeGaps(execActions);
        const double breakoutTime = EffectiveBreakoutTimeSeconds(data);
        StartActionsWorker(execActions, resolved, wmCfg, execMeta, breakoutTime, data.hotkey);
        return running_;
    }

    void EngineSetActiveHomeTab(int tab) {
        using quickscript::MainTab;
        MainTab t = MainTab::Clicker;
        if (tab == 1) t = MainTab::Recorder;
        else if (tab == 2) t = MainTab::Macro;
        else if (tab == 3) t = MainTab::ScriptCustom;
        SetActiveHomeTab(t);
    }

    void EngineSelectHomeItem(int tab, const std::wstring& path) {
        using quickscript::MainTab;
        // Web 列表点选极频繁：禁止每次 LoadScripts/LoadRecordings（读盘+数动作）和
        // RegisterAllHotkeys（卸装全部热键）。选中只改索引；热键表随脚本增删/改热键刷新。
        auto findRecording = [&]() -> int {
            if (path.empty()) return -1;
            for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
                if (_wcsicmp(recordings_[static_cast<size_t>(i)].path.c_str(), path.c_str()) == 0)
                    return i;
            }
            return -1;
        };
        auto findScript = [&]() -> int {
            if (path.empty()) return -1;
            for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
                if (_wcsicmp(scripts_[static_cast<size_t>(i)].path.c_str(), path.c_str()) == 0)
                    return i;
            }
            return -1;
        };
        if (tab == 1) {
            const int want = path.empty() ? -1 : findRecording();
            bool reloaded = false;
            if (want < 0 && !path.empty()) {
                LoadRecordings();
                reloaded = true;
            }
            const int idx = path.empty() ? -1 : findRecording();
            if (activeHomeTab_ == MainTab::Recorder && selectedRecording_ == idx && selectedScript_ < 0) {
                if (reloaded) RegisterAllHotkeys();
                return;
            }
            SetActiveHomeTab(MainTab::Recorder);
            selectedRecording_ = idx;
            selectedScript_ = -1;
            if (reloaded) RegisterAllHotkeys();
            EngineSaveHomeStateLite();
            return;
        }
        if (tab == 2) {
            bool reloaded = false;
            if (!path.empty() && findScript() < 0) {
                LoadScripts();
                reloaded = true;
            }
            const int idx = path.empty() ? -1 : findScript();
            if (activeHomeTab_ == MainTab::Macro && selectedScript_ == idx && selectedRecording_ < 0) {
                if (reloaded) RegisterAllHotkeys();
                return;
            }
            SetActiveHomeTab(MainTab::Macro);
            selectedScript_ = idx;
            selectedRecording_ = -1;
            if (reloaded) RegisterAllHotkeys();
            EngineSaveHomeStateLite();
            return;
        }
        // 连点 / 脚本定制：切 Tab 即可；清掉脚本选中，避免 SaveHomeState 把 activeTab 盖回宏
        const MainTab wantTab = (tab == 3) ? MainTab::ScriptCustom : MainTab::Clicker;
        if (activeHomeTab_ == wantTab && selectedScript_ < 0 && selectedRecording_ < 0)
            return;
        SetActiveHomeTab(wantTab);
        selectedScript_ = -1;
        selectedRecording_ = -1;
        EngineSaveHomeStateLite();
    }

    void EngineSaveHomeState();
    void EngineSaveHomeStateLite();
    void ScheduleHomeStatePersist();
    void FlushHomeStatePersist();

    std::string EngineGetHomeStateJson() const {
        std::wstring scriptPath;
        std::wstring recordingPath;
        if (selectedScript_ >= 0 && selectedScript_ < static_cast<int>(scripts_.size()))
            scriptPath = scripts_[static_cast<size_t>(selectedScript_)].path;
        if (selectedRecording_ >= 0 && selectedRecording_ < static_cast<int>(recordings_.size()))
            recordingPath = recordings_[static_cast<size_t>(selectedRecording_)].path;
        auto esc = [](const std::wstring& w) {
            std::string s = ToUtf8(w);
            std::string o;
            o.reserve(s.size() + 8);
            for (unsigned char c : s) {
                if (c == '"') o += "\\\"";
                else if (c == '\\') o += "\\\\";
                else if (c == '\n') o += "\\n";
                else if (c == '\r') o += "\\r";
                else if (c == '\t') o += "\\t";
                else if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else o += static_cast<char>(c);
            }
            return o;
        };
        return std::string("{\"activeTab\":") + std::to_string(static_cast<int>(activeHomeTab_))
            + ",\"selectedScriptPath\":\"" + esc(scriptPath)
            + "\",\"selectedRecordingPath\":\"" + esc(recordingPath) + "\"}";
    }

    void EngineBeginHotkeyCaptureRelease(const Hotkey& editing) { BeginHotkeyCaptureRelease(editing); }
    void EngineBeginActionKeyCaptureRelease() { BeginActionKeyCaptureRelease(); }
    void EngineEndHotkeyCaptureRelease() { EndHotkeyCaptureRelease(); }
    void EngineDebugWindowClosedByUser() { OnDebugWindowClosedByUser(); }

    /// 壳 UI 模式：0=主页，1=宏编辑，2=录制优化。编辑/优化界面打开时静默全部启停热键，
    /// 注销 RegisterHotKey 并放行 LL 钩子（不吞键不触发）；回主页后全量重注册。
    void EngineSetUiMode(int mode) {
        shellUiMode_ = mode;
        const bool muted = (mode == 1 || mode == 2);
        const bool prevMuted = ghUiModeHotkeysMuted.exchange(muted, std::memory_order_relaxed);
        if (muted) {
            if (prevMuted) return;
            ClearAllHoldSessionLatches();
            ghHotkeyNeedKeyUp = false;
            ghHotkeyPending = false;
            ghHotkeyMouseHoldArmed = false;
            ghHotkeyMouseHoldDown = false;
            // 脚本/录制长按热键若正处于按住状态，一并解除，避免回主页后残留闩锁误触发
            for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                auto& h = ghPlaybackScriptHooks[i];
                h.holdArmed = false;
                h.holdActive = false;
                h.holdDownTick = 0;
                h.holdDownQpc = 0;
            }
            if (hwnd_) {
                UnregisterHotKey(hwnd_, HOTKEY_GLOBAL_ID);
                for (int i = 0; i < 100; ++i) UnregisterHotKey(hwnd_, HOTKEY_SCRIPT_BASE + i);
                for (int i = 0; i < 100; ++i) UnregisterHotKey(hwnd_, HOTKEY_RECORDING_BASE + i);
            }
            return;
        }
        // 主页：丢弃「调试结束再静音」的待办，避免回主页后热键被再次掐死
        debugRestoreUiMute_ = false;
        // 主页：每次都重注册（即使 prev 已是 unmuted）。
        // 否则「曾 mute 卸键 / RegisterHotKey 丢失」后 F8 会彻底无响应。
        ghHotkeyNeedKeyUp = false;
        ghHotkeyPending = false;
        RegisterAllHotkeys();
    }

    /// 主页显式武装热键（桥 setActiveHomeTab / 启动后调用）
    void EngineEnsureHotkeysArmed() {
        debugRestoreUiMute_ = false;
        ghUiModeHotkeysMuted.store(false, std::memory_order_relaxed);
        ghHotkeyNeedKeyUp = false;
        ghHotkeyPending = false;
        RegisterAllHotkeys();
        InstallGlobalHotkeyHooks();
    }

    /// source: 0=主壳 WebView，1=助手窗。输入框聚焦时卸掉键盘 RegisterHotKey，避免搜/聊时触发脚本。
    void EngineSetTypingHotkeysMuted(bool muted, int source) {
        const uint32_t bit = (source != 0) ? 2u : 1u;
        uint32_t prev = ghUiTypingMuteBits.load(std::memory_order_relaxed);
        uint32_t next = 0;
        do {
            next = muted ? (prev | bit) : (prev & ~bit);
        } while (!ghUiTypingMuteBits.compare_exchange_weak(
            prev, next, std::memory_order_relaxed, std::memory_order_relaxed));
        if ((prev == 0) == (next == 0)) return;
        if (ghUiModeHotkeysMuted.load(std::memory_order_relaxed)) return;
        ghHotkeyNeedKeyUp = false;
        ghHotkeyPending = false;
        RegisterAllHotkeys();
    }

    void EngineApplyGlobalHotkeyFromSettings() {
        Hotkey hk{};
        hk.text = appSettings_.home.globalHotkeyText.empty()
            ? L"F8" : appSettings_.home.globalHotkeyText;
        hk.vk = static_cast<UINT>(appSettings_.home.globalHotkeyVk
            ? appSettings_.home.globalHotkeyVk : VK_F8);
        hk.modifiers = static_cast<UINT>(appSettings_.home.globalHotkeyModifiers);
        hk.holdMode = appSettings_.home.globalHotkeyHold;
        hk.enabled = hk.vk != 0;
        globalHotkey_ = hk;
        RegisterAllHotkeys();
    }

    void EngineReloadSettings() {
        quickscript::AppSettings loaded;
        if (TryLoadAppSettings(loaded)) appSettings_ = std::move(loaded);
        quickscript::ApplyThemeFromSettings(appSettings_);
        clickerSettings_.button = static_cast<quickscript::MouseButtonChoice>(
            std::clamp(appSettings_.home.clickerButton, 0, 2));
        clickerSettings_.intervalMode = static_cast<quickscript::ClickIntervalMode>(
            std::clamp(appSettings_.home.clickerIntervalMode, 0, 2));
        clickerSettings_.customIntervalSeconds = appSettings_.home.clickerCustomInterval;
        if (clickerSettings_.customIntervalSeconds <= 0) clickerSettings_.customIntervalSeconds = 0.1;
        recorderSettings_.captureScope = quickscript::RecordCaptureScope::Global;
        recorderSettings_.inputMode = static_cast<quickscript::RecorderInputMode>(
            std::clamp(appSettings_.home.recorderInputMode, 0, 3));
        recorderWindowMode_ = appSettings_.home.recorderWindowMode != 0;
        appSettings_.home.recorderCaptureScope = 1;
        ApplyDebugWindowSetting();
        ApplyOtherOsSettings();
        EngineApplyGlobalHotkeyFromSettings();
    }

    void EngineReloadScriptsAndHotkeys() { RefreshScriptLibraryUi(); }

    void EngineOpenScheduledTasks() {
        if (headlessUi_) return;
#if (QST_GDI_LEGACY == 1)
        ShowScheduledTaskDialog();
#endif
    }

    void EngineReloadScheduledTasks() { scheduledTasks_.Reload(); }
    void EngineTouchScheduledIntervalClock(const std::wstring& id) {
        scheduledTasks_.TouchIntervalClock(id);
    }
    void EngineTickScheduledTasks() { scheduledTasks_.Tick(); }

    Hotkey EngineGlobalHotkey() const { return globalHotkey_; }

    void EngineSetGlobalHotkey(const Hotkey& hk) {
        if (hk.enabled && hk.vk) {
            std::wstring conflict;
            if (HotkeyChordConflicts(hk.vk, hk.modifiers, L"", true, conflict)) {
                ShowPromptInfo(conflict);
                return;
            }
        }
        globalHotkey_ = hk;
        globalHotkey_.enabled = globalHotkey_.vk != 0;
        appSettings_.home.globalHotkeyText = globalHotkey_.text;
        appSettings_.home.globalHotkeyVk = static_cast<int>(globalHotkey_.vk);
        appSettings_.home.globalHotkeyModifiers = static_cast<int>(globalHotkey_.modifiers);
        appSettings_.home.globalHotkeyHold = globalHotkey_.holdMode;
        SaveAppSettingsPreserveUserSettings(appSettings_, true);
        RegisterAllHotkeys();
    }

    bool EngineHotkeyChordConflicts(UINT vk, UINT modifiers, const std::wstring& excludePath,
        bool excludeGlobal, std::wstring& errOut) const {
        return HotkeyChordConflicts(vk, modifiers, excludePath, excludeGlobal, errOut);
    }


    bool Create() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &EngineHost::WndProc;
        wc.hInstance = g_instance;
        wc.lpszClassName = L"KeyMouseEngineWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadAppIcon();
        wc.hIconSm = LoadAppIconSmall();
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int x = (screenW - UiHomeWidth()) / 2;
        int y = (screenH - UiHomeHeight()) / 2;
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"键鼠工坊", WS_POPUP | WS_MINIMIZEBOX, x, y, UiHomeWidth(), UiHomeHeight(), nullptr, nullptr, g_instance, this);
        if (hwnd_) ApplyWindowIcons(hwnd_);
        return hwnd_ != nullptr;
    }

    void Show(int nCmdShow) {
        ShowWindow(hwnd_, nCmdShow);
        UpdateWindow(hwnd_);
        // 托盘须在窗口创建/显示之后添加；WM_CREATE 里 NIM_ADD 常失败
        EnsureTrayIcon();
    }

    LRESULT RouteEditorDropPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT RouteEditorTipPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT RouteClickerDropPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    friend LRESULT CALLBACK EditorDropPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK EditorTipPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK ClickerDropPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    enum class Page { Home, Editor };
    // 注意：自 kMoveX 起的序号必须与 editor_param_layout.h 的 EID_* 对齐。
    // 勿在 kMoveX 前插入新 Id，否则找图测试/预览等 WM_COMMAND 会错位。
    enum Id { kScriptName = 1001, kModeCombo, kActionCombo, kAdd, kModify, kClear, kSave, kCancel, kLoad, kBatchExit, kBatchSelectAll, kBatchDeselect, kBatchDelete, kBatchCopy, kMoveX, kMoveY, kMoveRandomX, kMoveRandomY, kMoveFromVar, kMoveVarX, kMoveVarY, kClickButton, kClickCount, kClickWait, kClickRandom, kWaitDuration, kWaitRandom, kRemark, kListRemarkEdit, kClose, kKeyCapture, kClickLWin, kClickRWin, kClickLCtrl, kClickRCtrl, kClickLAlt, kClickRAlt, kClickLShift, kClickRShift, kKeyLWin, kKeyRWin, kKeyLCtrl, kKeyRCtrl, kKeyLAlt, kKeyRAlt, kKeyLShift, kKeyRShift, kCrosshair, kLoopCount, kLoopFromVar, kLoopVarExpr, kLoopVarName, kDefineBlockName, kRunBlockCombo, kKeyPressCapture, kMousePressButton, kMousePressLWin, kMousePressRWin, kMousePressLCtrl, kMousePressRCtrl, kMousePressLAlt, kMousePressRAlt, kMousePressLShift, kMousePressRShift, kKeyPressLWin, kKeyPressRWin, kKeyPressLCtrl, kKeyPressRCtrl, kKeyPressLAlt, kKeyPressRAlt, kKeyPressLShift, kKeyPressRShift, kHotkeyShortcutCombo, kHotkeyShortcutCount, kHotkeyShortcutWait, kHotkeyShortcutRandom, kQuickInputText, kQuickInputVarCombo, kQuickInputInsert, kQuickInputCharInterval, kQuickInputCount, kQuickInputWait, kQuickInputRandom, kRunMacroCombo, kMousePlaybackCombo, kMousePlaybackCount, kMousePlaybackWait, kMousePlaybackRandom, kScrollVertical, kScrollHorizontal, kScrollSteps, kScrollDirection, kScrollCount, kScrollWait, kScrollRandom, kFindFullScreen, kFindSelectRegion, kFindX1, kFindY1, kFindX2, kFindY2, kFindTest, kFindScreenshot, kFindLocalImage, kFindClearImage, kFindImagePreview, kFindMatchThreshold, kFindScaleMin, kFindScaleMax, kFindFollowUp, kFindOffsetX, kFindOffsetY, kFindSelectOffset, kFindUntilFound, kFindMatchVar, kOcrFullScreen, kOcrSelectRegion, kOcrX1, kOcrY1, kOcrX2, kOcrY2, kOcrResultMode, kOcrSearchText, kOcrSearchVarCombo, kOcrSearchVarInsert, kOcrFollowUp, kOcrOffsetX, kOcrOffsetY, kOcrSelectOffset, kOcrUntilFound, kOcrResultVar, kOcrTest, kOcrInstallDep, kOcrRegionByImage, kOcrFindSelectRegion, kOcrFindScreenshot, kOcrFindLocalImage, kOcrFindClearImage, kOcrFindImagePreview, kOcrFindMatchThreshold, kOcrFindScaleMin, kOcrFindScaleMax, kOcrDigitsOnly, kIfVarCombo, kIfOperator, kIfValue, kIfConnector, kIfAddCondition, kIfConditionList, kRunProgramCombo, kRunProgramPath, kRunProgramBrowse, kRunProgramCrosshair, kRunProgramArgs, kCloseProgramPath, kCloseProgramBrowse, kCloseProgramCrosshair, kCloseProgramMatchFileName, kOpenWebpageUrl, kOpenFilePath, kOpenFileBrowse, kTimerVarName, kAiPrompt, kAiInsertVar, kAiVarCombo, kAiModel, kAiContextMode, kAiOutputVar, kAiOutputType, kAiTimeout, kAiFallback, kAiImageScale, kAiRegionByImage, kAiRegionByImage2, kAiFindSelectRegion, kAiFindMatchThreshold, kAiFindScaleMin, kAiFindScaleMax, kAiTargetPreview, kAiTargetScreenshot, kAiTargetLocal, kAiTargetClear, kAiFullScreen, kAiSelectRegion, kAiSearchRegion, kAiSearchX1, kAiSearchY1, kAiSearchX2, kAiSearchY2, kAiMaxSteps, kAiWithImage, kAiConfirm, kAiMaxStepsHint, kCursorPosVarName, kGotoStepExpr, kMoveRelX, kMoveRelY, kMoveRelRandomX, kMoveRelRandomY, kBreakoutTime = 5099, kWmSelectMethod = 5101, kWmSpecifyWindowBtn, kWmTargetPath, kWmTargetBrowse, kWmTargetCrosshair, kWmFakeFocus };
    enum class HoverButton { None, Import, Export, Load, Clear, Add, Modify, Cancel, Save, Close, Minimize, Settings, HomeCard, HomeScroll, EditorScroll, Create, CommonHotkey, HomeEdit, HomeDelete, ScriptHotkey, Row, RowCopy, RowDelete, RowCheckbox, BatchExit, BatchSelectAll, BatchDeselect, BatchDelete, BatchCopy, Crosshair, ClickerInterval, ClickerHotkey, RecorderHotkey };
    enum MenuId { kCopyLast = 3001, kCopyFirst, kCopyBeforeSelected, kCopyAfterSelected, kAddLast, kAddFirst, kAddBeforeSelected, kAddAfterSelected, kAddAsChild, kHotCustom = 3101, kHotF8, kHotF10, kHotLeft, kHotMiddle, kHotRight, kHotX1, kHotX2, kHotSpace };
    struct HotkeyMenuItem { int id; const wchar_t* title; const wchar_t* desc; };
    struct EditorControlLayout { HWND hwnd = nullptr; RECT base{}; };
    struct PopupCombo { bool open = false; std::vector<std::wstring> items; int sel = 0; };
    struct ModifierHolds { HWND lWin = nullptr; HWND rWin = nullptr; HWND lCtrl = nullptr; HWND rCtrl = nullptr; HWND lAlt = nullptr; HWND rAlt = nullptr; HWND lShift = nullptr; HWND rShift = nullptr; };
    enum class QuickInputTipKind { None, TextExample, VariableHelp };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        EngineHost* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<EngineHost*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else self = reinterpret_cast<EngineHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        if (crosshairDrag_.IsActive()) {
            if (msg == WM_HOTKEY) { OnHotkey(static_cast<int>(wp)); return 0; }
            if (crosshairDrag_.HandleMessage(msg, wp, lp,
                [this](int x, int y) {
                    if (moveX_) SetText(moveX_, std::to_wstring(x));
                    if (moveY_) SetText(moveY_, std::to_wstring(y));
                },
                nullptr)) {
                return 0;
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
        switch (msg) {
        case WM_CREATE: Init(); return 0;
        case WM_PAINT: Paint(); return 0;
        case WM_COMMAND: OnCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp)); return 0;
        case WM_DRAWITEM: DrawOwnerItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
        case WM_MEASUREITEM: MeasureOwnerItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp)); return TRUE;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSELEAVE: OnMouseLeave(); return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt{}; GetCursorPos(&pt); ScreenToClient(hwnd_, &pt);
                if (page_ == Page::Editor && PtInRemark(pt.x, pt.y)) SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                else if (HitGrayButton(pt.x, pt.y)) SetCursor(LoadCursorW(nullptr, IDC_HAND));
                else SetCursor(LoadCursorW(nullptr, IsClickablePoint(pt.x, pt.y) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_LBUTTONDOWN: OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONUP: OnMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSEWHEEL: OnWheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
        // 顶层下拉弹层不会随 owner 自动位移，拖动/移动时必须按锚点重算屏幕坐标。
        case WM_MOVE:
            SyncOwnedDropPopups();
            promptModal_.OnOwnerResize();
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_WINDOWPOSCHANGED: {
            const auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
            if (pos && !(pos->flags & SWP_NOMOVE)) {
                TrySyncUiScaleIfDisplayChanged();
            }
            if (page_ == Page::Editor && IsWindowVisible(hwnd_) && pos
                && !(pos->flags & SWP_NOMOVE)) {
                ApplyParamLayerMasks();
            }
            SyncOwnedDropPopups();
            promptModal_.OnOwnerResize();
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
        case WM_SHOWWINDOW:
            if (!wp) {
                CloseEditorPopup();
                CloseClickerDropPopup();
                CancelQuickInputTip();
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_DPICHANGED: OnDpiChanged(wp, lp); return 0;
        case WM_DISPLAYCHANGE: RequestUiScaleSync(); return 0;
        case WM_SETTINGCHANGE:
            if (lp) {
                const wchar_t* section = reinterpret_cast<LPCWSTR>(lp);
                if (lstrcmpiW(section, L"Display") == 0
                    || lstrcmpiW(section, L"WindowMetrics") == 0
                    || lstrcmpiW(section, L"Desktop") == 0) {
                    RequestUiScaleSync();
                    return 0;
                }
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                CloseEditorPopup();
                CloseClickerDropPopup();
                CancelQuickInputTip();
            } else {
                if (page_ == Page::Home) UpdateClickerLayout();
                SyncOwnedDropPopups();
            }
            promptModal_.OnOwnerResize();
            if (wp != SIZE_MINIMIZED) InvalidateRect(hwnd_, nullptr, TRUE);
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE
                && !trayMenuOpen_
                && !breakoutTaskbarTransition_
                && breakoutPaused_.load(std::memory_order_relaxed)
                && !breakoutUiVisibleOnScreen_
                && breakoutTaskbarShown_) {
                PostMessageW(hwnd_, WM_APP_RESTORE_INSTANCE, 0, 0);
            } else if (LOWORD(wp) == WA_INACTIVE) {
                HWND fg = GetForegroundWindow();
                if (fg != hwnd_ && fg != editorDropPopup_ && fg != editorTipPopup_ && fg != clickerDropPopup_) {
                    CloseEditorPopup();
                    CloseClickerDropPopup();
                    CancelQuickInputTip();
                    // 窗口失焦时取消拖拽状态，避免拖拽卡死
                    if (crosshairDrag_.IsActive()) crosshairDrag_.End();
                    if (dragging_) {
                        dragging_ = false;
                        dragIndex_ = -1;
                        ReleaseCapture();
                    }
                }
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_ACTIVATEAPP:
            if (wp && !trayMenuOpen_
                && !breakoutTaskbarTransition_
                && breakoutPaused_.load(std::memory_order_relaxed)
                && !breakoutUiVisibleOnScreen_
                && breakoutTaskbarShown_
                && (GetForegroundWindow() == hwnd_ || GetActiveWindow() == hwnd_)) {
                RestoreBreakoutWindowToScreen();
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_SYSCOMMAND:
            if (!trayMenuOpen_
                && !breakoutTaskbarTransition_
                && breakoutPaused_.load(std::memory_order_relaxed)) {
                const UINT cmd = wp & 0xFFF0;
                if (cmd == SC_RESTORE) {
                    RestoreBreakoutWindowToScreen();
                    return 0;
                }
                if (cmd == SC_MINIMIZE && breakoutUiVisibleOnScreen_
                    && breakoutTaskbarShown_) {
                    MinimizeBreakoutWindowForUser();
                    return 0;
                }
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_SETFOCUS:
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_TIMER:
            if (wp == kHotkeyLatchSyncTimerId) {
                SyncHotkeyLatches();
                PollHoldHotkeys();
                return 0;
            }
            if (wp == kImeHotkeyPassTimerId) {
                RefreshImeHotkeyPassCache();
                return 0;
            }
            if (wp == kHotkeyHookWatchdogTimerId) {
                TickHotkeyHookWatchdog();
                return 0;
            }
            if (wp == kHomeStatePersistTimerId) {
                KillTimer(hwnd_, kHomeStatePersistTimerId);
                try { PersistHomeStateJsonOnly(); } catch (...) {}
                return 0;
            }
            if (wp == kDisplaySyncTimerId) {
                SyncUiScaleLayout();
                if (displaySyncPass_ < 4) {
                    ++displaySyncPass_;
                    SetTimer(hwnd_, kDisplaySyncTimerId, 200, nullptr);
                } else {
                    displaySyncPass_ = 0;
                    KillTimer(hwnd_, kDisplaySyncTimerId);
                }
                return 0;
            }
            if (wp == kHoverTimerId) { UpdateHoverFromCursor(); return 0; }
            if (wp == kQuickInputTipTimerId) { OnQuickInputTipTimer(); return 0; }
            if (wp == kScheduledTaskTimerId) { scheduledTasks_.Tick(); return 0; }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_APP_SCHEDULED_TICK:
            scheduledTasks_.Tick();
            return 0;
        case WM_APP_HOTKEY_WATCHDOG:
            TickHotkeyHookWatchdog();
            return 0;
        case WM_HOLD_THRESHOLD_FIRE:
            // Timer Queue 精确定时：隐藏窗 WM_TIMER 常被 coalescing 推迟
            PollHoldHotkeys();
            return 0;
        case WM_HOTKEY: OnHotkey(static_cast<int>(wp)); return 0;
        case WM_INPUT: {
            // 看门狗参照：Raw 输入（RIDEV_INPUTSINK）只要有键盘/鼠标事件就刷新
            // 对应 tick；LL 钩子被静默卸载后，此处仍在流而 LL 无事件，即触发重装。
            {
                RAWINPUTHEADER rh{};
                UINT rhSize = sizeof(rh);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_HEADER, &rh, &rhSize,
                        sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
                    if (rh.dwType == RIM_TYPEKEYBOARD) ghRawKbLastInputTick = GetTickCount();
                    else if (rh.dwType == RIM_TYPEMOUSE) ghRawMouseLastInputTick = GetTickCount();
                }
            }
            // VirtualHid：按设备区分自家注入 vs 真人键鼠（脱离 + 长按抬起）。
            // 即使未开脱离，也要吃 Raw：补 VHID 键盘指纹，并处理真人 KEYUP 停长按。
            const bool monitorBreakout = breakout_input::BreakoutShouldMonitor()
                && ForegroundInputRouter::Instance().IsHidActive()
                && ForegroundInputRouter::Instance().ActiveBackend()
                    == quickscript::ForegroundInputBackend::VirtualHid;
            const auto hint = synthetic_input::OnRawInput(
                reinterpret_cast<HRAWINPUT>(lp), monitorBreakout);
            if (hint.physicalKeyDown) {
                NoteRawHotkeyVk(hint.vk, true);
                NotePhysicalHoldKeyDown(hint.vk);
            }
            // VirtualHid 同键注入在 LL 无 INJECTED/ExtraInfo，Expect 会被连点刷满；
            // 真人松手改认 Raw 设备 KEYUP（Interception 同键常回灌真键盘 Raw，仍只走 LL）。
            if (hint.physicalKeyUp && hint.vk != 0) {
                NoteRawHotkeyVk(hint.vk, false);
                ClearToggleConsumedForVk(hint.vk);
                const bool vhid = ForegroundInputRouter::Instance().IsHidActive()
                    && ForegroundInputRouter::Instance().ActiveBackend()
                        == quickscript::ForegroundInputBackend::VirtualHid;
                if (vhid || !ForegroundInputRouter::Instance().IsHidActive()) {
                    NotePhysicalHoldKeyUp(hint.vk);
                }
            }
            if (hint.userBreakout
                && !breakout_input::BreakoutShouldIgnoreInput(hint.msg, hint.vk)) {
                const bool holdDown = hint.vk != 0 && (hint.physicalKeyDown
                    || hint.msg == WM_LBUTTONDOWN || hint.msg == WM_RBUTTONDOWN
                    || hint.msg == WM_MBUTTONDOWN || hint.msg == WM_XBUTTONDOWN);
                if (holdDown) {
                    breakout_input::BreakoutNoteUserHold(hint.vk);
                }
                breakout_input::BreakoutSignalUserInput();
            }
            if (hint.physicalKeyUp && hint.vk != 0) {
                breakout_input::BreakoutNoteUserRelease(hint.vk);
            }
            if (hint.physicalButtonUp && hint.vk != 0) {
                // 连点左键按住即停：LL 会被 MatchesMouseButton 误伤，改认 Raw 真人抬起。
                // Interception 注入可能与真鼠标同设备且 ExtraInfo 不一定回灌 Raw，松手仍走 LL。
                const bool interception = ForegroundInputRouter::Instance().IsHidActive()
                    && ForegroundInputRouter::Instance().ActiveBackend()
                        == quickscript::ForegroundInputBackend::Interception;
                if (!interception
                    && hotkey_stop::ShouldTreatRawMouseUpAsPhysical(hint.fromOurDevice, false)) {
                    NotePhysicalHoldKeyUp(hint.vk);
                }
                // physicalButtonUp 在未开脱离时也会上报；注入抬起可能仍带指纹。
                // 脱离计数只在监控中、且非指纹回声时更新，避免把用户仍按住的键洗成已松开。
                if (breakout_input::BreakoutShouldMonitor()
                    && !synthetic_input::MatchesMouseButton(hint.vk, false)) {
                    breakout_input::BreakoutNoteUserRelease(hint.vk);
                }
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
        case WM_GLOBAL_HOTKEY_DETECTED: {
            const int id = lp ? static_cast<int>(lp) : HOTKEY_GLOBAL_ID;
            OnHotkey(id, static_cast<int>(wp));
            return 0;
        }
        case WM_GETICON:
            if (breakoutPaused_.load(std::memory_order_relaxed)) {
                const UINT which = static_cast<UINT>(wp);
                if (which == ICON_BIG) {
                    return reinterpret_cast<LRESULT>(LoadBreakoutPauseIcon());
                }
                if (which == ICON_SMALL) {
                    return reinterpret_cast<LRESULT>(LoadBreakoutPauseIconSmall());
                }
                if (which == ICON_SMALL2) {
                    return reinterpret_cast<LRESULT>(LoadBreakoutPauseIcon());
                }
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_RUN_DONE: OnRunDone(); return 0;
        case WM_APP_RUN_SCHEDULED_PENDING:
            TryStartPendingScheduled();
            return 0;
        case WM_APP_EXT_RUN_SCRIPT: {
            auto* path = reinterpret_cast<std::wstring*>(lp);
            if (path) {
                RunActionsFromPath(*path);
                delete path;
            }
            extRunPending_ = false;
            return 0;
        }
        case WM_APP_EXT_STOP_SCRIPT:
            StopRun();
            return 0;
        case WM_APP_LOGIC_CONVERT_DONE: {
            // 磁盘脚本已由 worker 写回；编辑器列表立即重载（runner 用的是启动时副本，不受影响）
            if (!currentPath_.empty()) {
                const std::wstring path = currentPath_;
                LoadScriptFile(path);
                if (qst::desktop_tools::MacroDebug().IsCreated()) {
                    qst::desktop_tools::MacroDebug().AppendLog(
                        L"逻辑转化：已刷新编辑器动作列表");
                }
            }
            return 0;
        }
        case WM_APP_BREAKOUT_UI: UpdateBreakoutPauseIcons(); return 0;
        case WM_APP_RESTORE_INSTANCE: RestoreMainWindowForUser(); return 0;
        case WM_APP_EDITOR_FINISH_OPEN:
            FinishDeferredEditorOpen(static_cast<int>(wp));
            return 0;
        case WM_APP_EDITOR_PARSE_MORE:
            ContinueEditorProgressiveParse(static_cast<int>(wp));
            return 0;
        case WM_APP_HOME_REFRESH_LISTS:
            if (page_ == Page::Home) {
                // wp!=0：从编辑页返回——主页已揭开，再做表单复位/图片清理（勿挡首帧）
                if (wp) {
                    ResetActionFormSession(true);
                    CleanupNewImages();
                }
                LoadScripts();
                LoadRecordings();
                if (selectedScript_ >= static_cast<int>(scripts_.size())) selectedScript_ = -1;
                if (selectedRecording_ >= static_cast<int>(recordings_.size())) selectedRecording_ = -1;
                ClampHomeScroll();
                ClampRecordingScroll();
                RegisterAllHotkeys();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_FIND_TEST_DONE: OnFindTestDone(static_cast<int>(wp), static_cast<int>(lp)); return 0;
        case WM_OPEN_AGENT_DIALOG:
#if (QST_GDI_LEGACY == 1)
            OpenAgentDialog();
#endif
            return 0;
        case WM_AGENT_SCRIPT_LIBRARY_CHANGED: RefreshScriptLibraryUi(); return 0;
        case WM_APP_UI_SCALE_SYNC:
            SyncUiScaleLayout();
            if (lp) {
                displaySyncPass_ = 1;
                SetTimer(hwnd_, kDisplaySyncTimerId, 200, nullptr);
            }
            return 0;
        case WM_EDITOR_PARAM_CHROME:
            if (page_ == Page::Editor) RepaintParamPanelChrome();
            return 0;
        case WM_OCR_SUBPANEL_REFRESH:
            ocrSubPanelRefreshPosted_ = false;
            if (page_ == Page::Editor && popupAction_.sel == 18)
                RefreshOcrSubPanel();
            UnlockParamViewportRedraw();
            return 0;
        case WM_APP_PROMPT:
            if (!promptPendingMessage_.empty()) {
                std::wstring promptMsg = std::move(promptPendingMessage_);
                promptPendingMessage_.clear();
                if (headlessUi_) {
                    NotifyHeadlessUserMessage(promptMsg);
                } else {
                    CloseEditorPopup();
                    CloseClickerDropPopup();
                    if (promptModal_.visible()) promptModal_.Close();
                    promptModal_.OnOwnerResize();
                    promptModal_.ShowInfo(promptMsg);
                }
            }
            return 0;
        case WM_TRAY: return OnTrayMessage(lp);
        case WM_CTLCOLORSTATIC:
            return OnCtlColorStatic(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
        case WM_CTLCOLOREDIT: return OnEditColor(reinterpret_cast<HDC>(wp));
        case WM_CLOSE:
            // 关闭到托盘（设置项）：仅隐藏，进程继续跑。真正退出靠托盘「退出」。
            // 注意：此处不要 Uninstall*Hooks / DestroyWindow，那些路径会卡成幽灵进程。
            if (page_ == Page::Home && appSettings_.other.closeToTray && !clicking_ && !recording_) {
                if (running_) {
                    try { SaveHomeState(); } catch (...) {}
                    CloseWindowDuringRun();
                    return 0;
                }
                try { SaveHomeState(); } catch (...) {}
                HideToTray();
                return 0;
            }
            QuitApplication();
            return 0;
        case WM_DESTROY:
            // 正常不应走到这里（QuitApplication 直接 TerminateProcess）。
            stopFlag_ = true;
            clicking_ = false;
            running_ = false;
            RemoveTrayIcon();
            PostQuitMessage(0);
            TerminateProcess(GetCurrentProcess(), 0);
        case WM_APP_QUIT_APP:
            QuitApplication();
            return 0;
        case WM_APP_ENSURE_TRAY:
            EnsureTrayIcon();
            return 0;
        case WM_APP_DEFER_EXT_BRIDGE:
            HotkeyDiagLog("engine: deferred StartExtBridgeAlwaysOn");
            // AV（如 VDA）不能靠 C++ catch；Body 单独函数 + 本处仅 __try
            {
                struct Guard {
                    static void RunBody(EngineHost* self) {
                        self->StartExtBridgeAlwaysOn();
                    }
                    static bool Run(EngineHost* self) {
                        __try {
                            RunBody(self);
                            return true;
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                            HotkeySehBreadcrumb("EXTBR", GetExceptionCode());
                            return false;
                        }
                    }
                };
                try {
                    if (Guard::Run(this)) {
                        HotkeyDiagLog("engine: deferred StartExtBridgeAlwaysOn done");
                    } else {
                        HotkeyDiagLog("engine: deferred StartExtBridgeAlwaysOn SEH swallowed");
                    }
                } catch (...) {
                    HotkeyDiagLog("engine: deferred StartExtBridgeAlwaysOn C++ exception");
                }
            }
            return 0;
        case WM_QUERYENDSESSION: if (recording_) StopRecording(); return TRUE;
        case WM_ENDSESSION: if (recording_) StopRecordingCleanup(); return 0;
        default:
            if (wmTaskbarCreated_ && msg == wmTaskbarCreated_) {
                // Explorer 重启后托盘图标会丢，强制重新 NIM_ADD
                trayActive_ = false;
                EnsureTrayIcon();
                return 0;
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
    }

    // ── Initialization & cleanup ────────────────────────────────────
    void CreateUiFonts() {
        font_ = CreateFontW(UiFontHeight(23), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        editorFont_ = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        bigFont_ = CreateFontW(UiFontHeight(39), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        titleFont_ = CreateFontW(UiFontHeight(22), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        hotFont_ = CreateFontW(UiFontHeight(25), 0, 0, 0, FW_BOLD, FALSE, TRUE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        closeFont_ = CreateFontW(UiFontHeight(32), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        homeFont_ = CreateFontW(UiFontHeight(25), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        homeTabFont_ = CreateFontW(UiFontHeight(28), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    }

    void DestroyUiFonts() {
        auto del = [](HFONT& f) {
            if (f) {
                DeleteObject(f);
                f = nullptr;
            }
        };
        del(font_);
        del(editorFont_);
        del(bigFont_);
        del(titleFont_);
        del(hotFont_);
        del(closeFont_);
        del(homeFont_);
        del(homeTabFont_);
    }

    void RecreateUiFonts() {
        DestroyUiFonts();
        CreateUiFonts();
        editorFontsApplied_ = false;
        editorFontsAppliedTo_ = nullptr;
        editorScaleAppliedPct_ = -1;
        ApplyFont(hwnd_, font_);
    }

    void ApplyDpiLayout() {
        UpdateClickerLayout();
        if (page_ == Page::Editor) {
            ApplyEditorControlScale(true);
        }
        if (!editorControls_.empty()) {
            editorFontsApplied_ = false;
            ApplyEditorFonts();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void RefreshOpenAgentDialogs() {
#if (QST_GDI_LEGACY == 1)
        for (auto& d : agentDialogs_) {
            if (d && d->IsAlive()) d->ApplyDpiLayout();
        }
#endif
    }

    void RecreateThemeBrushes() {
        if (lineGreenBrush_) { DeleteObject(lineGreenBrush_); lineGreenBrush_ = nullptr; }
        lineGreenBrush_ = CreateSolidBrush(kLineGreen);
    }

    /// 主题 id 已写入 settings / CurrentTheme 后，立刻刷新主窗与已打开的附属窗
    void ApplyThemeAndRefreshUi() {
        quickscript::ApplyThemeFromSettings(appSettings_);
        RecreateThemeBrushes();
        RedrawWindow(hwnd_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
#if (QST_GDI_LEGACY == 1)
        for (auto& d : agentDialogs_) {
            if (!d || !d->IsAlive()) continue;
            RedrawWindow(d->Hwnd(), nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        if (HWND opt = RecordingOptimizeDialog::ActiveHwnd()) {
            RedrawWindow(opt, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
#endif
        if (qst::desktop_tools::MacroDebug().IsCreated() && IsWindow(qst::desktop_tools::MacroDebug().Hwnd())) {
            RedrawWindow(qst::desktop_tools::MacroDebug().Hwnd(), nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
    }

    void SyncUiScaleLayout() {
        const int oldPct = UiScalePercent();
        UiScaleInitFromHwnd(hwnd_);
        const bool scaleChanged = (UiScalePercent() != oldPct);
        lastUiScalePercent_ = UiScalePercent();
        lastUiMonitor_ = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monInfo{};
        monInfo.cbSize = sizeof(monInfo);
        if (lastUiMonitor_ && GetMonitorInfoW(lastUiMonitor_, &monInfo)) {
            lastUiScreenW_ = monInfo.rcMonitor.right - monInfo.rcMonitor.left;
            lastUiScreenH_ = monInfo.rcMonitor.bottom - monInfo.rcMonitor.top;
        }
        RecreateUiFonts();
        ApplyDpiLayout();
        const int targetW = page_ == Page::Home ? UiHomeWidth() : UiEditorWidth();
        const int targetH = page_ == Page::Home ? UiHomeHeight() : UiEditorHeight();
        UiResizeWindowClient(hwnd_, targetW, targetH, scaleChanged);
        RefreshOpenAgentDialogs();
        NotifyActiveSettingsDialogRelayout();
        RedrawWindow(hwnd_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void RequestUiScaleSync() {
        PostMessageW(hwnd_, WM_APP_UI_SCALE_SYNC, 0, 1);
    }

    void TrySyncUiScaleIfDisplayChanged() {
        const int oldPct = lastUiScalePercent_;
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monInfo{};
        monInfo.cbSize = sizeof(monInfo);
        if (!mon || !GetMonitorInfoW(mon, &monInfo)) return;
        const int sw = monInfo.rcMonitor.right - monInfo.rcMonitor.left;
        const int sh = monInfo.rcMonitor.bottom - monInfo.rcMonitor.top;
        UiScaleInitFromHwnd(hwnd_);
        if (mon != lastUiMonitor_ || sw != lastUiScreenW_ || sh != lastUiScreenH_
            || UiScalePercent() != oldPct) {
            SyncUiScaleLayout();
        }
    }

    void OnDpiChanged(WPARAM /*wp*/, LPARAM /*lp*/) {
        SyncUiScaleLayout();
    }

    void Init() {
        InitCommonControls();
        EnsureScriptsDir();
        UiScaleInitFromHwnd(hwnd_);
        CreateUiFonts();
        UpdateClickerLayout();
        whiteBrush_ = CreateSolidBrush(kWhite);
        panelBrush_ = CreateSolidBrush(kPanel);
        lineGreenBrush_ = CreateSolidBrush(kLineGreen);
        crosshairDragCursor_ = CreateCrosshairDragCursor(kCrosshairBlue);
        // E1.1：headless 产品路径不创建 GDI 编辑器/主页下拉 HWND（Web 已接管）
        if (!headlessUi_) {
            CreateEditorControls();
            CaptureEditorControlLayout();
            AttachEditorChildSubclass();
            ApplyFont(hwnd_, font_);
            ShowWindow(mode_, SW_HIDE);
            ShowWindow(actionCombo_, SW_HIDE);
            ShowWindow(mousePressButton_, SW_HIDE);
            ShowWindow(clickButton_, SW_HIDE);
            ShowWindow(loopTypeCombo_, SW_HIDE);
            ShowWindow(runBlockCombo_, SW_HIDE);
            ShowWindow(hotkeyShortcutCombo_, SW_HIDE);
            ShowWindow(quickInputVarCombo_, SW_HIDE);
            ShowWindow(runMacroCombo_, SW_HIDE);
            ShowWindow(mousePlaybackCombo_, SW_HIDE);
            InitHotkeyShortcutPresets();
        } else if (font_) {
            ApplyFont(hwnd_, font_);
        }
        LoadScripts();
        LoadRecordings();
        LoadAgentConversations();
        LoadAppSettings(appSettings_);
        recorderWindowMode_ = appSettings_.home.recorderWindowMode != 0;
        quickscript::ApplyThemeFromSettings(appSettings_);
        ApplyDebugWindowSetting();
        ApplyOtherOsSettings();
        CleanOrphanImages();  // 启动时清理孤立图片
        if (!headlessUi_) {
            CreateEditorDropPopup();
            CreateClickerDropPopup();
            CreateEditorTipPopup();
        }
        page_ = Page::Home;
        if (!headlessUi_) ShowEditorControls(false);
        RestoreHomeState();   // 恢复上次退出时的界面状态
        EngineApplyGlobalHotkeyFromSettings();
        InvalidateRect(hwnd_, nullptr, TRUE);
        HotkeyDiagLog("engine Init: RegisterAllHotkeys begin");
        RegisterAllHotkeys();
        HotkeyDiagLog("engine Init: InstallGlobalHotkeyHooks begin");
        InstallGlobalHotkeyHooks();
        HotkeyDiagLog("engine Init: hooks installed");
        // VirtualHid 脱离/热键：Raw Input 按 VID_5153&PID_5648 识别自家设备
        synthetic_input::RegisterRawInputSink(hwnd_);
        scheduledTasks_.Reload();
        scheduledTasks_.SetRunCallback([this](const std::wstring& path) { OnScheduledTaskFire(path); });
        SetTimer(hwnd_, kScheduledTaskTimerId, 1000, nullptr);
        StartScheduledTaskQueueTimer();
        // 延迟到 Show()/WM_APP_ENSURE_TRAY：WM_CREATE 阶段 NIM_ADD 不可靠
        PostMessageW(hwnd_, WM_APP_ENSURE_TRAY, 0, 0);
        SetAgentUiNotifyHwnd(hwnd_);
        if (!headlessUi_) {
            promptModal_.Bind(hwnd_, font_, [this] {
                if (page_ == Page::Editor) ApplyParamLayerMasks();
            });
        }
        outerShadow_.Attach(hwnd_);
        SyncUiScaleLayout();
        if (!headlessUi_ && page_ == Page::Home) ShowEditorControls(false);
        // 扩展桥 + VDA 预热延后：WM_CREATE 内调用 VirtualDesktopAccessor 在部分机器上会 AV
        // 导致引擎窗创建失败、壳永远起不来。延后到消息泵后再跑。
        HotkeyDiagLog("engine Init: defer ext bridge");
        PostMessageW(hwnd_, WM_APP_DEFER_EXT_BRIDGE, 0, 0);
        HotkeyDiagLog("engine Init: done");
    }

    /// 进程启动后常开本机桥（扩展顶栏弹窗 / 窗口模式共用）。失败只打日志。
    void StartExtBridgeAlwaysOn() {
        using namespace windowmode;
        ExtScriptApiHandlers handlers;
        handlers.listScripts = [this]() {
            std::vector<ExtScriptInfo> out;
            EnsureScriptsDir();
            std::vector<ScriptFileEntry> files;
            EnumerateScriptJsonFiles(ScriptsDir(), files);
            for (const auto& fe : files) {
                ExtScriptInfo info;
                info.file = ToUtf8(fe.fileName);
                info.path = ToUtf8(fe.path);
                const auto content = ReadAll(fe.path);
                std::wstring name = ExtractString(content, L"scriptName");
                if (name.empty()) name = fe.fileName;
                info.name = ToUtf8(name);
                const auto wm = windowmode::ParseWindowModeJson(content);
                info.windowModeEnabled = wm.enabled;
                info.windowName = ToUtf8(wm.windowName.empty()
                    ? wm.targetWindowTitle : wm.windowName);
                info.windowClassName = ToUtf8(wm.windowClassName);
                {
                    const auto resolved = windowmode::ResolveInputStrategy(wm);
                    if (resolved == windowmode::WindowModeInputStrategy::Cdp) {
                        info.inputStrategy = "cdp";
                    } else if (resolved == windowmode::WindowModeInputStrategy::SoftMessage) {
                        info.inputStrategy = "softMessage";
                    } else {
                        info.inputStrategy = "auto";
                    }
                }
                out.push_back(std::move(info));
            }
            return out;
        };
        handlers.runScript = [this](const std::string& pathOrFileUtf8, std::string& err) -> bool {
            if (running_.load()) {
                err = "busy";
                return false;
            }
            if (extRunPending_.exchange(true)) {
                err = "busy";
                return false;
            }
            std::wstring resolved;
            if (!ResolveExtScriptPath(pathOrFileUtf8, resolved, err)) {
                extRunPending_ = false;
                return false;
            }
            auto* heap = new std::wstring(std::move(resolved));
            if (!PostMessageW(hwnd_, WM_APP_EXT_RUN_SCRIPT, 0, reinterpret_cast<LPARAM>(heap))) {
                delete heap;
                extRunPending_ = false;
                err = "post_failed";
                return false;
            }
            return true;
        };
        handlers.stopScript = [this]() {
            PostMessageW(hwnd_, WM_APP_EXT_STOP_SCRIPT, 0, 0);
        };
        handlers.getRunState = [this]() {
            ExtRunState st;
            st.running = running_.load() || extRunPending_.load();
            std::lock_guard<std::mutex> lock(extScriptStateMu_);
            st.currentScript = ToUtf8(runningScriptName_);
            return st;
        };
        ExtBridgeServer::Instance().SetScriptApiHandlers(std::move(handlers));
        std::wstring bridgeErr;
        HotkeyDiagLog("engine: ExtBridge Start begin");
        if (!ExtBridgeServer::Instance().Start(bridgeErr)) {
            windowmode::WindowModeLogf(L"[扩展桥] 常开启动失败: %s",
                bridgeErr.empty() ? L"(unknown)" : bridgeErr.c_str());
            HotkeyDiagLog("engine: ExtBridge Start failed");
        } else {
            windowmode::WindowModeLogEventf(L"[扩展桥] 常开监听 port=%d",
                ExtBridgeServer::Instance().Port());
            HotkeyDiagLog("engine: ExtBridge Start ok");
        }
        // softMessage/假焦点/CDP 窗口模式都可能用到「鼠标宏」；预热只查找不创建。
        HotkeyDiagLog("engine: VDA Warmup begin");
        windowmode::MacroVirtualDesktop::WarmupAtProcessStart();
        HotkeyDiagLog("engine: VDA Warmup end");
    }

    bool ResolveExtScriptPath(const std::string& pathOrFileUtf8, std::wstring& outPath,
        std::string& err) const {
        if (pathOrFileUtf8.empty()) {
            err = "bad_path";
            return false;
        }
        if (!ResolveLibraryScriptPath(FromUtf8(pathOrFileUtf8), outPath)) {
            err = "bad_path";
            return false;
        }
        return true;
    }



    // ── Editor control creation ────────────────────────────────────
    void CreateEditorControls();

    void AddEditorControl(HWND h) { editorControls_.push_back(h); }
    void AddGroup(std::vector<HWND>& group, HWND h) { group.push_back(h); editorControls_.push_back(h); }

    RECT ParamViewportRect() const {
        return ParamScrollContentRect();
    }

    void MoveParamAware(HWND h, int x, int y, int w, int hgt, BOOL repaint = FALSE) const {
        if (!h) return;
        if (paramViewport_ && GetParent(h) == paramViewport_) {
            InvalidateParamControlInViewport(h);
            const RECT vp = ParamViewportRect();
            MoveWindow(h, x - vp.left, y - vp.top, w, hgt, repaint);
            if (IsGrayButton(h)) {
                RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
            }
        } else {
            MoveWindow(h, x, y, w, hgt, repaint);
        }
    }

    void SetParamPosAware(HWND h, int x, int y, int w, int hgt, UINT flags) const {
        if (!h) return;
        if (paramViewport_ && GetParent(h) == paramViewport_) {
            InvalidateParamControlInViewport(h);
            const RECT vp = ParamViewportRect();
            SetWindowPos(h, nullptr, x - vp.left, y - vp.top, w, hgt, flags);
        } else {
            SetWindowPos(h, nullptr, x, y, w, hgt, flags);
        }
    }

    void AttachToParamViewport(HWND h) const {
        if (!h || !paramViewport_ || h == paramViewport_) return;
        RECT rc = WindowClientRect(h);
        if (GetParent(h) != paramViewport_) {
            SetParent(h, paramViewport_);
            const RECT vp = ParamViewportRect();
            MoveWindow(h, rc.left - vp.left, rc.top - vp.top, rc.right - rc.left, rc.bottom - rc.top, FALSE);
        }
        SetWindowSubclass(h, EditorChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(const_cast<EngineHost*>(this)));
        auto* self = const_cast<EngineHost*>(this);
        if (IsMarkedParamCheckbox(h)) {
            LONG style = GetWindowLongW(h, GWL_STYLE);
            if ((style & BS_TYPEMASK) != BS_OWNERDRAW) {
                SetWindowLongW(h, GWL_STYLE, (style & ~BS_TYPEMASK) | BS_OWNERDRAW | BS_AUTOCHECKBOX);
            }
        }
        if (self->IsGrayButton(h)) {
            LONG style = GetWindowLongW(h, GWL_STYLE);
            if (style & WS_TABSTOP) {
                SetWindowLongW(h, GWL_STYLE, style & ~WS_TABSTOP);
            }
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }

    // 使用新布局系统构建参数面板布局
    // group: 输出参数，布局中的 HWND 会被添加到该 group
    // idx: 布局索引 (对应 popupAction_.sel 的值)
    UILayoutResult BuildAndStoreLayout(UILayout layout, std::vector<HWND>& group, int idx) {
        auto result = BuildLayout(hwnd_, layout);
        for (const auto& p : result.placements) {
            if (p.hwnd) {
                if (p.type == UIComponentType::CheckBox) MarkParamCheckbox(p.hwnd);
                AttachToParamViewport(p.hwnd);
                group.push_back(p.hwnd);
                editorControls_.push_back(p.hwnd);
            }
        }
        paramLayoutResults_[idx] = result;
        return result;
    }

    // 获取指定 action index 对应的布局结果中的 HWND
    HWND LayoutHwnd(int idx, int id) const {
        auto it = paramLayoutResults_.find(idx);
        if (it != paramLayoutResults_.end()) return it->second.HwndForId(id);
        return nullptr;
    }

    static HWND FindLayoutTextControl(const UILayoutResult& result, const wchar_t* text, int occurrence = 0) {
        int seen = 0;
        for (const auto& p : result.placements) {
            if (!p.hwnd) continue;
            if (p.type != UIComponentType::Label
                && p.type != UIComponentType::EditorLabel
                && p.type != UIComponentType::Hint) {
                continue;
            }
            wchar_t buf[128]{};
            GetWindowTextW(p.hwnd, buf, 128);
            if (wcscmp(buf, text) != 0) continue;
            if (seen++ == occurrence) return p.hwnd;
        }
        return nullptr;
    }

    void RestoreEditorControlLayout(HWND hwnd) {
        if (!hwnd) return;
        const RECT scaled = ScaledEditorLayoutRect(hwnd);
        MoveParamAware(hwnd, scaled.left, scaled.top, scaled.right - scaled.left, scaled.bottom - scaled.top, FALSE);
        SetWindowRgn(hwnd, nullptr, TRUE);
    }

    void RestoreEditorGroupLayout(const std::vector<HWND>& group) {
        std::unordered_set<HWND> groupSet(group.begin(), group.end());
        for (const auto& item : editorLayouts_) {
            if (!groupSet.count(item.hwnd)) continue;
            RestoreEditorControlLayout(item.hwnd);
        }
    }

    // 显示/隐藏指定 action index 的布局
    void ShowParamLayout(int idx, bool visible) {
        auto it = paramLayoutResults_.find(idx);
        if (it == paramLayoutResults_.end()) return;
        if (!visible) {
            for (const auto& p : it->second.placements) {
                if (p.hwnd) ParkParamControl(p.hwnd);
            }
            return;
        }
        for (const auto& p : it->second.placements) {
            if (!p.hwnd) continue;
            if (!UsesRuntimeParamLayout(p.hwnd)) RestoreEditorControlLayout(p.hwnd);
            ShowWindow(p.hwnd, SW_SHOW);
            SetWindowRgn(p.hwnd, nullptr, TRUE);
        }
    }

    // 收集当前动作类型所有可见参数面板中需要 GDI 绘制的控件，按 layer 排序
    // 返回: (hwnd, type, layer) 的排序列表 (低 layer → 高 layer, 即先绘制的在前)
    struct ParamDrawItem {
        HWND hwnd = nullptr;
        UIComponentType type = UIComponentType::Label;
        int layer = 0;
    };
    std::vector<ParamDrawItem> CollectParamDrawItems() const {
        std::vector<ParamDrawItem> items;
        const int sel = popupAction_.sel;
        for (const auto& [idx, result] : paramLayoutResults_) {
            // 只收集当前可见的 action 相关布局
            if (idx != sel && !IsSubPanelIdxVisible(sel, idx)) continue;
            for (const auto& p : result.placements) {
                if (!p.hwnd || !IsWindowVisible(p.hwnd)) continue;
                // 只收集需要自定义 GDI 绘制的类型
                if (p.type != UIComponentType::Edit
                    && p.type != UIComponentType::FieldEdit
                    && p.type != UIComponentType::ComboLabel) continue;
                items.push_back({p.hwnd, p.type, p.layer});
            }
        }
        std::sort(items.begin(), items.end(),
            [](const ParamDrawItem& a, const ParamDrawItem& b) { return a.layer < b.layer; });
        return items;
    }
    bool IsSubPanelIdxVisible(int sel, int idx) const {
        // 判断子面板索引是否对应当前 sel 可见
        if (idx == 170 || idx == 171) return sel == 17;                          // 找图子面板
        if (idx >= 180 && idx <= 186) return sel == 18;                          // OCR 子面板
        if (idx == 240) return sel == 24 && popupRunProgram_.sel <= 0;           // 打开程序文件子面板
        if (idx == 29) return sel == 29 || sel == 30 || sel == 31;             // AI 共用面板
        if (idx == 300) return sel == 30;                                        // AI 图片子面板
        if (idx == 310) return sel == 31;                                        // AI 动作子面板
        if (idx == 320) return sel == 29 || sel == 30 || sel == 31;             // AI 找图子区域
        return false;
    }

    void CreateParamControls();

    void ApplyEditorFonts() {
        if (editorFontsApplied_ && editorFontsAppliedTo_ == editorFont_) return;
        for (HWND h : editorControls_) {
            if (!h) continue;
            wchar_t cls[16]{};
            GetClassNameW(h, cls, 16);
            if (wcscmp(cls, L"Button") == 0) {
                LONG style = GetWindowLongW(h, GWL_STYLE);
                if (style & BS_OWNERDRAW) continue;
            }
            wchar_t text[256]{};
            GetWindowTextW(h, text, 256);
            if (text[0] == L'*') continue;
            // FALSE：避免每个控件立刻重绘，打开编辑页时数百次同步重绘极慢
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(editorFont_), FALSE);
        }
        editorFontsApplied_ = true;
        editorFontsAppliedTo_ = editorFont_;
    }

    void ShowGroup(const std::vector<HWND>& controls, bool visible) { for (HWND h : controls) ShowWindow(h, visible ? SW_SHOW : SW_HIDE); }

    bool IsParamViewportDescendant(HWND h) const {
        if (!h || !paramViewport_) return false;
        if (h == paramViewport_) return true;
        return IsParamViewportChild(h);
    }

    void ParkAllParamPanelControls() const {
        if (!paramViewport_) return;
        for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            ParkParamControl(child);
        }
    }

    void HideEditorMacroHeaderControls() const {
        for (HWND h : {labelMacro_, name_, labelBreakoutTime_, breakoutTimeEdit_,
                wmSelectMethod_, wmSpecifyWindowBtn_, wmTargetPathEdit_,
                wmTargetBrowseBtn_, wmTargetCrosshairBtn_, wmFakeFocusCheck_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
    }

    bool IsBatchToolbarHwnd(HWND hwnd) const {
        return hwnd == loadBtn_ || hwnd == clearBtn_
            || hwnd == batchExitBtn_ || hwnd == batchSelectAllBtn_
            || hwnd == batchDeselectBtn_ || hwnd == batchDeleteBtn_
            || hwnd == batchCopyBtn_ || hwnd == labelBatchCount_;
    }

    // includeParamViewport：打开编辑页首帧可传 false，避免参数区在 ResetActionFormSession 前闪出旧布局
    void ShowEditorControls(bool visible, bool includeParamViewport = true) {
        if (!visible) {
            CloseEditorPopup();
            // 离开编辑页：整组隐藏即可，勿逐个 Park（长脚本参数控件极多，MoveWindow 会卡）
            if (paramViewport_) ShowWindow(paramViewport_, SW_HIDE);
            HideEditorMacroHeaderControls();
            for (HWND h : editorControls_) {
                if (!h || IsParamViewportDescendant(h)) continue;
                ShowWindow(h, SW_HIDE);
            }
            ApplyParamLayerMasks();
            return;
        }
        // 页眉/批量工具栏/备注编辑/自绘下拉锚点由专用函数按状态显隐，不可一律 SW_SHOW。
        // 否则 mode_/actionCombo_ 等 STATIC 会叠在父窗口自绘之上，出现透明/乱字。
        // 添加/修改/备注：须等 ApplyEditorFooterLayout，否则会在设计坐标上闪一下。
        for (HWND h : editorControls_) {
            if (!h || IsParamViewportDescendant(h)) continue;
            if (IsEditorMacroHeaderHwnd(h)) continue;
            if (IsBatchToolbarHwnd(h)) continue;
            if (IsEditorComboAnchorHwnd(h)) continue;
            if (IsFooterControl(h)) continue;
            if (h == listRemarkEdit_ || IsParamMaskControl(h)) continue;
            ShowWindow(h, SW_SHOW);
        }
        for (HWND h : {remarkLabel_, remark_, addBtn_, modifyBtn_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
        if (includeParamViewport && paramViewport_) ShowWindow(paramViewport_, SW_SHOW);
        else if (paramViewport_) ShowWindow(paramViewport_, SW_HIDE);
        if (page_ == Page::Editor) {
            UpdateEditorWindowModeChrome();
            UpdateBatchToolbar();
            HideEditorComboHwnds();
        }
        ApplyParamLayerMasks();
    }

    bool IsFooterControl(HWND hwnd) const {
        return hwnd == remarkLabel_ || hwnd == remark_ || hwnd == addBtn_ || hwnd == modifyBtn_;
    }

    bool ShouldShowModifyButton() const {
        return !batchEditMode_ && selectedIndex_ >= 0
            && selectedIndex_ < static_cast<int>(actions_.size());
    }

    bool IsParamMaskControl(HWND hwnd) const {
        return hwnd == paramTopMask_ || hwnd == paramBottomMask_ || hwnd == paramRightMask_;
    }

    COLORREF ParamMaskColor(HWND hwnd) const {
        return hwnd == paramBottomMask_ ? kPanel : kWhite;
    }

    /// 打开编辑页时延后显示参数区，避免 UpdateParamViewportGeometry 强制揭开旧控件（绿条闪一下）
    bool ParamViewportAllowed() const {
        return page_ == Page::Editor && !editorOpenPending_;
    }

    void ApplyParamLayerMasks() {
        if (!hwnd_) return;
        UpdateParamViewportGeometry();
        for (HWND h : {paramTopMask_, paramBottomMask_, paramRightMask_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
        if (page_ != Page::Editor) return;

        for (HWND h : {cancelBtn_, saveBtn_}) {
            if (h && IsWindowVisible(h)) {
                SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
        }
        if (name_ && IsWindowVisible(name_)) {
            SetWindowPos(name_, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        if (breakoutTimeEdit_ && IsWindowVisible(breakoutTimeEdit_)) {
            SetWindowPos(breakoutTimeEdit_, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    void UpdateParamViewportGeometry() {
        if (!paramViewport_) return;
        const RECT visible = ParamScrollContentRect();
        const int w = visible.right - visible.left;
        const int h = visible.bottom - visible.top;
        if (w <= 0 || h <= 0) return;
        const RECT cur = WindowClientRect(paramViewport_);
        const bool sameBox = cur.left == visible.left && cur.top == visible.top
            && (cur.right - cur.left) == w && (cur.bottom - cur.top) == h;
        const bool wantShow = ParamViewportAllowed();
        const bool shown = IsWindowVisible(paramViewport_) != FALSE;
        if (sameBox && wantShow == shown) return;
        SetWindowPos(paramViewport_, HWND_TOP, visible.left, visible.top, w, h,
            SWP_NOACTIVATE | (wantShow ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }

    bool IsParamScrollManagedHwnd(HWND hwnd) const {
        for (const auto& entry : paramScrollLayout_) {
            if (entry.hwnd == hwnd) return true;
        }
        return false;
    }

    RECT EditorComboClientRect(HWND combo) const {
        if (!combo) return RECT{};
        if (IsParamScrollManagedHwnd(combo)) {
            for (const auto& entry : paramScrollLayout_) {
                if (entry.hwnd != combo) continue;
                const int y = entry.baseY - paramScrollY_;
                return RECT{entry.baseX, y, entry.baseX + entry.baseW, y + entry.baseH};
            }
        }
        return WindowClientRect(combo);
    }

    RECT PaintExcludeRectForChild(HWND hwnd) const {
        RECT rc = WindowClientRect(hwnd);
        if (IsParamScrollManagedHwnd(hwnd)) {
            RECT content = ParamScrollContentRect();
            RECT clipped{};
            if (IntersectRect(&clipped, &rc, &content)) return clipped;
            return RECT{};
        }
        return rc;
    }

    bool IsParamPanelCheckbox(HWND hwnd) const {
        // 仅依赖标记位：勿再用 IsParamViewportChild/page_ 门禁，否则 DRAWITEM
        // 路由变化时会落到 DrawOwnerButton，勾选框整块变绿按钮。
        return IsMarkedParamCheckbox(hwnd);
    }

    void PaintParamPanelCheckbox(HWND hwnd, HDC hdc, const RECT* itemRc = nullptr) const {
        if (!hwnd || !hdc) return;
        RECT rc{};
        if (itemRc) rc = *itemRc;
        else GetClientRect(hwnd, &rc);
        FillRectColor(hdc, rc, kWhite);
        wchar_t text[128]{};
        GetWindowTextW(hwnd, text, 128);
        HGDIOBJ oldFont = SelectObject(hdc, editorFont_ ? editorFont_ : font_);
        constexpr int kCbSize = 18;
        const int cbTop = rc.top + (rc.bottom - rc.top - kCbSize) / 2;
        RECT cbRc{rc.left, cbTop, rc.left + kCbSize, cbTop + kCbSize};
        DrawCheckbox(hdc, cbRc, Checked(hwnd));
        RECT textRc{rc.left + kCbSize + 4, rc.top, rc.right, rc.bottom};
        ::DrawTextIn(hdc, text, textRc, kText);
        if (oldFont) SelectObject(hdc, oldFont);
    }

    void DrawParamPanelCheckboxItem(DRAWITEMSTRUCT* dis) const {
        if (!dis) return;
        PaintParamPanelCheckbox(dis->hwndItem, dis->hDC, &dis->rcItem);
    }

    bool IsFindImageParamEdit(HWND hwnd) const {
        return hwnd == findX1_ || hwnd == findY1_ || hwnd == findX2_ || hwnd == findY2_
            || hwnd == findMatchThreshold_ || hwnd == findScaleMin_ || hwnd == findScaleMax_
            || hwnd == findOffsetX_ || hwnd == findOffsetY_ || hwnd == findMatchVar_;
    }

    bool IsOcrParamEdit(HWND hwnd) const {
        return hwnd == ocrX1_ || hwnd == ocrY1_ || hwnd == ocrX2_ || hwnd == ocrY2_
            || hwnd == ocrSearchEdit_ || hwnd == ocrOffsetX_ || hwnd == ocrOffsetY_
            || hwnd == ocrResultVar_
            || hwnd == ocrFindMatchThreshold_ || hwnd == ocrFindScaleMin_ || hwnd == ocrFindScaleMax_;
    }

    bool IsControlShown(HWND h) const {
        if (!h) return false;
        if (!hwnd_ || !IsWindowVisible(hwnd_)) {
            return (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0;
        }
        return IsWindowVisible(h);
    }

    void CollectVisibleParamControls(std::vector<HWND>& out) const {
        out.clear();
        auto append = [&](const std::vector<HWND>& group) {
            for (HWND h : group) if (IsControlShown(h)) out.push_back(h);
        };
        const int sel = popupAction_.sel;
        switch (sel) {
        case 0: append(moveControls_); break;
        case 34: append(moveRelControls_); break;
        case 1: append(waitControls_); break;
        case 2: append(clickControls_); break;
        case 3: append(mousePlaybackControls_); break;
        case 4: append(runMacroControls_); break;
        case 5: case 6: append(mousePressControls_); break;
        case 7: append(scrollWheelControls_); break;
        case 8: append(keyControls_); break;
        case 9: case 10: append(keyPressControls_); break;
        case 11: append(hotkeyShortcutControls_); break;
        case 12: append(quickInputControls_); break;
        case 13: append(loopControls_); break;
        case 14: append(endLoopControls_); break;
        case 15: append(defineBlockControls_); break;
        case 16: append(runBlockControls_); break;
        case 17:
            append(findImageControls_);
            if (popupFindFollowUp_.sel == 2) append(findImageVarControls_);
            else append(findImageOffsetControls_);
            break;
        case 18:
            append(ocrDepControls_);
            append(ocrFindRegionToggleControls_);
            append(ocrControls_);
            if (ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_)) {
                append(ocrFindRegionControls_);
            }
            append(ocrFollowControls_);
            if (popupOcrResultMode_.sel == 1) append(ocrSearchControls_);
            if (popupOcrFollowUp_.sel == 2) append(ocrFollowVarControls_);
            else append(ocrFollowOffsetControls_);
            break;
        case 19: append(ifControls_); break;
        case 20: append(elseControls_); break;
        case 21: append(lockScreenshotControls_); break;
        case 22: append(unlockScreenshotControls_); break;
        case 23: append(stopMacroControls_); break;
        case 24:
            append(runProgramControls_);
            if (popupRunProgram_.sel <= 0) append(runProgramFileControls_);
            break;
        case 25: append(closeProgramControls_); break;
        case 26: append(openWebpageControls_); break;
        case 27: append(openFileControls_); break;
        case 28: append(timerRecordControls_); break;
        case 32: append(getCursorPosControls_); break;
        case 33: append(gotoControls_); break;
        case 29: append(aiCommonControls_); append(aiTextControls_); break;
        case 30: append(aiCommonControls_); append(aiImageControls_);
            if (aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_)) append(aiFindRegionControls_);
            break;
        case 31: append(aiCommonControls_); append(aiActionControls_);
            if (aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_)) append(aiFindRegionControls_);
            break;
        default: break;
        }
    }

    int ParamPanelBottomClient() const {
        int bottom = 0;
        if (actionCombo_) {
            bottom = std::max(bottom, static_cast<int>(ScaledLayoutClientRect(actionCombo_).bottom));
        }
        std::vector<HWND> controls;
        CollectVisibleParamControls(controls);
        for (HWND h : controls) {
            bottom = std::max(bottom, static_cast<int>(WindowClientRect(h).bottom));
        }
        return bottom;
    }

    RECT ScaledLayoutClientRect(HWND hwnd) const {
        for (const auto& item : editorLayouts_) {
            if (item.hwnd != hwnd) continue;
            const RECT& b = item.base;
            if (IsFindImageHwnd(hwnd)) {
                return RECT{
                    ScaleX(b.left),
                    ScaleX(b.top),
                    ScaleX(b.right),
                    ScaleX(b.bottom)
                };
            }
            if (IsOcrDepHwnd(hwnd) || IsOcrHwnd(hwnd)) {
                return WindowClientRect(hwnd);
            }
            return RECT{
                ScaleX(b.left),
                ScaleY(b.top),
                ScaleX(b.right),
                ScaleY(b.bottom)
            };
        }
        return WindowClientRect(hwnd);
    }

    int ParamScrollEditorRightMargin() const {
        return ScaleX(kParamScrollEditorRightMarginDesign);
    }

    int ParamScrollBarGap() const {
        return ScaleX(kParamScrollBarGapDesign);
    }

    int ParamScrollContentLeft() const {
        return ScaleX(kParamScrollLeftDesign);
    }

    int ParamFieldInset() const {
        return ScaleX(kParamFieldInsetDesign);
    }

    int ParamPanelLeft() const {
        return ParamScrollContentLeft() + ParamFieldInset();
    }

    int ParamFieldMaxRight() const {
        return ParamScrollContentRight() - ParamFieldInset();
    }

    int ParamFieldMaxWidth() const {
        return std::max(1, ParamFieldMaxRight() - ParamPanelLeft());
    }

    int ParamScrollOuterRight() const {
        return ScaleX(kParamScrollRightDesign);
    }

    int ParamScrollOuterBottom() const {
        return ScaleY(kParamScrollBottomDesign);
    }

    int ParamScrollTrackRightEdge() const {
        return ParamScrollOuterRight();
    }

    RECT ParamScrollTrackRect() const {
        const int right = ParamScrollTrackRightEdge();
        const int left = right - UiLen(kEditorScrollW);
        const int top = ParamPanelContentTopY() + UiLen(2);
        const int bottom = ParamScrollViewportBottomY() - UiLen(2);
        if (bottom <= top || right <= left) return RECT{};
        return RECT{left, top, right, bottom};
    }

    int ParamScrollContentRight() const {
        return std::max(ParamScrollContentLeft() + 1,
            ParamScrollOuterRight() - UiLen(kEditorScrollW) - ParamScrollBarGap());
    }

    int ParamScrollContentWidth() const {
        return std::max(1, ParamScrollContentRight() - ParamScrollContentLeft());
    }

    int ParamPanelContentTopY() const {
        return ScaleY(kParamScrollTopDesign);
    }

    int ParamScrollViewportBottomY() const {
        return ParamScrollOuterBottom();
    }

    RECT ParamScrollOuterRect() const {
        return RECT{
            ParamScrollContentLeft(),
            ParamPanelContentTopY(),
            ParamScrollOuterRight(),
            ParamScrollOuterBottom()
        };
    }

    RECT ParamScrollContentRect() const {
        return RECT{
            ParamScrollContentLeft(),
            ParamPanelContentTopY(),
            ParamScrollContentRight(),
            ParamScrollViewportBottomY()
        };
    }

    RECT ParamScrollViewportRect() const {
        return ParamScrollContentRect();
    }

    RECT ParamEditorRedrawRect() const {
        RECT rc = ParamScrollViewportRect();
        RECT track = ParamScrollTrackRect();
        if (track.right > track.left) rc.right = std::max(rc.right, track.right);

        if (actionCombo_) {
            RECT actionRc = WindowClientRect(actionCombo_);
            rc.left = std::min(rc.left, actionRc.left);
            rc.top = std::min(rc.top, actionRc.top - ScaleY(34));
            rc.right = std::max(rc.right, actionRc.right);
        }
        rc.bottom = std::max(rc.bottom, static_cast<LONG>(ParamScrollViewportBottomY()));
        return rc;
    }

    bool ParamRectIntersectsContent(const RECT& rc) const {
        RECT content = ParamScrollContentRect();
        RECT vis{};
        return IntersectRect(&vis, &rc, &content) != FALSE;
    }

    bool ParamRectIntersectsViewport(const RECT& rc) const {
        return ParamRectIntersectsContent(rc);
    }

    void InvalidateParamScrollChrome() {
        if (!hwnd_) return;
        InvalidateParamPanelArea();
    }

    void InvalidateParamPanelArea() {
        if (!hwnd_) return;
        RECT area = ParamEditorRedrawRect();
        InvalidateRect(hwnd_, &area, FALSE);
        if (paramViewport_ && IsWindowVisible(paramViewport_))
            InvalidateRect(paramViewport_, nullptr, FALSE);
    }

    void InvalidateParamControlInViewport(HWND h) const {
        if (!h || !paramViewport_) return;
        RECT rc = WindowClientRect(h);
        const RECT vp = ParamViewportRect();
        RECT vr{
            rc.left - vp.left, rc.top - vp.top,
            rc.right - vp.left, rc.bottom - vp.top
        };
        InvalidateRect(paramViewport_, &vr, FALSE);
    }

    void LockParamViewportRedraw() const {
        if (paramViewport_) SendMessageW(paramViewport_, WM_SETREDRAW, FALSE, 0);
    }

    void UnlockParamViewportRedraw() const {
        if (!paramViewport_) return;
        SendMessageW(paramViewport_, WM_SETREDRAW, TRUE, 0);
        // 不带 RDW_ERASE：由 WM_PAINT 一次填白底+子控件，避免先擦后画的闪白
        RedrawWindow(paramViewport_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_NOERASE);
    }

    void RequestOcrSubPanelRefresh() {
        if (!hwnd_ || ocrSubPanelRefreshPosted_) return;
        ocrSubPanelRefreshPosted_ = true;
        LockParamViewportRedraw();
        PostMessageW(hwnd_, WM_OCR_SUBPANEL_REFRESH, 0, 0);
    }

    void HideInactiveParamControls() {
        const int sel = popupAction_.sel;
        const bool aiFindImage = (sel == 30 && aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_))
            || (sel == 31 && aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_));
        auto stashGroup = [&](const std::vector<HWND>& group, bool active) {
            if (active) return;
            ParkParamGroup(group);
        };
        stashGroup(moveControls_, sel == 0);
        stashGroup(moveRelControls_, sel == 34);
        stashGroup(waitControls_, sel == 1);
        stashGroup(clickControls_, sel == 2);
        stashGroup(mousePlaybackControls_, sel == 3);
        stashGroup(runMacroControls_, sel == 4);
        stashGroup(mousePressControls_, sel == 5 || sel == 6);
        stashGroup(scrollWheelControls_, sel == 7);
        stashGroup(keyControls_, sel == 8);
        stashGroup(keyPressControls_, sel == 9 || sel == 10);
        stashGroup(hotkeyShortcutControls_, sel == 11);
        stashGroup(quickInputControls_, sel == 12);
        stashGroup(loopControls_, sel == 13);
        stashGroup(endLoopControls_, sel == 14);
        stashGroup(defineBlockControls_, sel == 15);
        stashGroup(runBlockControls_, sel == 16);
        stashGroup(findImageControls_, sel == 17);
        stashGroup(findImageOffsetControls_, sel == 17 && popupFindFollowUp_.sel != 2);
        stashGroup(findImageVarControls_, sel == 17 && popupFindFollowUp_.sel == 2);
        stashGroup(ocrDepControls_, sel == 18);
        stashGroup(ocrFindRegionToggleControls_, sel == 18);
        stashGroup(ocrControls_, sel == 18);
        stashGroup(ocrFindRegionControls_, sel == 18 && ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_));
        stashGroup(ocrFollowControls_, sel == 18);
        stashGroup(ocrSearchControls_, sel == 18 && popupOcrResultMode_.sel == 1);
        stashGroup(ocrFollowOffsetControls_, sel == 18 && popupOcrFollowUp_.sel != 2);
        stashGroup(ocrFollowVarControls_, sel == 18 && popupOcrFollowUp_.sel == 2);
        stashGroup(ifControls_, sel == 19);
        stashGroup(elseControls_, sel == 20);
        stashGroup(lockScreenshotControls_, sel == 21);
        stashGroup(unlockScreenshotControls_, sel == 22);
        stashGroup(stopMacroControls_, sel == 23);
        stashGroup(runProgramControls_, sel == 24);
        stashGroup(runProgramFileControls_, sel == 24 && popupRunProgram_.sel <= 0);
        stashGroup(closeProgramControls_, sel == 25);
        stashGroup(openWebpageControls_, sel == 26);
        stashGroup(openFileControls_, sel == 27);
        stashGroup(timerRecordControls_, sel == 28);
        stashGroup(getCursorPosControls_, sel == 32);
        stashGroup(gotoControls_, sel == 33);
        stashGroup(aiCommonControls_, sel == 29 || sel == 30 || sel == 31);
        stashGroup(aiTextControls_, sel == 29);
        stashGroup(aiImageControls_, sel == 30);
        stashGroup(aiActionControls_, sel == 31);
        stashGroup(aiFindRegionControls_, aiFindImage);
    }

    bool IsEditorParamComboHwnd(HWND hwnd) const {
        for (HWND h : {mousePressButton_, clickButton_, loopTypeCombo_, runBlockCombo_, hotkeyShortcutCombo_,
            quickInputVarCombo_, aiVarCombo_, runMacroCombo_, mousePlaybackCombo_, scrollDirectionCombo_, findFollowUpCombo_,
            ocrResultModeCombo_, ocrFollowUpCombo_, ocrSearchVarCombo_, ifVarCombo_, ifOperatorCombo_,
            ifConnectorCombo_, runProgramCombo_, aiModelCombo_, aiContextModeCombo_, aiOutputTypeCombo_,
            aiSearchRegionCombo_}) {
            if (h == hwnd) return true;
        }
        return false;
    }

    void CollectParamScrollControls(std::vector<HWND>& out) const;

    struct RemarkFieldMetrics {
        int panelLeft = 0;
        int labelW = 0;
        int fieldH = 0;
        int editX = 0;
        int editW = 0;
    };

    RemarkFieldMetrics ComputeRemarkFieldMetrics() const {
        const int panelLeft = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int labelW = ScaleX(44);
        const int fieldH = ScaleY(22);
        const int labelGap = ScaleX(4);
        const int editX = panelLeft + labelW + labelGap;
        const int editW = std::max(40, maxRight - editX);
        return RemarkFieldMetrics{panelLeft, labelW, fieldH, editX, editW};
    }

    void LayoutRemarkStyleFieldRow(HWND label, HWND edit, int y, const RemarkFieldMetrics& m) const {
        if (label) {
            MoveParamAware(label, m.panelLeft, y, m.labelW, m.fieldH, FALSE);
            ShowWindow(label, SW_SHOW);
        }
        if (edit) {
            MoveParamAware(edit, m.editX, y, m.editW, m.fieldH, FALSE);
            ShowWindow(edit, SW_SHOW);
        }
    }

    int MeasureParamScrollContentBottom(const std::vector<HWND>& controls) const {
        int bottom = ParamPanelContentTopY();
        for (HWND h : controls) {
            if (!h || IsFooterControl(h)) continue;
            if (IsIntentionallyHiddenParamControl(h)) continue;
            if (!IsControlShown(h) && !IsEditorParamComboHwnd(h)) continue;
            const RECT rc = WindowClientRect(h);
            if (rc.top < ParamPanelContentTopY() - 64) continue;
            bottom = std::max(bottom, static_cast<int>(rc.bottom));
        }
        return bottom;
    }

    void ApplyEditorFooterLayout(int contentBottom = -1) {
        if (page_ != Page::Editor) return;

        const RemarkFieldMetrics remarkMetrics = ComputeRemarkFieldMetrics();
        const int panelLeft = remarkMetrics.panelLeft;
        const int maxRight = ParamFieldMaxRight();
        const int labelW = remarkMetrics.labelW;
        const int remarkH = remarkMetrics.fieldH;
        const int btnH = ScaleY(30);
        const int btnW = ScaleX(76);
        const int gap = ScaleY(18);
        const int panelTop = ParamPanelContentTopY();
        const int minRemarkY = ScaleY(kEditorRemarkY);
        const bool dynamicFooter = UsesDynamicParamPanel();
        if (contentBottom < 0) {
            std::vector<HWND> controls;
            CollectParamScrollControls(controls);
            contentBottom = MeasureParamScrollContentBottom(controls);
        }
        if (dynamicFooter && paramLayoutBottomHint_ > panelTop) {
            contentBottom = std::max(contentBottom, paramLayoutBottomHint_);
        }
        if (dynamicFooter && contentBottom < panelTop) return;
        const int measuredBottom = contentBottom > panelTop ? contentBottom + gap : 0;
        int remarkY;
        if (dynamicFooter) {
            remarkY = contentBottom + gap;
        } else {
            remarkY = measuredBottom > 0 ? measuredBottom : minRemarkY;
        }
        const int addY = remarkY + remarkH + gap;
        const int btnGap = ScaleX(10);
        int addX = maxRight - btnW;
        int modifyX = addX - btnGap - btnW;
        if (modifyX < panelLeft) {
            modifyX = panelLeft;
            addX = std::min(maxRight - btnW, modifyX + btnGap + btnW);
        }
        const int labelX = panelLeft;
        const int remarkX = remarkMetrics.editX;
        const int remarkW = remarkMetrics.editW;

        auto moveFooter = [&](HWND h, int x, int y, int w, int hgt) {
            if (!h) return;
            EnsureParamViewportParent(h);
            SetParamPosAware(h, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        };

        if (remarkLabel_) {
            ShowWindow(remarkLabel_, SW_SHOW);
            moveFooter(remarkLabel_, labelX, remarkY, labelW, remarkH);
        }
        if (remark_) {
            ShowWindow(remark_, SW_SHOW);
            moveFooter(remark_, remarkX, remarkY, remarkW, remarkH);
        }
        if (modifyBtn_) {
            ShowWindow(modifyBtn_, ShouldShowModifyButton() ? SW_SHOW : SW_HIDE);
            moveFooter(modifyBtn_, modifyX, addY, btnW, btnH);
        }
        if (addBtn_) {
            ShowWindow(addBtn_, SW_SHOW);
            moveFooter(addBtn_, addX, addY, btnW, btnH);
        }
        for (HWND h : {remarkLabel_, remark_, modifyBtn_, addBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    void RefreshEditorEdits() {
        if (!hwnd_ || page_ != Page::Editor) return;
        auto refreshEdit = [](HWND child) {
            if (!child || !IsWindowVisible(child)) return;
            wchar_t cls[16]{};
            GetClassNameW(child, cls, 16);
            if (lstrcmpW(cls, L"Edit") != 0) return;
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        };
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            refreshEdit(child);
        if (paramViewport_) {
            for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                refreshEdit(child);
        }
    }

    void RefreshGrayButtonsInParamViewport() {
        if (!paramViewport_ || page_ != Page::Editor) return;
        for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsGrayButton(child) || !IsWindowVisible(child)) continue;
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
        }
        UpdateHoverFromCursor();
    }

    void ClearParamScrollTrack(HDC hdc) const {
        RECT track = ParamScrollTrackRect();
        if (track.right <= track.left || track.bottom <= track.top) return;
        FillRectColor(hdc, track, kWhite);
    }

    // OCR / 找图 / AI 等依赖运行时堆叠布局的面板，须在 Reveal 之后刷新，
    // 否则 RevealParamControlsForCapture 会把控件恢复到静态坐标。
    void RefreshDynamicParamLayout() {
        const int sel = popupAction_.sel;
        if (sel == 0) RefreshMoveParamPanel();
        else if (sel == 17) RefreshFindImageSubPanel();
        else if (sel == 18) {
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
        } else if (sel == 29 || sel == 30 || sel == 31) RefreshAiSubPanel();
    }

    // 参数面板装饰（绿边/下拉框/复选框）统一在 paramViewport_ WM_PAINT 中绘制；
    // 此处仅触发 viewport 重绘，并在主窗口 DC 上画滚动条。
    void RepaintParamPanelChrome() {
        if (!hwnd_ || page_ != Page::Editor) return;
        HDC mainDc = GetDC(hwnd_);
        if (MaxParamScroll() > 0) PaintParamScrollScrollbar(mainDc);
        else ClearParamScrollTrack(mainDc);
        ReleaseDC(hwnd_, mainDc);
        if (paramViewport_) {
            RedrawWindow(paramViewport_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOCHILDREN);
            RefreshGrayButtonsInParamViewport();
        }
    }

    void ClearParamViewportSurface() {
        if (!paramViewport_ || page_ != Page::Editor) return;
        InvalidateRect(paramViewport_, nullptr, FALSE);
    }

    void RefreshParamViewportChildren() const {
        if (!paramViewport_ || page_ != Page::Editor) return;
        for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child)) continue;
            RedrawWindow(child, nullptr, nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
        }
    }

    void FillParamViewportGaps(HDC hdc, HWND viewport, const RECT& rcPaint) const {
        if (!hdc || !viewport) return;
        HRGN fillRgn = CreateRectRgnIndirect(&rcPaint);
        for (HWND child = GetWindow(viewport, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child)) continue;
            if (IsEditorParamComboHwnd(child)) continue;
            RECT cr = MapRectFromMain(viewport, WindowClientRect(child));
            // Edit 外边框画在 client 外 1px，需避让；灰按钮边框画在 client 内，勿外扩——
            // 否则滚动后按钮外侧 1px 永不刷白，留下「全图/测试」等边沿线残影。
            wchar_t cls[16]{};
            GetClassNameW(child, cls, 16);
            if (lstrcmpW(cls, L"Edit") == 0) InflateRect(&cr, 1, 1);
            RECT inter{};
            if (!IntersectRect(&inter, &cr, &rcPaint)) continue;
            HRGN childRgn = CreateRectRgnIndirect(&inter);
            CombineRgn(fillRgn, fillRgn, childRgn, RGN_DIFF);
            DeleteObject(childRgn);
        }
        SelectClipRgn(hdc, fillRgn);
        FillRectColor(hdc, rcPaint, kWhite);
        SelectClipRgn(hdc, nullptr);
        DeleteObject(fillRgn);
    }

    void PostLayoutParamPanelRedraw() {
        if (!paramViewport_ || page_ != Page::Editor) return;
        // 先刷非灰按钮子控件；灰按钮必须最后画，否则 viewport 白底/邻近勾选框会盖住上边框。
        for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child)) continue;
            if (IsEditorParamComboHwnd(child)) continue;
            if (IsGrayButton(child)) continue;
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        }
        if (popupAction_.sel == 18) {
            const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
            if (regionByImage) RaiseOcrFindImageHeaderControls();
            else RaiseOcrRegionButtons();
        }
        RefreshGrayButtonsInParamViewport();
    }

    void RaiseAiFindImageHeaderControls() const {
        for (HWND h : {aiFindImageLabel_, aiFindSelectRegionBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
        }
    }

    void RaiseOcrFindImageHeaderControls() const {
        for (HWND h : {ocrFindImageLabel_, ocrFindSelectRegionBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
        }
    }

    void RefreshEditorChildWindows(const RECT* updateRect) {
        if (!hwnd_ || page_ != Page::Editor) return;
        auto refreshChild = [&](HWND child) {
            if (!child || !IsWindowVisible(child)) return;
            if (updateRect) {
                wchar_t cls[16]{};
                GetClassNameW(child, cls, 16);
                if (lstrcmpW(cls, L"Edit") != 0) {
                    RECT rc = WindowClientRect(child);
                    RECT inter{};
                    if (!IntersectRect(&inter, &rc, updateRect)) return;
                }
            }
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        };
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            refreshChild(child);
        if (paramViewport_) {
            for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                refreshChild(child);
        }
    }

    void EnsureParamFooterVisible() {
        if (page_ != Page::Editor) return;
        const int maxScroll = MaxParamScroll();
        if (maxScroll <= 0) return;
        int footerBottom = ParamPanelContentTopY();
        for (HWND h : {remarkLabel_, remark_, modifyBtn_, addBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            footerBottom = std::max(footerBottom, static_cast<int>(WindowClientRect(h).bottom));
        }
        const int vpBottom = ParamScrollViewportBottomY();
        if (footerBottom <= vpBottom) return;
        paramScrollY_ = std::min(maxScroll, footerBottom - vpBottom + ScaleY(8));
        ApplyParamScrollOffset(false);
    }

    void RestoreStaticParamLayoutsForSel(int sel) {
        auto restoreIdx = [&](int idx) {
            auto it = paramLayoutResults_.find(idx);
            if (it == paramLayoutResults_.end()) return;
            for (const auto& p : it->second.placements) {
                if (!p.hwnd || UsesRuntimeParamLayout(p.hwnd)) continue;
                RestoreEditorControlLayout(p.hwnd);
            }
        };
        if (sel >= 0 && sel <= 34 && sel != 29) restoreIdx(sel);
        if (sel == 5 || sel == 6) { restoreIdx(5); restoreIdx(6); }
        if (sel == 9 || sel == 10) { restoreIdx(9); restoreIdx(10); }
        if (sel == 29 || sel == 30 || sel == 31) restoreIdx(29);
        if (sel == 24) restoreIdx(240);
    }

    bool UsesDynamicParamPanel(int sel = -1) const {
        if (sel < 0) sel = popupAction_.sel;
        return sel == 0 || sel == 17 || sel == 18 || sel == 29 || sel == 30 || sel == 31;
    }

    void SyncParamScrollLayout(int layoutBottomHint = -1) {
        if (layoutBottomHint >= 0) paramLayoutBottomHint_ = layoutBottomHint;
        CaptureParamScrollBaseLayout(layoutBottomHint);
        paramScrollY_ = std::clamp(paramScrollY_, 0, MaxParamScroll());
        ApplyParamScrollOffset(false);
        UpdateParamViewportGeometry();
        RepaintParamPanelChrome();
        PostLayoutParamPanelRedraw();
    }

    void ConfigureEditorCombos() {}

    void CaptureEditorControlLayout() {
        editorLayouts_.clear();
        std::unordered_set<HWND> seen;
        for (HWND h : editorControls_) {
            if (!h || !seen.insert(h).second) continue;
            RECT rc = WindowClientRect(h);
            editorLayouts_.push_back(EditorControlLayout{h, rc});
        }
    }

    bool IsFindImageHwnd(HWND hwnd) const {
        auto contains = [hwnd](const std::vector<HWND>& group) {
            for (HWND h : group) if (h == hwnd) return true;
            return false;
        };
        return contains(findImageControls_) || contains(findImageOffsetControls_) || contains(findImageVarControls_);
    }

    bool IsMoveHwnd(HWND hwnd) const {
        for (HWND h : moveControls_) if (h == hwnd) return true;
        return false;
    }

    bool IsOcrDepHwnd(HWND hwnd) const {
        return hwnd == ocrDepStatusLabel_ || hwnd == ocrDepInstallBtn_;
    }

    bool IsOcrHwnd(HWND hwnd) const {
        if (IsOcrDepHwnd(hwnd)) return false;
        auto contains = [hwnd](const std::vector<HWND>& group) {
            for (HWND h : group) if (h == hwnd) return true;
            return false;
        };
        return contains(ocrControls_) || contains(ocrSearchControls_) || contains(ocrFollowControls_)
            || contains(ocrFollowOffsetControls_) || contains(ocrFollowVarControls_)
            || contains(ocrFindRegionToggleControls_) || contains(ocrFindRegionControls_);
    }

    bool IsAiLayoutHwnd(HWND hwnd) const {
        const int sel = popupAction_.sel;
        if (sel != 29 && sel != 30 && sel != 31) return false;
        auto contains = [hwnd](const std::vector<HWND>& group) {
            for (HWND h : group) if (h == hwnd) return true;
            return false;
        };
        return contains(aiCommonControls_) || contains(aiImageControls_)
            || contains(aiActionControls_) || contains(aiFindRegionControls_);
    }

    bool IsAiDynamicHwnd(HWND hwnd) const {
        return IsAiLayoutHwnd(hwnd);
    }

    bool UsesRuntimeParamLayout(HWND hwnd) const {
        if (popupAction_.sel == 0 && IsMoveHwnd(hwnd)) return true;
        if (IsOcrDepHwnd(hwnd) && popupAction_.sel == 18) return true;
        if (IsOcrHwnd(hwnd)) return true;
        if (IsAiLayoutHwnd(hwnd)) return true;
        if (popupAction_.sel == 17 && IsFindImageHwnd(hwnd)) return true;
        return false;
    }

    RECT ScaledEditorLayoutRect(HWND hwnd) const {
        for (const auto& item : editorLayouts_) {
            if (item.hwnd != hwnd) continue;
            const int x = ScaleX(item.base.left);
            const int y = ScaleY(item.base.top);
            const int w = std::max(1, ScaleX(item.base.right - item.base.left));
            int h = std::max(1, ScaleY(item.base.bottom - item.base.top));
            if (IsEditorListHeaderLabelBase(item.base)) h = EditorListColumnHeaderHeight(y);
            return RECT{x, y, x + w, y + h};
        }
        return WindowClientRect(hwnd);
    }

    void SyncParamControlScrollPosition(HWND hwnd) const {
        if (!hwnd || UsesRuntimeParamLayout(hwnd)) return;
        const RECT scaled = ScaledEditorLayoutRect(hwnd);
        MoveParamAware(hwnd, scaled.left, scaled.top, scaled.right - scaled.left, scaled.bottom - scaled.top, FALSE);
    }

    bool IsEditorListHeaderLabelBase(const RECT& base) const {
        if (base.top != kEditorListColumnHeaderY) return false;
        return base.left == 32 || base.left == 94 || base.left == 438 || base.left == 631;
    }

    int EditorListColumnHeaderHeight(int scaledHeaderTop) const {
        return std::max(1, UiLen(kListY) - scaledHeaderTop - UiLen(2));
    }

    struct FindImageSideButtonLayout {
        int actionX = 0;
        int btnW = 0;
        int compactBtnH = 0;
        int sideGap = 0;
        int stackHeight = 0;

        int StackTop(int previewTop) const {
            return previewTop + std::max(0, (kFindImageSize - stackHeight) / 2);
        }

        int SideBtnY(int previewTop, int index) const {
            return StackTop(previewTop) + index * (compactBtnH + sideGap);
        }
    };

    FindImageSideButtonLayout ComputeFindImageSideButtonLayout() const {
        FindImageSideButtonLayout layout;
        layout.actionX = ScaleX(kFindActionBtnX);
        layout.btnW = std::max(1, ScaleX(kFindBtnW));
        layout.compactBtnH = std::max(1, ScaleY(kFindImageSideBtnH));
        layout.sideGap = std::max(2, ScaleY(kFindImageSideBtnGap));
        layout.stackHeight = layout.compactBtnH * 3 + layout.sideGap * 2;
        return layout;
    }

    void LayoutFindImageSideStack(
        int previewTop, HWND screenshot, HWND local, HWND clear) const {
        LayoutOcrFindImageSideStack(previewTop, screenshot, local, clear);
    }

    int LayoutFindImageMatchScaleRows(int y) const {
        const int left = ScaleX(kFindContentLeft);
        const int fieldH = ScaleY(22);
        const int rowGap = ScaleY(kFindVGap);
        auto show = [](HWND h) { if (h) ShowWindow(h, SW_SHOW); };
        if (findMatchThreshold_) {
            MoveParamAware(findMatchThreshold_, left + ScaleX(91), y,
                ScaleX(40), fieldH, FALSE);
            show(findMatchThreshold_);
        }
        for (HWND h : findImageControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"范围") == 0) {
                MoveParamAware(h, left, y, ScaleX(90), fieldH, FALSE);
                show(h);
            } else if (wcscmp(buf, L"%") == 0) {
                MoveParamAware(h, left + ScaleX(135), y,
                    ScaleX(24), fieldH, FALSE);
                show(h);
            }
        }
        y += fieldH + rowGap;
        if (findScaleMin_) {
            MoveParamAware(findScaleMin_, left + ScaleX(65), y,
                ScaleX(40), fieldH, FALSE);
            show(findScaleMin_);
        }
        if (findScaleMax_) {
            MoveParamAware(findScaleMax_, left + ScaleX(151), y,
                ScaleX(40), fieldH, FALSE);
            show(findScaleMax_);
        }
        for (HWND h : findImageControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"最小缩放") == 0) {
                MoveParamAware(h, left, y, ScaleX(64), fieldH, FALSE);
                show(h);
            } else if (wcscmp(buf, L"最大") == 0) {
                MoveParamAware(h, left + ScaleX(110), y,
                    ScaleX(40), fieldH, FALSE);
                show(h);
            }
        }
        return y + fieldH + rowGap;
    }

    int LayoutFindImageFollowUpRow(int y) const {
        const int left = ScaleX(kFindContentLeft);
        const int btnH = ScaleY(kFindBtnH);
        const int rowGap = ScaleY(kFindVGap);
        const int labelW = ScaleX(kFindFollowLabelW);
        const int comboW = ScaleX(kFindFollowComboW);
        const int gap = ScaleX(8);
        if (findFollowUpLabel_) {
            MoveParamAware(findFollowUpLabel_, left, y, labelW, btnH, FALSE);
            ShowWindow(findFollowUpLabel_, SW_SHOW);
        }
        if (findFollowUpCombo_) MoveParamAware(findFollowUpCombo_, left + labelW + gap, y, comboW, btnH, FALSE);
        return y + btnH + rowGap;
    }

    int LayoutFindImageOffsetBlock(int y) const {
        if (findOffsetXLabel_) ShowWindow(findOffsetXLabel_, SW_SHOW);
        if (findOffsetYLabel_) ShowWindow(findOffsetYLabel_, SW_SHOW);
        if (findMatchVarLabel_) ShowWindow(findMatchVarLabel_, SW_HIDE);
        y = LayoutParamCoordPairRow(y, findOffsetXLabel_, findOffsetX_, findOffsetYLabel_, findOffsetY_, kFindOffsetLabelW);
        const int rowGap = ScaleY(kFindVGap);
        const int fieldH = ScaleY(22);
        if (findSelectOffsetBtn_) {
            MoveOcrAt(findSelectOffsetBtn_, kFindSelectOffsetLeft, y, kFindSelectOffsetW, kFindImageSideBtnH);
            ShowWindow(findSelectOffsetBtn_, SW_SHOW);
        }
        y += OcrScaleY(kFindImageSideBtnH) + rowGap;
        LayoutFindImageTimeRow(y);
        return y + fieldH + rowGap;
    }

    void LayoutFindImageTimeRow(int y) const {
        LayoutRemarkStyleFieldRow(findTimeLabel_, findTimeEdit_, y, ComputeRemarkFieldMetrics());
    }

    int LayoutFindImageVarBlock(int y) const {
        if (findOffsetXLabel_) ShowWindow(findOffsetXLabel_, SW_HIDE);
        if (findOffsetYLabel_) ShowWindow(findOffsetYLabel_, SW_HIDE);
        if (findSelectOffsetBtn_) ShowWindow(findSelectOffsetBtn_, SW_HIDE);
        if (findTimeLabel_) ShowWindow(findTimeLabel_, SW_HIDE);
        if (findTimeEdit_) ShowWindow(findTimeEdit_, SW_HIDE);
        const int left = ScaleX(kFindContentLeft);
        const int fieldH = ScaleY(22);
        const int rowGap = ScaleY(kFindVGap);
        const int labelW = ScaleX(kFindMatchVarLabelW);
        const int editW = ScaleX(kFindMatchVarEditW);
        const int gap = ScaleX(4);
        if (findMatchVarLabel_) {
            MoveParamAware(findMatchVarLabel_, left, y, labelW, fieldH, FALSE);
            ShowWindow(findMatchVarLabel_, SW_SHOW);
        }
        if (findMatchVar_) MoveParamAware(findMatchVar_, left + labelW + gap, y, editW, fieldH, FALSE);
        return y + fieldH + rowGap;
    }

    int LayoutFindImagePreviewBlockAt(int previewY) const {
        if (!findImagePreviewBtn_) return previewY;
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        const int previewX = ScaleX(kFindContentLeft);
        MoveParamAware(findImagePreviewBtn_, previewX, previewY, kFindImageSize, kFindImageSize, FALSE);
        LayoutOcrFindImageSideStack(previewY, findScreenshotBtn_, findLocalImageBtn_, findClearImageBtn_);
        return previewY + kFindImageSize + layout.sideGap;
    }

    int LayoutFindImagePreviewBlock() const {
        if (page_ != Page::Editor || popupAction_.sel != 17) return 0;
        const int previewY = ScaleY(kFindImageRowY);
        const int afterPreview = LayoutFindImagePreviewBlockAt(previewY);
        return LayoutFindImageMatchScaleRows(afterPreview);
    }


    void ApplyEditorControlScale(bool force = false) {
        const int pct = UiScalePercent();
        if (!force && editorScaleAppliedPct_ == pct && page_ == Page::Editor) {
            // 打开过程中参数区尚未就绪：勿刷新子面板，否则会把上次动作的绿按钮画出来
            if (editorOpenPending_) {
                ApplyParamLayerMasks();
                return;
            }
            // 缩放未变：只刷新当前动作类型的动态子面板与页眉
            if (popupAction_.sel == 0) RefreshMoveParamPanel();
            else if (popupAction_.sel == 17) RefreshFindImageSubPanel();
            else if (popupAction_.sel == 18) {
                RefreshOcrDepStatus();
                RefreshOcrSubPanel();
            } else if (popupAction_.sel == 29 || popupAction_.sel == 30 || popupAction_.sel == 31) {
                RefreshAiSubPanel();
            }
            UpdateEditorWindowModeChrome();
            ApplyParamLayerMasks();
            return;
        }
        for (const auto& item : editorLayouts_) {
            if (item.hwnd == paramViewport_) {
                UpdateParamViewportGeometry();
                continue;
            }
            if (IsFooterControl(item.hwnd)) continue;
            if (IsEditorMacroHeaderHwnd(item.hwnd)) continue;
            if (IsFindImageHwnd(item.hwnd)) {
                // 找图控件由 RefreshFindImageSubPanel 统一堆叠布局
            } else if (IsMoveHwnd(item.hwnd)) {
                // 移动鼠标控件由 RefreshMoveParamPanel 统一堆叠布局
            } else if (IsOcrDepHwnd(item.hwnd) || IsOcrHwnd(item.hwnd) || IsAiLayoutHwnd(item.hwnd)) {
                // OCR / AI 动态区由 Refresh*SubPanel 统一堆叠布局
            } else {
                const int x = ScaleX(item.base.left);
                const int y = ScaleY(item.base.top);
                const int w = std::max(1, ScaleX(item.base.right - item.base.left));
                int h = std::max(1, ScaleY(item.base.bottom - item.base.top));
                if (IsEditorListHeaderLabelBase(item.base)) h = EditorListColumnHeaderHeight(y);
                MoveParamAware(item.hwnd, x, y, w, h, FALSE);
            }
        }
        if (!editorOpenPending_) {
            if (page_ == Page::Editor && popupAction_.sel == 0) {
                RefreshMoveParamPanel();
            }
            if (page_ == Page::Editor && popupAction_.sel == 17) {
                RefreshFindImageSubPanel();
            }
            ConfigureEditorCombos();
            if (page_ == Page::Editor && (popupAction_.sel == 18)) {
                RefreshOcrDepStatus();
                RefreshOcrSubPanel();
            }
            if (page_ == Page::Editor && (popupAction_.sel == 29 || popupAction_.sel == 30 || popupAction_.sel == 31)) {
                RefreshAiSubPanel();
            }
            if (page_ == Page::Editor) UpdateEditorWindowModeChrome();
        } else {
            ConfigureEditorCombos();
            if (page_ == Page::Editor) UpdateEditorWindowModeChrome();
        }
        ApplyParamLayerMasks();
        editorScaleAppliedPct_ = pct;
    }

    // ── Page navigation ────────────────────────────────────────────
    void ShowHome() {
        CommitInlineRemark();
        CloseEditorPopup();
        CancelQuickInputTip();
        StopHoverTimer();
        BeginSmoothPageTransition(hwnd_);
        ++editorOpenGeneration_;
        editorOpenPending_ = false;
        editorOpenPhase_ = 0;
        ClearEditorProgressiveState();
        if (batchEditMode_) {
            batchEditMode_ = false;
            batchSelected_.clear();
        }
        // 仅清内存草稿；表单复位/扫盘删图放到揭开后（与录制优化关窗一样：先见主页）
        actionFormDrafts_.clear();
        page_ = Page::Home;
        ShowEditorControls(false);
        if (hasHomeRectBeforeEditor_) {
            SetWindowPos(hwnd_, nullptr,
                homeRectBeforeEditor_.left, homeRectBeforeEditor_.top,
                UiHomeWidth(), UiHomeHeight(),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
            hasHomeRectBeforeEditor_ = false;
        }
        ClampHomeScroll();
        ClampRecordingScroll();
        // 勿用 EndSmooth 的 ALLCHILDREN：隐藏的编辑子控件极多，会拖住揭开
        editorFullClientBlit_ = true;
        RedrawWindow(hwnd_, nullptr, nullptr,
            RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
        editorFullClientBlit_ = false;
        SetWindowCloaked(hwnd_, false);
        BOOL disableTransitions = FALSE;
        DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED,
            &disableTransitions, sizeof(disableTransitions));
        outerShadow_.Sync();
        // wp=1：揭开后再 ResetActionFormSession + CleanupNewImages + 刷列表
        PostMessageW(hwnd_, WM_APP_HOME_REFRESH_LISTS, 1, 0);
    }

    /// 清理编辑期间新增但未被任何脚本引用的图片（取消编辑或退出时调用）
    void CleanupNewImages() {
        if (newImagePaths_.empty()) return;
        const auto allRefs = CollectAllReferencedImages();
        for (const auto& path : newImagePaths_) {
            if (allRefs.find(path) == allRefs.end()) {
                DeleteFileW(path.c_str());
            }
        }
        newImagePaths_.clear();
    }

    void ShowEditorFor(int index, bool createNew);

    void FinishDeferredEditorOpen(int openGen) {
        if (openGen != editorOpenGeneration_ || page_ != Page::Editor) return;
        if (!editorOpenPending_) return;
        LockParamViewportRedraw();
        ResetActionFormSession(editorOpenCreateNew_);
        if (editorOpenCreateNew_) RefreshRunBlockCombo();
        UpdateEditMode();
        if (mode_) ShowWindow(mode_, SW_HIDE);
        if (actionCombo_) ShowWindow(actionCombo_, SW_HIDE);
        editorOpenPending_ = false;
        editorOpenPhase_ = 0;
        if (paramViewport_) ShowWindow(paramViewport_, SW_SHOW);
        // Reset/LoadForm 已 ApplyEditorFooterLayout；再兜一次，避免首开坐标仍是创建时的设计值
        ApplyEditorFooterLayout();
        ApplyParamLayerMasks();
        UnlockParamViewportRedraw();
        DiscardSpuriousEditorInput();
    }

    // ── Action form ↔ UI binding ───────────────────────────────────
    void SetPopupSel(PopupCombo& pc, HWND label, int sel) {
        pc.sel = sel;
        SetText(label, sel >= 0 && sel < static_cast<int>(pc.items.size()) ? pc.items[static_cast<size_t>(sel)] : L"");
    }

    void SyncEditorPopupLayer() {
        // 下拉展开时保持参数面板布局不变，弹层在 Paint 中叠加绘制
    }

    void RefreshParamPanel();

    struct ParamScrollLayoutEntry {
        HWND hwnd = nullptr;
        int baseX = 0;
        int baseY = 0;
        int baseW = 0;
        int baseH = 0;
    };

    void EnsureParamViewportParent(HWND h) const {
        AttachToParamViewport(h);
    }

    void ClearParamScrollClipping() {
        for (const auto& entry : paramScrollLayout_) {
            if (entry.hwnd) SetWindowRgn(entry.hwnd, nullptr, TRUE);
        }
    }

    RECT ParamScrollThumbRect() const {
        RECT track = ParamScrollTrackRect();
        const int maxScroll = MaxParamScroll();
        if (maxScroll <= 0 || track.bottom <= track.top) return track;
        const int trackH = track.bottom - track.top;
        const int vpTop = static_cast<int>(ParamScrollViewportRect().top);
        const int contentH = std::max(1, paramContentBottom_ - vpTop);
        const int viewH = std::max(1, static_cast<int>(ParamScrollViewportRect().bottom - ParamScrollViewportRect().top));
        const int thumbH = std::max(32, trackH * viewH / contentH);
        const int range = std::max(1, trackH - thumbH);
        const int top = track.top + range * paramScrollY_ / maxScroll;
        return RECT{track.left, top, track.right, top + thumbH};
    }

    void UpdateParamScrollFromThumb(int thumbTop) {
        RECT track = ParamScrollTrackRect();
        RECT thumb = ParamScrollThumbRect();
        const int maxScroll = MaxParamScroll();
        const int trackHeight = static_cast<int>(track.bottom - track.top);
        const int thumbHeight = static_cast<int>(thumb.bottom - thumb.top);
        const int range = std::max(1, trackHeight - thumbHeight);
        const int thumbOffset = thumbTop - static_cast<int>(track.top);
        paramScrollY_ = std::clamp(thumbOffset * maxScroll / range, 0, maxScroll);
    }

    void InvalidateParamScrollArea() {
        InvalidateParamScrollChrome();
    }

    void CaptureParamScrollBaseLayout(int layoutBottomHint = -1);

    bool IsIntentionallyHiddenParamControl(HWND h) const {
        if (!h) return false;
        // ApplyParamScrollOffset 会对布局项强制 ShowWindow；无选中时必须继续隐藏「修改」
        if (h == modifyBtn_ && !ShouldShowModifyButton()) return true;
        const int sel = popupAction_.sel;
        if (h == quickInputVarCombo_ && sel != 12) return true;
        if (h == aiVarCombo_ && sel != 29 && sel != 30 && sel != 31) return true;
        if (sel == 31) {
            if (h == aiOutputVarLabel_ || h == aiOutputVarEdit_
                || h == aiOutputTypeLabel_ || h == aiOutputTypeCombo_) {
                return true;
            }
        }
        if (sel != 31) {
            if (h == aiWithImageCheck_ || h == aiLogicConvertCheck_
                || h == aiMaxStepsLabel_ || h == aiMaxStepsEdit_
                || h == aiMaxStepsHint_) {
                return true;
            }
        }
        const bool aiFindImage = (sel == 30 && aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_))
            || (sel == 31 && aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_));
        if (aiFindImage) {
            if (h == aiRegionLabel_ || h == aiFullScreenBtn_ || h == aiSelectRegionBtn_
                || h == aiActionRegionLabel_ || h == aiFullScreenBtn2_ || h == aiSelectRegionBtn2_) {
                return true;
            }
        }
        if (sel == 18) {
            const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
            if (regionByImage) {
                if (h == ocrRegionLabel_ || h == ocrFullScreenBtn_ || h == ocrSelectRegionBtn_) {
                    return true;
                }
            } else {
                for (HWND hide : ocrFindRegionControls_) {
                    if (h == hide) return true;
                }
                if (h == ocrFindSelectRegionBtn_ || h == ocrFindImageLabel_) return true;
            }
        }
        return false;
    }

    void RevealParamControlsForCapture() {
        std::vector<HWND> controls;
        CollectParamScrollControls(controls);
        for (HWND h : controls) {
            if (!h || IsIntentionallyHiddenParamControl(h)) continue;
            ShowWindow(h, SW_SHOW);
            SetWindowRgn(h, nullptr, TRUE);
        }
    }

    void ApplyParamScrollOffset(bool repaintChrome = true, bool eraseViewport = true, bool hideOffscreen = true, bool postLayoutRedraw = true);

    void RestoreParamPanelLayout() {
        std::vector<HWND> active;
        CollectParamScrollControls(active);
        std::unordered_set<HWND> activeSet(active.begin(), active.end());
        for (const auto& item : editorLayouts_) {
            if (item.hwnd == paramViewport_) {
                UpdateParamViewportGeometry();
                continue;
            }
            if (IsFooterControl(item.hwnd)) continue;
            if (IsOcrDepHwnd(item.hwnd)) continue;
            if (IsOcrHwnd(item.hwnd)) continue;
            if (IsFindImageHwnd(item.hwnd)) continue;
            if (IsMoveHwnd(item.hwnd)) continue;
            if (IsAiLayoutHwnd(item.hwnd)) continue;
            if (!activeSet.count(item.hwnd)) continue;
            const int x = ScaleX(item.base.left);
            const int y = ScaleY(item.base.top);
            const int w = std::max(1, ScaleX(item.base.right - item.base.left));
            int h = std::max(1, ScaleY(item.base.bottom - item.base.top));
            if (IsEditorListHeaderLabelBase(item.base)) h = EditorListColumnHeaderHeight(y);
            MoveParamAware(item.hwnd, x, y, w, h, FALSE);
        }
    }

    void RebuildParamPanelLayout() {
        ClearParamScrollClipping();
        paramScrollY_ = 0;
        HideInactiveParamControls();
        RestoreParamPanelLayout();
        RefreshDynamicParamLayout();
        SyncSharedVarComboVisibility();
        if (!UsesDynamicParamPanel()) {
            RevealParamControlsForCapture();
            CaptureParamScrollBaseLayout();
            paramScrollY_ = std::clamp(paramScrollY_, 0, MaxParamScroll());
            ApplyParamScrollOffset(false);
        }
        ApplyParamLayerMasks();
        RepaintParamPanelChrome();
        PostLayoutParamPanelRedraw();
    }

    void RestoreEditorAfterScreenOverlay() {
        SetWindowPos(hwnd_, HWND_TOP,
            findRegionSavedRect_.left, findRegionSavedRect_.top,
            findRegionSavedRect_.right - findRegionSavedRect_.left,
            findRegionSavedRect_.bottom - findRegionSavedRect_.top,
            SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd_);
        if (page_ != Page::Editor) {
            InvalidateRect(hwnd_, nullptr, TRUE);
            UpdateWindow(hwnd_);
            return;
        }
        paramScrollY_ = 0;
        paramLayoutBottomHint_ = -1;
        RefreshDynamicParamLayout();
        RepaintParamPanelChrome();
        PostLayoutParamPanelRedraw();
        InvalidateRect(hwnd_, nullptr, TRUE);
        if (paramViewport_) InvalidateRect(paramViewport_, nullptr, TRUE);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    void ParkParamControl(HWND h) const {
        if (!h) return;
        ShowWindow(h, SW_HIDE);
        SetWindowRgn(h, nullptr, TRUE);
        MoveParamAware(h, -5000, -5000, 1, 1, FALSE);
    }

    void ParkParamGroup(const std::vector<HWND>& group) const {
        for (HWND h : group) ParkParamControl(h);
    }

    void HideAiFindRegionControls() const {
        ParkParamGroup(aiFindRegionControls_);
    }

    void ApplyParamScroll() {
        ClearParamScrollClipping();
        const int sel = popupAction_.sel;
        if (!UsesDynamicParamPanel(sel)) {
            RestoreStaticParamLayoutsForSel(sel);
        }
        CaptureParamScrollBaseLayout();
        paramScrollY_ = std::clamp(paramScrollY_, 0, MaxParamScroll());
        ApplyParamScrollOffset(true);
        UpdateParamViewportGeometry();
        if (MaxParamScroll() <= 0 && hwnd_) {
            HDC dc = GetDC(hwnd_);
            ClearParamScrollTrack(dc);
            ReleaseDC(hwnd_, dc);
        }
    }

    void ScrollParamPanel(int deltaY) {
        const int maxScroll = MaxParamScroll();
        if (maxScroll <= 0) return;
        const int oldScroll = paramScrollY_;
        paramScrollY_ = std::clamp(paramScrollY_ + deltaY, 0, maxScroll);
        if (oldScroll == paramScrollY_) return;
        // 参数区内锚点的下拉随内容滚出视口；滚动时关闭，避免菜单悬在滚动区外
        CloseEditorPopupIfParamAnchored();
        // 滚轮滚动：冻结重绘→移位→单次合成刷新，避免整区闪烁与按钮边缘残影
        LockParamViewportRedraw();
        ApplyParamScrollOffset(/*repaintChrome*/ false, /*eraseViewport*/ false,
            /*hideOffscreen*/ true, /*postLayoutRedraw*/ false);
        UnlockParamViewportRedraw();
        if (hwnd_) {
            HDC mainDc = GetDC(hwnd_);
            if (mainDc) {
                PaintParamScrollScrollbar(mainDc);
                ReleaseDC(hwnd_, mainDc);
            }
        }
    }

    bool EditorPopupUsesParamViewportAnchor() const {
        // 0=模式 1=动作类型 23=窗口选择方式：均在参数滚动区外
        return editorPopupOpen_ >= 2 && editorPopupOpen_ != 23;
    }

    void CloseEditorPopupIfParamAnchored() {
        if (EditorPopupUsesParamViewportAnchor()) CloseEditorPopup();
    }

    int MaxParamScroll() const {
        if (paramContentBottom_ <= 0) return 0;
        const int vpBottom = static_cast<int>(ParamScrollContentRect().bottom);
        return std::max(0, paramContentBottom_ - vpBottom);
    }

    void UpdateMoveVarControls() {
        const bool fromVar = moveFromVar_ && Checked(moveFromVar_);
        if (moveX_) EnableWindow(moveX_, fromVar ? FALSE : TRUE);
        if (moveY_) EnableWindow(moveY_, fromVar ? FALSE : TRUE);
        if (moveVarX_) EnableWindow(moveVarX_, fromVar ? TRUE : FALSE);
        if (moveVarY_) EnableWindow(moveVarY_, fromVar ? TRUE : FALSE);
    }

    void UpdateLoopVarControls() {
        const bool fromVar = loopFromVar_ && Checked(loopFromVar_);
        if (loopCount_) EnableWindow(loopCount_, fromVar ? FALSE : TRUE);
        if (loopVarExpr_) EnableWindow(loopVarExpr_, fromVar ? TRUE : FALSE);
    }

    void HideEditorComboHwnds() {
        for (HWND h : {mode_, actionCombo_, mousePressButton_, clickButton_, loopTypeCombo_, runBlockCombo_, hotkeyShortcutCombo_, quickInputVarCombo_, aiVarCombo_, runMacroCombo_, mousePlaybackCombo_, scrollDirectionCombo_, findFollowUpCombo_, ocrResultModeCombo_, ocrFollowUpCombo_, ocrSearchVarCombo_, ifVarCombo_, ifOperatorCombo_, ifConnectorCombo_, runProgramCombo_, aiModelCombo_, aiContextModeCombo_, aiOutputTypeCombo_, aiSearchRegionCombo_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
    }

    bool IsEditorComboAnchorHwnd(HWND h) const {
        return h == mode_ || h == actionCombo_ || h == mousePressButton_ || h == clickButton_
            || h == loopTypeCombo_ || h == runBlockCombo_ || h == hotkeyShortcutCombo_
            || h == quickInputVarCombo_ || h == aiVarCombo_ || h == runMacroCombo_
            || h == mousePlaybackCombo_ || h == scrollDirectionCombo_ || h == findFollowUpCombo_
            || h == ocrResultModeCombo_ || h == ocrFollowUpCombo_ || h == ocrSearchVarCombo_
            || h == ifVarCombo_ || h == ifOperatorCombo_ || h == ifConnectorCombo_
            || h == runProgramCombo_ || h == aiModelCombo_ || h == aiContextModeCombo_
            || h == aiOutputTypeCombo_ || h == aiSearchRegionCombo_
            || h == wmSelectMethod_;
    }

    void LayoutFindImageRegionButtons(int y) {
        if (!findRegionLabel_ || !findFullScreenBtn_ || !findSelectRegionBtn_) return;
        const int left = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int btnGap = ScaleX(kFindRegionBtnGap);
        const int labelGap = ScaleX(kFindRegionLabelGap);
        const int selectW = kFindBtnW;
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        wchar_t labelBuf[32]{};
        GetWindowTextW(findRegionLabel_, labelBuf, 32);
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, labelBuf, static_cast<int>(wcslen(labelBuf)), &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(ScaleX(kFindRegionLabelW),
            static_cast<int>(labelSz.cx) + 4);
        const int fullWWant = std::max(44, static_cast<int>(fullSz.cx) + ScaleX(18));
        const int selectWScaled = ScaleX(selectW);
        const int availW = maxRight - left;
        const int fullWMax = std::max(44, availW - labelW - labelGap - btnGap - selectWScaled);
        const int fullW = std::max(44, std::min(fullWMax, fullWWant));
        const int totalW = labelW + labelGap + fullW + btnGap + selectWScaled;
        const int startX = left + std::max(0, (availW - totalW) / 2);
        const int labelXDesign = MulDiv(startX, kEditorBaseWidth, UiEditorWidth());
        const int fullXDesign = MulDiv(startX + labelW + labelGap, kEditorBaseWidth, UiEditorWidth());
        const int selectXDesign = MulDiv(startX + labelW + labelGap + fullW + btnGap, kEditorBaseWidth, UiEditorWidth());
        const int fullWDesign = MulDiv(fullW, kEditorBaseWidth, UiEditorWidth());
        MoveOcrAt(findRegionLabel_, labelXDesign, y, MulDiv(labelW, kEditorBaseWidth, UiEditorWidth()), kFindBtnH);
        MoveOcrAt(findFullScreenBtn_, fullXDesign, y, fullWDesign, kFindBtnH);
        MoveOcrAt(findSelectRegionBtn_, selectXDesign, y, selectW, kFindBtnH);
        ShowWindow(findRegionLabel_, SW_SHOW);
        ShowWindow(findFullScreenBtn_, SW_SHOW);
        ShowWindow(findSelectRegionBtn_, SW_SHOW);
    }

    void RaiseFindImageRegionButtons() const {
        for (HWND h : {findRegionLabel_, findFullScreenBtn_, findSelectRegionBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
        }
    }

    void RaiseOcrRegionButtons() const {
        for (HWND h : {ocrRegionLabel_, ocrFullScreenBtn_, ocrSelectRegionBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
        }
    }

    // 参数区复选框切换会触发 viewport 白底填充；邻近灰按钮须立即重绘，否则上边框被盖住。
    void RefreshOcrNeighborGrayButtons() {
        if (popupAction_.sel != 18) return;
        if (ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_))
            RaiseOcrFindImageHeaderControls();
        RaiseOcrRegionButtons();
        RefreshGrayButtonsInParamViewport();
    }

    int LayoutFindImageHeaderRow(int y) const {
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        const int rowH = layout.compactBtnH;
        const int left = ScaleX(kFindContentLeft);
        if (findImageHeaderLabel_) {
            MoveParamAware(findImageHeaderLabel_, left, y, ScaleX(90), rowH, FALSE);
            ShowWindow(findImageHeaderLabel_, SW_SHOW);
        }
        PlaceOcrFindImageCompactButton(findTestBtn_, y);
        if (findTestBtn_) ShowWindow(findTestBtn_, SW_SHOW);
        return y + rowH + layout.sideGap;
    }

    void RefreshMoveParamPanel();

    void RefreshFindImageSubPanel() {
        const bool saveVar = popupFindFollowUp_.sel == 2;
        if (saveVar) {
            ParkParamGroup(findImageOffsetControls_);
            for (HWND h : findImageVarControls_) {
                if (h) ShowWindow(h, SW_SHOW);
            }
        } else {
            ParkParamGroup(findImageVarControls_);
            for (HWND h : findImageOffsetControls_) {
                if (h) ShowWindow(h, SW_SHOW);
            }
        }
        HideFindImageFloatingControls();
        const int rowGap = ScaleY(kFindVGap);
        const int btnH = ScaleY(kFindBtnH);
        int y = ScaleY(kFindRegionRowY);
        LayoutFindImageRegionButtons(y);
        RaiseFindImageRegionButtons();
        y += btnH + rowGap;
        y = LayoutParamCoordRows(
            y,
            findX1Label_, findX1_, findY1Label_, findY1_,
            findX2Label_, findX2_, findY2Label_, findY2_);
        y = LayoutFindImageHeaderRow(y);
        y = LayoutFindImagePreviewBlockAt(y);
        if (findImagePreviewBtn_) {
            SetWindowPos(findImagePreviewBtn_, HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        y = LayoutFindImageMatchScaleRows(y);
        y = LayoutFindImageFollowUpRow(y);
        if (saveVar) y = LayoutFindImageVarBlock(y);
        else y = LayoutFindImageOffsetBlock(y);
        ShowFindImageSideControls(
            findImagePreviewBtn_, findScreenshotBtn_, findLocalImageBtn_, findClearImageBtn_);
        RefreshGrayButtonsInParamViewport();
        SyncParamScrollLayout(y);
        FiDbgLog(L"FIND_PANEL_REFRESH", L"layout snapshot");
        FiDbgLogGrayButtonLayout(L"FIND/screenshot", findScreenshotBtn_);
        FiDbgLogGrayButtonLayout(L"FIND/local", findLocalImageBtn_);
        FiDbgLogGrayButtonLayout(L"FIND/clear", findClearImageBtn_);
        FiDbgLogGrayButtonLayout(L"FIND/test", findTestBtn_);
        FiDbgLogGrayButtonLayout(L"OCR/screenshot", ocrFindScreenshotBtn_);
        FiDbgLogGrayButtonLayout(L"OCR/local", ocrFindLocalImageBtn_);
        FiDbgLogGrayButtonLayout(L"OCR/clear", ocrFindClearImageBtn_);
    }

    const wchar_t* GrayButtonDebugName(HWND hwnd) const {
        if (!hwnd) return L"(null)";
        if (hwnd == findFullScreenBtn_) return L"findFullScreen";
        if (hwnd == findSelectRegionBtn_) return L"findSelectRegion";
        if (hwnd == findTestBtn_) return L"findTest";
        if (hwnd == findImagePreviewBtn_) return L"findImagePreview";
        if (hwnd == findScreenshotBtn_) return L"findScreenshot";
        if (hwnd == findLocalImageBtn_) return L"findLocalImage";
        if (hwnd == findClearImageBtn_) return L"findClearImage";
        if (hwnd == findSelectOffsetBtn_) return L"findSelectOffset";
        if (hwnd == ocrFindScreenshotBtn_) return L"ocrFindScreenshot";
        if (hwnd == ocrFindLocalImageBtn_) return L"ocrFindLocalImage";
        if (hwnd == ocrFindClearImageBtn_) return L"ocrFindClearImage";
        if (hwnd == ocrFindSelectRegionBtn_) return L"ocrFindSelectRegion";
        return L"grayOther";
    }

    void SizeFindFullScreenButton() {
        if (!findFullScreenBtn_ || !findSelectRegionBtn_ || !findRegionLabel_) return;
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, L"找图区域", 4, &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(kFindRegionLabelW, static_cast<int>(labelSz.cx) + 4);
        const int selectX = kFindSelectRegionX;
        const int fullX = kFindContentLeft + labelW + kFindRegionLabelGap;
        const int fullWMax = selectX - kFindRegionBtnGap - fullX;
        const int fullWWant = std::max(32, static_cast<int>(fullSz.cx) + 10);
        const int fullW = std::max(32, std::min(fullWMax, fullWWant));
        MoveParamAware(findRegionLabel_, kFindContentLeft, kFindRegionRowY, labelW, kFindBtnH, FALSE);
        MoveParamAware(findFullScreenBtn_, fullX, kFindRegionRowY, fullW, kFindBtnH, FALSE);
        MoveParamAware(findSelectRegionBtn_, selectX, kFindRegionRowY, kFindBtnW, kFindBtnH, FALSE);
    }

    static int OcrScaleX(int designPx) {
        return ScaleX(designPx);
    }

    static int OcrScaleY(int designPx) {
        return ScaleY(designPx);
    }

    static int OcrScale(int designPx) { return OcrScaleX(designPx); }

    void MoveOcrAt(HWND hwnd, int xDesign, int yClient, int wDesign, int hDesign) const {
        if (!hwnd) return;
        EnsureParamViewportParent(hwnd);
        int w = std::max(1, OcrScaleX(wDesign));
        int h = std::max(1, OcrScaleY(hDesign));
        const int x = OcrScaleX(xDesign);
        if (wDesign == kFindImageSize && hDesign == kFindImageSize) {
            w = h = kFindImageSize;
        }
        const int maxRight = ParamScrollContentRight();
        if (x < maxRight && x + w > maxRight) {
            w = std::max(1, maxRight - x);
            if (wDesign == kFindImageSize && hDesign == kFindImageSize) h = w;
        }
        SetParamPosAware(hwnd, x, yClient, w, h,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    }

    int OcrTextButtonWidthDesign(HDC hdc, HFONT font, const wchar_t* text, int minDesignPx, int padDesignPx) const {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
        SIZE sz{};
        GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
        SelectObject(hdc, old);
        return std::max(minDesignPx, static_cast<int>(sz.cx) + padDesignPx);
    }

    void LayoutOcrFindImageSideStack(int previewTop, HWND screenshot, HWND local, HWND clear) const {
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        auto place = [&](HWND h, int index) {
            if (!h) return;
            MoveOcrAt(h, kFindActionBtnX, layout.SideBtnY(previewTop, index), kFindBtnW, kFindImageSideBtnH);
        };
        place(screenshot, 0);
        place(local, 1);
        place(clear, 2);
    }

    void PlaceOcrFindImageCompactButton(HWND btn, int yClient) const {
        if (!btn) return;
        MoveOcrAt(btn, kFindActionBtnX, yClient, kFindBtnW, kFindImageSideBtnH);
    }

    void PlaceAiFindImageCompactButton(HWND btn, int yClient) const {
        if (!btn) return;
        MoveAiRegionAt(btn, kFindActionBtnX, yClient, kFindBtnW, kFindImageSideBtnH);
    }

    void ShowFindImageSideControls(HWND preview, HWND screenshot, HWND local, HWND clear,
                                   HWND selectRegion = nullptr) const {
        auto show = [](HWND h) {
            if (!h) return;
            ShowWindow(h, SW_SHOW);
            SetWindowRgn(h, nullptr, TRUE);
        };
        show(selectRegion);
        show(preview);
        show(screenshot);
        show(local);
        show(clear);
    }

    void FinishGrayButtonClick(HWND ctrl) {
        if (!ctrl || !IsGrayButton(ctrl)) return;
        SendMessageW(ctrl, BM_SETSTATE, FALSE, 0);
        if (GetFocus() == ctrl) {
            HWND parent = GetParent(ctrl);
            if (parent) SetFocus(parent);
        }
        InvalidateGrayButton(ctrl);
        UpdateHoverFromCursor();
    }

    void LayoutAiFindImageSideStack(int previewTop, HWND screenshot, HWND local, HWND clear) const {
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        auto place = [&](HWND h, int index) {
            if (!h) return;
            MoveAiRegionAt(h, kFindActionBtnX, layout.SideBtnY(previewTop, index), kFindBtnW, kFindImageSideBtnH);
        };
        place(screenshot, 0);
        place(local, 1);
        place(clear, 2);
    }

    void SizeOcrRegionButtonsAt(int yClient) {
        if (!ocrFullScreenBtn_ || !ocrSelectRegionBtn_ || !ocrRegionLabel_) return;
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, L"识别区域", 4, &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(kFindRegionLabelW, static_cast<int>(labelSz.cx) + 4);
        const int fullX = kFindContentLeft + labelW + kFindRegionLabelGap;
        const int fullWMax = kFindSelectRegionX - kFindRegionBtnGap - fullX;
        const int fullWWant = std::max(32, static_cast<int>(fullSz.cx) + 10);
        const int fullW = std::max(32, std::min(fullWMax, fullWWant));
        const int btnH = OcrScale(kFindBtnH);
        MoveOcrAt(ocrRegionLabel_, kFindContentLeft, yClient, labelW, kFindBtnH);
        MoveOcrAt(ocrFullScreenBtn_, fullX, yClient, fullW, kFindBtnH);
        MoveOcrAt(ocrSelectRegionBtn_, kFindSelectRegionX, yClient, kFindBtnW, kFindBtnH);
        (void)btnH;
    }

    void SizeOcrRegionButtons() {
        SizeOcrRegionButtonsAt(OcrScale(kOcrRegionRowY));
    }

    bool IsParamComboVisible(int comboId) const {
        const int sel = popupAction_.sel;
        switch (comboId) {
        case 2: return sel == 5 || sel == 6;
        case 3: return sel == 2;
        case 4: return sel == 13;
        case 5: return sel == 16;
        case 6: return sel == 11;
        case 7: return sel == 12 || sel == 29 || sel == 30 || sel == 31;
        case 8: return sel == 4;
        case 9: return sel == 3;
        case 10: return sel == 7;
        case 11: return sel == 17;
        case 12: return sel == 19;
        case 13: return sel == 19;
        case 14: return sel == 19;
        case 15: return sel == 24;
        case 16: return sel == 18;
        case 17: return sel == 18;
        case 18: return sel == 18 && popupOcrResultMode_.sel == 1;
        case 19: return sel == 29 || sel == 30 || sel == 31;
        case 20: return sel == 29 || sel == 30 || sel == 31;
        case 21: return sel == 29 || sel == 30;
        case 22: return false; // region combo removed in favor of region selection button
        default: return false;
        }
    }

    void InitCrosshairDrag() {
        crosshairDrag_.SetOwner(hwnd_);
        crosshairDrag_.SetDragCursor(crosshairDragCursor_);
        crosshairDrag_.ClearButtons();
        crosshairDrag_.RegisterButton(crosshairBtn_, {CrosshairDragMode::Coordinates, nullptr});
        crosshairDrag_.RegisterButton(runProgramCrosshairBtn_, {CrosshairDragMode::ProgramPath, runProgramPath_});
        crosshairDrag_.RegisterButton(closeProgramCrosshairBtn_, {CrosshairDragMode::ProgramPath, closeProgramPath_});
        CrosshairDragBinding wmTargetBinding{};
        wmTargetBinding.mode = CrosshairDragMode::WindowTarget;
        wmTargetBinding.targetEdit = wmTargetPathEdit_;
        wmTargetBinding.onWindowTarget = [this](const WindowInfoFromPoint& info) {
            ApplyWindowModeTargetFromPoint(info);
        };
        crosshairDrag_.RegisterButton(wmTargetCrosshairBtn_, wmTargetBinding);
    }

    void ApplyWindowModeTargetFromPoint(const WindowInfoFromPoint& info) {
        if (!info.processPath.empty()) {
            scriptWindowMode_.targetExePath = info.processPath;
            if (wmTargetPathEdit_) SetText(wmTargetPathEdit_, info.processPath);
        }
        if (!info.windowTitle.empty()) {
            scriptWindowMode_.windowName = info.windowTitle;
            scriptWindowMode_.targetWindowTitle = info.windowTitle;
        }
        if (!info.windowClassName.empty()) scriptWindowMode_.windowClassName = info.windowClassName;
        if (!info.childWindowClassName.empty()) {
            scriptWindowMode_.childWindowClassName = info.childWindowClassName;
        }
        scriptWindowMode_.targetPickX = info.x;
        scriptWindowMode_.targetPickY = info.y;
        scriptWindowMode_.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
        SyncScriptWindowModeFromEditor();
    }

    void InitRunProgramPresets() {
        popupRunProgram_.items.clear();
        for (int i = 0; i < RunProgramPresetCount(); ++i) {
            popupRunProgram_.items.push_back(RunProgramPresetAt(i).label);
        }
        popupRunProgram_.sel = 0;
        if (runProgramCombo_) {
            SetText(runProgramCombo_, popupRunProgram_.items.empty() ? L"选择文件" : popupRunProgram_.items[0]);
        }
    }

    void UpdateRunProgramSubPanel() {
        RefreshParamPanel();
    }

    void RefreshRunBlockCombo() {
        if (!runBlockCombo_) return;
        std::wstring prevText = popupRunBlock_.sel >= 0 && popupRunBlock_.sel < static_cast<int>(popupRunBlock_.items.size())
            ? popupRunBlock_.items[static_cast<size_t>(popupRunBlock_.sel)] : GetText(runBlockCombo_);
        popupRunBlock_.items.clear();
        for (const auto& a : actions_) {
            if (a.type != ActionType::DefineBlock || a.blockName.empty()) continue;
            popupRunBlock_.items.push_back(a.blockName);
        }
        if (!prevText.empty()) {
            int idx = -1;
            for (int i = 0; i < static_cast<int>(popupRunBlock_.items.size()); ++i) {
                if (popupRunBlock_.items[static_cast<size_t>(i)] == prevText) { idx = i; break; }
            }
            popupRunBlock_.sel = std::max(-1, idx);
        } else if (!popupRunBlock_.items.empty()) {
            popupRunBlock_.sel = 0;
        } else {
            popupRunBlock_.sel = -1;
        }
        SetText(runBlockCombo_, popupRunBlock_.sel >= 0 && popupRunBlock_.sel < static_cast<int>(popupRunBlock_.items.size())
            ? popupRunBlock_.items[static_cast<size_t>(popupRunBlock_.sel)] : L"");
    }

    void RefreshRunMacroCombo() {
        if (!runMacroCombo_) return;
        LoadScripts();
        const std::wstring currentName = Trim(GetText(name_));
        const std::wstring prevText = popupRunMacro_.sel >= 0 && popupRunMacro_.sel < static_cast<int>(popupRunMacro_.items.size())
            ? popupRunMacro_.items[static_cast<size_t>(popupRunMacro_.sel)] : GetText(runMacroCombo_);
        popupRunMacro_.items.clear();
        runMacroPaths_.clear();
        for (const auto& script : scripts_) {
            if (script.name.empty()) continue;
            if (!currentName.empty() && script.name == currentName) continue;
            popupRunMacro_.items.push_back(script.name);
            runMacroPaths_.push_back(script.path);
        }
        if (!prevText.empty()) {
            int idx = -1;
            for (int i = 0; i < static_cast<int>(popupRunMacro_.items.size()); ++i) {
                if (popupRunMacro_.items[static_cast<size_t>(i)] == prevText) { idx = i; break; }
            }
            popupRunMacro_.sel = idx;
        } else if (!popupRunMacro_.items.empty()) {
            popupRunMacro_.sel = 0;
        } else {
            popupRunMacro_.sel = -1;
        }
        SetText(runMacroCombo_, popupRunMacro_.sel >= 0 && popupRunMacro_.sel < static_cast<int>(popupRunMacro_.items.size())
            ? popupRunMacro_.items[static_cast<size_t>(popupRunMacro_.sel)] : L"");
    }

    void RefreshMousePlaybackCombo() {
        if (!mousePlaybackCombo_) return;
        LoadRecordings();
        const std::wstring prevText = popupMousePlayback_.sel >= 0 && popupMousePlayback_.sel < static_cast<int>(popupMousePlayback_.items.size())
            ? popupMousePlayback_.items[static_cast<size_t>(popupMousePlayback_.sel)] : GetText(mousePlaybackCombo_);
        popupMousePlayback_.items.clear();
        mousePlaybackPaths_.clear();
        for (const auto& rec : recordings_) {
            if (rec.name.empty()) continue;
            popupMousePlayback_.items.push_back(rec.name);
            mousePlaybackPaths_.push_back(rec.path);
        }
        if (!prevText.empty()) {
            int idx = -1;
            for (int i = 0; i < static_cast<int>(popupMousePlayback_.items.size()); ++i) {
                if (popupMousePlayback_.items[static_cast<size_t>(i)] == prevText) { idx = i; break; }
            }
            popupMousePlayback_.sel = idx;
        } else if (!popupMousePlayback_.items.empty()) {
            popupMousePlayback_.sel = 0;
        } else {
            popupMousePlayback_.sel = -1;
        }
        SetText(mousePlaybackCombo_, popupMousePlayback_.sel >= 0 && popupMousePlayback_.sel < static_cast<int>(popupMousePlayback_.items.size())
            ? popupMousePlayback_.items[static_cast<size_t>(popupMousePlayback_.sel)] : L"");
    }

    int EditorComboPopupIdForHwnd(HWND hwnd) const {
        if (hwnd == mode_) return 0;
        if (hwnd == wmSelectMethod_ && IsEditorWindowModeActive()) return 23;
        if (hwnd == actionCombo_) return 1;
        if (hwnd == mousePressButton_ && IsParamComboVisible(2)) return 2;
        if (hwnd == clickButton_ && IsParamComboVisible(3)) return 3;
        if (hwnd == loopTypeCombo_ && IsParamComboVisible(4)) return 4;
        if (hwnd == runBlockCombo_ && IsParamComboVisible(5)) return 5;
        if (hwnd == hotkeyShortcutCombo_ && IsParamComboVisible(6)) return 6;
        if (hwnd == quickInputVarCombo_ || hwnd == aiVarCombo_) {
            return hwnd == ActiveVarComboHwnd() ? 7 : -1;
        }
        if (hwnd == runMacroCombo_ && IsParamComboVisible(8)) return 8;
        if (hwnd == mousePlaybackCombo_ && IsParamComboVisible(9)) return 9;
        if (hwnd == scrollDirectionCombo_ && IsParamComboVisible(10)) return 10;
        if (hwnd == findFollowUpCombo_ && IsParamComboVisible(11)) return 11;
        if (hwnd == ifVarCombo_ && IsParamComboVisible(12)) return 12;
        if (hwnd == ifOperatorCombo_ && IsParamComboVisible(13)) return 13;
        if (hwnd == ifConnectorCombo_ && IsParamComboVisible(14)) return 14;
        if (hwnd == runProgramCombo_ && IsParamComboVisible(15)) return 15;
        if (hwnd == ocrResultModeCombo_ && IsParamComboVisible(16)) return 16;
        if (hwnd == ocrFollowUpCombo_ && IsParamComboVisible(17)) return 17;
        if (hwnd == ocrSearchVarCombo_ && IsParamComboVisible(18)) return 18;
        if (hwnd == aiModelCombo_ && IsParamComboVisible(19)) return 19;
        if (hwnd == aiContextModeCombo_ && IsParamComboVisible(20)) return 20;
        if (hwnd == aiOutputTypeCombo_ && IsParamComboVisible(21)) return 21;
        if (hwnd == aiSearchRegionCombo_ && IsParamComboVisible(22)) return 22;
        return -1;
    }

    int EditorComboPopupIdAtPoint(int x, int y) const {
        if (mode_ && PtIn(WindowClientRect(mode_), x, y)) return 0;
        if (wmSelectMethod_ && IsEditorWindowModeActive() && PtIn(WindowClientRect(wmSelectMethod_), x, y)) return 23;
        if (actionCombo_ && PtIn(WindowClientRect(actionCombo_), x, y)) return 1;
        struct ComboHit { int id; HWND hwnd; };
        const ComboHit hits[] = {
            {2, mousePressButton_}, {3, clickButton_}, {4, loopTypeCombo_}, {5, runBlockCombo_},
            {6, hotkeyShortcutCombo_}, {8, runMacroCombo_}, {9, mousePlaybackCombo_},
            {10, scrollDirectionCombo_}, {11, findFollowUpCombo_},
            {12, ifVarCombo_}, {13, ifOperatorCombo_}, {14, ifConnectorCombo_}, {15, runProgramCombo_},
            {16, ocrResultModeCombo_}, {17, ocrFollowUpCombo_}, {18, ocrSearchVarCombo_},
            {19, aiModelCombo_}, {20, aiContextModeCombo_}, {21, aiOutputTypeCombo_}, {22, aiSearchRegionCombo_},
        };
        for (const auto& hit : hits) {
            if (!hit.hwnd || !IsParamComboVisible(hit.id)) continue;
            if (PtIn(EditorComboClientRect(hit.hwnd), x, y)) return hit.id;
        }
        if (IsParamComboVisible(7)) {
            if (HWND varCombo = ActiveVarComboHwnd()) {
                if (PtIn(EditorComboClientRect(varCombo), x, y)) return 7;
            }
        }
        return -1;
    }

    void InitHotkeyShortcutPresets() {
        popupHotkeyShortcut_.items.clear();
        for (int i = 0; i < ShortcutPresetCount(); ++i) {
            popupHotkeyShortcut_.items.push_back(ShortcutPresetAt(i).label);
        }
        popupHotkeyShortcut_.sel = 0;
        if (hotkeyShortcutCombo_) SetText(hotkeyShortcutCombo_, popupHotkeyShortcut_.items.empty() ? L"" : popupHotkeyShortcut_.items[0]);
    }

    void RebuildQuickInputVarPopup() {
        quickInputVarItems_ = BuildQuickInputVarItems(actions_);
        popupQuickInputVar_.items.clear();
        for (const auto& item : quickInputVarItems_) {
            popupQuickInputVar_.items.push_back(item.display);
        }
        if (popupQuickInputVar_.sel < 0 || popupQuickInputVar_.sel >= static_cast<int>(popupQuickInputVar_.items.size())) {
            popupQuickInputVar_.sel = popupQuickInputVar_.items.empty() ? -1 : 0;
        }
    }

    std::wstring QuickInputVarPopupDisplayText() const {
        return popupQuickInputVar_.sel >= 0 && popupQuickInputVar_.sel < static_cast<int>(popupQuickInputVar_.items.size())
            ? popupQuickInputVar_.items[static_cast<size_t>(popupQuickInputVar_.sel)] : L"";
    }

    void SyncSharedVarComboVisibility() {
        const int sel = popupAction_.sel;
        if (quickInputVarCombo_) {
            ShowWindow(quickInputVarCombo_, sel == 12 ? SW_SHOW : SW_HIDE);
        }
        if (aiVarCombo_) {
            ShowWindow(aiVarCombo_, (sel == 29 || sel == 30 || sel == 31) ? SW_SHOW : SW_HIDE);
        }
    }

    void RefreshActiveVarCombo() {
        RebuildQuickInputVarPopup();
        HWND active = ActiveVarComboHwnd();
        if (!active) return;
        SetText(active, QuickInputVarPopupDisplayText());
        InvalidateEditorComboArea(7);
    }

    void RefreshQuickInputVarCombo() {
        RefreshActiveVarCombo();
    }

    void RefreshIfVarCombo() {
        quickInputVarItems_ = BuildQuickInputVarItems(actions_);
        popupIfVar_.items.clear();
        for (const auto& item : quickInputVarItems_) {
            popupIfVar_.items.push_back(item.display);
        }
        if (popupIfVar_.sel < 0 || popupIfVar_.sel >= static_cast<int>(popupIfVar_.items.size())) {
            popupIfVar_.sel = popupIfVar_.items.empty() ? -1 : 0;
        }
        SetText(ifVarCombo_, popupIfVar_.sel >= 0 && popupIfVar_.sel < static_cast<int>(popupIfVar_.items.size())
            ? popupIfVar_.items[static_cast<size_t>(popupIfVar_.sel)] : L"");
    }

    void AppendIfCondition() {
        if (!ifConditionList_) return;
        const int varSel = popupIfVar_.sel;
        if (varSel < 0 || varSel >= static_cast<int>(quickInputVarItems_.size())) return;
        static const wchar_t* opSymbols[] = { L"==", L"!=", L"<", L"<=", L">", L">=", L">>" };
        const int opSel = std::clamp(popupIfOperator_.sel, 0, 6);
        static const wchar_t* connectors[] = { L"and", L"or", L"not" };
        const int connSel = std::clamp(popupIfConnector_.sel, 0, 2);
        const std::wstring varCode = quickInputVarItems_[static_cast<size_t>(varSel)].codeHint;
        const std::wstring value = Trim(GetText(ifValueEdit_));
        const std::wstring clause = varCode + opSymbols[opSel] + value;
        std::wstring current = GetText(ifConditionList_);
        if (Trim(current).empty()) {
            SetText(ifConditionList_, clause);
        } else {
            while (!current.empty() && (current.back() == L'\r' || current.back() == L'\n')) current.pop_back();
            current += L" ";
            current += connectors[connSel];
            current += L"\r\n";
            current += clause;
            SetText(ifConditionList_, current);
        }
        SetFocus(ifConditionList_);
    }

    void OnActionsChanged() {
        MarkVisibleActionsDirty();
        if (editorPopupOpen_ == 7) CloseEditorPopup();
        const int sel = popupAction_.sel;
        if (sel == 12 || sel == 29 || sel == 30 || sel == 31) RefreshActiveVarCombo();
        if (sel == 18) RefreshOcrSearchVarCombo();
        if (sel == 19) RefreshIfVarCombo();
        if (page_ == Page::Editor) {
            SyncSharedVarComboVisibility();
            RepaintParamPanelChrome();
        }
    }

    HWND ActiveVarComboHwnd() const {
        const int sel = popupAction_.sel;
        if (sel == 12) return quickInputVarCombo_;
        if (sel == 29 || sel == 30 || sel == 31) return aiVarCombo_;
        return nullptr;
    }

    void InsertTextAtEditSelection(HWND edit, const std::wstring& insert) {
        if (!edit || insert.empty()) return;
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(insert.c_str()));
        SetFocus(edit);
    }

    void InsertQuickInputVariable() {
        if (!quickInputEdit_) return;
        const int sel = popupQuickInputVar_.sel;
        if (sel < 0 || sel >= static_cast<int>(quickInputVarItems_.size())) return;
        InsertTextAtEditSelection(quickInputEdit_, quickInputVarItems_[static_cast<size_t>(sel)].insertText);
    }

    void InsertAiPromptVariable() {
        if (!aiPromptEdit_) return;
        const int sel = popupQuickInputVar_.sel;
        if (sel < 0 || sel >= static_cast<int>(quickInputVarItems_.size())) return;
        InsertTextAtEditSelection(aiPromptEdit_, quickInputVarItems_[static_cast<size_t>(sel)].insertText);
    }

    void RefreshAiVarCombo() {
        RefreshActiveVarCombo();
    }

    void RefreshOcrSearchVarCombo() {
        quickInputVarItems_ = BuildQuickInputVarItems(actions_);
        popupOcrSearchVar_.items.clear();
        for (const auto& item : quickInputVarItems_) {
            popupOcrSearchVar_.items.push_back(item.display);
        }
        if (popupOcrSearchVar_.sel < 0 || popupOcrSearchVar_.sel >= static_cast<int>(popupOcrSearchVar_.items.size())) {
            popupOcrSearchVar_.sel = popupOcrSearchVar_.items.empty() ? -1 : 0;
        }
        SetText(ocrSearchVarCombo_, popupOcrSearchVar_.sel >= 0 && popupOcrSearchVar_.sel < static_cast<int>(popupOcrSearchVar_.items.size())
            ? popupOcrSearchVar_.items[static_cast<size_t>(popupOcrSearchVar_.sel)] : L"");
    }

    void RefreshAiModelCombo() {
        popupAiModel_.items.clear();
        const auto& models = appSettings_.ai.savedModels;
        for (const auto& m : models) {
            popupAiModel_.items.push_back(m.modelName);
        }
        if (popupAiModel_.items.empty()) {
            popupAiModel_.items.push_back(appSettings_.ai.modelName);
        }
        if (popupAiModel_.sel < 0 || popupAiModel_.sel >= static_cast<int>(popupAiModel_.items.size())) {
            // ★默认选「设置→AI助手」里的当前模型，而不是列表第 1 个：
            // 列表顺序是用户添加顺序，默认第 1 个会让新建 AI 动作在用户不知情的情况下
            // 用上另一个模型（实测：用户选了 deepseek-v4.1-flash，新建动作却默认了豆包）。
            int defIdx = 0;
            const std::wstring& def = appSettings_.ai.modelName;
            if (!def.empty()) {
                for (size_t i = 0; i < popupAiModel_.items.size(); ++i) {
                    if (popupAiModel_.items[i] == def) { defIdx = static_cast<int>(i); break; }
                }
            }
            popupAiModel_.sel = popupAiModel_.items.empty() ? -1 : defIdx;
        }
        SetText(aiModelCombo_, popupAiModel_.sel >= 0 && popupAiModel_.sel < static_cast<int>(popupAiModel_.items.size())
            ? popupAiModel_.items[static_cast<size_t>(popupAiModel_.sel)] : L"");
    }

    void MoveAiAt(HWND hwnd, int x, int y, int w, int h) const {
        if (hwnd) {
            EnsureParamViewportParent(hwnd);
            SetParamPosAware(hwnd, x, y, w, h,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
    }

    void MoveAiAtVisible(HWND hwnd, int x, int y, int w, int h) const {
        MoveAiAt(hwnd, x, y, w, h);
        if (hwnd) ShowWindow(hwnd, SW_SHOW);
    }

    void LayoutParamRegionButtons(HWND regionLabel, HWND fullBtn, HWND selectBtn, int y) {
        if (!regionLabel || !fullBtn || !selectBtn) return;
        const int left = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int btnH = ScaleY(kFindBtnH);
        const int btnGap = ScaleX(kFindRegionBtnGap);
        const int labelGap = ScaleX(kFindRegionLabelGap);
        const int selectW = ScaleX(kFindBtnW);
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        wchar_t labelBuf[32]{};
        GetWindowTextW(regionLabel, labelBuf, 32);
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, labelBuf, static_cast<int>(wcslen(labelBuf)), &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(ScaleX(kFindRegionLabelW),
            static_cast<int>(labelSz.cx) + 4);
        const int fullWWant = std::max(44, static_cast<int>(fullSz.cx) + ScaleX(18));
        const int availW = maxRight - left;
        const int fullWMax = std::max(44, availW - labelW - labelGap - btnGap - selectW);
        const int fullW = std::max(44, std::min(fullWMax, fullWWant));
        const int totalW = labelW + labelGap + fullW + btnGap + selectW;
        const int startX = left + std::max(0, (availW - totalW) / 2);
        MoveParamAware(regionLabel, startX, y, labelW, btnH, FALSE);
        MoveParamAware(fullBtn, startX + labelW + labelGap, y, fullW, btnH, FALSE);
        MoveParamAware(selectBtn, startX + labelW + labelGap + fullW + btnGap, y, selectW, btnH, FALSE);
        ShowWindow(regionLabel, SW_SHOW);
        ShowWindow(fullBtn, SW_SHOW);
        ShowWindow(selectBtn, SW_SHOW);
    }

    int LayoutParamCoordPairRow(
        int y,
        HWND xLabel, HWND xEdit, HWND yLabel, HWND yEdit,
        int labelWDesign = kFindCoordLabelW) const {
        const int left = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int fieldH = ScaleY(22);
        const int rowGap = ScaleY(kFindVGap);
        const int labelW = ScaleX(labelWDesign);
        const int editW = ScaleX(kFindEditW);
        const int labelEditGap = ScaleX(kFindCoordLabelEditGap);
        const int pairGap = ScaleX(kFindCoordPairGap);
        const int pairW = labelW + labelEditGap + editW;
        const int totalW = pairW * 2 + pairGap;
        const int startX = left + std::max(0, (maxRight - left - totalW) / 2);

        int x = startX;
        auto mv = [&](HWND h, int w) {
            if (!h) return;
            MoveParamAware(h, x, y, w, fieldH, FALSE);
            ShowWindow(h, SW_SHOW);
            x += w;
        };
        mv(xLabel, labelW);
        x += labelEditGap;
        mv(xEdit, editW);
        x += pairGap;
        mv(yLabel, labelW);
        x += labelEditGap;
        mv(yEdit, editW);
        return y + fieldH + rowGap;
    }

    int LayoutParamCoordRows(
        int y,
        HWND x1Label, HWND x1Edit, HWND y1Label, HWND y1Edit,
        HWND x2Label, HWND x2Edit, HWND y2Label, HWND y2Edit) {
        y = LayoutParamCoordPairRow(y, x1Label, x1Edit, y1Label, y1Edit);
        y = LayoutParamCoordPairRow(y, x2Label, x2Edit, y2Label, y2Edit);
        return y;
    }

    static int AiScaleX(int designPx) {
        return ScaleX(designPx);
    }

    static int AiScaleY(int designPx) {
        return ScaleY(designPx);
    }

    int AiPanelLeft() const { return ParamPanelLeft(); }
    int AiPanelWidth() const { return ParamFieldMaxWidth(); }

    static int AiRegionXDesign(int xFromFindContent) {
        return xFromFindContent + (kParamScrollLeftDesign - kFindContentLeft);
    }

    void MoveAiRegionAt(HWND hwnd, int xDesign, int yClient, int wDesign, int hDesign) const {
        MoveOcrAt(hwnd, AiRegionXDesign(xDesign), yClient, wDesign, hDesign);
    }

    void SizeAiRegionButtonsAt(HWND regionLabel, HWND fullBtn, HWND selectBtn, int yClient) {
        if (!regionLabel || !fullBtn || !selectBtn) return;
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        wchar_t labelBuf[32]{};
        GetWindowTextW(regionLabel, labelBuf, 32);
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, labelBuf, static_cast<int>(wcslen(labelBuf)), &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(kFindRegionLabelW, static_cast<int>(labelSz.cx) + 4);
        const int fullX = kFindContentLeft + labelW + kFindRegionLabelGap;
        const int fullWMax = kFindSelectRegionX - kFindRegionBtnGap - fullX;
        const int fullWWant = std::max(32, static_cast<int>(fullSz.cx) + 10);
        const int fullW = std::max(32, std::min(fullWMax, fullWWant));
        MoveAiRegionAt(regionLabel, kFindContentLeft, yClient, labelW, kFindBtnH);
        MoveAiRegionAt(fullBtn, fullX, yClient, fullW, kFindBtnH);
        MoveAiRegionAt(selectBtn, kFindSelectRegionX, yClient, kFindBtnW, kFindBtnH);
    }

    int LayoutAiFindRegionBlock(int y) {
        HideAiFindRegionFloatingControls();
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        const int fieldH = OcrScale(22);
        const int rowGap = OcrScale(kFindVGap);

        MoveAiRegionAt(aiFindImagePreviewBtn_, kFindContentLeft, y, kFindImageSize, kFindImageSize);
        LayoutAiFindImageSideStack(y, aiFindScreenshotBtn_, aiFindLocalImageBtn_, aiFindClearImageBtn_);
        if (aiFindImagePreviewBtn_) {
            SetWindowPos(aiFindImagePreviewBtn_, HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        ShowFindImageSideControls(
            aiFindImagePreviewBtn_, aiFindScreenshotBtn_, aiFindLocalImageBtn_, aiFindClearImageBtn_);
        y += kFindImageSize + layout.sideGap;

        MoveAiRegionAt(aiFindMatchThreshold_, kFindContentLeft + 91, y, 40, 22);
        if (aiFindMatchLabel_) {
            MoveAiRegionAt(aiFindMatchLabel_, kFindContentLeft, y, 90, 22);
            ShowWindow(aiFindMatchLabel_, SW_SHOW);
        }
        if (aiFindMatchPctLabel_) {
            MoveAiRegionAt(aiFindMatchPctLabel_, kFindContentLeft + 135, y, 24, 22);
            ShowWindow(aiFindMatchPctLabel_, SW_SHOW);
        }
        if (aiFindMatchThreshold_) ShowWindow(aiFindMatchThreshold_, SW_SHOW);
        y += fieldH + rowGap;

        MoveAiRegionAt(aiFindScaleMin_, kFindContentLeft + 65, y, 40, 22);
        MoveAiRegionAt(aiFindScaleMax_, kFindContentLeft + 151, y, 40, 22);
        if (aiFindScaleMinLabel_) {
            MoveAiRegionAt(aiFindScaleMinLabel_, kFindContentLeft, y, 64, 22);
            ShowWindow(aiFindScaleMinLabel_, SW_SHOW);
        }
        if (aiFindScaleMaxLabel_) {
            MoveAiRegionAt(aiFindScaleMaxLabel_, kFindContentLeft + 110, y, 40, 22);
            ShowWindow(aiFindScaleMaxLabel_, SW_SHOW);
        }
        if (aiFindScaleMin_) ShowWindow(aiFindScaleMin_, SW_SHOW);
        if (aiFindScaleMax_) ShowWindow(aiFindScaleMax_, SW_SHOW);
        y += fieldH + rowGap;

        if (aiFindSelectRegionBtn_) {
            MoveAiRegionAt(aiFindSelectRegionBtn_, kFindContentLeft, y, kFindBlockW, 28);
            ShowWindow(aiFindSelectRegionBtn_, SW_SHOW);
        }
        y += OcrScale(28) + rowGap;
        HWND lx1 = nullptr, ly1 = nullptr, lx2 = nullptr, ly2 = nullptr;
        for (HWND h : aiFindRegionControls_) {
            if (!h) continue;
            wchar_t buf[8]{};
            GetWindowTextW(h, buf, 8);
            if (wcscmp(buf, L"X1") == 0 && !lx1) lx1 = h;
            else if (wcscmp(buf, L"Y1") == 0 && !ly1) ly1 = h;
            else if (wcscmp(buf, L"X2") == 0 && !lx2) lx2 = h;
            else if (wcscmp(buf, L"Y2") == 0 && !ly2) ly2 = h;
        }
        return LayoutParamCoordRows(
            y,
            lx1, aiImageRegionX1_, ly1, aiImageRegionY1_,
            lx2, aiImageRegionX2_, ly2, aiImageRegionY2_);
    }

    int LayoutAiCommonStack(int y, int sel) {
        const int left = ParamPanelLeft();
        const int fullW = ParamFieldMaxWidth();
        const int rowGap = ScaleY(8);
        const int labelGap = ScaleY(6);
        const int fieldH = ScaleY(22);
        const int comboH = ScaleY(21);
        const int btnH = ScaleY(28);
        const int textFieldH = ScaleY(EditorParamLayout::kPanelTextFieldH);
        const int hintH = ScaleY(28);
        const int blockGap = ScaleY(12);
        const bool actionExecute = sel == 31;

        const int promptLabelH = ScaleY(kEditorLabelAboveComboH);
        MoveAiAtVisible(aiPromptLabel_, left, y, fullW, promptLabelH);
        y += promptLabelH + labelGap;

        MoveAiAtVisible(aiPromptEdit_, left, y, fullW, textFieldH);
        y += textFieldH + rowGap;

        if (actionExecute) {
            MoveAiAtVisible(aiMaxStepsLabel_, left, y, fullW, fieldH);
            y += fieldH + labelGap;
            MoveAiAtVisible(aiMaxStepsEdit_, left, y, fullW, fieldH);
            y += fieldH + ScaleY(4);
            MoveAiAtVisible(aiMaxStepsHint_, left, y, fullW, hintH);
            y += hintH + rowGap;
        } else {
            for (HWND h : {aiMaxStepsLabel_, aiMaxStepsEdit_, aiMaxStepsHint_}) ParkParamControl(h);
        }

        MoveAiAtVisible(aiVarLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiVarCombo_, left, y, fullW, comboH);
        y += comboH + rowGap;
        MoveAiAtVisible(aiInsertVarBtn_, left, y, fullW, btnH);
        y += btnH + blockGap;

        MoveAiAtVisible(aiModelLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiModelCombo_, left, y, fullW, comboH);
        y += comboH + rowGap;

        MoveAiAtVisible(aiContextLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiContextModeCombo_, left, y, fullW, comboH);
        y += comboH + rowGap;

        MoveAiAtVisible(aiTimeoutLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiTimeoutEdit_, left, y, fullW, fieldH);
        y += fieldH + rowGap;

        MoveAiAtVisible(aiFallbackLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiFallbackEdit_, left, y, fullW, fieldH);
        y += fieldH + rowGap;

        if (actionExecute && aiWithImageCheck_) {
            MoveAiAtVisible(aiWithImageCheck_, left, y, fullW, AiScaleY(25));
            y += AiScaleY(25) + rowGap;
        } else if (aiWithImageCheck_) {
            ParkParamControl(aiWithImageCheck_);
        }
        // 逻辑转化放到动作详情最底部（见 RefreshAiSubPanel），此处先停放
        if (aiLogicConvertCheck_) ParkParamControl(aiLogicConvertCheck_);

        if (!actionExecute) {
            MoveAiAtVisible(aiOutputVarLabel_, left, y, fullW, fieldH);
            y += fieldH + labelGap;
            MoveAiAtVisible(aiOutputVarEdit_, left, y, fullW, fieldH);
            y += fieldH + rowGap;
            MoveAiAtVisible(aiOutputTypeLabel_, left, y, fullW, fieldH);
            y += fieldH + labelGap;
            MoveAiAtVisible(aiOutputTypeCombo_, left, y, fullW, comboH);
            y += comboH;
        } else {
            for (HWND h : {aiOutputVarLabel_, aiOutputVarEdit_, aiOutputTypeLabel_, aiOutputTypeCombo_}) {
                if (h) ParkParamControl(h);
            }
        }
        return y;
    }

    int LayoutAiImageFields(int y) {
        const int left = ParamPanelLeft();
        const int fullW = ParamFieldMaxWidth();
        const int rowGap = ScaleY(8);
        const int labelGap = ScaleY(6);
        const int fieldH = ScaleY(22);

        MoveAiAtVisible(aiImageScaleLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiImageScaleEdit_, left, y, fullW, fieldH);
        y += fieldH + rowGap;
        return y;
    }

    int LayoutAiRegionSection(
        bool regionByImage,
        HWND regionByImageCheck,
        HWND regionLabel,
        HWND fullBtn,
        HWND selectBtn,
        HWND x1Label, HWND x1Edit, HWND y1Label, HWND y1Edit,
        HWND x2Label, HWND x2Edit, HWND y2Label, HWND y2Edit,
        int y) {
        const int rowGap = OcrScale(kFindVGap);
        const int btnH = OcrScale(kFindBtnH);

        // 绝对识别区域始终在前
        LayoutParamRegionButtons(regionLabel, fullBtn, selectBtn, y);
        y += btnH + rowGap;
        y = LayoutParamCoordRows(
            y,
            x1Label, x1Edit, y1Label, y1Edit,
            x2Label, x2Edit, y2Label, y2Edit);

        if (regionByImageCheck) {
            MoveParamAware(regionByImageCheck, ParamPanelLeft(), y, ParamFieldMaxWidth(), OcrScale(22), FALSE);
            ShowWindow(regionByImageCheck, SW_SHOW);
            y += OcrScale(22) + rowGap;
        }

        if (regionByImage) {
            HideAiFindRegionFloatingControls();
            const FindImageSideButtonLayout sideLayout = ComputeFindImageSideButtonLayout();
            const int headerRowY = y;
            const int headerLabelH = OcrScale(kFindBtnH);
            if (aiFindImageLabel_) {
                MoveAiRegionAt(aiFindImageLabel_, kFindContentLeft,
                    headerRowY + std::max(0, (sideLayout.compactBtnH - headerLabelH) / 2), 90, kFindBtnH);
                ShowWindow(aiFindImageLabel_, SW_SHOW);
            }
            y += sideLayout.compactBtnH + sideLayout.sideGap;
            y = LayoutAiFindRegionBlock(y);
            RaiseAiFindImageHeaderControls();
        } else {
            ShowParamLayout(320, false);
            HideAiFindRegionControls();
        }
        return y;
    }

    void HideAiActionExecuteRegionControls() {
        ShowParamLayout(320, false);
        HideAiFindRegionControls();
        for (HWND h : {
            aiRegionByImageCheck2_, aiActionRegionLabel_, aiFullScreenBtn2_, aiSelectRegionBtn2_,
            aiActCoordX1Label_, aiSearchX1Edit2_, aiActCoordY1Label_, aiSearchY1Edit2_,
            aiActCoordX2Label_, aiSearchX2Edit2_, aiActCoordY2Label_, aiSearchY2Edit2_}) {
            if (h) ParkParamControl(h);
        }
    }

    void RefreshAiSubPanel() {
        const int sel = popupAction_.sel;
        if (sel != 29 && sel != 30 && sel != 31) {
            HideAiFindRegionControls();
            return;
        }

        const bool imageMode = sel == 30;
        const bool actionExecuteMode = sel == 31;
        const bool withImage = actionExecuteMode && aiWithImageCheck_ && Checked(aiWithImageCheck_);
        const bool regionByImage = imageMode
            ? (aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_))
            : (withImage && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_));

        HideAiFindRegionControls();
        ParkParamGroup(aiImageControls_);
        ParkParamGroup(aiActionControls_);

        const int sectionGap = ScaleY(12);
        const int rowGap = ScaleY(8);
        int y = ParamPanelContentTopY();
        y = LayoutAiCommonStack(y, sel);

        if (imageMode) {
            y += sectionGap;
            y = LayoutAiImageFields(y);
            y = LayoutAiRegionSection(
                regionByImage,
                aiRegionByImageCheck_,
                aiRegionLabel_, aiFullScreenBtn_, aiSelectRegionBtn_,
                aiCoordX1Label_, aiSearchX1Edit_, aiCoordY1Label_, aiSearchY1Edit_,
                aiCoordX2Label_, aiSearchX2Edit_, aiCoordY2Label_, aiSearchY2Edit_,
                y);
        } else if (actionExecuteMode) {
            y += sectionGap;
            if (withImage) {
                y = LayoutAiRegionSection(
                    regionByImage,
                    aiRegionByImageCheck2_,
                    aiActionRegionLabel_, aiFullScreenBtn2_, aiSelectRegionBtn2_,
                    aiActCoordX1Label_, aiSearchX1Edit2_, aiActCoordY1Label_, aiSearchY1Edit2_,
                    aiActCoordX2Label_, aiSearchX2Edit2_, aiActCoordY2Label_, aiSearchY2Edit2_,
                    y);
            } else {
                HideAiActionExecuteRegionControls();
            }
            // 逻辑转化：动作详情最底部，紧邻其附属控件
            if (aiLogicConvertCheck_) {
                y += sectionGap;
                MoveAiAtVisible(aiLogicConvertCheck_, ParamPanelLeft(), y,
                    ParamFieldMaxWidth(), AiScaleY(25));
                y += AiScaleY(25) + rowGap;
            }
        }

        if (regionByImage && (imageMode || withImage)) RefreshGrayButtonsInParamViewport();
        paramScrollY_ = 0;
        SyncParamScrollLayout(y);
    }

    void InsertOcrSearchVariable() {
        if (!ocrSearchEdit_) return;
        const int sel = popupOcrSearchVar_.sel;
        if (sel < 0 || sel >= static_cast<int>(quickInputVarItems_.size())) return;
        const std::wstring insert = quickInputVarItems_[static_cast<size_t>(sel)].insertText;
        DWORD start = 0, end = 0;
        SendMessageW(ocrSearchEdit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        const std::wstring current = GetText(ocrSearchEdit_);
        const std::wstring updated = current.substr(0, start) + insert + current.substr(end);
        SetText(ocrSearchEdit_, updated);
        const DWORD pos = start + static_cast<DWORD>(insert.size());
        SendMessageW(ocrSearchEdit_, EM_SETSEL, pos, pos);
        SetFocus(ocrSearchEdit_);
        if (popupAction_.sel == 18) { RefreshOcrSubPanel(); SyncParamScrollLayout(); }
    }

    int LayoutOcrSearchBlock(int y, int rowGap, int btnH, int fieldH) {
        (void)btnH;
        MoveOcrAt(ocrSearchLabel_, kFindContentLeft, y, kOcrSearchVarLabelW, 22);
        MoveOcrAt(ocrSearchEdit_, kOcrSearchEditX, y, kOcrSearchEditW, 22);
        y += fieldH + rowGap;

        HDC hdc = GetDC(hwnd_);
        const int insertW = OcrTextButtonWidthDesign(hdc, editorFont_ ? editorFont_ : font_, L"插入", kOcrInsertBtnW, 16);
        ReleaseDC(hwnd_, hdc);
        const int insertX = kOcrPanelRight - insertW;
        const int comboX = kFindContentLeft + kOcrSearchVarLabelW + kOcrVarComboGap;
        const int comboW = std::max(40, insertX - kOcrVarInsertGap - comboX);
        const int rowH = OcrScale(kOcrCompactBtnH);
        MoveOcrAt(ocrSearchVarLabel_, kFindContentLeft, y + (rowH - fieldH) / 2, kOcrSearchVarLabelW, 22);
        MoveOcrAt(ocrSearchVarCombo_, comboX, y + (rowH - OcrScale(kFindBtnH)) / 2, comboW, kFindBtnH);
        MoveOcrAt(ocrSearchVarInsertBtn_, insertX, y, insertW, kOcrCompactBtnH);
        return y + rowH + rowGap;
    }

    int LayoutOcrFindRegionBlock(int y) {
        HideOcrFindImageFloatingControls();
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        const int fieldH = OcrScaleY(22);
        const int rowGap = OcrScaleY(kFindVGap);
        auto show = [](HWND h) {
            if (!h) return;
            ShowWindow(h, SW_SHOW);
            SetWindowRgn(h, nullptr, TRUE);
        };
        for (HWND h : {ocrFindImagePreviewBtn_, ocrFindScreenshotBtn_, ocrFindLocalImageBtn_, ocrFindClearImageBtn_}) {
            if (h && IsWindowVisible(h)) InvalidateParamControlInViewport(h);
        }
        MoveOcrAt(ocrFindImagePreviewBtn_, kFindContentLeft, y, kFindImageSize, kFindImageSize);
        show(ocrFindImagePreviewBtn_);
        LayoutOcrFindImageSideStack(y, ocrFindScreenshotBtn_, ocrFindLocalImageBtn_, ocrFindClearImageBtn_);
        show(ocrFindScreenshotBtn_);
        show(ocrFindLocalImageBtn_);
        show(ocrFindClearImageBtn_);
        y += kFindImageSize + layout.sideGap;

        MoveOcrAt(ocrFindMatchThreshold_, kFindContentLeft + 91, y, 40, 22);
        show(ocrFindMatchThreshold_);
        for (HWND h : ocrFindRegionControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"范围") == 0) {
                MoveOcrAt(h, kFindContentLeft, y, 90, 22);
                show(h);
            } else if (wcscmp(buf, L"%") == 0) {
                MoveOcrAt(h, kFindContentLeft + 135, y, 24, 22);
                show(h);
            }
        }
        y += fieldH + rowGap;

        MoveOcrAt(ocrFindScaleMin_, kFindContentLeft + 65, y, 40, 22);
        MoveOcrAt(ocrFindScaleMax_, kFindContentLeft + 151, y, 40, 22);
        show(ocrFindScaleMin_);
        show(ocrFindScaleMax_);
        for (HWND h : ocrFindRegionControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"最小缩放") == 0) {
                MoveOcrAt(h, kFindContentLeft, y, 64, 22);
                show(h);
            } else if (wcscmp(buf, L"最大") == 0) {
                MoveOcrAt(h, kFindContentLeft + 110, y, 40, 22);
                show(h);
            }
        }
        y += fieldH + rowGap;

        // 选取区域位置（整排）+ 模板内相对偏移
        if (ocrFindSelectRegionBtn_) {
            MoveOcrAt(ocrFindSelectRegionBtn_, kFindContentLeft, y, kFindBlockW, kFindBtnH);
            show(ocrFindSelectRegionBtn_);
            SetWindowPos(ocrFindSelectRegionBtn_, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        y += OcrScaleY(kFindBtnH) + rowGap;
        HWND lx1 = nullptr, ly1 = nullptr, lx2 = nullptr, ly2 = nullptr;
        for (HWND h : ocrFindRegionControls_) {
            if (!h) continue;
            wchar_t buf[8]{};
            GetWindowTextW(h, buf, 8);
            if (wcscmp(buf, L"X1") == 0 && !lx1) lx1 = h;
            else if (wcscmp(buf, L"Y1") == 0 && !ly1) ly1 = h;
            else if (wcscmp(buf, L"X2") == 0 && !lx2) lx2 = h;
            else if (wcscmp(buf, L"Y2") == 0 && !ly2) ly2 = h;
        }
        return LayoutParamCoordRows(
            y,
            lx1, ocrImageRegionX1_, ly1, ocrImageRegionY1_,
            lx2, ocrImageRegionX2_, ly2, ocrImageRegionY2_);
    }

    void HideOcrDynamicRegionControls() const {
        for (HWND h : {ocrRegionLabel_, ocrFullScreenBtn_, ocrSelectRegionBtn_,
                       ocrFindSelectRegionBtn_, ocrFindImageLabel_}) {
            if (!h) continue;
            if (IsWindowVisible(h)) InvalidateParamControlInViewport(h);
            ShowWindow(h, SW_HIDE);
        }
        for (HWND h : ocrFindRegionControls_) {
            if (!h) continue;
            if (IsWindowVisible(h)) InvalidateParamControlInViewport(h);
            ShowWindow(h, SW_HIDE);
        }
    }

    void HideFindImageFloatingControls() const {
        for (HWND h : {findTestBtn_, findImagePreviewBtn_, findScreenshotBtn_,
                       findLocalImageBtn_, findClearImageBtn_}) {
            if (!h) continue;
            if (IsWindowVisible(h)) InvalidateParamControlInViewport(h);
            ShowWindow(h, SW_HIDE);
        }
    }

    void HideAiFindRegionFloatingControls() const {
        for (HWND h : {aiFindImagePreviewBtn_, aiFindScreenshotBtn_,
                       aiFindLocalImageBtn_, aiFindClearImageBtn_}) {
            if (!h) continue;
            if (IsWindowVisible(h)) InvalidateParamControlInViewport(h);
            ShowWindow(h, SW_HIDE);
        }
    }

    void HideOcrFindImageFloatingControls() const {
        for (HWND h : {ocrFindImagePreviewBtn_, ocrFindScreenshotBtn_,
                       ocrFindLocalImageBtn_, ocrFindClearImageBtn_}) {
            if (!h) continue;
            if (IsWindowVisible(h)) InvalidateParamControlInViewport(h);
            ShowWindow(h, SW_HIDE);
        }
    }

    int LayoutOcrFindImageHeaderRow(int y) {
        const FindImageSideButtonLayout sideLayout = ComputeFindImageSideButtonLayout();
        const int headerRowY = y;
        const int headerLabelH = OcrScale(kFindBtnH);
        if (ocrFindImageLabel_) {
            MoveOcrAt(ocrFindImageLabel_, kFindContentLeft,
                headerRowY + std::max(0, (sideLayout.compactBtnH - headerLabelH) / 2),
                90, kFindBtnH);
            ShowWindow(ocrFindImageLabel_, SW_SHOW);
            SetWindowPos(ocrFindImageLabel_, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return y + sideLayout.compactBtnH + sideLayout.sideGap;
    }

    int LayoutOcrRegionModeRow(int y) {
        SizeOcrRegionButtonsAt(y);
        if (ocrRegionLabel_) ShowWindow(ocrRegionLabel_, SW_SHOW);
        if (ocrFullScreenBtn_) ShowWindow(ocrFullScreenBtn_, SW_SHOW);
        if (ocrSelectRegionBtn_) ShowWindow(ocrSelectRegionBtn_, SW_SHOW);
        return y + OcrScaleY(kFindBtnH) + OcrScaleY(kFindVGap);
    }

    bool OcrSearchTextContainsVariable() const {
        if (!ocrSearchEdit_) return false;
        const std::wstring text = GetText(ocrSearchEdit_);
        const size_t open = text.find(L'{');
        if (open == std::wstring::npos) return false;
        const size_t close = text.find(L'}', open + 1);
        return close != std::wstring::npos;
    }

    void RefreshOcrSubPanel();

    void InvalidateOcrEditorPanel() {
        InvalidateParamScrollArea();
    }

    int OcrDepRowY() const {
        return ParamPanelContentTopY();
    }

    int OcrContentStartY() const {
        const int depH = ScaleY(kFindBtnH);
        const int depGap = ScaleY(kOcrDepToRegionGap);
        return OcrDepRowY() + depH + depGap;
    }

    void RefreshOcrDepStatus() {
        if (!ocrDepStatusLabel_ || !ocrDepInstallBtn_) return;
        const OcrEnvStatus env = CheckOcrEnvironment(false);
        const bool ready = env.state == OcrEnvState::Ready;
        SetText(ocrDepStatusLabel_, ready ? L"文字识别已安装" : L"文字识别未安装");
        SetWindowTextW(ocrDepInstallBtn_, ready ? L"修复/更新" : L"一键安装");
        EnableWindow(ocrDepInstallBtn_, TRUE);
        EnsureParamViewportParent(ocrDepStatusLabel_);
        EnsureParamViewportParent(ocrDepInstallBtn_);
        const int depY = OcrDepRowY();
        const int left = ScaleX(kFindContentLeft);
        const int panelRight = ParamScrollContentRight();
        const int maxW = std::max(1, panelRight - left - ScaleX(4));
        const int gap = ScaleX(8);
        const int depH = ScaleY(kFindBtnH);
        HDC hdc = GetDC(hwnd_);
        const wchar_t* btnText = ready ? L"修复/更新" : L"一键安装";
        const int btnWDesign = OcrTextButtonWidthDesign(hdc, editorFont_ ? editorFont_ : font_, btnText, kFindBtnW, 16);
        ReleaseDC(hwnd_, hdc);
        const int btnW = std::min(OcrScaleX(btnWDesign), maxW - ScaleX(120) - gap);
        const int labelW = std::max(ScaleX(120), maxW - btnW - gap);
        const int btnX = left + labelW + gap;
        SetParamPosAware(ocrDepStatusLabel_, left, depY, labelW, depH, SWP_NOZORDER | SWP_NOACTIVATE);
        SetParamPosAware(ocrDepInstallBtn_, btnX, depY, btnW, depH, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(ocrDepStatusLabel_, SW_SHOW);
        ShowWindow(ocrDepInstallBtn_, SW_SHOW);
    }

#if (QST_GDI_LEGACY == 1)
    void ShowOcrInstallDialog() {
        const OcrEnvStatus env = CheckOcrEnvironment(false);
        const bool repair = env.state == OcrEnvState::Ready;
        OcrInstallDialog dlg;
        dlg.Show(hwnd_, repair);
        RefreshOcrDepStatus();
    }
#endif

    void ApplyOcrFullScreen() {
        ocrFullScreen_ = true;
        int x = 0, y = 0, w = 0, h = 0;
        GetVirtualScreenRect(x, y, w, h);
        SetText(ocrX1_, std::to_wstring(x));
        SetText(ocrY1_, std::to_wstring(y));
        SetText(ocrX2_, std::to_wstring(x + w));
        SetText(ocrY2_, std::to_wstring(y + h));
        RefreshCoordFieldEdits({ocrX1_, ocrY1_, ocrX2_, ocrY2_});
    }

    void ApplyAiFullScreen() {
        aiFullScreen_ = true;
        int x = 0, y = 0, w = 0, h = 0;
        GetVirtualScreenRect(x, y, w, h);
        if (popupAction_.sel == 30) {
            SetText(aiSearchX1Edit_, std::to_wstring(x));
            SetText(aiSearchY1Edit_, std::to_wstring(y));
            SetText(aiSearchX2Edit_, std::to_wstring(x + w));
            SetText(aiSearchY2Edit_, std::to_wstring(y + h));
            RefreshCoordFieldEdits({aiSearchX1Edit_, aiSearchY1Edit_, aiSearchX2Edit_, aiSearchY2Edit_});
        } else if (popupAction_.sel == 31) {
            SetText(aiSearchX1Edit2_, std::to_wstring(x));
            SetText(aiSearchY1Edit2_, std::to_wstring(y));
            SetText(aiSearchX2Edit2_, std::to_wstring(x + w));
            SetText(aiSearchY2Edit2_, std::to_wstring(y + h));
            RefreshCoordFieldEdits({aiSearchX1Edit2_, aiSearchY1Edit2_, aiSearchX2Edit2_, aiSearchY2Edit2_});
        }
    }

    void BeginOcrRegionSelect() {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(L"选取区域");
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this](RECT sel) {
            if (sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0) {
                RestoreEditorAfterScreenOverlay();
                return;
            }
            const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            ocrFullScreen_ = false;
            SetText(ocrX1_, std::to_wstring(sel.left + vsX));
            SetText(ocrY1_, std::to_wstring(sel.top + vsY));
            SetText(ocrX2_, std::to_wstring(sel.right + vsX));
            SetText(ocrY2_, std::to_wstring(sel.bottom + vsY));
            RestoreEditorAfterScreenOverlay();
        });
    }

    void FlushPendingUiMessages() const {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    void HideEditorForScreenCapture() {
        if (!hwnd_) return;
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        UpdateWindow(hwnd_);
        FlushPendingUiMessages();
        Sleep(150);
    }

    void RestoreEditorAfterScreenCapture() {
        RestoreEditorAfterScreenOverlay();
    }

    void TestOcr() {
        if (ocrTestRunning_.exchange(true)) return;
        const OcrEnvStatus env = CheckOcrEnvironment(false);
        if (env.state != OcrEnvState::Ready) {
            ocrTestRunning_ = false;
            ShowOcrInstallDialog();
            return;
        }
        if (ocrTestBtn_) EnableWindow(ocrTestBtn_, FALSE);

        const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
        const bool digitsOnly = ocrDigitsOnlyCheck_ && Checked(ocrDigitsOnlyCheck_);
        if (regionByImage && ocrFindImagePath_.empty()) {
            ocrTestRunning_ = false;
            if (ocrTestBtn_) EnableWindow(ocrTestBtn_, TRUE);
            ShowPromptInfo(L"请先设置要查找的图片。");
            return;
        }

        HideEditorForScreenCapture();

        int sx1 = ToInt(ocrX1_), sy1 = ToInt(ocrY1_), sx2 = ToInt(ocrX2_), sy2 = ToInt(ocrY2_);
        if (regionByImage) {
            if (!ResolveOcrAbsRegionFromFindImage(
                    ToInt(ocrImageRegionX1_), ToInt(ocrImageRegionY1_),
                    ToInt(ocrImageRegionX2_), ToInt(ocrImageRegionY2_),
                    sx1, sy1, sx2, sy2)) {
                RestoreEditorAfterScreenCapture();
                ocrTestRunning_ = false;
                if (ocrTestBtn_) EnableWindow(ocrTestBtn_, TRUE);
                ShowPromptInfo(L"未找到参考图片，无法测试识别。");
                return;
            }
        } else if (ocrFullScreen_) {
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            sx1 = vsX; sy1 = vsY; sx2 = vsX + vsW; sy2 = vsY + vsH;
        }

        if (!ocrOverlay_) ocrOverlay_ = std::make_unique<OcrOverlay>();

        std::wstring searchTarget;
        if (popupOcrResultMode_.sel == 1) {
            MacroVariableContext ctx;
            ctx.matchVars = &matchVars_;
            ctx.matchListVars = &matchListVars_;
            ctx.ocrVars = &ocrVars_;
            ctx.loopVars = &loopVars_;
            ctx.timerStarts = &timerStarts_;
            ctx.curLoops = curLoops_;
            searchTarget = ResolveMacroVariables(GetText(ocrSearchEdit_), ctx);
        }
        EnsureOcrSession();
        ocrOverlay_->Show(sx1, sy1, sx2, sy2, searchTarget, OcrOverlayMode::Test, digitsOnly);
        ReleaseOcrSession();

        RestoreEditorAfterScreenCapture();

        ocrTestRunning_ = false;
        if (ocrTestBtn_) EnableWindow(ocrTestBtn_, TRUE);
        if (popupAction_.sel == 18) {
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
        }
    }

    void CancelQuickInputTip() {
        quickInputTipPending_ = QuickInputTipKind::None;
        quickInputTipPendingVarIndex_ = -1;
        quickInputTipShown_ = QuickInputTipKind::None;
        if (hwnd_) KillTimer(hwnd_, kQuickInputTipTimerId);
        if (editorTipPopup_) ShowWindow(editorTipPopup_, SW_HIDE);
    }

    bool IsPointInQuickInputEdit(int x, int y) const {
        if (!quickInputEdit_ || popupAction_.sel != 12 || !IsWindowVisible(quickInputEdit_)) return false;
        const RECT rc = WindowClientRect(quickInputEdit_);
        return PtInRect(&rc, POINT{x, y});
    }

    void BeginQuickInputTextTipHover(int x, int y) {
        if (popupAction_.sel != 12) { CancelQuickInputTip(); return; }
        if (quickInputTipPending_ == QuickInputTipKind::TextExample
            && quickInputTipAnchor_.x == x && quickInputTipAnchor_.y == y) return;
        quickInputTipPending_ = QuickInputTipKind::TextExample;
        quickInputTipPendingVarIndex_ = -1;
        quickInputTipAnchor_ = POINT{x, y};
        quickInputTipHoverStart_ = GetTickCount();
        quickInputTipShown_ = QuickInputTipKind::None;
        if (editorTipPopup_) ShowWindow(editorTipPopup_, SW_HIDE);
        if (hwnd_) SetTimer(hwnd_, kQuickInputTipTimerId, 50, nullptr);
    }

    void BeginQuickInputVarTipHover(int varIndex) {
        if (varIndex < 0 || varIndex >= static_cast<int>(quickInputVarItems_.size())) {
            CancelQuickInputTip();
            return;
        }
        if (quickInputTipPending_ == QuickInputTipKind::VariableHelp && quickInputTipPendingVarIndex_ == varIndex) return;
        quickInputTipPending_ = QuickInputTipKind::VariableHelp;
        quickInputTipPendingVarIndex_ = varIndex;
        GetCursorPos(&quickInputTipAnchor_);
        quickInputTipHoverStart_ = GetTickCount();
        quickInputTipShown_ = QuickInputTipKind::None;
        if (editorTipPopup_) ShowWindow(editorTipPopup_, SW_HIDE);
        if (hwnd_) SetTimer(hwnd_, kQuickInputTipTimerId, 50, nullptr);
    }

    void OnQuickInputTipTimer() {
        if (quickInputTipPending_ == QuickInputTipKind::None) {
            KillTimer(hwnd_, kQuickInputTipTimerId);
            return;
        }
        if (GetTickCount() - quickInputTipHoverStart_ < static_cast<DWORD>(kQuickInputTipDelayMs)) return;
        KillTimer(hwnd_, kQuickInputTipTimerId);
        if (quickInputTipPending_ == QuickInputTipKind::TextExample) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            if (!IsPointInQuickInputEdit(pt.x, pt.y)) {
                CancelQuickInputTip();
                return;
            }
            quickInputTipShown_ = QuickInputTipKind::TextExample;
            quickInputTipAnchor_ = pt;
        } else if (quickInputTipPending_ == QuickInputTipKind::VariableHelp) {
            if (editorPopupOpen_ != 7 || editorPopupHover_ != quickInputTipPendingVarIndex_) {
                CancelQuickInputTip();
                return;
            }
            quickInputTipShown_ = QuickInputTipKind::VariableHelp;
            GetCursorPos(&quickInputTipAnchor_);
        }
        SyncQuickInputTipPopup();
    }

    void UpdateQuickInputTextTip(int x, int y) {
        if (IsPointInQuickInputEdit(x, y)) BeginQuickInputTextTipHover(x, y);
        else if (quickInputTipPending_ == QuickInputTipKind::TextExample || quickInputTipShown_ == QuickInputTipKind::TextExample) CancelQuickInputTip();
    }

    bool IsBlockNameDuplicate(const std::wstring& name, int excludeIndex = -1) const {
        for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
            if (i == excludeIndex) continue;
            if (actions_[static_cast<size_t>(i)].type == ActionType::DefineBlock && actions_[static_cast<size_t>(i)].blockName == name) return true;
        }
        return false;
    }

    bool ValidateDefineBlockName(const std::wstring& name, int excludeIndex = -1) {
        if (!IsValidBlockName(name)) {
            ShowPromptInfo(L"块名称只能以字母开始，后面只能包含字母和数字。");
            return false;
        }
        if (IsBlockNameDuplicate(name, excludeIndex)) {
            ShowPromptInfo(L"块名称不能重复。");
            return false;
        }
        return true;
    }

    bool TryInsertActionFromForm(size_t pos, int indentOverride = -1) {
        ScriptAction action = ActionFromForm();
        if (action.type == ActionType::DefineBlock) {
            if (!ValidateDefineBlockName(action.blockName)) return false;
            pos = 0;
            indentOverride = 0;
        }
        const int addedSel = popupAction_.sel;
        // 添加过程不选中新动作，避免 UpdateEditMode 瞬间露出「修改」按钮
        InsertAction(pos, action, indentOverride, /*selectInserted=*/false);
        selectedIndex_ = -1;
        actionFormDrafts_.erase(addedSel);
        loadingForm_ = true;
        LoadForm(DefaultActionForPopupSel(addedSel));
        SetText(remark_, L"");
        loadingForm_ = false;
        UpdateEditMode();
        if (action.type == ActionType::DefineBlock || action.type == ActionType::RunBlock) RefreshRunBlockCombo();
        return true;
    }

    int ComboSelForType(ActionType type) const {
        switch (type) {
        case ActionType::MoveMouse: return 0;
        case ActionType::Wait: return 1;
        case ActionType::MouseClick: return 2;
        case ActionType::MousePlayback: return 3;
        case ActionType::RunMacro: return 4;
        case ActionType::MouseDown: return 5;
        case ActionType::MouseUp: return 6;
        case ActionType::KeyClick: return 8;
        case ActionType::KeyDown: return 9;
        case ActionType::KeyUp: return 10;
        case ActionType::HotkeyShortcut: return 11;
        case ActionType::QuickInput: return 12;
        case ActionType::Loop: return 13;
        case ActionType::EndLoop: return 14;
        case ActionType::DefineBlock: return 15;
        case ActionType::RunBlock: return 16;
        case ActionType::ScrollWheel: return 7;
        case ActionType::FindImage: return 17;
        case ActionType::TextRecognition: return 18;
        case ActionType::If: return 19;
        case ActionType::Else: return 20;
        case ActionType::LockScreenshot: return 21;
        case ActionType::UnlockScreenshot: return 22;
        case ActionType::StopMacro: return 23;
        case ActionType::RunProgram: return 24;
        case ActionType::CloseProgram: return 25;
        case ActionType::OpenWebpage: return 26;
        case ActionType::OpenFile: return 27;
        case ActionType::ActivateWindow: return 25; // 复用关闭程序图标位
        case ActionType::TimerRecordTime: return 28;
        case ActionType::AiTextAnalysis: return 29;
        case ActionType::AiImageAnalysis: return 30;
        case ActionType::AiActionExecute: return 31;
        case ActionType::GetCursorPos: return 32;
        case ActionType::Goto: return 33;
        case ActionType::MoveMouseRelative: return 34;
        case ActionType::MultiMatch: return 17;
        default: return 14;
        }
    }

    bool IsImplementedActionPopup(int idx) const {
        return idx == 0 || idx == 1 || idx == 2 || idx == 3 || idx == 4 || idx == 5 || idx == 6 || idx == 7 || idx == 8 || idx == 9 || idx == 10
            || idx == 11 || idx == 12
            || idx == 13 || idx == 14 || idx == 15 || idx == 16 || idx == 17 || idx == 18 || idx == 19
            || idx == 20 || idx == 21 || idx == 22 || idx == 23 || idx == 24
            || idx == 25 || idx == 26 || idx == 27 || idx == 28
            || idx == 29 || idx == 30 || idx == 31 || idx == 32 || idx == 33 || idx == 34;
    }

    void UpdateEditMode() {
        const bool selected = ShouldShowModifyButton();
        if (selected) LoadForm(actions_[static_cast<size_t>(selectedIndex_)]);
        if (batchEditMode_) SyncBatchSelectedSize();
        scrollOffset_ = std::clamp(scrollOffset_, 0, MaxEditorScroll());
        RefreshActionListLayer();
        ApplyEditorFooterLayout();
    }

    void ClearEditorActions() {
        actions_.clear();
        collapsedContainers_.clear();
        editorActionBlocks_.clear();
        editorActionParsed_.clear();
        editorParsePending_ = false;
        editorParseCursor_ = 0;
        MarkVisibleActionsDirty();
        selectedIndex_ = -1;
        hoverIndex_ = -1;
        editingRemarkIndex_ = -1;
        if (batchEditMode_) batchSelected_.clear();
        if (!loadingForm_) RestoreActionFormDraftForCurrentType();
        UpdateBatchToolbar();
        UpdateEditMode();
        OnActionsChanged();
    }

    int BatchSelectedCount() const {
        int count = 0;
        for (bool checked : batchSelected_) if (checked) ++count;
        return count;
    }

    void SyncBatchSelectedSize() {
        if (!batchEditMode_) return;
        if (batchSelected_.size() < actions_.size()) batchSelected_.resize(actions_.size(), false);
        else if (batchSelected_.size() > actions_.size()) batchSelected_.resize(actions_.size());
    }

    RECT ToolbarAreaRect() const {
        RECT rc{};
        bool has = false;
        for (HWND h : {loadBtn_, clearBtn_, batchExitBtn_, batchSelectAllBtn_, batchDeselectBtn_,
                       batchDeleteBtn_, batchCopyBtn_, labelBatchCount_, labelList_}) {
            if (!h) continue;
            const RECT hr = WindowClientRect(h);
            if (hr.right <= hr.left || hr.bottom <= hr.top) continue;
            if (!has) {
                rc = hr;
                has = true;
            } else {
                rc.left = std::min(rc.left, hr.left);
                rc.top = std::min(rc.top, hr.top);
                rc.right = std::max(rc.right, hr.right);
                rc.bottom = std::max(rc.bottom, hr.bottom);
            }
        }
        if (!has) {
            return RECT{
                0,
                ScaleY(90),
                ScaleX(780),
                ScaleY(132)
            };
        }
        InflateRect(&rc, 4, 4);
        return rc;
    }

    void InvalidateToolbarArea() {
        if (!hwnd_) return;
        const RECT toolbarRc = ToolbarAreaRect();
        InvalidateRect(hwnd_, &toolbarRc, TRUE);
    }

    void RedrawEditorSurface() {
        if (!hwnd_ || page_ != Page::Editor) return;
        InvalidateToolbarArea();
        const RECT listRc = ActionListRect();
        InvalidateRect(hwnd_, &listRc, TRUE);
        RefreshActionListLayer();
        if (UsesDynamicParamPanel()) {
            RefreshDynamicParamLayout();
        } else {
            ApplyEditorFooterLayout();
            SyncParamScrollLayout();
        }
        RepaintParamPanelChrome();
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        UpdateWindow(hwnd_);
    }

    void UpdateBatchToolbar() {
        ShowWindow(loadBtn_, batchEditMode_ ? SW_HIDE : SW_SHOW);
        ShowWindow(clearBtn_, batchEditMode_ ? SW_HIDE : SW_SHOW);
        ShowWindow(batchExitBtn_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        ShowWindow(batchSelectAllBtn_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        ShowWindow(batchDeselectBtn_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        ShowWindow(batchDeleteBtn_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        ShowWindow(batchCopyBtn_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        ShowWindow(labelBatchCount_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        if (labelList_) SetText(labelList_, L"动作列表");
        if (labelBatchCount_) SetText(labelBatchCount_, L"已选中:" + std::to_wstring(BatchSelectedCount()) + L"个");
        const bool hasSelection = BatchSelectedCount() > 0;
        EnableWindow(batchDeleteBtn_, hasSelection ? TRUE : FALSE);
        EnableWindow(batchCopyBtn_, hasSelection ? TRUE : FALSE);
        InvalidateToolbarArea();
    }

    void EnterBatchEditMode() {
        if (batchEditMode_) return;
        CommitInlineRemark();
        batchEditMode_ = true;
        const bool hadSelection = selectedIndex_ >= 0;
        selectedIndex_ = -1;
        hoverIndex_ = -1;
        if (hadSelection && !loadingForm_) RestoreActionFormDraftForCurrentType();
        batchSelected_.assign(actions_.size(), false);
        UpdateBatchToolbar();
        UpdateEditMode();
        RedrawEditorSurface();
    }

    void ExitBatchEditMode() {
        if (!batchEditMode_) return;
        batchEditMode_ = false;
        batchSelected_.clear();
        UpdateBatchToolbar();
        UpdateEditMode();
        RedrawEditorSurface();
    }

    void ToggleBatchSelection(int index) {
        if (!batchEditMode_ || index < 0 || index >= static_cast<int>(actions_.size())) return;
        SyncBatchSelectedSize();
        batchSelected_[static_cast<size_t>(index)] = !batchSelected_[static_cast<size_t>(index)];
        UpdateBatchToolbar();
        RefreshActionListLayer();
    }

    void BatchSelectAll() {
        if (!batchEditMode_) return;
        batchSelected_.assign(actions_.size(), true);
        UpdateBatchToolbar();
        RefreshActionListLayer();
    }

    void BatchDeselectAll() {
        if (!batchEditMode_) return;
        batchSelected_.assign(actions_.size(), false);
        UpdateBatchToolbar();
        RefreshActionListLayer();
    }

    void BatchDeleteSelected() {
        if (!batchEditMode_ || BatchSelectedCount() == 0) return;
        CommitInlineRemark();
        // 从后往前按子树删除，且只刷新一次 UI（勿对每项调用 DeleteActionAt）
        for (int i = static_cast<int>(actions_.size()) - 1; i >= 0; --i) {
            if (i >= static_cast<int>(batchSelected_.size()) || !batchSelected_[static_cast<size_t>(i)]) continue;
            const int end = SubtreeEnd(i);
            EraseEditorActionsRange(i, end);
        }
        batchSelected_.assign(actions_.size(), false);
        selectedIndex_ = -1;
        hoverIndex_ = -1;
        editingRemarkIndex_ = -1;
        RenumberActions();
        RefreshRunBlockCombo();
        if (!loadingForm_) RestoreActionFormDraftForCurrentType();
        UpdateBatchToolbar();
        UpdateEditMode();
        OnActionsChanged();
    }

    void BatchCopySelected() {
        if (!batchEditMode_ || BatchSelectedCount() == 0) return;
        std::vector<ScriptAction> copies;
        for (size_t i = 0; i < actions_.size(); ++i) {
            if (i < batchSelected_.size() && batchSelected_[i]) {
                ScriptAction copy = actions_[i];
                copy.originalNo = NextNo();
                copies.push_back(copy);
            }
        }
        for (const auto& copy : copies) actions_.push_back(copy);
        if (!copies.empty()) {
            // 追加的副本视为已解析，避免与分步解析数组长度错位
            if (editorActionParsed_.size() == actions_.size() - copies.size()) {
                editorActionParsed_.resize(actions_.size(), 1);
            } else {
                editorActionParsed_.assign(actions_.size(), 1);
                editorActionBlocks_.clear();
                editorParsePending_ = false;
                editorParseCursor_ = static_cast<int>(actions_.size());
            }
            MarkVisibleActionsDirty();
        }
        batchSelected_.assign(actions_.size(), false);
        RenumberActions();
        UpdateBatchToolbar();
        UpdateEditMode();
        OnActionsChanged();
    }

    bool Checked(HWND h) const {
        if (IsMarkedParamCheckbox(h)) return IsParamCheckboxChecked(h);
        return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    void SetChecked(HWND h, bool checked, bool immediateRedraw = true) {
        if (IsMarkedParamCheckbox(h)) {
            SetParamCheckboxChecked(h, checked, immediateRedraw);
            return;
        }
        SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    void RedrawParamCheckbox(HWND hwnd) const {
        if (!hwnd) return;
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
    }

    void OnParamCheckboxClicked(HWND ctrl) {
        if (!ctrl) return;
        const int id = GetDlgCtrlID(ctrl);
        if (id == kOcrRegionByImage || ctrl == ocrRegionByImageCheck_) {
            if (popupAction_.sel == 18) RequestOcrSubPanelRefresh();
            else RebuildParamPanelLayout();
            return;
        }
        if (id == kAiRegionByImage || id == kAiRegionByImage2 || id == kAiWithImage
            || ctrl == aiRegionByImageCheck_ || ctrl == aiRegionByImageCheck2_
            || ctrl == aiWithImageCheck_ || ctrl == aiLogicConvertCheck_) {
            RebuildParamPanelLayout();
        } else if (id == kMoveFromVar || id == kLoopFromVar
            || ctrl == moveFromVar_ || ctrl == loopFromVar_) {
            UpdateMoveVarControls();
            UpdateLoopVarControls();
        }
    }

    void ToggleParamCheckbox(HWND ctrl) {
        if (!ctrl) return;
        const int id = GetDlgCtrlID(ctrl);
        const bool lockVp = (id == EditorParamLayout::EID_OcrDigitsOnly && paramViewport_);
        if (lockVp) LockParamViewportRedraw();
        SetChecked(ctrl, !Checked(ctrl));
        if (!IsMarkedParamCheckbox(ctrl)) RedrawParamCheckbox(ctrl);
        if (lockVp) UnlockParamViewportRedraw();
        // 勾选框重绘之后再抬灰按钮，避免「纯数字」白底盖住「全图/选取区域」上边框。
        if (id == EditorParamLayout::EID_OcrDigitsOnly) RefreshOcrNeighborGrayButtons();
        OnParamCheckboxClicked(ctrl);
    }

    void ReadModifierHolds(ScriptAction& action, HWND lWin, HWND rWin, HWND lCtrl, HWND rCtrl, HWND lAlt, HWND rAlt, HWND lShift, HWND rShift) {
        action.holdLeftWin = Checked(lWin); action.holdRightWin = Checked(rWin);
        action.holdLeftCtrl = Checked(lCtrl); action.holdRightCtrl = Checked(rCtrl);
        action.holdLeftAlt = Checked(lAlt); action.holdRightAlt = Checked(rAlt);
        action.holdLeftShift = Checked(lShift); action.holdRightShift = Checked(rShift);
    }

    void WriteModifierHolds(const ScriptAction& action, HWND lWin, HWND rWin, HWND lCtrl, HWND rCtrl, HWND lAlt, HWND rAlt, HWND lShift, HWND rShift) {
        SetChecked(lWin, action.holdLeftWin); SetChecked(rWin, action.holdRightWin);
        SetChecked(lCtrl, action.holdLeftCtrl); SetChecked(rCtrl, action.holdRightCtrl);
        SetChecked(lAlt, action.holdLeftAlt); SetChecked(rAlt, action.holdRightAlt);
        SetChecked(lShift, action.holdLeftShift); SetChecked(rShift, action.holdRightShift);
    }

    // 各动作类型各自一套「同时按住」控件；LoadForm 只写当前类型时，其它类型会残留勾选
    void WriteModifierHoldsQuiet(const ScriptAction& action, HWND lWin, HWND rWin, HWND lCtrl, HWND rCtrl,
        HWND lAlt, HWND rAlt, HWND lShift, HWND rShift) {
        SetChecked(lWin, action.holdLeftWin, false); SetChecked(rWin, action.holdRightWin, false);
        SetChecked(lCtrl, action.holdLeftCtrl, false); SetChecked(rCtrl, action.holdRightCtrl, false);
        SetChecked(lAlt, action.holdLeftAlt, false); SetChecked(rAlt, action.holdRightAlt, false);
        SetChecked(lShift, action.holdLeftShift, false); SetChecked(rShift, action.holdRightShift, false);
    }

    void ClearAllModifierHoldCheckboxes() {
        ScriptAction empty{};
        WriteModifierHoldsQuiet(empty, clickLWin_, clickRWin_, clickLCtrl_, clickRCtrl_,
            clickLAlt_, clickRAlt_, clickLShift_, clickRShift_);
        WriteModifierHoldsQuiet(empty, mousePressLWin_, mousePressRWin_, mousePressLCtrl_, mousePressRCtrl_,
            mousePressLAlt_, mousePressRAlt_, mousePressLShift_, mousePressRShift_);
        WriteModifierHoldsQuiet(empty, keyLWin_, keyRWin_, keyLCtrl_, keyRCtrl_,
            keyLAlt_, keyRAlt_, keyLShift_, keyRShift_);
        WriteModifierHoldsQuiet(empty, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_,
            keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_);
    }

    ActionType ActionTypeForPopupSel(int sel) const {
        switch (sel) {
        case 0: return ActionType::MoveMouse;
        case 1: return ActionType::Wait;
        case 2: return ActionType::MouseClick;
        case 3: return ActionType::MousePlayback;
        case 4: return ActionType::RunMacro;
        case 5: return ActionType::MouseDown;
        case 6: return ActionType::MouseUp;
        case 7: return ActionType::ScrollWheel;
        case 8: return ActionType::KeyClick;
        case 9: return ActionType::KeyDown;
        case 10: return ActionType::KeyUp;
        case 11: return ActionType::HotkeyShortcut;
        case 12: return ActionType::QuickInput;
        case 13: return ActionType::Loop;
        case 14: return ActionType::EndLoop;
        case 15: return ActionType::DefineBlock;
        case 16: return ActionType::RunBlock;
        case 17: return ActionType::FindImage;
        case 18: return ActionType::TextRecognition;
        case 19: return ActionType::If;
        case 20: return ActionType::Else;
        case 21: return ActionType::LockScreenshot;
        case 22: return ActionType::UnlockScreenshot;
        case 23: return ActionType::StopMacro;
        case 24: return ActionType::RunProgram;
        case 25: return ActionType::CloseProgram;
        case 26: return ActionType::OpenWebpage;
        case 27: return ActionType::OpenFile;
        case 28: return ActionType::TimerRecordTime;
        case 29: return ActionType::AiTextAnalysis;
        case 30: return ActionType::AiImageAnalysis;
        case 31: return ActionType::AiActionExecute;
        case 32: return ActionType::GetCursorPos;
        case 33: return ActionType::Goto;
        case 34: return ActionType::MoveMouseRelative;
        default: return ActionType::EndLoop;
        }
    }

    ScriptAction DefaultActionForPopupSel(int sel) const {
        ScriptAction action;
        action.type = ActionTypeForPopupSel(sel);
        switch (sel) {
        case 0:
            action.moveVarExprX = L"0";
            action.moveVarExprY = L"0";
            break;
        case 34:
            action.x = 0;
            action.y = 0;
            break;
        case 1:
            action.duration = 0.5;
            break;
        case 2:
            action.duration = 0.01;
            break;
        case 3:
            action.clickCount = 1;
            action.duration = 0.01;
            break;
        case 7:
            action.scrollVertical = true;
            action.scrollHorizontal = false;
            action.scrollSteps = 1;
            action.scrollDirection = 0;
            action.clickCount = 1;
            action.duration = 0.01;
            break;
        case 8:
            action.keyText.clear();
            action.keyVk = 0;
            action.clickCount = 1;
            action.duration = 0.01;
            break;
        case 9:
        case 10:
            action.keyText.clear();
            action.keyVk = 0;
            break;
        case 11:
            action.clickCount = 1;
            action.duration = 0.01;
            break;
        case 12:
            action.charInterval = 0.01;
            action.clickCount = 1;
            action.duration = 0.01;
            break;
        case 14:
            action.customText = L"跳出循环";
            break;
        case 15:
            action.blockName = L"block1";
            break;
        case 17:
            action.searchFullScreen = true;
            action.matchThreshold = 65.0;
            action.imageScaleMin = 1.0;
            action.imageScaleMax = 1.0;
            action.findImageFollowUp = 0;
            action.findTimeExpr = L"0";
            action.matchVarName = L"matchRet";
            break;
        case 18:
            action.searchFullScreen = true;
            action.ocrResultMode = 0;
            action.ocrFollowUp = 0;
            action.matchVarName = L"a";
            break;
        case 29:
            action.aiOutputVarName = L"aiResult";
            action.aiOutputType = 0;
            action.aiContextMode = 0;
            action.aiTimeoutSec = 30;
            break;
        case 30:
            action.aiOutputVarName = L"aiImgResult";
            action.aiOutputType = 0;
            action.aiContextMode = 0;
            action.aiTimeoutSec = 30;
            action.aiImageScale = 1.0;
            action.searchFullScreen = true;
            break;
        case 31:
            action.aiContextMode = 0;
            action.aiTimeoutSec = 30;
            action.aiImageScale = 0.5;
            action.aiMaxSteps = 10;
            action.searchFullScreen = true;
            break;
        case 32:
            action.matchVarName = L"a";
            break;
        default:
            break;
        }
        return action;
    }

    // ── 添加表单草稿（仅内存，从不写入脚本文件）────────────────────
    // 生命周期：
    //   · 存在：未点「添加」、未退出编辑时；切换动作类型，或选中/取消选中已添加动作
    //   · 清除：「添加」后清掉该类型草稿；退出编辑 / 打开编辑 ResetActionFormSession 整表清空
    // 脚本 JSON 里的 holdLeftWin 等是已添加动作的字段，不是草稿。
    void SaveCurrentActionFormDraft() {
        if (loadingForm_ || selectedIndex_ >= 0) return;
        actionFormDrafts_[popupAction_.sel] = ActionFromForm();
    }

    void RestoreActionFormDraftForCurrentType() {
        const int sel = popupAction_.sel;
        const auto it = actionFormDrafts_.find(sel);
        if (it != actionFormDrafts_.end()) {
            LoadForm(it->second);
            return;
        }
        LoadForm(DefaultActionForPopupSel(sel));
        // 无草稿：再清修饰键，避免取消选中列表项后勾选残留又被写进草稿
        ClearAllModifierHoldCheckboxes();
    }

    void ClearAllParamCheckboxStates() {
        // 批量静默清勾选，避免每个框 RDW_UPDATENOW 拖慢打开/切类型
        auto clearMarked = [this](HWND root) {
            if (!root) return;
            for (HWND child = GetWindow(root, GW_CHILD); child;
                 child = GetWindow(child, GW_HWNDNEXT)) {
                if (IsMarkedParamCheckbox(child)) SetChecked(child, false, false);
            }
        };
        clearMarked(paramViewport_);
        for (HWND h : editorControls_) {
            // 窗口模式「聚焦」属于脚本级开关，勿随动作表单清空。
            if (h == wmFakeFocusCheck_) continue;
            if (h && IsMarkedParamCheckbox(h)) SetChecked(h, false, false);
        }
    }

    void ResetEditorScriptChromeDefaults() {
        scriptWindowMode_ = {};
        popupMode_.sel = 0;
        SetPopupSel(popupMode_, mode_, 0);
        popupWmSelectMethod_.sel = 0;
        if (!popupWmSelectMethod_.items.empty()) {
            SetPopupSel(popupWmSelectMethod_, wmSelectMethod_, 0);
        }
        if (breakoutTimeEdit_) SetText(breakoutTimeEdit_, L"0");
        if (wmTargetPathEdit_) SetText(wmTargetPathEdit_, L"");
        SyncWindowModeUiFromScript();
    }

    void ResetEditorTransientFormState() {
        formKeyText_.clear();
        formKeyVk_ = 0;
        formKeyPressText_.clear();
        formKeyPressVk_ = 0;
        if (keyEdit_) SetText(keyEdit_, L"");
        if (keyPressEdit_) SetText(keyPressEdit_, L"");
        ClearAllModifierHoldCheckboxes();
        findImagePath_.clear();
        findImageFullScreen_ = true;
        ocrFindImagePath_.clear();
        ocrFullScreen_ = true;
        aiFindImagePath_.clear();
        if (findImagePreviewBitmap_) {
            DeleteBitmapHandle(findImagePreviewBitmap_);
            findImagePreviewBitmap_ = nullptr;
        }
        if (ocrFindImagePreviewBitmap_) {
            DeleteBitmapHandle(ocrFindImagePreviewBitmap_);
            ocrFindImagePreviewBitmap_ = nullptr;
        }
        if (aiFindImagePreviewBitmap_) {
            DeleteBitmapHandle(aiFindImagePreviewBitmap_);
            aiFindImagePreviewBitmap_ = nullptr;
        }
        ClearAllParamCheckboxStates();
    }

    // resetScriptChrome：退出编辑/新建宏时清模式·脱离时间·窗口模式；
    // 打开已有宏时为 false（脚本头已由 LoadScriptFile 写入）。
    void ResetActionFormSession(bool resetScriptChrome = true) {
        actionFormDrafts_.clear();
        if (resetScriptChrome) ResetEditorScriptChromeDefaults();
        ResetEditorTransientFormState();
        // 子下拉也复位，避免「右键/侧键」等留在上次状态
        if (!popupMouseBtn_.items.empty()) SetPopupSel(popupMouseBtn_, mousePressButton_, 0);
        if (!popupClickBtn_.items.empty()) SetPopupSel(popupClickBtn_, clickButton_, 0);
        if (!popupScrollDir_.items.empty()) SetPopupSel(popupScrollDir_, scrollDirectionCombo_, 0);
        if (!popupHotkeyShortcut_.items.empty()) SetPopupSel(popupHotkeyShortcut_, hotkeyShortcutCombo_, 0);
        if (!popupLoopType_.items.empty()) SetPopupSel(popupLoopType_, loopTypeCombo_, 0);
        loadingForm_ = true;
        SetPopupSel(popupAction_, actionCombo_, 0);
        SetText(remark_, L"");
        if (page_ == Page::Editor) {
            LoadForm(DefaultActionForPopupSel(0));
        } else {
            // 已回主页：勿 RefreshParamPanel（会把参数区 ShowWindow 叠到主页上）
            ClearAllModifierHoldCheckboxes();
            ClearAllParamCheckboxStates();
        }
        loadingForm_ = false;
    }

    void DiscardSpuriousEditorInput() {
        if (!hwnd_) return;
        MSG msg{};
        // Peek(hwnd) 不含子控件；下拉选类型后要把落在勾选框上的 UP 清掉
        while (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_NOREMOVE)) {
            const bool ours = msg.hwnd == hwnd_
                || (msg.hwnd && IsChild(hwnd_, msg.hwnd))
                || (editorDropPopup_ && msg.hwnd == editorDropPopup_);
            if (!ours) break;
            PeekMessageW(&msg, msg.hwnd, msg.message, msg.message, PM_REMOVE);
        }
    }

    void UncloakEditorAfterReady() {
        if (!hwnd_) return;
        // 仍 cloak：整客户区先画一帧再揭开（避免 ExcludeClip 空洞露出主页）
        editorFullClientBlit_ = true;
        RedrawWindow(hwnd_, nullptr, nullptr,
            RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
        editorFullClientBlit_ = false;
        SetWindowCloaked(hwnd_, false);
        BOOL disableTransitions = FALSE;
        DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED,
            &disableTransitions, sizeof(disableTransitions));
        DiscardSpuriousEditorInput();
        outerShadow_.Sync();
    }

    ScriptAction ActionFromForm() {
        ScriptAction action{};
        action.remark = GetText(remark_);
        const int sel = popupAction_.sel;
        if (sel == 0) {
            action.type = ActionType::MoveMouse;
            action.moveFromVar = Checked(moveFromVar_);
            action.moveVarExprX = Trim(GetText(moveVarX_));
            action.moveVarExprY = Trim(GetText(moveVarY_));
            action.x = ToInt(moveX_);
            action.y = ToInt(moveY_);
            action.randomX = std::max(0, ToInt(moveRandomX_));
            action.randomY = std::max(0, ToInt(moveRandomY_));
        }
        else if (sel == 34) {
            action.type = ActionType::MoveMouseRelative;
            action.x = ToInt(moveRelX_);
            action.y = ToInt(moveRelY_);
            action.randomX = std::max(0, ToInt(moveRelRandomX_));
            action.randomY = std::max(0, ToInt(moveRelRandomY_));
            action.coordsAreNormalized = false;
        }
        else if (sel == 1) { action.type = ActionType::Wait; action.duration = std::max(0.0, ToDouble(waitDuration_, 0.5)); action.randomDuration = std::max(0.0, ToDouble(waitRandom_)); }
        else if (sel == 2) { action.type = ActionType::MouseClick; action.button = static_cast<MouseButtonType>(std::max(0, popupClickBtn_.sel)); action.clickCount = std::max(1, ToInt(clickCount_, 1)); action.duration = std::max(0.0, ToDouble(clickWait_, 0.01)); action.randomDuration = std::max(0.0, ToDouble(clickRandom_)); ReadModifierHolds(action, clickLWin_, clickRWin_, clickLCtrl_, clickRCtrl_, clickLAlt_, clickRAlt_, clickLShift_, clickRShift_); }
        else if (sel == 3) {
            action.type = ActionType::MousePlayback;
            action.blockName = popupMousePlayback_.sel >= 0 && popupMousePlayback_.sel < static_cast<int>(popupMousePlayback_.items.size())
                ? popupMousePlayback_.items[static_cast<size_t>(popupMousePlayback_.sel)] : Trim(GetText(mousePlaybackCombo_));
            action.targetPath = popupMousePlayback_.sel >= 0 && popupMousePlayback_.sel < static_cast<int>(mousePlaybackPaths_.size())
                ? mousePlaybackPaths_[static_cast<size_t>(popupMousePlayback_.sel)] : L"";
            action.clickCount = std::max(1, ToInt(mousePlaybackCount_, 1));
            action.duration = std::max(0.0, ToDouble(mousePlaybackWait_, 0.01));
            action.randomDuration = std::max(0.0, ToDouble(mousePlaybackRandom_));
        }
        else if (sel == 4) {
            action.type = ActionType::RunMacro;
            action.blockName = popupRunMacro_.sel >= 0 && popupRunMacro_.sel < static_cast<int>(popupRunMacro_.items.size())
                ? popupRunMacro_.items[static_cast<size_t>(popupRunMacro_.sel)] : Trim(GetText(runMacroCombo_));
            action.targetPath = popupRunMacro_.sel >= 0 && popupRunMacro_.sel < static_cast<int>(runMacroPaths_.size())
                ? runMacroPaths_[static_cast<size_t>(popupRunMacro_.sel)] : L"";
        }
        else if (sel == 5) { action.type = ActionType::MouseDown; action.button = static_cast<MouseButtonType>(std::max(0, popupMouseBtn_.sel)); ReadModifierHolds(action, mousePressLWin_, mousePressRWin_, mousePressLCtrl_, mousePressRCtrl_, mousePressLAlt_, mousePressRAlt_, mousePressLShift_, mousePressRShift_); }
        else if (sel == 6) { action.type = ActionType::MouseUp; action.button = static_cast<MouseButtonType>(std::max(0, popupMouseBtn_.sel)); ReadModifierHolds(action, mousePressLWin_, mousePressRWin_, mousePressLCtrl_, mousePressRCtrl_, mousePressLAlt_, mousePressRAlt_, mousePressLShift_, mousePressRShift_); }
        else if (sel == 7) {
            action.type = ActionType::ScrollWheel;
            action.scrollVertical = Checked(scrollVertical_);
            action.scrollHorizontal = Checked(scrollHorizontal_);
            if (!action.scrollVertical && !action.scrollHorizontal) action.scrollVertical = true;
            action.scrollSteps = std::max(1, ToInt(scrollSteps_, 1));
            action.scrollDirection = std::clamp(popupScrollDir_.sel, 0, 1);
            action.clickCount = std::max(1, ToInt(scrollCount_, 1));
            action.duration = std::max(0.0, ToDouble(scrollWait_, 0.01));
            action.randomDuration = std::max(0.0, ToDouble(scrollRandom_));
        }
        else if (sel == 8) { action.type = ActionType::KeyClick; action.keyText = formKeyText_; action.keyVk = formKeyVk_; action.clickCount = std::max(1, ToInt(keyCount_, 1)); action.duration = std::max(0.0, ToDouble(keyWait_, 0.01)); action.randomDuration = std::max(0.0, ToDouble(keyRandom_)); ReadModifierHolds(action, keyLWin_, keyRWin_, keyLCtrl_, keyRCtrl_, keyLAlt_, keyRAlt_, keyLShift_, keyRShift_); }
        else if (sel == 9) { action.type = ActionType::KeyDown; action.keyText = formKeyPressText_; action.keyVk = formKeyPressVk_; ReadModifierHolds(action, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_, keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_); }
        else if (sel == 10) { action.type = ActionType::KeyUp; action.keyText = formKeyPressText_; action.keyVk = formKeyPressVk_; ReadModifierHolds(action, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_, keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_); }
        else if (sel == 11) {
            action.type = ActionType::HotkeyShortcut;
            action.shortcutPreset = std::clamp(popupHotkeyShortcut_.sel, 0, ShortcutPresetCount() - 1);
            ApplyShortcutPreset(action, action.shortcutPreset);
            action.clickCount = std::max(1, ToInt(hotkeyShortcutCount_, 1));
            action.duration = std::max(0.0, ToDouble(hotkeyShortcutWait_, 0.01));
            action.randomDuration = std::max(0.0, ToDouble(hotkeyShortcutRandom_));
        }
        else if (sel == 12) {
            action.type = ActionType::QuickInput;
            action.inputText = GetText(quickInputEdit_);
            action.charInterval = std::max(0.0, ToDouble(quickInputCharInterval_, 0.01));
            action.clickCount = std::max(1, ToInt(quickInputCount_, 1));
            action.duration = std::max(0.0, ToDouble(quickInputWait_, 0.01));
            action.randomDuration = std::max(0.0, ToDouble(quickInputRandom_));
        }
        else if (sel == 13) { action.type = ActionType::Loop; action.loopCount = ToInt(loopCount_, -1); action.loopFromVar = Checked(loopFromVar_); action.loopVarExpr = GetText(loopVarExpr_); action.loopVarName = GetText(loopVarName_); }
        else if (sel == 14) { action.type = ActionType::EndLoop; action.customText = L"跳出循环"; }
        else if (sel == 15) { action.type = ActionType::DefineBlock; action.blockName = Trim(GetText(defineBlockName_)); }
        else if (sel == 16) { action.type = ActionType::RunBlock; action.blockName = Trim(GetText(runBlockCombo_)); }
        else if (sel == 17) {
            action.type = ActionType::FindImage;
            action.searchX1 = ToInt(findX1_);
            action.searchY1 = ToInt(findY1_);
            action.searchX2 = ToInt(findX2_);
            action.searchY2 = ToInt(findY2_);
            action.searchFullScreen = findImageFullScreen_;
            action.imagePath = findImagePath_;
            action.matchThreshold = std::clamp(ToDouble(findMatchThreshold_, 65.0), 1.0, 100.0);
            action.imageScaleMin = std::max(0.1, ToDouble(findScaleMin_, 1.0));
            action.imageScaleMax = std::max(action.imageScaleMin, ToDouble(findScaleMax_, action.imageScaleMin));
            action.imageScale = (action.imageScaleMin + action.imageScaleMax) * 0.5;
            action.findImageFollowUp = std::clamp(popupFindFollowUp_.sel, 0, 2);
            action.offsetX = ToInt(findOffsetX_);
            action.offsetY = ToInt(findOffsetY_);
            action.findTimeExpr = action.findImageFollowUp == 2 ? L"0" : GetText(findTimeEdit_);
            if (action.findTimeExpr.empty()) action.findTimeExpr = L"0";
            action.matchVarName = Trim(GetText(findMatchVar_));
            if (action.matchVarName.empty()) action.matchVarName = L"matchRet";
        }
        else if (sel == 18) {
            action.type = ActionType::TextRecognition;
            const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
            const bool digitsOnly = ocrDigitsOnlyCheck_ && Checked(ocrDigitsOnlyCheck_);
            action.ocrRegionByImage = regionByImage;
            action.ocrDigitsOnly = digitsOnly;
            action.searchX1 = ToInt(ocrX1_);
            action.searchY1 = ToInt(ocrY1_);
            action.searchX2 = ToInt(ocrX2_);
            action.searchY2 = ToInt(ocrY2_);
            action.searchFullScreen = ocrFullScreen_;
            action.imageRegionX1 = ToInt(ocrImageRegionX1_);
            action.imageRegionY1 = ToInt(ocrImageRegionY1_);
            action.imageRegionX2 = ToInt(ocrImageRegionX2_);
            action.imageRegionY2 = ToInt(ocrImageRegionY2_);
            action.imagePath = ocrFindImagePath_;
            if (regionByImage) {
                action.matchThreshold = std::clamp(ToDouble(ocrFindMatchThreshold_, 65.0), 1.0, 100.0);
                action.imageScaleMin = std::max(0.1, ToDouble(ocrFindScaleMin_, 1.0));
                action.imageScaleMax = std::max(action.imageScaleMin, ToDouble(ocrFindScaleMax_, action.imageScaleMin));
                action.imageScale = (action.imageScaleMin + action.imageScaleMax) * 0.5;
            }
            action.ocrResultMode = std::clamp(popupOcrResultMode_.sel, 0, 1);
            action.ocrSearchText = GetText(ocrSearchEdit_);
            action.ocrFollowUp = std::clamp(popupOcrFollowUp_.sel, 0, 2);
            action.offsetX = ToInt(ocrOffsetX_);
            action.offsetY = ToInt(ocrOffsetY_);
            action.findUntilFound = Checked(ocrUntilFound_);
            action.matchVarName = Trim(GetText(ocrResultVar_));
            if (action.matchVarName.empty()) action.matchVarName = L"a";
        }
        else if (sel == 19) {
            action.type = ActionType::If;
            action.conditionExpr = GetText(ifConditionList_);
        }
        else if (sel == 20) {
            action.type = ActionType::Else;
        }
        else if (sel == 21) {
            action.type = ActionType::LockScreenshot;
        }
        else if (sel == 22) {
            action.type = ActionType::UnlockScreenshot;
        }
        else if (sel == 23) {
            action.type = ActionType::StopMacro;
        }
        else if (sel == 24) {
            action.type = ActionType::RunProgram;
            action.shortcutPreset = std::clamp(popupRunProgram_.sel, 0, RunProgramPresetCount() - 1);
            action.targetPath = Trim(GetText(runProgramPath_));
            action.inputText = GetText(runProgramArgs_);
            action.blockName = RunProgramDisplayName(action.shortcutPreset, action.targetPath);
        }
        else if (sel == 25) {
            action.type = ActionType::CloseProgram;
            action.targetPath = Trim(GetText(closeProgramPath_));
            action.matchFileNameOnly = closeProgramMatchFileName_ && Checked(closeProgramMatchFileName_);
        }
        else if (sel == 26) {
            action.type = ActionType::OpenWebpage;
            action.targetPath = Trim(GetText(openWebpageUrl_));
        }
        else if (sel == 27) {
            action.type = ActionType::OpenFile;
            action.targetPath = Trim(GetText(openFilePath_));
        }
        else if (sel == 28) {
            action.type = ActionType::TimerRecordTime;
            action.loopVarName = Trim(GetText(timerVarName_));
        }
        else if (sel == 32) {
            action.type = ActionType::GetCursorPos;
            action.matchVarName = Trim(GetText(cursorPosVarName_));
            if (action.matchVarName.empty()) action.matchVarName = L"a";
        }
        else if (sel == 33) {
            action.type = ActionType::Goto;
            action.gotoStepExpr = Trim(GetText(gotoStepEdit_));
        }
        else if (sel == 29) {
            action.type = ActionType::AiTextAnalysis;
            action.aiPrompt = GetText(aiPromptEdit_);
            action.aiOutputVarName = Trim(GetText(aiOutputVarEdit_));
            if (action.aiOutputVarName.empty()) action.aiOutputVarName = L"aiResult";
            action.aiOutputType = std::clamp(popupAiOutputType_.sel, 0, 1);
            action.aiModelName = popupAiModel_.sel >= 0 && popupAiModel_.sel < static_cast<int>(popupAiModel_.items.size())
                ? popupAiModel_.items[static_cast<size_t>(popupAiModel_.sel)] : L"";
            action.aiContextMode = std::clamp(popupAiContextMode_.sel, 0, 3);
            action.aiTimeoutSec = std::max(5, ToInt(aiTimeoutEdit_, 30));
            action.aiFallbackValue = Trim(GetText(aiFallbackEdit_));
        }
        else if (sel == 30) {
            action.type = ActionType::AiImageAnalysis;
            action.aiPrompt = GetText(aiPromptEdit_);
            action.aiOutputVarName = Trim(GetText(aiOutputVarEdit_));
            if (action.aiOutputVarName.empty()) action.aiOutputVarName = L"aiImgResult";
            action.aiOutputType = std::clamp(popupAiOutputType_.sel, 0, 1);
            action.aiModelName = popupAiModel_.sel >= 0 && popupAiModel_.sel < static_cast<int>(popupAiModel_.items.size())
                ? popupAiModel_.items[static_cast<size_t>(popupAiModel_.sel)] : L"";
            action.aiContextMode = std::clamp(popupAiContextMode_.sel, 0, 3);
            action.aiTimeoutSec = std::max(5, ToInt(aiTimeoutEdit_, 30));
            action.aiFallbackValue = Trim(GetText(aiFallbackEdit_));
            action.aiImageScale = std::clamp(ToDouble(aiImageScaleEdit_, 1.0), 0.1, 1.0);
            action.aiRegionByImage = aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_);
            action.aiTargetImagePath = aiFindImagePath_;
            action.aiSearchX1 = ToInt(aiSearchX1Edit_);
            action.aiSearchY1 = ToInt(aiSearchY1Edit_);
            action.aiSearchX2 = ToInt(aiSearchX2Edit_);
            action.aiSearchY2 = ToInt(aiSearchY2Edit_);
            action.searchFullScreen = aiFullScreen_;
            if (action.aiRegionByImage) {
                action.matchThreshold = std::clamp(ToDouble(aiFindMatchThreshold_, 65.0), 1.0, 100.0);
                action.imageScaleMin = std::max(0.1, ToDouble(aiFindScaleMin_, 0.9));
                action.imageScaleMax = std::max(action.imageScaleMin, ToDouble(aiFindScaleMax_, 1.1));
                action.imageScale = (action.imageScaleMin + action.imageScaleMax) * 0.5;
                action.imageRegionX1 = ToInt(aiImageRegionX1_);
                action.imageRegionY1 = ToInt(aiImageRegionY1_);
                action.imageRegionX2 = ToInt(aiImageRegionX2_);
                action.imageRegionY2 = ToInt(aiImageRegionY2_);
            }
        }
        else if (sel == 31) {
            action.type = ActionType::AiActionExecute;
            action.aiPrompt = GetText(aiPromptEdit_);
            action.aiModelName = popupAiModel_.sel >= 0 && popupAiModel_.sel < static_cast<int>(popupAiModel_.items.size())
                ? popupAiModel_.items[static_cast<size_t>(popupAiModel_.sel)] : L"";
            action.aiContextMode = std::clamp(popupAiContextMode_.sel, 0, 3);
            action.aiTimeoutSec = std::max(5, ToInt(aiTimeoutEdit_, 30));
            action.aiFallbackValue = Trim(GetText(aiFallbackEdit_));
            action.aiImageScale = 0.5;
            action.aiWithImage = aiWithImageCheck_ && Checked(aiWithImageCheck_);
            action.aiLogicConvert = aiLogicConvertCheck_ && Checked(aiLogicConvertCheck_);
            action.aiRegionByImage = action.aiWithImage && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_);
            action.aiTargetImagePath = aiFindImagePath_;
            action.aiSearchX1 = ToInt(aiSearchX1Edit2_);
            action.aiSearchY1 = ToInt(aiSearchY1Edit2_);
            action.aiSearchX2 = ToInt(aiSearchX2Edit2_);
            action.aiSearchY2 = ToInt(aiSearchY2Edit2_);
            action.searchFullScreen = aiFullScreen_;
            if (action.aiRegionByImage) {
                action.matchThreshold = std::clamp(ToDouble(aiFindMatchThreshold_, 65.0), 1.0, 100.0);
                action.imageScaleMin = std::max(0.1, ToDouble(aiFindScaleMin_, 0.9));
                action.imageScaleMax = std::max(action.imageScaleMin, ToDouble(aiFindScaleMax_, 1.1));
                action.imageScale = (action.imageScaleMin + action.imageScaleMax) * 0.5;
                action.imageRegionX1 = ToInt(aiImageRegionX1_);
                action.imageRegionY1 = ToInt(aiImageRegionY1_);
                action.imageRegionX2 = ToInt(aiImageRegionX2_);
                action.imageRegionY2 = ToInt(aiImageRegionY2_);
            }
            action.aiMaxSteps = ToInt(aiMaxStepsEdit_, 10);
        }
        else { action.type = ActionType::MoveMouse; }
        return action;
    }

    void LoadForm(const ScriptAction& action);

    // findImagePath_ now stored in ScriptAction, above functions use utils::FindImagesDir()/EnsureFindImagesDir()

    void UpdateFindImagePreview() {
        if (findImagePreviewBitmap_) {
            DeleteBitmapHandle(findImagePreviewBitmap_);
            findImagePreviewBitmap_ = nullptr;
        }
        if (!findImagePath_.empty()) {
            findImagePreviewBitmap_ = LoadBitmapFromFile(ResolveImagePath(findImagePath_));
        }
        if (findImagePreviewBtn_) {
            RedrawWindow(findImagePreviewBtn_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        }
    }

    void UpdateOcrFindImagePreview() {
        if (ocrFindImagePreviewBitmap_) {
            DeleteBitmapHandle(ocrFindImagePreviewBitmap_);
            ocrFindImagePreviewBitmap_ = nullptr;
        }
        if (!ocrFindImagePath_.empty()) {
            ocrFindImagePreviewBitmap_ = LoadBitmapFromFile(ocrFindImagePath_);
        }
        if (ocrFindImagePreviewBtn_) {
            RedrawWindow(ocrFindImagePreviewBtn_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        }
    }

    void UpdateAiFindImagePreview() {
        if (aiFindImagePreviewBitmap_) {
            DeleteBitmapHandle(aiFindImagePreviewBitmap_);
            aiFindImagePreviewBitmap_ = nullptr;
        }
        if (!aiFindImagePath_.empty()) {
            aiFindImagePreviewBitmap_ = LoadBitmapFromFile(aiFindImagePath_);
        }
        if (aiFindImagePreviewBtn_) {
            RedrawWindow(aiFindImagePreviewBtn_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        }
    }

    void SetAiRegionCoords(int x1, int y1, int x2, int y2) {
        const std::wstring sx1 = std::to_wstring(x1);
        const std::wstring sy1 = std::to_wstring(y1);
        const std::wstring sx2 = std::to_wstring(x2);
        const std::wstring sy2 = std::to_wstring(y2);
        if (popupAction_.sel == 30) {
            SetText(aiSearchX1Edit_, sx1);
            SetText(aiSearchY1Edit_, sy1);
            SetText(aiSearchX2Edit_, sx2);
            SetText(aiSearchY2Edit_, sy2);
        } else if (popupAction_.sel == 31) {
            SetText(aiSearchX1Edit2_, sx1);
            SetText(aiSearchY1Edit2_, sy1);
            SetText(aiSearchX2Edit2_, sx2);
            SetText(aiSearchY2Edit2_, sy2);
        } else {
            if (aiSearchX1Edit_) { SetText(aiSearchX1Edit_, sx1); SetText(aiSearchY1Edit_, sy1); SetText(aiSearchX2Edit_, sx2); SetText(aiSearchY2Edit_, sy2); }
            if (aiSearchX1Edit2_) { SetText(aiSearchX1Edit2_, sx1); SetText(aiSearchY1Edit2_, sy1); SetText(aiSearchX2Edit2_, sx2); SetText(aiSearchY2Edit2_, sy2); }
        }
    }

    bool ResolveOcrAbsRegionFromFindImage(int relX1, int relY1, int relX2, int relY2,
                                          int& outX1, int& outY1, int& outX2, int& outY2) const {
        if (ocrFindImagePath_.empty()) return false;
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenRect(vsX, vsY, vsW, vsH);
        int findX1 = vsX, findY1 = vsY, findX2 = vsX + vsW, findY2 = vsY + vsH;
        if (!ocrFullScreen_) {
            const int sx1 = ToInt(ocrX1_), sy1 = ToInt(ocrY1_);
            const int sx2 = ToInt(ocrX2_), sy2 = ToInt(ocrY2_);
            if (sx2 > sx1 && sy2 > sy1) {
                findX1 = sx1; findY1 = sy1; findX2 = sx2; findY2 = sy2;
            }
        }
        const double threshold = std::clamp(ToDouble(ocrFindMatchThreshold_, 65.0), 1.0, 100.0);
        const double scaleMin = std::max(0.1, ToDouble(ocrFindScaleMin_, 1.0));
        const double scaleMax = std::max(scaleMin, ToDouble(ocrFindScaleMax_, scaleMin));
        const TemplateScale ts = ComputeTemplateScale(
            ScriptCoordMetaForExecution(loadedCoordMeta_), vsW, vsH);
        HBITMAP tmpl = LoadBitmapFromFile(ocrFindImagePath_);
        if (!tmpl) return false;
        ScriptAction probe{};
        probe.matchThreshold = threshold;
        probe.imageScaleMin = scaleMin;
        probe.imageScaleMax = scaleMax;
        probe.imageRegionX1 = relX1;
        probe.imageRegionY1 = relY1;
        probe.imageRegionX2 = relX2;
        probe.imageRegionY2 = relY2;
        ImageMatchOptions opt = BuildExecutionFindImageOptions(probe, ts);
        RestrictFindImageToSingleAnchor(opt);
        opt.maxOverlap = 0.5;
        const ImageMatchOutput output = FindTemplateOnScreenMulti(
            findX1, findY1, findX2, findY2, tmpl, opt);
        DeleteBitmapHandle(tmpl);
        if (output.matches.empty()) return false;
        const ImageMatchResult& match = output.matches.front();
        return ApplyImageRegionToMatch(probe,
            match.topLeftX, match.topLeftY, match.bottomRightX, match.bottomRightY,
            outX1, outY1, outX2, outY2);
    }

    void RefreshCoordFieldEdits(std::initializer_list<HWND> edits) {
        for (HWND h : edits) {
            if (!h) continue;
            InvalidateRect(h, nullptr, TRUE);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        }
        RepaintParamPanelChrome();
    }

    void EnsureFindImageRegionDefaults() {
        findImageFullScreen_ = true;
        ApplyFindImageFullScreen();
    }

    void EnsureOcrRegionDefaults() {
        ocrFullScreen_ = true;
        ApplyOcrFullScreen();
    }

    void EnsureAiRegionDefaults() {
        aiFullScreen_ = true;
        ApplyAiFullScreen();
    }

    void ApplyFindImageFullScreen() {
        findImageFullScreen_ = true;
        int x = 0, y = 0, w = 0, h = 0;
        GetVirtualScreenRect(x, y, w, h);
        SetText(findX1_, std::to_wstring(x));
        SetText(findY1_, std::to_wstring(y));
        SetText(findX2_, std::to_wstring(x + w));
        SetText(findY2_, std::to_wstring(y + h));
        RefreshCoordFieldEdits({findX1_, findY1_, findX2_, findY2_});
    }

    void BeginFindRegionSelect(bool captureTemplate) {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(captureTemplate ? L"屏幕截图" : L"选取区域");
        // Store previous window state and hide it
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this, captureTemplate](RECT sel) {
            if (sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0) {
                RestoreEditorAfterScreenOverlay();
                return;
            }
            // Client coords from overlay → screen coords (add virtual screen origin)
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            const int x1 = sel.left + vsX, y1 = sel.top + vsY;
            const int x2 = sel.right + vsX, y2 = sel.bottom + vsY;
            if (captureTemplate) {
                // Screenshot mode: capture template image ONLY — do NOT touch search region
                EnsureFindImagesDir();
                const std::wstring path = FindImagesDir() + L"\\template_"
                    + std::to_wstring(GetTickCount()) + L".bmp";
                HBITMAP bmp = CaptureScreenRegion(x1, y1, x2, y2);
            if (bmp && SaveBitmapToFile(bmp, path)) {
                findImagePath_ = path;
                newImagePaths_.insert(path);
                findImageFullScreen_ = false;
                if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
                    actions_[static_cast<size_t>(selectedIndex_)].imagePath = path;
                }
                UpdateFindImagePreview();
                }
                DeleteBitmapHandle(bmp);
            } else {
                // Region select mode: update coordinates only
                findImageFullScreen_ = false;
                SetText(findX1_, std::to_wstring(x1));
                SetText(findY1_, std::to_wstring(y1));
                SetText(findX2_, std::to_wstring(x2));
                SetText(findY2_, std::to_wstring(y2));
            }
            RestoreEditorAfterScreenOverlay();
        });
    }

    bool HasFindImageTemplate(const std::wstring& path) const {
        if (path.empty()) return false;
        const std::wstring resolved = ResolveImagePath(path);
        return GetFileAttributesW(resolved.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

        void BeginOcrFindRegionSelect() {
        if (!ocrRegionByImageCheck_ || !Checked(ocrRegionByImageCheck_)) return;
        if (!HasFindImageTemplate(ocrFindImagePath_)) {
            ShowPromptInfo(L"请先设置要查找的图片。");
            return;
        }
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenRect(vsX, vsY, vsW, vsH);
        int findX1 = vsX, findY1 = vsY, findX2 = vsX + vsW, findY2 = vsY + vsH;
        if (!ocrFullScreen_) {
            const int sx1 = ToInt(ocrX1_), sy1 = ToInt(ocrY1_);
            const int sx2 = ToInt(ocrX2_), sy2 = ToInt(ocrY2_);
            if (sx2 > sx1 && sy2 > sy1) {
                findX1 = sx1; findY1 = sy1; findX2 = sx2; findY2 = sy2;
            }
        }
        const double threshold = std::clamp(ToDouble(ocrFindMatchThreshold_, 65.0), 1.0, 100.0);
        const double scaleMin = std::max(0.1, ToDouble(ocrFindScaleMin_, 1.0));
        const double scaleMax = std::max(scaleMin, ToDouble(ocrFindScaleMax_, scaleMin));

        if (!matchOverlay_) matchOverlay_ = std::make_unique<MatchOverlay>();
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);

        const auto result = matchOverlay_->Show(
            ocrFindImagePath_, findX1, findY1, findX2, findY2,
            threshold, scaleMin, scaleMax, MatchOverlayMode::RelativeRegionPick);

        RestoreEditorAfterScreenOverlay();

        if (!result.cancelled && result.regionValid && matchOverlay_->matchResult_.found) {
            const int anchorX = matchOverlay_->matchResult_.topLeftX;
            const int anchorY = matchOverlay_->matchResult_.topLeftY;
            SetText(ocrImageRegionX1_, std::to_wstring(result.regionX1 - anchorX));
            SetText(ocrImageRegionY1_, std::to_wstring(result.regionY1 - anchorY));
            SetText(ocrImageRegionX2_, std::to_wstring(result.regionX2 - anchorX));
            SetText(ocrImageRegionY2_, std::to_wstring(result.regionY2 - anchorY));
        }
    }

    void BeginOcrFindScreenshot() {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(L"屏幕截图");
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this](RECT sel) {
            const bool cancelled = sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0;
            if (!cancelled) {
                const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int x1 = sel.left + vsX, y1 = sel.top + vsY;
                const int x2 = sel.right + vsX, y2 = sel.bottom + vsY;
                EnsureFindImagesDir();
                const std::wstring path = FindImagesDir() + L"\\template_"
                    + std::to_wstring(GetTickCount()) + L".bmp";
                HBITMAP bmp = CaptureScreenRegion(x1, y1, x2, y2);
                if (bmp && SaveBitmapToFile(bmp, path)) {
                    ocrFindImagePath_ = path;
                    newImagePaths_.insert(path);
                    if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
                        actions_[static_cast<size_t>(selectedIndex_)].imagePath = path;
                    }
                    UpdateOcrFindImagePreview();
                }
                DeleteBitmapHandle(bmp);
            }
            RestoreEditorAfterScreenOverlay();
        });
    }

    void LoadOcrFindImageFromFile() {
        wchar_t fileName[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"图片文件\0*.bmp;*.png;*.jpg;*.jpeg\0所有文件\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        const std::wstring copied = EnsureImageInLibrary(fileName);
        ocrFindImagePath_ = copied;
        if (IsPathInImageDir(copied)) newImagePaths_.insert(copied);
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
            actions_[static_cast<size_t>(selectedIndex_)].imagePath = copied;
        }
        UpdateOcrFindImagePreview();
    }

    void ClearOcrFindImage() {
        if (!ocrFindImagePath_.empty() && IsPathInImageDir(ocrFindImagePath_)) {
            auto it = newImagePaths_.find(ocrFindImagePath_);
            if (it != newImagePaths_.end()) {
                const auto allRefs = CollectAllReferencedImages();
                if (allRefs.find(ocrFindImagePath_) == allRefs.end()) {
                    DeleteFileW(ocrFindImagePath_.c_str());
                }
                newImagePaths_.erase(it);
            }
        }
        ocrFindImagePath_.clear();
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
            actions_[static_cast<size_t>(selectedIndex_)].imagePath.clear();
        }
        UpdateOcrFindImagePreview();
        RefreshGrayButtonsInParamViewport();
    }

    void BeginOcrOffsetSelect() {
        if (popupOcrFollowUp_.sel == 2) return;

        const bool searchMode = popupOcrResultMode_.sel == 1;
        if (searchMode) {
            if (OcrSearchTextContainsVariable()) return;
            if (Trim(GetText(ocrSearchEdit_)).empty()) {
                ShowPromptInfo(L"请先输入要在结果中查找的文字。");
                return;
            }
        }

        const OcrEnvStatus env = CheckOcrEnvironment(false);
        if (env.state != OcrEnvState::Ready) {
            ShowOcrInstallDialog();
            return;
        }

        const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
        if (regionByImage && ocrFindImagePath_.empty()) {
            ShowPromptInfo(L"请先设置要查找的图片。");
            return;
        }

        HideEditorForScreenCapture();

        int sx1 = ToInt(ocrX1_), sy1 = ToInt(ocrY1_), sx2 = ToInt(ocrX2_), sy2 = ToInt(ocrY2_);
        if (regionByImage) {
            if (!ResolveOcrAbsRegionFromFindImage(
                    ToInt(ocrImageRegionX1_), ToInt(ocrImageRegionY1_),
                    ToInt(ocrImageRegionX2_), ToInt(ocrImageRegionY2_),
                    sx1, sy1, sx2, sy2)) {
                RestoreEditorAfterScreenCapture();
                ShowPromptInfo(L"未找到参考图片，无法选择偏移位置。");
                return;
            }
        } else if (ocrFullScreen_) {
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            sx1 = vsX; sy1 = vsY; sx2 = vsX + vsW; sy2 = vsY + vsH;
        }

        std::wstring searchTarget;
        if (searchMode) {
            MacroVariableContext ctx;
            ctx.matchVars = &matchVars_;
            ctx.matchListVars = &matchListVars_;
            ctx.ocrVars = &ocrVars_;
            ctx.loopVars = &loopVars_;
            ctx.timerStarts = &timerStarts_;
            ctx.curLoops = curLoops_;
            searchTarget = ResolveMacroVariables(GetText(ocrSearchEdit_), ctx);
        }

        if (!ocrOverlay_) ocrOverlay_ = std::make_unique<OcrOverlay>();
        EnsureOcrSession();
        const bool digitsOnly = ocrDigitsOnlyCheck_ && Checked(ocrDigitsOnlyCheck_);
        const auto result = ocrOverlay_->Show(sx1, sy1, sx2, sy2, searchTarget, OcrOverlayMode::OffsetPick, digitsOnly);
        ReleaseOcrSession();

        RestoreEditorAfterScreenCapture();

        if (!result.cancelled && result.anchorValid) {
            SetText(ocrOffsetX_, std::to_wstring(result.offsetX));
            SetText(ocrOffsetY_, std::to_wstring(result.offsetY));
        }
    }

    void BeginFindOffsetSelect() {
        if (findImagePath_.empty()) {
            ShowPromptInfo(L"请先设置要查找的图片。");
            return;
        }
        int sx1 = ToInt(findX1_), sy1 = ToInt(findY1_), sx2 = ToInt(findX2_), sy2 = ToInt(findY2_);
        if (findImageFullScreen_) {
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            sx1 = vsX; sy1 = vsY; sx2 = vsX + vsW; sy2 = vsY + vsH;
        }
        const double threshold = std::clamp(ToDouble(findMatchThreshold_, 65.0), 1.0, 100.0);
        const double scaleMin = std::max(0.1, ToDouble(findScaleMin_, 1.0));
        const double scaleMax = std::max(scaleMin, ToDouble(findScaleMax_, scaleMin));

        if (!matchOverlay_) matchOverlay_ = std::make_unique<MatchOverlay>();
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);

        auto result = matchOverlay_->Show(findImagePath_, sx1, sy1, sx2, sy2, threshold, scaleMin, scaleMax,
                                          MatchOverlayMode::OffsetPick);

        RestoreEditorAfterScreenOverlay();

        if (!result.cancelled && matchOverlay_->matchResult_.found) {
            SetText(findOffsetX_, std::to_wstring(result.offsetX));
            SetText(findOffsetY_, std::to_wstring(result.offsetY));
        }
    }

    void EndFindOffsetSelect() {
        // No-op: offset selection is now handled by MatchOverlay in BeginFindOffsetSelect()
    }

    void LoadFindImageFromFile() {
        wchar_t fileName[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"图片文件\0*.bmp;*.png;*.jpg;*.jpeg\0所有文件\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        const std::wstring copied = EnsureImageInLibrary(fileName);
        findImagePath_ = copied;
        if (IsPathInImageDir(copied)) newImagePaths_.insert(copied);
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
            actions_[static_cast<size_t>(selectedIndex_)].imagePath = copied;
        }
        UpdateFindImagePreview();
    }

    void OpenFindImageCropEditor() {
        // 产品裁切走 Web #ov-crop → FindImageCropRect；GDI 裁切编辑器已移除。
        ShowPromptInfo(L"请使用编辑器中的网页裁切工具。");
    }

    void BeginAiRegionSelect() {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(L"选取分析区域");
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this](RECT sel) {
            if (sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0) {
                RestoreEditorAfterScreenOverlay();
                return;
            }
            const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            aiFullScreen_ = false;
            SetAiRegionCoords(
                sel.left + vsX, sel.top + vsY,
                sel.right + vsX, sel.bottom + vsY);
            RestoreEditorAfterScreenOverlay();
        });
    }

    void BeginAiFindRegionSelect() {
        const bool regionByImage = (popupAction_.sel == 30 && aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_))
            || (popupAction_.sel == 31 && aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_));
        if (!regionByImage) return;
        if (!HasFindImageTemplate(aiFindImagePath_)) {
            ShowPromptInfo(L"请先设置要查找的图片。");
            return;
        }
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenRect(vsX, vsY, vsW, vsH);
        int findX1 = vsX, findY1 = vsY, findX2 = vsX + vsW, findY2 = vsY + vsH;
        const bool useEdit2 = (popupAction_.sel == 31);
        const int sx1 = ToInt(useEdit2 ? aiSearchX1Edit2_ : aiSearchX1Edit_);
        const int sy1 = ToInt(useEdit2 ? aiSearchY1Edit2_ : aiSearchY1Edit_);
        const int sx2 = ToInt(useEdit2 ? aiSearchX2Edit2_ : aiSearchX2Edit_);
        const int sy2 = ToInt(useEdit2 ? aiSearchY2Edit2_ : aiSearchY2Edit_);
        if (!aiFullScreen_ && sx2 > sx1 && sy2 > sy1) {
            findX1 = sx1; findY1 = sy1; findX2 = sx2; findY2 = sy2;
        }
        const double threshold = std::clamp(ToDouble(aiFindMatchThreshold_, 65.0), 1.0, 100.0);
        const double scaleMin = std::max(0.1, ToDouble(aiFindScaleMin_, 1.0));
        const double scaleMax = std::max(scaleMin, ToDouble(aiFindScaleMax_, scaleMin));

        if (!matchOverlay_) matchOverlay_ = std::make_unique<MatchOverlay>();
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);

        const auto result = matchOverlay_->Show(
            aiFindImagePath_, findX1, findY1, findX2, findY2,
            threshold, scaleMin, scaleMax, MatchOverlayMode::RelativeRegionPick);

        RestoreEditorAfterScreenOverlay();

        if (!result.cancelled && result.regionValid && matchOverlay_->matchResult_.found) {
            const int anchorX = matchOverlay_->matchResult_.topLeftX;
            const int anchorY = matchOverlay_->matchResult_.topLeftY;
            SetText(aiImageRegionX1_, std::to_wstring(result.regionX1 - anchorX));
            SetText(aiImageRegionY1_, std::to_wstring(result.regionY1 - anchorY));
            SetText(aiImageRegionX2_, std::to_wstring(result.regionX2 - anchorX));
            SetText(aiImageRegionY2_, std::to_wstring(result.regionY2 - anchorY));
        }
    }

    void BeginAiFindScreenshot() {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(L"屏幕截图");
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this](RECT sel) {
            const bool cancelled = sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0;
            if (!cancelled) {
                const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int x1 = sel.left + vsX, y1 = sel.top + vsY;
                const int x2 = sel.right + vsX, y2 = sel.bottom + vsY;
                EnsureFindImagesDir();
                const std::wstring path = FindImagesDir() + L"\\template_"
                    + std::to_wstring(GetTickCount()) + L".bmp";
                HBITMAP bmp = CaptureScreenRegion(x1, y1, x2, y2);
                if (bmp && SaveBitmapToFile(bmp, path)) {
                    aiFindImagePath_ = path;
                    newImagePaths_.insert(path);
                    UpdateAiFindImagePreview();
                }
                DeleteBitmapHandle(bmp);
            }
            RestoreEditorAfterScreenOverlay();
        });
    }

    void LoadAiFindImageFromFile() {
        wchar_t fileName[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"图片文件\0*.bmp;*.png;*.jpg;*.jpeg\0所有文件\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        const std::wstring copied = EnsureImageInLibrary(fileName);
        aiFindImagePath_ = copied;
        if (IsPathInImageDir(copied)) newImagePaths_.insert(copied);
        UpdateAiFindImagePreview();
        if (popupAction_.sel == 30 || popupAction_.sel == 31) RebuildParamPanelLayout();
    }

    void ClearAiFindImage() {
        if (!aiFindImagePath_.empty() && IsPathInImageDir(aiFindImagePath_)) {
            auto it = newImagePaths_.find(aiFindImagePath_);
            if (it != newImagePaths_.end()) {
                const auto allRefs = CollectAllReferencedImages();
                if (allRefs.find(aiFindImagePath_) == allRefs.end()) {
                    DeleteFileW(aiFindImagePath_.c_str());
                }
                newImagePaths_.erase(it);
            }
        }
        aiFindImagePath_.clear();
        UpdateAiFindImagePreview();
        if (popupAction_.sel == 30 || popupAction_.sel == 31) RebuildParamPanelLayout();
    }

    void BrowseProgramExecutable(HWND targetEdit) {
        if (!targetEdit) return;
        wchar_t fileName[MAX_PATH]{};
        const std::wstring current = Trim(GetText(targetEdit));
        if (!current.empty()) wcsncpy_s(fileName, current.c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"可执行文件 (*.exe;*.msc;*.bat;*.cmd)\0*.exe;*.msc;*.bat;*.cmd\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        SetText(targetEdit, fileName);
    }

    void BrowseOpenFile(HWND targetEdit) {
        if (!targetEdit) return;
        wchar_t fileName[MAX_PATH]{};
        const std::wstring current = Trim(GetText(targetEdit));
        if (!current.empty()) wcsncpy_s(fileName, current.c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        SetText(targetEdit, fileName);
    }

    void ClearFindImage() {
        // 如果是编辑期间新截图的图片且未保存到脚本，删除残留文件
        if (!findImagePath_.empty() && IsPathInImageDir(findImagePath_)) {
            auto it = newImagePaths_.find(findImagePath_);
            if (it != newImagePaths_.end()) {
                // 检查是否被任何已有脚本引用
                const auto allRefs = CollectAllReferencedImages();
                if (allRefs.find(findImagePath_) == allRefs.end()) {
                    DeleteFileW(findImagePath_.c_str());
                }
                newImagePaths_.erase(it);
            }
        }
        findImagePath_.clear();
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
            actions_[static_cast<size_t>(selectedIndex_)].imagePath.clear();
        }
        UpdateFindImagePreview();
        RefreshGrayButtonsInParamViewport();
    }

    void TestFindImage() {
        if (findTestRunning_.exchange(true)) return;
        auto finishTest = [&]() {
            findTestRunning_ = false;
            if (findTestBtn_) {
                EnableWindow(findTestBtn_, TRUE);
                FinishGrayButtonClick(findTestBtn_);
            }
        };
        // 无图时在此直接返回，不调用 MatchOverlay / 屏幕冻结
        const std::wstring testPath = ResolveImagePath(findImagePath_);
        if (!HasFindImageTemplate(testPath)) {
            finishTest();
            QueuePromptInfo(L"请先设置要查找的图片。");
            return;
        }
        if (findTestBtn_) EnableWindow(findTestBtn_, FALSE);

        if (!matchOverlay_) matchOverlay_ = std::make_unique<MatchOverlay>();
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);

        int sx1 = ToInt(findX1_), sy1 = ToInt(findY1_), sx2 = ToInt(findX2_), sy2 = ToInt(findY2_);
        if (findImageFullScreen_) {
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            sx1 = vsX; sy1 = vsY; sx2 = vsX + vsW; sy2 = vsY + vsH;
        }
        const double threshold = std::clamp(ToDouble(findMatchThreshold_, 65.0), 1.0, 100.0);
        const double scaleMin = std::max(0.1, ToDouble(findScaleMin_, 1.0));
        const double scaleMax = std::max(scaleMin, ToDouble(findScaleMax_, scaleMin));

        ScriptAction probe{};
        probe.matchThreshold = threshold;
        probe.imageScaleMin = scaleMin;
        probe.imageScaleMax = scaleMax;
        const CoordMeta execMeta = ScriptCoordMetaForExecution(loadedCoordMeta_);
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenRect(vsX, vsY, vsW, vsH);
        const TemplateScale ts = ComputeTemplateScale(execMeta, vsW, vsH);
        const ImageMatchOptions findOpt = BuildExecutionFindImageOptions(probe, ts);

        matchOverlay_->Show(testPath, sx1, sy1, sx2, sy2, findOpt,
                            MatchOverlayMode::Test);

        RestoreEditorAfterScreenOverlay();
        finishTest();
    }

    void OnFindTestDone(int /*found*/, int /*lParam*/) {
        // Kept for backward compatibility; current TestFindImage uses MatchOverlay inline.
        findTestRunning_ = false;
        if (findTestBtn_) EnableWindow(findTestBtn_, TRUE);
    }

    // ── WM_COMMAND dispatch ────────────────────────────────────────
    void OnCommand(int id, int code, HWND ctrl) {
        struct GrayButtonClickReset {
            EngineHost* self = nullptr;
            HWND btn = nullptr;
            int notifyCode = 0;
            ~GrayButtonClickReset() {
                // WM_LBUTTONUP 已在子类过程中复位；此处仅兜底键盘触发的 BN_CLICKED
                if (self && btn && notifyCode == BN_CLICKED && self->IsGrayButton(btn)
                    && GetFocus() == btn) {
                    self->FinishGrayButtonClick(btn);
                }
            }
        } grayClickReset{this, ctrl, code};
        if ((code == CBN_DROPDOWN || code == CBN_CLOSEUP) && ctrl) {
            if (code == CBN_DROPDOWN) StyleComboDropdownList(ctrl);
            InvalidateRect(ctrl, nullptr, FALSE);
        }
        if (id == kClose) { if (page_ == Page::Editor) ShowHome(); else SendMessageW(hwnd_, WM_CLOSE, 0, 0); return; }
        if (id == kActionCombo && code == CBN_SELCHANGE) { if (!loadingForm_) { RefreshParamPanel(); InvalidateRect(hwnd_, nullptr, FALSE); } return; }
        if (id == kCancel) { ShowHome(); return; }
        if (id == kSave) { SaveIfEditor(); ShowHome(); return; }
        if (id == kClear) { ClearEditorActions(); return; }
        if (id == kLoad) { EnterBatchEditMode(); return; }
        if (id == kBatchExit) { ExitBatchEditMode(); return; }
        if (id == kBatchSelectAll) { BatchSelectAll(); return; }
        if (id == kBatchDeselect) { BatchDeselectAll(); return; }
        if (id == kBatchDelete) { BatchDeleteSelected(); return; }
        if (id == kBatchCopy) { BatchCopySelected(); return; }
        if (id == kKeyCapture) { CaptureActionKey(); return; }
        if (id == kKeyPressCapture) { CaptureKeyPress(); return; }
        if (id == kQuickInputInsert) { InsertQuickInputVariable(); return; }
        if (id == kOcrSearchVarInsert) { InsertOcrSearchVariable(); return; }
        if (id == kOcrSearchText && code == EN_CHANGE && !loadingForm_ && popupAction_.sel == 18) {
            RefreshOcrSubPanel();
            return;
        }
        if (id == kIfAddCondition) { AppendIfCondition(); return; }
        if (id == kRunProgramBrowse || id == kCloseProgramBrowse) { BrowseProgramExecutable(id == kRunProgramBrowse ? runProgramPath_ : closeProgramPath_); return; }
        if (id == kOpenFileBrowse) { BrowseOpenFile(openFilePath_); return; }
        if (id == kFindFullScreen) { ApplyFindImageFullScreen(); return; }
        if (id == kFindSelectRegion) { BeginFindRegionSelect(false); return; }
        if (id == kFindScreenshot) { BeginFindRegionSelect(true); return; }
        if (id == kFindLocalImage) { LoadFindImageFromFile(); return; }
        if (id == kFindImagePreview) { OpenFindImageCropEditor(); return; }
        if (id == kFindClearImage) { ClearFindImage(); return; }
        if (id == kFindTest) {
            FiDbgLogFmt(L"ON_COMMAND_FIND_TEST",
                L"ctrl=%p parent=%p chain=%s",
                ctrl, ctrl ? GetParent(ctrl) : nullptr,
                FiDbgWindowChain(ctrl).c_str());
            TestFindImage();
            return;
        }
        if (id == kFindSelectOffset) { BeginFindOffsetSelect(); return; }
        if (id == kOcrFullScreen) { ApplyOcrFullScreen(); return; }
        if (id == kOcrSelectRegion) { BeginOcrRegionSelect(); return; }
        if (id == kOcrSelectOffset) { BeginOcrOffsetSelect(); return; }
        if (id == kOcrTest) { TestOcr(); return; }
        if (id == kOcrInstallDep) { ShowOcrInstallDialog(); return; }
        if (id == kOcrFindSelectRegion) { BeginOcrFindRegionSelect(); return; }
        if (id == kOcrFindScreenshot) { BeginOcrFindScreenshot(); return; }
        if (id == kOcrFindLocalImage) { LoadOcrFindImageFromFile(); return; }
        if (id == kOcrFindImagePreview) { LoadOcrFindImageFromFile(); return; }
        if (id == kOcrFindClearImage) { ClearOcrFindImage(); return; }
        if (id == kAiInsertVar) { InsertAiPromptVariable(); return; }
        if (id == kAiSelectRegion) { BeginAiRegionSelect(); return; }
        if (id == kAiFullScreen) { ApplyAiFullScreen(); return; }
        if (id == kAiFindSelectRegion) { BeginAiFindRegionSelect(); return; }
        if (id == kAiTargetScreenshot) { BeginAiFindScreenshot(); return; }
        if (id == kAiTargetLocal) { LoadAiFindImageFromFile(); return; }
        if (id == kAiTargetPreview) { LoadAiFindImageFromFile(); return; }
        if (id == kAiTargetClear) { ClearAiFindImage(); return; }
        if (id == kListRemarkEdit && code == EN_KILLFOCUS) { CommitInlineRemark(); return; }
        if (id == kModify) { ModifySelected(); return; }
        if (id == kAdd) { ShowAddMenu(); return; }
        if (id >= kCopyLast && id <= kCopyAfterSelected) { CopyActionByMenu(id); return; }
        if (id >= kAddLast && id <= kAddAsChild) { AddActionByMenu(id); return; }
        if (id >= kHotCustom && id <= kHotSpace) { SetCommonHotkeyFromMenu(id); return; }
        if (id == kWmSpecifyWindowBtn && code == BN_CLICKED) { ShowWindowClassPickDialog(); return; }
        if (id == kWmTargetBrowse && code == BN_CLICKED) { BrowseProgramExecutable(wmTargetPathEdit_); return; }
        if (id == kWmFakeFocus && code == BN_CLICKED) {
            // 鼠标点击由 EditorChildSubclassProc → ToggleParamCheckbox 处理；此处仅同步到 config。
            SyncScriptWindowModeFromEditor();
            return;
        }
    }

    void InsertAction(size_t pos, const ScriptAction& src, int indentOverride = -1, bool selectInserted = true) {
        ScriptAction action = src;
        action.originalNo = NextNo();
        if (indentOverride >= 0) action.indent = indentOverride;
        pos = std::min(pos, actions_.size());
        if (action.type == ActionType::EndLoop && !HasLoopParentAt(actions_, pos, action.indent)) {
            ShowPromptInfo(kEndLoopNeedsLoopParentMsg);
            return;
        }
        actions_.insert(actions_.begin() + static_cast<std::ptrdiff_t>(pos), action);
        if (editorActionParsed_.size() + 1 == actions_.size()) {
            editorActionParsed_.insert(editorActionParsed_.begin() + static_cast<std::ptrdiff_t>(pos), 1);
        } else {
            editorActionParsed_.assign(actions_.size(), 1);
            editorActionBlocks_.clear();
            editorParsePending_ = false;
            editorParseCursor_ = static_cast<int>(actions_.size());
        }
        if (!editorActionBlocks_.empty() && editorActionBlocks_.size() + 1 == actions_.size()) {
            editorActionBlocks_.insert(editorActionBlocks_.begin() + static_cast<std::ptrdiff_t>(pos), std::wstring{});
        }
        if (editorParseCursor_ > static_cast<int>(pos)) ++editorParseCursor_;
        MarkVisibleActionsDirty();
        if (selectInserted) {
            selectedIndex_ = static_cast<int>(pos);
        } else if (selectedIndex_ >= static_cast<int>(pos)) {
            ++selectedIndex_;
        }
        {
            std::set<int> updated;
            for (int idx : collapsedContainers_) {
                updated.insert(idx >= static_cast<int>(pos) ? idx + 1 : idx);
            }
            collapsedContainers_ = std::move(updated);
        }
        if (IsExpandableContainer(action.type)) collapsedContainers_.erase(static_cast<int>(pos));
        RenumberActions();
        if (selectInserted) {
            EnsureSelectedVisible();
            UpdateEditMode();
        } else {
            RefreshActionListLayer();
        }
        OnActionsChanged();
    }

    void ShowAddMenu() { ShowAddMenuAt(0, 0); }

    void ShowAddMenuAt(int x, int y) {
        if (selectedIndex_ < 0) { TryInsertActionFromForm(actions_.size(), 0); return; }
        const ScriptAction preview = ActionFromForm();
        if (preview.type == ActionType::DefineBlock) { TryInsertActionFromForm(0, 0); return; }
        std::vector<ThemedPopupMenuItem> items{
            {kAddLast, L"添加到最后"},
            {kAddFirst, L"插入到最前"},
            {kAddBeforeSelected, L"插入到选择项前"},
            {kAddAfterSelected, L"插入到选择项后"},
        };
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())
            && IsSubtreeContainer(actions_[static_cast<size_t>(selectedIndex_)].type)) {
            items.push_back({kAddAsChild, L"添加为子节点"});
        }
        POINT pt{x, y};
        if (x == 0 && y == 0) {
            GetCursorPos(&pt);
        } else {
            ClientToScreen(hwnd_, &pt);
        }
        const int id = ThemedPopupMenu::Show(hwnd_, pt, items);
        if (id != 0) AddActionByMenu(id);
    }

    void AddActionByMenu(int id) {
        size_t pos = actions_.size();
        int indent = -1;
        if (id == kAddFirst) pos = 0;
        if (id == kAddLast) {
            pos = actions_.size();
            indent = 0;
        }
        if (id == kAddBeforeSelected && selectedIndex_ >= 0) {
            pos = static_cast<size_t>(selectedIndex_);
            indent = actions_[static_cast<size_t>(selectedIndex_)].indent;
        }
        if (id == kAddAfterSelected && selectedIndex_ >= 0) {
            pos = static_cast<size_t>(SubtreeEnd(selectedIndex_));
            indent = actions_[static_cast<size_t>(selectedIndex_)].indent;
        }
        if (id == kAddAsChild && selectedIndex_ >= 0 && IsSubtreeContainer(actions_[static_cast<size_t>(selectedIndex_)].type)) {
            pos = static_cast<size_t>(ContainerBodyEndIndex(selectedIndex_));
            indent = actions_[static_cast<size_t>(selectedIndex_)].indent + 1;
        }
        TryInsertActionFromForm(pos, indent);
    }

    void CopyActionByMenu(int id) {
        if (copySource_ < 0 || copySource_ >= static_cast<int>(actions_.size())) return;
        ScriptAction action = actions_[static_cast<size_t>(copySource_)];
        size_t pos = actions_.size();
        if (id == kCopyFirst) pos = 0;
        if (id == kCopyBeforeSelected && selectedIndex_ >= 0) pos = static_cast<size_t>(selectedIndex_);
        if (id == kCopyAfterSelected && selectedIndex_ >= 0) pos = static_cast<size_t>(selectedIndex_ + 1);
        InsertAction(pos, action);
    }

    void ApplyCapturedKeyToForm(Hotkey& out, UINT& formVk, std::wstring& formText, HWND keyEdit,
                                HWND lWin, HWND rWin, HWND lCtrl, HWND rCtrl,
                                HWND lAlt, HWND rAlt, HWND lShift, HWND rShift) {
        LockWindowUpdate(hwnd_);
        formVk = out.vk;
        formText = VkName(out.vk);
        SetText(keyEdit, formText);
        SetChecked(lCtrl, (out.modifiers & MOD_CONTROL) != 0);
        SetChecked(lAlt, (out.modifiers & MOD_ALT) != 0);
        SetChecked(lShift, (out.modifiers & MOD_SHIFT) != 0);
        SetChecked(lWin, (out.modifiers & MOD_WIN) != 0);
        SetChecked(rCtrl, false);
        SetChecked(rAlt, false);
        SetChecked(rShift, false);
        SetChecked(rWin, false);
        LockWindowUpdate(nullptr);
    }

    void CaptureKeyPress() {
        BeginActionKeyCaptureRelease();
        HotkeyCapture cap;
        Hotkey oldValue{};
        oldValue.vk = formKeyPressVk_;
        oldValue.text = formKeyPressText_.empty() ? VkName(formKeyPressVk_) : formKeyPressText_;
        oldValue.enabled = oldValue.vk != 0;
        Hotkey out;
        const bool ok = cap.Show(hwnd_, oldValue, false, out);
        EndHotkeyCaptureRelease();
        if (!ok || !out.enabled || out.vk == 0) return;
        ApplyCapturedKeyToForm(out, formKeyPressVk_, formKeyPressText_, keyPressEdit_,
            keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_,
            keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_);
    }

    void CaptureActionKey() {
        BeginActionKeyCaptureRelease();
        HotkeyCapture cap;
        Hotkey oldValue{};
        oldValue.vk = formKeyVk_;
        oldValue.text = formKeyText_.empty() ? VkName(formKeyVk_) : formKeyText_;
        oldValue.enabled = oldValue.vk != 0;
        Hotkey out;
        const bool ok = cap.Show(hwnd_, oldValue, false, out);
        EndHotkeyCaptureRelease();
        if (!ok || !out.enabled || out.vk == 0) return;
        ApplyCapturedKeyToForm(out, formKeyVk_, formKeyText_, keyEdit_,
            keyLWin_, keyRWin_, keyLCtrl_, keyRCtrl_,
            keyLAlt_, keyRAlt_, keyLShift_, keyRShift_);
    }

    void ModifySelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(actions_.size())) return;
        ScriptAction action = ActionFromForm();
        if (action.type == ActionType::DefineBlock && !ValidateDefineBlockName(action.blockName, selectedIndex_)) return;
        if (action.type == ActionType::EndLoop
            && !HasLoopParentAt(actions_, static_cast<size_t>(selectedIndex_), actions_[static_cast<size_t>(selectedIndex_)].indent)) {
            ShowPromptInfo(kEndLoopNeedsLoopParentMsg);
            return;
        }
        action.originalNo = actions_[static_cast<size_t>(selectedIndex_)].originalNo;
        action.indent = actions_[static_cast<size_t>(selectedIndex_)].indent;
        actions_[static_cast<size_t>(selectedIndex_)] = action;
        if (action.type == ActionType::DefineBlock || action.type == ActionType::RunBlock) RefreshRunBlockCombo();
        OnActionsChanged();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    int NextNo() const { int maxNo = 0; for (const auto& a : actions_) maxNo = std::max(maxNo, a.originalNo); return maxNo + 1; }

    // ── Script file I/O ────────────────────────────────────────────
    void LoadScripts();
    void ClearScriptHotkeysCollidingWithGlobal();

    void LoadRecordings() {
        recordings_.clear();
        EnsureScriptsDir();
        std::vector<ScriptFileEntry> files;
        EnumerateScriptJsonFiles(RecordingsDir(), files);
        for (const auto& fe : files) {
            ScriptMeta meta{};
            meta.path = fe.path;
            meta.folder = fe.folder;
            const auto content = ReadAll(meta.path);
            meta.name = ExtractString(content, L"scriptName");
            if (meta.name.empty()) meta.name = fe.fileName;
            meta.name = StripJsonExtension(std::move(meta.name));
            if (meta.name.empty()) meta.name = L"未命名";
            meta.recordTime = ExtractString(content, L"recordTime");
            if (meta.recordTime.empty()) meta.recordTime = L"未知";
            meta.actionCount = CountActionsInJson(content);
            meta.durationSeconds = ExtractNumber(content, L"durationSeconds", 0);
            meta.hotkey.text = ExtractString(content, L"hotkeyText");
            meta.hotkey.vk = static_cast<UINT>(ExtractNumber(content, L"hotkeyVk", 0));
            meta.hotkey.modifiers = static_cast<UINT>(ExtractNumber(content, L"hotkeyModifiers", 0));
            meta.hotkey.holdMode = ExtractBool(content, L"hotkeyHold", false);
            meta.hotkey.enabled = meta.hotkey.vk != 0;
            recordings_.push_back(meta);
        }
    }

    void SaveIfEditor() {
        if (page_ != Page::Editor) return;
        EnsureEditorFullyParsed();
        SyncFormIntoActionsBeforeRun();
        EnsureScriptsDir();
        if (currentPath_.empty()) {
            std::wstring scriptName = GetText(name_);
            if (Trim(scriptName).empty()) scriptName = L"未命名脚本";
            currentPath_ = ScriptsDir() + L"\\" + scriptName + L".json";
        }
        SaveScriptFile(currentPath_);
    }

    void SaveScriptFile(const std::wstring& path) {
        ScriptFileData data{};
        data.scriptName = GetText(name_);
        // Web 壳 headless：无 name_ HWND，GetText 为空；从路径去 .json 回填（避免列表显示 xxx.json）
        if (Trim(data.scriptName).empty()) {
            const auto slash = path.find_last_of(L"\\/");
            std::wstring file = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
            data.scriptName = StripJsonExtension(std::move(file));
        } else {
            data.scriptName = StripJsonExtension(std::move(data.scriptName));
        }
        if (Trim(data.scriptName).empty()) {
            data.scriptName = IsRecordingScriptPath(path)
                ? (L"键鼠录制-" + TimestampName())
                : (L"鼠标宏-" + TimestampName());
        }
        data.recordTime = currentRecordTime_.empty() ? NowText() : currentRecordTime_;
        data.durationSeconds = saveDurationSeconds_;
        if (IsRecordingScriptPath(path)) {
            data.recordingCaptureMode = currentRecordingCaptureMode_;
            data.inputTimingVersion = currentInputTimingVersion_;
        }
        data.hotkey = saveHotkeyOverride_.has_value() ? *saveHotkeyOverride_
            : (currentScriptIndex_ >= 0 && currentScriptIndex_ < static_cast<int>(scripts_.size())
                ? scripts_[static_cast<size_t>(currentScriptIndex_)].hotkey : Hotkey{0, 0, L"", false});
        SyncScriptWindowModeFromEditor();
        data.windowMode = scriptWindowMode_;
        if (IsRecordingScriptPath(path)) {
            // 窗口模式录制：保存目标窗口身份 + 窗口相对坐标标记，回放跟随窗口。
            RecordingWindowTarget wmTgt = recordingWmTarget_.enabled
                ? recordingWmTarget_ : GetRecordingWindowTarget();
            bool anyRel = false;
            for (const auto& a : data.actions) {
                if (a.windowRelative) { anyRel = true; break; }
            }
            const bool keepWm = (recorderWindowMode_ || anyRel) && wmTgt.enabled;
            if (keepWm) {
                windowmode::WindowModeScriptConfig wm;
                wm.enabled = true;
                // 窗口相对录制默认「后台窗口模式」：目标留在用户当前桌面，
                // 回放跟随窗口且支持被其他窗口遮挡（不搬宏桌面，避免窗口“消失”）。
                wm.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
                wm.coordSpace = windowmode::WindowModeCoordinateSpace::WindowClient;
                wm.windowRelativeCoordinates = true;
                wm.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
                wm.targetExePath = wmTgt.exePath;
                wm.windowName = wmTgt.windowTitle;
                wm.windowClassName = wmTgt.windowClassName;
                wm.childWindowClassName = wmTgt.childWindowClassName;
                wm.autoLaunchTarget = !wmTgt.exePath.empty();
                wm.recordClientWidth = wmTgt.clientW;
                wm.recordClientHeight = wmTgt.clientH;
                if ((wm.recordClientWidth <= 0 || wm.recordClientHeight <= 0)
                    && wmTgt.hwnd && IsWindow(wmTgt.hwnd)) {
                    RECT rc{};
                    if (GetClientRect(wmTgt.hwnd, &rc)) {
                        wm.recordClientWidth = (std::max)(0, static_cast<int>(rc.right - rc.left));
                        wm.recordClientHeight = (std::max)(0, static_cast<int>(rc.bottom - rc.top));
                    }
                }
                data.windowMode = wm;
            } else if (anyRel) {
                windowmode::FinalizeWindowModeForPlayback(data.windowMode, true, true);
            } else {
                data.windowMode = windowmode::DefaultWindowModeConfig();
            }
            data.breakoutTimeSeconds = 0;
        } else {
            data.breakoutTimeSeconds = data.windowMode.enabled
                ? 0.0 : NormalizeBreakoutTimeSeconds(ParseBreakoutTimeFromEditor());
        }
        data.actions = actions_;
        data.coordsNormalized = false;

        CoordMeta pixelMeta = CaptureCurrentCoordMeta(
            data.windowMode.enabled ? &data.windowMode : nullptr);
        const CoordMeta storeMeta = BuildScriptCoordMetaForSave(pixelMeta);
        data.coordMeta = storeMeta;
        SyncNormFieldsFromPixels(data.actions, pixelMeta);

        if (!SaveScriptFileData(path, data)) {
            ShowPromptInfo(L"保存失败：无法写入文件，请检查磁盘空间和权限。");
            return;
        }
        newImagePaths_.clear();
        CleanOrphanImages();
        loadedCoordMeta_ = storeMeta;
        scriptWindowMode_ = data.windowMode;
    }

    ScriptAction ParseScriptActionBlock(const std::wstring& block, size_t fallbackNo,
        bool coordsNormalized = false) const {
        return ::ParseScriptActionBlock(block, fallbackNo, coordsNormalized);
    }

    std::vector<ScriptAction> ParseActionsFromContent(const std::wstring& content,
        bool coordsNormalized = false) const {
        std::vector<ScriptAction> parsed;
        const auto blocks = ExtractJsonActionBlocks(content);
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto type = ExtractString(blocks[i], L"type");
            if (!type.empty()) parsed.push_back(ParseScriptActionBlock(blocks[i], i, coordsNormalized));
        }
        return parsed;
    }

    std::vector<ScriptAction> ParseActionsFromFile(const std::wstring& path) const {
        if (path.empty()) return {};
        const auto content = ReadAll(path);
        const bool normalized = HasCoordMetaJson(content);
        std::vector<ScriptAction> actions = ParseActionsFromContent(content, normalized);
        if (normalized && !actions.empty()) {
            DenormalizeScriptToCurrentScreen(actions);
        }
        return actions;
    }

    void LoadScriptFile(const std::wstring& path) {
        ClearEditorProgressiveState();
        const ScriptFileData data = LoadScriptFileData(path, true);
        ApplyScriptFileDataToEditor(data);
    }

    void ClearEditorProgressiveState() {
        editorActionBlocks_.clear();
        editorActionParsed_.clear();
        editorParseCursor_ = 0;
        editorParsePending_ = false;
        editorLoadCoordsNormalized_ = false;
        editorLoadCoordMeta_ = {};
    }

    void ApplyScriptFileChrome(const std::wstring& content, const ScriptFileData* fullData) {
        if (fullData) {
            SetText(name_, fullData->scriptName);
            SetText(breakoutTimeEdit_, FormatBreakoutTimeForEditor(fullData->breakoutTimeSeconds));
            currentRecordTime_ = fullData->recordTime;
            currentRecordingCaptureMode_ = fullData->recordingCaptureMode;
            currentInputTimingVersion_ = fullData->inputTimingVersion;
            scriptWindowMode_ = fullData->windowMode;
            scriptWindowMode_.autoLaunchTarget =
                windowmode::ShouldAutoLaunchTarget(scriptWindowMode_);
            loadedCoordMeta_ = ScriptCoordMetaForExecution(fullData->coordMeta);
        } else {
            SetText(name_, ExtractString(content, L"scriptName"));
            SetText(breakoutTimeEdit_, FormatBreakoutTimeForEditor(
                NormalizeBreakoutTimeSeconds(ExtractNumber(content, L"breakoutTimeSeconds", 0))));
            currentRecordTime_ = ExtractString(content, L"recordTime");
            currentRecordingCaptureMode_ = static_cast<int>(
                ExtractNumber(content, L"recordingCaptureMode", -1));
            currentInputTimingVersion_ = std::max(0, static_cast<int>(
                ExtractNumber(content, L"inputTimingVersion", 0)));
            scriptWindowMode_ = windowmode::ParseWindowModeJson(content);
            scriptWindowMode_.autoLaunchTarget =
                windowmode::ShouldAutoLaunchTarget(scriptWindowMode_);
            if (HasCoordMetaJson(content)) {
                loadedCoordMeta_ = ScriptCoordMetaForExecution(ParseCoordMetaJson(content));
            } else {
                loadedCoordMeta_ = StandardScriptCoordMeta();
            }
        }
        popupMode_.sel = !scriptWindowMode_.enabled ? 0
            : (scriptWindowMode_.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow ? 2 : 1);
        SyncWindowModeUiFromScript();
    }

    void ApplyScriptFileDataToEditor(const ScriptFileData& data) {
        ApplyScriptFileChrome(L"", &data);
        actions_ = data.actions;
        if (!data.coordsNormalized && !actions_.empty()) {
            MigrateLegacyScriptToNormalized(actions_, StandardScriptCoordMeta());
        }
        NormalizeScriptActionList(actions_);
        collapsedContainers_.clear();
        MarkVisibleActionsDirty();
        RefreshRunBlockCombo();
        OnActionsChanged();
    }

    /// 打开编辑器：只解析首屏动作，其余分块继续（避免长脚本堵在揭开前）
    void LoadScriptFileProgressive(const std::wstring& path) {
        ClearEditorProgressiveState();
        const std::wstring content = ReadAll(path);
        ApplyScriptFileChrome(content, nullptr);

        editorLoadCoordsNormalized_ = HasCoordMetaJson(content);
        if (editorLoadCoordsNormalized_) {
            editorLoadCoordMeta_ = ParseCoordMetaJson(content);
            if (editorLoadCoordMeta_.refWidth <= 0 || editorLoadCoordMeta_.refHeight <= 0)
                editorLoadCoordMeta_ = StandardScriptCoordMeta();
        } else {
            editorLoadCoordMeta_ = StandardScriptCoordMeta();
        }

        editorActionBlocks_ = ExtractJsonActionBlocks(content);
        const int n = static_cast<int>(editorActionBlocks_.size());
        actions_.assign(static_cast<size_t>(n), ScriptAction{});
        editorActionParsed_.assign(static_cast<size_t>(n), 0);
        collapsedContainers_.clear();
        MarkVisibleActionsDirty();

        const int firstPage = std::max(VisibleActionRows() + 2, 24);
        EnsureEditorActionsParsed(0, firstPage);
        editorParseCursor_ = firstPage;
        editorParsePending_ = editorParseCursor_ < n;
        if (!editorParsePending_) {
            FinalizeEditorProgressiveParse();
        }
    }

    void EnsureEditorActionsParsed(int begin, int end) {
        if (actions_.empty()) return;
        begin = std::max(0, begin);
        end = std::min(end, static_cast<int>(actions_.size()));
        if (begin >= end) return;
        if (editorActionParsed_.size() != actions_.size()) {
            editorActionParsed_.assign(actions_.size(), 1);
            return;
        }
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenBounds(vsX, vsY, vsW, vsH);
        const CoordMeta denormMeta = StandardScriptCoordMeta();
        for (int i = begin; i < end; ++i) {
            if (editorActionParsed_[static_cast<size_t>(i)]) continue;
            if (i >= static_cast<int>(editorActionBlocks_.size())) {
                editorActionParsed_[static_cast<size_t>(i)] = 1;
                continue;
            }
            const auto& block = editorActionBlocks_[static_cast<size_t>(i)];
            if (ExtractString(block, L"type").empty()) {
                editorActionParsed_[static_cast<size_t>(i)] = 1;
                continue;
            }
            ScriptAction a = ParseScriptActionBlock(block, static_cast<size_t>(i), editorLoadCoordsNormalized_);
            if (!editorLoadCoordsNormalized_) {
                std::vector<ScriptAction> tmp{std::move(a)};
                MigrateLegacyScriptToNormalized(tmp, StandardScriptCoordMeta());
                a = std::move(tmp[0]);
            }
            DenormalizeActionCoords(a, denormMeta, vsW, vsH);
            actions_[static_cast<size_t>(i)] = std::move(a);
            editorActionParsed_[static_cast<size_t>(i)] = 1;
        }
    }

    void EnsureEditorFullyParsed() {
        if (!editorParsePending_ && editorActionBlocks_.empty()) return;
        EnsureEditorActionsParsed(0, static_cast<int>(actions_.size()));
        FinalizeEditorProgressiveParse();
    }

    void FinalizeEditorProgressiveParse() {
        editorActionBlocks_.clear();
        editorActionParsed_.assign(actions_.size(), 1);
        editorParsePending_ = false;
        editorParseCursor_ = static_cast<int>(actions_.size());
        NormalizeScriptActionList(actions_);
        MarkVisibleActionsDirty();
        if (page_ == Page::Editor) {
            RefreshRunBlockCombo();
            OnActionsChanged();
        }
    }

    void ContinueEditorProgressiveParse(int openGen) {
        if (openGen != editorOpenGeneration_ || page_ != Page::Editor) return;
        if (!editorParsePending_) return;
        constexpr int kChunk = 64;
        const int n = static_cast<int>(actions_.size());
        const int end = std::min(n, editorParseCursor_ + kChunk);
        EnsureEditorActionsParsed(editorParseCursor_, end);
        editorParseCursor_ = end;
        // 若用户已滚到未解析区，补一下当前视口
        if (page_ == Page::Editor) {
            const auto& vis = VisibleActionIndices();
            const int rows = VisibleActionRows();
            const int from = scrollOffset_;
            const int to = std::min(static_cast<int>(vis.size()), scrollOffset_ + rows + 1);
            for (int vi = from; vi < to; ++vi) {
                EnsureEditorActionsParsed(vis[static_cast<size_t>(vi)], vis[static_cast<size_t>(vi)] + 1);
            }
            RefreshActionListLayer();
        }
        if (editorParseCursor_ >= n) {
            FinalizeEditorProgressiveParse();
            if (page_ == Page::Editor) RefreshActionListLayer();
        } else {
            PostMessageW(hwnd_, WM_APP_EDITOR_PARSE_MORE, static_cast<WPARAM>(openGen), 0);
        }
    }

    std::wstring ChooseScriptPath(bool save, const std::wstring& defaultName = L"鼠标宏.json") {
        wchar_t fileName[MAX_PATH]{};
        wcsncpy_s(fileName, defaultName.c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"JSON 脚本 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        ofn.lpstrDefExt = L"json";
        const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
        return ok ? std::wstring(fileName) : L"";
    }

    std::wstring EnsureJsonExtension(std::wstring path) const {
        const auto slash = path.find_last_of(L"\\/");
        const auto dot = path.find_last_of(L'.');
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
            std::wstring ext = path.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            if (ext == L".json") return path;
            path.erase(dot);
        }
        return path + L".json";
    }

    std::wstring SafeScriptFileName(std::wstring name) const {
        if (Trim(name).empty()) name = TimestampName();
        for (wchar_t& ch : name) {
            if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
        }
        return name;
    }

    void ImportScript() {
        const bool toRecordings = ImportTargetsRecordings();
        wchar_t fileName[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"脚本文件 (*.zip;*.json)\0*.zip;*.json\0ZIP 脚本包 (*.zip)\0*.zip\0JSON 脚本 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;

        const std::wstring path(fileName);
        std::wstring lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });

        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == L".zip") {
            ImportScriptFromZipFile(path, toRecordings);
        } else {
            ImportScriptFromJsonFile(path, toRecordings);
        }
    }

    bool ImportTargetsRecordings() const {
        return activeHomeTab_ == quickscript::MainTab::Recorder;
    }

    std::wstring ImportTargetDir(bool toRecordings) const {
        return toRecordings ? RecordingsDir() : ScriptsDir();
    }

    bool WriteImportedJson(const std::wstring& target, std::wstring content);

    void FinishImport(bool toRecordings) {
        if (toRecordings) {
            LoadRecordings();
            ClampRecordingScroll();
        } else {
            LoadScripts();
            ClampHomeScroll();
            RegisterAllHotkeys();
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
        ShowPromptInfo(L"导入成功！");
    }

    void ImportScriptFromJsonFile(const std::wstring& path, bool toRecordings) {
        const auto content = ReadAll(path);
        ScriptFileData data = ParseScriptContent(content);
        if (data.scriptName.empty()) {
            ShowPromptInfo(L"导入失败：文件格式不正确，未找到脚本名称。");
            return;
        }
        if (toRecordings) {
            data.windowMode = windowmode::DefaultWindowModeConfig();
            data.breakoutTimeSeconds = 0;
        }
        data.recordTime = NowText();
        EnsureScriptsDir();
        const auto baseDir = ImportTargetDir(toRecordings);
        std::wstring target = baseDir + L"\\" + SafeScriptFileName(data.scriptName) + L".json";
        int suffix = 1;
        while (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            target = baseDir + L"\\" + SafeScriptFileName(data.scriptName)
                + L"-" + std::to_wstring(suffix++) + L".json";
        }
        if (!SaveScriptFileData(target, data)) {
            ShowPromptInfo(L"导入失败：无法写入目标目录。");
            return;
        }
        FinishImport(toRecordings);
    }

    void ImportScriptFromZipFile(const std::wstring& zipPath, bool toRecordings);

    void ExportSelectedScript() {
        if (selectedScript_ < 0 || selectedScript_ >= static_cast<int>(scripts_.size())) {
            ShowPromptInfo(L"请选择要导出的宏。");
            return;
        }
        const auto& script = scripts_[static_cast<size_t>(selectedScript_)];
        const auto content = ReadAll(script.path);
        const auto imgPaths = CollectImagePathsFromJson(content);

        // 根据是否包含图片选择导出格式
        if (!imgPaths.empty()) {
            ExportScriptAsZip(script);
        } else {
            ExportScriptAsJson(script);
        }
    }

    void ExportScriptAsJson(const ScriptMeta& script) {
        const auto chosenPath = ChooseScriptPath(true, SafeScriptFileName(script.name) + L".json");
        if (chosenPath.empty()) return;
        const auto path = EnsureJsonExtension(chosenPath);
        if (!CopyFileW(script.path.c_str(), path.c_str(), FALSE)) {
            ShowPromptInfo(L"导出失败：无法写入目标文件。");
        } else {
            ShowPromptInfo(L"导出成功！");
        }
    }

    void ExportScriptAsZip(const ScriptMeta& script) {
        wchar_t fileName[MAX_PATH]{};
        const auto defaultName = SafeScriptFileName(script.name) + L".zip";
        wcsncpy_s(fileName, defaultName.c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"ZIP 脚本包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&ofn)) return;

        // 确保扩展名为 .zip
        std::wstring zipPath(fileName);
        if (zipPath.size() < 4 || _wcsicmp(zipPath.substr(zipPath.size() - 4).c_str(), L".zip") != 0) {
            zipPath += L".zip";
        }

        // 收集文件列表：JSON + 所有引用的图片
        const auto content = ReadAll(script.path);
        const auto imgPaths = CollectImagePathsFromJson(content);
        std::vector<std::pair<std::wstring, std::wstring>> files;
        // JSON 文件在 ZIP 中固定命名为 script.json
        files.push_back({L"script.json", script.path});
        for (const auto& imgPath : imgPaths) {
            // 图片在 ZIP 中用原始文件名
            const auto slashPos = imgPath.find_last_of(L"\\/");
            std::wstring imgName = (slashPos == std::wstring::npos) ? imgPath : imgPath.substr(slashPos + 1);
            files.push_back({imgName, imgPath});
        }
        const auto zipResult = CreateZipFile(zipPath, files, script.path);
        if (zipResult.success) {
            if (zipResult.skippedFiles.empty()) {
                ShowPromptInfo(L"导出成功！图片已一同打包。");
            } else {
                const std::wstring msg = L"导出成功，但有 " + std::to_wstring(zipResult.skippedFiles.size())
                    + L" 张图片未找到已跳过。\n\n对方导入后需要重新截图或选择本地图片。";
                ShowPromptInfo(msg.c_str());
            }
        } else {
            ShowPromptInfo(L"导出失败：无法创建 ZIP 文件，请检查保存路径是否有写入权限。");
        }
    }

    RECT ClampToWorkArea(RECT rc) const {
        HMONITOR monitor = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) return rc;
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        if (rc.left < info.rcWork.left) { rc.left = info.rcWork.left; rc.right = rc.left + width; }
        if (rc.top < info.rcWork.top) { rc.top = info.rcWork.top; rc.bottom = rc.top + height; }
        if (rc.right > info.rcWork.right) { rc.right = info.rcWork.right; rc.left = rc.right - width; }
        if (rc.bottom > info.rcWork.bottom) { rc.bottom = info.rcWork.bottom; rc.top = rc.bottom - height; }
        return rc;
    }

    RECT EditorRectFromHome(RECT homeRc) const {
        const int homeCenterX = homeRc.left + (homeRc.right - homeRc.left) / 2;
        const int homeCenterY = homeRc.top + (homeRc.bottom - homeRc.top) / 2;
        RECT editorRc{
            homeCenterX - UiEditorWidth() / 2,
            homeCenterY - UiEditorHeight() / 2,
            homeCenterX + UiEditorWidth() / 2,
            homeCenterY + UiEditorHeight() / 2
        };
        return ClampToWorkArea(editorRc);
    }

    // ── Layout helpers / RECT getters ──────────────────────────────
    RECT HomeListRect() const { return UiRect4(kHomeCardX, kHomeListY, kHomeCardX + kHomeCardW, kHomeListBottom); }
    int HomeListHeight() const { RECT r = HomeListRect(); return r.bottom - r.top; }
    int HomeContentHeight() const { return static_cast<int>(scripts_.size()) * UiLen(kHomeCardStep) - UiLen(kHomeCardGap); }
    int MaxHomeScroll() const { return std::max(0, HomeContentHeight() - HomeListHeight()); }
    int AgentConvContentHeight() const { return static_cast<int>(agentConversations_.size()) * UiLen(kHomeCardStep) - UiLen(kHomeCardGap); }
    int MaxAgentConvScroll() const { return std::max(0, AgentConvContentHeight() - HomeListHeight()); }
    int ActiveHomeContentHeight() const {
        if (activeHomeTab_ == quickscript::MainTab::ScriptCustom)
            return std::max(AgentConvContentHeight(), 1);
        if (activeHomeTab_ == quickscript::MainTab::Recorder)
            return std::max(RecordingContentHeight(), 1);
        return std::max(HomeContentHeight(), 1);
    }
    int ActiveHomeListMaxScroll() const {
        if (activeHomeTab_ == quickscript::MainTab::ScriptCustom) return MaxAgentConvScroll();
        if (activeHomeTab_ == quickscript::MainTab::Recorder) return MaxRecordingScroll();
        return MaxHomeScroll();
    }
    void ClampHomeScroll() { homeScrollOffset_ = std::clamp(homeScrollOffset_, 0, MaxHomeScroll()); }
    void ClampAgentConvScroll() { homeScrollOffset_ = std::clamp(homeScrollOffset_, 0, MaxAgentConvScroll()); }
    RECT HomeCardRect(int i) const {
        const int cardX = UiLen(kHomeCardX);
        const int cardW = UiLen(kHomeCardW);
        const int cardH = UiLen(kHomeCardH);
        const int y = UiLen(kHomeListY) + i * UiLen(kHomeCardStep) - homeScrollOffset_;
        return RECT{cardX, y, cardX + cardW, y + cardH};
    }
    int RecordingContentHeight() const { return static_cast<int>(recordings_.size()) * UiLen(kHomeCardStep) - UiLen(kHomeCardGap); }
    int MaxRecordingScroll() const { return std::max(0, RecordingContentHeight() - HomeListHeight()); }
    void ClampRecordingScroll() { homeScrollOffset_ = std::clamp(homeScrollOffset_, 0, MaxRecordingScroll()); }
    RECT RecordingCardRect(int i) const { return HomeCardRect(i); }
    std::wstring RecordingHotkeyText(const ScriptMeta& rec) const {
        if (!rec.hotkey.enabled || rec.hotkey.text.empty()) return L"设置热键";
        return rec.hotkey.holdMode ? (rec.hotkey.text + L"（长按）") : rec.hotkey.text;
    }
    RECT RecordingHotkeyRect(int i) const {
        if (i < 0 || i >= static_cast<int>(recordings_.size())) return RECT{};
        RECT r = RecordingCardRect(i);
        const int left = r.left + UiLen(14);
        const int rightLimit = r.right - UiLen(96);
        const int hotW = TextWidth(RecordingHotkeyText(recordings_[static_cast<size_t>(i)]), hotFont_);
        const int nameW = TextWidth(recordings_[static_cast<size_t>(i)].name, homeFont_);
        const int hotLeft = std::min(left + nameW + UiLen(10), rightLimit - hotW);
        return RECT{hotLeft, r.top + UiLen(17), std::min(hotLeft + hotW, rightLimit), r.top + UiLen(48)};
    }
    RECT RecorderBannerKeyRect() const {
        // 与 PaintRecorderHome 黄条居中布局保持一致（命中热区跟着文案走）
        const RECT cr = CreateRect();
        const bool hold = GlobalHotkeyUsesHold();
        const std::wstring prefix = hold ? L"按住" : L"按";
        std::wstring suffix;
        if (recording_) suffix = L"键停止 录制";
        else if (selectedRecording_ >= 0) suffix = L"键开始 回放";
        else suffix = L"键开始 录制";
        const std::wstring hotText = globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text;
        const int hotW = std::max(UiLen(56), TextWidth(hotText, bigFont_) + UiLen(20));
        const int gap = UiLen(20);
        const int prefixW = TextWidth(prefix, bigFont_);
        const int suffixW = TextWidth(suffix, bigFont_);
        const int totalW = prefixW + gap + hotW + gap + suffixW;
        const int left = cr.left + (cr.right - cr.left - totalW) / 2 + prefixW + gap;
        return RECT{left, cr.top + UiLen(21), left + hotW, cr.bottom - UiLen(21)};
    }
    RECT HomeScrollTrackRect() const { RECT list = HomeListRect(); return RECT{list.right + UiLen(5), list.top, list.right + UiLen(5) + UiLen(kHomeScrollW), list.bottom}; }
    RECT HomeScrollThumbRect() const {
        RECT track = HomeScrollTrackRect();
        const int maxScroll = ActiveHomeListMaxScroll();
        if (maxScroll <= 0) return RECT{track.left, track.top, track.right, track.bottom};
        const int trackH = track.bottom - track.top;
        const int contentH = ActiveHomeContentHeight();
        const int thumbH = std::max(UiLen(46), trackH * HomeListHeight() / contentH);
        const int range = std::max(1, trackH - thumbH);
        const int top = track.top + range * homeScrollOffset_ / maxScroll;
        return RECT{track.left, top, track.right, top + thumbH};
    }
    RECT CreateRect() const { return UiRect4(35, 375, 683, 459); }
    RECT CreateWordRect() const {
        RECT cr = CreateRect();
        const int gap = UiLen(12);
        const int padV = UiLen(21);
        const int createBtnW = UiLen(78);
        const int wClick = TextWidth(L"点击", bigFont_);
        const int wSuffix = TextWidth(L"鼠标宏", bigFont_);
        const int total = wClick + gap + createBtnW + gap + wSuffix;
        int x = cr.left + (cr.right - cr.left - total) / 2 + wClick + gap;
        return RECT{x, cr.top + padV, x + createBtnW, cr.bottom - padV};
    }
    std::wstring ScriptHotkeyText(const ScriptMeta& script) const {
        if (!script.hotkey.enabled || script.hotkey.text.empty()) return L"设置热键";
        return script.hotkey.holdMode ? (script.hotkey.text + L"（长按）") : script.hotkey.text;
    }
    RECT ScriptHotkeyRect(int i) const {
        if (i < 0 || i >= static_cast<int>(scripts_.size())) return RECT{};
        RECT r = HomeCardRect(i);
        const int left = r.left + UiLen(14);
        const int rightLimit = r.right - UiLen(96);
        const int hotW = TextWidth(ScriptHotkeyText(scripts_[static_cast<size_t>(i)]), hotFont_);
        const int nameW = TextWidth(scripts_[static_cast<size_t>(i)].name, homeFont_);
        const int hotLeft = std::min(left + nameW + UiLen(10), rightLimit - hotW);
        return RECT{hotLeft, r.top + UiLen(17), std::min(hotLeft + hotW, rightLimit), r.top + UiLen(48)};
    }
    RECT ImportRect() const { return UiRect4(60, 149, 166, 182); }
    RECT ExportRect() const { return UiRect4(180, 149, 286, 182); }
    RECT TimerRect() const { return UiRect4(300, 149, 406, 182); }
    RECT RecorderModeRect() const {
        // 贴齐「按 XX 键开始录制」黄条右上角，不留缝
        const RECT cr = CreateRect();
        const std::wstring label = RecorderInputModeLabel(recorderSettings_.inputMode);
        const int btnW = std::max(UiLen(88), TextWidth(label, homeFont_) + UiLen(24));
        const int btnH = UiLen(28);
        return RECT{
            cr.right - btnW,
            cr.top,
            cr.right,
            cr.top + btnH};
    }

    static std::wstring RecorderInputModeLabel(quickscript::RecorderInputMode mode) {
        switch (mode) {
        case quickscript::RecorderInputMode::DesktopAbsolute: return L"绝对坐标";
        case quickscript::RecorderInputMode::FpsRelative: return L"相对坐标";
        case quickscript::RecorderInputMode::ImageLocate: return L"图片定位";
        case quickscript::RecorderInputMode::Auto:
        default: return L"自动识别";
        }
    }

    void CycleRecorderInputMode() {
        // 自动识别 → 相对坐标 → 绝对坐标 → 图片定位 → …
        switch (recorderSettings_.inputMode) {
        case quickscript::RecorderInputMode::Auto:
            recorderSettings_.inputMode = quickscript::RecorderInputMode::FpsRelative;
            break;
        case quickscript::RecorderInputMode::FpsRelative:
            recorderSettings_.inputMode = quickscript::RecorderInputMode::DesktopAbsolute;
            break;
        case quickscript::RecorderInputMode::DesktopAbsolute:
            recorderSettings_.inputMode = quickscript::RecorderInputMode::ImageLocate;
            break;
        case quickscript::RecorderInputMode::ImageLocate:
        default:
            recorderSettings_.inputMode = quickscript::RecorderInputMode::Auto;
            break;
        }
    }
    RECT HelpRect() const { return UiRect4(576, 149, 686, 182); }
    RECT HomeFooterRect() const { return UiRect4(36, 468, 700, 492); }
    RECT ScriptCustomHeaderRect() const { return UiRect4(36, 125, 684, 182); }
    RECT AgentConvChatRect(int i) const { return UiEdgeRect(HomeCardRect(i), 96, 14, 24, 45); }
    RECT AgentConvDeleteRect(int i) const { return UiEdgeRect(HomeCardRect(i), 96, 58, 24, 88); }

    void LoadAgentConversations() {
        agentConversations_.clear();
        LoadAgentConversationList(agentConversations_);
        ClampAgentConvScroll();
    }

    void OnAgentConversationClosed(AgentConversationSavePayload&& payload) {
        if (payload.shouldSave) SaveAgentConversation(payload);
        LoadAgentConversations();
        if (IsWindow(hwnd_)) InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void ConfirmDeleteAgentConversation(int index) {
        if (index < 0 || index >= static_cast<int>(agentConversations_.size())) return;
        pendingDeleteAgentConv_ = index;
        const std::wstring name = agentConversations_[static_cast<size_t>(index)].name;
        promptModal_.ShowConfirm(L"您确定要删除对话 \"" + name + L"\"\n吗？", [this](bool ok) {
            if (ok && pendingDeleteAgentConv_ >= 0
                && pendingDeleteAgentConv_ < static_cast<int>(agentConversations_.size())) {
                DeleteAgentConversation(agentConversations_[static_cast<size_t>(pendingDeleteAgentConv_)].id);
                LoadAgentConversations();
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            pendingDeleteAgentConv_ = -1;
            StDiscardSpuriousInputAfterModal(hwnd_);
        });
    }

    void QueuePromptInfo(std::wstring message) {
        promptPendingMessage_ = std::move(message);
        PostMessageW(hwnd_, WM_APP_PROMPT, 0, 0);
    }

    void ShowPromptInfo(const std::wstring& message) {
        if (headlessUi_) {
            NotifyHeadlessUserMessage(message);
            return;
        }
        CloseEditorPopup();
        CloseClickerDropPopup();
        promptModal_.ShowInfo(message);
    }

    /// headless：推 Web toast；非 WebView 构建时 MessageBox 兜底
    void NotifyHeadlessUserMessage(const std::wstring& message) {
#if defined(QST_WEBVIEW_WITH_ENGINE) && QST_WEBVIEW_WITH_ENGINE
        std::string utf8 = ToUtf8(message);
        std::string esc;
        esc.reserve(utf8.size() + 8);
        for (unsigned char c : utf8) {
            if (c == '"' || c == '\\') { esc.push_back('\\'); esc.push_back(static_cast<char>(c)); }
            else if (c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); esc += buf; }
            else esc.push_back(static_cast<char>(c));
        }
        qst::webview::PostToWebUi(
            std::string("{\"type\":\"engine.toast\",\"ok\":true,\"text\":\"") + esc + "\"}");
        return;
#else
        HWND owner = (webViewHostHwnd_ && IsWindow(webViewHostHwnd_)) ? webViewHostHwnd_ : hwnd_;
        if (owner) MessageBoxW(owner, message.c_str(), L"键鼠工坊", MB_OK | MB_ICONINFORMATION);
#endif
    }
    int TextWidth(const std::wstring& text, HFONT font) const {
        if (!hwnd_ || !font) return static_cast<int>(text.size()) * 18;
        HDC hdc = GetDC(hwnd_);
        HGDIOBJ oldFont = SelectObject(hdc, font);
        SIZE size{};
        GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
        SelectObject(hdc, oldFont);
        ReleaseDC(hwnd_, hdc);
        return size.cx;
    }

    RECT CommonHotRect() const {
        const RECT cr = CreateRect();
        const bool hold = GlobalHotkeyUsesHold();
        const std::wstring prefix = hold ? L"按住" : L"按";
        const std::wstring suffix = L"键开始 运行宏";
        const int hotW = std::max(UiLen(80), TextWidth(globalHotkey_.text, bigFont_) + UiLen(28));
        const int prefixW = TextWidth(prefix, bigFont_);
        const int suffixW = TextWidth(suffix, bigFont_);
        const int gap = UiLen(20);
        const int totalW = prefixW + gap + hotW + gap + suffixW;
        const int left = cr.left + (cr.right - cr.left - totalW) / 2 + prefixW + gap;
        return RECT{left, cr.top + UiLen(27), left + hotW, cr.bottom - UiLen(20)};
    }
    RECT RunHintRect() const { return CreateRect(); }
    RECT CloseRect() const { RECT rc{}; GetClientRect(hwnd_, &rc); return RECT{rc.right - UiLen(kCloseBtnW), 0, rc.right, UiLen(kTitleH)}; }
    RECT MinimizeRect() const { RECT close = CloseRect(); return RECT{close.left - UiLen(kTitleBtnW), 0, close.left, UiLen(kTitleH)}; }
    RECT SettingsRect() const { RECT min = MinimizeRect(); return RECT{min.left - UiLen(kTitleBtnW), 0, min.left, UiLen(kTitleH)}; }
    RECT LoadButtonRect() const { return WindowClientRect(loadBtn_); }
    RECT ClearButtonRect() const { return WindowClientRect(clearBtn_); }
    RECT BatchExitButtonRect() const { return WindowClientRect(batchExitBtn_); }
    RECT BatchSelectAllButtonRect() const { return WindowClientRect(batchSelectAllBtn_); }
    RECT BatchDeselectButtonRect() const { return WindowClientRect(batchDeselectBtn_); }
    RECT BatchDeleteButtonRect() const { return WindowClientRect(batchDeleteBtn_); }
    RECT BatchCopyButtonRect() const { return WindowClientRect(batchCopyBtn_); }
    RECT AddButtonRect() const { return WindowClientRect(addBtn_); }
    RECT ModifyButtonRect() const { return WindowClientRect(modifyBtn_); }
    RECT CancelButtonRect() const { return WindowClientRect(cancelBtn_); }
    RECT SaveButtonRect() const { return WindowClientRect(saveBtn_); }
    RECT CrosshairButtonRect() const { return WindowClientRect(crosshairBtn_); }
    int ActionListCheckboxColumnRight() const {
        return UiLen(kListX) + UiLen(kListInnerPad) + UiLen(kColRemarkInList) - UiLen(4);
    }
    RECT CheckboxRect(int index) const {
        RECT list = ActionListRect();
        const int slot = VisibleSlotOf(index);
        if (slot < 0) return RECT{};
        const int rowH = std::max(1, UiLen(kRowH));
        const int localY = UiLen(kListInnerPad) + (slot - scrollOffset_) * rowH;
        RECT local = LocalCheckboxRect(index, localY);
        OffsetRect(&local, list.left, list.top);
        return local;
    }
    bool HitClose(int x, int y) const { return PtIn(CloseRect(), x, y); }
    bool HitMinimize(int x, int y) const { return PtIn(MinimizeRect(), x, y); }
    bool HitSettings(int x, int y) const { return PtIn(SettingsRect(), x, y); }
    RECT ActionListRect() const {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int bodyBottom = static_cast<int>(rc.bottom) - (page_ == Page::Editor ? UiLen(kBottomH) : 0);
        const int h = std::max(0, std::min(UiLen(kListH), bodyBottom - UiLen(kListY)));
        return RECT{UiLen(kListX), UiLen(kListY), UiLen(kListX) + UiLen(kListW), UiLen(kListY) + h};
    }
    int VisibleActionRows() const {
        const RECT list = ActionListRect();
        const int rowH = std::max(1, UiLen(kRowH));
        return std::max(0, static_cast<int>((list.bottom - list.top) / rowH));
    }
    int VisibleActionCount() const {
        return static_cast<int>(VisibleActionIndices().size());
    }
    void MarkVisibleActionsDirty() {
        visibleActionIndexCacheDirty_ = true;
    }
    const std::vector<int>& VisibleActionIndices() const {
        // 删/插动作后若忘记 MarkVisibleActionsDirty，旧缓存会越界访问 actions_ → 卡死/崩溃
        if (!visibleActionIndexCacheDirty_) {
            for (int idx : visibleActionIndexCache_) {
                if (idx < 0 || idx >= static_cast<int>(actions_.size())) {
                    visibleActionIndexCacheDirty_ = true;
                    break;
                }
            }
        }
        if (visibleActionIndexCacheDirty_) {
            visibleActionIndexCache_.clear();
            visibleActionIndexCache_.reserve(actions_.size());
            for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
                if (IsRowVisible(i)) visibleActionIndexCache_.push_back(i);
            }
            visibleActionIndexCacheDirty_ = false;
        }
        return visibleActionIndexCache_;
    }
    int VisibleSlotOf(int index) const {
        int slot = 0;
        for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
            if (!IsRowVisible(i)) continue;
            if (i == index) return slot;
            ++slot;
        }
        return -1;
    }
    int ContainerBodyEndIndex(int containerIndex) const { return ContainerBodyEnd(actions_, containerIndex); }
    int LoopBodyEnd(int loopIndex) const { return ContainerBodyEndIndex(loopIndex); }
    int SubtreeEnd(int index) const { return ::SubtreeEnd(actions_, index); }
    bool IsContainerExpanded(int index) const { return ::IsContainerExpanded(collapsedContainers_, index); }
    bool IsLoopExpanded(int index) const { return IsContainerExpanded(index); }
    bool IsRowVisible(int index) const { return IsRowVisibleInTree(actions_, collapsedContainers_, index); }
    int ActionLeftClient(int indent) const { return kListX + kListInnerPad + kColActionInList + indent * kIndentStep; }
    int ActionContentLeftLocal(int indent, int pad = kListInnerPad) const { return pad + kColActionInList + indent * kIndentStep; }
    int BatchCheckboxLeftLocal(int indent, ActionType type, int pad = kListInnerPad) const {
        const int contentLeft = ActionContentLeftLocal(indent, pad);
        return IsExpandableContainer(type) ? contentLeft + kExpandToggleSlot : contentLeft;
    }
    int ActionTextLeftLocal(int indent, ActionType type, bool batchMode, int pad = kListInnerPad) const {
        const int contentLeft = ActionContentLeftLocal(indent, pad);
        if (!batchMode) return contentLeft;
        if (IsExpandableContainer(type)) return contentLeft + kExpandToggleSlot + kBatchCheckboxSize + kBatchItemGap;
        return contentLeft + kBatchCheckboxSize + kBatchItemGap;
    }
    int ExpandToggleLeftLocal(int indent, bool batchMode, int pad = kListInnerPad) const {
        const int contentLeft = ActionContentLeftLocal(indent, pad);
        return batchMode ? contentLeft : contentLeft - kExpandToggleSlot;
    }
    int IndentFromX(int x) const { return std::max(0, (x - ActionLeftClient(0)) / kIndentStep); }
    struct DragInsertTarget { int insertIndex = 0; int targetIndent = 0; bool nested = false; };
    int ActionListContentRight() const {
        return UiLen(kListX) + UiLen(kListW)
            - (MaxEditorScroll() > 0 ? UiLen(kEditorScrollW) + UiLen(6) : 0);
    }
    int MaxEditorScroll() const { return std::max(0, VisibleActionCount() - VisibleActionRows()); }
    RECT EditorScrollTrackRect() const {
        RECT list = ActionListRect();
        return RECT{list.right - kEditorScrollW - 4, list.top + 2, list.right - 4, list.bottom - 2};
    }
    RECT EditorScrollThumbRect() const {
        RECT track = EditorScrollTrackRect();
        const int maxScroll = MaxEditorScroll();
        if (maxScroll <= 0) return track;
        const int trackH = track.bottom - track.top;
        const int contentH = std::max(VisibleActionCount() * std::max(1, UiLen(kRowH)), 1);
        RECT list = ActionListRect();
        const int visibleH = std::max(1, static_cast<int>(list.bottom - list.top));
        const int thumbH = std::max(32, trackH * visibleH / contentH);
        const int range = std::max(1, trackH - thumbH);
        const int top = track.top + range * scrollOffset_ / maxScroll;
        return RECT{track.left, top, track.right, top + thumbH};
    }
    void UpdateEditorScrollFromThumb(int thumbTop) {
        RECT track = EditorScrollTrackRect();
        RECT thumb = EditorScrollThumbRect();
        const int maxScroll = MaxEditorScroll();
        const int trackHeight = static_cast<int>(track.bottom - track.top);
        const int thumbHeight = static_cast<int>(thumb.bottom - thumb.top);
        const int range = std::max(1, trackHeight - thumbHeight);
        const int thumbOffset = thumbTop - static_cast<int>(track.top);
        scrollOffset_ = std::clamp(thumbOffset * maxScroll / range, 0, maxScroll);
    }

    RECT ClickerTabRect() const { const int tabW = UiHomeWidth() / 4; return RECT{0, UiLen(kTitleH), tabW, UiLen(kHomeContentTop)}; }
    RECT RecorderTabRect() const { const int tabW = UiHomeWidth() / 4; return RECT{tabW, UiLen(kTitleH), tabW * 2, UiLen(kHomeContentTop)}; }
    RECT MacroTabRect() const { const int tabW = UiHomeWidth() / 4; return RECT{tabW * 2, UiLen(kTitleH), tabW * 3, UiLen(kHomeContentTop)}; }
    RECT ScriptCustomTabRect() const { const int tabW = UiHomeWidth() / 4; return RECT{tabW * 3, UiLen(kTitleH), tabW * 4, UiLen(kHomeContentTop)}; }

    static constexpr int kClickerLabelX = 61;
    static constexpr int kClickerFieldGap = 8;
    static constexpr int kClickerComboRight = 575;
    static constexpr int kClickerRadioSize = 20;

    struct ClickerHomeLayout {
        int leftRadioLeft = 167;
        int middleRadioLeft = 303;
        int rightRadioLeft = 437;
        int intervalComboLeft = 245;
        int hotkeyComboLeft = 245;
    };
    ClickerHomeLayout clickerLayout_{};

    void UpdateClickerLayout() {
        static constexpr int kRadioTextGap = 9;
        static constexpr int kOptionGap = 17;
        int x = UiLen(kClickerLabelX) + TextWidth(L"点击类型:", homeFont_) + UiLen(kClickerFieldGap);
        clickerLayout_.leftRadioLeft = x;
        x += UiLen(kClickerRadioSize) + UiLen(kRadioTextGap) + TextWidth(L"鼠标左键", homeFont_) + UiLen(kOptionGap);
        clickerLayout_.middleRadioLeft = x;
        x += UiLen(kClickerRadioSize) + UiLen(kRadioTextGap) + TextWidth(L"鼠标中键", homeFont_) + UiLen(kOptionGap);
        clickerLayout_.rightRadioLeft = x;
        clickerLayout_.intervalComboLeft = UiLen(kClickerLabelX) + TextWidth(L"每次点击间隔时间:", homeFont_) + UiLen(kClickerFieldGap);
        clickerLayout_.hotkeyComboLeft = UiLen(kClickerLabelX) + TextWidth(L"启停的全局热键:", homeFont_) + UiLen(kClickerFieldGap);
    }

    RECT ClickerLeftRadioRect() const {
        return RECT{clickerLayout_.leftRadioLeft, UiLen(171), clickerLayout_.leftRadioLeft + UiLen(kClickerRadioSize), UiLen(191)};
    }
    RECT ClickerMiddleRadioRect() const {
        return RECT{clickerLayout_.middleRadioLeft, UiLen(171), clickerLayout_.middleRadioLeft + UiLen(kClickerRadioSize), UiLen(191)};
    }
    RECT ClickerRightRadioRect() const {
        return RECT{clickerLayout_.rightRadioLeft, UiLen(171), clickerLayout_.rightRadioLeft + UiLen(kClickerRadioSize), UiLen(191)};
    }
    RECT ClickerIntervalRect() const {
        return RECT{clickerLayout_.intervalComboLeft, UiLen(236), UiLen(kClickerComboRight), UiLen(266)};
    }
    RECT ClickerHotkeyRect() const {
        return RECT{clickerLayout_.hotkeyComboLeft, UiLen(303), UiLen(kClickerComboRight), UiLen(333)};
    }
    static constexpr int kClickerDropdownItemH = 68;
    static constexpr int kClickerHotkeyMenuCount = 7;
    bool ClickerIntervalPopupOpen() const { return clickerDropPopupKind_ == 0; }
    bool ClickerHotkeyPopupOpen() const { return clickerDropPopupKind_ == 1; }
    int ClickerPopupItemCount() const {
        if (clickerDropPopupKind_ == 0) return 3;
        if (clickerDropPopupKind_ == 1) return kClickerHotkeyMenuCount;
        return 0;
    }
    int ClickerPopupVisibleCount() const {
        if (clickerPopupVisibleCount_ > 0) return clickerPopupVisibleCount_;
        return ClickerPopupItemCount();
    }
    RECT ClickerBannerKeyRect() const {
        RECT cr = CreateRect();
        const bool hold = GlobalHotkeyUsesHold();
        const std::wstring prefix = hold ? L"按住" : L"按";
        const std::wstring hotText = globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text;
        const std::wstring suffix = clicking_
            ? (L"键停止 连点（当前 " + ClickerButtonTitle() + L" " + ClickIntervalComboText() + L"）")
            : (L"键开始 " + ClickerButtonTitle() + L" 连点");
        const int hotW = std::max(UiLen(56), TextWidth(hotText, bigFont_) + UiLen(20));
        const int gap = UiLen(10);
        const int prefixW = TextWidth(prefix, bigFont_);
        const int totalW = prefixW + gap + hotW + gap + TextWidth(suffix, bigFont_);
        const int centered = (cr.right - cr.left - totalW) / 2;
        const int pad = centered > UiLen(20) ? centered : UiLen(20);
        const int left = cr.left + pad + prefixW + gap;
        return RECT{left, cr.top + UiLen(21), left + hotW, cr.bottom - UiLen(21)};
    }

    bool GlobalHotkeyUsesHold() const {
        return globalHotkey_.holdMode || NeedsMouseHoldHotkey(globalHotkey_.vk);
    }
    // 主界面卡片右侧操作列：宏/录制共用；右缘留白 16
    // 「取消选择」「已选中✓」需同宽且左右对齐；列宽取够「取消选择」（88，原 72 会裁字）
    // rightPad=16，列宽=88 → rightStart=104
    RECT HomeCardEditBtn(const RECT& r) const { return UiEdgeRect(r, 104, 14, 16, 45); }
    RECT HomeCardDeleteBtn(const RECT& r) const { return UiEdgeRect(r, 104, 56, 16, 88); }
    RECT HomeCardSelectedTag(const RECT& r) const { return UiEdgeRect(r, 104, 58, 16, 95); }
    RECT HomeCardNameRect(const RECT& r) const { return UiInsetRect(r, 14, 17, 112, 48); }
    RECT HomeCardMetaLeft(const RECT& r) const {
        return RECT{r.left + UiLen(14), r.top + UiLen(58), r.left + UiLen(350), r.top + UiLen(88)};
    }
    RECT HomeCardMetaRight(const RECT& r) const {
        return RECT{r.left + UiLen(344), r.top + UiLen(58), r.left + UiLen(510), r.top + UiLen(88)};
    }
    // 「重命名/删除」与宏「编辑/删除」同列；「优化」同行等高，紧挨其左（右对齐贴齐）
    RECT RecordingOptimizeRect(int i) const { return UiEdgeRect(RecordingCardRect(i), 164, 14, 116, 45); }
    RECT RecordingRenameRect(int i) const { return UiEdgeRect(RecordingCardRect(i), 104, 14, 16, 45); }
    RECT RecordingDeleteRect(int i) const { return UiEdgeRect(RecordingCardRect(i), 104, 56, 16, 88); }
    RECT RecordingDeselectRect(int i) const { return UiEdgeRect(RecordingCardRect(i), 104, 14, 16, 45); }
    RECT RecordingSelectedTagRect(int i) const { return UiEdgeRect(RecordingCardRect(i), 104, 58, 16, 95); }
    RECT RecorderHotkeyRect() const { return UiRect4(267, 397, 359, 438); }
    RECT RecorderScopeRect() const { return UiRect4(556, 369, 692, 412); }

    static LRESULT CALLBACK EditorChildSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR refData) {
        auto* self = reinterpret_cast<EngineHost*>(refData);
        if (self && hwnd == self->paramViewport_) {
            switch (msg) {
            case WM_COMMAND:
                self->OnCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
                return 0;
            case WM_DRAWITEM:
                self->DrawOwnerItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
                return TRUE;
            case WM_MEASUREITEM:
                self->MeasureOwnerItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp));
                return TRUE;
            case WM_CTLCOLORSTATIC:
                return self->OnCtlColorStatic(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
            case WM_CTLCOLOREDIT:
                return self->OnEditColor(reinterpret_cast<HDC>(wp));
            case WM_ERASEBKGND:
                // 背景由 WM_PAINT 统一填充，避免 erase+paint 双次白闪
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                self->FillParamViewportGaps(hdc, hwnd, ps.rcPaint);
                self->PaintEditorParamChrome(hdc, hwnd);
                EndPaint(hwnd, &ps);
                // 父窗白底填充后须再刷灰按钮，否则「全图/选取区域」上边框常被盖住
                self->RefreshGrayButtonsInParamViewport();
                return 0;
            }
            case WM_MOUSEWHEEL:
                self->OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
                return 0;
            case WM_MOUSEMOVE: {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ClientToScreen(hwnd, &pt);
                ScreenToClient(self->hwnd_, &pt);
                self->OnMouseMove(pt.x, pt.y);
                return 0;
            }
            case WM_MOUSELEAVE: {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(self->hwnd_, &pt);
                self->OnMouseMove(pt.x, pt.y);
                return 0;
            }
            default:
                break;
            }
        }
        if (self && self->IsParamPanelCheckbox(hwnd)) {
            switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PRINTCLIENT:
                self->PaintParamPanelCheckbox(hwnd, reinterpret_cast<HDC>(wp));
                return 0;
            case WM_PAINT:
                return DefSubclassProc(hwnd, msg, wp, lp);
            case WM_LBUTTONDOWN:
                // 按下即切换（与设置页一致），避免等 UP 才反馈、连点时显得迟钝。
                SetFocus(hwnd);
                SetCapture(hwnd);
                self->ToggleParamCheckbox(hwnd);
                return 0;
            case WM_LBUTTONDBLCLK:
                // 系统对第二次按下发 DBLCLK 而非 DOWN；原先直接吞掉会导致快速连点只生效一次。
                SetFocus(hwnd);
                SetCapture(hwnd);
                self->ToggleParamCheckbox(hwnd);
                return 0;
            case WM_LBUTTONUP:
                // 仍用 capture：下拉切类型露出勾选框后，误落在本控件上的 UP 不得再切一次。
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            case WM_KEYDOWN:
                if (wp == VK_SPACE) {
                    self->ToggleParamCheckbox(hwnd);
                    return 0;
                }
                break;
            case WM_NCDESTROY:
                RemovePropW(hwnd, kParamCheckboxProp);
                RemovePropW(hwnd, kParamCheckboxCheckedProp);
                break;
            default:
                break;
            }
        }
        if (self && self->IsGrayButton(hwnd)) {
            if (msg == WM_LBUTTONDOWN) {
                FiDbgOnGrayClick(hwnd);
            }
            if (msg == WM_LBUTTONUP) {
                FiDbgLogFmt(L"GRAY_LBUTTONUP", L"btn=%p name=%s",
                    hwnd, self->GrayButtonDebugName(hwnd));
                LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
                self->EditorChildMessage(hwnd, msg, wp, lp);
                self->FinishGrayButtonClick(hwnd);
                return r;
            }
            if (msg == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lp) != hwnd) {
                LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
                self->FinishGrayButtonClick(hwnd);
                return r;
            }
        }
        wchar_t clsName[32]{};
        GetClassNameW(hwnd, clsName, 32);
        if (lstrcmpW(clsName, L"EDIT") == 0 && ModernEditHandleShortcutMessage(hwnd, msg, wp, lp))
            return 0;
        if (self && (msg == WM_ERASEBKGND || msg == WM_PAINT)) {
            if (self->IsParamMaskControl(hwnd)) {
                if (msg == WM_ERASEBKGND) return 1;
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRectColor(hdc, rc, hwnd == self->paramBottomMask_ ? kPanel : kWhite);
                EndPaint(hwnd, &ps);
                return 0;
            }
            if (self->EditorComboPopupIdForHwnd(hwnd) >= 0) {
                return msg == WM_ERASEBKGND ? 1 : 0;
            }
        }
        if (self && msg == WM_ERASEBKGND && self->IsGrayButton(hwnd)) {
            return 1;
        }
        if (self && msg == WM_MOUSEWHEEL && self->page_ == Page::Editor) {
            self->OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        if (self && msg == WM_KEYDOWN && hwnd == self->listRemarkEdit_) {
            if (wp == VK_RETURN) { self->CommitInlineRemark(); return 0; }
            if (wp == VK_ESCAPE) { self->CancelInlineRemark(); return 0; }
        }
        if (self && msg == WM_LBUTTONDOWN) {
            CrosshairDragBinding binding{};
            if (self->crosshairDrag_.TryGetBinding(hwnd, binding)) {
                self->crosshairDrag_.Begin(binding);
                return 0;
            }
        }
        // Combo toggling is handled in parent OnMouseDown via EditorComboHitTest.
        if (self && hwnd == self->quickInputEdit_ && msg == WM_MOUSEMOVE) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            ScreenToClient(self->hwnd_, &pt);
            self->UpdateQuickInputTextTip(pt.x, pt.y);
        }
        if (self && hwnd == self->quickInputEdit_ && msg == WM_MOUSELEAVE) {
            self->CancelQuickInputTip();
        }
        if (self) self->EditorChildMessage(hwnd, msg, wp, lp);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    void EditorChildMessage(HWND hwnd, UINT msg, WPARAM, LPARAM lp) {
        if (page_ != Page::Editor) return;
        if (msg == WM_MOUSEMOVE) EnsureChildMouseTracking(hwnd);
        if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE) {
            POINT pt{};
            if (msg == WM_MOUSEMOVE) {
                pt.x = GET_X_LPARAM(lp);
                pt.y = GET_Y_LPARAM(lp);
                ClientToScreen(hwnd, &pt);
            } else {
                GetCursorPos(&pt);
            }
            ScreenToClient(hwnd_, &pt);
            OnMouseMove(pt.x, pt.y);
        }
    }

    void AttachEditorChildSubclass() {
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            SetWindowSubclass(child, EditorChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        }
    }

    RECT WindowClientRect(HWND child) const {
        RECT rc{};
        if (!child) return rc;
        GetWindowRect(child, &rc);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&rc), 2);
        return rc;
    }

    void UpdateHoverFromCursor() {
        if (page_ != Page::Editor || !hwnd_) return;
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (pt.x < rc.left || pt.y < rc.top || pt.x >= rc.right || pt.y >= rc.bottom) return;
        OnMouseMove(pt.x, pt.y);
    }

    void StartHoverTimer() { if (hwnd_) SetTimer(hwnd_, kHoverTimerId, 30, nullptr); }
    void StopHoverTimer() { if (hwnd_) KillTimer(hwnd_, kHoverTimerId); }

    // ── Inline remark editing ──────────────────────────────────────
    void CommitInlineRemark() {
        if (editingRemarkIndex_ < 0 || !listRemarkEdit_) return;
        const int idx = editingRemarkIndex_;
        // 先清索引，避免 ShowWindow(SW_HIDE) 同步触发 EN_KILLFOCUS 重入
        editingRemarkIndex_ = -1;
        if (IsWindowVisible(listRemarkEdit_)) {
            if (idx >= 0 && idx < static_cast<int>(actions_.size())) {
                actions_[static_cast<size_t>(idx)].remark = GetText(listRemarkEdit_);
            }
            ShowWindow(listRemarkEdit_, SW_HIDE);
        }
        RefreshActionListLayer();
    }

    void CancelInlineRemark() {
        if (editingRemarkIndex_ < 0 || !listRemarkEdit_) return;
        editingRemarkIndex_ = -1;
        ShowWindow(listRemarkEdit_, SW_HIDE);
        RefreshActionListLayer();
    }

    void BeginInlineRemarkEdit(int row) {
        if (row < 0 || row >= static_cast<int>(actions_.size())) return;
        if (editingRemarkIndex_ != row) CommitInlineRemark();
        editingRemarkIndex_ = row;
        if (selectedIndex_ < 0 && !loadingForm_) {
            actionFormDrafts_[popupAction_.sel] = ActionFromForm();
        }
        selectedIndex_ = row;
        LoadForm(actions_[static_cast<size_t>(row)]);
        RECT rc = RemarkRect(row);
        MoveWindow(listRemarkEdit_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        SetText(listRemarkEdit_, actions_[static_cast<size_t>(row)].remark);
        ShowWindow(listRemarkEdit_, SW_SHOW);
        SetWindowPos(listRemarkEdit_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetFocus(listRemarkEdit_);
        SendMessageW(listRemarkEdit_, EM_SETSEL, 0, -1);
        RefreshActionListLayer();
    }

    RECT RemarkRect(int index) const {
        RECT r = RowRect(index);
        const int left = kColRemarkClient + 1;
        const int right = std::max(left + 40, std::min(kColOpClient - 4, ActionListContentRight() - 2));
        return RECT{left, r.top + (kRowH - kRemarkEditH) / 2, right, r.top + (kRowH - kRemarkEditH) / 2 + kRemarkEditH};
    }

    bool PtInRemark(int x, int y) const {
        int row = HitRow(x, y);
        return row >= 0 && PtIn(RemarkRect(row), x, y);
    }

    void UpdateInlineRemarkEditorPosition() {
        if (editingRemarkIndex_ < 0 || !listRemarkEdit_ || !IsWindowVisible(listRemarkEdit_)) return;
        const int visible = VisibleActionRows();
        const int slot = VisibleSlotOf(editingRemarkIndex_);
        if (slot < 0 || slot < scrollOffset_ || slot >= scrollOffset_ + visible) {
            CommitInlineRemark();
            return;
        }
        RECT rc = RemarkRect(editingRemarkIndex_);
        MoveWindow(listRemarkEdit_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
    }

    void EnsureMouseTracking() {
        if (trackingMouse_) return;
        TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd_, 0};
        if (TrackMouseEvent(&tme)) trackingMouse_ = true;
    }

    void EnsureChildMouseTracking(HWND hwnd) {
        TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    }

    void OnMouseLeave() {
        trackingMouse_ = false;
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        RECT client{};
        GetClientRect(hwnd_, &client);
        if (PtInRect(&client, pt)) {
            UpdateGrayButtonHover(pt.x, pt.y);
            FlushGrayButtonHover();
            return;
        }
        ClearGrayButtonHover();
        pendingHoverGrayOld_ = nullptr;
        pendingHoverGrayNew_ = nullptr;
        if (page_ == Page::Editor) {
            const int oldRow = hoverIndex_;
            const HoverButton oldButton = hoverButton_;
            hoverIndex_ = -1;
            hoverButton_ = HoverButton::None;
            if (oldRow != -1) RefreshActionListLayer();
            if (oldButton != HoverButton::None) InvalidateHoverButton(oldButton);
        } else if (page_ == Page::Home) {
            const int oldCard = homeHover_;
            const int oldRecording = recordingHover_;
            const HoverButton oldButton = hoverButton_;
            homeHover_ = -1;
            recordingHover_ = -1;
            hoverButton_ = HoverButton::None;
            if (oldCard != -1) InvalidateHomeCard(oldCard);
            if (oldRecording != -1) InvalidateRecordingCard(oldRecording);
            if (oldButton != HoverButton::None) InvalidateHoverButton(oldButton);
        }
    }

    // ── Action list off-screen rendering ───────────────────────────
    void RefreshActionListLayer() {
        if (page_ != Page::Editor || !hwnd_) return;
        scrollOffset_ = std::clamp(scrollOffset_, 0, MaxEditorScroll());
        RECT list = ActionListRect();
        const int lw = list.right - list.left;
        const int lh = list.bottom - list.top;
        if (lw <= 0 || lh <= 0) return;
        HDC wndDc = GetDC(hwnd_);
        HDC memDc = CreateCompatibleDC(wndDc);
        HBITMAP memBmp = CreateCompatibleBitmap(wndDc, lw, lh);
        HGDIOBJ oldBmp = SelectObject(memDc, memBmp);
        HGDIOBJ oldFont = SelectObject(memDc, editorFont_);
        RenderBatchScope batch(memDc);
        PaintActionListLocal(memDc, lw, lh);
        SelectObject(memDc, oldFont);
        batch.End();
        if (IsWindowVisible(listRemarkEdit_)) {
            RECT erc = WindowClientRect(listRemarkEdit_);
            ExcludeClipRect(wndDc, erc.left, erc.top, erc.right, erc.bottom);
        }
        BitBlt(wndDc, list.left, list.top, lw, lh, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDc);
        PaintDragMarker(wndDc);
        UpdateInlineRemarkEditorPosition();
        ReleaseDC(hwnd_, wndDc);
    }

    void DrawExpandTriangle(HDC hdc, RECT rc, bool expanded, COLORREF color) {
        ::DrawExpandTriangle(hdc, rc, expanded, color);
    }

    void ToggleContainerExpand(int index) {
        if (index < 0 || index >= static_cast<int>(actions_.size()) || !IsExpandableContainer(actions_[static_cast<size_t>(index)].type)) return;
        if (IsContainerExpanded(index)) collapsedContainers_.insert(index);
        else collapsedContainers_.erase(index);
        MarkVisibleActionsDirty();
        scrollOffset_ = std::clamp(scrollOffset_, 0, MaxEditorScroll());
        RefreshActionListLayer();
    }

    void PaintActionListLocal(HDC hdc, int width, int height);

    RECT LocalCheckboxRect(int index, int y) const {
        const int pad = UiLen(kListInnerPad);
        const auto& a = actions_[static_cast<size_t>(index)];
        const int rowH = std::max(1, UiLen(kRowH));
        const int cb = std::max(1, UiLen(kBatchCheckboxSize));
        const int top = y + (rowH - cb) / 2;
        const int left = BatchCheckboxLeftLocal(a.indent, a.type, pad);
        return RECT{left, top, left + cb, top + cb};
    }

    RECT LocalCopyRect(int, int y, int contentRight) const {
        const int rowH = std::max(1, UiLen(kRowH));
        return RECT{contentRight - UiLen(104), y + UiLen(6), contentRight - UiLen(62), y + rowH - UiLen(6)};
    }
    RECT LocalDeleteRect(int, int y, int contentRight) const {
        const int rowH = std::max(1, UiLen(kRowH));
        return RECT{contentRight - UiLen(58), y + UiLen(6), contentRight - UiLen(18), y + rowH - UiLen(6)};
    }

    void PaintEditorScrollbarLocal(HDC hdc, int width, int height);
    void PaintParamScrollScrollbar(HDC hdc);

    void InvalidateRectClipped(RECT rc) {
        if (rc.right > rc.left && rc.bottom > rc.top) InvalidateRect(hwnd_, &rc, FALSE);
    }

    void InvalidateHomeCard(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        InvalidateRectClipped(HomeCardRect(index));
    }

    void InvalidateRecordingCard(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        InvalidateRectClipped(RecordingCardRect(index));
    }

    void InvalidateAgentConvCard(int index) {
        if (index < 0 || index >= static_cast<int>(agentConversations_.size())) return;
        InvalidateRectClipped(HomeCardRect(index));
    }

    int HitAgentConvCard(int x, int y) const {
        if (activeHomeTab_ != quickscript::MainTab::ScriptCustom) return -1;
        RECT list = HomeListRect();
        if (!PtIn(list, x, y)) return -1;
        for (int i = 0; i < static_cast<int>(agentConversations_.size()); ++i) {
            RECT r = HomeCardRect(i);
            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
        }
        return -1;
    }

    int HitRecordingCard(int x, int y) const {
        if (activeHomeTab_ != quickscript::MainTab::Recorder) return -1;
        RECT list = HomeListRect();
        if (!PtIn(list, x, y)) return -1;
        for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
            RECT r = RecordingCardRect(i);
            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
        }
        return -1;
    }

    void InvalidateRow(int index) {
        if (page_ == Page::Editor) { RefreshActionListLayer(); return; }
        if (index < 0 || index >= static_cast<int>(actions_.size())) return;
        RECT r = RowRect(index);
        if (r.bottom < kListY || r.top > kListY + kListH) return;
        InvalidateRectClipped(r);
    }

    HWND HoverButtonHwnd(HoverButton btn) const {
        switch (btn) {
        case HoverButton::Load: return loadBtn_;
        case HoverButton::Clear: return clearBtn_;
        case HoverButton::Add: return addBtn_;
        case HoverButton::Modify: return modifyBtn_;
        case HoverButton::Cancel: return cancelBtn_;
        case HoverButton::Save: return saveBtn_;
        case HoverButton::Crosshair: return hoverCrosshairBtn_ ? hoverCrosshairBtn_ : crosshairBtn_;
        case HoverButton::BatchExit: return batchExitBtn_;
        case HoverButton::BatchSelectAll: return batchSelectAllBtn_;
        case HoverButton::BatchDeselect: return batchDeselectBtn_;
        case HoverButton::BatchDelete: return batchDeleteBtn_;
        case HoverButton::BatchCopy: return batchCopyBtn_;
        default: return nullptr;
        }
    }

    void InvalidateHoverButton(HoverButton btn) {
        if (btn == HoverButton::Close || btn == HoverButton::Minimize || btn == HoverButton::Settings) {
            RECT rc = btn == HoverButton::Close ? CloseRect()
                : (btn == HoverButton::Minimize ? MinimizeRect() : SettingsRect());
            InvalidateRect(hwnd_, &rc, FALSE);
            return;
        }
        HWND btnHwnd = HoverButtonHwnd(btn);
        if (btnHwnd) InvalidateRect(btnHwnd, nullptr, FALSE);
    }

    // ── Mouse input ────────────────────────────────────────────────
    void OnMouseMove(int x, int y) {
        if (promptModal_.visible()) return;
        if (page_ == Page::Editor && editorPopupOpen_ >= 0) {
            if (EditorDropPopupVisible()) return;
        }
        if (page_ == Page::Editor) UpdateQuickInputTextTip(x, y);
        if (homeScrollbarDragging_) {
            UpdateHomeScrollFromThumb(y - homeScrollbarDragOffset_);
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (editorScrollbarDragging_) {
            UpdateEditorScrollFromThumb(y - editorScrollbarDragOffset_);
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            RefreshActionListLayer();
            return;
        }
        if (paramScrollbarDragging_) {
            CloseEditorPopupIfParamAnchored();
            UpdateParamScrollFromThumb(y - paramScrollbarDragOffset_);
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            ApplyParamScrollOffset(true, false);
            InvalidateRectClipped(ParamScrollTrackRect());
            return;
        }
        HoverButton oldButton = hoverButton_;
        hoverButton_ = HitButton(x, y);
        hoverCrosshairBtn_ = (hoverButton_ == HoverButton::Crosshair) ? CrosshairButtonAtPoint(x, y) : nullptr;
        UpdateGrayButtonHover(x, y);
        if (page_ == Page::Editor && PtInRemark(x, y)) SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        else if (hoverGrayBtn_) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        else SetCursor(LoadCursorW(nullptr, IsClickablePoint(x, y) ? IDC_HAND : IDC_ARROW));
        EnsureMouseTracking();
        if (page_ == Page::Home) {
            const int oldCard = homeHover_;
            const int oldRecording = recordingHover_;
            const int oldAgentConv = agentConvHover_;
            homeHover_ = HitHomeCard(x, y);
            recordingHover_ = HitRecordingCard(x, y);
            agentConvHover_ = HitAgentConvCard(x, y);
            if (oldCard != homeHover_) {
                InvalidateHomeCard(oldCard);
                InvalidateHomeCard(homeHover_);
            }
            if (oldRecording != recordingHover_) {
                InvalidateRecordingCard(oldRecording);
                InvalidateRecordingCard(recordingHover_);
            }
            if (oldAgentConv != agentConvHover_) {
                InvalidateAgentConvCard(oldAgentConv);
                InvalidateAgentConvCard(agentConvHover_);
            }
            if (oldButton != hoverButton_) {
                InvalidateHoverButton(oldButton);
                InvalidateHoverButton(hoverButton_);
            }
            FlushGrayButtonHover();
            return;
        }
        const int oldRow = hoverIndex_;
        hoverIndex_ = HitRow(x, y);
        if (dragging_) {
            if (abs(x - dragStartX_) >= kDragThreshold || abs(y - dragStartY_) >= kDragThreshold) {
                MoveDragged(x, y);
                RefreshActionListLayer();
            }
        } else {
            if (oldRow != hoverIndex_) RefreshActionListLayer();
            // Defer all button invalidations until after RefreshActionListLayer
            // to avoid races between direct screen painting and WM_DRAWITEM
            HoverButton oldHb = (oldButton != hoverButton_) ? oldButton : HoverButton::None;
            HoverButton newHb = (oldButton != hoverButton_) ? hoverButton_ : HoverButton::None;
            if (oldHb != HoverButton::None) InvalidateHoverButton(oldHb);
            if (newHb != HoverButton::None) InvalidateHoverButton(newHb);
            FlushGrayButtonHover();
        }
    }

    HWND CrosshairButtonAtPoint(int x, int y) const {
        return crosshairDrag_.HitButton(x, y, [this](HWND hwnd) { return WindowClientRect(hwnd); });
    }

    HoverButton HitButton(int x, int y) const {
        if (HitClose(x, y)) return HoverButton::Close;
        if (HitMinimize(x, y)) return HoverButton::Minimize;
        if (page_ == Page::Home && HitSettings(x, y)) return HoverButton::Settings;
        if (page_ == Page::Home) {
            if (PtIn(ClickerTabRect(), x, y) || PtIn(RecorderTabRect(), x, y) || PtIn(MacroTabRect(), x, y) || PtIn(ScriptCustomTabRect(), x, y)) return HoverButton::HomeCard;
            if (activeHomeTab_ == quickscript::MainTab::Clicker) {
                if (ClickerDropPopupVisible()) return HoverButton::ClickerInterval;
                if (PtIn(ClickerIntervalRect(), x, y)) return HoverButton::ClickerInterval;
                if (PtIn(ClickerHotkeyRect(), x, y)) return HoverButton::ClickerHotkey;
                if (PtIn(ClickerBannerKeyRect(), x, y)) return HoverButton::CommonHotkey;
                if (PtIn(ClickerLeftRadioRect(), x, y) || PtIn(ClickerMiddleRadioRect(), x, y) || PtIn(ClickerRightRadioRect(), x, y)) return HoverButton::HomeCard;
                return HoverButton::None;
            }
            if (activeHomeTab_ == quickscript::MainTab::Recorder) {
                if (PtIn(ImportRect(), x, y)) return HoverButton::Import;
                if (PtIn(ExportRect(), x, y)) return HoverButton::Export;
                if (PtIn(TimerRect(), x, y)) return HoverButton::HomeCard;
                if (PtIn(RecorderModeRect(), x, y)) return HoverButton::HomeEdit;
                if (PtIn(RecorderBannerKeyRect(), x, y)) return HoverButton::CommonHotkey;
                if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) return HoverButton::HomeScroll;
                for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
                    RECT r = RecordingCardRect(i);
                    RECT list = HomeListRect();
                    if (r.bottom < list.top || r.top > list.bottom) continue;
                    if (i == selectedRecording_) {
                        if (PtIn(RecordingDeselectRect(i), x, y)) return HoverButton::HomeEdit;
                        if (PtIn(RecordingSelectedTagRect(i), x, y)) return HoverButton::HomeCard;
                    } else if (i == recordingHover_) {
                        if (PtIn(RecordingOptimizeRect(i), x, y)) return HoverButton::HomeEdit;
                        if (PtIn(RecordingRenameRect(i), x, y)) return HoverButton::HomeEdit;
                        if (PtIn(RecordingDeleteRect(i), x, y)) return HoverButton::HomeDelete;
                    }
                    if (PtIn(RecordingHotkeyRect(i), x, y)) return HoverButton::ScriptHotkey;
                    if (PtIn(r, x, y)) return HoverButton::HomeCard;
                }
                return HoverButton::None;
            }
            if (activeHomeTab_ == quickscript::MainTab::ScriptCustom) {
                if (PtIn(CreateRect(), x, y)) return HoverButton::Create;
                if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) return HoverButton::HomeScroll;
                for (int i = 0; i < static_cast<int>(agentConversations_.size()); ++i) {
                    RECT r = HomeCardRect(i);
                    RECT list = HomeListRect();
                    if (r.bottom < list.top || r.top > list.bottom) continue;
                    if (PtIn(AgentConvChatRect(i), x, y)) return HoverButton::HomeEdit;
                    if (PtIn(AgentConvDeleteRect(i), x, y)) return HoverButton::HomeDelete;
                    if (PtIn(r, x, y)) return HoverButton::HomeCard;
                }
                return HoverButton::None;
            }
            if (PtIn(ImportRect(), x, y)) return HoverButton::Import;
            if (PtIn(ExportRect(), x, y)) return HoverButton::Export;
            if (PtIn(TimerRect(), x, y)) return HoverButton::HomeCard;
            if (selectedScript_ >= 0 && PtIn(CommonHotRect(), x, y)) return HoverButton::CommonHotkey;
            if (selectedScript_ < 0 && PtIn(CreateWordRect(), x, y)) return HoverButton::Create;
            if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) return HoverButton::HomeScroll;
            for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
                RECT r = HomeCardRect(i);
                RECT list = HomeListRect();
                if (r.bottom < list.top || r.top > list.bottom) continue;
                RECT hot = ScriptHotkeyRect(i);
                RECT edit = HomeCardEditBtn(r);
                RECT del = HomeCardDeleteBtn(r);
                if (PtIn(hot, x, y)) return HoverButton::ScriptHotkey;
                if (i == selectedScript_ && PtIn(edit, x, y)) return HoverButton::HomeEdit;
                if (i == homeHover_) {
                    if (i != selectedScript_ && PtIn(edit, x, y)) return HoverButton::HomeEdit;
                    if (PtIn(del, x, y)) return HoverButton::HomeDelete;
                }
                if (PtIn(r, x, y)) return HoverButton::HomeCard;
            }
            return HoverButton::None;
        }
        if (PtIn(LoadButtonRect(), x, y) && !batchEditMode_) return HoverButton::Load;
        if (PtIn(ClearButtonRect(), x, y) && !batchEditMode_) return HoverButton::Clear;
        if (batchEditMode_) {
            if (PtIn(BatchExitButtonRect(), x, y)) return HoverButton::BatchExit;
            if (PtIn(BatchSelectAllButtonRect(), x, y)) return HoverButton::BatchSelectAll;
            if (PtIn(BatchDeselectButtonRect(), x, y)) return HoverButton::BatchDeselect;
            if (BatchSelectedCount() > 0) {
                if (PtIn(BatchDeleteButtonRect(), x, y)) return HoverButton::BatchDelete;
                if (PtIn(BatchCopyButtonRect(), x, y)) return HoverButton::BatchCopy;
            }
        }
        if (PtIn(AddButtonRect(), x, y)) return HoverButton::Add;
        if (ShouldShowModifyButton() && PtIn(ModifyButtonRect(), x, y)) return HoverButton::Modify;
        if (PtIn(CancelButtonRect(), x, y)) return HoverButton::Cancel;
        if (PtIn(SaveButtonRect(), x, y)) return HoverButton::Save;
        if (CrosshairButtonAtPoint(x, y)) return HoverButton::Crosshair;
        if (MaxEditorScroll() > 0 && PtIn(EditorScrollTrackRect(), x, y)) return HoverButton::EditorScroll;
        int row = HitRow(x, y);
        if (row >= 0) {
            if (batchEditMode_) {
                if (IsExpandableContainer(actions_[static_cast<size_t>(row)].type) && PtIn(ExpandToggleRect(row), x, y)) return HoverButton::Row;
                if (PtIn(CheckboxRect(row), x, y)) return HoverButton::RowCheckbox;
                return HoverButton::None;
            }
            if (PtIn(CopyRect(row), x, y)) return HoverButton::RowCopy;
            if (PtIn(DeleteRect(row), x, y)) return HoverButton::RowDelete;
            return HoverButton::Row;
        }
        return HoverButton::None;
    }

    bool IsClickablePoint(int x, int y) const {
        if (page_ == Page::Editor && PtInRemark(x, y)) return true;
        const HoverButton hb = HitButton(x, y);
        // 整行可选中，但空白行体不做手型；仅复制/删除/批量勾选等真交互点
        if (hb == HoverButton::None || hb == HoverButton::Row) {
            return HitGrayButton(x, y) != nullptr;
        }
        return true;
    }

    void ClickButton(HoverButton button, int x, int y) {
        if (button == HoverButton::Close) { if (page_ == Page::Editor) ShowHome(); else SendMessageW(hwnd_, WM_CLOSE, 0, 0); return; }
        if (button == HoverButton::Minimize) { ShowWindow(hwnd_, SW_MINIMIZE); return; }
        if (button == HoverButton::Settings && page_ == Page::Home) { ShowSettingsDialog(); return; }
        if (button == HoverButton::Import) { ImportScript(); return; }
        if (button == HoverButton::Export) { ExportSelectedScript(); return; }
        if (button == HoverButton::Create) { ShowEditorFor(-1, true); return; }
        if (button == HoverButton::CommonHotkey) { CaptureGlobalHotkey(); return; }
        if (button == HoverButton::Load) { EnterBatchEditMode(); return; }
        if (button == HoverButton::Clear) { ClearEditorActions(); return; }
        if (button == HoverButton::BatchExit) { ExitBatchEditMode(); return; }
        if (button == HoverButton::BatchSelectAll) { BatchSelectAll(); return; }
        if (button == HoverButton::BatchDeselect) { BatchDeselectAll(); return; }
        if (button == HoverButton::BatchDelete) { BatchDeleteSelected(); return; }
        if (button == HoverButton::BatchCopy) { BatchCopySelected(); return; }
        if (button == HoverButton::Add) { ShowAddMenuAt(x, y); return; }
        if (button == HoverButton::Modify) { ModifySelected(); return; }
        if (button == HoverButton::Cancel) { ShowHome(); return; }
        if (button == HoverButton::Save) { SaveIfEditor(); ShowHome(); return; }
    }

    int HitHomeCard(int x, int y) const {
        if (activeHomeTab_ != quickscript::MainTab::Macro) return -1;
        RECT list = HomeListRect();
        if (!PtIn(list, x, y)) return -1;
        for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
            RECT r = HomeCardRect(i);
            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
        }
        return -1;
    }

    void OnMouseDown(int x, int y) {
        if (promptModal_.visible()) return;
        if (HitClose(x, y)) { if (page_ == Page::Editor) ShowHome(); else SendMessageW(hwnd_, WM_CLOSE, 0, 0); return; }
        if (HitMinimize(x, y)) { ShowWindow(hwnd_, SW_MINIMIZE); return; }
        if (HitSettings(x, y) && page_ == Page::Home) { ShowSettingsDialog(); return; }
        if (y <= UiLen(kTitleH)) { ReleaseCapture(); SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(x, y)); return; }
        if (page_ == Page::Home && (activeHomeTab_ == quickscript::MainTab::Macro || activeHomeTab_ == quickscript::MainTab::ScriptCustom)
            && ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollThumbRect(), x, y)) {
            RECT thumb = HomeScrollThumbRect();
            homeScrollbarDragging_ = true;
            homeScrollbarDragOffset_ = y - thumb.top;
            SetCapture(hwnd_);
            return;
        }
        if (page_ == Page::Editor && EditorComboHitTest(x, y)) return;
        if (page_ == Page::Editor && editorPopupOpen_ >= 0) {
            if (EditorDropPopupVisible()) {
                POINT pt{x, y};
                ClientToScreen(hwnd_, &pt);
                RECT drop{};
                GetWindowRect(editorDropPopup_, &drop);
                if (!PtIn(drop, pt.x, pt.y)) {
                    const int comboHit = EditorComboPopupIdAtPoint(x, y);
                    if (comboHit == editorPopupOpen_) ToggleEditorPopup(comboHit);
                    else CloseEditorPopup();
                }
            } else if (HandleEditorPopupClick(x, y)) return;
        }
        if (page_ == Page::Home) { OnHomeClick(x, y); return; }
        if (MaxEditorScroll() > 0 && PtIn(EditorScrollThumbRect(), x, y)) {
            RECT thumb = EditorScrollThumbRect();
            editorScrollbarDragging_ = true;
            editorScrollbarDragOffset_ = y - thumb.top;
            SetCapture(hwnd_);
            return;
        }
        if (MaxEditorScroll() > 0 && PtIn(EditorScrollTrackRect(), x, y)) {
            RECT thumb = EditorScrollThumbRect();
            editorScrollbarDragging_ = true;
            editorScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateEditorScrollFromThumb(y - editorScrollbarDragOffset_);
            SetCapture(hwnd_);
            RefreshActionListLayer();
            return;
        }
        if (MaxParamScroll() > 0 && PtIn(ParamScrollThumbRect(), x, y)) {
            RECT thumb = ParamScrollThumbRect();
            paramScrollbarDragging_ = true;
            paramScrollbarDragOffset_ = y - thumb.top;
            SetCapture(hwnd_);
            return;
        }
        if (MaxParamScroll() > 0 && PtIn(ParamScrollTrackRect(), x, y)) {
            RECT thumb = ParamScrollThumbRect();
            paramScrollbarDragging_ = true;
            paramScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateParamScrollFromThumb(y - paramScrollbarDragOffset_);
            SetCapture(hwnd_);
            ApplyParamScrollOffset();
            return;
        }
        HoverButton hit = HitButton(x, y);
        if (hit == HoverButton::Row || hit == HoverButton::RowCopy || hit == HoverButton::RowDelete || hit == HoverButton::RowCheckbox) { OnEditorClick(x, y); return; }
        if (hit != HoverButton::None) { ClickButton(hit, x, y); return; }
        OnEditorClick(x, y);
    }

    void OnMouseUp(int, int) {
        if (promptModal_.visible()) return;
        if (homeScrollbarDragging_) {
            homeScrollbarDragging_ = false;
            ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (editorScrollbarDragging_) {
            editorScrollbarDragging_ = false;
            ReleaseCapture();
            RefreshActionListLayer();
            return;
        }
        if (paramScrollbarDragging_) {
            paramScrollbarDragging_ = false;
            ReleaseCapture();
            ApplyParamScrollOffset(true);
            return;
        }
        if (page_ != Page::Editor || !dragging_) { dragging_ = false; dragIndex_ = -1; dragTargetIndex_ = -1; dragTargetIndent_ = 0; dragTargetNested_ = false; dragStartX_ = 0; dragStartY_ = 0; ReleaseCapture(); return; }
        CompleteDrag();
        dragging_ = false;
        dragIndex_ = -1;
        dragTargetIndex_ = -1;
        dragTargetIndent_ = 0;
        dragTargetNested_ = false;
        dragStartX_ = 0;
        dragStartY_ = 0;
        dragMoved_ = false;
        ReleaseCapture();
        RefreshActionListLayer();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SetActiveHomeTab(quickscript::MainTab tab) {
        if (activeHomeTab_ == tab) return;
        activeHomeTab_ = tab;
        CloseClickerDropPopup();
        CloseEditorPopup();
        hoverButton_ = HoverButton::None;
        homeHover_ = -1;
        recordingHover_ = -1;
        agentConvHover_ = -1;
        if (tab == quickscript::MainTab::Recorder) ClampRecordingScroll();
        if (tab == quickscript::MainTab::Macro) { homeScrollOffset_ = 0; ClampHomeScroll(); }
        if (tab == quickscript::MainTab::ScriptCustom) { homeScrollOffset_ = 0; ClampAgentConvScroll(); }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    // ── 主界面状态缓存（退出时保存，启动时恢复） ──────────────────
    void SaveHomeState();
    void SaveHomeStateCore(bool persistJson);
    void PersistHomeStateJsonOnly();

    void RestoreHomeState();

    bool HandleHomeNavClick(int x, int y) {
        if (PtIn(ClickerTabRect(), x, y)) { SetActiveHomeTab(quickscript::MainTab::Clicker); return true; }
        if (PtIn(RecorderTabRect(), x, y)) { SetActiveHomeTab(quickscript::MainTab::Recorder); return true; }
        if (PtIn(MacroTabRect(), x, y)) { SetActiveHomeTab(quickscript::MainTab::Macro); return true; }
        if (PtIn(ScriptCustomTabRect(), x, y)) { SetActiveHomeTab(quickscript::MainTab::ScriptCustom); return true; }
        return false;
    }

    void CaptureGlobalHotkey() {
        BeginHotkeyCaptureRelease(globalHotkey_);
        HotkeyCapture cap;
        Hotkey out;
        const bool ok = cap.Show(hwnd_, globalHotkey_, false, out, true,
                appSettings_.other.holdThresholdSeconds)
            && out.enabled && out.vk != 0;
        if (ok) globalHotkey_ = out;
        EndHotkeyCaptureRelease();
        if (ok) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool HandleClickerIntervalClick(int x, int y) {
        if (PtIn(ClickerIntervalRect(), x, y)) {
            const RECT rc = ClickerIntervalRect();
            const bool onArrow = x >= rc.right - 36;
            if (clickerSettings_.intervalMode == quickscript::ClickIntervalMode::Custom && !onArrow) {
                ShowCustomIntervalDialog();
                return true;
            }
            if (clickerDropPopupKind_ == 0) CloseClickerDropPopup();
            else OpenClickerDropPopup(0);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (clickerDropPopupKind_ == 0) {
            if (PtIn(ClickerHotkeyRect(), x, y)) return false;
            CloseClickerDropPopup();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        return false;
    }

    bool HandleClickerHotkeyClick(int x, int y) {
        if (PtIn(ClickerHotkeyRect(), x, y)) {
            if (clickerDropPopupKind_ == 1) CloseClickerDropPopup();
            else OpenClickerDropPopup(1);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (clickerDropPopupKind_ == 1) {
            if (PtIn(ClickerIntervalRect(), x, y)) return false;
            CloseClickerDropPopup();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        return false;
    }

    void SyncOwnedDropPopups() {
        if (editorPopupOpen_ >= 0) SyncEditorDropPopup();
        if (clickerDropPopupKind_ >= 0) SyncClickerDropPopup();
        if (quickInputTipShown_ != QuickInputTipKind::None) SyncQuickInputTipPopup();
    }

    void OpenClickerDropPopup(int kind) {
        clickerDropPopupKind_ = kind;
        clickerPopupHover_ = -1;
        clickerPopupScroll_ = 0;
        clickerPopupVisibleCount_ = 0;
        SyncClickerDropPopup();
    }

    void CloseClickerDropPopup() {
        if (clickerDropPopupKind_ < 0) return;
        clickerDropPopupKind_ = -1;
        clickerPopupHover_ = -1;
        clickerPopupScroll_ = 0;
        clickerPopupVisibleCount_ = 0;
        if (clickerDropPopup_) ShowWindow(clickerDropPopup_, SW_HIDE);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool ClickerDropPopupVisible() const {
        return clickerDropPopup_ && IsWindowVisible(clickerDropPopup_) == TRUE;
    }

    void CreateClickerDropPopup() {
        RegisterClickerDropPopupClass();
        clickerDropPopup_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"QSClickerDropPopup", L"",
            WS_POPUP,
            0, 0, 0, 0,
            hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (clickerDropPopup_) {
            SetWindowLongPtrW(clickerDropPopup_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(clickerDropPopup_, SW_HIDE);
        }
    }

    void SyncClickerDropPopup() {
        if (!clickerDropPopup_) return;
        if (clickerDropPopupKind_ < 0 || page_ != Page::Home
            || activeHomeTab_ != quickscript::MainTab::Clicker
            || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
            ShowWindow(clickerDropPopup_, SW_HIDE);
            return;
        }
        const RECT anchor = clickerDropPopupKind_ == 0 ? ClickerIntervalRect() : ClickerHotkeyRect();
        const int itemCount = ClickerPopupItemCount();
        const int w = anchor.right - anchor.left;
        const int totalH = itemCount * kClickerDropdownItemH + 2;

        POINT anchorBottomScreen{anchor.left, anchor.bottom};
        ClientToScreen(hwnd_, &anchorBottomScreen);

        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int spaceBelow = static_cast<int>(work.bottom) - anchorBottomScreen.y;

        int h = totalH;
        const int x = anchorBottomScreen.x;
        int y = anchorBottomScreen.y;
        if (h > spaceBelow) {
            clickerPopupVisibleCount_ = std::max(1, spaceBelow / kClickerDropdownItemH);
            h = clickerPopupVisibleCount_ * kClickerDropdownItemH + 2;
            clickerPopupScroll_ = std::clamp(clickerPopupScroll_, 0,
                std::max(0, itemCount - clickerPopupVisibleCount_));
        } else {
            clickerPopupVisibleCount_ = itemCount;
            clickerPopupScroll_ = 0;
        }
        y = std::max(static_cast<int>(work.top),
            std::min(y, static_cast<int>(work.bottom) - h));

        RECT existing{};
        GetWindowRect(clickerDropPopup_, &existing);
        const bool samePos = existing.left == x && existing.top == y
            && (existing.right - existing.left) == w && (existing.bottom - existing.top) == h;
        if (samePos && ClickerDropPopupVisible()) return;
        SetWindowPos(clickerDropPopup_, HWND_TOPMOST, x, y, w, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
    }

    void InvalidateClickerPopupRow(int idx) {
        if (!clickerDropPopup_ || idx < 0) return;
        RECT client{};
        GetClientRect(clickerDropPopup_, &client);
        const int vis = idx - clickerPopupScroll_;
        if (vis < 0 || vis >= ClickerPopupVisibleCount()) return;
        RECT row{client.left + 1, client.top + 1 + vis * kClickerDropdownItemH,
            client.right - 1, client.top + 1 + (vis + 1) * kClickerDropdownItemH};
        InvalidateRect(clickerDropPopup_, &row, FALSE);
    }

    int HitClickerPopupItemLocal(int, int y) const {
        if (clickerDropPopupKind_ < 0) return -1;
        const int visible = ClickerPopupVisibleCount();
        const int rel = (y - 1) / kClickerDropdownItemH;
        if (rel < 0 || rel >= visible) return -1;
        const int idx = rel + clickerPopupScroll_;
        if (idx >= ClickerPopupItemCount()) return -1;
        return idx;
    }

    void OnClickerDropPopupWheel(int delta) {
        if (clickerDropPopupKind_ < 0) return;
        const int itemCount = ClickerPopupItemCount();
        const int scrollMax = std::max(0, itemCount - ClickerPopupVisibleCount());
        if (scrollMax <= 0) return;
        const int oldScroll = clickerPopupScroll_;
        clickerPopupScroll_ = std::clamp(clickerPopupScroll_ + (delta > 0 ? -1 : 1), 0, scrollMax);
        if (oldScroll != clickerPopupScroll_ && clickerDropPopup_) {
            InvalidateRect(clickerDropPopup_, nullptr, FALSE);
        }
    }

    void SelectClickerPopupItem(int idx) {
        if (clickerDropPopupKind_ == 0) {
            const quickscript::ClickIntervalMode modes[3] = {
                quickscript::ClickIntervalMode::Custom,
                quickscript::ClickIntervalMode::Efficient,
                quickscript::ClickIntervalMode::Extreme
            };
            if (idx < 0 || idx >= 3) return;
            CloseClickerDropPopup();
            if (modes[idx] == quickscript::ClickIntervalMode::Custom) {
                ShowCustomIntervalDialog();
                return;
            }
            clickerSettings_.intervalMode = modes[idx];
        } else if (clickerDropPopupKind_ == 1) {
            static const HotkeyMenuItem kItems[] = {
                {kHotCustom, L"自定义", L"将您指定的按键设为启停热键"},
                {kHotLeft, L"鼠标左键", L"按住左键开始连点，松开停止"},
                {kHotMiddle, L"鼠标中键", L"将点击中键设为启停热键"},
                {kHotRight, L"鼠标右键", L"将点击右键设为启停热键"},
                {kHotX1, L"鼠标侧键1", L"一般为鼠标左侧后部的键"},
                {kHotX2, L"鼠标侧键2", L"一般为鼠标左侧前部的键"},
                {kHotSpace, L"空格键", L"将空格键设为启停热键"},
            };
            if (idx < 0 || idx >= kClickerHotkeyMenuCount) return;
            CloseClickerDropPopup();
            SetCommonHotkeyFromMenu(kItems[idx].id);
            return;
        }
        CloseClickerDropPopup();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ShowCustomIntervalDialog();

    void OnClickerHomeClick(int x, int y) {
        if (HandleClickerIntervalClick(x, y)) return;
        if (HandleClickerHotkeyClick(x, y)) return;
        if (PtIn(ClickerBannerKeyRect(), x, y)) { CaptureGlobalHotkey(); return; }
        if (PtIn(ClickerLeftRadioRect(), x, y)) { clickerSettings_.button = quickscript::MouseButtonChoice::Left; RegisterAllHotkeys(); }
        else if (PtIn(ClickerMiddleRadioRect(), x, y)) { clickerSettings_.button = quickscript::MouseButtonChoice::Middle; RegisterAllHotkeys(); }
        else if (PtIn(ClickerRightRadioRect(), x, y)) { clickerSettings_.button = quickscript::MouseButtonChoice::Right; RegisterAllHotkeys(); }
        else if (PtIn(CreateRect(), x, y)) return;
        else if (clickerDropPopupKind_ >= 0) {
            CloseClickerDropPopup();
            return;
        }
        else return;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CaptureRecordingHotkey(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        BeginHotkeyCaptureRelease(recordings_[static_cast<size_t>(index)].hotkey);
        HotkeyCapture cap; Hotkey out;
        const bool ok = cap.Show(hwnd_, recordings_[static_cast<size_t>(index)].hotkey, true, out,
                false, appSettings_.other.holdThresholdSeconds);
        if (ok) {
            if (out.enabled && out.vk) {
                std::wstring conflict;
                if (HotkeyChordConflicts(out.vk, out.modifiers,
                        recordings_[static_cast<size_t>(index)].path, false, conflict)) {
                    EndHotkeyCaptureRelease();
                    promptModal_.ShowInfo(conflict);
                    return;
                }
            }
            recordings_[static_cast<size_t>(index)].hotkey = out;
            PersistRecordingHotkey(index);
            RegisterAllHotkeys();
        }
        EndHotkeyCaptureRelease();
        if (ok) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void PersistRecordingHotkey(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        const ScriptMeta backup = recordings_[static_cast<size_t>(index)];
        const auto oldActions = actions_;
        const auto oldPath = currentPath_;
        const auto oldName = GetText(name_);
        const auto oldTime = currentRecordTime_;
        const int oldIndex = currentScriptIndex_;
        currentPath_ = backup.path;
        LoadScriptFile(currentPath_);
        SetText(name_, backup.name);
        currentRecordTime_ = backup.recordTime;
        saveDurationSeconds_ = backup.durationSeconds;
        saveHotkeyOverride_ = backup.hotkey;
        SaveScriptFile(currentPath_);
        saveHotkeyOverride_.reset();
        saveDurationSeconds_ = 0;
        actions_ = oldActions;
        currentPath_ = oldPath;
        SetText(name_, oldName);
        currentRecordTime_ = oldTime;
        currentScriptIndex_ = oldIndex;
        LoadRecordings();
    }

    void PersistRecordingRename(int index, const std::wstring& newName) {
        if (index < 0 || index >= static_cast<int>(recordings_.size()) || newName.empty()) return;
        ScriptMeta backup = recordings_[static_cast<size_t>(index)];
        const auto oldActions = actions_;
        const auto oldPath = currentPath_;
        const auto oldName = GetText(name_);
        const auto oldTime = currentRecordTime_;
        const int oldIndex = currentScriptIndex_;
        currentPath_ = backup.path;
        LoadScriptFile(currentPath_);
        SetText(name_, newName);
        currentRecordTime_ = backup.recordTime;
        saveDurationSeconds_ = backup.durationSeconds;
        saveHotkeyOverride_ = backup.hotkey;
        SaveScriptFile(currentPath_);
        saveHotkeyOverride_.reset();
        saveDurationSeconds_ = 0;
        const std::wstring newPath = RecordingsDir() + L"\\" + newName + L".json";
        if (_wcsicmp(currentPath_.c_str(), newPath.c_str()) != 0) {
            if (GetFileAttributesW(newPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                MoveFileW(currentPath_.c_str(), newPath.c_str());
            }
        }
        actions_ = oldActions;
        currentPath_ = oldPath;
        SetText(name_, oldName);
        currentRecordTime_ = oldTime;
        currentScriptIndex_ = oldIndex;
        LoadRecordings();
    }

    void ShowRenameRecordingDialog(int index);

    void OnRecorderHomeClick(int x, int y) {
        if (PtIn(ImportRect(), x, y)) { ImportScript(); return; }
        if (PtIn(ExportRect(), x, y)) { ExportSelectedRecording(); return; }
        if (PtIn(TimerRect(), x, y)) { ShowScheduledTaskDialog(); return; }
        if (PtIn(RecorderModeRect(), x, y) && !recording_) {
            CycleRecorderInputMode();
            SaveHomeState();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (PtIn(CreateRect(), x, y)) {
            if (PtIn(RecorderBannerKeyRect(), x, y)) { CaptureGlobalHotkey(); return; }
            return;
        }
        if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) {
            RECT thumb = HomeScrollThumbRect();
            homeScrollbarDragging_ = true;
            homeScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateHomeScrollFromThumb(y - homeScrollbarDragOffset_);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
            RECT r = RecordingCardRect(i);
            RECT list = HomeListRect();
            if (r.bottom < list.top || r.top > list.bottom) continue;
            if (!PtIn(r, x, y)) continue;
            if (x >= RecordingHotkeyRect(i).left && x <= RecordingHotkeyRect(i).right && y >= RecordingHotkeyRect(i).top && y <= RecordingHotkeyRect(i).bottom) { CaptureRecordingHotkey(i); return; }
            if (i != selectedRecording_ && i == recordingHover_) {
                if (PtIn(RecordingOptimizeRect(i), x, y)) {
#if (QST_GDI_LEGACY == 1)
                    RecordingOptimizeDialog optimizeDlg;
                    if (optimizeDlg.Show(hwnd_, recordings_[static_cast<size_t>(i)]).saved) {
                        LoadRecordings();
                    }
#endif
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                if (PtIn(RecordingRenameRect(i), x, y)) {
                    ShowRenameRecordingDialog(i);
                    return;
                }
                if (PtIn(RecordingDeleteRect(i), x, y)) {
                    ConfirmDeleteRecording(i);
                    return;
                }
            }
            selectedRecording_ = (selectedRecording_ == i) ? -1 : i;
            if (selectedRecording_ >= 0) selectedScript_ = -1;  // 与鼠标宏选择互斥
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    void ExportSelectedRecording() {
        if (selectedRecording_ < 0 || selectedRecording_ >= static_cast<int>(recordings_.size())) {
            ShowPromptInfo(L"请选择要导出的录制。");
            return;
        }
        auto& meta = recordings_[static_cast<size_t>(selectedRecording_)];
        const auto content = ReadAll(meta.path);
        const auto imgPaths = CollectImagePathsFromJson(content);

        // 根据是否包含图片选择导出格式
        if (!imgPaths.empty()) {
            // ZIP 格式导出
            wchar_t fileBuffer[MAX_PATH + 128]{};
            const auto defaultName = meta.name + L".zip";
            wcsncpy_s(fileBuffer, defaultName.c_str(), defaultName.size());
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFile = fileBuffer;
            ofn.nMaxFile = MAX_PATH + 128;
            ofn.lpstrFilter = L"ZIP 脚本包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (!GetSaveFileNameW(&ofn)) return;

            std::wstring zipPath(fileBuffer);
            if (zipPath.size() < 4 || _wcsicmp(zipPath.substr(zipPath.size() - 4).c_str(), L".zip") != 0) {
                zipPath += L".zip";
            }
            std::vector<std::pair<std::wstring, std::wstring>> files;
            files.push_back({L"script.json", meta.path});
            for (const auto& imgPath : imgPaths) {
                const auto slashPos = imgPath.find_last_of(L"\\/");
                std::wstring imgName = (slashPos == std::wstring::npos) ? imgPath : imgPath.substr(slashPos + 1);
                files.push_back({imgName, imgPath});
            }
            const auto zipResult = CreateZipFile(zipPath, files, meta.path);
            if (zipResult.success) {
                if (zipResult.skippedFiles.empty()) {
                    ShowPromptInfo(L"导出成功！图片已一同打包。");
                } else {
                    const std::wstring msg = L"导出成功，但有 " + std::to_wstring(zipResult.skippedFiles.size())
                        + L" 张图片未找到已跳过。\n\n对方导入后需要重新截图或选择本地图片。";
                    ShowPromptInfo(msg);
                }
            } else {
                ShowPromptInfo(L"导出失败：无法创建 ZIP 文件，请检查保存路径是否有写入权限。");
            }
        } else {
            // 纯 JSON 导出
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd_;
            std::wstring name = meta.name + L".json";
            std::wstring ext = L"JSON 文件 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
            wchar_t fileBuffer[MAX_PATH + 128]{};
            wcsncpy_s(fileBuffer, name.c_str(), name.size());
            ofn.lpstrFile = fileBuffer;
            ofn.nMaxFile = MAX_PATH + 128;
            ofn.lpstrFilter = ext.c_str();
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (!GetSaveFileNameW(&ofn)) return;
            if (!CopyFileW(meta.path.c_str(), fileBuffer, FALSE)) {
                ShowPromptInfo(L"导出失败：无法写入目标文件。");
            } else {
                ShowPromptInfo(L"导出成功！");
            }
        }
    }

    void OnMacroHomeClick(int x, int y) {
        if (PtIn(ImportRect(), x, y)) { ImportScript(); return; }
        if (PtIn(ExportRect(), x, y)) { ExportSelectedScript(); return; }
        if (PtIn(TimerRect(), x, y)) { ShowScheduledTaskDialog(); return; }
        if (selectedScript_ >= 0 && PtIn(CommonHotRect(), x, y)) { CaptureGlobalHotkey(); return; }
        if (selectedScript_ < 0 && PtIn(CreateWordRect(), x, y)) { ShowEditorFor(-1, true); return; }
        if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) {
            RECT thumb = HomeScrollThumbRect();
            homeScrollbarDragging_ = true;
            homeScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateHomeScrollFromThumb(y - homeScrollbarDragOffset_);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
            RECT r = HomeCardRect(i);
            RECT list = HomeListRect();
            if (r.bottom < list.top || r.top > list.bottom) continue;
            if (x < r.left || x > r.right || y < r.top || y > r.bottom) continue;
            RECT hot = ScriptHotkeyRect(i);
            RECT edit = HomeCardEditBtn(r);
            RECT del = HomeCardDeleteBtn(r);
            if (i == selectedScript_ && x >= edit.left && x <= edit.right && y >= edit.top && y <= edit.bottom) {
                selectedScript_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (i == homeHover_) {
                if (i != selectedScript_ && x >= edit.left && x <= edit.right && y >= edit.top && y <= edit.bottom) {
                    ShowEditorFor(i, false);
                    return;
                }
                if (x >= del.left && x <= del.right && y >= del.top && y <= del.bottom) {
                    ConfirmDelete(i);
                    return;
                }
            }
            if (x >= hot.left && x <= hot.right && y >= hot.top && y <= hot.bottom) { CaptureScriptHotkey(i); return; }
            selectedScript_ = selectedScript_ == i ? -1 : i;
            if (selectedScript_ >= 0) selectedRecording_ = -1;  // 与键鼠录制选择互斥
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    void OnHomeClick(int x, int y) {
        if (HandleHomeNavClick(x, y)) return;
        if (activeHomeTab_ == quickscript::MainTab::Clicker) { OnClickerHomeClick(x, y); return; }
        if (activeHomeTab_ == quickscript::MainTab::Recorder) { OnRecorderHomeClick(x, y); return; }
        if (activeHomeTab_ == quickscript::MainTab::ScriptCustom) {
            OnScriptCustomHomeClick(x, y);
            return;
        }
        OnMacroHomeClick(x, y);
    }

    void OnScriptCustomHomeClick(int x, int y) {
        if (PtIn(CreateRect(), x, y)) {
            OpenAgentDialog(-1);
            return;
        }
        if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) {
            RECT thumb = HomeScrollThumbRect();
            homeScrollbarDragging_ = true;
            homeScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateHomeScrollFromThumb(y - homeScrollbarDragOffset_);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        for (int i = 0; i < static_cast<int>(agentConversations_.size()); ++i) {
            RECT r = HomeCardRect(i);
            RECT list = HomeListRect();
            if (r.bottom < list.top || r.top > list.bottom) continue;
            if (PtIn(AgentConvChatRect(i), x, y)) { OpenAgentDialog(i); return; }
            if (PtIn(AgentConvDeleteRect(i), x, y)) { ConfirmDeleteAgentConversation(i); return; }
        }
    }

    void CaptureScriptHotkey(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        BeginHotkeyCaptureRelease(scripts_[static_cast<size_t>(index)].hotkey);
        HotkeyCapture cap; Hotkey out;
        const bool ok = cap.Show(hwnd_, scripts_[static_cast<size_t>(index)].hotkey, true, out,
                false, appSettings_.other.holdThresholdSeconds);
        if (ok) {
            if (out.enabled && out.vk) {
                std::wstring conflict;
                if (HotkeyChordConflicts(out.vk, out.modifiers,
                        scripts_[static_cast<size_t>(index)].path, false, conflict)) {
                    EndHotkeyCaptureRelease();
                    promptModal_.ShowInfo(conflict);
                    return;
                }
            }
            scripts_[static_cast<size_t>(index)].hotkey = out;
            PersistScriptHotkey(index);
        }
        EndHotkeyCaptureRelease();
        if (ok) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void PersistScriptHotkey(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        auto oldActions = actions_; auto oldPath = currentPath_; auto oldName = GetText(name_); auto oldTime = currentRecordTime_; int oldIndex = currentScriptIndex_;
        currentScriptIndex_ = index; currentPath_ = scripts_[static_cast<size_t>(index)].path; LoadScriptFile(currentPath_); SaveScriptFile(currentPath_);
        actions_ = oldActions; currentPath_ = oldPath; SetText(name_, oldName); currentRecordTime_ = oldTime; currentScriptIndex_ = oldIndex;
    }

    void ConfirmDelete(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        pendingDeleteIndex_ = index;
        const std::wstring name = scripts_[static_cast<size_t>(index)].name;
        promptModal_.ShowConfirm(L"您确定要删除宏 \"" + name + L"\"\n吗？", [this](bool ok) {
            if (ok) ExecutePendingDelete();
            else pendingDeleteIndex_ = -1;
            StDiscardSpuriousInputAfterModal(hwnd_);
        });
    }

    void ExecutePendingDelete() {
        if (pendingDeleteIndex_ >= 0 && pendingDeleteIndex_ < static_cast<int>(scripts_.size())) {
            const auto scriptPath = scripts_[static_cast<size_t>(pendingDeleteIndex_)].path;
            DeleteUnreferencedImagesOfScript(scriptPath);
            DeleteFileW(scriptPath.c_str());
            selectedScript_ = -1;
            pendingDeleteIndex_ = -1;
            LoadScripts();
            ClampHomeScroll();
            RegisterAllHotkeys();
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void ConfirmDeleteRecording(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        pendingRecordingDeleteIndex_ = index;
        const std::wstring name = recordings_[static_cast<size_t>(index)].name;
        promptModal_.ShowConfirm(L"您确定要删除录制 \"" + name + L"\"\n吗？", [this](bool ok) {
            if (ok) ExecutePendingRecordingDelete();
            else pendingRecordingDeleteIndex_ = -1;
            StDiscardSpuriousInputAfterModal(hwnd_);
        });
    }

    void ExecutePendingRecordingDelete() {
        if (pendingRecordingDeleteIndex_ >= 0
            && pendingRecordingDeleteIndex_ < static_cast<int>(recordings_.size())) {
            const auto recPath = recordings_[static_cast<size_t>(pendingRecordingDeleteIndex_)].path;
            DeleteUnreferencedImagesOfScript(recPath);
            DeleteFileW(recPath.c_str());
            if (selectedRecording_ == pendingRecordingDeleteIndex_) selectedRecording_ = -1;
            else if (selectedRecording_ > pendingRecordingDeleteIndex_) --selectedRecording_;
            pendingRecordingDeleteIndex_ = -1;
            LoadRecordings();
            ClampRecordingScroll();
            RegisterAllHotkeys();
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void ShowHotkeyMenuAt(RECT anchor) {
        const std::vector<ThemedPopupMenuItem> items{
            {kHotCustom, L"自定义"},
            {kHotLeft, L"鼠标左键"},
            {kHotMiddle, L"鼠标中键"},
            {kHotRight, L"鼠标右键"},
            {kHotX1, L"鼠标侧键1"},
            {kHotX2, L"鼠标侧键2"},
            {kHotSpace, L"空格键"},
        };
        POINT pt{anchor.left, anchor.bottom};
        ClientToScreen(hwnd_, &pt);
        const int id = ThemedPopupMenu::Show(hwnd_, pt, items);
        if (id != 0) SetCommonHotkeyFromMenu(id);
    }

    void ShowCommonHotkeyMenu() { ShowHotkeyMenuAt(CommonHotRect()); }

    void SetCommonHotkeyFromMenu(int id) {
        if (id == kHotCustom) {
            BeginHotkeyCaptureRelease(globalHotkey_);
            HotkeyCapture cap; Hotkey out;
            if (cap.Show(hwnd_, globalHotkey_, false, out, true,
                    appSettings_.other.holdThresholdSeconds))
                globalHotkey_ = out;
            EndHotkeyCaptureRelease();
        }
        else if (id == kHotLeft) globalHotkey_ = Hotkey{0, VK_LBUTTON, L"鼠标左键", true, false};
        else if (id == kHotMiddle) globalHotkey_ = Hotkey{0, VK_MBUTTON, L"鼠标中键", true, false};
        else if (id == kHotRight) globalHotkey_ = Hotkey{0, VK_RBUTTON, L"鼠标右键", true, false};
        else if (id == kHotX1) globalHotkey_ = Hotkey{0, VK_XBUTTON1, L"鼠标侧键1", true, false};
        else if (id == kHotX2) globalHotkey_ = Hotkey{0, VK_XBUTTON2, L"鼠标侧键2", true, false};
        else if (id == kHotSpace) globalHotkey_ = Hotkey{0, VK_SPACE, L"空格键", true, false};
        if (id != kHotCustom) RegisterAllHotkeys();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    /// 设置热键弹窗：临时注销/放行「正在编辑」的键，便于同键短按↔长按切换；其它占用键仍拦截。
    void BeginHotkeyCaptureRelease(const Hotkey& editing);

    /// 动作按键捕获：放行全部键并临时注销 RegisterHotKey，允许录入与启停热键相同的键。
    void BeginActionKeyCaptureRelease() {
        ghHotkeyCaptureOpen = true;
        ghHotkeyCapturePassAll = true;
        ghHotkeyCaptureIgnoreVk = 0;
        ClearAllHoldSessionLatches();
        ghHotkeyNeedKeyUp = false;
        if (!hwnd_) return;
        UnregisterHotKey(hwnd_, HOTKEY_GLOBAL_ID);
        for (int i = 0; i < 100; ++i) UnregisterHotKey(hwnd_, HOTKEY_SCRIPT_BASE + i);
        for (int i = 0; i < 100; ++i) UnregisterHotKey(hwnd_, HOTKEY_RECORDING_BASE + i);
    }

    void EndHotkeyCaptureRelease();

    void RegisterAllHotkeys();

    /// 回放期间注销 RegisterHotKey：系统会吞掉同键 SendInput；物理启停改走 LL 钩子。
    void SuspendHotkeysForPlayback();

    void ResumeHotkeysAfterPlayback();

    void InstallGlobalHotkeyHooks();

    void UninstallGlobalHotkeyHooks();

    void RefreshGlobalHotkeyHooks();

    /// LL 钩子存活看门狗（WM_TIMER 每 10s）：检测 Windows 静默卸载后重装。
    void TickHotkeyHookWatchdog();

    /// 全局启停热键「启动」：严格按当前主界面 Tab 分流，互不串用对方选中项。
    /// 选中缓存（selectedScript_/selectedRecording_）仍可按原逻辑保留，但不跨 Tab 触发。
    bool TryStartByActiveHomeTab() {
        using quickscript::MainTab;
        switch (activeHomeTab_) {
        case MainTab::Macro:
            if (selectedScript_ >= 0 && selectedScript_ < static_cast<int>(scripts_.size())) {
                RunScriptByIndex(selectedScript_);
                return true;
            }
            return false;
        case MainTab::Recorder:
            if (selectedRecording_ >= 0 && selectedRecording_ < static_cast<int>(recordings_.size())) {
                // 必须带磁盘路径启动：LoadScriptFile 不写 currentPath_，
                // RunCurrentActions 会把空路径当成鼠标宏，录制页倍速不会生效。
                RunRecordingByIndex(selectedRecording_);
                return true;
            }
            ToggleRecording();
            return true;
        case MainTab::Clicker:
        case MainTab::ScriptCustom:
        default:
            return false;
        }
    }

    void OnHotkey(int id, int holdCmd = 0);

    void RunScriptByIndex(int index);
    void RunRecordingByIndex(int index);

    /// 热键撞车检测：excludePath 非空时跳过该脚本/录制文件；excludeGlobal 跳过全局。
    /// 返回 true 表示冲突，errOut 为说明。
    bool HotkeyChordConflicts(UINT vk, UINT modifiers, const std::wstring& excludePath,
        bool excludeGlobal, std::wstring& errOut) const;

#if (QST_GDI_LEGACY == 1)
    void ShowScheduledTaskDialog() {
        ScheduledTaskDialog dlg;
        dlg.Show(hwnd_, scheduledTasks_);
        if (IsWindow(hwnd_)) {
            SetForegroundWindow(hwnd_);
        }
        StDiscardSpuriousInputAfterModal(hwnd_);
    }
#endif

#if (QST_GDI_LEGACY == 1)
    void OpenAgentDialog(int restoreIndex = -1) {
        agentDialogs_.erase(
            std::remove_if(agentDialogs_.begin(), agentDialogs_.end(),
                [](const std::unique_ptr<AgentDialog>& d) { return !d || !d->IsAlive(); }),
            agentDialogs_.end());

        LoadAppSettings(appSettings_);

        AgentDialog::RestoreData restore;
        const AgentDialog::RestoreData* restorePtr = nullptr;
        if (restoreIndex >= 0 && restoreIndex < static_cast<int>(agentConversations_.size())) {
            AgentConversationRecord rec;
            if (LoadAgentConversationRecord(agentConversations_[static_cast<size_t>(restoreIndex)].id, rec)) {
                restore.id = rec.meta.id;
                restore.name = rec.meta.name;
                restore.createdTime = rec.meta.createdTime;
                restore.messages = std::move(rec.messages);
                restore.chatDisplay = std::move(rec.chatDisplay);
                restorePtr = &restore;
            }
        }

        auto dlg = std::make_unique<AgentDialog>();
        if (!dlg->Show(hwnd_, appSettings_.ai, restorePtr,
            [this](AgentConversationSavePayload&& payload) {
                OnAgentConversationClosed(std::move(payload));
            })) return;
        agentDialogs_.push_back(std::move(dlg));
    }
#endif

#if (QST_GDI_LEGACY != 1)
    void ShowScheduledTaskDialog() {}
    void OpenAgentDialog(int = -1) {}
    void ShowSettingsDialog() {}
    void ShowOcrInstallDialog() {}
#endif

    void RefreshScriptLibraryUi();

    bool IsEditorWindowModeActive() const;

    bool IsEditorMacroHeaderHwnd(HWND hwnd) const {
        return hwnd == labelMacro_ || hwnd == name_ || hwnd == mode_
            || hwnd == labelBreakoutTime_ || hwnd == breakoutTimeEdit_
            || hwnd == wmSelectMethod_ || hwnd == wmSpecifyWindowBtn_
            || hwnd == wmTargetPathEdit_ || hwnd == wmTargetBrowseBtn_ || hwnd == wmTargetCrosshairBtn_
            || hwnd == wmFakeFocusCheck_;
    }

    RECT wmModeLabelRect_{};
    RECT wmSelectMethodLabelRect_{};

    int ScaleEditorX(int designX) const {
        return ScaleX(designX);
    }

    int ScaleEditorY(int designY) const {
        return ScaleY(designY);
    }

    int ScaleEditorW(int designW) const {
        return std::max(1, ScaleX(designW));
    }

    int ScaleEditorH(int designH) const {
        return std::max(1, ScaleY(designH));
    }

    RECT EditorMacroHeaderTextRect() const {
        if (labelMacro_) return WindowClientRect(labelMacro_);
        const int y = ScaleEditorY(kEditorMacroHeaderRowY);
        const int h = ScaleEditorH(kEditorMacroHeaderRowH);
        return RECT{0, y, 0, y + h};
    }

    RECT EditorRowLabelTextRect(int rowY, int rowH) const {
        const int left = ScaleEditorX(kEditorMacroNameLabelX);
        const int right = ScaleEditorX(kEditorMacroNameEditX) - ScaleEditorX(2);
        return RECT{left, rowY, right, rowY + rowH};
    }

    void UpdateEditorWindowModeChrome();

    void SyncScriptWindowModeFromEditor();

    void SyncWindowModeUiFromScript() {
        popupMode_.sel = !scriptWindowMode_.enabled ? 0
            : (scriptWindowMode_.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow ? 2 : 1);
        SetPopupSel(popupMode_, mode_, popupMode_.sel);
        const int methodSel = windowmode::SelectMethodToComboIndex(scriptWindowMode_.selectMethod);
        if (methodSel >= 0 && methodSel < static_cast<int>(popupWmSelectMethod_.items.size())) {
            popupWmSelectMethod_.sel = methodSel;
        }
        if (wmSelectMethod_ && popupWmSelectMethod_.sel >= 0
            && popupWmSelectMethod_.sel < static_cast<int>(popupWmSelectMethod_.items.size())) {
            SetText(wmSelectMethod_, popupWmSelectMethod_.items[static_cast<size_t>(popupWmSelectMethod_.sel)]);
        }
        if (wmTargetPathEdit_) SetText(wmTargetPathEdit_, scriptWindowMode_.targetExePath);
        if (wmFakeFocusCheck_) {
            SetParamCheckboxChecked(wmFakeFocusCheck_,
                scriptWindowMode_.fakeFocusEnabled, false);
        }
        if (page_ == Page::Editor) UpdateEditorWindowModeChrome();
    }

    void ShowWindowClassPickDialog() {
        windowmode::WindowPickResult pick{};
        pick.windowTitle = scriptWindowMode_.windowName;
        pick.windowClassName = scriptWindowMode_.windowClassName;
        pick.childWindowClassName = scriptWindowMode_.childWindowClassName;
        pick.processPath = scriptWindowMode_.targetExePath;
#if (QST_GDI_LEGACY == 1)
        if (!wmPickDialog_.Show(hwnd_, pick)) return;
#else
        return;
#endif
        scriptWindowMode_.windowName = pick.windowTitle;
        scriptWindowMode_.windowClassName = pick.windowClassName;
        scriptWindowMode_.childWindowClassName = pick.childWindowClassName;
        scriptWindowMode_.targetWindowTitle = pick.windowTitle;
        scriptWindowMode_.targetPickX = pick.pickX;
        scriptWindowMode_.targetPickY = pick.pickY;
        if (!pick.processPath.empty()) scriptWindowMode_.targetExePath = pick.processPath;
        if (!pick.documentPath.empty()) {
            scriptWindowMode_.launchArgs = L"\"" + pick.documentPath + L"\"";
        }
        windowmode::AnnotateInputStrategyForSave(scriptWindowMode_);
        if (wmTargetPathEdit_ && !pick.processPath.empty()) SetText(wmTargetPathEdit_, pick.processPath);
        SyncScriptWindowModeFromEditor();
    }

    void FinalizeWindowModeAutoLaunch(windowmode::WindowModeScriptConfig& cfg) const {
        cfg.autoLaunchTarget = windowmode::ShouldAutoLaunchTarget(cfg);
    }

    static void ApplyWindowPickToConfig(windowmode::WindowModeScriptConfig& cfg,
        const windowmode::WindowPickResult& pick) {
        cfg.windowName = pick.windowTitle;
        cfg.windowClassName = pick.windowClassName;
        cfg.childWindowClassName = pick.childWindowClassName;
        cfg.targetWindowTitle = pick.windowTitle;
        cfg.targetPickX = pick.pickX;
        cfg.targetPickY = pick.pickY;
        if (!pick.processPath.empty()) cfg.targetExePath = pick.processPath;
        if (!pick.documentPath.empty()) {
            cfg.launchArgs = L"\"" + pick.documentPath + L"\"";
        } else if (cfg.launchArgs.empty() && !pick.windowTitle.empty()) {
            // 拾取时没有完整路径时，至少把标题里的文件名写入 launchArgs，运行时再按脚本目录解析。
            std::wstring stem = pick.windowTitle;
            const auto dash = stem.find(L" - ");
            if (dash != std::wstring::npos) stem = stem.substr(0, dash);
            while (!stem.empty() && (stem.front() == L'*' || stem.front() == L' ')) stem.erase(stem.begin());
            while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'\t')) stem.pop_back();
            if (!stem.empty() && stem.find(L'.') != std::wstring::npos
                && stem != L"无标题" && _wcsicmp(stem.c_str(), L"Untitled") != 0) {
                cfg.launchArgs = L"\"" + stem + L"\"";
            }
        }
        windowmode::AnnotateInputStrategyForSave(cfg);
    }

    static void ApplyWindowInfoToConfig(windowmode::WindowModeScriptConfig& cfg,
        const WindowInfoFromPoint& info) {
        if (!info.windowTitle.empty()) cfg.windowName = info.windowTitle;
        if (!info.windowClassName.empty()) cfg.windowClassName = info.windowClassName;
        if (!info.childWindowClassName.empty()) cfg.childWindowClassName = info.childWindowClassName;
        if (!info.processPath.empty()) cfg.targetExePath = info.processPath;
        if (!info.documentPath.empty()) cfg.launchArgs = L"\"" + info.documentPath + L"\"";
        cfg.targetWindowTitle = cfg.windowName;
        cfg.targetPickX = info.x;
        cfg.targetPickY = info.y;
        windowmode::AnnotateInputStrategyForSave(cfg);
    }

    bool IsOwnProcessWindowAt(int x, int y) const {
        POINT pt{x, y};
        HWND pointHwnd = WindowFromPoint(pt);
        if (!pointHwnd) return false;
        HWND root = GetAncestor(pointHwnd, GA_ROOT);
        if (!root) root = pointHwnd;
        DWORD pid = 0;
        GetWindowThreadProcessId(root, &pid);
        return pid != 0 && pid == GetCurrentProcessId();
    }

    void PumpMessagesFor(DWORD ms) const {
        const DWORD deadline = GetTickCount() + ms;
        while (static_cast<int>(deadline - GetTickCount()) > 0) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(msg.wParam));
                    return;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(30);
        }
    }

    /// 解析「选择窗口方式」：填充 cfg 的窗口身份字段。可在编辑器运行 / 热键 / 定时任务路径复用。
    bool ResolveWindowModeSelectMethod(windowmode::WindowModeScriptConfig& cfg);

    bool PrepareWindowModeRunConfig(windowmode::WindowModeScriptConfig& cfg);

#if (QST_GDI_LEGACY == 1)
    void ShowSettingsDialog() {
        if (settingsDialog_ && settingsDialog_->IsAlive()) {
            SetForegroundWindow(settingsDialog_->Hwnd());
            return;
        }
        settingsDialog_.reset();
        // 始终从磁盘加载最新设置后再打开，确保 AI 助手修改后立即可见
        LoadAppSettings(appSettings_);
        settingsDialog_ = std::make_unique<SettingsDialog>();
        if (!settingsDialog_->Show(hwnd_, appSettings_, [this]() {
            ApplyThemeAndRefreshUi();
            ApplyDebugWindowSetting();
            ApplyOtherOsSettings();
        })) {
            settingsDialog_.reset();
        }
    }
#endif

    void RunActionsFromPath(const std::wstring& path) {
        if (running_ && workerFinished_.load(std::memory_order_relaxed)) OnRunDone();
        std::wstring resolved;
        if (running_ || path.empty() || !ResolveLibraryScriptPath(path, resolved)) {
            nextRunFromScheduled_ = false;
            if (!path.empty() && !running_) {
                const std::wstring msg = L"运行失败：找不到脚本（可能已拖入专业模式文件夹） " + path;
                AppendDebugLog(msg);
                windowmode::WindowModeLog(msg);
            }
            return;
        }
        const ScriptFileData data = LoadScriptFileData(resolved, false);
        if (data.actions.empty()) {
            nextRunFromScheduled_ = false;
            return;
        }
        windowmode::WindowModeScriptConfig wmCfg = data.windowMode;
        bool anyRel = wmCfg.windowRelativeCoordinates;
        for (const auto& a : data.actions) {
            if (a.windowRelative) { anyRel = true; break; }
        }
        windowmode::FinalizeWindowModeForPlayback(wmCfg, anyRel, IsRecordingScriptPath(resolved));
        if (!ResolveWindowModeSelectMethod(wmCfg)) {
            nextRunFromScheduled_ = false;
            return;
        }

        CoordMeta execMeta = ScriptCoordMetaForExecution(data.coordMeta);
        std::vector<ScriptAction> execActions =
            PrepareScriptActionsForExecution(data.actions, execMeta);
        if (IsRecordingScriptPath(resolved) || ScriptIsTimedInputSequence(execActions))
            RepairCompressedRelativeGaps(execActions);

        const double breakoutTime = EffectiveBreakoutTimeSeconds(data);
        StartActionsWorker(execActions, resolved, wmCfg, execMeta, breakoutTime,
            data.hotkey);
    }

    int RandomInt(int maxValue) { if (maxValue <= 0) return 0; std::uniform_int_distribution<int> dist(-maxValue, maxValue); return dist(rng_); }
    double RandomDelay(double maxValue) { if (maxValue <= 0) return 0; std::uniform_real_distribution<double> dist(0.0, maxValue); return dist(rng_); }
    double ParseBreakoutTimeFromEditor() const {
        if (!breakoutTimeEdit_) return 0;
        const std::wstring text = Trim(GetText(breakoutTimeEdit_));
        if (text.empty()) return 0;
        try {
            size_t consumed = 0;
            const double value = std::stod(text, &consumed);
            if (consumed == 0) return 0;
            return NormalizeBreakoutTimeSeconds(value);
        } catch (...) {
            return 0;
        }
    }

    static std::wstring FormatBreakoutTimeForEditor(double seconds) {
        if (seconds <= 0) return L"0";
        std::wstringstream ss;
        ss << seconds;
        std::wstring out = ss.str();
        if (auto dot = out.find(L'.'); dot != std::wstring::npos) {
            while (!out.empty() && out.back() == L'0') out.pop_back();
            if (!out.empty() && out.back() == L'.') out.pop_back();
        }
        return out.empty() ? L"0" : out;
    }

    bool BreakoutTriggered() const {
        return workerBreakoutTime_ > 0
            && breakoutUserInput_.load(std::memory_order_relaxed);
    }

    bool StopRequested() const {
        return stopFlag_.load(std::memory_order_relaxed)
            || ghEmergencyStop.load(std::memory_order_relaxed);
    }

    void SleepInterruptible(double seconds) {
        if (seconds <= 0.0 || StopRequested()) return;
        using clock = std::chrono::steady_clock;
        auto end = clock::now()
            + std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(seconds));
        while (!StopRequested() && !BreakoutTriggered()) {
            const auto now = clock::now();
            if (now >= end) break;
            if (scheduledYieldRequested_.load(std::memory_order_acquire) && scheduledYieldHook_) {
                const auto rem = end - now;
                scheduledYieldHook_();
                if (StopRequested() || BreakoutTriggered()) return;
                end = clock::now() + rem;
                continue;
            }
            const auto rem = end - now;
            // 旧实现固定 sleep 10ms：短等待（录制相对移动常见 4~8ms）会被拖糊成台阶感
            if (rem > std::chrono::milliseconds(2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // rem ≤ 2ms：忙等收尾，保证亚毫秒级对齐
        }
    }

    std::wstring ResolveQuickInputText(const std::wstring& text, bool parseEscapes = false) {
        MacroVariableContext ctx;
        ctx.matchVars = &matchVars_;
        ctx.matchListVars = &matchListVars_;
        ctx.ocrVars = workerUsesOcrVars_ ? &ocrVars_ : nullptr;
        ctx.aiVars = &aiVars_;
        ctx.userVars = &userVars_;
        ctx.loopVars = &loopVars_;
        ctx.timerStarts = &timerStarts_;
        ctx.curLoops = curLoops_;
        return ::ResolveQuickInputText(text, ctx, parseEscapes);
    }

    void SendKey(UINT vk, bool down) {
        // 允许与长按热键同 VK 注入（连点热键本身）；假抬起靠 ExtraInfo/INJECTED/Expect 过滤。
        if (running_) MarkSimulatedInput();
        SendKeyboardKey(vk, down);
        if (running_) UnmarkSimulatedInput();
    }

    void MarkSimulatedInput() {
        simulatingInputDepth_.fetch_add(1, std::memory_order_relaxed);
    }

    void UnmarkSimulatedInput() {
        simulatingInputDepth_.fetch_sub(1, std::memory_order_relaxed);
    }

    bool ShouldIgnoreHotkeyStop() const {
        return simulatingInputDepth_.load(std::memory_order_relaxed) > 0;
    }

    /// 输入框聚焦或「中文输入法不触发热键」：仅挡空闲启动（停止不受影响）。
    bool ShouldSuppressHotkeyWhileTyping() const {
        if (ghHotkeySessionBusy.load(std::memory_order_relaxed)) return false;
        if (UiTypingHotkeysMuted()) return true;
        if (!appSettings_.other.resolveImeConflict) return false;
        return IsChineseImeActiveForHotkeyPass();
    }

    void SendHeldModifiers(const ScriptAction& a, bool down) {
        if (a.holdLeftWin) SendKey(VK_LWIN, down);
        if (a.holdRightWin) SendKey(VK_RWIN, down);
        if (a.holdLeftCtrl) SendKey(VK_LCONTROL, down);
        if (a.holdRightCtrl) SendKey(VK_RCONTROL, down);
        if (a.holdLeftAlt) SendKey(VK_LMENU, down);
        if (a.holdRightAlt) SendKey(VK_RMENU, down);
        if (a.holdLeftShift) SendKey(VK_LSHIFT, down);
        if (a.holdRightShift) SendKey(VK_RSHIFT, down);
    }

    // ── Script execution (worker thread) ───────────────────────────
    void SyncFormIntoActionsBeforeRun();

    void RunCurrentActions();

    void StartActionsWorker(const std::vector<ScriptAction>& actions, const std::wstring& selfPath, const windowmode::WindowModeScriptConfig& wmCfg = {}, const CoordMeta& execCoordMeta = {}, double breakoutTime = 0, const Hotkey& scriptHotkey = {}, int debugStartIndex = -1, bool debugStepMode = false, const std::vector<int>* debugBreakpoints = nullptr, const Hotkey& debugHotkey = {});

    void StopRun();
    void OnScheduledTaskFire(const std::wstring& path);
    void EnqueueScheduledPath(const std::wstring& path);
    void TryStartPendingScheduled();
    void RequestScheduledYield(const std::wstring& path);
    bool TakeScheduledYieldPath(std::wstring& out);
    void ClearScheduledYield();
    void DeferScheduledPath(const std::wstring& path);
    std::wstring TakeDeferredScheduledPath();
    void ReleaseAllHeldInputs() {
        if (running_) MarkSimulatedInput();
        const UINT modKeys[] = { VK_LWIN, VK_RWIN, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT };
        for (UINT vk : modKeys) {
            if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) SendKey(vk, false);
        }
        struct { int vk; MouseButtonType button; } mouseBtns[] = {
            { VK_LBUTTON, MouseButtonType::Left },
            { VK_RBUTTON, MouseButtonType::Right },
            { VK_MBUTTON, MouseButtonType::Middle },
        };
        for (const auto& btn : mouseBtns) {
            if (GetAsyncKeyState(btn.vk) & 0x8000) {
                MouseButtonEvent(btn.button, false);
            }
        }
        if (running_) UnmarkSimulatedInput();
    }
    void ForceEndBreakoutUiState() {
        const bool wasPaused = breakoutPaused_.load(std::memory_order_relaxed);
        const bool hadBreakoutTaskbar = breakoutTaskbarShown_;
        breakoutPaused_ = false;
        breakoutUserInput_ = false;
        breakout_input::BreakoutClearUserHolds();
        if (!hwnd_ || !IsWindow(hwnd_)) {
            breakoutTaskbarShown_ = false;
            breakoutPlacement_ = BreakoutTaskbarPlacement{};
            if (wasPaused || hadBreakoutTaskbar) RestoreIconsAfterBreakout();
            return;
        }
        SetWindowCloaked(hwnd_, false);
        if (breakoutTaskbarShown_) {
            breakoutTaskbarShown_ = false;
            breakoutUiVisibleOnScreen_ = false;
            SetWindowTextW(hwnd_, L"键鼠工坊");
            const LONG_PTR ex = breakoutPlacement_.saved ? breakoutPlacement_.exStyle
                : (runSavedRectValid_ ? runSavedExStyle_ : GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
            if (breakoutPlacement_.saved) {
                const int w = std::max(static_cast<int>(breakoutPlacement_.rect.right - breakoutPlacement_.rect.left), 1);
                const int h = std::max(static_cast<int>(breakoutPlacement_.rect.bottom - breakoutPlacement_.rect.top), 1);
                SetWindowPos(hwnd_, nullptr, breakoutPlacement_.rect.left, breakoutPlacement_.rect.top, w, h,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                if (wasMinimizedBeforeRun_) {
                    ShowWindow(hwnd_, SW_MINIMIZE);
                }
            }
            breakoutPlacement_.saved = false;
        }
        if (wasPaused || hadBreakoutTaskbar) RestoreIconsAfterBreakout();
    }

    void CloseWindowDuringRun() {
        hiddenToTray_ = true;
        EnsureTrayIcon();
        if (breakoutPaused_.load(std::memory_order_relaxed)) {
            if (!breakoutTaskbarShown_) {
                ShowBreakoutTaskbarPresence();
            } else {
                MinimizeBreakoutWindowForUser();
            }
        } else if (appSettings_.other.autoHideMainWindow) {
            HideUserFacingMainWindow(false);
        } else {
            HideToTray();
        }
        UpdateStatusTip();
    }

    void RestoreMainWindowAfterRun();

    void OnRunDone();

    bool NeedsBreakoutWindowRestore() const {
        return !breakoutUiVisibleOnScreen_;
    }

    void MinimizeBreakoutWindowForUser() {
        if (!hwnd_ || !breakoutPaused_.load(std::memory_order_relaxed) || !breakoutTaskbarShown_) return;
        breakoutUiVisibleOnScreen_ = false;
        if (!breakoutPlacement_.saved && runSavedRectValid_) {
            breakoutPlacement_.rect = runSavedRect_;
            breakoutPlacement_.exStyle = runSavedExStyle_;
            breakoutPlacement_.saved = true;
        }
        breakoutTaskbarTransition_ = true;
        MinimizeBreakoutWindowOnTaskbar(hwnd_, &breakoutPlacement_);
        breakoutTaskbarTransition_ = false;
    }

    void ReturnBreakoutWindowToTaskbar() {
        MinimizeBreakoutWindowForUser();
    }

    void ShowBreakoutTaskbarPresence() {
        if (!hwnd_) return;
        breakoutTaskbarShown_ = true;
        breakoutUiVisibleOnScreen_ = false;
        SetWindowTextW(hwnd_, L"键鼠工坊-脱离中");
        if (!breakoutPlacement_.saved && runSavedRectValid_) {
            breakoutPlacement_.rect = runSavedRect_;
            breakoutPlacement_.exStyle = runSavedExStyle_;
            breakoutPlacement_.saved = true;
        }
        // 所有场景统一使用系统最小化按钮；不再 cloak，确保点击必定走 SC_RESTORE。
        breakoutTaskbarTransition_ = true;
        MinimizeBreakoutWindowOnTaskbar(hwnd_, &breakoutPlacement_);
        breakoutTaskbarTransition_ = false;
    }

    void HideBreakoutTaskbarPresence() {
        if (!hwnd_ || !breakoutTaskbarShown_) return;
        breakoutTaskbarShown_ = false;
        breakoutUiVisibleOnScreen_ = false;
        SetWindowTextW(hwnd_, L"键鼠工坊");
        const bool hide = running_
            && (appSettings_.other.autoHideMainWindow || wasMinimizedBeforeRun_);
        HideMainWindowFromTaskbarAfterBreakout(hwnd_, &breakoutPlacement_, hide);
        if (!hide && breakoutPlacement_.saved) {
            const int w = std::max(static_cast<int>(breakoutPlacement_.rect.right - breakoutPlacement_.rect.left), 1);
            const int h = std::max(static_cast<int>(breakoutPlacement_.rect.bottom - breakoutPlacement_.rect.top), 1);
            SetWindowPos(hwnd_, nullptr, breakoutPlacement_.rect.left, breakoutPlacement_.rect.top, w, h,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (wasMinimizedBeforeRun_) {
                ShowWindow(hwnd_, SW_MINIMIZE);
            }
        }
        breakoutPlacement_.saved = false;
    }

    void RestoreBreakoutWindowToScreen() {
        if (!hwnd_) return;
        hiddenToTray_ = false;
        SetWindowCloaked(hwnd_, false);
        if (IsIconic(hwnd_)) {
            ShowWindow(hwnd_, SW_RESTORE);
        }
        RECT rc{};
        if (breakoutPlacement_.saved) rc = breakoutPlacement_.rect;
        else if (runSavedRectValid_) rc = runSavedRect_;
        else GetWindowRect(hwnd_, &rc);
        const int w = std::max(static_cast<int>(rc.right - rc.left), 1);
        const int h = std::max(static_cast<int>(rc.bottom - rc.top), 1);
        if (breakoutPlacement_.saved) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, breakoutPlacement_.exStyle);
        } else if (runSavedRectValid_) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, runSavedExStyle_);
        }
        SetWindowPos(hwnd_, HWND_TOP, rc.left, rc.top, w, h, SWP_SHOWWINDOW);
        ShowWindow(hwnd_, SW_SHOW);
        SetWindowCloaked(hwnd_, false);
        RestoreWindowTaskbarLivePreview(hwnd_);
        TaskbarYieldMessages();
        SwitchToThisWindow(hwnd_, TRUE);
        SetForegroundWindow(hwnd_);
        breakoutUiVisibleOnScreen_ = true;
        if (breakoutPaused_.load(std::memory_order_relaxed) && breakoutTaskbarShown_) {
            ApplyMainBreakoutTaskbarPresentation(hwnd_);
        }
    }

    void ApplyAllBreakoutIcons() {
        if (!hwnd_) return;
        ApplyMainBreakoutTaskbarPresentation(hwnd_);
        EnsureTrayIcon();
    }

    void RestoreIconsAfterBreakout() {
        if (!hwnd_) return;
        ApplyMainWindowNormalTaskbarPresentation(hwnd_);
        if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
            RestoreWindowTaskbarLivePreview(hwnd_);
        } else if (IsIconic(hwnd_)) {
            RefreshWindowTaskbarGrouping(hwnd_);
        }
        EnsureTrayIcon();
    }

    void UpdateBreakoutPauseIcons() {
        if (!hwnd_) return;
        const bool paused = breakoutPaused_.load(std::memory_order_relaxed);
        if (paused) {
            // 脱离状态只改变主窗口；AI、调试等辅助窗口完全保持原任务栏与显示状态。
            ShowBreakoutTaskbarPresence();
            ApplyAllBreakoutIcons();
            UpdateStatusTip();
        } else {
            HideBreakoutTaskbarPresence();
            RestoreIconsAfterBreakout();
            UpdateStatusTip();
        }
    }
    void RemoveTrayIcon() {
        if (!trayActive_) return;
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayActive_ = false;
    }
    void EnsureTrayIcon() {
        // 产品 Web 壳拥有唯一系统托盘；headless Engine 宿主窗不得再 NIM_ADD（否则双图标）
        if (headlessUi_) return;
        if (!hwnd_ || !IsWindow(hwnd_)) return;
        if (!wmTaskbarCreated_) {
            wmTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        }
        HICON icon = breakoutPaused_.load(std::memory_order_relaxed)
            ? LoadBreakoutPauseIconSmall()
            : (running_ ? LoadTrayRunningIconSmall() : LoadAppIconSmall());
        if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
        if (!icon) return;

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = icon;
        if (breakoutPaused_.load(std::memory_order_relaxed)) wcscpy_s(nid.szTip, L"键鼠工坊-脱离中");
        else if (clicking_) wcscpy_s(nid.szTip, L"键鼠工坊-连点运行中");
        else if (recording_) wcscpy_s(nid.szTip, L"键鼠工坊-录制中");
        else if (running_) wcscpy_s(nid.szTip, L"键鼠工坊-运行中");
        else wcscpy_s(nid.szTip, L"键鼠工坊");

        static HICON s_lastIcon = nullptr;
        static wchar_t s_lastTip[128]{};
        if (trayActive_ && icon == s_lastIcon && wcscmp(nid.szTip, s_lastTip) == 0) return;

        // 仅在系统确认成功后标记 trayActive_；MODIFY 失败则回退 ADD（Explorer 清图标后常见）
        if (trayActive_) {
            if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
                trayActive_ = false;
                if (Shell_NotifyIconW(NIM_ADD, &nid)) trayActive_ = true;
            }
        } else if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            trayActive_ = true;
        } else if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            trayActive_ = true;
        }
        if (trayActive_) {
            s_lastIcon = icon;
            wcscpy_s(s_lastTip, nid.szTip);
        }
    }
    void AddTray() { EnsureTrayIcon(); }
    void RemoveTray() {
        // 停止录制/连点后刷新提示；常驻托盘不删除（关闭到托盘依赖它）
        EnsureTrayIcon();
    }

    /// headless：用户可见窗是 Web 壳；GDI：仍是引擎主窗。勿把引擎窗 Show 给用户。
    HWND UserFacingMainHwnd() const {
        if (headlessUi_ && webViewHostHwnd_ && IsWindow(webViewHostHwnd_)) return webViewHostHwnd_;
        return hwnd_;
    }

    void KeepEngineHostHiddenIfHeadless() {
        if (headlessUi_ && hwnd_ && IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
    }

    void HideUserFacingMainWindow(bool prepareLivePreview) {
        HWND ui = UserFacingMainHwnd();
        if (!ui || !IsWindow(ui)) {
            KeepEngineHostHiddenIfHeadless();
            return;
        }
        if (prepareLivePreview && !headlessUi_ && ui == hwnd_) {
            PrepareHwndTaskbarLivePreview(hwnd_);
        }
        ShowWindow(ui, SW_HIDE);
        KeepEngineHostHiddenIfHeadless();
    }

    void ShowUserFacingMainWindow(int showCmd) {
        KeepEngineHostHiddenIfHeadless();
        HWND ui = UserFacingMainHwnd();
        if (!ui || !IsWindow(ui)) return;
        ShowWindow(ui, showCmd);
    }

    void RestoreMainWindowForUser() {
        if (!hwnd_) return;
        if (headlessUi_) {
            HWND show = (webViewHostHwnd_ && IsWindow(webViewHostHwnd_)) ? webViewHostHwnd_ : nullptr;
            if (show) {
                ShowWindow(show, SW_SHOW);
                SetForegroundWindow(show);
            }
            KeepEngineHostHiddenIfHeadless();
            return;
        }
        hiddenToTray_ = false;
        if (breakoutPaused_.load(std::memory_order_relaxed) && breakoutTaskbarShown_) {
            if (!breakoutUiVisibleOnScreen_) {
                RestoreBreakoutWindowToScreen();
            } else {
                SwitchToThisWindow(hwnd_, TRUE);
                SetForegroundWindow(hwnd_);
            }
            return;
        }
        SetWindowCloaked(hwnd_, false);
        if (IsIconic(hwnd_)) {
            ShowWindow(hwnd_, SW_RESTORE);
        } else {
            ShowWindow(hwnd_, SW_SHOW);
        }
        SetForegroundWindow(hwnd_);
        EnsureTrayIcon();
    }

    void QuitApplication() {
        // 可重入保护。
        static std::atomic_bool quitting{false};
        if (quitting.exchange(true)) {
            TerminateProcess(GetCurrentProcess(), 0);
        }

        try {
            windowmode::ExtBridgeServer::Instance().Stop();
        } catch (...) {
        }

        // ExitProcess 可能卡在 VDA/WinRT 的 DLL_PROCESS_DETACH，进程变幽灵。
        // 看门狗与最终退出一律用 TerminateProcess，保证任务管理器里一定消失。
        std::thread([] {
            Sleep(400);
            TerminateProcess(GetCurrentProcess(), 0);
        }).detach();

        // 1) 立刻摘托盘 + 藏窗口。
        RemoveTrayIcon();
        if (hwnd_ && IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
        qst::desktop_tools::MacroDebug().Hide();
        if (statusTipWindow_ && IsWindow(statusTipWindow_)) ShowWindow(statusTipWindow_, SW_HIDE);
#if (QST_GDI_LEGACY == 1)
        for (const auto& dialog : agentDialogs_) {
            if (dialog && dialog->IsAlive()) ShowWindow(dialog->Hwnd(), SW_HIDE);
        }
#endif

        // 2) 只发停止信号，不做 join / Unhook / DestroyWindow。
        stopFlag_ = true;
        clicking_ = false;
        running_ = false;
        g_recording = false;
        recording_ = false;
        aiHttpAbort_.Abort();
        ghHotkeyEnabled = false;

        // 3) 尽力保存（卡住由看门狗 TerminateProcess）。
        try {
            SaveHomeState();
        } catch (...) {
        }

        TerminateProcess(GetCurrentProcess(), 0);
    }

    void RestoreFromTray() {
        // 设置 / 录制优化打开时优先激活对应窗口
#if (QST_GDI_LEGACY == 1)
        if (HWND settings = SettingsDialog::ActiveHwnd()) {
            if (IsIconic(settings)) ShowWindow(settings, SW_RESTORE);
            else ShowWindow(settings, SW_SHOW);
            SwitchToThisWindow(settings, TRUE);
            SetForegroundWindow(settings);
            EnsureTrayIcon();
            return;
        }
        if (HWND opt = RecordingOptimizeDialog::ActiveHwnd()) {
            if (IsIconic(opt)) ShowWindow(opt, SW_RESTORE);
            else ShowWindow(opt, SW_SHOW);
            SwitchToThisWindow(opt, TRUE);
            SetForegroundWindow(opt);
            EnsureTrayIcon();
            return;
        }
#endif
        RestoreMainWindowForUser();
    }
    void HideToTray() {
        hiddenToTray_ = true;
        EnsureTrayIcon();
        HideUserFacingMainWindow(false);
    }
    LRESULT OnTrayMessage(LPARAM lp) {
        const UINT evt = LOWORD(lp);
        if (evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU) {
            POINT pt{};
            if (evt == WM_CONTEXTMENU) {
                pt.x = GET_X_LPARAM(lp);
                pt.y = GET_Y_LPARAM(lp);
            } else {
                GetCursorPos(&pt);
            }
            trayMenuOpen_ = true;
            const TrayMenuAction action = TrayMenu::Show(hwnd_, pt);
            trayMenuOpen_ = false;
            switch (action) {
            case TrayMenuAction::ShowWindow:
                RestoreFromTray();
                break;
            case TrayMenuAction::Exit:
                // 菜单嵌套循环已结束，直接硬退出（不要 PostMessage，避免丢失）。
                QuitApplication();
                break;
            default:
                break;
            }
            return 0;
        }
        if (evt == WM_LBUTTONUP) {
            RestoreFromTray();
            return 0;
        }
        return 0;
    }

    // ── Recording ────────────────────────────────────────────────────
    void ToggleRecording();

    void StartRecording();

    void StopRecording();

    void ConvertRecordedToActions() {
        std::vector<RecordedEvent> events;
        {
            std::lock_guard<std::mutex> lock(g_recordMutex);
            events = g_recordedEvents;
        }
        RecordingConversionResult converted =
            ConvertRecordedEventsToActions(std::move(events), globalHotkey_,
                recorderWindowMode_ && GetRecordingWindowTarget().enabled);
        actions_ = std::move(converted.actions);
        saveDurationSeconds_ = converted.durationSeconds;
        currentRecordingCaptureMode_ = static_cast<int>(recorderSettings_.inputMode);
        currentInputTimingVersion_ = kInputTimingVersionExplicitWaits;
    }

    void SaveRecording() {
        EnsureScriptsDir();
        const std::wstring baseName = L"键鼠录制-" + TimestampName();
        std::wstring name = baseName;
        std::wstring path = RecordingsDir() + L"\\" + name + L".json";
        for (int suffix = 1; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++suffix) {
            name = baseName + L"-" + std::to_wstring(suffix);
            path = RecordingsDir() + L"\\" + name + L".json";
        }
        currentPath_ = path;
        currentRecordTime_ = NowText();
        SetText(name_, name);
        // 录制保存不得沿用「宏编辑」残留的 currentScriptIndex_/热键（否则会出现同名感 + 误带 J 长按）
        currentScriptIndex_ = -1;
        saveHotkeyOverride_ = Hotkey{0, 0, L"", false};
        SaveScriptFile(path);
        lastSavedRecordingPath_ = path;
        saveHotkeyOverride_.reset();
        saveDurationSeconds_ = 0;
        RefreshRunBlockCombo();
        LoadRecordings();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // ── Clicker ──────────────────────────────────────────────────────
    void ToggleClicker();

    void StartClicking();

    void StopClicking();

    // ── Row layout helpers ─────────────────────────────────────────
    RECT RowRect(int index) const {
        const int slot = VisibleSlotOf(index);
        if (slot < 0) return RECT{};
        RECT list = ActionListRect();
        const int rowH = std::max(1, UiLen(kRowH));
        const int y = list.top + (slot - scrollOffset_) * rowH;
        return RECT{list.left, y, ActionListContentRight(), y + rowH};
    }
    bool EditorComboHitTest(int x, int y) {
        const int id = EditorComboPopupIdAtPoint(x, y);
        if (id < 0) return false;
        ToggleEditorPopup(id);
        return true;
    }

    int HitRow(int x, int y) const {
        RECT list = ActionListRect();
        if (x < list.left || x > ActionListContentRight() || y < list.top || y > list.bottom) return -1;
        const int rowH = std::max(1, UiLen(kRowH));
        const int visibleSlot = scrollOffset_ + (y - list.top) / rowH;
        const auto& vis = VisibleActionIndices();
        if (visibleSlot < 0 || visibleSlot >= static_cast<int>(vis.size())) return -1;
        return vis[static_cast<size_t>(visibleSlot)];
    }
    RECT ExpandToggleRect(int index) const {
        if (index < 0 || index >= static_cast<int>(actions_.size())) return RECT{};
        const auto& a = actions_[static_cast<size_t>(index)];
        if (!IsExpandableContainer(a.type)) return RECT{};
        RECT r = RowRect(index);
        const int pad = UiLen(kListInnerPad);
        const int expandLeft = ExpandToggleLeftLocal(a.indent, batchEditMode_, pad);
        const int toggleW = std::max(1, UiLen(kExpandToggleWidth));
        return RECT{r.left + expandLeft, r.top + UiLen(8), r.left + expandLeft + toggleW, r.bottom - UiLen(8)};
    }
    RECT CopyRect(int i) const {
        RECT r = RowRect(i);
        return RECT{r.right - UiLen(104), r.top + UiLen(6), r.right - UiLen(62), r.bottom - UiLen(6)};
    }
    RECT DeleteRect(int i) const {
        RECT r = RowRect(i);
        return RECT{r.right - UiLen(58), r.top + UiLen(6), r.right - UiLen(18), r.bottom - UiLen(6)};
    }
    bool PtIn(RECT r, int x, int y) const { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

    void OnEditorClick(int x, int y) {
        int row = HitRow(x, y); if (row < 0) return;
        if (batchEditMode_) {
            if (IsExpandableContainer(actions_[static_cast<size_t>(row)].type) && PtIn(ExpandToggleRect(row), x, y)) {
                ToggleContainerExpand(row);
                return;
            }
            if (PtIn(CheckboxRect(row), x, y)) ToggleBatchSelection(row);
            return;
        }
        if (PtIn(DeleteRect(row), x, y)) { CommitInlineRemark(); DeleteActionAt(row); return; }
        if (PtIn(CopyRect(row), x, y)) { copySource_ = row; ShowCopyMenu(x, y); return; }
        if (PtIn(RemarkRect(row), x, y)) { BeginInlineRemarkEdit(row); return; }
        if (IsExpandableContainer(actions_[static_cast<size_t>(row)].type) && PtIn(ExpandToggleRect(row), x, y)) { ToggleContainerExpand(row); return; }
        CommitInlineRemark();
        dragIndex_ = row;
        dragTargetIndex_ = row;
        dragTargetIndent_ = actions_[static_cast<size_t>(row)].indent;
        dragTargetNested_ = false;
        dragStartX_ = x;
        dragStartY_ = y;
        dragMoved_ = false;
        dragging_ = true;
        SetCapture(hwnd_);
    }

    // 删除 [row, end) 并同步可见缓存 / 分步解析状态（不做 UI 刷新）
    void EraseEditorActionsRange(int row, int end) {
        const int n = static_cast<int>(actions_.size());
        if (row < 0 || end <= row || end > n) return;
        const int count = end - row;
        // 先按删除前长度裁剪 batchSelected，避免 Sync 按新长度截尾导致漏删
        if (batchEditMode_) {
            if (static_cast<int>(batchSelected_.size()) == n) {
                batchSelected_.erase(batchSelected_.begin() + row, batchSelected_.begin() + end);
            } else {
                batchSelected_.clear();
            }
        }
        if (static_cast<int>(editorActionParsed_.size()) == n) {
            editorActionParsed_.erase(editorActionParsed_.begin() + row, editorActionParsed_.begin() + end);
        }
        if (static_cast<int>(editorActionBlocks_.size()) == n) {
            editorActionBlocks_.erase(editorActionBlocks_.begin() + row, editorActionBlocks_.begin() + end);
        } else if (!editorActionBlocks_.empty()) {
            editorActionBlocks_.clear();
        }
        collapsedContainers_ = RemapCollapsedAfterDelete(collapsedContainers_, row, end);
        actions_.erase(actions_.begin() + row, actions_.begin() + end);
        if (static_cast<int>(editorActionParsed_.size()) != static_cast<int>(actions_.size())) {
            editorActionParsed_.assign(actions_.size(), 1);
        }
        if (batchEditMode_ && batchSelected_.size() != actions_.size()) {
            batchSelected_.assign(actions_.size(), false);
        }
        if (editorActionBlocks_.empty()) {
            editorParsePending_ = false;
            editorParseCursor_ = static_cast<int>(actions_.size());
        } else {
            if (editorParseCursor_ >= end) editorParseCursor_ -= count;
            else if (editorParseCursor_ > row) editorParseCursor_ = row;
            editorParsePending_ = editorParseCursor_ < static_cast<int>(actions_.size());
        }
        if (selectedIndex_ >= row && selectedIndex_ < end) selectedIndex_ = -1;
        else if (selectedIndex_ >= end) selectedIndex_ -= count;
        if (hoverIndex_ >= row && hoverIndex_ < end) hoverIndex_ = -1;
        else if (hoverIndex_ >= end) hoverIndex_ -= count;
        if (editingRemarkIndex_ >= row && editingRemarkIndex_ < end) {
            editingRemarkIndex_ = -1;
            if (listRemarkEdit_) ShowWindow(listRemarkEdit_, SW_HIDE);
        } else if (editingRemarkIndex_ >= end) {
            editingRemarkIndex_ -= count;
        }
        MarkVisibleActionsDirty();
    }

    void DeleteActionAt(int row) {
        if (row < 0 || row >= static_cast<int>(actions_.size())) return;
        const int end = SubtreeEnd(row);
        const bool clearedSelection = selectedIndex_ >= row && selectedIndex_ < end;
        EraseEditorActionsRange(row, end);
        RenumberActions();
        RefreshRunBlockCombo();
        // 删除当前选中项后恢复添加草稿（无草稿则该类型默认值）
        if (clearedSelection && !loadingForm_) RestoreActionFormDraftForCurrentType();
        UpdateEditMode();
        OnActionsChanged();
    }

    DragInsertTarget HitInsertTarget(int x, int y) const {
        DragInsertTarget target{};
        const auto vis = VisibleActionIndices();
        if (actions_.empty()) return target;
        RECT list = ActionListRect();
        const int listH = static_cast<int>(list.bottom - list.top);
        const int rowH = std::max(1, UiLen(kRowH));
        const int relativeY = std::clamp(y - static_cast<int>(list.top), 0, listH);
        const int visibleRow = std::clamp(scrollOffset_ + relativeY / rowH, 0, std::max(0, static_cast<int>(vis.size()) - 1));
        const int inRowY = relativeY % rowH;
        const int slot = std::clamp(visibleRow + (inRowY > rowH / 2 ? 1 : 0), 0, static_cast<int>(vis.size()));
        target.insertIndex = slot >= static_cast<int>(vis.size()) ? static_cast<int>(actions_.size()) : vis[static_cast<size_t>(slot)];
        const int xIndent = IndentFromX(x);
        if (target.insertIndex > 0) {
            int prevIndex = target.insertIndex - 1;
            if (dragging_ && prevIndex >= dragIndex_ && prevIndex < SubtreeEnd(dragIndex_)) {
                prevIndex = dragIndex_ - 1;
            }
            if (prevIndex >= 0) {
                const auto& prev = actions_[static_cast<size_t>(prevIndex)];
                if (IsSubtreeContainer(prev.type) && xIndent > prev.indent) {
                    target.targetIndent = prev.indent + 1;
                    target.nested = true;
                } else if (prev.indent > 0) {
                    const int parentLeft = ActionLeftClient(prev.indent - 1);
                    const int childLeft = ActionLeftClient(prev.indent);
                    const int threshold = (parentLeft + childLeft) / 2;
                    target.targetIndent = x < threshold ? prev.indent - 1 : prev.indent;
                    target.nested = target.targetIndent > 0;
                } else {
                    target.targetIndent = prev.indent;
                }
            }
        }
        // Prevent inserting a peer/reduced-indent item inside a container body (would split the container from its children)
        bool adjusted;
        do {
            adjusted = false;
            for (int i = target.insertIndex - 1; i >= 0; --i) {
                if (IsSubtreeContainer(actions_[i].type)) {
                    const int bodyEnd = ContainerBodyEndIndex(i);
                    if (target.insertIndex > i && target.insertIndex < bodyEnd && target.targetIndent <= actions_[i].indent) {
                        target.insertIndex = bodyEnd;
                        target.targetIndent = actions_[i].indent;
                        target.nested = false;
                        adjusted = true;
                    }
                    break;
                }
            }
        } while (adjusted);
        return target;
    }

    void MoveDragged(int x, int y) {
        if (dragIndex_ < 0) return;
        const DragInsertTarget target = HitInsertTarget(x, y);
        int insertIndex = target.insertIndex;
        const int dragEnd = SubtreeEnd(dragIndex_);
        if (insertIndex > dragIndex_ && insertIndex < dragEnd) insertIndex = dragEnd;
        if ((insertIndex == dragIndex_ || insertIndex == dragEnd) &&
            target.targetIndent == actions_[static_cast<size_t>(dragIndex_)].indent) {
            return;
        }
        if (target.targetIndent != dragTargetIndent_ || insertIndex != dragTargetIndex_ || target.nested != dragTargetNested_) {
            dragTargetIndex_ = insertIndex;
            dragTargetIndent_ = target.targetIndent;
            dragTargetNested_ = target.nested;
            dragMoved_ = true;
        }
    }

    void CompleteDrag() {
        if (dragIndex_ < 0 || dragIndex_ >= static_cast<int>(actions_.size())) return;
        if (!dragMoved_) {
            const int nextSel = selectedIndex_ == dragIndex_ ? -1 : dragIndex_;
            if (nextSel >= 0 && selectedIndex_ < 0 && !loadingForm_) {
                // 选中已添加动作前：只保存当前「添加表单」草稿（须在 LoadForm 列表项之前）
                SaveCurrentActionFormDraft();
            } else if (nextSel < 0 && selectedIndex_ >= 0 && !loadingForm_) {
                selectedIndex_ = -1;
                RestoreActionFormDraftForCurrentType();
                UpdateEditMode();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            selectedIndex_ = nextSel;
            UpdateEditMode();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        const int dragEnd = SubtreeEnd(dragIndex_);
        const int count = dragEnd - dragIndex_;
        if (dragTargetIndex_ > dragIndex_ && dragTargetIndex_ < dragEnd) {
            dragTargetIndex_ = dragEnd;
        }
        std::vector<ScriptAction> block(actions_.begin() + dragIndex_, actions_.begin() + dragEnd);
        const int indentDelta = dragTargetIndent_ - block.front().indent;
        for (auto& action : block) action.indent = std::max(0, action.indent + indentDelta);
        int insertIndex = dragTargetIndex_;
        if (insertIndex > dragIndex_) insertIndex -= count;
        insertIndex = std::clamp(insertIndex, 0, static_cast<int>(actions_.size()));
        {
            std::vector<ScriptAction> trial = actions_;
            trial.erase(trial.begin() + dragIndex_, trial.begin() + dragEnd);
            trial.insert(trial.begin() + insertIndex, block.begin(), block.end());
            if (const std::wstring endLoopErr = ValidateEndLoopPlacements(trial); !endLoopErr.empty()) {
                ShowPromptInfo(kEndLoopNeedsLoopParentMsg);
                dragIndex_ = -1;
                dragMoved_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
        actions_.erase(actions_.begin() + dragIndex_, actions_.begin() + dragEnd);
        collapsedContainers_ = RemapCollapsedAfterMove(collapsedContainers_, dragIndex_, dragEnd, insertIndex, count);
        actions_.insert(actions_.begin() + insertIndex, block.begin(), block.end());
        // 拖拽改序后重建解析标记，避免与 actions_ 长度错位
        editorActionParsed_.assign(actions_.size(), 1);
        if (!editorActionBlocks_.empty()) {
            editorActionBlocks_.clear();
            editorParsePending_ = false;
            editorParseCursor_ = static_cast<int>(actions_.size());
        }
        MarkVisibleActionsDirty();
        selectedIndex_ = insertIndex;
        RenumberActions();
        EnsureSelectedVisible();
        UpdateEditMode();
        OnActionsChanged();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void RenumberActions() { for (size_t i = 0; i < actions_.size(); ++i) actions_[i].originalNo = static_cast<int>(i + 1); }
    void UpdateHomeScrollFromThumb(int thumbTop) {
        RECT track = HomeScrollTrackRect();
        RECT thumb = HomeScrollThumbRect();
        const int maxScroll = ActiveHomeListMaxScroll();
        const int trackHeight = static_cast<int>(track.bottom - track.top);
        const int thumbHeight = static_cast<int>(thumb.bottom - thumb.top);
        const int range = std::max(1, trackHeight - thumbHeight);
        const int thumbOffset = thumbTop - static_cast<int>(track.top);
        homeScrollOffset_ = std::clamp(thumbOffset * maxScroll / range, 0, maxScroll);
    }

    void OnWheel(int delta) {
        if (promptModal_.visible()) return;
        if (page_ == Page::Editor && editorPopupOpen_ >= 0 && !EditorDropPopupVisible()) {
            PopupCombo* pc = GetEditorPopup();
            if (pc && pc->open && !pc->items.empty()) {
                const int total = static_cast<int>(pc->items.size());
                const int visible = EditorPopupVisibleCount();
                const int scrollMax = std::max(0, total - visible);
                if (scrollMax > 0) {
                    const int oldScroll = editorPopupScroll_;
                    editorPopupScroll_ = std::clamp(editorPopupScroll_ + (delta < 0 ? 1 : -1), 0, scrollMax);
                    if (oldScroll != editorPopupScroll_) InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return;
        }
        if (page_ == Page::Home) {
            if (activeHomeTab_ == quickscript::MainTab::Macro
                || activeHomeTab_ == quickscript::MainTab::Recorder
                || activeHomeTab_ == quickscript::MainTab::ScriptCustom) {
                const int maxScroll = ActiveHomeListMaxScroll();
                if (maxScroll > 0) {
                    const int step = std::max(1, UiLen(kHomeCardStep));
                    const int old = homeScrollOffset_;
                    homeScrollOffset_ = std::clamp(
                        homeScrollOffset_ + (delta < 0 ? step : -step), 0, maxScroll);
                    if (old != homeScrollOffset_) {
                        POINT cursorPt{};
                        GetCursorPos(&cursorPt);
                        ScreenToClient(hwnd_, &cursorPt);
                        if (activeHomeTab_ == quickscript::MainTab::Macro)
                            homeHover_ = HitHomeCard(cursorPt.x, cursorPt.y);
                        else if (activeHomeTab_ == quickscript::MainTab::Recorder)
                            recordingHover_ = HitRecordingCard(cursorPt.x, cursorPt.y);
                        else
                            agentConvHover_ = HitAgentConvCard(cursorPt.x, cursorPt.y);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
            }
            return;
        }
        if (page_ != Page::Editor) return;
        // Scroll parameter panel when cursor is inside the scroll viewport (below action combo, above cancel/save)
        {
            POINT cursorPt;
            GetCursorPos(&cursorPt);
            ScreenToClient(hwnd_, &cursorPt);
            RECT paramVp = ParamScrollViewportRect();
            if (PtIn(paramVp, cursorPt.x, cursorPt.y)) {
                ScrollParamPanel(delta < 0 ? 40 : -40);
                return;
            }
        }
        if (MaxEditorScroll() <= 0) return;
        const int oldScroll = scrollOffset_;
        const int maxOffset = MaxEditorScroll();
        scrollOffset_ = std::clamp(scrollOffset_ + (delta < 0 ? 3 : -3), 0, maxOffset);
        if (oldScroll != scrollOffset_) {
            // 滚动后按光标位置重算 hover，避免旧行高亮跟着滚走再被新行替换造成闪烁
            POINT cursorPt{};
            GetCursorPos(&cursorPt);
            ScreenToClient(hwnd_, &cursorPt);
            hoverIndex_ = HitRow(cursorPt.x, cursorPt.y);
            RefreshActionListLayer();
        }
    }
    void EnsureSelectedVisible() {
        if (selectedIndex_ < 0) return;
        const int slot = VisibleSlotOf(selectedIndex_);
        if (slot < 0) return;
        const int visible = VisibleActionRows();
        if (visible <= 0) { scrollOffset_ = 0; return; }
        if (slot < scrollOffset_) scrollOffset_ = slot;
        if (slot >= scrollOffset_ + visible) scrollOffset_ = slot - visible + 1;
        scrollOffset_ = std::clamp(scrollOffset_, 0, MaxEditorScroll());
    }

    void ShowCopyMenu(int x, int y) {
        POINT pt{x, y};
        ClientToScreen(hwnd_, &pt);
        const int id = ThemedPopupMenu::Show(hwnd_, pt, {
            {kCopyLast, L"复制到最后"},
            {kCopyFirst, L"复制到最前"},
            {kCopyBeforeSelected, L"复制到当前项前"},
            {kCopyAfterSelected, L"复制到当前项后"},
        });
        if (id != 0) CopyActionByMenu(id);
    }

    void DrawTextIn(HDC hdc, const std::wstring& text, RECT rc, COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    void DrawTopAction(HDC hdc, RECT rc, const std::wstring& text, int iconType) {
        DrawTopActionGlyph(hdc, rc, iconType);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, text, RECT{rc.left + UiLen(32), rc.top, rc.right, rc.bottom}, kWhite);
    }

    void DrawMacroCreatePrompt(HDC hdc, const RECT& cr) {
        const wchar_t* partClick = L"点击";
        const wchar_t* partCreate = L"创建";
        const wchar_t* partSuffix = L"鼠标宏";
        const int gap = UiLen(12);
        const int padV = UiLen(21);
        const int createBtnW = UiLen(78);
        const int wClick = TextWidth(partClick, bigFont_);
        const int wSuffix = TextWidth(partSuffix, bigFont_);
        const int total = wClick + gap + createBtnW + gap + wSuffix;
        int x = cr.left + (cr.right - cr.left - total) / 2;
        DrawTextIn(hdc, partClick, RECT{x, cr.top, x + wClick, cr.bottom}, kBannerText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += wClick + gap;
        RECT btn{x, cr.top + padV, x + createBtnW, cr.bottom - padV};
        FillRectColor(hdc, btn, kOrange);
        DrawTextIn(hdc, partCreate, btn, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += createBtnW + gap;
        DrawTextIn(hdc, partSuffix, RECT{x, cr.top, x + wSuffix, cr.bottom}, kBannerText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    void DrawTitleButtons(HDC hdc) {
        RECT close = CloseRect();
        if (page_ == Page::Home) {
            RECT settings = SettingsRect();
            RECT minimize = MinimizeRect();
            if (hoverButton_ == HoverButton::Settings) FillAlphaRect(hdc, settings, RGB(0, 0, 0), kCloseHoverAlpha);
            if (hoverButton_ == HoverButton::Minimize) FillAlphaRect(hdc, minimize, RGB(0, 0, 0), kCloseHoverAlpha);
            DrawGearGlyph(hdc, settings, kWhite, kNavStripGreen);
            SelectObject(hdc, closeFont_);
            DrawTextIn(hdc, L"−", minimize, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            RECT minimize = MinimizeRect();
            if (hoverButton_ == HoverButton::Minimize) FillAlphaRect(hdc, minimize, RGB(0, 0, 0), kCloseHoverAlpha);
            SelectObject(hdc, closeFont_);
            DrawTextIn(hdc, L"−", minimize, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (hoverButton_ == HoverButton::Close) FillAlphaRect(hdc, close, RGB(0, 0, 0), kCloseHoverAlpha);
        SelectObject(hdc, closeFont_);
        DrawTextIn(hdc, L"×", close, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void DrawHomeShell(HDC hdc) {
        SelectObject(hdc, homeTabFont_);
        DrawTextIn(hdc, L"键鼠工坊", RECT{UiLen(14), 0, UiLen(120), UiLen(kTitleH)}, kWhite);
        DrawNavTab(hdc, ClickerTabRect(), quickscript::MainTab::Clicker, L"鼠标连点", 0);
        DrawNavTab(hdc, RecorderTabRect(), quickscript::MainTab::Recorder, L"键鼠录制", 1);
        DrawNavTab(hdc, MacroTabRect(), quickscript::MainTab::Macro, L"鼠标宏", 2);
        DrawNavTab(hdc, ScriptCustomTabRect(), quickscript::MainTab::ScriptCustom, L"脚本定制", 3);
    }

    void DrawNavTab(HDC hdc, RECT rc, quickscript::MainTab tab, const std::wstring& text, int iconType);

    void DrawRadio(HDC hdc, RECT rc, bool checked);

    std::wstring ClickIntervalTitle(quickscript::ClickIntervalMode mode) const {
        switch (mode) {
        case quickscript::ClickIntervalMode::Custom: return L"自定义";
        case quickscript::ClickIntervalMode::Efficient: return L"高效模式(每秒10次点击)";
        case quickscript::ClickIntervalMode::Extreme: return L"极速模式(每秒100次)";
        default: return L"高效模式(每秒10次点击)";
        }
    }

    std::wstring ClickIntervalComboText() const {
        if (clickerSettings_.intervalMode == quickscript::ClickIntervalMode::Custom) {
            wchar_t buf[64]{};
            swprintf_s(buf, L"间隔%.3f秒点击", clickerSettings_.customIntervalSeconds);
            return buf;
        }
        return ClickIntervalTitle(clickerSettings_.intervalMode);
    }

    RECT ClickerCustomHintRect() const {
        struct Segment { const wchar_t* text; };
        static const Segment kSegments[] = {
            {L"点击"}, {L"修改时间间隔"}, {L"，点击下拉箭头可快速设置"},
            {L"极速模式"}, {L"和"}, {L"高效模式"},
        };
        int textW = UiLen(16);
        for (const auto& seg : kSegments) {
            textW += TextWidth(seg.text, homeFont_);
        }
        return UiRect4(kClickerLabelX, 203, kClickerLabelX + textW, 231);
    }

    void DrawClickerCustomHint(HDC hdc) {
        const RECT rc = ClickerCustomHintRect();
        FillRectColor(hdc, rc, kCreateYellow);
        SelectObject(hdc, homeFont_);
        struct Segment { const wchar_t* text; COLORREF color; };
        static const Segment kSegments[] = {
            {L"点击", kBannerText},
            {L"修改时间间隔", kMainGreen},
            {L"，点击下拉箭头可快速设置", kBannerText},
            {L"极速模式", RGB(220, 60, 60)},
            {L"和", kBannerText},
            {L"高效模式", kMainGreen},
        };
        int x = rc.left + UiLen(8);
        const int y = rc.top;
        const int h = rc.bottom - rc.top;
        for (const auto& seg : kSegments) {
            const int w = TextWidth(seg.text, homeFont_);
            DrawTextIn(hdc, seg.text, RECT{x, y, x + w, y + h}, seg.color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x += w;
        }
    }

    std::wstring ClickerButtonTitle() const {
        switch (clickerSettings_.button) {
        case quickscript::MouseButtonChoice::Left: return L"左键";
        case quickscript::MouseButtonChoice::Middle: return L"中键";
        case quickscript::MouseButtonChoice::Right: return L"右键";
        default: return L"左键";
        }
    }

    void DrawClickerCombo(HDC hdc, RECT rc, const std::wstring& text, bool dropped = false) {
        const COLORREF borderColor = dropped ? kMainGreen : kComboBorderGray;
        FillRectColor(hdc, rc, kWhite);
        DrawTextIn(hdc, text, RECT{rc.left + 8, rc.top, rc.right - 36, rc.bottom}, kMainGreen);
        const int arrowCenterX = rc.right - 16;
        const int arrowCenterY = rc.top + 15;
        DrawComboDownArrow(hdc, arrowCenterX, arrowCenterY, kMainGreen);
        DrawBorderRect(hdc, rc, borderColor);
    }

    void DrawClickerCombo(HDC hdc, RECT rc, bool dropped = false) {
        DrawClickerCombo(hdc, rc, ClickIntervalComboText(), dropped);
    }

    void DrawClickerPopupMenuItem(HDC hdc, const RECT& row, const wchar_t* title, const wchar_t* desc, bool checked, bool hovered);

    void PaintClickerDropPopupContent(HDC hdc, HWND popupHwnd);

    // ── Editor popup combo drawing ─────────────────────────────────
    PopupCombo* PopupComboForEditorId(int id) {
        switch (id) {
        case 0: return &popupMode_;
        case 1: return &popupAction_;
        case 2: return &popupMouseBtn_;
        case 3: return &popupClickBtn_;
        case 4: return &popupLoopType_;
        case 5: return &popupRunBlock_;
        case 6: return &popupHotkeyShortcut_;
        case 7: return &popupQuickInputVar_;
        case 8: return &popupRunMacro_;
        case 9: return &popupMousePlayback_;
        case 10: return &popupScrollDir_;
        case 11: return &popupFindFollowUp_;
        case 12: return &popupIfVar_;
        case 13: return &popupIfOperator_;
        case 14: return &popupIfConnector_;
        case 15: return &popupRunProgram_;
        case 16: return &popupOcrResultMode_;
        case 17: return &popupOcrFollowUp_;
        case 18: return &popupOcrSearchVar_;
        case 19: return &popupAiModel_;
        case 20: return &popupAiContextMode_;
        case 21: return &popupAiOutputType_;
        case 22: return &popupAiSearchRegion_;
        case 23: return &popupWmSelectMethod_;
        default: return nullptr;
        }
    }

    std::wstring EditorComboDisplayText(HWND label) const {
        std::wstring text = GetText(label);
        if (!text.empty()) return text;
        const int id = const_cast<EngineHost*>(this)->EditorComboPopupIdForHwnd(label);
        if (id < 0) return text;
        PopupCombo* pc = const_cast<EngineHost*>(this)->PopupComboForEditorId(id);
        if (!pc || pc->sel < 0 || pc->sel >= static_cast<int>(pc->items.size())) return text;
        return pc->items[static_cast<size_t>(pc->sel)];
    }

    PopupCombo* GetEditorPopup() {
        return PopupComboForEditorId(editorPopupOpen_);
    }

    int EditorPopupVisibleCount() const {
        PopupCombo* pc = const_cast<EngineHost*>(this)->GetEditorPopup();
        if (!pc || pc->items.empty()) return 0;
        const int maxVisible = kEditorPopupMaxHeight / kEditorPopupItemH;
        return std::min(maxVisible, static_cast<int>(pc->items.size()));
    }

    RECT EditorPopupListRect() const {
        RECT base = EditorPopupRect();
        const int visible = EditorPopupVisibleCount();
        return RECT{base.left, base.bottom, base.right, base.bottom + visible * kEditorPopupItemH + 2};
    }

    RECT EditorPopupRect() const {
        switch (editorPopupOpen_) {
        case 0: return WindowClientRect(mode_);
        case 23: return WindowClientRect(wmSelectMethod_);
        case 1: return WindowClientRect(actionCombo_);
        case 2: return EditorComboClientRect(mousePressButton_);
        case 3: return EditorComboClientRect(clickButton_);
        case 4: return EditorComboClientRect(loopTypeCombo_);
        case 5: return EditorComboClientRect(runBlockCombo_);
        case 6: return EditorComboClientRect(hotkeyShortcutCombo_);
        case 7: {
            HWND combo = ActiveVarComboHwnd();
            return combo ? EditorComboClientRect(combo) : RECT{};
        }
        case 8: return EditorComboClientRect(runMacroCombo_);
        case 9: return EditorComboClientRect(mousePlaybackCombo_);
        case 10: return EditorComboClientRect(scrollDirectionCombo_);
        case 11: return EditorComboClientRect(findFollowUpCombo_);
        case 12: return EditorComboClientRect(ifVarCombo_);
        case 13: return EditorComboClientRect(ifOperatorCombo_);
        case 14: return EditorComboClientRect(ifConnectorCombo_);
        case 15: return EditorComboClientRect(runProgramCombo_);
        case 16: return EditorComboClientRect(ocrResultModeCombo_);
        case 17: return EditorComboClientRect(ocrFollowUpCombo_);
        case 18: return EditorComboClientRect(ocrSearchVarCombo_);
        case 19: return EditorComboClientRect(aiModelCombo_);
        case 20: return EditorComboClientRect(aiContextModeCombo_);
        case 21: return EditorComboClientRect(aiOutputTypeCombo_);
        case 22: return EditorComboClientRect(aiSearchRegionCombo_);
        default: return RECT{};
        }
    }

    RECT EditorComboInvalidateRect(int popupId) const {
        HWND combo = nullptr;
        switch (popupId) {
        case 0: combo = mode_; break;
        case 23: combo = wmSelectMethod_; break;
        case 1: combo = actionCombo_; break;
        case 2: combo = mousePressButton_; break;
        case 3: combo = clickButton_; break;
        case 4: combo = loopTypeCombo_; break;
        case 5: combo = runBlockCombo_; break;
        case 6: combo = hotkeyShortcutCombo_; break;
        case 7: combo = ActiveVarComboHwnd(); break;
        case 8: combo = runMacroCombo_; break;
        case 9: combo = mousePlaybackCombo_; break;
        case 10: combo = scrollDirectionCombo_; break;
        case 11: combo = findFollowUpCombo_; break;
        case 12: combo = ifVarCombo_; break;
        case 13: combo = ifOperatorCombo_; break;
        case 14: combo = ifConnectorCombo_; break;
        case 15: combo = runProgramCombo_; break;
        case 16: combo = ocrResultModeCombo_; break;
        case 17: combo = ocrFollowUpCombo_; break;
        case 18: combo = ocrSearchVarCombo_; break;
        case 19: combo = aiModelCombo_; break;
        case 20: combo = aiContextModeCombo_; break;
        case 21: combo = aiOutputTypeCombo_; break;
        case 22: combo = aiSearchRegionCombo_; break;
        default: return RECT{};
        }
        if (!combo) return RECT{};
        RECT rc = EditorComboClientRect(combo);
        if (popupId == 0) {
            const RECT header = EditorMacroHeaderTextRect();
            rc.left = std::max<LONG>(0, rc.left - ScaleEditorX(50));
            rc.top = std::min(rc.top, header.top);
            rc.bottom = std::max(rc.bottom, header.bottom);
        } else if (popupId == 23) {
            const RECT header = EditorMacroHeaderTextRect();
            rc.left = std::max<LONG>(0, rc.left - ScaleEditorX(108));
            rc.top = std::min(rc.top, header.top);
            rc.bottom = std::max(rc.bottom, header.bottom);
        } else if (popupId == 1) rc.top = std::max<LONG>(0, rc.top - 28);
        InflateRect(&rc, 3, 3);
        return rc;
    }

    void InvalidateEditorComboArea(int popupId) {
        if (popupId < 0) return;
        const RECT rc = EditorComboInvalidateRect(popupId);
        if (rc.right <= rc.left || rc.bottom <= rc.top) return;
        HWND combo = nullptr;
        switch (popupId) {
        case 0: combo = mode_; break;
        case 23: combo = wmSelectMethod_; break;
        case 1: combo = actionCombo_; break;
        case 2: combo = mousePressButton_; break;
        case 3: combo = clickButton_; break;
        case 4: combo = loopTypeCombo_; break;
        case 5: combo = runBlockCombo_; break;
        case 6: combo = hotkeyShortcutCombo_; break;
        case 7: combo = ActiveVarComboHwnd(); break;
        case 8: combo = runMacroCombo_; break;
        case 9: combo = mousePlaybackCombo_; break;
        case 10: combo = scrollDirectionCombo_; break;
        case 11: combo = findFollowUpCombo_; break;
        case 12: combo = ifVarCombo_; break;
        case 13: combo = ifOperatorCombo_; break;
        case 14: combo = ifConnectorCombo_; break;
        case 15: combo = runProgramCombo_; break;
        case 16: combo = ocrResultModeCombo_; break;
        case 17: combo = ocrFollowUpCombo_; break;
        case 18: combo = ocrSearchVarCombo_; break;
        case 19: combo = aiModelCombo_; break;
        case 20: combo = aiContextModeCombo_; break;
        case 21: combo = aiOutputTypeCombo_; break;
        case 22: combo = aiSearchRegionCombo_; break;
        default: break;
        }
        if (combo && IsParamViewportChild(combo) && paramViewport_) {
            const RECT vp = ParamViewportRect();
            RECT inv{
                rc.left - vp.left, rc.top - vp.top,
                rc.right - vp.left, rc.bottom - vp.top
            };
            InvalidateRect(paramViewport_, &inv, FALSE);
            RepaintParamPanelChrome();
        } else {
            InvalidateRect(hwnd_, &rc, FALSE);
        }
    }

    void InvalidateEditorParamPanel() {
        InvalidateParamScrollArea();
    }

    void DrawEditorCombo(HDC hdc, HWND label, RECT rc, bool dropped = false);

    void CreateEditorDropPopup() {
        RegisterEditorDropPopupClass();
        editorDropPopup_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"QSEditorDropPopup", L"",
            WS_POPUP,
            0, 0, 0, 0,
            hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (editorDropPopup_) {
            SetWindowLongPtrW(editorDropPopup_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(editorDropPopup_, SW_HIDE);
        }
    }

    bool EditorDropPopupVisible() const {
        return editorDropPopup_ && IsWindowVisible(editorDropPopup_) == TRUE;
    }

    void SyncEditorDropPopup() {
        if (!editorDropPopup_) return;
        if (editorPopupOpen_ < 0 || page_ != Page::Editor || !IsWindowVisible(hwnd_)) {
            ShowWindow(editorDropPopup_, SW_HIDE);
            return;
        }
        PopupCombo* pc = GetEditorPopup();
        if (!pc || !pc->open || pc->items.empty()) {
            ShowWindow(editorDropPopup_, SW_HIDE);
            return;
        }
        RECT list = EditorPopupListRect();
        const int w = list.right - list.left;
        const int h = list.bottom - list.top;
        POINT screenTop{list.left, list.top};
        ClientToScreen(hwnd_, &screenTop);
        const int x = static_cast<int>(screenTop.x);
        int y = static_cast<int>(screenTop.y);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        if (y + h > work.bottom) {
            RECT anchor = EditorPopupRect();
            POINT screenAnchor{anchor.left, anchor.top};
            ClientToScreen(hwnd_, &screenAnchor);
            y = static_cast<int>(screenAnchor.y) - h;
        }
        y = std::max(static_cast<int>(work.top), std::min(y, static_cast<int>(work.bottom) - h));
        RECT existing{};
        GetWindowRect(editorDropPopup_, &existing);
        const bool samePos = existing.left == x && existing.top == y
            && (existing.right - existing.left) == w && (existing.bottom - existing.top) == h;
        if (samePos && EditorDropPopupVisible()) return;
        SetWindowPos(editorDropPopup_, HWND_TOPMOST, x, y, w, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void InvalidateEditorPopupRow(int idx) {
        if (!editorDropPopup_ || idx < 0) return;
        RECT client{};
        GetClientRect(editorDropPopup_, &client);
        const int vis = idx - editorPopupScroll_;
        if (vis < 0 || vis >= EditorPopupVisibleCount()) return;
        RECT row{client.left + 1, client.top + 1 + vis * kEditorPopupItemH, client.right - 1, client.top + 1 + (vis + 1) * kEditorPopupItemH};
        InvalidateRect(editorDropPopup_, &row, FALSE);
    }

    void PaintEditorDropPopupContent(HDC hdc, HWND popupHwnd);

    void CreateEditorTipPopup() {
        RegisterEditorTipPopupClass();
        editorTipPopup_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"QSEditorTipPopup", L"",
            WS_POPUP,
            0, 0, 0, 0,
            hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (editorTipPopup_) {
            SetWindowLongPtrW(editorTipPopup_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(editorTipPopup_, SW_HIDE);
        }
    }

    void SyncQuickInputTipPopup() {
        if (!editorTipPopup_) return;
        if (quickInputTipShown_ == QuickInputTipKind::None || page_ != Page::Editor) {
            ShowWindow(editorTipPopup_, SW_HIDE);
            return;
        }
        const SIZE size = MeasureQuickInputTipSize();
        if (size.cx <= 0 || size.cy <= 0) {
            ShowWindow(editorTipPopup_, SW_HIDE);
            return;
        }
        POINT screenPt{};
        if (quickInputTipShown_ == QuickInputTipKind::TextExample) {
            POINT clientPt = quickInputTipAnchor_;
            ClientToScreen(hwnd_, &clientPt);
            screenPt.x = clientPt.x + 12;
            screenPt.y = clientPt.y + 16;
        } else if (quickInputTipShown_ == QuickInputTipKind::VariableHelp) {
            if (editorDropPopup_ && EditorDropPopupVisible()) {
                RECT popupRc{};
                GetWindowRect(editorDropPopup_, &popupRc);
                const int vis = editorPopupHover_ - editorPopupScroll_;
                if (vis >= 0 && vis < EditorPopupVisibleCount()) {
                    screenPt.x = popupRc.right + 4;
                    screenPt.y = popupRc.top + 1 + vis * kEditorPopupItemH;
                } else {
                    GetCursorPos(&screenPt);
                }
            } else {
                GetCursorPos(&screenPt);
            }
        }
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int x = screenPt.x;
        int y = screenPt.y;
        const int tipW = static_cast<int>(size.cx);
        const int tipH = static_cast<int>(size.cy);
        const int workLeft = static_cast<int>(work.left);
        const int workTop = static_cast<int>(work.top);
        const int workRight = static_cast<int>(work.right);
        const int workBottom = static_cast<int>(work.bottom);
        if (x + tipW > workRight) x = std::max(workLeft, workRight - tipW);
        if (y + tipH > workBottom) y = std::max(workTop, workBottom - tipH);
        x = std::max(workLeft, x);
        y = std::max(workTop, y);
        SetWindowPos(editorTipPopup_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
        InvalidateRect(editorTipPopup_, nullptr, FALSE);
    }

    void CloseEditorPopup() {
        const int closedId = editorPopupOpen_;
        if (editorPopupOpen_ >= 0) {
            PopupCombo* pc = GetEditorPopup();
            if (pc) pc->open = false;
        }
        if (closedId == 7) CancelQuickInputTip();
        editorPopupOpen_ = -1;
        editorPopupHover_ = -1;
        editorPopupScroll_ = 0;
        SyncEditorDropPopup();
        InvalidateEditorComboArea(closedId);
        RepaintParamPanelChrome();
    }

    void ToggleEditorPopup(int id) {
        if (editorPopupOpen_ == id) { CloseEditorPopup(); return; }
        const int prevId = editorPopupOpen_;
        if (prevId == 7 || id == 7) CancelQuickInputTip();
        if (prevId >= 0) {
            PopupCombo* prevPc = GetEditorPopup();
            if (prevPc) prevPc->open = false;
            editorPopupOpen_ = -1;
            editorPopupHover_ = -1;
            editorPopupScroll_ = 0;
        }
        editorPopupOpen_ = id;
        editorPopupHover_ = -1;
        editorPopupScroll_ = 0;
        if (id == 5) RefreshRunBlockCombo();
        else if (id == 7) RefreshActiveVarCombo();
        else if (id == 8) RefreshRunMacroCombo();
        else if (id == 9) RefreshMousePlaybackCombo();
        else if (id == 12) RefreshIfVarCombo();
        else if (id == 16) { RefreshOcrSubPanel(); SyncParamScrollLayout(); }
        else if (id == 17) { RefreshOcrSubPanel(); SyncParamScrollLayout(); }
        else if (id == 18) RefreshOcrSearchVarCombo();
        else if (id == 19) RefreshAiModelCombo();
        PopupCombo* pc = GetEditorPopup();
        if (pc) pc->open = true;
        SyncEditorDropPopup();
        if (prevId >= 0) InvalidateEditorComboArea(prevId);
        InvalidateEditorComboArea(id);
        RepaintParamPanelChrome();
    }

    int HitEditorPopupItem(int x, int y) const {
        if (editorPopupOpen_ < 0 || EditorDropPopupVisible()) return -1;
        PopupCombo* pc = const_cast<EngineHost*>(this)->GetEditorPopup();
        if (!pc || !pc->open || pc->items.empty()) return -1;
        RECT popup = EditorPopupListRect();
        if (!PtIn(popup, x, y)) return -1;
        return HitEditorPopupItemLocal(x - popup.left, y - popup.top);
    }

    int HitEditorPopupItemLocal(int, int y) const {
        if (editorPopupOpen_ < 0) return -1;
        PopupCombo* pc = const_cast<EngineHost*>(this)->GetEditorPopup();
        if (!pc || pc->items.empty()) return -1;
        const int visible = EditorPopupVisibleCount();
        const int rel = (y - 1) / kEditorPopupItemH;
        if (rel < 0 || rel >= visible) return -1;
        const int idx = rel + editorPopupScroll_;
        const int total = static_cast<int>(pc->items.size());
        return idx < total ? idx : -1;
    }

    void OnEditorDropPopupWheel(int delta) {
        PopupCombo* pc = GetEditorPopup();
        if (!pc || !pc->open || pc->items.empty()) return;
        const int total = static_cast<int>(pc->items.size());
        const int visible = EditorPopupVisibleCount();
        const int scrollMax = std::max(0, total - visible);
        if (scrollMax <= 0) return;
        const int oldScroll = editorPopupScroll_;
        editorPopupScroll_ = std::clamp(editorPopupScroll_ + (delta < 0 ? 1 : -1), 0, scrollMax);
        if (oldScroll == editorPopupScroll_ || !editorDropPopup_) return;
        // 滚动后按光标重算 hover（与动作列表滚轮一致），避免高亮停在旧索引
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(editorDropPopup_, &pt);
        const int idx = HitEditorPopupItemLocal(pt.x, pt.y);
        if (idx != editorPopupHover_) editorPopupHover_ = idx;
        InvalidateRect(editorDropPopup_, nullptr, FALSE);
        if (quickInputTipShown_ == QuickInputTipKind::VariableHelp) SyncQuickInputTipPopup();
        else if (editorPopupOpen_ == 7 && editorPopupHover_ >= 0) BeginQuickInputVarTipHover(editorPopupHover_);
    }

    void SelectEditorPopupItem(int idx) {
        if (editorPopupOpen_ < 0) return;
        PopupCombo* pc = GetEditorPopup();
        if (!pc) return;
        if (idx < 0 || idx >= static_cast<int>(pc->items.size())) { CloseEditorPopup(); return; }
        if (editorPopupOpen_ == 1 && !IsImplementedActionPopup(idx)) {
            ShowPromptInfo(L"该动作类型暂未实现。");
            CloseEditorPopup();
            return;
        }
        const bool switchingActionType = editorPopupOpen_ == 1;
        const int prevActionSel = switchingActionType ? popupAction_.sel : -1;
        // 未选中列表项时：切换类型先存当前「添加表单」草稿，再恢复目标类型草稿（无则默认）
        // 注意：须在表单已与 selectedIndex_ 一致时调用；取消选中路径必须先 Restore，否则会把列表动作的勾选写进草稿
        if (switchingActionType && !loadingForm_ && selectedIndex_ < 0 && prevActionSel >= 0) {
            actionFormDrafts_[prevActionSel] = ActionFromForm();
        }
        pc->sel = idx;
        HWND label = nullptr;
        switch (editorPopupOpen_) {
        case 0: label = mode_; break;
        case 23: label = wmSelectMethod_; break;
        case 1: label = actionCombo_; break;
        case 2: label = mousePressButton_; break;
        case 3: label = clickButton_; break;
        case 4: label = loopTypeCombo_; break;
        case 5: label = runBlockCombo_; break;
        case 6: label = hotkeyShortcutCombo_; break;
        case 7: label = ActiveVarComboHwnd(); break;
        case 8: label = runMacroCombo_; break;
        case 9: label = mousePlaybackCombo_; break;
        case 10: label = scrollDirectionCombo_; break;
        case 11: label = findFollowUpCombo_; break;
        case 12: label = ifVarCombo_; break;
        case 13: label = ifOperatorCombo_; break;
        case 14: label = ifConnectorCombo_; break;
        case 15: label = runProgramCombo_; break;
        case 16: label = ocrResultModeCombo_; break;
        case 17: label = ocrFollowUpCombo_; break;
        case 18: label = ocrSearchVarCombo_; break;
        case 19: label = aiModelCombo_; break;
        case 20: label = aiContextModeCombo_; break;
        case 21: label = aiOutputTypeCombo_; break;
        case 22: label = aiSearchRegionCombo_; break;
        }
        if (label) SetText(label, pc->items[static_cast<size_t>(idx)]);
        if (switchingActionType) {
            if (!loadingForm_ && selectedIndex_ < 0) {
                RestoreActionFormDraftForCurrentType();
            } else if (!loadingForm_ && selectedIndex_ >= 0) {
                // 已选中列表动作时改类型：用新类型默认值（保留备注），不碰添加草稿
                ScriptAction next = DefaultActionForPopupSel(idx);
                next.remark = GetText(remark_);
                LoadForm(next);
            } else {
                RefreshParamPanel();
            }
            // 下拉在 LBUTTONDOWN 选中；匹配的 UP 可能落在新露出的「同时按住」勾选框上
            DiscardSpuriousEditorInput();
        } else if (editorPopupOpen_ == 0) {
            if (page_ == Page::Editor) UpdateEditorWindowModeChrome();
            SyncScriptWindowModeFromEditor();
        } else if (editorPopupOpen_ == 23) {
            SyncScriptWindowModeFromEditor();
            // 切换「选择窗口方式」后必须刷新，「指定窗口类」按钮显隐依赖 sel==2。
            if (page_ == Page::Editor) UpdateEditorWindowModeChrome();
        } else if (editorPopupOpen_ == 11) RefreshFindImageSubPanel();
        else if (editorPopupOpen_ == 16) { RefreshOcrSubPanel(); SyncParamScrollLayout(); }
        else if (editorPopupOpen_ == 17) { RefreshOcrSubPanel(); SyncParamScrollLayout(); }
        else if (editorPopupOpen_ == 15) UpdateRunProgramSubPanel();
        const int closedId = editorPopupOpen_;
        pc->open = false;
        editorPopupOpen_ = -1;
        editorPopupHover_ = -1;
        SyncEditorDropPopup();
        InvalidateEditorComboArea(closedId);
        RepaintParamPanelChrome();
    }

    bool HandleEditorPopupClick(int x, int y) {
        if (editorPopupOpen_ < 0) return false;
        int idx = HitEditorPopupItem(x, y);
        if (idx >= 0) { SelectEditorPopupItem(idx); return true; }
        RECT anchor = EditorPopupRect();
        if (!PtIn(anchor, x, y)) {
            CloseEditorPopup();
            return true;
        }
        return false;
    }

    void FillAlphaRect(HDC hdc, RECT rc, COLORREF color, BYTE alpha) {
        ::FillAlphaRect(hdc, rc, color, alpha);
    }

    bool IsHotkeyMenuChecked(int id) const {
        if (id == kHotLeft) return globalHotkey_.vk == VK_LBUTTON && globalHotkey_.modifiers == 0;
        if (id == kHotMiddle) return globalHotkey_.vk == VK_MBUTTON && globalHotkey_.modifiers == 0;
        if (id == kHotRight) return globalHotkey_.vk == VK_RBUTTON && globalHotkey_.modifiers == 0;
        if (id == kHotX1) return globalHotkey_.vk == VK_XBUTTON1 && globalHotkey_.modifiers == 0;
        if (id == kHotX2) return globalHotkey_.vk == VK_XBUTTON2 && globalHotkey_.modifiers == 0;
        if (id == kHotSpace) return globalHotkey_.vk == VK_SPACE && globalHotkey_.modifiers == 0;
        return id == kHotCustom && globalHotkey_.enabled
            && !IsHotkeyMenuChecked(kHotLeft) && !IsHotkeyMenuChecked(kHotMiddle)
            && !IsHotkeyMenuChecked(kHotRight) && !IsHotkeyMenuChecked(kHotX1)
            && !IsHotkeyMenuChecked(kHotX2) && !IsHotkeyMenuChecked(kHotSpace);
    }

    void MeasureOwnerItem(MEASUREITEMSTRUCT* mis) {
        if (!mis) return;
        if (mis->CtlType == ODT_COMBOBOX) {
            mis->itemHeight = kComboItemH;
            return;
        }
    }

    void DrawComboItem(DRAWITEMSTRUCT* dis) {
        if (!dis || dis->itemID == static_cast<UINT>(-1)) return;
        const bool inList = (dis->itemState & ODS_COMBOBOXEDIT) == 0;
        const int curSel = ComboBox_GetCurSel(dis->hwndItem);
        const bool isCurSel = inList && static_cast<int>(dis->itemID) == curSel;
        bool highlighted = false;
        if (inList && !isCurSel) {
            COMBOBOXINFO cbi{sizeof(cbi)};
            if (GetComboBoxInfo(dis->hwndItem, &cbi) && cbi.hwndList) {
                auto it = g_comboListHover.find(cbi.hwndList);
                highlighted = (it != g_comboListHover.end() && it->second == static_cast<int>(dis->itemID));
            }
        }
        COLORREF bg = kWhite;
        COLORREF fg = inList ? kText : kMainGreen;
        if (isCurSel) {
            bg = kComboMenuSelectBlue;
            fg = kComboMenuSelectText;
        } else if (highlighted) {
            bg = kComboMenuHoverBlue;
            fg = kText;
        }
        RECT rc = dis->rcItem;
        FillRectColor(dis->hDC, rc, bg);
        wchar_t text[256]{};
        SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, reinterpret_cast<LPARAM>(text));
        SelectObject(dis->hDC, font_);
        DrawTextIn(dis->hDC, text, RECT{rc.left + 8, rc.top, rc.right - 8, rc.bottom}, fg);
    }

    void DrawOwnerItem(DRAWITEMSTRUCT* dis) {
        if (!dis) return;
        if (dis->CtlType == ODT_COMBOBOX) { DrawComboItem(dis); return; }
        // 勾选框优先：避免被后面的绿按钮分支误吃
        if (IsMarkedParamCheckbox(dis->hwndItem)) { DrawParamPanelCheckboxItem(dis); return; }
        if (crosshairDrag_.IsCrosshairButton(dis->hwndItem)) {
            crosshairDrag_.DrawButton(dis, font_, hoverCrosshairBtn_);
            return;
        }
        if (EditorComboPopupIdForHwnd(dis->hwndItem) >= 0) {
            FillRectColor(dis->hDC, dis->rcItem, kWhite);
            return;
        }
        if (IsGrayButton(dis->hwndItem)) { DrawGrayButton(dis); return; }
        DrawOwnerButton(dis);
    }

    bool IsGrayButton(HWND hwnd) const {
        return hwnd == findFullScreenBtn_ || hwnd == findSelectRegionBtn_ || hwnd == findTestBtn_
            || hwnd == findImagePreviewBtn_
            || hwnd == findScreenshotBtn_ || hwnd == findLocalImageBtn_ || hwnd == findClearImageBtn_
            || hwnd == findSelectOffsetBtn_
            || hwnd == ocrDepInstallBtn_
            || hwnd == ocrFullScreenBtn_ || hwnd == ocrSelectRegionBtn_ || hwnd == ocrTestBtn_
            || hwnd == ocrSelectOffsetBtn_
            || hwnd == ocrFindSelectRegionBtn_ || hwnd == ocrFindImagePreviewBtn_
            || hwnd == ocrFindScreenshotBtn_ || hwnd == ocrFindLocalImageBtn_ || hwnd == ocrFindClearImageBtn_
            || hwnd == aiFindSelectRegionBtn_ || hwnd == aiFindImagePreviewBtn_
            || hwnd == aiFindScreenshotBtn_ || hwnd == aiFindLocalImageBtn_ || hwnd == aiFindClearImageBtn_
            || hwnd == aiFullScreenBtn_ || hwnd == aiFullScreenBtn2_
            || hwnd == aiSelectRegionBtn_ || hwnd == aiSelectRegionBtn2_
            || hwnd == wmTargetBrowseBtn_;
    }

    HWND HitGrayButton(int x, int y) const {
        if (page_ != Page::Editor) return nullptr;
        auto testChild = [&](HWND child) -> HWND {
            if (!IsGrayButton(child) || !IsWindowVisible(child)) return nullptr;
            if (PtIn(WindowClientRect(child), x, y)) return child;
            return nullptr;
        };
        auto scanParent = [&](HWND parent) -> HWND {
            if (!parent) return nullptr;
            for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                if (HWND h = testChild(child)) return h;
            }
            return nullptr;
        };
        if (HWND vpHit = scanParent(paramViewport_)) return vpHit;
        return scanParent(hwnd_);
    }

    void InvalidateGrayButton(HWND btn) {
        if (!btn) return;
        RedrawWindow(btn, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    }

    void UpdateGrayButtonHover(int x, int y) {
        HWND hit = HitGrayButton(x, y);
        if (hit == hoverGrayBtn_) return;
        HWND old = hoverGrayBtn_;
        hoverGrayBtn_ = hit;
        pendingHoverGrayOld_ = old;
        pendingHoverGrayNew_ = hit;
        FiDbgOnGrayHoverChanged(old, hit);
    }

    void FlushGrayButtonHover() {
        if (!pendingHoverGrayOld_ && !pendingHoverGrayNew_) return;
        InvalidateGrayButton(pendingHoverGrayOld_);
        InvalidateGrayButton(pendingHoverGrayNew_);
        pendingHoverGrayOld_ = nullptr;
        pendingHoverGrayNew_ = nullptr;
    }

    void ClearGrayButtonHover() {
        if (!hoverGrayBtn_) return;
        HWND old = hoverGrayBtn_;
        hoverGrayBtn_ = nullptr;
        InvalidateGrayButton(old);
    }

    bool IsGreenButtonHovered(HWND hwnd) const {
        if (IsGrayButton(hwnd)) return hwnd == hoverGrayBtn_;
        if (crosshairDrag_.IsCrosshairButton(hwnd)) return false;
        if (hwnd == batchDeleteBtn_ || hwnd == batchCopyBtn_) {
            if (BatchSelectedCount() == 0) return false;
            return hwnd == batchDeleteBtn_ && hoverButton_ == HoverButton::BatchDelete ||
                   hwnd == batchCopyBtn_ && hoverButton_ == HoverButton::BatchCopy;
        }
        return hwnd == loadBtn_ && hoverButton_ == HoverButton::Load ||
               hwnd == clearBtn_ && hoverButton_ == HoverButton::Clear ||
               hwnd == addBtn_ && hoverButton_ == HoverButton::Add ||
               hwnd == modifyBtn_ && hoverButton_ == HoverButton::Modify ||
               hwnd == cancelBtn_ && hoverButton_ == HoverButton::Cancel ||
               hwnd == saveBtn_ && hoverButton_ == HoverButton::Save ||
               hwnd == batchExitBtn_ && hoverButton_ == HoverButton::BatchExit ||
               hwnd == batchSelectAllBtn_ && hoverButton_ == HoverButton::BatchSelectAll ||
               hwnd == batchDeselectBtn_ && hoverButton_ == HoverButton::BatchDeselect;
    }

    bool IsOwnerDrawButtonDisabled(HWND hwnd) const {
        return (hwnd == batchDeleteBtn_ || hwnd == batchCopyBtn_) && BatchSelectedCount() == 0;
    }

    void DrawGrayButton(DRAWITEMSTRUCT* dis) {
        if (!dis) return;
        FiDbgBumpGrayDraw(dis->hwndItem, dis->itemState, hoverGrayBtn_, false);
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        const bool hovered = dis->hwndItem == hoverGrayBtn_;
        const COLORREF fill = hovered ? kGrayButtonHover : kGrayButton;
        const COLORREF border = hovered ? kMainGreen : kGrayButtonBorder;
        // 先铺面板底色再填按钮，避免滚动后边缘透出旧像素
        FillRectColor(hdc, rc, kWhite);
        FillRectColor(hdc, rc, fill);
        if (dis->hwndItem == findImagePreviewBtn_ || dis->hwndItem == ocrFindImagePreviewBtn_
            || dis->hwndItem == aiFindImagePreviewBtn_) {
            HBITMAP preview = dis->hwndItem == findImagePreviewBtn_
                ? findImagePreviewBitmap_
                : (dis->hwndItem == ocrFindImagePreviewBtn_ ? ocrFindImagePreviewBitmap_ : aiFindImagePreviewBitmap_);
            if (preview) {
                BITMAP bm{};
                GetObjectW(preview, sizeof(bm), &bm);
                if (bm.bmWidth > 0 && bm.bmHeight > 0) {
                    const int pad = 4;
                    RECT imgRc = rc;
                    imgRc.left += pad;
                    imgRc.top += pad;
                    imgRc.right -= pad;
                    imgRc.bottom -= pad;
                    HDC mem = CreateCompatibleDC(hdc);
                    HGDIOBJ oldBmp = SelectObject(mem, preview);
                    SetStretchBltMode(hdc, HALFTONE);
                    StretchBlt(hdc, imgRc.left, imgRc.top, imgRc.right - imgRc.left, imgRc.bottom - imgRc.top,
                        mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(mem, oldBmp);
                    DeleteDC(mem);
                }
            }
            DrawBorderRect(hdc, RECT{rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1}, border);
            return;
        }
        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, 64);
        SelectObject(hdc, editorFont_ ? editorFont_ : font_);
        DrawTextIn(hdc, text, rc, kGrayButtonText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        // 边框内缩 1px：D2D DrawRectangle 描边居中，贴 client 边缘时上/左边易被裁掉半像素
        RECT borderRc{rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1};
        if (borderRc.right > borderRc.left && borderRc.bottom > borderRc.top)
            DrawBorderRect(hdc, borderRc, border);
    }

    void DrawOwnerButton(DRAWITEMSTRUCT* dis) {
        if (!dis) return;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        const bool disabled = IsOwnerDrawButtonDisabled(dis->hwndItem);
        const bool pressed = !disabled && (dis->itemState & ODS_SELECTED) != 0;
        const bool hovered = !disabled && IsGreenButtonHovered(dis->hwndItem);
        const COLORREF fill = disabled ? kButtonDisabledGreen : (pressed || hovered ? kButtonGreenHover : kButtonGreen);
        // 先铺白底再画圆角，清掉四角残留（滚动/移位后常见）
        FillRectColor(hdc, rc, kWhite);
        FillRoundRectColor(hdc, rc, fill, 5);
        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, 64);
        SelectObject(hdc, font_);
        DrawTextIn(hdc, text, rc, disabled ? kButtonDisabledText : kWhite,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ── WM_PAINT entry point ───────────────────────────────────────
    void Paint() {
        if (headlessUi_) {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd_, &ps);
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd_, &ps);
            return;
        }
        PAINTSTRUCT ps{}; HDC windowDc = BeginPaint(hwnd_, &ps); RECT rc{}; GetClientRect(hwnd_, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        HDC hdc = CreateCompatibleDC(windowDc);
        HBITMAP bmp = CreateCompatibleBitmap(windowDc, w, h);
        HGDIOBJ oldBmp = SelectObject(hdc, bmp);
        RenderBatchScope batch(hdc);
        RECT title{0, 0, rc.right, UiLen(kTitleH)};
        RECT body{0, UiLen(kTitleH), rc.right, rc.bottom - (page_ == Page::Editor ? UiLen(kBottomH) : 0)};
        FillRectColor(hdc, title, kMainGreen);
        FillRectColor(hdc, body, page_ == Page::Home ? kMainGreen : kWhite);
        if (page_ == Page::Editor) {
            FillRectColor(hdc, RECT{0, rc.bottom - UiLen(kBottomH), rc.right, rc.bottom}, kPanel);
        }
        SelectObject(hdc, page_ == Page::Editor ? editorFont_ : titleFont_);
        if (page_ == Page::Editor) DrawTextIn(hdc, L"◴ 键鼠工坊-鼠标宏", RECT{UiLen(14), 0, UiLen(360), UiLen(kTitleH)}, kWhite);
        SelectObject(hdc, font_);
        if (page_ == Page::Home) {
            PaintHome(hdc, w, h);
        } else {
            PaintEditor(hdc);
        }
        DrawTitleButtons(hdc);
        batch.End();
        const int blitW = ps.rcPaint.right - ps.rcPaint.left;
        const int blitH = ps.rcPaint.bottom - ps.rcPaint.top;
        // 打开过渡整客户区 BitBlt：ExcludeClipRect 会在子控件未画完时露出主页残留
        if (!editorFullClientBlit_ && !editorOpenPending_) {
            for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                if (!IsWindowVisible(child)) continue;
                RECT childRc = PaintExcludeRectForChild(child);
                if (childRc.right <= childRc.left || childRc.bottom <= childRc.top) continue;
                RECT inter{};
                if (!IntersectRect(&inter, &childRc, &ps.rcPaint)) continue;
                ExcludeClipRect(windowDc, childRc.left, childRc.top, childRc.right, childRc.bottom);
            }
        }
        BitBlt(windowDc, ps.rcPaint.left, ps.rcPaint.top, blitW, blitH, hdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(hdc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(hdc);
        EndPaint(hwnd_, &ps);
        if (page_ == Page::Editor && !promptModal_.visible()) {
            RepaintParamPanelChrome();
            HDC chromeDc = GetDC(hwnd_);
            PaintEditorListHeaderChrome(chromeDc);
            ReleaseDC(hwnd_, chromeDc);
        }
    }

    // ── Home screen painting ───────────────────────────────────────
    void PaintHome(HDC hdc, int clientW, int clientH) {
        FillRectColor(hdc, RECT{0, 0, clientW, UiLen(kHomeContentTop)}, kNavStripGreen);
        FillRectColor(hdc, RECT{0, UiLen(kHomeFooterTop), clientW, clientH}, kNavStripGreen);
        DrawHomeShell(hdc);
        if (activeHomeTab_ == quickscript::MainTab::Clicker) {
            PaintClickerHome(hdc);
        } else if (activeHomeTab_ == quickscript::MainTab::Recorder) {
            PaintRecorderHome(hdc);
        } else if (activeHomeTab_ == quickscript::MainTab::Macro) {
            PaintMacroHome(hdc);
        } else {
            PaintScriptCustomHome(hdc);
        }
    }

    void PaintClickerHome(HDC hdc) {
        UpdateClickerLayout();
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"点击类型:", RECT{UiLen(kClickerLabelX), UiLen(169), clickerLayout_.leftRadioLeft - UiLen(kClickerFieldGap), UiLen(194)}, kWhite);
        DrawRadio(hdc, ClickerLeftRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Left);
        DrawTextIn(hdc, L"鼠标左键", RECT{clickerLayout_.leftRadioLeft + UiLen(kClickerRadioSize) + UiLen(9), UiLen(169),
            clickerLayout_.middleRadioLeft - UiLen(17), UiLen(194)}, kWhite);
        DrawRadio(hdc, ClickerMiddleRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Middle);
        DrawTextIn(hdc, L"鼠标中键", RECT{clickerLayout_.middleRadioLeft + UiLen(kClickerRadioSize) + UiLen(9), UiLen(169),
            clickerLayout_.rightRadioLeft - UiLen(17), UiLen(194)}, kWhite);
        DrawRadio(hdc, ClickerRightRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Right);
        DrawTextIn(hdc, L"鼠标右键", RECT{clickerLayout_.rightRadioLeft + UiLen(kClickerRadioSize) + UiLen(9), UiLen(169), UiLen(kClickerComboRight), UiLen(194)}, kWhite);

        if (clickerSettings_.intervalMode == quickscript::ClickIntervalMode::Custom) {
            DrawClickerCustomHint(hdc);
        }
        DrawTextIn(hdc, L"每次点击间隔时间:", RECT{UiLen(kClickerLabelX), UiLen(238), clickerLayout_.intervalComboLeft - UiLen(kClickerFieldGap), UiLen(263)}, kWhite);
        DrawClickerCombo(hdc, ClickerIntervalRect(), ClickerIntervalPopupOpen());
        DrawTextIn(hdc, L"启停的全局热键:", RECT{UiLen(kClickerLabelX), UiLen(305), clickerLayout_.hotkeyComboLeft - UiLen(kClickerFieldGap), UiLen(330)}, kWhite);
        DrawClickerCombo(hdc, ClickerHotkeyRect(), globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text, ClickerHotkeyPopupOpen());

        RECT hint = CreateRect();
        FillRectColor(hdc, hint, clicking_ ? RGB(255, 200, 200) : kCreateYellow);
        SelectObject(hdc, bigFont_);
        const bool hold = GlobalHotkeyUsesHold();
        const std::wstring prefix = hold ? L"按住" : L"按";
        const std::wstring hotText = globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text;
        const std::wstring actionText = clicking_
            ? (L"键停止 连点（当前 " + ClickerButtonTitle() + L" " + ClickIntervalComboText() + L"）")
            : (L"键开始 " + ClickerButtonTitle() + L" 连点");
        RECT keyBox = ClickerBannerKeyRect();
        const int gap = UiLen(10);
        const int prefixW = TextWidth(prefix, bigFont_);
        RECT prefixRc{keyBox.left - gap - prefixW, hint.top, keyBox.left - gap, hint.bottom};
        const COLORREF bannerFg = clicking_ ? RGB(180, 40, 40) : kBannerText;
        DrawTextIn(hdc, prefix, prefixRc, bannerFg, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        FillRectColor(hdc, keyBox, clicking_ ? RGB(200, 50, 50) : kOrange);
        DrawTextIn(hdc, hotText, keyBox, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, actionText, RECT{keyBox.right + gap, hint.top, hint.right - UiLen(12), hint.bottom},
            bannerFg, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
    }

    void DrawRecorderEmptyIcon(HDC hdc) {
        ::DrawRecorderEmptyIcon(hdc);
    }

    void PaintRecorderHome(HDC hdc) {
        DrawTopAction(hdc, ImportRect(), L"导入", 0);
        DrawTopAction(hdc, ExportRect(), L"导出", 1);
        DrawTopAction(hdc, TimerRect(), L"定时", 2);

        if (recordings_.empty() && !recording_) {
            DrawRecorderEmptyIcon(hdc);
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, L"没有已录制记录，按下面的提示开始录制吧", UiRect4(60, 320, 660, 350), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            int saved = SaveDC(hdc);
            RECT list = HomeListRect();
            IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
            for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
                RECT r = RecordingCardRect(i);
                if (r.bottom < list.top || r.top > list.bottom) continue;
                COLORREF cardColor = i == selectedRecording_ ? kDarkGreen : (i == recordingHover_ ? kCardHoverGreen : kCardGreen);
                FillRectColor(hdc, r, cardColor);
                RECT hotRc = RecordingHotkeyRect(i);
                SelectObject(hdc, homeFont_);
                DrawTextIn(hdc, recordings_[static_cast<size_t>(i)].name, HomeCardNameRect(r), kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, hotFont_);
                DrawTextIn(hdc, RecordingHotkeyText(recordings_[static_cast<size_t>(i)]), hotRc, kWhite);
                SelectObject(hdc, homeFont_);
                DrawTextIn(hdc, L"录制时间: " + recordings_[static_cast<size_t>(i)].recordTime, HomeCardMetaLeft(r), kSecondaryText);
                DrawTextIn(hdc, L"时长: " + FormatDuration(recordings_[static_cast<size_t>(i)].durationSeconds), HomeCardMetaRight(r), kSecondaryText);
                if (i == selectedRecording_) {
                    DrawTextIn(hdc, L"取消选择", RecordingDeselectRect(i), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    RECT tag = RecordingSelectedTagRect(i);
                    FillRectColor(hdc, tag, kSelectedYellow);
                    DrawTextIn(hdc, L"已选中\u2713", tag, kMainGreen, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else if (i == recordingHover_) {
                    DrawTextIn(hdc, L"优化", RecordingOptimizeRect(i), kWhite, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    // 与宏「编辑/删除」同列右对齐（「重命名」三字、「删除」两字）
                    DrawTextIn(hdc, L"重命名", RecordingRenameRect(i), kWhite, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    DrawTextIn(hdc, L"删除", RecordingDeleteRect(i), kWhite, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                }
            }
            RestoreDC(hdc, saved);
            PaintHomeScrollbar(hdc);
        }

        RECT hint = CreateRect();
        FillRectColor(hdc, hint, recording_ ? RGB(255, 230, 230) : kCreateYellow);
        // 黄条右上：录制坐标模式切换（自动识别 / 相对坐标 / 绝对坐标）
        {
            const RECT modeRc = RecorderModeRect();
            FillRoundRectColor(hdc, modeRc, kOrange, UiLen(4));
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, RecorderInputModeLabel(recorderSettings_.inputMode), modeRc,
                kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(hdc, bigFont_);
        const bool hold = GlobalHotkeyUsesHold();
        const std::wstring prefix = hold ? L"按住" : L"按";
        std::wstring suffix;
        if (recording_) suffix = L"键停止 录制";
        else if (selectedRecording_ >= 0) suffix = L"键开始 回放";
        else suffix = L"键开始 录制";
        const std::wstring hotText = globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text;
        const RECT keyBox = RecorderBannerKeyRect();
        const int gap = UiLen(20);
        const int prefixW = TextWidth(prefix, bigFont_);
        RECT prefixRc{keyBox.left - gap - prefixW, hint.top, keyBox.left - gap, hint.bottom};
        RECT suffixRc{keyBox.right + gap, hint.top, keyBox.right + gap + TextWidth(suffix, bigFont_), hint.bottom};
        const COLORREF bannerFg = recording_ ? RGB(180, 40, 40) : kBannerText;
        DrawTextIn(hdc, prefix, prefixRc, bannerFg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        FillRectColor(hdc, keyBox, recording_ ? RGB(220, 60, 60) : kOrange);
        DrawTextIn(hdc, hotText, keyBox, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, suffix, suffixRc, bannerFg, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"前往修改全局启停热键   在录制列表，您也可以为您的录制设置单独的热键", HomeFooterRect(), kFooterHint);
    }

    void PaintScriptCustomHome(HDC hdc) {
        const RECT header = ScriptCustomHeaderRect();
        SelectObject(hdc, bigFont_);
        DrawTextIn(hdc, L"AI 脚本助手", RECT{header.left, header.top, header.right, header.top + UiLen(30)},
            kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"通过自然语言描述需求，AI 将自动生成或修改自动化脚本。",
            RECT{header.left, header.top + UiLen(30), header.right, header.bottom},
            kFooterHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        int saved = SaveDC(hdc);
        RECT list = HomeListRect();
        IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
        for (int i = 0; i < static_cast<int>(agentConversations_.size()); ++i) {
            RECT r = HomeCardRect(i);
            if (r.bottom < list.top || r.top > list.bottom) continue;
            const COLORREF cardColor = i == agentConvHover_ ? kCardHoverGreen : kCardGreen;
            FillRectColor(hdc, r, cardColor);
            const auto& conv = agentConversations_[static_cast<size_t>(i)];
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, conv.name, HomeCardNameRect(r),
                kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            DrawTextIn(hdc, L"对话时间: " + conv.createdTime, HomeCardMetaLeft(r), kSecondaryText);
            DrawTextIn(hdc, L"对话轮数: " + std::to_wstring(conv.roundCount), HomeCardMetaRight(r), kSecondaryText);
            DrawTextIn(hdc, L"对话", AgentConvChatRect(i), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextIn(hdc, L"删除", AgentConvDeleteRect(i), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        RestoreDC(hdc, saved);
        if (MaxAgentConvScroll() > 0) PaintHomeScrollbar(hdc);

        RECT hint = CreateRect();
        FillRectColor(hdc, hint, kCreateYellow);
        SelectObject(hdc, bigFont_);
        DrawTextIn(hdc, L"开始 AI 对话", UiPadRect(hint, 0, 21, 0, 21),
            kBannerText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"点击上方按钮启动 AI 脚本助手，支持列表/读取/写入脚本",
            HomeFooterRect(), kFooterHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    void PaintMacroHome(HDC hdc) {
        DrawTopAction(hdc, ImportRect(), L"导入", 0);
        DrawTopAction(hdc, ExportRect(), L"导出", 1);
        DrawTopAction(hdc, TimerRect(), L"定时", 2);
        int saved = SaveDC(hdc);
        RECT list = HomeListRect();
        IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
        for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
            RECT r = HomeCardRect(i);
            if (r.bottom < list.top || r.top > list.bottom) continue;
            COLORREF cardColor = i == selectedScript_ ? kDarkGreen : (i == homeHover_ ? kCardHoverGreen : kCardGreen);
            FillRectColor(hdc, r, cardColor);
            RECT hotRc = ScriptHotkeyRect(i);
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, scripts_[static_cast<size_t>(i)].name, HomeCardNameRect(r), kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, hotFont_);
            DrawTextIn(hdc, ScriptHotkeyText(scripts_[static_cast<size_t>(i)]), hotRc, kWhite);
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, L"录制时间: " + scripts_[static_cast<size_t>(i)].recordTime, HomeCardMetaLeft(r), kSecondaryText);
            DrawTextIn(hdc, L"动作数: " + std::to_wstring(scripts_[static_cast<size_t>(i)].actionCount), HomeCardMetaRight(r), kSecondaryText);
            if (i == selectedScript_) {
                DrawTextIn(hdc, L"取消选择", HomeCardEditBtn(r), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (i == homeHover_) {
                    DrawTextIn(hdc, L"删除", HomeCardDeleteBtn(r), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                RECT tag = HomeCardSelectedTag(r);
                FillRectColor(hdc, tag, kSelectedYellow);
                DrawTextIn(hdc, L"已选中\u2713", tag, kMainGreen, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else if (i == homeHover_) {
                DrawTextIn(hdc, L"编辑", HomeCardEditBtn(r), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                DrawTextIn(hdc, L"删除", HomeCardDeleteBtn(r), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        RestoreDC(hdc, saved);
        PaintHomeScrollbar(hdc);
        RECT cr = CreateRect();
        FillRectColor(hdc, cr, kCreateYellow);
        SelectObject(hdc, bigFont_);
        if (selectedScript_ >= 0) {
            const bool hold = GlobalHotkeyUsesHold();
            const std::wstring prefix = hold ? L"按住" : L"按";
            const std::wstring suffix = L"键开始 运行宏";
            RECT hot = CommonHotRect();
            const int gap = UiLen(20);
            const int prefixW = TextWidth(prefix, bigFont_);
            const int suffixW = TextWidth(suffix, bigFont_);
            RECT prefixRc{hot.left - gap - prefixW, cr.top, hot.left - gap, cr.bottom};
            RECT suffixRc{hot.right + gap, cr.top, hot.right + gap + suffixW, cr.bottom};
            DrawTextIn(hdc, prefix, prefixRc, kBannerText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            FillRectColor(hdc, hot, kOrange);
            DrawTextIn(hdc, globalHotkey_.text, hot, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            DrawTextIn(hdc, suffix, suffixRc, kBannerText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            DrawMacroCreatePrompt(hdc, cr);
        }
        SelectObject(hdc, homeFont_); DrawTextIn(hdc, L"前往修改全局启停热键   在宏列表中，您也可以为您的宏设置单独热键", HomeFooterRect(), kFooterHint);
    }

    void PaintHomeScrollbar(HDC hdc) {
        if (ActiveHomeListMaxScroll() <= 0) return;
        // 主界面（宏列表 / 录制列表）共用绿圆角滚轮；编辑页细灰轨另绘，不要混用
        FillRoundRectColor(hdc, HomeScrollTrackRect(), kHomeScrollTrack, UiLen(10));
        FillRoundRectColor(hdc, HomeScrollThumbRect(), kHomeScrollThumb, UiLen(10));
    }

    // ── Editor screen painting ─────────────────────────────────────
    void PaintEditor(HDC hdc);
    void PaintEditorParamChrome(HDC hdc, HWND hdcWindow = nullptr);
    void PaintEditorListHeaderChrome(HDC hdc);

    void PaintEditorTipPopupContent(HDC hdc, HWND popupHwnd);

    SIZE MeasureQuickInputTipSize() const;

    void DrawEditorFieldBorder(HDC hdc, HWND ctrl, HWND hdcWindow = nullptr);
    bool IsParamViewportChild(HWND h) const { return paramViewport_ && h && GetParent(h) == paramViewport_; }
    RECT MapRectFromMain(HWND to, const RECT& rc) const {
        if (!to || to == hwnd_) return rc;
        RECT r = rc;
        MapWindowPoints(hwnd_, to, reinterpret_cast<POINT*>(&r), 2);
        return r;
    }

    void PaintActionList(HDC hdc);

    void PaintDragMarker(HDC hdc);

    LRESULT OnCtlColor(HDC hdc) { SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, kText); return reinterpret_cast<LRESULT>(whiteBrush_); }

    LRESULT OnCtlColorStatic(HDC hdc, HWND ctrl) {
        if (ctrl == paramBottomMask_) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kPanel);
            return reinterpret_cast<LRESULT>(panelBrush_);
        }
        if (ctrl == paramTopMask_ || ctrl == paramRightMask_) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kWhite);
            return reinterpret_cast<LRESULT>(whiteBrush_);
        }
        if (page_ == Page::Editor && ctrl && EditorComboPopupIdForHwnd(ctrl) < 0) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kWhite);
            SetTextColor(hdc, kText);
            return reinterpret_cast<LRESULT>(whiteBrush_);
        }
        return OnCtlColor(hdc);
    }
    LRESULT OnEditColor(HDC hdc) { SetBkMode(hdc, OPAQUE); SetTextColor(hdc, kText); SetBkColor(hdc, kWhite); return reinterpret_cast<LRESULT>(whiteBrush_); }

    bool KeyFunctionDebugActive() const {
        return appSettings_.playback.autoOutputKeyFunctionDebug
            && qst::desktop_tools::MacroDebug().IsCreated();
    }

    static bool IsPlaybackDebugMetaLine(const std::wstring& t) {
        if (t.compare(0, 4, L"[时间轴") == 0) return true;
        if (t.find(L"精密时间轴") != std::wstring::npos) return true;
        if (t.find(L"次执行鼠标宏") != std::wstring::npos) return true;
        if (t.find(L"逐步日志已封顶") != std::wstring::npos) return true;
        return false;
    }

    bool DeferredDetailDebugDropped() const {
        return deferPlaybackDebugUi_.load(std::memory_order_relaxed)
            && deferredDebugDropped_.load(std::memory_order_relaxed);
    }

    void ClearDeferredDebugLog() {
        std::lock_guard<std::mutex> lock(deferredDebugMutex_);
        deferredDebugRecs_.clear();
        deferredDebugTexts_.clear();
        deferredDebugDropped_.store(false, std::memory_order_relaxed);
    }

    void PrepareDeferredPlaybackDebug(size_t actionCount) {
        std::lock_guard<std::mutex> lock(deferredDebugMutex_);
        deferredDebugRecs_.clear();
        deferredDebugTexts_.clear();
        deferredDebugDropped_.store(false, std::memory_order_relaxed);
        const size_t cap = kMaxDeferredPlaybackDebugRecs;
        deferredDebugRecs_.reserve((std::min)(actionCount + 64, cap + 64));
        deferredDebugTexts_.reserve(256);
    }

    /// 刷出延后日志并关掉延后开关。必须在 PostMessage(WM_RUN_DONE) 之前调用，
    /// 否则 UI 会以为已结束并启动下一轮，与仍在格式化上万行日志的工作线程抢 CPU。
    void DisarmDeferredPlaybackDebugUi() {
        FlushDeferredDebugLog();
        deferPlaybackDebugUi_.store(false, std::memory_order_relaxed);
    }

    bool AcceptDeferredDetailLocked() {
        if (deferredDebugDropped_.load(std::memory_order_relaxed)) return false;
        if (deferredDebugRecs_.size() < kMaxDeferredPlaybackDebugRecs) return true;
        deferredDebugDropped_.store(true, std::memory_order_relaxed);
        DeferredDbgRec rec{};
        rec.kind = DeferredDbgKind::Line;
        rec.textIdx = deferredDebugTexts_.size();
        deferredDebugTexts_.push_back(
            L"逐步日志已封顶（避免长录制多轮回放把时间轴拖变形）；后续轮次只保留时间轴统计");
        deferredDebugRecs_.push_back(rec);
        return false;
    }

    void FlushDeferredDebugLog() {
        std::vector<DeferredDbgRec> recs;
        std::vector<std::wstring> texts;
        {
            std::lock_guard<std::mutex> lock(deferredDebugMutex_);
            recs.swap(deferredDebugRecs_);
            texts.swap(deferredDebugTexts_);
        }
        if (recs.empty() || !qst::desktop_tools::MacroDebug().IsCreated()) return;
        std::vector<std::wstring> batch;
        batch.reserve(recs.size());
        for (const auto& r : recs) {
            switch (r.kind) {
            case DeferredDbgKind::Line:
                if (r.textIdx < texts.size()) batch.push_back(texts[r.textIdx]);
                break;
            case DeferredDbgKind::Wait: {
                ScriptAction tmp{};
                tmp.type = ActionType::Wait;
                tmp.originalNo = r.no;
                tmp.duration = r.duration;
                tmp.randomDuration = r.randomDuration;
                std::wstring line = FormatGenericActionDebug(tmp);
                wchar_t suf[48]{};
                swprintf_s(suf, L" late=%lluus",
                    static_cast<unsigned long long>(r.lateUs));
                line += suf;
                batch.push_back(std::move(line));
                break;
            }
            case DeferredDbgKind::MoveRel: {
                ScriptAction tmp{};
                tmp.type = ActionType::MoveMouseRelative;
                tmp.originalNo = r.no;
                batch.push_back(FormatMoveMouseRelativeDebug(tmp, r.x, r.y));
                break;
            }
            case DeferredDbgKind::MoveAbs: {
                ScriptAction tmp{};
                tmp.type = ActionType::MoveMouse;
                tmp.originalNo = r.no;
                batch.push_back(FormatMoveMouseDebug(tmp, r.x, r.y));
                break;
            }
            }
        }
        if (!batch.empty()) qst::desktop_tools::MacroDebug().AppendLogBatch(batch);
    }

    void PushDeferredDebugLine(std::wstring text) {
        const bool meta = IsPlaybackDebugMetaLine(text);
        if (!meta && deferredDebugDropped_.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(deferredDebugMutex_);
        if (!meta && !AcceptDeferredDetailLocked()) return;
        DeferredDbgRec rec{};
        rec.kind = DeferredDbgKind::Line;
        rec.textIdx = deferredDebugTexts_.size();
        deferredDebugTexts_.push_back(std::move(text));
        deferredDebugRecs_.push_back(rec);
    }

    void AppendDebugLog(const std::wstring& text) {
        if (!KeyFunctionDebugActive()) return;
        if (deferPlaybackDebugUi_.load(std::memory_order_relaxed)) {
            PushDeferredDebugLine(text);
            return;
        }
        qst::desktop_tools::MacroDebug().AppendLog(text);
    }

    void AppendDeferredWaitDebug(const ScriptAction& a, uint64_t lateUs, double timeScale = 1.0) {
        if (!KeyFunctionDebugActive()) return;
        ScriptAction shown = a;
        shown.duration = quickscript::ScalePlaybackTimeSeconds(a.duration, timeScale);
        shown.randomDuration = quickscript::ScalePlaybackTimeSeconds(a.randomDuration, timeScale);
        if (!deferPlaybackDebugUi_.load(std::memory_order_relaxed)) {
            std::wstring line = FormatGenericActionDebug(shown);
            wchar_t suf[48]{};
            swprintf_s(suf, L" late=%lluus",
                static_cast<unsigned long long>(lateUs));
            line += suf;
            qst::desktop_tools::MacroDebug().AppendLog(line);
            return;
        }
        if (deferredDebugDropped_.load(std::memory_order_relaxed)) return;
        DeferredDbgRec rec{};
        rec.kind = DeferredDbgKind::Wait;
        rec.no = ActionDebugIndex(shown);
        rec.duration = shown.duration;
        rec.randomDuration = shown.randomDuration;
        rec.lateUs = lateUs;
        std::lock_guard<std::mutex> lock(deferredDebugMutex_);
        if (!AcceptDeferredDetailLocked()) return;
        deferredDebugRecs_.push_back(rec);
    }

    void AppendDeferredMoveRelDebug(const ScriptAction& a, int dx, int dy) {
        if (!KeyFunctionDebugActive()) return;
        if (!deferPlaybackDebugUi_.load(std::memory_order_relaxed)) {
            qst::desktop_tools::MacroDebug().AppendLog(FormatMoveMouseRelativeDebug(a, dx, dy));
            return;
        }
        if (deferredDebugDropped_.load(std::memory_order_relaxed)) return;
        DeferredDbgRec rec{};
        rec.kind = DeferredDbgKind::MoveRel;
        rec.no = ActionDebugIndex(a);
        rec.x = dx;
        rec.y = dy;
        std::lock_guard<std::mutex> lock(deferredDebugMutex_);
        if (!AcceptDeferredDetailLocked()) return;
        deferredDebugRecs_.push_back(rec);
    }

    void AppendDeferredMoveAbsDebug(const ScriptAction& a, int x, int y) {
        if (!KeyFunctionDebugActive()) return;
        if (!deferPlaybackDebugUi_.load(std::memory_order_relaxed)) {
            qst::desktop_tools::MacroDebug().AppendLog(FormatMoveMouseDebug(a, x, y));
            return;
        }
        if (deferredDebugDropped_.load(std::memory_order_relaxed)) return;
        DeferredDbgRec rec{};
        rec.kind = DeferredDbgKind::MoveAbs;
        rec.no = ActionDebugIndex(a);
        rec.x = x;
        rec.y = y;
        std::lock_guard<std::mutex> lock(deferredDebugMutex_);
        if (!AcceptDeferredDetailLocked()) return;
        deferredDebugRecs_.push_back(rec);
    }

    void AppendBreakoutDebugLog(const std::wstring& text) {
        if (!qst::desktop_tools::MacroDebug().IsCreated()) return;
        qst::desktop_tools::MacroDebug().AppendLog(text);
    }

    void AppendAiDebugLog(const std::wstring& text) {
        if (!qst::desktop_tools::MacroDebug().IsCreated()) return;
        qst::desktop_tools::MacroDebug().AppendLog(text);
    }

    void StoreAiOutputVar(const std::wstring& outputVarName, int outputType,
        const std::wstring& textResult, const std::wstring& fallback) {
        if (!textResult.empty()) {
            if (outputType == 1) {
                double num = 0;
                try { num = std::stod(textResult); }
                catch (...) { num = 0; }
                aiVars_[outputVarName] = std::to_wstring(static_cast<int>(num));
            } else {
                aiVars_[outputVarName] = textResult;
            }
        } else {
            aiVars_[outputVarName] = fallback;
        }
    }

    std::wstring EffectiveAiModelName(const ScriptAction& a) const {
        return ResolveActionAiModelName(a, appSettings_.ai);
    }

    AiActionResult RunAiTextAnalysisForAction(
        const ScriptAction& a,
        const std::wstring& resolvedPrompt,
        AiSessionStore* sessions = nullptr,
        int loopDepth = 0) {
        AiActionResult result;
        result.errorMessage = L"AI 未启用或未配置模型";
        const std::wstring modelName = EffectiveAiModelName(a);
        if (modelName.empty() || !appSettings_.ai.enabled) return result;

        const int timeoutMs = std::max(5000, a.aiTimeoutSec > 0 ? a.aiTimeoutSec * 1000 : 30000);
        static const wchar_t* kAiTextSystemPrompt =
            L"文本分析助手。只输出用户要求的结果，不要解释或 Markdown，尽量简短。";

        ScriptAction prepAction = a;
        prepAction.aiModelName = modelName;
        AgentCore* corePtr = nullptr;
        std::unique_ptr<AgentCore> ownedCore = PrepareAiAnalysisCore(
            sessions, prepAction, loopDepth, kAiTextSystemPrompt, appSettings_, timeoutMs, 512, corePtr);
        AgentCore* core = corePtr ? corePtr : ownedCore.get();
        if (!core) {
            result.errorMessage = L"无法创建 AI 客户端";
            return result;
        }

        const size_t histBefore = core->GetHistory().size();
        AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };
        result = ExecuteAiTextAnalysis(
            core, resolvedPrompt, a.aiOutputType, stopFlag_, a.aiTimeoutSec,
            logFn, &aiHttpAbort_, a.aiContextMode);
        if (result.ok && sessions) {
            sessions->PropagateHistoryAfterCall(
                a.aiContextMode, loopDepth, core, histBefore,
                prepAction, kAiTextSystemPrompt, appSettings_, timeoutMs, false, 512);
        }
        return result;
    }

    AiActionResult RunAiImageAnalysisForAction(
        const ScriptAction& a,
        const std::wstring& resolvedPrompt,
        const std::string& screenshotBase64,
        AiSessionStore* sessions = nullptr,
        int loopDepth = 0,
        const std::vector<std::string>* extraImageJpegBase64 = nullptr) {
        AiActionResult result;
        result.errorMessage = L"AI 未启用或未配置模型";
        const std::wstring modelName = EffectiveAiModelName(a);
        if (modelName.empty() || !appSettings_.ai.enabled) return result;
        if (screenshotBase64.empty()) {
            result.errorMessage = L"截屏编码失败";
            return result;
        }

        const int timeoutMs = ResolveAiImageAnalysisTimeoutSec(a.aiTimeoutSec, resolvedPrompt.size()) * 1000;
        static const wchar_t* kAiImageSystemPrompt =
            L"截图分析助手。只输出用户要求的结果，不要解释或 Markdown，尽量简短。";

        ScriptAction prepAction = a;
        prepAction.aiModelName = modelName;
        AgentCore* corePtr = nullptr;
        std::unique_ptr<AgentCore> ownedCore = PrepareAiAnalysisCore(
            sessions, prepAction, loopDepth, kAiImageSystemPrompt, appSettings_, timeoutMs, 1024, corePtr);
        AgentCore* core = corePtr ? corePtr : ownedCore.get();
        if (!core) {
            result.errorMessage = L"无法创建 AI 客户端";
            return result;
        }

        const size_t histBefore = core->GetHistory().size();
        AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };
        result = ExecuteAiImageAnalysis(
            core, resolvedPrompt, screenshotBase64, a.aiOutputType,
            stopFlag_, a.aiTimeoutSec, logFn, &aiHttpAbort_, a.aiContextMode,
            extraImageJpegBase64);
        if (result.ok && sessions) {
            sessions->PropagateHistoryAfterCall(
                a.aiContextMode, loopDepth, core, histBefore,
                prepAction, kAiImageSystemPrompt, appSettings_, timeoutMs, false, 1024);
        }
        return result;
    }

    AiActionResult RunAiActionExecuteForAction(
        const ScriptAction& a,
        const std::wstring& resolvedPrompt,
        const std::string& screenshotBase64,
        int captureWidth,
        int captureHeight,
        AiSessionStore* sessions = nullptr,
        int loopDepth = 0,
        const AiCaptureMapping* captureMapping = nullptr,
        const std::vector<std::string>* extraImageJpegBase64 = nullptr) {
        AiActionResult result;
        result.errorMessage = L"AI 未启用或未配置模型";
        const std::wstring modelName = EffectiveAiModelName(a);
        if (modelName.empty() || !appSettings_.ai.enabled) return result;

        const bool withImage = !screenshotBase64.empty();
        const AiActionRouteKind route = ClassifyAiActionRoute(resolvedPrompt, withImage);
        const int effectiveTimeoutSec = ResolveAiActionExecuteTimeoutSec(a.aiTimeoutSec, withImage);
        const int timeoutMs = effectiveTimeoutSec * 1000;

        // 模型来源必须可见：曾经「静默换成识图模型」让用户以为软件用错了模型
        if (!modelName.empty() && !appSettings_.ai.modelName.empty()
            && modelName != appSettings_.ai.modelName) {
            AppendAiDebugLog(L"  [诊断] 本动作使用模型「" + modelName
                + L"」（动作里指定的），与设置→AI助手的当前模型「"
                + appSettings_.ai.modelName + L"」不同；要改请在编辑器 AI 动作的模型下拉里换");
        }

        // 带图执行：工具 Agent 保留文本主模型（识图走 locate 子模型）；
        // Vision/Composite 改用列表识图模型；皆无则终止本步。
        ScriptAction prepAction = a;
        std::wstring execModel = modelName;
        if (withImage && !ModelSupportsVision(execModel)) {
            const std::wstring vm = ResolveVisionSubtaskModelName(appSettings_.ai, modelName);
            const bool toolAgent = (route == AiActionRouteKind::ToolExecute
                || route == AiActionRouteKind::MultiTurnTools);
            if (vm.empty()) {
                result.errorMessage = MissingVisionModelError(modelName);
                AppendAiDebugLog(L"  [错误] " + result.errorMessage);
                return result;
            }
            if (toolAgent) {
                AppendAiDebugLog(L"  [诊断] 主模型「" + modelName
                    + L"」非多模态：规划轮用文本模型；locateAndClick 识图将改用「" + vm + L"」");
            } else {
                AppendAiDebugLog(L"  [诊断] 主模型「" + modelName
                    + L"」非多模态，带图执行改用「" + vm + L"」");
                execModel = vm;
            }
        }
        prepAction.aiModelName = execModel;
        AgentCore* corePtr = nullptr;
        std::unique_ptr<AgentCore> ownedCore = PrepareAiActionExecuteCore(
            sessions, prepAction, loopDepth, route, captureWidth, captureHeight,
            appSettings_, timeoutMs, corePtr);
        AgentCore* core = corePtr ? corePtr : ownedCore.get();
        if (!core) {
            result.errorMessage = L"无法创建 AI 客户端";
            return result;
        }

        const size_t histBefore = core->GetHistory().size();
        AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };
        result = ExecuteAiActionExecute(
            core, resolvedPrompt, screenshotBase64, captureWidth, captureHeight, a.aiContextMode,
            stopFlag_, a.aiTimeoutSec, logFn, &aiHttpAbort_, captureMapping, nullptr, 10,
            extraImageJpegBase64);
        if (result.ok && sessions) {
            std::wstring sysPrompt;
            if (route == AiActionRouteKind::VisionQuery || route == AiActionRouteKind::CompositeClick) {
                sysPrompt = BuildAiActionVisionQuerySystemPrompt(captureWidth, captureHeight);
            } else if (route == AiActionRouteKind::MultiTurnTools) {
                sysPrompt = BuildAiActionHybridSystemPrompt(captureWidth, captureHeight);
            } else if (withImage) {
                sysPrompt = BuildAiActionExecuteSystemPrompt(captureWidth, captureHeight);
            } else {
                sysPrompt = BuildAiActionExecuteTextSystemPrompt();
            }
            const bool useTools = route == AiActionRouteKind::ToolExecute
                || route == AiActionRouteKind::MultiTurnTools;
            sessions->PropagateHistoryAfterCall(
                a.aiContextMode, loopDepth, core, histBefore,
                prepAction, sysPrompt, appSettings_, timeoutMs, useTools, 1024);
        }
        return result;
    }

    void OnDebugWindowClosedByUser() {
        qst::desktop_tools::MacroDebug().MarkWebHidden();
        debugWindowSettingKnown_ = true;
        debugWindowSettingApplied_ = false;
        if (!appSettings_.playback.enableDebugOutputWindow) return;
        SaveAppSettingsPreserveUserSettings(appSettings_, false);
        appSettings_.playback.enableDebugOutputWindow = false;
        SaveAppSettings(appSettings_);
        NotifyActiveSettingsDialogSync();
#if defined(QST_WEBVIEW_WITH_ENGINE) && QST_WEBVIEW_WITH_ENGINE
        qst::webview::NotifyWebDebugWindowSetting(false);
#endif
    }

    void ShowDebugWindow() {
        // Web 壳：独立 WebView 顶层窗；GDI：MacroDebugWindow
        qst::desktop_tools::MacroDebug().SetWebUiEnabled(headlessUi_);
        if (!qst::desktop_tools::MacroDebug().IsCreated()) {
            qst::desktop_tools::MacroDebug().Create(font_, titleFont_, closeFont_, [this]() {
                OnDebugWindowClosedByUser();
            });
        }
        qst::desktop_tools::MacroDebug().Show();
    }

    void HideDebugWindow() {
        qst::desktop_tools::MacroDebug().SetWebUiEnabled(headlessUi_);
        qst::desktop_tools::MacroDebug().Hide();
    }

    void ApplyDebugWindowSetting() {
        // 仅在开关变化时 Show/Hide，避免切 Tab→saveSettings→ReloadSettings 反复抢焦点
        const bool want = appSettings_.playback.enableDebugOutputWindow;
        if (want == debugWindowSettingApplied_ && debugWindowSettingKnown_) {
            return;
        }
        debugWindowSettingKnown_ = true;
        debugWindowSettingApplied_ = want;
        if (want) ShowDebugWindow();
        else HideDebugWindow();
    }

    void ApplyOtherOsSettings() {
        SetAutoStartOnBoot(appSettings_.other.autoStartOnBoot);
        ghHoldThresholdMs.store(
            HoldThresholdMsFromSeconds(appSettings_.other.holdThresholdSeconds),
            std::memory_order_relaxed);
        UiScaleSetUserFactor(appSettings_.other.uiScaleFactor);
        // 「中文输入法不触发热键」：切换 RegisterHotKey / LL 放行模式
        RegisterAllHotkeys();
    }

    void UpdateStatusTip() {
        if (headlessUi_) { HideStatusTip(); return; }
        if (appSettings_.other.hideBottomRightTip) { HideStatusTip(); return; }
        const wchar_t* text = breakoutPaused_.load(std::memory_order_relaxed) ? L"脱离中..."
            : (clicking_ ? L"连点中..." : (recording_ ? L"录制中..." : (running_ ? L"回放中..." : L"")));
        if (!text[0]) { HideStatusTip(); return; }
        if (!statusTipWindow_) {
            statusTipWindow_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                L"STATIC", text, WS_POPUP | SS_CENTER | SS_CENTERIMAGE,
                0, 0, 160, 36, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(statusTipWindow_, WM_SETFONT, reinterpret_cast<WPARAM>(homeFont_), TRUE);
        }
        SetWindowTextW(statusTipWindow_, text);
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(statusTipWindow_, HWND_TOPMOST, sw - 180, sh - 80, 160, 36, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }

    void HideStatusTip() {
        if (statusTipWindow_) ShowWindow(statusTipWindow_, SW_HIDE);
    }

    void Cleanup() { EndHotkeyCaptureRelease(); ClearAllHoldSessionLatches(); FlushHomeStatePersist(); SaveHomeState(); outerShadow_.Detach(); FiDbgShutdown(); if (crosshairDrag_.IsActive()) crosshairDrag_.End(); CloseEditorPopup(); CloseClickerDropPopup(); CancelQuickInputTip(); StopScheduledTaskQueueTimer(); KillTimer(hwnd_, kScheduledTaskTimerId); KillTimer(hwnd_, kHotkeyLatchSyncTimerId); KillTimer(hwnd_, kHomeStatePersistTimerId); if (editorDropPopup_) { DestroyWindow(editorDropPopup_); editorDropPopup_ = nullptr; } if (clickerDropPopup_) { DestroyWindow(clickerDropPopup_); clickerDropPopup_ = nullptr; } if (editorTipPopup_) { DestroyWindow(editorTipPopup_); editorTipPopup_ = nullptr; } qst::desktop_tools::DestroyMacroDebug(); if (statusTipWindow_) { DestroyWindow(statusTipWindow_); statusTipWindow_ = nullptr; } StopClickerCleanup(); StopRecordingCleanup(); ForceEndBreakoutUiState(); breakout_input::UninstallBreakoutHooks(); synthetic_input::UnregisterRawInputSink(hwnd_); ghWorkerCancelFlag = nullptr; stopFlag_ = true; if (worker_.joinable()) worker_.detach(); ReleaseAllHeldInputs(); if (trayActive_) { NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd_; nid.uID = 1; Shell_NotifyIconW(NIM_DELETE, &nid); trayActive_ = false; } ghPlaybackHotkeySuspended = false; ghPassThroughTypingHotkeys.store(false, std::memory_order_relaxed); ghPlaybackScriptHookCount = 0; UnregisterHotKey(hwnd_, HOTKEY_GLOBAL_ID); for (int i = 0; i < 100; ++i) { UnregisterHotKey(hwnd_, HOTKEY_SCRIPT_BASE + i); UnregisterHotKey(hwnd_, HOTKEY_RECORDING_BASE + i); } UninstallGlobalHotkeyHooks(); if (crosshairDragCursor_) { DestroyCursor(crosshairDragCursor_); crosshairDragCursor_ = nullptr; } if (findImagePreviewBitmap_) { DeleteBitmapHandle(findImagePreviewBitmap_); findImagePreviewBitmap_ = nullptr; } if (ocrFindImagePreviewBitmap_) { DeleteBitmapHandle(ocrFindImagePreviewBitmap_); ocrFindImagePreviewBitmap_ = nullptr; } if (aiFindImagePreviewBitmap_) { DeleteBitmapHandle(aiFindImagePreviewBitmap_); aiFindImagePreviewBitmap_ = nullptr; } DeleteObject(font_); DeleteObject(editorFont_); DeleteObject(bigFont_); DeleteObject(titleFont_); DeleteObject(hotFont_); DeleteObject(closeFont_); DeleteObject(homeFont_); DeleteObject(homeTabFont_); DeleteObject(whiteBrush_); DeleteObject(panelBrush_); DeleteObject(lineGreenBrush_); }
    void StopClickerCleanup();
    void StopRecordingCleanup() {
        if (recording_) {
            recording_ = false;
            UninstallRecordingHooks();
            g_recording = false;
            SetRecordingDebugSink({});
            DiscardClickCaptures();
            EndHighResTimer();
        }
    }

    HWND hwnd_ = nullptr; HMONITOR lastUiMonitor_ = nullptr; int lastUiScreenW_ = 0; int lastUiScreenH_ = 0;
    int lastUiScalePercent_ = 100; int displaySyncPass_ = 0;
    HFONT font_ = nullptr; HFONT editorFont_ = nullptr; HFONT bigFont_ = nullptr; HFONT titleFont_ = nullptr; HFONT hotFont_ = nullptr; HFONT closeFont_ = nullptr; HFONT homeFont_ = nullptr; HFONT homeTabFont_ = nullptr; HBRUSH whiteBrush_ = nullptr; HBRUSH panelBrush_ = nullptr; HBRUSH lineGreenBrush_ = nullptr;
    HWND labelMacro_ = nullptr; HWND name_ = nullptr; HWND labelBreakoutTime_ = nullptr; HWND breakoutTimeEdit_ = nullptr; HWND mode_ = nullptr; HWND labelList_ = nullptr; HWND labelBatchCount_ = nullptr; HWND actionCombo_ = nullptr; HWND addBtn_ = nullptr; HWND modifyBtn_ = nullptr; HWND clearBtn_ = nullptr; HWND loadBtn_ = nullptr;
    HWND batchExitBtn_ = nullptr; HWND batchSelectAllBtn_ = nullptr; HWND batchDeselectBtn_ = nullptr; HWND batchDeleteBtn_ = nullptr; HWND batchCopyBtn_ = nullptr;
    HWND cancelBtn_ = nullptr; HWND saveBtn_ = nullptr; HWND crosshairBtn_ = nullptr; HWND paramViewport_ = nullptr; HWND paramTopMask_ = nullptr; HWND paramBottomMask_ = nullptr; HWND paramRightMask_ = nullptr;
    HWND runProgramCombo_ = nullptr; HWND runProgramPath_ = nullptr; HWND runProgramBrowseBtn_ = nullptr; HWND runProgramOrLabel_ = nullptr; HWND runProgramCrosshairBtn_ = nullptr; HWND runProgramArgs_ = nullptr;
    HWND closeProgramPath_ = nullptr; HWND closeProgramBrowseBtn_ = nullptr; HWND closeProgramOrLabel_ = nullptr; HWND closeProgramCrosshairBtn_ = nullptr; HWND closeProgramMatchFileName_ = nullptr;
    HWND openWebpageUrl_ = nullptr; HWND openFilePath_ = nullptr; HWND openFileBrowseBtn_ = nullptr; HWND timerVarName_ = nullptr; HWND cursorPosVarName_ = nullptr; HWND gotoStepEdit_ = nullptr;
    HWND moveHintLabel_ = nullptr; HWND moveXLabel_ = nullptr; HWND moveYLabel_ = nullptr;
    HWND moveRandomXLabel_ = nullptr; HWND moveRandomYLabel_ = nullptr;
    HWND moveVarXLabel_ = nullptr; HWND moveVarYLabel_ = nullptr; HWND moveHintFooter_ = nullptr;
    HWND moveX_ = nullptr; HWND moveY_ = nullptr; HWND moveRandomX_ = nullptr; HWND moveRandomY_ = nullptr; HWND moveFromVar_ = nullptr; HWND moveVarX_ = nullptr; HWND moveVarY_ = nullptr; HWND moveRelX_ = nullptr; HWND moveRelY_ = nullptr; HWND moveRelRandomX_ = nullptr; HWND moveRelRandomY_ = nullptr; HWND waitDuration_ = nullptr; HWND waitRandom_ = nullptr; HWND clickButton_ = nullptr; HWND clickCount_ = nullptr; HWND clickWait_ = nullptr; HWND clickRandom_ = nullptr;
    HWND keyEdit_ = nullptr; HWND keyPressEdit_ = nullptr; HWND keyCount_ = nullptr; HWND keyWait_ = nullptr; HWND keyRandom_ = nullptr; HWND loopTypeCombo_ = nullptr; HWND loopCount_ = nullptr; HWND loopFromVar_ = nullptr; HWND loopVarExpr_ = nullptr; HWND loopVarName_ = nullptr; HWND defineBlockName_ = nullptr; HWND runBlockCombo_ = nullptr; HWND remarkLabel_ = nullptr; HWND remark_ = nullptr; HWND listRemarkEdit_ = nullptr;
    HWND clickLWin_ = nullptr; HWND clickRWin_ = nullptr; HWND clickLCtrl_ = nullptr; HWND clickRCtrl_ = nullptr; HWND clickLAlt_ = nullptr; HWND clickRAlt_ = nullptr; HWND clickLShift_ = nullptr; HWND clickRShift_ = nullptr;
    HWND mousePressButton_ = nullptr; HWND mousePressLWin_ = nullptr; HWND mousePressRWin_ = nullptr; HWND mousePressLCtrl_ = nullptr; HWND mousePressRCtrl_ = nullptr; HWND mousePressLAlt_ = nullptr; HWND mousePressRAlt_ = nullptr; HWND mousePressLShift_ = nullptr; HWND mousePressRShift_ = nullptr;
    HWND keyLWin_ = nullptr; HWND keyRWin_ = nullptr; HWND keyLCtrl_ = nullptr; HWND keyRCtrl_ = nullptr; HWND keyLAlt_ = nullptr; HWND keyRAlt_ = nullptr; HWND keyLShift_ = nullptr; HWND keyRShift_ = nullptr;
    HWND keyPressLWin_ = nullptr; HWND keyPressRWin_ = nullptr; HWND keyPressLCtrl_ = nullptr; HWND keyPressRCtrl_ = nullptr; HWND keyPressLAlt_ = nullptr; HWND keyPressRAlt_ = nullptr; HWND keyPressLShift_ = nullptr; HWND keyPressRShift_ = nullptr;
    HWND hotkeyShortcutCombo_ = nullptr; HWND hotkeyShortcutCount_ = nullptr; HWND hotkeyShortcutWait_ = nullptr; HWND hotkeyShortcutRandom_ = nullptr;
    HWND runMacroCombo_ = nullptr; HWND mousePlaybackCombo_ = nullptr; HWND mousePlaybackCount_ = nullptr; HWND mousePlaybackWait_ = nullptr; HWND mousePlaybackRandom_ = nullptr;
    HWND scrollVertical_ = nullptr; HWND scrollHorizontal_ = nullptr; HWND scrollSteps_ = nullptr; HWND scrollDirectionCombo_ = nullptr; HWND scrollCount_ = nullptr; HWND scrollWait_ = nullptr; HWND scrollRandom_ = nullptr;
    HWND findRegionLabel_ = nullptr; HWND findFullScreenBtn_ = nullptr; HWND findSelectRegionBtn_ = nullptr;
    HWND findImageHeaderLabel_ = nullptr;
    HWND findX1Label_ = nullptr; HWND findY1Label_ = nullptr; HWND findX2Label_ = nullptr; HWND findY2Label_ = nullptr;
    HWND findX1_ = nullptr; HWND findY1_ = nullptr; HWND findX2_ = nullptr; HWND findY2_ = nullptr;
    HWND findFollowUpLabel_ = nullptr; HWND findFollowUpCombo_ = nullptr;
    HWND findOffsetXLabel_ = nullptr; HWND findOffsetYLabel_ = nullptr; HWND findMatchVarLabel_ = nullptr;
    HWND findTestBtn_ = nullptr; HWND findImagePreviewBtn_ = nullptr; HWND findScreenshotBtn_ = nullptr; HWND findLocalImageBtn_ = nullptr; HWND findClearImageBtn_ = nullptr;
    HWND findMatchThreshold_ = nullptr; HWND findScaleMin_ = nullptr; HWND findScaleMax_ = nullptr; HWND findOffsetX_ = nullptr; HWND findOffsetY_ = nullptr; HWND findSelectOffsetBtn_ = nullptr; HWND findTimeLabel_ = nullptr; HWND findTimeEdit_ = nullptr; HWND findMatchVar_ = nullptr;
    HWND ocrDepStatusLabel_ = nullptr; HWND ocrDepInstallBtn_ = nullptr;
    HWND ocrRegionLabel_ = nullptr; HWND ocrFullScreenBtn_ = nullptr; HWND ocrSelectRegionBtn_ = nullptr;
    HWND ocrX1_ = nullptr; HWND ocrY1_ = nullptr; HWND ocrX2_ = nullptr; HWND ocrY2_ = nullptr;
    HWND ocrX1Label_ = nullptr; HWND ocrY1Label_ = nullptr; HWND ocrX2Label_ = nullptr; HWND ocrY2Label_ = nullptr;
    HWND ocrResultModeLabel_ = nullptr; HWND ocrResultModeCombo_ = nullptr;
    HWND ocrSearchLabel_ = nullptr; HWND ocrSearchEdit_ = nullptr; HWND ocrSearchVarLabel_ = nullptr; HWND ocrSearchVarCombo_ = nullptr; HWND ocrSearchVarInsertBtn_ = nullptr;
    HWND ocrFollowUpLabel_ = nullptr; HWND ocrFollowUpCombo_ = nullptr;
    HWND ocrOffsetXLabel_ = nullptr; HWND ocrOffsetX_ = nullptr; HWND ocrOffsetYLabel_ = nullptr; HWND ocrOffsetY_ = nullptr;
    HWND ocrSelectOffsetBtn_ = nullptr;
    HWND ocrResultVarLabel_ = nullptr; HWND ocrUntilFound_ = nullptr; HWND ocrResultVar_ = nullptr; HWND ocrTestBtn_ = nullptr;
    HWND ocrRegionByImageCheck_ = nullptr;
    HWND ocrDigitsOnlyCheck_ = nullptr;
    HWND ocrFindImageLabel_ = nullptr; HWND ocrFindSelectRegionBtn_ = nullptr; HWND ocrFindImagePreviewBtn_ = nullptr;
    HWND ocrImageRegionX1_ = nullptr; HWND ocrImageRegionY1_ = nullptr;
    HWND ocrImageRegionX2_ = nullptr; HWND ocrImageRegionY2_ = nullptr;
    HWND ocrFindScreenshotBtn_ = nullptr; HWND ocrFindLocalImageBtn_ = nullptr; HWND ocrFindClearImageBtn_ = nullptr;
    HWND ocrFindMatchThreshold_ = nullptr; HWND ocrFindScaleMin_ = nullptr; HWND ocrFindScaleMax_ = nullptr;
    // ── AI 动作控制 ──
    HWND aiPromptLabel_ = nullptr;
    HWND aiPromptEdit_ = nullptr; HWND aiVarLabel_ = nullptr; HWND aiVarCombo_ = nullptr; HWND aiInsertVarBtn_ = nullptr;
    HWND aiModelLabel_ = nullptr;
    HWND aiModelCombo_ = nullptr;
    HWND aiContextLabel_ = nullptr;
    HWND aiContextModeCombo_ = nullptr;
    HWND aiTimeoutLabel_ = nullptr;
    HWND aiOutputVarLabel_ = nullptr;
    HWND aiOutputVarEdit_ = nullptr;
    HWND aiOutputTypeLabel_ = nullptr;
    HWND aiOutputTypeCombo_ = nullptr;
    HWND aiTimeoutEdit_ = nullptr; HWND aiFallbackLabel_ = nullptr; HWND aiFallbackEdit_ = nullptr;
    HWND aiImageScaleLabel_ = nullptr;
    HWND aiImageScaleEdit_ = nullptr;
    HWND aiRegionByImageCheck_ = nullptr;
    HWND aiRegionLabel_ = nullptr;
    HWND aiFullScreenBtn_ = nullptr;
    HWND aiSelectRegionBtn_ = nullptr;
    HWND aiCoordX1Label_ = nullptr; HWND aiCoordY1Label_ = nullptr;
    HWND aiCoordX2Label_ = nullptr; HWND aiCoordY2Label_ = nullptr;
    HWND aiSearchX1Edit_ = nullptr; HWND aiSearchY1Edit_ = nullptr;
    HWND aiSearchX2Edit_ = nullptr; HWND aiSearchY2Edit_ = nullptr;
    HWND aiFindImageLabel_ = nullptr; HWND aiFindSelectRegionBtn_ = nullptr; HWND aiFindImagePreviewBtn_ = nullptr;
    HWND aiImageRegionX1_ = nullptr; HWND aiImageRegionY1_ = nullptr;
    HWND aiImageRegionX2_ = nullptr; HWND aiImageRegionY2_ = nullptr;
    HWND aiFindScreenshotBtn_ = nullptr; HWND aiFindLocalImageBtn_ = nullptr; HWND aiFindClearImageBtn_ = nullptr;
    HWND aiFindMatchLabel_ = nullptr; HWND aiFindMatchThreshold_ = nullptr; HWND aiFindMatchPctLabel_ = nullptr;
    HWND aiFindScaleMinLabel_ = nullptr; HWND aiFindScaleMin_ = nullptr;
    HWND aiFindScaleMaxLabel_ = nullptr; HWND aiFindScaleMax_ = nullptr;
    HWND aiRegionByImageCheck2_ = nullptr;
    HWND aiActionRegionLabel_ = nullptr;
    HWND aiFullScreenBtn2_ = nullptr;
    HWND aiSelectRegionBtn2_ = nullptr;
    HWND aiActCoordX1Label_ = nullptr; HWND aiActCoordY1Label_ = nullptr;
    HWND aiActCoordX2Label_ = nullptr; HWND aiActCoordY2Label_ = nullptr;
    HWND aiSearchX1Edit2_ = nullptr; HWND aiSearchY1Edit2_ = nullptr;
    HWND aiSearchX2Edit2_ = nullptr; HWND aiSearchY2Edit2_ = nullptr;
    HWND aiMaxStepsLabel_ = nullptr;
    HWND aiMaxStepsEdit_ = nullptr;
    HWND aiWithImageCheck_ = nullptr;
    HWND aiLogicConvertCheck_ = nullptr;
    HWND aiMaxStepsHint_ = nullptr;
    HWND aiSearchRegionCombo_ = nullptr;
    HWND quickInputEdit_ = nullptr; HWND quickInputVarCombo_ = nullptr; HWND quickInputInsertBtn_ = nullptr; HWND quickInputCharInterval_ = nullptr; HWND quickInputCount_ = nullptr; HWND quickInputWait_ = nullptr; HWND quickInputRandom_ = nullptr;
    HWND ifVarCombo_ = nullptr; HWND ifOperatorCombo_ = nullptr; HWND ifValueEdit_ = nullptr; HWND ifConnectorCombo_ = nullptr; HWND ifAddConditionBtn_ = nullptr; HWND ifConditionList_ = nullptr;
    std::vector<HWND> editorControls_, moveControls_, moveRelControls_, waitControls_, mousePressControls_, clickControls_, mousePlaybackControls_, runMacroControls_, keyPressControls_, keyControls_, hotkeyShortcutControls_, quickInputControls_, loopControls_, endLoopControls_, defineBlockControls_, runBlockControls_, scrollWheelControls_, findImageControls_, findImageOffsetControls_, findImageVarControls_, ocrDepControls_, ocrFindRegionToggleControls_, ocrControls_, ocrFindRegionControls_, ocrSearchControls_, ocrFollowControls_, ocrFollowOffsetControls_, ocrFollowVarControls_, ifControls_, elseControls_, lockScreenshotControls_, unlockScreenshotControls_, stopMacroControls_, runProgramControls_, runProgramFileControls_, closeProgramControls_, openWebpageControls_, openFileControls_, timerRecordControls_, getCursorPosControls_, gotoControls_, aiCommonControls_, aiTextControls_, aiImageControls_, aiActionControls_, aiFindRegionControls_;

    // ── 新布局系统: 参数面板布局结果缓存 (索引 = popupAction_.sel) ──
    std::unordered_map<int, UILayoutResult> paramLayoutResults_;
    std::vector<EditorControlLayout> editorLayouts_;
    std::vector<ParamScrollLayoutEntry> paramScrollLayout_;
    int paramContentBottom_ = 0;
    int paramControlsBottom_ = 0;
    int paramLayoutBottomHint_ = -1;
    std::vector<ScriptMeta> scripts_; std::vector<ScriptMeta> recordings_;
    std::vector<AgentConversationMeta> agentConversations_;
    std::vector<ScriptAction> actions_;
    std::set<int> collapsedContainers_;
    mutable std::vector<int> visibleActionIndexCache_;
    mutable bool visibleActionIndexCacheDirty_ = true;
    bool editorOpenPending_ = false;
    bool editorFullClientBlit_ = false;
    bool editorOpenCreateNew_ = false;
    int editorOpenGeneration_ = 0;
    int editorOpenPhase_ = 0;
    std::wstring editorOpenPath_;
    // 编辑器长脚本分步解析（与录制优化首屏策略一致）
    std::vector<std::wstring> editorActionBlocks_;
    std::vector<uint8_t> editorActionParsed_;
    bool editorParsePending_ = false;
    int editorParseCursor_ = 0;
    bool editorLoadCoordsNormalized_ = false;
    CoordMeta editorLoadCoordMeta_{};
    Page page_ = Page::Home; quickscript::MainTab activeHomeTab_ = quickscript::MainTab::Clicker; Hotkey globalHotkey_{0, VK_F8, L"F8", true};
    RECT homeRectBeforeEditor_{};
    int selectedScript_ = -1, selectedRecording_ = -1, currentScriptIndex_ = -1, homeHover_ = -1, recordingHover_ = -1, agentConvHover_ = -1, hoverIndex_ = -1, selectedIndex_ = -1, editingRemarkIndex_ = -1, copySource_ = -1, dragIndex_ = -1, dragTargetIndex_ = -1, dragTargetIndent_ = 0, dragStartX_ = 0, dragStartY_ = 0, scrollOffset_ = 0, homeScrollOffset_ = 0, homeScrollbarDragOffset_ = 0, editorScrollbarDragOffset_ = 0, pendingDeleteIndex_ = -1, pendingRecordingDeleteIndex_ = -1, pendingDeleteAgentConv_ = -1, paramScrollY_ = 0, paramScrollbarDragOffset_ = 0;
    HoverButton hoverButton_ = HoverButton::None;
    HWND hoverGrayBtn_ = nullptr;
    HWND pendingHoverGrayOld_ = nullptr, pendingHoverGrayNew_ = nullptr;
    bool dragging_ = false, dragMoved_ = false, dragTargetNested_ = false, homeScrollbarDragging_ = false, editorScrollbarDragging_ = false, paramScrollbarDragging_ = false, trackingMouse_ = false, hasHomeRectBeforeEditor_ = false, wasVisibleBeforeRun_ = true, wasMinimizedBeforeRun_ = false, loadingForm_ = false, batchEditMode_ = false, findImageFullScreen_ = true, ocrFullScreen_ = true, aiFullScreen_ = true;
    int clickerDropPopupKind_ = -1;
    int clickerPopupHover_ = -1;
    int clickerPopupScroll_ = 0;
    int clickerPopupVisibleCount_ = 0;
    CrosshairDragController crosshairDrag_;
    HWND hoverCrosshairBtn_ = nullptr;
    int editorPopupOpen_ = -1;
    bool ocrSubPanelRefreshPosted_ = false;
    int editorPopupHover_ = -1;
    int editorPopupScroll_ = 0;
    HWND editorDropPopup_ = nullptr;
    HWND clickerDropPopup_ = nullptr;
    HWND editorTipPopup_ = nullptr;
    QuickInputTipKind quickInputTipPending_ = QuickInputTipKind::None;
    QuickInputTipKind quickInputTipShown_ = QuickInputTipKind::None;
    int quickInputTipPendingVarIndex_ = -1;
    POINT quickInputTipAnchor_{};
    DWORD quickInputTipHoverStart_ = 0;
    double saveDurationSeconds_ = 0;
    std::optional<Hotkey> saveHotkeyOverride_;
    std::vector<bool> batchSelected_;
    HCURSOR crosshairDragCursor_ = nullptr;
    std::wstring currentPath_, currentRecordTime_, formKeyText_, formKeyPressText_;
    UINT formKeyVk_ = 0, formKeyPressVk_ = 0;
    quickscript::ClickerSettings clickerSettings_{};
    quickscript::RecorderSettings recorderSettings_{};
    quickscript::AppSettings appSettings_{};
    /// 窗口模式录制开关（HomeState.recorderWindowMode；录制时以最上层窗口为目标）。
    bool recorderWindowMode_ = false;
    RecordingWindowTarget recordingWmTarget_{};
    int currentRecordingCaptureMode_ = -1;
    int currentInputTimingVersion_ = 0;
    bool trayActive_ = false;
    UINT wmTaskbarCreated_ = 0;
    bool editorFontsApplied_ = false;
    HFONT editorFontsAppliedTo_ = nullptr;
    int editorScaleAppliedPct_ = -1;
    bool hiddenToTray_ = false;
    int clickCountDone_ = 0;
    bool headlessUi_ = false;
    bool debugWindowSettingKnown_ = false;
    bool debugWindowSettingApplied_ = false;
    HWND webViewHostHwnd_ = nullptr;
    std::wstring lastRecordingError_;
    std::wstring lastSavedRecordingPath_;
    /// 精密时间轴回放：调试行写入内存，全部结束后再刷窗。
    /// 必须有上限：1.5 万步×多轮会在工作线程里无限 push，后期轮次卡顿后追赶连发，看起来像脚本变形。
    /// 与 MacroDebugWindow 的 pending 上限对齐；超出后只保留时间轴统计。
    static constexpr size_t kMaxDeferredPlaybackDebugRecs = 2000;
    std::atomic<bool> deferPlaybackDebugUi_{false};
    std::atomic<bool> deferredDebugDropped_{false};
    std::mutex deferredDebugMutex_;
    enum class DeferredDbgKind : uint8_t { Line, Wait, MoveRel, MoveAbs };
    struct DeferredDbgRec {
        DeferredDbgKind kind = DeferredDbgKind::Line;
        int no = 0;
        int x = 0;
        int y = 0;
        double duration = 0;
        double randomDuration = 0;
        uint64_t lateUs = 0;
        size_t textIdx = SIZE_MAX;
    };
    std::vector<DeferredDbgRec> deferredDebugRecs_;
    std::vector<std::wstring> deferredDebugTexts_;
    HWND statusTipWindow_ = nullptr;
    PopupCombo popupMode_, popupWmSelectMethod_, popupAction_, popupMouseBtn_, popupClickBtn_, popupLoopType_, popupRunBlock_, popupHotkeyShortcut_, popupQuickInputVar_, popupRunMacro_, popupMousePlayback_, popupScrollDir_, popupFindFollowUp_, popupOcrResultMode_, popupOcrFollowUp_, popupOcrSearchVar_, popupIfVar_, popupIfOperator_, popupIfConnector_, popupRunProgram_, popupAiModel_, popupAiContextMode_, popupAiOutputType_, popupAiSearchRegion_;
    std::vector<QuickInputVarItem> quickInputVarItems_;
    std::vector<std::wstring> runMacroPaths_, mousePlaybackPaths_;
    std::wstring findImagePath_;
    std::wstring ocrFindImagePath_;
    std::wstring aiFindImagePath_;
    std::unordered_map<int, ScriptAction> actionFormDrafts_; // 仅内存；退出/打开编辑清空，不写脚本
    std::unordered_set<std::wstring> newImagePaths_;      // 编辑期间新增的图片路径，用于取消时清理
    std::atomic<bool> findTestRunning_{false};
    std::atomic<bool> ocrTestRunning_{false};
    PromptModal promptModal_;
    WindowOuterShadow outerShadow_;
    std::wstring promptPendingMessage_;
    bool workerUsesOcrVars_ = false;
    HBITMAP findImagePreviewBitmap_ = nullptr;
    HBITMAP ocrFindImagePreviewBitmap_ = nullptr;
    HBITMAP aiFindImagePreviewBitmap_ = nullptr;
    RECT findRegionSavedRect_{};
    std::unique_ptr<MatchOverlay> matchOverlay_;
    std::unique_ptr<OcrOverlay> ocrOverlay_;
    std::unique_ptr<ScreenshotOverlay> screenshotOverlay_;
#if (QST_GDI_LEGACY == 1)
    std::vector<std::unique_ptr<AgentDialog>> agentDialogs_;
    std::unique_ptr<SettingsDialog> settingsDialog_;
#endif
    std::unordered_map<std::wstring, ImageMatchResult> matchVars_;
    std::unordered_map<std::wstring, ImageMatchListVar> matchListVars_;
    std::unordered_map<std::wstring, OcrVarResult> ocrVars_;
    std::unordered_map<std::wstring, std::wstring> aiVars_;
    std::unordered_map<std::wstring, std::wstring> userVars_;
    std::unordered_map<std::wstring, std::wstring> imageVars_;
    std::unordered_map<std::wstring, int> loopVars_;
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> timerStarts_;
    int curLoops_ = 0;
    std::atomic<int> simulatingInputDepth_{0};
    std::atomic<bool> breakoutUserInput_{false};
    std::atomic<bool> breakoutPaused_{false};
    bool breakoutTaskbarShown_ = false;
    bool breakoutUiVisibleOnScreen_ = false;
    bool breakoutTaskbarTransition_ = false;
    bool trayMenuOpen_ = false;
    BreakoutTaskbarPlacement breakoutPlacement_{};
    RECT runSavedRect_{};
    LONG_PTR runSavedExStyle_ = 0;
    bool runSavedRectValid_ = false;
    double workerBreakoutTime_{0};
    BreakoutHookState breakoutHookState_{};
    DWORD lastHotkeyTick_ = 0;
    std::wstring lastHotkeyRegisterWarning_;
    std::atomic_bool running_{false}, stopFlag_{false}; AiHttpAbortSlot aiHttpAbort_; std::thread worker_; std::mt19937 rng_{std::random_device{}()};
    /// worker 已自然结束但 WM_RUN_DONE 尚未处理（UI 忙/卡）时置位，
    /// 热键路径据此自动复位 running_，避免「按热键无反应」。
    std::atomic_bool workerFinished_{false};
    std::atomic_bool extRunPending_{false};
    // ── 编辑器调试状态（仅缓存，编辑会话结束即清理）──────────────
    std::atomic_bool debugMode_{false};
    std::atomic_bool debugStepMode_{false};
    std::atomic_bool debugPaused_{false};
    std::atomic_bool debugStepSignal_{false};
    int debugStartIndex_ = 0;
    /// 调试起点所在 defineBlock 祖先链（含起点块自身）；调试时这些块体按流程执行一次
    std::unordered_set<int> debugBlockEntrySet_;
    std::unordered_set<int> debugBreakpoints_;
    /// 仅当 activeActions 指向调试脚本副本时才启用闸门/断点（嵌套宏/指令块不生效）
    const std::vector<ScriptAction>* debugActionsPtr_ = nullptr;
    UINT debugHotkeyVk_ = 0;
    UINT debugHotkeyMods_ = 0;
    /// 调试前编辑器处于「宏编辑界面热键静音」状态；调试期间临时解除，结束后恢复
    bool debugRestoreUiMute_ = false;
    int shellUiMode_ = 0; // 0=home 1=editor 2=opt（与壳 SetUiMode 同步）
    mutable std::mutex extScriptStateMu_;
    std::wstring runningScriptPath_;
    std::wstring runningScriptName_;
    windowmode::WindowModeScriptConfig runningWindowMode_{};
    std::atomic<int> executedSteps_{0};
    std::atomic<int> playbackActionIndex_{0};
    std::atomic<int> playbackActionTotal_{0};
    windowmode::WindowModeScriptConfig scriptWindowMode_{};
    CoordMeta loadedCoordMeta_{};  // 当前加载脚本的 coordMeta，供保存时复用
#if (QST_GDI_LEGACY == 1)
    windowmode::WindowPickDialog wmPickDialog_{};
#endif
    HWND wmSelectMethod_ = nullptr;
    HWND wmSpecifyWindowBtn_ = nullptr;
    HWND wmTargetPathEdit_ = nullptr;
    HWND wmTargetBrowseBtn_ = nullptr;
    HWND wmTargetCrosshairBtn_ = nullptr;
    HWND wmFakeFocusCheck_ = nullptr;
    ScheduledTaskScheduler scheduledTasks_;
    std::vector<std::wstring> pendingScheduledPaths_;
    bool runningFromScheduled_ = false;
    bool nextRunFromScheduled_ = false;
    bool scheduledInterruptStop_ = false;
    std::mutex scheduledYieldMu_;
    std::wstring scheduledYieldPath_;
    std::wstring deferredScheduledAfter_;
    std::atomic_bool scheduledYieldRequested_{false};
    std::atomic_bool scheduledYieldActive_{false};
    std::function<void()> scheduledYieldHook_;
    // Recording
    std::atomic_bool recording_{false};
    bool recordingWasVisible_ = true;
    // Clicker
    std::atomic_bool clicking_{false};
    std::thread clickerThread_;
};
