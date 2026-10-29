cmake_minimum_required(VERSION 3.22)

project(rbt-cpp-viewer-example LANGUAGES CXX)

find_package(rbt CONFIG REQUIRED)

add_executable(rbt-cpp-viewer main.cpp)
target_compile_features(rbt-cpp-viewer PRIVATE cxx_std_20)
target_compile_options(rbt-cpp-viewer PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(rbt-cpp-viewer PRIVATE rbt::runtime rbt::iox2)
