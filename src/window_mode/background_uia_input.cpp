#include "background_uia_input.h"

#include "com_apartment.h"
#include "window_mode_log.h"

#include <UIAutomation.h>
#include <oleacc.h>      // ⚠ 2026-10-06：MSAA 的 AccessibleObjectFromPoint（UIA 快路径的备选）
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

/// ⚠ 2026-10-06：**走的是快路径还是回退路径** —— 限流记一次。
/// 两条路径最终都只表现为 `UIA 点击 屏幕(x,y) 客户区(x,y)` 那一行，
/// 而「性能没改善」时第一个要回答的就是「快路径到底命中没有」。
/// ⚠ **两种路径分别限流** —— 否则首次走了快路径之后，后面偶尔回退就再也看不到，
///   而「**有时快有时慢**」正是最难查的形态。
/// ⚠ 用 `Event`（落盘）—— 用户导出的诊断里要能看到。
/// ⚠ 2026-10-06：**快路径为什么没命中** —— 记下 `ElementFromPoint` 的失败 hr（限流一次）。
/// 实测 UWP 计算器上快路径**未命中**，但「未命中」本身分不清根因：
///   · `E_ACCESSDENIED`(0x80070005) / `UIA_E_ELEMENTNOTAVAILABLE`(0x80040201)
///     ⇒ **AppContainer 拦截**（业界同类工具 pywinauto 就是栽在这条：
///       「依赖 ElementFromPoint()，在 AppContainer 进程中因 UIAccess=FALSE 被系统拦截」）
///   · `E_INVALIDARG` / `E_FAIL` ⇒ 参数或实现问题
///   · `S_OK` 但元素为空 ⇒ 该点确实没有元素
/// ⇒ 三者修法完全不同（拦截要换 API，参数问题要改调用）⇒ **必须能区分**。
void LogUiaHitOnce(HRESULT hr) {
    static bool logged = false;
    if (logged) return;
    logged = true;
    const wchar_t* guess = L"未知";
    if (hr == static_cast<HRESULT>(0x80070005L)) guess = L"E_ACCESSDENIED ⇒ 像是被 AppContainer 拦截";
    else if (hr == UIA_E_ELEMENTNOTAVAILABLE) guess = L"UIA_E_ELEMENTNOTAVAILABLE ⇒ 元素已移除（可重试）";
    else if (hr == static_cast<HRESULT>(0x80070057L)) guess = L"E_INVALIDARG ⇒ 参数问题";
    else if (SUCCEEDED(hr)) guess = L"S_OK 但元素为空 ⇒ 该点确实没有元素";
    WindowModeLogEventf(
        L"[窗口/后台窗口模式] UIA 快路径未命中原因：ElementFromPoint hr=0x%08lX（%s）",
        static_cast<unsigned long>(hr), guess);
}

/// ⚠⚠ 2026-10-06：**MSAA 备选定位** —— 用 `AccessibleObjectFromPoint` 拿该点的元素，
/// 再经 `ElementFromIAccessible` 转成 UIA 元素。
///
/// 为什么要有这条：实测 UWP 计算器上 **UIA 的 `ElementFromPoint` 未命中**
/// （业界同类工具 pywinauto 的 `uia` backend 就是栽在「`ElementFromPoint()` 在
/// AppContainer 进程中因 `UIAccess=FALSE` 被系统拦截」这一条）。
/// **MSAA 是一条独立于 UIA AppContainer 支持的路径**（老 API，走 OLEACC 的
/// 桥接层）⇒ 值得一试；**失败就返回 false**，不影响后面的全树遍历回退。
///
/// ⚠ `varChild` 必须 `VariantClear` —— 它可能带 BSTR/接口，不释放会泄漏。
bool TryHitTestViaMsaa(IUIAutomation* uia, int sx, int sy,
    Microsoft::WRL::ComPtr<IUIAutomationElement>& out) {
    if (!uia) return false;
    POINT pt{sx, sy};
    VARIANT varChild{};
    IAccessible* pacc = nullptr;
    const HRESULT hrAcc = AccessibleObjectFromPoint(pt, &pacc, &varChild);
    if (FAILED(hrAcc) || !pacc) {
        VariantClear(&varChild);
        return false;
    }
    Microsoft::WRL::ComPtr<IAccessible> acc;
    acc.Attach(pacc);   // 接管引用计数（AccessibleObjectFromPoint 已 AddRef）
    const long childId = (varChild.vt == VT_I4) ? varChild.lVal : 0;
    VariantClear(&varChild);
    Microsoft::WRL::ComPtr<IUIAutomationElement> el;
    const HRESULT hrEl = uia->ElementFromIAccessible(acc.Get(), childId, &el);
    if (FAILED(hrEl) || !el) return false;
    out = el;
    return true;
}

enum class UiaPath { Fast, Fallback };

/// ⚠ 2026-10-06：**MSAA 备选路径是否奏效**（限流一次）—— 用来确认
/// 「MSAA 能否绕过 UIA 在 AppContainer 上的限制」这个假设。
void LogUiaMsaaOnce(bool ok) {
    static bool logged = false;
    if (logged) return;
    logged = true;
    WindowModeLogEventf(
        ok ? L"[窗口/后台窗口模式] UIA 快路径备选：**MSAA 命中**（ElementFromPoint 被拦但 MSAA 可用）"
           : L"[窗口/后台窗口模式] UIA 快路径备选：MSAA 也没拿到 ⇒ 只能走全树遍历");
}
void LogUiaPathOnce(UiaPath p) {
    static bool fastLogged = false;
    static bool fallbackLogged = false;
    bool& slot = (p == UiaPath::Fast) ? fastLogged : fallbackLogged;
    if (slot) return;
    slot = true;
    WindowModeLogEventf(L"[窗口/后台窗口模式] UIA 路径：%s",
        p == UiaPath::Fast
            ? L"快路径 ElementFromPoint 命中（**未遍历全树**）"
            : L"快路径未命中 ⇒ 回退全树遍历（FindAllBuildCache）");
}

/// ⚠ 2026-10-06：**回退路径的分段计时**（限流打一次）。
/// 用户日志的 `[时间轴统计] … max=` 只给了**总耗时**，而回退路径有两段开销完全不同：
///   · `FindAll`（1 次跨进程调用，但要跨进程枚举整棵树）
///   · 遍历取属性（`GetElement` × N；矩形走缓存是**本地读**）
/// ⇒ 不分开测就无法知道该优化哪一段（前科：拿着总耗时猜，猜错方向白改一轮）。
/// ⚠ 只在**总耗时 ≥ 50ms** 时记 —— 正常情况（几 ms）不值得刷日志。
void LogUiaTimingOnce(DWORD findAllMs, DWORD scanMs, int elems) {
    static bool logged = false;
    if (logged) return;
    if (findAllMs + scanMs < 50) return;
    logged = true;
    WindowModeLogEventf(
        L"[窗口/后台窗口模式] UIA 回退路径耗时：FindAll=%lums 遍历=%lums（元素 %d 个）"
        L" ⇒ 慢在哪段看这两个数",
        static_cast<unsigned long>(findAllMs), static_cast<unsigned long>(scanMs), elems);
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
    // ⚠ 2026-10-06：分段计时用（见 LogUiaTimingOnce）。只用于回退路径的诊断。
    const DWORD uiaT0 = GetTickCount();
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

    // ⚠⚠⚠ 2026-10-06 **性能快路径：先试 `ElementFromPoint`**，别一上来就遍历全树。
    //
    //   用户实测（UWP 计算器）：树里有 **49 个**带矩形的元素，而每个
    //   `get_CurrentBoundingRectangle` 都是**跨进程调用** ⇒ 一次点击 **185ms**
    //   ⇒ 时间轴被拖慢 17 倍（日志：`预期=250ms 实际=4274ms`，`max=185413us`）。
    //
    //   `ElementFromPoint` **一次调用**就能拿到该点**最深**的元素，再沿**父链**向上找
    //   可 Invoke/Toggle 的（通常 3~5 层）⇒ 跨进程调用数从 ~49 降到 ~5。
    //   ⚠ 找不到（点不在 UIA 树上 / 该实现不支持）就**回退**下面的全树遍历 —— 行为不退化。
    ComPtr<IUIAutomationElement> best;
    {
        ComPtr<IUIAutomationElement> hit;
        POINT hitPt{sx, sy};
        // ⚠⚠ 2026-10-06：`ElementFromPoint` **官方文档明确说**
        //   「返回 `UIA_E_ELEMENTNOTAVAILABLE`，前提是该点下的元素在方法返回时已被移除；
        //     客户端应该优雅地处理这个错误，例如**再次尝试调用**」
        //   ⇒ 所以这里**重试一次**（UWP 的界面更新频繁，元素随时可能被重建）。
        HRESULT hrHit = E_FAIL;
        for (int attempt = 0; attempt < 2 && !hit; ++attempt) {
            hrHit = uia->ElementFromPoint(hitPt, &hit);
            if (SUCCEEDED(hrHit) && hit) break;
            hit.Reset();
            if (hrHit == UIA_E_ELEMENTNOTAVAILABLE) Sleep(20);   // 只在「元素已移除」时短暂等
            else break;                                          // 其它错误不必重试
        }
        // ⚠⚠ 2026-10-06：**记下 `ElementFromPoint` 的失败 hr** —— 这是判断
        //   「快路径为什么没命中」的唯一依据。实测（UWP 计算器）快路径**未命中**，
        //   但光知道「未命中」分不清是：
        //     · `E_ACCESSDENIED` / `UIA_E_ELEMENTNOTAVAILABLE` ⇒ AppContainer 拦截
        //       （业界同类工具 pywinauto 就是栽在这条，`UIAccess=FALSE` 被系统拦）
        //     · `E_INVALIDARG` / `E_FAIL` ⇒ 参数或实现问题
        //     · `S_OK` 但 `hit` 为空 ⇒ 该点确实没有元素
        //   ⇒ 三者的修法完全不同（拦截要换 API，参数问题要改调用）。
        if (!(SUCCEEDED(hrHit) && hit)) {
            LogUiaHitOnce(hrHit);
            // ⚠⚠ UIA 的 ElementFromPoint 没拿到 ⇒ **再试 MSAA**（独立路径，
            //   不依赖 UIA 的 AppContainer 支持）。拿到就同样沿父链找可 Invoke 的。
            Microsoft::WRL::ComPtr<IUIAutomationElement> msaaHit;
            if (TryHitTestViaMsaa(uia.Get(), sx, sy, msaaHit) && msaaHit) {
                LogUiaMsaaOnce(true);
                hit = msaaHit;
            } else {
                LogUiaMsaaOnce(false);
            }
        }
        if (SUCCEEDED(hrHit) && hit) {
            ComPtr<IUIAutomationTreeWalker> walker;
            if (SUCCEEDED(uia->get_ControlViewWalker(&walker)) && walker) {
                ComPtr<IUIAutomationElement> cur = hit;
                // ⚠ 父链设上限（16 层）：UWP 的树不深，正常 3~5 层就够；
                //   设上限是防「Walker 返回环」这类异常实现把这里挂死。
                for (int depth = 0; depth < 16 && cur; ++depth) {
                    if (UiaSupportsInvokeOrToggle(cur.Get())) { best = cur; break; }
                    ComPtr<IUIAutomationElement> parent;
                    if (FAILED(walker->GetParentElement(cur.Get(), &parent)) || !parent) break;
                    cur = parent;
                }
            }
        }
    }
    if (best) {
        // 快路径命中 ⇒ **完全不遍历全树**（这是 185ms → 几 ms 的关键）。
        // ⚠ 2026-10-06：记一行**限流的**路径标记 —— 否则跑完日志里分不清
        //   到底走了快路径还是回退路径，而「性能没改善」时这正是第一个要回答的问题。
        LogUiaPathOnce(UiaPath::Fast);
        return UiaInvokeOrToggle(best.Get());
    }
    LogUiaPathOnce(UiaPath::Fallback);

    ComPtr<IUIAutomationElementArray> arr;
    ComPtr<IUIAutomationCondition> allCond;
    if (FAILED(uia->CreateTrueCondition(&allCond)) || !allCond) {
        LogUiaFailOnce(L"CreateTrueCondition", 0, sx, sy);
        return false;
    }
    // ⚠⚠ 2026-10-06 性能：用 **`FindAllBuildCache`** 一次把 `BoundingRectangle` / `Name`
    //   **随查找批量取回**，而不是 `FindAll` 之后再逐个 `get_CurrentBoundingRectangle`
    //   —— 后者是 **N 次跨进程调用**（UWP 计算器 49 个元素 ⇒ 累计 100ms+）。
    //   缓存请求下走 `get_CachedBoundingRectangle`（**本地读**，不跨进程）。
    //   ⚠ 部分 UIA 提供程序不支持缓存请求 ⇒ **失败就回退**原来的逐个取（`useCache=false`）。
    ComPtr<IUIAutomationCacheRequest> cacheReq;
    if (SUCCEEDED(uia->CreateCacheRequest(&cacheReq)) && cacheReq) {
        cacheReq->AddProperty(UIA_BoundingRectanglePropertyId);
        cacheReq->AddProperty(UIA_NamePropertyId);
        cacheReq->put_AutomationElementMode(AutomationElementMode_Full);
    } else {
        cacheReq.Reset();
    }
    bool useCache = false;
    // 统一入口：优先 BuildCache，不支持则回退普通 FindAll。⚠ 两者都失败才返回 false。
    auto findAllInto = [&](TreeScope scope) -> bool {
        arr.Reset();
        useCache = false;
        if (cacheReq) {
            if (SUCCEEDED(root->FindAllBuildCache(scope, allCond.Get(), cacheReq.Get(), &arr))
                && arr) {
                useCache = true;
                return true;
            }
            arr.Reset();
        }
        return SUCCEEDED(root->FindAll(scope, allCond.Get(), &arr)) && arr != nullptr;
    };
    bool found = findAllInto(TreeScope_Descendants);
    int count = 0;
    if (found && FAILED(arr->get_Length(&count))) count = 0;
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
            if (!findAllInto(TreeScope_Descendants)) break;
            if (FAILED(arr->get_Length(&count))) count = 0;
        }
    }
    if (count <= 0) {
        if (findAllInto(TreeScope_Children)) {
            if (FAILED(arr->get_Length(&count))) count = 0;
        }
    }
    if (count <= 0) {
        LogUiaFailOnce(L"FindAll 返回 0 个元素（重试 + Children 回退后仍为空）", 0, sx, sy);
        return false;
    }
    const DWORD uiaT1 = GetTickCount();   // FindAll 段结束

    // 找包含该点、面积最小（最深）且支持 Invoke/Toggle 的元素。
    // ⚠ 上面 `ElementFromPoint` 快路径没命中才会走到这里（全树遍历 + 诊断）。
    double bestArea = 1e18;
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        RECT rc{};
        // ⚠ 缓存路径下 `get_CachedBoundingRectangle` 是**本地读**（属性随 FindAllBuildCache
        //   一并取回）；非缓存路径才走跨进程的 `get_CurrentBoundingRectangle`。
        const HRESULT hrRc = useCache ? el->get_CachedBoundingRectangle(&rc)
                                      : el->get_CurrentBoundingRectangle(&rc);
        if (FAILED(hrRc) || rc.right <= rc.left || rc.bottom <= rc.top) {
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
        // ⚠⚠ 2026-10-05：把**窗口几何**也打出来 —— 这是验证「客户区 → 屏幕」换算
        //   是否正确的**唯一直接证据**。用户实测反推的客户区原点是 `(1497, -171)`
        //   （y 为负 ⇒ 窗口顶部在屏幕上方之外），这不像正常摆法 ⇒ 高度怀疑换算有偏。
        RECT wr{}, cr{};
        GetWindowRect(topLevel, &wr);
        GetClientRect(topLevel, &cr);
        POINT clientOrg{0, 0};
        ClientToScreen(topLevel, &clientOrg);
        // 该点上「窗口管理器认为」是什么窗口 —— 判断点是否落在 UWP 的内容窗上
        // （期望是 `Windows.UI.Core.CoreWindow`；若是别的，说明点被遮挡或坐标偏了）
        wchar_t atCls[128]{};
        POINT atPt{sx, sy};
        HWND at = WindowFromPoint(atPt);
        if (at) GetClassNameW(at, atCls, 128);
        WindowModeLogEventf(
            L"[窗口/后台窗口模式] ⚠ UIA 元素诊断：树内矩形元素=%d 包含该点的=%d "
            L"其中可 Invoke/Toggle 的=%d | 最近元素矩形=(%ld,%ld)-(%ld,%ld) 名字=「%s」 "
            L"| 查询点屏幕(%d,%d) 该点窗口类=%s | 窗口矩形=(%ld,%ld)-(%ld,%ld) "
            L"客户区尺寸=%ldx%ld 客户区原点(屏幕)=(%ld,%ld) ⇒ %s",
            totalEls, covering, coveringInvokable,
            nearRc.left, nearRc.top, nearRc.right, nearRc.bottom,
            nearName.empty() ? L"(无)" : nearName.c_str(), sx, sy,
            atCls[0] ? atCls : L"(null)",
            wr.left, wr.top, wr.right, wr.bottom,
            cr.right - cr.left, cr.bottom - cr.top, clientOrg.x, clientOrg.y,
            covering == 0
                ? L"该点**不在任何元素内** ⇒ 多半是**坐标系不一致**（看上面窗口几何对不对）"
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
    LogUiaTimingOnce(uiaT1 - uiaT0, GetTickCount() - uiaT1, count);
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
