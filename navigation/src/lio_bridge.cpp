#include <rbt_navigation/runtime/lio_bridge.hpp>

#include <rbt_navigation/runtime/native.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace rbt::navigation::lio {
namespace {
constexpr std::uint32_t map_odom_valid_flag = 1U;

std::string_view frameName(const std::uint8_t (&bytes)[64]) {
    const auto end = std::find(std::begin(bytes), std::end(bytes), std::uint8_t{0});
    if (end == std::end(bytes)
        || !std::all_of(end, std::end(bytes), [](std::uint8_t value) { return value == 0; })) {
        throw std::invalid_argument("LIO frame name is not canonically NUL-padded");
    }
    const auto length = static_cast<std::size_t>(end - std::begin(bytes));
    if (length == 0) throw std::invalid_argument("LIO frame name is empty");
    return {reinterpret_cast<const char*>(bytes), length};
}

rbt::tf::Transform3 transform(const double (&position)[3], const double (&quaternion)[4]) {
    return {rbt::tf::Vec3(position[0], position[1], position[2]),
            rbt::tf::Quaternion(quaternion[0], quaternion[1], quaternion[2], quaternion[3])};
}

rbt::tf::Transform3 bodyExtrinsic(const BodyExtrinsic& value) {
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)
        || !std::isfinite(value.yaw)) {
        throw std::invalid_argument("LIO base-to-base_link extrinsic must be finite");
    }
    return {rbt::tf::Vec3(value.x, value.y, value.z),
            rbt::tf::Quaternion(0.0, 0.0, std::sin(value.yaw / 2.0), std::cos(value.yaw / 2.0))};
}

rbt::tf::Vec3 cross(rbt::tf::Vec3 lhs, rbt::tf::Vec3 rhs) {
    return {lhs.y() * rhs.z() - lhs.z() * rhs.y(),
            lhs.z() * rhs.x() - lhs.x() * rhs.z(),
            lhs.x() * rhs.y() - lhs.y() * rhs.x()};
}

Pose2D planarPose(const rbt::tf::Transform3& value) {
    const auto translation = value.translation();
    const auto q = value.rotation();
    const double roll = std::atan2(2.0 * (q.w() * q.x() + q.y() * q.z()),
                                   1.0 - 2.0 * (q.x() * q.x() + q.y() * q.y()));
    const double pitch = std::asin(std::clamp(2.0 * (q.w() * q.y() - q.z() * q.x()), -1.0, 1.0));
    if (std::abs(roll) > 0.2 || std::abs(pitch) > 0.2) {
        throw std::invalid_argument("LIO final base_link pose is nonplanar");
    }
    const double yaw = std::atan2(2.0 * (q.w() * q.z() + q.x() * q.y()),
                                  1.0 - 2.0 * (q.y() * q.y() + q.z() * q.z()));
    const Pose2D result{translation.x(), translation.y(), yaw};
    if (!finite(result)) throw std::invalid_argument("LIO final base_link pose is not finite");
    return result;
}

void validateConfig(const ProjectionConfig& config) {
    if (config.max_frame_age_ns <= 0 || config.max_uncertainty_ns > max_frame_uncertainty_ns
        || config.max_points == 0 || config.max_points > max_frame_points) {
        throw std::invalid_argument("invalid LIO bridge limits");
    }
    (void)bodyExtrinsic(config.base_to_base_link);
}
} // namespace

ProjectedFrame validateAndProject(const wire::LioFrameHeader& header,
                                  std::span<const wire::LioPoint> points,
                                  const rbt::time::Timestamp& now,
                                  const ProjectionConfig& config,
                                  std::uint64_t previous_sequence,
                                  TimePoint previous_stamp_ns) {
    validateConfig(config);
    if (now.clock.kind() != rbt::time::ClockKind::HostMonotonic || now.ns < 0) {
        throw std::invalid_argument("LIO bridge requires a valid HostMonotonic epoch");
    }
    if (header.reserved != 0 || (header.flags & ~map_odom_valid_flag) != 0
        || (header.flags & map_odom_valid_flag) == 0) {
        throw std::invalid_argument("LIO frame flags or reserved field are invalid");
    }
    if (header.sequence == 0 || header.sequence <= previous_sequence) {
        throw std::invalid_argument("LIO frame sequence is zero, duplicate, or regressed");
    }
    if (header.uncertainty_ns > config.max_uncertainty_ns) {
        throw std::invalid_argument("LIO frame uncertainty exceeds the configured bound");
    }
    if (points.size() > config.max_points) {
        throw std::invalid_argument("LIO frame point count exceeds the configured bound");
    }

    const auto stamp_ns = native::decodeStamp(header.stamp, now.clock);
    if (!fresh(stamp_ns, now.ns, config.max_frame_age_ns)) {
        throw std::invalid_argument("LIO frame timestamp is stale or in the future");
    }
    if (previous_stamp_ns >= 0 && stamp_ns <= previous_stamp_ns) {
        throw std::invalid_argument("LIO frame timestamp is duplicate or regressed");
    }

    const auto world = frameName(header.world_frame);
    const auto base = frameName(header.base_frame);
    if (world != "odom" || (base != "base_footprint" && base != "base_link")) {
        throw std::invalid_argument("LIO frame names do not match the navigation frame contract");
    }

    const auto map_from_odom = transform(header.map_odom_position, header.map_odom_quaternion_xyzw);
    const auto odom_from_base = transform(header.odom_base_position, header.odom_base_quaternion_xyzw);
    const auto base_from_link = bodyExtrinsic(config.base_to_base_link);
    const auto odom_from_link = base == "base_footprint"
        ? odom_from_base.compose(base_from_link)
        : odom_from_base;
    const auto map_from_link = map_from_odom.compose(odom_from_link);
    const auto map_pose = planarPose(map_from_link);

    for (const auto& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)
            || !std::isfinite(point.intensity)) {
            throw std::invalid_argument("LIO frame contains a nonfinite point");
        }
    }

    const auto sensor_odom = rbt::tf::Vec3(header.sensor_origin[0], header.sensor_origin[1],
                                           header.sensor_origin[2]);
    const auto linear_base = rbt::tf::Vec3(header.linear_velocity[0], header.linear_velocity[1],
                                           header.linear_velocity[2]);
    const auto angular_base = rbt::tf::Vec3(header.angular_velocity[0], header.angular_velocity[1],
                                            header.angular_velocity[2]);
    const bool has_footprint_extrinsic = base == "base_footprint";
    const auto link_twist_rotation = has_footprint_extrinsic ? base_from_link.inverse() :
        rbt::tf::Transform3::identity();
    const auto lever_velocity = has_footprint_extrinsic
        ? cross(angular_base, base_from_link.translation()) : rbt::tf::Vec3::zero();
    const auto linear_in_base = rbt::tf::Vec3(linear_base.x() + lever_velocity.x(),
                                              linear_base.y() + lever_velocity.y(),
                                              linear_base.z() + lever_velocity.z());
    const auto linear_link = link_twist_rotation.apply_vector(linear_in_base);
    const auto angular_link = link_twist_rotation.apply_vector(angular_base);

    const rbt::time::Timestamp stamp{now.clock, stamp_ns};
    const auto uncertainty = std::optional<std::uint64_t>{header.uncertainty_ns};
    const rbt::tf::DynamicRecord map_to_odom{
        rbt::tf::FrameId("map"), rbt::tf::FrameId("odom"), stamp, uncertainty, 0, map_from_odom};
    const rbt::tf::DynamicRecord odom_to_base_link{
        rbt::tf::FrameId("odom"), rbt::tf::FrameId("base_link"), stamp, uncertainty, 0, odom_from_link};

    ProjectedFrame result{stamp,
                          header.sequence,
                          header.uncertainty_ns,
                          wire::Odom2D{header.stamp, header.sequence, linear_link.x(), angular_link.z()},
                          map_to_odom,
                          odom_to_base_link,
                          map_pose,
                          map_from_link.translation().z(),
                          sensor_odom,
                          map_from_odom.apply_point(sensor_odom),
                          {},
                          {}};
    result.points_odom.reserve(points.size());
    result.points_map.reserve(points.size());
    for (const auto& point : points) {
        const rbt::tf::Vec3 odom_point(point.x, point.y, point.z);
        result.points_odom.push_back(odom_point);
        result.points_map.push_back(map_from_odom.apply_point(odom_point));
    }
    return result;
}

} // namespace rbt::navigation::lio
