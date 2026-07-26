#pragma once
// virtual_hid.h — QstVHid 用户态客户端（IOCTL → VhfReadReportSubmit）

#include "script_types.h"

#include <mutex>
#include <string>

class VirtualHidBackend {
public:
    static VirtualHidBackend& Instance();

    bool ProbeAvailable(std::wstring* errorOut = nullptr);
    bool Open(std::wstring* errorOut = nullptr);
    void Close();
    bool IsOpen() const;

    bool SendKey(unsigned short scanCode, bool down, bool extended);
    bool MoveRelative(int dx, int dy);
    bool MoveAbsoluteScreen(int screenX, int screenY);
    bool Button(MouseButtonType button, bool down);
    bool Wheel(int delta, bool horizontal);
    /// 全抬起：键槽/修饰清零 + 鼠标按钮清零，避免粘键。
    bool ReleaseAll();

    std::wstring LastError() const;

private:
    enum class PointerMode : int { Relative = 0, Absolute = 1 };

    VirtualHidBackend() = default;
    bool SubmitReportLocked(unsigned char reportId, const unsigned char* data, unsigned char len);
    bool SubmitKeyboardLocked();
    bool SubmitMouseRelLocked(unsigned char buttons, char dx, char dy, char wheel, char hwheel);
    bool SubmitMouseAbsLocked(unsigned char buttons, unsigned short x, unsigned short y);
    bool SubmitMouseButtonsLocked();
    void CompactKeysLocked();
    bool KeyboardStateChangedLocked() const;
    static unsigned char ScanToHidUsage(unsigned short scanCode, bool extended);
    static unsigned char ModifierMask(unsigned short scanCode, bool extended);

    mutable std::mutex mutex_;
    void* device_ = nullptr;  // HANDLE
    std::wstring lastError_;
    unsigned char modifiers_ = 0;
    /// Boot keyboard 6-key slots；抬起后左对齐压缩（贴近实体固件），满时挤掉最旧槽。
    unsigned char keys_[6]{};
    unsigned char lastSentModifiers_ = 0;
    unsigned char lastSentKeys_[6]{};
    unsigned char mouseButtons_ = 0;
    PointerMode pointerMode_ = PointerMode::Relative;
    unsigned short lastAbsX_ = 0;
    unsigned short lastAbsY_ = 0;
};
