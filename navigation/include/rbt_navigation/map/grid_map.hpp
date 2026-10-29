#pragma once
#include <rbt_navigation/core/types.hpp>
#include <cstddef>
#include <optional>
#include <utility>

namespace rbt::navigation {
struct OccupancyGrid {
    std::size_t width{}, height{};
    double resolution{};
    Pose2D origin; // map <- grid; origin at lower-left corner, yaw supported
    std::vector<std::int8_t> cells; // row-major y*width+x; -1 unknown, 0..100
    std::int8_t occupied_threshold{50};
};
class IMapQuery {
public:
    virtual ~IMapQuery() = default;
    virtual bool isOccupied(double x, double y) const = 0;
    // Conservative clearance to occupied cell area and map boundary, metres.
    // Unknown / outside / invalid queries return zero clearance.
    virtual double getDistanceToObstacle(double x, double y) const = 0;
};
class GridMap final : public IMapQuery {
public:
    explicit GridMap(OccupancyGrid grid);
    [[nodiscard]] std::optional<std::pair<int,int>> worldToGrid(double x,double y) const;
    [[nodiscard]] Pose2D gridToWorld(int x,int y) const;
    [[nodiscard]] bool cellOccupied(int x,int y) const;
    [[nodiscard]] bool isOccupied(double x,double y) const override;
    [[nodiscard]] double getDistanceToObstacle(double x,double y) const override;
    [[nodiscard]] double signedDistance(int x,int y) const;
    [[nodiscard]] bool isFree(double x,double y,double radius) const;
    [[nodiscard]] bool segmentFree(const Pose2D& a,const Pose2D& b,double radius) const;
    [[nodiscard]] std::size_t width() const noexcept { return grid_.width; }
    [[nodiscard]] std::size_t height() const noexcept { return grid_.height; }
    [[nodiscard]] double resolution() const noexcept { return grid_.resolution; }
    [[nodiscard]] const Pose2D& origin() const noexcept { return grid_.origin; }
private:
    OccupancyGrid grid_;
    std::vector<std::uint8_t> occupied_;
    std::vector<double> distance_to_occupied_, distance_to_free_;
};
} // namespace rbt::navigation
