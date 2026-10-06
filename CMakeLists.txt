cmake_minimum_required(VERSION 3.20)

project(QuickScriptTool LANGUAGES C CXX)

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
    # 注入副本（安装目录那份 DLL 永不被映射 ⇒ 覆盖安装/卸载不必重启电脑）
    src/window_mode/fake_focus/fake_focus_stage.cpp
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

# ── window_mode 数据层（构建去重 A5）─────────────────────────────
# window_mode_types.cpp（818 行）与 window_mode_json.cpp（214 行）是**纯函数**
# （只有匿名 namespace，无全局可变状态、无静态注册），却分别被 2 / 3 个 target
# 各编一份：types → window_mode_core + script_action_builder_core；
# json → qst_engine + script_action_builder_core + WindowModeSelfTest。
# 合计约 1,246 行重复编译。这里编一次进 STATIC 库。
# 与 qst_utils 同理：**必须 STATIC 而不是 OBJECT** —— OBJECT 库的 .obj 不跨
# 中间 OBJECT 库传递；STATIC 库的 PUBLIC 链接依赖可正常传递。
add_library(window_mode_common STATIC
    src/window_mode/window_mode_types.cpp
    src/window_mode/window_mode_json.cpp
)
target_compile_definitions(window_mode_common PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(window_mode_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(window_mode_common PUBLIC
    user32 gdi32 shell32 shlwapi ole32
)
if(MSVC)
    target_compile_options(window_mode_common PRIVATE /W4 /permissive- /utf-8)
endif()

add_library(window_mode_core OBJECT ${WINDOW_MODE_CORE_SOURCES})
target_compile_definitions(window_mode_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(window_mode_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(window_mode_core PUBLIC
    window_mode_common
    ${OpenCV_LIBS}
    user32
    gdi32
    dwmapi
    shell32
    shlwapi
    ole32
    oleaut32
    UIAutomationCore
    # ⚠⚠ 2026-10-06：`oleacc` 必须挂在 **PUBLIC** 上（而不是只挂产品 + 某个自检）。
    #   原因：`background_uia_input.cpp`（MSAA 的 `AccessibleObjectFromPoint`）属于
    #   `window_mode_core`，而**任何**链接它的目标都会带上那个 obj ⇒ 都需要 oleacc。
    #   我先只在 `QST_COMMON_LINK_LIBS` 和 `WindowModeSelfTest` 各加了一处，
    #   结果 **CI 在 `InjectionSelfTest` 上炸了 LNK2019**（本机不编那个目标，看不见）。
    #   ⇒ 挂在 PUBLIC 上，依赖者自动继承，**不会再漏第四个目标**。
    oleacc
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

# ── MinHook（BSD-2-Clause；third_party/minhook/）────────────────────────────
# FakeFocus 的内联钩用它拿 **trampoline**：目标函数头**只在安装时写一次**，之后原函数
# 经 trampoline 调用 —— 消除「临时还原 + 重写跳转」的多线程写入竞态
# （旧实现让 Unity/UE 这类多线程目标 0xC0000005 闪退；复现见 build/_tmp/hookrace/）。
set(MINHOOK_SOURCES
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
    third_party/minhook/src/hde/hde64.c
)
add_library(qst_minhook STATIC ${MINHOOK_SOURCES})
target_include_directories(qst_minhook PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/hde
)
if(MSVC)
    # MinHook 是 C 代码：不要套 /permissive-（C++ 专用）。
    target_compile_options(qst_minhook PRIVATE /W3 /utf-8)
    # 运行时库必须与 FakeFocus 一致（/MT），否则 DLL 里会同时存在两套 CRT 堆。
    set_property(TARGET qst_minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
target_link_libraries(FakeFocus PRIVATE user32 dwmapi qst_minhook)
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
        DEPENDS ${FAKEFOCUS_DLL_SOURCES} ${MINHOOK_SOURCES} ${_FAKEFOCUS32_SCRIPT} ${_FAKEFOCUS_DEF}
        COMMENT "Building FakeFocus32.dll (x86)"
        VERBATIM
    )
    add_dependencies(FakeFocus32 FakeFocus)
endif()

# ── 共享工具库（构建去重 #9）─────────────────────────────────────
# utils.cpp（1,814 行）原本被 4 个 target 各编一份：script_core_common /
# scheduled_task_core / app_settings_store_core / RecorderSelfTest。
# 这里编一次进 STATIC 库。
# 为什么必须 STATIC 而不是 OBJECT：OBJECT 库的 .obj 不会经由中间 OBJECT 库
# 传递给消费者（实测：只链 qst_engine 会缺 qst_desktop_tools 的符号，
# 所以 QstWebViewShell 才要逐个列出）。STATIC 库可正常传递。
# utils.cpp 无全局注册对象（纯函数），不担心静态库裁剪掉初始化。
# app_instance.cpp：g_instance 的定义（原在壳里 → 引擎/桌面工具链接不到）。
add_library(qst_utils STATIC
    src/utils.cpp
    src/app_instance.cpp
    src/script_package.cpp
    src/player/player_runtime.cpp)
target_compile_definitions(qst_utils PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(qst_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(qst_utils PUBLIC user32 shell32 shlwapi winmm advapi32 crypt32 gdi32)
if(MSVC)
    target_compile_options(qst_utils PRIVATE /W4 /permissive- /utf-8)
endif()

set(SCRIPT_CORE_COMMON_SOURCES
    src/action_utils.cpp
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
    src/ocr_winrt.cpp
    src/opencv_runtime.cpp
)

add_library(script_core_common OBJECT ${SCRIPT_CORE_COMMON_SOURCES})
target_compile_definitions(script_core_common PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(script_core_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/driver/qst_vhid
    ${OpenCV_INCLUDE_DIRS}
)
# windowsapp：ocr_winrt.cpp 用 C++/WinRT 调 Windows.Media.Ocr（系统自带 OCR）。
# 必须 PUBLIC —— 任何链 script_core_common 的目标（含各 SelfTest）都会带进 ocr_winrt.obj。
target_link_libraries(script_core_common PUBLIC ${OpenCV_LIBS} setupapi qst_utils windowsapp)
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
)
target_compile_definitions(scheduled_task_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(scheduled_task_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(scheduled_task_core PUBLIC user32 shell32 shlwapi qst_utils)
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
# ★ qst_utils：QuickInputVarItemsJson（给 Web 编辑器的变量清单）用 utils 的 EscapeJson、
#   而变量清单的数字格式化也共用 utils —— 别去掉，否则 MacroVariablesSelfTest LNK2019。
target_link_libraries(macro_variables_core PUBLIC user32 gdi32 shell32 advapi32 qst_utils)
if(MSVC)
    target_compile_options(macro_variables_core PRIVATE /W4 /permissive- /utf-8)
endif()

add_library(script_action_builder_core OBJECT
    src/script_action_builder.cpp
    src/agent_ai_actions.cpp
    src/ai_action_router.cpp
    # ai_decide.cpp 也编进这里：agent_ai_actions.cpp（本库成员）会调它的判断表 +
    # 诊断日志，而本库的消费者（ScriptActionBuilderSelfTest / ScriptIoSelfTest /
    # ScriptSerializationSelfTest）都**不链 qst_engine**（见下方 458 行注释：
    # 链 qst_engine 就不能再链本库）。新库只依赖它会让这 3 个目标链接失败（实测 LNK2019）。
    src/ai_decide.cpp
    src/app_settings_store.cpp
    src/app_theme.cpp
    src/script_io.cpp
    src/recorder_timeline.cpp
)
# ⚠ 本库还引 webai::Enabled/ModelNameW/IsWebAiModelName/EnsureProfileInSettings
#   （agent_ai_actions.cpp），真身在 src/web_ai/web_ai_config.cpp —— 那个文件会拉进
#   window_mode/ext_bridge（浏览器桥 + HTTP），逻辑档不该背 ⇒ 三个消费者各自链
#   `tools/web_ai_link_stubs.cpp`（恒 false 的桩，理由见该文件头）。加新消费者时别忘了它。
target_compile_definitions(script_action_builder_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_include_directories(script_action_builder_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(script_action_builder_core PUBLIC
    script_core_common
    window_mode_common
    ${OpenCV_LIBS}
    user32 shell32 shlwapi
)
if(MSVC)
    target_compile_options(script_action_builder_core PRIVATE /W4 /permissive- /utf-8)
endif()

qst_add_selftest(WindowModeSelfTest
    tools/window_mode_selftest.cpp
    LIBS
        window_mode_core
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 dwmapi shell32 ole32 oleaut32 UIAutomationCore urlmon d3d11 dxgi windowsapp
        # ⚠ 2026-10-06：oleacc 用于 MSAA 的 AccessibleObjectFromPoint
        #   （`background_uia_input.cpp` 的 TryHitTestViaMsaa）。
        #   ⚠ 自检**不用** QST_COMMON_LINK_LIBS，所以这里必须单独列 ——
        #     漏了就是 LNK2019（已踩一次）。
        oleacc
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
    tools/web_ai_link_stubs.cpp
    LIBS
        script_action_builder_core
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 shell32 shlwapi
)

add_library(app_settings_store_core OBJECT
    src/app_settings_store.cpp
)
target_compile_definitions(app_settings_store_core PUBLIC
    UNICODE _UNICODE NOMINMAX
    QST_WEBVIEW_SHELL=1
)
target_include_directories(app_settings_store_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(app_settings_store_core PUBLIC user32 shell32 shlwapi winmm advapi32 crypt32 qst_utils)
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
    tools/web_ai_link_stubs.cpp
    LIBS
        script_action_builder_core
        script_core_common
        ${OpenCV_LIBS}
        user32 gdi32 shell32 shlwapi
)

# 脚本包依赖收集（导出/导入）：只链 qst_utils，不需要 OpenCV / 引擎。
# 锁住「递归收集嵌套脚本 + 图片」「多个 runMacro 都要改写」「循环引用安全」。
qst_add_selftest(ScriptPackageSelfTest
    tools/script_package_selftest.cpp
    LIBS
        qst_utils
        user32 shell32 shlwapi
)

# D 段（#4 统一序列化）的安全网：脚本 读→写→再读 往返。
# 默认跑内置合成用例（CI 安全）；--corpus <dir> 跑真实历史脚本库（D1 基线）。
qst_add_selftest(ScriptSerializationSelfTest
    tools/script_serialization_selftest.cpp
    tools/window_mode_link_stubs.cpp
    tools/web_ai_link_stubs.cpp
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

# 构建去重（架构评估 #9）：原先这里重编 9 个被测源（约 11,791 行，含
# macro_execute_tools.cpp 6,646 行），它们都已在 qst_engine 内编过一次。
# 改链 qst_engine 后：
#   - tools/fetch_webpage_link_stub.cpp 不再需要（agent_web.cpp 的真身已在 qst_engine）；
#   - tools/window_mode_link_stubs.cpp 也不再需要（符号由 window_mode_core 提供）；
#   - tools/engine_link_stubs.cpp 已删除（引擎→壳依赖已倒置，见 engine_ui_hooks.*）。
qst_add_selftest(AiActionRouterSelfTest
    tools/ai_action_router_selftest.cpp
    LIBS
        qst_engine
        qst_desktop_tools
        qst_engine_ui_stubs
        Qst::WebView2
        ${QST_COMMON_LINK_LIBS}
)

# 构建去重（架构评估 #9）：原先这里直接列出 24 个被测 .cpp（22,595 行）逐个重编，
# 而这些源已经在 qst_engine 里编过一次。改为直接链 qst_engine：
#   - 少编 22,595 行（含 macro_execute_tools.cpp 6,646 行、agent_tools.cpp 2,087 行）；
#   - 不再需要 window_mode_link_stubs.cpp —— 那些符号由 window_mode_core 真身提供，
#     同时链 stub 会符号重复。
# 注意：OBJECT 库的 .obj 不会经由中间 OBJECT 库传递，必须像 QstWebViewShell 那样
# 把 qst_engine / qst_desktop_tools / qst_engine_ui_stubs 逐个列出，否则会缺
# RenderBatchScope / UiEditorWidth / ResolveProgramLaunchPath 等符号。
# 链 qst_engine 后**不要**再链 script_action_builder_core（其源已在 qst_engine 内）。
# 也不再需要 tools/engine_link_stubs.cpp —— 引擎对壳的 5 个符号依赖已倒置成
# 回调注入（src/engine/engine_ui_hooks.*），引擎可独立链接。
qst_add_selftest(AgentAssistantSelfTest
    tools/agent_assistant_selftest.cpp
    LIBS
        qst_engine
        qst_desktop_tools
        qst_engine_ui_stubs
        Qst::WebView2
        ${QST_COMMON_LINK_LIBS}
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
    # 产品自有 section（skills/agent/*.md）—— 缺了就会静默回落到内嵌文本，
    # 于是「改了 md 却没生效」这类问题在自检里看不出来。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/skills/agent/desktop.md"
        "$<TARGET_FILE_DIR:AgentAssistantSelfTest>/skills/agent/desktop.md"
    COMMENT "Copy Agent skills for AgentAssistantSelfTest")

# AI 助手「动手」层（runDesktopTask）：参数解析 / 安全闸 / 动作构造 / 结果清洗。
# 只链 qst_utils —— 被测逻辑是纯函数，不碰引擎、不碰 OpenCV，
# 所以它能在无桌面会话、无网、无驱动的机器上跑（进逻辑档）。
qst_add_selftest(AgentDesktopTaskSelfTest
    tools/agent_desktop_task_selftest.cpp
    src/agent_desktop_task.cpp
    LIBS
        qst_utils
        user32 shell32 shlwapi winmm
)

# AI 助手 MCP 客户端（agent_mcp.*）：协议纯函数 + stdio 客户端端到端。
# 只链 qst_utils —— agent_mcp.cpp 的外部依赖只有 nlohmann/json 与 Win32，
# 所以能在无桌面会话的机器上跑（进逻辑档）。
# ⚠ 该 exe 自带 `--fake-mcp-server` 模式：用例把自己的路径当 command 起子进程，
#   从而用**真管道 + 真进程**验证握手/列工具/调用/错误/超时（不依赖任何外部 MCP server）。
qst_add_selftest(AgentMcpSelfTest
    tools/agent_mcp_selftest.cpp
    src/agent_mcp.cpp
    LIBS
        qst_utils
        user32 shell32 shlwapi winmm
)

# OOXML 层（src/ooxml/）：自研 inflate（RFC 1951）+ zip 读写（支持「未改动条目
# 原样搬运」）+ xlsx 读写（**只对该单元格做外科手术式替换**，其余 part 一字不动）。
# 这是「不装 Office 也能读写 xlsx/docx/pptx」的地基，**不依赖任何第三方库**。
# 只链 qst_utils —— 纯逻辑，能在无桌面会话、无网、无 Office 的机器上跑（进逻辑档）。
# ⚠ zip 夹具是**硬编码的真 deflate 流 + 真 zip**（用 Python zlib/zipfile 生成后内联）；
#   xlsx 夹具由用例自己用 ZipArchive 拼出来（可读性优先，deflate 路径已由 zip 用例覆盖）。
qst_add_selftest(OoxmlSelfTest
    tools/ooxml_selftest.cpp
    src/ooxml/inflate.cpp
    src/ooxml/zip_archive.cpp
    src/ooxml/xlsx_doc.cpp
    LIBS
        qst_utils
        user32 shell32 shlwapi
)

# SQLite 只读层（src/sqlite/）：手写的 SQLite 3 文件解析器，只做「读一张表」。
# 用它读**浏览器历史库**（Edge/Chrome 的 History）—— 对照实测的 UI 路线
# （observePage 在内置页必失败 → listUiControls 只给进视口的几十条、标题还被省略号截断
#  → 反复 zoom 读像素 → Excel COM 收尾 23.6 秒），读库是**一条命令、全量、标题完整**。
# **不依赖 sqlite3.c / Python**（产品不依赖别人的软件，见 AGENTS.md）。
# ⚠ 夹具是**真的 SQLite 库字节**（用 Python 官方 sqlite3 造好后内联），
#   期望值同样由官方模块查出 —— 不是照本实现反推的。
# ⚠ 只链纯逻辑那部分：`sqlite_read.cpp`（不碰 Win32）。
#   `sqlite_file.cpp` 带 FILE_SHARE_* 打开（浏览器运行时库被占用）⇒ 需要 windows.h，
#   留给真正读文件的宿主（AgentAssistantSelfTest）覆盖。
qst_add_selftest(SqliteSelfTest
    tools/sqlite_selftest.cpp
    src/sqlite/sqlite_read.cpp
    LIBS
        qst_utils
        user32
)

# 网页版 AI 后端（C 方案，docs/web-ai-backend-design.md）：**纯逻辑**部分。
# 只链 web_ai_prompt.cpp（消息→提示词、tool_calls 反解析、OpenAI/SSE 响应构造）
# 与 web_ai_config_parse.cpp（开关文件解析）—— 不链驱动/桥/HTTP，因此无
# 浏览器、无网、无桌面会话也能跑（进逻辑档）。
# ⚠ 真机部分（豆包能不能写进去/读回来）**不在本套**：那必须由产品进程内的
#   `POST /qst/web-ai/probe` 跑 —— 外部进程的 WS 探针进不了桥的业务链路
#   （`ext_bridge_server.cpp` 的 HandleClient 只把入站消息当 result/ready 回执）。
qst_add_selftest(WebAiSelfTest
    tools/web_ai_selftest.cpp
    src/web_ai/web_ai_prompt.cpp
    src/web_ai/web_ai_image.cpp
    src/web_ai/web_ai_config_parse.cpp
    # web_ai_prompt.cpp 会写窗口模式日志 ⇒ 补 windowmode::WindowModeLogEventf 的桩
    # （真身 window_mode_log.cpp 在 window_mode_core 里，逻辑档不背；见该文件头注释）。
    tools/window_mode_link_stubs.cpp
    LIBS
        qst_utils
        user32
)

# 窗口反代（B 类·界面反代，docs/ai-proxy-roadmap.md §3）：**纯逻辑**部分。
# 只链 window_ai_profile.cpp（档案合并 / 窗口匹配 / 输入框打分 / 回复提取 / 稳定判据）
# —— 它只 include UIAutomation.h 取**常量**，不调任何 UIA 函数 ⇒ 不需要
#   uiautomationcore、不需要窗口、不需要键鼠（进逻辑档）。
# ⚠ 真机部分（到底能不能写进豆包客户端 / Cursor）**不在本套**：
#   必须由产品进程内的 `POST /qst/window-ai/probe` 跑（要 UIA + 前台 + 真控件树）。
qst_add_selftest(WindowAiSelfTest
    tools/window_ai_selftest.cpp
    src/web_ai/window_ai_profile.cpp
    LIBS
        qst_utils
        user32
)



# 办公文档提取脚本：自检必须测**当前**脚本（否则会测到 build 目录里的旧副本）
add_custom_command(TARGET AiActionRouterSelfTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AiActionRouterSelfTest>/office"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tools/office/read_doc.ps1"
        "$<TARGET_FILE_DIR:AiActionRouterSelfTest>/office/read_doc.ps1"
    # （批 B 已删）这里原先还拷 TrackerVit 的 vit.onnx 给 locate_track 用例 —— 那条
    # 「本地跟住」链路（连同用例）批 B 已整体撤销，模型因此**没有任何读者**，拷贝已成孤儿。
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

# 窗口变速（变速齿轮）时钟换算：被测逻辑是纯 header
# （src/window_mode/time_scale_clock.h），不链任何库 —— 目标进程内的 IAT 补丁
# 那一半由 WindowModeSelfTest 的交互档经 FakeFocus_TimeScaleDiag 验。
qst_add_selftest(TimeScaleSelfTest
    tools/time_scale_selftest.cpp
)

# 桥接 JSON 边界契约（架构评估验收 A1）：锁住 saveSettings「提示成功但全部回滚」
# 那次 P0。被测逻辑已上收到 src/json_util.h（纯 header），所以只链 qst_utils，
# 不依赖引擎能独立链接——这样 B 段删掉 engine_link_stubs.cpp 后本 suite 仍成立。
qst_add_selftest(BridgeJsonSelfTest
    tools/bridge_json_selftest.cpp
    LIBS qst_utils
)

# 桥接命令契约（架构评估 #10 / 验收报告 §7.2 C）：双向校验
# src/webview/bridge_commands.h ↔ C++ 入站分派 ↔ ui/*.js 发送侧。
# 纯源码分析（读文件 + 正则），不链任何库。
qst_add_selftest(BridgeContractSelfTest
    tools/bridge_contract_selftest.cpp
)

# 引擎执行侧（架构评估验收 B3）：UI 钩子契约（默认跑）+ headless 跑
# Wait/Loop/VarCompute（--engine 显式打开）。
# 需要 B 段的依赖倒置已完成（引擎可脱离壳链接），否则这里会缺壳符号。
qst_add_selftest(ScriptRunnerSelfTest
    tools/script_runner_selftest.cpp
    LIBS
        qst_engine
        qst_desktop_tools
        qst_engine_ui_stubs
        Qst::WebView2
        ${QST_COMMON_LINK_LIBS}
)

qst_add_selftest(HotkeyStopSelfTest
    tools/hotkey_stop_selftest.cpp
)

# 悬浮球全屏隐藏判据（纯几何）：钉死「非全屏页面不得隐藏悬浮球」。
qst_add_selftest(FloatBallGeomSelfTest
    tools/float_ball_geom_selftest.cpp
)

# 叠层扣住鼠标捕获的兜底判据（纯逻辑 + 真消息队列）：
# 钉死「终止事件丢了必须自己收尾，绝不把用户的鼠标控制权留在我们手里」。
qst_add_selftest(OverlayInputGuardSelfTest
    tools/overlay_input_guard_selftest.cpp
    LIBS
        user32
)

# 专属热键作用域判据（纯逻辑）：钉死「作用域外不注册、不吞键」。
qst_add_selftest(HotkeyScopeSelfTest
    tools/hotkey_scope_selftest.cpp
)

# 悬浮球全屏判据「实机探针」：与产品共用同一份判据，需真实桌面会话，
# 输出给人看 —— **不进 $LogicSuites / $InteractiveSuites / CI 清单**。
qst_add_selftest(FloatBallProbe
    tools/float_ball_probe.cpp
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
    resources/selftest_version.rc
    LIBS user32 shell32 shlwapi winmm gdi32 qst_utils
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
        # （批 D 已删）这里原先还拷 TrackerVit 的 vit.onnx（698KB）：「定位之后的本地跟住」
        # 那条快路径批 B 已整体撤销 ⇒ **src/ 里没有任何读者**，把没人加载的模型打进产物
        # 只会给下一个人留一条重建跟踪器的线索。模型本体 + 实测文档 + 复现工具都留在仓里
        # （resources/models/、docs/local-detection-feasibility.md、tools/local_detection_bench/）。
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
            "${CMAKE_SOURCE_DIR}/resources/pause.wav"
            "$<TARGET_FILE_DIR:${target}>/pause.wav"
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
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/skills/agent/desktop.md"
            "$<TARGET_FILE_DIR:${target}>/skills/agent/desktop.md"
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

# ── QstScriptPlayer（导出 EXE 的运行时模板 → QstPlayer.exe）──────
# 导出时把「脚本包」追加到本 exe 尾部即得产物（见 src/script_package.h 的 payload 说明）。
# 刻意不依赖 WebView2 / ui/ / WebView2Fixed（558MB）—— 那些只有主程序用。
# DELAYLOAD opencv：没带找图组件的产物必须能正常启动（只是禁用找图 + 给出可读提示），
# 否则"回退 + 提示"根本无从谈起。
if(QST_BUILD_WEBVIEW_SHELL)
    add_executable(QstScriptPlayer WIN32
        resources/QstScriptPlayer.rc
        src/player/player_main.cpp
    )
    # 产物名 QstPlayer.exe：与主程序 QuickScriptTool.exe 明确区分，
    # 避免用户/杀软把导出的脚本误当主程序。
    set_target_properties(QstScriptPlayer PROPERTIES OUTPUT_NAME QstPlayer)
    target_compile_definitions(QstScriptPlayer PRIVATE
        UNICODE _UNICODE NOMINMAX
        QST_WEBVIEW_WITH_ENGINE=1
        QST_GDI_LEGACY=0
    )
    target_include_directories(QstScriptPlayer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/driver/qst_vhid
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(QstScriptPlayer PRIVATE
        qst_engine
        qst_desktop_tools
        qst_engine_ui_stubs
        ${QST_COMMON_LINK_LIBS}
    )
    if(MSVC)
        target_compile_options(QstScriptPlayer PRIVATE /W4 /permissive- /utf-8)
        target_sources(QstScriptPlayer PRIVATE resources/qst_uac.manifest)
        target_link_libraries(QstScriptPlayer PRIVATE delayimp)
        target_link_options(QstScriptPlayer PRIVATE
            "/DELAYLOAD:opencv_world4100.dll"
            "/MANIFESTUAC:NO"
        )
    endif()

    # 把模板放到 tools/player/，随产品分发。导出时只做「复制 + 追加数据」，
    # 所以 player 版本永远与主程序一致，也不会因网络/工具链问题失败。
    add_custom_command(TARGET QstScriptPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:QstScriptPlayer>/tools/player"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:QstScriptPlayer>"
            "$<TARGET_FILE_DIR:QstScriptPlayer>/tools/player/QstPlayer.exe"
        COMMENT "Copy QstPlayer.exe template into tools/player")
endif()

# ── 安装目录 Uninstall.exe（启动 Inno 卸载器并走 UAC）──────────
add_executable(QstUninstall WIN32 installer/qst_uninstall.cpp)
target_compile_definitions(QstUninstall PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(QstUninstall PRIVATE /W4 /permissive- /utf-8)
endif()
