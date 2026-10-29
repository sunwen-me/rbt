#include <rbt_m3t/camera_motion.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& description) {
    if (!condition) {
        throw std::runtime_error("check failed: " + description);
    }
}

void expect_tf_error(rbt::tf::TfErrorCode code,
                     const std::function<void()>& action,
                     const std::string& description) {
    try {
        action();
    } catch (const rbt::tf::TfError& error) {
        if (error.code() != code) {
            throw std::runtime_error("unexpected TF error for " + description + ": " + error.what());
        }
        return;
    }
    throw std::runtime_error("expected TF error: " + description);
}

[[nodiscard]] rbt::time::ClockIdentity make_clock(rbt::time::ClockKind kind,
                                                  std::uint8_t source_byte,
                                                  std::uint64_t generation = 0U) {
    std::array<std::uint8_t, 48> bytes {};
    bytes[0] = source_byte;
    auto result = rbt::time::ClockIdentity::create(
        kind, rbt::time::ClockSourceId(bytes), generation);
    check(result.has_value(), "test clock identity is valid");
    return std::move(result).value();
}

[[nodiscard]] rbt::tf::Transform3 translation(double x, double y, double z) {
    return rbt::tf::Transform3(rbt::tf::Vec3(x, y, z), rbt::tf::Quaternion::identity());
}

[[nodiscard]] rbt::tf::Transform3 transform(const Eigen::Affine3d& pose) {
    const Eigen::Quaterniond rotation(pose.linear());
    return rbt::tf::Transform3(
        rbt::tf::Vec3(pose.translation().x(), pose.translation().y(), pose.translation().z()),
        rbt::tf::Quaternion(rotation.x(), rotation.y(), rotation.z(), rotation.w()));
}

[[nodiscard]] rbt_m3t::CameraMotionConfig camera_config(
    const std::string& time_basis = "received",
    bool allow_unknown_uncertainty = true,
    int maximum_sample_age_ms = 1000) {
    rbt_m3t::CameraMotionConfig config;
    config.enabled = true;
    config.world_frame = "world";
    config.pitch_frame = "camera/pitch";
    config.pitch_provider = "head-pitch";
    config.depth_frame = "camera/depth_optical";
    config.time_basis = time_basis;
    config.allow_unknown_uncertainty = allow_unknown_uncertainty;
    config.maximum_sample_age_ms = maximum_sample_age_ms;
    config.maximum_wait_ms = 0;
    config.mount_in_pitch.translation() = Eigen::Vector3f(0.0F, 2.0F, 0.0F);
    auto left = Eigen::Affine3f::Identity();
    left.translation().x() = 10.0F;
    auto right = Eigen::Affine3f::Identity();
    right.translation().x() = -10.0F;
    config.arm_bases = {
        rbt_m3t::CameraMotionArmBase { "so101_left", left },
        rbt_m3t::CameraMotionArmBase { "so101_right", right },
    };
    return config;
}

class Fixture {
  public:
    explicit Fixture(rbt_m3t::CameraMotionConfig config = camera_config(),
                     bool unknown_sdk_uncertainty = true,
                     rbt::tf::Transform3 sdk_transform = translation(0.0, 0.0, 3.0))
        : device_clock(make_clock(rbt::time::ClockKind::Device, 1U))
        , host_clock(make_clock(rbt::time::ClockKind::HostMonotonic, 2U))
        , motion(std::move(config)) {
        motion.bind_pitch("00000000-0000-4000-8000-000000000001");
        const auto sdk = motion.buffer().bind_provider(
            "camera-sdk-calibration", "00000000-0000-4000-8000-000000000002");
        rbt::tf::StaticSet snapshot;
        snapshot.revision = 1U;
        snapshot.effective_from = rbt::time::Timestamp { device_clock, 0 };
        snapshot.effective_uncertainty_ns = unknown_sdk_uncertainty
                                                ? std::nullopt
                                                : std::optional<std::uint64_t> { 0U };
        snapshot.transforms.push_back(rbt::tf::StaticEdge {
            rbt::tf::FrameId("camera/depth_optical"),
            rbt::tf::FrameId("camera/color_optical"),
            std::move(sdk_transform),
        });
        motion.buffer().apply_static_snapshot(sdk, std::move(snapshot));
    }

    [[nodiscard]] rbt::tf::DynamicRecord pitch(std::int64_t stamp_ns,
                                               double x,
                                               std::uint64_t epoch = 1U) const {
        return rbt::tf::DynamicRecord {
            rbt::tf::FrameId("world"),
            rbt::tf::FrameId("camera/pitch"),
            rbt::time::Timestamp { host_clock, stamp_ns },
            0U,
            epoch,
            translation(x, 0.0, 0.0),
        };
    }

    [[nodiscard]] rbt_m3t::RectifiedPair pair(std::int64_t received_ns) const {
        rbt_m3t::RectifiedPair result {};
        result.timing.capture = rbt::time::Timestamp { device_clock, 500 };
        result.timing.received = rbt::time::Timestamp { host_clock, received_ns };
        result.timing.capture_uncertainty_ns = 0U;
        result.timing.received_uncertainty_ns = 0U;
        result.color_capture = *result.timing.capture;
        result.frame_id = "camera/color_optical";
        return result;
    }

    rbt::time::ClockIdentity device_clock;
    rbt::time::ClockIdentity host_clock;
    rbt_m3t::CameraMotion motion;
};

void test_nearest_pitch_composes_native_clock_domains_and_seeds() {
    Fixture fixture;
    fixture.motion.ingest_pitch(fixture.pitch(1000, 1.0));
    fixture.motion.ingest_pitch(fixture.pitch(1200, 3.0));

    const auto pair = fixture.pair(1100);
    const auto selection = fixture.motion.select(pair);
    check(selection.pitch_sample_timestamp.ns == 1000
              && selection.pitch_sample_offset_ns == -100
              && selection.pitch_sample_offset_abs_ns == 100U,
          "nearest sample tie chooses older measurement and reports signed offset");
    check(selection.selection_policy == "nearest"
              && selection.lookup.requested_timestamp.ns == pair.timing.received.ns,
          "selection reports nearest policy and the image timestamp");
    check(std::abs(selection.camera_in_world.translation().x() - 1.0F) < 1.0e-6F
              && std::abs(selection.camera_in_world.translation().y() - 2.0F) < 1.0e-6F
              && std::abs(selection.camera_in_world.translation().z() - 3.0F) < 1.0e-6F,
          "pitch, configured mount, and device-clock SDK colour offset compose");
    check(selection.lookup.sources.size() == 3U
              && !selection.lookup.timing_uncertainty_ns.has_value(),
          "native source provenance is merged and SDK unknown uncertainty is preserved");
    check(selection.lookup.sources[0].timestamp.clock == fixture.host_clock
              && selection.lookup.sources.back().timestamp.clock == fixture.device_clock,
          "composed source timestamps retain their original clock domains");

    std::vector<rbt_m3t::ArmSeed> seeds(2U);
    seeds[0].name = "so101_left";
    seeds[0].fixed_base = true;
    seeds[0].joint_angles["5"] = 0.25;
    seeds[1].name = "so101_right";
    seeds[1].fixed_base = true;
    const auto converted = fixture.motion.camera_seeds(seeds, selection);
    check(converted.size() == 2U
              && std::abs(converted[0].base_in_camera.translation().x() - 9.0F) < 1.0e-6F
              && std::abs(converted[0].base_in_camera.translation().y() + 2.0F) < 1.0e-6F
              && std::abs(converted[0].base_in_camera.translation().z() + 3.0F) < 1.0e-6F,
          "configured world arm bases are converted into camera coordinates");
    check(seeds[0].base_in_camera.translation().isZero()
              && converted[0].joint_angles.at("5") == 0.25,
          "seed conversion returns copies and preserves joint angles");
}

void test_rotated_camera_chain_and_seed_inverse_round_trip() {
    auto config = camera_config();
    Eigen::Affine3d mount = Eigen::Affine3d::Identity();
    mount.linear() << 0.0, 0.0, 1.0,
                      1.0, 0.0, 0.0,
                      0.0, 1.0, 0.0;
    mount.translation() = Eigen::Vector3d(0.04657, -0.025, 0.0);
    config.mount_in_pitch = mount.cast<float>();

    Eigen::Affine3d sdk = Eigen::Affine3d::Identity();
    sdk.linear() = Eigen::AngleAxisd(0.31, Eigen::Vector3d::UnitX()).toRotationMatrix();
    sdk.translation() = Eigen::Vector3d(0.021, -0.014, 0.037);

    auto& left_base = config.arm_bases.front().base_in_world;
    left_base.linear() = Eigen::AngleAxisf(0.27F, Eigen::Vector3f::UnitZ()).toRotationMatrix();
    left_base.translation() = Eigen::Vector3f(0.31F, 0.42F, -0.18F);
    const Eigen::Affine3d configured_world_base = left_base.cast<double>();

    Fixture fixture(std::move(config), true, transform(sdk));
    const Eigen::Affine3d world_from_pitch = [&] {
        Eigen::Affine3d pose = Eigen::Affine3d::Identity();
        pose.linear() = Eigen::AngleAxisd(-0.42, Eigen::Vector3d::UnitY()).toRotationMatrix();
        pose.translation() = Eigen::Vector3d(0.13, -0.06, 0.22);
        return pose;
    }();
    fixture.motion.ingest_pitch(rbt::tf::DynamicRecord {
        rbt::tf::FrameId("world"),
        rbt::tf::FrameId("camera/pitch"),
        rbt::time::Timestamp { fixture.host_clock, 1000 },
        0U,
        1U,
        transform(world_from_pitch),
    });

    const auto selection = fixture.motion.select(fixture.pair(1000));
    const Eigen::Affine3d expected_camera = world_from_pitch * mount * sdk;
    check((selection.camera_in_world.cast<double>().matrix() - expected_camera.matrix())
                  .cwiseAbs()
                  .maxCoeff()
              < 1.0e-5,
          "rotated pitch, mount, and SDK extrinsics compose in the expected direction");

    std::vector<rbt_m3t::ArmSeed> seeds(2U);
    seeds[0].name = "so101_left";
    seeds[0].fixed_base = true;
    seeds[1].name = "so101_right";
    seeds[1].fixed_base = true;
    const auto converted = fixture.motion.camera_seeds(seeds, selection);
    const Eigen::Affine3d reconstructed_world_base =
        selection.camera_in_world.cast<double>() * converted[0].base_in_camera.cast<double>();
    check((reconstructed_world_base.matrix() - configured_world_base.matrix())
                  .cwiseAbs()
                  .maxCoeff()
              < 1.0e-5,
          "inverse camera seed conversion reconstructs the configured world base pose");
}

void test_nearest_selection_before_after_and_single_sample() {
    Fixture fixture;
    fixture.motion.ingest_pitch(fixture.pitch(1000, 1.0));
    fixture.motion.ingest_pitch(fixture.pitch(1200, 3.0));
    const auto before = fixture.motion.select(fixture.pair(900));
    check(before.pitch_sample_timestamp.ns == 1000
              && before.pitch_sample_offset_ns == 100,
          "nearest selection accepts a future sample before the first measurement");
    const auto after = fixture.motion.select(fixture.pair(1300));
    check(after.pitch_sample_timestamp.ns == 1200
              && after.pitch_sample_offset_ns == -100,
          "nearest selection accepts a past sample after the last measurement");

    Fixture single;
    single.motion.ingest_pitch(single.pitch(1000, 4.0));
    const auto selected = single.motion.select(single.pair(1100));
    check(selected.pitch_sample_timestamp.ns == 1000
              && selected.pitch_sample_offset_abs_ns == 100U,
          "nearest selection supports a single retained measurement");
}

void test_nearest_selection_rejects_wrong_clock_staleness_and_unknown_uncertainty() {
    Fixture wrong_clock(camera_config("capture", true));
    wrong_clock.motion.ingest_pitch(wrong_clock.pitch(1000, 1.0));
    expect_tf_error(rbt::tf::TfErrorCode::ClockDomainMismatch,
                    [&] { (void)wrong_clock.motion.select(wrong_clock.pair(1000)); },
                    "capture query does not map the host pitch clock");

    Fixture stale(camera_config("received", true, 10));
    stale.motion.ingest_pitch(stale.pitch(1000, 1.0));
    expect_tf_error(rbt::tf::TfErrorCode::InsufficientHistory,
                    [&] { (void)stale.motion.select(stale.pair(20'000'000)); },
                    "nearest pitch sample exceeds configured maximum age");

    Fixture unknown(camera_config("received", false));
    unknown.motion.ingest_pitch(unknown.pitch(1000, 1.0));
    bool rejected_unknown_source = false;
    try {
        (void)unknown.motion.select(unknown.pair(1000));
    } catch (const rbt::tf::TfError& error) {
        check(error.time_error() == rbt::time::TimeError::UnknownUncertainty,
              "unknown transform uncertainty is rejected unless opted in");
        rejected_unknown_source = true;
    }
    check(rejected_unknown_source, "unknown transform uncertainty must be rejected");

    Fixture unknown_image_time(camera_config("received", false), false);
    unknown_image_time.motion.ingest_pitch(unknown_image_time.pitch(1000, 1.0));
    auto pair = unknown_image_time.pair(1000);
    pair.timing.received_uncertainty_ns.reset();
    expect_tf_error(rbt::tf::TfErrorCode::UnknownUncertainty,
                    [&] { (void)unknown_image_time.motion.select(pair); },
                    "unknown image timestamp uncertainty is checked independently");
}

void test_pitch_provider_replacement_and_epoch_clear_sample_cache() {
    Fixture fixture;
    fixture.motion.ingest_pitch(fixture.pitch(1000, 1.0));
    fixture.motion.bind_pitch("00000000-0000-4000-8000-000000000003");
    expect_tf_error(rbt::tf::TfErrorCode::InsufficientHistory,
                    [&] { (void)fixture.motion.select(fixture.pair(1000)); },
                    "provider replacement clears retained pitch samples");
    fixture.motion.ingest_pitch(fixture.pitch(1000, 4.0));
    const auto replaced = fixture.motion.select(fixture.pair(1000));
    check(std::abs(replaced.camera_in_world.translation().x() - 4.0F) < 1.0e-6F,
          "new provider samples are selected after replacement");

    fixture.motion.ingest_pitch(fixture.pitch(1200, 7.0, 2U));
    const auto epoch = fixture.motion.select(fixture.pair(1200));
    check(epoch.pitch_sample_timestamp.ns == 1200
              && std::abs(epoch.camera_in_world.translation().x() - 7.0F) < 1.0e-6F,
          "accepted transform epoch clears old nearest candidates");
}

void test_rejected_native_pitch_sample_is_not_cached() {
    Fixture fixture;
    fixture.motion.ingest_pitch(fixture.pitch(1000, 1.0));
    fixture.motion.ingest_pitch(fixture.pitch(1200, 3.0));
    expect_tf_error(rbt::tf::TfErrorCode::OutOfOrderSample,
                    [&] { fixture.motion.ingest_pitch(fixture.pitch(1100, 9.0)); },
                    "native TF buffer rejects out-of-order pitch sample");
    const auto selection = fixture.motion.select(fixture.pair(1100));
    check(selection.pitch_sample_timestamp.ns == 1000
              && std::abs(selection.camera_in_world.translation().x() - 1.0F) < 1.0e-6F,
          "rejected native record does not enter the nearest-sample cache");
}

void test_clock_generation_change_rebases_the_local_mount_snapshot() {
    Fixture fixture;
    fixture.motion.ingest_pitch(fixture.pitch(1000, 1.0));
    (void)fixture.motion.select(fixture.pair(1000));

    const auto next_clock = make_clock(rbt::time::ClockKind::HostMonotonic, 2U, 1U);
    fixture.motion.ingest_pitch(rbt::tf::DynamicRecord {
        rbt::tf::FrameId("world"),
        rbt::tf::FrameId("camera/pitch"),
        rbt::time::Timestamp { next_clock, 1200 },
        0U,
        1U,
        translation(6.0, 0.0, 0.0),
    });
    auto pair = fixture.pair(1200);
    pair.timing.received.clock = next_clock;
    const auto selection = fixture.motion.select(pair);
    check(selection.pitch_sample_timestamp.clock == next_clock
              && selection.pitch_sample_timestamp.ns == 1200
              && std::abs(selection.camera_in_world.translation().x() - 6.0F) < 1.0e-6F,
          "new accepted pitch clock generation gets a fresh local static mount authority");
}

void test_concurrent_pitch_ingest_and_selection() {
    Fixture fixture;
    constexpr std::int64_t first_stamp = 1'000'000;
    constexpr std::int64_t step_ns = 1'000'000;
    constexpr std::int64_t sample_count = 128;
    fixture.motion.ingest_pitch(fixture.pitch(first_stamp, 0.0));

    std::atomic<std::int64_t> newest_stamp { first_stamp };
    std::atomic<bool> writer_finished { false };
    std::exception_ptr writer_error;
    std::exception_ptr reader_error;
    std::size_t reader_selections = 0U;

    std::thread writer([&] {
        try {
            for (std::int64_t index = 1; index <= sample_count; ++index) {
                const auto stamp = first_stamp + index * step_ns;
                fixture.motion.ingest_pitch(fixture.pitch(stamp, static_cast<double>(index)));
                newest_stamp.store(stamp, std::memory_order_release);
                std::this_thread::yield();
            }
        } catch (...) {
            writer_error = std::current_exception();
        }
        writer_finished.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
        try {
            do {
                const auto stamp = newest_stamp.load(std::memory_order_acquire);
                const auto selection = fixture.motion.select(fixture.pair(stamp));
                check(selection.pitch_sample_timestamp.ns == stamp,
                      "concurrent selection finds the published pitch sample");
                ++reader_selections;
                std::this_thread::yield();
            } while (!writer_finished.load(std::memory_order_acquire)
                     || reader_selections < 32U);
        } catch (...) {
            reader_error = std::current_exception();
        }
    });
    writer.join();
    reader.join();

    if (writer_error) {
        std::rethrow_exception(writer_error);
    }
    if (reader_error) {
        std::rethrow_exception(reader_error);
    }
    check(reader_selections >= 32U,
          "concurrent ingest and select complete repeated timestamped queries");
}

} // namespace

int main() {
    try {
        test_nearest_pitch_composes_native_clock_domains_and_seeds();
        test_rotated_camera_chain_and_seed_inverse_round_trip();
        test_nearest_selection_before_after_and_single_sample();
        test_nearest_selection_rejects_wrong_clock_staleness_and_unknown_uncertainty();
        test_pitch_provider_replacement_and_epoch_clear_sample_cache();
        test_rejected_native_pitch_sample_is_not_cached();
        test_clock_generation_change_rebases_the_local_mount_snapshot();
        test_concurrent_pitch_ingest_and_selection();
    } catch (const std::exception& error) {
        std::cerr << "M3T camera motion test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "M3T camera motion tests passed\n";
    return 0;
}
