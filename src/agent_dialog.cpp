// ──────────────────────────────────────────────────────────────────
// agent_dialog.cpp — AI Agent 聊天对话框实现
// Win32 非模态对话框 + RichEdit 消息显示 + 后台线程 API 调用
// ──────────────────────────────────────────────────────────────────

#include "agent_dialog.h"

#include <imm.h>

#include "agent_attachment.h"
#include "agent_conversation_store.h"
#include "agent_core.h"
#include "agent_reference.h"
#include "agent_tools.h"
#include "agent_system_prompt.h"
#include "config.h"
#include "drawing.h"
#include "modern_edit.h"
#include "scheduled_task_ui.h"
#include "taskbar_window.h"
#include "utils.h"

#include <windowsx.h>
#include <commctrl.h>
#include <richedit.h>

#include <algorithm>
#include <string>
#include <shellapi.h>
#include <commdlg.h>
#include <thread>

namespace {

void FillAlphaRect(HDC hdc, RECT rc, COLORREF color, BYTE alpha) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    RECT local{0, 0, w, h};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(mem, &local, brush);
    DeleteObject(brush);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, 0};
    GdiAlphaBlend(hdc, rc.left, rc.top, w, h, mem, 0, 0, w, h, bf);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

std::wstring NowHHMM() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[16]{};
    swprintf_s(buf, L"%02d:%02d", st.wHour, st.wMinute);
    return buf;
}

std::vector<std::wstring> ParseMultiSelectFiles(const wchar_t* buffer) {
    std::vector<std::wstring> out;
    if (!buffer || !buffer[0]) return out;
    const std::wstring root = buffer;
    const wchar_t* p = buffer + root.size() + 1;
    if (!*p) {
        out.push_back(root);
        return out;
    }
    while (*p) {
        std::wstring name = p;
        if (name.find(L":\\") == std::wstring::npos && !root.empty())
            out.push_back(root + L"\\" + name);
        else
            out.push_back(name);
        p += name.size() + 1;
    }
    return out;
}

}  // namespace

AgentDialog::AgentDialog() {
    bgBrush_ = CreateSolidBrush(RGB(255, 255, 255));
}

AgentDialog::~AgentDialog() {
    if (IsAlive()) {
        KillTimer(hwnd_, kThinkingTimerId);
        DestroyWindow(hwnd_);
    }
    if (bgBrush_) { DeleteObject(bgBrush_); bgBrush_ = nullptr; }
}

// ── Show（非模态，可多个实例）──────────────────────────────────────
bool AgentDialog::Show(HWND owner, const quickscript::AiApiSettings& aiSettings,
    const RestoreData* restore, CloseCallback onClose) {
    if (IsAlive()) {
        SetForegroundWindow(hwnd_);
        return true;
    }

    owner_ = owner;
    aiSettings_ = aiSettings;
    savedModels_ = aiSettings.savedModels;
    selectedProfileIndex_ = savedModels_.empty() ? -1 : 0;
    initialized_ = false;
    conversationStarted_ = false;
    layoutUpdating_ = false;
    inputLines_ = 1;
    inputScrollPos_ = 0;
    inputScrollMax_ = 0;
    inputScrollVisible_ = false;
    hoverClose_ = hoverMinimize_ = hoverSend_ = hoverAttach_ = false;
    hoverAttachmentRemove_ = -1;
    thinking_ = false;
    thinkingBlinkPhase_ = 0;
    enterSendIssuedForKey_ = false;
    inputEdit_ = nullptr;
    chatView_ = nullptr;
    currentAssistantMsg_.clear();
    AgentReleaseAttachmentBitmaps(attachments_);
    onClose_ = std::move(onClose);
    restoreData_ = {};
    conversationId_.clear();
    conversationName_.clear();
    createdTime_.clear();
    if (restore) {
        restoreData_ = *restore;
        conversationId_ = restore->id;
        conversationName_ = restore->name;
        createdTime_ = restore->createdTime;
        if (conversationName_.empty() && !restore->messages.empty())
            conversationName_ = SummarizeConversationName(restore->messages);
    }

    static bool registered = false;
    const wchar_t* cls = L"QuickScriptAgentDialog";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &AgentDialog::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = bgBrush_;
        RegisterClassW(&wc);
        registered = true;
    }

    RECT ownerRc{};
    if (owner_ && IsWindow(owner_))
        GetWindowRect(owner_, &ownerRc);

    static int sCascade = 0;
    const int cascade = (sCascade++ % 10) * 28;
    const int x = ownerRc.left + cascade;
    const int y = ownerRc.top + cascade;

    hwnd_ = CreateWindowExW(0, cls, L"",
        WS_POPUP | WS_CLIPCHILDREN,
        x, y, kDialogW, kDialogH, nullptr, nullptr,
        GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    ApplyTaskbarWindowStyle(hwnd_, L"AI 脚本助手");

    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    return true;
}

// ── WndProc ───────────────────────────────────────────────────────
LRESULT CALLBACK AgentDialog::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AgentDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<AgentDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<AgentDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Handle ────────────────────────────────────────────────────────
LRESULT AgentDialog::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        Init();
        return 0;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkColor(hdc, kWhite);
        SetTextColor(hdc, kText);
        static HBRUSH whiteBrush = CreateSolidBrush(kWhite);
        return reinterpret_cast<LRESULT>(whiteBrush);
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (initialized_) {
            UpdateInputHeight();
            PositionControls();
        }
        return 0;

    case WM_SHOWWINDOW:
        if (wp && initialized_) {
            PositionControls();
            PostMessageW(hwnd_, WM_AGENT_DEFER_FOCUS, 0, 0);
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);

    case WM_AGENT_DEFER_FOCUS:
        if (inputEdit_ && IsWindow(inputEdit_)) SetFocus(inputEdit_);
        return 0;

    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == kInputEdit) {
            UpdateInputHeight();
            EnsureInputCaretVisible();
            return 0;
        }
        break;

    case WM_DROPFILES: {
        HandleFileDrop(reinterpret_cast<HDROP>(wp));
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (PtIn(InputRect(), pt.x, pt.y) || HitInputScrollTrack(pt.x, pt.y)) {
            HandleInputMouseWheel(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
            return 0;
        }
        break;
    }

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (HitClose(pt.x, pt.y) || HitMinimize(pt.x, pt.y)) return HTCLIENT;
        if (HitTitle(pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_DRAWITEM:
        return DefWindowProcW(hwnd_, msg, wp, lp);

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) modelCombo_.Close();
        return DefWindowProcW(hwnd_, msg, wp, lp);

    case WM_MOVE:
    case WM_WINDOWPOSCHANGED:
        if (modelCombo_.IsOpen()) modelCombo_.SyncPopupPosition(ModelComboRect());
        return DefWindowProcW(hwnd_, msg, wp, lp);

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (HitClose(x, y)) {
            DestroyWindow(hwnd_);
            return 0;
        }
        if (HitMinimize(x, y)) {
            ShowWindow(hwnd_, SW_MINIMIZE);
            return 0;
        }
        const int removeIdx = HitAttachmentRemove(x, y);
        if (removeIdx >= 0) {
            RemoveAttachmentAt(removeIdx);
            return 0;
        }
        if (HitAttach(x, y)) {
            OpenAttachFileDialog();
            return 0;
        }
        if (!savedModels_.empty() && modelCombo_.HitField(ModelComboRect(), x, y)) {
            modelCombo_.Toggle(ModelComboRect());
            return 0;
        }
        if (HitSend(x, y)) {
            if (thinking_) CancelRequest();
            else SendUserMessage();
            return 0;
        }
        if (inputScrollVisible_ && HitInputScrollThumb(x, y)) {
            inputScrollbarDragging_ = true;
            inputScrollbarDragOffset_ = y - InputScrollThumbRect().top;
            SetCapture(hwnd_);
            return 0;
        }
        if (inputScrollVisible_ && HitInputScrollTrack(x, y)) {
            inputScrollbarDragging_ = true;
            const RECT thumb = InputScrollThumbRect();
            inputScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateInputScrollFromThumb(y - inputScrollbarDragOffset_);
            SetCapture(hwnd_);
            return 0;
        }
        if (modelCombo_.IsOpen()) {
            POINT screenPt{x, y};
            ClientToScreen(hwnd_, &screenPt);
            if (!modelCombo_.HitPopupScreen(screenPt.x, screenPt.y)
                && !( !savedModels_.empty() && modelCombo_.HitField(ModelComboRect(), x, y))) {
                modelCombo_.Close();
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (inputScrollbarDragging_) {
            UpdateInputScrollFromThumb(y - inputScrollbarDragOffset_);
            return 0;
        }
        bool needRedraw = false;
        if (HitClose(x, y) != hoverClose_) { hoverClose_ = HitClose(x, y); needRedraw = true; }
        if (HitMinimize(x, y) != hoverMinimize_) { hoverMinimize_ = HitMinimize(x, y); needRedraw = true; }
        if (HitSend(x, y) != hoverSend_) { hoverSend_ = HitSend(x, y); needRedraw = true; }
        if (HitAttach(x, y) != hoverAttach_) { hoverAttach_ = HitAttach(x, y); needRedraw = true; }
        const bool hm = !savedModels_.empty() && modelCombo_.HitField(ModelComboRect(), x, y);
        if (hm != hoverModelCombo_) { hoverModelCombo_ = hm; needRedraw = true; }
        const int removeIdx = HitAttachmentRemove(x, y);
        if (removeIdx != hoverAttachmentRemove_) {
            hoverAttachmentRemove_ = removeIdx;
            needRedraw = true;
        }
        if (needRedraw) InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
        if (inputScrollbarDragging_) {
            inputScrollbarDragging_ = false;
            ReleaseCapture();
            return 0;
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (inputScrollbarDragging_) inputScrollbarDragging_ = false;
        return 0;

    // ── Agent 回调消息 ────────────────────────────────────────────
    case WM_AGENT_RESPONSE: {
        auto* pText = reinterpret_cast<std::wstring*>(lp);
        if (pText) {
            OnAssistantResponse(*pText);
            delete pText;
        }
        return 0;
    }

    case WM_AGENT_TOOLCALL: {
        auto* pData = reinterpret_cast<std::pair<std::wstring,std::wstring>*>(lp);
        if (pData) {
            OnToolCall(pData->first, pData->second);
            delete pData;
        }
        return 0;
    }

    case WM_AGENT_ERROR: {
        auto* pText = reinterpret_cast<std::wstring*>(lp);
        if (pText) {
            OnError(*pText);
            delete pText;
        }
        return 0;
    }

    case WM_AGENT_REASONING: {
        auto* pText = reinterpret_cast<std::wstring*>(lp);
        if (pText) {
            OnReasoning(*pText);
            delete pText;
        }
        return 0;
    }

    case WM_AGENT_REASONING_DELTA: {
        auto* pText = reinterpret_cast<std::wstring*>(lp);
        if (pText) {
            OnReasoningDelta(*pText);
            delete pText;
        }
        return 0;
    }

    case WM_AGENT_CONTENT_DELTA: {
        auto* pText = reinterpret_cast<std::wstring*>(lp);
        if (pText) {
            OnContentDelta(*pText);
            delete pText;
        }
        return 0;
    }

    case WM_AGENT_STATUS: {
        auto* pText = reinterpret_cast<std::wstring*>(lp);
        if (pText) {
            OnStatus(*pText);
            delete pText;
        }
        return 0;
    }

    case WM_PAINT:
        Paint();
        return 0;

    case WM_TIMER:
        if (wp == kThinkingTimerId && thinking_) {
            thinkingBlinkPhase_ = (thinkingBlinkPhase_ + 1) % 4;
            const RECT thinkRc = ThinkingStatusRect();
            InvalidateRect(hwnd_, &thinkRc, FALSE);
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd_, kThinkingTimerId);
        Cleanup();
        hwnd_ = nullptr;
        return 0;

    case WM_IME_SETCONTEXT:
        if (wp) lp |= 0x01000000 | 0x80000000;  // ISC_SHOWUICANDIDATEWINDOW | ISC_SHOWUICOMPOSITIONWINDOW
        return DefWindowProcW(hwnd_, msg, wp, lp);

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ── 输入框 Enter 发送 ─────────────────────────────────────────────
namespace {

bool EditImeHasCompositionText(HWND hwnd) {
    HIMC imc = ImmGetContext(hwnd);
    if (!imc) return false;
    const LONG bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(hwnd, imc);
    return bytes > 0;
}

}  // namespace

LRESULT CALLBACK AgentDialog::InputSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR refData) {
    auto* self = reinterpret_cast<AgentDialog*>(refData);
    if (!self) return DefSubclassProc(hwnd, msg, wp, lp);

    if (ModernEditHandleShortcutMessage(hwnd, msg, wp, lp)) return 0;

    if (msg == WM_KEYDOWN && wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (self->TryPasteAttachmentsFromClipboard()) return 0;
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_PASTE) {
        if (self->TryPasteAttachmentsFromClipboard()) return 0;
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_DROPFILES) {
        self->HandleFileDrop(reinterpret_cast<HDROP>(wp));
        return 0;
    }
    if (msg == WM_CHAR && wp == L'\r') {
        if (GetKeyState(VK_SHIFT) & 0x8000) return DefSubclassProc(hwnd, msg, wp, lp);
        if (EditImeHasCompositionText(hwnd)) return DefSubclassProc(hwnd, msg, wp, lp);
        if (!self->thinking_) self->SendUserMessage();
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (GetKeyState(VK_SHIFT) & 0x8000) return DefSubclassProc(hwnd, msg, wp, lp);
        if (EditImeHasCompositionText(hwnd)) return DefSubclassProc(hwnd, msg, wp, lp);
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        self->HandleInputMouseWheel(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK AgentDialog::ChatViewSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR refData) {
    auto* self = reinterpret_cast<AgentDialog*>(refData);
    if (self && msg == WM_DROPFILES) {
        self->HandleFileDrop(reinterpret_cast<HDROP>(wp));
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ── Init ──────────────────────────────────────────────────────────
void AgentDialog::Init() {
    font_ = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    closeFont_ = CreateFontW(36, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    chatFont_ = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    smallFont_ = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");

    LoadLibraryW(L"Msftedit.dll");
    DragAcceptFiles(hwnd_, TRUE);

    // 创建消息显示区（RichEdit，只读）
    RECT msgRc = MessagesRect();
    const int msgW = std::max(1, static_cast<int>(msgRc.right - msgRc.left));
    const int msgH = std::max(1, static_cast<int>(msgRc.bottom - msgRc.top));
    chatView_ = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
        msgRc.left, msgRc.top, msgW, msgH,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChatView)),
        GetModuleHandleW(nullptr), nullptr);
    if (!chatView_) return;

    SendMessageW(chatView_, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
    SendMessageW(chatView_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(kHorizPad, kHorizPad));
    DragAcceptFiles(chatView_, TRUE);
    SetWindowSubclass(chatView_, ChatViewSubclassProc, 8803, reinterpret_cast<DWORD_PTR>(this));

    // 创建输入框（标准 EDIT + IME 友好子类）
    inputEdit_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 100, 40,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInputEdit)),
        GetModuleHandleW(nullptr), nullptr);
    if (!inputEdit_) return;

    SendMessageW(inputEdit_, EM_SETLIMITTEXT, kModernEditLimitAgentInput, 0);
    SendMessageW(inputEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
    DragAcceptFiles(inputEdit_, TRUE);
    SetWindowSubclass(inputEdit_, InputSubclassProc, 8802, reinterpret_cast<DWORD_PTR>(this));

    // 设置字体
    SendMessageW(chatView_, WM_SETFONT, reinterpret_cast<WPARAM>(chatFont_), TRUE);
    SendMessageW(inputEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

    modelCombo_.Init(hwnd_, font_);
    modelCombo_.SetPlaceholder(L"请选择模型");
    modelCombo_.SetSelectionCallback([this](int index) {
        selectedProfileIndex_ = index;
        ApplySelectedModelProfile();
        RebuildAgentCore();
    });

    ApplySelectedModelProfile();
    RefreshModelCombo();
    PositionControls();
    RebuildAgentCore();

    initialized_ = true;
    aliveFlag_ = std::make_shared<std::atomic<bool>>(true);
    PositionControls();
    SetWelcomeMessage();
    if (!restoreData_.messages.empty() && agent_) {
        agent_->SetFullHistory(restoreData_.messages);
        if (chatView_ && !restoreData_.chatDisplay.empty()) {
            SetWindowTextW(chatView_, restoreData_.chatDisplay.c_str());
            CHARFORMAT2W cf{};
            cf.cbSize = sizeof(cf);
            cf.dwMask = CFM_COLOR;
            cf.crTextColor = kText;
            SendMessageW(chatView_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
        }
        conversationStarted_ = true;
    }
    outerShadow_.Attach(hwnd_);
}

void AgentDialog::ApplySelectedModelProfile() {
    if (selectedProfileIndex_ >= 0
        && selectedProfileIndex_ < static_cast<int>(savedModels_.size())) {
        const auto& profile = savedModels_[selectedProfileIndex_];
        aiSettings_.apiUrl = profile.apiUrl;
        aiSettings_.apiKey = profile.apiKey;
        aiSettings_.modelName = profile.modelName;
        aiSettings_.temperature = profile.temperature;
        aiSettings_.maxTokens = profile.maxTokens;
    }
}

void AgentDialog::RefreshModelCombo() {
    std::vector<std::wstring> names;
    names.reserve(savedModels_.size());
    for (const auto& model : savedModels_)
        names.push_back(model.modelName);
    modelCombo_.SetItems(std::move(names));
    if (savedModels_.empty()) {
        modelCombo_.SetSelectedIndex(-1);
        return;
    }
    if (selectedProfileIndex_ < 0 || selectedProfileIndex_ >= static_cast<int>(savedModels_.size()))
        selectedProfileIndex_ = 0;
    modelCombo_.SetSelectedIndex(selectedProfileIndex_);
}

void AgentDialog::RebuildAgentCore() {
    AgentConfig cfg;
    cfg.apiUrl = aiSettings_.apiUrl;
    cfg.apiKey = aiSettings_.apiKey;
    cfg.model = aiSettings_.modelName;
    cfg.temperature = aiSettings_.temperature;
    cfg.maxTokens = aiSettings_.maxTokens;
    if (cfg.maxTokens > 393216) cfg.maxTokens = 393216;
    if (cfg.maxTokens < 1) cfg.maxTokens = 4096;
    const int scaledTimeout = cfg.maxTokens > 0 ? cfg.maxTokens * 25 : 0;
    cfg.recvTimeoutMs = std::max({120000, 180000, scaledTimeout});
    const std::wstring systemPrompt = BuildAgentSystemPrompt(cfg.model, cfg.apiUrl);
    const std::vector<AgentTool> tools = BuildDefaultAgentTools();

    if (agent_) {
        agent_->UpdateConfig(cfg, systemPrompt);
        agent_->UpdateTools(tools);
        return;
    }

    agent_ = std::make_shared<AgentCore>(cfg, systemPrompt, tools);
}

void AgentDialog::SetWelcomeMessage() {
    if (!chatView_) return;
    const wchar_t* welcome =
        L"AI 脚本助手已就绪。请描述你想要创建的自动化脚本，或提供已有的脚本让我帮你优化。";
    SetWindowTextW(chatView_, welcome);
    SendMessageW(chatView_, EM_SETSEL, 0, -1);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = kHint;
    SendMessageW(chatView_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    SendMessageW(chatView_, EM_SETSEL, 0, 0);
    conversationStarted_ = false;
}

void AgentDialog::ClearWelcomeIfNeeded() {
    if (!conversationStarted_ && chatView_) {
        SetWindowTextW(chatView_, L"");
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = kText;
        SendMessageW(chatView_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
        conversationStarted_ = true;
    }
}

std::wstring AgentDialog::ValidateApiConfig() const {
    if (!aiSettings_.enabled)
        return L"请先在「设置 → AI助手」中启用 AI 脚本助手。";
    if (Trim(aiSettings_.apiUrl).empty())
        return L"请先在「设置 → AI助手」中填写 API 地址。";
    if (Trim(aiSettings_.apiKey).empty())
        return L"请先在「设置 → AI助手」中填写 API 密钥。";
    if (Trim(aiSettings_.modelName).empty())
        return L"请先在「设置 → AI助手」中填写模型名称。";
    const std::wstring url = Trim(aiSettings_.apiUrl);
    if (url.find(L"://") != url.rfind(L"://"))
        return L"API 地址格式异常（内容重复），请在「设置 → AI助手」中重新填写 API 地址。";
    if (url.find(L"anthropic.com") != std::wstring::npos && url.find(L"chat/completions") == std::wstring::npos)
        return L"Anthropic 原生 API 与当前助手不兼容。请填写 OpenAI 兼容的 Chat Completions 地址，或使用兼容中转服务。";
    if (Trim(aiSettings_.apiKey).size() > 8) {
        const std::wstring key = Trim(aiSettings_.apiKey);
        const size_t half = key.size() / 2;
        if (half > 0 && key.substr(0, half) == key.substr(half))
            return L"API 密钥格式异常（内容重复），请在「设置 → AI助手」中重新填写 API 密钥。";
    }
    return L"";
}

int AgentDialog::CountInputLines() const {
    if (!inputEdit_) return 1;
    const int len = GetWindowTextLengthW(inputEdit_);
    if (len <= 0) return 1;

    const int editLines = static_cast<int>(SendMessageW(inputEdit_, EM_GETLINECOUNT, 0, 0));

    std::wstring text(static_cast<size_t>(len), L'\0');
    GetWindowTextW(inputEdit_, text.data(), len + 1);
    int breakLines = 1;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            ++breakLines;
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else if (text[i] == L'\n') {
            ++breakLines;
        }
    }

    return std::max({1, editLines, breakLines});
}

void AgentDialog::UpdateInputScrollMetrics() {
    if (!inputEdit_) return;
    const int contentLines = CountInputLines();
    const int visibleLines = VisibleInputLines();
    inputScrollVisible_ = contentLines > visibleLines;
    inputScrollMax_ = std::max(0, contentLines - visibleLines);
    inputScrollPos_ = std::clamp(inputScrollPos_, 0, inputScrollMax_);
    SyncInputEditScroll();
}

void AgentDialog::HandleInputMouseWheel(int delta) {
    const bool hadScroll = inputScrollVisible_;
    UpdateInputScrollMetrics();
    if (hadScroll != inputScrollVisible_) {
        PositionControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (!inputScrollVisible_) return;
    SetInputScrollPos(inputScrollPos_ - delta);
}

int AgentDialog::VisibleInputLines() const {
    const RECT rc = InputRect();
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
    return std::max(1, h / kInputLineH);
}

void AgentDialog::SetInputScrollPos(int pos) {
    inputScrollPos_ = std::clamp(pos, 0, inputScrollMax_);
    SyncInputEditScroll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AgentDialog::SyncInputEditScroll() {
    if (!inputEdit_) return;
    const int first = static_cast<int>(SendMessageW(inputEdit_, EM_GETFIRSTVISIBLELINE, 0, 0));
    if (first != inputScrollPos_) {
        SendMessageW(inputEdit_, EM_LINESCROLL, 0, inputScrollPos_ - first);
    }
}

void AgentDialog::UpdateInputScrollFromThumb(int thumbTop) {
    const RECT track = InputScrollTrackRect();
    const int trackH = track.bottom - track.top;
    const int contentLines = std::max(1, CountInputLines());
    const int visibleLines = VisibleInputLines();
    const int thumbH = std::max(24, trackH * visibleLines / contentLines);
    const int range = std::max(1, trackH - thumbH);
    const int offset = std::clamp(static_cast<int>(thumbTop - track.top), 0, range);
    SetInputScrollPos(inputScrollMax_ > 0 ? offset * inputScrollMax_ / range : 0);
}

void AgentDialog::EnsureInputCaretVisible() {
    if (!inputEdit_) return;
    DWORD selStart = 0, selEnd = 0;
    SendMessageW(inputEdit_, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart),
        reinterpret_cast<LPARAM>(&selEnd));
    const int caretLine = static_cast<int>(SendMessageW(inputEdit_, EM_LINEFROMCHAR, selEnd, 0));
    const int visibleLines = VisibleInputLines();
    if (caretLine < inputScrollPos_) {
        SetInputScrollPos(caretLine);
    } else if (caretLine >= inputScrollPos_ + visibleLines) {
        SetInputScrollPos(caretLine - visibleLines + 1);
    }
}

// ── PositionControls ──────────────────────────────────────────────
void AgentDialog::UpdateInputHeight() {
    if (!inputEdit_) return;
    int lines = CountInputLines();
    lines = std::min(lines, kInputMaxLines);
    const bool hadScroll = inputScrollVisible_;
    if (lines == inputLines_) {
        UpdateInputScrollMetrics();
        EnsureInputCaretVisible();
        if (hadScroll != inputScrollVisible_) {
            PositionControls();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    inputLines_ = lines;
    UpdateInputScrollMetrics();
    EnsureInputCaretVisible();
    PositionControls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AgentDialog::PositionControls() {
    if (layoutUpdating_) return;
    layoutUpdating_ = true;

    RECT msgRc = MessagesRect();
    const int msgW = std::max(1, static_cast<int>(msgRc.right - msgRc.left));
    const int msgH = std::max(1, static_cast<int>(msgRc.bottom - msgRc.top));
    if (chatView_) SetWindowPos(chatView_, nullptr,
        msgRc.left, msgRc.top, msgW, msgH,
        SWP_NOZORDER | SWP_NOACTIVATE);

    RECT inRc = InputRect();
    const int inW = std::max(1, static_cast<int>(inRc.right - inRc.left));
    const int inH = std::max(1, static_cast<int>(inRc.bottom - inRc.top));
    if (inputEdit_) SetWindowPos(inputEdit_, nullptr,
        inRc.left, inRc.top, inW, inH,
        SWP_NOZORDER | SWP_NOACTIVATE);

    layoutUpdating_ = false;
}

int AgentDialog::InputFrameHeight() const {
    int h = kFrameInnerPad;
    if (!attachments_.empty()) h += kAttachmentBarH + 4;
    h += kInputPadV + inputLines_ * kInputLineH + kInputPadV;
    h += std::max(kSendBtnH, kAttachBtnH) + kFrameInnerPad;
    return h;
}

// ── Paint ─────────────────────────────────────────────────────────
void AgentDialog::Paint() {
    if (!font_ || !closeFont_) return;
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{}; GetClientRect(hwnd_, &rc);
    const int w = ClientW();
    const int h = ClientH();

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // 背景
    FillRectColor(mem, rc, kWhite);

    // 标题栏（与主界面一致，仅保留一行）
    const RECT closeRc = CloseRect();
    const RECT minRc = MinimizeRect();
    FillRectColor(mem, RECT{0, 0, w, kTitleBarH}, kMainGreen);
    SelectObject(mem, font_);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, kWhite);
    RECT titleRc{kHorizPad + 20, 0, w - kCloseBtnW - kTitleBtnW, kTitleBarH};
    DrawTextW(mem, L"AI 脚本助手", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hoverMinimize_) FillAlphaRect(mem, minRc, RGB(90, 190, 125), kCloseHoverAlpha);
    if (hoverClose_) FillAlphaRect(mem, closeRc, RGB(90, 190, 125), kCloseHoverAlpha);
    SelectObject(mem, closeFont_);
    DrawTextW(mem, L"−", -1, const_cast<RECT*>(&minRc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(mem, L"×", -1, const_cast<RECT*>(&closeRc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 输入区分割线与背景
    const RECT inputFrame = InputFrameRect();
    HPEN linePen = CreatePen(PS_SOLID, 1, kComboBorderGray);
    HGDIOBJ oldPen = SelectObject(mem, linePen);
    MoveToEx(mem, 0, inputFrame.top - 1, nullptr);
    LineTo(mem, w, inputFrame.top - 1);
    SelectObject(mem, oldPen);
    DeleteObject(linePen);
    FillRectColor(mem, RECT{0, inputFrame.top, w, h}, kWhite);
    DrawModernEditBorder(mem, inputFrame);

    DrawAttachmentBar(mem);
    DrawInputScrollbar(mem);

    const RECT attachRc = AttachBtnRect();
    FillRectColor(mem, attachRc, hoverAttach_ ? RGB(235, 235, 235) : RGB(248, 248, 248));
    HPEN attachPen = CreatePen(PS_SOLID, 1, kComboBorderGray);
    oldPen = SelectObject(mem, attachPen);
    Rectangle(mem, attachRc.left, attachRc.top, attachRc.right, attachRc.bottom);
    SelectObject(mem, oldPen);
    DeleteObject(attachPen);
    SelectObject(mem, font_);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, kText);
    DrawTextW(mem, L"+", -1, const_cast<RECT*>(&attachRc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (!savedModels_.empty())
        modelCombo_.DrawField(mem, ModelComboRect(), hoverModelCombo_);

    const RECT sendRc = SendBtnRect();
  const wchar_t* sendLabel = thinking_ ? L"取消" : L"发送";
  StDrawGreenButton(mem, font_, sendRc, sendLabel, hoverSend_, true);

    if (thinking_) {
        SelectObject(mem, chatFont_);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, kMainGreen);
        const std::wstring label = ThinkingStatusText();
        RECT thinkRc = ThinkingStatusRect();
        DrawTextW(mem, label.c_str(), -1, &thinkRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

// ── Cleanup ───────────────────────────────────────────────────────
void AgentDialog::TryExportConversation() {
    if (!onClose_) return;
    AgentConversationSavePayload payload;
    if (agent_) {
        const auto& hist = agent_->GetHistory();
        payload.roundCount = CountConversationRounds(hist);
        payload.shouldSave = payload.roundCount > 0;
        if (payload.shouldSave) {
            payload.messages = hist;
            payload.id = conversationId_.empty() ? TimestampName() : conversationId_;
            payload.createdTime = createdTime_.empty() ? NowText() : createdTime_;
            payload.name = conversationName_.empty()
                ? SummarizeConversationName(hist) : conversationName_;
            if (chatView_) payload.chatDisplay = GetText(chatView_);
        }
    }
    onClose_(std::move(payload));
}

void AgentDialog::Cleanup() {
    TryExportConversation();
    onClose_ = nullptr;
    if (aliveFlag_) *aliveFlag_ = false;
    if (cancelRequested_) cancelRequested_->store(true);
    httpAbort_.Abort();
    if (agent_) agent_->AbortActiveHttp();
    KillTimer(hwnd_, kThinkingTimerId);
    AgentReleaseAttachmentBitmaps(attachments_);
    inputEdit_ = nullptr;
    chatView_ = nullptr;
    modelCombo_.Destroy();
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    if (closeFont_) { DeleteObject(closeFont_); closeFont_ = nullptr; }
    if (chatFont_) { DeleteObject(chatFont_); chatFont_ = nullptr; }
    if (smallFont_) { DeleteObject(smallFont_); smallFont_ = nullptr; }
    agent_.reset();
    initialized_ = false;
}

int AgentDialog::ClientW() const {
    RECT rc{}; GetClientRect(hwnd_, &rc);
    const int w = static_cast<int>(rc.right - rc.left);
    return w > 0 ? w : kDialogW;
}

int AgentDialog::ClientH() const {
    RECT rc{}; GetClientRect(hwnd_, &rc);
    const int h = static_cast<int>(rc.bottom - rc.top);
    return h > 0 ? h : kDialogH;
}

// ── Hit testing ───────────────────────────────────────────────────
bool AgentDialog::PtIn(const RECT& rc, int x, int y) const {
    return StPtIn(rc, x, y);
}

bool AgentDialog::HitClose(int x, int y) const {
    return PtIn(CloseRect(), x, y);
}

bool AgentDialog::HitMinimize(int x, int y) const {
    return PtIn(MinimizeRect(), x, y);
}

bool AgentDialog::HitTitle(int x, int y) const {
    return y >= 0 && y < kTitleBarH && x < MinimizeRect().left;
}

bool AgentDialog::HitSend(int x, int y) const {
    return PtIn(SendBtnRect(), x, y);
}

bool AgentDialog::HitAttach(int x, int y) const {
    return PtIn(AttachBtnRect(), x, y);
}

// ── Layout rects ──────────────────────────────────────────────────
RECT AgentDialog::MessagesRect() const {
    const int clientW = ClientW();
    const int clientH = ClientH();
    const int frameTop = clientH - kInputBottomPad - InputFrameHeight();
    const int bottom = std::max(kTitleBarH + 8, frameTop - kInputGap);
    return RECT{kHorizPad, kTitleBarH + 4, clientW - kHorizPad, bottom};
}

RECT AgentDialog::InputFrameRect() const {
    const int clientW = ClientW();
    const int clientH = ClientH();
    const int h = InputFrameHeight();
    return RECT{kHorizPad, clientH - kInputBottomPad - h, clientW - kHorizPad,
        clientH - kInputBottomPad};
}

RECT AgentDialog::AttachmentBarRect() const {
    const RECT frame = InputFrameRect();
    if (attachments_.empty()) return RECT{};
    return RECT{frame.left + kFrameInnerPad, frame.top + kFrameInnerPad,
        frame.right - kFrameInnerPad, frame.top + kFrameInnerPad + kAttachmentBarH};
}

RECT AgentDialog::AttachmentChipRect(int index) const {
    const RECT bar = AttachmentBarRect();
    const int x = bar.left + index * (kAttachmentChipW + kAttachmentChipGap);
    return RECT{x, bar.top + 4, x + kAttachmentChipW, bar.bottom - 4};
}

RECT AgentDialog::InputRect() const {
    const RECT frame = InputFrameRect();
    int top = frame.top + kFrameInnerPad;
    if (!attachments_.empty()) top += kAttachmentBarH + 4;
    const int bottom = frame.bottom - kFrameInnerPad - std::max(kSendBtnH, kAttachBtnH) - 4;
    const int left = frame.left + kFrameInnerPad;
    int right = frame.right - kFrameInnerPad;
    if (inputScrollVisible_) right -= kInputScrollW + 4;
    return RECT{left, top, right, bottom};
}

RECT AgentDialog::AttachBtnRect() const {
    const RECT frame = InputFrameRect();
    const int top = frame.bottom - kFrameInnerPad - kAttachBtnH;
    return RECT{frame.left + kFrameInnerPad, top,
        frame.left + kFrameInnerPad + kAttachBtnW, top + kAttachBtnH};
}

RECT AgentDialog::ModelComboRect() const {
    const RECT attach = AttachBtnRect();
    return RECT{attach.right + kModelComboGap, attach.top,
        attach.right + kModelComboGap + kModelComboW, attach.bottom};
}

RECT AgentDialog::SendBtnRect() const {
    const RECT frame = InputFrameRect();
    const int top = frame.bottom - kFrameInnerPad - kSendBtnH;
    return RECT{frame.right - kFrameInnerPad - kSendBtnW, top,
        frame.right - kFrameInnerPad, top + kSendBtnH};
}

RECT AgentDialog::InputScrollTrackRect() const {
    const RECT input = InputRect();
    return RECT{input.right + 2, input.top, input.right + 2 + kInputScrollW, input.bottom};
}

RECT AgentDialog::InputScrollThumbRect() const {
    const RECT track = InputScrollTrackRect();
    if (inputScrollMax_ <= 0) return track;
    const int trackH = std::max(1, static_cast<int>(track.bottom - track.top));
    const int contentLines = std::max(1, CountInputLines());
    const int visibleLines = VisibleInputLines();
    const int thumbH = std::max(24, trackH * visibleLines / contentLines);
    const int range = std::max(1, trackH - thumbH);
    const int top = track.top + range * inputScrollPos_ / std::max(1, inputScrollMax_);
    return RECT{track.left, top, track.right, top + thumbH};
}

bool AgentDialog::HitInputScrollThumb(int x, int y) const {
    if (!inputScrollVisible_) return false;
    return PtIn(InputScrollThumbRect(), x, y);
}

bool AgentDialog::HitInputScrollTrack(int x, int y) const {
    if (!inputScrollVisible_) return false;
    return PtIn(InputScrollTrackRect(), x, y);
}

RECT AgentDialog::CloseRect() const {
    const int clientW = ClientW();
    return RECT{clientW - kCloseBtnW, 0, clientW, kTitleBarH};
}

RECT AgentDialog::MinimizeRect() const {
    const RECT close = CloseRect();
    return RECT{close.left - kTitleBtnW, 0, close.left, kTitleBarH};
}

// ── Chat view helpers ─────────────────────────────────────────────
std::wstring AgentDialog::FormatPlainChat(const std::wstring& text) const {
    std::wstring out = text;
    auto eraseToken = [&](const wchar_t* tok) {
        const size_t n = wcslen(tok);
        for (size_t p = 0; (p = out.find(tok, p)) != std::wstring::npos; )
            out.erase(p, n);
    };
    eraseToken(L"**");
    eraseToken(L"__");
    eraseToken(L"```");
    eraseToken(L"`");
    return out;
}

std::wstring AgentDialog::ToolSkillLabel(const std::wstring& name) const {
    if (name == L"readScriptReference") return L"查阅脚本参考";
    if (name == L"readAgentSkill") return L"查阅助手Skill";
    if (name == L"listAiModels") return L"查看已添加AI模型";
    if (name == L"buildScriptActions") return L"构建脚本动作";
    if (name == L"createMacroScript") return L"创建鼠标宏";
    if (name == L"optimizeRecording") return L"优化键鼠录制";
    if (name == L"deleteScriptFile") return L"删除脚本或录制";
    if (name == L"listScripts") return L"浏览脚本与录制列表";
    if (name == L"readScript") return L"读取脚本内容";
    if (name == L"writeScript") return L"保存脚本";
    if (name == L"getScriptStats") return L"分析脚本结构";
    if (name == L"optimizeScript") return L"优化脚本";
    if (name == L"listScheduledTasks") return L"查看定时任务";
    if (name == L"createScheduledTask") return L"创建定时任务";
    if (name == L"updateScheduledTask") return L"修改定时任务";
    if (name == L"deleteScheduledTask") return L"删除定时任务";
    if (name == L"listSettings") return L"查看当前设置";
    if (name == L"updateSettings") return L"修改设置";
    return name;
}

void AgentDialog::AppendStyledText(const std::wstring& text, COLORREF color) {
    if (!chatView_ || text.empty()) return;
    const int len = GetWindowTextLengthW(chatView_);
    SendMessageW(chatView_, EM_SETSEL, len, len);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = color;
    SendMessageW(chatView_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    SendMessageW(chatView_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    ScrollToBottom();
}

void AgentDialog::BeginThinkingBlock() {
    thinkingBlockOpen_ = true;
    thinkingHasContent_ = false;
    statusLinePending_ = false;
    thinkingStatusStart_ = -1;
    thinkingStatusEnd_ = -1;
    lastStatusText_.clear();
    AppendStyledText(L"[思考] " + NowHHMM() + L"\n", kHint);
}

void AgentDialog::AppendThinkingLine(const std::wstring& line) {
    if (!thinkingBlockOpen_ || line.empty()) return;
    thinkingHasContent_ = true;
    statusLinePending_ = false;
    AppendStyledText(line + L"\n", kHint);
}

void AgentDialog::AppendThinkingDelta(const std::wstring& delta) {
    if (!thinkingBlockOpen_ || delta.empty()) return;
    thinkingHasContent_ = true;
    statusLinePending_ = false;
    AppendStyledText(delta, kHint);
}

void AgentDialog::EndThinkingBlock() {
    if (!thinkingBlockOpen_) return;
    if (!thinkingHasContent_)
        AppendStyledText(L"（无详细思考内容）\n", kHint);
    AppendStyledText(L"\n", kText);
    thinkingBlockOpen_ = false;
    thinkingHasContent_ = false;
    statusLinePending_ = false;
    thinkingStatusStart_ = -1;
    thinkingStatusEnd_ = -1;
}

void AgentDialog::BeginAssistantStream() {
    if (assistantStreamOpen_) return;
    assistantStreamOpen_ = true;
    AppendStyledText(L"[AI助手] " + NowHHMM() + L"\n", kText);
}

void AgentDialog::AppendAssistantDelta(const std::wstring& delta) {
    if (delta.empty()) return;
    const std::wstring plain = FormatPlainChat(delta);
    if (plain.empty()) return;
    if (!assistantStreamOpen_) BeginAssistantStream();
    AppendStyledText(plain, kText);
}

void AgentDialog::FinalizeAssistantStream() {
    if (assistantStreamOpen_) {
        AppendStyledText(L"\n\n", kText);
        assistantStreamOpen_ = false;
    }
}

void AgentDialog::AddMessage(const std::wstring& role, const std::wstring& content) {
    std::wstring text;
    if (role == L"user") {
        text = L"[你] " + NowHHMM() + L"\n" + content + L"\n\n";
    } else if (role == L"assistant") {
        text = L"[AI助手] " + NowHHMM() + L"\n" + FormatPlainChat(content) + L"\n\n";
    } else if (role == L"tool") {
        return;
    } else {
        text = content + L"\n\n";
    }

    AppendStyledText(text, kText);
}

void AgentDialog::AppendToChatView(const std::wstring& text) {
    // 选择末尾追加
    int len = GetWindowTextLength(chatView_);
    SendMessageW(chatView_, EM_SETSEL, len, len);
    SendMessageW(chatView_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    ScrollToBottom();
}

void AgentDialog::ScrollToBottom() {
    SendMessageW(chatView_, WM_VSCROLL, SB_BOTTOM, 0);
}

void AgentDialog::ShowThinking(bool show) {
    thinking_ = show;
    if (show) {
        thinkingBlinkPhase_ = 0;
        if (IsAlive())
            SetTimer(hwnd_, kThinkingTimerId, kThinkingTimerMs, nullptr);
    } else {
        lastStatusText_.clear();
        if (IsAlive())
            KillTimer(hwnd_, kThinkingTimerId);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (inputEdit_) EnableWindow(inputEdit_, show ? FALSE : TRUE);
}

std::wstring AgentDialog::ThinkingStatusText() const {
    static const wchar_t* kDots[] = { L"", L".", L"..", L"..." };
    const int phase = thinkingBlinkPhase_ % 4;
    if (!lastStatusText_.empty())
        return lastStatusText_ + kDots[phase];
    return std::wstring(L"正在思考") + kDots[phase];
}

RECT AgentDialog::ThinkingStatusRect() const {
    const RECT frame = InputFrameRect();
    return RECT{kHorizPad, frame.top - 34, ClientW() - kHorizPad, frame.top - 8};
}

std::wstring AgentDialog::BuildUserDisplayText(const std::wstring& text) const {
    std::wstring out = text;
    for (const auto& att : attachments_) {
        if (!out.empty()) out += L"\n";
        out += L"[附件] " + att.fileName;
    }
    if (out.empty()) out = L"(仅附件)";
    return out;
}

void AgentDialog::AddAttachmentsFromPaths(const std::vector<std::wstring>& paths) {
    for (const auto& path : paths) {
        if (static_cast<int>(attachments_.size()) >= kMaxAttachments) break;
        AgentPendingAttachment item;
        std::wstring error;
        if (!AgentLoadAttachmentFromPath(path, item, error)) {
            if (!error.empty()) AddMessage(L"system", error);
            continue;
        }
        bool duplicate = false;
        for (const auto& existing : attachments_) {
            if (_wcsicmp(existing.path.c_str(), item.path.c_str()) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        attachments_.push_back(std::move(item));
    }
    UpdateInputHeight();
    UpdateInputScrollMetrics();
    PositionControls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AgentDialog::HandleFileDrop(HDROP drop) {
    if (!drop) return;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> paths;
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(len, L'\0');
        DragQueryFileW(drop, i, path.data(), len + 1);
        path.resize(len);
        paths.push_back(path);
    }
    DragFinish(drop);
    AddAttachmentsFromPaths(paths);
}

bool AgentDialog::TryPasteAttachmentsFromClipboard() {
    std::vector<std::wstring> filePaths;
    AgentPendingAttachment imageAttachment;
    bool hasImage = false;
    std::wstring error;
    if (!AgentTryReadClipboardAttachments(hwnd_, filePaths, imageAttachment, hasImage, error))
        return false;

    if (!error.empty()) {
        AddMessage(L"system", error);
        return true;
    }

    if (!filePaths.empty()) {
        AddAttachmentsFromPaths(filePaths);
        return true;
    }

    if (hasImage) {
        if (static_cast<int>(attachments_.size()) >= kMaxAttachments) {
            AddMessage(L"system", L"附件数量已达上限（最多 8 个）。");
            AgentReleaseAttachmentBitmap(imageAttachment);
            return true;
        }
        attachments_.push_back(std::move(imageAttachment));
        UpdateInputHeight();
        UpdateInputScrollMetrics();
        PositionControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    return false;
}

void AgentDialog::OpenAttachFileDialog() {
    wchar_t buffer[65536] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter =
        L"All Files (*.*)\0*.*\0"
        L"Images\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.tif;*.tiff\0"
        L"Scripts\0*.json;*.txt\0\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn))
        AddAttachmentsFromPaths(ParseMultiSelectFiles(buffer));
}

void AgentDialog::RemoveAttachmentAt(int index) {
    if (index < 0 || index >= static_cast<int>(attachments_.size())) return;
    AgentReleaseAttachmentBitmap(attachments_[static_cast<size_t>(index)]);
    attachments_.erase(attachments_.begin() + index);
    UpdateInputHeight();
    PositionControls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int AgentDialog::HitAttachmentRemove(int x, int y) const {
    for (int i = 0; i < static_cast<int>(attachments_.size()); ++i) {
        const RECT chip = AttachmentChipRect(i);
        const RECT removeRc{chip.right - 22, chip.top, chip.right, chip.top + 22};
        if (PtIn(removeRc, x, y)) return i;
    }
    return -1;
}

void AgentDialog::DrawAttachmentBar(HDC hdc) {
    if (attachments_.empty()) return;
    SelectObject(hdc, smallFont_);
    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < static_cast<int>(attachments_.size()); ++i) {
        const auto& att = attachments_[static_cast<size_t>(i)];
        const RECT chip = AttachmentChipRect(i);
        FillRectColor(hdc, chip, RGB(248, 248, 248));
        HPEN pen = CreatePen(PS_SOLID, 1, kComboBorderGray);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        Rectangle(hdc, chip.left, chip.top, chip.right, chip.bottom);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        const RECT thumbRc{chip.left + 6, chip.top + 8, chip.left + 46, chip.bottom - 8};
        if (att.thumbnail) {
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ oldBmp = SelectObject(mem, att.thumbnail);
            StretchBlt(hdc, thumbRc.left, thumbRc.top,
                thumbRc.right - thumbRc.left, thumbRc.bottom - thumbRc.top,
                mem, 0, 0, 40, 40, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteDC(mem);
        } else {
            FillRectColor(hdc, thumbRc, RGB(230, 230, 230));
            SetTextColor(hdc, kHint);
            DrawTextW(hdc, L"FILE", -1, const_cast<RECT*>(&thumbRc),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        SetTextColor(hdc, kText);
        RECT nameRc{chip.left + 52, chip.top + 10, chip.right - 24, chip.bottom - 10};
        DrawTextW(hdc, att.fileName.c_str(), -1, &nameRc,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const RECT removeRc{chip.right - 22, chip.top + 2, chip.right - 2, chip.top + 22};
        SetTextColor(hdc, hoverAttachmentRemove_ == i ? RGB(200, 60, 60) : kHint);
        DrawTextW(hdc, L"×", -1, const_cast<RECT*>(&removeRc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void AgentDialog::DrawInputScrollbar(HDC hdc) {
    if (!inputScrollVisible_) return;
    const RECT track = InputScrollTrackRect();
    FillRectColor(hdc, track, kScrollTrackGray);
    const RECT thumb = InputScrollThumbRect();
    FillRectColor(hdc, thumb, kScrollThumbGray);
}

// ── SendUserMessage ───────────────────────────────────────────────
void AgentDialog::SendUserMessage() {
    if (thinking_) return;

    const int len = static_cast<int>(GetWindowTextLengthW(inputEdit_));
    if (len == 0 && attachments_.empty()) return;

    std::wstring text;
    if (len > 0) {
        text.assign(len, L'\0');
        GetWindowTextW(inputEdit_, text.data(), len + 1);
        text.resize(len);
    }

    ClearWelcomeIfNeeded();
    if (createdTime_.empty()) createdTime_ = NowText();
    if (conversationName_.empty())
        conversationName_ = DeriveConversationTitleFromPrompt(text);
    const std::wstring displayText = BuildUserDisplayText(text);
    AddMessage(L"user", displayText);

    const std::wstring configErr = ValidateApiConfig();
    SetWindowTextW(inputEdit_, L"");
    inputLines_ = 1;
    inputScrollPos_ = 0;
    inputScrollVisible_ = false;
    inputScrollMax_ = 0;

    ChatMessage userMsg = AgentBuildUserMessage(text, attachments_);
    AgentReleaseAttachmentBitmaps(attachments_);
    PositionControls();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (!configErr.empty()) {
        AddMessage(L"system", configErr);
        return;
    }

    if (!agent_) return;

    ShowThinking(true);
    BeginThinkingBlock();
    cancelRequested_ = std::make_shared<std::atomic<bool>>(false);
    httpAbort_.Clear();
    lastStatusText_.clear();

    auto agentPtr = agent_;        // shared_ptr — 线程持有引用
    auto alive = aliveFlag_;       // shared_ptr — 线程持有引用
    auto cancelFlag = cancelRequested_;
    AiHttpAbortSlot* httpAbort = &httpAbort_;
    HWND hwnd = hwnd_;

    std::thread([agentPtr, alive, cancelFlag, httpAbort, hwnd, userMsg]() {
        AgentSendCallbacks callbacks;
        callbacks.cancelFlag = cancelFlag.get();
        callbacks.httpAbort = httpAbort;
        callbacks.onStatus = [hwnd, alive](const std::wstring& status) {
            if (status.empty() || !*alive) return;
            auto* p = new std::wstring(status);
            PostMessageW(hwnd, AgentDialog::WM_AGENT_STATUS, 0, reinterpret_cast<LPARAM>(p));
        };
        callbacks.onReasoningDelta = [hwnd, alive](const std::wstring& delta) {
            if (delta.empty() || !*alive) return;
            auto* p = new std::wstring(delta);
            PostMessageW(hwnd, AgentDialog::WM_AGENT_REASONING_DELTA, 0, reinterpret_cast<LPARAM>(p));
        };
        callbacks.onReasoning = [hwnd, alive](const std::wstring& reasoning) {
            if (reasoning.empty() || !*alive) return;
            auto* p = new std::wstring(reasoning);
            PostMessageW(hwnd, AgentDialog::WM_AGENT_REASONING, 0, reinterpret_cast<LPARAM>(p));
        };
        callbacks.onContentDelta = [hwnd, alive](const std::wstring& delta) {
            if (delta.empty() || !*alive) return;
            auto* p = new std::wstring(delta);
            PostMessageW(hwnd, AgentDialog::WM_AGENT_CONTENT_DELTA, 0, reinterpret_cast<LPARAM>(p));
        };
        callbacks.onToolCall = [hwnd, alive](const std::wstring& name, const std::wstring& args) {
            if (!*alive) return;
            auto* p = new std::pair<std::wstring, std::wstring>(name, args);
            PostMessageW(hwnd, AgentDialog::WM_AGENT_TOOLCALL, 0, reinterpret_cast<LPARAM>(p));
        };

        std::wstring response;
        try {
            response = agentPtr->SendMessage(userMsg, callbacks);
        } catch (const json::parse_error& e) {
            response = L"[错误] JSON 解析失败：" + FromUtf8(e.what());
        } catch (const std::exception& e) {
            response = L"[错误] Agent 调用异常：" + FromUtf8(e.what());
        } catch (...) {
            response = L"[错误] Agent 调用发生未知异常。";
        }

        if (!*alive) return;

        if (!response.empty() && response.find(L"[错误]") == 0) {
            auto* p = new std::wstring(response);
            PostMessageW(hwnd, AgentDialog::WM_AGENT_ERROR, 0, reinterpret_cast<LPARAM>(p));
        } else {
            auto* p = new std::wstring(response);
            PostMessageW(hwnd, AgentDialog::WM_AGENT_RESPONSE, 0, reinterpret_cast<LPARAM>(p));
        }
    }).detach();
}

void AgentDialog::CancelRequest() {
    if (!thinking_) return;
    if (cancelRequested_) cancelRequested_->store(true);
    httpAbort_.Abort();
    if (agent_) agent_->AbortActiveHttp();
    lastStatusText_ = L"正在取消";
    const RECT thinkRc = ThinkingStatusRect();
    InvalidateRect(hwnd_, &thinkRc, FALSE);
}

// ── 回调处理 ──────────────────────────────────────────────────────
void AgentDialog::OnStatus(const std::wstring& status) {
    if (status.empty() || !thinking_) return;
    if (status == lastStatusText_) return;

    lastStatusText_ = status;
    const RECT thinkRc = ThinkingStatusRect();
    InvalidateRect(hwnd_, &thinkRc, FALSE);

    if (!thinkingBlockOpen_) return;

    const std::wstring line = status + L"\n";
    if (statusLinePending_ && thinkingStatusStart_ >= 0 && thinkingStatusEnd_ > thinkingStatusStart_) {
        SendMessageW(chatView_, EM_SETSEL, thinkingStatusStart_, thinkingStatusEnd_);
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = kHint;
        SendMessageW(chatView_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
        SendMessageW(chatView_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        thinkingStatusEnd_ = thinkingStatusStart_ + static_cast<int>(line.size());
        ScrollToBottom();
        return;
    }
    statusLinePending_ = true;
    thinkingStatusStart_ = GetWindowTextLengthW(chatView_);
    AppendStyledText(line, kHint);
    thinkingStatusEnd_ = thinkingStatusStart_ + static_cast<int>(line.size());
}

void AgentDialog::OnReasoning(const std::wstring& reasoning) {
    if (thinkingBlockOpen_ && !thinkingHasContent_)
        AppendThinkingLine(reasoning);
}

void AgentDialog::OnReasoningDelta(const std::wstring& delta) {
    AppendThinkingDelta(delta);
}

void AgentDialog::OnContentDelta(const std::wstring& delta) {
    if (thinkingBlockOpen_) EndThinkingBlock();
    AppendAssistantDelta(delta);
}

void AgentDialog::OnAssistantResponse(const std::wstring& text) {
    ShowThinking(false);
    if (assistantStreamOpen_) {
        FinalizeAssistantStream();
    } else {
        EndThinkingBlock();
        if (!text.empty()) AddMessage(L"assistant", text);
    }
    if (inputEdit_) SetFocus(inputEdit_);
}

void AgentDialog::OnToolCall(const std::wstring& name, const std::wstring& /*args*/) {
    const std::wstring label = ToolSkillLabel(name);
    if (thinkingBlockOpen_) {
        AppendThinkingLine(L"· " + label);
        return;
    }
    if (thinking_) {
        lastStatusText_ = L"调用工具: " + label;
        const RECT thinkRc = ThinkingStatusRect();
        InvalidateRect(hwnd_, &thinkRc, FALSE);
    }
}

void AgentDialog::OnError(const std::wstring& error) {
    ShowThinking(false);
    EndThinkingBlock();
    FinalizeAssistantStream();
    AddMessage(L"system", error);
}
