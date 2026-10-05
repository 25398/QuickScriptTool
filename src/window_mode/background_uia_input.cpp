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

bool TryUiaInvokeAtScreenPoint(HWND topLevel, int sx, int sy) {
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
    if (FAILED(hrFind) || !arr) {
        LogUiaFailOnce(L"FindAll(Descendants)", static_cast<long>(hrFind), sx, sy);
        return false;
    }
    int count = 0;
    if (FAILED(arr->get_Length(&count)) || count <= 0) {
        LogUiaFailOnce(L"FindAll 返回 0 个元素（UWP 的 UIA 树为空？）", 0, sx, sy);
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

}  // namespace windowmode
