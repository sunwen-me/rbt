cmake_minimum_required(VERSION 3.22)
project(prior_map_acceptance LANGUAGES C CXX)

include(CTest)

set(MID360_SOURCE_DIR "" CACHE PATH "Path to the existing MID360 LIO workspace")
set(RBT_NAVIGATION_SOURCE_DIR "" CACHE PATH "Path to the navigation source tree")
set(PRIOR_MAP_REVIEW_CONFIG "" CACHE FILEPATH "Reviewed YAML used by the acceptance test")
if(NOT MID360_SOURCE_DIR OR NOT EXISTS "${MID360_SOURCE_DIR}/src/super_lio/CMakeLists.txt")
  message(FATAL_ERROR "MID360_SOURCE_DIR must point to the existing MID360 LIO workspace")
endif()
if(NOT RBT_NAVIGATION_SOURCE_DIR OR
   NOT EXISTS "${RBT_NAVIGATION_SOURCE_DIR}/include/rbt_navigation/runtime/lio_output.hpp")
  message(FATAL_ERROR "RBT_NAVIGATION_SOURCE_DIR must point to the navigation source tree")
endif()
if(NOT PRIOR_MAP_REVIEW_CONFIG)
  set(PRIOR_MAP_REVIEW_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/review-config.yaml")
endif()
if(NOT EXISTS "${PRIOR_MAP_REVIEW_CONFIG}")
  message(FATAL_ERROR "PRIOR_MAP_REVIEW_CONFIG must point to the reviewed Super-LIO YAML")
endif()

set(LIO_WITH_KISS_MATCHER ON CACHE BOOL "Build existing vendored KISS matcher" FORCE)
set(LIO_WITH_FAST_GICP ON CACHE BOOL "Build existing vendored FastVGICP backend" FORCE)
set(LIO_WITH_RBT_NAVIGATION ON CACHE BOOL "Build existing navigation integration" FORCE)
set(RBT_NAVIGATION_SOURCE_DIR "${RBT_NAVIGATION_SOURCE_DIR}" CACHE PATH "" FORCE)

# Keep the MID360 workspace as its own subproject so its CMakeLists derives
# MID_WORKSPACE_DIR from the original source tree as expected.
add_subdirectory("${MID360_SOURCE_DIR}" mid360)

add_executable(prior_map_acceptance prior_map_acceptance.cpp)
target_link_libraries(prior_map_acceptance PRIVATE lio_core)
target_compile_options(prior_map_acceptance PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  add_test(NAME prior_map_acceptance
    COMMAND prior_map_acceptance
      "${PRIOR_MAP_REVIEW_CONFIG}"
      "${CMAKE_CURRENT_BINARY_DIR}/prior-map-acceptance.json")
  set_tests_properties(prior_map_acceptance PROPERTIES TIMEOUT 180)
endif()
