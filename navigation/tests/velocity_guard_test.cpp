#include <rbt_navigation/safety/velocity_guard.hpp>

#include <iostream>

using namespace rbt::navigation;

namespace {

VelocityCommand command(std::uint64_t source, std::uint64_t sequence,
                        TimePoint stamp, TimePoint until, Twist2D velocity) {
    return {source, sequence, stamp, until, velocity};
}

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    VelocityGuardConfig config;
    config.allowed_source = 7;
    VelocityGuard guard(config);
    if (guard.commandValidUntil() != -1) {
        return fail("new guard reported an active command deadline");
    }

    if (guard.accept(command(7, 1, 0, 100'000'000, {0.5, 0.4}), 0) !=
            CommandRejection::None ||
        guard.commandValidUntil() != 100'000'000) {
        return fail("guard did not retain the accepted input deadline");
    }

    if (guard.accept(command(8, 99, 10'000'000, 200'000'000, {}), 10'000'000) !=
            CommandRejection::WrongSource ||
        guard.commandValidUntil() != 100'000'000) {
        return fail("wrong source changed the active input deadline");
    }
    const Twist2D output = guard.step(20'000'000, 0.02);
    if (output.linear_x <= 0.0 || output.angular_z <= 0.0) {
        return fail("wrong source replaced the configured authority's command");
    }

    if (guard.accept(command(7, 1, 20'000'000, 300'000'000, {}), 20'000'000) !=
            CommandRejection::Replay ||
        guard.commandValidUntil() != 100'000'000) {
        return fail("replayed command refreshed the active input deadline");
    }
    if (guard.accept(command(7, 2, 20'000'000, 120'000'000, {0.2, 0.1}), 20'000'000) !=
            CommandRejection::None ||
        guard.commandValidUntil() != 120'000'000) {
        return fail("valid configured-authority refresh was rejected");
    }
    const Twist2D expired = guard.step(120'000'000, 0.1);
    if (expired.linear_x != 0.0 || expired.angular_z != 0.0 ||
        guard.commandValidUntil() != -1) {
        return fail("expired command did not clear its deadline and output");
    }

    VelocityGuard estopped(config);
    if (estopped.accept(command(7, 1, 0, 100'000'000, {0.5, 0.4}), 0) !=
            CommandRejection::None) {
        return fail("estop setup command was rejected");
    }
    estopped.setEmergencyStop(true);
    if (!estopped.emergencyStop() || estopped.commandValidUntil() != -1 ||
        estopped.accept(command(7, 2, 10'000'000, 110'000'000, {0.5, 0.4}), 10'000'000) !=
            CommandRejection::EmergencyStop) {
        return fail("emergency stop did not invalidate and reject active commands");
    }
    estopped.setEmergencyStop(false);
    if (estopped.commandValidUntil() != -1 ||
        estopped.accept(command(7, 2, 20'000'000, 120'000'000, {0.5, 0.4}), 20'000'000) !=
            CommandRejection::None) {
        return fail("estop release reused an invalidated deadline or rejected a fresh command");
    }

    VelocityGuard invalidated(config);
    if (invalidated.accept(command(7, 1, 0, 100'000'000, {0.5, 0.4}), 0) !=
            CommandRejection::None ||
        invalidated.step(-1, 0.02).linear_x != 0.0 ||
        invalidated.commandValidUntil() != -1) {
        return fail("invalid time did not invalidate the command deadline");
    }

    std::cout << "velocity guard authority, replay, deadline and estop: PASS\n";
    return 0;
}
