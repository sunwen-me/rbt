#include <rbt_m3t/model.hpp>

#include <m3t/body.h>
#include <m3t/normal_renderer.h>
#include <m3t/renderer_geometry.h>
#include <opencv2/calib3d.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::array<const char*, 2> kArmNames {"so101_left", "so101_right"};
constexpr std::size_t kMinimumCorrespondences = 6U;
constexpr double kMaximumReprojectionRmse = 5.0;

struct Options {
    fs::path model_path;
    fs::path snapshot_path;
    fs::path output_path;
    std::optional<fs::path> joint_angles_path;
    bool neutral {false};
};

struct Snapshot {
    cv::Mat color;
    cv::Mat depth;
    cv::Mat camera_matrix;
    m3t::Intrinsics intrinsics {};
    double depth_unit_m {0.0};
    std::string frame_id;
    std::string serial;
    fs::path path;
};

struct Correspondences {
    std::vector<Eigen::Vector3f> object_points;
    std::vector<cv::Point2f> image_points;
    std::vector<cv::Point> rendered_pixels;
};

struct PoseResult {
    Eigen::Affine3f base_in_camera {Eigen::Affine3f::Identity()};
    double rmse_px {0.0};
    std::size_t inlier_count {0U};
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

[[nodiscard]] bool finite(double value) {
    return std::isfinite(value);
}

[[nodiscard]] std::string usage() {
    return "usage: rbt-m3t-initialize --model MODEL_JSON --snapshot DIR "
           "--output NEW_INIT_JSON (--neutral | --joint-angles FILE_JSON)";
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            std::cout << usage() << "\n"
                      << "  --neutral              Confirm both physical arms are at 0 rad.\n"
                      << "  --joint-angles FILE    JSON object with exact joint values for both arms.\n"
                      << "Reference view: click model point, then matching camera point; repeat at least 6 times.\n"
                      << "Keys: a/d/w/s orbit, r reset view and picks, Enter solve, Esc cancel.\n";
            std::exit(0);
        }
        if (argument == "--model" || argument == "--snapshot" || argument == "--output"
            || argument == "--joint-angles") {
            if (index + 1 >= argc) {
                fail("missing value after " + argument + "\n" + usage());
            }
            const fs::path value = argv[++index];
            if (argument == "--model") {
                options.model_path = value;
            } else if (argument == "--snapshot") {
                options.snapshot_path = value;
            } else if (argument == "--output") {
                options.output_path = value;
            } else {
                options.joint_angles_path = value;
            }
            continue;
        }
        if (argument == "--neutral") {
            options.neutral = true;
            continue;
        }
        fail("unknown argument: " + argument + "\n" + usage());
    }

    if (options.model_path.empty() || options.snapshot_path.empty() || options.output_path.empty()) {
        fail("--model, --snapshot, and --output are required\n" + usage());
    }
    if (options.neutral == options.joint_angles_path.has_value()) {
        fail("choose exactly one of --neutral or --joint-angles FILE_JSON\n" + usage());
    }
    return options;
}

[[nodiscard]] cv::FileNode require_node(const cv::FileStorage& storage,
                                       const std::string& key,
                                       const std::string& path) {
    const auto node = storage[key];
    if (node.empty()) {
        fail(path + " is missing " + key);
    }
    return node;
}

[[nodiscard]] double require_number(const cv::FileNode& node,
                                    const std::string& field,
                                    const std::string& path) {
    if (!node.isInt() && !node.isReal()) {
        fail(path + " field " + field + " must be a number");
    }
    const double value = static_cast<double>(node);
    if (!finite(value)) {
        fail(path + " field " + field + " must be finite");
    }
    return value;
}

[[nodiscard]] std::string require_string(const cv::FileNode& node,
                                         const std::string& field,
                                         const std::string& path) {
    if (!node.isString()) {
        fail(path + " field " + field + " must be a string");
    }
    const std::string value = static_cast<std::string>(node);
    if (value.empty()) {
        fail(path + " field " + field + " must not be empty");
    }
    return value;
}

[[nodiscard]] Snapshot load_snapshot(const fs::path& directory) {
    Snapshot snapshot;
    snapshot.path = fs::absolute(directory).lexically_normal();
    const auto color_path = directory / "color.png";
    const auto depth_path = directory / "depth.png";
    const auto calibration_path = directory / "calibration.json";
    snapshot.color = cv::imread(color_path.string(), cv::IMREAD_COLOR);
    snapshot.depth = cv::imread(depth_path.string(), cv::IMREAD_UNCHANGED);
    if (snapshot.color.empty() || snapshot.color.type() != CV_8UC3) {
        fail("cannot read snapshot BGR color image: " + color_path.string());
    }
    if (snapshot.depth.empty() || snapshot.depth.type() != CV_16UC1) {
        fail("snapshot depth image must be U16: " + depth_path.string());
    }

    cv::FileStorage calibration(calibration_path.string(),
                                cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON);
    if (!calibration.isOpened()) {
        fail("cannot open snapshot calibration JSON: " + calibration_path.string());
    }
    const std::string where = calibration_path.string();
    const auto width_value = require_number(require_node(calibration, "width", where), "width", where);
    const auto height_value = require_number(require_node(calibration, "height", where), "height", where);
    const auto fx = require_number(require_node(calibration, "fx", where), "fx", where);
    const auto fy = require_number(require_node(calibration, "fy", where), "fy", where);
    const auto cx = require_number(require_node(calibration, "cx", where), "cx", where);
    const auto cy = require_number(require_node(calibration, "cy", where), "cy", where);
    snapshot.depth_unit_m = require_number(require_node(calibration, "depth_unit_m", where),
                                           "depth_unit_m", where);
    snapshot.frame_id = require_string(require_node(calibration, "frame_id", where), "frame_id", where);
    snapshot.serial = require_string(require_node(calibration, "serial", where), "serial", where);
    (void)require_number(require_node(calibration, "capture_ns", where), "capture_ns", where);

    if (width_value < 1.0 || height_value < 1.0
        || width_value > static_cast<double>(std::numeric_limits<int>::max())
        || height_value > static_cast<double>(std::numeric_limits<int>::max())
        || std::floor(width_value) != width_value || std::floor(height_value) != height_value) {
        fail("snapshot calibration width and height must be positive integers");
    }
    const int width = static_cast<int>(width_value);
    const int height = static_cast<int>(height_value);
    if (snapshot.color.cols != width || snapshot.color.rows != height
        || snapshot.depth.cols != width || snapshot.depth.rows != height) {
        fail("snapshot color, depth, and calibration dimensions must match");
    }
    if (fx <= 0.0 || fy <= 0.0 || snapshot.depth_unit_m <= 0.0
        || std::max({std::abs(fx), std::abs(fy), std::abs(cx), std::abs(cy)})
               > static_cast<double>(std::numeric_limits<float>::max())) {
        fail("snapshot focal lengths and depth_unit_m must be positive and intrinsics representable");
    }
    snapshot.intrinsics = m3t::Intrinsics {static_cast<float>(fx), static_cast<float>(fy),
                                           static_cast<float>(cx), static_cast<float>(cy),
                                           width, height};
    snapshot.camera_matrix = (cv::Mat_<double>(3, 3)
        << fx, 0.0, cx,
           0.0, fy, cy,
           0.0, 0.0, 1.0);
    return snapshot;
}

[[nodiscard]] std::set<std::string> revolute_joint_names(const rbt_m3t::RobotModel& model) {
    std::set<std::string> result;
    for (const auto& joint : model.joints) {
        if (joint.type == "revolute") {
            if (!result.insert(joint.name).second) {
                fail("model has duplicate revolute joint name: " + joint.name);
            }
        } else if (joint.type != "fixed") {
            fail("model contains unsupported joint type " + joint.type + " at " + joint.name);
        }
    }
    if (result.empty()) {
        fail("model has no revolute joints");
    }
    return result;
}

using ArmJointAngles = std::array<std::map<std::string, double>, 2>;

[[nodiscard]] ArmJointAngles read_joint_angles(
    const fs::path& path, const rbt_m3t::RobotModel& model) {
    cv::FileStorage input(path.string(),
                          cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON);
    if (!input.isOpened()) {
        fail("cannot open joint angles JSON: " + path.string());
    }
    const auto root = input.root();
    if (!root.isMap()) {
        fail(path.string() + " must contain an object with both arm names");
    }
    std::set<std::string> arm_names;
    for (auto it = root.begin(); it != root.end(); ++it) {
        arm_names.insert((*it).name());
    }
    if (arm_names != std::set<std::string> {kArmNames[0], kArmNames[1]}) {
        fail(path.string() + " must contain exactly so101_left and so101_right");
    }
    const auto required_names = revolute_joint_names(model);
    ArmJointAngles angles;
    for (std::size_t arm_index = 0; arm_index < kArmNames.size(); ++arm_index) {
        const auto* arm_name = kArmNames[arm_index];
        const auto arm = require_node(input, arm_name, path.string());
        if (!arm.isMap()) {
            fail(path.string() + " field " + arm_name + " must be an object");
        }
        std::set<std::string> seen;
        for (auto it = arm.begin(); it != arm.end(); ++it) {
            const auto joint = *it;
            const std::string joint_name = joint.name();
            if (!seen.insert(joint_name).second) {
                fail(path.string() + " contains duplicate joint key " + joint_name);
            }
            if (required_names.find(joint_name) == required_names.end()) {
                fail(path.string() + " has unknown or non-revolute joint " + joint_name
                     + " for " + arm_name);
            }
            const double value = require_number(joint, joint_name, path.string());
            const auto model_joint = std::find_if(model.joints.begin(), model.joints.end(),
                [&](const auto& candidate) { return candidate.name == joint_name; });
            if (model_joint == model.joints.end() || value < model_joint->lower
                || value > model_joint->upper) {
                fail("joint " + joint_name + " for " + arm_name + " is outside model limits");
            }
            angles[arm_index].emplace(joint_name, value);
        }
        if (seen != required_names) {
            fail(path.string() + " field " + arm_name + " must contain exactly all revolute joints");
        }
    }
    return angles;
}

[[nodiscard]] ArmJointAngles neutral_joint_angles(
    const rbt_m3t::RobotModel& model) {
    std::cout << "Confirm both physical arms are currently at 0 radians for every revolute joint.\n"
              << "Type exactly `both_arms_at_zero` to continue: " << std::flush;
    std::string confirmation;
    if (!std::getline(std::cin, confirmation) || confirmation != "both_arms_at_zero") {
        fail("neutral pose was not explicitly confirmed");
    }
    ArmJointAngles angles;
    for (const auto& joint : model.joints) {
        if (joint.type == "revolute") {
            if (joint.lower > 0.0 || joint.upper < 0.0) {
                fail("joint " + joint.name + " does not allow the confirmed neutral value 0");
            }
            for (auto& arm_angles : angles) {
                arm_angles.emplace(joint.name, 0.0);
            }
        } else if (joint.type != "fixed") {
            fail("model contains unsupported joint type " + joint.type + " at " + joint.name);
        }
    }
    if (angles.front().empty()) {
        fail("model has no revolute joints");
    }
    return angles;
}

[[nodiscard]] Eigen::Affine3f virtual_camera_pose(const Eigen::Vector3f& center,
                                                 float distance,
                                                 float yaw,
                                                 float pitch) {
    const Eigen::Vector3f eye_direction {
        std::sin(yaw) * std::cos(pitch),
        -std::cos(yaw) * std::cos(pitch),
        std::sin(pitch)};
    const Eigen::Vector3f forward = -eye_direction.normalized();
    Eigen::Vector3f down = -Eigen::Vector3f::UnitZ();
    down -= forward * down.dot(forward);
    down.normalize();
    const Eigen::Vector3f right = down.cross(forward).normalized();
    down = forward.cross(right).normalized();

    Eigen::Affine3f camera_to_world = Eigen::Affine3f::Identity();
    camera_to_world.linear().col(0) = right;
    camera_to_world.linear().col(1) = down;
    camera_to_world.linear().col(2) = forward;
    camera_to_world.translation() = center + distance * eye_direction;
    return camera_to_world;
}

class ReferenceScene {
  public:
    ReferenceScene(const rbt_m3t::RobotModel& model,
                   const fs::path& model_path,
                   const std::map<std::string, double>& joint_angles,
                   const Snapshot& snapshot)
        : geometry_{std::make_shared<m3t::RendererGeometry>("m3t_initializer_geometry")},
          intrinsics_{snapshot.intrinsics} {
        rbt_m3t::ArmSeed seed;
        seed.name = kArmNames.front();
        seed.joint_angles = joint_angles;
        const auto links = rbt_m3t::forward_kinematics(model, seed);
        std::map<std::string, Eigen::Affine3f> link_poses;
        for (const auto& link : links) {
            link_poses[link.link] = link.in_camera;
        }

        for (const auto& link : model.links) {
            if (link.mesh.empty()) {
                continue;
            }
            const auto pose = link_poses.find(link.name);
            if (pose == link_poses.end()) {
                fail("forward kinematics did not produce link " + link.name);
            }
            fs::path mesh_path = link.mesh;
            if (mesh_path.is_relative()) {
                mesh_path = model_path.parent_path() / mesh_path;
            }
            auto body = std::make_shared<m3t::Body>(link.name, mesh_path, 1.0f, true,
                                                     false, m3t::Transform3fA::Identity());
            body->set_body2world_pose(pose->second);
            if (!body->SetUp()) {
                fail("cannot load model mesh for link " + link.name + ": " + mesh_path.string());
            }
            if (!geometry_->AddBody(body)) {
                fail("cannot add model mesh for link " + link.name);
            }
            bodies_.push_back(std::move(body));
        }
        if (bodies_.empty()) {
            fail("model has no renderable mesh links");
        }
        if (!geometry_->SetUp()) {
            fail("cannot initialize the M3T reference renderer; check the OpenGL display");
        }

        calculate_bounds();
        const float focal = std::min(intrinsics_.fu, intrinsics_.fv);
        const float image_extent = static_cast<float>(std::min(intrinsics_.width,
                                                              intrinsics_.height));
        distance_ = std::max(0.1f, 2.6f * radius_ * focal / image_extent);
        const auto pose = virtual_camera_pose(center_, distance_, yaw_, pitch_);
        renderer_ = std::make_unique<m3t::FullNormalRenderer>(
            "m3t_initializer_renderer", geometry_, pose.inverse(), intrinsics_,
            std::max(0.005f, distance_ - 3.0f * radius_), distance_ + 4.0f * radius_);
        if (!renderer_->SetUp()) {
            fail("cannot set up the M3T reference renderer");
        }
        render();
    }

    void render() {
        const auto camera_to_world = virtual_camera_pose(center_, distance_, yaw_, pitch_);
        renderer_->set_world2camera_pose(camera_to_world.inverse());
        if (!renderer_->StartRendering() || !renderer_->FetchDepthImage()
            || !renderer_->FetchNormalImage()) {
            fail("M3T failed to render the reference model");
        }
    }

    void orbit(float yaw_delta, float pitch_delta) {
        yaw_ += yaw_delta;
        pitch_ = std::clamp(pitch_ + pitch_delta, -1.15f, 1.15f);
        render();
    }

    void reset_view() {
        yaw_ = 0.0f;
        pitch_ = 0.0f;
        render();
    }

    [[nodiscard]] Eigen::Vector3f point_in_model(const cv::Point& pixel) const {
        if (pixel.x < 0 || pixel.y < 0 || pixel.x >= intrinsics_.width
            || pixel.y >= intrinsics_.height) {
            fail("reference click is outside the rendered image");
        }
        const auto depth_value = renderer_->DepthImageValue(pixel);
        if (depth_value == 0U || depth_value == std::numeric_limits<unsigned short>::max()) {
            fail("reference click is on the background; choose a visible model surface");
        }
        const float depth = renderer_->Depth(depth_value);
        const Eigen::Vector3f point_camera = renderer_->PointVector(pixel);
        if (!std::isfinite(depth) || depth <= 0.0f || !point_camera.allFinite()) {
            fail("reference click has invalid rendered depth");
        }
        return renderer_->camera2world_pose() * point_camera;
    }

    [[nodiscard]] const cv::Mat& normal_image() const {
        return renderer_->normal_image();
    }

  private:
    void calculate_bounds() {
        Eigen::Vector3f minimum = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
        Eigen::Vector3f maximum = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
        for (const auto& body : bodies_) {
            for (const auto& vertex : body->vertices()) {
                const Eigen::Vector3f point = body->body2world_pose() * vertex;
                minimum = minimum.cwiseMin(point);
                maximum = maximum.cwiseMax(point);
            }
        }
        center_ = 0.5f * (minimum + maximum);
        radius_ = 0.5f * (maximum - minimum).norm();
        if (!center_.allFinite() || !std::isfinite(radius_) || radius_ <= 0.0f) {
            fail("model mesh bounds are invalid");
        }
    }

    std::shared_ptr<m3t::RendererGeometry> geometry_;
    std::vector<std::shared_ptr<m3t::Body>> bodies_;
    std::unique_ptr<m3t::FullNormalRenderer> renderer_;
    m3t::Intrinsics intrinsics_;
    Eigen::Vector3f center_ {Eigen::Vector3f::Zero()};
    float radius_ {0.0f};
    float distance_ {0.0f};
    float yaw_ {0.0f};
    float pitch_ {0.0f};
};

struct PickState {
    ReferenceScene* scene {nullptr};
    const Snapshot* snapshot {nullptr};
    Correspondences points;
    std::optional<Eigen::Vector3f> pending_model_point;
    std::optional<cv::Point> pending_model_pixel;
    std::string status {"click a visible point on the reference model"};
};

void on_reference_click(int event, int x, int y, int, void* user_data) {
    if (event != cv::EVENT_LBUTTONDOWN) {
        return;
    }
    auto& state = *static_cast<PickState*>(user_data);
    if (state.pending_model_point.has_value()) {
        state.status = "click its matching point in the camera image first";
        return;
    }
    try {
        const cv::Point pixel{x, y};
        state.pending_model_point = state.scene->point_in_model(pixel);
        state.pending_model_pixel = pixel;
        state.status = "model point saved; click the same point in the camera image";
    } catch (const std::exception& error) {
        state.status = error.what();
    }
}

void on_camera_click(int event, int x, int y, int, void* user_data) {
    if (event != cv::EVENT_LBUTTONDOWN) {
        return;
    }
    auto& state = *static_cast<PickState*>(user_data);
    if (!state.pending_model_point.has_value()) {
        state.status = "select a visible point on the reference model first";
        return;
    }
    if (x < 0 || y < 0 || x >= state.snapshot->color.cols || y >= state.snapshot->color.rows) {
        state.status = "camera click is outside the snapshot";
        return;
    }
    state.points.object_points.push_back(*state.pending_model_point);
    state.points.image_points.emplace_back(static_cast<float>(x), static_cast<float>(y));
    state.points.rendered_pixels.push_back(*state.pending_model_pixel);
    state.pending_model_point.reset();
    state.pending_model_pixel.reset();
    state.status = "paired point saved; click another model point or press Enter to solve";
}

[[nodiscard]] cv::Mat model_display_image(const ReferenceScene& scene,
                                         const PickState& state) {
    cv::Mat display;
    const auto& image = scene.normal_image();
    if (image.channels() == 4) {
        cv::cvtColor(image, display, cv::COLOR_BGRA2BGR);
    } else if (image.channels() == 3) {
        display = image.clone();
    } else {
        cv::cvtColor(image, display, cv::COLOR_GRAY2BGR);
    }
    for (std::size_t index = 0; index < state.points.rendered_pixels.size(); ++index) {
        const auto pixel = state.points.rendered_pixels[index];
        cv::circle(display, pixel, 5, cv::Scalar{0, 255, 0}, 2, cv::LINE_AA);
        cv::putText(display, std::to_string(index + 1U), pixel + cv::Point{6, -6},
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar{0, 255, 0}, 1, cv::LINE_AA);
    }
    if (state.pending_model_pixel.has_value()) {
        cv::drawMarker(display, *state.pending_model_pixel, cv::Scalar{0, 0, 255},
                       cv::MARKER_CROSS, 13, 2, cv::LINE_AA);
    }
    cv::putText(display, state.status.substr(0, 88), cv::Point{12, display.rows - 12},
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar{255, 255, 255}, 2, cv::LINE_AA);
    cv::putText(display, state.status.substr(0, 88), cv::Point{12, display.rows - 12},
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar{20, 20, 20}, 1, cv::LINE_AA);
    return display;
}

[[nodiscard]] cv::Mat camera_display_image(const Snapshot& snapshot,
                                          const PickState& state) {
    cv::Mat display = snapshot.color.clone();
    for (std::size_t index = 0; index < state.points.image_points.size(); ++index) {
        const auto& point = state.points.image_points[index];
        const cv::Point pixel{cvRound(point.x), cvRound(point.y)};
        cv::circle(display, pixel, 5, cv::Scalar{0, 255, 0}, 2, cv::LINE_AA);
        cv::putText(display, std::to_string(index + 1U), pixel + cv::Point{6, -6},
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar{0, 255, 0}, 1, cv::LINE_AA);
        const auto raw_depth = snapshot.depth.at<std::uint16_t>(pixel);
        if (raw_depth != 0U && raw_depth != std::numeric_limits<std::uint16_t>::max()) {
            const double depth_m = static_cast<double>(raw_depth) * snapshot.depth_unit_m;
            cv::putText(display, std::to_string(depth_m).substr(0, 4) + "m",
                        pixel + cv::Point{6, 14}, cv::FONT_HERSHEY_SIMPLEX, 0.4,
                        cv::Scalar{255, 220, 0}, 1, cv::LINE_AA);
        }
    }
    cv::putText(display, "paired: " + std::to_string(state.points.image_points.size()),
                cv::Point{12, 24}, cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar{0, 255, 255}, 1, cv::LINE_AA);
    cv::putText(display, state.status.substr(0, 88), cv::Point{12, display.rows - 12},
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar{255, 255, 255}, 2, cv::LINE_AA);
    cv::putText(display, state.status.substr(0, 88), cv::Point{12, display.rows - 12},
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar{20, 20, 20}, 1, cv::LINE_AA);
    return display;
}

void clear_picks(PickState* state) {
    state->points = {};
    state->pending_model_point.reset();
    state->pending_model_pixel.reset();
    state->status = "picks cleared; click a visible point on the reference model";
}

[[nodiscard]] Eigen::Vector3d mean_point(const std::vector<Eigen::Vector3f>& points) {
    Eigen::Vector3d mean = Eigen::Vector3d::Zero();
    for (const auto& point : points) {
        mean += point.cast<double>();
    }
    return mean / static_cast<double>(points.size());
}

void reject_degenerate(const Correspondences& points) {
    if (points.object_points.size() < kMinimumCorrespondences
        || points.object_points.size() != points.image_points.size()
        || points.object_points.size() != points.rendered_pixels.size()) {
        fail("at least six complete point pairs are required");
    }
    std::set<std::pair<int, int>> unique_model_pixels;
    std::set<std::pair<int, int>> unique_camera_pixels;
    for (std::size_t index = 0; index < points.object_points.size(); ++index) {
        const auto& model_pixel = points.rendered_pixels[index];
        const auto& camera_point = points.image_points[index];
        const std::pair<int, int> model_key {model_pixel.x, model_pixel.y};
        const std::pair<int, int> camera_key {cvRound(camera_point.x), cvRound(camera_point.y)};
        if (!unique_model_pixels.insert(model_key).second
            || !unique_camera_pixels.insert(camera_key).second) {
            fail("each correspondence must use a distinct model and camera pixel");
        }
    }
    const auto center = mean_point(points.object_points);
    Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
    for (const auto& point : points.object_points) {
        const Eigen::Vector3d delta = point.cast<double>() - center;
        covariance.noalias() += delta * delta.transpose();
    }
    covariance /= static_cast<double>(points.object_points.size());
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eigen_solver{covariance};
    if (eigen_solver.info() != Eigen::Success) {
        fail("could not check the selected 3D point geometry");
    }
    const auto eigenvalues = eigen_solver.eigenvalues();
    if (!eigenvalues.allFinite() || eigenvalues[2] <= 1.0e-8
        || eigenvalues[0] <= std::max(1.0e-10, eigenvalues[2] * 1.0e-5)) {
        fail("selected model points are degenerate; spread points across the arm in 3D");
    }

    cv::Point2d image_mean{};
    for (const auto& point : points.image_points) {
        image_mean.x += point.x;
        image_mean.y += point.y;
    }
    image_mean.x /= static_cast<double>(points.image_points.size());
    image_mean.y /= static_cast<double>(points.image_points.size());
    cv::Mat image_covariance = cv::Mat::zeros(2, 2, CV_64F);
    for (const auto& point : points.image_points) {
        const double dx = static_cast<double>(point.x) - image_mean.x;
        const double dy = static_cast<double>(point.y) - image_mean.y;
        image_covariance.at<double>(0, 0) += dx * dx;
        image_covariance.at<double>(0, 1) += dx * dy;
        image_covariance.at<double>(1, 0) += dx * dy;
        image_covariance.at<double>(1, 1) += dy * dy;
    }
    const double image_determinant = cv::determinant(image_covariance);
    if (!finite(image_determinant) || image_determinant < 1.0e4) {
        fail("selected camera points are too close or nearly collinear; spread them across the arm");
    }
}

[[nodiscard]] PoseResult solve_pose(const Correspondences& points,
                                    const Snapshot& snapshot) {
    reject_degenerate(points);
    std::vector<cv::Point3f> object_points;
    object_points.reserve(points.object_points.size());
    for (const auto& point : points.object_points) {
        object_points.emplace_back(point.x(), point.y(), point.z());
    }
    cv::Mat rvec, tvec, inliers;
    const bool solved = cv::solvePnPRansac(object_points, points.image_points,
                                           snapshot.camera_matrix, cv::noArray(),
                                           rvec, tvec, false, 1000, 5.0f, 0.999,
                                           inliers, cv::SOLVEPNP_EPNP);
    if (!solved || inliers.type() != CV_32SC1
        || (inliers.rows != 1 && inliers.cols != 1)
        || inliers.total() < kMinimumCorrespondences
        || inliers.total() > object_points.size()) {
        fail("PnP RANSAC found fewer than six consistent point pairs");
    }
    std::vector<cv::Point3f> inlier_objects;
    std::vector<cv::Point2f> inlier_images;
    inlier_objects.reserve(inliers.total());
    inlier_images.reserve(inliers.total());
    for (std::size_t index = 0; index < inliers.total(); ++index) {
        const int source_index = inliers.rows == 1
            ? inliers.at<int>(0, static_cast<int>(index))
            : inliers.at<int>(static_cast<int>(index), 0);
        if (source_index < 0
            || static_cast<std::size_t>(source_index) >= object_points.size()) {
            fail("PnP RANSAC returned an invalid correspondence index");
        }
        inlier_objects.push_back(object_points.at(static_cast<std::size_t>(source_index)));
        inlier_images.push_back(points.image_points.at(static_cast<std::size_t>(source_index)));
    }
    if (!cv::solvePnP(inlier_objects, inlier_images, snapshot.camera_matrix, cv::noArray(),
                      rvec, tvec, true, cv::SOLVEPNP_ITERATIVE)) {
        fail("could not refine the RANSAC pose estimate");
    }

    cv::Mat rotation;
    cv::Rodrigues(rvec, rotation);
    rotation.convertTo(rotation, CV_64F);
    tvec.convertTo(tvec, CV_64F);
    Eigen::Matrix3d rotation_eigen;
    Eigen::Vector3d translation_eigen;
    for (int row = 0; row < 3; ++row) {
        translation_eigen[row] = tvec.at<double>(row, 0);
        for (int column = 0; column < 3; ++column) {
            rotation_eigen(row, column) = rotation.at<double>(row, column);
        }
    }
    const double determinant = rotation_eigen.determinant();
    if (!rotation_eigen.allFinite() || !translation_eigen.allFinite()
        || !finite(determinant) || determinant <= 0.0
        || (rotation_eigen.transpose() * rotation_eigen - Eigen::Matrix3d::Identity()).norm() > 1.0e-3) {
        fail("PnP produced an invalid rigid transform");
    }

    for (const auto& point : points.object_points) {
        const Eigen::Vector3d camera_point = rotation_eigen * point.cast<double>()
                                             + translation_eigen;
        if (!camera_point.allFinite() || camera_point.z() <= 0.0) {
            fail("PnP places a selected model point behind the camera");
        }
    }

    double squared_error = 0.0;
    for (std::size_t inlier_index = 0; inlier_index < inliers.total(); ++inlier_index) {
        const int source_index = inliers.rows == 1
            ? inliers.at<int>(0, static_cast<int>(inlier_index))
            : inliers.at<int>(static_cast<int>(inlier_index), 0);
        const Eigen::Vector3d camera_point = rotation_eigen
            * points.object_points.at(static_cast<std::size_t>(source_index)).cast<double>()
            + translation_eigen;
        const double x = snapshot.intrinsics.fu * camera_point.x() / camera_point.z()
                         + snapshot.intrinsics.ppu;
        const double y = snapshot.intrinsics.fv * camera_point.y() / camera_point.z()
                         + snapshot.intrinsics.ppv;
        const auto& measured = points.image_points.at(static_cast<std::size_t>(source_index));
        squared_error += std::pow(x - measured.x, 2.0) + std::pow(y - measured.y, 2.0);
    }
    const double rmse = std::sqrt(squared_error / static_cast<double>(inliers.total()));
    if (!finite(rmse) || rmse > kMaximumReprojectionRmse) {
        fail("reprojection RMSE is " + std::to_string(rmse)
             + " px; maximum allowed is 5 px. Clear picks and try again.");
    }

    Eigen::Affine3f transform = Eigen::Affine3f::Identity();
    transform.linear() = rotation_eigen.cast<float>();
    transform.translation() = translation_eigen.cast<float>();
    if (!transform.matrix().allFinite()) {
        fail("PnP transform contains non-finite values");
    }
    return PoseResult {transform, rmse, inliers.total()};
}

[[nodiscard]] std::optional<PoseResult> collect_arm_pose(
    const std::string& arm_name,
    ReferenceScene* scene,
    const Snapshot& snapshot) {
    const std::string reference_window = arm_name + " reference model";
    const std::string camera_window = arm_name + " camera snapshot";
    PickState state;
    state.scene = scene;
    state.snapshot = &snapshot;

    cv::namedWindow(reference_window, cv::WINDOW_AUTOSIZE);
    cv::namedWindow(camera_window, cv::WINDOW_AUTOSIZE);
    cv::setMouseCallback(reference_window, on_reference_click, &state);
    cv::setMouseCallback(camera_window, on_camera_click, &state);

    std::cout << "\n" << arm_name << ": "
              << "click a visible model surface point, then the matching snapshot point. "
              << "Use at least six well-spread points.\n"
              << "Keys: a/d/w/s orbit; r clears picks and resets view; Enter solves; Esc cancels.\n";
    for (;;) {
        cv::imshow(reference_window, model_display_image(*scene, state));
        cv::imshow(camera_window, camera_display_image(snapshot, state));
        const int key = cv::waitKey(20) & 0xff;
        if (key == 27) {
            cv::destroyWindow(reference_window);
            cv::destroyWindow(camera_window);
            return std::nullopt;
        }
        if (key == 'a') {
            scene->orbit(-0.12f, 0.0f);
        } else if (key == 'd') {
            scene->orbit(0.12f, 0.0f);
        } else if (key == 'w') {
            scene->orbit(0.0f, -0.12f);
        } else if (key == 's') {
            scene->orbit(0.0f, 0.12f);
        } else if (key == 'r') {
            clear_picks(&state);
            scene->reset_view();
        } else if (key == 10 || key == 13) {
            if (state.pending_model_point.has_value()) {
                state.status = "finish the pending pair by clicking the camera snapshot";
                std::cerr << state.status << "\n";
                continue;
            }
            try {
                auto result = solve_pose(state.points, snapshot);
                std::cout << arm_name << " pose accepted: " << result.inlier_count
                          << " inliers, " << std::fixed << std::setprecision(2)
                          << result.rmse_px << " px RMSE\n";
                cv::destroyWindow(reference_window);
                cv::destroyWindow(camera_window);
                return result;
            } catch (const std::exception& error) {
                state.status = error.what();
                std::cerr << state.status << "\n";
            }
        }
        if (cv::getWindowProperty(reference_window, cv::WND_PROP_VISIBLE) < 1.0
            || cv::getWindowProperty(camera_window, cv::WND_PROP_VISIBLE) < 1.0) {
            cv::destroyWindow(reference_window);
            cv::destroyWindow(camera_window);
            return std::nullopt;
        }
    }
}

[[nodiscard]] std::string json_escape(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(character) << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] std::string initialization_json(
    const Snapshot& snapshot,
    const ArmJointAngles& joint_angles,
    const std::array<PoseResult, 2>& poses,
    const fs::path& snapshot_path) {
    const std::size_t total_inliers = poses[0].inlier_count + poses[1].inlier_count;
    const double aggregate_rmse = std::sqrt(
        (poses[0].rmse_px * poses[0].rmse_px * static_cast<double>(poses[0].inlier_count)
         + poses[1].rmse_px * poses[1].rmse_px * static_cast<double>(poses[1].inlier_count))
        / static_cast<double>(total_inliers));
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n  \"schema_version\": 1,\n"
           << "  \"coordinate_frame\": " << json_escape(snapshot.frame_id) << ",\n"
           << "  \"serial\": " << json_escape(snapshot.serial) << ",\n"
           << "  \"arms\": [\n";
    for (std::size_t arm_index = 0; arm_index < kArmNames.size(); ++arm_index) {
        const auto& pose = poses[arm_index].base_in_camera.matrix();
        output << "    {\n      \"name\": " << json_escape(kArmNames[arm_index]) << ",\n"
               << "      \"base_in_camera\": [";
        bool first = true;
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (!first) output << ", ";
                output << static_cast<double>(pose(row, column));
                first = false;
            }
        }
        output << "],\n      \"joint_angles\": {";
        first = true;
        for (const auto& [name, value] : joint_angles[arm_index]) {
            if (!first) output << ", ";
            output << json_escape(name) << ": " << value;
            first = false;
        }
        output << "},\n      \"fixed_base\": true\n    }";
        output << (arm_index + 1U == kArmNames.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"provenance\": {\n"
           << "    \"snapshot_path\": " << json_escape(snapshot_path.string()) << ",\n"
           << "    \"method\": \"manual_3d_2d_pnp\",\n"
           << "    \"reprojection_rmse_px\": " << aggregate_rmse << "\n"
           << "  }\n}\n";
    return output.str();
}

void write_new_file(const fs::path& output_path, const std::string& contents) {
    if (fs::exists(output_path)) {
        fail("refusing to overwrite existing initialization file: " + output_path.string());
    }
    const auto parent = output_path.has_parent_path() ? output_path.parent_path() : fs::current_path();
    if (!fs::exists(parent) || !fs::is_directory(parent)) {
        fail("output parent directory does not exist: " + parent.string());
    }
    const fs::path staging { output_path.string() + ".tmp" };
    std::error_code error;
    if (!fs::create_directory(staging, error)) {
        fail("cannot reserve new initialization staging directory: " + staging.string());
    }
    const auto temporary = staging / "initialization.json";
    const auto cleanup = [&] {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        fs::remove(staging, ignored);
    };
    try {
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::out);
            if (!output) fail("cannot create initialization file: " + temporary.string());
            output << contents;
            output.flush();
            if (!output) fail("failed writing initialization file: " + temporary.string());
        }
        // A hard link publishes the complete file atomically and fails if any
        // destination already exists, including a concurrently created symlink.
        fs::create_hard_link(temporary, output_path, error);
        if (error) {
            fail("cannot publish new initialization file " + output_path.string() + ": " + error.message());
        }
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (!fs::is_regular_file(options.model_path)) {
            fail("model JSON does not exist: " + options.model_path.string());
        }
        if (fs::exists(options.output_path)) {
            fail("refusing to overwrite existing initialization file: " + options.output_path.string());
        }
        const auto model = rbt_m3t::load_robot_model(options.model_path);
        const ArmJointAngles joint_angles = options.neutral
            ? neutral_joint_angles(model)
            : read_joint_angles(*options.joint_angles_path, model);
        const auto snapshot = load_snapshot(options.snapshot_path);

        std::array<PoseResult, 2> poses;
        for (std::size_t arm_index = 0; arm_index < kArmNames.size(); ++arm_index) {
            ReferenceScene scene{model, options.model_path, joint_angles[arm_index], snapshot};
            auto pose = collect_arm_pose(kArmNames[arm_index], &scene, snapshot);
            if (!pose.has_value()) {
                fail("initialization cancelled; no output was written");
            }
            poses[arm_index] = *pose;
        }

        const auto json = initialization_json(snapshot, joint_angles, poses, snapshot.path);
        write_new_file(options.output_path, json);
        std::cout << "Wrote fixed-base initialization for both arms to "
                  << options.output_path << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rbt-m3t-initialize: " << error.what() << "\n";
        return 2;
    }
}
