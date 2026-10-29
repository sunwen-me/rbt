cmake_minimum_required(VERSION 3.22)
project(rbt_hardware VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(rbt CONFIG REQUIRED)
add_library(rbt_hardware_messages INTERFACE)
add_library(rbt_hardware::messages ALIAS rbt_hardware_messages)
set_target_properties(rbt_hardware_messages PROPERTIES EXPORT_NAME messages)
target_compile_features(rbt_hardware_messages INTERFACE cxx_std_20)
target_include_directories(rbt_hardware_messages INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated/cpp>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rbt_hardware_messages INTERFACE rbt::iox2)
install(TARGETS rbt_hardware_messages EXPORT rbtHardwareTargets)
install(EXPORT rbtHardwareTargets NAMESPACE rbt_hardware:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt_hardware)
configure_package_config_file(cmake/rbt_hardwareConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rbt_hardwareConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt_hardware)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/rbt_hardwareConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rbt_hardwareConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rbt_hardwareConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt_hardware)
install(DIRECTORY include/ generated/cpp/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY types generated/descriptors generated/rust DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt_hardware)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt_hardware)
