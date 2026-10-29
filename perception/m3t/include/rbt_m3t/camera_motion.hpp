#pragma once

#include <rbt_m3t/model.hpp>

#include <rbt/transforms.hpp>

#include <Eigen/Geometry>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rbt_m3t {

struct CameraMotionArmBase {
    std::string arm;
    Eigen::Affine3f base_in_world { Eigen::Affine3f::Identity() };
};

struct CameraMotionConfig {
    bool enabled { false };
    std::string world_frame;
    std::string pitch_frame;
    std::string pitch_provider;
    std::string depth_frame;
    std::string time_basis { "capture" };
    bool allow_unknown_uncertainty { false };
    int maximum_interpolation_gap_ms { 200 };
    int maximum_wait_ms { 200 };
    int maximum_sample_age_ms { 1000 };
    Eigen::Affine3f mount_in_pitch { Eigen::Affine3f::Identity() };
    std::vector<CameraMotionArmBase> arm_bases;
};

struct CameraMotionSelection {
    Eigen::Affine3f camera_in_world { Eigen::Affine3f::Identity() };
    rbt::tf::LookupResult lookup;
    rbt::time::Timestamp pitch_sample_timestamp;
    std::int64_t pitch_sample_offset_ns { 0 };
    std::uint64_t pitch_sample_offset_abs_ns { 0U };
    std::string time_basis;
    std::string world_frame;
    std::string selection_policy { "nearest" };
};

class CameraMotion {
  public:
    explicit CameraMotion(CameraMotionConfig config);
    ~CameraMotion();
    CameraMotion(const CameraMotion&) = delete;
    CameraMotion& operator=(const CameraMotion&) = delete;
    CameraMotion(CameraMotion&&) = delete;
    CameraMotion& operator=(CameraMotion&&) = delete;

    [[nodiscard]] rbt::tf::TransformBuffer& buffer() noexcept;
    void bind_pitch(rbt::CapabilityInstanceId id);
    void ingest_pitch(const rbt::tf::DynamicRecord& record);
    // Performs timestamped lookups immediately; the owner may drain providers and retry.
    [[nodiscard]] CameraMotionSelection select(const RectifiedPair& pair);
    [[nodiscard]] std::vector<ArmSeed> camera_seeds(
        const std::vector<ArmSeed>& seeds,
        const CameraMotionSelection& selection) const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rbt_m3t
