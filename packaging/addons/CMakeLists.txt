cmake_minimum_required(VERSION 3.22)
project(rbt-camera-tracking-addons VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CTest)

option(RBT_ADDONS_ORBBEC "Build the Orbbec camera component" ON)
option(RBT_ADDONS_M3T "Build the M3T tracking component" ON)
find_package(rbt 0.8.0 EXACT CONFIG REQUIRED)

if(NOT ORBBEC_SDK_ROOT)
    set(ORBBEC_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/orbbec-sdk-2.10.6-arm64"
        CACHE PATH "OrbbecSDK 2.10.6 prefix")
endif()
set(RBT_M3T_UPSTREAM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/m3t"
    CACHE PATH "Bundled pinned M3T source")

if(RBT_ADDONS_ORBBEC)
    set(RBT_ORBBEC_BUILD_DEVICE ON CACHE BOOL "Build the physical camera driver" FORCE)
    add_subdirectory(drivers/orbbec)
    # Preserve the vendor library's extensions directory for its dynamic loader.
    install(DIRECTORY "${ORBBEC_SDK_ROOT}/lib/" DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY "${ORBBEC_SDK_ROOT}/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY "${ORBBEC_SDK_ROOT}/licenses/"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-addons/orbbec-licenses)
    install(DIRECTORY "${ORBBEC_SDK_ROOT}/share/"
        DESTINATION ${CMAKE_INSTALL_DATADIR} OPTIONAL)
endif()
if(RBT_ADDONS_M3T)
    add_subdirectory(perception/m3t)
endif()
foreach(program rbt-orbbec-camera rbt-orbbec-consumer rbt-m3t rbt-m3t-initialize)
    if(TARGET ${program})
        set_target_properties(${program} PROPERTIES
            INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
            INSTALL_RPATH_USE_LINK_PATH FALSE)
    endif()
endforeach()
install(FILES README-ARM64.md SOURCE-MANIFEST.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rbt-addons)
configure_file(env.sh.in "${CMAKE_CURRENT_BINARY_DIR}/env.sh" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/env.sh" DESTINATION .)
