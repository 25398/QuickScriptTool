#include "window_mode_json.h"

#include "utils.h"

namespace windowmode {

namespace {

bool JsonHasKey(const std::wstring& src, const std::wstring& key) {
    return src.find(L"\"" + key + L"\"") != std::wstring::npos;
}

bool ParseBool01(const std::wstring& src, const std::wstring& key, bool fallback) {
    const double v = ExtractNumber(src, key, fallback ? 1.0 : 0.0);
    return v != 0.0;
}

std::wstring CoordSpaceToJson(WindowModeCoordinateSpace space) {
    return space == WindowModeCoordinateSpace::ScreenAbsolute ? L"screenAbsolute" : L"windowClient";
}

WindowModeCoordinateSpace CoordSpaceFromJson(const std::wstring& text) {
    if (text == L"screenAbsolute") return WindowModeCoordinateSpace::ScreenAbsolute;
    return WindowModeCoordinateSpace::WindowClient;
}

std::wstring ExecutionKindToJson(WindowModeExecutionKind kind) {
    return kind == WindowModeExecutionKind::BackgroundWindow ? L"backgroundWindow" : L"hiddenDesktop";
}

WindowModeExecutionKind ExecutionKindFromJson(const std::wstring& text) {
    if (text == L"backgroundWindow") return WindowModeExecutionKind::BackgroundWindow;
    return WindowModeExecutionKind::HiddenDesktop;
}

std::wstring SelectMethodToJson(WindowSelectMethod method) {
    switch (method) {
    case WindowSelectMethod::MousePositionOnStartup: return L"mousePositionOnStartup";
    case WindowSelectMethod::UseEditorWindowClass: return L"useEditorWindowClass";
    case WindowSelectMethod::NoSelect: return L"noSelect";
    default: return L"selectOnStartup";
    }
}

}  // namespace

WindowModeScriptConfig DefaultWindowModeConfig() {
    return {};
}

WindowModeScriptConfig ParseWindowModeConfigObject(const std::wstring& block) {
    WindowModeScriptConfig cfg;
    if (block.empty()) return cfg;

    const bool enabledKeyPresent = JsonHasKey(block, L"enabled");
    cfg.enabled = ParseBool01(block, L"enabled", false);
    cfg.executionKind = ExecutionKindFromJson(ExtractString(block, L"executionKind"));
    cfg.targetExePath = ExtractString(block, L"targetExePath");
    cfg.targetWindowTitle = ExtractString(block, L"targetWindowTitle");
    cfg.coordSpace = CoordSpaceFromJson(ExtractString(block, L"coordSpace"));
    cfg.autoLaunchTarget = ParseBool01(block, L"autoLaunchTarget", false);
    cfg.launchArgs = ExtractString(block, L"launchArgs");

    const std::wstring selectMethod = ExtractString(block, L"selectMethod");
    if (selectMethod == L"mousePositionOnStartup") {
        cfg.selectMethod = WindowSelectMethod::MousePositionOnStartup;
    } else if (selectMethod == L"useEditorWindowClass") {
        cfg.selectMethod = WindowSelectMethod::UseEditorWindowClass;
    } else if (selectMethod == L"noSelect") {
        cfg.selectMethod = WindowSelectMethod::NoSelect;
    } else {
        cfg.selectMethod = WindowSelectMethod::SelectOnStartup;
    }
    cfg.windowName = ExtractString(block, L"windowName");
    cfg.windowClassName = ExtractString(block, L"windowClassName");
    cfg.childWindowClassName = ExtractString(block, L"childWindowClassName");
    cfg.useTopLevelWindow = ParseBool01(block, L"useTopLevelWindow", true);
    cfg.targetPickX = static_cast<int>(ExtractNumber(block, L"targetPickX", 0.0));
    cfg.targetPickY = static_cast<int>(ExtractNumber(block, L"targetPickY", 0.0));
    cfg.allowForegroundInputFallback = ParseBool01(block, L"allowForegroundInputFallback", false);
    if (cfg.windowName.empty() && !cfg.targetWindowTitle.empty()) {
        cfg.windowName = cfg.targetWindowTitle;
    }
    // 仅当旧脚本缺少 enabled 字段时，才从 executionKind / 路径推断是否启用
    if (!enabledKeyPresent && !cfg.enabled) {
        if (cfg.executionKind == WindowModeExecutionKind::BackgroundWindow) {
            cfg.enabled = true;
        } else if (cfg.executionKind == WindowModeExecutionKind::HiddenDesktop
            && !cfg.targetExePath.empty()) {
            cfg.enabled = true;
        }
    }
    return cfg;
}

std::wstring WindowModeConfigSummary(const WindowModeScriptConfig& cfg) {
    if (!cfg.enabled) return L"默认模式";
    if (cfg.executionKind == WindowModeExecutionKind::BackgroundWindow) return L"后台窗口模式";
    return L"窗口模式";
}

WindowModeScriptConfig ParseWindowModeJson(const std::wstring& content) {
    WindowModeScriptConfig cfg;
    if (content.empty()) return cfg;

    const auto pos = content.find(L"\"windowMode\"");
    if (pos == std::wstring::npos) return cfg;

    const auto brace = content.find(L'{', pos);
    if (brace == std::wstring::npos) return cfg;
    int depth = 0;
    std::wstring block;
    for (size_t i = brace; i < content.size(); ++i) {
        if (content[i] == L'{') ++depth;
        else if (content[i] == L'}') {
            --depth;
            block = content.substr(brace, i - brace + 1);
            if (depth == 0) break;
        }
    }
    if (block.empty()) return cfg;

    return ParseWindowModeConfigObject(block);
}

void WriteWindowModeJson(std::wstring& out, const WindowModeScriptConfig& cfg, bool trailingComma) {
    out += L"  \"windowMode\": {\n";
    out += L"    \"enabled\": " + std::to_wstring(cfg.enabled ? 1 : 0) + L",\n";
    out += L"    \"executionKind\": \"" + ExecutionKindToJson(cfg.executionKind) + L"\",\n";
    out += L"    \"targetExePath\": \"" + EscapeJson(cfg.targetExePath) + L"\",\n";
    out += L"    \"targetWindowTitle\": \"" + EscapeJson(cfg.targetWindowTitle) + L"\",\n";
    out += L"    \"coordSpace\": \"" + CoordSpaceToJson(cfg.coordSpace) + L"\",\n";
    out += L"    \"autoLaunchTarget\": " + std::to_wstring(cfg.autoLaunchTarget ? 1 : 0) + L",\n";
    out += L"    \"launchArgs\": \"" + EscapeJson(cfg.launchArgs) + L"\",\n";
    out += L"    \"selectMethod\": \"" + SelectMethodToJson(cfg.selectMethod) + L"\",\n";
    out += L"    \"windowName\": \"" + EscapeJson(cfg.windowName) + L"\",\n";
    out += L"    \"windowClassName\": \"" + EscapeJson(cfg.windowClassName) + L"\",\n";
    out += L"    \"childWindowClassName\": \"" + EscapeJson(cfg.childWindowClassName) + L"\",\n";
    out += L"    \"useTopLevelWindow\": " + std::to_wstring(cfg.useTopLevelWindow ? 1 : 0) + L",\n";
    out += L"    \"targetPickX\": " + std::to_wstring(cfg.targetPickX) + L",\n";
    out += L"    \"targetPickY\": " + std::to_wstring(cfg.targetPickY) + L",\n";
    out += L"    \"allowForegroundInputFallback\": "
        + std::to_wstring(cfg.allowForegroundInputFallback ? 1 : 0) + L"\n";
    out += L"  }" + std::wstring(trailingComma ? L",\n" : L"\n");
}

}  // namespace windowmode
