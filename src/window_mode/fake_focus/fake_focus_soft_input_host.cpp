#include "fake_focus_soft_input_host.h"
#include "fake_focus_soft_input.h"

#include <cstring>

namespace windowmode {
namespace {

HANDLE g_mapping = nullptr;
fakefocus::SoftInputState* g_view = nullptr;
DWORD g_pid = 0;

void TouchSeq() {
    if (g_view) ++g_view->seq;
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

    const DWORD bytes = static_cast<DWORD>(sizeof(fakefocus::SoftInputState));
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, name);
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
    TouchSeq();
}

void FakeFocusSoftInput_SetMouseButtonVk(UINT vk, bool down) {
    if (!g_view || vk >= 256) return;
    g_view->down[vk] = down ? 0x80 : 0;
    g_view->flags |= fakefocus::kSoftFlagKeysValid;
    TouchSeq();
}

void FakeFocusSoftInput_SetKey(UINT vk, bool down) {
    if (!g_view || vk == 0 || vk >= 256) return;
    g_view->down[vk] = down ? 0x80 : 0;
    g_view->flags |= fakefocus::kSoftFlagKeysValid;
    TouchSeq();
}

void FakeFocusSoftInput_ClearKeys() {
    if (!g_view) return;
    std::memset(g_view->down, 0, sizeof(g_view->down));
    g_view->flags &= ~fakefocus::kSoftFlagKeysValid;
    TouchSeq();
}

void FakeFocusSoftInput_Reset() {
    if (!g_view) return;
    const uint32_t magic = g_view->magic;
    const uint32_t version = g_view->version;
    std::memset(g_view, 0, sizeof(*g_view));
    g_view->magic = magic;
    g_view->version = version;
}

}  // namespace windowmode
