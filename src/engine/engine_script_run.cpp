// engine_script_run.cpp — F2 slice extracted from engine_host_window.h
#include "engine/engine_host_window.h"
#include "action_utils.h"
#include "agent_ui_notify.h"
#include "ai_logic_convert.h"
#include "color_match.h"
#include "desktop_tools/desktop_tools.h"
#include "image_var_util.h"
#include "macro_execute_tools.h"
#include "script_io.h"
#include "window_mode/ui_element_probe.h"
#include "window_mode/window_list.h"
#include "window_mode/window_capture.h"
#include "window_mode/window_capture_wgc.h"

#include <mutex>

// 合成桌面区域采集（微信截图同原理）：
// GDI BitBlt+CAPTUREBLT 拍不到 DirectComposition 表面（TSF 输入法候选框/组字框
// 是独立合成层），WGC 整屏采集的是 DWM 合成后的完整桌面，能拍到输入法状态。
// 优先 WGC 整屏后裁剪到目标区域；WGC 不可用/失败时回退 GDI BitBlt。
// cx1..cy2 为屏幕坐标区域。
static HBITMAP CaptureAiRegionComposed(int cx1, int cy1, int cx2, int cy2) {
    HBITMAP bmp = nullptr;
    bool usedWgc = false;
    if (windowmode::IsWgcCaptureAvailable()) {
        const int cxm = (cx1 + cx2) / 2;
        const int cym = (cy1 + cy2) / 2;
        HMONITOR hMon = MonitorFromPoint({ cxm, cym }, MONITOR_DEFAULTTONEAREST);
        if (hMon) {
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi)) {
                int mw = 0, mh = 0;
                HBITMAP full = windowmode::CaptureMonitorWgc(hMon, mw, mh);
                if (full) {
                    const int rx1 = std::max(cx1, static_cast<int>(mi.rcMonitor.left));
                    const int ry1 = std::max(cy1, static_cast<int>(mi.rcMonitor.top));
                    const int rx2 = std::min(cx2, static_cast<int>(mi.rcMonitor.right));
                    const int ry2 = std::min(cy2, static_cast<int>(mi.rcMonitor.bottom));
                    if (rx2 > rx1 && ry2 > ry1) {
                        bmp = windowmode::CropBitmapScreenRegion(full,
                            mi.rcMonitor.left, mi.rcMonitor.top, rx1, ry1, rx2, ry2);
                        usedWgc = (bmp != nullptr);
                    }
                    DeleteBitmapHandle(full);
                }
            }
        }
    }
    if (!bmp) bmp = CaptureScreenRegion(cx1, cy1, cx2, cy2);
    static std::once_flag diagOnce;
    std::call_once(diagOnce, [usedWgc] {
        if (qst::desktop_tools::MacroDebug().IsCreated()) {
            qst::desktop_tools::MacroDebug().AppendLog(
                usedWgc ? L"  [诊断] 观察帧：合成桌面采集(WGC)已启用（微信同原理，能拍到输入法组字框/候选框）"
                        : L"  [诊断] 观察帧：WGC 不可用，回退 GDI BitBlt 采集");
        }
    });
    return bmp;
}

// was engine_host_window.h:11614-11621
void EngineHost::RunScriptByIndex(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        const std::wstring path = scripts_[static_cast<size_t>(index)].path;
        // 编辑器正在编辑同一文件：用内存动作（含未保存修改）+ 已有 currentPath_
        if (page_ == Page::Editor && !currentPath_.empty()
            && _wcsicmp(path.c_str(), currentPath_.c_str()) == 0) {
            RunCurrentActions();
            return;
        }
        // 热键/主页：必须带磁盘路径启动，否则逻辑转化写回拿到空路径会静默跳过
        // （LoadScriptFile 不会设置 currentPath_，旧逻辑 RunCurrentActions 会传空 selfPath）
        RunActionsFromPath(path);
    }

void EngineHost::RunRecordingByIndex(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        const std::wstring path = recordings_[static_cast<size_t>(index)].path;
        if (page_ == Page::Editor && !currentPath_.empty()
            && _wcsicmp(path.c_str(), currentPath_.c_str()) == 0) {
            RunCurrentActions();
            return;
        }
        RunActionsFromPath(path);
    }

bool EngineHost::HotkeyChordConflicts(UINT vk, UINT modifiers, const std::wstring& excludePath,
    bool excludeGlobal, std::wstring& errOut) const {
        errOut.clear();
        if (!vk) return false;
        auto sameChord = [&](const Hotkey& hk) {
            return hk.enabled && hk.vk == vk && hk.modifiers == modifiers;
        };
        if (!excludeGlobal && sameChord(globalHotkey_)) {
            errOut = L"与全局启停热键冲突";
            return true;
        }
        for (const auto& s : scripts_) {
            if (!excludePath.empty() && _wcsicmp(s.path.c_str(), excludePath.c_str()) == 0) continue;
            if (sameChord(s.hotkey)) {
                errOut = L"与脚本「" + s.name + L"」热键冲突";
                return true;
            }
        }
        for (const auto& r : recordings_) {
            if (!excludePath.empty() && _wcsicmp(r.path.c_str(), excludePath.c_str()) == 0) continue;
            if (sameChord(r.hotkey)) {
                errOut = L"与录制「" + r.name + L"」热键冲突";
                return true;
            }
        }
        return false;
    }

// was engine_host_window.h:12346-12387
void EngineHost::RunCurrentActions() {
        if (running_) {
            // 避免「点了没反应」：正在运行时再次点击视为请求停止。
            StopRun();
            if (qst::desktop_tools::MacroDebug().IsCreated()) {
                qst::desktop_tools::MacroDebug().AppendLog(L"脚本正在运行，已请求停止");
            }
            return;
        }
        SyncFormIntoActionsBeforeRun();
        EnsureEditorFullyParsed();
        windowmode::WindowModeScriptConfig runCfg{};
        if (!PrepareWindowModeRunConfig(runCfg)) return;
        // 不把启动时解析出的临时窗口身份写回编辑器，避免污染「选择窗口方式」与保存内容。
        // 有路径且允许自动打开时，不要因为“当前没有窗口”而拦截运行——真正打开交给 BeginRun。
        if (runCfg.enabled && appSettings_.windowMode.blockRunWhenUnhealthy
            && !windowmode::ShouldAutoLaunchTarget(runCfg)) {
            std::wstring err;
            if (!windowmode::WindowModeExecutor::CheckRunHealth(runCfg, err)) {
                RestoreMainWindowForUser();
                promptModal_.ShowInfo(err.empty() ? L"窗口模式未就绪" : err);
                return;
            }
        }

        CoordMeta execMeta = ScriptCoordMetaForExecution(loadedCoordMeta_);
        std::vector<ScriptAction> execActions = actions_;
        SyncNormFieldsFromPixels(execActions,
            CaptureCurrentCoordMeta(runCfg.enabled ? &runCfg : nullptr));
        execActions = PrepareScriptActionsForExecution(execActions, execMeta);
        // 旧录制里相对移动间隔可能被 Raw 积压压成 0~1ms；回放前按设备报告间隔拉开。
        if (IsRecordingScriptPath(currentPath_) || ScriptIsTimedInputSequence(execActions))
            RepairCompressedRelativeGaps(execActions);

        const double breakoutTime = runCfg.enabled ? 0.0 : ParseBreakoutTimeFromEditor();
        Hotkey scriptHotkey{};
        for (const auto& s : scripts_) {
            if (_wcsicmp(s.path.c_str(), currentPath_.c_str()) == 0) {
                scriptHotkey = s.hotkey;
                break;
            }
        }
        if (!scriptHotkey.enabled || !scriptHotkey.vk) {
            for (const auto& r : recordings_) {
                if (_wcsicmp(r.path.c_str(), currentPath_.c_str()) == 0) {
                    scriptHotkey = r.hotkey;
                    break;
                }
            }
        }
        StartActionsWorker(execActions, currentPath_, runCfg, execMeta, breakoutTime,
            scriptHotkey);
    }

// 编辑器调试：从指定动作开始单次执行（与保存/热键同一套窗口模式；不受「宏执行次数」设置影响）
bool EngineHost::EngineDebugRunActions(const std::vector<ScriptAction>& actions, int startIndex,
    bool stepMode, const std::vector<int>& breakpoints,
    const Hotkey& debugHotkey, const std::wstring& displayName,
    const windowmode::WindowModeScriptConfig& wmCfgIn, std::wstring& err) {
        if (running_) {
            StopRun();
            err = L"脚本正在运行，已请求停止";
            return false;
        }
        if (actions.empty()) {
            err = L"没有可调试的动作";
            return false;
        }
        if (startIndex < 0 || startIndex >= static_cast<int>(actions.size())) {
            err = L"调试起点超出动作列表";
            return false;
        }
        for (int b : breakpoints) {
            if (b < 0 || b >= static_cast<int>(actions.size())) {
                err = L"断点序号超出动作列表";
                return false;
            }
        }
        // 起点位于容器体内时（defineBlock/Loop/If/Else），直接从中部执行会丢失容器
        // 上下文（块体被主流程跳过、循环变量/条件未建立、EndLoop/Goto 可能逃逸）。
        // 统一追溯到最外层祖先容器并从其绝对序号开始；defineBlock 祖先链在调试中
        // 按流程执行一次（嵌套块继续向上追溯）。Else 的容器归属其所属 If。
        debugBlockEntrySet_.clear();
        if (actions[static_cast<size_t>(startIndex)].type == ActionType::Else) {
            // 起点本身是 Else（结构性动作）：归属到所属 If 开始，让分支按条件执行
            for (int k = startIndex - 1; k >= 0; --k) {
                if (actions[static_cast<size_t>(k)].type == ActionType::If
                    && actions[static_cast<size_t>(k)].indent
                        == actions[static_cast<size_t>(startIndex)].indent) {
                    startIndex = k;
                    break;
                }
            }
        }
        int ancestorCursor = startIndex;
        int outer = -1;
        int outerIndent = INT_MAX;
        for (;;) {
            if (actions[static_cast<size_t>(ancestorCursor)].type == ActionType::DefineBlock) {
                debugBlockEntrySet_.insert(ancestorCursor);
            }
            const auto& acur = actions[static_cast<size_t>(ancestorCursor)];
            const int directParent = FindDirectParentIndex(
                actions, static_cast<size_t>(ancestorCursor), acur.indent);
            if (directParent < 0) break;
            int effParent = directParent;
            const auto& aDirect = actions[static_cast<size_t>(directParent)];
            if (aDirect.type == ActionType::Else) {
                // Else 自身在主流程中会被跳过：归属到同一缩进最近的 If
                for (int k = directParent - 1; k >= 0; --k) {
                    if (actions[static_cast<size_t>(k)].type == ActionType::If
                        && actions[static_cast<size_t>(k)].indent == aDirect.indent) {
                        effParent = k;
                        break;
                    }
                }
            }
            if (actions[static_cast<size_t>(effParent)].type == ActionType::DefineBlock) {
                debugBlockEntrySet_.insert(effParent);
            }
            if (actions[static_cast<size_t>(effParent)].indent < outerIndent) {
                outer = effParent;
                outerIndent = actions[static_cast<size_t>(effParent)].indent;
            }
            ancestorCursor = effParent;
        }
        if (outer >= 0) {
            startIndex = outer;
        }
        // 编辑器动作是当前屏幕像素坐标：与正常执行一致做「归一化→反归一化」，
        // 让 findImage 偏移等 n* 字段就绪（当前屏幕不变时是恒等变换）。
        const CoordMeta captureMeta = CaptureCurrentCoordMeta(nullptr);
        CoordMeta execMeta = ScriptCoordMetaForExecution(captureMeta);
        std::vector<ScriptAction> execActions = actions;
        SyncNormFieldsFromPixels(execActions, captureMeta);
        execActions = PrepareScriptActionsForExecution(execActions, execMeta);
        if (IsRecordingScriptPath(displayName) || ScriptIsTimedInputSequence(execActions)) {
            RepairCompressedRelativeGaps(execActions);
        }
        windowmode::WindowModeScriptConfig wmCfg = wmCfgIn;
        bool anyRel = wmCfg.windowRelativeCoordinates;
        for (const auto& a : execActions) {
            if (a.windowRelative) { anyRel = true; break; }
        }
        windowmode::FinalizeWindowModeForPlayback(wmCfg, anyRel, false);
        if (!ResolveWindowModeSelectMethod(wmCfg)) {
            RestoreMainWindowForUser();
            err = L"窗口模式未能绑定目标窗口";
            return false;
        }
        wmCfg.autoLaunchTarget = windowmode::ShouldAutoLaunchTarget(wmCfg);
        StartActionsWorker(execActions, displayName, wmCfg, execMeta, 0.0, Hotkey{},
            startIndex, stepMode, &breakpoints, debugHotkey);
        return true;
    }

// was engine_host_window.h:14229-14235
void EngineHost::StopRun() {
        if (scheduledInterruptStop_) {
            scheduledInterruptStop_ = false;  // 本次停止来自「定时脚本优先」，保留排队
        } else {
            pendingScheduledPaths_.clear();
            ClearScheduledYield();
        }
        stopFlag_ = true;
        ghEmergencyStop.store(true, std::memory_order_release);
        aiHttpAbort_.Abort();
        // 扩展桥可能卡在 WS/CDP 等待：先 Abort 再清闩锁，保证热键能强行中止。
        windowmode::WindowModeExecutor::NotifyCancel();
        ClearToggleHotkeyLatches();
    }

void EngineHost::EnqueueScheduledPath(const std::wstring& path) {
        if (path.empty()) return;
        for (const auto& existing : pendingScheduledPaths_) {
            if (_wcsicmp(existing.c_str(), path.c_str()) == 0) return;
        }
        if (pendingScheduledPaths_.size() >= 16) return;
        pendingScheduledPaths_.push_back(path);
    }

void EngineHost::TryStartPendingScheduled() {
        while (!running_ && !pendingScheduledPaths_.empty()) {
            const std::wstring path = pendingScheduledPaths_.front();
            pendingScheduledPaths_.erase(pendingScheduledPaths_.begin());
            nextRunFromScheduled_ = true;
            RunActionsFromPath(path);
            if (running_) return;
            nextRunFromScheduled_ = false;
            runningFromScheduled_ = false;
        }
    }

void EngineHost::RequestScheduledYield(const std::wstring& path) {
        if (path.empty()) return;
        std::lock_guard<std::mutex> lock(scheduledYieldMu_);
        // 已有待插入的定时：保留先到的，避免同秒第二条把路径盖掉
        if (scheduledYieldRequested_.load(std::memory_order_relaxed)
            && !scheduledYieldPath_.empty()) {
            return;
        }
        scheduledYieldPath_ = path;
        scheduledYieldRequested_.store(true, std::memory_order_release);
    }

bool EngineHost::TakeScheduledYieldPath(std::wstring& out) {
        if (!scheduledYieldRequested_.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lock(scheduledYieldMu_);
        if (!scheduledYieldRequested_.load(std::memory_order_relaxed)) return false;
        out = std::move(scheduledYieldPath_);
        scheduledYieldPath_.clear();
        scheduledYieldRequested_.store(false, std::memory_order_relaxed);
        return !out.empty();
    }

void EngineHost::ClearScheduledYield() {
        std::lock_guard<std::mutex> lock(scheduledYieldMu_);
        scheduledYieldPath_.clear();
        deferredScheduledAfter_.clear();
        scheduledYieldRequested_.store(false, std::memory_order_relaxed);
    }

void EngineHost::DeferScheduledPath(const std::wstring& path) {
        if (path.empty()) return;
        std::lock_guard<std::mutex> lock(scheduledYieldMu_);
        if (deferredScheduledAfter_.empty()) {
            deferredScheduledAfter_ = path;
            return;
        }
        if (_wcsicmp(deferredScheduledAfter_.c_str(), path.c_str()) == 0) return;
    }

std::wstring EngineHost::TakeDeferredScheduledPath() {
        std::lock_guard<std::mutex> lock(scheduledYieldMu_);
        std::wstring out = std::move(deferredScheduledAfter_);
        deferredScheduledAfter_.clear();
        return out;
    }

void EngineHost::OnScheduledTaskFire(const std::wstring& path) {
        if (path.empty()) return;
        if (running_ && workerFinished_.load(std::memory_order_relaxed)) OnRunDone();
        const bool busy = running_ || extRunPending_.load(std::memory_order_relaxed);
        const auto policy = ClampScheduledTaskConflictPolicy(
            appSettings_.playback.scheduledTaskConflictPolicy);
        const bool runningIsScheduled = runningFromScheduled_
            || scheduledYieldActive_.load(std::memory_order_relaxed)
            || scheduledYieldRequested_.load(std::memory_order_relaxed);
        const bool autoResume = appSettings_.playback.scheduledTaskAutoResume;
        const auto action = DecideScheduledTaskFire(policy, busy, runningIsScheduled, autoResume);
        auto dbg = [&](const std::wstring& msg) {
            if (qst::desktop_tools::MacroDebug().IsCreated()) {
                qst::desktop_tools::MacroDebug().AppendLog(msg);
            }
        };
        const std::wstring name = [&]() {
            const auto slash = path.find_last_of(L"\\/");
            return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
        }();
        switch (action) {
        case ScheduledTaskFireAction::Skip:
            dbg(L"定时任务跳过（当前脚本未结束）：" + name);
            return;
        case ScheduledTaskFireAction::RunNow:
            if (!pendingScheduledPaths_.empty()) {
                EnqueueScheduledPath(path);
                TryStartPendingScheduled();
                return;
            }
            nextRunFromScheduled_ = true;
            RunActionsFromPath(path);
            if (!running_) {
                nextRunFromScheduled_ = false;
                runningFromScheduled_ = false;
            }
            return;
        case ScheduledTaskFireAction::InterruptAndQueue:
            EnqueueScheduledPath(path);
            dbg(L"定时脚本优先：已请求停止当前脚本，随后执行 " + name);
            if (running_) {
                scheduledInterruptStop_ = true;
                StopRun();
            } else {
                TryStartPendingScheduled();
            }
            return;
        case ScheduledTaskFireAction::Queue:
            EnqueueScheduledPath(path);
            dbg(L"当前脚本结束后将执行定时：" + name);
            if (!running_) TryStartPendingScheduled();
            return;
        case ScheduledTaskFireAction::YieldAndResume:
            RequestScheduledYield(path);
            dbg(L"定时插入：暂停当前步骤，执行 " + name + L" 后继续");
            if (!running_ || workerFinished_.load(std::memory_order_relaxed)) {
                ClearScheduledYield();
                EnqueueScheduledPath(path);
                if (!running_) TryStartPendingScheduled();
            }
            return;
        default:
            return;
        }
    }

// was engine_host_window.h:12323-12337
void EngineHost::SyncFormIntoActionsBeforeRun() {
        if (page_ != Page::Editor) return;
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(actions_.size())) return;
        const ScriptAction& existing = actions_[static_cast<size_t>(selectedIndex_)];
        if (ComboSelForType(existing.type) != popupAction_.sel) return;
        ScriptAction action = ActionFromForm();
        if (action.type == ActionType::DefineBlock && !IsValidBlockName(action.blockName)) return;
        if (action.type == ActionType::EndLoop
            && !HasLoopParentAt(actions_, static_cast<size_t>(selectedIndex_), existing.indent)) {
            return;
        }
        action.originalNo = existing.originalNo;
        action.indent = existing.indent;
        actions_[static_cast<size_t>(selectedIndex_)] = action;
    }

// was engine_host_window.h:12341-14179
void EngineHost::StartActionsWorker(const std::vector<ScriptAction>& actions, const std::wstring& selfPath, const windowmode::WindowModeScriptConfig& wmCfg, const CoordMeta& execCoordMeta, double breakoutTime, const Hotkey& scriptHotkey, int debugStartIndex, bool debugStepMode, const std::vector<int>* debugBreakpoints, const Hotkey& debugHotkey) {
        if (running_) {
            StopRun();
            if (qst::desktop_tools::MacroDebug().IsCreated()) {
                qst::desktop_tools::MacroDebug().AppendLog(L"脚本正在运行，已请求停止");
            }
            return;
        }
        ClearScheduledYield();
        const bool debugMode = debugStartIndex >= 0;
        if (debugMode) {
            debugMode_.store(true, std::memory_order_relaxed);
            debugStepMode_.store(debugStepMode, std::memory_order_relaxed);
            debugPaused_.store(false, std::memory_order_relaxed);
            debugStepSignal_.store(false, std::memory_order_relaxed);
            debugStartIndex_ = debugStartIndex;
            debugBreakpoints_.clear();
            if (debugBreakpoints) {
                for (int b : *debugBreakpoints) debugBreakpoints_.insert(b);
            }
            debugHotkeyVk_ = debugHotkey.vk;
            debugHotkeyMods_ = debugHotkey.modifiers;
        }
        workerFinished_.store(false, std::memory_order_relaxed);
        runningFromScheduled_ = nextRunFromScheduled_;
        nextRunFromScheduled_ = false;
        running_ = true; stopFlag_ = false; breakoutUserInput_ = false; breakoutPaused_ = false;
        ghEmergencyStop.store(false, std::memory_order_release);
        ghWorkerCancelFlag = &stopFlag_;
        executedSteps_.store(0, std::memory_order_relaxed);
        SuspendHotkeysForPlayback();
        if (debugMode) {
            // 编辑器打开时 EngineSetUiMode(1) 会置「界面热键静音」并放行 LL 钩子，
            // 导致调试热键/通用启停热键全部失效。调试期间临时解除静音，结束后恢复。
            debugRestoreUiMute_ = ghUiModeHotkeysMuted.load(std::memory_order_relaxed);
            if (debugRestoreUiMute_) {
                ghUiModeHotkeysMuted.store(false, std::memory_order_relaxed);
            }
        }
        if (debugMode && debugHotkeyVk_ && hwnd_ && IsWindow(hwnd_)) {
            if (!RegisterHotKey(hwnd_, HOTKEY_DEBUG_ID, debugHotkeyMods_, debugHotkeyVk_)
                && qst::desktop_tools::MacroDebug().IsCreated()) {
                qst::desktop_tools::MacroDebug().AppendLog(
                    L"调试热键注册失败（可能与脚本/全局热键冲突），单步/暂停将不可用");
            }
        }
        {
            std::lock_guard<std::mutex> lock(extScriptStateMu_);
            runningScriptPath_ = selfPath;
            runningWindowMode_ = wmCfg;
            windowmode::WindowModeLogEventf(
                L"[窗口模式] 本次运行解析后配置：enabled=%d executionKind=%s targetExe=%ls autoLaunch=%d selectMethod=%d",
                wmCfg.enabled ? 1 : 0,
                wmCfg.executionKind == windowmode::WindowModeExecutionKind::HiddenDesktop
                    ? L"HiddenDesktop" : L"BackgroundWindow",
                wmCfg.targetExePath.c_str(),
                wmCfg.autoLaunchTarget ? 1 : 0,
                static_cast<int>(wmCfg.selectMethod));
            const auto slash = selfPath.find_last_of(L"\\/");
            runningScriptName_ = (slash == std::wstring::npos)
                ? selfPath : selfPath.substr(slash + 1);
            const auto dot = runningScriptName_.rfind(L'.');
            if (dot != std::wstring::npos) runningScriptName_.resize(dot);
        }
        ghHotkeySessionBusy.store(true, std::memory_order_relaxed);
        EnsureHotkeyAuxTimers();
        MouseInputRouter::Instance().Configure();
        BeginHighResTimer(); // 宏内短等待（录制轨迹）需要 1ms 定时器精度
        breakoutTaskbarShown_ = false;
        breakoutUiVisibleOnScreen_ = false;
        breakoutPlacement_ = BreakoutTaskbarPlacement{};
        workerBreakoutTime_ = (!wmCfg.enabled && breakoutTime > 0) ? breakoutTime : 0;
        aiHttpAbort_.Clear();
        wasVisibleBeforeRun_ = false;
        wasMinimizedBeforeRun_ = false;
        if (HWND face = UserFacingMainHwnd()) {
            wasVisibleBeforeRun_ = (IsWindowVisible(face) == TRUE);
            wasMinimizedBeforeRun_ = (IsIconic(face) == TRUE);
        }
        runSavedRectValid_ = GetWindowRestoredRect(hwnd_, &runSavedRect_);
        runSavedExStyle_ = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        SetWindowCloaked(hwnd_, false);
        CloseEditorPopup(); CancelQuickInputTip();
        if (appSettings_.other.playSoundOnStart) MessageBeep(MB_OK);
        EnsureTrayIcon();
        if (debugMode) {
            // 调试期间主界面/编辑界面一律隐藏（不受「自动隐藏」设置影响）
            HideUserFacingMainWindow(true);
        } else if (wmCfg.enabled
            && wmCfg.executionKind != windowmode::WindowModeExecutionKind::BackgroundWindow
            && !windowmode::UsesCdpInput(wmCfg)) {
            // 窗口模式假前台 SendInput 必须在 UI 线程先把壳藏掉，否则工作线程
            // SetForegroundWindow 抢不到游戏（调试能点、主页/热键不能点）。
            HideUserFacingMainWindow(true);
        } else if (wasMinimizedBeforeRun_) {
            // 运行期间主窗口不保留绿色按钮；辅助窗口不受影响并继续显示绿色按钮。
            HideUserFacingMainWindow(false);
        } else if (appSettings_.other.autoHideMainWindow) {
            // 自动隐藏前让 DWM 保存真实界面，供脱离时的最小化任务栏预览使用。
            HideUserFacingMainWindow(true);
        } else {
            KeepEngineHostHiddenIfHeadless();
        }
        UpdateStatusTip();
        windowmode::SetWindowModeLogSink([this](const std::wstring& line) {
            // 扩展桥常开：未开窗口模式时勿把桥心跳灌进宏调试窗（与 AI/默认宏无关）
            {
                std::lock_guard<std::mutex> lock(extScriptStateMu_);
                if (!runningWindowMode_.enabled) return;
            }
            if (!qst::desktop_tools::MacroDebug().IsCreated()) return;
            if (deferPlaybackDebugUi_.load(std::memory_order_relaxed)) {
                PushDeferredDebugLine(line);
                return;
            }
            qst::desktop_tools::MacroDebug().AppendLog(line);
        });
        if (qst::desktop_tools::MacroDebug().IsCreated()) qst::desktop_tools::MacroDebug().ClearLog();
        breakoutHookState_ = BreakoutHookState{};
        breakoutHookState_.running = &running_;
        breakoutHookState_.simulatingDepth = &simulatingInputDepth_;
        breakoutHookState_.userInput = &breakoutUserInput_;
        if (workerBreakoutTime_ > 0) {
            if (globalHotkey_.enabled && globalHotkey_.vk) {
                breakoutHookState_.ignoreHotkeys.push_back(globalHotkey_);
            }
            for (const auto& script : scripts_) {
                if (script.hotkey.enabled && script.hotkey.vk) {
                    breakoutHookState_.ignoreHotkeys.push_back(script.hotkey);
                }
            }
            if (scriptHotkey.enabled && scriptHotkey.vk) {
                breakoutHookState_.ignoreHotkeys.push_back(scriptHotkey);
            }
            breakout_input::InstallBreakoutHooks(breakoutHookState_);
        }
        worker_ = std::thread([this, actions, selfPath, wmCfg, execCoordMeta]() {
            if (debugMode_.load(std::memory_order_relaxed)) {
                // 只对调试脚本的主序列启用闸门/断点（嵌套宏/指令块指向其它 actions 副本）
                debugActionsPtr_ = &actions;
            }
            if (wmCfg.enabled) {
                windowmode::WindowModeLog(
                    wmCfg.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow
                        ? L"[窗口模式] 后台窗口模式：工作线程已启动"
                        : L"[窗口模式] 窗口模式：工作线程已启动");
            }
            bool usesOcr = ScriptUsesTextRecognition(actions);
            workerUsesOcrVars_ = usesOcr;
            matchVars_.clear();
            if (usesOcr) ocrVars_.clear();
            loopVars_.clear();
            timerStarts_.clear();
            aiVars_.clear();
            ClearImageVars(imageVars_);
            curLoops_ = 0;
            const unsigned long long imageVarRunId = GetTickCount64();
            bool ocrSessionHeld = false;
            auto holdOcrSession = [&ocrSessionHeld]() {
                if (ocrSessionHeld) return;
                EnsureOcrSession();
                ocrSessionHeld = true;
            };
            UINT heldKeyVk = 0; // 兼容单键跟踪；多键用 heldKeys
            std::unordered_set<UINT> heldKeys;
            HBITMAP lockedScreen_ = nullptr;
            int lockedVirtX_ = 0;
            int lockedVirtY_ = 0;

            windowmode::WindowModeExecutor wmExec;
            windowmode::WindowModeExecutor* wmExecPtr = nullptr;

            // 计算模板缩放比例（用于找图跨分辨率适配，嵌套宏可切换 activeCoordMeta）
            int execTargetW = 0, execTargetH = 0, execVirtX = 0, execVirtY = 0;
            GetVirtualScreenBounds(execVirtX, execVirtY, execTargetW, execTargetH);
            CoordMeta activeCoordMeta = execCoordMeta;
            auto currentTmplScale = [&]() -> TemplateScale {
                if (wmExecPtr && wmExecPtr->IsActive() && wmCfg.windowRelativeCoordinates) {
                    const TemplateScale wmTs = wmExecPtr->FindImageTemplateScale();
                    if (wmTs.sx > 0.0 && wmTs.sy > 0.0) return wmTs;
                }
                if (activeCoordMeta.refWidth <= 0 || activeCoordMeta.refHeight <= 0) {
                    return TemplateScale{};
                }
                return ComputeTemplateScale(activeCoordMeta, execTargetW, execTargetH);
            };

            if (wmCfg.enabled) {
                std::wstring wmErr;
                windowmode::BeginRunOptions wmBeginOpts;
                wmBeginOpts.launchTarget = true;
                wmBeginOpts.cancelFlag = &stopFlag_;
                // 对抗性测试：假焦点注入开关/技术/模块隐藏来自全局设置（默认经典远线程）
                wmExec.SetEnableFakeFocusInjection(
                    appSettings_.windowMode.enableFakeFocusInjection);
                wmExec.SetInjectionTechnique(
                    windowmode::inject::TechniqueFromInt(
                        appSettings_.windowMode.injectionTechnique));
                wmExec.SetHideInjectedModule(
                    appSettings_.windowMode.hideInjectedModule);
                {
                    const auto slash = selfPath.find_last_of(L"\\/");
                    wmBeginOpts.launchSearchDir = slash == std::wstring::npos ? L"" : selfPath.substr(0, slash);
                }
                if (!wmExec.BeginRun(wmCfg, wmErr, wmBeginOpts)) {
                    if (StopRequested()) {
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                        return;
                    }
                    if (hwnd_) {
                        promptPendingMessage_ = wmErr.empty()
                            ? L"窗口模式启动失败" : wmErr;
                        // 先结束运行并恢复主窗口，再弹提示，避免遮罩坐标错位导致「确定」点不到。
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                        PostMessageW(hwnd_, WM_APP_PROMPT, 0, 0);
                    } else {
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                    }
                    return;
                }
                wmExecPtr = &wmExec;
                wmExec.SetCoordMeta(activeCoordMeta);
                windowmode::WindowModeLog(L"[窗口模式] 已绑定目标，开始运行");
                windowmode::WindowModeLogDesktopSnap(L"绑定后", wmExec.TargetHwnd());
                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                    HWND th = wmExec.TargetHwnd();
                    wchar_t cls[128]{};
                    if (th) GetClassNameW(th, cls, 128);
                    wchar_t buf[192]{};
                    swprintf_s(buf, L"窗口模式已绑定 hwnd=0x%p class=%s%s",
                        th, cls,
                        wmExec.UsesBackgroundWindow() ? L" [后台]"
                            : (wmExec.IsCdpInputMode() ? L" [鼠标宏·扩展]" : L" [鼠标宏桌面]"));
                    AppendDebugLog(buf);
                }
            } else if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                bool anyRel = wmCfg.windowRelativeCoordinates;
                for (const auto& act : actions) {
                    if (act.windowRelative) { anyRel = true; break; }
                }
                if (anyRel) {
                    AppendDebugLog(
                        L"窗口模式未启用：脚本含窗口相对坐标/找图，将误走全屏桌面"
                        L"（游戏常见匹配度约 50% 失败）。请用窗口模式+图片定位重新录制。");
                }
            }
            if (usesOcr) holdOcrSession();

            {
                const auto backend = wmCfg.enabled
                    ? quickscript::ForegroundInputBackend::Software
                    : appSettings_.playback.foregroundInputBackend;
                ForegroundInputRouter::Instance().BeginSession(backend);
                // 脱离=0：不装脱离钩、不记移动/滚轮指纹；热键仍靠键/鼠标按钮指纹。
                if (ForegroundInputRouter::Instance().IsHidActive()) {
                    synthetic_input::SetBreakoutTracking(workerBreakoutTime_ > 0);
                }
                if (backend != quickscript::ForegroundInputBackend::Software
                    && ForegroundInputRouter::Instance().IsHidActive()) {
                    AppendDebugLog(std::wstring(L"前台注入后端：")
                        + quickscript::ForegroundInputBackendName(
                            ForegroundInputRouter::Instance().ActiveBackend()));
                    AppendDebugLog(L"HID模式：VirtualHid 绝对移标用 SetCursorPos（不发绝对 HID，避免 mouhid 主屏映射乱漂）；相对/按键/滚轮仍走驱动");
                    if (workerBreakoutTime_ > 0) {
                        AppendDebugLog(L"驱动注入：脱离=LL；VirtualHid 绝对移标不记移动指纹(靠INJECTED)，真人挪鼠/点击可脱离");
                    } else {
                        AppendDebugLog(L"驱动注入：仅已登记启停热键指纹（脱离=0）");
                    }
                } else if (ForegroundInputRouter::Instance().DidFallback()) {
                    const std::wstring fb = ForegroundInputRouter::Instance().FallbackReason();
                    AppendDebugLog(fb);
                    // 回放开始时再打一行醒目摘要，避免只扫过调试窗时漏看。
                    AppendDebugLog(L"【警告】当前不是驱动级注入，安全软件/反作弊可能仍按系统模拟处理。");
                }
            }

            auto wmSetPos = [this, wmExecPtr](int x, int y, int rx, int ry) {
                if (wmExecPtr && wmExecPtr->IsActive()) {
                    const bool hw = wmExecPtr->PreferHardwareInput();
                    if (hw) MarkSimulatedInput();
                    wmExecPtr->MoveMouseClient(x, y, rx, ry, [this](int r) { return RandomInt(r); }, true);
                    if (hw) UnmarkSimulatedInput();
                } else {
                    MarkSimulatedInput();
                    SetCursorScreenPos(x + RandomInt(rx), y + RandomInt(ry));
                    UnmarkSimulatedInput();
                }
            };
            auto wmSetLivePos = [this, wmExecPtr](int x, int y, int rx, int ry) {
                if (wmExecPtr && wmExecPtr->IsActive()) {
                    const bool hw = wmExecPtr->PreferHardwareInput();
                    if (hw) MarkSimulatedInput();
                    wmExecPtr->MoveMouseClient(x, y, rx, ry, [this](int r) { return RandomInt(r); }, false);
                    if (hw) UnmarkSimulatedInput();
                } else {
                    MarkSimulatedInput();
                    SetCursorScreenPos(x + RandomInt(rx), y + RandomInt(ry));
                    UnmarkSimulatedInput();
                }
            };

            auto wmUsesTarget = [wmExecPtr]() {
                return wmExecPtr && wmExecPtr->IsActive();
            };
            auto wmUsesBackground = [wmExecPtr]() {
                return wmExecPtr && wmExecPtr->IsActive() && wmExecPtr->UsesBackgroundWindow();
            };
            auto wmSendKey = [this, wmExecPtr, wmUsesTarget](UINT vk, bool down) {
                if (wmUsesTarget()) {
                    const bool hw = wmExecPtr->PreferHardwareInput();
                    if (hw) MarkSimulatedInput();
                    wmExecPtr->PostKeyToTarget(vk, down);
                    if (hw) UnmarkSimulatedInput();
                } else {
                    SendKey(vk, down);
                }
            };
            auto wmSendHeldModifiers = [wmSendKey](const ScriptAction& act, bool down) {
                if (act.holdLeftWin) wmSendKey(VK_LWIN, down);
                if (act.holdRightWin) wmSendKey(VK_RWIN, down);
                if (act.holdLeftCtrl) wmSendKey(VK_LCONTROL, down);
                if (act.holdRightCtrl) wmSendKey(VK_RCONTROL, down);
                if (act.holdLeftAlt) wmSendKey(VK_LMENU, down);
                if (act.holdRightAlt) wmSendKey(VK_RMENU, down);
                if (act.holdLeftShift) wmSendKey(VK_LSHIFT, down);
                if (act.holdRightShift) wmSendKey(VK_RSHIFT, down);
            };
            auto wmMouseButton = [this, wmExecPtr, wmUsesTarget](int cx, int cy, MouseButtonType btn, bool down) {
                if (wmUsesTarget()) {
                    const bool hw = wmExecPtr->PreferHardwareInput();
                    if (hw) MarkSimulatedInput();
                    wmExecPtr->PostMouseButtonAtClient(cx, cy, btn, down);
                    if (hw) UnmarkSimulatedInput();
                } else {
                    MouseButtonEvent(btn, down);
                }
            };
            auto wmMouseClick = [this, wmExecPtr, wmUsesTarget](int cx, int cy, MouseButtonType btn) {
                if (wmUsesTarget()) {
                    const bool hw = wmExecPtr->PreferHardwareInput();
                    if (hw) MarkSimulatedInput();
                    wmExecPtr->PostMouseClickAtClient(cx, cy, btn);
                    if (hw) UnmarkSimulatedInput();
                } else {
                    MouseClick(btn);
                }
            };
            auto wmSendShortcut = [wmSendKey](const ScriptAction& action) {
                ScriptAction tmp = action;
                ApplyShortcutPreset(tmp, action.shortcutPreset);
                if (tmp.holdLeftWin) wmSendKey(VK_LWIN, true);
                if (tmp.holdLeftCtrl) wmSendKey(VK_LCONTROL, true);
                if (tmp.holdLeftAlt) wmSendKey(VK_LMENU, true);
                if (tmp.holdLeftShift) wmSendKey(VK_LSHIFT, true);
                wmSendKey(tmp.keyVk, true);
                wmSendKey(tmp.keyVk, false);
                if (tmp.holdLeftShift) wmSendKey(VK_LSHIFT, false);
                if (tmp.holdLeftAlt) wmSendKey(VK_LMENU, false);
                if (tmp.holdLeftCtrl) wmSendKey(VK_LCONTROL, false);
                if (tmp.holdLeftWin) wmSendKey(VK_LWIN, false);
            };
            // 用户想用 Ctrl+Space / Win+Space / Alt+Shift 主动切换 IME：发键前不预切英文
            auto isImeToggleShortcut = [](const ScriptAction& a) {
                if (a.keyVk == VK_SPACE
                    && (a.holdLeftCtrl || a.holdRightCtrl
                        || a.holdLeftWin || a.holdRightWin))
                    return true;
                if ((a.holdLeftAlt || a.holdRightAlt)
                    && (a.keyVk == VK_LSHIFT || a.keyVk == VK_RSHIFT))
                    return true;
                return false;
            };

            auto clearLockedScreen = [&]() {
                if (lockedScreen_) {
                    DeleteBitmapHandle(lockedScreen_);
                    lockedScreen_ = nullptr;
                }
            };

            const std::vector<ScriptAction>* activeActions = &actions;
            std::wstring runningScriptPath = selfPath;

            auto containerBodyEnd = [&activeActions](size_t containerIndex) -> size_t {
                return static_cast<size_t>(ContainerBodyEnd(*activeActions, static_cast<int>(containerIndex)));
            };

            enum class RunRangeResult { Normal, BreakLoop, GotoPending };
            std::optional<size_t> pendingGoto;
            std::optional<size_t> loopEntryGotoTarget;
            bool pendingBreakLoop = false;
            std::function<RunRangeResult(size_t, size_t)> runRange;
            std::function<RunRangeResult(const std::wstring&)> runBlockByName;
            std::unordered_set<std::wstring> blockCallStack;

            auto notifyBreakoutUi = [&]() {
                HWND h = hwnd_;
                if (!h || !IsWindow(h)) return;
                DWORD_PTR result = 0;
                SendMessageTimeoutW(h, WM_APP_BREAKOUT_UI, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result);
            };
            auto waitBreakoutCooldown = [&]() {
                if (workerBreakoutTime_ <= 0 || StopRequested()) return;
                breakoutUserInput_ = false;
                breakoutPaused_ = true;
                const double t = workerBreakoutTime_;
                const auto idleMs = std::chrono::milliseconds(static_cast<int>(t * 1000.0));
                AppendBreakoutDebugLog(L"脱离时间：宏已中断，松开按键后等待 "
                    + FormatBreakoutTimeForEditor(t) + L" 秒再继续");
                notifyBreakoutUi();
                breakout_input::BreakoutCooldownState cool{};
                while (!StopRequested()) {
                    breakout_input::BreakoutReconcileUserHolds();
                    const bool holding = breakout_input::BreakoutUserHolding();
                    const bool fresh = breakoutUserInput_.exchange(false, std::memory_order_relaxed);
                    const auto now = std::chrono::steady_clock::now();
                    if (!breakout_input::BreakoutCooldownStillWaiting(
                            holding, fresh, now, idleMs, cool)) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                breakoutPaused_ = false;
                if (!StopRequested()) {
                    AppendBreakoutDebugLog(L"脱离时间：等待结束，从当前步骤重试后继续");
                }
                notifyBreakoutUi();
            };

            auto makeVarCtx = [&]() {
                MacroVariableContext ctx;
                ctx.matchVars = &matchVars_;
                ctx.ocrVars = usesOcr ? &ocrVars_ : nullptr;
                ctx.aiVars = &aiVars_;
                ctx.imageVars = &imageVars_;
                ctx.loopVars = &loopVars_;
                ctx.timerStarts = &timerStarts_;
                ctx.curLoops = curLoops_;
                return ctx;
            };

            auto resolveTemplatePath = [&](bool useVar, const std::wstring& stored) -> std::wstring {
                if (useVar) return ResolveRuntimeImagePath(stored, &imageVars_);
                return stored;
            };

            AiSessionStore aiSessions;
            int aiLoopDepth = 0;
            AiStepBudgetState aiRootBudget{};
            AiStepFrame* aiCurFrame = nullptr;
            const ScriptAction* aiInheritParent = nullptr;

            std::function<void(const ScriptAction&, const ScriptAction*)> runAiActionExecute;
            auto executeOne = std::function<void(const ScriptAction&)>();

            runAiActionExecute = [this, &usesOcr, &holdOcrSession, &heldKeyVk, &runRange, &runningScriptPath,
                &activeActions, &lockedScreen_, &lockedVirtX_, &lockedVirtY_, &clearLockedScreen, &makeVarCtx,
                &resolveTemplatePath,
                &executeOne, &runAiActionExecute, &aiSessions, &aiLoopDepth, &aiRootBudget, &aiCurFrame, &aiInheritParent,
                &pendingBreakLoop, wmExecPtr, &wmUsesTarget, &currentTmplScale, imageVarRunId](
                const ScriptAction& action,                 const ScriptAction* inheritFrom) {
                if (StopRequested()) return;
                AiActionExecuteNestGuard nestGuard;
                if (!nestGuard.entered()) {
                    AppendAiDebugLog(L"AI动作执行：嵌套已达上限（最多顶层+1层），跳过本步");
                    return;
                }
                ScriptAction eff = inheritFrom ? InheritAiActionFields(action, *inheritFrom) : action;
                aiInheritParent = &eff;

                // 逻辑转化：仅顶层录制；嵌套 AI 不开会话。
                // else 回退 AI（remark 含「逻辑转化回退」）走 heal 会话：可 promote，不清 memo。
                const bool logicConvertHeal = eff.aiLogicConvert
                    && AiActionExecuteNestDepth() <= 1
                    && eff.remark.find(L"逻辑转化回退") != std::wstring::npos;
                const bool logicConvertTop = eff.aiLogicConvert
                    && AiActionExecuteNestDepth() <= 1;
                if (logicConvertTop) {
                    std::wstring convertPath = runningScriptPath;
                    if (convertPath.empty()) {
                        std::lock_guard<std::mutex> lock(extScriptStateMu_);
                        convertPath = runningScriptPath_;
                    }
                    AiLogicConvertSessionBegin(true, eff.aiLogicBlockName, eff.aiPrompt,
                        convertPath, action.originalNo, logicConvertHeal);
                }

                auto flushLogicConvertWriteback = [&](const wchar_t* reasonTag) -> bool {
                    if (!AiLogicConvertSessionActive()) return false;
                    std::wstring pathFb = runningScriptPath;
                    if (pathFb.empty()) {
                        std::lock_guard<std::mutex> lock(extScriptStateMu_);
                        pathFb = runningScriptPath_;
                    }
                    std::wstring summary, err;
                    if (!TryFlushAiLogicConvertSession(pathFb, summary, err)) {
                        if (!err.empty()) {
                            AppendAiDebugLog(L"逻辑转化：写回未完成 — " + err
                                + (reasonTag && reasonTag[0]
                                    ? (L"（" + std::wstring(reasonTag) + L"）") : L""));
                        }
                        return false;
                    }
                    AppendAiDebugLog(L"逻辑转化：已写回脚本 → " + summary
                        + (reasonTag && reasonTag[0]
                            ? (L"（" + std::wstring(reasonTag) + L"）") : L""));
                    const std::wstring sp = AiLogicConvertSessionScriptPath().empty()
                        ? pathFb : AiLogicConvertSessionScriptPath();
                    NotifyLogicConvertUi(sp, summary);
                    NotifyAgentScriptLibraryChanged();
                    if (hwnd_ && !sp.empty()
                        && _wcsicmp(sp.c_str(), currentPath_.c_str()) == 0) {
                        PostMessageW(hwnd_, WM_APP_LOGIC_CONVERT_DONE, 0, 0);
                    }
                    return true;
                };

                AiStepFrame childFrame{};
                // Agent 闭环会执行大量原子动作（locate=move+click、wait…）；
                // aiMaxSteps 同时影响轮次，步骤预算需放大，否则中途「预算用尽」点不动。
                const int stepBudget = (eff.aiMaxSteps < 0)
                    ? -1
                    : std::clamp(std::max(eff.aiMaxSteps * 5, 40), 40, 200);
                childFrame.localMax = stepBudget;
                if (aiCurFrame && aiCurFrame->shared) {
                    childFrame.shared = aiCurFrame->shared;
                } else {
                    aiRootBudget = AiStepBudgetState{};
                    aiRootBudget.maxSteps = stepBudget;
                    childFrame.shared = &aiRootBudget;
                }
                AiStepFrame* prevFrame = aiCurFrame;
                aiCurFrame = &childFrame;

                auto propagateAiHistory = [&](AgentCore* core, const ScriptAction& action, size_t histBefore,
                    const std::wstring& sysPrompt, bool withTools, int maxTokens, int timeoutMs) {
                    if (core && action.aiContextMode != 0) {
                        aiSessions.PropagateHistoryAfterCall(
                            action.aiContextMode, aiLoopDepth, core, histBefore,
                            action, sysPrompt, appSettings_, timeoutMs, withTools, maxTokens);
                    }
                };

                auto resolveAiRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                    if (wmUsesTarget()) {
                        return wmExecPtr->ResolveAiScreenRect(
                            eff, x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    }
                    int sx = 0, sy = 0, rsw = 0, rsh = 0;
                    GetVirtualScreenRect(sx, sy, rsw, rsh);
                    int searchX1 = sx, searchY1 = sy, searchX2 = sx + rsw, searchY2 = sy + rsh;
                    if (eff.aiSearchX2 > eff.aiSearchX1 && eff.aiSearchY2 > eff.aiSearchY1) {
                        searchX1 = eff.aiSearchX1;
                        searchY1 = eff.aiSearchY1;
                        searchX2 = eff.aiSearchX2;
                        searchY2 = eff.aiSearchY2;
                    }
                    // 勾选「根据图片」：在绝对屏幕区域内找图，用匹配框作为最终截屏区
                    if (eff.aiRegionByImage && !eff.aiTargetImagePath.empty()) {
                        const TemplateScale tmplScale = currentTmplScale();
                        const std::wstring tmplPath = resolveTemplatePath(
                            eff.aiImageUseVar, eff.aiTargetImagePath);
                        HBITMAP tmpl = LoadBitmapFromFile(tmplPath);
                        if (!tmpl) return false;
                        ImageMatchOptions opt = BuildExecutionFindImageOptions(eff, tmplScale);
                        opt.maxMatches = 20;
                        opt.maxOverlap = 0.5;
                        ImageMatchOutput output;
                        if (lockedScreen_) {
                            output = FindTemplateInFrozenScreenMulti(
                                lockedScreen_, lockedVirtX_, lockedVirtY_,
                                searchX1, searchY1, searchX2, searchY2, tmpl, opt);
                        } else {
                            output = FindTemplateOnScreenMulti(
                                searchX1, searchY1, searchX2, searchY2, tmpl, opt);
                        }
                        DeleteBitmapHandle(tmpl);
                        if (output.matches.empty()) return false;
                        const ImageMatchResult& match = output.matches.front();
                        return ApplyImageRegionToMatch(eff,
                            match.topLeftX, match.topLeftY,
                            match.bottomRightX, match.bottomRightY,
                            x1, y1, x2, y2);
                    }
                    x1 = searchX1; y1 = searchY1; x2 = searchX2; y2 = searchY2;
                    return x2 > x1 && y2 > y1;
                };

                MacroVariableContext ctx = makeVarCtx();
                MacroClipboardSnapshot clipSnap;
                std::vector<std::string> clipExtraJpeg;
                if (PromptMentionsCtrlClipboard(eff.aiPrompt)) {
                    clipSnap = ReadMacroClipboardSnapshot();
                    ctx.clipboardSnapshot = &clipSnap;
                    ctx.clipboardExpandMode = ClipboardExpandMode::Ai;
                    clipExtraJpeg = EncodeClipboardSnapshotImages(clipSnap);
                    if (!clipExtraJpeg.empty()) {
                        AppendAiDebugLog(L"AI动作执行：附加剪贴板图片 "
                            + std::to_wstring(clipExtraJpeg.size()) + L" 张");
                    } else if (clipSnap.hasBitmap || std::any_of(
                        clipSnap.files.begin(), clipSnap.files.end(), LooksLikeImageFilePath)) {
                        AppendAiDebugLog(L"AI动作执行：提示词引用了剪贴板图片，但未能编码附图");
                    }
                }
                const std::wstring resolvedPrompt = ResolveMacroVariables(eff.aiPrompt, ctx);
                const std::wstring effModel = EffectiveAiModelName(eff);
                ScriptAction prepAction = eff;
                prepAction.aiModelName = effModel;

                AiCaptureMapping liveMap{};
                bool liveMapValid = false;
                // openWebpage 等动作后的本地 settle 结果（注入下一轮 Agent 观察提示）
                std::wstring lastUiSettleHint;
                bool lastUiSettleSuggestRefresh = false;
                int lastUiSettleElapsedMs = 0;
                bool lastUiSettleReacted = false;
                bool lastUiSettleSettled = false;
                std::wstring lastUiChangeRoisText;
                /// settle 刚写入 aiObs 后，下一轮观察必须上传，避免被「未变」短路
                bool forceNextObserveUpload = false;
                /// 最近一次定位/点击的屏幕坐标（动作局部验收，抑制视频区抢注意力）
                int lastActionScreenX = -1;
                int lastActionScreenY = -1;
                int nearDupClickCount = 0;
                int consecutiveDynamicOnlyObserves = 0;
                /// Alt+Tab 预览期间按住左 Alt（勿一按即松）
                bool altTabAltHeld = false;
                /// 预览已跨过的 API 轮数：Alt 悬太久会挡住整个桌面，超限强制松开
                int altTabHoldRounds = 0;

                auto releaseAltTabIfHeld = [&]() {
                    if (!altTabAltHeld) return;
                    SendKeyboardKey(VK_LMENU, false);
                    altTabAltHeld = false;
                    altTabHoldRounds = 0;
                };

                auto notePointerClick = [&](int sx, int sy, bool isPrimaryLeft) -> std::wstring {
                    if (sx < 0 || sy < 0) return {};
                    // 右键菜单常需同点再试/点菜单项，勿用近点拒绝误伤
                    if (!isPrimaryLeft) {
                        lastActionScreenX = sx;
                        lastActionScreenY = sy;
                        nearDupClickCount = 0;
                        return {};
                    }
                    if (lastActionScreenX >= 0 && lastActionScreenY >= 0) {
                        const long long dx = static_cast<long long>(sx) - lastActionScreenX;
                        const long long dy = static_cast<long long>(sy) - lastActionScreenY;
                        if (dx * dx + dy * dy <= 56LL * 56LL) {
                            ++nearDupClickCount;
                            if (nearDupClickCount >= 2) {
                                return L"[错误] 已连续在相近位置左键点击。请根据界面判断是否已成功"
                                    L"（如点赞高亮），达成则 completeTask，勿重复点击。"
                                    L"切窗用 activateWindow；打开桌面图标请 doubleClick=true。";
                            }
                        } else {
                            nearDupClickCount = 1;
                        }
                    } else {
                        nearDupClickCount = 1;
                    }
                    lastActionScreenX = sx;
                    lastActionScreenY = sy;
                    return {};
                };

                // 光标挪到虚拟屏角落（直接 SetCursor，勿走 executeActionsJson）
                auto parkCursorAwayFromUi = [&]() {
                    if (wmExecPtr && wmExecPtr->IsActive() && wmExecPtr->PreferHardwareInput()) {
                        return;
                    }
                    int vx = 0, vy = 0, vw = 0, vh = 0;
                    GetVirtualScreenRect(vx, vy, vw, vh);
                    if (vw < 32 || vh < 32) return;
                    SetCursorScreenPos(vx + std::max(8, vw) - 4, vy + std::max(8, vh) - 4);
                };

                auto executeActionsJsonNow = [&](const std::wstring& rawJson) -> std::wstring {
                    // 不变量：Alt 只在连续的 switchWindow 之间按住。
                    // 一旦改做别的动作就先松开（=confirm 落到选中窗），
                    // 否则 Alt 悬着会把后续点击变成 Alt+点击，且预览一直挂在屏幕上。
                    std::wstring altNote;
                    if (altTabAltHeld) {
                        releaseAltTabIfHeld();
                        Sleep(120);
                        AppendAiDebugLog(L"  [诊断] 非 switchWindow 动作：已先松开 Alt 结束预览");
                        altNote = L"；已自动松开 Alt 结束 Alt+Tab 预览（切到当时选中的窗口）";
                    }
                    // 点击/键鼠前藏壳与调试窗，避免挡桌面目标或进截屏
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    std::wstring jsonStr = Trim(rawJson);
                    const size_t bracketStart = jsonStr.find(L'[');
                    const size_t bracketEnd = jsonStr.rfind(L']');
                    if (bracketStart != std::wstring::npos && bracketEnd != std::wstring::npos
                        && bracketEnd > bracketStart) {
                        jsonStr = jsonStr.substr(bracketStart, bracketEnd - bracketStart + 1);
                    }
                    try {
                        nlohmann::json steps = nlohmann::json::parse(ToUtf8(jsonStr));
                        if (!steps.is_array())
                            return L"[错误] 返回内容不是 JSON 数组";
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：即时执行本批 "
                            + std::to_wstring(steps.size()) + L" 个动作");
                        bool settleNoReactionThisBatch = false;

                        // 截图坐标系 → 屏幕坐标（与 CompositeClick 一致；缩放/选区未映射会点偏）
                        const bool remapApi = liveMapValid
                            && liveMap.apiWidth > 0 && liveMap.apiHeight > 0
                            && liveMap.capX2 > liveMap.capX1 && liveMap.capY2 > liveMap.capY1;
                        // false = 应跳过本步（坐标无法解释，禁止放大飞点）
                        auto remapStepCoords = [&](nlohmann::json& params) -> bool {
                            if (!remapApi || !params.is_object()) return true;
                            if (!params.contains("type") || !params["type"].is_string()) return true;
                            const std::string type = params["type"].get<std::string>();
                            if (type != "moveMouse" && type != "mouseClick"
                                && type != "mouseDown" && type != "mouseUp") {
                                return true;
                            }
                            // locateAndClick 等已映射为屏幕绝对坐标，禁止再乘截图缩放
                            if (params.contains("coordSpace") && params["coordSpace"].is_string()
                                && params["coordSpace"].get<std::string>() == "screen") {
                                return true;
                            }
                            if (params.contains("moveFromVar")) {
                                const auto& mv = params["moveFromVar"];
                                if (mv.is_boolean() && mv.get<bool>()) return true;
                                if (mv.is_number_integer() && mv.get<int>() != 0) return true;
                            }
                            if (!params.contains("x") || !params.contains("y")) return true;
                            int apiX = 0, apiY = 0;
                            try {
                                if (params["x"].is_number()) apiX = params["x"].get<int>();
                                else if (params["x"].is_string()) apiX = std::stoi(params["x"].get<std::string>());
                                else return true;
                                if (params["y"].is_number()) apiY = params["y"].get<int>();
                                else if (params["y"].is_string()) apiY = std::stoi(params["y"].get<std::string>());
                                else return true;
                            } catch (...) {
                                return true;
                            }
                            const int rawX = apiX, rawY = apiY;
                            std::wstring coordNote;
                            if (!ResolveAgentPointerToApiImage(apiX, apiY,
                                    liveMap.apiWidth, liveMap.apiHeight,
                                    liveMap.srcWidth, liveMap.srcHeight, &coordNote)) {
                                AppendAiDebugLog(L"  跳过越界坐标：("
                                    + std::to_wstring(rawX) + L"," + std::to_wstring(rawY)
                                    + L") 不在截图 " + std::to_wstring(liveMap.apiWidth) + L"×"
                                    + std::to_wstring(liveMap.apiHeight)
                                    + L"；请用 locateAndClick，勿猜绝对坐标");
                                return false;
                            }
                            int screenX = apiX, screenY = apiY;
                            MapApiPointToScreen(liveMap, apiX, apiY, screenX, screenY);
                            params["x"] = screenX;
                            params["y"] = screenY;
                            if ((rawX != apiX || rawY != apiY) && !coordNote.empty()) {
                                AppendAiDebugLog(L"  [诊断] 指针坐标("
                                    + std::to_wstring(rawX) + L"," + std::to_wstring(rawY)
                                    + L")→api(" + std::to_wstring(apiX) + L","
                                    + std::to_wstring(apiY) + L") " + coordNote
                                    + L" → 屏幕(" + std::to_wstring(screenX) + L","
                                    + std::to_wstring(screenY) + L")");
                            }
                            return true;
                        };

                        int stepCount = 0;
                        int skippedBadPointer = 0;
                        int skippedInvalid = 0;
                        std::wstring firstInvalidError;
                        // 近点重复点击被拒 ≠ 坐标越界，回给模型的理由必须分开
                        std::wstring skippedDupNote;
                        // 批量配方里每行都有 Enter：只在「本批最后一个会触发界面变化的步骤」上 settle，
                        // 中间行狂等会把 10 行填表拖成十几秒空转
                        auto stepWantsInteractionSettle = [](const nlohmann::json& raw) -> bool {
                            if (!raw.is_object()) return false;
                            std::string type;
                            nlohmann::json p = raw;
                            if (raw.contains("action") && raw["action"].is_string()) {
                                type = raw["action"].get<std::string>();
                                p = raw.value("params", nlohmann::json::object());
                            } else if (raw.contains("type") && raw["type"].is_string()) {
                                type = raw["type"].get<std::string>();
                            } else {
                                return false;
                            }
                            if (type == "mouseMove") type = "moveMouse";
                            if (type == "mouseClick" || type == "hotkeyShortcut") return true;
                            if (type != "keyClick") return false;
                            try {
                                std::wstring kt;
                                if (p.contains("keyText") && p["keyText"].is_string())
                                    kt = FromUtf8(p["keyText"].get<std::string>());
                                for (auto& c : kt) {
                                    if (c >= L'a' && c <= L'z')
                                        c = static_cast<wchar_t>(c - L'a' + L'A');
                                }
                                auto flag = [&](const char* k) {
                                    return p.contains(k) && p[k].is_boolean() && p[k].get<bool>();
                                };
                                return kt == L"ENTER" || kt == L"RETURN" || kt == L"ESCAPE"
                                    || kt == L"F5"
                                    || flag("holdLeftCtrl") || flag("holdLeftAlt")
                                    || flag("holdLeftWin");
                            } catch (...) {
                                return false;
                            }
                        };
                        for (size_t stepIdx = 0; stepIdx < steps.size(); ++stepIdx) {
                            const auto& step = steps[stepIdx];
                            if (StopRequested()) break;
                            if (!step.is_object()) continue;

                            nlohmann::json params;
                            std::wstring actionType;
                            if (step.contains("action")) {
                                actionType = FromUtf8(step["action"].get<std::string>());
                                if (actionType == L"mouseMove") actionType = L"moveMouse";
                                params = step.value("params", nlohmann::json::object());
                                if (!params.is_object()) params = nlohmann::json::object();
                                params["type"] = ToUtf8(actionType);
                            } else if (step.contains("type")) {
                                params = step;
                                actionType = FromUtf8(step["type"].get<std::string>());
                            } else {
                                continue;
                            }

                            // Agent 闭环：stopMacro 不占步骤预算（构建器常自动追加，否则 10 步很快耗尽）
                            if (actionType == L"stopMacro") continue;

                            if (!remapStepCoords(params)) {
                                ++skippedBadPointer;
                                continue;
                            }

                            auto built = BuildScriptActionFromJson(params);
                            if (!built.ok) {
                                AppendAiDebugLog(L"  跳过无效动作：" + built.error);
                                ++skippedInvalid;
                                if (firstInvalidError.empty()) firstInvalidError = built.error;
                                continue;
                            }
                            ScriptAction stepAction = InheritAiActionFields(built.action, eff);
                            if (stepAction.type == ActionType::MouseClick
                                && params.contains("x") && params.contains("y")) {
                                try {
                                    int sx = lastActionScreenX, sy = lastActionScreenY;
                                    if (params["x"].is_number()) sx = params["x"].get<int>();
                                    if (params["y"].is_number()) sy = params["y"].get<int>();
                                    // locateAndClick 已带 coordSpace=screen 并做过近点校验；此处只拦 Agent 盲点
                                    const bool fromLocate =
                                        params.contains("coordSpace") && params["coordSpace"].is_string()
                                        && params["coordSpace"].get<std::string>() == "screen";
                                    std::string btn = "left";
                                    if (params.contains("button") && params["button"].is_string())
                                        btn = params["button"].get<std::string>();
                                    const bool primaryLeft = (btn == "left" || btn.empty());
                                    if (!fromLocate) {
                                        if (const std::wstring dup = notePointerClick(sx, sy, primaryLeft);
                                            !dup.empty()) {
                                            AppendAiDebugLog(L"  " + dup);
                                            if (skippedDupNote.empty()) skippedDupNote = dup;
                                            continue;
                                        }
                                    } else {
                                        lastActionScreenX = sx;
                                        lastActionScreenY = sy;
                                    }
                                } catch (...) {}
                            } else if (stepAction.type == ActionType::MoveMouse
                                && params.contains("x") && params.contains("y")) {
                                try {
                                    if (params["x"].is_number())
                                        lastActionScreenX = params["x"].get<int>();
                                    if (params["y"].is_number())
                                        lastActionScreenY = params["y"].get<int>();
                                } catch (...) {}
                            }
                            if (stepAction.type == ActionType::EndLoop) {
                                if (aiLoopDepth <= 0) {
                                    AppendAiDebugLog(L"  跳过结束循环：" + std::wstring(kEndLoopNeedsLoopParentMsg));
                                    continue;
                                }
                            }

                            if (!ConsumeAiStep(childFrame)) {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：步骤预算已用尽");
                                break;
                            }

                            AppendAiDebugLog(L"  执行 " + ActionName(stepAction)
                                + L" (步" + std::to_wstring(stepCount + 1) + L")");
                            // 开网页/启动程序：操作前截基线，操作后本地「反应→稳定」二次校验
                            // （无固定死延时；程序冷启动慢时避免观察抢在窗口出现之前）
                            bool isDoubleClickStep = false;
                            if (stepAction.type == ActionType::MouseClick) {
                                try {
                                    if (params.contains("clickCount")
                                        && params["clickCount"].is_number()) {
                                        isDoubleClickStep = params["clickCount"].get<int>() >= 2;
                                    }
                                } catch (...) {}
                            }
                            // 双击通常等同「打开」，同样要等窗口起来再观察
                            const bool wantsLaunchSettle =
                                stepAction.type == ActionType::OpenWebpage
                                || stepAction.type == ActionType::RunProgram
                                || stepAction.type == ActionType::OpenFile
                                || isDoubleClickStep;
                            // 点击/回车/快捷键后界面常要几百毫秒才画完：宿主就地等一下，
                            // 省掉 Agent 为此单开一轮 wait（一轮 = 一次 API + 常带图）
                            bool opensUiKey = false;
                            if (stepAction.type == ActionType::KeyClick) {
                                try {
                                    std::wstring kt;
                                    if (params.contains("keyText") && params["keyText"].is_string())
                                        kt = FromUtf8(params["keyText"].get<std::string>());
                                    for (auto& c : kt) {
                                        if (c >= L'a' && c <= L'z')
                                            c = static_cast<wchar_t>(c - L'a' + L'A');
                                    }
                                    auto flag = [&](const char* k) {
                                        return params.contains(k) && params[k].is_boolean()
                                            && params[k].get<bool>();
                                    };
                                    opensUiKey = kt == L"ENTER" || kt == L"RETURN"
                                        || kt == L"ESCAPE" || kt == L"F5"
                                        || flag("holdLeftCtrl") || flag("holdLeftAlt")
                                        || flag("holdLeftWin");
                                } catch (...) {}
                            }
                            bool wantsInteractionSettle = !wantsLaunchSettle
                                && (stepAction.type == ActionType::MouseClick
                                    || stepAction.type == ActionType::HotkeyShortcut
                                    || opensUiKey);
                            if (wantsInteractionSettle) {
                                for (size_t j = stepIdx + 1; j < steps.size(); ++j) {
                                    if (stepWantsInteractionSettle(steps[j])) {
                                        wantsInteractionSettle = false;
                                        break;
                                    }
                                }
                            }
                            const bool wantsSettle = wantsLaunchSettle || wantsInteractionSettle;
                            HBITMAP settleBaseline = nullptr;
                            if (wantsSettle && !StopRequested()) {
                                int bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
                                if (resolveAiRegion(bx1, by1, bx2, by2)) {
                                    if (wmUsesTarget()) {
                                        settleBaseline = wmExecPtr->CaptureScreenRegionFromWindow(
                                            bx1, by1, bx2, by2,
                                            lockedScreen_, lockedVirtX_, lockedVirtY_);
                                    } else {
                                        settleBaseline = CaptureScreenRegion(bx1, by1, bx2, by2);
                                    }
                                }
                            }

                            if (stepAction.type == ActionType::AiActionExecute) {
                                // 允许有限嵌套；达上限时 NestGuard 会跳过并打日志
                                runAiActionExecute(stepAction, &eff);
                            } else {
                                executeOne(stepAction);
                            }
                            // ShellExecute 失败时勿假成功：立刻回报，引导走开始菜单搜索兜底
                            if ((stepAction.type == ActionType::RunProgram
                                    || stepAction.type == ActionType::OpenFile
                                    || stepAction.type == ActionType::OpenWebpage)
                                && !LastLaunchProgramError().empty()) {
                                return L"[错误] " + LastLaunchProgramError();
                            }
                            // 逻辑转化：仅在真正执行成功后记轨迹（避免幽灵步骤）
                            if (AiLogicConvertSessionActive()
                                && stepAction.type != ActionType::AiActionExecute) {
                                AiLogicConvertNoteAction(stepAction);
                            }

                            if (wantsSettle && settleBaseline && !StopRequested()) {
                                int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                                resolveAiRegion(cx1, cy1, cx2, cy2);
                                UiVisualSettleOptions sopt;
                                if (wantsLaunchSettle) {
                                    sopt.pollIntervalMs = 150;
                                    sopt.reactDeadlineMs = 2200;
                                    sopt.stableHoldMs = 400;
                                    sopt.maxTotalMs = 4200;
                                    sopt.refreshSuggestMs = 4200;
                                } else if (wantsInteractionSettle) {
                                    // 交互反馈是毫秒级：预算给足 2.2s 就够，别把每步都拖成冷启动
                                    sopt.pollIntervalMs = 110;
                                    sopt.reactDeadlineMs = 700;
                                    sopt.stableHoldMs = 260;
                                    sopt.maxTotalMs = 2200;
                                    sopt.refreshSuggestMs = 2200;
                                }
                                const UiVisualSettleResult settled = WaitUiReactThenSettle(
                                    settleBaseline,
                                    [&]() -> HBITMAP {
                                        if (StopRequested()) return nullptr;
                                        if (wmUsesTarget()) {
                                            return wmExecPtr->CaptureScreenRegionFromWindow(
                                                cx1, cy1, cx2, cy2,
                                                lockedScreen_, lockedVirtX_, lockedVirtY_);
                                        }
                                        return CaptureScreenRegion(cx1, cy1, cx2, cy2);
                                    },
                                    stopFlag_, sopt);
                                DeleteBitmapHandle(settleBaseline);
                                settleBaseline = nullptr;
                                AppendAiDebugLog(L"  [诊断] " + settled.logLine);
                                lastUiSettleHint = settled.agentHint;
                                lastUiSettleSuggestRefresh = settled.suggestRefresh;
                                lastUiSettleElapsedMs = settled.elapsedMs;
                                lastUiSettleReacted = settled.reacted;
                                lastUiSettleSettled = settled.settled;
                                if (!settled.reacted)
                                    settleNoReactionThisBatch = true;
                                lastUiChangeRoisText.clear();
                                for (size_t i = 0; i < settled.lastChangeRois.size() && i < 4; ++i) {
                                    const auto& r = settled.lastChangeRois[i];
                                    if (!lastUiChangeRoisText.empty()) lastUiChangeRoisText += L";";
                                    lastUiChangeRoisText += std::to_wstring(r.x1) + L","
                                        + std::to_wstring(r.y1) + L","
                                        + std::to_wstring(r.x2) + L","
                                        + std::to_wstring(r.y2);
                                }
                                if (settled.lastFrame) {
                                    CommitSavedImage(settled.lastFrame, kAiObsImageVarName,
                                        imageVarRunId, imageVars_);
                                    DeleteBitmapHandle(settled.lastFrame);
                                    // 交互后界面没动就别强推图：交给差分决定，省一张截图的钱
                                    if (wantsLaunchSettle || settled.reacted)
                                        forceNextObserveUpload = true;
                                }
                            } else if (settleBaseline) {
                                DeleteBitmapHandle(settleBaseline);
                            }
                            if (pendingBreakLoop) break;
                            ++stepCount;
                        }
                        // 点击/移标后把光标泊到角落，降低 hover 对后续观察的干扰
                        if (lastActionScreenX >= 0)
                            parkCursorAwayFromUi();
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：本批完成 "
                            + std::to_wstring(stepCount) + L" 步");
                        // 模态框（尤其覆盖确认）像素差分极小，会被「界面未变」吞掉让模型瞎猜。
                        // 这里用窗口枚举直报，并强制下一轮上传画面。
                        if (stepCount > 0) {
                            const windowmode::ForegroundDialogInfo dlg =
                                windowmode::ProbeForegroundDialog();
                            if (dlg.present) {
                                forceNextObserveUpload = true;
                                std::wstring note = L"\n[对话框] kind=" + dlg.kind
                                    + L"；" + dlg.title;
                                if (!dlg.buttons.empty()) note += L"；按钮：" + dlg.buttons;
                                AppendAiDebugLog(L"  [诊断] 前台对话框：" + dlg.kind + L" "
                                    + dlg.title);
                                altNote += note;
                            }
                        }
                        if (stepCount == 0 && !skippedDupNote.empty()) {
                            return skippedDupNote
                                + L"\n若目标是桌面图标/文件：单击只会选中，打开请用 "
                                  L"locateAndClick(target=..., doubleClick=true) 或 openFile(路径)。"
                                + altNote;
                        }
                        if (stepCount == 0 && skippedBadPointer > 0) {
                            return L"[错误] 坐标越界已拒绝 "
                                + std::to_wstring(skippedBadPointer)
                                + L" 步；请用 locateAndClick，勿猜绝对坐标" + altNote;
                        }
                        if (stepCount == 0 && skippedInvalid > 0) {
                            return L"[错误] 本批 0 步：" + firstInvalidError
                                + (skippedInvalid > 1
                                    ? (L"（另有 " + std::to_wstring(skippedInvalid - 1)
                                        + L" 个无效动作）")
                                    : L"")
                                + altNote;
                        }
                        if (stepCount == 0) {
                            return L"[错误] 本批 0 步：没有可执行的动作（空数组或全部被跳过）"
                                + altNote;
                        }
                        std::wstring settleFact;
                        if (settleNoReactionThisBatch) {
                            settleFact = L"\n[事实] settle无反应：界面相对操作前几乎无变化。";
                        }
                        if (!skippedDupNote.empty()) {
                            return L"已执行 " + std::to_wstring(stepCount)
                                + L" 步；另跳过重复近点点击 1 步" + altNote + settleFact;
                        }
                        if (skippedBadPointer > 0) {
                            return L"已执行 " + std::to_wstring(stepCount) + L" 步；另跳过越界坐标 "
                                + std::to_wstring(skippedBadPointer) + L" 步" + altNote
                                + settleFact;
                        }
                        if (skippedInvalid > 0) {
                            return L"已执行 " + std::to_wstring(stepCount)
                                + L" 步；另跳过无效动作 "
                                + std::to_wstring(skippedInvalid) + L" 个："
                                + firstInvalidError + altNote + settleFact;
                        }
                        return L"已执行 " + std::to_wstring(stepCount) + L" 步" + altNote
                            + settleFact;
                    } catch (...) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：JSON 解析失败");
                        return L"[错误] JSON 解析失败";
                    }
                };

                // maxLongEdge：观察默认 768（省 token）；locate 用 1152
                auto captureObservationNow = [&](std::string& outB64, int& outW, int& outH,
                    int maxLongEdge = 768, double scaleOverride = 0.0) -> bool {
                    outB64.clear();
                    outW = outH = 0;
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    parkCursorAwayFromUi();
                    Sleep(40);
                    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                    if (!resolveAiRegion(cx1, cy1, cx2, cy2)) return false;
                    // 定位诊断：打印截图区域与前台窗口位置，便于判断截图是否盖住目标
                    // （如窗口底部的输入框被截在画面外）。
                    {
                        wchar_t title[256]{};
                        HWND fg = GetForegroundWindow();
                        RECT fr{};
                        if (fg) {
                            GetWindowTextW(fg, title, 256);
                            GetWindowRect(fg, &fr);
                        }
                        AppendAiDebugLog(L"  [诊断] 截图区域=("
                            + std::to_wstring(cx1) + L"," + std::to_wstring(cy1)
                            + L")-(" + std::to_wstring(cx2) + L"," + std::to_wstring(cy2)
                            + L") 前台窗口=「" + std::wstring(title) + L"」rect=("
                            + std::to_wstring(fr.left) + L"," + std::to_wstring(fr.top)
                            + L")-(" + std::to_wstring(fr.right) + L","
                            + std::to_wstring(fr.bottom)
                            + L") 窗口模式=" + (wmUsesTarget() ? L"是" : L"否"));
                    }
                    HBITMAP bmp = nullptr;
                    if (wmUsesTarget()) {
                        bmp = wmExecPtr->CaptureScreenRegionFromWindow(
                            cx1, cy1, cx2, cy2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    } else {
                        bmp = CaptureAiRegionComposed(cx1, cy1, cx2, cy2);
                    }
                    if (!bmp) return false;
                    CommitSavedImage(bmp, kAiObsImageVarName, imageVarRunId, imageVars_);
                    // 定位等视觉关键路径可用 scaleOverride 强制高清（默认仍跟 aiImageScale）
                    const double scale = scaleOverride > 0.0
                        ? std::clamp(scaleOverride, 0.1, 1.0)
                        : std::clamp(
                            eff.aiImageScale > 0.0 ? eff.aiImageScale : 0.5, 0.1, 1.0);
                    // 输入法状态叠加到截图：让 Agent 直接「看图」识别中/英文与组字残留，
                    // 而非靠盲猜或乱按快捷键。TSF 输入法候选框 CAPTUREBLT 拍不到，
                    // 画到图上是最可靠的感知途径。仅中文模式时叠加（避免每帧红字遮挡左上角）。
                    std::wstring imeStatus;
                    if (!wmUsesTarget()) {
                        imeStatus = QueryForegroundImeStatusText();
                        if (imeStatus.find(L"[输入法] 英文") != std::wstring::npos)
                            imeStatus.clear();
                    }
                    const AiImageEncodeResult enc = EncodeBitmapForAiAnalysis(
                        bmp, scale, maxLongEdge, imeStatus.empty() ? nullptr : &imeStatus);
                    DeleteBitmapHandle(bmp);
                    if (enc.base64.empty()) return false;
                    outB64 = enc.base64;
                    outW = enc.outWidth;
                    outH = enc.outHeight;
                    liveMap.capX1 = cx1;
                    liveMap.capY1 = cy1;
                    liveMap.capX2 = cx2;
                    liveMap.capY2 = cy2;
                    liveMap.srcWidth = enc.srcWidth;
                    liveMap.srcHeight = enc.srcHeight;
                    liveMap.apiWidth = enc.outWidth;
                    liveMap.apiHeight = enc.outHeight;
                    liveMapValid = liveMap.apiWidth > 0 && liveMap.capX2 > liveMap.capX1;
                    return true;
                };

                auto captureAiRegionBmp = [&](int cx1, int cy1, int cx2, int cy2) -> HBITMAP {
                    if (wmUsesTarget()) {
                        return wmExecPtr->CaptureScreenRegionFromWindow(
                            cx1, cy1, cx2, cy2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    }
                    return CaptureAiRegionComposed(cx1, cy1, cx2, cy2);
                };

                /// 智能观察：三帧标动态区 → 结构差分；视频播放不触发反复上传
                auto observeScreenForAgent = [&](bool forceRefresh) -> AiObserveCaptureResult {
                    // Alt+Tab 预览期间勿藏窗/挪标，否则切换器会关掉
                    std::unique_ptr<qst::desktop_tools::ScopedHideOwnUiForCapture> hideOwn;
                    if (!altTabAltHeld) {
                        hideOwn = std::make_unique<qst::desktop_tools::ScopedHideOwnUiForCapture>(
                            UserFacingMainHwnd());
                        parkCursorAwayFromUi();
                    }
                    AiObserveCaptureResult r;
                    if (!lastUiSettleHint.empty()) {
                        r.settleChecked = true;
                        r.uiReacted = lastUiSettleReacted;
                        r.uiSettled = lastUiSettleSettled;
                        r.suggestRefresh = lastUiSettleSuggestRefresh;
                        r.settleElapsedMs = lastUiSettleElapsedMs;
                        r.settleHint = lastUiSettleHint;
                        r.changeRoisText = lastUiChangeRoisText;
                        lastUiSettleHint.clear();
                    }
                    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                    if (!resolveAiRegion(cx1, cy1, cx2, cy2)) return r;

                    // 三帧短采样：标出持续运动格子（视频），再与 aiObs 做结构差分
                    HBITMAP f0 = captureAiRegionBmp(cx1, cy1, cx2, cy2);
                    if (!f0) return r;
                    Sleep(90);
                    HBITMAP f1 = captureAiRegionBmp(cx1, cy1, cx2, cy2);
                    Sleep(90);
                    HBITMAP f2 = captureAiRegionBmp(cx1, cy1, cx2, cy2);
                    if (!f1 || !f2) {
                        if (f1) DeleteBitmapHandle(f1);
                        if (f2) DeleteBitmapHandle(f2);
                        // 退化：单帧
                        f1 = f2 = nullptr;
                    }

                    ScreenBusyMask busy{};
                    HBITMAP bmp = f2 ? f2 : f0;
                    if (f1 && f2) {
                        busy = BuildBusyMaskFromTripleFrames(f0, f1, f2, 12, 32, 0.08);
                        DeleteBitmapHandle(f0);
                        DeleteBitmapHandle(f1);
                        f0 = f1 = nullptr;
                        // f2 作为当前帧
                    } else {
                        bmp = f0;
                        f0 = nullptr;
                    }

                    const bool skipUnchangedCheck = forceRefresh || forceNextObserveUpload;
                    forceNextObserveUpload = false;

                    if (!skipUnchangedCheck) {
                        const auto it = imageVars_.find(kAiObsImageVarName);
                        if (it != imageVars_.end() && !it->second.empty()) {
                            HBITMAP baseline = LoadBitmapFromFile(it->second);
                            if (baseline) {
                                BITMAP bb{}, cb{};
                                const bool sizeOk =
                                    GetObjectW(baseline, sizeof(bb), &bb)
                                    && GetObjectW(bmp, sizeof(cb), &cb)
                                    && bb.bmWidth > 0 && bb.bmHeight > 0
                                    && bb.bmWidth == cb.bmWidth && bb.bmHeight == cb.bmHeight;
                                if (sizeOk) {
                                    const ScreenChangeDiffResult diff = DiffBitmapsChangedRegions(
                                        baseline, bmp, 12, 48, 6,
                                        busy.valid() ? &busy : nullptr);
                                    r.changedRatio = diff.changedRatio;
                                    r.rawChangedRatio = diff.rawChangedRatio;
                                    r.busyCoverageRatio = diff.busyCoverageRatio;
                                    r.onlyDynamicChanged = diff.onlyDynamicChanged;
                                    r.matchScore = diff.nearlyIdentical
                                        ? 100.0
                                        : std::max(0.0, (1.0 - diff.changedRatio) * 100.0);

                                    // 动作局部：点击附近是否有结构变化（点赞态等）
                                    bool actionLocalStructural = false;
                                    if (lastActionScreenX >= 0 && lastActionScreenY >= 0
                                        && !diff.rois.empty()) {
                                        const int lx = lastActionScreenX - cx1;
                                        const int ly = lastActionScreenY - cy1;
                                        for (const auto& rr : diff.rois) {
                                            if (lx >= rr.x1 - 80 && lx < rr.x2 + 80
                                                && ly >= rr.y1 - 80 && ly < rr.y2 + 80) {
                                                actionLocalStructural = true;
                                                break;
                                            }
                                        }
                                    }

                                    auto fillRois = [&]() {
                                        r.changeRoisText.clear();
                                        // 优先动作附近的结构 ROI，避免视频大框抢注意力
                                        std::vector<ScreenChangeRoi> ordered = diff.rois;
                                        if (lastActionScreenX >= 0) {
                                            const int lx = lastActionScreenX - cx1;
                                            const int ly = lastActionScreenY - cy1;
                                            std::stable_sort(ordered.begin(), ordered.end(),
                                                [&](const ScreenChangeRoi& a, const ScreenChangeRoi& b) {
                                                    const int acx = (a.x1 + a.x2) / 2;
                                                    const int acy = (a.y1 + a.y2) / 2;
                                                    const int bcx = (b.x1 + b.x2) / 2;
                                                    const int bcy = (b.y1 + b.y2) / 2;
                                                    const long long da =
                                                        1LL * (acx - lx) * (acx - lx)
                                                        + 1LL * (acy - ly) * (acy - ly);
                                                    const long long db =
                                                        1LL * (bcx - lx) * (bcx - lx)
                                                        + 1LL * (bcy - ly) * (bcy - ly);
                                                    return da < db;
                                                });
                                        }
                                        for (size_t i = 0; i < ordered.size() && i < 4; ++i) {
                                            const auto& rr = ordered[i];
                                            if (!r.changeRoisText.empty()) r.changeRoisText += L";";
                                            r.changeRoisText += std::to_wstring(rr.x1) + L","
                                                + std::to_wstring(rr.y1) + L","
                                                + std::to_wstring(rr.x2) + L","
                                                + std::to_wstring(rr.y2);
                                        }
                                    };

                                    const bool structurallyQuiet = diff.sameSize
                                        && (diff.nearlyIdentical || diff.changedRatio < 0.003
                                            || diff.onlyDynamicChanged);
                                    if (structurallyQuiet && !actionLocalStructural) {
                                        ++consecutiveDynamicOnlyObserves;
                                        DeleteBitmapHandle(baseline);
                                        DeleteBitmapHandle(bmp);
                                        r.ok = true;
                                        r.unchanged = true;
                                        r.width = liveMap.apiWidth;
                                        r.height = liveMap.apiHeight;
                                        if (diff.onlyDynamicChanged || busy.busyCoverageRatio >= 0.05) {
                                            r.onlyDynamicChanged = true;
                                            r.settleHint =
                                                L"本地观察：仅动态区在变，控件区已稳定。"
                                                L"细则 section=agent。";
                                            if (consecutiveDynamicOnlyObserves >= 2) {
                                                r.settleHint +=
                                                    L" 已连续多次仅动态变化。";
                                            }
                                        }
                                        return r;
                                    }
                                    consecutiveDynamicOnlyObserves = 0;
                                    fillRois();
                                }
                                DeleteBitmapHandle(baseline);
                            }
                        }
                    }

                    consecutiveDynamicOnlyObserves = 0;
                    CommitSavedImage(bmp, kAiObsImageVarName, imageVarRunId, imageVars_);
                    const double scale = std::clamp(
                        eff.aiImageScale > 0.0 ? eff.aiImageScale : 0.5, 0.1, 1.0);
                    std::wstring imeStatus;
                    if (!wmUsesTarget()) {
                        imeStatus = QueryForegroundImeStatusText();
                        if (imeStatus.find(L"[输入法] 英文") != std::wstring::npos)
                            imeStatus.clear();
                    }
                    const AiImageEncodeResult enc = EncodeBitmapForAiAnalysis(
                        bmp, scale, 768, imeStatus.empty() ? nullptr : &imeStatus);
                    DeleteBitmapHandle(bmp);
                    if (enc.base64.empty()) return r;
                    r.ok = true;
                    r.unchanged = false;
                    r.base64 = enc.base64;
                    r.width = enc.outWidth;
                    r.height = enc.outHeight;
                    liveMap.capX1 = cx1;
                    liveMap.capY1 = cy1;
                    liveMap.capX2 = cx2;
                    liveMap.capY2 = cy2;
                    liveMap.srcWidth = enc.srcWidth;
                    liveMap.srcHeight = enc.srcHeight;
                    liveMap.apiWidth = enc.outWidth;
                    liveMap.apiHeight = enc.outHeight;
                    liveMapValid = liveMap.apiWidth > 0 && liveMap.capX2 > liveMap.capX1;
                    return r;
                };

                AiActionHostHooks agentHooks;
                agentHooks.onExecuteActions = [&](const std::wstring& actionsJson) {
                    return executeActionsJsonNow(actionsJson);
                };
                agentHooks.onObserveScreen = [&](bool forceRefresh) {
                    return observeScreenForAgent(forceRefresh);
                };
                agentHooks.onCaptureScreen = [&](std::string& b64, int& w, int& h) {
                    return captureObservationNow(b64, w, h);
                };
                agentHooks.onProbeDisabledSubmit = [](std::wstring& disabledName) {
                    return windowmode::FindDisabledSubmitButtonInForeground(disabledName);
                };
                agentHooks.onProbeForegroundDialog = []() {
                    return windowmode::FormatForegroundDialogProbe();
                };
                agentHooks.onQueryImeStatus = []() {
                    return QueryForegroundImeStatusText();
                };
                agentHooks.onListWindows = [&]() -> std::wstring {
                    // 台账要排除本软件自身窗口；枚举期间也顺手藏壳，避免壳窗抢 Z 序
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    return windowmode::FormatWindowList(windowmode::ListSwitchableWindows());
                };
                agentHooks.onActivateWindow = [&](const std::wstring& query) -> std::wstring {
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    // Alt 还按着时切窗会被预览吃掉，先落地
                    releaseAltTabIfHeld();
                    const auto all = windowmode::ListSwitchableWindows();
                    const auto hits = windowmode::MatchWindows(all, query);
                    if (hits.empty()) {
                        return L"[错误] 没有标题/进程名包含「" + query + L"」的窗口。"
                            L"当前窗口（Z 序）：\n" + windowmode::FormatWindowList(all)
                            + L"\n换个关键词再调；确实没开就用 runProgram/openFile 打开。";
                    }
                    // 多候选一律拒绝自动切：模糊 match（edge/excel）极易切错窗
                    if (hits.size() > 1) {
                        return L"[错误] activateWindow「" + query + L"」匹配到 "
                            + std::to_wstring(hits.size())
                            + L" 个窗口，拒绝自动选择以免切错。"
                            L"请把 match 改成能唯一锁定的标题关键词（如「历史记录」「浏览记录.xlsx」），"
                            L"不要只写进程名 edge/excel/msedge。\n候选：\n"
                            + windowmode::FormatWindowList(hits);
                    }
                    const auto& target = hits.front();
                    std::wstring error;
                    const bool ok = windowmode::ActivateWindow(target.hwnd, error);
                    AppendAiDebugLog(L"  [诊断] activateWindow「" + query + L"」→ "
                        + (ok ? L"已切到：" + target.title : L"失败：" + error));
                    if (!ok) {
                        return L"[错误] 切窗失败：" + error
                            + L"。可改用 switchWindow(action=openPreview, force=true) 兜底。";
                    }
                    if (AiLogicConvertSessionActive())
                        AiLogicConvertNoteWindowActivate(query);
                    std::wstring out = L"已切到前台：" + target.title;
                    if (!target.processName.empty()) out += L" [" + target.processName + L"]";
                    return out;
                };
                agentHooks.onActivateByProcess = [&](const std::wstring& processName) -> std::wstring {
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    releaseAltTabIfHeld();
                    windowmode::SwitchableWindow hit;
                    std::wstring error;
                    // 刚 ShellExecute 完窗口可能尚未进 Alt+Tab 列表，短轮询
                    bool ok = false;
                    for (int i = 0; i < 8; ++i) {
                        if (i > 0) Sleep(150);
                        ok = windowmode::ActivateByProcessName(processName, &hit, error);
                        if (ok) break;
                    }
                    AppendAiDebugLog(L"  [诊断] activateByProcess「" + processName + L"」→ "
                        + (ok ? L"已切到：" + hit.title : L"失败：" + error));
                    if (!ok) {
                        return L"[错误] 按进程激活失败：" + error;
                    }
                    if (AiLogicConvertSessionActive() && !hit.title.empty())
                        AiLogicConvertNoteWindowActivate(hit.title);
                    std::wstring out = L"已按进程切到前台：" + hit.title;
                    if (!hit.processName.empty()) out += L" [" + hit.processName + L"]";
                    return out;
                };
                agentHooks.onLocateAndClick = [&](const std::wstring& targetDesc,
                    int refineLevels, const std::wstring& button,
                    int clickCount) -> std::wstring {
                    LocateAndClickNestGuard locateGuard;
                    if (!locateGuard.entered()) {
                        return L"[错误] locateAndClick 不可嵌套（防无限外包定位）。";
                    }
                    // 整段定位+点击期间藏壳/调试窗（含 Zoom 二次截屏）
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    std::string b64;
                    int aw = 0, ah = 0;
                    // 定位长边 960 + 满分辨率（不被 aiImageScale=0.5 再压一半）：
                    // 小控件/输入框在整屏缩略后仍可辨，识图请求体也保持可控
                    if (!captureObservationNow(b64, aw, ah, 960, 1.0) || b64.empty())
                        return L"[错误] locateAndClick 截屏失败";
                    if (!liveMapValid) {
                        return L"[错误] locateAndClick 无有效截图映射";
                    }
                    AppendAiDebugLog(L"  [诊断] locateAndClick freshCore targetLen="
                        + std::to_wstring(targetDesc.size()));
                    const int timeoutMs = ResolveAiLocateVisionTimeoutSec(
                        eff.aiTimeoutSec) * 1000;
                    // 视觉定位子任务：主模型（用户所选）非多模态时，
                    // 自动路由到 savedModels 中已保存的多模态模型。
                    std::wstring locateModel = effModel;
                    const std::wstring visionModel = ResolveVisionSubtaskModelName(
                        appSettings_.ai, effModel);
                    if (!visionModel.empty() && visionModel != effModel) {
                        locateModel = visionModel;
                        AppendAiDebugLog(L"  [诊断] 主模型「" + effModel
                            + L"」非多模态，识图定位改用「" + visionModel + L"」");
                    } else if (visionModel.empty() && !ModelSupportsVision(effModel)) {
                        return L"[错误] " + MissingVisionModelError(effModel);
                    }
                    auto visionCore = CreateAiActionCore(
                        locateModel, appSettings_.ai.savedModels,
                        appSettings_.ai.apiUrl, appSettings_.ai.apiKey,
                        BuildAiActionVisionQuerySystemPrompt(aw, ah),
                        timeoutMs);
                    if (!visionCore) return L"[错误] 无法创建识图客户端";
                    ZoomRefineLocateOptions zopts;
                    zopts.maxLevels = std::clamp(refineLevels > 0 ? refineLevels : 1, 1, 2);
                    zopts.adaptiveRefineDepth = true;
                    if (zopts.maxLevels > 1) {
                        AppendAiDebugLog(L"  [诊断] locateAndClick refineLevels="
                            + std::to_wstring(zopts.maxLevels)
                            + L"（自适应：紧凑粗框可跳过二级）");
                    }
                    const ZoomRefineLocateResult zr = ExecuteZoomRefineLocate(
                        visionCore.get(), targetDesc, b64, aw, ah, liveMap,
                        stopFlag_,
                        [this](const std::wstring& line) { AppendAiDebugLog(line); },
                        &aiHttpAbort_, zopts);
                    if (!zr.ok) {
                        const std::wstring detail = zr.errorMessage.empty()
                            ? L"定位失败" : zr.errorMessage;
                        return L"[错误] " + detail
                            + L"。换短标签再 locate 最多1次，或看图换策略 / completeTask。";
                    }
                    if (zr.skippedRefine) {
                        AppendAiDebugLog(L"  [诊断] locateAndClick 自适应跳过二级 refine");
                    }
                    // 点击前查 UIA：控件灰掉说明前置条件没满足，点它只会白烧轮次
                    const windowmode::UiElementState uiState =
                        windowmode::ProbeUiElementAtPoint(zr.screenX, zr.screenY);
                    if (uiState.probed && !uiState.enabled) {
                        AppendAiDebugLog(L"  [诊断] UIA：目标不可用（禁用），已拦截点击 name="
                            + uiState.name);
                        std::wstring why = L"[错误] 目标「" + targetDesc + L"」当前是灰色不可用状态";
                        if (!uiState.name.empty()) why += L"（控件名：" + uiState.name + L"）";
                        why += L"，点击已被拦截。灰掉=前置条件没满足，不是位置点错，"
                               L"重复点/换描述都没用。请回头检查本步之前的输入是否合法或有必填项没填："
                               L"例如文件名框里不能出现 \\ / : * ? \" < > |（要改目录得用目录选择器，"
                               L"不是把路径塞进文件名）、必选项没选、内容为空。"
                               L"先修正输入，再重新提交。";
                        if (const std::wstring memo = uiState.name; !memo.empty())
                            AppendAiTaskMemoLine(L"blocked: disabled " + memo);
                        return why;
                    }
                    const bool isDouble = clickCount >= 2;
                    const bool isLeftSingle = (button != L"right") && !isDouble;
                    // 双击/右键是「换手法再试」，不算重复盲点
                    if (const std::wstring dup =
                            notePointerClick(zr.screenX, zr.screenY, isLeftSingle);
                        !dup.empty()) {
                        return dup;
                    }
                    int br = 0, bg = 0, bb = 0;
                    const bool hadBefore = GetScreenPixelRgb(zr.screenX, zr.screenY, br, bg, bb);
                    const std::wstring clickJson = BuildScreenClickActionsJson(
                        zr.screenX, zr.screenY, false, button, clickCount);
                    // 逻辑转化模板必须在点击前截取：点击会把按钮/菜单/页面切换到新状态，
                    // 点击后截到的模板是「后置状态」，下次运行时门闩 findImage 会找不到它，
                    // 快路径永远失效、每轮都烧 AI。先移开指针再截，模板=点击前界面=门闩要找的状态。
                    parkCursorAwayFromUi();
                    if (AiLogicConvertSessionActive()) {
                        // 藏起壳/宏调试窗再裁模板，避免裁进调试信息窗口
                        qst::desktop_tools::ScopedHideOwnUiForCapture hideForLogicTmpl(
                            UserFacingMainHwnd());
                        const std::wstring tmpl = CaptureAiLogicConvertTemplateAt(
                            zr.screenX, zr.screenY);
                        if (!tmpl.empty()) {
                            AiLogicConvertNoteLocate(targetDesc, zr.screenX, zr.screenY,
                                button, clickCount, tmpl);
                        }
                    }
                    const std::wstring execMsg = executeActionsJsonNow(clickJson);
                    std::wstring out = L"locateAndClick 已"
                        + std::wstring(isDouble ? L"双击" : (button == L"right" ? L"右键点击" : L"点击"))
                        + L"屏幕("
                        + std::to_wstring(zr.screenX) + L"," + std::to_wstring(zr.screenY)
                        + L") 识图轮次=" + std::to_wstring(zr.levelsUsed);
                    if (zr.usedFindImageSnap) {
                        out += L"；找图精修"
                            + std::to_wstring(static_cast<int>(zr.findImageScore + 0.5)) + L"%";
                    }
                    if (hadBefore) {
                        if (VerifyClickEffectByColorSample(zr.screenX, zr.screenY, br, bg, bb, 12))
                            out += L"；采样色已变";
                        else
                            out += L"；采样色接近";
                    }
                    out += L"；已移开指针防 hover";
                    AppendAiTaskMemoLine(L"done: locateAndClick");
                    if (!execMsg.empty()) out += L"；" + execMsg;
                    return out;
                };

                agentHooks.onSwitchWindow = [&](const std::wstring& paramsJson) -> std::wstring {
                    nlohmann::json params;
                    try {
                        params = nlohmann::json::parse(ToUtf8(paramsJson));
                    } catch (...) {
                        return L"[错误] switchWindow 参数 JSON 无效";
                    }
                    if (!params.is_object()) params = nlohmann::json::object();
                    std::string action;
                    if (params.contains("action") && params["action"].is_string())
                        action = params["action"].get<std::string>();
                    for (auto& c : action) {
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    }

                    auto tapKey = [&](UINT vk) {
                        SendKeyboardKey(vk, true);
                        Sleep(25);
                        SendKeyboardKey(vk, false);
                    };

                    bool wantConfirm = false;
                    if (params.contains("confirm")) {
                        const auto& cf = params["confirm"];
                        if (cf.is_boolean()) wantConfirm = cf.get<bool>();
                        else if (cf.is_number_integer()) wantConfirm = cf.get<int>() != 0;
                    }
                    // 松开 Alt 才会真正跳到选中窗口；这里是唯一的「落地」动作
                    auto finishSwitch = [&]() {
                        MarkSimulatedInput();
                        SendKeyboardKey(VK_LMENU, false);
                        UnmarkSimulatedInput();
                        altTabAltHeld = false;
                        altTabHoldRounds = 0;
                        Sleep(120);
                        forceNextObserveUpload = true;
                        AppendAiDebugLog(L"  [诊断] switchWindow：已松开 Alt，切换落地");
                    };

                    if (action == "openpreview" || action == "open") {
                        if (altTabAltHeld) releaseAltTabIfHeld();
                        // 按住 Alt，轻点 Tab，保持 Alt → 出现窗口预览
                        MarkSimulatedInput();
                        SendKeyboardKey(VK_LMENU, true);
                        altTabAltHeld = true;
                        Sleep(40);
                        tapKey(VK_TAB);
                        UnmarkSimulatedInput();
                        Sleep(180); // 等切换器动画
                        altTabHoldRounds = 1;
                        forceNextObserveUpload = true;
                        AppendAiDebugLog(L"  [诊断] switchWindow openPreview：Alt 已按住，预览应已出现");
                        return L"[EXECUTED][OBSERVE]\nswitchWindow openPreview：已按住 Alt 并点 Tab。"
                            L"看蓝框选中项与目标窗口差几格，然后一步收尾："
                            L"move(steps=差值, direction, confirm=true) —— confirm=true 会在移动后立刻"
                            L"松开 Alt 落地，不用再单独调 confirm（Alt 悬着会挡屏幕）。"
                            L"优先仍可用 activateWindow(match=标题或进程名)。";
                    }

                    if (action == "move") {
                        if (!altTabAltHeld) {
                            return L"[错误] 尚未 openPreview（Alt 未按住）。请先 switchWindow(openPreview)。";
                        }
                        int steps = 1;
                        if (params.contains("steps") && params["steps"].is_number_integer())
                            steps = params["steps"].get<int>();
                        steps = std::clamp(steps, 1, 40);
                        std::string dir = "right";
                        if (params.contains("direction") && params["direction"].is_string())
                            dir = params["direction"].get<std::string>();
                        for (auto& c : dir) {
                            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                        }
                        UINT vk = VK_RIGHT;
                        if (dir == "left") vk = VK_LEFT;
                        else if (dir == "tab") vk = VK_TAB;
                        MarkSimulatedInput();
                        for (int i = 0; i < steps && !StopRequested(); ++i) {
                            // Alt 保持按下，仅点方向键/Tab；间隔防吞键
                            tapKey(vk);
                            Sleep(45);
                        }
                        UnmarkSimulatedInput();
                        if (StopRequested()) {
                            releaseAltTabIfHeld();
                            return L"[错误] 用户取消";
                        }
                        if (wantConfirm) {
                            finishSwitch();
                            return L"[EXECUTED][OBSERVE]\nswitchWindow move×"
                                + std::to_wstring(steps)
                                + L" 后已松开 Alt，切换落地。请看图确认是否到了目标窗口；"
                                  L"不对就用 activateWindow(match=标题或进程名) 直接唤窗。";
                        }
                        ++altTabHoldRounds;
                        forceNextObserveUpload = true;
                        AppendAiDebugLog(L"  [诊断] switchWindow move×" + std::to_wstring(steps)
                            + L" (" + FromUtf8(dir) + L")，Alt 仍按住 第"
                            + std::to_wstring(altTabHoldRounds) + L" 轮");
                        // Alt 悬太多轮：预览一直盖着屏幕，强制落地免得卡死
                        if (altTabHoldRounds >= 4) {
                            finishSwitch();
                            return L"[EXECUTED][OBSERVE]\nswitchWindow move×"
                                + std::to_wstring(steps)
                                + L"：预览已开了太多轮，宿主已强制松开 Alt 落地。"
                                  L"请看图确认当前窗口；仍不对请改用 activateWindow(match=…)，"
                                  L"别再反复开预览。";
                        }
                        return L"[EXECUTED][OBSERVE]\nswitchWindow move×" + std::to_wstring(steps)
                            + L"：蓝框已在目标窗就 confirm 松开 Alt（不松开不会切过去）；"
                              L"否则继续 move 或 cancel。下次可直接 move(confirm=true) 一步完成。";
                    }

                    if (action == "confirm" || action == "ok") {
                        if (!altTabAltHeld) {
                            return L"[错误] 没有打开的 Alt+Tab 预览可确认。";
                        }
                        finishSwitch();
                        return L"[EXECUTED][OBSERVE]\nswitchWindow confirm：已松开 Alt，已切换到选中窗口。";
                    }

                    if (action == "cancel" || action == "close") {
                        MarkSimulatedInput();
                        if (altTabAltHeld) {
                            tapKey(VK_ESCAPE);
                            Sleep(30);
                            SendKeyboardKey(VK_LMENU, false);
                            altTabAltHeld = false;
                            altTabHoldRounds = 0;
                        }
                        UnmarkSimulatedInput();
                        forceNextObserveUpload = true;
                        return L"[EXECUTED][OBSERVE]\nswitchWindow cancel：已取消切换。"
                            L"可改 activateWindow(match=…)，或确认窗口是否未打开。";
                    }

                    return L"[错误] switchWindow.action 须为 openPreview|move|confirm|cancel";
                };

                // aiMaxSteps=-1：步骤与 Agent 轮次均不人为封死（轮次仅留安全上限防死循环）
                // aiMaxSteps>0：轮次约 2×步数，避免「搜到结果就断」
                const int agentRounds = (eff.aiMaxSteps < 0)
                    ? -1
                    : ((eff.aiMaxSteps > 0)
                        ? std::clamp(std::max(eff.aiMaxSteps * 2, 10), 4, 40)
                        : 10);

                auto handleAiActionApiResult = [&](const AiActionResult& ar) {
                    // 用户热键终止：若 AI 已成功且有可固化轨迹，仍尝试写回再退出
                    if (StopRequested()) {
                        releaseAltTabIfHeld();
                        if (AiLogicConvertSessionActive()
                            && (AiLogicConvertPendingWriteback()
                                || AiLogicConvertShouldWriteback(ar.ok, ar.actionsAlreadyExecuted,
                                    ar.anyActionsExecuted, ar.completeReason, ar.textResult))) {
                            AiLogicConvertMarkPendingWriteback(true);
                            if (flushLogicConvertWriteback(L"热键终止"))
                                AiLogicConvertSessionEnd();
                        }
                        return;
                    }
                    if (!ar.ok) {
                        releaseAltTabIfHeld();
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：API 调用失败"
                            + (ar.errorMessage.empty() ? L"" : L" (" + ar.errorMessage + L")"));
                        return;
                    }
                    if (ar.visionQueryText) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]："
                            + AiActionRouteLabel(ar.routeKind) + L" → "
                            + Trim(ar.textResult));
                        return;
                    }
                    if (ar.actionsAlreadyExecuted) {
                        releaseAltTabIfHeld();
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：Agent 闭环结束 → "
                            + Trim(ar.textResult));
                        if (AiLogicConvertSessionActive()) {
                            if (!AiLogicConvertShouldWriteback(ar.ok, ar.actionsAlreadyExecuted,
                                    ar.anyActionsExecuted, ar.completeReason, ar.textResult)) {
                                AppendAiDebugLog(
                                    L"逻辑转化：任务未成功或无可固化轨迹，跳过写回");
                                AiLogicConvertSessionEnd();
                            } else {
                                AiLogicConvertMarkPendingWriteback(true);
                                if (flushLogicConvertWriteback(L"AI段结束")) {
                                    AiLogicConvertSessionEnd();
                                } else {
                                    // 路径暂缺：保留会话，等本 AI 步收尾 / 脚本结束再刷
                                    AppendAiDebugLog(
                                        L"逻辑转化：写回推迟到脚本结束或热键终止时重试");
                                }
                            }
                        }
                        return;
                    }
                    executeActionsJsonNow(ar.textResult);
                    releaseAltTabIfHeld();
                };

                if (effModel.empty() || !appSettings_.ai.enabled) {
                    AppendAiDebugLog(L"AI动作执行：未配置模型或 AI 未启用");
                    aiCurFrame = prevFrame;
                    return;
                }

                AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };

                // 首轮截图策略：勾选「带截图」→ 必截；未勾选则仅当 prompt 明显要看屏（点击/定位）才按需截。
                // 纯变量分析+按键等不截；运行中 locateAndClick / observe 仍可按需截屏。勿嵌套 aiActionExecute。
                auto runWithOptionalAutoCapture = [&](bool preferCapture) {
                    const bool wantCapture = preferCapture || eff.aiWithImage;
                    int capX1 = 0, capY1 = 0, capX2 = 0, capY2 = 0;
                    std::string screenshotB64;
                    int apiW = 0, apiH = 0;
                    AiCaptureMapping capMap{};
                    bool haveImage = false;

                    if (wantCapture && resolveAiRegion(capX1, capY1, capX2, capY2)) {
                        // 首轮截图同样隐藏本软件窗口（主界面+调试窗），避免日志滚动进图
                        qst::desktop_tools::ScopedHideOwnUiForCapture hideOwnUi(
                            UserFacingMainHwnd());
                        HBITMAP screenBmp = nullptr;
                        if (wmUsesTarget()) {
                            screenBmp = wmExecPtr->CaptureScreenRegionFromWindow(
                                capX1, capY1, capX2, capY2,
                                lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            screenBmp = CaptureAiRegionComposed(capX1, capY1, capX2, capY2);
                        }
                        int sw = 0, sh = 0;
                        if (screenBmp) {
                            BITMAP bm{};
                            if (GetObject(screenBmp, sizeof(bm), &bm)) {
                                sw = bm.bmWidth;
                                sh = bm.bmHeight;
                            }
                        }
                        if (screenBmp && sw > 0 && sh > 0) {
                            if (!eff.aiWithImage) {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel
                                    + L"]：未勾选带截图，但任务需看屏，已按需截取观察区("
                                    + std::to_wstring(sw) + L"×" + std::to_wstring(sh)
                                    + L")");
                            } else {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：截屏完成("
                                    + std::to_wstring(sw) + L"×" + std::to_wstring(sh)
                                    + L")，发送中…");
                            }
                            const double scale = std::clamp(
                                eff.aiImageScale > 0.0 ? eff.aiImageScale : 0.5, 0.1, 1.0);
                            // 需看屏的首轮（本地定位点击）用更高长边，避免整屏缩略后小控件不可辨
                            const int firstLongEdge = preferCapture ? 1280 : 1024;
                            std::wstring imeStatus;
                            if (!wmUsesTarget()) {
                                imeStatus = QueryForegroundImeStatusText();
                                if (imeStatus.find(L"[输入法] 英文") != std::wstring::npos)
                                    imeStatus.clear();
                            }
                            const AiImageEncodeResult enc = EncodeBitmapForAiAnalysis(
                                screenBmp, scale, firstLongEdge,
                                imeStatus.empty() ? nullptr : &imeStatus);
                            CommitSavedImage(screenBmp, kAiObsImageVarName, imageVarRunId, imageVars_);
                            DeleteBitmapHandle(screenBmp);
                            if (!enc.base64.empty()) {
                                AppendAiDebugLog(L"  图片 " + std::to_wstring(enc.srcWidth) + L"×"
                                    + std::to_wstring(enc.srcHeight) + L" → 上传 "
                                    + std::to_wstring(enc.outWidth) + L"×" + std::to_wstring(enc.outHeight)
                                    + L"（" + std::to_wstring(enc.base64.size()) + L" 字节 base64）");
                                screenshotB64 = enc.base64;
                                apiW = enc.outWidth;
                                apiH = enc.outHeight;
                                capMap.capX1 = capX1;
                                capMap.capY1 = capY1;
                                capMap.capX2 = capX2;
                                capMap.capY2 = capY2;
                                capMap.srcWidth = enc.srcWidth;
                                capMap.srcHeight = enc.srcHeight;
                                capMap.apiWidth = apiW;
                                capMap.apiHeight = apiH;
                                liveMap = capMap;
                                liveMapValid = capMap.apiWidth > 0 && capMap.capX2 > capMap.capX1;
                                haveImage = true;
                            } else {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：图片编码失败");
                            }
                        } else {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：截屏失败"
                                + (eff.aiWithImage ? L"" : L"（将尝试无图工具路径）"));
                            if (screenBmp) DeleteBitmapHandle(screenBmp);
                        }
                    } else if (wantCapture) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法定位分析区域"
                            + (eff.aiWithImage ? L"" : L"（将尝试无图工具路径）"));
                    }

                    try {
                        if (!haveImage) {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：发送 prompt（无截图）…");
                            const AiActionRouteKind route = ClassifyAiActionRoute(resolvedPrompt, false);
                            const int timeoutMs = ResolveAiActionExecuteTimeoutSec(
                                eff.aiTimeoutSec, false) * 1000;
                            AgentCore* corePtr = nullptr;
                            std::unique_ptr<AgentCore> ownedCore = PrepareAiActionExecuteCore(
                                &aiSessions, prepAction, aiLoopDepth, route, 0, 0,
                                appSettings_, timeoutMs, corePtr);
                            AgentCore* core = corePtr ? corePtr : ownedCore.get();
                            if (!core) {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法创建 AI 客户端");
                                return;
                            }
                            // heal 回退会话保留 memo/计划门闩，避免每轮漂移再烧 updateTaskMemo
                            if (AiActionExecuteNestDepth() <= 1
                                && !AiLogicConvertSessionIsHeal())
                                ResetAiActionSessionState(imageVarRunId);
                            const size_t histBefore = core->GetHistory().size();
                            AiActionResult ar = ExecuteAiActionExecute(
                                core, resolvedPrompt, "", 0, 0, eff.aiContextMode,
                                stopFlag_, eff.aiTimeoutSec, logFn, &aiHttpAbort_, nullptr,
                                &agentHooks, agentRounds,
                                clipExtraJpeg.empty() ? nullptr : &clipExtraJpeg);
                            if (ar.ok) {
                                propagateAiHistory(core, prepAction, histBefore,
                                    BuildAiActionExecuteTextSystemPrompt(),
                                    route == AiActionRouteKind::ToolExecute
                                        || route == AiActionRouteKind::MultiTurnTools,
                                    1024, timeoutMs);
                            }
                            handleAiActionApiResult(ar);
                            return;
                        }

                        const AiActionRouteKind route = ClassifyAiActionRoute(resolvedPrompt, true);
                        const int effectiveTimeoutSec = ResolveAiActionExecuteTimeoutSec(
                            eff.aiTimeoutSec, true);
                        const int timeoutMs = effectiveTimeoutSec * 1000;
                        // 文本主模型 + 列表识图模型：
                        // · 工具 Agent：规划仍用文本模型；locateAndClick 单独切识图子模型
                        // · Vision/Composite：整段须识图模型
                        // · 列表无识图模型：终止本步（勿仅警告后继续烧 API）
                        ScriptAction routedAction = prepAction;
                        std::wstring execModel = routedAction.aiModelName;
                        const std::wstring vm = ResolveVisionSubtaskModelName(
                            appSettings_.ai, execModel);
                        const bool primaryVision = ModelSupportsVision(execModel);
                        const bool toolAgent = (route == AiActionRouteKind::ToolExecute
                            || route == AiActionRouteKind::MultiTurnTools);
                        if (!primaryVision && vm.empty()) {
                            AiActionResult ar;
                            ar.ok = false;
                            ar.routeKind = route;
                            ar.errorMessage = MissingVisionModelError(execModel);
                            AppendAiDebugLog(L"  [错误] " + ar.errorMessage);
                            handleAiActionApiResult(ar);
                            return;
                        }
                        if (!primaryVision && !vm.empty()) {
                            if (toolAgent) {
                                AppendAiDebugLog(L"  [诊断] 主模型「" + execModel
                                    + L"」非多模态：规划轮用文本模型；"
                                      L"locateAndClick 识图将改用「" + vm + L"」");
                            } else {
                                AppendAiDebugLog(L"  [诊断] 主模型「" + execModel
                                    + L"」非多模态，带图执行改用「" + vm + L"」");
                                execModel = vm;
                            }
                        }
                        routedAction.aiModelName = execModel;
                        AgentCore* corePtr = nullptr;
                        std::unique_ptr<AgentCore> ownedCore = PrepareAiActionExecuteCore(
                            &aiSessions, routedAction, aiLoopDepth, route, apiW, apiH,
                            appSettings_, timeoutMs, corePtr);
                        AgentCore* core = corePtr ? corePtr : ownedCore.get();
                        if (!core) {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法创建 AI 客户端");
                            return;
                        }
                        if (AiActionExecuteNestDepth() <= 1
                            && !AiLogicConvertSessionIsHeal())
                            ResetAiActionSessionState(imageVarRunId);
                        const size_t histBefore = core->GetHistory().size();
                        AiActionResult ar = ExecuteAiActionExecute(
                            core, resolvedPrompt, screenshotB64,
                            apiW, apiH, eff.aiContextMode,
                            stopFlag_, eff.aiTimeoutSec, logFn, &aiHttpAbort_, &capMap,
                            &agentHooks, agentRounds,
                            clipExtraJpeg.empty() ? nullptr : &clipExtraJpeg);
                        if (ar.ok) {
                            std::wstring sysPrompt;
                            if (route == AiActionRouteKind::VisionQuery
                                || route == AiActionRouteKind::CompositeClick) {
                                sysPrompt = BuildAiActionVisionQuerySystemPrompt(apiW, apiH);
                            } else if (route == AiActionRouteKind::MultiTurnTools) {
                                sysPrompt = BuildAiActionHybridSystemPrompt(apiW, apiH);
                            } else {
                                sysPrompt = BuildAiActionExecuteSystemPrompt(apiW, apiH);
                            }
                            propagateAiHistory(core, routedAction, histBefore, sysPrompt,
                                route == AiActionRouteKind::ToolExecute
                                    || route == AiActionRouteKind::MultiTurnTools,
                                1024, timeoutMs);
                        }
                        handleAiActionApiResult(ar);
                    } catch (...) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：执行异常");
                    }
                };

                const bool needScreen = AiActionPromptLikelyNeedsScreenCapture(resolvedPrompt);
                if (!eff.aiWithImage && !needScreen) {
                    AppendAiDebugLog(L"AI动作执行 [" + effModel
                        + L"]：未勾选带截图且任务无需看屏 → 首轮无图"
                        L"（需定位时再调 locateAndClick，宿主会按需截屏）");
                }
                runWithOptionalAutoCapture(needScreen);
                releaseAltTabIfHeld();
                if (logicConvertTop) {
                    // AI 步收尾：路径已齐则写回；仍失败则保留 pending，等脚本结束/热键终止再刷
                    if (AiLogicConvertPendingWriteback()) {
                        if (flushLogicConvertWriteback(L"AI步收尾"))
                            AiLogicConvertSessionEnd();
                    } else {
                        AiLogicConvertSessionEnd();
                    }
                }
                aiCurFrame = prevFrame;
            };

            // 精密键鼠回放（对齐 LLIR/FLOW 思路）：
            // 1) 绝对时间轴  2) 关闭加速使 SendInput 贴近 Raw 录制值  3) 提高线程优先级  4) 忙等收尾
            struct InputTimelineState {
                bool enabled = false;
                PrecisionInputTimeline precision;
                void Reset(size_t waitHint = 0) {
                    precision.Reset(waitHint);
                }
            };
            InputTimelineState inputTimeline;
            inputTimeline.enabled = IsRecordingScriptPath(selfPath)
                || ScriptIsTimedInputSequence(actions);
            // 全局倍速：录制目录直接播放，或带 QPC timingUs 的录制轨迹（含另存到宏目录）。
            // 手写鼠标宏没有 timingUs，顶层仍为 1。嵌套 mousePlayback 另推动作字段。
            // 编辑器调试不走 timingUs 兜底（调试 selfPath 常是显示名），避免误套设置倍速。
            double playbackTimeScale = 1.0;
            bool applyRecordingSpeed = IsRecordingScriptPath(selfPath);
            if (!applyRecordingSpeed && !debugMode_.load(std::memory_order_relaxed)) {
                for (const auto& a : actions) {
                    if (a.timingUs > 0) {
                        applyRecordingSpeed = true;
                        break;
                    }
                }
            }
            if (applyRecordingSpeed) {
                playbackTimeScale = quickscript::RecordingPlaybackTimeScale(appSettings_);
            }

            // 精密轴期间：调试行先写入内存，本轮结束后再批量刷窗。
            // 这样既不影响时序，用户仍可复制完整日志（含 late 统计）供分析。
            struct DeferPlaybackDebugUiGuard {
                EngineHost* self = nullptr;
                bool armed = false;
                DeferPlaybackDebugUiGuard(EngineHost* s, bool enable, size_t actionCount) : self(s) {
                    if (!s || !enable) return;
                    s->PrepareDeferredPlaybackDebug(actionCount);
                    s->deferPlaybackDebugUi_.store(true, std::memory_order_relaxed);
                    if (s->appSettings_.playback.autoOutputKeyFunctionDebug
                        && qst::desktop_tools::MacroDebug().IsCreated()) {
                        qst::desktop_tools::MacroDebug().AppendLog(
                            L"精密时间轴回放：调试逐步日志延后刷出，且有上限（避免长录制多轮把时间轴拖变形）");
                    }
                    armed = true;
                }
                ~DeferPlaybackDebugUiGuard() {
                    if (!armed || !self) return;
                    self->DisarmDeferredPlaybackDebugUi();
                    armed = false;
                }
                void DisarmNow() {
                    if (!armed || !self) return;
                    self->DisarmDeferredPlaybackDebugUi();
                    armed = false;
                }
            } deferDbgGuard(this, inputTimeline.enabled, actions.size());

            // 绝对时间轴开启时不再额外垫 SendInput 间隔：迟到限速会与追赶打架，加重跑次抖动。
            MouseInputRouter::Instance().SetCatchUpGapUs(0);
            // SPI/优先级/绑核必须在通知 UI 结束前回恢复：否则下一轮会和析构抢加速设置。
            {
            struct CatchUpGapResetGuard {
                ~CatchUpGapResetGuard() {
                    MouseInputRouter::Instance().SetCatchUpGapUs(0);
                }
            } catchUpGapResetGuard;
            MouseBallisticsGuard ballisticsGuard(inputTimeline.enabled
                || (wmExecPtr && wmExecPtr->PreferHardwareInput()));
            // 加速已关：保持 Raw 原包大小；未关才拆包防加倍（拆包会改变报告次数）。
            const bool splitLargeMoves =
                inputTimeline.enabled && !ballisticsGuard.FlatVerified();
            MouseInputRouter::Instance().SetSplitLargeMoves(splitLargeMoves);
            PlaybackProcessPriorityGuard processPriGuard(inputTimeline.enabled);
            PlaybackThreadAffinityGuard affinityGuard(inputTimeline.enabled);
            MultimediaTimerGuard timerPeriodGuard(inputTimeline.enabled);
            struct ThreadPriorityGuard {
                HANDLE thread = nullptr;
                int prev = THREAD_PRIORITY_NORMAL;
                bool active = false;
                explicit ThreadPriorityGuard(bool enable) {
                    if (!enable) return;
                    thread = GetCurrentThread();
                    prev = GetThreadPriority(thread);
                    active = true;
                    // 勿用 TIME_CRITICAL：会饿死 UI/LL 钩子，表现为停止热键失灵、键鼠假死
                    SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
                }
                ~ThreadPriorityGuard() {
                    if (active) SetThreadPriority(thread, prev);
                }
            } threadPriGuard(inputTimeline.enabled);

            bool timelineInterrupted = false;
            bool wmTargetLostLogged = false;
            auto wmAbortIfTargetLost = [this, wmExecPtr, &wmTargetLostLogged]() -> bool {
                if (!wmExecPtr || !wmExecPtr->IsActive()) return false;
                if (wmExecPtr->TargetStillAlive()) return false;
                if (!wmTargetLostLogged) {
                    wmTargetLostLogged = true;
                    windowmode::WindowModeLog(
                        L"[窗口模式] 目标窗口已消失（进程退出/闪退），停止脚本");
                    AppendDebugLog(L"窗口模式：目标已闪退或关闭，已停止（不会对失效窗口继续记步）");
                }
                stopFlag_.store(true, std::memory_order_relaxed);
                return true;
            };
            auto waitAbsoluteTimeline = [this, &inputTimeline, &timelineInterrupted, &wmAbortIfTargetLost, &playbackTimeScale](
                double waitSec, uint64_t timingUs = 0) {
                waitSec = quickscript::ScalePlaybackTimeSeconds(waitSec, playbackTimeScale);
                timingUs = quickscript::ScalePlaybackTimeUs(timingUs, playbackTimeScale);
                if (timingUs == 0 && waitSec <= 0.0) {
                    MouseInputRouter::Instance().NoteWaitLatenessUs(0);
                    return true;
                }
                const auto cancelled = [this, &wmAbortIfTargetLost] {
                    return StopRequested() || BreakoutTriggered() || wmAbortIfTargetLost();
                };
                // 绝对轴：与录制 QPC 戳对齐。间隙等待会叠 SendInput 开销，整体偏慢且更抖。
                const bool ok = (timingUs > 0)
                    ? inputTimeline.precision.WaitDeltaUs(timingUs, cancelled)
                    : inputTimeline.precision.WaitDeltaSeconds(waitSec, cancelled);
                MouseInputRouter::Instance().NoteWaitLatenessUs(
                    inputTimeline.precision.LastLatenessUs());
                if (!ok) timelineInterrupted = true;
                return ok;
            };

            int scheduledYieldDepth = 0;
            bool scheduledYieldLocalStop = false;
            executeOne = [this, &usesOcr, &holdOcrSession, &heldKeyVk, &heldKeys, &runRange, &runningScriptPath, &activeActions, &lockedScreen_, &lockedVirtX_, &lockedVirtY_, &clearLockedScreen, &makeVarCtx, &resolveTemplatePath, &executeOne, &runAiActionExecute, &aiSessions, &aiLoopDepth, &pendingBreakLoop, wmExecPtr, &wmSetPos, &wmSetLivePos, &wmSendKey, &wmSendHeldModifiers, &wmMouseButton, &wmMouseClick, &wmSendShortcut, &isImeToggleShortcut, &wmUsesTarget, &wmUsesBackground, &activeCoordMeta, &currentTmplScale, execTargetW, execTargetH, &inputTimeline, &waitAbsoluteTimeline, &wmAbortIfTargetLost, &playbackTimeScale, imageVarRunId, &scheduledYieldDepth, &scheduledYieldLocalStop](const ScriptAction& a) {
                if (StopRequested() || scheduledYieldLocalStop || wmAbortIfTargetLost()) return;
                executedSteps_.fetch_add(1, std::memory_order_relaxed);
                // 兼容未 Normalize 的旧内存对象：瞬时类仍可能带前延迟
                if (inputTimeline.enabled && a.randomDuration <= 1e-12) {
                    const bool durationIsInterval =
                        a.type == ActionType::Wait
                        || ActionUsesInterRepeatInterval(a.type);
                    if (!durationIsInterval && (a.timingUs > 0 || a.duration > 1e-9)) {
                        waitAbsoluteTimeline(a.duration, a.timingUs);
                        if (StopRequested() || wmAbortIfTargetLost()) return;
                    }
                }

                if (KeyFunctionDebugActive() && !DeferredDetailDebugDropped()
                    && a.type != ActionType::MoveMouse
                    && a.type != ActionType::MoveMouseRelative
                    && a.type != ActionType::Wait
                    && a.type != ActionType::KeyDown
                    && a.type != ActionType::KeyUp
                    && a.type != ActionType::FindImage
                    && a.type != ActionType::TextRecognition) {
                    AppendDebugLog(FormatGenericActionDebug(a));
                }
                if (a.type == ActionType::MoveMouse) {
                    int x = a.x;
                    int y = a.y;
                    if (a.moveFromVar) {
                        MacroVariableContext ctx = makeVarCtx();
                        if (!TryResolveIntOperand(a.moveVarExprX, ctx, x)) x = 0;
                        if (!TryResolveIntOperand(a.moveVarExprY, ctx, y)) y = 0;
                    }
                    if (KeyFunctionDebugActive()) {
                        AppendDeferredMoveAbsDebug(a, x, y);
                    }
                    // 精密轴只对「录制回放」禁用随机抖动（录制坐标本就是精确采样，
                    // 且录制时 randomX/randomY 已置 0）；手写宏（含时间轴输入序列脚本）
                    // 的 ±随机必须生效，否则每次移动都落在同一点。
                    const bool recordingReplay = IsRecordingScriptPath(runningScriptPath);
                    const int rx = recordingReplay ? 0 : a.randomX;
                    const int ry = recordingReplay ? 0 : a.randomY;
                    wmSetPos(x, y, rx, ry);
                }
                else if (a.type == ActionType::MoveMouseRelative) {
                    const bool recordingReplay = IsRecordingScriptPath(runningScriptPath);
                    const int dx = a.x + (recordingReplay ? 0 : RandomInt(a.randomX));
                    const int dy = a.y + (recordingReplay ? 0 : RandomInt(a.randomY));
                    if (KeyFunctionDebugActive()) {
                        AppendDeferredMoveRelDebug(a, dx, dy);
                    }
                    if (wmUsesTarget()) {
                        const bool hw = wmExecPtr->PreferHardwareInput();
                        if (hw) MarkSimulatedInput();
                        wmExecPtr->MoveMouseRelativeClient(dx, dy);
                        if (hw) UnmarkSimulatedInput();
                    } else {
                        MarkSimulatedInput();
                        SendMouseMoveRelative(dx, dy);
                        UnmarkSimulatedInput();
                    }
                }
                else if (a.type == ActionType::Wait) {
                    if (a.randomDuration > 1e-12 || !inputTimeline.enabled) {
                        const double waitSec = quickscript::ScalePlaybackTimeSeconds(
                            a.duration + RandomDelay(a.randomDuration), playbackTimeScale);
                        if (waitSec > 0.0) {
                            SleepInterruptible(waitSec);
                            if (inputTimeline.enabled) inputTimeline.Reset();
                        }
                    } else if (a.timingUs > 0) {
                        waitAbsoluteTimeline(0.0, a.timingUs);
                    } else if (a.duration > 0.0) {
                        waitAbsoluteTimeline(a.duration, 0);
                    }
                    if (KeyFunctionDebugActive()) {
                        AppendDeferredWaitDebug(a,
                            inputTimeline.enabled
                                ? inputTimeline.precision.LastLatenessUs() : 0,
                            playbackTimeScale);
                    }
                }
                else if (a.type == ActionType::MouseDown) {
                    MarkSimulatedInput();
                    wmSendHeldModifiers(a, true);
                    wmMouseButton(a.x, a.y, a.button, true);
                    UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::MouseUp) {
                    MarkSimulatedInput();
                    wmMouseButton(a.x, a.y, a.button, false);
                    wmSendHeldModifiers(a, false);
                    UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::MouseClick) for (int i = 0; i < a.clickCount && !StopRequested(); ++i) {
                    int cx = a.x;
                    int cy = a.y;
                    if (a.x != 0 || a.y != 0) {
                        wmSetPos(a.x, a.y, a.randomX, a.randomY);
                        cx = a.x + RandomInt(a.randomX);
                        cy = a.y + RandomInt(a.randomY);
                    }
                    // 窗口模式 CDP/扩展键鼠不走本机 SendInput，勿 Mark（否则脱离检测会误判忙碌）。
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    wmSendHeldModifiers(a, true);
                    wmMouseClick(cx, cy, a.button);
                    wmSendHeldModifiers(a, false);
                    if (markSim) UnmarkSimulatedInput();
                    // duration=两次重复之间的间隔；执行 1 次时不等待，首前/末后也不插等待
                    if (ShouldWaitAfterRepeat(a, i) && !StopRequested()) {
                        SleepInterruptible(quickscript::ScalePlaybackTimeSeconds(
                            a.duration + RandomDelay(a.randomDuration), playbackTimeScale));
                    }
                }
                else if (a.type == ActionType::KeyDown) {
                    // 已按住再 KeyDown = 录制自动重复；精密回放跳过注入，避免向游戏灌重复 KEYDOWN
                    if (inputTimeline.enabled && heldKeys.count(a.keyVk)) {
                        if (KeyFunctionDebugActive() && !DeferredDetailDebugDropped()) {
                            AppendDebugLog(FormatGenericActionDebug(a) + L" (已按住，跳过)");
                        }
                    } else {
                        if (KeyFunctionDebugActive() && !DeferredDetailDebugDropped()) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        const bool markSim = !wmUsesTarget();
                        if (markSim) MarkSimulatedInput();
                        wmSendHeldModifiers(a, true);
                        wmSendKey(a.keyVk, true);
                        heldKeyVk = a.keyVk;
                        heldKeys.insert(a.keyVk);
                        if (markSim) UnmarkSimulatedInput();
                    }
                }
                else if (a.type == ActionType::KeyUp) {
                    if (KeyFunctionDebugActive() && !DeferredDetailDebugDropped()) {
                        AppendDebugLog(FormatGenericActionDebug(a));
                    }
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    wmSendKey(a.keyVk, false);
                    if (heldKeyVk == a.keyVk) heldKeyVk = 0;
                    heldKeys.erase(a.keyVk);
                    wmSendHeldModifiers(a, false);
                    if (markSim) UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::KeyClick) for (int i = 0; i < a.clickCount && !StopRequested(); ++i) {
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    // 表格导航键/ASCII 键前先切英文 IME：中文组合窗会把 Enter/Tab/Home 吃掉
                    // 软投递后台窗口不经系统 IME，勿改用户前台输入法。
                    if (!wmUsesTarget() || (wmExecPtr && wmExecPtr->PreferHardwareInput())) {
                        if (!isImeToggleShortcut(a)) ForceEnglishImeBeforeAsciiKey();
                    }
                    // 按键点击 = 真实物理按键：必须发 KEYDOWN/KEYUP（scan code），
                    // 不能用 KEYEVENTF_UNICODE——游戏/DirectInput/轮询键盘状态的应用
                    // 收不到 Unicode 事件，导致「按键点击失效」（鼠标正常、按键无反应）。
                    // 文字输入请用 quickInput（走 Unicode，天然绕开中文 IME）。
                    wmSendHeldModifiers(a, true);
                    wmSendKey(a.keyVk, true);
                    wmSendKey(a.keyVk, false);
                    wmSendHeldModifiers(a, false);
                    if (markSim) UnmarkSimulatedInput();
                    if (ShouldWaitAfterRepeat(a, i) && !StopRequested()) {
                        SleepInterruptible(quickscript::ScalePlaybackTimeSeconds(
                            a.duration + RandomDelay(a.randomDuration), playbackTimeScale));
                    }
                }
                else if (a.type == ActionType::HotkeyShortcut) for (int i = 0; i < a.clickCount && !StopRequested(); ++i) {
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    if (!wmUsesTarget() || (wmExecPtr && wmExecPtr->PreferHardwareInput())) {
                        if (!isImeToggleShortcut(a)) ForceEnglishImeBeforeAsciiKey();
                    }
                    wmSendShortcut(a);
                    if (markSim) UnmarkSimulatedInput();
                    if (ShouldWaitAfterRepeat(a, i) && !StopRequested()) {
                        SleepInterruptible(quickscript::ScalePlaybackTimeSeconds(
                            a.duration + RandomDelay(a.randomDuration), playbackTimeScale));
                    }
                }
                else if (a.type == ActionType::QuickInput) for (int i = 0; i < a.clickCount && !StopRequested(); ++i) {
                    // 不在整段输入期间持有 MarkSimulatedInput：否则热键停止会被 ShouldIgnoreHotkeyStop 挡住。
                    // 注入键由 LL 钩子过滤 LLKHF_INJECTED；字间轮询 stopFlag_
                    // （紧急停会经 ghWorkerCancelFlag 一并置位）以便立即终止。
                    const std::wstring text = ResolveQuickInputText(a.inputText, a.parseEscapes);
                    if (text.empty() && !Trim(a.inputText).empty()) {
                        AppendDebugLog(L"[警告] quickInput 文本解析后为空（原文本："
                            + a.inputText
        + L"）。变量请使用 {var} / {var.属性} / {time:格式} / {Now}。");
                    }
                    if (wmExecPtr && wmExecPtr->IsActive()) {
                        // 窗口模式 soft/CDP 输入直接投递给目标窗口，不经系统 IME，无需准备
                        wmExecPtr->SendQuickInputToTarget(text,
                            quickscript::ScalePlaybackTimeSeconds(a.charInterval, playbackTimeScale));
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            wchar_t buf[160]{};
                            swprintf_s(buf, L"快捷输入→目标窗口 hwnd=0x%p%s",
                                wmExecPtr->TargetHwnd(),
                                wmUsesBackground() ? L" [后台窗口模式]" : L" [窗口模式]");
                            AppendDebugLog(buf);
                        }
                    } else {
                        // 桌面模式：先清挂起组字（TSF 输入法 ImmSetConversionStatus 常不生效，
                        // 残留拼音会把数字/字母拼进组字串，如「1」→「h1」），含 ASCII 再切英文
                        PrepareImeForTextInput(text);
                        SendQuickInputText(text,
                            quickscript::ScalePlaybackTimeSeconds(a.charInterval, playbackTimeScale),
                            &stopFlag_);
                    }
                    if (ShouldWaitAfterRepeat(a, i) && !StopRequested()) {
                        SleepInterruptible(quickscript::ScalePlaybackTimeSeconds(
                            a.duration + RandomDelay(a.randomDuration), playbackTimeScale));
                    }
                }
                else if (a.type == ActionType::ScrollWheel) for (int i = 0; i < a.clickCount && !StopRequested(); ++i) {
                    MarkSimulatedInput();
                    const bool positive = a.scrollDirection == 0;
                    if (wmUsesTarget()) {
                        if (a.scrollVertical) {
                            wmExecPtr->PostScrollWheelAtClient(a.x, a.y, a.scrollSteps, true, positive);
                        }
                        if (a.scrollHorizontal) {
                            wmExecPtr->PostScrollWheelAtClient(a.x, a.y, a.scrollSteps, false, positive);
                        }
                    } else {
                        const int delta = (positive ? 1 : -1) * WHEEL_DELTA;
                        for (int step = 0; step < a.scrollSteps; ++step) {
                            if (a.scrollVertical)
                                MouseInputRouter::Instance().Wheel(delta, false);
                            if (a.scrollHorizontal)
                                MouseInputRouter::Instance().Wheel(delta, true);
                        }
                    }
                    UnmarkSimulatedInput();
                    if (ShouldWaitAfterRepeat(a, i) && !StopRequested()) {
                        SleepInterruptible(quickscript::ScalePlaybackTimeSeconds(
                            a.duration + RandomDelay(a.randomDuration), playbackTimeScale));
                    }
                }
                else if (a.type == ActionType::FindImage) {
                    // 找图/保存图片时隐藏本软件窗口（主界面+调试输出窗）：
                    // 否则日志滚动会被当成界面变化、整屏截进图片变量（误匹配 + 烧 API）。
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwnUi(
                        UserFacingMainHwnd());
                    const TemplateScale findTmplScale = currentTmplScale();
                    ScriptAction findAct = a;
                    findAct.imagePath = resolveTemplatePath(a.imageUseVar, a.imagePath);
                    const bool hasTemplate = a.imageUseVar
                        ? !a.imagePath.empty()
                        : !a.imagePath.empty();

                    // ── 后续操作：保存图片 ──
                    if (a.findImageFollowUp == 3) {
                        const std::wstring saveTarget = a.matchVarName.empty() ? L"image" : a.matchVarName;
                        int capX1 = 0, capY1 = 0, capX2 = 0, capY2 = 0;
                        bool regionOk = false;
                        ImageMatchResult lastRawMatch{};

                        if (!hasTemplate) {
                            if (a.searchFullScreen) {
                                int sx = 0, sy = 0, sw = 0, sh = 0;
                                GetVirtualScreenRect(sx, sy, sw, sh);
                                capX1 = sx; capY1 = sy; capX2 = sx + sw; capY2 = sy + sh;
                            } else {
                                capX1 = a.searchX1; capY1 = a.searchY1;
                                capX2 = a.searchX2; capY2 = a.searchY2;
                                // 空/倒置区域按整屏处理（与编辑器测试一致），避免静默截 0 面积
                                if (capX2 <= capX1 || capY2 <= capY1) {
                                    int sx = 0, sy = 0, sw = 0, sh = 0;
                                    GetVirtualScreenRect(sx, sy, sw, sh);
                                    capX1 = sx; capY1 = sy; capX2 = sx + sw; capY2 = sy + sh;
                                }
                            }
                            regionOk = (capX2 > capX1 && capY2 > capY1);
                            if (wmUsesTarget() && regionOk) {
                                int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                                ScriptAction probe = a;
                                if (wmExecPtr->ResolveClientSearchRect(probe, cx1, cy1, cx2, cy2)
                                    && wmExecPtr->MapClientRect(cx1, cy1, cx2, cy2, capX1, capY1, capX2, capY2)) {
                                    regionOk = true;
                                }
                            }
                        } else {
                            // 有模板：找锚图 → ApplyImageRegion → 截图区
                            if (findAct.imagePath.empty()) {
                                regionOk = false;
                            } else if (wmUsesTarget()) {
                                ImageMatchOutput output = wmExecPtr->FindImageClient(
                                    findAct, lockedScreen_, lockedVirtX_, lockedVirtY_);
                                if (!output.matches.empty()) {
                                    lastRawMatch = output.matches.front();
                                    const ImageMatchResult& match = output.matches.front();
                                    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                                    if (ApplyImageRegionToMatch(a,
                                            match.topLeftX, match.topLeftY,
                                            match.bottomRightX, match.bottomRightY,
                                            cx1, cy1, cx2, cy2)
                                        && wmExecPtr->MapClientRect(cx1, cy1, cx2, cy2,
                                            capX1, capY1, capX2, capY2)) {
                                        regionOk = true;
                                    }
                                }
                            } else {
                                int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                                if (a.searchFullScreen) {
                                    int sx = 0, sy = 0, sw = 0, sh = 0;
                                    GetVirtualScreenRect(sx, sy, sw, sh);
                                    x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                                }
                                HBITMAP tmpl = LoadBitmapFromFile(findAct.imagePath);
                                if (tmpl) {
                                    ImageMatchOptions opt = BuildExecutionFindImageOptions(findAct, findTmplScale);
                                    opt.maxMatches = 20;
                                    opt.maxOverlap = 0.5;
                                    ImageMatchOutput output;
                                    if (lockedScreen_) {
                                        output = FindTemplateInFrozenScreenMulti(
                                            lockedScreen_, lockedVirtX_, lockedVirtY_,
                                            x1, y1, x2, y2, tmpl, opt);
                                    } else {
                                        output = FindTemplateOnScreenMulti(x1, y1, x2, y2, tmpl, opt);
                                    }
                                    DeleteBitmapHandle(tmpl);
                                    if (!output.matches.empty()) {
                                        lastRawMatch = output.matches.front();
                                        const ImageMatchResult& match =
                                            NormalizeMatchVarResult(output.matches.front(), a.matchThreshold,
                                                                    a.perfectMatch);
                                        if (match.found) {
                                            regionOk = ApplyImageRegionToMatch(a,
                                                match.topLeftX, match.topLeftY,
                                                match.bottomRightX, match.bottomRightY,
                                                capX1, capY1, capX2, capY2);
                                        }
                                    }
                                }
                            }
                        }

                        if (regionOk) {
                            HBITMAP bmp = nullptr;
                            if (wmUsesTarget()) {
                                bmp = wmExecPtr->CaptureScreenRegionFromWindow(
                                    capX1, capY1, capX2, capY2,
                                    lockedScreen_, lockedVirtX_, lockedVirtY_);
                            } else {
                                bmp = CaptureScreenOrFrozenRegion(
                                    capX1, capY1, capX2, capY2,
                                    lockedScreen_, lockedVirtX_, lockedVirtY_);
                            }
                            if (bmp) {
                                CommitSavedImage(bmp, saveTarget, imageVarRunId, imageVars_);
                                DeleteBitmapHandle(bmp);
                            }
                        }
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatFindImageDebug(a, lastRawMatch, regionOk, 0, 0));
                        }
                    } else {
                    // 窗口/后台模式也必须 Prepare：偏移 nOffset 依赖 templateW/H。
                    // 若这里留空，ResolveFindImageClickPoint 会把偏移算成 0（落在中心）。
                    const PreparedFindImageMatch findPrep = PrepareFindImageMatch(findAct, findTmplScale);
                    int lastFindMs = 0;
                    bool findUsedAnamorphic = false;
                    auto runFind = [&]() -> ImageMatchResult {
                        findUsedAnamorphic = false;
                        if (wmUsesTarget()) {
                            ImageMatchOutput output = wmExecPtr->FindImageClient(
                                findAct, lockedScreen_, lockedVirtX_, lockedVirtY_);
                            lastFindMs = output.elapsedMs;
                            if (appSettings_.playback.autoOutputKeyFunctionDebug
                                && output.matches.empty()) {
                                wchar_t buf[320]{};
                                swprintf_s(buf,
                                    L"找图诊断(窗口模式) 无匹配 %dms bestNcc=%.1f%% "
                                    L"（应走扩展/客户区；若见全屏找图调试则窗口模式未激活）",
                                    output.elapsedMs, output.debugBestNccPercent);
                                AppendDebugLog(buf);
                            }
                            if (output.matches.empty()) return {};
                            return output.matches.front();
                        }
                        if (a.windowRelative) {
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(
                                    L"找图跳过：动作为窗口相对，但窗口模式未绑定；"
                                    L"全屏桌面 GDI 对游戏会得到假 0%");
                            }
                            return {};
                        }
                        int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                        if (a.searchFullScreen) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                        } else {
                            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
                            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
                            // 搜索区域为空/倒置（含全 0 的未配置状态）时回退整屏：
                            // 与 Web 编辑器「测试」和窗口模式 ResolveClientSearchRect 语义一致，
                            // 否则运行期会在 0 面积区域上找图，rawCandidates=0 永远匹配不到。
                            if ((x2 <= x1 || y2 <= y1)
                                || (x1 <= vsX + 2 && y1 <= vsY + 2
                                    && x2 >= vsX + vsW - 2 && y2 >= vsY + vsH - 2)) {
                                x1 = vsX; y1 = vsY; x2 = vsX + vsW; y2 = vsY + vsH;
                            }
                        }
                        HBITMAP tmpl = findPrep.bitmap;
                        if (!tmpl) {
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(L"找图失败: 无法加载模板 " + findAct.imagePath);
                            }
                            return {};
                        }
                        ImageMatchOptions opt = findPrep.options;
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            wchar_t buf[960]{};
                            swprintf_s(buf,
                                L"找图调试 ref=%dx%d cap=%dx%d cur=%dx%d@%ddpi scale=%.3fx%.3f "
                                L"preScale=%d matchScale=%.3f~%.3f tpl=%dx%d search=(%d,%d)-(%d,%d) thr=%.0f locked=%d",
                                activeCoordMeta.refWidth, activeCoordMeta.refHeight,
                                activeCoordMeta.captureWidth > 0 ? activeCoordMeta.captureWidth
                                    : activeCoordMeta.refWidth,
                                activeCoordMeta.captureHeight > 0 ? activeCoordMeta.captureHeight
                                    : activeCoordMeta.refHeight,
                                execTargetW, execTargetH, GetDpiForSystem(),
                                findTmplScale.sx, findTmplScale.sy,
                                findPrep.templatePreScaled ? 1 : 0,
                                opt.scaleMin, opt.scaleMax,
                                findPrep.templateW, findPrep.templateH, x1, y1, x2, y2, opt.thresholdPercent,
                                lockedScreen_ ? 1 : 0);
                            AppendDebugLog(buf);
                        }
                        opt.maxMatches = 20;
                        opt.maxOverlap = 0.5;
                        auto doMatch = [&](HBITMAP bmp, const ImageMatchOptions& matchOpt) -> ImageMatchOutput {
                            if (lockedScreen_) {
                                return FindTemplateInFrozenScreenMulti(
                                    lockedScreen_, lockedVirtX_, lockedVirtY_, x1, y1, x2, y2, bmp, matchOpt);
                            }
                            return FindTemplateOnScreenMulti(x1, y1, x2, y2, bmp, matchOpt);
                        };
                        ImageMatchOutput output = doMatch(tmpl, opt);
                        lastFindMs = output.elapsedMs;

                        // 宽高比变化时：仅当 NCC 还有希望时再试非等比拉伸
                        const bool aspectChanged =
                            std::abs(findTmplScale.sx - findTmplScale.sy) > 0.02;
                        if (output.matches.empty() && aspectChanged
                            && output.debugBestNccPercent >= opt.thresholdPercent * 0.40) {
                            HBITMAP stretched = LoadScaledTemplateBitmap(
                                findAct.imagePath, findTmplScale.sx, findTmplScale.sy);
                            if (stretched) {
                                ImageMatchOptions stretchOpt = opt;
                                stretchOpt.scaleMin = 1.0;
                                stretchOpt.scaleMax = 1.0;
                                stretchOpt.scaleStep = 1.0;
                                stretchOpt.disablePyramid = true;
                                stretchOpt.crossResolutionMatch = true;
                                ImageMatchOutput stretchOut = doMatch(stretched, stretchOpt);
                                lastFindMs += stretchOut.elapsedMs;
                                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                    wchar_t buf[320]{};
                                    swprintf_s(buf,
                                        L"找图兜底 非等比拉伸 bestNcc=%.1f%% matches=%d",
                                        stretchOut.debugBestNccPercent,
                                        static_cast<int>(stretchOut.matches.size()));
                                    AppendDebugLog(buf);
                                }
                                if (!stretchOut.matches.empty()) {
                                    findUsedAnamorphic = true;
                                    output = std::move(stretchOut);
                                }
                                DeleteBitmapHandle(stretched);
                            }
                        }

                        if (appSettings_.playback.autoOutputKeyFunctionDebug && output.matches.empty()) {
                            wchar_t buf[320]{};
                            swprintf_s(buf,
                                L"找图诊断 无共识匹配 %dms rawCandidates=%d bestNcc=%.1f%%",
                                output.elapsedMs, output.debugRawCandidates, output.debugBestNccPercent);
                            AppendDebugLog(buf);
                            // 模板过大提示：整屏/大区域模板对光标、时钟、消息等任何微小变化都敏感，
                            // 阈值越高越容易「无共识」→ 循环里反复触发后续动作烧 API。
                            const long long searchArea =
                                static_cast<long long>(std::max(1, x2 - x1))
                                * std::max(1, y2 - y1);
                            const long long tplArea =
                                static_cast<long long>(findPrep.templateW)
                                * findPrep.templateH;
                            if (searchArea > 0 && tplArea * 100 >= searchArea * 70) {
                                wchar_t warn[320]{};
                                swprintf_s(warn,
                                    L"找图提示 模板过大（约占搜索区 %d%%）：整屏/大区域模板对任何微小变化都敏感，"
                                    L"建议改用小目标区域、降低阈值，或在循环里加变化确认，避免反复触发后续步骤。",
                                    static_cast<int>(tplArea * 100 / searchArea));
                                AppendDebugLog(warn);
                            }
                        }
                        if (output.matches.empty()) return {};
                        return output.matches.front();
                    };
                    ImageMatchResult lastRawMatch{};
                    int lastTargetX = 0, lastTargetY = 0;
                    bool lastHadTarget = false;
                    // 找图时限按脚本原值（含正数），不随回放倍速缩放
                    const double findTimeSec = ResolveFindImageTimeSec(a.findTimeExpr, makeVarCtx());
                    const bool loopUntilFound = findTimeSec < 0.0;
                    const auto findStart = std::chrono::steady_clock::now();
                    do {
                        const ImageMatchResult rawMatch = runFind();
                        lastRawMatch = rawMatch;
                        const ImageMatchResult match = NormalizeMatchVarResult(
                            rawMatch, a.matchThreshold, a.perfectMatch);
                        if (a.findImageFollowUp == 2) {
                            const std::wstring varName = a.matchVarName.empty() ? L"matchRet" : a.matchVarName;
                            matchVars_[varName] = match;
                            break;
                        } else if (match.found) {
                            int tx = 0, ty = 0;
                            ResolveFindImageClickPoint(match, findPrep.templateW, findPrep.templateH,
                                a.nOffsetX, a.nOffsetY, findTmplScale,
                                findPrep.templatePreScaled || findUsedAnamorphic, tx, ty);
                            lastTargetX = tx;
                            lastTargetY = ty;
                            lastHadTarget = true;
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                int cx = 0, cy = 0;
                                FindImageMatchCenter(match, cx, cy);
                                const bool scaledTpl = findPrep.templatePreScaled || findUsedAnamorphic;
                                const int scaledOffX = static_cast<int>(std::round(
                                    a.nOffsetX * findPrep.templateW
                                    * (scaledTpl ? findTmplScale.sx : match.scale)));
                                const int scaledOffY = static_cast<int>(std::round(
                                    a.nOffsetY * findPrep.templateH
                                    * (scaledTpl ? findTmplScale.sy : match.scale)));
                                int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
                                GetVirtualScreenBounds(vsX, vsY, vsW, vsH);
                                wchar_t buf[640]{};
                                swprintf_s(buf,
                                    L"找图落点 %dms tl=(%d,%d) box=%dx%d scale=%.3f "
                                    L"center=(%d,%d) norm=(%.4f,%.4f) "
                                    L"offNorm=(%.4f,%.4f) offScaled=(%d,%d) target=(%d,%d)",
                                    lastFindMs,
                                    match.topLeftX, match.topLeftY,
                                    match.bottomRightX - match.topLeftX,
                                    match.bottomRightY - match.topLeftY,
                                    match.scale,
                                    cx, cy,
                                    vsW > 0 ? (cx - vsX) / static_cast<double>(vsW) : 0.0,
                                    vsH > 0 ? (cy - vsY) / static_cast<double>(vsH) : 0.0,
                                    a.nOffsetX, a.nOffsetY, scaledOffX, scaledOffY, tx, ty);
                                AppendDebugLog(buf);
                            }
                            const std::wstring varName = a.matchVarName.empty() ? L"matchRet" : a.matchVarName;
                            matchVars_[varName] = match;
                            if (a.findImageFollowUp == 0) {
                                wmSetLivePos(tx, ty, 0, 0);
                                MarkSimulatedInput();
                                if (wmUsesTarget()) wmExecPtr->PostMouseClickAtClient(tx, ty, a.button, false);
                                else wmMouseClick(tx, ty, a.button);
                                UnmarkSimulatedInput();
                            } else if (a.findImageFollowUp == 1) {
                                wmSetLivePos(tx, ty, 0, 0);
                                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                    wchar_t buf[160]{};
                                    if (wmUsesTarget()) {
                                        int ccx = 0, ccy = 0;
                                        if (wmExecPtr->GetCursorClientPos(ccx, ccy)) {
                                            swprintf_s(buf, L"找图软光标客户区=(%d,%d) 目标=(%d,%d)",
                                                ccx, ccy, tx, ty);
                                        } else {
                                            swprintf_s(buf, L"找图软光标未知 目标=(%d,%d)", tx, ty);
                                        }
                                    } else {
                                        POINT cur{};
                                        GetCursorPos(&cur);
                                        swprintf_s(buf, L"找图光标实际位置=(%d,%d)", cur.x, cur.y);
                                    }
                                    AppendDebugLog(buf);
                                }
                            }
                            break;
                        } else if (!loopUntilFound) {
                            if (findTimeSec <= 0.0) break;
                            const double elapsed = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - findStart).count();
                            if (elapsed >= findTimeSec) break;
                        }
                        // 可中断等待；窗口模式单次找图常 1~2s，重试间隔宜短以便热键立刻停
                        SleepInterruptible(0.05);
                    } while (!StopRequested() && !BreakoutTriggered());
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(FormatFindImageDebug(a, lastRawMatch, lastHadTarget, lastTargetX, lastTargetY));
                    }
                    if (findPrep.bitmap) {
                        DeleteBitmapHandle(findPrep.bitmap);
                    }
                    } // end else (non-saveImage followUp)
                }
                else if (a.type == ActionType::TextRecognition) {
                    // OCR 按图定位/识别同样隐藏本软件窗口，避免日志窗进入识别区域
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwnOcr(
                        UserFacingMainHwnd());
                    usesOcr = true;
                    workerUsesOcrVars_ = true;
                    holdOcrSession();
                    const std::wstring varName = a.matchVarName.empty() ? L"a" : a.matchVarName;
                    auto resolveOcrRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                        if (wmUsesTarget()) {
                            if (a.ocrRegionByImage) {
                                ScriptAction probe = a;
                                probe.imagePath = resolveTemplatePath(a.imageUseVar, a.imagePath);
                                // search* 为绝对找图范围；相对偏移在 imageRegion*
                                ImageMatchOutput output = wmExecPtr->FindImageClient(
                                    probe, lockedScreen_, lockedVirtX_, lockedVirtY_);
                                if (output.matches.empty()) return false;
                                const ImageMatchResult& match = output.matches.front();
                                int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                                if (!ApplyImageRegionToMatch(a,
                                        match.topLeftX, match.topLeftY,
                                        match.bottomRightX, match.bottomRightY,
                                        cx1, cy1, cx2, cy2)) {
                                    return false;
                                }
                                return wmExecPtr->MapClientRect(cx1, cy1, cx2, cy2, x1, y1, x2, y2);
                            }
                            if (!wmExecPtr->ResolveClientSearchRect(a, x1, y1, x2, y2)) return false;
                            return wmExecPtr->MapClientRect(x1, y1, x2, y2, x1, y1, x2, y2);
                        }
                        if (a.ocrRegionByImage) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            int findX1 = sx, findY1 = sy, findX2 = sx + sw, findY2 = sy + sh;
                            if (!a.searchFullScreen && a.searchX2 > a.searchX1 && a.searchY2 > a.searchY1) {
                                findX1 = a.searchX1;
                                findY1 = a.searchY1;
                                findX2 = a.searchX2;
                                findY2 = a.searchY2;
                            }
                            const TemplateScale tmplScale = currentTmplScale();
                            const std::wstring tmplPath = resolveTemplatePath(a.imageUseVar, a.imagePath);
                            HBITMAP tmpl = LoadBitmapFromFile(tmplPath);
                            if (!tmpl) return false;
                            ImageMatchOptions opt = BuildExecutionFindImageOptions(a, tmplScale);
                            opt.maxMatches = 20;
                            opt.maxOverlap = 0.5;
                            ImageMatchOutput output;
                            if (lockedScreen_) {
                                output = FindTemplateInFrozenScreenMulti(
                                    lockedScreen_, lockedVirtX_, lockedVirtY_,
                                    findX1, findY1, findX2, findY2, tmpl, opt);
                            } else {
                                output = FindTemplateOnScreenMulti(
                                    findX1, findY1, findX2, findY2, tmpl, opt);
                            }
                            DeleteBitmapHandle(tmpl);
                            if (output.matches.empty()) return false;
                            const ImageMatchResult& match = output.matches.front();
                            return ApplyImageRegionToMatch(a,
                                match.topLeftX, match.topLeftY,
                                match.bottomRightX, match.bottomRightY,
                                x1, y1, x2, y2);
                        }
                        if (a.searchFullScreen) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                        } else if (x2 <= x1 || y2 <= y1) {
                            // 空/倒置区域按整屏处理（与编辑器测试一致），避免 OCR 静默跑 0 面积区域
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                        }
                        return true;
                    };
                    auto runOcrAction = [&]() -> OcrVarResult {
                        OcrEngineOutput output;
                        if (wmUsesTarget()) {
                            output = wmExecPtr->RunOcrOnClientRegion(
                                a, lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                            if (!resolveOcrRegion(x1, y1, x2, y2)) {
                                return a.ocrResultMode == 1
                                    ? MakeOcrSearchVarResult(OcrTextLine{}, false)
                                    : MakeOcrTextVarResult(L"");
                            }
                            output = RunOcrOnScreenRegion(
                                x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_, a.ocrDigitsOnly);
                        }
                        if (!output.success) {
                            return a.ocrResultMode == 1
                                ? MakeOcrSearchVarResult(OcrTextLine{}, false)
                                : MakeOcrTextVarResult(L"");
                        }
                        if (a.ocrResultMode == 0) {
                            return MakeOcrTextVarResult(ConcatOcrLines(output));
                        }
                        MacroVariableContext ctx = makeVarCtx();
                        const std::wstring target = ResolveMacroVariables(a.ocrSearchText, ctx);
                        const auto found = FindTextInOcrLines(output, target);
                        if (found.has_value()) return MakeOcrSearchVarResult(*found, true);
                        return MakeOcrSearchVarResult(OcrTextLine{}, false);
                    };
                    auto applyFollowUpAt = [&](int centerX, int centerY) {
                        if (a.ocrFollowUp == 2) return;
                        int tx = centerX + a.offsetX;
                        int ty = centerY + a.offsetY;
                        if (wmExecPtr && wmExecPtr->IsActive()) {
                            windowmode::ScreenToClientPoint(
                                wmExecPtr->TargetHwnd(), tx, ty, tx, ty);
                        }
                        if (a.ocrFollowUp == 0) {
                            wmSetLivePos(tx, ty, 0, 0);
                            MarkSimulatedInput();
                            if (wmUsesTarget()) {
                                wmExecPtr->PostMouseClickAtClient(tx, ty, MouseButtonType::Left, false);
                            } else {
                                wmMouseClick(tx, ty, MouseButtonType::Left);
                            }
                            UnmarkSimulatedInput();
                        } else if (a.ocrFollowUp == 1) {
                            wmSetLivePos(tx, ty, 0, 0);
                        }
                    };
                    auto emitOcrDebug = [&](const std::wstring& textContent, bool searchFound) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatOcrDebug(a, textContent, searchFound, makeVarCtx()));
                        }
                    };
                    if (a.ocrResultMode == 0 && a.ocrFollowUp != 2) {
                        OcrEngineOutput output;
                        if (wmUsesTarget()) {
                            output = wmExecPtr->RunOcrOnClientRegion(
                                a, lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                            if (!resolveOcrRegion(x1, y1, x2, y2)) {
                                ocrVars_[varName] = MakeOcrTextVarResult(L"");
                                emitOcrDebug(L"", false);
                                return;
                            }
                            output = RunOcrOnScreenRegion(
                                x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_, a.ocrDigitsOnly);
                        }
                        const std::wstring text = output.success ? ConcatOcrLines(output) : L"";
                        ocrVars_[varName] = MakeOcrTextVarResult(text);
                        emitOcrDebug(text, false);
                        if (output.success && !output.lines.empty()) {
                            int minX = output.lines.front().x1, minY = output.lines.front().y1;
                            int maxX = output.lines.front().x2, maxY = output.lines.front().y2;
                            for (const auto& line : output.lines) {
                                minX = std::min(minX, line.x1);
                                minY = std::min(minY, line.y1);
                                maxX = std::max(maxX, line.x2);
                                maxY = std::max(maxY, line.y2);
                            }
                            applyFollowUpAt((minX + maxX) / 2, (minY + maxY) / 2);
                        }
                    } else if (a.ocrResultMode == 0) {
                        OcrVarResult result = runOcrAction();
                        // 逻辑转化动态列表：错屏 OCR 时按备注 listActivate 再试一次
                        if (a.remark.find(L"文字识别：动态列表") != std::wstring::npos
                            && !LooksLikeListOcrText(result.text)) {
                            std::wstring listAct;
                            const size_t p = a.remark.find(L"|listActivate:");
                            if (p != std::wstring::npos)
                                listAct = Trim(a.remark.substr(p + 14));
                            if (!listAct.empty() && !wmUsesTarget()) {
                                AppendDebugLog(L"逻辑转化 OCR 不像列表，重试 activate+OCR："
                                    + listAct);
                                const auto all = windowmode::ListSwitchableWindows();
                                const auto hits = windowmode::MatchWindows(all, listAct);
                                if (hits.size() == 1) {
                                    std::wstring err;
                                    if (windowmode::ActivateWindow(hits.front().hwnd, err)) {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                                        result = runOcrAction();
                                    }
                                }
                            }
                            if (!LooksLikeListOcrText(result.text)) {
                                AppendDebugLog(L"逻辑转化 OCR 仍不像列表，保留原文供动态填写 AI 自检");
                            }
                        }
                        ocrVars_[varName] = result;
                        emitOcrDebug(result.text, false);
                    } else {
                        OcrVarResult lastResult{};
                        do {
                            const OcrVarResult result = runOcrAction();
                            lastResult = result;
                            ocrVars_[varName] = result;
                            if (result.found && a.ocrFollowUp != 2) {
                                const int centerX = (result.topLeftX + result.bottomRightX) / 2;
                                const int centerY = (result.topLeftY + result.bottomRightY) / 2;
                                applyFollowUpAt(centerX, centerY);
                                break;
                            }
                            if (result.found || !a.findUntilFound) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        } while (!StopRequested() && !BreakoutTriggered());
                        emitOcrDebug(lastResult.text, lastResult.found != 0);
                    }
                }
                else if (a.type == ActionType::RunMacro) for (int i = 0; i < a.clickCount && !StopRequested() && !scheduledYieldLocalStop; ++i) {
                    std::wstring path = a.targetPath;
                    if (path.empty() || path == runningScriptPath) break;
                    // 嵌套宏只允许脚本/录制目录，防任意路径加载后链式 runProgram
                    {
                        wchar_t fullPath[MAX_PATH]{};
                        wchar_t scriptsDir[MAX_PATH]{};
                        wchar_t recDir[MAX_PATH]{};
                        if (GetFullPathNameW(path.c_str(), MAX_PATH, fullPath, nullptr) == 0) break;
                        GetFullPathNameW(ScriptsDir().c_str(), MAX_PATH, scriptsDir, nullptr);
                        GetFullPathNameW(RecordingsDir().c_str(), MAX_PATH, recDir, nullptr);
                        const size_t sl = wcslen(scriptsDir);
                        const size_t rl = wcslen(recDir);
                        const bool inScripts = sl > 0 && _wcsnicmp(fullPath, scriptsDir, sl) == 0
                            && (fullPath[sl] == L'\\' || fullPath[sl] == L'\0');
                        const bool inRec = rl > 0 && _wcsnicmp(fullPath, recDir, rl) == 0
                            && (fullPath[rl] == L'\\' || fullPath[rl] == L'\0');
                        if (!inScripts && !inRec) {
                            AppendDebugLog(L"运行宏失败：路径不在脚本/录制目录内");
                            break;
                        }
                        path = fullPath;
                    }
                    const ScriptFileData nestedData = LoadScriptFileData(path, false);
                    if (nestedData.actions.empty()) break;
                    CoordMeta nestedMeta = ScriptCoordMetaForExecution(nestedData.coordMeta);
                    std::vector<ScriptAction> nested =
                        PrepareScriptActionsForExecution(nestedData.actions, nestedMeta);
                    if (IsRecordingScriptPath(path) || ScriptIsTimedInputSequence(nested)
                        || nestedData.inputTimingVersion > 0) {
                        RepairCompressedRelativeGaps(nested);
                    }
                    if (!usesOcr && ScriptUsesTextRecognition(nested)) {
                        usesOcr = true;
                        workerUsesOcrVars_ = true;
                    }
                    if (usesOcr) holdOcrSession();
                    const std::vector<ScriptAction>* prevActions = activeActions;
                    const std::wstring prevPath = runningScriptPath;
                    const CoordMeta prevCoordMeta = activeCoordMeta;
                    activeCoordMeta = nestedMeta;
                    if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                    activeActions = &nested;
                    runningScriptPath = path;
                    // 嵌套录制/精密轨迹：对齐时间轴原点，避免沿用外层已漂移的 elapsed
                    if (inputTimeline.enabled
                        && (IsRecordingScriptPath(path)
                            || ScriptIsTimedInputSequence(nested)
                            || nestedData.inputTimingVersion > 0)) {
                        inputTimeline.Reset();
                    }
                    runRange(0, nested.size());
                    activeActions = prevActions;
                    runningScriptPath = prevPath;
                    activeCoordMeta = prevCoordMeta;
                    if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                    if (ShouldWaitAfterRepeat(a, i) && !StopRequested() && !scheduledYieldLocalStop) {
                        SleepInterruptible(quickscript::ScalePlaybackTimeSeconds(
                            a.duration + RandomDelay(a.randomDuration), playbackTimeScale));
                    }
                }
                else if (a.type == ActionType::LockScreenshot) {
                    clearLockedScreen();
                    if (wmUsesTarget()) {
                        if (!wmExecPtr->LockWindowCapture(lockedScreen_, lockedVirtX_, lockedVirtY_)) {
                            AppendDebugLog(std::wstring(wmUsesBackground() ? L"后台窗口模式" : L"窗口模式")
                                + L"：锁定窗口截图失败，后续找图将使用实时截图");
                        }
                    } else {
                        lockedScreen_ = CaptureVirtualScreen(lockedVirtX_, lockedVirtY_);
                    }
                }
                else if (a.type == ActionType::UnlockScreenshot) {
                    clearLockedScreen();
                }
                else if (a.type == ActionType::StopMacro) {
                    // 运行宏调用栈：stopMacro 置全局停止，结束调用方。
                    // 定时插入：原脚本不是父脚本，只结束这次定时，随后从暂停处继续。
                    if (StopMacroShouldEndEntireRun(scheduledYieldDepth)) {
                        stopFlag_ = true;
                    } else {
                        scheduledYieldLocalStop = true;
                    }
                }
                else if (a.type == ActionType::EndLoop) {
                    if (aiLoopDepth > 0) pendingBreakLoop = true;
                }
                else if (a.type == ActionType::RunProgram) {
                    const std::wstring path = ResolveRunProgramPath(
                        a.shortcutPreset, ExpandEnvironmentVars(a.targetPath));
                    if (path.empty()) {
                        AppendDebugLog(L"运行程序失败：目标路径为空");
                    } else if (!LaunchProgram(path, a.inputText)) {
                        AppendDebugLog(L"运行程序失败：无法启动「" + path + L"」");
                        AppendAiDebugLog(L"  [诊断] runProgram 启动失败：「" + path
                            + L"」。若是浏览器请先 listWindows→activateWindow 复用已开窗口；"
                              L"裸名 edge 会被解析为 msedge.exe 真实路径；"
                              L"仍打不开请 openAppViaSearch(query=应用显示名) 走开始菜单搜索。");
                    }
                }
                else if (a.type == ActionType::CloseProgram) {
                    if (!a.targetPath.empty())
                        CloseProgramsByTarget(ExpandEnvironmentVars(a.targetPath),
                            a.matchFileNameOnly);
                }
                else if (a.type == ActionType::OpenWebpage) {
                    if (!a.targetPath.empty()) {
                        const std::wstring webTarget = ExpandEnvironmentVars(a.targetPath);
                        const auto schemeEnd = webTarget.find(L"://");
                        bool allow = false;
                        if (schemeEnd != std::wstring::npos) {
                            std::wstring scheme = webTarget.substr(0, schemeEnd);
                            for (auto& ch : scheme) {
                                if (ch >= L'A' && ch <= L'Z')
                                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                            }
                            allow = (scheme == L"http" || scheme == L"https");
                        } else {
                            // 无 scheme：当作 https 补全由系统处理前仍拒绝 file/UNC 裸路径冒充网页
                            allow = false;
                            AppendDebugLog(L"打开网页失败：仅允许 http/https URL");
                        }
                        if (allow && !LaunchProgram(webTarget, L"")) {
                            AppendDebugLog(L"打开网页失败：无法打开「" + webTarget + L"」");
                        } else if (!allow && schemeEnd != std::wstring::npos) {
                            AppendDebugLog(L"打开网页失败：仅允许 http/https URL");
                        }
                    }
                }
                else if (a.type == ActionType::OpenFile) {
                    if (!a.targetPath.empty())
                        LaunchProgram(ExpandEnvironmentVars(a.targetPath), L"");
                }
                else if (a.type == ActionType::ActivateWindow) {
                    const std::wstring query = Trim(ExpandEnvironmentVars(a.targetPath));
                    if (query.empty()) {
                        AppendDebugLog(L"激活窗口失败：match/targetPath 为空");
                    } else {
                        qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                        const auto all = windowmode::ListSwitchableWindows();
                        const auto hits = windowmode::MatchWindows(all, query);
                        if (hits.empty()) {
                            AppendDebugLog(L"激活窗口失败：无匹配「" + query + L"」");
                        } else if (hits.size() > 1) {
                            AppendDebugLog(L"激活窗口失败：「" + query + L"」匹配到 "
                                + std::to_wstring(hits.size()) + L" 个窗口，拒绝自动选择");
                        } else {
                            std::wstring err;
                            if (!windowmode::ActivateWindow(hits.front().hwnd, err)) {
                                AppendDebugLog(L"激活窗口失败：" + err);
                            } else {
                                AppendDebugLog(L"已激活窗口：" + hits.front().title);
                            }
                        }
                    }
                }
                else if (a.type == ActionType::TimerRecordTime) {
                    if (!a.loopVarName.empty()) {
                        timerStarts_[a.loopVarName] = std::chrono::steady_clock::now();
                    }
                }
                else if (a.type == ActionType::MousePlayback) for (int i = 0; i < a.clickCount && !StopRequested() && !scheduledYieldLocalStop; ++i) {
                    std::wstring path = a.targetPath;
                    if (path.empty()) break;
                    {
                        wchar_t fullPath[MAX_PATH]{};
                        wchar_t scriptsDir[MAX_PATH]{};
                        wchar_t recDir[MAX_PATH]{};
                        if (GetFullPathNameW(path.c_str(), MAX_PATH, fullPath, nullptr) == 0) break;
                        GetFullPathNameW(ScriptsDir().c_str(), MAX_PATH, scriptsDir, nullptr);
                        GetFullPathNameW(RecordingsDir().c_str(), MAX_PATH, recDir, nullptr);
                        const size_t sl = wcslen(scriptsDir);
                        const size_t rl = wcslen(recDir);
                        const bool inScripts = sl > 0 && _wcsnicmp(fullPath, scriptsDir, sl) == 0
                            && (fullPath[sl] == L'\\' || fullPath[sl] == L'\0');
                        const bool inRec = rl > 0 && _wcsnicmp(fullPath, recDir, rl) == 0
                            && (fullPath[rl] == L'\\' || fullPath[rl] == L'\0');
                        if (!inScripts && !inRec) {
                            AppendDebugLog(L"运行录制回放失败：路径不在脚本/录制目录内");
                            break;
                        }
                        path = fullPath;
                    }
                    const ScriptFileData nestedData = LoadScriptFileData(path, false);
                    if (nestedData.actions.empty()) break;
                    CoordMeta nestedMeta = ScriptCoordMetaForExecution(nestedData.coordMeta);
                    std::vector<ScriptAction> nested =
                        PrepareScriptActionsForExecution(nestedData.actions, nestedMeta);
                    if (IsRecordingScriptPath(path) || ScriptIsTimedInputSequence(nested)
                        || nestedData.inputTimingVersion > 0) {
                        RepairCompressedRelativeGaps(nested);
                    }
                    const std::vector<ScriptAction>* prevActions = activeActions;
                    const std::wstring prevPath = runningScriptPath;
                    const CoordMeta prevCoordMeta = activeCoordMeta;
                    activeCoordMeta = nestedMeta;
                    if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                    activeActions = &nested;
                    runningScriptPath = path;
                    if (inputTimeline.enabled
                        && (IsRecordingScriptPath(path)
                            || ScriptIsTimedInputSequence(nested)
                            || nestedData.inputTimingVersion > 0)) {
                        inputTimeline.Reset();
                    }
                    const double prevScale = playbackTimeScale;
                    playbackTimeScale = quickscript::PlaybackTimeScaleAlways(a.playbackSpeed);
                    runRange(0, nested.size());
                    playbackTimeScale = prevScale;
                    activeActions = prevActions;
                    runningScriptPath = prevPath;
                    activeCoordMeta = prevCoordMeta;
                    if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                    if (ShouldWaitAfterRepeat(a, i) && !StopRequested() && !scheduledYieldLocalStop) {
                        SleepInterruptible(quickscript::ScalePlaybackTimeSeconds(
                            a.duration + RandomDelay(a.randomDuration), playbackTimeScale));
                    }
                }
                else if (a.type == ActionType::GetCursorPos) {
                    int cx = 0, cy = 0;
                    bool gotPos = false;
                    if (wmExecPtr && wmExecPtr->IsActive()) {
                        gotPos = wmExecPtr->GetCursorClientPos(cx, cy);
                    } else {
                        POINT pt{};
                        gotPos = GetCursorPos(&pt) == TRUE;
                        cx = pt.x;
                        cy = pt.y;
                    }
                    if (gotPos) {
                        const std::wstring varName = a.matchVarName.empty() ? L"a" : a.matchVarName;
                        ImageMatchResult match{};
                        match.found = true;
                        match.topLeftX = cx;
                        match.topLeftY = cy;
                        match.bottomRightX = cx;
                        match.bottomRightY = cy;
                        match.x = cx;
                        match.y = cy;
                        match.score = 100.0;
                        matchVars_[varName] = match;
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"获取当前光标位置→[" + varName + L"] "
                                + std::to_wstring(cx) + L"," + std::to_wstring(cy));
                        }
                    }
                }
                else if (a.type == ActionType::GetColor) {
                    MacroVariableContext ctx = makeVarCtx();
                    int px = a.x, py = a.y;
                    if (a.moveFromVar) {
                        TryResolveIntOperand(a.moveVarExprX, ctx, px);
                        TryResolveIntOperand(a.moveVarExprY, ctx, py);
                    }
                    int r = 0, g = 0, b = 0;
                    HBITMAP fr = lockedScreen_;
                    const bool ok = GetScreenPixelRgb(px, py, r, g, b,
                        fr, lockedVirtX_, lockedVirtY_);
                    const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                    if (ok) {
                        aiVars_[varName] = FormatColorHex(r, g, b);
                        ImageMatchResult match{};
                        match.found = true;
                        match.x = px;
                        match.y = py;
                        match.topLeftX = px;
                        match.topLeftY = py;
                        match.bottomRightX = px;
                        match.bottomRightY = py;
                        match.score = 100.0;
                        matchVars_[varName] = match;
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"获取颜色@" + std::to_wstring(px) + L","
                                + std::to_wstring(py) + L" → " + aiVars_[varName]);
                        }
                    }
                }
                else if (a.type == ActionType::FindColor) {
                    int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                    if (a.searchFullScreen) {
                        int vx = 0, vy = 0, vw = 0, vh = 0;
                        GetVirtualScreenRect(vx, vy, vw, vh);
                        x1 = vx; y1 = vy; x2 = vx + vw; y2 = vy + vh;
                    }
                    const ColorMatchHit hit = FindColorInScreenRegion(
                        x1, y1, x2, y2, a.colorR, a.colorG, a.colorB, a.colorTolerance,
                        lockedScreen_, lockedVirtX_, lockedVirtY_, 2, &stopFlag_);
                    if (StopRequested()) return;
                    const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                    ImageMatchResult match{};
                    if (hit.found) {
                        match.found = true;
                        match.x = hit.x;
                        match.y = hit.y;
                        match.topLeftX = hit.x;
                        match.topLeftY = hit.y;
                        match.bottomRightX = hit.x;
                        match.bottomRightY = hit.y;
                        match.score = 100.0 - hit.distance;
                        aiVars_[varName] = FormatColorHex(hit.r, hit.g, hit.b);
                        const int tx = hit.x + a.offsetX;
                        const int ty = hit.y + a.offsetY;
                        if (a.findImageFollowUp == 0) {
                            ScriptAction click = a;
                            click.type = ActionType::MouseClick;
                            click.x = tx; click.y = ty;
                            click.moveFromVar = false;
                            executeOne(click);
                        } else if (a.findImageFollowUp == 1) {
                            ScriptAction mv = a;
                            mv.type = ActionType::MoveMouse;
                            mv.x = tx; mv.y = ty;
                            mv.moveFromVar = false;
                            executeOne(mv);
                        }
                    }
                    matchVars_[varName] = match;
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(hit.found
                            ? (L"找色命中 " + FormatColorHex(a.colorR, a.colorG, a.colorB)
                                + L" @ " + std::to_wstring(hit.x) + L"," + std::to_wstring(hit.y))
                            : (L"找色未命中 " + FormatColorHex(a.colorR, a.colorG, a.colorB)));
                    }
                }
                else if (a.type == ActionType::ColorMatch) {
                    MacroVariableContext ctx = makeVarCtx();
                    int px = a.x, py = a.y;
                    if (a.moveFromVar) {
                        TryResolveIntOperand(a.moveVarExprX, ctx, px);
                        TryResolveIntOperand(a.moveVarExprY, ctx, py);
                    }
                    int r = 0, g = 0, b = 0, dist = 0;
                    const bool matched = MatchColorAtScreenPoint(px, py,
                        a.colorR, a.colorG, a.colorB, a.colorTolerance,
                        &r, &g, &b, &dist, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                    ImageMatchResult match{};
                    match.found = matched;
                    match.x = px;
                    match.y = py;
                    match.topLeftX = px;
                    match.topLeftY = py;
                    match.bottomRightX = px;
                    match.bottomRightY = py;
                    match.score = matched ? (100.0 - dist) : 0.0;
                    matchVars_[varName] = match;
                    aiVars_[varName] = FormatColorHex(r, g, b);
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(std::wstring(matched ? L"颜色匹配成功" : L"颜色匹配失败")
                            + L" @" + std::to_wstring(px) + L"," + std::to_wstring(py)
                            + L" 实际" + FormatColorHex(r, g, b));
                    }
                }
                else if (a.type == ActionType::AiTextAnalysis) {
                    if (StopRequested()) return;
                    MacroVariableContext ctx = makeVarCtx();
                    const std::wstring resolvedPrompt = ResolveMacroVariables(a.aiPrompt, ctx);
                    const std::wstring outputVarName = a.aiOutputVarName.empty() ? L"aiResult" : a.aiOutputVarName;
                    const std::wstring fallback = a.aiFallbackValue.empty()
                        ? (a.aiOutputType == 1 ? L"0" : L"") : a.aiFallbackValue;
                    const std::wstring modelLabel = EffectiveAiModelName(a);
                    AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：发送 prompt…");
                    try {
                        const AiActionResult ar = RunAiTextAnalysisForAction(
                            a, resolvedPrompt, &aiSessions, aiLoopDepth);
                        if (ar.ok) {
                            StoreAiOutputVar(outputVarName, a.aiOutputType, ar.textResult, fallback);
                            AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：完成 → "
                                + outputVarName + L" = " + aiVars_[outputVarName]);
                        } else {
                            StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                            AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：失败，使用降级值："
                                + fallback + (ar.errorMessage.empty() ? L"" : L" (" + ar.errorMessage + L")"));
                        }
                    } catch (...) {
                        StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                        AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：执行异常，使用降级值：" + fallback);
                    }
                }
                else if (a.type == ActionType::AiImageAnalysis) {
                    if (StopRequested()) return;
                    MacroVariableContext ctx = makeVarCtx();
                    MacroClipboardSnapshot clipSnap;
                    std::vector<std::string> clipExtraJpeg;
                    if (PromptMentionsCtrlClipboard(a.aiPrompt)) {
                        clipSnap = ReadMacroClipboardSnapshot();
                        ctx.clipboardSnapshot = &clipSnap;
                        ctx.clipboardExpandMode = ClipboardExpandMode::Ai;
                        clipExtraJpeg = EncodeClipboardSnapshotImages(clipSnap);
                        if (!clipExtraJpeg.empty()) {
                            AppendAiDebugLog(L"AI图片分析：附加剪贴板图片 "
                                + std::to_wstring(clipExtraJpeg.size()) + L" 张");
                        } else if (clipSnap.hasBitmap || std::any_of(
                            clipSnap.files.begin(), clipSnap.files.end(), LooksLikeImageFilePath)) {
                            AppendAiDebugLog(L"AI图片分析：提示词引用了剪贴板图片，但未能编码附图");
                        }
                    }
                    const std::wstring resolvedPrompt = ResolveMacroVariables(a.aiPrompt, ctx);
                    const std::wstring outputVarName = a.aiOutputVarName.empty() ? L"aiImgResult" : a.aiOutputVarName;
                    const std::wstring fallback = a.aiFallbackValue.empty()
                        ? (a.aiOutputType == 1 ? L"0" : L"") : a.aiFallbackValue;
                    const std::wstring modelLabel = EffectiveAiModelName(a);
                    const double scale = std::clamp(a.aiImageScale, 0.1, 1.0);
                    HBITMAP screenBmp = nullptr;
                    int sw = 0, sh = 0;
                    int capX1 = 0, capY1 = 0, capX2 = 0, capY2 = 0;
                    auto resolveAiRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                        ScriptAction aiProbe = a;
                        if (a.aiImageUseVar) {
                            aiProbe.aiTargetImagePath = resolveTemplatePath(true, a.aiTargetImagePath);
                            aiProbe.imagePath = aiProbe.aiTargetImagePath;
                            aiProbe.aiImageUseVar = false;
                        }
                        if (wmUsesTarget()) {
                            return wmExecPtr->ResolveAiScreenRect(
                                aiProbe, x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                        }
                        int sx = 0, sy = 0, rsw = 0, rsh = 0;
                        GetVirtualScreenRect(sx, sy, rsw, rsh);
                        int searchX1 = sx, searchY1 = sy, searchX2 = sx + rsw, searchY2 = sy + rsh;
                        if (a.aiSearchX2 > a.aiSearchX1 && a.aiSearchY2 > a.aiSearchY1) {
                            searchX1 = a.aiSearchX1;
                            searchY1 = a.aiSearchY1;
                            searchX2 = a.aiSearchX2;
                            searchY2 = a.aiSearchY2;
                        }
                        if (a.aiRegionByImage && !a.aiTargetImagePath.empty()) {
                            const TemplateScale tmplScale = currentTmplScale();
                            HBITMAP tmpl = LoadBitmapFromFile(aiProbe.aiTargetImagePath);
                            if (!tmpl) return false;
                            ImageMatchOptions opt = BuildExecutionFindImageOptions(a, tmplScale);
                            opt.maxMatches = 20;
                            opt.maxOverlap = 0.5;
                            ImageMatchOutput output;
                            if (lockedScreen_) {
                                output = FindTemplateInFrozenScreenMulti(
                                    lockedScreen_, lockedVirtX_, lockedVirtY_,
                                    searchX1, searchY1, searchX2, searchY2, tmpl, opt);
                            } else {
                                output = FindTemplateOnScreenMulti(
                                    searchX1, searchY1, searchX2, searchY2, tmpl, opt);
                            }
                            DeleteBitmapHandle(tmpl);
                            if (output.matches.empty()) return false;
                            const ImageMatchResult& match = output.matches.front();
                            return ApplyImageRegionToMatch(a,
                                match.topLeftX, match.topLeftY,
                                match.bottomRightX, match.bottomRightY,
                                x1, y1, x2, y2);
                        }
                        x1 = searchX1; y1 = searchY1; x2 = searchX2; y2 = searchY2;
                        return x2 > x1 && y2 > y1;
                    };
                    if (!resolveAiRegion(capX1, capY1, capX2, capY2)) {
                        StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                        AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：无法定位分析区域，使用降级值：" + fallback);
                    } else {
                        if (wmUsesTarget()) {
                            screenBmp = wmExecPtr->CaptureScreenRegionFromWindow(
                                capX1, capY1, capX2, capY2,
                                lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            screenBmp = CaptureAiRegionComposed(capX1, capY1, capX2, capY2);
                        }
                        if (screenBmp) {
                            BITMAP bm{};
                            if (GetObject(screenBmp, sizeof(bm), &bm)) { sw = bm.bmWidth; sh = bm.bmHeight; }
                        }
                        if (!screenBmp || sw <= 0 || sh <= 0) {
                            StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                            AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：截屏失败，使用降级值：" + fallback);
                        } else {
                            std::wstring imeStatus;
                            if (!wmUsesTarget()) {
                                imeStatus = QueryForegroundImeStatusText();
                                if (imeStatus.find(L"[输入法] 英文") != std::wstring::npos)
                                    imeStatus.clear();
                            }
                            const AiImageEncodeResult encoded = EncodeBitmapForAiAnalysis(
                                screenBmp, scale, 768, imeStatus.empty() ? nullptr : &imeStatus);
                            DeleteBitmapHandle(screenBmp);
                            std::wstring captureInfo = L"AI图片分析 [" + modelLabel + L"]：截屏完成("
                                + std::to_wstring(encoded.srcWidth) + L"×" + std::to_wstring(encoded.srcHeight);
                            if (encoded.effectiveScale < 0.999
                                || encoded.outWidth != encoded.srcWidth
                                || encoded.outHeight != encoded.srcHeight) {
                                captureInfo += L"→" + std::to_wstring(encoded.outWidth)
                                    + L"×" + std::to_wstring(encoded.outHeight);
                            }
                            captureInfo += L")，发送中…";
                            AppendAiDebugLog(captureInfo);

                            if (encoded.base64.empty()) {
                                StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                                AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：图片编码失败，使用降级值：" + fallback);
                            } else {
                                AppendAiDebugLog(L"  图片数据 " + std::to_wstring(encoded.base64.size())
                                    + L" 字节(base64)，调用 API…");
                                try {
                                    const AiActionResult ar = RunAiImageAnalysisForAction(
                                        a, resolvedPrompt, encoded.base64, &aiSessions, aiLoopDepth,
                                        clipExtraJpeg.empty() ? nullptr : &clipExtraJpeg);
                                    if (ar.ok) {
                                        StoreAiOutputVar(outputVarName, a.aiOutputType, ar.textResult, fallback);
                                        AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：完成 → "
                                            + outputVarName + L" = " + aiVars_[outputVarName]);
                                    } else {
                                        StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                                        AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：失败，使用降级值："
                                            + fallback + (ar.errorMessage.empty() ? L"" : L" (" + ar.errorMessage + L")"));
                                    }
                                } catch (...) {
                                    StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                                    AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：执行异常，使用降级值：" + fallback);
                                }
                            }
                        }
                    }
                }
                else if (a.type == ActionType::AiActionExecute) {
                    runAiActionExecute(a, nullptr);
                }
            };

            auto executeWithBreakout = [&](const ScriptAction& action) {
                if (workerBreakoutTime_ <= 0) {
                    executeOne(action);
                    return;
                }
                while (!StopRequested() && !scheduledYieldLocalStop) {
                    breakoutUserInput_ = false;
                    executeOne(action);
                    if (StopRequested() || scheduledYieldLocalStop
                        || !breakoutUserInput_.load(std::memory_order_relaxed)) break;
                    waitBreakoutCooldown();
                    // 脱离暂停期间墙钟继续走；精密时间轴若不清零，后续 Wait 会因「已过点」
                    // 瞬间追赶跑完整轮，看起来像跳到脚本末尾，下一轮又从开头开始。
                    // 与注入后端无关（系统模拟 / Interception / VirtualHid 共用此路径）。
                    if (inputTimeline.enabled) {
                        inputTimeline.Reset();
                        timelineInterrupted = false;
                    }
                }
            };

            // ── 编辑器调试闸门（仅调试脚本主序列，嵌套 runRange 不生效）──
            auto debugGate = [&](size_t i) {
                if (!debugMode_.load(std::memory_order_relaxed)
                    || StopRequested() || scheduledYieldLocalStop) return;
                if (debugActionsPtr_ != activeActions) return;
                if (debugStepMode_.load(std::memory_order_relaxed)) {
                    if (!debugStepSignal_.load(std::memory_order_relaxed)) {
                        if (qst::desktop_tools::MacroDebug().IsCreated()) {
                            qst::desktop_tools::MacroDebug().AppendLog(
                                L"调试：等待单步（第 " + std::to_wstring(static_cast<int>(i) + 1)
                                + L" 步，按调试热键执行）");
                        }
                        while (!debugStepSignal_.load(std::memory_order_relaxed)
                            && !StopRequested() && !scheduledYieldLocalStop) {
                            SleepInterruptible(0.02);
                        }
                    }
                    debugStepSignal_.store(false, std::memory_order_relaxed);
                } else {
                    while (debugPaused_.load(std::memory_order_relaxed)
                        && !StopRequested() && !scheduledYieldLocalStop) {
                        SleepInterruptible(0.02);
                    }
                }
            };
            auto debugAfterAction = [&](size_t i) {
                if (!debugMode_.load(std::memory_order_relaxed)
                    || StopRequested() || scheduledYieldLocalStop) return;
                if (debugActionsPtr_ != activeActions) return;
                if (debugBreakpoints_.count(static_cast<int>(i))) {
                    if (qst::desktop_tools::MacroDebug().IsCreated()) {
                        qst::desktop_tools::MacroDebug().AppendLog(
                            L"调试：断点命中第 " + std::to_wstring(static_cast<int>(i) + 1)
                            + L" 步，执行后结束调试");
                    }
                    stopFlag_.store(true, std::memory_order_relaxed);
                }
            };

            runBlockByName = [&](const std::wstring& name) -> RunRangeResult {
                if (StopRequested() || scheduledYieldLocalStop || name.empty()) return RunRangeResult::Normal;
                aiSessions.ClearBlock();
                std::unordered_map<std::wstring, size_t> blockDefs;
                for (size_t i = 0; i < activeActions->size(); ++i) {
                    if ((*activeActions)[i].type == ActionType::DefineBlock && !(*activeActions)[i].blockName.empty()) {
                        blockDefs[(*activeActions)[i].blockName] = i;
                    }
                }
                const auto it = blockDefs.find(name);
                if (it == blockDefs.end()) return RunRangeResult::Normal;
                if (blockCallStack.count(name)) return RunRangeResult::Normal;
                blockCallStack.insert(name);
                const RunRangeResult result = runRange(it->second + 1, containerBodyEnd(it->second));
                blockCallStack.erase(name);
                return result;
            };
            std::function<void()> drainScheduledYield;
            runRange = [&](size_t start, size_t end) -> RunRangeResult {
                auto isDirectLoopBodyRange = [&](size_t loopIdx) {
                    return start == loopIdx + 1 && end == containerBodyEnd(loopIdx);
                };
                auto resolveGotoCursor = [&](size_t targetIdx, size_t& outCursor) {
                    const int outerLoop = OutermostEnclosingLoop(*activeActions, targetIdx);
                    if (outerLoop < 0) {
                        outCursor = targetIdx;
                        return;
                    }
                    if (isDirectLoopBodyRange(static_cast<size_t>(outerLoop))) {
                        outCursor = targetIdx;
                        return;
                    }
                    loopEntryGotoTarget = targetIdx;
                    outCursor = static_cast<size_t>(outerLoop);
                };
                auto consumePendingGoto = [&](size_t scopeStart, size_t scopeEnd, size_t& cursor) -> RunRangeResult {
                    if (!pendingGoto) return RunRangeResult::Normal;
                    const size_t targetIdx = *pendingGoto;
                    pendingGoto.reset();
                    resolveGotoCursor(targetIdx, cursor);
                    if (cursor < scopeStart || cursor >= scopeEnd) return RunRangeResult::GotoPending;
                    return RunRangeResult::Normal;
                };
                for (size_t i = start; i < end && !StopRequested() && !scheduledYieldLocalStop; ) {
                    if (drainScheduledYield) drainScheduledYield();
                    if (StopRequested() || scheduledYieldLocalStop) break;
                    if (pendingGoto) {
                        const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                        if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                        continue;
                    }
                    const auto& a = (*activeActions)[i];
                    if (a.type == ActionType::EndLoop) return RunRangeResult::BreakLoop;
                    const bool debugEntryBlock = debugMode_.load(std::memory_order_relaxed)
                        && debugBlockEntrySet_.count(static_cast<int>(i));
                    if (SkipsInMainFlow(a.type)) {
                        if (debugEntryBlock) {
                            // 调试入口所在的 defineBlock：块体按流程执行一次（单步可进入块内）
                            debugGate(i);
                            if (StopRequested() || scheduledYieldLocalStop) {
                                return RunRangeResult::Normal;
                            }
                            const size_t bodyEnd = containerBodyEnd(i);
                            const RunRangeResult bodyResult = runRange(i + 1, bodyEnd);
                            if (bodyResult == RunRangeResult::GotoPending || pendingGoto) {
                                const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                                if (gotoResult == RunRangeResult::GotoPending) {
                                    return RunRangeResult::GotoPending;
                                }
                                continue;
                            }
                            if (bodyResult == RunRangeResult::BreakLoop) {
                                return RunRangeResult::BreakLoop;
                            }
                            debugAfterAction(i);
                            i = bodyEnd;
                            continue;
                        }
                        i = containerBodyEnd(i);
                        continue;
                    }
                    // 结构性动作（Else）不设闸门；可执行动作在闸门等待后仍受停止控制
                    if (a.type != ActionType::Else) {
                        debugGate(i);
                        if (StopRequested() || scheduledYieldLocalStop) {
                            return RunRangeResult::Normal;
                        }
                    }
                    if (a.type == ActionType::Loop) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        const size_t bodyEnd = containerBodyEnd(i);
                        const int thisLoopDepth = aiLoopDepth;
                        ++aiLoopDepth;
                        aiSessions.EnsureLoopDepth(aiLoopDepth);
                        int iter = 1;
                        bool broke = false;
                        const auto loopStartTime = std::chrono::steady_clock::now();
                        while (!StopRequested() && !scheduledYieldLocalStop && !broke) {
                            inputTimeline.Reset(); // 每次循环从零重新对齐录制时间轴
                            aiSessions.ClearLoopAt(thisLoopDepth);
                            if (!a.loopVarName.empty()) loopVars_[a.loopVarName] = iter;
                            MacroVariableContext ctx = makeVarCtx();
                            const int maxLoop = ResolveLoopMaxCount(a, ctx, loopStartTime);
                            if (!(maxLoop < 0 || iter <= maxLoop)) break;
                            size_t runFrom = i + 1;
                            if (iter == 1 && loopEntryGotoTarget) {
                                const size_t entryTarget = *loopEntryGotoTarget;
                                if (entryTarget > i && entryTarget < bodyEnd) {
                                    if (EnclosingChildLoopInBody(*activeActions, i, entryTarget) < 0) {
                                        runFrom = entryTarget;
                                        loopEntryGotoTarget.reset();
                                    }
                                }
                            }
                            const RunRangeResult bodyResult = runRange(runFrom, bodyEnd);
                            if (scheduledYieldLocalStop) broke = true;
                            if (bodyResult == RunRangeResult::BreakLoop) broke = true;
                            else if (bodyResult == RunRangeResult::GotoPending) broke = true;
                            ++iter;
                        }
                        --aiLoopDepth;
                        if (!a.loopVarName.empty()) loopVars_.erase(a.loopVarName);
                        debugAfterAction(i);
                        if (pendingGoto) {
                            const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                            if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                            continue;
                        }
                        i = bodyEnd;
                    } else if (a.type == ActionType::If) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        MacroVariableContext ctx = makeVarCtx();
                        const bool cond = EvaluateConditionExpr(a.conditionExpr, ctx);
                        const int level = a.indent;
                        const size_t trueEnd = containerBodyEnd(i);
                        int elseIdx = -1;
                        for (size_t j = trueEnd; j < activeActions->size(); ++j) {
                            if ((*activeActions)[j].indent < level) break;
                            if ((*activeActions)[j].indent == level && (*activeActions)[j].type == ActionType::If) break;
                            if ((*activeActions)[j].indent == level && (*activeActions)[j].type == ActionType::Else) {
                                elseIdx = static_cast<int>(j);
                                break;
                            }
                        }
                        RunRangeResult branchResult = RunRangeResult::Normal;
                        if (cond) {
                            branchResult = elseIdx >= 0
                                ? runRange(i + 1, static_cast<size_t>(elseIdx))
                                : runRange(i + 1, trueEnd);
                            i = elseIdx >= 0 ? containerBodyEnd(static_cast<size_t>(elseIdx)) : trueEnd;
                        } else if (elseIdx >= 0) {
                            branchResult = runRange(static_cast<size_t>(elseIdx) + 1,
                                containerBodyEnd(static_cast<size_t>(elseIdx)));
                            i = containerBodyEnd(static_cast<size_t>(elseIdx));
                        } else {
                            i = trueEnd;
                        }
                        if (branchResult == RunRangeResult::GotoPending || pendingGoto) {
                            const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                            if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                            continue;
                        }
                        if (branchResult == RunRangeResult::BreakLoop) return RunRangeResult::BreakLoop;
                        debugAfterAction(i);
                    } else if (a.type == ActionType::Else) {
                        i = containerBodyEnd(i);
                    } else if (a.type == ActionType::RunBlock) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        bool runBlockGotoContinue = false;
                        for (int r = 0; r < a.clickCount && !StopRequested() && !scheduledYieldLocalStop; ++r) {
                            const RunRangeResult blockResult = runBlockByName(a.blockName);
                            if (blockResult == RunRangeResult::GotoPending || pendingGoto) {
                                const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                                if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                                runBlockGotoContinue = true;
                                break;
                            }
                            if (ShouldWaitAfterRepeat(a, r) && !StopRequested() && !scheduledYieldLocalStop) {
                                SleepInterruptible(quickscript::ScalePlaybackTimeSeconds(
                                    a.duration + RandomDelay(a.randomDuration), playbackTimeScale));
                            }
                        }
                        if (runBlockGotoContinue) continue;
                        debugAfterAction(i);
                        ++i;
                    } else if (a.type == ActionType::Goto) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        MacroVariableContext ctx = makeVarCtx();
                        int targetNo = 0;
                        if (TryResolveGotoStepNo(a.gotoStepExpr, ctx, targetNo)) {
                            const size_t targetIdx = FindActionIndexByNo(*activeActions, targetNo);
                            if (targetIdx < activeActions->size()) {
                                if (targetIdx < start || targetIdx >= end) {
                                    pendingGoto = targetIdx;
                                    return RunRangeResult::GotoPending;
                                }
                                resolveGotoCursor(targetIdx, i);
                                debugAfterAction(i);
                                continue;
                            }
                        }
                        debugAfterAction(i);
                        ++i;
                    } else {
                        executeWithBreakout(a);
                        if (pendingBreakLoop) {
                            pendingBreakLoop = false;
                            return RunRangeResult::BreakLoop;
                        }
                        debugAfterAction(i);
                        ++i;
                    }
                }
                return RunRangeResult::Normal;
            };
            auto releaseHeldKeys = [&]() {
                if (!heldKeys.empty()) {
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    for (UINT vk : heldKeys) wmSendKey(vk, false);
                    if (markSim) UnmarkSimulatedInput();
                    heldKeys.clear();
                    heldKeyVk = 0;
                } else if (heldKeyVk != 0) {
                    wmSendKey(heldKeyVk, false);
                    heldKeyVk = 0;
                }
            };
            drainScheduledYield = [&]() {
                if (scheduledYieldDepth > 0 || StopRequested()) return;
                std::wstring path;
                if (!TakeScheduledYieldPath(path)) return;
                if (path.empty()) return;
                if (!runningScriptPath.empty()
                    && _wcsicmp(path.c_str(), runningScriptPath.c_str()) == 0) {
                    AppendDebugLog(L"定时插入改为结束后再跑：与当前脚本相同");
                    DeferScheduledPath(path);
                    return;
                }
                const ScriptFileData nestedData = LoadScriptFileData(path, false);
                if (nestedData.actions.empty()) {
                    AppendDebugLog(L"定时插入失败：无法加载脚本");
                    return;
                }
                releaseHeldKeys();
                CoordMeta nestedMeta = ScriptCoordMetaForExecution(nestedData.coordMeta);
                std::vector<ScriptAction> nested =
                    PrepareScriptActionsForExecution(nestedData.actions, nestedMeta);
                if (IsRecordingScriptPath(path) || ScriptIsTimedInputSequence(nested)
                    || nestedData.inputTimingVersion > 0) {
                    RepairCompressedRelativeGaps(nested);
                }
                if (!usesOcr && ScriptUsesTextRecognition(nested)) {
                    usesOcr = true;
                    workerUsesOcrVars_ = true;
                }
                if (usesOcr) holdOcrSession();
                const std::wstring nestedName = [&]() {
                    const auto slash = path.find_last_of(L"\\/");
                    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
                }();
                AppendDebugLog(L"定时插入开始：" + nestedName);
                const std::vector<ScriptAction>* prevActions = activeActions;
                const std::wstring prevPath = runningScriptPath;
                const CoordMeta prevCoordMeta = activeCoordMeta;
                const bool prevTl = inputTimeline.enabled;
                const auto prevPendingGoto = pendingGoto;
                const auto prevLoopEntryGoto = loopEntryGotoTarget;
                const bool prevPendingBreak = pendingBreakLoop;
                const auto prevBlockStack = blockCallStack;
                const int prevAiLoopDepth = aiLoopDepth;
                pendingGoto.reset();
                loopEntryGotoTarget.reset();
                pendingBreakLoop = false;
                blockCallStack.clear();
                aiLoopDepth = 0;
                activeCoordMeta = nestedMeta;
                if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                activeActions = &nested;
                runningScriptPath = path;
                inputTimeline.enabled = false;
                scheduledYieldActive_.store(true, std::memory_order_relaxed);
                ++scheduledYieldDepth;
                scheduledYieldLocalStop = false;
                runRange(0, nested.size());
                --scheduledYieldDepth;
                scheduledYieldLocalStop = false;
                scheduledYieldActive_.store(false, std::memory_order_relaxed);
                pendingGoto = prevPendingGoto;
                loopEntryGotoTarget = prevLoopEntryGoto;
                pendingBreakLoop = prevPendingBreak;
                blockCallStack = prevBlockStack;
                aiLoopDepth = prevAiLoopDepth;
                releaseHeldKeys();
                inputTimeline.enabled = prevTl;
                activeActions = prevActions;
                runningScriptPath = prevPath;
                activeCoordMeta = prevCoordMeta;
                if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                AppendDebugLog(StopRequested()
                    ? L"定时插入中止，不再继续原脚本：" + nestedName
                    : L"定时插入结束，继续原脚本：" + nestedName);
            };
            scheduledYieldHook_ = drainScheduledYield;
            if (debugMode_.load(std::memory_order_relaxed)) {
                // 调试：从指定序号单次执行到最后一个动作，不循环、不受「宏执行次数」影响
                if (debugStepMode_.load(std::memory_order_relaxed)
                    && qst::desktop_tools::MacroDebug().IsCreated()) {
                    qst::desktop_tools::MacroDebug().AppendLog(
                        L"调试开始（单步）：从第 " + std::to_wstring(debugStartIndex_ + 1)
                        + L" 步起，按调试热键单步执行，通用启停热键可终止");
                } else if (qst::desktop_tools::MacroDebug().IsCreated()) {
                    qst::desktop_tools::MacroDebug().AppendLog(
                        L"调试开始（运行）：从第 " + std::to_wstring(debugStartIndex_ + 1)
                        + L" 步起，断点执行后结束，调试热键可暂停/继续");
                }
                // goto 目标在调试起点之前时会向外逃逸：从目标位置重新进入，直到跑完或停止
                size_t debugCursor = static_cast<size_t>(debugStartIndex_);
                while (!StopRequested()) {
                    const RunRangeResult debugResult = runRange(debugCursor, actions.size());
                    if (debugResult != RunRangeResult::GotoPending || !pendingGoto) break;
                    debugCursor = *pendingGoto;
                    pendingGoto.reset();
                    if (debugCursor >= actions.size()) break;
                }
            } else while (!StopRequested()) {
                ++curLoops_;
                inputTimeline.Reset(actions.size());
                timelineInterrupted = false;
                // 每轮独立统计，避免「第2轮 SendInput ok=上轮累计」误导。
                if (inputTimeline.enabled) MouseInputRouter::Instance().ResetStats();
                aiSessions.ClearMacro();
                aiRootBudget = AiStepBudgetState{};
                aiCurFrame = nullptr;
                if (KeyFunctionDebugActive()) {
                    AppendDebugLog(FormatMacroLoopDebug(curLoops_));
                    if (std::abs(playbackTimeScale - 1.0) > 1e-9) {
                        const double spd = 1.0 / playbackTimeScale;
                        wchar_t buf[128]{};
                        swprintf_s(buf, L"回放倍速 %.3gx（等待/间隔已按倍速缩放）", spd);
                        AppendDebugLog(buf);
                    }
                }
                matchVars_.clear();
                if (usesOcr) ocrVars_.clear();
                loopVars_.clear();
                timerStarts_.clear();
                aiVars_.clear();
                ClearImageVars(imageVars_);
                pendingGoto.reset();
                loopEntryGotoTarget.reset();
                runRange(0, actions.size());
                if (inputTimeline.enabled && KeyFunctionDebugActive()) {
                    const auto st = inputTimeline.precision.Stats();
                    const auto ms = MouseInputRouter::Instance().Stats();
                    wchar_t summary[400]{};
                    swprintf_s(summary,
                        L"[时间轴统计] waits=%llu late>1ms=%llu p95=%lluus max=%lluus rebase=%llu | "
                        L"SendInput ok=%llu fail=%llu paced=%llu | "
                        L"ballistics=%s split=%s",
                        static_cast<unsigned long long>(st.eventCount),
                        static_cast<unsigned long long>(st.lateEventCount),
                        static_cast<unsigned long long>(st.p95LateUs),
                        static_cast<unsigned long long>(st.maxLateUs),
                        static_cast<unsigned long long>(st.rebaseCount),
                        static_cast<unsigned long long>(ms.sentEvents),
                        static_cast<unsigned long long>(ms.failedEvents),
                        static_cast<unsigned long long>(ms.pacedWaits),
                        ballisticsGuard.FlatVerified() ? L"flat" : L"accel?",
                        splitLargeMoves ? L"on" : L"off");
                    AppendDebugLog(summary);
                    if (timelineInterrupted || StopRequested()) {
                        AppendDebugLog(
                            StopRequested()
                                ? L"[时间轴] 本轮未跑完：已停止"
                                : L"[时间轴] 本轮未跑完：等待被跳出打断");
                    }
                    // 不在轮间 Flush：1744 行格式化会卡住工作线程数百 ms～数秒，
                    // 多轮 FPS 回放时游戏状态已漂。完整日志在全部结束后一次刷出。
                }
                // 轮间抬起残留按键，避免无限循环时上一轮没松开的键拖进下一轮。
                releaseHeldKeys();
                const auto& ps = appSettings_.playback;
                if (ps.enablePlaybackCount && ps.playbackCount > 0 && curLoops_ >= ps.playbackCount) break;
                if (StopRequested()) break;
                if (ps.enablePlaybackInterval) {
                    const double span = std::max(0.0, ps.playbackIntervalMaxSeconds - ps.playbackIntervalMinSeconds);
                    const double wait = ps.playbackIntervalMinSeconds + RandomDelay(span);
                    SleepInterruptible(wait);
                }
            }
            // Final release (for clean state regardless of stop path)
            releaseHeldKeys();
            ReleaseAllHeldInputs();
            scheduledYieldHook_ = nullptr;
            scheduledYieldActive_.store(false, std::memory_order_relaxed);
            clearLockedScreen();
            ClearImageVars(imageVars_);
            if (ocrSessionHeld) ReleaseOcrSession();
            if (wmCfg.enabled) wmExec.EndRun();
            ForegroundInputRouter::Instance().EndSession();
            } // MouseBallisticsGuard / 优先级：须在 PostMessage(WM_RUN_DONE) 前恢复
            breakout_input::UninstallBreakoutHooks();
            workerUsesOcrVars_ = false;
            // 脚本自然结束 / 热键终止后的最终写回（AI 段曾因路径空推迟）
            if (AiLogicConvertPendingWriteback() || AiLogicConvertSessionActive()) {
                std::wstring summary, err;
                if (TryFlushAiLogicConvertSession(selfPath, summary, err)) {
                    AppendAiDebugLog(L"逻辑转化：已写回脚本 → " + summary + L"（脚本结束）");
                    NotifyLogicConvertUi(selfPath, summary);
                    NotifyAgentScriptLibraryChanged();
                    if (hwnd_ && !selfPath.empty()
                        && _wcsicmp(selfPath.c_str(), currentPath_.c_str()) == 0) {
                        PostMessageW(hwnd_, WM_APP_LOGIC_CONVERT_DONE, 0, 0);
                    }
                } else if (AiLogicConvertPendingWriteback() && !err.empty()) {
                    AppendAiDebugLog(L"逻辑转化：脚本结束时写回失败 — " + err);
                }
                AiLogicConvertSessionEnd();
            }
            // 先刷调试缓冲再通知 UI 结束：否则下一轮回放会和上万行格式化抢 CPU，时间轴追赶连发。
            deferDbgGuard.DisarmNow();
            workerFinished_.store(true, std::memory_order_relaxed);
            PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
        });
    }

// was engine_host_window.h:14250-14300
void EngineHost::RestoreMainWindowAfterRun() {
        if (!hwnd_) return;
        SetWindowCloaked(hwnd_, false);
        breakoutTaskbarShown_ = false;
        breakoutUiVisibleOnScreen_ = false;

        if (headlessUi_) {
            // 引擎宿主始终隐藏；只恢复/保持 Web 壳可见性（对齐 autoHide / 关闭到托盘）
            ShowWindow(hwnd_, SW_HIDE);
            HWND face = (webViewHostHwnd_ && IsWindow(webViewHostHwnd_)) ? webViewHostHwnd_ : nullptr;
            if (face) {
                if (!wasVisibleBeforeRun_) {
                    // 开始前已关闭到托盘（不可见）：结束后保持隐藏，避免与 closeToTray 互踩
                    ShowWindow(face, SW_HIDE);
                } else if (wasMinimizedBeforeRun_) {
                    ShowWindow(face, SW_SHOWMINNOACTIVE);
                } else {
                    ShowWindow(face, SW_SHOWNOACTIVATE);
                }
            }
            breakoutPlacement_ = BreakoutTaskbarPlacement{};
            runSavedRectValid_ = false;
            return;
        }

        const LONG_PTR ex = runSavedRectValid_ ? runSavedExStyle_
            : (breakoutPlacement_.saved ? breakoutPlacement_.exStyle
                : GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);

        RECT rc{};
        if (runSavedRectValid_) rc = runSavedRect_;
        else if (breakoutPlacement_.saved) rc = breakoutPlacement_.rect;
        else GetWindowRect(hwnd_, &rc);

        const int w = std::max(static_cast<int>(rc.right - rc.left), 1);
        const int h = std::max(static_cast<int>(rc.bottom - rc.top), 1);

        if (!wasVisibleBeforeRun_) {
            ShowWindow(hwnd_, SW_HIDE);
        } else if (wasMinimizedBeforeRun_) {
            breakoutTaskbarTransition_ = true;
            if (IsIconic(hwnd_)) {
                ShowWindow(hwnd_, SW_RESTORE);
            }
            SetWindowPos(hwnd_, HWND_BOTTOM, rc.left, rc.top, w, h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            PrepareHwndTaskbarLivePreview(hwnd_);
            DwmFlush();
            ShowWindow(hwnd_, SW_MINIMIZE);
            TaskbarYieldMessages();
            breakoutTaskbarTransition_ = false;
        } else {
            // 自动隐藏后恢复：进任务栏但不抢焦点，避免打断用户当前操作
            SetWindowPos(hwnd_, HWND_BOTTOM, rc.left, rc.top, w, h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            PrepareHwndTaskbarLivePreview(hwnd_);
        }

        breakoutPlacement_ = BreakoutTaskbarPlacement{};
        runSavedRectValid_ = false;
    }

// was engine_host_window.h:14302-14342
void EngineHost::OnRunDone() {
        running_ = false;
        runningFromScheduled_ = false;
        nextRunFromScheduled_ = false;
        scheduledInterruptStop_ = false;
        scheduledYieldActive_.store(false, std::memory_order_relaxed);
        {
            std::wstring leftover;
            if (TakeScheduledYieldPath(leftover)) EnqueueScheduledPath(leftover);
            leftover = TakeDeferredScheduledPath();
            if (!leftover.empty()) EnqueueScheduledPath(leftover);
        }
        // 调试会话收尾：注销调试热键、清空断点/暂停状态（仅缓存，不持久化）
        if (debugMode_.exchange(false, std::memory_order_relaxed)) {
            if (hwnd_ && IsWindow(hwnd_)) {
                UnregisterHotKey(hwnd_, HOTKEY_DEBUG_ID);
            }
            debugStepMode_.store(false, std::memory_order_relaxed);
            debugPaused_.store(false, std::memory_order_relaxed);
            debugStepSignal_.store(false, std::memory_order_relaxed);
            debugBreakpoints_.clear();
            debugActionsPtr_ = nullptr;
            debugBlockEntrySet_.clear();
            debugHotkeyVk_ = 0;
            debugHotkeyMods_ = 0;
            if (debugRestoreUiMute_) {
                // 仅当壳仍停在宏编辑/录制优化时才恢复静音；已回主页则丢弃，否则 F8/录制热键全哑
                debugRestoreUiMute_ = false;
                if (shellUiMode_ == 1 || shellUiMode_ == 2) {
                    ghUiModeHotkeysMuted.store(true, std::memory_order_relaxed);
                }
            }
        }
        // 复位脚本取消闩锁，避免残留 true 误伤其它会话（连点已不再读此标志）。
        ghWorkerCancelFlag = nullptr;
        stopFlag_ = false;
        ghEmergencyStop.store(false, std::memory_order_release);
        ClearAllHoldSessionLatches();
        extRunPending_ = false;
        ForegroundInputRouter::Instance().EndSession();
        ResumeHotkeysAfterPlayback();
        {
            std::lock_guard<std::mutex> lock(extScriptStateMu_);
            runningScriptPath_.clear();
            runningScriptName_.clear();
            runningWindowMode_ = windowmode::DefaultWindowModeConfig();
        }
        windowmode::SetWindowModeLogSink(nullptr);
        executedSteps_.store(0, std::memory_order_relaxed);
        ghHotkeySessionBusy.store(recording_ || clicking_, std::memory_order_relaxed);
        EnsureHotkeyAuxTimers();
        ClearToggleHotkeyLatches();
        EndHighResTimer();
        breakout_input::UninstallBreakoutHooks();
        breakoutHookState_ = BreakoutHookState{};
        workerBreakoutTime_ = 0;
        breakoutUserInput_ = false;
        breakoutPaused_ = false;
        if (worker_.joinable()) worker_.detach();
        if (hwnd_ && IsWindow(hwnd_)) {
            SetWindowCloaked(hwnd_, false);
            BOOL off = FALSE;
            DwmSetWindowAttribute(hwnd_, DWMWA_FORCE_ICONIC_REPRESENTATION, &off, sizeof(off));
            DwmSetWindowAttribute(hwnd_, DWMWA_FREEZE_REPRESENTATION, &off, sizeof(off));
        }
        ApplyMainWindowNormalTaskbarPresentation(hwnd_);
        RestoreMainWindowAfterRun();
        if (hwnd_ && IsWindow(hwnd_) && !wasMinimizedBeforeRun_) {
            if (IsWindowVisible(hwnd_)) {
                ApplyMainWindowNormalTaskbarPresentation(hwnd_);
                RestoreWindowTaskbarLivePreview(hwnd_);
                // 预览/FRAMECHANGED 可能抬升 Z 序：再沉底且不激活
                SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        EnsureTrayIcon();
        HideStatusTip();
        if (hwnd_ && IsWindow(hwnd_) && !pendingScheduledPaths_.empty()) {
            PostMessageW(hwnd_, WM_APP_RUN_SCHEDULED_PENDING, 0, 0);
        }
    }
