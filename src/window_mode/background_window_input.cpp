#include "background_window_input.h"

#include "action_utils.h"
#include "background_uia_input.h"
#include "background_input_target.h"
#include "cdp/cdp_input.h"
#include "utils.h"
#include "window_coords.h"
#include "window_mode_log.h"
#include "window_mode_types.h"
#include "window_target.h"
#include "fake_focus/fake_focus_soft_input_host.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace windowmode {

LPARAM BuildWindowKeyLParam(UINT vk, bool down) {
    UINT scan = 0;
    switch (vk) {
    case VK_UP: scan = 0x48; break;
    case VK_LEFT: scan = 0x4B; break;
    case VK_RIGHT: scan = 0x4D; break;
    case VK_DOWN: scan = 0x50; break;
    case VK_INSERT: scan = 0x52; break;
    case VK_DELETE: scan = 0x53; break;
    case VK_HOME: scan = 0x47; break;
    case VK_END: scan = 0x4F; break;
    case VK_PRIOR: scan = 0x49; break;
    case VK_NEXT: scan = 0x51; break;
    default: {
        UINT scanEx = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
        if (!scanEx) scanEx = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        scan = scanEx & 0xFFu;
        // DirectInput 风格 0xCB 不能塞进 WM 的 8 位扫描码字段。
        if (scan >= 0x80) scan &= 0x7Fu;
        break;
    }
    }
    LPARAM lParam = 1;
    if (scan) lParam |= static_cast<LPARAM>(scan) << 16;
    // 方向/编辑键必须带 KF_EXTENDED：否则 VK_LEFT 会被当成小键盘 4。
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

bool g_lcaQueuedKeys = false;

bool LooksLikeMapleStoryHwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND top = TopLevelTargetWindow(hwnd);
    if (!top) top = hwnd;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (LooksLikeMapleStoryWindowClass(cls)) return true;
    GetClassNameW(hwnd, cls, 256);
    if (LooksLikeMapleStoryWindowClass(cls)) return true;
    wchar_t title[512]{};
    GetWindowTextW(top, title, 512);
    if (LooksLikeMapleStoryTitle(title)) return true;
    return LooksLikeMapleStoryExecutable(QueryHwndProcessImagePath(top));
}

bool ShouldPostLcaQueuedKeys(HWND hwnd) {
    if (g_lcaQueuedKeys) return true;
    return LooksLikeMapleStoryHwnd(hwnd);
}

bool LooksLikeWeixinSoftHwnd(HWND hwnd) {
    return LooksLikeWeixinTarget(WindowModeScriptConfig{}, hwnd);
}

bool LooksLikeQtSoftHwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (LooksLikeQtRenderWindowClass(cls)) return true;
    HWND top = TopLevelTargetWindow(hwnd);
    if (top && top != hwnd) {
        GetClassNameW(top, cls, 256);
        if (LooksLikeQtRenderWindowClass(cls)) return true;
    }
    return false;
}

bool ClassPrefersPostedQuickKeys(const wchar_t* cls) {
    if (!cls || !cls[0]) return false;
    if (LooksLikeQtRenderWindowClass(cls)) return true;
    if (LooksLikeAdobeAirWindowClass(cls)) return true;
    if (LooksLikeGameWindowClass(cls)) return true;
    if (LooksLikeChromiumBrowserClass(cls)) return true;
    if (LooksLikeEmulatorWindowClass(cls)) return true;
    if (LooksLikeMapleStoryWindowClass(cls)) return true;
    if (LooksLikeGlfwOrSdlWindowClass(cls)) return true;
    return false;
}

bool WindowPrefersPostedQuickKeys(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND top = TopLevelTargetWindow(hwnd);
    if (!top) top = hwnd;
    if (LooksLikeWeixinSoftHwnd(hwnd) || LooksLikeWeixinSoftHwnd(top)) return true;
    if (ShouldPostLcaQueuedKeys(hwnd) || ShouldPostLcaQueuedKeys(top)) return true;
    if (IsAndroidEmulatorTarget(top, nullptr) || IsAndroidEmulatorTarget(hwnd, nullptr)) {
        return true;
    }
    if (HwndLooksLikeChromiumShell(hwnd) || HwndLooksLikeChromiumShell(top)) return true;
    if (IsDesktopEmulatorTarget(top, nullptr)) return true;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (ClassPrefersPostedQuickKeys(cls)) return true;
    if (top != hwnd) {
        GetClassNameW(top, cls, 256);
        if (ClassPrefersPostedQuickKeys(cls)) return true;
    }
    return false;
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

bool WindowAcceptsWmPaste(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    return IsTextInputClass(cls);
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
    if (LooksLikeWeixinSoftHwnd(hwnd) || LooksLikeWeixinSoftHwnd(top)) {
        const BOOL ok = PostMessageW(hwnd, msg, wp, lp);
        if (top && IsWindow(top)) PostMessageW(top, WM_NULL, 0, 0);
        else PostMessageW(hwnd, WM_NULL, 0, 0);
        return ok != FALSE;
    }
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
    // QWindow / AIR / 游戏 / Chromium 控件都会“成功”吃掉 WM_PASTE 但输入框不动。
    // 只对真正的 Edit/RichEdit 等声称粘贴成功，其余走 KEY* 或 WM_CHAR。
    if (!WindowAcceptsWmPaste(input)) return false;

    ScopedClipboardUnicodeText clip(text);
    if (!clip.Ok()) return false;

    // 同步粘贴：须在 ScopedClipboardUnicodeText 析构恢复剪贴板之前完成。
    // 勿 PostMessage(WM_PASTE)，否则异步粘贴时剪贴板可能已还原，导致重复/错贴。
    SendMessageTimeoutSafe(input, WM_PASTE, 0, 0);
    return true;
}

LPARAM BuildKeyLParam(UINT vk, bool down) {
    return BuildWindowKeyLParam(vk, down);
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
    if (ShouldPostLcaQueuedKeys(hwnd) || ShouldPostLcaQueuedKeys(top)) {
        // LCA 后台一：按键前不发 WM_ACTIVATE/SETFOCUS（冒险岛发激活会冻客户端）。
        return;
    }
    // 微信 4.x Qt：WM_ACTIVATE 会让客户端 SetForegroundWindow，把微信切到前台。
    if (LooksLikeWeixinSoftHwnd(hwnd) || LooksLikeWeixinSoftHwnd(top)) {
        return;
    }
    wchar_t title[512]{};
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(top, title, 512);
    // 只给安卓模拟器发 WM_ACTIVATE。勿把所有 Qt*QWindowIcon（含其它 Qt 桌面程序）都激活，
    // 否则会像微信一样抢前台。
    if (IsAndroidEmulatorTarget(top, nullptr)
        || LooksLikeAndroidEmulatorWindowTitle(title)
        || LooksLikeAndroidEmulatorWindowClass(cls)) {
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
    // 冒险岛：LCA 后台一键鼠都打到绑定的顶层 MapleStory 窗，不要落到最大子表面。
    if (IsDesktopEmulatorTarget(root, nullptr) || ShouldPostLcaQueuedKeys(root)) {
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

/// 后台逐字投递的最小时序（毫秒）。
/// 真实键盘不会在同一瞬间完成 DOWN→UP：多数目标按帧取键，且只在「该键仍按下」时
/// 才接受由自己的 TranslateMessage 合成的 WM_CHAR。零间隔连发会把整串按键挤进同一帧、
/// 并把所有 WM_CHAR 排到所有 KEYUP 之后，表现为吞字
/// （实测：后台窗口快捷输入 "11" 只进一个 1；前台走 SendInput 正常）。
/// 现场试值：环境变量 QST_LCA_KEY_MS=<0~500> 同时覆盖按住与间隔（0=关闭，回到旧的立刻 DOWN/UP）。
int PostedKeyStepMs() {
    static const int value = [] {
        wchar_t buf[16]{};
        const DWORD n = GetEnvironmentVariableW(L"QST_LCA_KEY_MS", buf, 16);
        if (n > 0 && n < 16) {
            const int parsed = _wtoi(buf);
            if (parsed >= 0 && parsed <= 500) return parsed;
        }
        return 24;  // ≥1 帧（60fps≈17ms / 30fps≈33ms 取中）
    }();
    return value;
}

/// 队列屏障开关（默认开）。关掉即回到「纯固定按住时序」，只用于现场 A/B 对照：
/// 环境变量 QST_LCA_NO_BARRIER=1。
bool PostedKeyBarrierEnabled() {
    static const bool value = [] {
        wchar_t buf[8]{};
        return GetEnvironmentVariableW(L"QST_LCA_NO_BARRIER", buf, 8) == 0;
    }();
    return value;
}

HWND ResolveSoftInputHwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
    HWND root = TopLevelTargetWindow(hwnd);
    if (!root) root = hwnd;
    if (ShouldPostLcaQueuedKeys(root) || ShouldPostLcaQueuedKeys(hwnd)) return root;
    int lastX = 0;
    int lastY = 0;
    if (GetLastSoftMouseClientPos(hwnd, lastX, lastY)) {
        int hitX = lastX;
        int hitY = lastY;
        if (HWND hit = ResolveBackgroundPostTarget(hwnd, hitX, hitY)) return hit;
    }
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

void SetLcaBackgroundMessageMode(bool enabled) {
    g_lcaQueuedKeys = enabled;
}

bool WaitSoftKeyPostTurn(HWND target, int timeoutMs) {
    if (!target || !IsWindow(target)) return false;
    if (!FakeFocusSoftInput_IsAttached() || !FakeFocusSoftInput_PostKeyEventsEnabled()) return false;
    // 现场 A/B：QST_NO_SOFT_KEY_BARRIER=1 关掉软键屏障（回到「宿主一次写完所有键态」的旧行为）。
    {
        static const bool disabled = [] {
            wchar_t buf[8]{};
            return GetEnvironmentVariableW(L"QST_NO_SOFT_KEY_BARRIER", buf, 8) != 0;
        }();
        if (disabled) return false;
    }
    // 交叉点：DLL 灌键线程 1ms 轮询后才把事件 PostMessage 给目标，而状态（down[]）由宿主**立刻**写好。
    // 因此这里先用一次跨线程同步消息做「队列屏障」：屏障消息排在已投递的键消息之后被目标处理，
    // 它返回时目标已经跑完这批键的 WndProc（Chromium 正是在那里 GetKeyState 判 Ctrl+V 组合键）。
    // 两趟：第一趟可能早于灌键线程的 PostMessage，第二趟兜住那一笔。
    bool ok = false;
    for (int pass = 0; pass < 2; ++pass) {
        std::this_thread::sleep_for(std::chrono::milliseconds(pass == 0 ? 2 : 1));
        DWORD_PTR res = 0;
        ok = SendMessageTimeoutW(target, WM_NULL, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_NORMAL,
            static_cast<UINT>(timeoutMs > 0 ? timeoutMs : 80), &res) != 0;
        if (!ok) return false;
    }
    return true;
}

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

bool SendQuickInputViaPostedKeys(HWND hwnd, const std::wstring& text, double charInterval,
    const std::atomic_bool* cancelFlag) {
    if (!hwnd || !IsWindow(hwnd) || text.empty()) return false;
    HWND top = TopLevelTargetWindow(hwnd);
    if (!top || !IsWindow(top)) top = hwnd;

    const bool drainKeys = FakeFocusSoftInput_IsAttached()
        && FakeFocusSoftInput_PostKeyEventsEnabled();
    const bool weixin = LooksLikeWeixinSoftHwnd(hwnd) || LooksLikeWeixinSoftHwnd(top);
    WindowModeLog(weixin
        ? L"[窗口模式] 微信 Qt 快捷输入走 KEY*/Ctrl+V，不向 QWindow 发 WM_PASTE"
        : L"[窗口模式] 快捷输入：非 Edit 走 KEY*/Ctrl+V（不发 WM_PASTE）");

    // 按住/间隔：见 PostedKeyStepMs 注释。用户给了字间隔就取较大者，绝不低于最小时序。
    const int stepMs = PostedKeyStepMs();
    int gapMs = stepMs;
    if (charInterval > 0) {
        gapMs = std::max(gapMs, static_cast<int>(charInterval * 1000.0 + 0.5));
    }
    auto waitMs = [&](int ms) {
        if (ms > 0) WindowModeSleepInterruptible(cancelFlag, std::chrono::milliseconds(ms));
    };

    auto postVk = [&](UINT vk, bool down) {
        if (drainKeys) {
            SoftSetVkDown(vk, down);
            FakeFocusSoftInput_SetKey(vk, down);
            return;
        }
        PostKeyToWindow(top, vk, down);
    };

    bool allHaveVk = true;
    for (wchar_t ch : text) {
        if (ch == L'\r' || ch == L'\n') continue;
        if (VkKeyScanW(ch) == -1) {
            allHaveVk = false;
            break;
        }
    }

    if (!allHaveVk) {
        // 非 Edit 不吃 WM_PASTE；Ctrl+V 须在剪贴板还原之前处理完。
        ScopedClipboardUnicodeText clip(text);
        if (!clip.Ok()) return false;
        if (drainKeys) {
            // 软键态与事件是两条路：每笔之后必须等目标处理完再写下一步键态，
            // 否则目标处理 WM_KEYDOWN(V) 时读到的 Ctrl 已经是抬起（只出 v 不粘贴）。
            SoftSetVkDown(VK_LCONTROL, true);
            SoftSetVkDown(VK_CONTROL, true);
            FakeFocusSoftInput_SetKey(VK_LCONTROL, true);
            FakeFocusSoftInput_SetKey(VK_CONTROL, true);
            WaitSoftKeyPostTurn(top);
            FakeFocusSoftInput_SetKey('V', true);
            WaitSoftKeyPostTurn(top);
            FakeFocusSoftInput_SetKey('V', false);
            WaitSoftKeyPostTurn(top);
            FakeFocusSoftInput_SetKey(VK_LCONTROL, false);
            FakeFocusSoftInput_SetKey(VK_CONTROL, false);
            WaitSoftKeyPostTurn(top);
            SoftSetVkDown('V', false);
            SoftSetVkDown(VK_LCONTROL, false);
            SoftSetVkDown(VK_CONTROL, false);
        } else {
            if (FakeFocusSoftInput_IsAttached()) {
                FakeFocusSoftInput_SetKey(VK_LCONTROL, true);
                FakeFocusSoftInput_SetKey(VK_CONTROL, true);
            }
            DWORD_PTR res = 0;
            auto send = [&](UINT msg, UINT vk, bool down) {
                SendMessageTimeoutW(top, msg, vk, BuildWindowKeyLParam(vk, down),
                    SMTO_ABORTIFHUNG | SMTO_NORMAL, 500, &res);
            };
            send(WM_KEYDOWN, VK_CONTROL, true);
            waitMs(stepMs);
            if (FakeFocusSoftInput_IsAttached()) FakeFocusSoftInput_SetKey('V', true);
            send(WM_KEYDOWN, 'V', true);
            waitMs(stepMs);
            send(WM_KEYUP, 'V', false);
            send(WM_KEYUP, VK_CONTROL, false);
            if (FakeFocusSoftInput_IsAttached()) {
                FakeFocusSoftInput_SetKey('V', false);
                FakeFocusSoftInput_SetKey(VK_LCONTROL, false);
                FakeFocusSoftInput_SetKey(VK_CONTROL, false);
            }
        }
        WindowModeSleepInterruptible(cancelFlag, std::chrono::milliseconds(80));
        return true;
    }

    // 目标线程队列屏障（决定性时序，与目标帧率无关）：
    // 跨线程同步 SendMessage 会被目标线程当作**队列里的消息**处理 —— 它排在「我们已 PostMessage
    // 投递的消息之后、目标自己 TranslateMessage 补发的 WM_CHAR 之前」。等它返回即证明：本键已
    // 被目标处理（键处于按下态），且它的 WM_CHAR 已经排在接下来要处理的队列里，先于我们随后发的 UP。
    // 这样就不会出现「DOWN/UP 挤进同一帧、WM_CHAR 被排到 KEYUP 之后 → 目标按『键仍按下』判定时整串被吞」。
    // 失败（UIPI 拒发 / 目标不应答 / 进程内灌键队列另有时序）→ 回落固定按住/间隔兜底。
    auto queueBarrier = [&]() -> bool {
        if (drainKeys || !PostedKeyBarrierEnabled()) return false;
        DWORD_PTR res = 0;
        return SendMessageTimeoutW(top, WM_NULL, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_NORMAL, 120, &res) != 0;
    };
    // 探测一次：顺带把队列里已有的点击/按键冲干净（替代固定的前导等待）。
    const bool useBarrier = queueBarrier();

    // 诊断：把真正发出的文本与时序写进日志（现场排「吞字」时先看这一行）。
    {
        std::wstring preview;
        const size_t limit = 48;
        for (size_t i = 0; i < text.size() && i < limit; ++i) {
            const wchar_t ch = text[i];
            if (ch == L'\r') preview += L"\\r";
            else if (ch == L'\n') preview += L"\\n";
            else if (ch == L'\t') preview += L"\\t";
            else preview.push_back(ch);
        }
        if (text.size() > limit) preview += L"…";
        wchar_t pacing[32]{};
        if (useBarrier) swprintf_s(pacing, L"队列屏障");
        else swprintf_s(pacing, L"%dms", stepMs);
        WindowModeLogf(
            L"[窗口模式] 快捷输入逐字投递 %zu 字 按住=%s 间隔=%dms 文本=\"%s\"",
            text.size(), pacing, gapMs, preview.c_str());
    }

    // 先让目标把队列里已有的消息（上一条点击/按键）处理完，避免首字被并进同一帧。
    if (!useBarrier) waitMs(stepMs);

    for (wchar_t ch : text) {
        if (WindowModeCancelled(cancelFlag)) return true;
        if (ch == L'\r' || ch == L'\n') {
            postVk(VK_RETURN, true);
            if (!(useBarrier && queueBarrier())) waitMs(stepMs);
            postVk(VK_RETURN, false);
        } else {
            const SHORT scanned = VkKeyScanW(ch);
            const UINT vk = static_cast<UINT>(LOBYTE(scanned));
            const bool shift = (HIBYTE(scanned) & 1) != 0;
            if (shift) postVk(VK_LSHIFT, true);
            postVk(vk, true);
            // 必须以真实键盘的 DOWN→（目标自己 TranslateMessage 出 WM_CHAR）→UP 顺序落地：
            // 立刻发 UP 会让 WM_CHAR 排在 KEYUP 之后，目标按「键仍按下」判定时整串被吞。
            if (!(useBarrier && queueBarrier())) waitMs(stepMs);
            postVk(vk, false);
            if (shift) postVk(VK_LSHIFT, false);
        }
        // 本字的 UP 先落地，下一字不会与本字并进同一帧（无屏障时才退化成固定间隔）。
        if (!(useBarrier && queueBarrier())) waitMs(gapMs);
    }
    return true;
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
    // 调用方已是 Edit/RichEdit 时不要被「上次点击点」拐到别的表面。
    HWND input = hwnd;
    wchar_t inputCls[256]{};
    if (!GetClassNameW(hwnd, inputCls, 256) || !IsTextInputClass(inputCls)) {
        input = ResolveSoftInputHwnd(hwnd);
        if (!input) input = FindTextInputTarget(root);
    }

    if (input) {
        const bool posted = WindowPrefersPostedQuickKeys(input)
            || WindowPrefersPostedQuickKeys(root);
        if (posted) {
            // 若点到的是真 Edit（即便顶层是 Qt/游戏），仍走 EM_REPLACESEL / WM_PASTE。
            if (SendQuickInputViaEditMessages(input, text, charInterval, cancelFlag)) return;
            if (WindowModeCancelled(cancelFlag)) return;
            if (WindowAcceptsWmPaste(input)
                && SendQuickInputViaClipboard(input, text, true)) {
                return;
            }
            if (WindowModeCancelled(cancelFlag)) return;
            SendQuickInputViaPostedKeys(input, text, charInterval, cancelFlag);
            return;
        }
        PrimeWindowSoftFocus(input);
        if (SendQuickInputViaEditMessages(input, text, charInterval, cancelFlag)) return;
        if (WindowModeCancelled(cancelFlag)) return;
        if (SendQuickInputViaClipboard(input, text, true)) return;
        if (WindowModeCancelled(cancelFlag)) return;
        // 非 Edit / 剪贴板失败：逐字 WM_CHAR（中文、自定义 Win32 控件）
        if (SendQuickInputViaSoftChars(input, text, charInterval, cancelFlag)) return;
    }

    if (WindowModeCancelled(cancelFlag)) return;
    if (WindowPrefersPostedQuickKeys(root)) {
        SendQuickInputViaPostedKeys(root, text, charInterval, cancelFlag);
        return;
    }
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

bool IsArrowVirtualKey(UINT vk) {
    return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
}

bool ShouldMirrorLcaNavKeyState(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (LooksLikeMapleStoryHwnd(hwnd)) return true;
    // 未登记 LCA 游戏：前台时同样靠本机键态走路。自检探针是 TOOLWINDOW，勿 SendInput。
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return (ex & WS_EX_TOOLWINDOW) == 0;
}

/// 目标窗口（或其顶层）此刻是否就是前台窗。
/// 方向键的本机键态兜底（SendInput）只在「目标就是前台」时才有意义：
/// 目标在后台时 SendInput 打的是当前前台窗——用户正在看的浏览器/视频会收到 ←/→/↑/↓，
/// 而目标本身（DirectInput 读设备态）因为失焦早就停了轮询，照样不会走。
/// 所以后台一律不补真键，只写软键态 + PostMessage（后台走路靠 DI 软键）。
bool TargetOwnsForegroundWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == hwnd) return true;
    HWND top = TopLevelTargetWindow(hwnd);
    if (top && IsWindow(top) && (fg == top || IsChild(top, fg))) return true;
    return false;
}

void PostKeyToWindow(HWND hwnd, UINT vk, bool down) {
    vk = NormalizeScriptKeyVk(vk, L"");
    if (vk > 255) vk = 0;
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

    if (FakeFocusSoftInput_IsAttached()) {
        FakeFocusSoftInput_SetKey(vk, down);
    }

    HWND top = TopLevelTargetWindow(target);
    const bool weixinQt = LooksLikeWeixinSoftHwnd(hwnd) || LooksLikeWeixinSoftHwnd(target)
        || LooksLikeWeixinSoftHwnd(top);
    const bool qtWindow = weixinQt || LooksLikeQtSoftHwnd(hwnd) || LooksLikeQtSoftHwnd(target)
        || LooksLikeQtSoftHwnd(top);
    if (!weixinQt) {
        PrimeWindowSoftFocus(target);
        top = TopLevelTargetWindow(target);
    }
    const bool lcaKeys = ShouldPostLcaQueuedKeys(hwnd) || ShouldPostLcaQueuedKeys(target)
        || ShouldPostLcaQueuedKeys(top);
    if (lcaKeys) {
        // LCA 后台一：PostMessage 只发 KEYDOWN/KEYUP，不附带 WM_CHAR；WM_NULL 催队列。
        HWND send = top && IsWindow(top) ? top : target;
        const LPARAM lp = BuildWindowKeyLParam(vk, down);
        // 走路不走 WndProc：冒险岛 / 未登记 LCA 方向键兼写本机键态。
        // DirectInput 键盘钩已在轮询（lastCb=256）后不再 SendInput，以免方向键打进当前前台窗。
        if (IsArrowVirtualKey(vk) && ShouldMirrorLcaNavKeyState(send)) {
            bool mapleDiLive = false;
            DWORD gaks = 0, diState = 0, diData = 0, lastCb = 0;
            DWORD hitReady = 0, gfw = 0, focus = 0;
            if (LooksLikeMapleStoryHwnd(send) && FakeFocusSoftInput_IsAttached()
                && FakeFocusSoftInput_ReadMapleHits(
                    gaks, diState, diData, lastCb, hitReady, gfw, focus)) {
                mapleDiLive = diState > 0 && lastCb == 256;
            }
            // 兜底真键只在目标就是前台窗时补：后台时它打的是遮挡窗（浏览器视频 ←→ 跳转）。
            const bool mirrorLocalKey = !mapleDiLive && TargetOwnsForegroundWindow(send);
            static bool loggedArrowKeyState = false;
            if (!loggedArrowKeyState) {
                loggedArrowKeyState = true;
                WindowModeLogf(
                    L"[窗口模式] 方向键%s本机键态 vk=0x%02X lParam=0x%08X"
                    L"（DI lastCb=%lu diState=%lu；仅目标为前台时才补真键，后台不再打进遮挡窗）",
                    mapleDiLive ? L"改走 DirectInput 软键、不再写"
                                : (mirrorLocalKey ? L"兼写" : L"目标在后台、不写"),
                    vk, static_cast<unsigned>(lp),
                    static_cast<unsigned long>(lastCb),
                    static_cast<unsigned long>(diState));
                DWORD diag = 0, iatPoll = 0, diVt = 0;
                if (LooksLikeMapleStoryHwnd(send) && FakeFocusSoftInput_IsAttached()
                    && FakeFocusSoftInput_ReadMapleInstall(diag, iatPoll, diVt)) {
                    WindowModeLogf(
                        L"[窗口模式] 冒险岛首方向键后 hitReady=%lu gfw=%lu gaks=%lu diState=%lu "
                        L"lastCb=%lu iatPoll=%lu diag=0x%08X foundVt=%lu patchedSlot=%lu",
                        static_cast<unsigned long>(hitReady),
                        static_cast<unsigned long>(gfw),
                        static_cast<unsigned long>(gaks),
                        static_cast<unsigned long>(diState),
                        static_cast<unsigned long>(lastCb),
                        static_cast<unsigned long>(iatPoll),
                        static_cast<unsigned>(diag),
                        static_cast<unsigned long>(diVt & 0xFFu),
                        static_cast<unsigned long>((diVt >> 8) & 0xFFu));
                }
            }
            if (mirrorLocalKey) SendKeyboardKey(vk, down);
        }
        PostMessageW(send, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
        PostMessageW(send, WM_NULL, 0, 0);
        return;
    }
    // MuMu：KEYDOWN 与 WM_CHAR 都会进 Android 文本层，双发变成 11223344。
    // 微信 4.x Qt：自己把 KEYDOWN 转成字，再吃宿主 WM_CHAR 就会一次变两次。
    const bool androidEmu = IsAndroidEmulatorTarget(top ? top : target, nullptr);
    HWND send = (weixinQt && top && IsWindow(top)) ? top : target;
    if (down) {
        const UINT keyMsg = useSys ? WM_SYSKEYDOWN : WM_KEYDOWN;
        DeliverWindowMessage(send, keyMsg, vk, BuildKeyLParam(vk, true));
        if (!androidEmu && !qtWindow) {
            if (wchar_t ch = SoftVkToChar(vk)) {
                const UINT charMsg = useSys ? WM_SYSCHAR : WM_CHAR;
                DeliverWindowMessage(send, charMsg, static_cast<WPARAM>(ch),
                    BuildKeyLParam(vk, true));
            }
        }
        if (weixinQt) {
            PostMessageW(send, WM_NULL, 0, 0);
        }
    } else {
        const UINT keyMsg = useSys ? WM_SYSKEYUP : WM_KEYUP;
        DeliverWindowMessage(send, keyMsg, vk, BuildKeyLParam(vk, false));
        if (weixinQt) {
            PostMessageW(send, WM_NULL, 0, 0);
        }
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
