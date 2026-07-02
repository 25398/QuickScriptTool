// ── 编辑器绘制函数 ────────────────────────────────────────
#include "main_window.h"
#include "drawing.h"

// --------------------------------------------------
    void MainWindow::PaintActionListLocal(HDC hdc, int width, int height) {
        RECT outer{0, 0, width, height};
        HBRUSH white = CreateSolidBrush(kWhite);
        FillRect(hdc, &outer, white);
        DeleteObject(white);
        const int pad = kListInnerPad;
        const int contentTop = pad + 1;
        const int contentRight = width - pad - (MaxEditorScroll() > 0 ? kEditorScrollW + 6 : 0);
        const int contentBottom = height - pad;
        const int contentHeight = std::max(0, contentBottom - contentTop);
        const int visible = contentHeight / kRowH;
        const auto vis = VisibleActionIndices();
        const int end = std::min(static_cast<int>(vis.size()), scrollOffset_ + visible + 1);
        for (int vi = scrollOffset_; vi < end; ++vi) {
            const int i = vis[static_cast<size_t>(vi)];
            const int y = contentTop + (vi - scrollOffset_) * kRowH;
            if (y + kRowH > contentBottom) break;
            RECT r{pad, y, contentRight, y + kRowH};
            const bool batchChecked = batchEditMode_ && i < static_cast<int>(batchSelected_.size()) && batchSelected_[static_cast<size_t>(i)];
            COLORREF bg = batchEditMode_
                ? (batchChecked ? kBatchSelectedRow : (i == hoverIndex_ ? kHoverGray : kWhite))
                : (i == selectedIndex_ ? kMainGreen : (i == hoverIndex_ ? kHoverGray : kWhite));
            HBRUSH b = CreateSolidBrush(bg);
            FillRect(hdc, &r, b);
            DeleteObject(b);
            const auto& a = actions_[static_cast<size_t>(i)];
            COLORREF fg = (!batchEditMode_ && i == selectedIndex_) ? kWhite : kText;
            const int textLeft = ActionTextLeftLocal(a.indent, a.type, batchEditMode_, pad);
            DrawTextIn(hdc, std::to_wstring(a.originalNo > 0 ? a.originalNo : i + 1), RECT{pad + kColNoInList, r.top, pad + kColActionInList - 4, r.bottom}, (!batchEditMode_ && i == selectedIndex_) ? kWhite : kIndexGreen);
            if (batchEditMode_) DrawListCheckbox(hdc, LocalCheckboxRect(i, y), batchChecked);
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
        FrameRect(hdc, &outer, lineGreenBrush_);
    }

// --------------------------------------------------
    void MainWindow::PaintEditorScrollbarLocal(HDC hdc, int width, int height) {
        if (MaxEditorScroll() <= 0) return;
        const int pad = kListInnerPad;
        RECT track{width - kEditorScrollW - 4, pad + 2, width - pad - 4, height - pad - 2};
        HBRUSH trackBrush = CreateSolidBrush(kScrollTrackGray);
        FillRect(hdc, &track, trackBrush);
        DeleteObject(trackBrush);
        RECT thumb = EditorScrollThumbRect();
        const int trackH = track.bottom - track.top;
        const int thumbH = thumb.bottom - thumb.top;
        const int maxScroll = MaxEditorScroll();
        const int range = std::max(1, trackH - thumbH);
        const int top = track.top + range * scrollOffset_ / std::max(1, maxScroll);
        RECT localThumb{track.left, top, track.right, top + thumbH};
        HBRUSH thumbBrush = CreateSolidBrush(kScrollThumbGray);
        FillRect(hdc, &localThumb, thumbBrush);
        DeleteObject(thumbBrush);
    }

// --------------------------------------------------
    void MainWindow::DrawNavTab(HDC hdc, RECT rc, quickscript::MainTab tab, const std::wstring& text, int iconType) {
        const bool active = activeHomeTab_ == tab;
        if (active) {
            FillGradientRect(hdc, rc, RGB(59, 157, 92), RGB(44, 128, 75), true);
        } else {
            HBRUSH bg = CreateSolidBrush(kMainGreen);
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
        }
        DrawNavIcon(hdc, rc, iconType);
        SelectObject(hdc, homeTabFont_);
        DrawTextIn(hdc, text, RECT{rc.left + 52, rc.top, rc.right - 6, rc.bottom}, kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

// --------------------------------------------------
    void MainWindow::DrawRadio(HDC hdc, RECT rc, bool checked) {
        HPEN pen = CreatePen(PS_SOLID, 2, kWhite);
        HBRUSH brush = CreateSolidBrush(kMainGreen);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        Ellipse(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
        if (checked) {
            HBRUSH dot = CreateSolidBrush(kWhite);
            RECT inner{rc.left + 6, rc.top + 6, rc.right - 6, rc.bottom - 6};
            oldBrush = SelectObject(hdc, dot);
            oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
            Ellipse(hdc, inner.left, inner.top, inner.right, inner.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(dot);
        }
    }

// --------------------------------------------------
    void MainWindow::PaintClickerIntervalPopup(HDC hdc) {
        if (!clickerIntervalOpen_) return;
        RECT popup = ClickerIntervalPopupRect();
        HBRUSH white = CreateSolidBrush(kWhite);
        FillRect(hdc, &popup, white);
        DeleteObject(white);
        HPEN border = CreatePen(PS_SOLID, 1, kComboPopupBorderGray);
        HGDIOBJ oldPen = SelectObject(hdc, border);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, popup.left, popup.top, popup.right, popup.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(border);

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
        for (int i = 0; i < 3; ++i) {
            RECT row = ClickerIntervalOptionRect(i);
            const bool checked = clickerSettings_.intervalMode == modes[i];
            HBRUSH rowBrush = CreateSolidBrush(checked ? RGB(232, 245, 238) : kWhite);
            FillRect(hdc, &row, rowBrush);
            DeleteObject(rowBrush);
            RECT box{row.left + 20, row.top + 19, row.left + 38, row.top + 37};
            DrawListCheckbox(hdc, box, checked);
            DrawTextIn(hdc, ClickIntervalTitle(modes[i]), RECT{row.left + 78, row.top + 12, row.right - 20, row.top + 39}, RGB(50, 50, 50));
            DrawTextIn(hdc, descs[i], RECT{row.left + 78, row.top + 41, row.right - 20, row.bottom - 8}, RGB(145, 145, 145));
        }
    }

// --------------------------------------------------
    void MainWindow::DrawEditorCombo(HDC hdc, HWND label, RECT rc, bool dropped) {
        const COLORREF borderColor = dropped ? kMainGreen : kComboBorderGray;
        HBRUSH white = CreateSolidBrush(kWhite);
        FillRect(hdc, &rc, white);
        DeleteObject(white);
        const int arrowW = 26;
        DrawTextIn(hdc, GetText(label).empty() ? L" " : GetText(label), RECT{rc.left + 10, rc.top, rc.right - arrowW, rc.bottom}, kText);
        const int arrowCenterX = rc.right - arrowW / 2;
        const int arrowCenterY = rc.top + (rc.bottom - rc.top) / 2;
        POINT arrow[3] = {
            {arrowCenterX - 5, arrowCenterY - 3},
            {arrowCenterX + 5, arrowCenterY - 3},
            {arrowCenterX,     arrowCenterY + 4}
        };
        HBRUSH arrowBrush = CreateSolidBrush(kMainGreen);
        HGDIOBJ oldBrush = SelectObject(hdc, arrowBrush);
        HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Polygon(hdc, arrow, 3);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(arrowBrush);
        HPEN borderPen = CreatePen(PS_SOLID, 1, borderColor);
        oldPen = SelectObject(hdc, borderPen);
        oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);
    }

// --------------------------------------------------
    void MainWindow::PaintEditorDropPopupContent(HDC hdc, HWND popupHwnd) {
        PopupCombo* pc = GetEditorPopup();
        if (!pc || pc->items.empty()) return;
        RECT client{};
        GetClientRect(popupHwnd, &client);
        HBRUSH white = CreateSolidBrush(kWhite);
        FillRect(hdc, &client, white);
        DeleteObject(white);
        HPEN border = CreatePen(PS_SOLID, 1, kComboPopupBorderGray);
        HGDIOBJ oldPen = SelectObject(hdc, border);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, client.left, client.top, client.right, client.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(border);
        MoveToEx(hdc, client.right - 1, client.top, nullptr);
        LineTo(hdc, client.right - 1, client.bottom - 1);

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
            HBRUSH rowBrush = CreateSolidBrush(rowBg);
            FillRect(hdc, &row, rowBrush);
            DeleteObject(rowBrush);
            DrawTextIn(hdc, pc->items[static_cast<size_t>(i)], RECT{row.left + 10, row.top, row.right - 6, row.bottom},
                selected ? kComboMenuSelectText : kText);
        }
        if (scrollMax > 0) {
            RECT track{client.right - 10, client.top + 1, client.right - 1, client.bottom - 1};
            HBRUSH trackBrush = CreateSolidBrush(kComboScrollTrackGray);
            FillRect(hdc, &track, trackBrush);
            DeleteObject(trackBrush);
            const int trackH = track.bottom - track.top;
            const int thumbH = std::max(18, trackH * visible / total);
            const int thumbTop = track.top + (trackH - thumbH) * editorPopupScroll_ / scrollMax;
            RECT thumb{track.left, thumbTop, track.right, thumbTop + thumbH};
            HBRUSH thumbBrush = CreateSolidBrush(kComboScrollThumbGray);
            FillRect(hdc, &thumb, thumbBrush);
            DeleteObject(thumbBrush);
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
        HBRUSH bg = CreateSolidBrush(kWhite);
        FillRect(hdc, &client, bg);
        DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, kComboPopupBorderGray);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, client.left, client.top, client.right, client.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
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
            HBRUSH headerBrush = CreateSolidBrush(kComboMenuHoverBlue);
            FillRect(hdc, &header, headerBrush);
            DeleteObject(headerBrush);
            DrawTextIn(hdc, item.display, RECT{header.left + 8, header.top, header.right - 4, header.bottom}, kText);
            DrawTextIn(hdc, item.tooltip,
                RECT{client.left + 8, client.top + 28, client.right - 8, client.bottom - 4}, kText,
                DT_LEFT | DT_WORDBREAK);
        }
    }

// --------------------------------------------------
    void MainWindow::PaintEditor(HDC hdc) {
        HPEN pen = CreatePen(PS_SOLID, 1, kLineGreen);
        auto oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, kListX, kListY + 3, nullptr);
        LineTo(hdc, kListX + kListW, kListY + 3);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child) || child == listRemarkEdit_) continue;
            wchar_t cls[16]{};
            GetClassNameW(child, cls, 16);
            if (wcscmp(cls, L"Edit") == 0) DrawEditorFieldBorder(hdc, child);
        }
        SelectObject(hdc, editorFont_);
        const RECT modeRc = WindowClientRect(mode_);
        const RECT actionRc = WindowClientRect(actionCombo_);
        DrawTextIn(hdc, L"模式:", RECT{modeRc.left - 50, modeRc.top + 1, modeRc.left - 6, modeRc.bottom}, kText);
        DrawEditorCombo(hdc, mode_, modeRc, editorPopupOpen_ == 0);
        DrawTextIn(hdc, L"请选择要添加的宏", RECT{actionRc.left, actionRc.top - 28, actionRc.right, actionRc.top - 4}, kText);
        DrawEditorCombo(hdc, actionCombo_, actionRc, editorPopupOpen_ == 1);
        if (IsParamComboVisible(2)) DrawEditorCombo(hdc, mousePressButton_, WindowClientRect(mousePressButton_), editorPopupOpen_ == 2);
        if (IsParamComboVisible(3)) DrawEditorCombo(hdc, clickButton_, WindowClientRect(clickButton_), editorPopupOpen_ == 3);
        if (IsParamComboVisible(4)) DrawEditorCombo(hdc, loopTypeCombo_, WindowClientRect(loopTypeCombo_), editorPopupOpen_ == 4);
        if (IsParamComboVisible(5)) DrawEditorCombo(hdc, runBlockCombo_, WindowClientRect(runBlockCombo_), editorPopupOpen_ == 5);
        if (IsParamComboVisible(6)) DrawEditorCombo(hdc, hotkeyShortcutCombo_, WindowClientRect(hotkeyShortcutCombo_), editorPopupOpen_ == 6);
        if (IsParamComboVisible(7)) DrawEditorCombo(hdc, quickInputVarCombo_, WindowClientRect(quickInputVarCombo_), editorPopupOpen_ == 7);
        if (IsParamComboVisible(8)) DrawEditorCombo(hdc, runMacroCombo_, WindowClientRect(runMacroCombo_), editorPopupOpen_ == 8);
        if (IsParamComboVisible(9)) DrawEditorCombo(hdc, mousePlaybackCombo_, WindowClientRect(mousePlaybackCombo_), editorPopupOpen_ == 9);
        if (IsParamComboVisible(10)) DrawEditorCombo(hdc, scrollDirectionCombo_, WindowClientRect(scrollDirectionCombo_), editorPopupOpen_ == 10);
        if (IsParamComboVisible(11)) DrawEditorCombo(hdc, findFollowUpCombo_, WindowClientRect(findFollowUpCombo_), editorPopupOpen_ == 11);
        if (IsParamComboVisible(12)) DrawEditorCombo(hdc, ifVarCombo_, WindowClientRect(ifVarCombo_), editorPopupOpen_ == 12);
        if (IsParamComboVisible(13)) DrawEditorCombo(hdc, ifOperatorCombo_, WindowClientRect(ifOperatorCombo_), editorPopupOpen_ == 13);
        if (IsParamComboVisible(14)) DrawEditorCombo(hdc, ifConnectorCombo_, WindowClientRect(ifConnectorCombo_), editorPopupOpen_ == 14);
        PaintActionList(hdc);
        PaintDragMarker(hdc);
    }

// --------------------------------------------------
    void MainWindow::DrawEditorFieldBorder(HDC hdc, HWND ctrl) {
        if (!ctrl || !IsWindowVisible(ctrl)) return;
        RECT rc = WindowClientRect(ctrl);
        if (ctrl != name_) InflateRect(&rc, 1, 1);
        HPEN pen = CreatePen(PS_SOLID, 1, kLineGreen);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
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
        PaintActionListLocal(memDc, lw, lh);
        SelectObject(memDc, oldFont);
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
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 154, 72));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, lineLeft, y, nullptr);
        LineTo(hdc, list.right - 4, y);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        POINT marker[3] = {{lineLeft, y - 5}, {lineLeft, y + 5}, {lineLeft + 10, y}};
        HBRUSH brush = CreateSolidBrush(RGB(255, 154, 72));
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        Polygon(hdc, marker, 3);
        SelectObject(hdc, oldBrush);
        DeleteObject(brush);
    }

// --------------------------------------------------
    void MainWindow::DrawTextIn(HDC hdc, const std::wstring& text, RECT rc, COLORREF color, UINT format) { SetTextColor(hdc, color); SetBkMode(hdc, TRANSPARENT); DrawTextW(hdc, text.c_str(), -1, &rc, format); }
