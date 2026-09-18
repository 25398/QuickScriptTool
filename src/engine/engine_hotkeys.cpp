// engine_hotkeys.cpp — F2 slice: global/script hotkey register + capture release + OnHotkey
#include "engine/engine_host_window.h"

// was engine_host_window.h:11550-11578
void EngineHost::BeginHotkeyCaptureRelease(const Hotkey& editing) {
        // 捕获期间禁用全部启停热键（含其它脚本的长按），避免边设置边触发宏
        ghHotkeyCaptureOpen = true;
        ghHotkeyCapturePassAll = true;
        ghHotkeyCaptureIgnoreVk = (editing.enabled && editing.vk) ? editing.vk : 0;
        ClearAllHoldSessionLatches();
        ghHotkeyNeedKeyUp = false;
        if (!hwnd_) return;

        // 注销期间卸掉 RegisterHotKey，避免系统热键通道仍触发
        UnregisterHotKey(hwnd_, HOTKEY_GLOBAL_ID);
        for (int i = 0; i < 100; ++i) {
            UnregisterHotKey(hwnd_, HOTKEY_SCRIPT_BASE + i);
            UnregisterHotKey(hwnd_, HOTKEY_RECORDING_BASE + i);
        }
        (void)editing;
    }

// was engine_host_window.h:11599-11604
void EngineHost::EndHotkeyCaptureRelease() {
        ghHotkeyCaptureOpen = false;
        ghHotkeyCapturePassAll = false;
        ghHotkeyCaptureIgnoreVk = 0;
        RegisterAllHotkeys();
    }

// was engine_host_window.h:11606-11714
void EngineHost::RegisterAllHotkeys() {
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
        UnregisterHotKey(hwnd_, HOTKEY_GLOBAL_ID);
        for (int i = 0; i < 100; ++i) {
            UnregisterHotKey(hwnd_, HOTKEY_SCRIPT_BASE + i);
            UnregisterHotKey(hwnd_, HOTKEY_RECORDING_BASE + i);
        }
        // 重建失败表：本轮重新注册成功的会被移除，失败的由 LL 钩子兜底触发
        ClearRegFailIds();

        const bool passMode = appSettings_.other.resolveImeConflict;
        ghPassThroughTypingHotkeys.store(passMode, std::memory_order_relaxed);

        // 重建前保留按住状态，避免启动脚本时 Suspend→Register 清掉 holdActive 导致松不开
        struct SavedHoldState {
            int hotkeyId = 0;
            bool holdArmed = false;
            bool holdActive = false;
            bool holdExtended = false;
            DWORD holdDownTick = 0;
            LONGLONG holdDownQpc = 0;
            uint32_t holdSeq = 0;
            uint32_t holdInvalidSeq = 0;
            DWORD lastToggleTick = 0;
            bool toggleNeedKeyUp = false;
            bool toggleConsumed = false;
        };
        SavedHoldState savedHold[kMaxPlaybackScriptHooks]{};
        int savedHoldCount = 0;
        for (int i = 0; i < ghPlaybackScriptHookCount && savedHoldCount < kMaxPlaybackScriptHooks; ++i) {
            const auto& h = ghPlaybackScriptHooks[i];
            const bool keepHold = h.holdMode
                && (h.holdArmed || h.holdActive || h.holdInvalidSeq != 0);
            if (!keepHold && h.lastToggleTick == 0 && !h.toggleNeedKeyUp && !h.toggleConsumed) continue;
            savedHold[savedHoldCount++] = SavedHoldState{
                h.hotkeyId, h.holdArmed, h.holdActive, h.holdExtended, h.holdDownTick,
                h.holdDownQpc, h.holdSeq, h.holdInvalidSeq, h.lastToggleTick,
                h.toggleNeedKeyUp, h.toggleConsumed};
        }

        // 始终刷新脚本/录制热键表，供回放挂起 / 输入放行 / 长按模式的 LL 钩子使用
        // 含鼠标 VK：回放挂起时 RegisterHotKey 会卸掉，需靠 LL 停宏
        ghPlaybackScriptHookCount = 0;
        for (int i = 0; i < static_cast<int>(scripts_.size()) && i < 100; ++i) {
            if (ghPlaybackScriptHookCount >= kMaxPlaybackScriptHooks) break;
            const auto& hk = scripts_[static_cast<size_t>(i)].hotkey;
            if (!hk.enabled || !hk.vk) continue;
            if (globalHotkey_.enabled && globalHotkey_.vk
                && hk.vk == globalHotkey_.vk && hk.modifiers == globalHotkey_.modifiers) {
                continue;
            }
            ghPlaybackScriptHooks[ghPlaybackScriptHookCount++] = PlaybackScriptHook{
                hk.vk, hk.modifiers, HOTKEY_SCRIPT_BASE + i, hk.holdMode};
        }
        for (int i = 0; i < static_cast<int>(recordings_.size()) && i < 100; ++i) {
            if (ghPlaybackScriptHookCount >= kMaxPlaybackScriptHooks) break;
            const auto& hk = recordings_[static_cast<size_t>(i)].hotkey;
            if (!hk.enabled || !hk.vk) continue;
            if (globalHotkey_.enabled && globalHotkey_.vk
                && hk.vk == globalHotkey_.vk && hk.modifiers == globalHotkey_.modifiers) {
                continue;
            }
            ghPlaybackScriptHooks[ghPlaybackScriptHookCount++] = PlaybackScriptHook{
                hk.vk, hk.modifiers, HOTKEY_RECORDING_BASE + i, hk.holdMode};
        }
        bool anyArmed = ghHotkeyMouseHoldArmed;
        for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
            auto& h = ghPlaybackScriptHooks[i];
            for (int s = 0; s < savedHoldCount; ++s) {
                if (savedHold[s].hotkeyId != h.hotkeyId) continue;
                h.lastToggleTick = savedHold[s].lastToggleTick;
                h.toggleNeedKeyUp = savedHold[s].toggleNeedKeyUp;
                h.toggleConsumed = savedHold[s].toggleConsumed;
                if (!h.holdMode) break;
                h.holdArmed = savedHold[s].holdArmed;
                h.holdActive = savedHold[s].holdActive;
                h.holdExtended = savedHold[s].holdExtended;
                h.holdDownTick = savedHold[s].holdDownTick;
                h.holdDownQpc = savedHold[s].holdDownQpc;
                h.holdSeq = savedHold[s].holdSeq;
                h.holdInvalidSeq = savedHold[s].holdInvalidSeq;
                break;
            }
            if (h.holdMode && IsActiveHoldHotkeyId(h.hotkeyId)) {
                h.holdActive = true;
                h.holdArmed = false;
            }
            if (h.holdArmed) anyArmed = true;
        }
        if (anyArmed) {
            DWORD remain = CurrentHoldThresholdMs();
            if (ghHotkeyMouseHoldArmed && ghHotkeyMouseDownQpc > 0) {
                const DWORD elapsed = HoldElapsedMs(ghHotkeyMouseDownQpc);
                remain = (elapsed >= remain) ? 1 : (remain - elapsed);
            } else {
                for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                    const auto& h = ghPlaybackScriptHooks[i];
                    if (!h.holdArmed || h.holdDownQpc <= 0) continue;
                    const DWORD elapsed = HoldElapsedMs(h.holdDownQpc);
                    const DWORD thr = CurrentHoldThresholdMs();
                    const DWORD r = (elapsed >= thr) ? 1 : (thr - elapsed);
                    if (r < remain) remain = r;
                }
            }
            ScheduleHoldThresholdTimer(remain);
        }

        // 长按（含鼠标长按、左键默认长按语义）必须走 LL；RegisterHotKey 无 KEYUP 且会当成单击切换
        auto skipRegisterHotKeyFor = [&](const Hotkey& hk) {
            if (!hk.vk) return true;
            if (hk.holdMode) return true;
            if (hk.vk == VK_LBUTTON) return true; // NeedsMouseHoldHotkey 恒为 true
            // 与全局启停同键：不注册脚本专属热键（新建脚本默认不设热键；旧文件误写 F8 也不再抢占）
            if (globalHotkey_.enabled && globalHotkey_.vk
                && hk.vk == globalHotkey_.vk && hk.modifiers == globalHotkey_.modifiers) {
                return true;
            }
            return false;
        };

        std::wstring regFail;
        auto tryReg = [&](int id, UINT mods, UINT vk, const std::wstring& label) {
            if (RegisterHotKey(hwnd_, id, mods | MOD_NOREPEAT, vk)) {
                RemoveRegFailId(id);
                return;
            }
            // 注册失败（组合键被其它程序占用 / 跨提权限制）：该热键改由 LL 钩子兜底
            AddRegFailId(id);
            if (!regFail.empty()) regFail += L"；";
            regFail += label;
            regFail += L"注册失败（已转钩子兜底）";
            HotkeyDiagLog("RegisterHotKey failed id=" + std::to_string(id)
                + " vk=" + std::to_string(vk) + " mods=" + std::to_string(mods)
                + " gle=" + std::to_string(GetLastError()));
        };

        // 回放挂起、「中文输入法不触发热键」、壳处于编辑/优化、或输入框聚焦：键盘热键不注册
        // RegisterHotKey（系统会吞键导致打不出字母 / 编辑界面不能误触发）
        const bool uiMuted = ghUiModeHotkeysMuted.load(std::memory_order_relaxed);
        const bool typingMuted = UiTypingHotkeysMuted();
        const bool skipKeyboardRegister = ghPlaybackHotkeySuspended || passMode || uiMuted || typingMuted;
        if (!skipKeyboardRegister) {
            // 全局启停热键必须经 RegisterHotKey 注册（正常模式下 LL 钩子会放行、依赖此通道）。
            // 注意不能用 skipRegisterHotKeyFor：其「与全局启停同键则跳过」规则是为脚本/录制
            // 热键设计的，套用到全局热键自身会把它也跳过，导致 F8 等键盘热键彻底无响应。
            if (globalHotkey_.enabled && globalHotkey_.vk
                && !globalHotkey_.holdMode && globalHotkey_.vk != VK_LBUTTON) {
                tryReg(HOTKEY_GLOBAL_ID, globalHotkey_.modifiers, globalHotkey_.vk, L"全局热键");
            }
            for (int i = 0; i < static_cast<int>(scripts_.size()) && i < 100; ++i) {
                const auto& hk = scripts_[static_cast<size_t>(i)].hotkey;
                if (hk.enabled && hk.vk && !skipRegisterHotKeyFor(hk)) {
                    tryReg(HOTKEY_SCRIPT_BASE + i, hk.modifiers, hk.vk,
                        L"脚本「" + scripts_[static_cast<size_t>(i)].name + L"」");
                }
            }
            for (int i = 0; i < static_cast<int>(recordings_.size()) && i < 100; ++i) {
                const auto& hk = recordings_[static_cast<size_t>(i)].hotkey;
                if (hk.enabled && hk.vk && !skipRegisterHotKeyFor(hk)) {
                    tryReg(HOTKEY_RECORDING_BASE + i, hk.modifiers, hk.vk,
                        L"录制「" + recordings_[static_cast<size_t>(i)].name + L"」");
                }
            }
        } else {
            // 鼠标单击类热键仍可用 RegisterHotKey；长按/键盘走 LL。输入框聚焦时鼠标也不注册。
            if (!ghPlaybackHotkeySuspended && !typingMuted && globalHotkey_.enabled && globalHotkey_.vk
                && IsMouseVk(globalHotkey_.vk) && !skipRegisterHotKeyFor(globalHotkey_)) {
                tryReg(HOTKEY_GLOBAL_ID, globalHotkey_.modifiers, globalHotkey_.vk, L"全局热键");
            }
            if (!ghPlaybackHotkeySuspended && !typingMuted) {
                for (int i = 0; i < static_cast<int>(scripts_.size()) && i < 100; ++i) {
                    const auto& hk = scripts_[static_cast<size_t>(i)].hotkey;
                    if (hk.enabled && hk.vk && IsMouseVk(hk.vk) && !skipRegisterHotKeyFor(hk)) {
                        tryReg(HOTKEY_SCRIPT_BASE + i, hk.modifiers, hk.vk,
                            L"脚本「" + scripts_[static_cast<size_t>(i)].name + L"」");
                    }
                }
                for (int i = 0; i < static_cast<int>(recordings_.size()) && i < 100; ++i) {
                    const auto& hk = recordings_[static_cast<size_t>(i)].hotkey;
                    if (hk.enabled && hk.vk && IsMouseVk(hk.vk) && !skipRegisterHotKeyFor(hk)) {
                        tryReg(HOTKEY_RECORDING_BASE + i, hk.modifiers, hk.vk,
                            L"录制「" + recordings_[static_cast<size_t>(i)].name + L"」");
                    }
                }
            }
        }

        RefreshGlobalHotkeyHooks();
        ghHotkeyHwnd = hwnd_;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        // 始终重装（卸旧再装）：Windows 可能已按 LowLevelHooksTimeout 静默卸载，
        // 仅判空会永远装不回来，表现为热键/捕获整段失效。
        if (ghHotkeyKbHook) { UnhookWindowsHookEx(ghHotkeyKbHook); ghHotkeyKbHook = nullptr; }
        ghHotkeyKbHook = SetWindowsHookExW(WH_KEYBOARD_LL, HotkeyKbProc, inst, 0);
        if (!ghHotkeyKbHook) {
            HotkeyDiagLog("LLKB install failed in RegisterAllHotkeys gle="
                + std::to_string(GetLastError()));
        }
        // 任一鼠标热键（全局/脚本/录制）都需要鼠标 LL（回放挂起 / 长按时靠它启停）
        if (ghHotkeyMouseHook) { UnhookWindowsHookEx(ghHotkeyMouseHook); ghHotkeyMouseHook = nullptr; }
        if (NeedHotkeyMouseHook()) {
            ghHotkeyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, HotkeyMouseProc, inst, 0);
            if (!ghHotkeyMouseHook) {
                HotkeyDiagLog("LLMS install failed in RegisterAllHotkeys gle="
                    + std::to_string(GetLastError()));
            }
        }
        // 钩子安装失败一次性提示（被安全软件拦截时用户能看见原因）
        if (!ghHotkeyKbHook || (NeedHotkeyMouseHook() && !ghHotkeyMouseHook)) {
            if (!ghHotkeyHookWarned) {
                ghHotkeyHookWarned = true;
                HotkeyDiagLog("LL hook unavailable: kb=" + std::to_string(ghHotkeyKbHook != nullptr)
                    + " mouse=" + std::to_string(ghHotkeyMouseHook != nullptr));
                ShowPromptInfo(L"全局热键钩子安装失败，热键可能无响应"
                    L"（多为安全软件拦截全局钩子，请将本程序加入白名单后重启）");
            }
        } else {
            ghHotkeyHookWarned = false;
        }
        // 10s 看门狗：检测静默卸载并重装（忙碌时 1s，避免回放饿死 UI 后停不下来）
        EnsureHotkeyAuxTimers();
        StartHotkeyStopPoller();

        // 注入指纹只盯启停热键 VK（脱离=0 时）；改热键后立刻刷新。
        UINT watched[256]{};
        int watchedN = 0;
        auto pushWatched = [&](UINT vk) {
            if (!vk || watchedN >= 256) return;
            for (int i = 0; i < watchedN; ++i) if (watched[i] == vk) return;
            watched[watchedN++] = vk;
        };
        if (globalHotkey_.enabled) pushWatched(globalHotkey_.vk);
        for (const auto& s : scripts_) {
            if (s.hotkey.enabled) pushWatched(s.hotkey.vk);
        }
        for (const auto& r : recordings_) {
            if (r.hotkey.enabled) pushWatched(r.hotkey.vk);
        }
        synthetic_input::SetWatchedHotkeyVks(watched, watchedN);

        if (!regFail.empty()) {
            lastHotkeyRegisterWarning_ = L"热键注册失败（可能被占用）：" + regFail;
            ShowPromptInfo(lastHotkeyRegisterWarning_);
        } else {
            lastHotkeyRegisterWarning_.clear();
        }
    }

// was engine_host_window.h:11717-11720
void EngineHost::SuspendHotkeysForPlayback() {
        ghPlaybackHotkeySuspended = true;
        RegisterAllHotkeys();
    }

// was engine_host_window.h:11722-11728
void EngineHost::ResumeHotkeysAfterPlayback() {
        ghPlaybackHotkeySuspended = false;
        ClearAllHoldSessionLatches();
        ClearToggleHotkeyLatches();
        ghPlaybackScriptHookCount = 0;
        RegisterAllHotkeys();
    }

// was engine_host_window.h:11730-11748
void EngineHost::InstallGlobalHotkeyHooks() {
        ghHotkeyHwnd      = hwnd_;
        ghHotkeyEnabled   = true;
        ghHotkeyVk        = globalHotkey_.vk;
        ghHotkeyMods      = globalHotkey_.modifiers;
        ghHotkeyHoldMode  = globalHotkey_.holdMode;
        ghHotkeyPending   = false;
        ghHotkeyNeedKeyUp = false;
        ghHotkeyMouseHoldArmed = false;
        ghHotkeyMouseHoldDown = false;
        ghHotkeyMouseDownTick = 0;
        HINSTANCE inst    = GetModuleHandleW(nullptr);
        // 卸旧重装：Windows 可能已静默卸载 LL（LowLevelHooksTimeout），判空装不回来
        if (ghHotkeyKbHook) { UnhookWindowsHookEx(ghHotkeyKbHook); ghHotkeyKbHook = nullptr; }
        ghHotkeyKbHook = SetWindowsHookExW(WH_KEYBOARD_LL, HotkeyKbProc, inst, 0);
        if (!ghHotkeyKbHook) {
            HotkeyDiagLog("LLKB install failed in InstallGlobalHotkeyHooks gle="
                + std::to_string(GetLastError()));
        }
        if (ghHotkeyMouseHook) { UnhookWindowsHookEx(ghHotkeyMouseHook); ghHotkeyMouseHook = nullptr; }
        if (NeedHotkeyMouseHook())
            ghHotkeyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, HotkeyMouseProc, inst, 0);
        if (NeedHotkeyMouseHook() && !ghHotkeyMouseHook) {
            HotkeyDiagLog("LLMS install failed in InstallGlobalHotkeyHooks gle="
                + std::to_string(GetLastError()));
        }
        EnsureHotkeyAuxTimers();
        StartHotkeyStopPoller();
    }

// was engine_host_window.h:11750-11755
void EngineHost::UninstallGlobalHotkeyHooks() {
        StopHotkeyStopPoller();
        CancelHoldThresholdTimer();
        CancelHoldReleaseTimer();
        ghHotkeyEnabled = false;
        KillTimer(hwnd_, kHotkeyLatchSyncTimerId);
        KillTimer(hwnd_, kHotkeyHookWatchdogTimerId);
        KillTimer(hwnd_, kImeHotkeyPassTimerId);
        ghImeHotkeyPassCache.store(false, std::memory_order_relaxed);
        ClearRegFailIds();
        if (ghHotkeyKbHook)     { UnhookWindowsHookEx(ghHotkeyKbHook);     ghHotkeyKbHook     = nullptr; }
        if (ghHotkeyMouseHook)  { UnhookWindowsHookEx(ghHotkeyMouseHook);  ghHotkeyMouseHook  = nullptr; }
    }

// was engine_host_window.h:11757-11789
void EngineHost::RefreshGlobalHotkeyHooks() {
        const UINT prevVk = ghHotkeyVk;
        const bool prevHold = ghHotkeyHoldMode;
        const bool busy = ghHotkeySessionBusy.load(std::memory_order_relaxed);
        const bool keepHoldLatch = (ghHotkeyMouseHoldArmed || ghHotkeyMouseHoldDown
            || IsActiveHoldHotkeyId(HOTKEY_GLOBAL_ID));
        // 启动宏 Suspend→Refresh 时保留 NeedKeyUp，挡住 LL 连发；脚本结束 Resume 会 ClearToggleHotkeyLatches
        const bool keepToggleLatch = busy || ghHotkeyHandling
            || (ghHotkeyNeedKeyUp && ghPlaybackHotkeySuspended);
        ghHotkeyVk      = globalHotkey_.vk;
        ghHotkeyMods    = globalHotkey_.modifiers;
        ghHotkeyHoldMode = globalHotkey_.holdMode;
        ghHotkeyEnabled = globalHotkey_.enabled;
        if (keepHoldLatch && prevVk == ghHotkeyVk && prevHold == ghHotkeyHoldMode && NeedsHoldHotkey()) {
            if (IsActiveHoldHotkeyId(HOTKEY_GLOBAL_ID)) {
                ghHotkeyMouseHoldDown = true;
                ghHotkeyMouseHoldArmed = false;
            } else {
                // ActiveId 已清但 holdDown 残留：清掉僵尸闩锁，否则无法再武装
                ghHotkeyMouseHoldArmed = false;
                ghHotkeyMouseHoldDown = false;
                ghHotkeyMouseDownTick = 0;
            }
        } else if (!keepToggleLatch) {
            ghHotkeyPending = false;
            ghHotkeyNeedKeyUp = false;
            ghHotkeyMouseHoldArmed = false;
            ghHotkeyMouseHoldDown = false;
            ghHotkeyMouseDownTick = 0;
        }
        HINSTANCE inst  = GetModuleHandleW(nullptr);
        if (NeedHotkeyMouseHook() && !ghHotkeyMouseHook)
            ghHotkeyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, HotkeyMouseProc, inst, 0);
        else if (!NeedHotkeyMouseHook() && ghHotkeyMouseHook)
            { UnhookWindowsHookEx(ghHotkeyMouseHook); ghHotkeyMouseHook = nullptr; }
        EnsureHotkeyAuxTimers();
    }

// was engine_host_window.h:11817-11977
void EngineHost::OnHotkey(int id, int holdCmd) {
        // 壳处于宏编辑/录制优化/输入框聚焦：即使队列残留 RegisterHotKey/WM 消息也一律不启动
        if (UiStartHotkeysMuted()
            && !ghHotkeySessionBusy.load(std::memory_order_relaxed)
            && !clicking_ && !recording_ && !running_) return;
        // 看门狗：worker 已自然结束但 WM_RUN_DONE 尚未处理（UI 忙/消息队列卡）时，
        // 自动复位运行态，避免「按热键无反应」（running_ 残留导致热键只走 StopRun）。
        if (running_ && workerFinished_.load(std::memory_order_relaxed)) {
            OnRunDone();
        }
        if (id == HOTKEY_GLOBAL_ID) {
            const bool holdKey = NeedsHoldHotkey();

            // 松开停止：优先处理，不被 Handling 闩锁吞掉
            if (holdKey && holdCmd == static_cast<int>(kHotHoldStop)) {
                ghHotkeyNeedKeyUp = false;
                ghHotkeyPending = false;
                ghHotkeyMouseHoldArmed = false;
                ghHotkeyMouseHoldDown = false;
                const bool dedicatedOwns = DedicatedHoldSessionActive();
                if (!dedicatedOwns) {
                    ClearActiveHoldHotkeyId();
                    ClearPhysicalHoldDownVk(globalHotkey_.vk);
                    BlockHoldRearm(globalHotkey_.vk);
                }
                if (clicking_) { StopClicking(); return; }
                if (recording_) { StopRecording(); return; }
                // 专属长按拥有本次运行：全局松手（含左键连点回声）不得把宏停掉
                if (running_ && dedicatedOwns) return;
                if (running_) { StopRun(); return; }
                return;
            }

            // 忙碌时单击停止：不受 Handling / minGap 挡住。
            // 不得单凭残留的 ghEmergencyStop 在空闲时吞掉下一次启动。
            // 紧急停止已置位时即使启动那次按键的消费闩还粘着也必须停
            // （非管理员 + 提权游戏：UIPI 丢 KEYUP，独占全屏饿死清闩定时器）。
            if (!holdKey && (clicking_ || recording_ || running_)) {
                const bool consumeOk = TryConsumeGlobalTogglePress();
                if (!hotkey_stop::AllowBusyToggleStop(consumeOk,
                        ghEmergencyStop.load(std::memory_order_acquire))) {
                    return;
                }
                ghHotkeyNeedKeyUp = true;
                ghHotkeyPending = false;
                if (clicking_) StopClicking();
                else if (recording_) StopRecording();
                else if (running_) StopRun();
                FlushQueuedGlobalHotkeys(hwnd_);
                return;
            }

            if (ghHotkeyHandling) return;
            SyncHotkeyLatches();

            // 长按达阈值后启动（普通单击热键仍走下方切换）
            if (holdKey && holdCmd == static_cast<int>(kHotHoldStart)) {
                const DWORD now = GetTickCount();
                // 注意：Poll/LL 在 Post Start 前就会置 holdDown/ActiveId，便于队列延迟期间
                // 仍能靠 KEYUP 停；切勿把这两个闩锁当成「已经启动成功」而直接 return，
                // 否则全局长按（含鼠标左键）永远进不了连点/录制/宏。
                // 真正已在跑：忽略重复 Start，保留闩锁以便松手可停
                if (clicking_ || recording_ || running_) {
                    ghHotkeyPending = false;
                    return;
                }
                if (now - lastHotkeyTick_ < 30u) {
                    ghHotkeyPending = false;
                    ghHotkeyMouseHoldArmed = false;
                    ghHotkeyMouseHoldDown = false;
                    ClearActiveHoldHotkeyId();
                    return;
                }
                // 短按抬起已作废本序号：丢弃迟到的 Start（勿用 GetAsyncKeyState）
                if (ghHotkeyHoldPostedSeq != 0
                    && ghHotkeyHoldPostedSeq == ghHotkeyHoldInvalidSeq) {
                    ghHotkeyPending = false;
                    ghHotkeyMouseHoldArmed = false;
                    ghHotkeyMouseHoldDown = false;
                    ClearActiveHoldHotkeyId();
                    return;
                }
                if (ShouldSuppressHotkeyWhileTyping()) {
                    ghHotkeyPending = false;
                    ghHotkeyMouseHoldArmed = false;
                    ghHotkeyMouseHoldDown = false;
                    ClearActiveHoldHotkeyId();
                    return;
                }
                lastHotkeyTick_ = now;
                ghHotkeyHandling = true;
                ghHotkeyNeedKeyUp = false;
                ghHotkeyPending = false;
                SetActiveHoldHotkeyId(HOTKEY_GLOBAL_ID);
                struct HoldStartGuard {
                    bool started = false;
                    ~HoldStartGuard() {
                        // 未真正启动时清掉 holdDown，否则后续按住会被吞掉
                        if (!started) {
                            ghHotkeyMouseHoldArmed = false;
                            ghHotkeyMouseHoldDown = false;
                            ClearActiveHoldHotkeyId();
                        }
                        ghHotkeyNeedKeyUp = false;
                        ghHotkeyPending = false;
                        ghHotkeyHandling = false;
                    }
                } holdGuard;
                if (activeHomeTab_ == quickscript::MainTab::Clicker) {
                    holdGuard.started = true;
                    StartClicking();
                    return;
                }
                holdGuard.started = TryStartByActiveHomeTab();
                return;
            }

            // 长按（含左键默认长按）不走 RegisterHotKey 单击通道
            if (holdKey) return;

            // 单击切换：RegisterHotKey 每次完整按下只投递一次，不能再用 NeedKeyUp 挡掉——
            // 否则「上一轮 NeedKeyUp 未清 + 本轮已按下」会被 Sync 误判成仍未抬起，表现为偶发无响应。
            const DWORD now = GetTickCount();
            const bool busy = clicking_ || recording_ || running_
                || ghHotkeySessionBusy.load(std::memory_order_relaxed);
            const DWORD minGap = busy ? 60u : 80u;
            if (!busy && now - lastHotkeyTick_ < minGap) return;
            if (!TryConsumeGlobalTogglePress()) return;
            lastHotkeyTick_ = now;
            ghHotkeyHandling = true;
            ghHotkeyPending = false;
            // 置位以挡住 Suspend 后 LL 连发；抬起由钩子 KEYUP / Sync 清除（只清不强制保持）
            ghHotkeyNeedKeyUp = true;
            struct HotkeyHandlingGuard {
                HWND hwnd;
                ~HotkeyHandlingGuard() {
                    FlushQueuedGlobalHotkeys(hwnd);
                    SyncHotkeyLatches();
                    ghHotkeyHandling = false;
                }
            } handlingGuard{hwnd_};

            if (clicking_) { StopClicking(); return; }
            if (recording_) { StopRecording(); return; }
            if (running_) { StopRun(); return; }
            // 「中文输入法不触发热键」：中文模式不启动连点/宏/录制（停止不受影响）
            if (ShouldSuppressHotkeyWhileTyping()) return;
            if (activeHomeTab_ == quickscript::MainTab::Clicker) {
                ToggleClicker();
                return;
            }
            TryStartByActiveHomeTab();
            return;
        }
        // 调试热键：单步=执行一步；运行=暂停/继续（仅在调试会话中生效）
        if (id == HOTKEY_DEBUG_ID) {
            if (!debugMode_.load(std::memory_order_relaxed)) return;
            if (debugStepMode_.load(std::memory_order_relaxed)) {
                debugStepSignal_.store(true, std::memory_order_relaxed);
            } else {
                const bool paused = !debugPaused_.load(std::memory_order_relaxed);
                debugPaused_.store(paused, std::memory_order_relaxed);
                if (qst::desktop_tools::MacroDebug().IsCreated()) {
                    qst::desktop_tools::MacroDebug().AppendLog(
                        paused ? L"调试已暂停（再按调试热键继续）" : L"调试继续");
                }
            }
            return;
        }
        // 调试会话中只响应全局启停热键与调试热键；脚本/录制热键不触发
        if (debugMode_.load(std::memory_order_relaxed)) return;
        if (clicking_ || recording_) return;

        // 录制热键（须先于 SCRIPT_BASE：900+ 也满足 >= 800）
        if (id >= HOTKEY_RECORDING_BASE && id < HOTKEY_RECORDING_BASE + 100) {
            const int recIndex = id - HOTKEY_RECORDING_BASE;
            if (recIndex < 0 || recIndex >= static_cast<int>(recordings_.size())) return;
            const bool holdKey = recordings_[static_cast<size_t>(recIndex)].hotkey.holdMode;
            if (holdKey && holdCmd == static_cast<int>(kHotHoldStop)) {
                for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                    if (ghPlaybackScriptHooks[i].hotkeyId == id) {
                        ClearPhysicalHoldDownVk(ghPlaybackScriptHooks[i].vk);
                        BlockHoldRearm(ghPlaybackScriptHooks[i].vk);
                        ghPlaybackScriptHooks[i].holdArmed = false;
                        ghPlaybackScriptHooks[i].holdActive = false;
                        break;
                    }
                }
                ClearActiveHoldHotkeyId();
                if (running_) StopRun();
                return;
            }
            if (holdKey && holdCmd == static_cast<int>(kHotHoldStart)) {
                if (running_) return;
                for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                    auto& h = ghPlaybackScriptHooks[i];
                    if (h.hotkeyId != id) continue;
                    if (h.holdSeq != 0 && h.holdSeq == h.holdInvalidSeq) {
                        ClearActiveHoldHotkeyId();
                        h.holdArmed = false;
                        h.holdActive = false;
                        return;
                    }
                    break;
                }
                if (ShouldSuppressHotkeyWhileTyping()) {
                    ClearActiveHoldHotkeyId();
                    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                        if (ghPlaybackScriptHooks[i].hotkeyId == id) {
                            ghPlaybackScriptHooks[i].holdArmed = false;
                            ghPlaybackScriptHooks[i].holdActive = false;
                            break;
                        }
                    }
                    return;
                }
                SetActiveHoldHotkeyId(id);
                RunRecordingByIndex(recIndex);
                return;
            }
            // 长按热键不走 RegisterHotKey 单击通道；若仍收到 holdCmd==0 则忽略
            if (holdKey) return;
            if (!running_ && ShouldSuppressHotkeyWhileTyping()) return;
            const DWORD now = GetTickCount();
            const bool busy = running_ || ghHotkeySessionBusy.load(std::memory_order_relaxed);
            const DWORD minGap = busy ? 60u : 80u;
            DWORD* tickSlot = nullptr;
            for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                if (ghPlaybackScriptHooks[i].hotkeyId == id) {
                    tickSlot = &ghPlaybackScriptHooks[i].lastToggleTick;
                    break;
                }
            }
            if (!running_ && tickSlot && now - *tickSlot < minGap) return;
            const bool recConsumeOk = TryConsumeScriptTogglePress(id);
            if (running_) {
                if (!hotkey_stop::AllowBusyToggleStop(recConsumeOk,
                        ghEmergencyStop.load(std::memory_order_acquire))) {
                    return;
                }
                StopRun();
                return;
            }
            if (!recConsumeOk) return;
            if (tickSlot) *tickSlot = now;
            SetScriptToggleNeedKeyUp(id, true);
            RunRecordingByIndex(recIndex);
            return;
        }

        if (id >= HOTKEY_SCRIPT_BASE && id < HOTKEY_RECORDING_BASE) {
            const int scriptIndex = id - HOTKEY_SCRIPT_BASE;
            const bool holdKey = scriptIndex >= 0
                && scriptIndex < static_cast<int>(scripts_.size())
                && scripts_[static_cast<size_t>(scriptIndex)].hotkey.holdMode;

            if (holdKey && holdCmd == static_cast<int>(kHotHoldStop)) {
                for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                    if (ghPlaybackScriptHooks[i].hotkeyId == id) {
                        ClearPhysicalHoldDownVk(ghPlaybackScriptHooks[i].vk);
                        BlockHoldRearm(ghPlaybackScriptHooks[i].vk);
                        ghPlaybackScriptHooks[i].holdArmed = false;
                        ghPlaybackScriptHooks[i].holdActive = false;
                        break;
                    }
                }
                ClearActiveHoldHotkeyId();
                if (running_) StopRun();
                return;
            }
            if (holdKey && holdCmd == static_cast<int>(kHotHoldStart)) {
                if (running_) return;
                // 短按抬起已作废：丢弃迟到 Start
                for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                    auto& h = ghPlaybackScriptHooks[i];
                    if (h.hotkeyId != id) continue;
                    if (h.holdSeq != 0 && h.holdSeq == h.holdInvalidSeq) {
                        ClearActiveHoldHotkeyId();
                        h.holdArmed = false;
                        h.holdActive = false;
                        return;
                    }
                    break;
                }
                if (ShouldSuppressHotkeyWhileTyping()) {
                    ClearActiveHoldHotkeyId();
                    for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                        if (ghPlaybackScriptHooks[i].hotkeyId == id) {
                            ghPlaybackScriptHooks[i].holdArmed = false;
                            ghPlaybackScriptHooks[i].holdActive = false;
                            break;
                        }
                    }
                    return;
                }
                SetActiveHoldHotkeyId(id);
                RunScriptByIndex(scriptIndex);
                return;
            }
            // 长按热键不走 RegisterHotKey 单击通道；若仍收到 holdCmd==0 则忽略
            if (holdKey) return;

            // 「中文输入法不触发热键」：中文模式不启动宏（运行中仍可停）
            if (!running_ && ShouldSuppressHotkeyWhileTyping()) return;
            const DWORD now = GetTickCount();
            const bool busy = running_ || ghHotkeySessionBusy.load(std::memory_order_relaxed);
            const DWORD minGap = busy ? 60u : 80u;
            DWORD* tickSlot = nullptr;
            for (int i = 0; i < ghPlaybackScriptHookCount; ++i) {
                if (ghPlaybackScriptHooks[i].hotkeyId == id) {
                    tickSlot = &ghPlaybackScriptHooks[i].lastToggleTick;
                    break;
                }
            }
            if (!running_ && tickSlot && now - *tickSlot < minGap) return;
            const bool scriptConsumeOk = TryConsumeScriptTogglePress(id);
            if (running_) {
                if (!hotkey_stop::AllowBusyToggleStop(scriptConsumeOk,
                        ghEmergencyStop.load(std::memory_order_acquire))) {
                    return;
                }
                StopRun();
                return;
            }
            if (!scriptConsumeOk) return;
            if (tickSlot) *tickSlot = now;
            SetScriptToggleNeedKeyUp(id, true);
            RunScriptByIndex(scriptIndex);
            return;
        }
        if (running_) { StopRun(); return; }
    }

// ── LL 钩子存活看门狗 ─────────────────────────────────────────────
// Windows 会在钩子回调超时（LowLevelHooksTimeout）时**静默卸载** WH_KEYBOARD_LL /
// WH_MOUSE_LL，句柄仍是非空，应用侧无从感知。此时 IME 兼容模式、回放挂起、长按/
// 鼠标热键、Web 捕获等所有 LL 依赖路径全部失效，直到重启——这是「自己机器正常、
// 别的机器热键没反应」的典型来源。看门狗以 Raw 输入（RIDEV_INPUTSINK）为参照：
// 系统确实有输入而 LL 长时间无事件 → 判定已死 → 卸旧重装，并写入诊断日志。
void EngineHost::TickHotkeyHookWatchdog() {
    const HWND hwnd = ghHotkeyHwnd;
    if (!hwnd || !IsWindow(hwnd)) return;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const DWORD now = GetTickCount();

    // 需要的钩子缺失 → 立即补装
    bool kbOk = ghHotkeyKbHook != nullptr;
    if (!kbOk) {
        ghHotkeyKbHook = SetWindowsHookExW(WH_KEYBOARD_LL, HotkeyKbProc, inst, 0);
        kbOk = ghHotkeyKbHook != nullptr;
        if (!kbOk) {
            HotkeyDiagLog("watchdog LLKB install failed gle=" + std::to_string(GetLastError()));
        } else {
            HotkeyDiagLog("watchdog LLKB reinstalled (missing)");
            ghHotkeyKbLastEventTick = now;
        }
    }
    bool mouseNeeded = NeedHotkeyMouseHook();
    bool mouseOk = !mouseNeeded || ghHotkeyMouseHook != nullptr;
    if (mouseNeeded && !mouseOk) {
        ghHotkeyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, HotkeyMouseProc, inst, 0);
        mouseOk = ghHotkeyMouseHook != nullptr;
        if (!mouseOk) {
            HotkeyDiagLog("watchdog LLMS install failed gle=" + std::to_string(GetLastError()));
        } else {
            HotkeyDiagLog("watchdog LLMS reinstalled (missing)");
            ghHotkeyMouseLastEventTick = now;
        }
    }

    // 存活检测：Raw 键盘输入在流、LL 键盘钩子却长期无事件 → 已被静默卸载
    const bool sessionBusy = ghHotkeySessionBusy.load(std::memory_order_relaxed);
    const DWORD rawWindow = sessionBusy ? 2000u : 6000u;
    const DWORD staleWindow = sessionBusy ? 1500u : 6000u;
    const DWORD kbRawActive = (now - ghRawKbLastInputTick) < rawWindow;
    const bool kbStale = (now - ghHotkeyKbLastEventTick) > staleWindow;
    if (kbOk && kbRawActive && kbStale) {
        HotkeyDiagLog("watchdog: keyboard LL dropped (raw active, ll silent), reinstalling");
        UnhookWindowsHookEx(ghHotkeyKbHook);
        ghHotkeyKbHook = SetWindowsHookExW(WH_KEYBOARD_LL, HotkeyKbProc, inst, 0);
        if (!ghHotkeyKbHook) {
            HotkeyDiagLog("watchdog LLKB reinstall failed gle=" + std::to_string(GetLastError()));
        } else {
            ghHotkeyKbLastEventTick = GetTickCount();
        }
    }
    const DWORD msRawActive = (now - ghRawMouseLastInputTick) < rawWindow;
    const bool msStale = (now - ghHotkeyMouseLastEventTick) > staleWindow;
    if (mouseNeeded && mouseOk && msRawActive && msStale) {
        HotkeyDiagLog("watchdog: mouse LL dropped (raw active, ll silent), reinstalling");
        UnhookWindowsHookEx(ghHotkeyMouseHook);
        ghHotkeyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, HotkeyMouseProc, inst, 0);
        if (!ghHotkeyMouseHook) {
            HotkeyDiagLog("watchdog LLMS reinstall failed gle=" + std::to_string(GetLastError()));
        } else {
            ghHotkeyMouseLastEventTick = GetTickCount();
        }
    }

    // 任一钩子最终不可用：一次性提示（避免每 10s 重复打扰）
    const bool anyBroken = !ghHotkeyKbHook || (mouseNeeded && !ghHotkeyMouseHook);
    if (anyBroken && !ghHotkeyHookWarned) {
        ghHotkeyHookWarned = true;
        ShowPromptInfo(L"全局热键钩子不可用，热键可能无响应"
            L"（多为安全软件拦截全局钩子，请将本程序加入白名单后重启）");
    } else if (!anyBroken) {
        ghHotkeyHookWarned = false;
    }
}


