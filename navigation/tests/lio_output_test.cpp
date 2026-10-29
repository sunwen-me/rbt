#include <rbt_navigation/runtime/lio_output.hpp>
#include <rbt_navigation/runtime/lio_bridge.hpp>
#include <rbt_navigation/runtime/native.hpp>
#include <rbt_navigation/map/obstacle_layer.hpp>
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    namespace native = rbt::navigation::native;
    namespace output = rbt::navigation::runtime;
    auto clock = native::hostClock();
    const auto now = native::now(clock);
    const rbt::time::ConvertedTimestamp epoch{{now.clock, now.ns - 100'000'000}, 140'512};
    output::LioNavigationFrameFields fields;
    fields.world_frame = "odom";
    fields.base_frame = "base_footprint";
    fields.odom_base_position = {1, 2, 0};
    fields.odom_base_quaternion_xyzw = {0, 0, 0, 1};
    fields.map_odom_position = {3, 4, 0};
    fields.map_odom_quaternion_xyzw = {0, 0, 0, 1};
    fields.sensor_origin = {1, 2, .7};
    fields.linear_velocity = {.2, 0, 0};
    fields.angular_velocity = {0, 0, .1};
    fields.map_odom_valid = true;
    const auto header = output::makeLioNavigationHeader(epoch, 1, fields);
    const rbt::navwire::LioPoint points[]{{2, 3, .5F, 1}};
    const auto projected = rbt::navigation::lio::validateAndProject(header, points, now);
    assert(native::decodeStamp(header.stamp, now.clock) == epoch.timestamp.ns);
    assert(projected.stamp.ns == epoch.timestamp.ns);
    assert(projected.uncertainty_ns == epoch.uncertainty_ns);
    assert(projected.points_map[0] == rbt::tf::Vec3(5, 7, .5));
    assert(projected.map_base_link.x == 4 && projected.map_base_link.y == 6);
    assert(projected.odometry.linear_x == .2 && projected.odometry.angular_z == .1);

    // The odometry datum is at the IMU, 0.735 m above the base. Preserve
    // that offset through the producer contract and map transform so low
    // obstacles are classified relative to the base rather than odom z=0.
    fields.odom_base_position[2] = -.735;
    fields.map_odom_position[2] = 3;
    fields.sensor_origin[2] = 0;
    const auto elevated = output::makeLioNavigationHeader(epoch, 2, fields);
    const rbt::navwire::LioPoint height_points[]{
        {2.5F, 3.5F, -.535F, 1}, // 0.20 m above the base
        {3.5F, 3.5F, -.735F, 1}  // ground at base height
    };
    const auto height_frame = rbt::navigation::lio::validateAndProject(
        elevated, height_points, now);
    assert(std::abs(height_frame.map_base_z - 2.265) < 1e-9);
    assert(std::abs(height_frame.sensor_origin_map.z() - 3) < 1e-9);
    rbt::navigation::OccupancyGrid grid;
    grid.width = 10;
    grid.height = 10;
    grid.resolution = 1;
    grid.cells.assign(100, 0);
    rbt::navigation::ObstacleLayer layer(grid);
    std::vector<rbt::navigation::ObstaclePoint> returns;
    for (const auto& point : height_frame.points_map)
        returns.push_back({point.x(), point.y(), point.z()});
    const auto& origin = height_frame.sensor_origin_map;
    assert(layer.integrate(returns, {origin.x(), origin.y(), origin.z()},
                           height_frame.map_base_link, height_frame.map_base_z));
    assert(layer.combinedGrid().cells[7 * grid.width + 5] == 100);
    assert(layer.combinedGrid().cells[7 * grid.width + 6] == 0);

    fields.map_odom_valid = false;
    const auto unlocalized = output::makeLioNavigationHeader(epoch, 3, fields);
    assert(unlocalized.flags == 0);
    bool rejected = false;
    try { (void)rbt::navigation::lio::validateAndProject(unlocalized, points, now); }
    catch (const std::exception&) { rejected = true; }
    assert(rejected);
    std::cout << "producer/consumer wire clock, pose, point and localization contract: PASS\n";
}
