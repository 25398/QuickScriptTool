// ──────────────────────────────────────────────────────────────────
// player_main.cpp — 独立脚本播放器（导出 EXE 的运行端）
//
// 形态：`player.exe` 模板 + 尾部追加的脚本包（见 script_package.h 的 payload 说明）。
//
// 运行语义（2026-09-19 按用户要求定稿）：
//   双击即跑 → 跑完自己退出；被 F9 / 脚本自带热键停止也自己退出。
//   **不常驻托盘**、不留后台进程、不写注册表、不开机自启。
//   ⚠ 唯一的例外：**默认模式下的「中断脱离」**（用户动了鼠标/键盘 → 宏暂停、等脱离
//      时间到了再恢复执行）。那段期间 `IsRunning()` 仍为 true —— 见
//      `engine_script_run.cpp` 的 `waitBreakoutCooldown()`：它只把 `breakoutPaused_`
//      置真、在 worker 线程里等待，`running_` 不动。所以下面的等待循环天然会继续等，
//      不会误退出 —— 这正是"按原逻辑恢复执行"。
//
// 为什么先"自我迁移"到 %LOCALAPPDATA%\QstPlayer\rt\<hash>：
//   引擎的所有路径都从 AppDir() 派生（ScriptsDir() = AppDir()\scripts），
//   而假焦点注入器按 `ModuleDirectory()` 找 FakeFocus32/64.dll。
//   如果直接在用户放 exe 的地方解包，会：
//     ① 把 Desktop 弄出个 scripts\ 目录；
//     ② 同一目录下两个导出的 exe 互相覆盖；
//     ③ exe 放在只读目录（Program Files / 光盘）时直接失败。
//   迁到一个按 payload 哈希命名的目录，三个问题一次解决，而且同一 exe 反复运行
//   命中同一目录、不重复解包。
// ──────────────────────────────────────────────────────────────────

#include <windows.h>
#include <delayimp.h>   // PDelayLoadInfo / PfnDliHook（延迟加载失败钩子）

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "ai_decide.h"
#include "engine/engine_ui_hooks.h"
#include "engine/qst_engine.h"
#include "ocr_backend.h"
#include "opencv_runtime.h"
#include "player/player_runtime.h"
#include "script_package.h"
#include "utils.h"
#include "window_mode/ext_bridge/ext_bridge_server.h"
#include "window_mode/fake_focus/fake_focus_stage.h"   // 注入副本目录清扫

namespace {

constexpr wchar_t kWndClass[] = L"QstScriptPlayerWindow";
/// 脚本没配热键时的兜底停止键。**只在脚本确实没有热键时才用** ——
/// 用户明确要求不要写死一个固定热键，但完全没有逃生键更糟。
constexpr UINT kFallbackStopVk = VK_F9;
/// 长按判定：≥ 这个时长算「长按」（停止），否则算「单击」（暂停/继续）。
/// 与引擎既有的「长按=停止、单击=启动」习惯保持一致。
constexpr DWORD kLongPressMs = 600;
/// 启动后等这么久还没进入"运行中"就当它没跑起来（避免万一卡住时永不退出）
constexpr DWORD kStartGraceMs = 8000;

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
std::wstring g_runtimeDir;
std::wstring g_rootScriptPath;
/// 日志路径。启动早期就指向 %TEMP%，确定运行时目录后再改过去 ——
/// 这样"连运行时目录都没建起来"这种最需要日志的情况也有日志可看。
std::wstring g_logPath;
std::mutex g_logMu;

std::wstring JoinPath(const std::wstring& dir, const wchar_t* name) {
    if (dir.empty()) return name ? name : L"";
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += name ? name : L"";
    return out;
}

/// 归一化路径：正反斜杠统一 + 折叠重复分隔符 + GetFullPathNameW 展开。
/// 为什么必须做：环境变量（LOCALAPPDATA）完全可能是 `D://a//b` 或带尾斜杠，
/// 而 GetModuleFileNameW 返回的是规范形式。直接 _wcsicmp 会判定"不是同一个目录"
/// —— 那就成了**无限迁移循环**（每次重启都认为还需要迁移）。实测踩过。
std::wstring NormalizePath(const std::wstring& in) {
    if (in.empty()) return L"";
    std::wstring p;
    p.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t ch = in[i];
        if (ch == L'/') ch = L'\\';
        if (ch == L'\\' && !p.empty() && p.back() == L'\\') {
            // 盘符后的 "\" 要保留一个；其余重复的折叠
            if (!(p.size() == 2 && p[1] == L':')) continue;
        }
        p += ch;
    }
    wchar_t full[MAX_PATH * 2]{};
    if (GetFullPathNameW(p.c_str(), MAX_PATH * 2, full, nullptr) != 0) {
        p = full;
    }
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

bool SameDir(const std::wstring& a, const std::wstring& b) {
    const std::wstring na = NormalizePath(a);
    const std::wstring nb = NormalizePath(b);
    return !na.empty() && !nb.empty() && _wcsicmp(na.c_str(), nb.c_str()) == 0;
}

std::wstring TempDir() {
    wchar_t buf[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, buf) == 0) return L"";
    std::wstring out(buf);
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

/// 播放器日志：`<runtimeDir>\player.log`（启动早期落在 `%TEMP%\QstPlayer-boot.log`）。
/// 为什么一定要有：这是个没有界面的程序，出问题时用户唯一能给的东西就是它。
/// （"发我看看 player.log" 比"你描述一下现象"有用得多。）
void PlayerLog(const std::wstring& line) {
    if (g_logPath.empty()) return;
    std::lock_guard<std::mutex> lock(g_logMu);
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::string bytes = ToUtf8(std::wstring(prefix) + line + L"\r\n");
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD put = 0;
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr);
    CloseHandle(h);
}

void InfoBox(const std::wstring& text, const wchar_t* title, UINT flags = MB_OK | MB_ICONINFORMATION) {
    MessageBoxW(nullptr, text.c_str(), title, flags);
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return L"";
    std::wstring full(path);
    const auto slash = full.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"" : full.substr(0, slash);
}

std::wstring SelfExePath() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return L"";
    return path;
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsImageName(const std::wstring& name) {
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    return ext == L".bmp" || ext == L".png" || ext == L".jpg" || ext == L".jpeg";
}

/// 把 `scripts\` 里的图片挪进 `scripts\images\`。
/// 为什么需要：zip 里的图片条目是**扁平基名**（为了兼容旧的导入逻辑），
/// 而引擎的 `FindImagesDir()` 是 `ScriptsDir()\images` —— 不挪就找不到模板图。
void MoveImagesIntoPlace(const std::wstring& scriptsDir) {
    const std::wstring imgDir = JoinPath(scriptsDir, L"images");
    EnsureDirectoryTree(imgDir);

    auto sweep = [&](const std::wstring& from) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring name(fd.cFileName);
            if (!IsImageName(name)) continue;
            const std::wstring src = JoinPath(from, name.c_str());
            const std::wstring dst = JoinPath(imgDir, name.c_str());
            if (FileExists(dst)) DeleteFileW(dst.c_str());
            MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    sweep(scriptsDir);
    sweep(JoinPath(scriptsDir, L"scripts"));
}

/// 把 from 目录里的**文件**搬到 to 目录（不递归子目录；已存在的不覆盖）。
/// 不覆盖是关键：运行时目录根可能已经有被本进程加载的 DLL，覆盖会失败。
void MoveFilesInto(const std::wstring& from, const std::wstring& to) {
    if (from.empty() || to.empty()) return;
    EnsureDirectoryTree(to);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name(fd.cFileName);
        const std::wstring src = JoinPath(from, name.c_str());
        const std::wstring dst = JoinPath(to, name.c_str());
        if (FileExists(dst)) continue;
        MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/// 按前缀分发暂存目录的内容：
///   `rt\*`       → 运行时目录**根**（假焦点注入器按 ModuleDirectory() 找 DLL；
///                  OpenCV 也从 exe 旁边加载）
///   其余（script.json / scripts\* / 图片 / package.json）→ `scripts\`
/// 为什么必须走暂存：直接把 zip 解到 `scripts\` 的话，`rt\` 前缀会变成
/// `scripts\rt\`，上面两类都找不到组件。
void DistributeStage(const std::wstring& stageDir, const std::wstring& runtimeDir,
    const std::wstring& scriptsDir) {
    MoveFilesInto(JoinPath(stageDir, L"rt"), runtimeDir);
    MoveFilesInto(stageDir, scriptsDir);
    // scripts\ 子目录（嵌套脚本）
    const std::wstring nestedFrom = JoinPath(stageDir, L"scripts");
    const std::wstring nestedTo = JoinPath(scriptsDir, L"scripts");
    EnsureDirectoryTree(nestedTo);
    MoveFilesInto(nestedFrom, nestedTo);
    // 图片统一进 scripts 下的 images 目录
    MoveImagesIntoPlace(scriptsDir);
}

/// 清理暂存目录（含子目录）。
void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0
                || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            const std::wstring child = JoinPath(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTree(child);
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

/// 解包 payload 到 runtimeDir（脚本进 scripts\，运行时组件进根）。已解过就跳过。
bool EnsureExtracted(const std::wstring& selfPath, const std::wstring& runtimeDir,
    std::wstring& err) {
    const std::wstring scriptsDir = JoinPath(runtimeDir, L"scripts");
    const std::wstring rootScript = JoinPath(scriptsDir, L"script.json");
    if (FileExists(rootScript)) {
        // 已解过：兼容早期版本把 rt\ 组件落在 scripts\rt\ 的情况，补搬一次
        MoveFilesInto(JoinPath(scriptsDir, L"rt"), runtimeDir);
        RemoveTree(JoinPath(scriptsDir, L"rt"));
        return true;
    }

    if (!EnsureDirectoryTree(scriptsDir)) {
        err = L"无法创建运行时目录：\n" + scriptsDir
            + L"\n\n请检查磁盘空间与杀毒软件设置。";
        return false;
    }

    const std::wstring zipPath = JoinPath(runtimeDir, L"payload.zip");
    const std::wstring stageDir = JoinPath(runtimeDir, L"_stage");
    if (!scriptpkg::ExtractEmbeddedPayload(selfPath, zipPath, nullptr)) {
        err = L"这个文件里没有找到脚本数据。\n\n"
              L"可能是复制/传输过程中损坏了，或者它本来就是「键鼠工坊」主程序而不是导出的脚本。";
        return false;
    }
    EnsureDirectoryTree(stageDir);
    const int n = ExtractZipFile(zipPath, stageDir);
    DeleteFileW(zipPath.c_str());
    if (n <= 0) {
        RemoveTree(stageDir);
        err = L"脚本数据解包失败（可能被杀毒软件拦截）。";
        return false;
    }

    DistributeStage(stageDir, runtimeDir, scriptsDir);
    RemoveTree(stageDir);

    if (!FileExists(rootScript)) {
        err = L"脚本数据里缺少主脚本（script.json）。";
        return false;
    }
    return true;
}

/// 迁移到运行时目录并重新拉起自己。返回 true 表示"已交接，当前进程应退出"。
bool RelocateAndRelaunch(const std::wstring& runtimeDir, const std::wstring& selfPath,
    std::wstring& err) {
    if (!EnsureDirectoryTree(runtimeDir)) {
        err = L"无法创建运行时目录：\n" + runtimeDir;
        return false;
    }

    const std::wstring target = JoinPath(runtimeDir, L"QstPlayer.exe");
    if (!FileExists(target)) {
        if (!CopyFileW(selfPath.c_str(), target.c_str(), FALSE)) {
            err = L"无法把播放器复制到运行时目录：\n" + runtimeDir
                + L"\n\n请检查该目录是否可写，或换一台电脑试试。";
            return false;
        }
    }
    if (!EnsureExtracted(selfPath, runtimeDir, err)) return false;

    // 循环断路：给子进程带一个标记，让它**永不**再迁移。
    // 即使路径比较因为某种环境差异判错，也不会无限重启。
    std::wstring cmd = L"\"" + target + L"\" --qst-no-relocate";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // ⚠ 必须重试：target 是刚 CopyFileW 出来的，杀软扫描期间 CreateProcess
    //   会报 ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED（实测首次运行必失败）。
    BOOL launched = FALSE;
    DWORD launchErr = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        launched = CreateProcessW(target.c_str(), cmd.data(), nullptr, nullptr, FALSE,
            0, nullptr, runtimeDir.c_str(), &si, &pi);
        if (launched) break;
        launchErr = GetLastError();
        if (launchErr != ERROR_SHARING_VIOLATION && launchErr != ERROR_ACCESS_DENIED) break;
        Sleep(500);
    }
    if (!launched) {
        err = L"无法启动播放器副本（错误 " + std::to_wstring(launchErr) + L"）：\n" + target
            + L"\n\n多半是杀毒软件正在扫描刚生成的文件，稍等几秒再双击一次即可。";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ENDSESSION:
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterPlayerClass(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

/// 脚本热键绑定（读自脚本 JSON 的 hotkeyVk / hotkeyModifiers）。
struct HotkeyBinding {
    UINT vk = 0;
    UINT mods = 0;
    bool fallback = false;  // true = 脚本没配热键，用的是兜底键
    bool Valid() const { return vk != 0; }
};

/// 修饰键是否按下。编码与 Win32 RegisterHotKey 的 MOD_* 一致
/// （引擎就是用同一套值注册脚本热键的）。
bool ModifiersDown(UINT mods) {
    if ((mods & MOD_CONTROL) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
    if ((mods & MOD_ALT) && !(GetAsyncKeyState(VK_MENU) & 0x8000)) return false;
    if ((mods & MOD_SHIFT) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) return false;
    if ((mods & MOD_WIN)
        && !(GetAsyncKeyState(VK_LWIN) & 0x8000)
        && !(GetAsyncKeyState(VK_RWIN) & 0x8000)) return false;
    return true;
}

bool HotkeyDown(const HotkeyBinding& b) {
    if (!b.Valid()) return false;
    return (GetAsyncKeyState(static_cast<int>(b.vk)) & 0x8000) != 0 && ModifiersDown(b.mods);
}

/// 把 JSON 里某个数值字段改成 0（只认第一层出现的那个键名，够用）。
void ZeroJsonNumberField(std::wstring& json, const std::wstring& key) {
    const std::wstring needle = L"\"" + key + L"\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::wstring::npos) {
        const auto colon = json.find(L':', pos + needle.size());
        if (colon == std::wstring::npos) break;
        size_t i = colon + 1;
        while (i < json.size() && (json[i] == L' ' || json[i] == L'\t')) ++i;
        size_t j = i;
        while (j < json.size() && (iswdigit(json[j]) || json[j] == L'-' || json[j] == L'.')) ++j;
        if (j > i) json.replace(i, j - i, L"0");
        pos = colon + 1;
    }
}

/// 抹掉脚本里的热键字段。
/// **为什么必须做**：引擎加载脚本时会自己把「脚本热键」注册成启动该宏的热键，
/// 而运行中按它走的是引擎的**停止**分支 —— 会抢在播放器的「单击/长按」判定之前
/// 把脚本停掉（实测：单击一下脚本直接结束，暂停根本来不及）。
/// 播放器要独占这个键（单击=暂停/继续，长按=停止），所以先让引擎看不见它。
void StripScriptHotkey(const std::wstring& scriptPath) {
    std::wstring json = ReadAll(scriptPath);
    if (json.empty()) return;
    ZeroJsonNumberField(json, L"hotkeyVk");
    ZeroJsonNumberField(json, L"hotkeyModifiers");
    ZeroJsonNumberField(json, L"hotkeyHold");
    std::ofstream out(scriptPath, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write("\xEF\xBB\xBF", 3);
    const std::string bytes = ToUtf8(json);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// 从脚本 JSON 读热键。字段与 `SetScriptHotkeyJson` / `SaveScriptFileData` 写的一致。
HotkeyBinding ReadScriptHotkey(const std::wstring& scriptJson) {
    HotkeyBinding b;
    if (scriptJson.empty()) return b;
    b.vk = static_cast<UINT>(ExtractNumber(scriptJson, L"hotkeyVk", 0));
    b.mods = static_cast<UINT>(ExtractNumber(scriptJson, L"hotkeyModifiers", 0));
    return b;
}

/// 暂停 / 继续。**判据只能是 `IsPlaybackPaused()`**，不要自己维护一份状态 ——
/// 引擎在脚本开始/结束时会把该标志复位，自己存一份就会和引擎不同步。
void TogglePlaybackPause() {
    const bool paused = !qst::engine::IsPlaybackPaused();
    qst::engine::SetPlaybackPaused(paused);
    if (paused) {
        PlayAppPauseSound();
        PlayerLog(L"已暂停（再按一次热键继续；长按热键则停止）");
    } else {
        PlayAppStartupSound();
        PlayerLog(L"已继续");
    }
}

/// 抽干消息队列。
void DrainMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/// 等到脚本结束。
/// 语义（**别改**）：`IsRunning()` 为真就一直等 —— 包括**默认模式下的中断脱离**
/// （那时 `breakoutPaused_=true` 但 `running_` 仍为 true），脱离结束后引擎自己恢复执行。
/// 只有真的跑完 / 被停止（`running_` 转 false）才返回 → 播放器随之退出。
void WaitUntilFinished(const HotkeyBinding& hotkey) {
    const DWORD t0 = GetTickCount();
    bool sawRunning = false;
    DWORD lastBeat = t0;
    bool keyWasDown = false;
    bool keyConsumedByLongPress = false;
    DWORD keyDownAt = 0;

    PlayerLog(L"进入等待循环");
    if (hotkey.Valid()) {
        PlayerLog(hotkey.fallback
            ? (L"脚本没配热键，使用兜底停止键 F9（单击=暂停/继续，长按=停止）")
            : (L"脚本热键已生效（单击=暂停/继续，长按=停止）"));
    } else {
        PlayerLog(L"没有可用的控制热键");
    }

    for (;;) {
        // ── 热键：单击=暂停/继续，长按=停止 ──
        // 用轮询而不是 RegisterHotKey：① 需要区分单击/长按，而 WM_HOTKEY 不给抬起事件；
        // ② 引擎自己也会注册脚本热键，轮询不与它抢注册。
        if (hotkey.Valid()) {
            const bool down = HotkeyDown(hotkey);
            const DWORD held = down ? (GetTickCount() - keyDownAt) : 0;
            if (down && !keyWasDown) {
                keyDownAt = GetTickCount();
                keyConsumedByLongPress = false;
            } else if (down && !keyConsumedByLongPress && held >= kLongPressMs) {
                PlayerLog(L"热键长按：停止脚本");
                keyConsumedByLongPress = true;
                qst::engine::StopScript();
            } else if (!down && keyWasDown) {
                if (!keyConsumedByLongPress && held < kLongPressMs) {
                    TogglePlaybackPause();
                }
                keyConsumedByLongPress = false;
            }
            keyWasDown = down;
        }

        if (GetTickCount() - lastBeat > 30000) {   // 30s 一次，长跑脚本也能在日志里看出还活着   // 30s 一次，长跑脚本也能在日志里看出还活着
            lastBeat = GetTickCount();
            PlayerLog(std::wstring(L"心跳：IsRunning=")
                + (qst::engine::IsRunning() ? L"1" : L"0")
                + (qst::engine::IsPlaybackPaused() ? L"（已暂停）" : L""));
        }

        if (qst::engine::IsRunning()) {
            sawRunning = true;
        } else if (sawRunning) {
            PlayerLog(L"脚本已结束（跑完或已停止），播放器退出");
            return;
        } else if (GetTickCount() - t0 > kStartGraceMs) {
            PlayerLog(L"等待超时：脚本始终没有进入运行状态");
            return;
        }
        // 用 MsgWait 而不是 Sleep：既让出 CPU，又保证窗口消息能被派发
        MsgWaitForMultipleObjectsEx(0, nullptr, 30, QS_ALLINPUT, 0);
        DrainMessages();
    }
}

}  // namespace

// delayimp 需要**全局可见**的 hook 符号（不能放进匿名命名空间）。
//
// 为什么播放器必须有这个钩子：exe 是用 `/DELAYLOAD:opencv_world4100.dll` 链 OpenCV 的。
// 缺 DLL 时 delay-load 助手默认会抛 `0xC06D007E` —— **进程当场死**，
// 没有日志、没有结束音、用户完全不知道发生了什么（实测踩过）。
// 装上钩子至少能把失败原因写进 player.log；真正的优雅降级靠调用点的
// `OpenCvAvailable()` 守卫（见 LoadBitmapFromFile / resolveOcrRegion）。
FARPROC WINAPI QstPlayerDelayLoadFailureHook(unsigned dliNotify, PDelayLoadInfo pdli) {
    if (dliNotify == dliFailLoadLib || dliNotify == dliFailGetProc) {
        const char* name = (pdli && pdli->szDll) ? pdli->szDll : "dependency";
        const std::string text(name ? name : "dependency");
        if (text.find("opencv") != std::string::npos
            || text.find("OpenCV") != std::string::npos) {
            MarkOpenCvUnavailable(L"延迟加载 opencv_world4100.dll 失败");
            PlayerLog(L"延迟加载 opencv_world4100.dll 失败：找图/按图取区域将不可用");
        } else {
            PlayerLog(L"延迟加载失败：" + FromUtf8(text));
        }
    }
    return nullptr;
}

extern "C" const PfnDliHook __pfnDliFailureHook2 = QstPlayerDelayLoadFailureHook;

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;
    // 启动早期就把日志指向 %TEMP%：万一连运行时目录都建不起来，至少还有这份日志
    g_logPath = JoinPath(TempDir(), L"QstPlayer-boot.log");
    PlayerLog(L"=== 脚本播放器启动 ===");

    bool noRelocate = false;
    {
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i) {
                if (lstrcmpiW(argv[i], L"--qst-no-relocate") == 0) noRelocate = true;
            }
            LocalFree(argv);
        }
    }

    // 被浏览器当成「扩展的原生消息宿主」拉起时必须**立刻退出**。
    // 浏览器给宿主的调用形如：`QstPlayer.exe chrome-extension://<id>/ --parent-window=0`，
    // 且 stdin/stdout 是管道。播放器不提供这个能力；不早退的话会被反复拉起 + 每次
    // 都去解析 payload / 迁移目录 → 进程风暴（实测踩过，几十个实例）。
    {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        const bool piped = in && out && in != INVALID_HANDLE_VALUE
            && out != INVALID_HANDLE_VALUE
            && GetFileType(in) == FILE_TYPE_PIPE
            && GetFileType(out) == FILE_TYPE_PIPE;
        bool extFlag = false;
        int argc2 = 0;
        if (LPWSTR* argv2 = CommandLineToArgvW(GetCommandLineW(), &argc2)) {
            for (int i = 1; i < argc2; ++i) {
                if (lstrcmpiW(argv2[i], L"--ext-native-host") == 0) extFlag = true;
            }
            LocalFree(argv2);
        }
        if (piped || extFlag) return 0;
    }

    const std::wstring selfPath = SelfExePath();
    PlayerLog(L"自身路径：" + selfPath);

    // 1) 读尾部 payload，算出运行时目录
    scriptpkg::PayloadInfo info;
    if (!scriptpkg::ReadPayloadInfo(selfPath, info)) {
        PlayerLog(L"尾部没有 payload —— 这不是导出的脚本 exe");
        InfoBox(L"这个文件里没有找到脚本数据。\n\n"
                L"如果你拿到的是「键鼠工坊」主程序，请直接运行它。",
            L"脚本播放器", MB_OK | MB_ICONERROR);
        return 1;
    }
    PlayerLog(L"payload: offset=" + std::to_wstring(info.offset) + L" size="
        + std::to_wstring(info.size) + L" hash=" + std::to_wstring(info.hash));
    g_runtimeDir = NormalizePath(playerruntime::RuntimeDirFor(info.hash));
    if (!g_runtimeDir.empty()) {
        if (!EnsureDirectoryTree(g_runtimeDir)) {
            PlayerLog(L"运行时目录创建失败：" + g_runtimeDir);
        } else {
            g_logPath = JoinPath(g_runtimeDir, L"player.log");
        }
    }
    if (g_runtimeDir.empty()) {
        InfoBox(L"无法确定运行时目录（LOCALAPPDATA 不可用）。", L"脚本播放器",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    // 2) 还没在运行时目录里 → 迁移并交接
    if (!noRelocate && !SameDir(ExeDir(), g_runtimeDir)) {
        PlayerLog(L"需要迁移到运行时目录：" + g_runtimeDir);
        std::wstring err;
        if (!RelocateAndRelaunch(g_runtimeDir, selfPath, err)) {
            PlayerLog(L"迁移失败：" + err);
            InfoBox(err, L"脚本播放器", MB_OK | MB_ICONERROR);
            return 1;
        }
        return 0;
    }

    // 3) 单实例（**不能复用壳的 KeyMouse_SingleInstance**，否则会和已安装的软件互踢）
    wchar_t mutexName[128]{};
    swprintf_s(mutexName, L"QstScriptPlayer_%llx",
        static_cast<unsigned long long>(info.hash));
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        PlayerLog(L"已有实例在运行，本次直接退出");
        return 0;
    }

    // 4) 读清单 → 准备依赖
    scriptpkg::PlayerManifest manifest;
    {
        const std::wstring manifestPath = JoinPath(JoinPath(g_runtimeDir, L"scripts"),
            L"package.json");
        const std::wstring raw = ReadAll(manifestPath);
        if (!raw.empty()) scriptpkg::ParsePlayerManifest(raw, manifest);
    }
    playerruntime::ResolveInput rin;
    rin.manifest = manifest;
    rin.bundledDir = g_runtimeDir;
    rin.installedAppDir = playerruntime::FindInstalledAppDir();
    const playerruntime::ResolveOutput resolved = playerruntime::ResolveRuntime(rin);
    if (!resolved.ok) {
        PlayerLog(L"依赖解析失败：" + resolved.fatalError);
        InfoBox(resolved.fatalError, L"脚本播放器", MB_OK | MB_ICONERROR);
        if (mutex) CloseHandle(mutex);
        return 1;
    }
    // OCR 后端按导出模式定：自带 = 系统 WinRT OCR（零安装）；
    // 走软件 = 用已装软件的 Python 环境，失败再回退系统 OCR。
    if (resolved.ocr == playerruntime::ComponentSource::SystemOcr) {
        SetOcrBackendPreference(OcrBackend::WinRt);
        std::wstring why;
        if (!WinRtOcrAvailable(&why)) {
            PlayerLog(L"系统 OCR 不可用：" + why);
        }
    } else if (resolved.ocr == playerruntime::ComponentSource::InstalledApp) {
        SetOcrBackendPreference(OcrBackend::Auto);
    }
    PlayerLog(L"运行时目录：" + g_runtimeDir);
    PlayerLog(std::wstring(L"找图组件=") + playerruntime::ComponentSourceName(resolved.openCv)
        + L"；OCR=" + playerruntime::ComponentSourceName(resolved.ocr)
        + (resolved.openCvPath.empty() ? L"" : (L"；OpenCV=" + resolved.openCvPath)));
    for (const auto& w : resolved.warnings) PlayerLog(L"提示：" + w);
    // 假焦点「注入副本」目录与产品壳共用（同一份构建同一份副本）：按年龄回收，
    // 被进程映射的旧副本删不掉就跳过。播放器不常驻，所以这里是它唯一的清理点。
    {
        const windowmode::FakeFocusSweepResult sw =
            windowmode::SweepStaleFakeFocusArtifacts(g_runtimeDir,
                windowmode::kFakeFocusStageKeepDays);
        PlayerLog(L"假焦点副本清扫：旧目录=" + std::to_wstring(sw.stageDirsRemoved)
            + L" 占用跳过=" + std::to_wstring(sw.stageKeptLocked)
            + L" 让位残留=" + std::to_wstring(sw.leftoversRemoved));
    }

    // 首次运行写一份 README（**不弹窗**：用户要的是"双击即跑"）。
    // 说明落盘而不是弹框，用户随时能在运行时目录里翻到。
    const std::wstring noticeMarker = JoinPath(g_runtimeDir, L"notice_shown");
    if (!FileExists(noticeMarker)) {
        std::wstring text =
            L"这是一个由「键鼠工坊」导出的脚本播放器。\r\n\r\n"
            L"· 它会模拟键盘和鼠标操作，请先确认当前屏幕上的操作不会被误触；\r\n"
            L"· 随时按 F9 停止；如果脚本自己配了热键，按那个键也可以停止；\r\n"
            L"· 脚本跑完（或被你停止）后本程序会自动退出，不会留在后台；\r\n"
            L"· 本程序不写注册表、不开机自启、不常驻托盘，删除文件即彻底移除；\r\n"
            L"· 运行日志在同目录的 player.log，遇到问题可以把它发给脚本作者。\r\n";
        text += L"\r\n运行模式：\r\n";
        text += std::wstring(L"  图像识别（找图/找色）：")
            + playerruntime::ComponentSourceName(resolved.openCv) + L"\r\n";
        text += std::wstring(L"  文字识别（OCR）：")
            + playerruntime::ComponentSourceName(resolved.ocr) + L"\r\n";
        for (const auto& w : resolved.warnings) text += L"\r\n提示：" + w + L"\r\n";
        if (!resolved.openCvPath.empty()) {
            text += L"\r\n图像识别组件：" + resolved.openCvPath + L"\r\n";
        }
        {
            std::ofstream out(JoinPath(g_runtimeDir, L"README.txt"),
                std::ios::binary | std::ios::trunc);
            if (out) {
                out.write("\xEF\xBB\xBF", 3);
                const std::string bytes = ToUtf8(text);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
        }
        HANDLE h = CreateFileW(noticeMarker.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    // 4.35) OCR 诊断接到 player.log：OCR 类脚本"不生效"时，
    //   最难判断的就是**到底有没有识别到、识别成了什么** —— 这行日志是唯一线索。
    SetOcrDiagnosticSink(+[](const std::wstring& line) { PlayerLog(line); });

    // 4.36) 本地判断表的诊断也接到 player.log。
    //   为什么必须有这一条：AI 动作执行里那些「视觉闸为什么拦下」「算不算游戏前台」
    //   的判断，原先只写进宏调试窗（`MacroDebug().AppendLog`，窗口没建就直接丢弃）——
    //   播放器是无界面程序，等于**这些判断在导出的 exe 里完全没有痕迹**。
    //   接上之后：① 用户报障「游戏没反应」时有据可查；② 离线影子测试
    //   （tools/verify/ai_decide_shadow.py）才有输入数据 —— 判断行带输入信号，
    //   可以用来复算和标定置信度，进而决定要不要接本地小模型做判断后端。
    SetAiDecisionLogSink(+[](const std::wstring& line) { PlayerLog(line); });

    // 4.4) 先探测 OpenCV 并把状态定下来（Ready / Missing）。
    //   为什么必须在跑脚本之前：`OpenCvAvailable()` 在 Unknown 时会去 LoadLibrary，
    //   虽然本身安全，但**脚本执行期**才有调用点的话，一旦某个调用点漏了守卫，
    //   delay-load 桩就会在没准备的情况下抛异常把进程带走。
    //   启动期探一次、写进日志，后面所有守卫都拿到确定状态。
    {
        std::wstring cvReason;
        if (TryInitOpenCv(&cvReason)) {
            PlayerLog(L"OpenCV 已就绪（找图可用）");
        } else {
            PlayerLog(L"OpenCV 不可用：" + cvReason);
        }
    }

    // 4.5) 控制热键：**必须在 engine::Start() 之前**读并抹掉。
    //   两个都踩过：
    //   ① 引擎 Start() 时会扫描脚本目录，把「脚本热键」注册成**回放钩子**
    //      （engine_host_window.h 的 ghPlaybackScriptHooks）——运行中按它走的是
    //      **紧急停止**分支，会抢在播放器的「单击/长按」判定之前把脚本停掉；
    //   ② 引擎加载脚本时还会把 script.json 规范化重写（补 coordMeta / 动作默认字段），
    //      那一步会把 hotkeyVk 清成 0，读晚了就什么也读不到。
    //   所以：先读出来自己用，再抹掉让引擎彻底看不见。
    g_rootScriptPath = JoinPath(JoinPath(g_runtimeDir, L"scripts"), L"script.json");
    HotkeyBinding hotkey = ReadScriptHotkey(ReadAll(g_rootScriptPath));
    if (hotkey.Valid()) {
        StripScriptHotkey(g_rootScriptPath);
    } else {
        hotkey.vk = kFallbackStopVk;
        hotkey.mods = 0;
        hotkey.fallback = true;
    }

    // 5) 起引擎
    // 播放器**不是**主程序：关掉「把自己注册成浏览器扩展原生宿主」。
    // 否则会写 HKCU 注册表（违反"产物不写注册表"），并被浏览器反复拉起。
    windowmode::SetExtNativeHostAllowed(false);
    if (!qst::engine::Start(inst)) {
        PlayerLog(L"引擎启动失败");
        InfoBox(L"播放器初始化失败。", L"脚本播放器", MB_OK | MB_ICONERROR);
        if (mutex) CloseHandle(mutex);
        return 1;
    }
    qst::engine::SetUiHost(nullptr);

    // 引擎的 UI 出口全部转发到 player.log —— 这是无界面程序唯一的可诊断面。
    {
        qst::engine::UiBridgeHooks hooks;
        hooks.postToWebUi = [](std::string jsonUtf8) {
            PlayerLog(L"engine: " + FromUtf8(jsonUtf8));
        };
        hooks.hotkeyLogLine = [](const std::string& line) {
            PlayerLog(L"hotkey: " + FromUtf8(line));
        };
        qst::engine::SetUiBridgeHooks(std::move(hooks));
    }

    if (!RegisterPlayerClass(inst)) {
        PlayerLog(L"窗口类注册失败");
        InfoBox(L"窗口类注册失败。", L"脚本播放器", MB_OK | MB_ICONERROR);
        if (mutex) CloseHandle(mutex);
        return 1;
    }
    // 隐藏消息窗：只为收 WM_HOTKEY（RegisterHotKey 需要一个窗口）。
    // 刻意做成 TOOLWINDOW + 0 尺寸 —— 播放器不该出现在任务栏 / Alt-Tab 里。
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWndClass, L"脚本播放器",
        0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) {
        PlayerLog(L"消息窗创建失败");
        InfoBox(L"播放器初始化失败（消息窗）。", L"脚本播放器", MB_OK | MB_ICONERROR);
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    // 6) 跑脚本
    {
        std::string err;
        if (!qst::engine::RunScriptPath(g_rootScriptPath, err)) {
            const std::wstring detail = err.empty() ? L"未知原因" : FromUtf8(err);
            PlayerLog(L"脚本启动失败：" + detail);
            InfoBox(L"脚本没能开始运行。\n\n原因：" + detail
                + L"\n\n如果是「窗口/后台窗口模式未就绪」，请在「键鼠工坊」里把该脚本的"
                  L"窗口选择方式改为「指定窗口类」后重新导出。",
                L"脚本播放器", MB_OK | MB_ICONWARNING);
            if (mutex) CloseHandle(mutex);
            return 1;
        }
        PlayerLog(L"脚本已开始运行：" + g_rootScriptPath);
    }

    // 7) 等它跑完（含"中断脱离"期间继续等待），然后自己退出
    WaitUntilFinished(hotkey);

    // 结束音是 SND_ASYNC 播的（引擎在 worker 收尾时放）。进程若立刻退出，
    // 声音会被硬截断 —— 实测"只响了一半"，听感很怪。等它放完再退。
    {
        const DWORD ms = WavDurationMs(AppFinishSoundFilePath());
        // 拿不到时长（比如回退成了系统提示音）就给个短缓冲
        const DWORD waitMs = (ms > 0) ? (ms + 150) : 400;
        Sleep(waitMs > 3000 ? 3000 : waitMs);
    }

    if (g_hwnd) DestroyWindow(g_hwnd);
    if (mutex) CloseHandle(mutex);
    // 刻意不调 qst::engine::Shutdown()：headless 窗 WM_DESTROY 会 TerminateProcess，
    // 进程退出时由 OS 回收即可（ScriptRunnerSelfTest --engine 里踩过这个坑）。
    return 0;
}
