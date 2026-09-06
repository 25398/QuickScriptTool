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

}  // namespace windowmode
