cmake_minimum_required(VERSION 3.22)

project(rbt-cpp-detector-example LANGUAGES CXX)

find_package(rbt CONFIG REQUIRED)

add_executable(rbt-cpp-detector main.cpp)
target_compile_features(rbt-cpp-detector PRIVATE cxx_std_20)
target_compile_options(rbt-cpp-detector PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(rbt-cpp-detector PRIVATE rbt::runtime rbt::iox2)
