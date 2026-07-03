#pragma once
// ──────────────────────────────────────────────────────────────────
// recorder.h — 鼠标/键盘录制基础设施
// 使用全局低级钩子捕获键盘和鼠标事件，存储为可回放事件序列
// 所有对 g_recordedEvents 的访问必须持有 g_recordMutex 锁
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

/// 录制事件数据结构 — 存储时间偏移、消息类型、键码和鼠标坐标
struct RecordedEvent {
    DWORD timeOffsetMs;     // 相对于录制开始时间的毫秒偏移
    UINT msg;               // WM_KEYDOWN/WM_KEYUP/WM_LBUTTONDOWN/等
    WPARAM vkOrButton;      // 虚拟键码或鼠标按键标识
    int x, y;               // 鼠标坐标（键盘事件时为0）
    int wheelDelta = 0;     // 滚轮增量（WM_MOUSEWHEEL/WM_MOUSEHWHEEL时使用，正上/右，负下/左）
};

// ── 全局录制状态 ──────────────────────────────────────────────────

extern std::vector<RecordedEvent> g_recordedEvents;
extern std::mutex g_recordMutex;  // 保护 g_recordedEvents 跨线程访问
extern std::atomic_bool g_recording;
extern std::atomic<DWORD> g_recordStartTick;
extern HHOOK g_keyboardHook;
extern HHOOK g_mouseHook;
extern std::atomic<DWORD> g_lastMouseMoveTick;

constexpr DWORD kMouseMoveSampleMs = 40; // 鼠标移动采样间隔(ms)

/// 设置录制时需忽略的全局停止热键（避免终止键被录入脚本）
void SetRecordingIgnoreHotkey(UINT modifiers, UINT vk, bool enabled);

// ── 钩子回调函数 ──────────────────────────────────────────────────

/// 键盘低级钩子回调 — 捕获按键按下/释放事件
LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wp, LPARAM lp);

/// 鼠标低级钩子回调 — 捕获鼠标移动/按键事件
LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp);

// ── 钩子管理 ──────────────────────────────────────────────────────

/// 安装全局键盘和鼠标钩子（重复调用安全）
void InstallRecordingHooks();

/// 卸载全局键盘和鼠标钩子
void UninstallRecordingHooks();
