#include "synthetic_input_filter.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace synthetic_input {
namespace {

constexpr int kRingSize = 128;                 // 仅键/鼠标按钮，避免被移动刷掉
constexpr LONGLONG kKeyButtonTtlUs = 300000;   // 300ms（HID 异步回灌可能晚于 NoteKey）
constexpr LONGLONG kWheelTtlUs = 120000;
// Interception 绝对移动在 LL 无 INJECTED，指纹窗需覆盖动作间短等待。
constexpr LONGLONG kMoveTtlUs = 200000;        // 200ms
constexpr LONGLONG kTeardownTtlUs = 200000;    // ReleaseAll 异步抬起
enum class Kind : uint8_t {
    Key = 0,
    MouseButton,
};

struct Entry {
    Kind kind = Kind::Key;
    UINT vk = 0;
    unsigned short scan = 0;
    bool extended = false;
    bool down = false;
    LONGLONG qpcUs = 0;
};

std::mutex g_mu;
Entry g_ring[kRingSize]{};
unsigned g_ringWrite = 0;   // 无符号计数 + 掩码索引，避免会话内长跑溢出为负索引
std::atomic<bool> g_session{false};
std::atomic<bool> g_breakoutTracking{false};
UINT g_watchedVks[256]{};
int g_watchedCount = 0;
std::atomic<LONGLONG> g_teardownUntilUs{0};
std::atomic<LONGLONG> g_lastMoveQpcUs{0};
std::atomic<LONGLONG> g_lastWheelQpcUs{0};
std::atomic<LONGLONG> g_lastRawMoveQpcUs{0};
std::atomic<LONGLONG> g_lastRawWheelQpcUs{0};

std::mutex g_devMu;
std::vector<HANDLE> g_vhidDevices;
bool g_rawRegistered = false;

/// 按 VK 计的待匹配注入按下/抬起（VirtualHid / Interception 无可靠 INJECTED）
std::atomic<int> g_synthKeyUpCredit[256]{};
std::atomic<int> g_synthKeyDownCredit[256]{};
/// 最近一次 Expect 的时间（µs）；超时未 Consume 则作废，避免 Interception 漏回灌导致真人松手被永远当成注入
std::atomic<LONGLONG> g_synthKeyUpExpectUs[256]{};
std::atomic<LONGLONG> g_synthKeyDownExpectUs[256]{};
constexpr LONGLONG kSynthCreditTtlUs = 100000; // 100ms

void ClearSynthKeyCredits() {
    for (auto& c : g_synthKeyUpCredit) c.store(0, std::memory_order_relaxed);
    for (auto& c : g_synthKeyDownCredit) c.store(0, std::memory_order_relaxed);
    for (auto& t : g_synthKeyUpExpectUs) t.store(0, std::memory_order_relaxed);
    for (auto& t : g_synthKeyDownExpectUs) t.store(0, std::memory_order_relaxed);
}

LONGLONG QpcUsNow() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (freq.QuadPart <= 0) return 0;
    return (now.QuadPart * 1000000LL) / freq.QuadPart;
}

bool InTeardownWindow() {
    const LONGLONG until = g_teardownUntilUs.load(std::memory_order_acquire);
    if (until == 0) return false;
    return QpcUsNow() <= until;
}

void PushLocked(Entry e) {
    e.qpcUs = QpcUsNow();
    g_ring[g_ringWrite & (kRingSize - 1)] = e;
    ++g_ringWrite;
}

bool MatchLocked(Kind kind, UINT vk, unsigned short scan, bool extended, bool down) {
    const LONGLONG now = QpcUsNow();
    const unsigned total = g_ringWrite < static_cast<unsigned>(kRingSize)
        ? g_ringWrite : static_cast<unsigned>(kRingSize);
    for (unsigned i = 0; i < total; ++i) {
        const int idx = static_cast<int>((g_ringWrite - 1 - i) & (kRingSize - 1));
        const Entry& e = g_ring[idx];
        if (e.kind != kind) continue;
        if (now - e.qpcUs > kKeyButtonTtlUs) continue;
        if (e.down != down) continue;
        if (kind == Kind::Key) {
            if (vk != 0 && e.vk != 0 && e.vk == vk) return true;
            if (scan != 0 && e.scan == scan && e.extended == extended) return true;
            continue;
        }
        if (e.vk == vk) return true;
    }
    return false;
}

bool DeviceNameLooksLikeVirtualHid(const wchar_t* name) {
    if (!name || !name[0]) return false;
    for (const wchar_t* p = name; *p; ++p) {
        const wchar_t* t = kVirtualHidVidPidToken;
        const wchar_t* q = p;
        bool ok = true;
        while (*t) {
            wchar_t a = *q++;
            wchar_t b = *t++;
            if (a >= L'a' && a <= L'z') a = static_cast<wchar_t>(a - L'a' + L'A');
            if (b >= L'a' && b <= L'z') b = static_cast<wchar_t>(b - L'a' + L'A');
            if (!a || a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

bool IsWatchedVkLocked(UINT vk) {
    if (vk == 0) return false;
    for (int i = 0; i < g_watchedCount; ++i) {
        if (g_watchedVks[i] == vk) return true;
    }
    return false;
}

bool RecentPulse(std::atomic<LONGLONG>& stamp, LONGLONG ttlUs) {
    const LONGLONG t = stamp.load(std::memory_order_acquire);
    if (t == 0) return false;
    return (QpcUsNow() - t) <= ttlUs;
}

}  // namespace

void BeginSession() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_ringWrite = 0;
        std::memset(g_ring, 0, sizeof(g_ring));
        g_teardownUntilUs.store(0, std::memory_order_release);
        g_lastMoveQpcUs.store(0, std::memory_order_release);
        g_lastWheelQpcUs.store(0, std::memory_order_release);
        g_lastRawMoveQpcUs.store(0, std::memory_order_release);
        g_lastRawWheelQpcUs.store(0, std::memory_order_release);
        // 默认只服务热键；脱离由调用方 SetBreakoutTracking(true)
        g_breakoutTracking.store(false, std::memory_order_release);
        g_session.store(true, std::memory_order_release);
    }
    ClearSynthKeyCredits();
    // 不在持 g_mu 时扫设备，避免与 WM_INPUT 锁序死锁
    RefreshVirtualHidRawDevices();
}

void EndSession() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_ringWrite = 0;
    std::memset(g_ring, 0, sizeof(g_ring));
    g_session.store(false, std::memory_order_release);
    g_breakoutTracking.store(false, std::memory_order_release);
    g_lastMoveQpcUs.store(0, std::memory_order_release);
    g_lastWheelQpcUs.store(0, std::memory_order_release);
    g_lastRawMoveQpcUs.store(0, std::memory_order_release);
    g_lastRawWheelQpcUs.store(0, std::memory_order_release);
    ClearSynthKeyCredits();
    // teardown 窗口保留至超时，覆盖 ReleaseAll 异步抬起
}

void SetBreakoutTracking(bool enabled) {
    g_breakoutTracking.store(enabled, std::memory_order_release);
    if (!enabled) {
        g_lastMoveQpcUs.store(0, std::memory_order_release);
        g_lastWheelQpcUs.store(0, std::memory_order_release);
        g_lastRawMoveQpcUs.store(0, std::memory_order_release);
        g_lastRawWheelQpcUs.store(0, std::memory_order_release);
    }
}

bool BreakoutTrackingEnabled() {
    return g_breakoutTracking.load(std::memory_order_acquire);
}

void SetWatchedHotkeyVks(const UINT* vks, int count) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_watchedCount = 0;
    if (!vks || count <= 0) return;
    for (int i = 0; i < count && g_watchedCount < static_cast<int>(sizeof(g_watchedVks) / sizeof(g_watchedVks[0])); ++i) {
        const UINT vk = vks[i];
        if (vk == 0) continue;
        if (IsWatchedVkLocked(vk)) continue;
        g_watchedVks[g_watchedCount++] = vk;
    }
}

void NoteSessionTeardown() {
    g_teardownUntilUs.store(QpcUsNow() + kTeardownTtlUs, std::memory_order_release);
}

// 同键连点可能积压数帧；过大则漏回灌时真人松手会长时间被挡
constexpr int kMaxSynthCreditPerVk = 16;

void ExpectSyntheticKeyUp(UINT vk) {
    if (vk == 0 || vk >= 256) return;
    g_synthKeyUpExpectUs[vk].store(QpcUsNow(), std::memory_order_relaxed);
    int cur = g_synthKeyUpCredit[vk].load(std::memory_order_relaxed);
    while (cur < kMaxSynthCreditPerVk) {
        if (g_synthKeyUpCredit[vk].compare_exchange_weak(
                cur, cur + 1, std::memory_order_relaxed)) {
            return;
        }
    }
}

bool ConsumeSyntheticKeyUp(UINT vk) {
    if (vk == 0 || vk >= 256) return false;
    const LONGLONG now = QpcUsNow();
    const LONGLONG stamp = g_synthKeyUpExpectUs[vk].load(std::memory_order_relaxed);
    if (stamp == 0 || now - stamp > kSynthCreditTtlUs) {
        g_synthKeyUpCredit[vk].store(0, std::memory_order_relaxed);
        return false;
    }
    int cur = g_synthKeyUpCredit[vk].load(std::memory_order_relaxed);
    while (cur > 0) {
        if (g_synthKeyUpCredit[vk].compare_exchange_weak(
                cur, cur - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void ExpectSyntheticKeyDown(UINT vk) {
    if (vk == 0 || vk >= 256) return;
    g_synthKeyDownExpectUs[vk].store(QpcUsNow(), std::memory_order_relaxed);
    int cur = g_synthKeyDownCredit[vk].load(std::memory_order_relaxed);
    while (cur < kMaxSynthCreditPerVk) {
        if (g_synthKeyDownCredit[vk].compare_exchange_weak(
                cur, cur + 1, std::memory_order_relaxed)) {
            return;
        }
    }
}

bool ConsumeSyntheticKeyDown(UINT vk) {
    if (vk == 0 || vk >= 256) return false;
    const LONGLONG now = QpcUsNow();
    const LONGLONG stamp = g_synthKeyDownExpectUs[vk].load(std::memory_order_relaxed);
    if (stamp == 0 || now - stamp > kSynthCreditTtlUs) {
        g_synthKeyDownCredit[vk].store(0, std::memory_order_relaxed);
        return false;
    }
    int cur = g_synthKeyDownCredit[vk].load(std::memory_order_relaxed);
    while (cur > 0) {
        if (g_synthKeyDownCredit[vk].compare_exchange_weak(
                cur, cur - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void NoteKey(UINT vk, unsigned short scanCode, bool extended, bool down) {
    if (!g_session.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    // 脱离开启：宏按键都要挡脱离钩；否则只挡已登记的启停热键。
    if (!g_breakoutTracking.load(std::memory_order_relaxed) && !IsWatchedVkLocked(vk)) return;
    Entry e{};
    e.kind = Kind::Key;
    e.vk = vk;
    e.scan = scanCode;
    e.extended = extended;
    e.down = down;
    PushLocked(e);
}

void NoteMouseButton(UINT buttonVk, bool down) {
    if (!g_session.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_breakoutTracking.load(std::memory_order_relaxed) && !IsWatchedVkLocked(buttonVk)) return;
    Entry e{};
    e.kind = Kind::MouseButton;
    e.vk = buttonVk;
    e.down = down;
    PushLocked(e);
}

void NoteMouseWheel() {
    if (!g_session.load(std::memory_order_acquire)) return;
    if (!g_breakoutTracking.load(std::memory_order_acquire)) return;
    g_lastWheelQpcUs.store(QpcUsNow(), std::memory_order_release);
}

void NoteMouseMove() {
    if (!g_session.load(std::memory_order_acquire)) return;
    if (!g_breakoutTracking.load(std::memory_order_acquire)) return;
    g_lastMoveQpcUs.store(QpcUsNow(), std::memory_order_release);
}

void NoteVirtualHidRawKeyboard() {
    (void)0;
}

void NoteVirtualHidRawKeyboardEvent(UINT vk, unsigned short scan, bool extended, bool down) {
    // Raw 路径不依赖 watched 表：长按热键与脚本同键时必须挡住注入抬起/按下。
    if (!g_session.load(std::memory_order_acquire)) return;
    if (vk == 0 && scan == 0) return;
    std::lock_guard<std::mutex> lock(g_mu);
    Entry e{};
    e.kind = Kind::Key;
    e.vk = vk;
    e.scan = scan;
    e.extended = extended;
    e.down = down;
    PushLocked(e);
}

void NoteVirtualHidRawMouseButton() {
    (void)0;
}

void NoteVirtualHidRawMouseWheel() {
    if (!g_session.load(std::memory_order_acquire)) return;
    if (!g_breakoutTracking.load(std::memory_order_acquire)) return;
    g_lastRawWheelQpcUs.store(QpcUsNow(), std::memory_order_release);
}

void NoteVirtualHidRawMouseMove() {
    if (!g_session.load(std::memory_order_acquire)) return;
    if (!g_breakoutTracking.load(std::memory_order_acquire)) return;
    g_lastRawMoveQpcUs.store(QpcUsNow(), std::memory_order_release);
}

bool MatchesKey(UINT vk, unsigned short scanCode, bool extended, bool down) {
    if (InTeardownWindow()) return true;
    if (!g_session.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    return MatchLocked(Kind::Key, vk, scanCode, extended, down);
}

bool MatchesMouseButton(UINT buttonVk, bool down) {
    if (InTeardownWindow()) return true;
    if (!g_session.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    return MatchLocked(Kind::MouseButton, buttonVk, 0, false, down);
}

bool MatchesMouseWheel() {
    if (InTeardownWindow()) return true;
    if (!g_session.load(std::memory_order_acquire)) return false;
    if (!g_breakoutTracking.load(std::memory_order_acquire)) return false;
    return RecentPulse(g_lastWheelQpcUs, kWheelTtlUs)
        || RecentPulse(g_lastRawWheelQpcUs, kWheelTtlUs);
}

bool MatchesMouseMove() {
    if (InTeardownWindow()) return true;
    if (!g_session.load(std::memory_order_acquire)) return false;
    if (!g_breakoutTracking.load(std::memory_order_acquire)) return false;
    return RecentPulse(g_lastMoveQpcUs, kMoveTtlUs)
        || RecentPulse(g_lastRawMoveQpcUs, kMoveTtlUs);
}

void RefreshVirtualHidRawDevices() {
    UINT count = 0;
    GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
    if (count == 0) {
        std::lock_guard<std::mutex> lock(g_devMu);
        g_vhidDevices.clear();
        return;
    }
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) {
        return;
    }
    std::vector<HANDLE> found;
    for (UINT i = 0; i < count; ++i) {
        const HANDLE h = list[i].hDevice;
        UINT nameChars = 0;
        GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, nullptr, &nameChars);
        if (nameChars == 0) continue;
        std::vector<wchar_t> name(nameChars + 1, L'\0');
        if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, name.data(), &nameChars) == static_cast<UINT>(-1)) {
            continue;
        }
        if (DeviceNameLooksLikeVirtualHid(name.data())) {
            found.push_back(h);
        }
    }
    std::lock_guard<std::mutex> lock(g_devMu);
    g_vhidDevices = std::move(found);
}

bool IsVirtualHidRawDevice(HANDLE device) {
    if (!device) return false;
    std::lock_guard<std::mutex> lock(g_devMu);
    for (HANDLE h : g_vhidDevices) {
        if (h == device) return true;
    }
    return false;
}

bool RegisterRawInputSink(HWND hwnd) {
    if (!hwnd) return false;
    RAWINPUTDEVICE rid[2]{};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x06;
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x02;
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        return false;
    }
    g_rawRegistered = true;
    RefreshVirtualHidRawDevices();
    return true;
}

void UnregisterRawInputSink(HWND hwnd) {
    if (!g_rawRegistered || !hwnd) return;
    RAWINPUTDEVICE rid[2]{};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x06;
    rid[0].dwFlags = RIDEV_REMOVE;
    rid[0].hwndTarget = nullptr;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x02;
    rid[1].dwFlags = RIDEV_REMOVE;
    rid[1].hwndTarget = nullptr;
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
    g_rawRegistered = false;
}

RawBreakoutHint OnRawInput(HRAWINPUT hRawInput, bool monitorUserBreakout) {
    RawBreakoutHint hint{};
    UINT size = 0;
    GetRawInputData(hRawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0) return hint;
    std::vector<BYTE> buf(size);
    if (GetRawInputData(hRawInput, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER))
        == static_cast<UINT>(-1)) {
        return hint;
    }
    const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buf.data());
    const HANDLE device = raw->header.hDevice;
    hint.device = device;

    // SendInput / 无设备句柄：不是物理键盘报告
    if (!device) {
        if (raw->header.dwType == RIM_TYPEKEYBOARD) {
            const USHORT flags = raw->data.keyboard.Flags;
            const bool up = (flags & RI_KEY_BREAK) != 0;
            const UINT vk = static_cast<UINT>(raw->data.keyboard.VKey);
            const unsigned short scan = static_cast<unsigned short>(raw->data.keyboard.MakeCode);
            const bool ext = (flags & RI_KEY_E0) != 0;
            if (vk != 0 && vk != 0xFF) {
                NoteVirtualHidRawKeyboardEvent(vk, scan, ext, !up);
            }
        }
        return hint;
    }

    bool isOurs = IsVirtualHidRawDevice(device);
    if (!isOurs) {
        UINT nameChars = 0;
        GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &nameChars);
        if (nameChars > 0) {
            std::vector<wchar_t> name(nameChars + 1, L'\0');
            if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &nameChars)
                != static_cast<UINT>(-1)
                && DeviceNameLooksLikeVirtualHid(name.data())) {
                std::lock_guard<std::mutex> lock(g_devMu);
                g_vhidDevices.push_back(device);
                isOurs = true;
            }
        }
    }

    hint.fromOurDevice = isOurs;
    if (isOurs) {
        if (raw->header.dwType == RIM_TYPEKEYBOARD) {
            const USHORT flags = raw->data.keyboard.Flags;
            const bool up = (flags & RI_KEY_BREAK) != 0;
            const UINT vk = static_cast<UINT>(raw->data.keyboard.VKey);
            const unsigned short scan = static_cast<unsigned short>(raw->data.keyboard.MakeCode);
            const bool ext = (flags & RI_KEY_E0) != 0;
            NoteVirtualHidRawKeyboardEvent(vk, scan, ext, !up);
            // 勿 Consume：信用留给 LL（VHID 注入无 INJECTED，全靠 Expect）
        } else if (raw->header.dwType == RIM_TYPEMOUSE) {
            const USHORT flags = raw->data.mouse.usButtonFlags;
            if (flags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) {
                NoteVirtualHidRawMouseWheel();
            } else if (flags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP
                | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_UP
                | RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_UP
                | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP
                | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_5_UP)) {
                NoteVirtualHidRawMouseButton();
            } else {
                NoteVirtualHidRawMouseMove();
            }
        }
        return hint;
    }

    // 真人设备：键盘按下/抬起始终上报（长按启停主路径，不依赖脱离开关）
    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        const USHORT flags = raw->data.keyboard.Flags;
        const bool up = (flags & RI_KEY_BREAK) != 0;
        const UINT vk = static_cast<UINT>(raw->data.keyboard.VKey);
        const unsigned short scan = static_cast<unsigned short>(raw->data.keyboard.MakeCode);
        const bool ext = (flags & RI_KEY_E0) != 0;
        // Interception / SendInput 打上的 ExtraInformation：当作注入，不触发长按松手
        if (IsSyntheticExtraInfo(static_cast<ULONG_PTR>(raw->data.keyboard.ExtraInformation))) {
            if (vk != 0 && vk != 0xFF) {
                NoteVirtualHidRawKeyboardEvent(vk, scan, ext, !up);
                // 勿在此 Consume：Interception ExtraInfo 常只出现在 Raw，信用须留给 LL 钩子
            }
            return hint;
        }
        if (vk != 0 && vk != 0xFF) {
            if (up) {
                hint.physicalKeyUp = true;
                hint.vk = vk;
                hint.msg = WM_KEYUP;
            } else {
                hint.physicalKeyDown = true;
                hint.vk = vk;
                hint.msg = WM_KEYDOWN;
            }
        }
        if (!monitorUserBreakout || !g_session.load(std::memory_order_acquire)) {
            return hint;
        }
        if (!up) {
            if (MatchesKey(vk, scan, ext, true)) return hint;
            hint.userBreakout = true;
            hint.msg = WM_KEYDOWN;
            hint.vk = vk;
        }
        return hint;
    }

    if (raw->header.dwType == RIM_TYPEMOUSE) {
        const USHORT flags = raw->data.mouse.usButtonFlags;
        if (IsSyntheticExtraInfo(static_cast<ULONG_PTR>(raw->data.mouse.ulExtraInformation))) {
            return hint;
        }
        // 真人鼠标抬起始终上报（连点「按住即停」），不被连点/宏的 Note 指纹挡住
        if (flags & RI_MOUSE_LEFT_BUTTON_UP) {
            hint.physicalButtonUp = true; hint.msg = WM_LBUTTONUP; hint.vk = VK_LBUTTON;
        } else if (flags & RI_MOUSE_RIGHT_BUTTON_UP) {
            hint.physicalButtonUp = true; hint.msg = WM_RBUTTONUP; hint.vk = VK_RBUTTON;
        } else if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) {
            hint.physicalButtonUp = true; hint.msg = WM_MBUTTONUP; hint.vk = VK_MBUTTON;
        } else if (flags & RI_MOUSE_BUTTON_4_UP) {
            hint.physicalButtonUp = true; hint.msg = WM_XBUTTONUP; hint.vk = VK_XBUTTON1;
        } else if (flags & RI_MOUSE_BUTTON_5_UP) {
            hint.physicalButtonUp = true; hint.msg = WM_XBUTTONUP; hint.vk = VK_XBUTTON2;
        }

        if (!monitorUserBreakout || !g_session.load(std::memory_order_acquire)) {
            return hint;
        }

        // 脱离判定：指纹命中视为注入回声，但不撤掉上面的 physicalButtonUp
        if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) {
            if (MatchesMouseButton(VK_LBUTTON, true)) return hint;
            hint.userBreakout = true; hint.msg = WM_LBUTTONDOWN; hint.vk = VK_LBUTTON;
        } else if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
            if (MatchesMouseButton(VK_RBUTTON, true)) return hint;
            hint.userBreakout = true; hint.msg = WM_RBUTTONDOWN; hint.vk = VK_RBUTTON;
        } else if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) {
            if (MatchesMouseButton(VK_MBUTTON, true)) return hint;
            hint.userBreakout = true; hint.msg = WM_MBUTTONDOWN; hint.vk = VK_MBUTTON;
        } else if (flags & RI_MOUSE_BUTTON_4_DOWN) {
            if (MatchesMouseButton(VK_XBUTTON1, true)) return hint;
            hint.userBreakout = true; hint.msg = WM_XBUTTONDOWN; hint.vk = VK_XBUTTON1;
        } else if (flags & RI_MOUSE_BUTTON_5_DOWN) {
            if (MatchesMouseButton(VK_XBUTTON2, true)) return hint;
            hint.userBreakout = true; hint.msg = WM_XBUTTONDOWN; hint.vk = VK_XBUTTON2;
        } else if (flags & RI_MOUSE_LEFT_BUTTON_UP) {
            if (MatchesMouseButton(VK_LBUTTON, false)) return hint;
        } else if (flags & RI_MOUSE_RIGHT_BUTTON_UP) {
            if (MatchesMouseButton(VK_RBUTTON, false)) return hint;
        } else if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) {
            if (MatchesMouseButton(VK_MBUTTON, false)) return hint;
        } else if (flags & RI_MOUSE_BUTTON_4_UP) {
            if (MatchesMouseButton(VK_XBUTTON1, false)) return hint;
        } else if (flags & RI_MOUSE_BUTTON_5_UP) {
            if (MatchesMouseButton(VK_XBUTTON2, false)) return hint;
        } else if (flags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) {
            if (MatchesMouseWheel()) return hint;
            hint.userBreakout = true;
            hint.msg = (flags & RI_MOUSE_HWHEEL) ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL;
            hint.vk = 0;
        } else {
            // VirtualHid 绝对移标走 SetCursorPos：其它设备上的 Raw 移动回灌无法可靠区分，
            // 纯移动不判脱离；真人脱离请按键 / 点击 / 滚轮。
            return hint;
        }
    }
    return hint;
}

}  // namespace synthetic_input
