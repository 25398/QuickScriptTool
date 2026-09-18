cmake_minimum_required(VERSION 3.20)

project(QuickScriptTool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── WebView2 壳（产品）────────────────────────────────────────────
# CMake/MSBuild 目标名仍是 QstWebViewShell；OUTPUT_NAME 为 QuickScriptTool.exe。
option(QST_BUILD_WEBVIEW_SHELL "Build QstWebViewShell (WebView UI + full engine)" ON)
option(QST_WEBVIEW_BUNDLE_FIXED "Copy WebView2Fixed next to QuickScriptTool.exe (portable)" ON)
option(QST_WEBVIEW_ALLOW_EVERGREEN "Allow system Evergreen if WebView2Fixed missing (dev only)" OFF)

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
    src/window_mode/background_input_target.cpp
    src/window_mode/background_uia_input.cpp
    src/window_mode/ui_element_probe.cpp
    src/window_mode/window_list.cpp
    src/window_mode/window_mode_permission.cpp
    src/window_mode/fake_focus/fake_focus_injector.cpp
    src/window_mode/fake_focus/fake_focus_soft_input_host.cpp
    src/window_mode/cdp/cdp_input.cpp
    src/window_mode/ext_bridge/ext_bridge_server.cpp
    src/window_mode/ext_bridge/ext_native_host.cpp
    src/window_mode/ext_bridge/ext_input.cpp
    # 注入技术库（对抗性测试框架，见 docs/anticheat-injection-testing.md）
    src/window_mode/injection/inject_common.cpp
    src/window_mode/injection/inject_technique.cpp
    src/window_mode/injection/inject_remote_thread.cpp
    src/window_mode/injection/inject_apc.cpp
    src/window_mode/injection/inject_thread_hijack.cpp
    src/window_mode/injection/inject_manual_map.cpp
    src/window_mode/injection/inject_peb_hide.cpp
    src/window_mode/injection/inject_hook.cpp
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
set(_FAKEFOCUS_DEF "${CMAKE_CURRENT_SOURCE_DIR}/src/window_mode/fake_focus/fake_focus.def")
if(MSVC)
    target_link_options(FakeFocus PRIVATE "/DEF:${_FAKEFOCUS_DEF}")
    set_property(TARGET FakeFocus APPEND PROPERTY LINK_DEPENDS "${_FAKEFOCUS_DEF}")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(FakeFocus PROPERTIES OUTPUT_NAME FakeFocus64)
else()
    set_target_properties(FakeFocus PROPERTIES OUTPUT_NAME FakeFocus32)
endif()
if(MSVC)
    target_compile_options(FakeFocus PRIVATE /W4 /permissive- /utf-8)
    # javaw/GLFW 进程通常没有旁路 VCRUNTIME140；/MD 注入会 LoadLibrary 失败甚至在游戏线程崩。
    set_property(TARGET FakeFocus PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
        DEPENDS ${FAKEFOCUS_DLL_SOURCES} ${_FAKEFOCUS32_SCRIPT} ${_FAKEFOCUS_DEF}
        COMMENT "Building FakeFocus32.dll (x86)"
        VERBATIM
    )
    add_dependencies(FakeFocus32 FakeFocus)
endif()

set(SCRIPT_CORE_COMMON_SOURCES
    src/action_utils.cpp
    src/ai_ui_layout.cpp
    src/ai_plan_util.cpp
    src/input/mouse_input_backend.cpp
    src/input/hid_interception.cpp
    src/input/virtual_hid.cpp
    src/input/foreground_input_router.cpp
    src/input/synthetic_input_filter.cpp
    src/input/input_emergency_teardown.cpp
    src/coord_space.cpp
    src/findimage_template_crop.cpp
    src/image_match.cpp
    src/image_match_engines.cpp
    src/image_var_util.cpp
    src/color_match.cpp
    src/ocr_engine.cpp
    src/opencv_runtime.cpp
    src/utils.cpp
)

add_library(script_core_common OBJECT ${SCRIPT_CORE_COMMON_SOURCES})
target_compile_definitions(script_core_common PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(script_core_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/driver/qst_vhid
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(script_core_common PUBLIC ${OpenCV_LIBS} setupapi)
if(MSVC)
    target_compile_options(script_core_common PRIVATE /W4 /permissive- /utf-8)
endif()

# ── 产品分层库（WebView 壳 + Engine）──────────────────────────────
include(cmake/QstProductLibs.cmake)
include(cmake/WebView2.cmake)

list(APPEND QST_ENGINE_SOURCES ${QST_ENGINE_UI_STUB_SOURCES})

add_library(qst_desktop_tools OBJECT ${QST_DESKTOP_TOOLS_SOURCES})
qst_configure_product_lib(qst_desktop_tools)

add_library(qst_engine_ui_stubs OBJECT ${QST_ENGINE_DIALOG_STUB_SOURCES})
qst_configure_product_lib(qst_engine_ui_stubs)
target_compile_definitions(qst_engine_ui_stubs PUBLIC QST_GDI_LEGACY=0)

add_library(qst_engine OBJECT ${QST_ENGINE_SOURCES})
qst_configure_product_lib(qst_engine)
target_compile_definitions(qst_engine PUBLIC
    QST_GDI_LEGACY=0
    QST_WEBVIEW_WITH_ENGINE=1
    QST_WEBVIEW_SHELL=1
)
target_link_libraries(qst_engine PUBLIC
    qst_desktop_tools
    qst_engine_ui_stubs
    ${QST_COMMON_LINK_LIBS}
)
qst_link_webview2(qst_engine)

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
    src/var_compute.cpp
)
target_compile_definitions(macro_variables_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(macro_variables_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(macro_variables_core PUBLIC user32 gdi32 shell32 advapi32)
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
    src/recorder_timeline.cpp
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
add_dependencies(WindowModeSelfTest FakeFocus)
if(TARGET FakeFocus32)
    add_dependencies(WindowModeSelfTest FakeFocus32)
endif()

qst_add_selftest(ScheduledTaskSelfTest
    tools/scheduled_task_selftest.cpp
    LIBS scheduled_task_core user32 shell32 shlwapi
)


qst_add_selftest(MacroVariablesSelfTest
    tools/macro_variables_selftest.cpp
    LIBS macro_variables_core user32 gdi32 shell32 advapi32
)

qst_add_selftest(ScriptActionBuilderSelfTest
    tools/script_action_builder_selftest.cpp
    tools/window_mode_link_stubs.cpp
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
target_compile_definitions(app_settings_store_core PUBLIC
    UNICODE _UNICODE NOMINMAX
    QST_WEBVIEW_SHELL=1
)
target_include_directories(app_settings_store_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(app_settings_store_core PUBLIC user32 shell32 shlwapi winmm advapi32 crypt32)
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
    tools/window_mode_link_stubs.cpp
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

qst_add_selftest(OcrSelfTest
    tools/ocr_selftest.cpp
    LIBS
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 shell32 shlwapi urlmon
)

qst_add_selftest(AiActionRouterSelfTest
    tools/ai_action_router_selftest.cpp
    tools/window_mode_link_stubs.cpp
    tools/fetch_webpage_link_stub.cpp
    src/macro_execute_tools.cpp
    src/page_snapshot.cpp
    src/ai_locate_cache.cpp
    src/ai_locate_verify.cpp
    src/office_doc.cpp
    src/process_utils.cpp
    src/ai_action_lookahead.cpp
    src/ai_logic_convert.cpp
    src/recording_to_findimage.cpp
    LIBS
        script_action_builder_core
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 shell32 shlwapi advapi32 ole32 imm32
)

qst_add_selftest(AgentAssistantSelfTest
    tools/agent_assistant_selftest.cpp
    tools/window_mode_link_stubs.cpp
    src/agent_attachment.cpp
    src/agent_conversation_store.cpp
    src/agent_script_ops.cpp
    src/recording_optimize_ops.cpp
    src/agent_shell.cpp
    src/agent_web.cpp
    src/agent_webview.cpp
    src/agent_undo.cpp
    src/agent_ui_notify.cpp
    src/agent_reference.cpp
    src/agent_core.cpp
    src/agent_tools.cpp
    src/macro_execute_tools.cpp
    src/page_snapshot.cpp
    src/ai_locate_cache.cpp
    src/ai_locate_verify.cpp
    src/office_doc.cpp
    src/process_utils.cpp
    src/ai_action_lookahead.cpp
    src/ai_logic_convert.cpp
    src/recording_to_findimage.cpp
    src/scheduled_task_store.cpp
    src/scheduled_task_types.cpp
    src/ui_scale.cpp
    LIBS
        script_action_builder_core
        script_core_common
        Qst::WebView2
        ${OpenCV_LIBS}
        user32 gdi32 shell32 shlwapi advapi32 ole32 oleaut32 imm32 winhttp crypt32
)

add_custom_command(TARGET AgentAssistantSelfTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AgentAssistantSelfTest>/skills/agent"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/.cursor/skills/agent-conversation/SKILL.md"
        "$<TARGET_FILE_DIR:AgentAssistantSelfTest>/skills/agent/conversation.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/.cursor/skills/agent-revert/SKILL.md"
        "$<TARGET_FILE_DIR:AgentAssistantSelfTest>/skills/agent/revert.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/.cursor/skills/agent-shell/SKILL.md"
        "$<TARGET_FILE_DIR:AgentAssistantSelfTest>/skills/agent/shell.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/.cursor/skills/agent-script/SKILL.md"
        "$<TARGET_FILE_DIR:AgentAssistantSelfTest>/skills/agent/scriptstrategy.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/.cursor/skills/agent-optimize/SKILL.md"
        "$<TARGET_FILE_DIR:AgentAssistantSelfTest>/skills/agent/optimize.md"
    COMMENT "Copy Agent skills for AgentAssistantSelfTest")

# 办公文档提取脚本：自检必须测**当前**脚本（否则会测到 build 目录里的旧副本）
add_custom_command(TARGET AiActionRouterSelfTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AiActionRouterSelfTest>/office"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tools/office/read_doc.ps1"
        "$<TARGET_FILE_DIR:AiActionRouterSelfTest>/office/read_doc.ps1"
    COMMENT "Copy office read_doc.ps1 for AiActionRouterSelfTest")

qst_add_selftest(AppSettingsStoreSelfTest
    tools/app_settings_store_selftest.cpp
    LIBS
        app_settings_store_core
        user32 shell32 shlwapi winmm
)

qst_add_selftest(VirtualHidSelfTest
    tools/virtual_hid_selftest.cpp
    src/input/virtual_hid.cpp
    LIBS user32 setupapi
)

qst_add_selftest(ThemeUiSelfTest
    tools/theme_ui_selftest.cpp
    src/app_theme.cpp
    src/ui_scale.cpp
    LIBS
        user32 gdi32 shell32
)

qst_add_selftest(BreakoutCooldownSelfTest
    tools/breakout_cooldown_selftest.cpp
)

qst_add_selftest(HotkeyStopSelfTest
    tools/hotkey_stop_selftest.cpp
)

qst_add_selftest(ClickerTimingSelfTest
    tools/clicker_timing_selftest.cpp
)

# 录制时间轴自检：不要链 script_core_common（含 SendInput/找图），否则易被 360 HEUR 误删。
qst_add_selftest(RecorderSelfTest
    tools/recorder_selftest.cpp
    tools/recorder_selftest_support.cpp
    src/recorder_timeline.cpp
    src/recording_to_findimage.cpp
    src/input_timeline_scheduler.cpp
    src/utils.cpp
    resources/selftest_version.rc
    LIBS user32 shell32 shlwapi winmm gdi32
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

# ── 注入技术库自测 / 对抗测试驱动 ─────────────────────────────
add_library(InjectionTestPayload SHARED tools/injection_test_payload.cpp)
target_compile_definitions(InjectionTestPayload PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(InjectionTestPayload PRIVATE kernel32 user32)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(InjectionTestPayload PROPERTIES OUTPUT_NAME InjectionTestPayload64)
else()
    set_target_properties(InjectionTestPayload PROPERTIES OUTPUT_NAME InjectionTestPayload32)
endif()
if(MSVC)
    target_compile_options(InjectionTestPayload PRIVATE /W4 /permissive- /utf-8 /GS-)
    target_link_options(InjectionTestPayload PRIVATE /NODEFAULTLIB /ENTRY:DllMain)
endif()

add_executable(InjectionTestTarget tools/injection_test_target.cpp)
target_compile_definitions(InjectionTestTarget PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(InjectionTestTarget PRIVATE user32)
if(MSVC)
    target_compile_options(InjectionTestTarget PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(InjectionTestTarget PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()

qst_add_selftest(InjectionSelfTest
    tools/injection_selftest.cpp
    LIBS
        window_mode_core
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 dwmapi shell32 ole32 oleaut32 UIAutomationCore urlmon d3d11 dxgi windowsapp
)
add_dependencies(InjectionSelfTest InjectionTestTarget InjectionTestPayload)
if(TARGET FakeFocus)
    add_dependencies(InjectionSelfTest FakeFocus)
endif()

# ── POST_BUILD：OpenCV / 图标 / OCR helper / 扩展 / VDA / Interception ─
function(qst_copy_product_runtime target)
    if(NOT WIN32)
        return()
    endif()

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
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_opencv_dbg}"
                    "$<TARGET_FILE_DIR:${target}>"
                COMMENT "Copying OpenCV debug runtime DLL for ${target}"
                CONFIGURATIONS Debug)
        endif()
        if(_opencv_rel)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_opencv_rel}"
                    "$<TARGET_FILE_DIR:${target}>"
                COMMENT "Copying OpenCV release runtime DLL for ${target}"
                CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
        endif()
    endforeach()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/breakout_pause.ico"
            "$<TARGET_FILE_DIR:${target}>/breakout_pause.ico"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/tray_running.ico"
            "$<TARGET_FILE_DIR:${target}>/tray_running.ico"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/app_icon.ico"
            "$<TARGET_FILE_DIR:${target}>/app_icon.ico"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/startup.wav"
            "$<TARGET_FILE_DIR:${target}>/startup.wav"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/finish.wav"
            "$<TARGET_FILE_DIR:${target}>/finish.wav"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/paddle_ocr_helper.py"
            "$<TARGET_FILE_DIR:${target}>/tools/paddle_ocr_helper.py"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/requirements-ocr.txt"
            "$<TARGET_FILE_DIR:${target}>/tools/requirements-ocr.txt"
        # extension\edge may hold bridge_runtime.json: host-written runtime bridge token with a
        # hardened ACL. cmake -E copy_directory fails on it with "Permission denied", and it must
        # never ship. Copy through the shared exclusion helper instead.
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/copy_extension_edge.ps1"
            -Source "${CMAKE_SOURCE_DIR}/extension/edge"
            -Dest "$<TARGET_FILE_DIR:${target}>/extension/edge"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/skills/agent"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/.cursor/skills/agent-conversation/SKILL.md"
            "$<TARGET_FILE_DIR:${target}>/skills/agent/conversation.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/.cursor/skills/agent-revert/SKILL.md"
            "$<TARGET_FILE_DIR:${target}>/skills/agent/revert.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/.cursor/skills/agent-shell/SKILL.md"
            "$<TARGET_FILE_DIR:${target}>/skills/agent/shell.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/.cursor/skills/agent-script/SKILL.md"
            "$<TARGET_FILE_DIR:${target}>/skills/agent/scriptstrategy.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/.cursor/skills/agent-optimize/SKILL.md"
            "$<TARGET_FILE_DIR:${target}>/skills/agent/optimize.md"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/office"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/office/read_doc.ps1"
            "$<TARGET_FILE_DIR:${target}>/office/read_doc.ps1"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/skills/agent/command.md"
            "$<TARGET_FILE_DIR:${target}>/skills/agent/command.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/skills/agent/office.md"
            "$<TARGET_FILE_DIR:${target}>/skills/agent/office.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/skills/agent/game.md"
            "$<TARGET_FILE_DIR:${target}>/skills/agent/game.md"
        COMMENT "Copying icons, OCR helpers, Edge extension, and Agent skills for ${target}")

    foreach(_vda_dll IN ITEMS
        "VirtualDesktopAccessor11.dll"
        "VirtualDesktopAccessor11_23h2.dll"
        "VirtualDesktopAccessor10.dll")
        if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/VirtualDesktopAccessor/${_vda_dll}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/third_party/VirtualDesktopAccessor/${_vda_dll}"
                    "$<TARGET_FILE_DIR:${target}>/${_vda_dll}"
                COMMENT "Copying VirtualDesktopAccessor ${_vda_dll} for ${target}")
        endif()
    endforeach()

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_INTERCEPTION_DLL "${CMAKE_SOURCE_DIR}/third_party/interception/x64/interception.dll")
    else()
        set(_INTERCEPTION_DLL "${CMAKE_SOURCE_DIR}/third_party/interception/x86/interception.dll")
    endif()
    if(EXISTS "${_INTERCEPTION_DLL}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_INTERCEPTION_DLL}"
                "$<TARGET_FILE_DIR:${target}>/interception.dll"
            COMMENT "Copying interception.dll for local/dev (not shipped in default zip)")
        endif()
endfunction()

function(qst_copy_webview_ui target)
    if(NOT WIN32)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/ui"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/index.html"
            "$<TARGET_FILE_DIR:${target}>/ui/index.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/agent.html"
            "$<TARGET_FILE_DIR:${target}>/ui/agent.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/agent.css"
            "$<TARGET_FILE_DIR:${target}>/ui/agent.css"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/shell.css"
            "$<TARGET_FILE_DIR:${target}>/ui/shell.css"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/bridge.js"
            "$<TARGET_FILE_DIR:${target}>/ui/bridge.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/app.js"
            "$<TARGET_FILE_DIR:${target}>/ui/app.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/visual_editor.js"
            "$<TARGET_FILE_DIR:${target}>/ui/visual_editor.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/pro-mode.js"
            "$<TARGET_FILE_DIR:${target}>/ui/pro-mode.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/pro-mode.css"
            "$<TARGET_FILE_DIR:${target}>/ui/pro-mode.css"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/app_icon.ico"
            "$<TARGET_FILE_DIR:${target}>/ui/app_icon.ico"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/debug.html"
            "$<TARGET_FILE_DIR:${target}>/ui/debug.html"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/ui/vendor"
            "$<TARGET_FILE_DIR:${target}>/ui/vendor"
        COMMENT "Copying WebView ui/ for ${target}")
endfunction()

function(qst_bundle_webview2_fixed target)
    if(NOT WIN32 OR NOT QST_WEBVIEW_BUNDLE_FIXED)
        return()
    endif()
    file(READ "${CMAKE_SOURCE_DIR}/tools/webview2_fixed_version.txt" _wv2_ver_file)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+" _wv2_ver "${_wv2_ver_file}")
    if(NOT _wv2_ver)
        message(WARNING "webview2_fixed_version.txt: no version line — skip WebView2Fixed bundle for ${target}")
        return()
    endif()
    set(_fixed_src "${CMAKE_SOURCE_DIR}/third_party/webview2_fixed/${_wv2_ver}/WebView2Fixed")
    if(NOT EXISTS "${_fixed_src}/msedgewebview2.exe")
        message(WARNING "WebView2Fixed missing at ${_fixed_src} — run tools/fetch_webview2_fixed.ps1")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -D "QST_WV2_SRC=${_fixed_src}"
            -D "QST_WV2_DST=$<TARGET_FILE_DIR:${target}>/WebView2Fixed"
            -D "QST_WV2_USERDATA=$<TARGET_FILE_DIR:${target}>/WebView2UserData"
            -P "${CMAKE_SOURCE_DIR}/cmake/CopyWebView2Fixed.cmake"
        COMMENT "Bundling WebView2Fixed ${_wv2_ver} for ${target}")
endfunction()

# ── QstWebViewShell（产品 exe → QuickScriptTool.exe）──────────────
if(QST_BUILD_WEBVIEW_SHELL)
    add_executable(QstWebViewShell WIN32
        resources/QuickScriptTool.rc
        src/webview/qst_webview_shell.cpp
        src/webview/webview_bridge_backend.cpp
    )
    # 用户/发版入口必须是 QuickScriptTool.exe；目标名保留以免打散 MSBuild / 打包脚本。
    set_target_properties(QstWebViewShell PROPERTIES OUTPUT_NAME QuickScriptTool)
    target_compile_definitions(QstWebViewShell PRIVATE
        UNICODE _UNICODE NOMINMAX
        QST_WEBVIEW_SHELL=1
        QST_WEBVIEW_USE_FIXED=1
        QST_WEBVIEW_WITH_ENGINE=1
        QST_GDI_LEGACY=0
    )
    if(QST_WEBVIEW_ALLOW_EVERGREEN)
        target_compile_definitions(QstWebViewShell PRIVATE QST_WEBVIEW_ALLOW_EVERGREEN=1)
    else()
        target_compile_definitions(QstWebViewShell PRIVATE QST_WEBVIEW_ALLOW_EVERGREEN=0)
    endif()
    target_include_directories(QstWebViewShell PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/driver/qst_vhid
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(QstWebViewShell PRIVATE
        qst_engine
        qst_desktop_tools
        qst_engine_ui_stubs
        ${QST_COMMON_LINK_LIBS}
    )
    qst_link_webview2(QstWebViewShell)
    if(MSVC)
        target_compile_options(QstWebViewShell PRIVATE /W4 /permissive- /utf-8)
        # OpenCV DELAYLOAD：缺 DLL / 杀软隔离时仍进壳，只禁用找图。
        target_link_libraries(QstWebViewShell PRIVATE delayimp)
        target_sources(QstWebViewShell PRIVATE resources/qst_uac.manifest)
        target_link_options(QstWebViewShell PRIVATE
            "/DELAYLOAD:opencv_world4100.dll"
            "/MANIFESTUAC:NO"
        )
    endif()

    add_dependencies(QstWebViewShell FakeFocus)
    if(TARGET FakeFocus32)
        add_dependencies(QstWebViewShell FakeFocus32)
    endif()

    # 先清旧文件名，避免后续 POST_BUILD（如 WebView2Fixed）失败时仍留下误导入口。
    add_custom_command(TARGET QstWebViewShell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:QstWebViewShell>/QstWebViewShell.exe"
            "$<TARGET_FILE_DIR:QstWebViewShell>/QstWebViewShell.pdb"
        COMMENT "Remove leftover QstWebViewShell.exe (product OUTPUT_NAME is QuickScriptTool)")
    qst_copy_webview_ui(QstWebViewShell)
    qst_copy_product_runtime(QstWebViewShell)
    qst_bundle_webview2_fixed(QstWebViewShell)
endif()

# ── 安装目录 Uninstall.exe（启动 Inno 卸载器并走 UAC）──────────
add_executable(QstUninstall WIN32 installer/qst_uninstall.cpp)
target_compile_definitions(QstUninstall PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(QstUninstall PRIVATE /W4 /permissive- /utf-8)
endif()
