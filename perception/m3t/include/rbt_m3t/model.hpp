#pragma once

#include <rbt_m3t/frame_source.hpp>
#include <rbt_m3t/joint_prior_types.hpp>
#include <Eigen/Geometry>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rbt_m3t {

struct ModelLink {
    std::string name;
    std::filesystem::path mesh; // Empty for a purely kinematic / virtual frame.
};

struct ModelJoint {
    std::string name, type, parent, child;
    Eigen::Affine3f origin { Eigen::Affine3f::Identity() }; // T_parent<-child at q=0.
    Eigen::Vector3f axis { Eigen::Vector3f::UnitZ() };
    double lower { 0.0 }, upper { 0.0 };
};

struct RobotModel {
    std::string name, root_link;
    std::vector<ModelLink> links;
    std::vector<ModelJoint> joints;
};

struct ArmSeed {
    std::string name; // Distinct frame prefix, e.g. so101_left / so101_right.
    Eigen::Affine3f base_in_camera { Eigen::Affine3f::Identity() };
    std::map<std::string, double> joint_angles;
    bool fixed_base { false };
};

// Extra constraints belong to one arm's optimizer. Metadata uses upstream
// Constraint/SoftConstraint YAML, including joint frames and constrained axes.
struct ConstraintOptions {
    std::string name, arm, link1, link2;
    std::filesystem::path metadata;
    bool soft { false };
};

struct EngineOptions {
    std::filesystem::path cache_directory;
    int model_divides { 2 }, model_points { 200 }, model_image_size { 512 };
    int correspondence_iterations { 3 }, update_iterations { 2 };
    bool use_region { true }, use_depth { true }, use_texture { false };
    bool measured_occlusions { true }, modeled_occlusions { false }, region_checking { false };
    int occlusion_image_size { 512 };
    // Upstream metadata supplies the complete per-object tuning interface.
    std::filesystem::path region_metadata, depth_metadata, texture_metadata;
    std::filesystem::path optimizer_metadata, refiner_metadata;
    bool use_static_detector { false }, refine_initialization { false };
    bool visualize_correspondences { false }, visualize_optimization { false }, visualize_results { false };
    std::vector<ConstraintOptions> constraints;
};

struct LinkEstimate {
    std::string arm, link;
    Eigen::Affine3f in_camera { Eigen::Affine3f::Identity() };
};

struct TrackingEstimate {
    std::vector<LinkEstimate> links;
    std::map<std::string, std::map<std::string, double>> joint_angles;
    std::vector<std::string> modalities { "region", "depth" };
    JointPriorReport joint_prior;
};

[[nodiscard]] RobotModel load_robot_model(const std::filesystem::path& path);
[[nodiscard]] std::vector<LinkEstimate> forward_kinematics(const RobotModel& model,
                                                          const ArmSeed& seed);
[[nodiscard]] std::vector<ArmSeed> seeds_with_joint_priors(
    const RobotModel& model, const std::vector<ArmSeed>& seeds,
    const std::vector<JointAnglePrior>& priors);

// All OpenGL/M3T creation, calls and destruction belong to one tracking thread.
class TrackerEngine {
  public:
    TrackerEngine(RobotModel model, std::vector<ArmSeed> seeds, EngineOptions options,
                  std::shared_ptr<CameraImages> images);
    ~TrackerEngine();
    TrackerEngine(const TrackerEngine&) = delete;
    TrackerEngine& operator=(const TrackerEngine&) = delete;
    void set_up();
    // Explicit reset from caller-supplied seeds; no automatic pose detection.
    // Arm identities and fixed-base choices must match the constructed engine.
    void reinitialize(const std::vector<ArmSeed>& seeds, int iteration = 0);
    [[nodiscard]] TrackingEstimate step(
        int iteration, const std::vector<JointAnglePrior>& priors = {});
  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rbt_m3t
