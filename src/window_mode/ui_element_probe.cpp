#include "ui_element_probe.h"

#include "com_apartment.h"

#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
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

// ── UIA 控件枚举 / 调用（桌面侧「UIA 优先定位」）─────────────────────
namespace {

bool IsInteractiveControlType(CONTROLTYPEID id) {
    switch (id) {
        case UIA_ButtonControlTypeId:
        case UIA_EditControlTypeId:
        case UIA_ComboBoxControlTypeId:
        case UIA_CheckBoxControlTypeId:
        case UIA_RadioButtonControlTypeId:
        case UIA_HyperlinkControlTypeId:
        case UIA_ListItemControlTypeId:
        case UIA_MenuItemControlTypeId:
        case UIA_TabItemControlTypeId:
        case UIA_TreeItemControlTypeId:
        case UIA_SplitButtonControlTypeId:
            return true;
        default:
            return false;
    }
}

std::wstring LowerCopyLocal(std::wstring s) {
    for (auto& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

std::wstring TrimCopy(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r'
        || s[e - 1] == L'\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

}  // namespace

std::vector<UiControlInfo> ListInteractiveUiControls(HWND hwnd, int maxCount) {
    std::vector<UiControlInfo> out;
    if (!hwnd) hwnd = GetForegroundWindow();
    if (!hwnd || !IsWindow(hwnd)) return out;
    if (maxCount <= 0) maxCount = 60;

    // 枚举根不止「前台窗口」：菜单/下拉/工具提示是独立顶层窗口，浏览器「…」菜单里的
    // 「历史记录」就不在前台主窗口的 UIA 子树里。漏掉它们会迫使模型回到像素识图，
    // 而菜单行又宽又相近，识图极易点错（实测把「设置」当成「历史记录」点开了）。
    std::vector<HWND> roots;
    roots.push_back(hwnd);
    if (HWND popup = GetLastActivePopup(hwnd); popup && popup != hwnd) {
        roots.push_back(popup);
    }
    struct PopupScan {
        std::vector<HWND>* roots = nullptr;
        HWND owner = nullptr;
    };
    PopupScan scan{ &roots, hwnd };
    EnumThreadWindows(GetWindowThreadProcessId(hwnd, nullptr),
        [](HWND w, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<PopupScan*>(lp);
            if (!s || !s->roots) return TRUE;
            if (!IsWindowVisible(w)) return TRUE;
            if (w == s->owner) return TRUE;
            if (GetWindow(w, GW_OWNER) != s->owner) return TRUE;
            for (HWND have : *s->roots) {
                if (have == w) return TRUE;
            }
            s->roots->push_back(w);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&scan));

    ComPtr<IUIAutomation> uia = CreateAutomation();
    if (!uia) return out;

    // 固定扫描上限：编号必须与 maxCount 无关，否则模型拿 30 条台账里的 id 去对
    // 80 条台账（或反过来）必然错位——实测就出现过「你给的编号 20 实际是「总览」」。
    constexpr int kScanCap = 120;
    for (HWND rootHwnd : roots) {
        if (static_cast<int>(out.size()) >= kScanCap * 2) break;
        ComPtr<IUIAutomationElement> root;
        if (FAILED(uia->ElementFromHandle(rootHwnd, &root)) || !root) continue;

        // 一次取回全部后代再本地过滤：比按类型逐个 FindAll 少几次跨进程调用
        VARIANT var{};
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        ComPtr<IUIAutomationCondition> cond;
        if (FAILED(uia->CreatePropertyCondition(UIA_IsControlElementPropertyId, var, &cond))
            || !cond) {
            continue;
        }
        ComPtr<IUIAutomationElementArray> found;
        if (FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &found)) || !found)
            continue;

        int count = 0;
        found->get_Length(&count);
        for (int i = 0; i < count && static_cast<int>(out.size()) < kScanCap * 2; ++i) {
            ComPtr<IUIAutomationElement> el;
            if (FAILED(found->GetElement(i, &el)) || !el) continue;

            CONTROLTYPEID ctype = 0;
            if (FAILED(el->get_CurrentControlType(&ctype))) continue;
            if (!IsInteractiveControlType(ctype)) continue;

            BOOL offscreen = FALSE;
            el->get_CurrentIsOffscreen(&offscreen);
            if (offscreen) continue;

            BSTR nameBstr = nullptr;
            el->get_CurrentName(&nameBstr);
            std::wstring name = TrimCopy(BstrToWide(nameBstr));
            if (name.empty() || name.size() > 80) continue;

            RECT rc{};
            if (FAILED(el->get_CurrentBoundingRectangle(&rc))) continue;
            // 空矩形没有点击价值
            if (rc.right - rc.left < 2 || rc.bottom - rc.top < 2) continue;
            // 多根合并去重：同名 + 同矩形视为同一个控件
            bool dup = false;
            for (const auto& have : out) {
                if (have.name == name && have.rect.left == rc.left
                    && have.rect.top == rc.top && have.rect.right == rc.right
                    && have.rect.bottom == rc.bottom) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            UiControlInfo info;
            info.name = std::move(name);
            info.controlType = ControlTypeLabel(ctype);
            info.controlTypeId = static_cast<int>(ctype);
            info.rect = rc;
            BOOL enabled = TRUE;
            if (SUCCEEDED(el->get_CurrentIsEnabled(&enabled))) info.enabled = enabled != FALSE;
            BSTR aid = nullptr;
            if (SUCCEEDED(el->get_CurrentAutomationId(&aid))) info.automationId = BstrToWide(aid);
            // 能力位：能不能直接 Invoke / 直接填值
            ComPtr<IUnknown> pat;
            if (SUCCEEDED(el->GetCurrentPattern(UIA_InvokePatternId, &pat)) && pat)
                info.invokable = true;
            pat.Reset();
            if (SUCCEEDED(el->GetCurrentPattern(UIA_ValuePatternId, &pat)) && pat)
                info.valuePattern = true;
            out.push_back(std::move(info));
        }
    }

    // 阅读顺序：先上后下、先左后右（与视觉扫描一致，便于「第 1 个」这类说法）
    std::stable_sort(out.begin(), out.end(),
        [](const UiControlInfo& a, const UiControlInfo& b) {
            if (a.rect.top != b.rect.top) return a.rect.top < b.rect.top;
            return a.rect.left < b.rect.left;
        });
    if (static_cast<int>(out.size()) > maxCount) out.resize(static_cast<size_t>(maxCount));
    for (size_t i = 0; i < out.size(); ++i) out[i].id = static_cast<int>(i) + 1;
    return out;
}

std::wstring FormatUiControlListForAgent(const std::vector<UiControlInfo>& items,
    size_t maxChars) {
    if (items.empty()) return {};
    std::wstring out;
    for (const auto& it : items) {
        std::wstring line = L"[" + std::to_wstring(it.id) + L"] ";
        line += it.controlType.empty() ? L"控件" : it.controlType;
        line += L" \"" + it.name + L"\"";
        if (!it.enabled) line += L"（灰）";
        if (it.valuePattern) line += L" [可填]";
        if (!it.invokable) line += L" [需点击]";
        if (it.rect.right > it.rect.left && it.rect.bottom > it.rect.top) {
            line += L" @";
            line += std::to_wstring((it.rect.left + it.rect.right) / 2);
            line += L",";
            line += std::to_wstring((it.rect.top + it.rect.bottom) / 2);
        }
        line += L"\n";
        if (out.size() + line.size() > maxChars) {
            out += L"…(截断)\n";
            break;
        }
        out += line;
    }
    return out;
}

int PickUiControlByName(const std::vector<UiControlInfo>& items, const std::wstring& name,
    bool* ambiguous) {
    if (ambiguous) *ambiguous = false;
    const std::wstring target = LowerCopyLocal(TrimCopy(name));
    if (target.empty()) return -1;

    struct Cand {
        int index;
        int score;
        bool exact;
        int tier;
        size_t nameLen;
    };
    std::vector<Cand> cands;
    for (size_t i = 0; i < items.size(); ++i) {
        const UiControlInfo& it = items[i];
        const std::wstring n = LowerCopyLocal(TrimCopy(it.name));
        if (n.empty()) continue;
        int tier = 0;
        if (n == target) tier = 4;
        else if (n.rfind(target, 0) == 0) tier = 3;
        else if (n.find(target) != std::wstring::npos) tier = 2;
        else if (target.find(n) != std::wstring::npos && n.size() >= 2) tier = 1;
        if (tier == 0) continue;
        int score = 0;
        switch (tier) {
        case 4: score = 1000; break;
        case 3: score = 700; break;
        case 2: score = 500; break;
        default: score = 400; break;
        }
        // 部分命中时名字越长越接近目标（「保存并关闭」优于「保存」）——否则会被
        // 短名抢走，比如「点击保存并关闭」选到工具条上的「保存」。
        if (tier <= 2) score += (std::min)(static_cast<int>(n.size()), 24);
        if (it.invokable) score += 120;
        if (!it.enabled) score -= 250;   // 灰控件点了也没用，别抢正确目标
        cands.push_back({static_cast<int>(i), score, tier == 4, tier, n.size()});
    }
    if (cands.empty()) return -1;
    std::stable_sort(cands.begin(), cands.end(), [&](const Cand& a, const Cand& b) {
        if (a.score != b.score) return a.score > b.score;
        return items[static_cast<size_t>(a.index)].rect.top
            < items[static_cast<size_t>(b.index)].rect.top;
    });
    const Cand& best = cands.front();
    if (best.score < 400) return -1;
    // 完全同名多项：取阅读顺序最前（列表/工具条第 1 个），不算歧义。
    // 部分命中：只有「同一匹配档 + 名字长度几乎相同」的竞争项才算歧义——
    // 名字明显更长的那个已经是更具体的匹配，不该因为多几分就判歧义。
    if (!best.exact) {
        for (size_t k = 1; k < cands.size(); ++k) {
            if (cands[k].tier != best.tier) continue;
            if (cands[k].score < best.score - 120) continue;
            const size_t d = cands[k].nameLen > best.nameLen
                ? cands[k].nameLen - best.nameLen : best.nameLen - cands[k].nameLen;
            if (d <= 1) {
                if (ambiguous) *ambiguous = true;
                break;
            }
        }
    }
    return best.index;
}

bool InvokeUiControlByName(const std::wstring& name, int expectedId,
    std::wstring& outActualName, int& outActualId, RECT& outRect, bool& outInvoked,
    std::wstring& outWarn) {
    outActualName.clear();
    outActualId = 0;
    outRect = RECT{};
    outInvoked = false;
    outWarn.clear();

    const std::vector<UiControlInfo> items = ListInteractiveUiControls(nullptr, 60);
    bool ambiguous = false;
    const int idx = PickUiControlByName(items, name, &ambiguous);
    if (idx < 0) return false;
    const UiControlInfo& hit = items[static_cast<size_t>(idx)];
    outActualName = hit.name;
    outActualId = hit.id;
    outRect = hit.rect;
    if (ambiguous) outWarn = L"名字存在近似竞争项，已按阅读顺序取最前";
    // id 与 name 不一致：以 name 为准（name 才是模型推理过的语义），但要留证
    if (expectedId > 0 && expectedId != hit.id) {
        if (!outWarn.empty()) outWarn += L"；";
        outWarn += L"你给的编号 " + std::to_wstring(expectedId) + L" 实际是「"
            + (expectedId <= static_cast<int>(items.size())
                ? items[static_cast<size_t>(expectedId) - 1].name : L"(越界)")
            + L"」，已按名字改用 " + std::to_wstring(hit.id);
    }
    if (!hit.enabled) {
        if (!outWarn.empty()) outWarn += L"；";
        outWarn += L"该控件当前是灰色不可用";
    }

    // 真的去 Invoke：上面那份列表没有带走 COM 引用，按名字重新取一次元素
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return false;
    ComPtr<IUIAutomation> uia = CreateAutomation();
    if (!uia) return false;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(fg, &root)) || !root) return false;
    VARIANT var{};
    var.vt = VT_BSTR;
    var.bstrVal = SysAllocString(hit.name.c_str());
    if (!var.bstrVal) return false;
    ComPtr<IUIAutomationCondition> cond;
    const HRESULT condHr = uia->CreatePropertyCondition(UIA_NamePropertyId, var, &cond);
    SysFreeString(var.bstrVal);
    if (FAILED(condHr) || !cond) return false;
    ComPtr<IUIAutomationElement> el;
    if (FAILED(root->FindFirst(TreeScope_Descendants, cond.Get(), &el)) || !el) return false;
    ComPtr<IUnknown> pat;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_InvokePatternId, &pat)) && pat) {
        ComPtr<IUIAutomationInvokePattern> invoke;
        if (SUCCEEDED(pat.As(&invoke)) && invoke && SUCCEEDED(invoke->Invoke())) {
            outInvoked = true;
        }
    }
    return true;
}

bool IsScreenPointOnForegroundWindow(int screenX, int screenY) {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return true;  // 判不了就别拦
    POINT pt{ screenX, screenY };
    HWND hit = WindowFromPoint(pt);
    if (!hit) return false;
    HWND hitRoot = GetAncestor(hit, GA_ROOT);
    HWND fgRoot = GetAncestor(fg, GA_ROOT);
    if (!hitRoot || !fgRoot) return true;   // 判不了就别拦
    if (hitRoot == fgRoot) return true;
    // 弹出菜单/下拉/工具提示是独立顶层窗口：同进程或 owner 链指回前台都算「属于前台」
    DWORD hitPid = 0;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(hitRoot, &hitPid);
    GetWindowThreadProcessId(fgRoot, &fgPid);
    if (hitPid != 0 && hitPid == fgPid) return true;
    for (HWND w = hitRoot; w; w = GetWindow(w, GW_OWNER)) {
        if (GetAncestor(w, GA_ROOT) == fgRoot) return true;
    }
    // UIA 命中测试交叉校验：现代文件对话框（IFileDialog）等用 DirectComposition 绘制，
    // WindowFromPoint 可能返回别的顶层窗口（实测「另存为」里的「桌面」被误判成遮挡、
    // 白白拦下一次合法点击）。UIA 的元素命中更接近「用户实际点的东西」——
    // 只要该点的 UIA 元素属于前台进程，就认定不遮挡。
    if (ComPtr<IUIAutomation> uia = CreateAutomation()) {
        ComPtr<IUIAutomationElement> el;
        if (SUCCEEDED(uia->ElementFromPoint(pt, &el)) && el) {
            int elPid = 0;
            if (SUCCEEDED(el->get_CurrentProcessId(&elPid)) && elPid != 0
                && elPid == static_cast<int>(fgPid)) {
                return true;
            }
        }
    }
    return false;
}


}  // namespace windowmode
