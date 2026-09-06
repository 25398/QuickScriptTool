// engine_window_mode_hooks.cpp — F2 slice extracted from engine_host_window.h
#include "engine/engine_host_window.h"

// was engine_host_window.h:11679-11681
bool EngineHost::IsEditorWindowModeActive() const {
        return popupMode_.sel == 1 || popupMode_.sel == 2;
    }

// was engine_host_window.h:11723-11876
void EngineHost::UpdateEditorWindowModeChrome() {
        if (page_ != Page::Editor) {
            HideEditorMacroHeaderControls();
            return;
        }
        if (labelMacro_) ShowWindow(labelMacro_, SW_SHOW);
        if (name_) ShowWindow(name_, SW_SHOW);
        // mode_ 必须保持隐藏：仅作命中锚点，外观由 PaintEditor/DrawEditorCombo 自绘
        if (mode_) ShowWindow(mode_, SW_HIDE);
        const bool wm = IsEditorWindowModeActive();
        const bool showBreakout = !wm;
        if (labelBreakoutTime_) ShowWindow(labelBreakoutTime_, SW_HIDE);
        if (breakoutTimeEdit_) ShowWindow(breakoutTimeEdit_, showBreakout ? SW_SHOW : SW_HIDE);
        // 下拉锚点控件仅用于命中测试，外观由父窗口自绘
        if (wmSelectMethod_) ShowWindow(wmSelectMethod_, SW_HIDE);
        if (wmSpecifyWindowBtn_) ShowWindow(wmSpecifyWindowBtn_, SW_HIDE);
        // 目标程序行显隐由下方 wm 分支按选择窗口方式决定。
        if (wmTargetPathEdit_) ShowWindow(wmTargetPathEdit_, SW_HIDE);
        if (wmTargetBrowseBtn_) ShowWindow(wmTargetBrowseBtn_, SW_HIDE);
        if (wmTargetCrosshairBtn_) ShowWindow(wmTargetCrosshairBtn_, SW_HIDE);
        if (wmFakeFocusCheck_) ShowWindow(wmFakeFocusCheck_, SW_HIDE);

        const int rowY = ScaleEditorY(kEditorMacroHeaderRowY);
        const int rowH = ScaleEditorH(kEditorMacroHeaderRowH);
        const int comboH = ScaleEditorH(kEditorModeComboH);
        const int comboY = rowY + std::max(0, (rowH - comboH) / 2);
        const int contentLeft = ScaleEditorX(kEditorMacroNameEditX);
        const int gap = ScaleEditorX(kEditorWmHeaderGap);
        const int btnGap = ScaleEditorX(kEditorWmTargetBtnGap);

        if (labelMacro_) {
            MoveWindow(labelMacro_, ScaleEditorX(kEditorMacroNameLabelX), rowY, ScaleEditorW(kEditorMacroNameLabelW), rowH, FALSE);
        }
        const int breakoutRowY = ScaleEditorY(kEditorBreakoutRowY);
        if (breakoutTimeEdit_) {
            MoveWindow(breakoutTimeEdit_, ScaleEditorX(kEditorBreakoutEditX), breakoutRowY,
                ScaleEditorW(kEditorBreakoutEditW), rowH, FALSE);
        }

        const int nameX = contentLeft;
        const int modeW = ScaleEditorW(kEditorModeComboW);
        const int modeX = ScaleEditorX(kEditorComboRight) - modeW;
        const int comboLabelGap = ScaleEditorX(6);
        const int modeLabelW = ScaleEditorW(50);

        if (wm) {
            const bool showSpec = popupWmSelectMethod_.sel == 1;
            // 启动时使用当前所在窗口：运行时再选窗，不显示「目标程序」行。
            const bool showTargetPath = popupWmSelectMethod_.sel == 1
                || popupWmSelectMethod_.sel == 2;
            if (wmSpecifyWindowBtn_) {
                ShowWindow(wmSpecifyWindowBtn_, showSpec ? SW_SHOW : SW_HIDE);
            }
            if (wmTargetPathEdit_) ShowWindow(wmTargetPathEdit_, showTargetPath ? SW_SHOW : SW_HIDE);
            if (wmTargetBrowseBtn_) ShowWindow(wmTargetBrowseBtn_, showTargetPath ? SW_SHOW : SW_HIDE);
            if (wmTargetCrosshairBtn_) {
                ShowWindow(wmTargetCrosshairBtn_, showTargetPath ? SW_SHOW : SW_HIDE);
                if (showTargetPath) {
                    const wchar_t* crossLabel = popupMode_.sel == 2 ? L"准星绑定窗口" : L"准星找程序";
                    SetWindowTextW(wmTargetCrosshairBtn_, crossLabel);
                }
            }

            if (mode_) MoveWindow(mode_, modeX, comboY, modeW, comboH, FALSE);
            const int modeLabelLeft = modeX - comboLabelGap - modeLabelW;
            wmModeLabelRect_ = RECT{modeLabelLeft, rowY, modeX - comboLabelGap, rowY + rowH};

            // 自右向左：模式 | [指定窗口类] | 选择窗口方式 | 宏名称（占满剩余宽度）
            int cursor = modeLabelLeft - gap;
            if (showSpec) {
                const int specW = ScaleEditorW(kEditorWmSpecifyBtnW);
                cursor -= specW;
                if (wmSpecifyWindowBtn_) MoveWindow(wmSpecifyWindowBtn_, cursor, rowY, specW, rowH, FALSE);
                cursor -= gap;
            }

            const int selLabelW = ScaleEditorW(kEditorWmSelectMethodLabelW);
            const int preferredSelW = ScaleEditorW(kEditorWmSelectMethodComboW);
            const int minNameW = ScaleEditorW(72);
            const int minSelW = ScaleEditorW(140);

            // 先保证选择方式文案完整，剩余宽度全部给宏名称。
            int selW = preferredSelW;
            int nameW = cursor - contentLeft - gap - selLabelW - selW;
            if (nameW < minNameW) {
                selW = std::max(minSelW, selW - (minNameW - nameW));
                nameW = cursor - contentLeft - gap - selLabelW - selW;
            }
            nameW = std::max(minNameW, nameW);

            cursor -= selW;
            const int selComboLeft = cursor;
            cursor -= selLabelW;
            wmSelectMethodLabelRect_ = RECT{cursor, rowY, selComboLeft, rowY + rowH};
            if (wmSelectMethod_) MoveWindow(wmSelectMethod_, selComboLeft, comboY, selW, comboH, FALSE);

            cursor -= gap;
            if (name_) MoveWindow(name_, nameX, rowY, nameW, rowH, FALSE);

            if (showTargetPath) {
                const int targetContentRight = ScaleEditorX(kEditorWmContentRight);
                const int targetRowY = rowY + rowH + ScaleEditorY(kEditorWmRowGap);
                const int crossW = ScaleEditorW(kEditorWmTargetCrosshairW);
                const int browseW = ScaleEditorW(kEditorWmTargetBrowseW);
                const bool showFakeFocus = popupMode_.sel == 1 || popupMode_.sel == 2;
                const int ffW = showFakeFocus ? ScaleEditorW(kEditorWmFakeFocusW) : 0;
                // 自右向左：聚焦 | 准星 | 浏览 | 路径（聚焦紧挨准星右侧，同排对齐）
                int right = targetContentRight;
                int ffX = 0;
                if (showFakeFocus) {
                    ffX = right - ffW;
                    right = ffX - btnGap;
                }
                const int crossX = right - crossW;
                const int browseX = crossX - btnGap - browseW;
                const int pathW = std::max(ScaleEditorW(120), browseX - btnGap - contentLeft);

                if (wmTargetPathEdit_) MoveWindow(wmTargetPathEdit_, contentLeft, targetRowY, pathW, rowH, FALSE);
                if (wmTargetBrowseBtn_) MoveWindow(wmTargetBrowseBtn_, browseX, targetRowY, browseW, rowH, FALSE);
                if (wmTargetCrosshairBtn_) MoveWindow(wmTargetCrosshairBtn_, crossX, targetRowY, crossW, rowH, FALSE);
                if (wmFakeFocusCheck_) {
                    ShowWindow(wmFakeFocusCheck_, showFakeFocus ? SW_SHOW : SW_HIDE);
                    if (showFakeFocus) {
                        MoveWindow(wmFakeFocusCheck_, ffX, targetRowY, ffW, rowH, FALSE);
                        SetWindowPos(wmFakeFocusCheck_, HWND_TOP, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    }
                }
            } else if (wmFakeFocusCheck_) {
                // 无目标程序行时：第二行右缘与目标行同一 contentRight，避免压住工具栏。
                const bool showFakeFocus = popupMode_.sel == 1 || popupMode_.sel == 2;
                ShowWindow(wmFakeFocusCheck_, showFakeFocus ? SW_SHOW : SW_HIDE);
                if (showFakeFocus) {
                    const int ffW = ScaleEditorW(kEditorWmFakeFocusW);
                    const int ffY = rowY + rowH + ScaleEditorY(kEditorWmRowGap);
                    const int ffX = ScaleEditorX(kEditorWmContentRight) - ffW;
                    MoveWindow(wmFakeFocusCheck_, ffX, ffY, ffW, rowH, FALSE);
                    SetWindowPos(wmFakeFocusCheck_, HWND_TOP, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
        } else {
            wmModeLabelRect_ = {};
            wmSelectMethodLabelRect_ = {};
            if (wmSpecifyWindowBtn_) ShowWindow(wmSpecifyWindowBtn_, SW_HIDE);
            const int nameW = ScaleEditorW(kEditorMacroNameEditW);
            if (name_) MoveWindow(name_, nameX, rowY, nameW, rowH, FALSE);
            if (mode_) MoveWindow(mode_, modeX, comboY, modeW, comboH, FALSE);
        }
        CenterModernSingleLineEditText(name_);
        CenterModernSingleLineEditText(breakoutTimeEdit_);
        CenterModernSingleLineEditText(wmTargetPathEdit_);
        if (page_ == Page::Editor) InvalidateRect(hwnd_, nullptr, FALSE);
    }

// was engine_host_window.h:11878-11918
void EngineHost::SyncScriptWindowModeFromEditor() {
        if (page_ != Page::Editor) return;
        scriptWindowMode_.enabled = IsEditorWindowModeActive();
        scriptWindowMode_.executionKind = popupMode_.sel == 2
            ? windowmode::WindowModeExecutionKind::BackgroundWindow
            : windowmode::WindowModeExecutionKind::HiddenDesktop;
        if (popupWmSelectMethod_.sel >= 0
            && popupWmSelectMethod_.sel < static_cast<int>(popupWmSelectMethod_.items.size())) {
            scriptWindowMode_.selectMethod =
                windowmode::ComboIndexToSelectMethod(popupWmSelectMethod_.sel);
        }
        if (windowmode::NormalizeSelectMethod(scriptWindowMode_.selectMethod)
            == windowmode::WindowSelectMethod::SelectOnStartup) {
            windowmode::StripRuntimeOnlySelectTarget(scriptWindowMode_);
        } else if (wmTargetPathEdit_) {
            scriptWindowMode_.targetExePath = Trim(GetText(wmTargetPathEdit_));
        }
        // 「不选择窗口」时清掉类名/标题，避免残留身份字段让绑定走错路径。
        if (scriptWindowMode_.selectMethod == windowmode::WindowSelectMethod::NoSelect) {
            scriptWindowMode_.windowName.clear();
            scriptWindowMode_.windowClassName.clear();
            scriptWindowMode_.childWindowClassName.clear();
            scriptWindowMode_.targetWindowTitle.clear();
            scriptWindowMode_.targetPickX = 0;
            scriptWindowMode_.targetPickY = 0;
        }
        // 有目标路径时始终允许自动打开：窗口不存在时由 BeginRun 启动。
        scriptWindowMode_.autoLaunchTarget =
            windowmode::ShouldAutoLaunchTarget(scriptWindowMode_);
        if (!scriptWindowMode_.windowName.empty()) {
            scriptWindowMode_.targetWindowTitle = scriptWindowMode_.windowName;
        }
        if (wmFakeFocusCheck_) {
            const bool wmOn = scriptWindowMode_.executionKind
                    == windowmode::WindowModeExecutionKind::HiddenDesktop
                || scriptWindowMode_.executionKind
                    == windowmode::WindowModeExecutionKind::BackgroundWindow;
            scriptWindowMode_.fakeFocusEnabled =
                IsParamCheckboxChecked(wmFakeFocusCheck_) && wmOn;
        }
        // 注意：切到默认模式时不要清内存/编辑框——手滑切走再切回要能恢复。
        // 持久化清空只在写盘：enabled=0 时 WriteWindowModeJson 落空配置。
    }

// was engine_host_window.h:12039-12178
bool EngineHost::ResolveWindowModeSelectMethod(windowmode::WindowModeScriptConfig& cfg) {
        if (!cfg.enabled) return true;

        using SM = windowmode::WindowSelectMethod;
        cfg.selectMethod = windowmode::NormalizeSelectMethod(cfg.selectMethod);
        const bool background =
            cfg.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow;

        switch (cfg.selectMethod) {
        case SM::MousePositionOnStartup:
        case SM::SelectOnStartup: {
            // 热键/启动瞬间：直接用当前前台聚焦窗口（旧「鼠标位置」已合并）。
            // 从本软件点运行/调试时前台仍是壳：先隐藏再取下一前台窗，否则永远绑到自己。
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &pid);
            bool hidForSelect = false;
            if (pid != 0 && pid == GetCurrentProcessId()) {
                HideUserFacingMainWindow(true);
                hidForSelect = true;
                windowmode::WindowModeLog(
                    L"[窗口模式] 启动时当前所在窗口：本软件在前台，已先隐藏再取下一前台窗");
                fg = GetForegroundWindow();
                pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &pid);
                const DWORD t0 = GetTickCount();
                while (!(fg && IsWindow(fg) && pid != 0 && pid != GetCurrentProcessId())
                    && static_cast<int>(GetTickCount() - t0) < 800) {
                    PumpMessagesFor(30);
                    fg = GetForegroundWindow();
                    pid = 0;
                    if (fg) GetWindowThreadProcessId(fg, &pid);
                }
            }
            if (!fg || !IsWindow(fg)) {
                if (hidForSelect) RestoreMainWindowForUser();
                promptModal_.ShowInfo(L"当前没有聚焦窗口，请先点击目标窗口再按热键启动。");
                return false;
            }
            {
                if (pid != 0 && pid == GetCurrentProcessId()) {
                    if (hidForSelect) RestoreMainWindowForUser();
                    promptModal_.ShowInfo(
                        L"当前聚焦的是本软件窗口，无法作为目标。\n"
                        L"请先点击目标窗口，再按热键启动宏。");
                    return false;
                }
            }
            RECT rc{};
            if (!GetWindowRect(fg, &rc)) {
                if (hidForSelect) RestoreMainWindowForUser();
                promptModal_.ShowInfo(L"未能获取当前聚焦窗口信息。");
                return false;
            }
            const int cx = (rc.left + rc.right) / 2;
            const int cy = (rc.top + rc.bottom) / 2;
            const auto info = GetWindowInfoFromPoint(cx, cy);
            if (info.windowClassName.empty() && info.windowTitle.empty()
                && info.processPath.empty()) {
                if (hidForSelect) RestoreMainWindowForUser();
                promptModal_.ShowInfo(L"未能获取当前聚焦窗口信息。");
                return false;
            }
            ApplyWindowInfoToConfig(cfg, info);
            cfg.autoLaunchTarget = false;
            break;
        }
        case SM::UseEditorWindowClass: {
            if (cfg.windowClassName.empty()
                && cfg.windowName.empty()
                && cfg.targetWindowTitle.empty()
                && cfg.targetPickX == 0 && cfg.targetPickY == 0) {
                promptModal_.ShowInfo(
                    L"请先点击「指定窗口类」配置目标窗口，或改用其他选择窗口方式。");
                return false;
            }
            break;
        }
        case SM::NoSelect: {
            cfg.windowName.clear();
            cfg.windowClassName.clear();
            cfg.childWindowClassName.clear();
            cfg.targetWindowTitle.clear();
            cfg.targetPickX = 0;
            cfg.targetPickY = 0;
            break;
        }
        }

        // 身份解析完成后再做模式级校验。
        const bool hasIdentity = !cfg.windowClassName.empty()
            || !cfg.windowName.empty()
            || !cfg.targetWindowTitle.empty()
            || cfg.targetPickX != 0 || cfg.targetPickY != 0;

        if (cfg.selectMethod == SM::NoSelect) {
            if (cfg.targetExePath.empty()) {
                promptModal_.ShowInfo(L"请填写目标程序路径，可使用「浏览」或「准星找程序」。");
                return false;
            }
        } else if (cfg.selectMethod == SM::UseEditorWindowClass) {
            // 窗口不存在时要能自动打开，因此需要目标程序路径。
            if (cfg.targetExePath.empty()) {
                promptModal_.ShowInfo(
                    L"请填写目标程序路径（窗口未打开时将自动启动），或用「指定窗口类」拾取带路径的窗口。");
                return false;
            }
        } else if (!background && !hasIdentity && cfg.targetExePath.empty()) {
            promptModal_.ShowInfo(L"未获取到目标窗口信息。");
            return false;
        }

        if (background
            && cfg.targetExePath.empty()
            && !hasIdentity) {
            promptModal_.ShowInfo(
                L"后台窗口模式请用「启动时使用当前所在窗口」「准星绑定窗口」或「指定窗口类」选择目标窗口。");
            return false;
        }

        FinalizeWindowModeAutoLaunch(cfg);
        // 再次强制：防止上游 JSON / 旧字段把 autoLaunch 关掉。
        cfg.autoLaunchTarget = windowmode::ShouldAutoLaunchTarget(cfg);
        if (windowmode::UsesFakeFocus(cfg)) {
            // 假焦点禁止任何前台抢焦点 fallback。
            cfg.allowForegroundInputFallback = false;
        } else if (!cfg.allowForegroundInputFallback) {
            cfg.allowForegroundInputFallback = appSettings_.windowMode.allowForegroundInputFallback;
        }
        return true;
    }

// was engine_host_window.h:12180-12186
bool EngineHost::PrepareWindowModeRunConfig(windowmode::WindowModeScriptConfig& cfg) {
        SyncScriptWindowModeFromEditor();
        cfg = scriptWindowMode_;
        bool anyRel = cfg.windowRelativeCoordinates;
        for (const auto& a : actions_) {
            if (a.windowRelative) { anyRel = true; break; }
        }
        windowmode::FinalizeWindowModeForPlayback(cfg, anyRel, false);
        if (!ResolveWindowModeSelectMethod(cfg)) return false;
        cfg.autoLaunchTarget = windowmode::ShouldAutoLaunchTarget(cfg);
        return true;
    }

