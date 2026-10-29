#include <rbt_navigation/runtime/native.hpp>
#include <iostream>

using namespace rbt::navigation;
namespace native=rbt::navigation::native;
namespace wire=rbt::navwire;
void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename F> void rejects(F&& fn) {
    bool rejected=false;
    try { fn(); } catch (const std::exception&) { rejected=true; }
    require(rejected,"invalid native boundary input accepted");
}
void checkMapBoundary(const rbt::time::HostClock& clock) {
    const auto prefix="rbt/navigation/boundary-"+rbt::generate_uuid();
    NavigationManager manager(std::make_unique<AStarPlanner>(),std::make_unique<DiffDrivePurePursuit>());
    native::MapWorker worker(manager,prefix,clock,100,2*second);
    auto pub=native::take(rbt::SlicePublisher<wire::MapHeader,std::int8_t>::create(
        native::stream(prefix,"map",wire::OCCUPANCY_MAP_DESCRIPTOR),wire::OCCUPANCY_MAP_DESCRIPTOR,4));
    auto send=[&](int mode) {
        auto loan=native::take(pub.loan_slice(4));
        loan.header()={};
        auto& h=loan.header();
        h.stamp=native::encodeStamp(native::now(clock));
        h.version=1; h.width=2; h.height=2; h.resolution=1;
        std::copy_n("map",3,h.frame);
        std::fill(loan.payload().begin(),loan.payload().end(),0);
        if (mode==1) h.stamp.source_id[0]^=1;
        if (mode==2) loan.payload()[0]=100; // same version must preserve contents
        if (mode==3) h.width=3; // declared shape mismatches payload
        if (!loan.send()) throw std::runtime_error("boundary map send failed");
    };
    auto awaitVersion=[&](std::uint64_t expected,int mode) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        do {
            send(mode);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (manager.status().map_version==expected) return;
        } while (std::chrono::steady_clock::now()<deadline);
        throw std::runtime_error("native map boundary did not reach expected validity");
    };
    awaitVersion(1,0);
    awaitVersion(0,1);
    awaitVersion(1,0);
    awaitVersion(0,2);
    awaitVersion(1,0);
    awaitVersion(0,3);
}
int main() try {
    auto clock=native::hostClock();
    const auto stamp=native::now(clock);
    auto wire_stamp=native::encodeStamp(stamp);
    require(native::decodeStamp(wire_stamp,stamp.clock)==stamp.ns,"timestamp roundtrip failed");
    auto bad=wire_stamp;
    bad.source_id[0]^=1;
    rejects([&]{(void)native::decodeStamp(bad,stamp.clock);});
    bad=wire_stamp; ++bad.generation;
    rejects([&]{(void)native::decodeStamp(bad,stamp.clock);});
    bad=wire_stamp; bad.kind=2;
    rejects([&]{(void)native::decodeStamp(bad,stamp.clock);});
    bad=wire_stamp; bad.reserved=1;
    rejects([&]{(void)native::decodeStamp(bad,stamp.clock);});
    bad=wire_stamp; bad.nanoseconds=-1;
    rejects([&]{(void)native::decodeStamp(bad,stamp.clock);});
    rbt::tf::TransformBuffer buffer;
    const auto map=buffer.bind_provider("localization",rbt::generate_uuid());
    const auto odom=buffer.bind_provider("odometry",rbt::generate_uuid());
    const auto transform=[](double x,double y,double yaw) {
        return rbt::tf::Transform3(rbt::tf::Vec3(x,y,0),rbt::tf::Quaternion(0,0,std::sin(yaw/2),std::cos(yaw/2)));
    };
    buffer.ingest_dynamic(map,{rbt::tf::FrameId("map"),rbt::tf::FrameId("odom"),stamp,0U,1,
                              transform(2,3,std::numbers::pi/2)});
    buffer.ingest_dynamic(odom,{rbt::tf::FrameId("odom"),rbt::tf::FrameId("base_link"),stamp,0U,1,
                               transform(1,0,0)});
    wire::Odom2D sample{wire_stamp,1,.2,.1};
    const auto state=native::lookupRobotState(buffer,sample,stamp,250'000'000);
    require(state.localized,"valid TF chain rejected");
    require(distance(state.pose,{2,4,std::numbers::pi/2})<1e-9,"map-odom-base composition wrong");
    require(std::abs(state.pose.yaw-std::numbers::pi/2)<1e-9,"TF yaw wrong");
    require(state.velocity.linear_x==.2 && state.velocity.angular_z==.1,"body velocity boundary wrong");
    const rbt::time::Timestamp future{stamp.clock,stamp.ns+second};
    require(!native::lookupRobotState(buffer,sample,future,250'000'000).localized,"expired odometry accepted");
    sample.stamp.nanoseconds=stamp.ns+1;
    require(!native::lookupRobotState(buffer,sample,stamp,250'000'000).localized,"future odometry accepted");
    sample.stamp=bad;
    require(!native::lookupRobotState(buffer,sample,stamp,250'000'000).localized,"bad clock accepted");
    checkMapBoundary(clock);
    std::cout<<"native clock identity and existing TF composition boundary: PASS\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
}
