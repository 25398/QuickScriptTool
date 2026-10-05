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
    if (LooksLikeWeixinTarget(config ? *config : WindowModeScriptConfig{}, top)) {
        return false;
    }
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

/// 显式命中「渲染表面类名」的后代（Chrome_RenderWidgetHostHWND / SDL_app / TheRender /
/// unitygfx / applicationframewindow …）。
/// ⚠ **不含**「最大后代」启发式 —— 那条在 `FindBackgroundInputChild` 里与「包装层让位」
///   一起处理：它需要 config（是否优先文本输入）才能判对。
HWND FindKnownRenderSurfaceChild(HWND top) {
    RenderChildContext ctx{};
    EnumDescendantRenderChildren(top, &ctx);
    return ctx.bestKnown;
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

/// 「最大后代」若是**包装层**（真正的输入控件长在它里面），必须让位给真控件。
///
/// 为什么：`PostMessage` **不会**向子窗转发 ⇒ 把键投给一个只作容器的父窗等于**完全没投**。
/// 实测树（Windows 11 商店版记事本 / WinUI3，2026-09-23 本机）：
///     `Notepad`(top) └ `NotepadTextBox`(755x553) └ `RichEditD2DPT`(755x553)
/// 父子客户区**一样大**，而 `EnumChildWindows` 是「父先于子」⇒「严格大于」的最大值启发式
/// 取到包装层 `NotepadTextBox`。投 `WM_CHAR` 给它 → 文档**一个字都不进**；
/// 投给 `RichEditD2DPT` → 正常进字。用户反馈原文：
/// 「后台窗口模式按键点击不生效，按 A 打不到其他应用的后台里面」。
///
/// 判据用 `IsChild(surface, input)` 而不是「窗口里有没有输入框」：
/// 只在「真文本控件确实长在这个最大子窗**里面**」时让位 —— 窗口别处的小搜索框
/// 不该改变既有选择（那会把投递目标从主区域挪到搜索框）。
HWND TextInputInsideSurface(HWND top, HWND surface, const WindowModeScriptConfig* config) {
    if (!surface || !IsWindow(surface)) return nullptr;
    if (!PreferTextInputBinding(top, config)) return nullptr;
    HWND input = FindTextInputTarget(top);
    if (!input || input == surface) return nullptr;
    if (!IsChild(surface, input)) return nullptr;
    return input;
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

    auto finish = [&](HWND hwnd, BackgroundInputTargetKind kind,
                      const wchar_t* why = L"") -> HWND {
        if (outKind) *outKind = kind;
        if (hwnd && hwnd != top) {
            // ⚠⚠ 原来这里有**两个各自独立的 static**（`s_lastLogged` / `s_lastKind`）——
            //   而本函数是**每拍都调**的热路径（每个鼠标采样一次）。两个目标交替出现时
            //   `hwnd != s_lastLogged` 与 `kind != s_lastKind` **总有一个成立** ⇒
            //   两条日志无限对刷。实测（2026-09-30，Win11 记事本）一次 54 步的宏里
            //   刷了 50+ 行「后台输入子窗」，把调试窗挤满、拖慢回放。
            //   ⇒ 收成一份状态 + 时间节流：目标**变了**立刻报（那才是有用的事件），
            //     没变就按间隔最多每 2 秒报一次。
            static HWND s_lastHwnd = nullptr;
            static BackgroundInputTargetKind s_lastKind = BackgroundInputTargetKind::TopLevel;
            static DWORD s_lastLogTick = 0;
            const DWORD nowTick = GetTickCount();
            const bool changed = (hwnd != s_lastHwnd || kind != s_lastKind);
            if (changed || nowTick - s_lastLogTick >= 2000) {
                wchar_t cls[128]{};
                GetClassNameW(hwnd, cls, 128);
                // ★ 把**命中的判据**一起打出来（2026-09-30）：之前只有 kind+class，
                //   于是"为什么选它"（走的是哪条分支）只能靠读代码猜 —— 而这一格
                //   恰恰是"投给容器 ⇒ 键鼠/滚轮全部石沉大海"那类事故的定位点。
                WindowModeLogf(L"[窗口/后台窗口模式] 后台输入子窗 kind=%s class=%s hwnd=0x%p"
                    L" 判据=%s%s",
                    BackgroundInputTargetKindName(kind), cls, hwnd,
                    (why && *why) ? why : L"(未标注)",
                    changed ? L"" : L"（同上，2s 节流）");
                s_lastHwnd = hwnd;
                s_lastKind = kind;
                s_lastLogTick = nowTick;
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
            return finish(child, BackgroundInputTargetKind::ConfigChildClass, L"配置的子窗类名命中");
        }
    }

    if (HWND emu = FindAndroidEmulatorRenderChild(top, config)) {
        return finish(emu, BackgroundInputTargetKind::AndroidEmulatorRender, L"安卓模拟器渲染子窗");
    }

    // 微信 4.x：键鼠打到顶层 QWindow，不要落到最大子表面（与 AIR/冒险岛相同）。
    if (LooksLikeWeixinTarget(config ? *config : WindowModeScriptConfig{}, top)
        && !LooksLikeChromiumShellTarget(config ? *config : WindowModeScriptConfig{}, top)) {
        return finish(top, BackgroundInputTargetKind::TopLevel, L"微信：投顶层 QWindow");
    }

    // DeSmuME/Dolphin 等：WM_LBUTTON / 假焦点绑顶层；勿误投到工具栏或最大子窗。
    if (IsDesktopEmulatorTop(top, config)) {
        return finish(top, BackgroundInputTargetKind::TopLevel, L"桌面模拟器：投顶层");
    }

    const bool fakeFocusTarget = config && UsesFakeFocus(*config, top);
    const bool desktopEmu = config && LooksLikeEmulatorTarget(*config, top);
    if (!fakeFocusTarget && !desktopEmu) {
        if (HWND browser = FindBrowserRenderWidget(top)) {
            return finish(browser, BackgroundInputTargetKind::BrowserRenderWidget,
                L"浏览器渲染子窗");
        }
    }

    if (HWND surface = FindKnownRenderSurfaceChild(top)) {
        // ⚠⚠ 「已知渲染面」同样可能是**包装层**，而 `PostMessage` 不向子窗转发
        //   ⇒ 投给包装层等于完全没投。这条修正原先只加在下面 `ctx.largest` 那一段，
        //   这一条早退漏了 —— 两处必须同一把尺（真控件长在里面的容器要让位）。
        if (HWND input = TextInputInsideSurface(top, surface, config)) {
            return finish(input, BackgroundInputTargetKind::TextInput, L"已知渲染面⊃真文本控件");
        }
        return finish(surface, BackgroundInputTargetKind::KnownRenderSurface, L"已知渲染面");
    }

    RenderChildContext ctx{};
    EnumDescendantRenderChildren(top, &ctx);

    if (ctx.largestArea >= 320 * 240) {
        // ⚠ 「最大后代」可能是**包装层**：真控件长在它里面，而 PostMessage 不向子窗转发
        //   ⇒ 投给包装层等于完全没投（现代记事本 NotepadTextBox ⊃ RichEditD2DPT，实测）。
        if (HWND input = TextInputInsideSurface(top, ctx.largest, config)) {
            return finish(input, BackgroundInputTargetKind::TextInput, L"最大后代⊃真文本控件");
        }
        return finish(ctx.largest, BackgroundInputTargetKind::KnownRenderSurface,
            L"最大后代(≥320x240)");
    }

    if (PreferTextInputBinding(top, config)) {
        if (HWND input = FindTextInputTarget(top)) {
            return finish(input, BackgroundInputTargetKind::TextInput, L"直接找文本输入控件");
        }
    }

    if (ctx.largestArea >= 160 * 120) {
        return finish(ctx.largest, BackgroundInputTargetKind::LargestSurface,
            L"最大后代(≥160x120)");
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
