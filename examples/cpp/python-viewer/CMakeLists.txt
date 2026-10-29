cmake_minimum_required(VERSION 3.22)

project(rbt-cpp-python-viewer-example LANGUAGES CXX)

find_package(rbt CONFIG REQUIRED)

add_executable(python_component_viewer main.cpp)
target_compile_features(python_component_viewer PRIVATE cxx_std_20)
target_compile_options(python_component_viewer PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(python_component_viewer PRIVATE rbt::runtime rbt::iox2)
