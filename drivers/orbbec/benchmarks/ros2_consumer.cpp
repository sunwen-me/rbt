#include "common.hpp"

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
std::int64_t integer(const std::string& text) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value <= 0)
        throw std::invalid_argument("expected positive integer");
    return value;
}
}

int main(int argc, char** argv) {
    try {
        std::int64_t start = 0, end = 0;
        std::string output;
        for (int i = 1; i < argc; ++i) {
            const std::string option(argv[i]);
            if (option == "--help") {
                std::cout << "ros2-perf-consumer --start-ns N --end-ns N --output FILE\n";
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("missing value");
            if (option == "--start-ns") start = integer(argv[i]);
            else if (option == "--end-ns") end = integer(argv[i]);
            else if (option == "--output") output = argv[i];
            else throw std::invalid_argument("unknown argument: " + option);
        }
        const auto now = bench::now_ns();
        if (start <= now || start - now > 30'000'000'000LL || end <= start
            || end - start > 120'000'000'000LL || output.empty())
            throw std::invalid_argument("invalid measurement window/output");

        // Isolated RMW domain and environment are supplied by the orchestrator.
        rclcpp::init(0, nullptr);
        auto node = std::make_shared<rclcpp::Node>("orbbec_perf_consumer");
        std::array<bench::Stats, 2> stats;
        std::array<rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr, 2> subscriptions;
        const auto* policy = std::getenv("RBT_BENCH_ROS_QOS");
        const bool sensor = policy != nullptr && std::string(policy) == "sensor";
        auto qos = rclcpp::QoS(rclcpp::KeepLast(sensor ? 1 : 10)).durability_volatile();
        if (sensor) qos.best_effort();
        else qos.reliable();
        for (std::size_t stream = 0; stream < subscriptions.size(); ++stream) {
            const auto topic = stream == 0 ? "/camera/color/image_raw" : "/camera/depth/image_raw";
            subscriptions[stream] = node->create_subscription<sensor_msgs::msg::Image>(topic, qos,
                [&, stream](sensor_msgs::msg::Image::ConstSharedPtr image) {
                    const auto received = bench::now_ns();
                    if (received < start) {
                        stats[stream].mark_ready();
                        return;
                    }
                    if (received >= end) return;
                    const auto expected_size = 640U * 480U * (stream == 0 ? 3U : 2U);
                    const auto expected_encoding = stream == 0 ? "rgb8" : "16UC1";
                    if (image->width != 640 || image->height != 480 || image->data.size() != expected_size
                        || image->encoding != expected_encoding || image->is_bigendian)
                        throw std::runtime_error("unexpected ROS image format or dimensions");
                    const auto marker = image->header.frame_id.rfind("/bench_");
                    if (marker == std::string::npos)
                        throw std::runtime_error("benchmark-only driver instrumentation missing");
                    const auto index = integer(image->header.frame_id.substr(marker + 7));
                    const auto packaged = static_cast<std::int64_t>(image->header.stamp.sec) * 1'000'000'000LL
                        + image->header.stamp.nanosec;
                    stats[stream].add(received, packaged, static_cast<std::uint64_t>(index),
                                      image->data.data(), image->data.size());
                });
        }
        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);
        auto timer = node->create_wall_timer(std::chrono::milliseconds(10), [&] {
            if (bench::now_ns() >= end) executor.cancel();
        });
        executor.spin();
        std::ofstream file(output);
        if (!file) throw std::runtime_error("cannot create output JSON");
        file << "{\"framework\":\"ros2\",\"clock\":\"CLOCK_MONOTONIC\",\"start_ns\":" << start
             << ",\"end_ns\":" << end << ",\"streams\":{\"color\":" << stats[0].json(start, end)
             << ",\"depth\":" << stats[1].json(start, end) << "}}\n";
        file.close();
        if (!file) throw std::runtime_error("failed writing summary");
        rclcpp::shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ROS benchmark consumer: " << error.what() << '\n';
        if (rclcpp::ok()) rclcpp::shutdown();
        return 1;
    }
}
