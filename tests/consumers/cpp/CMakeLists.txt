cmake_minimum_required(VERSION 3.22)
project(rbt-installed-cpp-consumer LANGUAGES CXX)

find_package(rbt CONFIG REQUIRED)

add_executable(rbt-installed-cpp-consumer main.cpp)
target_compile_features(rbt-installed-cpp-consumer PRIVATE cxx_std_20)
target_compile_options(rbt-installed-cpp-consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(
    rbt-installed-cpp-consumer
    PRIVATE
        rbt::runtime
        rbt::iox2
)
