#include "settings_dialog.h"
#include "recording_to_findimage.h"

#include "app_branding.h"
#include "app_settings_store.h"
#include "app_theme.h"
#include "theme_custom_dialog.h"
#include "controls.h"
#include "drawing.h"
#include "input/hid_interception.h"
#include "input/virtual_hid.h"
#include "modern_edit.h"
#include "render_context.h"
#include "scheduled_task_ui.h"
#include "taskbar_window.h"
#include "ui_scale.h"
#include "utils.h"

#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

int SL(int v) { return UiLen(v); }
int DialogContentTop() { return SL(kTitleH + 44); }
int DialogFooterTop(int clientH) { return clientH - SL(52); }
int DialogTabW(int clientW) { return clientW / 5; }
int AiRowH() { return SL(48); }

// 前台注入三选一：文案需完整显示，勿用英文缩写塞进窄格
const wchar_t* BackendOptionLabel(int index) {
    switch (index) {
    case 0: return L"系统模拟";
    case 1: return L"Interception";
    case 2: return L"虚拟HID";
    default: return L"";
    }
}
const wchar_t* kHidInstallBtnText = L"安装 Interception 驱动";
const wchar_t* kVirtualHidInstallBtnText = L"安装虚拟 HID 驱动";

void SetEditText(HWND edit, const std::wstring& text) {
    if (edit) SetWindowTextW(edit, text.c_str());
}

std::wstring FormatDouble4(double v) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"%.4f", v);
    return buf;
}

void MoveCtrl(HWND hwnd, int x, int y, int w, int h) {
    if (hwnd) SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void MoveEditInFrame(HWND edit, int outerX, int outerY, int outerW, int outerH) {
    PositionEditInBorderFrame(edit, outerX, outerY, outerW, outerH);
}

HWND MakeBorderedEdit(HWND parent, const wchar_t* text, int id) {
    return MakeModernSingleLineEdit(parent, text, id, 0, 0, 0, 0);
}

/// 切换子控件可见性且不触发立即重绘，避免「输入框先于父窗自定义绘制」闪一下。
void SetChildVisibleNoRedraw(HWND child, bool visible) {
    if (!child || !IsWindow(child)) return;
    const bool isVis = (GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE) != 0;
    if (isVis == visible) return;
    SetWindowPos(child, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW
        | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

}  // namespace

namespace {
HWND g_activeSettingsDialogHwnd = nullptr;
}  // namespace

HWND SettingsDialog::ActiveHwnd() {
    return g_activeSettingsDialogHwnd;
}

void NotifyActiveSettingsDialogSync() {
    if (g_activeSettingsDialogHwnd) {
        PostMessageW(g_activeSettingsDialogHwnd, WM_SETTINGS_EXTERNAL_SYNC, 0, 0);
    }
}

void NotifyActiveSettingsDialogRelayout() {
    if (g_activeSettingsDialogHwnd) {
        SendMessageW(g_activeSettingsDialogHwnd, WM_APP_UI_LAYOUT_REFRESH, 0, 0);
    }
}

int SettingsDialog::CenteredEditY(int rowTop, int rowHeight) const {
    return rowTop + (rowHeight - SL(kEditH)) / 2;
}

int SettingsDialog::ClientW() const {
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    const int w = static_cast<int>(rc.right - rc.left);
    return w > 0 ? w : UiHomeWidth();
}

int SettingsDialog::ClientH() const {
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    const int h = static_cast<int>(rc.bottom - rc.top);
    return h > 0 ? h : UiHomeHeight();
}

int SettingsDialog::BodyTextWidth(const wchar_t* text) const {
    if (!text || !*text || !hwnd_ || !bodyFont_) return 0;
    HDC hdc = GetDC(hwnd_);
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, bodyFont_));
    SIZE sz{};
    GetTextExtentPoint32W(hdc, text, static_cast<int>(lstrlenW(text)), &sz);
    SelectObject(hdc, oldFont);
    ReleaseDC(hwnd_, hdc);
    return sz.cx;
}

void SettingsDialog::UpdateInlineLayout() {
    const int gap = SL(kCoordEditGap);
    inlineLayout_.randomIntervalEditX = SL(kLabelAfterCheck) + BodyTextWidth(L"启用随机间隔时间，最长为") + gap;
    inlineLayout_.randomUnitX = inlineLayout_.randomIntervalEditX + SL(kEditW) + gap;
    inlineLayout_.pressReleaseEditX = SL(kLabelAfterCheck) + BodyTextWidth(L"启用按下抬起间隔，间隔") + gap;
    inlineLayout_.pressReleaseUnitX = inlineLayout_.pressReleaseEditX + SL(kEditW) + gap;
    inlineLayout_.clickLimitEditX = SL(kLabelAfterCheck) + BodyTextWidth(L"启用次数限制，点击") + gap;
    inlineLayout_.clickLimitSuffixX = inlineLayout_.clickLimitEditX + SL(kEditW) + gap;

    inlineLayout_.playbackCountEditX =
        SL(kLabelAfterCheck) + BodyTextWidth(L"启用回放次数") + gap
        + BodyTextWidth(L"回放") + gap;
    inlineLayout_.playbackCountSuffixX = inlineLayout_.playbackCountEditX + SL(kSmallEditW) + gap;
    inlineLayout_.playbackMinEditX =
        SL(kLabelAfterCheck) + BodyTextWidth(L"启用回放间隔") + gap
        + BodyTextWidth(L"最小间隔") + gap;
    inlineLayout_.playbackMinUnitX = inlineLayout_.playbackMinEditX + SL(kEditW) + gap;
    inlineLayout_.playbackMaxLabelX = inlineLayout_.playbackMinUnitX + BodyTextWidth(L"秒") + gap;
    inlineLayout_.playbackMaxEditX = inlineLayout_.playbackMaxLabelX + BodyTextWidth(L"最大间隔") + gap;
    inlineLayout_.playbackMaxUnitX = inlineLayout_.playbackMaxEditX + SL(kEditW) + gap;
    {
        // 勾选文案与「模板半径：」之间留出明显空隙
        const int afterCapture =
            SL(kLabelAfterCheck) + BodyTextWidth(L"录制时自动截取点击模板") + SL(24);
        inlineLayout_.recordingCaptureHalfSizeEditX =
            afterCapture + BodyTextWidth(L"模板半径：") + gap;
        inlineLayout_.recordingCaptureHalfSizeHintX =
            inlineLayout_.recordingCaptureHalfSizeEditX + SL(kSmallEditW) + gap;
    }

    inlineLayout_.jitterXEditX = SL(kIndent) + BodyTextWidth(L"横坐标抖动(X)") + gap;
    inlineLayout_.jitterYEditX = SL(kJitterYGroupLeft) + BodyTextWidth(L"纵坐标抖动(Y)") + gap;
    inlineLayout_.fixedXEditX = SL(kIndent) + BodyTextWidth(L"横坐标(X)") + gap;
    inlineLayout_.fixedYEditX = SL(kFixedYGroupLeft) + BodyTextWidth(L"纵坐标(Y)") + gap;

    static const wchar_t* kAiLabels[] = {
        L"API 地址:", L"API 密钥:", L"模型名称:", L"温度参数:", L"最大Token:",
    };
    int maxAiLabelW = 0;
    for (const wchar_t* label : kAiLabels) {
        maxAiLabelW = std::max(maxAiLabelW, BodyTextWidth(label));
    }
    inlineLayout_.aiLabelW = maxAiLabelW;
    inlineLayout_.aiEditX = SL(kMargin) + maxAiLabelW + gap;
    inlineLayout_.aiTempHintX = inlineLayout_.aiEditX + SL(kSmallEditW) + gap;
}

void SettingsDialog::CenterEditTextVertically(HWND edit) {
    CenterModernSingleLineEditText(edit);
}

int SettingsDialog::ClickRowY(int index) const {
    const int base = ClickTop();
    const int jitterSub = base + SL(kRowH) * 2 + SL(34);
    const int fixedCb = jitterSub + SL(kSubRowH) + SL(12);
    const int fixedSub = fixedCb + SL(34);
    const int crossY = fixedSub + SL(kSubRowH);
    const int countCb = crossY + SL(kSubRowH) + SL(10);
    switch (index) {
    case 0: return base;
    case 1: return base + SL(kRowH);
    case 2: return base + SL(kRowH) * 2;
    case 3: return fixedCb;
    case 4: return countCb;
    default: return base;
    }
}

RECT SettingsDialog::ClickCheckboxRect(int index) const {
    const int top = ClickRowY(index);
    return RECT{SL(kMargin), top, SL(kMargin) + SL(kCheckboxSize), top + SL(kCheckboxSize)};
}

bool SettingsDialog::HitClickCheckbox(int x, int y, int& outIndex) const {
    for (int i = 0; i < 5; ++i) {
        if (PtIn(ClickCheckboxRect(i), x, y)) { outIndex = i; return true; }
    }
    return false;
}

int SettingsDialog::PlaybackRowY(int index) const {
    const int base = PlaybackTop();
    switch (index) {
    case 0: return base;
    case 1: return base + SL(kRowH);
    case 2: case 3: return base + SL(kRowH) * 2;
    case 4: return base + SL(kRowH) * 3;
    case 5: return base + SL(kRowH) * 4;       // 前台注入选项
    case 6: return base + SL(kRowH) * 5;       // 驱动安装按钮
    default: return base;
    }
}

RECT SettingsDialog::PlaybackCheckboxRect(int index) const {
    const int top = PlaybackRowY(index);
    const int left = (index == 3) ? SL(400) : SL(kMargin);
    return RECT{left, top, left + SL(kCheckboxSize), top + SL(kCheckboxSize)};
}

bool SettingsDialog::HitPlaybackCheckbox(int x, int y, int& outIndex) const {
    for (int i = 0; i < 5; ++i) {
        if (PtIn(PlaybackCheckboxRect(i), x, y)) { outIndex = i; return true; }
    }
    return false;
}

RECT SettingsDialog::BackendOptionRect(int index) const {
    const int y = PlaybackRowY(5);
    const int h = SL(kCheckboxSize);
    const int gapAfter = SL(18);
    const int textPad = SL(6);
    // 标签「注入方式」按实际字宽占位，避免挤在窄左边栏里被裁切
    int left = SL(kMargin) + BodyTextWidth(L"注入方式") + SL(16);
    for (int i = 0; i < 3; ++i) {
        const int w = SL(kCheckboxSize) + textPad + BodyTextWidth(BackendOptionLabel(i)) + SL(4);
        if (i == index) return RECT{left, y, left + w, y + h};
        left += w + gapAfter;
    }
    return RECT{left, y, left, y + h};
}

bool SettingsDialog::HitBackendOption(int x, int y, int& outIndex) const {
    for (int i = 0; i < 3; ++i) {
        if (PtIn(BackendOptionRect(i), x, y)) { outIndex = i; return true; }
    }
    return false;
}

RECT SettingsDialog::HidInstallBtnRect() const {
    const int y = PlaybackRowY(6);
    const int h = SL(kCheckboxSize);
    const int padX = SL(14);
    const int w = BodyTextWidth(kHidInstallBtnText) + padX * 2;
    const int left = SL(kMargin);
    return RECT{left, y, left + std::max(w, SL(160)), y + h};
}

RECT SettingsDialog::VirtualHidInstallBtnRect() const {
    const RECT leftBtn = HidInstallBtnRect();
    const int padX = SL(14);
    const int w = BodyTextWidth(kVirtualHidInstallBtnText) + padX * 2;
    const int left = leftBtn.right + SL(12);
    return RECT{left, leftBtn.top, left + std::max(w, SL(160)), leftBtn.bottom};
}

void SettingsDialog::InstallHidDriver() {
    const std::wstring appDir = AppDir();
    const std::wstring candidates[] = {
        appDir + L"\\install-interception.exe",
        appDir + L"\\tools\\install-interception.exe",
    };
    std::wstring path;
    for (const auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            path = c;
            break;
        }
    }
    if (path.empty()) {
        ShowPromptAlert(L"未找到 Interception 安装程序。\n请使用完整发版包，或重新解压后再试。");
        return;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = hwnd_;
    sei.lpVerb = L"runas";
    sei.lpFile = path.c_str();
    sei.lpParameters = L"/install";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            ShowPromptAlert(L"已取消安装，需要管理员权限才能继续。");
        } else {
            ShowPromptAlert(L"启动 Interception 安装程序失败。");
        }
        return;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 60000);
        CloseHandle(sei.hProcess);
    }

    std::wstring probeErr;
    const bool ready = HidInterceptionBackend::Instance().ProbeAvailable(&probeErr);
    if (ready) {
        ShowPromptAlert(L"Interception 驱动已就绪。\n可在上方选择「Interception」后回放。");
    } else {
        ShowPromptAlert(L"安装程序已执行。\n请重启电脑后再选择「Interception」。");
    }
}

void SettingsDialog::InstallVirtualHidDriver() {
    const std::wstring appDir = AppDir();
    const std::wstring candidates[] = {
        appDir + L"\\driver\\qst_vhid\\sign_and_install.ps1",
        appDir + L"\\..\\driver\\qst_vhid\\sign_and_install.ps1",
        appDir + L"\\install-qst-vhid.ps1",
    };
    std::wstring script;
    for (const auto& c : candidates) {
        wchar_t full[MAX_PATH]{};
        if (GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr) == 0) continue;
        if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) {
            script = full;
            break;
        }
    }
    // Dev tree: resolve relative to exe → repo root
    if (script.empty()) {
        std::wstring tryPath = appDir + L"\\..\\..\\driver\\qst_vhid\\sign_and_install.ps1";
        wchar_t full[MAX_PATH]{};
        if (GetFullPathNameW(tryPath.c_str(), MAX_PATH, full, nullptr)
            && GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) {
            script = full;
        }
    }
    if (script.empty()) {
        ShowPromptAlert(L"未找到虚拟 HID 安装脚本。\n请使用完整发版包，或重新解压后再试。");
        return;
    }

    std::wstring scriptDir = script;
    const size_t slash = scriptDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) scriptDir.resize(slash);
    const std::wstring logPath = scriptDir + L"\\install_log.txt";

    // 优先走带日志包装的提升脚本；否则直接 -File
    std::wstring elevate = scriptDir + L"\\_elevate_install.ps1";
    std::wstring params;
    if (GetFileAttributesW(elevate.c_str()) != INVALID_FILE_ATTRIBUTES) {
        params = L"-NoProfile -ExecutionPolicy Bypass -File \"" + elevate + L"\"";
    } else {
        params = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -SkipBuild";
    }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = hwnd_;
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            ShowPromptAlert(L"已取消安装，需要管理员权限才能继续。");
        } else {
            ShowPromptAlert(L"启动虚拟 HID 安装脚本失败。");
        }
        return;
    }
    DWORD exitCode = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 180000);
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
    }

    std::wstring probeErr;
    if (VirtualHidBackend::Instance().ProbeAvailable(&probeErr)) {
        ShowPromptAlert(L"虚拟 HID 已就绪。\n可在上方选择「虚拟HID」后回放。");
        return;
    }

    std::wstring msg = L"虚拟 HID 仍未就绪。\n请重启电脑后再次点击安装。";
    if (exitCode != 0) {
        msg += L"\n安装退出码：" + std::to_wstring(exitCode) + L"。";
    }
    msg += L"\n详细日志：\n" + logPath;
    ShowPromptAlert(msg);
}

int SettingsDialog::OtherColWidth() const {
    return (ClientW() - SL(kMargin) * 2 - SL(16)) / 2;
}

int SettingsDialog::OtherRightColLeft() const {
    return SL(kMargin) + OtherColWidth() + SL(16);
}

RECT SettingsDialog::OtherCheckboxRect(int index) const {
    const int col = index / 3;
    const int row = index % 3;
    const int left = (col == 0) ? SL(kMargin) : OtherRightColLeft();
    const int top = DialogContentTop() + SL(kContentPad) + row * SL(kRowH);
    return RECT{left, top, left + SL(kCheckboxSize), top + SL(kCheckboxSize)};
}

RECT SettingsDialog::OtherCheckboxLabelRect(int index) const {
    const RECT box = OtherCheckboxRect(index);
    const int col = index / 3;
    const int right = (col == 0)
        ? OtherRightColLeft() - SL(8)
        : ClientW() - SL(kMargin);
    return RECT{box.right + SL(10), box.top, right, box.bottom};
}

RECT SettingsDialog::OtherHoldLabelRect() const {
    const int top = DialogContentTop() + SL(kContentPad) + 3 * SL(kRowH) + SL(4);
    return RECT{SL(kMargin), top, SL(kMargin) + SL(100), top + SL(kAiTopRowBtnH)};
}

RECT SettingsDialog::OtherHoldEditRect() const {
    const RECT label = OtherHoldLabelRect();
    return RECT{label.right + SL(8), label.top,
        label.right + SL(8) + SL(kEditW), label.bottom};
}

RECT SettingsDialog::OtherHoldUnitRect() const {
    const RECT edit = OtherHoldEditRect();
    return RECT{edit.right + SL(8), edit.top, edit.right + SL(40), edit.bottom};
}

RECT SettingsDialog::OtherThemeLabelRect() const {
    const int top = DialogContentTop() + SL(kContentPad) + 3 * SL(kRowH) + SL(4) + SL(kRowH);
    return RECT{SL(kMargin), top, SL(kMargin) + SL(kThemeLabelW), top + SL(kAiTopRowBtnH)};
}

RECT SettingsDialog::OtherThemeComboRect() const {
    const RECT label = OtherThemeLabelRect();
    return RECT{label.right + SL(12), label.top,
        label.right + SL(12) + SL(kThemeComboW), label.bottom};
}

void SettingsDialog::RefreshThemeCombo() {
    std::vector<std::wstring> names;
    names.reserve(static_cast<size_t>(quickscript::kThemeCount) + 1);
    names.push_back(L"自定义");
    const quickscript::AppTheme* catalog = quickscript::ThemeCatalog();
    for (int i = 0; i < quickscript::kThemeCount; ++i)
        names.push_back(catalog[i].name ? catalog[i].name : L"");
    themeCombo_.SetItems(std::move(names));

    working_.other.themeId = std::clamp(working_.other.themeId, 0, quickscript::kThemeCount - 1);
    const int sel = working_.other.useCustomTheme
        ? quickscript::kCustomThemeComboIndex
        : (working_.other.themeId + 1);
    themeCombo_.SetSelectedIndex(sel);
}

RECT SettingsDialog::AiCheckboxRect() const {
    const int top = DialogContentTop() + SL(kContentPad);
    return RECT{SL(kMargin), top, SL(kMargin) + SL(kCheckboxSize), top + SL(kCheckboxSize)};
}

RECT SettingsDialog::AiAddModelBtnRect() const {
    const int top = DialogContentTop() + SL(kContentPad);
    const int right = ClientW() - SL(kMargin);
    return RECT{right - SL(kAiAddModelBtnW), top, right, top + SL(kAiTopRowBtnH)};
}

RECT SettingsDialog::AiDeleteModelBtnRect() const {
    const RECT add = AiAddModelBtnRect();
    return RECT{add.left - SL(kAiTopRowGap) - SL(kAiDeleteBtnW), add.top,
        add.left - SL(kAiTopRowGap), add.bottom};
}

RECT SettingsDialog::AiModelComboRect() const {
    const RECT del = AiDeleteModelBtnRect();
    return RECT{del.left - SL(kAiTopRowGap) - SL(kAiModelComboW), del.top,
        del.left - SL(kAiTopRowGap), del.bottom};
}

bool SettingsDialog::HitAiAddModelBtn(int x, int y) const {
    return activeTab_ == Tab::AiApi && PtIn(AiAddModelBtnRect(), x, y);
}

bool SettingsDialog::HitAiDeleteModelBtn(int x, int y) const {
    return activeTab_ == Tab::AiApi && PtIn(AiDeleteModelBtnRect(), x, y);
}

quickscript::AiModelProfile SettingsDialog::ReadAiProfileFromControls() const {
    quickscript::AiModelProfile profile{};
    profile.apiUrl = GetText(editApiUrl_);
    profile.apiKey = GetText(editApiKey_);
    profile.modelName = GetText(editModelName_);
    profile.temperature = ToDouble(editTemperature_, 0.3);
    profile.maxTokens = ToInt(editMaxTokens_, 4096);
    profile.maxTokens = std::clamp(profile.maxTokens, 1, 393216);
    return profile;
}

void SettingsDialog::LoadAiProfileIntoControls(int index) {
    if (index < 0 || index >= static_cast<int>(working_.ai.savedModels.size())) return;
    const auto& profile = working_.ai.savedModels[index];
    SetEditText(editApiUrl_, profile.apiUrl);
    SetEditText(editApiKey_, profile.apiKey);
    SetEditText(editModelName_, profile.modelName);
    SetEditText(editTemperature_, std::to_wstring(profile.temperature));
    SetEditText(editMaxTokens_, std::to_wstring(profile.maxTokens));
}

void SettingsDialog::RefreshAiModelCombo() {
    std::vector<std::wstring> names;
    names.reserve(working_.ai.savedModels.size());
    for (const auto& model : working_.ai.savedModels)
        names.push_back(model.modelName);
    aiModelCombo_.SetItems(std::move(names));
    if (working_.ai.savedModels.empty()) {
        aiModelCombo_.SetSelectedIndex(-1);
        return;
    }
    int selectIndex = 0;
    const std::wstring currentName = Trim(GetText(editModelName_));
    for (size_t i = 0; i < working_.ai.savedModels.size(); ++i) {
        if (working_.ai.savedModels[i].modelName == currentName) {
            selectIndex = static_cast<int>(i);
            break;
        }
    }
    aiModelCombo_.SetSelectedIndex(selectIndex);
}

void SettingsDialog::AddCurrentAiProfile() {
    SyncSettingsFromControls();
    const auto profile = ReadAiProfileFromControls();
    if (Trim(profile.modelName).empty()) {
        ShowPromptAlert(L"请先填写模型名称。");
        return;
    }
    if (Trim(profile.apiUrl).empty() || Trim(profile.apiKey).empty()) {
        ShowPromptAlert(L"请先填写 API 地址和 API 密钥。");
        return;
    }
    auto& models = working_.ai.savedModels;
    const auto it = std::find_if(models.begin(), models.end(),
        [&](const quickscript::AiModelProfile& m) { return m.modelName == profile.modelName; });
    if (it != models.end()) *it = profile;
    else models.push_back(profile);
    RefreshAiModelCombo();
    for (size_t i = 0; i < models.size(); ++i) {
        if (models[i].modelName == profile.modelName) {
            aiModelCombo_.SetSelectedIndex(static_cast<int>(i));
            break;
        }
    }
    PersistWorkingSettings();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDialog::DeleteSelectedAiProfile() {
    const int sel = aiModelCombo_.SelectedIndex();
    if (sel < 0 || sel >= static_cast<int>(working_.ai.savedModels.size())) {
        ShowPromptAlert(L"请先在模型列表中选择要删除的模型。");
        return;
    }
    aiModelCombo_.Close();
    working_.ai.savedModels.erase(working_.ai.savedModels.begin() + sel);
    RefreshAiModelCombo();
    PersistWorkingSettings();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDialog::PersistWorkingSettings() {
    SyncSettingsFromControls();
    if (settings_) {
        *settings_ = working_;
        SaveAppSettings(*settings_);
    }
}

void SettingsDialog::ToggleCheckbox(Tab tab, int index) {
    if (tab == Tab::Click) {
        auto& c = working_.click;
        switch (index) {
        case 0: c.enableRandomInterval = !c.enableRandomInterval; break;
        case 1: c.enablePressReleaseInterval = !c.enablePressReleaseInterval; break;
        case 2: c.enableCoordinateJitter = !c.enableCoordinateJitter; break;
        case 3: c.enableFixedCoordinates = !c.enableFixedCoordinates; break;
        case 4: c.enableClickCountLimit = !c.enableClickCountLimit; break;
        default: break;
        }
    } else if (tab == Tab::Playback) {
        auto& p = working_.playback;
        switch (index) {
        case 0: p.enablePlaybackCount = !p.enablePlaybackCount; break;
        case 1: p.enablePlaybackInterval = !p.enablePlaybackInterval; break;
        case 2: p.enableDebugOutputWindow = !p.enableDebugOutputWindow; break;
        case 3: p.autoOutputKeyFunctionDebug = !p.autoOutputKeyFunctionDebug; break;
        case 4: p.recordingClickCaptureEnabled = !p.recordingClickCaptureEnabled; break;
        default: break;
        }
        p.enableHidDriverSimulation =
            p.foregroundInputBackend != quickscript::ForegroundInputBackend::Software;
    } else if (tab == Tab::Other) {
        auto& o = working_.other;
        switch (index) {
        case 0: o.autoHideMainWindow = !o.autoHideMainWindow; break;
        case 1: o.playSoundOnStart = !o.playSoundOnStart; break;
        case 2: o.hideBottomRightTip = !o.hideBottomRightTip; break;
        case 3: o.closeToTray = !o.closeToTray; break;
        case 4: o.autoStartOnBoot = !o.autoStartOnBoot; break;
        case 5: o.resolveImeConflict = !o.resolveImeConflict; break;
        default: break;
        }
    } else if (tab == Tab::AiApi) {
        working_.ai.enabled = !working_.ai.enabled;
    }
    UpdateControlVisibility();
}

void SettingsDialog::PositionChildControls() {
    UpdateInlineLayout();

    const int jitterSubY = ClickRowY(2) + SL(kSubLineOffset);
    const int fixedSubY = ClickRowY(3) + SL(kSubLineOffset);
    const int crossY = fixedSubY + SL(kSubRowH) + SL(4);

    MoveEditInFrame(editRandomInterval_, inlineLayout_.randomIntervalEditX,
        CenteredEditY(ClickRowY(0), SL(kCheckboxSize)), SL(kEditW), SL(kEditH));
    MoveEditInFrame(editPressRelease_, inlineLayout_.pressReleaseEditX,
        CenteredEditY(ClickRowY(1), SL(kCheckboxSize)), SL(kEditW), SL(kEditH));
    MoveEditInFrame(editJitterX_, inlineLayout_.jitterXEditX, CenteredEditY(jitterSubY, SL(kSubRowH)), SL(kSmallEditW), SL(kEditH));
    MoveEditInFrame(editJitterY_, inlineLayout_.jitterYEditX, CenteredEditY(jitterSubY, SL(kSubRowH)), SL(kSmallEditW), SL(kEditH));
    MoveEditInFrame(editFixedX_, inlineLayout_.fixedXEditX, CenteredEditY(fixedSubY, SL(kSubRowH)), SL(kSmallEditW), SL(kEditH));
    MoveEditInFrame(editFixedY_, inlineLayout_.fixedYEditX, CenteredEditY(fixedSubY, SL(kSubRowH)), SL(kSmallEditW), SL(kEditH));
    MoveCtrl(crosshairBtn_, SL(kIndent), crossY, SL(kCrosshairBtnW), SL(kCrosshairBtnH));
    MoveEditInFrame(editClickLimit_, inlineLayout_.clickLimitEditX,
        CenteredEditY(ClickRowY(4), SL(kCheckboxSize)), SL(kEditW), SL(kEditH));

    const int pb0 = PlaybackRowY(0);
    const int pb1 = PlaybackRowY(1);
    const int pb4 = PlaybackRowY(4);
    MoveEditInFrame(editPlaybackCount_, inlineLayout_.playbackCountEditX,
        CenteredEditY(pb0, SL(kCheckboxSize)), SL(kSmallEditW), SL(kEditH));
    MoveEditInFrame(editPlaybackMin_, inlineLayout_.playbackMinEditX,
        CenteredEditY(pb1, SL(kCheckboxSize)), SL(kEditW), SL(kEditH));
    MoveEditInFrame(editPlaybackMax_, inlineLayout_.playbackMaxEditX,
        CenteredEditY(pb1, SL(kCheckboxSize)), SL(kEditW), SL(kEditH));
    MoveEditInFrame(editRecordingCaptureHalfSize_, inlineLayout_.recordingCaptureHalfSizeEditX,
        CenteredEditY(pb4, SL(kCheckboxSize)), SL(kSmallEditW), SL(kEditH));

    {
        const RECT holdEdit = OtherHoldEditRect();
        MoveEditInFrame(editHoldThreshold_, holdEdit.left, holdEdit.top,
            holdEdit.right - holdEdit.left, holdEdit.bottom - holdEdit.top);
    }

    const int aiTop = DialogContentTop() + SL(kContentPad);
    const int aiEditW = ClientW() - inlineLayout_.aiEditX - SL(kMargin);
    MoveEditInFrame(editApiUrl_, inlineLayout_.aiEditX, CenteredEditY(aiTop + AiRowH(), AiRowH()), aiEditW, SL(kEditH));
    MoveEditInFrame(editApiKey_, inlineLayout_.aiEditX, CenteredEditY(aiTop + AiRowH() * 2, AiRowH()), aiEditW, SL(kEditH));
    MoveEditInFrame(editModelName_, inlineLayout_.aiEditX, CenteredEditY(aiTop + AiRowH() * 3, AiRowH()), aiEditW, SL(kEditH));
    MoveEditInFrame(editTemperature_, inlineLayout_.aiEditX, CenteredEditY(aiTop + AiRowH() * 4, AiRowH()), SL(kSmallEditW), SL(kEditH));
    MoveEditInFrame(editMaxTokens_, inlineLayout_.aiEditX, CenteredEditY(aiTop + AiRowH() * 5, AiRowH()), SL(kSmallEditW), SL(kEditH));

    const HWND edits[] = {
        editRandomInterval_, editPressRelease_, editJitterX_, editJitterY_,
        editFixedX_, editFixedY_, editClickLimit_,
        editPlaybackCount_, editPlaybackMin_, editPlaybackMax_, editRecordingCaptureHalfSize_,
        editHoldThreshold_,
        editApiUrl_, editApiKey_, editModelName_, editTemperature_, editMaxTokens_,
    };
    for (HWND edit : edits) CenterEditTextVertically(edit);
}

bool SettingsDialog::Show(HWND owner, quickscript::AppSettings& settings, SavedCallback onSaved) {
    if (IsAlive()) {
        SetForegroundWindow(hwnd_);
        return true;
    }

    owner_ = owner;
    settings_ = &settings;
    working_ = settings;
    saved_ = false;
    onSaved_ = std::move(onSaved);
    activeTab_ = Tab::Click;

    static bool registered = false;
    const wchar_t* cls = L"QuickScriptSettingsDlg";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &SettingsDialog::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    RECT ownerRc{};
    if (owner && IsWindow(owner))
        GetWindowRect(owner, &ownerRc);

    // 单例设置窗与主窗重叠；不级联偏移
    hwnd_ = CreateWindowExW(0, cls, L"", WS_POPUP | WS_CLIPCHILDREN,
        ownerRc.left, ownerRc.top, UiHomeWidth(), UiHomeHeight(), nullptr, nullptr,
        GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    UiResizeHwndToHome(hwnd_);
    PositionChildControls();

    g_activeSettingsDialogHwnd = hwnd_;

    ApplyTaskbarWindowStyle(hwnd_, L"鼠大侠-设置", true);
    outerShadow_.Attach(hwnd_);

    // 先显示但不立即重绘，再同步刷父窗自绘 + 全部子控件，避免输入框先于标签/标题出现。
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOREDRAW);
    RedrawWindow(hwnd_, nullptr, nullptr,
        RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    SetForegroundWindow(hwnd_);
    return true;
}

SettingsDialog::SettingsDialog() = default;

SettingsDialog::~SettingsDialog() {
    onSaved_ = nullptr;
    if (IsAlive()) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK SettingsDialog::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SettingsDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsDialog::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (crosshairDrag_.IsActive()) {
        if (crosshairDrag_.HandleMessage(msg, wp, lp,
            [this](int x, int y) {
                SetEditText(editFixedX_, std::to_wstring(x));
                SetEditText(editFixedY_, std::to_wstring(y));
            },
            nullptr)) {
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    switch (msg) {
    case WM_CREATE: {
        RecreateUiFonts();

        crosshairDragCursor_ = CreateCrosshairDragCursor(kCrosshairBlue);
        crosshairDrag_.SetOwner(hwnd_);
        crosshairDrag_.SetDragCursor(crosshairDragCursor_);

        editRandomInterval_ = MakeBorderedEdit(hwnd_, L"0.5000", kEditRandomInterval);
        editPressRelease_ = MakeBorderedEdit(hwnd_, L"0.0010", kEditPressRelease);
        editJitterX_ = MakeBorderedEdit(hwnd_, L"2", kEditJitterX);
        editJitterY_ = MakeBorderedEdit(hwnd_, L"2", kEditJitterY);
        editFixedX_ = MakeBorderedEdit(hwnd_, L"0", kEditFixedX);
        editFixedY_ = MakeBorderedEdit(hwnd_, L"0", kEditFixedY);
        crosshairBtn_ = MakeGreenButton(hwnd_, L"拖动准星到需要点击的地方", kCrosshairBtn, 0, 0, SL(kCrosshairBtnW), SL(kCrosshairBtnH));
        editClickLimit_ = MakeBorderedEdit(hwnd_, L"0", kEditClickLimit);
        editPlaybackCount_ = MakeBorderedEdit(hwnd_, L"1", kEditPlaybackCount);
        editPlaybackMin_ = MakeBorderedEdit(hwnd_, L"0.5000", kEditPlaybackMin);
        editPlaybackMax_ = MakeBorderedEdit(hwnd_, L"1.0000", kEditPlaybackMax);
        editRecordingCaptureHalfSize_ = MakeBorderedEdit(hwnd_, L"40", kEditRecordingCaptureHalfSize);
        editHoldThreshold_ = MakeBorderedEdit(hwnd_, L"0.2", kEditHoldThreshold);

        editApiUrl_ = MakeBorderedEdit(hwnd_, L"", kEditApiUrl);
        editApiKey_ = MakeModernSingleLineEdit(hwnd_, L"", kEditApiKey, 0, 0, 0, 0, ES_PASSWORD);
        editModelName_ = MakeBorderedEdit(hwnd_, L"gpt-4o", kEditModelName);
        editTemperature_ = MakeBorderedEdit(hwnd_, L"0.3", kEditTemperature);
        editMaxTokens_ = MakeBorderedEdit(hwnd_, L"4096", kEditMaxTokens);
        aiModelCombo_.Init(hwnd_, bodyFont_);
        aiModelCombo_.SetPlaceholder(L"请选择已添加的模型");
        aiModelCombo_.SetSelectionCallback([this](int index) {
            LoadAiProfileIntoControls(index);
        });
        themeCombo_.Init(hwnd_, bodyFont_);
        themeCombo_.SetPlaceholder(L"请选择主题");
        themeCombo_.SetSelectionCallback([this](int index) {
            const int total = quickscript::kThemeCount + 1;
            if (index < 0 || index >= total) return;
            if (index == quickscript::kCustomThemeComboIndex) {
                COLORREF main = static_cast<COLORREF>(working_.other.customMainColor & 0xFFFFFF);
                COLORREF accent = static_cast<COLORREF>(working_.other.customAccentColor & 0xFFFFFF);
                if (!quickscript::ShowCustomThemePicker(hwnd_, main, accent)) {
                    RefreshThemeCombo();
                    return;
                }
                working_.other.useCustomTheme = true;
                working_.other.customMainColor = static_cast<int>(main & 0xFFFFFF);
                working_.other.customAccentColor = static_cast<int>(accent & 0xFFFFFF);
            } else {
                working_.other.useCustomTheme = false;
                working_.other.themeId = index - 1;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        });

        crosshairDrag_.RegisterButton(crosshairBtn_, {CrosshairDragMode::Coordinates, nullptr});
        SetWindowSubclass(crosshairBtn_, [](HWND btn, UINT msg, WPARAM wp, LPARAM lp,
            UINT_PTR, DWORD_PTR refData) -> LRESULT {
            if (msg == WM_LBUTTONDOWN) {
                auto* self = reinterpret_cast<SettingsDialog*>(refData);
                CrosshairDragBinding binding{};
                if (self && self->crosshairDrag_.TryGetBinding(btn, binding)) {
                    self->crosshairDrag_.Begin(binding);
                    return 0;
                }
            }
            return DefSubclassProc(btn, msg, wp, lp);
        }, 0, reinterpret_cast<DWORD_PTR>(this));
        ApplyFont(hwnd_, bodyFont_);
        UpdateInlineLayout();
        // Make* 默认 WS_VISIBLE：先全部藏起，再按当前 Tab 显示，避免首帧露出未布局的编辑框。
        {
            const HWND hideAll[] = {
                editRandomInterval_, editPressRelease_, editJitterX_, editJitterY_,
                editFixedX_, editFixedY_, editClickLimit_, crosshairBtn_,
                editPlaybackCount_, editPlaybackMin_, editPlaybackMax_, editRecordingCaptureHalfSize_,
                editHoldThreshold_,
                editApiUrl_, editApiKey_, editModelName_, editTemperature_, editMaxTokens_,
            };
            for (HWND h : hideAll) SetChildVisibleNoRedraw(h, false);
        }
        PositionChildControls();
        SyncControlsFromSettings();
        RefreshAiModelCombo();
        RefreshThemeCombo();
        UpdateControlVisibility();
        promptModal_.Bind(hwnd_, bodyFont_);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkColor(hdc, kWhite);
        SetTextColor(hdc, kText);
        static HBRUSH brush = CreateSolidBrush(kWhite);
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        PositionChildControls();
        return 0;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        if (owner_ && IsWindow(owner_))
            PostMessageW(owner_, WM_APP_UI_SCALE_SYNC, 0, 1);
        return 0;
    case WM_APP_UI_LAYOUT_REFRESH:
        RelayoutForScaleChange();
        return 0;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (HitClose(pt.x, pt.y)) return HTCLIENT;
        if (HitTitle(pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis && crosshairDrag_.IsCrosshairButton(dis->hwndItem)) {
            crosshairDrag_.DrawButton(dis, bodyFont_, hoverCrosshairBtn_);
            return TRUE;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
    case WM_MOVE:
    case WM_WINDOWPOSCHANGED:
        if (aiModelCombo_.IsOpen()) aiModelCombo_.SyncPopupPosition(AiModelComboRect());
        if (themeCombo_.IsOpen()) themeCombo_.SyncPopupPosition(OtherThemeComboRect());
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            aiModelCombo_.Close();
            themeCombo_.Close();
        }
        if (LOWORD(wp) == WA_INACTIVE && crosshairDrag_.IsActive()) crosshairDrag_.End();
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (promptModal_.visible()) return 0;
        if (HitClose(x, y)) {
            aiModelCombo_.Close();
            themeCombo_.Close();
            DestroyWindow(hwnd_);
            return 0;
        }
        if (PtIn(RestoreLinkRect(), x, y)) { RestoreDefaults(); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
        if (PtIn(SaveBtnRect(), x, y)) { SaveAndClose(); return 0; }
        if (activeTab_ == Tab::About && PtIn(CheckUpgradeBtnRect(), x, y)) {
            ShowPromptAlert(L"当前已是最新版本。");
            return 0;
        }
        for (int t = 0; t < 5; ++t) {
            if (PtIn(TabRect(static_cast<Tab>(t)), x, y)) {
                if (activeTab_ != static_cast<Tab>(t)) {
                    aiModelCombo_.Close();
                    themeCombo_.Close();
                }
                activeTab_ = static_cast<Tab>(t);
                UpdateControlVisibility();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        HWND crosshairHit = crosshairDrag_.HitButton(x, y, [this](HWND btn) {
            RECT rc{}; GetWindowRect(btn, &rc);
            POINT tl{rc.left, rc.top};
            ScreenToClient(hwnd_, &tl);
            return RECT{tl.x, tl.y, tl.x + (rc.right - rc.left), tl.y + (rc.bottom - rc.top)};
        });
        if (crosshairHit) {
            CrosshairDragBinding b{};
            if (crosshairDrag_.TryGetBinding(crosshairHit, b)) crosshairDrag_.Begin(b);
            return 0;
        }
        if (activeTab_ == Tab::Click) {
            int row = -1;
            if (HitClickCheckbox(x, y, row)) {
                ToggleCheckbox(Tab::Click, row);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        } else if (activeTab_ == Tab::Playback) {
            if (PtIn(HidInstallBtnRect(), x, y)) {
                InstallHidDriver();
                return 0;
            }
            if (PtIn(VirtualHidInstallBtnRect(), x, y)) {
                InstallVirtualHidDriver();
                return 0;
            }
            int backend = -1;
            if (HitBackendOption(x, y, backend)) {
                working_.playback.foregroundInputBackend =
                    quickscript::ClampForegroundInputBackend(backend);
                working_.playback.enableHidDriverSimulation =
                    working_.playback.foregroundInputBackend
                    != quickscript::ForegroundInputBackend::Software;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            int row = -1;
            if (HitPlaybackCheckbox(x, y, row)) {
                ToggleCheckbox(Tab::Playback, row);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        } else if (activeTab_ == Tab::Other) {
            for (int row = 0; row < 6; ++row) {
                const RECT box = OtherCheckboxRect(row);
                const RECT label = OtherCheckboxLabelRect(row);
                const RECT hit{box.left, box.top, label.right, box.bottom};
                if (PtIn(hit, x, y)) {
                    ToggleCheckbox(Tab::Other, row);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            }
            if (themeCombo_.HitField(OtherThemeComboRect(), x, y)) {
                themeCombo_.Toggle(OtherThemeComboRect());
                return 0;
            }
        } else if (activeTab_ == Tab::AiApi) {
            if (PtIn(AiCheckboxRect(), x, y)) {
                ToggleCheckbox(Tab::AiApi, 0);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (HitAiAddModelBtn(x, y)) {
                AddCurrentAiProfile();
                return 0;
            }
            if (HitAiDeleteModelBtn(x, y)) {
                DeleteSelectedAiProfile();
                return 0;
            }
            if (aiModelCombo_.HitField(AiModelComboRect(), x, y)) {
                aiModelCombo_.Toggle(AiModelComboRect());
                return 0;
            }
        }
        if (aiModelCombo_.IsOpen()) {
            POINT screenPt{x, y};
            ClientToScreen(hwnd_, &screenPt);
            if (!aiModelCombo_.HitPopupScreen(screenPt.x, screenPt.y)
                && !aiModelCombo_.HitField(AiModelComboRect(), x, y)) {
                aiModelCombo_.Close();
            }
        }
        if (themeCombo_.IsOpen()) {
            POINT screenPt{x, y};
            ClientToScreen(hwnd_, &screenPt);
            if (!themeCombo_.HitPopupScreen(screenPt.x, screenPt.y)
                && !themeCombo_.HitField(OtherThemeComboRect(), x, y)) {
                themeCombo_.Close();
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (promptModal_.visible()) return 0;
        bool needRedraw = false;
        if (HitClose(x, y) != hoverClose_) { hoverClose_ = HitClose(x, y); needRedraw = true; }
        if (PtIn(RestoreLinkRect(), x, y) != hoverRestore_) { hoverRestore_ = PtIn(RestoreLinkRect(), x, y); needRedraw = true; }
        if (PtIn(SaveBtnRect(), x, y) != hoverSave_) { hoverSave_ = PtIn(SaveBtnRect(), x, y); needRedraw = true; }
        const bool hHid = activeTab_ == Tab::Playback && PtIn(HidInstallBtnRect(), x, y);
        if (hHid != hoverHidInstall_) { hoverHidInstall_ = hHid; needRedraw = true; }
        const bool hVhid = activeTab_ == Tab::Playback && PtIn(VirtualHidInstallBtnRect(), x, y);
        if (hVhid != hoverVirtualHidInstall_) { hoverVirtualHidInstall_ = hVhid; needRedraw = true; }
        const bool hu = activeTab_ == Tab::About && PtIn(CheckUpgradeBtnRect(), x, y);
        if (hu != hoverCheckUpgrade_) { hoverCheckUpgrade_ = hu; needRedraw = true; }
        const bool ha = HitAiAddModelBtn(x, y);
        if (ha != hoverAiAddModel_) { hoverAiAddModel_ = ha; needRedraw = true; }
        const bool hd = HitAiDeleteModelBtn(x, y);
        if (hd != hoverAiDeleteModel_) { hoverAiDeleteModel_ = hd; needRedraw = true; }
        const bool hm = activeTab_ == Tab::AiApi && aiModelCombo_.HitField(AiModelComboRect(), x, y);
        if (hm != hoverAiModelCombo_) { hoverAiModelCombo_ = hm; needRedraw = true; }
        const bool ht = activeTab_ == Tab::Other && themeCombo_.HitField(OtherThemeComboRect(), x, y);
        if (ht != hoverThemeCombo_) { hoverThemeCombo_ = ht; needRedraw = true; }
        HWND ch = crosshairDrag_.HitButton(x, y, [this](HWND btn) {
            RECT rc{}; GetWindowRect(btn, &rc);
            POINT tl{rc.left, rc.top};
            ScreenToClient(hwnd_, &tl);
            return RECT{tl.x, tl.y, tl.x + (rc.right - rc.left), tl.y + (rc.bottom - rc.top)};
        });
        if (ch != hoverCrosshairBtn_) {
            if (hoverCrosshairBtn_) InvalidateRect(hoverCrosshairBtn_, nullptr, FALSE);
            hoverCrosshairBtn_ = ch;
            if (hoverCrosshairBtn_) InvalidateRect(hoverCrosshairBtn_, nullptr, FALSE);
        }
        if (needRedraw) InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT:
        Paint();
        return 0;
    case WM_IME_SETCONTEXT:
        if (wp) lp |= 0x01000000 | 0x80000000;
        return DefWindowProcW(hwnd_, msg, wp, lp);

    case WM_SETTINGS_EXTERNAL_SYNC:
        if (settings_) working_ = *settings_;
        SyncControlsFromSettings();
        RefreshAiModelCombo();
        RefreshThemeCombo();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DESTROY:
        outerShadow_.Detach();
        aiModelCombo_.Destroy();
        themeCombo_.Destroy();
        g_activeSettingsDialogHwnd = nullptr;
        CleanupGdi();
        hwnd_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void SettingsDialog::RecreateUiFonts() {
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (closeFont_) { DeleteObject(closeFont_); closeFont_ = nullptr; }
    if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
    if (tabFont_) { DeleteObject(tabFont_); tabFont_ = nullptr; }
    if (smallFont_) { DeleteObject(smallFont_); smallFont_ = nullptr; }
    if (aboutTitleFont_) { DeleteObject(aboutTitleFont_); aboutTitleFont_ = nullptr; }
    titleFont_ = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    closeFont_ = CreateFontW(UiFontHeight(36), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    bodyFont_ = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    tabFont_ = CreateFontW(UiFontHeight(28), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    smallFont_ = CreateFontW(UiFontHeight(22), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    aboutTitleFont_ = CreateFontW(UiFontHeight(42), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
}

void SettingsDialog::RelayoutForScaleChange() {
    if (!hwnd_) return;
    RecreateUiFonts();
    ApplyFont(hwnd_, bodyFont_);
    aiModelCombo_.Init(hwnd_, bodyFont_);
    themeCombo_.Init(hwnd_, bodyFont_);
    UpdateInlineLayout();
    UiResizeWindowClient(hwnd_, UiHomeWidth(), UiHomeHeight(), true);
    PositionChildControls();
    RedrawWindow(hwnd_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void SettingsDialog::CleanupGdi() {
    if (crosshairDragCursor_) { DestroyCursor(crosshairDragCursor_); crosshairDragCursor_ = nullptr; }
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (closeFont_) { DeleteObject(closeFont_); closeFont_ = nullptr; }
    if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
    if (tabFont_) { DeleteObject(tabFont_); tabFont_ = nullptr; }
    if (smallFont_) { DeleteObject(smallFont_); smallFont_ = nullptr; }
    if (aboutTitleFont_) { DeleteObject(aboutTitleFont_); aboutTitleFont_ = nullptr; }
}

bool SettingsDialog::PtIn(const RECT& rc, int x, int y) const { return StPtIn(rc, x, y); }
bool SettingsDialog::HitClose(int x, int y) const { return PtIn(CloseRect(), x, y); }
bool SettingsDialog::HitTitle(int x, int y) const { return y >= 0 && y < SL(kTitleH) && x < CloseRect().left; }

RECT SettingsDialog::CloseRect() const {
    const int w = ClientW();
    return RECT{w - SL(kCloseBtnW), 0, w, SL(kTitleH)};
}
RECT SettingsDialog::TabRect(Tab tab) const {
    const int idx = static_cast<int>(tab);
    const int tabW = DialogTabW(ClientW());
    return RECT{idx * tabW, SL(kTitleH), (idx + 1) * tabW, DialogContentTop()};
}
RECT SettingsDialog::FooterRect() const {
    const int w = ClientW();
    const int h = ClientH();
    return RECT{0, DialogFooterTop(h), w, h};
}
RECT SettingsDialog::RestoreLinkRect() const {
    const int footerTop = DialogFooterTop(ClientH());
    return RECT{SL(kMargin), footerTop + SL(8), SL(kMargin) + SL(180), footerTop + SL(kFooterH) - SL(8)};
}
RECT SettingsDialog::SaveBtnRect() const {
    const int w = ClientW();
    const int footerTop = DialogFooterTop(ClientH());
    return RECT{w - SL(kMargin) - SL(96), footerTop + SL(8), w - SL(kMargin), footerTop + SL(kFooterH) - SL(8)};
}
RECT SettingsDialog::CheckUpgradeBtnRect() const {
    const RECT save = SaveBtnRect();
    return RECT{save.left - SL(116), save.top, save.left - SL(12), save.bottom};
}

void SettingsDialog::SyncControlsFromSettings() {
    const auto& c = working_.click;
    const auto& p = working_.playback;
    SetEditText(editRandomInterval_, FormatDouble4(c.randomIntervalMaxSeconds));
    SetEditText(editPressRelease_, FormatDouble4(c.pressReleaseIntervalSeconds));
    SetEditText(editJitterX_, std::to_wstring(c.jitterX));
    SetEditText(editJitterY_, std::to_wstring(c.jitterY));
    SetEditText(editFixedX_, std::to_wstring(c.fixedX));
    SetEditText(editFixedY_, std::to_wstring(c.fixedY));
    SetEditText(editClickLimit_, std::to_wstring(c.clickCountLimit));
    SetEditText(editPlaybackCount_, std::to_wstring(p.playbackCount));
    SetEditText(editPlaybackMin_, FormatDouble4(p.playbackIntervalMinSeconds));
    SetEditText(editPlaybackMax_, FormatDouble4(p.playbackIntervalMaxSeconds));
    SetEditText(editRecordingCaptureHalfSize_, std::to_wstring(p.recordingClickCaptureHalfSize));
    SetEditText(editHoldThreshold_, FormatHoldThresholdLabel(working_.other.holdThresholdSeconds));

    SetEditText(editApiUrl_, working_.ai.apiUrl);
    SetEditText(editApiKey_, working_.ai.apiKey);
    SetEditText(editModelName_, working_.ai.modelName);
    SetEditText(editTemperature_, std::to_wstring(working_.ai.temperature));
    SetEditText(editMaxTokens_, std::to_wstring(working_.ai.maxTokens));
}

void SettingsDialog::SyncSettingsFromControls() {
    auto& c = working_.click;
    auto& p = working_.playback;
    c.randomIntervalMaxSeconds = std::max(0.0, ToDouble(editRandomInterval_, c.randomIntervalMaxSeconds));
    c.pressReleaseIntervalSeconds = std::max(0.0, ToDouble(editPressRelease_, c.pressReleaseIntervalSeconds));
    c.jitterX = std::max(0, static_cast<int>(ToDouble(editJitterX_, c.jitterX)));
    c.jitterY = std::max(0, static_cast<int>(ToDouble(editJitterY_, c.jitterY)));
    c.fixedX = static_cast<int>(ToDouble(editFixedX_, c.fixedX));
    c.fixedY = static_cast<int>(ToDouble(editFixedY_, c.fixedY));
    c.clickCountLimit = std::max(0, static_cast<int>(ToDouble(editClickLimit_, c.clickCountLimit)));
    p.playbackCount = std::max(0, static_cast<int>(ToDouble(editPlaybackCount_, p.playbackCount)));
    p.playbackIntervalMinSeconds = std::max(0.0, ToDouble(editPlaybackMin_, p.playbackIntervalMinSeconds));
    p.playbackIntervalMaxSeconds = std::max(p.playbackIntervalMinSeconds,
        ToDouble(editPlaybackMax_, p.playbackIntervalMaxSeconds));
    p.recordingClickCaptureHalfSize = ClampClickCaptureHalfSize(
        static_cast<int>(ToDouble(editRecordingCaptureHalfSize_, p.recordingClickCaptureHalfSize)));
    p.enableHidDriverSimulation =
        p.foregroundInputBackend != quickscript::ForegroundInputBackend::Software;

    {
        const double hold = ToDouble(editHoldThreshold_, working_.other.holdThresholdSeconds);
        working_.other.holdThresholdSeconds = NormalizeHoldThresholdSeconds(hold);
    }

    working_.ai.apiUrl = GetText(editApiUrl_);
    working_.ai.apiKey = GetText(editApiKey_);
    working_.ai.modelName = GetText(editModelName_);
    working_.ai.temperature = ToDouble(editTemperature_, 0.3);
    working_.ai.maxTokens = ToInt(editMaxTokens_, 4096);
    working_.ai.maxTokens = std::clamp(working_.ai.maxTokens, 1, 393216);

    const int themeSel = themeCombo_.SelectedIndex();
    if (themeSel == quickscript::kCustomThemeComboIndex) {
        working_.other.useCustomTheme = true;
    } else if (themeSel > 0 && themeSel <= quickscript::kThemeCount) {
        working_.other.useCustomTheme = false;
        working_.other.themeId = themeSel - 1;
    }
}

void SettingsDialog::UpdateControlVisibility() {
    const bool clickTab = activeTab_ == Tab::Click;
    const bool playbackTab = activeTab_ == Tab::Playback;
    const bool otherTab = activeTab_ == Tab::Other;
    const bool aiTab = activeTab_ == Tab::AiApi;
    const bool parentVisible = IsWindowVisible(hwnd_) != FALSE;

    if (parentVisible) SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);

    const HWND clickEdits[] = {editRandomInterval_, editPressRelease_, editJitterX_, editJitterY_,
        editFixedX_, editFixedY_, editClickLimit_, crosshairBtn_};
    for (HWND e : clickEdits) SetChildVisibleNoRedraw(e, clickTab);
    const HWND playbackEdits[] = {editPlaybackCount_, editPlaybackMin_, editPlaybackMax_,
        editRecordingCaptureHalfSize_};
    for (HWND e : playbackEdits) SetChildVisibleNoRedraw(e, playbackTab);
    SetChildVisibleNoRedraw(editHoldThreshold_, otherTab);
    const HWND aiCtrls[] = {editApiUrl_, editApiKey_, editModelName_,
        editTemperature_, editMaxTokens_};
    for (HWND e : aiCtrls) SetChildVisibleNoRedraw(e, aiTab);
    if (!aiTab && aiModelCombo_.IsOpen()) aiModelCombo_.Close();
    if (activeTab_ != Tab::Other && themeCombo_.IsOpen()) themeCombo_.Close();
    if (clickTab || playbackTab || otherTab || aiTab) PositionChildControls();

    if (parentVisible) {
        SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(hwnd_, nullptr, nullptr,
            RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

void SettingsDialog::RestoreDefaults() {
    working_ = quickscript::DefaultAppSettings();
    SyncControlsFromSettings();
    RefreshAiModelCombo();
    RefreshThemeCombo();
    UpdateControlVisibility();
}

void SettingsDialog::SaveAndClose() {
    SyncSettingsFromControls();
    std::wstring warn;
    if (settings_) {
        *settings_ = working_;
        SaveAppSettings(*settings_);
        if (working_.playback.foregroundInputBackend
            == quickscript::ForegroundInputBackend::Interception) {
            std::wstring err;
            if (!HidInterceptionBackend::Instance().ProbeAvailable(&err)) {
                warn = err.empty()
                    ? L"设置已保存，但 Interception 未就绪，回放将回退系统模拟。\n请安装驱动并重启后再试。"
                    : (L"设置已保存，但 Interception 未就绪，回放将回退系统模拟。\n" + err);
            }
        } else if (working_.playback.foregroundInputBackend
            == quickscript::ForegroundInputBackend::VirtualHid) {
            std::wstring err;
            if (!VirtualHidBackend::Instance().ProbeAvailable(&err)) {
                warn = err.empty()
                    ? L"设置已保存，但虚拟 HID 未就绪，回放将回退系统模拟。\n请先点「安装虚拟 HID 驱动」。"
                    : (L"设置已保存，但虚拟 HID 未就绪，回放将回退系统模拟。\n" + err);
            }
        }
    }
    saved_ = true;
    SavedCallback cb = std::move(onSaved_);
    onSaved_ = nullptr;
    auto finish = [this, cb = std::move(cb)]() {
        DestroyWindow(hwnd_);
        if (cb) cb();
    };
    if (!warn.empty()) {
        promptModal_.ShowInfo(warn, std::move(finish));
        return;
    }
    finish();
}

void SettingsDialog::DrawSettingsTab(HDC hdc, const RECT& rc, Tab tab, const wchar_t* text) {
    if (activeTab_ == tab) FillRectColor(hdc, rc, kTabActiveGreen);
    SelectObject(hdc, tabFont_);
    DrawTextIn(hdc, text, rc, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void SettingsDialog::DrawFooter(HDC hdc) {
    FillRectColor(hdc, FooterRect(), kPanel);
    SelectObject(hdc, bodyFont_);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, hoverRestore_ ? kDarkGreen : kMainGreen);
    RECT restoreRc = RestoreLinkRect();
    DrawTextW(hdc, L"恢复所有默认值", -1, &restoreRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    StDrawGreenButton(hdc, bodyFont_, SaveBtnRect(), L"保存", hoverSave_);
    if (activeTab_ == Tab::About) {
        StDrawGreenButton(hdc, bodyFont_, CheckUpgradeBtnRect(), L"检查升级", hoverCheckUpgrade_);
    }
}

void SettingsDialog::PaintClickTab(HDC hdc) {
    const auto& c = working_.click;
    SelectObject(hdc, bodyFont_);
    const bool checks[] = {c.enableRandomInterval, c.enablePressReleaseInterval,
        c.enableCoordinateJitter, c.enableFixedCoordinates, c.enableClickCountLimit};
    const wchar_t* labels[] = {
        L"启用随机间隔时间，最长为", L"启用按下抬起间隔，间隔",
        L"启用坐标随机抖动", L"启用固定坐标", L"启用次数限制，点击",
    };
    for (int i = 0; i < 5; ++i) {
        const RECT cb = ClickCheckboxRect(i);
        StDrawCheckbox(hdc, cb, checks[i]);
        DrawTextIn(hdc, labels[i], RECT{cb.right + SL(10), cb.top, ClientW() - SL(kMargin), cb.bottom},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    const int jitterSubY = ClickRowY(2) + SL(kSubLineOffset);
    const int fixedSubY = ClickRowY(3) + SL(kSubLineOffset);
    UpdateInlineLayout();

    DrawTextIn(hdc, L"秒", RECT{inlineLayout_.randomUnitX, ClickRowY(0), inlineLayout_.randomUnitX + SL(40), ClickRowY(0) + SL(kCheckboxSize)},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"秒(建议0.001~0.05)", RECT{inlineLayout_.pressReleaseUnitX, ClickRowY(1), ClientW() - SL(kMargin), ClickRowY(1) + SL(kCheckboxSize)},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(hdc, L"横坐标抖动(X)", RECT{SL(kIndent), jitterSubY, inlineLayout_.jitterXEditX - SL(kCoordEditGap), jitterSubY + SL(kSubRowH)},
        c.enableCoordinateJitter ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"像素", RECT{inlineLayout_.jitterXEditX + SL(kSmallEditW) + SL(kCoordEditGap), jitterSubY,
        inlineLayout_.jitterXEditX + SL(kSmallEditW) + SL(kCoordEditGap) + SL(44), jitterSubY + SL(kSubRowH)},
        c.enableCoordinateJitter ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"纵坐标抖动(Y)", RECT{SL(kJitterYGroupLeft), jitterSubY, inlineLayout_.jitterYEditX - SL(kCoordEditGap), jitterSubY + SL(kSubRowH)},
        c.enableCoordinateJitter ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"像素", RECT{inlineLayout_.jitterYEditX + SL(kSmallEditW) + SL(kCoordEditGap), jitterSubY,
        inlineLayout_.jitterYEditX + SL(kSmallEditW) + SL(kCoordEditGap) + SL(44), jitterSubY + SL(kSubRowH)},
        c.enableCoordinateJitter ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(hdc, L"横坐标(X)", RECT{SL(kIndent), fixedSubY, inlineLayout_.fixedXEditX - SL(kCoordEditGap), fixedSubY + SL(kSubRowH)},
        c.enableFixedCoordinates ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"像素", RECT{inlineLayout_.fixedXEditX + SL(kSmallEditW) + SL(kCoordEditGap), fixedSubY,
        inlineLayout_.fixedXEditX + SL(kSmallEditW) + SL(kCoordEditGap) + SL(50), fixedSubY + SL(kSubRowH)},
        c.enableFixedCoordinates ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"纵坐标(Y)", RECT{SL(kFixedYGroupLeft), fixedSubY, inlineLayout_.fixedYEditX - SL(kCoordEditGap), fixedSubY + SL(kSubRowH)},
        c.enableFixedCoordinates ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"像素", RECT{inlineLayout_.fixedYEditX + SL(kSmallEditW) + SL(kCoordEditGap), fixedSubY,
        inlineLayout_.fixedYEditX + SL(kSmallEditW) + SL(kCoordEditGap) + SL(50), fixedSubY + SL(kSubRowH)},
        c.enableFixedCoordinates ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(hdc, L"次后，自动停止。", RECT{inlineLayout_.clickLimitSuffixX, ClickRowY(4), ClientW() - SL(kMargin), ClickRowY(4) + SL(kCheckboxSize)},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void SettingsDialog::PaintPlaybackTab(HDC hdc) {
    const auto& p = working_.playback;
    SelectObject(hdc, bodyFont_);
    UpdateInlineLayout();

    for (int i = 0; i < 5; ++i) {
        const RECT cb = PlaybackCheckboxRect(i);
        bool checked = false;
        switch (i) {
        case 0: checked = p.enablePlaybackCount; break;
        case 1: checked = p.enablePlaybackInterval; break;
        case 2: checked = p.enableDebugOutputWindow; break;
        case 3: checked = p.autoOutputKeyFunctionDebug; break;
        case 4: checked = p.recordingClickCaptureEnabled; break;
        }
        StDrawCheckbox(hdc, cb, checked);
    }

    const int y0 = PlaybackRowY(0);
    const int y1 = PlaybackRowY(1);
    const int y2 = PlaybackRowY(2);
    const int y4 = PlaybackRowY(4);
    const int y5 = PlaybackRowY(5);
    const int rowH = SL(kCheckboxSize);
    const int gap = SL(kCoordEditGap);

    // 回放次数：启用文案 + 回放 [N] 次后停止
    {
        const int enableEnd = SL(kLabelAfterCheck) + BodyTextWidth(L"启用回放次数");
        DrawTextIn(hdc, L"启用回放次数",
            RECT{SL(kLabelAfterCheck), y0, enableEnd, y0 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const int replayLeft = enableEnd + gap;
        DrawTextIn(hdc, L"回放",
            RECT{replayLeft, y0, inlineLayout_.playbackCountEditX - gap, y0 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, L"次后停止（0=无限）",
            RECT{inlineLayout_.playbackCountSuffixX, y0, ClientW() - SL(kMargin), y0 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 回放间隔：启用文案 + 最小/最大间隔
    {
        const int enableEnd = SL(kLabelAfterCheck) + BodyTextWidth(L"启用回放间隔");
        DrawTextIn(hdc, L"启用回放间隔",
            RECT{SL(kLabelAfterCheck), y1, enableEnd, y1 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, L"最小间隔",
            RECT{enableEnd + gap, y1, inlineLayout_.playbackMinEditX - gap, y1 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, L"秒",
            RECT{inlineLayout_.playbackMinUnitX, y1, inlineLayout_.playbackMaxLabelX - gap, y1 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, L"最大间隔",
            RECT{inlineLayout_.playbackMaxLabelX, y1, inlineLayout_.playbackMaxEditX - gap, y1 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, L"秒",
            RECT{inlineLayout_.playbackMaxUnitX, y1, ClientW() - SL(kMargin), y1 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    DrawTextIn(hdc, L"启用宏调试信息窗口",
        RECT{SL(kLabelAfterCheck), y2, SL(390), y2 + rowH},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"自动输出关键函数调试信息",
        RECT{SL(438), y2, ClientW() - SL(kMargin), y2 + rowH},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    {
        const int captureLabelEnd =
            SL(kLabelAfterCheck) + BodyTextWidth(L"录制时自动截取点击模板");
        DrawTextIn(hdc, L"录制时自动截取点击模板",
            RECT{SL(kLabelAfterCheck), y4, captureLabelEnd, y4 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const int radiusLeft = captureLabelEnd + SL(24);
        DrawTextIn(hdc, L"模板半径：",
            RECT{radiusLeft, y4, inlineLayout_.recordingCaptureHalfSizeEditX - gap, y4 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, L"默认40≈80×80",
            RECT{inlineLayout_.recordingCaptureHalfSizeHintX, y4, ClientW() - SL(kMargin), y4 + rowH},
            kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    {
        const int labelEnd = SL(kMargin) + BodyTextWidth(L"注入方式");
        DrawTextIn(hdc, L"注入方式",
            RECT{SL(kMargin), y5, labelEnd, y5 + rowH},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const int sel = static_cast<int>(p.foregroundInputBackend);
        for (int i = 0; i < 3; ++i) {
            const RECT rc = BackendOptionRect(i);
            StDrawRadio(hdc,
                RECT{rc.left, rc.top, rc.left + SL(kCheckboxSize), rc.top + SL(kCheckboxSize)},
                sel == i);
            DrawTextIn(hdc, BackendOptionLabel(i),
                RECT{rc.left + SL(kCheckboxSize) + SL(6), rc.top, rc.right, rc.bottom},
                kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        StDrawGreenButton(hdc, bodyFont_, HidInstallBtnRect(), kHidInstallBtnText, hoverHidInstall_);
        StDrawGreenButton(hdc, bodyFont_, VirtualHidInstallBtnRect(),
            kVirtualHidInstallBtnText, hoverVirtualHidInstall_);
    }
}

void SettingsDialog::PaintOtherTab(HDC hdc) {
    const auto& o = working_.other;
    const wchar_t* labels[] = {
        L"启用连点/录制/回放后自动隐藏主窗口", L"启动连点/录制/回放时播放声音",
        L"关闭右下角连点/录制/回放提示", L"点击主界面关闭按钮隐藏到托盘",
        L"开机自动启动", L"中文输入法不触发热键",
    };
    const bool checks[] = {
        o.autoHideMainWindow, o.playSoundOnStart, o.hideBottomRightTip, o.closeToTray,
        o.autoStartOnBoot, o.resolveImeConflict,
    };
    SelectObject(hdc, bodyFont_);
    for (int i = 0; i < 6; ++i) {
        const RECT rc = OtherCheckboxRect(i);
        StDrawCheckbox(hdc, rc, checks[i]);
        DrawTextIn(hdc, labels[i], OtherCheckboxLabelRect(i),
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    DrawTextIn(hdc, L"长按判定", OtherHoldLabelRect(), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"秒", OtherHoldUnitRect(), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(hdc, L"界面主题", OtherThemeLabelRect(), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    themeCombo_.DrawField(hdc, OtherThemeComboRect(), hoverThemeCombo_);
}

void SettingsDialog::PaintAboutTab(HDC hdc) {
    const auto& name = quickscript::AppBranding::AppDisplayName();
    const auto& version = quickscript::AppBranding::Version();
    const auto& tagline = quickscript::AppBranding::Tagline();
    const auto& website = quickscript::AppBranding::WebsiteUrl();
    const auto& contact = quickscript::AppBranding::ContactInfo();
    const auto& qq = quickscript::AppBranding::QqGroup();
    SelectObject(hdc, aboutTitleFont_);
    DrawTextIn(hdc, name, RECT{SL(kMargin) + SL(8), DialogContentTop() + SL(20), SL(kMargin) + SL(200), DialogContentTop() + SL(72)},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, bodyFont_);
    DrawTextIn(hdc, version, RECT{SL(kMargin) + SL(136), DialogContentTop() + SL(36), SL(400), DialogContentTop() + SL(68)},
        kMainGreen, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, smallFont_);
    DrawTextIn(hdc, tagline, RECT{SL(kMargin) + SL(8), DialogContentTop() + SL(76), SL(500), DialogContentTop() + SL(100)},
        kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const RECT infoBox{SL(kMargin), DialogContentTop() + SL(120), ClientW() - SL(kMargin), DialogContentTop() + SL(260)};
    DrawBorderRect(hdc, infoBox, kComboBorderGray);
    SelectObject(hdc, bodyFont_);

    const int rowH = SL(40);
    const int valueLeft = infoBox.left + SL(120);
    const COLORREF linkColor = RGB(0, 102, 204);

    DrawTextIn(hdc, L"官网地址：", RECT{infoBox.left + SL(16), infoBox.top + SL(16), valueLeft, infoBox.top + SL(16) + rowH},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!website.empty()) {
        DrawTextIn(hdc, website, RECT{valueLeft, infoBox.top + SL(16), infoBox.right - SL(16), infoBox.top + SL(16) + rowH},
            linkColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    DrawTextIn(hdc, L"联系方式：", RECT{infoBox.left + SL(16), infoBox.top + SL(16) + rowH, valueLeft, infoBox.top + SL(16) + rowH * 2},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!contact.empty()) {
        DrawTextIn(hdc, contact, RECT{valueLeft, infoBox.top + SL(16) + rowH, infoBox.right - SL(16), infoBox.top + SL(16) + rowH * 2},
            linkColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    DrawTextIn(hdc, L"QQ交流群：", RECT{infoBox.left + SL(16), infoBox.top + SL(16) + rowH * 2, valueLeft, infoBox.top + SL(16) + rowH * 3},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!qq.empty()) {
        DrawTextIn(hdc, qq, RECT{valueLeft, infoBox.top + SL(16) + rowH * 2, infoBox.right - SL(16), infoBox.top + SL(16) + rowH * 3},
            linkColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

void SettingsDialog::PaintAiApiTab(HDC hdc) {
    const auto& ai = working_.ai;
    SelectObject(hdc, bodyFont_);

    const int aiTop = DialogContentTop() + SL(kContentPad);
    const int aiLabelX = SL(kMargin);
    UpdateInlineLayout();

    const RECT aiCb = AiCheckboxRect();
    StDrawCheckbox(hdc, aiCb, ai.enabled);
    DrawTextIn(hdc, L"启用 AI 脚本助手",
        RECT{aiCb.right + SL(10), aiCb.top, AiModelComboRect().left - SL(8), aiCb.bottom},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    StDrawGreenButton(hdc, bodyFont_, AiAddModelBtnRect(), L"添加模型", hoverAiAddModel_);
    StDrawGreenButton(hdc, bodyFont_, AiDeleteModelBtnRect(), L"删除", hoverAiDeleteModel_);
    aiModelCombo_.DrawField(hdc, AiModelComboRect(), hoverAiModelCombo_);

    const auto drawLabel = [&](int row, const wchar_t* label) {
        DrawTextIn(hdc, label,
            RECT{aiLabelX, aiTop + AiRowH() * row, aiLabelX + inlineLayout_.aiLabelW, aiTop + AiRowH() * row + AiRowH()},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };

    drawLabel(1, L"API 地址:");
    drawLabel(2, L"API 密钥:");
    drawLabel(3, L"模型名称:");
    drawLabel(4, L"温度参数:");
    drawLabel(5, L"最大Token:");

    DrawTextIn(hdc, L"(0.0-2.0)",
        RECT{inlineLayout_.aiTempHintX, aiTop + AiRowH() * 4, ClientW() - SL(kMargin), aiTop + AiRowH() * 4 + AiRowH()},
        kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 底部提示
    SelectObject(hdc, smallFont_);
    DrawTextIn(hdc,
        L"请填 OpenAI 兼容地址（Chat Completions）。可填 base_url，如 https://api.openai.com/v1\n"
        L"不支持 Anthropic 原生 /v1/messages；DeepSeek 等 OpenAI 兼容服务同样可用。",
        RECT{SL(kMargin), aiTop + AiRowH() * 6 + SL(4), ClientW() - SL(kMargin), DialogFooterTop(ClientH()) - SL(12)},
        kHint, DT_LEFT | DT_WORDBREAK);
}

void SettingsDialog::Paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{}; GetClientRect(hwnd_, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    RenderBatchScope batch(mem);
    FillRectColor(mem, rc, kWhite);
    FillRectColor(mem, RECT{0, 0, rc.right, SL(kTitleH)}, kNavStripGreen);
    FillRectColor(mem, RECT{0, SL(kTitleH), rc.right, DialogContentTop()}, kNavStripGreen);
    SelectObject(mem, titleFont_);
    DrawTextIn(mem, L"鼠大侠-设置", RECT{SL(16), 0, SL(220), SL(kTitleH)}, kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hoverClose_) ::FillAlphaRect(mem, CloseRect(), RGB(0, 0, 0), kCloseHoverAlpha);
    SelectObject(mem, closeFont_);
    DrawTextIn(mem, L"×", CloseRect(), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawSettingsTab(mem, TabRect(Tab::Click), Tab::Click, L"点击设置");
    DrawSettingsTab(mem, TabRect(Tab::Playback), Tab::Playback, L"宏回放设置");
    DrawSettingsTab(mem, TabRect(Tab::Other), Tab::Other, L"其他设置");
    DrawSettingsTab(mem, TabRect(Tab::AiApi), Tab::AiApi, L"AI助手");
    DrawSettingsTab(mem, TabRect(Tab::About), Tab::About, L"关于");
    switch (activeTab_) {
    case Tab::Click: PaintClickTab(mem); break;
    case Tab::Playback: PaintPlaybackTab(mem); break;
    case Tab::Other: PaintOtherTab(mem); break;
    case Tab::AiApi: PaintAiApiTab(mem); break;
    case Tab::About: PaintAboutTab(mem); break;
    }
    DrawFooter(mem);
    {
        const HWND edits[] = {
            editRandomInterval_, editPressRelease_, editJitterX_, editJitterY_,
            editFixedX_, editFixedY_, editClickLimit_,
            editPlaybackCount_, editPlaybackMin_, editPlaybackMax_, editRecordingCaptureHalfSize_,
            editHoldThreshold_,
            editApiUrl_, editApiKey_, editModelName_, editTemperature_, editMaxTokens_,
        };
        for (HWND edit : edits) DrawEditOuterBorder(mem, hwnd_, edit);
    }
    batch.End();
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

void SettingsDialog::ShowPromptAlert(const std::wstring& message) {
    aiModelCombo_.Close();
    promptModal_.ShowInfo(message);
}
