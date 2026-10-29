#ifndef RBT_CPP_STREAM_HPP
#define RBT_CPP_STREAM_HPP

#include <iox2/backpressure_handler.hpp>
#include <iox2/iceoryx2.hpp>

#include <atomic>
#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <rbt/types.hpp>

namespace rbt {
namespace detail {

inline constexpr auto IpcService = iox2::ServiceType::Ipc;

enum class PressureEndpoint : std::uint8_t {
    Publisher,
    Subscriber,
};

class PressureState;

inline std::uint64_t saturating_add(std::atomic<std::uint64_t>& value,
                                    std::uint64_t amount = 1U) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    while (true) {
        const auto next = current > std::numeric_limits<std::uint64_t>::max() - amount
                            ? std::numeric_limits<std::uint64_t>::max()
                            : current + amount;
        if (value.compare_exchange_weak(current,
                                         next,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            return next;
        }
    }
}

inline std::uint64_t saturating_sub(std::atomic<std::uint64_t>& value,
                                    std::uint64_t amount = 1U) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    while (true) {
        const auto next = current < amount ? 0U : current - amount;
        if (value.compare_exchange_weak(current,
                                         next,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            return next;
        }
    }
}

inline std::uint64_t saturating_max(std::atomic<std::uint64_t>& value,
                                    std::uint64_t candidate) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    while (candidate > current) {
        if (value.compare_exchange_weak(current,
                                         candidate,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            return candidate;
        }
    }
    return current;
}

inline constexpr std::uint64_t kBorrowCommitted = std::uint64_t { 1U } << 63U;
inline constexpr std::uint64_t kBorrowReleasing = std::uint64_t { 1U } << 62U;
inline constexpr std::uint64_t kBorrowTimestampMask = kBorrowReleasing - 1U;

inline std::uint64_t monotonic_ns() noexcept {
    static const auto origin = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::steady_clock::now() - origin;
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    if (nanos <= 0) {
        return 1U;
    }
    const auto value = static_cast<std::uint64_t>(nanos);
    return value >= kBorrowTimestampMask ? kBorrowTimestampMask : value + 1U;
}

inline std::uint64_t unix_timestamp_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return millis < 0 ? 0U : static_cast<std::uint64_t>(millis);
}

struct BorrowSlot {
    std::atomic<std::uint64_t> state { 0U };
};

class LoanTracker {
  public:
    LoanTracker() = default;
    LoanTracker(const LoanTracker&) = delete;
    LoanTracker& operator=(const LoanTracker&) = delete;

    LoanTracker(LoanTracker&& other) noexcept
        : state_(std::move(other.state_)) {
    }

    LoanTracker& operator=(LoanTracker&& other) noexcept {
        if (this != &other) {
            release();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~LoanTracker() {
        release();
    }

    void release() noexcept;

  private:
    explicit LoanTracker(std::shared_ptr<PressureState> state)
        : state_(std::move(state)) {
    }

    std::shared_ptr<PressureState> state_;

    friend class PressureState;
};

class BorrowTracker {
  public:
    BorrowTracker() = default;
    BorrowTracker(const BorrowTracker&) = delete;
    BorrowTracker& operator=(const BorrowTracker&) = delete;

    BorrowTracker(BorrowTracker&& other) noexcept
        : state_(std::move(other.state_))
        , slot_(other.slot_)
        , start_ns_(other.start_ns_) {
        other.slot_ = 0U;
        other.start_ns_ = 0U;
    }

    BorrowTracker& operator=(BorrowTracker&& other) noexcept {
        if (this != &other) {
            release();
            state_ = std::move(other.state_);
            slot_ = other.slot_;
            start_ns_ = other.start_ns_;
            other.slot_ = 0U;
            other.start_ns_ = 0U;
        }
        return *this;
    }

    ~BorrowTracker() {
        release();
    }

    void release() noexcept;

  private:
    explicit BorrowTracker(std::shared_ptr<PressureState> state,
                           std::size_t slot,
                           std::uint64_t start_ns)
        : state_(std::move(state))
        , slot_(slot)
        , start_ns_(start_ns) {
    }

    std::shared_ptr<PressureState> state_;
    std::size_t slot_ { 0U };
    std::uint64_t start_ns_ { 0U };

    friend class PressureState;
    friend class BorrowReservation;
};

class BorrowReservation {
  public:
    BorrowReservation() = default;
    BorrowReservation(const BorrowReservation&) = delete;
    BorrowReservation& operator=(const BorrowReservation&) = delete;

    BorrowReservation(BorrowReservation&& other) noexcept
        : state_(std::move(other.state_))
        , slot_(other.slot_)
        , start_ns_(other.start_ns_) {
        other.slot_ = 0U;
        other.start_ns_ = 0U;
    }

    BorrowReservation& operator=(BorrowReservation&& other) noexcept {
        if (this != &other) {
            release();
            state_ = std::move(other.state_);
            slot_ = other.slot_;
            start_ns_ = other.start_ns_;
            other.slot_ = 0U;
            other.start_ns_ = 0U;
        }
        return *this;
    }

    ~BorrowReservation() {
        release();
    }

    [[nodiscard]] BorrowTracker commit() &&;

  private:
    BorrowReservation(std::shared_ptr<PressureState> state,
                      std::size_t slot,
                      std::uint64_t start_ns)
        : state_(std::move(state))
        , slot_(slot)
        , start_ns_(start_ns) {
    }

    void release() noexcept;

    std::shared_ptr<PressureState> state_;
    std::size_t slot_ { 0U };
    std::uint64_t start_ns_ { 0U };

    friend class PressureState;
};

class PressureState : public std::enable_shared_from_this<PressureState> {
  public:
    static std::shared_ptr<PressureState> publisher(std::string service_name,
                                                    StreamProfile profile,
                                                    StreamSemanticConfig semantic,
                                                    StreamResourceConfig resources,
                                                    std::function<std::uint64_t()> subscriber_count) {
        auto state = std::shared_ptr<PressureState>(
            new PressureState(std::move(service_name),
                              profile,
                              semantic,
                              resources,
                              PressureEndpoint::Publisher));
        state->subscriber_count_ = std::move(subscriber_count);
        register_state(state);
        return state;
    }

    static std::shared_ptr<PressureState> subscriber(std::string service_name,
                                                     StreamProfile profile,
                                                     StreamSemanticConfig semantic,
                                                     StreamResourceConfig resources) {
        auto state = std::shared_ptr<PressureState>(
            new PressureState(std::move(service_name),
                              profile,
                              semantic,
                              resources,
                              PressureEndpoint::Subscriber));
        register_state(state);
        return state;
    }

    [[nodiscard]] LoanTracker loan_succeeded(std::uint64_t payload_bytes) {
        saturating_add(loan_successes_);
        const auto in_use = saturating_add(loans_in_use_);
        saturating_max(loans_high_water_, in_use);
        payload_bytes_last_.store(payload_bytes, std::memory_order_relaxed);
        saturating_max(payload_bytes_max_seen_, payload_bytes);
        saturating_add(total_loaned_, payload_bytes);
        return LoanTracker(shared_from_this());
    }

    void loan_attempted() noexcept {
        saturating_add(loan_attempts_);
    }

    [[nodiscard]] std::optional<BorrowReservation> reserve_borrow() {
        const auto start_ns = monotonic_ns() & kBorrowTimestampMask;
        for (std::size_t slot = 0U; slot < borrow_slot_count_; ++slot) {
            auto& value = borrow_slots_[slot].state;
            std::uint64_t expected = 0U;
            if (value.compare_exchange_strong(expected,
                                              start_ns,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                return BorrowReservation(shared_from_this(), slot, start_ns);
            }
        }
        return std::nullopt;
    }

    void commit_borrow(std::size_t slot, std::uint64_t start_ns) noexcept {
        if (slot >= borrow_slot_count_) {
            return;
        }
        auto expected = start_ns;
        if (!borrow_slots_[slot].state.compare_exchange_strong(
                expected,
                start_ns | kBorrowCommitted,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            // A reservation is exclusively owned until it is committed. A
            // failed transition therefore indicates an internal ownership
            // invariant violation and must remain visible in release builds.
            std::terminate();
        }
    }

    void receive_attempted() noexcept {
        saturating_add(receive_attempts_);
    }

    void loan_released() noexcept {
        saturating_sub(loans_in_use_);
    }

    void borrow_succeeded() noexcept {
        saturating_add(received_samples_);
        const auto borrowed = saturating_add(borrowed_samples_);
        saturating_max(borrowed_high_water_, borrowed);
    }

    void loan_failed() noexcept {
        saturating_add(loan_failures_);
        set_event(PressureEvent::LoanPoolExhausted);
    }

    void send_attempted() noexcept {
        saturating_add(send_attempts_);
    }

    void send_succeeded() noexcept {
        saturating_add(send_successes_);
    }

    void send_failed(PressureEvent event) noexcept {
        saturating_add(send_failures_);
        if (event == PressureEvent::QueueFull) {
            auto pending = pending_queue_full_.load(std::memory_order_relaxed);
            while (pending != 0U
                   && !pending_queue_full_.compare_exchange_weak(pending,
                                                                  pending - 1U,
                                                                  std::memory_order_relaxed,
                                                                  std::memory_order_relaxed)) {
            }
            if (pending == 0U) {
                saturating_add(queue_full_);
            }
        } else if (event == PressureEvent::UnableToDeliver) {
            saturating_add(unable_to_deliver_);
        }
        set_event(event);
    }

    void borrow_failed() noexcept {
        saturating_add(borrow_failures_);
        set_event(PressureEvent::BorrowLimitReached);
    }

    void queue_full(std::optional<std::string> subscriber) noexcept {
        saturating_add(queue_full_);
        saturating_add(pending_queue_full_);
        set_event(PressureEvent::QueueFull, std::move(subscriber));
    }

    void readiness_notify_failed() noexcept {
        saturating_add(readiness_notify_failures_);
    }

    [[nodiscard]] StreamPressure snapshot() const {
        StreamPressure result;
        result.service_name = service_name_;
        result.profile = profile_;
        result.semantic = semantic_;
        result.resources = resources_;
        result.snapshot_timestamp_unix_ms = unix_timestamp_ms();
        {
            const std::lock_guard lock(event_mutex_);
            result.last_pressure_event = last_pressure_event_;
            result.last_pressure_subscriber = last_pressure_subscriber_;
        }

        if (endpoint_ == PressureEndpoint::Publisher) {
            PublisherPressure publisher;
            publisher.loan_attempts = loan_attempts_.load(std::memory_order_relaxed);
            publisher.loan_successes = loan_successes_.load(std::memory_order_relaxed);
            publisher.loans_in_use = loans_in_use_.load(std::memory_order_relaxed);
            publisher.loans_high_water = loans_high_water_.load(std::memory_order_relaxed);
            publisher.loan_failures = loan_failures_.load(std::memory_order_relaxed);
            publisher.payload_bytes_last = payload_bytes_last_.load(std::memory_order_relaxed);
            publisher.payload_bytes_max_seen = payload_bytes_max_seen_.load(std::memory_order_relaxed);
            publisher.total_loaned = total_loaned_.load(std::memory_order_relaxed);
            publisher.send_attempts = send_attempts_.load(std::memory_order_relaxed);
            publisher.send_successes = send_successes_.load(std::memory_order_relaxed);
            publisher.send_failures = send_failures_.load(std::memory_order_relaxed);
            publisher.queue_full = queue_full_.load(std::memory_order_relaxed);
            publisher.unable_to_deliver = unable_to_deliver_.load(std::memory_order_relaxed);
            publisher.readiness_notify_failures = readiness_notify_failures_.load(std::memory_order_relaxed);
            publisher.subscriber_count = subscriber_count_ ? subscriber_count_() : 0U;
            result.publisher = publisher;
        } else {
            SubscriberPressure subscriber;
            subscriber.receive_attempts = receive_attempts_.load(std::memory_order_relaxed);
            subscriber.received_samples = received_samples_.load(std::memory_order_relaxed);
            subscriber.borrowed_samples = borrowed_samples_.load(std::memory_order_relaxed);
            subscriber.borrowed_high_water = borrowed_high_water_.load(std::memory_order_relaxed);
            subscriber.borrow_failures = borrow_failures_.load(std::memory_order_relaxed);
            subscriber.borrow_duration_count = borrow_duration_count_.load(std::memory_order_relaxed);
            subscriber.borrow_duration_total_ns = borrow_duration_total_ns_.load(std::memory_order_relaxed);
            subscriber.borrow_duration_max_ns = borrow_duration_max_ns_.load(std::memory_order_relaxed);
            subscriber.oldest_borrow_age_ns = oldest_borrow_age_ns();
            result.subscriber = subscriber;
        }
        return result;
    }

    static std::vector<StreamPressure> snapshots() {
        std::vector<std::shared_ptr<PressureState>> live_states;
        {
            const std::lock_guard lock(registry_mutex());
            auto& registry = pressure_registry();
            live_states.reserve(registry.size());
            auto next = registry.begin();
            while (next != registry.end()) {
                if (auto state = next->lock()) {
                    live_states.emplace_back(std::move(state));
                    ++next;
                } else {
                    next = registry.erase(next);
                }
            }
        }

        std::vector<StreamPressure> snapshots;
        snapshots.reserve(live_states.size());
        for (const auto& state : live_states) {
            snapshots.emplace_back(state->snapshot());
        }
        return snapshots;
    }

  private:
    friend class BorrowTracker;
    friend class BorrowReservation;

    PressureState(std::string service_name,
                  StreamProfile profile,
                  StreamSemanticConfig semantic,
                  StreamResourceConfig resources,
                  PressureEndpoint endpoint)
        : service_name_(std::move(service_name))
        , profile_(profile)
        , semantic_(semantic)
        , resources_(resources)
        , endpoint_(endpoint) {
        borrow_slot_count_ = static_cast<std::size_t>(resources.max_borrowed);
        if (borrow_slot_count_ != 0U) {
            borrow_slots_ = std::make_unique<BorrowSlot[]>(borrow_slot_count_);
        }
    }

    static std::vector<std::weak_ptr<PressureState>>& pressure_registry() {
        static std::vector<std::weak_ptr<PressureState>> registry;
        return registry;
    }

    static std::mutex& registry_mutex() {
        static std::mutex mutex;
        return mutex;
    }

    static void register_state(const std::shared_ptr<PressureState>& state) {
        const std::lock_guard lock(registry_mutex());
        pressure_registry().emplace_back(state);
    }

    void set_event(PressureEvent event,
                   std::optional<std::string> subscriber = std::nullopt) noexcept {
        const std::lock_guard lock(event_mutex_);
        if (event == PressureEvent::QueueFull && !subscriber.has_value()
            && last_pressure_event_ == PressureEvent::QueueFull
            && last_pressure_subscriber_.has_value()) {
            return;
        }
        last_pressure_event_ = event;
        last_pressure_subscriber_ = std::move(subscriber);
    }

    void release_reservation(std::size_t slot, std::uint64_t start_ns) noexcept {
        if (slot >= borrow_slot_count_) {
            return;
        }
        (void)borrow_slots_[slot].state.compare_exchange_strong(start_ns,
                                                                 0U,
                                                                 std::memory_order_relaxed,
                                                                 std::memory_order_relaxed);
    }

    void release_borrow(std::size_t slot, std::uint64_t start_ns) noexcept {
        if (slot >= borrow_slot_count_) {
            return;
        }
        const auto committed = start_ns | kBorrowCommitted;
        auto expected = committed;
        if (!borrow_slots_[slot].state.compare_exchange_strong(expected,
                                                                committed | kBorrowReleasing,
                                                                std::memory_order_relaxed,
                                                                std::memory_order_relaxed)) {
            return;
        }
        // Keep the slot nonzero and unavailable until all aggregate counters
        // have been updated. Otherwise a concurrent reserve can acquire the
        // native slot before borrowed_samples is decremented and make the
        // reported high-water mark exceed the configured ceiling.
        saturating_sub(borrowed_samples_);
        const auto now = monotonic_ns();
        const auto duration = now > start_ns ? now - start_ns : 0U;
        saturating_add(borrow_duration_count_);
        saturating_add(borrow_duration_total_ns_, duration);
        saturating_max(borrow_duration_max_ns_, duration);
        borrow_slots_[slot].state.store(0U, std::memory_order_release);
    }

    [[nodiscard]] std::optional<std::uint64_t> oldest_borrow_age_ns() const noexcept {
        const auto now = monotonic_ns();
        std::optional<std::uint64_t> oldest;
        for (std::size_t slot = 0U; slot < borrow_slot_count_; ++slot) {
            const auto state = borrow_slots_[slot].state.load(std::memory_order_relaxed);
            if ((state & kBorrowCommitted) != 0U && (state & kBorrowReleasing) == 0U) {
                const auto start = state & kBorrowTimestampMask;
                const auto age = now > start ? now - start : 0U;
                if (!oldest.has_value() || age > oldest.value()) {
                    oldest = age;
                }
            }
        }
        return oldest;
    }

    std::string service_name_;
    StreamProfile profile_;
    StreamSemanticConfig semantic_;
    StreamResourceConfig resources_;
    PressureEndpoint endpoint_;
    std::function<std::uint64_t()> subscriber_count_;
    std::atomic<std::uint64_t> loan_attempts_ { 0U };
    std::atomic<std::uint64_t> loan_successes_ { 0U };
    std::atomic<std::uint64_t> loans_in_use_ { 0U };
    std::atomic<std::uint64_t> loans_high_water_ { 0U };
    std::atomic<std::uint64_t> loan_failures_ { 0U };
    std::atomic<std::uint64_t> payload_bytes_last_ { 0U };
    std::atomic<std::uint64_t> payload_bytes_max_seen_ { 0U };
    std::atomic<std::uint64_t> total_loaned_ { 0U };
    std::atomic<std::uint64_t> send_attempts_ { 0U };
    std::atomic<std::uint64_t> send_successes_ { 0U };
    std::atomic<std::uint64_t> send_failures_ { 0U };
    std::atomic<std::uint64_t> queue_full_ { 0U };
    std::atomic<std::uint64_t> pending_queue_full_ { 0U };
    std::atomic<std::uint64_t> unable_to_deliver_ { 0U };
    std::atomic<std::uint64_t> readiness_notify_failures_ { 0U };
    std::atomic<std::uint64_t> receive_attempts_ { 0U };
    std::atomic<std::uint64_t> received_samples_ { 0U };
    std::atomic<std::uint64_t> borrowed_samples_ { 0U };
    std::atomic<std::uint64_t> borrowed_high_water_ { 0U };
    std::atomic<std::uint64_t> borrow_failures_ { 0U };
    std::atomic<std::uint64_t> borrow_duration_count_ { 0U };
    std::atomic<std::uint64_t> borrow_duration_total_ns_ { 0U };
    std::atomic<std::uint64_t> borrow_duration_max_ns_ { 0U };
    std::unique_ptr<BorrowSlot[]> borrow_slots_;
    std::size_t borrow_slot_count_ { 0U };
    mutable std::mutex event_mutex_;
    std::optional<PressureEvent> last_pressure_event_;
    std::optional<std::string> last_pressure_subscriber_;
};

inline void LoanTracker::release() noexcept {
    if (state_ != nullptr) {
        state_->loan_released();
        state_.reset();
    }
}

inline void BorrowTracker::release() noexcept {
    if (state_ != nullptr) {
        state_->release_borrow(slot_, start_ns_);
        state_.reset();
        slot_ = 0U;
        start_ns_ = 0U;
    }
}

inline void BorrowReservation::release() noexcept {
    if (state_ != nullptr) {
        state_->release_reservation(slot_, start_ns_);
        state_.reset();
        slot_ = 0U;
        start_ns_ = 0U;
    }
}

inline BorrowTracker BorrowReservation::commit() && {
    auto state = std::move(state_);
    if (state == nullptr) {
        return BorrowTracker {};
    }
    const auto slot = slot_;
    const auto start_ns = start_ns_;
    slot_ = 0U;
    start_ns_ = 0U;
    state->commit_borrow(slot, start_ns);
    state->borrow_succeeded();
    return BorrowTracker(std::move(state), slot, start_ns);
}

inline StreamError map_send_error(iox2::SendError error, StreamProfile profile) noexcept {
    if (error == iox2::SendError::UnableToDeliver) {
        return profile == StreamProfile::Control ? StreamError::QueueFull : StreamError::UnableToDeliver;
    }
    return StreamError::Transport;
}

inline StreamError map_loan_error(iox2::LoanError error) noexcept {
    if (error == iox2::LoanError::OutOfMemory
        || error == iox2::LoanError::ExceedsMaxLoanedSamples) {
        return StreamError::LoanPoolExhausted;
    }
    return StreamError::Transport;
}

inline StreamError map_receive_error(iox2::ReceiveError error) noexcept {
    if (error == iox2::ReceiveError::ExceedsMaxBorrows) {
        return StreamError::BorrowLimitReached;
    }
    return StreamError::Transport;
}

inline StreamSemanticConfig semantic_config(StreamProfile profile) noexcept {
    return StreamSemanticConfig::for_profile(profile);
}

inline bool valid_resources(const StreamResourceConfig& resources) noexcept {
    return resources.max_loans != 0U && resources.max_borrowed != 0U
        && resources.max_loans <= std::numeric_limits<std::size_t>::max()
        && resources.max_borrowed <= std::numeric_limits<std::size_t>::max();
}

inline std::optional<StreamError> resolve_publisher_resources(
    const StreamDescriptor& stream,
    StreamResourceConfig requested,
    StreamResourceConfig& effective) noexcept {
    if (!valid_resources(requested) || !valid_resources(stream.resources)) {
        return StreamError::InvalidResourceConfig;
    }
    if (requested.max_loans != stream.resources.max_loans) {
        return StreamError::IncompatibleResourceConfig;
    }
    if (requested.max_borrowed != stream.resources.max_borrowed) {
        return StreamError::IncompatibleResourceConfig;
    }
    effective = stream.resources;
    return std::nullopt;
}

inline std::optional<StreamError> resolve_subscriber_resources(
    const StreamDescriptor& stream,
    StreamResourceConfig requested,
    StreamResourceConfig& effective) noexcept {
    if (!valid_resources(requested) || !valid_resources(stream.resources)) {
        return StreamError::InvalidResourceConfig;
    }
    if (requested.max_borrowed > stream.resources.max_borrowed) {
        return StreamError::IncompatibleResourceConfig;
    }
    effective = StreamResourceConfig { stream.resources.max_loans, requested.max_borrowed };
    return std::nullopt;
}

inline std::unique_ptr<iox2::BackpressureHandler> make_backpressure_handler(
    StreamProfile profile,
    const std::shared_ptr<PressureState>& pressure = {}) {
    if (profile != StreamProfile::Control) {
        return {};
    }

    return std::make_unique<iox2::BackpressureHandler>(
        [pressure](const iox2::BackpressureInfo& info) noexcept {
            if (pressure != nullptr) {
                try {
                    constexpr char hex[] = "0123456789abcdef";
                    const auto bytes = info.receiver_port_id();
                    std::string subscriber;
                    subscriber.reserve(bytes.size() * 2U);
                    for (std::size_t index = 0U; index < bytes.size(); ++index) {
                        const auto byte = bytes.element_at(index).value().get();
                        subscriber.push_back(hex[byte >> 4U]);
                        subscriber.push_back(hex[byte & 0x0fU]);
                    }
                    pressure->queue_full(std::move(subscriber));
                } catch (...) {
                    // The callback is noexcept. Preserve the QueueFull event
                    // even if allocating the optional diagnostic id fails.
                    pressure->queue_full(std::nullopt);
                }
            }
            return iox2::BackpressureAction::DiscardDataAndFail;
        });
}

inline Result<iox2::ServiceName> make_service_name(const StreamDescriptor& stream) {
    auto name = iox2::ServiceName::create(stream.service_name.c_str());
    if (!name.has_value()) {
        return Result<iox2::ServiceName>(StreamError::Transport);
    }
    return Result<iox2::ServiceName>(std::move(name).value());
}

inline Result<iox2::ServiceName> make_readiness_service_name(const StreamDescriptor& stream) {
    std::string name = stream.service_name;
    name += "/ready";
    auto readiness_name = iox2::ServiceName::create(name.c_str());
    if (!readiness_name.has_value()) {
        return Result<iox2::ServiceName>(StreamError::Transport);
    }
    return Result<iox2::ServiceName>(std::move(readiness_name).value());
}

inline bool notify_readiness(
    const std::shared_ptr<iox2::Notifier<IpcService>>& notifier) noexcept {
    return notifier != nullptr && notifier->notify().has_value();
}

inline Result<iox2::Node<IpcService>> make_node() {
    iox2::NodeBuilder builder;
    auto node = std::move(builder).create<IpcService>();
    if (!node.has_value()) {
        return Result<iox2::Node<IpcService>>(StreamError::Transport);
    }
    return Result<iox2::Node<IpcService>>(std::move(node).value());
}

} // namespace detail

inline std::vector<StreamPressure> local_pressure_snapshots() {
    return detail::PressureState::snapshots();
}

/// Stable rbt-owned identity for one event-driven readiness source.
///
/// The value is intentionally independent of iceoryx2 attachment addresses;
/// it remains valid for the source registration's lifetime and is the only
/// source identity exposed to a wait handler.
class WaitToken {
  public:
    /// The reserved token used by a WaitContext's cancellation source.
    static const WaitToken CANCELLATION;

    static constexpr WaitToken cancellation() noexcept {
        return WaitToken(std::numeric_limits<std::uint64_t>::max());
    }

    static constexpr WaitToken new_token(std::uint64_t value) noexcept {
        return WaitToken(value);
    }

    constexpr WaitToken() noexcept = default;
    explicit constexpr WaitToken(std::uint64_t value) noexcept
        : value_(value) {
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr std::uint64_t get() const noexcept {
        return value();
    }

    [[nodiscard]] constexpr std::uint64_t into_inner() const noexcept {
        return value();
    }

    friend constexpr bool operator==(const WaitToken&, const WaitToken&) = default;
    friend constexpr auto operator<=>(const WaitToken&, const WaitToken&) = default;

  private:
    std::uint64_t value_ { 0U };
};

inline constexpr WaitToken WaitToken::CANCELLATION = WaitToken::cancellation();
inline constexpr WaitToken WaitTokenCancellation = WaitToken::cancellation();

/// A non-owning readiness listener paired with a stable rbt token.
///
/// Coalesced activations only indicate that the subscriber may have data; they
/// do not correspond one-to-one with payloads.  Payload draining remains the
/// handler's job and is bounded by MAX_DRAIN_PER_SOURCE.  This is a C++ borrowed
/// view: the subscriber must remain alive at a stable address (must not be
/// moved) until the source and every wait call using it have ended.
class EventSource {
  public:
    EventSource(WaitToken token,
                iox2::Listener<detail::IpcService>& listener)
        : token_(token)
        , listener_(listener) {
    }

    [[nodiscard]] constexpr WaitToken token() const noexcept {
        return token_;
    }

    [[nodiscard]] iox2::Listener<detail::IpcService>& listener() const noexcept {
        return listener_.get();
    }

  private:
    WaitToken token_;
    std::reference_wrapper<iox2::Listener<detail::IpcService>> listener_;
};

template <typename T>
class LoanedSample;

template <typename T>
class ReceivedSample;

template <typename T>
class FixedSubscriber;

/// A fixed-size publisher that loans T directly from the iceoryx2 data segment.
template <typename T>
class FixedPublisher {
    using NativePublisher = iox2::Publisher<detail::IpcService, T, void>;
    using NativeFactory = iox2::PortFactoryPublishSubscribe<detail::IpcService, T, void>;
    using NativeSample = iox2::SampleMut<detail::IpcService, T, void>;
    using NativeNotifier = iox2::Notifier<detail::IpcService>;

    static_assert(std::is_standard_layout_v<T>, "fixed stream payload must be standard-layout");
    static_assert(std::is_trivially_copyable_v<T>, "fixed stream payload must be trivially copyable");

  public:
    static Result<FixedPublisher> create(const StreamDescriptor& stream,
                                          const MessageTypeDescriptor& expected) {
        return create(stream, expected, stream.resources);
    }

    static Result<FixedPublisher> create(const StreamDescriptor& stream,
                                          const MessageTypeDescriptor& expected,
                                          StreamResourceConfig resources) {
        StreamResourceConfig effective;
        if (const auto error = detail::resolve_publisher_resources(stream, resources, effective);
            error.has_value()) {
            return Result<FixedPublisher>(*error);
        }
        const auto& binding = FixedMessageBinding<T>::descriptor();
        if (!descriptors_equal(expected, binding)) {
            return Result<FixedPublisher>(StreamError::AbiMismatch);
        }
        // This check deliberately precedes ServiceName, Node, and transport open.
        if (const auto mismatch = preflight(stream, binding); mismatch.has_value()) {
            return Result<FixedPublisher>(*mismatch);
        }

        auto name = detail::make_service_name(stream);
        if (!name) {
            return Result<FixedPublisher>(name.error());
        }
        auto node = detail::make_node();
        if (!node) {
            return Result<FixedPublisher>(node.error());
        }

        auto readiness_name = detail::make_readiness_service_name(stream);
        if (!readiness_name) {
            return Result<FixedPublisher>(readiness_name.error());
        }
        auto readiness_event = node.value().service_builder(readiness_name.value()).event();
        auto readiness_factory = std::move(readiness_event).open_or_create();
        if (!readiness_factory.has_value()) {
            return Result<FixedPublisher>(StreamError::ReadinessOpenOrCreate);
        }
        auto readiness_notifier = std::move(readiness_factory).value().notifier_builder().create();
        if (!readiness_notifier.has_value()) {
            return Result<FixedPublisher>(StreamError::ReadinessNotifier);
        }

        auto service_builder = node.value().service_builder(name.value());
        auto publish_subscribe = std::move(service_builder).publish_subscribe<T>();
        const auto semantic = detail::semantic_config(stream.profile);
        std::move(publish_subscribe).enable_safe_overflow(semantic.safe_overflow);
        std::move(publish_subscribe).subscriber_max_buffer_size(semantic.queue_depth);
        std::move(publish_subscribe).subscriber_max_borrowed_samples(effective.max_borrowed);
        std::move(publish_subscribe).history_size(semantic.history_size);
        auto factory = std::move(publish_subscribe).open_or_create();
        if (!factory.has_value()) {
            return Result<FixedPublisher>(StreamError::Transport);
        }

        auto factory_owner = std::make_shared<NativeFactory>(std::move(factory).value());
        const std::weak_ptr<NativeFactory> pressure_factory = factory_owner;
        auto pressure = detail::PressureState::publisher(
            stream.service_name,
            stream.profile,
            semantic,
            effective,
            [pressure_factory]() {
                if (const auto factory = pressure_factory.lock()) {
                    return factory->dynamic_config().number_of_subscribers();
                }
                return std::uint64_t { 0U };
            });
        auto backpressure = detail::make_backpressure_handler(stream.profile, pressure);
        auto publisher_builder = factory_owner->publisher_builder();
        std::move(publisher_builder).backpressure_strategy(iox2::BackpressureStrategy::DiscardData);
        std::move(publisher_builder).max_loaned_samples(effective.max_loans);
        if (backpressure != nullptr) {
            std::move(publisher_builder).set_backpressure_handler(backpressure.get());
        }
        auto publisher = std::move(publisher_builder).create();
        if (!publisher.has_value()) {
            return Result<FixedPublisher>(StreamError::Transport);
        }

        FixedPublisher result(std::move(node).value(),
                              std::move(backpressure),
                              std::move(factory_owner),
                              std::move(publisher).value(),
                              std::make_shared<NativeNotifier>(std::move(readiness_notifier).value()),
                              effective,
                              std::move(pressure),
                              stream.profile);
        return Result<FixedPublisher>(std::move(result));
    }

    static Result<FixedPublisher> create_with_resources(const StreamDescriptor& stream,
                                                         const MessageTypeDescriptor& expected,
                                                         StreamResourceConfig resources) {
        return create(stream, expected, resources);
    }

    static Result<FixedPublisher> create_with_limits(const StreamDescriptor& stream,
                                                      const MessageTypeDescriptor& expected,
                                                      StreamResourceConfig resources) {
        return create(stream, expected, resources);
    }

    FixedPublisher(const FixedPublisher&) = delete;
    FixedPublisher& operator=(const FixedPublisher&) = delete;
    FixedPublisher(FixedPublisher&& other) noexcept
        : node_(std::move(other.node_))
        , backpressure_(std::move(other.backpressure_))
        , factory_(std::move(other.factory_))
        , endpoint_(std::move(other.endpoint_))
        , notifier_(std::move(other.notifier_))
        , resources_(other.resources_)
        , pressure_(std::move(other.pressure_))
        , profile_(other.profile_) {
        other.endpoint_.reset();
        other.notifier_.reset();
        other.factory_.reset();
        other.backpressure_.reset();
        other.node_.reset();
        other.pressure_.reset();
    }

    FixedPublisher& operator=(FixedPublisher&& other) noexcept {
        if (this != &other) {
            endpoint_ = std::move(other.endpoint_);
            notifier_ = std::move(other.notifier_);
            factory_ = std::move(other.factory_);
            backpressure_ = std::move(other.backpressure_);
            node_ = std::move(other.node_);
            resources_ = other.resources_;
            pressure_ = std::move(other.pressure_);
            profile_ = other.profile_;

            other.endpoint_.reset();
            other.notifier_.reset();
            other.factory_.reset();
            other.backpressure_.reset();
            other.node_.reset();
            other.pressure_.reset();
        }
        return *this;
    }
    ~FixedPublisher() = default;

    Result<LoanedSample<T>> loan() {
        if (!endpoint_.has_value()) {
            return Result<LoanedSample<T>>(StreamError::Transport);
        }
        pressure_->loan_attempted();
        auto sample = endpoint_->loan();
        if (!sample.has_value()) {
            if (sample.error() == iox2::LoanError::OutOfMemory
                || sample.error() == iox2::LoanError::ExceedsMaxLoanedSamples) {
                pressure_->loan_failed();
            }
            return Result<LoanedSample<T>>(detail::map_loan_error(sample.error()));
        }
        return Result<LoanedSample<T>>(
            LoanedSample<T>(std::move(sample).value(), profile_, pressure_, notifier_, sizeof(T), node_));
    }

    [[nodiscard]] StreamProfile profile() const noexcept {
        return profile_;
    }

    [[nodiscard]] StreamPressure pressure() const {
        return pressure_ != nullptr ? pressure_->snapshot() : StreamPressure {};
    }

  private:
    FixedPublisher(iox2::Node<detail::IpcService>&& node,
                   std::unique_ptr<iox2::BackpressureHandler>&& backpressure,
                   std::shared_ptr<NativeFactory>&& factory,
                   NativePublisher&& endpoint,
                   std::shared_ptr<NativeNotifier>&& notifier,
                   StreamResourceConfig resources,
                   std::shared_ptr<detail::PressureState>&& pressure,
                   StreamProfile profile)
        : node_(std::make_shared<iox2::Node<detail::IpcService>>(std::move(node)))
        , backpressure_(std::move(backpressure))
        , factory_(std::move(factory))
        , endpoint_(std::in_place, std::move(endpoint))
        , notifier_(std::move(notifier))
        , resources_(resources)
        , pressure_(std::move(pressure))
        , profile_(profile) {
    }

    // Members are destroyed in reverse declaration order: endpoint, factory, handler, node.
    std::shared_ptr<iox2::Node<detail::IpcService>> node_;
    std::unique_ptr<iox2::BackpressureHandler> backpressure_;
    std::shared_ptr<NativeFactory> factory_;
    std::optional<NativePublisher> endpoint_;
    std::shared_ptr<NativeNotifier> notifier_;
    StreamResourceConfig resources_;
    std::shared_ptr<detail::PressureState> pressure_;
    StreamProfile profile_;

    friend class LoanedSample<T>;
};

/// A fixed-size subscriber.  It uses open(), never open_or_create().
template <typename T>
class FixedSubscriber {
    using NativeSubscriber = iox2::Subscriber<detail::IpcService, T, void>;
    using NativeFactory = iox2::PortFactoryPublishSubscribe<detail::IpcService, T, void>;
    using NativeSample = iox2::Sample<detail::IpcService, T, void>;
    using NativeListener = iox2::Listener<detail::IpcService>;

    static_assert(std::is_standard_layout_v<T>, "fixed stream payload must be standard-layout");
    static_assert(std::is_trivially_copyable_v<T>, "fixed stream payload must be trivially copyable");

  public:
    static Result<FixedSubscriber> create(const StreamDescriptor& stream,
                                          const MessageTypeDescriptor& expected) {
        return create(stream, expected, stream.resources);
    }

    static Result<FixedSubscriber> create(const StreamDescriptor& stream,
                                          const MessageTypeDescriptor& expected,
                                          StreamResourceConfig resources) {
        StreamResourceConfig effective;
        if (const auto error = detail::resolve_subscriber_resources(stream, resources, effective);
            error.has_value()) {
            return Result<FixedSubscriber>(*error);
        }
        const auto& binding = FixedMessageBinding<T>::descriptor();
        if (!descriptors_equal(expected, binding)) {
            return Result<FixedSubscriber>(StreamError::AbiMismatch);
        }
        // This check deliberately precedes ServiceName, Node, and transport open.
        if (const auto mismatch = preflight(stream, binding); mismatch.has_value()) {
            return Result<FixedSubscriber>(*mismatch);
        }

        auto name = detail::make_service_name(stream);
        if (!name) {
            return Result<FixedSubscriber>(name.error());
        }
        auto node = detail::make_node();
        if (!node) {
            return Result<FixedSubscriber>(node.error());
        }

        auto readiness_name = detail::make_readiness_service_name(stream);
        if (!readiness_name) {
            return Result<FixedSubscriber>(readiness_name.error());
        }
        auto readiness_event = node.value().service_builder(readiness_name.value()).event();
        auto readiness_factory = std::move(readiness_event).open();
        if (!readiness_factory.has_value()) {
            return Result<FixedSubscriber>(StreamError::ReadinessOpen);
        }
        auto readiness_listener = std::move(readiness_factory).value().listener_builder().create();
        if (!readiness_listener.has_value()) {
            return Result<FixedSubscriber>(StreamError::ReadinessListener);
        }

        auto service_builder = node.value().service_builder(name.value());
        auto publish_subscribe = std::move(service_builder).publish_subscribe<T>();
        const auto semantic = detail::semantic_config(stream.profile);
        const auto advertised = stream.resources;
        std::move(publish_subscribe).enable_safe_overflow(semantic.safe_overflow);
        std::move(publish_subscribe).subscriber_max_buffer_size(semantic.queue_depth);
        std::move(publish_subscribe).subscriber_max_borrowed_samples(advertised.max_borrowed);
        std::move(publish_subscribe).history_size(semantic.history_size);
        auto factory = std::move(publish_subscribe).open();
        if (!factory.has_value()) {
            return Result<FixedSubscriber>(StreamError::Transport);
        }

        auto subscriber_builder = factory.value().subscriber_builder();
        std::move(subscriber_builder).buffer_size(semantic.queue_depth);
        auto subscriber = std::move(subscriber_builder).create();
        if (!subscriber.has_value()) {
            return Result<FixedSubscriber>(StreamError::Transport);
        }

        FixedSubscriber result(std::move(node).value(),
                               std::move(factory).value(),
                               std::move(subscriber).value(),
                               std::move(readiness_listener).value(),
                               effective,
                               detail::PressureState::subscriber(
                                   stream.service_name, stream.profile, semantic, effective),
                               stream.profile);
        return Result<FixedSubscriber>(std::move(result));
    }

    static Result<FixedSubscriber> create_with_resources(const StreamDescriptor& stream,
                                                         const MessageTypeDescriptor& expected,
                                                         StreamResourceConfig resources) {
        return create(stream, expected, resources);
    }

    static Result<FixedSubscriber> create_with_limits(const StreamDescriptor& stream,
                                                       const MessageTypeDescriptor& expected,
                                                       StreamResourceConfig resources) {
        return create(stream, expected, resources);
    }

    FixedSubscriber(const FixedSubscriber&) = delete;
    FixedSubscriber& operator=(const FixedSubscriber&) = delete;
    FixedSubscriber(FixedSubscriber&& other) noexcept
        : node_(std::move(other.node_))
        , factory_(std::move(other.factory_))
        , listener_(std::move(other.listener_))
        , endpoint_(std::move(other.endpoint_))
        , resources_(other.resources_)
        , pressure_(std::move(other.pressure_))
        , profile_(other.profile_) {
        other.endpoint_.reset();
        other.listener_.reset();
        other.factory_.reset();
        other.node_.reset();
        other.pressure_.reset();
    }

    FixedSubscriber& operator=(FixedSubscriber&& other) noexcept {
        if (this != &other) {
            endpoint_ = std::move(other.endpoint_);
            listener_ = std::move(other.listener_);
            factory_ = std::move(other.factory_);
            node_ = std::move(other.node_);
            resources_ = other.resources_;
            pressure_ = std::move(other.pressure_);
            profile_ = other.profile_;

            other.endpoint_.reset();
            other.listener_.reset();
            other.factory_.reset();
            other.node_.reset();
            other.pressure_.reset();
        }
        return *this;
    }
    ~FixedSubscriber() = default;

    Result<std::optional<ReceivedSample<T>>> receive() const {
        if (!endpoint_.has_value()) {
            return Result<std::optional<ReceivedSample<T>>>(StreamError::Transport);
        }
        pressure_->receive_attempted();
        auto reservation = pressure_->reserve_borrow();
        if (!reservation.has_value()) {
            pressure_->borrow_failed();
            return Result<std::optional<ReceivedSample<T>>>(StreamError::BorrowLimitReached);
        }
        auto result = endpoint_->receive();
        if (!result.has_value()) {
            if (result.error() == iox2::ReceiveError::ExceedsMaxBorrows) {
                pressure_->borrow_failed();
            }
            return Result<std::optional<ReceivedSample<T>>>(detail::map_receive_error(result.error()));
        }
        auto samples = std::move(result).value();
        if (!samples.has_value()) {
            return Result<std::optional<ReceivedSample<T>>>(std::optional<ReceivedSample<T>> {});
        }
        ReceivedSample<T> received_sample(
            std::move(samples).value(), std::move(*reservation).commit(), node_);
        std::optional<ReceivedSample<T>> sample(std::move(received_sample));
        return Result<std::optional<ReceivedSample<T>>>(std::move(sample));
    }

    [[nodiscard]] std::uint64_t buffer_size() const noexcept {
        assert(endpoint_.has_value());
        if (!endpoint_.has_value()) {
            return 0U;
        }
        return endpoint_->buffer_size();
    }

    [[nodiscard]] EventSource event_source(WaitToken token) const& {
        assert(listener_.has_value());
        return EventSource(token, listener_.value());
    }

    EventSource event_source(WaitToken) const&& = delete;

    [[nodiscard]] EventSource wait_source(WaitToken token) const& {
        return event_source(token);
    }

    EventSource wait_source(WaitToken) const&& = delete;

    [[nodiscard]] StreamPressure pressure() const {
        return pressure_ != nullptr ? pressure_->snapshot() : StreamPressure {};
    }

  private:
    FixedSubscriber(iox2::Node<detail::IpcService>&& node,
                    NativeFactory&& factory,
                    NativeSubscriber&& endpoint,
                    NativeListener&& listener,
                    StreamResourceConfig resources,
                    std::shared_ptr<detail::PressureState>&& pressure,
                    StreamProfile profile)
        : node_(std::make_shared<iox2::Node<detail::IpcService>>(std::move(node)))
        , factory_(std::in_place, std::move(factory))
        , listener_(std::in_place, std::move(listener))
        , endpoint_(std::in_place, std::move(endpoint))
        , resources_(resources)
        , pressure_(std::move(pressure))
        , profile_(profile) {
    }

    // Members are destroyed in reverse declaration order: endpoint, factory, node.
    std::shared_ptr<iox2::Node<detail::IpcService>> node_;
    std::optional<NativeFactory> factory_;
    mutable std::optional<NativeListener> listener_;
    std::optional<NativeSubscriber> endpoint_;
    StreamResourceConfig resources_;
    std::shared_ptr<detail::PressureState> pressure_;
    StreamProfile profile_;

    friend class ReceivedSample<T>;
};

/// A mutable fixed-size native loan.  send() consumes the guard logically.
template <typename T>
class LoanedSample {
    using NativeSample = iox2::SampleMut<detail::IpcService, T, void>;

    static_assert(std::is_standard_layout_v<T>, "fixed stream payload must be standard-layout");
    static_assert(std::is_trivially_copyable_v<T>, "fixed stream payload must be trivially copyable");

  public:
    LoanedSample(const LoanedSample&) = delete;
    LoanedSample& operator=(const LoanedSample&) = delete;
    LoanedSample(LoanedSample&& other) noexcept
        : node_(std::move(other.node_))
        , sample_(std::move(other.sample_))
        , pressure_(std::move(other.pressure_))
        , loan_(std::move(other.loan_))
        , notifier_(std::move(other.notifier_))
        , profile_(other.profile_) {
        other.sample_.reset();
    }

    LoanedSample& operator=(LoanedSample&& other) noexcept {
        if (this != &other) {
            loan_ = std::move(other.loan_);
            sample_ = std::move(other.sample_);
            profile_ = other.profile_;
            pressure_ = std::move(other.pressure_);
            notifier_ = std::move(other.notifier_);
            node_ = std::move(other.node_);
            other.sample_.reset();
        }
        return *this;
    }
    ~LoanedSample() = default;

    T& payload() {
        assert(sample_.has_value());
        return sample_->payload_mut();
    }

    const T& payload() const {
        assert(sample_.has_value());
        return sample_->payload();
    }

    Result<std::size_t> send() {
        if (!sample_.has_value()) {
            return Result<std::size_t>(StreamError::Transport);
        }
        if (pressure_ != nullptr) {
            pressure_->send_attempted();
        }
        // Sending transfers application authority to the transport. Release
        // the local pressure ownership before the native allocation becomes
        // reusable, including when another thread loans concurrently.
        loan_.release();
        auto sent = iox2::send(std::move(sample_).value());
        const bool readiness_notified = detail::notify_readiness(notifier_);
        if (!readiness_notified && pressure_ != nullptr) {
            pressure_->readiness_notify_failed();
        }
        sample_.reset();
        if (!sent.has_value()) {
            if (pressure_ != nullptr) {
                pressure_->send_failed(sent.error() == iox2::SendError::UnableToDeliver
                                           && profile_ == StreamProfile::Control
                                       ? PressureEvent::QueueFull
                                       : PressureEvent::UnableToDeliver);
            }
            return Result<std::size_t>(detail::map_send_error(sent.error(), profile_));
        }
        if (pressure_ != nullptr) {
            pressure_->send_succeeded();
        }
        if (!readiness_notified) {
            return Result<std::size_t>(StreamError::ReadinessNotify);
        }
        return Result<std::size_t>(std::move(sent).value());
    }

  private:
    LoanedSample(NativeSample&& sample,
                 StreamProfile profile,
                 const std::shared_ptr<detail::PressureState>& pressure,
                 const std::shared_ptr<iox2::Notifier<detail::IpcService>>& notifier,
                 std::uint64_t payload_bytes,
                 std::shared_ptr<iox2::Node<detail::IpcService>> node)
        : node_(std::move(node))
        , sample_(std::in_place, std::move(sample))
        , pressure_(pressure)
        , loan_(pressure != nullptr ? pressure->loan_succeeded(payload_bytes) : detail::LoanTracker {})
        , notifier_(notifier)
        , profile_(profile) {
    }

    // Declared first, destroyed last: a sample keeps its port alive, and a
    // node destroyed before its ports cannot remove its node directory.
    std::shared_ptr<iox2::Node<detail::IpcService>> node_;
    // Declare the native sample before LoanTracker so LoanTracker is
    // destroyed first; the pressure count reaches zero before native loan reuse.
    std::optional<NativeSample> sample_;
    std::shared_ptr<detail::PressureState> pressure_;
    detail::LoanTracker loan_;
    std::shared_ptr<iox2::Notifier<detail::IpcService>> notifier_;
    StreamProfile profile_;

    friend class FixedPublisher<T>;
};

/// A received fixed-size native sample guard.  Its payload reference is valid while it lives.
template <typename T>
class ReceivedSample {
    using NativeSample = iox2::Sample<detail::IpcService, T, void>;

    static_assert(std::is_standard_layout_v<T>, "fixed stream payload must be standard-layout");
    static_assert(std::is_trivially_copyable_v<T>, "fixed stream payload must be trivially copyable");

  public:
    ReceivedSample(const ReceivedSample&) = delete;
    ReceivedSample& operator=(const ReceivedSample&) = delete;
    ReceivedSample(ReceivedSample&& other) noexcept
        : node_(std::move(other.node_))
        , borrow_(std::move(other.borrow_))
        , sample_(std::move(other.sample_)) {
        other.sample_.reset();
    }

    ReceivedSample& operator=(ReceivedSample&& other) noexcept {
        if (this != &other) {
            sample_ = std::move(other.sample_);
            borrow_ = std::move(other.borrow_);
            node_ = std::move(other.node_);
            other.sample_.reset();
        }
        return *this;
    }
    ~ReceivedSample() = default;

    [[nodiscard]] const T& payload() const {
        assert(sample_.has_value());
        return sample_->payload();
    }

  private:
    ReceivedSample(NativeSample&& sample,
                   detail::BorrowTracker&& borrow,
                   std::shared_ptr<iox2::Node<detail::IpcService>> node)
        : node_(std::move(node))
        , borrow_(std::move(borrow))
        , sample_(std::in_place, std::move(sample)) {
    }

    // Declared first, destroyed last: a sample keeps its port alive, and a
    // node destroyed before its ports cannot remove its node directory.
    std::shared_ptr<iox2::Node<detail::IpcService>> node_;
    detail::BorrowTracker borrow_;
    // Declared after borrow_ so native Drop releases the borrow before the counter.
    std::optional<NativeSample> sample_;

    friend class FixedSubscriber<T>;
};

template <typename Header, typename Element>
class LoanedSliceSample;

template <typename Header, typename Element>
class ReceivedSliceSample;

/// A header-plus-slice publisher.  loan_slice() is a direct native loan.
template <typename Header, typename Element>
class SlicePublisher {
    using Payload = iox2::bb::Slice<Element>;
    using NativePublisher = iox2::Publisher<detail::IpcService, Payload, Header>;
    using NativeFactory = iox2::PortFactoryPublishSubscribe<detail::IpcService, Payload, Header>;
    using NativeSample = iox2::SampleMut<detail::IpcService, Payload, Header>;
    using NativeNotifier = iox2::Notifier<detail::IpcService>;

    static_assert(std::is_standard_layout_v<Header>, "slice stream header must be standard-layout");
    static_assert(std::is_trivially_copyable_v<Header>, "slice stream header must be trivially copyable");
    static_assert(std::is_standard_layout_v<Element>, "slice stream element must be standard-layout");
    static_assert(std::is_trivially_copyable_v<Element>, "slice stream element must be trivially copyable");

  public:
    static Result<SlicePublisher> create(const StreamDescriptor& stream,
                                          const MessageTypeDescriptor& expected,
                                          std::size_t maximum_elements) {
        return create(stream,
                      expected,
                      maximum_elements,
                      stream.resources);
    }

    static Result<SlicePublisher> create(const StreamDescriptor& stream,
                                         const MessageTypeDescriptor& expected,
                                         std::size_t maximum_elements,
                                         StreamResourceConfig resources) {
        StreamResourceConfig effective;
        if (const auto error = detail::resolve_publisher_resources(stream, resources, effective);
            error.has_value()) {
            return Result<SlicePublisher>(*error);
        }
        const auto& binding = SliceMessageBinding<Header, Element>::descriptor();
        if (!descriptors_equal(expected, binding)) {
            return Result<SlicePublisher>(StreamError::AbiMismatch);
        }
        // This check deliberately precedes ServiceName, Node, and transport open.
        if (const auto mismatch = preflight(stream, binding); mismatch.has_value()) {
            return Result<SlicePublisher>(*mismatch);
        }

        auto name = detail::make_service_name(stream);
        if (!name) {
            return Result<SlicePublisher>(name.error());
        }
        auto node = detail::make_node();
        if (!node) {
            return Result<SlicePublisher>(node.error());
        }

        auto readiness_name = detail::make_readiness_service_name(stream);
        if (!readiness_name) {
            return Result<SlicePublisher>(readiness_name.error());
        }
        auto readiness_event = node.value().service_builder(readiness_name.value()).event();
        auto readiness_factory = std::move(readiness_event).open_or_create();
        if (!readiness_factory.has_value()) {
            return Result<SlicePublisher>(StreamError::ReadinessOpenOrCreate);
        }
        auto readiness_notifier = std::move(readiness_factory).value().notifier_builder().create();
        if (!readiness_notifier.has_value()) {
            return Result<SlicePublisher>(StreamError::ReadinessNotifier);
        }

        auto service_builder = node.value().service_builder(name.value());
        auto publish_subscribe =
            std::move(service_builder).publish_subscribe<Payload>().template user_header<Header>();
        const auto semantic = detail::semantic_config(stream.profile);
        std::move(publish_subscribe).enable_safe_overflow(semantic.safe_overflow);
        std::move(publish_subscribe).subscriber_max_buffer_size(semantic.queue_depth);
        std::move(publish_subscribe).subscriber_max_borrowed_samples(effective.max_borrowed);
        std::move(publish_subscribe).history_size(semantic.history_size);
        auto factory = std::move(publish_subscribe).open_or_create();
        if (!factory.has_value()) {
            return Result<SlicePublisher>(StreamError::Transport);
        }

        auto factory_owner = std::make_shared<NativeFactory>(std::move(factory).value());
        const std::weak_ptr<NativeFactory> pressure_factory = factory_owner;
        auto pressure = detail::PressureState::publisher(
            stream.service_name,
            stream.profile,
            semantic,
            effective,
            [pressure_factory]() {
                if (const auto factory = pressure_factory.lock()) {
                    return factory->dynamic_config().number_of_subscribers();
                }
                return std::uint64_t { 0U };
            });
        auto backpressure = detail::make_backpressure_handler(stream.profile, pressure);
        auto publisher_builder = factory_owner->publisher_builder();
        std::move(publisher_builder).backpressure_strategy(iox2::BackpressureStrategy::DiscardData);
        std::move(publisher_builder).initial_max_slice_len(maximum_elements);
        std::move(publisher_builder).max_loaned_samples(effective.max_loans);
        if (backpressure != nullptr) {
            std::move(publisher_builder).set_backpressure_handler(backpressure.get());
        }
        auto publisher = std::move(publisher_builder).create();
        if (!publisher.has_value()) {
            return Result<SlicePublisher>(StreamError::Transport);
        }

        SlicePublisher result(std::move(node).value(),
                              std::move(backpressure),
                              std::move(factory_owner),
                              std::move(publisher).value(),
                              std::make_shared<NativeNotifier>(std::move(readiness_notifier).value()),
                              maximum_elements,
                              effective,
                              std::move(pressure),
                              stream.profile);
        return Result<SlicePublisher>(std::move(result));
    }

    static Result<SlicePublisher> create_with_resources(const StreamDescriptor& stream,
                                                        const MessageTypeDescriptor& expected,
                                                        std::size_t maximum_elements,
                                                        StreamResourceConfig resources) {
        return create(stream, expected, maximum_elements, resources);
    }

    static Result<SlicePublisher> create_with_limits(const StreamDescriptor& stream,
                                                      const MessageTypeDescriptor& expected,
                                                      std::size_t maximum_elements,
                                                      StreamResourceConfig resources) {
        return create(stream, expected, maximum_elements, resources);
    }

    static Result<SlicePublisher> create_with_limits(const StreamDescriptor& stream,
                                                      const MessageTypeDescriptor& expected,
                                                      StreamResourceConfig resources,
                                                      std::size_t maximum_elements) {
        return create(stream, expected, maximum_elements, resources);
    }

    SlicePublisher(const SlicePublisher&) = delete;
    SlicePublisher& operator=(const SlicePublisher&) = delete;
    SlicePublisher(SlicePublisher&& other) noexcept
        : node_(std::move(other.node_))
        , backpressure_(std::move(other.backpressure_))
        , factory_(std::move(other.factory_))
        , endpoint_(std::move(other.endpoint_))
        , notifier_(std::move(other.notifier_))
        , maximum_elements_(other.maximum_elements_)
        , resources_(other.resources_)
        , pressure_(std::move(other.pressure_))
        , profile_(other.profile_) {
        other.endpoint_.reset();
        other.notifier_.reset();
        other.factory_.reset();
        other.backpressure_.reset();
        other.node_.reset();
        other.pressure_.reset();
    }

    SlicePublisher& operator=(SlicePublisher&& other) noexcept {
        if (this != &other) {
            endpoint_ = std::move(other.endpoint_);
            notifier_ = std::move(other.notifier_);
            factory_ = std::move(other.factory_);
            backpressure_ = std::move(other.backpressure_);
            node_ = std::move(other.node_);
            maximum_elements_ = other.maximum_elements_;
            resources_ = other.resources_;
            pressure_ = std::move(other.pressure_);
            profile_ = other.profile_;

            other.endpoint_.reset();
            other.notifier_.reset();
            other.factory_.reset();
            other.backpressure_.reset();
            other.node_.reset();
            other.pressure_.reset();
        }
        return *this;
    }
    ~SlicePublisher() = default;

    [[nodiscard]] std::size_t maximum_elements() const noexcept {
        return maximum_elements_;
    }

    Result<LoanedSliceSample<Header, Element>> loan_slice(std::size_t element_count) {
        if (!endpoint_.has_value()) {
            return Result<LoanedSliceSample<Header, Element>>(StreamError::Transport);
        }
        if (element_count > maximum_elements_) {
            return Result<LoanedSliceSample<Header, Element>>(StreamError::InvalidPayloadLength);
        }
        pressure_->loan_attempted();
        auto sample = endpoint_->loan_slice(static_cast<std::uint64_t>(element_count));
        if (!sample.has_value()) {
            if (sample.error() == iox2::LoanError::OutOfMemory
                || sample.error() == iox2::LoanError::ExceedsMaxLoanedSamples) {
                pressure_->loan_failed();
            }
            return Result<LoanedSliceSample<Header, Element>>(detail::map_loan_error(sample.error()));
        }
        return Result<LoanedSliceSample<Header, Element>>(
            LoanedSliceSample<Header, Element>(
                std::move(sample).value(),
                profile_,
                pressure_,
                notifier_,
                element_count > std::numeric_limits<std::uint64_t>::max() / sizeof(Element)
                    ? std::numeric_limits<std::uint64_t>::max()
                    : static_cast<std::uint64_t>(element_count * sizeof(Element)),
                node_));
    }

    Result<LoanedSliceSample<Header, Element>> loan() {
        return loan_slice(maximum_elements_);
    }

    [[nodiscard]] StreamProfile profile() const noexcept {
        return profile_;
    }

    [[nodiscard]] StreamPressure pressure() const {
        return pressure_ != nullptr ? pressure_->snapshot() : StreamPressure {};
    }

  private:
    SlicePublisher(iox2::Node<detail::IpcService>&& node,
                   std::unique_ptr<iox2::BackpressureHandler>&& backpressure,
                   std::shared_ptr<NativeFactory>&& factory,
                   NativePublisher&& endpoint,
                   std::shared_ptr<NativeNotifier>&& notifier,
                   std::size_t maximum_elements,
                   StreamResourceConfig resources,
                   std::shared_ptr<detail::PressureState>&& pressure,
                   StreamProfile profile)
        : node_(std::make_shared<iox2::Node<detail::IpcService>>(std::move(node)))
        , backpressure_(std::move(backpressure))
        , factory_(std::move(factory))
        , endpoint_(std::in_place, std::move(endpoint))
        , notifier_(std::move(notifier))
        , maximum_elements_(maximum_elements)
        , resources_(resources)
        , pressure_(std::move(pressure))
        , profile_(profile) {
    }

    std::shared_ptr<iox2::Node<detail::IpcService>> node_;
    std::unique_ptr<iox2::BackpressureHandler> backpressure_;
    std::shared_ptr<NativeFactory> factory_;
    std::optional<NativePublisher> endpoint_;
    std::shared_ptr<NativeNotifier> notifier_;
    std::size_t maximum_elements_;
    StreamResourceConfig resources_;
    std::shared_ptr<detail::PressureState> pressure_;
    StreamProfile profile_;

    friend class LoanedSliceSample<Header, Element>;
};

/// A header-plus-slice subscriber.  It opens an existing service only.
template <typename Header, typename Element>
class SliceSubscriber {
    using Payload = iox2::bb::Slice<Element>;
    using NativeSubscriber = iox2::Subscriber<detail::IpcService, Payload, Header>;
    using NativeFactory = iox2::PortFactoryPublishSubscribe<detail::IpcService, Payload, Header>;
    using NativeSample = iox2::Sample<detail::IpcService, Payload, Header>;
    using NativeListener = iox2::Listener<detail::IpcService>;

    static_assert(std::is_standard_layout_v<Header>, "slice stream header must be standard-layout");
    static_assert(std::is_trivially_copyable_v<Header>, "slice stream header must be trivially copyable");
    static_assert(std::is_standard_layout_v<Element>, "slice stream element must be standard-layout");
    static_assert(std::is_trivially_copyable_v<Element>, "slice stream element must be trivially copyable");

  public:
    static Result<SliceSubscriber> create(const StreamDescriptor& stream,
                                          const MessageTypeDescriptor& expected) {
        return create(stream, expected, stream.resources);
    }

    static Result<SliceSubscriber> create(const StreamDescriptor& stream,
                                          const MessageTypeDescriptor& expected,
                                          StreamResourceConfig resources) {
        StreamResourceConfig effective;
        if (const auto error = detail::resolve_subscriber_resources(stream, resources, effective);
            error.has_value()) {
            return Result<SliceSubscriber>(*error);
        }
        const auto& binding = SliceMessageBinding<Header, Element>::descriptor();
        if (!descriptors_equal(expected, binding)) {
            return Result<SliceSubscriber>(StreamError::AbiMismatch);
        }
        // This check deliberately precedes ServiceName, Node, and transport open.
        if (const auto mismatch = preflight(stream, binding); mismatch.has_value()) {
            return Result<SliceSubscriber>(*mismatch);
        }

        auto name = detail::make_service_name(stream);
        if (!name) {
            return Result<SliceSubscriber>(name.error());
        }
        auto node = detail::make_node();
        if (!node) {
            return Result<SliceSubscriber>(node.error());
        }

        auto readiness_name = detail::make_readiness_service_name(stream);
        if (!readiness_name) {
            return Result<SliceSubscriber>(readiness_name.error());
        }
        auto readiness_event = node.value().service_builder(readiness_name.value()).event();
        auto readiness_factory = std::move(readiness_event).open();
        if (!readiness_factory.has_value()) {
            return Result<SliceSubscriber>(StreamError::ReadinessOpen);
        }
        auto readiness_listener = std::move(readiness_factory).value().listener_builder().create();
        if (!readiness_listener.has_value()) {
            return Result<SliceSubscriber>(StreamError::ReadinessListener);
        }

        auto service_builder = node.value().service_builder(name.value());
        auto publish_subscribe =
            std::move(service_builder).publish_subscribe<Payload>().template user_header<Header>();
        const auto semantic = detail::semantic_config(stream.profile);
        const auto advertised = stream.resources;
        std::move(publish_subscribe).enable_safe_overflow(semantic.safe_overflow);
        std::move(publish_subscribe).subscriber_max_buffer_size(semantic.queue_depth);
        std::move(publish_subscribe).subscriber_max_borrowed_samples(advertised.max_borrowed);
        std::move(publish_subscribe).history_size(semantic.history_size);
        auto factory = std::move(publish_subscribe).open();
        if (!factory.has_value()) {
            return Result<SliceSubscriber>(StreamError::Transport);
        }

        auto subscriber_builder = factory.value().subscriber_builder();
        std::move(subscriber_builder).buffer_size(semantic.queue_depth);
        auto subscriber = std::move(subscriber_builder).create();
        if (!subscriber.has_value()) {
            return Result<SliceSubscriber>(StreamError::Transport);
        }

        SliceSubscriber result(std::move(node).value(),
                               std::move(factory).value(),
                               std::move(subscriber).value(),
                               std::move(readiness_listener).value(),
                               effective,
                               detail::PressureState::subscriber(
                                   stream.service_name, stream.profile, semantic, effective),
                               stream.profile);
        return Result<SliceSubscriber>(std::move(result));
    }

    static Result<SliceSubscriber> create_with_resources(const StreamDescriptor& stream,
                                                         const MessageTypeDescriptor& expected,
                                                         StreamResourceConfig resources) {
        return create(stream, expected, resources);
    }

    static Result<SliceSubscriber> create_with_limits(const StreamDescriptor& stream,
                                                       const MessageTypeDescriptor& expected,
                                                       StreamResourceConfig resources) {
        return create(stream, expected, resources);
    }

    SliceSubscriber(const SliceSubscriber&) = delete;
    SliceSubscriber& operator=(const SliceSubscriber&) = delete;
    SliceSubscriber(SliceSubscriber&& other) noexcept
        : node_(std::move(other.node_))
        , factory_(std::move(other.factory_))
        , listener_(std::move(other.listener_))
        , endpoint_(std::move(other.endpoint_))
        , resources_(other.resources_)
        , pressure_(std::move(other.pressure_))
        , profile_(other.profile_) {
        other.endpoint_.reset();
        other.listener_.reset();
        other.factory_.reset();
        other.node_.reset();
        other.pressure_.reset();
    }

    SliceSubscriber& operator=(SliceSubscriber&& other) noexcept {
        if (this != &other) {
            endpoint_ = std::move(other.endpoint_);
            listener_ = std::move(other.listener_);
            factory_ = std::move(other.factory_);
            node_ = std::move(other.node_);
            resources_ = other.resources_;
            pressure_ = std::move(other.pressure_);
            profile_ = other.profile_;

            other.endpoint_.reset();
            other.listener_.reset();
            other.factory_.reset();
            other.node_.reset();
            other.pressure_.reset();
        }
        return *this;
    }
    ~SliceSubscriber() = default;

    Result<std::optional<ReceivedSliceSample<Header, Element>>> receive() const {
        if (!endpoint_.has_value()) {
            return Result<std::optional<ReceivedSliceSample<Header, Element>>>(StreamError::Transport);
        }
        pressure_->receive_attempted();
        auto reservation = pressure_->reserve_borrow();
        if (!reservation.has_value()) {
            pressure_->borrow_failed();
            return Result<std::optional<ReceivedSliceSample<Header, Element>>>(StreamError::BorrowLimitReached);
        }
        auto result = endpoint_->receive();
        if (!result.has_value()) {
            if (result.error() == iox2::ReceiveError::ExceedsMaxBorrows) {
                pressure_->borrow_failed();
            }
            return Result<std::optional<ReceivedSliceSample<Header, Element>>>(
                detail::map_receive_error(result.error()));
        }
        auto samples = std::move(result).value();
        if (!samples.has_value()) {
            return Result<std::optional<ReceivedSliceSample<Header, Element>>>(
                std::optional<ReceivedSliceSample<Header, Element>> {});
        }
        ReceivedSliceSample<Header, Element> received_sample(
            std::move(samples).value(), std::move(*reservation).commit(), node_);
        std::optional<ReceivedSliceSample<Header, Element>> sample(std::move(received_sample));
        return Result<std::optional<ReceivedSliceSample<Header, Element>>>(std::move(sample));
    }

    [[nodiscard]] EventSource event_source(WaitToken token) const& {
        assert(listener_.has_value());
        return EventSource(token, listener_.value());
    }

    EventSource event_source(WaitToken) const&& = delete;

    [[nodiscard]] EventSource wait_source(WaitToken token) const& {
        return event_source(token);
    }

    EventSource wait_source(WaitToken) const&& = delete;

    [[nodiscard]] StreamPressure pressure() const {
        return pressure_ != nullptr ? pressure_->snapshot() : StreamPressure {};
    }

    [[nodiscard]] std::uint64_t buffer_size() const noexcept {
        assert(endpoint_.has_value());
        if (!endpoint_.has_value()) {
            return 0U;
        }
        return endpoint_->buffer_size();
    }

  private:
    SliceSubscriber(iox2::Node<detail::IpcService>&& node,
                    NativeFactory&& factory,
                    NativeSubscriber&& endpoint,
                    NativeListener&& listener,
                    StreamResourceConfig resources,
                    std::shared_ptr<detail::PressureState>&& pressure,
                    StreamProfile profile)
        : node_(std::make_shared<iox2::Node<detail::IpcService>>(std::move(node)))
        , factory_(std::in_place, std::move(factory))
        , listener_(std::in_place, std::move(listener))
        , endpoint_(std::in_place, std::move(endpoint))
        , resources_(resources)
        , pressure_(std::move(pressure))
        , profile_(profile) {
    }

    std::shared_ptr<iox2::Node<detail::IpcService>> node_;
    std::optional<NativeFactory> factory_;
    mutable std::optional<NativeListener> listener_;
    std::optional<NativeSubscriber> endpoint_;
    StreamResourceConfig resources_;
    std::shared_ptr<detail::PressureState> pressure_;
    StreamProfile profile_;

    friend class ReceivedSliceSample<Header, Element>;
};

/// Mutable header-plus-slice guard backed by exactly one native loan.
template <typename Header, typename Element>
class LoanedSliceSample {
    using Payload = iox2::bb::Slice<Element>;
    using NativeSample = iox2::SampleMut<detail::IpcService, Payload, Header>;

    static_assert(std::is_standard_layout_v<Header>, "slice stream header must be standard-layout");
    static_assert(std::is_trivially_copyable_v<Header>, "slice stream header must be trivially copyable");
    static_assert(std::is_standard_layout_v<Element>, "slice stream element must be standard-layout");
    static_assert(std::is_trivially_copyable_v<Element>, "slice stream element must be trivially copyable");

  public:
    LoanedSliceSample(const LoanedSliceSample&) = delete;
    LoanedSliceSample& operator=(const LoanedSliceSample&) = delete;
    LoanedSliceSample(LoanedSliceSample&& other) noexcept
        : node_(std::move(other.node_))
        , sample_(std::move(other.sample_))
        , pressure_(std::move(other.pressure_))
        , loan_(std::move(other.loan_))
        , notifier_(std::move(other.notifier_))
        , profile_(other.profile_) {
        other.sample_.reset();
    }

    LoanedSliceSample& operator=(LoanedSliceSample&& other) noexcept {
        if (this != &other) {
            loan_ = std::move(other.loan_);
            sample_ = std::move(other.sample_);
            profile_ = other.profile_;
            pressure_ = std::move(other.pressure_);
            notifier_ = std::move(other.notifier_);
            node_ = std::move(other.node_);
            other.sample_.reset();
        }
        return *this;
    }
    ~LoanedSliceSample() = default;

    Header& header() {
        assert(sample_.has_value());
        return sample_->user_header_mut();
    }

    const Header& header() const {
        assert(sample_.has_value());
        return sample_->user_header();
    }

    std::span<Element> payload() {
        assert(sample_.has_value());
        auto payload = sample_->payload_mut();
        return std::span<Element>(payload.data(), static_cast<std::size_t>(payload.number_of_elements()));
    }

    std::span<const Element> payload() const {
        assert(sample_.has_value());
        const auto payload = sample_->payload();
        return std::span<const Element>(payload.data(), static_cast<std::size_t>(payload.number_of_elements()));
    }

    Result<std::size_t> send() {
        if (!sample_.has_value()) {
            return Result<std::size_t>(StreamError::Transport);
        }
        if (pressure_ != nullptr) {
            pressure_->send_attempted();
        }
        // Sending transfers application authority to the transport before
        // the native allocation can be reused by another publisher thread.
        loan_.release();
        auto sent = iox2::send(std::move(sample_).value());
        const bool readiness_notified = detail::notify_readiness(notifier_);
        if (!readiness_notified && pressure_ != nullptr) {
            pressure_->readiness_notify_failed();
        }
        sample_.reset();
        if (!sent.has_value()) {
            if (pressure_ != nullptr) {
                pressure_->send_failed(sent.error() == iox2::SendError::UnableToDeliver
                                           && profile_ == StreamProfile::Control
                                       ? PressureEvent::QueueFull
                                       : PressureEvent::UnableToDeliver);
            }
            return Result<std::size_t>(detail::map_send_error(sent.error(), profile_));
        }
        if (pressure_ != nullptr) {
            pressure_->send_succeeded();
        }
        if (!readiness_notified) {
            return Result<std::size_t>(StreamError::ReadinessNotify);
        }
        return Result<std::size_t>(std::move(sent).value());
    }

  private:
    LoanedSliceSample(NativeSample&& sample,
                      StreamProfile profile,
                      const std::shared_ptr<detail::PressureState>& pressure,
                      const std::shared_ptr<iox2::Notifier<detail::IpcService>>& notifier,
                      std::uint64_t payload_bytes,
                      std::shared_ptr<iox2::Node<detail::IpcService>> node)
        : node_(std::move(node))
        , sample_(std::in_place, std::move(sample))
        , pressure_(pressure)
        , loan_(pressure != nullptr ? pressure->loan_succeeded(payload_bytes) : detail::LoanTracker {})
        , notifier_(notifier)
        , profile_(profile) {
    }

    // Declared first, destroyed last: a sample keeps its port alive, and a
    // node destroyed before its ports cannot remove its node directory.
    std::shared_ptr<iox2::Node<detail::IpcService>> node_;
    // Declare the native sample before LoanTracker so LoanTracker is
    // destroyed first; the pressure count reaches zero before native loan reuse.
    std::optional<NativeSample> sample_;
    std::shared_ptr<detail::PressureState> pressure_;
    detail::LoanTracker loan_;
    std::shared_ptr<iox2::Notifier<detail::IpcService>> notifier_;
    StreamProfile profile_;

    friend class SlicePublisher<Header, Element>;
};

/// Received header-plus-slice guard.  payload() points directly into SHM.
template <typename Header, typename Element>
class ReceivedSliceSample {
    using Payload = iox2::bb::Slice<Element>;
    using NativeSample = iox2::Sample<detail::IpcService, Payload, Header>;

    static_assert(std::is_standard_layout_v<Header>, "slice stream header must be standard-layout");
    static_assert(std::is_trivially_copyable_v<Header>, "slice stream header must be trivially copyable");
    static_assert(std::is_standard_layout_v<Element>, "slice stream element must be standard-layout");
    static_assert(std::is_trivially_copyable_v<Element>, "slice stream element must be trivially copyable");

  public:
    ReceivedSliceSample(const ReceivedSliceSample&) = delete;
    ReceivedSliceSample& operator=(const ReceivedSliceSample&) = delete;
    ReceivedSliceSample(ReceivedSliceSample&& other) noexcept
        : node_(std::move(other.node_))
        , borrow_(std::move(other.borrow_))
        , sample_(std::move(other.sample_)) {
        other.sample_.reset();
    }

    ReceivedSliceSample& operator=(ReceivedSliceSample&& other) noexcept {
        if (this != &other) {
            sample_ = std::move(other.sample_);
            borrow_ = std::move(other.borrow_);
            node_ = std::move(other.node_);
            other.sample_.reset();
        }
        return *this;
    }
    ~ReceivedSliceSample() = default;

    [[nodiscard]] const Header& header() const {
        assert(sample_.has_value());
        return sample_->user_header();
    }

    [[nodiscard]] std::span<const Element> payload() const {
        assert(sample_.has_value());
        const auto payload = sample_->payload();
        return std::span<const Element>(payload.data(), static_cast<std::size_t>(payload.number_of_elements()));
    }

  private:
    ReceivedSliceSample(NativeSample&& sample,
                        detail::BorrowTracker&& borrow,
                        std::shared_ptr<iox2::Node<detail::IpcService>> node)
        : node_(std::move(node))
        , borrow_(std::move(borrow))
        , sample_(std::in_place, std::move(sample)) {
    }

    // Declared first, destroyed last: a sample keeps its port alive, and a
    // node destroyed before its ports cannot remove its node directory.
    std::shared_ptr<iox2::Node<detail::IpcService>> node_;
    detail::BorrowTracker borrow_;
    // Declared after borrow_ so native Drop releases the borrow before the counter.
    std::optional<NativeSample> sample_;

    friend class SliceSubscriber<Header, Element>;
};

} // namespace rbt

#endif // RBT_CPP_STREAM_HPP
