#pragma once

#include <string>

namespace quickscript {

enum class MainTab {
    Clicker,
    Recorder,
    Macro,
    ScriptCustom
};

enum class MouseButtonChoice {
    Left,
    Middle,
    Right
};

enum class ClickIntervalMode {
    Custom,
    Efficient,
    Extreme
};

enum class RecordCaptureScope {
    Window,
    Global
};

struct ClickerSettings {
    MouseButtonChoice button = MouseButtonChoice::Left;
    ClickIntervalMode intervalMode = ClickIntervalMode::Efficient;
    double customIntervalSeconds = 0.1;
};

struct RecorderSettings {
    RecordCaptureScope captureScope = RecordCaptureScope::Window;
};

class IClickerFeature {
public:
    virtual ~IClickerFeature() = default;
    virtual void ApplySettings(const ClickerSettings& settings) = 0;
    virtual void StartClicking() = 0;
    virtual void StopClicking() = 0;
};

class IRecorderFeature {
public:
    virtual ~IRecorderFeature() = default;
    virtual void ApplySettings(const RecorderSettings& settings) = 0;
    virtual void StartRecording() = 0;
    virtual void StopRecording() = 0;
};

} // namespace quickscript
