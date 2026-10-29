cmake_minimum_required(VERSION 3.22)
project(rbt-m3t VERSION 0.3.0 LANGUAGES CXX)
include(CTest)
include(GNUInstallDirs)
option(RBT_M3T_ENABLE_HEXAGON_ORB
    "Enable the optional Hexagon ORB backend" OFF)
set(RBT_M3T_DSP_SKEL "" CACHE FILEPATH
    "Path to the custom Hexagon ORB skeleton shared library")
set(RBT_M3T_DSP_STUB "" CACHE FILEPATH
    "Path to the ARM FastRPC ORB stub shared library")

find_package(rbt CONFIG REQUIRED)
find_package(Threads REQUIRED)

get_filename_component(RBT_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(rbt_m3t_orb_core STATIC
    dsp/src/orb_extract.cpp
    dsp/src/orb_match.cpp
    dsp/src/orb_dispatch.cpp)
target_include_directories(rbt_m3t_orb_core PUBLIC dsp/include)
target_compile_features(rbt_m3t_orb_core PUBLIC cxx_std_17)
target_compile_options(rbt_m3t_orb_core PRIVATE
    -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)

if(RBT_M3T_ENABLE_HEXAGON_ORB)
    find_package(OpenCV REQUIRED COMPONENTS core features2d imgproc)

    add_library(rbt_m3t_dsp_backend STATIC src/dsp_orb.cpp)
    target_include_directories(rbt_m3t_dsp_backend PUBLIC include)
    target_include_directories(rbt_m3t_dsp_backend SYSTEM PUBLIC
        ${OpenCV_INCLUDE_DIRS})
    target_compile_features(rbt_m3t_dsp_backend PUBLIC cxx_std_17)
    target_compile_options(rbt_m3t_dsp_backend PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(rbt_m3t_dsp_backend
        PUBLIC ${OpenCV_LIBS} Threads::Threads
        PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(rbt_m3t_dsp_backend PRIVATE
        RBT_M3T_DSP_INSTALL_LIBDIR="${CMAKE_INSTALL_LIBDIR}")

    foreach(_rbt_m3t_dsp_artifact RBT_M3T_DSP_SKEL RBT_M3T_DSP_STUB)
        if("${${_rbt_m3t_dsp_artifact}}" STREQUAL "")
            message(FATAL_ERROR
                "RBT_M3T_ENABLE_HEXAGON_ORB requires ${_rbt_m3t_dsp_artifact} "
                "to name an existing shared library")
        endif()
        get_filename_component(_rbt_m3t_dsp_artifact_path
            "${${_rbt_m3t_dsp_artifact}}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_rbt_m3t_dsp_artifact_path}" OR
           IS_DIRECTORY "${_rbt_m3t_dsp_artifact_path}")
            message(FATAL_ERROR
                "${_rbt_m3t_dsp_artifact} must name an existing file: "
                "${${_rbt_m3t_dsp_artifact}}")
        endif()
        set(_rbt_m3t_dsp_${_rbt_m3t_dsp_artifact}_path
            "${_rbt_m3t_dsp_artifact_path}")
    endforeach()

    set(_rbt_m3t_dsp_skel_output
        "${CMAKE_CURRENT_BINARY_DIR}/librbt_m3t_orb_skel.so")
    set(_rbt_m3t_dsp_stub_output
        "${CMAKE_CURRENT_BINARY_DIR}/librbt_m3t_orb_stub.so")
    add_custom_command(
        OUTPUT "${_rbt_m3t_dsp_skel_output}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_rbt_m3t_dsp_RBT_M3T_DSP_SKEL_path}"
            "${_rbt_m3t_dsp_skel_output}"
        DEPENDS "${_rbt_m3t_dsp_RBT_M3T_DSP_SKEL_path}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${_rbt_m3t_dsp_stub_output}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_rbt_m3t_dsp_RBT_M3T_DSP_STUB_path}"
            "${_rbt_m3t_dsp_stub_output}"
        DEPENDS "${_rbt_m3t_dsp_RBT_M3T_DSP_STUB_path}"
        VERBATIM)
    add_custom_target(rbt_m3t_dsp_runtime_artifacts ALL
        DEPENDS "${_rbt_m3t_dsp_skel_output}" "${_rbt_m3t_dsp_stub_output}")
    install(FILES "${_rbt_m3t_dsp_skel_output}" "${_rbt_m3t_dsp_stub_output}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/rbt-m3t/dsp")
endif()

include(cmake/UpstreamM3T.cmake)
rbt_add_m3t_upstream()

add_library(rbt_m3t_orbbec_protocol STATIC
    ${RBT_REPOSITORY_ROOT}/drivers/orbbec/src/core.cpp
    ${RBT_REPOSITORY_ROOT}/drivers/orbbec/src/config.cpp)
target_include_directories(rbt_m3t_orbbec_protocol PUBLIC
    ${RBT_REPOSITORY_ROOT}/drivers/orbbec/include
    ${RBT_REPOSITORY_ROOT}/drivers/orbbec/generated)
target_compile_features(rbt_m3t_orbbec_protocol PUBLIC cxx_std_20)
target_link_libraries(rbt_m3t_orbbec_protocol PUBLIC rbt::runtime rbt::iox2)

add_library(rbt_m3t_adapter STATIC src/frame_source.cpp src/model.cpp src/app_config.cpp
    src/camera_motion.cpp src/joint_prior.cpp)
target_include_directories(rbt_m3t_adapter PUBLIC include)
target_include_directories(rbt_m3t_adapter SYSTEM PRIVATE third_party)
target_compile_features(rbt_m3t_adapter PUBLIC cxx_std_20)
target_compile_options(rbt_m3t_adapter PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(rbt_m3t_adapter PUBLIC rbt_m3t_upstream rbt_m3t_orbbec_protocol Threads::Threads)

add_executable(rbt-m3t src/main.cpp)
target_compile_options(rbt-m3t PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(rbt-m3t PRIVATE rbt_m3t_adapter)
add_executable(rbt-m3t-initialize src/initialize.cpp)
target_compile_options(rbt-m3t-initialize PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(rbt-m3t-initialize PRIVATE rbt_m3t_adapter)

if(RBT_M3T_ENABLE_HEXAGON_ORB)
    add_dependencies(rbt-m3t rbt_m3t_dsp_runtime_artifacts)
    add_dependencies(rbt-m3t-initialize rbt_m3t_dsp_runtime_artifacts)
endif()

if(BUILD_TESTING)
    add_library(rbt_m3t_renderer_audit SHARED tests/glfw_renderer_audit.cpp)
    target_link_libraries(rbt_m3t_renderer_audit PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})
    target_compile_options(rbt_m3t_renderer_audit PRIVATE -Wall -Wextra -Werror)

    foreach(name frame_source model app_config camera_motion joint_prior joint_prior_optimizer)
        add_executable(rbt_m3t_${name}_test tests/${name}_test.cpp)
        target_link_libraries(rbt_m3t_${name}_test PRIVATE rbt_m3t_adapter)
        target_compile_options(rbt_m3t_${name}_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME rbt_m3t_${name}_test COMMAND rbt_m3t_${name}_test)
        set_tests_properties(rbt_m3t_${name}_test PROPERTIES TIMEOUT 10)
    endforeach()

    add_library(rbt_m3t_orb_extract_test_core STATIC dsp/src/orb_extract.cpp)
    target_include_directories(rbt_m3t_orb_extract_test_core PUBLIC dsp/include)
    target_compile_features(rbt_m3t_orb_extract_test_core PUBLIC cxx_std_17)
    target_compile_definitions(rbt_m3t_orb_extract_test_core PRIVATE
        RBT_ORB_TESTING=1)
    target_compile_options(rbt_m3t_orb_extract_test_core PRIVATE
        -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)

    foreach(name extract match dispatch)
        add_executable(rbt_m3t_orb_${name}_test dsp/tests/orb_${name}_test.cpp)
        target_include_directories(rbt_m3t_orb_${name}_test PRIVATE dsp/include)
        target_compile_options(rbt_m3t_orb_${name}_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
        if(name STREQUAL "extract")
            target_compile_definitions(rbt_m3t_orb_${name}_test PRIVATE
                RBT_ORB_TESTING=1)
            target_link_libraries(rbt_m3t_orb_${name}_test PRIVATE
                rbt_m3t_orb_extract_test_core)
        else()
            target_link_libraries(rbt_m3t_orb_${name}_test PRIVATE
                rbt_m3t_orb_core)
        endif()
        add_test(NAME rbt_m3t_orb_${name}_test
            COMMAND rbt_m3t_orb_${name}_test)
        set_tests_properties(rbt_m3t_orb_${name}_test PROPERTIES TIMEOUT 30)
    endforeach()

    if(RBT_M3T_ENABLE_HEXAGON_ORB)
        add_library(rbt_m3t_dsp_fake_stub SHARED
            tests/dsp_orb_fake_stub.cpp)
        target_compile_features(rbt_m3t_dsp_fake_stub PRIVATE cxx_std_17)
        target_compile_options(rbt_m3t_dsp_fake_stub PRIVATE
            -Wall -Wextra -Wpedantic -Werror)

        add_executable(rbt_m3t_dsp_orb_client_test
            tests/dsp_orb_client_test.cpp)
        set(_rbt_m3t_dsp_client_test_dir
            "${CMAKE_CURRENT_BINARY_DIR}/dsp-client-test")
        set_target_properties(rbt_m3t_dsp_orb_client_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${_rbt_m3t_dsp_client_test_dir}")
        file(MAKE_DIRECTORY "${_rbt_m3t_dsp_client_test_dir}")
        set(_rbt_m3t_dsp_fake_module_fixture
            "${_rbt_m3t_dsp_client_test_dir}/fake-module.fixture")
        file(WRITE "${_rbt_m3t_dsp_fake_module_fixture}"
            "Readable fake DSP module fixture for client tests.\n")
        target_compile_options(rbt_m3t_dsp_orb_client_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        target_link_libraries(rbt_m3t_dsp_orb_client_test PRIVATE
            rbt_m3t_dsp_backend ${CMAKE_DL_LIBS})
        add_test(NAME rbt_m3t_dsp_orb_client_test
            COMMAND rbt_m3t_dsp_orb_client_test
                "$<TARGET_FILE:rbt_m3t_dsp_fake_stub>"
                "${_rbt_m3t_dsp_fake_module_fixture}"
                "$<TARGET_FILE_DIR:rbt_m3t_dsp_orb_client_test>")
        set_tests_properties(rbt_m3t_dsp_orb_client_test
            PROPERTIES TIMEOUT 30)

        add_executable(rbt_m3t_dsp_device_test tests/dsp_device_test.cpp)
        target_compile_options(rbt_m3t_dsp_device_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        target_link_libraries(rbt_m3t_dsp_device_test PRIVATE
            rbt_m3t_dsp_backend rbt_m3t_orb_core ${OpenCV_LIBS})
        add_dependencies(rbt_m3t_dsp_device_test
            rbt_m3t_dsp_runtime_artifacts)
    endif()

    add_executable(rbt_m3t_rgbd_fixture tests/rgbd_fixture.cpp)
    add_executable(rbt_m3t_tracking_output_consumer_test tests/tracking_output_consumer.cpp)
    foreach(target rbt_m3t_rgbd_fixture rbt_m3t_tracking_output_consumer_test)
        target_link_libraries(${target} PRIVATE rbt_m3t_adapter)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endforeach()
endif()

set_target_properties(rbt-m3t rbt-m3t-initialize PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    INSTALL_RPATH_USE_LINK_PATH TRUE)
install(TARGETS rbt-m3t rbt-m3t-initialize RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY configs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-m3t/configs)
install(PROGRAMS scripts/prepare_robot.py DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-m3t/scripts)
install(FILES README.md NOTICE.md UPSTREAM.toml VERIFICATION.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-m3t)
install(DIRECTORY licenses/ DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-m3t/licenses)
install(FILES third_party/nlohmann/LICENSE.MIT third_party/nlohmann/PROVENANCE.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-m3t/licenses/nlohmann-json)
install(DIRECTORY dsp/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-m3t/dsp
    PATTERN "__pycache__" EXCLUDE)
