#include "hid_interception.h"

#include <windows.h>

#include <cstring>
#include <mutex>

namespace {

// 与 third_party/interception/interception.h 布局一致；运行时 GetProcAddress，避免 dllimport 链接。
using InterceptionContext = void*;
using InterceptionDevice = int;

enum InterceptionKeyState : unsigned short {
    INTERCEPTION_KEY_DOWN = 0x00,
    INTERCEPTION_KEY_UP = 0x01,
    INTERCEPTION_KEY_E0 = 0x02,
};

enum InterceptionMouseState : unsigned short {
    INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN = 0x001,
    INTERCEPTION_MOUSE_LEFT_BUTTON_UP = 0x002,
    INTERCEPTION_MOUSE_RIGHT_BUTTON_DOWN = 0x004,
    INTERCEPTION_MOUSE_RIGHT_BUTTON_UP = 0x008,
    INTERCEPTION_MOUSE_MIDDLE_BUTTON_DOWN = 0x010,
    INTERCEPTION_MOUSE_MIDDLE_BUTTON_UP = 0x020,
    INTERCEPTION_MOUSE_BUTTON_4_DOWN = 0x040,
    INTERCEPTION_MOUSE_BUTTON_4_UP = 0x080,
    INTERCEPTION_MOUSE_BUTTON_5_DOWN = 0x100,
    INTERCEPTION_MOUSE_BUTTON_5_UP = 0x200,
    INTERCEPTION_MOUSE_WHEEL = 0x400,
    INTERCEPTION_MOUSE_HWHEEL = 0x800,
};

enum InterceptionMouseFlag : unsigned short {
    INTERCEPTION_MOUSE_MOVE_RELATIVE = 0x000,
    INTERCEPTION_MOUSE_MOVE_ABSOLUTE = 0x001,
    INTERCEPTION_MOUSE_VIRTUAL_DESKTOP = 0x002,
    INTERCEPTION_MOUSE_MOVE_NOCOALESCE = 0x008,
};

struct InterceptionMouseStroke {
    unsigned short state;
    unsigned short flags;
    short rolling;
    int x;
    int y;
    unsigned int information;
};

struct InterceptionKeyStroke {
    unsigned short code;
    unsigned short state;
    unsigned int information;
};

using InterceptionStroke = char[sizeof(InterceptionMouseStroke)];

using FnCreateContext = InterceptionContext (*)();
using FnDestroyContext = void (*)(InterceptionContext);
using FnSend = int (*)(InterceptionContext, InterceptionDevice, const InterceptionStroke*, unsigned int);
using FnIsKeyboard = int (*)(InterceptionDevice);
using FnIsMouse = int (*)(InterceptionDevice);
using FnIsInvalid = int (*)(InterceptionDevice);
using FnGetHardwareId = unsigned int (*)(InterceptionContext, InterceptionDevice, void*, unsigned int);

constexpr int kMaxKeyboard = 10;
constexpr int kMaxMouse = 10;
constexpr int kMaxDevice = kMaxKeyboard + kMaxMouse;
inline int KeyboardDevice(int index) { return index + 1; }
inline int MouseDevice(int index) { return kMaxKeyboard + index + 1; }

struct ApiTable {
    HMODULE module = nullptr;
    FnCreateContext create_context = nullptr;
    FnDestroyContext destroy_context = nullptr;
    FnSend send = nullptr;
    FnIsKeyboard is_keyboard = nullptr;
    FnIsMouse is_mouse = nullptr;
    FnIsInvalid is_invalid = nullptr;
    FnGetHardwareId get_hardware_id = nullptr;
};

ApiTable g_api;
std::once_flag g_loadOnce;
std::wstring g_loadError;

bool LoadApiOnce() {
    std::call_once(g_loadOnce, []() {
        wchar_t path[MAX_PATH]{};
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            g_loadError = L"GetModuleFileName failed";
            return;
        }
        std::wstring dir(path);
        const auto slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir.resize(slash + 1);
        const std::wstring dllPath = dir + L"interception.dll";
        g_api.module = LoadLibraryW(dllPath.c_str());
        if (!g_api.module) {
            g_api.module = LoadLibraryW(L"interception.dll");
        }
        if (!g_api.module) {
            g_loadError = L"无法加载 interception.dll（请将其放在程序目录）";
            return;
        }
        auto load = [&](const char* name) -> FARPROC {
            return GetProcAddress(g_api.module, name);
        };
        g_api.create_context = reinterpret_cast<FnCreateContext>(load("interception_create_context"));
        g_api.destroy_context = reinterpret_cast<FnDestroyContext>(load("interception_destroy_context"));
        g_api.send = reinterpret_cast<FnSend>(load("interception_send"));
        g_api.is_keyboard = reinterpret_cast<FnIsKeyboard>(load("interception_is_keyboard"));
        g_api.is_mouse = reinterpret_cast<FnIsMouse>(load("interception_is_mouse"));
        g_api.is_invalid = reinterpret_cast<FnIsInvalid>(load("interception_is_invalid"));
        g_api.get_hardware_id = reinterpret_cast<FnGetHardwareId>(load("interception_get_hardware_id"));
        if (!g_api.create_context || !g_api.destroy_context || !g_api.send
            || !g_api.is_keyboard || !g_api.is_mouse || !g_api.is_invalid) {
            g_loadError = L"interception.dll 导出符号不完整";
            FreeLibrary(g_api.module);
            g_api = {};
            return;
        }
    });
    return g_api.module != nullptr && g_api.create_context != nullptr;
}

}  // namespace

HidInterceptionBackend& HidInterceptionBackend::Instance() {
    static HidInterceptionBackend inst;
    return inst;
}

bool HidInterceptionBackend::EnsureApiLoaded(std::wstring* errorOut) {
    if (!LoadApiOnce()) {
        lastError_ = g_loadError.empty() ? L"Interception API 加载失败" : g_loadError;
        if (errorOut) *errorOut = lastError_;
        return false;
    }
    return true;
}

bool HidInterceptionBackend::ResolveDevicesLocked(std::wstring* errorOut) {
    keyboardDevice_ = 0;
    mouseDevice_ = 0;
    for (int i = 0; i < kMaxKeyboard; ++i) {
        const int dev = KeyboardDevice(i);
        if (g_api.is_invalid(dev)) continue;
        if (!g_api.is_keyboard(dev)) continue;
        // 有硬件 ID 的设备优先（过滤空槽）
        if (g_api.get_hardware_id) {
            wchar_t hw[256]{};
            const unsigned int n = g_api.get_hardware_id(
                context_, dev, hw, static_cast<unsigned int>(sizeof(hw)));
            if (n == 0) continue;
        }
        keyboardDevice_ = dev;
        break;
    }
    for (int i = 0; i < kMaxMouse; ++i) {
        const int dev = MouseDevice(i);
        if (g_api.is_invalid(dev)) continue;
        if (!g_api.is_mouse(dev)) continue;
        if (g_api.get_hardware_id) {
            wchar_t hw[256]{};
            const unsigned int n = g_api.get_hardware_id(
                context_, dev, hw, static_cast<unsigned int>(sizeof(hw)));
            if (n == 0) continue;
        }
        mouseDevice_ = dev;
        break;
    }
    // 若硬件 ID 全空（部分环境），回退到第一个 keyboard/mouse 槽位
    if (keyboardDevice_ == 0) {
        for (int i = 0; i < kMaxKeyboard; ++i) {
            const int dev = KeyboardDevice(i);
            if (!g_api.is_invalid(dev) && g_api.is_keyboard(dev)) {
                keyboardDevice_ = dev;
                break;
            }
        }
    }
    if (mouseDevice_ == 0) {
        for (int i = 0; i < kMaxMouse; ++i) {
            const int dev = MouseDevice(i);
            if (!g_api.is_invalid(dev) && g_api.is_mouse(dev)) {
                mouseDevice_ = dev;
                break;
            }
        }
    }
    if (keyboardDevice_ == 0 || mouseDevice_ == 0) {
        lastError_ = L"未找到可用的 Interception 键盘/鼠标设备（请确认已安装驱动并重启）";
        if (errorOut) *errorOut = lastError_;
        return false;
    }
    return true;
}

bool HidInterceptionBackend::ProbeAvailable(std::wstring* errorOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureApiLoaded(errorOut)) return false;
    InterceptionContext ctx = g_api.create_context();
    if (!ctx) {
        lastError_ = L"Interception 驱动未就绪（请以管理员运行 install-interception.exe 后重启）";
        if (errorOut) *errorOut = lastError_;
        return false;
    }
    // 临时挂到 context_ 以便 ResolveDevices
    context_ = ctx;
    const bool ok = ResolveDevicesLocked(errorOut);
    g_api.destroy_context(ctx);
    context_ = nullptr;
    keyboardDevice_ = 0;
    mouseDevice_ = 0;
    return ok;
}

bool HidInterceptionBackend::Open(std::wstring* errorOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_) return true;
    if (!EnsureApiLoaded(errorOut)) return false;
    context_ = g_api.create_context();
    if (!context_) {
        lastError_ = L"Interception 驱动未就绪（请以管理员运行 install-interception.exe 后重启）";
        if (errorOut) *errorOut = lastError_;
        return false;
    }
    if (!ResolveDevicesLocked(errorOut)) {
        g_api.destroy_context(context_);
        context_ = nullptr;
        keyboardDevice_ = 0;
        mouseDevice_ = 0;
        return false;
    }
    lastError_.clear();
    return true;
}

void HidInterceptionBackend::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_ && g_api.destroy_context) {
        g_api.destroy_context(context_);
    }
    context_ = nullptr;
    keyboardDevice_ = 0;
    mouseDevice_ = 0;
}

bool HidInterceptionBackend::IsOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return context_ != nullptr;
}

bool HidInterceptionBackend::SendKey(unsigned short scanCode, bool down, bool extended) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ || keyboardDevice_ == 0) {
        lastError_ = L"HID keyboard context not open";
        return false;
    }
    InterceptionKeyStroke key{};
    key.code = scanCode;
    key.state = down ? INTERCEPTION_KEY_DOWN : INTERCEPTION_KEY_UP;
    if (extended) key.state |= INTERCEPTION_KEY_E0;
    InterceptionStroke stroke{};
    std::memcpy(stroke, &key, sizeof(key));
    const int n = g_api.send(context_, keyboardDevice_, &stroke, 1);
    if (n != 1) {
        lastError_ = L"interception_send keyboard failed";
        return false;
    }
    return true;
}

bool HidInterceptionBackend::SendMouseStrokeLocked(unsigned short state, unsigned short flags,
    short rolling, int x, int y) {
    if (!context_ || mouseDevice_ == 0) {
        lastError_ = L"HID mouse context not open";
        return false;
    }
    InterceptionMouseStroke mouse{};
    mouse.state = state;
    mouse.flags = flags;
    mouse.rolling = rolling;
    mouse.x = x;
    mouse.y = y;
    InterceptionStroke stroke{};
    std::memcpy(stroke, &mouse, sizeof(mouse));
    const int n = g_api.send(context_, mouseDevice_, &stroke, 1);
    if (n != 1) {
        lastError_ = L"interception_send mouse failed";
        return false;
    }
    return true;
}

bool HidInterceptionBackend::MoveRelative(int dx, int dy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dx == 0 && dy == 0) return true;
    // 与 SendInput 的 MOUSEEVENTF_MOVE_NOCOALESCE 对齐：避免系统合并报告，
    // 否则相对录制回放的「包次数/包大小」会偏离 Raw 采样，游戏视角会慢慢漂。
    const unsigned short flags = static_cast<unsigned short>(
        INTERCEPTION_MOUSE_MOVE_RELATIVE | INTERCEPTION_MOUSE_MOVE_NOCOALESCE);
    return SendMouseStrokeLocked(0, flags, 0, dx, dy);
}

bool HidInterceptionBackend::MoveAbsoluteScreen(int screenX, int screenY) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) {
        lastError_ = L"invalid virtual screen metrics";
        return false;
    }
    // 与 SendInput MOUSEEVENTF_VIRTUALDESK 一致：归一化到 0..65535
    const int normX = static_cast<int>(
        (static_cast<long long>(screenX - vx) * 65535) / (vw - 1));
    const int normY = static_cast<int>(
        (static_cast<long long>(screenY - vy) * 65535) / (vh - 1));
    const unsigned short flags = static_cast<unsigned short>(
        INTERCEPTION_MOUSE_MOVE_ABSOLUTE | INTERCEPTION_MOUSE_VIRTUAL_DESKTOP);
    return SendMouseStrokeLocked(0, flags, 0, normX, normY);
}

bool HidInterceptionBackend::Button(MouseButtonType button, bool down) {
    std::lock_guard<std::mutex> lock(mutex_);
    unsigned short state = 0;
    switch (button) {
    case MouseButtonType::Right:
        state = down ? INTERCEPTION_MOUSE_RIGHT_BUTTON_DOWN
                     : INTERCEPTION_MOUSE_RIGHT_BUTTON_UP;
        break;
    case MouseButtonType::Middle:
        state = down ? INTERCEPTION_MOUSE_MIDDLE_BUTTON_DOWN
                     : INTERCEPTION_MOUSE_MIDDLE_BUTTON_UP;
        break;
    case MouseButtonType::X1:
        state = down ? INTERCEPTION_MOUSE_BUTTON_4_DOWN
                     : INTERCEPTION_MOUSE_BUTTON_4_UP;
        break;
    case MouseButtonType::X2:
        state = down ? INTERCEPTION_MOUSE_BUTTON_5_DOWN
                     : INTERCEPTION_MOUSE_BUTTON_5_UP;
        break;
    case MouseButtonType::Left:
    default:
        state = down ? INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN
                     : INTERCEPTION_MOUSE_LEFT_BUTTON_UP;
        break;
    }
    return SendMouseStrokeLocked(state, INTERCEPTION_MOUSE_MOVE_RELATIVE, 0, 0, 0);
}

bool HidInterceptionBackend::Wheel(int delta, bool horizontal) {
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned short state = horizontal
        ? INTERCEPTION_MOUSE_HWHEEL : INTERCEPTION_MOUSE_WHEEL;
    return SendMouseStrokeLocked(state, INTERCEPTION_MOUSE_MOVE_RELATIVE,
        static_cast<short>(delta), 0, 0);
}

std::wstring HidInterceptionBackend::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}
