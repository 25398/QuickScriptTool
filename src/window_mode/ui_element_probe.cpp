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
    int n = 0;
    const UiControlTypeRow* rows = UiControlTypeTable(&n);
    for (int i = 0; i < n; ++i) {
        if (rows[i].controlTypeId == static_cast<int>(id)) return rows[i].label;
    }
    return L"";
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

/// 该元素是不是「窗口自身标题栏上的按钮」（关闭/最小化/最大化）。
///
/// 判据（不靠按钮名字，名字是本地化的、且和应用内按钮完全重名）：
/// 沿父链上溯，只要祖先里出现 **TitleBar** 控件类型，或**直接**挂在一个
/// ControlType=Window 的元素下（UIA 里窗口自己的按钮就是标题栏的直接子节点，
/// 而应用自己画的「关闭」按钮在内容树里，父链上是 Pane/Group 之类）——
/// 就认定为窗口自身按钮。
///
/// ⚠ 这条守卫是真实事故换来的：模型想关游戏内的卡牌面板，UIA 命中窗口自己的
///   「关闭」按钮（`关闭 [按钮 id=3] InvokePattern`），一击把整个游戏窗口关掉。
/// ★★ shell 图标宿主类名判据（纯函数；实现放在这里，声明在头文件，供自检逐格断言）。
/// 只看结构不看名字：`SHELLDLL_DefView` 是 Windows 外壳视图的宿主 —— 桌面图标
/// （Progman/WorkerW 下）与资源管理器文件列表（CabinetWClass 下）的项父链上都经过它，
/// 而别的应用里的普通 ListView 不会经过。
bool ClassNameIsShellIconHost(const wchar_t* className) {
    if (!className || !*className) return false;
    return _wcsicmp(className, L"SHELLDLL_DefView") == 0;
}

/// 元素（或祖先，≤8 层）落在 shell 视图里 ⇒ 该目标的"打开"= **双击**。
/// ⚠ 为什么必须结构判据：UIA 对桌面图标/文件项给的是 ListItem + InvokePattern，
///   而 shell 的 Invoke **只做选中**（实测：回执说"已触发"，浏览器却没起来，
///   见 2026-09-29 用户日志与 `UiControlInfo::shellIconItem` 的说明）。
bool IsShellIconItem(IUIAutomationElement* el) {
    if (!el) return false;
    ComPtr<IUIAutomation> uia = CreateAutomation();
    if (!uia) return false;
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(uia->get_ControlViewWalker(&walker)) || !walker) return false;
    ComPtr<IUIAutomationElement> cur = el;
    for (int depth = 0; depth < 8 && cur; ++depth) {
        BSTR cls = nullptr;
        if (SUCCEEDED(cur->get_CurrentClassName(&cls)) && cls) {
            const bool hit = ClassNameIsShellIconHost(cls);
            SysFreeString(cls);
            if (hit) return true;
        }
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(cur.Get(), &parent)) || !parent) return false;
        cur = parent;
    }
    return false;
}

bool IsWindowChromeButton(IUIAutomationElement* el) {
    if (!el) return false;
    ComPtr<IUIAutomation> uia = CreateAutomation();
    if (!uia) return false;
    ComPtr<IUIAutomationTreeWalker> walker;
    // 用 ControlViewWalker：与枚举时的 IsControlElement 过滤口径一致
    if (FAILED(uia->get_ControlViewWalker(&walker)) || !walker) return false;

    ComPtr<IUIAutomationElement> cur = el;
    for (int depth = 0; depth < 4 && cur; ++depth) {
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(cur.Get(), &parent)) || !parent) return false;
        CONTROLTYPEID ptype = 0;
        if (FAILED(parent->get_CurrentControlType(&ptype))) return false;
        if (ptype == UIA_TitleBarControlTypeId) return true;
        // 直接挂在窗口元素下 = 非客户区按钮（有的窗口标题栏没有独立 TitleBar 元素）
        if (ptype == UIA_WindowControlTypeId) return depth == 0;
        cur = parent;
    }
    return false;
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
    // 该点是否落在**窗口自身**的标题栏按钮上（名字和应用内按钮完全重名，必须靠父链判）
    state.titleBarControl = IsWindowChromeButton(el.Get());
    state.shellIconItem = IsShellIconItem(el.Get());
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
    int n = 0;
    const UiControlTypeRow* rows = UiControlTypeTable(&n);
    for (int i = 0; i < n; ++i) {
        if (rows[i].controlTypeId == static_cast<int>(id)) return true;
    }
    return false;
}

/// 这几类**必须**额外通过「可聚焦」闸才收（理由见 `UiControlTypeTable` 里那几行的注释）。
bool NeedsFocusableGate(CONTROLTYPEID id) {
    int n = 0;
    const UiControlTypeRow* rows = UiControlTypeTable(&n);
    for (int i = 0; i < n; ++i) {
        if (rows[i].controlTypeId == static_cast<int>(id)) return rows[i].needsFocusable;
    }
    return false;
}

/// 数值 → 简短可读串（%g 语义：去掉无意义的尾零，超出精度自动转科学计数）。
/// 为什么要它：滑块/进度的量级跨得很远（音量 0~100、缩放 0~500、视频进度 0~1e7），
/// 用固定小数位打印必然要么全是 `0.000000` 要么拖出一串长数字把模型带偏。
std::wstring FormatNumShort(double v) {
    wchar_t buf[40]{};
    swprintf_s(buf, L"%.4g", v);
    return buf;
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

/// 窗口自身的控制按钮名字（各地语言都要覆盖：UIA 给的是本地化字符串）。
/// ⚠ 只在**已经确认元素属于标题栏**时才用名字做二次判定，绝不单凭名字拦点击 ——
///   应用里的「关闭」按钮同样叫这个名字，靠名字拦会误伤正常操作。
bool LooksLikeWindowControlName(const std::wstring& nameLower) {
    static const wchar_t* kNames[] = {
        L"关闭", L"最小化", L"最大化", L"还原", L"close", L"minimize", L"maximize", L"restore",
    };
    for (const wchar_t* n : kNames) {
        if (nameLower == n) return true;
    }
    return false;
}

}  // namespace

// ── 在册控件类型表：**单一事实来源**（角色 / 动作能力 / 是否需可聚焦闸）────────
// 为什么要一张表而不是三处 switch：类型清单、中文角色、动作动词、可聚焦闸
// 是三份必须同步的事实，分散写必然漂移（加一类只改了两处 ⇒ 那一类静默半残）。
// ⚠ 自检遍历的就是这张表本身（`UiControlTypeTable`），不另抄常量。
const UiControlTypeRow* UiControlTypeTable(int* outCount) {
    static const UiControlTypeRow kRows[] = {
        // 触发类：动作是「点」。
        { UIA_ButtonControlTypeId,      L"按钮",       L"click",  false },
        { UIA_SplitButtonControlTypeId, L"拆分按钮",   L"click",  false },
        { UIA_HyperlinkControlTypeId,   L"链接",       L"click",  false },
        { UIA_HeaderItemControlTypeId,  L"列头",       L"click",  false },
        { UIA_MenuBarControlTypeId,     L"菜单栏",     L"click",  false },
        { UIA_TabControlTypeId,         L"标签栏",     L"click",  false },
        // 文本类：能力是「填值」，点它只是为了聚焦。
        { UIA_EditControlTypeId,        L"输入框",     L"fill",   false },
        { UIA_DocumentControlTypeId,    L"文档区",     L"click",  true  },
        // 开关类：能力是「切换」（点两次等于没做，而模型看不出来）。
        { UIA_CheckBoxControlTypeId,    L"复选框",     L"toggle", false },
        // 选择类：从一个集合里选一个。
        { UIA_ComboBoxControlTypeId,    L"下拉框",     L"select", false },
        { UIA_RadioButtonControlTypeId, L"单选框",     L"select", false },
        { UIA_ListItemControlTypeId,    L"列表项",     L"select", false },
        { UIA_TreeItemControlTypeId,    L"树节点",     L"select", false },
        { UIA_DataItemControlTypeId,    L"数据项",     L"select", false },
        { UIA_MenuItemControlTypeId,    L"菜单项",     L"select", false },
        { UIA_TabItemControlTypeId,     L"标签页",     L"select", false },
        // 连续量：给的是「值」不是「点」。
        { UIA_SliderControlTypeId,      L"滑块",       L"slide",  false },
        { UIA_SpinnerControlTypeId,     L"数值调节",   L"slide",  false },
        { UIA_ScrollBarControlTypeId,   L"滚动条",     L"scroll", false },
        // ⚠ 容器类：**只有可聚焦**才收。它们是「能打字的地方」（编辑器正文/画布），
        //   但绝大多数同名同类的容器只是布局壳子 —— 全收进来会把条数预算挤满、
        //   把真按钮挤出清单（这比漏收更糟）。needsFocusable=true 即这道闸。
        { UIA_GroupControlTypeId,       L"分组",       L"focus",  true  },
        { UIA_CustomControlTypeId,      L"自定义控件", L"focus",  true  },
    };
    if (outCount) *outCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
    return kRows;
}

// ── 控件类型 → 动作能力动词（纯函数，自检可直接逐格断言）──────────────────
const wchar_t* UiActionVerbForControl(int controlTypeId) {
    int n = 0;
    const UiControlTypeRow* rows = UiControlTypeTable(&n);
    for (int i = 0; i < n; ++i) {
        if (rows[i].controlTypeId == controlTypeId) return rows[i].action;
    }
    // ⚠ 不在册的类型**不返回 click**：点一个说不清是什么的东西会打错目标。
    //   「可聚焦」是任何可交互控件的共同下限，不构成误导性的能力承诺。
    return L"focus";
}

std::vector<UiControlInfo> ListInteractiveUiControls(HWND hwnd, int maxCount,
    int* offscreenSkipped) {
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
            if (offscreen) {
                // ★ 丢掉是对的（offscreen 条目没有可信坐标，点了会打到别处），但**必须计数**：
                //   滚动列表（历史记录/书签/文件列表）里没进视口的条目走的都是这条，
                //   以前静默丢弃 ⇒ 模型看到「47 条」以为就这么多，于是转去 zoom 读像素。
                //   把「还有 N 条在视口外」带回去，模型就知道该先 scrollWheel。
                if (offscreenSkipped) ++*offscreenSkipped;
                continue;
            }

            // ★可聚焦：读键盘可聚焦性（读失败按「不可聚焦」处理 —— 只有下面那几类
            //   容器/文档控件依赖它做闸，读失败时宁可漏收，也不要把布局壳子灌进清单）。
            BOOL focusable = FALSE;
            const bool gotFocusable =
                SUCCEEDED(el->get_CurrentIsKeyboardFocusable(&focusable));
            const bool isFocusable = gotFocusable && focusable != FALSE;
            // 文档区/分组/自定义控件**只有可聚焦**才算可交互（见 IsInteractiveControlType 末段）。
            if (NeedsFocusableGate(ctype) && !isFocusable) continue;

            BSTR nameBstr = nullptr;
            el->get_CurrentName(&nameBstr);
            std::wstring name = TrimCopy(BstrToWide(nameBstr));
            if (name.empty()) continue;
            // ★ 超长名字**截断保留**，不是整条丢弃 —— 以前 `name.size() > 80` 直接 continue，
            //   结果恰恰把「浏览器历史记录」这种长标题条目全滤掉了（那些正是模型要的）。
            //   截断后仍保留前缀，足以辨认与匹配；真要全名可用 automationId / 滚动后重列。
            if (name.size() > 80) name = name.substr(0, 79) + L"…";

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
            info.action = UiActionVerbForControl(static_cast<int>(ctype));
            info.rect = rc;
            // ★窗口自身的标题栏按钮（关/最小化/最大化）：仍然列出来（模型有知情权），
            //   但**按名字选中它时会被拒** —— 它的名字和应用内按钮完全重名，
            //   实测就是这样把整个游戏窗口关掉的。见 UiControlInfo::titleBarControl。
            info.titleBarControl = IsWindowChromeButton(el.Get());
        // ★ 桌面/资源管理器图标：shell 的 Invoke 只"选中"，"打开"要双击（见头文件说明）
        info.shellIconItem = IsShellIconItem(el.Get());
            BOOL enabled = TRUE;
            if (SUCCEEDED(el->get_CurrentIsEnabled(&enabled))) info.enabled = enabled != FALSE;
            BSTR aid = nullptr;
            if (SUCCEEDED(el->get_CurrentAutomationId(&aid))) info.automationId = BstrToWide(aid);
            // ★可聚焦：读键盘可聚焦性（失败按「不可聚焦」处理 —— 位置在下面按类型分流）。
            info.focusable = isFocusable;
            // ★★密码框：**必须在读 ValuePattern 之前**定下来 —— 它的值一律不回传
            //   （UIA 在部分应用里会把密码明文交出来，照抄就进了 API 请求）。
            BOOL isPwd = FALSE;
            if (SUCCEEDED(el->get_CurrentIsPassword(&isPwd))) info.password = isPwd != FALSE;
            // 能力位：能不能直接 Invoke / 直接填值
            ComPtr<IUnknown> pat;
            if (SUCCEEDED(el->GetCurrentPattern(UIA_InvokePatternId, &pat)) && pat)
                info.invokable = true;
            pat.Reset();
            if (SUCCEEDED(el->GetCurrentPattern(UIA_ValuePatternId, &pat)) && pat)
                info.valuePattern = true;
            // ★★可读状态事实（见 UiControlInfo::state 的理由）：这些都是**画面上读不出来
            //   或很容易读错**的东西，而它们恰恰决定下一步该不该动手、动哪一步。
            pat.Reset();
            ComPtr<IUIAutomationValuePattern> vp;
            const bool hasVp = SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId,
                IID_PPV_ARGS(&vp))) && vp;
            if (hasVp) {
                BOOL ro = FALSE;
                if (SUCCEEDED(vp->get_CurrentIsReadOnly(&ro)) && ro) info.state.push_back(L"readonly");
                BSTR val = nullptr;
                if (SUCCEEDED(vp->get_CurrentValue(&val))) {
                    std::wstring v = TrimCopy(BstrToWide(val));
                    // ⚠⚠ 密码框的值**绝不能回传**：UIA 在部分应用里会把明文给出来
                    //    （不是所有都返回圆点）—— 那等于把用户的密码写进 API 请求。
                    //    这一类只报「它是密码框」这个事实，值一个字都不给。
                    if (info.password) {
                        info.state.push_back(L"password(值不回传)");
                    } else if (!v.empty()) {
                        if (v.size() > 60) v = v.substr(0, 59) + L"…";
                        info.state.push_back(L"value:\"" + v + L"\"");
                    }
                }
            }
            pat.Reset();
            ComPtr<IUIAutomationRangeValuePattern> rvp;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_RangeValuePatternId,
                    IID_PPV_ARGS(&rvp))) && rvp) {
                double v = 0.0, lo = 0.0, hi = 0.0;
                const bool okV = SUCCEEDED(rvp->get_CurrentValue(&v));
                const bool okLo = SUCCEEDED(rvp->get_CurrentMinimum(&lo));
                const bool okHi = SUCCEEDED(rvp->get_CurrentMaximum(&hi));
                // 数量级差异极大（音量 0~100 / 缩放 0~500 / 视频进度 0~1e7）⇒ 用 %g 打印，
                // 避免出现 `range:0-10000000` 这种把模型带偏的长数字串。
                if (okV) info.state.push_back(L"value:" + FormatNumShort(v));
                if (okLo && okHi) {
                    info.state.push_back(L"range:" + FormatNumShort(lo) + L"-"
                        + FormatNumShort(hi));
                }
            }
            pat.Reset();
            ComPtr<IUIAutomationTogglePattern> tp;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&tp)))
                && tp) {
                ToggleState ts = ToggleState_Indeterminate;
                if (SUCCEEDED(tp->get_CurrentToggleState(&ts))) {
                    info.state.push_back(ts == ToggleState_On ? L"toggle:on"
                        : (ts == ToggleState_Off ? L"toggle:off" : L"toggle:indeterminate"));
                }
            }
            pat.Reset();
            ComPtr<IUIAutomationExpandCollapsePattern> ecp;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
                    IID_PPV_ARGS(&ecp))) && ecp) {
                ExpandCollapseState es = ExpandCollapseState_Collapsed;
                if (SUCCEEDED(ecp->get_CurrentExpandCollapseState(&es))) {
                    if (es == ExpandCollapseState_Expanded) info.state.push_back(L"state:expanded");
                    else if (es == ExpandCollapseState_Collapsed) info.state.push_back(L"state:collapsed");
                }
            }
            pat.Reset();
            ComPtr<IUIAutomationScrollPattern> sp;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&sp)))
                && sp) {
                BOOL vScrollable = FALSE;
                double vPct = 0.0;
                if (SUCCEEDED(sp->get_CurrentVerticallyScrollable(&vScrollable)) && vScrollable
                    && SUCCEEDED(sp->get_CurrentVerticalScrollPercent(&vPct))) {
                    info.state.push_back(L"v:" + FormatNumShort(vPct) + L"%");
                }
            }
            pat.Reset();
            BOOL hasFocus = FALSE;
            if (SUCCEEDED(el->get_CurrentHasKeyboardFocus(&hasFocus)) && hasFocus) {
                info.state.push_back(L"focused");
            }
            if (info.password) {
                // ⚠ 密码框只报「它是密码框」这个事实，值一个字都不给（见上面 ValuePattern 那段）。
                info.state.push_back(L"password(值不回传)");
            }
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
    // 先按行渲染（每行独立成串），再做预算内的挑选 —— 这样截断时能报出**还剩多少条**。
    std::vector<std::wstring> lines;
    lines.reserve(items.size());
    for (const auto& it : items) {
        std::wstring line = L"[" + std::to_wstring(it.id) + L"] ";
        line += it.controlType.empty() ? L"控件" : it.controlType;
        line += L" \"" + it.name + L"\"";
        // ★动作能力动词：回答「这个控件支持哪一类操作」（对齐 Windows-MCP 的 `[action: …]`）。
        //   它是**事实**不是建议 —— 模型不必靠截图猜「这是按钮还是输入框」。
        if (!it.action.empty()) line += L" action:" + it.action;
        // 标题栏按钮必须一眼可辨，否则模型会把它当成应用里的同名按钮
        if (it.titleBarControl) line += L"（窗口自身按钮·勿按名字点击）";
        if (!it.enabled) line += L"（灰）";
        if (it.valuePattern) line += L" [可填]";
        if (!it.invokable) line += L" [需点击]";
        // ★可读状态事实（focused / value:… / range:… / toggle:… / state:… / v:…% / readonly）
        //   —— 这些在画面上读不出来或很容易读错，正是「先点一下看看」的主要成因。
        for (const auto& s : it.state) {
            line += L" [";
            line += s;
            line += L"]";
        }
        if (it.rect.right > it.rect.left && it.rect.bottom > it.rect.top) {
            line += L" @";
            line += std::to_wstring((it.rect.left + it.rect.right) / 2);
            line += L",";
            line += std::to_wstring((it.rect.top + it.rect.bottom) / 2);
        }
        line += L"\n";
        lines.push_back(std::move(line));
    }

    std::wstring out;
    size_t shown = 0;
    for (; shown < lines.size(); ++shown) {
        // ★ 截断提示本身要留出预算，否则「还剩 N 条」会被挤掉 —— 而那正是模型
        //   决定要不要换个办法的关键信息（以前只回一句「…(截断)」，模型以为
        //   列表就这么长，于是转去 zoom 读像素，白烧好几轮）。
        const size_t reserve = 160;
        if (out.size() + lines[shown].size() + reserve > maxChars) break;
        out += lines[shown];
    }

    if (shown < lines.size()) {
        const size_t hidden = lines.size() - shown;
        const int firstHiddenId = items[shown].id;
        const int lastId = items.back().id;
        out += L"…(共 " + std::to_wstring(lines.size()) + L" 条，这里只列了前 "
            + std::to_wstring(shown) + L" 条；还有 " + std::to_wstring(hidden)
            + L" 条没显示，编号 " + std::to_wstring(firstHiddenId) + L"–"
            + std::to_wstring(lastId) + L")\n";
        out += L"★ 要全量请调大 maxCount 重列（上限 80）；"
               L"或在已知准确名字时**直接** invokeUiControl(name=…) —— "
               L"宿主按名字重新定位，不依赖这里列没列出来。\n";
        // 列表型内容（历史记录/书签/文件列表/消息列表）常常滚不到底：
        // 这时「名字」才是正路，别去 zoom 看截图（标题在界面上是被省略号截断的）。
        out += L"★ 若上面多为列表项：先 scrollWheel 滚动再重列，或直接用已知的完整名字触发；"
               L"**别靠截图读标题** —— 界面上标题是省略号截断的，UIA 名字才是完整的。\n";
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
        // ★窗口自身的标题栏按钮（关闭/最小化/最大化）**永不参与按名字挑选**。
        //   它的名字（「关闭」）和应用内按钮完全一样，评分上还因为可 Invoke 白拿 120 分
        //   ——实测就是这样把游戏窗口关掉的（模型想关卡牌面板，结果关了游戏）。
        //   窗口生命周期操作必须走明确意图的通道，不能靠「找个叫『关闭』的东西点一下」。
        if (it.titleBarControl) continue;
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
    std::wstring& outWarn, bool* outShellIconItem) {
    if (outShellIconItem) *outShellIconItem = false;
    outActualName.clear();
    outActualId = 0;
    outRect = RECT{};
    outInvoked = false;
    outWarn.clear();

    const std::vector<UiControlInfo> items = ListInteractiveUiControls(nullptr, 60);
    // ★先单独判「这个名字只命中窗口自身的标题栏按钮」——那是最危险的一类误点
    //   （「关闭」一击关掉整个窗口）。给出可执行的解释，而不是笼统的「没找到」。
    {
        const std::wstring wantLower = LowerCopyLocal(TrimCopy(name));
        for (const auto& it : items) {
            if (it.titleBarControl && LowerCopyLocal(TrimCopy(it.name)) == wantLower) {
                outActualName = it.name;
                outActualId = it.id;
                outWarn = L"「" + it.name + L"」是**窗口自身**的标题栏按钮（关/最小化/最大化），"
                    L"不是当前应用里的按钮 —— 按名字点它会把整个窗口关掉"
                    L"（实测把游戏窗口关没了、进度丢失）。"
                    L"窗口操作请走明确通道（switchWindow 关窗/切窗）；"
                    L"要关应用内的面板，请用更具体的名字或 locateAndClick 描述面板内的关闭控件。";
                return false;
            }
        }
    }
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
    // ★ 先把「这是 shell 图标」告诉调用方：shell 的 Invoke **只做选中**，
    //   用户语义里的"打开"必须**双击**（见 `UiControlInfo::shellIconItem` 的说明）。
    if (outShellIconItem) *outShellIconItem = IsShellIconItem(el.Get());
    ComPtr<IUnknown> pat;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_InvokePatternId, &pat)) && pat) {
        ComPtr<IUIAutomationInvokePattern> invoke;
        if (SUCCEEDED(pat.As(&invoke)) && invoke && SUCCEEDED(invoke->Invoke())) {
            outInvoked = true;
        }
    }
    return true;
}

NonClientPointInfo ProbeWindowNonClientAtPoint(HWND hwnd, int screenX, int screenY) {
    NonClientPointInfo info;
    HWND target = hwnd;
    if (!target || !IsWindow(target)) {
        target = GetForegroundWindow();
        if (!target || !IsWindow(target)) return info;   // 判不了 ⇒ probed=false ⇒ 放行
        target = GetAncestor(target, GA_ROOT);
        if (!target || !IsWindow(target)) target = GetForegroundWindow();
    }
    if (!target || !GetWindowRect(target, &info.windowRect)) return info;
    RECT cr{};
    if (!GetClientRect(target, &cr)) return info;
    POINT tl{ cr.left, cr.top };
    POINT br{ cr.right, cr.bottom };
    if (!ClientToScreen(target, &tl) || !ClientToScreen(target, &br)) return info;
    info.clientRect = RECT{ tl.x, tl.y, br.x, br.y };
    // 客户区退化（最小化 / 还没建好）→ 量不准，按「判不了」处理，**别拦**
    if (info.clientRect.right <= info.clientRect.left
        || info.clientRect.bottom <= info.clientRect.top) {
        return info;
    }
    info.probed = true;
    info.nonClient = IsPointInWindowNonClientStrip(
        screenX, screenY, info.windowRect, info.clientRect);
    if (info.nonClient) {
        if (ComPtr<IUIAutomation> uia = CreateAutomation()) {
            POINT pt{ screenX, screenY };
            ComPtr<IUIAutomationElement> el;
            if (SUCCEEDED(uia->ElementFromPoint(pt, &el)) && el) {
                BSTR nm = nullptr;
                if (SUCCEEDED(el->get_CurrentName(&nm)) && nm) {
                    info.hint = nm;
                    SysFreeString(nm);
                }
            }
        }
    }
    return info;
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
