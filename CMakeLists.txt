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
    src/ocr_engine.cpp
    src/ocr_install_dialog.cpp
    src/ocr_overlay.cpp
    src/process_utils.cpp
    src/crosshair_drag.cpp
    src/match_overlay.cpp
    src/popup_combo.cpp
    src/recorder.cpp
    src/recording_optimize_dialog.cpp
    src/script_io.cpp
    src/screenshot_overlay.cpp
    src/utils.cpp
)

target_compile_definitions(QuickScriptTool PRIVATE UNICODE _UNICODE NOMINMAX)

target_include_directories(QuickScriptTool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/DuiLib_Ultimate
    ${CMAKE_CURRENT_SOURCE_DIR}/DuiLib_Ultimate/DuiLib
)

target_link_libraries(QuickScriptTool PRIVATE
    ${OpenCV_LIBS}
    user32
    gdi32
    comdlg32
    shell32
    comctl32
    msimg32
    ole32
    oleaut32
)

if(MSVC)
    target_compile_options(QuickScriptTool PRIVATE /W4 /permissive- /utf-8)
endif()

# Copy OpenCV runtime DLL next to the executable
if(WIN32)
    file(GLOB OPENCV_RUNTIME_DLLS
        "${OpenCV_DIR}/x64/vc*/bin/opencv_world*.dll"
        "${OpenCV_DIR}/bin/opencv_world*.dll")
    foreach(OPENCV_DLL IN LISTS OPENCV_RUNTIME_DLLS)
        add_custom_command(TARGET QuickScriptTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENCV_DLL}"
                "$<TARGET_FILE_DIR:QuickScriptTool>"
            COMMENT "Copying OpenCV runtime DLL")
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
