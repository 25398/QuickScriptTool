// ──────────────────────────────────────────────────────────────────
// main_features.h — 功能模块抽象接口
// 定义连点击 (Clicker) 和录制 (Recorder) 功能的接口与设置结构体。
// MainWindow 实现 IClickerFeature 和 IRecorderFeature 接口。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <string>

namespace quickscript {

// 主界面标签页索引
enum class MainTab {
    Clicker,        // 连点击
    Recorder,       // 录制器
    Macro,          // 宏脚本
    ScriptCustom    // 自定义脚本
};

// 鼠标按键选择
enum class MouseButtonChoice {
    Left,    // 左键
    Middle,  // 中键
    Right    // 右键
};

// 连点击间隔模式
enum class ClickIntervalMode {
    Custom,     // 自定义间隔
    Efficient,  // 高效模式
    Extreme     // 极限模式
};

// 录制捕获范围
enum class RecordCaptureScope {
    Window,  // 仅当前窗口
    Global   // 全局捕获
};

enum class RecorderInputMode {
    Auto = 0,
    DesktopAbsolute = 1,
    FpsRelative = 2,
};

// ── 连点击设置 ───────────────────────────────────────────────────
struct ClickerSettings {
    MouseButtonChoice button = MouseButtonChoice::Left;          // 点击按键
    ClickIntervalMode intervalMode = ClickIntervalMode::Efficient;  // 间隔模式
    double customIntervalSeconds = 0.1;                         // 自定义间隔 (秒)
};

// ── 录制器设置 ───────────────────────────────────────────────────
struct RecorderSettings {
    RecordCaptureScope captureScope = RecordCaptureScope::Window;  // 捕获范围
    RecorderInputMode inputMode = RecorderInputMode::Auto;
};

// 连点击功能接口
class IClickerFeature {
public:
    virtual ~IClickerFeature() = default;
    virtual void ApplySettings(const ClickerSettings& settings) = 0;
    virtual void StartClicking() = 0;
    virtual void StopClicking() = 0;
};

// 录制器功能接口
class IRecorderFeature {
public:
    virtual ~IRecorderFeature() = default;
    virtual void ApplySettings(const RecorderSettings& settings) = 0;
    virtual void StartRecording() = 0;
    virtual void StopRecording() = 0;
};

} // namespace quickscript
