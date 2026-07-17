#include "background_uia_input.h"

#include "com_apartment.h"

#include <UIAutomation.h>
#include <wrl/client.h>

#include <vector>

namespace windowmode {

namespace {

using Microsoft::WRL::ComPtr;

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

}  // namespace windowmode
