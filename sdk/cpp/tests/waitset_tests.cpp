#include <rbt/control.hpp>
#include <rbt/sensor.hpp>
#include <rbt/wait.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

template <typename T>
void require(const T& condition) {
    if (!static_cast<bool>(condition)) {
        std::abort();
    }
}

std::string unique_service_name() {
    static std::atomic<std::uint64_t> next { 0U };
    return "rbt_cpp_wait_" + std::to_string(static_cast<unsigned long long>(next.fetch_add(1U)));
}

using Command = rbt::control::ControlCommand;
using ImagePublisher = rbt::SlicePublisher<rbt::sensor::ImageHeader, std::uint8_t>;
using ImageSubscriber = rbt::SliceSubscriber<rbt::sensor::ImageHeader, std::uint8_t>;

rbt::StreamDescriptor command_stream() {
    return rbt::StreamDescriptor(unique_service_name(),
                                 rbt::control::CONTROL_COMMAND_DESCRIPTOR,
                                 rbt::StreamProfile::Control);
}

bool send_command_until_delivered(rbt::FixedPublisher<Command>& publisher,
                                  std::uint64_t sequence) {
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        auto loan = publisher.loan();
        if (loan) {
            auto sample = std::move(loan).value();
            sample.payload().sequence = sequence;
            sample.payload().value = static_cast<float>(sequence);
            sample.payload().reserved = 0U;
            auto sent = sample.send();
            if (sent && sent.value() > 0U) {
                return true;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool send_image_until_delivered(ImagePublisher& publisher, std::uint64_t sequence) {
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        auto loan = publisher.loan_slice(3U);
        if (loan) {
            auto sample = std::move(loan).value();
            sample.header().sequence = sequence;
            sample.payload()[0] = 3U;
            sample.payload()[1] = 1U;
            sample.payload()[2] = 4U;
            auto sent = sample.send();
            if (sent && sent.value() > 0U) {
                return true;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

void readiness_failures_have_typed_stream_errors() {
    require(rbt::WaitToken::CANCELLATION == rbt::WaitToken::cancellation());
    require(std::string_view(rbt::to_string(rbt::StreamError::ReadinessOpenOrCreate))
            == "ReadinessOpenOrCreate");
    require(std::string_view(rbt::to_string(rbt::StreamError::ReadinessOpen)) == "ReadinessOpen");
    require(std::string_view(rbt::to_string(rbt::StreamError::ReadinessNotifier))
            == "ReadinessNotifier");
    require(std::string_view(rbt::to_string(rbt::StreamError::ReadinessListener))
            == "ReadinessListener");
    require(std::string_view(rbt::to_string(rbt::StreamError::ReadinessNotify)) == "ReadinessNotify");
    require(rbt::StreamError::ReadinessNotify != rbt::StreamError::Transport);
    require(rbt::WaitProgress::StopAfterSamples(1U).should_stop());
}

void fixed_source_wakes_and_receives() {
    const auto descriptor = command_stream();
    auto publisher_result = rbt::FixedPublisher<Command>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto subscriber_result = rbt::FixedSubscriber<Command>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto context_result = rbt::WaitContext::create("wait-fixed", 1U);
    require(publisher_result && subscriber_result && context_result);

    auto publisher = std::move(publisher_result).value();
    auto subscriber = std::move(subscriber_result).value();
    auto context = std::move(context_result).value();
    const auto token = rbt::WaitToken(7U);
    const auto source = subscriber.event_source(token);

    std::jthread producer([publisher = std::move(publisher)](std::stop_token) mutable {
        std::this_thread::sleep_for(20ms);
        auto loan = publisher.loan();
        require(loan);
        auto sample = std::move(loan).value();
        sample.payload().sequence = 42U;
        sample.payload().value = 4.2F;
        sample.payload().reserved = 0U;
        require(sample.send());
    });

    const auto result = context.run({ source }, [&](rbt::ReadyEvent event, std::size_t budget) {
        require(budget == rbt::MAX_DRAIN_PER_SOURCE);
        require(!event.is_cancellation());
        require(event.token_value() == token);
        if (event.is_synthetic()) {
            return rbt::WaitProgress::Continue;
        }
        auto received = subscriber.receive();
        require(received);
        if (!received.value().has_value()) {
            return rbt::WaitProgress::Empty;
        }
        auto sample = std::move(received).value();
        require(sample.has_value());
        require(std::move(sample).value().payload().sequence == 42U);
        return rbt::WaitProgress::stop_after_samples(1U);
    });
    producer.join();

    require(result);
    require(result.value() == rbt::WaitExit::Stopped);
    require(context.stats().wakeups() >= 1U);
    require(context.stats().samples_processed() == 1U);
    require(context.stats().empty_wakeups() == 0U);
}

void slice_source_wakes_and_receives_payload() {
    const auto descriptor = rbt::StreamDescriptor(unique_service_name(),
                                                   rbt::sensor::IMAGE_DESCRIPTOR,
                                                   rbt::StreamProfile::Sensor);
    auto publisher_result = ImagePublisher::create(descriptor, rbt::sensor::IMAGE_DESCRIPTOR, 8U);
    auto subscriber_result = ImageSubscriber::create(descriptor, rbt::sensor::IMAGE_DESCRIPTOR);
    auto context_result = rbt::WaitContext::create("wait-slice", 10U);
    require(publisher_result && subscriber_result && context_result);

    auto publisher = std::move(publisher_result).value();
    auto subscriber = std::move(subscriber_result).value();
    auto context = std::move(context_result).value();
    const auto token = rbt::WaitToken(11U);
    require(send_image_until_delivered(publisher, 19U));
    const auto source = subscriber.event_source(token);
    bool synthetic_seen = false;

    const auto result = context.run({ source }, [&](rbt::ReadyEvent event, std::size_t budget) {
        require(budget == rbt::MAX_DRAIN_PER_SOURCE);
        require(!event.is_cancellation());
        require(event.token_value() == token);
        if (event.is_synthetic()) {
            synthetic_seen = true;
            return rbt::WaitProgress::Continue;
        }
        auto received = subscriber.receive();
        require(received);
        if (!received.value().has_value()) {
            return rbt::WaitProgress::Empty;
        }
        auto optional_sample = std::move(received).value();
        require(optional_sample.has_value());
        auto sample = std::move(optional_sample).value();
        require(sample.header().sequence == 19U);
        const auto payload = sample.payload();
        require(payload.size() == 3U);
        require(payload[0] == 3U);
        require(payload[1] == 1U);
        require(payload[2] == 4U);
        return rbt::WaitProgress::stop_after_samples(1U);
    });

    require(result);
    require(result.value() == rbt::WaitExit::Stopped);
    require(synthetic_seen);
    require(context.stats().wakeups() >= 1U);
    require(context.stats().samples_processed() == 1U);
    require(context.stats().empty_wakeups() == 0U);
}

void throwing_handler_becomes_typed_error() {
    const auto descriptor = command_stream();
    auto publisher_result = rbt::FixedPublisher<Command>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto subscriber_result = rbt::FixedSubscriber<Command>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto context_result = rbt::WaitContext::create("wait-handler-error", 9U);
    require(publisher_result && subscriber_result && context_result);

    auto publisher = std::move(publisher_result).value();
    auto subscriber = std::move(subscriber_result).value();
    auto context = std::move(context_result).value();
    const auto source = subscriber.event_source(rbt::WaitToken(42U));
    std::jthread producer([publisher = std::move(publisher)](std::stop_token) mutable {
        std::this_thread::sleep_for(20ms);
        auto loan = publisher.loan();
        require(loan);
        require(std::move(loan).value().send());
    });

    const auto result = context.run({ source }, [](rbt::ReadyEvent event, std::size_t) {
        if (!event.is_synthetic()) {
            throw std::runtime_error("intentional wait handler failure");
        }
        return rbt::WaitProgress::Continue;
    });
    producer.join();
    require(!result);
    require(result.error() == rbt::WaitError::Handler);
}

void synthetic_progress_counts_samples_only() {
    const auto descriptor = command_stream();
    auto publisher_result = rbt::FixedPublisher<Command>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto subscriber_result = rbt::FixedSubscriber<Command>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto context_result = rbt::WaitContext::create("wait-synthetic", 5U);
    require(publisher_result && subscriber_result && context_result);

    auto publisher = std::move(publisher_result).value();
    auto subscriber = std::move(subscriber_result).value();
    auto context = std::move(context_result).value();
    const auto source = subscriber.event_source(rbt::WaitToken::new_token(41U));
    const auto wake = context.wake_handle();
    std::jthread canceller([wake](std::stop_token) {
        std::this_thread::sleep_for(20ms);
        require(wake.cancel());
    });

    const auto result = context.run({ source }, [&](rbt::ReadyEvent event, std::size_t) {
        if (event.is_synthetic()) {
            return rbt::WaitProgress::samples_and_empty(3U, 4U);
        }
        return rbt::WaitProgress::Continue;
    });
    canceller.join();
    require(result && result.value() == rbt::WaitExit::Cancelled);
    require(context.stats().samples_processed() == 3U);
    require(context.stats().empty_wakeups() == 0U);
    require(context.stats().wakeups() >= 1U);
    require(context.stats().cancellation_wakeups() == 1U);
    (void)publisher;
}

void stop_token_wakes_idle_wait() {
    auto context_result = rbt::WaitContext::create("wait-cancel", 2U);
    require(context_result);
    auto context = std::move(context_result).value();
    std::stop_source stop_source;
    std::jthread canceller([&stop_source](std::stop_token) {
        std::this_thread::sleep_for(20ms);
        stop_source.request_stop();
    });

    const auto result = context.run(stop_source.get_token(), std::vector<rbt::EventSource> {},
                                    [](rbt::ReadyEvent, std::size_t) {
                                        return rbt::WaitProgress::Continue;
                                    });
    canceller.join();

    require(result);
    require(result.value() == rbt::WaitExit::Cancelled);
    require(context.stats().cancellation_wakeups() == 1U);
    require(context.stats().waits() >= 1U);
    require(context.stats().wakeups() >= 1U);
}

void wake_handle_keeps_cancellation_node_alive() {
    rbt::WaitWakeHandle wake;
    {
        auto context_result = rbt::WaitContext::create("wait-handle-lifetime", 8U);
        require(context_result);
        auto context = std::move(context_result).value();
        wake = context.wake_handle();
    }

    // EffectScope may retain this handle until after the blocking context has
    // returned.  Waking it must remain a valid, typed operation and the final
    // handle drop must release notifier-before-node resources.
    require(wake.wake());
    require(wake.is_cancelled());
}

void moved_context_is_safe_after_move_assignment() {
    auto target_result = rbt::WaitContext::create("wait-move-target", 11U);
    auto source_result = rbt::WaitContext::create("wait-move-source", 12U);
    require(target_result && source_result);

    auto target = std::move(target_result).value();
    auto source = std::move(source_result).value();
    const auto old_target_wake = target.wake_handle();
    target = std::move(source);

    require(target.process_instance() == "wait-move-source");
    require(target.activation() == 12U);
    require(target.wake_handle());
    require(!source.wake_handle());
    require(!source.is_cancelled());
    auto moved_from_wake = source.wake_handle();
    const auto moved_from_result = moved_from_wake.wake();
    require(!moved_from_result);
    require(moved_from_result.error() == rbt::WaitError::Notify);

    // An external handle to the overwritten context state remains valid, and
    // the newly assigned context owns an independent cancellation endpoint.
    require(old_target_wake.wake());
    require(old_target_wake.is_cancelled());
    require(target.cancel());
    require(target.is_cancelled());
}

void two_sources_use_stable_tokens() {
    const auto descriptor_a = command_stream();
    const auto descriptor_b = command_stream();
    auto publisher_a_result = rbt::FixedPublisher<Command>::create(
        descriptor_a, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto publisher_b_result = rbt::FixedPublisher<Command>::create(
        descriptor_b, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto subscriber_a_result = rbt::FixedSubscriber<Command>::create(
        descriptor_a, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto subscriber_b_result = rbt::FixedSubscriber<Command>::create(
        descriptor_b, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto context_result = rbt::WaitContext::create("wait-two", 3U);
    require(publisher_a_result && publisher_b_result && subscriber_a_result && subscriber_b_result
            && context_result);

    auto publisher_a = std::move(publisher_a_result).value();
    auto publisher_b = std::move(publisher_b_result).value();
    auto subscriber_a = std::move(subscriber_a_result).value();
    auto subscriber_b = std::move(subscriber_b_result).value();
    auto context = std::move(context_result).value();
    const auto token_a = rbt::WaitToken(21U);
    const auto token_b = rbt::WaitToken(22U);
    const auto source_a = subscriber_a.event_source(token_a);
    const auto source_b = subscriber_b.event_source(token_b);
    std::atomic<bool> seen_a { false };
    std::atomic<bool> seen_b { false };

    std::jthread producer([publisher_a = std::move(publisher_a),
                           publisher_b = std::move(publisher_b)](std::stop_token) mutable {
        std::this_thread::sleep_for(15ms);
        auto first = publisher_a.loan();
        require(first);
        auto first_sample = std::move(first).value();
        first_sample.payload().sequence = 101U;
        require(first_sample.send());
        std::this_thread::sleep_for(15ms);
        auto second = publisher_b.loan();
        require(second);
        auto second_sample = std::move(second).value();
        second_sample.payload().sequence = 202U;
        require(second_sample.send());
    });

    const auto result = context.run({ source_a, source_b }, [&](rbt::ReadyEvent event, std::size_t) {
        if (event.is_synthetic()) {
            return rbt::WaitProgress::Continue;
        }
        if (event.token_value() == token_a) {
            auto received = subscriber_a.receive();
            require(received);
            if (received.value().has_value()) {
                auto sample = std::move(received).value();
                require(std::move(sample).value().payload().sequence == 101U);
                seen_a.store(true, std::memory_order_release);
                return seen_b.load(std::memory_order_acquire) ? rbt::WaitProgress::Stop
                                                               : rbt::WaitProgress::samples(1U);
            }
        }
        if (event.token_value() == token_b) {
            auto received = subscriber_b.receive();
            require(received);
            if (received.value().has_value()) {
                auto sample = std::move(received).value();
                require(std::move(sample).value().payload().sequence == 202U);
                seen_b.store(true, std::memory_order_release);
                return seen_a.load(std::memory_order_acquire) ? rbt::WaitProgress::Stop
                                                               : rbt::WaitProgress::samples(1U);
            }
        }
        return rbt::WaitProgress::Empty;
    });
    producer.join();

    require(result);
    require(result.value() == rbt::WaitExit::Stopped);
    require(seen_a.load(std::memory_order_acquire));
    require(seen_b.load(std::memory_order_acquire));
}

void endpoint_and_context_lifecycle_can_be_reused() {
    const auto descriptor_a = command_stream();
    auto publisher_a_result = rbt::FixedPublisher<Command>::create(
        descriptor_a, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto subscriber_a_result = rbt::FixedSubscriber<Command>::create(
        descriptor_a, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    require(publisher_a_result && subscriber_a_result);

    auto publisher_a = std::move(publisher_a_result).value();
    auto subscriber_a = std::move(subscriber_a_result).value();
    const auto token_a = rbt::WaitToken(101U);

    {
        const auto descriptor_b = command_stream();
        auto publisher_b_result = rbt::FixedPublisher<Command>::create(
            descriptor_b, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
        auto subscriber_b_result = rbt::FixedSubscriber<Command>::create(
            descriptor_b, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
        auto context_result = rbt::WaitContext::create("wait-lifecycle", 801U);
        require(publisher_b_result && subscriber_b_result && context_result);

        auto publisher_b = std::move(publisher_b_result).value();
        auto subscriber_b = std::move(subscriber_b_result).value();
        auto context = std::move(context_result).value();
        const auto token_b = rbt::WaitToken(102U);
        std::atomic<bool> sources_attached { false };
        std::jthread producer([&publisher_a, &publisher_b, &sources_attached](std::stop_token) {
            while (!sources_attached.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            require(send_command_until_delivered(publisher_a, 10U));
            require(send_command_until_delivered(publisher_b, 20U));
        });

        const auto source_a = subscriber_a.event_source(token_a);
        const auto source_b = subscriber_b.event_source(token_b);
        std::size_t synthetic_sources_seen = 0U;
        bool seen_a = false;
        bool seen_b = false;
        const auto first_result = context.run({ source_a, source_b }, [&](rbt::ReadyEvent event,
                                                                          std::size_t budget) {
            require(budget == rbt::MAX_DRAIN_PER_SOURCE);
            require(!event.is_cancellation());
            if (event.is_synthetic()) {
                ++synthetic_sources_seen;
                if (synthetic_sources_seen == 2U) {
                    sources_attached.store(true, std::memory_order_release);
                }
                return rbt::WaitProgress::Continue;
            }

            if (event.token_value() == token_a) {
                auto received = subscriber_a.receive();
                require(received);
                if (!received.value().has_value()) {
                    return rbt::WaitProgress::Empty;
                }
                auto optional_sample = std::move(received).value();
                auto sample = std::move(optional_sample).value();
                require(sample.payload().sequence == 10U);
                seen_a = true;
                return seen_b ? rbt::WaitProgress::stop_after_samples(1U)
                              : rbt::WaitProgress::samples(1U);
            }
            require(event.token_value() == token_b);
            auto received = subscriber_b.receive();
            require(received);
            if (!received.value().has_value()) {
                return rbt::WaitProgress::Empty;
            }
            auto optional_sample = std::move(received).value();
            auto sample = std::move(optional_sample).value();
            require(sample.payload().sequence == 20U);
            seen_b = true;
            return seen_a ? rbt::WaitProgress::stop_after_samples(1U)
                          : rbt::WaitProgress::samples(1U);
        });
        producer.join();

        require(first_result && first_result.value() == rbt::WaitExit::Stopped);
        require(seen_a && seen_b);
        require(context.stats().samples_processed() == 2U);

        // Consuming a sample leaves its readiness sidecar notification
        // pending.  Reusing the same context must classify that edge as an
        // empty wake before receiving the next payload normally.
        require(send_command_until_delivered(publisher_a, 30U));
        auto stale = subscriber_a.receive();
        require(stale && stale.value().has_value());
        auto stale_sample = std::move(stale).value();
        require(stale_sample.value().payload().sequence == 30U);
        stale_sample.reset();

        const auto empty_before = context.stats().empty_wakeups();
        std::atomic<bool> synthetic_seen { false };
        std::atomic<bool> spurious_seen { false };
        std::jthread second_producer([&publisher_a, &synthetic_seen, &spurious_seen](std::stop_token) {
            while (!synthetic_seen.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!spurious_seen.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            require(send_command_until_delivered(publisher_a, 40U));
        });

        bool saw_spurious = false;
        const auto source_again = subscriber_a.event_source(token_a);
        const auto second_result = context.run({ source_again }, [&](rbt::ReadyEvent event,
                                                                     std::size_t budget) {
            require(budget == rbt::MAX_DRAIN_PER_SOURCE);
            require(!event.is_cancellation());
            require(event.token_value() == token_a);
            if (event.is_synthetic()) {
                synthetic_seen.store(true, std::memory_order_release);
                return rbt::WaitProgress::Continue;
            }

            auto received = subscriber_a.receive();
            require(received);
            if (!received.value().has_value()) {
                saw_spurious = true;
                spurious_seen.store(true, std::memory_order_release);
                return rbt::WaitProgress::Empty;
            }
            auto optional_sample = std::move(received).value();
            auto sample = std::move(optional_sample).value();
            require(sample.payload().sequence == 40U);
            return rbt::WaitProgress::stop_after_samples(1U);
        });
        second_producer.join();

        require(second_result && second_result.value() == rbt::WaitExit::Stopped);
        require(saw_spurious);
        require(context.stats().empty_wakeups() == empty_before + 1U);
        require(context.stats().samples_processed() == 3U);
    }

    // The first context and source guards are gone, but A remains a usable
    // endpoint.  A replacement context must attach it and receive normally.
    auto replacement_result = rbt::WaitContext::create("wait-lifecycle", 802U);
    require(replacement_result);
    auto replacement = std::move(replacement_result).value();
    std::atomic<bool> replacement_attached { false };
    std::jthread replacement_producer([&publisher_a, &replacement_attached](std::stop_token) {
        while (!replacement_attached.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        require(send_command_until_delivered(publisher_a, 50U));
    });

    const auto replacement_source = subscriber_a.event_source(token_a);
    const auto replacement_wait = replacement.run({ replacement_source }, [&](rbt::ReadyEvent event,
                                                                               std::size_t budget) {
        require(budget == rbt::MAX_DRAIN_PER_SOURCE);
        require(!event.is_cancellation());
        require(event.token_value() == token_a);
        if (event.is_synthetic()) {
            replacement_attached.store(true, std::memory_order_release);
            return rbt::WaitProgress::Continue;
        }
        auto received = subscriber_a.receive();
        require(received);
        if (!received.value().has_value()) {
            return rbt::WaitProgress::Empty;
        }
        auto optional_sample = std::move(received).value();
        auto sample = std::move(optional_sample).value();
        require(sample.payload().sequence == 50U);
        return rbt::WaitProgress::stop_after_samples(1U);
    });
    replacement_producer.join();

    require(replacement_wait && replacement_wait.value() == rbt::WaitExit::Stopped);
    require(replacement.stats().samples_processed() == 1U);
    require(replacement.stats().empty_wakeups() == 0U);
}

void repeated_waitset_cycles_reuse_endpoints() {
    constexpr std::uint64_t kCycles = 100U;
    const auto descriptor = command_stream();
    auto publisher_result = rbt::FixedPublisher<Command>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto subscriber_result = rbt::FixedSubscriber<Command>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    require(publisher_result && subscriber_result);

    auto publisher = std::move(publisher_result).value();
    auto subscriber = std::move(subscriber_result).value();

    const auto token = rbt::WaitToken(201U);
    for (std::uint64_t cycle = 0U; cycle < kCycles; ++cycle) {
        auto context_result = rbt::WaitContext::create("wait-cycles", 1'000U + cycle);
        require(context_result);
        auto context = std::move(context_result).value();
        const auto wake = context.wake_handle();
        std::atomic<bool> source_attached { false };
        std::atomic<bool> sample_received { false };
        std::jthread producer([&publisher, &source_attached, &sample_received, wake, cycle](std::stop_token) {
            while (!source_attached.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            require(send_command_until_delivered(publisher, cycle));
            while (!sample_received.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            require(wake.cancel());
        });

        const auto source = subscriber.event_source(token);
        const auto result = context.run({ source }, [&](rbt::ReadyEvent event, std::size_t budget) {
            require(budget == rbt::MAX_DRAIN_PER_SOURCE);
            require(!event.is_cancellation());
            require(event.token_value() == token);
            if (event.is_synthetic()) {
                source_attached.store(true, std::memory_order_release);
                return rbt::WaitProgress::Continue;
            }

            auto received = subscriber.receive();
            require(received);
            if (!received.value().has_value()) {
                return rbt::WaitProgress::Empty;
            }
            auto optional_sample = std::move(received).value();
            auto sample = std::move(optional_sample).value();
            require(sample.payload().sequence == cycle);
            sample_received.store(true, std::memory_order_release);
            return rbt::WaitProgress::samples(1U);
        });
        producer.join();

        require(result && result.value() == rbt::WaitExit::Cancelled);
        require(context.stats().wakeups() >= 1U);
        require(context.stats().samples_processed() == 1U);
        require(context.stats().empty_wakeups() == 0U);
        require(context.stats().cancellation_wakeups() == 1U);
    }
}

void spurious_readiness_is_empty_and_sources_detach() {
    const auto descriptor = command_stream();
    auto publisher_result = rbt::FixedPublisher<Command>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto subscriber_result = rbt::FixedSubscriber<Command>::create(
        descriptor, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto context_result = rbt::WaitContext::create("wait-spurious", 4U);
    require(publisher_result && subscriber_result && context_result);
    auto publisher = std::move(publisher_result).value();
    auto subscriber = std::move(subscriber_result).value();
    auto context = std::move(context_result).value();

    auto loan = publisher.loan();
    require(loan);
    auto sample = std::move(loan).value();
    sample.payload().sequence = 9U;
    require(sample.send());
    auto received = subscriber.receive();
    require(received && received.value().has_value());
    std::move(received).value().reset();

    const auto wake = context.wake_handle();
    std::jthread canceller([wake](std::stop_token) {
        std::this_thread::sleep_for(40ms);
        require(wake.cancel());
    });
    const auto source = subscriber.event_source(rbt::WaitToken(31U));
    const auto result = context.run({ source }, [&](rbt::ReadyEvent event, std::size_t) {
        if (!event.is_synthetic()) {
            auto empty = subscriber.receive();
            require(empty && !empty.value().has_value());
            return rbt::WaitProgress::Empty;
        }
        return rbt::WaitProgress::Continue;
    });
    canceller.join();
    require(result && result.value() == rbt::WaitExit::Cancelled);
    require(context.stats().empty_wakeups() >= 1U);

    // The first run's guards have detached before source/subscriber teardown.
    {
        auto replacement_result = rbt::WaitContext::create("wait-spurious", 6U);
        require(replacement_result);
        auto replacement = std::move(replacement_result).value();
        const auto source_again = subscriber.event_source(rbt::WaitToken(32U));
        const auto stopped = replacement.run({ source_again }, [](rbt::ReadyEvent, std::size_t) {
            return rbt::WaitProgress::Stop;
        });
        require(stopped && stopped.value() == rbt::WaitExit::Stopped);
    }
}

} // namespace

int main() {
    readiness_failures_have_typed_stream_errors();
    fixed_source_wakes_and_receives();
    slice_source_wakes_and_receives_payload();
    throwing_handler_becomes_typed_error();
    synthetic_progress_counts_samples_only();
    stop_token_wakes_idle_wait();
    wake_handle_keeps_cancellation_node_alive();
    moved_context_is_safe_after_move_assignment();
    two_sources_use_stable_tokens();
    endpoint_and_context_lifecycle_can_be_reused();
    repeated_waitset_cycles_reuse_endpoints();
    spurious_readiness_is_empty_and_sources_detach();
    return 0;
}
