#include <rbt_m3t/model.hpp>

#include <m3t/body.h>
#include <m3t/basic_depth_renderer.h>
#include <m3t/constraint.h>
#include <m3t/depth_modality.h>
#include <m3t/depth_model.h>
#include <m3t/link.h>
#include <m3t/optimizer.h>
#include <m3t/refiner.h>
#include <m3t/region_modality.h>
#include <m3t/region_model.h>
#include <m3t/soft_constraint.h>
#include <m3t/static_detector.h>
#include <m3t/texture_modality.h>
#include <m3t/silhouette_renderer.h>
#include <m3t/tracker.h>

#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rbt_m3t {

namespace detail {

double unwrap_and_clamp_joint_angle(double wrapped, double previous,
                                    double lower, double upper) {
    constexpr double pi = 3.14159265358979323846;
    const auto turns = std::round((previous - wrapped) / (2.0 * pi));
    return std::clamp(wrapped + turns * (2.0 * pi), lower, upper);
}

} // namespace detail

namespace {

constexpr std::size_t kMaximumLinks = 32U;
constexpr std::size_t kMaximumNameBytes = 256U;
constexpr float kRigidTolerance = 1.0e-4F;
constexpr float kAxisTolerance = 1.0e-5F;

[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument(message);
}

[[noreturn]] void failed(const std::string& message) {
    throw std::runtime_error(message);
}

[[nodiscard]] bool finite_matrix(const Eigen::Matrix4f& matrix) noexcept {
    return matrix.array().isFinite().all();
}

[[nodiscard]] bool is_rigid_pose(const Eigen::Affine3f& pose) noexcept {
    const auto matrix = pose.matrix();
    if (!finite_matrix(matrix)) {
        return false;
    }
    if ((matrix.row(3) - Eigen::RowVector4f { 0.0F, 0.0F, 0.0F, 1.0F })
            .cwiseAbs()
            .maxCoeff()
        > kRigidTolerance) {
        return false;
    }
    const Eigen::Matrix3f rotation = matrix.topLeftCorner<3, 3>();
    if ((rotation.transpose() * rotation - Eigen::Matrix3f::Identity())
            .cwiseAbs()
            .maxCoeff()
        > kRigidTolerance) {
        return false;
    }
    return std::abs(rotation.determinant() - 1.0F) <= kRigidTolerance;
}

void validate_camera_pose(const Eigen::Affine3f& pose) {
    if (!is_rigid_pose(pose)) {
        invalid("M3T camera pose must be a finite proper rigid transform");
    }
}

void validate_name(const std::string& name, const char* description) {
    if (name.empty() || name.size() > kMaximumNameBytes) {
        invalid(std::string(description) + " must be non-empty and at most 256 bytes");
    }
}

[[nodiscard]] bool is_number(const cv::FileNode& node) noexcept {
    return node.isInt() || node.isReal();
}

[[nodiscard]] std::string read_string(const cv::FileNode& node,
                                      const char* description) {
    if (!node.isString()) {
        invalid(std::string(description) + " must be a string");
    }
    std::string value;
    node >> value;
    return value;
}

[[nodiscard]] double read_number(const cv::FileNode& node,
                                 const char* description) {
    if (!is_number(node)) {
        invalid(std::string(description) + " must be a number");
    }
    double value = 0.0;
    node >> value;
    if (!std::isfinite(value)) {
        invalid(std::string(description) + " must be finite");
    }
    return value;
}

[[nodiscard]] int read_integer(const cv::FileNode& node,
                               const char* description) {
    if (!node.isInt()) {
        invalid(std::string(description) + " must be an integer");
    }
    int value = 0;
    node >> value;
    return value;
}

[[nodiscard]] Eigen::Vector3f read_vector3(const cv::FileNode& node,
                                           const char* description) {
    if (!node.isSeq() || node.size() != 3U) {
        invalid(std::string(description) + " must contain exactly three numbers");
    }
    Eigen::Vector3f result;
    for (int index = 0; index < 3; ++index) {
        const auto value = read_number(node[index], description);
        if (std::abs(value) > std::numeric_limits<float>::max()) {
            invalid(std::string(description) + " exceeds the supported range");
        }
        result[index] = static_cast<float>(value);
    }
    return result;
}

[[nodiscard]] Eigen::Affine3f read_origin(const cv::FileNode& node) {
    if (!node.isSeq() || node.size() != 16U) {
        invalid("joint origin must contain exactly 16 row-major numbers");
    }
    Eigen::Affine3f result = Eigen::Affine3f::Identity();
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const auto value = read_number(node[row * 4 + column], "joint origin");
            if (std::abs(value) > std::numeric_limits<float>::max()) {
                invalid("joint origin exceeds the supported range");
            }
            result.matrix()(row, column) = static_cast<float>(value);
        }
    }
    if (!is_rigid_pose(result)) {
        invalid("joint origin must be a finite proper rigid transform");
    }
    return result;
}

[[nodiscard]] std::filesystem::path resolve_mesh(
    const std::filesystem::path& json_path, const std::string& mesh) {
    if (mesh.empty()) {
        return {};
    }
    const std::filesystem::path relative { mesh };
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        invalid("link mesh paths must be relative to robot.json");
    }
    for (const auto& component : relative) {
        if (component == "..") {
            invalid("link mesh paths may not escape the robot model directory");
        }
    }
    if (relative.extension() != ".obj") {
        invalid("link meshes must use relative OBJ paths");
    }
    const auto base = json_path.parent_path().empty() ? std::filesystem::current_path()
                                                      : json_path.parent_path();
    return std::filesystem::absolute(base / relative).lexically_normal();
}

struct ModelIndex {
    std::unordered_map<std::string, std::size_t> link_indices;
    std::vector<std::vector<std::size_t>> child_joint_indices;
    std::vector<std::size_t> revolute_joint_indices;
    std::size_t root_index {};
};

[[nodiscard]] ModelIndex validate_model(const RobotModel& model) {
    validate_name(model.name, "robot name");
    validate_name(model.root_link, "root link name");
    if (model.links.empty() || model.links.size() > kMaximumLinks) {
        invalid("robot model must contain between one and 32 links");
    }
    if (model.joints.size() + 1U != model.links.size()) {
        invalid("robot model must be one connected tree with one fewer joint than links");
    }

    ModelIndex index;
    index.child_joint_indices.resize(model.links.size());
    for (std::size_t i = 0; i < model.links.size(); ++i) {
        const auto& link = model.links[i];
        validate_name(link.name, "link name");
        if (!index.link_indices.emplace(link.name, i).second) {
            invalid("robot model contains duplicate link names");
        }
        if (!link.mesh.empty()) {
            if (link.mesh.is_absolute()) {
                // Programmatically constructed models may already hold an absolute path.
                if (link.mesh.extension() != ".obj") {
                    invalid("link meshes must use OBJ files");
                }
            } else {
                if (link.mesh.extension() != ".obj") {
                    invalid("link meshes must use OBJ files");
                }
                for (const auto& component : link.mesh) {
                    if (component == "..") {
                        invalid("link mesh paths may not contain parent directory components");
                    }
                }
            }
        }
    }
    const auto root = index.link_indices.find(model.root_link);
    if (root == index.link_indices.end()) {
        invalid("robot root link is not present in the link list");
    }
    index.root_index = root->second;

    std::set<std::string> joint_names;
    std::vector<int> parent_counts(model.links.size(), 0);
    for (std::size_t i = 0; i < model.joints.size(); ++i) {
        const auto& joint = model.joints[i];
        validate_name(joint.name, "joint name");
        if (!joint_names.insert(joint.name).second) {
            invalid("robot model contains duplicate joint names");
        }
        if (joint.type != "fixed" && joint.type != "revolute") {
            invalid("joint type must be fixed or revolute");
        }
        const auto parent = index.link_indices.find(joint.parent);
        const auto child = index.link_indices.find(joint.child);
        if (parent == index.link_indices.end() || child == index.link_indices.end()) {
            invalid("joint references a link that is not present in the model");
        }
        if (parent->second == child->second) {
            invalid("joint may not connect a link to itself");
        }
        if (!is_rigid_pose(joint.origin)) {
            invalid("joint origin must be a finite proper rigid transform");
        }
        if (!joint.axis.array().isFinite().all()) {
            invalid("joint axis must be finite");
        }
        if (!std::isfinite(joint.lower) || !std::isfinite(joint.upper)
            || std::abs(joint.lower) > std::numeric_limits<float>::max()
            || std::abs(joint.upper) > std::numeric_limits<float>::max()
            || joint.lower > joint.upper) {
            invalid("joint limits must be finite, float-representable, and ordered");
        }
        if (joint.type == "revolute") {
            if ((joint.axis - Eigen::Vector3f::UnitZ()).cwiseAbs().maxCoeff()
                > kAxisTolerance) {
                invalid("only +Z revolute joint axes are supported");
            }
            index.revolute_joint_indices.push_back(i);
        }
        ++parent_counts[child->second];
        if (parent_counts[child->second] > 1) {
            invalid("a link may have only one parent joint");
        }
        index.child_joint_indices[parent->second].push_back(i);
    }

    for (std::size_t i = 0; i < model.links.size(); ++i) {
        if (i == index.root_index) {
            if (parent_counts[i] != 0) {
                invalid("the declared root link may not have a parent joint");
            }
        } else if (parent_counts[i] != 1) {
            invalid("every non-root link must have exactly one parent joint");
        }
    }

    std::vector<bool> visited(model.links.size(), false);
    std::vector<std::size_t> stack { index.root_index };
    while (!stack.empty()) {
        const auto link_index = stack.back();
        stack.pop_back();
        if (visited[link_index]) {
            invalid("robot joint graph contains a cycle");
        }
        visited[link_index] = true;
        for (const auto joint_index : index.child_joint_indices[link_index]) {
            stack.push_back(index.link_indices.at(model.joints[joint_index].child));
        }
    }
    if (std::find(visited.begin(), visited.end(), false) != visited.end()) {
        invalid("robot joint graph is disconnected from its root");
    }

    return index;
}

[[nodiscard]] std::map<std::string, double> validate_seed(
    const RobotModel& model, const ModelIndex& index, const ArmSeed& seed) {
    validate_name(seed.name, "arm name");
    if (!is_rigid_pose(seed.base_in_camera)) {
        invalid("arm base pose must be a finite proper rigid transform");
    }
    if (seed.joint_angles.size() != index.revolute_joint_indices.size()) {
        invalid("arm seed must provide exactly one angle for every revolute joint");
    }
    for (const auto& [name, angle] : seed.joint_angles) {
        const auto found = std::find_if(model.joints.begin(), model.joints.end(),
                                        [&name](const ModelJoint& joint) {
                                            return joint.name == name;
                                        });
        if (found == model.joints.end() || found->type != "revolute") {
            invalid("arm seed contains an unknown or fixed joint angle");
        }
        if (!std::isfinite(angle) || angle < found->lower || angle > found->upper) {
            invalid("arm seed joint angles must be finite and within their limits");
        }
    }
    for (const auto joint_index : index.revolute_joint_indices) {
        const auto& joint = model.joints[joint_index];
        if (seed.joint_angles.find(joint.name) == seed.joint_angles.end()) {
            invalid("arm seed is missing a revolute joint angle");
        }
    }
    return seed.joint_angles;
}

[[nodiscard]] Eigen::Affine3f joint_transform(const ModelJoint& joint,
                                              double angle) {
    Eigen::Affine3f rotation = Eigen::Affine3f::Identity();
    if (joint.type == "revolute") {
        rotation.rotate(Eigen::AngleAxisf(static_cast<float>(angle),
                                          Eigen::Vector3f::UnitZ()));
    }
    return joint.origin * rotation;
}

[[nodiscard]] std::vector<Eigen::Affine3f> calculate_poses(
    const RobotModel& model, const ModelIndex& index, const ArmSeed& seed) {
    const auto angles = validate_seed(model, index, seed);
    std::vector<Eigen::Affine3f> poses(model.links.size(), Eigen::Affine3f::Identity());
    poses[index.root_index] = seed.base_in_camera;
    std::vector<std::size_t> stack { index.root_index };
    while (!stack.empty()) {
        const auto parent_index = stack.back();
        stack.pop_back();
        for (const auto joint_index : index.child_joint_indices[parent_index]) {
            const auto& joint = model.joints[joint_index];
            const auto child_index = index.link_indices.at(joint.child);
            const double angle = joint.type == "revolute" ? angles.at(joint.name) : 0.0;
            poses[child_index] = poses[parent_index] * joint_transform(joint, angle);
            if (!is_rigid_pose(poses[child_index])) {
                invalid("forward kinematics produced a non-finite rigid pose");
            }
            stack.push_back(child_index);
        }
    }
    return poses;
}

[[nodiscard]] Eigen::Affine3f to_eigen_affine(const m3t::Transform3fA& pose) {
    Eigen::Affine3f result = Eigen::Affine3f::Identity();
    result.matrix() = pose.matrix();
    return result;
}

[[nodiscard]] m3t::Transform3fA to_m3t_transform(const Eigen::Affine3f& pose) {
    m3t::Transform3fA result = m3t::Transform3fA::Identity();
    result.matrix() = pose.matrix();
    return result;
}

[[nodiscard]] bool valid_rgbd_pair(const RectifiedPair& pair) noexcept {
    const auto& intrinsics = pair.intrinsics;
    return !pair.bgr.empty() && pair.bgr.type() == CV_8UC3
           && !pair.depth_u16.empty() && pair.depth_u16.type() == CV_16UC1
           && pair.bgr.size() == pair.depth_u16.size()
           && intrinsics.width == pair.bgr.cols && intrinsics.height == pair.bgr.rows
           && std::isfinite(intrinsics.fu) && intrinsics.fu > 0.0F
           && std::isfinite(intrinsics.fv) && intrinsics.fv > 0.0F
           && std::isfinite(intrinsics.ppu) && std::isfinite(intrinsics.ppv)
           && std::isfinite(pair.depth_scale) && pair.depth_scale > 0.0;
}

[[nodiscard]] double project_joint_angle(const ModelJoint& joint,
                                         const m3t::Link& link,
                                         double previous) {
    const Eigen::Affine3f relative = joint.origin.inverse()
                                     * to_eigen_affine(link.joint2parent_pose());
    const double wrapped = std::atan2(static_cast<double>(relative.linear()(1, 0)),
                                      static_cast<double>(relative.linear()(0, 0)));
    return detail::unwrap_and_clamp_joint_angle(wrapped, previous,
                                                joint.lower, joint.upper);
}

struct ValidatedJointPrior {
    std::size_t arm_index {};
    std::size_t joint_index {};
    float target_angle {};
    float standard_deviation {};
};

[[nodiscard]] std::vector<ValidatedJointPrior> validate_joint_priors(
    const RobotModel& model, const ModelIndex& index,
    const std::vector<ArmSeed>& seeds,
    const std::vector<JointAnglePrior>& priors) {
    std::map<std::string, std::size_t> arm_indices;
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        (void)validate_seed(model, index, seeds[i]);
        if (!arm_indices.emplace(seeds[i].name, i).second) {
            invalid("arm names must be unique when applying joint priors");
        }
    }

    std::set<std::pair<std::string, std::string>> seen_joints;
    std::vector<ValidatedJointPrior> result;
    result.reserve(priors.size());
    for (const auto& prior : priors) {
        const auto arm = arm_indices.find(prior.arm);
        if (arm == arm_indices.end()) {
            invalid("joint prior references an unknown arm");
        }
        const auto joint = std::find_if(model.joints.begin(), model.joints.end(),
                                        [&prior](const ModelJoint& candidate) {
                                            return candidate.name == prior.joint;
                                        });
        if (joint == model.joints.end() || joint->type != "revolute") {
            invalid("joint prior references an unknown or non-revolute joint");
        }
        if (!seen_joints.emplace(prior.arm, prior.joint).second) {
            invalid("joint priors may contain at most one value per arm and joint");
        }
        if (!std::isfinite(prior.angle_rad) || prior.angle_rad < joint->lower
            || prior.angle_rad > joint->upper) {
            invalid("joint prior angles must be finite and within their joint limits");
        }
        const float target_angle = static_cast<float>(prior.angle_rad);
        if (!std::isfinite(target_angle)) {
            invalid("joint prior angle must be float-representable");
        }
        if (!std::isfinite(prior.standard_deviation_rad)
            || prior.standard_deviation_rad <= 0.0) {
            invalid("joint prior standard deviations must be finite and positive");
        }
        const float standard_deviation =
            static_cast<float>(prior.standard_deviation_rad);
        if (!std::isfinite(standard_deviation) || standard_deviation <= 0.0F) {
            invalid("joint prior standard deviation must be positive and float-representable");
        }
        const double represented_standard_deviation = standard_deviation;
        const double variance = represented_standard_deviation
                                * represented_standard_deviation;
        const double precision = 1.0 / variance;
        const double smallest_positive_float =
            std::numeric_limits<float>::denorm_min() > 0.0F
                ? std::numeric_limits<float>::denorm_min()
                : std::numeric_limits<float>::min();
        if (!std::isfinite(precision) || precision <= 0.0
            || precision < smallest_positive_float
            || precision > std::numeric_limits<float>::max()) {
            invalid("joint prior precision must be positive and float-representable");
        }
        const float float_precision = static_cast<float>(precision);
        if (!std::isfinite(float_precision) || float_precision <= 0.0F) {
            invalid("joint prior precision must be positive and float-representable");
        }
        const double maximum_weighted_residual =
            (joint->upper - joint->lower) * precision;
        if (!std::isfinite(maximum_weighted_residual)
            || maximum_weighted_residual > std::numeric_limits<float>::max()) {
            invalid("joint prior weighted residual exceeds the supported float range");
        }
        result.push_back(ValidatedJointPrior {
            arm->second,
            static_cast<std::size_t>(std::distance(model.joints.begin(), joint)),
            target_angle,
            standard_deviation,
        });
    }
    return result;
}

[[nodiscard]] std::string cache_component(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const unsigned char byte : value) {
        encoded.push_back(hex[byte >> 4U]);
        encoded.push_back(hex[byte & 0x0FU]);
    }
    return encoded;
}

[[nodiscard]] std::string object_name(const std::string& arm,
                                      const std::string& link,
                                      const char* component) {
    return "arm_" + cache_component(arm) + "_link_" + cache_component(link) + "_"
           + component;
}

void validate_options(const EngineOptions& options) {
    if (options.cache_directory.empty()) {
        invalid("M3T model cache directory must be set");
    }
    if (!options.use_region && !options.use_depth && !options.use_texture) {
        invalid("M3T requires at least one tracking modality");
    }
    if (options.model_divides < 0 || options.model_divides > 5
        || options.model_points < 1 || options.model_points > 10'000
        || options.model_image_size < 64 || options.model_image_size > 4096
        || options.occlusion_image_size < 64 || options.occlusion_image_size > 4096) {
        invalid("M3T model options are outside their supported bounds");
    }
    if (options.correspondence_iterations < 1 || options.correspondence_iterations > 20
        || options.update_iterations < 1 || options.update_iterations > 20) {
        invalid("M3T iteration counts must be between one and 20");
    }
}

[[nodiscard]] int checked_visualization_save_index(std::uint64_t index) {
    if (index > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        invalid("M3T visualization save index exceeds the supported integer range");
    }
    return static_cast<int>(index);
}

void validate_depth_metadata(const EngineOptions& options) {
    if (!options.use_depth || options.depth_metadata.empty()) {
        return;
    }

    cv::FileStorage storage;
    try {
        if (!storage.open(options.depth_metadata.string(), cv::FileStorage::READ)) {
            invalid("could not open depth modality metadata: "
                    + options.depth_metadata.string());
        }
    } catch (const cv::Exception& error) {
        invalid("could not parse depth modality metadata: "
                + std::string(error.what()));
    }

    const auto measure_occlusions = storage["measure_occlusions"];
    if (!measure_occlusions.empty()) {
        if (!measure_occlusions.isInt()) {
            invalid("depth metadata measure_occlusions must be integer 0 or 1");
        }
        int configured = -1;
        measure_occlusions >> configured;
        if (configured != 0 && configured != 1) {
            invalid("depth metadata measure_occlusions must be integer 0 or 1");
        }
        if ((configured != 0) != options.measured_occlusions) {
            invalid("depth metadata measure_occlusions conflicts with the "
                    "EngineOptions measured_occlusions flag");
        }
    }
    storage.release();
}

} // namespace

RobotModel load_robot_model(const std::filesystem::path& path) {
    if (path.empty()) {
        invalid("robot JSON path must be set");
    }
    cv::FileStorage storage;
    try {
        if (!storage.open(path.string(), cv::FileStorage::READ)) {
            invalid("could not open robot JSON: " + path.string());
        }
    } catch (const cv::Exception& error) {
        invalid("could not parse robot JSON: " + std::string(error.what()));
    }

    RobotModel model;
    try {
        const auto schema_version = read_integer(storage["schema_version"], "schema_version");
        if (schema_version != 1) {
            invalid("unsupported robot JSON schema_version");
        }
        model.name = read_string(storage["name"], "robot name");
        model.root_link = read_string(storage["root_link"], "root_link");
        if (read_string(storage["units"], "units") != "metres_radians") {
            invalid("robot JSON units must be metres_radians");
        }

        const auto links_node = storage["links"];
        if (!links_node.isSeq() || links_node.empty() || links_node.size() > kMaximumLinks) {
            invalid("robot JSON links must contain between one and 32 entries");
        }
        model.links.reserve(links_node.size());
        for (const auto& node : links_node) {
            ModelLink link;
            link.name = read_string(node["name"], "link name");
            const auto mesh = read_string(node["mesh"], "link mesh");
            const int visuals = read_integer(node["visuals"], "link visuals count");
            if (visuals < 0) {
                invalid("link visuals count must be non-negative");
            }
            link.mesh = resolve_mesh(path, mesh);
            model.links.push_back(std::move(link));
        }

        const auto joints_node = storage["joints"];
        if (!joints_node.isSeq() || joints_node.size() + 1U != model.links.size()) {
            invalid("robot JSON joints must have one fewer entry than links");
        }
        model.joints.reserve(joints_node.size());
        for (const auto& node : joints_node) {
            ModelJoint joint;
            joint.name = read_string(node["name"], "joint name");
            joint.type = read_string(node["type"], "joint type");
            joint.parent = read_string(node["parent"], "joint parent");
            joint.child = read_string(node["child"], "joint child");
            joint.origin = read_origin(node["origin"]);
            joint.axis = read_vector3(node["axis"], "joint axis");
            joint.lower = read_number(node["lower"], "joint lower limit");
            joint.upper = read_number(node["upper"], "joint upper limit");
            model.joints.push_back(std::move(joint));
        }
    } catch (const cv::Exception& error) {
        storage.release();
        invalid("could not read robot JSON: " + std::string(error.what()));
    }
    storage.release();
    (void)validate_model(model);
    return model;
}

std::vector<LinkEstimate> forward_kinematics(const RobotModel& model,
                                             const ArmSeed& seed) {
    const auto index = validate_model(model);
    const auto poses = calculate_poses(model, index, seed);
    std::vector<LinkEstimate> estimates;
    estimates.reserve(model.links.size());
    for (std::size_t i = 0; i < model.links.size(); ++i) {
        estimates.push_back(LinkEstimate { seed.name, model.links[i].name, poses[i] });
    }
    return estimates;
}

std::vector<ArmSeed> seeds_with_joint_priors(
    const RobotModel& model, const std::vector<ArmSeed>& seeds,
    const std::vector<JointAnglePrior>& priors) {
    const auto index = validate_model(model);
    const auto validated_priors = validate_joint_priors(model, index, seeds, priors);
    auto result = seeds;
    for (std::size_t i = 0; i < priors.size(); ++i) {
        result[validated_priors[i].arm_index].joint_angles[priors[i].joint] =
            priors[i].angle_rad;
    }
    for (const auto& seed : result) {
        (void)calculate_poses(model, index, seed);
    }
    return result;
}

class TrackerEngine::Impl {
  public:
    struct JointState {
        std::size_t joint_index {};
        std::shared_ptr<m3t::Link> link;
        double angle {};
    };

    struct ArmState {
        ArmSeed seed;
        std::shared_ptr<m3t::Optimizer> optimizer;
        std::shared_ptr<m3t::StaticDetector> static_detector;
        std::shared_ptr<m3t::Link> root;
        std::unordered_map<std::string, std::shared_ptr<m3t::Link>> links;
        std::vector<JointState> joints;
    };

    Impl(RobotModel robot, std::vector<ArmSeed> arm_seeds, EngineOptions engine_options,
         std::shared_ptr<CameraImages> camera_images)
        : model{std::move(robot)}, seeds{std::move(arm_seeds)}, options{std::move(engine_options)},
          images{std::move(camera_images)}, owner_thread{std::this_thread::get_id()} {
        index = validate_model(model);
        validate_options(options);
        validate_depth_metadata(options);
        if (seeds.size() != 2U) {
            invalid("M3T articulated tracking requires exactly two arm seeds");
        }
        std::set<std::string> arm_names;
        for (const auto& seed : seeds) {
            (void)validate_seed(model, index, seed);
            if (!arm_names.insert(seed.name).second) {
                invalid("arm names must be unique");
            }
        }
        if (options.constraints.size() > 64U) {
            invalid("M3T supports at most 64 extra constraints");
        }
        std::set<std::string> constraint_names;
        for (const auto& constraint : options.constraints) {
            validate_name(constraint.name, "constraint name");
            validate_name(constraint.arm, "constraint arm name");
            validate_name(constraint.link1, "constraint link1 name");
            validate_name(constraint.link2, "constraint link2 name");
            if (!constraint_names.insert(constraint.name).second) {
                invalid("constraint names must be unique");
            }
            if (arm_names.find(constraint.arm) == arm_names.end()) {
                invalid("constraint references an unknown arm");
            }
            if (constraint.link1 == constraint.link2
                || index.link_indices.find(constraint.link1) == index.link_indices.end()
                || index.link_indices.find(constraint.link2) == index.link_indices.end()) {
                invalid("constraint links must be different existing model links");
            }
            std::error_code error;
            if (constraint.metadata.empty()
                || !std::filesystem::is_regular_file(constraint.metadata, error)
                || error) {
                invalid("constraint metadata must be a regular file");
            }
        }
        if (!images) {
            invalid("M3T camera images must be provided");
        }
        tracker = std::make_shared<m3t::Tracker>(
            "rbt_m3t_dual_arm", options.correspondence_iterations,
            options.update_iterations, false, false, std::chrono::milliseconds { 33 }, 1, 1);
        color_camera = std::make_shared<RbtColorCamera>("rbt_m3t_color", images);
        depth_camera = std::make_shared<RbtDepthCamera>("rbt_m3t_depth", images);
    }

    void ensure_owner_thread() const {
        if (std::this_thread::get_id() != owner_thread) {
            throw std::logic_error("M3T engine methods must stay on their construction thread");
        }
    }

    void prepare_renderers() {
        const bool use_depth_renderer = options.modeled_occlusions;
        const bool use_region_renderer = options.region_checking && options.use_region;
        const bool use_body_renderer = options.use_texture
                                       || (options.region_checking && options.use_depth);
        if (!use_depth_renderer && !use_region_renderer && !use_body_renderer) {
            return;
        }

        renderer_geometry = std::make_shared<m3t::RendererGeometry>("rbt_m3t_geometry");
        if (use_depth_renderer) {
            depth_renderer = std::make_shared<m3t::FocusedBasicDepthRenderer>(
                "rbt_m3t_modeled_occlusion", renderer_geometry, depth_camera,
                options.occlusion_image_size);
        }
        if (use_region_renderer) {
            region_silhouette_renderer = std::make_shared<m3t::FocusedSilhouetteRenderer>(
                "rbt_m3t_region_silhouette", renderer_geometry, color_camera,
                m3t::IDType::REGION, options.occlusion_image_size);
        }
        if (use_body_renderer) {
            body_silhouette_renderer = std::make_shared<m3t::FocusedSilhouetteRenderer>(
                "rbt_m3t_body_silhouette", renderer_geometry, color_camera,
                m3t::IDType::BODY, options.occlusion_image_size);
        }
    }

    void add_visual_body(const std::shared_ptr<m3t::Body>& body) {
        if (next_visual_body_id > 255U) {
            failed("M3T supports at most 255 visual body identifiers");
        }
        const auto id = static_cast<uchar>(next_visual_body_id++);
        body->set_body_id(id);
        body->set_region_id(id);
        target_bodies.push_back(body);

        if (renderer_geometry) {
            if (!renderer_geometry->AddBody(body)) {
                failed("could not add an M3T body to shared renderer geometry");
            }
            if (depth_renderer && !depth_renderer->AddReferencedBody(body)) {
                failed("could not add a body to the modeled-occlusion renderer");
            }
            if (region_silhouette_renderer
                && !region_silhouette_renderer->AddReferencedBody(body)) {
                failed("could not add a body to the region silhouette renderer");
            }
            if (body_silhouette_renderer
                && !body_silhouette_renderer->AddReferencedBody(body)) {
                failed("could not add a body to the body silhouette renderer");
            }
        }
    }

    void build_arm(const ArmSeed& seed, std::size_t arm_number) {
        const auto camera_poses = calculate_poses(model, index, seed);
        std::vector<Eigen::Affine3f> initial_poses;
        initial_poses.reserve(camera_poses.size());
        for (const auto& pose : camera_poses) {
            const auto world_pose = images->camera_in_world * pose;
            if (!is_rigid_pose(world_pose)) {
                invalid("camera-frame seed produced a non-finite rigid world pose");
            }
            initial_poses.push_back(world_pose);
        }
        ArmState arm;
        arm.seed = seed;
        const auto arm_prefix = "arm" + std::to_string(arm_number) + "_" + cache_component(seed.name);
        const auto arm_cache = options.cache_directory / arm_prefix;
        std::filesystem::create_directories(arm_cache);

        std::function<std::shared_ptr<m3t::Link>(std::size_t, std::optional<std::size_t>)> make_link;
        make_link = [&](std::size_t link_index, std::optional<std::size_t> parent_joint_index) {
            const auto& link_model = model.links[link_index];
            const auto joint_pose = parent_joint_index.has_value()
                                        ? joint_transform(model.joints[*parent_joint_index],
                                            model.joints[*parent_joint_index].type == "revolute"
                                                ? seed.joint_angles.at(model.joints[*parent_joint_index].name)
                                                : 0.0)
                                        : Eigen::Affine3f::Identity();
            std::array<bool, 6> free_directions { false, false, false, false, false, false };
            if (parent_joint_index.has_value()
                && model.joints[*parent_joint_index].type == "revolute") {
                free_directions[2] = true;
            } else if (!parent_joint_index.has_value() && !seed.fixed_base) {
                free_directions = { true, true, true, true, true, true };
            }

            std::shared_ptr<m3t::Body> body;
            std::shared_ptr<m3t::RegionModality> region_modality;
            std::shared_ptr<m3t::DepthModality> depth_modality;
            std::shared_ptr<m3t::TextureModality> texture_modality;
            if (!link_model.mesh.empty()) {
                const auto name = object_name(seed.name, link_model.name, "body");
                body = std::make_shared<m3t::Body>(name, link_model.mesh, 1.0F, true, false,
                                                   m3t::Transform3fA::Identity());
                body->set_body2world_pose(to_m3t_transform(initial_poses[link_index]));
                const auto body_cache = arm_cache / cache_component(link_model.name);
                add_visual_body(body);
                std::shared_ptr<m3t::RegionModel> region_model;
                std::shared_ptr<m3t::DepthModel> depth_model;
                if (options.use_region || options.use_depth) {
                    std::filesystem::create_directories(body_cache);
                }
                if (options.use_region) {
                    region_model = std::make_shared<m3t::RegionModel>(
                        object_name(seed.name, link_model.name, "region_model"), body,
                        body_cache / "region.bin", 0.8F, options.model_divides,
                        options.model_points, 0.05F, 0.002F, false,
                        options.model_image_size);
                    const auto modality_name =
                        object_name(seed.name, link_model.name, "region_modality");
                    if (options.region_metadata.empty()) {
                        region_modality = std::make_shared<m3t::RegionModality>(
                            modality_name, body, color_camera, region_model);
                        region_modality->set_n_lines_max(options.model_points);
                    } else {
                        region_modality = std::make_shared<m3t::RegionModality>(
                            modality_name, options.region_metadata, body,
                            color_camera, region_model);
                    }
                    if (options.measured_occlusions) {
                        region_modality->MeasureOcclusions(depth_camera);
                    } else {
                        region_modality->DoNotMeasureOcclusions();
                    }
                    if (options.modeled_occlusions) {
                        region_modality->ModelOcclusions(depth_renderer);
                    } else {
                        region_modality->DoNotModelOcclusions();
                    }
                    if (options.region_checking) {
                        region_modality->UseRegionChecking(region_silhouette_renderer);
                    } else {
                        region_modality->DoNotUseRegionChecking();
                    }
                }
                if (options.use_depth) {
                    depth_model = std::make_shared<m3t::DepthModel>(
                        object_name(seed.name, link_model.name, "depth_model"), body,
                        body_cache / "depth.bin", 0.8F, options.model_divides,
                        options.model_points, 0.05F, 0.002F, false,
                        options.model_image_size);
                    const auto modality_name =
                        object_name(seed.name, link_model.name, "depth_modality");
                    if (options.depth_metadata.empty()) {
                        depth_modality = std::make_shared<m3t::DepthModality>(
                            modality_name, body, depth_camera, depth_model);
                        depth_modality->set_n_points_max(options.model_points);
                    } else {
                        depth_modality = std::make_shared<m3t::DepthModality>(
                            modality_name, options.depth_metadata, body,
                            depth_camera, depth_model);
                    }
                    if (options.measured_occlusions) {
                        depth_modality->MeasureOcclusions();
                    } else {
                        depth_modality->DoNotMeasureOcclusions();
                    }
                    if (options.modeled_occlusions) {
                        depth_modality->ModelOcclusions(depth_renderer);
                    } else {
                        depth_modality->DoNotModelOcclusions();
                    }
                    if (options.region_checking) {
                        depth_modality->UseSilhouetteChecking(body_silhouette_renderer);
                    } else {
                        depth_modality->DoNotUseSilhouetteChecking();
                    }
                }
                if (options.use_texture) {
                    const auto modality_name =
                        object_name(seed.name, link_model.name, "texture_modality");
                    if (options.texture_metadata.empty()) {
                        texture_modality = std::make_shared<m3t::TextureModality>(
                            modality_name, body, color_camera, body_silhouette_renderer);
                    } else {
                        texture_modality = std::make_shared<m3t::TextureModality>(
                            modality_name, options.texture_metadata, body,
                            color_camera, body_silhouette_renderer);
                    }
                    if (options.measured_occlusions) {
                        texture_modality->MeasureOcclusions(depth_camera);
                    } else {
                        texture_modality->DoNotMeasureOcclusions();
                    }
                    if (options.modeled_occlusions) {
                        texture_modality->ModelOcclusions(depth_renderer);
                    } else {
                        texture_modality->DoNotModelOcclusions();
                    }
                }
            }

            auto link = std::make_shared<m3t::Link>(
                object_name(seed.name, link_model.name, "link"), body,
                m3t::Transform3fA::Identity(), to_m3t_transform(joint_pose),
                to_m3t_transform(initial_poses[link_index]), free_directions, true);
            if ((region_modality && !link->AddModality(region_modality))
                || (depth_modality && !link->AddModality(depth_modality))
                || (texture_modality && !link->AddModality(texture_modality))) {
                failed("could not attach M3T modalities to a visual link");
            }
            arm.links.emplace(link_model.name, link);
            if (parent_joint_index.has_value()
                && model.joints[*parent_joint_index].type == "revolute") {
                arm.joints.push_back(JointState { *parent_joint_index, link,
                    seed.joint_angles.at(model.joints[*parent_joint_index].name) });
            }
            for (const auto joint_index : index.child_joint_indices[link_index]) {
                const auto child_index = index.link_indices.at(model.joints[joint_index].child);
                auto child_link = make_link(child_index, joint_index);
                if (!link->AddChildLink(child_link)) {
                    failed("could not attach an M3T child link");
                }
            }
            return link;
        };

        arm.root = make_link(index.root_index, std::nullopt);
        const auto optimizer_name =
            object_name(seed.name, model.root_link, "optimizer");
        if (options.optimizer_metadata.empty()) {
            arm.optimizer = std::make_shared<m3t::Optimizer>(optimizer_name, arm.root);
        } else {
            arm.optimizer = std::make_shared<m3t::Optimizer>(
                optimizer_name, options.optimizer_metadata, arm.root);
        }
        for (const auto& constraint_options : options.constraints) {
            if (constraint_options.arm != seed.name) {
                continue;
            }
            if (constraint_options.soft) {
                const auto constraint = std::make_shared<m3t::SoftConstraint>(
                    constraint_options.name, constraint_options.metadata,
                    arm.links.at(constraint_options.link1),
                    arm.links.at(constraint_options.link2));
                if (!arm.optimizer->AddSoftConstraint(constraint)) {
                    failed("could not attach M3T soft constraint to its arm optimizer");
                }
            } else {
                const auto constraint = std::make_shared<m3t::Constraint>(
                    constraint_options.name, constraint_options.metadata,
                    arm.links.at(constraint_options.link1),
                    arm.links.at(constraint_options.link2));
                if (!arm.optimizer->AddConstraint(constraint)) {
                    failed("could not attach M3T constraint to its arm optimizer");
                }
            }
        }
        if (!tracker->AddOptimizer(arm.optimizer)) {
            failed("could not add an M3T arm optimizer");
        }
        arm.static_detector = std::make_shared<m3t::StaticDetector>(
            object_name(seed.name, model.root_link, "static_detector"),
            arm.optimizer, to_m3t_transform(initial_poses[index.root_index]), false);
        if (!tracker->AddDetector(arm.static_detector)) {
            failed("could not add an M3T seed detector");
        }
        arms.push_back(std::move(arm));
    }

    void add_refiner() {
        if (!options.refine_initialization) {
            return;
        }
        if (options.refiner_metadata.empty()) {
            refiner = std::make_shared<m3t::Refiner>(
                "rbt_m3t_initial_refiner", options.correspondence_iterations,
                options.update_iterations, 1);
        } else {
            refiner = std::make_shared<m3t::Refiner>(
                "rbt_m3t_initial_refiner", options.refiner_metadata);
        }
        for (const auto& arm : arms) {
            if (!refiner->AddOptimizer(arm.optimizer)) {
                failed("could not add an arm optimizer to the M3T refiner");
            }
        }
        if (!tracker->AddRefiner(refiner)) {
            failed("could not add the M3T initialization refiner");
        }
    }

    void project_joints() {
        for (auto& arm : arms) {
            for (auto& joint : arm.joints) {
                const auto& model_joint = model.joints[joint.joint_index];
                joint.angle = project_joint_angle(model_joint, *joint.link, joint.angle);
                joint.link->set_joint2parent_pose(to_m3t_transform(
                    joint_transform(model_joint, joint.angle)));
            }
        }
    }

    void clear_joint_angle_priors() {
        for (auto& arm : arms) {
            arm.optimizer->ClearJointAnglePriors();
        }
    }

    TrackingEstimate estimates() const {
        TrackingEstimate result;
        result.modalities.clear();
        if (options.use_region) {
            result.modalities.emplace_back("region");
        }
        if (options.use_depth) {
            result.modalities.emplace_back("depth");
        }
        if (options.use_texture) {
            result.modalities.emplace_back("texture");
        }
        const Eigen::Affine3f world_to_camera = images->camera_in_world.inverse();
        for (const auto& arm : arms) {
            if (!is_rigid_pose(arm.root->link2world_pose())) {
                failed("M3T produced a non-finite or non-rigid arm base pose");
            }
            for (const auto& model_link : model.links) {
                const auto link = arm.links.at(model_link.name);
                const auto pose = world_to_camera * to_eigen_affine(link->link2world_pose());
                if (!is_rigid_pose(pose)) {
                    failed("M3T produced a non-finite or non-rigid link pose");
                }
                result.links.push_back(LinkEstimate { arm.seed.name, model_link.name, pose });
            }
            auto& angles = result.joint_angles[arm.seed.name];
            for (const auto& joint : arm.joints) {
                angles.emplace(model.joints[joint.joint_index].name, joint.angle);
            }
        }
        return result;
    }

    RobotModel model;
    std::vector<ArmSeed> seeds;
    EngineOptions options;
    std::shared_ptr<CameraImages> images;
    ModelIndex index;
    std::shared_ptr<m3t::Tracker> tracker;
    std::shared_ptr<RbtColorCamera> color_camera;
    std::shared_ptr<RbtDepthCamera> depth_camera;
    std::shared_ptr<m3t::RendererGeometry> renderer_geometry;
    std::shared_ptr<m3t::FocusedBasicDepthRenderer> depth_renderer;
    std::shared_ptr<m3t::FocusedSilhouetteRenderer> region_silhouette_renderer;
    std::shared_ptr<m3t::FocusedSilhouetteRenderer> body_silhouette_renderer;
    std::shared_ptr<m3t::Refiner> refiner;
    std::vector<std::shared_ptr<m3t::Body>> target_bodies;
    std::vector<ArmState> arms;
    std::thread::id owner_thread;
    std::size_t next_visual_body_id { 1U };
    bool set_up {};
    bool started {};
};

TrackerEngine::TrackerEngine(RobotModel model, std::vector<ArmSeed> seeds,
                             EngineOptions options,
                             std::shared_ptr<CameraImages> images)
    : impl_{std::make_unique<Impl>(std::move(model), std::move(seeds),
                                   std::move(options), std::move(images))} {}

TrackerEngine::~TrackerEngine() = default;

void TrackerEngine::set_up() {
    impl_->ensure_owner_thread();
    if (impl_->set_up) {
        return;
    }
    validate_camera_pose(impl_->images->camera_in_world);
    if (!impl_->images->current || !valid_rgbd_pair(*impl_->images->current)) {
        invalid("M3T setup requires a caller-selected RGB-D pair");
    }
    impl_->prepare_renderers();
    for (std::size_t i = 0; i < impl_->seeds.size(); ++i) {
        impl_->build_arm(impl_->seeds[i], i);
    }
    impl_->add_refiner();
    if (!impl_->tracker->SetUp()) {
        failed("could not set up the articulated M3T tracker");
    }
    impl_->set_up = true;
}

void TrackerEngine::reinitialize(const std::vector<ArmSeed>& seeds, int iteration) {
    impl_->ensure_owner_thread();
    if (!impl_->set_up) {
        throw std::logic_error("set up the M3T engine before reinitializing it");
    }
    if (iteration < 0) {
        invalid("M3T iteration index must be non-negative");
    }
    validate_camera_pose(impl_->images->camera_in_world);
    if (!impl_->images->current || !valid_rgbd_pair(*impl_->images->current)) {
        invalid("M3T reinitialization requires a valid caller-selected RGB-D pair");
    }
    if (seeds.size() != impl_->arms.size()) {
        invalid("M3T reinitialization must provide exactly the constructed arms");
    }

    std::map<std::string, ArmSeed> requested_seeds;
    for (const auto& seed : seeds) {
        const auto arm = std::find_if(impl_->arms.begin(), impl_->arms.end(),
                                      [&seed](const Impl::ArmState& candidate) {
                                          return candidate.seed.name == seed.name;
                                      });
        if (arm == impl_->arms.end()) {
            invalid("M3T reinitialization arm identities must match the constructed arms");
        }
        if (seed.fixed_base != arm->seed.fixed_base) {
            invalid("M3T reinitialization may not change an arm's fixed-base choice");
        }
        (void)validate_seed(impl_->model, impl_->index, seed);
        if (!requested_seeds.emplace(seed.name, seed).second) {
            invalid("M3T reinitialization arm names must be unique");
        }
    }
    impl_->clear_joint_angle_priors();
    if (!impl_->tracker->UpdateCameras(iteration)) {
        failed("M3T cameras could not read the shared RGB-D pair for reinitialization");
    }

    for (auto& arm : impl_->arms) {
        auto seed = requested_seeds.at(arm.seed.name);
        arm.seed = seed;
        const Eigen::Affine3f base_in_world =
            impl_->images->camera_in_world * seed.base_in_camera;
        arm.root->set_link2world_pose(to_m3t_transform(base_in_world));
        if (arm.root->body_ptr()) {
            arm.root->body_ptr()->set_body2world_pose(to_m3t_transform(base_in_world));
        }
        arm.static_detector->set_link2world_pose(to_m3t_transform(base_in_world));
        for (auto& joint : arm.joints) {
            const auto& model_joint = impl_->model.joints[joint.joint_index];
            joint.angle = seed.joint_angles.at(model_joint.name);
            joint.link->set_joint2parent_pose(to_m3t_transform(
                joint_transform(model_joint, joint.angle)));
        }
    }
    impl_->seeds = seeds;

    impl_->tracker->ExecuteDetection(true);
    if (!impl_->tracker->ExecuteDetectingStep(iteration)) {
        failed("M3T seeded detection or initialization refinement failed");
    }
    impl_->project_joints();
    if (!impl_->tracker->CalculateConsistentPoses()) {
        failed("M3T could not synchronize reinitialized articulated poses");
    }
    if (!impl_->tracker->ExecuteStartingStep(iteration)) {
        failed("M3T could not restart tracking after reinitialization");
    }
    impl_->started = true;
}

TrackingEstimate TrackerEngine::step(
    int iteration, const std::vector<JointAnglePrior>& priors) {
    impl_->ensure_owner_thread();
    if (!impl_->set_up) {
        throw std::logic_error("set up the M3T engine before stepping it");
    }
    if (iteration < 0) {
        invalid("M3T iteration index must be non-negative");
    }
    validate_camera_pose(impl_->images->camera_in_world);
    if (!impl_->images->current || !valid_rgbd_pair(*impl_->images->current)) {
        invalid("M3T step requires a valid caller-selected RGB-D pair");
    }
    std::vector<ArmSeed> active_seeds;
    active_seeds.reserve(impl_->arms.size());
    for (const auto& arm : impl_->arms) {
        active_seeds.push_back(arm.seed);
    }
    const auto validated_priors = validate_joint_priors(
        impl_->model, impl_->index, active_seeds, priors);
    impl_->clear_joint_angle_priors();
    if (!impl_->tracker->UpdateCameras(iteration)) {
        failed("M3T cameras could not read the shared RGB-D pair");
    }
    if (!impl_->started) {
        if (impl_->options.use_static_detector || impl_->options.refine_initialization) {
            impl_->tracker->ExecuteDetection(true);
            if (!impl_->tracker->ExecuteDetectingStep(iteration)) {
                failed("M3T seeded detection or initialization refinement failed");
            }
            impl_->project_joints();
            if (!impl_->tracker->CalculateConsistentPoses()) {
                failed("M3T could not synchronize initial articulated poses");
            }
        } else {
            if (!impl_->tracker->CalculateConsistentPoses()) {
                failed("M3T could not synchronize initial articulated poses");
            }
            impl_->tracker->StartTracking();
        }
        if (!impl_->tracker->ExecuteStartingStep(iteration)) {
            failed("M3T could not start tracking modalities");
        }
        impl_->started = true;
    }

    for (int corr_iteration = 0;
         corr_iteration < impl_->options.correspondence_iterations;
         ++corr_iteration) {
        if (!impl_->tracker->CalculateCorrespondences(iteration, corr_iteration)) {
            failed("M3T could not calculate correspondences");
        }
        if (impl_->options.visualize_correspondences) {
            const auto save_index = static_cast<std::uint64_t>(iteration)
                                    * static_cast<std::uint64_t>(
                                        impl_->options.correspondence_iterations)
                                    + static_cast<std::uint64_t>(corr_iteration);
            if (!impl_->tracker->VisualizeCorrespondences(
                    checked_visualization_save_index(save_index))) {
                failed("M3T correspondence visualization failed");
            }
        }
        for (int update_iteration = 0;
             update_iteration < impl_->options.update_iterations;
             ++update_iteration) {
            for (std::size_t arm_index = 0; arm_index < impl_->arms.size(); ++arm_index) {
                auto& arm = impl_->arms[arm_index];
                std::vector<m3t::JointAnglePrior> optimizer_priors;
                for (const auto& prior : validated_priors) {
                    if (prior.arm_index != arm_index) {
                        continue;
                    }
                    const auto joint = std::find_if(
                        arm.joints.begin(), arm.joints.end(),
                        [&prior](const Impl::JointState& candidate) {
                            return candidate.joint_index == prior.joint_index;
                        });
                    if (joint == arm.joints.end()) {
                        failed("validated joint prior has no M3T articulated link");
                    }
                    optimizer_priors.push_back(m3t::JointAnglePrior {
                        joint->link->first_jacobian_index(),
                        static_cast<float>(joint->angle),
                        prior.target_angle,
                        prior.standard_deviation,
                    });
                }
                if (!arm.optimizer->SetJointAnglePriors(std::move(optimizer_priors))) {
                    impl_->clear_joint_angle_priors();
                    failed("M3T could not apply a validated joint angle prior");
                }
            }
            if (!impl_->tracker->CalculateGradientAndHessian(
                    iteration, corr_iteration, update_iteration)
                || !impl_->tracker->CalculateOptimization(
                    iteration, corr_iteration, update_iteration)) {
                failed("M3T articulated optimization failed");
            }
            impl_->project_joints();
            if (!impl_->tracker->CalculateConsistentPoses()) {
                failed("M3T could not restore exact articulated poses");
            }
            if (impl_->options.visualize_optimization) {
                const auto save_index =
                    (static_cast<std::uint64_t>(iteration)
                         * static_cast<std::uint64_t>(
                             impl_->options.correspondence_iterations)
                     + static_cast<std::uint64_t>(corr_iteration))
                        * static_cast<std::uint64_t>(
                            impl_->options.update_iterations)
                    + static_cast<std::uint64_t>(update_iteration);
                if (!impl_->tracker->VisualizeOptimization(
                        checked_visualization_save_index(save_index))) {
                    failed("M3T optimization visualization failed");
                }
            }
        }
    }
    if (!impl_->tracker->CalculateResults(iteration)) {
        failed("M3T could not calculate tracking results");
    }
    if (impl_->options.visualize_results
        && !impl_->tracker->VisualizeResults(iteration)) {
        failed("M3T result visualization failed");
    }
    auto result = impl_->estimates();
    result.joint_prior.enabled = !priors.empty();
    result.joint_prior.status = priors.empty() ? "disabled" : "applied";
    result.joint_prior.priors = priors;
    return result;
}

} // namespace rbt_m3t
