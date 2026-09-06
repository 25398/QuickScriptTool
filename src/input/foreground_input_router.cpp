#include "foreground_input_router.h"
#include "hid_interception.h"
#include "synthetic_input_filter.h"
#include "virtual_hid.h"

#include <windows.h>

namespace {

UINT MouseButtonToVk(MouseButtonType button) {
    switch (button) {
    case MouseButtonType::Right: return VK_RBUTTON;
    case MouseButtonType::Middle: return VK_MBUTTON;
    case MouseButtonType::X1: return VK_XBUTTON1;
    case MouseButtonType::X2: return VK_XBUTTON2;
    case MouseButtonType::Left:
    default: return VK_LBUTTON;
    }
}

UINT ScanToVk(unsigned short scanCode, bool extended) {
    UINT mapped = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
    if (mapped == 0) mapped = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK);
    // MapVirtualKey 对部分扩展键不区分左右；保留 scan 指纹为主。
    (void)extended;
    return mapped;
}

}  // namespace

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
    {
        std::lock_guard<std::mutex> lock(reasonMu_);
        fallbackReason_.clear();
    }

    // 软件模拟也要开指纹会话：长按热键=脚本同键时靠 NoteKey/ExtraInfo 滤假抬起
    if (backend == quickscript::ForegroundInputBackend::Software) {
        synthetic_input::BeginSession();
        return;
    }

    std::wstring err;
    if (backend == quickscript::ForegroundInputBackend::VirtualHid) {
        if (!VirtualHidBackend::Instance().Open(&err)) {
            didFallback_.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(reasonMu_);
                fallbackReason_ = err.empty()
                    ? L"【警告】虚拟 HID 未就绪，本会话已回退系统模拟（非驱动级注入）。请先在设置中安装驱动。"
                    : (L"【警告】虚拟 HID 未就绪，本会话已回退系统模拟（非驱动级注入）：" + err);
            }
            synthetic_input::BeginSession();
            return;
        }
        active_.store(static_cast<int>(Active::VirtualHid), std::memory_order_release);
        synthetic_input::BeginSession();
        synthetic_input::RefreshVirtualHidRawDevices();
        return;
    }

    // Interception
    if (!HidInterceptionBackend::Instance().Open(&err)) {
        didFallback_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(reasonMu_);
            fallbackReason_ = err.empty()
                ? L"【警告】Interception 未就绪，本会话已回退系统模拟（非驱动级注入）。请先安装驱动并重启。"
                : (L"【警告】Interception 未就绪，本会话已回退系统模拟（非驱动级注入）：" + err);
        }
        synthetic_input::BeginSession();
        return;
    }
    active_.store(static_cast<int>(Active::Interception), std::memory_order_release);
    synthetic_input::BeginSession();
    synthetic_input::RefreshVirtualHidRawDevices();
}

void ForegroundInputRouter::EndSession() {
    const Active a = static_cast<Active>(active_.load(std::memory_order_acquire));
    if (a == Active::VirtualHid) {
        // Close 内含抬起；先开 teardown 窗，避免异步 LL 当成真人输入。
        synthetic_input::NoteSessionTeardown();
        VirtualHidBackend::Instance().Close();
        synthetic_input::EndSession();
    } else if (a == Active::Interception) {
        synthetic_input::NoteSessionTeardown();
        HidInterceptionBackend::Instance().Close();
        synthetic_input::EndSession();
    } else if (sessionActive_.load(std::memory_order_acquire)) {
        synthetic_input::NoteSessionTeardown();
        synthetic_input::EndSession();
    }
    active_.store(static_cast<int>(Active::None), std::memory_order_release);
    sessionActive_.store(false, std::memory_order_release);
}

void ForegroundInputRouter::ForceTeardown() {
    EndSession();
    // 会话标志可能已乱：再强制收一次，避免虚拟键鼠残留按下。
    VirtualHidBackend::Instance().Close();
    HidInterceptionBackend::Instance().Close();
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
    std::lock_guard<std::mutex> lock(reasonMu_);
    return fallbackReason_;
}

bool ForegroundInputRouter::SendKey(UINT vk, unsigned short scanCode, bool down, bool extended) {
    const Active a = static_cast<Active>(active_.load(std::memory_order_acquire));
    if (a == Active::None) return false;
    // 优先用调用方 VK（与脚本/LL vkCode 一致）；ScanToVk 仅作回退，避免 Expect 对不上长按同键
    if (vk == 0) vk = ScanToVk(scanCode, extended);
    synthetic_input::NoteKey(vk, scanCode, extended, down);
    // 仅 VirtualHid 需要 Expect（无 INJECTED/ExtraInfo）。
    // Software/Interception 打 ExtraInfo；若再 Expect，同键连点会刷满 TTL，
    // 真人松手在 LL 被当成注入 → 脚本不停、打字插键（bian→bani）。
    const bool needExpect = (a == Active::VirtualHid);
    if (needExpect) {
        if (down) synthetic_input::ExpectSyntheticKeyDown(vk);
        else synthetic_input::ExpectSyntheticKeyUp(vk);
    }
    bool ok = false;
    if (a == Active::VirtualHid) {
        ok = VirtualHidBackend::Instance().SendKey(scanCode, down, extended);
    } else if (a == Active::Interception) {
        ok = HidInterceptionBackend::Instance().SendKey(scanCode, down, extended);
    }
    if (!ok && needExpect) {
        if (down) synthetic_input::ConsumeSyntheticKeyDown(vk);
        else synthetic_input::ConsumeSyntheticKeyUp(vk);
    }
    return ok;
}

bool ForegroundInputRouter::MoveRelative(int dx, int dy) {
    const Active a = static_cast<Active>(active_.load(std::memory_order_acquire));
    if (a == Active::None) return false;
    if (dx != 0 || dy != 0) synthetic_input::NoteMouseMove();
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
    if (a == Active::None) return false;
    synthetic_input::NoteMouseButton(MouseButtonToVk(button), down);
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
    if (a == Active::None) return false;
    synthetic_input::NoteMouseWheel();
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
        // 绝对桌面：只 SetCursorPos（不发绝对 HID）。
        // 不要 NoteMouseMove：SetCursorPos 在 LL 带 INJECTED；若再记移动指纹，
        // 绝对宏会连续刷新 TTL，真人挪鼠会被当成注入而无法脱离。
        (void)VirtualHidBackend::Instance().MoveAbsoluteScreen(x, y);
        return SetCursorPos(x, y) != FALSE;
    }
    if (a == Active::Interception) {
        // Interception 绝对报告在 LL 上通常无 INJECTED，需移动指纹。
        synthetic_input::NoteMouseMove();
        if (!HidInterceptionBackend::Instance().MoveAbsoluteScreen(x, y)) return false;
        SetCursorPos(x, y);
        return true;
    }
    return SetCursorPos(x, y) != FALSE;
}
