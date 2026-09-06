// engine_gdi_editor.cpp — F2 slice extracted from engine_host_window.h
#include "engine/engine_host_window.h"

// was engine_host_window.h:1623-1722
void EngineHost::CreateEditorControls() {
        labelMacro_ = MakeLabel(hwnd_, L"宏名称:", -1, kEditorMacroNameLabelX, kEditorMacroHeaderRowY, kEditorMacroNameLabelW, kEditorMacroHeaderRowH);
        editorControls_.push_back(labelMacro_);
        name_ = MakeEdit(hwnd_, L"", kScriptName, kEditorMacroNameEditX, kEditorMacroHeaderRowY, kEditorMacroNameEditW, kEditorMacroHeaderRowH);
        SendMessageW(name_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        editorControls_.push_back(name_);
        labelBreakoutTime_ = MakeLabel(hwnd_, L"脱离时间:", -1, kEditorBreakoutLabelX, kEditorBreakoutRowY, kEditorBreakoutLabelW, kEditorMacroHeaderRowH);
        editorControls_.push_back(labelBreakoutTime_);
        breakoutTimeEdit_ = MakeEdit(hwnd_, L"0", kBreakoutTime, kEditorBreakoutEditX, kEditorBreakoutRowY, kEditorBreakoutEditW, kEditorMacroHeaderRowH);
        SendMessageW(breakoutTimeEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        editorControls_.push_back(breakoutTimeEdit_);
        ShowWindow(labelMacro_, SW_HIDE);
        ShowWindow(name_, SW_HIDE);
        ShowWindow(labelBreakoutTime_, SW_HIDE);
        ShowWindow(breakoutTimeEdit_, SW_HIDE);
        mode_ = MakeLabel(hwnd_, L"默认模式", kModeCombo,
            kEditorComboRight - kEditorModeComboW, kEditorMacroHeaderRowY, kEditorModeComboW, kEditorModeComboH);
        editorControls_.push_back(mode_);
        popupMode_.items = {L"默认模式", L"窗口模式", L"后台窗口模式"}; popupMode_.sel = 0;
        popupWmSelectMethod_.items = {
            L"启动时使用当前所在窗口",
            L"使用宏编辑时获取到的窗口类名",
            L"不选择窗口"
        };
        popupWmSelectMethod_.sel = 0;
        wmSelectMethod_ = MakeLabel(hwnd_, L"启动时使用当前所在窗口", kWmSelectMethod,
            560, kEditorMacroHeaderRowY, kEditorWmSelectMethodComboW, kEditorModeComboH);
        editorControls_.push_back(wmSelectMethod_);
        wmSpecifyWindowBtn_ = MakeGreenButton(hwnd_, L"指定窗口类", kWmSpecifyWindowBtn, 740,
            kEditorMacroHeaderRowY, kEditorWmSpecifyBtnW, kEditorMacroHeaderRowH);
        editorControls_.push_back(wmSpecifyWindowBtn_);
        ShowWindow(wmSelectMethod_, SW_HIDE);
        ShowWindow(wmSpecifyWindowBtn_, SW_HIDE);
        wmTargetPathEdit_ = MakeEdit(hwnd_, L"", kWmTargetPath, 83, 84, 400, kEditorMacroHeaderRowH);
        SendMessageW(wmTargetPathEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        editorControls_.push_back(wmTargetPathEdit_);
        wmTargetBrowseBtn_ = MakeGrayButton(hwnd_, L"浏览", kWmTargetBrowse, 0, 84, kEditorWmTargetBrowseW, kEditorMacroHeaderRowH);
        editorControls_.push_back(wmTargetBrowseBtn_);
        wmTargetCrosshairBtn_ = MakeGreenButton(hwnd_, L"准星找程序", kWmTargetCrosshair, 0, 84, kEditorWmTargetCrosshairW, kEditorMacroHeaderRowH);
        editorControls_.push_back(wmTargetCrosshairBtn_);
        ShowWindow(wmTargetPathEdit_, SW_HIDE);
        ShowWindow(wmTargetBrowseBtn_, SW_HIDE);
        ShowWindow(wmTargetCrosshairBtn_, SW_HIDE);
        wmFakeFocusCheck_ = MakeCheckBox(hwnd_, L"聚焦", kWmFakeFocus, 0, 84, kEditorWmFakeFocusW, kEditorMacroHeaderRowH);
        editorControls_.push_back(wmFakeFocusCheck_);
        ShowWindow(wmFakeFocusCheck_, SW_HIDE);
        // 与参数区勾选框同一套 owner-draw / 点击切换（prop 状态），否则 BN_CLICKED 读不到勾选。
        SetWindowSubclass(wmFakeFocusCheck_, EditorChildSubclassProc, 1,
            reinterpret_cast<DWORD_PTR>(this));
        labelList_ = MakeLabel(hwnd_, L"动作列表", -1, kEditorListLabelX, kEditorToolbarLabelY, 80, kEditorMacroHeaderRowH); editorControls_.push_back(labelList_);
        labelBatchCount_ = MakeLabel(hwnd_, L"已选中:0个", -1, 95, kEditorToolbarLabelY, 120, kEditorMacroHeaderRowH); editorControls_.push_back(labelBatchCount_);
        ShowWindow(labelBatchCount_, SW_HIDE);
        loadBtn_ = MakeGreenButton(hwnd_, L"批量编辑", kLoad, 428, kEditorToolbarBtnY, 105, kEditorToolbarBtnH); editorControls_.push_back(loadBtn_);
        clearBtn_ = MakeGreenButton(hwnd_, L"清空列表", kClear, 546, kEditorToolbarBtnY, 105, kEditorToolbarBtnH); editorControls_.push_back(clearBtn_);
        batchExitBtn_ = MakeGreenButton(hwnd_, L"退出批量编辑", kBatchExit, 258, kEditorToolbarBtnY, 118, kEditorToolbarBtnH); editorControls_.push_back(batchExitBtn_);
        batchSelectAllBtn_ = MakeGreenButton(hwnd_, L"全选", kBatchSelectAll, 390, kEditorToolbarBtnY, 68, kEditorToolbarBtnH); editorControls_.push_back(batchSelectAllBtn_);
        batchDeselectBtn_ = MakeGreenButton(hwnd_, L"取消选择", kBatchDeselect, 464, kEditorToolbarBtnY, 88, kEditorToolbarBtnH); editorControls_.push_back(batchDeselectBtn_);
        batchDeleteBtn_ = MakeGreenButton(hwnd_, L"删除所选项", kBatchDelete, 558, kEditorToolbarBtnY, 105, kEditorToolbarBtnH); editorControls_.push_back(batchDeleteBtn_);
        batchCopyBtn_ = MakeGreenButton(hwnd_, L"复制所选项", kBatchCopy, 667, kEditorToolbarBtnY, 105, kEditorToolbarBtnH); editorControls_.push_back(batchCopyBtn_);
        ShowWindow(batchExitBtn_, SW_HIDE);
        ShowWindow(batchSelectAllBtn_, SW_HIDE);
        ShowWindow(batchDeselectBtn_, SW_HIDE);
        ShowWindow(batchDeleteBtn_, SW_HIDE);
        ShowWindow(batchCopyBtn_, SW_HIDE);
        HWND labelNo = MakeLabel(hwnd_, L"序号", -1, 32, kEditorListColumnHeaderY, 60, kEditorMacroHeaderRowH); editorControls_.push_back(labelNo);
        HWND labelAction = MakeLabel(hwnd_, L"动作", -1, 94, kEditorListColumnHeaderY, 80, kEditorMacroHeaderRowH); editorControls_.push_back(labelAction);
        HWND labelRemark = MakeLabel(hwnd_, L"备注", -1, 438, kEditorListColumnHeaderY, 80, kEditorMacroHeaderRowH); editorControls_.push_back(labelRemark);
        HWND labelOp = MakeLabel(hwnd_, L"操作", -1, 631, kEditorListColumnHeaderY, 80, kEditorMacroHeaderRowH); editorControls_.push_back(labelOp);
        actionCombo_ = MakeLabel(hwnd_, L"移动鼠标到", kActionCombo,
            kEditorComboRight - kEditorActionComboW, kEditorActionComboY, kEditorActionComboW, kEditorActionComboH);
        editorControls_.push_back(actionCombo_);
        popupAction_.items = {
            L"移动鼠标到", L"等待", L"鼠标点击", L"运行录制回放", L"运行鼠标宏",
            L"鼠标按下", L"鼠标松开", L"滚动滚轮", L"按键点击", L"键盘按下", L"键盘松开",
            L"快捷按键", L"快捷输入", L"循环", L"跳出循环", L"定义宏指令块", L"运行宏指令块",
            L"找图", L"文字识别", L"条件-如果", L"条件-否则",
            L"锁定截屏", L"解锁截屏", L"结束宏运行", L"运行程序", L"关闭程序",
            L"打开网页", L"打开文件", L"计时器记录时间",
            L"AI文字分析", L"AI图片分析", L"AI动作执行", L"获取当前光标位置", L"跳转",
            L"相对移动鼠标"
        };
        popupAction_.sel = 0;
        // 勿加 WS_CLIPCHILDREN / WS_EX_COMPOSITED：父 DC 需画输入框边框与下拉外观；
        // CLIPCHILDREN 会裁掉边线，COMPOSITED 易导致自绘勾选框走错 DRAWITEM 成绿按钮。
        paramViewport_ = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE,
            920, 225, 255, 755, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        editorControls_.push_back(paramViewport_);
        CreateParamControls();
        cancelBtn_ = MakeGreenButton(hwnd_, L"取消", kCancel, 775, 702, 104, 34); editorControls_.push_back(cancelBtn_);
        saveBtn_ = MakeGreenButton(hwnd_, L"保存", kSave, 891, 702, 104, 34); editorControls_.push_back(saveBtn_);
        listRemarkEdit_ = MakeEdit(hwnd_, L"", kListRemarkEdit, kColRemarkClient + 1, kListY + 2, kRemarkEditW, kRemarkEditH);
        ShowWindow(listRemarkEdit_, SW_HIDE);
        editorControls_.push_back(listRemarkEdit_);
        paramTopMask_ = MakeLabel(hwnd_, L"", -1, 0, 0, 1, 1); editorControls_.push_back(paramTopMask_);
        paramBottomMask_ = MakeLabel(hwnd_, L"", -1, 0, 0, 1, 1); editorControls_.push_back(paramBottomMask_);
        paramRightMask_ = MakeLabel(hwnd_, L"", -1, 0, 0, 1, 1); editorControls_.push_back(paramRightMask_);
    }

// was engine_host_window.h:1893-2324
void EngineHost::CreateParamControls() {
        using namespace EditorParamLayout;
        namespace EID = EditorParamLayout;  // for ID constants

        // ── 0. 移动鼠标到 ──
        {
            auto r = BuildAndStoreLayout(MoveMouse(), moveControls_, 0);
            moveHintLabel_ = FindLayoutTextControl(r, L"移动到(左上角为0,0)");
            moveXLabel_ = FindLayoutTextControl(r, L"X:", 0);
            moveYLabel_ = FindLayoutTextControl(r, L"Y:", 0);
            moveRandomXLabel_ = FindLayoutTextControl(r, L"±随机:", 0);
            moveRandomYLabel_ = FindLayoutTextControl(r, L"±随机:", 1);
            moveVarXLabel_ = FindLayoutTextControl(r, L"X:", 1);
            moveVarYLabel_ = FindLayoutTextControl(r, L"Y:", 1);
            moveHintFooter_ = FindLayoutTextControl(
                r, L"*提示:可使用来自找图、找色，获取颜色，文字识别保存到变量中的值");
            moveX_ = r.HwndForId(EID_MoveX); moveY_ = r.HwndForId(EID_MoveY);
            moveRandomX_ = r.HwndForId(EID_MoveRandomX); moveRandomY_ = r.HwndForId(EID_MoveRandomY);
            crosshairBtn_ = r.HwndForId(EID_Crosshair); moveFromVar_ = r.HwndForId(EID_MoveFromVar);
            moveVarX_ = r.HwndForId(EID_MoveVarX); moveVarY_ = r.HwndForId(EID_MoveVarY);
        }

        // ── 34. 相对移动鼠标 ──
        {
            auto r = BuildAndStoreLayout(MoveMouseRelative(), moveRelControls_, 34);
            moveRelX_ = r.HwndForId(EID_MoveRelX); moveRelY_ = r.HwndForId(EID_MoveRelY);
            moveRelRandomX_ = r.HwndForId(EID_MoveRelRandomX);
            moveRelRandomY_ = r.HwndForId(EID_MoveRelRandomY);
        }

        // ── 1. 等待 ──
        {
            auto r = BuildAndStoreLayout(Wait(), waitControls_, 1);
            waitDuration_ = r.HwndForId(EID_WaitDuration); waitRandom_ = r.HwndForId(EID_WaitRandom);
        }

        // ── 2. 鼠标点击 ──
        {
            auto r = BuildAndStoreLayout(MouseClick(), clickControls_, 2);
            clickButton_ = r.HwndForId(EID_ClickButton);
            clickLWin_ = r.HwndForId(EID_ClickLWin); clickRWin_ = r.HwndForId(EID_ClickRWin);
            clickLCtrl_ = r.HwndForId(EID_ClickLCtrl); clickRCtrl_ = r.HwndForId(EID_ClickRCtrl);
            clickLAlt_ = r.HwndForId(EID_ClickLAlt); clickRAlt_ = r.HwndForId(EID_ClickRAlt);
            clickLShift_ = r.HwndForId(EID_ClickLShift); clickRShift_ = r.HwndForId(EID_ClickRShift);
            clickCount_ = r.HwndForId(EID_ClickCount); clickWait_ = r.HwndForId(EID_ClickWait);
            clickRandom_ = r.HwndForId(EID_ClickRandom);
            popupClickBtn_.items = {L"左键", L"右键", L"中键", L"侧键1", L"侧键2"}; popupClickBtn_.sel = 0;
        }

        // ── 3. 鼠标回放 ──
        {
            auto r = BuildAndStoreLayout(MousePlayback(), mousePlaybackControls_, 3);
            mousePlaybackCombo_ = r.HwndForId(EID_MousePlaybackCombo);
            mousePlaybackCount_ = r.HwndForId(EID_MousePlaybackCount);
            mousePlaybackWait_ = r.HwndForId(EID_MousePlaybackWait);
            mousePlaybackRandom_ = r.HwndForId(EID_MousePlaybackRandom);
            popupMousePlayback_.items.clear(); popupMousePlayback_.sel = -1;
        }

        // ── 4. 运行鼠标宏 ──
        {
            auto r = BuildAndStoreLayout(RunMacro(), runMacroControls_, 4);
            runMacroCombo_ = r.HwndForId(EID_RunMacroCombo);
            popupRunMacro_.items.clear(); popupRunMacro_.sel = -1;
        }

        // ── 5/6. 鼠标按下/松开 (共用) ──
        {
            auto r = BuildAndStoreLayout(MousePress(), mousePressControls_, 5);
            mousePressButton_ = r.HwndForId(EID_MousePressButton);
            mousePressLWin_ = r.HwndForId(EID_MousePressLWin); mousePressRWin_ = r.HwndForId(EID_MousePressRWin);
            mousePressLCtrl_ = r.HwndForId(EID_MousePressLCtrl); mousePressRCtrl_ = r.HwndForId(EID_MousePressRCtrl);
            mousePressLAlt_ = r.HwndForId(EID_MousePressLAlt); mousePressRAlt_ = r.HwndForId(EID_MousePressRAlt);
            mousePressLShift_ = r.HwndForId(EID_MousePressLShift); mousePressRShift_ = r.HwndForId(EID_MousePressRShift);
            popupMouseBtn_.items = {L"左键", L"右键", L"中键", L"侧键1", L"侧键2"}; popupMouseBtn_.sel = 0;
            // 共享: 将同样的布局注册到 idx 6
            paramLayoutResults_[6] = r;
            for (HWND h : mousePressControls_) { if (h) editorControls_.push_back(h); }
        }

        // ── 7. 滚动滚轮 ──
        {
            auto r = BuildAndStoreLayout(ScrollWheel(), scrollWheelControls_, 7);
            scrollVertical_ = r.HwndForId(EID_ScrollVertical); scrollHorizontal_ = r.HwndForId(EID_ScrollHorizontal);
            scrollSteps_ = r.HwndForId(EID_ScrollSteps); scrollDirectionCombo_ = r.HwndForId(EID_ScrollDirection);
            scrollCount_ = r.HwndForId(EID_ScrollCount); scrollWait_ = r.HwndForId(EID_ScrollWait);
            scrollRandom_ = r.HwndForId(EID_ScrollRandom);
            SetChecked(scrollVertical_, true);
            popupScrollDir_.items = {L"向上/左", L"向下/右"}; popupScrollDir_.sel = 0;
        }

        // ── 8. 按键点击 ──
        {
            auto r = BuildAndStoreLayout(KeyClick(), keyControls_, 8);
            keyEdit_ = r.HwndForId(EID_KeyCapture);
            keyLWin_ = r.HwndForId(EID_KeyLWin); keyRWin_ = r.HwndForId(EID_KeyRWin);
            keyLCtrl_ = r.HwndForId(EID_KeyLCtrl); keyRCtrl_ = r.HwndForId(EID_KeyRCtrl);
            keyLAlt_ = r.HwndForId(EID_KeyLAlt); keyRAlt_ = r.HwndForId(EID_KeyRAlt);
            keyLShift_ = r.HwndForId(EID_KeyLShift); keyRShift_ = r.HwndForId(EID_KeyRShift);
            // 循环次数等通过动态 ID 0 查找 (遍历 placements 按顺序取)
            for (auto& p : r.placements) {
                if (p.id == 0 && !keyCount_ && p.hwnd) keyCount_ = p.hwnd;
                else if (p.id == 0 && keyCount_ && !keyWait_ && p.hwnd) keyWait_ = p.hwnd;
                else if (p.id == 0 && keyCount_ && keyWait_ && !keyRandom_ && p.hwnd) keyRandom_ = p.hwnd;
            }
        }

        // ── 9/10. 键盘按下/松开 (共用) ──
        {
            auto r = BuildAndStoreLayout(KeyPress(), keyPressControls_, 9);
            keyPressEdit_ = r.HwndForId(EID_KeyPressCapture);
            keyPressLWin_ = r.HwndForId(EID_KeyPressLWin); keyPressRWin_ = r.HwndForId(EID_KeyPressRWin);
            keyPressLCtrl_ = r.HwndForId(EID_KeyPressLCtrl); keyPressRCtrl_ = r.HwndForId(EID_KeyPressRCtrl);
            keyPressLAlt_ = r.HwndForId(EID_KeyPressLAlt); keyPressRAlt_ = r.HwndForId(EID_KeyPressRAlt);
            keyPressLShift_ = r.HwndForId(EID_KeyPressLShift); keyPressRShift_ = r.HwndForId(EID_KeyPressRShift);
            paramLayoutResults_[10] = r;
            for (HWND h : keyPressControls_) { if (h) editorControls_.push_back(h); }
        }

        // ── 11. 快捷按键 ──
        {
            auto r = BuildAndStoreLayout(HotkeyShortcut(), hotkeyShortcutControls_, 11);
            hotkeyShortcutCombo_ = r.HwndForId(EID_HotkeyShortcutCombo);
            hotkeyShortcutCount_ = r.HwndForId(EID_HotkeyShortcutCount);
            hotkeyShortcutWait_ = r.HwndForId(EID_HotkeyShortcutWait);
            hotkeyShortcutRandom_ = r.HwndForId(EID_HotkeyShortcutRandom);
        }

        // ── 12. 快捷输入 ──
        {
            auto r = BuildAndStoreLayout(QuickInput(), quickInputControls_, 12);
            quickInputEdit_ = r.HwndForId(EID_QuickInputText);
            quickInputVarCombo_ = r.HwndForId(EID_QuickInputVarCombo);
            quickInputInsertBtn_ = r.HwndForId(EID_QuickInputInsert);
            quickInputCharInterval_ = r.HwndForId(EID_QuickInputCharInterval);
            quickInputCount_ = r.HwndForId(EID_QuickInputCount);
            quickInputWait_ = r.HwndForId(EID_QuickInputWait);
            quickInputRandom_ = r.HwndForId(EID_QuickInputRandom);
        }

        // ── 13. 循环 ──
        {
            auto r = BuildAndStoreLayout(Loop(), loopControls_, 13);
            loopTypeCombo_ = r.HwndForId(EID_LoopTypeCombo);
            loopCount_ = r.HwndForId(EID_LoopCount); loopFromVar_ = r.HwndForId(EID_LoopFromVar);
            loopVarExpr_ = r.HwndForId(EID_LoopVarExpr); loopVarName_ = r.HwndForId(EID_LoopVarName);
            popupLoopType_.items = {L"次数循环"}; popupLoopType_.sel = 0;
        }

        // ── 14. 结束循环 ──
        { BuildAndStoreLayout(EndLoop(), endLoopControls_, 14); }

        // ── 15. 定义宏指令块 ──
        {
            auto r = BuildAndStoreLayout(DefineBlock(), defineBlockControls_, 15);
            defineBlockName_ = r.HwndForId(EID_DefineBlockName);
        }

        // ── 16. 运行宏指令块 ──
        {
            auto r = BuildAndStoreLayout(RunBlock(), runBlockControls_, 16);
            runBlockCombo_ = r.HwndForId(EID_RunBlockCombo);
            popupRunBlock_.items.clear(); popupRunBlock_.sel = -1;
        }

        // ── 17. 找图 (基础 + 子面板) ──
        {
            auto r = BuildAndStoreLayout(FindImageBase(), findImageControls_, 17);
            findFullScreenBtn_ = r.HwndForId(EID_FindFullScreen); findSelectRegionBtn_ = r.HwndForId(EID_FindSelectRegion);
            findX1_ = r.HwndForId(EID_FindX1); findY1_ = r.HwndForId(EID_FindY1);
            findX2_ = r.HwndForId(EID_FindX2); findY2_ = r.HwndForId(EID_FindY2);
            findTestBtn_ = r.HwndForId(EID_FindTest);
            findImagePreviewBtn_ = r.HwndForId(EID_FindImagePreview);
            findScreenshotBtn_ = r.HwndForId(EID_FindScreenshot); findLocalImageBtn_ = r.HwndForId(EID_FindLocalImage);
            findClearImageBtn_ = r.HwndForId(EID_FindClearImage);
            findMatchThreshold_ = r.HwndForId(EID_FindMatchThreshold);
            findScaleMin_ = r.HwndForId(EID_FindScaleMin); findScaleMax_ = r.HwndForId(EID_FindScaleMax);
            findFollowUpCombo_ = r.HwndForId(EID_FindFollowUp);
            popupFindFollowUp_.items = {L"点击", L"鼠标移动到", L"保存到变量"}; popupFindFollowUp_.sel = 0;

            auto r2 = BuildAndStoreLayout(FindImageOffset(), findImageOffsetControls_, 170);
            findOffsetX_ = r2.HwndForId(EID_FindOffsetX); findOffsetY_ = r2.HwndForId(EID_FindOffsetY);
            findSelectOffsetBtn_ = r2.HwndForId(EID_FindSelectOffset);

            auto r3 = BuildAndStoreLayout(FindImageVar(), findImageVarControls_, 171);
            findMatchVar_ = r3.HwndForId(EID_FindMatchVar);

            findRegionLabel_ = r.HwndForId(-1);  // first label in base layout
            auto findLabel = [](const UILayoutResult& lr, const wchar_t* text) -> HWND {
                for (const auto& p : lr.placements) {
                    if (!p.hwnd) continue;
                    if (p.type != UIComponentType::Label && p.type != UIComponentType::EditorLabel) continue;
                    wchar_t buf[64]{};
                    GetWindowTextW(p.hwnd, buf, 64);
                    if (wcscmp(buf, text) == 0) return p.hwnd;
                }
                return nullptr;
            };
            findImageHeaderLabel_ = findLabel(r, L"要查找的图");
            findX1Label_ = findLabel(r, L"X1");
            findY1Label_ = findLabel(r, L"Y1");
            findX2Label_ = findLabel(r, L"X2");
            findY2Label_ = findLabel(r, L"Y2");
            findTimeLabel_ = findLabel(r2, L"时间");
            findTimeEdit_ = r2.HwndForId(EID_FindTime);
            findFollowUpLabel_ = findLabel(r, L"后续操作");
            findOffsetXLabel_ = findLabel(r2, L"X偏");
            findOffsetYLabel_ = findLabel(r2, L"Y偏");
            findMatchVarLabel_ = findLabel(r3, L"匹配度保存到");
        }

        // ── 18. 文字识别 (基础 + 子面板) ──
        {
            auto r = BuildAndStoreLayout(OcrDepStatus(), ocrDepControls_, 180);
            ocrDepStatusLabel_ = r.HwndForId(-1);
            ocrDepInstallBtn_ = r.HwndForId(EID_OcrInstallDep);

            auto rt = BuildAndStoreLayout(OcrFindRegionToggle(), ocrFindRegionToggleControls_, 181);
            ocrRegionByImageCheck_ = rt.HwndForId(EID_OcrRegionByImage);
            ocrDigitsOnlyCheck_ = rt.HwndForId(EID_OcrDigitsOnly);

            auto rb = BuildAndStoreLayout(OcrBase(), ocrControls_, 18);
            ocrFullScreenBtn_ = rb.HwndForId(EID_OcrFullScreen); ocrSelectRegionBtn_ = rb.HwndForId(EID_OcrSelectRegion);
            ocrX1_ = rb.HwndForId(EID_OcrX1); ocrY1_ = rb.HwndForId(EID_OcrY1);
            ocrX2_ = rb.HwndForId(EID_OcrX2); ocrY2_ = rb.HwndForId(EID_OcrY2);
            ocrResultModeCombo_ = rb.HwndForId(EID_OcrResultMode); ocrTestBtn_ = rb.HwndForId(EID_OcrTest);
            popupOcrResultMode_.items = {L"获取文字", L"文字查找"}; popupOcrResultMode_.sel = 0;

            auto rs = BuildAndStoreLayout(OcrSearch(), ocrSearchControls_, 182);
            ocrSearchEdit_ = rs.HwndForId(EID_OcrSearchText);
            ocrSearchVarCombo_ = rs.HwndForId(EID_OcrSearchVarCombo); ocrSearchVarInsertBtn_ = rs.HwndForId(EID_OcrSearchVarInsert);
            popupOcrSearchVar_.items.clear(); popupOcrSearchVar_.sel = -1;

            auto rf = BuildAndStoreLayout(OcrFollowUp(), ocrFollowControls_, 183);
            ocrFollowUpCombo_ = rf.HwndForId(EID_OcrFollowUp); ocrUntilFound_ = rf.HwndForId(EID_OcrUntilFound);
            popupOcrFollowUp_.items = {L"点击", L"鼠标移动到", L"保存到变量"}; popupOcrFollowUp_.sel = 0;

            auto ro = BuildAndStoreLayout(OcrFollowOffset(), ocrFollowOffsetControls_, 184);
            ocrOffsetX_ = ro.HwndForId(EID_OcrOffsetX); ocrOffsetY_ = ro.HwndForId(EID_OcrOffsetY);
            ocrSelectOffsetBtn_ = ro.HwndForId(EID_OcrSelectOffset);

            auto rv = BuildAndStoreLayout(OcrFollowVar(), ocrFollowVarControls_, 185);
            ocrResultVar_ = rv.HwndForId(EID_OcrResultVar);

            auto rfr = BuildAndStoreLayout(OcrFindRegion(), ocrFindRegionControls_, 186);
            ocrFindImagePreviewBtn_ = rfr.HwndForId(EID_OcrFindImagePreview);
            ocrFindScreenshotBtn_ = rfr.HwndForId(EID_OcrFindScreenshot); ocrFindLocalImageBtn_ = rfr.HwndForId(EID_OcrFindLocalImage);
            ocrFindClearImageBtn_ = rfr.HwndForId(EID_OcrFindClearImage);
            ocrFindMatchThreshold_ = rfr.HwndForId(EID_OcrFindMatchThreshold);
            ocrFindScaleMin_ = rfr.HwndForId(EID_OcrFindScaleMin); ocrFindScaleMax_ = rfr.HwndForId(EID_OcrFindScaleMax);
            ocrFindSelectRegionBtn_ = rfr.HwndForId(EID_OcrFindSelectRegion);
            ocrFindImageLabel_ = rfr.HwndForId(-1);
            ocrImageRegionX1_ = rfr.HwndForId(EID_OcrImageRegionX1);
            ocrImageRegionY1_ = rfr.HwndForId(EID_OcrImageRegionY1);
            ocrImageRegionX2_ = rfr.HwndForId(EID_OcrImageRegionX2);
            ocrImageRegionY2_ = rfr.HwndForId(EID_OcrImageRegionY2);

            ocrRegionLabel_ = rb.HwndForId(-1);
            auto findLabel = [](const UILayoutResult& r, const wchar_t* text) -> HWND {
                for (const auto& p : r.placements) {
                    if (!p.hwnd) continue;
                    if (p.type != UIComponentType::Label && p.type != UIComponentType::EditorLabel) continue;
                    wchar_t buf[64]{};
                    GetWindowTextW(p.hwnd, buf, 64);
                    if (wcscmp(buf, text) == 0) return p.hwnd;
                }
                return nullptr;
            };
            ocrResultModeLabel_ = findLabel(rb, L"结果处理");
            ocrFollowUpLabel_ = findLabel(rf, L"后续操作");
            ocrSearchLabel_ = findLabel(rs, L"查找:");
            ocrSearchVarLabel_ = findLabel(rs, L"变量:");
            ocrX1Label_ = findLabel(rb, L"X1");
            ocrY1Label_ = findLabel(rb, L"Y1");
            ocrX2Label_ = findLabel(rb, L"X2");
            ocrY2Label_ = findLabel(rb, L"Y2");
            ocrOffsetXLabel_ = findLabel(ro, L"X偏");
            ocrOffsetYLabel_ = findLabel(ro, L"Y偏");
            ocrResultVarLabel_ = findLabel(rv, L"结果保存到");
            SetPopupSel(popupOcrResultMode_, ocrResultModeCombo_, 0);
            SetPopupSel(popupOcrFollowUp_, ocrFollowUpCombo_, 0);
            RefreshOcrSubPanel();
            RefreshOcrDepStatus();
        }

        // ── 19. 条件-如果 ──
        {
            auto r = BuildAndStoreLayout(IfCondition(), ifControls_, 19);
            ifVarCombo_ = r.HwndForId(EID_IfVarCombo); ifOperatorCombo_ = r.HwndForId(EID_IfOperator);
            ifValueEdit_ = r.HwndForId(EID_IfValue); ifConnectorCombo_ = r.HwndForId(EID_IfConnector);
            ifAddConditionBtn_ = r.HwndForId(EID_IfAddCondition); ifConditionList_ = r.HwndForId(EID_IfConditionList);
            popupIfOperator_.items = {L"等于", L"不等于", L"小于", L"小于等于", L"大于", L"大于等于", L"包含"}; popupIfOperator_.sel = 0;
            popupIfConnector_.items = {L"并且(and)", L"或者(or)", L"非(not)"}; popupIfConnector_.sel = 0;
        }

        // ── 20. 条件-否则 ──
        { BuildAndStoreLayout(ElseCondition(), elseControls_, 20); }

        // ── 21. 锁定截屏 ──
        { BuildAndStoreLayout(LockScreenshot(), lockScreenshotControls_, 21); }

        // ── 22. 解锁截屏 ──
        { BuildAndStoreLayout(UnlockScreenshot(), unlockScreenshotControls_, 22); }

        // ── 23. 结束宏运行 ──
        { BuildAndStoreLayout(StopMacro(), stopMacroControls_, 23); }

        // ── 24. 打开程序 ──
        {
            auto r = BuildAndStoreLayout(RunProgram(), runProgramControls_, 24);
            runProgramCombo_ = r.HwndForId(EID_RunProgramCombo);

            auto rf = BuildAndStoreLayout(RunProgramFile(), runProgramFileControls_, 240);
            runProgramPath_ = rf.HwndForId(EID_RunProgramPath); runProgramBrowseBtn_ = rf.HwndForId(EID_RunProgramBrowse);
            runProgramCrosshairBtn_ = rf.HwndForId(EID_RunProgramCrosshair); runProgramArgs_ = rf.HwndForId(EID_RunProgramArgs);
        }

        // ── 25. 关闭程序 ──
        {
            auto r = BuildAndStoreLayout(CloseProgram(), closeProgramControls_, 25);
            closeProgramPath_ = r.HwndForId(EID_CloseProgramPath); closeProgramBrowseBtn_ = r.HwndForId(EID_CloseProgramBrowse);
            closeProgramCrosshairBtn_ = r.HwndForId(EID_CloseProgramCrosshair);
            closeProgramMatchFileName_ = r.HwndForId(EID_CloseProgramMatchFileName);
        }

        // ── 26. 打开网页 ──
        {
            auto r = BuildAndStoreLayout(OpenWebpage(), openWebpageControls_, 26);
            openWebpageUrl_ = r.HwndForId(EID_OpenWebpageUrl);
        }

        // ── 27. 打开文件 ──
        {
            auto r = BuildAndStoreLayout(OpenFile(), openFileControls_, 27);
            openFilePath_ = r.HwndForId(EID_OpenFilePath); openFileBrowseBtn_ = r.HwndForId(EID_OpenFileBrowse);
        }

        // ── 28. 计时器记录时间 ──
        {
            auto r = BuildAndStoreLayout(TimerRecord(), timerRecordControls_, 28);
            timerVarName_ = r.HwndForId(EID_TimerVarName);
        }

        // ── 32. 获取当前光标位置 ──
        {
            auto r = BuildAndStoreLayout(GetCursorPos(), getCursorPosControls_, 32);
            cursorPosVarName_ = r.HwndForId(EID_CursorPosVarName);
        }

        // ── 33. 跳转 ──
        {
            auto r = BuildAndStoreLayout(Goto(), gotoControls_, 33);
            gotoStepEdit_ = r.HwndForId(EID_GotoStepExpr);
        }

        // ── AI 公共部分 ──
        popupAiModel_.items = {}; popupAiModel_.sel = -1;
        popupAiContextMode_.items = {L"无上下文", L"宏上下文", L"循环上下文", L"指令块上下文"}; popupAiContextMode_.sel = 1;
        popupAiOutputType_.items = {L"文本", L"整数"}; popupAiOutputType_.sel = 0;

        // ── 29. AI文字分析 ──
        {
            auto r = BuildAndStoreLayout(AiCommon(), aiCommonControls_, 29);
            aiPromptEdit_ = r.HwndForId(EID_AiPrompt); aiInsertVarBtn_ = r.HwndForId(EID_AiInsertVar);
            aiVarCombo_ = r.HwndForId(EID_AiVarCombo);
            aiModelCombo_ = r.HwndForId(EID_AiModel); aiContextModeCombo_ = r.HwndForId(EID_AiContextMode);
            aiTimeoutEdit_ = r.HwndForId(EID_AiTimeout); aiFallbackEdit_ = r.HwndForId(EID_AiFallback);
            aiOutputVarEdit_ = r.HwndForId(EID_AiOutputVar); aiOutputTypeCombo_ = r.HwndForId(EID_AiOutputType);
            aiPromptLabel_ = FindLayoutTextControl(r, L"提示词 (Prompt)");
            aiVarLabel_ = FindLayoutTextControl(r, L"变量");
            aiModelLabel_ = FindLayoutTextControl(r, L"AI 模型");
            aiContextLabel_ = FindLayoutTextControl(r, L"上下文模式");
            aiTimeoutLabel_ = FindLayoutTextControl(r, L"超时(秒)");
            aiFallbackLabel_ = FindLayoutTextControl(r, L"降级值(失败时使用)");
            aiOutputVarLabel_ = FindLayoutTextControl(r, L"输出变量名");
            aiOutputTypeLabel_ = FindLayoutTextControl(r, L"输出类型");
            aiMaxStepsEdit_ = r.HwndForId(EID_AiMaxSteps);
            aiWithImageCheck_ = r.HwndForId(EID_AiWithImage);
            aiLogicConvertCheck_ = r.HwndForId(EID_AiLogicConvert);
            aiMaxStepsLabel_ = FindLayoutTextControl(r, L"最大步骤数");
            aiMaxStepsHint_ = FindLayoutTextControl(r, L"*提示:-1表示不限制步数");
        }

        // ── 30. AI图片分析专用 ──
        {
            auto r = BuildAndStoreLayout(AiImage(), aiImageControls_, 300);
            aiImageScaleEdit_ = r.HwndForId(EID_AiImageScale); aiRegionByImageCheck_ = r.HwndForId(EID_AiRegionByImage);
            aiFullScreenBtn_ = r.HwndForId(EID_AiFullScreen); aiSelectRegionBtn_ = r.HwndForId(EID_AiSelectRegion);
            aiSearchX1Edit_ = r.HwndForId(EID_AiSearchX1); aiSearchY1Edit_ = r.HwndForId(EID_AiSearchY1);
            aiSearchX2Edit_ = r.HwndForId(EID_AiSearchX2); aiSearchY2Edit_ = r.HwndForId(EID_AiSearchY2);
            aiImageScaleLabel_ = FindLayoutTextControl(r, L"截屏缩放(0.1-1.0)");
            aiRegionLabel_ = FindLayoutTextControl(r, L"识别区域");
            aiCoordX1Label_ = FindLayoutTextControl(r, L"X1");
            aiCoordY1Label_ = FindLayoutTextControl(r, L"Y1");
            aiCoordX2Label_ = FindLayoutTextControl(r, L"X2");
            aiCoordY2Label_ = FindLayoutTextControl(r, L"Y2");
        }

        // ── 31. AI动作执行专用 ──
        {
            auto r = BuildAndStoreLayout(AiAction(), aiActionControls_, 310);
            aiRegionByImageCheck2_ = r.HwndForId(EID_AiRegionByImage2);
            aiFullScreenBtn2_ = r.HwndForId(EID_AiFullScreen); aiSelectRegionBtn2_ = r.HwndForId(EID_AiSelectRegion);
            aiSearchX1Edit2_ = r.HwndForId(EID_AiSearchX1); aiSearchY1Edit2_ = r.HwndForId(EID_AiSearchY1);
            aiSearchX2Edit2_ = r.HwndForId(EID_AiSearchX2); aiSearchY2Edit2_ = r.HwndForId(EID_AiSearchY2);
            aiActionRegionLabel_ = FindLayoutTextControl(r, L"识别区域");
            aiActCoordX1Label_ = FindLayoutTextControl(r, L"X1");
            aiActCoordY1Label_ = FindLayoutTextControl(r, L"Y1");
            aiActCoordX2Label_ = FindLayoutTextControl(r, L"X2");
            aiActCoordY2Label_ = FindLayoutTextControl(r, L"Y2");
        }

        // ── AI 根据图片选取区域 (共用) ──
        {
            auto r = BuildAndStoreLayout(AiFindRegion(), aiFindRegionControls_, 320);
            aiFindImagePreviewBtn_ = r.HwndForId(EID_AiTargetPreview);
            aiFindScreenshotBtn_ = r.HwndForId(EID_AiTargetScreenshot); aiFindLocalImageBtn_ = r.HwndForId(EID_AiTargetLocal);
            aiFindClearImageBtn_ = r.HwndForId(EID_AiTargetClear);
            aiFindMatchThreshold_ = r.HwndForId(EID_AiFindMatchThreshold);
            aiFindScaleMin_ = r.HwndForId(EID_AiFindScaleMin); aiFindScaleMax_ = r.HwndForId(EID_AiFindScaleMax);
            aiFindSelectRegionBtn_ = r.HwndForId(EID_AiFindSelectRegion);
            aiFindImageLabel_ = FindLayoutTextControl(r, L"要查找的图");
            aiFindMatchLabel_ = FindLayoutTextControl(r, L"范围");
            aiFindMatchPctLabel_ = FindLayoutTextControl(r, L"%");
            aiFindScaleMinLabel_ = FindLayoutTextControl(r, L"最小缩放");
            aiFindScaleMaxLabel_ = FindLayoutTextControl(r, L"最大");
            aiImageRegionX1_ = r.HwndForId(EID_AiImageRegionX1);
            aiImageRegionY1_ = r.HwndForId(EID_AiImageRegionY1);
            aiImageRegionX2_ = r.HwndForId(EID_AiImageRegionX2);
            aiImageRegionY2_ = r.HwndForId(EID_AiImageRegionY2);
        }

        InitRunProgramPresets();
        InitCrosshairDrag();

        AddEditorControl(remarkLabel_ = MakeLabel(hwnd_, L"备注", -1, 807, kEditorRemarkY, 44, 22));
        AddEditorControl(remark_ = MakeEdit(hwnd_, L"", kRemark, 857, kEditorRemarkY, 117, 22));
        AddEditorControl(modifyBtn_ = MakeGreenButton(hwnd_, L"修改", kModify, 837, kEditorAddY, 76, 30));
        AddEditorControl(addBtn_ = MakeGreenButton(hwnd_, L"添加", kAdd, 927, kEditorAddY, 76, 30));
    }

// was engine_host_window.h:7070-7360
void EngineHost::LoadForm(const ScriptAction& action) {
        loadingForm_ = true;
        // 先清空全部勾选/修饰键，再写入当前类型，避免跨类型与退出后残留
        ClearAllModifierHoldCheckboxes();
        ClearAllParamCheckboxStates();
        SetText(remark_, action.remark);
        if (action.type == ActionType::MoveMouse) {
            SetPopupSel(popupAction_, actionCombo_, 0);
            SetText(moveX_, std::to_wstring(action.x));
            SetText(moveY_, std::to_wstring(action.y));
            SetText(moveRandomX_, std::to_wstring(action.randomX));
            SetText(moveRandomY_, std::to_wstring(action.randomY));
            SetChecked(moveFromVar_, action.moveFromVar);
            SetText(moveVarX_, action.moveVarExprX.empty() ? L"0" : action.moveVarExprX);
            SetText(moveVarY_, action.moveVarExprY.empty() ? L"0" : action.moveVarExprY);
        }
        else if (action.type == ActionType::MoveMouseRelative) {
            SetPopupSel(popupAction_, actionCombo_, 34);
            SetText(moveRelX_, std::to_wstring(action.x));
            SetText(moveRelY_, std::to_wstring(action.y));
            SetText(moveRelRandomX_, std::to_wstring(action.randomX));
            SetText(moveRelRandomY_, std::to_wstring(action.randomY));
        }
        else if (action.type == ActionType::Wait) { SetPopupSel(popupAction_, actionCombo_, 1); SetText(waitDuration_, F3(action.duration)); SetText(waitRandom_, F3(action.randomDuration)); }
        else if (action.type == ActionType::MouseDown || action.type == ActionType::MouseUp) { SetPopupSel(popupAction_, actionCombo_, ComboSelForType(action.type)); SetPopupSel(popupMouseBtn_, mousePressButton_, static_cast<int>(action.button)); WriteModifierHolds(action, mousePressLWin_, mousePressRWin_, mousePressLCtrl_, mousePressRCtrl_, mousePressLAlt_, mousePressRAlt_, mousePressLShift_, mousePressRShift_); }
        else if (action.type == ActionType::MouseClick) { SetPopupSel(popupAction_, actionCombo_, 2); SetPopupSel(popupClickBtn_, clickButton_, static_cast<int>(action.button)); SetText(clickCount_, std::to_wstring(action.clickCount)); SetText(clickWait_, F3(action.duration)); SetText(clickRandom_, F3(action.randomDuration)); WriteModifierHolds(action, clickLWin_, clickRWin_, clickLCtrl_, clickRCtrl_, clickLAlt_, clickRAlt_, clickLShift_, clickRShift_); }
        else if (action.type == ActionType::MousePlayback) {
            SetPopupSel(popupAction_, actionCombo_, 3);
            RefreshMousePlaybackCombo();
            if (!action.blockName.empty()) {
                int idx = -1;
                for (size_t i = 0; i < popupMousePlayback_.items.size(); ++i) {
                    if (popupMousePlayback_.items[i] == action.blockName) { idx = static_cast<int>(i); break; }
                }
                SetPopupSel(popupMousePlayback_, mousePlaybackCombo_, idx);
            }
            SetText(mousePlaybackCount_, std::to_wstring(action.clickCount));
            SetText(mousePlaybackWait_, F3(action.duration));
            SetText(mousePlaybackRandom_, F3(action.randomDuration));
        }
        else if (action.type == ActionType::RunMacro) {
            SetPopupSel(popupAction_, actionCombo_, 4);
            RefreshRunMacroCombo();
            if (!action.blockName.empty()) {
                int idx = -1;
                for (size_t i = 0; i < popupRunMacro_.items.size(); ++i) {
                    if (popupRunMacro_.items[i] == action.blockName) { idx = static_cast<int>(i); break; }
                }
                SetPopupSel(popupRunMacro_, runMacroCombo_, idx);
            }
        }
        else if (action.type == ActionType::KeyDown || action.type == ActionType::KeyUp) { SetPopupSel(popupAction_, actionCombo_, ComboSelForType(action.type)); formKeyPressText_ = action.keyText; formKeyPressVk_ = action.keyVk; SetText(keyPressEdit_, formKeyPressText_); WriteModifierHolds(action, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_, keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_); }
        else if (action.type == ActionType::KeyClick) { SetPopupSel(popupAction_, actionCombo_, 8); formKeyText_ = action.keyText; formKeyVk_ = action.keyVk; SetText(keyEdit_, formKeyText_); SetText(keyCount_, std::to_wstring(action.clickCount)); SetText(keyWait_, F3(action.duration)); SetText(keyRandom_, F3(action.randomDuration)); WriteModifierHolds(action, keyLWin_, keyRWin_, keyLCtrl_, keyRCtrl_, keyLAlt_, keyRAlt_, keyLShift_, keyRShift_); }
        else if (action.type == ActionType::HotkeyShortcut) {
            SetPopupSel(popupAction_, actionCombo_, 11);
            SetPopupSel(popupHotkeyShortcut_, hotkeyShortcutCombo_, std::clamp(action.shortcutPreset, 0, ShortcutPresetCount() - 1));
            SetText(hotkeyShortcutCount_, std::to_wstring(action.clickCount));
            SetText(hotkeyShortcutWait_, F3(action.duration));
            SetText(hotkeyShortcutRandom_, F3(action.randomDuration));
        }
        else if (action.type == ActionType::QuickInput) {
            SetPopupSel(popupAction_, actionCombo_, 12);
            RefreshQuickInputVarCombo();
            SetText(quickInputEdit_, action.inputText);
            SetText(quickInputCharInterval_, F3(action.charInterval));
            SetText(quickInputCount_, std::to_wstring(action.clickCount));
            SetText(quickInputWait_, F3(action.duration));
            SetText(quickInputRandom_, F3(action.randomDuration));
        }
        else if (action.type == ActionType::Loop) { SetPopupSel(popupAction_, actionCombo_, 13); SetText(loopCount_, std::to_wstring(action.loopCount)); SetChecked(loopFromVar_, action.loopFromVar); SetText(loopVarExpr_, action.loopVarExpr); SetText(loopVarName_, action.loopVarName); SetPopupSel(popupLoopType_, loopTypeCombo_, 0); }
        else if (action.type == ActionType::EndLoop) { SetPopupSel(popupAction_, actionCombo_, 14); }
        else if (action.type == ActionType::DefineBlock) { SetPopupSel(popupAction_, actionCombo_, 15); SetText(defineBlockName_, action.blockName.empty() ? L"block1" : action.blockName); }
        else if (action.type == ActionType::RunBlock) { SetPopupSel(popupAction_, actionCombo_, 16); RefreshRunBlockCombo(); if (!action.blockName.empty()) { int idx = -1; for (size_t i = 0; i < popupRunBlock_.items.size(); ++i) { if (popupRunBlock_.items[i] == action.blockName) { idx = static_cast<int>(i); break; } } SetPopupSel(popupRunBlock_, runBlockCombo_, idx); } }
        else if (action.type == ActionType::ScrollWheel) {
            SetPopupSel(popupAction_, actionCombo_, 7);
            SetChecked(scrollVertical_, action.scrollVertical);
            SetChecked(scrollHorizontal_, action.scrollHorizontal);
            SetText(scrollSteps_, std::to_wstring(action.scrollSteps));
            SetPopupSel(popupScrollDir_, scrollDirectionCombo_, std::clamp(action.scrollDirection, 0, 1));
            SetText(scrollCount_, std::to_wstring(action.clickCount));
            SetText(scrollWait_, F3(action.duration));
            SetText(scrollRandom_, F3(action.randomDuration));
        }
        else if (action.type == ActionType::FindImage) {
            SetPopupSel(popupAction_, actionCombo_, 17);
            findImageFullScreen_ = action.searchFullScreen;
            findImagePath_ = ResolveImagePath(action.imagePath);
            if (action.searchFullScreen) {
                ApplyFindImageFullScreen();
            } else {
                SetText(findX1_, std::to_wstring(action.searchX1));
                SetText(findY1_, std::to_wstring(action.searchY1));
                SetText(findX2_, std::to_wstring(action.searchX2));
                SetText(findY2_, std::to_wstring(action.searchY2));
                RefreshCoordFieldEdits({findX1_, findY1_, findX2_, findY2_});
            }
            SetText(findMatchThreshold_, std::to_wstring(static_cast<int>(action.matchThreshold)));
            SetText(findScaleMin_, F3(action.imageScaleMin > 0.0 ? action.imageScaleMin : action.imageScale));
            SetText(findScaleMax_, F3(action.imageScaleMax > 0.0 ? action.imageScaleMax : action.imageScale));
            SetPopupSel(popupFindFollowUp_, findFollowUpCombo_, std::clamp(action.findImageFollowUp, 0, 2));
            SetText(findOffsetX_, std::to_wstring(action.offsetX));
            SetText(findOffsetY_, std::to_wstring(action.offsetY));
            SetText(findTimeEdit_, action.findTimeExpr.empty() ? L"0" : action.findTimeExpr);
            SetText(findMatchVar_, action.matchVarName.empty() ? L"matchRet" : action.matchVarName);
            UpdateFindImagePreview();
        }
        else if (action.type == ActionType::TextRecognition) {
            SetPopupSel(popupAction_, actionCombo_, 18);
            ocrFullScreen_ = action.searchFullScreen;
            SetChecked(ocrRegionByImageCheck_, action.ocrRegionByImage);
            SetChecked(ocrDigitsOnlyCheck_, action.ocrDigitsOnly);
            ocrFindImagePath_ = action.ocrRegionByImage ? action.imagePath : L"";
            if (action.ocrRegionByImage) {
                SetText(ocrFindMatchThreshold_, std::to_wstring(static_cast<int>(action.matchThreshold)));
                SetText(ocrFindScaleMin_, F3(action.imageScaleMin > 0.0 ? action.imageScaleMin : action.imageScale));
                SetText(ocrFindScaleMax_, F3(action.imageScaleMax > 0.0 ? action.imageScaleMax : action.imageScale));
                SetText(ocrImageRegionX1_, std::to_wstring(action.imageRegionX1));
                SetText(ocrImageRegionY1_, std::to_wstring(action.imageRegionY1));
                SetText(ocrImageRegionX2_, std::to_wstring(action.imageRegionX2));
                SetText(ocrImageRegionY2_, std::to_wstring(action.imageRegionY2));
                UpdateOcrFindImagePreview();
            } else {
                UpdateOcrFindImagePreview();
            }
            if (action.searchFullScreen) {
                ApplyOcrFullScreen();
            } else {
                SetText(ocrX1_, std::to_wstring(action.searchX1));
                SetText(ocrY1_, std::to_wstring(action.searchY1));
                SetText(ocrX2_, std::to_wstring(action.searchX2));
                SetText(ocrY2_, std::to_wstring(action.searchY2));
                RefreshCoordFieldEdits({ocrX1_, ocrY1_, ocrX2_, ocrY2_});
            }
            SetPopupSel(popupOcrResultMode_, ocrResultModeCombo_, std::clamp(action.ocrResultMode, 0, 1));
            SetText(ocrSearchEdit_, action.ocrSearchText);
            SetPopupSel(popupOcrFollowUp_, ocrFollowUpCombo_, std::clamp(action.ocrFollowUp, 0, 2));
            SetText(ocrOffsetX_, std::to_wstring(action.offsetX));
            SetText(ocrOffsetY_, std::to_wstring(action.offsetY));
            SetChecked(ocrUntilFound_, action.findUntilFound);
            SetText(ocrResultVar_, action.matchVarName.empty() ? L"a" : action.matchVarName);
            RefreshOcrSearchVarCombo();
        }
        else if (action.type == ActionType::If) {
            SetPopupSel(popupAction_, actionCombo_, 19);
            RefreshIfVarCombo();
            SetText(ifConditionList_, action.conditionExpr);
            SetText(ifValueEdit_, L"0");
            SetPopupSel(popupIfOperator_, ifOperatorCombo_, 0);
            SetPopupSel(popupIfConnector_, ifConnectorCombo_, 0);
        }
        else if (action.type == ActionType::Else) {
            SetPopupSel(popupAction_, actionCombo_, 20);
        }
        else if (action.type == ActionType::LockScreenshot) {
            SetPopupSel(popupAction_, actionCombo_, 21);
        }
        else if (action.type == ActionType::UnlockScreenshot) {
            SetPopupSel(popupAction_, actionCombo_, 22);
        }
        else if (action.type == ActionType::StopMacro) {
            SetPopupSel(popupAction_, actionCombo_, 23);
        }
        else if (action.type == ActionType::RunProgram) {
            SetPopupSel(popupAction_, actionCombo_, 24);
            SetPopupSel(popupRunProgram_, runProgramCombo_, std::clamp(action.shortcutPreset, 0, RunProgramPresetCount() - 1));
            SetText(runProgramPath_, action.targetPath);
            SetText(runProgramArgs_, action.inputText);
        }
        else if (action.type == ActionType::CloseProgram) {
            SetPopupSel(popupAction_, actionCombo_, 25);
            SetText(closeProgramPath_, action.targetPath);
            SetChecked(closeProgramMatchFileName_, action.matchFileNameOnly);
        }
        else if (action.type == ActionType::OpenWebpage) {
            SetPopupSel(popupAction_, actionCombo_, 26);
            SetText(openWebpageUrl_, action.targetPath);
        }
        else if (action.type == ActionType::OpenFile) {
            SetPopupSel(popupAction_, actionCombo_, 27);
            SetText(openFilePath_, action.targetPath);
        }
        else if (action.type == ActionType::ActivateWindow) {
            // Web 壳为主；GDI 复用关闭程序路径框展示 match
            SetPopupSel(popupAction_, actionCombo_, 25);
            SetText(closeProgramPath_, action.targetPath);
        }
        else if (action.type == ActionType::TimerRecordTime) {
            SetPopupSel(popupAction_, actionCombo_, 28);
            SetText(timerVarName_, action.loopVarName);
        }
        else if (action.type == ActionType::GetCursorPos) {
            SetPopupSel(popupAction_, actionCombo_, 32);
            SetText(cursorPosVarName_, action.matchVarName);
        }
        else if (action.type == ActionType::Goto) {
            SetPopupSel(popupAction_, actionCombo_, 33);
            SetText(gotoStepEdit_, action.gotoStepExpr);
        }
        else if (action.type == ActionType::AiTextAnalysis) {
            SetPopupSel(popupAction_, actionCombo_, 29);
            RefreshAiModelCombo();
            SetText(aiPromptEdit_, action.aiPrompt);
            SetText(aiOutputVarEdit_, action.aiOutputVarName.empty() ? L"aiResult" : action.aiOutputVarName);
            SetPopupSel(popupAiOutputType_, aiOutputTypeCombo_, std::clamp(action.aiOutputType, 0, 1));
            if (!action.aiModelName.empty()) {
                int idx = -1;
                for (size_t i = 0; i < popupAiModel_.items.size(); ++i) {
                    if (popupAiModel_.items[i] == action.aiModelName) { idx = static_cast<int>(i); break; }
                }
                SetPopupSel(popupAiModel_, aiModelCombo_, idx);
            } else if (!popupAiModel_.items.empty()) {
                SetPopupSel(popupAiModel_, aiModelCombo_, popupAiModel_.sel >= 0 ? popupAiModel_.sel : 0);
            }
            SetPopupSel(popupAiContextMode_, aiContextModeCombo_, std::clamp(action.aiContextMode, 0, 3));
            SetText(aiTimeoutEdit_, std::to_wstring(action.aiTimeoutSec));
            SetText(aiFallbackEdit_, action.aiFallbackValue);
        }
        else if (action.type == ActionType::AiImageAnalysis) {
            SetPopupSel(popupAction_, actionCombo_, 30);
            RefreshAiModelCombo();
            SetText(aiPromptEdit_, action.aiPrompt);
            SetText(aiOutputVarEdit_, action.aiOutputVarName.empty() ? L"aiImgResult" : action.aiOutputVarName);
            SetPopupSel(popupAiOutputType_, aiOutputTypeCombo_, std::clamp(action.aiOutputType, 0, 1));
            if (!action.aiModelName.empty()) {
                int idx = -1;
                for (size_t i = 0; i < popupAiModel_.items.size(); ++i) {
                    if (popupAiModel_.items[i] == action.aiModelName) { idx = static_cast<int>(i); break; }
                }
                SetPopupSel(popupAiModel_, aiModelCombo_, idx);
            } else if (!popupAiModel_.items.empty()) {
                SetPopupSel(popupAiModel_, aiModelCombo_, popupAiModel_.sel >= 0 ? popupAiModel_.sel : 0);
            }
            SetPopupSel(popupAiContextMode_, aiContextModeCombo_, std::clamp(action.aiContextMode, 0, 3));
            SetText(aiTimeoutEdit_, std::to_wstring(action.aiTimeoutSec));
            SetText(aiFallbackEdit_, action.aiFallbackValue);
            SetChecked(aiRegionByImageCheck_, action.aiRegionByImage);
            aiFindImagePath_ = action.aiTargetImagePath;
            if (action.aiRegionByImage) {
                SetText(aiFindMatchThreshold_, F3(action.matchThreshold));
                SetText(aiFindScaleMin_, F3(action.imageScaleMin > 0.0 ? action.imageScaleMin : 0.9));
                SetText(aiFindScaleMax_, F3(action.imageScaleMax > 0.0 ? action.imageScaleMax : 1.1));
                SetText(aiImageRegionX1_, std::to_wstring(action.imageRegionX1));
                SetText(aiImageRegionY1_, std::to_wstring(action.imageRegionY1));
                SetText(aiImageRegionX2_, std::to_wstring(action.imageRegionX2));
                SetText(aiImageRegionY2_, std::to_wstring(action.imageRegionY2));
            }
            UpdateAiFindImagePreview();
            SetText(aiImageScaleEdit_, F3(action.aiImageScale));
            if (action.searchFullScreen) {
                ApplyAiFullScreen();
            } else {
                SetText(aiSearchX1Edit_, std::to_wstring(action.aiSearchX1));
                SetText(aiSearchY1Edit_, std::to_wstring(action.aiSearchY1));
                SetText(aiSearchX2Edit_, std::to_wstring(action.aiSearchX2));
                SetText(aiSearchY2Edit_, std::to_wstring(action.aiSearchY2));
                RefreshCoordFieldEdits({aiSearchX1Edit_, aiSearchY1Edit_, aiSearchX2Edit_, aiSearchY2Edit_});
            }
        }
        else if (action.type == ActionType::AiActionExecute) {
            SetPopupSel(popupAction_, actionCombo_, 31);
            RefreshAiModelCombo();
            SetText(aiPromptEdit_, action.aiPrompt);
            if (!action.aiModelName.empty()) {
                int idx = -1;
                for (size_t i = 0; i < popupAiModel_.items.size(); ++i) {
                    if (popupAiModel_.items[i] == action.aiModelName) { idx = static_cast<int>(i); break; }
                }
                SetPopupSel(popupAiModel_, aiModelCombo_, idx);
            } else if (!popupAiModel_.items.empty()) {
                SetPopupSel(popupAiModel_, aiModelCombo_, popupAiModel_.sel >= 0 ? popupAiModel_.sel : 0);
            }
            SetPopupSel(popupAiContextMode_, aiContextModeCombo_, std::clamp(action.aiContextMode, 0, 3));
            SetText(aiTimeoutEdit_, std::to_wstring(action.aiTimeoutSec));
            SetText(aiFallbackEdit_, action.aiFallbackValue);
            SetChecked(aiWithImageCheck_, action.aiWithImage);
            SetChecked(aiLogicConvertCheck_, action.aiLogicConvert);
            SetChecked(aiRegionByImageCheck2_, action.aiRegionByImage);
            aiFindImagePath_ = action.aiTargetImagePath;
            if (action.aiRegionByImage) {
                SetText(aiFindMatchThreshold_, F3(action.matchThreshold));
                SetText(aiFindScaleMin_, F3(action.imageScaleMin > 0.0 ? action.imageScaleMin : 0.9));
                SetText(aiFindScaleMax_, F3(action.imageScaleMax > 0.0 ? action.imageScaleMax : 1.1));
                SetText(aiImageRegionX1_, std::to_wstring(action.imageRegionX1));
                SetText(aiImageRegionY1_, std::to_wstring(action.imageRegionY1));
                SetText(aiImageRegionX2_, std::to_wstring(action.imageRegionX2));
                SetText(aiImageRegionY2_, std::to_wstring(action.imageRegionY2));
            }
            UpdateAiFindImagePreview();
            if (action.searchFullScreen) {
                ApplyAiFullScreen();
            } else {
                SetText(aiSearchX1Edit2_, std::to_wstring(action.aiSearchX1));
                SetText(aiSearchY1Edit2_, std::to_wstring(action.aiSearchY1));
                SetText(aiSearchX2Edit2_, std::to_wstring(action.aiSearchX2));
                SetText(aiSearchY2Edit2_, std::to_wstring(action.aiSearchY2));
                RefreshCoordFieldEdits({aiSearchX1Edit2_, aiSearchY1Edit2_, aiSearchX2Edit2_, aiSearchY2Edit2_});
            }
            SetText(aiMaxStepsEdit_, std::to_wstring(action.aiMaxSteps));
        }
        else { SetPopupSel(popupAction_, actionCombo_, 14); }
        RefreshParamPanel();
        loadingForm_ = false;
        RECT actionRc = WindowClientRect(actionCombo_);
        RECT clientRc = WindowClientRect(hwnd_);
        RECT panelRc{actionRc.left, actionRc.bottom + UiLen(4), clientRc.right,
            clientRc.bottom - UiLen(kBottomH)};
        InvalidateRect(hwnd_, &panelRc, FALSE);
    }

// was engine_host_window.h:8693-8708
bool EngineHost::WriteImportedJson(const std::wstring& target, std::wstring content) {
        content = UpdateJsonStringField(content, L"recordTime", NowText());
        if (content.find(L"\"recordTime\"") == std::wstring::npos) {
            const auto brace = content.find(L'{');
            if (brace != std::wstring::npos) {
                const std::wstring insert = L"\n  \"recordTime\": \"" + EscapeJson(NowText()) + L"\",";
                content.insert(brace + 1, insert);
            }
        }
        std::ofstream out(target, std::ios::binary);
        if (!out) return false;
        out.write("\xEF\xBB\xBF", 3);
        const auto bytes = ToUtf8(content);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return out.good();
    }

// was engine_host_window.h:8750-8872
void EngineHost::ImportScriptFromZipFile(const std::wstring& zipPath, bool toRecordings) {
        // 先读取 ZIP 中的 JSON 内容
        std::string jsonUtf8 = ReadTextFromZip(zipPath, "script.json");
        if (jsonUtf8.empty()) {
            ShowPromptInfo(L"导入失败：ZIP 文件中未找到 script.json。");
            return;
        }
        const std::wstring content = FromUtf8(jsonUtf8);
        std::wstring name = ExtractString(content, L"scriptName");
        if (name.empty()) {
            ShowPromptInfo(L"导入失败：文件格式不正确，未找到脚本名称。");
            return;
        }

        // 解压 ZIP 到临时目录
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring extractDir = std::wstring(tempDir) + L"qs_import_" + std::to_wstring(GetTickCount());
        int extracted = ExtractZipFile(zipPath, extractDir);
        if (extracted < 0) {
            ShowPromptInfo(L"导入失败：无法解压 ZIP 文件。");
            return;
        }

        // 复制图片到 images 目录并重映射
        EnsureFindImagesDir();
        std::wstring modifiedContent = content;
        const auto imgDir = FindImagesDir();

        // 收集 ZIP 中解压出的图片文件名（排除 script.json）
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = extractDir + L"\\*";
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring extractedFile = extractDir + L"\\" + fd.cFileName;
                std::wstring fileName(fd.cFileName);
                // 跳过 script.json
                if (_wcsicmp(fileName.c_str(), L"script.json") == 0) continue;
                // 检查是否为图片（.bmp, .png, .jpg）
                auto dotPos = fileName.find_last_of(L'.');
                if (dotPos == std::wstring::npos) continue;
                std::wstring ext = fileName.substr(dotPos);
                std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
                if (ext != L".bmp" && ext != L".png" && ext != L".jpg" && ext != L".jpeg") continue;

                // 复制到 images 目录
                std::wstring destPath = imgDir + L"\\" + fileName;
                // 如果目标已存在，生成新文件名
                if (GetFileAttributesW(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    const auto nameNoExt = fileName.substr(0, dotPos);
                    destPath = imgDir + L"\\" + nameNoExt + L"_" + std::to_wstring(GetTickCount()) + ext;
                }
                if (!CopyFileW(extractedFile.c_str(), destPath.c_str(), FALSE)) continue;

                const std::wstring relPath = ImagePathForJson(destPath);
                const auto blocks = ExtractJsonActionBlocks(modifiedContent);
                for (const auto& block : blocks) {
                    const auto type = ExtractString(block, L"type");
                    const bool usesImage = type == L"findImage"
                        || (type == L"textRecognition"
                            && ExtractNumber(block, L"ocrRegionByImage", 0) != 0);
                    if (!usesImage) continue;
                    const auto oldImgPath = ExtractString(block, L"imagePath");
                    if (oldImgPath.empty()) continue;
                    const auto oldSlash = oldImgPath.find_last_of(L"\\/");
                    const std::wstring oldFileName = (oldSlash == std::wstring::npos)
                        ? oldImgPath : oldImgPath.substr(oldSlash + 1);
                    const auto destSlash = destPath.find_last_of(L"\\/");
                    const std::wstring destFileName = (destSlash == std::wstring::npos)
                        ? destPath : destPath.substr(destSlash + 1);
                    if (_wcsicmp(oldFileName.c_str(), fileName.c_str()) == 0 ||
                        _wcsicmp(oldFileName.c_str(), destFileName.c_str()) == 0) {
                        const auto key = L"\"imagePath\": \"" + EscapeJson(oldImgPath) + L"\"";
                        const auto pos = modifiedContent.find(key);
                        if (pos != std::wstring::npos) {
                            modifiedContent.replace(pos + 14, EscapeJson(oldImgPath).size(), EscapeJson(relPath));
                        }
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        // 清理临时目录
        RemoveDirectoryW(extractDir.c_str());
        // 也删除临时文件
        WIN32_FIND_DATAW fd2{};
        const std::wstring cleanPattern = extractDir + L"\\*";
        HANDLE hFind2 = FindFirstFileW(cleanPattern.c_str(), &fd2);
        if (hFind2 != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    DeleteFileW((extractDir + L"\\" + fd2.cFileName).c_str());
                }
            } while (FindNextFileW(hFind2, &fd2));
            FindClose(hFind2);
        }
        RemoveDirectoryW(extractDir.c_str());

        // 保存修改后的 JSON 到目标目录
        ScriptFileData importData = ParseScriptContent(modifiedContent);
        if (importData.scriptName.empty()) importData.scriptName = name;
        if (toRecordings) {
            importData.windowMode = windowmode::DefaultWindowModeConfig();
            importData.breakoutTimeSeconds = 0;
        }
        importData.recordTime = NowText();
        EnsureScriptsDir();
        const auto baseDir = ImportTargetDir(toRecordings);
        std::wstring target = baseDir + L"\\" + SafeScriptFileName(importData.scriptName) + L".json";
        int suffix = 1;
        while (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            target = baseDir + L"\\" + SafeScriptFileName(importData.scriptName)
                + L"-" + std::to_wstring(suffix++) + L".json";
        }
        if (!SaveScriptFileData(target, importData)) {
            ShowPromptInfo(L"导入失败：无法写入目标目录。");
            return;
        }
        FinishImport(toRecordings);
    }

// was engine_host_window.h:9485-9729
void EngineHost::ShowCustomIntervalDialog() {
        CloseClickerDropPopup();
        UiScaleInitFromHwnd(hwnd_);
        struct DialogState {
            double val = 0.0;
            bool ok = false;
            bool done = false;
            HWND edit = nullptr;
            bool hoverClose = false;
            bool hoverCancel = false;
            bool hoverOk = false;
            HFONT titleFont = nullptr;
            HFONT bodyFont = nullptr;
            HFONT closeFont = nullptr;
        };
        DialogState state{};
        state.val = clickerSettings_.customIntervalSeconds;
        const int dlgW = UiLen(420);
        const int dlgH = UiLen(225);
        const wchar_t* clsName = L"QuickScriptCustomIntervalDlg";
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                auto cancelRect = []() { return RECT{UiLen(108), UiLen(168), UiLen(198), UiLen(206)}; };
                auto okRect = []() { return RECT{UiLen(222), UiLen(168), UiLen(312), UiLen(206)}; };
                auto closeRect = []() { return RECT{UiLen(380), 0, UiLen(420), UiLen(kTitleH)}; };
                auto editOuterRect = []() {
                    const int bodyTop = UiLen(38);
                    const int bodyBottom = UiLen(168);
                    const int rowH = UiLen(38);
                    const int editW = UiLen(140);
                    const int gap = UiLen(10);
                    const int unitW = UiLen(32);
                    const int groupW = editW + gap + unitW;
                    const int groupLeft = (UiLen(420) - groupW) / 2;
                    const int rowTop = bodyTop + (bodyBottom - bodyTop - rowH) / 2;
                    return RECT{groupLeft, rowTop, groupLeft + editW, rowTop + rowH};
                };
                auto unitRect = [&]() {
                    RECT edit = editOuterRect();
                    return RECT{edit.right + UiLen(10), edit.top, edit.right + UiLen(42), edit.bottom};
                };
                auto centerEditText = [](HWND edit) {
                    CenterModernSingleLineEditText(edit);
                };
                if (msg == WM_CREATE) {
                    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    st = reinterpret_cast<DialogState*>(cs->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                    st->titleFont = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    st->bodyFont = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    st->closeFont = CreateFontW(UiFontHeight(36), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    const RECT editOuter = editOuterRect();
                    st->edit = MakeModernSingleLineEdit(hwnd, nullptr, 100,
                        editOuter.left + 1, editOuter.top + 1,
                        editOuter.right - editOuter.left - 2, editOuter.bottom - editOuter.top - 2,
                        ES_CENTER);
                    ApplyModernEditBehavior(st->edit, false);
                    SendMessageW(st->edit, WM_SETFONT, reinterpret_cast<WPARAM>(st->bodyFont), TRUE);
                    wchar_t buf[32]{};
                    swprintf_s(buf, L"%.3f", st->val);
                    SetWindowTextW(st->edit, buf);
                    centerEditText(st->edit);
                    return 0;
                }
                if (!st) return DefWindowProcW(hwnd, msg, wp, lp);
                if (msg == WM_ERASEBKGND) return 1;
                if (msg == WM_SETCURSOR) {
                    if (LOWORD(lp) == HTCLIENT) {
                        POINT pt{};
                        GetCursorPos(&pt);
                        ScreenToClient(hwnd, &pt);
                        const bool hand = st->hoverClose || st->hoverCancel || st->hoverOk;
                        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                        return TRUE;
                    }
                    return DefWindowProcW(hwnd, msg, wp, lp);
                }
                if (msg == WM_MOUSEMOVE) {
                    const int x = GET_X_LPARAM(lp);
                    const int y = GET_Y_LPARAM(lp);
                    const RECT closeR = closeRect();
                    const RECT cancelR = cancelRect();
                    const RECT okR = okRect();
                    const bool hc = PtInRect(&closeR, POINT{x, y});
                    const bool hcan = PtInRect(&cancelR, POINT{x, y});
                    const bool hok = PtInRect(&okR, POINT{x, y});
                    if (hc != st->hoverClose || hcan != st->hoverCancel || hok != st->hoverOk) {
                        st->hoverClose = hc;
                        st->hoverCancel = hcan;
                        st->hoverOk = hok;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                if (msg == WM_CTLCOLOREDIT) {
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    SetBkColor(hdc, kWhite);
                    SetTextColor(hdc, kText);
                    static HBRUSH brush = CreateSolidBrush(kWhite);
                    return reinterpret_cast<LRESULT>(brush);
                }
                if (msg == WM_PAINT) {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(hwnd, &ps);
                    RECT client{}; GetClientRect(hwnd, &client);
                    const int th = UiLen(kTitleH);
                    RECT titleRc{0, 0, client.right, th};
                    FillRectColor(hdc, titleRc, kMainGreen);
                    FillRectColor(hdc, RECT{0, th, client.right, client.bottom}, kWhite);
                    if (st->hoverClose) FillRectColor(hdc, closeRect(), kCloseHover);
                    HGDIOBJ oldFont = SelectObject(hdc, st->titleFont);
                    ::DrawTextIn(hdc, L"  键鼠工坊-自定义连点间隔", titleRc, kWhite);
                    SelectObject(hdc, st->closeFont);
                    ::DrawTextIn(hdc, L"×", closeRect(), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, st->bodyFont);
                    DrawBorderRect(hdc, editOuterRect(), kComboBorderGray);
                    ::DrawTextIn(hdc, L"秒", unitRect(), kText);
                    const RECT cancel = cancelRect();
                    const RECT ok = okRect();
                    if (st->hoverCancel) FillRectColor(hdc, cancel, kComboHoverGreen);
                    DrawBorderRoundRect(hdc, cancel, kMainGreen, UiLen(6));
                    ::DrawTextIn(hdc, L"取消", cancel, st->hoverCancel ? kDarkGreen : kMainGreen,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    StDrawGreenButton(hdc, st->bodyFont, ok, L"确定", st->hoverOk);
                    SelectObject(hdc, oldFont);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                if (msg == WM_LBUTTONDOWN) {
                    const int x = GET_X_LPARAM(lp);
                    const int y = GET_Y_LPARAM(lp);
                    const RECT closeR = closeRect();
                    const RECT cancelR = cancelRect();
                    const RECT okR = okRect();
                    if (PtInRect(&closeR, POINT{x, y}) || PtInRect(&cancelR, POINT{x, y})) {
                        st->done = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                    if (PtInRect(&okR, POINT{x, y})) {
                        wchar_t buf[64]{};
                        GetWindowTextW(st->edit, buf, 63);
                        try { st->val = std::stod(buf); st->ok = st->val > 0.0; } catch (...) { st->ok = false; }
                        st->done = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }
                if (msg == WM_KEYDOWN && wp == VK_RETURN) {
                    wchar_t buf[64]{};
                    GetWindowTextW(st->edit, buf, 63);
                    try { st->val = std::stod(buf); st->ok = st->val > 0.0; } catch (...) { st->ok = false; }
                    st->done = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (msg == WM_DESTROY) {
                    if (st->titleFont) { DeleteObject(st->titleFont); st->titleFont = nullptr; }
                    if (st->bodyFont) { DeleteObject(st->bodyFont); st->bodyFont = nullptr; }
                    if (st->closeFont) { DeleteObject(st->closeFont); st->closeFont = nullptr; }
                    st->done = true;
                    return 0;
                }
                if (msg == WM_CLOSE) { st->done = true; DestroyWindow(hwnd); return 0; }
                return DefWindowProcW(hwnd, msg, wp, lp);
            };
            wc.hInstance = g_instance;
            wc.lpszClassName = clsName;
            wc.hbrBackground = nullptr;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            registered = true;
        }
        RECT ownerClient{};
        GetClientRect(hwnd_, &ownerClient);
        POINT ownerTl{0, 0};
        ClientToScreen(hwnd_, &ownerTl);
        const RECT ownerScreen{
            ownerTl.x, ownerTl.y,
            ownerTl.x + ownerClient.right, ownerTl.y + ownerClient.bottom
        };
        const int ownerW = ownerScreen.right - ownerScreen.left;
        const int ownerH = ownerScreen.bottom - ownerScreen.top;
        const int dlgX = ownerScreen.left + (ownerW - dlgW) / 2;
        const int dlgY = ownerScreen.top + (ownerH - dlgH) / 2;

        static constexpr BYTE kOverlayAlpha = 145;
        static constexpr wchar_t kOverlayClass[] = L"QuickScriptCustomIntervalOverlay";
        static bool overlayRegistered = false;
        if (!overlayRegistered) {
            WNDCLASSW owc{};
            owc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                if (msg == WM_ERASEBKGND) return 1;
                if (msg == WM_PAINT) {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(hwnd, &ps);
                    RECT rc{};
                    GetClientRect(hwnd, &rc);
                    ::FillAlphaRect(hdc, rc, RGB(0, 0, 0), kOverlayAlpha);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            };
            owc.hInstance = g_instance;
            owc.lpszClassName = kOverlayClass;
            owc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&owc);
            overlayRegistered = true;
        }
        HWND overlay = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kOverlayClass, L"", WS_POPUP,
            ownerScreen.left, ownerScreen.top, ownerW, ownerH,
            hwnd_, nullptr, g_instance, nullptr);
        ShowWindow(overlay, SW_SHOWNA);
        UpdateWindow(overlay);

        HWND dlg = CreateWindowExW(WS_EX_TOPMOST, clsName, L"", WS_POPUP,
            dlgX, dlgY, dlgW, dlgH, hwnd_, nullptr, g_instance, &state);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        SetForegroundWindow(dlg);
        SetFocus(state.edit);
        MSG msg{};
        while (!state.done && IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        if (IsWindow(overlay)) DestroyWindow(overlay);
        SetForegroundWindow(hwnd_);
        if (state.ok) {
            clickerSettings_.intervalMode = quickscript::ClickIntervalMode::Custom;
            clickerSettings_.customIntervalSeconds = state.val;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

// was engine_host_window.h:9817-10064
void EngineHost::ShowRenameRecordingDialog(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        UiScaleInitFromHwnd(hwnd_);
        struct DialogState {
            std::wstring name;
            bool ok = false;
            bool done = false;
            HWND edit = nullptr;
            bool hoverClose = false;
            bool hoverCancel = false;
            bool hoverOk = false;
            HFONT titleFont = nullptr;
            HFONT bodyFont = nullptr;
            HFONT closeFont = nullptr;
        };
        DialogState state{};
        state.name = recordings_[static_cast<size_t>(index)].name;
        const int dlgW = UiLen(420);
        const int dlgH = UiLen(225);
        const wchar_t* clsName = L"QuickScriptRenameRecordingDlg";
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                auto cancelRect = []() { return RECT{UiLen(108), UiLen(168), UiLen(198), UiLen(206)}; };
                auto okRect = []() { return RECT{UiLen(222), UiLen(168), UiLen(312), UiLen(206)}; };
                auto closeRect = []() { return RECT{UiLen(380), 0, UiLen(420), UiLen(kTitleH)}; };
                auto editOuterRect = []() { return RECT{UiLen(40), UiLen(78), UiLen(380), UiLen(116)}; };
                auto centerEditText = [](HWND edit) {
                    CenterModernSingleLineEditText(edit);
                };
                if (msg == WM_CREATE) {
                    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    st = reinterpret_cast<DialogState*>(cs->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                    st->titleFont = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    st->bodyFont = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    st->closeFont = CreateFontW(UiFontHeight(36), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    const RECT editOuter = editOuterRect();
                    st->edit = MakeModernSingleLineEdit(hwnd, nullptr, 100,
                        editOuter.left + 1, editOuter.top + 1,
                        editOuter.right - editOuter.left - 2, editOuter.bottom - editOuter.top - 2);
                    ApplyModernEditBehavior(st->edit, false);
                    SendMessageW(st->edit, WM_SETFONT, reinterpret_cast<WPARAM>(st->bodyFont), TRUE);
                    SetWindowTextW(st->edit, st->name.c_str());
                    centerEditText(st->edit);
                    return 0;
                }
                if (!st) return DefWindowProcW(hwnd, msg, wp, lp);
                if (msg == WM_ERASEBKGND) return 1;
                if (msg == WM_SETCURSOR) {
                    if (LOWORD(lp) == HTCLIENT) {
                        const bool hand = st->hoverClose || st->hoverCancel || st->hoverOk;
                        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                        return TRUE;
                    }
                    return DefWindowProcW(hwnd, msg, wp, lp);
                }
                if (msg == WM_MOUSEMOVE) {
                    const int x = GET_X_LPARAM(lp);
                    const int y = GET_Y_LPARAM(lp);
                    const RECT closeR = closeRect();
                    const RECT cancelR = cancelRect();
                    const RECT okR = okRect();
                    const bool hc = PtInRect(&closeR, POINT{x, y});
                    const bool hcan = PtInRect(&cancelR, POINT{x, y});
                    const bool hok = PtInRect(&okR, POINT{x, y});
                    if (hc != st->hoverClose || hcan != st->hoverCancel || hok != st->hoverOk) {
                        // 只刷按钮区，避免整窗 Invalidate 导致编辑框一起闪
                        auto dirty = [](HWND hwnd, const RECT& a, const RECT& b) {
                            RECT u{};
                            UnionRect(&u, &a, &b);
                            InvalidateRect(hwnd, &u, FALSE);
                        };
                        if (hc != st->hoverClose) dirty(hwnd, closeR, closeR);
                        if (hcan != st->hoverCancel) dirty(hwnd, cancelR, cancelR);
                        if (hok != st->hoverOk) dirty(hwnd, okR, okR);
                        st->hoverClose = hc;
                        st->hoverCancel = hcan;
                        st->hoverOk = hok;
                    }
                    return 0;
                }
                if (msg == WM_CTLCOLOREDIT) {
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    SetBkColor(hdc, kWhite);
                    SetTextColor(hdc, kText);
                    static HBRUSH brush = CreateSolidBrush(kWhite);
                    return reinterpret_cast<LRESULT>(brush);
                }
                if (msg == WM_PAINT) {
                    PAINTSTRUCT ps{};
                    HDC windowDc = BeginPaint(hwnd, &ps);
                    RECT client{}; GetClientRect(hwnd, &client);
                    const int cw = client.right - client.left;
                    const int ch = client.bottom - client.top;
                    HDC hdc = CreateCompatibleDC(windowDc);
                    HBITMAP bmp = CreateCompatibleBitmap(windowDc, cw, ch);
                    HGDIOBJ oldBmp = SelectObject(hdc, bmp);
                    const int th = UiLen(kTitleH);
                    RECT titleRc{0, 0, client.right, th};
                    FillRectColor(hdc, titleRc, kMainGreen);
                    FillRectColor(hdc, RECT{0, th, client.right, client.bottom}, kWhite);
                    if (st->hoverClose) FillRectColor(hdc, closeRect(), kCloseHover);
                    HGDIOBJ oldFont = SelectObject(hdc, st->titleFont);
                    ::DrawTextIn(hdc, L"  键鼠工坊-重命名", titleRc, kWhite);
                    SelectObject(hdc, st->closeFont);
                    ::DrawTextIn(hdc, L"×", closeRect(), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, st->bodyFont);
                    DrawBorderRect(hdc, editOuterRect(), kComboBorderGray);
                    const RECT cancel = cancelRect();
                    const RECT ok = okRect();
                    if (st->hoverCancel) FillRectColor(hdc, cancel, kComboHoverGreen);
                    DrawBorderRoundRect(hdc, cancel, kMainGreen, UiLen(6));
                    ::DrawTextIn(hdc, L"取消", cancel, st->hoverCancel ? kDarkGreen : kMainGreen,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    StDrawGreenButton(hdc, st->bodyFont, ok, L"确定", st->hoverOk);
                    SelectObject(hdc, oldFont);
                    const int blitW = ps.rcPaint.right - ps.rcPaint.left;
                    const int blitH = ps.rcPaint.bottom - ps.rcPaint.top;
                    BitBlt(windowDc, ps.rcPaint.left, ps.rcPaint.top, blitW, blitH,
                        hdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                    SelectObject(hdc, oldBmp);
                    DeleteObject(bmp);
                    DeleteDC(hdc);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                if (msg == WM_LBUTTONDOWN) {
                    const int x = GET_X_LPARAM(lp);
                    const int y = GET_Y_LPARAM(lp);
                    const RECT closeR = closeRect();
                    const RECT cancelR = cancelRect();
                    const RECT okR = okRect();
                    if (PtInRect(&closeR, POINT{x, y}) || PtInRect(&cancelR, POINT{x, y})) {
                        st->done = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                    if (PtInRect(&okR, POINT{x, y})) {
                        wchar_t buf[256]{};
                        GetWindowTextW(st->edit, buf, 255);
                        st->name = buf;
                        st->ok = !Trim(st->name).empty();
                        st->done = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }
                if (msg == WM_KEYDOWN && wp == VK_RETURN) {
                    wchar_t buf[256]{};
                    GetWindowTextW(st->edit, buf, 255);
                    st->name = buf;
                    st->ok = !Trim(st->name).empty();
                    st->done = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (msg == WM_DESTROY) {
                    if (st->titleFont) { DeleteObject(st->titleFont); st->titleFont = nullptr; }
                    if (st->bodyFont) { DeleteObject(st->bodyFont); st->bodyFont = nullptr; }
                    if (st->closeFont) { DeleteObject(st->closeFont); st->closeFont = nullptr; }
                    st->done = true;
                    return 0;
                }
                if (msg == WM_CLOSE) { st->done = true; DestroyWindow(hwnd); return 0; }
                return DefWindowProcW(hwnd, msg, wp, lp);
            };
            wc.hInstance = g_instance;
            wc.lpszClassName = clsName;
            wc.hbrBackground = nullptr;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            registered = true;
        }

        RECT ownerClient{};
        GetClientRect(hwnd_, &ownerClient);
        POINT ownerTl{0, 0};
        ClientToScreen(hwnd_, &ownerTl);
        const RECT ownerScreen{
            ownerTl.x, ownerTl.y,
            ownerTl.x + ownerClient.right, ownerTl.y + ownerClient.bottom
        };
        const int ownerW = ownerScreen.right - ownerScreen.left;
        const int ownerH = ownerScreen.bottom - ownerScreen.top;
        const int dlgX = ownerScreen.left + (ownerW - dlgW) / 2;
        const int dlgY = ownerScreen.top + (ownerH - dlgH) / 2;

        // 与「自定义连点间隔」一致：半透明遮罩，不用 EnableWindow（关窗时主界面会闪）
        static constexpr BYTE kOverlayAlpha = 145;
        static constexpr wchar_t kOverlayClass[] = L"QuickScriptRenameRecordingOverlay";
        static bool overlayRegistered = false;
        if (!overlayRegistered) {
            WNDCLASSW owc{};
            owc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                if (msg == WM_ERASEBKGND) return 1;
                if (msg == WM_PAINT) {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(hwnd, &ps);
                    RECT rc{};
                    GetClientRect(hwnd, &rc);
                    ::FillAlphaRect(hdc, rc, RGB(0, 0, 0), kOverlayAlpha);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            };
            owc.hInstance = g_instance;
            owc.lpszClassName = kOverlayClass;
            owc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&owc);
            overlayRegistered = true;
        }
        HWND overlay = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kOverlayClass, L"", WS_POPUP,
            ownerScreen.left, ownerScreen.top, ownerW, ownerH,
            hwnd_, nullptr, g_instance, nullptr);
        ShowWindow(overlay, SW_SHOWNA);
        UpdateWindow(overlay);

        HWND dlg = CreateWindowExW(WS_EX_TOPMOST, clsName, L"", WS_POPUP | WS_CLIPCHILDREN,
            dlgX, dlgY, dlgW, dlgH, hwnd_, nullptr, g_instance, &state);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        SetForegroundWindow(dlg);
        SetFocus(state.edit);
        SendMessageW(state.edit, EM_SETSEL, 0, -1);
        MSG msg{};
        while (!state.done && IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        if (IsWindow(overlay)) DestroyWindow(overlay);
        SetForegroundWindow(hwnd_);
        StDiscardSpuriousInputAfterModal(hwnd_);
        if (state.ok) {
            PersistRecordingRename(index, state.name);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

// was engine_host_window.h:5146-5281
void EngineHost::RefreshOcrSubPanel() {
        HideOcrDynamicRegionControls();
        const bool searchMode = popupOcrResultMode_.sel == 1;
        const bool saveVar = popupOcrFollowUp_.sel == 2;
        const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
        if (searchMode) {
            for (HWND h : ocrSearchControls_) {
                if (h) ShowWindow(h, SW_SHOW);
            }
        } else {
            ParkParamGroup(ocrSearchControls_);
        }
        if (saveVar) {
            ParkParamGroup(ocrFollowOffsetControls_);
            for (HWND h : ocrFollowVarControls_) {
                if (h) ShowWindow(h, SW_SHOW);
            }
        } else {
            ParkParamGroup(ocrFollowVarControls_);
            for (HWND h : ocrFollowOffsetControls_) {
                if (h) ShowWindow(h, SW_SHOW);
            }
        }
        // 布局 186 仅用于创建控件；显示/定位由下方动态堆叠负责，避免静态坐标与复选框行重叠
        ShowParamLayout(186, false);
        // ShowParamLayout(18) 会把识别区域/找图控件短暂显示在静态坐标，须再次隐藏后再动态堆叠
        HideOcrDynamicRegionControls();
        if (ocrTestBtn_) ShowWindow(ocrTestBtn_, SW_HIDE);
        if (editorPopupOpen_ == 18) CloseEditorPopup();
        if (ocrUntilFound_) ShowWindow(ocrUntilFound_, searchMode ? SW_SHOW : SW_HIDE);

        const int rowGap = OcrScaleY(kFindVGap);
        const int btnH = OcrScaleY(kFindBtnH);
        const int fieldH = OcrScaleY(22);

        int y = OcrContentStartY();
        if (ocrRegionByImageCheck_) {
            // MoveOcrAt 内部会再 Scale，此处必须传设计像素，勿传已缩放的 fieldH
            MoveOcrAt(ocrRegionByImageCheck_, kFindContentLeft, y, kFindBlockW, 22);
            ShowWindow(ocrRegionByImageCheck_, SW_SHOW);
            y += fieldH + rowGap;
        }
        if (ocrDigitsOnlyCheck_) {
            MoveOcrAt(ocrDigitsOnlyCheck_, kFindContentLeft, y, kFindBlockW, 22);
            ShowWindow(ocrDigitsOnlyCheck_, SW_SHOW);
            y += fieldH + rowGap;
        }

        if (regionByImage) {
            y += rowGap;
            y = LayoutOcrFindImageHeaderRow(y);
            y = LayoutOcrFindRegionBlock(y);
            if (ocrFindImagePreviewBtn_) {
                SetWindowPos(ocrFindImagePreviewBtn_, HWND_BOTTOM, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            ShowFindImageSideControls(
                ocrFindImagePreviewBtn_, ocrFindScreenshotBtn_, ocrFindLocalImageBtn_, ocrFindClearImageBtn_);
            RaiseOcrFindImageHeaderControls();
            RefreshGrayButtonsInParamViewport();
        }

        // 识别区域始终为绝对屏幕坐标（根据图片时其上方另有相对偏移控件）
        y = LayoutOcrRegionModeRow(y);
        RaiseOcrRegionButtons();
        RefreshGrayButtonsInParamViewport();

        y = LayoutParamCoordRows(
            y,
            ocrX1Label_, ocrX1_, ocrY1Label_, ocrY1_,
            ocrX2Label_, ocrX2_, ocrY2Label_, ocrY2_);

        MoveOcrAt(ocrResultModeLabel_, kFindContentLeft, y, kFindFollowLabelW, kFindBtnH);
        MoveOcrAt(ocrResultModeCombo_, kFindContentLeft + kFindFollowLabelW + 8, y, kFindFollowComboW, kFindBtnH);
        if (ocrResultModeLabel_) ShowWindow(ocrResultModeLabel_, SW_SHOW);
        y += btnH + rowGap;

        if (searchMode) {
            y = LayoutOcrSearchBlock(y, rowGap, btnH, fieldH);
        }

        MoveOcrAt(ocrFollowUpLabel_, kFindContentLeft, y, kFindFollowLabelW, kFindBtnH);
        MoveOcrAt(ocrFollowUpCombo_, kFindContentLeft + kFindFollowLabelW + 8, y, kFindFollowComboW, kFindBtnH);
        if (ocrFollowUpLabel_) ShowWindow(ocrFollowUpLabel_, SW_SHOW);
        y += btnH + rowGap;

        if (!saveVar) {
            if (ocrOffsetXLabel_) ShowWindow(ocrOffsetXLabel_, SW_SHOW);
            if (ocrOffsetYLabel_) ShowWindow(ocrOffsetYLabel_, SW_SHOW);
            if (ocrResultVarLabel_) ShowWindow(ocrResultVarLabel_, SW_HIDE);
            y = LayoutParamCoordPairRow(y, ocrOffsetXLabel_, ocrOffsetX_, ocrOffsetYLabel_, ocrOffsetY_, kFindOffsetLabelW);
        } else {
            if (ocrOffsetXLabel_) ShowWindow(ocrOffsetXLabel_, SW_HIDE);
            if (ocrOffsetYLabel_) ShowWindow(ocrOffsetYLabel_, SW_HIDE);
            if (ocrResultVarLabel_) ShowWindow(ocrResultVarLabel_, SW_SHOW);
            if (ocrSelectOffsetBtn_) ShowWindow(ocrSelectOffsetBtn_, SW_HIDE);
            MoveOcrAt(ocrResultVarLabel_, kFindContentLeft, y, 100, 22);
            MoveOcrAt(ocrResultVar_, kFindContentLeft + 91, y, kOcrResultVarEditW, 22);
            y += fieldH + rowGap;
        }

        if (searchMode) {
            const int compactRowH = std::max(fieldH, OcrScale(kOcrCompactBtnH));
            const int testX = kOcrPanelRight - kOcrTestBtnW;
            if (ocrUntilFound_) {
                MoveOcrAt(ocrUntilFound_, kFindContentLeft, y + (compactRowH - fieldH) / 2, 140, 22);
            }
            if (ocrSelectOffsetBtn_) ShowWindow(ocrSelectOffsetBtn_, SW_HIDE);
            MoveOcrAt(ocrTestBtn_, testX, y + (compactRowH - OcrScale(kOcrCompactBtnH)) / 2, kOcrTestBtnW, kOcrCompactBtnH);
            y += compactRowH + rowGap;
        } else if (!saveVar) {
            const int rowY = y;
            const int left = OcrScaleX(kFindContentLeft);
            const int maxRight = ParamScrollContentRight() - OcrScaleX(4);
            const int gap = OcrScaleX(8);
            const int testW = std::min(OcrScaleX(kOcrTestBtnW), std::max(OcrScaleX(48), (maxRight - left) / 4));
            const int offsetW = std::min(OcrScaleX(kFindSelectOffsetW), std::max(OcrScaleX(48), maxRight - left - testW - gap));
            const int rowH = btnH;
            if (ocrSelectOffsetBtn_) {
                ShowWindow(ocrSelectOffsetBtn_, SW_SHOW);
                SetParamPosAware(ocrSelectOffsetBtn_, left, rowY, offsetW, rowH, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (ocrTestBtn_) {
                ShowWindow(ocrTestBtn_, SW_SHOW);
                SetParamPosAware(ocrTestBtn_, left + offsetW + gap, rowY, std::min(testW, maxRight - left - offsetW - gap), rowH,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            y += rowH + rowGap;
        } else {
            MoveOcrAt(ocrTestBtn_, kFindContentLeft, y, kFindBtnW, kFindBtnH);
            y += btnH + rowGap;
        }
        // 勿单独 Invalidate viewport：异步 WM_PAINT 白底会盖住灰按钮上边框；
        // SyncParamScrollLayout → RepaintParamPanelChrome / PostLayoutParamPanelRedraw 已统一刷新。
        paramScrollY_ = 0;
        SyncParamScrollLayout(y);
    }

// was engine_host_window.h:3250-3355
void EngineHost::RefreshParamPanel() {
        const int sel = popupAction_.sel;
        if (sel != 12) CancelQuickInputTip();
        if (!UsesDynamicParamPanel(sel)) paramLayoutBottomHint_ = -1;

        // 先隐藏所有 Combo 标签，再由 ShowParamLayout 显示正确的
        HideEditorComboHwnds();
        HideInactiveParamControls();

        // ── 主面板切换 (基于布局索引) ──
        for (int i = 0; i <= 31; ++i) {
            if (i == 29) continue;
            ShowParamLayout(i, i == sel);
        }
        ShowParamLayout(29, sel == 29 || sel == 30 || sel == 31);
        ShowParamLayout(300, sel == 30);
        ShowParamLayout(310, sel == 31);
        ShowParamLayout(32, sel == 32);
        ShowParamLayout(33, sel == 33);
        ShowParamLayout(34, sel == 34);

        if (sel == 0) RefreshMoveParamPanel();

        // ── 共享布局 (sel 5/6 共用; 9/10 共用) ──
        ShowParamLayout(5, sel == 5 || sel == 6);
        ShowParamLayout(6, sel == 5 || sel == 6);
        ShowParamLayout(9, sel == 9 || sel == 10);
        ShowParamLayout(10, sel == 9 || sel == 10);

        // ── 找图子面板 (位置 170/171) ──
        if (sel == 17) {
            RefreshFindImageSubPanel();
            if (!loadingForm_ && selectedIndex_ < 0) EnsureFindImageRegionDefaults();
        } else {
            ShowParamLayout(170, false);
            ShowParamLayout(171, false);
        }

        // ── 文字识别子面板 (位置 180-186) ──
        ShowParamLayout(180, sel == 18);
        if (sel == 18) {
            SetPopupSel(popupOcrResultMode_, ocrResultModeCombo_,
                std::clamp(popupOcrResultMode_.sel, 0, static_cast<int>(popupOcrResultMode_.items.size()) - 1));
            SetPopupSel(popupOcrFollowUp_, ocrFollowUpCombo_,
                std::clamp(popupOcrFollowUp_.sel, 0, static_cast<int>(popupOcrFollowUp_.items.size()) - 1));
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
            if (!loadingForm_ && selectedIndex_ < 0) EnsureOcrRegionDefaults();
        } else {
            ShowParamLayout(182, false);
            ShowParamLayout(184, false);
            ShowParamLayout(185, false);
            ShowParamLayout(186, false);
        }

        // ── 打开程序文件子面板 ──
        ShowParamLayout(240, sel == 24 && popupRunProgram_.sel <= 0);

        // ── AI 子面板 ──
        if (sel == 29 || sel == 30 || sel == 31) {
            RefreshAiModelCombo();
            SetPopupSel(popupAiContextMode_, aiContextModeCombo_, std::clamp(popupAiContextMode_.sel, 0, 3));
            if (sel != 31) {
                SetPopupSel(popupAiOutputType_, aiOutputTypeCombo_, std::clamp(popupAiOutputType_.sel, 0, 1));
            }
            RefreshAiSubPanel();
            if (!loadingForm_ && selectedIndex_ < 0) EnsureAiRegionDefaults();
        } else {
            ParkParamGroup(aiCommonControls_);
            ParkParamGroup(aiImageControls_);
            ParkParamGroup(aiActionControls_);
            HideAiFindRegionControls();
        }

        // ── 底部操作（备注/保存/取消） ──
        if (sel != 17 && sel != 18 && sel != 30 && sel != 31) ClearGrayButtonHover();
        if (sel == 3) RefreshMousePlaybackCombo();
        if (sel == 4) RefreshRunMacroCombo();
        if (sel == 16) RefreshRunBlockCombo();
        if (sel == 12 || sel == 29 || sel == 30 || sel == 31) {
            SyncSharedVarComboVisibility();
            RefreshActiveVarCombo();
        }
        if (sel == 18) RefreshOcrSearchVarCombo();
        if (sel == 19) RefreshIfVarCombo();

        UpdateMoveVarControls();
        UpdateLoopVarControls();
        paramScrollY_ = 0;
        if (UsesDynamicParamPanel(sel)) {
            ApplyParamScrollOffset(true);
            UpdateParamViewportGeometry();
            if (MaxParamScroll() <= 0 && hwnd_) {
                HDC dc = GetDC(hwnd_);
                ClearParamScrollTrack(dc);
                ReleaseDC(hwnd_, dc);
            }
        } else {
            ApplyParamScroll();
        }
        if (hwnd_) {
            RECT listRc = ActionListRect();
            InvalidateRect(hwnd_, &listRc, TRUE);
            RedrawWindow(hwnd_, &listRc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        }
    }

// was engine_host_window.h:3562-3677
void EngineHost::ApplyParamScrollOffset(bool repaintChrome, bool eraseViewport, bool hideOffscreen, bool postLayoutRedraw ) {
        const RECT contentVp = ParamScrollContentRect();
        const bool effectiveHideOffscreen = hideOffscreen && paramScrollY_ > 0;
        // 记录移位前可见控件区域，SWP_NOCOPYBITS 不会自动擦掉旧位置
        std::vector<RECT> staleVpRects;
        if (paramViewport_ && page_ == Page::Editor) {
            const RECT vp = ParamViewportRect();
            for (const auto& entry : paramScrollLayout_) {
                if (!entry.hwnd || IsIntentionallyHiddenParamControl(entry.hwnd)) continue;
                const bool pinFooter = IsFooterControl(entry.hwnd) && !UsesDynamicParamPanel();
                const int scrollY = pinFooter ? entry.baseY : entry.baseY - paramScrollY_;
                const RECT mainRc{entry.baseX, scrollY, entry.baseX + entry.baseW, scrollY + entry.baseH};
                RECT visible{};
                if (!IntersectRect(&visible, &mainRc, &contentVp)) continue;
                staleVpRects.push_back(RECT{
                    mainRc.left - vp.left, mainRc.top - vp.top,
                    mainRc.right - vp.left, mainRc.bottom - vp.top
                });
            }
        }
        // 滚动/重排前仅标记旧区域待重绘（不 erase，避免白底闪一下盖住子控件）
        if (eraseViewport && paramViewport_ && page_ == Page::Editor) {
            for (const RECT& r : staleVpRects)
                InvalidateRect(paramViewport_, &r, FALSE);
        }
        const bool dynamicPanelScroll = UsesDynamicParamPanel();
        HDWP hdwp = (!dynamicPanelScroll && !paramScrollLayout_.empty())
            ? BeginDeferWindowPos(static_cast<UINT>(paramScrollLayout_.size()))
            : nullptr;
        for (const auto& entry : paramScrollLayout_) {
            if (!entry.hwnd) continue;
            if (IsIntentionallyHiddenParamControl(entry.hwnd)) {
                ShowWindow(entry.hwnd, SW_HIDE);
                SetWindowRgn(entry.hwnd, nullptr, FALSE);
                if (dynamicPanelScroll) {
                    SetParamPosAware(entry.hwnd, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                } else if (hdwp) {
                    hdwp = DeferWindowPos(hdwp, entry.hwnd, nullptr, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                } else {
                    SetParamPosAware(entry.hwnd, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                }
                continue;
            }
            EnsureParamViewportParent(entry.hwnd);
            const bool pinFooter = IsFooterControl(entry.hwnd) && !dynamicPanelScroll;
            const int y = pinFooter ? entry.baseY : entry.baseY - paramScrollY_;
            const RECT ctrl{entry.baseX, y, entry.baseX + entry.baseW, y + entry.baseH};
            RECT visible{};
            if (effectiveHideOffscreen && !IsFooterControl(entry.hwnd)
                && !IntersectRect(&visible, &ctrl, &contentVp)) {
                ShowWindow(entry.hwnd, SW_HIDE);
                SetWindowRgn(entry.hwnd, nullptr, FALSE);
                if (dynamicPanelScroll) {
                    SetParamPosAware(entry.hwnd, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                } else if (hdwp) {
                    hdwp = DeferWindowPos(hdwp, entry.hwnd, nullptr, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                } else {
                    SetParamPosAware(entry.hwnd, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                }
                continue;
            }
            SetWindowRgn(entry.hwnd, nullptr, FALSE);
            const RECT vp = ParamViewportRect();
            const int targetX = GetParent(entry.hwnd) == paramViewport_ ? entry.baseX - vp.left : entry.baseX;
            const int targetY = GetParent(entry.hwnd) == paramViewport_ ? y - vp.top : y;
            if (dynamicPanelScroll) {
                SetParamPosAware(entry.hwnd, entry.baseX, y, entry.baseW, entry.baseH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            } else if (hdwp) {
                hdwp = DeferWindowPos(hdwp, entry.hwnd, nullptr, targetX, targetY, entry.baseW, entry.baseH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            } else {
                SetWindowPos(entry.hwnd, nullptr, targetX, targetY, entry.baseW, entry.baseH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            }
            if (IsEditorParamComboHwnd(entry.hwnd)) {
                ShowWindow(entry.hwnd, SW_HIDE);
            } else if (entry.hwnd == modifyBtn_) {
                ShowWindow(modifyBtn_, ShouldShowModifyButton() ? SW_SHOWNA : SW_HIDE);
            } else {
                ShowWindow(entry.hwnd, SW_SHOWNA);
            }
        }
        if (hdwp) EndDeferWindowPos(hdwp);
        for (HWND h : {remarkLabel_, remark_, modifyBtn_, addBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (paramViewport_ && page_ == Page::Editor) {
            for (const RECT& r : staleVpRects)
                InvalidateRect(paramViewport_, &r, FALSE);
            if (!postLayoutRedraw) {
                for (const auto& entry : paramScrollLayout_) {
                    if (!entry.hwnd || IsIntentionallyHiddenParamControl(entry.hwnd)) continue;
                    const bool pinFooter = IsFooterControl(entry.hwnd) && !UsesDynamicParamPanel();
                    const int y = pinFooter ? entry.baseY : entry.baseY - paramScrollY_;
                    const RECT ctrl{entry.baseX, y, entry.baseX + entry.baseW, y + entry.baseH};
                    RECT visible{};
                    if (!IntersectRect(&visible, &ctrl, &contentVp)) continue;
                    InvalidateParamControlInViewport(entry.hwnd);
                }
            }
        }
        // 滚轮轻量路径（erase/repaint 均为 false）跳过遮罩与取消/保存强制重绘，避免整区闪
        if (eraseViewport || repaintChrome) ApplyParamLayerMasks();
        if (editorPopupOpen_ >= 0) SyncEditorDropPopup();
        if (quickInputTipShown_ != QuickInputTipKind::None) SyncQuickInputTipPopup();
        if (repaintChrome) RepaintParamPanelChrome();
        if (postLayoutRedraw) PostLayoutParamPanelRedraw();
    }

// was engine_host_window.h:2356-2459
void EngineHost::CollectParamScrollControls(std::vector<HWND>& out) const {
        out.clear();
        const int sel = popupAction_.sel;
        auto appendGroup = [&](const std::vector<HWND>& group) {
            for (HWND h : group) {
                if (!h) continue;
                if (std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
            }
        };
        auto appendHwnd = [&](HWND h) {
            if (!h) return;
            if (std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
        };
        switch (sel) {
        case 0: appendGroup(moveControls_); break;
        case 34: appendGroup(moveRelControls_); break;
        case 1: appendGroup(waitControls_); break;
        case 2: appendGroup(clickControls_); break;
        case 3: appendGroup(mousePlaybackControls_); break;
        case 4: appendGroup(runMacroControls_); break;
        case 5: case 6: appendGroup(mousePressControls_); break;
        case 7: appendGroup(scrollWheelControls_); break;
        case 8: appendGroup(keyControls_); break;
        case 9: case 10: appendGroup(keyPressControls_); break;
        case 11: appendGroup(hotkeyShortcutControls_); break;
        case 12: appendGroup(quickInputControls_); break;
        case 13: appendGroup(loopControls_); break;
        case 14: appendGroup(endLoopControls_); break;
        case 15: appendGroup(defineBlockControls_); break;
        case 16: appendGroup(runBlockControls_); break;
        case 17:
            appendGroup(findImageControls_);
            if (popupFindFollowUp_.sel == 2) appendGroup(findImageVarControls_);
            else appendGroup(findImageOffsetControls_);
            break;
        case 18:
            appendGroup(ocrDepControls_);
            appendGroup(ocrFindRegionToggleControls_);
            appendGroup(ocrControls_);
            if (ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_)) appendGroup(ocrFindRegionControls_);
            appendGroup(ocrFollowControls_);
            if (popupOcrResultMode_.sel == 1) appendGroup(ocrSearchControls_);
            if (popupOcrFollowUp_.sel == 2) appendGroup(ocrFollowVarControls_);
            else appendGroup(ocrFollowOffsetControls_);
            appendHwnd(ocrResultModeLabel_);
            appendHwnd(ocrFollowUpLabel_);
            appendHwnd(ocrSearchLabel_);
            appendHwnd(ocrSearchVarLabel_);
            appendHwnd(ocrX1Label_);
            appendHwnd(ocrY1Label_);
            appendHwnd(ocrX2Label_);
            appendHwnd(ocrY2Label_);
            appendHwnd(ocrRegionLabel_);
            appendHwnd(ocrFindImageLabel_);
            break;
        case 19: appendGroup(ifControls_); break;
        case 20: appendGroup(elseControls_); break;
        case 21: appendGroup(lockScreenshotControls_); break;
        case 22: appendGroup(unlockScreenshotControls_); break;
        case 23: appendGroup(stopMacroControls_); break;
        case 24:
            appendGroup(runProgramControls_);
            if (popupRunProgram_.sel <= 0) appendGroup(runProgramFileControls_);
            break;
        case 25: appendGroup(closeProgramControls_); break;
        case 26: appendGroup(openWebpageControls_); break;
        case 27: appendGroup(openFileControls_); break;
        case 28: appendGroup(timerRecordControls_); break;
        case 32: appendGroup(getCursorPosControls_); break;
        case 33: appendGroup(gotoControls_); break;
        case 29: appendGroup(aiCommonControls_); appendGroup(aiTextControls_); break;
        case 30:
            appendGroup(aiCommonControls_);
            appendGroup(aiImageControls_);
            if (aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_)) appendGroup(aiFindRegionControls_);
            break;
        case 31:
            appendGroup(aiCommonControls_);
            appendGroup(aiActionControls_);
            if (aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_)) appendGroup(aiFindRegionControls_);
            break;
        default: break;
        }
        struct ComboEntry { HWND hwnd; int id; };
        static const ComboEntry kParamCombos[] = {
            {mousePressButton_, 2}, {clickButton_, 3}, {loopTypeCombo_, 4}, {runBlockCombo_, 5},
            {hotkeyShortcutCombo_, 6}, {runMacroCombo_, 8},
            {mousePlaybackCombo_, 9}, {scrollDirectionCombo_, 10}, {findFollowUpCombo_, 11},
            {ifVarCombo_, 12}, {ifOperatorCombo_, 13}, {ifConnectorCombo_, 14},
            {runProgramCombo_, 15}, {ocrResultModeCombo_, 16}, {ocrFollowUpCombo_, 17},
            {ocrSearchVarCombo_, 18}, {aiModelCombo_, 19}, {aiContextModeCombo_, 20},
            {aiOutputTypeCombo_, 21}, {aiSearchRegionCombo_, 22},
        };
        for (const auto& entry : kParamCombos) {
            if (!entry.hwnd || !IsParamComboVisible(entry.id)) continue;
            if (std::find(out.begin(), out.end(), entry.hwnd) == out.end()) out.push_back(entry.hwnd);
        }
        if (IsParamComboVisible(7)) {
            if (HWND varCombo = ActiveVarComboHwnd()) {
                if (std::find(out.begin(), out.end(), varCombo) == out.end()) out.push_back(varCombo);
            }
        }
    }

// was engine_host_window.h:3404-3506
void EngineHost::CaptureParamScrollBaseLayout(int layoutBottomHint ) {
        if (UsesDynamicParamPanel()) {
            RevealParamControlsForCapture();
            if (paramViewport_) UpdateWindow(paramViewport_);
        }

        std::unordered_map<HWND, ParamScrollLayoutEntry> prevLayout;
        for (const auto& entry : paramScrollLayout_) {
            if (entry.hwnd) prevLayout[entry.hwnd] = entry;
        }

        paramScrollLayout_.clear();
        paramControlsBottom_ = ParamPanelContentTopY();
        paramContentBottom_ = ParamPanelContentTopY();
        std::vector<HWND> controls;
        CollectParamScrollControls(controls);
        const bool dynamicPanel = UsesDynamicParamPanel();
        if (dynamicPanel && paramScrollY_ != 0) {
            for (const auto& [hwnd, entry] : prevLayout) {
                if (!hwnd) continue;
                SetParamPosAware(hwnd, entry.baseX, entry.baseY, entry.baseW, entry.baseH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            }
        }
        for (HWND h : controls) {
            if (!h) continue;
            if (IsIntentionallyHiddenParamControl(h)) continue;
            if (!dynamicPanel) {
                if (!IsControlShown(h) && !IsEditorParamComboHwnd(h)) continue;
            } else if (!IsEditorParamComboHwnd(h)) {
                const RECT probe = WindowClientRect(h);
                if (probe.top < -1000) continue;
            }
            if (IsFooterControl(h)) continue;
            EnsureParamViewportParent(h);
            if (!dynamicPanel) SyncParamControlScrollPosition(h);
            RECT rc = WindowClientRect(h);
            if (dynamicPanel && paramScrollY_ != 0) {
                OffsetRect(&rc, 0, paramScrollY_);
            }
            if (auto it = prevLayout.find(h); it != prevLayout.end()) {
                const auto& old = it->second;
                const int oldW = old.baseW;
                const int oldH = old.baseH;
                const int newW = rc.right - rc.left;
                const int newH = rc.bottom - rc.top;
                if (old.baseX != rc.left || old.baseY != rc.top || oldW != newW || oldH != newH) {
                    const RECT vp = ParamViewportRect();
                    RECT oldVp{
                        old.baseX - vp.left, old.baseY - vp.top,
                        old.baseX + oldW - vp.left, old.baseY + oldH - vp.top
                    };
                    if (paramViewport_) InvalidateRect(paramViewport_, &oldVp, FALSE);
                }
            }
            const int maxRight = ParamFieldMaxRight();
            if (rc.left < maxRight && rc.right > maxRight) {
                SetParamPosAware(h, rc.left, rc.top, maxRight - rc.left, rc.bottom - rc.top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                rc.right = maxRight;
            }
            paramScrollLayout_.push_back(ParamScrollLayoutEntry{
                h, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top
            });
            paramControlsBottom_ = std::max(paramControlsBottom_, static_cast<int>(rc.bottom));
            paramContentBottom_ = std::max(paramContentBottom_, static_cast<int>(rc.bottom));
        }

        int footerAnchor = paramControlsBottom_;
        if (layoutBottomHint >= 0) {
            footerAnchor = std::max(footerAnchor, layoutBottomHint);
        } else if (UsesDynamicParamPanel() && paramLayoutBottomHint_ >= 0) {
            footerAnchor = std::max(footerAnchor, paramLayoutBottomHint_);
        }
        ApplyEditorFooterLayout(footerAnchor);

        for (HWND h : {remarkLabel_, remark_, modifyBtn_, addBtn_}) {
            if (!h) continue;
            EnsureParamViewportParent(h);
            RECT rc = WindowClientRect(h);
            const int maxRight = ParamFieldMaxRight();
            if (rc.left < maxRight && rc.right > maxRight) {
                SetParamPosAware(h, rc.left, rc.top, maxRight - rc.left, rc.bottom - rc.top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                rc.right = maxRight;
            }
            paramScrollLayout_.push_back(ParamScrollLayoutEntry{
                h, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top
            });
            paramContentBottom_ = std::max(paramContentBottom_, static_cast<int>(rc.bottom));
        }

        UpdateParamViewportGeometry();

        if (footerAnchor >= ParamPanelContentTopY()) {
            const int gap = ScaleY(18);
            const int remarkH = ScaleY(22);
            const int btnGap = ScaleY(18);
            const int btnH = ScaleY(30);
            const int estimatedBottom = footerAnchor + gap + remarkH + btnGap + btnH;
            paramContentBottom_ = std::max(paramContentBottom_, estimatedBottom);
        }
    }

// was engine_host_window.h:3138-3219
void EngineHost::ShowEditorFor(int index, bool createNew) {
        CloseEditorPopup();
        ++editorOpenGeneration_;
        const int openGen = editorOpenGeneration_;
        RECT homeRc{};
        if (page_ == Page::Home && GetWindowRect(hwnd_, &homeRc)) {
            homeRectBeforeEditor_ = homeRc;
            hasHomeRectBeforeEditor_ = true;
        } else {
            GetWindowRect(hwnd_, &homeRc);
        }

        // cloak 内：改尺寸 + 首屏列表；参数表单延后，缩短揭开前耗时
        BeginSmoothPageTransition(hwnd_);
        page_ = Page::Editor;
        HideEditorComboHwnds();
        ShowEditorControls(false);
        RECT editorRc = EditorRectFromHome(homeRc);
        SetWindowPos(hwnd_, nullptr, editorRc.left, editorRc.top, UiEditorWidth(), UiEditorHeight(),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);

        selectedIndex_ = -1; hoverIndex_ = -1; scrollOffset_ = 0; editingRemarkIndex_ = -1;
        collapsedContainers_.clear();
        batchEditMode_ = false;
        batchSelected_.clear();
        actions_.clear();
        MarkVisibleActionsDirty();
        ShowWindow(listRemarkEdit_, SW_HIDE);

        editorOpenPending_ = true;
        editorOpenCreateNew_ = createNew;
        editorOpenPhase_ = 0;
        editorOpenPath_.clear();
        actionFormDrafts_.clear();

        if (createNew) {
            currentScriptIndex_ = -1;
            currentPath_.clear();
            currentRecordTime_ = NowText();
            SetText(name_, L"鼠标宏-" + TimestampName());
            loadedCoordMeta_ = StandardScriptCoordMeta();
            ResetEditorScriptChromeDefaults();
            SetPopupSel(popupMode_, mode_, 0);
            SetText(remark_, L"");
        } else if (index >= 0 && index < static_cast<int>(scripts_.size())) {
            currentScriptIndex_ = index;
            currentPath_ = scripts_[static_cast<size_t>(index)].path;
            editorOpenPath_ = currentPath_;
            LoadScriptFileProgressive(currentPath_);
        } else {
            editorOpenPending_ = false;
        }

        ApplyEditorFonts();
        ApplyEditorControlScale(false);
        UpdateBatchToolbar();
        StartHoverTimer();
        FiDbgInit(hwnd_);
        FiDbgSetMainWnd(hwnd_);

        // 首帧只出列表与页眉；参数区与添加/修改等 footer 等 FinishDeferred 布局后再显示
        ShowEditorControls(true, false);
        HideEditorComboHwnds();
        if (mode_) ShowWindow(mode_, SW_HIDE);
        if (actionCombo_) ShowWindow(actionCombo_, SW_HIDE);
        SetPopupSel(popupAction_, actionCombo_, 0);
        ResetEditorTransientFormState();
        for (HWND h : {remarkLabel_, remark_, addBtn_, modifyBtn_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
        if (paramViewport_) ShowWindow(paramViewport_, SW_HIDE);
        RefreshActionListLayer();
        UncloakEditorAfterReady();

        if (editorOpenPending_) {
            editorOpenPhase_ = 1;
            PostMessageW(hwnd_, WM_APP_EDITOR_FINISH_OPEN, static_cast<WPARAM>(openGen), 0);
        }
        if (editorParsePending_) {
            PostMessageW(hwnd_, WM_APP_EDITOR_PARSE_MORE, static_cast<WPARAM>(openGen), 0);
        }
    }

// was engine_host_window.h:3921-4008
void EngineHost::RefreshMoveParamPanel() {
        const int left = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int fieldH = ScaleY(22);
        const int hintH = ScaleY(25);
        const int gap1 = ScaleX(1);
        const int gap5 = ScaleX(5);
        const int gap7 = ScaleY(7);
        const int gap8 = ScaleY(8);
        const int gap9 = ScaleY(9);
        const int gap10 = ScaleY(10);
        const int gap15 = ScaleY(15);
        const int btnH = ScaleY(32);

        int y = ScaleY(180);

        if (moveHintLabel_) {
            MoveParamAware(moveHintLabel_, left, y,
                std::min(ScaleX(190), maxRight - left), hintH, FALSE);
            ShowWindow(moveHintLabel_, SW_SHOW);
        }
        y += hintH + gap9;

        auto layoutCoordRandomRow = [&](HWND xLabel, HWND xEdit, HWND rndLabel, HWND rndEdit) {
            int x = left;
            auto mv = [&](HWND h, int wDesign) {
                if (!h) return;
                const int w = ScaleX(wDesign);
                MoveParamAware(h, x, y, w, fieldH, FALSE);
                ShowWindow(h, SW_SHOW);
                x += w;
            };
            mv(xLabel, 25);
            x += gap1;
            mv(xEdit, 87);
            x += gap1;
            mv(rndLabel, 50);
            x += gap5;
            mv(rndEdit, 25);
        };

        layoutCoordRandomRow(moveXLabel_, moveX_, moveRandomXLabel_, moveRandomX_);
        y += fieldH + gap15;

        layoutCoordRandomRow(moveYLabel_, moveY_, moveRandomYLabel_, moveRandomY_);
        y += fieldH + gap10;

        if (crosshairBtn_) {
            MoveParamAware(crosshairBtn_, left, y, ScaleX(186), btnH, FALSE);
            ShowWindow(crosshairBtn_, SW_SHOW);
        }
        y += btnH + gap7;

        if (moveFromVar_) {
            MoveParamAware(moveFromVar_, left, y, ScaleX(180), hintH, FALSE);
            ShowWindow(moveFromVar_, SW_SHOW);
        }
        y += hintH + gap7;

        auto layoutVarRow = [&](HWND xLabel, HWND xEdit) {
            int x = left;
            auto mv = [&](HWND h, int wDesign) {
                if (!h) return;
                const int w = ScaleX(wDesign);
                MoveParamAware(h, x, y, w, fieldH, FALSE);
                ShowWindow(h, SW_SHOW);
                x += w;
            };
            mv(xLabel, 25);
            x += gap1;
            mv(xEdit, 168);
        };

        layoutVarRow(moveVarXLabel_, moveVarX_);
        y += fieldH + gap15;

        layoutVarRow(moveVarYLabel_, moveVarY_);
        y += fieldH + gap8;

        if (moveHintFooter_) {
            const int hintFooterH = ScaleY(56);
            MoveParamAware(moveHintFooter_, left, y, std::max(1, maxRight - left), hintFooterH, FALSE);
            ShowWindow(moveHintFooter_, SW_SHOW);
            y += hintFooterH;
        }

        SyncParamScrollLayout(y);
    }

