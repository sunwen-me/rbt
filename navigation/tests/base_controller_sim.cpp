#include <rbt/effect_scope.hpp>
#include <rbt_navigation/runtime/native.hpp>
#include <rbt_navigation/safety/velocity_guard.hpp>
#include <rbt_navigation/simulation/simulated_base.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace rbt::navigation;
namespace native=rbt::navigation::native;
namespace wire=rbt::navwire;

int main(int argc,char** argv) try {
    native::Options options(argc,argv);
    native::installSignals();
    const auto prefix=options.get("prefix","rbt/navigation/demo");

    VelocityGuardConfig config;
    config.allowed_source=options.integer("guard-source-id",100);
    config.max_linear=options.number("max-linear",0.7);
    config.max_angular=options.number("max-angular",1.8);
    const double timeout_ms=options.number("base-timeout-ms",250);
    if (timeout_ms<1 || timeout_ms>3'600'000)
        throw std::invalid_argument("invalid base timeout");
    config.watchdog_ns=static_cast<TimePoint>(timeout_ms*1'000'000);
    const auto estop_source=options.integer("estop-source",2);
    const double hz=options.number("control-hz",50);
    const double duration=options.number("duration",0);
    if (!estop_source || estop_source==config.allowed_source || hz<10 || hz>200
        || duration<0 || duration>86400)
        throw std::invalid_argument("invalid base controller configuration");

    VelocityGuard safety(config);
    SimulatedBaseAdapter base({options.number("initial-x",1),options.number("initial-y",1),
                               options.number("initial-yaw",0)});
    rbt::EffectScope effects;
    effects.defer_sync([&] {
        safety.setEmergencyStop(true);
        base.apply({},0.02);
    });

    auto clock=native::hostClock();
    const auto command_topic=options.get("cmd-vel-topic","/cmd_vel");
    native::FixedInput<wire::VelocityCommand> commands(command_topic);
    native::FixedInput<wire::EmergencyStop> emergency(prefix,"estop");
    auto states=native::publisher<wire::BaseState>(prefix,"base_state");
    auto odometry=native::publisher<wire::Odom2D>(prefix,"odom");
    const auto started=native::now(clock).ns;
    auto previous=started;
    std::uint64_t sequence=0,rejected=0,estop_sequence=0;
    std::uint64_t odom_publish_drops=0,state_publish_drops=0;
    const auto no_publication_log=std::chrono::steady_clock::time_point::min();
    auto last_publication_log=no_publication_log;
    auto reportPublicationDrop=[&](const char* channel,std::uint64_t& channel_drops,
                                   const std::exception& error) {
        ++channel_drops;
        const auto wall_now=std::chrono::steady_clock::now();
        if (last_publication_log==no_publication_log ||
            wall_now-last_publication_log>=std::chrono::seconds(1)) {
            const auto total=odom_publish_drops+state_publish_drops;
            std::cerr<<"base controller simulator: dropped "<<channel<<" publication: "<<error.what()
                     <<"; publication_drops="<<total<<" (odom="<<odom_publish_drops
                     <<", base_state="<<state_publish_drops
                     <<")\n";
            last_publication_log=wall_now;
        }
    };
    std::cout<<"base controller SIMULATOR ready: "<<prefix
             <<" final_cmd_vel_topic="<<command_topic<<std::endl;
    while (!native::interrupted) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::duration<double>(1/hz);
        const auto cycle_started=native::now(clock);
        if (duration>0 && cycle_started.ns-started>=duration*second) break;
        emergency.poll([&](const wire::EmergencyStop& message) {
            try {
                const auto current=native::now(clock);
                const auto stamp=native::decodeStamp(message.stamp,current.clock);
                if (!message.sequence || message.source_id!=estop_source
                    || message.sequence<=estop_sequence || message.active>1 || message.reserved
                    || !fresh(stamp,current.ns,config.watchdog_ns))
                    throw std::invalid_argument("invalid emergency stop command");
                estop_sequence=message.sequence;
                safety.setEmergencyStop(message.active!=0);
            } catch (const std::exception&) { ++rejected; }
        });
        commands.poll([&](const wire::VelocityCommand& message) {
            try {
                const auto current=native::now(clock);
                const auto stamp=native::decodeStamp(message.stamp,current.clock);
                if (safety.accept({message.source_id,message.sequence,stamp,message.valid_until_ns,
                    {message.linear_x,message.angular_z}},current.ns)!=CommandRejection::None) ++rejected;
            } catch (const std::exception&) { ++rejected; }
        });
        const auto current=native::now(clock);
        const double dt=static_cast<double>(current.ns-previous)/second;
        previous=current.ns;
        const auto velocity=safety.step(current.ns,std::max(dt,1e-6));
        // Large scheduling gaps stop rather than integrating an unobserved interval.
        base.apply(velocity,std::clamp(dt,1e-6,0.2));
        const auto pose=base.pose();
        const auto stamp=native::encodeStamp(current);
        try {
            native::publish(odometry,wire::Odom2D{stamp,++sequence,velocity.linear_x,velocity.angular_z});
        } catch (const std::runtime_error& error) {
            reportPublicationDrop("odometry",odom_publish_drops,error);
        }
        try {
            native::publish(states,wire::BaseState{stamp,sequence,pose.x,pose.y,pose.yaw,
                velocity.linear_x,velocity.angular_z,rejected,safety.watchdogExpired(current.ns)?1U:0U,
                safety.emergencyStop()?1U:0U});
        } catch (const std::runtime_error& error) {
            reportPublicationDrop("base_state",state_publish_drops,error);
        }
        std::this_thread::sleep_until(deadline);
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr<<"base controller simulator: "<<e.what()<<'\n';
    return 1;
}
