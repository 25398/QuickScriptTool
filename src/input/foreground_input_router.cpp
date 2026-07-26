#include "foreground_input_router.h"
#include "hid_interception.h"
#include "virtual_hid.h"

#include <windows.h>

ForegroundInputRouter& ForegroundInputRouter::Instance() {
    static ForegroundInputRouter inst;
    return inst;
}

void ForegroundInputRouter::BeginSession(bool wantHid) {
    BeginSession(wantHid
        ? quickscript::ForegroundInputBackend::Interception
        : quickscript::ForegroundInputBackend::Software);
}

void ForegroundInputRouter::BeginSession(quickscript::ForegroundInputBackend backend) {
    EndSession();
    sessionActive_.store(true, std::memory_order_release);
    active_.store(static_cast<int>(Active::None), std::memory_order_release);
    didFallback_.store(false, std::memory_order_release);
    fallbackReason_.clear();

    if (backend == quickscript::ForegroundInputBackend::Software) return;

    std::wstring err;
    if (backend == quickscript::ForegroundInputBackend::VirtualHid) {
        if (!VirtualHidBackend::Instance().Open(&err)) {
            didFallback_.store(true, std::memory_order_release);
            fallbackReason_ = err.empty()
                ? L"【警告】虚拟 HID 未就绪，本会话已回退系统模拟（非驱动级注入）。请先在设置中安装驱动。"
                : (L"【警告】虚拟 HID 未就绪，本会话已回退系统模拟（非驱动级注入）：" + err);
            return;
        }
        active_.store(static_cast<int>(Active::VirtualHid), std::memory_order_release);
        return;
    }

    // Interception
    if (!HidInterceptionBackend::Instance().Open(&err)) {
        didFallback_.store(true, std::memory_order_release);
        fallbackReason_ = err.empty()
            ? L"【警告】Interception 未就绪，本会话已回退系统模拟（非驱动级注入）。请先安装驱动并重启。"
            : (L"【警告】Interception 未就绪，本会话已回退系统模拟（非驱动级注入）：" + err);
        return;
    }
    active_.store(static_cast<int>(Active::Interception), std::memory_order_release);
}

void ForegroundInputRouter::EndSession() {
    const Active a = static_cast<Active>(active_.load(std::memory_order_acquire));
    if (a == Active::VirtualHid) {
        VirtualHidBackend::Instance().ReleaseAll();
        VirtualHidBackend::Instance().Close();
    } else if (a == Active::Interception) {
        HidInterceptionBackend::Instance().Close();
    }
    active_.store(static_cast<int>(Active::None), std::memory_order_release);
    sessionActive_.store(false, std::memory_order_release);
}

bool ForegroundInputRouter::IsHidActive() const {
    return active_.load(std::memory_order_acquire) != static_cast<int>(Active::None);
}

quickscript::ForegroundInputBackend ForegroundInputRouter::ActiveBackend() const {
    switch (static_cast<Active>(active_.load(std::memory_order_acquire))) {
    case Active::Interception: return quickscript::ForegroundInputBackend::Interception;
    case Active::VirtualHid: return quickscript::ForegroundInputBackend::VirtualHid;
    case Active::None:
    default: return quickscript::ForegroundInputBackend::Software;
    }
}

bool ForegroundInputRouter::DidFallback() const {
    return didFallback_.load(std::memory_order_acquire);
}

std::wstring ForegroundInputRouter::FallbackReason() const {
    return fallbackReason_;
}

bool ForegroundInputRouter::SendKey(unsigned short scanCode, bool down, bool extended) {
    const Active a = static_cast<Active>(active_.load(std::memory_order_acquire));
    if (a == Active::VirtualHid) {
        return VirtualHidBackend::Instance().SendKey(scanCode, down, extended);
    }
    if (a == Active::Interception) {
        return HidInterceptionBackend::Instance().SendKey(scanCode, down, extended);
    }
    return false;
}

bool ForegroundInputRouter::MoveRelative(int dx, int dy) {
    const Active a = static_cast<Active>(active_.load(std::memory_order_acquire));
    if (a == Active::VirtualHid) {
        return VirtualHidBackend::Instance().MoveRelative(dx, dy);
    }
    if (a == Active::Interception) {
        return HidInterceptionBackend::Instance().MoveRelative(dx, dy);
    }
    return false;
}

bool ForegroundInputRouter::Button(MouseButtonType button, bool down) {
    const Active a = static_cast<Active>(active_.load(std::memory_order_acquire));
    if (a == Active::VirtualHid) {
        return VirtualHidBackend::Instance().Button(button, down);
    }
    if (a == Active::Interception) {
        return HidInterceptionBackend::Instance().Button(button, down);
    }
    return false;
}

bool ForegroundInputRouter::Wheel(int delta, bool horizontal) {
    const Active a = static_cast<Active>(active_.load(std::memory_order_acquire));
    if (a == Active::VirtualHid) {
        return VirtualHidBackend::Instance().Wheel(delta, horizontal);
    }
    if (a == Active::Interception) {
        return HidInterceptionBackend::Instance().Wheel(delta, horizontal);
    }
    return false;
}

bool ForegroundInputRouter::SetCursorScreen(int x, int y) {
    const Active a = static_cast<Active>(active_.load(std::memory_order_acquire));
    if (a == Active::VirtualHid) {
        // Pure VHF absolute report — do not also call SetCursorPos (avoids hybrid path).
        return VirtualHidBackend::Instance().MoveAbsoluteScreen(x, y);
    }
    if (a == Active::Interception) {
        if (!HidInterceptionBackend::Instance().MoveAbsoluteScreen(x, y)) return false;
        SetCursorPos(x, y);
        return true;
    }
    return SetCursorPos(x, y) != FALSE;
}
