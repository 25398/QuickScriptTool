// WindowModeDiag — 宏桌面找图诊断工具，输出 wmdiag_report.txt 与截图供分析
#include "window_mode/macro_virtual_desktop.h"
#include "window_mode/virtual_desktop_accessor.h"
#include "window_mode/window_capture.h"
#include "window_mode/window_mode_executor.h"
#include "window_mode/window_target.h"

#include "image_match.h"

#include <dwmapi.h>

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>

namespace {

FILE* gReport = nullptr;

void Log(const wchar_t* msg) {
    std::fwprintf(stdout, L"%s\n", msg);
    if (gReport) std::fwprintf(gReport, L"%s\n", msg);
}

void Logf(const wchar_t* fmt, ...) {
    wchar_t buf[1024]{};
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    Log(buf);
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    const auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos);
    return dir;
}

std::wstring ShowCmdName(UINT cmd) {
    switch (cmd) {
    case SW_HIDE: return L"SW_HIDE";
    case SW_SHOWNORMAL: return L"SW_SHOWNORMAL";
    case SW_SHOWMINIMIZED: return L"SW_SHOWMINIMIZED";
    case SW_SHOWMAXIMIZED: return L"SW_SHOWMAXIMIZED";
    case SW_SHOWNOACTIVATE: return L"SW_SHOWNOACTIVATE";
    case SW_SHOW: return L"SW_SHOW";
    case SW_MINIMIZE: return L"SW_MINIMIZE";
    case SW_SHOWMINNOACTIVE: return L"SW_SHOWMINNOACTIVE";
    case SW_SHOWNA: return L"SW_SHOWNA";
    case SW_RESTORE: return L"SW_RESTORE";
    default: return L"cmd=" + std::to_wstring(cmd);
    }
}

bool IsWindowCloaked(HWND hwnd) {
    int cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked != 0;
}

struct DesktopSnap {
    int userDesktop = -1;
    int targetDesktop = -1;
    int onCurrentVd = -1;
    bool iconic = false;
    UINT showCmd = 0;
    bool cloaked = false;
    std::wstring desktopName;
};

DesktopSnap Snap(HWND root) {
    DesktopSnap s{};
    auto& vda = windowmode::VirtualDesktopAccessor::Instance();
    std::wstring err;
    vda.EnsureLoaded(err);
    s.userDesktop = vda.GetCurrentDesktopNumber();
    s.targetDesktop = vda.GetWindowDesktopNumber(root);
    s.onCurrentVd = vda.IsWindowOnCurrentVirtualDesktop(root);
    s.iconic = IsIconic(root) == TRUE;
    s.cloaked = IsWindowCloaked(root);
    if (s.targetDesktop >= 0) {
        s.desktopName = vda.GetDesktopName(s.targetDesktop);
    }
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(root, &wp)) {
        s.showCmd = wp.showCmd;
    }
    return s;
}

void LogSnap(const wchar_t* label, const DesktopSnap& s) {
    Logf(L"  [%s] UserDesktop=%d TargetDesktop=%d (%s) OnCurrentVD=%d Iconic=%d Cloaked=%d showCmd=%s",
        label, s.userDesktop, s.targetDesktop,
        s.desktopName.empty() ? L"?" : s.desktopName.c_str(),
        s.onCurrentVd, s.iconic ? 1 : 0, s.cloaked ? 1 : 0,
        ShowCmdName(s.showCmd).c_str());
}

bool DesktopChanged(const DesktopSnap& before, const DesktopSnap& after) {
    return before.userDesktop >= 0 && after.userDesktop >= 0
        && before.userDesktop != after.userDesktop;
}

void SaveCapture(HWND hwnd, const std::wstring& tag) {
    windowmode::WindowCaptureResult cap = windowmode::CaptureWindowClient(hwnd);
    if (!cap.bitmap) {
        Logf(L"  [%s] Capture FAILED (null bitmap)", tag.c_str());
        return;
    }
    const bool blank = windowmode::IsCaptureLikelyBlank(cap.bitmap);
    const std::wstring path = ExeDir() + L"\\wmdiag_" + tag + L".bmp";
    const bool saved = SaveBitmapToFile(cap.bitmap, path);
    Logf(L"  [%s] %dx%d blank=%d saved=%d -> %s",
        tag.c_str(), cap.w, cap.h, blank ? 1 : 0, saved ? 1 : 0, path.c_str());
    DeleteObject(cap.bitmap);
}

HWND ParseHwnd(const wchar_t* arg) {
    if (!arg || !*arg) return nullptr;
    unsigned long long v = 0;
    if (arg[0] == L'0' && (arg[1] == L'x' || arg[1] == L'X')) {
        v = wcstoull(arg + 2, nullptr, 16);
    } else {
        v = wcstoull(arg, nullptr, 10);
    }
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(v));
}

HWND ResolveTargetHwnd(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--hwnd") == 0 && i + 1 < argc) {
            return ParseHwnd(argv[i + 1]);
        }
        if (wcscmp(argv[i], L"--foreground") == 0) {
            return GetForegroundWindow();
        }
    }
    Log(L"请把要测试的窗口切到前台，5 秒后采集...");
    for (int i = 5; i > 0; --i) {
        Logf(L"  %d...", i);
        Sleep(1000);
    }
    return GetForegroundWindow();
}

bool HasFlag(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], flag) == 0) return true;
    }
    return false;
}

windowmode::WindowModeScriptConfig BuildConfig(HWND target, HWND root) {
    windowmode::WindowModeScriptConfig cfg{};
    cfg.enabled = true;
    cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    cfg.coordSpace = windowmode::WindowModeCoordinateSpace::WindowClient;
    cfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
    cfg.useTopLevelWindow = (target == root);
    cfg.autoLaunchTarget = false;
    cfg.targetExePath = L"C:\\Windows\\System32\\notepad.exe";

    wchar_t cls[256]{};
    wchar_t title[512]{};
    GetClassNameW(root, cls, 256);
    GetWindowTextW(root, title, 512);
    cfg.windowClassName = cls;
    cfg.windowName = title;

    RECT rc{};
    GetWindowRect(target, &rc);
    cfg.targetPickX = (rc.left + rc.right) / 2;
    cfg.targetPickY = (rc.top + rc.bottom) / 2;
    return cfg;
}

bool StepDesktopSwitch(const wchar_t* stepName, HWND root,
    const std::function<void()>& action) {
    const DesktopSnap before = Snap(root);
    Logf(L"--- STEP: %s ---", stepName);
    LogSnap(L"before", before);
    action();
    Sleep(250);
    const DesktopSnap after = Snap(root);
    LogSnap(L"after", after);
    const bool changed = DesktopChanged(before, after);
    if (changed) {
        Logf(L"  >>> 桌面切换! %d -> %d <<<", before.userDesktop, after.userDesktop);
    } else {
        Log(L"  桌面未切换");
    }
    return changed;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring reportPath = ExeDir() + L"\\wmdiag_report.txt";
    _wfopen_s(&gReport, reportPath.c_str(), L"w, ccs=UTF-8");

    Log(L"========================================");
    Log(L"  WindowModeDiag 宏桌面找图诊断");
    Log(L"========================================");
    Logf(L"报告: %s", reportPath.c_str());
    Log(L"用法:");
    Log(L"  WindowModeDiag.exe                    (5秒后取前台窗口)");
    Log(L"  WindowModeDiag.exe --foreground");
    Log(L"  WindowModeDiag.exe --hwnd 0x00123456");
    Log(L"  加 --minimized 测试最小化场景");
    Log(L"");

    HWND target = ResolveTargetHwnd(argc, argv);
    if (!target || !IsWindow(target)) {
        Log(L"[错误] 无效窗口句柄");
        if (gReport) fclose(gReport);
        return 1;
    }

    HWND root = windowmode::TopLevelTargetWindow(target);
    wchar_t cls[256]{}, title[512]{};
    GetClassNameW(root, cls, 256);
    GetWindowTextW(root, title, 512);
    Logf(L"目标 HWND=0x%p 根=0x%p 类=%s 标题=%s",
        target, root, cls, title);

    auto& vda = windowmode::VirtualDesktopAccessor::Instance();
    std::wstring err;
    if (!vda.EnsureLoaded(err)) {
        Logf(L"[错误] VDA: %s", err.c_str());
        if (gReport) fclose(gReport);
        return 1;
    }
    Logf(L"VDA=%s 桌面数=%d 当前桌面=%d",
        vda.LoadedDllName().c_str(), vda.GetDesktopCount(), vda.GetCurrentDesktopNumber());

    windowmode::MacroVirtualDesktop macroDesktop;
    if (!macroDesktop.OpenOrCreate()) {
        Logf(L"[错误] 宏桌面: %s", macroDesktop.LastError().c_str());
        if (gReport) fclose(gReport);
        return 1;
    }
    Logf(L"宏桌面 index=%d 名称=%s",
        macroDesktop.DesktopIndex(),
        vda.GetDesktopName(macroDesktop.DesktopIndex()).c_str());

    if (!vda.IsWindowOnDesktopNumber(root, macroDesktop.DesktopIndex())) {
        Log(L"移入宏桌面（MoveWindowToDesktopNumber）...");
        const int deskBefore = vda.GetCurrentDesktopNumber();
        if (!macroDesktop.MoveWindowToMacroDesktop(root)) {
            Logf(L"[错误] %s", macroDesktop.LastError().c_str());
            if (gReport) fclose(gReport);
            return 1;
        }
        Sleep(300);
        Logf(L"  UserDesktop %d -> %d %s",
            deskBefore, vda.GetCurrentDesktopNumber(),
            deskBefore != vda.GetCurrentDesktopNumber() ? L"**切换**" : L"OK");
    }

    if (HasFlag(argc, argv, L"--minimized")) {
        Log(L"--minimized: 最小化目标...");
        ShowWindow(root, SW_MINIMIZE);
        Sleep(500);
    }

    Log(L"");
    Log(L"========== 基线 ==========");
    LogSnap(L"baseline", Snap(root));
    SaveCapture(target, L"00_baseline");

    int switchCount = 0;

    Log(L"");
    Log(L"========== 逐步检测（哪一步切桌面） ==========");

    if (StepDesktopSwitch(L"A: PinMacroDesktopWindowBottom", root, [&] {
            windowmode::PinMacroDesktopWindowBottom(root);
        })) ++switchCount;

    if (StepDesktopSwitch(L"B: ScopedVisionCapturePrep (仅构造)", root, [&] {
            windowmode::ScopedVisionCapturePrep prep(root, false);
            Logf(L"  prep.Ready=%d", prep.Ready() ? 1 : 0);
            SaveCapture(target, L"01_after_prep_ctor");
        })) ++switchCount;

    Sleep(200);
    LogSnap(L"after_prep_dtor", Snap(root));
    SaveCapture(target, L"02_after_prep_dtor");

    windowmode::WindowModeExecutor exec;
    windowmode::BeginRunOptions opts{};
    opts.launchTarget = false;
    const windowmode::WindowModeScriptConfig cfg = BuildConfig(target, root);

    if (StepDesktopSwitch(L"C: Executor BeginRun + Bind", root, [&] {
            if (!exec.BeginRun(cfg, err, opts)) {
                Logf(L"  BeginRun FAIL: %s", err.c_str());
            } else {
                Logf(L"  BeginRun OK target=0x%p", exec.TargetHwnd());
            }
        })) ++switchCount;

    if (exec.IsActive()) {
        if (StepDesktopSwitch(L"D: DiagnoseVisionPipeline (完整模拟第一次找图)", root, [&] {
                const auto d = exec.DiagnoseVisionPipeline();
                Logf(L"  ensureReady=%d prepReady=%d capture=%dx%d ok=%d blank=%d err=%s",
                    d.ensureReadyOk ? 1 : 0, d.prepReady ? 1 : 0,
                    d.captureW, d.captureH, d.captureOk ? 1 : 0, d.captureBlank ? 1 : 0,
                    d.ensureErr.c_str());
                SaveCapture(exec.TargetHwnd(), L"03_pipeline");
            })) ++switchCount;
    }

    Log(L"");
    Log(L"========== 汇总 ==========");
    Logf(L"切桌面步骤数: %d", switchCount);
    LogSnap(L"final", Snap(root));

    Log(L"");
    Log(L"请把以下文件发给我分析:");
    Logf(L"  %s", reportPath.c_str());
    Log(L"  同目录 wmdiag_*.bmp");
    Log(L"");
    Log(L"按 Enter 退出...");

    exec.EndRun();
    if (gReport) fclose(gReport);
    (void)getwchar();
    return switchCount > 0 ? 2 : 0;
}
