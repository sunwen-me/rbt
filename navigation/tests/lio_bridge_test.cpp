#include <rbt_navigation/runtime/lio_bridge.hpp>
#include <rbt_navigation/runtime/native.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

using namespace rbt::navigation;
namespace lio = rbt::navigation::lio;
namespace native = rbt::navigation::native;
namespace wire = rbt::navwire;

namespace {
constexpr double tolerance = 1e-9;

wire::LioFrameHeader validHeader(const rbt::time::Timestamp& stamp) {
    wire::LioFrameHeader header{};
    header.stamp = native::encodeStamp(stamp);
    header.sequence = 7;
    header.uncertainty_ns = 131'000;
    header.flags = 1;
    std::copy_n("odom", 4, header.world_frame);
    std::copy_n("base_footprint", 14, header.base_frame);
    header.odom_base_position[0] = 1.0;
    header.odom_base_position[1] = 2.0;
    header.odom_base_quaternion_xyzw[3] = 1.0;
    header.map_odom_position[0] = 10.0;
    header.map_odom_quaternion_xyzw[2] = std::sin(std::numbers::pi / 4.0);
    header.map_odom_quaternion_xyzw[3] = std::cos(std::numbers::pi / 4.0);
    header.sensor_origin[0] = 2.0;
    header.sensor_origin[1] = 1.0;
    header.linear_velocity[0] = 1.0;
    header.angular_velocity[2] = 2.0;
    return header;
}

template <typename F>
bool rejects(F&& operation) {
    try {
        operation();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void testProjectionAndExtrinsic() {
    const auto clock = native::hostClock();
    const auto now = native::now(clock);
    auto header = validHeader(now);
    const std::vector<wire::LioPoint> points{{3.0F, 1.0F, 0.0F, 42.0F}};
    lio::ProjectionConfig config;
    config.base_to_base_link = {1.0, 0.0, 0.0, std::numbers::pi / 2.0};

    const auto result = lio::validateAndProject(header, points, now, config);
    assert(result.sequence == header.sequence);
    assert(result.stamp.clock == now.clock);
    assert(result.stamp.ns == now.ns);
    assert(result.uncertainty_ns == 131'000);
    assert(result.odometry.stamp.nanoseconds == header.stamp.nanoseconds);
    assert(result.odometry.sequence == header.sequence);
    assert(std::abs(result.odometry.linear_x - 2.0) < tolerance);
    assert(std::abs(result.odometry.angular_z - 2.0) < tolerance);

    assert(result.map_to_odom.parent.str() == "map");
    assert(result.map_to_odom.child.str() == "odom");
    assert(result.odom_to_base_link.parent.str() == "odom");
    assert(result.odom_to_base_link.child.str() == "base_link");
    assert(result.map_to_odom.stamp.clock == result.odom_to_base_link.stamp.clock);
    assert(result.map_to_odom.stamp.ns == result.odom_to_base_link.stamp.ns);
    assert(result.map_to_odom.uncertainty_ns == 131'000);
    assert(result.odom_to_base_link.uncertainty_ns == 131'000);
    assert(*result.map_to_odom.uncertainty_ns + *result.odom_to_base_link.uncertainty_ns
           <= 1'000'000);

    assert(std::abs(result.map_base_link.x - 8.0) < tolerance);
    assert(std::abs(result.map_base_link.y - 2.0) < tolerance);
    assert(std::abs(result.map_base_link.yaw - std::numbers::pi) < tolerance);
    assert(std::abs(result.map_base_z) < tolerance);
    assert(std::abs(result.sensor_origin_map.x() - 9.0) < tolerance);
    assert(std::abs(result.sensor_origin_map.y() - 2.0) < tolerance);
    assert(result.points_odom.size() == 1 && result.points_map.size() == 1);
    assert(std::abs(result.points_map[0].x() - 9.0) < tolerance);
    assert(std::abs(result.points_map[0].y() - 3.0) < tolerance);
}

void testBaseLinkInputSkipsExtrinsic() {
    const auto clock = native::hostClock();
    const auto now = native::now(clock);
    auto header = validHeader(now);
    std::fill(std::begin(header.base_frame), std::end(header.base_frame), std::uint8_t{0});
    std::copy_n("base_link", 9, header.base_frame);
    header.linear_velocity[0] = 0.5;
    header.angular_velocity[2] = 0.25;
    const lio::ProjectionConfig config{250'000'000, 250'000, 10,
                                       {1.0, 0.0, 0.0, std::numbers::pi / 2.0}};
    const auto result = lio::validateAndProject(header, {}, now, config);
    assert(std::abs(result.odometry.linear_x - 0.5) < tolerance);
    assert(std::abs(result.odometry.angular_z - 0.25) < tolerance);
    assert(std::abs(result.odom_to_base_link.transform.translation().x() - 1.0) < tolerance);
    assert(std::abs(result.odom_to_base_link.transform.translation().y() - 2.0) < tolerance);
}

void testRejectsBadFrames() {
    const auto clock = native::hostClock();
    const auto now = native::now(clock);
    const std::vector<wire::LioPoint> points{};
    const auto valid = validHeader(now);
    auto mustReject = [&](wire::LioFrameHeader header, const lio::ProjectionConfig& config = {},
                          std::uint64_t previous_sequence = 0, TimePoint previous_stamp = -1,
                          std::span<const wire::LioPoint> frame_points = {}) {
        return rejects([&] {
            (void)lio::validateAndProject(header, frame_points, now, config,
                                          previous_sequence, previous_stamp);
        });
    };

    auto header = valid;
    header.flags = 0;
    assert(mustReject(header));
    header = valid;
    header.flags |= 2;
    assert(mustReject(header));
    header = valid;
    header.reserved = 1;
    assert(mustReject(header));
    header = valid;
    header.uncertainty_ns = lio::max_frame_uncertainty_ns + 1;
    assert(mustReject(header));
    header = valid;
    header.sequence = 0;
    assert(mustReject(header));
    assert(mustReject(valid, {}, valid.sequence));
    assert(mustReject(valid, {}, 0, valid.stamp.nanoseconds));
    header = valid;
    header.stamp.generation++;
    assert(mustReject(header));
    header = valid;
    header.stamp.nanoseconds = now.ns + 1;
    assert(mustReject(header));
    header = valid;
    header.stamp.nanoseconds = std::max<TimePoint>(0, now.ns - 250'000'001);
    assert(mustReject(header));
    header = valid;
    header.map_odom_quaternion_xyzw[2] = 0.2;
    assert(mustReject(header));
    header = valid;
    header.base_frame[20] = 'x';
    assert(mustReject(header));
    header = valid;
    header.world_frame[0] = 'm';
    assert(mustReject(header));
    header = valid;
    header.odom_base_quaternion_xyzw[0] = 1.0;
    header.odom_base_quaternion_xyzw[3] = 0.0;
    assert(mustReject(header));

    header = valid;
    header.odom_base_quaternion_xyzw[0] = std::sin(0.21 / 2.0);
    header.odom_base_quaternion_xyzw[3] = std::cos(0.21 / 2.0);
    assert(mustReject(header));
    header = valid;
    const std::vector<wire::LioPoint> nonfinite{{std::numeric_limits<float>::quiet_NaN(), 0, 0, 0}};
    assert(mustReject(header, {}, 0, -1, nonfinite));
    header = valid;
    const std::vector<wire::LioPoint> too_many(2);
    lio::ProjectionConfig low_limit;
    low_limit.max_points = 1;
    assert(mustReject(header, low_limit, 0, -1, too_many));
    assert(points.empty());
}
} // namespace

int main() {
    testProjectionAndExtrinsic();
    testBaseLinkInputSkipsExtrinsic();
    testRejectsBadFrames();
}
