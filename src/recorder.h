#pragma once
// ──────────────────────────────────────────────────────────────────
// recorder.h — 鼠标/键盘录制基础设施
// 使用全局低级钩子捕获键盘和鼠标事件，存储为可回放事件序列
// 光标隐藏 / ClipCursor 裁剪时：另用 Raw Input 录制相对位移（FPS 视角）
// 所有对 g_recordedEvents 的访问必须持有 g_recordMutex 锁
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

enum class RecordingCaptureMode : int {
    Auto = 0,
    Absolute = 1,
    Relative = 2,
};

enum class RecordedEventSource : uint8_t {
    Synthetic = 0,
    LowLevelHook = 1,
    RawInput = 2,
};

/// 自定义相对移动事件（非系统消息）；x/y 存 dx/dy
constexpr UINT kWmRecordedRelativeMove = 0xC100;

/// 录制事件数据结构 — 存储时间偏移、消息类型、键码和鼠标坐标
struct RecordedEvent {
    uint64_t timeOffsetUs = 0; // 相对录制起点的微秒（QPC），保证平滑回放时长
    uint64_t sequence = 0;     // 跨采集线程的全局顺序号；同时间戳时用于稳定排序
    UINT msg = 0;              // WM_KEYDOWN/… 或 kWmRecordedRelativeMove
    WPARAM vkOrButton = 0;     // 虚拟键码或鼠标按键标识
    int x = 0, y = 0;          // 绝对坐标；相对移动时为 dx/dy
    int wheelDelta = 0;        // 滚轮增量（正上/右，负下/左）
    RecordedEventSource source = RecordedEventSource::Synthetic;
    std::wstring capturePath;  // BUTTON DOWN 异步截图完成后回填
    int captureOffsetX = 0;    // 边缘裁剪补偿 → FindImage.offset
    int captureOffsetY = 0;
};

/// 窗口相对录制目标：开启后鼠标事件坐标按目标窗口客户区记录（跟随窗口回放）。
struct RecordingWindowTarget {
    bool enabled = false;
    HWND hwnd = nullptr;
    int clientW = 0;
    int clientH = 0;
    std::wstring exePath;
    std::wstring windowTitle;
    std::wstring windowClassName;
    std::wstring childWindowClassName;
};

// ── 全局录制状态 ──────────────────────────────────────────────────

extern std::vector<RecordedEvent> g_recordedEvents;
extern std::mutex g_recordMutex;
extern std::atomic_bool g_recording;
extern HHOOK g_keyboardHook;
extern HHOOK g_mouseHook;

/// 绝对光标移动最小采样间隔（微秒）— 约 1kHz 上限，保留轨迹细节
constexpr uint64_t kAbsoluteMoveSampleUs = 1000;
/// 相对位移：每个 Raw Input 包立即落盘（不再合并），此常量仅作兼容保留
constexpr uint64_t kRelativeMoveSampleUs = 0;

/// 重置录制时钟（QPC）；StartRecording 时调用
void InitRecordingClock();

/// 当前相对录制起点的微秒偏移
uint64_t RecordingOffsetUs();

/// 当前是否处于「鼠标视角锁定」：光标不可见，或 ClipCursor 裁剪到非整屏
bool IsRelativeMouseCaptureActive();

/// 设置录制通道。Auto 会按光标隐藏/ClipCursor 动态切换。
void SetRecordingCaptureMode(RecordingCaptureMode mode);
RecordingCaptureMode GetRecordingCaptureMode();

/// 设置窗口相对录制目标（窗口模式录制）；enabled=false 恢复屏幕绝对录制。
void SetRecordingWindowTarget(const RecordingWindowTarget& target);
/// 当前窗口相对录制目标（快照拷贝）。
RecordingWindowTarget GetRecordingWindowTarget();
/// 窗口相对录制开启且目标点落在目标窗口内：屏幕坐标 → 客户区坐标，返回 true。
bool MapRecordingPointToClientIfWindowRelative(int& x, int& y);

/// 设置录制时需忽略的启停热键（全局/脚本/录制），避免误录入
struct RecordingIgnoreChord {
    UINT modifiers = 0;
    UINT vk = 0;
};
/// 图片定位模式：相对采集阶段跳过点击截图（找图转换仅对绝对事件有意义）
void SetRecordingClickCaptureSkipRelative(bool skip);
void SetRecordingIgnoreHotkeys(const RecordingIgnoreChord* items, int count);
/// 兼容：单热键忽略（enabled=false 或 vk=0 清空）
void SetRecordingIgnoreHotkey(UINT modifiers, UINT vk, bool enabled);

/// 录制调试输出（钩子线程可调；空 sink 关闭）。主窗在启停录制时接入宏调试窗。
using RecordingDebugSink = std::function<void(const std::wstring&)>;
void SetRecordingDebugSink(RecordingDebugSink sink);
void ClearRecordingDebugStats();
struct RecordingDebugStats {
    uint64_t keyDown = 0;
    uint64_t keyUp = 0;
    uint64_t keyRepeatSkipped = 0;
    uint64_t mouseDown = 0;
    uint64_t mouseUp = 0;
    uint64_t wheel = 0;
    uint64_t absMove = 0;
    uint64_t relMove = 0;
};
RecordingDebugStats GetRecordingDebugStats();

/// 录制点击自动截模板：StartRecording 注入；钩子内在 CallNextHookEx 前截像素，worker 只写盘
void SetRecordingClickCaptureConfig(bool enabled, int halfSize);
/// Stop 时在 Convert 前调用；超时后仍返回（缺 path 视为无模板）
void FlushClickCaptures(DWORD timeoutMs = 3000);
/// 异常停录：丢弃队列，不 Convert
void DiscardClickCaptures();
uint64_t GetRecordingClickCaptureSessionId();

/// 提升系统定时器精度（回放/录制短等待）；成对调用 EndHighResTimer
void BeginHighResTimer();
void EndHighResTimer();

// ── 钩子回调函数 ──────────────────────────────────────────────────

LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wp, LPARAM lp);
LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp);

/// 安装钩子并等待 Raw Input sink 注册完成；false 表示相对录制不可用。
bool InstallRecordingHooks();
void UninstallRecordingHooks();

/// 录制捕获范围：0=当前窗口过滤，1=全局（与 AppSettings.home.recorderCaptureScope 一致）
void SetRecordingCaptureScope(int scope);
int GetRecordingCaptureScope();
/// StartRecording：按 scope 锁定过滤根窗；ownProcessId 用于避免把自己锁成目标。
void BeginRecordingScopeSession(HWND preferredForeground, DWORD ownProcessId);
void EndRecordingScopeSession();

/// 纯逻辑（selftest / 钩子共用）：scope 0 时仅接受目标树；root 空且 armOnExternal 时用 candidate 上锁（candidate 不可为 own）。
struct RecordingScopeEval {
    bool accept = false;
    HWND newRoot = nullptr; // 非空表示应写入 session root
};
inline RecordingScopeEval EvaluateRecordingScopeFilter(
    int scope,
    HWND currentRoot,
    bool armOnExternal,
    HWND candidateRoot,
    bool candidateIsOwnProcess) {
    RecordingScopeEval ev{};
    if (scope != 0) {
        ev.accept = true;
        return ev;
    }
    if (currentRoot) {
        ev.accept = candidateRoot && candidateRoot == currentRoot;
        return ev;
    }
    if (!armOnExternal) {
        ev.accept = false;
        return ev;
    }
    if (!candidateRoot || candidateIsOwnProcess) {
        ev.accept = false;
        return ev;
    }
    ev.accept = true;
    ev.newRoot = candidateRoot;
    return ev;
}

/// 钩子内：键盘用前台窗；鼠标用命中窗。scope=全局恒 true。
bool RecordingScopeAllowsEvent(bool isKeyboard, int screenX, int screenY);
