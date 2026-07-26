#pragma once
// synthetic_input_filter.h — 驱动级注入与真人输入的区分（仅本进程内存，不改 HID 包）
// 1) 热键：只记「已注册启停热键」对应的键/鼠标按钮指纹（脱离=0 时）
// 2) 脱离（breakout>0）：记全部键/按钮 + 移动/滚轮；VirtualHid 另用 Raw 设备表 + 指纹兜底

#include <windows.h>

namespace synthetic_input {

/// VirtualHid VHF：VendorID=0x5153 ProductID=0x5648（"QS"/"VH"）
inline constexpr wchar_t kVirtualHidVidPidToken[] = L"VID_5153&PID_5648";

void BeginSession();
void EndSession();

/// 仅当脚本脱离时间>0 时打开：记全部键/按钮 + 移动/滚轮，供脱离判定。
void SetBreakoutTracking(bool enabled);
bool BreakoutTrackingEnabled();

/// 热键指纹只记这些 VK（全局/脚本启停热键）。脱离>0 时仍记全部键/按钮。
void SetWatchedHotkeyVks(const UINT* vks, int count);

/// 会话收尾（ReleaseAll 等）前调用：短时内 LL 事件一律视为注入，避免抬起误触热键/脱离。
void NoteSessionTeardown();

void NoteKey(UINT vk, unsigned short scanCode, bool extended, bool down);
void NoteMouseButton(UINT buttonVk, bool down);
void NoteMouseWheel();
void NoteMouseMove();

void NoteVirtualHidRawKeyboard();
void NoteVirtualHidRawMouseButton();
void NoteVirtualHidRawMouseWheel();
void NoteVirtualHidRawMouseMove();

bool MatchesKey(UINT vk, unsigned short scanCode, bool extended, bool down);
bool MatchesMouseButton(UINT buttonVk, bool down);
bool MatchesMouseWheel();
bool MatchesMouseMove();

void RefreshVirtualHidRawDevices();
bool IsVirtualHidRawDevice(HANDLE device);

bool RegisterRawInputSink(HWND hwnd);
void UnregisterRawInputSink(HWND hwnd);

struct RawBreakoutHint {
    bool fromOurDevice = false;
    bool userBreakout = false;
    UINT msg = 0;
    UINT vk = 0;
};

/// 处理 WM_INPUT。monitorUserBreakout 为 true 时填充 hint（调用方再排除启停热键）。
RawBreakoutHint OnRawInput(HRAWINPUT hRawInput, bool monitorUserBreakout);

}  // namespace synthetic_input
