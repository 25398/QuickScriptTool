#include "virtual_hid.h"

#include <vector>
#include <cstring>

#include "../../driver/qst_vhid/qst_vhid_ioctl.h"

#include <setupapi.h>

#pragma comment(lib, "setupapi.lib")

// Match DEFINE_GUID in kernel header
EXTERN_C const GUID GUID_DEVINTERFACE_QSTVHID =
    { 0xa7c3e9f1, 0x4b2d, 0x4e8a, { 0x9c, 0x1f, 0x6d, 0x5e, 0x8a, 0x7b, 0x3c, 0x2d } };

namespace {

HANDLE OpenByInterface() {
    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_QSTVHID, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    HANDLE found = INVALID_HANDLE_VALUE;

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(info, nullptr, &GUID_DEVINTERFACE_QSTVHID, index, &ifData);
         ++index) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifData, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;

        std::vector<BYTE> buf(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &ifData, detail, needed, nullptr, nullptr)) {
            continue;
        }
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            found = h;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(info);
    return found;
}

char ClampI8(int v) {
    if (v > 127) return 127;
    if (v < -127) return -127;
    return static_cast<char>(v);
}

}  // namespace

VirtualHidBackend& VirtualHidBackend::Instance() {
    static VirtualHidBackend inst;
    return inst;
}

unsigned char VirtualHidBackend::ModifierMask(unsigned short scanCode, bool extended) {
    if (!extended) {
        switch (scanCode) {
        case 0x1D: return 0x01; // LCtrl
        case 0x2A: return 0x02; // LShift
        case 0x38: return 0x04; // LAlt
        case 0x36: return 0x20; // RShift
        default: return 0;
        }
    }
    switch (scanCode) {
    case 0x1D: return 0x10; // RCtrl
    case 0x38: return 0x40; // RAlt
    case 0x5B: return 0x08; // LWin
    case 0x5C: return 0x80; // RWin
    default: return 0;
    }
}

unsigned char VirtualHidBackend::ScanToHidUsage(unsigned short scanCode, bool extended) {
    if (ModifierMask(scanCode, extended) != 0) return 0;

    if (extended) {
        switch (scanCode) {
        case 0x48: return 0x52; // Up
        case 0x50: return 0x51; // Down
        case 0x4B: return 0x50; // Left
        case 0x4D: return 0x4F; // Right
        case 0x47: return 0x4A; // Home
        case 0x4F: return 0x4D; // End
        case 0x49: return 0x4B; // PageUp
        case 0x51: return 0x4E; // PageDown
        case 0x52: return 0x49; // Insert
        case 0x53: return 0x4C; // Delete
        case 0x35: return 0x54; // Numpad /
        case 0x1C: return 0x58; // Numpad Enter
        case 0x37: return 0x46; // Print Screen
        case 0x5D: return 0x65; // Application
        case 0x46: return 0x48; // Ctrl+Break → Pause
        default: return 0;
        }
    }

    // Set-1 → HID Keyboard page usage (descriptor allows 0x00..0x65)
    static const unsigned char kMap[128] = {
        /*00*/ 0,    0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, // Esc 1-6
        /*08*/ 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B, // 7-0 - = BS Tab
        /*10*/ 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, // QWERTYUI
        /*18*/ 0x12, 0x13, 0x2F, 0x30, 0x28, 0,    0x04, 0x16, // OP[] Enter Ctrl A S
        /*20*/ 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, // DFGHJKL ;
        /*28*/ 0x34, 0x35, 0,    0x31, 0x1D, 0x1B, 0x06, 0x19, // '" LShift \ ZXCV
        /*30*/ 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0,    0x55, // BNM ,./ RShift KP*
        /*38*/ 0,    0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, // LAlt Space Caps F1-F5
        /*40*/ 0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F, // F6-F10 NumLock Scroll KP7
        /*48*/ 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59, // KP8-9 - KP4-6 + KP1
        /*50*/ 0x5A, 0x5B, 0x62, 0x63, 0,    0,    0,    0x44, // KP2-3 0 .     F11
        /*58*/ 0x45, 0,    0,    0,    0x46, 0,    0,    0,    // F12       SysRq
        /*60*/ 0,    0,    0,    0,    0,    0,    0,    0,
        /*68*/ 0,    0,    0,    0,    0,    0,    0,    0,
        /*70*/ 0,    0,    0,    0,    0,    0,    0,    0,
        /*78*/ 0,    0,    0,    0,    0,    0,    0,    0,
    };
    if (scanCode < 128) return kMap[scanCode];
    return 0;
}

void VirtualHidBackend::CompactKeysLocked() {
    unsigned char packed[6]{};
    int n = 0;
    for (int i = 0; i < 6; ++i) {
        if (keys_[i] != 0) packed[n++] = keys_[i];
    }
    std::memcpy(keys_, packed, sizeof(keys_));
}

bool VirtualHidBackend::KeyboardStateChangedLocked() const {
    if (modifiers_ != lastSentModifiers_) return true;
    for (int i = 0; i < 6; ++i) {
        if (keys_[i] != lastSentKeys_[i]) return true;
    }
    return false;
}

bool VirtualHidBackend::SubmitReportLocked(unsigned char reportId, const unsigned char* data,
    unsigned char len) {
    if (!device_ || device_ == INVALID_HANDLE_VALUE) {
        lastError_ = L"VirtualHid device not open";
        return false;
    }
    QSTVHID_SUBMIT_REPORT req{};
    req.ReportId = reportId;
    req.Length = len;
    std::memcpy(req.Data, data, len);

    DWORD returned = 0;
    const DWORD ioctl = static_cast<DWORD>(IOCTL_QSTVHID_SUBMIT_REPORT);
    if (!DeviceIoControl(static_cast<HANDLE>(device_), ioctl,
            &req, static_cast<DWORD>(sizeof(req)), nullptr, 0, &returned, nullptr)) {
        lastError_ = L"DeviceIoControl IOCTL_QSTVHID_SUBMIT_REPORT failed, gle="
            + std::to_wstring(GetLastError());
        return false;
    }
    return true;
}

bool VirtualHidBackend::SubmitKeyboardLocked() {
    CompactKeysLocked();
    if (!KeyboardStateChangedLocked()) return true;

    unsigned char report[QSTVHID_REPORT_LEN_KEYBOARD]{};
    report[0] = QSTVHID_REPORT_ID_KEYBOARD;
    report[1] = modifiers_;
    report[2] = 0;
    for (int i = 0; i < 6; ++i) report[3 + i] = keys_[i];
    if (!SubmitReportLocked(QSTVHID_REPORT_ID_KEYBOARD, report, QSTVHID_REPORT_LEN_KEYBOARD)) {
        return false;
    }
    lastSentModifiers_ = modifiers_;
    std::memcpy(lastSentKeys_, keys_, sizeof(keys_));
    return true;
}

bool VirtualHidBackend::SubmitMouseRelLocked(unsigned char buttons, char dx, char dy,
    char wheel, char hwheel) {
    unsigned char report[6]{};
    report[0] = QSTVHID_REPORT_ID_MOUSE_REL;
    report[1] = buttons;
    report[2] = static_cast<unsigned char>(dx);
    report[3] = static_cast<unsigned char>(dy);
    report[4] = static_cast<unsigned char>(wheel);
    report[5] = static_cast<unsigned char>(hwheel);
    return SubmitReportLocked(QSTVHID_REPORT_ID_MOUSE_REL, report, 6);
}

bool VirtualHidBackend::SubmitMouseAbsLocked(unsigned char buttons, unsigned short x,
    unsigned short y) {
    unsigned char report[QSTVHID_REPORT_LEN_MOUSE_ABS]{};
    report[0] = QSTVHID_REPORT_ID_MOUSE_ABS;
    report[1] = buttons;
    report[2] = static_cast<unsigned char>(x & 0xFF);
    report[3] = static_cast<unsigned char>((x >> 8) & 0xFF);
    report[4] = static_cast<unsigned char>(y & 0xFF);
    report[5] = static_cast<unsigned char>((y >> 8) & 0xFF);
    return SubmitReportLocked(QSTVHID_REPORT_ID_MOUSE_ABS, report, QSTVHID_REPORT_LEN_MOUSE_ABS);
}

bool VirtualHidBackend::SubmitMouseButtonsLocked() {
    // 按钮只发相对零位移，避免绝对报告把光标按主屏映射拽偏。
    return SubmitMouseRelLocked(mouseButtons_, 0, 0, 0, 0);
}

bool VirtualHidBackend::ProbeAvailable(std::wstring* errorOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    HANDLE h = OpenByInterface();
    if (h == INVALID_HANDLE_VALUE) {
        lastError_ = L"未找到虚拟 HID 设备（请先在设置中安装虚拟 HID 驱动）";
        if (errorOut) *errorOut = lastError_;
        return false;
    }
    CloseHandle(h);
    lastError_.clear();
    return true;
}

bool VirtualHidBackend::Open(std::wstring* errorOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_ && device_ != INVALID_HANDLE_VALUE) return true;
    HANDLE h = OpenByInterface();
    if (h == INVALID_HANDLE_VALUE) {
        lastError_ = L"打开虚拟 HID 失败（请确认已安装驱动）";
        if (errorOut) *errorOut = lastError_;
        return false;
    }
    device_ = h;
    modifiers_ = 0;
    std::memset(keys_, 0, sizeof(keys_));
    std::memset(lastSentKeys_, 0, sizeof(lastSentKeys_));
    lastSentModifiers_ = 0xFF; // force first submit
    mouseButtons_ = 0;
    pointerMode_ = PointerMode::Relative;
    lastAbsX_ = 0;
    lastAbsY_ = 0;
    lastScreenX_ = 0;
    lastScreenY_ = 0;
    lastError_.clear();
    return true;
}

void VirtualHidBackend::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    // 关句柄前尽量抬起；进程被杀时仍依赖驱动 FileCleanup 发空报告。
    modifiers_ = 0;
    std::memset(keys_, 0, sizeof(keys_));
    mouseButtons_ = 0;
    if (device_ && device_ != INVALID_HANDLE_VALUE) {
        lastSentModifiers_ = 0xFF; // force submit even if last sent was already zero
        (void)SubmitKeyboardLocked();
        (void)SubmitMouseButtonsLocked();
        CloseHandle(static_cast<HANDLE>(device_));
    }
    device_ = nullptr;
    std::memset(lastSentKeys_, 0, sizeof(lastSentKeys_));
    lastSentModifiers_ = 0;
    pointerMode_ = PointerMode::Relative;
    lastAbsX_ = 0;
    lastAbsY_ = 0;
    lastScreenX_ = 0;
    lastScreenY_ = 0;
}

bool VirtualHidBackend::IsOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ != nullptr && device_ != INVALID_HANDLE_VALUE;
}

bool VirtualHidBackend::SendKey(unsigned short scanCode, bool down, bool extended) {
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned char mod = ModifierMask(scanCode, extended);
    if (mod != 0) {
        if (down) modifiers_ |= mod;
        else modifiers_ = static_cast<unsigned char>(modifiers_ & ~mod);
        return SubmitKeyboardLocked();
    }
    const unsigned char usage = ScanToHidUsage(scanCode, extended);
    if (usage == 0) {
        lastError_ = L"unsupported scan code for VirtualHid (scan=0x"
            + std::to_wstring(scanCode) + (extended ? L" E0)" : L")");
        return false;
    }
    if (down) {
        for (int i = 0; i < 6; ++i) {
            if (keys_[i] == usage) return SubmitKeyboardLocked();
        }
        int empty = -1;
        for (int i = 0; i < 6; ++i) {
            if (keys_[i] == 0) { empty = i; break; }
        }
        if (empty < 0) {
            // 满槽：挤掉最旧，贴近部分实体键盘丢键行为（仍非 NKRO）
            for (int i = 0; i < 5; ++i) keys_[i] = keys_[i + 1];
            keys_[5] = usage;
        } else {
            keys_[empty] = usage;
        }
    } else {
        for (int i = 0; i < 6; ++i) {
            if (keys_[i] == usage) keys_[i] = 0;
        }
        CompactKeysLocked();
    }
    return SubmitKeyboardLocked();
}

bool VirtualHidBackend::MoveRelative(int dx, int dy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dx == 0 && dy == 0) return true;
    // 单步位移上限：畸形脚本给 INT_MIN 时下方 while 会循环数百万次 DeviceIoControl。
    constexpr int kMaxDelta = 32767;
    if (dx > kMaxDelta) dx = kMaxDelta;
    if (dx < -kMaxDelta) dx = -kMaxDelta;
    if (dy > kMaxDelta) dy = kMaxDelta;
    if (dy < -kMaxDelta) dy = -kMaxDelta;
    pointerMode_ = PointerMode::Relative;
    while (dx != 0 || dy != 0) {
        const char sx = ClampI8(dx);
        const char sy = ClampI8(dy);
        if (!SubmitMouseRelLocked(mouseButtons_, sx, sy, 0, 0)) return false;
        dx -= sx;
        dy -= sy;
    }
    return true;
}

bool VirtualHidBackend::MoveAbsoluteScreen(int screenX, int screenY) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 不向系统提交 HID 绝对报告：mouhid 多屏常按主屏映射，且与 SetCursorPos 异步打架导致轨迹乱漂。
    // 桌面绝对回放由 ForegroundInputRouter::SetCursorScreen → SetCursorPos 落像素；
    // 点击/滚轮仍走相对 HID 报告（见 SubmitMouseButtonsLocked / Wheel）。
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw > 1 && vh > 1) {
        long long nx64 = (static_cast<long long>(screenX - vx) * 65535) / (vw - 1);
        long long ny64 = (static_cast<long long>(screenY - vy) * 65535) / (vh - 1);
        if (nx64 < 0) nx64 = 0;
        if (ny64 < 0) ny64 = 0;
        if (nx64 > 65535) nx64 = 65535;
        if (ny64 > 65535) ny64 = 65535;
        lastAbsX_ = static_cast<unsigned short>(nx64);
        lastAbsY_ = static_cast<unsigned short>(ny64);
    }
    pointerMode_ = PointerMode::Absolute;
    lastScreenX_ = screenX;
    lastScreenY_ = screenY;
    lastError_.clear();
    return true;
}

bool VirtualHidBackend::Button(MouseButtonType button, bool down) {
    std::lock_guard<std::mutex> lock(mutex_);
    unsigned char bit = 0x01;
    switch (button) {
    case MouseButtonType::Right: bit = 0x02; break;
    case MouseButtonType::Middle: bit = 0x04; break;
    case MouseButtonType::X1: bit = 0x08; break;
    case MouseButtonType::X2: bit = 0x10; break;
    case MouseButtonType::Left:
    default: bit = 0x01; break;
    }
    if (down) mouseButtons_ |= bit;
    else mouseButtons_ = static_cast<unsigned char>(mouseButtons_ & ~bit);
    return SubmitMouseButtonsLocked();
}

bool VirtualHidBackend::Wheel(int delta, bool horizontal) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 实体鼠标：滚轮脉冲后下一帧 wheel=0；只发非零容易被部分程序当成“粘住”。
    pointerMode_ = PointerMode::Relative;
    int notches = delta / 120;
    if (notches == 0 && delta != 0) notches = (delta > 0) ? 1 : -1;
    const char w = ClampI8(notches);
    if (horizontal) {
        if (!SubmitMouseRelLocked(mouseButtons_, 0, 0, 0, w)) return false;
        return SubmitMouseRelLocked(mouseButtons_, 0, 0, 0, 0);
    }
    if (!SubmitMouseRelLocked(mouseButtons_, 0, 0, w, 0)) return false;
    return SubmitMouseRelLocked(mouseButtons_, 0, 0, 0, 0);
}

bool VirtualHidBackend::ReleaseAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    modifiers_ = 0;
    std::memset(keys_, 0, sizeof(keys_));
    mouseButtons_ = 0;
    bool ok = true;
    if (device_ && device_ != INVALID_HANDLE_VALUE) {
        ok = SubmitKeyboardLocked() && SubmitMouseButtonsLocked();
    }
    return ok;
}

std::wstring VirtualHidBackend::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}
