#include <rbt/control.hpp>
#include <rbt/sensor.hpp>
#include <rbt/stream.hpp>

#include <cstdint>
#include <thread>
#include <utility>

int main() {
    const rbt::StreamDescriptor fixed_stream(
        "rbt_cpp_compile_fixed", rbt::control::CONTROL_COMMAND_DESCRIPTOR, rbt::StreamProfile::Sensor);
    const rbt::StreamDescriptor slice_stream(
        "rbt_cpp_compile_slice", rbt::sensor::IMAGE_DESCRIPTOR, rbt::StreamProfile::Sensor);
    auto fixed_publisher = rbt::FixedPublisher<rbt::control::ControlCommand>::create(
        fixed_stream, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto fixed_subscriber = rbt::FixedSubscriber<rbt::control::ControlCommand>::create(
        fixed_stream, rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    auto slice_publisher = rbt::SlicePublisher<rbt::sensor::ImageHeader, std::uint8_t>::create(
        slice_stream, rbt::sensor::IMAGE_DESCRIPTOR, 16U);
    auto slice_subscriber = rbt::SliceSubscriber<rbt::sensor::ImageHeader, std::uint8_t>::create(
        slice_stream, rbt::sensor::IMAGE_DESCRIPTOR);
    if (!fixed_publisher || !fixed_subscriber || !slice_publisher || !slice_subscriber) {
        return 1;
    }
    auto fixed_loan_result = fixed_publisher.value().loan();
    if (!fixed_loan_result) {
        return 2;
    }

    auto fixed_loan = std::move(fixed_loan_result).value();
    auto moved_fixed_loan = std::move(fixed_loan);
    auto moved_fixed_send = fixed_loan.send();
    if (moved_fixed_send || moved_fixed_send.error() != rbt::StreamError::Transport) {
        return 3;
    }

    moved_fixed_loan.payload().value = 42.0F;
    moved_fixed_loan.payload().reserved = 0U;
    if (!moved_fixed_loan.send()) {
        return 4;
    }

    bool fixed_received = false;
    for (int i = 0; i < 100; ++i) {
        auto received = fixed_subscriber.value().receive();
        if (!received) {
            return 5;
        }
        if (received.value().has_value()) {
            auto received_optional = std::move(received).value();
            auto received_guard = std::move(received_optional).value();
            if (received_guard.payload().value != 42.0F) {
                return 6;
            }
            fixed_received = true;
            break;
        }
        std::this_thread::yield();
    }
    if (!fixed_received) {
        return 7;
    }

    {
        auto unsent_loan_result = fixed_publisher.value().loan();
        if (!unsent_loan_result) {
            return 8;
        }
        auto unsent_loan = std::move(unsent_loan_result).value();
        auto unsent_moved_loan = std::move(unsent_loan);
        unsent_moved_loan.payload().value = 7.0F;
    }

    auto slice_loan_result = slice_publisher.value().loan_slice(2U);
    if (!slice_loan_result) {
        return 9;
    }
    auto slice_loan = std::move(slice_loan_result).value();
    auto moved_slice_loan = std::move(slice_loan);
    auto moved_slice_send = slice_loan.send();
    if (moved_slice_send || moved_slice_send.error() != rbt::StreamError::Transport) {
        return 10;
    }

    moved_slice_loan.header().sequence = 9U;
    auto slice_payload = moved_slice_loan.payload();
    if (slice_payload.size() != 2U) {
        return 11;
    }
    slice_payload[0] = 1U;
    slice_payload[1] = 2U;
    if (!moved_slice_loan.send()) {
        return 12;
    }

    bool slice_received = false;
    for (int i = 0; i < 100; ++i) {
        auto received = slice_subscriber.value().receive();
        if (!received) {
            return 13;
        }
        if (received.value().has_value()) {
            auto received_optional = std::move(received).value();
            auto received_guard = std::move(received_optional).value();
            auto received_payload = received_guard.payload();
            if (received_guard.header().sequence != 9U || received_payload.size() != 2U
                || received_payload[1] != 2U) {
                return 14;
            }
            slice_received = true;
            break;
        }
        std::this_thread::yield();
    }
    if (!slice_received) {
        return 15;
    }
    return 0;
}
