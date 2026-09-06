#include "background_window_input.h"

#include "action_utils.h"
#include "background_uia_input.h"
#include "background_input_target.h"
#include "window_coords.h"
#include "window_mode_log.h"
#include "window_mode_types.h"
#include "window_target.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace windowmode {

namespace {

struct FindTextInputContext {
    HWND found = nullptr;
};

/// Logical button flags for message-based drag (MK_LBUTTON etc.).
WPARAM g_softMouseFlags = 0;
bool g_haveLastClientPos = false;
int g_lastClientX = 0;
int g_lastClientY = 0;
HWND g_lastClientHwnd = nullptr;
/// 软按键按下态（后台 PostMessage 路径自维护；物理 GetKeyState 看不到脚本修饰键）。
bool g_softVkDown[256] = {};

bool SoftIsVkDown(UINT vk) {
    return vk < 256 && g_softVkDown[vk];
}

void SoftSetVkDown(UINT vk, bool down) {
    if (vk >= 256) return;
    g_softVkDown[vk] = down;
    auto syncSide = [&](UINT left, UINT right, UINT generic) {
        if (vk == left || vk == right) {
            g_softVkDown[generic] = g_softVkDown[left] || g_softVkDown[right];
        } else if (vk == generic && !down) {
            g_softVkDown[left] = false;
            g_softVkDown[right] = false;
            g_softVkDown[generic] = false;
        }
    };
    syncSide(VK_LSHIFT, VK_RSHIFT, VK_SHIFT);
    syncSide(VK_LCONTROL, VK_RCONTROL, VK_CONTROL);
    syncSide(VK_LMENU, VK_RMENU, VK_MENU);
}

void ResetSoftKeyState() {
    std::memset(g_softVkDown, 0, sizeof(g_softVkDown));
}

bool IsTextInputClass(const wchar_t* cls) {
    static const wchar_t* kClasses[] = {
        L"Edit",
        L"RichEdit20W",
        L"RichEdit50W",
        L"RichEditD2DPT",
        L"Scintilla",
        L"TX_TELEdit",
        L"TMemo",
        L"ThunderRT6TextBox",
    };
    for (const wchar_t* name : kClasses) {
        if (_wcsicmp(cls, name) == 0) return true;
    }
    return false;
}

BOOL CALLBACK FindTextInputProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindTextInputContext*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (IsTextInputClass(cls)) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

WPARAM ModifierKeyFlags() {
    // 后台 soft 路径必须看脚本软按键态；物理 GetKeyState 会漏掉脚本 Ctrl/Shift，
    // 也会把用户正在按的修饰键污染进目标窗口消息。
    WPARAM flags = 0;
    if (SoftIsVkDown(VK_CONTROL) || SoftIsVkDown(VK_LCONTROL) || SoftIsVkDown(VK_RCONTROL)) {
        flags |= MK_CONTROL;
    }
    if (SoftIsVkDown(VK_SHIFT) || SoftIsVkDown(VK_LSHIFT) || SoftIsVkDown(VK_RSHIFT)) {
        flags |= MK_SHIFT;
    }
    return flags;
}

void RememberClientPos(HWND hwnd, int cx, int cy) {
    g_haveLastClientPos = true;
    g_lastClientX = cx;
    g_lastClientY = cy;
    g_lastClientHwnd = hwnd;
}

bool OpenClipboardRetry() {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(nullptr)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/// Walk to the deepest child under (cx,cy) in root client space; remap coords to that child.
HWND ResolveMouseTarget(HWND root, int& cx, int& cy) {
    if (!root || !IsWindow(root)) return nullptr;

    HWND current = root;
    POINT pt{cx, cy};

    for (int depth = 0; depth < 32; ++depth) {
        HWND child = ChildWindowFromPointEx(current, pt,
            CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == current) {
            child = RealChildWindowFromPoint(current, pt);
        }
        if (!child || child == current) break;

        MapWindowPoints(current, child, &pt, 1);
        current = child;
    }

    cx = pt.x;
    cy = pt.y;
    return current;
}

bool NotifyWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    return SendNotifyMessageW(hwnd, msg, wp, lp) != FALSE;
}

bool IsMouseWindowMessage(UINT msg) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return true;
    default:
        return false;
    }
}

/// MuMu 等 Qt 壳：键盘用同步 SendMessage；鼠标须异步（轨迹宏上百次 MOVE 同步会卡死/崩模拟器）。
bool DeliverWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND top = TopLevelTargetWindow(hwnd);
    if (IsAndroidEmulatorTarget(top ? top : hwnd, nullptr)) {
        if (IsMouseWindowMessage(msg)) {
            return NotifyWindowMessage(hwnd, msg, wp, lp);
        }
        DWORD_PTR res = 0;
        return SendMessageTimeoutW(hwnd, msg, wp, lp,
            SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &res) != 0;
    }
    return NotifyWindowMessage(hwnd, msg, wp, lp);
}

/// 若坐标超出子窗客户区但符合顶层窗，从顶层映射到子窗（录制绑顶层、投递子窗时）。
void MaybeRemapTopClientToPostTarget(HWND bindHwnd, HWND& postHwnd, int& hitX, int& hitY) {
    if (!bindHwnd || !postHwnd || bindHwnd == postHwnd) return;
    RECT childRc{};
    if (!GetClientRect(postHwnd, &childRc)) return;
    const int cw = std::max(0, static_cast<int>(childRc.right - childRc.left));
    const int ch = std::max(0, static_cast<int>(childRc.bottom - childRc.top));
    if (hitX >= 0 && hitY >= 0 && hitX < cw && hitY < ch) return;

    HWND top = TopLevelTargetWindow(bindHwnd);
    if (!top || top == postHwnd) return;
    RECT topRc{};
    if (!GetClientRect(top, &topRc)) return;
    const int tw = std::max(0, static_cast<int>(topRc.right - topRc.left));
    const int th = std::max(0, static_cast<int>(topRc.bottom - topRc.top));
    if (hitX < 0 || hitY < 0 || hitX >= tw || hitY >= th) return;
    MapClientPointBetweenHwnds(top, postHwnd, hitX, hitY);
}

/// 目标窗口挂死时用超时发送，避免脚本/UI 线程无限阻塞。
LRESULT SendMessageTimeoutSafe(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!hwnd || !IsWindow(hwnd)) return 0;
    DWORD_PTR res = 0;
    SendMessageTimeoutW(hwnd, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &res);
    return static_cast<LRESULT>(res);
}

bool SendQuickInputViaEditMessages(HWND input, const std::wstring& text, double charInterval,
    const std::atomic_bool* cancelFlag) {
    if (!input || !IsWindow(input) || text.empty()) return false;

    // EM_REPLACESEL 跨进程不封送指针：目标进程会按自身地址空间解引用 lParam，
    // 轻则读垃圾重则 0xC0000005 崩溃。跨进程一律走剪贴板/前台回退路径。
    DWORD pid = 0;
    GetWindowThreadProcessId(input, &pid);
    if (pid != GetCurrentProcessId()) return false;

    wchar_t cls[256]{};
    GetClassNameW(input, cls, 256);
    if (!IsTextInputClass(cls)) return false;

    if (charInterval <= 0) {
        if (WindowModeCancelled(cancelFlag)) return true;
        SendMessageTimeoutSafe(input, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        // EM_REPLACESEL 无文档化成功返回值（常为 0），已发送即视为成功
        SendMessageTimeoutSafe(input, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
        return true;
    }

    for (wchar_t ch : text) {
        if (WindowModeCancelled(cancelFlag)) return true;
        if (ch == L'\r' || ch == L'\n') {
            SendMessageTimeoutSafe(input, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\r\n"));
        } else {
            std::wstring one(1, ch);
            SendMessageTimeoutSafe(input, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(one.c_str()));
        }
        WindowModeSleepInterruptible(cancelFlag,
            std::chrono::milliseconds(static_cast<int>(charInterval * 1000.0)));
    }
    return true;
}

/// Temporarily replace CF_UNICODETEXT; restore previous content on destruction.
class ScopedClipboardUnicodeText {
public:
    explicit ScopedClipboardUnicodeText(const std::wstring& text) {
        if (text.empty() || !OpenClipboardRetry()) return;

        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (HANDLE existing = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* src = static_cast<const wchar_t*>(GlobalLock(existing))) {
                    savedText_.assign(src);
                    GlobalUnlock(existing);
                    hadSaved_ = true;
                }
            }
        }

        if (!EmptyClipboard()) {
            CloseClipboard();
            return;
        }

        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!mem) {
            CloseClipboard();
            return;
        }
        void* ptr = GlobalLock(mem);
        if (!ptr) {
            GlobalFree(mem);
            CloseClipboard();
            return;
        }
        std::memcpy(ptr, text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) {
            GlobalFree(mem);
            CloseClipboard();
            return;
        }

        ok_ = true;
        CloseClipboard();
    }

    ~ScopedClipboardUnicodeText() {
        if (!ok_) return;
        if (!OpenClipboardRetry()) return;

        EmptyClipboard();
        if (hadSaved_) {
            const size_t bytes = (savedText_.size() + 1) * sizeof(wchar_t);
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (mem) {
                if (void* ptr = GlobalLock(mem)) {
                    std::memcpy(ptr, savedText_.c_str(), bytes);
                    GlobalUnlock(mem);
                    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
                        GlobalFree(mem);
                    }
                } else {
                    GlobalFree(mem);
                }
            }
        }
        CloseClipboard();
    }

    bool Ok() const { return ok_; }

private:
    bool ok_ = false;
    bool hadSaved_ = false;
    std::wstring savedText_;
};

bool SendQuickInputViaClipboard(HWND input, const std::wstring& text, bool /*usePostMessage*/) {
    if (!input || !IsWindow(input) || text.empty()) return false;

    ScopedClipboardUnicodeText clip(text);
    if (!clip.Ok()) return false;

    // 同步粘贴：须在 ScopedClipboardUnicodeText 析构恢复剪贴板之前完成。
    // 勿 PostMessage(WM_PASTE)，否则异步粘贴时剪贴板可能已还原，导致重复/错贴。
    SendMessageTimeoutSafe(input, WM_PASTE, 0, 0);
    return true;
}

LPARAM BuildKeyLParam(UINT vk, bool down) {
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lParam = 1;
    if (scan) lParam |= static_cast<LPARAM>(scan) << 16;
    // 与 SendKeyboardKey 一致：方向/编辑键标扩展位，避免被当成小键盘
    switch (vk) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE:
    case VK_DIVIDE: case VK_NUMLOCK:
    case VK_RCONTROL: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_SNAPSHOT:
        lParam |= (1 << 24); // KF_EXTENDED
        break;
    default:
        break;
    }
    if (down) return lParam;
    lParam |= (1 << 30) | (static_cast<LPARAM>(1) << 31);
    return lParam;
}

/// 用软修饰键态把 VK 译成字符（后台路径看不到物理 GetKeyState 的脚本 Shift）。
wchar_t SoftVkToChar(UINT vk) {
    switch (vk) {
    case VK_SPACE: return L' ';
    case VK_RETURN: return L'\r';
    case VK_TAB: return L'\t';
    case VK_ESCAPE:
    case VK_BACK:
    case VK_DELETE:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_INSERT:
    case VK_LWIN: case VK_RWIN:
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
    case VK_LMENU: case VK_RMENU: case VK_MENU:
    case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
    case VK_F1: case VK_F2: case VK_F3: case VK_F4:
    case VK_F5: case VK_F6: case VK_F7: case VK_F8:
    case VK_F9: case VK_F10: case VK_F11: case VK_F12:
        return 0;
    default:
        break;
    }

    BYTE state[256]{};
    GetKeyboardState(state);
    for (int i = 0; i < 256; ++i) {
        if (g_softVkDown[i]) state[i] |= 0x80;
        else if (i == VK_SHIFT || i == VK_LSHIFT || i == VK_RSHIFT
            || i == VK_CONTROL || i == VK_LCONTROL || i == VK_RCONTROL
            || i == VK_MENU || i == VK_LMENU || i == VK_RMENU) {
            // 软路径未按下的修饰键：清掉物理残留，避免本机按着 Shift 污染后台回放。
            state[i] &= static_cast<BYTE>(~0x80);
        }
    }
    state[vk & 0xFF] |= 0x80;

    WCHAR buf[8]{};
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    int n = ToUnicode(vk, scan, state, buf, 8, 0);
    if (n < 0) {
        // 死键：再调一次清状态，本轮不产出字符。
        ToUnicode(vk, scan, state, buf, 8, 0);
        return 0;
    }
    if (n >= 1) return buf[0];
    return 0;
}

/// Soft focus without SetForegroundWindow / Z-order changes.
/// Android 模拟器（MuMu 等）用 WM_ACTIVATE；记事本等仍用 WM_SETFOCUS。
void PrimeWindowSoftFocus(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    HWND top = TopLevelTargetWindow(hwnd);
    if (!top) top = hwnd;
    // DeSmuME：不要发 WM_SETFOCUS/ACTIVATE（wx 消息泵敏感，易偶发崩）。
    if (IsDesktopEmulatorTarget(top, nullptr)) return;
    wchar_t title[512]{};
    wchar_t cls[256]{};
    GetWindowTextW(top, title, 512);
    GetClassNameW(hwnd, cls, 256);
    if (LooksLikeAndroidEmulatorWindowTitle(title)
        || LooksLikeQtRenderWindowClass(cls)) {
        PostMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        if (g_haveLastClientPos && g_lastClientHwnd == hwnd) {
            const WPARAM moveFlags = ModifierKeyFlags() | g_softMouseFlags;
            DeliverWindowMessage(hwnd, WM_MOUSEMOVE, moveFlags,
                MAKELPARAM(g_lastClientX, g_lastClientY));
        }
        return;
    }
    NotifyWindowMessage(hwnd, WM_SETFOCUS, 0, 0);
}

WPARAM ButtonFlag(MouseButtonType button) {
    switch (button) {
    case MouseButtonType::Right: return MK_RBUTTON;
    case MouseButtonType::Middle: return MK_MBUTTON;
    case MouseButtonType::X1: return MK_XBUTTON1;
    case MouseButtonType::X2: return MK_XBUTTON2;
    default: return MK_LBUTTON;
    }
}

WORD XButtonHiWord(MouseButtonType button) {
    if (button == MouseButtonType::X1) return XBUTTON1;
    if (button == MouseButtonType::X2) return XBUTTON2;
    return 0;
}

void ButtonMessages(MouseButtonType button, UINT& downMsg, UINT& upMsg, UINT& dblMsg) {
    switch (button) {
    case MouseButtonType::Right:
        downMsg = WM_RBUTTONDOWN;
        upMsg = WM_RBUTTONUP;
        dblMsg = WM_RBUTTONDBLCLK;
        break;
    case MouseButtonType::Middle:
        downMsg = WM_MBUTTONDOWN;
        upMsg = WM_MBUTTONUP;
        dblMsg = WM_MBUTTONDBLCLK;
        break;
    case MouseButtonType::X1:
    case MouseButtonType::X2:
        downMsg = WM_XBUTTONDOWN;
        upMsg = WM_XBUTTONUP;
        dblMsg = WM_XBUTTONDBLCLK;
        break;
    default:
        downMsg = WM_LBUTTONDOWN;
        upMsg = WM_LBUTTONUP;
        dblMsg = WM_LBUTTONDBLCLK;
        break;
    }
}

WPARAM SoftMouseWParam(WPARAM keyFlags, MouseButtonType button, bool includeButton) {
    WPARAM flags = keyFlags;
    if (includeButton) flags |= ButtonFlag(button);
    const WORD xHi = XButtonHiWord(button);
    if (xHi) return MAKEWPARAM(static_cast<WORD>(flags), xHi);
    return flags;
}

/// 软双击：PostMessage 不会自动合成 DBLCLK，需自行在阈值内发。
DWORD g_lastSoftClickTick = 0;
HWND g_lastSoftClickHwnd = nullptr;
MouseButtonType g_lastSoftClickButton = MouseButtonType::Left;
int g_lastSoftClickX = 0;
int g_lastSoftClickY = 0;
int g_softClickStreak = 0;

bool SoftIsDoubleClick(HWND target, MouseButtonType button, int hitX, int hitY) {
    const DWORD now = GetTickCount();
    const UINT limit = GetDoubleClickTime();
    const int slop = GetSystemMetrics(SM_CXDOUBLECLK) / 2;
    const bool same = target == g_lastSoftClickHwnd
        && button == g_lastSoftClickButton
        && abs(hitX - g_lastSoftClickX) <= slop
        && abs(hitY - g_lastSoftClickY) <= slop
        && (now - g_lastSoftClickTick) <= limit
        && g_softClickStreak >= 1;
    g_lastSoftClickTick = now;
    g_lastSoftClickHwnd = target;
    g_lastSoftClickButton = button;
    g_lastSoftClickX = hitX;
    g_lastSoftClickY = hitY;
    if (same) {
        ++g_softClickStreak;
        return (g_softClickStreak % 2) == 0; // 第 2、4… 次按下为 DBLCLK
    }
    g_softClickStreak = 1;
    return false;
}

HWND ResolveBackgroundPostTarget(HWND hwnd, int& cx, int& cy) {
    if (!hwnd || !IsWindow(hwnd)) return nullptr;

    HWND root = TopLevelTargetWindow(hwnd);
    if (!root) root = hwnd;

    // DeSmuME：触摸只吃顶层 WM_LBUTTON*，勿重定向到子窗。
    if (IsDesktopEmulatorTarget(root, nullptr)) {
        if (root != hwnd) {
            MapClientPointBetweenHwnds(hwnd, root, cx, cy);
        }
        ClampToClientRect(root, cx, cy);
        return root;
    }

    BackgroundInputTargetKind kind = BackgroundInputTargetKind::TopLevel;
    HWND surface = FindBackgroundInputChild(root, nullptr, &kind);

    if (surface && IsWindow(surface)) {
        int hitX = cx;
        int hitY = cy;
        if (surface != hwnd) {
            if (!MapClientPointBetweenHwnds(hwnd, surface, hitX, hitY)) {
                hitX = cx;
                hitY = cy;
            }
        }
        ClampToClientRect(surface, hitX, hitY);
        cx = hitX;
        cy = hitY;
        return surface;
    }

    ClampToClientRect(hwnd, cx, cy);
    return hwnd;
}

HWND ResolveSoftInputHwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
    int lastX = 0;
    int lastY = 0;
    if (GetLastSoftMouseClientPos(hwnd, lastX, lastY)) {
        int hitX = lastX;
        int hitY = lastY;
        if (HWND hit = ResolveBackgroundPostTarget(hwnd, hitX, hitY)) return hit;
    }
    HWND root = TopLevelTargetWindow(hwnd);
    if (!root) root = hwnd;
    if (HWND surface = FindBackgroundInputChild(root, nullptr)) {
        if (surface != root) return surface;
    }
    if (HWND edit = FindTextInputTarget(root)) return edit;
    return hwnd;
}

bool SendQuickInputViaSoftChars(HWND input, const std::wstring& text, double charInterval,
    const std::atomic_bool* cancelFlag) {
    if (!input || !IsWindow(input) || text.empty()) return false;
    PrimeWindowSoftFocus(input);
    for (wchar_t ch : text) {
        if (WindowModeCancelled(cancelFlag)) return true;
        if (ch == L'\r' || ch == L'\n') {
            NotifyWindowMessage(input, WM_KEYDOWN, VK_RETURN, BuildKeyLParam(VK_RETURN, true));
            NotifyWindowMessage(input, WM_CHAR, L'\r', BuildKeyLParam(VK_RETURN, true));
            NotifyWindowMessage(input, WM_KEYUP, VK_RETURN, BuildKeyLParam(VK_RETURN, false));
        } else {
            // 无对应 VK：直接 WM_CHAR，覆盖中文与自定义控件。
            NotifyWindowMessage(input, WM_CHAR, static_cast<WPARAM>(ch), 1);
        }
        if (charInterval > 0) {
            WindowModeSleepInterruptible(cancelFlag,
                std::chrono::milliseconds(static_cast<int>(charInterval * 1000.0)));
        }
    }
    return true;
}

}  // namespace

bool GetLastSoftMouseClientPos(HWND hwnd, int& cx, int& cy) {
    if (!g_haveLastClientPos) return false;
    if (hwnd && g_lastClientHwnd && IsWindow(g_lastClientHwnd)) {
        HWND queryRoot = TopLevelTargetWindow(hwnd);
        HWND lastRoot = TopLevelTargetWindow(g_lastClientHwnd);
        if (queryRoot && lastRoot && queryRoot != lastRoot) return false;
    }
    cx = g_lastClientX;
    cy = g_lastClientY;
    return true;
}

void RememberSoftMouseClientPos(HWND hwnd, int cx, int cy) {
    RememberClientPos(hwnd, cx, cy);
}

bool SoftMouseButtonHeld() {
    return g_softMouseFlags != 0;
}

void ResetSoftMouseState() {
    g_softMouseFlags = 0;
    g_haveLastClientPos = false;
    g_lastClientX = 0;
    g_lastClientY = 0;
    g_lastClientHwnd = nullptr;
    g_lastSoftClickTick = 0;
    g_lastSoftClickHwnd = nullptr;
    g_softClickStreak = 0;
    ResetSoftKeyState();
}

HWND FindTextInputTarget(HWND root) {
    if (!root || !IsWindow(root)) return nullptr;

    wchar_t cls[256]{};
    GetClassNameW(root, cls, 256);
    if (IsTextInputClass(cls)) return root;

    FindTextInputContext ctx{};
    EnumChildWindows(root, FindTextInputProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

void SendQuickInputViaForeground(HWND hwnd, const std::wstring& text, double charInterval,
    const std::atomic_bool* cancelFlag) {
    if (!hwnd || !IsWindow(hwnd) || text.empty()) return;
    if (WindowModeCancelled(cancelFlag)) return;

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;
    if (!IsWindowOnUserCurrentDesktop(root)) return;
    HWND input = FindTextInputTarget(root);
    if (!input) input = root;

    struct ScopedThreadAttach {
        DWORD cur = GetCurrentThreadId();
        DWORD fgThread = 0;
        DWORD targetThread = 0;
        bool fg = false;
        bool target = false;
        HWND prevFg = nullptr;

        ScopedThreadAttach(HWND targetRoot, HWND focusHwnd) {
            prevFg = GetForegroundWindow();
            fgThread = prevFg ? GetWindowThreadProcessId(prevFg, nullptr) : 0;
            targetThread = GetWindowThreadProcessId(targetRoot, nullptr);
            if (fgThread && fgThread != cur) {
                fg = AttachThreadInput(cur, fgThread, TRUE) == TRUE;
            }
            if (targetThread && targetThread != cur) {
                target = AttachThreadInput(cur, targetThread, TRUE) == TRUE;
            }
            AllowSetForegroundWindow(ASFW_ANY);
            RestoreWindowQuiet(targetRoot);
            SetForegroundWindow(targetRoot);
            BringWindowToTop(targetRoot);
            if (focusHwnd) SetFocus(focusHwnd);
        }

        ~ScopedThreadAttach() {
            if (target && targetThread) AttachThreadInput(cur, targetThread, FALSE);
            if (fg && fgThread) AttachThreadInput(cur, fgThread, FALSE);
            if (prevFg && IsWindow(prevFg)) SetForegroundWindow(prevFg);
        }
    };

    ScopedThreadAttach scope(root, input);

    for (size_t i = 0; i < text.size(); ++i) {
        if (WindowModeCancelled(cancelFlag)) return;
        const wchar_t ch = text[i];
        if (ch == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            SendKeyboardKey(VK_RETURN, true);
            SendKeyboardKey(VK_RETURN, false);
        } else if (ch == L'\n') {
            SendKeyboardKey(VK_RETURN, true);
            SendKeyboardKey(VK_RETURN, false);
        } else {
            SendUnicodeChar(ch);
        }
        if (charInterval > 0) {
            WindowModeSleepInterruptible(cancelFlag,
                std::chrono::milliseconds(static_cast<int>(charInterval * 1000.0)));
        }
    }
}

void PostQuickInputToWindow(HWND hwnd, const std::wstring& text, double charInterval,
    bool allowForegroundFallback, const std::atomic_bool* cancelFlag) {
    if (!hwnd || !IsWindow(hwnd) || text.empty()) return;
    if (WindowModeCancelled(cancelFlag)) return;

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;
    // 优先最近点击子控件，再 Edit 扫描，避免多输入框贴错位置。
    HWND input = ResolveSoftInputHwnd(hwnd);
    if (!input) input = FindTextInputTarget(root);

    if (input) {
        PrimeWindowSoftFocus(input);
        if (SendQuickInputViaEditMessages(input, text, charInterval, cancelFlag)) return;
        if (WindowModeCancelled(cancelFlag)) return;
        if (SendQuickInputViaClipboard(input, text, true)) return;
        if (WindowModeCancelled(cancelFlag)) return;
        // 非 Edit / 剪贴板失败：逐字 WM_CHAR（中文、自定义控件）
        if (SendQuickInputViaSoftChars(input, text, charInterval, cancelFlag)) return;
    }

    if (WindowModeCancelled(cancelFlag)) return;
    if (SendQuickInputViaClipboard(root, text, true)) return;
    if (WindowModeCancelled(cancelFlag)) return;
    if (SendQuickInputViaSoftChars(root, text, charInterval, cancelFlag)) return;

    if (allowForegroundFallback) {
        if (WindowModeCancelled(cancelFlag)) return;
        if (SendQuickInputViaUiAutomation(root, text)) return;
        SendQuickInputViaForeground(hwnd, text, charInterval, cancelFlag);
        return;
    }

    WindowModeLog(L"[窗口模式] 快捷输入失败：目标窗口不支持无焦点输入");
}

void PostScrollWheelToWindow(HWND hwnd, int cx, int cy, int steps, bool vertical, bool positive) {
    if (!hwnd || !IsWindow(hwnd) || steps <= 0) return;

    RememberClientPos(hwnd, cx, cy);

    int hitX = cx;
    int hitY = cy;
    HWND target = ResolveBackgroundPostTarget(hwnd, hitX, hitY);
    if (!target) target = hwnd;
    MaybeRemapTopClientToPostTarget(hwnd, target, hitX, hitY);
    cx = hitX;
    cy = hitY;

    const WPARAM moveFlags = ModifierKeyFlags() | g_softMouseFlags;
    DeliverWindowMessage(target, WM_MOUSEMOVE, moveFlags, MAKELPARAM(hitX, hitY));

    // WM_MOUSEWHEEL / WM_MOUSEHWHEEL lParam must be screen coordinates (MSDN).
    int sx = 0;
    int sy = 0;
    if (!ClientToScreenPoint(target, hitX, hitY, sx, sy)) {
        POINT pt{hitX, hitY};
        if (ClientToScreen(target, &pt)) {
            sx = pt.x;
            sy = pt.y;
        } else {
            sx = cx;
            sy = cy;
        }
    }

    // 与桌面路径对齐：每步一个 WHEEL_DELTA，禁止 steps*delta 挤进一条消息。
    if (steps > 200) steps = 200;
    const SHORT notch = static_cast<SHORT>(positive ? WHEEL_DELTA : -WHEEL_DELTA);
    const UINT msg = vertical ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL;
    const LPARAM lp = MAKELPARAM(sx, sy);
    for (int i = 0; i < steps; ++i) {
        const WPARAM wp = MAKEWPARAM(static_cast<WORD>(ModifierKeyFlags() | g_softMouseFlags), notch);
        DeliverWindowMessage(target, msg, wp, lp);
    }
}

void PostKeyToWindow(HWND hwnd, UINT vk, bool down) {
    if (!hwnd || !IsWindow(hwnd) || vk == 0) return;

    HWND target = ResolveSoftInputHwnd(hwnd);
    if (!target || !IsWindow(target)) target = hwnd;

    // Alt 状态须在更新本键之前读取：松开 Alt 时仍应发 SYSKEYUP。
    const bool altWasDown = SoftIsVkDown(VK_MENU) || SoftIsVkDown(VK_LMENU)
        || SoftIsVkDown(VK_RMENU);
    SoftSetVkDown(vk, down);
    const bool altDown = SoftIsVkDown(VK_MENU) || SoftIsVkDown(VK_LMENU)
        || SoftIsVkDown(VK_RMENU);
    const bool useSys = down ? altDown : altWasDown;

    PrimeWindowSoftFocus(target);
    HWND top = TopLevelTargetWindow(target);
    // MuMu 等安卓壳：KEYDOWN 与 WM_CHAR 都会进 Android 文本层，双发会变成 11223344。
    const bool androidEmu = IsAndroidEmulatorTarget(top ? top : target, nullptr);
    if (down) {
        const UINT keyMsg = useSys ? WM_SYSKEYDOWN : WM_KEYDOWN;
        DeliverWindowMessage(target, keyMsg, vk, BuildKeyLParam(vk, true));

        if (!androidEmu) {
            if (wchar_t ch = SoftVkToChar(vk)) {
                const UINT charMsg = useSys ? WM_SYSCHAR : WM_CHAR;
                DeliverWindowMessage(target, charMsg, static_cast<WPARAM>(ch),
                    BuildKeyLParam(vk, true));
            }
        }
    } else {
        const UINT keyMsg = useSys ? WM_SYSKEYUP : WM_KEYUP;
        DeliverWindowMessage(target, keyMsg, vk, BuildKeyLParam(vk, false));
    }
}

namespace {

bool ShouldThrottleDesktopEmuMouseMove(HWND target, int hitX, int hitY) {
    HWND top = TopLevelTargetWindow(target);
    if (!IsDesktopEmulatorTarget(top ? top : target, nullptr)) return false;
    // DeSmuME 对每条 MOVE 跑 ShowCursor 等重逻辑；拖拽也限频，避免轨迹宏崩进程。
    static HWND s_lastHwnd = nullptr;
    static int s_lastX = 0;
    static int s_lastY = 0;
    static DWORD s_lastTick = 0;
    const DWORD now = GetTickCount();
    const int dx = hitX - s_lastX;
    const int dy = hitY - s_lastY;
    if (target == s_lastHwnd && (now - s_lastTick) < 16 && (dx * dx + dy * dy) < 36) {
        return true;
    }
    s_lastHwnd = target;
    s_lastX = hitX;
    s_lastY = hitY;
    s_lastTick = now;
    return false;
}

}  // namespace

void PostMouseMoveToWindow(HWND hwnd, int cx, int cy) {
    if (!hwnd || !IsWindow(hwnd)) return;

    RememberClientPos(hwnd, cx, cy);

    int hitX = cx;
    int hitY = cy;
    HWND target = ResolveBackgroundPostTarget(hwnd, hitX, hitY);
    if (!target) target = hwnd;
    MaybeRemapTopClientToPostTarget(hwnd, target, hitX, hitY);

    if (ShouldThrottleDesktopEmuMouseMove(target, hitX, hitY)) return;

    const WPARAM wp = ModifierKeyFlags() | g_softMouseFlags;
    DeliverWindowMessage(target, WM_MOUSEMOVE, wp, MAKELPARAM(hitX, hitY));
}

void PostMouseButtonToWindow(HWND hwnd, int cx, int cy, MouseButtonType button, bool down) {
    if (!hwnd || !IsWindow(hwnd)) return;

    RememberClientPos(hwnd, cx, cy);

    int hitX = cx;
    int hitY = cy;
    HWND target = ResolveBackgroundPostTarget(hwnd, hitX, hitY);
    if (!target) target = hwnd;
    MaybeRemapTopClientToPostTarget(hwnd, target, hitX, hitY);

    const WPARAM buttonFlag = ButtonFlag(button);
    UINT downMsg = WM_LBUTTONDOWN;
    UINT upMsg = WM_LBUTTONUP;
    UINT dblMsg = WM_LBUTTONDBLCLK;
    ButtonMessages(button, downMsg, upMsg, dblMsg);

    HWND top = TopLevelTargetWindow(target);
    // DeSmuME 触摸坐标取自 DOWN/UP 的 lParam，点击前不必再发 MOVE（易触发 ShowCursor 风暴）。
    if (!IsDesktopEmulatorTarget(top ? top : target, nullptr)) {
        DeliverWindowMessage(target, WM_MOUSEMOVE,
            SoftMouseWParam(ModifierKeyFlags() | g_softMouseFlags, button, down),
            MAKELPARAM(hitX, hitY));
    }

    if (down) {
        g_softMouseFlags |= buttonFlag;
        const bool dbl = SoftIsDoubleClick(target, button, hitX, hitY);
        const UINT msg = dbl ? dblMsg : downMsg;
        DeliverWindowMessage(target, msg,
            SoftMouseWParam(ModifierKeyFlags() | g_softMouseFlags, button, true),
            MAKELPARAM(hitX, hitY));
    } else {
        DeliverWindowMessage(target, upMsg,
            SoftMouseWParam(ModifierKeyFlags() | (g_softMouseFlags & ~buttonFlag), button, false),
            MAKELPARAM(hitX, hitY));
        g_softMouseFlags &= ~buttonFlag;
    }
}

}  // namespace windowmode
