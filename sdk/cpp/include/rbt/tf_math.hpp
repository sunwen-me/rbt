#ifndef RBT_CPP_TF_MATH_HPP
#define RBT_CPP_TF_MATH_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <rbt/time.hpp>

namespace rbt::tf {

/// Stable transform errors shared by local math and transform-buffer APIs.
enum class TfErrorCode : std::uint8_t {
    UnknownFrame,
    DisconnectedFrames,
    MissingTransform,
    ConflictingParent,
    ConflictingAuthority,
    CycleDetected,
    InvalidTransform,
    InvalidQuaternion,
    InvalidFrame,
    PastExtrapolation,
    FutureExtrapolation,
    InsufficientHistory,
    InterpolationGapTooLarge,
    TransformEpochMismatch,
    ProviderUnavailable,
    SnapshotStale,
    SnapshotRevisionGap,
    SnapshotTooLarge,
    CapacityExceeded,
    InvalidConfig,
    TimestampUncertaintyExceeded,
    OutOfOrderSample,
    ClockMappingUnavailable,
    ClockDomainMismatch,
    ClockGenerationMismatch,
    ClockMappingExpired,
    UnknownUncertainty,
    Time,
};

[[nodiscard]] constexpr const char* to_string(TfErrorCode code) noexcept {
    switch (code) {
    case TfErrorCode::UnknownFrame:
        return "UnknownFrame";
    case TfErrorCode::DisconnectedFrames:
        return "DisconnectedFrames";
    case TfErrorCode::MissingTransform:
        return "MissingTransform";
    case TfErrorCode::ConflictingParent:
        return "ConflictingParent";
    case TfErrorCode::ConflictingAuthority:
        return "ConflictingAuthority";
    case TfErrorCode::CycleDetected:
        return "CycleDetected";
    case TfErrorCode::InvalidTransform:
        return "InvalidTransform";
    case TfErrorCode::InvalidQuaternion:
        return "InvalidQuaternion";
    case TfErrorCode::InvalidFrame:
        return "InvalidFrame";
    case TfErrorCode::PastExtrapolation:
        return "PastExtrapolation";
    case TfErrorCode::FutureExtrapolation:
        return "FutureExtrapolation";
    case TfErrorCode::InsufficientHistory:
        return "InsufficientHistory";
    case TfErrorCode::InterpolationGapTooLarge:
        return "InterpolationGapTooLarge";
    case TfErrorCode::TransformEpochMismatch:
        return "TransformEpochMismatch";
    case TfErrorCode::ProviderUnavailable:
        return "ProviderUnavailable";
    case TfErrorCode::SnapshotStale:
        return "SnapshotStale";
    case TfErrorCode::SnapshotRevisionGap:
        return "SnapshotRevisionGap";
    case TfErrorCode::SnapshotTooLarge:
        return "SnapshotTooLarge";
    case TfErrorCode::CapacityExceeded:
        return "CapacityExceeded";
    case TfErrorCode::InvalidConfig:
        return "InvalidConfig";
    case TfErrorCode::TimestampUncertaintyExceeded:
        return "TimestampUncertaintyExceeded";
    case TfErrorCode::OutOfOrderSample:
        return "OutOfOrderSample";
    case TfErrorCode::ClockMappingUnavailable:
        return "ClockMappingUnavailable";
    case TfErrorCode::ClockDomainMismatch:
        return "ClockDomainMismatch";
    case TfErrorCode::ClockGenerationMismatch:
        return "ClockGenerationMismatch";
    case TfErrorCode::ClockMappingExpired:
        return "ClockMappingExpired";
    case TfErrorCode::UnknownUncertainty:
        return "UnknownUncertainty";
    case TfErrorCode::Time:
        return "Time";
    }
    return "InvalidTransform";
}

/// Exception carrying the stable TF error code for a failed operation.
class TfError final : public std::runtime_error {
  public:
    explicit TfError(TfErrorCode code)
        : std::runtime_error(to_string(code)), code_(code) {
    }

    explicit TfError(rbt::time::TimeError time_error)
        : std::runtime_error(message_for_time_error(time_error))
        , code_(code_for_time_error(time_error))
        , time_error_(time_error) {
    }

    TfError(TfErrorCode code, rbt::time::TimeError time_error)
        : std::runtime_error(to_string(code))
        , code_(code)
        , time_error_(time_error) {
    }

    [[nodiscard]] TfErrorCode code() const noexcept {
        return code_;
    }

    [[nodiscard]] std::optional<rbt::time::TimeError> time_error() const noexcept {
        return time_error_;
    }

  private:
    [[nodiscard]] static constexpr TfErrorCode code_for_time_error(
        rbt::time::TimeError error) noexcept {
        switch (error) {
        case rbt::time::TimeError::ClockMappingUnavailable:
            return TfErrorCode::ClockMappingUnavailable;
        case rbt::time::TimeError::ClockDomainMismatch:
            return TfErrorCode::ClockDomainMismatch;
        case rbt::time::TimeError::ClockGenerationMismatch:
            return TfErrorCode::ClockGenerationMismatch;
        case rbt::time::TimeError::ClockMappingExpired:
            return TfErrorCode::ClockMappingExpired;
        case rbt::time::TimeError::UnknownUncertainty:
            return TfErrorCode::UnknownUncertainty;
        default:
            return TfErrorCode::Time;
        }
    }

    [[nodiscard]] static constexpr const char* message_for_time_error(
        rbt::time::TimeError error) noexcept {
        return rbt::time::to_string(error);
    }

    TfErrorCode code_;
    std::optional<rbt::time::TimeError> time_error_;
};

/// A case-sensitive coordinate-frame identifier in its canonical 64-byte form.
class FrameId {
  public:
    using Bytes = std::array<std::uint8_t, 64U>;

    explicit FrameId(std::string_view value) {
        if (!valid_name(value)) {
            throw TfError(TfErrorCode::InvalidFrame);
        }
        std::copy(value.begin(), value.end(), bytes_.begin());
    }

    [[nodiscard]] static FrameId from_bytes(const Bytes& bytes) {
        const auto zero = std::find(bytes.begin(), bytes.end(), std::uint8_t { 0U });
        if (zero == bytes.end()) {
            throw TfError(TfErrorCode::InvalidFrame);
        }
        const auto length = static_cast<std::size_t>(zero - bytes.begin());
        if (!std::all_of(zero, bytes.end(), [](std::uint8_t byte) { return byte == 0U; })) {
            throw TfError(TfErrorCode::InvalidFrame);
        }
        const std::string_view name(reinterpret_cast<const char*>(bytes.data()), length);
        if (!valid_name(name)) {
            throw TfError(TfErrorCode::InvalidFrame);
        }
        return FrameId(bytes, PrivateTag {});
    }

    [[nodiscard]] Bytes to_bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::string_view str() const noexcept {
        const auto zero = std::find(bytes_.begin(), bytes_.end(), std::uint8_t { 0U });
        const auto length = static_cast<std::size_t>(zero - bytes_.begin());
        return std::string_view(reinterpret_cast<const char*>(bytes_.data()), length);
    }

    [[nodiscard]] friend bool operator==(const FrameId&, const FrameId&) = default;

  private:
    struct PrivateTag {};

    FrameId(const Bytes& bytes, PrivateTag) noexcept
        : bytes_(bytes) {
    }

    [[nodiscard]] static bool valid_name(std::string_view value) noexcept {
        if (value.empty() || value.size() > 63U) {
            return false;
        }

        std::size_t segment_start = 0U;
        for (std::size_t index = 0U; index < value.size(); ++index) {
            const auto byte = static_cast<unsigned char>(value[index]);
            if (byte == static_cast<unsigned char>('/')) {
                if (!valid_segment(value.substr(segment_start, index - segment_start))) {
                    return false;
                }
                segment_start = index + 1U;
            } else if (byte > 0x7FU || byte == 0U) {
                return false;
            }
        }
        return valid_segment(value.substr(segment_start));
    }

    [[nodiscard]] static bool valid_segment(std::string_view segment) noexcept {
        if (segment.empty()) {
            return false;
        }
        const auto is_alphanumeric = [](unsigned char byte) {
            return (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z'))
                   || (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
                   || (byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9'));
        };

        const auto first = static_cast<unsigned char>(segment.front());
        if (!is_alphanumeric(first) && first != static_cast<unsigned char>('_')) {
            return false;
        }
        if (segment == "." || segment == "..") {
            return false;
        }
        for (std::size_t index = 1U; index < segment.size(); ++index) {
            const auto byte = static_cast<unsigned char>(segment[index]);
            if (!is_alphanumeric(byte) && byte != static_cast<unsigned char>('_')
                && byte != static_cast<unsigned char>('.') && byte != static_cast<unsigned char>('-')) {
                return false;
            }
        }
        return true;
    }

    Bytes bytes_ {};
};

/// A finite three-dimensional vector.
class Vec3 {
  public:
    Vec3(double x, double y, double z)
        : x_(x), y_(y), z_(z) {
        if (!finite(x_) || !finite(y_) || !finite(z_)) {
            throw TfError(TfErrorCode::InvalidTransform);
        }
    }

    [[nodiscard]] static constexpr Vec3 zero() noexcept {
        return Vec3(0.0, 0.0, 0.0, PrivateTag {});
    }

    [[nodiscard]] constexpr double x() const noexcept {
        return x_;
    }

    [[nodiscard]] constexpr double y() const noexcept {
        return y_;
    }

    [[nodiscard]] constexpr double z() const noexcept {
        return z_;
    }

    [[nodiscard]] friend bool operator==(const Vec3&, const Vec3&) = default;

  private:
    struct PrivateTag {};

    constexpr Vec3(double x, double y, double z, PrivateTag) noexcept
        : x_(x), y_(y), z_(z) {
    }

    [[nodiscard]] static constexpr bool finite(double value) noexcept {
        return value <= std::numeric_limits<double>::max()
               && value >= -std::numeric_limits<double>::max();
    }

    [[nodiscard]] Vec3 checked_add(Vec3 rhs) const {
        return Vec3(x_ + rhs.x_, y_ + rhs.y_, z_ + rhs.z_);
    }

    [[nodiscard]] Vec3 checked_negate() const {
        return Vec3(-x_, -y_, -z_);
    }

    [[nodiscard]] Vec3 interpolate(Vec3 rhs, double amount) const {
        return Vec3(interpolate_scalar(x_, rhs.x_, amount),
                    interpolate_scalar(y_, rhs.y_, amount),
                    interpolate_scalar(z_, rhs.z_, amount));
    }

    static double interpolate_scalar(double lhs, double rhs, double amount) noexcept;

    double x_;
    double y_;
    double z_;

    friend class Quaternion;
    friend class Transform3;
};

/// A unit quaternion in x, y, z, w component order.
class Quaternion {
  public:
    Quaternion(double x, double y, double z, double w) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w)) {
            throw TfError(TfErrorCode::InvalidQuaternion);
        }
        const auto norm = std::hypot(std::hypot(x, y), std::hypot(z, w));
        if (!std::isfinite(norm) || norm == 0.0 || std::abs(norm - 1.0) > 1.0e-6) {
            throw TfError(TfErrorCode::InvalidQuaternion);
        }
        x_ = x / norm;
        y_ = y / norm;
        z_ = z / norm;
        w_ = w / norm;
    }

    [[nodiscard]] static constexpr Quaternion identity() noexcept {
        return Quaternion(0.0, 0.0, 0.0, 1.0, PrivateTag {});
    }

    [[nodiscard]] constexpr double x() const noexcept {
        return x_;
    }

    [[nodiscard]] constexpr double y() const noexcept {
        return y_;
    }

    [[nodiscard]] constexpr double z() const noexcept {
        return z_;
    }

    [[nodiscard]] constexpr double w() const noexcept {
        return w_;
    }

    [[nodiscard]] friend bool operator==(const Quaternion&, const Quaternion&) = default;

  private:
    struct PrivateTag {};

    constexpr Quaternion(double x, double y, double z, double w, PrivateTag) noexcept
        : x_(x), y_(y), z_(z), w_(w) {
    }

    [[nodiscard]] static Quaternion normalized_result(double x, double y, double z, double w) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w)) {
            throw TfError(TfErrorCode::InvalidQuaternion);
        }
        const auto norm = std::hypot(std::hypot(x, y), std::hypot(z, w));
        if (!std::isfinite(norm) || norm == 0.0) {
            throw TfError(TfErrorCode::InvalidQuaternion);
        }
        return Quaternion(x / norm, y / norm, z / norm, w / norm, PrivateTag {});
    }

    [[nodiscard]] Quaternion conjugate() const noexcept {
        return Quaternion(-x_, -y_, -z_, w_, PrivateTag {});
    }

    [[nodiscard]] Quaternion checked_multiply(Quaternion rhs) const {
        return normalized_result(w_ * rhs.x_ + x_ * rhs.w_ + y_ * rhs.z_ - z_ * rhs.y_,
                                 w_ * rhs.y_ - x_ * rhs.z_ + y_ * rhs.w_ + z_ * rhs.x_,
                                 w_ * rhs.z_ + x_ * rhs.y_ - y_ * rhs.x_ + z_ * rhs.w_,
                                 w_ * rhs.w_ - x_ * rhs.x_ - y_ * rhs.y_ - z_ * rhs.z_);
    }

    [[nodiscard]] Quaternion slerp(Quaternion rhs, double amount) const {
        if (amount == 0.0) {
            return *this;
        }
        if (amount == 1.0) {
            return rhs;
        }

        auto dot = x_ * rhs.x_ + y_ * rhs.y_ + z_ * rhs.z_ + w_ * rhs.w_;
        if (dot < 0.0) {
            rhs = Quaternion(-rhs.x_, -rhs.y_, -rhs.z_, -rhs.w_, PrivateTag {});
            dot = -dot;
        }
        dot = std::clamp(dot, -1.0, 1.0);

        if (dot > 0.9995) {
            return normalized_result(interpolate_scalar(x_, rhs.x_, amount),
                                     interpolate_scalar(y_, rhs.y_, amount),
                                     interpolate_scalar(z_, rhs.z_, amount),
                                     interpolate_scalar(w_, rhs.w_, amount));
        }

        const auto theta = std::acos(dot);
        const auto sin_theta = std::sin(theta);
        if (!std::isfinite(sin_theta) || sin_theta == 0.0) {
            throw TfError(TfErrorCode::InvalidQuaternion);
        }
        const auto lhs_weight = std::sin((1.0 - amount) * theta) / sin_theta;
        const auto rhs_weight = std::sin(amount * theta) / sin_theta;
        return normalized_result(x_ * lhs_weight + rhs.x_ * rhs_weight,
                                 y_ * lhs_weight + rhs.y_ * rhs_weight,
                                 z_ * lhs_weight + rhs.z_ * rhs_weight,
                                 w_ * lhs_weight + rhs.w_ * rhs_weight);
    }

    [[nodiscard]] Vec3 rotate_vector(Vec3 vector) const {
        const auto scale = std::max({ std::abs(vector.x_), std::abs(vector.y_), std::abs(vector.z_) });
        if (scale == 0.0) {
            return Vec3::zero();
        }

        const auto x = vector.x_ / scale;
        const auto y = vector.y_ / scale;
        const auto z = vector.z_ / scale;
        const auto xx = x_ * x_;
        const auto yy = y_ * y_;
        const auto zz = z_ * z_;
        const auto xy = x_ * y_;
        const auto xz = x_ * z_;
        const auto yz = y_ * z_;
        const auto xw = x_ * w_;
        const auto yw = y_ * w_;
        const auto zw = z_ * w_;

        const auto rx = ((1.0 - 2.0 * (yy + zz)) * x) + (2.0 * (xy - zw) * y)
                        + (2.0 * (xz + yw) * z);
        const auto ry = (2.0 * (xy + zw) * x) + ((1.0 - 2.0 * (xx + zz)) * y)
                        + (2.0 * (yz - xw) * z);
        const auto rz = (2.0 * (xz - yw) * x) + (2.0 * (yz + xw) * y)
                        + ((1.0 - 2.0 * (xx + yy)) * z);
        return Vec3(rx * scale, ry * scale, rz * scale);
    }

    static double interpolate_scalar(double lhs, double rhs, double amount) noexcept;

    double x_;
    double y_;
    double z_;
    double w_;

    friend class Transform3;
};

/// A rigid transform that maps points from a source frame into a target frame.
class Pose3;

class Transform3 {
  public:
    constexpr Transform3(Vec3 translation, Quaternion rotation) noexcept
        : translation_(translation), rotation_(rotation) {
    }

    [[nodiscard]] static constexpr Transform3 identity() noexcept {
        return Transform3(Vec3::zero(), Quaternion::identity());
    }

    [[nodiscard]] constexpr Vec3 translation() const noexcept {
        return translation_;
    }

    [[nodiscard]] constexpr Quaternion rotation() const noexcept {
        return rotation_;
    }

    /// Returns the inverse transform, reversing source and target frames.
    [[nodiscard]] Transform3 inverse() const {
        const auto inverse_rotation = rotation_.conjugate();
        const auto inverse_translation = inverse_rotation.rotate_vector(translation_.checked_negate());
        return Transform3(inverse_translation, inverse_rotation);
    }

    /// Composes `T_target<-middle * T_middle<-source`.
    [[nodiscard]] Transform3 compose(Transform3 rhs) const {
        return Transform3(apply_point(rhs.translation_), rotation_.checked_multiply(rhs.rotation_));
    }

    /// Rotates and translates a point.
    [[nodiscard]] Vec3 apply_point(Vec3 point) const {
        return rotation_.rotate_vector(point).checked_add(translation_);
    }

    /// Rotates a vector without applying translation.
    [[nodiscard]] Vec3 apply_vector(Vec3 vector) const {
        return rotation_.rotate_vector(vector);
    }

    /// Applies this transform to both the position and orientation of a pose.
    [[nodiscard]] Pose3 apply_pose(Pose3 pose) const;

    /// Interpolates translation linearly and rotation with shortest-path SLERP.
    [[nodiscard]] Transform3 interpolate(Transform3 rhs, double amount) const {
        if (!std::isfinite(amount) || amount < 0.0 || amount > 1.0) {
            throw TfError(TfErrorCode::InvalidTransform);
        }
        return Transform3(translation_.interpolate(rhs.translation_, amount),
                          rotation_.slerp(rhs.rotation_, amount));
    }

    [[nodiscard]] friend bool operator==(const Transform3&, const Transform3&) = default;

  private:
    Vec3 translation_;
    Quaternion rotation_;
};

/// A rigid pose containing a position and orientation.
class Pose3 {
  public:
    constexpr Pose3(Vec3 position, Quaternion orientation) noexcept
        : position_(position), orientation_(orientation) {
    }

    [[nodiscard]] static constexpr Pose3 identity() noexcept {
        return Pose3(Vec3::zero(), Quaternion::identity());
    }

    [[nodiscard]] constexpr Vec3 position() const noexcept {
        return position_;
    }

    [[nodiscard]] constexpr Quaternion orientation() const noexcept {
        return orientation_;
    }

    [[nodiscard]] friend bool operator==(const Pose3&, const Pose3&) = default;

  private:
    Vec3 position_;
    Quaternion orientation_;

    friend class Transform3;
};

inline double Vec3::interpolate_scalar(double lhs, double rhs, double amount) noexcept {
    if (amount == 0.0) {
        return lhs;
    }
    if (amount == 1.0) {
        return rhs;
    }
    return std::fma(lhs, 1.0 - amount, rhs * amount);
}

inline double Quaternion::interpolate_scalar(double lhs, double rhs, double amount) noexcept {
    if (amount == 0.0) {
        return lhs;
    }
    if (amount == 1.0) {
        return rhs;
    }
    return std::fma(lhs, 1.0 - amount, rhs * amount);
}

inline Pose3 Transform3::apply_pose(Pose3 pose) const {
    return Pose3(apply_point(pose.position_), rotation_.checked_multiply(pose.orientation_));
}

} // namespace rbt::tf

#endif
