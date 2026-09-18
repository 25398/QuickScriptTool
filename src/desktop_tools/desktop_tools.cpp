// ──────────────────────────────────────────────────────────────────
// desktop_tools.cpp — DesktopTools 门面实现（调现有 overlay，不改行为）
// ──────────────────────────────────────────────────────────────────

#include "desktop_tools/desktop_tools.h"
#include "desktop_tools/float_ball_geom.h"

#include "coord_space.h"
#include "crosshair_drag.h"
#include "drag_pick_overlay.h"
#include "drawing.h"
#include "findimage_template_crop.h"
#include "hotkey_dialog.h"
#include "image_match.h"
#include "image_var_util.h"
#include "input/virtual_hid.h"
#include "opencv_runtime.h"
#include "macro_debug_window.h"
#include "ocr_engine.h"
#include "ocr_overlay.h"
#include "process_utils.h"
#include "screenshot_overlay.h"
#include "utils.h"
#include "window_mode/window_target.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <urlmon.h>

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

namespace qst::desktop_tools {
namespace {

std::mutex g_diagMu;

void AppendFindImageDiag(const std::wstring& line) {
    std::lock_guard<std::mutex> lock(g_diagMu);
    std::wstring path = AppDir() + L"\\findimage_diag.log";
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"ab") != 0 || !fp) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fwprintf(fp, L"%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds, line.c_str());
    fclose(fp);
}

std::wstring ResolveExistingPath(const std::wstring& path) {
    if (path.empty()) return {};
    wchar_t full[MAX_PATH]{};
    if (GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr) != 0
        && GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) {
        return full;
    }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    return {};
}

bool FileExistsAttr(const std::wstring& path) {
    return !path.empty()
        && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring LocalAppQstDir() {
    wchar_t buf[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) return {};
    const std::wstring d = std::wstring(buf) + L"\\QuickScriptTool";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

bool HidPackageReadyAt(const std::wstring& scriptDir) {
    if (scriptDir.empty()) return false;
    const std::wstring pkg = scriptDir + L"\\package";
    return FileExistsAttr(pkg + L"\\QstVHid.sys")
        && FileExistsAttr(pkg + L"\\qst_vhid.inf")
        && FileExistsAttr(pkg + L"\\interception.sys");
}

std::wstring InterceptionDllBesideExe() {
    return AppDir() + L"\\interception.dll";
}

bool RunHiddenProcess(std::wstring cmd, DWORD timeoutMs) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}

bool DownloadUrlToFile(const std::wstring& url, const std::wstring& dest) {
    DeleteFileW(dest.c_str());
    const HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), dest.c_str(), 0, nullptr);
    if (!SUCCEEDED(hr) || !FileExistsAttr(dest)) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(dest.c_str(), GetFileExInfoStandard, &fad)) return false;
    const ULONGLONG n = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    return n >= 1024;
}

bool ExtractZipTo(const std::wstring& zip, const std::wstring& dest) {
    CreateDirectoryW(dest.c_str(), nullptr);
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
        L"\"Expand-Archive -LiteralPath '" + zip + L"' -DestinationPath '" + dest
        + L"' -Force\"";
    return RunHiddenProcess(std::move(cmd), 180000);
}

void PromoteHidExtractLayout(const std::wstring& dest) {
    if (FileExistsAttr(dest + L"\\driver\\qst_vhid\\_elevate_install.ps1")) return;
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW((dest + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        const std::wstring child = dest + L"\\" + fd.cFileName;
        if (FileExistsAttr(child + L"\\driver\\qst_vhid\\_elevate_install.ps1")
            || FileExistsAttr(child + L"\\_elevate_install.ps1")) {
            // 单根目录压缩：把内容提升到 dest
            const std::wstring cmd = L"cmd.exe /c xcopy /E /Y /Q \"" + child
                + L"\\*\" \"" + dest + L"\\\" >nul";
            RunHiddenProcess(cmd, 120000);
            break;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

bool CopyIfPresent(const std::wstring& from, const std::wstring& to) {
    if (!FileExistsAttr(from)) return false;
    const auto slash = to.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        const std::wstring dir = to.substr(0, slash);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

bool MergeHidPayloadInto(const std::wstring& extracted, const std::wstring& appDir) {
    PromoteHidExtractLayout(extracted);
    CreateDirectoryW((appDir + L"\\driver").c_str(), nullptr);
    CreateDirectoryW((appDir + L"\\driver\\qst_vhid").c_str(), nullptr);
    CreateDirectoryW((appDir + L"\\driver\\qst_vhid\\package").c_str(), nullptr);
    const std::wstring srcRoot = FileExistsAttr(extracted + L"\\driver\\qst_vhid\\_elevate_install.ps1")
        ? (extracted + L"\\driver\\qst_vhid")
        : extracted;
    CopyIfPresent(srcRoot + L"\\_elevate_install.ps1",
        appDir + L"\\driver\\qst_vhid\\_elevate_install.ps1");
    CopyIfPresent(srcRoot + L"\\repair_boot.ps1",
        appDir + L"\\driver\\qst_vhid\\repair_boot.ps1");
    const std::wstring pkgSrc = srcRoot + L"\\package";
    CopyIfPresent(pkgSrc + L"\\QstVHid.sys", appDir + L"\\driver\\qst_vhid\\package\\QstVHid.sys");
    CopyIfPresent(pkgSrc + L"\\qst_vhid.inf", appDir + L"\\driver\\qst_vhid\\package\\qst_vhid.inf");
    CopyIfPresent(pkgSrc + L"\\qst_vhid.cat", appDir + L"\\driver\\qst_vhid\\package\\qst_vhid.cat");
    CopyIfPresent(pkgSrc + L"\\interception.sys", appDir + L"\\driver\\qst_vhid\\package\\interception.sys");
    const std::wstring dllSrc = FileExistsAttr(extracted + L"\\interception.dll")
        ? (extracted + L"\\interception.dll")
        : (srcRoot + L"\\interception.dll");
    CopyIfPresent(dllSrc, appDir + L"\\interception.dll");
    // 默认 zip 已带安装脚本：不能仅凭脚本存在就当合并成功（Program Files 不可写时 .sys 拷失败）。
    return FileExistsAttr(appDir + L"\\driver\\qst_vhid\\_elevate_install.ps1")
        && HidPackageReadyAt(appDir + L"\\driver\\qst_vhid");
}

bool DownloadAndInstallHidPayload(std::string* err) {
    const std::wstring url =
        L"https://www.quickscripttool.cloud/downloads/QuickScriptTool-HidDriver.zip";
    const std::wstring local = LocalAppQstDir();
    const std::wstring zip = (local.empty() ? AppDir() : local) + L"\\QuickScriptTool-HidDriver.zip";
    const std::wstring stage = (local.empty() ? AppDir() : local) + L"\\hid_driver_extract";
    if (!DownloadUrlToFile(url, zip)) {
        if (err) *err = "无法从官网下载驱动包。请检查网络，或手动打开 "
            "https://www.quickscripttool.cloud/downloads/QuickScriptTool-HidDriver.zip";
        return false;
    }
    if (!ExtractZipTo(zip, stage)) {
        if (err) *err = "驱动包下载成功但解压失败";
        return false;
    }
    std::wstring dest = AppDir();
    if (!MergeHidPayloadInto(stage, dest)) {
        dest = local;
        if (!MergeHidPayloadInto(stage, dest)) {
            if (err) *err = "驱动包解压后未找到安装脚本";
            return false;
        }
    }
    return true;
}

std::string JsonEscapeUtf8(const std::wstring& w) {
    const std::string s = ToUtf8(w);
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            } else {
                o.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return o;
}

std::string JsonStringW(const std::wstring& w) {
    return "\"" + JsonEscapeUtf8(w) + "\"";
}

double NormalizeThresholdPercent(double thr) {
    if (thr > 0.0 && thr <= 1.0) thr *= 100.0;
    if (thr < 1.0) thr = 1.0;
    if (thr > 100.0) thr = 100.0;
    return thr;
}

}  // namespace

ScopedHideShell::ScopedHideShell(HWND shellHwnd) : hwnd(shellHwnd) {
    if (!hwnd) return;
    wasVisible = IsWindowVisible(hwnd) != FALSE;
    if (!wasVisible) return;
    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);
    // 对齐原生 HideEditorForScreenCapture：刷消息并稍等，避免截屏/找图仍含编辑窗残影
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Sleep(150);
}

ScopedHideShell::~ScopedHideShell() {
    if (hwnd && wasVisible) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }
}

namespace {

void FlushUiMessagesBriefly() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool IsOwnCaptureUiClass(const wchar_t* cls) {
    if (!cls || !cls[0]) return false;
    return _wcsicmp(cls, L"QstWebViewShellWindow") == 0
        || _wcsicmp(cls, L"KeyMouseDebugWebWindow") == 0
        || _wcsicmp(cls, L"QstAgentWebWindow") == 0
        || _wcsicmp(cls, L"KeyMouseDebugWnd") == 0
        || _wcsicmp(cls, qst::desktop_tools::kFloatBallClass) == 0;
}

void CollectOwnUiHwnds(HWND mainUi, std::vector<HWND>& out) {
    auto add = [&](HWND h) {
        if (!h || !IsWindow(h)) return;
        for (HWND x : out) if (x == h) return;
        out.push_back(h);
    };
    add(mainUi);
    add(MacroDebug().Hwnd());
    add(FindWindowW(L"KeyMouseDebugWebWindow", nullptr));
    add(FindWindowW(L"KeyMouseDebugWnd", nullptr));
    add(FindWindowW(L"QstAgentWebWindow", nullptr));
    add(FindWindowW(L"QstWebViewShellWindow", nullptr));
    add(FindWindowW(qst::desktop_tools::kFloatBallClass, nullptr));

    const DWORD pid = GetCurrentProcessId();
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* vec = reinterpret_cast<std::vector<HWND>*>(lp);
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid != GetCurrentProcessId()) return TRUE;
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
        wchar_t cls[128]{};
        if (GetClassNameW(hwnd, cls, 128) <= 0) return TRUE;
        if (!IsOwnCaptureUiClass(cls)) return TRUE;
        for (HWND x : *vec) if (x == hwnd) return TRUE;
        vec->push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    (void)pid;
}

bool CloakWindowForCapture(HWND hwnd, bool cloak) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    BOOL v = cloak ? TRUE : FALSE;
    return SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &v, sizeof(v)));
}

bool PreferCloakForCapture(HWND hwnd) {
    // WebView2 宿主被 SW_HIDE/SHOW 后，合成层偶发只剩一块旧尺寸脏矩形
    // （深蓝日志底 + 四周 sky 宿主底），宏调试窗看起来像「坏了」。
    // DWM cloak 不拆 HWND/控制器，截屏照样不含本软件窗。
    wchar_t cls[128]{};
    if (GetClassNameW(hwnd, cls, 128) <= 0) return false;
    if (_wcsicmp(cls, qst::desktop_tools::kFloatBallClass) == 0) return false;
    return IsOwnCaptureUiClass(cls);
}

}  // namespace

ScopedHideOwnUiForCapture::ScopedHideOwnUiForCapture(HWND mainUi) {
    CollectOwnUiHwnds(mainUi, hwnds);
    wasVisible.assign(hwnds.size(), 0);
    usedCloak.assign(hwnds.size(), 0);
    bool any = false;
    for (size_t i = 0; i < hwnds.size(); ++i) {
        HWND h = hwnds[i];
        if (IsWindowVisible(h) && !IsIconic(h)) {
            wasVisible[i] = 1;
            if (PreferCloakForCapture(h) && CloakWindowForCapture(h, true)) {
                usedCloak[i] = 1;
            } else {
                ShowWindow(h, SW_HIDE);
            }
            any = true;
        }
    }
    if (!any) return;
    FlushUiMessagesBriefly();
    Sleep(80);
}

ScopedHideOwnUiForCapture::~ScopedHideOwnUiForCapture() {
    for (size_t i = 0; i < hwnds.size(); ++i) {
        if (!wasVisible[i]) continue;
        HWND h = hwnds[i];
        if (!h || !IsWindow(h)) continue;
        if (i < usedCloak.size() && usedCloak[i]) {
            CloakWindowForCapture(h, false);
        } else {
            ShowWindow(h, SW_SHOWNOACTIVATE);
        }
    }
}

ScreenRegionResult PickScreenRegion(HWND owner, const wchar_t* title) {
    ScreenRegionResult out;
    ScopedHideShell hide(owner);
    ScreenshotOverlay overlay;
    overlay.SetTitle(title ? title : L"选取区域");
    RECT result{};
    bool got = false;
    overlay.Show([&](RECT rc) {
        result = rc;
        got = (rc.right > rc.left && rc.bottom > rc.top);
    });
    if (!got) {
        out.detail = "cancelled";
        return out;
    }
    int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
    GetVirtualScreenRect(vsX, vsY, vsW, vsH);
    out.ok = true;
    out.x1 = result.left + vsX;
    out.y1 = result.top + vsY;
    out.x2 = result.right + vsX;
    out.y2 = result.bottom + vsY;
    return out;
}

DragPickResult PickScreenDrag(HWND owner) {
    DragPickResult out;
    ScopedHideShell hide(owner);
    const DragPickOutcome r = ShowScreenDragPickOverlay();
    if (!r.ok) {
        out.detail = "cancelled";
        return out;
    }
    out.ok = true;
    out.x1 = r.x1;
    out.y1 = r.y1;
    out.x2 = r.x2;
    out.y2 = r.y2;
    out.durationSec = r.durationSec;
    return out;
}

DragPickResult PickTemplateDrag(HWND owner, const std::wstring& imagePath) {
    DragPickResult out;
    ScopedHideShell hide(owner);
    const std::wstring resolved = ResolveImagePath(imagePath);
    HBITMAP bmp = LoadBitmapFromFile(resolved);
    if (!bmp) {
        out.detail = "无法加载模板图";
        return out;
    }
    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    const DragPickOutcome r = ShowTemplateDragPickOverlay(bmp, bm.bmWidth, bm.bmHeight);
    DeleteBitmapHandle(bmp);
    if (!r.ok) {
        out.detail = "cancelled";
        return out;
    }
    out.ok = true;
    out.x1 = r.x1;
    out.y1 = r.y1;
    out.x2 = r.x2;
    out.y2 = r.y2;
    out.durationSec = r.durationSec;
    return out;
}

TemplateCaptureResult CaptureTemplateScreenshot(HWND owner, const wchar_t* title) {
    TemplateCaptureResult out;
    ScopedHideShell hide(owner);
    ScreenshotOverlay overlay;
    overlay.SetTitle(title ? title : L"屏幕截图");
    RECT result{};
    bool got = false;
    overlay.Show([&](RECT rc) {
        result = rc;
        got = (rc.right > rc.left && rc.bottom > rc.top);
    });
    if (!got) {
        out.detail = "cancelled";
        return out;
    }
    int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
    GetVirtualScreenRect(vsX, vsY, vsW, vsH);
    const int x1 = result.left + vsX;
    const int y1 = result.top + vsY;
    const int x2 = result.right + vsX;
    const int y2 = result.bottom + vsY;
    EnsureFindImagesDir();
    const std::wstring path = FindImagesDir() + L"\\template_"
        + std::to_wstring(GetTickCount()) + L".bmp";
    HBITMAP bmp = CaptureScreenRegion(x1, y1, x2, y2);
    if (!bmp || !SaveBitmapToFile(bmp, path)) {
        DeleteBitmapHandle(bmp);
        out.detail = "截图保存失败";
        return out;
    }
    DeleteBitmapHandle(bmp);
    const std::wstring stored = ImagePathForJson(path);
    out.ok = true;
    out.imagePath = stored.empty() ? path : stored;
    out.resolvedPath = path;
    return out;
}

namespace {
bool ResolveBoundWindowClientScreenRect(
    const std::wstring& className,
    const std::wstring& title,
    const std::wstring& exePath,
    int& x1, int& y1, int& x2, int& y2) {
    windowmode::WindowTargetQuery q;
    q.className = className;
    q.titleContains = title;
    q.exePath = exePath;
    HWND hwnd = windowmode::FindMainWindowDefault(q, true);
    HWND cap = hwnd;
    if (hwnd) {
        if (HWND surface = windowmode::FindBrowserCaptureSurface(hwnd)) cap = surface;
    }
    RECT rc{};
    POINT origin{0, 0};
    if (cap && IsWindow(cap) && GetClientRect(cap, &rc)
        && (rc.right - rc.left) > 0 && (rc.bottom - rc.top) > 0
        && ClientToScreen(cap, &origin)) {
        x1 = origin.x;
        y1 = origin.y;
        x2 = origin.x + (rc.right - rc.left);
        y2 = origin.y + (rc.bottom - rc.top);
        return x2 > x1 && y2 > y1;
    }
    return false;
}

bool TryConstrainSearchRectToWindow(
    int constrainToWindow,
    const std::wstring& className,
    const std::wstring& title,
    const std::wstring& exePath,
    int& x1, int& y1, int& x2, int& y2,
    bool& windowConstrained,
    std::string& err,
    const char* missingWindowErr) {
    windowConstrained = false;
    if (!constrainToWindow) return true;
    const bool hasIdentity = !className.empty() || !title.empty() || !exePath.empty();
    if (!hasIdentity) return true;
    if (!ResolveBoundWindowClientScreenRect(className, title, exePath, x1, y1, x2, y2)) {
        err = missingWindowErr ? missingWindowErr
            : "未找到目标窗口，无法在窗口内操作。请先打开并绑定窗口后再测试。";
        return false;
    }
    windowConstrained = true;
    return true;
}
}  // namespace


FindImageMatchResult FindImageMatch(HWND owner, const FindImageMatchParams& params) {
    FindImageMatchResult out;
    out.modeUtf8 = params.modeUtf8;
    if (!OpenCvAvailable()) {
        out.detail = ToUtf8(OpenCvUnavailableMessage());
        AppendFindImageDiag(L"findImageMatch skipped: OpenCV unavailable");
        return out;
    }
    AppendFindImageDiag(std::wstring(L"findImageMatch mode=") + FromUtf8(params.modeUtf8)
        + L" imagePath=" + params.imagePath
        + L" search=(" + std::to_wstring(params.searchX1) + L"," + std::to_wstring(params.searchY1)
        + L")-(" + std::to_wstring(params.searchX2) + L"," + std::to_wstring(params.searchY2)
        + L") full=" + std::to_wstring(params.searchFullScreen)
        + L" thr=" + std::to_wstring(params.matchThreshold)
        + L" perfect=" + std::to_wstring(params.perfectMatch)
        + L" scale=" + std::to_wstring(params.imageScaleMin)
        + L"~" + std::to_wstring(params.imageScaleMax));

    // 变量模式：合成锚框选区 / 选偏移（不要求当前帧能匹配到模板文件）
    if (params.modeUtf8 == "regionBySize" || params.modeUtf8 == "offsetBySize") {
        int w = params.syntheticW;
        int h = params.syntheticH;
        if (params.syntheticUseScreen) {
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            w = vsW;
            h = vsH;
        } else if ((w <= 0 || h <= 0) && !params.imagePath.empty()) {
            std::wstring resolved = params.imagePath;
            if (resolved.size() < 2 || resolved[1] != L':') {
                resolved = ResolveImagePath(params.imagePath);
            }
            GetImageFileSize(resolved, w, h);
        }
        if (w <= 0 || h <= 0) {
            out.detail = params.modeUtf8 == "offsetBySize"
                ? "无法确定图片尺寸，无法在屏幕中心画出锚框"
                : "无法确定图片尺寸，请手填相对区域坐标";
            return out;
        }
        ScopedHideShell hide(owner);
        MatchOverlay overlay;
        const auto result = (params.modeUtf8 == "offsetBySize")
            ? overlay.ShowSyntheticAnchorOffset(w, h)
            : overlay.ShowSyntheticAnchor(w, h);
        if (result.cancelled) {
            out.detail = "cancelled";
            return out;
        }
        out.ok = true;
        out.offsetX = result.offsetX;
        out.offsetY = result.offsetY;
        out.regionValid = result.regionValid;
        out.regionX1 = result.regionX1;
        out.regionY1 = result.regionY1;
        out.regionX2 = result.regionX2;
        out.regionY2 = result.regionY2;
        out.found = overlay.matchDone_ && overlay.matchResult_.found;
        if (out.found) {
            out.matchTopLeftX = overlay.matchResult_.topLeftX;
            out.matchTopLeftY = overlay.matchResult_.topLeftY;
        }
        out.matchCount = static_cast<int>(overlay.matchResults_.size());
        out.bestScore = 100.0;
        return out;
    }

    if (params.imagePath.empty()) {
        out.detail = "请先截图或选择图片";
        AppendFindImageDiag(L"  -> 空图片路径");
        return out;
    }
    std::wstring resolved = ResolveImagePath(params.imagePath);
    if (resolved.empty() || GetFileAttributesW(resolved.c_str()) == INVALID_FILE_ATTRIBUTES) {
        out.detail = std::string("找不到路径: ")
            + ToUtf8(resolved.empty() ? params.imagePath : resolved);
        AppendFindImageDiag(L"  -> 解析失败: " + (resolved.empty() ? params.imagePath : resolved));
        return out;
    }
    out.resolvedPath = resolved;
    AppendFindImageDiag(L"  -> resolved=" + resolved);

    int x1 = params.searchX1, y1 = params.searchY1;
    int x2 = params.searchX2, y2 = params.searchY2;
    int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
    GetVirtualScreenRect(vsX, vsY, vsW, vsH);
    bool windowConstrained = false;
    if (!TryConstrainSearchRectToWindow(
            params.constrainToWindow, params.windowClassName, params.windowTitle,
            params.targetExePath, x1, y1, x2, y2, windowConstrained, out.detail,
            "未找到目标窗口，无法在窗口内找图。请先打开并绑定窗口后再测试。")) {
        AppendFindImageDiag(L"  -> constrainToWindow: 未找到目标窗口");
        return out;
    }
    if (windowConstrained) {
        AppendFindImageDiag(std::wstring(L"  -> constrainToWindow search=(")
            + std::to_wstring(x1) + L"," + std::to_wstring(y1)
            + L")-(" + std::to_wstring(x2) + L"," + std::to_wstring(y2) + L")");
    }
    if (!windowConstrained && (params.searchFullScreen || x2 <= x1 || y2 <= y1)) {
        x1 = vsX;
        y1 = vsY;
        x2 = vsX + vsW;
        y2 = vsY + vsH;
    }

    const double thr = NormalizeThresholdPercent(params.matchThreshold);
    ScriptAction probe{};
    probe.matchThreshold = thr;
    probe.perfectMatch = params.perfectMatch != 0;
    probe.imageScaleMin = (std::max)(0.1, params.imageScaleMin);
    probe.imageScaleMax = (std::max)(probe.imageScaleMin, params.imageScaleMax);
    CoordMeta meta{};
    const TemplateScale ts = ComputeTemplateScale(meta, vsW, vsH);
    ImageMatchOptions findOpt = BuildExecutionFindImageOptions(probe, ts);
    int keep = params.maxMatches > 0 ? params.maxMatches : 20;
    if (keep < 1) keep = 1;
    if (keep > 200) keep = 200;
    findOpt.maxMatches = keep;
    findOpt.maxOverlap = 0.5;

    MatchOverlayMode mode = MatchOverlayMode::Test;
    if (params.modeUtf8 == "offset") mode = MatchOverlayMode::OffsetPick;
    else if (params.modeUtf8 == "region") mode = MatchOverlayMode::RelativeRegionPick;
    if (mode == MatchOverlayMode::OffsetPick || mode == MatchOverlayMode::RelativeRegionPick)
        findOpt.maxMatches = 1;

    ScopedHideShell hide(owner);
    MatchOverlay overlay;
    if (windowConstrained) overlay.SetAllowExpandSearchToVirtualScreen(false);
    const auto result = overlay.Show(resolved, x1, y1, x2, y2, findOpt, mode);
    if (result.cancelled) {
        out.detail = "cancelled";
        AppendFindImageDiag(L"  -> 用户取消");
        return out;
    }
    out.ok = true;
    out.offsetX = result.offsetX;
    out.offsetY = result.offsetY;
    out.regionValid = result.regionValid;
    out.regionX1 = result.regionX1;
    out.regionY1 = result.regionY1;
    out.regionX2 = result.regionX2;
    out.regionY2 = result.regionY2;
    out.found = overlay.matchDone_ && overlay.matchResult_.found;
    if (out.found) {
        out.matchTopLeftX = overlay.matchResult_.topLeftX;
        out.matchTopLeftY = overlay.matchResult_.topLeftY;
    }
    out.matchCount = static_cast<int>(overlay.matchResults_.size());
    out.bestScore = (overlay.matchDone_ && !overlay.matchResults_.empty())
        ? overlay.matchResults_.front().score
        : 0.0;
    AppendFindImageDiag(std::wstring(L"  -> found=") + (out.found ? L"1" : L"0")
        + L" count=" + std::to_wstring(out.matchCount)
        + L" best=" + std::to_wstring(out.bestScore)
        + L" region=(" + std::to_wstring(x1) + L"," + std::to_wstring(y1)
        + L")-(" + std::to_wstring(x2) + L"," + std::to_wstring(y2)
        + L") mode=" + FromUtf8(params.modeUtf8));
    return out;
}

namespace {
std::atomic_uint g_webCropFileSeq{0};
}  // namespace

FindImageCropResult FindImageCropRect(const std::wstring& imagePathOrStored,
    int offsetX, int offsetY, int cropX, int cropY, int cropW, int cropH) {
    FindImageCropResult out;
    if (!OpenCvAvailable()) {
        out.detail = ToUtf8(OpenCvUnavailableMessage());
        return out;
    }
    if (imagePathOrStored.empty()) {
        out.detail = "请先截图或选择图片";
        return out;
    }
    const std::wstring resolved = ResolveImagePath(imagePathOrStored);
    if (resolved.empty() || GetFileAttributesW(resolved.c_str()) == INVALID_FILE_ATTRIBUTES) {
        out.detail = std::string("找不到路径: ")
            + ToUtf8(resolved.empty() ? imagePathOrStored : resolved);
        return out;
    }
    HBITMAP bmp = LoadBitmapFromFile(resolved);
    if (!bmp) {
        out.detail = "无法加载图片";
        return out;
    }
    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    const int W = bm.bmWidth;
    const int H = bm.bmHeight;
    DeleteBitmapHandle(bmp);
    if (W < kFindImageCropMinSide || H < kFindImageCropMinSide) {
        out.detail = "图片过小，无法裁切";
        return out;
    }

    CropRect raw = NormalizeCropRect(cropX, cropY, cropX + cropW, cropY + cropH);
    const auto computed = ComputeCroppedFindImageOffset(W, H, offsetX, offsetY, raw);
    if (!computed.ok) {
        out.detail = "裁切区域无效";
        return out;
    }
    if (IsFullImageCrop(computed.rect, W, H)) {
        out.ok = true;
        out.unchanged = true;
        return out;
    }

    EnsureFindImagesDir();
    const unsigned seq = g_webCropFileSeq.fetch_add(1) + 1;
    const std::wstring name = MakeFindImageCropFileName(GetTickCount64(), seq);
    const std::wstring path = FindImagesDir() + L"\\" + name;
    if (!SaveCroppedTemplateRegion(resolved,
            computed.rect.L, computed.rect.T, computed.rect.R, computed.rect.B, path)) {
        out.detail = "保存裁切图片失败";
        return out;
    }
    const std::wstring stored = ImagePathForJson(path);
    out.ok = true;
    out.imagePath = stored.empty() ? path : stored;
    out.offsetX = computed.offsetX;
    out.offsetY = computed.offsetY;
    return out;
}

CrosshairPickResult CrosshairPick(HWND owner, const std::string& modeUtf8) {
    CrosshairPickResult out;
    out.modeUtf8 = modeUtf8.empty() ? "coordinates" : modeUtf8;
    if (!owner || !IsWindow(owner)) {
        out.detail = "无效宿主窗口";
        return out;
    }
    CrosshairDragMode mode = CrosshairDragMode::Coordinates;
    if (out.modeUtf8 == "programPath" || out.modeUtf8 == "program") {
        mode = CrosshairDragMode::ProgramPath;
    } else if (out.modeUtf8 == "windowTarget" || out.modeUtf8 == "window") {
        mode = CrosshairDragMode::WindowTarget;
    }

    CrosshairDragController drag;
    HCURSOR cursor = CreateCrosshairDragCursor(RGB(64, 140, 255));
    drag.SetOwner(owner);
    drag.SetDragCursor(cursor);

    int lastX = 0, lastY = 0;
    bool haveReleasePt = false;
    WindowInfoFromPoint info{};
    std::wstring programPath;
    bool cancelled = false;
    bool got = false;

    CrosshairDragBinding binding{};
    binding.mode = mode;
    if (mode == CrosshairDragMode::WindowTarget) {
        binding.onWindowTarget = [&](const WindowInfoFromPoint& i) {
            info = i;
            got = true;
        };
    }
    drag.Begin(binding);

    const bool startedWithButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool armed = startedWithButtonDown;

    MSG msg{};
    while (drag.IsActive() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        const UINT m = msg.message;
        if (m == WM_LBUTTONDOWN || m == WM_NCLBUTTONDOWN) armed = true;
        if ((m == WM_LBUTTONUP || m == WM_NCLBUTTONUP) && !armed) {
            continue;
        }
        const bool handled = drag.HandleMessage(msg.message, msg.wParam, msg.lParam,
            [&](int x, int y) {
                lastX = x;
                lastY = y;
                haveReleasePt = true;
            },
            [&](const std::wstring& path) {
                programPath = path;
                got = true;
            });
        if (handled) {
            if (m == WM_LBUTTONUP || m == WM_NCLBUTTONUP) {
                got = true;
                if (!haveReleasePt) {
                    POINT pt{};
                    GetCursorPos(&pt);
                    lastX = pt.x;
                    lastY = pt.y;
                    haveReleasePt = true;
                }
            }
            if (m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN) {
                cancelled = true;
            }
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            cancelled = true;
            drag.End();
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (cursor) DestroyCursor(cursor);

    if (cancelled || !got) {
        out.detail = "cancelled";
        return out;
    }
    if (!haveReleasePt) {
        POINT pt{};
        GetCursorPos(&pt);
        lastX = pt.x;
        lastY = pt.y;
        haveReleasePt = true;
    }

    std::ostringstream oss;
    if (mode == CrosshairDragMode::ProgramPath) {
        if (programPath.empty()) programPath = GetProcessPathFromPoint(lastX, lastY);
        oss << "{\"x\":" << lastX << ",\"y\":" << lastY
            << ",\"processPath\":" << JsonStringW(programPath) << "}";
    } else if (mode == CrosshairDragMode::WindowTarget) {
        if (info.processPath.empty() && info.windowClassName.empty() && info.windowTitle.empty()) {
            info = GetWindowInfoFromPoint(lastX, lastY);
        }
        oss << "{\"x\":" << info.x << ",\"y\":" << info.y
            << ",\"windowTitle\":" << JsonStringW(info.windowTitle)
            << ",\"windowClassName\":" << JsonStringW(info.windowClassName)
            << ",\"childWindowClassName\":" << JsonStringW(info.childWindowClassName)
            << ",\"processPath\":" << JsonStringW(info.processPath)
            << ",\"documentPath\":" << JsonStringW(info.documentPath) << "}";
    } else {
        oss << "{\"x\":" << lastX << ",\"y\":" << lastY << "}";
    }
    out.ok = true;
    out.pickJson = oss.str();
    return out;
}

WindowTargetResult PickWindowTarget(HWND owner) {
    WindowTargetResult out;
    if (!owner || !IsWindow(owner)) {
        out.detail = "无效宿主窗口";
        return out;
    }
    CrosshairDragController drag;
    HCURSOR cursor = CreateCrosshairDragCursor(RGB(64, 140, 255));
    drag.SetOwner(owner);
    drag.SetDragCursor(cursor);

    int lastX = 0, lastY = 0;
    bool haveReleasePt = false;
    WindowInfoFromPoint info{};
    bool cancelled = false;
    bool got = false;

    CrosshairDragBinding binding{};
    binding.mode = CrosshairDragMode::WindowTarget;
    binding.onWindowTarget = [&](const WindowInfoFromPoint& i) {
        info = i;
        got = true;
    };
    drag.Begin(binding);

    const bool startedWithButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool armed = startedWithButtonDown;

    MSG msg{};
    while (drag.IsActive() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        const UINT m = msg.message;
        if (m == WM_LBUTTONDOWN || m == WM_NCLBUTTONDOWN) armed = true;
        if ((m == WM_LBUTTONUP || m == WM_NCLBUTTONUP) && !armed) {
            continue;
        }
        const bool handled = drag.HandleMessage(msg.message, msg.wParam, msg.lParam,
            [&](int x, int y) {
                lastX = x;
                lastY = y;
                haveReleasePt = true;
            },
            [&](const std::wstring&) {});
        if (handled) {
            if (m == WM_LBUTTONUP || m == WM_NCLBUTTONUP) {
                got = true;
                if (!haveReleasePt) {
                    POINT pt{};
                    GetCursorPos(&pt);
                    lastX = pt.x;
                    lastY = pt.y;
                    haveReleasePt = true;
                }
            }
            if (m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN) {
                cancelled = true;
            }
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            cancelled = true;
            drag.End();
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (cursor) DestroyCursor(cursor);

    if (cancelled || !got) {
        out.detail = "cancelled";
        return out;
    }
    if (!haveReleasePt) {
        POINT pt{};
        GetCursorPos(&pt);
        lastX = pt.x;
        lastY = pt.y;
    }
    if (info.processPath.empty() && info.windowClassName.empty() && info.windowTitle.empty()) {
        info = GetWindowInfoFromPoint(lastX, lastY);
    }
    out.ok = true;
    out.pickX = info.x ? info.x : lastX;
    out.pickY = info.y ? info.y : lastY;
    out.windowTitle = info.windowTitle;
    out.windowClassName = info.windowClassName;
    out.childWindowClassName = info.childWindowClassName;
    out.processPath = info.processPath;
    out.documentPath = info.documentPath;
    return out;
}

ActionKeyResult CaptureActionKey(HWND owner, const Hotkey& oldValue) {
    ActionKeyResult out;
    HotkeyCapture cap;
    Hotkey editing = oldValue;
    if (editing.text.empty() && editing.vk != 0) editing.text = VkName(editing.vk);
    editing.enabled = editing.vk != 0;
    Hotkey captured{};
    const bool ok = cap.Show(owner, editing, false, captured);
    if (!ok || !captured.enabled || captured.vk == 0) {
        out.detail = "cancelled";
        return out;
    }
    out.ok = true;
    out.keyVk = captured.vk;
    out.keyText = captured.text;
    out.holdLeftCtrl = (captured.modifiers & MOD_CONTROL) != 0;
    out.holdLeftAlt = (captured.modifiers & MOD_ALT) != 0;
    out.holdLeftShift = (captured.modifiers & MOD_SHIFT) != 0;
    out.holdLeftWin = (captured.modifiers & MOD_WIN) != 0;
    return out;
}

HotkeyCaptureResult CaptureHotkey(HWND owner, const HotkeyCaptureParams& params) {
    HotkeyCaptureResult out;
    if (params.beginRelease) params.beginRelease();
    HotkeyCapture cap;
    Hotkey captured{};
    const bool ok = cap.Show(owner, params.editing, params.scriptHotkey, captured,
        params.globalStartStop, params.holdThresholdSeconds)
        && captured.enabled && captured.vk != 0;
    if (params.endRelease) params.endRelease();
    if (!ok) {
        out.detail = "cancelled";
        return out;
    }
    out.ok = true;
    out.hotkey = captured;
    return out;
}

TestOcrResult TestOcr(HWND owner, const TestOcrParams& params) {
    TestOcrResult out;
    out.modeUtf8 = params.modeUtf8.empty() ? "test" : params.modeUtf8;
    const bool offsetMode = (out.modeUtf8 == "offset");

    const OcrEnvStatus env = CheckOcrEnvironment(false);
    if (env.state != OcrEnvState::Ready) {
        out.detail = "OCR 未就绪";
        out.needInstall = true;
        return out;
    }

    int sx1 = params.searchX1, sy1 = params.searchY1;
    int sx2 = params.searchX2, sy2 = params.searchY2;
    double thr = NormalizeThresholdPercent(params.matchThreshold);

    // 校验在隐藏前完成，便于用户看到错误提示且不必闪一下编辑窗
    std::wstring resolvedImage;
    if (params.ocrRegionByImage) {
        if (params.imagePath.empty()) {
            out.detail = "请先设置要查找的图片。";
            return out;
        }
        resolvedImage = ResolveImagePath(params.imagePath);
        if (resolvedImage.empty()
            || GetFileAttributesW(resolvedImage.c_str()) == INVALID_FILE_ATTRIBUTES) {
            out.detail = "找不到参考图片。";
            return out;
        }
    }

    bool windowConstrained = false;
    if (!TryConstrainSearchRectToWindow(
            params.constrainToWindow, params.windowClassName, params.windowTitle,
            params.targetExePath, sx1, sy1, sx2, sy2, windowConstrained, out.detail,
            "未找到目标窗口，无法在窗口内识别。请先打开并绑定窗口后再测试。")) {
        return out;
    }

    // 凡需截屏/找图的测试路径：先藏壳再动手（对齐原生 TestOcr）
    ScopedHideShell hide(owner);

    if (params.ocrRegionByImage) {
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenRect(vsX, vsY, vsW, vsH);
        int findX1 = vsX, findY1 = vsY, findX2 = vsX + vsW, findY2 = vsY + vsH;
        if (windowConstrained) {
            findX1 = sx1; findY1 = sy1; findX2 = sx2; findY2 = sy2;
        } else if (!params.searchFullScreen && sx2 > sx1 && sy2 > sy1) {
            findX1 = sx1; findY1 = sy1; findX2 = sx2; findY2 = sy2;
        }
        HBITMAP tmpl = LoadBitmapFromFile(resolvedImage);
        if (!tmpl) {
            out.detail = "无法加载参考图片。";
            return out;
        }
        ScriptAction probe{};
        probe.matchThreshold = thr;
        probe.perfectMatch = params.perfectMatch != 0;
        probe.imageScaleMin = (std::max)(0.1, params.imageScaleMin);
        probe.imageScaleMax = (std::max)(probe.imageScaleMin, params.imageScaleMax);
        probe.imageRegionX1 = params.imageRegionX1;
        probe.imageRegionY1 = params.imageRegionY1;
        probe.imageRegionX2 = params.imageRegionX2;
        probe.imageRegionY2 = params.imageRegionY2;
        CoordMeta meta{};
        const TemplateScale ts = ComputeTemplateScale(meta, vsW, vsH);
        ImageMatchOptions opt = BuildExecutionFindImageOptions(probe, ts);
        RestrictFindImageToSingleAnchor(opt);
        opt.maxOverlap = 0.5;
        const ImageMatchOutput output = FindTemplateOnScreenMulti(
            findX1, findY1, findX2, findY2, tmpl, opt);
        DeleteBitmapHandle(tmpl);
        if (output.matches.empty()) {
            out.detail = offsetMode
                ? "未找到参考图片，无法选择偏移位置。"
                : "未找到参考图片，无法测试识别。";
            return out;
        }
        const ImageMatchResult& match = output.matches.front();
        if (!ApplyImageRegionToMatch(probe,
                match.topLeftX, match.topLeftY, match.bottomRightX, match.bottomRightY,
                sx1, sy1, sx2, sy2)) {
            out.detail = "相对区域无效。";
            return out;
        }
    } else if (!windowConstrained && (params.searchFullScreen || (sx2 <= sx1 || sy2 <= sy1))) {
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenRect(vsX, vsY, vsW, vsH);
        sx1 = vsX; sy1 = vsY; sx2 = vsX + vsW; sy2 = vsY + vsH;
    }

    EnsureOcrSession();
    if (offsetMode) {
        OcrOverlay overlay;
        const auto result = overlay.Show(
            sx1, sy1, sx2, sy2, params.ocrSearchText,
            OcrOverlayMode::OffsetPick, params.ocrDigitsOnly != 0);
        ReleaseOcrSession();
        if (result.cancelled || !result.anchorValid) {
            out.detail = "cancelled";
            return out;
        }
        out.ok = true;
        out.offsetX = result.offsetX;
        out.offsetY = result.offsetY;
        return out;
    }
    // 对齐原生 TestOcr：冻屏叠层跑识别并绘制红框/状态条（非静默 OCR）
    {
        OcrOverlay overlay;
        std::wstring searchTarget;
        if (params.ocrResultMode == 1) searchTarget = params.ocrSearchText;
        const auto result = overlay.Show(
            sx1, sy1, sx2, sy2, searchTarget,
            OcrOverlayMode::Test, params.ocrDigitsOnly != 0);
        ReleaseOcrSession();
        out.ok = true;
        out.text = result.text;
        if (!result.ocrOk && !result.text.empty()) {
            // 仍返回 ok，叠层已展示失败原因；detail 供调试
            out.detail = ToUtf8(result.text);
        }
        return out;
    }
}

InstallDriverResult InstallDriver(HWND owner, const std::string& kindUtf8,
    const DriverProgressFn& onProgress, bool uninstall) {
    InstallDriverResult out;
    out.kindUtf8 = kindUtf8;
    auto progress = [&](int percent, int step, const char* status) {
        if (onProgress) onProgress(percent, step, status);
    };
    const std::wstring appDir = AppDir();
    const bool isVhid = (kindUtf8 == "vhid" || kindUtf8 == "virtualHid");
    const bool isIc = (kindUtf8 == "interception" || kindUtf8 == "ic");
    if (!isVhid && !isIc) {
        out.detail = "unknown driver kind";
        return out;
    }

    progress(10, 0, uninstall ? "检查卸载脚本…" : "检查安装文件…");
    const std::wstring localQst = LocalAppQstDir();
    const std::wstring cands[] = {
        appDir + L"\\driver\\qst_vhid\\_elevate_install.ps1",
        localQst + L"\\driver\\qst_vhid\\_elevate_install.ps1",
        appDir + L"\\..\\driver\\qst_vhid\\_elevate_install.ps1",
        appDir + L"\\..\\..\\driver\\qst_vhid\\_elevate_install.ps1",
    };
    auto scriptDirOf = [](const std::wstring& scriptPath) -> std::wstring {
        std::wstring dir = scriptPath;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir.resize(slash);
        return dir;
    };
    auto pickScript = [&](bool preferPackage) -> std::wstring {
        std::wstring fallback;
        for (const auto& cand : cands) {
            const std::wstring resolved = ResolveExistingPath(cand);
            if (resolved.empty()) continue;
            if (fallback.empty()) fallback = resolved;
            if (!preferPackage || HidPackageReadyAt(scriptDirOf(resolved))) return resolved;
        }
        return preferPackage ? std::wstring{} : fallback;
    };
    std::wstring script = pickScript(true);
    if (script.empty()) script = pickScript(false);
    if (!uninstall) {
        const std::wstring scriptDir = scriptDirOf(script);
        const bool needPkg = !HidPackageReadyAt(scriptDir);
        const bool needDll = isIc && !FileExistsAttr(InterceptionDllBesideExe())
            && !FileExistsAttr(localQst + L"\\interception.dll");
        if (script.empty() || needPkg || needDll) {
            progress(18, 0, "正在下载驱动包…");
            std::string dlErr;
            if (!DownloadAndInstallHidPayload(&dlErr)) {
                out.detail = dlErr.empty()
                    ? "未找到驱动文件，且官网下载失败"
                    : dlErr;
                return out;
            }
            script = pickScript(true);
            if (script.empty()) script = pickScript(false);
        }
    }
    if (script.empty()) {
        out.detail = "未找到驱动安装脚本。请检查网络后重试，或从官网下载 QuickScriptTool-HidDriver.zip";
        return out;
    }
    const auto readLogTail = [&](int maxLines) -> std::string {
        std::wstring dir = script;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir.resize(slash);
        std::ifstream log((dir + L"\\install_log.txt").c_str());
        if (!log) return {};
        std::deque<std::string> lines;
        std::string line;
        while (std::getline(log, line)) {
            if (line.find("EXIT_CODE=") != std::string::npos) continue;
            lines.push_back(line);
            if (static_cast<int>(lines.size()) > maxLines) lines.pop_front();
        }
        std::string tail;
        for (const auto& l : lines) {
            if (!tail.empty()) tail += "\n";
            tail += l;
        }
        if (!tail.empty() && static_cast<unsigned char>(tail[0]) == 0xEF) {
            tail = tail.substr(3);  // 去掉 PowerShell UTF-8 BOM
        }
        return tail;
    };

    const std::wstring kindArg = isVhid ? L"vhid" : L"interception";
    std::wstring params =
        L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -Kind " + kindArg;
    if (uninstall) {
        params += L" -Uninstall";
    }

    progress(35, 1, "申请管理员权限…");
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD e = GetLastError();
        out.detail = (e == ERROR_CANCELLED) ? "已取消（需要管理员权限）" : "启动安装脚本失败";
        return out;
    }
    progress(70, 2, uninstall ? "卸载驱动 / 清除过滤…" : "安装驱动 / 注册服务…");
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 300000);
        DWORD exitCode = 1;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        // 旧脚本 10/11 曾改 BCD / 进固件重启，会把机器装到无法开机。新产品一律拒绝跟进。
        if (exitCode == 10 || exitCode == 11) {
            out.detail = "已拒绝修改启动配置（测试签名 / Secure Boot / 内存完整性）。"
                         "请改用「系统模拟」，或点「卸载并修复」清除旧版残留。";
            return out;
        }
        if (exitCode == 99) {
            out.detail = "本地缺少驱动内核文件。请再点安装以下载 QuickScriptTool-HidDriver.zip，"
                         "或从官网手动下载后重试。";
            return out;
        }
        if (exitCode == 3) {
            out.detail = "当前 Windows 内核不信任该驱动签名。已中止安装，"
                         "未改启动项、未注册键盘/鼠标过滤驱动。请继续使用「系统模拟」。";
            return out;
        }
        if (exitCode == 5) {
            out.detail = "内核驱动未能启动（签名策略拒载）。已回滚，"
                         "未改键盘/鼠标过滤驱动，电脑可正常重启。请使用「系统模拟」。";
            return out;
        }
        if (exitCode == 4) {
            out.detail = "已拒绝为加载驱动去关闭安全启动或开启测试签名。请使用「系统模拟」。";
            return out;
        }
        if (exitCode == 90) {
            out.detail = "管理员权限未生效（UAC 未将安装脚本提权）。请关闭本程序，右键以管理员身份重新运行后再试。";
            const std::string tail = readLogTail(8);
            if (!tail.empty()) {
                out.detail += "\n\n安装日志：\n" + tail;
            }
            return out;
        }
        if (exitCode != 0) {
            out.detail = uninstall ? "卸载未完成" : "安装未完成（设备未就绪）";
            const std::string tail = readLogTail(8);
            if (!tail.empty()) {
                out.detail += "。安装日志：\n" + tail;
            }
            out.detail += "（脚本退出码 " + std::to_string(exitCode) + "）";
            return out;
        }
    }
    progress(100, 3, uninstall ? "卸载完成" : "安装完成");
    out.ok = true;
    out.uninstalled = uninstall;
    return out;
}

VhidInstallStatus QueryVhidInstallStatus() {
    VhidInstallStatus out;
    const std::wstring appDir = AppDir();
    const std::wstring scriptDir = appDir + L"\\driver\\qst_vhid";

    const std::wstring scriptDirLocal = LocalAppQstDir() + L"\\driver\\qst_vhid";
    out.installScriptPresent =
        FileExistsAttr(scriptDir + L"\\_elevate_install.ps1")
        || FileExistsAttr(scriptDirLocal + L"\\_elevate_install.ps1")
        || FileExistsAttr(appDir + L"\\..\\driver\\qst_vhid\\_elevate_install.ps1");
    out.packagePresent = HidPackageReadyAt(scriptDir) || HidPackageReadyAt(scriptDirLocal)
        || HidPackageReadyAt(appDir + L"\\..\\driver\\qst_vhid");
    out.hidDllPresent = FileExistsAttr(InterceptionDllBesideExe())
        || FileExistsAttr(LocalAppQstDir() + L"\\interception.dll");

    // 内存完整性（HVCI）：与 Windows 安全中心同一注册表开关
    HKEY hvciKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios"
            L"\\HypervisorEnforcedCodeIntegrity",
            0, KEY_READ, &hvciKey) == ERROR_SUCCESS) {
        DWORD enabled = 0, cb = sizeof(enabled);
        if (RegQueryValueExW(hvciKey, L"Enabled", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(&enabled), &cb) == ERROR_SUCCESS) {
            out.hvciEnabled = (enabled == 1);
        }
        RegCloseKey(hvciKey);
    }

    // 待重启续装：安装脚本已注册计划任务（Task Scheduler 任务文件存在即可判定）
    void* fsRedir = nullptr;
    Wow64DisableWow64FsRedirection(&fsRedir);
    const DWORD taskAttr =
        GetFileAttributesW(L"C:\\Windows\\System32\\Tasks\\QstVHidFinishInstall");
    const DWORD taskSbAttr =
        GetFileAttributesW(L"C:\\Windows\\System32\\Tasks\\QstVHidWaitSb");
    if (fsRedir) Wow64RevertWow64FsRedirection(fsRedir);
    out.rebootPending = taskAttr != INVALID_FILE_ATTRIBUTES ||
        taskSbAttr != INVALID_FILE_ATTRIBUTES;

    // 驱动就绪：设备接口可探测即视为可用
    out.driverReady = VirtualHidBackend::Instance().ProbeAvailable(nullptr);
    out.driverNeedsUpdate = false;
    if (out.driverReady) {
        unsigned va = 0, vb = 0, vc = 0, vd = 0;
        constexpr unsigned kMinA = 1, kMinB = 0, kMinC = 1, kMinD = 0;
        const bool haveVer = VirtualHidBackend::QueryInstalledDriverVersion(va, vb, vc, vd);
        auto pack = [](unsigned a, unsigned b, unsigned c, unsigned d) -> unsigned long long {
            return (static_cast<unsigned long long>(a) << 48)
                | (static_cast<unsigned long long>(b) << 32)
                | (static_cast<unsigned long long>(c) << 16)
                | static_cast<unsigned long long>(d);
        };
        if (!haveVer || pack(va, vb, vc, vd) < pack(kMinA, kMinB, kMinC, kMinD)) {
            out.driverNeedsUpdate = true;
        }
    }

    // 最近一次安装退出码（供 UI 提示上次失败原因）
    std::ifstream log((scriptDir + L"\\install_log.txt").c_str());
    std::string line;
    while (std::getline(log, line)) {
        const size_t pos = line.find("EXIT_CODE=");
        if (pos != std::string::npos) {
            out.lastExitCode = std::atoi(line.c_str() + pos + 10);
        }
        if (line.find("PENDING_SB=1") != std::string::npos) {
            out.pendingSb = true;
        }
    }
    return out;
}

BrowsePathResult BrowsePath(HWND owner, bool executableOnly) {
    BrowsePathResult out;

    // IFileOpenDialog 比 GetOpenFileName 启动更快，且少受旧式 shell 扩展拖累
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            dlg->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM
                | FOS_DONTADDTORECENT);
        }
        dlg->SetTitle(executableOnly ? L"选择程序" : L"选择文件");
        if (executableOnly) {
            const COMDLG_FILTERSPEC filters[] = {
                { L"可执行文件 (*.exe)", L"*.exe" },
                { L"所有文件 (*.*)", L"*.*" },
            };
            dlg->SetFileTypes(ARRAYSIZE(filters), filters);
            dlg->SetFileTypeIndex(1);
            dlg->SetDefaultExtension(L"exe");
        } else {
            const COMDLG_FILTERSPEC filters[] = {
                { L"所有文件 (*.*)", L"*.*" },
            };
            dlg->SetFileTypes(ARRAYSIZE(filters), filters);
        }
        hr = dlg->Show(owner && IsWindow(owner) ? owner : nullptr);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            out.detail = "cancelled";
            dlg->Release();
            return out;
        }
        if (SUCCEEDED(hr)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    out.ok = true;
                    out.path = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
        if (out.ok) return out;
        if (out.detail.empty()) out.detail = "cancelled";
        return out;
    }

    wchar_t fileBuf[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_DONTADDTORECENT
        | OFN_NOCHANGEDIR;
    if (executableOnly) {
        ofn.lpstrFilter = L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrTitle = L"选择程序";
    } else {
        ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0";
        ofn.lpstrTitle = L"选择文件";
    }
    if (!GetOpenFileNameW(&ofn)) {
        out.detail = "cancelled";
        return out;
    }
    out.ok = true;
    out.path = fileBuf;
    return out;
}

BrowsePathResult PickImageFile(HWND owner) {
    BrowsePathResult out;

    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            dlg->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM
                | FOS_DONTADDTORECENT);
        }
        dlg->SetTitle(L"选择图片");
        const COMDLG_FILTERSPEC filters[] = {
            { L"图片文件", L"*.bmp;*.png;*.jpg;*.jpeg;*.gif" },
            { L"所有文件 (*.*)", L"*.*" },
        };
        dlg->SetFileTypes(ARRAYSIZE(filters), filters);
        dlg->SetFileTypeIndex(1);
        hr = dlg->Show(owner && IsWindow(owner) ? owner : nullptr);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            out.detail = "cancelled";
            dlg->Release();
            return out;
        }
        if (SUCCEEDED(hr)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    out.ok = true;
                    out.path = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
        if (out.ok) return out;
        if (out.detail.empty()) out.detail = "cancelled";
        return out;
    }

    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Images\0*.bmp;*.png;*.jpg;*.jpeg;*.gif\0All\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_DONTADDTORECENT
        | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        out.detail = "cancelled";
        return out;
    }
    out.ok = true;
    out.path = file;
    return out;
}

void RequestShowDebugWindow(const std::function<void()>& applyDebugWindowSetting) {
    if (applyDebugWindowSetting) applyDebugWindowSetting();
}

namespace {
std::function<void(std::string)> g_macroDebugWebPoster;
MacroDebugController g_macroDebug;
}  // namespace

void SetMacroDebugWebPoster(std::function<void(std::string)> poster) {
    g_macroDebugWebPoster = std::move(poster);
    // 引擎 Create 时可能已 ApplyDebugWindowSetting，但当时 poster 未挂上
    g_macroDebug.FlushPendingWebShow();
}

void MacroDebugController::FlushPendingWebShow() {
    if (!webUi_ || !webVisible_) return;
    PostJson("{\"type\":\"debugWindow.show\",\"ok\":true}");
}

void MacroDebugController::PostJson(std::string jsonUtf8) const {
    if (g_macroDebugWebPoster) g_macroDebugWebPoster(std::move(jsonUtf8));
}

MacroDebugWindow& MacroDebugController::Native() {
    if (!native_) native_ = std::make_unique<MacroDebugWindow>();
    return *native_;
}

void MacroDebugController::SetWebUiEnabled(bool enabled) {
    if (webUi_ == enabled) return;
    if (webUi_ && !enabled) {
        webCreated_ = false;
        webVisible_ = false;
    }
    if (!webUi_ && enabled && native_) {
        native_->Hide();
    }
    webUi_ = enabled;
}

void MacroDebugController::Create(HFONT bodyFont, HFONT titleFont, HFONT closeFont,
                                  std::function<void()> onClosed) {
    onClosed_ = std::move(onClosed);
    if (webUi_) {
        webCreated_ = true;
        return;
    }
    Native().Create(bodyFont, titleFont, closeFont, onClosed_);
}

void MacroDebugController::Show() {
    if (webUi_) {
        webCreated_ = true;
        webVisible_ = true;
        PostJson("{\"type\":\"debugWindow.show\",\"ok\":true}");
        return;
    }
    Native().Show();
}

void MacroDebugController::Hide() {
    if (webUi_) {
        // 从未打开过：勿向壳推 hide（旧壳在未就绪时会误 Ensure→弹出）
        if (!webCreated_ && !webVisible_) return;
        webVisible_ = false;
        PostJson("{\"type\":\"debugWindow.hide\",\"ok\":true}");
        return;
    }
    if (native_) native_->Hide();
}

void MacroDebugController::MarkWebHidden() {
    webVisible_ = false;
}

void MacroDebugController::Destroy() {
    webCreated_ = false;
    webVisible_ = false;
    if (native_) {
        native_->Destroy();
        native_.reset();
    }
}

bool MacroDebugController::IsCreated() const {
    if (webUi_) return webCreated_;
    return native_ && native_->IsCreated();
}

HWND MacroDebugController::Hwnd() const {
    if (webUi_ || !native_) return nullptr;
    return native_->Hwnd();
}

void MacroDebugController::FlushWebPendingLogs() {
    std::vector<std::wstring> batch;
    {
        std::lock_guard<std::mutex> lock(webLogMu_);
        batch.swap(webPendingLogs_);
        webLogFlushScheduled_.store(false, std::memory_order_relaxed);
    }
    if (batch.empty() || !webCreated_) return;
    if (batch.size() == 1) {
        PostJson(std::string("{\"type\":\"debugWindow.append\",\"ok\":true,\"text\":")
            + JsonStringW(batch[0]) + "}");
        return;
    }
    std::ostringstream oss;
    oss << "{\"type\":\"debugWindow.appendBatch\",\"ok\":true,\"lines\":[";
    for (size_t i = 0; i < batch.size(); ++i) {
        if (i) oss << ",";
        oss << JsonStringW(batch[i]);
    }
    oss << "]}";
    PostJson(oss.str());
}

namespace {
void CALLBACK MacroDebugWebLogFlushCb(PVOID param, BOOLEAN) {
    auto* self = static_cast<MacroDebugController*>(param);
    if (self) self->FlushWebPendingLogs();
}

void CapWebPendingLogs(std::vector<std::wstring>& pending) {
    constexpr size_t kMaxPending = 2000;
    if (pending.size() <= kMaxPending) return;
    pending.erase(pending.begin(),
        pending.begin() + static_cast<std::ptrdiff_t>(pending.size() - kMaxPending));
}

void ScheduleWebLogFlush(MacroDebugController* self, std::atomic<bool>& scheduled) {
    if (!self) return;
    if (scheduled.exchange(true, std::memory_order_acq_rel)) return;
    HANDLE timer = nullptr;
    if (!CreateTimerQueueTimer(&timer, nullptr, MacroDebugWebLogFlushCb, self, 32, 0,
            WT_EXECUTEDEFAULT | WT_EXECUTEONLYONCE)) {
        scheduled.store(false, std::memory_order_relaxed);
        self->FlushWebPendingLogs();
    }
}
}  // namespace

void MacroDebugController::AppendLog(const std::wstring& text) {
    if (webUi_) {
        if (!webCreated_) return;
        {
            std::lock_guard<std::mutex> lock(webLogMu_);
            webPendingLogs_.push_back(text);
            CapWebPendingLogs(webPendingLogs_);
        }
        ScheduleWebLogFlush(this, webLogFlushScheduled_);
        return;
    }
    if (native_) native_->AppendLog(text);
}

void MacroDebugController::AppendLogBatch(const std::vector<std::wstring>& lines) {
    if (webUi_) {
        if (!webCreated_ || lines.empty()) return;
        {
            std::lock_guard<std::mutex> lock(webLogMu_);
            webPendingLogs_.insert(webPendingLogs_.end(), lines.begin(), lines.end());
            CapWebPendingLogs(webPendingLogs_);
        }
        ScheduleWebLogFlush(this, webLogFlushScheduled_);
        return;
    }
    if (native_) native_->AppendLogBatch(lines);
}

void MacroDebugController::ClearLog() {
    if (webUi_) {
        if (!webCreated_) return;
        {
            std::lock_guard<std::mutex> lock(webLogMu_);
            webPendingLogs_.clear();
            webLogFlushScheduled_.store(false, std::memory_order_relaxed);
        }
        PostJson("{\"type\":\"debugWindow.clear\",\"ok\":true}");
        return;
    }
    if (native_) native_->ClearLog();
}

MacroDebugController& MacroDebug() {
    return g_macroDebug;
}

void DestroyMacroDebug() {
    g_macroDebug.Destroy();
}

}  // namespace qst::desktop_tools
