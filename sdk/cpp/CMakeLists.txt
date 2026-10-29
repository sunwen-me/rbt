cmake_minimum_required(VERSION 3.22)

project(rbt-cpp-sdk VERSION 0.8.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Resolve every native layer explicitly: upstream's C++ config leaves its C
# and BB dependencies unversioned, so old *_DIR cache entries could mix ABIs.
find_package(iceoryx2-c 0.10.0 EXACT REQUIRED CONFIG)
find_package(iceoryx2-bb-cxx 0.10.0 EXACT REQUIRED CONFIG)
find_package(iceoryx2-cxx 0.10.0 EXACT REQUIRED CONFIG)

get_filename_component(RBT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(RBT_ICEORYX2_FORK_ROOT "${RBT_REPO_ROOT}/third_party/iceoryx2-rbt")
file(STRINGS "${RBT_ICEORYX2_FORK_ROOT}/SOURCE_DIFF_SHA256" _rbt_iox2_diff_line LIMIT_COUNT 1)
string(REGEX MATCH "^[0-9a-fA-F]+" RBT_ICEORYX2_SOURCE_DIFF_SHA256 "${_rbt_iox2_diff_line}")
string(LENGTH "${RBT_ICEORYX2_SOURCE_DIFF_SHA256}" _rbt_iox2_diff_length)
if(NOT _rbt_iox2_diff_length EQUAL 64)
    message(FATAL_ERROR "Canonical iceoryx2 source fingerprint is missing or invalid")
endif()

get_filename_component(
    RBT_ICEORYX2_PREFIX "${iceoryx2-cxx_DIR}/../../.." REALPATH
)
foreach(_rbt_native_component iceoryx2-c iceoryx2-bb-cxx)
    get_filename_component(_rbt_native_component_prefix
        "${${_rbt_native_component}_DIR}/../../.." REALPATH)
    if(NOT _rbt_native_component_prefix STREQUAL RBT_ICEORYX2_PREFIX)
        message(FATAL_ERROR "${_rbt_native_component} and iceoryx2-cxx must use the same maintained prefix")
    endif()
endforeach()
get_filename_component(_rbt_expected_native_headers
    "${RBT_ICEORYX2_PREFIX}/include/iceoryx2/v0.10.0" REALPATH)
foreach(_rbt_native_target iceoryx2-c::includes-only
        iceoryx2-bb-cxx::iceoryx2-bb-cxx iceoryx2-cxx::includes-only-cxx)
    get_target_property(_rbt_native_headers "${_rbt_native_target}" INTERFACE_INCLUDE_DIRECTORIES)
    get_filename_component(_rbt_native_headers "${_rbt_native_headers}" REALPATH)
    if(NOT _rbt_native_headers STREQUAL _rbt_expected_native_headers)
        message(FATAL_ERROR "Already imported native target ${_rbt_native_target} belongs to another baseline")
    endif()
endforeach()
execute_process(
    COMMAND bash "${RBT_REPO_ROOT}/scripts/check_iceoryx2_rbt_prefix.sh" "${RBT_ICEORYX2_PREFIX}"
    RESULT_VARIABLE _rbt_iox2_prefix_status
    OUTPUT_VARIABLE _rbt_iox2_prefix_stdout
    ERROR_VARIABLE _rbt_iox2_prefix_stderr
)
if(NOT _rbt_iox2_prefix_status EQUAL 0)
    message(FATAL_ERROR
        "iceoryx2 native prefix provenance check failed:\n"
        "${_rbt_iox2_prefix_stdout}${_rbt_iox2_prefix_stderr}"
    )
endif()

add_library(rbt-iox2 INTERFACE)
add_library(rbt::iox2 ALIAS rbt-iox2)
set_target_properties(rbt-iox2 PROPERTIES EXPORT_NAME iox2)
target_compile_features(rbt-iox2 INTERFACE cxx_std_20)
target_include_directories(rbt-iox2
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../generated/cpp>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rbt-iox2 INTERFACE iceoryx2-cxx::static-lib-cxx)

add_library(rbt-cpp STATIC src/runtime.cpp)
add_library(rbt::runtime ALIAS rbt-cpp)
set_target_properties(rbt-cpp PROPERTIES
    EXPORT_NAME runtime
    OUTPUT_NAME rbt_runtime
)
target_compile_features(rbt-cpp PUBLIC cxx_std_20)
target_include_directories(rbt-cpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../generated/cpp>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rbt-cpp PUBLIC rbt-iox2)

function(rbt_cpp_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

rbt_cpp_warnings(rbt-cpp)

add_executable(descriptor_tests tests/descriptor_tests.cpp)
target_link_libraries(descriptor_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(descriptor_tests)

add_executable(stream_compile tests/stream_compile.cpp)
target_link_libraries(stream_compile PRIVATE rbt-cpp)
rbt_cpp_warnings(stream_compile)

add_executable(pressure_tests tests/pressure_tests.cpp)
target_link_libraries(pressure_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(pressure_tests)

add_executable(effect_scope_tests tests/effect_scope_tests.cpp)
target_link_libraries(effect_scope_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(effect_scope_tests)

add_executable(waitset_tests tests/waitset_tests.cpp)
target_link_libraries(waitset_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(waitset_tests)

add_executable(runtime_tests tests/runtime_tests.cpp)
target_link_libraries(runtime_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(runtime_tests)

add_executable(rpc_tests tests/rpc_tests.cpp)
target_link_libraries(rpc_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(rpc_tests)

add_executable(time_tests tests/time_tests.cpp)
target_link_libraries(time_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(time_tests)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(host_clock_namespace_tests tests/host_clock_namespace_tests.cpp)
    target_link_libraries(host_clock_namespace_tests PRIVATE rbt-cpp)
    rbt_cpp_warnings(host_clock_namespace_tests)
endif()

add_executable(tf_math_tests tests/tf_math_tests.cpp)
target_link_libraries(tf_math_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(tf_math_tests)

add_executable(tf_tests tests/tf_tests.cpp)
target_link_libraries(tf_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(tf_tests)

add_test(NAME descriptor_tests COMMAND descriptor_tests)
add_test(NAME stream_compile COMMAND stream_compile)
add_test(NAME pressure_tests COMMAND pressure_tests)
add_test(NAME effect_scope_tests COMMAND effect_scope_tests)
add_test(NAME waitset_tests COMMAND waitset_tests)
add_test(NAME runtime_tests COMMAND runtime_tests)
add_test(NAME rpc_tests COMMAND rpc_tests)
add_test(NAME time_tests COMMAND time_tests)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME host_clock_namespace_tests COMMAND host_clock_namespace_tests)
endif()
add_test(NAME tf_math_tests COMMAND tf_math_tests)
add_test(NAME tf_tests COMMAND tf_tests)

add_executable(tf_transport_tests tests/tf_transport_tests.cpp)
target_link_libraries(tf_transport_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(tf_transport_tests)
add_test(NAME tf_transport_tests COMMAND tf_transport_tests)

add_executable(tf_component_tests tests/tf_component_tests.cpp)
target_link_libraries(tf_component_tests PRIVATE rbt-cpp)
rbt_cpp_warnings(tf_component_tests)
add_test(NAME tf_component_tests COMMAND tf_component_tests)

add_executable(tf_probe tools/tf_probe.cpp)
target_link_libraries(tf_probe PRIVATE rbt-cpp)
rbt_cpp_warnings(tf_probe)

# The IDL/code generator owns generated/cpp.  Keep the SDK and descriptor
# tests buildable while that output is absent, and add the demonstrations as
# soon as the generated headers are available.
set(RBT_CPP_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../generated/cpp)
set(RBT_CPP_SENSOR_HEADER ${RBT_CPP_GENERATED_DIR}/rbt/sensor.hpp)
set(RBT_CPP_CONTROL_HEADER ${RBT_CPP_GENERATED_DIR}/rbt/control.hpp)
set(RBT_CPP_MISMATCH_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../generated/mismatch/cpp)
set(RBT_CPP_MISMATCH_SENSOR_HEADER ${RBT_CPP_MISMATCH_GENERATED_DIR}/rbt/sensor.hpp)
if(EXISTS ${RBT_CPP_SENSOR_HEADER} AND EXISTS ${RBT_CPP_CONTROL_HEADER})
    add_executable(layout_cpp src/layout_cpp.cpp)
    target_link_libraries(layout_cpp PRIVATE rbt-cpp)
    rbt_cpp_warnings(layout_cpp)

    add_executable(interop_cpp src/interop_cpp.cpp)
    target_link_libraries(interop_cpp PRIVATE rbt-cpp)
    rbt_cpp_warnings(interop_cpp)
else()
    message(STATUS "rbt-cpp generated headers are absent; layout_cpp and interop_cpp are deferred")
endif()

if(EXISTS ${RBT_CPP_SENSOR_HEADER})
    add_executable(python_component_viewer ../../examples/cpp/python-viewer/main.cpp)
    target_link_libraries(python_component_viewer PRIVATE rbt-cpp)
    rbt_cpp_warnings(python_component_viewer)
endif()

set(RBT_CPP_DEMO_HEADER ${RBT_CPP_GENERATED_DIR}/rbt/demo.hpp)
if(EXISTS ${RBT_CPP_DEMO_HEADER})
    add_executable(camera_component ../../examples/cpp/camera/main.cpp)
    target_link_libraries(camera_component PRIVATE rbt-cpp)
    rbt_cpp_warnings(camera_component)

    add_executable(detector_component ../../examples/cpp/detector/main.cpp)
    target_link_libraries(detector_component PRIVATE rbt-cpp)
    rbt_cpp_warnings(detector_component)

    add_executable(viewer_component ../../examples/cpp/viewer/main.cpp)
    target_link_libraries(viewer_component PRIVATE rbt-cpp)
    rbt_cpp_warnings(viewer_component)

    add_executable(rpc_interop tools/rpc_interop.cpp)
    target_link_libraries(rpc_interop PRIVATE rbt-cpp)
    rbt_cpp_warnings(rpc_interop)
endif()

if(EXISTS ${RBT_CPP_MISMATCH_SENSOR_HEADER})
    add_executable(abi_mismatch_cpp src/abi_mismatch_cpp.cpp)
    target_include_directories(abi_mismatch_cpp BEFORE PRIVATE ${RBT_CPP_MISMATCH_GENERATED_DIR})
    target_link_libraries(abi_mismatch_cpp PRIVATE rbt-cpp)
    rbt_cpp_warnings(abi_mismatch_cpp)
else()
    message(STATUS "rbt-cpp mismatch generated header is absent; abi_mismatch_cpp is deferred")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(rbt_time_camera ../../examples/09-time-camera/main.cpp)
    target_link_libraries(rbt_time_camera PRIVATE rbt-cpp)
    rbt_cpp_warnings(rbt_time_camera)

    add_executable(rbt_time_camera_consumer ../../examples/09-time-camera/consumer.cpp)
    target_link_libraries(rbt_time_camera_consumer PRIVATE rbt-cpp)
    rbt_cpp_warnings(rbt_time_camera_consumer)
endif()

install(
    TARGETS rbt-cpp rbt-iox2
    EXPORT rbtTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/rbt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    DIRECTORY ../../generated/cpp/rbt
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
    PATTERN "rpc.hpp" EXCLUDE
)
install(
    EXPORT rbtTargets
    FILE rbtTargets.cmake
    NAMESPACE rbt::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt
)
configure_package_config_file(
    cmake/rbtConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rbtConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/rbtConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/rbtConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/rbtConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt
)
install(
    FILES
        ../../LICENSE-APACHE
        ../../LICENSE-MIT
        ../../THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rbt
)
