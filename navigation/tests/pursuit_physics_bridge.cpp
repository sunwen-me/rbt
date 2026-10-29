#include <rbt_navigation/controller/pure_pursuit.hpp>
#include <rbt_navigation/controller/vector_pursuit.hpp>
#include <rbt_navigation/core/types.hpp>
#include <rbt_navigation/geometry/footprint.hpp>
#include <rbt_navigation/geometry/geometry_io.hpp>
#include <rbt_navigation/map/grid_map.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rbt::navigation;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaximumMapCells = 4'000'000U;
constexpr std::size_t kMaximumPathPoses = 1'000'000U;
constexpr double kDt = 0.02;
constexpr TimePoint kStartOffsetNs = 1;

[[noreturn]] void invalid(const std::string& message) {
    throw std::runtime_error(message);
}

struct Arguments {
    std::string controller;
    fs::path path;
    fs::path map;
    fs::path geometry;
    bool allow_reverse{true};
    double lookahead_time{1.5};
    double min_lookahead{0.3};
    double vector_pursuit_k{5.0};
    bool vector_pursuit_use_heading_from_path{true};
};

std::vector<std::string_view> splitCsv(const std::string& line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0U;
    while (true) {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string::npos) {
            fields.emplace_back(line.data() + start, line.size() - start);
            return fields;
        }
        fields.emplace_back(line.data() + start, comma - start);
        start = comma + 1U;
    }
}

template <typename Integer>
Integer parseInteger(std::string_view value, const std::string& name) {
    Integer parsed_value{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        invalid("invalid " + name);
    }
    return parsed_value;
}

double parseCsvDouble(std::string_view value, const std::string& name) {
    double parsed_value{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value,
                                        std::chars_format::general);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
        || !std::isfinite(parsed_value)) {
        invalid("invalid finite " + name);
    }
    return parsed_value;
}

double parseOptionDouble(const std::string& value, const std::string& name) {
    double parsed_value{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value,
                                        std::chars_format::general);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
        || !std::isfinite(parsed_value)) {
        invalid("invalid finite " + name);
    }
    return parsed_value;
}

Arguments parseArguments(int argc, char** argv) {
    Arguments args;
    std::vector<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string option(argv[i]);
        if (option != "--controller" && option != "--path" && option != "--map"
            && option != "--geometry" && option != "--allow-reverse"
            && option != "--lookahead-time" && option != "--min-lookahead"
            && option != "--vp-k" && option != "--vp-use-heading-from-path") {
            invalid("unknown argument: " + option);
        }
        if (std::find(seen.begin(), seen.end(), option) != seen.end()) {
            invalid("duplicate argument: " + option);
        }
        seen.push_back(option);
        if (i + 1 >= argc) invalid("missing value for " + option);
        const std::string value(argv[++i]);
        if (option == "--controller") args.controller = value;
        else if (option == "--path") args.path = value;
        else if (option == "--map") args.map = value;
        else if (option == "--geometry") args.geometry = value;
        else if (option == "--allow-reverse") {
            if (value != "0" && value != "1") invalid("--allow-reverse must be 0 or 1");
            args.allow_reverse = value == "1";
        } else if (option == "--vp-use-heading-from-path") {
            if (value != "0" && value != "1") {
                invalid("--vp-use-heading-from-path must be 0 or 1");
            }
            args.vector_pursuit_use_heading_from_path = value == "1";
        } else if (option == "--lookahead-time") {
            args.lookahead_time = parseOptionDouble(value, "--lookahead-time");
        } else if (option == "--min-lookahead") {
            args.min_lookahead = parseOptionDouble(value, "--min-lookahead");
        } else {
            args.vector_pursuit_k = parseOptionDouble(value, "--vp-k");
        }
    }
    if (args.controller != "rpp" && args.controller != "rpp-dwpp"
        && args.controller != "vector-pursuit" && args.controller != "vector-pursuit-window") {
        invalid("--controller must be rpp, rpp-dwpp, vector-pursuit, or vector-pursuit-window");
    }
    if (args.path.empty() || args.map.empty() || args.geometry.empty()) {
        invalid("usage: pursuit_physics_bridge --controller rpp|rpp-dwpp|vector-pursuit|vector-pursuit-window"
                " --path FILE --map FILE --geometry FILE [--allow-reverse 0|1]"
                " [--lookahead-time SECONDS] [--min-lookahead METRES] [--vp-k VALUE]"
                " [--vp-use-heading-from-path 0|1]");
    }
    if (!(args.lookahead_time > 0.0) || !(args.min_lookahead > 0.0)
        || args.min_lookahead > 0.8 || !(args.vector_pursuit_k > 0.0)) {
        invalid("controller lookahead and vector pursuit settings are outside valid ranges");
    }
    for (const fs::path& file : {args.path, args.map, args.geometry}) {
        if (!fs::is_regular_file(file)) invalid("input file does not exist: " + file.string());
    }
    return args;
}

Path2D loadPathCsv(const fs::path& file) {
    std::ifstream input(file);
    if (!input) invalid("cannot open path CSV: " + file.string());
    std::string line;
    if (!std::getline(input, line)
        || line != "index,x,y,body_yaw,production_pose_free,production_segment_free") {
        invalid("path CSV header is invalid: " + file.string());
    }
    Path2D path;
    path.map_version = 1U;
    path.goal_id = 1U;
    while (std::getline(input, line)) {
        if (line.empty()) invalid("path CSV contains an empty row");
        const auto fields = splitCsv(line);
        if (fields.size() != 6U) invalid("path CSV row must have six columns");
        if (path.poses.size() >= kMaximumPathPoses) invalid("path CSV exceeds pose limit");
        const std::size_t index = parseInteger<std::size_t>(fields[0], "path index");
        if (index != path.poses.size()) invalid("path CSV indices must be contiguous from zero");
        const Pose2D pose{parseCsvDouble(fields[1], "path x"),
                          parseCsvDouble(fields[2], "path y"),
                          parseCsvDouble(fields[3], "path body_yaw")};
        const int pose_free = parseInteger<int>(fields[4], "path production_pose_free");
        const int segment_free = parseInteger<int>(fields[5], "path production_segment_free");
        if (!finite(pose) || (pose_free != 0 && pose_free != 1)
            || (segment_free != 0 && segment_free != 1)) {
            invalid("path CSV row contains invalid values");
        }
        path.poses.push_back(pose);
    }
    if (input.bad()) invalid("failed reading path CSV: " + file.string());
    if (path.poses.empty()) invalid("path CSV contains no poses");
    return path;
}

struct MapMetadata {
    std::size_t width{};
    std::size_t height{};
    double resolution{};
    Pose2D origin{};
};

bool sameMetadata(const MapMetadata& a, const MapMetadata& b) noexcept {
    return a.width == b.width && a.height == b.height && a.resolution == b.resolution
        && a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.origin.yaw == b.origin.yaw;
}

OccupancyGrid loadMapCsv(const fs::path& file) {
    std::ifstream input(file);
    if (!input) invalid("cannot open map CSV: " + file.string());
    std::string line;
    if (!std::getline(input, line)
        || line != "width,height,resolution_m,origin_x,origin_y,origin_yaw,cell_x,cell_y,world_x,world_y,occupancy") {
        invalid("map CSV header is invalid: " + file.string());
    }
    OccupancyGrid grid;
    std::vector<std::uint8_t> seen;
    MapMetadata metadata;
    bool have_metadata = false;
    std::size_t row_count = 0U;
    while (std::getline(input, line)) {
        if (line.empty()) invalid("map CSV contains an empty row");
        const auto fields = splitCsv(line);
        if (fields.size() != 11U) invalid("map CSV row must have eleven columns");
        MapMetadata row_metadata;
        row_metadata.width = parseInteger<std::size_t>(fields[0], "map width");
        row_metadata.height = parseInteger<std::size_t>(fields[1], "map height");
        row_metadata.resolution = parseCsvDouble(fields[2], "map resolution");
        row_metadata.origin = {parseCsvDouble(fields[3], "map origin_x"),
                               parseCsvDouble(fields[4], "map origin_y"),
                               parseCsvDouble(fields[5], "map origin_yaw")};
        if (row_metadata.width == 0U || row_metadata.height == 0U
            || row_metadata.width > kMaximumMapCells / row_metadata.height
            || row_metadata.width * row_metadata.height > kMaximumMapCells
            || !(row_metadata.resolution > 0.0) || !finite(row_metadata.origin)) {
            invalid("map CSV metadata is outside valid ranges");
        }
        if (!have_metadata) {
            metadata = row_metadata;
            grid.width = metadata.width;
            grid.height = metadata.height;
            grid.resolution = metadata.resolution;
            grid.origin = metadata.origin;
            grid.cells.resize(grid.width * grid.height);
            seen.resize(grid.cells.size(), 0U);
            have_metadata = true;
        } else if (!sameMetadata(metadata, row_metadata)) {
            invalid("map CSV metadata changes between rows");
        }
        const std::size_t cell_x = parseInteger<std::size_t>(fields[6], "map cell_x");
        const std::size_t cell_y = parseInteger<std::size_t>(fields[7], "map cell_y");
        (void)parseCsvDouble(fields[8], "map world_x");
        (void)parseCsvDouble(fields[9], "map world_y");
        const int occupancy = parseInteger<int>(fields[10], "map occupancy");
        if (cell_x >= grid.width || cell_y >= grid.height || occupancy < -1 || occupancy > 100) {
            invalid("map CSV cell is outside valid ranges");
        }
        const std::size_t cell_index = cell_y * grid.width + cell_x;
        if (seen[cell_index] != 0U) invalid("map CSV contains a duplicate cell");
        seen[cell_index] = 1U;
        grid.cells[cell_index] = static_cast<std::int8_t>(occupancy);
        ++row_count;
    }
    if (input.bad()) invalid("failed reading map CSV: " + file.string());
    if (!have_metadata || row_count != grid.width * grid.height
        || std::find(seen.begin(), seen.end(), 0U) != seen.end()) {
        invalid("map CSV does not contain every cell exactly once");
    }
    return grid;
}

TimePoint secondsToTime(double seconds_value, const std::string& name) {
    if (!std::isfinite(seconds_value) || seconds_value < 0.0) {
        invalid(name + " must be finite and nonnegative");
    }
    const long double nanoseconds = static_cast<long double>(seconds_value)
        * static_cast<long double>(second);
    const long double max_time = static_cast<long double>(std::numeric_limits<TimePoint>::max()
                                                          - kStartOffsetNs);
    if (!std::isfinite(nanoseconds) || nanoseconds > max_time) {
        invalid(name + " is outside the supported time range");
    }
    return static_cast<TimePoint>(std::round(nanoseconds)) + kStartOffsetNs;
}

struct InputSample {
    double now_s{};
    double pose_stamp_s{};
    double odom_stamp_s{};
    Pose2D pose{};
    Twist2D measured{};
};

InputSample parseInputLine(const std::string& line) {
    std::istringstream input(line);
    InputSample sample;
    std::string extra;
    if (!(input >> sample.now_s >> sample.pose_stamp_s >> sample.odom_stamp_s
          >> sample.pose.x >> sample.pose.y >> sample.pose.yaw
          >> sample.measured.linear_x >> sample.measured.angular_z)
        || (input >> extra)) {
        invalid("stdin line must contain exactly eight finite doubles");
    }
    if (!std::isfinite(sample.now_s) || !std::isfinite(sample.pose_stamp_s)
        || !std::isfinite(sample.odom_stamp_s) || !finite(sample.pose) || !finite(sample.measured)) {
        invalid("stdin line must contain exactly eight finite doubles");
    }
    return sample;
}

std::unique_ptr<ILocalController> makeController(const Arguments& args,
                                                  ControllerConfig config) {
    config.geometry = args.controller == "vector-pursuit"
        || args.controller == "vector-pursuit-window"
        ? PursuitGeometry::VectorPursuit : PursuitGeometry::PurePursuit;
    config.use_dynamic_window = args.controller == "rpp-dwpp"
        || args.controller == "vector-pursuit-window";
    if (config.geometry == PursuitGeometry::VectorPursuit) {
        return std::make_unique<VectorPursuit>(config);
    }
    return std::make_unique<RegulatedPurePursuit>(config);
}

ControllerConfig controllerConfig(const Arguments& args) {
    ControllerConfig config;
    config.max_linear = 0.2;
    config.max_angular = 0.5;
    config.max_acceleration = 0.8;
    config.max_deceleration = 0.8;
    config.max_angular_acceleration = 2.5;
    config.max_angular_deceleration = 2.5;
    config.lookahead_time = args.lookahead_time;
    config.min_lookahead = args.min_lookahead;
    config.position_tolerance = 0.08;
    config.yaw_tolerance = 0.06;
    config.rotate_to_heading_speed = 0.5;
    config.use_feasible_path_handler = true;
    config.allow_reverse = args.allow_reverse;
    config.vector_pursuit_k = args.vector_pursuit_k;
    config.vector_pursuit_min_radius = 0.10;
    config.vector_pursuit_use_heading_from_path = args.vector_pursuit_use_heading_from_path;
    config.reaction_time = 0.25;
    return config;
}

void printResult(ErrorCode error, bool reached, Twist2D command) {
    if (error != ErrorCode::None || !finite(command)) {
        if (error == ErrorCode::None) error = ErrorCode::ControllerFailure;
        command = {};
        reached = false;
    }
    std::cout << static_cast<std::uint32_t>(error) << ' ' << (reached ? 1 : 0) << ' '
              << command.linear_x << ' ' << command.angular_z << '\n' << std::flush;
    if (!std::cout) invalid("failed writing controller response");
}

int run(const Arguments& args) {
    Path2D path = loadPathCsv(args.path);
    OccupancyGrid occupancy = loadMapCsv(args.map);
    auto map = std::make_shared<const GridMap>(std::move(occupancy));
    const RobotGeometry geometry = loadRobotGeometry(args.geometry.string());
    auto footprint = std::make_shared<const FootprintCollisionChecker>(geometry.footprint);
    ControllerConfig config = controllerConfig(args);
    std::unique_ptr<ILocalController> controller = makeController(args, config);
    const double robot_radius = footprint->radius();
    Twist2D previous_command{};
    TimePoint previous_now_ns = -1;
    std::cout << std::setprecision(17);
    std::string line;
    while (std::getline(std::cin, line)) {
        const InputSample sample = parseInputLine(line);
        const TimePoint now_ns = secondsToTime(sample.now_s, "now_s");
        const TimePoint tf_stamp_ns = secondsToTime(sample.pose_stamp_s, "pose_stamp_s");
        const TimePoint odom_stamp_ns = secondsToTime(sample.odom_stamp_s, "odom_stamp_s");
        if (now_ns < previous_now_ns) invalid("now_s must be monotonic");
        previous_now_ns = now_ns;
        const RobotState state{sample.pose, sample.measured, tf_stamp_ns, odom_stamp_ns, true};
        const MapSnapshot map_snapshot{map, path.map_version, now_ns};
        const ControllerContext context{now_ns, kDt, previous_command, map_snapshot,
            robot_radius, 250'000'000, 2 * second, footprint};
        ControllerResult result;
        try {
            result = controller->computeCommand(state, path, context);
        } catch (const std::exception& error) {
            std::cerr << "pursuit physics bridge: controller threw: " << error.what() << '\n';
            result = {{}, ErrorCode::ControllerFailure, false};
        } catch (...) {
            std::cerr << "pursuit physics bridge: controller threw an unknown exception\n";
            result = {{}, ErrorCode::ControllerFailure, false};
        }
        printResult(result.error, result.reached_goal, result.command);
        previous_command = result.error == ErrorCode::None && finite(result.command)
            ? result.command : Twist2D{};
    }
    if (std::cin.bad()) invalid("failed reading stdin");
    return 0;
}

} // namespace

int main(int argc, char** argv) try {
    return run(parseArguments(argc, argv));
} catch (const std::exception& error) {
    std::cerr << "pursuit physics bridge: " << error.what() << '\n';
    return 1;
}
