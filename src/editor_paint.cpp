// ── 编辑器绘制函数 ────────────────────────────────────────
#include "main_window.h"
#include "drawing.h"
#include "scheduled_task_ui.h"
#include "modern_edit.h"

// --------------------------------------------------
    void MainWindow::PaintActionListLocal(HDC hdc, int width, int height) {
        RECT outer{0, 0, width, height};
        FillRectColor(hdc, outer, kWhite);
        const int pad = UiLen(kListInnerPad);
        const int rowH = std::max(1, UiLen(kRowH));
        const int contentTop = pad + 1;
        const int contentRight = width - pad - (MaxEditorScroll() > 0 ? UiLen(kEditorScrollW) + UiLen(6) : 0);
        const int contentBottom = height - pad;
        const int contentHeight = std::max(0, contentBottom - contentTop);
        const int visible = contentHeight / rowH;
        const auto vis = VisibleActionIndices();
        const int end = std::min(static_cast<int>(vis.size()), scrollOffset_ + visible + 1);
        for (int vi = scrollOffset_; vi < end; ++vi) {
            EnsureEditorActionsParsed(vis[static_cast<size_t>(vi)], vis[static_cast<size_t>(vi)] + 1);
        }
        for (int vi = scrollOffset_; vi < end; ++vi) {
            const int i = vis[static_cast<size_t>(vi)];
            const int y = contentTop + (vi - scrollOffset_) * rowH;
            if (y + rowH > contentBottom) break;
            RECT r{pad, y, contentRight, y + rowH};
            const bool batchChecked = batchEditMode_ && i < static_cast<int>(batchSelected_.size()) && batchSelected_[static_cast<size_t>(i)];
            COLORREF bg = batchEditMode_
                ? (batchChecked ? kBatchSelectedRow : (i == hoverIndex_ ? kHoverGray : kWhite))
                : (i == selectedIndex_ ? kMainGreen : (i == hoverIndex_ ? kHoverGray : kWhite));
            FillRectColor(hdc, r, bg);
            const auto& a = actions_[static_cast<size_t>(i)];
            COLORREF fg = (!batchEditMode_ && i == selectedIndex_) ? kWhite : kText;
            const int textLeft = ActionTextLeftLocal(a.indent, a.type, batchEditMode_, pad);
            DrawTextIn(hdc, std::to_wstring(a.originalNo > 0 ? a.originalNo : i + 1), RECT{pad + kColNoInList, r.top, pad + kColActionInList - 4, r.bottom}, (!batchEditMode_ && i == selectedIndex_) ? kWhite : kIndexGreen);
            if (batchEditMode_) DrawCheckbox(hdc, LocalCheckboxRect(i, y), batchChecked);
            if (IsExpandableContainer(a.type)) {
                const int expandLeft = ExpandToggleLeftLocal(a.indent, batchEditMode_, pad);
                RECT expandRc{expandLeft, r.top + 8, expandLeft + kExpandToggleWidth, r.bottom - 8};
                DrawExpandTriangle(hdc, expandRc, IsContainerExpanded(i), fg);
            }
            DrawTextIn(hdc, ActionName(a), RECT{textLeft, r.top, pad + kColRemarkInList - 4, r.bottom}, fg);
            if (i != editingRemarkIndex_) {
                if (a.remark.empty()) {
                    if (!batchEditMode_ && i == selectedIndex_) {
                        DrawTextIn(hdc, L"点击添加备注", RECT{pad + kColRemarkInList, r.top, pad + kColOpInList - 4, r.bottom}, kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                } else {
                    DrawTextIn(hdc, a.remark, RECT{pad + kColRemarkInList, r.top, pad + kColOpInList - 4, r.bottom}, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }
            if (!batchEditMode_) {
                if (i == selectedIndex_) {
                    DrawTextIn(hdc, L"拖动改变位置", RECT{pad + kColOpInList, r.top, r.right - 4, r.bottom}, RGB(246, 241, 79), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                } else if (i == hoverIndex_) {
                    DrawTextIn(hdc, L"复制", LocalCopyRect(i, y, contentRight), kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    DrawTextIn(hdc, L"删除", LocalDeleteRect(i, y, contentRight), kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
        }
        PaintEditorScrollbarLocal(hdc, width, height);
        DrawBorderRect(hdc, outer, kLineGreen);
    }

// --------------------------------------------------
    void MainWindow::PaintEditorScrollbarLocal(HDC hdc, int width, int height) {
        if (MaxEditorScroll() <= 0) return;
        const int pad = UiLen(kListInnerPad);
        const int scrollW = UiLen(kEditorScrollW);
        RECT track{width - scrollW - UiLen(4), pad + UiLen(2), width - pad - UiLen(4), height - pad - UiLen(2)};
        FillRectColor(hdc, track, kScrollTrackGray);
        RECT thumb = EditorScrollThumbRect();
        const int trackH = track.bottom - track.top;
        const int thumbH = thumb.bottom - thumb.top;
        const int maxScroll = MaxEditorScroll();
        const int range = std::max(1, trackH - thumbH);
        const int top = track.top + range * scrollOffset_ / std::max(1, maxScroll);
        RECT localThumb{track.left, top, track.right, top + thumbH};
        FillRectColor(hdc, localThumb, kScrollThumbGray);
    }

// --------------------------------------------------
    void MainWindow::DrawNavTab(HDC hdc, RECT rc, quickscript::MainTab tab, const std::wstring& text, int iconType) {
        if (activeHomeTab_ == tab) {
            FillRectColor(hdc, rc, kTabActiveGreen);
        }
        DrawNavIcon(hdc, rc, iconType, homeTabFont_);
        SelectObject(hdc, homeTabFont_);
        DrawTextIn(hdc, text, RECT{rc.left + UiLen(52), rc.top, rc.right - UiLen(6), rc.bottom},
            kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

// --------------------------------------------------
    void MainWindow::DrawRadio(HDC hdc, RECT rc, bool checked) {
        DrawHomeRadio(hdc, rc, checked);
    }

// --------------------------------------------------
    void MainWindow::DrawClickerPopupMenuItem(HDC hdc, const RECT& row, const wchar_t* title,
        const wchar_t* desc, bool checked, bool hovered) {
        const int rowH = row.bottom - row.top;
        COLORREF bg = checked ? kBatchSelectedRow
            : (hovered ? kComboMenuHoverBlue : kWhite);
        FillRectColor(hdc, row, bg);

        static constexpr int kPopupCheckboxSize = 22;
        const int boxTop = row.top + (rowH - kPopupCheckboxSize) / 2;
        RECT box{row.left + 11, boxTop, row.left + 11 + kPopupCheckboxSize, boxTop + kPopupCheckboxSize};
        StDrawCheckbox(hdc, box, checked);

        SelectObject(hdc, homeFont_);
        const int textLeft = box.right + 10;
        const int titleH = 24;
        const int descH = 22;
        const int textGap = 8;
        const int blockH = titleH + textGap + descH;
        const int blockTop = row.top + (rowH - blockH) / 2;
        const COLORREF titleColor = checked ? RGB(30, 30, 30) : (hovered ? kText : RGB(30, 30, 30));
        DrawTextIn(hdc, title, RECT{textLeft, blockTop, row.right - 10, blockTop + titleH},
            titleColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, desc, RECT{textLeft, blockTop + titleH + textGap, row.right - 10, blockTop + blockH},
            RGB(145, 150, 155), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    void MainWindow::PaintClickerDropPopupContent(HDC hdc, HWND popupHwnd) {
        if (clickerDropPopupKind_ < 0) return;
        RECT client{};
        GetClientRect(popupHwnd, &client);
        FillRectColor(hdc, client, kWhite);
        DrawBorderRect(hdc, client, kComboPopupBorderGray);

        const int itemCount = ClickerPopupItemCount();
        const int visible = ClickerPopupVisibleCount();
        const int scrollMax = std::max(0, itemCount - visible);
        clickerPopupScroll_ = std::clamp(clickerPopupScroll_, 0, scrollMax);
        const int contentRight = client.right - 1 - (scrollMax > 0 ? 12 : 0);

        if (clickerDropPopupKind_ == 0) {
            const quickscript::ClickIntervalMode modes[3] = {
                quickscript::ClickIntervalMode::Custom,
                quickscript::ClickIntervalMode::Efficient,
                quickscript::ClickIntervalMode::Extreme
            };
            const wchar_t* descs[3] = {
                L"手动输入间隔时间",
                L"间隔0.1秒(每秒10次)",
                L"间隔0.01秒(每秒100次)"
            };
            for (int vis = 0; vis < visible; ++vis) {
                const int i = vis + clickerPopupScroll_;
                if (i >= itemCount) break;
                RECT row{client.left + 1, client.top + 1 + vis * kClickerDropdownItemH,
                    contentRight, client.top + 1 + (vis + 1) * kClickerDropdownItemH};
                const bool checked = clickerSettings_.intervalMode == modes[i];
                const bool hovered = !checked && clickerPopupHover_ == i;
                DrawClickerPopupMenuItem(hdc, row, ClickIntervalTitle(modes[i]).c_str(), descs[i], checked, hovered);
            }
        } else {
            static const HotkeyMenuItem kItems[] = {
                {kHotCustom, L"自定义", L"将您指定的按键设为启停热键"},
                {kHotLeft, L"鼠标左键", L"长按左键开始连点，松开停止（约0.2秒）"},
                {kHotMiddle, L"鼠标中键", L"将点击中键设为启停热键"},
                {kHotRight, L"鼠标右键", L"将点击右键设为启停热键"},
                {kHotX1, L"鼠标侧键1", L"一般为鼠标左侧后部的键"},
                {kHotX2, L"鼠标侧键2", L"一般为鼠标左侧前部的键"},
                {kHotSpace, L"空格键", L"将空格键设为启停热键"},
            };
            for (int vis = 0; vis < visible; ++vis) {
                const int i = vis + clickerPopupScroll_;
                if (i >= itemCount) break;
                RECT row{client.left + 1, client.top + 1 + vis * kClickerDropdownItemH,
                    contentRight, client.top + 1 + (vis + 1) * kClickerDropdownItemH};
                const bool checked = IsHotkeyMenuChecked(kItems[i].id);
                const bool hovered = !checked && clickerPopupHover_ == i;
                DrawClickerPopupMenuItem(hdc, row, kItems[i].title, kItems[i].desc, checked, hovered);
            }
        }

        if (scrollMax > 0) {
            RECT track{client.right - 10, client.top + 1, client.right - 1, client.bottom - 1};
            FillRectColor(hdc, track, kComboScrollTrackGray);
            const int trackH = track.bottom - track.top;
            const int thumbH = std::max(18, trackH * visible / itemCount);
            const int thumbTop = track.top + (trackH - thumbH) * clickerPopupScroll_ / scrollMax;
            RECT thumb{track.left, thumbTop, track.right, thumbTop + thumbH};
            FillRectColor(hdc, thumb, kComboScrollThumbGray);
        }
    }

// --------------------------------------------------
    void MainWindow::DrawEditorCombo(HDC hdc, HWND label, RECT rc, bool dropped) {
        const COLORREF borderColor = dropped ? kMainGreen : kComboBorderGray;
        FillRectColor(hdc, rc, kWhite);
        const int arrowW = 26;
        const std::wstring comboText = EditorComboDisplayText(label);
        DrawTextIn(hdc, comboText.empty() ? L" " : comboText, RECT{rc.left + 10, rc.top, rc.right - arrowW, rc.bottom}, kText);
        const int arrowCenterX = rc.right - arrowW / 2;
        const int arrowCenterY = rc.top + (rc.bottom - rc.top) / 2;
        DrawComboDownArrow(hdc, arrowCenterX, arrowCenterY, kMainGreen);
        DrawBorderRect(hdc, rc, borderColor);
    }

// --------------------------------------------------
    void MainWindow::PaintEditorDropPopupContent(HDC hdc, HWND popupHwnd) {
        PopupCombo* pc = GetEditorPopup();
        if (!pc || pc->items.empty()) return;
        RECT client{};
        GetClientRect(popupHwnd, &client);
        FillRectColor(hdc, client, kWhite);
        DrawBorderRect(hdc, client, kComboPopupBorderGray);
        ResolveRenderContext(hdc).DrawLine(client.right - 1, client.top, client.right - 1, client.bottom - 1,
            kComboPopupBorderGray, 1.0f);

        const int itemH = kEditorPopupItemH;
        const int total = static_cast<int>(pc->items.size());
        const int visible = EditorPopupVisibleCount();
        const int scrollMax = std::max(0, total - visible);
        editorPopupScroll_ = std::clamp(editorPopupScroll_, 0, scrollMax);
        SelectObject(hdc, editorFont_);
        const int contentRight = client.right - 1 - (scrollMax > 0 ? 12 : 0);
        for (int vis = 0; vis < visible; ++vis) {
            const int i = vis + editorPopupScroll_;
            if (i >= total) break;
            RECT row{client.left + 1, client.top + 1 + vis * itemH, contentRight, client.top + 1 + (vis + 1) * itemH};
            const bool selected = pc->sel == i;
            const bool hovered = !selected && editorPopupHover_ == i;
            COLORREF rowBg = selected ? kComboMenuSelectBlue : (hovered ? kComboMenuHoverBlue : kWhite);
            FillRectColor(hdc, row, rowBg);
            DrawTextIn(hdc, pc->items[static_cast<size_t>(i)], RECT{row.left + 10, row.top, row.right - 6, row.bottom},
                selected ? kComboMenuSelectText : kText);
        }
        if (scrollMax > 0) {
            RECT track{client.right - 10, client.top + 1, client.right - 1, client.bottom - 1};
            FillRectColor(hdc, track, kComboScrollTrackGray);
            const int trackH = track.bottom - track.top;
            const int thumbH = std::max(18, trackH * visible / total);
            const int thumbTop = track.top + (trackH - thumbH) * editorPopupScroll_ / scrollMax;
            RECT thumb{track.left, thumbTop, track.right, thumbTop + thumbH};
            FillRectColor(hdc, thumb, kComboScrollThumbGray);
        }
    }

    namespace {
    const wchar_t kQuickInputTextExampleTip[] =
        L"比如：找图保存的变量matchRet1的值，可以写为：{matchRet1.matchData} 或 {matchRet1.matchData*10-1}";
    }

    SIZE MainWindow::MeasureQuickInputTipSize() const {
        constexpr int kPad = 8;
        if (quickInputTipShown_ == QuickInputTipKind::TextExample) {
            const int maxW = 320;
            RECT rc{0, 0, maxW - kPad * 2, 0};
            HDC hdc = GetDC(hwnd_);
            HGDIOBJ oldFont = SelectObject(hdc, editorFont_);
            DrawTextW(hdc, kQuickInputTextExampleTip, -1, &rc, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
            SelectObject(hdc, oldFont);
            ReleaseDC(hwnd_, hdc);
            return SIZE{maxW, rc.bottom - rc.top + kPad * 2 + 2};
        }
        if (quickInputTipShown_ == QuickInputTipKind::VariableHelp) {
            const int idx = editorPopupHover_;
            if (idx < 0 || idx >= static_cast<int>(quickInputVarItems_.size())) return SIZE{220, 56};
            const auto& item = quickInputVarItems_[static_cast<size_t>(idx)];
            const int w = 240;
            const int headerH = 24;
            RECT bodyRc{0, 0, w - kPad * 2, 0};
            HDC hdc = GetDC(hwnd_);
            HGDIOBJ oldFont = SelectObject(hdc, editorFont_);
            DrawTextW(hdc, item.tooltip.c_str(), static_cast<int>(item.tooltip.size()), &bodyRc, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
            SelectObject(hdc, oldFont);
            ReleaseDC(hwnd_, hdc);
            const int bodyH = std::max(24, static_cast<int>(bodyRc.bottom - bodyRc.top));
            return SIZE{w, headerH + bodyH + kPad + 2};
        }
        return SIZE{0, 0};
    }

    void MainWindow::PaintEditorTipPopupContent(HDC hdc, HWND popupHwnd) {
        RECT client{};
        GetClientRect(popupHwnd, &client);
        FillRectColor(hdc, client, kWhite);
        DrawBorderRect(hdc, client, kComboPopupBorderGray);
        SelectObject(hdc, editorFont_);
        if (quickInputTipShown_ == QuickInputTipKind::TextExample) {
            DrawTextIn(hdc, kQuickInputTextExampleTip,
                RECT{client.left + 8, client.top + 8, client.right - 8, client.bottom - 8}, kText,
                DT_LEFT | DT_WORDBREAK);
            return;
        }
        if (quickInputTipShown_ == QuickInputTipKind::VariableHelp) {
            const int idx = editorPopupHover_;
            if (idx < 0 || idx >= static_cast<int>(quickInputVarItems_.size())) return;
            const auto& item = quickInputVarItems_[static_cast<size_t>(idx)];
            RECT header{client.left + 1, client.top + 1, client.right - 1, client.top + 25};
            FillRectColor(hdc, header, kComboMenuHoverBlue);
            DrawTextIn(hdc, item.display, RECT{header.left + 8, header.top, header.right - 4, header.bottom}, kText);
            DrawTextIn(hdc, item.tooltip,
                RECT{client.left + 8, client.top + 28, client.right - 8, client.bottom - 4}, kText,
                DT_LEFT | DT_WORDBREAK);
        }
    }

// --------------------------------------------------
    void MainWindow::PaintEditorListHeaderChrome(HDC hdc) {
        // 动作列表上边框横线（右缘 776 为 1200×1080 下列表宽度，不延伸到右侧参数面板）
        constexpr int kListHeaderLineRight = 776;
        const int listLeft = UiLen(kListX);
        const int listRight = UiLen(kListHeaderLineRight);
        const int headerLineY = UiLen(kListY);

        ResolveRenderContext(hdc).DrawLine(listLeft, headerLineY, listRight, headerLineY, kLineGreen, 1.0f);
    }

    void MainWindow::PaintEditorParamChrome(HDC hdc, HWND hdcWindow) {
        if (!hdcWindow) hdcWindow = hwnd_;
        const bool vpDc = (hdcWindow == paramViewport_);
        auto controlOnThisDc = [&](HWND h) {
            if (!h) return false;
            return IsParamViewportChild(h) ? vpDc : !vpDc;
        };
        auto mapRect = [&](const RECT& rc) { return MapRectFromMain(hdcWindow, rc); };

        RECT contentVp = mapRect(ParamScrollContentRect());
        HRGN paramClip = CreateRectRgnIndirect(&contentVp);
        HRGN oldClip = CreateRectRgn(0, 0, 0, 0);
        const int hadClip = GetClipRgn(hdc, oldClip);
        SelectClipRgn(hdc, paramClip);

        SelectObject(hdc, editorFont_);
        auto drawComboIfActive = [&](HWND h, int popupId) {
            if (!controlOnThisDc(h) || !IsParamComboVisible(popupId)) return;
            const RECT clientRc = EditorComboClientRect(h);
            if (clientRc.right <= clientRc.left || clientRc.bottom <= clientRc.top) return;
            if (!ParamRectIntersectsContent(clientRc)) return;
            RECT rc = mapRect(clientRc);
            if (rc.right <= rc.left || rc.bottom <= rc.top) return;
            DrawEditorCombo(hdc, h, rc, editorPopupOpen_ == popupId);
        };
        drawComboIfActive(mousePressButton_, 2);
        drawComboIfActive(clickButton_, 3);
        drawComboIfActive(loopTypeCombo_, 4);
        drawComboIfActive(runBlockCombo_, 5);
        drawComboIfActive(hotkeyShortcutCombo_, 6);
        if (HWND varCombo = ActiveVarComboHwnd()) drawComboIfActive(varCombo, 7);
        drawComboIfActive(runMacroCombo_, 8);
        drawComboIfActive(mousePlaybackCombo_, 9);
        drawComboIfActive(scrollDirectionCombo_, 10);
        drawComboIfActive(findFollowUpCombo_, 11);
        drawComboIfActive(ifVarCombo_, 12);
        drawComboIfActive(ifOperatorCombo_, 13);
        drawComboIfActive(ifConnectorCombo_, 14);
        drawComboIfActive(runProgramCombo_, 15);
        drawComboIfActive(ocrResultModeCombo_, 16);
        drawComboIfActive(ocrFollowUpCombo_, 17);
        drawComboIfActive(ocrSearchVarCombo_, 18);
        drawComboIfActive(aiModelCombo_, 19);
        drawComboIfActive(aiContextModeCombo_, 20);
        drawComboIfActive(aiOutputTypeCombo_, 21);

        auto drawEditBorder = [&](HWND h) {
            if (!controlOnThisDc(h) || !IsWindowVisible(h)) return;
            if (h == listRemarkEdit_) return;
            RECT rc = WindowClientRect(h);
            if (rc.right <= rc.left || rc.bottom <= rc.top) return;
            RECT outer = rc;
            InflateRect(&outer, 1, 1);
            if (!ParamRectIntersectsContent(outer)) return;
            DrawEditorFieldBorder(hdc, h, hdcWindow);
        };
        const int sel = popupAction_.sel;
        for (const auto& [idx, result] : paramLayoutResults_) {
            if (idx != sel && !IsSubPanelIdxVisible(sel, idx)) continue;
            for (const auto& p : result.placements) {
                if (!p.hwnd || !IsWindowVisible(p.hwnd)) continue;
                if (p.type == UIComponentType::Edit
                    || p.type == UIComponentType::FieldEdit
                    || p.type == UIComponentType::MultilineEdit
                    || p.type == UIComponentType::CaptureField) {
                    drawEditBorder(p.hwnd);
                }
            }
        }
        if (remark_ && IsWindowVisible(remark_)) drawEditBorder(remark_);
        // 灰按钮边框由 DrawGrayButton 自绘；勿在父 DC 再描一层，滚动后易在外侧留下残影。

        SelectClipRgn(hdc, hadClip == 1 ? oldClip : nullptr);
        DeleteObject(paramClip);
        DeleteObject(oldClip);
    }

    void MainWindow::PaintEditor(HDC hdc) {
        PaintParamScrollScrollbar(hdc);

        SelectObject(hdc, editorFont_);
        const RECT modeRc = WindowClientRect(mode_);
        const RECT actionRc = WindowClientRect(actionCombo_);
        const RECT headerTextRc = EditorMacroHeaderTextRect();
        if (IsEditorWindowModeActive()) {
            if (wmModeLabelRect_.right > wmModeLabelRect_.left) {
                DrawTextIn(hdc, L"模式:", wmModeLabelRect_, kText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        } else {
            DrawTextIn(hdc, L"模式:", RECT{modeRc.left - ScaleEditorX(50), headerTextRc.top, modeRc.left - ScaleEditorX(6), headerTextRc.bottom}, kText,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        DrawEditorCombo(hdc, mode_, modeRc, editorPopupOpen_ == 0);
        if (IsEditorWindowModeActive() && wmSelectMethod_) {
            const RECT wmRc = WindowClientRect(wmSelectMethod_);
            if (wmSelectMethodLabelRect_.right > wmSelectMethodLabelRect_.left) {
                DrawTextIn(hdc, L"选择窗口方式:", wmSelectMethodLabelRect_, kText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
            DrawEditorCombo(hdc, wmSelectMethod_, wmRc, editorPopupOpen_ == 23);
        }
        if (IsEditorWindowModeActive() && wmTargetPathEdit_ && IsWindowVisible(wmTargetPathEdit_)) {
            const RECT pathRc = WindowClientRect(wmTargetPathEdit_);
            const RECT labelRc = EditorRowLabelTextRect(pathRc.top, pathRc.bottom - pathRc.top);
            DrawTextIn(hdc, L"目标程序:", labelRc, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawEditOuterBorder(hdc, hwnd_, wmTargetPathEdit_, kComboBorderGray);
        }
        DrawTextIn(hdc, L"请选择要添加的宏", RECT{actionRc.left, actionRc.top - 28, actionRc.right, actionRc.top - 4}, kText);
        DrawEditorCombo(hdc, actionCombo_, actionRc, editorPopupOpen_ == 1);
        if (name_ && IsWindowVisible(name_)) {
            DrawEditOuterBorder(hdc, hwnd_, name_, kComboBorderGray);
        }
        if (!IsEditorWindowModeActive() && breakoutTimeEdit_ && IsWindowVisible(breakoutTimeEdit_)) {
            const RECT editRc = WindowClientRect(breakoutTimeEdit_);
            const RECT labelRc = EditorRowLabelTextRect(editRc.top, editRc.bottom - editRc.top);
            DrawTextIn(hdc, L"脱离时间:", labelRc, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawEditOuterBorder(hdc, hwnd_, breakoutTimeEdit_, kComboBorderGray);
        }
        PaintActionList(hdc);
        PaintEditorListHeaderChrome(hdc);
        PaintDragMarker(hdc);
    }

// --------------------------------------------------
    void MainWindow::PaintParamScrollScrollbar(HDC hdc) {
        if (MaxParamScroll() <= 0) return;
        RECT track = ParamScrollTrackRect();
        if (track.bottom <= track.top) return;
        FillRectColor(hdc, track, kScrollTrackGray);
        RECT thumb = ParamScrollThumbRect();
        FillRectColor(hdc, thumb, kScrollThumbGray);
    }

// --------------------------------------------------
    void MainWindow::DrawEditorFieldBorder(HDC hdc, HWND ctrl, HWND hdcWindow) {
        if (!ctrl || !IsWindowVisible(ctrl)) return;
        if (!hdcWindow) hdcWindow = hwnd_;
        RECT rc = MapRectFromMain(hdcWindow, WindowClientRect(ctrl));
        InflateRect(&rc, 1, 1);
        if (ctrl == name_ || ctrl == breakoutTimeEdit_) {
            DrawBorderRect(hdc, rc, kComboBorderGray);
            return;
        }
        // 仅画边框，不填充内部——父 DC 填充会盖住 EDIT 子控件文字
        RECT content = MapRectFromMain(hdcWindow, ParamScrollContentRect());
        RECT visible{};
        if (!IntersectRect(&visible, &rc, &content)) return;
        const int saved = SaveDC(hdc);
        IntersectClipRect(hdc, content.left, content.top, content.right, content.bottom);
        DrawBorderRect(hdc, visible, kComboBorderGray);
        RestoreDC(hdc, saved);
    }

// --------------------------------------------------
    void MainWindow::PaintActionList(HDC hdc) {
        RECT list = ActionListRect();
        const int lw = list.right - list.left;
        const int lh = list.bottom - list.top;
        if (lw <= 0 || lh <= 0) return;
        HDC memDc = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, lw, lh);
        HGDIOBJ oldBmp = SelectObject(memDc, memBmp);
        HGDIOBJ oldFont = SelectObject(memDc, editorFont_);
        RenderBatchScope batch(memDc);
        PaintActionListLocal(memDc, lw, lh);
        SelectObject(memDc, oldFont);
        batch.End();
        if (IsWindowVisible(listRemarkEdit_)) {
            RECT erc = WindowClientRect(listRemarkEdit_);
            ExcludeClipRect(hdc, erc.left, erc.top, erc.right, erc.bottom);
        }
        BitBlt(hdc, list.left, list.top, lw, lh, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDc);
    }

// --------------------------------------------------
    void MainWindow::PaintDragMarker(HDC hdc) {
        if (!dragging_ || !dragMoved_ || dragTargetIndex_ < 0) return;
        const auto vis = VisibleActionIndices();
        int visibleSlot = static_cast<int>(vis.size());
        for (int i = 0; i < static_cast<int>(vis.size()); ++i) {
            if (vis[static_cast<size_t>(i)] >= dragTargetIndex_) { visibleSlot = i; break; }
        }
        if (visibleSlot < scrollOffset_ || visibleSlot > scrollOffset_ + VisibleActionRows() + 1) return;
        RECT list = ActionListRect();
        const int y = std::clamp(
            static_cast<int>(list.top + (visibleSlot - scrollOffset_) * kRowH),
            static_cast<int>(list.top),
            static_cast<int>(list.bottom));
        const int lineLeft = dragTargetNested_
            ? (list.left + kListInnerPad + kColActionInList + dragTargetIndent_ * kIndentStep)
            : (list.left + 4);
        IRenderContext& ctx = ResolveRenderContext(hdc);
        ctx.DrawLine(lineLeft, y, list.right - 4, y, kOrange, 3.0f);
        POINT marker[3] = {{lineLeft, y - 5}, {lineLeft, y + 5}, {lineLeft + 10, y}};
        ctx.DrawPolygon(marker, 3, kOrange, true);
    }

// --------------------------------------------------
    void MainWindow::DrawTextIn(HDC hdc, const std::wstring& text, RECT rc, COLORREF color, UINT format) {
        ::DrawTextIn(hdc, text, rc, color, format);
    }
