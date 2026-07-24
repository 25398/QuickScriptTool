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
    cfg.fakeFocusEnabled = ParseBool01(block, L"fakeFocusEnabled", false);
    const std::wstring strategy = ExtractString(block, L"inputStrategy");
    if (strategy == L"softMessage" || strategy == L"postMessage") {
        cfg.inputStrategy = WindowModeInputStrategy::SoftMessage;
    } else if (strategy == L"cdp") {
        cfg.inputStrategy = WindowModeInputStrategy::Cdp;
    } else {
        cfg.inputStrategy = WindowModeInputStrategy::Auto;
    }
    cfg.cdpPort = static_cast<int>(ExtractNumber(block, L"cdpPort", 9222.0));
    if (cfg.cdpPort <= 0) cfg.cdpPort = 9222;
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
    // 关闭窗口模式时清掉目标身份，避免残留误绑。
    if (enabledKeyPresent && !cfg.enabled) {
        cfg.targetExePath.clear();
        cfg.targetWindowTitle.clear();
        cfg.windowName.clear();
        cfg.windowClassName.clear();
        cfg.childWindowClassName.clear();
        cfg.launchArgs.clear();
        cfg.fakeFocusEnabled = false;
        cfg.inputStrategy = WindowModeInputStrategy::Auto;
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
    WindowModeScriptConfig w = cfg;
    if (!w.enabled) {
        w.targetExePath.clear();
        w.targetWindowTitle.clear();
        w.windowName.clear();
        w.windowClassName.clear();
        w.childWindowClassName.clear();
        w.launchArgs.clear();
        w.fakeFocusEnabled = false;
        w.inputStrategy = WindowModeInputStrategy::Auto;
    }
    out += L"  \"windowMode\": {\n";
    out += L"    \"enabled\": " + std::to_wstring(w.enabled ? 1 : 0) + L",\n";
    out += L"    \"executionKind\": \"" + ExecutionKindToJson(w.executionKind) + L"\",\n";
    out += L"    \"targetExePath\": \"" + EscapeJson(w.targetExePath) + L"\",\n";
    out += L"    \"targetWindowTitle\": \"" + EscapeJson(w.targetWindowTitle) + L"\",\n";
    out += L"    \"coordSpace\": \"" + CoordSpaceToJson(w.coordSpace) + L"\",\n";
    out += L"    \"autoLaunchTarget\": " + std::to_wstring(w.autoLaunchTarget ? 1 : 0) + L",\n";
    out += L"    \"launchArgs\": \"" + EscapeJson(w.launchArgs) + L"\",\n";
    out += L"    \"selectMethod\": \"" + SelectMethodToJson(w.selectMethod) + L"\",\n";
    out += L"    \"windowName\": \"" + EscapeJson(w.windowName) + L"\",\n";
    out += L"    \"windowClassName\": \"" + EscapeJson(w.windowClassName) + L"\",\n";
    out += L"    \"childWindowClassName\": \"" + EscapeJson(w.childWindowClassName) + L"\",\n";
    out += L"    \"useTopLevelWindow\": " + std::to_wstring(w.useTopLevelWindow ? 1 : 0) + L",\n";
    out += L"    \"targetPickX\": " + std::to_wstring(w.targetPickX) + L",\n";
    out += L"    \"targetPickY\": " + std::to_wstring(w.targetPickY) + L",\n";
    out += L"    \"allowForegroundInputFallback\": "
        + std::to_wstring(w.allowForegroundInputFallback ? 1 : 0) + L",\n";
    out += L"    \"fakeFocusEnabled\": " + std::to_wstring(w.fakeFocusEnabled ? 1 : 0) + L",\n";
    const wchar_t* strategy = L"auto";
    if (w.inputStrategy == WindowModeInputStrategy::SoftMessage) strategy = L"softMessage";
    else if (w.inputStrategy == WindowModeInputStrategy::Cdp) strategy = L"cdp";
    out += L"    \"inputStrategy\": \"" + std::wstring(strategy) + L"\",\n";
    out += L"    \"cdpPort\": " + std::to_wstring(w.cdpPort > 0 ? w.cdpPort : 9222) + L"\n";
    out += L"  }" + std::wstring(trailingComma ? L",\n" : L"\n");
}

}  // namespace windowmode
