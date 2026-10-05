cmake_minimum_required(VERSION 3.20)
project(qst_yolo_bench CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OpenCV_DIR "D:/OpenCV/opencv/build" CACHE PATH "")
find_package(OpenCV REQUIRED)

add_executable(bench main.cpp)
target_compile_options(bench PRIVATE /utf-8)
target_include_directories(bench PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(bench PRIVATE ${OpenCV_LIBS})

add_custom_command(TARGET bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:bench>" "${CMAKE_CURRENT_SOURCE_DIR}/bench.exe")
