#include <rbt/demo.hpp>
#include <rbt/rpc.hpp>
#include <rbt/sensor.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using FixedRequest = rbt::demo::Frame;
using FixedResponse = rbt::demo::Detection;
using FixedClient = rbt::RpcClient<FixedRequest, FixedResponse>;
using FixedServer = rbt::RpcServer<FixedRequest, FixedResponse>;
using FixedServerRequest = rbt::RpcRequest<FixedRequest, FixedResponse>;
using FixedPending = rbt::PendingRpcCall<FixedRequest, FixedResponse>;
using Slice = iox2::bb::Slice<std::uint8_t>;
using ImageHeader = rbt::sensor::ImageHeader;
using DynamicClient = rbt::RpcClient<Slice, Slice, ImageHeader, ImageHeader>;
using DynamicServer = rbt::RpcServer<Slice, Slice, ImageHeader, ImageHeader>;

constexpr std::size_t kDefaultCalls = 4U;
constexpr std::size_t kMaxCalls = 1'000'000U;
constexpr std::size_t kBurstWindow = 16U;
constexpr auto kCallTimeout = 3s;
constexpr std::uint64_t kNonceBase = 0xBADC'0000'0000'0000ULL;
constexpr std::uint64_t kMarkerBase = 0xC0DE'0000ULL;
constexpr std::uint64_t kOperandBase = 0x0100ULL;
constexpr std::uint64_t kOperandStep = 17ULL;
constexpr std::uint64_t kTransformMultiplier = 0x9E37'79B1'85EB'CA87ULL;
constexpr std::uint64_t kTransformXor = 0xD1CE'F00D'CAFE'BEEFULL;
constexpr std::size_t kDynamicRequestBytes = 4'096U;
constexpr std::size_t kDynamicResponseBytes = 65'536U;
constexpr std::uint64_t kDynamicTimestampBase = 0x1020'3040'5060'7000ULL;

bool quiet() {
    const auto* value = std::getenv("RBT_RPC_INTEROP_QUIET");
    return value != nullptr && std::string(value) == "1";
}

bool report_index(std::size_t index) {
    return !quiet() || index % 100U == 0U;
}

void client_pace() {
    const auto* value = std::getenv("RBT_RPC_INTEROP_PACE_MS");
    if (value == nullptr) {
        return;
    }
    try {
        const auto milliseconds = std::stoull(value);
        if (milliseconds != 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        }
    } catch (const std::exception&) {
        // Invalid pacing is deliberately treated as no pacing. It is only a
        // soak-harness aid and must not change interop correctness.
    }
}

void print_usage(const char* program) {
    std::cerr << "usage: " << program
              << " <client|server|burst-client|burst-server|dynamic-client|dynamic-server> <service-name> [calls]\n"
                 "calls must be between 1 and 1000000 (default 4)\n";
}

bool parse_calls(const char* text, std::size_t& calls) {
    try {
        const auto parsed = std::stoull(text);
        if (parsed == 0U || parsed > kMaxCalls
            || parsed > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        calls = static_cast<std::size_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

FixedRequest request_for(std::size_t index) {
    const auto index_value = static_cast<std::uint64_t>(index);
    const auto marker = kMarkerBase | index_value;
    const auto operand = kOperandBase + index_value * kOperandStep;
    return FixedRequest { kNonceBase | index_value, (marker << 32U) | operand };
}

std::uint64_t expected_value(std::uint64_t nonce, std::uint64_t value) {
    return (nonce * kTransformMultiplier) ^ std::rotl(value, 23) ^ kTransformXor;
}

bool validate_request(std::size_t index, const FixedRequest& request) {
    const auto expected = request_for(index);
    if (request.sequence != expected.sequence || request.value != expected.value) {
        std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=server"
                  << " index=" << index << " expected_nonce=" << expected.sequence
                  << " actual_nonce=" << request.sequence
                  << " expected_value=" << expected.value
                  << " actual_value=" << request.value << '\n';
        return false;
    }
    return true;
}

rbt::RpcResourceConfig resources() {
    return rbt::RpcResourceConfig::defaults()
        .with_max_in_flight(1U)
        .with_max_clients(1U)
        .with_max_request_loans(1U)
        .with_response_buffer_size(1U)
        .with_max_borrowed_responses(1U)
        .with_max_response_loans(1U)
        .with_max_request_slice_len(1U)
        .with_max_response_slice_len(1U);
}

rbt::RpcDescriptor descriptor(const std::string& service) {
    return rbt::RpcDescriptor::for_fixed_with_resources<FixedRequest, FixedResponse>(
        service, resources());
}

rbt::RpcResourceConfig burst_resources() {
    return rbt::RpcResourceConfig::defaults()
        .with_max_in_flight(kBurstWindow)
        .with_max_clients(1U)
        .with_max_request_loans(kBurstWindow)
        .with_response_buffer_size(1U)
        .with_max_borrowed_responses(1U)
        .with_max_response_loans(1U)
        .with_max_request_slice_len(1U)
        .with_max_response_slice_len(1U);
}

rbt::RpcDescriptor burst_descriptor(const std::string& service) {
    return rbt::RpcDescriptor::for_fixed_with_resources<FixedRequest, FixedResponse>(
        service, burst_resources());
}

rbt::RpcDescriptor dynamic_descriptor(const std::string& service) {
    return rbt::RpcDescriptor::for_slice_with_resources<ImageHeader,
                                                         std::uint8_t,
                                                         ImageHeader,
                                                         std::uint8_t>(
        service,
        resources()
            .with_max_request_slice_len(kDynamicRequestBytes)
            .with_max_response_slice_len(kDynamicResponseBytes));
}

std::uint64_t dynamic_nonce(std::size_t index) {
    return kNonceBase | static_cast<std::uint64_t>(index);
}

std::uint8_t dynamic_request_byte(std::size_t index, std::size_t offset) {
    return static_cast<std::uint8_t>(
        (static_cast<std::uint64_t>(index) * 17U
         + static_cast<std::uint64_t>(offset) * 31U + 7U)
        & 0xffU);
}

std::uint8_t dynamic_response_byte(std::size_t index, std::size_t offset) {
    return static_cast<std::uint8_t>(
        (static_cast<std::uint64_t>(index) * 29U
         + static_cast<std::uint64_t>(offset) * 13U + 0xa5U)
        & 0xffU);
}

template <typename Element>
std::uint64_t dynamic_checksum(const iox2::bb::Slice<Element>& bytes) {
    std::uint64_t checksum = 14'695'981'039'346'656'037ULL;
    for (std::uint64_t offset = 0U; offset < bytes.number_of_elements(); ++offset) {
        checksum = (checksum ^ bytes[offset]) * 1'099'511'628'211ULL;
    }
    return checksum;
}

bool report_idle(const std::string& role,
                 const std::string& service,
                 std::size_t calls,
                 bool client_side) {
    const auto snapshots = rbt::local_rpc_pressure_snapshots();
    std::vector<const rbt::RpcPressure*> matches;
    for (const auto& snapshot : snapshots) {
        const bool has_expected_side =
            client_side ? snapshot.client.has_value() : snapshot.server.has_value();
        if (snapshot.service_name == service && has_expected_side) {
            matches.push_back(&snapshot);
        }
    }
    const auto fail = [&role](const char* reason) {
        std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=" << role
                  << " idle_error=" << reason << '\n';
        return false;
    };
    if (matches.size() != 1U) {
        return fail("snapshot_missing_or_ambiguous");
    }

    const auto& snapshot = *matches.front();
    if (snapshot.service_name != service) {
        return fail("service_mismatch");
    }
    if (snapshot.snapshot_timestamp_unix_ms == 0U) {
        return fail("timestamp_missing");
    }
    if (client_side) {
        if (!snapshot.is_client() || !snapshot.client.has_value()) {
            return fail("client_metrics_missing_or_server_metrics_present");
        }
        const auto& metrics = *snapshot.client;
        std::cout << "RBT_RPC_INTEROP IDLE language=cpp role=" << role
                  << " service=" << snapshot.service_name << " calls=" << calls
                  << " endpoint=client snapshot_timestamp_unix_ms="
                  << snapshot.snapshot_timestamp_unix_ms
                  << " calls_started=" << metrics.calls_started
                  << " calls_completed=" << metrics.calls_completed
                  << " in_flight_current=" << metrics.in_flight_current
                  << " response_guards_current=" << metrics.response_guards_current
                  << std::endl;
        if (metrics.calls_started != calls || metrics.calls_completed != calls
            || metrics.in_flight_current != 0U || metrics.response_guards_current != 0U) {
            return fail("client_counters_not_idle");
        }
    } else {
        if (!snapshot.is_server() || !snapshot.server.has_value()) {
            return fail("server_metrics_missing_or_client_metrics_present");
        }
        const auto& metrics = *snapshot.server;
        std::cout << "RBT_RPC_INTEROP IDLE language=cpp role=" << role
                  << " service=" << snapshot.service_name << " calls=" << calls
                  << " endpoint=server snapshot_timestamp_unix_ms="
                  << snapshot.snapshot_timestamp_unix_ms
                  << " requests_received=" << metrics.requests_received
                  << " responses_sent=" << metrics.responses_sent
                  << " requests_current=" << metrics.requests_current << std::endl;
        if (metrics.requests_received != calls || metrics.responses_sent != calls
            || metrics.requests_current != 0U) {
            return fail("server_counters_not_idle");
        }
    }
    return true;
}

int run_server(const std::string& service, std::size_t calls) {
    auto server_result = FixedServer::open_or_create(descriptor(service));
    if (!server_result.has_value()) {
        std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=server service=" << service
                  << " error=" << rbt::to_string(server_result.error()) << '\n';
        return 1;
    }
    auto server = std::move(server_result).value();
    std::cout << "RBT_RPC_INTEROP READY language=cpp role=server service=" << service
              << " calls=" << calls << std::endl;

    for (std::size_t index = 0U; index < calls; ++index) {
        auto received = server.receive_timeout(kCallTimeout);
        if (!received.has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=server index=" << index
                      << " receive_error=" << rbt::to_string(received.error()) << '\n';
            return 1;
        }
        auto maybe_request = std::move(received).value();
        if (!maybe_request.has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=server index=" << index
                      << " error=timeout\n";
            return 1;
        }
        auto request = std::move(maybe_request).value();
        const auto input = request.payload();
        if (!validate_request(index, input)) {
            return 1;
        }
        const auto marker = input.value >> 32U;
        const auto operand = input.value & std::numeric_limits<std::uint32_t>::max();
        if (report_index(index)) {
            std::cout << "RBT_RPC_INTEROP REQUEST language=cpp role=server index="
                      << index << " nonce=" << input.sequence << " marker=" << marker
                      << " operand=" << operand << std::endl;
        }

        auto response_result = request.loan_response();
        if (!response_result.has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=server index=" << index
                      << " loan_error=" << rbt::to_string(response_result.error()) << '\n';
            return 1;
        }
        auto response = std::move(response_result).value();
        auto& output = response.payload();
        output.sequence = input.sequence;
        output.value = expected_value(input.sequence, input.value);
        auto sent = response.send();
        if (!sent.has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=server index=" << index
                      << " send_error=" << rbt::to_string(sent.error()) << '\n';
            return 1;
        }
    }

    if (!report_idle("server", service, calls, false)) {
        return 1;
    }
    std::cout << "RBT_RPC_INTEROP PASS language=cpp role=server service=" << service
              << " calls=" << calls << std::endl;
    return 0;
}

int run_client(const std::string& service, std::size_t calls) {
    auto client_result = FixedClient::open(descriptor(service));
    if (!client_result.has_value()) {
        std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=client service=" << service
                  << " error=" << rbt::to_string(client_result.error()) << '\n';
        return 1;
    }
    auto client = std::move(client_result).value();
    std::cout << "RBT_RPC_INTEROP READY language=cpp role=client service=" << service
              << " calls=" << calls << std::endl;

    for (std::size_t index = 0U; index < calls; ++index) {
        const auto input = request_for(index);
        auto response_result = client.call_copy(input, kCallTimeout);
        if (!response_result.has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=client index=" << index
                      << " call_error=" << rbt::to_string(response_result.error()) << '\n';
            return 1;
        }
        const auto& response = response_result.value().payload();
        const auto expected = expected_value(input.sequence, input.value);
        if (response.sequence != input.sequence || response.value != expected) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=client index=" << index
                      << " expected_nonce=" << input.sequence
                      << " actual_nonce=" << response.sequence
                      << " expected_value=" << expected
                      << " actual_value=" << response.value << '\n';
            return 1;
        }
        if (report_index(index)) {
            std::cout << "RBT_RPC_INTEROP RESPONSE language=cpp role=client index="
                      << index << " nonce=" << response.sequence
                      << " checksum=" << response.value << std::endl;
        }
        client_pace();
    }

    if (!report_idle("client", service, calls, true)) {
        return 1;
    }
    std::cout << "RBT_RPC_INTEROP PASS language=cpp role=client service=" << service
              << " calls=" << calls << std::endl;
    return 0;
}

int run_burst_server(const std::string& service, std::size_t calls) {
    auto server_result = FixedServer::open_or_create(burst_descriptor(service));
    if (!server_result.has_value()) {
        std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=burst-server service="
                  << service << " error=" << rbt::to_string(server_result.error()) << '\n';
        return 1;
    }
    auto server = std::move(server_result).value();
    std::cout << "RBT_RPC_INTEROP READY language=cpp role=burst-server service="
              << service << " calls=" << calls << std::endl;

    for (std::size_t base = 0U; base < calls;) {
        const auto window_size = std::min(kBurstWindow, calls - base);
        std::vector<FixedServerRequest> requests;
        requests.reserve(window_size);
        for (std::size_t offset = 0U; offset < window_size; ++offset) {
            const auto index = base + offset;
            auto received = server.receive_timeout(kCallTimeout);
            if (!received.has_value() || !received.value().has_value()) {
                std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=burst-server index="
                          << index << " error=receive\n";
                return 1;
            }
            auto request = std::move(received).value().value();
            const auto input = request.payload();
            if (!validate_request(index, input)) {
                return 1;
            }
            if (report_index(index)) {
                std::cout << "RBT_RPC_INTEROP REQUEST language=cpp role=burst-server index="
                          << index << " nonce=" << input.sequence
                          << " value=" << input.value << std::endl;
            }
            requests.push_back(std::move(request));
        }

        for (std::size_t offset = window_size; offset > 0U; --offset) {
            const auto index = base + offset - 1U;
            auto& request = requests[offset - 1U];
            const auto input = request.payload();
            auto response_result = request.loan_response();
            if (!response_result.has_value()) {
                std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=burst-server index="
                          << index << " loan_error="
                          << rbt::to_string(response_result.error()) << '\n';
                return 1;
            }
            auto response = std::move(response_result).value();
            auto& output = response.payload();
            output.sequence = input.sequence;
            output.value = expected_value(input.sequence, input.value);
            auto sent = response.send();
            if (!sent.has_value()) {
                std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=burst-server index="
                          << index << " send_error=" << rbt::to_string(sent.error()) << '\n';
                return 1;
            }
        }
        base += window_size;
    }

    if (!report_idle("burst-server", service, calls, false)) {
        return 1;
    }
    std::cout << "RBT_RPC_INTEROP PASS language=cpp role=burst-server service="
              << service << " calls=" << calls << std::endl;
    return 0;
}

int run_burst_client(const std::string& service, std::size_t calls) {
    auto client_result = FixedClient::open(burst_descriptor(service));
    if (!client_result.has_value()) {
        std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=burst-client service="
                  << service << " error=" << rbt::to_string(client_result.error()) << '\n';
        return 1;
    }
    auto client = std::move(client_result).value();
    std::cout << "RBT_RPC_INTEROP READY language=cpp role=burst-client service="
              << service << " calls=" << calls << std::endl;

    for (std::size_t base = 0U; base < calls;) {
        const auto window_size = std::min(kBurstWindow, calls - base);
        std::vector<std::pair<std::size_t, FixedPending>> pending_calls;
        pending_calls.reserve(window_size);
        for (std::size_t offset = 0U; offset < window_size; ++offset) {
            const auto index = base + offset;
            const auto input = request_for(index);
            auto loan_result = client.loan_request();
            if (!loan_result.has_value()) {
                std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=burst-client index="
                          << index << " loan_error=" << rbt::to_string(loan_result.error())
                          << '\n';
                return 1;
            }
            auto request = std::move(loan_result).value();
            request.payload() = input;
            auto pending_result = std::move(request).send();
            if (!pending_result.has_value()) {
                std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=burst-client index="
                          << index << " send_error="
                          << rbt::to_string(pending_result.error()) << '\n';
                return 1;
            }
            pending_calls.emplace_back(index, std::move(pending_result).value());
        }

        for (auto pending = pending_calls.rbegin(); pending != pending_calls.rend(); ++pending) {
            const auto index = pending->first;
            const auto input = request_for(index);
            auto response_result = pending->second.wait(kCallTimeout);
            if (!response_result.has_value()) {
                std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=burst-client index="
                          << index << " wait_error="
                          << rbt::to_string(response_result.error()) << '\n';
                return 1;
            }
            const auto& response = response_result.value().payload();
            const auto expected = expected_value(input.sequence, input.value);
            if (response.sequence != input.sequence || response.value != expected) {
                std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=burst-client index="
                          << index << " expected_nonce=" << input.sequence
                          << " actual_nonce=" << response.sequence
                          << " expected_value=" << expected
                          << " actual_value=" << response.value << '\n';
                return 1;
            }
            if (report_index(index)) {
                std::cout << "RBT_RPC_INTEROP RESPONSE language=cpp role=burst-client index="
                          << index << " nonce=" << response.sequence
                          << " checksum=" << response.value << std::endl;
            }
        }
        base += window_size;
    }

    if (!report_idle("burst-client", service, calls, true)) {
        return 1;
    }
    std::cout << "RBT_RPC_INTEROP PASS language=cpp role=burst-client service="
              << service << " calls=" << calls << std::endl;
    return 0;
}

int run_dynamic_server(const std::string& service, std::size_t calls) {
    auto server_result = DynamicServer::open_or_create(dynamic_descriptor(service));
    if (!server_result.has_value()) {
        std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-server service="
                  << service << " error=" << rbt::to_string(server_result.error()) << '\n';
        return 1;
    }
    auto server = std::move(server_result).value();
    std::cout << "RBT_RPC_INTEROP READY language=cpp role=dynamic-server service="
              << service << " calls=" << calls << std::endl;

    for (std::size_t index = 0U; index < calls; ++index) {
        auto received = server.receive_timeout(kCallTimeout);
        if (!received.has_value() || !received.value().has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-server index="
                      << index << " error=receive\n";
            return 1;
        }
        auto request = std::move(received).value().value();
        const auto header = request.user_header();
        const auto input = request.payload();
        if (header.sequence != dynamic_nonce(index)
            || header.timestamp_ns
                   != kDynamicTimestampBase + static_cast<std::uint64_t>(index)
            || header.width != kDynamicRequestBytes || header.height != 1U
            || header.stride != kDynamicRequestBytes
            || header.encoding.value != rbt::sensor::GRAY8.value
            || input.number_of_elements() != kDynamicRequestBytes) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-server index="
                      << index << " error=request_header\n";
            return 1;
        }
        for (std::size_t offset = 0U; offset < kDynamicRequestBytes; ++offset) {
            if (input[static_cast<std::uint64_t>(offset)]
                != dynamic_request_byte(index, offset)) {
                std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-server index="
                          << index << " offset=" << offset << " error=request_byte\n";
                return 1;
            }
        }
        const auto request_checksum = dynamic_checksum(input);
        auto response_result = request.loan_response();
        if (!response_result.has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-server index="
                      << index << " error=" << rbt::to_string(response_result.error())
                      << '\n';
            return 1;
        }
        auto response = std::move(response_result).value();
        auto& output_header = response.user_header();
        output_header.sequence = header.sequence;
        output_header.timestamp_ns = header.timestamp_ns ^ kTransformXor;
        output_header.width = static_cast<std::uint32_t>(kDynamicResponseBytes);
        output_header.height = 1U;
        output_header.stride = static_cast<std::uint32_t>(kDynamicResponseBytes);
        output_header.encoding = rbt::sensor::GRAY8;
        auto output = response.payload();
        if (output.number_of_elements() != kDynamicResponseBytes) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-server index="
                      << index << " error=response_size\n";
            return 1;
        }
        for (std::size_t offset = 0U; offset < kDynamicResponseBytes; ++offset) {
            output[static_cast<std::uint64_t>(offset)] =
                dynamic_response_byte(index, offset);
        }
        const auto response_checksum = dynamic_checksum(output);
        auto sent = response.send();
        if (!sent.has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-server index="
                      << index << " error=" << rbt::to_string(sent.error()) << '\n';
            return 1;
        }
        if (report_index(index)) {
            std::cout << "RBT_RPC_INTEROP DYNAMIC language=cpp role=server index="
                      << index << " nonce=" << header.sequence
                      << " request_bytes=" << kDynamicRequestBytes
                      << " request_checksum=" << request_checksum
                      << " response_bytes=" << kDynamicResponseBytes
                      << " response_checksum=" << response_checksum << std::endl;
        }
    }
    if (!report_idle("dynamic-server", service, calls, false)) {
        return 1;
    }
    std::cout << "RBT_RPC_INTEROP PASS language=cpp role=dynamic-server service="
              << service << " calls=" << calls << std::endl;
    return 0;
}

int run_dynamic_client(const std::string& service, std::size_t calls) {
    auto client_result = DynamicClient::open(dynamic_descriptor(service));
    if (!client_result.has_value()) {
        std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-client service="
                  << service << " error=" << rbt::to_string(client_result.error()) << '\n';
        return 1;
    }
    auto client = std::move(client_result).value();
    std::cout << "RBT_RPC_INTEROP READY language=cpp role=dynamic-client service="
              << service << " calls=" << calls << std::endl;
    for (std::size_t index = 0U; index < calls; ++index) {
        auto loan_result = client.loan_request_slice(kDynamicRequestBytes);
        if (!loan_result.has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-client index="
                      << index << " error=" << rbt::to_string(loan_result.error()) << '\n';
            return 1;
        }
        auto request = std::move(loan_result).value();
        auto& header = request.user_header();
        header.sequence = dynamic_nonce(index);
        header.timestamp_ns = kDynamicTimestampBase + static_cast<std::uint64_t>(index);
        header.width = static_cast<std::uint32_t>(kDynamicRequestBytes);
        header.height = 1U;
        header.stride = static_cast<std::uint32_t>(kDynamicRequestBytes);
        header.encoding = rbt::sensor::GRAY8;
        auto input = request.payload();
        for (std::size_t offset = 0U; offset < kDynamicRequestBytes; ++offset) {
            input[static_cast<std::uint64_t>(offset)] =
                dynamic_request_byte(index, offset);
        }
        const auto request_checksum = dynamic_checksum(input);
        auto pending = std::move(request).send();
        if (!pending.has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-client index="
                      << index << " error=" << rbt::to_string(pending.error()) << '\n';
            return 1;
        }
        auto response_result = std::move(pending).value().wait(kCallTimeout);
        if (!response_result.has_value()) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-client index="
                      << index << " error=" << rbt::to_string(response_result.error())
                      << '\n';
            return 1;
        }
        const auto& response = response_result.value();
        const auto& output_header = response.user_header();
        const auto output = response.payload();
        if (output_header.sequence != dynamic_nonce(index)
            || output_header.timestamp_ns
                   != ((kDynamicTimestampBase + static_cast<std::uint64_t>(index))
                       ^ kTransformXor)
            || output_header.width != kDynamicResponseBytes || output_header.height != 1U
            || output_header.stride != kDynamicResponseBytes
            || output_header.encoding.value != rbt::sensor::GRAY8.value
            || output.number_of_elements() != kDynamicResponseBytes) {
            std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-client index="
                      << index << " error=response_header\n";
            return 1;
        }
        for (std::size_t offset = 0U; offset < kDynamicResponseBytes; ++offset) {
            if (output[static_cast<std::uint64_t>(offset)]
                != dynamic_response_byte(index, offset)) {
                std::cerr << "RBT_RPC_INTEROP FAIL language=cpp role=dynamic-client index="
                          << index << " offset=" << offset << " error=response_byte\n";
                return 1;
            }
        }
        if (report_index(index)) {
            std::cout << "RBT_RPC_INTEROP DYNAMIC language=cpp role=client index="
                      << index << " nonce=" << output_header.sequence
                      << " request_bytes=" << kDynamicRequestBytes
                      << " request_checksum=" << request_checksum
                      << " response_bytes=" << kDynamicResponseBytes
                      << " response_checksum=" << dynamic_checksum(output) << std::endl;
        }
        client_pace();
    }
    if (!report_idle("dynamic-client", service, calls, true)) {
        return 1;
    }
    std::cout << "RBT_RPC_INTEROP PASS language=cpp role=dynamic-client service="
              << service << " calls=" << calls << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto role_valid = argc >= 2
                            && (std::string(argv[1]) == "client"
                                || std::string(argv[1]) == "server"
                                || std::string(argv[1]) == "burst-client"
                                || std::string(argv[1]) == "burst-server"
                                || std::string(argv[1]) == "dynamic-client"
                                || std::string(argv[1]) == "dynamic-server");
    if (argc < 3 || argc > 4 || !role_valid || std::string(argv[2]).empty()) {
        print_usage(argv[0]);
        return 2;
    }
    std::size_t calls = kDefaultCalls;
    if (argc == 4 && !parse_calls(argv[3], calls)) {
        print_usage(argv[0]);
        return 2;
    }
    const std::string role(argv[1]);
    const std::string service(argv[2]);
    if (role == "server") {
        return run_server(service, calls);
    }
    if (role == "burst-server") {
        return run_burst_server(service, calls);
    }
    if (role == "burst-client") {
        return run_burst_client(service, calls);
    }
    if (role == "dynamic-server") {
        return run_dynamic_server(service, calls);
    }
    if (role == "dynamic-client") {
        return run_dynamic_client(service, calls);
    }
    return run_client(service, calls);
}

