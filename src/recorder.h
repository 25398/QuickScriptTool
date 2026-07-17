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
#include <mutex>
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

/// 设置录制时需忽略的全局停止热键（避免终止键被录入脚本）
void SetRecordingIgnoreHotkey(UINT modifiers, UINT vk, bool enabled);

/// 提升系统定时器精度（回放/录制短等待）；成对调用 EndHighResTimer
void BeginHighResTimer();
void EndHighResTimer();

// ── 钩子回调函数 ──────────────────────────────────────────────────

LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wp, LPARAM lp);
LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp);

/// 安装钩子并等待 Raw Input sink 注册完成；false 表示相对录制不可用。
bool InstallRecordingHooks();
void UninstallRecordingHooks();
