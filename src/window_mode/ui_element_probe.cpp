#include "ui_element_probe.h"

#include "com_apartment.h"

#include <UIAutomation.h>
#include <wrl/client.h>

#include <initializer_list>

namespace windowmode {

namespace {

using Microsoft::WRL::ComPtr;

ComPtr<IUIAutomation> CreateAutomation() {
    // Do not CoUninitialize: other modules cache COM objects on this thread.
    EnsureThreadComApartment();
    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&uia)))) {
        return nullptr;
    }
    return uia;
}

std::wstring BstrToWide(BSTR bstr) {
    std::wstring out = bstr ? bstr : L"";
    if (bstr) SysFreeString(bstr);
    return out;
}

std::wstring ControlTypeLabel(CONTROLTYPEID id) {
    switch (id) {
        case UIA_ButtonControlTypeId: return L"按钮";
        case UIA_EditControlTypeId: return L"输入框";
        case UIA_ComboBoxControlTypeId: return L"下拉框";
        case UIA_CheckBoxControlTypeId: return L"复选框";
        case UIA_RadioButtonControlTypeId: return L"单选框";
        case UIA_HyperlinkControlTypeId: return L"链接";
        case UIA_ListItemControlTypeId: return L"列表项";
        case UIA_MenuItemControlTypeId: return L"菜单项";
        case UIA_TabItemControlTypeId: return L"标签页";
        case UIA_TextControlTypeId: return L"文本";
        default: return L"";
    }
}

bool LooksLikeSubmitName(const std::wstring& name) {
    static const wchar_t* kKeys[] = {
        L"保存", L"確定", L"确定", L"提交", L"应用", L"下一步", L"完成",
        L"Save", L"OK", L"Submit", L"Apply", L"Next", L"Finish" };
    for (const wchar_t* k : kKeys) {
        if (name.find(k) != std::wstring::npos) return true;
    }
    return false;
}

bool ContainsAnyKeyword(const std::wstring& text, std::initializer_list<const wchar_t*> keys) {
    for (const wchar_t* k : keys) {
        if (text.find(k) != std::wstring::npos) return true;
    }
    return false;
}

std::wstring WindowTextOf(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    const int got = GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(got > 0 ? static_cast<size_t>(got) : 0);
    return text;
}

std::wstring ClassNameOf(HWND hwnd) {
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    return cls;
}

struct DialogScanContext {
    std::wstring buttons;
    std::wstring message;
    int buttonCount = 0;
};

BOOL CALLBACK ScanDialogChildProc(HWND child, LPARAM lp) {
    auto* ctx = reinterpret_cast<DialogScanContext*>(lp);
    if (!ctx) return FALSE;
    if (!IsWindowVisible(child)) return TRUE;

    const std::wstring cls = ClassNameOf(child);
    const std::wstring text = WindowTextOf(child);
    if (text.empty()) return TRUE;

    if (cls == L"Button") {
        // 分组框也是 Button 类，但没有可点语义
        const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
        if ((style & BS_TYPEMASK) == BS_GROUPBOX) return TRUE;
        if (ctx->buttonCount < 5) {
            if (!ctx->buttons.empty()) ctx->buttons += L"|";
            ctx->buttons += text;
            ++ctx->buttonCount;
        }
        return TRUE;
    }
    if ((cls == L"Static" || cls == L"DirectUIHWND") && ctx->message.size() < 120) {
        if (!ctx->message.empty()) ctx->message += L" ";
        ctx->message += text;
    }
    return TRUE;
}

}  // namespace

ForegroundDialogInfo ProbeOfficeInlineSavePanel() {
    ForegroundDialogInfo info;
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return info;

    ComPtr<IUIAutomation> uia = CreateAutomation();
    if (!uia) return info;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(fg, &root)) || !root) return info;

    // 迷你「保存此文件」不是 #32770，Win32 枚举看不见；用 UIA 名字探测
    bool hitTitle = false;
    bool hitMore = false;
    std::wstring titleHit;
    const wchar_t* kNames[] = {
        L"保存此文件", L"Save this file", L"更多选项", L"More options",
        L"更多选项...", L"More options...",
    };
    for (const wchar_t* nm : kNames) {
        VARIANT varName{};
        varName.vt = VT_BSTR;
        varName.bstrVal = SysAllocString(nm);
        if (!varName.bstrVal) continue;
        ComPtr<IUIAutomationCondition> cond;
        ComPtr<IUIAutomationElement> el;
        if (SUCCEEDED(uia->CreatePropertyCondition(UIA_NamePropertyId, varName, &cond))
            && cond
            && SUCCEEDED(root->FindFirst(TreeScope_Descendants, cond.Get(), &el))
            && el) {
            const std::wstring n = nm;
            if (n.find(L"保存此文件") != std::wstring::npos
                || n.find(L"Save this file") != std::wstring::npos) {
                hitTitle = true;
                titleHit = n;
            } else {
                hitMore = true;
            }
        }
        SysFreeString(varName.bstrVal);
    }
    // 仅「更多选项」不够：Excel 别处也可能有；必须命中标题才认定迷你保存框
    if (!hitTitle) return info;

    info.present = true;
    info.kind = L"saveAsMini";
    info.title = titleHit.empty() ? L"保存此文件" : titleHit;
    info.buttons = hitMore ? L"更多选项|保存|取消" : L"保存|取消";
    return info;
}

ForegroundDialogInfo ProbeForegroundDialog() {
    ForegroundDialogInfo info;
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return info;
    // 标准对话框类
    if (ClassNameOf(fg) == L"#32770") {
        info.present = true;
        info.title = WindowTextOf(fg);

        DialogScanContext ctx;
        EnumChildWindows(fg, &ScanDialogChildProc, reinterpret_cast<LPARAM>(&ctx));
        info.buttons = ctx.buttons;
        info.message = ctx.message.size() > 120 ? ctx.message.substr(0, 120) : ctx.message;

        if (ContainsAnyKeyword(info.title + L" " + info.message,
                { L"已存在", L"是否替换", L"要替换", L"替换它", L"确认另存为", L"覆盖",
                  L"already exists", L"Confirm Save As", L"Replace" })) {
            info.kind = L"confirmOverwrite";
        } else if (ContainsAnyKeyword(info.title, { L"另存为", L"保存为", L"Save As" })) {
            info.kind = L"saveAs";
        } else if (ContainsAnyKeyword(info.title, { L"打开", L"Open" })) {
            info.kind = L"openFile";
        } else {
            info.kind = L"dialog";
        }
        return info;
    }

    // Office 内嵌迷你保存（Excel「保存此文件」）：不是独立 #32770
    return ProbeOfficeInlineSavePanel();
}

std::wstring FormatForegroundDialogProbe() {
    const ForegroundDialogInfo info = ProbeForegroundDialog();
    if (!info.present) return {};
    std::wstring out = L"kind=" + info.kind + L";title=" + info.title;
    if (!info.buttons.empty()) out += L";buttons=" + info.buttons;
    if (!info.message.empty()) out += L";msg=" + info.message;
    return out;
}

UiElementState ProbeUiElementAtPoint(int screenX, int screenY) {
    UiElementState state;
    ComPtr<IUIAutomation> uia = CreateAutomation();
    if (!uia) return state;

    POINT pt{ screenX, screenY };
    ComPtr<IUIAutomationElement> el;
    if (FAILED(uia->ElementFromPoint(pt, &el)) || !el) return state;

    BOOL enabled = TRUE;
    if (FAILED(el->get_CurrentIsEnabled(&enabled))) return state;
    BOOL offscreen = FALSE;
    el->get_CurrentIsOffscreen(&offscreen);

    BSTR nameBstr = nullptr;
    el->get_CurrentName(&nameBstr);
    CONTROLTYPEID ctype = 0;
    el->get_CurrentControlType(&ctype);

    state.probed = true;
    state.enabled = enabled != FALSE;
    state.offscreen = offscreen != FALSE;
    state.name = BstrToWide(nameBstr);
    state.controlType = ControlTypeLabel(ctype);
    return state;
}

bool FindDisabledSubmitButtonInForeground(std::wstring& disabledName) {
    disabledName.clear();
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return false;

    ComPtr<IUIAutomation> uia = CreateAutomation();
    if (!uia) return false;

    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(fg, &root)) || !root) return false;

    VARIANT var{};
    var.vt = VT_I4;
    var.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, var, &cond)) || !cond)
        return false;

    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &found)) || !found) return false;

    int count = 0;
    found->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(found->GetElement(i, &el)) || !el) continue;
        BSTR nameBstr = nullptr;
        el->get_CurrentName(&nameBstr);
        const std::wstring name = BstrToWide(nameBstr);
        if (name.empty() || !LooksLikeSubmitName(name)) continue;
        BOOL enabled = TRUE;
        if (FAILED(el->get_CurrentIsEnabled(&enabled))) continue;
        if (enabled == FALSE) {
            disabledName = name;
            return true;
        }
    }
    return false;
}

}  // namespace windowmode
