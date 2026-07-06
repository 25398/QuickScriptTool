#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_dialog.h — AI Agent 聊天对话框
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <memory>
#include <string>
#include <vector>
#include <atomic>

#include "agent_attachment.h"
#include "agent_core.h"
#include "agent_system_prompt.h"
#include "app_settings.h"
#include "config.h"
#include "panel_popup_combo.h"

class AgentDialog {
public:
    AgentDialog();
    ~AgentDialog();

    bool Show(HWND owner, const quickscript::AiApiSettings& aiSettings);
    bool IsAlive() const { return hwnd_ != nullptr && IsWindow(hwnd_); }
    bool IsVisible() const { return IsAlive() && IsWindowVisible(hwnd_); }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK InputSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR refData);
    static LRESULT CALLBACK ChatViewSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR refData);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Init();
    void Paint();
    void Cleanup();
    void PositionControls();
    void UpdateInputHeight();
    void SetWelcomeMessage();
    void ClearWelcomeIfNeeded();
    void RefreshModelCombo();
    void ApplySelectedModelProfile();
    void RebuildAgentCore();
    std::wstring ValidateApiConfig() const;
    std::wstring BuildUserDisplayText(const std::wstring& text) const;

    void SendUserMessage();
    std::wstring ThinkingStatusText() const;
    RECT ThinkingStatusRect() const;
    void OnAssistantResponse(const std::wstring& text);
    void OnToolCall(const std::wstring& name, const std::wstring& args);
    void OnReasoning(const std::wstring& reasoning);
    void OnReasoningDelta(const std::wstring& delta);
    void OnContentDelta(const std::wstring& delta);
    void OnStatus(const std::wstring& status);
    void OnError(const std::wstring& error);

    void BeginThinkingBlock();
    void AppendThinkingLine(const std::wstring& line);
    void AppendThinkingDelta(const std::wstring& delta);
    void EndThinkingBlock();
    void BeginAssistantStream();
    void AppendAssistantDelta(const std::wstring& delta);
    void FinalizeAssistantStream();
    void AppendStyledText(const std::wstring& text, COLORREF color);
    std::wstring FormatPlainChat(const std::wstring& text) const;
    std::wstring ToolSkillLabel(const std::wstring& name) const;

    void AddAttachmentsFromPaths(const std::vector<std::wstring>& paths);
    void HandleFileDrop(HDROP drop);
    bool TryPasteAttachmentsFromClipboard();
    void OpenAttachFileDialog();
    void RemoveAttachmentAt(int index);
    void DrawAttachmentBar(HDC hdc);
    void DrawInputScrollbar(HDC hdc);
    void UpdateInputScrollMetrics();
    void HandleInputMouseWheel(int delta);
    int VisibleInputLines() const;
    void SetInputScrollPos(int pos);
    void SyncInputEditScroll();
    void UpdateInputScrollFromThumb(int thumbTop);
    void EnsureInputCaretVisible();
    bool HitInputScrollThumb(int x, int y) const;
    bool HitInputScrollTrack(int x, int y) const;
    int HitAttachmentRemove(int x, int y) const;
    RECT AttachmentChipRect(int index) const;
    RECT AttachmentBarRect() const;

    void AddMessage(const std::wstring& role, const std::wstring& content);
    void AppendToChatView(const std::wstring& text);
    void ScrollToBottom();
    void ShowThinking(bool show);

    bool HitClose(int x, int y) const;
    bool HitMinimize(int x, int y) const;
    bool HitTitle(int x, int y) const;
    bool HitSend(int x, int y) const;
    bool HitAttach(int x, int y) const;
    bool PtIn(const RECT& rc, int x, int y) const;

    int CountInputLines() const;
    int ClientW() const;
    int ClientH() const;

    int InputFrameHeight() const;
    RECT MessagesRect() const;
    RECT InputFrameRect() const;
    RECT InputRect() const;
    RECT AttachBtnRect() const;
    RECT ModelComboRect() const;
    RECT SendBtnRect() const;
    RECT InputScrollTrackRect() const;
    RECT InputScrollThumbRect() const;
    RECT CloseRect() const;
    RECT MinimizeRect() const;

    enum CtrlId {
        kChatView = 4001,
        kInputEdit = 4002,
    };

    static constexpr UINT WM_AGENT_RESPONSE = WM_USER + 101;
    static constexpr UINT WM_AGENT_TOOLCALL = WM_USER + 102;
    static constexpr UINT WM_AGENT_ERROR = WM_USER + 103;
    static constexpr UINT WM_AGENT_CHUNK = WM_USER + 104;
    static constexpr UINT WM_AGENT_DEFER_FOCUS = WM_USER + 105;
    static constexpr UINT WM_AGENT_REASONING = WM_USER + 106;
    static constexpr UINT WM_AGENT_REASONING_DELTA = WM_USER + 107;
    static constexpr UINT WM_AGENT_CONTENT_DELTA = WM_USER + 108;
    static constexpr UINT WM_AGENT_STATUS = WM_USER + 109;
    static constexpr UINT kThinkingTimerId = 2001;
    static constexpr int kThinkingTimerMs = 420;

    static constexpr int kDialogW = kHomeWidth;
    static constexpr int kDialogH = kHomeHeight;
    static constexpr int kTitleBarH = kTitleH;
    static constexpr int kInputLineH = 28;
    static constexpr int kInputPadV = 10;
    static constexpr int kInputMaxLines = 5;
    static constexpr int kInputBottomPad = 14;
    static constexpr int kInputGap = 10;
    static constexpr int kFrameInnerPad = 8;
    static constexpr int kSendBtnW = 78;
    static constexpr int kSendBtnH = 36;
    static constexpr int kAttachBtnW = 34;
    static constexpr int kAttachBtnH = 34;
    static constexpr int kHorizPad = 16;
    static constexpr int kCloseBtnW = 46;
    static constexpr int kModelComboW = 205;
    static constexpr int kModelComboGap = 8;
    static constexpr int kAttachmentBarH = 64;
    static constexpr int kAttachmentChipW = 128;
    static constexpr int kAttachmentChipGap = 8;
    static constexpr int kInputScrollW = kEditorScrollW;
    static constexpr int kMaxAttachments = 8;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND inputEdit_ = nullptr;
    HWND chatView_ = nullptr;

    PanelPopupCombo modelCombo_;

    std::shared_ptr<AgentCore> agent_;
    std::shared_ptr<std::atomic<bool>> aliveFlag_;
    quickscript::AiApiSettings aiSettings_;
    std::vector<quickscript::AiModelProfile> savedModels_;
    int selectedProfileIndex_ = -1;
    std::vector<AgentPendingAttachment> attachments_;

    HFONT font_ = nullptr;
    HFONT closeFont_ = nullptr;
    HFONT chatFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HBRUSH bgBrush_ = nullptr;

    bool hoverClose_ = false;
    bool hoverMinimize_ = false;
    bool hoverSend_ = false;
    bool hoverAttach_ = false;
    bool hoverModelCombo_ = false;
    bool thinking_ = false;
    int thinkingBlinkPhase_ = 0;
    bool thinkingBlockOpen_ = false;
    bool thinkingHasContent_ = false;
    bool assistantStreamOpen_ = false;
    bool statusLinePending_ = false;
    int thinkingStatusStart_ = -1;
    int thinkingStatusEnd_ = -1;
    std::wstring lastStatusText_;
    bool enterSendIssuedForKey_ = false;
    bool initialized_ = false;
    bool conversationStarted_ = false;
    bool layoutUpdating_ = false;
    bool inputScrollVisible_ = false;
    int inputLines_ = 1;
    int inputScrollPos_ = 0;
    int inputScrollMax_ = 0;
    int hoverAttachmentRemove_ = -1;
    bool inputScrollbarDragging_ = false;
    int inputScrollbarDragOffset_ = 0;

    std::wstring currentAssistantMsg_;
};
