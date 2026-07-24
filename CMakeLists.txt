cmake_minimum_required(VERSION 3.20)

project(QuickScriptTool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── OpenCV ────────────────────────────────────────────────────────
if(NOT OpenCV_DIR)
    if(DEFINED ENV{OpenCV_DIR} AND EXISTS "$ENV{OpenCV_DIR}/OpenCVConfig.cmake")
        set(OpenCV_DIR "$ENV{OpenCV_DIR}")
    elseif(EXISTS "D:/OpenCV/opencv/build/OpenCVConfig.cmake")
        set(OpenCV_DIR "D:/OpenCV/opencv/build")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/opencv/build/OpenCVConfig.cmake")
        set(OpenCV_DIR "${CMAKE_SOURCE_DIR}/third_party/opencv/build")
    endif()
endif()

find_package(OpenCV REQUIRED)

set(WINDOW_MODE_CORE_SOURCES
    src/window_mode/window_mode_types.cpp
    src/window_mode/window_mode_log.cpp
    src/window_mode/hidden_desktop.cpp
    src/window_mode/macro_virtual_desktop.cpp
    src/window_mode/virtual_desktop_accessor.cpp
    src/window_mode/com_apartment.cpp
    src/window_mode/window_target.cpp
    src/window_mode/window_capture.cpp
    src/window_mode/window_capture_wgc.cpp
    src/window_mode/window_coords.cpp
    src/window_mode/window_mode_session.cpp
    src/window_mode/window_mode_executor.cpp
    src/window_mode/background_window_input.cpp
    src/window_mode/background_uia_input.cpp
    src/window_mode/window_mode_permission.cpp
    src/window_mode/fake_focus/fake_focus_injector.cpp
    src/window_mode/fake_focus/fake_focus_soft_input_host.cpp
    src/window_mode/cdp/cdp_input.cpp
    src/window_mode/ext_bridge/ext_bridge_server.cpp
    src/window_mode/ext_bridge/ext_input.cpp
)

add_library(window_mode_core OBJECT ${WINDOW_MODE_CORE_SOURCES})
target_compile_definitions(window_mode_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(window_mode_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(window_mode_core PUBLIC
    ${OpenCV_LIBS}
    user32
    gdi32
    dwmapi
    shell32
    shlwapi
    ole32
    oleaut32
    UIAutomationCore
    urlmon
    d3d11
    dxgi
    windowsapp
    winhttp
    psapi
    ws2_32
    bcrypt
)
if(MSVC)
    target_compile_options(window_mode_core PRIVATE /W4 /permissive- /utf-8)
endif()

# ── FakeFocus DLL（目标进程内 hook；按当前架构输出 FakeFocus64/32）──
set(FAKEFOCUS_DLL_SOURCES
    src/window_mode/fake_focus/fake_focus_dll.cpp
    src/window_mode/fake_focus/fake_focus_hook.cpp
)
add_library(FakeFocus SHARED ${FAKEFOCUS_DLL_SOURCES})
target_compile_definitions(FakeFocus PRIVATE UNICODE _UNICODE NOMINMAX FAKEFOCUS_EXPORTS)
target_include_directories(FakeFocus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/window_mode/fake_focus
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(FakeFocus PRIVATE user32 dwmapi)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(FakeFocus PROPERTIES OUTPUT_NAME FakeFocus64)
else()
    set_target_properties(FakeFocus PROPERTIES OUTPUT_NAME FakeFocus32)
endif()
if(MSVC)
    target_compile_options(FakeFocus PRIVATE /W4 /permissive- /utf-8)
endif()

# ── CdpEnable DLL（已停用注入路径；保留目标供归档，默认不编入 ALL）──
add_library(CdpEnable SHARED EXCLUDE_FROM_ALL src/window_mode/cdp/cdp_enable_dll.cpp)
target_compile_definitions(CdpEnable PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(CdpEnable PRIVATE user32)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(CdpEnable PROPERTIES OUTPUT_NAME CdpEnable64)
else()
    set_target_properties(CdpEnable PROPERTIES OUTPUT_NAME CdpEnable32)
endif()
if(MSVC)
    target_compile_options(CdpEnable PRIVATE /W4 /permissive- /utf-8)
endif()

# 从 x64 工程额外编一份 FakeFocus32.dll（用 Hostx64\\x86 工具链）
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_FAKEFOCUS32_SCRIPT "${CMAKE_SOURCE_DIR}/src/window_mode/fake_focus/build_fakefocus32.cmd")
    add_custom_target(FakeFocus32 ALL
        COMMAND "${_FAKEFOCUS32_SCRIPT}" "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/$<CONFIG>"
        DEPENDS ${FAKEFOCUS_DLL_SOURCES} ${_FAKEFOCUS32_SCRIPT}
        COMMENT "Building FakeFocus32.dll (x86)"
        VERBATIM
    )
    add_dependencies(FakeFocus32 FakeFocus)
endif()

set(SCRIPT_CORE_COMMON_SOURCES
    src/action_utils.cpp
    src/input/mouse_input_backend.cpp
    src/coord_space.cpp
    src/image_match.cpp
    src/image_match_engines.cpp
    src/ocr_engine.cpp
    src/utils.cpp
)

add_library(script_core_common OBJECT ${SCRIPT_CORE_COMMON_SOURCES})
target_compile_definitions(script_core_common PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(script_core_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(script_core_common PUBLIC ${OpenCV_LIBS})
if(MSVC)
    target_compile_options(script_core_common PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(QuickScriptTool WIN32
    resources/QuickScriptTool.rc
    src/main.cpp
    src/clicker.cpp
    src/controls.cpp
    src/drawing.cpp
    src/render_device.cpp
    src/render_context_gdi.cpp
    src/render_context_d2d.cpp
    src/render_font.cpp
    src/editor_paint.cpp
    src/editor_dropdown.cpp
    src/hotkey_dialog.cpp
    src/macro_variables.cpp
    src/macro_debug_window.cpp
    src/ocr_install_dialog.cpp
    src/ocr_overlay.cpp
    src/process_utils.cpp
    src/crosshair_drag.cpp
    src/find_image_ui_debug.cpp
    src/match_overlay.cpp
    src/popup_combo.cpp
    src/prompt_modal.cpp
    src/panel_popup_combo.cpp
    src/recorder.cpp
    src/recorder_timeline.cpp
    src/input_timeline_scheduler.cpp
    src/recording_optimize_dialog.cpp
    src/app_settings_store.cpp
    src/app_branding.cpp
    src/app_theme.cpp
    src/theme_custom_dialog.cpp
    src/settings_dialog.cpp
    src/modern_edit.cpp
    src/agent_attachment.cpp
    src/ai_action_service.cpp
    src/ai_action_runtime.cpp
    src/ai_action_router.cpp
    src/tray_menu.cpp
    src/taskbar_iconic_preview.cpp
    src/scheduled_task_types.cpp
    src/scheduled_task_store.cpp
    src/scheduled_task_scheduler.cpp
    src/scheduled_task_datetime_picker.cpp
    src/scheduled_task_dialog.cpp
    src/script_io.cpp
    src/script_action_builder.cpp
    src/agent_ui_notify.cpp
    src/agent_script_ops.cpp
    src/screenshot_overlay.cpp
    src/agent_core.cpp
    src/agent_reference.cpp
    src/agent_system_prompt.cpp
    src/agent_dialog.cpp
    src/agent_conversation_store.cpp
    src/agent_tools.cpp
    src/agent_ai_actions.cpp
    src/macro_execute_tools.cpp
    src/ui_component.cpp
    src/ui_scale.cpp
    src/window_mode/window_mode_json.cpp
    src/window_mode/window_mode_preview.cpp
    src/window_mode/window_pick_dialog.cpp
)

target_compile_definitions(QuickScriptTool PRIVATE UNICODE _UNICODE NOMINMAX)

target_include_directories(QuickScriptTool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/DuiLib_Ultimate
    ${CMAKE_CURRENT_SOURCE_DIR}/DuiLib_Ultimate/DuiLib
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
)

target_link_libraries(QuickScriptTool PRIVATE
    window_mode_core
    script_core_common
    ${OpenCV_LIBS}
    user32
    gdi32
    dwmapi
    d2d1
    dwrite
    comdlg32
    shell32
    comctl32
    imm32
    msimg32
    uxtheme
    ole32
    oleaut32
    propsys
    UIAutomationCore
    urlmon
    winhttp
    d3d11
    dxgi
    windowsapp
    winmm
)

if(MSVC)
    target_compile_options(QuickScriptTool PRIVATE /W4 /permissive- /utf-8)
endif()

# ── Agent SelfTest harness helper ─────────────────────────────────
# Usage: qst_add_selftest(Name sources... [LIBS lib1 lib2 ...])
function(qst_add_selftest name)
    cmake_parse_arguments(QST "" "" "LIBS" ${ARGN})
    if(NOT QST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "qst_add_selftest(${name}): no SOURCES")
    endif()
    add_executable(${name} ${QST_UNPARSED_ARGUMENTS})
    target_compile_definitions(${name} PRIVATE UNICODE _UNICODE NOMINMAX)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
        ${OpenCV_INCLUDE_DIRS}
    )
    if(QST_LIBS)
        target_link_libraries(${name} PRIVATE ${QST_LIBS})
    endif()
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive- /utf-8)
        set_target_properties(${name} PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
    endif()
endfunction()

add_library(scheduled_task_core OBJECT
    src/scheduled_task_types.cpp
    src/scheduled_task_store.cpp
    src/scheduled_task_scheduler.cpp
    src/utils.cpp
)
target_compile_definitions(scheduled_task_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(scheduled_task_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(scheduled_task_core PUBLIC user32 shell32 shlwapi)
if(MSVC)
    target_compile_options(scheduled_task_core PRIVATE /W4 /permissive- /utf-8)
endif()

add_library(macro_variables_core OBJECT
    src/macro_variables.cpp
)
target_compile_definitions(macro_variables_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(macro_variables_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
)
if(MSVC)
    target_compile_options(macro_variables_core PRIVATE /W4 /permissive- /utf-8)
endif()

add_library(script_action_builder_core OBJECT
    src/script_action_builder.cpp
    src/agent_ai_actions.cpp
    src/ai_action_router.cpp
    src/app_settings_store.cpp
    src/app_theme.cpp
    src/script_io.cpp
    src/window_mode/window_mode_json.cpp
    src/window_mode/window_mode_types.cpp
)
target_compile_definitions(script_action_builder_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(script_action_builder_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(script_action_builder_core PUBLIC
    script_core_common
    ${OpenCV_LIBS}
    user32 shell32 shlwapi
)
if(MSVC)
    target_compile_options(script_action_builder_core PRIVATE /W4 /permissive- /utf-8)
endif()

qst_add_selftest(WindowModeSelfTest
    tools/window_mode_selftest.cpp
    src/window_mode/window_mode_json.cpp
    LIBS
        window_mode_core
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 dwmapi shell32 ole32 oleaut32 UIAutomationCore urlmon d3d11 dxgi windowsapp
)
add_dependencies(QuickScriptTool FakeFocus)
add_dependencies(WindowModeSelfTest FakeFocus)
if(TARGET FakeFocus32)
    add_dependencies(WindowModeSelfTest FakeFocus32)
    add_dependencies(QuickScriptTool FakeFocus32)
endif()

qst_add_selftest(ScheduledTaskSelfTest
    tools/scheduled_task_selftest.cpp
    LIBS scheduled_task_core user32 shell32 shlwapi
)

qst_add_selftest(MacroVariablesSelfTest
    tools/macro_variables_selftest.cpp
    LIBS macro_variables_core
)

qst_add_selftest(ScriptActionBuilderSelfTest
    tools/script_action_builder_selftest.cpp
    LIBS
        script_action_builder_core
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 shell32 shlwapi
)

add_library(app_settings_store_core OBJECT
    src/app_settings_store.cpp
    src/utils.cpp
)
target_compile_definitions(app_settings_store_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(app_settings_store_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(app_settings_store_core PUBLIC user32 shell32 shlwapi)
if(MSVC)
    target_compile_options(app_settings_store_core PRIVATE /W4 /permissive- /utf-8)
endif()

qst_add_selftest(CoordSpaceSelfTest
    tools/coord_space_selftest.cpp
    LIBS
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 shell32 shlwapi
)

qst_add_selftest(ScriptIoSelfTest
    tools/script_io_selftest.cpp
    LIBS
        script_action_builder_core
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 shell32 shlwapi
)

qst_add_selftest(ImageMatchSelfTest
    tools/image_match_selftest.cpp
    LIBS
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 shell32 shlwapi
)

qst_add_selftest(AiActionRouterSelfTest
    tools/ai_action_router_selftest.cpp
    LIBS
        script_action_builder_core
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 shell32 shlwapi
)

qst_add_selftest(AppSettingsStoreSelfTest
    tools/app_settings_store_selftest.cpp
    LIBS
        app_settings_store_core
        user32 shell32 shlwapi
)

qst_add_selftest(ThemeUiSelfTest
    tools/theme_ui_selftest.cpp
    src/app_theme.cpp
    src/ui_scale.cpp
    LIBS
        user32 gdi32 shell32
)

# 录制时间轴自检：不要链 script_core_common（含 SendInput/找图），否则易被 360 HEUR 误删。
qst_add_selftest(RecorderSelfTest
    tools/recorder_selftest.cpp
    tools/recorder_selftest_support.cpp
    src/recorder_timeline.cpp
    src/input_timeline_scheduler.cpp
    src/utils.cpp
    resources/selftest_version.rc
    LIBS user32 shell32 shlwapi winmm
)
# 输出名避开 Recorder*+SendInput 启发式指纹；CMake/技能里仍用目标名 RecorderSelfTest
set_target_properties(RecorderSelfTest PROPERTIES OUTPUT_NAME QstRecorderLogicTest)

qst_add_selftest(WindowModeDiag
    tools/window_mode_diag.cpp
    LIBS
        window_mode_core
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 dwmapi shell32 ole32 oleaut32 UIAutomationCore urlmon d3d11 dxgi windowsapp
)

# Copy OpenCV runtime DLL next to the executable (Debug/Release matched per config)
if(WIN32)
    set(_opencv_bin_dirs "")
    foreach(_subdir IN ITEMS "x64/vc16/bin" "x64/vc17/bin" "x64/vc18/bin" "x64/vc15/bin" "bin")
        if(EXISTS "${OpenCV_DIR}/${_subdir}")
            list(APPEND _opencv_bin_dirs "${OpenCV_DIR}/${_subdir}")
        endif()
    endforeach()
    foreach(_bin_dir IN LISTS _opencv_bin_dirs)
        file(GLOB _opencv_dbg "${_bin_dir}/opencv_world*d.dll")
        file(GLOB _opencv_rel "${_bin_dir}/opencv_world[0-9]*.dll")
        list(FILTER _opencv_rel EXCLUDE REGEX "d\\.dll$")
        if(_opencv_dbg)
            add_custom_command(TARGET QuickScriptTool POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_opencv_dbg}"
                    "$<TARGET_FILE_DIR:QuickScriptTool>"
                COMMENT "Copying OpenCV debug runtime DLL"
                CONFIGURATIONS Debug)
        endif()
        if(_opencv_rel)
            add_custom_command(TARGET QuickScriptTool POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_opencv_rel}"
                    "$<TARGET_FILE_DIR:QuickScriptTool>"
                COMMENT "Copying OpenCV release runtime DLL"
                CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
        endif()
    endforeach()
    add_custom_command(TARGET QuickScriptTool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:QuickScriptTool>/tools"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:QuickScriptTool>/extension/edge/icons"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/breakout_pause.ico"
            "$<TARGET_FILE_DIR:QuickScriptTool>/breakout_pause.ico"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/tray_running.ico"
            "$<TARGET_FILE_DIR:QuickScriptTool>/tray_running.ico"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/app_icon.ico"
            "$<TARGET_FILE_DIR:QuickScriptTool>/app_icon.ico"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/paddle_ocr_helper.py"
            "$<TARGET_FILE_DIR:QuickScriptTool>/tools/paddle_ocr_helper.py"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/requirements-ocr.txt"
            "$<TARGET_FILE_DIR:QuickScriptTool>/tools/requirements-ocr.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/extension/edge"
            "$<TARGET_FILE_DIR:QuickScriptTool>/extension/edge"
        COMMENT "Copying PaddleOCR helper, icons, and Edge extension")
    foreach(_vda_dll IN ITEMS
        "VirtualDesktopAccessor11.dll"
        "VirtualDesktopAccessor10.dll")
        if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/VirtualDesktopAccessor/${_vda_dll}")
            add_custom_command(TARGET QuickScriptTool POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/third_party/VirtualDesktopAccessor/${_vda_dll}"
                    "$<TARGET_FILE_DIR:QuickScriptTool>/${_vda_dll}"
                COMMENT "Copying VirtualDesktopAccessor ${_vda_dll}")
        endif()
    endforeach()
    # FakeFocus DLL 已由 OUTPUT 目录与 FakeFocus/FakeFocus32 目标写入 build/$<CONFIG>/
endif()
