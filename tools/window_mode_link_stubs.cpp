// Selftests that link script_action_builder_core (window_mode_types.obj) without
// window_mode_core need these symbols. Product builds resolve them via
// background_input_target / window_target / cdp_input.
#include "window_mode/background_input_target.h"
#include "window_mode/cdp/cdp_input.h"
#include "window_mode/window_target.h"

#include <string>

namespace windowmode {

bool AndroidEmulatorPrefersFakeFocus(HWND, const WindowModeScriptConfig*) {
    return false;
}

bool AndroidEmulatorPrefersFakeFocusFromConfig(const WindowModeScriptConfig&) {
    return false;
}

bool LooksLikeMonitorCoveringFullscreen(HWND) {
    return false;
}

std::wstring QueryHwndProcessImagePath(HWND) {
    return {};
}

/// 逻辑档里**不写**窗口模式日志。
///
/// 需要它的是 WebAiSelfTest：`web_ai_prompt.cpp`（纯逻辑那半边）会调
/// `WindowModeLogEventf` 留痕，而真身 `window_mode_log.cpp` 依赖
/// VirtualDesktopAccessor / window_target（整个 window_mode_core）——
/// 逻辑档不背这些依赖，也不该往用户日志文件里写东西（自检结果不能有副作用）。
void WindowModeLogEventf(const wchar_t* /*fmt*/, ...) {
}

}  // namespace windowmode
