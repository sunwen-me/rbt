#ifndef RBT_NAVIGATION_RUNTIME_LIO_OUTPUT_HPP
#define RBT_NAVIGATION_RUNTIME_LIO_OUTPUT_HPP

#include <rbt/navwire.hpp>
#include <rbt/runtime.hpp>
#include <rbt_navigation/runtime/lio_contract.hpp>
#include <rbt_navigation/runtime/lio_receipt_clock.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace rbt::navigation::runtime {
namespace wire = ::rbt::navwire;

inline constexpr std::size_t lio_navigation_max_points = 200'000U;
inline constexpr std::uint32_t lio_navigation_map_odom_valid = 1U;
using LioNavigationPublisher = rbt::SlicePublisher<wire::LioFrameHeader, wire::LioPoint>;
using LioNavigationPublishCallback =
    std::function<void(const wire::LioFrameHeader&, std::span<const wire::LioPoint>)>;

/// Opens a bounded frame publisher and advertises the established capability.
template <typename Factory>
[[nodiscard]] LioNavigationPublisher provideLioNavigationFrameV1(
    rbt::ActivationContext& context, Factory&& factory) {
    const auto capability = rbt::generate_uuid();
    const auto descriptor = rbt::OwnedStreamDescriptor::from(rbt::StreamDescriptor(
        "rbt/cap/" + capability + "/stream", wire::LIO_NAVIGATION_FRAME_DESCRIPTOR,
        rbt::StreamProfile::Sensor));
    auto created = factory(descriptor.borrowed(), wire::LIO_NAVIGATION_FRAME_DESCRIPTOR,
                           lio_navigation_max_points);
    if (!created) {
        throw std::runtime_error("create LIO navigation-frame publisher: "
                                 + std::string(rbt::to_string(created.error())));
    }
    auto publisher = std::move(created).value();
    if (!context.provide<lio::LioNavigationFrameV1>(capability, descriptor)) {
        throw std::runtime_error("provide LIO navigation-frame capability failed");
    }
    return publisher;
}

struct LioNavigationFrameFields {
    std::string_view world_frame;
    std::string_view base_frame;
    std::array<double, 3> odom_base_position{};
    std::array<double, 4> odom_base_quaternion_xyzw{};
    std::array<double, 3> map_odom_position{};
    std::array<double, 4> map_odom_quaternion_xyzw{};
    std::array<double, 3> sensor_origin{};
    std::array<double, 3> linear_velocity{};
    std::array<double, 3> angular_velocity{};
    bool map_odom_valid{};
};

inline void copyFrameName(std::uint8_t (&out)[64], std::string_view name) {
    if (name.empty() || name.size() > 63U) {
        throw std::invalid_argument("LIO navigation frame names must contain 1..63 bytes");
    }
    std::fill(std::begin(out), std::end(out), 0U);
    std::copy(name.begin(), name.end(), out);
}

[[nodiscard]] inline wire::LioFrameHeader makeLioNavigationHeader(
    const rbt::time::ConvertedTimestamp& converted,
    std::uint64_t sequence,
    const LioNavigationFrameFields& fields) {
    if (converted.timestamp.clock.kind() != rbt::time::ClockKind::HostMonotonic
        || converted.timestamp.ns < 0 || sequence == 0U
        || converted.uncertainty_ns > 250'000U) {
        throw std::invalid_argument("invalid LIO navigation-frame timestamp or localization");
    }
    wire::LioFrameHeader header{};
    const auto& identity = converted.timestamp.clock;
    std::copy(identity.source_id().bytes().begin(), identity.source_id().bytes().end(),
              std::begin(header.stamp.source_id));
    header.stamp.generation = identity.generation();
    header.stamp.nanoseconds = converted.timestamp.ns;
    // navwire encodes HostMonotonic as 1; the SDK's enum starts at 0.
    header.stamp.kind = 1U;
    header.sequence = sequence;
    header.uncertainty_ns = converted.uncertainty_ns;
    header.flags = fields.map_odom_valid ? lio_navigation_map_odom_valid : 0U;
    copyFrameName(header.world_frame, fields.world_frame);
    copyFrameName(header.base_frame, fields.base_frame);
    std::copy(fields.odom_base_position.begin(), fields.odom_base_position.end(),
              std::begin(header.odom_base_position));
    std::copy(fields.odom_base_quaternion_xyzw.begin(), fields.odom_base_quaternion_xyzw.end(),
              std::begin(header.odom_base_quaternion_xyzw));
    if (fields.map_odom_valid) {
        std::copy(fields.map_odom_position.begin(), fields.map_odom_position.end(),
                  std::begin(header.map_odom_position));
        std::copy(fields.map_odom_quaternion_xyzw.begin(), fields.map_odom_quaternion_xyzw.end(),
                  std::begin(header.map_odom_quaternion_xyzw));
    }
    std::copy(fields.sensor_origin.begin(), fields.sensor_origin.end(),
              std::begin(header.sensor_origin));
    std::copy(fields.linear_velocity.begin(), fields.linear_velocity.end(),
              std::begin(header.linear_velocity));
    std::copy(fields.angular_velocity.begin(), fields.angular_velocity.end(),
              std::begin(header.angular_velocity));
    return header;
}

/// Publishes one complete header + XYZI scan; the callback runs only after a
/// successful send and is optional for production callers and fixtures.
inline void publishLioNavigationFrame(
    LioNavigationPublisher& publisher,
    const wire::LioFrameHeader& header,
    std::span<const wire::LioPoint> points,
    const LioNavigationPublishCallback& callback = {}) {
    if (points.empty() || points.size() > lio_navigation_max_points) {
        throw std::length_error("LIO navigation-frame point count is outside 1..200000");
    }
    auto loan = publisher.loan_slice(points.size());
    if (!loan) {
        throw std::runtime_error("loan LIO navigation frame: "
                                 + std::string(rbt::to_string(loan.error())));
    }
    auto sample = std::move(loan).value();
    sample.header() = header;
    std::copy(points.begin(), points.end(), sample.payload().begin());
    auto sent = sample.send();
    if (!sent) {
        throw std::runtime_error("send LIO navigation frame: "
                                 + std::string(rbt::to_string(sent.error())));
    }
    if (callback) callback(header, points);
}

} // namespace rbt::navigation::runtime

#endif
