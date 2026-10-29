#include <rbt_navigation/runtime/native.hpp>
#include <rbt_navigation/geometry/footprint.hpp>
#include <rbt_navigation/planner/state_lattice.hpp>
#ifdef RBT_NAVIGATION_CONFIG_IO
#include <rbt_navigation/geometry/geometry_io.hpp>
#endif
#include <deque>
#include <filesystem>
#include <iostream>

using namespace rbt::navigation;
namespace native=rbt::navigation::native;
namespace wire=rbt::navwire;

#ifdef RBT_NAVIGATION_CONFIG_IO
namespace {
std::filesystem::path resolveDefaultGeometry() {
    std::error_code error;
    const auto executable=std::filesystem::read_symlink("/proc/self/exe",error);
    if (error) throw std::runtime_error("cannot locate runtime executable for default robot geometry: "+error.message());
    const auto executable_directory=executable.parent_path();
    const auto build_geometry=(executable_directory/"config"/"front-drive-geometry.yaml").lexically_normal();
    const auto install_geometry=(executable_directory/RBT_NAVIGATION_DEFAULT_GEOMETRY_INSTALL_RELATIVE).lexically_normal();
    const auto is_regular_file=[](const std::filesystem::path& path) {
        std::error_code file_error;
        return std::filesystem::is_regular_file(path,file_error) && !file_error;
    };
    if (is_regular_file(build_geometry)) return build_geometry;
    if (is_regular_file(install_geometry)) return install_geometry;
    throw std::runtime_error("default robot geometry not found (checked "+build_geometry.string()+
        " and "+install_geometry.string()+"); install config/front-drive-geometry.yaml or pass --robot-geometry YAML|circle");
}
}
#endif

int main(int argc,char** argv) try {
    native::Options options(argc,argv);
    native::installSignals();
    const auto prefix=options.get("prefix","rbt/navigation/demo");
    const auto map_id=options.get("tf-map");
    const auto odom_id=options.get("tf-odom");
    if (map_id.empty() || odom_id.empty() || map_id==odom_id)
        throw std::invalid_argument("provide distinct --tf-map and --tf-odom authority UUIDs");
    const auto source=options.integer("source-id",1);
    const double hz=options.number("control-hz",50);
    const double duration=options.number("duration",0);
    if (!source || hz<5 || hz>200 || duration<0 || duration>86400)
        throw std::invalid_argument("invalid source, control frequency or duration");
    const auto command_lifetime=static_cast<TimePoint>(std::clamp(2.0/hz,0.1,0.25)*second);
    auto clock=native::hostClock();
    RuntimeConfig runtime;
    runtime.robot_radius=options.number("robot-radius",0.18);
    const auto planner_mode=options.get("planner","astar");
    if (planner_mode!="astar" && planner_mode!="smac-lattice")
        throw std::invalid_argument("planner must be astar or smac-lattice");
    std::optional<RobotGeometry> geometry;
    const auto geometry_file=options.get("robot-geometry");
    const auto lattice_file=options.get("lattice-file");
    std::string geometry_source;
#ifdef RBT_NAVIGATION_CONFIG_IO
    if (geometry_file=="circle") {
        geometry_source="circle";
    } else {
        const auto geometry_path=geometry_file.empty() ? resolveDefaultGeometry()
            : std::filesystem::absolute(geometry_file).lexically_normal();
        geometry_source=geometry_path.string();
        geometry=loadRobotGeometry(geometry_source);
        // Odom2D carries forward speed and yaw rate only. This adapter supports
        // the physical chassis's longitudinal base_link offset from its axle.
        if (std::abs(geometry->drive_to_base.y)>1e-9
            || std::abs(normalizeAngle(geometry->drive_to_base.yaw))>1e-9)
            throw std::invalid_argument("navigation geometry requires longitudinal, aligned drive_to_base");
        runtime.footprint=std::make_shared<const FootprintCollisionChecker>(geometry->footprint);
        runtime.robot_radius=runtime.footprint->radius();
    }
#else
    if (geometry_file.empty())
        throw std::invalid_argument("default robot geometry requires RBT_NAVIGATION_MAP_IO; pass --robot-geometry circle");
    if (geometry_file!="circle" || !lattice_file.empty() || planner_mode=="smac-lattice")
        throw std::invalid_argument("geometry and lattice loading require RBT_NAVIGATION_MAP_IO; pass --robot-geometry circle");
    geometry_source="circle";
#endif
    const double planning_clearance=options.number("planning-clearance",geometry ? 0.0 : 0.10);
    if (planning_clearance<0) throw std::invalid_argument("planning clearance must be nonnegative");
    const auto milliseconds=[&](std::string_view key,double fallback) {
        const double ms=options.number(key,fallback);
        if (ms<1 || ms>3'600'000) throw std::invalid_argument("invalid timeout");
        return static_cast<TimePoint>(ms*1'000'000);
    };
    runtime.max_input_age_ns=milliseconds("input-timeout-ms",250);
    runtime.max_map_age_ns=milliseconds("map-timeout-ms",2000);
    runtime.planning_timeout_ns=milliseconds("plan-timeout-ms",500);
    ControllerConfig controller;
    controller.max_linear=options.number("max-linear",0.6);
    controller.max_angular=options.number("max-angular",1.5);
    controller.regulated_min_speed=std::min(controller.regulated_min_speed,controller.max_linear);
    controller.min_approach_speed=std::min(controller.min_approach_speed,controller.max_linear);
    controller.rotate_to_heading_speed=std::min(controller.rotate_to_heading_speed,controller.max_angular);
    const auto controller_mode=options.get("controller","rpp-dwpp");
    if (controller_mode!="rpp" && controller_mode!="rpp-dwpp"
        && controller_mode!="vector-pursuit" && controller_mode!="vector-pursuit-window")
        throw std::invalid_argument("controller must be rpp, rpp-dwpp, vector-pursuit or vector-pursuit-window");
    controller.use_dynamic_window=controller_mode=="rpp-dwpp" || controller_mode=="vector-pursuit-window";
    if (controller_mode=="vector-pursuit" || controller_mode=="vector-pursuit-window")
        controller.geometry=PursuitGeometry::VectorPursuit;
    const auto allow_reverse=options.integer("allow-reverse",0);
    if (allow_reverse>1) throw std::invalid_argument("allow-reverse must be 0 or 1");
    controller.allow_reverse=allow_reverse!=0;
    std::unique_ptr<IGlobalPlanner> planner;
#ifdef RBT_NAVIGATION_CONFIG_IO
    if (planner_mode=="smac-lattice") {
        if (!geometry || lattice_file.empty())
            throw std::invalid_argument("smac-lattice requires polygon geometry (default provided) and --lattice-file");
        StateLatticeConfig config;
        config.timeout=std::chrono::nanoseconds(runtime.planning_timeout_ns);
        config.planning_clearance=planning_clearance;
        config.allow_reverse_expansion=controller.allow_reverse;
        planner=std::make_unique<StateLatticePlanner>(loadLatticeControlSet(lattice_file),runtime.footprint,config);
        controller.use_feasible_path_handler=true;
    }
#endif
    if (!planner) planner=std::make_unique<AStarPlanner>(PlannerConfig{
        runtime.robot_radius+planning_clearance,std::chrono::nanoseconds(runtime.planning_timeout_ns)});
    NavigationManager manager(std::move(planner),
        std::make_unique<RegulatedPurePursuit>(controller),runtime);
    native::MapWorker maps(manager,prefix,clock,options.number("map-hz",10),runtime.max_map_age_ns);
    rbt::tf::TransformBuffer buffer;
    std::optional<rbt::tf::DynamicTfSubscriber> map_tf,odom_tf;
    native::FixedInput<wire::Odom2D> odom_sub(prefix,"odom");
    native::FixedInput<wire::GoalCommand> goal_sub(prefix,"goal");
    const auto desired_topic=options.get("desired-topic",prefix+"/desired_cmd_vel");
    auto commands=native::publisherAt<wire::VelocityCommand>(desired_topic);
    auto statuses=native::publisher<wire::NavigationStatus>(prefix,"status");
    auto paths=native::take(rbt::SlicePublisher<wire::PathHeader,wire::PathPoint>::create(
        native::stream(prefix,"path",wire::PLANNED_PATH_DESCRIPTOR),wire::PLANNED_PATH_DESCRIPTOR,
        native::max_path_points));
    std::deque<wire::Odom2D> odometry;
    std::uint64_t odom_sequence=0,sequence=0;
    const auto started=native::now(clock).ns;
    // A new process on the same identified host clock starts above the old
    // process's sequence range, while individual commands remain strictly ordered.
    sequence=static_cast<std::uint64_t>(started);
    auto previous=started;
    TimePoint path_published=-second;
    TimePoint tf_warning_ns=-second;
    NavigationStatus previous_status;
    std::cout<<"navigation runtime ready: "<<prefix<<" planner="<<planner_mode<<" controller="<<controller_mode
             <<" desired_topic="<<desired_topic
             <<" geometry="<<geometry_source<<" footprint_points="<<(geometry ? geometry->footprint.size() : 0U)
             <<" robot_radius="<<runtime.robot_radius<<" planning_clearance="<<planning_clearance<<std::endl;
    while (!native::interrupted) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::duration<double>(1/hz);
        const auto cycle_started=native::now(clock);
        if (duration>0 && cycle_started.ns-started>=duration*second) break;
        try {
            if (!map_tf) map_tf.emplace(rbt::tf::DynamicTfSubscriber::open(map_id,"navigation_localization",buffer));
            if (!odom_tf) odom_tf.emplace(rbt::tf::DynamicTfSubscriber::open(odom_id,"navigation_odometry",buffer));
            (void)map_tf->drain(); (void)odom_tf->drain();
        }
        catch (const std::exception& e) {
            odometry.clear();
            if (cycle_started.ns-tf_warning_ns>=second) {
                std::cerr<<"TF input unavailable: "<<e.what()<<'\n';
                tf_warning_ns=cycle_started.ns;
            }
        }
        odom_sub.poll([&](const wire::Odom2D& odom) {
            try {
                const auto current=native::now(clock);
                const auto stamp=native::decodeStamp(odom.stamp,current.clock);
                if (!fresh(stamp,current.ns,runtime.max_input_age_ns) || !finite(Twist2D{odom.linear_x,odom.angular_z}))
                    throw std::invalid_argument("stale/nonfinite odometry");
                if (odom.sequence<=odom_sequence) return;
                odom_sequence=odom.sequence;
                odometry.push_back(odom);
                if (odometry.size()>32) odometry.pop_front();
            } catch (const std::exception&) { odometry.clear(); }
        });
        RobotState state;
        std::string state_error="no odometry samples";
        const auto lookup_time=native::now(clock);
        for (auto i=odometry.rbegin();i!=odometry.rend();++i) {
            std::string candidate_error;
            state=native::lookupRobotState(buffer,*i,lookup_time,runtime.max_input_age_ns,&candidate_error);
            if (i==odometry.rbegin()) state_error=std::move(candidate_error);
            if (state.localized) break;
        }
        if (geometry && state.localized) state.pose=baseToDrive(state.pose,*geometry);
        goal_sub.poll([&](const wire::GoalCommand& goal) {
            try {
                const auto current=native::now(clock);
                const auto stamp=native::decodeStamp(goal.stamp,current.clock);
                if (!fresh(stamp,current.ns,second) || goal.reserved || goal.operation>1)
                    throw std::invalid_argument("invalid goal command");
                if (goal.operation==1) manager.cancel(goal.goal_id);
                else {
                    const Pose2D base_goal{goal.x,goal.y,goal.yaw};
                    const auto target=geometry ? baseToDrive(base_goal,*geometry) : base_goal;
                    if (!manager.setGoal({goal.goal_id,target,goal.timeout_ns},state,current.ns))
                        std::cerr<<"goal rejected: "<<goal.goal_id<<"; localization="<<state.localized
                                 <<"; "<<state_error<<'\n';
                }
            } catch (const std::exception& e) { std::cerr<<"goal input rejected: "<<e.what()<<'\n'; }
        });
        const auto current=native::now(clock);
        const double dt=static_cast<double>(current.ns-previous)/second;
        previous=current.ns;
        const auto velocity=manager.controlStep(state,current.ns,std::max(dt,1e-6));
        wire::VelocityCommand command{};
        command.stamp=native::encodeStamp(current);
        command.source_id=source;
        command.sequence=++sequence;
        command.valid_until_ns=current.ns+command_lifetime;
        command.linear_x=velocity.linear_x;
        command.angular_z=velocity.angular_z;
        native::publish(commands,command);
        const auto status=manager.status();
        native::publish(statuses,wire::NavigationStatus{native::encodeStamp(current),status.goal_id,
            status.map_version,static_cast<std::uint32_t>(status.state),static_cast<std::uint32_t>(status.error)});
        if (status.state!=previous_status.state || status.goal_id!=previous_status.goal_id
            || status.map_version!=previous_status.map_version || current.ns-path_published>=second) {
            if (status.state!=previous_status.state || status.goal_id!=previous_status.goal_id)
                std::cerr<<"navigation state="<<static_cast<unsigned>(status.state)
                         <<" error="<<static_cast<unsigned>(status.error)<<" goal="<<status.goal_id
                         <<" pose=("<<state.pose.x<<','<<state.pose.y<<','<<state.pose.yaw<<")\n";
            const auto path=manager.path();
            if (path.poses.size()>native::max_path_points) throw std::runtime_error("path exceeds native capacity");
            auto loan=native::take(paths.loan_slice(path.poses.size()));
            loan.header()={};
            loan.header().stamp=native::encodeStamp(current);
            loan.header().goal_id=path.goal_id;
            loan.header().map_version=path.map_version;
            std::copy_n("map",3,loan.header().frame);
            auto payload=loan.payload();
            for (std::size_t i=0;i<path.poses.size();++i) {
                const auto pose=geometry ? driveToBase(path.poses[i],*geometry) : path.poses[i];
                payload[i]={pose.x,pose.y,pose.yaw};
            }
            if (!loan.send()) throw std::runtime_error("path publish failed");
            path_published=current.ns;
            previous_status=status;
        }
        std::this_thread::sleep_until(deadline);
    }
    manager.cancel(0);
    const auto current=native::now(clock);
    wire::VelocityCommand stop{};
    stop.stamp=native::encodeStamp(current);
    stop.source_id=source;
    stop.sequence=++sequence;
    stop.valid_until_ns=current.ns+command_lifetime;
    native::publish(commands,stop);
    return 0;
} catch (const std::exception& e) {
    std::cerr<<"navigation runtime: "<<e.what()<<'\n';
    return 1;
}
