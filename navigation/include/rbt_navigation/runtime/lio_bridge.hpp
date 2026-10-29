#pragma once

#include <rbt/navwire.hpp>
#include <rbt/tf_transport.hpp>
#include <rbt_navigation/core/types.hpp>
#include <rbt_navigation/runtime/lio_contract.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rbt::navigation::lio {
namespace wire = ::rbt::navwire;

inline constexpr std::uint64_t max_frame_uncertainty_ns = 250'000;
inline constexpr std::size_t max_frame_points = 200'000;

struct BodyExtrinsic {
    double x{};
    double y{};
    double z{};
    double yaw{};
};

struct ProjectionConfig {
    TimePoint max_frame_age_ns{250'000'000};
    std::uint64_t max_uncertainty_ns{max_frame_uncertainty_ns};
    std::size_t max_points{max_frame_points};
    BodyExtrinsic base_to_base_link{};
};

struct ProjectedFrame {
    rbt::time::Timestamp stamp;
    std::uint64_t sequence{};
    std::uint64_t uncertainty_ns{};
    wire::Odom2D odometry{};
    rbt::tf::DynamicRecord map_to_odom;
    rbt::tf::DynamicRecord odom_to_base_link;
    Pose2D map_base_link;
    double map_base_z{};
    rbt::tf::Vec3 sensor_origin_odom{rbt::tf::Vec3::zero()};
    rbt::tf::Vec3 sensor_origin_map{rbt::tf::Vec3::zero()};
    std::vector<rbt::tf::Vec3> points_odom;
    std::vector<rbt::tf::Vec3> points_map;
};

// Validates a complete LIO frame and projects its transforms, twist, sensor
// origin, and world points without changing any navigation algorithms.
// Sequence and timestamp floors are supplied by the activation so invalid
// frames can invalidate cached outputs without allowing an old frame to recover.
[[nodiscard]] ProjectedFrame validateAndProject(
    const wire::LioFrameHeader& header,
    std::span<const wire::LioPoint> points,
    const rbt::time::Timestamp& now,
    const ProjectionConfig& config = {},
    std::uint64_t previous_sequence = 0,
    TimePoint previous_stamp_ns = -1);

} // namespace rbt::navigation::lio

