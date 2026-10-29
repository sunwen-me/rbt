#ifndef RBT_CPP_TF_WIRE_HPP
#define RBT_CPP_TF_WIRE_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <rbt/tf.hpp>
#include <rbt/transforms.hpp>

namespace rbt::tf {

inline constexpr std::uint8_t kTimestampUncertaintyKnown = 1U;
inline constexpr std::uint32_t kStaticStatusSuccess = 0U;
inline constexpr std::uint32_t kStaticStatusSnapshotTooLarge = 1U;
inline constexpr std::uint32_t kStaticStatusProviderUnavailable = 2U;

inline void validate_wire_authority(const CapabilityInstanceId& expected,
                                    const std::uint8_t (&authority)[16]);
inline void encode_static_header(const CapabilityInstanceId& authority,
                                 const StaticSet& set,
                                 std::uint8_t (&wire_authority)[16],
                                 std::uint32_t& status,
                                 std::uint32_t& count,
                                 TfTimestamp& effective_from);
inline StaticSet decode_static_payload(const CapabilityInstanceId& expected,
                                       const std::uint8_t (&authority)[16],
                                       std::uint64_t revision,
                                       std::uint32_t status,
                                       std::uint32_t count,
                                       const TfTimestamp& effective_from_wire,
                                       std::span<const StaticTransform> payload,
                                       std::size_t max);

/// Encodes an identified timestamp and its optional precision bound.
[[nodiscard]] inline TfTimestamp encode_timestamp(
    const rbt::time::Timestamp& timestamp,
    std::optional<std::uint64_t> uncertainty_ns) {
    if (!timestamp.clock.is_well_formed()) {
        throw TfError(rbt::time::TimeError::InvalidTimestamp);
    }
    std::uint8_t kind = 0U;
    switch (timestamp.clock.kind()) {
    case rbt::time::ClockKind::HostMonotonic:
        kind = 1U;
        break;
    case rbt::time::ClockKind::SystemRealtime:
        kind = 2U;
        break;
    case rbt::time::ClockKind::Device:
        kind = 3U;
        break;
    }
    TfTimestamp wire {};
    wire.kind = kind;
    wire.flags = uncertainty_ns.has_value() ? kTimestampUncertaintyKnown : 0U;
    std::copy(timestamp.clock.source_id().bytes().begin(),
              timestamp.clock.source_id().bytes().end(),
              std::begin(wire.source_id));
    wire.generation = timestamp.clock.generation();
    wire.nanoseconds = timestamp.ns;
    wire.uncertainty_ns = uncertainty_ns.value_or(0U);
    return wire;
}

/// Decodes a timestamp, rejecting unknown flags and noncanonical fields.
[[nodiscard]] inline std::pair<rbt::time::Timestamp, std::optional<std::uint64_t>> decode_timestamp(
    const TfTimestamp& wire) {
    if (!std::all_of(std::begin(wire.reserved), std::end(wire.reserved), [](std::uint8_t byte) {
            return byte == 0U;
        })
        || (wire.flags & static_cast<std::uint8_t>(~kTimestampUncertaintyKnown)) != 0U) {
        throw TfError(TfErrorCode::InvalidTransform);
    }

    rbt::time::ClockKind kind;
    switch (wire.kind) {
    case 1U:
        kind = rbt::time::ClockKind::HostMonotonic;
        break;
    case 2U:
        kind = rbt::time::ClockKind::SystemRealtime;
        break;
    case 3U:
        kind = rbt::time::ClockKind::Device;
        break;
    default:
        throw TfError(TfErrorCode::InvalidTransform);
    }

    std::optional<std::uint64_t> uncertainty_ns;
    if ((wire.flags & kTimestampUncertaintyKnown) != 0U) {
        uncertainty_ns = wire.uncertainty_ns;
    } else if (wire.uncertainty_ns != 0U) {
        throw TfError(TfErrorCode::InvalidTransform);
    }

    std::array<std::uint8_t, 48U> source_bytes {};
    std::copy(std::begin(wire.source_id), std::end(wire.source_id), source_bytes.begin());
    auto source_id = rbt::time::ClockSourceId(std::move(source_bytes));
    auto clock = rbt::time::ClockIdentity::create(kind, std::move(source_id), wire.generation);
    if (!clock) {
        throw TfError(clock.error());
    }
    auto timestamp = rbt::time::Timestamp::create(std::move(clock).value(), wire.nanoseconds);
    if (!timestamp) {
        throw TfError(timestamp.error());
    }
    return { std::move(timestamp).value(), uncertainty_ns };
}

/// Encodes a validated transform in canonical xyzw quaternion order.
[[nodiscard]] inline TfTransform3 encode_transform(const Transform3& transform) {
    const auto translation = transform.translation();
    const auto rotation = transform.rotation();
    const auto x = translation.x();
    const auto y = translation.y();
    const auto z = translation.z();
    const auto qx = rotation.x();
    const auto qy = rotation.y();
    const auto qz = rotation.z();
    const auto qw = rotation.w();
    return TfTransform3 { TfVec3 { x, y, z }, TfQuaternion { qx, qy, qz, qw } };
}

/// Decodes and validates a transform record.
[[nodiscard]] inline Transform3 decode_transform(const TfTransform3& wire) {
    return Transform3(Vec3(wire.translation.x, wire.translation.y, wire.translation.z),
                      Quaternion(wire.rotation.x, wire.rotation.y, wire.rotation.z, wire.rotation.w));
}

/// Encodes a static edge.
[[nodiscard]] inline StaticTransform encode_static_edge(const StaticEdge& edge) {
    StaticTransform wire {};
    const auto parent = edge.parent.to_bytes();
    const auto child = edge.child.to_bytes();
    std::copy(parent.begin(), parent.end(), std::begin(wire.parent.bytes));
    std::copy(child.begin(), child.end(), std::begin(wire.child.bytes));
    wire.transform = encode_transform(edge.transform);
    return wire;
}

/// Decodes a static edge and validates both frame IDs and its transform.
[[nodiscard]] inline StaticEdge decode_static_edge(const StaticTransform& wire) {
    FrameId::Bytes parent_bytes {};
    FrameId::Bytes child_bytes {};
    std::copy(std::begin(wire.parent.bytes), std::end(wire.parent.bytes), parent_bytes.begin());
    std::copy(std::begin(wire.child.bytes), std::end(wire.child.bytes), child_bytes.begin());
    return StaticEdge { FrameId::from_bytes(parent_bytes),
                        FrameId::from_bytes(child_bytes),
                        decode_transform(wire.transform) };
}

/// Encodes one dynamic sample together with its physical authority.
[[nodiscard]] inline DynamicTransform encode_dynamic(const CapabilityInstanceId& authority,
                                                     const DynamicRecord& record) {
    DynamicTransform wire {};
    const auto bytes = detail::parse_uuid(authority);
    if (!bytes || !detail::nonzero(*bytes)) {
        throw TfError(TfErrorCode::InvalidTransform);
    }
    std::copy(bytes->begin(), bytes->end(), std::begin(wire.authority));
    const auto parent = record.parent.to_bytes();
    const auto child = record.child.to_bytes();
    std::copy(parent.begin(), parent.end(), std::begin(wire.parent.bytes));
    std::copy(child.begin(), child.end(), std::begin(wire.child.bytes));
    wire.stamp = encode_timestamp(record.stamp, record.uncertainty_ns);
    wire.transform = encode_transform(record.transform);
    wire.transform_epoch = record.transform_epoch;
    return wire;
}

/// Decodes one dynamic sample after checking its physical authority.
[[nodiscard]] inline DynamicRecord decode_dynamic(const CapabilityInstanceId& expected,
                                                  const DynamicTransform& wire) {
    validate_wire_authority(expected, wire.authority);
    const auto [stamp, uncertainty_ns] = decode_timestamp(wire.stamp);
    FrameId::Bytes parent_bytes {};
    FrameId::Bytes child_bytes {};
    std::copy(std::begin(wire.parent.bytes), std::end(wire.parent.bytes), parent_bytes.begin());
    std::copy(std::begin(wire.child.bytes), std::end(wire.child.bytes), child_bytes.begin());
    return DynamicRecord { FrameId::from_bytes(parent_bytes),
                           FrameId::from_bytes(child_bytes),
                           stamp,
                           uncertainty_ns,
                           wire.transform_epoch,
                           decode_transform(wire.transform) };
}

/// Builds a successful static snapshot response header.
[[nodiscard]] inline StaticHeader static_snapshot_header(const CapabilityInstanceId& authority,
                                                         const StaticSet& set) {
    StaticHeader header {};
    header.revision = set.revision;
    encode_static_header(authority, set, header.authority, header.status, header.count,
                         header.effective_from);
    return header;
}

/// Builds a successful static update header.
[[nodiscard]] inline StaticUpdateHeader static_update_header(const CapabilityInstanceId& authority,
                                                              const StaticSet& set) {
    StaticUpdateHeader header {};
    header.revision = set.revision;
    encode_static_header(authority, set, header.authority, header.status, header.count,
                         header.effective_from);
    return header;
}

/// Decodes a full static snapshot, checking authority, status, count and bounds.
[[nodiscard]] inline StaticSet decode_static_snapshot(const CapabilityInstanceId& expected,
                                                      const StaticHeader& header,
                                                      std::span<const StaticTransform> payload,
                                                      std::size_t max) {
    return decode_static_payload(expected, header.authority, header.revision, header.status, header.count,
                                 header.effective_from, payload, max);
}

/// Decodes one complete static update, checking authority, status, count and bounds.
[[nodiscard]] inline StaticSet decode_static_update(const CapabilityInstanceId& expected,
                                                    const StaticUpdateHeader& header,
                                                    std::span<const StaticTransform> payload,
                                                    std::size_t max) {
    return decode_static_payload(expected, header.authority, header.revision, header.status, header.count,
                                 header.effective_from, payload, max);
}

inline void validate_wire_authority(const CapabilityInstanceId& expected,
                                    const std::uint8_t (&authority)[16]) {
    const auto expected_bytes = detail::parse_uuid(expected);
    const bool authority_nonzero = std::any_of(std::begin(authority), std::end(authority), [](std::uint8_t byte) {
        return byte != 0U;
    });
    if (!expected_bytes || !detail::nonzero(*expected_bytes) || !authority_nonzero) {
        throw TfError(TfErrorCode::InvalidTransform);
    }
    if (!std::equal(expected_bytes->begin(), expected_bytes->end(), std::begin(authority))) {
        throw TfError(TfErrorCode::ConflictingAuthority);
    }
}

inline void encode_static_header(const CapabilityInstanceId& authority,
                                 const StaticSet& set,
                                 std::uint8_t (&wire_authority)[16],
                                 std::uint32_t& status,
                                 std::uint32_t& count,
                                 TfTimestamp& effective_from) {
    const auto bytes = detail::parse_uuid(authority);
    if (!bytes || !detail::nonzero(*bytes)) {
        throw TfError(TfErrorCode::InvalidTransform);
    }
    std::copy(bytes->begin(), bytes->end(), std::begin(wire_authority));
    status = kStaticStatusSuccess;
    count = set.transforms.size() > std::numeric_limits<std::uint32_t>::max()
                ? std::numeric_limits<std::uint32_t>::max()
                : static_cast<std::uint32_t>(set.transforms.size());
    effective_from = encode_timestamp(set.effective_from, set.effective_uncertainty_ns);
}

inline StaticSet decode_static_payload(const CapabilityInstanceId& expected,
                                       const std::uint8_t (&authority)[16],
                                       std::uint64_t revision,
                                       std::uint32_t status,
                                       std::uint32_t count,
                                       const TfTimestamp& effective_from_wire,
                                       std::span<const StaticTransform> payload,
                                       std::size_t max) {
    validate_wire_authority(expected, authority);
    if (status != kStaticStatusSuccess) {
        if (count != 0U || !payload.empty()) {
            throw TfError(TfErrorCode::InvalidTransform);
        }
        if (status == kStaticStatusSnapshotTooLarge) {
            throw TfError(TfErrorCode::SnapshotTooLarge);
        }
        if (status == kStaticStatusProviderUnavailable) {
            throw TfError(TfErrorCode::ProviderUnavailable);
        }
        throw TfError(TfErrorCode::InvalidTransform);
    }
    if (payload.size() > max) {
        throw TfError(TfErrorCode::SnapshotTooLarge);
    }
    if (payload.size() != static_cast<std::size_t>(count)) {
        throw TfError(TfErrorCode::InvalidTransform);
    }
    const auto [effective_from, effective_uncertainty_ns] = decode_timestamp(effective_from_wire);
    StaticSet set;
    set.revision = revision;
    set.effective_from = effective_from;
    set.effective_uncertainty_ns = effective_uncertainty_ns;
    try {
        set.transforms.reserve(payload.size());
        for (const auto& wire_edge : payload) {
            set.transforms.push_back(decode_static_edge(wire_edge));
        }
    } catch (const std::bad_alloc&) {
        throw TfError(TfErrorCode::CapacityExceeded);
    } catch (const std::length_error&) {
        throw TfError(TfErrorCode::CapacityExceeded);
    }
    return set;
}

} // namespace rbt::tf

#endif
