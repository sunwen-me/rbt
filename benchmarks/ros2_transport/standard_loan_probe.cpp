#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <iostream>

template<class T>
void probe(rclcpp::Node& node, const char* name) {
  auto qos = rclcpp::QoS(1).best_effort().durability_volatile();
  auto publisher = node.create_publisher<T>(std::string("/rbt_loan_probe/") + name, qos);
  auto subscriber = node.create_subscription<T>(std::string("/rbt_loan_probe/") + name, qos,
      [](const typename T::ConstSharedPtr&) {});
  std::cout << "{\"type\":\"sensor_msgs/" << name << "\",\"publisher_can_loan\":"
            << (publisher->can_loan_messages() ? "true" : "false")
            << ",\"subscriber_can_loan\":" << (subscriber->can_loan_messages() ? "true" : "false")
            << "}\n";
}

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);
  rclcpp::Node node("rbt_standard_loan_probe", rclcpp::NodeOptions().use_intra_process_comms(false));
  probe<sensor_msgs::msg::Imu>(node, "Imu");
  probe<sensor_msgs::msg::PointCloud2>(node, "PointCloud2");
  probe<sensor_msgs::msg::Image>(node, "Image");
  rclcpp::shutdown();
}
