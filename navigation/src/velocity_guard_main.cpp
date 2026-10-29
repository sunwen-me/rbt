#include <rbt_navigation/runtime/native.hpp>
#include <rbt_navigation/safety/velocity_guard.hpp>
#include <rbt/effect_scope.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace rbt::navigation;
namespace native=rbt::navigation::native;
namespace wire=rbt::navwire;

namespace {
TimePoint millisecondsToNanoseconds(double milliseconds,const char* name) {
    constexpr TimePoint nanoseconds_per_millisecond=1'000'000;
    const auto max_milliseconds=std::numeric_limits<TimePoint>::max()/nanoseconds_per_millisecond;
    if (milliseconds<=0 || milliseconds>static_cast<double>(max_milliseconds))
        throw std::invalid_argument(std::string(name)+" must be positive and fit in nanoseconds");
    const auto nanoseconds=static_cast<TimePoint>(milliseconds*nanoseconds_per_millisecond);
    if (nanoseconds<=0) throw std::invalid_argument(std::string(name)+" is below one nanosecond");
    return nanoseconds;
}
} // namespace

int main(int argc,char** argv) try {
    native::Options options(argc,argv);
    native::installSignals();
    const auto prefix=options.get("prefix","rbt/navigation/demo");
    const auto desired_topic=options.get("desired-topic",prefix+"/desired_cmd_vel");
    const auto cmd_vel_topic=options.get("cmd-vel-topic","/cmd_vel");
    VelocityGuardConfig config;
    config.allowed_source=options.integer("source-id",1);
    config.max_linear=options.number("max-linear",0.7);
    config.max_angular=options.number("max-angular",1.8);
    const auto guard_source=options.integer("guard-source-id",100);
    const auto estop_source=options.integer("estop-source",2);
    const double timeout_ms=options.number("guard-timeout-ms",250);
    const double command_ttl_ms=options.number("cmd-ttl-ms",100);
    const double hz=options.number("control-hz",50);
    const double duration=options.number("duration",0);
    if (desired_topic.empty() || cmd_vel_topic.empty() || desired_topic==cmd_vel_topic)
        throw std::invalid_argument("desired and final command topics must be nonempty and distinct");
    if (!config.allowed_source || !guard_source || !estop_source ||
        config.allowed_source==guard_source || config.allowed_source==estop_source || guard_source==estop_source)
        throw std::invalid_argument("raw, guard, and emergency-stop source IDs must be nonzero and distinct");
    if (hz<10 || hz>200 || duration<0 || duration>86400 || timeout_ms<1 || timeout_ms>3'600'000 ||
        command_ttl_ms<1 || command_ttl_ms>250 || command_ttl_ms>timeout_ms)
        throw std::invalid_argument("invalid guard frequency, duration, command TTL, or watchdog timeout");
    config.watchdog_ns=millisecondsToNanoseconds(timeout_ms,"guard timeout");
    const auto command_ttl_ns=millisecondsToNanoseconds(command_ttl_ms,"command TTL");
    VelocityGuard guard(config);

    auto clock=native::hostClock();
    native::FixedInput<wire::VelocityCommand> commands(desired_topic);
    native::FixedInput<wire::EmergencyStop> emergency(prefix,"estop");
    auto output=native::publisherAt<wire::VelocityCommand>(cmd_vel_topic);
    std::optional<rbt::FixedPublisher<wire::GuardState>> statuses;
    try {
        statuses.emplace(native::publisher<wire::GuardState>(prefix,"guard_state"));
    } catch (const std::runtime_error& error) {
        std::cerr<<"velocity guard: status publisher unavailable: "<<error.what()<<'\n';
    }

    const auto started=native::now(clock).ns;
    auto previous=started;
    std::uint64_t sequence=static_cast<std::uint64_t>(started),rejected=0,estop_sequence=0;
    auto next_sequence=[&]() {
        if (sequence==std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("velocity guard output sequence exhausted");
        return ++sequence;
    };
    auto boundedDeadline=[&](TimePoint now) {
        if (now<0 || now>std::numeric_limits<TimePoint>::max()-command_ttl_ns)
            throw std::overflow_error("velocity guard output deadline overflow");
        return now+command_ttl_ns;
    };

    // EffectScope runs this before the clock and publisher leave scope, on both
    // normal shutdown and exception unwinding.
    rbt::EffectScope cleanup;
    cleanup.defer_sync([&]() noexcept {
        try {
            const auto current=native::now(clock);
            wire::VelocityCommand stop{};
            stop.stamp=native::encodeStamp(current);
            stop.source_id=guard_source;
            stop.sequence=next_sequence();
            stop.valid_until_ns=boundedDeadline(current.ns);
            native::publish(output,stop);
        } catch (const std::exception& error) {
            std::cerr<<"velocity guard: final zero publication failed: "<<error.what()<<'\n';
        } catch (...) {
            std::cerr<<"velocity guard: final zero publication failed\n";
        }
    });

    std::cout<<"velocity guard ready: desired_topic="<<desired_topic
             <<" cmd_vel_topic="<<cmd_vel_topic
             <<" source_id="<<config.allowed_source
             <<" guard_source_id="<<guard_source<<std::endl;
    while (!native::interrupted) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::duration<double>(1/hz);
        const auto cycle_started=native::now(clock);
        if (duration>0 && cycle_started.ns-started>=duration*second) break;
        emergency.poll([&](const wire::EmergencyStop& message) {
            try {
                const auto current=native::now(clock);
                const auto stamp=native::decodeStamp(message.stamp,current.clock);
                if (message.source_id!=estop_source || message.sequence<=estop_sequence ||
                    message.active>1 || message.reserved || !fresh(stamp,current.ns,config.watchdog_ns))
                    throw std::invalid_argument("invalid emergency stop command");
                estop_sequence=message.sequence;
                guard.setEmergencyStop(message.active!=0);
            } catch (const std::exception&) { ++rejected; }
        });
        commands.poll([&](const wire::VelocityCommand& message) {
            try {
                const auto current=native::now(clock);
                const auto stamp=native::decodeStamp(message.stamp,current.clock);
                if (guard.accept({message.source_id,message.sequence,stamp,message.valid_until_ns,
                    {message.linear_x,message.angular_z}},current.ns)!=CommandRejection::None) ++rejected;
            } catch (const std::exception&) { ++rejected; }
        });

        const auto current=native::now(clock);
        const double dt=static_cast<double>(current.ns-previous)/second;
        previous=current.ns;
        const auto velocity=guard.step(current.ns,std::max(dt,1e-6));
        const auto stamp=native::encodeStamp(current);
        const auto output_sequence=next_sequence();
        const auto ttl_deadline=boundedDeadline(current.ns);
        const bool moving=velocity.linear_x!=0.0 || velocity.angular_z!=0.0;
        const auto valid_until=moving?std::min(ttl_deadline,guard.commandValidUntil()):ttl_deadline;
        native::publish(output,wire::VelocityCommand{stamp,guard_source,output_sequence,valid_until,
            velocity.linear_x,velocity.angular_z});

        if (statuses) {
            try {
                native::publish(*statuses,wire::GuardState{stamp,output_sequence,config.allowed_source,
                    guard_source,rejected,guard.watchdogExpired(current.ns)?1U:0U,
                    guard.emergencyStop()?1U:0U});
            } catch (const std::runtime_error&) {}
        }
        std::this_thread::sleep_until(deadline);
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr<<"velocity guard: "<<error.what()<<'\n';
    return 1;
}
