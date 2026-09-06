#pragma once
// synthetic_input_filter.h — 驱动级注入与真人输入的区分（仅本进程内存，不改 HID 包）
// 1) 热键：只记「已注册启停热键」对应的键/鼠标按钮指纹（脱离=0 时）
// 2) 脱离（breakout>0）：记全部键/按钮 + 移动/滚轮；VirtualHid 另用 Raw 设备表 + 指纹兜底

#include <windows.h>

namespace synthetic_input {

/// VirtualHid VHF：VendorID=0x5153 ProductID=0x5648（"QS"/"VH"）
inline constexpr wchar_t kVirtualHidVidPidToken[] = L"VID_5153&PID_5648";

/// 注入键鼠 ExtraInformation / dwExtraInfo 标记（Interception information 同源）。
/// 长按热键与脚本同键时：Raw/LL 凭此忽略脚本抬起，只认真人松手。
inline constexpr ULONG_PTR kSyntheticExtraInfo = static_cast<ULONG_PTR>(0x51535448u); // 'QSTH'

inline bool IsSyntheticExtraInfo(ULONG_PTR info) {
    return info == kSyntheticExtraInfo;
}

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

/// VirtualHid 无 INJECTED/ExtraInfo：注入前后 Expect，LL Consume；未消费完的抬起视为真人松手。
void ExpectSyntheticKeyUp(UINT vk);
bool ConsumeSyntheticKeyUp(UINT vk);
void ExpectSyntheticKeyDown(UINT vk);
bool ConsumeSyntheticKeyDown(UINT vk);

void NoteVirtualHidRawKeyboard();
void NoteVirtualHidRawKeyboardEvent(UINT vk, unsigned short scan, bool extended, bool down);
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
    /// 真人键盘按下/抬起（非 VirtualHid / 非注入 ExtraInfo）。长按启停用抬起停。
    bool physicalKeyDown = false;
    bool physicalKeyUp = false;
    bool physicalButtonUp = false;
    UINT msg = 0;
    UINT vk = 0;
    HANDLE device = nullptr;
};

/// 处理 WM_INPUT。monitorUserBreakout 为 true 时填充脱离 hint（调用方再排除启停热键）。
/// 无论是否脱离监控，都会：① 给自家 VHID 键盘补指纹 ② 报告真人 KEYUP。
RawBreakoutHint OnRawInput(HRAWINPUT hRawInput, bool monitorUserBreakout);

}  // namespace synthetic_input
