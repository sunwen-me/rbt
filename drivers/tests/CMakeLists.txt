cmake_minimum_required(VERSION 3.22)
project(rbt_hardware_ipc_tests LANGUAGES CXX)

include(CTest)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(Threads REQUIRED)
find_package(rbt CONFIG REQUIRED)
find_package(rbt_hardware CONFIG REQUIRED)
find_package(rbt_navigation CONFIG REQUIRED)

add_executable(rbt_hardware_ipc_fixture hardware_ipc_fixture.cpp)
target_compile_features(rbt_hardware_ipc_fixture PRIVATE cxx_std_20)
target_compile_options(rbt_hardware_ipc_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(rbt_hardware_ipc_fixture PRIVATE
    rbt_hardware::messages rbt_navigation::messages rbt::runtime Threads::Threads)

set(RBT_TEST_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(RBT_BASE_CONTROLLER_BUILD_DIR "${RBT_TEST_REPO_ROOT}/target/base-controller-build"
    CACHE PATH "Build directory containing rbt_base_controller")
set(RBT_DM_IMU_BUILD_DIR "${RBT_TEST_REPO_ROOT}/target/dm-imu-build"
    CACHE PATH "Build directory containing rbt_dm_imu")
set(RBT_NAVIGATION_BUILD_DIR "${RBT_TEST_REPO_ROOT}/target/navigation-build"
    CACHE PATH "Build directory containing rbt_navigation_runtime and rbt_velocity_guard")

if(BUILD_TESTING)
    foreach(scenario pipeline navigation navigation_crash)
        add_test(NAME hardware_ipc_${scenario}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/hardware_ipc_test.py
                --scenario ${scenario}
                --fixture $<TARGET_FILE:rbt_hardware_ipc_fixture>
                --base-build-dir ${RBT_BASE_CONTROLLER_BUILD_DIR}
                --imu-build-dir ${RBT_DM_IMU_BUILD_DIR}
                --navigation-build-dir ${RBT_NAVIGATION_BUILD_DIR}
                --output-dir ${CMAKE_CURRENT_BINARY_DIR}/evidence/${scenario})
    endforeach()
    set_tests_properties(hardware_ipc_pipeline PROPERTIES TIMEOUT 40)
    set_tests_properties(hardware_ipc_navigation PROPERTIES TIMEOUT 35)
    set_tests_properties(hardware_ipc_navigation_crash PROPERTIES TIMEOUT 35)
endif()
