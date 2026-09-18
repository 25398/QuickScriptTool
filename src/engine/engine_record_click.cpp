// engine_record_click.cpp — F2 slice extracted from engine_host_window.h
#include "engine/engine_host_window.h"
#include "clicker_timing.h"
#include "window_mode/window_target.h"

// was engine_host_window.h:14652-14654
void EngineHost::ToggleRecording() {
        if (recording_) { StopRecording(); } else { StartRecording(); }
    }

namespace {

// 顶层窗口在给定点是否属于本软件自身（截图/调试/代理窗）。
bool IsOwnPointWindow(int x, int y) {
    HWND h = WindowFromPoint(POINT{x, y});
    if (!h) return false;
    HWND root = GetAncestor(h, GA_ROOT);
    if (!root) root = h;
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    if (pid == GetCurrentProcessId()) return true;
    return false;
}

// 窗口模式录制：解析录制开始时鼠标位置的最上层目标窗口及其输入子窗口。
RecordingWindowTarget ResolveRecordingWindowTarget() {
    RecordingWindowTarget out;
    POINT pt{};
    GetCursorPos(&pt);
    if (IsOwnPointWindow(pt.x, pt.y)) {
        // 光标在本软件上：退而取前台非本软件窗口。
        HWND fg = GetForegroundWindow();
        if (fg && IsWindow(fg)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            if (pid != GetCurrentProcessId()) {
                RECT frc{};
                POINT fpt = pt;
                if (GetWindowRect(fg, &frc)) {
                    fpt.x = (frc.left + frc.right) / 2;
                    fpt.y = (frc.top + frc.bottom) / 2;
                }
                WindowInfoFromPoint info = GetWindowInfoFromPoint(fpt.x, fpt.y);
                out.exePath = info.processPath;
                out.windowTitle = info.windowTitle;
                out.windowClassName = info.windowClassName;
                out.childWindowClassName = info.childWindowClassName;
                HWND top = fg;
                HWND input = top;
                if (!out.childWindowClassName.empty()) {
                    HWND child = windowmode::FindChildWindowByClass(top, out.childWindowClassName);
                    if (child && IsWindow(child)) input = child;
                }
                out.hwnd = input;
                RECT rc{};
                if (GetClientRect(input, &rc)) {
                    out.clientW = static_cast<int>(rc.right - rc.left);
                    out.clientH = static_cast<int>(rc.bottom - rc.top);
                }
                out.enabled = out.hwnd != nullptr && out.clientW > 0 && out.clientH > 0;
                return out;
            }
        }
        return out;
    }

    WindowInfoFromPoint info = GetWindowInfoFromPoint(pt.x, pt.y);
    HWND top = info.windowClassName.empty() ? nullptr
        : GetAncestor(WindowFromPoint(pt), GA_ROOT);
    if (!top) top = WindowFromPoint(pt);
    if (!top) return out;
    HWND input = top;
    if (!info.childWindowClassName.empty()) {
        HWND child = windowmode::FindChildWindowByClass(top, info.childWindowClassName);
        if (child && IsWindow(child)) input = child;
    }
    out.hwnd = input;
    RECT rc{};
    if (GetClientRect(input, &rc)) {
        out.clientW = static_cast<int>(rc.right - rc.left);
        out.clientH = static_cast<int>(rc.bottom - rc.top);
    }
    out.exePath = info.processPath;
    out.windowTitle = info.windowTitle;
    out.windowClassName = info.windowClassName;
    out.childWindowClassName = info.childWindowClassName;
    out.enabled = out.hwnd != nullptr && out.clientW > 0 && out.clientH > 0;
    return out;
}

}  // namespace

// was engine_host_window.h:14656-14725
void EngineHost::StartRecording() {
        // 录制时只忽略「启停热键」（按下用于停止录制，且不应写进脚本）；
        // 项目里其它脚本/录制的热键（含长按热键）必须原样录下来，
        // 否则录制中按到这些键会被吞掉，生成的动作列表缺键。
        if (globalHotkey_.enabled && globalHotkey_.vk) {
            SetRecordingIgnoreHotkey(globalHotkey_.modifiers, globalHotkey_.vk, true);
        } else {
            SetRecordingIgnoreHotkey(0, 0, false);
        }
        // 勿用编辑器残留的 scriptWindowMode_ 弹「窗口模式无法录制」（易误导）。
        // 窗口相对录制由 recorderWindowMode_ + SetRecordingWindowTarget 决定；
        // 保存时 SaveScriptFileData 仅对「非窗口相对」录制强制关 windowMode。
        recorderWindowMode_ = appSettings_.home.recorderWindowMode != 0;
        const auto inputMode = recorderSettings_.inputMode;
        const auto requestedMode = inputMode == quickscript::RecorderInputMode::DesktopAbsolute
            ? RecordingCaptureMode::Absolute
            : (inputMode == quickscript::RecorderInputMode::FpsRelative
                ? RecordingCaptureMode::Relative
                : (inputMode == quickscript::RecorderInputMode::ImageLocate
                    ? RecordingCaptureMode::Auto   // 图片定位：按自动采集（绝对优先），相对事件不截图
                    : RecordingCaptureMode::Auto));
        SetRecordingCaptureMode(requestedMode);
        // 图片定位：默认开启点击截图（找图转换素材）；相对事件在采集侧跳过
        const bool imageLocate = inputMode == quickscript::RecorderInputMode::ImageLocate;
        SetRecordingClickCaptureConfig(
            imageLocate ? true : appSettings_.playback.recordingClickCaptureEnabled,
            appSettings_.playback.recordingClickCaptureHalfSize);
        SetRecordingClickCaptureSkipRelative(imageLocate);
        // 窗口模式录制：解析目标窗口；坐标按客户区记录（跟随窗口回放）。
        // 可与图片定位同时开：模板在点击送达目标前截取（悬停缓存 / 钩子内同步截）。
        if (recorderWindowMode_) {
            const auto wmTarget = ResolveRecordingWindowTarget();
            if (!wmTarget.enabled) {
                lastRecordingError_ = L"窗口模式录制：未找到目标窗口，请将鼠标移到目标窗口上再开始录制。";
                ShowPromptInfo(lastRecordingError_);
                return;
            }
            SetRecordingWindowTarget(wmTarget);
            recordingWmTarget_ = wmTarget;
        } else {
            SetRecordingWindowTarget({});
            recordingWmTarget_ = {};
        }
        BeginHighResTimer();
        if (!InstallRecordingHooks()) {
            UninstallRecordingHooks();
            EndHighResTimer();
            SetRecordingIgnoreHotkey(0, 0, false);
            lastRecordingError_ = requestedMode != RecordingCaptureMode::Absolute
                ? L"高精度录制启动失败：Raw Input 初始化失败。"
                : L"录制钩子初始化失败。";
            ShowPromptInfo(lastRecordingError_);
            return;
        }
        InitRecordingClock();
        ClearRecordingDebugStats();
        if (appSettings_.playback.enableDebugOutputWindow
            && appSettings_.playback.autoOutputKeyFunctionDebug) {
            ShowDebugWindow();
            qst::desktop_tools::MacroDebug().ClearLog();
            SetRecordingDebugSink([this](const std::wstring& line) {
                if (qst::desktop_tools::MacroDebug().IsCreated()) qst::desktop_tools::MacroDebug().AppendLog(line);
            });
            const wchar_t* modeName =
                imageLocate ? L"图片定位"
                : (requestedMode == RecordingCaptureMode::Relative ? L"相对坐标"
                    : (requestedMode == RecordingCaptureMode::Absolute ? L"绝对坐标" : L"自动识别"));
            qst::desktop_tools::MacroDebug().AppendLog(
                std::wstring(L"开始键鼠录制 模式=") + modeName
                + (GetRecordingWindowTarget().enabled ? L" 窗口相对" : L" 全屏"));
        } else {
            SetRecordingDebugSink({});
        }
        {
            std::lock_guard<std::mutex> lock(g_recordMutex);
            g_recordedEvents.clear();
            const bool startsRelative = requestedMode == RecordingCaptureMode::Relative
                || (requestedMode == RecordingCaptureMode::Auto && IsRelativeMouseCaptureActive());
            if (!startsRelative) {
            RecordedEvent initialPos{};
                initialPos.timeOffsetUs = 0;
                initialPos.sequence = 0;
                initialPos.msg = WM_MOUSEMOVE;
                initialPos.source = RecordedEventSource::Synthetic;
                POINT pt{}; GetCursorPos(&pt);
                // 窗口相对录制：初始位置也按客户区记录；光标不在目标窗口内则跳过
                // （首个窗口内事件会建立起点）。
                const auto wmTgt = GetRecordingWindowTarget();
                int ipx = pt.x, ipy = pt.y;
                if (wmTgt.enabled && !MapRecordingPointToClientIfWindowRelative(ipx, ipy)) {
                    // 不记录初始位置
                } else {
                    initialPos.x = ipx;
                    initialPos.y = ipy;
                    g_recordedEvents.push_back(initialPos);
                }
            }
        }
        g_recording = true;
        recording_ = true;
        ghHotkeySessionBusy.store(true, std::memory_order_relaxed);
        ghEmergencyStop.store(false, std::memory_order_release);
        EnsureHotkeyAuxTimers();
        recordingWasVisible_ = false;
        if (HWND face = UserFacingMainHwnd()) {
            recordingWasVisible_ = (IsWindowVisible(face) == TRUE);
        }
        CloseEditorPopup(); CancelQuickInputTip();
        if (appSettings_.other.playSoundOnStart) PlayAppStartupSound();
        if (appSettings_.other.autoHideMainWindow) {
            AddTray();
            HideUserFacingMainWindow(false);
        } else {
            KeepEngineHostHiddenIfHeadless();
        }
        UpdateStatusTip();
    }

// was engine_host_window.h:14727-14776
void EngineHost::StopRecording() {
        recording_ = false;
        ghHotkeySessionBusy.store(clicking_ || running_, std::memory_order_relaxed);
        EnsureHotkeyAuxTimers();
        ghHotkeyPending = false;
        SetRecordingIgnoreHotkey(0, 0, false);
        // 先卸钩并冲刷 Raw 累计（此时仍保持 g_recording，避免丢最后一段相对位移）
        UninstallRecordingHooks();
        g_recording = false;
        SetRecordingDebugSink({});
        FlushClickCaptures(3000);
        EndHighResTimer();
        RemoveTray();
        HideStatusTip();
        if (appSettings_.other.autoHideMainWindow && recordingWasVisible_) {
            ShowUserFacingMainWindow(SW_SHOW);
        }
        KeepEngineHostHiddenIfHeadless();
        ConvertRecordedToActions();
        // 图片定位：自动把带截图的点击转为找图点击（同「转为找图点击」全量转换），
        // 并吸收点击前的前置移动；相对事件无截图自然跳过。
        if (recorderSettings_.inputMode == quickscript::RecorderInputMode::ImageLocate) {
            ConvertToFindImageOptions copt{};
            copt.requireCapturePath = true;
            copt.findTimeExpr = L"0";
            const ConvertToFindImageResult conv =
                ConvertActionsToFindImage(actions_, 0, static_cast<int>(actions_.size()), copt);
            RenumberScriptActions(actions_);
            if (qst::desktop_tools::MacroDebug().IsCreated()) {
                qst::desktop_tools::MacroDebug().AppendLog(
                    L"图片定位：已自动转换找图点击 成功=" + std::to_wstring(conv.converted)
                    + L" 跳过=" + std::to_wstring(conv.skipped));
            }
        }
        if (appSettings_.playback.enableDebugOutputWindow
            && appSettings_.playback.autoOutputKeyFunctionDebug
            && qst::desktop_tools::MacroDebug().IsCreated()) {
            const auto st = GetRecordingDebugStats();
            wchar_t summary[320]{};
            swprintf_s(summary,
                L"[录制结束] 时长=%.3fs 动作=%zu | "
                L"键↓%llu ↑%llu 跳过重复%llu | 鼠↓%llu ↑%llu 滚轮%llu | "
                L"绝对移动%llu 相对移动%llu",
                saveDurationSeconds_,
                actions_.size(),
                static_cast<unsigned long long>(st.keyDown),
                static_cast<unsigned long long>(st.keyUp),
                static_cast<unsigned long long>(st.keyRepeatSkipped),
                static_cast<unsigned long long>(st.mouseDown),
                static_cast<unsigned long long>(st.mouseUp),
                static_cast<unsigned long long>(st.wheel),
                static_cast<unsigned long long>(st.absMove),
                static_cast<unsigned long long>(st.relMove));
            qst::desktop_tools::MacroDebug().AppendLog(summary);
            std::vector<std::wstring> lines;
            lines.reserve(actions_.size());
            for (size_t i = 0; i < actions_.size(); ++i) {
                actions_[i].originalNo = static_cast<int>(i + 1);
                lines.push_back(FormatGenericActionDebug(actions_[i]));
            }
            if (!lines.empty()) qst::desktop_tools::MacroDebug().AppendLogBatch(lines);
        }
        if (!actions_.empty()) {
            SaveRecording();
        } else {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        // 窗口相对录制状态仅本次录制有效：保存完成后清空，避免影响后续普通录制。
        SetRecordingWindowTarget({});
        recordingWmTarget_ = {};
        ClearEmergencyStopIfIdle(clicking_, false, running_);
        UpdateStatusTip();
    }

// was engine_host_window.h:14819-14899
void EngineHost::StartClicking() {
        if (clicking_) return;
        clicking_ = true;
        ghHotkeySessionBusy.store(true, std::memory_order_relaxed);
        ghEmergencyStop.store(false, std::memory_order_release);
        EnsureHotkeyAuxTimers();
        clickCountDone_ = 0;
        if (appSettings_.other.playSoundOnStart) PlayAppStartupSound();
        if (appSettings_.other.autoHideMainWindow) {
            AddTray();
            HideUserFacingMainWindow(false);
        } else {
            KeepEngineHostHiddenIfHeadless();
        }
        UpdateStatusTip();
        // 与录制回放/鼠标宏共用设置里的注入方式（系统模拟 / Interception / 虚拟HID）。
        const auto clickBackend = appSettings_.playback.foregroundInputBackend;
        // 快照连点配置：避免工作线程每轮读 clickerSettings_/appSettings_.click
        // 与 UI 线程改设置构成数据竞争（撕裂 double/结构体赋值）。
        const auto clickerCfg = clickerSettings_;
        const auto clickCfg = appSettings_.click;
        clickerThread_ = std::thread([this, clickBackend, clickerCfg, clickCfg]() {
            ForegroundInputRouter::Instance().BeginSession(clickBackend);
            BeginHighResTimer();
            struct SessionGuard {
                ~SessionGuard() {
                    EndHighResTimer();
                    ForegroundInputRouter::Instance().EndSession();
                }
            } sessionGuard;

            MouseButtonType button = MouseButtonType::Left;
            if (clickerCfg.button == quickscript::MouseButtonChoice::Right) {
                button = MouseButtonType::Right;
            } else if (clickerCfg.button == quickscript::MouseButtonChoice::Middle) {
                button = MouseButtonType::Middle;
            }

            const auto& cs = clickCfg;
            const uint64_t holdUs = ClickerHoldUs(
                cs.enablePressReleaseInterval, cs.pressReleaseIntervalSeconds);
            PrecisionInputTimeline clock;
            const auto cancelled = [this] {
                return !clicking_ || ghEmergencyStop.load(std::memory_order_relaxed);
            };
            // 只用 clicking_ 控制启停。勿读 stopFlag_：那是脚本取消闩锁，
            // stopMacro / StopRun 后常保持 true，会导致「界面显示连点中但从不点」。
            while (clicking_ && !ghEmergencyStop.load(std::memory_order_relaxed)) {
                double interval = 0.1;
                switch (clickerCfg.intervalMode) {
                case quickscript::ClickIntervalMode::Custom:
                    interval = std::max(0.001, clickerCfg.customIntervalSeconds);
                    break;
                case quickscript::ClickIntervalMode::Efficient:
                    interval = 0.1;
                    break;
                case quickscript::ClickIntervalMode::Extreme:
                    interval = 0.01;
                    break;
                }
                if (cs.enableRandomInterval) interval += RandomDelay(cs.randomIntervalMaxSeconds);
                const uint64_t gapUs = ClickerGapUs(interval);

                if (cs.enableFixedCoordinates || cs.enableCoordinateJitter) {
                    int clickX = 0, clickY = 0;
                    if (cs.enableFixedCoordinates) {
                        clickX = cs.fixedX;
                        clickY = cs.fixedY;
                    } else {
                        POINT pt{}; GetCursorPos(&pt);
                        clickX = pt.x;
                        clickY = pt.y;
                    }
                    if (cs.enableCoordinateJitter) {
                        clickX += RandomInt(cs.jitterX);
                        clickY += RandomInt(cs.jitterY);
                    }
                    SetCursorScreenPos(clickX, clickY);
                }

                MouseButtonEvent(button, true);
                // 与脚本 SleepInterruptible 隔离：后者读 stopFlag_，宏结束后常为 true。
                // 必须先撑满 hold 再抬起：旧实现 int(秒*1000)+sleep(5ms) 会把 0.001s 截成 0ms，
                // down/up 粘在同一时刻，目标程序看不到一次完整点击。
                clock.WaitGapUs(holdUs, cancelled);
                MouseButtonEvent(button, false);

                ++clickCountDone_;
                if (cs.enableClickCountLimit && cs.clickCountLimit > 0 && clickCountDone_ >= cs.clickCountLimit) {
                    PostMessageW(hwnd_, WM_HOTKEY, HOTKEY_GLOBAL_ID, 0);
                    break;
                }

                if (!clock.WaitGapUs(gapUs, cancelled)) break;
            }
        });
    }

// was engine_host_window.h:14901-14914
void EngineHost::StopClicking() {
        clicking_ = false;
        ClearAllHoldSessionLatches();
        ghHotkeySessionBusy.store(recording_ || running_, std::memory_order_relaxed);
        EnsureHotkeyAuxTimers();
        ghHotkeyPending = false;
        if (clickerThread_.joinable()) {
            HANDLE h = reinterpret_cast<HANDLE>(clickerThread_.native_handle());
            if (h && WaitForSingleObject(h, 800) == WAIT_OBJECT_0) {
                clickerThread_.join();
            } else {
                ForegroundInputRouter::Instance().ForceTeardown();
                if (h && WaitForSingleObject(h, 400) == WAIT_OBJECT_0) {
                    clickerThread_.join();
                } else {
                    clickerThread_.detach();
                }
            }
        }
        ClearEmergencyStopIfIdle(false, recording_, running_);
        RemoveTray();
        HideStatusTip();
        if (appSettings_.other.autoHideMainWindow) {
            ShowUserFacingMainWindow(SW_SHOW);
        }
        KeepEngineHostHiddenIfHeadless();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

