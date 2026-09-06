#include "background_input_target.h"

#include "background_window_input.h"
#include "cdp/cdp_input.h"
#include "window_coords.h"
#include "window_mode_log.h"
#include "window_target.h"

#include <algorithm>
#include <string>

namespace windowmode {

namespace {

std::wstring ToLowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

bool IsInputNoiseWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return true;
    wchar_t cls[256]{};
    wchar_t title[256]{};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, title, 256);
    const std::wstring lowerCls = ToLowerCopy(cls);
    const std::wstring lowerTitle = ToLowerCopy(title);
    if (lowerCls.find(L"toolbar") != std::wstring::npos) return true;
    if (lowerCls.find(L"statusbar") != std::wstring::npos) return true;
    if (lowerCls.find(L"msctfime") != std::wstring::npos) return true;
    if (lowerTitle.find(L"msctfime") != std::wstring::npos) return true;
    if (lowerCls.find(L"so_py") != std::wstring::npos) return true;
    return false;
}

int ClientArea(HWND hwnd) {
    RECT rc{};
    if (!hwnd || !GetClientRect(hwnd, &rc)) return 0;
    return std::max(0, static_cast<int>(rc.right - rc.left))
        * std::max(0, static_cast<int>(rc.bottom - rc.top));
}

bool IsAndroidEmulatorTop(HWND top, const WindowModeScriptConfig* config) {
    if (config && LooksLikeAndroidEmulatorExecutable(config->targetExePath)) return true;
    if (config && LooksLikeAndroidEmulatorWindowTitle(config->windowName)) return true;
    top = TopLevelTargetWindow(top);
    if (!top) return false;
    wchar_t cls[256]{};
    wchar_t title[512]{};
    GetClassNameW(top, cls, 256);
    GetWindowTextW(top, title, 512);
    if (LooksLikeAndroidEmulatorWindowClass(cls)) return true;
    if (LooksLikeAndroidEmulatorWindowTitle(title)) return true;
    if (config) {
        if (LooksLikeAndroidEmulatorWindowClass(config->windowClassName)
            || LooksLikeAndroidEmulatorWindowClass(config->childWindowClassName)) {
            return true;
        }
        if (LooksLikeQtRenderWindowClass(config->windowClassName)
            || LooksLikeQtRenderWindowClass(config->childWindowClassName)) {
            return true;
        }
        if (LooksLikeAndroidEmulatorWindowTitle(config->windowName)) return true;
    }
    return LooksLikeAndroidEmulatorExecutable(QueryHwndProcessImagePath(top));
}

int ScoreRenderSurfaceClass(const wchar_t* cls) {
    if (!cls || !*cls) return 0;
    const std::wstring lower = ToLowerCopy(cls);
    static const wchar_t* kHigh[] = {
        L"therender",
        L"chrome_renderwidgethosthwnd",
        L"internet explorer_server",
        L"sdl_app",
        L"glfw",
        L"sunawtcanvas",
        L"hwndwrapper",
        L"windowsforms10.window",
        L"directuihwnd",
        L"vgcanvas",
        L"unitygfx",
        L"gfxwndclass",
        L"cefwebwindow",
        L"mozillawindowclass",
        L"applicationframewindow",
        L"opengl",
        L"render",
    };
    for (const wchar_t* pat : kHigh) {
        if (lower.find(pat) != std::wstring::npos) {
            if (lower.find(L"toolbar") != std::wstring::npos) continue;
            if (lower.find(L"intermediate d3d") != std::wstring::npos) continue;
            return 100;
        }
    }
    if (lower.find(L"d3d") != std::wstring::npos
        && lower.find(L"renderwidget") == std::wstring::npos) {
        return 0;
    }
    return 0;
}

struct RenderChildContext {
    bool androidEmulator = false;
    HWND namedTheRender = nullptr;
    HWND classRender = nullptr;
    HWND bestKnown = nullptr;
    int bestKnownScore = 0;
    HWND bestQtRender = nullptr;
    int bestQtScore = 0;
    HWND largest = nullptr;
    int largestArea = 0;
};

BOOL CALLBACK EnumRenderChildProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<RenderChildContext*>(lp);
    if (!IsWindow(hwnd) || IsInputNoiseWindow(hwnd)) return TRUE;

    wchar_t cls[256]{};
    wchar_t title[256]{};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, title, 256);
    const std::wstring lowerCls = ToLowerCopy(cls);
    const std::wstring lowerTitle = ToLowerCopy(title);

    if (lowerCls == L"therender" || lowerTitle == L"therender") {
        ctx->namedTheRender = hwnd;
    }
    if (lowerCls.find(L"render") != std::wstring::npos
        && lowerCls.find(L"toolbar") == std::wstring::npos) {
        ctx->classRender = hwnd;
    }

    const int area = ClientArea(hwnd);
    if (area > ctx->largestArea) {
        ctx->largestArea = area;
        ctx->largest = hwnd;
    }

    const int clsScore = ScoreRenderSurfaceClass(cls);
    if (clsScore > 0) {
        const int score = clsScore + area / 4096;
        if (score > ctx->bestKnownScore) {
            ctx->bestKnownScore = score;
            ctx->bestKnown = hwnd;
        }
    }

    if (ctx->androidEmulator && LooksLikeQtRenderWindowClass(cls)) {
        const int score = 95 + area / 2048;
        if (score > ctx->bestQtScore) {
            ctx->bestQtScore = score;
            ctx->bestQtRender = hwnd;
        }
    }
    return TRUE;
}

void EnumDescendantRenderChildren(HWND parent, RenderChildContext* ctx) {
    if (!parent || !ctx) return;
    EnumChildWindows(parent, [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* ctx = reinterpret_cast<RenderChildContext*>(lp);
        EnumRenderChildProc(hwnd, lp);
        EnumDescendantRenderChildren(hwnd, ctx);
        return TRUE;
    }, reinterpret_cast<LPARAM>(ctx));
}

struct DescendantClassSearch {
    const wchar_t* className = nullptr;
    HWND found = nullptr;
};

BOOL CALLBACK EnumDescendantClassProc(HWND hwnd, LPARAM lp) {
    auto* search = reinterpret_cast<DescendantClassSearch*>(lp);
    if (search->found) return TRUE;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (_wcsicmp(cls, search->className) == 0) {
        search->found = hwnd;
        return TRUE;
    }
    EnumChildWindows(hwnd, EnumDescendantClassProc, lp);
    return TRUE;
}

HWND FindDescendantByExactClass(HWND top, const wchar_t* className) {
    if (!top || !className || !*className) return nullptr;
    DescendantClassSearch search{};
    search.className = className;
    EnumChildWindows(top, EnumDescendantClassProc, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

struct QtRenderSearch {
    HWND best = nullptr;
    int bestDepth = -1;
    int bestArea = 0;
};

struct QtSearchPayload {
    QtRenderSearch* search = nullptr;
    int depth = 0;
};

void FindDeepestQtRenderChild(HWND hwnd, int depth, QtRenderSearch* search);

BOOL CALLBACK EnumQtRenderChildProc(HWND hwnd, LPARAM lp) {
    auto* payload = reinterpret_cast<QtSearchPayload*>(lp);
    FindDeepestQtRenderChild(hwnd, payload->depth + 1, payload->search);
    return TRUE;
}

void FindDeepestQtRenderChild(HWND hwnd, int depth, QtRenderSearch* search) {
    if (!hwnd || !search || !IsWindow(hwnd) || IsInputNoiseWindow(hwnd)) return;

    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    const int area = ClientArea(hwnd);
    if (LooksLikeQtRenderWindowClass(cls) && area >= 200 * 200) {
        if (depth > search->bestDepth
            || (depth == search->bestDepth && area > search->bestArea)) {
            search->bestDepth = depth;
            search->bestArea = area;
            search->best = hwnd;
        }
    }

    QtSearchPayload payload{search, depth};
    EnumChildWindows(hwnd, EnumQtRenderChildProc, reinterpret_cast<LPARAM>(&payload));
}

HWND FindAndroidEmulatorRenderChild(HWND top, const WindowModeScriptConfig* config) {
    if (!IsAndroidEmulatorTop(top, config)) return nullptr;
    RenderChildContext ctx{};
    ctx.androidEmulator = true;
    EnumDescendantRenderChildren(top, &ctx);
    if (ctx.namedTheRender) return ctx.namedTheRender;
    if (ctx.classRender) return ctx.classRender;
    QtRenderSearch qtSearch{};
    FindDeepestQtRenderChild(top, 0, &qtSearch);
    if (qtSearch.best) return qtSearch.best;
    if (ctx.bestQtRender) return ctx.bestQtRender;
    if (ctx.largestArea >= 200 * 200) return ctx.largest;
    return nullptr;
}

HWND FindKnownRenderSurfaceChild(HWND top) {
    RenderChildContext ctx{};
    EnumDescendantRenderChildren(top, &ctx);
    if (ctx.bestKnown) return ctx.bestKnown;
    if (ctx.largestArea >= 320 * 240) return ctx.largest;
    return nullptr;
}

bool IsDesktopEmulatorTop(HWND top, const WindowModeScriptConfig* config) {
    top = TopLevelTargetWindow(top);
    if (!top) return false;
    if (IsAndroidEmulatorTop(top, config)) return false;
    if (config && LooksLikeEmulatorTarget(*config, top)) return true;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (LooksLikeEmulatorWindowClass(cls)) return true;
    return LooksLikeEmulatorExecutable(QueryHwndProcessImagePath(top));
}

bool PreferTextInputBinding(HWND top, const WindowModeScriptConfig* config) {
    if (IsDesktopEmulatorTop(top, config)) return false;
    if (!config) {
        wchar_t cls[256]{};
        GetClassNameW(top, cls, 256);
        if (LooksLikeGameWindowClass(cls)) return false;
        return true;
    }
    if (LooksLikeGameWindowClass(config->windowClassName)
        || LooksLikeGameWindowClass(config->childWindowClassName)
        || LooksLikeEmulatorTarget(*config, top)
        || LooksLikeAndroidEmulatorExecutable(config->targetExePath)
        || IsAndroidEmulatorTop(top, config)) {
        return false;
    }
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (LooksLikeGameWindowClass(cls) || LooksLikeEmulatorWindowClass(cls)) return false;
    return true;
}

}  // namespace

const wchar_t* BackgroundInputTargetKindName(BackgroundInputTargetKind kind) {
    switch (kind) {
    case BackgroundInputTargetKind::ConfigChildClass: return L"configChild";
    case BackgroundInputTargetKind::AndroidEmulatorRender: return L"androidRender";
    case BackgroundInputTargetKind::BrowserRenderWidget: return L"browserRender";
    case BackgroundInputTargetKind::KnownRenderSurface: return L"renderSurface";
    case BackgroundInputTargetKind::TextInput: return L"textInput";
    case BackgroundInputTargetKind::LargestSurface: return L"largestChild";
    default: return L"topLevel";
    }
}

HWND FindBackgroundInputChild(HWND top, const WindowModeScriptConfig* config,
    BackgroundInputTargetKind* outKind) {
    top = TopLevelTargetWindow(top);
    if (!top || !IsWindow(top)) return nullptr;

    auto finish = [&](HWND hwnd, BackgroundInputTargetKind kind) -> HWND {
        if (outKind) *outKind = kind;
        if (hwnd && hwnd != top) {
            static HWND s_lastLogged = nullptr;
            static BackgroundInputTargetKind s_lastKind = BackgroundInputTargetKind::TopLevel;
            if (hwnd != s_lastLogged || kind != s_lastKind) {
                wchar_t cls[128]{};
                GetClassNameW(hwnd, cls, 128);
                WindowModeLogf(L"[窗口模式] 后台输入子窗 kind=%s class=%s hwnd=0x%p",
                    BackgroundInputTargetKindName(kind), cls, hwnd);
                s_lastLogged = hwnd;
                s_lastKind = kind;
            }
        }
        return hwnd ? hwnd : top;
    };

    if (config && !config->childWindowClassName.empty()) {
        const bool qtChild = LooksLikeQtRenderWindowClass(config->childWindowClassName);
        const bool androidEmu = IsAndroidEmulatorTop(top, config);
        if (qtChild && androidEmu) {
            QtRenderSearch qt{};
            FindDeepestQtRenderChild(top, 0, &qt);
            if (qt.best) {
                return finish(qt.best, BackgroundInputTargetKind::AndroidEmulatorRender);
            }
        } else if (HWND child = FindChildWindowByClass(top, config->childWindowClassName)) {
            return finish(child, BackgroundInputTargetKind::ConfigChildClass);
        }
    }

    if (HWND emu = FindAndroidEmulatorRenderChild(top, config)) {
        return finish(emu, BackgroundInputTargetKind::AndroidEmulatorRender);
    }

    // DeSmuME/Dolphin 等：WM_LBUTTON / 假焦点绑顶层；勿误投到工具栏或最大子窗。
    if (IsDesktopEmulatorTop(top, config)) {
        return finish(top, BackgroundInputTargetKind::TopLevel);
    }

    const bool fakeFocusTarget = config && UsesFakeFocus(*config);
    const bool desktopEmu = config && LooksLikeEmulatorTarget(*config, top);
    if (!fakeFocusTarget && !desktopEmu) {
        if (HWND browser = FindBrowserRenderWidget(top)) {
            return finish(browser, BackgroundInputTargetKind::BrowserRenderWidget);
        }
    }

    if (HWND surface = FindKnownRenderSurfaceChild(top)) {
        return finish(surface, BackgroundInputTargetKind::KnownRenderSurface);
    }

    if (PreferTextInputBinding(top, config)) {
        if (HWND input = FindTextInputTarget(top)) {
            return finish(input, BackgroundInputTargetKind::TextInput);
        }
    }

    RenderChildContext ctx{};
    EnumDescendantRenderChildren(top, &ctx);
    if (ctx.largestArea >= 160 * 120) {
        return finish(ctx.largest, BackgroundInputTargetKind::LargestSurface);
    }

    if (outKind) *outKind = BackgroundInputTargetKind::TopLevel;
    return top;
}

bool IsDesktopEmulatorTarget(HWND hwnd, const WindowModeScriptConfig* config) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    return IsDesktopEmulatorTop(hwnd, config);
}

bool AndroidEmulatorPrefersFakeFocus(HWND top, const WindowModeScriptConfig* config) {
    (void)top;
    (void)config;
    // MuMu 等 Qt 壳：仅 PostMessage 到渲染子窗（OpenGL）；不注入假焦点 DLL。
    return false;
}

bool AndroidEmulatorPrefersFakeFocusFromConfig(const WindowModeScriptConfig& config) {
    (void)config;
    return false;
}

bool IsAndroidEmulatorTarget(HWND hwnd, const WindowModeScriptConfig* config) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    return IsAndroidEmulatorTop(hwnd, config);
}

bool AndroidEmulatorNeedsHardwareInput(HWND hwnd, const WindowModeScriptConfig* config) {
    (void)hwnd;
    (void)config;
    // 后台模式禁止假前台 SendInput（会屏外停放、占用焦点、坐标错乱）。
    return false;
}

bool MapClientPointBetweenHwnds(HWND fromHwnd, HWND toHwnd, int& cx, int& cy) {
    if (!fromHwnd || !toHwnd || fromHwnd == toHwnd) return true;
    int sx = 0;
    int sy = 0;
    if (!ClientToScreenPoint(fromHwnd, cx, cy, sx, sy)) return false;
    if (!ScreenToClientPoint(toHwnd, sx, sy, cx, cy)) return false;
    return true;
}

void ClampToClientRect(HWND hwnd, int& cx, int& cy) {
    if (!hwnd || !IsWindow(hwnd)) return;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return;
    const int maxX = std::max(0, static_cast<int>(rc.right - rc.left) - 1);
    const int maxY = std::max(0, static_cast<int>(rc.bottom - rc.top) - 1);
    cx = std::clamp(cx, 0, maxX);
    cy = std::clamp(cy, 0, maxY);
}

}  // namespace windowmode
