#include "background_window_input.h"

#include "action_utils.h"
#include "background_uia_input.h"
#include "window_coords.h"
#include "window_mode_log.h"
#include "window_mode_types.h"
#include "window_target.h"

#include <chrono>
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
    WPARAM flags = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) flags |= MK_CONTROL;
    if (GetKeyState(VK_SHIFT) & 0x8000) flags |= MK_SHIFT;
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

bool SendQuickInputViaEditMessages(HWND input, const std::wstring& text, double charInterval,
    const std::atomic_bool* cancelFlag) {
    if (!input || !IsWindow(input) || text.empty()) return false;

    wchar_t cls[256]{};
    GetClassNameW(input, cls, 256);
    if (!IsTextInputClass(cls)) return false;

    if (charInterval <= 0) {
        if (WindowModeCancelled(cancelFlag)) return true;
        SendMessageW(input, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        // EM_REPLACESEL 无文档化成功返回值（常为 0），已发送即视为成功
        SendMessageW(input, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
        return true;
    }

    for (wchar_t ch : text) {
        if (WindowModeCancelled(cancelFlag)) return true;
        if (ch == L'\r' || ch == L'\n') {
            SendMessageW(input, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\r\n"));
        } else {
            std::wstring one(1, ch);
            SendMessageW(input, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(one.c_str()));
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
    SendMessageW(input, WM_PASTE, 0, 0);
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

/// Soft focus without SetForegroundWindow / Z-order changes.
/// Avoid WM_ACTIVATE: many apps (e.g. Notepad) raise their top-level window on activate.
void PrimeWindowSoftFocus(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    NotifyWindowMessage(hwnd, WM_SETFOCUS, 0, 0);
}

WPARAM ButtonFlag(MouseButtonType button) {
    switch (button) {
    case MouseButtonType::Right: return MK_RBUTTON;
    case MouseButtonType::Middle: return MK_MBUTTON;
    default: return MK_LBUTTON;
    }
}

void ButtonMessages(MouseButtonType button, UINT& downMsg, UINT& upMsg) {
    switch (button) {
    case MouseButtonType::Right:
        downMsg = WM_RBUTTONDOWN;
        upMsg = WM_RBUTTONUP;
        break;
    case MouseButtonType::Middle:
        downMsg = WM_MBUTTONDOWN;
        upMsg = WM_MBUTTONUP;
        break;
    default:
        downMsg = WM_LBUTTONDOWN;
        upMsg = WM_LBUTTONUP;
        break;
    }
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

void ResetSoftMouseState() {
    g_softMouseFlags = 0;
    g_haveLastClientPos = false;
    g_lastClientX = 0;
    g_lastClientY = 0;
    g_lastClientHwnd = nullptr;
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
    HWND input = FindTextInputTarget(root);

    if (input) {
        PrimeWindowSoftFocus(input);
        if (SendQuickInputViaEditMessages(input, text, charInterval, cancelFlag)) return;
        if (WindowModeCancelled(cancelFlag)) return;
        if (SendQuickInputViaClipboard(input, text, true)) return;
    }

    if (WindowModeCancelled(cancelFlag)) return;
    if (SendQuickInputViaClipboard(root, text, true)) return;

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
    HWND target = ResolveMouseTarget(hwnd, hitX, hitY);
    if (!target) target = hwnd;

    PrimeWindowSoftFocus(target);

    const WPARAM moveFlags = ModifierKeyFlags() | g_softMouseFlags;
    NotifyWindowMessage(target, WM_MOUSEMOVE, moveFlags, MAKELPARAM(hitX, hitY));

    // WM_MOUSEWHEEL / WM_MOUSEHWHEEL lParam must be screen coordinates (MSDN).
    int sx = 0;
    int sy = 0;
    if (!ClientToScreenPoint(hwnd, cx, cy, sx, sy)) {
        POINT pt{hitX, hitY};
        if (ClientToScreen(target, &pt)) {
            sx = pt.x;
            sy = pt.y;
        } else {
            sx = cx;
            sy = cy;
        }
    }

    const SHORT delta = static_cast<SHORT>((positive ? WHEEL_DELTA : -WHEEL_DELTA) * steps);
    const WPARAM wp = MAKEWPARAM(static_cast<WORD>(ModifierKeyFlags() | g_softMouseFlags), delta);
    const LPARAM lp = MAKELPARAM(sx, sy);
    const UINT msg = vertical ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL;
    NotifyWindowMessage(target, msg, wp, lp);
}

void PostKeyToWindow(HWND hwnd, UINT vk, bool down) {
    if (!hwnd || !IsWindow(hwnd) || vk == 0) return;
    PrimeWindowSoftFocus(hwnd);
    const UINT msg = down ? WM_KEYDOWN : WM_KEYUP;
    NotifyWindowMessage(hwnd, msg, vk, BuildKeyLParam(vk, down));
}

void PostMouseMoveToWindow(HWND hwnd, int cx, int cy) {
    if (!hwnd || !IsWindow(hwnd)) return;

    RememberClientPos(hwnd, cx, cy);

    int hitX = cx;
    int hitY = cy;
    HWND target = ResolveMouseTarget(hwnd, hitX, hitY);
    if (!target) target = hwnd;

    const WPARAM wp = ModifierKeyFlags() | g_softMouseFlags;
    NotifyWindowMessage(target, WM_MOUSEMOVE, wp, MAKELPARAM(hitX, hitY));
}

void PostMouseButtonToWindow(HWND hwnd, int cx, int cy, MouseButtonType button, bool down) {
    if (!hwnd || !IsWindow(hwnd)) return;

    RememberClientPos(hwnd, cx, cy);

    int hitX = cx;
    int hitY = cy;
    HWND target = ResolveMouseTarget(hwnd, hitX, hitY);
    if (!target) target = hwnd;

    PrimeWindowSoftFocus(target);

    const WPARAM buttonFlag = ButtonFlag(button);
    UINT downMsg = WM_LBUTTONDOWN;
    UINT upMsg = WM_LBUTTONUP;
    ButtonMessages(button, downMsg, upMsg);

    // Move first so the target sees the cursor at the click point.
    NotifyWindowMessage(target, WM_MOUSEMOVE,
        ModifierKeyFlags() | g_softMouseFlags | (down ? buttonFlag : 0),
        MAKELPARAM(hitX, hitY));

    if (down) {
        g_softMouseFlags |= buttonFlag;
        NotifyWindowMessage(target, downMsg, ModifierKeyFlags() | g_softMouseFlags,
            MAKELPARAM(hitX, hitY));
    } else {
        // Button-up: flag for this button should already be clear in wParam after release.
        NotifyWindowMessage(target, upMsg, ModifierKeyFlags() | (g_softMouseFlags & ~buttonFlag),
            MAKELPARAM(hitX, hitY));
        g_softMouseFlags &= ~buttonFlag;
    }
}

}  // namespace windowmode
