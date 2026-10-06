#include "window_mode_executor.h"

#include "background_uia_input.h"
#include "background_window_input.h"
#include "background_input_target.h"
#include "action_utils.h"
#include "input/foreground_input_router.h"
#include "coord_space.h"
#include "ext_bridge/ext_bridge_server.h"
#include "window_mode_log.h"
#include "window_mode_requirements.h"
#include "window_capture.h"
#include "window_coords.h"
#include "window_mode_permission.h"
#include "window_target.h"
#include "window_list.h"
#include "virtual_desktop_accessor.h"
#include "fake_focus/fake_focus_soft_input_host.h"
#include "injection/inject_technique.h"
#include "mouse_wheel_events.h"

#include "action_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <thread>

namespace windowmode {

WindowModeExecutor::WindowModeExecutor() = default;
WindowModeExecutor::~WindowModeExecutor() {
    if (active_) EndRun();
}


namespace {

int g_mapleSoftKeyLogs = 0;
int g_mapleSoftClickLogs = 0;
/// 软键屏障无应答（目标不应答/权限）后，本次运行不再逐键等待。
bool g_softKeyPacingOff = false;

/// 把 mapleDiag 里的运行期「命中」高位解成人话，附在安装日志后面。
/// 这些高位全 0 而 gaks/diState 也全 0 ⇒ 客户端根本不走这些 API（不是钩子没装上）。
/// 背景：星辰冒险岛后台只原地平A 的日志里 gfw/gaks/diState/lastCb 全 0，光看计数器
/// 分不清「没装上」和「没调用」，这一行就是用来区分的。
std::wstring MaplePollHitSummary(DWORD diag) {
    struct Ent {
        DWORD bit;
        const wchar_t* name;
    };
    static const Ent kEnts[] = {
        {0x0020000u, L"GetKeyState"},
        {0x0040000u, L"GetKeyboardState"},
        {0x0080000u, L"GetCursorPos"},
        {0x0100000u, L"GetProcAddress"},
    };
    std::wstring out;
    for (const Ent& e : kEnts) {
        if ((diag & e.bit) == 0) continue;
        if (!out.empty()) out += L"+";
        out += e.name;
    }
    if (out.empty()) out = L"无";
    return out;
}

void LogMapleHookHits(const wchar_t* when) {
    DWORD gaks = 0;
    DWORD diState = 0;
    DWORD diData = 0;
    DWORD lastCb = 0;
    DWORD hitReady = 0;
    DWORD gfw = 0;
    DWORD focus = 0;
    if (FakeFocusSoftInput_ReadMapleHits(gaks, diState, diData, lastCb, hitReady, gfw, focus)) {
        // ⚠ 2026-10-05 改名：原来叫「冒险岛钩命中」，但**对任何目标都会打**
        //   （10-04 为了给 Unity/UE/GLFW 也出数据而放开了条件）⇒ 用户看到
        //   「冒险岛…」会以为日志写错了。改成通用名，并**标出哪几个字段只对冒险岛有效**。
        WindowModeLogEventf(
            L"[窗口/后台窗口模式] 假焦点钩命中 %s hitReady=%lu "
            L"[仅冒险岛有效: gfw=%lu focus=%lu gaks=%lu diState=%lu diData=%lu] lastCb=%lu",
            when ? when : L"",
            static_cast<unsigned long>(hitReady),
            static_cast<unsigned long>(gfw),
            static_cast<unsigned long>(focus),
            static_cast<unsigned long>(gaks),
            static_cast<unsigned long>(diState),
            static_cast<unsigned long>(diData),
            static_cast<unsigned long>(lastCb));
        // ⚠ 2026-10-04：上面那行里 `gfw/focus/gaks/diState/diData` 是**冒险岛专用的命中位**
        //   （`MapleBumpHit` 只在 `g_mapleSafe` 时递增，**故意没放开** —— `gaks` 还被
        //   `EvaluateKeyStatePhase` 用作「客户端在不在查键态」的判据，放开会改变既有行为）
        //   ⇒ **非冒险岛目标恒为 0，那是「没人在数」而不是「游戏不走这些入口」**。
        //   真正对通用目标有效的是下面这行的 `泵/WM_INPUT/WM_KEY`。
        // 输入路径体检：这行回答「游戏走哪条路」。判读见 requirements §23。
        DWORD pump = 0, msgInput = 0, msgKey = 0, msgActivate = 0, rsAdds = 0, rsRounds = 0;
        if (FakeFocusSoftInput_ReadMaplePathProbe(pump, msgInput, msgKey, msgActivate,
                rsAdds, rsRounds)) {
            const int downKeys = FakeFocusSoftInput_DownKeyCount();
            const wchar_t* path = L"未知";
            if (pump == 0) {
                // ⚠ lite 模式（UE5 精简）**本就不钩 PeekMessage/GetMessage**
                //   （`InstallRawInputHooks(user32, lite)` 里 `if (lite) return;`）
                //   ⇒ 这里恒 0 属正常，不是「游戏不走钩子」。
                path = L"消息泵都不经我们的钩子（缓存/晚解析；⚠ lite 模式本就不钩泵 —— "
                       L"先看「假焦点注入技术=… lite=?」，lite=1 时恒 0 属正常）";
            } else if (msgInput > 0) {
                path = L"Raw Input(WM_INPUT)：后台天然收不到 → 真后台走不了路";
            } else if (msgKey > 0) {
                path = L"消息驱动(WM_KEYDOWN)：后台不动=它自己按激活态门控";
            } else {
                path = L"泵在被调但没有键/输入消息（键态另走入口）";
            }
            WindowModeLogEventf(
                L"[窗口/后台窗口模式] 假焦点输入体检 %s 泵=%lu WM_INPUT=%lu WM_KEY=%lu "
                L"WM_ACTIVATE=%lu（吞=%lu）软键按下=%d rescan=%lu轮/+%lu槽 ⇒ %s",
                when ? when : L"",
                static_cast<unsigned long>(pump),
                static_cast<unsigned long>(msgInput),
                static_cast<unsigned long>(msgKey),
                static_cast<unsigned long>(msgActivate),
                static_cast<unsigned long>(focus),
                downKeys,
                static_cast<unsigned long>(rsRounds),
                static_cast<unsigned long>(rsAdds),
                path);
            // 假光标实证（2026-10-04）：上面那行只说「游戏有没有走我们的钩子」，
            // 这行说「宿主有没有把光标喂进去」—— `SyncFakeFocusCursor()` 在共享内存
            // 没挂时是**静默 return**，光看「假焦点已注入」永远发现不了。
            int softCx = 0, softCy = 0;
            bool softCursorValid = false, softPostKeyEvents = false;
            if (FakeFocusSoftInput_ReadSoftCursor(softCx, softCy,
                    softCursorValid, softPostKeyEvents)) {
                WindowModeLogEventf(
                    L"[窗口/后台窗口模式] 假焦点软光标 %s 假光标=(%d,%d) cursorValid=%d postKeyEvents=%d"
                    L"（cursorValid=0 ⇒ 宿主没喂光标，GetCursorPos 钩子会回退真光标）",
                    when ? when : L"", softCx, softCy,
                    softCursorValid ? 1 : 0, softPostKeyEvents ? 1 : 0);
            }
        }
    }
    DWORD diag = 0;
    DWORD iatPoll = 0;
    DWORD diVt = 0;
    if (!FakeFocusSoftInput_ReadMapleInstall(diag, iatPoll, diVt)) return;
    const std::wstring pollHit = MaplePollHitSummary(diag);
    WindowModeLogEventf(
        L"[窗口/后台窗口模式] 假焦点钩安装 %s iatPoll=%lu diag=0x%08X foundVt=%lu patchedSlot=%lu heapVt=%lu "
        L"pwPoll=%lu | pollHit=%s gpaIat=%d dinputIat=%d stage=%d fault=%d",
        when ? when : L"",
        static_cast<unsigned long>(iatPoll),
        static_cast<unsigned>(diag),
        static_cast<unsigned long>(diVt & 0xFFu),
        static_cast<unsigned long>((diVt >> 8) & 0xFFu),
        static_cast<unsigned long>((diVt >> 16) & 0xFFu),
        // pwPoll：进程级（堆/主模块之外）补到的键态类缓存指针数。0 = 那条路也没东西可补。
        static_cast<unsigned long>((diVt >> 24) & 0xFFu),
        pollHit.c_str(),
        (diag & 0x0200000u) ? 1 : 0,
        (diag & 0x0400000u) ? 1 : 0,
        // stage：安装阶段号（DLL 写在 diag 最高 8 位）。5=装完；<5 说明安装中途没了，
        // 那个数字就是死亡点（1=入口 2=指针就绪 3=PEB/IAT 扫完 4=DI 钩完 5=全完成）。
        static_cast<int>((diag >> 24) & 0xFFu),
        // fault=1：安装过程中抛过异常（已被 DLL 的 SEH 兜住，游戏保住了，只是没装上）。
        (diag & 0x0800000u) ? 1 : 0);
}

void DebugLog(const wchar_t* msg) {
    WindowModeLog(msg);
}

/// 构建指纹：把本进程 exe + 两个注入 DLL 的「大小@时间」打进日志。
/// 为什么必须有这一行：排查时最常踩的坑是**分析的日志来自旧构建**。
/// 实测踩过 4 次（dist 快照过期 / 换部署目录 / DLL 三天没换 / exe 与 DLL 不同批），
/// 每次都白查一轮。有了这行，任何一份日志都能自证它跑的是哪次构建 ——
/// **看日志第一件事就是核这一行**，对不上就别往下分析。
void LogBuildFingerprint() {
    static const wchar_t* kFiles[] = {
        L"QuickScriptTool.exe", L"FakeFocus32.dll", L"FakeFocus64.dll",
    };
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? std::wstring() : dir.substr(0, slash + 1);
    std::wstring out;
    for (const wchar_t* name : kFiles) {
        const std::wstring path = dir + name;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            out += std::wstring(L" ") + name + L"=缺失";
            continue;
        }
        const ULONGLONG bytes =
            (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        SYSTEMTIME utc{}, local{};
        FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
        if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
        wchar_t one[160]{};
        swprintf_s(one, L" %s=%llu@%04u-%02u-%02u %02u:%02u", name, bytes,
            static_cast<unsigned>(local.wYear), static_cast<unsigned>(local.wMonth),
            static_cast<unsigned>(local.wDay), static_cast<unsigned>(local.wHour),
            static_cast<unsigned>(local.wMinute));
        out += one;
    }
    WindowModeLogEventf(L"[窗口/后台窗口模式] 构建指纹：%s", out.c_str());
}

/// 冒险岛后台走路：客户端「失焦即停输入轮询」（2009 dinput8 靠 WM_ACTIVATE 停，不逐帧查前台）。
/// FakeFocus32/64.dll 的 IAT 钩子只能吞掉**注入之后**到来的失活；如果脚本是在游戏
/// 已经不在前台时点运行的，客户端早在注入前就停了轮询 —— 此时 DI 虚表钩子/软键态
/// 全都不会被读取（诊断就是 diState=0 lastCb=0），后台只剩原地平A。
/// 这里在注入后补一次**真激活**（不用假 WM_ACTIVATE：给冒险岛灌假激活会冻客户端），
/// 让客户端自己把轮询重新打开，随后把前台还给用户窗口；之后它收到的失活由 IAT 吞掉，
/// 轮询就一直是开的，用户切去浏览器看视频也不再影响走路。
/// 只在「还没在轮询」时动手；已经在轮询（前台启动过）就完全不碰前台。
void WakeMapleStoryInputPolling(HWND targetHwnd, const std::atomic_bool* cancelFlag) {
    HWND top = TopLevelTargetWindow(targetHwnd);
    if (!top || !IsWindow(top)) return;
    if (!FakeFocusSoftInput_IsAttached()) return;

    DWORD gaks = 0, diState = 0, diData = 0, lastCb = 0;
    DWORD hitReady = 0, gfw = 0, focus = 0;
    if (FakeFocusSoftInput_ReadMapleHits(gaks, diState, diData, lastCb, hitReady, gfw, focus)
        && diState > 0) {
        return;  // 客户端本来就在轮询：不动前台
    }

    const HWND prevFg = GetForegroundWindow();
    if (prevFg == top || (prevFg && IsChild(top, prevFg))) return;

    std::wstring err;
    if (!ActivateWindow(top, err)) {
        WindowModeLogf(
            L"[窗口/后台窗口模式] 冒险岛后台走路：唤醒输入轮询失败（切不到前台）: %s", err.c_str());
        return;
    }
    WindowModeLog(
        L"[窗口/后台窗口模式] 冒险岛后台走路：客户端在注入前已失焦停轮询，已临时激活以恢复（马上还前台）");
    for (int i = 0; i < 30; ++i) {
        WindowModeSleepInterruptible(cancelFlag, std::chrono::milliseconds(20));
        if (WindowModeCancelled(cancelFlag)) break;
        DWORD a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0;
        if (FakeFocusSoftInput_ReadMapleHits(a, b, c, d, e, f, g) && b > 0) break;
    }
    DWORD a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0;
    const bool woke =
        FakeFocusSoftInput_ReadMapleHits(a, b, c, d, e, f, g) && b > 0;

    if (prevFg && IsWindow(prevFg) && prevFg != top) {
        std::wstring backErr;
        if (ActivateWindow(prevFg, backErr)) {
            WindowModeLog(L"[窗口/后台窗口模式] 冒险岛后台走路：已把前台还给用户窗口");
        } else {
            WindowModeLogf(L"[窗口/后台窗口模式] 冒险岛后台走路：还原前台失败（可手动点回）: %s",
                backErr.c_str());
        }
    }
    if (woke) {
        WindowModeLog(
            L"[窗口/后台窗口模式] 冒险岛后台走路：客户端输入轮询已恢复（之后的失活由 IAT 吞掉，"
            L"切走也会继续走）");
    } else {
        WindowModeLog(
            L"[窗口/后台窗口模式] 冒险岛后台走路：仍未见 DirectInput 轮询（diState=0）——"
            L"请先把游戏切到前台再点运行，或在游戏里按一下方向键再运行");
    }
}

// ── 冒险岛后台会话：停摆看门狗 + 循环对齐 + 目标身份（2026-09-29，A/B/C 三项）──────
// 现场判据（用户 09-28/09-29 日志）：
//   活跃态：`泵=2924 WM_KEY=172 gaks=255 软键按下=1~2 rescan=10轮/+0槽`
//   休眠态：`泵=0 gaks=0 软键按下=0 rescan=+0槽` —— 客户端**彻底停摆**，喂什么都没用。
// 用户实证：**手动点一下游戏窗口就恢复** ⇒ 这里把「叫醒」自动化，并对齐到每个循环开始。

/// 方向键判据（休眠期"只发抬起不发按下"规则用）。
/// 只收方向键：用户现场的症状（一直往一个方向冲）就是方向键；攻击/技能键在停摆期
/// 本来也不会生效，但**不**在这里替脚本做取舍 —— 少改一类键就少一类回归。
bool IsArrowKeyVk(UINT vk) {
    return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
}
}  // namespace

// ── 键态停摆 / 恢复：**对外可见**（自检要直接调用，见 window_mode_executor.h 的声明）──
/// 旧版 DLL 把钩命中计数夹在这个值上（`MapleBumpHit` 的历史写法）。夹住之后它再也变不了，
/// 「变没变」就不再是「客户端还在不在查键态」的判据 —— 这时**宁可什么都不做**：
/// 判成停摆会去清脚本正按着的方向键，而判据又永远无法再变真 ⇒ 按住的键永久丢失。
/// 新版 DLL 已去掉夹取（计数器径直越过 255），所以这个分支只在
/// 「目标进程里还残留着旧 DLL」时命中，是给「同一份 DLL 被反复注入」兜底的。
/// 判据用**恰好等于**而不是 `>=`：新版计数会长期 `> 255`，用 `>=` 会把新功能永久废掉。
constexpr DWORD kLegacyGaksClampCeiling = 255;

/// 键态轮询的相位（纯函数，自检逐格钉住 —— 见 WindowModeSelfTest 的 `maple_keystate_stall_resync`）。
/// 判据取自用户 2026-10-01 日志：后台坏状态是「消息在泵（`泵=172 WM_KEY=36`）但**键态一次都不查**
/// （`gaks=0 吞=0`）」⇒ 攻击（消息驱动）照打、移动（轮询键态）读不到；
/// 而客户端在"还按着 ←"时停了轮询，就**永远看不到那次松开**，于是恢复后一路顶墙。
KeyStatePhase EvaluateKeyStatePhase(DWORD prevGaks, DWORD nowGaks, bool wasStalled) {
    // ⚠⚠ 夹顶（旧 DLL）⇒ 计数停在同一值不代表客户端停了轮询，判据失效 ⇒ 不判。
    //   前科（用户 2026-10-02 日志）：`gaks` 一到 255 就恒为 255 ⇒ 每次看门狗都判「键态停摆」
    //   ⇒ 清掉脚本正按着的 →（`已清方向键陈旧位（1 个）`、`持键 2 个`={→,C}）⇒
    //   角色「原地打、然后往左、偶尔往右」，且因 Resumed 永不触发而**再也回不来**。
    if (nowGaks == kLegacyGaksClampCeiling && prevGaks == kLegacyGaksClampCeiling) {
        return KeyStatePhase::None;
    }
    // 计数变化 = 客户端正在查键态（哪怕只是查了一次光标以外的键）。
    const bool polling = (nowGaks != prevGaks);
    if (!polling && !wasStalled) return KeyStatePhase::Stalled;
    if (polling && wasStalled) return KeyStatePhase::Resumed;
    return KeyStatePhase::None;
}

/// 清掉共享内存里**方向键**的陈旧按下位。
/// ⚠⚠ `held` = 脚本此刻**真正按着**的键（`softHeldKeys_`）—— 这些**一个都不许清**。
///   脚本意图不是"陈旧位"：清掉它等于凭空丢一次按下，而判据（键态恢复）可能永远不触发，
///   于是这次按下永久丢失 ⇒ 用户看到「角色不走/乱走」。
///   本函数只该清「没人认领却还按着」的位（真陈旧），判据就是「不在 `held` 里」。
/// 返回清掉的方向键个数。
int ClearStaleArrowSoftKeys(const std::unordered_set<UINT>& held) {
    if (!FakeFocusSoftInput_IsAttached()) return 0;
    int cleared = 0;
    const UINT arrows[] = {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN};
    for (UINT vk : arrows) {
        if (held.count(vk)) continue;  // 脚本按着的：是意图，不是陈旧位
        if (!FakeFocusSoftInput_IsKeyDown(vk)) continue;
        FakeFocusSoftInput_SetKey(vk, false);
        ++cleared;
    }
    return cleared;
}

/// 恢复轮询后按脚本持键重新对齐：持键的补按下、方向键里没持的全部置抬起。
/// 这样客户端"恢复轮询后读到的第一帧"就是正确状态，不会带着陈旧方向继续走。
/// 返回重新按下（补发）的键数。
int ResyncSoftHeldKeys(const std::vector<UINT>& held) {
    if (!FakeFocusSoftInput_IsAttached()) return 0;
    int pressed = 0;
    for (UINT vk : held) {
        if (!FakeFocusSoftInput_IsKeyDown(vk)) ++pressed;
        FakeFocusSoftInput_SetKey(vk, true);
    }
    const UINT arrows[] = {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN};
    for (UINT vk : arrows) {
        bool isHeld = false;
        for (UINT h : held) {
            if (h == vk) { isHeld = true; break; }
        }
        if (!isHeld && FakeFocusSoftInput_IsKeyDown(vk)) FakeFocusSoftInput_SetKey(vk, false);
    }
    return pressed;
}

namespace {

/// 客户端「活着的程度」：多个计数任一在涨就算活着。
/// ⚠ 不能只看 `diState` —— 实测活跃态里可以是 `WM_KEY=172 gaks=255` 而 `diState=0`
/// （客户端走消息路径、没走 DirectInput），只认 diState 会把活的误判成停摆、反复抢前台。
ULONG MapleClientProgress() {
    DWORD gaks = 0, diState = 0, diData = 0, lastCb = 0, hitReady = 0, gfw = 0, focus = 0;
    ULONG sum = 0;
    if (FakeFocusSoftInput_ReadMapleHits(gaks, diState, diData, lastCb, hitReady, gfw, focus)) {
        sum += gaks + diState + diData + focus + hitReady;
    }
    DWORD pump = 0, msgInput = 0, msgKey = 0, msgActivate = 0, rsAdds = 0, rsRounds = 0;
    if (FakeFocusSoftInput_ReadMaplePathProbe(pump, msgInput, msgKey, msgActivate,
            rsAdds, rsRounds)) {
        sum += pump + msgKey + msgInput + msgActivate;
    }
    return sum;
}

/// 用户最近是否**没有**键鼠输入（`GetLastInputInfo`）。
/// 用途：自动叫醒会让游戏短暂抢到前台，**如果用户正在打字，字符就会落进游戏**
/// （2026-09-30 用户实测"变成在游戏里面乱按"）。所以叫醒前必须先确认用户此刻没在操作。
bool UserInputIdleForMs(DWORD ms) {
    LASTINPUTINFO li{};
    li.cbSize = sizeof(li);
    if (!GetLastInputInfo(&li)) return true;  // 拿不到就别挡着（罕见路径）
    const DWORD now = GetTickCount();
    const DWORD last = li.dwTime;
    if (now < last) return true;  // 回绕
    return (now - last) >= ms;
}

/// 自动"叫醒"（真激活抢前台）是否开启。默认**开**，但只用于**会话开始时的一次性叫醒**；
/// 周期性叫醒（会话中/每轮）**一律不做** —— 那会让用户打字落进游戏（实测事故）。
/// 需要完全关掉时设 `QST_MAPLE_NO_AUTO_WAKE=1`。
bool MapleAutoWakeEnabled() {
    static const bool on = []() {
        wchar_t buf[8]{};
        const DWORD n = GetEnvironmentVariableW(L"QST_MAPLE_NO_AUTO_WAKE", buf, 8);
        return !(n > 0 && buf[0] == L'1');
    }();
    return on;
}

/// 等客户端「动」起来（B：循环开始对齐）。
/// 先白看 probeMs：活跃态本来就该在这段时间里涨；不涨且允许叫醒 → 补一次真激活，
/// 再等 budgetMs 看有没有涨。返回 true = 已在动（含被叫醒）。
/// ⚠ `allowWake=false` 是纯观察（给别的调用方判断用），绝不碰前台。
bool WaitMapleClientAlive(HWND top, const std::atomic_bool* cancelFlag,
    int budgetMs, bool allowWake, const wchar_t* phase) {
    if (!top || !IsWindow(top) || !FakeFocusSoftInput_IsAttached()) return true;
    ULONG before = MapleClientProgress();
    for (int waited = 0; waited < 240; waited += 20) {
        WindowModeSleepInterruptible(cancelFlag, std::chrono::milliseconds(20));
        if (WindowModeCancelled(cancelFlag)) return true;
        if (MapleClientProgress() != before) return true;
    }
    if (!allowWake) return false;
    // ⚠ 抢前台有很重的副作用（用户打字会落进游戏）⇒ 两道闸：
    //   ① 只有"会话开始时的一次性叫醒"会传 allowWake=true（周期性路径一律 false）；
    //   ② 用户此刻在操作就**不叫**，如实打日志，让用户自己决定要不要点一下。
    if (!MapleAutoWakeEnabled()) {
        static bool loggedOff = false;
        if (!loggedOff) {
            loggedOff = true;
            WindowModeLog(
                L"[窗口/后台窗口模式] 冒险岛后台走路：客户端停摆，自动叫醒已被关闭"
                L"（QST_MAPLE_NO_AUTO_WAKE=1）—— 请手动点一下游戏窗口。");
        }
        return false;
    }
    if (!UserInputIdleForMs(3000)) {
        static bool loggedBusy = false;
        if (!loggedBusy) {
            loggedBusy = true;
            WindowModeLog(
                L"[窗口/后台窗口模式] 冒险岛后台走路：客户端停摆，但检测到你正在操作键鼠 ⇒"
                L"**不抢前台**（避免你的输入落进游戏）。若脚本不动，请手动点一下游戏窗口。");
        }
        return false;
    }
    WindowModeLogf(
        L"[窗口/后台窗口模式] 冒险岛后台走路：%s 时客户端没在动（计数不涨）→ 补一次真激活叫醒",
        phase ? phase : L"会话中");
    WakeMapleStoryInputPolling(top, cancelFlag);
    before = MapleClientProgress();
    for (int waited = 0; waited < budgetMs; waited += 50) {
        WindowModeSleepInterruptible(cancelFlag, std::chrono::milliseconds(50));
        if (WindowModeCancelled(cancelFlag)) return true;
        if (MapleClientProgress() != before) {
            WindowModeLogf(
                L"[窗口/后台窗口模式] 冒险岛后台走路：已叫醒（%s 后客户端开始取消息/查键态）",
                phase ? phase : L"激活");
            return true;
        }
    }
    WindowModeLogf(
        L"[窗口/后台窗口模式] 冒险岛后台走路：仍未见客户端活动（%s）——若动作停在原地，"
        L"请手动点一下游戏窗口；这行说明补激活没起作用",
        phase ? phase : L"激活");
    return false;
}

/// 目标身份（C）：hwnd/pid/类/客户区/标题 + **同类名顶层窗口数**。
/// 为什么要它：双开时两份客户端类名相同（`MapleStoryClass`），一份活跃一份停摆，
/// 而「输入到底发给了哪一份」以前在日志里查不到 —— 用户反馈过「动作跑到另外一个窗口」。
struct MapleTargetIdentity {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    int cw = 0;
    int ch = 0;
    wchar_t cls[128]{};
    wchar_t title[192]{};
    int sameClass = 0;
};

BOOL CALLBACK MapleCountSameClassProc(HWND h, LPARAM lp) {
    auto* id = reinterpret_cast<MapleTargetIdentity*>(lp);
    if (!id || h == id->hwnd || !IsWindowVisible(h)) return TRUE;
    wchar_t cls[128]{};
    if (!GetClassNameW(h, cls, 128) || _wcsicmp(cls, id->cls) != 0) return TRUE;
    ++id->sameClass;
    return TRUE;
}

MapleTargetIdentity DescribeMapleTarget(HWND top) {
    MapleTargetIdentity id{};
    id.hwnd = top;
    if (!top || !IsWindow(top)) return id;
    GetClassNameW(top, id.cls, 128);
    GetWindowTextW(top, id.title, 192);
    GetWindowThreadProcessId(top, &id.pid);
    RECT rc{};
    if (GetClientRect(top, &rc)) {
        id.cw = static_cast<int>(rc.right - rc.left);
        id.ch = static_cast<int>(rc.bottom - rc.top);
    }
    EnumWindows(&MapleCountSameClassProc, reinterpret_cast<LPARAM>(&id));
    return id;
}

void LogMapleTargetIdentity(HWND top, const wchar_t* when) {
    const MapleTargetIdentity id = DescribeMapleTarget(top);
    WindowModeLogf(
        L"[窗口/后台窗口模式] 目标身份 %s hwnd=0x%p pid=%lu 类=%s 客户区=%dx%d 标题=\"%s\" 同类名窗口=%d",
        when ? when : L"", reinterpret_cast<void*>(id.hwnd),
        static_cast<unsigned long>(id.pid), id.cls, id.cw, id.ch, id.title, id.sameClass);
    if (id.sameClass > 0) {
        WindowModeLog(
            L"[窗口/后台窗口模式] ⚠ 还存在同类名的其它顶层窗口（双开/多开？）——"
            L"上面那行的 hwnd 就是本会话实际投递的那一个；若动作跑到另一份客户端上，请把这行发我");
    }
}

/// 会话中看门狗（A）：每 5s 让调用方看一次「客户端还在不在动」，停摆就补真激活。
/// 只在后台模式 + 冒险岛 + 假焦点已生效时工作（前台模式不需要，也绝不去抢前台）。
void MaybeWakeMapleStoryWatchdog(HWND targetHwnd, const std::atomic_bool* cancelFlag) {
    const HWND top = TopLevelTargetWindow(targetHwnd);
    if (!top || !IsWindow(top)) return;
    // ⚠ allowWake=false：**周期性路径永不抢前台**（每轮都抢=用户打字落进游戏，实测事故）。
    //   这里只做观察与记账，真正的叫醒只发生在会话开始时那一次。
    WaitMapleClientAlive(top, cancelFlag, 0, false, L"会话中");
}

void ClientMatchResultsToScreen(HWND hwnd, ImageMatchOutput& output) {
    if (!hwnd || !IsWindow(hwnd)) return;
    for (auto& m : output.matches) {
        ClientToScreenPoint(hwnd, m.topLeftX, m.topLeftY, m.topLeftX, m.topLeftY);
        ClientToScreenPoint(hwnd, m.bottomRightX, m.bottomRightY, m.bottomRightX, m.bottomRightY);
        ClientToScreenPoint(hwnd, m.x, m.y, m.x, m.y);
    }
}

void MapMatchPointBetweenWindows(HWND fromHwnd, HWND toHwnd, int& x, int& y) {
    if (!fromHwnd || !toHwnd || fromHwnd == toHwnd) return;
    int sx = 0;
    int sy = 0;
    ClientToScreenPoint(fromHwnd, x, y, sx, sy);
    ScreenToClientPoint(toHwnd, sx, sy, x, y);
}

void MapMatchResultsToInputClient(HWND captureHwnd, HWND inputHwnd, ImageMatchOutput& output) {
    if (!captureHwnd || !inputHwnd || captureHwnd == inputHwnd) return;
    for (auto& m : output.matches) {
        MapMatchPointBetweenWindows(captureHwnd, inputHwnd, m.topLeftX, m.topLeftY);
        MapMatchPointBetweenWindows(captureHwnd, inputHwnd, m.bottomRightX, m.bottomRightY);
        MapMatchPointBetweenWindows(captureHwnd, inputHwnd, m.x, m.y);
    }
}


UINT VkFromMouseButton(MouseButtonType button) {
    switch (button) {
    case MouseButtonType::Right: return VK_RBUTTON;
    case MouseButtonType::Middle: return VK_MBUTTON;
    case MouseButtonType::X1: return VK_XBUTTON1;
    case MouseButtonType::X2: return VK_XBUTTON2;
    default: return VK_LBUTTON;
    }
}

bool FillClientSizeFromHwnd(HWND hwnd, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    outW = std::max(0, static_cast<int>(rc.right - rc.left));
    outH = std::max(0, static_cast<int>(rc.bottom - rc.top));
    if (outW < 400 || outH < 300) {
        HWND root = GetAncestor(hwnd, GA_ROOT);
        if (root && root != hwnd && GetClientRect(root, &rc)) {
            const int rw = std::max(0, static_cast<int>(rc.right - rc.left));
            const int rh = std::max(0, static_cast<int>(rc.bottom - rc.top));
            if (rw >= 400 && rh >= 300) {
                outW = rw;
                outH = rh;
            }
        }
    }
    return outW > 0 && outH > 0;
}

/// canvas 像素 → 宿主 surface；|sx-sy|>0.05 时强制等比。
void MapCanvasMatchesToSurface(ImageMatchOutput& output, int canvasW, int canvasH,
    int surfaceW, int surfaceH,
    int iframeCssX, int iframeCssY, int iframeCssW, int iframeCssH,
    int pageCssW, int pageCssH) {
    if (canvasW <= 0 || canvasH <= 0 || surfaceW <= 0 || surfaceH <= 0) return;
    double dx = 0, dy = 0, dw = surfaceW, dh = surfaceH;
    if (iframeCssW > 0 && iframeCssH > 0 && pageCssW > 0 && pageCssH > 0) {
        double scaleX = surfaceW / static_cast<double>(pageCssW);
        double scaleY = surfaceH / static_cast<double>(pageCssH);
        if (std::fabs(scaleX - scaleY) > 0.05) {
            const double s = 0.5 * (scaleX + scaleY);
            scaleX = s;
            scaleY = s;
        }
        dx = iframeCssX * scaleX;
        dy = iframeCssY * scaleY;
        dw = std::max(1.0, iframeCssW * scaleX);
        dh = std::max(1.0, iframeCssH * scaleY);
    }
    auto mapPt = [&](int& x, int& y) {
        x = static_cast<int>(std::lround(dx + x * dw / canvasW));
        y = static_cast<int>(std::lround(dy + y * dh / canvasH));
    };
    for (auto& m : output.matches) {
        mapPt(m.topLeftX, m.topLeftY);
        mapPt(m.bottomRightX, m.bottomRightY);
        mapPt(m.x, m.y);
    }
}

/// 可靠 surface：拒绝缩略客户区与严重非等比；优先 pageCss×均匀 dpr。
bool ResolveExtVisionSurface(ExtInputSession& ext, HWND hwnd, int preferW, int preferH,
    int& outW, int& outH) {
    outW = preferW;
    outH = preferH;
    if (outW < 400 || outH < 300) {
        FillClientSizeFromHwnd(TopLevelTargetWindow(hwnd), outW, outH);
    }
    int pw = 0, ph = 0;
    ext.GetPageCssSize(pw, ph);
    const double dpr = ext.DevicePixelRatio();
    if (pw >= 64 && ph >= 64) {
        const double sx = outW > 0 ? outW / static_cast<double>(pw) : 0.0;
        const double sy = outH > 0 ? outH / static_cast<double>(ph) : 0.0;
        const bool tiny = outW < 400 || outH < 300;
        const bool skew = (sx > 0.1 && sy > 0.1 && std::fabs(sx - sy) > 0.05);
        if (tiny || skew) {
            const double s = (dpr > 0.1) ? dpr : 1.5;
            outW = std::max(1, static_cast<int>(std::lround(pw * s)));
            outH = std::max(1, static_cast<int>(std::lround(ph * s)));
            return true;
        }
    }
    if (outW < 400 || outH < 300) {
        outW = 2560;
        outH = 1440;
    }
    return outW >= 64 && outH >= 64;
}

}  // namespace

bool WindowModeExecutor::CheckRunHealth(const WindowModeScriptConfig& config, std::wstring& err) {
    if (!config.enabled) return true;

    if (config.executionKind == WindowModeExecutionKind::BackgroundWindow) {
        if (config.targetExePath.empty()
            && config.windowClassName.empty()
            && config.windowName.empty()
            && config.targetWindowTitle.empty()
            && config.targetPickX == 0 && config.targetPickY == 0) {
            err = L"后台窗口模式请先指定目标窗口";
            return false;
        }

        WindowModeScriptConfig probe = config;
        probe.autoLaunchTarget = false;
        WindowModeSession session;
        if (!session.Start(probe, err)) return false;
        // Start() 对「指定窗口类」会跳过绑窗；这里必须实探一次，否则高完整性游戏
        // 会被当成「路径有效」放行，随后 BeginRun 再误当成未找到去自动打开。
        std::wstring bindErr;
        const bool bound = session.RefreshTarget(bindErr);
        if (ShouldAbortAutoLaunchOnBindFailure(session.State().health)) {
            err = bindErr.empty()
                ? HealthToUserHint(session.State().health)
                : bindErr;
            return false;
        }
        if (bound && session.State().targetHwnd) return true;
        if (!config.targetExePath.empty()) return session.ValidateTargetExe(err);
        err = L"未找到目标窗口，请确认窗口已打开且类名/标题匹配";
        return false;
    }

    if (config.targetExePath.empty()) {
        err = L"请填写目标程序路径";
        return false;
    }

    WindowModeScriptConfig probe = config;
    probe.autoLaunchTarget = true;

    WindowModeSession session;
    if (!session.Start(probe, err)) return false;
    return session.ValidateTargetExe(err);
}

bool WindowModeExecutor::UsesBackgroundWindow() const {
    return active_ && session_.Config().executionKind == WindowModeExecutionKind::BackgroundWindow;
}

bool WindowModeExecutor::CanWindowTimeScale() const {
    return UsesBackgroundWindow() && fakeFocus_.IsInjected()
        && FakeFocusSoftInput_IsAttached();
}

bool WindowModeExecutor::FakeFocusActive() const {
    // 委托注入器：它同时考虑「仅变速注入」和「全屏拆钩」两种情况。
    return fakeFocus_.fake_focus_active();
}

bool WindowModeExecutor::SetWindowTimeScale(double speed) {
    if (!CanWindowTimeScale()) {
        // 静默失败曾让「变速没生效」白查两轮 —— 这里必须出声，并说清是哪一环不满足。
        if (speed > 0.0) {
            WindowModeLogf(
                L"[窗口/后台窗口模式] 窗口变速未下发：后台模式=%d 已注入=%d 共享内存=%d（三者需全为 1）",
                UsesBackgroundWindow() ? 1 : 0, fakeFocus_.IsInjected() ? 1 : 0,
                FakeFocusSoftInput_IsAttached() ? 1 : 0);
        }
        return false;
    }
    const bool written = FakeFocusSoftInput_SetTimeScale(speed);
    if (speed > 0.0) {
        WindowModeLogf(L"[窗口/后台窗口模式] 窗口变速已下发：目标时钟 %.3g 倍（%s）",
            speed, written ? L"已写入共享内存" : L"写入失败");
        timeScaleAppliedTick_ = GetTickCount64();
        timeScaleActive_ = true;
        timeScaleNextLogTick_ = timeScaleAppliedTick_ + 400;  // 等 DLL 轮询跑过再打第一条
        timeScaleLoggedTimes_ = 0;
        timeScaleLastPump_ = 0;      // 节拍探针重新取基准（下一行才是首次读数）
        timeScaleLastPumpTick_ = 0;
    } else {
        WindowModeLog(L"[窗口/后台窗口模式] 窗口变速已复位（目标时钟回到原速）");
        timeScaleActive_ = false;
    }
    return written;
}

void WindowModeExecutor::MaybeLogTimeScaleDiag() {
    if (!timeScaleActive_) return;
    const ULONGLONG now = GetTickCount64();
    if (now < timeScaleNextLogTick_) return;
    // 前 3 条每 2 秒一条（看调用计数涨不涨），之后不再刷屏。
    if (timeScaleLoggedTimes_ >= 3) return;
    ++timeScaleLoggedTimes_;
    timeScaleNextLogTick_ = now + 2000;

    DWORD diag = 0;
    std::wstring err;
    if (!fakeFocus_.QueryTimeScaleDiag(diag, err)) {
        WindowModeLogf(L"[窗口/后台窗口模式] 窗口变速诊断读取失败: %s", err.c_str());
        return;
    }
    const uint32_t scale = (diag >> 16) & 0xFFFFu;
    const uint32_t hooks = (diag >> 8) & 0xFFu;
    // bit4 = 目标是 Unity IL2CPP 且能调 Time.timeScale；bit5 = 已由我们设过。
    // Unity 游戏里这两位才是变速能不能生效的关键。
    WindowModeLogf(
        L"[窗口/后台窗口模式] 窗口变速诊断：DLL 侧 已装=%d 轮询=%d 仅变速=%d 钩已拆=%d 仅IAT=%d 倍率=%u 时间钩=%u "
        L"Unity可用=%d Unity已设=%d",
        (diag & 1u) ? 1 : 0, (diag & 2u) ? 1 : 0, (diag & 4u) ? 1 : 0, (diag & 8u) ? 1 : 0,
        (diag & 64u) ? 1 : 0,
        scale, hooks, (diag & 16u) ? 1 : 0, (diag & 32u) ? 1 : 0);
    // ★「时钟变了但游戏不变速」的判别器：目标**主循环节拍**（消息泵调用次数/秒）。
    // 时钟倍率对「按帧推进」的游戏无效 —— 这一行会直接显示节拍有没有跟着倍率上去：
    //   · 节拍跟着涨（≈倍率倍）→ 游戏确实在按虚拟时钟跑，慢的是别的东西；
    //   · 节拍不涨           → 节拍由 vsync/固定帧/Sleep 封顶，时钟补丁天生改不动它
    //                          （要么解锁帧率，要么这台游戏不适合变速）。
    // 判读同 LESSONS §3：「计数不涨=钩子不在路径；猛涨但不变速=时钟不是决定它的东西」。
    {
        DWORD pump = 0, msgInput = 0, msgKey = 0, msgActivate = 0, rsAdds = 0, rsRounds = 0;
        if (FakeFocusSoftInput_ReadMaplePathProbe(pump, msgInput, msgKey, msgActivate,
                rsAdds, rsRounds)) {
            const ULONGLONG now = GetTickCount64();
            if (timeScaleLastPumpTick_ != 0 && now > timeScaleLastPumpTick_) {
                const DWORD dPump = pump - timeScaleLastPump_;
                const ULONGLONG dMs = now - timeScaleLastPumpTick_;
                const double perSec = dMs > 0
                    ? static_cast<double>(dPump) * 1000.0 / static_cast<double>(dMs) : 0.0;
                WindowModeLogf(
                    L"[窗口/后台窗口模式] 目标主循环节拍：%.1f 次/秒（%.3g 倍时钟下；"
                    L"原速基准看首次那行）—— 节拍不随倍率涨 = 帧率被 vsync/固定帧封顶",
                    perSec, FakeFocusSoftInput_TimeScale());
            }
            timeScaleLastPump_ = pump;
            timeScaleLastPumpTick_ = now;
        }
    }
    // Unity 状态（宿主直接读共享内存，不需远程调用）：
    //   bits0..15 = 进入前读到的 Time.timeScale×1000（0xFFFF = 读不到）
    //   bit16 可用 / bit17 已设 / bit18 get失败 / bit19 set失败 / bit20 后台运行已设
    const uint32_t us = FakeFocusSoftInput_UnityState();
    if (us & (1u << 16)) {
        const uint32_t raw = us & 0xFFFFu;
        WindowModeLogf(
            L"[窗口/后台窗口模式] Unity 变速：进入前 timeScale=%s 已设=%d get失败=%d set失败=%d "
            L"后台运行方法找到=%d 后台运行已设=%d",
            raw == 0xFFFFu ? L"读不到" : std::to_wstring(raw / 1000.0).c_str(),
            (us & (1u << 17)) ? 1 : 0, (us & (1u << 18)) ? 1 : 0,
            (us & (1u << 19)) ? 1 : 0, (us & (1u << 21)) ? 1 : 0,
            (us & (1u << 20)) ? 1 : 0);
        if (!(us & (1u << 17))) {
            WindowModeLog(
                L"[窗口/后台窗口模式] Unity 变速：Time.timeScale 没设上 —— 若 set失败=1 说明 invoke 抛异常；"
                L"若两个都为 0 则是游戏在变速后又把它改回去了");
        }
    }

    // 调用计数是本轮加的硬证据：“不涨”和“涨但游戏不加速”是两种完全不同的故障。
    uint32_t calls[6]{};
    if (FakeFocusSoftInput_TimeHookCalls(calls, 6)) {
        WindowModeLogf(
            L"[窗口/后台窗口模式] 窗口变速调用计数：QPC=%u Tick64=%u Tick=%u FileTime=%u timeGetTime=%u",
            calls[0], calls[1], calls[2], calls[3], calls[4]);
        if (calls[0] == 0u && calls[1] == 0u && calls[2] == 0u && calls[3] == 0u
            && calls[4] == 0u) {
            WindowModeLog(
                L"[窗口/后台窗口模式] 窗口变速：钩子一次都没被调用 —— 目标不走这些 API（钩错地址，或目标用别的时间源），变速不可能生效");
        }
    }
    if (hooks == 0u) {
        WindowModeLog(
            L"[窗口/后台窗口模式] 窗口变速诊断：一个时间函数都没钩上 —— 目标进程不走这些 API，"
            L"或地址判定把候选全拒了");
    }
}

bool WindowModeExecutor::IsCdpInputMode() const {
    return active_ && UsesCdpInput(session_.Config());
}

bool WindowModeExecutor::UsesClientCoords() const {
    return session_.Config().coordSpace == WindowModeCoordinateSpace::WindowClient;
}

HWND WindowModeExecutor::CaptureTargetHwnd() const {
    HWND hwnd = TargetHwnd();
    if (!hwnd) return nullptr;
    const bool childBinding = !session_.Config().childWindowClassName.empty()
        || (GetParent(hwnd) != nullptr && GetParent(hwnd) != GetDesktopWindow())
        || FindBrowserRenderWidget(hwnd) == hwnd;
    if (childBinding) {
        HWND root = GetAncestor(hwnd, GA_ROOT);
        if (root && IsWindow(root)) return root;
    }
    return hwnd;
}

HWND WindowModeExecutor::VisionCaptureHwnd() const {
    HWND target = TargetHwnd();
    HWND root = CaptureTargetHwnd();
    if (!target || !root) return root;

    // CDP+扩展：找图/键鼠坐标统一用顶层客户区，勿绑 D3D/RenderWidget（ScreenToClient 会打偏）。
    if (IsCdpInputMode()) return root;

    // 截图可走合成层；输入绑定单独走 FindBrowserRenderWidget。
    if (HWND surface = FindBrowserCaptureSurface(root)) {
        RECT rc{};
        if (GetClientRect(surface, &rc)) {
            const int w = std::max(0, static_cast<int>(rc.right - rc.left));
            const int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
            if (w > 0 && h > 0) return surface;
        }
    }

    if (target == root) return root;

    RECT rc{};
    if (GetClientRect(target, &rc)) {
        const int w = std::max(0, static_cast<int>(rc.right - rc.left));
        const int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
        if (w > 0 && h > 0 && !IsBrowserCompositorHwnd(target)) return target;
    }

    const auto& st = session_.State();
    if (st.clientW > 0 && st.clientH > 0 && !IsBrowserCompositorHwnd(target)) return target;
    return root;
}

bool WindowModeExecutor::EnsureTargetReady(std::wstring& err) {
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        return RefreshTarget(err);
    }

    // CDP/扩展：不依赖 Win32 可见性；宏桌面查看/最小化都不打断标签页键鼠与找图。
    if (IsCdpInputMode()) {
        err.clear();
        return true;
    }
    if (PreferHardwareInput()) {
        err.clear();
        return true;
    }

    HWND root = TopLevelTargetWindow(hwnd);
    const bool background = UsesBackgroundWindow();
    const bool onUserDesktop = IsWindowOnUserCurrentDesktop(root);

    if (background || onUserDesktop) {
        const bool skipVisibleZOrder = background && !IsTargetWindowMinimized(hwnd);
        if (IsTargetWindowMinimized(hwnd)) {
            // 后台窗口模式：目标保持最小化/离屏，绝不恢复或前置。
            // PostMessage 对最小化窗口仍能送达鼠标/键盘消息；恢复只会让窗口“弹出”，
            // 违背后台操作的预期（用户已明确要求完全不切换前台）。
            if (background) {
                err.clear();
                return true;
            }
            RestoreWindowQuiet(root, skipVisibleZOrder);
            if (IsTargetWindowMinimized(hwnd)) {
                RestoreWindowNonActivating(root);
                if (IsTargetWindowMinimized(hwnd)) {
                    err = L"目标窗口仍处于最小化状态";
                    return false;
                }
            }
        } else if (!background) {
            RestoreWindowQuiet(root, skipVisibleZOrder);
        }
    }
    return true;
}

bool WindowModeExecutor::EnsureTargetBound(std::wstring& err) {
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        return RefreshTarget(err);
    }

    HWND root = TopLevelTargetWindow(hwnd);
    if (!root || !IsWindow(root)) {
        return RefreshTarget(err);
    }
    // 后台：不迁宏桌面。CDP：安静停放（偏离则迁回并最小化），禁止 Cloak/还原。
    if (UsesBackgroundWindow()) {
        err.clear();
        return true;
    }
    if (IsCdpInputMode()) {
        // CDP 找图/键鼠只走扩展；禁止每次 Ensure 再 Park/Move。
        err.clear();
        return true;
    }
    if (PreferHardwareInput()) {
        err.clear();
        return true;
    }
    // softMessage/假焦点：目标须留在「鼠标宏」。
    auto& vda = VirtualDesktopAccessor::Instance();
    const int macroIdx = vda.FindDesktopIndexByName(kMacroDesktopDisplayName);
    if (macroIdx >= 0 && !vda.IsWindowOnDesktopNumber(root, macroIdx)) {
        const int desk = vda.GetWindowDesktopNumber(root);
        if (desk < 0 && IsMacroVisionLatched(root)) {
            // latch 且 desk 未知：跳过狂搬。
        } else {
            EnsureTargetOnMacroDesktop(root, false);
        }
    }
    return true;
}

bool WindowModeExecutor::PrepareVisionCapture() {
    std::wstring err;
    return EnsureTargetBound(err);
}

bool WindowModeExecutor::ResolveOcrScreenRect(const ScriptAction& a,
    int& sx1, int& sy1, int& sx2, int& sy2,
    HBITMAP lockedBmp, int lockX, int lockY) {
    if (!active_) return false;

    if (a.ocrRegionByImage) {
        ScriptAction probe = a;
        ImageMatchOutput output = FindImageClient(probe, lockedBmp, lockX, lockY);
        if (output.matches.empty()) return false;
        const ImageMatchResult& match = output.matches.front();
        int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
        if (!ApplyImageRegionToMatch(a,
                match.topLeftX, match.topLeftY, match.bottomRightX, match.bottomRightY,
                cx1, cy1, cx2, cy2)) {
            return false;
        }
        return MapClientRect(cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2);
    }

    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
    if (!ResolveClientSearchRect(a, cx1, cy1, cx2, cy2)) return false;
    return MapClientRect(cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2);
}

bool WindowModeExecutor::ResolveAiScreenRect(const ScriptAction& a,
    int& sx1, int& sy1, int& sx2, int& sy2,
    HBITMAP lockedBmp, int lockX, int lockY) {
    if (!active_) return false;

    if (a.aiRegionByImage && !a.aiTargetImagePath.empty()) {
        ScriptAction probe = a;
        if (probe.imagePath.empty()) probe.imagePath = probe.aiTargetImagePath;
        // 窗口/后台窗口模式先在整个目标窗口内找图，再用 imageRegion 二次筛选。
        probe.searchFullScreen = true;
        probe.searchX1 = 0;
        probe.searchY1 = 0;
        probe.searchX2 = 0;
        probe.searchY2 = 0;
        ImageMatchOutput output = FindImageClient(probe, lockedBmp, lockX, lockY);
        if (output.matches.empty()) return false;
        const ImageMatchResult& match = output.matches.front();
        int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
        if (!ApplyImageRegionToMatch(a,
                match.topLeftX, match.topLeftY, match.bottomRightX, match.bottomRightY,
                cx1, cy1, cx2, cy2)) {
            return false;
        }
        return MapClientRect(cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2);
    }

    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
    if (!ResolveClientSearchRect(a, cx1, cy1, cx2, cy2)) return false;
    return MapClientRect(cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2);
}

bool WindowModeExecutor::BeginRun(const WindowModeScriptConfig& config, std::wstring& err,
    BeginRunOptions options) {
    // 需求见 window_mode_requirements.h：
    // §1 指定窗口类按身份打开；§2 启动不切宏桌面视图；§3 等待要短。
    if (active_) EndRun();
    // 兜底：上一次运行若是异常结束（本进程被杀/目标闪退），系统里可能还留着我们
    // SendInput 补过、却没配对的 KEYDOWN。新会话开头先全部松开，避免「一开脚本就朝某方向走」。
    ReleaseMirroredLcaNavKeys();
    if (!config.enabled) {
        active_ = false;
        WindowModeLogEvent(L"[窗口/后台窗口模式] BeginRun：窗口模式未启用，跳过（不会创建宏桌面）");
        return true;
    }

    WindowModeLogEventf(L"[窗口/后台窗口模式] BeginRun：窗口模式启用 kind=%s，开始准备宏桌面/绑窗",
        config.executionKind == WindowModeExecutionKind::HiddenDesktop
            ? L"HiddenDesktop" : L"BackgroundWindow");
    ResetSoftMouseState();
    extLayoutFresh_ = false;
    cancelFlag_ = options.cancelFlag;
    session_.SetCancelFlag(options.cancelFlag);
    session_.SetLaunchSearchDir(options.launchSearchDir);

    if (WindowModeCancelled(options.cancelFlag)) {
        err = L"已取消";
        EndRun();
        return false;
    }

    // 统一写入：避免 JSON 里 autoLaunchTarget=0 导致 Start/Bind 提前失败。
    WindowModeScriptConfig runConfig = config;
    runConfig.autoLaunchTarget = ShouldAutoLaunchTarget(runConfig);
    // 模拟器：在进 Session 前先降级，避免后续仍按 HiddenDesktop 分支决策。
    if (runConfig.executionKind == WindowModeExecutionKind::HiddenDesktop
        && ConfigLooksLikeEmulatorTarget(runConfig)) {
        runConfig.executionKind = WindowModeExecutionKind::BackgroundWindow;
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] BeginRun：模拟器目标已改为后台窗口模式（不创建虚拟桌面、不搬窗）");
    }

    if (!session_.Start(runConfig, err)) {
        active_ = false;
        return false;
    }
    // Session::Start 可能再改 executionKind；后续分支以会话为准。
    runConfig = session_.Config();
    WindowModeLogEventf(L"[窗口/后台窗口模式] BeginRun：生效 kind=%s targetExe=%ls",
        runConfig.executionKind == WindowModeExecutionKind::HiddenDesktop
            ? L"HiddenDesktop" : L"BackgroundWindow",
        runConfig.targetExePath.c_str());
    // 每轮开头打一次构建指纹：日志自证「跑的是哪次构建」，避免再拿旧构建的日志做分析。
    LogBuildFingerprint();

    const bool background = runConfig.executionKind == WindowModeExecutionKind::BackgroundWindow;
    const bool shouldAutoLaunch = ShouldAutoLaunchTarget(runConfig);

    auto abortIfBindFatal = [&]() -> bool {
        const WindowModeHealth health = session_.State().health;
        if (!ShouldAbortAutoLaunchOnBindFailure(health)) return false;
        if (err.empty()) {
            err = session_.State().lastError.empty()
                ? HealthToUserHint(health)
                : session_.State().lastError;
        }
        WindowModeLogf(L"[窗口/后台窗口模式] 已找到目标但无法绑定（%s），禁止自动打开以免重复启动搞挂游戏",
            err.c_str());
        EndRun();
        return true;
    };

    if (shouldAutoLaunch) {
        if (options.launchTarget) {
            // 「指定窗口类」绑定后须再校验标题/文档身份：勿把同程序其它窗当成已找到而跳过打开。
            bool bound = session_.RefreshTarget(err);
            if (abortIfBindFatal()) return false;
            if (bound && session_.State().targetHwnd
                && runConfig.selectMethod == WindowSelectMethod::UseEditorWindowClass) {
                HWND top = session_.State().targetHwnd;
                HWND root = GetAncestor(top, GA_ROOT);
                if (!root) root = top;
                if (!DoesTopWindowMatchConfig(root, runConfig)) {
                    WindowModeLog(L"[窗口/后台窗口模式] 已绑窗口与指定标题/文档不符，改为自动打开目标");
                    session_.ClearTargetBinding();
                    bound = false;
                    err.clear();
                }
            }
            if (bound && session_.State().targetHwnd) {
                WindowModeLog(L"[窗口/后台窗口模式] 已找到匹配的目标窗口，跳过自动打开");
            } else {
                WindowModeLogf(L"[窗口/后台窗口模式] 未找到目标窗口，自动打开: %s",
                    runConfig.targetExePath.c_str());
                // 只清绑定，保留宏桌面，避免 Close/Open 虚拟桌面造成卡顿。
                session_.ClearTargetBinding();
                const bool launched = background
                    ? session_.LaunchTargetOnDefaultDesktop(err)
                    : session_.LaunchTargetOnDesktop(err);
                if (!launched) {
                    EndRun();
                    return false;
                }
                // 启动后再次核验指定窗口类身份（防止绑到空白同程序窗）。
                if (runConfig.selectMethod == WindowSelectMethod::UseEditorWindowClass
                    && session_.State().targetHwnd) {
                    HWND top = session_.State().targetHwnd;
                    HWND root = GetAncestor(top, GA_ROOT);
                    if (!root) root = top;
                    if (!DoesTopWindowMatchConfig(root, runConfig)) {
                        err = L"已启动程序，但未绑定到指定标题/文档对应的窗口。"
                              L"请确认脚本目录旁有该文件，或在「指定窗口类」拾取时保存文档路径。";
                        EndRun();
                        return false;
                    }
                }
            }
        } else if (!session_.RefreshTarget(err)) {
            if (abortIfBindFatal()) return false;
            if (!session_.ValidateTargetExe(err)) {
                EndRun();
                return false;
            }
            active_ = false;
            EndRun();
            return true;
        }
    } else if (!session_.RefreshTarget(err)) {
        if (abortIfBindFatal()) return false;
        if (background) {
            if (runConfig.windowClassName.empty() && runConfig.windowName.empty()) {
                err = L"后台窗口模式请先指定目标窗口";
            }
        } else if (runConfig.targetExePath.empty()) {
            err = L"窗口/后台窗口模式已启用，但未配置目标程序";
        }
        EndRun();
        return false;
    }

    const auto& st = session_.State();
    // CDP/扩展走本机桥，不依赖 Win32 跨完整性输入；Chrome 渲染进程还常在 AppContainer，
    // 不能用 UAC 提示拦住（用户以管理员运行本工具时尤其容易误报）。
    if (!UsesCdpInput(runConfig) && st.targetPid != 0 && !CheckPermissionMatch(st.targetPid)) {
        WindowModeLogf(L"[窗口/后台窗口模式] UIPI：无法向目标 pid=%lu 发送输入（目标完整性更高）",
            static_cast<unsigned long>(st.targetPid));
        err = HealthToUserHint(WindowModeHealth::PermissionMismatch);
        EndRun();
        return false;
    }

    if (st.health != WindowModeHealth::Ok) {
        WindowModeLogf(L"[窗口/后台窗口模式] 目标未就绪，结束会话：%s",
            st.lastError.empty() ? HealthToUserHint(st.health) : st.lastError.c_str());
        err = HealthToUserHint(st.health);
        if (!st.lastError.empty()) err = st.lastError;
        EndRun();
        return false;
    }

    {
        HWND acTop = TopLevelTargetWindow(session_.State().targetHwnd);
        if (LooksLikeKernelAntiCheatProtectedTarget(runConfig, acTop)) {
            err = KernelAntiCheatBackgroundUnsupportedHint();
            WindowModeLog(
                L"[窗口/后台窗口模式] 内核反作弊目标（英雄联盟/Valorant 等）：拒绝后台窗口/宏桌面，"
                L"不尝试注入（VirtualAllocEx 会被拒绝，PostMessage 游戏不读）");
            EndRun();
            return false;
        }
    }

    hardwareFallback_ = false;
    // 每次运行重新告警一次：换了个脚本/重开一局之后，用户仍需要看到"为什么没反应"。
    backgroundNoFakeFocusWarned_ = false;
    uwpSkippedFakeFocus_ = false;
    g_softKeyPacingOff = false;  // 每次运行重新尝试「软键屏障」等待
    SetLcaBackgroundMessageMode(false);
    active_ = true;
    {
        const auto strategy = ResolveInputStrategy(runConfig);
        const wchar_t* name = L"softMessage";
        if (strategy == WindowModeInputStrategy::Cdp) name = L"cdp";
        else if (strategy == WindowModeInputStrategy::Auto) name = L"auto";
        WindowModeLogf(L"[窗口/后台窗口模式] 输入策略=%s class=%s cdpPort=%d",
            name, runConfig.windowClassName.c_str(), runConfig.cdpPort);
    }
    {
        // 坐标语义（2026-10-04）：脚本声明的坐标系与「后台窗口模式」的预期不符时，
        // 窗口一旦移动，所有坐标动作都会**整体偏移** —— 而且这是**静默**的：
        // 日志里只有「移动 → 客户区(x,y)」看起来一切正常。用户报障原文：
        //   「后台窗口模式貌似使用的坐标是绝对坐标，窗口移动后就不能使用了」
        //   「编辑宏的时候，后台窗口模式下鼠标不会移动到指定位置」
        // ⚠ 这两句是同一个根因：脚本按屏幕绝对坐标存，回放却用**当前**窗口位置换算。
        const bool rel = runConfig.windowRelativeCoordinates;
        WindowModeLogEventf(
            L"[窗口/后台窗口模式] 坐标语义 %s（coordSpace=%s 录制客户区=%dx%d）",
            rel ? L"窗口客户区相对（动作坐标直接就是客户区像素）"
                : L"屏幕绝对（回放时按**当前**窗口位置换算成客户区）",
            runConfig.coordSpace == windowmode::WindowModeCoordinateSpace::WindowClient
                ? L"windowClient" : L"screenAbsolute",
            runConfig.recordClientWidth, runConfig.recordClientHeight);
        if (!rel && UsesBackgroundWindow()) {
            WindowModeLogEventf(
                L"[窗口/后台窗口模式] ⚠ 本脚本按**屏幕绝对坐标**回放，而后台窗口模式会用"
                L"**当前**窗口位置换算成客户区 ⇒ 窗口一旦被移动（或换了分辨率），"
                L"所有鼠标坐标动作都会整体偏移，表现为「点不到指定位置 / 鼠标像没动」。"
                L"建议：用「后台窗口模式」重新录制该脚本，或改用「窗口模式」。");
        }
    }
    if (UsesCdpInput(runConfig)) {
        // 尽早拉起本机桥，给扩展轮询留时间（扩展约 1.5s 扫一次端口）。
        {
            std::wstring warmErr;
            if (ExtBridgeServer::Instance().Start(warmErr)) {
                WindowModeLog(L"[窗口/后台窗口模式] 本机桥已预热；请保持扩展选项页打开或稍后点「重新连接」");
            }
        }
        std::wstring cdpErr;
        if (!EnsureCdpReady(cdpErr)) {
            err = cdpErr;
            EndRun();
            return false;
        }
        HWND top = TopLevelTargetWindow(session_.State().targetHwnd);
        if (top && IsWindow(top)) {
            if (UsesBackgroundWindow()) {
                EnsureTargetBelowUserWindows(top);
            } else {
                if (!IsMacroVisionCaptureReady(top) && !IsMacroVisionLatched(top)) {
                    PrepareMacroDesktopForCdpBind(top);
                }
                StartCdpMacroDesktopWatchPump(top);
                WindowModeLog(L"[窗口/后台窗口模式] CDP 路径就绪（扩展键鼠/找图；禁 Win32 展开）");
            }
        }
    } else if (LooksLikeChromiumShellTarget(runConfig, session_.State().targetHwnd)) {
        WindowModeLog(L"[窗口/后台窗口模式] Chromium 壳（Electron/CEF）：将优先假焦点 DLL 真后台（注入失败才假前台）");
    } else if (LooksLikeWeixinTarget(runConfig, session_.State().targetHwnd)) {
        WindowModeLog(
            L"[窗口/后台窗口模式] 微信 Qt：将注入精简假焦点（只骗前景查询；键鼠仍 PostMessage，不走 Chromium 灌键）");
    } else if (LooksLikeEmulatorTarget(runConfig, session_.State().targetHwnd)) {
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] 桌面模拟器：将注入精简假焦点（仅前景+键态；禁 RawInput/WM_INPUT）");
    } else {
        HWND topHint = TopLevelTargetWindow(session_.State().targetHwnd);
        const bool weixinQt = LooksLikeWeixinTarget(runConfig, topHint);
        const bool androidEmu = !weixinQt && (
            LooksLikeAndroidEmulatorExecutable(runConfig.targetExePath)
            || LooksLikeAndroidEmulatorWindowClass(runConfig.windowClassName)
            || LooksLikeAndroidEmulatorWindowClass(runConfig.childWindowClassName)
            || LooksLikeAndroidEmulatorWindowTitle(runConfig.windowName)
            || LooksLikeQtRenderWindowClass(runConfig.windowClassName)
            || (topHint && IsWindow(topHint)
                && LooksLikeAndroidEmulatorExecutable(QueryHwndProcessImagePath(topHint))));
        if (androidEmu) {
            const bool qtMumu = AndroidEmulatorPrefersFakeFocusFromConfig(runConfig)
                || (topHint && AndroidEmulatorPrefersFakeFocus(topHint, &runConfig));
            WindowModeLog(qtMumu
                ? L"[窗口/后台窗口模式] MuMu 等 Qt 安卓壳：PostMessage 到渲染子窗（OpenGL；绑子窗坐标）"
                : L"[窗口/后台窗口模式] 安卓模拟器：PostMessage 到渲染子窗"
                  L"（LDPlayer 可设 childWindowClassName=TheRender）");
        } else {
            wchar_t clsHint[256]{};
            wchar_t titleHint[512]{};
            if (topHint) {
                GetClassNameW(topHint, clsHint, 256);
                GetWindowTextW(topHint, titleHint, 512);
            }
            const bool adobeAirHint = LooksLikeAdobeAirWindowClass(clsHint)
                || LooksLikeAdobeAirWindowClass(runConfig.windowClassName)
                || LooksLikeAdobeAirWindowClass(runConfig.childWindowClassName);
            const bool game3d = LooksLikeGameWindowClass(clsHint)
                || LooksLikeGameWindowClass(runConfig.windowClassName)
                || LooksLikeGameWindowClass(runConfig.childWindowClassName);
            const bool mapleHint = LooksLikeMapleStoryWindowClass(clsHint)
                || LooksLikeMapleStoryWindowClass(runConfig.windowClassName)
                || LooksLikeMapleStoryTitle(titleHint)
                || LooksLikeMapleStoryTitle(runConfig.windowName)
                || LooksLikeMapleStoryTitle(runConfig.targetWindowTitle)
                || LooksLikeMapleStoryExecutable(runConfig.targetExePath)
                || LooksLikeMapleStoryExecutable(QueryHwndProcessImagePath(topHint));
            const bool lcaHint = mapleHint
                || PrefersLcaBackgroundMessages(runConfig, topHint);
            WindowModeLog(adobeAirHint
                ? L"[窗口/后台窗口模式] Adobe AIR：将注入假焦点（只骗前景查询；键鼠 PostMessage）"
                : mapleHint
                ? L"[窗口/后台窗口模式] 冒险岛：技能键走窗口消息；将注入精简假焦点（吞失活+DirectInput）以便后台走路"
                : lcaHint
                ? L"[窗口/后台窗口模式] 未登记游戏：不注入假焦点，键鼠走 LCA 后台窗口消息（PostMessage KEY*）"
                : game3d
                ? L"[窗口/后台窗口模式] 3D/游戏窗口：将注入假焦点（钩光标/RawInput/焦点查询）；失败才回退窗口消息"
                : L"[窗口/后台窗口模式] 使用 PostMessage/软消息（非 CDP）");
        }
    }
    loggedClientScale_ = false;
    softInputSkipLogged_ = false;
    wasMinimizedAtBeginRun_ = false;
    {
        HWND top = TopLevelTargetWindow(session_.State().targetHwnd);
        wasMinimizedAtBeginRun_ = top && IsWindow(top) && IsIconic(top);
        // 后台窗口模式（含 CDP 扩展找图）：最小化/托盘隐藏时置底还原以便截图与 Chromium 软输入。
        // 宏桌面 CDP 仍跳过 Win32 展开。
        const bool needsQuietGeo = top && IsWindow(top) && TargetNeedsQuietPlaybackRestore(top);
        if (needsQuietGeo && !(IsCdpInputMode() && !UsesBackgroundWindow())) {
            std::wstring geoErr;
            if (!EnsurePlaybackGeometry(geoErr)) {
                err = geoErr.empty() ? L"目标窗口处于最小化/隐藏状态，无法还原后执行" : geoErr;
                EndRun();
                return false;
            }
        }
    }
    TryInstallFakeFocus();
    // 假焦点未注入时的本机 SendInput：RDP / 独占全屏 / Electron 失败 / 游戏 Raw Input。
    rdpSavedForeground_ = nullptr;
    if (PreferHardwareInput()) {
        HWND top = TopLevelTargetWindow(session_.State().targetHwnd);
        wchar_t cls[256]{};
        if (top) GetClassNameW(top, cls, 256);
        const bool rdp = IsRemoteDesktopPlaybackTarget();
        const bool covering = top && IsWindow(top) && LooksLikeMonitorCoveringFullscreen(top);
        const bool electronShell =
            LooksLikeChromiumShellTarget(runConfig, session_.State().targetHwnd);
        WindowModeLogf(L"[窗口/后台窗口模式] 本机输入目标 class=%s hwnd=0x%p rdp=%d covering=%d fallback=%d",
            cls, top, rdp ? 1 : 0, covering ? 1 : 0, hardwareFallback_ ? 1 : 0);
        if (electronShell) {
            WindowModeLog(
                L"[窗口/后台窗口模式] Chromium 壳：假焦点注入失败，回退屏上假前台 SendInput（会占键盘焦点）");
        } else if (rdp && runConfig.executionKind == WindowModeExecutionKind::HiddenDesktop) {
            WindowModeLog(
                L"[窗口/后台窗口模式] 远程桌面：窗口模式(宏桌面)无效，改为用户桌面假前台"
                L"（与后台窗口模式同一输入路径）");
        } else if (covering) {
            WindowModeLog(
                L"[窗口/后台窗口模式] 独占全屏：假前台 SendInput（相对鼠标；无法真后台）");
        } else {
            WindowModeLog(
                L"[窗口/后台窗口模式] 假焦点未生效（未注入 / 仅时钟补丁 / 钩已拆），回退假前台 SendInput（绝对坐标；会占键鼠）");
        }
        const bool backgroundHw = UsesBackgroundWindow();
        if (!backgroundHw && top && IsWindow(top) && !covering) {
            auto& vda = VirtualDesktopAccessor::Instance();
            std::wstring vdaErr;
            if (vda.EnsureLoaded(vdaErr)) {
                const int userDesk = vda.GetCurrentDesktopNumber();
                const int macroIdx = vda.FindDesktopIndexByName(kMacroDesktopDisplayName);
                if (userDesk >= 0 && macroIdx >= 0
                    && vda.IsWindowOnDesktopNumber(top, macroIdx)
                    && !vda.IsWindowOnDesktopNumber(top, userDesk)) {
                    vda.MoveWindowToDesktopNumberPreservingView(top, userDesk);
                    WindowModeLog(L"[窗口/后台窗口模式] 已从「鼠标宏」迁回用户桌面（本机输入回退）");
                }
            }
        }
        rdpSavedForeground_ = GetForegroundWindow();
        if (rdpSavedForeground_ == top) rdpSavedForeground_ = nullptr;
        hwOffscreenParked_ = false;
        hwParkedHwnd_ = nullptr;
        hwSavedTopmost_ = false;
        hwSavedWp_ = {};
        if (!backgroundHw && top && IsWindow(top) && !covering && !electronShell
            && ParkHardwareInputTargetOffscreen(top, &hwSavedWp_, &hwSavedTopmost_)) {
            hwOffscreenParked_ = true;
            hwParkedHwnd_ = top;
            WindowModeLog(
                L"[窗口/后台窗口模式] 本机输入：已屏外+顶置（仍保持前台焦点；"
                L"不能同时操作其它窗口，结束脚本会还原位置）");
        }
        if (backgroundHw) {
            WindowModeLog(
                L"[窗口/后台窗口模式] 后台窗口：找图保持后台；键鼠在投递前再假前台 SendInput"
                L"（避免切走游戏后相对鼠标打到其它窗）");
        } else if (!EnsureHardwareInputFocus()) {
            WindowModeLog(L"[窗口/后台窗口模式] 未能切到前台，SendInput 可能打到其它窗");
        } else {
            WindowModeLog(L"[窗口/后台窗口模式] 假前台本机输入已激活");
        }
        POINT pt{};
        HWND seedHwnd = TargetHwnd();
        if (seedHwnd && IsWindow(seedHwnd) && GetCursorPos(&pt)) {
            int scx = 0, scy = 0;
            if (ScreenToClientPoint(seedHwnd, pt.x, pt.y, scx, scy)) {
                RememberSoftMouseClientPos(seedHwnd, scx, scy);
                WindowModeLogf(L"[窗口/后台窗口模式] 本机输入软光标播种 客户区(%d,%d)", scx, scy);
            }
        }
    } else if (UsesChromiumShellInProcInput()) {
        FakeFocusSoftInput_SetPostKeyEvents(true);
        WindowModeLog(
            L"[窗口/后台窗口模式] Chromium 壳真后台就绪（焦点欺骗 + 进程内 PostMessage 队列）");
    } else if (UsesBackgroundWindow() && FakeFocusActive()
        && LooksLikeMapleStoryTarget(runConfig, TargetHwnd())) {
        // 后台窗口模式 + 冒险岛：注入前客户端若已失焦停轮询，后台只会原地平A。
        // 先打目标身份（C），再等客户端真的在动才放行动作。
        const HWND mapleTop = TopLevelTargetWindow(TargetHwnd());
        mapleIdentityHwnd_ = mapleTop;
        LogMapleTargetIdentity(mapleTop, L"BeginRun");
        mapleWatchdogNextTick_ = GetTickCount64() + 5000;  // 刚对齐过，5s 内不再看
        mapleWatchdogMisses_ = 0;
        // 叫醒**只允许会话第一次**：用户的宏每个循环都重新 BeginRun，每轮都真激活
        // 会反复把前台抢回游戏（用户实测"打字落进游戏、窗口被拉回前台几秒"）。
        // 第一次之后只观察（allowWake=false），永不再抢前台。
        const bool allowWake = !mapleWokeOnce_;
        WaitMapleClientAlive(mapleTop, cancelFlag_, 2000, allowWake,
            allowWake ? L"会话开始" : L"循环开始");
        mapleWokeOnce_ = true;
    }
    DebugLog(L"[WindowMode] Executor::BeginRun OK");
    return true;
}


void WindowModeExecutor::TryInstallFakeFocus() {
    HWND hwnd = TargetHwnd();
    HWND top = TopLevelTargetWindow(hwnd);
    if (!top) top = hwnd;
    const auto& cfg = session_.Config();
    wchar_t cls[256]{};
    if (top) GetClassNameW(top, cls, 256);
    const bool delphiVcl = HwndLooksLikeDelphiVclGame(top)
        || LooksLikeDelphiVclGameWindowClass(cls)
        || LooksLikeDelphiVclGameWindowClass(cfg.windowClassName)
        || LooksLikeDelphiVclGameWindowClass(cfg.childWindowClassName);

    if (IsRemoteDesktopPlaybackTarget()) {
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] 独占全屏/远程桌面：跳过假焦点注入，键鼠走本机输入");
        return;
    }
    const bool mapleStory = LooksLikeMapleStoryTarget(session_.Config(), top);

    // ── UWP 壳进程：**禁止注入假焦点**（2026-10-05，用户实测崩溃）──────────────────
    //   现象：目标 = UWP 计算器（class=ApplicationFrameWindow、
    //         targetExe=...\ApplicationFrameHost.exe）⇒「移动鼠标然后点击目标窗口没有反应，
    //         而且**结束后目标窗口崩溃**」。
    //   ⚠ 为什么危险：`ApplicationFrameHost.exe` 是**系统壳进程**，一个进程托管**所有**
    //     UWP 应用 ⇒ 注入的影响面远超单个应用；日志也显示注入本身不顺
    //     （setwindowshook 未装入 → classic 失败 → 才勉强成功）。
    //   ★ 为什么**不影响功能**：UWP/WinUI 的 `PostMessage` 本来就无效，它走
    //     `background_uia_input` 的 **UIA Invoke** 兜底，**不依赖假焦点**。
    //   ⇒ **禁止一切注入**（不只假焦点钩，**连时钟补丁也不行**）。
    //   ⚠⚠ 2026-10-06 更正：这里原来写的是「变速仍会走『仅时钟补丁』（那条只改时钟
    //     函数，不装任何假焦点钩）」—— **这个认知是错的**。用户实测：
    //     「时钟补丁」虽然不装假焦点钩，但**同样是把 DLL 塞进壳进程**（走 setwindowshook）
    //     ⇒ 注入后目标**自己退出**（日志 `目标窗口已消失 … exit=0x00000000`），
    //       紧接着下一轮注入报 `VirtualAllocEx 失败: Win32=5（目标已退出）`。
    //   ⇒ 代价（刻意权衡）：**UWP 目标不再支持窗口变速** —— 变速是可选增强，
    //     把目标带走是硬故障；而且壳进程是**共享**的，带走它会影响**所有** UWP 应用。
    const bool uwpShell = LooksLikeUwpShellWindowClass(cls)
        || LooksLikeUwpShellWindowClass(session_.Config().windowClassName)
        || LooksLikeUwpShellWindowClass(session_.Config().childWindowClassName)
        || LooksLikeUwpShellExecutable(session_.Config().targetExePath);

    // ── 变速只需要「目标进程里有代码」，与「这个目标要不要假焦点」完全无关 ──────────
    // 所以先算出来，让下面那些「不需要假焦点就早退」的分支不再把变速一起挡掉 ——
    // 否则 2D 游戏 / 未登记游戏 / 传奇 这类目标永远拿不到变速，而它们恰恰很需要。
    const bool timeScaleWanted = enableWindowTimeScale_ && UsesBackgroundWindow();
    const bool fakeFocusNeeded = !uwpShell
        && (UsesFakeFocusForTarget(session_.Config(), top) || mapleStory);
    if (uwpShell) {
        uwpSkippedFakeFocus_ = true;
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] UWP 壳进程（ApplicationFrameHost）：**跳过假焦点注入**"
            L"（系统进程、影响面大且实测会崩；UWP 走 UIA Invoke 兜底，不依赖假焦点）");
    }
    // 只装时钟补丁（不装任何假焦点钩）的条件：需要变速，且**确实不需要**假焦点。
    // ⚠ 这里**不能**再带 `!enableFakeFocusInjection_`（2026-09-19 修，MC 实测踩到）：
    // 用户关掉假焦点 + 开着变速时，3D/GLFW 目标会被带成「仅时钟补丁」→ 引擎随即回退
    // 假前台 SendInput（绝对坐标，**会抢鼠标/键盘**）→ 用户看到的就是「假后台」。
    // 需要假焦点的目标一律走完整注入，理由同微信/冒险岛：没有假焦点就没有真后台。
    const bool timeScaleOnlyInject =
        ShouldInjectTimeScaleOnly(timeScaleWanted, fakeFocusNeeded);
    // ★ 决策输入必须落盘（2026-10-03 修，见 `window_mode_requirements.h` §22）：
    //   下面**每一条**早退（未登记游戏 / 不需要假焦点 / 内核反作弊 / 仅时钟补丁 / 远程桌面 …）
    //   原来都只走 `WindowModeLog`（非 Event ⇒ **不落盘**）⇒ 用户导出的诊断里
    //   「压根没尝试注入」和「尝试了但失败」长得**一模一样**（都只剩 BeginRun/EndRun）。
    //   现场前科：用户的 10-02 构建从「每轮都有 `假焦点钩命中 注入后`」变成「一行都没有」，
    //   而三行注入诊断**只在 `lite && mapleStory` 分支里打** ⇒ 必须先能区分这几种情况，
    //   否则只能靠读源码猜（这次就是这么耗掉的）。
    WindowModeLogEventf(
        L"[窗口/后台窗口模式] 假焦点决策：mapleStory=%d fakeFocusNeeded=%d timeScaleWanted=%d "
        L"timeScaleOnly=%d class=%s targetExe=%s",
        mapleStory ? 1 : 0, fakeFocusNeeded ? 1 : 0, timeScaleWanted ? 1 : 0,
        timeScaleOnlyInject ? 1 : 0, cls[0] ? cls : L"(空)",
        cfg.targetExePath.empty() ? L"(空)" : cfg.targetExePath.c_str());
    if (PrefersLcaBackgroundMessages(session_.Config(), top)) {
        SetLcaBackgroundMessageMode(true);
        if (!mapleStory) {
            if (!timeScaleWanted) {
                WindowModeLogEvent(
                    L"[窗口/后台窗口模式] 未登记游戏：跳过假焦点注入，键鼠走 LCA 窗口消息（PostMessage KEY*，方向键 KF_EXTENDED）");
                return;
            }
            // 变速只需要目标进程里有代码：这里不早退，
            // 下面会走「仅时钟补丁」（不装任何假焦点钩，键鼠路径一字节不变）。
            WindowModeLog(
                L"[窗口/后台窗口模式] 未登记游戏：不装假焦点，但开着窗口变速 → 仅注入时钟补丁");
        } else {
            WindowModeLog(
                L"[窗口/后台窗口模式] 冒险岛：技能键走 PostMessage；注入精简假焦点吞失活/DirectInput 后台走路");
        }
    }
    // 铺满/远程跳过与 UsesFakeFocusForTarget 同一套例外（传奇 Delphi）。
    // 冒险岛 UsesFakeFocusForTarget=false（保持 PostMessage），但仍须 mapleSafe 精简注入。
    if (!fakeFocusNeeded) {
        if (top && IsWindow(top) && LooksLikeMonitorCoveringFullscreen(top) && !delphiVcl) {
            WindowModeLogEvent(
                L"[窗口/后台窗口模式] 独占全屏/远程桌面：跳过假焦点注入，键鼠走本机输入");
        } else {
            // ★ 落盘：这一条正是「平A 能打、方向键全废」的典型来源（键鼠只剩 PostMessage）。
            WindowModeLogEventf(
                L"[窗口/后台窗口模式] class=%s：未注入假焦点，键鼠走 PostMessage"
                L"（方向键/DirectInput 类目标会表现为「能平A、不能走」）", cls);
        }
        if (!timeScaleWanted) return;
        // ⚠⚠⚠ 2026-10-06：**UWP 壳进程连时钟补丁也不注入** ——
        //   用户实测（嵌套「后台窗口模式」）：注入到 `ApplicationFrameHost.exe` 后，
        //   目标**自己退出**（日志 `目标窗口已消失 … exit=0x00000000`），
        //   紧接着下一轮注入报 `VirtualAllocEx 失败: Win32=5（目标已退出）`。
        //   ⇒ 「时钟补丁」虽然**不装假焦点钩**，但**同样是把 DLL 塞进壳进程**
        //     （走 `setwindowshook`）—— 对 `ApplicationFrameHost.exe` 这种
        //     **系统壳进程**一样危险（它托管着**所有** UWP 应用）。
        //   ⇒ 明确告知「UWP 目标不支持窗口变速」，而不是偷偷注入把目标带走。
        if (LooksLikeUwpShellWindowClass(cls)
            || LooksLikeUwpShellExecutable(cfg.targetExePath)) {
            WindowModeLogEvent(
                L"[窗口/后台窗口模式] ⛔ UWP 壳进程：**连时钟补丁也不注入**"
                L"（往 ApplicationFrameHost.exe 里塞 DLL 会把目标带走；"
                L"UWP 目标不支持窗口变速，键鼠走 UIA/PostMessage）");
            return;
        }
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] 该目标不需要假焦点，但开着窗口变速 → 仅注入时钟补丁");
    }
    if (LooksLikeKernelAntiCheatProtectedTarget(session_.Config(), top)) {
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] 假焦点跳过：内核反作弊目标禁止注入（会被拒绝访问，且有封号风险）");
        return;
    }
    DWORD pid = session_.State().targetPid;
    if (pid == 0 && top) GetWindowThreadProcessId(top, &pid);
    const bool chromiumShell = LooksLikeChromiumShellTarget(cfg, top);
    const bool weixinQt = !chromiumShell && LooksLikeWeixinTarget(cfg, top);
    // timeScaleOnlyInject / timeScaleWanted 已在函数开头算好（变速与假焦点无关）。
    if (!enableFakeFocusInjection_ && !timeScaleOnlyInject) {
        // 微信 4.x Qt 查真 GetForegroundWindow；不注入就会丢键，并在 WM_ACTIVATE 后抢前台。
        if (weixinQt) {
            WindowModeLog(
                L"[窗口/后台窗口模式] 微信 Qt 后台必须注入精简假焦点，已忽略「关闭假焦点注入」设置");
        } else if (mapleStory) {
            WindowModeLog(
                L"[窗口/后台窗口模式] 冒险岛后台走路必须注入精简假焦点，已忽略「关闭假焦点注入」设置");
        } else if (BackgroundTargetRequiresFakeFocus(
                session_.Config(), top, enableFakeFocusInjection_)) {
            // 后台窗口模式 + 3D/游戏目标（GLFW/UE/RawInput 消费者）：不注入假焦点就只能回退
            // 「假前台 SendInput（绝对坐标）」—— 那是**会抢鼠标/键盘的假后台**，用户实测抱怨过
            // （MC/GLFW30）。与微信/冒险岛同理：没有假焦点就没有真后台，忽略该设置。
            WindowModeLog(
                L"[窗口/后台窗口模式] 后台模式 + 3D/游戏目标必须注入假焦点"
                L"（否则只能假前台 SendInput，会抢鼠标/键盘，等于假后台），"
                L"已忽略「关闭假焦点注入」设置");
        } else if (!UsesBackgroundWindow()
            && GameTargetNeedsHardwareWithoutFakeFocus(session_.Config(), top)) {
            hardwareFallback_ = true;
            WindowModeLogEvent(
                L"[窗口/后台窗口模式] 设置未启用假焦点注入：不注入 DLL，"
                L"游戏/3D 目标改走假前台 SendInput（PostMessage 无法驱动 Raw Input）");
            return;
        } else {
            WindowModeLogEvent(
                L"[窗口/后台窗口模式] 设置未启用假焦点注入：不注入 DLL，改走软消息/必要时假前台");
            return;
        }
    }
    if (timeScaleOnlyInject) {
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] 已关闭假焦点注入，但启用了窗口变速：仅注入时钟补丁"
            L"（不装假焦点钩，键鼠仍走软消息/必要时假前台）");
    }
    if (top && IsFakeFocusInjectionUnsupported(top)) {
        WindowModeLogEvent(IsRemoteDesktopWindow(top)
            ? L"[窗口/后台窗口模式] 假焦点跳过：远程桌面注入会搞挂 mstsc，请保持窗口前台用本机输入"
            : L"[窗口/后台窗口模式] 假焦点跳过：真浏览器请用网页兼容（Electron 壳应已放行）");
        return;
    }
    const bool androidQt = AndroidEmulatorPrefersFakeFocus(top, &cfg);
    const bool glfwSdl = LooksLikeGlfwOrSdlWindowClass(cls)
        || LooksLikeGlfwOrSdlWindowClass(cfg.windowClassName)
        || LooksLikeGlfwOrSdlWindowClass(cfg.childWindowClassName);
    const bool native3d = LooksLikeGameWindowClass(cls)
        || LooksLikeGameWindowClass(cfg.windowClassName)
        || LooksLikeGameWindowClass(cfg.childWindowClassName);
    const bool desktopEmu = LooksLikeEmulatorTarget(cfg, top)
        && !IsAndroidEmulatorTarget(top, &cfg);
    const bool adobeAir = LooksLikeAdobeAirWindowClass(cls)
        || LooksLikeAdobeAirWindowClass(cfg.windowClassName)
        || LooksLikeAdobeAirWindowClass(cfg.childWindowClassName);
    // SetWindowsHook 在目标 UI 线程 LoadLibrary。GLFW/Java《我的世界》这一下就会崩。
    // DeSmuME：同样禁止在消息线程装 DLL（启动高概率崩）。
    // ★ Adobe AIR（造梦西游 / 4399 微端）同样在列：AIR 的 UI 线程里 LoadLibrary
    //   一个注入 DLL 会卡死播放器随后退出（本仓已记：AIR「一启动就卡死退出」）。
    //   ⚠ 别因为「默认技术是 classic」就省掉这一条 —— 设置里可以选 setwindowshook。
    //   判据收在 ForbidsSetWindowsHookTechnique（新增脆弱目标只改那一处）。
    if (ForbidsSetWindowsHookTechnique(chromiumShell, weixinQt, androidQt, native3d,
            desktopEmu, adobeAir)
        && fakeFocus_.injection_technique() == inject::Technique::SetWindowsHook) {
        WindowModeLogf(
            L"[窗口/后台窗口模式] %s：注入技术 setwindowshook 改为 classic（避免在游戏线程装 DLL 崩溃）",
            desktopEmu ? L"桌面模拟器"
                : (adobeAir ? L"Adobe AIR"
                : (glfwSdl ? L"GLFW/SDL" : (androidQt ? L"Qt 安卓壳"
                : (weixinQt ? L"微信 Qt"
                : (chromiumShell ? L"Chromium 壳" : L"3D/游戏窗"))))));
        fakeFocus_.SetInjectionTechnique(inject::Technique::ClassicRemoteThread);
    }
    const bool tianlongDx = LooksLikeTianLongBaBuTarget(cfg, top)
        || LooksLikeTianLongBaBuWindowClass(cls)
        || LooksLikeTianLongBaBuWindowClass(cfg.windowClassName)
        || LooksLikeTianLongBaBuWindowClass(cfg.childWindowClassName);
    const bool lite = chromiumShell
        || weixinQt
        || androidQt
        || desktopEmu
        || delphiVcl
        || glfwSdl
        || adobeAir
        || mapleStory
        || tianlongDx
        || LooksLikeUnrealEngineWindowClass(cls)
        || LooksLikeUnrealEngineWindowClass(cfg.windowClassName)
        || LooksLikeUnrealEngineWindowClass(cfg.childWindowClassName);
    const bool emulator = LooksLikeEmulatorTarget(cfg, top);
    std::wstring ffErr;
    // ⚠⚠⚠ 2026-10-06：**UWP 壳进程的总闸** —— 到真正的注入点前**无条件**拦一次。
    //
    //   上面虽然有几条早退（未登记游戏 / 不需要假焦点 / 仅时钟补丁），但那些都是
    //   **条件性**的：只要 `fakeFocusNeeded` 被判成 true（或将来有人改动那段决策），
    //   就会**绕过**它们直接落到这里注入 ⇒ 目标被带走。
    //   ⇒ 在注入点前再放一道**不依赖任何决策变量**的闸，保证「UWP 绝不注入」这条
    //     不变量不靠「上面几条路都记得加判据」来维持。
    if (uwpShell) {
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] ⛔ UWP 壳进程：**禁止一切注入**（含时钟补丁）—— "
            L"往 ApplicationFrameHost.exe 里塞 DLL 会把目标带走；键鼠走 UIA/PostMessage");
        return;
    }
    // Chromium 壳 / 原生 3D（GLFW/Unity/SDL）：只注入窗口 PID。
    // Minecraft javaw 的 helper 子进程没有消息泵，先注入它们会把主进程误记成「跳过」。
    // 仅变速注入时只碰窗口自己的进程：变速只需要游戏循环所在的那个进程。
    fakeFocus_.SetTimeScaleOnly(timeScaleOnlyInject);
    if (!fakeFocus_.InjectAndInstall(pid, top, ffErr, lite,
            timeScaleOnlyInject
                || chromiumShell || weixinQt || native3d || desktopEmu || mapleStory /*windowPidOnly*/)) {
        // ★ 持久化（Event）：注入失败 = 假焦点一个钩都没装上 ⇒ 后台模式**不会**回退假前台
        //   SendInput（见 PreferHardwareInput 的「后台永不抢鼠标」），键鼠只剩 PostMessage
        //   ⇒ 消息驱动的攻击键照打、**方向键（依赖软键态/DirectInput 软键）全部失效**。
        //   用户报障原文就是这个形态：「原地不动的平A，不能走A」。
        //   ⚠ 原来这一条只走非 Event 通道，而那条通道会被 `runningWindowMode_.enabled`
        //     过滤掉（嵌套模式要到 BeginRun 成功后才 publish）⇒ 诊断报告里
        //     **只剩 BeginRun/EndRun**，注入期信息一个字都没有（2026-10-03 现场）。
        WindowModeLogEventf(L"[窗口/后台窗口模式] ⛔ 假焦点注入失败%s: %s",
            lite ? L"（精简/Chromium壳/Qt安卓）" : L"", ffErr.c_str());
        if (androidQt) {
            WindowModeLog(
                L"[窗口/后台窗口模式] MuMu 假焦点失败：仍仅用 PostMessage，不占用前台；"
                L"请确认模拟器为 OpenGL 渲染");
        } else if (glfwSdl) {
            WindowModeLog(
                L"[窗口/后台窗口模式] GLFW/SDL 假焦点失败：后台无法驱动暂停菜单/视角。"
                L"若安全中心拦截了 FakeFocus64.dll，请到「保护历史记录」允许后"
                L"完全退出游戏再试；并确认 exe 旁有该 DLL（静态 CRT）");
        } else if (native3d && !delphiVcl && !lite) {
            hardwareFallback_ = true;
            WindowModeLog(
                L"[窗口/后台窗口模式] 3D 假焦点失败：PostMessage 无法驱动 Raw Input，"
                L"键鼠改走假前台 SendInput。请确认目录含 FakeFocus64.dll");
        } else if (delphiVcl) {
            WindowModeLog(
                L"[窗口/后台窗口模式] 传奇/Delphi 假焦点注入失败：后台仍仅 PostMessage（鼠标会原地点击）。"
                L"请确认目录含 FakeFocus32.dll（32 位客户端）或 FakeFocus64.dll");
        } else if (adobeAir) {
            WindowModeLog(
                L"[窗口/后台窗口模式] Adobe AIR 假焦点失败：后台只剩 PostMessage（造梦等要点不到）。"
                L"请确认目录含 FakeFocus32.dll / FakeFocus64.dll");
        } else if (weixinQt) {
            WindowModeLog(
                L"[窗口/后台窗口模式] 微信 Qt 假焦点失败：后台只剩 PostMessage KEY*（不发 WM_CHAR/激活，"
                L"无焦点欺骗时 Qt 可能丢键）。请确认目录含 FakeFocus64.dll / FakeFocus32.dll，"
                L"并允许安全中心放行");
        } else if (mapleStory) {
            WindowModeLog(
                L"[窗口/后台窗口模式] 冒险岛假焦点注入失败：技能键仍 PostMessage；走路可能仅前台有效。"
                L"请确认目录含 FakeFocus32.dll（32 位客户端）或 FakeFocus64.dll，并允许安全中心放行");
        } else if ((lite && !chromiumShell) || emulator) {
            if (!(UsesBackgroundWindow() && emulator)) {
                hardwareFallback_ = true;
            }
            WindowModeLog(emulator
                ? (UsesBackgroundWindow()
                    ? L"[窗口/后台窗口模式] 模拟器假焦点注入失败：后台模式不回退 SendInput（请检查 FakeFocus64.dll）"
                    : L"[窗口/后台窗口模式] 模拟器假焦点失败，回退假前台 SendInput（后台模式亦会占焦点）")
                : L"[窗口/后台窗口模式] UE5 假焦点失败，回退假前台 SendInput");
        }
        // hardwareFallback：走假前台 SendInput，不要再标 LCA 纯 PostMessage。
        if (!chromiumShell && !weixinQt && !hardwareFallback_) {
            SetLcaBackgroundMessageMode(true);
            WindowModeLog(
                L"[窗口/后台窗口模式] 假焦点失败：回退 LCA 窗口消息（PostMessage KEY*，不发 WM_CHAR/激活）");
        }
        return;
    }
    if (fakeFocus_.time_scale_only()) {
        // 仅变速注入：一个假焦点钩都没装，下面那些「假焦点已注入」的分类日志全是误导。
        // 「仅注入时钟补丁」的说明已在 TryInstallFakeFocus 里打过，这里不重复。
        return;
    }
    if (chromiumShell) {
        FakeFocusSoftInput_SetPostKeyEvents(true);
        WindowModeLog(
            L"[窗口/后台窗口模式] Chromium 壳假焦点：仅窗口进程、焦点欺骗+灌键鼠线程"
            L"（适用 QQ/Discord/VS Code/CEF 等；tech= 见上）");
    } else if (weixinQt) {
        HWND seedHwnd = top && IsWindow(top) ? top : TargetHwnd();
        POINT seed{};
        if (seedHwnd && IsWindow(seedHwnd)) {
            RECT rc{};
            GetClientRect(seedHwnd, &rc);
            seed.x = (std::max)(0, static_cast<int>(rc.right - rc.left)) / 2;
            seed.y = (std::max)(0, static_cast<int>(rc.bottom - rc.top)) / 2;
            ClientToScreen(seedHwnd, &seed);
            FakeFocusSoftInput_SetCursorScreen(seed.x, seed.y);
        } else if (GetCursorPos(&seed)) {
            FakeFocusSoftInput_SetCursorScreen(seed.x, seed.y);
        }
        WindowModeLog(
            L"[窗口/后台窗口模式] 微信 Qt 假焦点已注入（前景查询+软光标/键态；键鼠仍 PostMessage，"
            L"不走 Chromium 灌键/不发 WM_CHAR/激活）");
    } else if (androidQt) {
        FakeFocusSoftInput_SetPostKeyEvents(true);
        WindowModeLog(
            L"[窗口/后台窗口模式] MuMu/Qt 安卓壳假焦点已注入（进程内灌键鼠 PostMessage；不占前台）");
    } else if (lite && desktopEmu) {
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] 桌面模拟器精简假焦点已注入（仅前景+键态；禁 RawInput/WM_INPUT）");
    } else if (lite && delphiVcl) {
        WindowModeLog(
            L"[窗口/后台窗口模式] 传奇/Delphi 精简假焦点已注入（钩 GetCursorPos；后台不占前台）");
    } else if (lite && tianlongDx) {
        HWND seedHwnd = top && IsWindow(top) ? top : TargetHwnd();
        POINT seed{};
        if (seedHwnd && IsWindow(seedHwnd)) {
            RECT rc{};
            GetClientRect(seedHwnd, &rc);
            seed.x = (std::max)(0, static_cast<int>(rc.right - rc.left)) / 2;
            seed.y = (std::max)(0, static_cast<int>(rc.bottom - rc.top)) / 2;
            ClientToScreen(seedHwnd, &seed);
            FakeFocusSoftInput_SetCursorScreen(seed.x, seed.y);
        } else if (GetCursorPos(&seed)) {
            FakeFocusSoftInput_SetCursorScreen(seed.x, seed.y);
        }
        WindowModeLog(
            L"[窗口/后台窗口模式] 天龙八部假焦点已注入（前景查询+软光标/键态；找图点击走 PostMessage，"
            L"不抢前台、不走 LCA 纯消息）");
    } else if (lite && glfwSdl) {
        POINT seed{};
        if (GetCursorPos(&seed)) {
            FakeFocusSoftInput_SetCursorScreen(seed.x, seed.y);
        }
        WindowModeLog(
            L"[窗口/后台窗口模式] GLFW/SDL 精简假焦点已注入（GetCursorPos/RawInput，不钩 PeekMessage；"
            L"暂停菜单与视角由软光标驱动；真光标不夹不跟）");
    } else if (lite && adobeAir) {
        WindowModeLog(
            L"[窗口/后台窗口模式] Adobe AIR 假焦点已注入（只骗前景查询；不钩光标/RawInput/不改 WndProc）");
    } else if (lite && mapleStory) {
        HWND seedHwnd = top && IsWindow(top) ? top : TargetHwnd();
        POINT seed{};
        if (seedHwnd && IsWindow(seedHwnd)) {
            RECT rc{};
            GetClientRect(seedHwnd, &rc);
            seed.x = (std::max)(0, static_cast<int>(rc.right - rc.left)) / 2;
            seed.y = (std::max)(0, static_cast<int>(rc.bottom - rc.top)) / 2;
            ClientToScreen(seedHwnd, &seed);
            FakeFocusSoftInput_SetCursorScreen(seed.x, seed.y);
        } else if (GetCursorPos(&seed)) {
            FakeFocusSoftInput_SetCursorScreen(seed.x, seed.y);
        }
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] 冒险岛假焦点已注入（仅 IAT 前景/键态 + DI 虚表；"
            L"禁止假 WM_INPUT / dinput8 可写节 / 注入线程协作级别 / 运行中远程线程计数）"
            L" —— 若后面**没有**紧跟「假焦点钩命中/钩安装」行，说明目标进程里的 DLL "
            L"没往共享内存写（多为进程内挂着**旧版** DLL，请完全退出游戏再运行）");
        g_mapleSoftKeyLogs = 0;
        g_mapleSoftClickLogs = 0;
        LogMapleHookHits(L"注入后");
    } else if (lite) {
        WindowModeLog(
            L"[窗口/后台窗口模式] UE5 精简假焦点已注入（不钩 PeekMessage；不占用户前台）");
    } else if (emulator) {
        WindowModeLog(
            L"[窗口/后台窗口模式] 桌面模拟器假焦点已注入（GetAsyncKeyState/RawInput；真后台键鼠）");
    } else if (native3d) {
        WindowModeLog(
            L"[窗口/后台窗口模式] 3D/GLFW 假焦点已注入（钩 GetForegroundWindow/GetCursorPos/RawInput；"
            L"暂停菜单与视角不再跟用户真光标）");
        // ⚠ 2026-10-04：这句以前**只报「注入成功」，一个命中数据都不报** ⇒ 用户日志里
        //   根本看不出游戏到底走哪条输入路径，于是「后台模式鼠标不动」只能靠猜。
        //   Unity/UE/GLFW 全部落在这个分支，务必保留这行（判读见 requirements §22）。
        //   ⚠ 文案里的「冒险岛」是**历史名称**（被 skills/LESSONS/历次日志引用，不能改），
        //     实际已是通用假焦点诊断 —— 所以这里补一句前缀消除歧义。
        // ⚠ 必须是 **Event（落盘）** —— `WindowModeLog` 不落盘，用户导出的日志里会只剩
        //   「假焦点钩命中」却没有这句说明，反而更容易被误读成「那是冒险岛专属的」。
        WindowModeLogEvent(
            L"[窗口/后台窗口模式] 下列「假焦点钩命中 / 输入体检 / 钩安装」为**通用假焦点诊断**"
            L"（名称沿用历史；Unity / UE / GLFW / 桌面模拟器同样适用）：用来判定游戏走哪条输入路径。"
            L"⚠ 若后面**没有**紧跟这三行，说明目标进程里的 DLL 没往共享内存写 —— 多为进程内挂着"
            L"**旧版** FakeFocus，请**完全退出游戏进程**再运行");
        LogMapleHookHits(L"注入后");
    }
}

bool WindowModeExecutor::UsesChromiumShellInProcInput() const {
    if (!FakeFocusActive() || !FakeFocusSoftInput_IsAttached()) return false;
    return LooksLikeChromiumShellTarget(session_.Config(), TargetHwnd());
}

bool WindowModeExecutor::UsesMapleStoryFakeFocusInput() const {
    // 技能键必须 PostMessage。旧 true 会跳过 PostMessage，字母/Ctrl 也不动。
    return false;
}

bool WindowModeExecutor::UsesInProcFakeFocusSoftInput() const {
    return UsesChromiumShellInProcInput() || UsesMapleStoryFakeFocusInput();
}

const wchar_t* WindowModeExecutor::RelativeMoveRouteName() const {
    if (UsesCdpInput(session_.Config())) return L"CDP 绝对化（软输入）";
    if (PreferHardwareInput()) return L"SendInput 相对（硬件）";
    if (UsesInProcFakeFocusSoftInput()) return L"进程内软输入（Chromium壳/冒险岛）";
    return L"跨进程软输入（假焦点软光标 + WM_MOUSEMOVE）";
}

bool WindowModeExecutor::PreferHardwareInput() const {
    if (UsesCdpInput(session_.Config())) return false;
    const auto& cfg = session_.Config();
    // Chromium 壳：假焦点注入成功 → 真后台进程内 PostMessage；失败才假前台。
    if (LooksLikeChromiumShellTarget(cfg, TargetHwnd())) {
        return !FakeFocusActive();
    }
    HWND top = TopLevelTargetWindow(TargetHwnd());
    // ★★ 后台窗口模式：**永不**回退到假前台 SendInput（2026-09-30 用户报障
    //    「鼠标拖拽会抢占鼠标使用」）。
    //
    //    机制：原先走到下面那些分支时会 `SendHardwareCursorToClient` ——
    //    它先 `EnsureHardwareInputFocus()`（**把用户当前窗口切走**、把目标拉到前台），
    //    再 `SetCursorPos` 搬走**系统光标**。用户正握着鼠标的手被整只抢走
    //    （拖拽尤其明显：要连续搬 100+ 次）。而且它**根本不会成功**：
    //    后台模式下目标不在前台，`SendInput` 打的是用户当时在看的那只窗
    //    （浏览器/文档），既没驱动游戏、又把用户的鼠标搞乱。
    //
    //    ⇒ 与「模拟器假焦点注入失败 ⇒ 后台模式不回退 SendInput」那条**同一把尺**
    //      （见 `InjectAndInstall` 失败分支）。那次只改了模拟器那一个分支，
    //      普通 3D 游戏照样走进假前台 —— 这里一次收口，`UsesBackgroundWindow()`
    //      下一律不抢鼠标。
    //
    //    ⚠ 代价必须说清（**不是**"都修好了"）：没有假焦点时这些输入确实驱动不了
    //      Raw Input 游戏，只剩 PostMessage，多半点了没反应。所以配一条一次性告警，
    //      而不是偷偷抢用户的鼠标 —— 后者会让人以为"后台模式本来就会抢鼠标"，
    //      从而放弃整个功能。
    if (UsesBackgroundWindow()) {
        if (FakeFocusActive()) return false;
        // ⚠⚠ 2026-10-05：UWP 是**我们主动跳过**注入的（不是「没拿到」）
        //   ⇒ 绝不能报下面那条「未拿到假焦点 / 请放行 DLL」的告警 ——
        //   实测用户看到它就去折腾安全中心放行，而真正该做的是「UWP 走 UIA」。
        if (!uwpSkippedFakeFocus_ && !backgroundNoFakeFocusWarned_) {
            backgroundNoFakeFocusWarned_ = true;
            WindowModeLog(
                L"[窗口/后台窗口模式] ★ 后台模式未拿到假焦点：已按「真后台」处理 —— "
                L"不回退假前台 SendInput（那会抢走你的鼠标/键盘，却仍驱动不了游戏）。"
                L"代价是键鼠只剩 PostMessage，Raw Input 游戏（Unity/UE/GLFW）可能无响应。"
                L"请确认 exe 旁有 FakeFocus64.dll / FakeFocus32.dll 并在安全中心放行；"
                L"或改用「窗口模式」（那种模式允许占键鼠）");
        }
        return false;
    }
    if (hardwareFallback_) return true;
    if (LooksLikeRemoteDesktopWindowClass(cfg.windowClassName)
        || LooksLikeRemoteDesktopWindowClass(cfg.childWindowClassName)
        || LooksLikeRemoteDesktopExePath(cfg.targetExePath)) {
        return true;
    }
    if (top && IsRemoteDesktopWindow(top)) return true;
    if (top && IsWindow(top) && LooksLikeMonitorCoveringFullscreen(top)
        && !PrefersLcaBackgroundMessages(cfg, top)) {
        return true;
    }
    // 窗口化 UE5/Unity：未注入时 PostMessage 无效；勿等铺满才改走 SendInput。
    if (!FakeFocusActive()
        && GameTargetNeedsHardwareWithoutFakeFocus(cfg, top)) {
        return true;
    }
    return false;
}

bool WindowModeExecutor::IsRemoteDesktopPlaybackTarget() const {
    const auto& cfg = session_.Config();
    if (LooksLikeRemoteDesktopExePath(cfg.targetExePath)
        || LooksLikeRemoteDesktopWindowClass(cfg.windowClassName)
        || LooksLikeRemoteDesktopWindowClass(cfg.childWindowClassName)) {
        return true;
    }
    HWND top = TopLevelTargetWindow(TargetHwnd());
    return top && IsRemoteDesktopWindow(top);
}

bool WindowModeExecutor::UsesRelativeHardwareCursor() const {
    // 远程桌面必须绝对坐标。窗口化 UE5（国王大道）也必须绝对 SetCursorPos，
    // 否则点击落在系统光标处；仅真铺满监视器才改相对位移（避免拆 DXGI）。
    if (IsRemoteDesktopPlaybackTarget()) return false;
    if (LooksLikeChromiumShellTarget(session_.Config(), TargetHwnd())) return false;
    HWND top = TopLevelTargetWindow(TargetHwnd());
    return top && IsWindow(top) && LooksLikeMonitorCoveringFullscreen(top);
}

void WindowModeExecutor::RestoreHardwareOffscreenPark() {
    if (!hwOffscreenParked_) return;
    HWND hwnd = hwParkedHwnd_;
    if (hwnd && IsWindow(hwnd)) {
        RestoreHardwareInputTargetOffscreen(hwnd, hwSavedWp_, hwSavedTopmost_);
        WindowModeLog(L"[窗口/后台窗口模式] 本机输入：已把屏外目标还原到原位置");
    }
    hwOffscreenParked_ = false;
    hwParkedHwnd_ = nullptr;
    hwSavedTopmost_ = false;
    hwSavedWp_ = {};
}

bool WindowModeExecutor::EnsureHardwareInputFocus() {
    if (!PreferHardwareInput()) return true;

    HWND top = TopLevelTargetWindow(TargetHwnd());
    if (!top || !IsWindow(top)) return false;

    HWND fg = GetForegroundWindow();
    if (fg == top) return true;
    if (fg && IsChild(top, fg)) return true;

    std::wstring err;
    if (!ActivateWindow(top, err)) {
        WindowModeLogf(L"[窗口/后台窗口模式] 本机输入假前台激活失败: %s", err.c_str());
        return false;
    }
    return true;
}

void WindowModeExecutor::DropFakeFocusForHardwareInput(const wchar_t* reason) {
    // 仅变速注入 / 已经拆过 → fake_focus_active() 为 false，这里天然什么都不做。
    // 这也顺带避免了「每次移动光标都发一次远程调用」。
    if (!fakeFocus_.fake_focus_active()) return;
    const wchar_t* why = (reason && *reason) ? reason : L"本机输入回退";
    if (enableWindowTimeScale_) {
        std::wstring err;
        if (fakeFocus_.DisableFakeFocusKeepTimeScale(err)) {
            WindowModeLogEventf(
                L"[窗口/后台窗口模式] %s：已拆假焦点钩（保留时钟补丁，窗口变速继续生效）", why);
            return;
        }
    }
    fakeFocus_.Unload();
    WindowModeLogEventf(L"[窗口/后台窗口模式] %s：已卸载假焦点（避免 PeekMessage 冻 DXGI）", why);
}

bool WindowModeExecutor::SendHardwareCursorToClient(int cx, int cy) {
    const bool chromiumShell = LooksLikeChromiumShellTarget(session_.Config(), TargetHwnd());
    if (!chromiumShell) DropFakeFocusForHardwareInput(L"全屏游戏");
    EnsureHardwareInputFocus();
    HWND hwnd = TargetHwnd();

    // 脚本约定 (0,0)=当前位置。绝对 SetCursorPos 到客户区原点 = 飞到窗口左上角。
    if (cx == 0 && cy == 0) {
        int lx = 0, ly = 0;
        POINT pt{};
        bool resolved = false;
        if (hwnd && IsWindow(hwnd) && GetCursorPos(&pt)
            && ScreenToClientPoint(hwnd, pt.x, pt.y, lx, ly)
            && (lx != 0 || ly != 0)) {
            cx = lx;
            cy = ly;
            resolved = true;
        } else if (GetLastSoftMouseClientPos(hwnd, lx, ly) && (lx != 0 || ly != 0)) {
            cx = lx;
            cy = ly;
            resolved = true;
        }
        if (!resolved) {
            WindowModeLogEvent(
                L"[窗口/后台窗口模式] 跳过绝对光标到客户区(0,0)（保持系统光标当前位置）");
            return true;
        }
    }

    // 调用约定：cx/cy 已是目标客户区（MoveMouseClient / MapScriptPointToClient 之后）。
    // 旧逻辑用 UsesClientCoords()（看 coordSpace）门控，窗口相对脚本常被标成 screenAbsolute，
    // 客户区小数被当成屏幕坐标 → SetCursorPos 飞到屏幕左上角。
    if (!UsesRelativeHardwareCursor()) {
        int sx = cx;
        int sy = cy;
        if (hwnd && IsWindow(hwnd)) {
            if (!ClientToScreenPoint(hwnd, cx, cy, sx, sy)) {
                WindowModeLogf(L"[窗口/后台窗口模式] ClientToScreen 失败 hwnd=0x%p client=(%d,%d)",
                    hwnd, cx, cy);
                return false;
            }
        }
        RememberSoftMouseClientPos(hwnd, cx, cy);
        WindowModeLogEventf(L"[窗口/后台窗口模式] 本机绝对光标 客户区(%d,%d) → 屏幕(%d,%d)",
            cx, cy, sx, sy);
        return SetCursorScreenPos(sx, sy);
    }

    // UE5 独占全屏：绝对客户区 → 相对 SendInput；不碰 SetCursorPos。
    int prevX = 0;
    int prevY = 0;
    bool havePrev = GetLastSoftMouseClientPos(hwnd, prevX, prevY);
    if (!havePrev) {
        havePrev = GetCursorClientPos(prevX, prevY);
    }
    RememberSoftMouseClientPos(hwnd, cx, cy);
    if (!havePrev) return true;
    const int dx = cx - prevX;
    const int dy = cy - prevY;
    if (dx == 0 && dy == 0) return true;
    SendMouseMoveRelative(dx, dy);
    return true;
}

void WindowModeExecutor::MoveMouseRelativeClient(int dx, int dy) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;
    if (dx == 0 && dy == 0) return;

    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) return;
        int cx = 0, cy = 0;
        if (!GetLastSoftMouseClientPos(TargetHwnd(), cx, cy)) {
            GetCursorClientPos(cx, cy);
        }
        MoveMouseClient(cx + dx, cy + dy, 0, 0, [](int) { return 0; }, false);
        return;
    }
    if (PreferHardwareInput()) {
        if (!PrepareSoftInputFast(err)) return;
        DropFakeFocusForHardwareInput(L"相对鼠标回退");
        EnsureHardwareInputFocus();
        int cx = 0, cy = 0;
        if (GetLastSoftMouseClientPos(TargetHwnd(), cx, cy)) {
            RememberSoftMouseClientPos(TargetHwnd(), cx + dx, cy + dy);
        }
        // 相对移动必须保持相对 SendInput：窗口化 FPS（枪神纪/UE）读 Raw Input，
        // 改成绝对 SetCursorPos 镜头不会转。
        SendMouseMoveRelative(dx, dy);
        return;
    }
    if (!PrepareSoftInputFast(err)) return;
    int cx = 0, cy = 0;
    if (!GetLastSoftMouseClientPos(TargetHwnd(), cx, cy)) {
        GetCursorClientPos(cx, cy);
    }
    cx += dx;
    cy += dy;
    SyncFakeFocusCursor(cx, cy);
    RememberSoftMouseClientPos(TargetHwnd(), cx, cy);
    if (!UsesInProcFakeFocusSoftInput()) {
        HWND top = TopLevelTargetWindow(TargetHwnd());
        // 与绝对移动一致：DeSmuME 纯相对轨迹不洪泛 MOVE；按住拖拽才投递（且底层有节流）。
        const bool desktopEmu = IsDesktopEmulatorTarget(top, &session_.Config());
        if (!desktopEmu || SoftMouseButtonHeld()) {
            PostMouseMoveToWindow(TargetHwnd(), cx, cy);
        }
    }
}

void WindowModeExecutor::SyncFakeFocusCursor(int cx, int cy) const {
    if (!FakeFocusSoftInput_IsAttached()) return;
    HWND hwnd = TargetHwnd();
    int sx = cx;
    int sy = cy;
    if (hwnd && IsWindow(hwnd)) {
        ClientToScreenPoint(hwnd, cx, cy, sx, sy);
    }
    FakeFocusSoftInput_SetCursorScreen(sx, sy);
}

void WindowModeExecutor::SyncFakeFocusMouseButton(MouseButtonType button, bool down) const {
    if (!FakeFocusSoftInput_IsAttached()) return;
    FakeFocusSoftInput_SetMouseButtonVk(VkFromMouseButton(button), down);
}

void WindowModeExecutor::NotifyCancel() {
    ExtBridgeServer::Instance().AbortPending();
    SignalStopCdpMacroDesktopWatchPump();
}

void WindowModeExecutor::EndRun() {
    // 先松开本会话用 SendInput 补过 KEYDOWN 的方向键：脚本中途停止 / 目标闪退 /
    // 用户切走都会让 DOWN/UP 不配对，留下**系统级卡键**（游戏朝一个方向一直走）。
    InvalidateSoftInputFastPath();
    ReleaseMirroredLcaNavKeys();
    RestoreHardwareOffscreenPark();
    hardwareFallback_ = false;
    SetLcaBackgroundMessageMode(false);
    HWND top = TopLevelTargetWindow(session_.State().targetHwnd);
    const bool fsGame = top && IsWindow(top) && LooksLikeFullscreenGameTarget(top);
    const bool rdp = IsRemoteDesktopPlaybackTarget();
    if (!fsGame && !rdp && wasMinimizedAtBeginRun_ && top && IsWindow(top)
        && !UsesCdpInput(session_.Config())) {
        HWND preserveFg = GetForegroundWindow();
        ShowWindow(top, SW_SHOWMINNOACTIVE);
        if (preserveFg && IsWindow(preserveFg) && preserveFg != top
            && GetForegroundWindow() == top) {
            SetForegroundWindow(preserveFg);
        }
        WindowModeLog(L"[窗口/后台窗口模式] 会话结束: 已把目标窗口恢复为最小化");
    }
    wasMinimizedAtBeginRun_ = false;
    loggedClientScale_ = false;
    softInputSkipLogged_ = false;
    // ⚠ 2026-10-04：这里原来还要求 `LooksLikeMapleStoryTarget(...)` ⇒ **非冒险岛目标拿不到
    //   跑完后的累计计数**，而「注入后」那一行是注入瞬间打的、计数必然全 0
    //   ⇒ 等于**没有任何可判读的数据**。去掉目标类型条件（`LogMapleHookHits` 内部对
    //   「共享内存没挂」已有守卫，无条件调用是安全的）。
    if (fakeFocus_.IsInjected()) {
        LogMapleHookHits(L"结束前");
    }
    fakeFocus_.Unload();
    StopCdpMacroDesktopWatchPump();
    top = TopLevelTargetWindow(session_.State().targetHwnd);
    if (!fsGame && !rdp && top && IsWindow(top) && !UsesBackgroundWindow()) {
        if (UsesCdpInput(session_.Config())) {
            RestoreMacroDesktopWindowAfterRun(top);
        } else if (session_.Config().executionKind
                == WindowModeExecutionKind::HiddenDesktop) {
            // softMessage 后台窗口模式：把目标窗口从「鼠标宏」桌面移回用户当前桌面，
            // 避免回放结束后窗口留在宏桌面不可见（“窗口消失/没反应”）。
            auto& vda = VirtualDesktopAccessor::Instance();
            std::wstring err;
            if (vda.EnsureLoaded(err)) {
                const int userDesk = vda.GetCurrentDesktopNumber();
                const int macroIdx = vda.FindDesktopIndexByName(kMacroDesktopDisplayName);
                if (userDesk >= 0 && userDesk != macroIdx
                    && !vda.IsWindowOnDesktopNumber(top, userDesk)) {
                    vda.MoveWindowToDesktopNumberPreservingView(top, userDesk);
                    WindowModeLogEvent(L"[窗口/后台窗口模式] 会话结束: 目标已移回用户桌面");
                }
            }
        }
    }
    if (rdpSavedForeground_ && IsWindow(rdpSavedForeground_)) {
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow(rdpSavedForeground_);
        WindowModeLog(L"[窗口/后台窗口模式] 本机输入：已还原回放前的前台窗口");
    }
    rdpSavedForeground_ = nullptr;
    ExtBridgeServer::Instance().ClearAbort();
    cdp_.Disconnect();
    ext_.Disconnect();
    session_.Stop();
    active_ = false;
    cancelFlag_ = nullptr;
    extLayoutFresh_ = false;
    ResetSoftMouseState();
    WindowModeLogEvent(L"[窗口/后台窗口模式] EndRun：窗口模式会话结束（「鼠标宏」桌面保留，不会自动删除）");
}

void WindowModeExecutor::UpdateExtSurfaceSize() {
    if (!ext_.IsConnected()) return;
    if (IsCdpInputMode()) {
        const auto& st = session_.State();
        int sw = 0, sh = 0;
        if (ResolveExtVisionSurface(ext_, TargetHwnd(), st.clientW, st.clientH, sw, sh)) {
            static int sLastSw = 0, sLastSh = 0;
            if (sw != sLastSw || sh != sLastSh) {
                WindowModeLogf(L"[窗口/后台窗口模式] 扩展鼠标表面 %dx%d", sw, sh);
                sLastSw = sw;
                sLastSh = sh;
            }
        }
        if (sw >= 64 && sh >= 64) {
            ext_.SetSurfaceSize(sw, sh);
        }
        return;
    }
    HWND cap = VisionCaptureHwnd();
    if (!cap || !IsWindow(cap)) cap = TargetHwnd();
    RECT rc{};
    if (!cap || !GetClientRect(cap, &rc)) return;
    const int w = std::max(0, static_cast<int>(rc.right - rc.left));
    const int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
    if (w > 0 && h > 0) ext_.SetSurfaceSize(w, h);
}

void WindowModeExecutor::ToCaptureSurfaceClient(int& cx, int& cy) const {
    if (IsCdpInputMode()) return;
    HWND input = TargetHwnd();
    HWND cap = VisionCaptureHwnd();
    if (!input || !cap || !IsWindow(input) || !IsWindow(cap) || input == cap) return;
    const int inX = cx;
    const int inY = cy;

    RECT inRc{}, capRc{};
    GetClientRect(input, &inRc);
    GetClientRect(cap, &capRc);
    int inW = std::max(0, static_cast<int>(inRc.right - inRc.left));
    int inH = std::max(0, static_cast<int>(inRc.bottom - inRc.top));
    int capW = std::max(0, static_cast<int>(capRc.right - capRc.left));
    int capH = std::max(0, static_cast<int>(capRc.bottom - capRc.top));
    // 还原后偶发仍读到 215×28 缩略客户区，按会话纠正尺寸缩放，避免键鼠打偏。
    if (inW < 400 || inH < 300) {
        int rw = session_.State().clientW;
        int rh = session_.State().clientH;
        if (rw < 400 || rh < 300) {
            FillClientSizeFromHwnd(TopLevelTargetWindow(input), rw, rh);
        }
        if (rw >= 400 && rh >= 300 && inW > 0 && inH > 0) {
            cx = static_cast<int>(std::lround(cx * (static_cast<double>(rw) / inW)));
            cy = static_cast<int>(std::lround(cy * (static_cast<double>(rh) / inH)));
            inW = rw;
            inH = rh;
        }
    }
    if (capW < 400 || capH < 300) {
        int rw = 0, rh = 0;
        FillClientSizeFromHwnd(TopLevelTargetWindow(cap), rw, rh);
        if (rw >= 400 && rh >= 300) {
            capW = rw;
            capH = rh;
        }
    }

    MapMatchPointBetweenWindows(input, cap, cx, cy);
    if (cx != inX || cy != inY) {
        WindowModeLogf(
            L"[窗口/后台窗口模式] 网页坐标对齐 input=(%d,%d)@%dx%d -> surface=(%d,%d)@%dx%d",
            inX, inY, inW, inH, cx, cy, capW, capH);
    }
}

void WindowModeExecutor::MaybeRefreshExtLayout() {
    if (extLayoutFresh_ || !UsesCdpInput(session_.Config()) || !ext_.IsConnected()) return;
    if (WindowModeCancelled(cancelFlag_)) return;
    // 最小化也可向扩展要壳页 iframe 布局；surface 禁用 215×28 缩略尺寸。
    const auto& st = session_.State();
    int sw = 0, sh = 0;
    ResolveExtVisionSurface(ext_, TargetHwnd(), st.clientW, st.clientH, sw, sh);
    if (sw >= 64 && sh >= 64) {
        ext_.SetSurfaceSize(sw, sh);
    }

    // attach 已带 iframe 矩形时禁止再打 layout：旧扩展 soft-swap 会弄死 MV3 桥。
    if (ext_.HasValidIframeLayout()) {
        extLayoutFresh_ = true;
        int ix = 0, iy = 0, iw = 0, ih = 0, pw = 0, ph = 0;
        ext_.GetIframeCssRect(ix, iy, iw, ih);
        ext_.GetPageCssSize(pw, ph);
        WindowModeLogf(
            L"[窗口/后台窗口模式] 沿用 attach 布局 iframeCss=(%d,%d) %dx%d pageCss=%dx%d surface=%dx%d（跳过 layout 防断桥）",
            ix, iy, iw, ih, pw, ph, ext_.SurfaceW(), ext_.SurfaceH());
        return;
    }

    std::wstring err;
    if (ext_.RefreshLayout(err)) {
        extLayoutFresh_ = true;
        WindowModeLog(L"[窗口/后台窗口模式] 已刷新扩展坐标布局（未展开窗口）");
    } else if (!err.empty()) {
        WindowModeLogVerbosef(L"[窗口/后台窗口模式] 扩展布局刷新跳过: %s", err.c_str());
    }
}

HWND WindowModeExecutor::TargetHwnd() const {
    return session_.State().targetHwnd;
}

bool WindowModeExecutor::TargetStillAlive() const {
    if (!active_) return false;
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) return false;
    const DWORD storedTopPid = session_.State().targetPid;
    const DWORD storedBindPid = session_.State().bindPid;
    DWORD liveBindPid = 0;
    GetWindowThreadProcessId(hwnd, &liveBindPid);
    HWND top = TopLevelTargetWindow(hwnd);
    DWORD liveTopPid = 0;
    if (top && IsWindow(top)) {
        GetWindowThreadProcessId(top, &liveTopPid);
    }
    if (!TargetBindPidStillMatches(storedBindPid, liveBindPid, storedTopPid, liveTopPid)) {
        return false;
    }
    const DWORD checkPid = storedBindPid != 0 ? storedBindPid : storedTopPid;
    if (checkPid == 0) return true;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, checkPid);
    if (!process) {
        // 窗口还在但打不开进程：多半权限问题，不按闪退处理。
        return true;
    }
    DWORD exitCode = STILL_ACTIVE;
    const BOOL ok = GetExitCodeProcess(process, &exitCode);
    CloseHandle(process);
    // 查询本身失败（句柄/权限的瞬时问题）**不等于**进程已退出。
    // 误判的代价是「脚本被无故停掉」，用户看到的就是「跑着跑着突然停了/像闪退」；
    // 漏判一次只是多跑一拍，下一拍还会再查。所以查询失败一律按「还活着」处理。
    if (!ok) return true;
    return exitCode == STILL_ACTIVE;
}

std::wstring WindowModeExecutor::TargetAliveDebug() const {
    HWND hwnd = TargetHwnd();
    DWORD liveBindPid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &liveBindPid);
    HWND top = TopLevelTargetWindow(hwnd);
    DWORD liveTopPid = 0;
    if (top) GetWindowThreadProcessId(top, &liveTopPid);
    // 把「进程查询结果」也打出来 —— 判死时最需要区分的是：
    //   真的退出了（proc=2 且带退出码） vs 查询失败被误判（proc=0/-1）。
    const DWORD checkPid = session_.State().bindPid != 0
        ? session_.State().bindPid : session_.State().targetPid;
    DWORD exitCode = 0;
    int procState = -1;
    if (checkPid != 0) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, checkPid);
        if (h) {
            procState = GetExitCodeProcess(h, &exitCode)
                ? (exitCode == STILL_ACTIVE ? 1 : 2) : 0;
            CloseHandle(h);
        }
    }
    wchar_t buf[420]{};
    swprintf_s(buf,
        L"hwnd=0x%p isWindow=%d liveBindPid=%lu storedBindPid=%lu top=0x%p liveTopPid=%lu "
        L"storedTopPid=%lu pid=%lu proc=%d exit=0x%08X",
        hwnd, (hwnd && IsWindow(hwnd)) ? 1 : 0,
        static_cast<unsigned long>(liveBindPid),
        static_cast<unsigned long>(session_.State().bindPid),
        top,
        static_cast<unsigned long>(liveTopPid),
        static_cast<unsigned long>(session_.State().targetPid),
        static_cast<unsigned long>(checkPid),
        procState,
        static_cast<unsigned>(exitCode));
    return buf;
}

WindowModeHealth WindowModeExecutor::Health() const {
    return session_.State().health;
}

bool WindowModeExecutor::RefreshTarget(std::wstring& err) {
    if (!active_) return false;
    // 目标可能重开成新 HWND：快速路径缓存必须作废，否则会拿旧窗口的几何继续跑。
    InvalidateSoftInputFastPath();
    if (!session_.RefreshTarget(err)) return false;
    const auto health = session_.State().health;
    if (health != WindowModeHealth::Ok) {
        err = session_.State().lastError.empty()
            ? HealthToUserHint(health) : session_.State().lastError;
        return false;
    }
    return true;
}

bool WindowModeExecutor::MoveToScreen(int sx, int sy) {
    if (PreferHardwareInput()) {
        int cx = sx;
        int cy = sy;
        HWND hwnd = TargetHwnd();
        if (hwnd && IsWindow(hwnd) && UsesClientCoords()) {
            if (!ScreenToClientPoint(hwnd, sx, sy, cx, cy)) return false;
        }
        return SendHardwareCursorToClient(cx, cy);
    }
    return SetCursorPos(sx, sy) == TRUE;
}

bool WindowModeExecutor::PrepareSoftInput(std::wstring& err) {
    // Message-based input must NOT expand / raise the target.
    // Vision capture (find-image/OCR) uses ScopedVisionCapturePrep for quiet bottom restore.
    // 浏览器子控件可能晚于顶层出现：每次输入前刷新绑定到 RenderWidget。
    if (!session_.RefreshInputBinding(err)) {
        HWND hwnd = TargetHwnd();
        if (!hwnd || !IsWindow(hwnd)) {
            return RefreshTarget(err);
        }
        err.clear();
    }
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        return RefreshTarget(err);
    }
    if (!EnsurePlaybackGeometry(err)) return false;
    if (!EnsureTargetBound(err)) return false;
    // 本机 SendInput：每次键鼠前确认仍在前台，否则会打到本软件并响提示音。
    if (PreferHardwareInput()) {
        EnsureHardwareInputFocus();
    }
    return true;
}

bool WindowModeExecutor::PrepareSoftInputFast(std::wstring& err) {
    const bool ok = PrepareSoftInputFastImpl(err);
    if (ok || softInputSkipLogged_) return ok;
    // ⚠ 2026-10-04：失败原因原来**没有任何落盘记录** —— 5 个调用点里 4 个直接 `return`
    //   （一行日志都没有），剩下那个写的是 `WindowModeLogf`（**非 Event ⇒ 不落盘**）。
    //   于是「键鼠动作被静默丢弃」在用户导出的日志里表现为「移动/点击 → 客户区」那行
    //   **没出现**，却查不出原因（这正是本次「后台窗口模式鼠标不移动到指定位置」的
    //   候选原因之一）。⇒ 收口到一处：Event 落盘 + 一次性限流（每拍一次的热路径，
    //   不能每拍刷屏）。
    softInputSkipLogged_ = true;
    WindowModeLogEventf(
        L"[窗口/后台窗口模式] ⚠ 软输入未就绪 —— 本拍键鼠动作被跳过：%s"
        L"（本会话后续同类跳过不再重复报；若全程只有这一条，说明绑定在运行中失效了）",
        err.empty() ? L"PrepareSoftInputFast 失败（原因未填）" : err.c_str());
    return false;
}

bool WindowModeExecutor::PrepareSoftInputFastImpl(std::wstring& err) {
    // 相对移动/键盘/点击都是**每拍一次**的最热路径（一段回放可上万拍）。完整路径里的
    // `RefreshInputBinding` 会 `EnumChildWindows` 全树找 RenderWidget ——
    // 对 GLFW/UE 这类绑定恒为顶层的目标纯属白跑，8ms 一拍喂不起。
    // 判据见 `CanUseSoftInputFastPathClass`（纯函数，已自检 `soft_input_fast_path`）。
    HWND bound = TargetHwnd();
    HWND top = (bound && IsWindow(bound)) ? TopLevelTargetWindow(bound) : nullptr;
    const bool topAlive = top && IsWindow(top);
    RECT rc{};
    const bool haveRect = topAlive && GetClientRect(bound, &rc) != FALSE;
    const int liveW = haveRect ? static_cast<int>(rc.right - rc.left) : 0;
    const int liveH = haveRect ? static_cast<int>(rc.bottom - rc.top) : 0;
    const bool haveCached = fastPreparedHwnd_ == bound && fastPreparedHwnd_ != nullptr;

    // 类名一起比：HWND 会被系统复用，旧缓存可能指向「同值不同窗」。
    bool classMatches = false;
    std::wstring topCls;
    if (topAlive) {
        wchar_t cls[256]{};
        GetClassNameW(top, cls, 256);
        topCls.assign(cls);
        classMatches = haveCached && topCls == fastPreparedTopClass_;
    }

    if (!CanUseSoftInputFastPathClass(topAlive, bound == top, haveCached, classMatches,
            fastPreparedClientW_, fastPreparedClientH_, liveW, liveH)) {
        if (!PrepareSoftInput(err)) return false;
        // 完整路径可能改过绑定：缓存必须按**刷新后**的实际目标重记，
        // 否则下一次进来还拿旧 hwnd 去比，永远命不中（退化成每拍全树枚举）。
        HWND fresh = TargetHwnd();
        HWND freshTop = (fresh && IsWindow(fresh)) ? TopLevelTargetWindow(fresh) : nullptr;
        wchar_t freshCls[256]{};
        if (freshTop && IsWindow(freshTop)) GetClassNameW(freshTop, freshCls, 256);
        RECT freshRc{};
        const bool freshRect = fresh && IsWindow(fresh) && GetClientRect(fresh, &freshRc) != FALSE;
        fastPreparedHwnd_ = fresh;
        fastPreparedClientW_ = freshRect ? static_cast<int>(freshRc.right - freshRc.left) : 0;
        fastPreparedClientH_ = freshRect ? static_cast<int>(freshRc.bottom - freshRc.top) : 0;
        fastPreparedTopClass_.assign(freshCls);
        return true;
    }
    // 绑定与几何都没变：仍要做前台确认（本机 SendInput 必须落在目标前台）。
    if (PreferHardwareInput()) {
        EnsureHardwareInputFocus();
    }
    return true;
}

bool WindowModeExecutor::EnsureCdpReady(std::wstring& err) {
    if (WindowModeCancelled(cancelFlag_)) {
        err = L"已取消";
        return false;
    }
    if (cdp_.IsConnected() || ext_.IsConnected()) return true;

    const auto& cfg = session_.Config();
    const std::wstring hint = !cfg.windowName.empty() ? cfg.windowName : cfg.targetWindowTitle;
    HWND top = TopLevelTargetWindow(TargetHwnd());

    // 主路径：配套扩展。勿先扫 9222~9230——WinHttp 连不上时会卡数十秒，热键 Abort 也打不进。
    WindowModeLog(L"[窗口/后台窗口模式] 网页兼容：经配套扩展桥投递键鼠（不重启）");
    std::wstring attachHint = hint;
    if (attachHint.empty() && top && IsWindow(top)) {
        wchar_t title[512]{};
        GetWindowTextW(top, title, 512);
        attachHint = title;
    }

    std::wstring extErr;
    if (ext_.EnsureReady(attachHint, top, extErr)) {
        err.clear();
        return true;
    }
    if (WindowModeCancelled(cancelFlag_) || extErr == L"已取消"
        || ExtBridgeServer::Instance().IsAborted()) {
        err = L"已取消";
        return false;
    }

    // 回退：用户已自行开启 remote-debugging-port 时再试直连（短超时）。
    if (top && IsWindow(top) && cdp_.ConnectForWindow(top, cfg.cdpPort, hint, err)) {
        return true;
    }
    if (WindowModeCancelled(cancelFlag_)) {
        err = L"已取消";
        return false;
    }

    OpenExtensionInstallGuide();
    err = L"未能连接配套浏览器扩展。请加载 extension\\edge 后重试。"
          L"（若已手动开启 --remote-debugging-port 也可直接使用）";
    if (!extErr.empty() && extErr != L"NO_EXTENSION") {
        err += L"\n";
        err += extErr;
    }
    WindowModeLogf(L"[窗口/后台窗口模式] %s", err.c_str());
    return false;
}


bool WindowModeExecutor::RecordedClientSize(int& w, int& h) const {
    w = session_.Config().recordClientWidth;
    h = session_.Config().recordClientHeight;
    if (w > 0 && h > 0) return true;
    // ⚠ 回退到 `coordMeta_.capture`（**屏幕分辨率**）—— 这只对**找图模板缩放**成立
    //   （`FindImageSurfaceScale`：全屏场景下屏幕分辨率 ≈ 客户区尺寸）。
    // ⚠⚠ **坐标缩放绝不能走这个回退**（2026-10-05 用户实测）：
    //   窗口化目标（UWP 计算器 480x799）下，动作坐标**已经是客户区像素**，
    //   拿屏幕分辨率当「录制客户区」会把它们再缩一次 ⇒ 点错位置
    //   （实测 (181,599) → (34,332)，表现为「移动了但点击没反应」）。
    //   ⇒ `MapScriptPointToClient` **不走本函数**，直接读 `recordClientWidth/Height`。
    if (session_.Config().windowRelativeCoordinates
        && coordMeta_.captureWidth > 0 && coordMeta_.captureHeight > 0) {
        w = coordMeta_.captureWidth;
        h = coordMeta_.captureHeight;
        return true;
    }
    w = 0;
    h = 0;
    return false;
}

bool WindowModeExecutor::LiveClientSize(int& w, int& h) const {
    w = 0;
    h = 0;
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) hwnd = VisionCaptureHwnd();
    HWND top = TopLevelTargetWindow(hwnd);
    // MuMu：缩放按渲染子窗（游戏区），勿用含顶栏的顶层客户区。
    if (top && hwnd == top && IsAndroidEmulatorTarget(top, &session_.Config())) {
        if (HWND render = FindBackgroundInputChild(top, &session_.Config())) {
            if (render != top) hwnd = render;
        }
    }
    RECT rc{};
    if (hwnd && IsWindow(hwnd) && GetClientRect(hwnd, &rc)) {
        w = std::max(0, static_cast<int>(rc.right - rc.left));
        h = std::max(0, static_cast<int>(rc.bottom - rc.top));
    }
    if (w <= 0 || h <= 0) {
        w = session_.State().clientW;
        h = session_.State().clientH;
    }
    return w > 0 && h > 0;
}

TemplateScale WindowModeExecutor::FindImageSurfaceScale(int surfaceW, int surfaceH) const {
    TemplateScale ts{};
    if (session_.Config().windowRelativeCoordinates) {
        int recW = 0, recH = 0;
        if (RecordedClientSize(recW, recH) && recW > 0 && recH > 0
            && surfaceW > 0 && surfaceH > 0) {
            CoordMeta clientMeta = coordMeta_;
            clientMeta.captureWidth = recW;
            clientMeta.captureHeight = recH;
            return ComputeTemplateScale(clientMeta, surfaceW, surfaceH);
        }
        return ts;
    }
    if (coordMeta_.captureWidth > 0 || coordMeta_.refWidth > 0) {
        int curX = 0, curY = 0, curW = 0, curH = 0;
        GetVirtualScreenBounds(curX, curY, curW, curH);
        return ComputeTemplateScale(coordMeta_, curW, curH);
    }
    return ts;
}

TemplateScale WindowModeExecutor::FindImageTemplateScale() const {
    int liveW = 0, liveH = 0;
    if (!LiveClientSize(liveW, liveH)) return {};
    return FindImageSurfaceScale(liveW, liveH);
}

bool WindowModeExecutor::EnsurePlaybackGeometry(std::wstring& err) {
    // 宏桌面 CDP：窗口停放在宏桌面，找图走扩展截图，禁止 Win32 展开抢前台。
    // 后台窗口模式（含浏览器扩展找图）最小化时仍置底还原，让页面/客户区能出图。
    if (IsCdpInputMode() && !UsesBackgroundWindow()) {
        err.clear();
        return true;
    }
    if (PreferHardwareInput()) {
        HWND root = TopLevelTargetWindow(TargetHwnd());
        if (root && IsWindow(root) && IsIconic(root)) {
            // 远程桌面：最小化时本机 SendInput 进不了会话；还原+假前台是唯一可行路径。
            if (IsRemoteDesktopWindow(root)) {
                WindowModeLog(L"[窗口/后台窗口模式] 远程桌面已最小化：还原并假前台（无法在最小化态操控远程会话）");
                ShowWindow(root, SW_RESTORE);
                EnsureHardwareInputFocus();
                if (IsIconic(root)) {
                    err = L"远程桌面仍处于最小化，无法回放（请先还原 mstsc 窗口）";
                    WindowModeLogf(L"[窗口/后台窗口模式] %s", err.c_str());
                    return false;
                }
                err.clear();
                return true;
            }
            err = L"全屏游戏处于最小化，无法回放";
            return false;
        }
        err.clear();
        return true;
    }
    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        if (!RefreshTarget(err)) return false;
        hwnd = TargetHwnd();
    }
    HWND root = TopLevelTargetWindow(hwnd);
    if (!root || !IsWindow(root)) {
        return RefreshTarget(err);
    }

    // Chromium/Electron/CEF：最小化或隐藏后 GPU 合成停摆。
    // Win32 安静 ShowWindow/置底还原只会唤出空白壳（bestNcc≈0，已证伪），禁止再走。
    // 真浏览器走扩展截图；QQ 等 Chromium 壳须保持「已还原且可被遮挡」，勿最小化。
    if (UsesChromiumShellInProcInput()
        || LooksLikeChromiumShellTarget(session_.Config(), root)) {
        if (TargetNeedsQuietPlaybackRestore(root)
            || TargetNeedsQuietPlaybackRestore(hwnd)) {
            WindowModeLog(
                L"[窗口/后台窗口模式] Chromium 壳已最小化/隐藏：禁止 Win32 安静还原（只会得到空白窗）");
            WindowModeLog(
                L"[窗口/后台窗口模式] 请先还原目标窗口，再保持后台（可被其它窗完全挡住），勿点最小化");
            err = L"Chromium 壳最小化后无法截图与可靠输入，请还原窗口（可被遮挡）";
            return false;
        }
        err.clear();
        return true;
    }

    if (!TargetNeedsQuietPlaybackRestore(root)
        && !TargetNeedsQuietPlaybackRestore(hwnd)) {
        err.clear();
        return true;
    }
    HWND preserveFg = GetForegroundWindow();
    const bool iconic = IsIconic(root) != FALSE;
    const bool hidden = root && IsWindow(root) && !IsWindowVisible(root);
    WindowModeLogf(
        L"[窗口/后台窗口模式] 目标需安静还原以便回放（不抢前台） iconic=%d visible=%d",
        iconic ? 1 : 0, hidden ? 0 : 1);
    if (!RestoreOnUserDesktopBottom(root, preserveFg)) {
        RestoreWindowNonActivating(root);
        if (root && IsWindow(root) && !IsWindowVisible(root)) {
            ShowWindow(root, SW_SHOWNOACTIVATE);
            EnsureTargetBelowUserWindows(root);
        }
    }
    if (TargetNeedsQuietPlaybackRestore(root)) {
        err = L"目标窗口仍处于最小化/隐藏状态，无法执行";
        WindowModeLogf(L"[窗口/后台窗口模式] %s iconic=%d visible=%d",
            err.c_str(),
            IsIconic(root) ? 1 : 0,
            (root && IsWindowVisible(root)) ? 1 : 0);
        return false;
    }
    std::wstring bindErr;
    session_.RefreshInputBinding(bindErr);
    err.clear();
    return true;
}

void WindowModeExecutor::MoveMouseClient(int cx, int cy, int randomX, int randomY,
    const std::function<int(int)>& randomInt, bool scaleRecordedClient) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;
    if (!UsesCdpInput(session_.Config())) {
        std::wstring prepErr;
        if (!PrepareSoftInputFast(prepErr)) {
            // ⚠ 这里是**拖拽路径上的静默丢步**（2026-09-30）：`PrepareSoftInputFast` 失败
            //   （绑定失效/几何变化/刷不出目标）时原来只写一句 DebugLog 就 return ——
            //   于是一次拖拽会**中间断掉**：按下去了、移动全丢、最后再抬起，
            //   目标那边看到的是"点了两下"，而日志里连一行正式的都没有。
            //   ⚠ 2026-10-04：失败原因现在由 `PrepareSoftInputFast` **自己**统一报
            //   （Event 落盘 + 一次性限流，见那里）—— 不必每个调用点各写一份。
            return;
        }
    }
    if (!MapScriptPointToClient(cx, cy, scaleRecordedClient)) return;
    // 与点击一致：(0,0)=当前位置；硬件绝对光标路径必须先解析，否则飞到左上角。
    ResolveClickClientPos(cx, cy);
    if (scaleRecordedClient && session_.Config().windowRelativeCoordinates
        && (randomX != 0 || randomY != 0)) {
        // ⚠ 同 `MapScriptPointToClient`：**直接读**录制尺寸，不走 `RecordedClientSize()`
        //   —— 它的回退是屏幕分辨率，用在这里会把随机抖动白缩一次。
        const int recW = session_.Config().recordClientWidth;
        const int recH = session_.Config().recordClientHeight;
        int liveW = 0, liveH = 0;
        if (recW > 0 && recH > 0 && LiveClientSize(liveW, liveH)) {
            ScaleWindowClientPoint(recW, recH, liveW, liveH, randomX, randomY);
        }
    }
    WindowModeLogf(L"[窗口/后台窗口模式] 移动 → 客户区(%d,%d) ±(%d,%d)", cx, cy, randomX, randomY);

    const int rx = randomInt(randomX);
    const int ry = randomInt(randomY);
    const int tx = cx + rx;
    const int ty = cy + ry;

    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) {
            WindowModeLogf(L"[窗口/后台窗口模式] 网页鼠标移动被跳过（CDP/扩展桥未就绪）：%s",
                err.empty() ? L"EnsureCdpReady 失败" : err.c_str());
            return;
        }
        if (UserOnMacroDesktopNow()) {
            RaiseMacroDesktopWindowForWatch(TargetHwnd());
        }
        MaybeRefreshExtLayout();
        UpdateExtSurfaceSize();
        RememberSoftMouseClientPos(TargetHwnd(), tx, ty);
        int sx = tx;
        int sy = ty;
        ToCaptureSurfaceClient(sx, sy);
        const bool ok = ext_.IsConnected() ? ext_.MouseMove(sx, sy, err)
                                           : cdp_.MouseMove(sx, sy, err);
        if (!ok) {
            WindowModeLogf(L"[窗口/后台窗口模式] 网页键鼠移动失败: %s", err.c_str());
        }
        return;
    }
    if (PreferHardwareInput()) {
        SendHardwareCursorToClient(tx, ty);
        return;
    }
    SyncFakeFocusCursor(tx, ty);
    RememberSoftMouseClientPos(TargetHwnd(), tx, ty);
    if (!UsesInProcFakeFocusSoftInput()) {
        HWND top = TopLevelTargetWindow(TargetHwnd());
        // DeSmuME 纯移动会触发 ShowCursor 洪泛崩溃；按住键拖拽时仍须发 MOVE。
        const bool desktopEmu = IsDesktopEmulatorTarget(top, &session_.Config());
        if (!desktopEmu || SoftMouseButtonHeld()) {
            PostMouseMoveToWindow(TargetHwnd(), tx, ty);
        }
    }
}

void WindowModeExecutor::ResolveClickClientPos(int& cx, int& cy) const {
    // 脚本里鼠标点击 x=y=0 表示「当前位置」。窗口/网页键鼠不移动系统光标，
    // 必须落到上次软光标，否则会点到客户区 (0,0)。
    // 软光标若已是 (0,0) 视为未初始化（曾被错误 Remember），改用系统光标。
    if (cx != 0 || cy != 0) return;
    int lx = 0, ly = 0;
    if (GetLastSoftMouseClientPos(TargetHwnd(), lx, ly) && (lx != 0 || ly != 0)) {
        cx = lx;
        cy = ly;
        return;
    }
    if (GetCursorClientPos(lx, ly)) {
        cx = lx;
        cy = ly;
    }
}

bool WindowModeExecutor::MapScriptPointToClient(int& cx, int& cy, bool scaleRecordedClient) const {
    if (!active_) return false;
    // 窗口相对录制脚本：坐标已是目标窗口客户区。
    // （coordSpace 历史值不可靠——旧脚本被误标 windowClient 但存的是屏幕坐标。）
    if (session_.Config().windowRelativeCoordinates) {
        if (cx == 0 && cy == 0) return true;
        if (scaleRecordedClient) {
            // ⚠⚠ 这里**必须直接读** `recordClientWidth/Height`，**不能**走
            //   `RecordedClientSize()` —— 后者取不到时会回退到 `coordMeta_.capture`
            //   （**屏幕分辨率**），而本函数的入参**已经是客户区像素**
            //   ⇒ 再按「屏幕 → 当前窗口」缩一次就是**白缩**。
            //   2026-10-05 用户实测：UWP 计算器客户区 480x799，动作 (181,599)
            //   被缩成 (34,332)，表现为「移动了但点击没反应」。
            //   ⇒ 取不到录制尺寸就**不缩放**（比例 1）：不缩放至少不会主动引入错误。
            //   正确来源：**取点时**写进 `windowMode.recordClientWidth/Height`（见 ui/app.js）。
            const int recW = session_.Config().recordClientWidth;
            const int recH = session_.Config().recordClientHeight;
            int liveW = 0, liveH = 0;
            if (recW > 0 && recH > 0 && LiveClientSize(liveW, liveH)) {
                const int inX = cx;
                const int inY = cy;
                if (ScaleWindowClientPoint(recW, recH, liveW, liveH, cx, cy)
                    && !loggedClientScale_) {
                    WindowModeLogEventf(
                        L"[窗口/后台窗口模式] 客户区缩放 录制%dx%d → 当前%dx%d (%d,%d)→(%d,%d)",
                        recW, recH, liveW, liveH, inX, inY, cx, cy);
                    loggedClientScale_ = true;
                }
            }
        }
        return true;
    }
    // (0,0) = 「当前位置」（点击沿用上次软光标），保持原语义。
    if (cx == 0 && cy == 0) return true;

    HWND hwnd = TargetHwnd();
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;

    // scaleRecordedClient=false：找图/OCR 等运行时落点，已是客户区坐标。
    // 不可再当屏幕坐标做 ScreenToClient，否则会误判「客户区外」并取消点击。
    if (!scaleRecordedClient) {
        if (cx < 0 || cy < 0 || cx >= client.right || cy >= client.bottom) {
            WindowModeLogEventf(L"[窗口/后台窗口模式] 目标点客户区(%d,%d) 超出目标窗口(%dx%d)，已取消",
                cx, cy, static_cast<int>(client.right), static_cast<int>(client.bottom));
            return false;
        }
        return true;
    }

    const int screenX = cx;
    const int screenY = cy;
    if (!ScreenToClientPoint(hwnd, screenX, screenY, cx, cy)) return false;
    if (cx < 0 || cy < 0 || cx >= client.right || cy >= client.bottom) {
        WindowModeLogEventf(L"[窗口/后台窗口模式] 目标点屏幕(%d,%d) 在目标窗口客户区外(%dx%d)，已取消",
            screenX, screenY, static_cast<int>(client.right), static_cast<int>(client.bottom));
        return false;
    }
    return true;
}

void WindowModeExecutor::PostMouseButtonAtClient(int cx, int cy, MouseButtonType button, bool down,
    bool scaleRecordedClient) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;
    if (!UsesCdpInput(session_.Config())) {
        // 每拍一次的热路径：与相对移动共用快速路径（见 PrepareSoftInputFast 注释）。
        std::wstring prepErr;
        if (!PrepareSoftInputFast(prepErr)) return;
    }
    if (!MapScriptPointToClient(cx, cy, scaleRecordedClient)) return;
    // UWP/WinUI：PostMessage 不生效，走 UIA Invoke 兜底（与是否窗口相对录制无关；
    // WindowUsesUiaClickFallback 已排除 Win32/Unity，避免误抢前台）。
    // Down 成功调用 Invoke 后，配对 Up 直接跳过，避免重复触发。
    // ⚠⚠ 2026-10-05：**必须在 UIA 分支之前**解析坐标 —— 动作坐标 `(0,0)` 是
    //   「用**当前鼠标位置**」的特殊值，由 `ResolveClickClientPos` 换成真实客户区坐标。
    //   原来 UIA 分支排在它**之前** ⇒ 拿到 `(0,0)` ⇒ `ClientToScreen` 得到
    //   **客户区原点** ⇒ UIA 跑到窗口左上角找元素 ⇒ 「该点下没有可 Invoke 的元素」。
    //   用户实测（UWP 计算器）：动作 `@0,0`，UIA 查询点 = 客户区原点 `(1678,428)`，
    //   而正确值应是 `(1858,1023)`。
    //   ⚠ 这条对**所有**走 UIA 的目标都成立（不只 UWP）：动作写 `(0,0)` 时，
    //     非 UIA 路径会被 `ResolveClickClientPos` 修正，UIA 路径却漏了。
    ResolveClickClientPos(cx, cy);

    if (!IsCdpInputMode() && down
        && WindowUsesUiaClickFallback(TopLevelTargetWindow(TargetHwnd()))) {
        if (TryUiaClickAtClient(cx, cy)) {
            uiaInvokePending_ = true;
            return;
        }
    }
    if (!down && uiaInvokePending_) {
        uiaInvokePending_ = false;
        return;
    }

    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) return;
        MaybeRefreshExtLayout();
        UpdateExtSurfaceSize();
        RememberSoftMouseClientPos(TargetHwnd(), cx, cy);
        int sx = cx;
        int sy = cy;
        ToCaptureSurfaceClient(sx, sy);
        const bool ok = ext_.IsConnected()
            ? ext_.MouseButton(sx, sy, button, down, err)
            : cdp_.MouseButton(sx, sy, button, down, err);
        if (!ok) {
            WindowModeLogf(L"[窗口/后台窗口模式] 网页鼠标按键失败: %s", err.c_str());
        }
        return;
    }
    if (PreferHardwareInput()) {
        SendHardwareCursorToClient(cx, cy);
        MouseButtonEvent(button, down);
        return;
    }
    SyncFakeFocusCursor(cx, cy);
    SyncFakeFocusMouseButton(button, down);
    if (!UsesInProcFakeFocusSoftInput()) {
        PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, down);
    } else {
        WindowModeLogEventf(L"[窗口/后台窗口模式] 假焦点软鼠标 %s %s 客户区(%d,%d) (%s)",
            button == MouseButtonType::Left ? L"左键"
                : button == MouseButtonType::Right ? L"右键"
                : button == MouseButtonType::Middle ? L"中键" : L"侧键",
            down ? L"down" : L"up", cx, cy,
            UsesMapleStoryFakeFocusInput() ? L"共享内存/DirectInput" : L"DLL/PostMessage 队列");
    }
}

namespace {

/// ⚠ 2026-10-05：UIA 兜底**没被执行**的原因，只在首次打一条（点击可能很频繁）。
/// 与 `background_uia_input.cpp` 的 `LogUiaFailOnce`（UIA **内部**失败）配对 ——
/// 两者合起来覆盖「**没走到 UIA**」与「**UIA 走了但失败**」两种情形，
/// 用户报「点击没反应」时能一次定位到是哪一层。
bool g_uiaSkipLogged = false;
void LogUiaSkipOnce(const wchar_t* why) {
    if (g_uiaSkipLogged) return;
    g_uiaSkipLogged = true;
    WindowModeLogEventf(L"[窗口/后台窗口模式] UIA 兜底未执行：%s", why);
}

}  // namespace

bool WindowModeExecutor::TryUiaClickAtClient(int cx, int cy) {
    // ⚠ 2026-10-05：**每条早退都补日志** —— 用户报「UWP 点击没反应」时，
    //   必须一眼看出死在哪一层：是**没走到 UIA**，还是 UIA 内部失败。
    //   （UIA 内部的失败日志在 `background_uia_input.cpp` 的 `LogUiaFailOnce`。）
    //   限流：只在首次失败打一条（点击可能很频繁）。
    if (PreferHardwareInput()) {
        LogUiaSkipOnce(L"PreferHardwareInput()=true（会走假前台/硬件输入，不试 UIA）");
        return false;
    }
    HWND input = TargetHwnd();
    HWND top = TopLevelTargetWindow(input);
    if (!top || !IsWindow(top)) {
        LogUiaSkipOnce(L"目标窗口无效");
        return false;
    }
    int sx = cx, sy = cy;
    if (!ClientToScreenPoint(input ? input : top, sx, sy, sx, sy)) {
        if (!ClientToScreenPoint(top, cx, cy, sx, sy)) {
            LogUiaSkipOnce(L"ClientToScreenPoint 失败（拿不到屏幕坐标）");
            return false;
        }
    }
    // UIA Invoke 对部分应用（UWP 计算器等）会把目标窗口唤到前台：
    // 记住点击前的前台窗口，点击后若目标被唤出则立即还原，保持后台不抢焦点。
    HWND preserveFg = GetForegroundWindow();
    const bool ok = TryUiaInvokeAtScreenPoint(top, sx, sy);
    if (ok) {
        ++uiaInvokeCount_;
        WindowModeLogEventf(L"[窗口/后台窗口模式] UIA 点击 屏幕(%d,%d) 客户区(%d,%d)",
            sx, sy, cx, cy);
        // UIA Invoke 的窗口激活是异步的：快速轮询，一旦发现目标被唤出立即还原原前台，
        // 尽量缩短可见闪烁（后台操作绝不占用前台）。
        if (preserveFg && IsWindow(preserveFg) && preserveFg != top) {
            bool restored = false;
            for (int i = 0; i < 12; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                HWND curFg = GetForegroundWindow();
                HWND curTop = TopLevelTargetWindow(curFg);
                if (curTop != top) continue;
                // 还原点击前的前台窗口：挂到当前前台线程后再 SetForegroundWindow。
                DWORD curThread = GetCurrentThreadId();
                DWORD fgThread = curFg ? GetWindowThreadProcessId(curFg, nullptr) : 0;
                if (fgThread && fgThread != curThread) {
                    AttachThreadInput(curThread, fgThread, TRUE);
                }
                AllowSetForegroundWindow(ASFW_ANY);
                SetForegroundWindow(preserveFg);
                if (fgThread && fgThread != curThread) {
                    AttachThreadInput(curThread, fgThread, FALSE);
                }
                WindowModeLogEvent(L"[窗口/后台窗口模式] UIA 点击后已还原原前台窗口");
                restored = true;
                break;
            }
            if (!restored) {
                // 未检测到目标被唤出：结束前兜底再查一次
                HWND curFg = GetForegroundWindow();
                HWND curTop = TopLevelTargetWindow(curFg);
                if (curTop == top) {
                    DWORD curThread = GetCurrentThreadId();
                    DWORD fgThread = curFg ? GetWindowThreadProcessId(curFg, nullptr) : 0;
                    if (fgThread && fgThread != curThread) {
                        AttachThreadInput(curThread, fgThread, TRUE);
                    }
                    AllowSetForegroundWindow(ASFW_ANY);
                    SetForegroundWindow(preserveFg);
                    if (fgThread && fgThread != curThread) {
                        AttachThreadInput(curThread, fgThread, FALSE);
                    }
                    WindowModeLogEvent(L"[窗口/后台窗口模式] UIA 点击后已还原原前台窗口");
                }
            }
        }
    }
    return ok;
}

void WindowModeExecutor::PostMouseClickAtClient(int cx, int cy, MouseButtonType button,
    bool scaleRecordedClient) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return;
    if (!UsesCdpInput(session_.Config())) {
        std::wstring prepErr;
        if (!PrepareSoftInputFast(prepErr)) return;
    }
    if (!MapScriptPointToClient(cx, cy, scaleRecordedClient)) return;
    // ⚠⚠ 2026-10-05：同 `PostMouseButtonAtClient` —— **必须先把 `(0,0)` 哨兵解析掉**
    //   再交给 UIA，否则 UIA 拿到 `(0,0)` ⇒ `ClientToScreen` 得到**客户区原点**
    //   ⇒ 跑到窗口左上角找元素。（这里原来也排在 UIA 分支之后。）
    ResolveClickClientPos(cx, cy);
    // UWP/WinUI：与 PostMouseButtonAtClient 相同，不依赖 windowRelativeCoordinates。
    if (!IsCdpInputMode()
        && WindowUsesUiaClickFallback(TopLevelTargetWindow(TargetHwnd()))
        && TryUiaClickAtClient(cx, cy)) {
        return;
    }
    WindowModeLogEventf(L"[窗口/后台窗口模式] 点击 → 客户区(%d,%d) %s",
        cx, cy, button == MouseButtonType::Left ? L"左键"
            : button == MouseButtonType::Right ? L"右键"
            : button == MouseButtonType::Middle ? L"中键"
            : button == MouseButtonType::X1 ? L"侧键X1"
            : button == MouseButtonType::X2 ? L"侧键X2" : L"未知");

    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) return;
        MaybeRefreshExtLayout();
        UpdateExtSurfaceSize();
        RememberSoftMouseClientPos(TargetHwnd(), cx, cy);
        int sx = cx;
        int sy = cy;
        ToCaptureSurfaceClient(sx, sy);
        const bool ok = ext_.IsConnected()
            ? ext_.MouseClick(sx, sy, button, err)
            : cdp_.MouseClick(sx, sy, button, err);
        if (ok) return;
        WindowModeLogf(L"[窗口/后台窗口模式] 网页点击失败: %s，回退软点击", err.c_str());
        // 扩展桥超时/卡死时立刻 PostMessage，避免脚本空等像「失焦」。
        if (PrepareSoftInput(err)) {
            SyncFakeFocusCursor(cx, cy);
            SyncFakeFocusMouseButton(button, true);
            PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, true);
            if (UsesFakeFocus(session_.Config(), TargetHwnd())) {
                WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(25));
            }
            SyncFakeFocusMouseButton(button, false);
            PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, false);
        }
        return;
    }
    if (PreferHardwareInput()) {
        SendHardwareCursorToClient(cx, cy);
        MouseClick(button);
        return;
    }
    SyncFakeFocusCursor(cx, cy);
    SyncFakeFocusMouseButton(button, true);
    if (!UsesInProcFakeFocusSoftInput()) {
        PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, true);
    }
    if (UsesFakeFocus(session_.Config(), TargetHwnd())) {
        WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(25));
    }
    SyncFakeFocusMouseButton(button, false);
    if (!UsesInProcFakeFocusSoftInput()) {
        PostMouseButtonToWindow(TargetHwnd(), cx, cy, button, false);
    }
    if (UsesInProcFakeFocusSoftInput()) {
        WindowModeLogEventf(L"[窗口/后台窗口模式] 假焦点软点击 客户区(%d,%d) (%s)",
            cx, cy,
            UsesMapleStoryFakeFocusInput() ? L"共享内存/DirectInput" : L"DLL/PostMessage 队列");
        if (UsesMapleStoryFakeFocusInput() && ++g_mapleSoftClickLogs == 1) {
            LogMapleHookHits(L"首击后");
        }
    }

}

void WindowModeExecutor::PostKeyToTarget(UINT vk, bool down) {
    if (!active_ || vk == 0 || WindowModeCancelled(cancelFlag_)) return;
    // 脚本持键影子集（2026-10-01）：键态恢复后要按它重新对齐，所以每个键事件都记账。
    // ⚠ 记的是**脚本意图**（不是"有没有写进共享内存"）。
    // ⚠⚠ 2026-10-02：**必须放在下面看门狗之前**。看门狗会清"陈旧方向位"，
    //   而它要排除的正是在按着的键；若记账还在后面，触发看门狗的那一次按下就还没进集合，
    //   会被当成陈旧位清掉（用户症状：脚本按 → 却被清掉 ⇒ 角色原地打）。
    if (down) {
        softHeldKeys_.insert(vk);
    } else {
        softHeldKeys_.erase(vk);
    }
    // A：停摆看门狗（挂在这条"每拍都会走"的路径上，判据只是一次整数比较）。
    // 用户实证「点一下窗口就恢复」——这里把它自动化；日志里能看出叫醒成没成。
    {
        const ULONGLONG nowTick = GetTickCount64();
        if (nowTick >= mapleWatchdogNextTick_) {
            mapleWatchdogNextTick_ = nowTick
                + (mapleWatchdogMisses_ >= 2 ? 30000ull : 5000ull);
            if (UsesBackgroundWindow() && FakeFocusActive()
                && LooksLikeMapleStoryTarget(session_.Config(), TargetHwnd())) {
                const bool alive = WaitMapleClientAlive(TopLevelTargetWindow(TargetHwnd()),
                    cancelFlag_, 800, /*allowWake=*/false, L"会话中");
                mapleClientDormant_ = !alive;
                if (alive) {
                    mapleWatchdogMisses_ = 0;
                } else if (mapleWatchdogMisses_ < 3) {
                    ++mapleWatchdogMisses_;
                }
                const HWND top = TopLevelTargetWindow(TargetHwnd());
                if (top && top != mapleIdentityHwnd_) {
                    WindowModeLog(L"[窗口/后台窗口模式] ⚠ 本会话的目标窗口变了（可能切到了另一份"
                                  L"客户端/另一开）—— 下面这行是变更后的实际目标");
                    mapleIdentityHwnd_ = top;
                    LogMapleTargetIdentity(top, L"变更后");
                }
                // ★键态停摆 / 恢复（2026-10-01）：客户端"消息在泵但不查键态"时，
                //   "还按着 ← 时停了轮询"会让它永远看不到那次松开 ⇒ 一路顶墙。
                //   停摆期清掉陈旧方向位；恢复轮询后按脚本持键重新对齐。
                DWORD ksGaks = 0, ksDi = 0, ksDiData = 0, ksLastCb = 0, ksHr = 0, ksGfw = 0, ksFocus = 0;
                if (FakeFocusSoftInput_ReadMapleHits(ksGaks, ksDi, ksDiData, ksLastCb,
                        ksHr, ksGfw, ksFocus)) {
                    const KeyStatePhase phase =
                        EvaluateKeyStatePhase(keyStateLastGaks_, ksGaks, keyStateStalled_);
                    keyStateLastGaks_ = ksGaks;
                    if (phase == KeyStatePhase::Stalled) {
                        keyStateStalled_ = true;
                        // ⚠⚠ 传入脚本此刻的持键集：**脚本按着的键不是"陈旧位"**，一个都不许清。
                        //   前科（用户 2026-10-02）：旧 DLL 把 gaks 夹在 255 ⇒ 每轮都误判停摆 ⇒
                        //   把脚本从 t=2.03s 起一直按着的 → 清掉 ⇒「原地打」。
                        const int cleared = ClearStaleArrowSoftKeys(softHeldKeys_);
                        WindowModeLogf(
                            L"[窗口/后台窗口模式] 冒险岛后台走路：**键态停摆**（客户端在收消息但"
                            L"一直不查键态）—— 已清方向键陈旧位（%d 个）；持键 %zu 个待恢复时重发",
                            cleared, softHeldKeys_.size());
                    } else if (phase == KeyStatePhase::Resumed) {
                        keyStateStalled_ = false;
                        std::vector<UINT> held(softHeldKeys_.begin(), softHeldKeys_.end());
                        const int pressed = ResyncSoftHeldKeys(held);
                        WindowModeLogf(
                            L"[窗口/后台窗口模式] 冒险岛后台走路：**轮询恢复**—— 已按脚本持键"
                            L"重新对齐（补发按下 %d 个，共持键 %zu 个）",
                            pressed, held.size());
                    }
                }
            }
        }
    }
    // ② 休眠期"不发按下"从**抑制**改成**只记账**（2026-09-30 深夜，用户录制日志为证）。
    //    `录制宏.txt` 里出现**未配对/错序**的按键事件（`松开V` 之前没有 `按下V`、孤立的
    //    `松开←`、`按下空格键` 后面跟着错位的松开）—— 而"抑制按下、抬起照发"正好会制造这种
    //    流：按下被丢掉、抬起照发 ⇒ 录制/软键态与真实意图相反 ⇒ 回放里角色不动或乱走。
    //    ⇒ 结论：**永不丢事件**（丢事件比"迟到生效"更坏，它会污染录制数据）。
    //    这里保留检测与日志（给人看），但**不改变任何投递**。
    if (mapleClientDormant_ && down && IsArrowKeyVk(vk)) {
        const ULONGLONG nowTick = GetTickCount64();
        if (nowTick >= mapleDormantRetryTick_) {
            mapleDormantRetryTick_ = nowTick + 1000;
            WindowModeLogf(L"[窗口/后台窗口模式] 冒险岛后台走路：客户端停摆中"
                L"（方向键 vk=0x%02X 仍照常投递；仅记账提醒，避免污染录制/键态配对）", vk);
        }
    }

    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) return;
        const bool ok = ext_.IsConnected()
            ? ext_.KeyEvent(vk, down, err)
            : cdp_.KeyEvent(vk, down, err);
        if (!ok) {
            WindowModeLogf(L"[窗口/后台窗口模式] 网页按键失败: %s", err.c_str());
        }
        return;
    }
    // ⚠ 键盘动作与相对移动同属**每拍一次**的最热路径（一段 FPS 回放里
    //   KEYDOWN/KEYUP 成百上千次）。此前这里用的是**完整** `PrepareSoftInput`，
    //   每一次按键都 `EnumChildWindows` 全树找渲染子窗 —— 对 GLFW/MC 这类
    //   绑定恒为顶层的目标纯属白跑。用户日志里「大尖峰紧跟在键盘动作之后」
    //   正是这条：软键态写完还要等一次全树枚举才轮到下一拍。
    //   与相对移动用同一条快速路径（判据 `CanUseSoftInputFastPath`，已自检）。
    if (!PrepareSoftInputFast(err)) return;
    if (PreferHardwareInput()) {
        const bool chromiumShell = LooksLikeChromiumShellTarget(session_.Config(), TargetHwnd());
        if (!chromiumShell) DropFakeFocusForHardwareInput(L"本机点击回退");
        EnsureHardwareInputFocus();
        if (chromiumShell && FakeFocusSoftInput_IsAttached()) {
            FakeFocusSoftInput_SetKey(vk, down);
        }
        SendKeyboardKey(vk, down);
        WindowModeLogEventf(L"[窗口/后台窗口模式] 本机按键 vk=0x%02X %s%s",
            vk, down ? L"down" : L"up",
            chromiumShell ? L" (Chromium壳 假前台回退)" : L"");
        return;
    }
    // Chromium 壳：进程内 PostMessage。冒险岛不走这条（UsesMapleStoryFakeFocusInput 恒 false）。
    if (UsesInProcFakeFocusSoftInput()) {
        FakeFocusSoftInput_SetKey(vk, down);
        WindowModeLogEventf(L"[窗口/后台窗口模式] 假焦点软按键 vk=0x%02X %s (%s)",
            vk, down ? L"down" : L"up",
            UsesMapleStoryFakeFocusInput() ? L"共享内存/DirectInput" : L"DLL/PostMessage 队列");
        if (UsesMapleStoryFakeFocusInput() && ++g_mapleSoftKeyLogs == 1) {
            LogMapleHookHits(L"首键后");
        }
        // 软键态是「当前值」、事件走灌键线程：不等目标处理完就写下一步，
        // 组合键（Ctrl+V/Ctrl+C…）会被目标读成「Ctrl 已抬起」→ 只出 v 不粘贴。
        // 冒险岛走 DirectInput 共享内存（等消息队列无意义），其余（Chromium 壳/Qt/微信）都等。
        if (!UsesMapleStoryFakeFocusInput()) {
            if (!g_softKeyPacingOff && !WaitSoftKeyPostTurn(TargetHwnd())) {
                g_softKeyPacingOff = true;  // 目标不应答（超时/权限）：本次运行不再逐键等，免得卡住回放
                WindowModeLog(L"[窗口/后台窗口模式] 软键屏障无应答：本会话改为不等待（组合键可能退化成普通字符）");
            }
        }
        return;
    }
    HWND top = TopLevelTargetWindow(TargetHwnd());
    if (FakeFocusSoftInput_IsAttached()) {
        FakeFocusSoftInput_SetKey(vk, down);
    }
    // DeSmuME 只吃 GetAsyncKeyState 轮询，外层 WM_KEY* 无效且可能干扰。
    if (!IsDesktopEmulatorTarget(top, &session_.Config())) {
        PostKeyToWindow(TargetHwnd(), vk, down);
    }
}

void WindowModeExecutor::PostScrollWheelAtClient(int cx, int cy, int steps, bool vertical, bool positive,
    bool scaleRecordedClient) {
    if (!active_ || steps <= 0) return;
    if (!UsesCdpInput(session_.Config())) {
        std::wstring prepErr;
        if (!PrepareSoftInputFast(prepErr)) return;
    }
    if (!MapScriptPointToClient(cx, cy, scaleRecordedClient)) return;
    ResolveClickClientPos(cx, cy);
    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) return;
        const bool ok = ext_.IsConnected()
            ? ext_.Scroll(cx, cy, steps, vertical, positive, err)
            : cdp_.Scroll(cx, cy, steps, vertical, positive, err);
        if (!ok) {
            WindowModeLogf(L"[窗口/后台窗口模式] 网页滚轮失败: %s", err.c_str());
        }
        return;
    }
    if (PreferHardwareInput()) {
        SendHardwareCursorToClient(cx, cy);
        // ⚠ 展开成多条 SendInput（2026-09-30）：原来是 `(positive ? WHEEL_DELTA : -WHEEL_DELTA) * steps`
        //   一次发完 —— 步数一大，`SendInput` 的 `mouseData`（DWORD，语义有符号）
        //   仍会被下层按 16 位解释；而且"一条消息 = N 格"与另外两条路径
        //   （宿主 PostMessage / 假焦点 DLL）的"一格一条"语义不一致，同一脚本在
        //   不同输入路径上手感不同。现在三条路径共用 `MouseWheelEventsForSteps`。
        int deltas[8]{};
        const int n = MouseWheelEventsForSteps(steps, positive, deltas,
            static_cast<int>(sizeof(deltas) / sizeof(deltas[0])));
        for (int i = 0; i < n; ++i) {
            ForegroundInputRouter::Instance().Wheel(deltas[i], !vertical);
        }
        return;
    }
    SyncFakeFocusCursor(cx, cy);
    if (UsesInProcFakeFocusSoftInput()) {
        FakeFocusSoftInput_PushWheel(vertical, positive, steps);
        // ⚠ 别把"投出去"说成"送到了"（2026-09-30）：`PushWheel` 只是入队，
        //   真正的落点是目标进程里 DLL 决定的（Raw Input 直投 / 进程内 PostMessage）。
        //   原本文案写死「DLL/PostMessage 队列」，对走 Raw Input 的游戏是**假承诺** ——
        //   用户看到这行就以为滚轮发出去了，实际游戏一条都没收到。
        WindowModeLogEventf(L"[窗口/后台窗口模式] 假焦点软滚轮 入队 steps=%d %s（%s；"
            L"落点由目标进程内的 DLL 分流：Raw Input 直投或进程内 PostMessage）",
            steps, vertical ? L"竖向" : L"横向",
            UsesMapleStoryFakeFocusInput() ? L"共享内存/DirectInput" : L"DLL/软输入队列");
        return;
    }
    PostScrollWheelToWindow(TargetHwnd(), cx, cy, steps, vertical, positive);
}

void WindowModeExecutor::SendQuickInputToTarget(const std::wstring& text, double charInterval) {
    if (!active_) return;
    std::wstring err;
    if (UsesCdpInput(session_.Config())) {
        if (!EnsureCdpReady(err)) {
            WindowModeLogf(L"[窗口/后台窗口模式] 快捷输入跳过(网页桥): %s", err.c_str());
            return;
        }
        const bool ok = ext_.IsConnected()
            ? ext_.InsertText(text, err)
            : cdp_.InsertText(text, err);
        if (!ok) {
            WindowModeLogf(L"[窗口/后台窗口模式] 网页快捷输入失败: %s", err.c_str());
        }
        (void)charInterval;
        return;
    }
    if (!PrepareSoftInput(err)) {
        WindowModeLogf(L"[窗口/后台窗口模式] 快捷输入跳过: %s", err.c_str());
        return;
    }
    if (!session_.RefreshInputBinding(err)) {
        WindowModeLogf(L"[窗口/后台窗口模式] 快捷输入刷新绑定失败: %s", err.c_str());
        return;
    }

    if (PreferHardwareInput()) {
        DropFakeFocusForHardwareInput(L"快捷输入回退");
        SendQuickInputText(text, charInterval, cancelFlag_);
        return;
    }
    HWND hwnd = TargetHwnd();
    const bool allowForeground = UsesFakeFocus(session_.Config(), TargetHwnd())
        ? false
        : session_.Config().allowForegroundInputFallback;
    PostQuickInputToWindow(hwnd, text, charInterval, allowForeground, cancelFlag_);

}

bool WindowModeExecutor::ResolveClientSearchRect(const ScriptAction& a,
    int& x1, int& y1, int& x2, int& y2) const {
    if (!active_) return false;

    HWND hwnd = VisionCaptureHwnd();
    if (!hwnd || !IsWindow(hwnd)) hwnd = TargetHwnd();

    int clientW = 0;
    int clientH = 0;
    int liveW = 0;
    int liveH = 0;
    RECT clientRc{};
    if (hwnd && GetClientRect(hwnd, &clientRc)) {
        liveW = std::max(0, static_cast<int>(clientRc.right - clientRc.left));
        liveH = std::max(0, static_cast<int>(clientRc.bottom - clientRc.top));
        clientW = liveW;
        clientH = liveH;
    }

    const auto& st = session_.State();
    if (clientW <= 0 || clientH <= 0) {
        clientW = st.clientW;
        clientH = st.clientH;
    }
    if (clientW <= 0 || clientH <= 0) {
        WindowModeLogf(L"[窗口/后台窗口模式] ResolveClientSearchRect: 客户区无效 hwnd=0x%p live=%dx%d bound=%dx%d",
            hwnd, liveW, liveH, st.clientW, st.clientH);
        return false;
    }

    // 窗口/后台窗口模式：绝对「选取区域」不生效，默认整个客户区。
    // 「根据图片选取区域」在 FindImage 命中后再用 imageRegion 二次筛选。
    if (!a.searchFullScreen && a.searchX2 > a.searchX1 && a.searchY2 > a.searchY1) {
        WindowModeLogf(
            L"[窗口/后台窗口模式] ResolveClientSearchRect: 忽略绝对选取区域 (%d,%d)-(%d,%d)，改用全客户区 %dx%d",
            a.searchX1, a.searchY1, a.searchX2, a.searchY2, clientW, clientH);
    }
    if (!EffectiveWindowModeClientSearchRect(clientW, clientH, x1, y1, x2, y2)) {
        WindowModeLogf(L"[窗口/后台窗口模式] ResolveClientSearchRect: 全客户区无效 %dx%d", clientW, clientH);
        return false;
    }
    return true;
}

bool WindowModeExecutor::MapClientRect(int cx1, int cy1, int cx2, int cy2,
    int& sx1, int& sy1, int& sx2, int& sy2) const {
    if (!active_) return false;
    // 入参始终是目标客户区（ResolveClientSearchRect / FindImageClient 命中框）。
    // 不能看 coordSpace：默认 screenAbsolute，原样拷贝会把 (0,0,w,h) 当成屏幕坐标，
    // OCR/AI/保存图片就会截到虚拟屏左上角而不是目标窗口。
    HWND hwnd = VisionCaptureHwnd();
    if (!hwnd || !IsWindow(hwnd)) hwnd = TargetHwnd();
    return MapClientRectToScreen(hwnd, cx1, cy1, cx2, cy2, sx1, sy1, sx2, sy2);
}

ImageMatchOutput WindowModeExecutor::FindImageClient(const ScriptAction& a,
    HBITMAP lockedBmp, int lockX, int lockY) {
    (void)lockX;
    (void)lockY;
    ImageMatchOutput output{};
    if (!active_) {
        WindowModeLog(L"[窗口/后台窗口模式] FindImageClient: executor 未激活");
        return output;
    }
    if (WindowModeCancelled(cancelFlag_)) return output;

    std::wstring err;
    if (!EnsureTargetBound(err)) {
        WindowModeLogf(L"[窗口/后台窗口模式] FindImageClient: EnsureTargetBound 失败 %s",
            err.empty() ? L"(无详情)" : err.c_str());
        return output;
    }
    if (WindowModeCancelled(cancelFlag_)) return output;

    if (UsesBackgroundWindow()) {
        std::wstring geoErr;
        if (!EnsurePlaybackGeometry(geoErr)) {
            WindowModeLogf(L"[窗口/后台窗口模式] FindImageClient: 最小化目标无法安静还原 %s",
                geoErr.c_str());
            return output;
        }
    }

    HWND prepRoot = TopLevelTargetWindow(CaptureTargetHwnd());
    int deskAtStart = -1;
    {
        auto& vda = VirtualDesktopAccessor::Instance();
        deskAtStart = vda.GetCurrentDesktopNumber();
    }
    WindowModeLogDesktopSnap(L"找图前", prepRoot);

    ScriptAction probe = a;
    if (probe.imagePath.empty() && !probe.aiTargetImagePath.empty()) {
        probe.imagePath = probe.aiTargetImagePath;
    }

    HBITMAP tmpl = nullptr;
    ImageMatchOptions opt;
    TemplateScale ts = FindImageTemplateScale();
    if (ts.sx <= 0.0 || ts.sy <= 0.0) {
        ts = FindImageSurfaceScale(0, 0);
    }
    const PreparedFindImageMatch findMatch = PrepareFindImageMatch(probe, ts);
    tmpl = findMatch.bitmap;
    opt = findMatch.options;
    if (!tmpl) {
        WindowModeLog(L"[窗口/后台窗口模式] FindImageClient: 模板图加载失败");
        return output;
    }
    int keepMatches = 1;
    if (a.type == ActionType::MultiMatch) {
        keepMatches = (a.multiMatchMode == 1)
            ? std::clamp(a.multiMatchMax, 1, kMultiMatchMaxHits)
            : 1;
    }
    opt.maxMatches = keepMatches;
    if (keepMatches <= 1)
        opt.disablePyramid = true;
    opt.maxOverlap = 0.5;

    auto applyWindowRelativeSurface = [&](int surfaceW, int surfaceH,
            ImageMatchOptions& io, int& x1, int& y1, int& x2, int& y2) {
        if (surfaceW <= 0 || surfaceH <= 0) return;
        if (session_.Config().windowRelativeCoordinates) {
            const TemplateScale surfTs = FindImageSurfaceScale(surfaceW, surfaceH);
            if (surfTs.sx > 0.0 && surfTs.sy > 0.0) {
                ts = surfTs;
                io = BuildExecutionFindImageOptions(probe, ts);
                io.maxMatches = keepMatches;
                if (keepMatches <= 1)
                    io.disablePyramid = true;
                io.maxOverlap = 0.5;
                // 窗口拉伸可大于 1.0；全局 anamorphic 选项曾把 scaleMax 封在 1.05。
                const double lo = std::min(surfTs.sx, surfTs.sy);
                const double hi = std::max(surfTs.sx, surfTs.sy);
                io.scaleMin = std::max(0.1, std::min(io.scaleMin, lo * 0.92));
                io.scaleMax = std::max(io.scaleMax, hi * 1.08);
                if (io.scaleMax < io.scaleMin) io.scaleMax = io.scaleMin;
            }
        }
        int liveW = 0, liveH = 0;
        LiveClientSize(liveW, liveH);
        if (liveW <= 0 || liveH <= 0) {
            x1 = 0;
            y1 = 0;
            x2 = surfaceW;
            y2 = surfaceH;
            return;
        }
        const bool fullClient = probe.searchFullScreen
            || (x1 <= 0 && y1 <= 0 && x2 >= liveW - 1 && y2 >= liveH - 1);
        if (fullClient) {
            x1 = 0;
            y1 = 0;
            x2 = surfaceW;
            y2 = surfaceH;
            return;
        }
        x1 = std::clamp((x1 * surfaceW) / liveW, 0, surfaceW);
        y1 = std::clamp((y1 * surfaceH) / liveH, 0, surfaceH);
        x2 = std::clamp((x2 * surfaceW) / liveW, 0, surfaceW);
        y2 = std::clamp((y2 * surfaceH) / liveH, 0, surfaceH);
    };

    auto mapSurfaceMatchesToLiveClient = [&](ImageMatchOutput& matched, int surfaceW, int surfaceH) {
        if (!session_.Config().windowRelativeCoordinates) return;
        if (surfaceW <= 0 || surfaceH <= 0) return;
        int liveW = 0, liveH = 0;
        if (!LiveClientSize(liveW, liveH) || liveW <= 0 || liveH <= 0) return;
        if (surfaceW == liveW && surfaceH == liveH) return;
        for (auto& m : matched.matches) {
            m.topLeftX = (m.topLeftX * liveW) / surfaceW;
            m.topLeftY = (m.topLeftY * liveH) / surfaceH;
            m.bottomRightX = (m.bottomRightX * liveW) / surfaceW;
            m.bottomRightY = (m.bottomRightY * liveH) / surfaceH;
            m.x = (m.x * liveW) / surfaceW;
            m.y = (m.y * liveH) / surfaceH;
        }
    };

    // 窗口/后台窗口模式 CDP：找图必须走扩展 HTTP 截图（PrintWindow/Cloak 必切屏，已证伪）。
    // 后台 CDP：可扩展优先，失败再同桌面 Win32。
    // 注意：以前这里按扩展版本号（≥1.1.15 / ≥1.0.21）判断能力，但扩展版本号已重置为
    // 1.0.0 起重新计数，版本比较不再有意义，反而会把可用路径判成不可用（截图链路整体
    // 关闭）。现在只以「扩展是否已连接」为准；能力不足时由截图命令自身的错误路径兜底。
    const bool windowModeCdp = !UsesBackgroundWindow() && UsesCdpInput(session_.Config());
    const bool cdpExt = UsesCdpInput(session_.Config());
    const bool cdpExtVision = UsesCdpInput(session_.Config()) && ext_.IsConnected();
    const bool preferExtShot = UsesCdpInput(session_.Config()) && ext_.IsConnected();
    if (preferExtShot) {
        if (UserOnMacroDesktopNow() && prepRoot && IsWindow(prepRoot)) {
            const DWORD settle0 = GetTickCount();
            RaiseMacroDesktopWindowForWatch(prepRoot);
            for (int i = 0; i < 8; ++i) {
                if (WindowModeCancelled(cancelFlag_)) break;
                if (!IsMacroVisionInvisibilityActive(prepRoot) && !IsIconic(prepRoot)) {
                    RECT wr{};
                    if (GetWindowRect(prepRoot, &wr) && wr.left > -2000 && wr.top > -2000) break;
                }
                RaiseMacroDesktopWindowForWatch(prepRoot);
                WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(40));
            }
            WindowModeLogf(L"[窗口/后台窗口模式] 找图观看就绪 settle=%ums onMacro=1 ready=%d",
                GetTickCount() - settle0, IsMacroVisionCaptureReady(prepRoot) ? 1 : 0);
        }
        MaybeRefreshExtLayout();
        const auto& st = session_.State();
        int refW = 0, refH = 0;
        if (ResolveExtVisionSurface(ext_, CaptureTargetHwnd(), st.clientW, st.clientH, refW, refH)) {
            WindowModeLogf(L"[窗口/后台窗口模式] 找图表面: pageCss×dpr → %dx%d (onMacro=%d)",
                refW, refH, UserOnMacroDesktopNow() ? 1 : 0);
        }

        HBITMAP shot = nullptr;
        int sw = 0, sh = 0;
        bool canvasSpace = false;
        std::wstring shotErr;
        const DWORD tShot0 = GetTickCount();
        const bool got = cdpExtVision
            ? ext_.CaptureScreenshotForVisionMatch(refW, refH, &shot, &sw, &sh, &canvasSpace, shotErr)
            : ext_.CaptureScreenshotForClientMatch(refW, refH, &shot, &sw, &sh, shotErr);
        const DWORD shotMs = GetTickCount() - tShot0;
        if (got && shot && sw >= 64 && sh >= 64 && !IsCaptureLikelyBlank(shot)) {
            ext_.SetSurfaceSize(refW, refH);
            int cx1 = 0, cy1 = 0, cx2 = sw, cy2 = sh;
            if (!canvasSpace) {
                if (!ResolveClientSearchRect(probe, cx1, cy1, cx2, cy2)) {
                    cx1 = 0;
                    cy1 = 0;
                    cx2 = sw;
                    cy2 = sh;
                }
                if (cx2 > sw || cy2 > sh || cx2 <= cx1 || cy2 <= cy1) {
                    cx1 = 0;
                    cy1 = 0;
                    cx2 = sw;
                    cy2 = sh;
                }
            }
            applyWindowRelativeSurface(sw, sh, opt, cx1, cy1, cx2, cy2);
            cx1 = std::clamp(cx1, 0, sw);
            cy1 = std::clamp(cy1, 0, sh);
            cx2 = std::clamp(cx2, 0, sw);
            cy2 = std::clamp(cy2, 0, sh);

            ImageMatchOutput matched = FindTemplateInFrozenScreenMulti(
                shot, 0, 0, cx1, cy1, cx2, cy2, tmpl, opt);
            const DWORD tMatch0 = GetTickCount();
            if (canvasSpace) {
                int ix = 0, iy = 0, iw = 0, ih = 0, pw = 0, ph = 0;
                ext_.GetIframeCssRect(ix, iy, iw, ih);
                ext_.GetPageCssSize(pw, ph);
                MapCanvasMatchesToSurface(matched, sw, sh, refW, refH, ix, iy, iw, ih, pw, ph);
                WindowModeLogf(
                    L"[窗口/后台窗口模式] canvas→iframe映射: canvas=%dx%d surface=%dx%d iframeCss=(%d,%d)%dx%d pageCss=%dx%d",
                    sw, sh, refW, refH, ix, iy, iw, ih, pw, ph);
            }
            const DWORD matchMs = GetTickCount() - tMatch0;
            DeleteObject(shot);
            WindowModeLogf(
                L"[窗口/后台窗口模式] 匹配(扩展截图%s): 命中=%zu best=%.1f%% peakNcc=%.1f%% pixelAgree=%.1f%% shot=%ums match=%ums",
                canvasSpace ? L"/canvas" : L"/client",
                matched.matches.size(),
                matched.matches.empty() ? 0.0 : matched.matches.front().score,
                matched.debugBestNccPercent,
                matched.debugBestPixelAgreePercent,
                shotMs, matchMs);
            DeleteBitmapHandle(tmpl);
            WindowModeLogDesktopSnap(L"找图后", prepRoot);
            return matched;
        }
        if (shot) {
            DeleteObject(shot);
            shot = nullptr;
        }
        WindowModeLogf(
            L"[窗口/后台窗口模式] 扩展截图不可用: %s",
            shotErr.empty() ? L"(无详情)" : shotErr.c_str());
        if (windowModeCdp) {
            // 禁止回退 PrintWindow（sameProc Edge 必切屏）。
            WindowModeLog(L"[窗口/后台窗口模式] 窗口模式 CDP：禁止 Win32 PrintWindow 回退（零切屏）");
            DeleteBitmapHandle(tmpl);
            WindowModeLogDesktopSnap(L"找图后", prepRoot);
            return output;
        }
    } else if (UsesCdpInput(session_.Config()) && !ext_.IsConnected()) {
        // 扩展未连接：窗口/后台窗口模式 CDP 没有截图来源，直接返回空匹配（禁止 PrintWindow 回退）
        MaybeRefreshExtLayout();
        WindowModeLog(L"[窗口/后台窗口模式] 扩展未连接：无 HTTP 截图能力");
        if (windowModeCdp) {
            DeleteBitmapHandle(tmpl);
            return output;
        }
    }

    // CDP：禁止 ShowMacro/Cloak latch。窗口/后台窗口模式不应再落到下方 Win32。
    ScopedVisionCapturePrep prep(CaptureTargetHwnd(), UsesBackgroundWindow());
    WindowModeLogDesktopSnap(L"截图准备后", prepRoot);

    if (WindowModeCancelled(cancelFlag_)) {
        DeleteBitmapHandle(tmpl);
        return output;
    }
    if (!prep.Ready()) {
        WindowModeLog(L"[窗口/后台窗口模式] FindImageClient: VisionPrep 未就绪");
        DeleteBitmapHandle(tmpl);
        return output;
    }
    MaybeRefreshExtLayout();
    if (UsesBackgroundWindow()) {
        WindowModeSleepInterruptible(cancelFlag_, std::chrono::milliseconds(40));
        if (WindowModeCancelled(cancelFlag_)) {
            DeleteBitmapHandle(tmpl);
            return output;
        }
    }

    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
    if (!ResolveClientSearchRect(probe, cx1, cy1, cx2, cy2)) {
        WindowModeLog(L"[窗口/后台窗口模式] FindImageClient: ResolveClientSearchRect 失败");
        DeleteBitmapHandle(tmpl);
        return output;
    }

    WindowModeLogVerbosef(
        L"[窗口/后台窗口模式] FindImageClient: 模板 %dx%d scale=%.3fx%.3f matchScale=%.3f~%.3f thr=%.0f search=(%d,%d)-(%d,%d)",
        findMatch.templateW, findMatch.templateH, ts.sx, ts.sy,
        opt.scaleMin, opt.scaleMax, opt.thresholdPercent, cx1, cy1, cx2, cy2);

    const HWND captureHwnd = VisionCaptureHwnd();
    const HWND inputHwnd = TargetHwnd();

    auto captureAndMatch = [&]() -> ImageMatchOutput {
        if (lockedBmp) {
            BITMAP lbm{};
            int lw = 0, lh = 0;
            if (GetObjectW(lockedBmp, sizeof(lbm), &lbm)) {
                lw = lbm.bmWidth;
                lh = lbm.bmHeight;
            }
            int mx1 = cx1, my1 = cy1, mx2 = cx2, my2 = cy2;
            ImageMatchOptions matchOpt = opt;
            if (lw > 0 && lh > 0) {
                applyWindowRelativeSurface(lw, lh, matchOpt, mx1, my1, mx2, my2);
            }
            ImageMatchOutput matched = FindTemplateInFrozenScreenMulti(
                lockedBmp, 0, 0, mx1, my1, mx2, my2, tmpl, matchOpt);
            mapSurfaceMatchesToLiveClient(matched, lw, lh);
            MapMatchResultsToInputClient(captureHwnd, inputHwnd, matched);
            return matched;
        }
        WindowCaptureResult capture = CaptureWindowClientForVision(captureHwnd);
        if (!capture.bitmap) {
            WindowModeLog(L"[窗口/后台窗口模式] FindImageClient: 截图失败");
            return {};
        }
        const auto& st = session_.State();
        if (st.clientW > 0 && st.clientH > 0
            && (capture.w < st.clientW / 2 || capture.h < st.clientH / 2)) {
            WindowModeLogf(L"[窗口/后台窗口模式] FindImageClient: 截图尺寸偏小 %dx%d 期望约%dx%d",
                capture.w, capture.h, st.clientW, st.clientH);
        }
        const bool blank = IsCaptureLikelyBlank(capture.bitmap);
        WindowModeLogVerbosef(L"[窗口/后台窗口模式] FindImageClient: 截图 %dx%d blank=%d print=%d wgc=%d",
            capture.w, capture.h, blank ? 1 : 0, capture.fromPrintWindow ? 1 : 0,
            capture.fromPrintWindow ? 0 : 1);
        if (blank) {
            DeleteObject(capture.bitmap);
            return {};
        }
        int mx1 = cx1, my1 = cy1, mx2 = cx2, my2 = cy2;
        ImageMatchOptions matchOpt = opt;
        applyWindowRelativeSurface(capture.w, capture.h, matchOpt, mx1, my1, mx2, my2);
        ImageMatchOutput matched = FindTemplateInFrozenScreenMulti(
            capture.bitmap, 0, 0, mx1, my1, mx2, my2, tmpl, matchOpt);
        if (IsCdpInputMode() && ext_.IsConnected()) {
            const auto& stCap = session_.State();
            if (stCap.clientW >= 400 && stCap.clientH >= 300) {
                ext_.SetSurfaceSize(stCap.clientW, stCap.clientH);
            } else if (capture.w >= 64 && capture.h >= 64) {
                ext_.SetSurfaceSize(capture.w, capture.h);
            }
        }
        DeleteObject(capture.bitmap);
        mapSurfaceMatchesToLiveClient(matched, capture.w, capture.h);
        MapMatchResultsToInputClient(captureHwnd, inputHwnd, matched);
        WindowModeLogVerbosef(L"[窗口/后台窗口模式] FindImageClient: 匹配数=%zu 最高=%.1f%%",
            matched.matches.size(),
            matched.matches.empty() ? 0.0 : matched.matches.front().score);
        return matched;
    };

    output = captureAndMatch();
    // PrintWindow 可能短暂抢到宏桌面：立刻 Correct/HoldView（禁止放任闪屏）。
    // CDP：禁止 Correct/HoldView（用户可进「鼠标宏」）。
    if (!cdpExt) {
        if (!UserOnMacroDesktopNow()) {
            /* view-steal removed */
            /* HoldPreferred removed */
        }
    }
    WindowModeLogDesktopSnap(L"找图后", prepRoot);
    if (!cdpExt && deskAtStart >= 0) {
        auto& vda = VirtualDesktopAccessor::Instance();
        const int nowDesk = vda.GetCurrentDesktopNumber();
        if (nowDesk >= 0 && nowDesk != deskAtStart) {
            WindowModeLogf(L"[窗口/后台窗口模式] 找图后 UserDesk 变化: %d->%d",
                deskAtStart, nowDesk);
            if (!UserOnMacroDesktopNow()) {
                /* view-steal removed */
                /* HoldPreferred removed */
            }
        }
    }

    DeleteBitmapHandle(tmpl);
    return output;
}

WindowModeExecutor::VisionPipelineDiag WindowModeExecutor::DiagnoseVisionPipeline() {
    VisionPipelineDiag diag{};
    if (!active_) {
        diag.ensureErr = L"executor 未激活";
        return diag;
    }

    diag.ensureReadyOk = EnsureTargetBound(diag.ensureErr);
    if (!diag.ensureReadyOk) return diag;

    if (!PrepareVisionCapture()) {
        diag.ensureErr = L"PrepareVisionCapture 失败";
        return diag;
    }

    ScopedVisionCapturePrep prep(CaptureTargetHwnd(), UsesBackgroundWindow());
    diag.prepReady = prep.Ready();
    if (!diag.prepReady) return diag;

    const HWND captureHwnd = VisionCaptureHwnd();
    WindowCaptureResult capture = CaptureWindowClient(captureHwnd);
    if (!capture.bitmap) return diag;

    diag.captureW = capture.w;
    diag.captureH = capture.h;
    diag.captureBlank = IsCaptureLikelyBlank(capture.bitmap);
    diag.captureOk = true;
    DeleteObject(capture.bitmap);
    return diag;
}

bool WindowModeExecutor::LockWindowCapture(HBITMAP& outBmp, int& outX, int& outY) {
    if (!active_ || WindowModeCancelled(cancelFlag_)) return false;
    if (!PrepareVisionCapture()) return false;
    ScopedVisionCapturePrep prep(CaptureTargetHwnd(), UsesBackgroundWindow());
    if (!prep.Ready()) return false;

    const HWND captureHwnd = VisionCaptureHwnd();
    WindowCaptureResult capture = CaptureWindowClient(captureHwnd);
    if (!capture.bitmap) return false;
    outBmp = capture.bitmap;
    outX = 0;
    outY = 0;
    return !IsCaptureLikelyBlank(outBmp);
}

HBITMAP WindowModeExecutor::CaptureScreenRegionFromWindow(int sx1, int sy1, int sx2, int sy2,
    HBITMAP lockedBmp, int lockX, int lockY) {
    (void)lockX;
    (void)lockY;
    if (!active_ || WindowModeCancelled(cancelFlag_)) return nullptr;
    if (!PrepareVisionCapture()) return nullptr;

    ScopedVisionCapturePrep prep(CaptureTargetHwnd(), UsesBackgroundWindow());
    if (!prep.Ready()) return nullptr;

    if (lockedBmp) {
        HWND hwnd = VisionCaptureHwnd();
        int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
        if (!ScreenToClientPoint(hwnd, sx1, sy1, cx1, cy1)
            || !ScreenToClientPoint(hwnd, sx2, sy2, cx2, cy2)) {
            return nullptr;
        }
        return CropBitmapClientRegion(lockedBmp, cx1, cy1, cx2, cy2);
    }

    HWND hwnd = VisionCaptureHwnd();
    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
    if (!ScreenToClientPoint(hwnd, sx1, sy1, cx1, cy1)
        || !ScreenToClientPoint(hwnd, sx2, sy2, cx2, cy2)) {
        return nullptr;
    }
    WindowCaptureResult region = CaptureWindowRegion(hwnd, cx1, cy1, cx2, cy2);
    return region.bitmap;
}

OcrEngineOutput WindowModeExecutor::RunOcrOnClientRegion(const ScriptAction& a,
    HBITMAP lockedBmp, int lockX, int lockY) {
    OcrEngineOutput output{};
    if (!active_) {
        output.error = L"目标窗口/后台窗口模式未激活";
        return output;
    }

    int sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
    if (!ResolveOcrScreenRect(a, sx1, sy1, sx2, sy2, lockedBmp, lockX, lockY)) {
        output.error = L"无法定位识别区域";
        return output;
    }

    HBITMAP regionBmp = CaptureScreenRegionFromWindow(sx1, sy1, sx2, sy2, lockedBmp, lockX, lockY);
    if (!regionBmp) {
        output.error = L"无法截取识别区域";
        return output;
    }

    output = RunOcrOnBitmap(regionBmp, sx1, sy1, a.ocrDigitsOnly);
    DeleteObject(regionBmp);
    return output;
}

bool WindowModeExecutor::GetCursorClientPos(int& cx, int& cy) const {
    if (!active_) return false;
    // Soft-input modes do not move the system cursor; report last posted client position.
    // (0,0) soft 视为未播种，继续读系统光标（避免硬件路径再飞到左上角）。
    int softX = 0, softY = 0;
    if (GetLastSoftMouseClientPos(TargetHwnd(), softX, softY) && (softX != 0 || softY != 0)) {
        cx = softX;
        cy = softY;
        return true;
    }
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    // 硬件绝对光标 / 窗口相对：必须映射到目标客户区（勿把屏幕坐标当客户区）。
    if (PreferHardwareInput() || UsesClientCoords()
        || session_.Config().windowRelativeCoordinates) {
        HWND hwnd = TargetHwnd();
        if (!hwnd || !IsWindow(hwnd)) return false;
        return ScreenToClientPoint(hwnd, pt.x, pt.y, cx, cy);
    }
    cx = pt.x;
    cy = pt.y;
    return true;
}

}  // namespace windowmode
