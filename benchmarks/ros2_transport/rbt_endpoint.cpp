#include "common.hpp"

#include <rbt/runtime.hpp>
#include <rbt/stream.hpp>
#include <rbt/wait.hpp>
#include <rbt/transport_bench.hpp>

#include <memory>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace {

using namespace transport_bench;

template <typename Message>
void run_publisher(const Options& options, Window& window, Metrics& metrics) {
    const rbt::StreamDescriptor descriptor(
        options.topic,
        rbt::FixedMessageBinding<Message>::descriptor(),
        rbt::StreamProfile::Sensor);
    auto created = rbt::FixedPublisher<Message>::create(
        descriptor, rbt::FixedMessageBinding<Message>::descriptor());
    if (!created.has_value()) {
        throw std::runtime_error(std::string("RBT publisher initialization failed: ")
                                 + rbt::to_string(created.error()));
    }
    auto publisher = std::move(created).value();
    write_ready(options, Backend::Rbt, true, true);
    window = wait_for_window_file(options.window_file);

    run_publish_schedule(options, window, metrics, true,
        [&publisher](std::uint64_t sequence,
                     std::int64_t end_ns,
                     std::size_t expected_bytes,
                     std::uintptr_t& address) -> std::optional<std::int64_t> {
            auto loan = publisher.loan();
            if (!loan.has_value()) {
                throw std::runtime_error(std::string("RBT publisher loan failed: ")
                                         + rbt::to_string(loan.error()));
            }
            auto& message = loan.value().payload();
            fill_message(message, sequence, expected_bytes);
            address = reinterpret_cast<std::uintptr_t>(std::addressof(message));
            const auto sent_ns = static_cast<std::int64_t>(message.sent_ns);
            if (sent_ns >= end_ns || monotonic_now_ns() >= end_ns) {
                return std::nullopt;
            }
            auto sent = loan.value().send();
            if (!sent.has_value()) {
                throw std::runtime_error(std::string("RBT publisher send failed: ")
                                         + rbt::to_string(sent.error()));
            }
            return sent_ns;
        });
}

template <typename Message>
void run_subscriber(const Options& options, Window& window, Metrics& metrics) {
    const rbt::StreamDescriptor descriptor(
        options.topic,
        rbt::FixedMessageBinding<Message>::descriptor(),
        rbt::StreamProfile::Sensor);
    auto created = rbt::FixedSubscriber<Message>::create(
        descriptor, rbt::FixedMessageBinding<Message>::descriptor());
    if (!created.has_value()) {
        throw std::runtime_error(std::string("RBT subscriber initialization failed: ")
                                 + rbt::to_string(created.error()));
    }
    auto subscriber = std::move(created).value();
    write_ready(options, Backend::Rbt, true, true);
    window = wait_for_window_file(options.window_file);

    const auto process_instance = "rbt_bench_sub_" + std::to_string(::getpid());
    auto wait_context_result = rbt::WaitContext::create(process_instance, 1U);
    if (!wait_context_result.has_value()) {
        throw std::runtime_error(std::string("RBT WaitContext initialization failed: ")
                                 + rbt::to_string(wait_context_result.error()));
    }
    auto wait_context = std::move(wait_context_result).value();
    const auto token = rbt::WaitToken::new_token(1U);
    const std::vector<rbt::EventSource> sources { subscriber.event_source(token) };
    std::stop_source stop_source;
    const auto stop_at = window.end_ns + kShutdownDrainGraceNs;
    std::mutex deadline_mutex;
    std::condition_variable_any deadline_cv;
    std::jthread deadline_thread([&stop_source, &deadline_mutex, &deadline_cv, stop_at](
                                     std::stop_token stop_token) {
        std::unique_lock lock(deadline_mutex);
        const auto remaining_ns = std::max<std::int64_t>(stop_at - monotonic_now_ns(), 0LL);
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::nanoseconds(remaining_ns);
        deadline_cv.wait_until(lock, stop_token, deadline, [] { return false; });
        if (!stop_token.stop_requested()) {
            stop_source.request_stop();
        }
    });

    std::string receive_error;
    auto waited = wait_context.run(stop_source.get_token(), sources,
        [&subscriber, &metrics, &window, &receive_error](rbt::ReadyEvent,
                                                         std::size_t budget) {
            std::size_t processed = 0U;
            for (; processed < budget; ++processed) {
                auto received = subscriber.receive();
                if (!received.has_value()) {
                    receive_error = std::string("RBT subscriber receive failed: ")
                        + rbt::to_string(received.error());
                    return rbt::WaitProgress::stop();
                }
                auto sample = std::move(received).value();
                if (!sample.has_value()) {
                    break;
                }
                const auto& message = sample->payload();
                const auto receive_ns = monotonic_now_ns();
                const auto address = reinterpret_cast<std::uintptr_t>(std::addressof(message));
                record_received(metrics,
                                message,
                                receive_ns,
                                window,
                                payload_size(message),
                                address,
                                true);
            }
            if (processed == 0U) {
                return rbt::WaitProgress::empty();
            }
            return processed == budget ? rbt::WaitProgress::samples(processed)
                                       : rbt::WaitProgress::samples_and_empty(processed, 1U);
        });
    deadline_thread.request_stop();
    deadline_cv.notify_all();
    if (!waited.has_value()) {
        throw std::runtime_error(std::string("RBT subscriber WaitSet failed: ")
                                 + rbt::to_string(waited.error()));
    }
    if (!receive_error.empty()) {
        throw std::runtime_error(receive_error);
    }
    if (waited.value() != rbt::WaitExit::Cancelled) {
        throw std::runtime_error("RBT subscriber WaitSet stopped before shutdown deadline");
    }

    // Bound the final nonblocking drain; samples received after end_ns are not measured.
    for (std::size_t drained = 0U; drained < 64U; ++drained) {
        auto received = subscriber.receive();
        if (!received.has_value()) {
            throw std::runtime_error(std::string("RBT subscriber drain failed: ")
                                     + rbt::to_string(received.error()));
        }
        auto sample = std::move(received).value();
        if (!sample.has_value()) {
            break;
        }
        const auto& message = sample->payload();
        const auto receive_ns = monotonic_now_ns();
        const auto address = reinterpret_cast<std::uintptr_t>(std::addressof(message));
        record_received(metrics,
                        message,
                        receive_ns,
                        window,
                        payload_size(message),
                        address,
                        true);
    }
}

template <typename Message>
void run_case(const Options& options, Window& window, Metrics& metrics) {
    if (options.role == Role::Publisher) {
        run_publisher<Message>(options, window, metrics);
    } else {
        run_subscriber<Message>(options, window, metrics);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::optional<transport_bench::Options> options;
    transport_bench::Window window;
    transport_bench::Metrics metrics;
    metrics.address_probes.reserve(8U);
    try {
        options = transport_bench::parse_options(argc, argv, transport_bench::Backend::Rbt);
        switch (options->bench_case) {
        case transport_bench::BenchCase::Imu:
            run_case<rbt::transport_bench::ImuFrame>(*options, window, metrics);
            break;
        case transport_bench::BenchCase::Cloud:
            run_case<rbt::transport_bench::CloudFrame>(*options, window, metrics);
            break;
        case transport_bench::BenchCase::Video:
            run_case<rbt::transport_bench::VideoFrame>(*options, window, metrics);
            break;
        }
        transport_bench::write_output(*options, transport_bench::Backend::Rbt, window, metrics);
        return metrics.invalid == 0U ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "rbt_transport_bench: " << error.what() << '\n';
        if (options.has_value()) {
            try {
                transport_bench::write_output(
                    options.value(), transport_bench::Backend::Rbt, window, metrics, error.what());
            } catch (const std::exception& output_error) {
                std::cerr << "rbt_transport_bench: could not write output JSON: "
                          << output_error.what() << '\n';
            }
        }
        return 1;
    }
}
