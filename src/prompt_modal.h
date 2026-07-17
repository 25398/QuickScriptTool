#pragma once
// ──────────────────────────────────────────────────────────────────
// prompt_modal.h — 统一提示/确认弹窗（主界面删除确认风格）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <commctrl.h>

#include <functional>
#include <string>

enum class PromptModalMode { Info, Confirm };

enum class PromptModalButton { None, Ok, Cancel };

struct PromptModalLayout {
    RECT dialog{};
    RECT message{};
    RECT ok{};
    RECT cancel{};
    bool hasCancel = false;
};

PromptModalLayout ComputePromptModalLayout(const RECT& client, PromptModalMode mode,
                                           const std::wstring& message = L"",
                                           HFONT font = nullptr);

void PaintPromptModal(HDC hdc, const RECT& client, const std::wstring& message,
    PromptModalMode mode, bool hoverOk, bool hoverCancel, HFONT textFont);

PromptModalButton PromptModalHitTest(int x, int y, const RECT& client, PromptModalMode mode,
                                     const std::wstring& message = L"", HFONT font = nullptr);

class PromptModal {
public:
    void Bind(HWND owner, HFONT font, std::function<void()> afterClose = nullptr);
    void OnOwnerResize();

    bool visible() const { return visible_; }
    PromptModalMode mode() const { return mode_; }

    void ShowInfo(const std::wstring& message);
    void ShowConfirm(const std::wstring& message, std::function<void(bool accepted)> onDone);
    void Close(PromptModalButton button = PromptModalButton::None);

private:
    void EnsureShield();
    void SyncShield();
    void InvalidateButtonRegion();
    bool UpdateHover(int x, int y, const RECT& client);
    void TrackMouseLeave();
    RECT ClientRect() const;
    RECT OwnerScreenRect() const;
    void RefreshOwnerAfterClose();
    void ReleaseMessageFont();
    void EnsureMessageFont();

    static LRESULT CALLBACK ShieldProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR, DWORD_PTR refData);

    HWND owner_ = nullptr;
    HWND shield_ = nullptr;
    HFONT font_ = nullptr;
    HFONT messageFont_ = nullptr;
    bool visible_ = false;
    PromptModalMode mode_ = PromptModalMode::Info;
    std::wstring message_;
    bool hoverOk_ = false;
    bool hoverCancel_ = false;
    bool cursorOnButton_ = false;
    bool trackingLeave_ = false;
    // Ignore the mouse-up that still belongs to the click which opened this modal.
    bool suppressClickUntilRelease_ = false;
    PromptModalButton armedButton_ = PromptModalButton::None;
    std::function<void(bool)> onDone_;
    std::function<void()> afterClose_;
};
