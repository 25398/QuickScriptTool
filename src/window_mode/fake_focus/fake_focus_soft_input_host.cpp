#include "fake_focus_soft_input_host.h"
#include "fake_focus_soft_input.h"

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

bool FakeFocusSoftInput_PostKeyEventsEnabled() {
    return g_view != nullptr
        && (g_view->flags & fakefocus::kSoftFlagPostKeyEvents) != 0;
}

void FakeFocusSoftInput_PushWheel(bool vertical, bool positive, int steps) {
    if (!g_view || !(g_view->flags & fakefocus::kSoftFlagPostKeyEvents)) return;
    if (steps < 1) steps = 1;
    if (steps > 64) steps = 64;
    // 0xFE=竖向滚轮 0xFD=横向；down=正向；pad=步进数。
    PushKeyEvent(vertical ? 0xFE : 0xFD, positive, static_cast<uint16_t>(steps));
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
