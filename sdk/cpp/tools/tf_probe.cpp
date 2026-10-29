#include <rbt/tf_transport.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rbt::CapabilityInstanceId;
using rbt::tf::DynamicRecord;
using rbt::tf::FrameId;
using rbt::tf::LookupOptions;
using rbt::tf::Quaternion;
using rbt::tf::StaticEdge;
using rbt::tf::StaticSet;
using rbt::tf::TfError;
using rbt::tf::TfErrorCode;
using rbt::tf::Transform3;
using rbt::tf::TransformBuffer;
using rbt::tf::Vec3;
using rbt::time::ClockIdentity;
using rbt::time::ClockKind;
using rbt::time::ClockMapping;
using rbt::time::ClockMappingCalibration;
using rbt::time::ClockSourceId;
using rbt::time::Timestamp;

struct InteropArgs {
    CapabilityInstanceId static_id;
    CapabilityInstanceId dynamic_id;
    CapabilityInstanceId snapshot_id;
    std::filesystem::path ready;
    std::filesystem::path peer_ready;
    std::filesystem::path done;
    std::chrono::seconds timeout { 0 };
};

[[nodiscard]] ClockIdentity fixture_clock(ClockKind kind = ClockKind::Device,
                                          std::uint8_t source_seed = 42U,
                                          std::uint64_t generation = 7U) {
    std::array<std::uint8_t, 48U> bytes {};
    std::fill_n(bytes.begin(), 16U, source_seed);
    auto result = ClockIdentity::create(kind, ClockSourceId(std::move(bytes)), generation);
    if (!result) {
        throw std::runtime_error("cannot create fixture clock");
    }
    return std::move(result).value();
}

[[nodiscard]] Transform3 translated(double x, double y = 0.0, double z = 0.0) {
    return Transform3(Vec3(x, y, z), Quaternion::identity());
}

[[nodiscard]] Timestamp fixture_stamp(const ClockIdentity& clock, std::int64_t ns) {
    return Timestamp { clock, ns };
}

[[nodiscard]] StaticSet fixture_static(std::uint64_t revision,
                                       const ClockIdentity& clock) {
    return StaticSet { revision,
                       fixture_stamp(clock, 0),
                       0U,
                       { StaticEdge { FrameId("base_link"),
                                      FrameId("camera_link"),
                                      translated(1.0) },
                         StaticEdge { FrameId("camera_link"),
                                      FrameId("camera_optical"),
                                      Transform3::identity() } } };
}

[[nodiscard]] std::array<DynamicRecord, 2U> fixture_dynamics(const ClockIdentity& clock) {
    return { DynamicRecord { FrameId("odom"),
                             FrameId("base_link"),
                             fixture_stamp(clock, 100'000'000),
                             0U,
                             3U,
                             translated(0.0) },
             DynamicRecord { FrameId("odom"),
                             FrameId("base_link"),
                             fixture_stamp(clock, 200'000'000),
                             0U,
                             3U,
                             Transform3(Vec3(2.0, 0.0, 0.0),
                                        Quaternion(0.0, 0.0, std::sin(std::numbers::pi / 4.0),
                                                   std::cos(std::numbers::pi / 4.0))) } };
}

[[nodiscard]] bool near(double lhs, double rhs, double epsilon = 1.0e-9) {
    return std::abs(lhs - rhs) <= epsilon;
}

[[nodiscard]] bool near(const Vec3& lhs, const Vec3& rhs, double epsilon = 1.0e-9) {
    return near(lhs.x(), rhs.x(), epsilon) && near(lhs.y(), rhs.y(), epsilon)
           && near(lhs.z(), rhs.z(), epsilon);
}

void touch_file(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::runtime_error("empty synchronization path");
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create synchronization file: " + path.string());
    }
}

[[nodiscard]] bool wait_for_file(const std::filesystem::path& path,
                                 std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return std::filesystem::exists(path);
}

[[nodiscard]] std::uint64_t parse_positive_integer(std::string_view value,
                                                   std::uint64_t maximum) {
    std::size_t consumed = 0U;
    const auto parsed = std::stoull(std::string(value), &consumed, 10);
    if (consumed != value.size() || parsed == 0U || parsed > maximum) {
        throw std::runtime_error("invalid positive integer argument");
    }
    return parsed;
}

[[nodiscard]] InteropArgs parse_interop_args(int argc, char** argv) {
    InteropArgs args;
    for (int index = 3; index < argc; ++index) {
        const std::string_view key(argv[index]);
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for argument " + std::string(key));
        }
        const std::string_view value(argv[++index]);
        if (key == "--static-id") {
            args.static_id = value;
        } else if (key == "--dynamic-id") {
            args.dynamic_id = value;
        } else if (key == "--snapshot-id") {
            args.snapshot_id = value;
        } else if (key == "--ready") {
            args.ready = value;
        } else if (key == "--peer-ready") {
            args.peer_ready = value;
        } else if (key == "--done") {
            args.done = value;
        } else if (key == "--timeout-sec") {
            args.timeout = std::chrono::seconds(parse_positive_integer(value, 3600U));
        } else {
            throw std::runtime_error("unknown argument: " + std::string(key));
        }
    }
    if (args.static_id.empty() || args.dynamic_id.empty() || args.snapshot_id.empty()
        || args.ready.empty() || args.peer_ready.empty() || args.done.empty()
        || args.timeout.count() == 0) {
        throw std::runtime_error("interop requires all capability, path, and timeout arguments");
    }
    return args;
}

void validate_clock_and_stamp(const DynamicRecord& record,
                              const ClockIdentity& expected_clock,
                              std::int64_t expected_ns,
                              double expected_x) {
    if (record.parent.str() != "odom" || record.child.str() != "base_link"
        || record.stamp.clock != expected_clock || record.stamp.ns != expected_ns
        || record.transform_epoch != 3U || record.uncertainty_ns != std::optional<std::uint64_t>(0U)
        || !near(record.transform.translation().x(), expected_x)) {
        throw std::runtime_error("dynamic fixture wire fields differ");
    }
}

[[nodiscard]] bool rpc_server_is_idle(std::string_view service_name) {
    for (const auto& pressure : rbt::local_rpc_pressure_snapshots()) {
        if (pressure.service_name == service_name && pressure.server) {
            return pressure.server->requests_current == 0U;
        }
    }
    return false;
}

[[nodiscard]] int interop_publish(const InteropArgs& args) {
    const auto clock = fixture_clock();
    auto static_provider = rbt::tf::StaticTfProvider::open(args.static_id,
                                                            args.static_id,
                                                            args.snapshot_id,
                                                            64U);
    auto dynamic_publisher = rbt::tf::DynamicTfPublisher::open(args.dynamic_id);
    static_provider.replace(fixture_static(10U, clock));
    touch_file(args.ready);
    const auto deadline = std::chrono::steady_clock::now() + args.timeout;
    if (!wait_for_file(args.peer_ready, deadline)) {
        throw std::runtime_error("timed out waiting for consumer readiness");
    }

    static_provider.replace(fixture_static(11U, clock));
    std::uint64_t dynamic_samples = 0U;
    for (const auto& record : fixture_dynamics(clock)) {
        dynamic_publisher.publish(record);
        ++dynamic_samples;
    }

    std::size_t snapshot_responses = 0U;
    while (std::chrono::steady_clock::now() < deadline
           && !std::filesystem::exists(args.done)) {
        snapshot_responses += static_provider.serve_requests();
        std::this_thread::sleep_for(2ms);
    }
    if (!std::filesystem::exists(args.done)) {
        throw std::runtime_error("timed out waiting for consumer verification");
    }
    const auto current_revision = static_provider.current_revision();
    const auto static_pressure = static_provider.update_stream_pressure();
    const auto dynamic_pressure = dynamic_publisher.pressure();
    if (snapshot_responses == 0U || current_revision != std::optional<std::uint64_t>(11U)
        || dynamic_samples != 2U || !static_pressure.publisher || !dynamic_pressure.publisher
        || static_pressure.publisher->loans_in_use != 0U
        || static_pressure.publisher->send_successes < 2U
        || dynamic_pressure.publisher->loans_in_use != 0U
        || dynamic_pressure.publisher->send_successes != 2U
        || !rpc_server_is_idle(rbt::tf::transport_detail::endpoint_service(args.snapshot_id, "rpc"))) {
        throw std::runtime_error("publisher-side TF fixture or RPC/loan accounting differs");
    }
    std::cout << "{\"status\":\"PASS\",\"language\":\"cpp\","
                 "\"static_snapshot\":\"PASS\",\"static_update\":\"PASS\","
                 "\"dynamic\":\"PASS\",\"lookup\":\"PASS\","
                 "\"static_revision\":11,\"dynamic_samples\":2,"
                 "\"borrowed_samples\":0,\"pending_rpc\":0}\n";
    return 0;
}

[[nodiscard]] bool has_static_revision(const rbt::tf::TfPressureSnapshot& pressure,
                                       std::string_view authority_id,
                                       std::uint64_t revision) {
    for (const auto& provider : pressure.providers) {
        if (provider.authority.physical() == authority_id
            && provider.static_revision == std::optional<std::uint64_t>(revision)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint64_t stored_samples_for(const rbt::tf::TfPressureSnapshot& pressure,
                                               std::string_view authority_id) {
    for (const auto& provider : pressure.providers) {
        if (provider.authority.physical() == authority_id) {
            return provider.stored_samples;
        }
    }
    return 0U;
}

[[nodiscard]] bool rpc_is_idle(std::string_view service_name) {
    for (const auto& pressure : rbt::local_rpc_pressure_snapshots()) {
        if (pressure.service_name == service_name && pressure.client) {
            return pressure.client->response_guards_current == 0U
                   && pressure.client->in_flight_current == 0U;
        }
    }
    return false;
}

[[nodiscard]] int interop_consume(const InteropArgs& args) {
    const auto clock = fixture_clock();
    TransformBuffer buffer;
    auto dynamic_subscriber = rbt::tf::DynamicTfSubscriber::open(args.dynamic_id,
                                                                  "dynamic_source",
                                                                  buffer);
    auto static_client = rbt::tf::StaticTfClient::open(args.static_id,
                                                       args.static_id,
                                                       args.snapshot_id,
                                                       "static_source",
                                                       buffer,
                                                       64U);
    // Both stream listeners are live before the publisher is allowed to send.
    const auto wait_deadline = std::chrono::steady_clock::now() + args.timeout;
    if (!wait_for_file(args.ready, wait_deadline)) {
        throw std::runtime_error("timed out waiting for publisher readiness");
    }
    touch_file(args.peer_ready);

    const auto deadline = std::chrono::steady_clock::now() + args.timeout;
    std::size_t dynamic_samples = 0U;
    std::size_t static_updates = 0U;
    while (static_updates == 0U && std::chrono::steady_clock::now() < deadline) {
        static_updates += static_client.drain_updates();
        if (static_updates == 0U) {
            std::this_thread::sleep_for(2ms);
        }
    }
    if (static_updates == 0U) {
        throw std::runtime_error("timed out waiting for a static update before snapshot refresh");
    }
    static_client.refresh_snapshot(args.timeout);
    while (std::chrono::steady_clock::now() < deadline) {
        dynamic_samples += dynamic_subscriber.drain();
        static_updates += static_client.drain_updates();
        const auto pressure = buffer.pressure_snapshot();
        if (dynamic_samples >= 2U && static_updates >= 1U
            && has_static_revision(pressure, args.static_id, 11U)) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    if (dynamic_samples < 2U || static_updates < 1U
        || !has_static_revision(buffer.pressure_snapshot(), args.static_id, 11U)) {
        throw std::runtime_error("timed out waiting for complete TF fixture");
    }

    const auto dynamics = fixture_dynamics(clock);
    validate_clock_and_stamp(dynamics[0], clock, 100'000'000, 0.0);
    validate_clock_and_stamp(dynamics[1], clock, 200'000'000, 2.0);
    const Timestamp query { clock, 150'000'000 };
    LookupOptions lookup_options {};
    lookup_options.timestamp_uncertainty_ns = 0U;
    const auto result = buffer.lookup_at("odom", "camera_optical", query, lookup_options);
    const Vec3 expected(1.0 + std::sqrt(0.5), std::sqrt(0.5), 0.0);
    const auto mapped = result.apply_point(Vec3::zero());
    if (!near(mapped, expected)) {
        throw std::runtime_error("TF lookup result differs from expected direction and interpolation");
    }
    const auto inverse = buffer.lookup_at("camera_optical", "odom", query, lookup_options);
    if (!near(inverse.apply_point(mapped), Vec3::zero())) {
        throw std::runtime_error("inverse TF lookup did not round-trip");
    }
    bool saw_dynamic = false;
    bool saw_static = false;
    for (const auto& source : result.sources) {
        if (source.authority.physical() == args.dynamic_id) {
            saw_dynamic = source.transform_epoch == std::optional<std::uint64_t>(3U)
                          && source.bracket_start.ns == 100'000'000
                          && source.bracket_end.ns == 200'000'000
                          && source.bracket_start.clock == clock
                          && source.bracket_end.clock == clock;
        }
        if (source.authority.physical() == args.static_id) {
            saw_static = source.static_revision == std::optional<std::uint64_t>(11U);
        }
    }
    if (!saw_dynamic || !saw_static
        || stored_samples_for(buffer.pressure_snapshot(), args.dynamic_id) != 2U) {
        throw std::runtime_error("TF source metadata differs from fixture");
    }
    const auto dynamic_pressure = dynamic_subscriber.pressure();
    const auto static_pressure = static_client.update_stream_pressure();
    if (!dynamic_pressure.subscriber || dynamic_pressure.subscriber->borrowed_samples != 0U
        || !static_pressure.subscriber || static_pressure.subscriber->borrowed_samples != 0U
        || !rpc_is_idle(rbt::tf::transport_detail::endpoint_service(args.snapshot_id, "rpc"))) {
        throw std::runtime_error("native TF sample or RPC guards remain borrowed");
    }

    touch_file(args.done);
    std::cout << "{\"status\":\"PASS\",\"language\":\"cpp\","
                 "\"static_snapshot\":\"PASS\",\"static_update\":\"PASS\","
                 "\"dynamic\":\"PASS\",\"lookup\":\"PASS\","
                 "\"static_revision\":11,\"dynamic_samples\":2,"
                 "\"borrowed_samples\":0,\"pending_rpc\":0}\n";
    return 0;
}

[[nodiscard]] std::string time_error_name(TfErrorCode code) {
    switch (code) {
    case TfErrorCode::ClockMappingUnavailable:
        return "clock_mapping_unavailable";
    case TfErrorCode::ClockMappingExpired:
        return "clock_mapping_expired";
    default:
        return rbt::tf::to_string(code);
    }
}

[[nodiscard]] ClockMapping make_fixture_mapping(const ClockIdentity& device,
                                               const ClockIdentity& host,
                                               std::int64_t expires_ns) {
    ClockMappingCalibration calibration {};
    calibration.source = device;
    calibration.target = host;
    calibration.source_anchor_ns = 100'000'000;
    calibration.target_anchor_ns = 100'000'000;
    calibration.rate_numerator = 1U;
    calibration.rate_denominator = 1U;
    calibration.valid_from_ns = 0;
    calibration.valid_until_ns = 300'000'000;
    calibration.calibration_id[0] = 1U;
    calibration.calibrated_at = Timestamp { host, 1 };
    calibration.expires_at = Timestamp { host, expires_ns };
    calibration.uncertainty_ns = 0U;
    auto mapping = ClockMapping::create(std::move(calibration));
    if (!mapping) {
        throw std::runtime_error("cannot create fixture clock mapping");
    }
    return std::move(mapping).value();
}

[[nodiscard]] Vec3 mapped_fixture_point() {
    const auto device = fixture_clock(ClockKind::Device, 42U, 7U);
    const auto host = fixture_clock(ClockKind::HostMonotonic, 85U, 0U);
    TransformBuffer buffer;
    const auto authority = buffer.bind_provider("fixture", "00000000-0000-0000-0000-000000000042");
    buffer.apply_static_snapshot(authority, fixture_static(10U, host));
    const auto dynamics = fixture_dynamics(host);
    for (const auto& record : dynamics) {
        buffer.ingest_dynamic(authority, record);
    }
    const auto query = Timestamp { device, 150'000'000 };

    auto missing_options = LookupOptions {};
    missing_options.timestamp_uncertainty_ns = 0U;
    std::string missing_error = "UNEXPECTED_OK";
    try {
        (void)buffer.lookup_at("odom", "camera_optical", query, missing_options);
    } catch (const TfError& error) {
        missing_error = time_error_name(error.code());
    }
    if (missing_error != "clock_mapping_unavailable") {
        throw std::runtime_error("fixture did not produce the expected missing-mapping error");
    }

    auto expired_mapping = make_fixture_mapping(device, host, 20);
    std::array<const ClockMapping*, 1U> expired_mappings { &expired_mapping };
    const Timestamp expired_now { host, 20 };
    LookupOptions expired_options {};
    expired_options.timestamp_uncertainty_ns = 0U;
    expired_options.mappings = std::span<const ClockMapping* const>(expired_mappings);
    expired_options.now = &expired_now;
    std::string expired_error = "UNEXPECTED_OK";
    try {
        (void)buffer.lookup_at("odom", "camera_optical", query, expired_options);
    } catch (const TfError& error) {
        expired_error = time_error_name(error.code());
    }
    if (expired_error != "clock_mapping_expired") {
        throw std::runtime_error("fixture did not produce the expected expired-mapping error");
    }

    auto mapping = make_fixture_mapping(device, host, 1000);
    std::array<const ClockMapping*, 1U> mappings { &mapping };
    const Timestamp now { host, 10 };
    LookupOptions options {};
    options.timestamp_uncertainty_ns = 0U;
    options.mappings = std::span<const ClockMapping* const>(mappings);
    options.now = &now;
    const auto result = buffer.lookup_at("odom", "camera_optical", query, options);
    return result.apply_point(Vec3::zero());
}

[[nodiscard]] int semantics() {
    const auto direction = translated(1.0).apply_point(Vec3(0.0, 0.0, 1.0));
    const auto inverse = translated(1.0).inverse().apply_point(direction);
    const auto map_from_odom = translated(10.0);
    const auto odom_from_base = Transform3(Vec3(1.0, 0.0, 0.0),
                                           Quaternion(0.0, 0.0, std::sin(std::numbers::pi / 8.0),
                                                      std::cos(std::numbers::pi / 8.0)));
    const auto base_from_camera = translated(1.0);
    const auto chain = map_from_odom.compose(odom_from_base).compose(base_from_camera)
                          .apply_point(Vec3::zero());
    const auto interpolated = translated(0.0).interpolate(
        Transform3(Vec3(2.0, 0.0, 0.0),
                   Quaternion(0.0, 0.0, std::sin(std::numbers::pi / 4.0),
                              std::cos(std::numbers::pi / 4.0))),
        0.5);
    const auto mapped = mapped_fixture_point();
    const auto q = interpolated.rotation();
    const double expected_qz = std::sin(std::numbers::pi / 8.0);
    const double expected_qw = std::cos(std::numbers::pi / 8.0);
    if (!near(direction, Vec3(1.0, 0.0, 1.0)) || !near(inverse, Vec3(0.0, 0.0, 1.0))
        || !near(chain, Vec3(11.0 + std::sqrt(0.5), std::sqrt(0.5), 0.0))
        || !near(interpolated.translation(), Vec3(1.0, 0.0, 0.0))
        || !near(q.z(), expected_qz) || !near(q.w(), expected_qw)
        || !near(mapped, Vec3(1.0 + std::sqrt(0.5), std::sqrt(0.5), 0.0))) {
        throw std::runtime_error("TF semantic fixture math differs");
    }
    std::cout << std::setprecision(17)
              << "{\"language\":\"cpp\",\"direction_point\":[" << direction.x() << ',' << direction.y() << ',' << direction.z()
              << "],\"inverse_point\":[" << inverse.x() << ',' << inverse.y() << ',' << inverse.z()
              << "],\"chain_point\":[" << chain.x() << ',' << chain.y() << ',' << chain.z()
              << "],\"interpolated_translation\":[" << interpolated.translation().x() << ','
              << interpolated.translation().y() << ',' << interpolated.translation().z()
              << "],\"interpolated_quaternion_xyzw\":[" << q.x() << ',' << q.y() << ',' << q.z()
              << ',' << q.w() << "],\"wrong_clock_error\":\"clock_mapping_unavailable\","
                 "\"expired_mapping_error\":\"clock_mapping_expired\",\"mapped_point\":["
              << mapped.x() << ',' << mapped.y() << ',' << mapped.z() << "]}\n";
    return 0;
}

void usage(const char* program) {
    std::cerr << "usage: " << program
              << " semantics | interop <publish|consume> --static-id UUID --dynamic-id UUID"
                 " --snapshot-id UUID --ready PATH --peer-ready PATH --done PATH --timeout-sec N\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "semantics") {
            return semantics();
        }
        if (argc < 3 || std::string_view(argv[1]) != "interop") {
            usage(argv[0]);
            return 2;
        }
        const std::string_view role(argv[2]);
        const auto args = parse_interop_args(argc, argv);
        if (role == "publish") {
            return interop_publish(args);
        }
        if (role == "consume") {
            return interop_consume(args);
        }
        usage(argv[0]);
        return 2;
    } catch (const rbt::tf::TfTransportError& error) {
        std::cerr << "tf_probe transport error: " << error.what() << '\n';
    } catch (const TfError& error) {
        std::cerr << "tf_probe transform error: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "tf_probe error: " << error.what() << '\n';
    }
    return 1;
}
