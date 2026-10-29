cmake_minimum_required(VERSION 3.22)
project(rbt_navigation VERSION 0.1.0 LANGUAGES CXX)
include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)
option(RBT_NAVIGATION_NATIVE "Build RBT native navigation runtime and independent velocity guard" ON)
option(RBT_NAVIGATION_MAP_IO "Build standard YAML + PGM/PNG map loading" ON)
set(RBT_NAVIGATION_SANITIZER "" CACHE STRING "Sanitizer: address or undefined")
if(RBT_NAVIGATION_SANITIZER)
    if(NOT RBT_NAVIGATION_SANITIZER MATCHES "^(address|undefined)$")
        message(FATAL_ERROR "Unsupported sanitizer")
    endif()
    add_compile_options(-fsanitize=${RBT_NAVIGATION_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${RBT_NAVIGATION_SANITIZER})
endif()
function(nav_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_include_directories(${target} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
endfunction()
add_library(rbt_navigation_core STATIC src/grid_map.cpp src/a_star.cpp src/pure_pursuit.cpp src/dynamic_window.cpp src/vector_pursuit.cpp
    src/navigation_manager.cpp src/safe_base.cpp src/obstacle_layer.cpp src/footprint.cpp
    src/feasible_path_handler.cpp src/state_lattice.cpp)
nav_target(rbt_navigation_core)
add_library(rbt_navigation::core ALIAS rbt_navigation_core)
set_target_properties(rbt_navigation_core PROPERTIES EXPORT_NAME core)
target_link_libraries(rbt_navigation_core PUBLIC Threads::Threads)
if(RBT_NAVIGATION_MAP_IO)
    find_package(yaml-cpp CONFIG REQUIRED)
    find_package(PNG REQUIRED)
    if(TARGET yaml-cpp::yaml-cpp)
        set(nav_yaml_target yaml-cpp::yaml-cpp)
    elseif(TARGET yaml-cpp)
        set(nav_yaml_target yaml-cpp)
    else()
        message(FATAL_ERROR "yaml-cpp package exports no supported target")
    endif()
    add_library(rbt_navigation_map_io STATIC src/map_io.cpp)
    nav_target(rbt_navigation_map_io)
    add_library(rbt_navigation::map_io ALIAS rbt_navigation_map_io)
    set_target_properties(rbt_navigation_map_io PROPERTIES EXPORT_NAME map_io)
    target_link_libraries(rbt_navigation_map_io PRIVATE ${nav_yaml_target} PNG::PNG)
    add_library(rbt_navigation_config_io STATIC src/geometry_io.cpp src/lattice_io.cpp)
    nav_target(rbt_navigation_config_io)
    add_library(rbt_navigation::config_io ALIAS rbt_navigation_config_io)
    set_target_properties(rbt_navigation_config_io PROPERTIES EXPORT_NAME config_io)
    target_link_libraries(rbt_navigation_config_io PUBLIC rbt_navigation_core PRIVATE ${nav_yaml_target})
    if(BUILD_TESTING)
        foreach(name geometry_io state_lattice)
            add_executable(navigation_${name}_test tests/${name}_test.cpp)
            nav_target(navigation_${name}_test)
            target_compile_options(navigation_${name}_test PRIVATE -UNDEBUG)
            target_link_libraries(navigation_${name}_test PRIVATE rbt_navigation_config_io)
            add_test(NAME navigation_${name} COMMAND navigation_${name}_test)
            set_tests_properties(navigation_${name} PROPERTIES TIMEOUT 60)
        endforeach()
        add_executable(rbt_navigation_pursuit_benchmark tests/pursuit_benchmark.cpp)
        nav_target(rbt_navigation_pursuit_benchmark)
        target_link_libraries(rbt_navigation_pursuit_benchmark PRIVATE rbt_navigation_config_io)
        add_executable(rbt_navigation_pursuit_physics_bridge tests/pursuit_physics_bridge.cpp)
        nav_target(rbt_navigation_pursuit_physics_bridge)
        target_link_libraries(rbt_navigation_pursuit_physics_bridge PRIVATE rbt_navigation_config_io)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME navigation_pursuit_benchmark_smoke COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pursuit_benchmark_smoke.py
            --binary $<TARGET_FILE:rbt_navigation_pursuit_benchmark>
            --geometry ${CMAKE_CURRENT_SOURCE_DIR}/config/front-drive-geometry.yaml
            --lattice ${CMAKE_CURRENT_SOURCE_DIR}/config/lattice/diff-5cm-0_5m.json
            --output ${CMAKE_CURRENT_BINARY_DIR}/pursuit-benchmark-smoke)
        set_tests_properties(navigation_pursuit_benchmark_smoke PROPERTIES TIMEOUT 60 LABELS "experiment")
        add_test(NAME navigation_pursuit_physics_bridge COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pursuit_physics_bridge.py
            --binary $<TARGET_FILE:rbt_navigation_pursuit_physics_bridge>
            --geometry ${CMAKE_CURRENT_SOURCE_DIR}/config/front-drive-geometry.yaml)
        set_tests_properties(navigation_pursuit_physics_bridge PROPERTIES TIMEOUT 60 LABELS "experiment")
        add_executable(navigation_map_io_test tests/map_io_test.cpp)
        nav_target(navigation_map_io_test)
        target_compile_options(navigation_map_io_test PRIVATE -UNDEBUG)
        target_link_libraries(navigation_map_io_test PRIVATE
            rbt_navigation_map_io rbt_navigation_core PNG::PNG)
        add_test(NAME navigation_map_io COMMAND navigation_map_io_test)
        set_tests_properties(navigation_map_io PROPERTIES TIMEOUT 30)
    endif()
    # Keep image/YAML dependencies out of existing message-only SDK consumers.
    install(TARGETS rbt_navigation_map_io EXPORT rbtNavigationMapIOTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT rbtNavigationMapIOTargets NAMESPACE rbt_navigation::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt_navigation)
    install(TARGETS rbt_navigation_config_io EXPORT rbtNavigationConfigIOTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT rbtNavigationConfigIOTargets NAMESPACE rbt_navigation::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt_navigation)
endif()
add_executable(rbt_navigation_sim tests/simulation_test.cpp)
nav_target(rbt_navigation_sim)
target_link_libraries(rbt_navigation_sim PRIVATE rbt_navigation_core)
if(BUILD_TESTING)
    foreach(name core map planner controller dynamic_window vector_pursuit manager safety velocity_guard obstacle_layer footprint lattice_tracking)
        add_executable(navigation_${name}_test tests/${name}_test.cpp)
        nav_target(navigation_${name}_test)
        target_link_libraries(navigation_${name}_test PRIVATE rbt_navigation_core)
        target_compile_options(navigation_${name}_test PRIVATE -UNDEBUG)
        add_test(NAME navigation_${name} COMMAND navigation_${name}_test)
        set_tests_properties(navigation_${name} PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME navigation_simulation COMMAND rbt_navigation_sim)
    set_tests_properties(navigation_simulation PROPERTIES TIMEOUT 60)
endif()
if(RBT_NAVIGATION_NATIVE)
    find_package(rbt CONFIG REQUIRED)
    # The external base package only needs the generated wire contract and SDK,
    # without linking navigation algorithms or the guard implementation.
    add_library(rbt_navigation_messages INTERFACE)
    add_library(rbt_navigation::messages ALIAS rbt_navigation_messages)
    set_target_properties(rbt_navigation_messages PROPERTIES EXPORT_NAME messages)
    target_compile_features(rbt_navigation_messages INTERFACE cxx_std_20)
    target_include_directories(rbt_navigation_messages INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated/cpp>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(rbt_navigation_messages INTERFACE rbt::iox2)
    add_library(rbt_navigation_native STATIC src/native.cpp)
    add_library(rbt_navigation::native ALIAS rbt_navigation_native)
    set_target_properties(rbt_navigation_native PROPERTIES EXPORT_NAME native)
    nav_target(rbt_navigation_native)
    target_include_directories(rbt_navigation_native PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated/cpp>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(rbt_navigation_native PUBLIC rbt_navigation_core rbt_navigation_messages rbt::runtime)
    add_library(rbt_navigation_lio STATIC src/lio_bridge.cpp)
    nav_target(rbt_navigation_lio)
    target_link_libraries(rbt_navigation_lio PUBLIC rbt_navigation_native)
    set_target_properties(rbt_navigation_lio PROPERTIES EXPORT_NAME lio)
    add_executable(rbt_navigation_lio_bridge src/lio_bridge_main.cpp)
    nav_target(rbt_navigation_lio_bridge)
    target_link_libraries(rbt_navigation_lio_bridge PRIVATE rbt_navigation_lio)
    set_target_properties(rbt_navigation_lio_bridge PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}" INSTALL_RPATH_USE_LINK_PATH TRUE)
    install(TARGETS rbt_navigation_lio EXPORT rbtNavigationTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(TARGETS rbt_navigation_lio_bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(BUILD_TESTING)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_executable(navigation_native_test tests/native_test.cpp)
        nav_target(navigation_native_test)
        target_link_libraries(navigation_native_test PRIVATE rbt_navigation_native)
        add_test(NAME navigation_native_adapter COMMAND navigation_native_test)
        set_tests_properties(navigation_native_adapter PROPERTIES TIMEOUT 10)
        foreach(name lio_bridge lio_receipt_clock lio_output)
            add_executable(navigation_${name}_test tests/${name}_test.cpp)
            nav_target(navigation_${name}_test)
            target_compile_options(navigation_${name}_test PRIVATE -UNDEBUG)
            target_link_libraries(navigation_${name}_test PRIVATE rbt_navigation_lio)
            add_test(NAME navigation_${name} COMMAND navigation_${name}_test)
            set_tests_properties(navigation_${name} PROPERTIES TIMEOUT 15)
        endforeach()
        add_executable(rbt_navigation_lio_replay_fixture tests/lio_replay_fixture.cpp)
        nav_target(rbt_navigation_lio_replay_fixture)
        target_link_libraries(rbt_navigation_lio_replay_fixture PRIVATE rbt_navigation_lio)
        add_executable(rbt_navigation_lio_source_probe tests/lio_source_probe.cpp)
        nav_target(rbt_navigation_lio_source_probe)
        target_link_libraries(rbt_navigation_lio_source_probe PRIVATE rbt_navigation_lio)
        foreach(scenario obstacle avoidance invalid_localization stale future foreign_clock source_crash bridge_crash wrong_provider)
            add_test(NAME navigation_lio_ipc_${scenario} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/lio_integration.py
                --build-dir ${CMAKE_CURRENT_BINARY_DIR} --scenario ${scenario}
                --output-dir ${CMAKE_CURRENT_BINARY_DIR}/lio-evidence)
            set_tests_properties(navigation_lio_ipc_${scenario} PROPERTIES
                TIMEOUT 40 LABELS "native;lio")
        endforeach()
        # This scenario validates a complete moving closed loop against wall
        # time; isolate it from other process-heavy IPC/sanitizer scenarios.
        set_tests_properties(navigation_lio_ipc_avoidance PROPERTIES RUN_SERIAL TRUE)
        add_executable(rbt_navigation_native_fixture tests/native_fixture.cpp)
        nav_target(rbt_navigation_native_fixture)
        target_link_libraries(rbt_navigation_native_fixture PRIVATE rbt_navigation_native)
        add_executable(rbt_navigation_base_sim tests/base_controller_sim.cpp)
        nav_target(rbt_navigation_base_sim)
        target_link_libraries(rbt_navigation_base_sim PRIVATE rbt_navigation_native)
        add_executable(rbt_navigation_telemetry_recorder tests/telemetry_recorder.cpp)
        nav_target(rbt_navigation_telemetry_recorder)
        target_link_libraries(rbt_navigation_telemetry_recorder PRIVATE rbt_navigation_native)
        foreach(scenario success obstacle cancel dropout watchdog estop guard_crash expired authority cmd_vel base_expired)
            add_test(NAME navigation_ipc_${scenario} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/native_integration.py
                --build-dir ${CMAKE_CURRENT_BINARY_DIR} --scenario ${scenario}
                --output-dir ${CMAKE_CURRENT_BINARY_DIR}/native-evidence/${scenario})
            set_tests_properties(navigation_ipc_${scenario} PROPERTIES TIMEOUT 45)
        endforeach()
        add_test(NAME navigation_ipc_rpp_success COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/native_integration.py
            --build-dir ${CMAKE_CURRENT_BINARY_DIR} --scenario success --controller rpp
            --output-dir ${CMAKE_CURRENT_BINARY_DIR}/native-evidence/rpp-success)
        set_tests_properties(navigation_ipc_rpp_success PROPERTIES TIMEOUT 45)
        if(RBT_NAVIGATION_MAP_IO)
            add_executable(rbt_navigation_lattice_fixture tests/lattice_native_fixture.cpp)
            nav_target(rbt_navigation_lattice_fixture)
            target_link_libraries(rbt_navigation_lattice_fixture PRIVATE
                rbt_navigation_native rbt_navigation_config_io)
            foreach(scenario turn reverse tail_sweep)
                add_test(NAME navigation_lattice_ipc_${scenario} COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/lattice_integration.py
                    --build-dir ${CMAKE_CURRENT_BINARY_DIR} --scenario ${scenario}
                    --output-dir ${CMAKE_CURRENT_BINARY_DIR}/lattice-evidence)
                set_tests_properties(navigation_lattice_ipc_${scenario} PROPERTIES
                    TIMEOUT 100 LABELS "native;lattice" RUN_SERIAL TRUE)
            endforeach()
            add_executable(rbt_navigation_map_demo_fixture tests/map_demo_fixture.cpp)
            nav_target(rbt_navigation_map_demo_fixture)
            target_link_libraries(rbt_navigation_map_demo_fixture PRIVATE
                rbt_navigation_native rbt_navigation_map_io)
            foreach(scenario obstacle map_dropout)
                add_test(NAME navigation_ipc_file_map_${scenario} COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/native_integration.py
                    --build-dir ${CMAKE_CURRENT_BINARY_DIR} --scenario ${scenario}
                    --map-file ${CMAKE_CURRENT_SOURCE_DIR}/maps/simulation_obstacle.yaml
                    --output-dir ${CMAKE_CURRENT_BINARY_DIR}/native-evidence/file-map-${scenario})
                set_tests_properties(navigation_ipc_file_map_${scenario} PROPERTIES TIMEOUT 45)
            endforeach()
            foreach(scenario rooms warehouse)
                add_test(NAME navigation_ipc_complex_${scenario} COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/complex_simulation.py
                    --build-dir ${CMAKE_CURRENT_BINARY_DIR} --scenario ${scenario} --no-render
                    --output-dir ${CMAKE_CURRENT_BINARY_DIR}/complex-evidence)
                set_tests_properties(navigation_ipc_complex_${scenario} PROPERTIES
                    TIMEOUT 120 LABELS "extended;native" RUN_SERIAL TRUE)
            endforeach()
        endif()
    endif()
    add_executable(rbt_navigation_runtime src/runtime_main.cpp)
    if(RBT_NAVIGATION_MAP_IO)
        target_link_libraries(rbt_navigation_runtime PRIVATE rbt_navigation_config_io)
        target_compile_definitions(rbt_navigation_runtime PRIVATE RBT_NAVIGATION_CONFIG_IO=1)
        file(RELATIVE_PATH RBT_NAVIGATION_DEFAULT_GEOMETRY_INSTALL_RELATIVE
            "${CMAKE_INSTALL_FULL_BINDIR}"
            "${CMAKE_INSTALL_FULL_DATADIR}/rbt_navigation/config/front-drive-geometry.yaml")
        target_compile_definitions(rbt_navigation_runtime PRIVATE
            "RBT_NAVIGATION_DEFAULT_GEOMETRY_INSTALL_RELATIVE=\"${RBT_NAVIGATION_DEFAULT_GEOMETRY_INSTALL_RELATIVE}\"")
        file(GENERATE
            OUTPUT "$<TARGET_FILE_DIR:rbt_navigation_runtime>/config/front-drive-geometry.yaml"
            INPUT "${CMAKE_CURRENT_SOURCE_DIR}/config/front-drive-geometry.yaml")
        if(BUILD_TESTING)
            add_test(NAME navigation_default_geometry COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_default_geometry.py
                --binary $<TARGET_FILE:rbt_navigation_runtime>
                --geometry ${CMAKE_CURRENT_SOURCE_DIR}/config/front-drive-geometry.yaml
                --install-relative ${RBT_NAVIGATION_DEFAULT_GEOMETRY_INSTALL_RELATIVE})
            set_tests_properties(navigation_default_geometry PROPERTIES TIMEOUT 30 LABELS "native")
        endif()
    endif()
    add_executable(rbt_velocity_guard src/velocity_guard_main.cpp)
    foreach(target rbt_navigation_runtime rbt_velocity_guard)
        nav_target(${target})
        target_link_libraries(${target} PRIVATE rbt_navigation_native)
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
            INSTALL_RPATH_USE_LINK_PATH TRUE)
    endforeach()
    install(TARGETS rbt_navigation_runtime rbt_velocity_guard RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(RBT_NAVIGATION_MAP_IO)
        add_executable(rbt_navigation_map_server src/map_server_main.cpp)
        nav_target(rbt_navigation_map_server)
        target_link_libraries(rbt_navigation_map_server PRIVATE
            rbt_navigation_native rbt_navigation_map_io)
        set_target_properties(rbt_navigation_map_server PROPERTIES
            INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}" INSTALL_RPATH_USE_LINK_PATH TRUE)
        install(TARGETS rbt_navigation_map_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    install(TARGETS rbt_navigation_native rbt_navigation_messages EXPORT rbtNavigationTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(TARGETS rbt_navigation_core EXPORT rbtNavigationTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS rbt_navigation_sim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(EXPORT rbtNavigationTargets NAMESPACE rbt_navigation:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt_navigation)
configure_package_config_file(cmake/rbt_navigationConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rbt_navigationConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt_navigation)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/rbt_navigationConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rbt_navigationConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rbt_navigationConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt_navigation)
install(DIRECTORY include/rbt_navigation DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES generated/cpp/rbt/navwire.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rbt)
install(DIRECTORY types generated/descriptors generated/rust DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt_navigation)
install(DIRECTORY config docs licenses DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt_navigation)
install(DIRECTORY maps DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt_navigation)
install(DIRECTORY integrations DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt_navigation)
install(FILES NOTICE.md README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt_navigation)
