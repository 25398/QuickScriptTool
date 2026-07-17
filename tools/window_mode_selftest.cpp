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
#include "window_mode/window_target.h"
#include "window_mode/background_window_input.h"
#include "action_utils.h"

#include <shellapi.h>

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

    if (runMacro) TestMacroDesktopSmoke();

    selftest::EmitSummary();
    if (!gJson && selftest::gFailed != 0) {
        std::fwprintf(stderr,
            L"Hint: FAIL name → .cursor/skills/window-mode-debug/reference.md\n");
    }
    return selftest::ExitCode();
}
