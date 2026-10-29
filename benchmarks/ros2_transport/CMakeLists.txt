cmake_minimum_required(VERSION 3.22)
project(transport_bench VERSION 0.1.0 LANGUAGES C CXX)
option(BENCH_BUILD_RBT "Build the standalone RBT SDK endpoint" ON)
option(BENCH_BUILD_ROS "Build the optional ROS 2/Fast DDS endpoint" ON)

function(bench_target name)
  target_compile_features(${name} PRIVATE cxx_std_20)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

if(BENCH_BUILD_RBT)
  find_package(rbt CONFIG REQUIRED)
  add_executable(rbt_transport_bench rbt_endpoint.cpp)
  bench_target(rbt_transport_bench)
  target_include_directories(rbt_transport_bench PRIVATE generated/cpp)
  target_link_libraries(rbt_transport_bench PRIVATE rbt::runtime)
endif()

if(BENCH_BUILD_ROS)
  find_package(ament_cmake REQUIRED)
  find_package(rosidl_default_generators REQUIRED)
  find_package(rclcpp REQUIRED)
  find_package(sensor_msgs REQUIRED)
  find_package(rmw_fastrtps_shared_cpp REQUIRED)
  rosidl_generate_interfaces(${PROJECT_NAME}
    msg/ImuFrame.msg msg/CloudFrame.msg msg/VideoFrame.msg
    SKIP_INSTALL)
  rosidl_get_typesupport_target(bench_typesupport ${PROJECT_NAME} rosidl_typesupport_cpp)
  add_executable(ros2_transport_bench ros2_endpoint.cpp)
  bench_target(ros2_transport_bench)
  target_link_libraries(ros2_transport_bench PRIVATE
    rclcpp::rclcpp ${bench_typesupport}
    rmw_fastrtps_shared_cpp::rmw_fastrtps_shared_cpp)
  add_executable(standard_loan_probe standard_loan_probe.cpp)
  bench_target(standard_loan_probe)
  target_link_libraries(standard_loan_probe PRIVATE
    rclcpp::rclcpp sensor_msgs::sensor_msgs_library)
  ament_package()
endif()
