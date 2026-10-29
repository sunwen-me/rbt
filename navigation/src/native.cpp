#include <rbt_navigation/runtime/native.hpp>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>

namespace rbt::navigation::native {
wire::Stamp encodeStamp(const rbt::time::Timestamp& stamp) {
    if (stamp.clock.kind()!=rbt::time::ClockKind::HostMonotonic || stamp.ns<0)
        throw std::invalid_argument("navigation requires HostMonotonic");
    wire::Stamp out{};
    std::copy(stamp.clock.source_id().bytes().begin(),stamp.clock.source_id().bytes().end(),out.source_id);
    out.generation=stamp.clock.generation();
    out.nanoseconds=stamp.ns;
    out.kind=1;
    return out;
}
TimePoint decodeStamp(const wire::Stamp& stamp,const rbt::time::ClockIdentity& expected) {
    if (expected.kind()!=rbt::time::ClockKind::HostMonotonic || stamp.kind!=1 || stamp.reserved!=0
        || stamp.generation!=expected.generation() || stamp.nanoseconds<0
        || !std::equal(std::begin(stamp.source_id),std::end(stamp.source_id),expected.source_id().bytes().begin()))
        throw std::invalid_argument("navigation timestamp clock identity mismatch");
    return stamp.nanoseconds;
}
RobotState lookupRobotState(rbt::tf::TransformBuffer& buffer,const wire::Odom2D& odom,
                           const rbt::time::Timestamp& current,TimePoint max_age,std::string* failure) {
    RobotState state;
    try {
        const auto stamp=decodeStamp(odom.stamp,current.clock);
        if (!fresh(stamp,current.ns,max_age) || !odom.sequence) {
            if (failure) *failure="stale/future odometry or zero sequence";
            return state;
        }
        state.velocity={odom.linear_x,odom.angular_z};
        if (!finite(state.velocity)) return RobotState{};
        const rbt::time::Timestamp query{current.clock,stamp};
        const auto result=buffer.lookup_at("map","base_link",query,{0U,{},&current});
        const auto t=result.transform.translation();
        const auto q=result.transform.rotation();
        // Reject gross nonplanar inputs instead of silently flattening a tilted robot.
        const double roll=std::atan2(2*(q.w()*q.x()+q.y()*q.z()),1-2*(q.x()*q.x()+q.y()*q.y()));
        const double pitch=std::asin(std::clamp(2*(q.w()*q.y()-q.z()*q.x()),-1.0,1.0));
        if (std::abs(roll)>0.2 || std::abs(pitch)>0.2) return RobotState{};
        state.pose={t.x(),t.y(),std::atan2(2*(q.w()*q.z()+q.x()*q.y()),1-2*(q.y()*q.y()+q.z()*q.z()))};
        state.tf_stamp_ns=stamp;
        state.odom_stamp_ns=stamp;
        state.localized=finite(state.pose);
    } catch (const std::exception& e) {
        if (failure) *failure=e.what();
        return RobotState{};
    }
    return state;
}

namespace {
const std::vector<std::string> keys={"prefix","source-id","estop-source","tf-map","tf-odom",
    "control-hz","map-hz","input-timeout-ms","map-timeout-ms","plan-timeout-ms",
    "max-linear","max-angular","robot-radius","duration","initial-x","initial-y","initial-yaw",
    "scenario","runtime-pid","guard-pid","output","controller","planning-clearance",
    "desired-topic","cmd-vel-topic","guard-source-id","guard-timeout-ms","base-timeout-ms",
    "cmd-ttl-ms","external-map","map-pid","planner","robot-geometry","lattice-file","allow-reverse"};
bool known(std::string_view key) { return std::find(keys.begin(),keys.end(),key)!=keys.end(); }
std::string trim(std::string value) {
    const auto first=value.find_first_not_of(" \t\r\n");
    if (first==std::string::npos) return {};
    return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}
}
void Options::readConfig(const std::string& file) {
    std::ifstream input(file);
    if (!input) throw std::invalid_argument("cannot read config: "+file);
    std::string line;
    std::size_t count=0;
    while (std::getline(input,line)) {
        if (++count>256 || line.size()>4096) throw std::invalid_argument("oversized navigation config");
        line=trim(line.substr(0,line.find('#')));
        if (line.empty()) continue;
        const auto equal=line.find('=');
        if (equal==std::string::npos) throw std::invalid_argument("expected key=value");
        const auto key=trim(line.substr(0,equal));
        const auto value=trim(line.substr(equal+1));
        if (!known(key) || value.empty() || values_.contains(key))
            throw std::invalid_argument("invalid/duplicate navigation config key: "+key);
        if (key=="robot-geometry" && value=="circle") {
            values_[key]=value;
        } else if (key=="robot-geometry" || key=="lattice-file") {
            const auto resource=std::filesystem::path(value);
            values_[key]=(resource.is_absolute() ? resource :
                std::filesystem::path(file).parent_path()/resource).lexically_normal().string();
        } else values_[key]=value;
    }
}
Options::Options(int argc,char** argv) {
    for (int i=1;i<argc;++i) {
        if (std::string_view(argv[i])=="--config") {
            if (++i>=argc) throw std::invalid_argument("missing config filename");
            readConfig(argv[i]);
        } else if (std::string_view(argv[i])=="--help") {
            throw std::invalid_argument("Options: --config FILE, --prefix SERVICE, --source-id N, --estop-source N, --tf-map UUID, --tf-odom UUID, --control-hz HZ, --map-hz HZ, --input-timeout-ms MS, --map-timeout-ms MS, --plan-timeout-ms MS, --robot-radius M, --planning-clearance M, --planner astar|smac-lattice, --robot-geometry YAML|circle, --lattice-file JSON, --allow-reverse 0|1, --controller rpp|rpp-dwpp|vector-pursuit|vector-pursuit-window, --max-linear M/S, --max-angular RAD/S, --duration SECONDS, --initial-x M, --initial-y M, --initial-yaw RAD");
        } else {
            const std::string key=argv[i];
            if (!key.starts_with("--") || !known(key.substr(2)) || ++i>=argc)
                throw std::invalid_argument("unknown option or missing value: "+key);
        }
    }
    for (int i=1;i<argc;++i) {
        const std::string key=argv[i];
        if (key=="--config") { ++i; continue; }
        values_[key.substr(2)]=argv[++i];
    }
}
std::string Options::get(std::string_view key,std::string fallback) const {
    const auto found=values_.find(key);
    return found==values_.end()?fallback:found->second;
}
double Options::number(std::string_view key,double fallback) const {
    const auto value=get(key);
    if (value.empty()) return fallback;
    std::size_t used=0;
    const double parsed=std::stod(value,&used);
    if (used!=value.size() || !std::isfinite(parsed)) throw std::invalid_argument("invalid number: "+std::string(key));
    return parsed;
}
std::uint64_t Options::integer(std::string_view key,std::uint64_t fallback) const {
    const auto value=get(key);
    if (value.empty()) return fallback;
    if (!std::all_of(value.begin(),value.end(),[](char c){return c>='0' && c<='9';}))
        throw std::invalid_argument("invalid unsigned integer: "+std::string(key));
    std::size_t used=0;
    const auto parsed=std::stoull(value,&used);
    if (used!=value.size()) throw std::invalid_argument("invalid integer");
    return parsed;
}

MapWorker::MapWorker(NavigationManager& manager,const std::string& prefix,
                     const rbt::time::HostClock& clock,double frequency,TimePoint max_age)
    : manager_(manager),clock_(clock),frequency_(frequency),max_age_(max_age),prefix_(prefix) {
    if (!std::isfinite(frequency) || frequency<1 || frequency>100 || max_age<=0)
        throw std::invalid_argument("invalid map worker frequency/age");
    thread_=std::jthread([this](std::stop_token stop){run(stop);});
}
MapWorker::~MapWorker() { thread_.request_stop(); }
void MapWorker::run(std::stop_token stop) {
    MapSnapshot cached;
    std::vector<std::int8_t> previous_cells;
    wire::MapHeader previous_header{};
    while (!stop.stop_requested()) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::duration<double>(1/frequency_);
        try {
            if (!subscriber_) {
                auto result=rbt::SliceSubscriber<wire::MapHeader,std::int8_t>::create(
                    stream(prefix_,"map",wire::OCCUPANCY_MAP_DESCRIPTOR),wire::OCCUPANCY_MAP_DESCRIPTOR);
                if (result) subscriber_.emplace(std::move(result).value());
                else if (result.error()!=rbt::StreamError::ReadinessOpen && result.error()!=rbt::StreamError::Transport)
                    throw std::runtime_error("map subscriber open: "+std::string(rbt::to_string(result.error())));
            }
            // Sensor stream replaces queued maps. Drain at most the native bound.
            for (std::size_t i=0;subscriber_ && i<rbt::MAX_DRAIN_PER_SOURCE;++i) {
                auto received=take(subscriber_->receive());
                if (!received) break;
                const auto& h=received->header();
                const auto current=now(clock_);
                const auto stamp=decodeStamp(h.stamp,current.clock);
                const auto cells=received->payload();
                const bool frame=std::memcmp(h.frame,"map\0",4)==0
                    && std::all_of(std::begin(h.frame)+4,std::end(h.frame),[](auto b){return b==0;});
                if (!frame || !fresh(stamp,current.ns,max_age_) || !h.version || !h.width || !h.height
                    || h.width>max_cells/h.height || cells.size()!=static_cast<std::size_t>(h.width)*h.height)
                    throw std::invalid_argument("invalid native occupancy map");
                if (h.version<cached.version || (h.version==cached.version && stamp<cached.stamp_ns)) continue;
                if (h.version==cached.version && cached.valid()) {
                    if (h.width!=previous_header.width || h.height!=previous_header.height
                        || h.resolution!=previous_header.resolution || h.origin_x!=previous_header.origin_x
                        || h.origin_y!=previous_header.origin_y || h.origin_yaw!=previous_header.origin_yaw
                        || !std::equal(cells.begin(),cells.end(),previous_cells.begin(),previous_cells.end()))
                        throw std::invalid_argument("map changed without version increment");
                    cached.stamp_ns=stamp;
                } else {
                    OccupancyGrid grid{h.width,h.height,h.resolution,{h.origin_x,h.origin_y,h.origin_yaw},
                                       std::vector<std::int8_t>(cells.begin(),cells.end()),50};
                    auto built=std::make_shared<const GridMap>(std::move(grid));
                    previous_cells.assign(cells.begin(),cells.end());
                    previous_header=h;
                    cached={std::move(built),h.version,stamp};
                }
                manager_.setMap(cached);
            }
        } catch (const std::exception& error) {
            manager_.setMap({});
            std::cerr<<"map input rejected: "<<error.what()<<'\n';
        }
        std::this_thread::sleep_until(deadline);
    }
}
} // namespace rbt::navigation::native
