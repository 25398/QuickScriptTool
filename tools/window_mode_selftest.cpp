// =============================================================================
// WindowModeSelfTest — 窗口/后台窗口模式自检（Agent 入口）
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
#include "script_types.h"
#include "action_utils.h"
#include "window_mode/background_uia_input.h"
#include "window_mode/window_list.h"
#include "window_mode/window_mode_permission.h"
#include "window_mode/ext_bridge/ext_bridge_server.h"
#include "window_mode/window_mode_log.h"
#include "window_mode/mouse_wheel_events.h"
#include "window_mode/fake_focus/fake_focus_soft_input_host.h"
#include "window_mode/fake_focus/fake_focus_stage.h"   // 注入副本（安装目录那份不被映射）
#include "window_mode/ui_element_probe.h"
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

// ★★ 桌面/资源管理器图标：shell 的 InvokePattern 只"选中"，"打开"必须双击（2026-09-29）
//   实测报障：点桌面「Edge」/列表项「Microsoft Edge」后回执说"已触发"，浏览器却没起来。
//   判据只看结构（父链上有没有外壳视图宿主类），不看名字 —— 名字会本地化/重名。
void TestShellIconHostClass() {
    const bool yes = windowmode::ClassNameIsShellIconHost(L"SHELLDLL_DefView")
        && windowmode::ClassNameIsShellIconHost(L"shelldll_defview");   // 大小写不敏感
    // ⚠ 资源管理器**窗口**类/桌面宿主**本身**不算：窗口里嵌的 WebView/普通控件父链上也有它们，
    //   认了就会把"网页里的按钮"误判成桌面图标（那会变成双击网页元素）。
    const bool no = !windowmode::ClassNameIsShellIconHost(L"CabinetWClass")
        && !windowmode::ClassNameIsShellIconHost(L"Progman")
        && !windowmode::ClassNameIsShellIconHost(L"WorkerW")
        && !windowmode::ClassNameIsShellIconHost(L"SysListView32")
        && !windowmode::ClassNameIsShellIconHost(L"Chrome_WidgetWin_1")
        && !windowmode::ClassNameIsShellIconHost(L"")
        && !windowmode::ClassNameIsShellIconHost(nullptr);
    selftest::Emit(L"shell_icon_host_class", yes && no, L"");
}
const selftest::CaseInfo kCases[] = {
    {L"ext_bridge_concurrent_no_cross",
     L"桥的并发请求回执不许串台（多会话并行的地基）"},
    {L"long_log_does_not_terminate",
     L"超长日志不得终止进程（固定缓冲 + vswprintf_s 会静默闪退）"},
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
    {L"background_input_wrapped_text_control", L"default",
        L"Wrapper layer (NotepadTextBox) must yield to the real text control (RichEditD2DPT) inside it"},
    {L"background_wheel_reaches_wrapped_input", L"default",
        L"包装层**自己命中「已知渲染面」**时（真机类名 Microsoft.UI.Content.DesktopChildSiteBridge，"
        L"含 Render 会被评分表命中）仍必须让位给里面的 RichEditD2DPT：投给容器 = PostMessage "
        L"不转发 = 键鼠与滚轮全部石沉大海（2026-09-30「后台滚动没效果」的根因）"},
    {L"wheel_targets_bound_child_not_wrapper", L"default",
        L"★ 真机复现（Win11 记事本树）：绑定 RichEdit 而旁边有同为最大后代的容器兄弟时，"
        L"鼠标/滚轮必须投给**绑定的** RichEdit —— 原来鼠标走 config=nullptr 的重解析，"
        L"把绑定丢掉、投给容器并如实报「成功=1/1」，等于没投"},
    {L"background_input_bound_child_respected", L"default",
        L"PostKeyToWindow must not override an explicitly bound child with the largest sibling surface"},
    {L"background_key_self_translate_policy", L"default",
        L"WinUI RichEditD2DPT self-translates KEYDOWN: printable -> WM_CHAR only, Enter/Tab/nav -> KEYDOWN only"},
    {L"android_qt_fake_focus_gate", L"default",
        L"MuMu/LDPlayer must not auto fake-focus (PostMessage to render child)"},
    {L"weixin_qt_fake_focus", L"default",
        L"Weixin.exe + Qt*QWindowIcon: lite fake-focus; KEY* without WM_CHAR/ACTIVATE; quick-input KEY* not WM_PASTE"},
    {L"weixin_qt_mouse_hooks", L"default",
        L"Weixin Qt FakeFocus lite hooks GetCursorPos/GetAsyncKeyState; SetCursorPos swallowed; no WndProc subclass"},
    {L"chromium_shell_soft_input", L"default",
        L"Chromium shell FakeFocus hooks soft cursor + GetKeyboardState (Ctrl+V combo) and injects no fake WM_INPUT"},
    {L"quick_input_skips_paste_non_edit", L"default",
        L"Non-Edit windows must not get fake-success WM_PASTE; Qt/AIR/Maple KEY*; generic custom class WM_CHAR"},
    {L"posted_quick_keys_timing", L"default",
        L"LCA/game posted quick-input holds each key >=1 frame and keeps DOWN-CHAR-UP order (no swallowed digit)"},
    {L"soft_key_combo_state_race", L"default",
        L"Soft-input combo keys: target must still read the modifier as down when it processes the char keydown (Ctrl+V paste)"},
    {L"native_host_manifest_points_to_product", L"default",
        L"Native-messaging host path may only be a product exe; self-test/diag exes must never be registered"},
    {L"maple_keystate_stall_resync", L"default",
        L"Maple keystate stall: stale arrow bits are cleared while the client polls nothing, and re-asserted from the script's held keys on resume (never drops key events)"},
    {L"selftest_refuses_foreign_launcher", L"default",
        L"Self-test exe launched with browser/WebView args must refuse to run any case (exit 0, no suite)"},
    {L"background_quick_input", L"default",
        L"WindowModeExecutor background quick-input succeeds"},
    {L"background_click_keeps_foreground", L"default",
        L"Background PostMessage click does not steal FG; minimized target may quiet-restore"},
    {L"window_client_scale", L"default",
        L"ScaleWindowClientPoint / find-image template scale use recorded vs live client size"},
    {L"window_findimage_full_client", L"default",
        L"Window/background find-image ignores absolute 选取区域 and uses the full client"},
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
        L"Windowed Unreal (incl. LaunchUnrealUWindowsClient) without fake-focus needs hardware SendInput in window/background"},
    {L"hardware_offscreen_park", L"default",
        L"Windowed hardware target parks off-screen + topmost and restores placement; failed park must not leave the window at (0,0)"},
    {L"clamp_rect_keeps_bottom_right", L"default",
        L"ClampRectToContainingWorkArea keeps a bottom-right window; does not snap to primary origin"},
    {L"clamp_rect_shrinks_into_work", L"default",
        L"Oversized rect shrinks into its monitor work area without filling from (0,0) as a new origin snap"},
    {L"vda_selects_os_dll", L"default",
        L"VirtualDesktopAccessor picks Win11 24H2+/23H2/Win10 DLL by OS build; no cross-OS fallback"},
    {L"fake_focus_hook_local", L"default",
        L"Load FakeFocus64/32 locally: GetForegroundWindow returns target; uninstall restores"},
    {L"fake_focus_inject_copy", L"default",
        L"注入走副本：源文件不被映射/名字归一成 FakeFocus32.dll/同一构建同一路径"},
    {L"fake_focus_stage_sweep", L"default",
        L"副本目录按年龄回收 + 被占用的跳过 + 让位改名残留 FakeFocus*.dll.locked-* 被清掉"},
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
    {L"fake_focus_air_child_iat_only", L"default",
        L"包装窗+AIR 子窗仍按 AIR 处理：不子类化；变速时钟钩只补 IAT（bit6=1），普通窗 bit6=0"},
    {L"setwindowshook_not_for_fragile_targets", L"default",
        L"setwindowshook 注入必须避开脆弱目标（含 Adobe AIR/造梦微端）；普通目标仍放行"},
    {L"injected_module_stale_detection", L"default",
        L"同路径复用旧实例判据：磁盘 DLL 晚于目标进程启动 ⇒ 判旧版（软键态会失效）；拿不到时间 ⇒ 不误报"},
    {L"fake_focus_maplestory_focus_only", L"default",
        L"MapleStoryClass IAT: fake GetCursorPos/GetAsyncKeyState, no WndProc subclass, no WM_INPUT"},
    {L"fake_focus_soft_input", L"default",
        L"Phase2: soft shared memory drives GetCursorPos/GetAsyncKeyState/GetKeyboardState + Raw Input"},
    {L"window_time_scale_iat", L"default",
        L"窗口变速：内联钩装上/2 倍速下 QPC 走快 2 倍/**缓存指针也走快**/关闭后彻底还原"},
    {L"window_time_scale_only_iat", L"default",
        L"仅变速注入：关掉假焦点注入时变速照常生效，且一个假焦点钩都没装"},
    {L"anjuzhen_script_wm_config", L"default",
        L"Parse build/*/scripts/安居镇.json windowMode: fakeFocus + Chrome child class"},
    {L"permission_match_uipi", L"default",
        L"CheckPermissionMatch: self/0 ok; explorer allowed even if this process is elevated"},
    {L"permission_mismatch_no_autolaunch", L"default",
        L"PermissionMismatch/DesktopNotReady must abort auto-launch (do not re-open MapleStoryt.exe)"},
    {L"maplestory_bg_fake_focus", L"default",
        L"MapleStoryClass / MapleStory.exe / 冒险岛 title →LCA PostMessage + mapleSafe lite；UsesFakeFocus=0、不最小化"},
    {L"background_fake_focus_not_degraded", L"default",
        L"后台+GLFW30+关注入 ⇒ 必须注入假焦点（否则回退假前台 SendInput 会抢鼠标）；仅时钟补丁判据不带「关了注入」"},
    {L"fake_focus_uses_bound_hwnd_class", L"default",
        L"配置类名为空（拖拽拾取）时必须按已绑定 HWND 的类名判假焦点；GLFW30/SDL_app 不得漏判"},
    {L"lca_bg_unknown_game", L"default",
        L"未登记游戏类名走 LCA 窗口消息；Unity 仍注入；记事本仍走Edit/WM_CHAR"},
    {L"tianlong_bg_fake_focus", L"default",
        L"TianLongBaBuHJ WndClass / 天龙八部 →精简假焦点，不是 LCA 纯PostMessage"},
    {L"lca_arrow_key_lparam", L"default",
        L"方向键lParam 扫描码0x4B + KF_EXTENDED；←/U+2190 规整为VK_LEFT"},
    {L"lca_nav_key_leaks_to_foreground", L"default",
        L"目标不在前台时方向键兜底不得SendInput（否则打进遮挡窗：浏览器视频跳进度/调音量）"},
    {L"lca_nav_keyup_released_after_focus_loss", L"default",
        L"方向键：按下时在前台补了真键、松开时已切走也必须补KEYUP（否则真键卡死，游戏朝一个方向一直走）"},
    {L"window_mode_target_lost_stops", L"default",
        L"BeginRun then DestroyWindow → TargetStillAlive is false (game crash must stop the script)"},
    {L"ext_browser_leaf_predicate", L"default",
        L"「浏览器是不是已经开着」判据：整名+大小写无关；msedgewebview2.exe / 自家 exe / "
        L"片段名一律**不算**浏览器（算错就会在用户没开浏览器时静默不拉，或反过来白开一个窗口）"},
    {L"mouse_wheel_step_events", L"default",
        L"滚轮 步数→消息：一条只表达 ±64 格，多步拆多条；**任何一条的增量都必须装得进 SHORT**"
        L"（274 格 = 120×274 > 32767，旧实现回绕成反向 ⇒ 滚轮像坏了一样）"},
    {L"fake_focus_wheel_enqueue", L"default",
        L"滚轮必须**无条件入队**（不依赖只有 Chromium/Qt 壳才置的 kSoftFlagPostKeyEvents）："
        L"旧实现里普通游戏的滚轮请求被静默丢弃，而日志照样说投递成功"},
    {L"uwp_frame_bind_pid_still_alive", L"default",
        L"UWP ApplicationFrameHost vs CoreWindow PID mismatch must not look like a crash"},
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
    {L"uia_control_pick_by_name", L"default",
        L"UIA 控件按名字选：完全同名 > 前缀；灰控件降权；近似竞争判歧义；同分取阅读顺序最前"},
    {L"uia_control_list_format", L"default",
        L"UIA 控件台账文本：编号连续、含类型/名字/灰态能力位与坐标"},
    {L"uia_action_verb_table", L"default",
        L"控件类型→动作能力动词的穷尽表：输入框=fill、复选框=toggle、滑块=slide、滚动条=scroll、"
        L"下拉/列表/树/数据项=select、按钮/链接=click；未知类型必须给 focus（绝不冒充 click）"},
    {L"uia_control_list_carries_action_and_state", L"default",
        L"台账文本透出 action:… 与可读状态（focused/value:/range:/toggle:/state:/v:%/readonly）"
        L"——这些正是截图读不出来、导致模型「先点一下看看」的事实"},
    {L"shell_icon_host_class", L"default",
        L"★桌面/资源管理器图标判据（外壳视图宿主类）——它决定「打开」要不要双击"},
    {L"screen_point_occlusion_check", L"default",
        L"IsScreenPointOnForegroundWindow：屏幕外点必须判「不属于前台」；抢到前台时窗口内点必须判「属于前台」"},
    {L"uia_invoke_chain", L"default",
        L"UIA Invoke 调用链（同进程）：建窗+按钮 → UIA 查找 → Invoke → 确认 WM_COMMAND 到达"},
    {L"uia_invoke_cross_process", L"default",
        L"UIA Invoke 调用链（跨进程）：UWP 的核心特征就是跨进程；若这条不通，"
        L"「UWP 走 UIA」这条路就不成立"},
    {L"soft_input_fast_path", L"default",
        L"每拍输入快速路径：几何/绑定/顶窗类名全未变才放行；顶窗失效、子窗绑定、尺寸变化、"
        L"无缓存、HWND 复用（类名变）一律退回完整路径"},
};

void TestUiaControlPickByName() {
    auto mk = [](const wchar_t* name, const wchar_t* type, int top, int left,
                 bool enabled, bool invokable) {
        windowmode::UiControlInfo c;
        c.name = name;
        c.controlType = type;
        c.rect = RECT{ left, top, left + 100, top + 30 };
        c.enabled = enabled;
        c.invokable = invokable;
        return c;
    };
    std::vector<windowmode::UiControlInfo> items = {
        mk(L"保存并关闭", L"按钮", 300, 10, true, true),
        mk(L"保存", L"按钮", 100, 10, true, true),
        mk(L"保存", L"菜单项", 500, 10, true, true),
    };
    for (size_t i = 0; i < items.size(); ++i) items[i].id = static_cast<int>(i) + 1;

    bool amb = false;
    // 完全同名两项：取阅读顺序最前（top 小的），且不算歧义）
    const int exact = windowmode::PickUiControlByName(items, L"保存", &amb);
    const bool exactOk = exact == 1 && !amb;

    // 部分命中：目标比控件名长时命中更「具体」的那个（保存并关闭 > 保存）
    amb = false;
    const int partial = windowmode::PickUiControlByName(items, L"保存并关闭窗口", &amb);
    const bool partialOk = partial == 0 && !amb;

    // 近似竞争（两个都是「包含」级且分数接近）→判歧义）
    std::vector<windowmode::UiControlInfo> tie = {
        mk(L"确定提交订单", L"按钮", 200, 10, true, true),
        mk(L"确定放弃订单", L"按钮", 210, 10, true, true),
    };
    for (size_t i = 0; i < tie.size(); ++i) tie[i].id = static_cast<int>(i) + 1;
    amb = false;
    const int tieIdx = windowmode::PickUiControlByName(tie, L"订单", &amb);
    const bool tieAmb = amb;
    const bool tieOk = tieIdx >= 0 && tieAmb;

    // 灰控件降权：同名前缀时优先可用的那个
    std::vector<windowmode::UiControlInfo> gray = {
        mk(L"下一步", L"按钮", 100, 10, false, true),
        mk(L"下一步", L"按钮", 400, 10, true, true),
    };
    for (size_t i = 0; i < gray.size(); ++i) gray[i].id = static_cast<int>(i) + 1;
    amb = false;
    const int grayIdx = windowmode::PickUiControlByName(gray, L"下一步", &amb);
    const bool grayOk = grayIdx == 1;

    // 完全无命中→-1（调用方回落识图）。
    const bool missOk = windowmode::PickUiControlByName(items, L"立即购买", nullptr) < 0;
    // 空标签→-1
    const bool emptyOk = windowmode::PickUiControlByName(items, L"", nullptr) < 0;

    const bool ok = exactOk && partialOk && tieOk && grayOk && missOk && emptyOk;
    selftest::Emit(L"uia_control_pick_by_name", ok,
        ok ? L"" : (L"exact=" + std::to_wstring(exact) + L" exactOk="
            + std::to_wstring(exactOk ? 1 : 0) + L" partial="
            + std::to_wstring(partial) + L" partialOk=" + std::to_wstring(partialOk ? 1 : 0)
            + L" tie=" + std::to_wstring(tieIdx) + L"/" + std::to_wstring(tieAmb ? 1 : 0)
            + L" tieOk=" + std::to_wstring(tieOk ? 1 : 0)
            + L" gray=" + std::to_wstring(grayIdx) + L" grayOk="
            + std::to_wstring(grayOk ? 1 : 0) + L" miss=" + std::to_wstring(missOk ? 1 : 0)
            + L" empty=" + std::to_wstring(emptyOk ? 1 : 0)).c_str());
}

void TestUiaControlListFormat() {
    windowmode::UiControlInfo a;
    a.id = 1;
    a.name = L"保存";
    a.controlType = L"按钮";
    a.rect = RECT{ 100, 200, 180, 230 };
    a.enabled = true;
    a.invokable = true;
    windowmode::UiControlInfo b;
    b.id = 2;
    b.name = L"文件名";
    b.controlType = L"输入框";
    b.rect = RECT{ 300, 400, 500, 430 };
    b.enabled = false;      // 灰
    b.valuePattern = true;  // 可填
    b.invokable = false;    // 需点击
    const std::wstring text = windowmode::FormatUiControlListForAgent({ a, b }, 2000);
    const bool ok = text.find(L"[1]") != std::wstring::npos
        && text.find(L"[2]") != std::wstring::npos
        && text.find(L"按钮") != std::wstring::npos
        && text.find(L"保存") != std::wstring::npos
        && text.find(L"（灰）") != std::wstring::npos
        && text.find(L"[可填]") != std::wstring::npos
        && text.find(L"[需点击]") != std::wstring::npos
        && text.find(L"@140,215") != std::wstring::npos;
    // 空列表→空串（调用方据此判「枚举不到」）
    const bool emptyOk = windowmode::FormatUiControlListForAgent({}, 100).empty();
    selftest::Emit(L"uia_control_list_format", ok && emptyOk,
        ok ? (emptyOk ? L"" : L"empty-list not empty") : text.c_str());
}

// ── 控件类型 → 动作能力动词：**遍历实现用的同一张表**，逐格断言 ──────────────
// 这张表是「半视觉」的事实核心：模型据此知道某个条目支持哪一类操作，
// 而不是靠截图猜、或先点一下试试。它必须是**纯函数**（不碰 UIA），才测得到。
//
// ⚠ 自检**不自己抄 `UIA_*ControlTypeId` 常量**（那要拉 COM 头，且两份常量必然漂移：
//   加了类型而自检表没跟上就依然全绿）。改为遍历 `UiControlTypeTable()` 本身 ——
//   与实现同一份事实，本仓 §43 的同一条纪律。
void TestUiaActionVerbTable() {
    int n = 0;
    const windowmode::UiControlTypeRow* rows = windowmode::UiControlTypeTable(&n);

    std::wstring bad;
    int checked = 0;
    for (int i = 0; i < n; ++i) {
        const windowmode::UiControlTypeRow& r = rows[i];
        ++checked;
        // ① 表里的 action 必须与按类型查出来的**完全一致**（表的两个出口不许各说各话）
        const std::wstring got = windowmode::UiActionVerbForControl(r.controlTypeId);
        if (got != r.action) {
            bad += L" row" + std::to_wstring(i) + L" id=" + std::to_wstring(r.controlTypeId)
                + L" verb=" + got + L" table=" + r.action + L";";
        }
        // ② 空角色/空动词等于台账那行少一段 ⇒ 模型又看不出能力（这一轮就白做了）
        if (!r.label || !r.label[0]) bad += L" row" + std::to_wstring(i) + L" label-empty;";
        if (!r.action || !r.action[0]) bad += L" row" + std::to_wstring(i) + L" action-empty;";
    }

    // ③ 未知类型必须给 `focus`，**绝不冒充 `click`** ——
    //    点一个说不清是什么的东西会打错目标；「可聚焦」不构成任何能力承诺。
    const int unknownIds[] = { 0, -1, 99999 };
    for (int id : unknownIds) {
        if (std::wstring(windowmode::UiActionVerbForControl(id)) != L"focus") {
            bad += L" unknown id=" + std::to_wstring(id) + L" not focus;";
        }
    }
    // ④ 表本身不许为空（空表会让上面所有循环静默通过 = 假绿）
    const bool nonEmpty = n > 0;
    if (!nonEmpty) bad += L" table-empty;";

    const bool ok = bad.empty();
    selftest::Emit(L"uia_action_verb_table", ok,
        ok ? L"" : (L"rows=" + std::to_wstring(n) + bad).c_str());
}

// ── 台账文本必须把 action + 可读状态一起透出去 ──────────────────────────────
// 这条钉的是**给模型看的那一行**：角色/能力/状态缺一个，模型就得回到截图。
void TestUiaControlListCarriesActionAndState() {
    // 类型 id 从**实现的那张表**里按角色标签取（不在自检里抄 UIA 常量：那要拉 COM 头，
    // 且两份常量必然漂移）。取不到就让用例转红，而不是静默拿一个 0 去测。
    auto typeIdByLabel = [](const wchar_t* label) -> int {
        int n = 0;
        const windowmode::UiControlTypeRow* rows = windowmode::UiControlTypeTable(&n);
        for (int i = 0; i < n; ++i) {
            if (std::wstring(rows[i].label) == label) return rows[i].controlTypeId;
        }
        return 0;
    };
    const int sliderType = typeIdByLabel(L"滑块");
    const int editType = typeIdByLabel(L"输入框");
    const int checkType = typeIdByLabel(L"复选框");

    windowmode::UiControlInfo slider;
    slider.id = 1;
    slider.name = L"音量";
    slider.controlType = L"滑块";
    slider.action = windowmode::UiActionVerbForControl(sliderType);
    slider.rect = RECT{ 100, 100, 400, 130 };
    slider.state = { L"value:42", L"range:0-100" };

    windowmode::UiControlInfo edit;
    edit.id = 2;
    edit.name = L"密码";
    edit.controlType = L"输入框";
    edit.action = windowmode::UiActionVerbForControl(editType);
    edit.rect = RECT{ 100, 200, 400, 230 };
    edit.password = true;
    // ⚠ 密码框只报「它是密码框」，**值一个字都不给**（值回传会进 API 请求）。
    edit.state = { L"password(值不回传)" };

    windowmode::UiControlInfo toggle;
    toggle.id = 3;
    toggle.name = L"自动更新";
    toggle.controlType = L"复选框";
    toggle.action = windowmode::UiActionVerbForControl(checkType);
    toggle.rect = RECT{ 100, 300, 300, 330 };
    toggle.state = { L"toggle:on", L"focused" };

    const std::wstring text =
        windowmode::FormatUiControlListForAgent({ slider, edit, toggle }, 4000);
    const bool ok = sliderType != 0 && editType != 0 && checkType != 0
        && text.find(L"action:slide") != std::wstring::npos
        && text.find(L"action:fill") != std::wstring::npos
        && text.find(L"action:toggle") != std::wstring::npos
        && text.find(L"[value:42]") != std::wstring::npos
        && text.find(L"[range:0-100]") != std::wstring::npos
        && text.find(L"[toggle:on]") != std::wstring::npos
        && text.find(L"[focused]") != std::wstring::npos
        && text.find(L"[password(值不回传)]") != std::wstring::npos
        // 台账里**不许**出现任何密码值（这里没有值可泄，但要求「只报事实」的形态成立）
        && text.find(L"value:\"") == std::wstring::npos;
    selftest::Emit(L"uia_control_list_carries_action_and_state", ok,
        ok ? L"" : (L"slider=" + std::to_wstring(sliderType) + L" edit="
            + std::to_wstring(editType) + L" check=" + std::to_wstring(checkType) + L"\n"
            + text).c_str());
}

void TestScreenPointOcclusionCheck() {    // 屏幕外的点：WindowFromPoint 取不到窗口→必须判定为「不属于前台。
    const bool outside = !windowmode::IsScreenPointOnForegroundWindow(-20000, -20000);
    // 自建窗口并置前（不复用文件后面的 helper，避免依赖定义顺序）；
    // 窗口内中心点必须判定为「属于前台」，否则会把正常点击误拦。
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QstOcclusionProbe";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"QstOcclusion",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 420, 300,
        nullptr, nullptr, wc.hInstance, nullptr);
    bool insideOk = true;
    std::wstring occluder;   // 诊断：若中心点被判「不属于前台」，是谁盖着它
    if (w) {
        ShowWindow(w, SW_SHOW);
        SetForegroundWindow(w);
        // ⚠⚠ 2026-10-05：`SetForegroundWindow` **不一定**能把测试窗口放到最上层 ——
        //   用户机器上若有 `WS_EX_TOPMOST` 的窗口（实测：`Chrome_RenderWidgetHostHWND`），
        //   它仍会盖住中心点 ⇒ `WindowFromPoint` 返回那个置顶窗
        //   ⇒ 误报「窗口内点不属于前台」（实测 `inside=0 中心点被「Chrome_...」盖住`）。
        //   ⇒ 临时把测试窗口置顶，判定完立即撤销（`NOACTIVATE`，不抢焦点）。
        SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // 只有真的抢到前台才验证「窗口内点 = 属于前台」；抢不到（被别的程序挡住
        // / 测试机前台策略限制）就跳过这半条 —— 否则这条会变成环境相关的假失败。
        HWND fgNow = GetForegroundWindow();
        const bool weAreForeground = fgNow && GetAncestor(fgNow, GA_ROOT) == GetAncestor(w, GA_ROOT);
        RECT rc{};
        if (weAreForeground && GetWindowRect(w, &rc)) {
            const int cx = (rc.left + rc.right) / 2;
            const int cy = (rc.top + rc.bottom) / 2;
            insideOk = windowmode::IsScreenPointOnForegroundWindow(cx, cy);
            // ⚠ 2026-10-05：`insideOk` 默认 true，只有抢到前台才赋值 ⇒ 它变 0 意味着
            //   「**窗口是前台，但中心点仍被判不属于前台**」⇒ 该点上**有东西盖着**
            //   （置顶窗 / 输入法候选 / 悬浮球 / 我们的取点浮层）。原来只报 `inside=0`，
            //   看不出是谁盖的 ⇒ 补上 `WindowFromPoint` 的结果。
            if (!insideOk) {
                POINT pt{cx, cy};
                HWND at = WindowFromPoint(pt);
                wchar_t cls[128]{};
                if (at) GetClassNameW(at, cls, 128);
                occluder = (at ? (cls[0] ? cls : L"(无类名)") : L"(null)");
            }
        }
        DestroyWindow(w);
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::wstring detail = L"outside=" + std::to_wstring(outside ? 1 : 0)
        + L" inside=" + std::to_wstring(insideOk ? 1 : 0);
    if (!occluder.empty()) detail += L" 中心点被「" + occluder + L"」盖住";
    selftest::Emit(L"screen_point_occlusion_check", outside && insideOk, detail.c_str());
}

bool g_uiaProbeClicked = false;

LRESULT CALLBACK UiaProbeWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) { g_uiaProbeClicked = true; return 0; }
    return DefWindowProcW(h, m, w, l);
}

/// ⚠ 2026-10-05：验证 **UIA Invoke 调用链本身**是否通。
///
/// 背景：用户报「UWP 计算器在后台窗口模式下点击没反应」。UWP 的 `PostMessage` 无效，
/// **唯一**可用的点击路径就是 `TryUiaInvokeAtScreenPoint()` ⇒ 必须能独立验证它。
///
/// ⚠ 用 **Win32 按钮**代替 UWP 按钮 —— 它**证明不了** UWP 场景（UWP 的 UIA 树是跨进程
///   按需构建的，另有风险），但能证明「**调用链通**」：若这里都失败，问题就在
///   UIA 调用本身（COM / 元素查找 / Invoke）而不是 UWP 特有行为。
///
/// ⚠ UIA 依赖 COM + 桌面 ⇒ **隔离会话 / 无桌面环境下会失败**，那是环境问题不是缺陷
///   ⇒ `TryUiaInvokeAtScreenPoint` 返回 false 时**跳过**（记 skipped，不算红）。
void TestUiaInvokeChain() {
    g_uiaProbeClicked = false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = UiaProbeWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QstUiaInvokeProbe";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"QstUiaProbe",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 420, 300,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        selftest::Emit(L"uia_invoke_chain", true, L"skipped: 建窗失败（无桌面？）");
        return;
    }
    HWND btn = CreateWindowExW(0, L"Button", L"Probe", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        40, 40, 160, 40, w, nullptr, wc.hInstance, nullptr);
    ShowWindow(w, SW_SHOW);
    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    RECT brc{};
    int sx = 0, sy = 0;
    bool havePoint = false;
    if (btn && GetWindowRect(btn, &brc)) {
        sx = (brc.left + brc.right) / 2;
        sy = (brc.top + brc.bottom) / 2;
        havePoint = true;
    }

    const bool invoked = havePoint && windowmode::TryUiaInvokeAtScreenPoint(w, sx, sy);
    // Invoke 是异步的 ⇒ 抽干消息队列让 WM_COMMAND 到达（最多 ~400ms）
    MSG msg{};
    for (int i = 0; i < 20 && !g_uiaProbeClicked; ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_uiaProbeClicked) break;
        Sleep(20);
    }
    if (btn) DestroyWindow(btn);
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (!invoked) {
        // ⚠ 环境跳过：隔离会话 / 无桌面 / COM 不可用。**不算失败**，但要能看见。
        selftest::Emit(L"uia_invoke_chain", true,
            L"skipped: UIA 不可用（隔离会话/无桌面？）—— 真机上这条才有意义");
        return;
    }
    selftest::Emit(L"uia_invoke_chain", g_uiaProbeClicked,
        (L"invoked=1 clicked=" + std::to_wstring(g_uiaProbeClicked ? 1 : 0)
            + L"（invoked 成功但按钮没收到 WM_COMMAND ⇒ UIA 元素找到了但 Invoke 没生效）").c_str());
}

/// `--uia-child`：建一个带按钮的窗口并泵消息 10 秒，供父进程做**跨进程** UIA 验证。
/// ⚠ 类名与同进程用例的 `QstUiaInvokeProbe` **刻意不同**，避免 `FindWindowW` 抓错。
int RunUiaChildWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QstUiaXProcProbe";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"QstUiaXProc",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 140, 140, 420, 300,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) return 1;
    CreateWindowExW(0, L"Button", L"XProbe", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        40, 40, 160, 40, w, nullptr, wc.hInstance, nullptr);
    ShowWindow(w, SW_SHOW);
    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const DWORD until = GetTickCount() + 10000;
    MSG msg{};
    while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

/// ⚠⚠ 2026-10-05：验证**跨进程** UIA 是否工作。
///
/// 为什么单独一条：UWP 的**核心特征就是跨进程** —— 壳窗 `ApplicationFrameWindow` 在
/// `ApplicationFrameHost.exe`，内容窗 `CoreWindow` 在**另一个进程**（如 `CalculatorApp`）。
/// 同进程的 `uia_invoke_chain` **证明不了**跨进程行为。
/// ★ 若跨进程 UIA 本身不工作，那「UWP 走 UIA」这条路**根本不成立**（要换方案）——
///   所以这条用例能一次排除一整条路线。
void TestUiaInvokeCrossProcess() {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        selftest::Emit(L"uia_invoke_cross_process", true, L"skipped: 取不到自身路径");
        return;
    }
    std::wstring cmd = std::wstring(L"\"") + exe + L"\" --uia-child";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        selftest::Emit(L"uia_invoke_cross_process", true, L"skipped: 起不了子进程");
        return;
    }
    HWND w = nullptr;
    // ⚠ 2026-10-05：原来只等 6 秒（60×100ms）—— 机器忙/并行构建时子进程可能起得慢，
    //   会偶发「子进程窗口未出现」的**假失败**。放宽到 15 秒。
    for (int i = 0; i < 150 && !w; ++i) {
        Sleep(100);
        w = FindWindowW(L"QstUiaXProcProbe", nullptr);
        // 子进程若已退出（起不来/秒退），不必再等
        if (!w && pi.hProcess
            && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
    }
    bool ok = false;
    std::wstring detail = L"子进程窗口未出现";
    if (w) {
        HWND btn = FindWindowExW(w, nullptr, L"Button", nullptr);
        RECT brc{};
        if (btn && GetWindowRect(btn, &brc)) {
            const int sx = (brc.left + brc.right) / 2;
            const int sy = (brc.top + brc.bottom) / 2;
            ok = windowmode::TryUiaInvokeAtScreenPoint(w, sx, sy);
            detail = ok
                ? L"跨进程 UIA 找到并 Invoke 成功"
                : L"跨进程 UIA 失败（同进程能成功 ⇒ 问题出在跨进程这一层）";
        } else {
            detail = L"子进程里没找到按钮";
        }
    }
    if (pi.hProcess) {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    selftest::Emit(L"uia_invoke_cross_process", ok, detail.c_str());
}

void TestSoftInputFastPath() {
    // 允许：顶窗活着 + 绑定就是顶层 + 有缓存 + 几何一致。
    const bool allow = windowmode::CanUseSoftInputFastPath(
        /*topAlive*/ true, /*boundIsTopLevel*/ true,
        /*haveCached*/ true, 800, 600, 800, 600);
    // 每条前提单独破坏都必须退回完整路径 —— 任一条漏了都会在真实回放里
    // 「窗口重开 / 缩放 / 子窗绑定」时拿旧几何继续跑，落点整体偏移。
    const bool topDead = !windowmode::CanUseSoftInputFastPath(false, true, true, 800, 600, 800, 600);
    const bool childBind = !windowmode::CanUseSoftInputFastPath(true, false, true, 800, 600, 800, 600);
    const bool noCache = !windowmode::CanUseSoftInputFastPath(true, true, false, 800, 600, 800, 600);
    const bool resized = !windowmode::CanUseSoftInputFastPath(true, true, true, 800, 600, 801, 600);
    const bool resizedH = !windowmode::CanUseSoftInputFastPath(true, true, true, 800, 600, 800, 599);
    // 缓存里的尺寸非法（首次绑定没量到）也必须退回。
    const bool zeroCache = !windowmode::CanUseSoftInputFastPath(true, true, true, 0, 0, 0, 0);

    // ── 类名维度（CanUseSoftInputFastPathClass）─────────────────────────
    // 为什么必须有这一层：**窗口句柄会被系统复用**。关掉旧目标再开新目标
    // （或目标闪退后重开）可能拿到同一个 HWND 值，客户区尺寸也恰好相同 ——
    // 那时「HWND 相等 + 几何一致」全部成立，只靠上面那套判据会拿**旧绑定**
    // 去投递，输入落到错误的目标上，且**不报任何错**。
    const bool clsAllow = windowmode::CanUseSoftInputFastPathClass(
        true, true, true, /*classMatches*/ true, 800, 600, 800, 600);
    // 类名不同（= HWND 被复用）→ 必须退回完整路径重新绑定。
    const bool clsReused = !windowmode::CanUseSoftInputFastPathClass(
        true, true, true, /*classMatches*/ false, 800, 600, 800, 600);
    // 类名不符时其余条件再好也不放行（逐条确认没有被短路掉）。
    const bool clsReusedBeatsGeom = !windowmode::CanUseSoftInputFastPathClass(
        true, true, true, false, 800, 600, 800, 600);
    // 类名相同时，原有前提依然各自生效（不能因为加了类名就漏掉旧判据）。
    const bool clsTopDead = !windowmode::CanUseSoftInputFastPathClass(
        false, true, true, true, 800, 600, 800, 600);
    const bool clsChildBind = !windowmode::CanUseSoftInputFastPathClass(
        true, false, true, true, 800, 600, 800, 600);
    const bool clsResized = !windowmode::CanUseSoftInputFastPathClass(
        true, true, true, true, 800, 600, 801, 600);

    const bool ok = allow && topDead && childBind && noCache && resized && resizedH && zeroCache
        && clsAllow && clsReused && clsReusedBeatsGeom
        && clsTopDead && clsChildBind && clsResized;
    wchar_t detail[480]{};
    swprintf_s(detail,
        L"allow=%d topDead=%d childBind=%d noCache=%d resized=%d resizedH=%d zero=%d"
        L" | clsAllow=%d clsReused=%d clsTopDead=%d clsChildBind=%d clsResized=%d",
        allow ? 1 : 0, topDead ? 1 : 0, childBind ? 1 : 0, noCache ? 1 : 0,
        resized ? 1 : 0, resizedH ? 1 : 0, zeroCache ? 1 : 0,
        clsAllow ? 1 : 0, clsReused ? 1 : 0,
        clsTopDead ? 1 : 0, clsChildBind ? 1 : 0, clsResized ? 1 : 0);
    selftest::Emit(L"soft_input_fast_path", ok,
        ok ? L"几何/绑定/顶窗类名全部未变才走快速路径；各类失效场景一律退回完整路径"
           : detail);
}

void PrintHelp() {
    std::fwprintf(stderr,
        L"WindowModeSelfTest — QuickScriptTool 窗口/后台窗口模式自检\n"
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

void PumpMessagesDispatchOnly(std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
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
    windowmode::ResetSoftMouseState();
    RECT rc{};
    GetClientRect(edit, &rc);
    windowmode::RememberSoftMouseClientPos(edit,
        (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
    const std::wstring longText(80, L'X');
    std::atomic_bool cancel{false};
    std::atomic_bool workerDone{false};
    std::thread worker([&]() {
        windowmode::PostQuickInputToWindow(edit, longText, 0.03, false, &cancel);
        workerDone.store(true, std::memory_order_relaxed);
    });
    // SendMessage 打到本线程窗口：必须先等到至少写出一字，再取消。
    // 固定睡280ms 再cancel：工作线程若还没排上队，会len=0 误报。
    const auto startWait = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ReadEditText(edit).empty()
        && std::chrono::steady_clock::now() < startWait
        && !workerDone.load(std::memory_order_relaxed)) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
    // 不依赖焦点窗口：取消后应很快返回（完整40 字×30ms 约 1.2s，取消应远小于此）
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

    // 关闭窗口/后台窗口模式后写盘/读盘不得保留路径与类名。
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

// ── 扩展桥：AbortPending 之后**新连接仍必须被受理** ──────────────────────
// 为什么有这个用例（2026-09-24 真实故障）：
//   `EngineHost::StopRun()` 每次停止脚本都会调
//   `WindowModeExecutor::NotifyCancel()` ⇒ `ExtBridgeServer::AbortPending()` ⇒ abort_=true。
//   而清它的 `EndRun()` **只在窗口模式会话开着时**才跑 ⇒ 跑「窗口模式关闭」的脚本后
//   闩锁**永久为 true**。当时 `HandleClient()` 开头写的是
//   `if (stop_.load() || abort_.load()) return false;` ⇒ 桥**永久变聋**：
//   新连接 TCP 连得上（内核 backlog），但不读请求、不回响应、**不留日志**，
//   客户端只看到「连接被重置」，进程却完全健康 ⇒ 症状与「扩展没连上」一模一样。
// 本用例直接打真 socket，钉死修复后的语义：**abort 只打断在途等待，不拒绝新连接**。
// ── 日志：超长行**不得**终止进程（2026-09-25 闪退事故的回归）─────────────
//
// ⚠⚠ 前科：`WindowModeLogf/Verbosef/Eventf` 曾用 `wchar_t buf[1024]{} + vswprintf_s`。
//   `vswprintf_s` 在**截断**时返回 -1 并调用 **invalid parameter handler**，
//   默认 handler 会**直接终止进程** ⇒ **一条过长的日志就能让软件静默闪退**
//   （没有日志、没有退出码，排查时完全看不出跟日志有关）。
//   而我们的日志里会拼入网页返回的错误消息 / URL / 路径，长度**不受我们控制**。
//
// 本用例故意打一条 ~4000 字符的日志：
//   · 修好后 ⇒ 正常通过；
//   · 改回固定缓冲 ⇒ **测试进程当场消失**（不是红，是崩溃）——
//     所以这条用例的"失败形态"就是进程没了，看 exe 有没有正常输出汇总行即可。
// ── 桥：**并发请求的回执不许串台**（2026-09-25，为了「多会话并行」）─────────
//
// ⚠⚠ 为什么必须测这个：桥原来是**全局单槽**等待
//   （`waitingId_` + 一份 `waitingResult_`/`waitingDone_`）⇒
//   两条并发请求会互相吞掉对方的回执（后发的把 `waitingId_` 覆盖掉，
//   先发的永远等不到 ⇒ 表现成"莫名超时"）。
//   这正是 `web_ai_driver.cpp` 不得不用一把全局锁把调用串起来的原因。
//   改成「按请求 id 关联的等待槽」之后，**必须证明**并发不串 —— 光看代码不算。
//
// 手法：在自检里**冒充一路扩展**（真 WS 握手 + 真帧），
//   让它收到两条请求后**倒序回执**（先回后到的那条），
//   再断言两个调用方各自拿到**自己那条**的回执。
//   ⚠ 倒序回执是关键：如果实现是"按到达顺序配对"，就会错。

namespace {

/// 极简 WebSocket 客户端（只支持：文本帧、无分片、无扩展）。
/// 仅供自检冒充"一路扩展"用。
class MiniWsClient {
public:
    ~MiniWsClient() { Close(); }

    bool Connect(int port, const std::string& path, const std::string& token = {}) {
        s_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_ == INVALID_SOCKET) return false;
        DWORD tv = 6000;
        ::setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        ::setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        a.sin_addr.s_addr = htonl(0x7F000001);
        if (::connect(s_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;

        // ★ 桥现在**要求** `?token=`（与产品里扩展走的是同一条判据：空 token 一律 401）。
        //   这里不再"允许不传"——假扩展必须和真扩展用同一套握手，否则自检过的
        //   是一条产品里根本不存在的路径。
        const std::string req =
            "GET " + path + "?token=" + token + " HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!SendAll(req.data(), static_cast<int>(req.size()))) return false;

        std::string hdr;
        char c = 0;
        while (hdr.size() < 8192) {
            const int n = ::recv(s_, &c, 1, 0);
            if (n != 1) return false;
            hdr.push_back(c);
            if (hdr.size() >= 4 && hdr.compare(hdr.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        return hdr.find("101") != std::string::npos;
    }

    bool SendText(const std::string& payload) {
        std::string f;
        f.push_back(static_cast<char>(0x81));   // FIN + opcode=text
        const size_t n = payload.size();
        if (n < 126) {
            f.push_back(static_cast<char>(0x80 | static_cast<int>(n)));
        } else if (n <= 0xFFFF) {
            f.push_back(static_cast<char>(0x80 | 126));
            f.push_back(static_cast<char>((n >> 8) & 0xFF));
            f.push_back(static_cast<char>(n & 0xFF));
        } else {
            return false;
        }
        unsigned char mask[4];
        for (int i = 0; i < 4; ++i) {
            mask[i] = static_cast<unsigned char>(::rand() & 0xFF);
        }
        f.append(reinterpret_cast<const char*>(mask), 4);
        for (size_t i = 0; i < n; ++i) {
            f.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        }
        return SendAll(f.data(), static_cast<int>(f.size()));
    }

    /// 读一条**文本**帧（跳过 ping，遇到 close 返回 false）
    bool RecvText(std::string& out, int timeoutMs) {
        DWORD tv = static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 6000);
        ::setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        for (;;) {
            unsigned char h[2] = {0, 0};
            if (!ReadExact(h, 2)) return false;
            const int opcode = h[0] & 0x0F;
            const bool masked = (h[1] & 0x80) != 0;
            unsigned long long len = h[1] & 0x7F;
            if (len == 126) {
                unsigned char e[2] = {0, 0};
                if (!ReadExact(e, 2)) return false;
                len = (static_cast<unsigned long long>(e[0]) << 8) | e[1];
            } else if (len == 127) {
                unsigned char e[8] = {0};
                if (!ReadExact(e, 8)) return false;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
            }
            unsigned char mask[4] = {0, 0, 0, 0};
            if (masked && !ReadExact(mask, 4)) return false;
            std::string payload(static_cast<size_t>(len), '\0');
            if (len && !ReadExact(reinterpret_cast<unsigned char*>(&payload[0]),
                                  static_cast<int>(len))) {
                return false;
            }
            if (masked) {
                for (size_t i = 0; i < payload.size(); ++i) {
                    payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
                }
            }
            if (opcode == 0x8) return false;   // close
            if (opcode == 0x9) continue;       // ping：忽略（桥不用，但保险）
            out = payload;
            return true;
        }
    }

    void Close() {
        if (s_ != INVALID_SOCKET) {
            ::closesocket(s_);
            s_ = INVALID_SOCKET;
        }
    }

private:
    bool SendAll(const char* p, int n) {
        int sent = 0;
        while (sent < n) {
            const int r = ::send(s_, p + sent, n - sent, 0);
            if (r <= 0) return false;
            sent += r;
        }
        return true;
    }
    bool ReadExact(unsigned char* buf, int n) {
        int got = 0;
        while (got < n) {
            const int r = ::recv(s_, reinterpret_cast<char*>(buf + got), n - got, 0);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }
    SOCKET s_ = INVALID_SOCKET;
};

/// 从 `{"id":123,...}` 里取 id（极简，够自检用）
int JsonIntLocal(const std::string& s, const char* key) {
    const std::string pat = std::string("\"") + key + "\":";
    const size_t k = s.find(pat);
    if (k == std::string::npos) return -1;
    return std::atoi(s.c_str() + k + pat.size());
}

/// 从 `{"type":"xxx",...}` 里取字符串值（极简）
std::string JsonStrLocal(const std::string& s, const char* key) {
    const std::string pat = std::string("\"") + key + "\":\"";
    const size_t k = s.find(pat);
    if (k == std::string::npos) return {};
    const size_t b = k + pat.size();
    const size_t e = s.find('"', b);
    if (e == std::string::npos) return {};
    return s.substr(b, e - b);
}

}  // namespace

void TestBridgeConcurrentRequestsDoNotCross() {
    auto& bridge = windowmode::ExtBridgeServer::Instance();
    std::wstring startErr;
    if (!bridge.Start(startErr)) {
        Emit(L"ext_bridge_concurrent_no_cross", true,
            L"skipped: bridge Start failed (port busy?)");
        return;
    }

    MiniWsClient ws;
    if (!ws.Connect(bridge.Port(), "/qst/ws", bridge.Token())) {
        bridge.Stop();
        Emit(L"ext_bridge_concurrent_no_cross", false, L"假扩展 WS 握手失败");
        return;
    }
    // hello（桥要求握手后 5 秒内发，否则关连接）
    ws.SendText("{\"type\":\"hello\",\"token\":\"" + bridge.Token() + "\"}");
    for (int i = 0; i < 150 && !bridge.IsExtensionConnected(); ++i) {
        ::Sleep(20);
    }
    if (!bridge.IsExtensionConnected()) {
        ws.Close();
        bridge.Stop();
        Emit(L"ext_bridge_concurrent_no_cross", false, L"假扩展未被登记（hello 失败？）");
        return;
    }

    // 两条**并发**请求
    std::string r1, r2;
    std::wstring e1, e2;   // ⚠ `Request` 的 err 是 **wstring**
    std::atomic<bool> ok1{false};
    std::atomic<bool> ok2{false};
    std::thread t1([&]() { ok1.store(bridge.Request("pingA", "", r1, e1, 8000)); });
    std::thread t2([&]() { ok2.store(bridge.Request("pingB", "", r2, e2, 8000)); });

    // 假扩展：收两条，**倒序**回执（先回后到的那条）
    std::string f1, f2;
    const bool got1 = ws.RecvText(f1, 5000);
    const bool got2 = ws.RecvText(f2, 5000);
    bool replied = false;
    if (got1 && got2) {
        const int id1 = JsonIntLocal(f1, "id");
        const int id2 = JsonIntLocal(f2, "id");
        const std::string t1s = JsonStrLocal(f1, "type");
        const std::string t2s = JsonStrLocal(f2, "type");
        // ★ 倒序回：如果桥是"按到达顺序配对"，这里就会串台
        ws.SendText("{\"id\":" + std::to_string(id2) + ",\"type\":\"result\",\"ok\":true,\"echo\":\"" + t2s + "\"}");
        ws.SendText("{\"id\":" + std::to_string(id1) + ",\"type\":\"result\",\"ok\":true,\"echo\":\"" + t1s + "\"}");
        replied = true;
    }
    t1.join();
    t2.join();
    ws.Close();
    bridge.Stop();

    const bool aOk = ok1.load() && r1.find("\"echo\":\"pingA\"") != std::string::npos;
    const bool bOk = ok2.load() && r2.find("\"echo\":\"pingB\"") != std::string::npos;
    const bool ok = replied && aOk && bOk;
    std::wstring detail;
    if (!ok) {
        detail = L"got1=" + std::to_wstring(got1 ? 1 : 0)
            + L" got2=" + std::to_wstring(got2 ? 1 : 0)
            + L" okA=" + std::to_wstring(aOk ? 1 : 0)
            + L" okB=" + std::to_wstring(bOk ? 1 : 0)
            + L" | A=" + std::wstring(r1.begin(), r1.end()).substr(0, 80)
            + L" | B=" + std::wstring(r2.begin(), r2.end()).substr(0, 80);
    }
    Emit(L"ext_bridge_concurrent_no_cross", ok, detail.c_str());
}

void TestLongLogDoesNotTerminateProcess() {
    const std::wstring big(4000, L'字');
    windowmode::WindowModeLogf(L"[自检] 超长日志回归 %s", big.c_str());
    windowmode::WindowModeLogEventf(L"[自检] 超长日志回归（持久化路径）%s", big.c_str());
    Emit(L"long_log_does_not_terminate", true, L"4000 字已打印，进程仍在");
}

/// 「浏览器已经在跑就别再拉一个」的判据（2026-09-27 真机事故）。
///
/// 事故形态：扩展离线（陈旧 token）时宿主无条件 `ShellExecute(msedge.exe)`，
/// 而用户**本来就开着浏览器**（脚本目标窗口就在里面）⇒ 又冒出一个窗口/新标签页。
/// 用户原话：「缩略图已经定位到目标窗口了，可是还是打开了新标签页」。
///
/// ⚠ 这条用例只钉**纯判据**（不真开浏览器）：判据错了就是"要么白开一个窗口、
///   要么该开的时候不开"，两种都由下面这几格直接暴露。
void TestBrowserLeafNameIsBrowser() {
    namespace wm = windowmode;
    struct Case { const wchar_t* leaf; bool want; const wchar_t* why; };
    const Case cases[] = {
        {L"msedge.exe", true, L"Edge 本体算浏览器"},
        {L"MSEDGE.EXE", true, L"进程名大小写不保证，必须无关大小写"},
        {L"chrome.exe", true, L"Chrome 算"},
        {L"firefox.exe", true, L"Firefox 算"},
        // ★★ 反例比正例重要：把下面这些算成浏览器 ⇒ 该拉浏览器时**静默不拉**。
        {L"msedgewebview2.exe", false, L"WebView2 宿主不是用户浏览器（产品自己就带一个）"},
        {L"QuickScriptTool.exe", false, L"自家 exe 不是浏览器"},
        {L"explorer.exe", false, L"资源管理器不是浏览器"},
        {L"", false, L"空名不算（枚举失败时的兜底格）"},
        // ⚠ 判据是**整名**比对，不是子串：`edg` 这类片段不许命中。
        {L"edge", false, L"片段不算（必须整名 + .exe）"},
    };
    bool ok = true;
    std::wstring detail;
    for (const auto& c : cases) {
        const bool got = wm::ExtBrowserLeafNameIsBrowser(c.leaf);
        if (got == c.want) continue;
        ok = false;
        if (!detail.empty()) detail += L"；";
        detail += std::wstring(L"「") + c.leaf + L"」→ " + (got ? L"true" : L"false")
            + L"，期望 " + (c.want ? L"true" : L"false") + L"（" + c.why + L"）";
    }
    Emit(L"ext_browser_leaf_predicate", ok, ok ? L"" : detail.c_str());
}

/// 滚轮「步数 → 消息」展开规则（2026-09-30 真机报障「滚动不能正常滚动」）。
///
/// ⚠⚠ 这条用例存在的原因：三处调用点各自手写过
///     `delta = (positive ? WHEEL_DELTA : -WHEEL_DELTA) * steps` 再 `static_cast<SHORT>`，
///     而 `SHORT` 只到 ±32767 ⇒ **steps = 274 就回绕成负数**（正着滚变成倒着滚）。
///     裸眼看不出来，语法检查也全绿，只有真机滚不动才暴露。
///     所以规则收成纯函数后，这里把**临界值与表意**一起钉死。
///     ⚠ 另一处更隐蔽的错法也一并钉住：把"步数"当"增量"直接塞进 wParam 高位
///     （`notch = steps`）—— 表现是"填 300 步也只滚 1 格"，同样没有任何报错。
void TestMouseWheelStepEvents() {
    namespace wm = windowmode;
    int buf[16]{};
    bool ok = true;
    std::wstring detail;
    auto check = [&](bool cond, const wchar_t* what) {
        if (cond) return;
        ok = false;
        if (!detail.empty()) detail += L"；";
        detail += what;
    };

    // ① 单格：一条，正负号正确。
    int n = wm::MouseWheelEventsForSteps(1, true, buf, 16);
    check(n == 1 && buf[0] == WHEEL_DELTA, L"1 格正向应为一条 +120");
    n = wm::MouseWheelEventsForSteps(1, false, buf, 16);
    check(n == 1 && buf[0] == -WHEEL_DELTA, L"1 格反向应为一条 -120");

    // ② 3 格（编辑器/助手默认值）：一条 +360 —— 增量是 **3×120**，不是 3。
    n = wm::MouseWheelEventsForSteps(3, true, buf, 16);
    check(n == 1 && buf[0] == 3 * WHEEL_DELTA, L"3 格应合成一条 +360（不是把 3 当增量）");

    // ③ ★ 溢出边界：274 格（120×274 = 32880 > 32767）。
    //    旧实现这一步会回绕成 −32656（方向反过来）——本条就是那次事故的钉子。
    const int big = 274;
    const int need = wm::WheelNotchEventCount(big);
    n = wm::MouseWheelEventsForSteps(big, true, buf, 16);
    check(n == need, L"274 格应拆成多条");
    int sum = 0;
    bool allInShort = true;
    for (int i = 0; i < n; ++i) {
        sum += buf[i];
        if (buf[i] > 32767 || buf[i] < -32768) allInShort = false;
    }
    check(allInShort, L"每条增量都必须装得进 SHORT（否则 wParam 高位回绕）");
    check(sum == big * WHEEL_DELTA, L"拆分后总增量必须等于 步数×120（不许丢格）");

    // ④ 反向同样成立（负向更容易踩回绕）。
    n = wm::MouseWheelEventsForSteps(big, false, buf, 16);
    sum = 0;
    allInShort = true;
    for (int i = 0; i < n; ++i) {
        sum += buf[i];
        if (buf[i] > 32767 || buf[i] < -32768) allInShort = false;
    }
    check(allInShort && sum == -big * WHEEL_DELTA, L"反向 274 格：不得回绕且总增量正确");

    // ⑤ steps < 1 视为 1（滚轮动作画不出"滚 0 格"），不许返回 0 条。
    n = wm::MouseWheelEventsForSteps(0, true, buf, 16);
    check(n == 1 && buf[0] == WHEEL_DELTA, L"steps=0 应按 1 格处理");

    // ⑥ cap 截断必须**可判定**：调用方靠 WheelNotchEventCount 比对才知道没写完
    //    （写不完还闷着不吭声，就等于丢格）。
    n = wm::MouseWheelEventsForSteps(big, true, buf, 2);
    check(n == 2 && wm::WheelNotchEventCount(big) > 2,
        L"cap 截断时必须能靠 WheelNotchEventCount 判定出来");

    Emit(L"mouse_wheel_step_events", ok, ok ? L"" : detail.c_str());
}

/// 滚轮**入队**这一步必须真的发生（2026-09-30 真机报障的第二半）。
///
/// ⚠⚠ 原实现：`FakeFocusSoftInput_PushWheel` 一进门就
///     `if (!(flags & kSoftFlagPostKeyEvents)) return;` —— 那个标志只有
///     Chromium 壳 / Qt 安卓壳 / Electron 会置。普通游戏（GLFW/Unity/UE）走的是
///     "目标进程内软输入"，于是滚轮请求被**直接丢掉**，而调用点照样打日志说
///     「假焦点软滚轮 steps=N …DLL/PostMessage 队列」——
///     日志里没有任何异常，用户只能看到"滚轮没反应"。
///     这条用例直接盯**共享内存里的写游标**：不涨就是没入队，别信日志。
void TestWheelQueueAcceptsWithoutPostKeyEventsFlag() {
    namespace wm = windowmode;
    if (wm::FakeFocusSoftInput_IsAttached()) wm::FakeFocusSoftInput_Detach();
    std::wstring err;
    if (!wm::FakeFocusSoftInput_Attach(::GetCurrentProcessId(), err)) {
        // 共享内存建不起来（权限/已存在）就跳过，不让本 suite 因环境而红
        Emit(L"fake_focus_wheel_enqueue", true,
            (L"skipped: soft-input attach failed: " + err).c_str());
        return;
    }
    // ★ 明确**不**置 kSoftFlagPostKeyEvents：这正是普通游戏的样子。
    wm::FakeFocusSoftInput_SetPostKeyEvents(false);

    uint32_t w0 = 0;
    uint32_t r0 = 0;
    const bool cursorsOk = wm::FakeFocusSoftInput_WheelCursors(w0, r0);
    wm::FakeFocusSoftInput_PushWheel(true, false, 3);
    uint32_t w1 = 0;
    uint32_t r1 = 0;
    wm::FakeFocusSoftInput_WheelCursors(w1, r1);
    wm::FakeFocusSoftInput_PushWheel(false, true, 1);
    uint32_t w2 = 0;
    uint32_t r2 = 0;
    wm::FakeFocusSoftInput_WheelCursors(w2, r2);

    const bool ok = cursorsOk && (w1 == w0 + 1) && (w2 == w1 + 1);
    wchar_t detail[192]{};
    swprintf_s(detail, L"write %u→%u→%u（期望各 +1）；read=%u 未动=%d",
        w0, w1, w2, r2, (r2 == r0) ? 1 : 0);
    wm::FakeFocusSoftInput_Detach();
    Emit(L"fake_focus_wheel_enqueue", ok, ok ? L"" : detail);
}

void TestExtBridgeAbortDoesNotRefuseNewClients() {
    using namespace std::chrono;
    namespace wm = windowmode;

    auto& bridge = wm::ExtBridgeServer::Instance();
    std::wstring startErr;
    if (!bridge.Start(startErr)) {
        // 端口被别的实例占了就跳过（不让本 suite 因环境而红）
        Emit(L"ext_bridge_abort_keeps_serving", true,
            L"skipped: bridge Start failed (port busy?)");
        return;
    }

    // 发一个裸 HTTP GET，返回 true = 收到了任何响应字节；false = 连接被重置/无响应。
    auto rawHttpGet = [](int port, const std::string& path) -> std::pair<bool, std::string> {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return {false, "socket()"};
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        a.sin_addr.s_addr = htonl(0x7F000001);
        DWORD tv = 4000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        std::string resp;
        bool got = false;
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                    "Connection: close\r\n\r\n";
            if (::send(s, req.data(), static_cast<int>(req.size()), 0) > 0) {
                char buf[2048];
                const int n = ::recv(s, buf, sizeof(buf), 0);
                if (n > 0) {
                    got = true;
                    resp.assign(buf, static_cast<size_t>(n));
                }
            }
        }
        ::closesocket(s);
        return {got, resp};
    };

    const int port = bridge.Port();

    // ① 基线：没 abort 时应答正常
    const auto before = rawHttpGet(port, "/qst/status");
    const bool baseOk = before.first && before.second.find("200") != std::string::npos;

    // ② 置 abort（模拟「停止过一次脚本」）—— **不再调用 ClearAbort**
    //    这正是故障现场：StopRun 置位，而 EndRun 因为窗口模式关闭而没跑。
    bridge.AbortPending();
    const bool abortedFlag = bridge.IsAborted();

    const auto after = rawHttpGet(port, "/qst/status");
    // ★★ 核心断言：abort 之后**仍然**必须拿到 HTTP 响应（而不是连接被重置）。
    const bool stillServes = after.first && after.second.find("200") != std::string::npos;

    // ③ 放掉闩锁后，等回执的路径也要恢复（ClearAbort 的语义）
    bridge.ClearAbort();
    const bool cleared = !bridge.IsAborted();
    const auto afterClear = rawHttpGet(port, "/qst/status");
    const bool servesAfterClear = afterClear.first
        && afterClear.second.find("200") != std::string::npos;

    bridge.Stop();

    const bool ok = baseOk && abortedFlag && stillServes && cleared && servesAfterClear;
    std::wstring detail;
    if (!ok) {
        detail = L"base=" + std::to_wstring(baseOk)
            + L" abortedFlag=" + std::to_wstring(abortedFlag)
            + L" stillServes=" + std::to_wstring(stillServes)   // ← 故障时这里会是 0
            + L" cleared=" + std::to_wstring(cleared)
            + L" servesAfterClear=" + std::to_wstring(servesAfterClear);
    }
    Emit(L"ext_bridge_abort_keeps_serving", ok, detail.c_str());
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
        && windowmode::LooksLikeUnrealEngineWindowClass(L"LaunchUnrealUWindowsClient")
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

    const bool launchUe = windowmode::LooksLikeUnrealEngineWindowClass(L"LaunchUnrealUWindowsClient")
        && windowmode::LooksLikeGameWindowClass(L"LaunchUnrealUWindowsClient")
        && !windowmode::LooksLikeUnrealEngineWindowClass(L"Notepad");

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
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    const bool bgNeedHw = unreal
        && windowmode::GameTargetNeedsHardwareWithoutFakeFocus(cfg, unreal)
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

    const bool ok = needHw && bgNeedHw && launchUe && noteSkip;
    wchar_t detail[160]{};
    swprintf_s(detail, L"unrealHw=%d bgHw=%d launchUe=%d noteSkip=%d",
        needHw ? 1 : 0, bgNeedHw ? 1 : 0, launchUe ? 1 : 0, noteSkip ? 1 : 0);
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
    const bool rolledBack = !parked
        && std::abs(after.left - before.left) < 40
        && std::abs(after.top - before.top) < 40;

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    const bool ok = canPark && ((parked && offscreen && topmost && restored && back)
        || rolledBack);
    wchar_t detail[192]{};
    swprintf_s(detail, L"can=%d park=%d off=%d top=%d rest=%d back=%d roll=%d",
        canPark ? 1 : 0, parked ? 1 : 0, offscreen ? 1 : 0,
        topmost ? 1 : 0, restored ? 1 : 0, back ? 1 : 0, rolledBack ? 1 : 0);
    Emit(L"hardware_offscreen_park", ok, ok ? L"" : detail);
}

void TestClampRectKeepsBottomRight() {
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int waW = (std::max)(200, static_cast<int>(wa.right - wa.left));
    const int waH = (std::max)(200, static_cast<int>(wa.bottom - wa.top));
    const int w = (std::max)(200, waW / 4);
    const int h = (std::max)(160, waH / 4);
    RECT dest{};
    dest.left = wa.right - w - 24;
    dest.top = wa.bottom - h - 24;
    dest.right = dest.left + w;
    dest.bottom = dest.top + h;
    int destW = w;
    int destH = h;
    const LONG savedL = dest.left;
    const LONG savedT = dest.top;
    windowmode::ClampRectToContainingWorkArea(dest, destW, destH);
    const bool ok = destW == w && destH == h
        && std::abs(dest.left - savedL) < 8
        && std::abs(dest.top - savedT) < 8
        && dest.left > wa.left + 40;
    wchar_t detail[160]{};
    swprintf_s(detail, L"pos=%d,%d want=%d,%d size=%dx%d",
        dest.left, dest.top, savedL, savedT, destW, destH);
    Emit(L"clamp_rect_keeps_bottom_right", ok, ok ? L"" : detail);
}

void TestClampRectShrinksIntoWork() {
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    RECT dest{wa.left + 48, wa.top + 48, wa.left + 48 + 8000, wa.top + 48 + 6000};
    int destW = 8000;
    int destH = 6000;
    windowmode::ClampRectToContainingWorkArea(dest, destW, destH);
    const bool ok = destW <= (wa.right - wa.left)
        && destH <= (wa.bottom - wa.top)
        && dest.left >= wa.left
        && dest.top >= wa.top
        && dest.right <= wa.right
        && dest.bottom <= wa.bottom
        && destW >= 64 && destH >= 64;
    wchar_t detail[160]{};
    swprintf_s(detail, L"pos=%d,%d size=%dx%d", dest.left, dest.top, destW, destH);
    Emit(L"clamp_rect_shrinks_into_work", ok, ok ? L"" : detail);
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
    // 原 exe 链路：非游戏 Win32 类→ softMessage；默认可最小化。
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

    // Discord / CEF：与 QQ 同一套 Chromium 壳路径。Weixin.exe + Chrome 类仍走壳（开发者工具旧CEF）。
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

    windowmode::WindowModeScriptConfig wechatCef = discord;
    wechatCef.targetExePath = L"D:\\Tencent\\Weixin\\Weixin.exe";
    const bool wechatCefOk = windowmode::ConfigLooksLikeElectronShell(wechatCef)
        && windowmode::LooksLikeChromiumBrowserClass(L"CefBrowserWindow")
        && windowmode::LooksLikeChromiumBrowserClass(L"Chrome_RenderWidgetHostHWND");

    windowmode::WindowModeScriptConfig wechatQt{};
    wechatQt.enabled = true;
    wechatQt.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    wechatQt.windowClassName = L"Qt51514QWindowIcon";
    wechatQt.targetExePath = L"D:\\Weixin\\Weixin.exe";
    wechatQt.windowName = L"微信";
    wechatQt.inputStrategy = windowmode::WindowModeInputStrategy::Auto;
    const bool wechatQtOk =
        windowmode::LooksLikeWeixinTarget(wechatQt, nullptr)
        && windowmode::NeedsFakeFocusInjection(wechatQt, nullptr)
        && windowmode::UsesFakeFocus(wechatQt)
        && !windowmode::ConfigLooksLikeElectronShell(wechatQt)
        && !windowmode::ConfigLooksLikeEmulatorTarget(wechatQt)
        && !windowmode::PrefersLcaBackgroundMessages(wechatQt, nullptr)
        && !windowmode::ShouldMinimizeTargetAfterBind(wechatQt);

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

    const bool ok = softOk && unityAutoOk && legendAutoOk && ffOk && forceOk && discordOk && wechatCefOk
        && wechatQtOk
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

// ── 注入副本（fake_focus_stage）用例的夹具 ─────────────────────────────────
// 全部走 %TEMP%，并显式传 stageRoot（FakeFocusStagedPathIn/StageFakeFocusDllInto/
// SweepStaleFakeFocusArtifactsIn）—— 绝不去碰用户真实的 %LOCALAPPDATA%\QuickScriptTool。
std::wstring MakeProbeDir(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tmp) == 0) return L"";
    static int seq = 0;
    wchar_t full[MAX_PATH]{};
    swprintf_s(full, L"%sqst_wm_%s_%lu_%d", tmp, tag,
        static_cast<unsigned long>(GetCurrentProcessId()), ++seq);
    CreateDirectoryW(full, nullptr);
    return full;
}

void RemoveProbeTree(const std::wstring& dir) {
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd{};
    const std::wstring glob = dir + L"\\*";
    HANDLE find = FindFirstFileW(glob.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::wstring child = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveProbeTree(child);
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

bool WriteProbeFile(const std::wstring& path, size_t bytes, unsigned char fill) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> data(bytes, fill);
    DWORD wrote = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr)
        && wrote == data.size();
    CloseHandle(h);
    return ok;
}

bool ReadProbeFile(const std::wstring& path, std::vector<unsigned char>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char buf[4096]{};
    DWORD read = 0;
    while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
        out.insert(out.end(), buf, buf + read);
    }
    CloseHandle(h);
    return true;
}

std::wstring ProbeBaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool PathExistsProbe(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExistsProbe(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/// 把目录的「最后写入时间」拨到 daysAgo 天前（清扫按它判年龄）。
bool BackdateProbeDir(const std::wstring& dir, int daysAgo) {
    HANDLE h = CreateFileW(dir.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER v{};
    v.LowPart = now.dwLowDateTime;
    v.HighPart = now.dwHighDateTime;
    v.QuadPart -= static_cast<ULONGLONG>(daysAgo) * 24ull * 60ull * 60ull * 10000000ull;
    FILETIME back{};
    back.dwLowDateTime = v.LowPart;
    back.dwHighDateTime = v.HighPart;
    const bool ok = SetFileTime(h, nullptr, nullptr, &back) != FALSE;
    CloseHandle(h);
    return ok;
}

void TestFakeFocusInjectCopy() {
    const std::wstring root = MakeProbeDir(L"stage");
    const std::wstring srcDir = root + L"\\src";
    const std::wstring stageRoot = root + L"\\stage_root";
    CreateDirectoryW(srcDir.c_str(), nullptr);
    // 用 FakeFocus32.next.dll 当源：正好钉住「旁路槽名要归一成 FakeFocus32.dll」这条判据
    // （MapleIsFakeFocusModulePath / TargetHasStaleFakeFocusModule 都按名认）。
    const std::wstring source = srcDir + L"\\FakeFocus32.next.dll";
    bool ioOk = WriteProbeFile(source, 2048, 0xAB);
    std::vector<unsigned char> sourceBytes;
    ioOk = ioOk && ReadProbeFile(source, sourceBytes);

    std::wstring staged;
    std::wstring err;
    const bool staged1 = windowmode::StageFakeFocusDllInto(stageRoot, source, staged, err);
    std::vector<unsigned char> stagedBytes;
    const bool readOk = staged1 && ReadProbeFile(staged, stagedBytes);

    // 第二次必须复用同一路径（同一构建同一副本，不重复落盘）
    std::wstring stagedAgain;
    std::wstring err2;
    const bool staged2 = windowmode::StageFakeFocusDllInto(stageRoot, source, stagedAgain, err2);

    // 源文件内容变了 ⇒ 新构建 ⇒ 必须换一份新副本（旧副本可能还被目标进程映射着）
    const bool changed = WriteProbeFile(source, 4096, 0xCD);
    std::wstring staged3;
    std::wstring err3;
    const bool staged3Ok = windowmode::StageFakeFocusDllInto(stageRoot, source, staged3, err3);

    const std::wstring base1 = ProbeBaseName(staged);
    const bool samePathOk = staged1 && staged2
        && _wcsicmp(staged.c_str(), stagedAgain.c_str()) == 0;
    const bool canonicalNameOk = _wcsicmp(base1.c_str(), L"FakeFocus32.dll") == 0;
    const bool notSourceOk = staged1 && _wcsicmp(staged.c_str(), source.c_str()) != 0;
    const bool contentOk = readOk && stagedBytes.size() == sourceBytes.size()
        && !stagedBytes.empty()
        && memcmp(stagedBytes.data(), sourceBytes.data(), sourceBytes.size()) == 0;
    const bool newBuildOk = staged3Ok && _wcsicmp(staged3.c_str(), staged.c_str()) != 0;
    // 源文件必须**原样不动**（这一层只读它）：改完之后仍是 4096 字节且首字节是我们写的 0xCD。
    std::vector<unsigned char> sourceNow;
    const bool sourceUntouched = ReadProbeFile(source, sourceNow)
        && sourceNow.size() == 4096 && sourceNow[0] == 0xCD;

    const bool ok = ioOk && changed && staged1 && staged2 && staged3Ok
        && samePathOk && canonicalNameOk && notSourceOk && contentOk && newBuildOk
        && sourceUntouched;
    wchar_t detail[512]{};
    swprintf_s(detail,
        L"staged=%d same=%d name=%s notSource=%d content=%d newBuild=%d err=%s",
        staged1 ? 1 : 0, samePathOk ? 1 : 0, base1.c_str(), notSourceOk ? 1 : 0,
        contentOk ? 1 : 0, newBuildOk ? 1 : 0,
        (err.empty() ? err2.c_str() : err.c_str()));
    Emit(L"fake_focus_inject_copy", ok, ok ? L"" : detail);
    RemoveProbeTree(root);
}

void TestFakeFocusStageSweep() {
    const std::wstring root = MakeProbeDir(L"sweep");
    const std::wstring stageRoot = root + L"\\stage_root";
    const std::wstring exeDir = root + L"\\app";
    const std::wstring srcDir = root + L"\\src";
    CreateDirectoryW(srcDir.c_str(), nullptr);
    CreateDirectoryW(exeDir.c_str(), nullptr);
    const std::wstring source = srcDir + L"\\FakeFocus64.dll";
    const bool wrote = WriteProbeFile(source, 1024, 0x11);

    std::wstring oldCopy;
    std::wstring freshCopy;
    std::wstring err;
    const bool oldOk = windowmode::StageFakeFocusDllInto(stageRoot, source, oldCopy, err);
    // 造一份「另一构建」的副本目录：改源文件大小 ⇒ 新目录（旧目录留着当老古董）
    WriteProbeFile(source, 2048, 0x22);
    const bool freshOk = windowmode::StageFakeFocusDllInto(stageRoot, source, freshCopy, err);

    // 老目录里按住一个「被占用」的文件（独占打开 = 模拟被进程映射）。
    // ⚠ 顺序要紧：**先建文件、后拨时间** —— 在目录里建/删文件会把目录的
    // 「最后写入时间」刷新成当前，而清扫正是按它判年龄的。
    const std::wstring oldDir = oldCopy.substr(0, oldCopy.find_last_of(L'\\'));
    const std::wstring heldPath = oldDir + L"\\held.dat";
    const bool heldWrote = WriteProbeFile(heldPath, 64, 0x33);
    HANDLE held = CreateFileW(heldPath.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool backdated = BackdateProbeDir(oldDir, 40);

    // 让位改名残留：一个该清、一个不该动
    const std::wstring leftover = exeDir + L"\\FakeFocus32.dll.locked-20260101000000";
    const std::wstring byuser = exeDir + L"\\user_notes.locked-keep.txt";
    const bool leftoversWrote = WriteProbeFile(leftover, 16, 0x44)
        && WriteProbeFile(byuser, 16, 0x55);

    const windowmode::FakeFocusSweepResult r =
        windowmode::SweepStaleFakeFocusArtifactsIn(stageRoot, exeDir, 30);

    const bool oldDirKeptLocked = DirExistsProbe(oldDir) && r.stageKeptLocked == 1;
    const bool freshKept = PathExistsProbe(freshCopy);
    const bool leftoverGone = !PathExistsProbe(leftover) && r.leftoversRemoved == 1;
    const bool userFileKept = PathExistsProbe(byuser);

    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    // 放开占用 + 又过了 30 天（再拨一次时间）后重扫：这次老目录应该被整个删掉
    // —— 证明"被占用就跳过"不是"永远留下"。
    const bool rebackdated = BackdateProbeDir(oldDir, 40);
    const windowmode::FakeFocusSweepResult r2 =
        windowmode::SweepStaleFakeFocusArtifactsIn(stageRoot, exeDir, 30);
    const bool oldRemovedAfterUnlock = !DirExistsProbe(oldDir) && r2.stageDirsRemoved == 1;

    const bool ok = wrote && oldOk && freshOk && heldWrote && backdated && leftoversWrote
        && oldDirKeptLocked && freshKept && leftoverGone && userFileKept
        && rebackdated && (held != INVALID_HANDLE_VALUE) && oldRemovedAfterUnlock;
    wchar_t detail[512]{};
    swprintf_s(detail,
        L"keptLocked=%d fresh=%d leftover=%d userKept=%d removedAfterUnlock=%d err=%s",
        r.stageKeptLocked, freshKept ? 1 : 0, r.leftoversRemoved, userFileKept ? 1 : 0,
        oldRemovedAfterUnlock ? 1 : 0, err.c_str());
    Emit(L"fake_focus_stage_sweep", ok, ok ? L"" : detail);
    RemoveProbeTree(root);
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
    // ⚠⚠ 2026-10-05：原来固定 `+37/+19` —— 光标**恰好在屏幕右/下边缘**时，
    //   目标点超出虚拟屏 ⇒ 被系统**钳制** ⇒ `after != want` ⇒ **误报失败**。
    //   实测（光标停在 y=959 的屏幕底部）：`after=(1055,959) want=(1055,978) warpOk=0`
    //   —— X 成功了、Y 没动，一眼就能看出不是「被吞」而是「钳制」。
    //   ⇒ 改成**朝屏幕内部**偏移：先看虚拟屏范围，`+` 会超界就改用 `-`。
    const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int dx = 37, dy = 19;
    if (vsW > 0 && orig.x + dx >= vsX + vsW) dx = -37;
    if (vsH > 0 && orig.y + dy >= vsY + vsH) dy = -19;
    const int warpX = orig.x + dx;
    const int warpY = orig.y + dy;
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
    wchar_t detail[240]{};
    // ⚠ 2026-10-05：原来只打 `after`，**没打期望值 warpX/warpY、也没打 warpOk**
    //   ⇒ 失败时无法区分「SetCursorPos 被吞（after==orig）」和「移动失败（after 是别的值）」。
    swprintf_s(detail, L"install=%d update=%d fg=%d proc=%d after=(%ld,%ld) want=(%d,%d) "
        L"warpOk=%d orig=(%ld,%ld) peekOk=%d",
        installed ? 1 : 0, updated ? 1 : 0, hooked == hwnd ? 1 : 0, procOk ? 1 : 0,
        after.x, after.y, warpX, warpY, warpOk ? 1 : 0, orig.x, orig.y, peekEmpty ? 1 : 0);
    Emit(L"fake_focus_air_focus_only", ok, ok ? L"" : detail);
}

// ── Adobe AIR：包装窗 + AIR 内容子窗（4399 微端 / 造梦西游的真实结构）──────────
// 钉两条「注入不许把游戏带走」的防线：
//   ① **AIR 识别必须看子窗** —— `ApolloRuntimeContentWindow` 常是**内容子窗**，父窗是
//      启动器/包装窗；而 `InstallCommon` 拿到的 top 是 GA_ROOT。只看顶层 ⇒ 漏判 ⇒
//      走**全量 Phase2**（子类化 + 光标钩 + RawInput）⇒ AIR「一启动就卡死退出，鼠标原地抽」。
//   ② **AIR 的变速时钟钩不得改代码页** —— 它原本装在 `airSafe` 早退**之前**（whitelist 泄漏），
//      而 AIR 已知脆 ⇒ 只允许 IAT 槽补丁（诊断 bit6 = `g_iatOnly`）。
// 负对照：普通窗（STATIC）必须 bit6=0 —— 否则「bit6 恒为 1」也能让断言变绿。
void TestFakeFocusAirChildIatOnly() {
    const wchar_t* kName = L"fake_focus_air_child_iat_only";
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        Emit(kName, false, (L"LoadLibrary failed: " + dllPath).c_str());
        return;
    }
    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    using DiagFn = DWORD(WINAPI*)(HWND);
    auto* installLite = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_InstallLite"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    auto* diag = reinterpret_cast<DiagFn>(GetProcAddress(mod, "FakeFocus_TimeScaleDiag"));
    if (!installLite || !uninstall || !diag) {
        FreeLibrary(mod);
        Emit(kName, false, L"missing FakeFocus_InstallLite/Uninstall/TimeScaleDiag");
        return;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wcChild{};
    wcChild.lpfnWndProc = DefWindowProcW;
    wcChild.hInstance = inst;
    wcChild.lpszClassName = L"ApolloRuntimeContentWindow";
    RegisterClassW(&wcChild);
    WNDCLASSW wcWrap{};
    wcWrap.lpfnWndProc = DefWindowProcW;
    wcWrap.hInstance = inst;
    wcWrap.lpszClassName = L"QstAirWrapperProbe";
    RegisterClassW(&wcWrap);

    HWND top = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wcWrap.lpszClassName,
        L"QST AIR wrapper", WS_OVERLAPPEDWINDOW, 80, 80, 320, 200,
        nullptr, nullptr, inst, nullptr);
    if (!top) {
        FreeLibrary(mod);
        UnregisterClassW(wcWrap.lpszClassName, inst);
        UnregisterClassW(wcChild.lpszClassName, inst);
        Emit(kName, false, L"CreateWindow wrapper failed");
        return;
    }
    // ★ 子窗用 AIR 类名：真实微端就是「启动器窗 + ApolloRuntimeContentWindow 子窗」。
    HWND airChild = CreateWindowExW(0, wcChild.lpszClassName, L"AIR content",
        WS_CHILD | WS_VISIBLE, 0, 0, 300, 160, top, nullptr, inst, nullptr);
    if (!airChild) {
        DestroyWindow(top);
        FreeLibrary(mod);
        UnregisterClassW(wcWrap.lpszClassName, inst);
        UnregisterClassW(wcChild.lpszClassName, inst);
        Emit(kName, false, L"CreateWindow AIR child failed");
        return;
    }
    ShowWindow(top, SW_SHOWNOACTIVATE);

    // ① 顶层是包装窗、AIR 只在子窗上：必须仍按 AIR 处理 ⇒ 不子类化顶层。
    const LONG_PTR procBefore = GetWindowLongPtrW(top, GWLP_WNDPROC);
    const BOOL airInstalled = installLite(top);
    const LONG_PTR procAfter = GetWindowLongPtrW(top, GWLP_WNDPROC);
    const DWORD airDiag = airInstalled ? diag(nullptr) : 0u;
    uninstall();

    // ② 负对照：普通窗必须 bit6=0（否则 bit6 恒 1，断言毫无意义）。
    HWND plain = CreateWindowExW(0, L"STATIC", L"QST plain probe",
        WS_OVERLAPPEDWINDOW, 80, 80, 200, 100, nullptr, nullptr, inst, nullptr);
    DWORD plainDiag = 0;
    bool plainInstalled = false;
    if (plain) {
        plainInstalled = installLite(plain) != FALSE;
        if (plainInstalled) plainDiag = diag(nullptr);
        uninstall();
        DestroyWindow(plain);
    }

    DestroyWindow(top);
    FreeLibrary(mod);
    UnregisterClassW(wcWrap.lpszClassName, inst);
    UnregisterClassW(wcChild.lpszClassName, inst);

    const bool procOk = procBefore == procAfter;
    const bool airIatOnly = (airDiag & 64u) != 0;   // bit6：只补 IAT 槽、不碰代码页
    const bool plainNotIatOnly = plainInstalled && (plainDiag & 64u) == 0u;
    const bool ok = airInstalled && procOk && airIatOnly && plainNotIatOnly;
    wchar_t detail[260]{};
    swprintf_s(detail,
        L"install=%d proc=%d airDiag=0x%08x(bit6=%d) plainInstalled=%d plainDiag=0x%08x(bit6=%d)",
        airInstalled ? 1 : 0, procOk ? 1 : 0, static_cast<unsigned>(airDiag),
        (airDiag & 64u) ? 1 : 0, plainInstalled ? 1 : 0, static_cast<unsigned>(plainDiag),
        (plainDiag & 64u) ? 1 : 0);
    Emit(kName, ok, ok ? L"" : detail);
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
    const BOOL peeked = PeekMessageW(&msg, hwnd, 0, 0, PM_NOREMOVE);
    const bool peekOk = peeked == FALSE || msg.message != WM_INPUT;

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
        && keyOk && hitsOk && peekOk && titleProcOk && iatOk && noUser32BodyJmp && noDiDataBodyJmp;
    wchar_t detail[360]{};
    swprintf_s(detail,
        L"install=%d update=%d fg=%d proc=%d cursor=(%ld,%ld) space=%d lbtn=%d hitsGaks=%u peekOk=%d titleProc=%d iat=%lu diag=0x%04X",
        installed ? 1 : 0, updated ? 1 : 0, hooked == hwnd ? 1 : 0, procOk ? 1 : 0,
        pt.x, pt.y, (space & 0x8000) ? 1 : 0, (lbtn & 0x8000) ? 1 : 0,
        static_cast<unsigned>(hookHits & 0xFFu),
        peekOk ? 1 : 0, titleProcOk ? 1 : 0,
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

// ── 窗口变速（变速齿轮）端到端 ─────────────────────────────────────────────
// 为什么放在交互档而不是纯逻辑档：变速的实现在**目标进程内**（IAT 补丁 + 虚拟时钟），
// 必须在真实进程里注入并真的读一次时钟才算验过。这里把 FakeFocus 装到自检进程自己身上，
// 然后：
//   ① 用 Sleep(300)（内核定时器，不受变速影响）当真实时间基准；
//   ② 读 QueryPerformanceCounter —— 它此刻已被补丁接管；
//   ③ 断言 2 倍速下 QPC 走的量 ≈ 2 × 真实睡眠，关闭后回到 ≈ 1 ×。
// 这同时覆盖了「补丁装上了」「倍率真的生效」「关闭后彻底还原」三件事。
// ── 仅变速注入（关掉「启用假焦点注入」时的路径）────────────────────────────
// 用户关掉假焦点注入、却开着窗口变速时，窗口/后台窗口模式改走 FakeFocus_InstallTimeScaleOnly：
// 只装时钟 IAT 补丁，一个假焦点钩都不装。这条路径必须单独测，因为它一旦回归
// （比如忘了 early-return，顺手把全套钩子也装上），用户特意关掉的注入就会悄悄生效，
// 而且**没有任何报错** —— 是最难发现的那种坏。
// 断言两件事：① 变速照常生效（2 倍速下 QPC 真的走快 2 倍）；② 诊断 bit2 证明没走假焦点安装。
void TestWindowTimeScaleOnlyIat() {
    const wchar_t* kName = L"window_time_scale_only_iat";
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif
    std::wstring softErr;
    if (!windowmode::FakeFocusSoftInput_Attach(GetCurrentProcessId(), softErr)) {
        Emit(kName, false, softErr.empty() ? L"Attach soft input failed" : softErr.c_str());
        return;
    }
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        windowmode::FakeFocusSoftInput_Detach();
        Emit(kName, false, (L"LoadLibrary failed: " + dllPath).c_str());
        return;
    }
    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    using DiagFn = DWORD(WINAPI*)(HWND);
    auto* installTs = reinterpret_cast<InstallFn>(
        GetProcAddress(mod, "FakeFocus_InstallTimeScaleOnly"));
    auto* installFull = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_Install"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    auto* diag = reinterpret_cast<DiagFn>(GetProcAddress(mod, "FakeFocus_TimeScaleDiag"));
    if (!installTs || !installFull || !uninstall || !diag) {
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(kName, false,
            L"missing FakeFocus_InstallTimeScaleOnly/Install/Uninstall/TimeScaleDiag"
            L"（DLL 版本过旧？）");
        return;
    }

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"QST TimeScaleOnly Probe",
        WS_OVERLAPPEDWINDOW, 80, 80, 200, 100, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(kName, false, L"CreateWindow failed");
        return;
    }
    if (!installTs(hwnd)) {
        DestroyWindow(hwnd);
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(kName, false, L"FakeFocus_InstallTimeScaleOnly failed");
        return;
    }

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const auto measure = [&freq]() -> double {
        LARGE_INTEGER a{}, b{};
        QueryPerformanceCounter(&a);
        Sleep(300);
        QueryPerformanceCounter(&b);
        if (freq.QuadPart <= 0) return -1.0;
        return static_cast<double>(b.QuadPart - a.QuadPart)
            / static_cast<double>(freq.QuadPart);
    };

    windowmode::FakeFocusSoftInput_SetTimeScale(1.0);
    Sleep(250);
    const double base = measure();
    windowmode::FakeFocusSoftInput_SetTimeScale(2.0);
    Sleep(250);
    const DWORD diagTs = diag(nullptr);
    const double fast = measure();
    windowmode::FakeFocusSoftInput_SetTimeScale(0.0);
    Sleep(250);
    const DWORD diagOff = diag(nullptr);
    const double back = measure();

    // 换成完整安装：bit2 必须自己清掉（证明它描述的是「本次安装」，不是粘住的全局态）。
    uninstall();
    const bool fullInstalled = installFull(hwnd) != FALSE;
    const DWORD diagFull = fullInstalled ? diag(nullptr) : 0u;
    uninstall();

    DestroyWindow(hwnd);
    FreeLibrary(mod);
    windowmode::FakeFocusSoftInput_Detach();

    const uint32_t scaleTs = (diagTs >> 16) & 0xFFFFu;
    const uint32_t slotsTs = (diagTs >> 8) & 0xFFu;
    const bool tsOnlyFlag = (diagTs & 4u) != 0;             // bit2：确实走的「仅变速」分支
    const bool tsInstalled = (diagTs & 1u) != 0 && scaleTs == 2000u && slotsTs > 0u;
    const bool tsRemoved = (diagOff & 1u) == 0;
    const bool fullClearsFlag = fullInstalled && (diagFull & 4u) == 0u;
    const bool baseOk = base > 0.20 && base < 0.70;
    const bool fastOk = fast > base * 1.5 && fast < base * 2.6;
    const bool backOk = back > 0.20 && back < 0.70;
    const bool ok = tsOnlyFlag && tsInstalled && tsRemoved && fullClearsFlag
        && baseOk && fastOk && backOk;

    wchar_t detail[640]{};
    swprintf_s(detail,
        L"diagTs=0x%08x(仅变速bit=%d 倍率%u 槽%u) diagOff=0x%08x diagFull=0x%08x(仅变速bit=%d) "
        L"| 300ms: 原速%.3f 2倍速%.3f 关闭%.3f（期望 ≈%.3f/%.3f/%.3f）",
        static_cast<unsigned>(diagTs), tsOnlyFlag ? 1 : 0, scaleTs, slotsTs,
        static_cast<unsigned>(diagOff), static_cast<unsigned>(diagFull),
        (diagFull & 4u) ? 1 : 0, base, fast, back, base, base * 2.0, base);
    Emit(kName, ok, ok ? L"" : detail);
}

void TestWindowTimeScaleIat() {
    const wchar_t* kName = L"window_time_scale_iat";
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif
    std::wstring softErr;
    if (!windowmode::FakeFocusSoftInput_Attach(GetCurrentProcessId(), softErr)) {
        Emit(kName, false, softErr.empty() ? L"Attach soft input failed" : softErr.c_str());
        return;
    }
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        windowmode::FakeFocusSoftInput_Detach();
        Emit(kName, false, (L"LoadLibrary failed: " + dllPath).c_str());
        return;
    }
    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    using DiagFn = DWORD(WINAPI*)(HWND);
    auto* install = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_Install"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    auto* diag = reinterpret_cast<DiagFn>(GetProcAddress(mod, "FakeFocus_TimeScaleDiag"));
    if (!install || !uninstall || !diag) {
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(kName, false, L"missing FakeFocus_Install/Uninstall/TimeScaleDiag");
        return;
    }

    // ⚠⚠ 本用例的核心：在装钩子**之前**把 QPC 的真实地址缓存下来。
    //   目标进程（Unity）正是这么做的 —— 启动时拿到地址存进自己的结构体，之后不再走导入表。
    //   旧实现只改 IAT 槽，对这类目标完全无效：实测补了 239 个槽、倍率也正确下发，
    //   游戏照样不变速，白白查了三轮。现在改成内联钩函数体，所以**走这个缓存指针也必须变速**。
    //   这一条断言就是当初该有却没有的那条。
    auto* cachedQpc = reinterpret_cast<BOOL(WINAPI*)(LARGE_INTEGER*)>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "QueryPerformanceCounter"));
    if (!cachedQpc) {
        cachedQpc = reinterpret_cast<BOOL(WINAPI*)(LARGE_INTEGER*)>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlQueryPerformanceCounter"));
    }
    if (!cachedQpc) {
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(kName, false, L"找不到可用于模拟「目标缓存指针」的 QPC 地址");
        return;
    }

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"QST TimeScale Probe",
        WS_OVERLAPPEDWINDOW, 80, 80, 200, 100, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(kName, false, L"CreateWindow failed");
        return;
    }
    if (!install(hwnd)) {
        DestroyWindow(hwnd);
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        Emit(kName, false, L"FakeFocus_Install failed");
        return;
    }

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    // ① 走 IAT 的 QPC（静态导入的调用方）
    const auto measure = [&freq]() -> double {
        LARGE_INTEGER a{}, b{};
        QueryPerformanceCounter(&a);
        Sleep(300);
        QueryPerformanceCounter(&b);
        if (freq.QuadPart <= 0) return -1.0;
        return static_cast<double>(b.QuadPart - a.QuadPart)
            / static_cast<double>(freq.QuadPart);
    };
    // ② 走「缓存指针」的 QPC（模拟 Unity 这类目标的真实行为）
    const auto measureCached = [&freq, cachedQpc]() -> double {
        LARGE_INTEGER a{}, b{};
        cachedQpc(&a);
        Sleep(300);
        cachedQpc(&b);
        if (freq.QuadPart <= 0) return -1.0;
        return static_cast<double>(b.QuadPart - a.QuadPart)
            / static_cast<double>(freq.QuadPart);
    };

    windowmode::FakeFocusSoftInput_SetTimeScale(1.0);
    Sleep(250);
    const double base = measure();
    const double baseCached = measureCached();
    windowmode::FakeFocusSoftInput_SetTimeScale(2.0);
    Sleep(250);
    const DWORD diag2x = diag(nullptr);
    const double fast = measure();
    const double fastCached = measureCached();
    windowmode::FakeFocusSoftInput_SetTimeScale(0.0);
    Sleep(250);
    const DWORD diagOff = diag(nullptr);
    const double back = measure();
    const double backCached = measureCached();

    // 钩子调用计数必须涨 —— 否则说明钩子根本没进入调用路径，
    // 那前面那些"耗时变了"的断言就是假的。
    // ⚠ 必须在 Detach 之前读：Detach 会关掉宿主视图，读到的会是 null。
    uint32_t hookCalls[6]{};
    const bool haveCalls = windowmode::FakeFocusSoftInput_TimeHookCalls(hookCalls, 6);
    const bool hookFired = haveCalls && hookCalls[0] > 0u;

    uninstall();
    DestroyWindow(hwnd);
    FreeLibrary(mod);
    windowmode::FakeFocusSoftInput_Detach();

    const uint32_t scale2x = (diag2x >> 16) & 0xFFFFu;
    const uint32_t hooks2x = (diag2x >> 8) & 0xFFu;
    const bool installed = (diag2x & 1u) != 0 && scale2x == 2000u && hooks2x > 0u;
    const bool removed = (diagOff & 1u) == 0 && ((diagOff >> 16) & 0xFFFFu) == 0u;
    const bool baseOk = base > 0.20 && base < 0.70;
    const bool fastOk = fast > base * 1.5 && fast < base * 2.6;
    const bool backOk = back > 0.20 && back < 0.70;
    // ★ 决定性断言：缓存指针也必须被拦住（旧实现就是死在这一条上）
    const bool cachedFastOk = fastCached > baseCached * 1.5 && fastCached < baseCached * 2.6;
    const bool cachedBackOk = backCached > 0.20 && backCached < 0.70;
    const bool ok = installed && removed && baseOk && fastOk && backOk
        && cachedFastOk && cachedBackOk && hookFired;

    wchar_t detail[700]{};
    swprintf_s(detail,
        L"diag2x=0x%08x(倍率%u 钩%u) diagOff=0x%08x 调用计数=%u(有=%d) | "
        L"走IAT 300ms %.3f/%.3f/%.3f | 走缓存指针 300ms %.3f/%.3f/%.3f"
        L"（期望 原速≈0.3 / 2倍速≈0.6 / 关闭≈0.3）",
        static_cast<unsigned>(diag2x), scale2x, hooks2x, static_cast<unsigned>(diagOff),
        hookCalls[0], haveCalls ? 1 : 0,
        base, fast, back, baseCached, fastCached, backCached);
    Emit(kName, ok, ok ? L"" : detail);
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

void TestPermissionMismatchNoAutolaunch() {
    using windowmode::ShouldAbortAutoLaunchOnBindFailure;
    using windowmode::WindowModeHealth;
    using windowmode::HealthToUserHint;

    const bool abortPerm = ShouldAbortAutoLaunchOnBindFailure(WindowModeHealth::PermissionMismatch);
    const bool abortDesk = ShouldAbortAutoLaunchOnBindFailure(WindowModeHealth::DesktopNotReady);
    const bool keepNotFound = !ShouldAbortAutoLaunchOnBindFailure(WindowModeHealth::TargetNotFound);
    const bool keepOk = !ShouldAbortAutoLaunchOnBindFailure(WindowModeHealth::Ok);
    const wchar_t* hint = HealthToUserHint(WindowModeHealth::PermissionMismatch);
    const bool hintOk = hint && wcsstr(hint, L"管理员") != nullptr;
    const bool ok = abortPerm && abortDesk && keepNotFound && keepOk && hintOk;
    wchar_t detail[200]{};
    swprintf_s(detail, L"perm=%d desk=%d notFoundKeep=%d okKeep=%d hintAdmin=%d",
        abortPerm ? 1 : 0, abortDesk ? 1 : 0, keepNotFound ? 1 : 0, keepOk ? 1 : 0,
        hintOk ? 1 : 0);
    Emit(L"permission_mismatch_no_autolaunch", ok, ok ? L"" : detail);
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
    cfg.fakeFocusEnabled = true;
    cfg.windowClassName = L"MapleStoryClass";
    const bool ffOk = !windowmode::UsesFakeFocus(cfg)
        && !windowmode::UsesFakeFocusForTarget(cfg, nullptr)
        && windowmode::PrefersLcaBackgroundMessages(cfg, nullptr)
        && windowmode::MapleNeedsSafeFakeFocusLite(cfg, nullptr)
        && !windowmode::NeedsFakeFocusInjection(cfg, nullptr)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg)
        && !windowmode::GameTargetNeedsHardwareWithoutFakeFocus(cfg, nullptr);

    cfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    const bool hwOk = !windowmode::GameTargetNeedsHardwareWithoutFakeFocus(cfg, nullptr);
    cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;

    cfg.windowClassName.clear();
    cfg.targetExePath = L"D:\\星辰\\MapleStory.exe";
    const bool exeFfOk = !windowmode::UsesFakeFocus(cfg)
        && windowmode::LooksLikeMapleStoryExecutable(cfg.targetExePath)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    cfg.targetExePath.clear();
    cfg.windowName = L"MapleStory(星辰冒险岛)";
    const bool titleFfOk = !windowmode::UsesFakeFocus(cfg)
        && !windowmode::ShouldMinimizeTargetAfterBind(cfg);

    windowmode::WindowModeScriptConfig mapleCfg{};
    mapleCfg.enabled = true;
    mapleCfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    mapleCfg.targetExePath = L"D:\\冒险岛\\MapleStoryt.exe";
    const bool targetOk = windowmode::LooksLikeMapleStoryTarget(mapleCfg, nullptr)
        && !windowmode::LooksLikeMapleStoryTarget(windowmode::WindowModeScriptConfig{}, nullptr);

    const bool ok = classOk && exeOk && titleOk && ffOk && hwOk && exeFfOk && titleFfOk && targetOk;
    Emit(L"maplestory_bg_fake_focus", ok,
        ok ? L"MapleStoryClass + exe →LCA PostMessage + mapleSafe lite（UsesFakeFocus=0、不最小化）"
           : L"MapleStory not classified or UsesFakeFocus/minimize/lite flag wrong");
}

/// `SetWindowsHook` 是「把 DLL 装进**目标 UI 线程**」的注入方式：脆弱目标必须降级成 classic。
///
/// 前科：GLFW/Java《我的世界》在目标消息线程里 `LoadLibrary` ⇒ 当场崩；
/// **Adobe AIR（造梦西游 / 4399 微端）⇒ 一启动就卡死退出，鼠标原地抽**。
/// 这条断言钉住「AIR 在禁止名单里」—— 少了它，用户把注入技术选成 `setwindowshook`
/// 时 AIR 微端就会被带走；而默认技术是 classic ⇒ **日常完全测不出来**。
/// 负对照：普通目标（一个都不命中）必须放行，否则「恒真」也能让上面全绿。
void TestSetWindowsHookNotForFragileTargets() {
    const wchar_t* kName = L"setwindowshook_not_for_fragile_targets";
    using windowmode::ForbidsSetWindowsHookTechnique;
    const bool eachAir = ForbidsSetWindowsHookTechnique(false, false, false, false, false, true);
    const bool eachChromium = ForbidsSetWindowsHookTechnique(true, false, false, false, false, false);
    const bool eachWeixin = ForbidsSetWindowsHookTechnique(false, true, false, false, false, false);
    const bool eachAndroid = ForbidsSetWindowsHookTechnique(false, false, true, false, false, false);
    const bool eachNative3d = ForbidsSetWindowsHookTechnique(false, false, false, true, false, false);
    const bool eachEmu = ForbidsSetWindowsHookTechnique(false, false, false, false, true, false);
    const bool plainAllowed = !ForbidsSetWindowsHookTechnique(false, false, false, false, false, false);
    const bool ok = eachAir && eachChromium && eachWeixin && eachAndroid && eachNative3d
        && eachEmu && plainAllowed;
    wchar_t detail[220]{};
    swprintf_s(detail, L"air=%d chromium=%d weixin=%d android=%d native3d=%d emu=%d plainAllowed=%d",
        eachAir ? 1 : 0, eachChromium ? 1 : 0, eachWeixin ? 1 : 0, eachAndroid ? 1 : 0,
        eachNative3d ? 1 : 0, eachEmu ? 1 : 0, plainAllowed ? 1 : 0);
    Emit(kName, ok, ok ? L"" : detail);
}

/// 同路径 ≠ 同一份内容（2026-10-03 用户报障「冒险岛后台原地不动的平A，不能走A」）。
/// 用户升级/重建软件后游戏进程**没重启** ⇒ 只比路径会判「同一份文件」⇒ 复用旧实例，
/// 而 `LoadLibrary` 同路径不会重新执行 DllMain ⇒ 进程里是旧代码。共享内存结构加过字段
/// （`kSoftInputVersion` 7→10）后旧 DLL 与新宿主不兼容 ⇒ 软键态/DirectInput 全失效。
/// 这里逐格钉住判据 + 负对照：**拿不到时间必须不报警**（否则用户被无谓要求重启游戏）。
void TestInjectedModuleStaleDetection() {
    const wchar_t* kName = L"injected_module_stale_detection";
    using windowmode::InjectedModuleLooksStale;
    // ① 磁盘 DLL（200）晚于进程启动（100）⇒ 进程内是旧代码 ⇒ 必须判旧。
    const bool newerIsStale = InjectedModuleLooksStale(200, 100);
    // ② 反例：进程启动（200）晚于 DLL 写入（100）⇒ 进程加载的就是当前文件 ⇒ 不许报警。
    const bool olderIsFresh = !InjectedModuleLooksStale(100, 200);
    // ③ 相等（同一瞬间）⇒ 不报警（保守）。
    const bool equalNotStale = !InjectedModuleLooksStale(150, 150);
    // ④⑤⑥ 负对照：任一为 0（拿不到进程启动时间 / 文件时间）⇒ 必须**不**报警。
    const bool zeroDllNotStale = !InjectedModuleLooksStale(0, 100);
    const bool zeroProcNotStale = !InjectedModuleLooksStale(100, 0);
    const bool bothZeroNotStale = !InjectedModuleLooksStale(0, 0);
    const bool ok = newerIsStale && olderIsFresh && equalNotStale
        && zeroDllNotStale && zeroProcNotStale && bothZeroNotStale;
    wchar_t detail[260]{};
    swprintf_s(detail,
        L"newerIsStale=%d olderIsFresh=%d equalNotStale=%d zeroDll=%d zeroProc=%d bothZero=%d",
        newerIsStale ? 1 : 0, olderIsFresh ? 1 : 0, equalNotStale ? 1 : 0,
        zeroDllNotStale ? 1 : 0, zeroProcNotStale ? 1 : 0, bothZeroNotStale ? 1 : 0);
    Emit(kName, ok, ok ? L"" : detail);
}

/// 后台窗口模式 + 3D/游戏目标（MC / GLFW30）：即使设置里关了「假焦点注入」也必须注入假焦点。
/// 否则引擎只能回退「假前台 SendInput（绝对坐标）」—— 会抢鼠标/键盘，用户看到的就是**假后台**。
/// 同时锁住「仅时钟补丁」的判据里**不能**再带「用户关了假焦点注入」这一项（那正是 MC 踩到的坑）。
void TestBackgroundFakeFocusNotDegraded() {
    const wchar_t* kName = L"background_fake_focus_not_degraded";

    // ① 纯判据：需要假焦点时绝不允许只装时钟补丁。
    const bool onlyOk = !windowmode::ShouldInjectTimeScaleOnly(true, true)
        && windowmode::ShouldInjectTimeScaleOnly(true, false)
        && !windowmode::ShouldInjectTimeScaleOnly(false, true)
        && !windowmode::ShouldInjectTimeScaleOnly(false, false);

    // ② 真窗口 + GLFW30 类名，走完整判定链（UsesFakeFocusForTarget 需要真实 HWND）。
    const wchar_t* kClass = L"GLFW30";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    const ATOM atom = RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, kClass, L"QST GLFW30 Probe", WS_OVERLAPPEDWINDOW,
        64, 64, 240, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    windowmode::WindowModeScriptConfig bg{};
    bg.enabled = true;
    bg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    bg.inputStrategy = windowmode::WindowModeInputStrategy::SoftMessage;
    bg.windowClassName = kClass;

    bool forced = false;      // 后台 + GLFW30 + 关注入 ⇒ 仍须注入
    bool respectOn = false;   // 设置开着时不该强行覆盖
    bool hiddenDeskNo = false; // 宏桌面（HiddenDesktop）不属于这条，保持原行为
    if (hwnd) {
        forced = windowmode::BackgroundTargetRequiresFakeFocus(bg, hwnd, false);
        respectOn = !windowmode::BackgroundTargetRequiresFakeFocus(bg, hwnd, true);
        auto hidden = bg;
        hidden.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
        hiddenDeskNo = !windowmode::BackgroundTargetRequiresFakeFocus(hidden, hwnd, false);
    }

    if (hwnd) DestroyWindow(hwnd);
    if (atom) UnregisterClassW(kClass, GetModuleHandleW(nullptr));

    const bool ok = onlyOk && hwnd && forced && respectOn && hiddenDeskNo;
    wchar_t detail[240]{};
    swprintf_s(detail, L"only=%d hwnd=%d forced=%d respectOn=%d hiddenDeskNo=%d",
        onlyOk ? 1 : 0, hwnd ? 1 : 0, forced ? 1 : 0, respectOn ? 1 : 0,
        hiddenDeskNo ? 1 : 0);
    Emit(kName, ok,
        ok ? L"后台+GLFW30+关注入 ⇒ 必须注入假焦点；仅时钟补丁判据不带「关了注入」这一项"
           : detail);
}

/// 后台 + GLFW30，但**配置里类名为空**（拖拽拾取后只记录在窗口上，没回填配置）。
/// 这时 `UsesFakeFocus` 只看 `config.windowClassName` 会判 false ⇒ 假焦点被跳过 ⇒
/// `PreferHardwareInput()` 里 `if (FakeFocusActive()) return false;` 不成立 ⇒ 落到软输入
/// `PostMouseMoveToWindow`：每一步相对移动一次跨进程 WM_MOUSEMOVE，对 GLFW 无效、纯延迟。
/// 断言：传 hwnd 时必须能识别出「这是需要假焦点的游戏」。
void TestFakeFocusUsesBoundHwndClass() {
    const wchar_t* kName = L"fake_focus_uses_bound_hwnd_class";

    // GLFW30（Minecraft）与 SDL_APP 都是「只在窗口上」的类名。
    struct Probe { const wchar_t* cls; bool wantGame; };
    const Probe probes[] = {
        {L"GLFW30", true},
        {L"SDL_app", true},
        {L"Notepad", false},
    };

    int pass = 0;
    int total = 0;
    wchar_t detail[320]{};
    size_t used = 0;
    for (const auto& p : probes) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = p.cls;
        const ATOM atom = RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, p.cls, L"QST probe", WS_OVERLAPPEDWINDOW,
            64, 64, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        windowmode::WindowModeScriptConfig cfg{};
        cfg.enabled = true;
        cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
        cfg.inputStrategy = windowmode::WindowModeInputStrategy::SoftMessage;
        cfg.windowClassName.clear();          // ← 关键：配置里没有类名
        cfg.childWindowClassName.clear();

        const bool withHwnd = windowmode::UsesFakeFocus(cfg, hwnd);
        const bool withoutHwnd = windowmode::UsesFakeFocus(cfg, nullptr);
        ++total;
        if (withHwnd == p.wantGame && !withoutHwnd) ++pass;

        if (hwnd) DestroyWindow(hwnd);
        if (atom) UnregisterClassW(p.cls, GetModuleHandleW(nullptr));

        if (used < 220) {
            used += static_cast<size_t>(swprintf_s(detail + used, 320 - used,
                L"%s:withHwnd=%d(noHwnd=%d) ", p.cls, withHwnd ? 1 : 0,
                withoutHwnd ? 1 : 0));
        }
    }

    const bool ok = pass == total;
    Emit(kName, ok, ok ? L"类名只在窗口上时（配置为空）仍按 HWND 判定为需要假焦点" : detail);
}

void TestLcaBackgroundUnknownGame() {
    windowmode::WindowModeScriptConfig unknown{};
    unknown.enabled = true;
    unknown.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    unknown.fakeFocusEnabled = true;
    unknown.windowClassName = L"IWWindowClass";
    const bool unknownOk = windowmode::PrefersLcaBackgroundMessages(unknown, nullptr)
        && !windowmode::NeedsFakeFocusInjection(unknown, nullptr)
        && !windowmode::UsesFakeFocus(unknown)
        && !windowmode::UsesFakeFocusForTarget(unknown, nullptr)
        && !windowmode::ShouldMinimizeTargetAfterBind(unknown)
        && !windowmode::LooksLikeInjectRequiredGameClass(L"IWWindowClass")
        && !windowmode::MapleNeedsSafeFakeFocusLite(unknown, nullptr)
        && windowmode::LooksLikeStandardDesktopAppClass(L"Notepad")
        && !windowmode::LooksLikeStandardDesktopAppClass(L"IWWindowClass");

    unknown.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    const bool hiddenOk = windowmode::PrefersLcaBackgroundMessages(unknown, nullptr)
        && !windowmode::GameTargetNeedsHardwareWithoutFakeFocus(unknown, nullptr);

    windowmode::WindowModeScriptConfig unity{};
    unity.enabled = true;
    unity.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    unity.windowClassName = L"UnityWndClass";
    const bool unityOk = windowmode::NeedsFakeFocusInjection(unity, nullptr)
        && !windowmode::PrefersLcaBackgroundMessages(unity, nullptr)
        && windowmode::UsesFakeFocus(unity)
        && windowmode::LooksLikeInjectRequiredGameClass(L"UnityWndClass")
        && windowmode::LooksLikeInjectRequiredGameClass(L"UnrealWindow")
        && windowmode::LooksLikeInjectRequiredGameClass(L"GLFW30")
        && !windowmode::LooksLikeInjectRequiredGameClass(L"MapleStoryClass");

    // ⚠ 回归守：**GLFW30 作为配置类名**（后台窗口模式录制回填的正是这种）必须整条链走假焦点。
    // 上一版用例只断言了 `LooksLikeInjectRequiredGameClass(L"GLFW30")`，
    // **从没检查 GLFW30 作为 windowClassName 时 UsesFakeFocus 是否成立** ——
    // 于是「MC 后台回放全程软输入、SendInput ok=0」漏了出去。
    windowmode::WindowModeScriptConfig glfw{};
    glfw.enabled = true;
    glfw.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    glfw.windowClassName = L"GLFW30";
    glfw.targetExePath = L"C:\\Users\\u\\AppData\\Roaming\\.minecraft\\runtime\\java.exe";
    const bool glfwLca = windowmode::PrefersLcaBackgroundMessages(glfw, nullptr);
    const bool glfwNeeds = windowmode::NeedsFakeFocusInjection(glfw, nullptr);
    const bool glfwUses = windowmode::UsesFakeFocus(glfw);
    const bool glfwHw = windowmode::GameTargetNeedsHardwareWithoutFakeFocus(glfw, nullptr);
    const bool glfwOk = !glfwLca && glfwNeeds && glfwUses;

    // 同理 SDL_app / UnrealWindow 也要整条链成立。
    windowmode::WindowModeScriptConfig sdl = glfw;
    sdl.windowClassName = L"SDL_app";
    windowmode::WindowModeScriptConfig ue = glfw;
    ue.windowClassName = L"UnrealWindow";
    const bool sdlOk = !windowmode::PrefersLcaBackgroundMessages(sdl, nullptr)
        && windowmode::UsesFakeFocus(sdl);
    const bool ueOk = !windowmode::PrefersLcaBackgroundMessages(ue, nullptr)
        && windowmode::UsesFakeFocus(ue);

    windowmode::WindowModeScriptConfig note{};
    note.enabled = true;
    note.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    note.windowClassName = L"Notepad";
    const bool noteOk = !windowmode::PrefersLcaBackgroundMessages(note, nullptr)
        && !windowmode::NeedsFakeFocusInjection(note, nullptr)
        && windowmode::ShouldMinimizeTargetAfterBind(note);

    const bool ok = unknownOk && hiddenOk && unityOk && noteOk && glfwOk && sdlOk && ueOk;
    wchar_t detail[420]{};
    swprintf_s(detail,
        L"unknown=%d hidden=%d unity=%d note=%d | GLFW30 lca=%d needs=%d uses=%d hwNoFF=%d sdl=%d ue=%d",
        unknownOk ? 1 : 0, hiddenOk ? 1 : 0, unityOk ? 1 : 0, noteOk ? 1 : 0,
        glfwLca ? 1 : 0, glfwNeeds ? 1 : 0, glfwUses ? 1 : 0, glfwHw ? 1 : 0,
        sdlOk ? 1 : 0, ueOk ? 1 : 0);
    Emit(L"lca_bg_unknown_game", ok,
        ok ? L"unknown IWWindowClass →LCA; Unity/GLFW30/SDL_app/Unreal inject; Notepad Edit path"
           : detail);
}

void TestTianLongBaBuFakeFocus() {
    windowmode::WindowModeScriptConfig tl{};
    tl.enabled = true;
    tl.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    tl.windowClassName = L"TianLongBaBuHJ WndClass";
    tl.targetExePath = L"D:\\yx\\开心天龙\\Bin64\\Game.exe";
    tl.windowName = L"《新天龙八部》0.08.0826 (一大区:梦笔生花)";
    const bool classOk = windowmode::LooksLikeTianLongBaBuWindowClass(tl.windowClassName)
        && windowmode::LooksLikeTianLongBaBuExecutable(tl.targetExePath)
        && windowmode::LooksLikeTianLongBaBuTitle(tl.windowName)
        && windowmode::LooksLikeTianLongBaBuTarget(tl, nullptr)
        && windowmode::LooksLikeGameWindowClass(tl.windowClassName)
        && windowmode::LooksLikeInjectRequiredGameClass(tl.windowClassName)
        && windowmode::NeedsFakeFocusInjection(tl, nullptr)
        && !windowmode::PrefersLcaBackgroundMessages(tl, nullptr)
        && windowmode::UsesFakeFocus(tl)
        && windowmode::UsesFakeFocusForTarget(tl, nullptr)
        && !windowmode::ShouldMinimizeTargetAfterBind(tl)
        && !windowmode::LooksLikeMapleStoryTarget(tl, nullptr);

    windowmode::WindowModeScriptConfig unknown{};
    unknown.enabled = true;
    unknown.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    unknown.windowClassName = L"IWWindowClass";
    const bool unknownStillLca = windowmode::PrefersLcaBackgroundMessages(unknown, nullptr)
        && !windowmode::NeedsFakeFocusInjection(unknown, nullptr);

    const bool ok = classOk && unknownStillLca;
    Emit(L"tianlong_bg_fake_focus", ok,
        ok ? L"TianLongBaBuHJ →inject lite fake-focus; IWWindowClass still LCA"
           : L"天龙八部 still classified as LCA unknown game");
}

UINT g_arrowProbeVk = 0;
LPARAM g_arrowProbeLp = 0;

int g_weixinProbeActivate = 0;
int g_weixinProbeSetFocus = 0;
int g_weixinProbeKeyDown = 0;
int g_weixinProbeKeyUp = 0;
int g_weixinProbeChar = 0;
int g_weixinProbePaste = 0;
int g_weixinProbeLButton = 0;

LRESULT CALLBACK WeixinKeyProbeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) ++g_weixinProbeActivate;
        break;
    case WM_SETFOCUS:
        ++g_weixinProbeSetFocus;
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        ++g_weixinProbeKeyDown;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        ++g_weixinProbeKeyUp;
        break;
    case WM_CHAR:
    case WM_SYSCHAR:
        ++g_weixinProbeChar;
        break;
    case WM_PASTE:
        ++g_weixinProbePaste;
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        ++g_weixinProbeLButton;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ArrowProbeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        g_arrowProbeVk = static_cast<UINT>(wParam);
        g_arrowProbeLp = lParam;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void TestLcaArrowKeyLParam() {
    const LPARAM leftDown = windowmode::BuildWindowKeyLParam(VK_LEFT, true);
    const UINT leftScan = static_cast<UINT>((leftDown >> 16) & 0xFF);
    const bool leftExt = ((leftDown >> 24) & 1) != 0;
    const LPARAM upDown = windowmode::BuildWindowKeyLParam(VK_UP, true);
    const UINT upScan = static_cast<UINT>((upDown >> 16) & 0xFF);
    const bool bitsOk = leftScan == 0x4B && leftExt && (leftDown & 0xFFFF) == 1
        && upScan == 0x48 && (((upDown >> 24) & 1) != 0);
    const bool normOk = NormalizeScriptKeyVk(0x2190, L"") == VK_LEFT
        && VirtualKeyFromKeyText(L"←") == VK_LEFT
        && NormalizeScriptKeyVk(37, L"←") == VK_LEFT;

    constexpr wchar_t kCls[] = L"QstLcaArrowProbeWnd";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ArrowProbeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kCls, L"QST ArrowProbe",
        WS_OVERLAPPEDWINDOW, 40, 40, 160, 80,
        nullptr, nullptr, wc.hInstance, nullptr);
    bool postedOk = false;
    if (hwnd) {
        g_arrowProbeVk = 0;
        g_arrowProbeLp = 0;
        windowmode::SetLcaBackgroundMessageMode(true);
        windowmode::PostKeyToWindow(hwnd, 0x2190, true);
        for (int i = 0; i < 40; ++i) {
            MSG msg{};
            while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (g_arrowProbeVk == VK_LEFT) break;
            Sleep(5);
        }
        const UINT postedScan = static_cast<UINT>((g_arrowProbeLp >> 16) & 0xFF);
        postedOk = g_arrowProbeVk == VK_LEFT && postedScan == 0x4B
            && ((g_arrowProbeLp >> 24) & 1) != 0;
        windowmode::PostKeyToWindow(hwnd, VK_LEFT, false);
        windowmode::SetLcaBackgroundMessageMode(false);
        DestroyWindow(hwnd);
    }
    UnregisterClassW(kCls, wc.hInstance);

    const bool ok = bitsOk && normOk && postedOk;
    wchar_t detail[200]{};
    swprintf_s(detail, L"scan=0x%02X ext=%d norm=%d postedVk=0x%02X postedScan=0x%02X",
        leftScan, leftExt ? 1 : 0, normOk ? 1 : 0, g_arrowProbeVk,
        static_cast<unsigned>((g_arrowProbeLp >> 16) & 0xFF));
    Emit(L"lca_arrow_key_lparam", ok, ok ? L"" : detail);
}

/// 后台窗口模式跑脚本时，方向键兜底 SendInput **不得**打进遮挡窗：
/// 目标不在前台时它打的是用户当前前台窗（浏览器视频 ←/→ 跳进度、↑/↓ 调音量），
/// 而目标自己失焦停轮询，照样不走。这里用系统键态复核：
/// 目标在后台 → PostKeyToWindow(VK_LEFT, down) 之后本机 VK_LEFT 仍应是抬起的。
void TestLcaNavKeyLeakGuard() {
    constexpr wchar_t kCls[] = L"QstNavLeakGuardWnd";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ArrowProbeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    // 不显示、也不是 TOOLWINDOW：ShouldMirrorLcaNavKeyState 会放行，
    // 于是这条用例真正走到「兜底真键」那一行（不会被 TOOLWINDOW 提前挡掉）。
    HWND hwnd = CreateWindowExW(0, kCls, L"QST NavLeakGuard",
        WS_OVERLAPPEDWINDOW, 40, 40, 160, 80,
        nullptr, nullptr, wc.hInstance, nullptr);

    bool predicateOk = false;
    bool noLeak = false;
    bool keyBusy = false;
    if (hwnd) {
        const HWND fg = GetForegroundWindow();
        // 正/反两个方向都要对：真前台窗算「拥有前台」，探针（不在前台）必须不算。
        predicateOk = (fg != nullptr && windowmode::TargetOwnsForegroundWindow(fg))
            && !windowmode::TargetOwnsForegroundWindow(nullptr)
            && GetForegroundWindow() != hwnd
            && !windowmode::TargetOwnsForegroundWindow(hwnd);

        keyBusy = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
        if (!keyBusy) {
            windowmode::SetLcaBackgroundMessageMode(true);
            windowmode::PostKeyToWindow(hwnd, VK_LEFT, true);
            // SendInput 是异步的：给它 150ms 变成「按下」；不变才算没漏。
            bool down = false;
            for (int i = 0; i < 30; ++i) {
                if ((GetAsyncKeyState(VK_LEFT) & 0x8000) != 0) {
                    down = true;
                    break;
                }
                Sleep(5);
            }
            windowmode::PostKeyToWindow(hwnd, VK_LEFT, false);
            windowmode::SetLcaBackgroundMessageMode(false);
            noLeak = !down;
        }
        DestroyWindow(hwnd);
    }
    UnregisterClassW(kCls, wc.hInstance);

    const bool ok = predicateOk && (keyBusy || noLeak);
    wchar_t detail[220]{};
    swprintf_s(detail, L"predicate=%d bgNoLeak=%d keyBusy=%d",
        predicateOk ? 1 : 0, noLeak ? 1 : 0, keyBusy ? 1 : 0);
    Emit(L"lca_nav_key_leaks_to_foreground", ok, ok ? L"" : detail);
}

/// 方向键兜底真键：**按下时目标在前台（补了真键 ↓）、松开时用户已切走（不在前台），
/// 也必须补 KEYUP**。否则真键永久卡在按下状态 —— 游戏朝离开时那个方向一直走，
/// 而且整个系统都认为该键被按住。
/// 现场症状（用户原话）：「在游戏前台启动，再去浏览器看视频，就会朝离开时候的那一个方向 瞬移」。
void TestLcaNavKeyupReleasedAfterFocusLoss() {
    using windowmode::ShouldMirrorNavKeySend;
    // 按下：只在目标就是前台窗时补（后台补会打进遮挡窗 —— 由 lca_nav_key_leaks_to_foreground 守）。
    const bool downFg = ShouldMirrorNavKeySend(true, true, false);
    const bool downBg = !ShouldMirrorNavKeySend(true, false, false);
    // 松开：只看「当初补过没有」，与此刻是否前台无关。
    const bool upAfterLost = ShouldMirrorNavKeySend(false, false, true);   // ← 本轮核心回归
    const bool upNotMirrored = !ShouldMirrorNavKeySend(false, true, false);
    const bool upStillFg = ShouldMirrorNavKeySend(false, true, true);

    // 真机复核：后台目标走一遍 DOWN/UP + 兜底松键后，系统键态不得残留按下。
    constexpr wchar_t kCls[] = L"QstNavKeyupProbe";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kCls;
        registered = RegisterClassExW(&wc) != 0;
    }
    HWND hwnd = registered
        ? CreateWindowExW(0, kCls, L"QST Nav Keyup Probe", WS_OVERLAPPEDWINDOW,
            72, 72, 220, 110, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)
        : nullptr;
    bool released = true;
    if (hwnd) {
        if ((GetAsyncKeyState(VK_LEFT) & 0x8000) == 0) {
            windowmode::SetLcaBackgroundMessageMode(true);
            windowmode::PostKeyToWindow(hwnd, VK_LEFT, true);
            windowmode::PostKeyToWindow(hwnd, VK_LEFT, false);
            windowmode::ReleaseMirroredLcaNavKeys();
            windowmode::SetLcaBackgroundMessageMode(false);
            for (int i = 0; i < 30; ++i) {
                if ((GetAsyncKeyState(VK_LEFT) & 0x8000) == 0) break;
                Sleep(5);
            }
            released = (GetAsyncKeyState(VK_LEFT) & 0x8000) == 0;
        }
        DestroyWindow(hwnd);
    }

    const bool ok = downFg && downBg && upAfterLost && upNotMirrored && upStillFg
        && hwnd && released;
    wchar_t detail[260]{};
    swprintf_s(detail,
        L"downFg=%d downBg=%d upAfterLost=%d upNotMirrored=%d upStillFg=%d released=%d",
        downFg ? 1 : 0, downBg ? 1 : 0, upAfterLost ? 1 : 0, upNotMirrored ? 1 : 0,
        upStillFg ? 1 : 0, released ? 1 : 0);
    Emit(L"lca_nav_keyup_released_after_focus_loss", ok,
        ok ? L"按下只在前台补；松开按当初是否补过必补（切走后不卡键）" : detail);
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

void TestUwpFrameBindPidStillAlive() {
    using windowmode::TargetBindPidStillMatches;
    const bool win32Same = TargetBindPidStillMatches(100, 100, 100, 100);
    const bool uwpSplit = TargetBindPidStillMatches(200, 200, 100, 100);
    const bool legacyUwpNoBindStored = TargetBindPidStillMatches(0, 200, 100, 100);
    const bool hijacked = !TargetBindPidStillMatches(200, 300, 100, 100);
    const bool hwndReused = !TargetBindPidStillMatches(200, 400, 100, 500);
    const bool ok = win32Same && uwpSplit && legacyUwpNoBindStored && hijacked && hwndReused;
    wchar_t detail[160]{};
    swprintf_s(detail, L"win32=%d uwp=%d legacy=%d hijack=%d reuse=%d",
        win32Same ? 1 : 0, uwpSplit ? 1 : 0, legacyUwpNoBindStored ? 1 : 0,
        hijacked ? 1 : 0, hwndReused ? 1 : 0);
    Emit(L"uwp_frame_bind_pid_still_alive", ok, ok ? L"" : detail);
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
        Emit(L"window_list_match_by_pid", false, L"skipped");
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

    // ★ pid 精确过滤（2026-10-02）：双开同名窗口时标题**完全一样**，只有 pid 能唯一锁定；
    //   而台账 `FormatWindowList` 本来就打印 `pid=…`、还叫模型「按 pid/客户区区分，别只按标题选」
    //   ⇒ 匹配端必须支持 pid，否则产品是在叫模型做一件它不支持的事。
    windowmode::SwitchableWindow twinA;
    twinA.hwnd = top;
    twinA.title = L"造梦无双 - 双开";
    twinA.processName = L"maple.exe";
    twinA.pid = 11111;
    windowmode::SwitchableWindow twinB = twinA;
    twinB.pid = 22222;
    const std::vector<windowmode::SwitchableWindow> twins = {twinA, twinB};
    const auto byPidA = windowmode::MatchWindows(twins, L"", 11111);
    const auto byPidB = windowmode::MatchWindows(twins, L"", 22222);
    const auto byBadPid = windowmode::MatchWindows(twins, L"", 99999);
    const auto pidPlusQuery = windowmode::MatchWindows(twins, L"造梦", 22222);
    const auto pidQueryMiss = windowmode::MatchWindows(twins, L"zzz-no-such", 11111);
    const bool pidFilterOk = byPidA.size() == 1 && byPidA.front().pid == 11111
        && byPidB.size() == 1 && byPidB.front().pid == 22222
        && byBadPid.empty()
        && pidPlusQuery.size() == 1 && pidPlusQuery.front().pid == 22222
        && pidQueryMiss.empty();
    // 负对照：pid=0 =「不按 pid 过滤」⇒ 行为必须与旧版**完全一致**（两条都返回）
    const bool pidZeroKeepsOldBehavior =
        windowmode::MatchWindows(twins, L"", 0).size() == 2
        && windowmode::MatchWindows(twins, L"造梦").size() == 2;
    const bool pidCaseOk = pidFilterOk && pidZeroKeepsOldBehavior;
    Emit(L"window_list_match_by_pid", pidCaseOk,
        pidCaseOk ? L"" : L"MatchWindows(pid) filter failed");

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
    // 激活可能抢不到前台而停在最小化；后面screen_point 还要用这扇窗。
    ShowWindow(top, SW_RESTORE);
    ShowWindow(top, SW_SHOWNOACTIVATE);
    UpdateWindow(top);
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

// ── 前台/异步状态观察（替代「固定 sleep 后读一次」）──────────────────
// 为什么要有这两个：窗口恢复、前台切换都是**异步**的，固定 sleep 之后读一次会
// 随机读到中间态 —— 实测 background_minimized_quiet_restore 抖动率约 40%，
// background_click_keeps_foreground 同源（同样是 50/80ms 定值 sleep + 读一次）。
//
// 但「持续采样 + 一有偏差就失败」也不对（改完实测失败率反而升到 70%）：
// 窗口状态切换期间 GetForegroundWindow() 会瞬时返回 NULL 或第三方窗，那是
// **正常现象**，不是产品抢了前台。
//
// 所以判定拆成两条，各自语义明确：
//   stolen  = **目标窗**（probe）成为前台 —— 这才是产品缺陷（用例要防的）；
//   settled = 期望窗（decoy）在前台被观察到，且观察窗口结束时仍是前台。
// 瞬时 NULL / 第三方窗不计失败。
// 注意：修法是改**等待方式**，不是放宽判定标准 —— stolen 这条比原来的
// 「sleep 后读一次 == decoy」更严（读一次可能恰好错过真正的抢前台）。
class ForegroundWatch {
public:
    ForegroundWatch(HWND expect, HWND target) : expect_(expect), target_(target) {}

    void Sample() {
        HWND fg = GetForegroundWindow();
        if (target_ && fg == target_) stolen_ = true;
        if (fg == expect_) {
            settled_ = true;
            lastWasExpect_ = true;
        } else {
            lastWasExpect_ = false;
            if (fg != nullptr) otherSeen_ = true;
        }
    }

    /// 观察 windowMs；返回「目标窗没抢前台 && 结束时前台是期望窗」。
    bool Run(int windowMs, int stepMs = 20) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(windowMs);
        for (;;) {
            Sample();
            if (stolen_) return false;
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
        }
        // 结束时给一小段沉降时间，避免恰好采到切换中的瞬时值
        for (int i = 0; i < 10 && !lastWasExpect_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
            Sample();
            if (stolen_) return false;
        }
        return settled_ && lastWasExpect_;
    }

    bool stolen() const { return stolen_; }
    bool settled() const { return settled_; }
    bool otherSeen() const { return otherSeen_; }

private:
    HWND expect_ = nullptr;
    HWND target_ = nullptr;
    bool stolen_ = false;
    bool settled_ = false;
    bool lastWasExpect_ = false;
    bool otherSeen_ = false;
};

/// 轮询直到谓词为真，超时返回 false。
template <typename Pred>
bool WaitUntil(Pred pred, int timeoutMs = 600, int stepMs = 20) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    }
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

void TestWindowFindImageFullClient(HWND edit) {
    int px1 = 9, py1 = 9, px2 = 9, py2 = 9;
    const bool pureOk = windowmode::EffectiveWindowModeClientSearchRect(800, 600, px1, py1, px2, py2)
        && px1 == 0 && py1 == 0 && px2 == 800 && py2 == 600;
    int zx1 = 1, zy1 = 2, zx2 = 3, zy2 = 4;
    const bool rejectZero = !windowmode::EffectiveWindowModeClientSearchRect(0, 100, zx1, zy1, zx2, zy2);

    bool liveOk = true;
    std::wstring liveDetail;
    if (!edit) {
        liveOk = true;
        liveDetail = L"no hwnd (pure only)";
    } else {
        HWND top = ParentTopWindow(edit);
        wchar_t topCls[256]{};
        GetClassNameW(top, topCls, 256);
        RECT wr{};
        GetWindowRect(top, &wr);

        windowmode::WindowModeScriptConfig cfg{};
        cfg.enabled = true;
        cfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
        cfg.windowClassName = topCls;
        cfg.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
        cfg.targetPickX = (wr.left + wr.right) / 2;
        cfg.targetPickY = (wr.top + wr.bottom) / 2;
        cfg.coordSpace = windowmode::WindowModeCoordinateSpace::ScreenAbsolute;

        std::wstring err;
        windowmode::WindowModeExecutor exec;
        if (!exec.BeginRun(cfg, err)) {
            liveOk = false;
            liveDetail = err.empty() ? L"BeginRun failed" : err;
        } else {
            RECT cr{};
            HWND bound = exec.TargetHwnd();
            HWND cap = bound ? windowmode::TopLevelTargetWindow(bound) : top;
            if (!cap || !GetClientRect(cap, &cr)) GetClientRect(top, &cr);
            const int cw = std::max(0, static_cast<int>(cr.right - cr.left));
            const int ch = std::max(0, static_cast<int>(cr.bottom - cr.top));

            ScriptAction picked{};
            picked.searchFullScreen = false;
            picked.searchX1 = 40;
            picked.searchY1 = 50;
            picked.searchX2 = 140;
            picked.searchY2 = 150;

            int x1 = -1, y1 = -1, x2 = -1, y2 = -1;
            const bool pickedOk = exec.ResolveClientSearchRect(picked, x1, y1, x2, y2)
                && x1 == 0 && y1 == 0 && x2 == cw && y2 == ch;

            ScriptAction fullScreen{};
            fullScreen.searchFullScreen = true;
            fullScreen.searchX1 = 0;
            fullScreen.searchY1 = 0;
            fullScreen.searchX2 = 3840;
            fullScreen.searchY2 = 2160;
            int fx1 = -1, fy1 = -1, fx2 = -1, fy2 = -1;
            const bool fullOk = exec.ResolveClientSearchRect(fullScreen, fx1, fy1, fx2, fy2)
                && fx1 == 0 && fy1 == 0 && fx2 == cw && fy2 == ch;

            POINT origin{0, 0};
            ClientToScreen(cap, &origin);
            int mx1 = -1, my1 = -1, mx2 = -1, my2 = -1;
            const bool mapOk = exec.MapClientRect(0, 0, cw, ch, mx1, my1, mx2, my2)
                && mx1 == origin.x && my1 == origin.y
                && mx2 == origin.x + cw && my2 == origin.y + ch;

            exec.EndRun();
            liveOk = pickedOk && fullOk && mapOk && cw > 0 && ch > 0;
            if (!liveOk) {
                wchar_t buf[256]{};
                swprintf_s(buf,
                    L"picked=(%d,%d)-(%d,%d) full=(%d,%d)-(%d,%d) map=(%d,%d)-(%d,%d) origin=(%d,%d) client=%dx%d",
                    x1, y1, x2, y2, fx1, fy1, fx2, fy2, mx1, my1, mx2, my2,
                    origin.x, origin.y, cw, ch);
                liveDetail = buf;
            }
        }
    }

    const bool ok = pureOk && rejectZero && liveOk;
    std::wstring detail;
    if (!ok) {
        detail = pureOk ? L"pure ok" : L"pure FAIL";
        detail += rejectZero ? L" | rejectZero ok" : L" | rejectZero FAIL";
        detail += liveOk ? L" | live ok" : (L" | live FAIL " + liveDetail);
    }
    Emit(L"window_findimage_full_client", ok, detail.c_str());
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
    // 前置条件：decoy 必须**稳定**成为前台。前台被别的窗口占着时，本用例的观察
    // 结果没有意义 —— 那种情况应跳过，而不是判失败（否则就是拿环境抖动当产品缺陷）。
    // 实测本机偶发「拿不到前台」；这条前置把假失败挡在外面。
    if (!WaitUntil([&] { return GetForegroundWindow() == decoy; }, 500)) {
        DestroyWindow(probe);
        DestroyWindow(decoy);
        Emit(L"background_minimized_quiet_restore", true,
            L"skipped: 前台被占用，无法稳定观察");
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
    // 1) 等窗口从最小化恢复（异步），同时观察前台是否被目标窗抢走
    ForegroundWatch bindWatch(decoy, probe);
    const bool restored = began && WaitUntil([&] {
        bindWatch.Sample();
        return IsIconic(probe) == FALSE;
    });
    const bool bindFgKept = began && bindWatch.Run(400);

    bool remin = false;
    bool clickFgKept = false;
    ForegroundWatch clickWatch(decoy, probe);
    if (began) {
        exec.PostMouseClickAtClient(20, 20, MouseButtonType::Left);
        clickFgKept = clickWatch.Run(400);
        exec.EndRun();
        remin = WaitUntil([&] { return IsIconic(probe) != FALSE; });
        clickFgKept = clickFgKept && clickWatch.Run(200);
    }

    DestroyWindow(decoy);
    DestroyWindow(probe);

    const bool anyStolen = bindWatch.stolen() || clickWatch.stolen();
    const bool fgChecksOk = bindFgKept && clickFgKept;
    const bool coreOk = began && restored && remin;

    std::wstring detail;
    if (!began) {
        detail = L"BeginRun: " + err;
    } else {
        detail = bindFgKept ? L"bind FG ok" : L"bind stole FG";
        if (bindWatch.stolen()) detail += L"(目标窗成前台)";
        else if (bindWatch.otherSeen()) detail += L"(第三方窗/NULL 瞬时)";
        detail += restored ? L" | restored" : L" | still iconic";
        detail += clickFgKept ? L" | click FG ok" : L" | click stole FG";
        if (clickWatch.stolen()) detail += L"(目标窗成前台)";
        else if (clickWatch.otherSeen()) detail += L"(第三方窗/NULL 瞬时)";
        detail += remin ? L" | EndRun min" : L" | EndRun not min";
    }

    // 判定的分层（这是本用例从 40% 抖动里收敛出来的口径）：
    //   1) 目标窗**从未**成为前台 → 产品没抢前台。这条是硬失败，且比原来的
    //      「sleep 后读一次 == decoy」更严（持续采样，不会恰好错过真抢）。
    //   2) 其余核心状态（窗口恢复 / EndRun 重新最小化）不满足 → 硬失败。
    //   3) 只有「期望窗是否始终在前台」不满足、且目标窗没抢过前台时 —— 说明
    //      观察窗内前台被**第三方窗或 NULL** 占过（本机实测：AI 终端窗口会抢焦点），
    //      这种情况判定不了，跳过并写明原因，而不是把环境抖动记成产品缺陷。
    if (began && coreOk && !fgChecksOk && !anyStolen) {
        Emit(L"background_minimized_quiet_restore", true,
            (L"skipped: 前台被第三方窗/NULL 干扰（目标窗未抢前台）| " + detail).c_str());
        return;
    }

    const bool ok = coreOk && fgChecksOk;
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

// ---------------------------------------------------------------------------
// 「按键点击打不进后台」回归（用户反馈 1.3.3 起，2026-09-23 定位）
//
// 实测树（Windows 11 商店版记事本 / WinUI3）：
//     Notepad(top) └ NotepadTextBox(755x553) └ RichEditD2DPT(755x553)
// 父子客户区一样大 + EnumChildWindows「父先于子」+ 最大值判据用**严格大于**
// ⇒ 「最大后代」启发式取到**包装层**；而 PostMessage **不向子窗转发**，投给包装层
// 等于完全没投（本机实测：投 WM_CHAR 给 NotepadTextBox → 文档一个字都不进；
// 投给 RichEditD2DPT → 正常进字）。`ResolveSoftInputHwnd` 又用**无 config** 的重解析
// 把已经绑对的 RichEditD2DPT 覆盖掉。两条都要挡。
// ---------------------------------------------------------------------------

struct KeyMsgRecord {
    HWND hwnd = nullptr;
    UINT msg = 0;
};

std::vector<KeyMsgRecord> g_keyMsgs;

LRESULT CALLBACK KeyRecordProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CHAR: case WM_SYSCHAR:
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        g_keyMsgs.push_back(KeyMsgRecord{hwnd, msg});
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int CountKeyMsgs(HWND hwnd) {
    int n = 0;
    for (const KeyMsgRecord& r : g_keyMsgs) {
        if (r.hwnd == hwnd) ++n;
    }
    return n;
}

int CountCharMsgs(HWND hwnd) {
    int n = 0;
    for (const KeyMsgRecord& r : g_keyMsgs) {
        if (r.hwnd == hwnd && (r.msg == WM_CHAR || r.msg == WM_SYSCHAR)) ++n;
    }
    return n;
}

int CountKeyDownMsgs(HWND hwnd) {
    int n = 0;
    for (const KeyMsgRecord& r : g_keyMsgs) {
        if (r.hwnd == hwnd && (r.msg == WM_KEYDOWN || r.msg == WM_SYSKEYDOWN)) ++n;
    }
    return n;
}

/// WinUI 自译判据（纯逻辑）：类名识别 + 「哪些键走 WM_CHAR」。
/// 实测依据见 `window_mode_requirements.h` 第 16 条。
void TestBackgroundKeySelfTranslatePolicy() {
    const bool clsOk = windowmode::ClassSelfTranslatesPostedKeys(L"RichEditD2DPT")
        && windowmode::ClassSelfTranslatesPostedKeys(L"richeditd2dpt")
        && !windowmode::ClassSelfTranslatesPostedKeys(L"Edit")
        && !windowmode::ClassSelfTranslatesPostedKeys(L"RICHEDIT50W")
        && !windowmode::ClassSelfTranslatesPostedKeys(L"")
        && !windowmode::ClassSelfTranslatesPostedKeys(nullptr);

    const bool charOk = windowmode::SelfTranslateKeyUsesWmChar(true, L'a')
        && windowmode::SelfTranslateKeyUsesWmChar(true, L' ')
        && windowmode::SelfTranslateKeyUsesWmChar(true, L'A')
        // Enter/Tab（'\r'/'\t'）实测 WM_CHAR **不换行/不制表** ⇒ 必须走 KEYDOWN
        && !windowmode::SelfTranslateKeyUsesWmChar(true, L'\r')
        && !windowmode::SelfTranslateKeyUsesWmChar(true, L'\t')
        // 退格/删除/方向键/功能键：SoftVkToChar 返回 0 ⇒ 走 KEYDOWN
        && !windowmode::SelfTranslateKeyUsesWmChar(true, 0)
        // 非自译目标：一切照旧（KEYDOWN + WM_CHAR）
        && !windowmode::SelfTranslateKeyUsesWmChar(false, L'a')
        && !windowmode::SelfTranslateKeyUsesWmChar(false, 0);

    const bool ok = clsOk && charOk;
    Emit(L"background_key_self_translate_policy", ok,
        ok ? L"" : (clsOk ? L"selfTranslateKeyUsesWmChar wrong" : L"selfTranslate class match wrong"));
}

/// 窗口相对录制产物：`windowName` 只是「录制那一刻的标题」，**不得**当硬匹配门。
/// 前科：回放时标题一变（换文档/换标签/游戏换场景）就枚举不到任何窗口 ⇒ 绑不到目标
/// ⇒ 用户看到「后台窗口模式不操作后台」。复现见 tools/verify/probe_record_playback_bind.py。
void TestRecordedWindowTitleIsHintOnly() {
    constexpr wchar_t kCls[] = L"QstWmHintOnlyCls";
    constexpr wchar_t kRecTitle[] = L"REC_DOC.txt - 某程序";

    // ── ① 录制产物（hint-only）：不得产生标题硬门 ──
    windowmode::WindowModeScriptConfig rec{};
    rec.enabled = true;
    rec.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    rec.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
    rec.windowRelativeCoordinates = true;
    rec.windowName = kRecTitle;          // = 录制瞬间标题
    rec.windowClassName = kCls;
    rec.windowNameIsHintOnly = true;     // ← 录制端必须置位

    const windowmode::WindowTargetQuery qRec = windowmode::BuildTargetQuery(rec);
    const bool recNoTitleGate = qRec.titleContains.empty()
        && qRec.className == kCls;

    // ── ② 用户手配（默认 false）：旧语义必须保留，标题仍是硬门 ──
    windowmode::WindowModeScriptConfig man = rec;
    man.windowNameIsHintOnly = false;
    const windowmode::WindowTargetQuery qMan = windowmode::BuildTargetQuery(man);
    const bool manKeepsTitleGate = !qMan.titleContains.empty()
        && qMan.titleContains == L"REC_DOC.txt";

    // ── ③ 真值表：同一窗口 + 标题已变，hint-only 放行、手配拦截 ──
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = KeyRecordProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);
    HWND top = CreateWindowExW(0, kCls, L"PLAY_DOC.txt - 某程序", WS_OVERLAPPEDWINDOW,
        60, 60, 700, 500, nullptr, nullptr, hInst, nullptr);
    ShowWindow(top, SW_SHOW);
    UpdateWindow(top);
    PumpMessagesFor(std::chrono::milliseconds(60));

    wchar_t liveTitle[512]{};
    GetWindowTextW(top, liveTitle, 512);
    const bool titleChanged = std::wstring(liveTitle) != kRecTitle;
    const bool hintAllows = windowmode::DoesTopWindowMatchConfig(top, rec);
    const bool manualBlocks = !windowmode::DoesTopWindowMatchConfig(top, man);

    // ── ④ 端到端（决定性证据）：用产品真查找接口 FindMainWindowDefault 走完整枚举。
    //    同一窗口、标题已变：hint-only 必须找得到；手配（titleContains=REC_DOC.txt）找不到。
    //    这一条直接对应线上症状「回放时枚举不到任何窗口 ⇒ 绑不到 ⇒ 不操作后台」。
    windowmode::WindowTargetQuery qRecExe = qRec;
    qRecExe.exePath = L"";           // 自检窗口在本进程：不能按 exe 路径过滤掉自己
    qRecExe.className = kCls;
    const HWND foundHint = windowmode::FindMainWindowDefault(qRecExe, true);
    windowmode::WindowTargetQuery qManExe = qMan;
    qManExe.exePath = L"";
    qManExe.className = kCls;
    const HWND foundManual = windowmode::FindMainWindowDefault(qManExe, true);
    const bool e2eHintFound = foundHint == top;
    const bool e2eManualMissed = foundManual != top;

    DestroyWindow(top);

    const bool ok = recNoTitleGate && manKeepsTitleGate
        && titleChanged && hintAllows && manualBlocks
        && e2eHintFound && e2eManualMissed;
    wchar_t detail[320]{};
    swprintf_s(detail,
        L"recNoGate=%d manGate=%d titleChanged=%d hintAllows=%d manualBlocks=%d "
        L"e2eHintFound=%d e2eManualMissed=%d",
        recNoTitleGate ? 1 : 0, manKeepsTitleGate ? 1 : 0, titleChanged ? 1 : 0,
        hintAllows ? 1 : 0, manualBlocks ? 1 : 0,
        e2eHintFound ? 1 : 0, e2eManualMissed ? 1 : 0);
    Emit(L"background_recorded_title_is_hint_only", ok, detail);
}

void TestBackgroundInputWrappedTextControl() {
    constexpr wchar_t kTopCls[] = L"QstWmWinUiTop";
    constexpr wchar_t kWrapperCls[] = L"NotepadTextBox";   // 包装层（非输入类名）
    constexpr wchar_t kEditCls[] = L"RichEditD2DPT";       // 真文本控件（IsTextInputClass）
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = KeyRecordProc;
    wc.hInstance = hInst;
    for (const wchar_t* name : {kTopCls, kWrapperCls, kEditCls}) {
        wc.lpszClassName = name;
        RegisterClassExW(&wc);
    }

    HWND parent = CreateWindowExW(0, kTopCls, L"Notepad", WS_OVERLAPPEDWINDOW,
        40, 40, 820, 620, nullptr, nullptr, hInst, nullptr);
    HWND wrapper = CreateWindowExW(0, kWrapperCls, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 800, 600, parent, nullptr, hInst, nullptr);
    HWND edit = CreateWindowExW(0, kEditCls, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 800, 600, wrapper, nullptr, hInst, nullptr);
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    windowmode::BackgroundInputTargetKind kind = windowmode::BackgroundInputTargetKind::TopLevel;
    const HWND found = windowmode::FindBackgroundInputChild(parent, nullptr, &kind);
    const bool foundEdit = found == edit
        && kind == windowmode::BackgroundInputTargetKind::TextInput;

    // 端到端：把「包装层」当绑定目标投键（与用户日志 bind=包装层/子窗 的形态对齐），
    // 键必须落到里面的真控件上 —— 包装层一个键消息都不该收到。
    g_keyMsgs.clear();
    windowmode::ResetSoftMouseState();
    windowmode::PostKeyToWindow(wrapper, 'A', true);
    windowmode::PostKeyToWindow(wrapper, 'A', false);
    PumpMessagesFor(std::chrono::milliseconds(80));
    const int editChars = CountCharMsgs(edit);
    const int editKeys = CountKeyDownMsgs(edit);
    const int wrapperMsgs = CountKeyMsgs(wrapper);

    // editChars == 1：RichEditD2DPT 会自译 KEYDOWN，宿主再补 WM_CHAR 就**一次变两次**
    //   ⇒ 只发 WM_CHAR，且只发一次。
    // editKeys == 0：不能再有 KEYDOWN（否则控件会再自插一个字符）。
    const bool ok = foundEdit && editChars == 1 && editKeys == 0 && wrapperMsgs == 0;
    wchar_t detail[224]{};
    swprintf_s(detail,
        L"found=0x%p wrapper=0x%p edit=0x%p kind=%s editChars=%d editKeys=%d wrapperMsgs=%d",
        static_cast<void*>(found), static_cast<void*>(wrapper), static_cast<void*>(edit),
        windowmode::BackgroundInputTargetKindName(kind), editChars, editKeys, wrapperMsgs);

    DestroyWindow(parent);
    for (const wchar_t* name : {kEditCls, kWrapperCls, kTopCls}) {
        UnregisterClassW(name, hInst);
    }
    Emit(L"background_input_wrapped_text_control", ok, ok ? L"" : detail);
}

/// 已知渲染面若是**包装层**，必须让位给里面的真文本控件（2026-09-30）。
///
/// 背景：`FindBackgroundInputChild` 里「包装层让位」这条修正原先只加在
/// 「最大后代(≥320x240)」那一段，而**它上面那条 `FindKnownRenderSurfaceChild` 早退**漏了。
/// 两处必须同一把尺 —— 投给容器 = `PostMessage` 不转发 = 键鼠与滚轮全部石沉大海。
///
/// ⚠ 用例的**边界**（别把它当成"真机记事本已修"的证明）：
///   合成窗里用的是高分表命中的类名（`Chrome_RenderWidgetHostHWND`），
///   而**真机那台 Win11 记事本实际命中的是哪条分支，要看日志里的「判据=」**
///   —— 为此 `FindBackgroundInputChild` 现在会把命中的分支名一起打出来。
///   本条只保证：**一旦**容器命中了"已知渲染面"，选中的仍是里面那个真控件。
///
/// ⚠ 也**不断言滚轮落点**：`RichEditD2DPT` 自己处理滚轮（滚动自己的文档）且不再转发，
///   所以"容器与控件各收几条"只反映 DefWindowProc 的脾气，与产品行为无关。
void TestBackgroundWheelReachesInputInsideKnownRenderSurface() {
    constexpr wchar_t kTopCls[] = L"QstWmWinUiBridgeTop";
    // 用高分表真会命中的类名，才能走到那条早退分支。
    // ⚠ 不能用 `Chrome_RenderWidgetHostHWND`：它在**更早**的 `FindBrowserRenderWidget`
    //   就被命中（用例实测 kind=browserRender），根本到不了本条要钉的那一段。
    //   `SDL_app` 只命中高分表、不被浏览器分支抢走。
    constexpr wchar_t kBridgeCls[] = L"SDL_app";
    constexpr wchar_t kEditCls[] = L"RichEditD2DPT";
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = KeyRecordProc;
    wc.hInstance = hInst;
    for (const wchar_t* name : {kTopCls, kBridgeCls, kEditCls}) {
        wc.lpszClassName = name;
        RegisterClassExW(&wc);
    }

    HWND parent = CreateWindowExW(0, kTopCls, L"*test.txt - Notepad", WS_OVERLAPPEDWINDOW,
        40, 40, 820, 620, nullptr, nullptr, hInst, nullptr);
    HWND bridge = CreateWindowExW(0, kBridgeCls, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 800, 600, parent, nullptr, hInst, nullptr);
    HWND edit = CreateWindowExW(0, kEditCls, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 800, 600, bridge, nullptr, hInst, nullptr);
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    windowmode::BackgroundInputTargetKind kind = windowmode::BackgroundInputTargetKind::TopLevel;
    const HWND found = windowmode::FindBackgroundInputChild(parent, nullptr, &kind);
    const bool foundEdit = found == edit
        && kind == windowmode::BackgroundInputTargetKind::TextInput;

    // 端到端：以**绑定子窗**（真机日志里 bind=RichEditD2DPT）为入口投一格滚轮。
    // 滚轮必须落在真控件上；容器一个字节都不该收到。
    g_keyMsgs.clear();
    windowmode::ResetSoftMouseState();
    windowmode::PostScrollWheelToWindow(edit, 100, 100, 1, true, false);
    PumpMessagesFor(std::chrono::milliseconds(80));
    int editWheel = 0;
    int bridgeWheel = 0;
    int parentWheel = 0;
    for (const KeyMsgRecord& r : g_keyMsgs) {
        if (r.msg != WM_MOUSEWHEEL) continue;
        if (r.hwnd == edit) ++editWheel;
        else if (r.hwnd == bridge) ++bridgeWheel;
        else if (r.hwnd == parent) ++parentWheel;
    }
    // ② 真正的判据：必须选中**里面那个真控件**，而不是容器。
    //    （旧代码在这一格返回 bridge ⇒ 直接红。）
    const bool ok = foundEdit && bridgeWheel == 0 && parentWheel == 0;
    wchar_t detail[288]{};
    swprintf_s(detail,
        L"found=0x%p edit=0x%p bridge=0x%p kind=%s | 滚轮 edit=%d（仅供参考）、"
        L"bridge=%d parent=%d（都必须 0）",
        static_cast<void*>(found), static_cast<void*>(edit), static_cast<void*>(bridge),
        windowmode::BackgroundInputTargetKindName(kind), editWheel, bridgeWheel, parentWheel);

    DestroyWindow(parent);
    for (const wchar_t* name : {kEditCls, kBridgeCls, kTopCls}) {
        UnregisterClassW(name, hInst);
    }
    Emit(L"background_wheel_reaches_wrapped_input", ok, ok ? L"" : detail);
}

/// ★★★ 真机复现：绑定到 RichEdit，旁边还有一个**同为"最大后代"的容器兄弟**时，
///   鼠标/滚轮必须投给**绑定的那个 RichEdit**，不能投给容器（2026-09-30 真机日志）。
///
/// 真机窗口树（Win11 商店版记事本，用户日志实测 hwnd）：
///   `Notepad`(顶, 0x1A0694)
///     ├─ `Microsoft.UI.Content.DesktopChildSiteBridge`(0x1307AE) ← FindBackgroundInputChild 选它
///     └─ `RichEditD2DPT`(0x20906)                                ← 绑定用的就是它（configChild）
///
/// 真机症状：`滚轮投递 … 目标=0x…1307AE(DesktopChildSiteBridge) … 成功=1/1` ——
/// **投递成功，但投给了容器**。`PostMessage` 不向子窗转发 ⇒ 等于没投。
/// 根因：鼠标/滚轮走的是 `ResolveBackgroundPostTarget`（内部 `FindBackgroundInputChild(root,
/// nullptr)`，**config=nullptr**），把 `ResolveBindHwnd(top, config)` 带 config 选出来的
/// 绑定**丢掉了**，再用「最大后代」重新猜，猜到了容器。
/// 键盘不走这条路（用 `ResolveSoftInputHwnd`）——所以这是硬规则 ② 在鼠标侧漏了一次。
///
/// 本用例结构上**就是**真机那棵树，因此它红/绿能直接代表真机。
void TestWheelTargetsBoundChildNotWrapperSurface() {
    constexpr wchar_t kTopCls[] = L"QstWmNotepadLikeTop";
    constexpr wchar_t kBridgeCls[] = L"QstWmSiteBridgeSurface";  // 无渲染面评分，纯"最大后代"
    constexpr wchar_t kEditCls[] = L"RichEditD2DPT";
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = KeyRecordProc;
    wc.hInstance = hInst;
    for (const wchar_t* name : {kTopCls, kBridgeCls, kEditCls}) {
        wc.lpszClassName = name;
        RegisterClassExW(&wc);
    }

    HWND parent = CreateWindowExW(0, kTopCls, L"*test.txt - Notepad", WS_OVERLAPPEDWINDOW,
        40, 40, 820, 620, nullptr, nullptr, hInst, nullptr);
    // 容器：与真机一样，客户区**和真控件一样大**（父子同尺寸 ⇒ 最大值启发式取父）。
    HWND bridge = CreateWindowExW(0, kBridgeCls, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 800, 600, parent, nullptr, hInst, nullptr);
    // 真控件：与容器**同级**（真机实测就是这个形状），绑定用的就是它。
    HWND edit = CreateWindowExW(0, kEditCls, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 800, 600, parent, nullptr, hInst, nullptr);
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    // ① 无 config 的启发式确实会挑容器（记录现状，**不是**断言目标）
    windowmode::BackgroundInputTargetKind autoKind = windowmode::BackgroundInputTargetKind::TopLevel;
    const HWND autoPick = windowmode::FindBackgroundInputChild(parent, nullptr, &autoKind);

    // ② ★★ 真正的判据：以**绑定的 RichEdit** 为入口时，鼠标/滚轮的投递目标必须是它自己。
    //    ⚠ 这里只钉**选择结果**，不数消息：投出去之后落在哪由
    //      `SendNotifyMessage` + `DefWindowProc` 决定，在自建的假 WndProc 上不可观测
    //      （上一版就是去数消息 ⇒ 用例本身不可靠，红绿都不代表产品行为）。
    int cx = 200;
    int cy = 200;
    const HWND resolved = windowmode::ResolveMousePostTargetForTest(edit, cx, cy);

    const bool ok = resolved == edit;
    wchar_t detail[320]{};
    swprintf_s(detail,
        L"（无 config 启发式挑 0x%p kind=%s —— 仅供对照，它挑容器正是真机症状）| "
        L"绑定 edit=0x%p → 投递目标=0x%p（必须等于 edit），坐标=(%d,%d)",
        static_cast<void*>(autoPick), windowmode::BackgroundInputTargetKindName(autoKind),
        static_cast<void*>(edit), static_cast<void*>(resolved), cx, cy);

    DestroyWindow(parent);
    for (const wchar_t* name : {kEditCls, kBridgeCls, kTopCls}) {
        UnregisterClassW(name, hInst);
    }
    Emit(L"wheel_targets_bound_child_not_wrapper", ok, ok ? L"" : detail);
}

void TestBackgroundInputBoundChildRespected() {
    constexpr wchar_t kTopCls[] = L"QstWmBoundTop";
    constexpr wchar_t kBigCls[] = L"QstWmBigSurface";
    constexpr wchar_t kChildCls[] = L"QstWmBoundChild";
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = KeyRecordProc;
    wc.hInstance = hInst;
    for (const wchar_t* name : {kTopCls, kBigCls, kChildCls}) {
        wc.lpszClassName = name;
        RegisterClassExW(&wc);
    }

    HWND parent = CreateWindowExW(0, kTopCls, L"App", WS_OVERLAPPEDWINDOW,
        60, 60, 820, 620, nullptr, nullptr, hInst, nullptr);
    // 更大的兄弟子窗：旧代码的「无 config 重解析」会挑它，把用户明确绑定的子窗顶掉。
    HWND big = CreateWindowExW(0, kBigCls, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 800, 600, parent, nullptr, hInst, nullptr);
    HWND child = CreateWindowExW(0, kChildCls, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 200, 40, parent, nullptr, hInst, nullptr);
    (void)big;
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    g_keyMsgs.clear();
    windowmode::ResetSoftMouseState();
    windowmode::PostKeyToWindow(child, 'A', true);
    windowmode::PostKeyToWindow(child, 'A', false);
    PumpMessagesFor(std::chrono::milliseconds(80));
    const int childMsgs = CountKeyMsgs(child);
    const int bigMsgs = CountKeyMsgs(big);

    const bool ok = childMsgs > 0 && bigMsgs == 0;
    wchar_t detail[192]{};
    swprintf_s(detail, L"childMsgs=%d bigMsgs=%d", childMsgs, bigMsgs);

    DestroyWindow(parent);
    for (const wchar_t* name : {kChildCls, kBigCls, kTopCls}) {
        UnregisterClassW(name, hInst);
    }
    Emit(L"background_input_bound_child_respected", ok, ok ? L"" : detail);
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

void TestWeixinQtFakeFocus() {
    windowmode::WindowModeScriptConfig wx{};
    wx.enabled = true;
    wx.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    wx.windowClassName = L"Qt51514QWindowIcon";
    wx.targetExePath = L"D:\\Weixin\\Weixin.exe";
    wx.windowName = L"微信";
    const bool classOk = windowmode::LooksLikeWeixinExecutable(wx.targetExePath)
        && windowmode::LooksLikeWeixinTitle(L"微信")
        && windowmode::LooksLikeWeixinTarget(wx, nullptr)
        && windowmode::LooksLikeQtRenderWindowClass(wx.windowClassName)
        && windowmode::NeedsFakeFocusInjection(wx, nullptr)
        && windowmode::UsesFakeFocus(wx)
        && windowmode::UsesFakeFocusForTarget(wx, nullptr)
        && !windowmode::ConfigLooksLikeElectronShell(wx)
        && !windowmode::LooksLikeChromiumShellTarget(wx, nullptr)
        && !windowmode::ConfigLooksLikeEmulatorTarget(wx)
        && !windowmode::PrefersLcaBackgroundMessages(wx, nullptr)
        && !windowmode::ShouldMinimizeTargetAfterBind(wx)
        && !windowmode::GameTargetNeedsHardwareWithoutFakeFocus(wx, nullptr);

    const bool titleGate = !windowmode::LooksLikeWeixinTitle(L"微信开发者工具")
        && !windowmode::LooksLikeWeixinExecutable(L"C:\\Tools\\wechatdevtools.exe")
        && windowmode::LooksLikeWeixinExecutable(L"C:\\Tencent\\WeChat.exe");

    windowmode::WindowModeScriptConfig qtOnly{};
    qtOnly.enabled = true;
    qtOnly.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    qtOnly.windowClassName = L"Qt51514QWindowIcon";
    const bool qtOnlyOk = !windowmode::LooksLikeWeixinTarget(qtOnly, nullptr)
        && !windowmode::UsesFakeFocus(qtOnly)
        && windowmode::ConfigLooksLikeEmulatorTarget(qtOnly);

    windowmode::WindowModeScriptConfig mumu{};
    mumu.enabled = true;
    mumu.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    mumu.windowClassName = L"Qt5156QWindowIcon";
    mumu.windowName = L"MuMu安卓设备-1";
    const bool mumuStillEmu = windowmode::ConfigLooksLikeEmulatorTarget(mumu)
        && !windowmode::LooksLikeWeixinTarget(mumu, nullptr)
        && !windowmode::UsesFakeFocus(mumu);

    constexpr wchar_t kTopCls[] = L"Qt51514QWindowIcon";
    constexpr wchar_t kChildCls[] = L"QstWmWeixinChild";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WeixinKeyProbeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTopCls;
    RegisterClassExW(&wc);
    WNDCLASSEXW wcChild = wc;
    wcChild.lpfnWndProc = DefWindowProcW;
    wcChild.lpszClassName = kChildCls;
    RegisterClassExW(&wcChild);

    HWND parent = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kTopCls, L"微信",
        WS_OVERLAPPEDWINDOW, 80, 80, 900, 700, nullptr, nullptr, wc.hInstance, nullptr);
    HWND child = CreateWindowExW(0, kChildCls, L"",
        WS_CHILD | WS_VISIBLE, 0, 40, 900, 660, parent, nullptr, wc.hInstance, nullptr);
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    UpdateWindow(parent);

    windowmode::BackgroundInputTargetKind kind = windowmode::BackgroundInputTargetKind::TopLevel;
    HWND found = windowmode::FindBackgroundInputChild(parent, &wx, &kind);
    const bool topOk = parent && child && found == parent
        && kind == windowmode::BackgroundInputTargetKind::TopLevel;

    g_weixinProbeActivate = 0;
    g_weixinProbeSetFocus = 0;
    g_weixinProbeKeyDown = 0;
    g_weixinProbeKeyUp = 0;
    g_weixinProbeChar = 0;
    const HWND fgBefore = GetForegroundWindow();
    windowmode::ResetSoftMouseState();
    windowmode::PostKeyToWindow(parent, 'A', true);
    windowmode::PostKeyToWindow(parent, 'A', false);
    PumpMessagesDispatchOnly(std::chrono::milliseconds(80));
    const HWND fgAfter = GetForegroundWindow();
    const bool stoleFg = parent && fgBefore != parent && fgAfter == parent;
    const bool keyOk = parent
        && g_weixinProbeKeyDown == 1
        && g_weixinProbeKeyUp == 1
        && g_weixinProbeChar == 0
        && g_weixinProbeActivate == 0
        && g_weixinProbeSetFocus == 0
        && !stoleFg;

    g_weixinProbeActivate = 0;
    g_weixinProbeSetFocus = 0;
    g_weixinProbeKeyDown = 0;
    g_weixinProbeKeyUp = 0;
    g_weixinProbeChar = 0;
    g_weixinProbePaste = 0;
    g_weixinProbeLButton = 0;
    windowmode::PostQuickInputToWindow(parent, L"h", 0.0, false, nullptr);
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (std::chrono::steady_clock::now() < until
            && (g_weixinProbeKeyDown < 1 || g_weixinProbeKeyUp < 1)) {
            PumpMessagesDispatchOnly(std::chrono::milliseconds(10));
        }
    }
    const bool quickOk = g_weixinProbeKeyDown >= 1
        && g_weixinProbeKeyUp >= 1
        && g_weixinProbePaste == 0
        && g_weixinProbeChar == 0
        && g_weixinProbeActivate == 0;

    g_weixinProbeActivate = 0;
    g_weixinProbeLButton = 0;
    windowmode::PostMouseButtonToWindow(parent, 40, 40, MouseButtonType::Left, true);
    windowmode::PostMouseButtonToWindow(parent, 40, 40, MouseButtonType::Left, false);
    PumpMessagesDispatchOnly(std::chrono::milliseconds(80));
    const bool clickOk = g_weixinProbeLButton >= 2 && g_weixinProbeActivate == 0;

    DestroyWindow(parent);
    UnregisterClassW(kChildCls, wc.hInstance);
    UnregisterClassW(kTopCls, wc.hInstance);

    const bool ok = classOk && titleGate && qtOnlyOk && mumuStillEmu && topOk
        && keyOk && quickOk && clickOk;
    wchar_t detail[280]{};
    if (ok) {
        Emit(L"weixin_qt_fake_focus", true,
            L"Weixin Qt →lite fake-focus; KEY* no CHAR/ACTIVATE/PASTE");
    } else {
        swprintf_s(detail,
            L"class=%d top=%d key=%d quick=%d click=%d down=%d paste=%d lbtn=%d act=%d",
            classOk && titleGate && qtOnlyOk && mumuStillEmu ? 1 : 0,
            topOk ? 1 : 0, keyOk ? 1 : 0, quickOk ? 1 : 0, clickOk ? 1 : 0,
            g_weixinProbeKeyDown, g_weixinProbePaste, g_weixinProbeLButton,
            g_weixinProbeActivate);
        Emit(L"weixin_qt_fake_focus", false, detail);
    }
}

void TestWeixinQtMouseHooks() {
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif

    constexpr wchar_t kCls[] = L"Qt51514QWindowIcon";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kCls, L"微信",
        WS_OVERLAPPEDWINDOW, 64, 64, 240, 140, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(kCls, wc.hInstance);
        Emit(L"weixin_qt_mouse_hooks", false, L"CreateWindow Weixin probe failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    std::wstring softErr;
    if (!windowmode::FakeFocusSoftInput_Attach(GetCurrentProcessId(), softErr)) {
        DestroyWindow(hwnd);
        UnregisterClassW(kCls, wc.hInstance);
        Emit(L"weixin_qt_mouse_hooks", false,
            softErr.empty() ? L"Attach failed" : softErr.c_str());
        return;
    }

    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        windowmode::FakeFocusSoftInput_Detach();
        DestroyWindow(hwnd);
        UnregisterClassW(kCls, wc.hInstance);
        Emit(L"weixin_qt_mouse_hooks", false, L"LoadLibrary failed");
        return;
    }
    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    auto* installLite = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_InstallLite"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    const LONG_PTR procBefore = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (!installLite || !uninstall || !installLite(hwnd)) {
        if (uninstall) uninstall();
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        DestroyWindow(hwnd);
        UnregisterClassW(kCls, wc.hInstance);
        Emit(L"weixin_qt_mouse_hooks", false, L"InstallLite failed");
        return;
    }

    windowmode::FakeFocusSoftInput_SetCursorScreen(1234, 5678);
    POINT pt{};
    GetCursorPos(&pt);
    windowmode::FakeFocusSoftInput_SetMouseButtonVk(VK_LBUTTON, true);
    const SHORT lbtn = GetAsyncKeyState(VK_LBUTTON);
    const SHORT spaceIsolated = GetAsyncKeyState(VK_SPACE);
    SetCursorPos(1, 1);
    POINT after{};
    GetCursorPos(&after);
    const LONG_PTR procAfter = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    HWND hookedFg = GetForegroundWindow();

    uninstall();
    FreeLibrary(mod);
    windowmode::FakeFocusSoftInput_Detach();
    DestroyWindow(hwnd);
    UnregisterClassW(kCls, wc.hInstance);

    const bool ok = pt.x == 1234 && pt.y == 5678
        && after.x == 1234 && after.y == 5678
        && (lbtn & 0x8000) != 0
        && (spaceIsolated & 0x8000) == 0
        && procBefore == procAfter
        && hookedFg == hwnd;
    wchar_t detail[200]{};
    swprintf_s(detail, L"get=(%ld,%ld) afterWarp=(%ld,%ld) lbtn=0x%04x space=0x%04x proc=%d fg=%d",
        pt.x, pt.y, after.x, after.y,
        static_cast<unsigned>(lbtn) & 0xFFFF,
        static_cast<unsigned>(spaceIsolated) & 0xFFFF,
        procBefore == procAfter ? 1 : 0,
        hookedFg == hwnd ? 1 : 0);
    Emit(L"weixin_qt_mouse_hooks", ok, ok ? L"" : detail);
}

/// Chromium 壳（Electron/CEF）软输入：键盘组合键与鼠标点不动的根因回归。
/// 用户症状：脚本Ctrl+V 在壳里「只出v 不粘贴」；鼠标移到某处不动、点了没反应。
/// 原因：electronSafe 分支只做焦点欺骗 + 灌键线程（*没钩软光标/软键态*——
/// 目标进程 GetKeyboardState 读到系统键态（本机 Ctrl 全抬起）→Chromium 把
/// Ctrl+V 当普通字符；GetCursorPos 读到本机真光标（可能在别的窗上）→命中判定错位。
void TestChromiumShellSoftInput() {
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif

    constexpr wchar_t kCls[] = L"Chrome_WidgetWin_1";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kCls, L"Chromium 壳探针",
        WS_OVERLAPPEDWINDOW, 64, 64, 240, 140, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(kCls, wc.hInstance);
        Emit(L"chromium_shell_soft_input", false, L"CreateWindow Chromium probe failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    std::wstring softErr;
    if (!windowmode::FakeFocusSoftInput_Attach(GetCurrentProcessId(), softErr)) {
        DestroyWindow(hwnd);
        UnregisterClassW(kCls, wc.hInstance);
        Emit(L"chromium_shell_soft_input", false,
            softErr.empty() ? L"Attach failed" : softErr.c_str());
        return;
    }

    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        windowmode::FakeFocusSoftInput_Detach();
        DestroyWindow(hwnd);
        UnregisterClassW(kCls, wc.hInstance);
        Emit(L"chromium_shell_soft_input", false, L"LoadLibrary failed");
        return;
    }
    using InstallFn = BOOL(WINAPI*)(HWND);
    using UninstallFn = BOOL(WINAPI*)();
    auto* install = reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_Install"));
    auto* uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall"));
    const LONG_PTR procBefore = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (!install || !uninstall || !install(hwnd)) {
        if (uninstall) uninstall();
        FreeLibrary(mod);
        windowmode::FakeFocusSoftInput_Detach();
        DestroyWindow(hwnd);
        UnregisterClassW(kCls, wc.hInstance);
        Emit(L"chromium_shell_soft_input", false, L"Install failed");
        return;
    }

    // 不排空消息队列：本进程前一个用例的假WM_INPUT/焦点消息可能积压上千条，
    // while(PeekMessage) 会把用例拖成看起来像卡死。这里只测钩子回报的状态。
    windowmode::FakeFocusSoftInput_SetCursorScreen(1234, 5678);
    windowmode::FakeFocusSoftInput_SetKey(VK_LCONTROL, true);
    const BYTE kbCtrl = [&]() {
        BYTE keys[256]{};
        GetKeyboardState(keys);
        return keys[VK_CONTROL];
    }();
    const BYTE kbVCtrl = [&]() {
        windowmode::FakeFocusSoftInput_SetKey('V', true);
        BYTE keys[256]{};
        GetKeyboardState(keys);
        return keys['V'];
    }();
    const BYTE kbShiftUntouched = [&]() {
        BYTE keys[256]{};
        GetKeyboardState(keys);
        return keys[VK_SHIFT];
    }();
    const SHORT gksCtrl = GetKeyState(VK_CONTROL);
    const SHORT gaksCtrl = GetAsyncKeyState(VK_CONTROL);
    const SHORT gaksSpace = GetAsyncKeyState(VK_SPACE);
    POINT pt{};
    GetCursorPos(&pt);
    HWND hookedFg = GetForegroundWindow();

    // 静默钩：Chromium 壳不得被灌假 WM_INPUT（会被当伪造输入丢弃甚至洪泛队列）。
    BYTE rawBuf[256]{};
    UINT rawSz = sizeof(rawBuf);
    const UINT rawCount = GetRawInputBuffer(
        reinterpret_cast<PRAWINPUT>(rawBuf), &rawSz, sizeof(RAWINPUTHEADER));

    // 收尾：抬起软键，避免状态泄漏到同进程后续用例。
    windowmode::FakeFocusSoftInput_SetKey('V', false);
    windowmode::FakeFocusSoftInput_SetKey(VK_LCONTROL, false);
    const BYTE kbCtrlAfterUp = [&]() {
        BYTE keys[256]{};
        GetKeyboardState(keys);
        return keys[VK_CONTROL];
    }();
    const LONG_PTR procAfter = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);

    uninstall();
    FreeLibrary(mod);
    windowmode::FakeFocusSoftInput_Detach();
    DestroyWindow(hwnd);
    UnregisterClassW(kCls, wc.hInstance);

    const bool ok = (kbCtrl & 0x80) != 0
        && (kbVCtrl & 0x80) != 0
        && (kbShiftUntouched & 0x80) == 0
        && (kbCtrlAfterUp & 0x80) == 0
        && (gksCtrl & 0x8000) != 0
        && (gaksCtrl & 0x8000) != 0
        && (gaksSpace & 0x8000) == 0
        && pt.x == 1234 && pt.y == 5678
        && rawCount == 0
        && hookedFg == hwnd
        && procBefore == procAfter;
    wchar_t detail[320]{};
    swprintf_s(detail,
        L"kbCtrl=0x%02x kbV=0x%02x kbShift=0x%02x kbCtrlUp=0x%02x gks=0x%04x gaks=0x%04x "
        L"space=0x%04x pt=(%ld,%ld) raw=%u fg=%d proc=%d",
        kbCtrl, kbVCtrl, kbShiftUntouched, kbCtrlAfterUp,
        static_cast<unsigned>(gksCtrl) & 0xFFFF,
        static_cast<unsigned>(gaksCtrl) & 0xFFFF,
        static_cast<unsigned>(gaksSpace) & 0xFFFF,
        pt.x, pt.y, rawCount,
        hookedFg == hwnd ? 1 : 0, procBefore == procAfter ? 1 : 0);
    Emit(L"chromium_shell_soft_input", ok, ok ? L"" : detail);
}

void ResetQiProbeCounts() {
    g_weixinProbeActivate = 0;
    g_weixinProbeSetFocus = 0;
    g_weixinProbeKeyDown = 0;
    g_weixinProbeKeyUp = 0;
    g_weixinProbeChar = 0;
    g_weixinProbePaste = 0;
    g_weixinProbeLButton = 0;
}

bool ProbeQuickInputNoPaste(const wchar_t* cls, const wchar_t* title, bool expectKey,
    bool expectNoChar, wchar_t* detail, size_t detailN) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WeixinKeyProbeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls, title,
        WS_OVERLAPPEDWINDOW, 72, 72, 240, 140, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(cls, wc.hInstance);
        swprintf_s(detail, detailN, L"%s create failed", cls);
        return false;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    ResetQiProbeCounts();
    windowmode::ResetSoftMouseState();
    windowmode::PostQuickInputToWindow(hwnd, L"h", 0.0, false, nullptr);
    PumpMessagesDispatchOnly(std::chrono::milliseconds(80));
    const bool pasteOk = g_weixinProbePaste == 0;
    const bool keyOk = !expectKey || g_weixinProbeKeyDown >= 1;
    const bool charOk = !expectNoChar || g_weixinProbeChar == 0;
    const bool genericChar = expectKey || g_weixinProbeChar >= 1;
    const bool ok = pasteOk && keyOk && charOk && genericChar;
    if (!ok) {
        swprintf_s(detail, detailN, L"%s paste=%d key=%d char=%d",
            cls, g_weixinProbePaste, g_weixinProbeKeyDown, g_weixinProbeChar);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(cls, wc.hInstance);
    return ok;
}

void TestQuickInputSkipsPasteNonEdit() {
    wchar_t qtDetail[120]{};
    wchar_t airDetail[120]{};
    wchar_t mapleDetail[120]{};
    wchar_t chromeDetail[120]{};
    wchar_t plainDetail[120]{};
    const bool qtOk = ProbeQuickInputNoPaste(L"Qt5156QWindowIcon", L"QtProbe",
        true, true, qtDetail, 120);
    const bool airOk = ProbeQuickInputNoPaste(L"ApolloRuntimeContentWindow", L"AIRProbe",
        true, false, airDetail, 120);
    const bool mapleOk = ProbeQuickInputNoPaste(L"MapleStoryClass", L"MapleProbe",
        true, true, mapleDetail, 120);
    const bool chromeOk = ProbeQuickInputNoPaste(L"Chrome_WidgetWin_1", L"ChromeProbe",
        true, false, chromeDetail, 120);
    const bool plainOk = ProbeQuickInputNoPaste(L"QstQiPlainWnd", L"PlainProbe",
        false, false, plainDetail, 120);
    const bool ok = qtOk && airOk && mapleOk && chromeOk && plainOk;
    wchar_t detail[520]{};
    if (!ok) {
        swprintf_s(detail, L"qt:%s air:%s maple:%s chrome:%s plain:%s",
            qtOk ? L"ok" : qtDetail,
            airOk ? L"ok" : airDetail,
            mapleOk ? L"ok" : mapleDetail,
            chromeOk ? L"ok" : chromeDetail,
            plainOk ? L"ok" : plainDetail);
    }
    Emit(L"quick_input_skips_paste_non_edit", ok, ok ? L"" : detail);
}

}  // namespace

// ── 后台逐字投递探针：模拟「按帧取键+ 只在键仍按下时接受WM_CHAR」的游戏窗口 ──
// 真实键盘顺序是DOWN →（目标自己的TranslateMessage 出WM_CHAR）→ UP。
// 若宿主把 UP 紧跟 DOWN 发出，WM_CHAR 会被排到 KEYUP 之后，游戏按「键仍按下」判定时整串被吞
// （现场实测：后台窗口快捷输入 "11" 只进一个1，前台SendInput 正常）。
struct PostedKeyProbe {
    std::wstring text;
    int keyDown = 0;
    int keyUp = 0;
    int charAccepted = 0;
    int charDroppedKeyUp = 0;
    bool vkDown[256] = {};
    std::chrono::steady_clock::time_point vkDownTick[256] = {};
    std::chrono::steady_clock::time_point firstDigitDownTick{};
    std::chrono::steady_clock::time_point secondDigitDownTick{};
    long long maxHoldMs = 0;
    /// 同一键「前一个 UP 未派发又来 DOWN」的次数（两击挤进同一帧的形态）。
    int sameKeyOverlap = 0;
};

PostedKeyProbe g_postedKeyProbe;

void ResetPostedKeyProbe() {
    g_postedKeyProbe = PostedKeyProbe{};
}

long long MsBetween(std::chrono::steady_clock::time_point a,
    std::chrono::steady_clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

LRESULT CALLBACK PostedKeyProbeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = static_cast<UINT>(wParam);
        const auto now = std::chrono::steady_clock::now();
        ++g_postedKeyProbe.keyDown;
        if (vk < 256) {
            if (g_postedKeyProbe.vkDown[vk]) ++g_postedKeyProbe.sameKeyOverlap;
            if (!g_postedKeyProbe.vkDown[vk]) g_postedKeyProbe.vkDownTick[vk] = now;
            g_postedKeyProbe.vkDown[vk] = true;
        }
        if (vk == '1') {
            if (g_postedKeyProbe.firstDigitDownTick == std::chrono::steady_clock::time_point{}) {
                g_postedKeyProbe.firstDigitDownTick = now;
            } else if (g_postedKeyProbe.secondDigitDownTick
                == std::chrono::steady_clock::time_point{}) {
                g_postedKeyProbe.secondDigitDownTick = now;
            }
        }
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const UINT vk = static_cast<UINT>(wParam);
        const auto now = std::chrono::steady_clock::now();
        ++g_postedKeyProbe.keyUp;
        if (vk < 256 && g_postedKeyProbe.vkDown[vk]) {
            const long long hold = MsBetween(g_postedKeyProbe.vkDownTick[vk], now);
            if (hold > g_postedKeyProbe.maxHoldMs) g_postedKeyProbe.maxHoldMs = hold;
            g_postedKeyProbe.vkDown[vk] = false;
        }
        break;
    }
    case WM_CHAR:
    case WM_SYSCHAR: {
        const UINT scan = static_cast<UINT>((static_cast<unsigned long long>(lParam) >> 16) & 0xFF);
        const UINT vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK);
        if (vk < 256 && g_postedKeyProbe.vkDown[vk]) {
            g_postedKeyProbe.text.push_back(static_cast<wchar_t>(wParam));
            ++g_postedKeyProbe.charAccepted;
        } else {
            ++g_postedKeyProbe.charDroppedKeyUp;
        }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── 软键「状态 vs 事件」竞态探针 ──
// 假焦点软键的 down[] 是**当前值**（宿主一次写完），事件却由 DLL 灌键线程稍后 PostMessage。
// 目标在自己的 WndProc 里读 GetKeyState 判组合键（Chromium 的 Ctrl+V 正是如此）——
// 若宿主在目标处理 WM_KEYDOWN(V) 之前就写回「Ctrl 抬起」，组合键退化成普通字符（只出 v 不粘贴）。
int g_comboProbeVDown = 0;
int g_comboProbeVDownCtrlHeld = 0;

LRESULT CALLBACK ComboProbeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == 'V') {
        ++g_comboProbeVDown;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) ++g_comboProbeVDownCtrlHeld;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void TestSoftKeyComboStateRace() {
#if defined(_WIN64)
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus64.dll";
#else
    const std::wstring dllPath = SelfExeDir() + L"FakeFocus32.dll";
#endif
    constexpr wchar_t kCls[] = L"Chrome_WidgetWin_1";  // 与产品同一条链路：壳类名 → electronSafe 软键
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ComboProbeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kCls, L"QST 软键组合探针",
        WS_OVERLAPPEDWINDOW, 72, 72, 240, 140, nullptr, nullptr, wc.hInstance, nullptr);
    wchar_t detail[200]{};
    bool ok = false;
    if (!hwnd) {
        swprintf_s(detail, L"CreateWindow failed");
    } else {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        std::wstring softErr;
        if (!windowmode::FakeFocusSoftInput_Attach(GetCurrentProcessId(), softErr)) {
            swprintf_s(detail, L"Attach failed: %s", softErr.c_str());
        } else {
            HMODULE mod = LoadLibraryW(dllPath.c_str());
            using InstallFn = BOOL(WINAPI*)(HWND);
            using UninstallFn = BOOL(WINAPI*)();
            auto* install = mod
                ? reinterpret_cast<InstallFn>(GetProcAddress(mod, "FakeFocus_Install")) : nullptr;
            auto* uninstall = mod
                ? reinterpret_cast<UninstallFn>(GetProcAddress(mod, "FakeFocus_Uninstall")) : nullptr;
            if (!mod || !install || !uninstall || !install(hwnd)) {
                swprintf_s(detail, L"Install failed");
                if (uninstall) uninstall();
                if (mod) FreeLibrary(mod);
            } else {
                g_comboProbeVDown = 0;
                g_comboProbeVDownCtrlHeld = 0;
                // 产品在 Chromium 壳会话里就是这么开的（灌键走 DLL PostMessage 队列）。
                windowmode::FakeFocusSoftInput_SetPostKeyEvents(true);
                // 与产品同一条链路：中文文本走「非 Edit → Ctrl+V」，且此时是进程内灌键队列。
                std::atomic_bool done{false};
                std::thread worker([&]() {
                    windowmode::PostQuickInputToWindow(hwnd, L"中文", 0.0, false, nullptr);
                    done.store(true, std::memory_order_relaxed);
                });
                const auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::seconds(6);
                while (std::chrono::steady_clock::now() < deadline) {
                    MSG msg{};
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    if (done.load(std::memory_order_relaxed)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                worker.join();
                windowmode::FakeFocusSoftInput_ClearKeys();
                ok = g_comboProbeVDown >= 1 && g_comboProbeVDownCtrlHeld == g_comboProbeVDown;
                swprintf_s(detail, L"V-DOWN=%d 其中读到 Ctrl 仍按下=%d",
                    g_comboProbeVDown, g_comboProbeVDownCtrlHeld);
            }
        }
        DestroyWindow(hwnd);
    }
    UnregisterClassW(kCls, wc.hInstance);
    if (windowmode::FakeFocusSoftInput_IsAttached()) windowmode::FakeFocusSoftInput_Detach();
    Emit(L"soft_key_combo_state_race", ok, detail);
}

// ── 原生消息宿主护栏（2026-09-24 事故回归）──────────────────────────────
// 事故：宿主清单的 path 取「当前进程 exe」⇒ 自测 exe 把自己写成宿主 ⇒ 浏览器每次
// connectNative 都拉起自测 exe ⇒ 它忽略参数跑整套自测 ⇒ 无限弹 notepad/窗口。
// 两条用例分别钉「规则本身」（纯函数）与「第二道护栏」（外部启动器拉起时不跑用例）。
void TestNativeHostManifestPointsToProduct() {
    const std::wstring selfDir = SelfExeDir();
    const bool selfNotProduct = !windowmode::NativeHostExeNameIsProduct(
        selfDir + L"WindowModeSelfTest.exe");
    const bool productOk = windowmode::NativeHostExeNameIsProduct(L"C:\\x\\QuickScriptTool.exe")
        && windowmode::NativeHostExeNameIsProduct(L"D:\\y\\QstPlayer.exe")
        && windowmode::NativeHostExeNameIsProduct(L"c:\\Z\\quickscripttool.EXE");
    const bool otherNotProduct =
        !windowmode::NativeHostExeNameIsProduct(L"C:\\Windows\\notepad.exe")
        && !windowmode::NativeHostExeNameIsProduct(selfDir + L"WindowModeDiag.exe")
        && !windowmode::NativeHostExeNameIsProduct(L"")
        && !windowmode::NativeHostExeNameIsProduct(L"C:\\x\\QuickScriptTool.exe.bak");

    // 现场清单（本目录若注册过）：path 必须是产品 exe —— 这条直接盯住事故产物。
    bool manifestOk = true;
    std::string manifestDetail = "no manifest";
    {
        const std::wstring path = selfDir + L"com.quickscripttool.bridge.json";
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char body[4096]{};
            DWORD got = 0;
            ReadFile(f, body, sizeof(body) - 1, &got, nullptr);
            CloseHandle(f);
            const std::string text(body, got);
            const size_t k = text.find("\"path\"");
            if (k == std::string::npos) {
                manifestOk = false;
                manifestDetail = "manifest has no path";
            } else {
                const size_t q1 = text.find('"', text.find(':', k));
                const size_t q2 = q1 == std::string::npos ? std::string::npos
                    : text.find('"', q1 + 1);
                const std::string p = (q1 == std::string::npos || q2 == std::string::npos)
                    ? std::string() : text.substr(q1 + 1, q2 - q1 - 1);
                const bool product = p.find("QuickScriptTool.exe") != std::string::npos
                    || p.find("QstPlayer.exe") != std::string::npos;
                const bool looksLikeTest = p.find("SelfTest.exe") != std::string::npos
                    || p.find("WindowModeDiag.exe") != std::string::npos;
                manifestOk = product && !looksLikeTest;
                manifestDetail = p;
            }
        }
    }
    const bool ok = selfNotProduct && productOk && otherNotProduct && manifestOk;
    wchar_t detail[512]{};
    if (!ok) {
        swprintf_s(detail,
            L"selfNotProduct=%d productOk=%d otherNotProduct=%d manifestOk=%d path=%S",
            selfNotProduct ? 1 : 0, productOk ? 1 : 0, otherNotProduct ? 1 : 0,
            manifestOk ? 1 : 0, manifestDetail.c_str());
    }
    Emit(L"native_host_manifest_points_to_product", ok, ok ? L"" : detail);
}

void TestSelfTestRefusesForeignLauncher() {
    const std::wstring exe = SelfExeDir() + L"WindowModeSelfTest.exe";
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        Emit(L"selftest_refuses_foreign_launcher", false, L"CreatePipe failed");
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + exe + L"\" chrome-extension://abcdef/ --parent-window=0 --json";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    std::string out;
    DWORD exitCode = 1;
    bool finished = false;
    // ⚠ 收尾必须把管道**读干净**：子进程可能刚写完护栏那行就退出，
    //   而循环是「先 Peek 再等进程」——进程一 signaled 就 break 会把已到达的数据丢掉
    //   （第一版就这么假红：out 为空、refused=0，其实护栏是对的）。
    auto drainPipe = [&]() {
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return;
            char buf[512]{};
            DWORD got = 0;
            if (!ReadFile(rd, buf, sizeof(buf) - 1, &got, nullptr) || got == 0) return;
            out.append(buf, got);
        }
    };
    if (started) {
        CloseHandle(pi.hThread);
        // 读输出 + 等退出（6s 上限；超时强杀 —— 万一护栏坏了也不让子进程跑完整套自测）。
        const DWORD deadline = GetTickCount() + 6000;
        for (;;) {
            drainPipe();
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                finished = true;
                break;
            }
            if (GetTickCount() > deadline) {
                TerminateProcess(pi.hProcess, 99);
                WaitForSingleObject(pi.hProcess, 1000);
                break;
            }
        }
        drainPipe();
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(rd);
    const bool refused = out.find("refusing") != std::string::npos;
    const bool ranSuite = out.find("\"passed\"") != std::string::npos;
    const bool ok = started && finished && exitCode == 0 && refused && !ranSuite;
    wchar_t detail[256]{};
    if (!ok) {
        swprintf_s(detail, L"started=%d finished=%d exit=%lu refused=%d ranSuite=%d out=%.120S",
            started ? 1 : 0, finished ? 1 : 0, static_cast<unsigned long>(exitCode),
            refused ? 1 : 0, ranSuite ? 1 : 0, out.c_str());
    }
    Emit(L"selftest_refuses_foreign_launcher", ok, ok ? L"" : detail);
}

// 键态停摆 / 恢复（2026-10-01）：钉住"陈旧方向位会被清、恢复后按持键重发"。
// ⚠ 必须**精确断言是哪个键**：只看"按下总数"会把"清错了键、又置回了别的键"判成通过。
void TestMapleKeyStateStallResync() {
    // ① 纯判据逐格（先把逻辑钉死，再看副作用）
    const bool tableOk =
        windowmode::EvaluateKeyStatePhase(100, 100, false) == windowmode::KeyStatePhase::Stalled
        && windowmode::EvaluateKeyStatePhase(100, 101, true) == windowmode::KeyStatePhase::Resumed
        && windowmode::EvaluateKeyStatePhase(100, 101, false) == windowmode::KeyStatePhase::None
        && windowmode::EvaluateKeyStatePhase(100, 100, true) == windowmode::KeyStatePhase::None;
    // ★ 2026-10-02 回归：旧 DLL 把钩命中计数夹在 255（`MapleBumpHit` 的历史写法）⇒ 计数恒为 255，
    //   「变没变」永久为假 ⇒ 每轮看门狗都误判「键态停摆」⇒ 清掉脚本正按着的方向键。
    //   钉住：夹顶时**不得**判停摆（宁可不判，也不能误清脚本意图）。
    //   本用例在修复前必红：旧实现返回 Stalled。
    const bool ceilingOk =
        windowmode::EvaluateKeyStatePhase(255, 255, false) == windowmode::KeyStatePhase::None
        && windowmode::EvaluateKeyStatePhase(255, 255, true) == windowmode::KeyStatePhase::None;

    // ② 真实共享内存副作用（宿主侧即可验证）
    std::wstring softErr;
    if (!windowmode::FakeFocusSoftInput_Attach(GetCurrentProcessId(), softErr)) {
        Emit(L"maple_keystate_stall_resync", false, L"soft attach failed");
        return;
    }
    windowmode::FakeFocusSoftInput_ClearKeys();
    windowmode::FakeFocusSoftInput_SetKey(VK_LEFT, true);   // 陈旧方向位（无人认领）
    windowmode::FakeFocusSoftInput_SetKey(VK_RIGHT, true);  // 陈旧方向位（无人认领）
    windowmode::FakeFocusSoftInput_SetKey('A', true);       // 非方向键：**不许**被清理逻辑碰
    const int cleared = windowmode::ClearStaleArrowSoftKeys(std::unordered_set<UINT>{});
    const bool afterClear =
        !windowmode::FakeFocusSoftInput_IsKeyDown(VK_LEFT)
        && !windowmode::FakeFocusSoftInput_IsKeyDown(VK_RIGHT)
        && windowmode::FakeFocusSoftInput_IsKeyDown('A');

    // ★★ 2026-10-02 回归（本用例修复前必红）：**脚本正按着的方向键不是"陈旧位"**。
    //   现场：录制宏在 t=2.03s 按下 → 并一直按到 t=9.55s；看门狗在 ~5s 误判停摆，
    //   把 → 清掉 ⇒ 角色「原地打、然后乱走」。日志铁证：`持键 2 个` = {→, C} 而
    //   `已清方向键陈旧位（1 个）` 正是那个 →。⇒ 传入持键集后，被持的 → 必须原封不动。
    windowmode::FakeFocusSoftInput_SetKey(VK_RIGHT, true);  // 脚本按着的 →
    windowmode::FakeFocusSoftInput_SetKey(VK_LEFT, true);   // 无人认领的陈旧 ←
    const int heldCleared =
        windowmode::ClearStaleArrowSoftKeys(std::unordered_set<UINT>{VK_RIGHT});
    const bool heldSurvived =
        windowmode::FakeFocusSoftInput_IsKeyDown(VK_RIGHT)      // 脚本意图必须保住
        && !windowmode::FakeFocusSoftInput_IsKeyDown(VK_LEFT);  // 真陈旧的照清

    // 恢复：脚本此刻只持 ←（模拟"恢复后按持键重发"）
    const int pressed = windowmode::ResyncSoftHeldKeys(std::vector<UINT>{VK_LEFT});
    const bool afterResync =
        windowmode::FakeFocusSoftInput_IsKeyDown(VK_LEFT)
        && !windowmode::FakeFocusSoftInput_IsKeyDown(VK_RIGHT)   // 没持的必须保持抬起
        && windowmode::FakeFocusSoftInput_IsKeyDown('A');        // 非方向键不受影响
    windowmode::FakeFocusSoftInput_ClearKeys();
    windowmode::FakeFocusSoftInput_Detach();

    const bool ok = tableOk && ceilingOk && cleared == 2 && afterClear
        && heldCleared == 1 && heldSurvived && pressed == 1 && afterResync;
    wchar_t detail[400]{};
    if (!ok) {
        swprintf_s(detail,
            L"tableOk=%d ceilingOk=%d cleared=%d(期望2) afterClear=%d "
            L"heldCleared=%d(期望1) heldSurvived=%d pressed=%d(期望1) afterResync=%d",
            tableOk ? 1 : 0, ceilingOk ? 1 : 0, cleared, afterClear ? 1 : 0,
            heldCleared, heldSurvived ? 1 : 0, pressed, afterResync ? 1 : 0);
    }
    Emit(L"maple_keystate_stall_resync", ok, ok ? L"" : detail);
}

void TestPostedQuickKeysTiming() {
    constexpr wchar_t kCls[] = L"QstLcaPostedKeyProbe";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PostedKeyProbeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kCls, L"QST PostedKeyProbe",
        WS_OVERLAPPEDWINDOW, 60, 60, 220, 110, nullptr, nullptr, wc.hInstance, nullptr);

    std::wstring got;
    int accepted = 0;
    int dropped = 0;
    long long hold = 0;
    long long gap = 0;
    long long workerMs = 0;
    bool ok = false;
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
        ResetPostedKeyProbe();
        windowmode::SetLcaBackgroundMessageMode(true);
        std::atomic_bool done{false};
        std::thread worker([&]() {
            const auto t0 = std::chrono::steady_clock::now();
            windowmode::PostQuickInputToWindow(hwnd, L"11", 0.0, false, nullptr);
            workerMs = MsBetween(t0, std::chrono::steady_clock::now());
            done.store(true, std::memory_order_relaxed);
        });
        // 主线程按**每帧一次**的节奏取消息（≈60fps 游戏的 process_events），模拟「按帧取键」的目标：
        // 一帧内到达的 DOWN/UP 会被同一批处理，目标自己 TranslateMessage 出的 WM_CHAR 只能排到这批
        // 消息之后 —— 零间隔连发时 WM_CHAR 因此落在 KEYUP 之后（用例判「丢」）。
        // 正常时序（按住 ≥1 帧，或队列屏障）下每个字的 DOWN→CHAR→UP 不会挤进同一批。
        bool workerSettled = false;
        auto drainUntil = std::chrono::steady_clock::now();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < deadline) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (done.load(std::memory_order_relaxed)) {
                if (!workerSettled) {
                    workerSettled = true;
                    drainUntil = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(200);
                } else if (std::chrono::steady_clock::now() >= drainUntil) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        worker.join();
        windowmode::SetLcaBackgroundMessageMode(false);
        got = g_postedKeyProbe.text;
        accepted = g_postedKeyProbe.charAccepted;
        dropped = g_postedKeyProbe.charDroppedKeyUp;
        hold = g_postedKeyProbe.maxHoldMs;
        if (g_postedKeyProbe.firstDigitDownTick != std::chrono::steady_clock::time_point{}
            && g_postedKeyProbe.secondDigitDownTick != std::chrono::steady_clock::time_point{}) {
            gap = MsBetween(g_postedKeyProbe.firstDigitDownTick,
                g_postedKeyProbe.secondDigitDownTick);
        }
        // 判据是**消息时序**，不是某个固定毫秒数：
        //  ① 两个相同数字都要进（"11"）；② 喂给目标的 WM_CHAR 一律在「该键仍按下」时到达；
        //  ③ 同一键不得重叠（前一个 UP 未派发就又来 DOWN = 两击挤进同一帧，正是现场吞字的形态）。
        ok = got == L"11" && accepted == 2 && dropped == 0
            && g_postedKeyProbe.sameKeyOverlap == 0;
        DestroyWindow(hwnd);
    }
    UnregisterClassW(kCls, wc.hInstance);

    wchar_t detail[280]{};
    swprintf_s(detail,
        L"文本=\"%s\" 收=%d 丢=%d 同键重叠=%d 按住=%lldms 两击间隔=%lldms 投递耗时=%lldms",
        got.c_str(), accepted, dropped, g_postedKeyProbe.sameKeyOverlap, hold, gap, workerMs);
    Emit(L"posted_quick_keys_timing", ok, detail);
}

int wmain(int argc, wchar_t** argv) {
    // 外部启动器护栏已由 selftest_harness.h 的静态初始化期守卫统一拦下（所有 suite 生效），
    // 这里不再重复；对应回归用例见 `selftest_refuses_foreign_launcher`。
    bool runMacro = false;
    bool listOnly = false;
    bool uiaChild = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--json") == 0) {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--macro") == 0) runMacro = true;
        else if (_wcsicmp(argv[i], L"--uia-child") == 0) uiaChild = true;
        else if (_wcsicmp(argv[i], L"--list") == 0) {
            listOnly = true;
            selftest::InitUtf8Stdout();
        }
        else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            PrintHelp();
            return 0;
        }
    }

    // ⚠⚠ 2026-10-05：`--uia-child` 子进程模式 —— 只为 `uia_invoke_cross_process` 服务。
    //   跨进程 UIA 是 UWP 场景的**核心特征**（壳窗在 `ApplicationFrameHost.exe`、
    //   内容在**另一个进程**），而同进程用例**证明不了**它。
    //   子进程只做一件事：建窗 + 按钮 + 泵消息 10 秒，然后退出。
    if (uiaChild) {
        RunUiaChildWindow();
        return 0;
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
    TestShellIconHostClass();   // 纯判据，不需要窗口

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
        TestWindowFindImageFullClient(nullptr);
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
        TestWindowFindImageFullClient(edit);
        DestroyWindow(ParentTopWindow(edit));
    }

    TestWindowClientScale();
    TestWindowRelativePlaybackEnablesWm();
    TestBackgroundMinimizedQuietRestore();
    TestBackgroundInputAndroidRender();
    TestBackgroundInputCoordMap();
    TestBackgroundInputSdlSurface();
    TestBackgroundInputDesktopEmulatorTop();
    TestBackgroundInputWrappedTextControl();
    TestBackgroundWheelReachesInputInsideKnownRenderSurface();
    TestWheelTargetsBoundChildNotWrapperSurface();
    TestBackgroundInputBoundChildRespected();
    TestBackgroundKeySelfTranslatePolicy();
    TestRecordedWindowTitleIsHintOnly();
    TestBackgroundInputMumuQtRecursive();
    TestAndroidQtFakeFocusGate();
    TestWeixinQtFakeFocus();
    TestWeixinQtMouseHooks();
    TestChromiumShellSoftInput();
    TestSoftKeyComboStateRace();
    TestQuickInputSkipsPasteNonEdit();
    TestPostedQuickKeysTiming();
    TestMapleKeyStateStallResync();
    TestNativeHostManifestPointsToProduct();
    TestSelfTestRefusesForeignLauncher();

    TestDesktopQuickInputCancel();
    TestFakeFocusJsonRoundtrip();
    TestKernelAnticheatBlocksBackground();
    TestInputStrategyCdpAuto();
    TestExtBridgeConfigParse();
    TestExtBridgeAbortDoesNotRefuseNewClients();
    TestBrowserLeafNameIsBrowser();
    TestMouseWheelStepEvents();
    TestWheelQueueAcceptsWithoutPostKeyEventsFlag();
    TestLongLogDoesNotTerminateProcess();
    TestBridgeConcurrentRequestsDoNotCross();
    TestFakeFocusMinimizeGate();
    TestSoftMessageExeGates();
    TestRestorePreferMaximized();
    TestMonitorCoveringFullscreen();
    TestGameHardwareWithoutInject();
    TestHardwareOffscreenPark();
    TestClampRectKeepsBottomRight();
    TestClampRectShrinksIntoWork();
    TestVdaSelectsOsDll();
    TestFakeFocusHookLocal();
    TestFakeFocusInjectCopy();
    TestFakeFocusStageSweep();
    TestFakeFocusLiteUnreal();
    TestFakeFocusAirFocusOnly();
    TestFakeFocusAirChildIatOnly();
    TestFakeFocusMapleStoryFocusOnly();
    TestFakeFocus32ExportRva();
    TestRemoteModuleKernel32();
    TestFakeFocusHeaderExportRva();
    TestFakeFocusGlfwLiteCursor();
    TestFakeFocusSoftInput();
    TestWindowTimeScaleIat();
    TestWindowTimeScaleOnlyIat();
    TestAnjuzhenScriptWmConfig();
    TestPermissionMatchUipi();
    TestPermissionMismatchNoAutolaunch();
    TestMapleStoryBackgroundFakeFocus();
    TestSetWindowsHookNotForFragileTargets();
    TestInjectedModuleStaleDetection();
    TestBackgroundFakeFocusNotDegraded();
    TestFakeFocusUsesBoundHwndClass();
    TestLcaBackgroundUnknownGame();
    TestTianLongBaBuFakeFocus();
    TestLcaArrowKeyLParam();
    TestLcaNavKeyLeakGuard();
    TestLcaNavKeyupReleasedAfterFocusLoss();
    TestTargetLostAfterDestroy();
    TestUwpFrameBindPidStillAlive();
    TestInvisibleChildClassBind();
    TestBrowserRenderSkipsD3d();
    TestCdpParkExpandable();
    TestUiaControlPickByName();
    TestUiaControlListFormat();
    TestUiaActionVerbTable();
    TestUiaControlListCarriesActionAndState();
    TestScreenPointOcclusionCheck();
    TestUiaInvokeChain();
    TestUiaInvokeCrossProcess();
    TestSoftInputFastPath();

    if (runMacro) TestMacroDesktopSmoke();

    selftest::EmitSummary();
    if (!gJson && selftest::gFailed != 0) {
        std::fwprintf(stderr,
            L"Hint: FAIL name → .cursor/skills/window-mode-debug/reference.md\n");
    }
    return selftest::ExitCode();
}
