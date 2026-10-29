#ifndef RBT_CPP_WAIT_HPP
#define RBT_CPP_WAIT_HPP

#include <iox2/iceoryx2.hpp>

#include <rbt/runtime.hpp>
#include <rbt/stream.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace rbt {

/// The maximum number of payload receive attempts a source handler should make
/// before returning to the native wait set.
inline constexpr std::size_t MAX_DRAIN_PER_SOURCE = 64U;

/// Identifies why a readiness event was dispatched.
enum class ReadyEventKind : std::uint8_t {
    Source,
    Cancellation,
};

/// A payload-free readiness notification delivered to a wait handler.
struct ReadyEvent {
    WaitToken token;
    ReadyEventKind kind { ReadyEventKind::Source };
    bool synthetic { false };

    [[nodiscard]] static constexpr ReadyEvent source(WaitToken source_token) noexcept {
        return ReadyEvent { source_token, ReadyEventKind::Source, false };
    }

    [[nodiscard]] static constexpr ReadyEvent synthetic_source(WaitToken source_token) noexcept {
        return ReadyEvent { source_token, ReadyEventKind::Source, true };
    }

    [[nodiscard]] static constexpr ReadyEvent cancellation() noexcept {
        return ReadyEvent { WaitToken::cancellation(), ReadyEventKind::Cancellation, false };
    }

    [[nodiscard]] constexpr WaitToken token_value() const noexcept {
        return token;
    }

    [[nodiscard]] constexpr WaitToken source_token() const noexcept {
        return token;
    }

    [[nodiscard]] constexpr ReadyEventKind event_kind() const noexcept {
        return kind;
    }

    [[nodiscard]] constexpr bool is_cancellation() const noexcept {
        return kind == ReadyEventKind::Cancellation;
    }

    [[nodiscard]] constexpr bool is_cancelled() const noexcept {
        return is_cancellation();
    }

    [[nodiscard]] constexpr bool is_synthetic() const noexcept {
        return synthetic;
    }

    friend constexpr bool operator==(const ReadyEvent&, const ReadyEvent&) = default;
};

/// Progress reported by one source handler dispatch.
class WaitProgress {
  public:
    enum class Kind : std::uint8_t {
        Continue,
        Stop,
        Samples,
        Empty,
        SamplesAndEmpty,
        StopAfterSamples,
    };

    constexpr WaitProgress() noexcept = default;

    /// Continue waiting without additional local accounting.
    static const WaitProgress Continue;
    /// Stop the wait loop normally.
    static const WaitProgress Stop;
    /// Continue after an empty/spurious source wake.
    static const WaitProgress Empty;

    [[nodiscard]] static constexpr WaitProgress continue_waiting() noexcept {
        return WaitProgress(Kind::Continue, 0U, 0U);
    }

    [[nodiscard]] static constexpr WaitProgress stop() noexcept {
        return WaitProgress(Kind::Stop, 0U, 0U);
    }

    [[nodiscard]] static constexpr WaitProgress stop_after_samples(std::size_t count) noexcept {
        return WaitProgress(Kind::StopAfterSamples, count, 0U);
    }

    /// Rust-parity spelling for a bounded normal stop after accounting for
    /// the given number of payload samples.
    [[nodiscard]] static constexpr WaitProgress StopAfterSamples(std::size_t count) noexcept {
        return stop_after_samples(count);
    }

    [[nodiscard]] static constexpr WaitProgress samples(std::size_t count) noexcept {
        return WaitProgress(Kind::Samples, count, 0U);
    }

    [[nodiscard]] static constexpr WaitProgress empty() noexcept {
        return WaitProgress(Kind::Empty, 0U, 1U);
    }

    [[nodiscard]] static constexpr WaitProgress samples_and_empty(std::size_t samples_count,
                                                                  std::size_t empty_count) noexcept {
        return WaitProgress(Kind::SamplesAndEmpty, samples_count, empty_count);
    }

    [[nodiscard]] constexpr Kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::size_t samples_processed() const noexcept {
        return kind_ == Kind::Samples || kind_ == Kind::SamplesAndEmpty || kind_ == Kind::StopAfterSamples
                   ? samples_
                   : 0U;
    }

    [[nodiscard]] constexpr std::size_t empty_wakeups() const noexcept {
        return kind_ == Kind::Empty || kind_ == Kind::SamplesAndEmpty ? empty_wakeups_ : 0U;
    }

    [[nodiscard]] constexpr bool should_stop() const noexcept {
        return kind_ == Kind::Stop || kind_ == Kind::StopAfterSamples;
    }

    friend constexpr bool operator==(const WaitProgress&, const WaitProgress&) = default;

  private:
    constexpr WaitProgress(Kind progress_kind,
                           std::size_t samples_count,
                           std::size_t empty_count) noexcept
        : kind_(progress_kind)
        , samples_(samples_count)
        , empty_wakeups_(empty_count) {
    }

    Kind kind_ { Kind::Continue };
    std::size_t samples_ { 0U };
    std::size_t empty_wakeups_ { 0U };
};

inline constexpr WaitProgress WaitProgress::Continue = WaitProgress::continue_waiting();
inline constexpr WaitProgress WaitProgress::Stop = WaitProgress::stop();
inline constexpr WaitProgress WaitProgress::Empty = WaitProgress::empty();

inline constexpr WaitProgress wait_progress_from_size(std::size_t count) noexcept {
    return WaitProgress::samples(count);
}

/// Why a wait context could not create, attach, or consume an event source.
enum class WaitError : std::uint8_t {
    Node,
    ServiceName,
    EventOpenOrCreate,
    EventOpen,
    Listener,
    Notifier,
    ListenerWait,
    WaitSetCreate,
    WaitSetAttach,
    WaitSetRun,
    DuplicateToken,
    Notify,
    Handler,
};

/// Why a blocking wait loop exited.
enum class WaitExit : std::uint8_t {
    Cancelled,
    Stopped,
};

[[nodiscard]] constexpr bool is_cancelled(WaitExit exit) noexcept {
    return exit == WaitExit::Cancelled;
}

[[nodiscard]] constexpr const char* to_string(WaitError error) noexcept {
    switch (error) {
    case WaitError::Node:
        return "Node";
    case WaitError::ServiceName:
        return "ServiceName";
    case WaitError::EventOpenOrCreate:
        return "EventOpenOrCreate";
    case WaitError::EventOpen:
        return "EventOpen";
    case WaitError::Listener:
        return "Listener";
    case WaitError::Notifier:
        return "Notifier";
    case WaitError::ListenerWait:
        return "ListenerWait";
    case WaitError::WaitSetCreate:
        return "WaitSetCreate";
    case WaitError::WaitSetAttach:
        return "WaitSetAttach";
    case WaitError::WaitSetRun:
        return "WaitSetRun";
    case WaitError::DuplicateToken:
        return "DuplicateToken";
    case WaitError::Notify:
        return "Notify";
    case WaitError::Handler:
        return "Handler";
    }
    return "Handler";
}

/// A small expected-style result for the experimental wait API.
template <typename T>
class WaitResult {
  public:
    explicit WaitResult(T value)
        : value_(std::move(value)) {
    }

    explicit WaitResult(WaitError error)
        : value_(error) {
    }

    WaitResult(const WaitResult&) = delete;
    WaitResult& operator=(const WaitResult&) = delete;
    WaitResult(WaitResult&&) noexcept = default;
    WaitResult& operator=(WaitResult&&) noexcept = default;
    ~WaitResult() = default;

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(value_);
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    T& value() & {
        return std::get<T>(value_);
    }

    const T& value() const& {
        return std::get<T>(value_);
    }

    T&& value() && {
        return std::get<T>(std::move(value_));
    }

    [[nodiscard]] WaitError error() const {
        return std::get<WaitError>(value_);
    }

  private:
    std::variant<T, WaitError> value_;
};

template <>
class WaitResult<void> {
  public:
    WaitResult() noexcept = default;

    explicit WaitResult(WaitError error) noexcept
        : error_(error) {
    }

    WaitResult(const WaitResult&) = default;
    WaitResult& operator=(const WaitResult&) = default;
    WaitResult(WaitResult&&) noexcept = default;
    WaitResult& operator=(WaitResult&&) noexcept = default;
    ~WaitResult() = default;

    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    void value() const noexcept {
    }

    [[nodiscard]] WaitError error() const {
        return error_.value_or(WaitError::Handler);
    }

  private:
    std::optional<WaitError> error_;
};

/// A point-in-time copy of one wait context's counters.
struct WaitStatsSnapshot {
    std::uint64_t waits { 0U };
    std::uint64_t wakeups { 0U };
    // Source wake dispatches that produced no payload sample.
    std::uint64_t empty_wakeups { 0U };
    std::uint64_t cancellation_wakeups { 0U };
    std::uint64_t samples_processed { 0U };

    [[nodiscard]] constexpr std::uint64_t cancellation_wakes() const noexcept {
        return cancellation_wakeups;
    }

    [[nodiscard]] constexpr std::uint64_t samples() const noexcept {
        return samples_processed;
    }

    friend constexpr bool operator==(const WaitStatsSnapshot&, const WaitStatsSnapshot&) = default;
};

namespace detail {

struct WaitStatsState {
    std::atomic<std::uint64_t> waits { 0U };
    std::atomic<std::uint64_t> wakeups { 0U };
    std::atomic<std::uint64_t> empty_wakeups { 0U };
    std::atomic<std::uint64_t> cancellation_wakeups { 0U };
    std::atomic<std::uint64_t> samples_processed { 0U };
};

inline std::shared_ptr<WaitStatsState> make_wait_stats_state() {
    return std::make_shared<WaitStatsState>();
}

} // namespace detail

/// Relaxed local counters for one wait context.
class WaitStats {
  public:
    WaitStats()
        : state_(detail::make_wait_stats_state()) {
    }

    [[nodiscard]] WaitStatsSnapshot snapshot() const noexcept {
        return WaitStatsSnapshot { state_->waits.load(std::memory_order_relaxed),
                                   state_->wakeups.load(std::memory_order_relaxed),
                                   state_->empty_wakeups.load(std::memory_order_relaxed),
                                   state_->cancellation_wakeups.load(std::memory_order_relaxed),
                                   state_->samples_processed.load(std::memory_order_relaxed) };
    }

    [[nodiscard]] std::uint64_t waits() const noexcept {
        return snapshot().waits;
    }

    [[nodiscard]] std::uint64_t wakeups() const noexcept {
        return snapshot().wakeups;
    }

    [[nodiscard]] std::uint64_t empty_wakeups() const noexcept {
        return snapshot().empty_wakeups;
    }

    [[nodiscard]] std::uint64_t cancellation_wakeups() const noexcept {
        return snapshot().cancellation_wakeups;
    }

    [[nodiscard]] std::uint64_t cancellation_wakes() const noexcept {
        return cancellation_wakeups();
    }

    [[nodiscard]] std::uint64_t samples_processed() const noexcept {
        return snapshot().samples_processed;
    }

    [[nodiscard]] std::uint64_t samples() const noexcept {
        return samples_processed();
    }

  private:
    explicit WaitStats(std::shared_ptr<detail::WaitStatsState> state)
        : state_(std::move(state)) {
    }

    void increment_waits() const noexcept {
        state_->waits.fetch_add(1U, std::memory_order_relaxed);
    }

    void increment_wakeups() const noexcept {
        state_->wakeups.fetch_add(1U, std::memory_order_relaxed);
    }

    void increment_empty_wakeups(std::size_t count) const noexcept {
        state_->empty_wakeups.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
    }

    void increment_cancellation_wakeups() const noexcept {
        state_->cancellation_wakeups.fetch_add(1U, std::memory_order_relaxed);
    }

    void increment_samples(std::size_t count) const noexcept {
        state_->samples_processed.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
    }

    std::shared_ptr<detail::WaitStatsState> state_;

    friend class WaitContext;
};

struct WaitWakeState {
    // A wake handle may outlive its WaitContext while activation teardown
    // finishes joining the blocked task.  Keep the owning node alive and
    // declare it before the notifier so C++ reverse destruction order drops
    // the notifier first.
    std::shared_ptr<iox2::Node<detail::IpcService>> node;
    std::shared_ptr<iox2::Notifier<detail::IpcService>> notifier;
    std::atomic<bool> cancelled { false };
};

/// A clonable activation cancellation handle.
class WaitWakeHandle {
  public:
    WaitWakeHandle() = default;

    [[nodiscard]] WaitResult<void> wake() const {
        if (state_ == nullptr || state_->notifier == nullptr) {
            return WaitResult<void>(WaitError::Notify);
        }
        state_->cancelled.store(true, std::memory_order_release);
        if (!state_->notifier->notify().has_value()) {
            return WaitResult<void>(WaitError::Notify);
        }
        return WaitResult<void>();
    }

    [[nodiscard]] WaitResult<void> cancel() const {
        return wake();
    }

    [[nodiscard]] WaitResult<void> request_stop() const {
        return wake();
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_ != nullptr && state_->cancelled.load(std::memory_order_acquire);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return state_ != nullptr;
    }

  private:
    explicit WaitWakeHandle(std::shared_ptr<WaitWakeState> state)
        : state_(std::move(state)) {
    }

    std::shared_ptr<WaitWakeState> state_;

    friend class WaitContext;
};

namespace detail {

inline WaitResult<iox2::ServiceName> cancellation_service_name(const std::string& process_instance,
                                                               ActivationId activation) {
    std::string name = "rbt/cancel/" + process_instance + "/" + std::to_string(activation);
    auto result = iox2::ServiceName::create(name.c_str());
    if (!result.has_value()) {
        return WaitResult<iox2::ServiceName>(WaitError::ServiceName);
    }
    return WaitResult<iox2::ServiceName>(std::move(result).value());
}

template <typename Return>
inline WaitProgress normalize_wait_progress(Return&& value) {
    using Value = std::remove_cvref_t<Return>;
    if constexpr (std::is_same_v<Value, WaitProgress>) {
        return std::forward<Return>(value);
    } else if constexpr (std::is_same_v<Value, std::size_t>) {
        return WaitProgress::samples(value);
    } else if constexpr (std::is_same_v<Value, WaitResult<WaitProgress>>) {
        if (value.has_value()) {
            return std::move(value).value();
        }
        return WaitProgress::stop();
    } else if constexpr (std::is_same_v<Value, WaitResult<void>>) {
        return value.has_value() ? WaitProgress::continue_waiting() : WaitProgress::stop();
    } else {
        static_assert(std::is_same_v<Value, WaitProgress>,
                      "wait handler must return WaitProgress, std::size_t, or WaitResult<WaitProgress>");
    }
}

template <typename Handler>
inline WaitProgress invoke_wait_handler(Handler& handler,
                                        ReadyEvent event,
                                        std::size_t budget,
                                        std::optional<WaitError>& error) {
    try {
        using Return = std::invoke_result_t<Handler&, ReadyEvent, std::size_t>;
        if constexpr (std::is_void_v<Return>) {
            std::invoke(handler, event, budget);
            return WaitProgress::continue_waiting();
        } else if constexpr (std::is_same_v<std::remove_cvref_t<Return>, WaitResult<WaitProgress>>
                             || std::is_same_v<std::remove_cvref_t<Return>, WaitResult<void>>) {
            auto result = std::invoke(handler, event, budget);
            if (!result.has_value()) {
                error = result.error();
                return WaitProgress::stop();
            }
            return normalize_wait_progress(std::move(result));
        } else {
            return normalize_wait_progress(std::invoke(handler, event, budget));
        }
    } catch (...) {
        // Never allow an exception to cross the native C callback boundary.
        // Critical component code maps this typed error through its existing
        // CriticalTaskExited supervision path.
        error = WaitError::Handler;
        return WaitProgress::stop();
    }
}

} // namespace detail

/// A blocking WaitSet context for one activation.
class WaitContext {
  public:
    static WaitResult<WaitContext> create(ProcessInstanceId process_instance,
                                          ActivationId activation) {
        iox2::NodeBuilder builder;
        auto node = std::move(builder)
                        .signal_handling_mode(iox2::SignalHandlingMode::Disabled)
                        .create<detail::IpcService>();
        if (!node.has_value()) {
            return WaitResult<WaitContext>(WaitError::Node);
        }

        const auto service_name = detail::cancellation_service_name(process_instance, activation);
        if (!service_name.has_value()) {
            return WaitResult<WaitContext>(service_name.error());
        }
        auto event_builder = node.value().service_builder(service_name.value()).event();
        auto event = std::move(event_builder).open_or_create();
        if (!event.has_value()) {
            return WaitResult<WaitContext>(WaitError::EventOpenOrCreate);
        }

        auto& event_factory = event.value();
        auto listener = event_factory.listener_builder().create();
        if (!listener.has_value()) {
            return WaitResult<WaitContext>(WaitError::Listener);
        }
        auto notifier = event_factory.notifier_builder().create();
        if (!notifier.has_value()) {
            return WaitResult<WaitContext>(WaitError::Notifier);
        }

        auto wake_state = std::make_shared<WaitWakeState>();
        wake_state->node = std::make_shared<iox2::Node<detail::IpcService>>(
            std::move(node).value());
        wake_state->notifier = std::make_shared<iox2::Notifier<detail::IpcService>>(
            std::move(notifier).value());
        return WaitResult<WaitContext>(WaitContext(std::move(process_instance),
                                                    activation,
                                                    std::move(listener).value(),
                                                    std::move(wake_state)));
    }

    WaitContext(const WaitContext&) = delete;
    WaitContext& operator=(const WaitContext&) = delete;
    // Keep native listener teardown ahead of wake-state teardown.  The member
    // declarations preserve that order for destruction, but default move
    // assignment replaces wake_state_ first.
    WaitContext(WaitContext&& other) noexcept
        : process_instance_(std::move(other.process_instance_))
        , activation_(other.activation_)
        , wake_state_(std::move(other.wake_state_))
        , cancellation_listener_(std::move(other.cancellation_listener_))
        , stats_(std::move(other.stats_)) {
        other.cancellation_listener_.reset();
        other.wake_state_.reset();
    }

    WaitContext& operator=(WaitContext&& other) noexcept {
        if (this != &other) {
            cancellation_listener_.reset();
            wake_state_.reset();

            process_instance_ = std::move(other.process_instance_);
            activation_ = other.activation_;
            wake_state_ = std::move(other.wake_state_);
            cancellation_listener_ = std::move(other.cancellation_listener_);
            stats_ = std::move(other.stats_);

            other.cancellation_listener_.reset();
            other.wake_state_.reset();
        }
        return *this;
    }
    ~WaitContext() = default;

    [[nodiscard]] const ProcessInstanceId& process_instance() const noexcept {
        return process_instance_;
    }

    [[nodiscard]] ActivationId activation() const noexcept {
        return activation_;
    }

    [[nodiscard]] ActivationId activation_id() const noexcept {
        return activation_;
    }

    [[nodiscard]] WaitToken cancellation_token() const noexcept {
        return WaitToken::cancellation();
    }

    [[nodiscard]] WaitWakeHandle wake_handle() const {
        return WaitWakeHandle(wake_state_);
    }

    [[nodiscard]] WaitWakeHandle wait_wake_handle() const {
        return wake_handle();
    }

    [[nodiscard]] WaitStats stats() const {
        return stats_;
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return wake_state_ != nullptr && wake_state_->cancelled.load(std::memory_order_acquire);
    }

    [[nodiscard]] WaitResult<void> cancel() const {
        return wake_handle().wake();
    }

    template <typename Handler>
    WaitResult<WaitExit> run(const std::vector<EventSource>& sources,
                             Handler&& handler) {
        return run_impl(std::stop_token {}, sources, std::forward<Handler>(handler));
    }

    template <typename Handler>
    WaitResult<WaitExit> run(std::initializer_list<EventSource> sources,
                             Handler&& handler) {
        return run_impl(std::stop_token {}, sources, std::forward<Handler>(handler));
    }

    template <typename Handler>
    WaitResult<WaitExit> run(std::stop_token stop_token,
                             const std::vector<EventSource>& sources,
                             Handler&& handler) {
        return run_impl(stop_token, sources, std::forward<Handler>(handler));
    }

    template <typename Handler>
    WaitResult<WaitExit> run(const std::vector<EventSource>& sources,
                             std::stop_token stop_token,
                             Handler&& handler) {
        return run_impl(stop_token, sources, std::forward<Handler>(handler));
    }

    template <typename Handler>
    WaitResult<WaitExit> run(std::initializer_list<EventSource> sources,
                             std::stop_token stop_token,
                             Handler&& handler) {
        return run_impl(stop_token, sources, std::forward<Handler>(handler));
    }

    template <typename Handler>
    WaitResult<WaitExit> run(std::stop_token stop_token,
                             std::initializer_list<EventSource> sources,
                             Handler&& handler) {
        return run_impl(stop_token, sources, std::forward<Handler>(handler));
    }

  private:
    WaitContext(ProcessInstanceId process_instance,
                ActivationId activation,
                iox2::Listener<detail::IpcService>&& cancellation_listener,
                std::shared_ptr<WaitWakeState>&& wake_state)
        : process_instance_(std::move(process_instance))
        , activation_(activation)
        , wake_state_(std::move(wake_state))
        , cancellation_listener_(std::move(cancellation_listener))
        , stats_() {
    }

    template <typename Range, typename Handler>
    WaitResult<WaitExit> run_impl(std::stop_token stop_token,
                                  const Range& sources,
                                  Handler&& handler) {
        std::vector<WaitToken> tokens;
        for (const auto& source : sources) {
            if (source.token() == WaitToken::cancellation()
                || std::find(tokens.begin(), tokens.end(), source.token()) != tokens.end()) {
                return WaitResult<WaitExit>(WaitError::DuplicateToken);
            }
            tokens.emplace_back(source.token());
        }

        if (is_cancelled()) {
            stats_.increment_cancellation_wakeups();
            return WaitResult<WaitExit>(WaitExit::Cancelled);
        }

        auto wake = wake_handle();
        auto stop_callback_function = [wake]() noexcept {
            const auto result = wake.wake();
            (void)result;
        };
        std::stop_callback stop_callback(stop_token, stop_callback_function);

        return run_attached(stop_token, sources, handler);
    }

    template <typename Range, typename Handler>
    WaitResult<WaitExit> run_attached(std::stop_token,
                                      const Range& sources,
                                      Handler& handler) {
        auto waitset_result = iox2::WaitSetBuilder()
                                  .signal_handling_mode(iox2::SignalHandlingMode::Disabled)
                                  .create<detail::IpcService>();
        if (!waitset_result.has_value()) {
            return WaitResult<WaitExit>(WaitError::WaitSetCreate);
        }
        auto waitset = std::move(waitset_result).value();

        std::vector<iox2::WaitSetGuard<detail::IpcService>> guards;
        guards.reserve(static_cast<std::size_t>(std::distance(std::begin(sources), std::end(sources))));
        for (const auto& source : sources) {
            auto guard = waitset.attach_notification(source.listener());
            if (!guard.has_value()) {
                return WaitResult<WaitExit>(WaitError::WaitSetAttach);
            }
            guards.emplace_back(std::move(guard).value());
        }

        auto cancellation_guard = waitset.attach_notification(cancellation_listener_.value());
        if (!cancellation_guard.has_value()) {
            return WaitResult<WaitExit>(WaitError::WaitSetAttach);
        }
        auto cancellation = std::move(cancellation_guard).value();
        // A normal stop wakes through the cancellation event immediately.
        // This low-frequency native interval only bounds the rare notifier
        // failure path; it never performs a payload receive.
        auto liveness_guard = waitset.attach_interval(iox2::bb::Duration::from_secs(1U));
        if (!liveness_guard.has_value()) {
            return WaitResult<WaitExit>(WaitError::WaitSetAttach);
        }
        auto cancellation_liveness = std::move(liveness_guard).value();

        for (const auto& source : sources) {
            std::optional<WaitError> callback_error;
            const auto progress = detail::invoke_wait_handler(handler,
                                                               ReadyEvent::synthetic_source(source.token()),
                                                               MAX_DRAIN_PER_SOURCE,
                                                               callback_error);
            if (callback_error.has_value()) {
                return WaitResult<WaitExit>(callback_error.value());
            }
            record_progress(progress);
            if (progress.should_stop()) {
                return WaitResult<WaitExit>(WaitExit::Stopped);
            }
            if (is_cancelled()) {
                stats_.increment_cancellation_wakeups();
                return WaitResult<WaitExit>(WaitExit::Cancelled);
            }
        }

        for (;;) {
            if (is_cancelled()) {
                stats_.increment_cancellation_wakeups();
                return WaitResult<WaitExit>(WaitExit::Cancelled);
            }
            stats_.increment_waits();

            std::optional<WaitError> callback_error;
            std::optional<WaitExit> callback_exit;
            bool cancellation_recorded = false;
            const auto* sources_ptr = std::addressof(sources);
            auto* handler_ptr = std::addressof(handler);
            auto* context = this;
            auto callback = [context,
                             sources_ptr,
                             handler_ptr,
                             &guards,
                             &cancellation,
                             &cancellation_liveness,
                             &callback_error,
                             &callback_exit,
                             &cancellation_recorded](
                                iox2::WaitSetAttachmentId<detail::IpcService> attachment_id) {
                context->stats_.increment_wakeups();

                if (context->is_cancelled()) {
                    if (!cancellation_recorded) {
                        cancellation_recorded = true;
                        context->stats_.increment_cancellation_wakeups();
                    }
                    callback_exit = WaitExit::Cancelled;
                    return iox2::CallbackProgression::Stop;
                }

                if (attachment_id.has_event_from(cancellation)) {
                    bool cancellation_activated = false;
                    auto event = context->cancellation_listener_.value().try_wait(
                        [&](iox2::EventActivation activation) {
                            cancellation_activated = cancellation_activated
                                || activation.count() > 0U;
                        });
                    if (!event.has_value()) {
                        callback_error = WaitError::ListenerWait;
                        return iox2::CallbackProgression::Stop;
                    }
                    if (!cancellation_activated && event.value() == 0U) {
                        // A notification edge without a queued cancellation
                        // event is a normal spurious wake.  The stop bridge
                        // sets the flag before notifying, so a real
                        // cancellation is still observed by the check above.
                        context->stats_.increment_empty_wakeups(1U);
                        return iox2::CallbackProgression::Continue;
                    }
                    if (!cancellation_recorded) {
                        cancellation_recorded = true;
                        context->stats_.increment_cancellation_wakeups();
                    }
                    context->wake_state_->cancelled.store(true, std::memory_order_release);
                    callback_exit = WaitExit::Cancelled;
                    return iox2::CallbackProgression::Stop;
                }

                if (attachment_id.has_event_from(cancellation_liveness)) {
                    return iox2::CallbackProgression::Continue;
                }

                std::size_t source_index = 0U;
                bool found = false;
                for (const auto& guard : guards) {
                    if (attachment_id.has_event_from(guard)) {
                        found = true;
                        break;
                    }
                    ++source_index;
                }
                if (!found || source_index >= static_cast<std::size_t>(std::distance(
                                             std::begin(*sources_ptr), std::end(*sources_ptr)))) {
                    return iox2::CallbackProgression::Continue;
                }

                const auto source_iterator = std::next(std::begin(*sources_ptr),
                                                       static_cast<std::ptrdiff_t>(source_index));
                const auto& source = *source_iterator;
                bool listener_activated = false;
                // Consume coalesced activations as one readiness dispatch;
                // the handler receives the fixed per-source drain budget.
                auto event = source.listener().try_wait(
                    [&](iox2::EventActivation activation) {
                        listener_activated = listener_activated
                            || activation.count() > 0U;
                    });
                if (!event.has_value()) {
                    callback_error = WaitError::ListenerWait;
                    return iox2::CallbackProgression::Stop;
                }
                const bool listener_empty = !listener_activated && event.value() == 0U;
                const auto progress = detail::invoke_wait_handler(*handler_ptr,
                                                                   ReadyEvent::source(source.token()),
                                                                   MAX_DRAIN_PER_SOURCE,
                                                                   callback_error);
                if (callback_error.has_value()) {
                    return iox2::CallbackProgression::Stop;
                }

                context->stats_.increment_samples(progress.samples_processed());
                // The final empty receive that terminates a productive drain
                // is not a wake-without-sample. A spurious listener edge and
                // an empty handler result still count as one dispatch.
                const auto empty_wakeups = progress.samples_processed() == 0U
                                               ? std::max(progress.empty_wakeups(),
                                                          listener_empty ? std::size_t(1U)
                                                                         : std::size_t(0U))
                                               : std::size_t(0U);
                context->stats_.increment_empty_wakeups(empty_wakeups);
                if (progress.should_stop()) {
                    callback_exit = WaitExit::Stopped;
                    return iox2::CallbackProgression::Stop;
                }
                return iox2::CallbackProgression::Continue;
            };

            iox2::bb::StaticFunction<iox2::CallbackProgression(
                iox2::WaitSetAttachmentId<detail::IpcService>)>
                static_callback(callback);
            auto wait_result = waitset.wait_and_process_once(static_callback);
            if (callback_error.has_value()) {
                return WaitResult<WaitExit>(callback_error.value());
            }
            if (!wait_result.has_value()) {
                return WaitResult<WaitExit>(WaitError::WaitSetRun);
            }
            if (callback_exit.has_value()) {
                return WaitResult<WaitExit>(callback_exit.value());
            }
        }
    }

    void record_progress(WaitProgress progress) const noexcept {
        // Synthetic probes are not native wakeups.  They may report work
        // drained during the construction race, but an Empty result here is
        // intentionally not counted as a spurious wake.
        stats_.increment_samples(progress.samples_processed());
    }

    ProcessInstanceId process_instance_;
    ActivationId activation_ { 0U };
    // Declare shared state before the listener so reverse destruction drops
    // the listener first, then the notifier/node state when no external wake
    // handle remains.
    std::shared_ptr<WaitWakeState> wake_state_;
    std::optional<iox2::Listener<detail::IpcService>> cancellation_listener_;
    WaitStats stats_;
};

} // namespace rbt

#endif // RBT_CPP_WAIT_HPP
