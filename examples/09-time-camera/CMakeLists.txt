cmake_minimum_required(VERSION 3.22)

project(rbt-time-camera-example LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The physical camera example requires Linux V4L2")
endif()

find_package(rbt CONFIG REQUIRED)

add_executable(rbt_time_camera main.cpp)
add_executable(rbt_time_camera_consumer consumer.cpp)

foreach(target rbt_time_camera rbt_time_camera_consumer)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(${target} PRIVATE rbt::runtime rbt::iox2)
endforeach()
