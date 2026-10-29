#include <rbt_navigation/map/map_io.hpp>
#include <rbt_navigation/runtime/native.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using namespace rbt::navigation;
namespace native = rbt::navigation::native;
namespace wire = rbt::navwire;

namespace {
struct Options {
    std::filesystem::path map_file;
    std::string prefix = "rbt/navigation/demo";
    double publish_hz = 2.0;
    double duration = 0.0;
    bool help = false;
};

void printUsage() {
    std::cout << "Usage: rbt_navigation_map_server --map FILE [--prefix SERVICE] "
                 "[--publish-hz HZ] [--duration SECONDS] [--help]\n"
                 "  --map FILE          Occupancy map YAML file (required)\n"
                 "  --prefix SERVICE    Native service prefix (default: rbt/navigation/demo)\n"
                 "  --publish-hz HZ     Republish frequency from 1 to 20 (default: 2)\n"
                 "  --duration SECONDS  Run duration from 0 to 86400; 0 runs until signalled\n";
}

double parseNumber(const std::string& value, const char* name) {
    std::size_t used = 0;
    const double parsed = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(parsed))
        throw std::invalid_argument(std::string("invalid ") + name);
    return parsed;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    std::set<std::string, std::less<>> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help") {
            if (!seen.insert(flag).second) throw std::invalid_argument("duplicate flag: --help");
            options.help = true;
            continue;
        }
        if (flag != "--map" && flag != "--prefix" && flag != "--publish-hz" && flag != "--duration")
            throw std::invalid_argument("unknown flag: " + flag);
        if (!seen.insert(flag).second) throw std::invalid_argument("duplicate flag: " + flag);
        if (i + 1 >= argc || std::string_view(argv[i + 1]).starts_with("--"))
            throw std::invalid_argument("missing value for " + flag);
        const std::string value = argv[++i];
        if (value.empty()) throw std::invalid_argument("empty value for " + flag);
        if (flag == "--map") options.map_file = value;
        else if (flag == "--prefix") options.prefix = value;
        else if (flag == "--publish-hz") options.publish_hz = parseNumber(value, "publish frequency");
        else options.duration = parseNumber(value, "duration");
    }
    if (options.help) return options;
    if (options.map_file.empty()) throw std::invalid_argument("--map FILE is required");
    if (options.prefix.empty()) throw std::invalid_argument("prefix must be nonempty");
    if (options.publish_hz < 1.0 || options.publish_hz > 20.0 ||
        options.duration < 0.0 || options.duration > 86400.0)
        throw std::invalid_argument("publish frequency must be 1..20 Hz and duration 0..86400 seconds");
    return options;
}

std::size_t validateGrid(const OccupancyGrid& grid) {
    if (!grid.width || !grid.height ||
        grid.width > std::numeric_limits<std::uint32_t>::max() ||
        grid.height > std::numeric_limits<std::uint32_t>::max() ||
        grid.width > native::max_cells / grid.height)
        throw std::invalid_argument("map dimensions are empty or exceed native capacity");
    const auto cell_count = grid.width * grid.height;
    if (cell_count > native::max_cells || grid.cells.size() != cell_count)
        throw std::invalid_argument("map cell count does not match its dimensions");
    if (!std::isfinite(grid.resolution) || grid.resolution <= 0.0 ||
        !finite(grid.origin))
        throw std::invalid_argument("map geometry is invalid");
    if (std::any_of(grid.cells.begin(), grid.cells.end(), [](std::int8_t value) {
            return value < -1 || value > 100;
        }))
        throw std::invalid_argument("map occupancy values must be -1 or 0..100");
    return cell_count;
}

void publishMap(rbt::SlicePublisher<wire::MapHeader, std::int8_t>& publisher,
                const OccupancyGrid& grid, std::size_t cell_count,
                const rbt::time::Timestamp& stamp, std::uint64_t version) {
    wire::MapHeader header{};
    header.stamp = native::encodeStamp(stamp);
    header.version = version;
    header.width = static_cast<std::uint32_t>(grid.width);
    header.height = static_cast<std::uint32_t>(grid.height);
    header.resolution = grid.resolution;
    header.origin_x = grid.origin.x;
    header.origin_y = grid.origin.y;
    header.origin_yaw = grid.origin.yaw;
    header.frame[0] = 'm';
    header.frame[1] = 'a';
    header.frame[2] = 'p';

    auto loan = native::take(publisher.loan_slice(cell_count));
    loan.header() = header;
    std::copy(grid.cells.begin(), grid.cells.end(), loan.payload().begin());
    if (!loan.send()) throw std::runtime_error("map publish failed");
}
} // namespace

int main(int argc, char** argv) {
    try {
        native::installSignals();
        const auto options = parseOptions(argc, argv);
        if (options.help) {
            printUsage();
            return 0;
        }

        auto grid = loadOccupancyMap(options.map_file, native::max_cells);
        const auto cell_count = validateGrid(grid);
        auto clock = native::hostClock();
        const auto started_stamp = native::now(clock);
        if (started_stamp.ns < 0) throw std::runtime_error("HostClock returned a negative timestamp");
        const auto version = started_stamp.ns == 0
            ? std::uint64_t{1} : static_cast<std::uint64_t>(started_stamp.ns);
        auto publisher = native::take(rbt::SlicePublisher<wire::MapHeader, std::int8_t>::create(
            native::stream(options.prefix, "map", wire::OCCUPANCY_MAP_DESCRIPTOR),
            wire::OCCUPANCY_MAP_DESCRIPTOR, cell_count));

        const auto run_started = std::chrono::steady_clock::now();
        const auto run_duration = std::chrono::duration<double>(options.duration);
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / options.publish_hz));
        auto next_publish = run_started;
        bool ready_reported = false;
        while (!native::interrupted) {
            const auto steady_now = std::chrono::steady_clock::now();
            if (options.duration > 0.0 && steady_now - run_started >= run_duration) break;
            if (steady_now >= next_publish) {
                publishMap(publisher, grid, cell_count, native::now(clock), version);
                if (!ready_reported) {
                    std::cout << "navigation map server ready: file=" << options.map_file.string()
                              << " size=" << grid.width << 'x' << grid.height
                              << " cells=" << cell_count
                              << " resolution=" << grid.resolution
                              << " version=" << version
                              << " prefix=" << options.prefix << std::endl;
                    ready_reported = true;
                }
                next_publish = steady_now + period;
                continue;
            }
            const auto remaining = next_publish - steady_now;
            const auto max_sleep = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(20));
            std::this_thread::sleep_for(std::min(remaining, max_sleep));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "navigation map server: " << error.what() << '\n';
        return 1;
    }
}
