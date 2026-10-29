#ifdef NDEBUG
#undef NDEBUG
#endif

#include <rbt/tf_math.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

using rbt::tf::FrameId;
using rbt::tf::Pose3;
using rbt::tf::Quaternion;
using rbt::tf::TfError;
using rbt::tf::TfErrorCode;
using rbt::tf::Transform3;
using rbt::tf::Vec3;

constexpr double kTolerance = 1.0e-9;

void close(double actual, double expected, double tolerance = kTolerance) {
    assert(std::abs(actual - expected) <= tolerance);
}

void close_vec(Vec3 actual, Vec3 expected, double tolerance = kTolerance) {
    close(actual.x(), expected.x(), tolerance);
    close(actual.y(), expected.y(), tolerance);
    close(actual.z(), expected.z(), tolerance);
}

Quaternion quat_z(double degrees) {
    const auto half_angle = degrees * std::acos(-1.0) / 360.0;
    return Quaternion(0.0, 0.0, std::sin(half_angle), std::cos(half_angle));
}

template <typename Callable>
void expect_error(TfErrorCode code, Callable&& callable) {
    bool threw = false;
    try {
        std::forward<Callable>(callable)();
    } catch (const TfError& error) {
        threw = true;
        assert(error.code() == code);
        assert(std::string_view(error.what()) == rbt::tf::to_string(code));
    }
    assert(threw);
}

void test_frame_id_contract() {
    const FrameId frame("robot/base_link");
    assert(frame.str() == "robot/base_link");
    const auto encoded = frame.to_bytes();
    assert(encoded.size() == 64U);
    assert(encoded[15] == 0U);
    assert(FrameId::from_bytes(encoded) == frame);
    assert(FrameId("Robot/base_link") != frame);
    assert(FrameId("_9/a.b-c") == FrameId("_9/a.b-c"));

    const std::string full(63U, 'a');
    assert(FrameId(full).str() == full);

    for (const auto invalid : {
             std::string_view(""),
             std::string_view("/base"),
             std::string_view("base/"),
             std::string_view("base//link"),
             std::string_view("."),
             std::string_view(".."),
             std::string_view("base/."),
             std::string_view("base/.."),
             std::string_view("-base"),
             std::string_view(".base"),
             std::string_view("base link"),
             std::string_view("base\0link", 9U),
             std::string_view("base/\xC3\xA9"),
         }) {
        expect_error(TfErrorCode::InvalidFrame, [invalid] { (void)FrameId(invalid); });
    }
    expect_error(TfErrorCode::InvalidFrame, [&full] { (void)FrameId(full + "a"); });

    auto invalid = encoded;
    invalid[16] = static_cast<std::uint8_t>('x');
    expect_error(TfErrorCode::InvalidFrame, [invalid] { (void)FrameId::from_bytes(invalid); });

    invalid = {};
    invalid[0] = static_cast<std::uint8_t>('a');
    invalid[2] = static_cast<std::uint8_t>('b');
    expect_error(TfErrorCode::InvalidFrame, [invalid] { (void)FrameId::from_bytes(invalid); });

    invalid.fill(static_cast<std::uint8_t>('a'));
    expect_error(TfErrorCode::InvalidFrame, [invalid] { (void)FrameId::from_bytes(invalid); });
}

void test_construction_and_error_codes() {
    assert(std::string_view(rbt::tf::to_string(TfErrorCode::UnknownUncertainty)) == "UnknownUncertainty");
    assert(Vec3::zero() == Vec3(0.0, 0.0, 0.0));
    assert(Quaternion::identity().w() == 1.0);
    assert(Pose3::identity() == Pose3(Vec3::zero(), Quaternion::identity()));
    assert(Transform3::identity().translation() == Vec3::zero());

    expect_error(TfErrorCode::InvalidTransform, [] {
        (void)Vec3(std::numeric_limits<double>::infinity(), 0.0, 0.0);
    });
    expect_error(TfErrorCode::InvalidTransform, [] { (void)Vec3(0.0, std::nan(""), 0.0); });
    expect_error(TfErrorCode::InvalidQuaternion, [] { (void)Quaternion(0.0, 0.0, 0.0, 0.0); });
    expect_error(TfErrorCode::InvalidQuaternion, [] { (void)Quaternion(0.0, 0.0, 0.0, 2.0); });
    expect_error(TfErrorCode::InvalidQuaternion, [] {
        (void)Quaternion(0.0, 0.0, std::nan(""), 1.0);
    });
    expect_error(TfErrorCode::InvalidQuaternion, [] {
        (void)Quaternion(0.0, 0.0, 0.0, std::numeric_limits<double>::infinity());
    });

    const Quaternion corrected(0.0, 0.0, 0.0, 1.0 + 5.0e-7);
    close(corrected.x(), 0.0);
    close(corrected.w(), 1.0);
    expect_error(TfErrorCode::InvalidQuaternion, [] { (void)Quaternion(0.0, 0.0, 0.0, 1.0 + 2.0e-6); });
}

void test_direction_composition_inverse_and_application() {
    const Transform3 target_from_middle(Vec3(1.0, 0.0, 0.0), quat_z(90.0));
    const Transform3 middle_from_source(Vec3(1.0, 0.0, 0.0), Quaternion::identity());
    const auto target_from_source = target_from_middle.compose(middle_from_source);
    close_vec(target_from_source.translation(), Vec3(1.0, 1.0, 0.0));
    close_vec(target_from_source.apply_point(Vec3::zero()), Vec3(1.0, 1.0, 0.0));

    const auto composed = Transform3(Vec3(2.0, -3.0, 1.0), quat_z(37.0))
                              .compose(Transform3(Vec3(-1.0, 4.0, 2.0), quat_z(-12.0)));
    const auto identity = composed.compose(composed.inverse());
    close_vec(identity.translation(), Vec3::zero());
    close_vec(identity.apply_vector(Vec3(1.0, 2.0, 3.0)), Vec3(1.0, 2.0, 3.0));

    const Transform3 transform(Vec3(3.0, 4.0, 5.0), quat_z(90.0));
    close_vec(transform.apply_vector(Vec3(1.0, 0.0, 0.0)), Vec3(0.0, 1.0, 0.0));
    close_vec(transform.apply_point(Vec3(1.0, 0.0, 0.0)), Vec3(3.0, 5.0, 5.0));
    const auto transformed_pose = transform.apply_pose(Pose3(Vec3::zero(), Quaternion::identity()));
    close_vec(transformed_pose.position(), Vec3(3.0, 4.0, 5.0));
    close_vec(Transform3(Vec3::zero(), transformed_pose.orientation()).apply_vector(Vec3(1.0, 0.0, 0.0)),
              Vec3(0.0, 1.0, 0.0));
}

void test_slerp_cases() {
    const auto halfway = Transform3::identity().interpolate(
        Transform3(Vec3::zero(), quat_z(90.0)), 0.5);
    close_vec(halfway.apply_vector(Vec3(1.0, 0.0, 0.0)),
              Vec3(std::sqrt(0.5), std::sqrt(0.5), 0.0));

    const auto negated_identity = Quaternion(0.0, 0.0, 0.0, -1.0);
    const auto antipodal = Transform3(Vec3::zero(), Quaternion::identity())
                               .interpolate(Transform3(Vec3::zero(), negated_identity), 0.5);
    close_vec(antipodal.apply_vector(Vec3(1.0, 0.0, 0.0)), Vec3(1.0, 0.0, 0.0));

    const auto shortest = Transform3::identity().interpolate(
        Transform3(Vec3::zero(), quat_z(270.0)), 0.5);
    close_vec(shortest.apply_vector(Vec3(1.0, 0.0, 0.0)),
              Vec3(std::sqrt(0.5), -std::sqrt(0.5), 0.0));

    const auto near_identical = Transform3::identity().interpolate(
        Transform3(Vec3::zero(), quat_z(1.0e-7 * 180.0 / std::acos(-1.0))), 0.5);
    assert(std::isfinite(near_identical.rotation().w()));
    close(std::hypot(std::hypot(near_identical.rotation().x(), near_identical.rotation().y()),
                     std::hypot(near_identical.rotation().z(), near_identical.rotation().w())),
          1.0);

    // dot = cos(angle/2) = 0.9996: exercise the normalized-linear branch near its threshold.
    const auto branch_angle = 2.0 * std::acos(0.9996);
    const Quaternion branch_target(0.0,
                                   0.0,
                                   std::sin(branch_angle / 2.0),
                                   std::cos(branch_angle / 2.0));
    const auto branch_midpoint = Transform3::identity().interpolate(
        Transform3(Vec3::zero(), branch_target), 0.5);
    close(std::hypot(std::hypot(branch_midpoint.rotation().x(), branch_midpoint.rotation().y()),
                     std::hypot(branch_midpoint.rotation().z(), branch_midpoint.rotation().w())),
          1.0);
    close_vec(branch_midpoint.apply_vector(Vec3(1.0, 0.0, 0.0)),
              Vec3(std::cos(branch_angle / 2.0), std::sin(branch_angle / 2.0), 0.0));

    const auto near_half_turn = Transform3(Vec3::zero(), quat_z(180.0 - 1.0e-6))
                                    .interpolate(
                                        Transform3(Vec3::zero(), quat_z(180.0 + 1.0e-6)), 0.5);
    close_vec(near_half_turn.apply_vector(Vec3(1.0, 0.0, 0.0)), Vec3(-1.0, 0.0, 0.0));

    const auto first = Transform3(Vec3(1.0, 2.0, 3.0), quat_z(15.0));
    const auto second = Transform3(Vec3(4.0, 5.0, 6.0), quat_z(75.0));
    assert(first.interpolate(second, 0.0) == first);
    assert(first.interpolate(second, 1.0) == second);
    for (const auto amount : { -0.1, 1.1, std::nan(""), std::numeric_limits<double>::infinity() }) {
        expect_error(TfErrorCode::InvalidTransform, [amount, first] {
            (void)first.interpolate(first, amount);
        });
    }
}

void test_large_finite_values_and_overflow() {
    const auto largest = std::numeric_limits<double>::max();
    const Transform3 translated(Vec3(largest, 0.0, 0.0), Quaternion::identity());
    close_vec(translated.apply_point(Vec3::zero()), Vec3(largest, 0.0, 0.0));
    expect_error(TfErrorCode::InvalidTransform, [translated] { (void)translated.compose(translated); });

    const auto canceled = translated.compose(translated.inverse());
    close_vec(canceled.translation(), Vec3::zero());
    const auto midpoint = translated.interpolate(
        Transform3(Vec3(-largest, 0.0, 0.0), Quaternion::identity()), 0.5);
    close_vec(midpoint.translation(), Vec3::zero());

    const auto rotation = quat_z(45.0);
    const Transform3 rotate_large(Vec3::zero(), rotation);
    expect_error(TfErrorCode::InvalidTransform, [rotate_large, largest] {
        (void)rotate_large.apply_vector(Vec3(largest, largest, 0.0));
    });
}

} // namespace

int main() {
    test_frame_id_contract();
    test_construction_and_error_codes();
    test_direction_composition_inverse_and_application();
    test_slerp_cases();
    test_large_finite_values_and_overflow();
    return 0;
}
