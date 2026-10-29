cmake_minimum_required(VERSION 3.22)

project(rbt-orbbec-driver VERSION 0.2.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

option(RBT_ORBBEC_BUILD_DEVICE "Build the Gemini335 hardware driver" ON)
set(ORBBEC_SDK_ROOT "" CACHE PATH "OrbbecSDK 2.10.6 installation prefix")

find_package(rbt CONFIG REQUIRED)
find_package(Threads REQUIRED)

function(rbt_orbbec_warnings target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

add_library(rbt_orbbec_core STATIC src/core.cpp src/config.cpp)
target_compile_features(rbt_orbbec_core PUBLIC cxx_std_20)
target_compile_options(rbt_orbbec_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(rbt_orbbec_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rbt_orbbec_core PUBLIC rbt::runtime rbt::iox2)

add_executable(rbt-orbbec-consumer src/consumer.cpp)
rbt_orbbec_warnings(rbt-orbbec-consumer)
target_include_directories(rbt-orbbec-consumer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
)
target_link_libraries(rbt-orbbec-consumer PRIVATE rbt_orbbec_core rbt::runtime rbt::iox2 Threads::Threads)
set_target_properties(rbt-orbbec-consumer PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

if(RBT_ORBBEC_BUILD_DEVICE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
        OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|aarch64|arm64)$")
        message(FATAL_ERROR "The pinned OrbbecSDK package requires Linux x86_64 or ARM64")
    endif()
    # Orbbec ships this package's version metadata as OrbbecSDKVersion.cmake,
    # while CMake's versioned CONFIG lookup expects *ConfigVersion.cmake.
    # Load the vendor package normally, then enforce the SDK's exact version.
    find_package(OrbbecSDK CONFIG REQUIRED HINTS "${ORBBEC_SDK_ROOT}/lib")
    set(_orbbec_sdk_version_file "${OrbbecSDK_DIR}/OrbbecSDKVersion.cmake")
    if(NOT EXISTS "${_orbbec_sdk_version_file}")
        message(FATAL_ERROR "OrbbecSDKVersion.cmake is missing from ${OrbbecSDK_DIR}")
    endif()
    file(READ "${_orbbec_sdk_version_file}" _orbbec_sdk_version_content)
    string(REGEX MATCH "set\\(PACKAGE_VERSION[ \t]+\"([^\"]+)\"\\)"
        _orbbec_sdk_version_match "${_orbbec_sdk_version_content}")
    if(NOT CMAKE_MATCH_1 STREQUAL "2.10.6")
        message(FATAL_ERROR
            "OrbbecSDK 2.10.6 is required; ${_orbbec_sdk_version_file} reports '${CMAKE_MATCH_1}'"
        )
    endif()

    add_library(rbt_orbbec_backend STATIC src/backend.cpp src/connection.cpp)
    rbt_orbbec_warnings(rbt_orbbec_backend)
    target_include_directories(rbt_orbbec_backend
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/generated
    )
    get_target_property(_orbbec_sdk_include_dirs ob::OrbbecSDK INTERFACE_INCLUDE_DIRECTORIES)
    if(_orbbec_sdk_include_dirs)
        target_include_directories(rbt_orbbec_backend SYSTEM PRIVATE ${_orbbec_sdk_include_dirs})
    endif()
    target_link_libraries(rbt_orbbec_backend PRIVATE rbt_orbbec_core ob::OrbbecSDK Threads::Threads)

    add_executable(rbt-orbbec-camera src/main.cpp src/component.cpp src/supervisor.cpp)
    rbt_orbbec_warnings(rbt-orbbec-camera)
    target_include_directories(rbt-orbbec-camera
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/generated
    )
    target_link_libraries(rbt-orbbec-camera
        PRIVATE rbt_orbbec_backend rbt_orbbec_core rbt::runtime rbt::iox2 Threads::Threads
    )
    set_target_properties(rbt-orbbec-camera PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
        INSTALL_RPATH_USE_LINK_PATH TRUE
    )

    install(TARGETS rbt-orbbec-camera RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    if(BUILD_TESTING)
        add_executable(rbt_orbbec_hardware_processing_probe tests/hardware_processing_probe.cpp)
        rbt_orbbec_warnings(rbt_orbbec_hardware_processing_probe)
        target_link_libraries(rbt_orbbec_hardware_processing_probe
            PRIVATE rbt_orbbec_backend rbt_orbbec_core ob::OrbbecSDK Threads::Threads)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(rbt_orbbec_core_test tests/core_test.cpp)
    rbt_orbbec_warnings(rbt_orbbec_core_test)
    target_link_libraries(rbt_orbbec_core_test PRIVATE rbt_orbbec_core)
    add_test(NAME rbt_orbbec_core_test COMMAND rbt_orbbec_core_test)

    add_executable(rbt_orbbec_imu_queue_test tests/imu_queue_test.cpp)
    rbt_orbbec_warnings(rbt_orbbec_imu_queue_test)
    target_link_libraries(rbt_orbbec_imu_queue_test PRIVATE rbt_orbbec_core Threads::Threads)
    add_test(NAME rbt_orbbec_imu_queue_test COMMAND rbt_orbbec_imu_queue_test)
    set_tests_properties(rbt_orbbec_imu_queue_test PROPERTIES TIMEOUT 5)

    add_executable(rbt_orbbec_component_test tests/component_test.cpp src/component.cpp)
    rbt_orbbec_warnings(rbt_orbbec_component_test)
    target_include_directories(rbt_orbbec_component_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/generated
    )
    target_link_libraries(rbt_orbbec_component_test
        PRIVATE rbt_orbbec_core rbt::runtime rbt::iox2 Threads::Threads
    )
    add_test(NAME rbt_orbbec_component_test COMMAND rbt_orbbec_component_test)
    set_tests_properties(rbt_orbbec_component_test PROPERTIES TIMEOUT 10)

    add_executable(rbt_orbbec_processing_test tests/processing_test.cpp)
    rbt_orbbec_warnings(rbt_orbbec_processing_test)
    target_link_libraries(rbt_orbbec_processing_test PRIVATE rbt_orbbec_core)
    add_test(NAME rbt_orbbec_processing_test COMMAND rbt_orbbec_processing_test)

    add_executable(rbt_orbbec_supervisor_fixture
        tests/supervisor_fixture.cpp src/supervisor.cpp src/component.cpp)
    rbt_orbbec_warnings(rbt_orbbec_supervisor_fixture)
    target_link_libraries(rbt_orbbec_supervisor_fixture
        PRIVATE rbt_orbbec_core rbt::runtime rbt::iox2 Threads::Threads)
endif()

install(TARGETS rbt-orbbec-consumer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY generated/rbt/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rbt)
install(FILES types/orbbec.rbt DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-orbbec/types)
install(FILES generated/descriptors/orbbec.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-orbbec/descriptors
)
install(DIRECTORY python/ DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-orbbec/python
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
)
install(FILES README.md VERIFICATION.md NOTICE.md UPSTREAM.toml
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(FILES sdk-manifest.json DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-orbbec)
install(DIRECTORY licenses/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses)
