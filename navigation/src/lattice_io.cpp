#include <rbt_navigation/planner/state_lattice.hpp>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace rbt::navigation {
namespace {

constexpr std::uintmax_t kMaximumLatticeFileBytes = 32U * 1024U * 1024U;
constexpr std::size_t kMaximumPrimitiveCount = 100'000U;
constexpr std::size_t kMaximumPoseCount = 2'000'000U;
constexpr double kEndpointTolerance = 1e-5;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::runtime_error("invalid state lattice control set: " + reason);
}

double readFiniteDouble(const YAML::Node& node, const char* field) {
    if (!node || !node.IsScalar()) invalid(std::string("missing or invalid ") + field);
    double value{};
    try {
        value = node.as<double>();
    } catch (const YAML::Exception&) {
        invalid(std::string("invalid ") + field);
    }
    if (!std::isfinite(value)) invalid(std::string(field) + " must be finite");
    return value;
}

std::uint32_t readUnsigned(const YAML::Node& node, const char* field) {
    if (!node || !node.IsScalar()) invalid(std::string("missing or invalid ") + field);
    const std::string text = node.Scalar();
    std::uint32_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
        invalid(std::string("invalid ") + field);
    }
    return value;
}

double shortestAngle(double from, double to) noexcept {
    return normalizeAngle(normalizeAngle(to) - normalizeAngle(from));
}

void validateControlSet(const LatticeControlSet& controls) {
    if (!std::isfinite(controls.resolution) || controls.resolution <= 0.0 ||
        !std::isfinite(controls.min_turning_radius) ||
        controls.min_turning_radius <= 0.0 || controls.headings.size() < 2U ||
        controls.headings.size() % 2U != 0U || controls.primitives.empty()) {
        invalid("metadata is incomplete");
    }

    for (std::size_t i = 0; i < controls.headings.size(); ++i) {
        const double heading = controls.headings[i];
        if (!std::isfinite(heading) || (i > 0U && heading <= controls.headings[i - 1U])) {
            invalid("heading angles must be finite and strictly increasing");
        }
        const std::size_t opposite = (i + controls.headings.size() / 2U) %
                                     controls.headings.size();
        if (std::abs(std::abs(shortestAngle(heading, controls.headings[opposite])) -
                     std::numbers::pi) > 1e-6) {
            invalid("opposite heading bins must differ by pi");
        }
    }

    std::vector<bool> has_primitives(controls.headings.size(), false);
    for (const LatticePrimitive& primitive : controls.primitives) {
        if (primitive.start_angle >= controls.headings.size() ||
            primitive.end_angle >= controls.headings.size() ||
            !std::isfinite(primitive.trajectory_length) ||
            primitive.trajectory_length < 0.0 || primitive.poses.empty()) {
            invalid("primitive metadata is invalid");
        }
        has_primitives[primitive.start_angle] = true;
        for (const Pose2D& pose : primitive.poses) {
            if (!finite(pose)) invalid("primitive pose must be finite");
        }
        const Pose2D& end = primitive.poses.back();
        const auto on_grid = [resolution = controls.resolution](double coordinate) {
            const double cells = coordinate / resolution;
            return std::isfinite(cells) &&
                   std::abs(cells - std::round(cells)) <= kEndpointTolerance;
        };
        if (!on_grid(end.x) || !on_grid(end.y) ||
            std::abs(shortestAngle(controls.headings[primitive.end_angle], end.yaw)) >
                kEndpointTolerance) {
            invalid("primitive endpoint must align with its grid cell and heading");
        }
    }
    if (std::any_of(has_primitives.begin(), has_primitives.end(),
                    [](bool present) { return !present; })) {
        invalid("every heading must have at least one primitive");
    }
}

} // namespace

LatticeControlSet loadLatticeControlSet(const std::string& path) {
    if (path.empty()) invalid("file path is empty");
    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error) invalid("cannot stat " + path);
    if (file_size == 0U || file_size > kMaximumLatticeFileBytes) {
        invalid("file size is outside the supported limit");
    }

    YAML::Node root;
    try {
        // yaml-cpp accepts JSON as a YAML subset; no JSON library is needed.
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& exception) {
        invalid("cannot parse " + path + ": " + exception.what());
    }
    if (!root || !root.IsMap()) invalid("root must be an object");
    const YAML::Node metadata = root["lattice_metadata"];
    if (!metadata || !metadata.IsMap()) invalid("lattice_metadata is missing");
    if (!metadata["motion_model"] || metadata["motion_model"].as<std::string>() != "diff") {
        invalid("only the differential-drive control set is supported");
    }

    LatticeControlSet controls;
    controls.resolution = readFiniteDouble(metadata["grid_resolution"], "grid_resolution");
    controls.min_turning_radius =
        readFiniteDouble(metadata["turning_radius"], "turning_radius");
    const std::uint32_t heading_count =
        readUnsigned(metadata["num_of_headings"], "num_of_headings");
    const std::uint32_t trajectory_count =
        readUnsigned(metadata["number_of_trajectories"], "number_of_trajectories");
    if (heading_count < 2U || heading_count > 256U || trajectory_count == 0U ||
        trajectory_count > kMaximumPrimitiveCount) {
        invalid("heading or primitive count is outside the supported limit");
    }

    const YAML::Node headings = metadata["heading_angles"];
    if (!headings || !headings.IsSequence() || headings.size() != heading_count) {
        invalid("heading_angles does not match num_of_headings");
    }
    controls.headings.reserve(heading_count);
    for (const YAML::Node& heading : headings) {
        controls.headings.push_back(readFiniteDouble(heading, "heading angle"));
    }

    const YAML::Node primitives = root["primitives"];
    if (!primitives || !primitives.IsSequence() ||
        primitives.size() != trajectory_count) {
        invalid("primitives does not match number_of_trajectories");
    }
    controls.primitives.reserve(primitives.size());
    std::size_t pose_count = 0U;
    for (const YAML::Node& input : primitives) {
        if (!input || !input.IsMap()) invalid("primitive must be an object");
        LatticePrimitive primitive;
        primitive.trajectory_id = readUnsigned(input["trajectory_id"], "trajectory_id");
        primitive.start_angle =
            readUnsigned(input["start_angle_index"], "start_angle_index");
        primitive.end_angle =
            readUnsigned(input["end_angle_index"], "end_angle_index");
        primitive.trajectory_length =
            readFiniteDouble(input["trajectory_length"], "trajectory_length");
        const YAML::Node poses = input["poses"];
        if (!poses || !poses.IsSequence() || poses.size() == 0U) {
            invalid("primitive poses must be a non-empty list");
        }
        if (poses.size() > kMaximumPoseCount - pose_count) {
            invalid("primitive pose count exceeds the supported limit");
        }
        pose_count += poses.size();
        primitive.poses.reserve(poses.size());
        for (const YAML::Node& coordinates : poses) {
            if (!coordinates || !coordinates.IsSequence() || coordinates.size() != 3U) {
                invalid("each pose must contain x, y, and yaw");
            }
            primitive.poses.push_back({
                readFiniteDouble(coordinates[0], "pose x"),
                readFiniteDouble(coordinates[1], "pose y"),
                readFiniteDouble(coordinates[2], "pose yaw")});
        }
        controls.primitives.push_back(std::move(primitive));
    }

    try {
        validateControlSet(controls);
    } catch (const std::runtime_error& exception) {
        invalid(exception.what());
    }
    return controls;
}

} // namespace rbt::navigation
