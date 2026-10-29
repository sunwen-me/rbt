#include <rbt_navigation/safety/safe_base.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace rbt::navigation;

namespace {

bool near(double a, double b, double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance;
}

VelocityCommand command(std::uint64_t sequence, TimePoint stamp, TimePoint until,
                        Twist2D velocity = {0.5, 0.4}, std::uint64_t source = 1) {
    return {source, sequence, stamp, until, velocity};
}

int fail(int code, const char* message) {
    std::cerr << message << '\n';
    return code;
}

} // namespace

int main() {
    constexpr TimePoint second_ns = second;

    try {
        SafeBaseConfig invalid;
        invalid.allowed_source = 0;
        SafeBase rejected(invalid);
        return fail(1, "zero source configuration was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        SafeBaseConfig invalid;
        invalid.max_linear = std::numeric_limits<double>::infinity();
        SafeBase rejected(invalid);
        return fail(1, "non-finite speed limit configuration was accepted");
    } catch (const std::invalid_argument&) {
    }

    SafeBase base;
    if (base.accept(command(1, 0, 250'000'000), 0) != CommandRejection::None) {
        return fail(2, "valid command was rejected");
    }
    const auto first = base.step(20'000'000, 0.02);
    if (!near(first.linear_x, 0.02) || !near(first.angular_z, 0.06)) {
        return fail(3, "configured acceleration limits were not applied");
    }

    if (base.accept(command(1, 20'000'000, second_ns), 20'000'000) != CommandRejection::Replay ||
        base.accept(command(2, 20'000'000, second_ns, {}, 2), 20'000'000) != CommandRejection::WrongSource) {
        return fail(4, "replay or wrong source was accepted");
    }
    auto bad = command(2, 20'000'000, second_ns,
                       {std::numeric_limits<double>::quiet_NaN(), 0.0});
    if (base.accept(bad, 20'000'000) != CommandRejection::NonFinite ||
        base.accept(command(2, 21'000'000, second_ns), 20'000'000) != CommandRejection::InvalidTime ||
        base.accept(command(2, 0, 20'000'000), 20'000'000) != CommandRejection::InvalidTime ||
        base.accept(command(2, 20'000'000, 10'000'000), 20'000'000) != CommandRejection::InvalidTime ||
        base.accept(command(2, 0, 300'000'001), 20'000'000) != CommandRejection::InvalidTime) {
        return fail(5, "invalid command data was accepted");
    }
    if (base.accept(command(2, 20'000'000, 100'000'000), 20'000'000) != CommandRejection::None) {
        return fail(6, "rejected command incorrectly advanced the sequence or watchdog");
    }
    const auto bounded = base.step(40'000'000, 0.02);
    if (!near(bounded.linear_x, 0.04) || !near(bounded.angular_z, 0.12)) {
        return fail(7, "rejected commands refreshed or disturbed safe output");
    }

    SafeBase clamped;
    if (clamped.accept(command(1, 0, 250'000'000, {20.0, -20.0}), 0) != CommandRejection::None) {
        return fail(8, "clamp command was rejected");
    }
    const auto maxed = clamped.step(200'000'000, 0.2);
    if (!near(maxed.linear_x, 0.2) || !near(maxed.angular_z, -0.6)) {
        return fail(9, "acceleration clamp did not bound the first step");
    }
    if (clamped.accept(command(2, 200'000'000, 450'000'000, {20.0, -20.0}),
                       200'000'000) != CommandRejection::None) {
        return fail(10, "speed-ramp refresh command was rejected");
    }
    const auto maxed_again = clamped.step(400'000'000, 0.2);
    if (!near(maxed_again.linear_x, 0.4) || !near(maxed_again.angular_z, -1.2)) {
        return fail(10, "acceleration-limited ramp is incorrect");
    }
    if (clamped.accept(command(3, 400'000'000, 650'000'000, {20.0, -20.0}),
                       400'000'000) != CommandRejection::None) {
        return fail(11, "second speed-ramp refresh command was rejected");
    }
    const auto max_speed = clamped.step(600'000'000, 0.2);
    if (!near(max_speed.linear_x, 0.6) || !near(max_speed.angular_z, -1.8)) {
        return fail(11, "speed clamp was not applied");
    }
    if (clamped.accept(command(4, 600'000'000, 850'000'000, {20.0, -20.0}),
                       600'000'000) != CommandRejection::None) {
        return fail(12, "third speed-ramp refresh command was rejected");
    }
    const auto max_speed_again = clamped.step(800'000'000, 0.2);
    if (!near(max_speed_again.linear_x, 0.7) || !near(max_speed_again.angular_z, -1.8)) {
        return fail(12, "configured speed limits were exceeded or not reached");
    }

    SafeBase paced;
    if (paced.accept(command(1, 0, 250'000'000), 0) != CommandRejection::None ||
        !near(paced.step(0, 0.2).linear_x, 0.0)) {
        return fail(13, "same-time first step accelerated the command");
    }
    const auto pace_first = paced.step(10'000'000, 0.2);
    const auto pace_second = paced.step(20'000'000, 0.2);
    if (!near(pace_first.linear_x, 0.01) || !near(pace_first.angular_z, 0.03) ||
        !near(pace_second.linear_x, 0.02) || !near(pace_second.angular_z, 0.06)) {
        return fail(14, "acceleration exceeded elapsed monotonic time");
    }

    SafeBase watch;
    if (watch.accept(command(1, 0, 250'000'000), 0) != CommandRejection::None) {
        return fail(13, "watchdog command was rejected");
    }
    if (!near(watch.step(100'000'000, 0.02).linear_x, 0.02) ||
        watch.accept(command(2, 200'000'000, 250'000'000, {}, 2), 200'000'000) !=
            CommandRejection::WrongSource ||
        !watch.watchdogExpired(250'000'000) ||
        watch.step(250'000'000, 0.02).linear_x != 0.0) {
        return fail(14, "watchdog did not stop independently at its deadline");
    }

    SafeBase cancel;
    if (cancel.accept(command(1, 0, 250'000'000, {0.4, 0.8}), 0) != CommandRejection::None) {
        return fail(15, "cancel setup command was rejected");
    }
    const auto moving = cancel.step(100'000'000, 0.1);
    if (!near(moving.linear_x, 0.1) || !near(moving.angular_z, 0.3)) {
        return fail(16, "cancel setup motion is incorrect");
    }
    if (cancel.accept(command(2, 100'000'000, 300'000'000, {}), 100'000'000) != CommandRejection::None) {
        return fail(17, "explicit zero command was rejected");
    }
    const auto slowing = cancel.step(150'000'000, 0.05);
    if (!near(slowing.linear_x, 0.05) || !near(slowing.angular_z, 0.15)) {
        return fail(18, "explicit zero command did not use controlled deceleration");
    }
    const auto stopped = cancel.step(200'000'000, 0.05);
    if (!near(stopped.linear_x, 0.0) || !near(stopped.angular_z, 0.0)) {
        return fail(18, "controlled deceleration did not reach zero");
    }

    SafeBase estopped;
    if (estopped.accept(command(1, 0, 250'000'000), 0) != CommandRejection::None) {
        return fail(19, "estop setup command was rejected");
    }
    estopped.step(20'000'000, 0.02);
    estopped.setEmergencyStop(true);
    if (!estopped.emergencyStop() || estopped.step(40'000'000, 0.02).linear_x != 0.0 ||
        estopped.accept(command(2, 40'000'000, second_ns), 40'000'000) != CommandRejection::EmergencyStop) {
        return fail(20, "estop did not latch immediate zero and reject commands");
    }
    estopped.setEmergencyStop(false);
    if (estopped.step(60'000'000, 0.02).linear_x != 0.0 ||
        estopped.accept(command(1, 60'000'000, 250'000'000), 60'000'000) != CommandRejection::Replay ||
        estopped.accept(command(2, 60'000'000, 250'000'000), 60'000'000) != CommandRejection::None ||
        estopped.step(80'000'000, 0.02).linear_x <= 0.0) {
        return fail(21, "estop release reused a command or failed to accept a fresh one");
    }

    SafeBase regressed;
    if (regressed.accept(command(1, 0, 250'000'000), 0) != CommandRejection::None) {
        return fail(22, "time regression setup command was rejected");
    }
    regressed.step(100'000'000, 0.02);
    if (regressed.step(99'000'000, 0.02).linear_x != 0.0 ||
        regressed.accept(command(2, 99'000'000, 250'000'000), 99'000'000) != CommandRejection::None ||
        regressed.step(119'000'000, std::numeric_limits<double>::quiet_NaN()).linear_x != 0.0) {
        return fail(23, "time regression or invalid dt did not clear command and output");
    }

    SimulatedBaseAdapter straight;
    straight.apply({1.0, 0.0}, 0.2);
    if (!near(straight.pose().x, 0.2) || !near(straight.pose().y, 0.0) ||
        !near(straight.pose().yaw, 0.0) || !near(straight.velocity().linear_x, 1.0)) {
        return fail(24, "simulated straight-line kinematics are incorrect");
    }
    SimulatedBaseAdapter arc;
    arc.apply({1.0, 1.0}, 0.2);
    if (!near(arc.pose().x, std::sin(0.2)) || !near(arc.pose().y, 1.0 - std::cos(0.2)) ||
        !near(arc.pose().yaw, 0.2)) {
        return fail(25, "simulated exact arc integration is incorrect");
    }
    SimulatedBaseAdapter wrapped({0.0, 0.0, 3.0});
    wrapped.apply({0.0, 2.0}, 0.2);
    if (!near(wrapped.pose().yaw, normalizeAngle(3.4))) {
        return fail(26, "simulated yaw was not normalized");
    }
    try {
        straight.apply({std::numeric_limits<double>::infinity(), 0.0}, 0.1);
        return fail(27, "simulated base accepted non-finite velocity");
    } catch (const std::invalid_argument&) {
    }
    try {
        straight.apply({1.0, 0.0}, 0.0);
        return fail(28, "simulated base accepted zero dt");
    } catch (const std::invalid_argument&) {
    }

    std::cout << "safe base replay, timing, watchdog, estop, slew and simulation: PASS\n";
    return 0;
}
