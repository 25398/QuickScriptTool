// =============================================================================
// WindowModeSelfTest — 窗口模式自检（Agent 入口）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
// 专项：.cursor/skills/window-mode-debug/SKILL.md + reference.md
//
//   MSBuild ... /t:WindowModeSelfTest
//   build\Release\WindowModeSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "window_mode/window_mode_executor.h"
#include "window_mode/window_mode_json.h"
#include "window_mode/window_mode_types.h"
#include "window_mode/window_target.h"
#include "window_mode/background_window_input.h"
#include "window_mode/ext_bridge/ext_bridge_server.h"
#include "window_mode/fake_focus/fake_focus_soft_input_host.h"
#include "action_utils.h"

#include <shellapi.h>
#include <dwmapi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using selftest::Emit;
using selftest::gJson;

const selftest::CaseInfo kCases[] = {
    {L"quote_args_strip", L"default",
        L"Strip outer quotes on document path (avoids Notepad invalid-filename)"},
    {L"no_select_ignores_doc", L"default",
        L"NoSelect launches bare exe; ignores leftover launchArgs/windowName document"},
    {L"ime_filter_null", L"default",
        L"IsLikelyImeOrToolWindow(nullptr) must reject"},
    {L"find_main_window", L"default",
        L"FindMainWindowDefault locates self-test top window by class"},
    {L"post_quick_input", L"default",
        L"PostQuickInputToWindow writes marker into EDIT"},
    {L"background_bind_child", L"default",
        L"Background BeginRun binds child EDIT, not only top-level"},
    {L"background_quick_input", L"default",
        L"WindowModeExecutor background quick-input succeeds"},
    {L"quick_input_cancel", L"default",
        L"Cancel flag aborts timed quick-input before all chars are written"},
    {L"desktop_quick_input_cancel", L"default",
        L"SendQuickInputText respects cancel flag between characters"},
    {L"macro_desktop_launch_bind", L"macro",
        L"Macro-desktop launches classic notepad and binds (needs --macro + VDA DLL)"},
    {L"macro_classic_with_store_open", L"macro",
        L"System32 notepad launch while Store Notepad already open must still bind"},
    {L"macro_store_path_class_bind", L"macro",
        L"WindowsApps Notepad path + UseEditorWindowClass binds RichEdit child"},
    {L"macro_editor_open_named_doc", L"macro",
        L"UseEditorWindowClass opens specific document from window title + searchDir"},
    {L"fake_focus_json_roundtrip", L"default",
        L"fakeFocusEnabled JSON parse/write defaults false and roundtrips"},
    {L"input_strategy_cdp_auto", L"default",
        L"Chrome_WidgetWin class → CDP strategy on save/resolve; game exe stays softMessage"},
    {L"ext_bridge_config_parse", L"default",
        L"ParseExtBridgeConfigJson accepts port/token; rejects bad JSON"},
    {L"fake_focus_minimize_gate", L"default",
        L"UsesFakeFocus only for HiddenDesktop; CDP keeps restored (no minimize-after-bind)"},
    {L"soft_message_exe_gates", L"default",
        L"Unity/game exe: softMessage, minimize-after-bind; fakeFocus keeps restored (no CDP prepare)"},
    {L"restore_prefer_maximized", L"default",
        L"RestoreMinimizedQuietPreferMax restores Zoomed after minimize; normal stay unzoomed"},
    {L"fake_focus_hook_local", L"default",
        L"Load FakeFocus64/32 locally: GetForegroundWindow returns target; uninstall restores"},
    {L"fake_focus_soft_input", L"default",
        L"Phase2: soft shared memory drives GetCursorPos/GetAsyncKeyState/GetKeyboardState"},
    {L"anjuzhen_script_wm_config", L"default",
        L"Parse build/*/scripts/安居镇.json windowMode: fakeFocus + Chrome child class"},
    {L"invisible_child_class_bind", L"default",
        L"FindChildWindowByClass finds WS_CHILD without WS_VISIBLE (macro-desktop case)"},
    {L"browser_render_skips_d3d", L"default",
        L"FindBrowserRenderWidget ignores Intermediate D3D; prefers RenderWidget or null"},
    {L"cdp_park_expandable", L"default",
        L"PrepareMacroDesktopForCdpBind: Move macro desk; no Cloak; no offscreen"},
};

void PrintHelp() {
    std::fwprintf(stderr,
        L"WindowModeSelfTest — QuickScriptTool 窗口模式自检\n"
        L"\n"
        L"用法:\n"
        L"  WindowModeSelfTest.exe [--json] [--list] [--macro] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  FAIL name → .cursor/skills/window-mode-debug/reference.md\n"
        L"\n"
        L"标志:\n"
        L"  --json   每行一个 JSON 用例结果 + 末行汇总（stdout）\n"
        L"  --list   列出用例，不执行\n"
        L"  --macro  额外跑宏桌面烟雾测试\n"
        L"  --help   显示本帮助\n");
}

constexpr wchar_t kTestClass[] = L"QuickScriptWmSelfTestEdit";
constexpr int kTestId = 8801;

void RegisterTestClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTestClass;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    registered = true;
}

HWND CreateTestEditWindow() {
    RegisterTestClass();
    HWND hwnd = CreateWindowExW(
        0, kTestClass, L"QST SelfTest",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        120, 120, 480, 320,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return nullptr;

    HWND edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
        8, 8, 456, 270,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTestId)),
        GetModuleHandleW(nullptr), nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return edit ? edit : hwnd;
}

std::wstring ReadEditText(HWND edit) {
    if (!edit) return L"";
    const int len = GetWindowTextLengthW(edit);
    if (len <= 0) return L"";
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(edit, text.data(), len + 1);
    text.resize(len);
    return text;
}

HWND ParentTopWindow(HWND edit) {
    return GetAncestor(edit, GA_ROOT);
}

// 与 FormatLaunchArgs 同规则：外层引号需剥掉。
std::wstring NormalizeDocArg(std::wstring a) {
    while (!a.empty() && (a.front() == L' ' || a.front() == L'\t')) a.erase(a.begin());
    while (!a.empty() && (a.back() == L' ' || a.back() == L'\t')) a.pop_back();
    if (a.size() >= 2 && a.front() == L'"' && a.back() == L'"') {
        a = a.substr(1, a.size() - 2);
    }
    return a;
}

void TestQuoteArgs() {
    const std::wstring raw = L"C:\\Users\\测试\\Desktop\\检查.txt";
    const std::wstring quoted = L"\"" + raw + L"\"";
    const std::wstring once = NormalizeDocArg(quoted);
    const std::wstring twice = NormalizeDocArg(NormalizeDocArg(quoted));
    const bool ok = (once == raw) && (twice == raw)
        && NormalizeDocArg(raw) == raw;
    Emit(L"quote_args_strip", ok, ok ? L"" : L"strip outer quotes failed");
}

void TestNoSelectIgnoresDocument() {
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring dir = std::wstring(tempDir) + L"qst_wm_noselect";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring docPath = dir + L"\\qst_noselect_doc.txt";
    {
        HANDLE hf = CreateFileW(docPath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            const char* body = "NOSELECT_SHOULD_NOT_OPEN\r\n";
            DWORD written = 0;
            WriteFile(hf, body, static_cast<DWORD>(strlen(body)), &written, nullptr);
            CloseHandle(hf);
        }
    }

    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    cfg.selectMethod = windowmode::WindowSelectMethod::NoSelect;
    cfg.targetExePath = L"C:\\Windows\\System32\\notepad.exe";
    // 模拟从「指定窗口类」切过来的残留：仍带文档参数与标题。
    cfg.launchArgs = L"\"" + docPath + L"\"";
    cfg.windowName = L"qst_noselect_doc.txt - Notepad";
    cfg.targetWindowTitle = cfg.windowName;
    cfg.autoLaunchTarget = true;

    windowmode::WindowModeExecutor exec;
    windowmode::BeginRunOptions opts;
    opts.launchTarget = true;
    opts.launchSearchDir = dir;
    std::wstring err;
    const bool ok = exec.BeginRun(cfg, err, opts);
    HWND th = ok ? exec.TargetHwnd() : nullptr;
    wchar_t title[512]{};
    if (th) {
        HWND root = GetAncestor(th, GA_ROOT);
        if (!root) root = th;
        GetWindowTextW(root, title, 512);
    }
    if (ok) exec.EndRun();

    const bool leakedDoc = title[0] != L'\0'
        && wcsstr(title, L"qst_noselect_doc.txt") != nullptr;
    Emit(L"no_select_ignores_doc", ok && !leakedDoc,
        ok ? (leakedDoc ? title : (title[0] ? title : L"ok bare notepad"))
           : err.c_str());

    DeleteFileW(docPath.c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestImeFilter() {
    const bool rejectNull = windowmode::IsLikelyImeOrToolWindow(nullptr);
    Emit(L"ime_filter_null", rejectNull, L"null should be rejected");
}

void TestFindTarget(HWND edit) {
    HWND top = ParentTopWindow(edit);
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);

    windowmode::WindowTargetQuery query{};
    query.className = cls;
    RECT rc{};
    GetWindowRect(top, &rc);
    query.pickX = (rc.left + rc.right) / 2;
    query.pickY = (rc.top + rc.bottom) / 2;

    HWND found = windowmode::FindMainWindowDefault(query);
    Emit(L"find_main_window", found != nullptr);
}

void TestPostQuickInput(HWND edit) {
    SetWindowTextW(edit, L"");
    const wchar_t* marker = L"QST_WM_TEST_42";
    windowmode::PostQuickInputToWindow(edit, marker, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::wstring text = ReadEditText(edit);
    Emit(L"post_quick_input", text.find(marker) != std::wstring::npos, text.c_str());
}

void PumpMessagesFor(std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void TestQuickInputCancel(HWND edit) {
    SetWindowTextW(edit, L"");
    const std::wstring longText(80, L'X');
    std::atomic_bool cancel{false};
    std::atomic_bool workerDone{false};
    std::thread worker([&]() {
        windowmode::PostQuickInputToWindow(edit, longText, 0.03, false, &cancel);
        workerDone.store(true, std::memory_order_relaxed);
    });
    // SendMessage 打到本线程窗口：等待期间必须泵消息，否则会死锁
    PumpMessagesFor(std::chrono::milliseconds(90));
    cancel.store(true, std::memory_order_relaxed);
    while (!workerDone.load(std::memory_order_relaxed)) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.join();
    const std::wstring text = ReadEditText(edit);
    wchar_t detail[96]{};
    swprintf_s(detail, L"len=%zu/%zu", text.size(), longText.size());
    const bool ok = !text.empty() && text.size() < longText.size();
    Emit(L"quick_input_cancel", ok, detail);
}

void TestDesktopQuickInputCancel() {
    // 不依赖焦点窗口：取消后应很快返回（完整 40 字×30ms 约 1.2s，取消应远小于此）
    const std::wstring longText(40, L'A');
    std::atomic_bool cancel{false};
    const auto t0 = std::chrono::steady_clock::now();
    std::thread worker([&]() {
        SendQuickInputText(longText, 0.03, &cancel);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancel.store(true, std::memory_order_relaxed);
    worker.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    wchar_t detail[64]{};
    swprintf_s(detail, L"elapsed_ms=%lld", static_cast<long long>(ms));
    Emit(L"desktop_quick_input_cancel", ms < 400, detail);
}

void TestExecutorBackground(HWND edit) {
    HWND top = ParentTopWindow(edit);
    wchar_t topCls[256]{};
    GetClassNameW(top, topCls, 256);
    RECT rc{};
    GetWindowRect(top, &rc);

    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    cfg.windowClassName = topCls;
    cfg.childWindowClassName = L"EDIT";
    cfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
    cfg.targetPickX = (rc.left + rc.right) / 2;
    cfg.targetPickY = (rc.top + rc.bottom) / 2;

    std::wstring err;
    if (!windowmode::WindowModeExecutor::CheckRunHealth(cfg, err)) {
        Emit(L"background_bind_child", false, err.c_str());
        Emit(L"background_quick_input", false, L"skipped: CheckRunHealth failed");
        return;
    }

    windowmode::WindowModeExecutor exec;
    if (!exec.BeginRun(cfg, err)) {
        Emit(L"background_bind_child", false, err.c_str());
        Emit(L"background_quick_input", false, L"skipped: BeginRun failed");
        return;
    }

    SetWindowTextW(edit, L"");
    const wchar_t* marker = L"QST_EXEC_TEST_99";
    exec.SendQuickInputToTarget(marker, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    HWND bound = exec.TargetHwnd();
    wchar_t boundCls[128]{};
    if (bound) GetClassNameW(bound, boundCls, 128);
    const bool boundChild = _wcsicmp(boundCls, L"EDIT") == 0;

    HWND boundEdit = windowmode::FindTextInputTarget(bound ? bound : top);
    const std::wstring text = ReadEditText(boundEdit ? boundEdit : edit);
    exec.EndRun();

    Emit(L"background_bind_child", boundChild, boundCls);
    Emit(L"background_quick_input", text.find(marker) != std::wstring::npos, text.c_str());
}

bool BeginMacroBind(const windowmode::WindowModeScriptConfig& cfg, std::wstring& err,
    HWND& outHwnd, wchar_t* outCls, size_t outClsChars) {
    outHwnd = nullptr;
    if (outCls && outClsChars) outCls[0] = L'\0';
    windowmode::WindowModeExecutor exec;
    windowmode::BeginRunOptions opts;
    opts.launchTarget = true;
    if (!exec.BeginRun(cfg, err, opts)) return false;
    outHwnd = exec.TargetHwnd();
    if (outHwnd && outCls && outClsChars) {
        GetClassNameW(outHwnd, outCls, static_cast<int>(outClsChars));
    }
    exec.EndRun();
    return outHwnd != nullptr;
}

void EnsureStoreNotepadOpen() {
    windowmode::WindowTargetQuery q{};
    q.exePath = L"C:\\Windows\\System32\\notepad.exe";
    q.allowStoreNotepadHandoff = true;
    if (windowmode::FindMainWindowDefault(q, true)
        || windowmode::FindMainWindowDefault(q, false)) {
        return;
    }
    ShellExecuteW(nullptr, L"open", L"notepad.exe", nullptr, nullptr, SW_SHOWNOACTIVATE);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (windowmode::FindMainWindowDefault(q, true)
            || windowmode::FindMainWindowDefault(q, false)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void TestMacroDesktopSmoke() {
    // 1) 经典路径冷启动
    {
        windowmode::WindowModeScriptConfig cfg{};
        cfg.enabled = true;
        cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
        cfg.selectMethod = windowmode::WindowSelectMethod::NoSelect;
        cfg.targetExePath = L"C:\\Windows\\System32\\notepad.exe";
        cfg.autoLaunchTarget = true;
        std::wstring err;
        HWND th = nullptr;
        wchar_t cls[128]{};
        const bool ok = BeginMacroBind(cfg, err, th, cls, 128);
        Emit(L"macro_desktop_launch_bind", ok, ok ? cls : err.c_str());
    }

    // 2) 已有商店记事本时，仍用 System32 路径启动/交接并绑定（复现“打不开指定窗”）
    {
        EnsureStoreNotepadOpen();
        windowmode::WindowModeScriptConfig cfg{};
        cfg.enabled = true;
        cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
        cfg.selectMethod = windowmode::WindowSelectMethod::NoSelect;
        cfg.targetExePath = L"C:\\Windows\\System32\\notepad.exe";
        cfg.autoLaunchTarget = true;
        std::wstring err;
        HWND th = nullptr;
        wchar_t cls[128]{};
        const bool ok = BeginMacroBind(cfg, err, th, cls, 128);
        Emit(L"macro_classic_with_store_open", ok, ok ? cls : err.c_str());
    }

    // 3) 用户常见：商店路径 + 指定窗口类 + RichEdit
    {
        windowmode::WindowModeScriptConfig cfg{};
        cfg.enabled = true;
        cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
        cfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
        cfg.targetExePath =
            L"C:\\Program Files\\WindowsApps\\Microsoft.WindowsNotepad_11.2605.29.0_x64__8wekyb3d8bbwe\\Notepad\\Notepad.exe";
        if (GetFileAttributesW(cfg.targetExePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            cfg.targetExePath = L"C:\\Windows\\System32\\notepad.exe";
            cfg.selectMethod = windowmode::WindowSelectMethod::NoSelect;
        } else {
            cfg.windowClassName = L"Notepad";
            cfg.childWindowClassName = L"RichEditD2DPT";
            cfg.useTopLevelWindow = true;
        }
        cfg.autoLaunchTarget = true;
        std::wstring err;
        HWND th = nullptr;
        wchar_t cls[128]{};
        const bool ok = BeginMacroBind(cfg, err, th, cls, 128);
        Emit(L"macro_store_path_class_bind", ok, ok ? cls : err.c_str());
    }

    // 4) 指定窗口类：必须按标题打开文档，不能只起空白记事本
    {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring dir = std::wstring(tempDir) + L"qst_wm_doc_test";
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring docPath = dir + L"\\qst_named_doc.txt";
        {
            HANDLE hf = CreateFileW(docPath.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                const char* body = "QST_NAMED_DOC\r\n";
                DWORD written = 0;
                WriteFile(hf, body, static_cast<DWORD>(strlen(body)), &written, nullptr);
                CloseHandle(hf);
            }
        }

        // 故意先开一个空白记事本：旧逻辑会绑它并跳过打开文档。
        EnsureStoreNotepadOpen();

        windowmode::WindowModeScriptConfig cfg{};
        cfg.enabled = true;
        cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
        cfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
        cfg.targetExePath = L"C:\\Windows\\System32\\notepad.exe";
        cfg.windowClassName = L"Notepad";
        cfg.childWindowClassName = L"RichEditD2DPT";
        cfg.useTopLevelWindow = true;
        cfg.windowName = L"qst_named_doc.txt - Notepad";
        cfg.targetWindowTitle = cfg.windowName;
        // 同时覆盖：空白记事本在场 + 必须打开标题对应文档（完整 launchArgs）。
        cfg.launchArgs = L"\"" + docPath + L"\"";
        cfg.autoLaunchTarget = true;

        windowmode::WindowModeExecutor exec;
        windowmode::BeginRunOptions opts;
        opts.launchTarget = true;
        opts.launchSearchDir = dir;
        std::wstring err;
        const bool ok = exec.BeginRun(cfg, err, opts);
        HWND th = ok ? exec.TargetHwnd() : nullptr;
        wchar_t title[512]{};
        if (th) {
            HWND root = GetAncestor(th, GA_ROOT);
            if (!root) root = th;
            GetWindowTextW(root, title, 512);
        }
        if (ok) exec.EndRun();

        const bool titleOk = title[0] != L'\0'
            && (wcsstr(title, L"qst_named_doc.txt") != nullptr);
        Emit(L"macro_editor_open_named_doc", ok && titleOk,
            ok ? (titleOk ? title : L"bound but title mismatch") : err.c_str());

        DeleteFileW(docPath.c_str());
        RemoveDirectoryW(dir.c_str());
    }
}

void TestFakeFocusJsonRoundtrip() {
    const std::wstring rawMissing = L"{\"enabled\":1,\"executionKind\":\"hiddenDesktop\"}";
    const auto cfgMissing = windowmode::ParseWindowModeConfigObject(rawMissing);
    const bool missingOk = !cfgMissing.fakeFocusEnabled;

    const std::wstring rawOn =
        L"{\"enabled\":1,\"executionKind\":\"hiddenDesktop\",\"fakeFocusEnabled\":1}";
    const auto cfgOn = windowmode::ParseWindowModeConfigObject(rawOn);
    const bool onOk = cfgOn.fakeFocusEnabled;

    std::wstring written;
    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    cfg.fakeFocusEnabled = true;
    windowmode::WriteWindowModeJson(written, cfg, false);
    const bool writeOk = written.find(L"\"fakeFocusEnabled\": 1") != std::wstring::npos;
    const auto round = windowmode::ParseWindowModeJson(written);
    const bool roundOk = round.fakeFocusEnabled;

    // 关闭窗口模式后写盘/读盘不得保留路径与类名。
    windowmode::WindowModeScriptConfig disabled = cfg;
    disabled.enabled = false;
    disabled.targetExePath = L"C:\\\\Games\\\\a.exe";
    disabled.windowClassName = L"Chrome_WidgetWin_1";
    disabled.windowName = L"stale";
    std::wstring writtenOff;
    windowmode::WriteWindowModeJson(writtenOff, disabled, false);
    const auto parsedOff = windowmode::ParseWindowModeJson(writtenOff);
    const bool clearOk = !parsedOff.enabled
        && parsedOff.targetExePath.empty()
        && parsedOff.windowClassName.empty()
        && parsedOff.windowName.empty()
        && writtenOff.find(L"C:\\\\Games") == std::wstring::npos;

    const std::wstring rawStale =
        L"{\"enabled\":0,\"targetExePath\":\"C:\\\\old.exe\",\"windowClassName\":\"X\"}";
    const auto stale = windowmode::ParseWindowModeConfigObject(rawStale);
    const bool loadClearOk = !stale.enabled
        && stale.targetExePath.empty()
        && stale.windowClassName.empty();

    const bool ok = missingOk && onOk && writeOk && roundOk && clearOk && loadClearOk;
    Emit(L"fake_focus_json_roundtrip", ok,
        ok ? L"" : L"fakeFocusEnabled JSON roundtrip / disabled-clear failed");
}

void TestInputStrategyCdpAuto() {
    windowmode::WindowModeScriptConfig chrome{};
    chrome.enabled = true;
    chrome.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    chrome.windowClassName = L"Chrome_WidgetWin_1";
    chrome.fakeFocusEnabled = true;
    chrome.inputStrategy = windowmode::WindowModeInputStrategy::Auto;

    const bool resolveOk =
        windowmode::ResolveInputStrategy(chrome) == windowmode::WindowModeInputStrategy::Cdp
        && windowmode::UsesCdpInput(chrome)
        && !windowmode::UsesFakeFocus(chrome)
        && !windowmode::ShouldMinimizeTargetAfterBind(chrome);

    windowmode::AnnotateInputStrategyForSave(chrome);
    std::wstring written;
    windowmode::WriteWindowModeJson(written, chrome, false);
    const bool writeOk = written.find(L"\"inputStrategy\": \"cdp\"") != std::wstring::npos
        && written.find(L"\"cdpPort\":") != std::wstring::npos;
    const auto round = windowmode::ParseWindowModeJson(written);
    const bool roundOk = round.inputStrategy == windowmode::WindowModeInputStrategy::Cdp
        && round.cdpPort == 9222;

    windowmode::WindowModeScriptConfig game{};
    game.enabled = true;
    game.windowClassName = L"UnityWndClass";
    game.inputStrategy = windowmode::WindowModeInputStrategy::Auto;
    game.fakeFocusEnabled = true;
    const bool gameOk =
        windowmode::ResolveInputStrategy(game) == windowmode::WindowModeInputStrategy::SoftMessage
        && !windowmode::UsesCdpInput(game)
        && windowmode::UsesFakeFocus(game);

    const std::wstring args = windowmode::EnsureRemoteDebuggingLaunchArgs(L"", 9222);
    const bool argsOk = args.find(L"--remote-debugging-port=9222") != std::wstring::npos;

    // 显式 softMessage 不被 Annotate 覆盖
    windowmode::WindowModeScriptConfig forceSoft = chrome;
    forceSoft.inputStrategy = windowmode::WindowModeInputStrategy::SoftMessage;
    windowmode::AnnotateInputStrategyForSave(forceSoft);
    const bool forceOk = forceSoft.inputStrategy == windowmode::WindowModeInputStrategy::SoftMessage;

    const bool ok = resolveOk && writeOk && roundOk && gameOk && argsOk && forceOk;
    Emit(L"input_strategy_cdp_auto", ok,
        ok ? L"" : L"CDP/softMessage strategy resolution or JSON annotate failed");
}

void TestExtBridgeConfigParse() {
    int port = 0;
    std::string token;
    const bool good = windowmode::ParseExtBridgeConfigJson(
        "{\"ok\":true,\"port\":19228,\"token\":\"0123456789abcdef\"}", port, token)
        && port == 19228
        && token == "0123456789abcdef";
    int badPort = 0;
    std::string badTok;
    const bool reject = !windowmode::ParseExtBridgeConfigJson(
        "{\"port\":80,\"token\":\"short\"}", badPort, badTok);
    Emit(L"ext_bridge_config_parse", good && reject,
        (good && reject) ? L"" : L"ext bridge config parse failed");
}

void TestFakeFocusMinimizeGate() {
    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    cfg.fakeFocusEnabled = false;
    const bool offOk = !windowmode::UsesFakeFocus(cfg)
        && windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.fakeFocusEnabled = true;
    const bool onOk = windowmode::UsesFakeFocus(cfg)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    const bool bgOk = !windowmode::UsesFakeFocus(cfg)
        && windowmode::ShouldMinimizeTargetAfterBind(cfg);

    const bool ok = offOk && onOk && bgOk;
    Emit(L"fake_focus_minimize_gate", ok,
        ok ? L"" : L"UsesFakeFocus / minimize gate mismatch");
}

void TestRestorePreferMaximized() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QstRestoreMaxSelfTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"QstRestoreMax",
        WS_OVERLAPPEDWINDOW, 80, 80, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        Emit(L"restore_prefer_maximized", false, L"CreateWindow failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    const bool maxOk = IsZoomed(hwnd) != FALSE;
    ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    const bool minOk = IsIconic(hwnd) != FALSE;
    // 安静铺满工作区：禁止依赖 IsZoomed（Maximize API 会切虚拟桌面）。
    const bool restoredMax = windowmode::RestoreMinimizedQuietPreferMax(hwnd)
        && !IsIconic(hwnd)
        && windowmode::WindowFillsWorkArea(hwnd)
        && !IsZoomed(hwnd);

    ShowWindow(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    MoveWindow(hwnd, 100, 100, 700, 500, TRUE);
    ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    const bool restoredNormal = windowmode::RestoreMinimizedQuietPreferMax(hwnd)
        && !IsIconic(hwnd)
        && !windowmode::WindowFillsWorkArea(hwnd)
        && !IsZoomed(hwnd);

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    const bool ok = maxOk && minOk && restoredMax && restoredNormal;
    wchar_t detail[160]{};
    swprintf_s(detail, L"max=%d min=%d fill=%d noZoom=%d restNorm=%d",
        maxOk ? 1 : 0, minOk ? 1 : 0, restoredMax ? 1 : 0,
        restoredMax ? 1 : 0, restoredNormal ? 1 : 0);
    Emit(L"restore_prefer_maximized", ok, detail);
}

void TestSoftMessageExeGates() {
    // 原 exe 链路：非 Chrome 类 → softMessage；默认可最小化；假焦点则保持还原。
    windowmode::WindowModeScriptConfig exe{};
    exe.enabled = true;
    exe.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    exe.windowClassName = L"UnityWndClass";
    exe.inputStrategy = windowmode::WindowModeInputStrategy::Auto;
    exe.fakeFocusEnabled = false;

    const bool softOk =
        windowmode::ResolveInputStrategy(exe) == windowmode::WindowModeInputStrategy::SoftMessage
        && !windowmode::UsesCdpInput(exe)
        && !windowmode::UsesFakeFocus(exe)
        && windowmode::ShouldMinimizeTargetAfterBind(exe);

    exe.fakeFocusEnabled = true;
    const bool ffOk =
        windowmode::UsesFakeFocus(exe)
        && !windowmode::UsesCdpInput(exe)
        && !windowmode::ShouldMinimizeTargetAfterBind(exe);

    // 显式 softMessage 即使用 Chrome 类也不走扩展桥策略。
    windowmode::WindowModeScriptConfig forceSoft{};
    forceSoft.enabled = true;
    forceSoft.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    forceSoft.windowClassName = L"Chrome_WidgetWin_1";
    forceSoft.inputStrategy = windowmode::WindowModeInputStrategy::SoftMessage;
    forceSoft.fakeFocusEnabled = true;
    const bool forceOk =
        windowmode::ResolveInputStrategy(forceSoft) == windowmode::WindowModeInputStrategy::SoftMessage
        && !windowmode::UsesCdpInput(forceSoft)
        && windowmode::UsesFakeFocus(forceSoft);

    const bool ok = softOk && ffOk && forceOk;
    Emit(L"soft_message_exe_gates", ok,
        ok ? L"" : L"softMessage/exe strategy or minimize/fakeFocus gates wrong");
}

std::wstring SelfExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path);
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return full.substr(0, slash + 1);
}

void TestFakeFocusHookLocal() {
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        Emit(L"fake_focus_hook_local", false,
            (L"LoadLibrary failed: " + dllPath).c_str());
        return;
    }

    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    using IsInstalledFn = BOOL(WINAPI*)();
    auto* install = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_Install"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    auto* isInstalled = reinterpret_cast<IsInstalledFn>(GetProcAddress(mod, "FakeFocus_IsInstalled"));
    if (!install || !uninstall || !isInstalled) {
        FreeLibrary(mod);
        Emit(L"fake_focus_hook_local", false, L"missing FakeFocus exports");
        return;
    }

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"QST FakeFocus Probe",
        WS_OVERLAPPEDWINDOW, 40, 40, 240, 120, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        FreeLibrary(mod);
        Emit(L"fake_focus_hook_local", false, L"CreateWindow failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    HWND before = GetForegroundWindow();
    const BOOL installed = install(hwnd);
    HWND hooked = GetForegroundWindow();
    const BOOL active = isInstalled();
    uninstall();
    HWND after = GetForegroundWindow();
    DestroyWindow(hwnd);
    FreeLibrary(mod);

    const bool ok = installed && active && hooked == hwnd
        && (after == before || after != hwnd);
    wchar_t detail[256]{};
    swprintf_s(detail,
        L"install=%d hooked=%p expect=%p before=%p after=%p",
        installed ? 1 : 0, hooked, hwnd, before, after);
    Emit(L"fake_focus_hook_local", ok, ok ? L"" : detail);
}

void TestFakeFocusSoftInput() {
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif

    std::wstring softErr;
    if (!windowmode::FakeFocusSoftInput_Attach(GetCurrentProcessId(), softErr)) {
        Emit(L"fake_focus_soft_input", false,
            softErr.empty() ? L"Attach soft input failed" : softErr.c_str());
        return;
    }

    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        windowmode::FakeFocusSoftInput_Detach();
        Emit(L"fake_focus_soft_input", false, L"LoadLibrary failed");
        return;
    }

    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    using HasSoftFn = BOOL(WINAPI*)();
    auto* install = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_Install"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    auto* hasSoft = reinterpret_cast<HasSoftFn>(GetProcAddress(mod, "FakeFocus_HasSoftInput"));
    if (!install || !uninstall || !hasSoft) {
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(L"fake_focus_soft_input", false, L"missing exports");
        return;
    }

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"QST SoftInput Probe",
        WS_OVERLAPPEDWINDOW, 80, 80, 200, 100, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(L"fake_focus_soft_input", false, L"CreateWindow failed");
        return;
    }

    if (!install(hwnd) || !hasSoft()) {
        uninstall();
        DestroyWindow(hwnd);
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(L"fake_focus_soft_input", false, L"Install/HasSoftInput failed");
        return;
    }

    windowmode::FakeFocusSoftInput_SetCursorScreen(1234, 5678);
    windowmode::FakeFocusSoftInput_SetMouseButtonVk(VK_LBUTTON, true);
    windowmode::FakeFocusSoftInput_SetKey(VK_SPACE, true);

    POINT pt{};
    const BOOL cursorOk = GetCursorPos(&pt);
    const SHORT lbtn = GetAsyncKeyState(VK_LBUTTON);
    const SHORT space = GetAsyncKeyState(VK_SPACE);
    BYTE keys[256]{};
    const BOOL kbOk = GetKeyboardState(keys);

    uninstall();
    DestroyWindow(hwnd);
    FreeLibrary(mod);
    windowmode::FakeFocusSoftInput_Detach();

    const bool ok = cursorOk && pt.x == 1234 && pt.y == 5678
        && (lbtn & 0x8000) != 0
        && (space & 0x8000) != 0
        && kbOk && (keys[VK_LBUTTON] & 0x80) != 0
        && (keys[VK_SPACE] & 0x80) != 0;
    wchar_t detail[256]{};
    swprintf_s(detail, L"pt=(%ld,%ld) lbtn=0x%04x space=0x%04x kbL=0x%02x kbSp=0x%02x",
        pt.x, pt.y, static_cast<unsigned>(lbtn) & 0xFFFF,
        static_cast<unsigned>(space) & 0xFFFF,
        keys[VK_LBUTTON], keys[VK_SPACE]);
    Emit(L"fake_focus_soft_input", ok, ok ? L"" : detail);
}

std::wstring ReadFileUtf8AsWide(const std::wstring& path) {
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return L"";
    const DWORD size = GetFileSize(hf, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0 || size > 8 * 1024 * 1024) {
        CloseHandle(hf);
        return L"";
    }
    std::string bytes(size, '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(hf, bytes.data(), size, &read, nullptr);
    CloseHandle(hf);
    if (!ok || read == 0) return L"";
    bytes.resize(read);
    if (bytes.size() >= 3
        && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
        wide.data(), n);
    return wide;
}

std::wstring FindAnjuzhenScriptPath() {
    const std::wstring exeDir = SelfExeDir();
    const wchar_t* rel[] = {
        L"scripts\\安居镇.json",
        L"..\\Debug\\scripts\\安居镇.json",
        L"..\\Release\\scripts\\安居镇.json",
        L"..\\..\\build\\Debug\\scripts\\安居镇.json",
        L"..\\..\\build\\Release\\scripts\\安居镇.json",
    };
    for (const wchar_t* r : rel) {
        const std::wstring path = exeDir + r;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    // Absolute fallback for this workspace.
    const std::wstring abs = L"D:\\other\\software\\build\\Debug\\scripts\\安居镇.json";
    if (GetFileAttributesW(abs.c_str()) != INVALID_FILE_ATTRIBUTES) return abs;
    return L"";
}

void TestAnjuzhenScriptWmConfig() {
    // 不依赖用户脚本文件是否仍在 build/*/scripts（易被清掉）；用内嵌样例校验解析。
    const std::wstring sample =
        L"{\n"
        L"  \"windowMode\": {\n"
        L"    \"enabled\": 1,\n"
        L"    \"executionKind\": \"hiddenDesktop\",\n"
        L"    \"windowClassName\": \"Chrome_WidgetWin_1\",\n"
        L"    \"childWindowClassName\": \"Chrome_RenderWidgetHostHWND\",\n"
        L"    \"fakeFocusEnabled\": 1\n"
        L"  }\n"
        L"}\n";
    const auto cfg = windowmode::ParseWindowModeJson(sample);
    const bool ok = cfg.enabled
        && cfg.executionKind == windowmode::WindowModeExecutionKind::HiddenDesktop
        && cfg.fakeFocusEnabled
        && cfg.windowClassName.find(L"Chrome_WidgetWin") != std::wstring::npos
        && cfg.childWindowClassName == L"Chrome_RenderWidgetHostHWND"
        && windowmode::ResolveInputStrategy(cfg) == windowmode::WindowModeInputStrategy::Cdp
        && windowmode::UsesCdpInput(cfg)
        && !windowmode::UsesFakeFocus(cfg)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    // 运行时：非 Chrome_ 类窗不应被判定为「假焦点不支持」。
    HWND probe = CreateWindowExW(0, L"STATIC", L"qst_wm_probe",
        WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const bool helperOk = !probe
        || !windowmode::IsBrowserFakeFocusUnsupported(probe);
    if (probe) DestroyWindow(probe);

    Emit(L"anjuzhen_script_wm_config", ok && helperOk,
        ok && helperOk ? L"sample JSON + CDP strategy"
                       : (ok ? L"IsBrowserFakeFocusUnsupported helper mismatch"
                             : L"windowMode sample parse or helper mismatch"));
}

constexpr wchar_t kInvisibleChildClass[] = L"QstInvisibleChildSelfTest";

void TestInvisibleChildClassBind() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kInvisibleChildClass;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = true;
    }

    HWND parent = CreateWindowExW(0, L"STATIC", L"QST InvisibleChild Parent",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 400, 300,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent) {
        Emit(L"invisible_child_class_bind", false, L"parent create failed");
        return;
    }
    // Intentionally omit WS_VISIBLE — mimics Chrome_RenderWidgetHostHWND off virtual desktop.
    HWND child = CreateWindowExW(0, kInvisibleChildClass, L"",
        WS_CHILD, 10, 10, 220, 180,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!child) {
        DestroyWindow(parent);
        Emit(L"invisible_child_class_bind", false, L"invisible child create failed");
        return;
    }

    HWND found = windowmode::FindChildWindowByClass(parent, kInvisibleChildClass);
    const bool ok = found == child && !IsWindowVisible(child);
    Emit(L"invisible_child_class_bind", ok,
        ok ? L"found invisible child by class" : L"FindChildWindowByClass missed invisible child");
    DestroyWindow(parent);
}

constexpr wchar_t kD3dClass[] = L"Intermediate D3D Window";
constexpr wchar_t kRenderClass[] = L"Chrome_RenderWidgetHostHWND";

void TestBrowserRenderSkipsD3d() {
    static bool registered = false;
    if (!registered) {
        for (const wchar_t* name : {kD3dClass, kRenderClass}) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = name;
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            RegisterClassExW(&wc);
        }
        registered = true;
    }

    HWND parent = CreateWindowExW(0, L"STATIC", L"QST Browser Parent",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 500, 400,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent) {
        Emit(L"browser_render_skips_d3d", false, L"parent create failed");
        return;
    }

    HWND d3d = CreateWindowExW(0, kD3dClass, L"",
        WS_CHILD, 0, 0, 480, 360,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    // Zero-area render widget — must still win over D3D for input binding.
    HWND render = CreateWindowExW(0, kRenderClass, L"",
        WS_CHILD, 0, 0, 0, 0,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!d3d || !render) {
        DestroyWindow(parent);
        Emit(L"browser_render_skips_d3d", false, L"child create failed");
        return;
    }

    HWND found = windowmode::FindBrowserRenderWidget(parent);
    const bool preferRender = found == render;
    DestroyWindow(render);

    HWND foundD3dOnly = windowmode::FindBrowserRenderWidget(parent);
    const bool skipD3d = foundD3dOnly == nullptr
        && windowmode::IsBrowserCompositorHwnd(d3d)
        && windowmode::FindBrowserCaptureSurface(parent) == d3d;

    const bool ok = preferRender && skipD3d;
    Emit(L"browser_render_skips_d3d", ok,
        ok ? L"render preferred; d3d capture-only"
           : L"FindBrowserRenderWidget incorrectly used Intermediate D3D");
    DestroyWindow(parent);
}

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef DWMWA_DISALLOW_PEEK
#define DWMWA_DISALLOW_PEEK 11
#endif

void TestCdpParkExpandable() {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"QST CDP Park Probe",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 90, 90, 640, 480,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        Emit(L"cdp_park_expandable", false, L"CreateWindow failed");
        return;
    }
    ShowWindow(hwnd, SW_RESTORE);
    UpdateWindow(hwnd);

    // 模拟旧版 Cloak 残余：Cloak+α=1。Park 必须刮干净。
    BOOL cloakOn = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloakOn, sizeof(cloakOn));
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, GetWindowLongPtr(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
    windowmode::SuppressMacroDesktopTaskbarPreview(hwnd);

    // Park：刮掉 Cloak/α=1；Minimize→Move 宏桌面（禁屏外）。
    windowmode::PrepareMacroDesktopForCdpBind(hwnd);

    BYTE alphaAfterPark = 255;
    auto readGhost = [&](BYTE& alphaOut) {
        alphaOut = 255;
        DWORD flags = 0;
        COLORREF key = 0;
        const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        BOOL cloaked = 0;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (ex & WS_EX_LAYERED) {
            GetLayeredWindowAttributes(hwnd, &key, &alphaOut, &flags);
        }
        const bool layeredGhost = (ex & WS_EX_LAYERED) != 0
            && (flags & LWA_ALPHA) != 0 && alphaOut <= 2;
        const bool appCloak = (static_cast<DWORD>(cloaked) & DWM_CLOAKED_APP) != 0;
        return layeredGhost || appCloak;
    };

    const bool notLatched = !windowmode::IsMacroVisionLatched(hwnd);
    const bool peekCleared = !windowmode::IsMacroDesktopTaskbarPreviewSuppressed(hwnd);
    const bool isGhost = readGhost(alphaAfterPark);
    const bool alwaysExpandable = !isGhost; // 无 Cloak 鬼影；任务栏可展开

    // 第二次 Park：不得再切屏操作；保持可查询状态。
    windowmode::PrepareMacroDesktopForCdpBind(hwnd);
    BYTE alphaAfterRaise = 255;
    windowmode::RaiseMacroDesktopWindowForWatch(hwnd); // 用户不在宏桌面应为 no-op
    const bool stillNotGhost = !readGhost(alphaAfterRaise);

    // 结束路径：恢复停放前坐标再最小化，任务栏还原不得落在 -32000。
    windowmode::RestoreMacroDesktopWindowAfterRun(hwnd);
    WINDOWPLACEMENT wpEnd{};
    wpEnd.length = sizeof(WINDOWPLACEMENT);
    const bool gotWp = GetWindowPlacement(hwnd, &wpEnd) == TRUE;
    const bool restoreRectOk = gotWp
        && wpEnd.rcNormalPosition.left > -10000
        && wpEnd.rcNormalPosition.top > -10000;
    ShowWindow(hwnd, SW_RESTORE);
    UpdateWindow(hwnd);
    RECT wr{};
    const bool gotWr = GetWindowRect(hwnd, &wr) == TRUE;
    const bool onScreen = gotWr && wr.left > -10000 && wr.top > -10000
        && wr.right > wr.left + 64 && wr.bottom > wr.top + 64;

    windowmode::ReleaseMacroDesktopVisionLatch(hwnd);
    windowmode::ClearMacroDesktopTaskbarPreviewSuppression(hwnd);
    DestroyWindow(hwnd);

    const bool ok = notLatched && peekCleared && alwaysExpandable && stillNotGhost
        && restoreRectOk && onScreen;
    wchar_t detail[320]{};
    swprintf_s(detail,
        L"noLatch=%d peekOff=%d ghost=%d exp=%d aPark=%u aRaise=%u restOk=%d onScr=%d rc=(%ld,%ld)",
        notLatched ? 1 : 0, peekCleared ? 1 : 0,
        isGhost ? 1 : 0, alwaysExpandable ? 1 : 0,
        static_cast<unsigned>(alphaAfterPark), static_cast<unsigned>(alphaAfterRaise),
        restoreRectOk ? 1 : 0, onScreen ? 1 : 0,
        gotWr ? wr.left : 0L, gotWr ? wr.top : 0L);
    Emit(L"cdp_park_expandable", ok,
        ok ? L"park: macro Move; no ghost; EndRun restore ok" : detail);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool runMacro = false;
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--json") == 0) {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--macro") == 0) runMacro = true;
        else if (_wcsicmp(argv[i], L"--list") == 0) {
            listOnly = true;
            selftest::InitUtf8Stdout();
        }
        else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            PrintHelp();
            return 0;
        }
    }

    if (listOnly) {
        selftest::PrintCaseList(L"WindowModeSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    if (!gJson) {
        std::fwprintf(stderr,
            L"=== WindowModeSelfTest ===\n"
            L"(Agent: .cursor/skills/module-selftest/SKILL.md)\n");
    }

    TestQuoteArgs();
    TestNoSelectIgnoresDocument();
    TestImeFilter();

    HWND edit = CreateTestEditWindow();
    if (!edit) {
        Emit(L"create_test_window", false, L"CreateWindow failed");
        Emit(L"find_main_window", false, L"skipped");
        Emit(L"post_quick_input", false, L"skipped");
        Emit(L"background_bind_child", false, L"skipped");
        Emit(L"background_quick_input", false, L"skipped");
        Emit(L"quick_input_cancel", false, L"skipped");
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        TestFindTarget(edit);
        TestPostQuickInput(edit);
        TestExecutorBackground(edit);
        TestQuickInputCancel(edit);
        DestroyWindow(ParentTopWindow(edit));
    }

    TestDesktopQuickInputCancel();
    TestFakeFocusJsonRoundtrip();
    TestInputStrategyCdpAuto();
    TestExtBridgeConfigParse();
    TestFakeFocusMinimizeGate();
    TestSoftMessageExeGates();
    TestRestorePreferMaximized();
    TestFakeFocusHookLocal();
    TestFakeFocusSoftInput();
    TestAnjuzhenScriptWmConfig();
    TestInvisibleChildClassBind();
    TestBrowserRenderSkipsD3d();
    TestCdpParkExpandable();

    if (runMacro) TestMacroDesktopSmoke();

    selftest::EmitSummary();
    if (!gJson && selftest::gFailed != 0) {
        std::fwprintf(stderr,
            L"Hint: FAIL name → .cursor/skills/window-mode-debug/reference.md\n");
    }
    return selftest::ExitCode();
}
