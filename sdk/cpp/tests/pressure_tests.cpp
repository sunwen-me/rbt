#include <rbt/control.hpp>
#include <rbt/stream.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::string unique_service_name() {
    static std::atomic<std::uint64_t> next { 0U };
    return "rbt_cpp_pressure_" + std::to_string(static_cast<unsigned long long>(next.fetch_add(1U)));
}

using ControlCommand = rbt::control::ControlCommand;
using ControlPublisher = rbt::FixedPublisher<ControlCommand>;
using ControlSubscriber = rbt::FixedSubscriber<ControlCommand>;
using ControlLoan = rbt::LoanedSample<ControlCommand>;
using ControlSample = rbt::ReceivedSample<ControlCommand>;

rbt::StreamDescriptor control_descriptor(rbt::StreamResourceConfig resources =
                                             rbt::StreamResourceConfig::defaults()) {
    return rbt::StreamDescriptor(
        unique_service_name(), rbt::control::CONTROL_COMMAND_DESCRIPTOR, rbt::StreamProfile::Control, resources);
}

std::optional<ControlSample> receive_control_sample(ControlSubscriber& subscriber) {
    auto received = subscriber.receive();
    if (!received || !received.value().has_value()) {
        return std::nullopt;
    }
    auto optional_sample = std::move(received).value();
    return std::move(optional_sample).value();
}

bool wait_for_subscribers(const rbt::FixedPublisher<rbt::control::ControlCommand>& publisher,
                          std::uint64_t expected) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (publisher.pressure().publisher->subscriber_count == expected) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

bool send_control(rbt::FixedPublisher<rbt::control::ControlCommand>& publisher,
                  std::uint64_t sequence,
                  rbt::StreamError* error = nullptr) {
    auto loan = publisher.loan();
    if (!loan) {
        return false;
    }
    auto sample = std::move(loan).value();
    sample.payload().sequence = sequence;
    sample.payload().value = static_cast<float>(sequence);
    sample.payload().reserved = 0U;
    auto sent = sample.send();
    if (!sent && error != nullptr) {
        *error = sent.error();
    }
    return static_cast<bool>(sent);
}

bool receive_sequence(const rbt::FixedSubscriber<rbt::control::ControlCommand>& subscriber,
                      std::uint64_t expected) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        auto received = subscriber.receive();
        if (!received) {
            return false;
        }
        if (received.value().has_value()) {
            auto optional_sample = std::move(received).value();
            auto sample = std::move(optional_sample).value();
            return sample.payload().sequence == expected;
        }
        std::this_thread::yield();
    }
    return false;
}

int test_resource_configuration() {
    const auto defaults = rbt::StreamResourceConfig::defaults();
    if (defaults.max_loans != 2U || defaults.max_borrowed != 1U) {
        return 1;
    }

    const auto default_descriptor = control_descriptor();
    if (default_descriptor.resources != defaults) {
        return 2;
    }
    auto default_publisher_result = ControlPublisher::create(
        default_descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!default_publisher_result) {
        return 3;
    }
    auto default_publisher = std::move(default_publisher_result).value();
    const auto default_pressure = default_publisher.pressure();
    if (!default_pressure.publisher.has_value() || default_pressure.resources != defaults
        || default_pressure.publisher->loan_attempts != 0U
        || default_pressure.publisher->loans_in_use != 0U) {
        return 4;
    }

    const rbt::StreamResourceConfig custom { 3U, 2U };
    const auto custom_descriptor = control_descriptor(custom);
    auto custom_publisher_result = ControlPublisher::create_with_resources(
        custom_descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR, custom);
    if (!custom_publisher_result) {
        return 5;
    }
    auto custom_publisher = std::move(custom_publisher_result).value();
    auto custom_subscriber_result = ControlSubscriber::create_with_resources(
        custom_descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR, custom);
    if (!custom_subscriber_result) {
        return 6;
    }
    auto custom_subscriber = std::move(custom_subscriber_result).value();
    if (!wait_for_subscribers(custom_publisher, 1U)) {
        return 7;
    }
    const auto custom_publisher_pressure = custom_publisher.pressure();
    const auto custom_subscriber_pressure = custom_subscriber.pressure();
    if (!custom_publisher_pressure.publisher.has_value()
        || custom_publisher_pressure.resources != custom
        || !custom_subscriber_pressure.subscriber.has_value()
        || custom_subscriber_pressure.resources != custom) {
        return 8;
    }

    const auto validation_descriptor = control_descriptor();
    const auto zero_loans = rbt::StreamResourceConfig { 0U, 1U };
    const auto zero_borrowed = rbt::StreamResourceConfig { 2U, 0U };
    auto zero_loans_publisher = ControlPublisher::create_with_resources(
        validation_descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR, zero_loans);
    auto zero_borrowed_publisher = ControlPublisher::create_with_resources(
        validation_descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR, zero_borrowed);
    auto zero_loans_subscriber = ControlSubscriber::create_with_resources(
        validation_descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR, zero_loans);
    auto zero_borrowed_subscriber = ControlSubscriber::create_with_resources(
        validation_descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR, zero_borrowed);
    if (zero_loans_publisher || zero_loans_publisher.error() != rbt::StreamError::InvalidResourceConfig
        || zero_borrowed_publisher || zero_borrowed_publisher.error() != rbt::StreamError::InvalidResourceConfig
        || zero_loans_subscriber || zero_loans_subscriber.error() != rbt::StreamError::InvalidResourceConfig
        || zero_borrowed_subscriber
        || zero_borrowed_subscriber.error() != rbt::StreamError::InvalidResourceConfig) {
        return 9;
    }

    const auto advertised = rbt::StreamResourceConfig { 3U, 2U };
    const auto advertised_descriptor = control_descriptor(advertised);
    auto mismatched_loans = ControlPublisher::create_with_resources(
        advertised_descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR, { 2U, 2U });
    auto mismatched_borrowed = ControlPublisher::create_with_resources(
        advertised_descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR, { 3U, 1U });
    auto excessive_borrowed = ControlSubscriber::create_with_resources(
        advertised_descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR, { 3U, 3U });
    if (mismatched_loans || mismatched_loans.error() != rbt::StreamError::IncompatibleResourceConfig
        || mismatched_borrowed || mismatched_borrowed.error() != rbt::StreamError::IncompatibleResourceConfig
        || excessive_borrowed || excessive_borrowed.error() != rbt::StreamError::IncompatibleResourceConfig) {
        return 10;
    }

    return 0;
}

int test_loan_pressure() {
    const auto descriptor = control_descriptor({ 3U, 2U });
    auto publisher_result = ControlPublisher::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!publisher_result) {
        return 1;
    }
    auto publisher = std::move(publisher_result).value();

    auto first_result = publisher.loan();
    auto second_result = publisher.loan();
    auto third_result = publisher.loan();
    if (!first_result || !second_result || !third_result) {
        return 2;
    }
    std::optional<ControlLoan> first { std::move(first_result).value() };
    std::optional<ControlLoan> second { std::move(second_result).value() };
    std::optional<ControlLoan> third { std::move(third_result).value() };

    auto failed = publisher.loan();
    if (failed || failed.error() != rbt::StreamError::LoanPoolExhausted) {
        return 3;
    }
    auto saturated = publisher.pressure();
    if (!saturated.publisher.has_value()
        || saturated.publisher->loan_attempts != 4U
        || saturated.publisher->loan_successes != 3U
        || saturated.publisher->loans_in_use != 3U
        || saturated.publisher->loans_high_water != 3U
        || saturated.publisher->loan_failures != 1U
        || saturated.last_pressure_event != rbt::PressureEvent::LoanPoolExhausted) {
        return 4;
    }

    // Move ownership before dropping it; the move itself must not decrement
    // the active-loan count.
    std::optional<ControlLoan> moved_first { std::move(first) };
    first.reset();
    const auto after_move = publisher.pressure();
    if (!after_move.publisher.has_value() || after_move.publisher->loans_in_use != 3U) {
        return 5;
    }
    moved_first.reset();
    const auto after_drop = publisher.pressure();
    if (!after_drop.publisher.has_value() || after_drop.publisher->loans_in_use != 2U) {
        return 6;
    }

    auto recovered_result = publisher.loan();
    if (!recovered_result) {
        return 7;
    }
    std::optional<ControlLoan> recovered { std::move(recovered_result).value() };
    const auto after_recovery = publisher.pressure();
    if (!after_recovery.publisher.has_value()
        || after_recovery.publisher->loan_attempts != 5U
        || after_recovery.publisher->loan_successes != 4U
        || after_recovery.publisher->loans_in_use != 3U
        || after_recovery.publisher->loans_high_water != 3U
        || after_recovery.publisher->loan_failures != 1U) {
        return 8;
    }

    recovered.reset();
    second.reset();
    third.reset();
    const auto released = publisher.pressure();
    return released.publisher.has_value() && released.publisher->loans_in_use == 0U
               && released.publisher->loans_high_water == 3U
           ? 0
           : 9;
}

int test_repeated_loan_pressure_recovery() {
    constexpr std::uint64_t kCycles = 1000U;
    const rbt::StreamResourceConfig resources { 1U, 1U };
    const auto descriptor = control_descriptor(resources);
    auto publisher_result = ControlPublisher::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!publisher_result) {
        return 1;
    }
    auto publisher = std::move(publisher_result).value();

    auto initial_result = publisher.loan();
    if (!initial_result) {
        return 2;
    }
    std::optional<ControlLoan> held { std::move(initial_result).value() };
    for (std::uint64_t cycle = 0U; cycle < kCycles; ++cycle) {
        auto exhausted = publisher.loan();
        if (exhausted || exhausted.error() != rbt::StreamError::LoanPoolExhausted) {
            return 3;
        }

        held.reset();
        auto recovered_result = publisher.loan();
        if (!recovered_result) {
            return 4;
        }
        held.emplace(std::move(recovered_result).value());
    }
    held.reset();

    const auto pressure = publisher.pressure();
    if (!pressure.publisher.has_value()) {
        return 5;
    }
    const auto& counters = pressure.publisher.value();
    return pressure.resources == resources
               && counters.loan_attempts == 2U * kCycles + 1U
               && counters.loan_successes == kCycles + 1U
               && counters.loan_failures == kCycles
               && counters.loans_high_water == 1U
               && counters.loans_in_use == 0U
               && pressure.last_pressure_event == rbt::PressureEvent::LoanPoolExhausted
           ? 0
           : 6;
}

int test_borrow_pressure() {
    const auto descriptor = control_descriptor({ 3U, 2U });
    auto publisher_result = ControlPublisher::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!publisher_result) {
        return 1;
    }
    auto subscriber_result = ControlSubscriber::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!subscriber_result) {
        return 2;
    }
    auto publisher = std::move(publisher_result).value();
    auto subscriber = std::move(subscriber_result).value();
    if (!wait_for_subscribers(publisher, 1U)) {
        return 3;
    }

    for (std::uint64_t sequence = 0U; sequence < 2U; ++sequence) {
        if (!send_control(publisher, sequence)) {
            return 4;
        }
    }
    auto first = receive_control_sample(subscriber);
    if (!first.has_value()) {
        return 5;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto after_first = subscriber.pressure();
    if (!after_first.subscriber.has_value()
        || after_first.subscriber->receive_attempts != 1U
        || after_first.subscriber->received_samples != 1U
        || after_first.subscriber->borrowed_samples != 1U
        || after_first.subscriber->borrowed_high_water != 1U
        || after_first.subscriber->borrow_failures != 0U
        || after_first.subscriber->borrow_duration_count != 0U
        || !after_first.subscriber->oldest_borrow_age_ns.has_value()
        || after_first.subscriber->oldest_borrow_age_ns.value() == 0U) {
        return 6;
    }

    auto second = receive_control_sample(subscriber);
    if (!second.has_value()) {
        return 7;
    }
    const auto after_second = subscriber.pressure();
    if (!after_second.subscriber.has_value()
        || after_second.subscriber->receive_attempts != 2U
        || after_second.subscriber->received_samples != 2U
        || after_second.subscriber->borrowed_samples != 2U
        || after_second.subscriber->borrowed_high_water != 2U
        || after_second.subscriber->borrow_failures != 0U
        || after_second.subscriber->borrow_duration_count != 0U
        || !after_second.subscriber->oldest_borrow_age_ns.has_value()) {
        return 8;
    }

    auto borrow_failure = subscriber.receive();
    if (borrow_failure || borrow_failure.error() != rbt::StreamError::BorrowLimitReached) {
        return 9;
    }
    const auto saturated = subscriber.pressure();
    if (!saturated.subscriber.has_value()
        || saturated.subscriber->receive_attempts != 3U
        || saturated.subscriber->received_samples != 2U
        || saturated.subscriber->borrowed_samples != 2U
        || saturated.subscriber->borrowed_high_water != 2U
        || saturated.subscriber->borrow_failures != 1U
        || saturated.subscriber->borrow_duration_count != 0U
        || !saturated.subscriber->oldest_borrow_age_ns.has_value()
        || saturated.last_pressure_event != rbt::PressureEvent::BorrowLimitReached) {
        return 10;
    }

    // Moving a receive guard preserves its active Borrow ownership.
    std::optional<ControlSample> moved_first { std::move(first) };
    first.reset();
    const auto after_move = subscriber.pressure();
    if (!after_move.subscriber.has_value() || after_move.subscriber->borrowed_samples != 2U) {
        return 11;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    moved_first.reset();
    const auto after_first_drop = subscriber.pressure();
    if (!after_first_drop.subscriber.has_value()
        || after_first_drop.subscriber->borrowed_samples != 1U
        || after_first_drop.subscriber->borrow_duration_count != 1U
        || after_first_drop.subscriber->borrow_duration_total_ns == 0U
        || after_first_drop.subscriber->borrow_duration_max_ns == 0U
        || !after_first_drop.subscriber->oldest_borrow_age_ns.has_value()) {
        return 12;
    }

    second.reset();
    const auto after_second_drop = subscriber.pressure();
    if (!after_second_drop.subscriber.has_value()
        || after_second_drop.subscriber->borrowed_samples != 0U
        || after_second_drop.subscriber->borrow_duration_count != 2U
        || after_second_drop.subscriber->borrow_duration_total_ns
               < after_first_drop.subscriber->borrow_duration_total_ns
        || after_second_drop.subscriber->borrow_duration_max_ns == 0U
        || after_second_drop.subscriber->oldest_borrow_age_ns.has_value()) {
        return 13;
    }

    // An empty receive reserves and releases a slot, but does not create a
    // completed Borrow lifetime.
    const auto before_empty = after_second_drop.subscriber.value();
    auto empty = subscriber.receive();
    if (!empty || empty.value().has_value()) {
        return 14;
    }
    const auto after_empty = subscriber.pressure();
    return after_empty.subscriber.has_value()
               && after_empty.subscriber->receive_attempts == before_empty.receive_attempts + 1U
               && after_empty.subscriber->received_samples == before_empty.received_samples
               && after_empty.subscriber->borrowed_samples == before_empty.borrowed_samples
               && after_empty.subscriber->borrowed_high_water == before_empty.borrowed_high_water
               && after_empty.subscriber->borrow_failures == before_empty.borrow_failures
               && after_empty.subscriber->borrow_duration_count == before_empty.borrow_duration_count
               && after_empty.subscriber->borrow_duration_total_ns == before_empty.borrow_duration_total_ns
               && after_empty.subscriber->borrow_duration_max_ns == before_empty.borrow_duration_max_ns
               && !after_empty.subscriber->oldest_borrow_age_ns.has_value()
           ? 0
           : 15;
}

int test_repeated_borrow_pressure_recovery() {
    constexpr std::uint64_t kCycles = 1000U;
    const rbt::StreamResourceConfig resources { 2U, 1U };
    const auto descriptor = control_descriptor(resources);
    auto publisher_result = ControlPublisher::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!publisher_result) {
        return 1;
    }
    auto subscriber_result = ControlSubscriber::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!subscriber_result) {
        return 2;
    }
    auto publisher = std::move(publisher_result).value();
    auto subscriber = std::move(subscriber_result).value();
    if (!wait_for_subscribers(publisher, 1U)) {
        return 3;
    }

    for (std::uint64_t cycle = 0U; cycle < kCycles; ++cycle) {
        const auto first_sequence = 2U * cycle;
        if (!send_control(publisher, first_sequence)
            || !send_control(publisher, first_sequence + 1U)) {
            return 4;
        }

        auto first = receive_control_sample(subscriber);
        if (!first.has_value()) {
            return 5;
        }
        auto borrow_failure = subscriber.receive();
        if (borrow_failure || borrow_failure.error() != rbt::StreamError::BorrowLimitReached) {
            return 6;
        }

        first.reset();
        auto recovered = receive_control_sample(subscriber);
        if (!recovered.has_value()) {
            return 7;
        }
        recovered.reset();
    }

    const auto publisher_pressure = publisher.pressure();
    const auto subscriber_pressure = subscriber.pressure();
    if (!publisher_pressure.publisher.has_value() || !subscriber_pressure.subscriber.has_value()) {
        return 8;
    }
    const auto& publisher_counters = publisher_pressure.publisher.value();
    const auto& subscriber_counters = subscriber_pressure.subscriber.value();
    return publisher_pressure.resources == resources
               && publisher_counters.loan_attempts == 2U * kCycles
               && publisher_counters.loan_successes == 2U * kCycles
               && publisher_counters.loan_failures == 0U
               && publisher_counters.loans_high_water == 1U
               && publisher_counters.loans_in_use == 0U
               && publisher_counters.send_attempts == 2U * kCycles
               && publisher_counters.send_successes == 2U * kCycles
               && publisher_counters.send_failures == 0U
               && subscriber_pressure.resources == resources
               && subscriber_counters.receive_attempts == 3U * kCycles
               && subscriber_counters.received_samples == 2U * kCycles
               && subscriber_counters.borrowed_samples == 0U
               && subscriber_counters.borrowed_high_water == 1U
               && subscriber_counters.borrow_failures == kCycles
               && subscriber_counters.borrow_duration_count == 2U * kCycles
               && subscriber_counters.borrow_duration_total_ns > 0U
               && subscriber_counters.borrow_duration_max_ns > 0U
               && !subscriber_counters.oldest_borrow_age_ns.has_value()
               && subscriber_pressure.last_pressure_event == rbt::PressureEvent::BorrowLimitReached
           ? 0
           : 9;
}

int test_borrow_state_isolation() {
    const auto descriptor = control_descriptor({ 3U, 2U });
    auto publisher_result = ControlPublisher::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto first_result = ControlSubscriber::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!publisher_result || !first_result) {
        return 1;
    }
    auto publisher = std::move(publisher_result).value();
    auto first_subscriber = std::move(first_result).value();
    if (!wait_for_subscribers(publisher, 1U) || !send_control(publisher, 0U)) {
        return 2;
    }
    auto first_sample = receive_control_sample(first_subscriber);
    if (!first_sample.has_value()) {
        return 3;
    }

    auto second_result = ControlSubscriber::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!second_result) {
        return 4;
    }
    auto second_subscriber = std::move(second_result).value();
    if (!wait_for_subscribers(publisher, 2U)) {
        return 5;
    }
    const auto before_release = second_subscriber.pressure();
    if (!before_release.subscriber.has_value()
        || before_release.subscriber->receive_attempts != 0U
        || before_release.subscriber->received_samples != 0U
        || before_release.subscriber->borrowed_samples != 0U
        || before_release.subscriber->borrowed_high_water != 0U
        || before_release.subscriber->borrow_failures != 0U
        || before_release.subscriber->borrow_duration_count != 0U
        || before_release.subscriber->borrow_duration_total_ns != 0U
        || before_release.subscriber->borrow_duration_max_ns != 0U
        || before_release.subscriber->oldest_borrow_age_ns.has_value()) {
        return 6;
    }

    first_sample.reset();
    const auto after_release = second_subscriber.pressure();
    const auto first_after_release = first_subscriber.pressure();
    return after_release.subscriber.has_value()
               && after_release.subscriber.value() == before_release.subscriber.value()
               && first_after_release.subscriber.has_value()
               && first_after_release.subscriber->borrowed_samples == 0U
               && first_after_release.subscriber->borrow_duration_count == 1U
           ? 0
           : 7;
}

int test_concurrent_loan_ownership() {
    constexpr std::uint64_t kMaxLoans = 3U;
    const auto descriptor = control_descriptor({ kMaxLoans, 2U });
    auto publisher_result = ControlPublisher::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!publisher_result) {
        return 1;
    }
    auto publisher = std::move(publisher_result).value();

    constexpr int kWorkers = 4;
    constexpr int kIterations = 64;
    std::atomic<bool> start { false };
    std::atomic<bool> unexpected_error { false };
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                auto loan_result = publisher.loan();
                if (!loan_result) {
                    if (loan_result.error() != rbt::StreamError::LoanPoolExhausted) {
                        unexpected_error.store(true, std::memory_order_release);
                    }
                    continue;
                }
                auto loan = std::move(loan_result).value();
                loan.payload().sequence = static_cast<std::uint64_t>(worker * kIterations + iteration);
                if ((iteration % 4) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    const auto pressure = publisher.pressure();
    if (unexpected_error.load(std::memory_order_acquire) || !pressure.publisher.has_value()) {
        return 2;
    }
    const auto& counters = pressure.publisher.value();
    return counters.loans_in_use == 0U && counters.loans_high_water <= kMaxLoans
               && counters.loan_attempts == counters.loan_successes + counters.loan_failures
           ? 0
           : 3;
}

int test_control_partial_delivery() {
    const rbt::StreamDescriptor descriptor(
        unique_service_name(), rbt::control::CONTROL_COMMAND_DESCRIPTOR, rbt::StreamProfile::Control);
    auto publisher_result = rbt::FixedPublisher<rbt::control::ControlCommand>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto first_result = rbt::FixedSubscriber<rbt::control::ControlCommand>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto second_result = rbt::FixedSubscriber<rbt::control::ControlCommand>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto stalled_result = rbt::FixedSubscriber<rbt::control::ControlCommand>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!publisher_result || !first_result || !second_result || !stalled_result) {
        return 19;
    }
    auto publisher = std::move(publisher_result).value();
    auto first = std::move(first_result).value();
    auto second = std::move(second_result).value();
    auto stalled = std::move(stalled_result).value();
    if (!wait_for_subscribers(publisher, 3U)) {
        return 20;
    }
    for (std::uint64_t sequence = 0U; sequence < 4U; ++sequence) {
        if (!send_control(publisher, sequence) || !receive_sequence(first, sequence)
            || !receive_sequence(second, sequence)) {
            return 21;
        }
    }
    rbt::StreamError send_error = rbt::StreamError::Transport;
    if (send_control(publisher, 4U, &send_error) || send_error != rbt::StreamError::QueueFull
        || !receive_sequence(first, 4U) || !receive_sequence(second, 4U)) {
        return 22;
    }
    for (std::uint64_t sequence = 0U; sequence < 4U; ++sequence) {
        if (!receive_sequence(stalled, sequence)) {
            return 23;
        }
    }
    const auto pressure = publisher.pressure();
    return pressure.publisher.has_value() && pressure.publisher->send_attempts == 5U
               && pressure.publisher->send_successes == 4U && pressure.publisher->send_failures == 1U
               && pressure.last_pressure_event == rbt::PressureEvent::QueueFull
               && pressure.last_pressure_subscriber.has_value()
               && pressure.last_pressure_subscriber->size() == 32U
           ? 0
           : 24;
}

// Counts iceoryx2 node directories; only the difference between two calls
// is meaningful.  A node destroyed while one of its ports is still alive
// cannot remove its directory, so a sample outliving its endpoint used to
// leave one behind per endpoint.
std::size_t node_dir_entries() {
    std::error_code error;
    std::size_t count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator("/tmp/iceoryx2/nodes", error)) {
        if (entry.is_directory(error)) {
            ++count;
        }
    }
    return count;
}

int test_sample_outliving_endpoint_releases_node() {
    const auto before = node_dir_entries();
    {
        const auto descriptor = control_descriptor();
        std::optional<ControlPublisher> publisher(
            std::move(ControlPublisher::create(descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR))
                .value());
        std::optional<ControlSubscriber> subscriber(
            std::move(ControlSubscriber::create(descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR))
                .value());
        std::optional<ControlSample> sample;
        for (int attempt = 0; attempt < 1000 && !sample.has_value(); ++attempt) {
            if (!send_control(*publisher, 1U)) {
                return 1;
            }
            sample = receive_control_sample(*subscriber);
            std::this_thread::yield();
        }
        if (!sample.has_value()) {
            return 2;
        }
        auto loan_result = publisher->loan();
        if (!loan_result) {
            return 3;
        }
        std::optional<ControlLoan> loan(std::move(loan_result).value());
        subscriber.reset();
        publisher.reset();
        if (sample->payload().sequence != 1U) {
            return 4;
        }
    }
    const auto after = node_dir_entries();
    if (after > before) {
        std::cerr << "node directory grew from " << before << " to " << after << " entries\n";
        return 5;
    }
    return 0;
}

} // namespace

int main() {
    const auto node_result = test_sample_outliving_endpoint_releases_node();
    if (node_result != 0) {
        std::cerr << "sample outliving endpoint node cleanup test failed: " << node_result << '\n';
        return node_result;
    }
    const auto result = test_resource_configuration();
    if (result != 0) {
        std::cerr << "resource configuration test failed: " << result << '\n';
        return result;
    }
    const auto loan_result = test_loan_pressure();
    if (loan_result != 0) {
        std::cerr << "loan pressure test failed: " << loan_result << '\n';
        return loan_result;
    }
    const auto repeated_loan_result = test_repeated_loan_pressure_recovery();
    if (repeated_loan_result != 0) {
        std::cerr << "repeated loan pressure recovery test failed: " << repeated_loan_result << '\n';
        return repeated_loan_result;
    }
    const auto borrow_result = test_borrow_pressure();
    if (borrow_result != 0) {
        std::cerr << "borrow pressure test failed: " << borrow_result << '\n';
        return borrow_result;
    }
    const auto repeated_borrow_result = test_repeated_borrow_pressure_recovery();
    if (repeated_borrow_result != 0) {
        std::cerr << "repeated borrow pressure recovery test failed: " << repeated_borrow_result << '\n';
        return repeated_borrow_result;
    }
    const auto isolation_result = test_borrow_state_isolation();
    if (isolation_result != 0) {
        std::cerr << "borrow state isolation test failed: " << isolation_result << '\n';
        return isolation_result;
    }
    const auto stress_result = test_concurrent_loan_ownership();
    if (stress_result != 0) {
        std::cerr << "concurrent loan ownership test failed: " << stress_result << '\n';
        return stress_result;
    }
    const auto partial_result = test_control_partial_delivery();
    if (partial_result != 0) {
        std::cerr << "partial delivery test failed: " << partial_result << '\n';
        return partial_result;
    }
    std::cout << "pressure recovery evidence loan_cycles=1000 borrow_cycles=1000 "
                 "loan_attempts=2001 loan_successes=1001 loan_failures=1000 "
                 "borrow_attempts=3000 borrow_successes=2000 borrow_failures=1000 "
                 "borrow_duration_count=2000 loans_in_use=0 borrowed_samples=0\n";
    return 0;
}
