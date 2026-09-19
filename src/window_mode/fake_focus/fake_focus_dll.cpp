#include "fake_focus_api.h"
#include "fake_focus_hook.h"
#include "fake_focus_soft_input.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>

#include <dwmapi.h>

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

namespace {

std::atomic<HWND> g_targetTop{nullptr};
std::atomic<HWND> g_focusHwnd{nullptr};
std::atomic_bool g_installed{false};
bool g_liteMode = false;
bool g_electronSafe = false;
bool g_airSafe = false;
bool g_weixinSafe = false;
bool g_tianlongSafe = false;
bool g_mapleSafe = false;
bool g_desktopEmuSafe = false;  // DeSmuME/Dolphin：极简假焦点，禁 RawInput/WM_INPUT
volatile LONG g_mapleHitGaks = 0;
volatile LONG g_mapleHitDiState = 0;
volatile LONG g_mapleHitDiData = 0;
volatile LONG g_mapleLastDiStateCb = 0;
volatile LONG g_mapleHitGfw = 0;
volatile LONG g_mapleHitFocus = 0;
fakefocus::SoftInputState* g_softView = nullptr;
bool g_softWritable = false;
int g_mapleInputHookCount = 0;
DWORD g_mapleDiag = 0;
int g_diVtPatchN = 0;
int g_mapleFoundVt = 0;
int g_mapleHeapVt = 0;
void MaplePublishHits() {
    if (!g_softWritable || !g_softView) return;
    if (!fakefocus::SoftInputStateLooksValid(g_softView)) return;
    g_softView->hitGaks = static_cast<uint32_t>(g_mapleHitGaks);
    g_softView->hitDiState = static_cast<uint32_t>(g_mapleHitDiState);
    g_softView->hitDiData = static_cast<uint32_t>(g_mapleHitDiData);
    g_softView->lastDiStateCb = static_cast<uint32_t>(g_mapleLastDiStateCb);
    g_softView->hitGfw = static_cast<uint32_t>(g_mapleHitGfw);
    g_softView->hitFocus = static_cast<uint32_t>(g_mapleHitFocus);
    g_softView->hitReady = 1;
    g_softView->mapleDiag = g_mapleDiag;
    g_softView->mapleIatPoll = static_cast<uint32_t>(g_mapleInputHookCount);
    const uint32_t foundVt = static_cast<uint32_t>(g_mapleFoundVt) & 0xFFu;
    const uint32_t patched = static_cast<uint32_t>(g_diVtPatchN) & 0xFFu;
    const uint32_t heapVt = static_cast<uint32_t>(g_mapleHeapVt) & 0xFFu;
    g_softView->mapleDiVt = foundVt | (patched << 8) | (heapVt << 16);
}
void MapleNoteDiStateCb(DWORD cb) {
    const LONG store = cb > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<LONG>(cb);
    InterlockedExchange(&g_mapleLastDiStateCb, store);
}
void MapleBumpHit(volatile LONG* c) {
    if (!c) return;
    const LONG v = InterlockedIncrement(c);
    if (v > 255) InterlockedExchange(c, 255);
    MaplePublishHits();
}

// mapleDiag 高位 = 运行期「这个钩子至少被调用过一次」标记。
// 低位（0x0001..0x10000）是**安装**位，高位是**命中**位，别混用。
// 宿主日志会把这些位解成人话，用来判断客户端到底走哪条输入路径：
//   IAT 直呼 / GetProcAddress 动态解析 / 全都不走（那就说明它是消息驱动 + 别的键态源）。
// 背景：星辰冒险岛后台只原地平A、不走路的日志里，gfw/gaks/diState/lastCb 全是 0，
// 光看计数器无法区分「钩子没装上」和「客户端根本不调这些 API」。
constexpr DWORD kMapleCalledKeyState = 0x0020000u;   // Hook_GetKeyState
constexpr DWORD kMapleCalledKbState = 0x0040000u;    // Hook_GetKeyboardState
constexpr DWORD kMapleCalledCursor = 0x0080000u;     // Hook_GetCursorPos
constexpr DWORD kMapleCalledGpa = 0x0100000u;        // Hook_GetProcAddress
constexpr DWORD kMapleIatGpaPatched = 0x0200000u;    // GetProcAddress 的 IAT 槽补到过
constexpr DWORD kMapleIatDinputWalked = 0x0400000u;  // dinput8/dinput 的 user32 IAT 补到过槽

void MapleMarkCalled(DWORD bit) {
    if ((g_mapleDiag & bit) != 0) return;
    g_mapleDiag |= bit;
    MaplePublishHits();
}

void MapleResetHookHits() {
    InterlockedExchange(&g_mapleHitGaks, 0);
    InterlockedExchange(&g_mapleHitDiState, 0);
    InterlockedExchange(&g_mapleHitDiData, 0);
    InterlockedExchange(&g_mapleLastDiStateCb, 0);
    InterlockedExchange(&g_mapleHitGfw, 0);
    InterlockedExchange(&g_mapleHitFocus, 0);
    MaplePublishHits();
}
BOOL (WINAPI* g_mapleRealGetCursorPos)(LPPOINT) = nullptr;
BOOL (WINAPI* g_mapleRealSetCursorPos)(int, int) = nullptr;
BOOL (WINAPI* g_mapleRealClipCursor)(const RECT*) = nullptr;
int (WINAPI* g_mapleRealShowCursor)(BOOL) = nullptr;
SHORT (WINAPI* g_mapleRealGetAsyncKeyState)(int) = nullptr;
SHORT (WINAPI* g_mapleRealGetKeyState)(int) = nullptr;
BOOL (WINAPI* g_mapleRealGetKeyboardState)(PBYTE) = nullptr;
HWND (WINAPI* g_mapleRealGetForegroundWindow)() = nullptr;
BOOL (WINAPI* g_mapleRealSetForegroundWindow)(HWND) = nullptr;
BOOL (WINAPI* g_mapleRealFlashWindow)(HWND, BOOL) = nullptr;
BOOL (WINAPI* g_mapleRealFlashWindowEx)(PFLASHWINFO) = nullptr;
void (WINAPI* g_mapleRealSwitchToThisWindow)(HWND, BOOL) = nullptr;
HWND (WINAPI* g_mapleRealGetActiveWindow)() = nullptr;
HWND (WINAPI* g_mapleRealGetFocus)() = nullptr;
BOOL (WINAPI* g_mapleRealIsIconic)(HWND) = nullptr;
BOOL (WINAPI* g_mapleRealIsWindowVisible)(HWND) = nullptr;
void* g_mapleNtUserGfw = nullptr;
void* g_mapleNtUserGaks = nullptr;
void* g_mapleNtUserKeyState = nullptr;
void* g_mapleNtUserKbState = nullptr;
void* g_mapleNtUserCursor = nullptr;
void* g_mapleNtUserSetCursor = nullptr;
HANDLE g_drainThread = nullptr;
std::atomic_bool g_drainStop{false};

void DrainSoftKeyEventsPost();
void MaybePostFakeWmInput();
void StopSoftKeyDrainThread();
bool ProcessImageLooksLikeDesktopEmu(HWND top);
bool ProcessImageLooksLikeWeixin(HWND top);
bool HwndLooksLikeWeixinClient(HWND top);
bool ClassLooksLikeTianLongBaBu(const wchar_t* cls);
bool LooksLikeDesktopEmuClassName(const wchar_t* cls);

WNDPROC g_oldWndProc = nullptr;
HWND g_subclassHwnd = nullptr;

fakefocus::InlineHook g_hookFg{};
fakefocus::InlineHook g_hookSetFg{};
fakefocus::InlineHook g_hookActive{};
fakefocus::InlineHook g_hookFocus{};
fakefocus::InlineHook g_hookCursor{};
fakefocus::InlineHook g_hookSetCursor{};
fakefocus::InlineHook g_hookClipCursor{};
fakefocus::InlineHook g_hookSetCapture{};
fakefocus::InlineHook g_hookShowCursor{};
fakefocus::InlineHook g_hookAsyncKey{};
fakefocus::InlineHook g_hookKeyState{};
fakefocus::InlineHook g_hookKeyboardState{};
fakefocus::InlineHook g_hookIsVisible{};
fakefocus::InlineHook g_hookDwmAttr{};
fakefocus::InlineHook g_hookDiLiveAcquire[8]{};
fakefocus::InlineHook g_hookDiLiveState[8]{};
constexpr int kMapleLiveDiN = 8;

HANDLE g_softMapping = nullptr;
DWORD g_softPid = 0;
HHOOK g_focusGuardHook = nullptr;
void CloseSoftInputView();

bool OpenSoftInputView(DWORD softPid) {
    if (g_softView) {
        if (fakefocus::SoftInputStateLooksValid(g_softView)) return true;
        CloseSoftInputView();
    }
    if (softPid == 0) softPid = GetCurrentProcessId();
    g_softPid = softPid;
    wchar_t name[128]{};
    fakefocus::SoftInputMappingName(softPid, name, 128);
    // FILE_MAP_ALL_ACCESS 含 EXECUTE，PAGE_READWRITE 映射常会失败，命中全是 0。
    DWORD access = FILE_MAP_READ | FILE_MAP_WRITE;
    g_softMapping = OpenFileMappingW(access, FALSE, name);
    if (!g_softMapping) {
        access = FILE_MAP_READ;
        g_softMapping = OpenFileMappingW(access, FALSE, name);
    }
    if (!g_softMapping) return false;
    g_softView = static_cast<fakefocus::SoftInputState*>(
        MapViewOfFile(g_softMapping, access, 0, 0, sizeof(fakefocus::SoftInputState)));
    if (!g_softView) {
        CloseHandle(g_softMapping);
        g_softMapping = nullptr;
        return false;
    }
    g_softWritable = (access & FILE_MAP_WRITE) != 0;
    if (!fakefocus::SoftInputStateLooksValid(g_softView)) {
        CloseSoftInputView();
        return false;
    }
    if (g_softWritable) g_softView->hitReady = 1;
    return true;
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
    g_softWritable = false;
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

struct SetFgCallCtx {
    HWND hwnd = nullptr;
    BOOL ok = FALSE;
};
void* CallOrigSetFg(void* raw) {
    auto* ctx = static_cast<SetFgCallCtx*>(raw);
    ctx->ok = SetForegroundWindow(ctx->hwnd);
    return nullptr;
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

struct SetCursorCallCtx {
    int x = 0;
    int y = 0;
    BOOL ok = FALSE;
};
void* CallOrigSetCursor(void* raw) {
    auto* ctx = static_cast<SetCursorCallCtx*>(raw);
    ctx->ok = SetCursorPos(ctx->x, ctx->y);
    return nullptr;
}
BOOL CallOriginalSetCursorPos(int x, int y) {
    SetCursorCallCtx ctx{x, y, FALSE};
    fakefocus::CallThroughOriginal(g_hookSetCursor, &CallOrigSetCursor, &ctx, nullptr);
    return ctx.ok;
}

struct ClipCursorCallCtx {
    const RECT* rc = nullptr;
    BOOL ok = FALSE;
};
void* CallOrigClipCursor(void* raw) {
    auto* ctx = static_cast<ClipCursorCallCtx*>(raw);
    ctx->ok = ClipCursor(ctx->rc);
    return nullptr;
}
BOOL CallOriginalClipCursor(const RECT* rc) {
    ClipCursorCallCtx ctx{rc, FALSE};
    fakefocus::CallThroughOriginal(g_hookClipCursor, &CallOrigClipCursor, &ctx, nullptr);
    return ctx.ok;
}

struct SetCaptureCallCtx {
    HWND hwnd = nullptr;
    HWND prev = nullptr;
};
void* CallOrigSetCapture(void* raw) {
    auto* ctx = static_cast<SetCaptureCallCtx*>(raw);
    ctx->prev = SetCapture(ctx->hwnd);
    return nullptr;
}
HWND CallOriginalSetCapture(HWND hwnd) {
    SetCaptureCallCtx ctx{hwnd, nullptr};
    fakefocus::CallThroughOriginal(g_hookSetCapture, &CallOrigSetCapture, &ctx, nullptr);
    return ctx.prev;
}

struct ShowCursorCallCtx {
    BOOL show = FALSE;
    int count = 0;
};
void* CallOrigShowCursor(void* raw) {
    auto* ctx = static_cast<ShowCursorCallCtx*>(raw);
    ctx->count = ShowCursor(ctx->show);
    return nullptr;
}
int CallOriginalShowCursor(BOOL show) {
    ShowCursorCallCtx ctx{show, 0};
    fakefocus::CallThroughOriginal(g_hookShowCursor, &CallOrigShowCursor, &ctx, nullptr);
    return ctx.count;
}

bool SoftCursorSwallowsWarp() {
    if (g_electronSafe) return false;
    // lite（GLFW/UE5）：游戏一旦以为自己有焦点就会 SetCursorPos/ClipCursor，
    // 不能等软光标播种之后才吞，否则前台真光标会被夹走。
    if (g_liteMode) return true;
    const fakefocus::SoftInputState* st = SoftState();
    return st && (st->flags & fakefocus::kSoftFlagCursorValid);
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

HWND CallMapleOrInlineForeground() {
    if (g_mapleSafe) return g_targetTop.load(std::memory_order_relaxed);
    if (g_mapleRealGetForegroundWindow) return g_mapleRealGetForegroundWindow();
    return CallOriginalForeground();
}

HWND WINAPI Hook_GetForegroundWindow() {
    if (g_mapleSafe) MapleBumpHit(&g_mapleHitGfw);
    // AIR/冒险岛只骗前景查询：不要在游戏线程顺带灌键/WM_ACTIVATE。
    if (!g_airSafe && !g_weixinSafe && !g_tianlongSafe && !g_mapleSafe) DrainSoftKeyEventsPost();
    HWND fake = g_targetTop.load(std::memory_order_relaxed);
    if (fake && IsWindow(fake)) return fake;
    // 方法体 JMP 后不能再进 user32/win32u 原函数（会递归）。
    if (g_mapleSafe) return fake;
    return CallMapleOrInlineForeground();
}

BOOL CallOriginalSetForegroundWindow(HWND hwnd) {
    SetFgCallCtx ctx{hwnd, FALSE};
    fakefocus::CallThroughOriginal(g_hookSetFg, &CallOrigSetFg, &ctx, nullptr);
    return ctx.ok;
}

BOOL CallMapleOrInlineSetForeground(HWND hwnd) {
    if (g_mapleSafe) return TRUE;
    if (g_mapleRealSetForegroundWindow) return g_mapleRealSetForegroundWindow(hwnd);
    return CallOriginalSetForegroundWindow(hwnd);
}

BOOL WINAPI Hook_SetForegroundWindow(HWND hwnd) {
    HWND fake = g_targetTop.load(std::memory_order_relaxed);
    if (g_mapleSafe) {
        (void)hwnd;
        (void)fake;
        // 方法体 JMP 后不能再调 user32 原函数（会递归，且真实 SetFG 会闪边框）。
        return TRUE;
    }
    if (fake && IsWindow(fake) && (hwnd == fake || (hwnd && IsChild(fake, hwnd)))) {
        HWND realFg = CallMapleOrInlineForeground();
        if (realFg == fake || (realFg && (realFg == hwnd || IsChild(fake, realFg)))) {
            // 真实前台已是目标（独占全屏）：不要吞掉，否则 DXGI Present 会停。
            return CallMapleOrInlineSetForeground(hwnd);
        }
        // 吞掉目标进程把自己抢到前台；允许把前台还回去。
        return TRUE;
    }
    return CallMapleOrInlineSetForeground(hwnd);
}

BOOL WINAPI Hook_FlashWindow(HWND, BOOL) {
    return TRUE;
}

BOOL WINAPI Hook_FlashWindowEx(PFLASHWINFO) {
    return TRUE;
}

void WINAPI Hook_SwitchToThisWindow(HWND hwnd, BOOL fAltTab) {
    if (g_mapleSafe) return;
    HWND fake = g_targetTop.load(std::memory_order_relaxed);
    if (fake && IsWindow(fake) && (hwnd == fake || (hwnd && IsChild(fake, hwnd)))) {
        HWND realFg = CallMapleOrInlineForeground();
        if (realFg == fake || (realFg && (realFg == hwnd || IsChild(fake, realFg)))) {
            if (g_mapleRealSwitchToThisWindow) g_mapleRealSwitchToThisWindow(hwnd, fAltTab);
            return;
        }
        return;
    }
    if (g_mapleRealSwitchToThisWindow) g_mapleRealSwitchToThisWindow(hwnd, fAltTab);
}

HWND WINAPI Hook_GetActiveWindow() {
    if (g_mapleSafe) MapleBumpHit(&g_mapleHitGfw);
    HWND fake = g_targetTop.load(std::memory_order_relaxed);
    if (fake && IsWindow(fake)) return fake;
    if (g_mapleSafe) return fake;
    if (g_mapleRealGetActiveWindow) return g_mapleRealGetActiveWindow();
    return CallOriginalActive();
}

HWND WINAPI Hook_GetFocus() {
    if (g_mapleSafe) MapleBumpHit(&g_mapleHitFocus);
    HWND focus = g_focusHwnd.load(std::memory_order_relaxed);
    if (focus && IsWindow(focus)) return focus;
    HWND fake = g_targetTop.load(std::memory_order_relaxed);
    if (fake && IsWindow(fake)) return fake;
    if (g_mapleSafe) return focus ? focus : fake;
    if (g_mapleRealGetFocus) return g_mapleRealGetFocus();
    return CallOriginalFocus();
}

BOOL WINAPI Hook_IsIconic(HWND hwnd) {
    HWND fake = g_targetTop.load(std::memory_order_relaxed);
    if (g_mapleSafe && fake && IsWindow(fake)
        && (hwnd == fake || (hwnd && IsChild(fake, hwnd)))) {
        return FALSE;
    }
    if (g_mapleRealIsIconic) return g_mapleRealIsIconic(hwnd);
    return FALSE;
}

HWND FindChromeRenderWidget(HWND top) {
    if (!top || !IsWindow(top)) return nullptr;
    struct Ctx { HWND found = nullptr; } ctx;
    EnumChildWindows(top, [](HWND w, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[256]{};
        GetClassNameW(w, cls, 256);
        if (wcsstr(cls, L"RenderWidgetHostHWND") != nullptr) {
            c->found = w;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

bool LooksLikeQtRenderClassName(const wchar_t* cls) {
    return cls && wcsstr(cls, L"QWindowIcon") != nullptr;
}

bool LooksLikeQtAndroidEmulatorTop(HWND top) {
    if (!top || !IsWindow(top)) return false;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    return LooksLikeQtRenderClassName(cls);
}

struct QtRenderPick {
    HWND best = nullptr;
    int bestDepth = -1;
    int bestArea = 0;
};

void VisitQtRenderChild(HWND hwnd, int depth, QtRenderPick* pick) {
    if (!hwnd || !pick || depth > 24) return;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (LooksLikeQtRenderClassName(cls)) {
        RECT rc{};
        if (GetClientRect(hwnd, &rc)) {
            const int area = std::max(0, static_cast<int>(rc.right - rc.left))
                * std::max(0, static_cast<int>(rc.bottom - rc.top));
            if (area >= 200 * 200
                && (depth > pick->bestDepth
                    || (depth == pick->bestDepth && area > pick->bestArea))) {
                pick->best = hwnd;
                pick->bestDepth = depth;
                pick->bestArea = area;
            }
        }
    }
    struct Ctx {
        QtRenderPick* pick = nullptr;
        int depth = 0;
    } ctx{pick, depth};
    EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        VisitQtRenderChild(child, c->depth + 1, c->pick);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

HWND FindQtAndroidRenderChild(HWND top) {
    if (!top || !IsWindow(top)) return nullptr;
    QtRenderPick pick{};
    VisitQtRenderChild(top, 0, &pick);
    return pick.best;
}

HWND ResolveSoftInputPostHwnd(HWND top) {
    if (!top || !IsWindow(top)) return nullptr;
    if (g_mapleSafe || g_airSafe || g_weixinSafe || g_tianlongSafe) return top;
    if (HWND render = FindChromeRenderWidget(top)) return render;
    if (HWND qt = FindQtAndroidRenderChild(top)) return qt;
    return top;
}

HWND SoftKeyPostHwnd() {
    HWND top = g_targetTop.load(std::memory_order_relaxed);
    if (HWND post = ResolveSoftInputPostHwnd(top)) return post;
    HWND focus = g_focusHwnd.load(std::memory_order_relaxed);
    if (focus && IsWindow(focus)) return focus;
    return top;
}

uint32_t g_keyEventRead = 0;
uint32_t g_mouseMoveRead = 0;
DWORD g_lastFocusRefreshMs = 0;

bool IsMouseVkLocal(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON
        || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

WPARAM MouseMkFlags(const fakefocus::SoftInputState* st) {
    WPARAM mk = 0;
    if (!st) return mk;
    if (st->down[VK_LBUTTON]) mk |= MK_LBUTTON;
    if (st->down[VK_RBUTTON]) mk |= MK_RBUTTON;
    if (st->down[VK_MBUTTON]) mk |= MK_MBUTTON;
    if (st->down[VK_XBUTTON1]) mk |= MK_XBUTTON1;
    if (st->down[VK_XBUTTON2]) mk |= MK_XBUTTON2;
    if (st->down[VK_SHIFT]) mk |= MK_SHIFT;
    if (st->down[VK_CONTROL]) mk |= MK_CONTROL;
    return mk;
}

void SoftRefreshFocusMessages() {
    HWND top = g_targetTop.load(std::memory_order_relaxed);
    HWND focus = SoftKeyPostHwnd();
    if (top && IsWindow(top)) {
        PostMessageW(top, WM_NCACTIVATE, TRUE, 0);
        PostMessageW(top, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
    }
    if (focus && IsWindow(focus)) {
        PostMessageW(focus, WM_SETFOCUS, 0, 0);
    }
}

LPARAM SoftClientLParam(HWND hwnd, const fakefocus::SoftInputState* st) {
    POINT pt{st->cursorScreenX, st->cursorScreenY};
    ScreenToClient(hwnd, &pt);
    return MAKELPARAM(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y));
}

void DrainSoftKeyEventsPost() {
    const fakefocus::SoftInputState* st = SoftState();
    if (!st || !(st->flags & fakefocus::kSoftFlagPostKeyEvents)) return;
    if (g_softPid == 0 || GetCurrentProcessId() != g_softPid) return;

    HWND hwnd = SoftKeyPostHwnd();
    if (!hwnd || !IsWindow(hwnd)) return;

    const DWORD now = GetTickCount();
    if (now - g_lastFocusRefreshMs >= 200) {
        g_lastFocusRefreshMs = now;
        SoftRefreshFocusMessages();
    }

    // 光标：只投递最新位置，避免 200+ 次外部 PostMessage 撑爆 Chromium。
    const uint32_t moveWrite = st->mouseMoveWrite;
    if (moveWrite != g_mouseMoveRead
        && (st->flags & fakefocus::kSoftFlagCursorValid)) {
        g_mouseMoveRead = moveWrite;
        PostMessageW(hwnd, WM_MOUSEMOVE, MouseMkFlags(st), SoftClientLParam(hwnd, st));
    }

    const uint32_t write = st->keyWrite;
    if (write < g_keyEventRead) {
        g_keyEventRead = write;
        return;
    }
    if (write - g_keyEventRead > fakefocus::kSoftKeyEventCap) {
        g_keyEventRead = write - fakefocus::kSoftKeyEventCap;
    }

    while (g_keyEventRead < write) {
        // 卸载时立刻收手：本函数由灌键线程调用，FreeLibrary 在另一个线程上等它退出。
        // 忙等里不看停止位会让 StopSoftKeyDrainThread 超时放行 → DLL 代码被解除映射后
        // 线程继续跑（表现为后续 LoadLibrary/FreeLibrary 卡死或崩溃）。
        if (g_drainStop.load(std::memory_order_acquire)) return;
        const fakefocus::SoftKeyEvent e =
            st->keyEvents[g_keyEventRead % fakefocus::kSoftKeyEventCap];
        ++g_keyEventRead;
        if (e.vk == 0) continue;

        if (IsMouseVkLocal(e.vk)) {
            UINT msg = WM_LBUTTONDOWN;
            WPARAM wp = MouseMkFlags(st);
            if (e.vk == VK_LBUTTON) {
                msg = e.down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
            } else if (e.vk == VK_RBUTTON) {
                msg = e.down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
            } else if (e.vk == VK_MBUTTON) {
                msg = e.down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
            } else if (e.vk == VK_XBUTTON1) {
                msg = e.down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                wp |= MAKEWPARAM(0, XBUTTON1);
            } else if (e.vk == VK_XBUTTON2) {
                msg = e.down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                wp |= MAKEWPARAM(0, XBUTTON2);
            }
            PostMessageW(hwnd, msg, wp, SoftClientLParam(hwnd, st));
            continue;
        }

        // 滚轮：vk=0xFE 竖向 / 0xFD 横向；down=正向；pad=步进。
        if (e.vk == 0xFE || e.vk == 0xFD) {
            int steps = e.pad ? static_cast<int>(e.pad) : 1;
            short delta = static_cast<short>((e.down ? WHEEL_DELTA : -WHEEL_DELTA) * steps);
            WPARAM wp = MAKEWPARAM(MouseMkFlags(st), delta);
            UINT msg = (e.vk == 0xFE) ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL;
            // 滚轮 lParam 为屏幕坐标。
            PostMessageW(hwnd, msg, wp,
                MAKELPARAM(static_cast<WORD>(st->cursorScreenX),
                    static_cast<WORD>(st->cursorScreenY)));
            continue;
        }

        const UINT scan = MapVirtualKeyW(e.vk, MAPVK_VK_TO_VSC);
        LPARAM lp = 1;
        if (scan) lp |= static_cast<LPARAM>(scan) << 16;
        if (!e.down) lp |= (1 << 30) | (static_cast<LPARAM>(1) << 31);
        PostMessageW(hwnd, e.down ? WM_KEYDOWN : WM_KEYUP, e.vk, lp);
    }
}

void StopSoftKeyDrainThread() {
    g_drainStop.store(true, std::memory_order_release);
    if (g_drainThread) {
        // DrainSoftKeyEventsPost 每轮都查停止位，正常 1~2ms 退出；1s 已足够宽容。
        WaitForSingleObject(g_drainThread, 1000);
        CloseHandle(g_drainThread);
        g_drainThread = nullptr;
    }
}

DWORD WINAPI SoftKeyDrainThreadProc(LPVOID) {
    while (!g_drainStop.load(std::memory_order_acquire)) {
        DrainSoftKeyEventsPost();
        Sleep(1);
    }
    return 0;
}

void StartSoftKeyDrainThread() {
    StopSoftKeyDrainThread();
    g_drainStop.store(false, std::memory_order_release);
    g_drainThread = CreateThread(nullptr, 0, SoftKeyDrainThreadProc, nullptr, 0, nullptr);
    if (g_drainThread) SetThreadPriority(g_drainThread, THREAD_PRIORITY_BELOW_NORMAL);
}

BOOL WINAPI Hook_GetCursorPos(LPPOINT pt) {
    if (g_mapleSafe) MapleMarkCalled(kMapleCalledCursor);
    // 冒险岛走 IAT，禁止在游戏线程灌 WM_INPUT / WM_ACTIVATE（会卡死）。
    // DeSmuME：同样禁止假 WM_INPUT（InputTimer 高频 GetAsyncKeyState 会洪泛崩进程）。
    // 微信/AIR/Electron：禁止假 WM_INPUT 与 SoftRefreshFocusMessages（会 Post WM_ACTIVATE 抢前台）。
    if (!g_mapleSafe && !g_desktopEmuSafe && !g_weixinSafe && !g_tianlongSafe && !g_airSafe) {
        // Electron（Chromium 壳）：静默钩——只回报软光标。
        // 灌 WM_INPUT 会被 Chromium 当伪造输入丢弃甚至洪泛消息队列；
        // 焦点消息与灌键已由 SoftKeyDrainThread 按 200ms/1ms 节拍负责，此处勿重复。
        if (!g_electronSafe) {
            MaybePostFakeWmInput();
            DrainSoftKeyEventsPost();
            const DWORD now = GetTickCount();
            if (now - g_lastFocusRefreshMs >= 200) {
                g_lastFocusRefreshMs = now;
                SoftRefreshFocusMessages();
            }
        }
    }
    if (!pt) return FALSE;
    const fakefocus::SoftInputState* st = SoftState();
    if (st && (st->flags & fakefocus::kSoftFlagCursorValid)) {
        pt->x = st->cursorScreenX;
        pt->y = st->cursorScreenY;
        return TRUE;
    }
    if (g_mapleSafe) {
        pt->x = 0;
        pt->y = 0;
        return TRUE;
    }
    if (g_mapleRealGetCursorPos) return g_mapleRealGetCursorPos(pt);
    return CallOriginalCursorPos(pt);
}

BOOL WINAPI Hook_SetCursorPos(int x, int y) {
    if (SoftCursorSwallowsWarp()) return TRUE;
    if (g_mapleRealSetCursorPos) return g_mapleRealSetCursorPos(x, y);
    return CallOriginalSetCursorPos(x, y);
}

BOOL WINAPI Hook_ClipCursor(const RECT* rc) {
    if (SoftCursorSwallowsWarp()) return TRUE;
    if (g_mapleRealClipCursor) return g_mapleRealClipCursor(rc);
    return CallOriginalClipCursor(rc);
}

HWND WINAPI Hook_SetCapture(HWND hwnd) {
    if (SoftCursorSwallowsWarp() && IsOurHwnd(hwnd)) return hwnd;
    return CallOriginalSetCapture(hwnd);
}

int WINAPI Hook_ShowCursor(BOOL show) {
    if (SoftCursorSwallowsWarp()) {
        static int fake = 0;
        fake += show ? 1 : -1;
        return fake;
    }
    if (g_mapleRealShowCursor) return g_mapleRealShowCursor(show);
    return CallOriginalShowCursor(show);
}

SHORT SoftKeyDownShort(int vKey, bool asyncStyle) {
    const fakefocus::SoftInputState* st = SoftState();
    if (!st || !(st->flags & fakefocus::kSoftFlagKeysValid)) return 0;
    const int vk = vKey & 0xFF;
    if (vk < 0 || vk >= 256 || !st->down[static_cast<size_t>(vk)]) {
        if (IsMouseVk(vk) && (st->flags & fakefocus::kSoftFlagCursorValid)) return 0;
        return 0;
    }
    return asyncStyle ? static_cast<SHORT>(0x8001) : static_cast<SHORT>(0xFF80);
}

SHORT WINAPI Hook_GetAsyncKeyState(int vKey) {
    if (g_mapleSafe) MapleBumpHit(&g_mapleHitGaks);
    if (!g_mapleSafe && !g_desktopEmuSafe && !g_weixinSafe && !g_tianlongSafe && !g_airSafe) {
        if (!g_electronSafe) MaybePostFakeWmInput();
        DrainSoftKeyEventsPost();
    }
    const fakefocus::SoftInputState* st = SoftState();
    if (st && (st->flags & fakefocus::kSoftFlagKeysValid)) {
        if (SHORT soft = SoftKeyDownShort(vKey, true)) return soft;
        return 0;
    }
    if (g_mapleSafe) return 0;
    if (g_mapleRealGetAsyncKeyState) return g_mapleRealGetAsyncKeyState(vKey);
    return CallOriginalAsyncKeyState(vKey);
}

SHORT WINAPI Hook_GetKeyState(int nVirtKey) {
    if (g_mapleSafe) MapleMarkCalled(kMapleCalledKeyState);
    const fakefocus::SoftInputState* st = SoftState();
    if (st && (st->flags & fakefocus::kSoftFlagKeysValid)) {
        if (SHORT soft = SoftKeyDownShort(nVirtKey, false)) return soft;
        return 0;
    }
    if (g_mapleSafe) return 0;
    if (g_mapleRealGetKeyState) return g_mapleRealGetKeyState(nVirtKey);
    return CallOriginalKeyState(nVirtKey);
}

BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState) {
    if (g_mapleSafe) MapleMarkCalled(kMapleCalledKbState);
    if (!lpKeyState) return FALSE;
    if (g_mapleSafe) {
        std::memset(lpKeyState, 0, 256);
        const fakefocus::SoftInputState* st = SoftState();
        if (st && (st->flags & fakefocus::kSoftFlagKeysValid)) {
            for (int i = 0; i < 256; ++i) {
                lpKeyState[i] = st->down[static_cast<size_t>(i)] ? 0x80 : 0;
            }
        }
        return TRUE;
    }
    if (g_mapleRealGetKeyboardState) {
        if (!g_mapleRealGetKeyboardState(lpKeyState)) return FALSE;
    } else if (!CallOriginalKeyboardState(lpKeyState)) {
        return FALSE;
    }

    const fakefocus::SoftInputState* st = SoftState();
    if (!st || !(st->flags & fakefocus::kSoftFlagKeysValid)) return TRUE;

    for (int i = 0; i < 256; ++i) {
        if (st->down[static_cast<size_t>(i)]) {
            lpKeyState[i] = static_cast<BYTE>(lpKeyState[i] | 0x80);
        } else {
            lpKeyState[i] = static_cast<BYTE>(lpKeyState[i] & ~0x80);
        }
    }
    return TRUE;
}

BOOL WINAPI Hook_IsWindowVisible(HWND hwnd) {
    // 宏桌面 / Pin+Cloak 时系统常报不可见；Chromium 据此丢弃输入。
    // 冒险岛 visible=0 时客户端会停轮询，IAT 必须回报可见。
    if (g_mapleSafe && IsOurHwnd(hwnd)) {
        MapleBumpHit(&g_mapleHitGfw);
        return TRUE;
    }
    if (IsOurHwnd(hwnd)) return TRUE;
    if (g_mapleSafe && g_mapleRealIsWindowVisible) return g_mapleRealIsWindowVisible(hwnd);
    if (g_mapleSafe) return TRUE;
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

// 星辰目录里是 2009 官方 dinput8：失焦后靠 WM_ACTIVATE 停轮询，不是每帧 GetForegroundWindow。
// 164352 / FakeFocus32.raw.dll：Peek 灌假 WM_INPUT、钩 GetRawInput*、改 dinput8 可写节、
// 注入线程 RegisterRawInputDevices(INPUTSINK) / SetCooperativeLevel = 立刻闪退。
// 只改 IAT 吞失活 + 27–32 方法虚表；禁止假 WM_INPUT、禁止代理 DLL、禁止 user32 方法体 JMP。
BOOL (WINAPI* g_mapleRealPeekMessageW)(LPMSG, HWND, UINT, UINT, UINT) = nullptr;
BOOL (WINAPI* g_mapleRealPeekMessageA)(LPMSG, HWND, UINT, UINT, UINT) = nullptr;
BOOL (WINAPI* g_mapleRealGetMessageW)(LPMSG, HWND, UINT, UINT) = nullptr;
BOOL (WINAPI* g_mapleRealGetMessageA)(LPMSG, HWND, UINT, UINT) = nullptr;
LRESULT (WINAPI* g_mapleRealDispatchMessageW)(const MSG*) = nullptr;
LRESULT (WINAPI* g_mapleRealDispatchMessageA)(const MSG*) = nullptr;
BOOL (WINAPI* g_mapleRealTranslateMessage)(const MSG*) = nullptr;
LRESULT (WINAPI* g_mapleRealCallWindowProcW)(WNDPROC, HWND, UINT, WPARAM, LPARAM) = nullptr;
LRESULT (WINAPI* g_mapleRealCallWindowProcA)(WNDPROC, HWND, UINT, WPARAM, LPARAM) = nullptr;

void MapleNeutralizeDeactivateMsg(MSG* msg) {
    if (!msg) return;
    HWND top = g_targetTop.load(std::memory_order_relaxed);
    const bool ours = IsOurHwnd(msg->hwnd)
        || (msg->hwnd && top && IsWindow(top) && IsChild(top, msg->hwnd));
    if (msg->message == WM_ACTIVATEAPP) {
        if (msg->wParam == FALSE) {
            msg->wParam = TRUE;
            MapleBumpHit(&g_mapleHitFocus);
        }
        return;
    }
    if (!ours) return;
    if (!IsDeactivate(msg->wParam, msg->message)) return;
    if (msg->message == WM_KILLFOCUS) {
        msg->message = WM_NULL;
        MapleBumpHit(&g_mapleHitFocus);
        return;
    }
    if (msg->message == WM_NCACTIVATE) {
        msg->wParam = TRUE;
        MapleBumpHit(&g_mapleHitFocus);
        return;
    }
    if (msg->message == WM_ACTIVATE) {
        msg->wParam = MAKEWPARAM(WA_ACTIVE, HIWORD(msg->wParam));
        MapleBumpHit(&g_mapleHitFocus);
    }
}

WPARAM MapleNeutralizeDeactivateParams(HWND hwnd, UINT msg, WPARAM wp) {
    HWND top = g_targetTop.load(std::memory_order_relaxed);
    const bool ours = IsOurHwnd(hwnd)
        || (hwnd && top && IsWindow(top) && IsChild(top, hwnd));
    if (msg == WM_ACTIVATEAPP && wp == FALSE) return TRUE;
    if (!ours || !IsDeactivate(wp, msg)) return wp;
    if (msg == WM_NCACTIVATE) return TRUE;
    if (msg == WM_ACTIVATE) return MAKEWPARAM(WA_ACTIVE, HIWORD(wp));
    if (msg == WM_ACTIVATEAPP) return TRUE;
    return wp;
}

BOOL WINAPI Hook_MaplePeekMessageW(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove) {
    auto orig = g_mapleRealPeekMessageW ? g_mapleRealPeekMessageW : PeekMessageW;
    const BOOL got = orig(lpMsg, hWnd, min, max, remove);
    if (got) MapleNeutralizeDeactivateMsg(lpMsg);
    return got;
}
BOOL WINAPI Hook_MaplePeekMessageA(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove) {
    auto orig = g_mapleRealPeekMessageA ? g_mapleRealPeekMessageA : PeekMessageA;
    const BOOL got = orig(lpMsg, hWnd, min, max, remove);
    if (got) MapleNeutralizeDeactivateMsg(lpMsg);
    return got;
}
BOOL WINAPI Hook_MapleGetMessageW(LPMSG lpMsg, HWND hWnd, UINT min, UINT max) {
    auto orig = g_mapleRealGetMessageW ? g_mapleRealGetMessageW : GetMessageW;
    const BOOL got = orig(lpMsg, hWnd, min, max);
    if (got) MapleNeutralizeDeactivateMsg(lpMsg);
    return got;
}
BOOL WINAPI Hook_MapleGetMessageA(LPMSG lpMsg, HWND hWnd, UINT min, UINT max) {
    auto orig = g_mapleRealGetMessageA ? g_mapleRealGetMessageA : GetMessageA;
    const BOOL got = orig(lpMsg, hWnd, min, max);
    if (got) MapleNeutralizeDeactivateMsg(lpMsg);
    return got;
}
LRESULT WINAPI Hook_MapleDispatchMessageW(const MSG* lpMsg) {
    auto orig = g_mapleRealDispatchMessageW ? g_mapleRealDispatchMessageW : DispatchMessageW;
    MSG copy{};
    if (lpMsg) {
        copy = *lpMsg;
        MapleNeutralizeDeactivateMsg(&copy);
        return orig(&copy);
    }
    return orig(lpMsg);
}
LRESULT WINAPI Hook_MapleDispatchMessageA(const MSG* lpMsg) {
    auto orig = g_mapleRealDispatchMessageA ? g_mapleRealDispatchMessageA : DispatchMessageA;
    MSG copy{};
    if (lpMsg) {
        copy = *lpMsg;
        MapleNeutralizeDeactivateMsg(&copy);
        return orig(&copy);
    }
    return orig(lpMsg);
}
BOOL WINAPI Hook_MapleTranslateMessage(const MSG* lpMsg) {
    auto orig = g_mapleRealTranslateMessage ? g_mapleRealTranslateMessage : TranslateMessage;
    return orig(lpMsg);
}
LRESULT WINAPI Hook_MapleCallWindowProcW(WNDPROC prev, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto orig = g_mapleRealCallWindowProcW ? g_mapleRealCallWindowProcW : CallWindowProcW;
    if (msg == WM_KILLFOCUS && (IsOurHwnd(hwnd)
        || (hwnd && IsOurHwnd(GetAncestor(hwnd, GA_ROOT))))) {
        return 0;
    }
    wp = MapleNeutralizeDeactivateParams(hwnd, msg, wp);
    return orig(prev, hwnd, msg, wp, lp);
}
LRESULT WINAPI Hook_MapleCallWindowProcA(WNDPROC prev, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto orig = g_mapleRealCallWindowProcA ? g_mapleRealCallWindowProcA : CallWindowProcA;
    if (msg == WM_KILLFOCUS && (IsOurHwnd(hwnd)
        || (hwnd && IsOurHwnd(GetAncestor(hwnd, GA_ROOT))))) {
        return 0;
    }
    wp = MapleNeutralizeDeactivateParams(hwnd, msg, wp);
    return orig(prev, hwnd, msg, wp, lp);
}

#include "fake_focus_raw_input.inl"

LRESULT CALLBACK FakeFocusWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (IsDeactivate(wp, msg)) {
        return 0;
    }
    if (g_oldWndProc) {
        return CallWindowProcW(g_oldWndProc, hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK FocusGuardGetMsgProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && lParam) {
        MSG* msg = reinterpret_cast<MSG*>(lParam);
        if (msg && IsOurHwnd(msg->hwnd) && IsDeactivate(msg->wParam, msg->message)) {
            msg->message = WM_NULL;
        }
    }
    return CallNextHookEx(g_focusGuardHook, nCode, wParam, lParam);
}

bool LooksLikeAdobeAirClassName(const wchar_t* cls) {
    if (!cls || !*cls) return false;
    wchar_t lower[256]{};
    wcsncpy_s(lower, cls, _TRUNCATE);
    CharLowerW(lower);
    return wcsstr(lower, L"apolloruntime") != nullptr
        || wcsstr(lower, L"adobeair") != nullptr;
}

bool LooksLikeMapleStoryClassName(const wchar_t* cls) {
    if (!cls || !*cls) return false;
    wchar_t lower[256]{};
    wcsncpy_s(lower, cls, _TRUNCATE);
    CharLowerW(lower);
    if (wcscmp(lower, L"maplestoryclass") == 0) return true;
    if (wcscmp(lower, L"maplestory") == 0) return true;
    return wcsstr(lower, L"maplestoryclass") != nullptr;
}

bool LooksLikeMapleStoryWindowTitle(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    if (!title[0]) return false;
    wchar_t lower[512]{};
    wcsncpy_s(lower, title, _TRUNCATE);
    CharLowerW(lower);
    if (wcsstr(lower, L"maplestory") != nullptr) return true;
    return wcsstr(title, L"冒险岛") != nullptr;
}

bool LooksLikeMapleStoryProcessImage() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH) || !path[0]) return false;
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    wchar_t lower[MAX_PATH]{};
    wcsncpy_s(lower, base, _TRUNCATE);
    CharLowerW(lower);
    if (wcscmp(lower, L"maplestory.exe") == 0) return true;
    if (wcscmp(lower, L"maplestoryt.exe") == 0) return true;
    const size_t n = wcslen(lower);
    // 与 window_mode_types.cpp LooksLikeMapleStoryExecutable 对齐：maplestory*.exe
    return n > 14 && wcsncmp(lower, L"maplestory", 10) == 0
        && n >= 4 && wcscmp(lower + (n - 4), L".exe") == 0;
}

bool LooksLikeMapleStoryTarget(HWND top) {
    if (LooksLikeMapleStoryProcessImage()) return true;
    if (!top || !IsWindow(top)) return false;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (LooksLikeMapleStoryClassName(cls)) return true;
    return LooksLikeMapleStoryWindowTitle(top);
}

void ResetFocusModeFlags() {
    g_liteMode = false;
    g_electronSafe = false;
    g_airSafe = false;
    g_weixinSafe = false;
    g_tianlongSafe = false;
    g_mapleSafe = false;
    g_desktopEmuSafe = false;
}

bool ShouldAttachFakeFocusSubclass(HWND top) {
    if (!top || !IsWindow(top)) return false;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (_wcsnicmp(cls, L"Chrome_", 7) == 0) return false;
    if (_wcsicmp(cls, L"MozillaWindowClass") == 0) return false;
    if (_wcsicmp(cls, L"ApplicationFrameWindow") == 0) return false;
    if (_wcsicmp(cls, L"TscShellContainerClass") == 0) return false;
    if (_wcsicmp(cls, L"UIMainClass") == 0) return false;
    if (_wcsicmp(cls, L"IHWindowClass") == 0) return false;
    // DeSmuME/wx 等：子类化吞 WM_KILLFOCUS/WM_ACTIVATE 会弄乱内部状态并崩。
    if (_wcsicmp(cls, L"DeSmuME") == 0) return false;
    if (wcsstr(cls, L"DeSmuME") != nullptr) return false;
    if (_wcsicmp(cls, L"wxWindowNR") == 0) return false;
    if (_wcsnicmp(cls, L"wxWindow", 8) == 0) return false;
    if (wcsstr(cls, L"Dolphin") != nullptr) return false;
    if (wcsstr(cls, L"melonDS") != nullptr) return false;
    if (ProcessImageLooksLikeDesktopEmu(top)) return false;
    // GLFW/SDL：远程线程改 WndProc 不安全；焦点守卫改用同进程 WH_GETMESSAGE。
    if (_wcsicmp(cls, L"GLFW30") == 0) return false;
    if (_wcsicmp(cls, L"SDL_APP") == 0) return false;
    // Adobe AIR（造梦 ApolloRuntime）：子类化会卡死播放器随后退出。
    if (LooksLikeAdobeAirClassName(cls)) return false;
    if (HwndLooksLikeWeixinClient(top)) return false;
    if (ClassLooksLikeTianLongBaBu(cls)) return false;
    // 冒险岛 DirectX：子类化 / 光标钩会让客户端无响应，只能任务管理器杀。
    if (LooksLikeMapleStoryTarget(top)) return false;
    return true;
}

/// 进程路径是否桌面模拟器（melonDS 顶层常为 Qt*QWindowIcon，不能只靠类名）。
bool ProcessImageLooksLikeDesktopEmu(HWND top) {
    if (!top || !IsWindow(top)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(top, &pid);
    if (pid == 0) return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &n);
    CloseHandle(proc);
    if (!ok || n == 0) return false;
    for (DWORD i = 0; i < n; ++i) {
        path[i] = static_cast<wchar_t>(towlower(path[i]));
    }
    const wchar_t* leaf = path;
    for (DWORD i = 0; i < n; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') leaf = path + i + 1;
    }
    static const wchar_t* kParts[] = {
        L"desmume", L"melonds", L"dolphin", L"pcsx2", L"citra", L"yuzu",
        L"ryujinx", L"duckstation", L"ppsspp", L"retroarch", L"bizhawk",
        L"mgba", L"snes9x", L"fceux", L"cemu", L"rpcs3", L"xenia", L"xemu",
    };
    for (const wchar_t* part : kParts) {
        if (wcsstr(leaf, part) != nullptr) return true;
    }
    return false;
}

bool ClassLooksLikeQtQWindowIcon(const wchar_t* cls) {
    if (!cls || !cls[0]) return false;
    wchar_t lower[256]{};
    size_t n = 0;
    for (; cls[n] && n < 255; ++n) {
        lower[n] = static_cast<wchar_t>(towlower(cls[n]));
    }
    return wcsstr(lower, L"qt") != nullptr && wcsstr(lower, L"qwindowicon") != nullptr;
}

bool ClassLooksLikeTianLongBaBu(const wchar_t* cls) {
    if (!cls || !cls[0]) return false;
    wchar_t lower[256]{};
    size_t n = 0;
    for (; cls[n] && n < 255; ++n) {
        lower[n] = static_cast<wchar_t>(towlower(cls[n]));
    }
    return wcsstr(lower, L"tianlong") != nullptr || wcsstr(lower, L"babuhj") != nullptr;
}

bool TitleLooksLikeWeixinClient(const wchar_t* title) {
    if (!title || !title[0]) return false;
    if (wcsstr(title, L"开发者工具") != nullptr) return false;
    wchar_t lower[512]{};
    size_t n = 0;
    for (; title[n] && n < 511; ++n) {
        lower[n] = static_cast<wchar_t>(towlower(title[n]));
    }
    if (wcsstr(lower, L"devtools") != nullptr) return false;
    if (wcscmp(title, L"微信") == 0) return true;
    if (title[0] == L'微' && title[1] == L'信') return true;
    return wcscmp(lower, L"wechat") == 0 || wcscmp(lower, L"weixin") == 0;
}

bool ProcessImageLooksLikeWeixin(HWND top) {
    if (!top || !IsWindow(top)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(top, &pid);
    if (pid == 0) return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &n);
    CloseHandle(proc);
    if (!ok || n == 0) return false;
    for (DWORD i = 0; i < n; ++i) {
        path[i] = static_cast<wchar_t>(towlower(path[i]));
    }
    const wchar_t* leaf = path;
    for (DWORD i = 0; i < n; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') leaf = path + i + 1;
    }
    return wcscmp(leaf, L"weixin.exe") == 0 || wcscmp(leaf, L"wechat.exe") == 0;
}

bool HwndLooksLikeWeixinClient(HWND top) {
    if (ProcessImageLooksLikeWeixin(top)) return true;
    if (!top || !IsWindow(top)) return false;
    wchar_t cls[256]{};
    wchar_t title[512]{};
    GetClassNameW(top, cls, 256);
    GetWindowTextW(top, title, 512);
    return ClassLooksLikeQtQWindowIcon(cls) && TitleLooksLikeWeixinClient(title);
}

bool LooksLikeDesktopEmuClassName(const wchar_t* cls) {
    if (!cls || !cls[0]) return false;
    if (_wcsicmp(cls, L"DeSmuME") == 0) return true;
    if (wcsstr(cls, L"DeSmuME") != nullptr) return true;
    if (_wcsicmp(cls, L"wxWindowNR") == 0) return true;
    if (_wcsnicmp(cls, L"wxWindow", 8) == 0) return true;
    if (wcsstr(cls, L"Dolphin") != nullptr) return true;
    if (wcsstr(cls, L"melonDS") != nullptr) return true;
    return false;
}

/// lite 下跳过 GetCursorPos：桌面模拟器/AIR/冒险岛。GLFW 必须钩，否则点击读真光标。
bool ShouldSkipGetCursorPosHook(HWND top) {
    if (!top || !IsWindow(top)) return false;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (LooksLikeDesktopEmuClassName(cls)) return true;
    if (ProcessImageLooksLikeDesktopEmu(top)) return true;
    // AIR 2D 吃 WM_MOUSE*；钩 GetCursorPos/SetCursorPos 会让真光标原地抽，并狂投 WM_INPUT。
    if (LooksLikeAdobeAirClassName(cls)) return true;
    // Phase2 全套会带 RawInput；微信光标钩走 InstallWeixinMouseStateHooks。
    if (HwndLooksLikeWeixinClient(top)) return true;
    if (LooksLikeMapleStoryTarget(top)) return true;
    return false;
}

bool LooksLikeGlfwOrSdlClassName(const wchar_t* cls) {
    return cls && (_wcsicmp(cls, L"GLFW30") == 0 || _wcsicmp(cls, L"SDL_APP") == 0);
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

constexpr int kMapleIatCap = 768;
struct MapleIatPatch {
    void** slot = nullptr;
    void* original = nullptr;
};
MapleIatPatch g_mapleIat[kMapleIatCap]{};
int g_mapleIatCount = 0;
HMODULE g_mapleExtraMods[48]{};
int g_mapleExtraModN = 0;
FARPROC (WINAPI* g_mapleRealGetProcAddress)(HMODULE, LPCSTR) = nullptr;
void MapleMarkIatDetour(void* detour);

void** g_diKbStateSlot = nullptr;
void* g_diKbStateOrig = nullptr;
void** g_diMouseStateSlot = nullptr;
void* g_diMouseStateOrig = nullptr;
void** g_diCreateDeviceSlot = nullptr;
void* g_diCreateDeviceOrig = nullptr;
using DiCreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, const GUID*, void**, void*);
DiCreateFn g_realDiCreate = nullptr;
using DiCreateAFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, void**, void*);
DiCreateAFn g_realDiCreateA = nullptr;
DiCreateAFn g_realDiCreateW = nullptr;
DiCreateFn g_realDiCreateEx = nullptr;
int g_diLastMouseX = 0;
int g_diLastMouseY = 0;
bool g_diHaveMouse = false;
BYTE g_maplePrevDik[256]{};
DWORD g_mapleDiSeq = 0;
struct MapleDiKindEnt {
    void* self = nullptr;
    int kind = 0;
};
MapleDiKindEnt g_mapleDiKind[8]{};
constexpr int kMapleDiVtCap = 40;
struct MapleDiVtPatch {
    void** slot = nullptr;
    void* orig = nullptr;
    int index = 0;
};
MapleDiVtPatch g_diVtPatch[kMapleDiVtCap]{};

bool MapleIatPatchSlot(void** slot, void* detour, bool countPoll) {
    if (!slot || !detour || g_mapleIatCount >= kMapleIatCap) return false;
    if (*slot == detour) return true;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    g_mapleIat[g_mapleIatCount].slot = slot;
    g_mapleIat[g_mapleIatCount].original = *slot;
    ++g_mapleIatCount;
    if (countPoll) ++g_mapleInputHookCount;
    MapleMarkIatDetour(detour);
    *slot = detour;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

enum class MapleIatWalkKind {
    All,
    // 游戏目录 dinput8/dinput：只补 user32 前景/轮询 IAT。
    // 禁止 GetProcAddress / DirectInputCreate* / 可写节（155648 立刻闪退）。
    DinputUser32Only,
};

bool MapleImportDllWanted(const char* dll, MapleIatWalkKind kind) {
    if (!dll || !*dll) return false;
    char lower[64]{};
    lstrcpynA(lower, dll, 64);
    CharLowerA(lower);
    const bool user32 = lstrcmpA(lower, "user32.dll") == 0 || lstrcmpA(lower, "user32") == 0
        || lstrcmpA(lower, "win32u.dll") == 0 || lstrcmpA(lower, "win32u") == 0
        || strncmp(lower, "api-ms-win-ntuser-", 18) == 0
        || strncmp(lower, "ext-ms-win-ntuser-", 18) == 0;
    if (kind == MapleIatWalkKind::DinputUser32Only) return user32;
    if (user32) return true;
    if (lstrcmpA(lower, "dinput8.dll") == 0 || lstrcmpA(lower, "dinput8") == 0) return true;
    if (lstrcmpA(lower, "dinput.dll") == 0 || lstrcmpA(lower, "dinput") == 0) return true;
    if (lstrcmpA(lower, "kernel32.dll") == 0 || lstrcmpA(lower, "kernel32") == 0) return true;
    if (lstrcmpA(lower, "kernelbase.dll") == 0 || lstrcmpA(lower, "kernelbase") == 0) return true;
    return strncmp(lower, "api-ms-win-core-libraryloader-", 30) == 0;
}

void MapleCopyImportBaseName(const char* name, char* out, int outLen) {
    if (!out || outLen < 2) return;
    out[0] = 0;
    if (!name || !*name) return;
    if (*name == '_') ++name;
    int i = 0;
    for (; name[i] && name[i] != '@' && i < outLen - 1; ++i) {
        out[i] = name[i];
    }
    out[i] = 0;
}

bool MapleMemCommittedReadable(const void* p, size_t bytes) {
    if (!p || bytes == 0) return false;
    const BYTE* cur = static_cast<const BYTE*>(p);
    const BYTE* end = cur + bytes;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & PAGE_GUARD) return false;
        switch (mbi.Protect & 0xFF) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            break;
        default:
            return false;
        }
        const BYTE* regionEnd = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cur) return false;
        cur = regionEnd;
    }
    return true;
}

BYTE* MapleImagePtr(BYTE* base, size_t imageSize, DWORD value, bool rva) {
    if (!base || !value) return nullptr;
    if (rva) {
        if (value >= imageSize) return nullptr;
        return base + value;
    }
    BYTE* p = reinterpret_cast<BYTE*>(static_cast<ULONG_PTR>(value));
    if (p < base || p >= base + imageSize) return nullptr;
    return p;
}

void* MapleDetourForImportName(const char* name, MapleIatWalkKind kind);
HRESULT WINAPI Hook_DirectInput8Create(HINSTANCE hinst, DWORD version,
    const GUID* iid, void** out, void* unk);
HRESULT WINAPI Hook_DirectInputCreateA(HINSTANCE hinst, DWORD version, void** out, void* unk);
HRESULT WINAPI Hook_DirectInputCreateW(HINSTANCE hinst, DWORD version, void** out, void* unk);
HRESULT WINAPI Hook_DirectInputCreateEx(HINSTANCE hinst, DWORD version,
    const GUID* iid, void** out, void* unk);
FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR name);

struct MapleIatPair {
    void* original = nullptr;
    void* detour = nullptr;
    bool poll = false;
};

int MapleIatFillPairs(MapleIatPair* out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    auto add = [&](void* original, void* detour, bool poll) {
        if (!original || !detour || n >= cap) return;
        out[n].original = original;
        out[n].detour = detour;
        out[n].poll = poll;
        ++n;
    };
    add(reinterpret_cast<void*>(g_mapleRealGetCursorPos),
        reinterpret_cast<void*>(&Hook_GetCursorPos), true);
    add(reinterpret_cast<void*>(g_mapleRealSetCursorPos),
        reinterpret_cast<void*>(&Hook_SetCursorPos), false);
    add(reinterpret_cast<void*>(g_mapleRealClipCursor),
        reinterpret_cast<void*>(&Hook_ClipCursor), false);
    add(reinterpret_cast<void*>(g_mapleRealShowCursor),
        reinterpret_cast<void*>(&Hook_ShowCursor), false);
    add(reinterpret_cast<void*>(g_mapleRealGetAsyncKeyState),
        reinterpret_cast<void*>(&Hook_GetAsyncKeyState), true);
    add(reinterpret_cast<void*>(g_mapleRealGetKeyState),
        reinterpret_cast<void*>(&Hook_GetKeyState), true);
    add(reinterpret_cast<void*>(g_mapleRealGetKeyboardState),
        reinterpret_cast<void*>(&Hook_GetKeyboardState), true);
    add(reinterpret_cast<void*>(g_mapleRealGetForegroundWindow),
        reinterpret_cast<void*>(&Hook_GetForegroundWindow), false);
    add(reinterpret_cast<void*>(g_mapleRealSetForegroundWindow),
        reinterpret_cast<void*>(&Hook_SetForegroundWindow), false);
    add(reinterpret_cast<void*>(g_mapleRealFlashWindow),
        reinterpret_cast<void*>(&Hook_FlashWindow), false);
    add(reinterpret_cast<void*>(g_mapleRealFlashWindowEx),
        reinterpret_cast<void*>(&Hook_FlashWindowEx), false);
    add(reinterpret_cast<void*>(g_mapleRealSwitchToThisWindow),
        reinterpret_cast<void*>(&Hook_SwitchToThisWindow), false);
    add(reinterpret_cast<void*>(g_mapleRealGetActiveWindow),
        reinterpret_cast<void*>(&Hook_GetActiveWindow), false);
    add(reinterpret_cast<void*>(g_mapleRealGetFocus),
        reinterpret_cast<void*>(&Hook_GetFocus), false);
    add(reinterpret_cast<void*>(g_mapleRealIsWindowVisible),
        reinterpret_cast<void*>(&Hook_IsWindowVisible), false);
    add(reinterpret_cast<void*>(g_mapleRealIsIconic),
        reinterpret_cast<void*>(&Hook_IsIconic), false);
    add(g_mapleNtUserCursor, reinterpret_cast<void*>(&Hook_GetCursorPos), true);
    add(g_mapleNtUserSetCursor, reinterpret_cast<void*>(&Hook_SetCursorPos), false);
    add(g_mapleNtUserGaks, reinterpret_cast<void*>(&Hook_GetAsyncKeyState), true);
    add(g_mapleNtUserKeyState, reinterpret_cast<void*>(&Hook_GetKeyState), true);
    add(g_mapleNtUserKbState, reinterpret_cast<void*>(&Hook_GetKeyboardState), true);
    add(g_mapleNtUserGfw, reinterpret_cast<void*>(&Hook_GetForegroundWindow), false);
    add(reinterpret_cast<void*>(g_mapleRealPeekMessageW),
        reinterpret_cast<void*>(&Hook_MaplePeekMessageW), false);
    add(reinterpret_cast<void*>(g_mapleRealPeekMessageA),
        reinterpret_cast<void*>(&Hook_MaplePeekMessageA), false);
    add(reinterpret_cast<void*>(g_mapleRealGetMessageW),
        reinterpret_cast<void*>(&Hook_MapleGetMessageW), false);
    add(reinterpret_cast<void*>(g_mapleRealGetMessageA),
        reinterpret_cast<void*>(&Hook_MapleGetMessageA), false);
    add(reinterpret_cast<void*>(g_mapleRealDispatchMessageW),
        reinterpret_cast<void*>(&Hook_MapleDispatchMessageW), false);
    add(reinterpret_cast<void*>(g_mapleRealDispatchMessageA),
        reinterpret_cast<void*>(&Hook_MapleDispatchMessageA), false);
    add(reinterpret_cast<void*>(g_mapleRealCallWindowProcW),
        reinterpret_cast<void*>(&Hook_MapleCallWindowProcW), false);
    add(reinterpret_cast<void*>(g_mapleRealCallWindowProcA),
        reinterpret_cast<void*>(&Hook_MapleCallWindowProcA), false);
    add(reinterpret_cast<void*>(g_realDiCreate),
        reinterpret_cast<void*>(&Hook_DirectInput8Create), true);
    add(reinterpret_cast<void*>(g_realDiCreateA),
        reinterpret_cast<void*>(&Hook_DirectInputCreateA), true);
    add(reinterpret_cast<void*>(g_realDiCreateW),
        reinterpret_cast<void*>(&Hook_DirectInputCreateW), true);
    add(reinterpret_cast<void*>(g_realDiCreateEx),
        reinterpret_cast<void*>(&Hook_DirectInputCreateEx), true);
    add(reinterpret_cast<void*>(g_mapleRealGetProcAddress),
        reinterpret_cast<void*>(&Hook_GetProcAddress), false);
    return n;
}

int MapleIatFillFocusPairs(MapleIatPair* out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    auto add = [&](void* original, void* detour) {
        if (!original || !detour || n >= cap) return;
        out[n].original = original;
        out[n].detour = detour;
        out[n].poll = false;
        ++n;
    };
    add(reinterpret_cast<void*>(g_mapleRealGetForegroundWindow),
        reinterpret_cast<void*>(&Hook_GetForegroundWindow));
    add(reinterpret_cast<void*>(g_mapleRealSetForegroundWindow),
        reinterpret_cast<void*>(&Hook_SetForegroundWindow));
    add(reinterpret_cast<void*>(g_mapleRealFlashWindow),
        reinterpret_cast<void*>(&Hook_FlashWindow));
    add(reinterpret_cast<void*>(g_mapleRealFlashWindowEx),
        reinterpret_cast<void*>(&Hook_FlashWindowEx));
    add(reinterpret_cast<void*>(g_mapleRealSwitchToThisWindow),
        reinterpret_cast<void*>(&Hook_SwitchToThisWindow));
    add(reinterpret_cast<void*>(g_mapleRealGetActiveWindow),
        reinterpret_cast<void*>(&Hook_GetActiveWindow));
    add(reinterpret_cast<void*>(g_mapleRealGetFocus),
        reinterpret_cast<void*>(&Hook_GetFocus));
    add(reinterpret_cast<void*>(g_mapleRealIsWindowVisible),
        reinterpret_cast<void*>(&Hook_IsWindowVisible));
    add(reinterpret_cast<void*>(g_mapleRealIsIconic),
        reinterpret_cast<void*>(&Hook_IsIconic));
    add(g_mapleNtUserGfw, reinterpret_cast<void*>(&Hook_GetForegroundWindow));
    return n;
}

int MapleIatFillDinputUser32Pairs(MapleIatPair* out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    auto add = [&](void* original, void* detour, bool poll) {
        if (!original || !detour || n >= cap) return;
        out[n].original = original;
        out[n].detour = detour;
        out[n].poll = poll;
        ++n;
    };
    add(reinterpret_cast<void*>(g_mapleRealGetCursorPos),
        reinterpret_cast<void*>(&Hook_GetCursorPos), true);
    add(reinterpret_cast<void*>(g_mapleRealSetCursorPos),
        reinterpret_cast<void*>(&Hook_SetCursorPos), false);
    add(reinterpret_cast<void*>(g_mapleRealGetAsyncKeyState),
        reinterpret_cast<void*>(&Hook_GetAsyncKeyState), true);
    add(reinterpret_cast<void*>(g_mapleRealGetKeyState),
        reinterpret_cast<void*>(&Hook_GetKeyState), true);
    add(reinterpret_cast<void*>(g_mapleRealGetKeyboardState),
        reinterpret_cast<void*>(&Hook_GetKeyboardState), true);
    add(reinterpret_cast<void*>(g_mapleRealGetForegroundWindow),
        reinterpret_cast<void*>(&Hook_GetForegroundWindow), false);
    add(reinterpret_cast<void*>(g_mapleRealGetActiveWindow),
        reinterpret_cast<void*>(&Hook_GetActiveWindow), false);
    add(reinterpret_cast<void*>(g_mapleRealGetFocus),
        reinterpret_cast<void*>(&Hook_GetFocus), false);
    add(reinterpret_cast<void*>(g_mapleRealIsIconic),
        reinterpret_cast<void*>(&Hook_IsIconic), false);
    add(reinterpret_cast<void*>(g_mapleRealIsWindowVisible),
        reinterpret_cast<void*>(&Hook_IsWindowVisible), false);
    add(g_mapleNtUserCursor, reinterpret_cast<void*>(&Hook_GetCursorPos), true);
    add(g_mapleNtUserSetCursor, reinterpret_cast<void*>(&Hook_SetCursorPos), false);
    add(g_mapleNtUserGaks, reinterpret_cast<void*>(&Hook_GetAsyncKeyState), true);
    add(g_mapleNtUserKeyState, reinterpret_cast<void*>(&Hook_GetKeyState), true);
    add(g_mapleNtUserKbState, reinterpret_cast<void*>(&Hook_GetKeyboardState), true);
    add(g_mapleNtUserGfw, reinterpret_cast<void*>(&Hook_GetForegroundWindow), false);
    add(reinterpret_cast<void*>(g_mapleRealPeekMessageW),
        reinterpret_cast<void*>(&Hook_MaplePeekMessageW), false);
    add(reinterpret_cast<void*>(g_mapleRealPeekMessageA),
        reinterpret_cast<void*>(&Hook_MaplePeekMessageA), false);
    add(reinterpret_cast<void*>(g_mapleRealGetMessageW),
        reinterpret_cast<void*>(&Hook_MapleGetMessageW), false);
    add(reinterpret_cast<void*>(g_mapleRealGetMessageA),
        reinterpret_cast<void*>(&Hook_MapleGetMessageA), false);
    add(reinterpret_cast<void*>(g_mapleRealDispatchMessageW),
        reinterpret_cast<void*>(&Hook_MapleDispatchMessageW), false);
    add(reinterpret_cast<void*>(g_mapleRealDispatchMessageA),
        reinterpret_cast<void*>(&Hook_MapleDispatchMessageA), false);
    add(reinterpret_cast<void*>(g_mapleRealCallWindowProcW),
        reinterpret_cast<void*>(&Hook_MapleCallWindowProcW), false);
    add(reinterpret_cast<void*>(g_mapleRealCallWindowProcA),
        reinterpret_cast<void*>(&Hook_MapleCallWindowProcA), false);
    return n;
}

void MapleIatWalkBoundThunks(IMAGE_THUNK_DATA* iat, MapleIatWalkKind kind) {
    MapleIatPair pairs[64]{};
    const int nPairs = kind == MapleIatWalkKind::DinputUser32Only
        ? MapleIatFillDinputUser32Pairs(pairs, 64)
        : MapleIatFillPairs(pairs, 64);
    int n = 0;
    for (; iat && MapleMemCommittedReadable(iat, sizeof(*iat)) && iat->u1.Function && n < 512;
        ++iat, ++n) {
        void* cur = reinterpret_cast<void*>(static_cast<ULONG_PTR>(iat->u1.Function));
        if (!cur) continue;
        for (int i = 0; i < nPairs; ++i) {
            if (pairs[i].original && cur == pairs[i].original) {
                MapleIatPatchSlot(reinterpret_cast<void**>(&iat->u1.Function),
                    pairs[i].detour, pairs[i].poll);
                break;
            }
        }
    }
}

bool MapleCStringInImage(const char* s, size_t remain, size_t cap) {
    if (!s || remain == 0) return false;
    const size_t n = remain < cap ? remain : cap;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == 0) return true;
    }
    return false;
}

void MapleIatWalkNamedThunks(BYTE* base, size_t imageSize,
    IMAGE_THUNK_DATA* names, IMAGE_THUNK_DATA* iat, MapleIatWalkKind kind) {
    if (!base || !names || !iat) return;
    int walked = 0;
    for (; MapleMemCommittedReadable(names, sizeof(*names))
            && MapleMemCommittedReadable(iat, sizeof(*iat))
            && names->u1.AddressOfData && iat->u1.Function && walked < 512;
        ++names, ++iat, ++walked) {
        if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
        const ULONG_PTR addrOfData = static_cast<ULONG_PTR>(names->u1.AddressOfData);
        BYTE* nameRec = nullptr;
        const ULONG_PTR baseAddr = reinterpret_cast<ULONG_PTR>(base);
        if (addrOfData >= baseAddr && addrOfData < baseAddr + imageSize) {
            nameRec = reinterpret_cast<BYTE*>(addrOfData);
        } else if (addrOfData < imageSize) {
            nameRec = base + addrOfData;
        }
        if (!nameRec) continue;
        const size_t nameOff = static_cast<size_t>(nameRec - base)
            + offsetof(IMAGE_IMPORT_BY_NAME, Name);
        if (nameOff >= imageSize) continue;
        const char* iname = reinterpret_cast<const char*>(base + nameOff);
        if (!MapleCStringInImage(iname, imageSize - nameOff, 96)) continue;
        void* detour = MapleDetourForImportName(iname, kind);
        if (!detour) continue;
        const bool poll = detour == reinterpret_cast<void*>(&Hook_GetCursorPos)
            || detour == reinterpret_cast<void*>(&Hook_GetAsyncKeyState)
            || detour == reinterpret_cast<void*>(&Hook_GetKeyState)
            || detour == reinterpret_cast<void*>(&Hook_GetKeyboardState)
            || detour == reinterpret_cast<void*>(&Hook_DirectInput8Create)
            || detour == reinterpret_cast<void*>(&Hook_DirectInputCreateA)
            || detour == reinterpret_cast<void*>(&Hook_DirectInputCreateW)
            || detour == reinterpret_cast<void*>(&Hook_DirectInputCreateEx);
        MapleIatPatchSlot(reinterpret_cast<void**>(&iat->u1.Function), detour, poll);
    }
}

void MapleIatWalkModuleByName(HMODULE mod, MapleIatWalkKind kind) {
    if (!mod) return;
    if (!MapleMemCommittedReadable(mod, sizeof(IMAGE_DOS_HEADER))) return;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    if (dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))
        || static_cast<size_t>(dos->e_lfanew) > 0x100000) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(mod) + dos->e_lfanew);
    if (!MapleMemCommittedReadable(nt, sizeof(IMAGE_NT_HEADERS))) return;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    BYTE* base = reinterpret_cast<BYTE*>(mod);
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (imageSize < sizeof(IMAGE_NT_HEADERS)
        || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > imageSize) {
        return;
    }

    const IMAGE_DATA_DIRECTORY& impDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress && impDir.Size && impDir.VirtualAddress < imageSize) {
        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + impDir.VirtualAddress);
        const BYTE* impEnd = base + impDir.VirtualAddress + impDir.Size;
        int nDesc = 0;
        for (; MapleMemCommittedReadable(desc, sizeof(*desc)) && desc->Name && nDesc < 256
                && reinterpret_cast<BYTE*>(desc) + sizeof(*desc) <= impEnd;
            ++desc, ++nDesc) {
            if (desc->Name >= imageSize || !desc->FirstThunk || desc->FirstThunk >= imageSize) {
                continue;
            }
            const char* dll = reinterpret_cast<const char*>(base + desc->Name);
            if (!MapleCStringInImage(dll, imageSize - desc->Name, 64)) continue;
            if (!MapleImportDllWanted(dll, kind)) continue;
            IMAGE_THUNK_DATA* iat =
                reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
            if (desc->OriginalFirstThunk && desc->OriginalFirstThunk < imageSize) {
                IMAGE_THUNK_DATA* names =
                    reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
                MapleIatWalkNamedThunks(base, imageSize, names, iat, kind);
            } else {
                MapleIatWalkBoundThunks(iat, kind);
            }
        }
    }

    const IMAGE_DATA_DIRECTORY& delayDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayDir.VirtualAddress && delayDir.Size && delayDir.VirtualAddress < imageSize) {
        auto* delay = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delayDir.VirtualAddress);
        const BYTE* delayEnd = base + delayDir.VirtualAddress + delayDir.Size;
        int nDelay = 0;
        for (; MapleMemCommittedReadable(delay, sizeof(*delay)) && delay->DllNameRVA && nDelay < 64
                && reinterpret_cast<BYTE*>(delay) + sizeof(*delay) <= delayEnd;
            ++delay, ++nDelay) {
            const bool rvaBased = (delay->Attributes.AllAttributes & 1) != 0;
            BYTE* dllPtr = MapleImagePtr(base, imageSize, delay->DllNameRVA, rvaBased);
            if (!dllPtr) continue;
            const char* dll = reinterpret_cast<const char*>(dllPtr);
            const size_t dllRemain = static_cast<size_t>((base + imageSize) - dllPtr);
            if (!MapleCStringInImage(dll, dllRemain, 64)) continue;
            if (!MapleImportDllWanted(dll, kind)) continue;
            BYTE* iatPtr = MapleImagePtr(base, imageSize, delay->ImportAddressTableRVA, rvaBased);
            if (!iatPtr) continue;
            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(iatPtr);
            BYTE* namePtr = MapleImagePtr(base, imageSize, delay->ImportNameTableRVA, rvaBased);
            if (namePtr) {
                MapleIatWalkNamedThunks(base, imageSize,
                    reinterpret_cast<IMAGE_THUNK_DATA*>(namePtr), iat, kind);
            } else {
                MapleIatWalkBoundThunks(iat, kind);
            }
        }
    }
}

bool MapleSectionSkipByName(const IMAGE_SECTION_HEADER& sec) {
    char name[9]{};
    memcpy(name, sec.Name, 8);
    return lstrcmpA(name, ".rsrc") == 0
        || lstrcmpA(name, ".reloc") == 0
        || lstrcmpA(name, ".pdata") == 0
        || lstrcmpA(name, ".edata") == 0;
}

void MaplePatchWritablePointerList(HMODULE mod, const MapleIatPair* pairs, int nPairs,
    size_t maxImageSize, int maxPatch) {
    if (!mod || !pairs || nPairs <= 0 || maxPatch <= 0) return;
    if (!MapleMemCommittedReadable(mod, sizeof(IMAGE_DOS_HEADER))) return;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    if (dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))
        || static_cast<size_t>(dos->e_lfanew) > 0x100000) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(mod) + dos->e_lfanew);
    if (!MapleMemCommittedReadable(nt, sizeof(IMAGE_NT_HEADERS))) return;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    BYTE* base = reinterpret_cast<BYTE*>(mod);
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (imageSize < sizeof(IMAGE_NT_HEADERS)) return;
    if (maxImageSize != 0 && imageSize > maxImageSize) return;
    WORD nsec = nt->FileHeader.NumberOfSections;
    if (nsec > 96) nsec = 96;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    if (!MapleMemCommittedReadable(sec, sizeof(IMAGE_SECTION_HEADER) * nsec)) return;
    constexpr size_t kMaxSectionScan = 2 * 1024 * 1024;
    int patched = 0;
    for (WORD i = 0; i < nsec && patched < maxPatch; ++i) {
        const DWORD ch = sec[i].Characteristics;
        if ((ch & IMAGE_SCN_MEM_WRITE) == 0) continue;
        if (ch & IMAGE_SCN_MEM_EXECUTE) continue;
        if (MapleSectionSkipByName(sec[i])) continue;
        DWORD vsz = sec[i].Misc.VirtualSize;
        if (vsz == 0) vsz = sec[i].SizeOfRawData;
        if (vsz < sizeof(void*)) continue;
        if (static_cast<size_t>(sec[i].VirtualAddress) >= imageSize) continue;
        BYTE* start = base + sec[i].VirtualAddress;
        size_t bytes = vsz;
        if (sec[i].VirtualAddress + bytes > imageSize) {
            bytes = imageSize - sec[i].VirtualAddress;
        }
        if (bytes > kMaxSectionScan) bytes = kMaxSectionScan;
        BYTE* end = start + bytes;
        MEMORY_BASIC_INFORMATION mbi{};
        for (BYTE* p = start; p + sizeof(void*) <= end && patched < maxPatch; ) {
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            BYTE* regionEnd = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd <= p) break;
            const DWORD prot = mbi.Protect;
            const DWORD page = prot & 0xFF;
            const bool writable = mbi.State == MEM_COMMIT
                && (prot & PAGE_GUARD) == 0
                && (page == PAGE_READWRITE || page == PAGE_WRITECOPY
                    || page == PAGE_EXECUTE_READWRITE || page == PAGE_EXECUTE_WRITECOPY);
            BYTE* scanEnd = regionEnd < end ? regionEnd : end;
            if (!writable) {
                p = regionEnd;
                continue;
            }
            BYTE* aligned = reinterpret_cast<BYTE*>(
                (reinterpret_cast<ULONG_PTR>(p) + sizeof(void*) - 1) & ~(sizeof(void*) - 1));
            for (; aligned + sizeof(void*) <= scanEnd && patched < maxPatch; aligned += sizeof(void*)) {
                void* cur = *reinterpret_cast<void**>(aligned);
                if (!cur) continue;
                for (int k = 0; k < nPairs; ++k) {
                    if (pairs[k].original && cur == pairs[k].original) {
                        if (MapleIatPatchSlot(reinterpret_cast<void**>(aligned),
                                pairs[k].detour, pairs[k].poll)) {
                            ++patched;
                        }
                        break;
                    }
                }
            }
            p = scanEnd;
        }
    }
}

void MaplePatchWritablePointers(HMODULE mod, size_t maxImageSize) {
    MapleIatPair pairs[48]{};
    const int nPairs = MapleIatFillPairs(pairs, 48);
    MaplePatchWritablePointerList(mod, pairs, nPairs, maxImageSize, 48);
}

void MaplePatchWritableFocusPointers(HMODULE mod, size_t maxImageSize) {
    MapleIatPair pairs[12]{};
    const int nPairs = MapleIatFillFocusPairs(pairs, 12);
    MaplePatchWritablePointerList(mod, pairs, nPairs, maxImageSize, 48);
}

bool MaplePathHasDirToken(const wchar_t* lower, const wchar_t* token) {
    return lower && token && wcsstr(lower, token) != nullptr;
}

bool MapleIsSystemModulePath(const wchar_t* lowerPath) {
    if (!lowerPath || !*lowerPath) return true;
    return MaplePathHasDirToken(lowerPath, L"\\windows\\")
        || MaplePathHasDirToken(lowerPath, L"\\system32\\")
        || MaplePathHasDirToken(lowerPath, L"\\syswow64\\")
        || MaplePathHasDirToken(lowerPath, L"\\winsxs\\");
}

bool MapleIsFakeFocusModulePath(const wchar_t* lowerPath) {
    if (!lowerPath || !*lowerPath) return false;
    const wchar_t* base = lowerPath;
    for (const wchar_t* p = lowerPath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    return lstrcmpW(base, L"fakefocus32.dll") == 0
        || lstrcmpW(base, L"fakefocus64.dll") == 0;
}

bool MapleSkipSensitiveGameModule(const wchar_t* lowerPath) {
    if (!lowerPath || !*lowerPath) return true;
    const wchar_t* base = lowerPath;
    for (const wchar_t* p = lowerPath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    if (wcsstr(base, L"ngs") == base) return true;
    if (wcsstr(base, L"blackcipher") != nullptr) return true;
    if (wcsstr(base, L"easyanticheat") != nullptr) return true;
    if (wcsstr(base, L"battleye") != nullptr) return true;
    if (wcsstr(base, L"beclient") != nullptr) return true;
    if (wcsstr(base, L"xigncode") != nullptr) return true;
    if (wcsstr(base, L"gameguard") != nullptr) return true;
    if (wcsstr(base, L"tenprotect") != nullptr) return true;
    if (lstrcmpW(base, L"npgl.dll") == 0 || lstrcmpW(base, L"npsc.dll") == 0) return true;
    if (wcsstr(base, L"vanguard") != nullptr) return true;
    if (wcsstr(base, L"opencv") != nullptr) return true;
    if (wcsstr(base, L"msvcp") == base || wcsstr(base, L"vcruntime") == base) return true;
    if (wcsstr(base, L"ucrtbase") == base || wcsstr(base, L"concrt") == base) return true;
    if (wcsstr(base, L"webview2") != nullptr || wcsstr(base, L"msedgewebview") != nullptr) {
        return true;
    }
    return false;
}

bool MapleIsDinputModule(HMODULE mod);

void MapleIatWalkGameDirDinputUser32();
void MaplePatchFocusPointersProcessWide();

bool MapleModuleInGameDir(HMODULE mod, const wchar_t* exeDirLower) {
    if (!mod || !exeDirLower || !*exeDirLower) return false;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mod, path, MAX_PATH) || !path[0]) return false;
    CharLowerW(path);
    const size_t n = wcslen(exeDirLower);
    if (n == 0 || wcsncmp(path, exeDirLower, n) != 0) return false;
    return path[n] == L'\\' || path[n] == L'/' || path[n] == 0;
}

struct MapleLdrEntry {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    struct {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR Buffer;
    } FullDllName;
    struct {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR Buffer;
    } BaseDllName;
};

void MapleIatWalkGameDirModules(HMODULE mainMod) {
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH) || !exePath[0]) return;
    CharLowerW(exePath);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) slash = wcsrchr(exePath, L'/');
    if (!slash) return;
    *slash = 0;

#if defined(_WIN64)
    BYTE* peb = reinterpret_cast<BYTE*>(__readgsqword(0x60));
    if (!MapleMemCommittedReadable(peb, 0x20)) return;
    BYTE* ldr = *reinterpret_cast<BYTE**>(peb + 0x18);
    if (!MapleMemCommittedReadable(ldr, 0x20)) return;
    LIST_ENTRY* head = reinterpret_cast<LIST_ENTRY*>(ldr + 0x10);
#else
    BYTE* peb = reinterpret_cast<BYTE*>(__readfsdword(0x30));
    if (!MapleMemCommittedReadable(peb, 0x10)) return;
    BYTE* ldr = *reinterpret_cast<BYTE**>(peb + 0x0C);
    if (!MapleMemCommittedReadable(ldr, 0x14)) return;
    LIST_ENTRY* head = reinterpret_cast<LIST_ENTRY*>(ldr + 0x0C);
#endif
    if (!MapleMemCommittedReadable(head, sizeof(LIST_ENTRY))) return;

    HMODULE extrasMod[48]{};
    int extraN = 0;
    int walked = 0;
    for (LIST_ENTRY* cur = head->Flink;
        cur && cur != head && walked < 128 && extraN < 48;
        cur = cur->Flink, ++walked) {
        if (!MapleMemCommittedReadable(cur, sizeof(LIST_ENTRY))) break;
        auto* entry = CONTAINING_RECORD(cur, MapleLdrEntry, InLoadOrderLinks);
        if (!MapleMemCommittedReadable(entry, sizeof(MapleLdrEntry))) continue;
        HMODULE mod = static_cast<HMODULE>(entry->DllBase);
        if (!mod || mod == mainMod) continue;
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(mod, path, MAX_PATH) || !path[0]) continue;
        CharLowerW(path);
        if (MapleIsSystemModulePath(path) || MapleIsFakeFocusModulePath(path)) continue;
        if (!MapleModuleInGameDir(mod, exePath)) continue;
        if (MapleSkipSensitiveGameModule(path)) continue;
        extrasMod[extraN++] = mod;
    }
    g_mapleExtraModN = extraN;
    for (int i = 0; i < extraN; ++i) {
        g_mapleExtraMods[i] = extrasMod[i];
    }
    for (int i = 0; i < extraN; ++i) {
        const MapleIatWalkKind kind = MapleIsDinputModule(extrasMod[i])
            ? MapleIatWalkKind::DinputUser32Only
            : MapleIatWalkKind::All;
        MapleIatWalkModuleByName(extrasMod[i], kind);
    }
    // 辅助 DLL 可写节只补前景/闪框（禁止 GetAsyncKeyState 等 poll 指针）。
    // 不按 SizeOfImage 跳过：检查 GetForegroundWindow 的缓存常在 >4MB 的游戏 DLL 里。
    // 主程序 poll 约 2；本地 dinput8 user32 再加 GAKS/光标后常 ≥4。
    // 若涨到 ≥10 说明又扫了辅助 DLL 的 poll 指针。
    // 禁止改 dinput8/dinput 可写节（155648 立刻闪退）。
    for (int i = 0; i < extraN; ++i) {
        if (MapleIsDinputModule(extrasMod[i])) continue;
        MaplePatchWritableFocusPointers(extrasMod[i], 0);
    }
}

void MapleIatInstallFromPeb() {
    HMODULE mainMod = GetModuleHandleW(nullptr);
    MapleIatWalkModuleByName(mainMod, MapleIatWalkKind::All);
    MaplePatchWritablePointers(mainMod, 0);
    // 主程序 poll API 已补上时仍要扫游戏目录：SetForegroundWindow/FlashWindow
    // 常在辅助 DLL 的 IAT 里。禁止 Toolhelp 全模块。
    MapleIatWalkGameDirModules(mainMod);
    // 已加载 dinput8/dinput（含系统目录）只补 user32 IAT，禁止 GPA/可写节。
    MapleIatWalkGameDirDinputUser32();
    // 打包器常把 GFW/IsWindowVisible 缓存在堆上，PE 节扫描看不到。
    MaplePatchFocusPointersProcessWide();
}

UINT MapleVkToDik(UINT vk) {
    switch (vk) {
    case VK_ESCAPE: return 0x01;
    case VK_TAB: return 0x0F;
    case VK_RETURN: return 0x1C;
    case VK_SPACE: return 0x39;
    case VK_LSHIFT: return 0x2A;
    case VK_RSHIFT: return 0x36;
    case VK_LCONTROL: case VK_CONTROL: return 0x1D;
    case VK_RCONTROL: return 0x9D;
    case VK_LMENU: case VK_MENU: return 0x38;
    case VK_RMENU: return 0xB8;
    case VK_LEFT: return 0xCB;
    case VK_RIGHT: return 0xCD;
    case VK_UP: return 0xC8;
    case VK_DOWN: return 0xD0;
    case VK_INSERT: return 0xD2;
    case VK_DELETE: return 0xD3;
    case VK_HOME: return 0xC7;
    case VK_END: return 0xCF;
    case VK_PRIOR: return 0xC9;
    case VK_NEXT: return 0xD1;
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return 0x3B + (vk - VK_F1);
    const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    return sc & 0xFF;
}

bool MapleFillDiKeyboard(DWORD cb, void* data) {
    // c_dfDIKeyboard is exactly 256. DIJOYSTATE2 is 272 — must not treat as keyboard.
    if (!data || cb != 256) return false;
    const fakefocus::SoftInputState* st = SoftState();
    if (!st || !(st->flags & fakefocus::kSoftFlagKeysValid)) return false;
    auto* keys = static_cast<BYTE*>(data);
    std::memset(keys, 0, 256);
    for (int vk = 1; vk < 256; ++vk) {
        if (!st->down[static_cast<size_t>(vk)]) continue;
        const UINT dik = MapleVkToDik(static_cast<UINT>(vk));
        if (dik != 0 && dik < 256) keys[dik] = 0x80;
    }
    return true;
}

bool MapleFillDiMouse(DWORD cb, void* data) {
    if (!data || (cb != 16 && cb != 20)) return false;
    const fakefocus::SoftInputState* st = SoftState();
    if (!st || !(st->flags & fakefocus::kSoftFlagCursorValid)) return false;
    const int x = st->cursorScreenX;
    const int y = st->cursorScreenY;
    int dx = 0;
    int dy = 0;
    if (g_diHaveMouse) {
        dx = x - g_diLastMouseX;
        dy = y - g_diLastMouseY;
    }
    g_diLastMouseX = x;
    g_diLastMouseY = y;
    g_diHaveMouse = true;
    std::memset(data, 0, cb);
    auto* p = static_cast<LONG*>(data);
    p[0] = dx;
    p[1] = dy;
    p[2] = 0;
    auto* buttons = reinterpret_cast<BYTE*>(p + 3);
    buttons[0] = (st->flags & fakefocus::kSoftFlagKeysValid) && st->down[VK_LBUTTON] ? 0x80 : 0;
    buttons[1] = (st->flags & fakefocus::kSoftFlagKeysValid) && st->down[VK_RBUTTON] ? 0x80 : 0;
    buttons[2] = (st->flags & fakefocus::kSoftFlagKeysValid) && st->down[VK_MBUTTON] ? 0x80 : 0;
    buttons[3] = 0;
    return true;
}

int MapleDiDeviceKind(void* self) {
    if (!self) return 0;
    for (int i = 0; i < 8; ++i) {
        if (g_mapleDiKind[i].self == self) return g_mapleDiKind[i].kind;
    }
    return 0;
}

void MapleNoteDiDeviceKind(void* self, DWORD cb) {
    if (!self) return;
    int kind = 0;
    if (cb == 256) kind = 1;
    else if (cb == 16 || cb == 20) kind = 2;
    else return;
    for (int i = 0; i < 8; ++i) {
        if (g_mapleDiKind[i].self == self) {
            g_mapleDiKind[i].kind = kind;
            return;
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (!g_mapleDiKind[i].self) {
            g_mapleDiKind[i].self = self;
            g_mapleDiKind[i].kind = kind;
            return;
        }
    }
    g_mapleDiKind[0].self = self;
    g_mapleDiKind[0].kind = kind;
}

bool MapleFillDiKeyboardData(DWORD cb, void* data, DWORD* n, DWORD flags) {
    if (!n) return false;
    if (cb != 16 && cb != 20 && cb != 24) return false;
    const fakefocus::SoftInputState* st = SoftState();
    BYTE now[256]{};
    if (st && (st->flags & fakefocus::kSoftFlagKeysValid)) {
        for (int vk = 1; vk < 256; ++vk) {
            if (!st->down[static_cast<size_t>(vk)]) continue;
            const UINT dik = MapleVkToDik(static_cast<UINT>(vk));
            if (dik != 0 && dik < 256) now[dik] = 0x80;
        }
    }
    const bool peek = (flags & 1u) != 0;
    const DWORD maxOut = *n > 64u ? 64u : *n;
    DWORD written = 0;
    auto emit = [&](int dik) {
        if (!data || written >= maxOut) return false;
        BYTE row[24]{};
        *reinterpret_cast<DWORD*>(row + 0) = static_cast<DWORD>(dik);
        *reinterpret_cast<DWORD*>(row + 4) = now[dik] ? 0x80u : 0u;
        *reinterpret_cast<DWORD*>(row + 8) = GetTickCount();
        *reinterpret_cast<DWORD*>(row + 12) = ++g_mapleDiSeq;
        std::memcpy(static_cast<BYTE*>(data) + written * cb, row, cb);
        ++written;
        return true;
    };
    if (!data) {
        for (int dik = 1; dik < 256; ++dik) {
            if (now[dik] != g_maplePrevDik[dik]) ++written;
        }
        *n = written;
        if (!peek) std::memcpy(g_maplePrevDik, now, 256);
        return true;
    }
    for (int dik = 1; dik < 256; ++dik) {
        if (now[dik] == g_maplePrevDik[dik]) continue;
        if (written >= maxOut) break;
        emit(dik);
        if (!peek) g_maplePrevDik[dik] = now[dik];
    }
    *n = written;
    return true;
}

bool MaplePatchVtPtr(void** slot, void* detour, int index) {
    if (!slot || !detour) return false;
    if (*slot == detour) return true;
    if (g_diVtPatchN >= kMapleDiVtCap) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    g_diVtPatch[g_diVtPatchN].slot = slot;
    g_diVtPatch[g_diVtPatchN].orig = *slot;
    g_diVtPatch[g_diVtPatchN].index = index;
    ++g_diVtPatchN;
    *slot = detour;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

bool MapleRel32Reachable(void* target, void* detour) {
#if defined(_M_X64) || defined(__x86_64__)
    (void)target;
    (void)detour;
    return true;
#else
    if (!target || !detour) return false;
    const INT64 rel = static_cast<INT64>(reinterpret_cast<ULONG_PTR>(detour))
        - (static_cast<INT64>(reinterpret_cast<ULONG_PTR>(target)) + 5);
    return rel >= static_cast<INT64>(INT32_MIN) && rel <= static_cast<INT64>(INT32_MAX);
#endif
}

bool MapleAddressInNtdll(void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || !mbi.AllocationBase) return false;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(static_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH) || !path[0]) {
        return false;
    }
    CharLowerW(path);
    return wcsstr(path, L"\\ntdll.dll") != nullptr;
}

bool MapleIsSplitUnsafePrefix(const BYTE* b) {
    if (!b) return true;
    // 6 字节间接跳：5 字节 E9 会留下 1 字节垃圾。
    if (b[0] == 0xFF && (b[1] == 0x25 || b[1] == 0x15 || b[1] == 0x12)) return true;
    if (b[0] == 0xFF && b[1] == 0xD2) return true;  // call edx，共享 Wow64 门
    if (b[0] == 0xEB) return true;  // 短跳 2 字节
    return false;
}

bool MapleLooksLikeHookableDiFn(void* target) {
    if (!target || !MapleMemCommittedReadable(target, 16)) return false;
    const BYTE* b = static_cast<const BYTE*>(target);
    if (MapleAddressInNtdll(target)) return false;
    if (MapleIsSplitUnsafePrefix(b)) return false;
    // 已是 JMP/INT3/RET：dinput 方法体不要覆盖（可能是跳转表）。
    if (b[0] == 0xE9 || b[0] == 0xEB || b[0] == 0xCC || b[0] == 0xC3 || b[0] == 0xC2) {
        return false;
    }
    return true;
}

void* MapleFollowSameImageE9(void* target) {
    if (!target || !MapleMemCommittedReadable(target, 5)) return target;
    const BYTE* b = static_cast<const BYTE*>(target);
    if (b[0] != 0xE9) return target;
    INT32 rel = 0;
    std::memcpy(&rel, b + 1, sizeof(rel));
    void* dest = static_cast<BYTE*>(target) + 5 + rel;
    MEMORY_BASIC_INFORMATION srcInfo{};
    MEMORY_BASIC_INFORMATION dstInfo{};
    if (VirtualQuery(target, &srcInfo, sizeof(srcInfo)) != sizeof(srcInfo)) return target;
    if (VirtualQuery(dest, &dstInfo, sizeof(dstInfo)) != sizeof(dstInfo)) return target;
    if (!srcInfo.AllocationBase || srcInfo.AllocationBase != dstInfo.AllocationBase) {
        return target;
    }
    if (MapleAddressInNtdll(dest)) return target;
    if (!MapleLooksLikeHookableDiFn(dest)) return target;
    return dest;
}

bool MapleInstallLiveDiFn(void* target, void* detour, fakefocus::InlineHook* slots, int n,
    DWORD diagBit) {
    if (!target || !detour || !slots || n <= 0) return false;
    target = MapleFollowSameImageE9(target);
    if (target == detour) return false;
    if (MapleAddressInNtdll(target)) return false;
    for (int i = 0; i < n; ++i) {
        if (slots[i].installed && slots[i].target == target) {
            if (diagBit) g_mapleDiag |= diagBit;
            return true;
        }
    }
    if (!MapleLooksLikeHookableDiFn(target)) return false;
    if (!MapleRel32Reachable(target, detour)) return false;
    for (int i = 0; i < n; ++i) {
        if (slots[i].installed) continue;
        if (!fakefocus::InstallInlineHook(slots[i], target, detour)) return false;
        if (diagBit) g_mapleDiag |= diagBit;
        return true;
    }
    return false;
}

void MapleRestoreLiveDiHooks() {
    for (int i = kMapleLiveDiN - 1; i >= 0; --i) {
        fakefocus::RemoveInlineHook(g_hookDiLiveState[i]);
        fakefocus::RemoveInlineHook(g_hookDiLiveAcquire[i]);
    }
    std::memset(g_maplePrevDik, 0, sizeof(g_maplePrevDik));
    g_mapleDiSeq = 0;
    std::memset(g_mapleDiKind, 0, sizeof(g_mapleDiKind));
}

#if defined(_M_IX86)
// COM 设备方法是 thiscall（this 在 ecx）。x86 用 fastcall+占位 edx 对齐栈参数。
HRESULT __fastcall Hook_DiAcquire(void* self) {
    (void)self;
    return 0;
}

HRESULT __fastcall Hook_DiKbGetDeviceState(void* self, void* /*edx*/, DWORD cb, void* data) {
    MapleBumpHit(&g_mapleHitDiState);
    MapleNoteDiStateCb(cb);
    MaplePublishHits();
    MapleNoteDiDeviceKind(self, cb);
    if (MapleFillDiKeyboard(cb, data)) return 0;
    if (MapleFillDiMouse(cb, data)) return 0;
    // 不是键盘/鼠标状态块：不要 memset 0（误钩其它设备时会把摇杆等读空）。
    return static_cast<HRESULT>(0x80070057L);
}

HRESULT __fastcall Hook_DiGetDeviceData(void* self, void* /*edx*/, DWORD cb, void* data, DWORD* n, DWORD flags) {
    MapleBumpHit(&g_mapleHitDiData);
    if (!n) return static_cast<HRESULT>(0x80070057L);
    const int kind = MapleDiDeviceKind(self);
    if (kind == 2) {
        *n = 0;
        return 0;
    }
    if (MapleFillDiKeyboardData(cb, data, n, flags)) return 0;
    *n = 0;
    return 0;
}
#else
HRESULT WINAPI Hook_DiAcquire(void* self) {
    (void)self;
    return 0;
}

HRESULT WINAPI Hook_DiKbGetDeviceState(void* self, DWORD cb, void* data) {
    MapleBumpHit(&g_mapleHitDiState);
    MapleNoteDiStateCb(cb);
    MaplePublishHits();
    MapleNoteDiDeviceKind(self, cb);
    if (MapleFillDiKeyboard(cb, data)) return 0;
    if (MapleFillDiMouse(cb, data)) return 0;
    return static_cast<HRESULT>(0x80070057L);
}

HRESULT WINAPI Hook_DiGetDeviceData(void* self, DWORD cb, void* data, DWORD* n, DWORD flags) {
    MapleBumpHit(&g_mapleHitDiData);
    if (!n) return static_cast<HRESULT>(0x80070057L);
    const int kind = MapleDiDeviceKind(self);
    if (kind == 2) {
        *n = 0;
        return 0;
    }
    if (MapleFillDiKeyboardData(cb, data, n, flags)) return 0;
    *n = 0;
    return 0;
}
#endif

#if defined(_M_IX86)
HRESULT __fastcall Hook_DiMouseGetDeviceState(void* self, void* edx, DWORD cb, void* data) {
    return Hook_DiKbGetDeviceState(self, edx, cb, data);
}
#else
HRESULT WINAPI Hook_DiMouseGetDeviceState(void* self, DWORD cb, void* data) {
    return Hook_DiKbGetDeviceState(self, cb, data);
}
#endif

#if defined(_M_IX86)
HRESULT __fastcall Hook_DiSetCooperativeLevel(void* self, void* /*edx*/, HWND hwnd, DWORD flags) {
#else
HRESULT WINAPI Hook_DiSetCooperativeLevel(void* self, HWND hwnd, DWORD flags) {
#endif
    // DISCL_BACKGROUND|NONEXCLUSIVE，去掉 FOREGROUND|EXCLUSIVE（dinput8hook / Reloaded DInputPleaseCooperate）
    flags = (flags | 0x08u | 0x02u) & ~(0x04u | 0x01u);
    for (int i = 0; i < g_diVtPatchN; ++i) {
        if (g_diVtPatch[i].index != 13 || !g_diVtPatch[i].orig) continue;
#if defined(_M_IX86)
        auto orig = reinterpret_cast<HRESULT (__thiscall*)(void*, HWND, DWORD)>(g_diVtPatch[i].orig);
#else
        auto orig = reinterpret_cast<HRESULT (WINAPI*)(void*, HWND, DWORD)>(g_diVtPatch[i].orig);
#endif
        return orig(self, hwnd, flags);
    }
    return 0;
}

bool MaplePatchVtableSlot(void* object, int index, void* detour, void** savedOrig, void*** savedSlot) {
    if (!object || !detour || !savedOrig || !savedSlot) return false;
    void** vtable = *reinterpret_cast<void***>(object);
    if (!vtable) return false;
    void** slot = &vtable[index];
    if (*slot == detour) return true;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *savedOrig = *slot;
    *savedSlot = slot;
    *slot = detour;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

using DiCreateDeviceFn = HRESULT(WINAPI*)(void*, const GUID*, void**, void*);

static const GUID kMapleSysKeyboard =
    {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kMapleSysMouse =
    {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

HRESULT WINAPI Hook_DiCreateDevice(void* self, const GUID* guid, void** out, void* unk) {
    auto orig = reinterpret_cast<DiCreateDeviceFn>(g_diCreateDeviceOrig);
    if (!orig) return static_cast<HRESULT>(0x80004005L);
    const HRESULT hr = orig(self, guid, out, unk);
    if (FAILED(hr) || !out || !*out || !guid) return hr;
    if (!MapleMemCommittedReadable(*out, sizeof(void*))) return hr;
    void** vt = *reinterpret_cast<void***>(*out);
    if (!vt) return hr;
    if (!MapleMemCommittedReadable(vt, sizeof(void*) * 14)) return hr;
    void* acquireFn = vt[7];
    void* stateFn = vt[9];
    MaplePatchVtPtr(&vt[7], reinterpret_cast<void*>(&Hook_DiAcquire), 7);
    if (InlineIsEqualGUID(*guid, kMapleSysKeyboard) && !g_diKbStateSlot) {
        MaplePatchVtableSlot(*out, 9, reinterpret_cast<void*>(&Hook_DiKbGetDeviceState),
            &g_diKbStateOrig, &g_diKbStateSlot);
    } else if (InlineIsEqualGUID(*guid, kMapleSysMouse) && !g_diMouseStateSlot) {
        MaplePatchVtableSlot(*out, 9, reinterpret_cast<void*>(&Hook_DiMouseGetDeviceState),
            &g_diMouseStateOrig, &g_diMouseStateSlot);
    } else {
        MaplePatchVtPtr(&vt[9], reinterpret_cast<void*>(&Hook_DiKbGetDeviceState), 9);
    }
    MaplePatchVtPtr(&vt[10], reinterpret_cast<void*>(&Hook_DiGetDeviceData), 10);
    MaplePatchVtPtr(&vt[13], reinterpret_cast<void*>(&Hook_DiSetCooperativeLevel), 13);
    if (InlineIsEqualGUID(*guid, kMapleSysKeyboard)) MapleNoteDiDeviceKind(*out, 256);
    else if (InlineIsEqualGUID(*guid, kMapleSysMouse)) MapleNoteDiDeviceKind(*out, 16);
    MapleInstallLiveDiFn(acquireFn, reinterpret_cast<void*>(&Hook_DiAcquire), g_hookDiLiveAcquire, kMapleLiveDiN,
        0);
    MapleInstallLiveDiFn(stateFn, reinterpret_cast<void*>(&Hook_DiKbGetDeviceState), g_hookDiLiveState, kMapleLiveDiN,
        0x0800);
    return hr;
}

HRESULT WINAPI Hook_DirectInput8Create(HINSTANCE hinst, DWORD version,
    const GUID* iid, void** out, void* unk) {
    if (!g_realDiCreate) {
        HMODULE di = GetModuleHandleW(L"dinput8.dll");
        if (!di) return static_cast<HRESULT>(0x80004005L);
        g_realDiCreate = reinterpret_cast<DiCreateFn>(
            GetProcAddress(di, "DirectInput8Create"));
    }
    if (!g_realDiCreate) return static_cast<HRESULT>(0x80004005L);
    const HRESULT hr = g_realDiCreate(hinst, version, iid, out, unk);
    if (SUCCEEDED(hr) && out && *out && !g_diCreateDeviceOrig) {
        MaplePatchVtableSlot(*out, 3, reinterpret_cast<void*>(&Hook_DiCreateDevice),
            &g_diCreateDeviceOrig, &g_diCreateDeviceSlot);
    }
    return hr;
}

void MapleHookDiFactoryObject(void* diObj) {
    if (!diObj || g_diCreateDeviceOrig) return;
    MaplePatchVtableSlot(diObj, 3, reinterpret_cast<void*>(&Hook_DiCreateDevice),
        &g_diCreateDeviceOrig, &g_diCreateDeviceSlot);
}

HRESULT WINAPI Hook_DirectInputCreateA(HINSTANCE hinst, DWORD version, void** out, void* unk) {
    if (!g_realDiCreateA) {
        HMODULE di = GetModuleHandleW(L"dinput.dll");
        if (!di) return static_cast<HRESULT>(0x80004005L);
        g_realDiCreateA = reinterpret_cast<DiCreateAFn>(
            GetProcAddress(di, "DirectInputCreateA"));
    }
    if (!g_realDiCreateA) return static_cast<HRESULT>(0x80004005L);
    const HRESULT hr = g_realDiCreateA(hinst, version, out, unk);
    if (SUCCEEDED(hr) && out) MapleHookDiFactoryObject(*out);
    return hr;
}

HRESULT WINAPI Hook_DirectInputCreateW(HINSTANCE hinst, DWORD version, void** out, void* unk) {
    if (!g_realDiCreateW) {
        HMODULE di = GetModuleHandleW(L"dinput.dll");
        if (!di) return static_cast<HRESULT>(0x80004005L);
        g_realDiCreateW = reinterpret_cast<DiCreateAFn>(
            GetProcAddress(di, "DirectInputCreateW"));
    }
    if (!g_realDiCreateW) return static_cast<HRESULT>(0x80004005L);
    const HRESULT hr = g_realDiCreateW(hinst, version, out, unk);
    if (SUCCEEDED(hr) && out) MapleHookDiFactoryObject(*out);
    return hr;
}

HRESULT WINAPI Hook_DirectInputCreateEx(HINSTANCE hinst, DWORD version,
    const GUID* iid, void** out, void* unk) {
    if (!g_realDiCreateEx) {
        HMODULE di = GetModuleHandleW(L"dinput.dll");
        if (!di) return static_cast<HRESULT>(0x80004005L);
        g_realDiCreateEx = reinterpret_cast<DiCreateFn>(
            GetProcAddress(di, "DirectInputCreateEx"));
    }
    if (!g_realDiCreateEx) return static_cast<HRESULT>(0x80004005L);
    const HRESULT hr = g_realDiCreateEx(hinst, version, iid, out, unk);
    if (SUCCEEDED(hr) && out) MapleHookDiFactoryObject(*out);
    return hr;
}

void MapleIatWalkGameDirDinputUser32() {
    // 星辰可能加载 SysWOW64\dinput8，游戏目录那份反而不在 PEB 游戏目录列表。
    // 只补 user32 IAT，仍禁止 GPA / DirectInputCreate* / 可写节。
    HMODULE mods[2] = {
        GetModuleHandleW(L"dinput8.dll"),
        GetModuleHandleW(L"dinput.dll"),
    };
    for (int i = 0; i < 2; ++i) {
        HMODULE mod = mods[i];
        if (!mod) continue;
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(mod, path, MAX_PATH) && path[0]) {
            CharLowerW(path);
            if (MapleIsFakeFocusModulePath(path)) continue;
        }
        const int before = g_mapleInputHookCount;
        MapleIatWalkModuleByName(mod, MapleIatWalkKind::DinputUser32Only);
        // dinput8/dinput 是**懒加载**的：注入那一轮 PEB 扫描时它们往往还没进进程，
        // 这里补到槽就记一位，方便日志区分「本地 dinput8 没被补」和「根本没加载」。
        if (g_mapleInputHookCount > before) g_mapleDiag |= kMapleIatDinputWalked;
    }
}

void MaplePatchFocusPointersProcessWide() {
    MapleIatPair pairs[12]{};
    const int nPairs = MapleIatFillFocusPairs(pairs, 12);
    if (nPairs <= 0) return;
    int patched = 0;
    BYTE* addr = nullptr;
    for (int regions = 0; regions < 4096 && patched < 48; ++regions) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        BYTE* regionEnd = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= addr) break;
        const DWORD page = mbi.Protect & 0xFF;
        const bool writable = mbi.State == MEM_COMMIT
            && (mbi.Protect & PAGE_GUARD) == 0
            && (page == PAGE_READWRITE || page == PAGE_WRITECOPY);
        if (writable) {
            HMODULE owner = static_cast<HMODULE>(mbi.AllocationBase);
            bool skip = MapleIsDinputModule(owner);
            if (!skip && owner) {
                wchar_t path[MAX_PATH]{};
                if (GetModuleFileNameW(owner, path, MAX_PATH) && path[0]) {
                    CharLowerW(path);
                    skip = MapleIsFakeFocusModulePath(path);
                }
            }
            if (!skip) {
                BYTE* aligned = reinterpret_cast<BYTE*>(
                    (reinterpret_cast<ULONG_PTR>(addr) + sizeof(void*) - 1)
                    & ~(sizeof(void*) - 1));
                BYTE* scanEnd = regionEnd;
                for (; aligned + sizeof(void*) <= scanEnd && patched < 48;
                    aligned += sizeof(void*)) {
                    void* cur = *reinterpret_cast<void**>(aligned);
                    if (!cur) continue;
                    for (int k = 0; k < nPairs; ++k) {
                        if (pairs[k].original && cur == pairs[k].original) {
                            if (MapleIatPatchSlot(reinterpret_cast<void**>(aligned),
                                    pairs[k].detour, false)) {
                                ++patched;
                            }
                            break;
                        }
                    }
                }
            }
        }
        addr = regionEnd;
    }
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR name) {
    if (g_mapleSafe) MapleMarkCalled(kMapleCalledGpa);
    auto orig = g_mapleRealGetProcAddress;
    FARPROC real = orig ? orig(module, name) : nullptr;
    if (!module || !name) return real;
    if (HIWORD(reinterpret_cast<ULONG_PTR>(name)) == 0) return real;
    void* detour = MapleDetourForImportName(name, MapleIatWalkKind::All);
    if (!detour || detour == reinterpret_cast<void*>(&Hook_GetProcAddress)) return real;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH) || !path[0]) return real;
    CharLowerW(path);
    if (wcsstr(path, L"user32") || wcsstr(path, L"ntuser") || wcsstr(path, L"win32u")
        || wcsstr(path, L"dinput8")
        || wcsstr(path, L"\\dinput.dll") || wcsstr(path, L"/dinput.dll")) {
        return reinterpret_cast<FARPROC>(detour);
    }
    return real;
}

void* MapleDetourForImportName(const char* name, MapleIatWalkKind kind) {
    if (!name || !*name) return nullptr;
    char base[96]{};
    MapleCopyImportBaseName(name, base, 96);
    const char* n = base[0] ? base : name;
    if (lstrcmpA(n, "GetCursorPos") == 0) return reinterpret_cast<void*>(&Hook_GetCursorPos);
    if (lstrcmpA(n, "SetCursorPos") == 0) return reinterpret_cast<void*>(&Hook_SetCursorPos);
    if (lstrcmpA(n, "ClipCursor") == 0) return reinterpret_cast<void*>(&Hook_ClipCursor);
    if (lstrcmpA(n, "ShowCursor") == 0) return reinterpret_cast<void*>(&Hook_ShowCursor);
    if (lstrcmpA(n, "GetAsyncKeyState") == 0)
        return reinterpret_cast<void*>(&Hook_GetAsyncKeyState);
    if (lstrcmpA(n, "GetKeyState") == 0) return reinterpret_cast<void*>(&Hook_GetKeyState);
    if (lstrcmpA(n, "GetKeyboardState") == 0)
        return reinterpret_cast<void*>(&Hook_GetKeyboardState);
    if (lstrcmpA(n, "GetForegroundWindow") == 0)
        return reinterpret_cast<void*>(&Hook_GetForegroundWindow);
    if (lstrcmpA(n, "SetForegroundWindow") == 0)
        return reinterpret_cast<void*>(&Hook_SetForegroundWindow);
    if (lstrcmpA(n, "FlashWindow") == 0)
        return reinterpret_cast<void*>(&Hook_FlashWindow);
    if (lstrcmpA(n, "FlashWindowEx") == 0)
        return reinterpret_cast<void*>(&Hook_FlashWindowEx);
    if (lstrcmpA(n, "SwitchToThisWindow") == 0)
        return reinterpret_cast<void*>(&Hook_SwitchToThisWindow);
    if (lstrcmpA(n, "GetActiveWindow") == 0)
        return reinterpret_cast<void*>(&Hook_GetActiveWindow);
    if (lstrcmpA(n, "GetFocus") == 0) return reinterpret_cast<void*>(&Hook_GetFocus);
    if (lstrcmpA(n, "IsIconic") == 0) return reinterpret_cast<void*>(&Hook_IsIconic);
    if (lstrcmpA(n, "IsWindowVisible") == 0)
        return reinterpret_cast<void*>(&Hook_IsWindowVisible);
    if (lstrcmpA(n, "PeekMessageW") == 0)
        return reinterpret_cast<void*>(&Hook_MaplePeekMessageW);
    if (lstrcmpA(n, "PeekMessageA") == 0)
        return reinterpret_cast<void*>(&Hook_MaplePeekMessageA);
    if (lstrcmpA(n, "GetMessageW") == 0)
        return reinterpret_cast<void*>(&Hook_MapleGetMessageW);
    if (lstrcmpA(n, "GetMessageA") == 0)
        return reinterpret_cast<void*>(&Hook_MapleGetMessageA);
    if (lstrcmpA(n, "DispatchMessageW") == 0)
        return reinterpret_cast<void*>(&Hook_MapleDispatchMessageW);
    if (lstrcmpA(n, "DispatchMessageA") == 0)
        return reinterpret_cast<void*>(&Hook_MapleDispatchMessageA);
    if (lstrcmpA(n, "CallWindowProcW") == 0)
        return reinterpret_cast<void*>(&Hook_MapleCallWindowProcW);
    if (lstrcmpA(n, "CallWindowProcA") == 0)
        return reinterpret_cast<void*>(&Hook_MapleCallWindowProcA);
    if (lstrcmpA(n, "NtUserGetForegroundWindow") == 0)
        return reinterpret_cast<void*>(&Hook_GetForegroundWindow);
    if (lstrcmpA(n, "NtUserGetAsyncKeyState") == 0)
        return reinterpret_cast<void*>(&Hook_GetAsyncKeyState);
    if (lstrcmpA(n, "NtUserGetKeyState") == 0)
        return reinterpret_cast<void*>(&Hook_GetKeyState);
    if (lstrcmpA(n, "NtUserGetKeyboardState") == 0)
        return reinterpret_cast<void*>(&Hook_GetKeyboardState);
    if (lstrcmpA(n, "NtUserGetCursorPos") == 0)
        return reinterpret_cast<void*>(&Hook_GetCursorPos);
    if (lstrcmpA(n, "NtUserSetCursorPos") == 0)
        return reinterpret_cast<void*>(&Hook_SetCursorPos);
    if (kind == MapleIatWalkKind::DinputUser32Only) return nullptr;
    if (lstrcmpA(n, "DirectInput8Create") == 0)
        return reinterpret_cast<void*>(&Hook_DirectInput8Create);
    if (lstrcmpA(n, "DirectInputCreateA") == 0)
        return reinterpret_cast<void*>(&Hook_DirectInputCreateA);
    if (lstrcmpA(n, "DirectInputCreateW") == 0)
        return reinterpret_cast<void*>(&Hook_DirectInputCreateW);
    if (lstrcmpA(n, "DirectInputCreateEx") == 0)
        return reinterpret_cast<void*>(&Hook_DirectInputCreateEx);
    if (lstrcmpA(n, "GetProcAddress") == 0)
        return reinterpret_cast<void*>(&Hook_GetProcAddress);
    return nullptr;
}

void MapleMarkIatDetour(void* detour) {
    if (!detour) return;
    if (detour == reinterpret_cast<void*>(&Hook_GetCursorPos)) g_mapleDiag |= 0x0001;
    else if (detour == reinterpret_cast<void*>(&Hook_GetAsyncKeyState)) g_mapleDiag |= 0x0002;
    else if (detour == reinterpret_cast<void*>(&Hook_GetKeyState)) g_mapleDiag |= 0x0004;
    else if (detour == reinterpret_cast<void*>(&Hook_GetKeyboardState)) g_mapleDiag |= 0x0008;
    else if (detour == reinterpret_cast<void*>(&Hook_DirectInput8Create)
        || detour == reinterpret_cast<void*>(&Hook_DirectInputCreateA)
        || detour == reinterpret_cast<void*>(&Hook_DirectInputCreateW)
        || detour == reinterpret_cast<void*>(&Hook_DirectInputCreateEx)) {
        g_mapleDiag |= 0x0010;
    }
    if (detour == reinterpret_cast<void*>(&Hook_GetForegroundWindow)) g_mapleDiag |= 0x0200;
    if (detour == reinterpret_cast<void*>(&Hook_SetForegroundWindow)
        || detour == reinterpret_cast<void*>(&Hook_SwitchToThisWindow)) {
        g_mapleDiag |= 0x0400;
    }
    if (detour == reinterpret_cast<void*>(&Hook_FlashWindow)
        || detour == reinterpret_cast<void*>(&Hook_FlashWindowEx)) {
        g_mapleDiag |= 0x0040;
    }
    if (detour == reinterpret_cast<void*>(&Hook_MaplePeekMessageW)
        || detour == reinterpret_cast<void*>(&Hook_MaplePeekMessageA)
        || detour == reinterpret_cast<void*>(&Hook_MapleGetMessageW)
        || detour == reinterpret_cast<void*>(&Hook_MapleGetMessageA)
        || detour == reinterpret_cast<void*>(&Hook_MapleDispatchMessageW)
        || detour == reinterpret_cast<void*>(&Hook_MapleDispatchMessageA)
        || detour == reinterpret_cast<void*>(&Hook_MapleCallWindowProcW)
        || detour == reinterpret_cast<void*>(&Hook_MapleCallWindowProcA)
        || detour == reinterpret_cast<void*>(&Hook_MapleTranslateMessage)) {
        g_mapleDiag |= 0x10000;
    }
    // GetProcAddress 本身被补上，才拦得住「运行时动态解析 user32!GetKeyboardState」这类客户端。
    if (detour == reinterpret_cast<void*>(&Hook_GetProcAddress)) {
        g_mapleDiag |= kMapleIatGpaPatched;
    }
}

bool MaplePtrInModuleExec(const void* p, BYTE* base, size_t imageSize) {
    if (!p || !base || imageSize < 16) return false;
    const BYTE* addr = static_cast<const BYTE*>(p);
    if (addr < base || addr >= base + imageSize) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
    switch (mbi.Protect & 0xFF) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

int MapleCountDiVtableMethods(void** vt, BYTE* base, size_t imageSize, int maxN) {
    if (!vt || maxN < 8) return 0;
    if (!MapleMemCommittedReadable(vt, sizeof(void*) * 8)) return 0;
    int n = 0;
    for (; n < maxN; ++n) {
        if (!MapleMemCommittedReadable(&vt[n], sizeof(void*))) break;
        void* fn = vt[n];
        if (!fn || !MaplePtrInModuleExec(fn, base, imageSize)) break;
    }
    return n;
}

bool MapleVtableInModuleData(void** vt, BYTE* base, size_t imageSize) {
    if (!vt || !base || imageSize < sizeof(void*) * 27) return false;
    const BYTE* p = reinterpret_cast<const BYTE*>(vt);
    if (p < base || p + sizeof(void*) * 27 > base + imageSize) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(vt, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
    const DWORD page = mbi.Protect & 0xFF;
    return page == PAGE_READONLY || page == PAGE_READWRITE || page == PAGE_WRITECOPY
        || page == PAGE_EXECUTE_READ || page == PAGE_EXECUTE_READWRITE
        || page == PAGE_EXECUTE_WRITECOPY;
}

bool MapleDiSlotsLookDistinct(void** vt) {
    if (!vt) return false;
    if (!MapleMemCommittedReadable(vt, sizeof(void*) * 11)) return false;
    if (vt[7] == vt[8] || vt[8] == vt[9] || vt[9] == vt[10] || vt[7] == vt[9]) return false;
    auto farEnough = [](void* a, void* b) {
        const ULONG_PTR x = reinterpret_cast<ULONG_PTR>(a);
        const ULONG_PTR y = reinterpret_cast<ULONG_PTR>(b);
        const ULONG_PTR d = x > y ? x - y : y - x;
        return d >= 16;
    };
    return farEnough(vt[7], vt[9]) && farEnough(vt[9], vt[10]) && farEnough(vt[7], vt[8]);
}

bool MapleLooksLikeDeviceVtable(void** vt, BYTE* base, size_t imageSize) {
    if (!vt) return false;
    const int n = MapleCountDiVtableMethods(vt, base, imageSize, 40);
    // IDirectInputDevice2=27 / Device7=29 / Device8=32。18 方法跳转表会闪退，仍拒绝。
    // 相邻两张 Device8 表会把计数顶到 40，不能再因 n>32/36 丢掉真表。
    if (n < 27) return false;
    if (!MapleDiSlotsLookDistinct(vt)) return false;
    // 设备对象只在堆上持有虚表指针时，模块内没有第二份引用——不能再要求 ImageHasPointerTo。
    return MapleVtableInModuleData(vt, base, imageSize);
}

bool MapleHookDeviceVtableSlots(void** vt) {
    if (!vt) return false;
    if (!MapleMemCommittedReadable(vt, sizeof(void*) * 14)) return false;
    void* acquireFn = vt[7];
    void* stateFn = vt[9];
    const bool acquireOk = MaplePatchVtPtr(&vt[7], reinterpret_cast<void*>(&Hook_DiAcquire), 7);
    const bool stateOk = MaplePatchVtPtr(&vt[9], reinterpret_cast<void*>(&Hook_DiKbGetDeviceState), 9);
    MaplePatchVtPtr(&vt[10], reinterpret_cast<void*>(&Hook_DiGetDeviceData), 10);
    MaplePatchVtPtr(&vt[13], reinterpret_cast<void*>(&Hook_DiSetCooperativeLevel), 13);
    MapleInstallLiveDiFn(acquireFn, reinterpret_cast<void*>(&Hook_DiAcquire), g_hookDiLiveAcquire, kMapleLiveDiN,
        0);
    MapleInstallLiveDiFn(stateFn, reinterpret_cast<void*>(&Hook_DiKbGetDeviceState), g_hookDiLiveState, kMapleLiveDiN,
        0x0800);
    return acquireOk && stateOk;
}

void MapleScanDiImageVtables(HMODULE mod) {
    if (!mod) return;
    if (!MapleMemCommittedReadable(mod, sizeof(IMAGE_DOS_HEADER))) return;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    if (dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))
        || static_cast<size_t>(dos->e_lfanew) > 0x100000) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(mod) + dos->e_lfanew);
    if (!MapleMemCommittedReadable(nt, sizeof(IMAGE_NT_HEADERS))) return;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    BYTE* base = reinterpret_cast<BYTE*>(mod);
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (imageSize < sizeof(IMAGE_NT_HEADERS)) return;
    WORD nsec = nt->FileHeader.NumberOfSections;
    if (nsec > 96) nsec = 96;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    if (!MapleMemCommittedReadable(sec, sizeof(IMAGE_SECTION_HEADER) * nsec)) return;
    constexpr int kMapleDiVtFindCap = 10;
    constexpr size_t kMaxSectionScan = 2 * 1024 * 1024;
    void** found[kMapleDiVtFindCap]{};
    int foundN = 0;
    for (WORD i = 0; i < nsec && foundN < kMapleDiVtFindCap; ++i) {
        const DWORD ch = sec[i].Characteristics;
        if (ch & IMAGE_SCN_MEM_EXECUTE) continue;
        if (MapleSectionSkipByName(sec[i])) continue;
        DWORD vsz = sec[i].Misc.VirtualSize;
        if (vsz == 0) vsz = sec[i].SizeOfRawData;
        if (vsz < sizeof(void*) * 27) continue;
        if (static_cast<size_t>(sec[i].VirtualAddress) >= imageSize) continue;
        BYTE* start = base + sec[i].VirtualAddress;
        size_t bytes = vsz;
        if (sec[i].VirtualAddress + bytes > imageSize) {
            bytes = imageSize - sec[i].VirtualAddress;
        }
        if (bytes > kMaxSectionScan) bytes = kMaxSectionScan;
        BYTE* end = start + bytes;
        MEMORY_BASIC_INFORMATION mbi{};
        for (BYTE* p = start; p + sizeof(void*) * 27 <= end && foundN < kMapleDiVtFindCap; ) {
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            BYTE* regionEnd = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd <= p) break;
            const DWORD prot = mbi.Protect;
            const DWORD page = prot & 0xFF;
            const bool readable = mbi.State == MEM_COMMIT
                && (prot & PAGE_GUARD) == 0
                && (page == PAGE_READONLY || page == PAGE_READWRITE || page == PAGE_WRITECOPY
                    || page == PAGE_EXECUTE_READ || page == PAGE_EXECUTE_READWRITE
                    || page == PAGE_EXECUTE_WRITECOPY);
            BYTE* scanEnd = regionEnd < end ? regionEnd : end;
            if (!readable) {
                p = regionEnd;
                continue;
            }
            BYTE* aligned = reinterpret_cast<BYTE*>(
                (reinterpret_cast<ULONG_PTR>(p) + sizeof(void*) - 1) & ~(sizeof(void*) - 1));
            for (; aligned + sizeof(void*) * 27 <= scanEnd && foundN < kMapleDiVtFindCap;
                 aligned += sizeof(void*)) {
                void** vt = reinterpret_cast<void**>(aligned);
                if (!MapleLooksLikeDeviceVtable(vt, base, imageSize)) continue;
                if (aligned >= start + sizeof(void*)) {
                    void** prev = reinterpret_cast<void**>(aligned - sizeof(void*));
                    if (MapleLooksLikeDeviceVtable(prev, base, imageSize)) continue;
                }
                bool dup = false;
                for (int s = 0; s < foundN; ++s) {
                    if (found[s] == vt) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
                found[foundN++] = vt;
            }
            p = scanEnd;
        }
    }
    g_mapleFoundVt += foundN;
    if (foundN <= 0) return;
    for (int i = 0; i < foundN; ++i) {
        MapleHookDeviceVtableSlots(found[i]);
    }
}

void* MapleDiDetourForIndex(int index) {
    if (index == 7) return reinterpret_cast<void*>(&Hook_DiAcquire);
    if (index == 9) return reinterpret_cast<void*>(&Hook_DiKbGetDeviceState);
    if (index == 10) return reinterpret_cast<void*>(&Hook_DiGetDeviceData);
    if (index == 13) return reinterpret_cast<void*>(&Hook_DiSetCooperativeLevel);
    return nullptr;
}

bool MapleIsDinputModule(HMODULE mod) {
    if (!mod) return false;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mod, path, MAX_PATH) || !path[0]) return false;
    CharLowerW(path);
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    return lstrcmpW(base, L"dinput8.dll") == 0 || lstrcmpW(base, L"dinput.dll") == 0;
}

void MaplePatchDiCachedPointers(HMODULE mod) {
    if (!mod || MapleIsDinputModule(mod)) return;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mod, path, MAX_PATH) || !path[0]) return;
    CharLowerW(path);
    if (MapleIsFakeFocusModulePath(path) || MapleIsSystemModulePath(path)) return;
    MapleIatPair pairs[24]{};
    int nPairs = 0;
    auto add = [&](void* orig, void* detour) {
        if (!orig || !detour || orig == detour || nPairs >= 24) return;
        for (int i = 0; i < nPairs; ++i) {
            if (pairs[i].original == orig) return;
        }
        pairs[nPairs].original = orig;
        pairs[nPairs].detour = detour;
        pairs[nPairs].poll = false;
        ++nPairs;
    };
    for (int i = 0; i < g_diVtPatchN; ++i) {
        add(g_diVtPatch[i].orig, MapleDiDetourForIndex(g_diVtPatch[i].index));
    }
    add(g_diKbStateOrig, reinterpret_cast<void*>(&Hook_DiKbGetDeviceState));
    add(g_diMouseStateOrig, reinterpret_cast<void*>(&Hook_DiMouseGetDeviceState));
    if (nPairs <= 0) return;
    MaplePatchWritablePointerList(mod, pairs, nPairs, 0, 32);
}

void MaplePatchDiCachedAll() {
    MaplePatchDiCachedPointers(GetModuleHandleW(nullptr));
    // 只替换 DirectInput 方法原地址（dinput .text），不是 user32 GetAsyncKeyState。
    for (int i = 0; i < g_mapleExtraModN; ++i) {
        MaplePatchDiCachedPointers(g_mapleExtraMods[i]);
    }
}

bool MapleDiHasStateHook() {
    for (int i = 0; i < g_diVtPatchN; ++i) {
        if (g_diVtPatch[i].index == 9) return true;
    }
    return g_diKbStateSlot != nullptr || g_diMouseStateSlot != nullptr;
}

bool MapleModuleBounds(HMODULE mod, BYTE** baseOut, size_t* sizeOut) {
    if (!mod || !baseOut || !sizeOut) return false;
    if (!MapleMemCommittedReadable(mod, sizeof(IMAGE_DOS_HEADER))) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))
        || static_cast<size_t>(dos->e_lfanew) > 0x100000) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(mod) + dos->e_lfanew);
    if (!MapleMemCommittedReadable(nt, sizeof(IMAGE_NT_HEADERS))) return false;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (imageSize < sizeof(IMAGE_NT_HEADERS)) return false;
    *baseOut = reinterpret_cast<BYTE*>(mod);
    *sizeOut = imageSize;
    return true;
}

#if defined(_M_IX86)
void MapleHookHeapDiDeviceVtables(HMODULE diMod) {
    BYTE* base = nullptr;
    size_t imageSize = 0;
    if (!MapleModuleBounds(diMod, &base, &imageSize)) return;
    BYTE* addr = nullptr;
    SIZE_T scanned = 0;
    constexpr SIZE_T kMaxScan = 48u * 1024u * 1024u;
    for (int regions = 0; regions < 8192 && g_diVtPatchN < kMapleDiVtCap; ++regions) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        BYTE* regionEnd = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= addr) break;
        const DWORD page = mbi.Protect & 0xFF;
        const bool heapish = mbi.State == MEM_COMMIT
            && (mbi.Protect & PAGE_GUARD) == 0
            && mbi.Type == MEM_PRIVATE
            && (page == PAGE_READWRITE || page == PAGE_WRITECOPY)
            && mbi.RegionSize >= sizeof(void*) * 2
            && mbi.RegionSize <= 8u * 1024u * 1024u;
        if (heapish && scanned < kMaxScan) {
            SIZE_T nbytes = mbi.RegionSize;
            if (scanned + nbytes > kMaxScan) nbytes = kMaxScan - scanned;
            BYTE* p = reinterpret_cast<BYTE*>(
                (reinterpret_cast<ULONG_PTR>(mbi.BaseAddress) + sizeof(void*) - 1)
                & ~(sizeof(void*) - 1));
            BYTE* end = static_cast<BYTE*>(mbi.BaseAddress) + nbytes;
            for (; p + sizeof(void*) <= end && g_diVtPatchN < kMapleDiVtCap; p += sizeof(void*)) {
                void* maybeVt = *reinterpret_cast<void**>(p);
                if (!maybeVt) continue;
                BYTE* vp = static_cast<BYTE*>(maybeVt);
                if (vp < base || vp + sizeof(void*) * 27 > base + imageSize) continue;
                void** vt = reinterpret_cast<void**>(maybeVt);
                if (!MapleLooksLikeDeviceVtable(vt, base, imageSize)) continue;
                const int before = g_diVtPatchN;
                MapleHookDeviceVtableSlots(vt);
                if (g_diVtPatchN > before) ++g_mapleHeapVt;
            }
            scanned += nbytes;
        }
        addr = regionEnd;
    }
}
#endif

void MapleHookDinputVtables() {
    HMODULE di8 = GetModuleHandleW(L"dinput8.dll");
    HMODULE di = GetModuleHandleW(L"dinput.dll");
    if (di8 || di) g_mapleDiag |= 0x0080;
    if (di8) MapleScanDiImageVtables(di8);
    if (di) MapleScanDiImageVtables(di);
    if (!MapleDiHasStateHook()) {
#if defined(_M_IX86)
        if (di8) MapleHookHeapDiDeviceVtables(di8);
        if (di) MapleHookHeapDiDeviceVtables(di);
#endif
    }
    bool state = MapleDiHasStateHook();
    bool acquire = false;
    bool data = false;
    for (int i = 0; i < g_diVtPatchN; ++i) {
        if (g_diVtPatch[i].index == 7) acquire = true;
        if (g_diVtPatch[i].index == 10) data = true;
    }
    if (state) g_mapleDiag |= 0x0020;
    if (acquire) g_mapleDiag |= 0x0100;
    if (data) g_mapleDiag |= 0x4000;
}

void MapleRestoreDiVtables() {
    MapleRestoreLiveDiHooks();
    auto restore = [](void** slot, void* orig) {
        if (!slot || !orig) return;
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
        *slot = orig;
        VirtualProtect(slot, sizeof(void*), old, &old);
    };
    for (int i = g_diVtPatchN - 1; i >= 0; --i) {
        restore(g_diVtPatch[i].slot, g_diVtPatch[i].orig);
        g_diVtPatch[i] = {};
    }
    g_diVtPatchN = 0;
    g_mapleFoundVt = 0;
    g_mapleHeapVt = 0;
    restore(g_diKbStateSlot, g_diKbStateOrig);
    restore(g_diMouseStateSlot, g_diMouseStateOrig);
    restore(g_diCreateDeviceSlot, g_diCreateDeviceOrig);
    g_diKbStateSlot = nullptr;
    g_diKbStateOrig = nullptr;
    g_diMouseStateSlot = nullptr;
    g_diMouseStateOrig = nullptr;
    g_diCreateDeviceSlot = nullptr;
    g_diCreateDeviceOrig = nullptr;
    g_realDiCreate = nullptr;
    g_realDiCreateA = nullptr;
    g_realDiCreateW = nullptr;
    g_realDiCreateEx = nullptr;
    g_diHaveMouse = false;
}

void RemoveMapleIatHooks() {
    MapleRestoreDiVtables();
    for (int i = g_mapleIatCount - 1; i >= 0; --i) {
        void** slot = g_mapleIat[i].slot;
        if (!slot) continue;
        DWORD old = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            *slot = g_mapleIat[i].original;
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
        g_mapleIat[i] = {};
    }
    g_mapleIatCount = 0;
    g_mapleInputHookCount = 0;
    g_mapleDiag = 0;
    MapleResetHookHits();
    g_mapleExtraModN = 0;
    std::memset(g_mapleExtraMods, 0, sizeof(g_mapleExtraMods));
    g_mapleRealGetProcAddress = nullptr;
    g_mapleRealGetCursorPos = nullptr;
    g_mapleRealSetCursorPos = nullptr;
    g_mapleRealClipCursor = nullptr;
    g_mapleRealShowCursor = nullptr;
    g_mapleRealGetAsyncKeyState = nullptr;
    g_mapleRealGetKeyState = nullptr;
    g_mapleRealGetKeyboardState = nullptr;
    g_mapleRealGetForegroundWindow = nullptr;
    g_mapleRealSetForegroundWindow = nullptr;
    g_mapleRealFlashWindow = nullptr;
    g_mapleRealFlashWindowEx = nullptr;
    g_mapleRealSwitchToThisWindow = nullptr;
    g_mapleRealGetActiveWindow = nullptr;
    g_mapleRealGetFocus = nullptr;
    g_mapleRealIsIconic = nullptr;
    g_mapleRealIsWindowVisible = nullptr;
    g_mapleRealPeekMessageW = nullptr;
    g_mapleRealPeekMessageA = nullptr;
    g_mapleRealGetMessageW = nullptr;
    g_mapleRealGetMessageA = nullptr;
    g_mapleRealDispatchMessageW = nullptr;
    g_mapleRealDispatchMessageA = nullptr;
    g_mapleRealTranslateMessage = nullptr;
    g_mapleRealCallWindowProcW = nullptr;
    g_mapleRealCallWindowProcA = nullptr;
    g_mapleNtUserGfw = nullptr;
    g_mapleNtUserGaks = nullptr;
    g_mapleNtUserKeyState = nullptr;
    g_mapleNtUserKbState = nullptr;
    g_mapleNtUserCursor = nullptr;
    g_mapleNtUserSetCursor = nullptr;
}

void InstallMapleIatHooks() {
    MapleResetHookHits();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    g_mapleRealGetCursorPos = reinterpret_cast<BOOL(WINAPI*)(LPPOINT)>(
        GetProcAddress(user32, "GetCursorPos"));
    g_mapleRealSetCursorPos = reinterpret_cast<BOOL(WINAPI*)(int, int)>(
        GetProcAddress(user32, "SetCursorPos"));
    g_mapleRealClipCursor = reinterpret_cast<BOOL(WINAPI*)(const RECT*)>(
        GetProcAddress(user32, "ClipCursor"));
    g_mapleRealShowCursor = reinterpret_cast<int(WINAPI*)(BOOL)>(
        GetProcAddress(user32, "ShowCursor"));
    g_mapleRealGetAsyncKeyState = reinterpret_cast<SHORT(WINAPI*)(int)>(
        GetProcAddress(user32, "GetAsyncKeyState"));
    g_mapleRealGetKeyState = reinterpret_cast<SHORT(WINAPI*)(int)>(
        GetProcAddress(user32, "GetKeyState"));
    g_mapleRealGetKeyboardState = reinterpret_cast<BOOL(WINAPI*)(PBYTE)>(
        GetProcAddress(user32, "GetKeyboardState"));
    g_mapleRealGetForegroundWindow = reinterpret_cast<HWND(WINAPI*)()>(
        GetProcAddress(user32, "GetForegroundWindow"));
    g_mapleRealSetForegroundWindow = reinterpret_cast<BOOL(WINAPI*)(HWND)>(
        GetProcAddress(user32, "SetForegroundWindow"));
    g_mapleRealFlashWindow = reinterpret_cast<BOOL(WINAPI*)(HWND, BOOL)>(
        GetProcAddress(user32, "FlashWindow"));
    g_mapleRealFlashWindowEx = reinterpret_cast<BOOL(WINAPI*)(PFLASHWINFO)>(
        GetProcAddress(user32, "FlashWindowEx"));
    g_mapleRealSwitchToThisWindow = reinterpret_cast<void(WINAPI*)(HWND, BOOL)>(
        GetProcAddress(user32, "SwitchToThisWindow"));
    g_mapleRealGetActiveWindow = reinterpret_cast<HWND(WINAPI*)()>(
        GetProcAddress(user32, "GetActiveWindow"));
    g_mapleRealGetFocus = reinterpret_cast<HWND(WINAPI*)()>(
        GetProcAddress(user32, "GetFocus"));
    g_mapleRealIsIconic = reinterpret_cast<BOOL(WINAPI*)(HWND)>(
        GetProcAddress(user32, "IsIconic"));
    g_mapleRealIsWindowVisible = reinterpret_cast<BOOL(WINAPI*)(HWND)>(
        GetProcAddress(user32, "IsWindowVisible"));
    g_mapleRealPeekMessageW = reinterpret_cast<BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT)>(
        GetProcAddress(user32, "PeekMessageW"));
    g_mapleRealPeekMessageA = reinterpret_cast<BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT)>(
        GetProcAddress(user32, "PeekMessageA"));
    g_mapleRealGetMessageW = reinterpret_cast<BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT)>(
        GetProcAddress(user32, "GetMessageW"));
    g_mapleRealGetMessageA = reinterpret_cast<BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT)>(
        GetProcAddress(user32, "GetMessageA"));
    g_mapleRealDispatchMessageW = reinterpret_cast<LRESULT(WINAPI*)(const MSG*)>(
        GetProcAddress(user32, "DispatchMessageW"));
    g_mapleRealDispatchMessageA = reinterpret_cast<LRESULT(WINAPI*)(const MSG*)>(
        GetProcAddress(user32, "DispatchMessageA"));
    g_mapleRealTranslateMessage = reinterpret_cast<BOOL(WINAPI*)(const MSG*)>(
        GetProcAddress(user32, "TranslateMessage"));
    g_mapleRealCallWindowProcW = reinterpret_cast<LRESULT(WINAPI*)(WNDPROC, HWND, UINT, WPARAM, LPARAM)>(
        GetProcAddress(user32, "CallWindowProcW"));
    g_mapleRealCallWindowProcA = reinterpret_cast<LRESULT(WINAPI*)(WNDPROC, HWND, UINT, WPARAM, LPARAM)>(
        GetProcAddress(user32, "CallWindowProcA"));
    HMODULE win32u = GetModuleHandleW(L"win32u.dll");
    if (win32u) {
        g_mapleNtUserGfw = reinterpret_cast<void*>(
            GetProcAddress(win32u, "NtUserGetForegroundWindow"));
        g_mapleNtUserGaks = reinterpret_cast<void*>(
            GetProcAddress(win32u, "NtUserGetAsyncKeyState"));
        g_mapleNtUserKeyState = reinterpret_cast<void*>(
            GetProcAddress(win32u, "NtUserGetKeyState"));
        g_mapleNtUserKbState = reinterpret_cast<void*>(
            GetProcAddress(win32u, "NtUserGetKeyboardState"));
        g_mapleNtUserCursor = reinterpret_cast<void*>(
            GetProcAddress(win32u, "NtUserGetCursorPos"));
        g_mapleNtUserSetCursor = reinterpret_cast<void*>(
            GetProcAddress(win32u, "NtUserSetCursorPos"));
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        g_mapleRealGetProcAddress = reinterpret_cast<FARPROC(WINAPI*)(HMODULE, LPCSTR)>(
            GetProcAddress(k32, "GetProcAddress"));
    }
    HMODULE di = GetModuleHandleW(L"dinput8.dll");
    if (di) {
        g_realDiCreate = reinterpret_cast<DiCreateFn>(
            GetProcAddress(di, "DirectInput8Create"));
    }
    HMODULE di7 = GetModuleHandleW(L"dinput.dll");
    if (di7) {
        g_realDiCreateA = reinterpret_cast<DiCreateAFn>(
            GetProcAddress(di7, "DirectInputCreateA"));
        g_realDiCreateW = reinterpret_cast<DiCreateAFn>(
            GetProcAddress(di7, "DirectInputCreateW"));
        g_realDiCreateEx = reinterpret_cast<DiCreateFn>(
            GetProcAddress(di7, "DirectInputCreateEx"));
    }
    MapleIatInstallFromPeb();
    // 已有键盘设备在注入前 CreateDevice：改确认过的设备 vtable 槽，并对
    // 这些表上唯一的 Acquire/GetDeviceState 方法体打 JMP（dinput .text）。
    // GetDeviceData 只改虚表槽，不打方法体。禁止 user32/win32u 方法体 JMP。
    // 禁止扫 18 方法跳转表（槽和 JMP 都会让星辰冒险岛几秒后闪退）。
    // 禁止 Poll、禁止注入线程 CreateDevice。
    MapleHookDinputVtables();
    MaplePatchDiCachedAll();
    // dinput8/dinput 常晚于注入才被游戏 LoadLibrary（日志特征：iatPoll=2 而 foundVt>0）。
    // PEB 那一轮它们还没进进程，user32 IAT 自然一个槽都没补上；这里在 DI 阶段之后再补一次，
    // 之后 iatPoll 应 ≥4（主程序 2 + 本地 dinput user32 的 GAKS/光标 2）。
    MapleIatWalkGameDirDinputUser32();
    MaplePublishHits();
    // 164352 闪退：禁止假 WM_INPUT、禁止改 dinput8 可写节、禁止注入线程
    // RegisterRawInputDevices / SetCooperativeLevel / Prime SendMessage。
}

void RemoveAllHooks() {
    RemoveMapleIatHooks();
    RemoveRawInputHooks();
    fakefocus::RemoveInlineHook(g_hookDwmAttr);
    fakefocus::RemoveInlineHook(g_hookIsVisible);
    fakefocus::RemoveInlineHook(g_hookKeyboardState);
    fakefocus::RemoveInlineHook(g_hookKeyState);
    fakefocus::RemoveInlineHook(g_hookAsyncKey);
    fakefocus::RemoveInlineHook(g_hookShowCursor);
    fakefocus::RemoveInlineHook(g_hookSetCapture);
    fakefocus::RemoveInlineHook(g_hookClipCursor);
    fakefocus::RemoveInlineHook(g_hookSetCursor);
    fakefocus::RemoveInlineHook(g_hookCursor);
    fakefocus::RemoveInlineHook(g_hookFocus);
    fakefocus::RemoveInlineHook(g_hookActive);
    fakefocus::RemoveInlineHook(g_hookSetFg);
    fakefocus::RemoveInlineHook(g_hookFg);
    if (g_focusGuardHook) {
        UnhookWindowsHookEx(g_focusGuardHook);
        g_focusGuardHook = nullptr;
    }
}

/// 软光标 + 软键态钩子（微信 Qt 与 Electron/CEF 共用）：
/// 让 QCursor::pos / GetAsyncKeyState(LBUTTON) / GetKeyboardState(修饰键) 对上脚本软输入。
/// 不装 RawInput / ClipCursor / 子类化；灌键线程由调用方决定（微信不装，Electron 装）。
bool InstallWeixinMouseStateHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    void* pCursor = reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos"));
    void* pAsync = reinterpret_cast<void*>(GetProcAddress(user32, "GetAsyncKeyState"));
    void* pKeyState = reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyState"));
    void* pKeys = reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyboardState"));
    if (!pCursor || !pAsync || !pKeyState || !pKeys) return false;
    if (!fakefocus::InstallInlineHook(g_hookCursor, pCursor, reinterpret_cast<void*>(&Hook_GetCursorPos))) {
        return false;
    }
    void* pSetCursor = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
    if (pSetCursor) {
        fakefocus::InstallInlineHook(g_hookSetCursor, pSetCursor,
            reinterpret_cast<void*>(&Hook_SetCursorPos));
    }
    auto undoCursor = [&]() {
        fakefocus::RemoveInlineHook(g_hookSetCursor);
        fakefocus::RemoveInlineHook(g_hookCursor);
    };
    if (!fakefocus::InstallInlineHook(g_hookAsyncKey, pAsync, reinterpret_cast<void*>(&Hook_GetAsyncKeyState))) {
        undoCursor();
        return false;
    }
    if (!fakefocus::InstallInlineHook(g_hookKeyState, pKeyState, reinterpret_cast<void*>(&Hook_GetKeyState))) {
        fakefocus::RemoveInlineHook(g_hookAsyncKey);
        undoCursor();
        return false;
    }
    if (!fakefocus::InstallInlineHook(g_hookKeyboardState, pKeys, reinterpret_cast<void*>(&Hook_GetKeyboardState))) {
        fakefocus::RemoveInlineHook(g_hookKeyState);
        fakefocus::RemoveInlineHook(g_hookAsyncKey);
        undoCursor();
        return false;
    }
    return true;
}

bool InstallPhase2Hooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    void* pCursor = reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos"));
    void* pAsync = reinterpret_cast<void*>(GetProcAddress(user32, "GetAsyncKeyState"));
    void* pKeyState = reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyState"));
    void* pKeys = reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyboardState"));
    void* pVisible = reinterpret_cast<void*>(GetProcAddress(user32, "IsWindowVisible"));
    if (!pAsync || !pKeyState || !pKeys || !pVisible) return false;

    // 仅桌面模拟器（类名或进程名）在 lite 下跳过 GetCursorPos；
    // UE5 lite 仍需要软光标钩，不能一并关掉。
    HWND topForCursor = g_targetTop.load(std::memory_order_relaxed);
    const bool skipCursorHook = g_liteMode
        && (g_desktopEmuSafe
            || (topForCursor && ShouldSkipGetCursorPosHook(topForCursor)));
    if (!skipCursorHook && !pCursor) return false;

    if (!skipCursorHook) {
        if (!fakefocus::InstallInlineHook(g_hookCursor, pCursor, reinterpret_cast<void*>(&Hook_GetCursorPos))) {
            return false;
        }
        void* pSetCursor = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
        void* pClip = reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor"));
        void* pCapture = reinterpret_cast<void*>(GetProcAddress(user32, "SetCapture"));
        void* pShow = reinterpret_cast<void*>(GetProcAddress(user32, "ShowCursor"));
        if (pSetCursor) {
            fakefocus::InstallInlineHook(g_hookSetCursor, pSetCursor,
                reinterpret_cast<void*>(&Hook_SetCursorPos));
        }
        if (pClip) {
            fakefocus::InstallInlineHook(g_hookClipCursor, pClip,
                reinterpret_cast<void*>(&Hook_ClipCursor));
        }
        if (pCapture) {
            fakefocus::InstallInlineHook(g_hookSetCapture, pCapture,
                reinterpret_cast<void*>(&Hook_SetCapture));
        }
        if (pShow) {
            fakefocus::InstallInlineHook(g_hookShowCursor, pShow,
                reinterpret_cast<void*>(&Hook_ShowCursor));
        }
    }
    auto undoCursor = [&]() {
        if (!skipCursorHook) {
            fakefocus::RemoveInlineHook(g_hookShowCursor);
            fakefocus::RemoveInlineHook(g_hookSetCapture);
            fakefocus::RemoveInlineHook(g_hookClipCursor);
            fakefocus::RemoveInlineHook(g_hookSetCursor);
            fakefocus::RemoveInlineHook(g_hookCursor);
        }
    };
    if (!fakefocus::InstallInlineHook(g_hookAsyncKey, pAsync, reinterpret_cast<void*>(&Hook_GetAsyncKeyState))) {
        undoCursor();
        return false;
    }
    if (!fakefocus::InstallInlineHook(g_hookKeyState, pKeyState, reinterpret_cast<void*>(&Hook_GetKeyState))) {
        fakefocus::RemoveInlineHook(g_hookAsyncKey);
        undoCursor();
        return false;
    }
    if (!fakefocus::InstallInlineHook(g_hookKeyboardState, pKeys, reinterpret_cast<void*>(&Hook_GetKeyboardState))) {
        fakefocus::RemoveInlineHook(g_hookKeyState);
        fakefocus::RemoveInlineHook(g_hookAsyncKey);
        undoCursor();
        return false;
    }
    if (!fakefocus::InstallInlineHook(g_hookIsVisible, pVisible, reinterpret_cast<void*>(&Hook_IsWindowVisible))) {
        fakefocus::RemoveInlineHook(g_hookKeyboardState);
        fakefocus::RemoveInlineHook(g_hookKeyState);
        fakefocus::RemoveInlineHook(g_hookAsyncKey);
        undoCursor();
        return false;
    }

    // lite 仍钩 GetRawInputData（不钩 PeekMessage）：GLFW 靠 WM_INPUT + GetRawInputData 读相对鼠标。
    // DeSmuME 等桌面模拟器禁用：InputTimer 高频轮询 + 假 WM_INPUT 会启动崩/长挂崩。
    // 天龙八部：找图点击靠 GetCursorPos/GetAsyncKeyState；假 WM_INPUT 易抢前台，不装 RawInput。
    if (!g_desktopEmuSafe && !g_tianlongSafe) {
        InstallRawInputHooks(user32, g_liteMode);
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

BOOL InstallCommon(HWND targetTop, bool lite) {
    if (!targetTop || !IsWindow(targetTop)) return FALSE;
    HWND top = GetAncestor(targetTop, GA_ROOT);
    if (!top) top = targetTop;

    if (g_installed.load(std::memory_order_relaxed)) {
        g_targetTop.store(top, std::memory_order_relaxed);
        HWND render = ResolveSoftInputPostHwnd(top);
        g_focusHwnd.store((render && IsWindow(render)) ? render : top, std::memory_order_relaxed);
        return TRUE;
    }

    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    const bool mapleSafe = LooksLikeMapleStoryTarget(top);
    const bool electronSafe = !mapleSafe
        && ((wcsstr(cls, L"Chrome_WidgetWin") != nullptr)
        || (wcsstr(cls, L"Chrome_RenderWidgetHostHWND") != nullptr)
        || (_wcsicmp(cls, L"CefBrowserWindow") == 0)
        || (_wcsicmp(cls, L"CefClientWindow") == 0)
        || (FindChromeRenderWidget(top) != nullptr));
    const bool airSafe = !mapleSafe && LooksLikeAdobeAirClassName(cls);
    const bool weixinSafe = !mapleSafe && !electronSafe && HwndLooksLikeWeixinClient(top);
    const bool tianlongSafe = !mapleSafe && !electronSafe && !weixinSafe
        && ClassLooksLikeTianLongBaBu(cls);
    // melonDS 顶层常为 Qt*QWindowIcon：必须靠进程名识别，不能只认类名。
    const bool desktopEmuSafe =
        (LooksLikeDesktopEmuClassName(cls) || ProcessImageLooksLikeDesktopEmu(top))
        && !airSafe && !mapleSafe && !weixinSafe && !tianlongSafe;
    g_electronSafe = electronSafe && !airSafe && !mapleSafe && !desktopEmuSafe && !weixinSafe
        && !tianlongSafe;
    g_airSafe = airSafe;
    g_weixinSafe = weixinSafe;
    g_tianlongSafe = tianlongSafe;
    g_mapleSafe = mapleSafe;
    g_desktopEmuSafe = desktopEmuSafe;
    g_liteMode = lite || electronSafe || airSafe || weixinSafe || mapleSafe
        || desktopEmuSafe || tianlongSafe;

    DWORD softPid = 0;
    GetWindowThreadProcessId(top, &softPid);
    OpenSoftInputView(softPid);

    g_targetTop.store(top, std::memory_order_relaxed);
    HWND render = ResolveSoftInputPostHwnd(top);
    g_focusHwnd.store((render && IsWindow(render)) ? render : top, std::memory_order_relaxed);

    // 冒险岛：IAT + 吞失活消息 + DirectInput 后台协作。禁止 user32/win32u 方法体 JMP。
    // 禁止 CallThroughOriginal、子类化 WndProc、假 WM_INPUT、注入线程里 CreateDevice。
    if (mapleSafe) {
        InstallMapleIatHooks();
        g_installed.store(true, std::memory_order_release);
        return TRUE;
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        CloseSoftInputView();
        ResetFocusModeFlags();
        return FALSE;
    }

    void* pFg = reinterpret_cast<void*>(GetProcAddress(user32, "GetForegroundWindow"));
    void* pSetFg = reinterpret_cast<void*>(GetProcAddress(user32, "SetForegroundWindow"));
    void* pActive = reinterpret_cast<void*>(GetProcAddress(user32, "GetActiveWindow"));
    void* pFocus = reinterpret_cast<void*>(GetProcAddress(user32, "GetFocus"));
    if (!pFg || !pSetFg || !pActive || !pFocus) {
        CloseSoftInputView();
        ResetFocusModeFlags();
        return FALSE;
    }

    if (!fakefocus::InstallInlineHook(g_hookFg, pFg, reinterpret_cast<void*>(&Hook_GetForegroundWindow))) {
        CloseSoftInputView();
        ResetFocusModeFlags();
        return FALSE;
    }
    if (!fakefocus::InstallInlineHook(g_hookSetFg, pSetFg, reinterpret_cast<void*>(&Hook_SetForegroundWindow))) {
        fakefocus::RemoveInlineHook(g_hookFg);
        CloseSoftInputView();
        ResetFocusModeFlags();
        return FALSE;
    }
    if (!fakefocus::InstallInlineHook(g_hookActive, pActive, reinterpret_cast<void*>(&Hook_GetActiveWindow))) {
        fakefocus::RemoveInlineHook(g_hookSetFg);
        fakefocus::RemoveInlineHook(g_hookFg);
        CloseSoftInputView();
        ResetFocusModeFlags();
        return FALSE;
    }
    if (!fakefocus::InstallInlineHook(g_hookFocus, pFocus, reinterpret_cast<void*>(&Hook_GetFocus))) {
        fakefocus::RemoveInlineHook(g_hookActive);
        fakefocus::RemoveInlineHook(g_hookSetFg);
        fakefocus::RemoveInlineHook(g_hookFg);
        CloseSoftInputView();
        ResetFocusModeFlags();
        return FALSE;
    }

    // Adobe AIR：只骗前景查询。子类化/光标 inline 会卡死播放器。
    if (airSafe) {
        g_installed.store(true, std::memory_order_release);
        return TRUE;
    }
    // 微信 4.x Qt：前景查询 + 软光标/键态。键鼠仍由宿主 PostMessage；
    // 禁止 Chromium 灌键线程、子类化、假 WM_INPUT、ClipCursor。
    if (weixinSafe) {
        (void)InstallWeixinMouseStateHooks();
        g_installed.store(true, std::memory_order_release);
        return TRUE;
    }
    // DeSmuME/Dolphin：前景查询 + 键态钩即可；禁止 RawInput/子类化/ClipCursor 清理。
    if (desktopEmuSafe) {
        if (!InstallPhase2Hooks()) {
            RemoveAllHooks();
            CloseSoftInputView();
            ResetFocusModeFlags();
            return FALSE;
        }
        g_installed.store(true, std::memory_order_release);
        return TRUE;
    }
    // Electron/CEF：焦点欺骗 + 灌键线程；Prime 只用 PostMessage（禁 SendMessageTimeout）。
    // ★必须同时钩软光标/键态（钩的是本进程 user32，因此 GetKeyboardState 只影响
    // 目标进程自己）：
    // ① Chromium 的按键预检 `IsKeyDown(GetKeyboardState(), modifiers)` 决定 Ctrl+V
    //    这类组合键算不算快捷键——不钩时它读到系统键态（本机 Ctrl 全抬起）→
    //    Ctrl 被丢掉，WM_KEYDOWN(V) 退化成普通字符输入，表现就是**只出 V 不粘贴**；
    // ② 点击命中判定要 GetCursorPos——不钩时目标读到本机真实光标（可能在别的窗上）
    //    → 悬停/焦点判断错位，表现就是**鼠标移到某处不动、点了没反应**。
    // 这两组钩子只读软状态、不写系统键态，也不会给 Chromium 灌假 WM_INPUT
    // （Hook_GetCursorPos 在 electronSafe 下是静默钩）。
    if (electronSafe) {
        if (!InstallWeixinMouseStateHooks()) {
            RemoveAllHooks();
            CloseSoftInputView();
            ResetFocusModeFlags();
            return FALSE;
        }
        SoftRefreshFocusMessages();
        StartSoftKeyDrainThread();
        g_installed.store(true, std::memory_order_release);
        return TRUE;
    }

    if (!InstallPhase2Hooks()) {
        RemoveAllHooks();
        CloseSoftInputView();
        ResetFocusModeFlags();
        return FALSE;
    }

    // lite 桌面模拟器：只钩 GetAsyncKeyState/前景窗，禁止子类化与 Prime（SendMessage 激活易崩）。
    if (!lite || ShouldAttachFakeFocusSubclass(top)) {
        AttachSubclass(top);
    }
    if (!lite) {
        HWND preserveFg = GetForegroundWindow();
        PrimeFakeFocusWindow(top, preserveFg);
    } else {
        // lite 会吞 ClipCursor/ShowCursor：先解开已有夹持，避免前台真光标被锁住。
        CallOriginalClipCursor(nullptr);
        ReleaseCapture();
        if (g_hookShowCursor.installed) {
            for (int i = 0; i < 8; ++i) {
                if (CallOriginalShowCursor(TRUE) >= 0) break;
            }
        }
        if (LooksLikeGlfwOrSdlClassName(cls)) {
            // 同进程 WH_GETMESSAGE 吞掉队列里的失焦；PostMessage 激活。不要 SendMessageTimeout。
            SoftRefreshFocusMessages();
            const DWORD tid = GetWindowThreadProcessId(top, nullptr);
            if (tid) {
                g_focusGuardHook = SetWindowsHookExW(
                    WH_GETMESSAGE, FocusGuardGetMsgProc, nullptr, tid);
            }
        }
    }
    if (LooksLikeQtAndroidEmulatorTop(top)) {
        StartSoftKeyDrainThread();
    }
    g_installed.store(true, std::memory_order_release);
    return TRUE;
}

}  // namespace

FAKEFOCUS_API LRESULT CALLBACK FakeFocus_HookProc(
    int nCode, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

FAKEFOCUS_API BOOL WINAPI FakeFocus_Install(HWND targetTop) {
    return InstallCommon(targetTop, false);
}

FAKEFOCUS_API BOOL WINAPI FakeFocus_InstallLite(HWND targetTop) {
    return InstallCommon(targetTop, true);
}

FAKEFOCUS_API BOOL WINAPI FakeFocus_UpdateTarget(HWND targetTop) {
    if (!g_installed.load(std::memory_order_acquire)) return FALSE;
    if (!targetTop || !IsWindow(targetTop)) return FALSE;
    HWND top = GetAncestor(targetTop, GA_ROOT);
    if (!top) top = targetTop;
    g_targetTop.store(top, std::memory_order_relaxed);
    HWND render = ResolveSoftInputPostHwnd(top);
    g_focusHwnd.store((render && IsWindow(render)) ? render : top, std::memory_order_relaxed);
    if (!g_electronSafe && !g_airSafe && !g_weixinSafe && !g_tianlongSafe && !g_mapleSafe
        && !g_desktopEmuSafe) {
        AttachSubclass(top);
    }
    if (!g_softView) {
        DWORD softPid = 0;
        GetWindowThreadProcessId(top, &softPid);
        OpenSoftInputView(softPid);
    }
    return TRUE;
}

FAKEFOCUS_API BOOL WINAPI FakeFocus_Uninstall(void) {
    if (!g_installed.load(std::memory_order_acquire)) {
        StopSoftKeyDrainThread();
        CloseSoftInputView();
        return TRUE;
    }

    StopSoftKeyDrainThread();
    DetachSubclass();
    RemoveAllHooks();
    ClipCursor(nullptr);
    ReleaseCapture();
    ResetRawInputState();
    CloseSoftInputView();
    ResetFocusModeFlags();
    g_keyEventRead = 0;
    g_mouseMoveRead = 0;
    g_lastFocusRefreshMs = 0;
    g_targetTop.store(nullptr, std::memory_order_relaxed);
    g_focusHwnd.store(nullptr, std::memory_order_relaxed);
    g_installed.store(false, std::memory_order_release);
    return TRUE;
}

FAKEFOCUS_API BOOL WINAPI FakeFocus_IsInstalled(void) {
    return g_installed.load(std::memory_order_acquire) ? TRUE : FALSE;
}

FAKEFOCUS_API BOOL WINAPI FakeFocus_HasSoftInput(void) {
    return SoftState() ? TRUE : FALSE;
}

FAKEFOCUS_API DWORD WINAPI FakeFocus_MapleIatCount(HWND) {
    const DWORD slots = static_cast<DWORD>(g_mapleInputHookCount) & 0xFFFFu;
    DWORD diag = g_mapleDiag & 0xFFFFu;
    if (SoftState()) diag |= 0x8000u;
    return (diag << 16) | slots;
}

FAKEFOCUS_API DWORD WINAPI FakeFocus_MapleHookHits(HWND) {
    const DWORD gaks = static_cast<DWORD>(g_mapleHitGaks) & 0xFFu;
    const DWORD diState = static_cast<DWORD>(g_mapleHitDiState) & 0xFFu;
    const DWORD diData = static_cast<DWORD>(g_mapleHitDiData) & 0xFFu;
    const DWORD lastCb = static_cast<DWORD>(g_mapleLastDiStateCb);
    const DWORD lastCbPacked = lastCb > 255u ? 255u : lastCb;
    return gaks | (diState << 8) | (diData << 16) | (lastCbPacked << 24);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // 仅 FreeLibrary：进程退出时 lpvReserved != 0，此时再拆钩会在 loader lock 下崩。
        FakeFocus_Uninstall();
    }
    return TRUE;
}
