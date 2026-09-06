// engine_settings_reload.cpp — F2 slice extracted from engine_host_window.h
#include "engine/engine_host_window.h"
#include "webview/webview_bridge_backend.h"

// was engine_host_window.h:7495-7522
void EngineHost::LoadScripts() {
        scripts_.clear(); EnsureScriptsDir();
        std::vector<ScriptFileEntry> files;
        EnumerateScriptJsonFiles(ScriptsDir(), files);
        for (const auto& fe : files) {
            ScriptMeta meta{};
            meta.path = fe.path;
            meta.folder = fe.folder;
            const auto content = ReadAll(meta.path);
            meta.name = ExtractString(content, L"scriptName");
            if (meta.name.empty()) meta.name = fe.fileName;
            meta.name = StripJsonExtension(std::move(meta.name));
            if (meta.name.empty()) meta.name = L"未命名";
            meta.recordTime = ExtractString(content, L"recordTime");
            if (meta.recordTime.empty()) meta.recordTime = L"未知";
            meta.actionCount = CountActionsInJson(content);
            meta.hotkey.text = ExtractString(content, L"hotkeyText");
            meta.hotkey.vk = static_cast<UINT>(ExtractNumber(content, L"hotkeyVk", 0));
            meta.hotkey.modifiers = static_cast<UINT>(ExtractNumber(content, L"hotkeyModifiers", 0));
            meta.hotkey.holdMode = ExtractBool(content, L"hotkeyHold", false);
            meta.hotkey.enabled = meta.hotkey.vk != 0;
            scripts_.push_back(meta);
        }
        ClampHomeScroll();
    }

/// 脚本/录制热键若与全局启停完全相同：清掉并写回磁盘（与极简「新建不设专属热键」一致，避免抢占 F8）
void EngineHost::ClearScriptHotkeysCollidingWithGlobal() {
        if (!globalHotkey_.enabled || !globalHotkey_.vk) return;
        auto clearOne = [&](Hotkey& hk, const std::wstring& path) {
            if (!hk.enabled || !hk.vk) return;
            if (hk.vk != globalHotkey_.vk || hk.modifiers != globalHotkey_.modifiers) return;
            ScriptFileData data = LoadScriptFileData(path, false);
            data.hotkey.vk = 0;
            data.hotkey.modifiers = 0;
            data.hotkey.text.clear();
            data.hotkey.enabled = false;
            data.hotkey.holdMode = false;
            if (SaveScriptFileData(path, data)) {
                hk = data.hotkey;
            } else {
                hk.vk = 0;
                hk.modifiers = 0;
                hk.text.clear();
                hk.enabled = false;
                hk.holdMode = false;
            }
        };
        for (auto& s : scripts_) clearOne(s.hotkey, s.path);
        for (auto& r : recordings_) clearOne(r.hotkey, r.path);
    }

// was engine_host_window.h:10709-10720
void EngineHost::RefreshScriptLibraryUi() {
        std::wstring selScriptPath;
        std::wstring selRecordingPath;
        std::wstring curScriptPath;
        if (selectedScript_ >= 0 && selectedScript_ < static_cast<int>(scripts_.size()))
            selScriptPath = scripts_[static_cast<size_t>(selectedScript_)].path;
        if (selectedRecording_ >= 0 && selectedRecording_ < static_cast<int>(recordings_.size()))
            selRecordingPath = recordings_[static_cast<size_t>(selectedRecording_)].path;
        if (currentScriptIndex_ >= 0 && currentScriptIndex_ < static_cast<int>(scripts_.size()))
            curScriptPath = scripts_[static_cast<size_t>(currentScriptIndex_)].path;

        LoadScripts();
        LoadRecordings();
        ClearScriptHotkeysCollidingWithGlobal();
        scheduledTasks_.Reload();

        auto findByPath = [](const std::vector<ScriptMeta>& list, const std::wstring& path) -> int {
            if (path.empty()) return -1;
            for (int i = 0; i < static_cast<int>(list.size()); ++i) {
                if (_wcsicmp(list[static_cast<size_t>(i)].path.c_str(), path.c_str()) == 0)
                    return i;
            }
            return -1;
        };
        selectedScript_ = findByPath(scripts_, selScriptPath);
        selectedRecording_ = findByPath(recordings_, selRecordingPath);
        currentScriptIndex_ = findByPath(scripts_, curScriptPath);

        ClampHomeScroll();
        ClampRecordingScroll();
        RegisterAllHotkeys();
        if (IsWindow(hwnd_)) InvalidateRect(hwnd_, nullptr, TRUE);
    }

// was engine_host_window.h:9298-9344
void EngineHost::EngineSaveHomeState() {
    try { SaveHomeStateCore(true); } catch (...) {}
}

void EngineHost::EngineSaveHomeStateLite() {
    try {
        SaveHomeStateCore(false);
        ScheduleHomeStatePersist();
    } catch (...) {}
}

void EngineHost::ScheduleHomeStatePersist() {
    if (!hwnd_ || !IsWindow(hwnd_)) {
        try { PersistHomeStateJsonOnly(); } catch (...) {}
        return;
    }
    // 连点选中合并写盘，避免 UI/盘 I/O 抖死整机
    SetTimer(hwnd_, kHomeStatePersistTimerId, 800, nullptr);
}

void EngineHost::FlushHomeStatePersist() {
    if (hwnd_ && IsWindow(hwnd_)) KillTimer(hwnd_, kHomeStatePersistTimerId);
    try { PersistHomeStateJsonOnly(); } catch (...) {}
}

void EngineHost::PersistHomeStateJsonOnly() {
    SaveAppSettings(appSettings_);
}

void EngineHost::SaveHomeState() {
    SaveHomeStateCore(true);
}

void EngineHost::SaveHomeStateCore(bool persistJson) {
        auto& hs = appSettings_.home;
        // 始终保存当前可见 Tab；选中路径单独保存（勿因有选中就盖掉连点 Tab）
        hs.activeTab = static_cast<int>(activeHomeTab_);
        hs.clickerButton = static_cast<int>(clickerSettings_.button);
        hs.clickerIntervalMode = static_cast<int>(clickerSettings_.intervalMode);
        hs.clickerCustomInterval = clickerSettings_.customIntervalSeconds;
        hs.recorderCaptureScope = static_cast<int>(recorderSettings_.captureScope);
        hs.recorderInputMode = static_cast<int>(recorderSettings_.inputMode);
        hs.selectedScriptPath = (selectedScript_ >= 0 && selectedScript_ < static_cast<int>(scripts_.size()))
            ? scripts_[static_cast<size_t>(selectedScript_)].path : L"";
        hs.selectedRecordingPath = (selectedRecording_ >= 0 && selectedRecording_ < static_cast<int>(recordings_.size()))
            ? recordings_[static_cast<size_t>(selectedRecording_)].path : L"";
        switch (activeHomeTab_) {
        case quickscript::MainTab::Clicker: hs.clickerScrollOffset = homeScrollOffset_; break;
        case quickscript::MainTab::Recorder: hs.recorderScrollOffset = homeScrollOffset_; break;
        case quickscript::MainTab::Macro: hs.macroScrollOffset = homeScrollOffset_; break;
        case quickscript::MainTab::ScriptCustom: hs.scriptCustomScrollOffset = homeScrollOffset_; break;
        }
        if (persistJson) SaveAppSettings(appSettings_);
        // 与 bridge g_ctx 对齐，避免 UI quietSaveSettings 仍持旧 selected*Path 写回覆盖
        try {
            qst::webview::SyncHomeSelectionCache(
                hs.selectedScriptPath, hs.selectedRecordingPath, hs.activeTab);
        } catch (...) {
        }

        // 同时写入简单文本文件（UTF-8）；内容未变则跳过，避免 Lite 路径刷盘卡死 UI/热键
        std::string utf8;
        utf8 += "\xEF\xBB\xBF"; // UTF-8 BOM
        auto addNum = [&](int v) { utf8 += std::to_string(v) + "\n"; };
        auto addDouble = [&](double v) { utf8 += std::to_string(v) + "\n"; };
        auto addWstr = [&](const std::wstring& s) { utf8 += ToUtf8(s) + "\n"; };
        addNum(hs.activeTab);
        addNum(hs.clickerButton);
        addNum(hs.clickerIntervalMode);
        addDouble(hs.clickerCustomInterval);
        addNum(hs.recorderCaptureScope);
        addWstr(hs.selectedScriptPath);
        addWstr(hs.selectedRecordingPath);
        addNum(hs.clickerScrollOffset);
        addNum(hs.recorderScrollOffset);
        addNum(hs.macroScrollOffset);
        addNum(hs.scriptCustomScrollOffset);
        addNum(hs.recorderInputMode); // 追加字段，保持旧 home_state.txt 行号兼容
        static std::string s_lastHomeStateUtf8;
        if (utf8 == s_lastHomeStateUtf8) return;
        s_lastHomeStateUtf8 = utf8;
        std::ofstream f(AppDir() + L"\\home_state.txt", std::ios::binary | std::ios::trunc);
        if (f.is_open()) f.write(utf8.data(), utf8.size());
    }

// was engine_host_window.h:9346-9483
void EngineHost::RestoreHomeState() {
        // home_state.txt 可能被异常退出/手动编辑写坏：所有数值解析必须容错，
        // 否则 stoi/stod 抛异常会让整个程序启动即崩。
        auto parseIntSafe = [](const std::wstring& s, int fallback) {
            try {
                if (s.empty()) return fallback;
                const std::wstring t = Trim(s);
                if (t.empty()) return fallback;
                size_t consumed = 0;
                const int v = std::stoi(t, &consumed);
                return (consumed > 0 && consumed == t.size()) ? v : fallback;
            } catch (...) {
                return fallback;
            }
        };
        auto parseDoubleSafe = [](const std::wstring& s, double fallback) {
            try {
                if (s.empty()) return fallback;
                const std::wstring t = Trim(s);
                if (t.empty()) return fallback;
                size_t consumed = 0;
                const double v = std::stod(t, &consumed);
                return (consumed > 0 && consumed == t.size()) ? v : fallback;
            } catch (...) {
                return fallback;
            }
        };
        // 优先从简单文本文件加载（UTF-8 编码，避免区域设置问题）
        const std::wstring stateFile = AppDir() + L"\\home_state.txt";
        const std::wstring content = ReadAll(stateFile);
        if (!content.empty()) {
            // 按行解析
            std::vector<std::wstring> lines;
            size_t start = 0;
            for (size_t i = 0; i <= content.size(); ++i) {
                if (i == content.size() || content[i] == L'\n') {
                    // 去掉末尾可能存在的 \r
                    size_t end = i;
                    if (end > start && content[end - 1] == L'\r') --end;
                    lines.push_back(content.substr(start, end - start));
                    start = i + 1;
                }
            }
            if (lines.size() < 6) return; // 不够字段数，回退到 JSON

            int activeTab = 0;
            activeTab = parseIntSafe(lines[0], 0);
            if (activeTab >= 0 && activeTab <= 3)
                activeHomeTab_ = static_cast<quickscript::MainTab>(activeTab);
            {
                int btn = parseIntSafe(lines[1], -1);
                if (btn >= 0 && btn <= 2)
                    clickerSettings_.button = static_cast<quickscript::MouseButtonChoice>(btn);
            }
            {
                int mode = parseIntSafe(lines[2], -1);
                if (mode >= 0 && mode <= 2)
                    clickerSettings_.intervalMode = static_cast<quickscript::ClickIntervalMode>(mode);
            }
            {
                double val = parseDoubleSafe(lines[3], -1.0);
                if (val > 0) clickerSettings_.customIntervalSeconds = val;
            }
            if (!lines[4].empty()) {
                // 兼容旧状态文件；产品侧已固定全局捕获
                recorderSettings_.captureScope = quickscript::RecordCaptureScope::Global;
            }
            if (lines.size() > 11) {
                const int mode = parseIntSafe(lines[11], -1);
                if (mode >= 0 && mode <= 2)
                    recorderSettings_.inputMode = static_cast<quickscript::RecorderInputMode>(mode);
            }
            // 恢复选中脚本（与录制互斥，优先恢复有路径的那个）
            selectedScript_ = -1;
            selectedRecording_ = -1;
            if (lines.size() > 6) {
                std::wstring recordingPath = lines[6];
                if (!recordingPath.empty()) {
                    for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
                        if (_wcsicmp(recordings_[static_cast<size_t>(i)].path.c_str(),
                                recordingPath.c_str()) == 0) {
                            selectedRecording_ = i;
                            break;
                        }
                    }
                }
            }
            // 只有录制没恢复成功时才尝试恢复脚本（与录制互斥）
            if (selectedRecording_ < 0) {
                std::wstring scriptPath = lines[5];
                if (!scriptPath.empty()) {
                    for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
                        if (_wcsicmp(scripts_[static_cast<size_t>(i)].path.c_str(),
                                scriptPath.c_str()) == 0) {
                            selectedScript_ = i;
                            break;
                        }
                    }
                }
            }
            // 恢复滚动位置
            switch (activeHomeTab_) {
            case quickscript::MainTab::Clicker:
                homeScrollOffset_ = (lines.size() > 7) ? parseIntSafe(lines[7], 0) : 0;
                break;
            case quickscript::MainTab::Recorder:
                homeScrollOffset_ = (lines.size() > 8) ? parseIntSafe(lines[8], 0) : 0;
                ClampRecordingScroll();
                break;
            case quickscript::MainTab::Macro:
                homeScrollOffset_ = (lines.size() > 9) ? parseIntSafe(lines[9], 0) : 0;
                ClampHomeScroll();
                break;
            case quickscript::MainTab::ScriptCustom:
                homeScrollOffset_ = (lines.size() > 10) ? parseIntSafe(lines[10], 0) : 0;
                ClampAgentConvScroll();
                break;
            }
            return;
        }

        // 回退到 JSON 中读取（首次启动时会进入这里）
        const auto& hs = appSettings_.home;
        // 恢复标签页选中
        if (hs.activeTab >= 0 && hs.activeTab <= 3)
            activeHomeTab_ = static_cast<quickscript::MainTab>(hs.activeTab);
        // 恢复连点设置
        if (hs.clickerButton >= 0 && hs.clickerButton <= 2)
            clickerSettings_.button = static_cast<quickscript::MouseButtonChoice>(hs.clickerButton);
        if (hs.clickerIntervalMode >= 0 && hs.clickerIntervalMode <= 2)
            clickerSettings_.intervalMode = static_cast<quickscript::ClickIntervalMode>(hs.clickerIntervalMode);
        if (hs.clickerCustomInterval > 0)
            clickerSettings_.customIntervalSeconds = hs.clickerCustomInterval;
        // 恢复录制设置（捕获范围固定全局）
        recorderSettings_.captureScope = quickscript::RecordCaptureScope::Global;
        appSettings_.home.recorderCaptureScope = 1;
        if (hs.recorderInputMode >= 0 && hs.recorderInputMode <= 3)
            recorderSettings_.inputMode = static_cast<quickscript::RecorderInputMode>(hs.recorderInputMode);
        // 恢复选中脚本/录制（与录制/脚本互斥，优先恢复有路径的那个）
        selectedScript_ = -1;
        selectedRecording_ = -1;
        if (!hs.selectedRecordingPath.empty()) {
            for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
                if (_wcsicmp(recordings_[static_cast<size_t>(i)].path.c_str(),
                        hs.selectedRecordingPath.c_str()) == 0) {
                    selectedRecording_ = i;
                    break;
                }
            }
        }
        if (selectedRecording_ < 0 && !hs.selectedScriptPath.empty()) {
            for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
                if (_wcsicmp(scripts_[static_cast<size_t>(i)].path.c_str(),
                        hs.selectedScriptPath.c_str()) == 0) {
                    selectedScript_ = i;
                    break;
                }
            }
        }
        // 恢复滚动位置
        switch (activeHomeTab_) {
        case quickscript::MainTab::Clicker: homeScrollOffset_ = hs.clickerScrollOffset; break;
        case quickscript::MainTab::Recorder: { homeScrollOffset_ = hs.recorderScrollOffset; ClampRecordingScroll(); break; }
        case quickscript::MainTab::Macro: { homeScrollOffset_ = hs.macroScrollOffset; ClampHomeScroll(); break; }
        case quickscript::MainTab::ScriptCustom: { homeScrollOffset_ = hs.scriptCustomScrollOffset; ClampAgentConvScroll(); break; }
        }
    }

