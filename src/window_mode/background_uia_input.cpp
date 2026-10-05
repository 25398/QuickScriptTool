#include "background_uia_input.h"

#include "com_apartment.h"
#include "window_mode_log.h"

#include <UIAutomation.h>
#include <wrl/client.h>

#include <vector>

#ifndef LSFW_LOCK
#define LSFW_LOCK 1
#define LSFW_UNLOCK 2
#endif

namespace windowmode {

namespace {

using Microsoft::WRL::ComPtr;

/// ⚠⚠ 2026-10-05：UIA 兜底的**每条失败路径原来都是静默 `return false`**
/// ⇒ 用户报「UWP 计算器点击没反应」时，日志里**连「试过 UIA」都看不出来**
/// （只有**成功**才会打「UIA 点击 屏幕(x,y) 客户区(x,y)」）⇒ 只能靠读源码猜。
/// ⇒ 失败必须能看见；**限流**（点击可能很频繁，只在首次失败打一条）。
void LogUiaFailOnce(const wchar_t* stage, long hr, int sx, int sy) {
    static bool logged = false;
    if (logged) return;
    logged = true;
    WindowModeLogEventf(
        L"[窗口/后台窗口模式] ⚠ UIA 兜底失败于「%s」hr=0x%08lX 屏幕(%d,%d)"
        L" ⇒ 该点击会退回 PostMessage（UWP/WinUI 不响应 PostMessage ⇒ 表现为「点击没反应」）",
        stage, static_cast<unsigned long>(hr), sx, sy);
}

bool TryValuePatternSet(IUIAutomationElement* element, const std::wstring& text) {
    if (!element) return false;

    ComPtr<IUIAutomationValuePattern> pattern;
    if (FAILED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern))) || !pattern) {
        return false;
    }

    BOOL readOnly = TRUE;
    if (FAILED(pattern->get_CurrentIsReadOnly(&readOnly)) || readOnly) return false;

    BSTR current = nullptr;
    std::wstring merged = text;
    if (SUCCEEDED(pattern->get_CurrentValue(&current)) && current) {
        merged = current;
        merged += text;
    }
    if (current) SysFreeString(current);

    BSTR value = SysAllocString(merged.c_str());
    if (!value) return false;
    const HRESULT hr = pattern->SetValue(value);
    SysFreeString(value);
    return SUCCEEDED(hr);
}

bool FindFirstByControlType(IUIAutomation* uia, IUIAutomationElement* root, int controlType,
    IUIAutomationElement** out) {
    if (!uia || !root || !out) return false;
    *out = nullptr;

    VARIANT var{};
    var.vt = VT_I4;
    var.lVal = controlType;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, var, &cond)) || !cond) {
        return false;
    }
    return SUCCEEDED(root->FindFirst(TreeScope_Descendants, cond.Get(), out)) && *out;
}

}  // namespace

bool ClassLooksLikeUiaHost(const wchar_t* cls) {
    if (!cls || !cls[0]) return false;
    if (_wcsicmp(cls, L"ApplicationFrameWindow") == 0) return true;
    if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0) return true;
    if (_wcsicmp(cls, L"WinUIDesktopWin32WindowClass") == 0) return true;
    if (_wcsicmp(cls, L"Windows.UI.Input.InputSite.WindowClass") == 0) return true;
    if (wcsstr(cls, L"DesktopChildSiteBridge") != nullptr) return true;
    if (wcsstr(cls, L"Xaml_WindowedPopup") != nullptr) return true;
    return false;
}

BOOL CALLBACK FindUiaHostChildProc(HWND hwnd, LPARAM lp) {
    auto* found = reinterpret_cast<bool*>(lp);
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (ClassLooksLikeUiaHost(cls)) {
        *found = true;
        return FALSE;
    }
    return TRUE;
}

bool WindowUsesUiaClickFallback(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND top = GetAncestor(hwnd, GA_ROOT);
    if (!top) top = hwnd;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (ClassLooksLikeUiaHost(cls)) return true;
    GetClassNameW(hwnd, cls, 256);
    if (ClassLooksLikeUiaHost(cls)) return true;
    bool found = false;
    EnumChildWindows(top, FindUiaHostChildProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool SendQuickInputViaUiAutomation(HWND hwnd, const std::wstring& text) {
    if (!hwnd || !IsWindow(hwnd) || text.empty()) return false;

    // Do not CoUninitialize: VDA and other modules may already hold COM objects
    // on this thread; uninit leaves dangling pointers and crashes later.
    EnsureThreadComApartment();

    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&uia))) || !uia) {
        return false;
    }

    HWND rootHwnd = GetAncestor(hwnd, GA_ROOT);
    if (!rootHwnd) rootHwnd = hwnd;

    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(rootHwnd, &root)) || !root) {
        return false;
    }

    static const int kControlTypes[] = {
        UIA_EditControlTypeId,
        UIA_DocumentControlTypeId,
        UIA_TextControlTypeId,
    };

    bool ok = false;
    for (int controlType : kControlTypes) {
        ComPtr<IUIAutomationElement> target;
        if (!FindFirstByControlType(uia.Get(), root.Get(), controlType, target.GetAddressOf())) {
            continue;
        }
        if (TryValuePatternSet(target.Get(), text)) {
            ok = true;
            break;
        }
    }

    if (!ok) {
        ok = TryValuePatternSet(root.Get(), text);
    }

    return ok;
}

namespace {

bool UiaInvokeOrToggle(IUIAutomationElement* element) {
    if (!element) return false;
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId,
            IID_PPV_ARGS(&invoke))) && invoke) {
        return SUCCEEDED(invoke->Invoke());
    }
    ComPtr<IUIAutomationTogglePattern> toggle;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TogglePatternId,
            IID_PPV_ARGS(&toggle))) && toggle) {
        return SUCCEEDED(toggle->Toggle());
    }
    return false;
}

bool UiaSupportsInvokeOrToggle(IUIAutomationElement* element) {
    if (!element) return false;
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId,
            IID_PPV_ARGS(&invoke))) && invoke) {
        return true;
    }
    ComPtr<IUIAutomationTogglePattern> toggle;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TogglePatternId,
            IID_PPV_ARGS(&toggle))) && toggle) {
        return true;
    }
    return false;
}

}  // namespace

bool TryUiaInvokeAtScreenPointOn(HWND topLevel, int sx, int sy) {
    if (!topLevel || !IsWindow(topLevel)) return false;
    EnsureThreadComApartment();

    ComPtr<IUIAutomation> uia;
    const HRESULT hrCo = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&uia));
    if (FAILED(hrCo) || !uia) {
        LogUiaFailOnce(L"CoCreateInstance(CUIAutomation)", static_cast<long>(hrCo), sx, sy);
        return false;
    }
    ComPtr<IUIAutomationElement> root;
    const HRESULT hrRoot = uia->ElementFromHandle(topLevel, &root);
    if (FAILED(hrRoot) || !root) {
        LogUiaFailOnce(L"ElementFromHandle", static_cast<long>(hrRoot), sx, sy);
        return false;
    }

    ComPtr<IUIAutomationElementArray> arr;
    ComPtr<IUIAutomationCondition> allCond;
    if (FAILED(uia->CreateTrueCondition(&allCond)) || !allCond) {
        LogUiaFailOnce(L"CreateTrueCondition", 0, sx, sy);
        return false;
    }
    const HRESULT hrFind = root->FindAll(TreeScope_Descendants, allCond.Get(), &arr);
    int count = 0;
    if (SUCCEEDED(hrFind) && arr) arr->get_Length(&count);
    // ⚠⚠ UWP 的 UIA 树是**按需构建**的（跨进程 + 元素虚拟化）⇒ 首次 `FindAll` 常返回
    //   0 个元素，紧接着再查就有了。用户报「UWP 计算器点击没反应」时，日志显示
    //   UIA 兜底**确实被调用但失败了** —— 这是最可能的原因之一。
    //   ⇒ 短暂重试（**只在拿到 0 个时**，最多 3 次 × 60ms）；仍为空才回退
    //     `TreeScope_Children`（个别 UWP 的 Descendants 不穿透）。
    //   ⚠ 代价：最坏 +180ms，且发生在**输入线程** ⇒ 只在**首次**点击时才可能付这个代价
    //     （之后 UIA 树已就绪，一次就拿到）。
    if (count <= 0) {
        for (int attempt = 0; attempt < 3 && count <= 0; ++attempt) {
            Sleep(60);
            arr.Reset();
            if (FAILED(root->FindAll(TreeScope_Descendants, allCond.Get(), &arr)) || !arr) break;
            if (FAILED(arr->get_Length(&count))) count = 0;
        }
    }
    if (count <= 0) {
        arr.Reset();
        if (SUCCEEDED(root->FindAll(TreeScope_Children, allCond.Get(), &arr)) && arr) {
            if (FAILED(arr->get_Length(&count))) count = 0;
        }
    }
    if (count <= 0) {
        LogUiaFailOnce(L"FindAll 返回 0 个元素（重试 + Children 回退后仍为空）", 0, sx, sy);
        return false;
    }

    // 找包含该点、面积最小（最深）且支持 Invoke/Toggle 的元素。
    ComPtr<IUIAutomationElement> best;
    double bestArea = 1e18;
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        RECT rc{};
        if (FAILED(el->get_CurrentBoundingRectangle(&rc)) || rc.right <= rc.left
            || rc.bottom <= rc.top) {
            continue;
        }
        if (sx < rc.left || sx >= rc.right || sy < rc.top || sy >= rc.bottom) continue;
        const double area = static_cast<double>(rc.right - rc.left)
            * static_cast<double>(rc.bottom - rc.top);
        if (area >= bestArea) continue;
        if (!UiaSupportsInvokeOrToggle(el.Get())) continue;
        best = el;
        bestArea = area;
    }
    if (!best) {
        // ⚠⚠ 2026-10-05：用户实测 UWP 计算器失败在这一步。**只知道「没有可 Invoke 的元素」
        //   无法区分两种根因**，必须把现场打出来：
        //     ① **坐标系不一致**（DPI 缩放 / 客户区原点算错）⇒ 该点根本不在任何元素里
        //        ⇒ 「包含该点的元素数 = 0」
        //     ② **控件不走 Invoke** ⇒ 点在元素里，但那个元素没有 Invoke/Toggle 模式
        //        ⇒ 「包含该点的元素数 > 0」
        //   另外打出**离该点最近的元素**的矩形与名字 —— 若是 ①，从偏移量能直接看出差多少。
        int totalEls = 0, covering = 0, coveringInvokable = 0;
        long long bestDist = -1;
        RECT nearRc{};
        std::wstring nearName;
        for (int i = 0; i < count; ++i) {
            ComPtr<IUIAutomationElement> el;
            if (FAILED(arr->GetElement(i, &el)) || !el) continue;
            RECT rc{};
            if (FAILED(el->get_CurrentBoundingRectangle(&rc)) || rc.right <= rc.left
                || rc.bottom <= rc.top) {
                continue;
            }
            ++totalEls;
            const bool covers = (sx >= rc.left && sx < rc.right && sy >= rc.top && sy < rc.bottom);
            if (covers) {
                ++covering;
                if (UiaSupportsInvokeOrToggle(el.Get())) ++coveringInvokable;
            }
            // 到矩形中心的曼哈顿距离（只用于「最近的那个」诊断）
            const long long dx = sx - (rc.left + rc.right) / 2;
            const long long dy = sy - (rc.top + rc.bottom) / 2;
            const long long d = dx * dx + dy * dy;
            if (bestDist < 0 || d < bestDist) {
                bestDist = d;
                nearRc = rc;
                BSTR nm = nullptr;
                if (SUCCEEDED(el->get_CurrentName(&nm)) && nm) {
                    nearName.assign(nm, SysStringLen(nm));
                    SysFreeString(nm);
                }
            }
        }
        // 该点上「窗口管理器认为」是什么窗口 —— 判断点是否落在 UWP 的内容窗上
        // （期望是 `Windows.UI.Core.CoreWindow`；若是别的，说明点被遮挡或坐标偏了）
        wchar_t atCls[128]{};
        POINT atPt{sx, sy};
        HWND at = WindowFromPoint(atPt);
        if (at) GetClassNameW(at, atCls, 128);
        WindowModeLogEventf(
            L"[窗口/后台窗口模式] ⚠ UIA 元素诊断：树内矩形元素=%d 包含该点的=%d "
            L"其中可 Invoke/Toggle 的=%d | 最近元素矩形=(%ld,%ld)-(%ld,%ld) 名字=「%s」 "
            L"| 查询点屏幕(%d,%d) 该点窗口类=%s ⇒ %s",
            totalEls, covering, coveringInvokable,
            nearRc.left, nearRc.top, nearRc.right, nearRc.bottom,
            nearName.empty() ? L"(无)" : nearName.c_str(), sx, sy,
            atCls[0] ? atCls : L"(null)",
            covering == 0
                ? L"该点**不在任何元素内** ⇒ 多半是**坐标系不一致**（DPI 缩放/客户区原点）"
                : L"该点在元素内但没有 Invoke/Toggle 模式 ⇒ 该控件不走 Invoke");
        LogUiaFailOnce(
            L"该点下没有支持 Invoke/Toggle 的 UIA 元素（坐标不对？或该控件不走 UIA）",
            0, sx, sy);
        return false;
    }

    // UIA Invoke 对 UWP 会激活窗口。标准做法（AHK / 后台键鼠）：
    // WS_EX_NOACTIVATE + LockSetForegroundWindow，尽量不让系统切前台。
    struct ScopedNoActivateGuard {
        HWND hwnd = nullptr;
        LONG_PTR oldEx = 0;
        bool changed = false;
        bool locked = false;
        HWND preserveFg = nullptr;

        explicit ScopedNoActivateGuard(HWND top) {
            hwnd = top;
            if (!hwnd || !IsWindow(hwnd)) return;
            preserveFg = GetForegroundWindow();
            oldEx = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if ((oldEx & WS_EX_NOACTIVATE) == 0) {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, oldEx | WS_EX_NOACTIVATE);
                changed = true;
            }
            locked = LockSetForegroundWindow(LSFW_LOCK) != FALSE;
        }
        ~ScopedNoActivateGuard() {
            if (changed && hwnd && IsWindow(hwnd)) {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, oldEx);
            }
            if (locked) LockSetForegroundWindow(LSFW_UNLOCK);
            if (preserveFg && IsWindow(preserveFg) && hwnd && IsWindow(hwnd)
                && GetForegroundWindow() == hwnd) {
                SetForegroundWindow(preserveFg);
            }
        }
    } guard(topLevel);

    // 扫描仅检查模式支持；末尾只对「最终最佳」执行一次 Invoke。
    return UiaInvokeOrToggle(best.Get());
}

/// UWP 的**内容窗**句柄（`Windows.UI.Core.CoreWindow` / `InputSite`）。
/// ⚠ 为什么需要它：UIA 的 `ElementFromHandle(壳窗)` + `FindAll(Descendants)` 对 UWP
///   **有时拿不到内容**（内容在**另一个进程**的 CoreWindow 下）⇒ 用内容窗句柄再试
///   往往就能拿到。跨进程 `EnumChildWindows` 是内核侧枚举，不需要目标进程配合。
HWND FindUwpContentChildForUia(HWND top) {
    if (!top || !IsWindow(top)) return nullptr;
    struct Ctx { HWND found = nullptr; } ctx;
    EnumChildWindows(top, [](HWND w, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[128]{};
        GetClassNameW(w, cls, 128);
        if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0
            || _wcsicmp(cls, L"Windows.UI.Input.InputSite.WindowClass") == 0) {
            c->found = w;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

bool TryUiaInvokeAtScreenPoint(HWND topLevel, int sx, int sy) {
    if (TryUiaInvokeAtScreenPointOn(topLevel, sx, sy)) return true;
    // ⚠⚠ 2026-10-05 回退：UWP 的 UIA 内容**有时不挂在壳窗下** ——
    //   用户实测 `ApplicationFrameWindow` 的树里「该点下没有可 Invoke 的元素」。
    //   而系统已经识别出内容窗（日志里的
    //   `后台输入子窗 … class=Windows.UI.Core.CoreWindow`）⇒ 换**内容窗句柄**再试一次。
    HWND content = FindUwpContentChildForUia(topLevel);
    if (content && content != topLevel) {
        wchar_t cls[128]{};
        GetClassNameW(content, cls, 128);
        WindowModeLogEventf(
            L"[窗口/后台窗口模式] UIA 在壳窗下没找到可点的元素，改用**内容子窗**重试"
            L"（class=%s hwnd=0x%p）",
            cls[0] ? cls : L"(无类名)", reinterpret_cast<void*>(content));
        if (TryUiaInvokeAtScreenPointOn(content, sx, sy)) return true;
    }
    return false;
}

}  // namespace windowmode
