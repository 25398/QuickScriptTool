# 独立小工程：只编译 race_test + 真实的 fake_focus_hook.cpp（+ MinHook），
# 用来复现/回归「临时还原 + 重写跳转」的多线程写入竞态。
# 刻意不进主工程：它要跑多线程压力，不适合放进 CI 的逻辑档。
cmake_minimum_required(VERSION 3.20)
project(HookRace C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# tools/verify/hook_race -> 仓库根
get_filename_component(QST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(MH "${QST_SRC}/third_party/minhook")
add_library(race_minhook STATIC
    "${MH}/src/buffer.c"
    "${MH}/src/hook.c"
    "${MH}/src/trampoline.c"
    "${MH}/src/hde/hde32.c"
    "${MH}/src/hde/hde64.c")
target_include_directories(race_minhook PUBLIC
    "${MH}/include" "${MH}/src" "${MH}/src/hde")
if(MSVC)
    target_compile_options(race_minhook PRIVATE /W3 /utf-8)
endif()

add_executable(race_test
    "${CMAKE_CURRENT_SOURCE_DIR}/race_test.cpp"
    "${QST_SRC}/src/window_mode/fake_focus/fake_focus_hook.cpp")

target_include_directories(race_test PRIVATE
    "${QST_SRC}/src/window_mode/fake_focus")

target_link_libraries(race_test PRIVATE race_minhook)

if(MSVC)
    target_compile_options(race_test PRIVATE /W4 /utf-8)
    # 必须关优化：要让被钩函数保持足够长的真实函数体，且不被内联。
    target_compile_options(race_test PRIVATE /Od)
endif()

target_link_libraries(race_test PRIVATE user32)
