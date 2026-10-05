#include "fake_focus_soft_input_host.h"
#include "fake_focus_soft_input.h"

#include "window_mode/mouse_wheel_events.h"
#include "window_mode/time_scale_clock.h"

#include <cstring>
#include <sddl.h>

namespace windowmode {
namespace {

HANDLE g_mapping = nullptr;
fakefocus::SoftInputState* g_view = nullptr;
DWORD g_pid = 0;

void TouchSeq() {
    if (g_view) ++g_view->seq;
}

void PushKeyEvent(UINT vk, bool down, uint16_t pad = 0) {
    if (!g_view || vk == 0 || vk >= 256) return;
    const uint32_t w = g_view->keyWrite;
    fakefocus::SoftKeyEvent& slot =
        g_view->keyEvents[w % fakefocus::kSoftKeyEventCap];
    slot.vk = static_cast<uint8_t>(vk);
    slot.down = down ? 1 : 0;
    slot.pad = pad;
    MemoryBarrier();
    g_view->keyWrite = w + 1;
}

}  // namespace

bool FakeFocusSoftInput_Attach(DWORD targetPid, std::wstring& err) {
    FakeFocusSoftInput_Detach();
    err.clear();
    if (targetPid == 0) {
        err = L"假焦点软输入：目标 PID 无效";
        return false;
    }

    wchar_t name[128]{};
    fakefocus::SoftInputMappingName(targetPid, name, 128);

    // 宿主可能是管理员、游戏是中完整性：允许同用户 Interactive + Medium IL，禁止 Everyone。
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)S:(ML;;NW;;;ME)",
            SDDL_REVISION_1, &sd, nullptr)) {
        err = L"假焦点软输入：构造安全描述符失败";
        return false;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    const DWORD bytes = static_cast<DWORD>(sizeof(fakefocus::SoftInputState));
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, bytes, name);
    LocalFree(sd);
    if (!g_mapping) {
        err = L"CreateFileMapping(假焦点软输入) 失败";
        return false;
    }

    g_view = static_cast<fakefocus::SoftInputState*>(
        MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
    if (!g_view) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        err = L"MapViewOfFile(假焦点软输入) 失败";
        return false;
    }

    std::memset(g_view, 0, sizeof(*g_view));
    g_view->magic = fakefocus::kSoftInputMagic;
    g_view->version = fakefocus::kSoftInputVersion;
    // 立刻视为键态有效（全抬起），避免目标高频 GetAsyncKeyState 在 flags 未置位时走原函数拆补丁。
    g_view->flags = fakefocus::kSoftFlagKeysValid;
    g_pid = targetPid;
    return true;
}

void FakeFocusSoftInput_Detach() {
    if (g_view) {
        UnmapViewOfFile(g_view);
        g_view = nullptr;
    }
    if (g_mapping) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
    g_pid = 0;
}

uint32_t FakeFocusSoftInput_UnityState() {
    if (!g_view) return 0;
    return g_view->unityState;
}

bool FakeFocusSoftInput_TimeHookCalls(uint32_t* out, int count) {
    if (!g_view || !out || count <= 0) return false;
    const int n = count > 6 ? 6 : count;
    for (int i = 0; i < n; ++i) out[i] = g_view->timeHookCalls[i];
    return true;
}

bool FakeFocusSoftInput_IsAttached() {
    return g_view != nullptr;
}

void FakeFocusSoftInput_SetCursorScreen(int sx, int sy) {
    if (!g_view) return;
    g_view->cursorScreenX = sx;
    g_view->cursorScreenY = sy;
    g_view->flags |= fakefocus::kSoftFlagCursorValid | fakefocus::kSoftFlagKeysValid;
    if (g_view->flags & fakefocus::kSoftFlagPostKeyEvents) {
        ++g_view->mouseMoveWrite;
    }
    TouchSeq();
}

void FakeFocusSoftInput_SetMouseButtonVk(UINT vk, bool down) {
    if (!g_view || vk >= 256) return;
    g_view->down[vk] = down ? 0x80 : 0;
    g_view->flags |= fakefocus::kSoftFlagKeysValid;
    PushKeyEvent(vk, down);
    TouchSeq();
}

void FakeFocusSoftInput_SetKey(UINT vk, bool down) {
    if (!g_view || vk == 0 || vk >= 256) return;
    g_view->down[vk] = down ? 0x80 : 0;
    auto sync = [&](UINT left, UINT right, UINT generic) {
        if (vk == left || vk == right) {
            g_view->down[generic] = (g_view->down[left] || g_view->down[right]) ? 0x80 : 0;
        } else if (vk == generic && !down) {
            g_view->down[left] = 0;
            g_view->down[right] = 0;
            g_view->down[generic] = 0;
        }
    };
    sync(VK_LSHIFT, VK_RSHIFT, VK_SHIFT);
    sync(VK_LCONTROL, VK_RCONTROL, VK_CONTROL);
    sync(VK_LMENU, VK_RMENU, VK_MENU);
    MemoryBarrier();
    g_view->flags |= fakefocus::kSoftFlagKeysValid;
    PushKeyEvent(vk, down);
    TouchSeq();
}

void FakeFocusSoftInput_SetPostKeyEvents(bool enabled) {
    if (!g_view) return;
    if (enabled) {
        g_view->flags |= fakefocus::kSoftFlagPostKeyEvents;
    } else {
        g_view->flags &= ~fakefocus::kSoftFlagPostKeyEvents;
    }
    TouchSeq();
}

bool FakeFocusSoftInput_WheelCursors(uint32_t& write, uint32_t& read) {
    if (!g_view) return false;
    write = g_view->wheelWrite;
    read = g_view->wheelRead;
    return true;
}

bool FakeFocusSoftInput_PostKeyEventsEnabled() {
    return g_view != nullptr
        && (g_view->flags & fakefocus::kSoftFlagPostKeyEvents) != 0;
}

void FakeFocusSoftInput_PushWheel(bool vertical, bool positive, int steps) {
    if (!g_view) return;
    if (steps < 1) steps = 1;
    if (steps > kMaxWheelNotchesPerEvent) steps = kMaxWheelNotchesPerEvent;
    // ⚠⚠ 这里**不能**再查 `kSoftFlagPostKeyEvents`（原实现就是这么静默空转的：
    //   那个标志只给 Chromium/Qt 壳置位，而普通游戏走的是"目标进程内软输入"这条路，
    //   于是滚轮请求被直接丢掉、日志却还说投递成功）。
    //   滚轮无条件入队，由 DLL 按目标类型分流：Raw Input 直投（游戏）+
    //   进程内 PostMessage（消息驱动应用）。
    //
    // ⚠ 入队是**环形**的：宿主写快于 DLL 消费时会覆盖最老的一格（宁可丢老的一格，
    //   也不要让整个队列卡死）—— 与 keyEvents 同一取舍。
    const uint32_t w = g_view->wheelWrite;
    if (w - g_view->wheelRead >= fakefocus::kSoftKeyEventCap) {
        // 队列已满：推进读游标丢掉最老一格，给新的一格腾位置。
        g_view->wheelRead = w - fakefocus::kSoftKeyEventCap + 1;
    }
    const int slot = static_cast<int>(w % fakefocus::kSoftKeyEventCap);
    g_view->wheelEvents[slot].vk = vertical ? 0xFE : 0xFD;  // 与 keyEvents 的滚轮编码一致
    g_view->wheelEvents[slot].down = positive ? 1 : 0;
    g_view->wheelEvents[slot].pad = static_cast<uint16_t>(steps);
    MemoryBarrier();
    g_view->wheelWrite = w + 1;
    TouchSeq();
}

void FakeFocusSoftInput_ClearKeys() {
    if (!g_view) return;
    std::memset(g_view->down, 0, sizeof(g_view->down));
    // 保持 KeysValid：全抬起仍走软键态，避免目标进程回落原 GetAsyncKeyState 拆 inline 补丁。
    g_view->flags |= fakefocus::kSoftFlagKeysValid;
    TouchSeq();
}

void FakeFocusSoftInput_Reset() {
    if (!g_view) return;
    const uint32_t magic = g_view->magic;
    const uint32_t version = g_view->version;
    std::memset(g_view, 0, sizeof(*g_view));
    g_view->magic = magic;
    g_view->version = version;
    g_view->flags = fakefocus::kSoftFlagKeysValid;
}

bool FakeFocusSoftInput_SetTimeScale(double speed) {
    if (!g_view) return false;
    uint32_t milli = 0;
    if (speed > 0.0) {
        milli = fakefocus::TimeScaleFromDouble(speed);
        if (fakefocus::TimeScaleIsIdentity(milli)) milli = fakefocus::kTimeScaleDen;
    }
    g_view->timeScaleMilli = milli;
    MemoryBarrier();
    TouchSeq();
    return true;
}

double FakeFocusSoftInput_TimeScale() {
    if (!g_view) return 0.0;
    const uint32_t milli = g_view->timeScaleMilli;
    if (milli == 0) return 0.0;
    return static_cast<double>(milli) / static_cast<double>(fakefocus::kTimeScaleDen);
}

bool FakeFocusSoftInput_ReadMapleHits(DWORD& gaks, DWORD& diState, DWORD& diData, DWORD& lastCb,
    DWORD& hitReady, DWORD& gfw, DWORD& focus) {
    gaks = 0;
    diState = 0;
    diData = 0;
    lastCb = 0;
    hitReady = 0;
    gfw = 0;
    focus = 0;
    if (!g_view || g_view->magic != fakefocus::kSoftInputMagic
        || g_view->version != fakefocus::kSoftInputVersion) {
        return false;
    }
    gaks = g_view->hitGaks;
    diState = g_view->hitDiState;
    diData = g_view->hitDiData;
    lastCb = g_view->lastDiStateCb;
    hitReady = g_view->hitReady;
    gfw = g_view->hitGfw;
    focus = g_view->hitFocus;
    return true;
}

/// 读宿主写进共享内存的**假光标**（DLL 的 `Hook_GetCursorPos` 返回的就是它）。
/// ★ 这是「宿主到底有没有把光标喂进去」的**唯一直接证据** —— 日志里那句
///   「假焦点已注入」只证明 DLL 装上了，证明不了光标在动；而
///   `SyncFakeFocusCursor()` 在 `FakeFocusSoftInput_IsAttached()` 为假时是**静默 return**。
/// @return false = 未挂共享内存（出参不动）
bool FakeFocusSoftInput_ReadSoftCursor(int& x, int& y, bool& cursorValid, bool& postKeyEvents) {
    if (!g_view || g_view->magic != fakefocus::kSoftInputMagic
        || g_view->version != fakefocus::kSoftInputVersion) {
        return false;
    }
    x = g_view->cursorScreenX;
    y = g_view->cursorScreenY;
    cursorValid = (g_view->flags & fakefocus::kSoftFlagCursorValid) != 0;
    postKeyEvents = (g_view->flags & fakefocus::kSoftFlagPostKeyEvents) != 0;
    return true;
}

/// 冒险岛输入路径体检（DLL 写、宿主读）：见 fake_focus_soft_input.h 的字段注释。
/// 与 `ReadMapleHits` 分开，免得动到既有的 7 个出参调用点。
bool FakeFocusSoftInput_ReadMaplePathProbe(DWORD& pump, DWORD& msgInput, DWORD& msgKey,
    DWORD& msgActivate, DWORD& rescanAdds, DWORD& rescanRounds) {
    pump = 0;
    msgInput = 0;
    msgKey = 0;
    msgActivate = 0;
    rescanAdds = 0;
    rescanRounds = 0;
    if (!g_view || g_view->magic != fakefocus::kSoftInputMagic
        || g_view->version != fakefocus::kSoftInputVersion) {
        return false;
    }
    pump = g_view->hitPump;
    msgInput = g_view->msgInput;
    msgKey = g_view->msgKey;
    msgActivate = g_view->msgActivate;
    rescanAdds = g_view->rescanAdds;
    rescanRounds = g_view->rescanRounds;
    return true;
}

/// 当前软键按下个数（宿主自己就能算：共享内存 down[] 非零即按下）。
/// 用途：证明「宿主确实在喂键」——否则「后台不动」会被误判成钩子问题。
int FakeFocusSoftInput_DownKeyCount() {
    if (!g_view) return -1;
    int n = 0;
    for (int i = 0; i < 256; ++i) {
        if (g_view->down[i]) ++n;
    }
    return n;
}

/// 单个键当前是否逻辑按下（共享内存 down[vk]）。
/// 自检要精确断言"是哪个键"——只看按下总数会把"清错了键又置回另一个"判成通过。
bool FakeFocusSoftInput_IsKeyDown(UINT vk) {
    if (!g_view || vk > 255) return false;
    return g_view->down[vk] != 0;
}

bool FakeFocusSoftInput_ReadMapleInstall(DWORD& diag, DWORD& iatPoll, DWORD& diVt) {
    diag = 0;
    iatPoll = 0;
    diVt = 0;
    if (!g_view || g_view->magic != fakefocus::kSoftInputMagic
        || g_view->version != fakefocus::kSoftInputVersion) {
        return false;
    }
    diag = g_view->mapleDiag;
    iatPoll = g_view->mapleIatPoll;
    diVt = g_view->mapleDiVt;
    return true;
}

}  // namespace windowmode
