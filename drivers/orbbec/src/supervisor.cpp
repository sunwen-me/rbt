#include <rbt_orbbec/component.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <syncstream>
#include <system_error>
#include <time.h>
#include <thread>
#include <string>
#include <string_view>
#include <utility>

namespace rbt_orbbec {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class SignalWatcher final {
  public:
    SignalWatcher() {
        sigemptyset(&signals_);
        sigaddset(&signals_, SIGINT);
        sigaddset(&signals_, SIGTERM);

        const int blocked = pthread_sigmask(SIG_BLOCK, &signals_, &previous_mask_);
        if (blocked != 0) {
            throw std::system_error(blocked, std::generic_category(), "cannot block SIGINT/SIGTERM");
        }
        mask_saved_ = true;

        try {
            waiter_ = std::thread([this] { wait_for_signal(); });
        } catch (...) {
            (void)restore_mask();
            throw;
        }
    }

    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

    ~SignalWatcher() {
        (void)finish();
    }

    [[nodiscard]] bool cancelled() const noexcept {
        return cancelled_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int signal_error() const noexcept {
        return signal_error_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int finish() noexcept {
        stopping_.store(true, std::memory_order_release);
        if (waiter_.joinable()) {
            waiter_.join();
        }
        return restore_mask();
    }

  private:
    void wait_for_signal() noexcept {
        while (!stopping_.load(std::memory_order_acquire)) {
            timespec timeout {};
            timeout.tv_nsec = 100'000'000;
            const int signal = sigtimedwait(&signals_, nullptr, &timeout);
            if (signal == SIGINT || signal == SIGTERM) {
                cancelled_.store(true, std::memory_order_release);
                return;
            }
            if (signal < 0 && errno != EAGAIN && errno != EINTR) {
                signal_error_.store(errno, std::memory_order_release);
                return;
            }
        }
    }

    [[nodiscard]] int restore_mask() noexcept {
        if (!mask_saved_) {
            return 0;
        }
        mask_saved_ = false;
        return pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
    }

    sigset_t signals_ {};
    sigset_t previous_mask_ {};
    std::atomic<bool> cancelled_ { false };
    std::atomic<bool> stopping_ { false };
    std::atomic<int> signal_error_ { 0 };
    std::thread waiter_;
    bool mask_saved_ { false };
};

enum class AttemptStop : std::uint8_t {
    None,
    Cancelled,
    Deadline,
    Disconnected,
    Failed,
    MonitorError,
};

struct AttemptResult {
    AttemptStop stop { AttemptStop::None };
    int runtime_result { 1 };
    bool ever_active { false };
    std::string detail;
};

[[nodiscard]] std::optional<Clock::time_point> make_deadline(const Options& options) {
    if (options.duration_seconds == 0U) {
        return std::nullopt;
    }
    const auto started = Clock::now();
    const auto maximum_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        Clock::time_point::max() - started).count();
    if (maximum_seconds <= 0
        || options.duration_seconds > static_cast<std::uint64_t>(maximum_seconds)) {
        return Clock::time_point::max();
    }
    return started + std::chrono::seconds(static_cast<std::chrono::seconds::rep>(options.duration_seconds));
}

[[nodiscard]] bool deadline_reached(const std::optional<Clock::time_point>& deadline) noexcept {
    return deadline && Clock::now() >= *deadline;
}

[[nodiscard]] std::chrono::milliseconds bounded_wait(
    const std::optional<Clock::time_point>& deadline) noexcept {
    constexpr auto maximum_wait = 100ms;
    if (!deadline) {
        return maximum_wait;
    }
    const auto remaining = *deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) {
        return 0ms;
    }
    return std::min(maximum_wait, std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
}

void log_event(std::string_view event,
               const Options& options,
               std::uint64_t attempt,
               std::string_view detail = {}) {
    std::osyncstream output(std::cerr);
    output << "{\"event\":" << json_quote(event)
           << ",\"serial\":" << json_quote(options.serial)
           << ",\"component_id\":" << json_quote(options.component_id)
           << ",\"attempt\":" << attempt;
    if (!detail.empty()) {
        output << ",\"reason\":" << json_quote(detail);
    }
    output << "}" << std::endl;
}

[[nodiscard]] AttemptResult run_attempt(const Options& options,
                                        rbt::RuntimeOptions runtime_options,
                                        DeviceMonitor* monitor,
                                        std::uint64_t starting_disconnect_generation,
                                        SignalWatcher& signals,
                                        const std::optional<Clock::time_point>& deadline) {
    auto runtime = std::make_unique<rbt::ComponentRuntime>(
        make_camera_component(options), std::move(runtime_options));

    std::mutex result_mutex;
    AttemptResult result;
    std::atomic<bool> ever_active { false };

    const auto request_stop = [&](AttemptStop stop, std::string detail = {}) {
        bool should_stop = false;
        {
            std::lock_guard lock(result_mutex);
            if (result.stop == AttemptStop::None) {
                result.stop = stop;
                result.detail = std::move(detail);
                should_stop = true;
            }
        }
        if (should_stop) {
            runtime->stop();
        }
    };

    std::jthread supervisor([&](std::stop_token stop) {
        try {
            while (!stop.stop_requested()) {
                if (signals.cancelled()) {
                    request_stop(AttemptStop::Cancelled);
                    return;
                }
                if (signals.signal_error() != 0) {
                    request_stop(AttemptStop::MonitorError, "sigtimedwait failed");
                    return;
                }
                if (deadline_reached(deadline)) {
                    request_stop(AttemptStop::Deadline);
                    return;
                }

                const auto state = runtime->state();
                if (state.kind == rbt::ComponentStateKind::Active) {
                    ever_active.store(true, std::memory_order_release);
                }
                if (state.kind == rbt::ComponentStateKind::Failed) {
                    request_stop(AttemptStop::Failed, state.reason);
                    return;
                }

                if (monitor != nullptr) {
                    if (monitor->disconnect_generation() != starting_disconnect_generation) {
                        request_stop(AttemptStop::Disconnected);
                        return;
                    }
                    if (!monitor->connected()) {
                        request_stop(AttemptStop::Disconnected);
                        return;
                    }
                    const auto wait = bounded_wait(deadline);
                    if (wait > 0ms) {
                        monitor->wait_for_change(wait);
                    }
                } else {
                    std::this_thread::sleep_for(std::min(20ms, bounded_wait(deadline)));
                }
            }
        } catch (const std::exception& error) {
            request_stop(AttemptStop::MonitorError, error.what());
        } catch (...) {
            request_stop(AttemptStop::MonitorError, "unknown supervisor monitor error");
        }
    });

    try {
        result.runtime_result = runtime->run();
    } catch (const std::exception& error) {
        runtime->stop();
        std::lock_guard lock(result_mutex);
        if (result.stop == AttemptStop::None) {
            result.stop = AttemptStop::Failed;
            result.detail = error.what();
        }
    } catch (...) {
        runtime->stop();
        std::lock_guard lock(result_mutex);
        if (result.stop == AttemptStop::None) {
            result.stop = AttemptStop::Failed;
            result.detail = "unknown exception from ComponentRuntime::run";
        }
    }

    supervisor.request_stop();
    if (supervisor.joinable()) {
        supervisor.join();
    }

    const auto final_state = runtime->state();
    if (final_state.kind == rbt::ComponentStateKind::Active) {
        ever_active.store(true, std::memory_order_release);
    }
    {
        std::lock_guard lock(result_mutex);
        result.ever_active = ever_active.load(std::memory_order_acquire);
        if (result.stop == AttemptStop::None) {
            if (final_state.kind == rbt::ComponentStateKind::Failed) {
                result.stop = AttemptStop::Failed;
                result.detail = final_state.reason;
            } else if (result.runtime_result != 0) {
                result.stop = AttemptStop::Failed;
                result.detail = "ComponentRuntime::run returned " + std::to_string(result.runtime_result);
            } else {
                result.stop = AttemptStop::Failed;
                result.detail = "ComponentRuntime stopped without a supervisor request";
            }
        }
    }

    // The attempt monitor is joined before runtime destruction so its SDK
    // presence checks cannot overlap the next runtime's activation/teardown.
    runtime.reset();
    return result;
}

[[nodiscard]] int duration_result(bool ever_active) noexcept {
    return ever_active ? 0 : 2;
}

enum class RemovalConfirmation : std::uint8_t {
    Removed,
    StillPresent,
    Cancelled,
    Deadline,
    MonitorError,
};

[[nodiscard]] RemovalConfirmation confirm_removal_after_failure(
    DeviceMonitor& monitor,
    std::uint64_t starting_disconnect_generation,
    SignalWatcher& signals,
    const std::optional<Clock::time_point>& deadline) {
    const auto grace_deadline = Clock::now() + 500ms;
    for (;;) {
        if (signals.cancelled()) {
            return RemovalConfirmation::Cancelled;
        }
        if (signals.signal_error() != 0) {
            return RemovalConfirmation::MonitorError;
        }
        if (deadline_reached(deadline)) {
            return RemovalConfirmation::Deadline;
        }
        if (monitor.disconnect_generation() != starting_disconnect_generation) {
            return RemovalConfirmation::Removed;
        }
        if (!monitor.connected()) {
            return RemovalConfirmation::Removed;
        }

        const auto now = Clock::now();
        if (now >= grace_deadline) {
            return RemovalConfirmation::StillPresent;
        }
        const auto grace_remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            grace_deadline - now);
        const auto wait = std::min({ 100ms, grace_remaining, bounded_wait(deadline) });
        if (wait > 0ms) {
            monitor.wait_for_change(wait);
        }
    }
}

[[nodiscard]] rbt::RuntimeOptions runtime_options_from_environment() {
    rbt::RuntimeOptions options;
    if (const char* address = std::getenv("RBT_COMPOSER_ADDR"); address != nullptr && *address != '\0') {
        options.composer_address = address;
    }
    return options;
}

[[nodiscard]] int run_without_reconnect(const Options& options,
                                        SignalWatcher& signals,
                                        const std::optional<Clock::time_point>& deadline) {
    if (signals.cancelled()) {
        return 0;
    }
    if (deadline_reached(deadline)) {
        return duration_result(false);
    }
    auto attempt = run_attempt(options, runtime_options_from_environment(), nullptr, 0U, signals, deadline);
    if (signals.cancelled() || attempt.stop == AttemptStop::Cancelled) {
        return 0;
    }
    if (attempt.stop == AttemptStop::Deadline) {
        return duration_result(attempt.ever_active);
    }
    if (attempt.stop != AttemptStop::Failed) {
        log_event("failed", options, 1U, "unexpected stop state");
        return 1;
    }
    log_event("failed", options, 1U, attempt.detail);
    return 1;
}

[[nodiscard]] int run_with_reconnect(const Options& options,
                                     DeviceMonitor& monitor,
                                     SignalWatcher& signals,
                                     const std::optional<Clock::time_point>& deadline) {
    bool ever_active = false;
    bool have_seen_absence = false;
    bool waiting_logged = false;
    std::uint64_t attempt_number = 0U;

    for (;;) {
        if (signals.cancelled()) {
            return 0;
        }
        if (signals.signal_error() != 0) {
            log_event("failed", options, attempt_number, "sigtimedwait failed");
            return 1;
        }
        if (deadline_reached(deadline)) {
            return duration_result(ever_active);
        }

        const auto presence_generation = monitor.disconnect_generation();
        const bool selected_device_present = monitor.connected();
        if (!selected_device_present) {
            have_seen_absence = true;
            if (!waiting_logged) {
                log_event("waiting", options, attempt_number + 1U);
                waiting_logged = true;
            }
            const auto wait = bounded_wait(deadline);
            if (wait > 0ms) {
                monitor.wait_for_change(wait);
            }
            continue;
        }

        if (waiting_logged) {
            waiting_logged = false;
        }
        auto activation_generation = presence_generation;
        if (have_seen_absence) {
            auto settle_generation = presence_generation;
            auto settle_deadline = Clock::now() + std::chrono::milliseconds(options.reconnect_delay_ms);
            bool lost_during_settle = false;
            while (Clock::now() < settle_deadline) {
                if (signals.cancelled()) {
                    return 0;
                }
                if (signals.signal_error() != 0) {
                    log_event("failed", options, attempt_number, "sigtimedwait failed");
                    return 1;
                }
                if (deadline_reached(deadline)) {
                    return duration_result(ever_active);
                }
                const auto current_generation = monitor.disconnect_generation();
                const bool present = monitor.connected();
                const auto confirmed_generation = monitor.disconnect_generation();
                if (current_generation != settle_generation || confirmed_generation != current_generation) {
                    if (!waiting_logged) {
                        log_event("waiting", options, attempt_number + 1U);
                        waiting_logged = true;
                    }
                    if (!present) {
                        lost_during_settle = true;
                        break;
                    }
                    settle_generation = confirmed_generation;
                    settle_deadline = Clock::now() + std::chrono::milliseconds(options.reconnect_delay_ms);
                    continue;
                }
                if (!present) {
                    lost_during_settle = true;
                    if (!waiting_logged) {
                        log_event("waiting", options, attempt_number + 1U);
                        waiting_logged = true;
                    }
                    break;
                }
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    settle_deadline - Clock::now());
                const auto wait = std::min(bounded_wait(deadline), std::max(0ms, remaining));
                if (wait > 0ms) {
                    monitor.wait_for_change(wait);
                }
            }
            if (lost_during_settle) {
                continue;
            }
            log_event("reconnecting", options, attempt_number + 1U);
            activation_generation = settle_generation;
            have_seen_absence = false;
        }

        if (signals.cancelled()) {
            return 0;
        }
        if (deadline_reached(deadline)) {
            return duration_result(ever_active);
        }

        ++attempt_number;
        auto attempt = run_attempt(options, runtime_options_from_environment(), &monitor,
                                   activation_generation, signals, deadline);
        ever_active = ever_active || attempt.ever_active;
        if (signals.cancelled() || attempt.stop == AttemptStop::Cancelled) {
            return 0;
        }
        if (attempt.stop == AttemptStop::Deadline) {
            return duration_result(ever_active);
        }
        if (deadline_reached(deadline)) {
            return duration_result(ever_active);
        }
        if (attempt.stop == AttemptStop::MonitorError) {
            log_event("failed", options, attempt_number, attempt.detail);
            return 1;
        }

        const bool generation_changed =
            monitor.disconnect_generation() != activation_generation;
        const bool device_present_after_attempt = monitor.connected();
        if (attempt.stop == AttemptStop::Disconnected || generation_changed || !device_present_after_attempt) {
            log_event("disconnected", options, attempt_number);
            have_seen_absence = true;
            waiting_logged = false;
            continue;
        }

        if (attempt.stop == AttemptStop::Failed || attempt.runtime_result != 0) {
            const auto confirmation = confirm_removal_after_failure(
                monitor, activation_generation, signals, deadline);
            if (confirmation == RemovalConfirmation::Cancelled) {
                return 0;
            }
            if (confirmation == RemovalConfirmation::Deadline) {
                return duration_result(ever_active);
            }
            if (confirmation == RemovalConfirmation::MonitorError) {
                log_event("failed", options, attempt_number, "device-removal confirmation failed");
                return 1;
            }
            if (confirmation == RemovalConfirmation::Removed) {
                log_event("disconnected", options, attempt_number);
                have_seen_absence = true;
                waiting_logged = false;
                continue;
            }
            log_event("failed", options, attempt_number, attempt.detail);
            return 1;
        }

        log_event("failed", options, attempt_number, "runtime exited unexpectedly");
        return 1;
    }
}

} // namespace

int run_camera_supervisor(Options options) {
    // This is the first operation in the driver run path. The signal waiter
    // blocks process signals before any SDK, runtime, or capture thread exists.
    SignalWatcher signals;
    int result = 1;
    try {
        validate_options(options);
        const auto deadline = make_deadline(options);
        if (!options.reconnect) {
            result = run_without_reconnect(options, signals, deadline);
        } else {
            auto monitor = watch_device(options.serial);
            result = run_with_reconnect(options, *monitor, signals, deadline);
        }
    } catch (const std::exception& error) {
        log_event("failed", options, 0U, error.what());
        result = 1;
    } catch (...) {
        log_event("failed", options, 0U, "unknown exception in Orbbec camera supervisor");
        result = 1;
    }

    const int restored = signals.finish();
    if (restored != 0) {
        log_event("failed", options, 0U, "cannot restore signal mask");
        return 1;
    }
    return result;
}

} // namespace rbt_orbbec
