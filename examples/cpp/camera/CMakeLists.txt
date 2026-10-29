cmake_minimum_required(VERSION 3.22)

project(rbt-cpp-camera-example LANGUAGES CXX)

find_package(rbt CONFIG REQUIRED)

add_executable(rbt-cpp-camera main.cpp)
target_compile_features(rbt-cpp-camera PRIVATE cxx_std_20)
target_compile_options(rbt-cpp-camera PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(rbt-cpp-camera PRIVATE rbt::runtime rbt::iox2)
