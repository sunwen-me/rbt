cmake_minimum_required(VERSION 3.22)

project(rbt_base_controller VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

option(RBT_BASE_CONTROLLER_SANITIZE "Enable address and undefined behavior sanitizers" OFF)

find_package(Threads REQUIRED)
find_package(rbt_hardware CONFIG REQUIRED)
find_package(rbt_navigation CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

function(rbt_base_controller_warnings target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(RBT_BASE_CONTROLLER_SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

set(DDSM_CORE_SOURCES
    src/ddsm_control.cpp
    src/ddsm_feedback_buffer.cpp
    src/ddsm_motor.cpp
    src/ddsm_protocol.cpp
    src/diff_drive_kinematics.cpp
    src/odometry.cpp
    src/pid_controller.cpp
    src/serial_comm.cpp
    src/yaw_auto_tuner.cpp
)

add_library(rbt_base_controller_core STATIC ${DDSM_CORE_SOURCES})
target_include_directories(rbt_base_controller_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(rbt_base_controller_core PUBLIC cxx_std_20)
target_link_libraries(rbt_base_controller_core PUBLIC Threads::Threads)
rbt_base_controller_warnings(rbt_base_controller_core)

add_library(rbt_base_controller_config STATIC src/native_config.cpp)
target_include_directories(rbt_base_controller_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(rbt_base_controller_config PUBLIC cxx_std_20)
target_compile_definitions(rbt_base_controller_config PRIVATE
    RBT_BASE_CONTROLLER_SOURCE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/config/ddsm_robot_current.yaml")
target_link_libraries(rbt_base_controller_config PUBLIC yaml-cpp::yaml-cpp)
rbt_base_controller_warnings(rbt_base_controller_config)

add_executable(rbt_base_controller src/native_main.cpp)
target_include_directories(rbt_base_controller PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(rbt_base_controller PRIVATE cxx_std_20)
target_link_libraries(rbt_base_controller PRIVATE
    rbt_base_controller_core
    rbt_base_controller_config
    rbt_hardware::messages
    rbt_navigation::messages
    rbt::runtime
    yaml-cpp::yaml-cpp
    Threads::Threads
)
rbt_base_controller_warnings(rbt_base_controller)
set_target_properties(rbt_base_controller PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

install(TARGETS rbt_base_controller RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/ddsm_robot_current.yaml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt_base_controller/config)
install(FILES README.md NOTICE.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(BUILD_TESTING)
    function(add_ddsm_test target source)
        add_executable(${target} ${source})
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/test)
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_compile_options(${target} PRIVATE -UNDEBUG)
        rbt_base_controller_warnings(${target})
    endfunction()

    add_ddsm_test(test_diff_drive_kinematics test/test_diff_drive_kinematics.cpp)
    target_link_libraries(test_diff_drive_kinematics PRIVATE rbt_base_controller_core)
    add_test(NAME diff_drive_kinematics_tests COMMAND test_diff_drive_kinematics)

    add_ddsm_test(test_pid_controller test/test_pid_controller.cpp)
    target_link_libraries(test_pid_controller PRIVATE rbt_base_controller_core)
    add_test(NAME pid_controller_tests COMMAND test_pid_controller)

    add_ddsm_test(test_ddsm_protocol test/test_ddsm_protocol.cpp)
    target_link_libraries(test_ddsm_protocol PRIVATE rbt_base_controller_core)
    add_test(NAME ddsm_protocol_tests COMMAND test_ddsm_protocol)

    add_ddsm_test(test_control_planner test/test_control_planner.cpp)
    target_link_libraries(test_control_planner PRIVATE rbt_base_controller_core)
    add_test(NAME control_planner_tests COMMAND test_control_planner)

    add_ddsm_test(test_feedback_buffer test/test_feedback_buffer.cpp)
    target_link_libraries(test_feedback_buffer PRIVATE rbt_base_controller_core)
    add_test(NAME feedback_buffer_tests COMMAND test_feedback_buffer)

    add_ddsm_test(test_serial_transport test/test_serial_transport.cpp)
    target_link_libraries(test_serial_transport PRIVATE rbt_base_controller_core util Threads::Threads)
    add_test(NAME serial_transport_tests COMMAND test_serial_transport)

    add_ddsm_test(test_yaw_auto_tuner test/test_yaw_auto_tuner.cpp)
    target_link_libraries(test_yaw_auto_tuner PRIVATE rbt_base_controller_core)
    add_test(NAME yaw_auto_tuner_tests COMMAND test_yaw_auto_tuner)

    add_ddsm_test(test_native_safety test/test_native_safety.cpp)
    target_compile_definitions(test_native_safety PRIVATE
        RBT_BASE_CONTROLLER_SOURCE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/config/ddsm_robot_current.yaml")
    target_link_libraries(test_native_safety PRIVATE
        rbt_base_controller_config
        rbt_base_controller_core
        rbt_hardware::messages
        rbt_navigation::messages
        rbt::runtime
        Threads::Threads
    )
    add_test(NAME native_safety_tests COMMAND test_native_safety)
endif()
