#pragma once
#include <rbt/hardware.hpp>
#include <rbt/stream.hpp>
#include <rbt/time.hpp>
#include <rbt/wait.hpp>
#include <algorithm>
#include <array>
#include <csignal>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace rbt::hardware::native {
template<class T> T take(rbt::Result<T> result) {
    if (!result) throw std::runtime_error(std::string("RBT: ")+rbt::to_string(result.error()));
    return std::move(result).value();
}
inline rbt::time::HostClock hostClock() {
    auto result=rbt::time::HostClock::create();
    if (!result) throw std::runtime_error("HostClock unavailable");
    return std::move(result).value();
}
inline rbt::time::Timestamp now(const rbt::time::HostClock& clock) {
    auto result=clock.now_monotonic();
    if (!result) throw std::runtime_error("HostClock read failed");
    return std::move(result).value();
}
template<class S=::rbt::hardware::Stamp> S encodeStamp(const rbt::time::Timestamp& value) {
    if (value.clock.kind()!=rbt::time::ClockKind::HostMonotonic || value.ns<0)
        throw std::invalid_argument("hardware telemetry requires HostMonotonic");
    S result{};
    std::copy(value.clock.source_id().bytes().begin(),value.clock.source_id().bytes().end(),result.source_id);
    result.generation=value.clock.generation(); result.nanoseconds=value.ns;
    // Wire kind 1 is the existing navigation HostMonotonic encoding. The
    // SDK ClockKind C++ enum uses 0 for this domain; it is not the wire value.
    result.kind=1U;
    return result;
}
template<class S> std::int64_t decodeStamp(const S& value,const rbt::time::ClockIdentity& expected) {
    if (!expected.is_well_formed() || expected.kind()!=rbt::time::ClockKind::HostMonotonic
        || value.reserved || value.nanoseconds<0 || value.kind!=1U
        || value.generation!=expected.generation()
        || !std::equal(std::begin(value.source_id),std::end(value.source_id),expected.source_id().bytes().begin()))
        throw std::invalid_argument("clock identity/timestamp mismatch");
    return value.nanoseconds;
}
inline bool fresh(std::int64_t stamp,std::int64_t current,std::int64_t age) {
    return stamp>=0 && stamp<=current && current-stamp<=age;
}
template<class T> rbt::FixedPublisher<T> publisher(const std::string& topic) {
    const auto& d=rbt::FixedMessageBinding<T>::descriptor();
    return take(rbt::FixedPublisher<T>::create({topic,d,rbt::StreamProfile::Sensor},d));
}
template<class T> void publish(rbt::FixedPublisher<T>& endpoint,const T& value) {
    auto sample=take(endpoint.loan()); sample.payload()=value;
    if (!sample.send()) throw std::runtime_error("RBT publication failed");
}
template<class T> class Input {
public:
    explicit Input(std::string topic):topic_(std::move(topic)) {}
    template<class F> void poll(F&& callback) {
        if (!endpoint_) {
            const auto& d=rbt::FixedMessageBinding<T>::descriptor();
            auto result=rbt::FixedSubscriber<T>::create({topic_,d,rbt::StreamProfile::Sensor},d);
            if (!result) {
                if (result.error()==rbt::StreamError::ReadinessOpen || result.error()==rbt::StreamError::Transport) return;
                throw std::runtime_error(std::string("RBT input: ")+rbt::to_string(result.error()));
            }
            endpoint_.emplace(std::move(result).value());
        }
        for (std::size_t i=0;i<rbt::MAX_DRAIN_PER_SOURCE;++i) {
            auto sample=take(endpoint_->receive());
            if (!sample) break;
            callback(sample->payload());
        }
    }
private:
    std::string topic_;
    std::optional<rbt::FixedSubscriber<T>> endpoint_;
};
inline volatile std::sig_atomic_t interrupted=0;
inline void signalHandler(int) { interrupted=1; }
inline void installSignals() { std::signal(SIGINT,signalHandler); std::signal(SIGTERM,signalHandler); }
template<std::size_t N> void frame(std::uint8_t (&dest)[N],const std::string& name) {
    if (name.empty() || name.size()>=N) throw std::invalid_argument("invalid frame name");
    std::fill(std::begin(dest),std::end(dest),0);
    std::copy(name.begin(),name.end(),dest);
}
} // namespace rbt::hardware::native
