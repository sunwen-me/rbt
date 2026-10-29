#include <rbt_navigation/map/obstacle_layer.hpp>
#include <rbt_navigation/runtime/lio_bridge.hpp>
#include <rbt_navigation/runtime/native.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace rbt::navigation;
namespace lio = rbt::navigation::lio;
namespace native = rbt::navigation::native;
namespace wire = rbt::navwire;

namespace {
constexpr std::size_t max_map_cells = 1'000'000;
constexpr auto loop_period = std::chrono::milliseconds(10);

const std::set<std::string, std::less<>> config_keys{
    "provider", "prefix", "static-prefix", "tf-map", "tf-odom",
    "max-frame-age-ms", "static-map-age-ms", "base-x", "base-y", "base-z", "base-yaw",
    "obstacle-min-height", "obstacle-max-height", "obstacle-self-radius", "obstacle-max-range"};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

double parseNumber(std::string_view text, const std::string& name) {
    std::size_t used = 0;
    const double value = std::stod(std::string(text), &used);
    if (used != text.size() || !std::isfinite(value))
        throw std::invalid_argument("invalid number for " + name);
    return value;
}

struct BridgeOptions {
    std::string provider{"super-lio"};
    std::string prefix{"rbt/navigation/demo"};
    std::string static_prefix{"rbt/navigation/static"};
    std::string tf_map;
    std::string tf_odom;
    lio::ProjectionConfig projection;
    ObstacleLayerConfig obstacle_layer;
    TimePoint static_map_age_ns{2 * second};
    bool help{};
};

void readConfig(const std::filesystem::path& path,
                std::map<std::string, std::string, std::less<>>& values) {
    std::ifstream input(path);
    if (!input) throw std::invalid_argument("cannot read config: " + path.string());
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        if (++line_number > 256 || line.size() > 4096)
            throw std::invalid_argument("oversized LIO bridge config");
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        const auto equal = line.find('=');
        if (equal == std::string::npos)
            throw std::invalid_argument("expected key=value in LIO bridge config");
        const auto key = trim(line.substr(0, equal));
        const auto value = trim(line.substr(equal + 1));
        if (!config_keys.contains(key) || value.empty() || values.contains(key))
            throw std::invalid_argument("unknown, empty, or duplicate LIO bridge config key: " + key);
        values.emplace(key, value);
    }
}

BridgeOptions parseOptions(int argc, char** argv) {
    BridgeOptions result;
    std::optional<std::filesystem::path> config;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) != "--config") continue;
        if (config || index + 1 >= argc)
            throw std::invalid_argument("--config must appear once with a filename");
        config = argv[index + 1];
        ++index;
    }

    std::map<std::string, std::string, std::less<>> values;
    if (config) readConfig(*config, values);
    std::set<std::string, std::less<>> supplied;
    bool help = false;
    for (int index = 1; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--config") {
            ++index;
            continue;
        }
        if (flag == "--help") {
            if (help) throw std::invalid_argument("duplicate --help");
            help = true;
            continue;
        }
        if (!flag.starts_with("--") || !config_keys.contains(flag.substr(2)))
            throw std::invalid_argument("unknown LIO bridge option: " + flag);
        const auto key = flag.substr(2);
        if (!supplied.insert(key).second || index + 1 >= argc
            || std::string_view(argv[index + 1]).starts_with("--")) {
            throw std::invalid_argument("duplicate option or missing value: " + flag);
        }
        const std::string value = argv[++index];
        if (value.empty()) throw std::invalid_argument("empty value for " + flag);
        values[key] = value;
    }

    auto text = [&](const char* key, std::string fallback) {
        const auto found = values.find(key);
        return found == values.end() ? std::move(fallback) : found->second;
    };
    auto number = [&](const char* key, double fallback) {
        const auto found = values.find(key);
        return found == values.end() ? fallback : parseNumber(found->second, key);
    };
    result.provider = text("provider", result.provider);
    result.prefix = text("prefix", result.prefix);
    result.static_prefix = text("static-prefix", result.static_prefix);
    result.tf_map = text("tf-map", {});
    result.tf_odom = text("tf-odom", {});
    const double frame_age_ms = number("max-frame-age-ms", 250.0);
    const double static_age_ms = number("static-map-age-ms", 2000.0);
    if (frame_age_ms <= 0.0 || frame_age_ms > 250.0
        || static_age_ms <= 0.0 || static_age_ms > 86'400'000.0) {
        throw std::invalid_argument("frame age must be in (0, 250] ms and static-map age must be positive");
    }
    result.projection.max_frame_age_ns = static_cast<TimePoint>(frame_age_ms * 1'000'000.0);
    result.static_map_age_ns = static_cast<TimePoint>(static_age_ms * 1'000'000.0);
    result.projection.base_to_base_link = {number("base-x", 0.0), number("base-y", 0.0),
                                           number("base-z", 0.0), number("base-yaw", 0.0)};
    result.obstacle_layer.min_height = number("obstacle-min-height", result.obstacle_layer.min_height);
    result.obstacle_layer.max_height = number("obstacle-max-height", result.obstacle_layer.max_height);
    result.obstacle_layer.self_radius = number("obstacle-self-radius", result.obstacle_layer.self_radius);
    result.obstacle_layer.max_range = number("obstacle-max-range", result.obstacle_layer.max_range);
    if (result.obstacle_layer.min_height < 0.0
        || result.obstacle_layer.max_height <= result.obstacle_layer.min_height
        || result.obstacle_layer.self_radius < 0.0 || result.obstacle_layer.max_range <= 0.0) {
        throw std::invalid_argument("invalid obstacle-layer height, radius, or range limits");
    }
    result.help = help;
    if (help) return result;
    if (result.provider.empty() || result.prefix.empty() || result.static_prefix.empty()
        || result.tf_map.empty() || result.tf_odom.empty()) {
        throw std::invalid_argument("provider, prefixes, --tf-map, and --tf-odom must be nonempty");
    }
    if (result.tf_map == result.tf_odom)
        throw std::invalid_argument("map and odom TF authorities must be distinct");
    if (result.prefix == result.static_prefix)
        throw std::invalid_argument("output and static map prefixes must be distinct");
    return result;
}

void printUsage() {
    std::cout << "Usage: rbt_navigation_lio_bridge --tf-map UUID --tf-odom UUID [options]\n"
                 "  --provider ID           Required LIO provider component (default: super-lio)\n"
                 "  --prefix SERVICE        Fused navigation output prefix (default: rbt/navigation/demo)\n"
                 "  --static-prefix SERVICE Static OccupancyMap input prefix (default: rbt/navigation/static)\n"
                 "  --max-frame-age-ms MS   Maximum LIO frame age (default: 250)\n"
                 "  --static-map-age-ms MS  Maximum static map age (default: 2000)\n"
                 "  --base-x M --base-y M --base-z M --base-yaw RAD\n"
                 "                           Planar base_footprint-to-base_link extrinsic (default: identity)\n"
                 "  --obstacle-min-height M --obstacle-max-height M --obstacle-self-radius M\n"
                 "  --obstacle-max-range M   Obstacle-layer settings (default: ObstacleLayerConfig)\n"
                 "  --config FILE           Strict key=value config file\n";
}

bool sameGrid(const OccupancyGrid& lhs, const OccupancyGrid& rhs) {
    return lhs.width == rhs.width && lhs.height == rhs.height
        && lhs.resolution == rhs.resolution && lhs.origin.x == rhs.origin.x
        && lhs.origin.y == rhs.origin.y && lhs.origin.yaw == rhs.origin.yaw
        && lhs.occupied_threshold == rhs.occupied_threshold && lhs.cells == rhs.cells;
}

bool sameGeometry(const wire::MapHeader& lhs, const wire::MapHeader& rhs) {
    return lhs.width == rhs.width && lhs.height == rhs.height
        && lhs.resolution == rhs.resolution && lhs.origin_x == rhs.origin_x
        && lhs.origin_y == rhs.origin_y && lhs.origin_yaw == rhs.origin_yaw;
}

bool mapFrame(const std::uint8_t (&frame)[64]) {
    return std::memcmp(frame, "map\0", 4) == 0
        && std::all_of(std::begin(frame) + 4, std::end(frame),
                       [](std::uint8_t value) { return value == 0; });
}

struct StaticMapCache {
    std::optional<OccupancyGrid> grid;
    wire::MapHeader header{};
    std::uint64_t version{};
    TimePoint stamp_ns{-1};
    bool valid{};

    void reject() noexcept { valid = false; }

    void accept(const wire::MapHeader& next_header, std::span<const std::int8_t> cells,
                const rbt::time::Timestamp& now, TimePoint max_age) {
        if (!mapFrame(next_header.frame)) throw std::invalid_argument("static map frame is not map");
        const auto stamp = native::decodeStamp(next_header.stamp, now.clock);
        if (!fresh(stamp, now.ns, max_age) || !next_header.version || !next_header.width
            || !next_header.height || next_header.width > max_map_cells / next_header.height
            || cells.size() != static_cast<std::size_t>(next_header.width) * next_header.height
            || !std::isfinite(next_header.resolution) || next_header.resolution <= 0.0
            || !std::isfinite(next_header.origin_x) || !std::isfinite(next_header.origin_y)
            || !std::isfinite(next_header.origin_yaw)
            || std::any_of(cells.begin(), cells.end(), [](std::int8_t cell) {
                   return cell < -1 || cell > 100;
               })) {
            throw std::invalid_argument("static map sample is stale or malformed");
        }
        if (next_header.version < version || (next_header.version == version && stamp < stamp_ns))
            return;
        if (next_header.version == version && grid) {
            if (!sameGeometry(next_header, header)
                || !std::equal(cells.begin(), cells.end(), grid->cells.begin(), grid->cells.end())) {
                throw std::invalid_argument("static map changed without a version increment");
            }
            stamp_ns = stamp;
            valid = true;
            return;
        }
        OccupancyGrid next{next_header.width, next_header.height, next_header.resolution,
                           {next_header.origin_x, next_header.origin_y, next_header.origin_yaw},
                           std::vector<std::int8_t>(cells.begin(), cells.end()), 50};
        grid = std::move(next);
        header = next_header;
        version = next_header.version;
        stamp_ns = stamp;
        valid = true;
    }
};

void publishMap(rbt::SlicePublisher<wire::MapHeader, std::int8_t>& publisher,
                const OccupancyGrid& grid, std::uint64_t version, TimePoint stamp_ns,
                const rbt::time::ClockIdentity& clock) {
    wire::MapHeader header{};
    header.stamp = native::encodeStamp(rbt::time::Timestamp{clock, stamp_ns});
    header.version = version;
    header.width = static_cast<std::uint32_t>(grid.width);
    header.height = static_cast<std::uint32_t>(grid.height);
    header.resolution = grid.resolution;
    header.origin_x = grid.origin.x;
    header.origin_y = grid.origin.y;
    header.origin_yaw = grid.origin.yaw;
    std::copy_n("map", 3, header.frame);
    auto loan = native::take(publisher.loan_slice(grid.cells.size()));
    loan.header() = header;
    std::copy(grid.cells.begin(), grid.cells.end(), loan.payload().begin());
    if (!loan.send()) throw std::runtime_error("fused OccupancyMap publish failed");
}

void rememberRejectedFrame(const wire::LioFrameHeader& header,
                           const rbt::time::ClockIdentity& clock,
                           std::uint64_t& sequence_floor, TimePoint& stamp_floor) noexcept {
    sequence_floor = std::max(sequence_floor, header.sequence);
    try {
        const auto stamp = native::decodeStamp(header.stamp, clock);
        stamp_floor = std::max(stamp_floor, stamp);
    } catch (...) {}
}

class LioBridgeComponent final : public rbt::Component {
public:
    explicit LioBridgeComponent(BridgeOptions options) : options_(std::move(options)) {}

    rbt::Result<void> prepare() override { return {}; }
    rbt::ComponentId id() const override { return "rbt-navigation-lio-bridge"; }
    std::vector<rbt::RequirementSpec> requirements() const override {
        return {{"lio_frame", lio::LioNavigationFrameV1::TYPE_NAME}};
    }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        try {
            auto frame_capability = context.require<lio::LioNavigationFrameV1>("lio_frame");
            if (!frame_capability) return rbt::Result<void>::failure(frame_capability.error());
            if (frame_capability.provider() != options_.provider) {
                return rbt::Result<void>::failure("resolved LIO provider differs from --provider");
            }
            auto opened = frame_capability.slice_subscriber<wire::LioFrameHeader, wire::LioPoint>();
            if (!opened) {
                return rbt::Result<void>::failure("cannot open resolved LIO slice: "
                    + std::string(rbt::to_string(opened.error())));
            }

            auto odometry = native::publisher<wire::Odom2D>(options_.prefix, "odom");
            auto map_publisher_result = rbt::SlicePublisher<wire::MapHeader, std::int8_t>::create(
                native::stream(options_.prefix, "map", wire::OCCUPANCY_MAP_DESCRIPTOR),
                wire::OCCUPANCY_MAP_DESCRIPTOR, max_map_cells);
            auto map_publisher = native::take(std::move(map_publisher_result));
            auto map_tf = rbt::tf::DynamicTfPublisher::open(options_.tf_map);
            auto odom_tf = rbt::tf::DynamicTfPublisher::open(options_.tf_odom);
            auto clock = native::hostClock();
            const auto options = options_;
            const auto task = context.spawn_critical("lio-navigation-bridge",
                [options, input = std::move(opened).value(), odometry = std::move(odometry),
                 map_publisher = std::move(map_publisher), map_tf = std::move(map_tf),
                 odom_tf = std::move(odom_tf), clock = std::move(clock)](std::stop_token stop) mutable {
                    runBridge(options, std::move(input), std::move(odometry),
                              std::move(map_publisher), std::move(map_tf), std::move(odom_tf),
                              std::move(clock), stop);
                });
            if (!task) return rbt::Result<void>::failure("failed to start LIO bridge activation task");
            std::cout << "lio bridge ready: provider=" << options_.provider
                      << " output_prefix=" << options_.prefix << std::endl;
            return {};
        } catch (const std::exception& error) {
            return rbt::Result<void>::failure(error.what());
        }
    }

private:
    static void runBridge(const BridgeOptions& options,
                          rbt::SliceSubscriber<wire::LioFrameHeader, wire::LioPoint> input,
                          rbt::FixedPublisher<wire::Odom2D> odometry,
                          rbt::SlicePublisher<wire::MapHeader, std::int8_t> map_publisher,
                          rbt::tf::DynamicTfPublisher map_tf,
                          rbt::tf::DynamicTfPublisher odom_tf,
                          rbt::time::HostClock clock,
                          std::stop_token stop) {
        const auto static_stream = native::stream(options.static_prefix, "map",
                                                  wire::OCCUPANCY_MAP_DESCRIPTOR);
        std::optional<rbt::SliceSubscriber<wire::MapHeader, std::int8_t>> static_input;
        StaticMapCache static_map;
        std::optional<ObstacleLayer> obstacle_layer;
        std::optional<OccupancyGrid> layer_static_grid;
        std::optional<OccupancyGrid> previous_fused_grid;
        const auto started = native::now(clock).ns;
        if (started < 0) throw std::runtime_error("HostClock returned a negative timestamp");
        std::uint64_t fused_version = started == 0 ? 1U : static_cast<std::uint64_t>(started);
        std::uint64_t sequence_floor = 0;
        TimePoint stamp_floor = -1;
        while (!stop.stop_requested()) {
            const auto deadline = std::chrono::steady_clock::now() + loop_period;

            try {
                if (!static_input) {
                    auto result = rbt::SliceSubscriber<wire::MapHeader, std::int8_t>::create(
                        static_stream, wire::OCCUPANCY_MAP_DESCRIPTOR);
                    if (result) static_input.emplace(std::move(result).value());
                    else if (result.error() != rbt::StreamError::ReadinessOpen
                             && result.error() != rbt::StreamError::Transport) {
                        throw std::runtime_error("static map subscriber open: "
                            + std::string(rbt::to_string(result.error())));
                    }
                }
                for (std::size_t count = 0; static_input && count < rbt::MAX_DRAIN_PER_SOURCE; ++count) {
                    auto received = native::take(static_input->receive());
                    if (!received) break;
                    try {
                        static_map.accept(received->header(), received->payload(),
                                          native::now(clock), options.static_map_age_ns);
                    } catch (const std::exception& error) {
                        static_map.reject();
                        std::cerr << "lio bridge static map rejected: " << error.what() << '\n';
                    }
                }
            } catch (const std::exception& error) {
                static_map.reject();
                static_input.reset();
                std::cerr << "lio bridge static map input: " << error.what() << '\n';
            }

            for (std::size_t count = 0; count < rbt::MAX_DRAIN_PER_SOURCE; ++count) {
                auto received = native::take(input.receive());
                if (!received) break;
                const auto& header = received->header();
                std::optional<lio::ProjectedFrame> projected;
                std::optional<rbt::time::Timestamp> map_now;
                const OccupancyGrid* fused_to_publish = nullptr;
                std::optional<TimePoint> fused_stamp;
                try {
                    projected.emplace(lio::validateAndProject(
                        header, received->payload(), native::now(clock), options.projection,
                        sequence_floor, stamp_floor));
                    sequence_floor = projected->sequence;
                    stamp_floor = projected->stamp.ns;

                    map_now = native::now(clock);
                    if (static_map.valid && static_map.grid
                        && fresh(static_map.stamp_ns, map_now->ns, options.static_map_age_ns)) {
                        if (!obstacle_layer || !layer_static_grid
                            || !sameGrid(*layer_static_grid, *static_map.grid)) {
                            obstacle_layer.emplace(*static_map.grid, options.obstacle_layer);
                            layer_static_grid = *static_map.grid;
                        }
                        std::vector<ObstaclePoint> points;
                        points.reserve(projected->points_map.size());
                        for (const auto& point : projected->points_map)
                            points.push_back({point.x(), point.y(), point.z()});
                        const auto& sensor = projected->sensor_origin_map;
                        const auto& pose = projected->map_base_link;
                        (void)obstacle_layer->integrate(points,
                            {sensor.x(), sensor.y(), sensor.z()}, pose, projected->map_base_z);
                        const auto& fused = obstacle_layer->combinedGrid();
                        if (previous_fused_grid && !sameGrid(*previous_fused_grid, fused)) {
                            if (fused_version == std::numeric_limits<std::uint64_t>::max())
                                throw std::overflow_error("fused map version exhausted");
                            ++fused_version;
                        }
                        if (!previous_fused_grid || !sameGrid(*previous_fused_grid, fused)) {
                            previous_fused_grid = fused;
                        }
                        fused_to_publish = &fused;
                        fused_stamp = std::min(static_map.stamp_ns, projected->stamp.ns);
                    }
                } catch (const std::exception& error) {
                    rememberRejectedFrame(header, native::now(clock).clock,
                                          sequence_floor, stamp_floor);
                    std::cerr << "lio bridge frame rejected: " << error.what() << '\n';
                    continue;
                }
                map_tf.publish(projected->map_to_odom);
                odom_tf.publish(projected->odom_to_base_link);
                native::publish(odometry, projected->odometry);
                if (fused_to_publish)
                    publishMap(map_publisher, *fused_to_publish, fused_version,
                               *fused_stamp, map_now->clock);
            }
            std::this_thread::sleep_until(deadline);
        }
    }

    BridgeOptions options_;
};
} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        if (options.help) {
            printUsage();
            return 0;
        }
        rbt::RuntimeOptions runtime_options;
        if (const char* address = std::getenv("RBT_COMPOSER_ADDR");
            address != nullptr && *address != '\0') {
            runtime_options.composer_address = address;
        }
        rbt::ComponentRuntime runtime(std::make_unique<LioBridgeComponent>(options),
                                      std::move(runtime_options));
        return runtime.run();
    } catch (const std::exception& error) {
        std::cerr << "lio bridge: " << error.what() << '\n';
        return 1;
    }
}
