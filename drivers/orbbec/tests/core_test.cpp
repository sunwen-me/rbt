#ifdef NDEBUG
#undef NDEBUG
#endif

#include <rbt_orbbec/driver.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace rbt_orbbec;

template <typename Callable>
void expect_invalid(Callable&& callable) {
    bool threw = false;
    try {
        callable();
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

Frame depth_frame(std::span<const std::uint8_t> payload) {
    return Frame { Kind::Depth, Format::Depth16, 2U, 2U, 4U, 0x0102030405060708ULL,
                   17U, 0.001, payload, {} };
}

void test_names_and_enabled_kinds() {
    assert(std::string_view(kind_name(Kind::LeftIr)) == "left_ir");
    assert(std::string_view(format_name(Format::Xyz32f)) == "xyz32f");
    assert(std::string_view(kind_name(static_cast<Kind>(99U))) == "unknown");
    assert(frame_id("camera", Kind::Color) == "camera/color_optical");
    assert(frame_id("camera", Kind::Depth) == "camera/depth_optical");
    assert(frame_id("camera", Kind::LeftIr) == "camera/left_ir_optical");
    assert(frame_id("camera", Kind::RightIr) == "camera/right_ir_optical");
    assert(frame_id("camera", Kind::Points) == frame_id("camera", Kind::Depth));
    assert(frame_id("camera", Kind::Accel) == "camera/accel");
    assert(frame_id("camera", Kind::Gyro) == "camera/gyro");
    expect_invalid([] { (void)frame_id("bad//prefix", Kind::Color); });
    assert(frame_id(std::string(49U, 'a'), Kind::Color).size() == 63U);
    assert(frame_id(std::string(49U, 'a'), Kind::Depth).size() == 63U);
    assert(frame_id(std::string(47U, 'a'), Kind::LeftIr).size() == 63U);
    assert(frame_id(std::string(46U, 'a'), Kind::RightIr).size() == 63U);
    expect_invalid([] { (void)frame_id(std::string(50U, 'a'), Kind::Color); });
    expect_invalid([] { (void)frame_id(std::string(48U, 'a'), Kind::LeftIr); });
    expect_invalid([] { (void)frame_id(std::string(47U, 'a'), Kind::RightIr); });
    assert(frame_id(std::string(57U, 'a'), Kind::Accel).size() == 63U);
    expect_invalid([] { (void)frame_id(std::string(58U, 'a'), Kind::Accel); });

    Options options;
    options.serial = "device-1";
    options.color = false;
    options.left_ir = true;
    options.points = true;
    options.imu = true;
    const auto kinds = enabled_kinds(options);
    assert((kinds == std::vector<Kind> { Kind::Depth, Kind::LeftIr, Kind::Points,
                                         Kind::Accel, Kind::Gyro }));
    validate_options(options);
}

void test_depth_filter_configuration() {
    const auto parsed = parse_depth_filter("Spatial:alpha=1.25,beta=-2.5e-1,gamma=+3");
    assert(parsed.name == "Spatial");
    assert(parsed.parameters.size() == 3U);
    assert(parsed.parameters[0].name == "alpha" && parsed.parameters[0].value == 1.25);
    assert(parsed.parameters[1].name == "beta" && parsed.parameters[1].value == -0.25);
    assert(parsed.parameters[2].name == "gamma" && parsed.parameters[2].value == 3.0);
    const auto no_parameters = parse_depth_filter("HoleFilling2");
    assert(no_parameters.name == "HoleFilling2" && no_parameters.parameters.empty());

    expect_invalid([] { (void)parse_depth_filter(""); });
    expect_invalid([] { (void)parse_depth_filter(":alpha=1"); });
    expect_invalid([] { (void)parse_depth_filter("9Filter:alpha=1"); });
    expect_invalid([] { (void)parse_depth_filter("Filter-Name:alpha=1"); });
    expect_invalid([] { (void)parse_depth_filter(std::string(64U, 'A')); });
    expect_invalid([] { (void)parse_depth_filter(std::string("Filter") + static_cast<char>(0xE9)); });
    expect_invalid([] { (void)parse_depth_filter("Filter:"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:=1"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha="); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha=1,"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha=1,,beta=2"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha=1,alpha=2"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:2alpha=1"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha=1=2"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha=nan"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha=inf"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha=1e9999"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha=1.5junk"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha=+-1"); });
    expect_invalid([] { (void)parse_depth_filter("Filter:alpha=++1"); });
    assert(parse_depth_filter("Filter:alpha=+1").parameters[0].value == 1.0);
    assert(parse_depth_filter("Filter:alpha=-1").parameters[0].value == -1.0);

    std::string too_many_parameters = "Filter";
    for (std::size_t index = 0U; index < 65U; ++index) {
        too_many_parameters.append(index == 0U ? ":" : ",");
        too_many_parameters.append("p").append(std::to_string(index)).append("=1");
    }
    expect_invalid([&] { (void)parse_depth_filter(too_many_parameters); });

    Options options;
    validate_processing_options(options);
    options.align_depth_to_color = true;
    validate_processing_options(options);
    options.color = false;
    expect_invalid([&] { validate_processing_options(options); });
    options.color = true;
    options.depth = false;
    expect_invalid([&] { validate_processing_options(options); });

    options = Options {};
    options.depth_filters.push_back(parse_depth_filter("Spatial:alpha=1"));
    validate_processing_options(options);
    options.depth = false;
    expect_invalid([&] { validate_processing_options(options); });

    options = Options {};
    options.reconnect_delay_ms = 100U;
    validate_processing_options(options);
    options.reconnect_delay_ms = 30000U;
    validate_processing_options(options);
    options.reconnect_delay_ms = 99U;
    expect_invalid([&] { validate_processing_options(options); });
    options.reconnect_delay_ms = 30001U;
    expect_invalid([&] { validate_processing_options(options); });

    options = Options {};
    for (std::size_t filter_index = 0U; filter_index < 16U; ++filter_index) {
        DepthFilterConfig filter;
        filter.name = "Filter" + std::to_string(filter_index);
        for (std::size_t parameter_index = 0U; parameter_index < 4U; ++parameter_index) {
            filter.parameters.push_back(FilterParameter { "p" + std::to_string(parameter_index), 1.0 });
        }
        options.depth_filters.push_back(std::move(filter));
    }
    validate_processing_options(options);
    options.depth_filters.back().parameters.push_back(FilterParameter { "extra", 2.0 });
    expect_invalid([&] { validate_processing_options(options); });
    options.depth_filters.back().parameters.pop_back();
    options.depth_filters.push_back(DepthFilterConfig { "Filter16", {} });
    expect_invalid([&] { validate_processing_options(options); });
    options.depth_filters.pop_back();
    options.depth_filters.back().name = options.depth_filters.front().name;
    expect_invalid([&] { validate_processing_options(options); });
}

void test_output_frame_id() {
    Options options;
    options.frame_prefix = "camera";
    assert(output_frame_id(options, Kind::Depth) == "camera/depth_optical");
    assert(output_frame_id(options, Kind::Points) == "camera/depth_optical");

    options.align_depth_to_color = true;
    assert(output_frame_id(options, Kind::Depth) == "camera/color_optical");
    assert(output_frame_id(options, Kind::Points) == "camera/color_optical");
    assert(output_frame_id(options, Kind::Color) == "camera/color_optical");
    assert(output_frame_id(options, Kind::LeftIr) == "camera/left_ir_optical");
    assert(output_frame_id(options, Kind::RightIr) == "camera/right_ir_optical");
    assert(output_frame_id(options, Kind::Accel) == "camera/accel");
    assert(output_frame_id(options, Kind::Gyro) == "camera/gyro");
    expect_invalid([&] { (void)output_frame_id(options, static_cast<Kind>(99U)); });
}

void test_options_validation() {
    Options options;
    expect_invalid([&] { validate_options(options); });

    options.serial = "device-1";
    validate_options(options);

    auto invalid_options = options;
    invalid_options.serial = std::string(63U, 's');
    validate_options(invalid_options);
    invalid_options.serial.push_back('s');
    expect_invalid([&] { validate_options(invalid_options); });
    invalid_options = options;
    invalid_options.serial = std::string("device\0one", 10U);
    expect_invalid([&] { validate_options(invalid_options); });

    invalid_options = options;
    invalid_options.width = 0U;
    expect_invalid([&] { validate_options(invalid_options); });
    invalid_options = options;
    invalid_options.height = 4097U;
    expect_invalid([&] { validate_options(invalid_options); });
    invalid_options = options;
    invalid_options.fps = 241U;
    expect_invalid([&] { validate_options(invalid_options); });
    invalid_options = options;
    invalid_options.frame_timeout_ms = 0U;
    expect_invalid([&] { validate_options(invalid_options); });
    invalid_options = options;
    invalid_options.color = false;
    invalid_options.depth = false;
    invalid_options.left_ir = false;
    invalid_options.right_ir = false;
    expect_invalid([&] { validate_options(invalid_options); });
    invalid_options = options;
    invalid_options.depth = false;
    invalid_options.points = true;
    expect_invalid([&] { validate_options(invalid_options); });
    invalid_options = options;
    invalid_options.width = 4096U;
    invalid_options.height = 4096U;
    invalid_options.points = true;
    expect_invalid([&] { validate_options(invalid_options); });
    invalid_options = options;
    invalid_options.frame_prefix = std::string(60U, 'a');
    expect_invalid([&] { validate_options(invalid_options); });
    invalid_options = options;
    invalid_options.reconnect_delay_ms = 99U;
    expect_invalid([&] { validate_options(invalid_options); });
}

void test_frame_validation() {
    std::array<std::uint8_t, 8> depth_payload {};
    validate_frame(depth_frame(depth_payload));

    const std::array<std::uint8_t, 8> padded_rgb {};
    const Frame rgb { Kind::Color, Format::Rgb8, 2U, 1U, 8U, 3U, 0U, 1.0,
                      padded_rgb, {} };
    validate_frame(rgb);

    const std::array<std::uint8_t, 24> points_payload {};
    const Frame points { Kind::Points, Format::Xyz32f, 2U, 1U, 24U, 0U, 0U, 1.0,
                         points_payload, {} };
    validate_frame(points);
    std::array<std::uint8_t, 24> invalid_xyz_payload {};
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(invalid_xyz_payload.data(), &nan, sizeof(nan));
    auto invalid_xyz = points;
    invalid_xyz.data = invalid_xyz_payload;
    validate_frame(invalid_xyz);

    const std::array<std::uint8_t, 12> imu_payload {};
    const Frame accel { Kind::Accel, Format::Accel32f, 1U, 1U, 12U, 0U, 0U, 1.0,
                        imu_payload, {} };
    validate_frame(accel);

    auto bad = depth_frame(depth_payload);
    bad.format = Format::Rgb8;
    expect_invalid([&] { validate_frame(bad); });
    bad = depth_frame(depth_payload);
    bad.stride = 3U;
    expect_invalid([&] { validate_frame(bad); });
    bad = depth_frame(depth_payload);
    bad.data = std::span<const std::uint8_t>(depth_payload).first(7U);
    expect_invalid([&] { validate_frame(bad); });
    bad = depth_frame(depth_payload);
    bad.unit_scale = 0.0;
    expect_invalid([&] { validate_frame(bad); });
    bad = depth_frame(depth_payload);
    bad.unit_scale = std::numeric_limits<double>::infinity();
    expect_invalid([&] { validate_frame(bad); });
    bad = depth_frame(depth_payload);
    bad.width = 4097U;
    expect_invalid([&] { validate_frame(bad); });

    auto bad_points = points;
    bad_points.stride = 23U;
    expect_invalid([&] { validate_frame(bad_points); });
    bad_points = points;
    bad_points.data = std::span<const std::uint8_t>(points_payload).first(23U);
    expect_invalid([&] { validate_frame(bad_points); });

    auto bad_imu = accel;
    bad_imu.height = 2U;
    expect_invalid([&] { validate_frame(bad_imu); });
    bad_imu = accel;
    bad_imu.data = std::span<const std::uint8_t>(imu_payload).first(8U);
    expect_invalid([&] { validate_frame(bad_imu); });
    std::array<std::uint8_t, 12> non_finite_imu {};
    const std::array<float, 3> nan_sample { 0.0F, std::numeric_limits<float>::quiet_NaN(), 1.0F };
    std::memcpy(non_finite_imu.data(), nan_sample.data(), non_finite_imu.size());
    bad_imu = accel;
    bad_imu.data = non_finite_imu;
    expect_invalid([&] { validate_frame(bad_imu); });
    const std::array<float, 3> infinite_sample { 0.0F, 1.0F,
                                                 std::numeric_limits<float>::infinity() };
    std::memcpy(non_finite_imu.data(), infinite_sample.data(), non_finite_imu.size());
    bad_imu.data = non_finite_imu;
    expect_invalid([&] { validate_frame(bad_imu); });
}

void test_wire_round_trip_and_tamper_checks() {
    std::array<std::uint8_t, 8> payload { 0x10U, 0x20U, 0x30U, 0x40U,
                                          0x50U, 0x60U, 0x70U, 0x80U };
    const auto frame = depth_frame(payload);
    std::vector<std::uint8_t> wire(kFrameHeaderBytes + payload.size(), 0xFFU);
    encode_frame(wire, frame, "camera/depth_optical");
    assert(std::string_view(reinterpret_cast<const char*>(wire.data()), 8U) == "RBTORB01");
    assert(wire[16] == 2U && wire[17] == 0U && wire[18] == 0U && wire[19] == 0U);
    assert(wire[32] == 0x08U && wire[33] == 0x07U && wire[34] == 0x06U
           && wire[35] == 0x05U && wire[38] == 0x02U && wire[39] == 0x01U);
    assert(wire[28] == 0U && wire[29] == 0U && wire[30] == 0U && wire[31] == 0U);

    const auto decoded = decode_frame(wire);
    assert(decoded.kind == frame.kind);
    assert(decoded.format == frame.format);
    assert(decoded.width == 2U && decoded.height == 2U && decoded.stride == 4U);
    assert(decoded.index == frame.index);
    assert(decoded.unit_scale == frame.unit_scale);
    assert(decoded.frame_id == "camera/depth_optical");
    assert(std::equal(decoded.data.begin(), decoded.data.end(), payload.begin(), payload.end()));

    expect_invalid([&] { encode_frame(std::span<std::uint8_t>(wire).first(wire.size() - 1U),
                                      frame, "camera/depth_optical"); });
    expect_invalid([&] { encode_frame(wire, frame, "camera//depth"); });

    auto corrupt = wire;
    corrupt[0] = static_cast<std::uint8_t>('X');
    expect_invalid([&] { (void)decode_frame(corrupt); });
    corrupt = wire;
    corrupt[28] = 1U;
    expect_invalid([&] { (void)decode_frame(corrupt); });
    corrupt = wire;
    corrupt[56] = 1U;
    expect_invalid([&] { (void)decode_frame(corrupt); });
    corrupt = wire;
    corrupt[8] = 99U;
    expect_invalid([&] { (void)decode_frame(corrupt); });
    corrupt = wire;
    corrupt[12] = 1U;
    expect_invalid([&] { (void)decode_frame(corrupt); });
    corrupt = wire;
    corrupt[48] = 0xFFU;
    expect_invalid([&] { (void)decode_frame(corrupt); });
    corrupt = wire;
    corrupt[64U + 21U] = static_cast<std::uint8_t>('x');
    expect_invalid([&] { (void)decode_frame(corrupt); });
    corrupt = wire;
    corrupt[40] = 0U;
    corrupt[41] = 0U;
    corrupt[42] = 0U;
    corrupt[43] = 0U;
    corrupt[44] = 0U;
    corrupt[45] = 0U;
    corrupt[46] = 0xF0U;
    corrupt[47] = 0x7FU;
    expect_invalid([&] { (void)decode_frame(corrupt); });
    corrupt = wire;
    corrupt.pop_back();
    expect_invalid([&] { (void)decode_frame(corrupt); });
}

void test_extrinsic_conversion() {
    Extrinsic extrinsic;
    extrinsic.parent = Kind::Depth;
    extrinsic.child = Kind::Color;
    extrinsic.rotation = { 0.0, -1.0, 0.0,
                           1.0,  0.0, 0.0,
                           0.0,  0.0, 1.0 };
    extrinsic.translation_m = { 0.25, -0.5, 1.0 };
    const auto transform = transform_from_extrinsic(extrinsic);
    assert(std::abs(transform.translation().x() - 0.25) < 1.0e-12);
    assert(std::abs(transform.translation().y() + 0.5) < 1.0e-12);
    const auto rotated = transform.apply_vector(rbt::tf::Vec3(1.0, 0.0, 0.0));
    assert(std::abs(rotated.x()) < 1.0e-12);
    assert(std::abs(rotated.y() - 1.0) < 1.0e-12);

    auto bad = extrinsic;
    bad.rotation = { -1.0, 0.0, 0.0,
                      0.0, 1.0, 0.0,
                      0.0, 0.0, 1.0 };
    expect_invalid([&] { (void)transform_from_extrinsic(bad); });
    bad = extrinsic;
    bad.rotation[0] = 2.0;
    expect_invalid([&] { (void)transform_from_extrinsic(bad); });
    bad = extrinsic;
    bad.rotation[4] = std::numeric_limits<double>::quiet_NaN();
    expect_invalid([&] { (void)transform_from_extrinsic(bad); });
    bad = extrinsic;
    bad.translation_m[2] = std::numeric_limits<double>::infinity();
    expect_invalid([&] { (void)transform_from_extrinsic(bad); });
    bad = extrinsic;
    bad.parent = bad.child;
    expect_invalid([&] { (void)transform_from_extrinsic(bad); });
}

void test_uuid_and_json_helpers() {
    const auto uuid = parse_uuid("00112233-4455-6677-8899-aabbccddeeff");
    assert((uuid == std::array<std::uint8_t, 16> { 0x00U, 0x11U, 0x22U, 0x33U,
                                                   0x44U, 0x55U, 0x66U, 0x77U,
                                                   0x88U, 0x99U, 0xAAU, 0xBBU,
                                                   0xCCU, 0xDDU, 0xEEU, 0xFFU }));
    expect_invalid([] { (void)parse_uuid("00112233-4455-6677-8899-AABBCCDDEEFF"); });
    expect_invalid([] { (void)parse_uuid("not-a-uuid"); });

    const std::string input("say \"hi\"\\\n\t\x01", 12U);
    assert(json_quote(input) == "\"say \\\"hi\\\"\\\\\\n\\t\\u0001\"");
}

} // namespace

int main() {
    test_names_and_enabled_kinds();
    test_depth_filter_configuration();
    test_output_frame_id();
    test_options_validation();
    test_frame_validation();
    test_wire_round_trip_and_tamper_checks();
    test_extrinsic_conversion();
    test_uuid_and_json_helpers();
    return 0;
}
