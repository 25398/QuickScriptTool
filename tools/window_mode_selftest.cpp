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
#include "window_mode/window_coords.h"
#include "window_mode/background_window_input.h"
#include "window_mode/background_input_target.h"
#include "window_mode/background_uia_input.h"
#include "window_mode/window_list.h"
#include "window_mode/window_mode_permission.h"
#include "window_mode/ext_bridge/ext_bridge_server.h"
#include "window_mode/fake_focus/fake_focus_soft_input_host.h"
#include "window_mode/virtual_desktop_accessor.h"
#include "window_mode/injection/inject_common.h"
#include "action_utils.h"

#include <shellapi.h>
#include <dwmapi.h>
#include <winternl.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
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
    {L"post_key_click_char", L"default",
        L"PostKeyToWindow KEYDOWN+WM_CHAR inserts printable char into EDIT (background soft keys)"},
    {L"post_key_shift_char", L"default",
        L"Soft Shift+1 posts WM_CHAR '!' into EDIT"},
    {L"background_bind_child", L"default",
        L"Background BeginRun binds child EDIT, not only top-level"},
    {L"background_input_android_render", L"default",
        L"FindBackgroundInputChild locates TheRender under synthetic LDPlayer tree"},
    {L"background_input_coord_map", L"default",
        L"MapClientPointBetweenHwnds applies toolbar offset parent→render child"},
    {L"background_input_sdl_surface", L"default",
        L"FindBackgroundInputChild prefers SDL_app render child over top-level"},
    {L"background_input_mumu_qt_recursive", L"default",
        L"FindBackgroundInputChild finds nested Qt QWindowIcon under MuMu title"},
    {L"background_input_desktop_emu_top", L"default",
        L"FindBackgroundInputChild keeps DeSmuME top (not toolbar/largest child) when config=null"},
    {L"android_qt_fake_focus_gate", L"default",
        L"MuMu/LDPlayer must not auto fake-focus (PostMessage to render child)"},
    {L"background_quick_input", L"default",
        L"WindowModeExecutor background quick-input succeeds"},
    {L"background_click_keeps_foreground", L"default",
        L"Background PostMessage click does not steal FG; minimized target may quiet-restore"},
    {L"window_client_scale", L"default",
        L"ScaleWindowClientPoint / find-image template scale use recorded vs live client size"},
    {L"window_relative_playback_enables_wm", L"default",
        L"录制回放 reviveEnabled 才把 enabled=0 复活；编辑器默认模式不得复活"},
    {L"background_minimized_quiet_restore", L"default",
        L"BeginRun on iconic target quiet-restores without stealing FG; EndRun re-minimizes"},
    {L"screen_point_to_client_within", L"default",
        L"Screen point inside target client maps; outside is rejected"},
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
    {L"kernel_anticheat_blocks_background", L"default",
        L"RiotWindowClass / League client must refuse background window (no inject, no PostMessage)"},
    {L"input_strategy_cdp_auto", L"default",
        L"Chrome_WidgetWin class → CDP strategy on save/resolve; game exe stays softMessage"},
    {L"ext_bridge_config_parse", L"default",
        L"ParseExtBridgeConfigJson accepts port/token; rejects bad JSON"},
    {L"fake_focus_minimize_gate", L"default",
        L"UsesFakeFocus for HiddenDesktop/BackgroundWindow + flag or Unity/TFrmMain class; then no minimize-after-bind"},
    {L"soft_message_exe_gates", L"default",
        L"Win32 exe: softMessage + minimize; Unity auto FakeFocus keeps restored; explicit softMessage skips CDP"},
    {L"restore_prefer_maximized", L"default",
        L"RestoreMinimizedQuietPreferMax restores Zoomed after minimize; normal stay unzoomed"},
    {L"monitor_covering_fullscreen", L"default",
        L"LooksLikeMonitorCoveringFullscreen: WS_POPUP covering monitor; framed/small windows excluded"},
    {L"game_hardware_without_inject", L"default",
        L"Windowed Unreal without fake-focus injection needs hardware SendInput; do not park offscreen"},
    {L"hardware_offscreen_park", L"default",
        L"Windowed hardware target parks off-screen + topmost and restores placement"},
    {L"vda_selects_os_dll", L"default",
        L"VirtualDesktopAccessor picks Win11 24H2+/23H2/Win10 DLL by OS build; no cross-OS fallback"},
    {L"fake_focus_hook_local", L"default",
        L"Load FakeFocus64/32 locally: GetForegroundWindow returns target; uninstall restores"},
    {L"fake_focus_lite_unreal", L"default",
        L"FakeFocus_InstallLite fakes foreground without hooking PeekMessage"},
    {L"fake_focus32_export_rva", L"default",
        L"FindExportRva resolves FakeFocus_InstallLite in FakeFocus32.dll (x86 stdcall names OK)"},
    {L"remote_module_kernel32", L"default",
        L"FindRemoteModule / PEB walk resolve kernel32!LoadLibraryW (Unity Toolhelp ERROR_BAD_LENGTH)"},
    {L"fake_focus_header_export_rva", L"default",
        L"FindExportRva finds FakeFocus_Install when export names live in SizeOfHeaders"},
    {L"fake_focus_glfw_lite_cursor", L"default",
        L"GLFW30 lite still hooks GetCursorPos (click coords); SetCursorPos warp swallowed"},
    {L"fake_focus_air_focus_only", L"default",
        L"ApolloRuntime AIR: fake GetForegroundWindow, no WndProc subclass, SetCursorPos not swallowed"},
    {L"fake_focus_maplestory_focus_only", L"default",
        L"MapleStoryClass IAT: fake GetCursorPos/GetAsyncKeyState, no WndProc subclass, no WM_INPUT"},
    {L"fake_focus_soft_input", L"default",
        L"Phase2: soft shared memory drives GetCursorPos/GetAsyncKeyState/GetKeyboardState + Raw Input"},
    {L"anjuzhen_script_wm_config", L"default",
        L"Parse build/*/scripts/安居镇.json windowMode: fakeFocus + Chrome child class"},
    {L"permission_match_uipi", L"default",
        L"CheckPermissionMatch: self/0 ok; explorer allowed even if this process is elevated"},
    {L"maplestory_bg_fake_focus", L"default",
        L"MapleStoryClass / MapleStory.exe / 冒险岛 title → game class + BackgroundWindow UsesFakeFocus"},
    {L"window_mode_target_lost_stops", L"default",
        L"BeginRun then DestroyWindow → TargetStillAlive is false (game crash must stop the script)"},
    {L"invisible_child_class_bind", L"default",
        L"FindChildWindowByClass finds WS_CHILD without WS_VISIBLE (macro-desktop case)"},
    {L"browser_render_skips_d3d", L"default",
        L"FindBrowserRenderWidget ignores Intermediate D3D; prefers RenderWidget or null"},
    {L"cdp_park_expandable", L"default",
        L"PrepareMacroDesktopForCdpBind: Move macro desk; no Cloak; no offscreen"},
    {L"window_list_enumerates_self", L"default",
        L"ListSwitchableWindows finds the self-test top window in Z order; own pid excluded"},
    {L"window_list_match_and_format", L"default",
        L"MatchWindows filters by title/process substring; FormatWindowList numbers from #1"},
    {L"window_activate_foreground", L"default",
        L"ActivateWindow brings the self-test window to foreground (restores if minimized)"},
    {L"window_activate_by_process", L"default",
        L"ActivateByProcessName activates frontmost window of given process"},
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

void TestPostKeyClickChar(HWND edit) {
    SetWindowTextW(edit, L"");
    windowmode::ResetSoftMouseState();
    // 模拟先点到输入框再按键（与回放「点一下再打字」对齐）。
    RECT rc{};
    GetClientRect(edit, &rc);
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    windowmode::RememberSoftMouseClientPos(edit, cx, cy);
    windowmode::PostKeyToWindow(edit, 'A', true);
    windowmode::PostKeyToWindow(edit, 'A', false);
    windowmode::PostKeyToWindow(edit, 'B', true);
    windowmode::PostKeyToWindow(edit, 'B', false);
    PumpMessagesFor(std::chrono::milliseconds(80));
    const std::wstring text = ReadEditText(edit);
    const bool ok = text.find(L'a') != std::wstring::npos
        || text.find(L'A') != std::wstring::npos;
    const bool okB = text.find(L'b') != std::wstring::npos
        || text.find(L'B') != std::wstring::npos;
    Emit(L"post_key_click_char", ok && okB, text.c_str());
}

void TestPostKeyShiftChar(HWND edit) {
    SetWindowTextW(edit, L"");
    windowmode::ResetSoftMouseState();
    windowmode::RememberSoftMouseClientPos(edit, 4, 4);
    // Shift+1 → '!'（软修饰须同步到 ToUnicode）
    windowmode::PostKeyToWindow(edit, VK_LSHIFT, true);
    windowmode::PostKeyToWindow(edit, '1', true);
    windowmode::PostKeyToWindow(edit, '1', false);
    windowmode::PostKeyToWindow(edit, VK_LSHIFT, false);
    PumpMessagesFor(std::chrono::milliseconds(80));
    const std::wstring text = ReadEditText(edit);
    Emit(L"post_key_shift_char", text.find(L'!') != std::wstring::npos, text.c_str());
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

    const auto mouseAlias = windowmode::ParseWindowModeConfigObject(
        L"{\"enabled\":1,\"selectMethod\":\"mousePositionOnStartup\"}");
    const bool aliasOk =
        mouseAlias.selectMethod == windowmode::WindowSelectMethod::SelectOnStartup
        && windowmode::NormalizeSelectMethod(
            windowmode::WindowSelectMethod::MousePositionOnStartup)
            == windowmode::WindowSelectMethod::SelectOnStartup
        && windowmode::SelectMethodToComboIndex(
            windowmode::WindowSelectMethod::UseEditorWindowClass) == 1
        && windowmode::ComboIndexToSelectMethod(1)
            == windowmode::WindowSelectMethod::UseEditorWindowClass
        && windowmode::ComboIndexToSelectMethod(0)
            == windowmode::WindowSelectMethod::SelectOnStartup;
    std::wstring writtenAlias;
    windowmode::WriteWindowModeJson(writtenAlias, mouseAlias, false);
    const bool writeAliasOk =
        writtenAlias.find(L"\"selectMethod\": \"selectOnStartup\"") != std::wstring::npos
        && writtenAlias.find(L"mousePositionOnStartup") == std::wstring::npos;

    const auto startup = windowmode::ParseWindowModeConfigObject(
        L"{\"enabled\":1,\"selectMethod\":\"selectOnStartup\"}");
    const auto missingMethod = windowmode::ParseWindowModeConfigObject(
        L"{\"enabled\":1}");
    const auto compactStartup = windowmode::ParseWindowModeConfigObject(
        L"{\"enabled\":1,\"selectMethod\":\"selectOnStartup\",\"windowClassName\":\"TFrmMain\"}");
    const auto leftover = windowmode::ParseWindowModeConfigObject(
        L"{\"enabled\":1,\"selectMethod\":\"selectOnStartup\","
        L"\"windowClassName\":\"TFrmMain\",\"targetExePath\":\"C:\\\\g.exe\","
        L"\"autoLaunchTarget\":1,\"launchArgs\":\"--x\"}");
    windowmode::WindowModeScriptConfig persistCfg{};
    persistCfg.enabled = true;
    persistCfg.selectMethod = windowmode::WindowSelectMethod::SelectOnStartup;
    persistCfg.windowClassName = L"TFrmMain";
    persistCfg.targetExePath = L"C:\\g.exe";
    persistCfg.autoLaunchTarget = true;
    std::wstring writtenStartup;
    windowmode::WriteWindowModeJson(writtenStartup, persistCfg, false);
    const auto persistRound = windowmode::ParseWindowModeJson(writtenStartup);
    windowmode::WindowModeScriptConfig classCfg{};
    classCfg.enabled = true;
    classCfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
    classCfg.windowClassName = L"Chrome_WidgetWin_1";
    classCfg.targetExePath = L"C:\\msedge.exe";
    std::wstring writtenClass;
    windowmode::WriteWindowModeJson(writtenClass, classCfg, false);
    const auto classRound = windowmode::ParseWindowModeJson(writtenClass);
    const bool persistOk =
        startup.selectMethod == windowmode::WindowSelectMethod::SelectOnStartup
        && missingMethod.selectMethod == windowmode::WindowSelectMethod::SelectOnStartup
        && compactStartup.selectMethod == windowmode::WindowSelectMethod::SelectOnStartup
        && compactStartup.windowClassName == L"TFrmMain"
        && leftover.selectMethod == windowmode::WindowSelectMethod::SelectOnStartup
        && leftover.windowClassName == L"TFrmMain"
        && persistRound.selectMethod == windowmode::WindowSelectMethod::SelectOnStartup
        && persistRound.windowClassName.empty()
        && persistRound.targetExePath.empty()
        && writtenStartup.find(L"TFrmMain") == std::wstring::npos
        && writtenStartup.find(L"\"selectMethod\": \"selectOnStartup\"") != std::wstring::npos
        && classRound.selectMethod == windowmode::WindowSelectMethod::UseEditorWindowClass
        && classRound.windowClassName == L"Chrome_WidgetWin_1"
        && classRound.targetExePath == L"C:\\msedge.exe"
        && windowmode::SelectMethodFromJson(L"mousePositionOnStartup")
            == windowmode::WindowSelectMethod::SelectOnStartup
        && windowmode::SelectMethodFromJsonUtf8("selectOnStartup")
            == windowmode::WindowSelectMethod::SelectOnStartup
        && windowmode::SelectMethodToComboIndex(
            windowmode::WindowSelectMethod::SelectOnStartup) == 0
        && windowmode::SelectMethodToComboIndex(
            windowmode::WindowSelectMethod::NoSelect) == 2
        && windowmode::ComboIndexToSelectMethod(2)
            == windowmode::WindowSelectMethod::NoSelect;

    const bool ok = missingOk && onOk && writeOk && roundOk && clearOk && loadClearOk
        && aliasOk && writeAliasOk && persistOk;
    Emit(L"fake_focus_json_roundtrip", ok,
        ok ? L"" : L"fakeFocusEnabled JSON roundtrip / selectMethod persist failed");
}

void TestKernelAnticheatBlocksBackground() {
    const bool classOk = windowmode::LooksLikeKernelAntiCheatToken(L"RiotWindowClass")
        && windowmode::LooksLikeKernelAntiCheatToken(L"League of Legends (TM) Client")
        && windowmode::LooksLikeKernelAntiCheatToken(L"C:\\Riot Games\\League of Legends\\LeagueClient.exe")
        && windowmode::LooksLikeKernelAntiCheatToken(L"VALORANT-Win64-Shipping.exe")
        && windowmode::LooksLikeKernelAntiCheatToken(L"英雄联盟");
    const bool negOk = !windowmode::LooksLikeKernelAntiCheatToken(L"Notepad")
        && !windowmode::LooksLikeKernelAntiCheatToken(L"UnityWndClass")
        && !windowmode::LooksLikeKernelAntiCheatToken(L"UnrealWindow")
        && !windowmode::LooksLikeKernelAntiCheatToken(L"");

    windowmode::WindowModeScriptConfig lol{};
    lol.enabled = true;
    lol.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    lol.windowClassName = L"RiotWindowClass";
    lol.targetWindowTitle = L"League of Legends (TM) Client";
    const bool cfgOk = windowmode::LooksLikeKernelAntiCheatProtectedTarget(lol, nullptr);

    windowmode::WindowModeScriptConfig unity{};
    unity.enabled = true;
    unity.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    unity.windowClassName = L"UnityWndClass";
    const bool unityOk = !windowmode::LooksLikeKernelAntiCheatProtectedTarget(unity, nullptr);

    const wchar_t* hint = windowmode::KernelAntiCheatBackgroundUnsupportedHint();
    const bool hintOk = hint && wcsstr(hint, L"后台窗口") && wcsstr(hint, L"默认模式");

    const bool ok = classOk && negOk && cfgOk && unityOk && hintOk;
    Emit(L"kernel_anticheat_blocks_background", ok,
        ok ? L"" : L"Riot/LoL background refusal detection failed");
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
        && windowmode::UsesFakeFocus(game)
        && windowmode::LooksLikeGameWindowClass(L"UnityWndClass");

    const std::wstring args = windowmode::EnsureRemoteDebuggingLaunchArgs(L"", 9222);
    const bool argsOk = args.find(L"--remote-debugging-port=9222") != std::wstring::npos;

    // 显式 softMessage 不被 Annotate 覆盖
    windowmode::WindowModeScriptConfig forceSoft = chrome;
    forceSoft.inputStrategy = windowmode::WindowModeInputStrategy::SoftMessage;
    windowmode::AnnotateInputStrategyForSave(forceSoft);
    const bool forceOk = forceSoft.inputStrategy == windowmode::WindowModeInputStrategy::SoftMessage;

    // Electron 壳（QQ）：Chrome_WidgetWin + 非浏览器 exe → 不得走扩展桥；须本机输入。
    windowmode::WindowModeScriptConfig qq = chrome;
    qq.targetExePath = L"D:\\QQ\\QQ.exe";
    qq.inputStrategy = windowmode::WindowModeInputStrategy::Auto;
    const bool qqAutoOk =
        windowmode::ConfigLooksLikeElectronShell(qq)
        && !windowmode::ConfigLooksLikeExtBridgeBrowser(qq)
        && windowmode::ResolveInputStrategy(qq) == windowmode::WindowModeInputStrategy::SoftMessage
        && !windowmode::UsesCdpInput(qq)
        && windowmode::UsesFakeFocus(qq)
        && !windowmode::ShouldMinimizeTargetAfterBind(qq);
    windowmode::AnnotateInputStrategyForSave(qq);
    const bool qqAnnotateOk = qq.inputStrategy == windowmode::WindowModeInputStrategy::Auto;

    // 录制曾误标 cdp：回放仍须按 exe 降级。
    windowmode::WindowModeScriptConfig qqSavedCdp = qq;
    qqSavedCdp.inputStrategy = windowmode::WindowModeInputStrategy::Cdp;
    const bool qqDemoteOk =
        windowmode::ResolveInputStrategy(qqSavedCdp) == windowmode::WindowModeInputStrategy::SoftMessage
        && !windowmode::UsesCdpInput(qqSavedCdp);

    // 真浏览器 exe 仍走 CDP。
    windowmode::WindowModeScriptConfig edge = chrome;
    edge.targetExePath = L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe";
    edge.inputStrategy = windowmode::WindowModeInputStrategy::Auto;
    const bool edgeOk =
        windowmode::ConfigLooksLikeExtBridgeBrowser(edge)
        && windowmode::ResolveInputStrategy(edge) == windowmode::WindowModeInputStrategy::Cdp
        && windowmode::UsesCdpInput(edge);

    const bool ok = resolveOk && writeOk && roundOk && gameOk && argsOk && forceOk
        && qqAutoOk && qqAnnotateOk && qqDemoteOk && edgeOk;
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
    cfg.windowClassName = L"Notepad";
    cfg.fakeFocusEnabled = false;
    const bool offOk = !windowmode::UsesFakeFocus(cfg)
        && windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.fakeFocusEnabled = true;
    const bool onOk = windowmode::UsesFakeFocus(cfg)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    const bool bgFlagOk = windowmode::UsesFakeFocus(cfg)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.fakeFocusEnabled = false;
    cfg.windowClassName = L"UnityWndClass";
    const bool bgUnityOk = windowmode::UsesFakeFocus(cfg)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.windowClassName = L"Notepad";
    const bool bgOffOk = !windowmode::UsesFakeFocus(cfg)
        && windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.windowClassName = L"GLFW30";
    const bool bgGlfwOk = windowmode::LooksLikeGameWindowClass(L"GLFW30")
        && windowmode::LooksLikeGlfwOrSdlWindowClass(L"GLFW30")
        && windowmode::LooksLikeGlfwOrSdlWindowClass(L"SDL_APP")
        && !windowmode::LooksLikeGlfwOrSdlWindowClass(L"UnityWndClass")
        && windowmode::LooksLikeGameWindowClass(L"SDL_APP")
        && windowmode::UsesFakeFocus(cfg)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.windowClassName = L"ApolloRuntimeContentWindow";
    const bool bgAirOk = windowmode::LooksLikeAdobeAirWindowClass(L"ApolloRuntimeContentWindow")
        && windowmode::LooksLikeAdobeAirWindowClass(L"ApolloRuntimeWindow")
        && windowmode::LooksLikeGameWindowClass(L"ApolloRuntimeContentWindow")
        && !windowmode::LooksLikeAdobeAirWindowClass(L"Notepad")
        && windowmode::UsesFakeFocus(cfg)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.windowClassName = L"TFrmMain";
    const bool bgLegendOk = windowmode::LooksLikeDelphiVclGameWindowClass(L"TFrmMain")
        && windowmode::LooksLikeGameWindowClass(L"TFrmMain")
        && windowmode::LooksLikeDelphiVclGameWindowClass(L"TFrmDlg")
        && windowmode::LooksLikeDelphiVclGameWindowClass(L"TPlayScene")
        && windowmode::LooksLikeDelphiVclGameWindowClass(L"TDXDraw")
        && !windowmode::LooksLikeDelphiVclGameWindowClass(L"TForm1")
        && !windowmode::LooksLikeGameWindowClass(L"TForm1")
        && !windowmode::LooksLikeDelphiVclGameWindowClass(L"ThunderRT6FormDC")
        && windowmode::UsesFakeFocus(cfg)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    windowmode::WindowModeScriptConfig form1Cfg = cfg;
    form1Cfg.windowClassName = L"TForm1";
    form1Cfg.fakeFocusEnabled = false;
    WNDCLASSW formWc{};
    formWc.lpfnWndProc = DefWindowProcW;
    formWc.hInstance = GetModuleHandleW(nullptr);
    formWc.lpszClassName = L"TForm1QstFfSelfTest";
    RegisterClassW(&formWc);
    WNDCLASSW dxWc{};
    dxWc.lpfnWndProc = DefWindowProcW;
    dxWc.hInstance = GetModuleHandleW(nullptr);
    dxWc.lpszClassName = L"TDXDrawQstSelfTest";
    RegisterClassW(&dxWc);
    HWND form1 = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        formWc.lpszClassName, L"QstForm1",
        WS_OVERLAPPEDWINDOW, 80, 80, 400, 300, nullptr, nullptr, formWc.hInstance, nullptr);
    HWND dxChild = nullptr;
    if (form1) {
        dxChild = CreateWindowExW(0, dxWc.lpszClassName, L"",
            WS_CHILD | WS_VISIBLE, 0, 0, 380, 260, form1, nullptr, dxWc.hInstance, nullptr);
    }
    const bool form1ShellOk = form1 && dxChild
        && !windowmode::LooksLikeDelphiVclGameWindowClass(L"TForm1QstFfSelfTest")
        && windowmode::HwndLooksLikeDelphiVclGame(form1)
        && windowmode::UsesFakeFocusForTarget(form1Cfg, form1)
        && !windowmode::ShouldMinimizeTargetAfterBind(form1Cfg, form1);
    if (dxChild) DestroyWindow(dxChild);
    if (form1) DestroyWindow(form1);
    UnregisterClassW(dxWc.lpszClassName, dxWc.hInstance);
    UnregisterClassW(formWc.lpszClassName, formWc.hInstance);

    cfg.windowClassName = L"TscShellContainerClass";
    cfg.targetExePath = L"C:\\Windows\\System32\\mstsc.exe";
    cfg.fakeFocusEnabled = true;
    const bool rdpNoFf = !windowmode::UsesFakeFocusForTarget(cfg, nullptr)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg)
        && windowmode::LooksLikeRemoteDesktopWindowClass(L"TscShellContainerClass")
        && windowmode::LooksLikeRemoteDesktopWindowClass(L"IHWindowClass")
        && windowmode::LooksLikeRemoteDesktopExePath(cfg.targetExePath);

    const bool ok = offOk && onOk && bgFlagOk && bgUnityOk && bgOffOk && bgGlfwOk && bgAirOk
        && bgLegendOk && form1ShellOk && rdpNoFf;
    Emit(L"fake_focus_minimize_gate", ok,
        ok ? L"" : L"UsesFakeFocus / minimize gate / RDP mismatch");
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

void TestMonitorCoveringFullscreen() {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        Emit(L"monitor_covering_fullscreen", false, L"GetMonitorInfo failed");
        return;
    }
    const RECT& r = mi.rcMonitor;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QstFsCoverSelfTest";
    RegisterClassW(&wc);

    // 不 ShowWindow，避免闪全屏。Create 尺寸已足够判断矩形。
    HWND popup = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"QstFsCover",
        WS_POPUP, r.left, r.top, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    const bool coverOk = popup && windowmode::LooksLikeMonitorCoveringFullscreen(popup);
    const bool notGame = popup && !windowmode::LooksLikeFullscreenGameTarget(popup);
    if (popup) DestroyWindow(popup);

    HWND framed = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"QstFsCoverFramed",
        WS_OVERLAPPEDWINDOW, r.left, r.top, w, h,
        nullptr, nullptr, wc.hInstance, nullptr);
    const bool framedCover = framed && windowmode::WindowCoversNearestMonitor(framed);
    const bool framedOk = framed && !windowmode::LooksLikeMonitorCoveringFullscreen(framed);
    if (framed) DestroyWindow(framed);

    HWND smallWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"QstFsCoverSmall",
        WS_POPUP, 80, 80, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
    const bool smallOk = smallWnd && !windowmode::LooksLikeMonitorCoveringFullscreen(smallWnd);
    if (smallWnd) DestroyWindow(smallWnd);

    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    const bool classOk = windowmode::LooksLikeGameWindowClass(L"UnrealWindow");
    const bool unrealCls = windowmode::LooksLikeUnrealEngineWindowClass(L"UnrealWindow")
        && !windowmode::LooksLikeUnrealEngineWindowClass(L"UnityWndClass");

    // UE5：小窗也必须当成 DXGI 敏感目标（不得等铺满才跳过假焦点）。
    // 类名带 UnrealWindow 子串即可，避免与真实游戏已注册的 UnrealWindow 冲突。
    WNDCLASSW ueWc{};
    ueWc.lpfnWndProc = DefWindowProcW;
    ueWc.hInstance = GetModuleHandleW(nullptr);
    ueWc.lpszClassName = L"QstUnrealWindowSelfTest";
    RegisterClassW(&ueWc);
    HWND unrealSmall = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        ueWc.lpszClassName, L"QstUeSmall",
        WS_POPUP, 80, 80, 400, 300, nullptr, nullptr, ueWc.hInstance, nullptr);
    const bool unrealAlways = unrealSmall
        && windowmode::LooksLikeFullscreenGameTarget(unrealSmall)
        && windowmode::LooksLikeUnrealEngineWindowClass(ueWc.lpszClassName)
        && !windowmode::LooksLikeMonitorCoveringFullscreen(unrealSmall);
    if (unrealSmall) DestroyWindow(unrealSmall);

    // 窗口化 UE5：ActivateWindow 不得误走「独占全屏无法切前台」弱路径。
    HWND unrealAct = CreateWindowExW(0, ueWc.lpszClassName, L"QstUeAct",
        WS_OVERLAPPEDWINDOW, 120, 120, 420, 320, nullptr, nullptr, ueWc.hInstance, nullptr);
    bool unrealActPathOk = false;
    if (unrealAct) {
        ShowWindow(unrealAct, SW_SHOWNOACTIVATE);
        std::wstring actErr;
        const bool activated = windowmode::ActivateWindow(unrealAct, actErr);
        const bool notExclusiveMsg =
            actErr.find(L"全屏独占游戏无法用 Win32") == std::wstring::npos;
        unrealActPathOk = windowmode::LooksLikeFullscreenGameTarget(unrealAct)
            && !windowmode::LooksLikeMonitorCoveringFullscreen(unrealAct)
            && notExclusiveMsg
            && (activated || !actErr.empty());
        DestroyWindow(unrealAct);
    }
    UnregisterClassW(ueWc.lpszClassName, ueWc.hInstance);

    const bool legendCls = windowmode::LooksLikeDelphiVclGameWindowClass(L"TFrmMain")
        && !windowmode::LooksLikeDelphiVclGameWindowClass(L"TForm1");
    WNDCLASSW legendWc{};
    legendWc.lpfnWndProc = DefWindowProcW;
    legendWc.hInstance = GetModuleHandleW(nullptr);
    legendWc.lpszClassName = L"TFrmMainQstSelfTest";
    RegisterClassW(&legendWc);
    HWND legendCover = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        legendWc.lpszClassName, L"QstLegendCover",
        WS_POPUP, r.left, r.top, w, h, nullptr, nullptr, legendWc.hInstance, nullptr);
    windowmode::WindowModeScriptConfig legendCfg{};
    legendCfg.enabled = true;
    legendCfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    legendCfg.windowClassName = L"TFrmMain";
    const bool legendNotFs = legendCover
        && windowmode::LooksLikeMonitorCoveringFullscreen(legendCover)
        && !windowmode::LooksLikeFullscreenGameTarget(legendCover)
        && windowmode::UsesFakeFocusForTarget(legendCfg, legendCover);
    if (legendCover) DestroyWindow(legendCover);
    UnregisterClassW(legendWc.lpszClassName, legendWc.hInstance);

    const bool ok = coverOk && notGame && framedCover && framedOk && smallOk
        && classOk && unrealCls && unrealAlways && unrealActPathOk
        && legendCls && legendNotFs;
    wchar_t detail[288]{};
    swprintf_s(detail,
        L"cover=%d notGame=%d framedCover=%d framed=%d small=%d unreal=%d ueCls=%d ueAlways=%d ueAct=%d legend=%d legendFs=%d",
        coverOk ? 1 : 0, notGame ? 1 : 0, framedCover ? 1 : 0, framedOk ? 1 : 0,
        smallOk ? 1 : 0, classOk ? 1 : 0, unrealCls ? 1 : 0, unrealAlways ? 1 : 0,
        unrealActPathOk ? 1 : 0, legendCls ? 1 : 0, legendNotFs ? 1 : 0);
    Emit(L"monitor_covering_fullscreen", ok, ok ? L"" : detail);
}

void TestGameHardwareWithoutInject() {
    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    cfg.fakeFocusEnabled = false;
    cfg.inputStrategy = windowmode::WindowModeInputStrategy::SoftMessage;

    WNDCLASSW ueWc{};
    ueWc.lpfnWndProc = DefWindowProcW;
    ueWc.hInstance = GetModuleHandleW(nullptr);
    ueWc.lpszClassName = L"QstUnrealWindowHwSelfTest";
    RegisterClassW(&ueWc);
    HWND unreal = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        ueWc.lpszClassName, L"QstUeHw",
        WS_OVERLAPPEDWINDOW, 120, 120, 420, 320, nullptr, nullptr, ueWc.hInstance, nullptr);
    const bool needHw = unreal
        && windowmode::GameTargetNeedsHardwareWithoutFakeFocus(cfg, unreal)
        && windowmode::UsesFakeFocusForTarget(cfg, unreal)
        && !windowmode::CanParkHardwareInputTargetOffscreen(unreal);
    if (unreal) DestroyWindow(unreal);
    UnregisterClassW(ueWc.lpszClassName, ueWc.hInstance);

    WNDCLASSW noteWc{};
    noteWc.lpfnWndProc = DefWindowProcW;
    noteWc.hInstance = GetModuleHandleW(nullptr);
    noteWc.lpszClassName = L"QstNotepadHwSelfTest";
    RegisterClassW(&noteWc);
    HWND note = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        noteWc.lpszClassName, L"QstNoteHw",
        WS_OVERLAPPEDWINDOW, 160, 160, 400, 300, nullptr, nullptr, noteWc.hInstance, nullptr);
    cfg.windowClassName = L"Notepad";
    const bool noteSkip = note
        && !windowmode::GameTargetNeedsHardwareWithoutFakeFocus(cfg, note)
        && windowmode::CanParkHardwareInputTargetOffscreen(note);
    if (note) DestroyWindow(note);
    UnregisterClassW(noteWc.lpszClassName, noteWc.hInstance);

    const bool ok = needHw && noteSkip;
    wchar_t detail[160]{};
    swprintf_s(detail, L"unrealHw=%d noteSkip=%d", needHw ? 1 : 0, noteSkip ? 1 : 0);
    Emit(L"game_hardware_without_inject", ok, ok ? L"" : detail);
}

void TestHardwareOffscreenPark() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QstHwOffscreenParkSelfTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"QstHwPark",
        WS_OVERLAPPEDWINDOW, 140, 140, 480, 360, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        Emit(L"hardware_offscreen_park", false, L"CreateWindow failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    RECT before{};
    GetWindowRect(hwnd, &before);

    const bool canPark = windowmode::CanParkHardwareInputTargetOffscreen(hwnd)
        && !windowmode::LooksLikeMonitorCoveringFullscreen(hwnd);
    WINDOWPLACEMENT saved{};
    bool wasTop = false;
    const bool parked = canPark
        && windowmode::ParkHardwareInputTargetOffscreen(hwnd, &saved, &wasTop);
    RECT mid{};
    GetWindowRect(hwnd, &mid);
    const bool offscreen = parked && mid.left <= -10000;
    const bool topmost = parked
        && (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const bool restored = parked
        && windowmode::RestoreHardwareInputTargetOffscreen(hwnd, saved, wasTop);
    RECT after{};
    GetWindowRect(hwnd, &after);
    const bool back = restored
        && std::abs(after.left - before.left) < 80
        && std::abs(after.top - before.top) < 80;

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    const bool ok = canPark && parked && offscreen && topmost && restored && back;
    wchar_t detail[192]{};
    swprintf_s(detail, L"can=%d park=%d off=%d top=%d rest=%d back=%d",
        canPark ? 1 : 0, parked ? 1 : 0, offscreen ? 1 : 0,
        topmost ? 1 : 0, restored ? 1 : 0, back ? 1 : 0);
    Emit(L"hardware_offscreen_park", ok, ok ? L"" : detail);
}

void TestVdaSelectsOsDll() {
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    const DWORD build = (rtlGetVersion && rtlGetVersion(&vi) == 0) ? vi.dwBuildNumber : 0;

    std::wstring err;
    auto& vda = windowmode::VirtualDesktopAccessor::Instance();
    if (!vda.EnsureLoaded(err)) {
        Emit(L"vda_selects_os_dll", false, err.empty() ? L"EnsureLoaded failed" : err.c_str());
        return;
    }
    const std::wstring path = vda.LoadedDllName();
    const bool is10 = path.find(L"VirtualDesktopAccessor10.dll") != std::wstring::npos;
    const bool is23 = path.find(L"VirtualDesktopAccessor11_23h2.dll") != std::wstring::npos;
    const bool is24 = path.find(L"VirtualDesktopAccessor11.dll") != std::wstring::npos && !is23;
    bool ok = false;
    if (build >= 26100) ok = is24 && !is10;
    else if (build >= 22000) ok = is23 && !is10;
    else if (build >= 10240) ok = is10 && !is23 && !is24;
    wchar_t detail[320]{};
    swprintf_s(detail, L"build=%lu dll=%s",
        static_cast<unsigned long>(build), path.c_str());
    Emit(L"vda_selects_os_dll", ok, ok ? L"" : detail);
}

void TestSoftMessageExeGates() {
    // 原 exe 链路：非游戏 Win32 类 → softMessage；默认可最小化。
    windowmode::WindowModeScriptConfig exe{};
    exe.enabled = true;
    exe.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    exe.windowClassName = L"Notepad";
    exe.inputStrategy = windowmode::WindowModeInputStrategy::Auto;
    exe.fakeFocusEnabled = false;

    const bool softOk =
        windowmode::ResolveInputStrategy(exe) == windowmode::WindowModeInputStrategy::SoftMessage
        && !windowmode::UsesCdpInput(exe)
        && !windowmode::UsesFakeFocus(exe)
        && windowmode::ShouldMinimizeTargetAfterBind(exe);

    // Unity 即使未勾选「聚焦」也自动假焦点，绑后保持还原。
    windowmode::WindowModeScriptConfig unity = exe;
    unity.windowClassName = L"UnityWndClass";
    const bool unityAutoOk =
        windowmode::ResolveInputStrategy(unity) == windowmode::WindowModeInputStrategy::SoftMessage
        && !windowmode::UsesCdpInput(unity)
        && windowmode::UsesFakeFocus(unity)
        && !windowmode::ShouldMinimizeTargetAfterBind(unity);

    windowmode::WindowModeScriptConfig legend = exe;
    legend.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    legend.windowClassName = L"TFrmMain";
    legend.fakeFocusEnabled = false;
    const bool legendAutoOk =
        windowmode::LooksLikeDelphiVclGameWindowClass(L"TFrmMain")
        && windowmode::ResolveInputStrategy(legend) == windowmode::WindowModeInputStrategy::SoftMessage
        && !windowmode::UsesCdpInput(legend)
        && windowmode::UsesFakeFocus(legend)
        && !windowmode::ShouldMinimizeTargetAfterBind(legend);

    exe.windowClassName = L"UnityWndClass";
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

    // Discord / 微信 / CEF：与 QQ 同一套 Chromium 壳路径。
    windowmode::WindowModeScriptConfig discord{};
    discord.enabled = true;
    discord.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    discord.windowClassName = L"Chrome_WidgetWin_1";
    discord.targetExePath = L"C:\\Discord\\Discord.exe";
    discord.inputStrategy = windowmode::WindowModeInputStrategy::Auto;
    const bool discordOk =
        windowmode::ConfigLooksLikeElectronShell(discord)
        && !windowmode::ConfigLooksLikeExtBridgeBrowser(discord)
        && windowmode::UsesFakeFocus(discord)
        && !windowmode::ShouldMinimizeTargetAfterBind(discord);

    windowmode::WindowModeScriptConfig wechat = discord;
    wechat.targetExePath = L"D:\\Tencent\\Weixin\\Weixin.exe";
    const bool wechatOk = windowmode::ConfigLooksLikeElectronShell(wechat)
        && windowmode::LooksLikeChromiumBrowserClass(L"CefBrowserWindow")
        && windowmode::LooksLikeChromiumBrowserClass(L"Chrome_RenderWidgetHostHWND");

    // DeSmuME 等模拟器：外层 WM_KEY* 无效，须自动假焦点（GetAsyncKeyState 钩）。
    windowmode::WindowModeScriptConfig desmume{};
    desmume.enabled = true;
    desmume.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    desmume.windowClassName = L"DeSmuME";
    const bool desmumeOk =
        windowmode::LooksLikeEmulatorWindowClass(L"DeSmuME")
        && windowmode::UsesFakeFocus(desmume)
        && !windowmode::ShouldMinimizeTargetAfterBind(desmume);
    const bool desmumeDesktopOk = windowmode::ConfigLooksLikeEmulatorTarget(desmume)
        && !windowmode::LooksLikeAndroidEmulatorExecutable(desmume.targetExePath);
    windowmode::WindowModeScriptConfig desmumeExe{};
    desmumeExe.enabled = true;
    desmumeExe.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    desmumeExe.targetExePath = L"C:\\emu\\DeSmuME_x64.exe";
    const bool desmumeExeOk =
        windowmode::LooksLikeEmulatorExecutable(desmumeExe.targetExePath)
        && windowmode::UsesFakeFocus(desmumeExe);

    // 汉化包/改名 exe（电脑端-melonDS.exe）：须识别并禁止 HiddenDesktop。
    windowmode::WindowModeScriptConfig melonPrefixed{};
    melonPrefixed.enabled = true;
    melonPrefixed.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    melonPrefixed.targetExePath = L"D:\\pok\\MNQ\\ME\\电脑端-melonDS.exe";
    const bool melonPrefixedOk =
        windowmode::LooksLikeEmulatorExecutable(melonPrefixed.targetExePath)
        && windowmode::ConfigLooksLikeEmulatorTarget(melonPrefixed);

    windowmode::WindowModeScriptConfig ld{};
    ld.enabled = true;
    ld.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    ld.targetExePath = L"D:\\LDPlayer\\dnplayer.exe";
    const bool ldOk =
        windowmode::LooksLikeAndroidEmulatorExecutable(ld.targetExePath)
        && !windowmode::UsesFakeFocus(ld)
        && !windowmode::UsesFakeFocusOrAndroidQt(ld);

    windowmode::WindowModeScriptConfig mumu{};
    mumu.enabled = true;
    mumu.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    mumu.windowClassName = L"Qt5156QWindowIcon";
    mumu.windowName = L"MuMu安卓设备-1";
    const bool mumuOk =
        windowmode::LooksLikeQtRenderWindowClass(mumu.windowClassName)
        && !windowmode::UsesFakeFocusOrAndroidQt(mumu)
        && !windowmode::UsesFakeFocus(mumu)
        && !windowmode::AndroidEmulatorNeedsHardwareInput(nullptr, &mumu);

    windowmode::WindowModeScriptConfig mumuHidden = mumu;
    mumuHidden.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    const bool emuDowngradeOk =
        windowmode::ConfigLooksLikeEmulatorTarget(mumuHidden);

    const bool ok = softOk && unityAutoOk && legendAutoOk && ffOk && forceOk && discordOk && wechatOk
        && desmumeOk && desmumeExeOk && desmumeDesktopOk && melonPrefixedOk
        && ldOk && mumuOk && emuDowngradeOk;
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
    const BOOL stillInstalled = isInstalled();
    // Install 可能把探测窗变成真前台；先把前台还回去，再确认钩已卸（否则 GFW 仍是探测窗会被误判成钩残留）。
    if (before && IsWindow(before)) SetForegroundWindow(before);
    HWND after = GetForegroundWindow();
    DestroyWindow(hwnd);
    FreeLibrary(mod);

    const bool ok = installed && active && hooked == hwnd && !stillInstalled
        && after != hwnd;
    wchar_t detail[256]{};
    swprintf_s(detail,
        L"install=%d hooked=%p expect=%p before=%p after=%p",
        installed ? 1 : 0, hooked, hwnd, before, after);
    Emit(L"fake_focus_hook_local", ok, ok ? L"" : detail);
}

void TestFakeFocusLiteUnreal() {
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        Emit(L"fake_focus_lite_unreal", false,
            (L"LoadLibrary failed: " + dllPath).c_str());
        return;
    }
    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    auto* installLite = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_InstallLite"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    if (!installLite || !uninstall) {
        FreeLibrary(mod);
        Emit(L"fake_focus_lite_unreal", false, L"missing FakeFocus_InstallLite");
        return;
    }

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"QST FakeFocus Lite",
        WS_OVERLAPPEDWINDOW, 48, 48, 240, 120, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        FreeLibrary(mod);
        Emit(L"fake_focus_lite_unreal", false, L"CreateWindow failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    const BOOL installed = installLite(hwnd);
    HWND hooked = GetForegroundWindow();
    MSG msg{};
    const BOOL peekEmpty = PeekMessageW(&msg, hwnd, 0, 0, PM_NOREMOVE) == FALSE
        || msg.message != WM_INPUT;
    uninstall();
    DestroyWindow(hwnd);
    FreeLibrary(mod);

    const bool ok = installed && hooked == hwnd && peekEmpty;
    wchar_t detail[160]{};
    swprintf_s(detail, L"install=%d fg=%d peekOk=%d",
        installed ? 1 : 0, hooked == hwnd ? 1 : 0, peekEmpty ? 1 : 0);
    Emit(L"fake_focus_lite_unreal", ok, ok ? L"" : detail);
}

void TestFakeFocusAirFocusOnly() {
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        Emit(L"fake_focus_air_focus_only", false,
            (L"LoadLibrary failed: " + dllPath).c_str());
        return;
    }
    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    auto* installLite = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_InstallLite"));
    auto* updateTarget = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_UpdateTarget"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    if (!installLite || !uninstall || !updateTarget) {
        FreeLibrary(mod);
        Emit(L"fake_focus_air_focus_only", false, L"missing FakeFocus_InstallLite/UpdateTarget");
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ApolloRuntimeContentWindow";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"QST AIR fake-focus",
        WS_OVERLAPPEDWINDOW, 80, 80, 240, 140, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        FreeLibrary(mod);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        Emit(L"fake_focus_air_focus_only", false, L"CreateWindow AIR failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    const LONG_PTR procBefore = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    POINT orig{};
    GetCursorPos(&orig);

    const BOOL installed = installLite(hwnd);
    const BOOL updated = updateTarget(hwnd);
    HWND hooked = GetForegroundWindow();
    const LONG_PTR procAfter = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    const int warpX = orig.x + 37;
    const int warpY = orig.y + 19;
    SetCursorPos(warpX, warpY);
    POINT after{};
    GetCursorPos(&after);
    MSG msg{};
    const BOOL peekEmpty = PeekMessageW(&msg, hwnd, 0, 0, PM_NOREMOVE) == FALSE
        || msg.message != WM_INPUT;

    uninstall();
    SetCursorPos(orig.x, orig.y);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    FreeLibrary(mod);

    const bool procOk = procBefore == procAfter;
    const bool warpOk = std::abs(after.x - warpX) <= 2 && std::abs(after.y - warpY) <= 2;
    const bool ok = installed && updated && hooked == hwnd && procOk && warpOk && peekEmpty;
    wchar_t detail[200]{};
    swprintf_s(detail, L"install=%d update=%d fg=%d proc=%d warp=(%ld,%ld) peekOk=%d",
        installed ? 1 : 0, updated ? 1 : 0, hooked == hwnd ? 1 : 0, procOk ? 1 : 0,
        after.x, after.y, peekEmpty ? 1 : 0);
    Emit(L"fake_focus_air_focus_only", ok, ok ? L"" : detail);
}

void TestFakeFocusMapleStoryFocusOnly() {
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif
    std::wstring softErr;
    if (!windowmode::FakeFocusSoftInput_Attach(GetCurrentProcessId(), softErr)) {
        Emit(L"fake_focus_maplestory_focus_only", false,
            softErr.empty() ? L"Attach soft input failed" : softErr.c_str());
        return;
    }
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        windowmode::FakeFocusSoftInput_Detach();
        Emit(L"fake_focus_maplestory_focus_only", false,
            (L"LoadLibrary failed: " + dllPath).c_str());
        return;
    }
    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    using CountFn = DWORD(WINAPI*)(HWND);
    auto* installLite = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_InstallLite"));
    auto* updateTarget = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_UpdateTarget"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    auto* mapleIatCount = reinterpret_cast<CountFn>(GetProcAddress(mod, "FakeFocus_MapleIatCount"));
    auto* mapleHookHits = reinterpret_cast<CountFn>(GetProcAddress(mod, "FakeFocus_MapleHookHits"));
    if (!installLite || !uninstall || !updateTarget || !mapleIatCount || !mapleHookHits) {
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(L"fake_focus_maplestory_focus_only", false,
            L"missing FakeFocus_InstallLite/UpdateTarget/MapleIatCount/MapleHookHits");
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MapleStoryClass";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"MapleStory(QST iat-input)",
        WS_OVERLAPPEDWINDOW, 80, 80, 240, 140, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        Emit(L"fake_focus_maplestory_focus_only", false, L"CreateWindow MapleStoryClass failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    const LONG_PTR procBefore = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);

    const BOOL installed = installLite(hwnd);
    const BOOL updated = updateTarget(hwnd);
    const DWORD maplePacked = mapleIatCount(hwnd);
    const DWORD iatSlots = maplePacked & 0xFFFFu;
    const DWORD mapleDiag = maplePacked >> 16;
    HWND hooked = GetForegroundWindow();
    const LONG_PTR procAfter = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);

    windowmode::FakeFocusSoftInput_SetCursorScreen(1414, 2525);
    windowmode::FakeFocusSoftInput_SetMouseButtonVk(VK_LBUTTON, true);
    windowmode::FakeFocusSoftInput_SetKey(VK_SPACE, true);
    POINT pt{};
    const BOOL cursorOk = GetCursorPos(&pt) && pt.x == 1414 && pt.y == 2525;
    const SHORT space = GetAsyncKeyState(VK_SPACE);
    const SHORT lbtn = GetAsyncKeyState(VK_LBUTTON);
    const bool keyOk = (space & 0x8000) != 0 && (lbtn & 0x8000) != 0;
    const DWORD hookHits = mapleHookHits(hwnd);
    const bool hitsOk = (hookHits & 0xFFu) > 0;

    MSG msg{};
    const BOOL peekEmpty = PeekMessageW(&msg, hwnd, 0, 0, PM_NOREMOVE) == FALSE
        || msg.message != WM_INPUT;

    WNDCLASSW wcTitle{};
    wcTitle.lpfnWndProc = DefWindowProcW;
    wcTitle.hInstance = wc.hInstance;
    wcTitle.lpszClassName = L"QstMapleTitleOnlyWnd";
    RegisterClassW(&wcTitle);
    HWND titleHwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wcTitle.lpszClassName, L"冒险岛",
        WS_OVERLAPPEDWINDOW, 80, 80, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    const LONG_PTR titleProcBefore = titleHwnd ? GetWindowLongPtrW(titleHwnd, GWLP_WNDPROC) : 0;
    const BOOL titleUpdated = titleHwnd ? updateTarget(titleHwnd) : FALSE;
    const LONG_PTR titleProcAfter = titleHwnd ? GetWindowLongPtrW(titleHwnd, GWLP_WNDPROC) : 0;
    const bool titleProcOk = titleHwnd && titleUpdated && titleProcBefore == titleProcAfter;

    uninstall();
    if (titleHwnd) DestroyWindow(titleHwnd);
    UnregisterClassW(wcTitle.lpszClassName, wc.hInstance);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    FreeLibrary(mod);
    windowmode::FakeFocusSoftInput_Detach();

    const bool procOk = procBefore == procAfter;
    const bool iatOk = iatSlots >= 1;
    const bool noUser32BodyJmp = (mapleDiag & 0x1000u) == 0;
    const bool noDiDataBodyJmp = (mapleDiag & 0x2000u) == 0;
    const bool ok = installed && updated && hooked == hwnd && procOk && cursorOk
        && keyOk && hitsOk && peekEmpty && titleProcOk && iatOk && noUser32BodyJmp && noDiDataBodyJmp;
    wchar_t detail[360]{};
    swprintf_s(detail,
        L"install=%d update=%d fg=%d proc=%d cursor=(%ld,%ld) space=%d lbtn=%d hitsGaks=%u peekOk=%d titleProc=%d iat=%lu diag=0x%04X",
        installed ? 1 : 0, updated ? 1 : 0, hooked == hwnd ? 1 : 0, procOk ? 1 : 0,
        pt.x, pt.y, (space & 0x8000) ? 1 : 0, (lbtn & 0x8000) ? 1 : 0,
        static_cast<unsigned>(hookHits & 0xFFu),
        peekEmpty ? 1 : 0, titleProcOk ? 1 : 0,
        static_cast<unsigned long>(iatSlots), static_cast<unsigned>(mapleDiag));
    Emit(L"fake_focus_maplestory_focus_only", ok, ok ? L"" : detail);
}

void TestFakeFocus32ExportRva() {
    using windowmode::inject::detail::ExportNameMatches;
    using windowmode::inject::detail::FindExportRva;
    using windowmode::inject::detail::ReadFileBytes;

    const bool matchOk =
        ExportNameMatches("FakeFocus_InstallLite", "FakeFocus_InstallLite") &&
        ExportNameMatches("_FakeFocus_InstallLite@4", "FakeFocus_InstallLite") &&
        ExportNameMatches("FakeFocus_InstallLite@4", "FakeFocus_InstallLite") &&
        ExportNameMatches("_FakeFocus_Uninstall@0", "FakeFocus_Uninstall") &&
        !ExportNameMatches("_FakeFocus_InstallLite@4", "FakeFocus_Install") &&
        !ExportNameMatches("_FakeFocus_Install@4", "FakeFocus_InstallLite") &&
        !ExportNameMatches("_FakeFocus_InstallLite", "FakeFocus_InstallLite") &&
        !ExportNameMatches("_FakeFocus_InstallLite@", "FakeFocus_InstallLite") &&
        !ExportNameMatches(nullptr, "FakeFocus_InstallLite");
    if (!matchOk) {
        Emit(L"fake_focus32_export_rva", false,
            L"ExportNameMatches stdcall cases");
        return;
    }

    const std::wstring path = SelfExeDir() + L"FakeFocus32.dll";
    std::vector<uint8_t> pe;
    std::wstring err;
    if (!ReadFileBytes(path, pe, err)) {
        Emit(L"fake_focus32_export_rva", false,
            (L"read failed: " + err).c_str());
        return;
    }
    const char* needed[] = {
        "FakeFocus_InstallLite",
        "FakeFocus_Install",
        "FakeFocus_Uninstall",
        "FakeFocus_UpdateTarget",
        "FakeFocus_HookProc",
        "FakeFocus_MapleIatCount",
        "FakeFocus_MapleHookHits",
    };
    for (const char* name : needed) {
        DWORD rva = 0;
        std::wstring oneErr;
        if (!FindExportRva(pe, name, rva, oneErr) || rva == 0) {
            wchar_t detail[192]{};
            swprintf_s(detail, L"%S rva=%u err=%s", name, rva,
                oneErr.c_str());
            Emit(L"fake_focus32_export_rva", false, detail);
            return;
        }
    }
    Emit(L"fake_focus32_export_rva", true, L"");
}

void TestRemoteModuleKernel32() {
    using windowmode::inject::detail::FindRemoteModule;
    using windowmode::inject::detail::FindRemoteModuleViaPeb;
    using windowmode::inject::detail::ResolveRemoteProcAddress;

    const DWORD pid = GetCurrentProcessId();
    HMODULE localK32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE viaFind = FindRemoteModule(pid, L"kernel32.dll");
    HMODULE viaPeb = FindRemoteModuleViaPeb(GetCurrentProcess(), L"kernel32.dll");
    HMODULE localNtdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE ntdllPeb = FindRemoteModuleViaPeb(GetCurrentProcess(), L"ntdll.dll");

    std::wstring err;
    const uintptr_t remote = ResolveRemoteProcAddress(
        GetCurrentProcess(), pid, L"kernel32.dll", "LoadLibraryW", err);
    FARPROC localFn = localK32 ? GetProcAddress(localK32, "LoadLibraryW") : nullptr;

    const bool ok = localK32 && viaFind == localK32 && viaPeb == localK32
        && localNtdll && ntdllPeb == localNtdll
        && remote != 0 && remote == reinterpret_cast<uintptr_t>(localFn);
    wchar_t detail[240]{};
    swprintf_s(detail,
        L"k32=%p find=%p peb=%p ntdllPeb=%p loadLib remote=%p local=%p err=%s",
        localK32, viaFind, viaPeb, ntdllPeb,
        reinterpret_cast<void*>(remote), localFn, err.c_str());
    Emit(L"remote_module_kernel32", ok, ok ? L"" : detail);
}

void WriteU16(std::vector<uint8_t>& pe, size_t off, uint16_t v) {
    if (off + 2 > pe.size()) return;
    pe[off] = static_cast<uint8_t>(v & 0xFF);
    pe[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void WriteU32(std::vector<uint8_t>& pe, size_t off, uint32_t v) {
    if (off + 4 > pe.size()) return;
    pe[off] = static_cast<uint8_t>(v & 0xFF);
    pe[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    pe[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    pe[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void TestFakeFocusHeaderExportRva() {
    using windowmode::inject::detail::FindExportRva;

    // PE32：导出名放在 SizeOfHeaders(0x400) 内、超出 SizeOfOptionalHeader(0xE0)。
    // 旧 RvaToPtr 只用 optSize 当头，会扫不到 FakeFocus_Install。
    std::vector<uint8_t> pe(0x500, 0);
    pe[0] = 'M';
    pe[1] = 'Z';
    WriteU32(pe, 0x3C, 0x80);
    pe[0x80] = 'P';
    pe[0x81] = 'E';
    WriteU16(pe, 0x84, 0x014C);
    WriteU16(pe, 0x86, 1);
    WriteU16(pe, 0x94, 0x00E0);
    WriteU16(pe, 0x96, 0x2102);
    WriteU16(pe, 0x98, 0x010B);
    WriteU32(pe, 0x98 + 60, 0x400);
    WriteU32(pe, 0x98 + 92, 16);
    WriteU32(pe, 0x98 + 96, 0x200);
    WriteU32(pe, 0x98 + 100, 0x40);
    memcpy(&pe[0x178], ".rdata\0\0", 8);
    WriteU32(pe, 0x178 + 8, 0x100);
    WriteU32(pe, 0x178 + 12, 0x1000);
    WriteU32(pe, 0x178 + 16, 0x200);
    WriteU32(pe, 0x178 + 20, 0x400);
    WriteU32(pe, 0x200 + 16, 1);
    WriteU32(pe, 0x200 + 20, 1);
    WriteU32(pe, 0x200 + 24, 1);
    WriteU32(pe, 0x200 + 28, 0x240);
    WriteU32(pe, 0x200 + 32, 0x244);
    WriteU32(pe, 0x200 + 36, 0x248);
    WriteU32(pe, 0x240, 0x1000);
    WriteU32(pe, 0x244, 0x250);
    WriteU16(pe, 0x248, 0);
    const char* name = "FakeFocus_Install";
    memcpy(&pe[0x250], name, strlen(name) + 1);

    DWORD rva = 0;
    std::wstring err;
    if (!FindExportRva(pe, "FakeFocus_Install", rva, err) || rva != 0x1000) {
        wchar_t detail[192]{};
        swprintf_s(detail, L"rva=%u err=%s", rva, err.c_str());
        Emit(L"fake_focus_header_export_rva", false, detail);
        return;
    }
    Emit(L"fake_focus_header_export_rva", true, L"");
}

void TestFakeFocusGlfwLiteCursor() {
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GLFW30";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"GLFW30", L"QST GLFW lite",
        WS_OVERLAPPEDWINDOW, 64, 64, 200, 120, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        Emit(L"fake_focus_glfw_lite_cursor", false, L"CreateWindow GLFW30 failed");
        UnregisterClassW(L"GLFW30", wc.hInstance);
        return;
    }

    std::wstring softErr;
    if (!windowmode::FakeFocusSoftInput_Attach(GetCurrentProcessId(), softErr)) {
        DestroyWindow(hwnd);
        UnregisterClassW(L"GLFW30", wc.hInstance);
        Emit(L"fake_focus_glfw_lite_cursor", false,
            softErr.empty() ? L"Attach failed" : softErr.c_str());
        return;
    }

    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        windowmode::FakeFocusSoftInput_Detach();
        DestroyWindow(hwnd);
        UnregisterClassW(L"GLFW30", wc.hInstance);
        Emit(L"fake_focus_glfw_lite_cursor", false, L"LoadLibrary failed");
        return;
    }
    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    auto* installLite = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_InstallLite"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    if (!installLite || !uninstall || !installLite(hwnd)) {
        if (uninstall) uninstall();
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        DestroyWindow(hwnd);
        UnregisterClassW(L"GLFW30", wc.hInstance);
        Emit(L"fake_focus_glfw_lite_cursor", false, L"InstallLite failed");
        return;
    }

    windowmode::FakeFocusSoftInput_SetCursorScreen(1234, 5678);
    POINT pt{};
    GetCursorPos(&pt);
    const SHORT spaceIsolated = GetAsyncKeyState(VK_SPACE);
    SetCursorPos(1, 1);
    POINT after{};
    GetCursorPos(&after);

    uninstall();
    FreeLibrary(mod);
    windowmode::FakeFocusSoftInput_Detach();
    DestroyWindow(hwnd);
    UnregisterClassW(L"GLFW30", wc.hInstance);

    const bool ok = pt.x == 1234 && pt.y == 5678 && after.x == 1234 && after.y == 5678
        && (spaceIsolated & 0x8000) == 0;
    wchar_t detail[160]{};
    swprintf_s(detail, L"get=(%ld,%ld) afterWarp=(%ld,%ld) space=0x%04x",
        pt.x, pt.y, after.x, after.y, static_cast<unsigned>(spaceIsolated) & 0xFFFF);
    Emit(L"fake_focus_glfw_lite_cursor", ok, ok ? L"" : detail);
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

    BYTE rawBuf[256]{};
    UINT rawSz = sizeof(rawBuf);
    const UINT rawCount = GetRawInputBuffer(
        reinterpret_cast<PRAWINPUT>(rawBuf), &rawSz, sizeof(RAWINPUTHEADER));
    const auto* rawFromBuf = reinterpret_cast<const RAWINPUT*>(rawBuf);
    const bool bufOk = rawCount == 1
        && rawFromBuf->header.dwType == RIM_TYPEMOUSE
        && (rawFromBuf->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0
        && (rawFromBuf->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0;

    windowmode::FakeFocusSoftInput_SetCursorScreen(2222, 3333);
    MSG msg{};
    const BOOL peeked = PeekMessageW(&msg, nullptr, WM_INPUT, WM_INPUT, PM_REMOVE);
    UINT rawNeed = 0;
    UINT rawCopied = 0;
    RAWINPUT rawPeek{};
    if (peeked && msg.message == WM_INPUT) {
        UINT qsz = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(msg.lParam), RID_INPUT,
            nullptr, &qsz, sizeof(RAWINPUTHEADER));
        rawNeed = qsz;
        if (rawNeed >= sizeof(RAWINPUTHEADER) && rawNeed <= sizeof(rawPeek)) {
            UINT sz = rawNeed;
            rawCopied = GetRawInputData(reinterpret_cast<HRAWINPUT>(msg.lParam), RID_INPUT,
                &rawPeek, &sz, sizeof(RAWINPUTHEADER));
        }
    }
    const bool peekOk = peeked && msg.message == WM_INPUT
        && rawCopied > 0 && rawPeek.header.dwType == RIM_TYPEMOUSE
        && (rawPeek.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0;

    uninstall();
    DestroyWindow(hwnd);
    FreeLibrary(mod);
    windowmode::FakeFocusSoftInput_Detach();

    const bool ok = cursorOk && pt.x == 1234 && pt.y == 5678
        && (lbtn & 0x8000) != 0
        && (space & 0x8000) != 0
        && kbOk && (keys[VK_LBUTTON] & 0x80) != 0
        && (keys[VK_SPACE] & 0x80) != 0
        && bufOk && peekOk;
    wchar_t detail[320]{};
    swprintf_s(detail,
        L"pt=(%ld,%ld) lbtn=0x%04x space=0x%04x kbL=0x%02x kbSp=0x%02x buf=%u peek=%d rawCopied=%u",
        pt.x, pt.y, static_cast<unsigned>(lbtn) & 0xFFFF,
        static_cast<unsigned>(space) & 0xFFFF,
        keys[VK_LBUTTON], keys[VK_SPACE],
        rawCount, peeked ? 1 : 0, rawCopied);
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
        || (!windowmode::IsBrowserFakeFocusUnsupported(probe)
            && !windowmode::IsFakeFocusInjectionUnsupported(probe));
    if (probe) DestroyWindow(probe);

    Emit(L"anjuzhen_script_wm_config", ok && helperOk,
        ok && helperOk ? L"sample JSON + CDP strategy"
                       : (ok ? L"IsBrowserFakeFocusUnsupported helper mismatch"
                             : L"windowMode sample parse or helper mismatch"));
}

void TestPermissionMatchUipi() {
    const bool selfOk = windowmode::CheckPermissionMatch(GetCurrentProcessId());
    const bool zeroOk = windowmode::CheckPermissionMatch(0);
    DWORD explorerPid = 0;
    if (HWND shell = GetShellWindow()) {
        GetWindowThreadProcessId(shell, &explorerPid);
    }
    bool explorerOk = true;
    std::wstring detail;
    if (explorerPid) {
        explorerOk = windowmode::CheckPermissionMatch(explorerPid);
        if (!explorerOk) {
            detail = L"explorer pid=" + std::to_wstring(explorerPid)
                + L" rejected; elevated tool must still drive medium-IL targets";
        }
    }
    const bool ok = selfOk && zeroOk && explorerOk;
    if (!ok && detail.empty()) {
        if (!selfOk) detail = L"self pid rejected";
        else if (!zeroOk) detail = L"pid 0 rejected";
    }
    if (ok) {
        detail = windowmode::IsCurrentProcessElevated()
            ? L"elevated: explorer allowed"
            : L"same-IL: explorer allowed";
    }
    Emit(L"permission_match_uipi", ok, detail.c_str());
}

void TestMapleStoryBackgroundFakeFocus() {
    const bool classOk = windowmode::LooksLikeMapleStoryWindowClass(L"MapleStoryClass")
        && windowmode::LooksLikeMapleStoryWindowClass(L"MapleStory")
        && windowmode::LooksLikeGameWindowClass(L"MapleStoryClass")
        && !windowmode::LooksLikeMapleStoryWindowClass(L"Notepad")
        && !windowmode::LooksLikeGameWindowClass(L"Notepad");
    const bool exeOk = windowmode::LooksLikeMapleStoryExecutable(L"C:\\Games\\MapleStory.exe")
        && windowmode::LooksLikeMapleStoryExecutable(L"MapleStoryT.exe")
        && !windowmode::LooksLikeMapleStoryExecutable(L"notepad.exe");
    const bool titleOk = windowmode::LooksLikeMapleStoryTitle(L"MapleStory(星辰冒险岛)")
        && windowmode::LooksLikeMapleStoryTitle(L"冒险岛")
        && !windowmode::LooksLikeMapleStoryTitle(L"记事本");

    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    cfg.fakeFocusEnabled = false;
    cfg.windowClassName = L"MapleStoryClass";
    const bool ffOk = windowmode::UsesFakeFocus(cfg)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.windowClassName.clear();
    cfg.targetExePath = L"D:\\星辰\\MapleStory.exe";
    const bool exeFfOk = windowmode::UsesFakeFocus(cfg)
        && windowmode::LooksLikeMapleStoryExecutable(cfg.targetExePath);

    cfg.targetExePath.clear();
    cfg.windowName = L"MapleStory(星辰冒险岛)";
    const bool titleFfOk = windowmode::UsesFakeFocus(cfg);

    const bool ok = classOk && exeOk && titleOk && ffOk && exeFfOk && titleFfOk;
    Emit(L"maplestory_bg_fake_focus", ok,
        ok ? L"MapleStoryClass + exe → BackgroundWindow fake focus"
           : L"MapleStory not classified as game / fake focus off");
}

void TestTargetLostAfterDestroy() {
    constexpr wchar_t kCls[] = L"QstWmTargetLostClass";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kCls;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = true;
    }
    HWND hwnd = CreateWindowExW(0,
        kCls, L"QST TargetLost",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 240, 140,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        Emit(L"window_mode_target_lost_stops", false, L"CreateWindow failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    PumpMessagesFor(std::chrono::milliseconds(50));
    RECT rc{};
    GetWindowRect(hwnd, &rc);

    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    cfg.fakeFocusEnabled = false;
    cfg.autoLaunchTarget = false;
    cfg.windowClassName = kCls;
    cfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
    cfg.targetPickX = (rc.left + rc.right) / 2;
    cfg.targetPickY = (rc.top + rc.bottom) / 2;

    windowmode::WindowModeExecutor exec;
    std::wstring err;
    if (!exec.BeginRun(cfg, err)) {
        DestroyWindow(hwnd);
        Emit(L"window_mode_target_lost_stops", false,
            err.empty() ? L"BeginRun failed" : err.c_str());
        return;
    }
    const bool aliveBefore = exec.TargetStillAlive();
    DestroyWindow(hwnd);
    hwnd = nullptr;
    const bool aliveAfter = exec.TargetStillAlive();
    exec.EndRun();
    const bool ok = aliveBefore && !aliveAfter;
    wchar_t detail[96]{};
    swprintf_s(detail, L"before=%d after=%d", aliveBefore ? 1 : 0, aliveAfter ? 1 : 0);
    Emit(L"window_mode_target_lost_stops", ok, ok ? L"" : detail);
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

// AI 动作执行的窗口台账：切窗必须靠本地枚举，不靠识图数 Alt+Tab 格子
void TestWindowListAndActivate(HWND edit) {
    const HWND top = ParentTopWindow(edit);
    if (!top) {
        Emit(L"window_list_enumerates_self", false, L"no top window");
        Emit(L"window_list_match_and_format", false, L"skipped");
        Emit(L"window_activate_foreground", false, L"skipped");
        Emit(L"window_activate_by_process", false, L"skipped");
        return;
    }

    const auto all = windowmode::ListSwitchableWindows(0);
    auto findSelf = [&](const std::vector<windowmode::SwitchableWindow>& list) {
        for (const auto& w : list) {
            if (w.hwnd == top) return true;
        }
        return false;
    };
    const bool listed = findSelf(all);
    // 默认重载排除本进程窗口，免得 AI 把自动化工具自己当成切换目标
    const bool ownExcluded = !findSelf(windowmode::ListSwitchableWindows());
    Emit(L"window_list_enumerates_self", listed && ownExcluded,
        (listed && ownExcluded) ? L""
            : (listed ? L"own pid not excluded by default overload"
                      : L"self-test window missing from switchable list"));

    const auto hits = windowmode::MatchWindows(all, L"qst selft");
    const bool matchedByTitle = findSelf(hits);
    const auto noHits = windowmode::MatchWindows(all, L"zzz-no-such-window-zzz");
    const auto formatted = windowmode::FormatWindowList(hits);
    const bool formatOk = formatted.rfind(L"#1 ", 0) == 0
        && formatted.find(L"QST SelfTest") != std::wstring::npos;
    // Edge 标题夹零宽空格 / 仅进程名时，「Microsoft Edge」仍应命中
    windowmode::SwitchableWindow fakeEdge;
    fakeEdge.hwnd = top;
    fakeEdge.title = L"\u9020\u68a6\u65e0\u53cc - Microsoft\u200B Edge";
    fakeEdge.processName = L"msedge.exe";
    const auto edgeHits = windowmode::MatchWindows({fakeEdge}, L"Microsoft Edge");
    const auto edgeAlias = windowmode::MatchWindows({fakeEdge}, L"edge");
    const bool edgeMatchOk = edgeHits.size() == 1 && edgeAlias.size() == 1;

    const bool matchOk = matchedByTitle && noHits.empty() && formatOk && edgeMatchOk;
    Emit(L"window_list_match_and_format", matchOk,
        matchOk ? L"" : L"MatchWindows/FormatWindowList failed");

    ShowWindow(top, SW_MINIMIZE);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    std::wstring error;
    const bool activated = windowmode::ActivateWindow(top, error);
    const bool restored = IsIconic(top) == FALSE;
    const bool isForeground = GetForegroundWindow() == top;
    const bool activateOk = activated && restored && isForeground;
    wchar_t detail[256]{};
    swprintf_s(detail, L"activated=%d restored=%d fg=%d err=%s",
        activated ? 1 : 0, restored ? 1 : 0, isForeground ? 1 : 0,
        error.empty() ? L"-" : error.c_str());
    Emit(L"window_activate_foreground", activateOk, activateOk ? L"" : detail);

    // 按进程名激活：自检窗的 processName 应能命中并置顶
    std::wstring selfProc;
    for (const auto& w : all) {
        if (w.hwnd == top) { selfProc = w.processName; break; }
    }
    ShowWindow(top, SW_MINIMIZE);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    windowmode::SwitchableWindow byProcHit;
    std::wstring byProcErr;
    const bool byProcOk = !selfProc.empty()
        && windowmode::ActivateByProcessName(selfProc, &byProcHit, byProcErr)
        && byProcHit.hwnd == top
        && GetForegroundWindow() == top;
    Emit(L"window_activate_by_process", byProcOk,
        byProcOk ? L"" : (selfProc.empty() ? L"no processName"
            : (byProcErr.empty() ? L"hwnd/fg mismatch" : byProcErr.c_str())));
}



void TestScreenPointToClientWithin(HWND edit) {
    HWND top = ParentTopWindow(edit);
    if (!top) {
        Emit(L"screen_point_to_client_within", false, L"no top window");
        return;
    }
    RECT rc{};
    if (!GetWindowRect(top, &rc) || rc.right <= rc.left || rc.bottom <= rc.top) {
        Emit(L"screen_point_to_client_within", false, L"invalid window rect");
        return;
    }
    RECT client{};
    GetClientRect(top, &client);

    int cx = -1, cy = -1;
    const int insideX = rc.left + 60;
    const int insideY = rc.top + 70;
    const bool insideOk = windowmode::ScreenPointToClientWithin(
        top, insideX, insideY, cx, cy);
    const bool insideValid = insideOk && cx >= 0 && cy >= 0
        && cx < client.right && cy < client.bottom;

    int keepX = 7, keepY = 9;
    const bool outsideRejected = !windowmode::ScreenPointToClientWithin(
        top, rc.right + 80, rc.bottom + 60, keepX, keepY)
        && keepX == 7 && keepY == 9;

    const std::wstring detail =
        (insideValid ? L"inside ok" : L"inside FAIL")
        + std::wstring(outsideRejected ? L" | outside ok" : L" | outside FAIL")
        + L" | client=" + std::to_wstring(client.right)
        + L"x" + std::to_wstring(client.bottom)
        + L" mapped=(" + std::to_wstring(cx) + L"," + std::to_wstring(cy) + L")";
    Emit(L"screen_point_to_client_within", insideValid && outsideRejected,
        detail.c_str());
}

bool ForceTestForeground(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND fg = GetForegroundWindow();
    const DWORD cur = GetCurrentThreadId();
    const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD targetTid = GetWindowThreadProcessId(hwnd, nullptr);
    bool attFg = false;
    bool attTarget = false;
    if (fgTid && fgTid != cur) attFg = AttachThreadInput(cur, fgTid, TRUE) == TRUE;
    if (targetTid && targetTid != cur) attTarget = AttachThreadInput(cur, targetTid, TRUE) == TRUE;
    AllowSetForegroundWindow(ASFW_ANY);
    ShowWindow(hwnd, SW_SHOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attTarget) AttachThreadInput(cur, targetTid, FALSE);
    if (attFg) AttachThreadInput(cur, fgTid, FALSE);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    return GetForegroundWindow() == hwnd;
}

void TestBackgroundClickKeepsForeground(HWND /*edit*/) {
    static const wchar_t kProbeClass[] = L"QuickScriptWmClickProbe";
    static bool probeRegistered = false;
    if (!probeRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kProbeClass;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        probeRegistered = true;
    }

    HWND probe = CreateWindowExW(0, kProbeClass, L"QST Click Probe",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 80, 240, 160,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND decoy = CreateWindowExW(0, kTestClass, L"QST FG Decoy",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        640, 80, 280, 160,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!probe || !decoy) {
        if (probe) DestroyWindow(probe);
        if (decoy) DestroyWindow(decoy);
        Emit(L"background_click_keeps_foreground", false, L"CreateWindow failed");
        return;
    }
    ShowWindow(probe, SW_SHOWNOACTIVATE);
    ShowWindow(decoy, SW_SHOW);
    if (!ForceTestForeground(decoy)) {
        DestroyWindow(probe);
        DestroyWindow(decoy);
        Emit(L"background_click_keeps_foreground", true, L"skipped: cannot take foreground");
        return;
    }

    windowmode::PostMouseButtonToWindow(probe, 20, 20, MouseButtonType::Left, true);
    windowmode::PostMouseButtonToWindow(probe, 20, 20, MouseButtonType::Left, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool rawKept = GetForegroundWindow() == decoy;

    RECT rc{};
    GetWindowRect(probe, &rc);
    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    cfg.windowClassName = kProbeClass;
    cfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
    cfg.windowRelativeCoordinates = true;
    cfg.useTopLevelWindow = true;
    cfg.targetPickX = (rc.left + rc.right) / 2;
    cfg.targetPickY = (rc.top + rc.bottom) / 2;

    windowmode::WindowModeExecutor exec;
    std::wstring err;
    const bool began = exec.BeginRun(cfg, err);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const bool afterBind = GetForegroundWindow() == decoy;

    bool clickKept = false;
    bool noUia = false;
    bool stayedMin = false;
    bool minFgKept = false;
    if (began) {
        exec.PostMouseClickAtClient(20, 20, MouseButtonType::Left);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        clickKept = GetForegroundWindow() == decoy;
        noUia = exec.UiaInvokeCount() == 0;

        ShowWindow(probe, SW_SHOWMINNOACTIVE);
        ForceTestForeground(decoy);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        exec.PostMouseClickAtClient(20, 20, MouseButtonType::Left);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stayedMin = IsIconic(probe) != FALSE;
        minFgKept = GetForegroundWindow() == decoy;
        exec.EndRun();
    } else if (probe) {
        ShowWindow(probe, SW_SHOWNOACTIVATE);
    }

    const bool uiaGate = !windowmode::WindowUsesUiaClickFallback(probe);
    DestroyWindow(decoy);
    DestroyWindow(probe);

    const bool ok = rawKept && began && afterBind && clickKept && noUia
        && minFgKept && uiaGate;
    std::wstring detail;
    if (!began) {
        detail = L"BeginRun: " + err;
    } else {
        detail = (rawKept ? L"raw ok" : L"raw stole FG");
        detail += afterBind ? L" | bind ok" : L" | bind stole FG";
        detail += clickKept ? L" | click ok" : L" | click stole FG";
        detail += noUia ? L" | no UIA" : L" | UIA invoked";
        detail += stayedMin ? L" | stayed min" : L" | restored min";
        detail += minFgKept ? L" | min FG ok" : L" | min stole FG";
        detail += uiaGate ? L" | uiaGate" : L" | uiaGate FAIL";
    }
    Emit(L"background_click_keeps_foreground", ok, detail.c_str());
}

void TestWindowClientScale() {
    int x = 100, y = 200;
    const bool half = windowmode::ScaleWindowClientPoint(2000, 1000, 1000, 500, x, y)
        && x == 50 && y == 100;

    int sameX = 80, sameY = 90;
    const bool same = !windowmode::ScaleWindowClientPoint(800, 600, 800, 600, sameX, sameY)
        && sameX == 80 && sameY == 90;

    int skipX = 10, skipY = 10;
    const bool invalid = !windowmode::ScaleWindowClientPoint(0, 0, 100, 100, skipX, skipY)
        && skipX == 10 && skipY == 10;

    int r1 = 10, t1 = 20, r2 = 110, t2 = 120;
    windowmode::ScaleWindowClientRect(200, 200, 100, 100, r1, t1, r2, t2);
    const bool rectOk = r1 == 5 && t1 == 10 && r2 == 55 && t2 == 60;

    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.windowRelativeCoordinates = true;
    cfg.recordClientWidth = 1920;
    cfg.recordClientHeight = 1080;
    std::wstring written;
    windowmode::WriteWindowModeJson(written, cfg, false);
    const auto round = windowmode::ParseWindowModeJson(written);
    const bool jsonOk = round.recordClientWidth == 1920 && round.recordClientHeight == 1080
        && round.windowRelativeCoordinates;

    CoordMeta meta{};
    meta.captureWidth = 800;
    meta.captureHeight = 600;
    const TemplateScale halfTpl = ComputeTemplateScale(meta, 400, 300);
    const bool tplOk = std::fabs(halfTpl.sx - 0.5) < 1e-6
        && std::fabs(halfTpl.sy - 0.5) < 1e-6;

    const bool ok = half && same && invalid && rectOk && jsonOk && tplOk;
    std::wstring detail;
    if (!ok) {
        detail = (half ? L"half ok" : L"half FAIL");
        detail += same ? L" | same ok" : L" | same FAIL";
        detail += invalid ? L" | invalid ok" : L" | invalid FAIL";
        detail += rectOk ? L" | rect ok" : L" | rect FAIL";
        detail += jsonOk ? L" | json ok" : L" | json FAIL";
        detail += tplOk ? L" | tpl ok" : L" | tpl FAIL";
    }
    Emit(L"window_client_scale", ok, detail.c_str());
}

void TestWindowRelativePlaybackEnablesWm() {
    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = false;
    cfg.windowRelativeCoordinates = true;
    cfg.windowClassName = L"UnityWndClass";
    cfg.targetExePath = L"C:\\Games\\game.exe";
    cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    windowmode::FinalizeWindowModeForPlayback(cfg, true, false);
    const bool editorDefaultOk = !cfg.enabled
        && cfg.coordSpace == windowmode::WindowModeCoordinateSpace::WindowClient
        && cfg.executionKind == windowmode::WindowModeExecutionKind::HiddenDesktop
        && cfg.windowClassName == L"UnityWndClass";

    windowmode::WindowModeScriptConfig revivedCfg = cfg;
    windowmode::FinalizeWindowModeForPlayback(revivedCfg, true, true);
    const bool reviveOk = revivedCfg.enabled
        && revivedCfg.coordSpace == windowmode::WindowModeCoordinateSpace::WindowClient
        && revivedCfg.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow;

    std::wstring written;
    windowmode::WindowModeScriptConfig offRel = revivedCfg;
    offRel.enabled = false;
    windowmode::WriteWindowModeJson(written, offRel, false);
    const auto parsed = windowmode::ParseWindowModeJson(written);
    const bool keepId = !parsed.enabled
        && parsed.windowRelativeCoordinates
        && parsed.windowClassName == L"UnityWndClass"
        && parsed.targetExePath.find(L"game.exe") != std::wstring::npos;

    windowmode::WindowModeScriptConfig revived = parsed;
    windowmode::FinalizeWindowModeForPlayback(revived, true, true);
    const bool parseRevive = revived.enabled && revived.windowClassName == L"UnityWndClass";

    windowmode::WindowModeScriptConfig parsedStay = parsed;
    windowmode::FinalizeWindowModeForPlayback(parsedStay, true, false);
    const bool parseStayOff = !parsedStay.enabled;

    windowmode::WindowModeScriptConfig noId{};
    noId.windowRelativeCoordinates = true;
    windowmode::FinalizeWindowModeForPlayback(noId, true, true);
    const bool noIdStaysOff = !noId.enabled;

    const bool ok = editorDefaultOk && reviveOk && keepId && parseRevive && parseStayOff && noIdStaysOff;
    std::wstring detail;
    if (!ok) {
        detail = editorDefaultOk ? L"editorDefault ok" : L"editorDefault FAIL";
        detail += reviveOk ? L" | revive ok" : L" | revive FAIL";
        detail += keepId ? L" | keepId ok" : L" | keepId FAIL";
        detail += parseRevive ? L" | parseRevive ok" : L" | parseRevive FAIL";
        detail += parseStayOff ? L" | parseStayOff ok" : L" | parseStayOff FAIL";
        detail += noIdStaysOff ? L" | noId ok" : L" | noId FAIL";
    }
    Emit(L"window_relative_playback_enables_wm", ok, detail.c_str());
}

void TestBackgroundMinimizedQuietRestore() {
    static const wchar_t kMinClass[] = L"QuickScriptWmMinProbe";
    static bool minRegistered = false;
    if (!minRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kMinClass;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        minRegistered = true;
    }

    HWND probe = CreateWindowExW(0, kMinClass, L"QST Min Probe",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, 320, 220,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    RegisterTestClass();
    HWND decoy = CreateWindowExW(0, kTestClass, L"QST Min Decoy",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        500, 100, 280, 160,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!probe || !decoy) {
        if (probe) DestroyWindow(probe);
        if (decoy) DestroyWindow(decoy);
        Emit(L"background_minimized_quiet_restore", false, L"CreateWindow failed");
        return;
    }
    ShowWindow(probe, SW_SHOWMINNOACTIVE);
    ShowWindow(decoy, SW_SHOW);
    if (!ForceTestForeground(decoy)) {
        DestroyWindow(probe);
        DestroyWindow(decoy);
        Emit(L"background_minimized_quiet_restore", true, L"skipped: cannot take foreground");
        return;
    }
    if (!IsIconic(probe)) {
        DestroyWindow(decoy);
        DestroyWindow(probe);
        Emit(L"background_minimized_quiet_restore", false, L"probe not iconic before BeginRun");
        return;
    }

    RECT rc{};
    GetWindowRect(probe, &rc);
    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    cfg.windowClassName = kMinClass;
    cfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
    cfg.windowRelativeCoordinates = true;
    cfg.useTopLevelWindow = true;
    cfg.targetPickX = (rc.left + rc.right) / 2;
    cfg.targetPickY = (rc.top + rc.bottom) / 2;

    windowmode::WindowModeExecutor exec;
    std::wstring err;
    const bool began = exec.BeginRun(cfg, err);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const bool fgKept = GetForegroundWindow() == decoy;
    const bool restored = began && IsIconic(probe) == FALSE;
    bool clickFgKept = false;
    bool remin = false;
    if (began) {
        exec.PostMouseClickAtClient(20, 20, MouseButtonType::Left);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        clickFgKept = GetForegroundWindow() == decoy;
        exec.EndRun();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        remin = IsIconic(probe) != FALSE;
        clickFgKept = clickFgKept && GetForegroundWindow() == decoy;
    }

    DestroyWindow(decoy);
    DestroyWindow(probe);

    const bool ok = began && fgKept && restored && clickFgKept && remin;
    std::wstring detail;
    if (!began) {
        detail = L"BeginRun: " + err;
    } else {
        detail = fgKept ? L"bind FG ok" : L"bind stole FG";
        detail += restored ? L" | restored" : L" | still iconic";
        detail += clickFgKept ? L" | click FG ok" : L" | click stole FG";
        detail += remin ? L" | EndRun min" : L" | EndRun not min";
    }
    Emit(L"background_minimized_quiet_restore", ok, detail.c_str());
}

void TestBackgroundInputAndroidRender() {
    constexpr wchar_t kParentCls[] = L"QstWmEmuParent";
    constexpr wchar_t kRenderCls[] = L"TheRender";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kParentCls;
    RegisterClassExW(&wc);

    WNDCLASSEXW wcRender = wc;
    wcRender.lpszClassName = kRenderCls;
    RegisterClassExW(&wcRender);

    HWND parent = CreateWindowExW(0, kParentCls, L"LDPlayer",
        WS_OVERLAPPEDWINDOW, 60, 60, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    HWND toolbar = CreateWindowExW(0, kParentCls, L"toolbar",
        WS_CHILD | WS_VISIBLE, 0, 0, 800, 48, parent, nullptr, wc.hInstance, nullptr);
    HWND render = CreateWindowExW(0, kRenderCls, L"TheRender",
        WS_CHILD | WS_VISIBLE, 0, 48, 800, 552, parent, nullptr, wc.hInstance, nullptr);
    (void)toolbar;
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    cfg.targetExePath = L"C:\\LDPlayer\\dnplayer.exe";

    windowmode::BackgroundInputTargetKind kind = windowmode::BackgroundInputTargetKind::TopLevel;
    HWND found = windowmode::FindBackgroundInputChild(parent, &cfg, &kind);
    const bool ok = found == render
        && kind == windowmode::BackgroundInputTargetKind::AndroidEmulatorRender;

    DestroyWindow(parent);
    UnregisterClassW(kRenderCls, wc.hInstance);
    UnregisterClassW(kParentCls, wc.hInstance);
    Emit(L"background_input_android_render", ok,
        ok ? L"" : L"TheRender child not found for android emulator config");
}

void TestBackgroundInputCoordMap() {
    constexpr wchar_t kParentCls[] = L"QstWmEmuMapParent";
    constexpr wchar_t kRenderCls[] = L"TheRender";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kParentCls;
    RegisterClassExW(&wc);
    WNDCLASSEXW wcRender = wc;
    wcRender.lpszClassName = kRenderCls;
    RegisterClassExW(&wcRender);

    HWND parent = CreateWindowExW(0, kParentCls, L"LDPlayer",
        WS_OVERLAPPEDWINDOW, 80, 80, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    HWND render = CreateWindowExW(0, kRenderCls, L"TheRender",
        WS_CHILD | WS_VISIBLE, 0, 40, 640, 440, parent, nullptr, wc.hInstance, nullptr);
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    int cx = 100;
    int cy = 120;
    const bool mapped = windowmode::MapClientPointBetweenHwnds(parent, render, cx, cy);
    const bool ok = mapped && cx == 100 && cy == 80;

    DestroyWindow(parent);
    UnregisterClassW(kRenderCls, wc.hInstance);
    UnregisterClassW(kParentCls, wc.hInstance);
    Emit(L"background_input_coord_map", ok,
        ok ? L"" : L"parent→render coordinate remap wrong");
}

void TestBackgroundInputSdlSurface() {
    constexpr wchar_t kParentCls[] = L"QstWmSdlParent";
    constexpr wchar_t kSdlCls[] = L"SDL_app";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kParentCls;
    RegisterClassExW(&wc);
    WNDCLASSEXW wcSdl = wc;
    wcSdl.lpszClassName = kSdlCls;
    RegisterClassExW(&wcSdl);

    HWND parent = CreateWindowExW(0, kParentCls, L"Game",
        WS_OVERLAPPEDWINDOW, 100, 100, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    HWND sdl = CreateWindowExW(0, kSdlCls, L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 640, 480, parent, nullptr, wc.hInstance, nullptr);
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    windowmode::BackgroundInputTargetKind kind = windowmode::BackgroundInputTargetKind::TopLevel;
    HWND found = windowmode::FindBackgroundInputChild(parent, nullptr, &kind);
    const bool ok = found == sdl
        && (kind == windowmode::BackgroundInputTargetKind::KnownRenderSurface
            || kind == windowmode::BackgroundInputTargetKind::LargestSurface);

    DestroyWindow(parent);
    UnregisterClassW(kSdlCls, wc.hInstance);
    UnregisterClassW(kParentCls, wc.hInstance);
    Emit(L"background_input_sdl_surface", ok,
        ok ? L"" : L"SDL_app render child not preferred");
}

void TestBackgroundInputDesktopEmulatorTop() {
    constexpr wchar_t kDesmumeCls[] = L"DeSmuME";
    constexpr wchar_t kToolbarCls[] = L"WindowToolBar32";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kDesmumeCls;
    RegisterClassExW(&wc);
    WNDCLASSEXW wcToolbar = wc;
    wcToolbar.lpszClassName = kToolbarCls;
    RegisterClassExW(&wcToolbar);

    HWND parent = CreateWindowExW(0, kDesmumeCls, L"DeSmuME",
        WS_OVERLAPPEDWINDOW, 120, 120, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    HWND toolbar = CreateWindowExW(0, kToolbarCls, L"toolbar",
        WS_CHILD | WS_VISIBLE, 0, 0, 800, 48, parent, nullptr, wc.hInstance, nullptr);
    HWND surface = CreateWindowExW(0, kDesmumeCls, L"surface",
        WS_CHILD | WS_VISIBLE, 0, 48, 800, 552, parent, nullptr, wc.hInstance, nullptr);
    (void)toolbar;
    (void)surface;
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    windowmode::BackgroundInputTargetKind kind = windowmode::BackgroundInputTargetKind::TopLevel;
    HWND foundNullCfg = windowmode::FindBackgroundInputChild(parent, nullptr, &kind);
    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    cfg.windowClassName = kDesmumeCls;
    HWND foundCfg = windowmode::FindBackgroundInputChild(parent, &cfg, &kind);
    const bool ok = foundNullCfg == parent && foundCfg == parent
        && kind == windowmode::BackgroundInputTargetKind::TopLevel;

    DestroyWindow(parent);
    UnregisterClassW(kToolbarCls, wc.hInstance);
    UnregisterClassW(kDesmumeCls, wc.hInstance);
    Emit(L"background_input_desktop_emu_top", ok,
        ok ? L"" : L"DeSmuME must post to top-level hwnd, not child surface");
}

void TestBackgroundInputMumuQtRecursive() {
    constexpr wchar_t kParentCls[] = L"QstWmMumuParent";
    constexpr wchar_t kQtShellCls[] = L"Qt5156QWindowIcon";
    constexpr wchar_t kQtRenderCls[] = L"Qt5156QWindowIcon";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kParentCls;
    RegisterClassExW(&wc);
    WNDCLASSEXW wcQt = wc;
    wcQt.lpszClassName = kQtShellCls;
    RegisterClassExW(&wcQt);
    WNDCLASSEXW wcQt2 = wc;
    wcQt2.lpszClassName = kQtRenderCls;
    RegisterClassExW(&wcQt2);

    HWND parent = CreateWindowExW(0, kParentCls, L"MuMu安卓设备-1",
        WS_OVERLAPPEDWINDOW, 120, 120, 900, 700, nullptr, nullptr, wc.hInstance, nullptr);
    HWND shell = CreateWindowExW(0, kQtShellCls, L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 900, 700, parent, nullptr, wc.hInstance, nullptr);
    HWND render = CreateWindowExW(0, kQtRenderCls, L"",
        WS_CHILD | WS_VISIBLE, 0, 40, 900, 660, shell, nullptr, wc.hInstance, nullptr);
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    cfg.windowClassName = L"Qt5156QWindowIcon";
    cfg.windowName = L"MuMu安卓设备-1";

    windowmode::BackgroundInputTargetKind kind = windowmode::BackgroundInputTargetKind::TopLevel;
    HWND found = windowmode::FindBackgroundInputChild(parent, &cfg, &kind);
    const bool ok = found == render
        && kind == windowmode::BackgroundInputTargetKind::AndroidEmulatorRender;

    DestroyWindow(parent);
    UnregisterClassW(kQtRenderCls, wc.hInstance);
    UnregisterClassW(kQtShellCls, wc.hInstance);
    UnregisterClassW(kParentCls, wc.hInstance);
    Emit(L"background_input_mumu_qt_recursive", ok,
        ok ? L"" : L"nested Qt QWindowIcon render child not found for MuMu");
}

void TestAndroidQtFakeFocusGate() {
    windowmode::WindowModeScriptConfig mumu{};
    mumu.enabled = true;
    mumu.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    mumu.windowClassName = L"Qt5156QWindowIcon";
    mumu.windowName = L"MuMu安卓设备-1";
    const bool mumuFf = windowmode::LooksLikeQtRenderWindowClass(mumu.windowClassName)
        && !windowmode::UsesFakeFocusOrAndroidQt(mumu)
        && !windowmode::UsesFakeFocus(mumu);

    const bool mumuNoHw = !windowmode::AndroidEmulatorNeedsHardwareInput(nullptr, &mumu);

    windowmode::WindowModeScriptConfig ld{};
    ld.enabled = true;
    ld.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    ld.targetExePath = L"D:\\LDPlayer\\dnplayer.exe";
    ld.childWindowClassName = L"TheRender";
    const bool ldNoFf = !windowmode::UsesFakeFocusOrAndroidQt(ld);
    const bool ldNoHw = !windowmode::AndroidEmulatorNeedsHardwareInput(nullptr, &ld);

    const bool ok = mumuFf && mumuNoHw && ldNoFf && ldNoHw;
    Emit(L"android_qt_fake_focus_gate", ok,
        ok ? L"" : L"MuMu PostMessage-only / LDPlayer gate wrong");
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
        Emit(L"post_key_click_char", false, L"skipped");
        Emit(L"post_key_shift_char", false, L"skipped");
        Emit(L"background_bind_child", false, L"skipped");
        Emit(L"background_quick_input", false, L"skipped");
        Emit(L"background_click_keeps_foreground", false, L"skipped");
        Emit(L"quick_input_cancel", false, L"skipped");
        Emit(L"window_list_enumerates_self", false, L"skipped");
        Emit(L"window_list_match_and_format", false, L"skipped");
        Emit(L"window_activate_foreground", false, L"skipped");
        Emit(L"screen_point_to_client_within", false, L"skipped");
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        TestFindTarget(edit);
        TestPostQuickInput(edit);
        TestPostKeyClickChar(edit);
        TestPostKeyShiftChar(edit);
        TestExecutorBackground(edit);
        TestBackgroundClickKeepsForeground(edit);
        TestQuickInputCancel(edit);
        TestWindowListAndActivate(edit);
        TestScreenPointToClientWithin(edit);
        DestroyWindow(ParentTopWindow(edit));
    }

    TestWindowClientScale();
    TestWindowRelativePlaybackEnablesWm();
    TestBackgroundMinimizedQuietRestore();
    TestBackgroundInputAndroidRender();
    TestBackgroundInputCoordMap();
    TestBackgroundInputSdlSurface();
    TestBackgroundInputDesktopEmulatorTop();
    TestBackgroundInputMumuQtRecursive();
    TestAndroidQtFakeFocusGate();

    TestDesktopQuickInputCancel();
    TestFakeFocusJsonRoundtrip();
    TestKernelAnticheatBlocksBackground();
    TestInputStrategyCdpAuto();
    TestExtBridgeConfigParse();
    TestFakeFocusMinimizeGate();
    TestSoftMessageExeGates();
    TestRestorePreferMaximized();
    TestMonitorCoveringFullscreen();
    TestGameHardwareWithoutInject();
    TestHardwareOffscreenPark();
    TestVdaSelectsOsDll();
    TestFakeFocusHookLocal();
    TestFakeFocusLiteUnreal();
    TestFakeFocusAirFocusOnly();
    TestFakeFocusMapleStoryFocusOnly();
    TestFakeFocus32ExportRva();
    TestRemoteModuleKernel32();
    TestFakeFocusHeaderExportRva();
    TestFakeFocusGlfwLiteCursor();
    TestFakeFocusSoftInput();
    TestAnjuzhenScriptWmConfig();
    TestPermissionMatchUipi();
    TestMapleStoryBackgroundFakeFocus();
    TestTargetLostAfterDestroy();
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
