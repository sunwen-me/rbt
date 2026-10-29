#pragma once

#include <rbt_m3t/camera_motion.hpp>
#include <rbt_m3t/model.hpp>
#include <filesystem>
#include <string>

namespace rbt_m3t {

struct TrackingConfig {
    RobotModel model;
    std::vector<ArmSeed> arms;
    EngineOptions engine;
    std::string coordinate_frame;
    std::string serial;
    std::filesystem::path initialization_path;
    JointPriorConfig joint_prior;
    CameraMotionConfig camera_motion;
};

struct TrackingInitialization {
    std::vector<ArmSeed> arms;
    std::string coordinate_frame;
    std::string serial;
};

[[nodiscard]] TrackingInitialization load_tracking_initialization(
    const std::filesystem::path& path, const RobotModel& model);

// Config: schema_version=1, robot_model path, initialization path,
// cache_directory path, optional EngineOptions numeric keys.
// Initialization is separate, measured data produced by rbt-m3t-initialize.
[[nodiscard]] TrackingConfig load_tracking_config(const std::filesystem::path& path);

// UTF-8 application JSON, schema_version=1; native StampedBytes carries timing.
// Matrices are T_camera<-link, row-major, translations metres; joint values radians.
[[nodiscard]] std::string tracking_json(const TrackingEstimate& estimate,
                                       const RectifiedPair& pair,
                                       std::uint64_t frame_sequence,
                                       std::uint64_t dropped_frames,
                                       const CameraMotionSelection* motion = nullptr);

struct TrackingCapability {
    static constexpr char TYPE_NAME[] = "rbt.m3t.ArmTracking@1";
};

} // namespace rbt_m3t
