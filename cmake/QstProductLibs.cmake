# QstProductLibs.cmake — qst_desktop_tools / qst_engine 源列表
# 由顶层 CMakeLists.txt include。产品 Engine 宿主：`src/engine/engine_host_window.h`。

set(QST_DESKTOP_TOOLS_SOURCES
    src/desktop_tools/desktop_tools.cpp
    src/screenshot_overlay.cpp
    src/drag_pick_overlay.cpp
    src/match_overlay.cpp
    src/ocr_overlay.cpp
    src/crosshair_drag.cpp
    src/hotkey_dialog.cpp
    src/macro_debug_window.cpp
    src/tray_menu.cpp
    src/themed_popup_menu.cpp
    src/desktop_tools/float_ball.cpp
    src/process_utils.cpp
    src/drawing.cpp
    src/render_device.cpp
    src/render_context_gdi.cpp
    src/render_context_d2d.cpp
    src/render_font.cpp
    src/ui_scale.cpp
    src/modern_edit.cpp
)

# Engine：宏/录制/连点/设置持久化/Agent 运行时 + headless EngineHost
# 旧 GDI 编辑器控件：仅 stubs（产品 UI 在 Web）。ApplyFont/Scale* 在 win32_ui_util。
set(QST_ENGINE_UI_STUB_SOURCES
    src/gdi_engine_ui_stubs.cpp
)

# 旧对话框空实现（EngineHost 头仍声明；产品 UI 在 Web）
set(QST_ENGINE_DIALOG_STUB_SOURCES
    src/gdi_legacy_stubs.cpp
)

set(QST_ENGINE_SOURCES
    src/clicker.cpp
    src/win32_ui_util.cpp
    src/macro_variables.cpp
    src/var_compute.cpp
    src/find_image_ui_debug.cpp
    src/recorder.cpp
    src/recorder_timeline.cpp
    src/recording_to_findimage.cpp
    src/input_timeline_scheduler.cpp
    src/app_settings_store.cpp
    src/app_branding.cpp
    src/app_theme.cpp
    src/agent_attachment.cpp
    src/ai_action_service.cpp
    src/ai_locate_verify.cpp
    src/office_doc.cpp
    src/mcp_server.cpp
    src/ai_action_runtime.cpp
    src/ai_action_router.cpp
    src/ai_decide.cpp
    src/ai_logic_convert.cpp
    src/scheduled_task_types.cpp
    src/scheduled_task_store.cpp
    src/scheduled_task_scheduler.cpp
    src/script_io.cpp
    src/script_action_builder.cpp
    src/recording_optimize_ops.cpp
    src/agent_ui_notify.cpp
    src/agent_script_ops.cpp
    src/agent_core.cpp
    src/agent_reference.cpp
    src/agent_system_prompt.cpp
    src/agent_conversation_store.cpp
    src/agent_tools.cpp
    src/agent_ai_actions.cpp
    src/agent_desktop_task.cpp
    src/agent_mcp.cpp
    src/agent_undo.cpp
    src/agent_shell.cpp
    # 网页版 AI 后端（C 方案，见 docs/web-ai-backend-design.md）：
    #   端点挂在 ExtBridgeServer 的监听器上（同一端口/同一 token），不是新服务。
    #   ⚠ web_ai_prompt.cpp 是**纯逻辑**（无 Win32），单测 `WebAiSelfTest` 只链它 + qst_utils。
    src/web_ai/web_ai_prompt.cpp
    src/web_ai/web_ai_image.cpp
    src/web_ai/web_ai_config.cpp
    src/web_ai/web_ai_config_parse.cpp
    src/web_ai/web_ai_driver.cpp
    src/web_ai/web_ai_backend.cpp
    # ★ 窗口反代（B 类·界面反代，docs/ai-proxy-roadmap.md §3）：
    #   把 GUI 客户端窗口（豆包客户端 / Cursor / 终端 TUI）当成 AI 后端。
    #   ⚠ `window_ai_profile.cpp` 是**纯逻辑**（只 include UIAutomation.h 取常量，
    #     不调任何 UIA 函数）⇒ `WindowAiSelfTest` 只链它 + qst_utils，
    #     不需要 uiautomationcore、不需要窗口。
    src/web_ai/window_ai_profile.cpp
    src/web_ai/window_ai_driver.cpp
    # OOXML 层（自研，不依赖 Office / 第三方库）：写 .xlsx 靠它（见 agent_shell 的
    # writeSpreadsheet）。⚠ 这三个源也编进 OoxmlSelfTest（只链 qst_utils）。
    src/ooxml/inflate.cpp
    src/ooxml/zip_archive.cpp
    src/ooxml/xlsx_doc.cpp
    # SQLite 只读层（自研，不依赖 sqlite3.c / Python）：读浏览器历史库靠它。
    # ⚠ sqlite_read.cpp 是纯逻辑（也编进 SqliteSelfTest）；
    #   sqlite_file.cpp 依赖 Win32（FILE_SHARE_* 打开被占用的库），只进产品库。
    src/sqlite/sqlite_read.cpp
    src/sqlite/sqlite_file.cpp
    # ★ 把「读浏览器历史/书签」接成助手工具（替代 observePage + 截图那条慢路）。
    #   依赖 Win32（注册表/LOCALAPPDATA/FileTimeToSystemTime）⇒ 只进产品库，不进纯逻辑自检。
    src/sqlite/browser_history.cpp
    src/agent_web.cpp
    src/agent_webview.cpp
    src/macro_execute_tools.cpp
    src/page_snapshot.cpp
    # window_mode_json.cpp 已移到 window_mode_common（STATIC，去重 A5）
    src/window_mode/window_mode_preview.cpp
    src/engine/engine_ui_hooks.cpp
    src/engine/engine_runtime.cpp
    src/engine/engine_hotkeys.cpp
    src/engine/engine_script_run.cpp
    src/engine/engine_record_click.cpp
    src/engine/engine_settings_reload.cpp
    src/engine/engine_window_mode_hooks.cpp
    src/engine/engine_gdi_editor.cpp
)

set(QST_COMMON_LINK_LIBS
    window_mode_core
    script_core_common
    ${OpenCV_LIBS}
    user32 gdi32 gdiplus dwmapi d2d1 dwrite comdlg32 shell32 comctl32 imm32 msimg32
    uxtheme ole32 oleaut32 propsys UIAutomationCore urlmon winhttp crypt32
    d3d11 dxgi windowsapp winmm
)

function(qst_configure_product_lib target)
    target_compile_definitions(${target} PUBLIC UNICODE _UNICODE NOMINMAX)
    target_include_directories(${target} PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/resources
        ${OpenCV_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/src/third_party
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
endfunction()
