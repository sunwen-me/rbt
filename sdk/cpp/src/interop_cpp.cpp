#include <rbt/control.hpp>
#include <rbt/sensor.hpp>
#include <rbt/stream.hpp>
#include <rbt/types.hpp>
#include <rbt/wait.hpp>

#include <algorithm>
#include <charconv>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

constexpr std::size_t kImageWidth = 1'920U;
constexpr std::size_t kImageHeight = 1'080U;
constexpr std::size_t kImageChannels = 3U;
constexpr std::size_t kImageBytes = kImageWidth * kImageHeight * kImageChannels;
constexpr std::uint64_t kImageChecksum = 793'152'000U;
constexpr std::size_t kPressureFrames = 40U;
constexpr std::uint64_t kPressurePeriodMs = 10U;
constexpr std::uint64_t kPressureOpenTimeoutMs = 5'000U;
constexpr std::uint64_t kPointCloudOpenPollMs = 100U;
constexpr std::uint64_t kPointCloudOpenTimeoutMs = 15'000U;
constexpr std::uint64_t kPointCloudReceiveTimeoutMs = 15'000U;
constexpr std::size_t kPointCloudCounts[] = { 0U, 1U, 100U, 100'000U };

template <typename T>
bool parse_unsigned(std::string_view text, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = first + text.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc {} && parsed.ptr == last;
}

void print_usage() {
    std::cerr << "usage: interop_cpp <image-subscriber|pointcloud-publisher|pointcloud-subscriber|control-publisher|"
                 "image-publisher|image-subscriber-hold|abi-mismatch-image> SERVICE [frames] [delay_ms]"
                 " [max_loans] [max_borrowed]\n";
}

rbt::StreamDescriptor image_stream(std::string_view service) {
    return rbt::StreamDescriptor(service, rbt::sensor::IMAGE_DESCRIPTOR, rbt::StreamProfile::Sensor);
}

rbt::StreamDescriptor image_stream(std::string_view service, rbt::StreamResourceConfig resources) {
    return rbt::StreamDescriptor(
        service, rbt::sensor::IMAGE_DESCRIPTOR, rbt::StreamProfile::Sensor, resources);
}

rbt::StreamDescriptor pointcloud_stream(std::string_view service) {
    return rbt::StreamDescriptor(service, rbt::sensor::POINT_CLOUD_DESCRIPTOR, rbt::StreamProfile::Sensor);
}

rbt::StreamDescriptor control_stream(std::string_view service) {
    return rbt::StreamDescriptor(service, rbt::control::CONTROL_COMMAND_DESCRIPTOR, rbt::StreamProfile::Control);
}

void fill_image_sample(rbt::LoanedSliceSample<rbt::sensor::ImageHeader, std::uint8_t>& sample,
                       std::uint64_t sequence) {
    auto& header = sample.header();
    header.sequence = sequence;
    header.timestamp_ns = sequence * 1'000'000U;
    header.width = static_cast<std::uint32_t>(kImageWidth);
    header.height = static_cast<std::uint32_t>(kImageHeight);
    header.stride = static_cast<std::uint32_t>(kImageWidth * kImageChannels);
    header.encoding = rbt::sensor::RGB8;

    const auto payload = sample.payload();
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>((sequence + index) % 256U);
    }
}

int image_subscriber(std::string_view service,
                     std::size_t frames,
                     std::uint64_t delay_ms,
                     rbt::StreamResourceConfig resources) {
    const auto descriptor = image_stream(service, resources);
    auto subscriber = rbt::SliceSubscriber<rbt::sensor::ImageHeader, std::uint8_t>::create_with_resources(
        descriptor, rbt::sensor::IMAGE_DESCRIPTOR, resources);
    if (!subscriber) {
        std::cerr << "IMAGE_CONSUMER create error=" << rbt::to_string(subscriber.error()) << '\n';
        return 1;
    }

    std::cout << "IMAGE_CONSUMER bound message=" << rbt::sensor::IMAGE_DESCRIPTOR.qualified_name()
              << " layout=" << rbt::lowercase_hex(rbt::sensor::IMAGE_DESCRIPTOR.layout_hash)
              << " payload=" << kImageBytes << " service=" << service << '\n';
    std::cout << "IMAGE_CONSUMER ready service=" << service << " frames=" << frames
              << " delay_ms=" << delay_ms << " buffer_size=" << subscriber.value().buffer_size()
              << " max_loans=" << resources.max_loans << " max_borrowed=" << resources.max_borrowed
              << std::endl;

    std::size_t received_count = 0U;
    std::optional<std::uint64_t> previous_sequence;
    std::size_t sequence_gaps = 0U;
    std::uint64_t estimated_dropped = 0U;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    auto endpoint = std::move(subscriber).value();
    auto context_result = rbt::WaitContext::create(rbt::generate_uuid(), 1U);
    if (!context_result) {
        std::cerr << "IMAGE_CONSUMER wait create error="
                  << rbt::to_string(context_result.error()) << '\n';
        return 1;
    }
    auto wait_context = std::move(context_result).value();
    std::stop_source wait_stop;
    std::mutex timer_mutex;
    std::condition_variable_any timer_condition;
    std::jthread timer([&](std::stop_token timer_stop) {
        std::unique_lock lock(timer_mutex);
        const bool stopped = timer_condition.wait_until(
            lock, timer_stop, deadline, [] { return false; });
        if (!stopped && !timer_stop.stop_requested()) {
            wait_stop.request_stop();
        }
    });

    bool handler_failed = false;
    const auto source = endpoint.event_source(rbt::WaitToken::new_token(1U));
    const auto wait_result = wait_context.run(
        wait_stop.get_token(), { source }, [&](rbt::ReadyEvent, std::size_t budget) {
            std::size_t drained = 0U;
            for (std::size_t index = 0U; index < budget && received_count < frames; ++index) {
                auto received = endpoint.receive();
                if (!received) {
                    std::cerr << "IMAGE_CONSUMER receive error="
                              << rbt::to_string(received.error()) << '\n';
                    handler_failed = true;
                    return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                }
                auto guarded = std::move(received).value();
                if (!guarded.has_value()) {
                    return rbt::WaitResult<rbt::WaitProgress>(
                        drained == 0U ? rbt::WaitProgress::empty()
                                      : rbt::WaitProgress::samples(drained));
                }

                const auto& sample = guarded.value();
                const auto& header = sample.header();
                const auto payload = sample.payload();
                const auto expected_timestamp = header.sequence * 1'000'000U;
                if (header.timestamp_ns != expected_timestamp || header.width != kImageWidth
                    || header.height != kImageHeight
                    || header.stride != kImageWidth * kImageChannels
                    || header.encoding.value != rbt::sensor::RGB8.value
                    || payload.size() != kImageBytes) {
                    std::cerr << "IMAGE_CONSUMER invalid payload/header sequence=" << header.sequence
                              << " timestamp_ns=" << header.timestamp_ns << " width=" << header.width
                              << " height=" << header.height << " stride=" << header.stride
                              << " encoding=" << header.encoding.value << " payload=" << payload.size() << '\n';
                    handler_failed = true;
                    return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                }

                std::uint64_t gap = 0U;
                if (previous_sequence.has_value()) {
                    if (header.sequence <= previous_sequence.value()) {
                        std::cerr << "IMAGE_CONSUMER sequence is not increasing: previous="
                                  << previous_sequence.value() << " current=" << header.sequence << '\n';
                        handler_failed = true;
                        return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                    }
                    gap = header.sequence - previous_sequence.value() - 1U;
                    if (gap > 0U) {
                        ++sequence_gaps;
                        estimated_dropped += gap;
                    }
                }
                previous_sequence = header.sequence;

                std::uint64_t checksum = 0U;
                for (const auto byte : payload) {
                    checksum += byte;
                }
                const auto middle = payload.size() / 2U;
                const auto expected_byte = [&header](std::size_t byte_index) {
                    return static_cast<unsigned>(
                        (header.sequence + static_cast<std::uint64_t>(byte_index)) % 256U);
                };
                if (checksum != kImageChecksum
                    || static_cast<unsigned>(payload.front()) != expected_byte(0U)
                    || static_cast<unsigned>(payload[middle]) != expected_byte(middle)
                    || static_cast<unsigned>(payload.back()) != expected_byte(payload.size() - 1U)) {
                    std::cerr << "IMAGE_CONSUMER invalid image pattern/checksum sequence="
                              << header.sequence << " checksum=" << checksum << " selected="
                              << static_cast<unsigned>(payload.front()) << ','
                              << static_cast<unsigned>(payload[middle]) << ','
                              << static_cast<unsigned>(payload.back()) << '\n';
                    handler_failed = true;
                    return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                }
                std::cout << "IMAGE_CONSUMER seq=" << header.sequence
                          << " timestamp_ns=" << header.timestamp_ns
                          << " encoding=" << header.encoding.value << " payload=" << payload.size()
                          << " checksum=" << checksum << " selected="
                          << static_cast<unsigned>(payload.front()) << ','
                          << static_cast<unsigned>(payload[middle]) << ','
                          << static_cast<unsigned>(payload.back()) << " pointer="
                          << static_cast<const void*>(payload.data()) << " gap=" << gap << '\n';
                ++received_count;
                ++drained;
                if (delay_ms > 0U && received_count < frames) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                }
            }
            if (received_count >= frames) {
                return rbt::WaitResult<rbt::WaitProgress>(
                    rbt::WaitProgress::stop_after_samples(drained));
            }
            return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitProgress::samples(drained));
        });
    timer.request_stop();
    timer.join();
    if (handler_failed || !wait_result) {
        if (!handler_failed) {
            std::cerr << "IMAGE_CONSUMER wait error=" << rbt::to_string(wait_result.error()) << '\n';
        }
        return 1;
    }
    if (wait_result.value() == rbt::WaitExit::Cancelled && received_count < frames) {
        std::cerr << "IMAGE_CONSUMER timed out waiting for frames\n";
        return 1;
    }
    std::cout << "IMAGE_CONSUMER complete service=" << service << " frames=" << received_count
              << " gaps=" << sequence_gaps << " estimated_dropped=" << estimated_dropped << '\n';
    return 0;
}

int image_publisher(std::string_view service,
                    std::size_t expected_subscribers,
                    rbt::StreamResourceConfig resources) {
    if (expected_subscribers == 0U) {
        std::cerr << "IMAGE_PUBLISHER expected_subscribers must be greater than zero\n";
        return 2;
    }

    const auto descriptor = image_stream(service, resources);
    auto publisher = rbt::SlicePublisher<rbt::sensor::ImageHeader, std::uint8_t>::create_with_resources(
        descriptor, rbt::sensor::IMAGE_DESCRIPTOR, kImageBytes, resources);
    if (!publisher) {
        std::cerr << "IMAGE_PUBLISHER create error=" << rbt::to_string(publisher.error()) << '\n';
        return 1;
    }

    std::cout << "IMAGE_PUBLISHER ready service=" << service << " frames=" << kPressureFrames
              << " period_ms=" << kPressurePeriodMs << " expected_subscribers=" << expected_subscribers
              << " payload=" << kImageBytes
              << " hash=" << rbt::lowercase_hex(rbt::sensor::IMAGE_DESCRIPTOR.layout_hash)
              << " max_loans=" << resources.max_loans << " max_borrowed=" << resources.max_borrowed
              << std::endl;

    const auto open_deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kPressureOpenTimeoutMs);
    std::size_t warmup_recipients = 0U;
    std::size_t warmup_attempts = 0U;
    std::size_t send_errors = 0U;
    while (warmup_recipients < expected_subscribers
           && std::chrono::steady_clock::now() < open_deadline) {
        auto loan = publisher.value().loan_slice(kImageBytes);
        if (!loan) {
            std::cerr << "IMAGE_PUBLISHER warmup loan error=" << rbt::to_string(loan.error()) << '\n';
            return 1;
        }
        fill_image_sample(loan.value(), 0U);
        auto sent = loan.value().send();
        ++warmup_attempts;
        if (!sent) {
            ++send_errors;
            std::cerr << "IMAGE_PUBLISHER warmup send error=" << rbt::to_string(sent.error()) << '\n';
        } else {
            warmup_recipients = std::max(warmup_recipients, sent.value());
        }
        if (warmup_recipients < expected_subscribers) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (warmup_recipients < expected_subscribers) {
        std::cerr << "IMAGE_PUBLISHER timed out waiting for subscribers expected=" << expected_subscribers
                  << " observed=" << warmup_recipients << " attempts=" << warmup_attempts << '\n';
        return 1;
    }
    std::cout << "IMAGE_PUBLISHER subscribers=" << warmup_recipients << " warmup_attempts=" << warmup_attempts
              << std::endl;

    const auto started = std::chrono::steady_clock::now();
    std::size_t delivered = 0U;
    std::size_t undelivered = 0U;
    std::uint64_t max_send_ms = 0U;
    for (std::size_t sequence = 1U; sequence <= kPressureFrames; ++sequence) {
        auto loan = publisher.value().loan_slice(kImageBytes);
        if (!loan) {
            std::cerr << "IMAGE_PUBLISHER loan error=" << rbt::to_string(loan.error()) << '\n';
            return 1;
        }
        fill_image_sample(loan.value(), static_cast<std::uint64_t>(sequence));
        const auto send_started = std::chrono::steady_clock::now();
        auto sent = loan.value().send();
        const auto send_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - send_started);
        max_send_ms = std::max(max_send_ms, static_cast<std::uint64_t>(send_elapsed.count()));
        if (!sent) {
            ++send_errors;
            std::cerr << "IMAGE_PUBLISHER send error=" << rbt::to_string(sent.error())
                      << " sequence=" << sequence << '\n';
            return 1;
        }
        if (sent.value() > 0U) {
            ++delivered;
        } else {
            ++undelivered;
        }
        if (sequence < kPressureFrames) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPressurePeriodMs));
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    std::cout << "IMAGE_PUBLISHER complete service=" << service << " frames=" << kPressureFrames
              << " delivered=" << delivered << " undelivered=" << undelivered << " send_errors=" << send_errors
              << " elapsed_ms=" << elapsed.count() << " max_send_ms=" << max_send_ms << '\n';
    return 0;
}

int image_subscriber_hold(std::string_view service, std::size_t hold_ms) {
    if (hold_ms == 0U) {
        std::cerr << "IMAGE_CONSUMER_HOLD hold_ms must be greater than zero\n";
        return 2;
    }
    auto subscriber = rbt::SliceSubscriber<rbt::sensor::ImageHeader, std::uint8_t>::create(
        image_stream(service), rbt::sensor::IMAGE_DESCRIPTOR);
    if (!subscriber) {
        std::cerr << "IMAGE_CONSUMER_HOLD create error=" << rbt::to_string(subscriber.error()) << '\n';
        return 1;
    }
    std::cout << "IMAGE_CONSUMER_HOLD ready service=" << service << " hold_ms=" << hold_ms
              << " buffer_size=" << subscriber.value().buffer_size() << " received=0" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    std::cout << "IMAGE_CONSUMER_HOLD complete service=" << service << " hold_ms=" << hold_ms
              << " received=0" << '\n';
    return 0;
}

rbt::sensor::PointXYZI point_at(std::size_t index) {
    const float value = static_cast<float>(index);
    return rbt::sensor::PointXYZI { value, value * 2.0F, -value, static_cast<float>(index % 31U) };
}

std::uint64_t point_checksum(const rbt::sensor::PointXYZI& point) {
    std::uint64_t checksum = std::bit_cast<std::uint32_t>(point.x);
    checksum = checksum * 31U + std::bit_cast<std::uint32_t>(point.y);
    checksum = checksum * 31U + std::bit_cast<std::uint32_t>(point.z);
    checksum = checksum * 31U + std::bit_cast<std::uint32_t>(point.intensity);
    return checksum;
}

int pointcloud_publisher(std::string_view service) {
    auto publisher = rbt::SlicePublisher<rbt::sensor::PointCloudHeader, rbt::sensor::PointXYZI>::create(
        pointcloud_stream(service), rbt::sensor::POINT_CLOUD_DESCRIPTOR, 100'000U);
    if (!publisher) {
        std::cerr << "POINTCLOUD_PUBLISHER create error=" << rbt::to_string(publisher.error()) << '\n';
        return 1;
    }

    std::cout << "POINTCLOUD_PUBLISHER message=" << rbt::sensor::POINT_CLOUD_DESCRIPTOR.qualified_name()
              << " layout=" << rbt::lowercase_hex(rbt::sensor::POINT_CLOUD_DESCRIPTOR.layout_hash)
              << " service=" << service << '\n';

    constexpr std::size_t counts[] = { 0U, 1U, 100U, 100'000U };
    std::size_t sequence = 0U;
    for (const auto count : counts) {
        bool delivered = false;
        for (std::size_t attempt = 0U; attempt < 2'000U && !delivered; ++attempt) {
            auto loan = publisher.value().loan_slice(count);
            if (!loan) {
                std::cerr << "POINTCLOUD_PUBLISHER loan error=" << rbt::to_string(loan.error()) << '\n';
                return 1;
            }
            auto& header = loan.value().header();
            header.sequence = static_cast<std::uint64_t>(sequence);
            header.timestamp_ns = header.sequence * 1'000'000U;
            header.width = static_cast<std::uint32_t>(count);
            header.height = 1U;
            header.dense = 1U;
            for (auto& byte : header.reserved) {
                byte = 0U;
            }

            const auto payload = loan.value().payload();
            std::uint64_t checksum = 0U;
            for (std::size_t index = 0U; index < payload.size(); ++index) {
                payload[index] = point_at(index);
                checksum += point_checksum(payload[index]);
            }
            const float first_x = count == 0U ? 0.0F : payload.front().x;
            const float last_x = count == 0U ? 0.0F : payload.back().x;
            const auto frame_sequence = header.sequence;
            const auto frame_timestamp_ns = header.timestamp_ns;
            auto sent = loan.value().send();
            if (!sent) {
                if (sent.error() == rbt::StreamError::UnableToDeliver) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                std::cerr << "POINTCLOUD_PUBLISHER send error=" << rbt::to_string(sent.error()) << '\n';
                return 1;
            }
            delivered = sent.value() > 0U;
            if (delivered) {
                std::cout << "POINTCLOUD_PUBLISHER elements=" << count << " checksum=" << checksum
                          << " first=" << first_x << " last=" << last_x << " recipients=" << sent.value()
                          << " sequence=" << frame_sequence << " timestamp_ns=" << frame_timestamp_ns << '\n';
            }
            if (!delivered) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (!delivered) {
            std::cerr << "POINTCLOUD_PUBLISHER timed out waiting for a recipient\n";
            return 1;
        }
        ++sequence;
        if (sequence < std::size(counts)) {
            // This fixture validates every advertised payload shape in order.
            // Sensor intentionally has a depth-one replacing queue, so leave
            // enough time for instrumented Python/ASan consumers to finish
            // their zero-copy ownership assertions before publishing the next
            // shape.  This is fixture pacing, not a transport guarantee.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    return 0;
}

void print_point(std::ostream& output, const rbt::sensor::PointXYZI* point) {
    if (point == nullptr) {
        output << "none";
        return;
    }
    output << '(' << point->x << ',' << point->y << ',' << point->z << ',' << point->intensity << ')';
}

bool points_equal(const rbt::sensor::PointXYZI& actual,
                  const rbt::sensor::PointXYZI& expected) {
    return actual.x == expected.x && actual.y == expected.y && actual.z == expected.z
        && actual.intensity == expected.intensity;
}

bool receive_pointcloud_frame(
    rbt::SliceSubscriber<rbt::sensor::PointCloudHeader, rbt::sensor::PointXYZI>& subscriber,
    std::string_view service,
    std::uint64_t expected_sequence,
    std::size_t expected_count) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kPointCloudReceiveTimeoutMs);
    auto context_result = rbt::WaitContext::create(rbt::generate_uuid(), 1U);
    if (!context_result) {
        std::cerr << "POINTCLOUD_SUBSCRIBER wait create error="
                  << rbt::to_string(context_result.error()) << " sequence=" << expected_sequence
                  << " elements=" << expected_count << '\n';
        return false;
    }
    auto wait_context = std::move(context_result).value();
    std::stop_source wait_stop;
    std::mutex timer_mutex;
    std::condition_variable_any timer_condition;
    std::jthread timer([&](std::stop_token timer_stop) {
        std::unique_lock lock(timer_mutex);
        const bool stopped = timer_condition.wait_until(
            lock, timer_stop, deadline, [] { return false; });
        if (!stopped && !timer_stop.stop_requested()) {
            wait_stop.request_stop();
        }
    });

    bool handler_failed = false;
    const auto source = subscriber.event_source(rbt::WaitToken::new_token(1U));
    const auto wait_result = wait_context.run(
        wait_stop.get_token(), { source }, [&](rbt::ReadyEvent, std::size_t budget) {
            if (budget == 0U) {
                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitProgress::empty());
            }
            auto received = subscriber.receive();
            if (!received) {
                std::cerr << "POINTCLOUD_SUBSCRIBER receive error="
                          << rbt::to_string(received.error()) << " sequence=" << expected_sequence
                          << " elements=" << expected_count << '\n';
                handler_failed = true;
                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
            }
            auto guarded = std::move(received).value();
            if (!guarded.has_value()) {
                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitProgress::empty());
            }

            const auto& sample = guarded.value();
            const auto& header = sample.header();
            const auto expected_timestamp = expected_sequence * 1'000'000U;
            const bool reserved_zero = std::all_of(
                std::begin(header.reserved), std::end(header.reserved), [](std::uint8_t byte) {
                    return byte == 0U;
                });
            if (header.sequence != expected_sequence || header.timestamp_ns != expected_timestamp
                || header.width != expected_count || header.height != 1U || header.dense != 1U
                || !reserved_zero) {
                std::cerr << "POINTCLOUD_SUBSCRIBER invalid header expected sequence="
                          << expected_sequence << " timestamp_ns=" << expected_timestamp
                          << " width=" << expected_count << " height=1 dense=1 reserved=[0;7] actual sequence="
                          << header.sequence << " timestamp_ns=" << header.timestamp_ns
                          << " width=" << header.width << " height=" << header.height
                          << " dense=" << static_cast<unsigned>(header.dense)
                          << " reserved_zero=" << (reserved_zero ? 1 : 0) << '\n';
                handler_failed = true;
                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
            }

            const auto payload = sample.payload();
            const auto expected_bytes = expected_count * sizeof(rbt::sensor::PointXYZI);
            const bool aligned = payload.empty()
                || reinterpret_cast<std::uintptr_t>(payload.data()) % alignof(rbt::sensor::PointXYZI) == 0U;
            if (payload.size() != expected_count || payload.size_bytes() != expected_bytes || !aligned) {
                std::cerr << "POINTCLOUD_SUBSCRIBER invalid payload sequence=" << expected_sequence
                          << " expected_elements=" << expected_count << " actual_elements=" << payload.size()
                          << " expected_bytes=" << expected_bytes << " actual_bytes=" << payload.size_bytes()
                          << " aligned=" << (aligned ? 1 : 0) << '\n';
                handler_failed = true;
                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
            }

            if (!payload.empty()) {
                const auto expected_first = point_at(0U);
                const auto expected_last = point_at(expected_count - 1U);
                if (!points_equal(payload.front(), expected_first)
                    || !points_equal(payload.back(), expected_last)) {
                    std::cerr << "POINTCLOUD_SUBSCRIBER invalid endpoints sequence=" << expected_sequence
                              << " expected_first=";
                    print_point(std::cerr, &expected_first);
                    std::cerr << " actual_first=";
                    print_point(std::cerr, &payload.front());
                    std::cerr << " expected_last=";
                    print_point(std::cerr, &expected_last);
                    std::cerr << " actual_last=";
                    print_point(std::cerr, &payload.back());
                    std::cerr << '\n';
                    handler_failed = true;
                    return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                }
            }

            std::uint64_t checksum = 0U;
            std::uint64_t expected_checksum = 0U;
            for (std::size_t index = 0U; index < payload.size(); ++index) {
                checksum += point_checksum(payload[index]);
                expected_checksum += point_checksum(point_at(index));
            }
            if (checksum != expected_checksum) {
                std::cerr << "POINTCLOUD_SUBSCRIBER invalid checksum sequence=" << expected_sequence
                          << " elements=" << expected_count << " expected=" << expected_checksum
                          << " actual=" << checksum << '\n';
                handler_failed = true;
                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
            }

            std::cout << "POINTCLOUD_SUBSCRIBER sequence=" << header.sequence
                      << " elements=" << payload.size() << " first=";
            print_point(std::cout, payload.empty() ? nullptr : &payload.front());
            std::cout << " last=";
            print_point(std::cout, payload.empty() ? nullptr : &payload.back());
            std::cout << " checksum=" << checksum
                      << " hash=" << rbt::lowercase_hex(rbt::sensor::POINT_CLOUD_DESCRIPTOR.layout_hash)
                      << '\n';
            return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitProgress::stop_after_samples(1U));
        });
    timer.request_stop();
    timer.join();

    if (handler_failed || !wait_result) {
        if (!handler_failed) {
            std::cerr << "POINTCLOUD_SUBSCRIBER wait error="
                      << rbt::to_string(wait_result.error()) << " sequence=" << expected_sequence
                      << " elements=" << expected_count << '\n';
        }
        return false;
    }
    if (wait_result.value() == rbt::WaitExit::Cancelled) {
        std::cerr << "POINTCLOUD_SUBSCRIBER timed out waiting for sequence=" << expected_sequence
                  << " elements=" << expected_count << " service=" << service << '\n';
        return false;
    }
    return true;
}

int pointcloud_subscriber(std::string_view service) {
    using Subscriber = rbt::SliceSubscriber<rbt::sensor::PointCloudHeader, rbt::sensor::PointXYZI>;

    const auto open_deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kPointCloudOpenTimeoutMs);
    std::optional<Subscriber> subscriber;
    while (!subscriber.has_value()) {
        auto attempt = Subscriber::create(pointcloud_stream(service), rbt::sensor::POINT_CLOUD_DESCRIPTOR);
        if (attempt) {
            subscriber.emplace(std::move(attempt).value());
            break;
        }
        const auto error = attempt.error();
        if (std::chrono::steady_clock::now() >= open_deadline) {
            std::cerr << "POINTCLOUD_SUBSCRIBER timed out opening existing service \"" << service
                      << "\" error=" << rbt::to_string(error) << '\n';
            return 1;
        }
        std::cerr << "POINTCLOUD_SUBSCRIBER waiting for existing service \"" << service
                  << "\": error=" << rbt::to_string(error) << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(kPointCloudOpenPollMs));
    }

    auto endpoint = std::move(subscriber).value();
    std::cout << "POINTCLOUD_SUBSCRIBER ready service=" << service
              << " counts=0,1,100,100000 hash="
              << rbt::lowercase_hex(rbt::sensor::POINT_CLOUD_DESCRIPTOR.layout_hash) << std::endl;
    for (std::size_t sequence = 0U; sequence < std::size(kPointCloudCounts); ++sequence) {
        if (!receive_pointcloud_frame(endpoint,
                                      service,
                                      static_cast<std::uint64_t>(sequence),
                                      kPointCloudCounts[sequence])) {
            return 1;
        }
    }
    std::cout << "POINTCLOUD_SUBSCRIBER complete service=" << service
              << " frames=" << std::size(kPointCloudCounts)
              << " hash=" << rbt::lowercase_hex(rbt::sensor::POINT_CLOUD_DESCRIPTOR.layout_hash) << '\n';
    return 0;
}

int control_publisher(std::string_view service) {
    auto publisher = rbt::FixedPublisher<rbt::control::ControlCommand>::create(
        control_stream(service), rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    if (!publisher) {
        std::cerr << "CONTROL_PUBLISHER create error=" << rbt::to_string(publisher.error()) << '\n';
        return 1;
    }

    std::size_t sequence = 0U;
    std::size_t delivered = 0U;
    bool connected = false;
    std::size_t attempts = 0U;
    while (!connected && attempts < 2'000U) {
        auto loan = publisher.value().loan();
        if (!loan) {
            std::cerr << "CONTROL_PUBLISHER loan error=" << rbt::to_string(loan.error()) << '\n';
            return 1;
        }
        loan.value().payload().sequence = sequence;
        loan.value().payload().value = static_cast<float>(sequence);
        loan.value().payload().reserved = 0U;
        auto sent = loan.value().send();
        if (!sent) {
            if (sent.error() == rbt::StreamError::QueueFull) {
                std::cout << "[control-test] queue saturation sequence=" << sequence << " error=QueueFull\n";
                break;
            }
            std::cerr << "CONTROL_PUBLISHER send error=" << rbt::to_string(sent.error()) << '\n';
            return 1;
        }
        if (sent.value() > 0U) {
            connected = true;
            ++delivered;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++attempts;
    }
    if (!connected) {
        std::cerr << "CONTROL_PUBLISHER timed out waiting for a subscriber\n";
        return 1;
    }
    ++sequence;

    bool queue_full = false;
    while (sequence < 64U) {
        auto loan = publisher.value().loan();
        if (!loan) {
            std::cerr << "CONTROL_PUBLISHER loan error=" << rbt::to_string(loan.error()) << '\n';
            return 1;
        }
        loan.value().payload().sequence = sequence;
        loan.value().payload().value = static_cast<float>(sequence);
        loan.value().payload().reserved = 0U;
        auto sent = loan.value().send();
        if (!sent) {
            if (sent.error() == rbt::StreamError::QueueFull) {
                queue_full = true;
                std::cout << "[control-test] queue saturation sequence=" << sequence << " error=QueueFull\n";
                break;
            }
            std::cerr << "CONTROL_PUBLISHER send error=" << rbt::to_string(sent.error()) << '\n';
            return 1;
        }
        if (sent.value() > 0U) {
            ++delivered;
            ++sequence;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::cout << "[control-test] sent=" << sequence + (queue_full ? 1U : 0U)
              << " delivered=" << delivered << " profile=Control\n";
    return queue_full ? 0 : 1;
}

int abi_mismatch_image(std::string_view service) {
    auto expected = rbt::sensor::IMAGE_DESCRIPTOR;
    expected.layout_hash[0] ^= 0xFFU;
    auto subscriber = rbt::SliceSubscriber<rbt::sensor::ImageHeader, std::uint8_t>::create(
        rbt::StreamDescriptor(service, rbt::sensor::IMAGE_DESCRIPTOR, rbt::StreamProfile::Sensor), expected);
    if (subscriber || subscriber.error() != rbt::StreamError::AbiMismatch) {
        std::cerr << "ABI_MISMATCH expected AbiMismatch\n";
        return 1;
    }
    std::cout << "binding rejected: message=" << expected.qualified_name()
              << " expected_layout=" << rbt::lowercase_hex(expected.layout_hash)
              << " provider_layout=" << rbt::lowercase_hex(rbt::sensor::IMAGE_DESCRIPTOR.layout_hash)
              << " expected_abi=" << expected.abi_major << " provider_abi="
              << rbt::sensor::IMAGE_DESCRIPTOR.abi_major
              << " error=AbiMismatch stage=preflight transport_opened=false\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 2;
    }
    const std::string_view command(argv[1]);
    const std::string_view service(argv[2]);
    if (command == "pointcloud-subscriber" && argc != 3) {
        print_usage();
        return 2;
    }

    if (command == "image-subscriber") {
        std::size_t frames = 3U;
        std::uint64_t delay_ms = 0U;
        rbt::StreamResourceConfig resources;
        if (argc >= 4 && !parse_unsigned(std::string_view(argv[3]), frames)) {
            std::cerr << "frames must be an unsigned integer\n";
            return 2;
        }
        if (argc >= 5 && !parse_unsigned(std::string_view(argv[4]), delay_ms)) {
            std::cerr << "delay_ms must be an unsigned integer\n";
            return 2;
        }
        if (argc >= 6 && !parse_unsigned(std::string_view(argv[5]), resources.max_loans)) {
            std::cerr << "max_loans must be an unsigned integer\n";
            return 2;
        }
        if (argc >= 7 && !parse_unsigned(std::string_view(argv[6]), resources.max_borrowed)) {
            std::cerr << "max_borrowed must be an unsigned integer\n";
            return 2;
        }
        if (argc >= 8) {
            print_usage();
            return 2;
        }
        return image_subscriber(service, frames, delay_ms, resources);
    }

    if (command == "image-publisher") {
        std::size_t expected_subscribers = 1U;
        rbt::StreamResourceConfig resources;
        if (argc >= 4 && !parse_unsigned(std::string_view(argv[3]), expected_subscribers)) {
            std::cerr << "expected_subscribers must be an unsigned integer\n";
            return 2;
        }
        if (argc >= 5 && !parse_unsigned(std::string_view(argv[4]), resources.max_loans)) {
            std::cerr << "max_loans must be an unsigned integer\n";
            return 2;
        }
        if (argc >= 6 && !parse_unsigned(std::string_view(argv[5]), resources.max_borrowed)) {
            std::cerr << "max_borrowed must be an unsigned integer\n";
            return 2;
        }
        if (argc >= 7) {
            print_usage();
            return 2;
        }
        return image_publisher(service, expected_subscribers, resources);
    }

    std::size_t frames = 3U;
    std::uint64_t delay_ms = 0U;
    if (argc >= 4 && !parse_unsigned(std::string_view(argv[3]), frames)) {
        std::cerr << "frames must be an unsigned integer\n";
        return 2;
    }
    if (argc >= 5 && !parse_unsigned(std::string_view(argv[4]), delay_ms)) {
        std::cerr << "delay_ms must be an unsigned integer\n";
        return 2;
    }
    if (argc >= 6) {
        print_usage();
        return 2;
    }

    if (command == "image-subscriber-hold") {
        return image_subscriber_hold(service, frames);
    }
    if (command == "pointcloud-publisher") {
        return pointcloud_publisher(service);
    }
    if (command == "pointcloud-subscriber") {
        return pointcloud_subscriber(service);
    }
    if (command == "control-publisher") {
        return control_publisher(service);
    }
    if (command == "abi-mismatch-image") {
        return abi_mismatch_image(service);
    }
    print_usage();
    return 2;
}
