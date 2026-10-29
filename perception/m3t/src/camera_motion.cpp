#include <rbt_m3t/camera_motion.hpp>

#include <rbt/runtime.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace rbt_m3t {
namespace {

constexpr double kRigidTolerance = 1.0e-5;
constexpr std::size_t kMaximumPitchSamples = 256U;
constexpr std::uint64_t kPitchHistoryNs = 10'000'000'000ULL;
constexpr std::uint64_t kMaximumTimestampUncertaintyNs = 1'000'000U;

[[nodiscard]] bool valid_frame_prefix(const std::string& value) noexcept {
    if (value.find('/') != std::string::npos) {
        return false;
    }
    try {
        (void)rbt::tf::FrameId(value);
        return true;
    } catch (const rbt::tf::TfError&) {
        return false;
    }
}

[[nodiscard]] bool is_rigid_pose(const Eigen::Affine3f& pose) noexcept {
    const Eigen::Matrix4d matrix = pose.matrix().cast<double>();
    if (!matrix.array().isFinite().all()) {
        return false;
    }
    if ((matrix.row(3) - Eigen::RowVector4d { 0.0, 0.0, 0.0, 1.0 })
            .cwiseAbs()
            .maxCoeff()
        > kRigidTolerance) {
        return false;
    }
    const Eigen::Matrix3d rotation = matrix.topLeftCorner<3, 3>();
    if ((rotation.transpose() * rotation - Eigen::Matrix3d::Identity())
            .cwiseAbs()
            .maxCoeff()
        > kRigidTolerance) {
        return false;
    }
    return std::abs(rotation.determinant() - 1.0) <= kRigidTolerance;
}

[[nodiscard]] rbt::tf::Transform3 to_tf_transform(const Eigen::Affine3f& pose) {
    if (!is_rigid_pose(pose)) {
        throw std::invalid_argument("camera-motion transform must be a finite proper rigid transform");
    }
    const Eigen::Matrix4d matrix = pose.matrix().cast<double>();
    const Eigen::Quaterniond rotation(matrix.topLeftCorner<3, 3>());
    const auto normalized = rotation.normalized();
    return rbt::tf::Transform3(
        rbt::tf::Vec3(matrix(0, 3), matrix(1, 3), matrix(2, 3)),
        rbt::tf::Quaternion(normalized.x(), normalized.y(), normalized.z(), normalized.w()));
}

[[nodiscard]] Eigen::Affine3d to_eigen_transform(const rbt::tf::Transform3& transform) {
    const auto translation = transform.translation();
    const auto rotation = transform.rotation();
    Eigen::Affine3d pose = Eigen::Affine3d::Identity();
    pose.translation() = Eigen::Vector3d(translation.x(), translation.y(), translation.z());
    pose.linear() = Eigen::Quaterniond(rotation.w(), rotation.x(), rotation.y(), rotation.z())
                        .toRotationMatrix();
    if (!pose.matrix().array().isFinite().all()) {
        throw std::invalid_argument("TF lookup returned a non-finite transform");
    }
    return pose;
}

[[nodiscard]] Eigen::Affine3f to_eigen_float(const rbt::tf::Transform3& transform) {
    const Eigen::Affine3d pose = to_eigen_transform(transform);
    const Eigen::Affine3f result = pose.cast<float>();
    if (!is_rigid_pose(result)) {
        throw std::invalid_argument("TF lookup transform exceeds Eigen float range");
    }
    return result;
}

[[nodiscard]] std::optional<std::uint64_t> combined_uncertainty(
    std::optional<std::uint64_t> lhs,
    std::optional<std::uint64_t> rhs) noexcept {
    if (!lhs || !rhs) {
        return {};
    }
    return std::max(*lhs, *rhs);
}

[[nodiscard]] std::uint64_t timestamp_distance_ns(std::int64_t lhs,
                                                  std::int64_t rhs) noexcept {
    if (lhs >= rhs) {
        return static_cast<std::uint64_t>(lhs) - static_cast<std::uint64_t>(rhs);
    }
    return static_cast<std::uint64_t>(rhs) - static_cast<std::uint64_t>(lhs);
}

[[nodiscard]] std::int64_t signed_timestamp_offset_ns(std::int64_t sample,
                                                      std::int64_t image,
                                                      std::uint64_t absolute_offset) {
    if (absolute_offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw rbt::tf::TfError(rbt::tf::TfErrorCode::InsufficientHistory);
    }
    const auto offset = static_cast<std::int64_t>(absolute_offset);
    return sample >= image ? offset : -offset;
}

} // namespace

class CameraMotion::Impl {
  public:
    explicit Impl(CameraMotionConfig config)
        : config_(std::move(config))
        , buffer_(resources_for(config_), policy_for(config_)) {
        validate_config();
        mount_slot_ = "rbt_m3t_camera_mount_" + rbt::generate_uuid().substr(0U, 8U);
        mount_physical_id_ = rbt::generate_uuid();
    }

    [[nodiscard]] rbt::tf::TransformBuffer& buffer() noexcept {
        return buffer_;
    }

    void bind_pitch(rbt::CapabilityInstanceId id) {
        const std::lock_guard lock(pitch_mutex_);
        if (pitch_authority_) {
            buffer_.unbind_provider(*pitch_authority_);
            pitch_authority_.reset();
        }
        pitch_authority_ = buffer_.bind_provider("m3t-camera-pitch", std::move(id));
        pitch_samples_.clear();
    }

    void ingest_pitch(const rbt::tf::DynamicRecord& record) {
        const std::lock_guard lock(pitch_mutex_);
        if (!pitch_authority_) {
            throw rbt::tf::TfError(rbt::tf::TfErrorCode::ProviderUnavailable);
        }
        if (record.parent.str() != std::string_view(config_.world_frame)
            || record.child.str() != std::string_view(config_.pitch_frame)) {
            throw rbt::tf::TfError(rbt::tf::TfErrorCode::ConflictingParent);
        }
        buffer_.ingest_dynamic(*pitch_authority_, record);

        if (!pitch_samples_.empty()) {
            const auto& previous = pitch_samples_.back();
            if (previous.stamp.clock != record.stamp.clock
                || previous.transform_epoch != record.transform_epoch) {
                pitch_samples_.clear();
            } else if (previous.stamp.ns == record.stamp.ns) {
                return;
            }
        }
        pitch_samples_.push_back(record);
        while (pitch_samples_.size() > kMaximumPitchSamples) {
            pitch_samples_.pop_front();
        }
        while (pitch_samples_.size() > 1U
               && timestamp_distance_ns(pitch_samples_.back().stamp.ns,
                                        pitch_samples_.front().stamp.ns)
                      > kPitchHistoryNs) {
            pitch_samples_.pop_front();
        }
    }

    [[nodiscard]] CameraMotionSelection select(const RectifiedPair& pair) {
        const std::lock_guard lock(pitch_mutex_);
        if (!pair.timing.capture.has_value()) {
            throw rbt::tf::TfError(rbt::tf::TfErrorCode::MissingTransform);
        }
        const auto& selected_timestamp = config_.time_basis == "capture"
                                             ? *pair.timing.capture
                                             : pair.timing.received;
        const auto selected_uncertainty = config_.time_basis == "capture"
                                              ? pair.timing.capture_uncertainty_ns
                                              : pair.timing.received_uncertainty_ns;
        if (!selected_timestamp.clock.is_well_formed()) {
            throw rbt::tf::TfError(rbt::time::TimeError::InvalidTimestamp);
        }
        if (!selected_uncertainty && !config_.allow_unknown_uncertainty) {
            throw rbt::tf::TfError(rbt::time::TimeError::UnknownUncertainty);
        }
        if (selected_uncertainty
            && *selected_uncertainty > kMaximumTimestampUncertaintyNs) {
            throw rbt::tf::TfError(rbt::tf::TfErrorCode::TimestampUncertaintyExceeded);
        }
        (void)rbt::tf::FrameId(pair.frame_id);

        if (pitch_samples_.empty()) {
            throw rbt::tf::TfError(rbt::tf::TfErrorCode::InsufficientHistory);
        }
        const auto& newest_pitch = pitch_samples_.back();
        if (newest_pitch.stamp.clock != selected_timestamp.clock) {
            throw rbt::tf::TfError(rbt::time::TimeError::ClockDomainMismatch);
        }
        const rbt::tf::DynamicRecord* nearest = nullptr;
        std::uint64_t nearest_offset = std::numeric_limits<std::uint64_t>::max();
        for (const auto& sample : pitch_samples_) {
            if (sample.stamp.clock != selected_timestamp.clock) {
                continue;
            }
            const auto offset = timestamp_distance_ns(sample.stamp.ns, selected_timestamp.ns);
            if (nearest == nullptr || offset < nearest_offset
                || (offset == nearest_offset && sample.stamp.ns < nearest->stamp.ns)) {
                nearest = &sample;
                nearest_offset = offset;
            }
        }
        if (nearest == nullptr) {
            throw rbt::tf::TfError(rbt::time::TimeError::ClockDomainMismatch);
        }
        const auto maximum_age_ns = static_cast<std::uint64_t>(config_.maximum_sample_age_ms)
                                    * 1'000'000U;
        if (nearest_offset > maximum_age_ns) {
            throw rbt::tf::TfError(rbt::tf::TfErrorCode::InsufficientHistory);
        }
        const auto pitch_offset = signed_timestamp_offset_ns(
            nearest->stamp.ns, selected_timestamp.ns, nearest_offset);
        ensure_mount_snapshot(nearest->stamp);

        rbt::tf::LookupOptions selected_options;
        selected_options.timestamp_uncertainty_ns = nearest->uncertainty_ns;
        const auto world_from_depth = buffer_.lookup_at(
            config_.world_frame, config_.depth_frame, nearest->stamp, selected_options);

        rbt::tf::LookupOptions capture_options;
        capture_options.timestamp_uncertainty_ns = pair.timing.capture_uncertainty_ns;
        const auto depth_from_color = buffer_.lookup_at(
            config_.depth_frame, pair.frame_id, *pair.timing.capture, capture_options);

        rbt::tf::LookupResult combined;
        combined.transform = world_from_depth.transform.compose(depth_from_color.transform);
        combined.requested_timestamp = selected_timestamp;
        combined.timing_uncertainty_ns = combined_uncertainty(
            combined_uncertainty(world_from_depth.timing_uncertainty_ns,
                                 depth_from_color.timing_uncertainty_ns),
            selected_uncertainty);
        combined.sources.reserve(world_from_depth.sources.size()
                                 + depth_from_color.sources.size());
        combined.sources.insert(combined.sources.end(), world_from_depth.sources.begin(),
                                world_from_depth.sources.end());
        combined.sources.insert(combined.sources.end(), depth_from_color.sources.begin(),
                                depth_from_color.sources.end());
        return CameraMotionSelection { to_eigen_float(combined.transform),
                                       std::move(combined),
                                       nearest->stamp,
                                       pitch_offset,
                                       nearest_offset,
                                       config_.time_basis,
                                       config_.world_frame,
                                       "nearest" };
    }

    [[nodiscard]] std::vector<ArmSeed> camera_seeds(
        const std::vector<ArmSeed>& seeds,
        const CameraMotionSelection& selection) const {
        if (seeds.size() != config_.arm_bases.size() || seeds.empty()) {
            throw std::invalid_argument("camera-motion arm bases must match the configured arms");
        }
        if (!is_rigid_pose(selection.camera_in_world)) {
            throw std::invalid_argument("camera-motion selection has an invalid camera pose");
        }

        std::map<std::string, const Eigen::Affine3f*> bases;
        for (const auto& arm_base : config_.arm_bases) {
            bases.emplace(arm_base.arm, &arm_base.base_in_world);
        }
        std::set<std::string> seen;
        const Eigen::Affine3d camera_in_world = selection.camera_in_world.cast<double>();
        const Eigen::Affine3d world_in_camera = camera_in_world.inverse();
        std::vector<ArmSeed> result = seeds;
        for (auto& seed : result) {
            const auto found = bases.find(seed.name);
            if (found == bases.end() || !seen.insert(seed.name).second || !seed.fixed_base
                || !is_rigid_pose(*found->second)) {
                throw std::invalid_argument("camera-motion seeds must be unique fixed-base configured arms");
            }
            const Eigen::Affine3d base_in_camera = world_in_camera * found->second->cast<double>();
            const Eigen::Affine3f converted = base_in_camera.cast<float>();
            if (!is_rigid_pose(converted)) {
                throw std::invalid_argument("camera-motion arm pose exceeds Eigen float range");
            }
            seed.base_in_camera = converted;
        }
        if (seen.size() != bases.size()) {
            throw std::invalid_argument("camera-motion arm bases must match the configured arms");
        }
        return result;
    }

  private:
    [[nodiscard]] static rbt::tf::TfResourceConfig resources_for(
        const CameraMotionConfig& config) {
        rbt::tf::TfResourceConfig resources;
        resources.max_history_duration = std::chrono::seconds(10);
        resources.max_dynamic_age = std::chrono::milliseconds(config.maximum_sample_age_ms);
        resources.snapshot_max_age = std::chrono::seconds(2);
        return resources;
    }

    [[nodiscard]] static rbt::tf::LookupPolicy policy_for(
        const CameraMotionConfig& config) {
        rbt::tf::LookupPolicy policy;
        policy.max_interpolation_gap = std::chrono::milliseconds(
            config.maximum_interpolation_gap_ms);
        policy.allow_unknown_uncertainty = config.allow_unknown_uncertainty;
        return policy;
    }

    void validate_config() const {
        if (!config_.enabled) {
            throw std::invalid_argument("camera-motion runtime requires enabled configuration");
        }
        try {
            (void)rbt::tf::FrameId(config_.world_frame);
            (void)rbt::tf::FrameId(config_.pitch_frame);
            (void)rbt::tf::FrameId(config_.depth_frame);
            (void)rbt::tf::FrameId(config_.pitch_provider);
        } catch (const rbt::tf::TfError&) {
            throw std::invalid_argument("camera-motion frames and provider must be valid RBT identifiers");
        }
        if (config_.world_frame == config_.pitch_frame
            || config_.world_frame == config_.depth_frame
            || config_.pitch_frame == config_.depth_frame) {
            throw std::invalid_argument("camera-motion world, pitch, and depth frames must be distinct");
        }
        if (config_.time_basis != "capture" && config_.time_basis != "received") {
            throw std::invalid_argument("camera-motion time_basis must be capture or received");
        }
        if (config_.maximum_interpolation_gap_ms < 1
            || config_.maximum_interpolation_gap_ms > 1000
            || config_.maximum_wait_ms < 0 || config_.maximum_wait_ms > 1000
            || config_.maximum_sample_age_ms < 1 || config_.maximum_sample_age_ms > 10'000) {
            throw std::invalid_argument("camera-motion time limits are outside the supported range");
        }
        if (!is_rigid_pose(config_.mount_in_pitch) || config_.arm_bases.empty()
            || config_.arm_bases.size() > 64U) {
            throw std::invalid_argument("camera-motion mount or arm bases are invalid");
        }
        std::set<std::string> arm_names;
        for (const auto& arm_base : config_.arm_bases) {
            if (!valid_frame_prefix(arm_base.arm) || !arm_names.insert(arm_base.arm).second
                || !is_rigid_pose(arm_base.base_in_world)) {
                throw std::invalid_argument("camera-motion arm bases must be valid and unique");
            }
        }
    }

    void ensure_mount_snapshot(const rbt::time::Timestamp& stamp) {
        if (!mount_clock_ || *mount_clock_ != stamp.clock) {
            mount_physical_id_ = rbt::generate_uuid();
            mount_authority_ = buffer_.bind_provider(mount_slot_, mount_physical_id_);
            mount_snapshot_ = rbt::tf::StaticSet {};
            mount_snapshot_.revision = 1U;
            mount_snapshot_.effective_from = rbt::time::Timestamp { stamp.clock, 0 };
            mount_snapshot_.effective_uncertainty_ns = 0U;
            mount_snapshot_.transforms.push_back(rbt::tf::StaticEdge {
                rbt::tf::FrameId(config_.pitch_frame),
                rbt::tf::FrameId(config_.depth_frame),
                to_tf_transform(config_.mount_in_pitch),
            });
            mount_clock_ = stamp.clock;
        }
        buffer_.apply_static_snapshot(*mount_authority_, mount_snapshot_);
    }

    CameraMotionConfig config_;
    rbt::tf::TransformBuffer buffer_;
    std::string mount_slot_;
    std::string mount_physical_id_;
    std::optional<rbt::tf::Authority> mount_authority_;
    std::optional<rbt::time::ClockIdentity> mount_clock_;
    rbt::tf::StaticSet mount_snapshot_;
    std::optional<rbt::tf::Authority> pitch_authority_;
    std::deque<rbt::tf::DynamicRecord> pitch_samples_;
    // Lock order is pitch_mutex_ then TransformBuffer internals; no provider IO runs here.
    std::mutex pitch_mutex_;
};

CameraMotion::CameraMotion(CameraMotionConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
}

CameraMotion::~CameraMotion() = default;

rbt::tf::TransformBuffer& CameraMotion::buffer() noexcept {
    return impl_->buffer();
}

void CameraMotion::bind_pitch(rbt::CapabilityInstanceId id) {
    impl_->bind_pitch(std::move(id));
}

void CameraMotion::ingest_pitch(const rbt::tf::DynamicRecord& record) {
    impl_->ingest_pitch(record);
}

CameraMotionSelection CameraMotion::select(const RectifiedPair& pair) {
    return impl_->select(pair);
}

std::vector<ArmSeed> CameraMotion::camera_seeds(
    const std::vector<ArmSeed>& seeds,
    const CameraMotionSelection& selection) const {
    return impl_->camera_seeds(seeds, selection);
}

} // namespace rbt_m3t
