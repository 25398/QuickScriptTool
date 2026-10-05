// ──────────────────────────────────────────────────────────────────
// window_ai_driver.cpp — 窗口反代驱动层实现（UIA + 键鼠）
//
// ⚠ 本文件是**唯一**碰 UIA/键鼠/剪贴板的地方；所有判断（匹配/打分/提取/稳定）
//   都在 `window_ai_profile.cpp` 的纯函数里 ⇒ 那一层能逐格自检。
// ──────────────────────────────────────────────────────────────────

#include "web_ai/window_ai_driver.h"

#include "action_utils.h"
#include "ocr_backend.h"
#include "ocr_engine.h"
#include "utils.h"
#include "window_mode/background_window_input.h"
#include "window_mode/fake_focus/fake_focus_soft_input_host.h"
#include "window_mode/com_apartment.h"
#include "window_mode/ui_element_probe.h"
#include "window_mode/virtual_desktop_accessor.h"
#include "window_mode/window_capture.h"
#include "window_mode/window_mode_executor.h"
#include "window_mode/window_list.h"
#include "window_mode/window_mode_log.h"

#include <UIAutomation.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>

namespace quickscript::webai {

using Microsoft::WRL::ComPtr;
using nlohmann::json;

namespace {

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string ToUtf8Safe(const std::wstring& w) {
    return ToUtf8(w);
}

std::wstring FromUtf8Safe(const std::string& s) {
    return FromUtf8(s);
}

std::wstring TruncW(const std::wstring& s, size_t n) {
    return s.size() <= n ? s : (s.substr(0, n) + L"…");
}

std::wstring LowerW(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) o.push_back(static_cast<wchar_t>(towlower(c)));
    return o;
}

bool ContainsNoCaseW(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return false;
    return LowerW(hay).find(LowerW(needle)) != std::wstring::npos;
}

// ── 剪贴板（写/读文本；与 agent_shell 的 copyAgentTextToClipboard 同一套做法）──
bool SetClipboardTextW(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* dst = GlobalLock(h)) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(h);
                ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
            }
            if (!ok) GlobalFree(h);
        }
    }
    CloseClipboard();
    return ok;
}

std::wstring GetClipboardTextW() {
    std::wstring out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

// ── UIA 基础设施 ──────────────────────────────────────────────────
ComPtr<IUIAutomation> CreateAutomation() {
    // 与 ui_element_probe.cpp 同一约定：不 CoUninitialize（别的模块在同线程缓存 COM 对象）
    windowmode::EnsureThreadComApartment();
    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&uia)))) {
        return nullptr;
    }
    return uia;
}

std::wstring BstrToWide(BSTR b) {
    std::wstring out = b ? b : L"";
    if (b) SysFreeString(b);
    return out;
}

bool IsPasswordElement(IUIAutomationElement* el) {
    // ⚠ 必须在读 ValuePattern **之前**判：UIA 在部分应用里会把密码明文交出来
    VARIANT v{};
    if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_IsPasswordPropertyId, &v))) {
        const bool pw = (v.vt == VT_BOOL && v.boolVal == VARIANT_TRUE);
        VariantClear(&v);
        return pw;
    }
    return false;
}

std::wstring ReadElementText(IUIAutomationElement* el, bool allowValue) {
    if (!el) return {};
    if (allowValue && !IsPasswordElement(el)) {
        ComPtr<IUIAutomationValuePattern> vp;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&vp))) && vp) {
            BSTR b = nullptr;
            if (SUCCEEDED(vp->get_CurrentValue(&b))) {
                const std::wstring s = BstrToWide(b);
                if (!s.empty()) return s;
            }
        }
    }
    ComPtr<IUIAutomationTextPattern> tp;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&tp))) && tp) {
        ComPtr<IUIAutomationTextRange> range;
        if (SUCCEEDED(tp->get_DocumentRange(&range)) && range) {
            BSTR b = nullptr;
            if (SUCCEEDED(range->GetText(-1, &b))) {
                const std::wstring s = BstrToWide(b);
                if (!s.empty()) return s;
            }
        }
    }
    BSTR name = nullptr;
    if (SUCCEEDED(el->get_CurrentName(&name))) return BstrToWide(name);
    return {};
}

/// UIA 快照（内部用：节点数据 + 对应的 COM 元素，供后续 focus/写入）
struct Snapshot {
    std::vector<WindowAiUiNode> nodes;
    std::vector<ComPtr<IUIAutomationElement>> els;
};

bool SnapshotImpl(HWND hwnd, int maxNodes, Snapshot& out) {
    out.nodes.clear();
    out.els.clear();
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (maxNodes <= 0) maxNodes = 400;

    ComPtr<IUIAutomation> uia = CreateAutomation();
    if (!uia) return false;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(hwnd, &root)) || !root) return false;

    // 只看 Control 视图（与产品其它 UIA 代码同一口径）
    VARIANT var{};
    var.vt = VT_BOOL;
    var.boolVal = VARIANT_TRUE;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(uia->CreatePropertyCondition(UIA_IsControlElementPropertyId, var, &cond)) || !cond) {
        return false;
    }
    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &found)) || !found) return false;

    int count = 0;
    if (FAILED(found->get_Length(&count))) return false;
    out.nodes.reserve(static_cast<size_t>((std::min)(count, maxNodes)));
    out.els.reserve(out.nodes.capacity());

    for (int i = 0; i < count && static_cast<int>(out.nodes.size()) < maxNodes; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(found->GetElement(i, &el)) || !el) continue;

        WindowAiUiNode n;
        CONTROLTYPEID ct = 0;
        if (SUCCEEDED(el->get_CurrentControlType(&ct))) n.controlTypeId = static_cast<int>(ct);
        const wchar_t* en = WindowAiControlTypeName(n.controlTypeId);
        if (en && *en) {
            n.controlType = en;
        } else {
            // 我的英文名表里没有 ⇒ 用产品的类型表给中文标签，并标出 id（探针里能看见缺口）
            std::wstring label;
            int rows = 0;
            const windowmode::UiControlTypeRow* table = windowmode::UiControlTypeTable(&rows);
            for (int r = 0; r < rows; ++r) {
                if (table[r].controlTypeId == n.controlTypeId) {
                    label = table[r].label;
                    break;
                }
            }
            n.controlType = label.empty()
                ? (L"Unknown" + std::to_wstring(n.controlTypeId))
                : label;
        }
        BSTR b = nullptr;
        if (SUCCEEDED(el->get_CurrentName(&b))) n.name = BstrToWide(b);
        if (SUCCEEDED(el->get_CurrentAutomationId(&b))) n.automationId = BstrToWide(b);
        RECT rc{};
        if (SUCCEEDED(el->get_CurrentBoundingRectangle(&rc))) n.rect = rc;

        auto boolProp = [&](PROPERTYID id, bool def) {
            VARIANT v{};
            bool r = def;
            if (SUCCEEDED(el->GetCurrentPropertyValue(id, &v))) {
                if (v.vt == VT_BOOL) r = (v.boolVal == VARIANT_TRUE);
                VariantClear(&v);
            }
            return r;
        };
        n.focusable = boolProp(UIA_IsKeyboardFocusablePropertyId, false);
        n.enabled = boolProp(UIA_IsEnabledPropertyId, true);
        n.offscreen = boolProp(UIA_IsOffscreenPropertyId, false);
        n.keyboardFocus = boolProp(UIA_HasKeyboardFocusPropertyId, false);

        ComPtr<IUnknown> pat;
        n.valuePattern = SUCCEEDED(el->GetCurrentPattern(UIA_ValuePatternId, &pat)) && pat;
        pat.Reset();
        n.textPattern = SUCCEEDED(el->GetCurrentPattern(UIA_TextPatternId, &pat)) && pat;
        n.value = ReadElementText(el.Get(), true);
        if (n.value.size() > 300) n.value = TruncW(n.value, 300);

        out.nodes.push_back(std::move(n));
        out.els.push_back(el);
    }
    return true;
}

void TapKey(WORD vk) {
    if (!vk) return;
    SendKeyboardKey(vk, true);
    Sleep(12);
    SendKeyboardKey(vk, false);
    Sleep(12);
}

void SendCtrlKey(WORD vk) {
    SendKeyboardKey(VK_CONTROL, true);
    Sleep(12);
    TapKey(vk);
    SendKeyboardKey(VK_CONTROL, false);
    Sleep(20);
}

/// 在前台窗口上点一下（仅在"UIA 聚焦失败"时兜底；会把鼠标挪过去）
void ClickPoint(int x, int y) {
    SetCursorPos(x, y);
    Sleep(40);
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

RECT ClientRectOnScreen(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    POINT tl{ rc.left, rc.top };
    POINT br{ rc.right, rc.bottom };
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    RECT out{ tl.x, tl.y, br.x, br.y };
    return out;
}

std::wstring DescribeNode(const WindowAiUiNode& n) {
    std::wstring s = n.controlType;
    if (!n.name.empty()) s += L"「" + TruncW(n.name, 40) + L"」";
    if (!n.automationId.empty()) s += L"#" + TruncW(n.automationId, 30);
    return s;
}

/// ★★ OCR 读一块**客户区相对**区域（Electron/Chromium 客户端的唯一读法）
///
/// 用**系统自带** WinRT OCR（`RunWinRtOcr`）而不是 Python 后端：
///   · 零安装、零体积、离线可用（Python 那套要 venv，且**起进程**、秒级开销）；
///   · 这里每次轮询都要读一次，必须是低延迟的本地调用。
/// ⚠ 失败要**如实**带回原因（语言包缺失 / 截图空白），不许返回空串冒充"没有回答"。
bool OcrReadClientRegion(HWND hwnd, const RECT& regionScreen, std::wstring& out, std::string& err) {
    out.clear();
    err.clear();
    // 1) 客户区坐标（CaptureWindowRegion 收的是客户区坐标）
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT tl{ 0, 0 };
    ClientToScreen(hwnd, &tl);
    const int cx1 = (std::max)(0, static_cast<int>(regionScreen.left - tl.x));
    const int cy1 = (std::max)(0, static_cast<int>(regionScreen.top - tl.y));
    const int cx2 = (std::min)(static_cast<int>(client.right), static_cast<int>(regionScreen.right - tl.x));
    const int cy2 = (std::min)(static_cast<int>(client.bottom), static_cast<int>(regionScreen.bottom - tl.y));
    if (cx2 - cx1 < 8 || cy2 - cy1 < 8) {
        err = "OCR 区域太小（配置里的相对矩形可能写反了）";
        return false;
    }
    windowmode::WindowCaptureResult cap = windowmode::CaptureWindowRegion(hwnd, cx1, cy1, cx2, cy2);
    if (!cap.bitmap) {
        err = "截取窗口区域失败（窗口可能已关闭/最小化）";
        return false;
    }
    std::wstring why;
    if (!WinRtOcrAvailable(&why)) {
        DeleteObject(cap.bitmap);
        err = "系统 OCR 不可用：" + ToUtf8Safe(why);
        return false;
    }
    const OcrEngineOutput ocr = RunWinRtOcr(cap.bitmap, /*digitsOnly=*/false);
    DeleteObject(cap.bitmap);
    if (!ocr.success) {
        err = "OCR 失败：" + ToUtf8Safe(ocr.error);
        return false;
    }
    out = ConcatOcrLines(ocr);
    return true;
}

}  // namespace

const wchar_t* WindowAiWriteProofName(WindowAiWriteProof p) {
    switch (p) {
    case WindowAiWriteProof::kVerified: return L"verified";
    case WindowAiWriteProof::kUnavailable: return L"unavailable";
    case WindowAiWriteProof::kMismatch: return L"mismatch";
    default: return L"failed";
    }
}

WindowAiDriver& WindowAiDriver::Instance() {
    static WindowAiDriver inst;
    return inst;
}

namespace {
std::mutex g_clientsMu;
std::vector<WindowAiClientProfile> g_clients;
bool g_clientsLoaded = false;
std::string g_clientsError;
}  // namespace

const std::vector<WindowAiClientProfile>& WindowAiDriver::Clients() {
    std::lock_guard<std::mutex> lock(g_clientsMu);
    if (!g_clientsLoaded) {
        std::vector<WindowAiClientProfile> list = BuiltinWindowAiClients();
        g_clientsError.clear();
        const std::wstring path = AppDir() + L"\\window_ai_providers.json";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const std::string utf8 = ToUtf8Safe(ReadAll(path));
            list = MergeWindowAiProfiles(utf8, list, g_clientsError);
            if (!g_clientsError.empty()) {
                windowmode::WindowModeLogEventf(L"[窗口AI] 档案覆盖文件有问题：%s",
                    FromUtf8Safe(g_clientsError).c_str());
            }
        }
        g_clients = std::move(list);
        g_clientsLoaded = true;
    }
    return g_clients;
}

void WindowAiDriver::ReloadClients() {
    std::lock_guard<std::mutex> lock(g_clientsMu);
    g_clientsLoaded = false;
}

std::vector<WindowAiTarget> WindowAiDriver::ListCandidateWindows() {
    std::vector<WindowAiTarget> out;
    for (const auto& w : windowmode::ListSwitchableWindows()) {
        WindowAiTarget t;
        t.hwnd = w.hwnd;
        t.title = w.title;
        t.processName = w.processName;
        t.foreground = w.foreground;
        t.minimized = w.minimized;
        GetWindowRect(w.hwnd, &t.windowRect);
        t.clientRectScreen = ClientRectOnScreen(w.hwnd);
        out.push_back(std::move(t));
    }
    return out;
}

namespace {

struct EnumClientCtx {
    std::vector<WindowAiTarget>* out = nullptr;
};

BOOL CALLBACK EnumClientWindowProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumClientCtx*>(lp);
    if (!ctx || !ctx->out) return TRUE;
    // 只看顶层窗口（有 owner 的是附属窗，如托盘气泡/工具窗）
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;
    // 进程名（exe 名，不含路径 —— 与 profile 里的写法一致）
    std::wstring exeName;
    {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            wchar_t buf[MAX_PATH] = {};
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
                const std::wstring full(buf);
                const size_t slash = full.find_last_of(L"\\/");
                exeName = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
            }
            CloseHandle(h);
        }
    }
    if (exeName.empty()) return TRUE;
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return TRUE;
    if (wr.right - wr.left < 40 || wr.bottom - wr.top < 40) return TRUE;   // 明显不是主窗
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    WindowAiTarget t;
    t.hwnd = hwnd;
    t.title = title;
    t.processName = exeName;
    t.foreground = (GetForegroundWindow() == hwnd);
    t.minimized = IsIconic(hwnd) != 0;
    t.windowRect = wr;
    t.clientRectScreen = ClientRectOnScreen(hwnd);
    ctx->out->push_back(std::move(t));
    return TRUE;
}

/// ★专用枚举：**不要求窗口可见**（`ListSwitchableWindows` 按 Alt+Tab 口径把隐藏窗滤掉了，
///   而实测 Cursor / 豆包的主窗恰恰就是"存在但隐藏"—— 那时它在 Alt+Tab 口径里根本不存在）。
std::vector<WindowAiTarget> EnumClientWindowsIncludingHidden() {
    std::vector<WindowAiTarget> out;
    EnumClientCtx ctx;
    ctx.out = &out;
    EnumWindows(EnumClientWindowProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

// ──────────────────────────────────────────────────────────────────
// ★★ 后台输入会话：**直接复用窗口模式那条链路**
//
// 为什么不再手拼假焦点 API（上一版就是这么写的，实测注入成功但文本送不进 Chromium）：
//   窗口模式的后台输入不是"注入一个 DLL + 灌几个键"那么简单 —— `WindowModeExecutor`
//   在 `BeginRun` 里做了**一整套准备**（绑窗、解析输入子窗、进程内焦点、软光标同步……），
//   再由它自己的 `SendQuickInputToTarget` / `PostMouseClickAtClient` 走正确分支。
// ⇒ 建一个**最小窗口模式会话**，用它的输入接口：
//     · `executionKind = BackgroundWindow`：只有"独立桌面模式"才 CreateDesktop + 搬窗，
//       后台窗口模式**在用户桌面上跑、不搬窗、不抢前台**
//       （依据：`window_mode_session.cpp` 里"模拟器误选独立桌面模式会 CreateDesktop + 搬窗"
//        这条注释，以及 BeginRun 里的 `background ? LaunchTargetOnDefaultDesktop : ...`）；
//     · `selectMethod = UseEditorWindowClass`：按"窗口类名 + 进程路径"绑定**已存在**的窗口；
//     · `allowForegroundInputFallback = false`：**绝不**回退到抢前台（用户明确要求）。
// ──────────────────────────────────────────────────────────────────
class WmInputSession {
public:
    ~WmInputSession() { End(); }

    bool Begin(HWND hwnd, bool fakeFocusEnabled, bool allowForegroundFallback) {
        if (ok_) return true;
        wchar_t cls[256] = {};
        GetClassNameW(hwnd, cls, 256);
        std::wstring exePath;
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (h) {
                wchar_t buf[MAX_PATH] = {};
                DWORD n = MAX_PATH;
                if (QueryFullProcessImageNameW(h, 0, buf, &n)) exePath = buf;
                CloseHandle(h);
            }
        }
        windowmode::WindowModeScriptConfig cfg;
        cfg.enabled = true;
        cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
        cfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
        cfg.windowClassName = cls;
        cfg.useTopLevelWindow = true;
        cfg.targetExePath = exePath;
        cfg.windowNameIsHintOnly = true;
        cfg.fakeFocusEnabled = fakeFocusEnabled;
        cfg.allowForegroundInputFallback = allowForegroundFallback;
        cfg.inputStrategy = windowmode::WindowModeInputStrategy::Auto;

        windowmode::WindowModeLogEventf(
            L"[窗口AI] 建后台输入会话：class=%s exe=%s fakeFocus=%d 前台回退=%d",
            cls, exePath.c_str(), fakeFocusEnabled ? 1 : 0, allowForegroundFallback ? 1 : 0);
        windowmode::BeginRunOptions opts;
        opts.launchTarget = false;   // ★ 只绑已存在的窗口，绝不自己启动/重启客户端
        if (!exec_.BeginRun(cfg, err_, opts)) {
            if (err_.empty()) err_ = L"BeginRun 失败（未给出原因）";
            return false;
        }
        ok_ = true;
        return true;
    }

    void End() {
        if (!ok_) return;
        exec_.EndRun();
        ok_ = false;
    }

    bool ok() const { return ok_; }
    const std::wstring& err() const { return err_; }
    windowmode::WindowModeExecutor& exec() { return exec_; }

private:
    windowmode::WindowModeExecutor exec_;
    bool ok_ = false;
    std::wstring err_;
};

}  // namespace

bool WindowAiDriver::FindClientWindow(const WindowAiClientProfile& profile,
    WindowAiTarget& out, std::string& err) {
    err.clear();
    const auto all = ListCandidateWindows();

    // ★★ 候选池 = 「可切换窗口」∪「含隐藏的全量枚举」。
    //   为什么要并：`ListSwitchableWindows` 是 Alt+Tab 口径，实测 Cursor / 豆包的主窗
    //   会处于"存在但隐藏"（`IsWindowVisible=false`、owner 为空、非工具窗）⇒ 在那里它
    //   根本不存在（甚至被同进程一个 158x26 的小窗顶替成"窗口太小"）。
    std::vector<WindowAiTarget> pool = all;
    for (const auto& w : EnumClientWindowsIncludingHidden()) {
        bool dup = false;
        for (const auto& e : pool) {
            if (e.hwnd == w.hwnd) { dup = true; break; }
        }
        if (!dup) pool.push_back(w);
    }

    // ★★ 匹配规则（每一条都对应一次实测踩坑）：
    //   ① 尺寸下限：客户端常有同进程的**小工具窗**（Cursor / 豆包都有个 158x26 的离屏窗，
    //      标题与大窗相同或干脆是会话名）—— 选它就给 26 像素高的窗口打字，回执还"成功"；
    //   ② 标题匹配不上时**放宽**（见下）：隐藏窗的 `GetWindowTextW` 常常是**空标题**
    //      （Chromium 不给），而档案里写的是 `titleContains:["豆包"]`；
    //   ③ 排除项 `titleExcludes` 在放宽后仍然尊重（"哪些窗口不要"是硬约束）。
    int matchedCount = 0;
    int tinyCount = 0;
    std::vector<const WindowAiTarget*> matched;
    for (const auto& w : pool) {
        if (!WindowAiWindowMatches(profile, w.processName, w.title)) continue;
        ++matchedCount;
        const int ww = w.windowRect.right - w.windowRect.left;
        const int wh = w.windowRect.bottom - w.windowRect.top;
        if (ww < profile.minWindowWidth || wh < profile.minWindowHeight) {
            ++tinyCount;
            continue;
        }
        matched.push_back(&w);
    }
    if (matched.empty()) {
        for (const auto& w : pool) {
            bool procOk = false;
            for (const auto& pn : profile.processNames) {
                if (!pn.empty() && LowerW(w.processName) == LowerW(pn)) { procOk = true; break; }
            }
            if (!procOk) continue;
            bool excluded = false;
            for (const auto& ex : profile.titleExcludes) {
                if (!ex.empty() && ContainsNoCaseW(w.title, ex)) { excluded = true; break; }
            }
            if (excluded) continue;
            const int ww = w.windowRect.right - w.windowRect.left;
            const int wh = w.windowRect.bottom - w.windowRect.top;
            if (ww < profile.minWindowWidth || wh < profile.minWindowHeight) continue;
            matched.push_back(&w);
        }
        if (!matched.empty()) {
            windowmode::WindowModeLogEventf(
                L"[窗口AI] 严格标题匹配没命中（隐藏窗标题可能是空的），已放宽到「进程名+尺寸」");
        }
    }

    // 排序：可见 > 前台 > 面积大
    std::stable_sort(matched.begin(), matched.end(),
        [](const WindowAiTarget* a, const WindowAiTarget* b) {
            const bool av = IsWindowVisible(a->hwnd) != 0;
            const bool bv = IsWindowVisible(b->hwnd) != 0;
            if (av != bv) return av;
            if (a->foreground != b->foreground) return a->foreground;
            const long long aa = static_cast<long long>(a->windowRect.right - a->windowRect.left)
                * (a->windowRect.bottom - a->windowRect.top);
            const long long ba = static_cast<long long>(b->windowRect.right - b->windowRect.left)
                * (b->windowRect.bottom - b->windowRect.top);
            return aa > ba;
        });

    // ★★ 逐个候选试：**恢复隐藏窗，并验它到底有没有可用的 UIA 树**。
    //
    //   为什么必须验（实测踩到）：豆包进程里有个**空壳大窗**（1920x997、标题为空、
    //   `FindAll(Descendants)` 返回 **0**），而真正有 UI 的那个窗有 **178** 个节点。
    //   只按"面积最大"选 ⇒ 选中空壳窗：字写进去、也提交了，然后什么都读不回来，
    //   而回执会说"提交后没读到新内容" —— 把「选错了窗」说成「客户端不回复」。
    //   ⇒ 判据落在**能用的证据**上：UIA 节点数 ≥ 8 才算"这个窗是活的"。
    const WindowAiTarget* fallback = nullptr;
    for (const WindowAiTarget* c : matched) {
        const bool wasVisible = IsWindowVisible(c->hwnd) != 0;
        // ★★ 先看它**在不在当前虚拟桌面**：不在的话渲染被系统节流，
        //   UIA 树读出来是**空的**（实测：独立工具与产品内查询都是 0 个节点、标题也空），
        //   于是会被误判成"客户端没开无障碍 / 空壳窗"。搬回当前桌面是唯一能让它可用的办法。
        bool movedDesktop = false;
        {
            auto& vd = windowmode::VirtualDesktopAccessor::Instance();
            if (vd.IsWindowOnCurrentVirtualDesktop(c->hwnd) == 0) {   // 明确"不在当前桌面"
                const int cur = vd.GetCurrentDesktopNumber();
                if (cur >= 0 && vd.MoveWindowToDesktopNumber(c->hwnd, cur)) {
                    movedDesktop = true;
                    Sleep(250);
                    windowmode::WindowModeLogEventf(
                        L"[窗口AI] 目标窗口在别的虚拟桌面，已搬回当前桌面：%s「%s」",
                        c->processName.c_str(), c->title.c_str());
                }
            }
        }
        if (!wasVisible || movedDesktop) {
            ShowWindow(c->hwnd, SW_SHOW);
            if (IsIconic(c->hwnd)) ShowWindow(c->hwnd, SW_RESTORE);
            Sleep(150);
        }
        if (!IsWindowVisible(c->hwnd)) continue;
        if (!fallback) fallback = c;
        WindowAiTarget cand = *c;
        cand.restoredFromHidden = !wasVisible;
        cand.movedToCurrentDesktop = movedDesktop;
        cand.clientRectScreen = ClientRectOnScreen(c->hwnd);
        GetWindowRect(c->hwnd, &cand.windowRect);
        std::vector<WindowAiUiNode> probeNodes;
        const bool hasTree = SnapshotControls(c->hwnd, probeNodes, 12)
            && probeNodes.size() >= 8;
        if (!hasTree) continue;   // 空壳窗：继续看下一个候选（同进程可能有真窗）
        out = cand;
        if (out.restoredFromHidden || out.movedToCurrentDesktop) {
            windowmode::WindowModeLogEventf(
                L"[窗口AI] 目标窗口原本不可用（隐藏=%d 别的桌面=%d），已恢复：%s「%s」（UIA 节点 %d）",
                out.restoredFromHidden ? 1 : 0, out.movedToCurrentDesktop ? 1 : 0,
                out.processName.c_str(), out.title.c_str(), static_cast<int>(probeNodes.size()));
        }
        return true;
    }
    if (fallback) {
        // 所有可见候选都没有 UIA 树：仍然返回一个（几何+OCR 路径不需要树），
        // 但如实记一笔 —— 否则后面的"读不回文本"会被误读成客户端的问题。
        const bool wasVisible = IsWindowVisible(fallback->hwnd) != 0;
        out = *fallback;
        out.restoredFromHidden = !wasVisible;
        out.clientRectScreen = ClientRectOnScreen(fallback->hwnd);
        windowmode::WindowModeLogEventf(
            L"[窗口AI] 警告：候选窗口都没有 UIA 树（可能是空壳窗或无障碍未开）：%s「%s」",
            out.processName.c_str(), out.title.c_str());
        return true;
    }
    if (matchedCount > 0 && tinyCount == matchedCount) {
        err = "匹配到的窗口都太小（" + std::to_string(tinyCount) + " 个，全部小于 "
            + std::to_string(profile.minWindowWidth) + "x" + std::to_string(profile.minWindowHeight)
            + "）：多半是托盘/悬浮小窗（连隐藏窗口一起找过了，只有这些）。"
              "请把客户端主窗口还原并切到前台，再重试。";
    }

    // ★★ 找不到时必须区分三种情况 —— 它们的处置**完全不同**，
    //   混成一句"进程没在跑"会让用户白重启客户端（实测：Cursor 的窗口
    //   被本软件的窗口模式搬到「鼠标宏」桌面后 `IsWindowVisible=false`，
    //   `ListSwitchableWindows` 按 Alt+Tab 口径把它滤掉 ⇒ 表现成"没在跑"）。
    bool processRunning = false;
    {
        // 按 exe 名找进程（不含路径；profile 里存的就是 exe 名）
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    for (const auto& want : profile.processNames) {
                        if (!want.empty() && LowerW(pe.szExeFile) == LowerW(want)) {
                            processRunning = true;
                            break;
                        }
                    }
                    if (processRunning) break;
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }
    if (processRunning) {
        err = "客户端进程在跑，但**没有一个可见的顶层窗口**（被最小化到托盘 / 在别的虚拟桌面 / "
              "或被本软件的窗口模式搬到「鼠标宏」桌面了）。请把它切到当前桌面并还原窗口，再重试。";
    } else {
        err = "进程没在跑：期望进程 " + ToUtf8Safe(profile.processNames.empty()
            ? std::wstring(L"(未配置，用 titleContains 匹配)")
            : profile.processNames[0])
            + "（当前可切换窗口 " + std::to_string(all.size()) + " 个）";
    }
    return false;
}

bool WindowAiDriver::SnapshotControls(HWND hwnd, std::vector<WindowAiUiNode>& out, int maxNodes) {
    Snapshot snap;
    if (!SnapshotImpl(hwnd, maxNodes, snap)) return false;
    out = snap.nodes;
    return true;
}

bool WindowAiDriver::ScanCopyButton(WindowAiClientProfile profile, HWND hwnd,
    const std::wstring& prompt, std::wstring& outReply, POINT& outPoint,
    std::vector<double>& outOffset, std::string& why, std::vector<CopyScanHit>* outHits) {
    outReply.clear();
    outPoint = POINT{};
    outOffset.clear();
    why.clear();
    profile.reply.restoreClipboard = true;
    // 锚点（用来把命中的绝对坐标换算回相对偏移）
    WindowAiTarget target;
    target.hwnd = hwnd;
    const RECT client = ClientRectOnScreen(hwnd);
    const int cw = (std::max)(1, static_cast<int>(client.right - client.left));
    const int ch = (std::max)(1, static_cast<int>(client.bottom - client.top));
    RECT anchor{};
    {
        Snapshot snap;
        if (!SnapshotImpl(hwnd, 400, snap)) {
            why = "UIA 快照失败";
            return false;
        }
        int limit = client.top + static_cast<int>(ch * 0.85);
        RECT inRect{};
        if (ResolveRelativeRect(profile.input.rectHint, client, inRect) && inRect.top > client.top) {
            limit = inRect.top - 4;
        }
        if (!PickWindowAiCopyAnchor(profile.reply, snap.nodes, client, prompt, limit, anchor)) {
            why = "没找到几何锚点（会话列里没有可用的文本节点）";
            return false;
        }
    }
    // ★ 扫描范围**刻意很窄**（理由见头文件）：往右是 赞/踩/分享/重新生成。
    //   ⚠ x 用**实测值**分档：OCR 行框量到图标在 x_rel≈0.334/0.418（复制/赞），
    //     而配置里的 `copyButtonXRel` 默认 0.28 会偏左 ~100px ⇒ 两档都试。
    const int baseX = client.left + static_cast<int>(profile.reply.copyButtonXRel * cw);
    const int xRels[] = { 334, 320, 300, 348, 360 };
    const int xs[] = { client.left + static_cast<int>(xRels[0] * cw / 1000),
                       client.left + static_cast<int>(xRels[1] * cw / 1000),
                       baseX,
                       client.left + static_cast<int>(xRels[3] * cw / 1000),
                       client.left + static_cast<int>(xRels[4] * cw / 1000) };
    const int yBase = static_cast<int>(anchor.bottom);
    //   ⚠ y 偏移也要放宽：实测图标行在锚点下方 **~96px**（配置默认 34px 远不够）
    const int ys[] = { yBase + 24, yBase + 34, yBase + 44, yBase + 56, yBase + 70,
                       yBase + 86, yBase + 100, yBase + 16 };
    int tried = 0;
    int hits = 0;
    for (int dy : ys) {
        for (int x : xs) {
            // 先抢前台：标定会在几秒内多次点击，中途失焦就会打到别的窗口
            std::wstring aerr;
            if (!windowmode::ActivateWindow(hwnd, aerr)) continue;
            Sleep(120);
            std::wstring got;
            std::string w;
            ++tried;
            if (TryClipboardReply(profile, hwnd, prompt, got, w, x, dy) && !got.empty()) {
                ++hits;
                // ★ 命中即**如实收集**（内容 + 落点 + 换算出的偏移），不在这里判"对不对"：
                //   验收要人来判（哪些内容是回答、哪些是别的卡片），工具只负责把地图打出来。
                CopyScanHit h;
                h.point = POINT{ x, dy };
                h.text = got;
                h.offset = { (x - client.left) / static_cast<double>(cw)
                                 - profile.reply.copyButtonXRel,
                             (dy - anchor.bottom) / static_cast<double>(ch) };
                windowmode::WindowModeLogEventf(
                    L"[窗口AI] 标定命中：落点=(%d,%d) 偏移=[%.4f,%.4f] 内容=「%s」",
                    x, dy, h.offset[0], h.offset[1], TruncW(got, 40).c_str());
                if (outHits) outHits->push_back(std::move(h));
                if (hits >= 8) goto done;   // 够了：再多点只会增加误触风险
            }
        }
    }
done:
    if (outHits && !outHits->empty()) {
        // 首个命中作为默认（调用方通常会据内容自行挑选）
        outPoint = outHits->front().point;
        outOffset = outHits->front().offset;
        outReply = outHits->front().text;
        return true;
    }
    why = "扫了 " + std::to_string(tried) + " 个落点，剪贴板一次都没变"
          "。可能原因：锚点没落在回答上（回答不在提问下方？），或该客户端的操作栏位置不同。";
    return false;
}

bool WindowAiDriver::ReadConversation(const WindowAiClientProfile& profile, HWND hwnd,
    std::wstring& text, std::string& err, std::string* sourceOut,
    const std::wstring& echoHint) {
    err.clear();
    text.clear();
    if (sourceOut) sourceOut->clear();
    Snapshot snap;
    if (!SnapshotImpl(hwnd, 400, snap)) {
        err = "UIA 快照失败（窗口可能已关闭）";
        return false;
    }
    std::wstring uiaText;
    // ① 容器候选里面积最大的那个，用 TextPattern 读整段（最接近"会话正文"）
    if (profile.reply.preferTextPattern) {
        int best = -1;
        long long bestArea = -1;
        for (size_t i = 0; i < snap.nodes.size(); ++i) {
            const auto& n = snap.nodes[i];
            if (!n.textPattern && !n.valuePattern) continue;
            if (!profile.reply.containerTypes.empty()) {
                bool ok = false;
                for (const auto& t : profile.reply.containerTypes) {
                    if (LowerW(t) == LowerW(n.controlType)) { ok = true; break; }
                }
                if (!ok) continue;
            }
            if (n.Area() > bestArea) { bestArea = n.Area(); best = static_cast<int>(i); }
        }
        if (best >= 0) {
            uiaText = ReadElementText(snap.els[static_cast<size_t>(best)].Get(), true);
        }
    }
    // ② 拼接兜底（Electron 常常只有一堆 Text/StaticText）
    if (uiaText.empty()) {
        std::wstring joined;
        for (const auto& n : snap.nodes) {
            std::wstring piece = n.value.empty() ? n.name : n.value;
            if (piece.empty()) continue;
            if (n.controlType == L"Button" || n.controlType == L"MenuItem") continue;  // 工具栏噪音
            joined += piece;
            joined += L"\n";
        }
        uiaText = joined;
    }

    // ③ ★★ OCR 兜底 / 择优：**两份都读，取更长的那份**。
    //
    //   ⚠⚠ 为什么不是"UIA 读不到才用 OCR"（第一版就是这么写的，真机上直接失效）：
    //     Chromium 的整页 `RootWebArea` **有** TextPattern，但只回**标题级**的二十来个字
    //     （实测豆包客户端：读回 `豆包\n` + 当前会话 id，共 26~43 字），
    //     而真正的会话正文（提问 + 回答）**只有 OCR 看得到**。
    //     ⇒ 按"长度取长者"，等价于"哪份更像正文就用哪份"，而且两条路都留了痕迹。
    std::wstring ocrText;
    std::string ocrErr;
    if (profile.reply.ocrFallback) {
        const RECT client = ClientRectOnScreen(hwnd);
        RECT region = client;
        RECT rel{};
        if (ResolveRelativeRect(profile.reply.ocrRegion, client, rel)) region = rel;
        std::wstring got;
        if (OcrReadClientRegion(hwnd, region, got, ocrErr)) ocrText = got;
    }

    // ★★ 择优（按"这份文本里有没有我们刚发出去的那句话"判断，而不是按长度）：
    //
    //   ⚠⚠ 实测教训：豆包客户端的整页 `RootWebArea` **确实包含会话正文**（914 字，
    //     混着侧栏的会话列表），而 OCR 那份虽然更长，却把消息操作**图标读成 `0`**
    //     （聊天列里 x≈0.28-0.35 那几行）⇒ 按长度取长者会选到**更脏**的那份，
    //     真机上表现为"回答是 `0`"。
    //   ⇒ 判据换成：**哪份里能找到提问的回显（忽略空白匹配），就用哪份**；
    //     两份都找不到（OCR 把提问也读花了）才退回"取长者"。
    size_t echoBegin = 0;
    size_t echoEnd = 0;
    const bool uiaHasEcho = !echoHint.empty()
        && FindWindowAiEchoIgnoringSpaces(uiaText, echoHint, echoBegin, echoEnd);
    const bool ocrHasEcho = !echoHint.empty()
        && FindWindowAiEchoIgnoringSpaces(ocrText, echoHint, echoBegin, echoEnd);

    if (uiaHasEcho && !uiaText.empty()) {
        text = uiaText;
        if (sourceOut) *sourceOut = "uia-echo";
    } else if (ocrHasEcho && !ocrText.empty()) {
        text = ocrText;
        if (sourceOut) *sourceOut = "ocr-echo";
    } else if (!uiaText.empty() && uiaText.size() >= ocrText.size()) {
        text = uiaText;
        if (sourceOut) *sourceOut = "uia";
    } else if (!ocrText.empty()) {
        text = ocrText;
        if (sourceOut) *sourceOut = "ocr";
    } else {
        err = ocrErr.empty()
            ? "窗口里读不到任何文本（UIA 树为空且 OCR 也没读到）"
            : ("读不到文本：UIA 无内容；OCR：" + ocrErr);
        return false;
    }
    if (profile.reply.maxChars > 0 && static_cast<int>(text.size()) > profile.reply.maxChars) {
        text.resize(static_cast<size_t>(profile.reply.maxChars));
    }
    return true;
}

std::string WindowAiDriver::LastClipboardDiag() const {
    return clipboardDiag_;
}

void WindowAiDriver::RememberSentPrompt(const std::wstring& text) {
    if (text.empty()) return;
    for (const auto& p : sentPrompts_) {
        if (p == text) return;   // 已经记过
    }
    sentPrompts_.push_back(text);
    if (sentPrompts_.size() > 8) sentPrompts_.erase(sentPrompts_.begin());
    // ★ 落盘：**内存记忆一重启就没了**，而"输入框里的残留"恰恰会跨重启存在
    //   （实测：上一次会话留下的 prompt 还在输入框里，重启后进程不认识它 ⇒ 一律拒绝）。
    try {
        nlohmann::json j;
        j["prompts"] = nlohmann::json::array();
        for (const auto& p : sentPrompts_) j["prompts"].push_back(ToUtf8Safe(p));
        const std::wstring path = AppDir() + L"\\window_ai_sent_prompts.json";
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (f) f << j.dump(1);
    } catch (...) {
        // 落盘失败不影响功能（只是下次重启后认不出残留）
    }
}

void WindowAiDriver::LoadSentPrompts() {
    if (sentPromptsLoaded_) return;
    sentPromptsLoaded_ = true;
    try {
        const std::wstring path = AppDir() + L"\\window_ai_sent_prompts.json";
        std::ifstream f(path, std::ios::binary);
        if (!f) return;
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
        if (!j.is_object() || !j.contains("prompts") || !j["prompts"].is_array()) return;
        for (const auto& v : j["prompts"]) {
            if (!v.is_string()) continue;
            const std::string s = v.get<std::string>();
            if (s.empty()) continue;
            const int wn = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                nullptr, 0);
            if (wn <= 0) continue;
            std::wstring w(static_cast<size_t>(wn), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), wn);
            sentPrompts_.push_back(w);
        }
    } catch (...) {
    }
}

bool WindowAiDriver::TryClipboardReply(const WindowAiClientProfile& profile, HWND hwnd,
    const std::wstring& prompt, std::wstring& out, std::string& why,
    int overrideX, int overrideY) {
    out.clear();
    why.clear();
    clipboardDiag_.clear();
    if (!profile.reply.useClipboard) {
        why = "配置里关掉了剪贴板通道（reply.useClipboard=false）";
        return false;
    }
    const RECT client = ClientRectOnScreen(hwnd);
    // ★★ 后台模式（`softHover_` 非空）**不抢前台**：旧逻辑开头就 `ActivateWindow` + 前台闸，
    //   那是因为它用**真实鼠标点击**（屏幕坐标，被挡住就点错窗口）。
    //   后台模式走 **Invoke（无鼠标）+ 软悬停**，既不需要前台、也不该抢 ——
    //   用户的要求就是"问个问题的时候电脑还能正常用"。
    const bool backgroundMode = static_cast<bool>(softHover_);
    if (!backgroundMode) {
        std::wstring aerr;
        if (!windowmode::ActivateWindow(hwnd, aerr)) {
            why = "无法把客户端切到前台（几何点击会打到别的窗口上）：" + ToUtf8Safe(aerr);
            return false;
        }
        if (GetForegroundWindow() != hwnd) {
            // 抢不到前台就别点：点了也不知道点到了谁
            why = "客户端拿不到前台（多半被本软件自己的浮窗/其它置顶窗挡着）⇒ "
                  "几何点击会打到那个窗口上，已中止（不猜落点）";
            return false;
        }
        Sleep(120);
    }
    Snapshot snap;
    if (!SnapshotImpl(hwnd, 400, snap)) {
        why = "UIA 快照失败（窗口可能已关闭）";
        return false;
    }
    const std::wstring prev = GetClipboardTextW();
    // 定位顺序：① UIA 名字/id 命中「复制」（少数客户端会给出名字）；
    //           ② **操作栏那一排的最左一颗**（首选 —— 见 PickWindowAiCopyRowButton 的说明）；
    //           ③ 配置的固定矩形；④ 锚点 + 偏移（几何推算，最后手段）。
    int limitY = client.top + static_cast<int>((client.bottom - client.top) * 0.85);
    {
        RECT inRect{};
        if (ResolveRelativeRect(profile.input.rectHint, client, inRect) && inRect.top > client.top) {
            limitY = inRect.top - 4;
        }
    }
    int idx = PickWindowAiCopyButton(profile.reply, snap.nodes, client);
    if (idx < 0) {
        idx = PickWindowAiCopyRowButton(profile.reply, snap.nodes, client, limitY);
        if (idx >= 0) {
            windowmode::WindowModeLogEventf(
                L"[窗口AI] 复制按钮：按「操作栏那一排的最左一颗」定位 → %s",
                DescribeNode(snap.nodes[static_cast<size_t>(idx)]).c_str());
        }
    }
    // ★★ **软悬停让操作栏出现**（2026-09-28；用户要求"彻底绕开 OCR"的关键一步）
    //
    //   为什么必须悬停：消息操作栏（复制/朗读/赞/踩…）**是 hover 才渲染的** ——
    //   不悬停，UIA 树里根本没有那一排（实测：193 个节点的树里找不到）。
    //   悬停本身走**软光标**（窗口模式会话 ⇒ 进程内队列）：不动物理鼠标、不抢前台。
    if (idx < 0 && softHover_) {
        int bottomLimit = client.bottom;
        {
            RECT inRect{};
            if (ResolveRelativeRect(profile.input.rectHint, client, inRect) && inRect.top > client.top) {
                bottomLimit = inRect.top - 4;
            } else {
                bottomLimit = client.top + static_cast<int>((client.bottom - client.top) * 0.85);
            }
        }
        const int cw = (std::max)(1, static_cast<int>(client.right - client.left));
        const int chh = (std::max)(1, static_cast<int>(client.bottom - client.top));
        const double hxRel = profile.reply.copyButtonXRel;
        // ⚠ **不依赖锚点**：锚点要从 UIA/OCR 的正文里推，而几何模式下正文常常读不到
        //   ⇒ 直接"沿着输入框往上"扫几档（最新那条回答就在输入框正上方）。
        std::vector<int> ys;
        for (int dy : { 40, 80, 130, 190, 260 }) {
            const int y = bottomLimit - dy;
            if (y > client.top + 20) ys.push_back(y);
        }
        // 有锚点就用锚点下方那几档（更准），没有就只靠上面那几档
        RECT anchor{};
        if (PickWindowAiCopyAnchor(profile.reply, snap.nodes, client, prompt, bottomLimit, anchor)) {
            for (double dy : { 0.020, 0.034, 0.050 }) {
                ys.insert(ys.begin(), anchor.bottom + static_cast<int>(dy * chh));
            }
        }
        for (int sy : ys) {
            const int sx = client.left + static_cast<int>(hxRel * cw);
            if (!softHover_(sx - client.left, sy - client.top)) break;
            Sleep(280);   // 给客户端渲染操作栏的时间
            Snapshot hovered;
            if (!SnapshotImpl(hwnd, 400, hovered)) continue;
            const int r = PickWindowAiCopyRowButton(profile.reply, hovered.nodes, client, limitY);
            if (r >= 0) {
                windowmode::WindowModeLogEventf(
                    L"[窗口AI] 软悬停(%d,%d)后发现操作栏：树 %d 个控件 → 最左一颗 = %s",
                    sx, sy, static_cast<int>(hovered.nodes.size()),
                    DescribeNode(hovered.nodes[static_cast<size_t>(r)]).c_str());
                snap = std::move(hovered);
                idx = r;
                clipboardDiag_ += "软悬停后发现操作栏（落点=" + std::to_string(sx) + ","
                    + std::to_string(sy) + "）";
                break;
            }
        }
    }
    // ★★ 落点**遮挡校验**：几何点击是屏幕坐标，置顶窗口（实测：本软件自己的
    //   「AI 脚本助手」浮窗就是 topmost）**即使不是前台也会吃掉点击** ⇒
    //   点下去看着成功、剪贴板却不变，而回执只能说"没点中"。
    //   判据同 `locateAndClick`：`WindowFromPoint` 取该点所属顶层窗口，必须是目标窗口。
    auto clickIfOnTarget = [&](int x, int y, std::string& failWhy) -> bool {
        POINT pt{ x, y };
        HWND under = WindowFromPoint(pt);
        if (under) {
            HWND root = GetAncestor(under, GA_ROOT);
            if (root && root != hwnd) {
                wchar_t cls[128] = {};
                GetClassNameW(root, cls, 128);
                wchar_t title[256] = {};
                GetWindowTextW(root, title, 256);
                failWhy = "落点(" + std::to_string(x) + "," + std::to_string(y) + ")被**别的窗口挡住**：「"
                    + ToUtf8Safe(title) + "」(" + ToUtf8Safe(cls) + ") ⇒ 点击会打到它身上，已中止";
                return false;
            }
        }
        ClickPoint(x, y);
        return true;
    };
    bool clicked = false;
    // ★ 标定模式：直接用给定的屏幕坐标（跳过"锚点+偏移"的推算）
    if (overrideX >= 0 && overrideY >= 0) {
        std::string blocked;
        if (!clickIfOnTarget(overrideX, overrideY, blocked)) {
            why = blocked;
            return false;
        }
        clicked = true;
        clipboardDiag_ = "标定落点=(" + std::to_string(overrideX) + ","
            + std::to_string(overrideY) + ")";
    }
    if (!clicked && idx >= 0) {
        IUIAutomationElement* el = snap.els[static_cast<size_t>(idx)].Get();
        const auto& rc = snap.nodes[static_cast<size_t>(idx)].rect;
        // 优先 InvokePattern（不移动鼠标、不受遮挡影响）
        ComPtr<IUIAutomationInvokePattern> inv;
        if (el && SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId,
                IID_PPV_ARGS(inv.GetAddressOf()))) && inv) {
            clicked = SUCCEEDED(inv->Invoke());
            if (clicked) {
                clipboardDiag_ = "UIA-Invoke: " + ToUtf8Safe(DescribeNode(snap.nodes[static_cast<size_t>(idx)]));
                windowmode::WindowModeLogEventf(L"[窗口AI] 复制按钮（Invoke）：%s",
                    DescribeNode(snap.nodes[static_cast<size_t>(idx)]).c_str());
            }
        }
        if (!clicked && rc.right > rc.left && rc.bottom > rc.top) {
            std::string blocked;
            const int cx = (rc.left + rc.right) / 2;
            const int cy = (rc.top + rc.bottom) / 2;
            if (!clickIfOnTarget(cx, cy, blocked)) {
                why = blocked;
                return false;
            }
            clicked = true;
            clipboardDiag_ = "UIA-点击: " + ToUtf8Safe(DescribeNode(snap.nodes[static_cast<size_t>(idx)]));
            windowmode::WindowModeLogEventf(L"[窗口AI] 复制按钮（点击）：%s",
                DescribeNode(snap.nodes[static_cast<size_t>(idx)]).c_str());
        }
    }
    if (!clicked) {
        RECT rel{};
        if (ResolveRelativeRect(profile.reply.copyButtonHint, client, rel)) {
            std::string blocked;
            const int cx = (rel.left + rel.right) / 2;
            const int cy = (rel.top + rel.bottom) / 2;
            if (!clickIfOnTarget(cx, cy, blocked)) {
                why = blocked;
                return false;
            }
            clicked = true;
            clipboardDiag_ = "几何矩形: 落点=(" + std::to_string(cx) + "," + std::to_string(cy) + ")";
            windowmode::WindowModeLogEventf(L"[窗口AI] 复制按钮（几何矩形）");
        }
    }
    if (!clicked && profile.reply.copyAnchorFallback) {
        // ★★ 几何锚点兜底（Chromium 客户端实测：308 个节点里**一个「复制」都没有**，
        //    消息操作栏不进 UIA）⇒ 用"回答最后一行文本"的位置推它：
        //    锚点左下角 + 偏移比例 = 复制图标的位置。
        RECT anchor{};
        // 锚点的下界 = **输入框顶边上方**（否则会锚到输入区自己的控件/附件菜单 —— 实测踩到）
        int bottomLimit = client.bottom;
        {
            RECT inRect{};
            if (ResolveRelativeRect(profile.input.rectHint, client, inRect) && inRect.top > client.top) {
                bottomLimit = inRect.top - 4;
            } else {
                bottomLimit = client.top + static_cast<int>((client.bottom - client.top) * 0.85);
            }
        }
        if (PickWindowAiCopyAnchor(profile.reply, snap.nodes, client, prompt, bottomLimit, anchor)) {
            const int cw = (std::max)(1, static_cast<int>(client.right - client.left));
            const int chh = (std::max)(1, static_cast<int>(client.bottom - client.top));
            const double dx = profile.reply.copyButtonOffset.size() == 2
                ? profile.reply.copyButtonOffset[0] : -0.006;
            const double dy = profile.reply.copyButtonOffset.size() == 2
                ? profile.reply.copyButtonOffset[1] : 0.024;
            // ★ 横向基准 = **会话列左缘 + copyButtonXRel**（不是锚点文本的左缘：
            //   操作栏贴列左缘排，而正文有缩进/居中 ⇒ 按文本左缘点会偏出几百像素）
            const int px = client.left + static_cast<int>(profile.reply.copyButtonXRel * cw)
                + static_cast<int>(dx * cw);
            const int py = anchor.bottom + static_cast<int>(dy * chh);
            std::string blocked;
            // ⚠ 后台模式**不做几何点击兜底**：那是**真实鼠标点击**（屏幕坐标）⇒ 必然抢前台，
            //   正是用户不接受的那件事。这里如实放弃，交给上层的 OCR 兜底。
            if (backgroundMode) {
                why = "后台模式下不走几何点击兜底（真鼠标点击会抢前台）⇒ 本轮改走 OCR 读回答";
                return false;
            }
            if (!clickIfOnTarget(px, py, blocked)) {
                why = blocked;
                return false;
            }
            clicked = true;
            clipboardDiag_ = "几何锚点: 锚点=[" + std::to_string(anchor.left) + ","
                + std::to_string(anchor.top) + "," + std::to_string(anchor.right) + ","
                + std::to_string(anchor.bottom) + "] 落点=(" + std::to_string(px) + ","
                + std::to_string(py) + ") 客户区=[" + std::to_string(client.left) + ","
                + std::to_string(client.top) + "," + std::to_string(client.right) + ","
                + std::to_string(client.bottom) + "]";
            windowmode::WindowModeLogEventf(
                L"[窗口AI] 复制按钮（几何锚点）：锚点=[%d,%d,%d,%d] → 落点=(%d,%d)",
                static_cast<int>(anchor.left), static_cast<int>(anchor.top),
                static_cast<int>(anchor.right), static_cast<int>(anchor.bottom), px, py);
        }
    }
    if (!clicked) {
        why = "UIA 里没有「复制」按钮（Chromium 常常不给消息操作栏），"
              "且配置里没有可用的 copyButtonHint";
        return false;
    }

    // 等剪贴板变化
    std::wstring got;
    const long long deadline = NowMs() + (std::max)(200, profile.reply.clipboardWaitMs);
    std::string lastWhy;
    for (;;) {
        const std::wstring now = GetClipboardTextW();
        std::string w;
        if (WindowAiClipboardReplyUsable(prev, now, prompt, profile.reply.maxChars, w)) {
            got = now;
            break;
        }
        lastWhy = w;
        if (NowMs() >= deadline) break;
        Sleep(120);
    }
    if (got.empty()) {
        why = lastWhy.empty() ? "复制按钮点下去了但剪贴板没变" : lastWhy;
        return false;
    }
    // ★再读一次确认稳定：生成中复制会拿到**半截**回答（与"两次读数相同"同一条纪律）
    Sleep(800);
    const std::wstring again = GetClipboardTextW();
    if (!again.empty() && again != got) {
        std::string w2;
        if (WindowAiClipboardReplyUsable(prev, again, prompt, profile.reply.maxChars, w2)) {
            got = again;   // 用更完整的那次
        }
    }
    out = got;
    if (profile.reply.maxChars > 0 && static_cast<int>(out.size()) > profile.reply.maxChars) {
        out.resize(static_cast<size_t>(profile.reply.maxChars));
    }
    // ★借了用户的剪贴板就还回去（读到的内容已经在我们手里了）
    if (profile.reply.restoreClipboard && !prev.empty()) {
        SetClipboardTextW(prev);
    }
    return true;
}

int WindowAiDriver::FindInput(const WindowAiClientProfile& profile, HWND hwnd,
    std::vector<WindowAiUiNode>& nodes, std::string& err) {
    err.clear();
    Snapshot snap;
    if (!SnapshotImpl(hwnd, 400, snap)) {
        err = "UIA 快照失败";
        return -1;
    }
    nodes = snap.nodes;
    const RECT client = ClientRectOnScreen(hwnd);
    const int idx = PickWindowAiInput(profile.input, snap.nodes, client);
    if (idx < 0) {
        err = "没找到符合判据的输入框（" + std::to_string(snap.nodes.size())
            + " 个控件里没有合格的；用探针 dump 调 window_ai_providers.json）";
        return -1;
    }
    return idx;
}

nlohmann::json WindowAiDriver::ProbeReport(const std::string& clientId, bool includeControls) {
    // ★ 探针是**诊断**路径：每次都重读一遍配置 —— 否则改了 json 却还在用旧档案，
    //   表现为"配置明明改了、落点还是老样子"（实测踩到：改了 copyButtonXRel 无效）。
    ReloadClients();
    json rep;
    rep["clientId"] = clientId;
    const auto& clients = Clients();
    const int ci = FindWindowAiClient(clients, clientId);
    if (ci < 0) {
        rep["ok"] = false;
        rep["error"] = "配置里没有这个客户端 id";
        json ids = json::array();
        for (const auto& c : clients) ids.push_back(c.id);
        rep["knownClients"] = ids;
        return rep;
    }
    const WindowAiClientProfile& profile = clients[static_cast<size_t>(ci)];
    rep["profile"] = json::parse(FormatWindowAiProfile(profile), nullptr, false);

    json cands = json::array();
    for (const auto& w : ListCandidateWindows()) {
        json c;
        c["process"] = ToUtf8Safe(w.processName);
        c["title"] = ToUtf8Safe(w.title);
        c["foreground"] = w.foreground;
        c["minimized"] = w.minimized;
        c["rect"] = json::array({w.windowRect.left, w.windowRect.top,
                                 w.windowRect.right, w.windowRect.bottom});
        bool matched = WindowAiWindowMatches(profile, w.processName, w.title);
        const int ww = w.windowRect.right - w.windowRect.left;
        const int wh = w.windowRect.bottom - w.windowRect.top;
        c["matched"] = matched;
        // ★ 标出"匹配但太小"的窗口：这类小工具窗是**实测踩过的坑**
        //   （Cursor 有个 158x26 的离屏同名窗，按首个匹配选就会对着它打字）
        c["tooSmall"] = matched && (ww < profile.minWindowWidth || wh < profile.minWindowHeight);
        cands.push_back(std::move(c));
    }
    rep["candidates"] = cands;
    rep["minWindowSize"] = json::array({profile.minWindowWidth, profile.minWindowHeight});

    WindowAiTarget target;
    std::string err;
    if (!FindClientWindow(profile, target, err)) {
        rep["ok"] = false;
        rep["findError"] = err;
        return rep;
    }
    rep["target"] = json{{"process", ToUtf8Safe(target.processName)},
                         {"title", ToUtf8Safe(target.title)},
                         {"foreground", target.foreground},
                         {"restoredFromHidden", target.restoredFromHidden},
                         {"movedToCurrentDesktop", target.movedToCurrentDesktop},
                         {"hwnd", reinterpret_cast<unsigned long long>(target.hwnd)}};

    Snapshot snap;
    if (!SnapshotImpl(target.hwnd, 400, snap)) {
        rep["ok"] = false;
        rep["error"] = "UIA 快照失败";
        return rep;
    }
    const RECT client = ClientRectOnScreen(target.hwnd);
    rep["clientArea"] = json{{"left", client.left}, {"top", client.top},
                             {"right", client.right}, {"bottom", client.bottom}};
    rep["controlCount"] = static_cast<int>(snap.nodes.size());

    // ★ 剪贴板通道诊断：**「复制」按钮到底在不在 UIA 树里**、名字/矩形是什么。
    //   这是校准 `reply.copyButtonHint` 的唯一依据 —— 拿不到按钮时，用户至少知道
    //   该往 window_ai_providers.json 里填哪个相对矩形（或确认该客户端确实不暴露操作栏）。
    {
        json clip;
        clip["useClipboard"] = profile.reply.useClipboard;
        clip["restoreClipboard"] = profile.reply.restoreClipboard;
        clip["hintConfigured"] = !profile.reply.copyButtonHint.empty();
        const int cbIdx = PickWindowAiCopyButton(profile.reply, snap.nodes, client);
        clip["copyButtonFound"] = cbIdx >= 0;
        if (cbIdx >= 0) {
            const auto& n = snap.nodes[static_cast<size_t>(cbIdx)];
            const double cw = (std::max)(1, static_cast<int>(client.right - client.left));
            const double chh = (std::max)(1, static_cast<int>(client.bottom - client.top));
            clip["picked"] = json{
                {"type", ToUtf8Safe(n.controlType)},
                {"name", ToUtf8Safe(TruncW(n.name, 60))},
                {"automationId", ToUtf8Safe(TruncW(n.automationId, 60))},
                {"rect", json::array({n.rect.left, n.rect.top, n.rect.right, n.rect.bottom})},
                {"rel", json::array({(n.rect.left - client.left) / cw,
                                     (n.rect.top - client.top) / chh,
                                     (n.rect.right - client.left) / cw,
                                     (n.rect.bottom - client.top) / chh})}};
        } else {
            clip["why"] = "UIA 树里没有名字/id 命中「复制」的按钮（Chromium 常常不给消息操作栏）"
                          "⇒ 要么在 window_ai_providers.json 里配 reply.copyButtonHint"
                          "（相对客户区 0..1 的 l,t,r,b），要么该客户端只能退回 UIA/OCR 读文本";
        }
        json btns = json::array();
        for (const auto& n : snap.nodes) {
            if (n.controlType != L"Button" && n.controlType != L"Hyperlink") continue;
            if (n.Width() < 8 || n.Height() < 8) continue;
            if (btns.size() >= 40) break;
            btns.push_back(json{{"type", ToUtf8Safe(n.controlType)},
                                {"name", ToUtf8Safe(TruncW(n.name, 40))},
                                {"automationId", ToUtf8Safe(TruncW(n.automationId, 40))},
                                {"rect", json::array({n.rect.left, n.rect.top, n.rect.right,
                                                      n.rect.bottom})}});
        }
        clip["buttons"] = btns;
        rep["clipboard"] = clip;
    }
    // ★★ 诊断的**触发条件 = 定位不到输入框**（而不是"树是否为空"）。
    //   理由（实测 2026-09-26）：豆包客户端的树里有 178 个节点、看着很"丰满"，
    //   但**没有一个能当输入框**（唯一带 ValuePattern+TextPattern 的是整页 `RootWebArea`），
    //   而它的底部输入区**一个 UIA 节点都没有**。
    //   ⇒ 只有"候选为空"才是真问题；按节点数判断会让探针跳过 OCR dump，
    //     用户拿不到任何"输入框在哪"的线索（方向全错）。
    const bool noInputCandidate = (PickWindowAiInput(profile.input, snap.nodes, client) < 0);
    if (snap.nodes.empty() || noInputCandidate) {
        rep["uiaUsable"] = false;
        rep["uiaDiagnosis"] =
            std::string("UIA ") + (snap.nodes.empty() ? "树为空" : "里没有一个能当输入框的控件")
            + "（共 " + std::to_string(snap.nodes.size()) + " 个节点）—— Chromium 系客户端"
            "（豆包客户端、Cursor、VS Code、多数国产客户端）**默认不把页面内容交给 UIA**，"
            "不是判据写错了。这类客户端两条路：① 客户端里打开无障碍（VS Code/Cursor："
            "editor.accessibilitySupport = on）；② ★**几何 + OCR**：在 window_ai_providers.json 里"
            "给该客户端填 input.rectHint（相对客户区 0..1 的 l,t,r,b）与 reply.ocrRegion —— "
            "本报告 ocrDump.lines[].rel 就是量它们的依据。";
        rep["ocrDumpHint"] = "ocrDump.lines[].rel = [l,t,r,b]（相对客户区）";
        // 整窗 OCR（带行框）：没有 UIA 时**唯一**能看到"窗口里有什么字、在哪儿"的手段
        RECT cc{};
        GetClientRect(target.hwnd, &cc);
        windowmode::WindowCaptureResult cap = windowmode::CaptureWindowRegion(target.hwnd,
            0, 0, cc.right, cc.bottom);
        if (cap.bitmap) {
            const OcrEngineOutput ocr = RunWinRtOcr(cap.bitmap, false);
            DeleteObject(cap.bitmap);
            const int cw = (std::max)(1, static_cast<int>(cc.right));
            const int chh = (std::max)(1, static_cast<int>(cc.bottom));
            json lines = json::array();
            std::wstring all;
            int n = 0;
            for (const auto& l : ocr.lines) {
                if (l.text.empty()) continue;
                all += l.text;
                all += L"\n";
                if (++n > 80) continue;
                lines.push_back(json{
                    {"text", ToUtf8Safe(TruncW(l.text, 60))},
                    {"rel", json::array({static_cast<double>(l.x1) / cw,
                                         static_cast<double>(l.y1) / chh,
                                         static_cast<double>(l.x2) / cw,
                                         static_cast<double>(l.y2) / chh})}});
            }
            rep["ocrDump"] = json{{"chars", static_cast<int>(all.size())},
                                  {"head", ToUtf8Safe(TruncW(all, 400))},
                                  {"lines", lines}};
        } else {
            rep["ocrDumpError"] = "截取窗口位图失败";
        }
    }

    // 输入框候选（全部合格项 + 分数），这是调参的主要依据
    json inputCands = json::array();
    for (const auto& n : snap.nodes) {
        const int score = ScoreWindowAiInput(profile.input, n, client);
        if (score < 0) continue;
        inputCands.push_back(json{{"score", score},
                                  {"type", ToUtf8Safe(n.controlType)},
                                  {"name", ToUtf8Safe(TruncW(n.name, 60))},
                                  {"automationId", ToUtf8Safe(TruncW(n.automationId, 40))},
                                  {"rect", json::array({n.rect.left, n.rect.top, n.rect.right, n.rect.bottom})},
                                  {"focusable", n.focusable},
                                  {"valuePattern", n.valuePattern},
                                  {"textPattern", n.textPattern},
                                  {"keyboardFocus", n.keyboardFocus}});
    }
    rep["inputCandidates"] = inputCands;
    const int pick = PickWindowAiInput(profile.input, snap.nodes, client);
    rep["inputPickIndex"] = pick;
    if (pick >= 0) {
        const auto& n = snap.nodes[static_cast<size_t>(pick)];
        rep["inputPick"] = json{{"type", ToUtf8Safe(n.controlType)},
                                {"name", ToUtf8Safe(TruncW(n.name, 60))},
                                {"valuePattern", n.valuePattern},
                                {"textPattern", n.textPattern},
                                {"keyboardFocus", n.keyboardFocus},
                                {"rect", json::array({n.rect.left, n.rect.top, n.rect.right, n.rect.bottom})}};
    }

    if (includeControls) {
        json arr = json::array();
        const int cap = 120;
        for (int i = 0; i < static_cast<int>(snap.nodes.size()) && i < cap; ++i) {
            const auto& n = snap.nodes[i];
            arr.push_back(json{{"type", ToUtf8Safe(n.controlType)},
                               {"name", ToUtf8Safe(TruncW(n.name, 70))},
                               {"automationId", ToUtf8Safe(TruncW(n.automationId, 40))},
                               {"value", ToUtf8Safe(TruncW(n.value, 80))},
                               {"rect", json::array({n.rect.left, n.rect.top, n.rect.right, n.rect.bottom})},
                               {"focusable", n.focusable},
                               {"valuePattern", n.valuePattern},
                               {"textPattern", n.textPattern},
                               {"offscreen", n.offscreen}});
        }
        rep["controls"] = arr;
        rep["controlsTruncated"] = static_cast<int>(snap.nodes.size()) > cap;
    }

    // 会话文本（只读）
    std::wstring conv;
    std::string cerr;
    if (ReadConversation(profile, target.hwnd, conv, cerr)) {
        rep["conversation"] = json{{"chars", static_cast<int>(conv.size())},
                                   {"head", ToUtf8Safe(TruncW(conv, 300))},
                                   {"tail", ToUtf8Safe(conv.size() > 300
                                        ? conv.substr(conv.size() - 300) : conv)}};
    } else {
        rep["conversationError"] = cerr;
    }
    rep["ok"] = true;
    return rep;
}

// ──────────────────────────────────────────────────────────────────
// 端到端：写进去 + 提交 + 等新回答
// ──────────────────────────────────────────────────────────────────
bool WindowAiDriver::StartNewConversation(const WindowAiClientProfile& profile, HWND hwnd,
    std::string& err) {
    err.clear();
    Snapshot snap;
    if (!SnapshotImpl(hwnd, 400, snap)) {
        err = "UIA 快照失败";
        return false;
    }
    // 找「新对话」按钮 —— 用 **Invoke**（后台可用，不需要前台、不动鼠标）
    for (size_t i = 0; i < snap.nodes.size(); ++i) {
        const auto& n = snap.nodes[i];
        if (n.controlType != L"Button" && n.controlType != L"Hyperlink") continue;
        if (!n.enabled || n.offscreen) continue;
        bool hit = false;
        for (const auto& want : profile.newConversationButtonNames) {
            if (!want.empty() && ContainsNoCaseW(n.name, want)) { hit = true; break; }
        }
        if (!hit) continue;
        ComPtr<IUIAutomationInvokePattern> inv;
        if (snap.els[i] && SUCCEEDED(snap.els[i]->GetCurrentPatternAs(UIA_InvokePatternId,
                IID_PPV_ARGS(inv.GetAddressOf()))) && inv) {
            if (SUCCEEDED(inv->Invoke())) {
                Sleep(700);   // 等界面切过去（新对话是瞬时的，留一点余量）
                windowmode::WindowModeLogEventf(L"[窗口AI] 已开新对话（Invoke「%s」）",
                    n.name.c_str());
                return true;
            }
        }
        if (n.rect.right > n.rect.left) {
            // 没有 InvokePattern 时才退化成点击，且**用后台软点击**（不抢前台）
            windowmode::PostMouseButtonToWindow(hwnd, (n.rect.left + n.rect.right) / 2 - 0,
                (n.rect.top + n.rect.bottom) / 2 - 0, MouseButtonType::Left, true);
            windowmode::PostMouseButtonToWindow(hwnd, (n.rect.left + n.rect.right) / 2 - 0,
                (n.rect.top + n.rect.bottom) / 2 - 0, MouseButtonType::Left, false);
            Sleep(700);
            windowmode::WindowModeLogEventf(L"[窗口AI] 已开新对话（后台软点击「%s」）",
                n.name.c_str());
            return true;
        }
    }
    err = "找不到「新对话」按钮（找了 ";
    for (size_t k = 0; k < profile.newConversationButtonNames.size(); ++k) {
        if (k) err += " / ";
        err += ToUtf8Safe(profile.newConversationButtonNames[k]);
    }
    err += "）⇒ **不在当前对话里硬写**（那会把新会话混进历史对话，正是串台的来源）。"
           "请在 window_ai_providers.json 里把该客户端的新对话按钮名配到 "
           "`newConversationButtonNames`。";
    return false;
}

WindowAiSendOutcome WindowAiDriver::SendAndRead(const std::string& clientId,
    const std::wstring& text, int idleTimeoutMs, int maxTotalMs, bool newConversation) {
    WindowAiSendOutcome out;
    if (text.empty()) {
        out.error = "EMPTY_PROMPT";
        out.message = "要写进窗口的文本为空";
        return out;
    }
    const auto& clients = Clients();
    const int ci = FindWindowAiClient(clients, clientId);
    if (ci < 0) {
        out.error = "NO_CLIENT_PROFILE";
        out.message = "配置里没有客户端 id：" + clientId;
        return out;
    }
    const WindowAiClientProfile profile = clients[static_cast<size_t>(ci)];
    const int idleMs = idleTimeoutMs > 0 ? idleTimeoutMs : profile.idleTimeoutMs;
    const int totalMs = maxTotalMs > 0 ? maxTotalMs : profile.maxTotalMs;

    // ① 找窗
    long long t = NowMs();
    if (!FindClientWindow(profile, out.target, out.error)) {
        out.message = out.error;
        out.error = "NO_CLIENT_WINDOW";
        return out;
    }
    out.findMs = static_cast<int>(NowMs() - t);

    const bool backgroundInput = (profile.inputMethod != L"foreground");
    // ★ 会话**活到整轮结束**：回复读取阶段要用它的软光标做悬停（让操作栏出现）
    WmInputSession wmSession;

    // ② 前台化 —— **默认不做**（走后台灌键）
    //
    // ⚠⚠ 用户原话：「抢占前台…不然的话用户问个问题电脑都用不了了」。
    //   ⇒ 默认 `inputMethod=background`：不激活、不动鼠标，走窗口模式那套后台灌键
    //     （`PostQuickInputToWindow` + `PostKeyToWindow`，注释写明适用于 Chromium 壳）。
    //   `foreground` 只在用户显式配置时才用（老行为：抢前台 + SendInput）。
    t = NowMs();
    if (!backgroundInput) {
        std::wstring aerr;
        if (!windowmode::ActivateWindow(out.target.hwnd, aerr)) {
            out.error = "ACTIVATE_FAILED";
            out.message = ToUtf8Safe(aerr.empty() ? L"无法把客户端切到前台" : aerr);
            return out;
        }
        Sleep(120);  // 给窗口一点时间完成重绘/取焦点
        out.target.foreground = (GetForegroundWindow() == out.target.hwnd);
    }
    out.activateMs = static_cast<int>(NowMs() - t);

    // ②.5 **新对话**（对应网页端的 `plan.newConversation`）
    //   ⚠ 必须在取基线之前做：换了对话，基线就完全不同了。
    if (newConversation) {
        std::string nerr;
        if (!StartNewConversation(profile, out.target.hwnd, nerr)) {
            out.error = "NEW_CONVERSATION_FAILED";
            out.message = nerr;
            return out;
        }
    }

    // ③ 基线会话文本（**必须先取**：否则会把上一轮回答当新回答 —— 与网页端同一条纪律）
    std::wstring baseline;
    {
        std::string cerr;
        if (!ReadConversation(profile, out.target.hwnd, baseline, cerr)) {
            // 读不到不算致命：客户端可能还没启用无障碍。如实记账，继续试。
            windowmode::WindowModeLogEventf(L"[窗口AI] 基线读取失败：%s",
                FromUtf8Safe(cerr).c_str());
        }
    }
    out.baselineText = baseline;
    out.baselineChars = static_cast<int>(baseline.size());

    // ④ 定位输入框 + 聚焦
    t = NowMs();
    Snapshot snap;
    if (!SnapshotImpl(out.target.hwnd, 400, snap)) {
        out.error = "UIA_SNAPSHOT_FAILED";
        out.message = "UIA 快照失败（窗口可能已关闭）";
        return out;
    }
    const RECT client = ClientRectOnScreen(out.target.hwnd);
    // ★★ **写入前的最后一道闸**：走剪贴板通道时，必须能定位到"消息操作栏那一排"。
    //   定位不到 ⇒ 客户端此刻**没在渲染**（Chromium 后台/隐藏时不给操作栏），
    //   ⇒ 现在失败只要 1 秒，而放过去会：往用户会话里塞一条消息 + **白等满 180 秒**
    //     （实测就这么烧掉一整轮，回执还只说 NO_REPLY，看不出真正原因）。
    if (profile.reply.useClipboard) {
        int limitY = client.top + static_cast<int>((client.bottom - client.top) * 0.85);
        {
            RECT inRect{};
            if (ResolveRelativeRect(profile.input.rectHint, client, inRect)
                && inRect.top > client.top) {
                limitY = inRect.top - 4;
            }
        }
        const bool rowFound = PickWindowAiCopyRowButton(profile.reply, snap.nodes, client, limitY) >= 0;
        // ⚠ 闸的判据要盯**真正要防的那件事**：客户端**根本没渲染**（Chromium 后台/隐藏时
        //   只给 6~14 个节点：框架 + 最小化/最大化/关闭）。渲染出来的页面控件多得多
        //   （实测登录后的豆包 193 个）。
        //   ⚠ 不能拿"操作栏找不找得到"当渲染判据：操作栏只挂在**最新那条回答**上，
        //     而且不一定进 UIA —— 用它当闸会把"能跑"的情况误拦成 WINDOW_NOT_RENDERED
        //     （实测就这样误拦过一次）。
        const bool looksRendered = snap.nodes.size() >= 40;
        if (!rowFound && !looksRendered) {
            out.error = "WINDOW_NOT_RENDERED";
            out.message = "目标窗口只有 " + std::to_string(snap.nodes.size())
                + " 个控件（框架级）⇒ 客户端此刻没在正常渲染"
                  "（Chromium 系客户端只在前台渲染时才把页面交给 UIA）。"
                  "已**在写入之前**中止，避免往你的会话里塞消息后白等。"
                  "请把该客户端窗口切到前台、点一下让它渲染，再重试。";
            windowmode::WindowModeLogEventf(
                L"[窗口AI] 未写入即中止：窗口树只有 %d 个节点（未渲染）",
                static_cast<int>(snap.nodes.size()));
            return out;
        }
        if (!rowFound) {
            // 渲染正常但操作栏这一轮拿不到 ⇒ **如实降级**：回答改走 OCR 读
            windowmode::WindowModeLogEventf(
                L"[窗口AI] 操作栏没找到（树 %d 个节点）⇒ 本轮回答只能走 OCR 读",
                static_cast<int>(snap.nodes.size()));
        }
    }
    const int inputIdx = PickWindowAiInput(profile.input, snap.nodes, client);
    bool geometryMode = false;
    RECT geometryRect{};
    if (inputIdx < 0) {
        // ★ 几何兜底：UIA 树为空（Electron/Chromium 默认如此）时，
        //   按配置里的相对矩形点下去。⚠ 回执必须标 `geometry-ocr`，
        //   因为这条路的定位精度取决于**用户配的那个矩形**，与 UIA 定位不是一个可信度。
        RECT rel{};
        if (ResolveRelativeRect(profile.input.rectHint, client, rel)) {
            geometryMode = true;
            geometryRect = rel;
        } else {
            out.error = "NO_INPUT";
            out.message = "没找到输入框（" + std::to_string(snap.nodes.size())
                + " 个控件里没有合格的），且配置里没有可用的 rectHint 几何兜底。";
            return out;
        }
    }
    IUIAutomationElement* inputEl = geometryMode
        ? nullptr : snap.els[static_cast<size_t>(inputIdx)].Get();
    out.mode = geometryMode ? L"geometry-ocr" : L"uia";
    out.inputMatchedBy = geometryMode
        ? (L"相对矩形[" + std::to_wstring(profile.input.rectHint[0]) + L"," +
           std::to_wstring(profile.input.rectHint[1]) + L"," +
           std::to_wstring(profile.input.rectHint[2]) + L"," +
           std::to_wstring(profile.input.rectHint[3]) + L"]")
        : DescribeNode(snap.nodes[static_cast<size_t>(inputIdx)]);

    bool focusOk = false;
    if (backgroundInput) {
        // ★ 后台灌键：**不需要焦点、不需要前台、不动鼠标** ⇒ 跳过整段聚焦逻辑。
        focusOk = true;
        out.write.tried.push_back(L"background: 不聚焦、不抢前台（走窗口模式的后台灌键）");
    }
    if (!backgroundInput && geometryMode) {
        // 几何模式：没有 UIA 元素可问焦点 ⇒ 只能"点下去"，且**无法核实**焦点。
        // ⚠ 这里如实标 note；调用方（回执）必须把 `geometry-ocr` 与"焦点未核实"一起说出去。
        ClickPoint((geometryRect.left + geometryRect.right) / 2,
            (geometryRect.top + geometryRect.bottom) / 2);
        Sleep(200);
        focusOk = true;   // 「已尝试聚焦」：几何模式没有可核实的判据，不许假装核实过
        out.write.tried.push_back(L"geometry: 已点击相对矩形中心（焦点无法核实）");
    }
    for (int attempt = 0; attempt < 3 && !backgroundInput && !focusOk; ++attempt) {
        if (inputEl && SUCCEEDED(inputEl->SetFocus())) {
            Sleep(80);
            VARIANT v{};
            if (SUCCEEDED(inputEl->GetCurrentPropertyValue(UIA_HasKeyboardFocusPropertyId, &v))) {
                focusOk = (v.vt == VT_BOOL && v.boolVal == VARIANT_TRUE);
                VariantClear(&v);
            }
        }
        if (!focusOk) {
            // 兜底：真点一下输入框中心（UIA SetFocus 对 Electron 常常无效）
            const RECT rc = geometryMode ? geometryRect
                                         : snap.nodes[static_cast<size_t>(inputIdx)].rect;
            if (rc.right > rc.left && rc.bottom > rc.top) {
                ClickPoint((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
                Sleep(150);
                VARIANT v2{};
                if (inputEl
                    && SUCCEEDED(inputEl->GetCurrentPropertyValue(UIA_HasKeyboardFocusPropertyId, &v2))) {
                    focusOk = (v2.vt == VT_BOOL && v2.boolVal == VARIANT_TRUE);
                    VariantClear(&v2);
                }
            }
        }
    }
    out.focusMs = static_cast<int>(NowMs() - t);
    if (!focusOk) {
        out.error = "FOCUS_FAILED";
        out.message = "输入框拿不到键盘焦点（UIA SetFocus 与点击都失败）——"
                      "这时打字会打到别的窗口，已中止";
        return out;
    }

    // ⑤ 写入
    //
    // ⚠⚠ **绝不用 Ctrl+A 清空**：像 Cursor 那种"编辑器 + 聊天面板"共存的客户端里，
    //   Ctrl+A 可能选中**整个代码文件**，随后的删除会**毁掉用户的代码**。
    //   取而代之：先读输入框里已有的内容 ——
    //     · 空 ⇒ 直接写（正常路径，聊天框本来就是空的）
    //     · 非空 ⇒ 有草稿，**中止并如实回报**（`INPUT_NOT_EMPTY`），让用户自己清
    //   「宁可这次不做事，也不破坏用户已有的输入」。
    t = NowMs();

    // ★★ 后台灌键通道（**默认**）：`PostQuickInputToWindow` + `PostKeyToWindow`
    //   —— 不激活窗口、不动鼠标、不抢前台（用户明确要求）。
    //   ⚠ 它**没有回读确认**（后台灌键是消息级的，目标不回话）；所以写完立刻**回读核实**
    //     （几何模式用 OCR 读输入框那块矩形）：读到了才算 verified，
    //     读不到**如实报 `BACKGROUND_INPUT_FAILED`**，绝不假装成功、也**不偷偷回退到抢前台**。
    if (backgroundInput) {
        // ★ 复用**窗口模式的后台输入链路**（见文件上方 `WmInputSession` 的说明）：
        //   手工调 `FakeFocusSoftInput_*` 能注入成功，但文本到不了 Chromium 的 composer ——
        //   缺的是 BeginRun 里那一整套准备（绑窗 / 解析输入子窗 / 进程内焦点 / 软光标同步）。
        if (!wmSession.Begin(out.target.hwnd, /*fakeFocusEnabled=*/true,
                profile.allowForegroundFallback)) {
            out.writeMs = static_cast<int>(NowMs() - t);
            out.error = "BACKGROUND_SESSION_FAILED";
            out.message = "建后台输入会话失败（复用窗口模式链路）："
                + ToUtf8Safe(wmSession.err())
                + "。**没有偷偷回退到抢前台**；若这个客户端确实只吃前台输入，"
                  "可在 window_ai_providers.json 里把 input.inputMethod 设为 \"foreground\"。";
            return out;
        }
        out.write.tried.push_back(L"background: 已建窗口模式后台会话（不抢前台、不搬桌面）");
        // 灌键可能走剪贴板通道 ⇒ 记下原内容，一轮结束后还回去
        const std::wstring clipBeforeBg = GetClipboardTextW();

        auto readBackInputBg = [&]() -> std::wstring {
            if (geometryMode) {
                std::wstring t2;
                std::string oe;
                if (OcrReadClientRegion(out.target.hwnd, geometryRect, t2, oe)) return t2;
                return {};
            }
            Snapshot again;
            if (!SnapshotImpl(out.target.hwnd, 400, again)) return {};
            const int idx2 = PickWindowAiInput(profile.input, again.nodes, client);
            if (idx2 < 0) return {};
            return ReadElementText(again.els[static_cast<size_t>(idx2)].Get(), true);
        };
        const std::wstring pre = readBackInputBg();
        // 输入框里有东西 ⇒ 先按"是不是我们自己的残留"处理（与前台通道同一套纪律）
        if (!pre.empty() && !WindowAiLooksLikePlaceholder(profile.input.placeholderHints, pre)) {
            out.write.tried.push_back(L"后台灌键：输入框非空，先按退格清理疑似残留");
            const size_t n = pre.size() < 400 ? pre.size() : 400;
            for (size_t i = 0; i < n; ++i) {
                windowmode::PostKeyToWindow(out.target.hwnd, VK_BACK, true);
                windowmode::PostKeyToWindow(out.target.hwnd, VK_BACK, false);
                Sleep(8);
            }
            Sleep(150);
        }
        // ★ 用**会话自己的输入接口**（它内部会选对 Chromium 该走的那条进程内路径）：
        //   ① 先在**客户区坐标**上点一下输入框（`PostMouseClickAtClient` 自带软光标同步）；
        //   ② `SendQuickInputToTarget` 灌文本（中文走它内部的剪贴板粘贴，且是进程内 Ctrl+V）。
        {
            const bool geometryRectOk = geometryMode
                && geometryRect.right > geometryRect.left && geometryRect.bottom > geometryRect.top;
            RECT inRect{};
            const bool hintOk = ResolveRelativeRect(profile.input.rectHint, client, inRect);
            const RECT useRect = geometryRectOk ? geometryRect : inRect;
            if (geometryRectOk || hintOk) {
                POINT origin{ 0, 0 };
                ClientToScreen(out.target.hwnd, &origin);
                // 屏幕坐标 → **客户区坐标**（会话接口收的是客户区）
                const int cx = (useRect.left + useRect.right) / 2 - origin.x;
                const int cy = (useRect.top + useRect.bottom) / 2 - origin.y;
                wmSession.exec().PostMouseClickAtClient(cx, cy, MouseButtonType::Left,
                    /*scaleRecordedClient=*/false);
                Sleep(220);
                out.write.tried.push_back(L"background: 已软点击输入框（客户区 "
                    + std::to_wstring(cx) + L"," + std::to_wstring(cy) + L"）");
            } else {
                out.write.tried.push_back(L"background: 没有可用的输入框矩形，跳过软点击");
            }
        }
        // ★ 文本用**剪贴板 + 进程内 Ctrl+V**（与上面那次软点击同一条链路）：
        //   ⚠ `SendQuickInputToTarget` 对 Chromium **什么也不做**（实测：日志里连一行都没有）——
        //     它按"找子 Edit 控件"的思路找输入目标，而 Chromium 的 composer 不是一个 Edit 子窗。
        //     软点击已经证明进程内按键是通的 ⇒ 就用 `PostKeyToTarget` 发 Ctrl+V。
        if (SetClipboardTextW(text)) {
            wmSession.exec().PostKeyToTarget(VK_CONTROL, true);
            Sleep(30);
            wmSession.exec().PostKeyToTarget('V', true);
            Sleep(30);
            wmSession.exec().PostKeyToTarget('V', false);
            wmSession.exec().PostKeyToTarget(VK_CONTROL, false);
            out.write.tried.push_back(L"background: 进程内 Ctrl+V（PostKeyToTarget）");
        } else {
            out.write.tried.push_back(L"background: 写剪贴板失败");
        }
        Sleep(350);
        // ★★ 核实（**两条路都认**）：
        //   ① 输入框回读里能看到我们的文本 ⇒ 粘进去了（正常路径）；
        //   ② **会话文本里出现了我们的文本** ⇒ 它已经提交进会话了（实测就发生过：
        //      进程内 Ctrl+V 生效、且被客户端立刻送出去了，而输入框此刻已经空了 ——
        //      只认 ① 会把**成功**判成失败，并因此重复粘贴）。
        auto compact = [](const std::wstring& s) {
            std::wstring o;
            for (wchar_t c : s) {
                if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'　') continue;
                o.push_back(static_cast<wchar_t>(towlower(c)));
            }
            return o;
        };
        const std::wstring probe = text.size() > 24 ? text.substr(0, 24) : text;
        const std::wstring cp = compact(probe);
        auto containsProbe = [&](const std::wstring& hay) {
            const std::wstring cg = compact(hay);
            if (cg.empty() || cp.empty()) return false;
            return LongestCommonSubstringLen(cg, cp) >= (std::min)(static_cast<size_t>(8), cp.size());
        };
        std::wstring got;
        bool landed = false;
        bool alreadySubmitted = false;
        for (int attempt = 0; attempt < 6 && !landed && !alreadySubmitted; ++attempt) {
            got = readBackInputBg();
            if (containsProbe(got)) { landed = true; break; }
            // 会话里出现我们的文本 ⇒ 已经发出去了
            std::wstring conv;
            std::string cerr;
            if (ReadConversation(profile, out.target.hwnd, conv, cerr, nullptr, text)
                && containsProbe(conv)) {
                alreadySubmitted = true;
                got = text;   // 记成"写入成功"，下面按"已提交"处理
                break;
            }
            Sleep(300);
        }
        out.write.channel = L"background-post";
        out.write.readBack = TruncW(got, 200);
        if (landed || alreadySubmitted) {
            out.write.proof = WindowAiWriteProof::kVerified;
            out.writeMs = static_cast<int>(NowMs() - t);
            if (alreadySubmitted) {
                // 客户端自己已经把它送出去了（实测会这样）⇒ **不要再按一次回车**，
                // 否则会往会话里多发一条空消息。
                out.write.tried.push_back(L"后台会话：文本已在会话里（客户端已发出，跳过回车）");
                out.submitMs = 0;
            } else {
                // 提交：走**会话自己的按键接口**（后台、不抢前台；Chromium 走进程内队列）
                t = NowMs();
                wmSession.exec().PostKeyToTarget(VK_RETURN, true);
                Sleep(40);
                wmSession.exec().PostKeyToTarget(VK_RETURN, false);
                out.submitMs = static_cast<int>(NowMs() - t);
                out.write.tried.push_back(L"后台会话：已写入并核实，后台回车已提交");
            }
            RememberSentPrompt(text);
            // 剪贴板还回去（灌键可能用过它）。会话在离开作用域时自动 EndRun（拆注入）
            if (!clipBeforeBg.empty()) SetClipboardTextW(clipBeforeBg);
        } else {
            out.write.proof = WindowAiWriteProof::kFailed;
            out.write.tried.push_back(L"后台灌键回读不一致：" + TruncW(got, 80));
            out.writeMs = static_cast<int>(NowMs() - t);
            out.error = "BACKGROUND_INPUT_FAILED";
            out.message = "后台灌键（不抢前台）没能把文本送进输入框，回读为空/不一致。"
                "**没有偷偷回退到抢前台**。若这个客户端确实只吃前台输入，"
                "请在 window_ai_providers.json 里把 input.inputMethod 设为 \"foreground\""
                "（并接受自动化期间会占用前台）。";
            return out;
        }
        goto wait_reply;
    }
    {
        auto readBackInput = [&]() -> std::wstring {
            if (geometryMode) {
                // ★ 几何模式下 UIA 读不回输入框 ⇒ 用 **OCR 读我们刚点过的那块矩形**。
                //   有这一步，写入才能从 `unavailable` 升级到 `verified`
                //   （Chromium 客户端是这条路唯一的证据来源）。
                std::wstring t;
                std::string oe;
                if (OcrReadClientRegion(out.target.hwnd, geometryRect, t, oe)) return t;
                return {};
            }
            Snapshot again;
            if (!SnapshotImpl(out.target.hwnd, 400, again)) return {};
            const int idx2 = PickWindowAiInput(profile.input, again.nodes, client);
            if (idx2 < 0) return {};
            return ReadElementText(again.els[static_cast<size_t>(idx2)].Get(), true);
        };

        // ⚠ 几何模式下的"输入框里已经有草稿吗"**不能直接看 OCR 文本**：
        //   空输入框会显示占位符（豆包是「发消息或按住空格说话．．」）⇒ 会误判成有草稿而中止。
        //   判据改成"点击前后**有没有变化**"：没变 = 还是占位符 = 空。
        const std::wstring beforeClick = geometryMode ? readBackInput() : std::wstring();
        if (geometryMode) {
            ClickPoint((geometryRect.left + geometryRect.right) / 2,
                (geometryRect.top + geometryRect.bottom) / 2);
            Sleep(180);
        }
        const std::wstring pre = readBackInput();
        // ★★ 两条判据取**合取**（都成立才算"有草稿"）：
        //   ① 点击前后 OCR 结果**变了**（空框里点一下，占位符还是那个占位符 ⇒ 不变）；
        //   ② 不像占位符。
        //   ⚠⚠ 为什么不能只看 ②：**OCR 会把占位符读花** —— 实测「发消息或按住空格说话…」
        //   被读成「发氵肖息或按任空格说话“」，任何提示词都匹配不上 ⇒ 空框被当成用户草稿
        //   ⇒ 整条链**永远拒绝工作**（而回执看着很合理）。① 不受 OCR 质量影响。
        const bool looksNotEmpty = geometryMode
            ? (!pre.empty() && pre != beforeClick
                && !WindowAiLooksLikePlaceholder(profile.input.placeholderHints, pre))
            : !pre.empty();
        // ★★ 特例：输入框里是**我们自己上一轮留下的字**（写过但没提交成功）⇒ 退格清掉再写。
        //   ⚠ 只清"能确认是我们自己的"：内容里含我们这次要写的文本、或含我们上一条 prompt 的
        //     显著片段（忽略空白比对）。**用户自己的草稿一律不动**（那才是安全边界）。
        bool clearedOwn = false;
        if (looksNotEmpty && geometryMode) {
            // ★ 判据：**输入框里的内容已经是会话里出现过的一条消息** ⇒ 那是我们自己发过的
            //   （写过但没提交成功的残留），可以安全退格清掉。
            //   ⚠ 不能拿"这次要写的文本"作参照 —— 实测残留往往是**上一轮**的 prompt。
            //   ⚠ 也不能凭"像我们的口吻"猜：只有能在会话正文里找到同源片段才算数；
            //     用户自己的草稿（会话里没有）一律不动。
            auto compact = [](const std::wstring& s) {
                std::wstring o;
                for (wchar_t c : s) {
                    if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'　') continue;
                    o.push_back(static_cast<wchar_t>(towlower(c)));
                }
                return o;
            };
            LoadSentPrompts();
            const std::wstring compactPre = compact(pre);
            const std::wstring compactBase = compact(baseline);
            bool ours = false;
            // ★★ 判据 = **最长公共子串**（对 OCR 认错字免疫，见 LongestCommonSubstringLen）：
            //   OCR 会把「**请**只回答」读成「**清**只回答」，一个字之差就让 find/片段比对
            //   全部落空（实测），而 LCS 仍能拿到 >10 字的公共段 ⇒ 判为同源。
            //   参照 ①：我们自己发过的 prompt（落盘，跨重启有效）；②：会话正文（读回时再叠一层）。
            auto sameSource = [](const std::wstring& x, const std::wstring& y) {
                if (x.size() < 8 || y.size() < 8) return false;
                const size_t lcs = LongestCommonSubstringLen(x, y);
                const size_t shorter = (std::min)(x.size(), y.size());
                return lcs >= 8 && lcs * 10 >= shorter * 6;   // ≥8 字且覆盖较短者 ≥60%
            };
            for (const auto& sent : sentPrompts_) {
                if (sameSource(compactPre, compact(sent))) { ours = true; break; }
            }
            if (!ours && !compactBase.empty()) {
                ours = sameSource(compactPre, compactBase);
            }
            if (ours) {
                const size_t n = pre.size() < 400 ? pre.size() : 400;
                for (size_t i = 0; i < n; ++i) {
                    TapKey(VK_BACK);
                    Sleep(6);
                }
                Sleep(120);
                const std::wstring after = readBackInput();
                clearedOwn = after.empty()
                    || WindowAiLooksLikePlaceholder(profile.input.placeholderHints, after)
                    || after.size() < pre.size();
                out.write.tried.push_back(clearedOwn
                    ? L"清掉了我们自己上一轮残留的输入（退格，内容能在会话正文里找到同源片段）"
                    : L"尝试清掉自己的残留输入，但回读仍非空");
                if (!clearedOwn) {
                    out.writeMs = static_cast<int>(NowMs() - t);
                    out.error = "INPUT_NOT_EMPTY";
                    out.message = "输入框里有内容，且无法确认能安全清掉（可能是用户草稿），已中止";
                    return out;
                }
            } else {
                out.write.tried.push_back(L"输入框内容在会话正文里找不到同源片段 ⇒ 当作用户草稿，不动它");
                // ★ 诊断：把"记住了几条、最佳相似度多少"写进日志 ——
                //   这类判据失败时，光看"拒绝了"完全无法定位（是没记住？还是相似度不够？）
                size_t bestLcs = 0;
                for (const auto& sent : sentPrompts_) {
                    bestLcs = (std::max)(bestLcs,
                        LongestCommonSubstringLen(compactPre, compact(sent)));
                }
                const size_t baseLcs = compactBase.empty()
                    ? 0 : LongestCommonSubstringLen(compactPre, compactBase);
                windowmode::WindowModeLogEventf(
                    L"[窗口AI] 输入框非空但不认作自己的残留：已记住 %d 条 prompt，最佳 LCS=%d（会话正文 LCS=%d），"
                    L"输入框内容=「%s」",
                    static_cast<int>(sentPrompts_.size()), static_cast<int>(bestLcs),
                    static_cast<int>(baseLcs), TruncW(pre, 60).c_str());
            }
        }
        if (profile.input.abortIfInputNotEmpty && looksNotEmpty && !clearedOwn) {
            out.writeMs = static_cast<int>(NowMs() - t);
            out.error = "INPUT_NOT_EMPTY";
            out.message = "输入框里已经有内容（草稿），已中止以免覆盖用户输入："
                + ToUtf8Safe(TruncW(pre, 60));
            return out;
        }

        std::wstring probe = text.size() > 24 ? text.substr(0, 24) : text;
        auto verify = [&](const std::wstring& got) {
            if (got.empty()) return WindowAiWriteProof::kUnavailable;
            // 比对前 24 个字符（客户端可能做 Markdown 转换/换行规范化）
            const std::wstring want = probe;
            if (got.find(want) != std::wstring::npos) return WindowAiWriteProof::kVerified;
            // 退一步：去掉空白再比
            std::wstring a;
            std::wstring b;
            for (wchar_t c : got) if (c != L' ' && c != L'\n' && c != L'\r') a.push_back(c);
            for (wchar_t c : want) if (c != L' ' && c != L'\n' && c != L'\r') b.push_back(c);
            if (!b.empty() && a.find(b) != std::wstring::npos) return WindowAiWriteProof::kVerified;
            return WindowAiWriteProof::kMismatch;
        };

        // 通道 A：ValuePattern（能直接设值时最省事）
        ComPtr<IUIAutomationValuePattern> vp;
        if (inputEl
            && SUCCEEDED(inputEl->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&vp))) && vp) {
            BOOL readonly = FALSE;
            vp->get_CurrentIsReadOnly(&readonly);
            if (!readonly) {
                BSTR b = SysAllocString(text.c_str());
                const HRESULT hr = vp->SetValue(b);
                SysFreeString(b);
                Sleep(60);
                out.write.tried.push_back(L"value-pattern hr=" + std::to_wstring(hr));
                if (SUCCEEDED(hr)) {
                    const std::wstring got = readBackInput();
                    const auto proof = verify(got);
                    if (proof != WindowAiWriteProof::kMismatch) {
                        out.write.proof = proof;
                        out.write.channel = L"value-pattern";
                        out.write.readBack = TruncW(got, 200);
                    } else {
                        out.write.tried.push_back(L"value-pattern 回读不一致：" + TruncW(got, 80));
                    }
                }
            } else {
                out.write.tried.push_back(L"value-pattern 只读");
            }
        } else {
            out.write.tried.push_back(L"value-pattern 不支持");
        }

        // 通道 B：剪贴板 + Ctrl+V（富文本/Electron 最可靠）
        if (out.write.proof == WindowAiWriteProof::kFailed) {
            if (SetClipboardTextW(text)) {
                SendCtrlKey('V');
                Sleep(180);
                const std::wstring got = readBackInput();
                const auto proof = verify(got);
                if (proof != WindowAiWriteProof::kMismatch) {
                    out.write.proof = proof;
                    out.write.channel = L"clipboard-paste";
                    out.write.readBack = TruncW(got, 200);
                } else {
                    out.write.tried.push_back(L"clipboard-paste 回读不一致：" + TruncW(got, 80));
                }
            } else {
                out.write.tried.push_back(L"写剪贴板失败");
            }
        }

        // 通道 C：逐字 Unicode 输入（前两条都不行时的最后一档）
        if (out.write.proof == WindowAiWriteProof::kFailed && text.size() <= 2000) {
            for (wchar_t ch : text) {
                if (ch == L'\n') { TapKey(VK_RETURN); continue; }
                SendUnicodeChar(ch);
                Sleep(6);
            }
            Sleep(150);
            const std::wstring got = readBackInput();
            const auto proof = verify(got);
            if (proof != WindowAiWriteProof::kMismatch) {
                out.write.proof = proof;
                out.write.channel = L"unicode-typing";
                out.write.readBack = TruncW(got, 200);
            } else {
                out.write.tried.push_back(L"unicode-typing 回读不一致：" + TruncW(got, 80));
            }
        }
    }
    out.writeMs = static_cast<int>(NowMs() - t);
    // ★ 记下这次发出去的 prompt：下一轮识别"输入框里的残留是不是我们自己的"要用它
    //   （判据不能依赖会话读回 —— 实测它可能只回 25 字，见 sentPrompts_ 的说明）
    if (out.write.proof != WindowAiWriteProof::kFailed) RememberSentPrompt(text);
    if (out.write.proof == WindowAiWriteProof::kFailed) {
        out.error = "WRITE_FAILED";
        out.message = "三条写入通道都没写进去（见 tried）";
        return out;
    }

    // ⑥ 提交
    t = NowMs();
    if (profile.submitMethod == L"button" && !profile.sendButtonNames.empty()) {
        // 按名字找发送按钮并 Invoke；找不到再退回回车
        Snapshot again;
        bool clicked = false;
        if (SnapshotImpl(out.target.hwnd, 400, again)) {
            for (size_t i = 0; i < again.nodes.size(); ++i) {
                const auto& n = again.nodes[i];
                if (n.controlType != L"Button") continue;
                bool nameOk = false;
                for (const auto& want : profile.sendButtonNames) {
                    if (!want.empty() && LowerW(n.name).find(LowerW(want)) != std::wstring::npos) {
                        nameOk = true;
                        break;
                    }
                }
                if (!nameOk) continue;
                ComPtr<IUIAutomationInvokePattern> ip;
                if (SUCCEEDED(again.els[i]->GetCurrentPatternAs(UIA_InvokePatternId,
                        IID_PPV_ARGS(&ip))) && ip) {
                    if (SUCCEEDED(ip->Invoke())) { clicked = true; break; }
                }
                if (n.rect.right > n.rect.left) {
                    ClickPoint((n.rect.left + n.rect.right) / 2, (n.rect.top + n.rect.bottom) / 2);
                    clicked = true;
                    break;
                }
            }
        }
        if (!clicked) {
            TapKey(VK_RETURN);
            out.write.tried.push_back(L"submit: 没找到发送按钮，退回回车");
        }
    } else {
        TapKey(VK_RETURN);
    }
    Sleep(200);
    out.submitMs = static_cast<int>(NowMs() - t);

wait_reply:
    // ⑦ 等**新**回答出现并稳定
    //
    //   稳定判据 = 「内容已经 **idleTimeoutMs** 没变过」+「至少读过两次」。
    //   ⚠ 为什么必须用**配置的空闲时长**而不是"两次相同就收"：流式渲染的客户端
    //     会出现"两次取样恰好一样、下一毫秒又变了"（网络抖动/分段渲染），
    //     固定 1.2 秒会把**半截回答**当最终答案交出去。
    //     这一条与网页端 `readAssistantReply` 的 `progressIdleTimeoutMs` 同一语义。
    t = NowMs();
    const long long deadline = t + totalMs;
    WindowAiStability stab;
    std::wstring lastReply;
    long long lastChangeAt = NowMs();
    bool sawAny = false;
    // ★ 剪贴板通道（**首选读法**）每轮只试一次：它要点击 + 借用户的剪贴板，
    //   不能每轮都做。触发点是"我们已经认为生成结束了"（或压根看不到新内容）。
    bool clipboardTried = false;
    std::string clipboardWhy;
    auto acceptViaClipboard = [&]() -> bool {
        std::wstring clip;
        std::string why;
        clipboardTried = true;
        // ★ 后台模式：给驱动装上**软悬停**（走会话的软光标 ⇒ 进程内、不抢前台）。
        //   用途：消息操作栏是 hover 才渲染的 —— 不悬停，UIA 树里就没有那一排。
        if (backgroundInput && wmSession.ok()) {
            softHover_ = [&wmSession](int cx, int cy) -> bool {
                if (!wmSession.ok()) return false;
                wmSession.exec().MoveMouseClient(cx, cy, 0, 0,
                    [](int) { return 0; },   // ⚠ 不能传空 std::function：MoveMouseClient 会调它 ⇒ bad_function_call
                    /*scaleRecordedClient=*/false);
                return true;
            };
        }
        const bool got = TryClipboardReply(profile, out.target.hwnd, text, clip, why);
        softHover_ = nullptr;   // 用完立刻摘掉（别让别处误用已经结束的会话）
        if (!got) {
            clipboardWhy = why;
            windowmode::WindowModeLogEventf(L"[窗口AI] 剪贴板通道没拿到：%s",
                FromUtf8Safe(why).c_str());
            return false;
        }
        out.replyText = clip;
        out.replyChars = static_cast<int>(clip.size());
        out.truncated = profile.reply.maxChars > 0
            && static_cast<int>(clip.size()) >= profile.reply.maxChars;
        out.replySource = "clipboard";
        out.ok = true;
        out.waitMs = static_cast<int>(NowMs() - t);
        windowmode::WindowModeLogEventf(
            L"[窗口AI] %s：剪贴板拿到回答 %d 字（模式=%s 写入=%s/%s，等 %dms）",
            FromUtf8Safe(profile.label).c_str(), out.replyChars, out.mode.c_str(),
            WindowAiWriteProofName(out.write.proof), out.write.channel.c_str(), out.waitMs);
        return true;
    };
    const long long clipboardFallbackAt = (std::max)(static_cast<long long>(idleMs),
        static_cast<long long>(4000));
    for (;;) {
        const long long now = NowMs();
        if (now >= deadline) break;
        std::wstring cur;
        std::string cerr;
        // ★ 轮询时把"我们刚发出去的那句话"传进去：它决定这一轮读 UIA 还是 OCR
        //   （见 ReadConversation 的择优说明 —— 按长度选会选到更脏的 OCR）
        if (!ReadConversation(profile, out.target.hwnd, cur, cerr, &out.replySource, text)) {
            // 读不到任何文本时，只要过了兜底时刻就试剪贴板通道
            // （有客户端既不给 UIA 文本、OCR 也读不出会话，但"复制"是好用的）
            if (!clipboardTried && (NowMs() - t) >= clipboardFallbackAt && acceptViaClipboard()) {
                return out;
            }
            Sleep(500);
            continue;
        }
        const std::wstring reply = ExtractWindowAiReply(baseline, cur, text,
            profile.reply.maxChars);
        if (!reply.empty()) {
            if (reply != lastReply) {
                lastReply = reply;
                lastChangeAt = NowMs();
            }
            sawAny = true;
            const bool idleEnough = (NowMs() - lastChangeAt) >= idleMs;
            if (idleEnough && FeedWindowAiStability(stab, reply, 2)) {
                // ★★ 生成结束 ⇒ 先用**剪贴板通道**拿原文（准、快、完整），
                //    拿不到才用 UIA/OCR 那份（并在回执里保留 clipboardWhy 说明原因）。
                if (!clipboardTried && acceptViaClipboard()) return out;
                out.replyText = reply;
                out.replyChars = static_cast<int>(reply.size());
                out.truncated = profile.reply.maxChars > 0
                    && static_cast<int>(reply.size()) >= profile.reply.maxChars;
                out.ok = true;
                out.waitMs = static_cast<int>(NowMs() - t);
                windowmode::WindowModeLogEventf(
                    L"[窗口AI] %s：新回答 %d 字（模式=%s 写入=%s/%s，来源=%s，等 %dms）",
                    FromUtf8Safe(profile.label).c_str(), out.replyChars,
                    out.mode.c_str(),
                    WindowAiWriteProofName(out.write.proof), out.write.channel.c_str(),
                    FromUtf8Safe(out.replySource).c_str(),
                    out.waitMs);
                return out;
            }
        } else if (!clipboardTried && (NowMs() - t) >= clipboardFallbackAt
            && acceptViaClipboard()) {
            // 看不到任何新内容（客户端自绘/读不回）⇒ 剪贴板是唯一出路
            return out;
        }
        Sleep(600);
    }
    out.waitMs = static_cast<int>(NowMs() - t);
    out.replyText = lastReply;
    out.replyChars = static_cast<int>(lastReply.size());
    out.error = sawAny ? "REPLY_NOT_STABLE" : "NO_REPLY";
    out.message = sawAny
        ? "回答一直在变（客户端可能仍在生成），未达到稳定判据"
        : ("提交后没读到新内容：可能没发出去（回车没生效？）或该客户端读不回文本"
           + (clipboardWhy.empty() ? std::string() : ("；剪贴板通道：" + clipboardWhy)));
    windowmode::WindowModeLogEventf(L"[窗口AI] %s 失败：%s（等 %dms，基线 %d 字）",
        FromUtf8Safe(profile.label).c_str(), FromUtf8Safe(out.error).c_str(),
        out.waitMs, out.baselineChars);
    return out;
}

// ──────────────────────────────────────────────────────────────────
// 干跑写入（探针）：**不提交**，写完用退格撤销
// ──────────────────────────────────────────────────────────────────
WindowAiDryRun WindowAiDriver::DryRunWrite(const std::string& clientId,
    const std::wstring& marker) {
    WindowAiDryRun run;
    const std::wstring text = marker.empty() ? std::wstring(L"QST探测") : marker;
    const auto& clients = Clients();
    const int ci = FindWindowAiClient(clients, clientId);
    if (ci < 0) {
        run.error = "NO_CLIENT_PROFILE";
        run.message = "配置里没有客户端 id：" + clientId;
        return run;
    }
    const WindowAiClientProfile profile = clients[static_cast<size_t>(ci)];

    WindowAiTarget target;
    std::string err;
    if (!FindClientWindow(profile, target, err)) {
        run.error = "NO_CLIENT_WINDOW";
        run.message = err;
        return run;
    }
    std::wstring aerr;
    if (!windowmode::ActivateWindow(target.hwnd, aerr)) {
        run.error = "ACTIVATE_FAILED";
        run.message = ToUtf8Safe(aerr.empty() ? L"无法把客户端切到前台" : aerr);
        return run;
    }
    Sleep(120);

    Snapshot snap;
    if (!SnapshotImpl(target.hwnd, 400, snap)) {
        run.error = "UIA_SNAPSHOT_FAILED";
        run.message = "UIA 快照失败";
        return run;
    }
    // ★★ **激活后重新验树**：Chromium 系客户端**只在前台渲染时才交 UIA 树**
    //   （实测豆包：后台/隐藏时空壳，点一下变 308 个节点）。
    //   激活后树仍不可用 ⇒ **立刻失败**，绝不继续写消息：
    //   否则会往用户会话里塞一条消息、再白等满 180 秒（实测就这样烧掉一整轮）。
    if (snap.nodes.size() < 8) {
        Sleep(400);
        Snapshot again;
        if (SnapshotImpl(target.hwnd, 400, again) && again.nodes.size() >= 8) {
            snap = std::move(again);
        } else {
            run.error = "WINDOW_TREE_EMPTY";
            run.message = "目标窗口没给 UIA 控件树（实测 Chromium 系客户端**只在前台渲染时**才给，"
                "后台/隐藏时空壳）⇒ 读回答所需的控件（消息操作栏）拿不到，"
                "已**在写入之前**中止，避免往用户会话里塞消息后白等。"
                "请把客户端切到前台点一下（让它渲染）后再试。";
            windowmode::WindowModeLogEventf(
                L"[窗口AI] 未写入即中止：窗口树只有 %d 个节点（激活后仍未恢复）",
                static_cast<int>(snap.nodes.size()));
            return run;
        }
    }
    const RECT client = ClientRectOnScreen(target.hwnd);
    const int idx = PickWindowAiInput(profile.input, snap.nodes, client);
    RECT inputRect{};
    bool geometryMode = false;
    if (idx < 0) {
        RECT rel{};
        if (ResolveRelativeRect(profile.input.rectHint, client, rel)) {
            geometryMode = true;
            inputRect = rel;
            run.inputMatchedBy = L"相对矩形（几何兜底，UIA 树为空）";
        } else {
            run.error = "NO_INPUT";
            run.message = "没找到输入框（" + std::to_string(snap.nodes.size())
                + " 个控件）且没有可用 rectHint；UIA 树为空时请在 window_ai_providers.json 里配 rectHint";
            return run;
        }
    } else {
        inputRect = snap.nodes[static_cast<size_t>(idx)].rect;
        run.inputMatchedBy = DescribeNode(snap.nodes[static_cast<size_t>(idx)]);
    }
    IUIAutomationElement* el = geometryMode ? nullptr : snap.els[static_cast<size_t>(idx)].Get();

    bool focusOk = false;
    if (geometryMode) {
        ClickPoint((inputRect.left + inputRect.right) / 2,
            (inputRect.top + inputRect.bottom) / 2);
        Sleep(200);
        focusOk = true;   // 几何模式无判据可核实（如实记账，不假装）
        run.write.tried.push_back(L"geometry: 已点击相对矩形中心（焦点无法核实）");
    }
    if (el && SUCCEEDED(el->SetFocus())) {
        Sleep(80);
        VARIANT v{};
        if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_HasKeyboardFocusPropertyId, &v))) {
            focusOk = (v.vt == VT_BOOL && v.boolVal == VARIANT_TRUE);
            VariantClear(&v);
        }
    }
    if (!focusOk) {
        const auto& rc = snap.nodes[static_cast<size_t>(idx)].rect;
        if (rc.right > rc.left && rc.bottom > rc.top) {
            ClickPoint((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
            Sleep(150);
            VARIANT v2{};
            if (el && SUCCEEDED(el->GetCurrentPropertyValue(UIA_HasKeyboardFocusPropertyId, &v2))) {
                focusOk = (v2.vt == VT_BOOL && v2.boolVal == VARIANT_TRUE);
                VariantClear(&v2);
            }
        }
    }
    run.focused = focusOk;
    if (!focusOk) {
        run.error = "FOCUS_FAILED";
        run.message = "输入框拿不到键盘焦点（打字会打到别处，已中止）";
        return run;
    }

    auto readBackInput = [&]() -> std::wstring {
        if (geometryMode) {
            std::wstring t;
            std::string oe;
            if (OcrReadClientRegion(target.hwnd, inputRect, t, oe)) return t;
            if (!oe.empty()) run.ocrError = oe;   // ★ 不许吞掉 OCR 的原始错误
            return {};
        }
        Snapshot again;
        if (!SnapshotImpl(target.hwnd, 400, again)) return {};
        const int idx2 = PickWindowAiInput(profile.input, again.nodes, client);
        if (idx2 < 0) return {};
        return ReadElementText(again.els[static_cast<size_t>(idx2)].Get(), true);
    };

    // 几何模式：空输入框会显示占位符 ⇒ 用"点击前后有没有变化"判有没有草稿（同 SendAndRead）
    const std::wstring beforeClick = geometryMode ? readBackInput() : std::wstring();
    if (geometryMode) {
        ClickPoint((inputRect.left + inputRect.right) / 2,
            (inputRect.top + inputRect.bottom) / 2);
        Sleep(180);
    }
    run.preExistingText = readBackInput();
    // 同 SendAndRead：既要"变化了"也要"不像占位符"（OCR 会把占位符读花，见那里的说明）
    const bool looksNotEmpty = geometryMode
        ? (!run.preExistingText.empty() && run.preExistingText != beforeClick
            && !WindowAiLooksLikePlaceholder(profile.input.placeholderHints, run.preExistingText))
        : !run.preExistingText.empty();
    if (profile.input.abortIfInputNotEmpty && looksNotEmpty) {
        run.error = "INPUT_NOT_EMPTY";
        run.message = "输入框里已有内容（草稿），干跑已中止以免覆盖："
            + ToUtf8Safe(TruncW(run.preExistingText, 60));
        return run;
    }

    // 写 marker（复用与正式路径同一套三档通道）
    ComPtr<IUIAutomationValuePattern> vp;
    if (el && SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&vp))) && vp) {
        BOOL ro = FALSE;
        vp->get_CurrentIsReadOnly(&ro);
        if (!ro) {
            BSTR b = SysAllocString(text.c_str());
            const HRESULT hr = vp->SetValue(b);
            SysFreeString(b);
            Sleep(60);
            run.write.tried.push_back(L"value-pattern hr=" + std::to_wstring(hr));
            if (SUCCEEDED(hr)) {
                run.write.proof = WindowAiWriteProof::kUnavailable;  // 下面回读再升级
                run.write.channel = L"value-pattern";
            }
        } else {
            run.write.tried.push_back(L"value-pattern 只读");
        }
    } else {
        run.write.tried.push_back(L"value-pattern 不支持");
    }
    if (run.write.proof == WindowAiWriteProof::kFailed) {
        if (SetClipboardTextW(text)) {
            SendCtrlKey('V');
            Sleep(180);
            run.write.proof = WindowAiWriteProof::kUnavailable;
            run.write.channel = L"clipboard-paste";
        } else {
            run.write.tried.push_back(L"写剪贴板失败");
        }
    }

    const std::wstring got = readBackInput();
    run.write.readBack = TruncW(got, 200);
    // ★ 回读区域读空时补一次**整窗 OCR**：用来区分「粘贴没生效」与「回读区域量错了」
    //   （只看到一句"回读为空"时，这两种情况的处置完全相反）
    if (got.empty()) {
        std::wstring full;
        std::string fe;
        POINT tl{ 0, 0 };
        ClientToScreen(target.hwnd, &tl);
        RECT cc{};
        GetClientRect(target.hwnd, &cc);
        RECT fullRect{ tl.x, tl.y, tl.x + cc.right, tl.y + cc.bottom };
        if (OcrReadClientRegion(target.hwnd, fullRect, full, fe)) {
            run.postWriteOcrHead = TruncW(full, 300);
        } else if (!fe.empty()) {
            run.ocrError = run.ocrError.empty() ? fe : (run.ocrError + "；整窗：" + fe);
        }
    }
    if (run.write.proof != WindowAiWriteProof::kFailed) {
        const bool same = !got.empty()
            && got.find(text.size() > 10 ? text.substr(0, 10) : text) != std::wstring::npos;
        run.write.proof = same ? WindowAiWriteProof::kVerified
                               : (got.empty() ? WindowAiWriteProof::kUnavailable
                                              : WindowAiWriteProof::kMismatch);
    }

    // 撤销：**退格**（绝不用 Ctrl+A + Del）
    if (run.write.proof != WindowAiWriteProof::kFailed) {
        const size_t n = text.size();
        for (size_t i = 0; i < n; ++i) {
            TapKey(VK_BACK);
            Sleep(6);
        }
        Sleep(80);
        const std::wstring after = readBackInput();
        run.clearedAfter = after.empty();
    }
    if (run.write.proof == WindowAiWriteProof::kFailed) {
        run.error = "WRITE_FAILED";
        run.message = "三条写入通道都没写进去";
    }
    return run;
}

}  // namespace quickscript::webai
