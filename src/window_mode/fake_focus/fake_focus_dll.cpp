#include "fake_focus_api.h"
#include "fake_focus_hook.h"
#include "fake_focus_soft_input.h"

#include <atomic>
#include <cstring>

#include <dwmapi.h>

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

namespace {

std::atomic<HWND> g_targetTop{nullptr};
std::atomic<HWND> g_focusHwnd{nullptr};
std::atomic_bool g_installed{false};

WNDPROC g_oldWndProc = nullptr;
HWND g_subclassHwnd = nullptr;

fakefocus::InlineHook g_hookFg{};
fakefocus::InlineHook g_hookActive{};
fakefocus::InlineHook g_hookFocus{};
fakefocus::InlineHook g_hookCursor{};
fakefocus::InlineHook g_hookAsyncKey{};
fakefocus::InlineHook g_hookKeyState{};
fakefocus::InlineHook g_hookKeyboardState{};
fakefocus::InlineHook g_hookIsVisible{};
fakefocus::InlineHook g_hookDwmAttr{};

HANDLE g_softMapping = nullptr;
fakefocus::SoftInputState* g_softView = nullptr;
DWORD g_softPid = 0;

bool OpenSoftInputView(DWORD softPid) {
    if (g_softView) return true;
    if (softPid == 0) softPid = GetCurrentProcessId();
    g_softPid = softPid;
    wchar_t name[128]{};
    fakefocus::SoftInputMappingName(softPid, name, 128);
    g_softMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!g_softMapping) return false;
    g_softView = static_cast<fakefocus::SoftInputState*>(
        MapViewOfFile(g_softMapping, FILE_MAP_READ, 0, 0, sizeof(fakefocus::SoftInputState)));
    if (!g_softView) {
        CloseHandle(g_softMapping);
        g_softMapping = nullptr;
        return false;
    }
    return fakefocus::SoftInputStateLooksValid(g_softView);
}

void CloseSoftInputView() {
    if (g_softView) {
        UnmapViewOfFile(g_softView);
        g_softView = nullptr;
    }
    if (g_softMapping) {
        CloseHandle(g_softMapping);
        g_softMapping = nullptr;
    }
    g_softPid = 0;
}

const fakefocus::SoftInputState* SoftState() {
    if (!g_softView) OpenSoftInputView(g_softPid);
    if (!fakefocus::SoftInputStateLooksValid(g_softView)) return nullptr;
    return g_softView;
}

bool IsMouseVk(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON
        || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

bool IsOurHwnd(HWND hwnd) {
    if (!hwnd) return false;
    HWND top = g_targetTop.load(std::memory_order_relaxed);
    if (!top || !IsWindow(top)) return false;
    if (hwnd == top) return true;
    return IsChild(top, hwnd) != FALSE;
}

void* CallOrigFg(void*) {
    return reinterpret_cast<void*>(GetForegroundWindow());
}
void* CallOrigActive(void*) {
    return reinterpret_cast<void*>(GetActiveWindow());
}
void* CallOrigFocus(void*) {
    return reinterpret_cast<void*>(GetFocus());
}

struct CursorCallCtx {
    LPPOINT pt = nullptr;
    BOOL ok = FALSE;
};
void* CallOrigCursor(void* raw) {
    auto* ctx = static_cast<CursorCallCtx*>(raw);
    ctx->ok = GetCursorPos(ctx->pt);
    return nullptr;
}

struct AsyncKeyCallCtx {
    int vk = 0;
    SHORT result = 0;
};
void* CallOrigAsyncKey(void* raw) {
    auto* ctx = static_cast<AsyncKeyCallCtx*>(raw);
    ctx->result = GetAsyncKeyState(ctx->vk);
    return nullptr;
}

struct KeyStateCallCtx {
    int vk = 0;
    SHORT result = 0;
};
void* CallOrigKeyState(void* raw) {
    auto* ctx = static_cast<KeyStateCallCtx*>(raw);
    ctx->result = GetKeyState(ctx->vk);
    return nullptr;
}

struct KeyboardStateCallCtx {
    PBYTE keys = nullptr;
    BOOL ok = FALSE;
};
void* CallOrigKeyboardState(void* raw) {
    auto* ctx = static_cast<KeyboardStateCallCtx*>(raw);
    ctx->ok = GetKeyboardState(ctx->keys);
    return nullptr;
}

struct VisibleCallCtx {
    HWND hwnd = nullptr;
    BOOL ok = FALSE;
};
void* CallOrigVisible(void* raw) {
    auto* ctx = static_cast<VisibleCallCtx*>(raw);
    ctx->ok = IsWindowVisible(ctx->hwnd);
    return nullptr;
}

struct DwmAttrCallCtx {
    HWND hwnd = nullptr;
    DWORD attr = 0;
    PVOID pv = nullptr;
    DWORD cb = 0;
    HRESULT hr = E_FAIL;
};
void* CallOrigDwmAttr(void* raw) {
    auto* ctx = static_cast<DwmAttrCallCtx*>(raw);
    ctx->hr = DwmGetWindowAttribute(ctx->hwnd, ctx->attr, ctx->pv, ctx->cb);
    return nullptr;
}

HWND CallOriginalForeground() {
    void* result = nullptr;
    if (fakefocus::CallThroughOriginal(g_hookFg, &CallOrigFg, nullptr, &result)) {
        return reinterpret_cast<HWND>(result);
    }
    return nullptr;
}

HWND CallOriginalActive() {
    void* result = nullptr;
    if (fakefocus::CallThroughOriginal(g_hookActive, &CallOrigActive, nullptr, &result)) {
        return reinterpret_cast<HWND>(result);
    }
    return nullptr;
}

HWND CallOriginalFocus() {
    void* result = nullptr;
    if (fakefocus::CallThroughOriginal(g_hookFocus, &CallOrigFocus, nullptr, &result)) {
        return reinterpret_cast<HWND>(result);
    }
    return nullptr;
}

BOOL CallOriginalCursorPos(LPPOINT pt) {
    CursorCallCtx ctx{pt, FALSE};
    fakefocus::CallThroughOriginal(g_hookCursor, &CallOrigCursor, &ctx, nullptr);
    return ctx.ok;
}

SHORT CallOriginalAsyncKeyState(int vk) {
    AsyncKeyCallCtx ctx{vk, 0};
    fakefocus::CallThroughOriginal(g_hookAsyncKey, &CallOrigAsyncKey, &ctx, nullptr);
    return ctx.result;
}

SHORT CallOriginalKeyState(int vk) {
    KeyStateCallCtx ctx{vk, 0};
    fakefocus::CallThroughOriginal(g_hookKeyState, &CallOrigKeyState, &ctx, nullptr);
    return ctx.result;
}

BOOL CallOriginalKeyboardState(PBYTE keys) {
    KeyboardStateCallCtx ctx{keys, FALSE};
    fakefocus::CallThroughOriginal(g_hookKeyboardState, &CallOrigKeyboardState, &ctx, nullptr);
    return ctx.ok;
}

BOOL CallOriginalIsWindowVisible(HWND hwnd) {
    VisibleCallCtx ctx{hwnd, FALSE};
    fakefocus::CallThroughOriginal(g_hookIsVisible, &CallOrigVisible, &ctx, nullptr);
    return ctx.ok;
}

HRESULT CallOriginalDwmGetWindowAttribute(HWND hwnd, DWORD attr, PVOID pv, DWORD cb) {
    DwmAttrCallCtx ctx{hwnd, attr, pv, cb, E_FAIL};
    fakefocus::CallThroughOriginal(g_hookDwmAttr, &CallOrigDwmAttr, &ctx, nullptr);
    return ctx.hr;
}

HWND WINAPI Hook_GetForegroundWindow() {
    HWND fake = g_targetTop.load(std::memory_order_relaxed);
    if (fake && IsWindow(fake)) return fake;
    return CallOriginalForeground();
}

HWND WINAPI Hook_GetActiveWindow() {
    HWND fake = g_targetTop.load(std::memory_order_relaxed);
    if (fake && IsWindow(fake)) return fake;
    return CallOriginalActive();
}

HWND WINAPI Hook_GetFocus() {
    HWND focus = g_focusHwnd.load(std::memory_order_relaxed);
    if (focus && IsWindow(focus)) return focus;
    HWND fake = g_targetTop.load(std::memory_order_relaxed);
    if (fake && IsWindow(fake)) return fake;
    return CallOriginalFocus();
}

BOOL WINAPI Hook_GetCursorPos(LPPOINT pt) {
    if (!pt) return FALSE;
    const fakefocus::SoftInputState* st = SoftState();
    if (st && (st->flags & fakefocus::kSoftFlagCursorValid)) {
        pt->x = st->cursorScreenX;
        pt->y = st->cursorScreenY;
        return TRUE;
    }
    return CallOriginalCursorPos(pt);
}

SHORT SoftKeyDownShort(int vKey, bool asyncStyle) {
    const fakefocus::SoftInputState* st = SoftState();
    if (!st || !(st->flags & fakefocus::kSoftFlagKeysValid)) return 0;
    const int vk = vKey & 0xFF;
    if (vk < 0 || vk >= 256 || !st->down[static_cast<size_t>(vk)]) {
        if (IsMouseVk(vk) && (st->flags & fakefocus::kSoftFlagCursorValid)) return 0;
        return 0;
    }
    return asyncStyle ? static_cast<SHORT>(0x8000) : static_cast<SHORT>(0xFF80);
}

SHORT WINAPI Hook_GetAsyncKeyState(int vKey) {
    if (SHORT soft = SoftKeyDownShort(vKey, true)) return soft;
    const fakefocus::SoftInputState* st = SoftState();
    if (st && (st->flags & fakefocus::kSoftFlagKeysValid)) {
        const int vk = vKey & 0xFF;
        if (IsMouseVk(vk) && (st->flags & fakefocus::kSoftFlagCursorValid)) return 0;
    }
    return CallOriginalAsyncKeyState(vKey);
}

SHORT WINAPI Hook_GetKeyState(int nVirtKey) {
    if (SHORT soft = SoftKeyDownShort(nVirtKey, false)) return soft;
    const fakefocus::SoftInputState* st = SoftState();
    if (st && (st->flags & fakefocus::kSoftFlagKeysValid)) {
        const int vk = nVirtKey & 0xFF;
        if (IsMouseVk(vk) && (st->flags & fakefocus::kSoftFlagCursorValid)) return 0;
    }
    return CallOriginalKeyState(nVirtKey);
}

BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState) {
    if (!lpKeyState) return FALSE;
    if (!CallOriginalKeyboardState(lpKeyState)) return FALSE;

    const fakefocus::SoftInputState* st = SoftState();
    if (!st || !(st->flags & fakefocus::kSoftFlagKeysValid)) return TRUE;

    for (int i = 0; i < 256; ++i) {
        if (st->down[static_cast<size_t>(i)]) {
            lpKeyState[i] = static_cast<BYTE>(lpKeyState[i] | 0x80);
        } else if (IsMouseVk(i) && (st->flags & fakefocus::kSoftFlagCursorValid)) {
            lpKeyState[i] = static_cast<BYTE>(lpKeyState[i] & ~0x80);
        }
    }
    return TRUE;
}

BOOL WINAPI Hook_IsWindowVisible(HWND hwnd) {
    // 宏桌面 / Pin+Cloak 时系统常报不可见；Chromium 据此丢弃输入。
    if (IsOurHwnd(hwnd)) return TRUE;
    return CallOriginalIsWindowVisible(hwnd);
}

HRESULT WINAPI Hook_DwmGetWindowAttribute(HWND hwnd, DWORD dwAttribute,
    PVOID pvAttribute, DWORD cbAttribute) {
    // Edge 用 DWMWA_CLOAKED 判断遮挡；宿主侧可能 Cloak 以免露脸。
    if (dwAttribute == DWMWA_CLOAKED && IsOurHwnd(hwnd)
        && pvAttribute && cbAttribute >= sizeof(DWORD)) {
        *static_cast<DWORD*>(pvAttribute) = 0;
        return S_OK;
    }
    return CallOriginalDwmGetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);
}

bool IsDeactivate(WPARAM wp, UINT msg) {
    if (msg == WM_ACTIVATE) {
        return LOWORD(wp) == WA_INACTIVE;
    }
    if (msg == WM_ACTIVATEAPP) {
        return wp == FALSE;
    }
    if (msg == WM_NCACTIVATE) {
        return wp == FALSE;
    }
    if (msg == WM_KILLFOCUS) {
        return true;
    }
    return false;
}

LRESULT CALLBACK FakeFocusWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (IsDeactivate(wp, msg)) {
        return 0;
    }
    if (g_oldWndProc) {
        return CallWindowProcW(g_oldWndProc, hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ShouldAttachFakeFocusSubclass(HWND top) {
    if (!top || !IsWindow(top)) return false;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (_wcsnicmp(cls, L"Chrome_", 7) == 0) return false;
    if (_wcsicmp(cls, L"MozillaWindowClass") == 0) return false;
    if (_wcsicmp(cls, L"ApplicationFrameWindow") == 0) return false;
    return true;
}

bool AttachSubclass(HWND top) {
    if (!ShouldAttachFakeFocusSubclass(top)) return true;
    if (!top || !IsWindow(top)) return false;
    if (g_subclassHwnd && g_subclassHwnd == top && g_oldWndProc) return true;

    if (g_subclassHwnd && g_oldWndProc && IsWindow(g_subclassHwnd)) {
        SetWindowLongPtrW(g_subclassHwnd, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(g_oldWndProc));
    }
    g_subclassHwnd = nullptr;
    g_oldWndProc = nullptr;

    LONG_PTR prev = SetWindowLongPtrW(top, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(FakeFocusWndProc));
    if (!prev) return false;
    g_oldWndProc = reinterpret_cast<WNDPROC>(prev);
    g_subclassHwnd = top;
    return true;
}

void DetachSubclass() {
    if (g_subclassHwnd && g_oldWndProc && IsWindow(g_subclassHwnd)) {
        SetWindowLongPtrW(g_subclassHwnd, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(g_oldWndProc));
    }
    g_subclassHwnd = nullptr;
    g_oldWndProc = nullptr;
}

void RemoveAllHooks() {
    fakefocus::RemoveInlineHook(g_hookDwmAttr);
    fakefocus::RemoveInlineHook(g_hookIsVisible);
    fakefocus::RemoveInlineHook(g_hookKeyboardState);
    fakefocus::RemoveInlineHook(g_hookKeyState);
    fakefocus::RemoveInlineHook(g_hookAsyncKey);
    fakefocus::RemoveInlineHook(g_hookCursor);
    fakefocus::RemoveInlineHook(g_hookFocus);
    fakefocus::RemoveInlineHook(g_hookActive);
    fakefocus::RemoveInlineHook(g_hookFg);
}

bool InstallPhase2Hooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    void* pCursor = reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos"));
    void* pAsync = reinterpret_cast<void*>(GetProcAddress(user32, "GetAsyncKeyState"));
    void* pKeyState = reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyState"));
    void* pKeys = reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyboardState"));
    void* pVisible = reinterpret_cast<void*>(GetProcAddress(user32, "IsWindowVisible"));
    if (!pCursor || !pAsync || !pKeyState || !pKeys || !pVisible) return false;

    if (!fakefocus::InstallInlineHook(g_hookCursor, pCursor, reinterpret_cast<void*>(&Hook_GetCursorPos))) {
        return false;
    }
    if (!fakefocus::InstallInlineHook(g_hookAsyncKey, pAsync, reinterpret_cast<void*>(&Hook_GetAsyncKeyState))) {
        fakefocus::RemoveInlineHook(g_hookCursor);
        return false;
    }
    if (!fakefocus::InstallInlineHook(g_hookKeyState, pKeyState, reinterpret_cast<void*>(&Hook_GetKeyState))) {
        fakefocus::RemoveInlineHook(g_hookAsyncKey);
        fakefocus::RemoveInlineHook(g_hookCursor);
        return false;
    }
    if (!fakefocus::InstallInlineHook(g_hookKeyboardState, pKeys, reinterpret_cast<void*>(&Hook_GetKeyboardState))) {
        fakefocus::RemoveInlineHook(g_hookKeyState);
        fakefocus::RemoveInlineHook(g_hookAsyncKey);
        fakefocus::RemoveInlineHook(g_hookCursor);
        return false;
    }
    if (!fakefocus::InstallInlineHook(g_hookIsVisible, pVisible, reinterpret_cast<void*>(&Hook_IsWindowVisible))) {
        fakefocus::RemoveInlineHook(g_hookKeyboardState);
        fakefocus::RemoveInlineHook(g_hookKeyState);
        fakefocus::RemoveInlineHook(g_hookAsyncKey);
        fakefocus::RemoveInlineHook(g_hookCursor);
        return false;
    }

    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        void* pDwm = reinterpret_cast<void*>(GetProcAddress(dwm, "DwmGetWindowAttribute"));
        if (pDwm) {
            fakefocus::InstallInlineHook(g_hookDwmAttr, pDwm,
                reinterpret_cast<void*>(&Hook_DwmGetWindowAttribute));
        }
    }
    return true;
}

}  // namespace

BOOL WINAPI FakeFocus_Install(HWND targetTop) {
    if (!targetTop || !IsWindow(targetTop)) return FALSE;
    HWND top = GetAncestor(targetTop, GA_ROOT);
    if (!top) top = targetTop;

    if (g_installed.load(std::memory_order_relaxed)) {
        return FakeFocus_UpdateTarget(top);
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return FALSE;

    void* pFg = reinterpret_cast<void*>(GetProcAddress(user32, "GetForegroundWindow"));
    void* pActive = reinterpret_cast<void*>(GetProcAddress(user32, "GetActiveWindow"));
    void* pFocus = reinterpret_cast<void*>(GetProcAddress(user32, "GetFocus"));
    if (!pFg || !pActive || !pFocus) return FALSE;

    DWORD softPid = 0;
    GetWindowThreadProcessId(top, &softPid);
    OpenSoftInputView(softPid);

    if (!fakefocus::InstallInlineHook(g_hookFg, pFg, reinterpret_cast<void*>(&Hook_GetForegroundWindow))) {
        CloseSoftInputView();
        return FALSE;
    }
    if (!fakefocus::InstallInlineHook(g_hookActive, pActive, reinterpret_cast<void*>(&Hook_GetActiveWindow))) {
        fakefocus::RemoveInlineHook(g_hookFg);
        CloseSoftInputView();
        return FALSE;
    }
    if (!fakefocus::InstallInlineHook(g_hookFocus, pFocus, reinterpret_cast<void*>(&Hook_GetFocus))) {
        fakefocus::RemoveInlineHook(g_hookActive);
        fakefocus::RemoveInlineHook(g_hookFg);
        CloseSoftInputView();
        return FALSE;
    }
    if (!InstallPhase2Hooks()) {
        RemoveAllHooks();
        CloseSoftInputView();
        return FALSE;
    }

    g_targetTop.store(top, std::memory_order_relaxed);
    g_focusHwnd.store(top, std::memory_order_relaxed);
    AttachSubclass(top);
    g_installed.store(true, std::memory_order_release);
    return TRUE;
}

BOOL WINAPI FakeFocus_UpdateTarget(HWND targetTop) {
    if (!g_installed.load(std::memory_order_acquire)) return FALSE;
    if (!targetTop || !IsWindow(targetTop)) return FALSE;
    HWND top = GetAncestor(targetTop, GA_ROOT);
    if (!top) top = targetTop;
    g_targetTop.store(top, std::memory_order_relaxed);
    g_focusHwnd.store(top, std::memory_order_relaxed);
    AttachSubclass(top);
    if (!g_softView) {
        DWORD softPid = 0;
        GetWindowThreadProcessId(top, &softPid);
        OpenSoftInputView(softPid);
    }
    return TRUE;
}

BOOL WINAPI FakeFocus_Uninstall(void) {
    if (!g_installed.load(std::memory_order_acquire)) {
        CloseSoftInputView();
        return TRUE;
    }

    DetachSubclass();
    RemoveAllHooks();
    CloseSoftInputView();
    g_targetTop.store(nullptr, std::memory_order_relaxed);
    g_focusHwnd.store(nullptr, std::memory_order_relaxed);
    g_installed.store(false, std::memory_order_release);
    return TRUE;
}

BOOL WINAPI FakeFocus_IsInstalled(void) {
    return g_installed.load(std::memory_order_acquire) ? TRUE : FALSE;
}

BOOL WINAPI FakeFocus_HasSoftInput(void) {
    return SoftState() ? TRUE : FALSE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        FakeFocus_Uninstall();
    }
    return TRUE;
}
