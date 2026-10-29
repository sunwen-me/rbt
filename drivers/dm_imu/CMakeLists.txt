cmake_minimum_required(VERSION 3.22)
project(rbt-dm-imu VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(rbt CONFIG REQUIRED)
find_package(rbt_hardware CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
if(NOT TARGET yaml-cpp::yaml-cpp)
    if(TARGET yaml-cpp)
        add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
    else()
        message(FATAL_ERROR "yaml-cpp package exports no supported target")
    endif()
endif()

set(RBT_DM_IMU_SANITIZERS "" CACHE STRING "Optional comma-separated C++ sanitizers (address,undefined)")
set_property(CACHE RBT_DM_IMU_SANITIZERS PROPERTY STRINGS "" address undefined address,undefined)
if(RBT_DM_IMU_SANITIZERS AND NOT RBT_DM_IMU_SANITIZERS MATCHES "^(address|undefined)(,(address|undefined))*$")
    message(FATAL_ERROR "RBT_DM_IMU_SANITIZERS must be empty, address, undefined, or address,undefined")
endif()

function(rbt_dm_imu_warnings target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(RBT_DM_IMU_SANITIZERS)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "RBT_DM_IMU_SANITIZERS is supported only with GCC or Clang")
        endif()
        target_compile_options(${target} PRIVATE
            -fsanitize=${RBT_DM_IMU_SANITIZERS}
            -fno-omit-frame-pointer
        )
        get_target_property(target_type ${target} TYPE)
        if(target_type STREQUAL "EXECUTABLE")
            target_link_options(${target} PRIVATE -fsanitize=${RBT_DM_IMU_SANITIZERS})
        endif()
    endif()
endfunction()

add_library(rbt_dm_imu_core STATIC
    src/config.cpp
    src/parser.cpp
    src/serial_port.cpp
)
rbt_dm_imu_warnings(rbt_dm_imu_core)
target_include_directories(rbt_dm_imu_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rbt_dm_imu_core PUBLIC yaml-cpp::yaml-cpp Threads::Threads)

add_executable(rbt_dm_imu src/main.cpp)
rbt_dm_imu_warnings(rbt_dm_imu)
target_link_libraries(rbt_dm_imu PRIVATE
    rbt_dm_imu_core
    rbt_hardware::messages
    rbt::runtime
    Threads::Threads
)
set_target_properties(rbt_dm_imu PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

add_executable(rbt_dm_imu_probe src/probe.cpp)
rbt_dm_imu_warnings(rbt_dm_imu_probe)
target_link_libraries(rbt_dm_imu_probe PRIVATE
    rbt_hardware::messages
    rbt::runtime
    Threads::Threads
)
set_target_properties(rbt_dm_imu_probe PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

if(BUILD_TESTING)
    add_executable(rbt_dm_imu_parser_test tests/parser_test.cpp src/parser.cpp)
    rbt_dm_imu_warnings(rbt_dm_imu_parser_test)
    target_include_directories(rbt_dm_imu_parser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(rbt_dm_imu_parser_test PRIVATE -UNDEBUG)
    add_test(NAME rbt_dm_imu_parser_test COMMAND rbt_dm_imu_parser_test)

    add_executable(rbt_dm_imu_config_test tests/config_test.cpp)
    rbt_dm_imu_warnings(rbt_dm_imu_config_test)
    target_compile_options(rbt_dm_imu_config_test PRIVATE -UNDEBUG)
    target_link_libraries(rbt_dm_imu_config_test PRIVATE rbt_dm_imu_core)
    add_test(NAME rbt_dm_imu_config_test COMMAND rbt_dm_imu_config_test)
endif()

install(TARGETS rbt_dm_imu rbt_dm_imu_probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY config/ DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-dm-imu/config)
install(FILES README.md NOTICE.md DEPLOYMENT.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
