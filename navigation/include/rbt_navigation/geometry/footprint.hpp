#pragma once

#include <rbt_navigation/core/types.hpp>
#include <rbt_navigation/map/grid_map.hpp>

#include <limits>
#include <vector>

namespace rbt::navigation {

struct FootprintPoint {
    double x{}, y{};
};

struct RobotGeometry {
    std::vector<FootprintPoint> footprint;
    Pose2D drive_to_base{};
};

[[nodiscard]] Pose2D composePose(const Pose2D& a, const Pose2D& b) noexcept;
[[nodiscard]] Pose2D inversePose(const Pose2D& pose) noexcept;
[[nodiscard]] Pose2D baseToDrive(const Pose2D& base_pose,
                                 const RobotGeometry& geometry) noexcept;
[[nodiscard]] Pose2D driveToBase(const Pose2D& drive_pose,
                                 const RobotGeometry& geometry) noexcept;

class FootprintCollisionChecker final {
public:
    explicit FootprintCollisionChecker(std::vector<FootprintPoint> polygon);

    [[nodiscard]] bool poseFree(const GridMap& map, const Pose2D& drive_pose,
                                double padding = 0.0) const;
    [[nodiscard]] bool motionFree(const GridMap& map, const Pose2D& a,
                                  const Pose2D& b, double padding = 0.0) const;
    [[nodiscard]] bool commandFree(
        const GridMap& map, const Pose2D& start, const Twist2D& command,
        double duration,
        double max_reference_distance = std::numeric_limits<double>::infinity(),
        double padding = 0.0) const;
    [[nodiscard]] double radius() const noexcept { return radius_; }
    [[nodiscard]] const std::vector<FootprintPoint>& footprint() const noexcept {
        return footprint_;
    }

private:
    std::vector<FootprintPoint> footprint_;
    double radius_{};
};

} // namespace rbt::navigation
