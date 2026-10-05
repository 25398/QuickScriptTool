# 负对照工程：用**修复前**的 fake_focus_hook.h/.cpp（本目录内的原样副本）构建
# 旧实现测试，证明 run_hook_race.py 的断言真的会红。
cmake_minimum_required(VERSION 3.20)
project(HookRaceLegacy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(race_test_legacy
    "${CMAKE_CURRENT_SOURCE_DIR}/race_test_legacy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/fake_focus_hook.cpp")

target_include_directories(race_test_legacy PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(race_test_legacy PRIVATE /W4 /utf-8)
    target_compile_options(race_test_legacy PRIVATE /Od)
endif()

target_link_libraries(race_test_legacy PRIVATE user32)
