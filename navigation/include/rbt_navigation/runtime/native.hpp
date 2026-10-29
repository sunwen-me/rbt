#pragma once
// Only the adapter layer includes native RBT/iceoryx2 types.
#include <rbt/navwire.hpp>
#include <rbt/tf_transport.hpp>
#include <rbt_navigation/runtime/navigation_manager.hpp>
#include <rbt_navigation/safety/velocity_guard.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

namespace rbt::navigation::native {
namespace wire = ::rbt::navwire;
inline constexpr std::size_t max_cells = 1'000'000;
inline constexpr std::size_t max_path_points = 100'000;

template <typename T> T take(rbt::Result<T> result) {
    if (!result) throw std::runtime_error("RBT stream operation failed: " + std::string(rbt::to_string(result.error())));
    return std::move(result).value();
}
inline rbt::time::HostClock hostClock() {
    auto result = rbt::time::HostClock::create();
    if (!result) throw std::runtime_error("RBT HostClock unavailable");
    return std::move(result).value();
}
inline rbt::time::Timestamp now(const rbt::time::HostClock& clock) {
    auto value = clock.now_monotonic();
    if (!value) throw std::runtime_error("RBT monotonic read failed");
    return std::move(value).value();
}
wire::Stamp encodeStamp(const rbt::time::Timestamp& stamp);
TimePoint decodeStamp(const wire::Stamp& stamp,const rbt::time::ClockIdentity& expected);
RobotState lookupRobotState(rbt::tf::TransformBuffer& buffer,const wire::Odom2D& odom,
                           const rbt::time::Timestamp& current,TimePoint max_age,
                           std::string* failure = nullptr);

inline rbt::StreamDescriptor stream(const std::string& prefix,std::string_view suffix,
                                   const rbt::MessageTypeDescriptor& type,
                                   rbt::StreamProfile profile = rbt::StreamProfile::Sensor) {
    return {prefix + "/" + std::string(suffix),type,profile};
}
template <typename T>
rbt::FixedPublisher<T> publisherAt(const std::string& topic) {
    const auto& d = rbt::FixedMessageBinding<T>::descriptor();
    return take(rbt::FixedPublisher<T>::create({topic,d,rbt::StreamProfile::Sensor},d));
}
template <typename T>
rbt::FixedSubscriber<T> subscriberAt(const std::string& topic) {
    const auto& d = rbt::FixedMessageBinding<T>::descriptor();
    return take(rbt::FixedSubscriber<T>::create({topic,d,rbt::StreamProfile::Sensor},d));
}
template <typename T>
rbt::FixedPublisher<T> publisher(const std::string& prefix,std::string_view suffix) {
    return publisherAt<T>(prefix+"/"+std::string(suffix));
}
template <typename T>
rbt::FixedSubscriber<T> subscriber(const std::string& prefix,std::string_view suffix) {
    return subscriberAt<T>(prefix+"/"+std::string(suffix));
}
template <typename T> void publish(rbt::FixedPublisher<T>& pub,const T& value) {
    auto loan = take(pub.loan());
    loan.payload() = value;
    if (!loan.send()) throw std::runtime_error("RBT send failed");
}
template <typename T,typename F> void drain(rbt::FixedSubscriber<T>& sub,F&& fn) {
    for (std::size_t i=0;i<rbt::MAX_DRAIN_PER_SOURCE;++i) {
        auto sample = take(sub.receive());
        if (!sample) break;
        fn(sample->payload());
    }
}

// RBT subscribers open existing services. Retry connection without delaying
// publication/control when a provider has not started yet.
template <typename T> class FixedInput {
public:
    explicit FixedInput(std::string topic) : topic_(std::move(topic)) {}
    FixedInput(std::string prefix,std::string suffix) : FixedInput(prefix+"/"+suffix) {}
    template <typename F> void poll(F&& fn) {
        if (!endpoint_) {
            const auto& d=rbt::FixedMessageBinding<T>::descriptor();
            auto result=rbt::FixedSubscriber<T>::create({topic_,d,rbt::StreamProfile::Sensor},d);
            if (!result) {
                if (result.error()==rbt::StreamError::ReadinessOpen || result.error()==rbt::StreamError::Transport) return;
                throw std::runtime_error("native input open: "+std::string(rbt::to_string(result.error())));
            }
            endpoint_.emplace(std::move(result).value());
        }
        drain(*endpoint_,std::forward<F>(fn));
    }
private:
    std::string topic_;
    std::optional<rbt::FixedSubscriber<T>> endpoint_;
};

class Options {
public:
    Options(int argc,char** argv);
    [[nodiscard]] std::string get(std::string_view key,std::string fallback = {}) const;
    [[nodiscard]] double number(std::string_view key,double fallback) const;
    [[nodiscard]] std::uint64_t integer(std::string_view key,std::uint64_t fallback) const;
private:
    void readConfig(const std::string& file);
    std::map<std::string,std::string,std::less<>> values_;
};
inline volatile std::sig_atomic_t interrupted = 0;
inline void signalHandler(int) { interrupted = 1; }
inline void installSignals() {
    std::signal(SIGINT,signalHandler);
    std::signal(SIGTERM,signalHandler);
}

// Each instance owns its own map subscriber and worker, so ESDF reconstruction
// and native map copies never execute in the control loop.
class MapWorker {
public:
    MapWorker(NavigationManager& manager,const std::string& prefix,
              const rbt::time::HostClock& clock,double frequency,TimePoint max_age);
    ~MapWorker();
private:
    void run(std::stop_token stop);
    NavigationManager& manager_;
    const rbt::time::HostClock& clock_;
    double frequency_;
    TimePoint max_age_;
    std::string prefix_;
    std::optional<rbt::SliceSubscriber<wire::MapHeader,std::int8_t>> subscriber_;
    std::jthread thread_;
};
} // namespace rbt::navigation::native
