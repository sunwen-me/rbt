#include <rbt_navigation/geometry/geometry_io.hpp>

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <set>
#include <stdexcept>
#include <string>

namespace rbt::navigation {
namespace {

[[noreturn]] void invalid(const std::string& message) {
    throw std::runtime_error("invalid robot geometry: " + message);
}

double yamlDouble(const YAML::Node& node, const std::string& name) {
    if (!node || !node.IsScalar()) invalid(name + " must be a number");

    double value{};
    try {
        value = node.as<double>();
    } catch (const YAML::Exception&) {
        invalid(name + " must be a number");
    }
    if (!std::isfinite(value)) invalid(name + " must be finite");
    return value;
}

} // namespace

RobotGeometry loadRobotGeometry(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& error) {
        invalid("cannot load " + path + ": " + error.what());
    }
    if (!root.IsMap()) invalid("root must be a mapping");

    std::set<std::string> keys;
    for (const auto& entry : root) {
        if (!entry.first.IsScalar()) invalid("field names must be strings");
        const std::string key = entry.first.Scalar();
        if (!keys.insert(key).second) invalid("duplicate field " + key);
        if (key != "frame" && key != "drive_to_base" && key != "footprint") {
            invalid("unknown field " + key);
        }
    }

    const YAML::Node frame = root["frame"];
    if (!frame || !frame.IsScalar() || frame.Scalar() != "base_footprint") {
        invalid("frame must be base_footprint");
    }

    const YAML::Node offset = root["drive_to_base"];
    if (!offset || !offset.IsSequence() || offset.size() != 3U) {
        invalid("drive_to_base must contain three numbers");
    }

    RobotGeometry geometry;
    geometry.drive_to_base = {
        yamlDouble(offset[0], "drive_to_base[0]"),
        yamlDouble(offset[1], "drive_to_base[1]"),
        yamlDouble(offset[2], "drive_to_base[2]")};

    const YAML::Node footprint = root["footprint"];
    if (!footprint || !footprint.IsSequence() || footprint.size() < 3U) {
        invalid("footprint must contain at least three points");
    }
    geometry.footprint.reserve(footprint.size());
    for (std::size_t i = 0; i < footprint.size(); ++i) {
        const YAML::Node point = footprint[i];
        if (!point.IsSequence() || point.size() != 2U) {
            invalid("footprint[" + std::to_string(i) + "] must contain two numbers");
        }
        geometry.footprint.push_back(
            {yamlDouble(point[0], "footprint coordinate"),
             yamlDouble(point[1], "footprint coordinate")});
    }

    try {
        (void)FootprintCollisionChecker(geometry.footprint);
    } catch (const std::invalid_argument& error) {
        invalid(std::string("invalid footprint polygon: ") + error.what());
    }
    return geometry;
}

} // namespace rbt::navigation
