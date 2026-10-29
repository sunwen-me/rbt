cmake_minimum_required(VERSION 3.22)

project(rbt-orbbec-bench-consumer VERSION 0.1.0 LANGUAGES CXX)

find_package(rbt CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(rbt-orbbec-bench-consumer
    rbt_consumer.cpp
    ../src/core.cpp
    ../src/config.cpp
)
target_compile_features(rbt-orbbec-bench-consumer PRIVATE cxx_std_20)
target_compile_options(rbt-orbbec-bench-consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(rbt-orbbec-bench-consumer PRIVATE
    ../include
    ../generated
)
target_link_libraries(rbt-orbbec-bench-consumer PRIVATE rbt::runtime rbt::iox2 Threads::Threads)

get_filename_component(_rbt_sdk_prefix "${rbt_DIR}/../../../" ABSOLUTE)
set_target_properties(rbt-orbbec-bench-consumer PROPERTIES
    BUILD_RPATH "${_rbt_sdk_prefix}/lib"
    INSTALL_RPATH "${_rbt_sdk_prefix}/lib"
)
