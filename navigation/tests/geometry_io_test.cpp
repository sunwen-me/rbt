#include <rbt_navigation/geometry/geometry_io.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>

using namespace rbt::navigation;

namespace {

class TempDirectory {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("rbt-geometry-io-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    std::filesystem::path write(const std::string& name, const std::string& text) const {
        const auto path = path_ / name;
        std::ofstream output(path);
        if (!output) throw std::runtime_error("cannot create geometry test file");
        output << text;
        if (!output) throw std::runtime_error("cannot write geometry test file");
        return path;
    }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

bool near(double actual, double expected, double tolerance = 1e-10) {
    return std::abs(actual - expected) <= tolerance;
}

template <typename Function>
void mustThrow(Function&& function) {
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

} // namespace

int main() {
    const auto config = std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "config/front-drive-geometry.yaml";
    const RobotGeometry geometry = loadRobotGeometry(config.string());
    assert(geometry.footprint.size() == 8U);
    assert(near(geometry.drive_to_base.x, -0.196));
    assert(near(geometry.drive_to_base.y, 0.0));
    assert(near(geometry.drive_to_base.yaw, 0.0));

    double min_x = geometry.footprint.front().x;
    double min_y = geometry.footprint.front().y;
    double max_x = min_x;
    double max_y = min_y;
    for (const FootprintPoint& point : geometry.footprint) {
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
    }
    assert(near(min_x, -0.339));
    assert(near(min_y, -0.23082589));
    assert(near(max_x, 0.050350579));
    assert(near(max_y, 0.23082589));

    const Pose2D drive_pose{2.0, 3.0, std::numbers::pi / 2.0};
    const Pose2D base_pose = driveToBase(drive_pose, geometry);
    assert(near(base_pose.x, 2.0));
    assert(near(base_pose.y, 2.804));
    assert(near(base_pose.yaw, drive_pose.yaw));
    const Pose2D round_trip = baseToDrive(base_pose, geometry);
    assert(near(round_trip.x, drive_pose.x));
    assert(near(round_trip.y, drive_pose.y));
    assert(near(round_trip.yaw, drive_pose.yaw));

    TempDirectory directory;
    const std::string valid =
        "frame: base_footprint\n"
        "drive_to_base: [-0.196, 0, 0]\n"
        "footprint: [[-0.3, -0.2], [0.2, -0.2], [0.2, 0.2], [-0.3, 0.2]]\n";
    const auto wrong_shape = directory.write(
        "wrong-shape.yaml",
        "frame: base_footprint\ndrive_to_base: [0, 0]\n"
        "footprint: [[0, 0], [1, 0], [0, 1]]\n");
    mustThrow([&] { (void)loadRobotGeometry(wrong_shape.string()); });

    const auto non_finite = directory.write(
        "non-finite.yaml",
        "frame: base_footprint\ndrive_to_base: [0, 0, 0]\n"
        "footprint: [[0, 0], [1, 0], [.nan, 1]]\n");
    mustThrow([&] { (void)loadRobotGeometry(non_finite.string()); });

    const auto duplicate = directory.write(
        "duplicate.yaml",
        "frame: base_footprint\nframe: base_footprint\n"
        "drive_to_base: [0, 0, 0]\n"
        "footprint: [[0, 0], [1, 0], [0, 1]]\n");
    mustThrow([&] { (void)loadRobotGeometry(duplicate.string()); });

    const auto self_intersecting = directory.write(
        "self-intersecting.yaml",
        "frame: base_footprint\ndrive_to_base: [0, 0, 0]\n"
        "footprint: [[0, 0], [1, 1], [0, 1], [1, 0]]\n");
    mustThrow([&] { (void)loadRobotGeometry(self_intersecting.string()); });

    const auto missing = directory.path() / "missing.yaml";
    mustThrow([&] { (void)loadRobotGeometry(missing.string()); });

    std::cout << "PASS geometry profile loading and validation\n";
    return 0;
}
