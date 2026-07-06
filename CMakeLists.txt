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

add_executable(QuickScriptTool WIN32
    src/main.cpp
    src/action_utils.cpp
    src/clicker.cpp
    src/controls.cpp
    src/drawing.cpp
    src/editor_paint.cpp
    src/editor_dropdown.cpp
    src/hotkey_dialog.cpp
    src/image_match.cpp
    src/image_match_engines.cpp
    src/macro_variables.cpp
    src/macro_debug_window.cpp
    src/ocr_engine.cpp
    src/ocr_install_dialog.cpp
    src/ocr_overlay.cpp
    src/process_utils.cpp
    src/crosshair_drag.cpp
    src/match_overlay.cpp
    src/popup_combo.cpp
    src/panel_popup_combo.cpp
    src/recorder.cpp
    src/recording_optimize_dialog.cpp
    src/app_settings_store.cpp
    src/app_branding.cpp
    src/settings_dialog.cpp
    src/modern_edit.cpp
    src/agent_attachment.cpp
    src/ai_action_service.cpp
    src/ai_action_runtime.cpp
    src/ai_action_router.cpp
    src/tray_menu.cpp
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
    src/utils.cpp
    src/agent_core.cpp
    src/agent_reference.cpp
    src/agent_system_prompt.cpp
    src/agent_dialog.cpp
    src/agent_tools.cpp
    src/agent_ai_actions.cpp
    src/macro_execute_tools.cpp
    src/ui_component.cpp
)

target_compile_definitions(QuickScriptTool PRIVATE UNICODE _UNICODE NOMINMAX)

target_include_directories(QuickScriptTool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/DuiLib_Ultimate
    ${CMAKE_CURRENT_SOURCE_DIR}/DuiLib_Ultimate/DuiLib
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
)

target_link_libraries(QuickScriptTool PRIVATE
    ${OpenCV_LIBS}
    user32
    gdi32
    comdlg32
    shell32
    comctl32
    imm32
    msimg32
    ole32
    oleaut32
    urlmon
    winhttp
)

if(MSVC)
    target_compile_options(QuickScriptTool PRIVATE /W4 /permissive- /utf-8)
endif()

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
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/paddle_ocr_helper.py"
            "$<TARGET_FILE_DIR:QuickScriptTool>/tools/paddle_ocr_helper.py"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/requirements-ocr.txt"
            "$<TARGET_FILE_DIR:QuickScriptTool>/tools/requirements-ocr.txt"
        COMMENT "Copying PaddleOCR helper script and requirements")
endif()
