#include <rbt/rpc.hpp>

#include <rbt/demo.hpp>
#include <rbt/sensor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
// Reuse the canonical fixed Frame/Detection ABI pair.  The request's
// sequence is the nonce and value is the operand; the response preserves the
// nonce in sequence and returns the result in value.
using FixedRequest = rbt::demo::Frame;
using FixedResponse = rbt::demo::Detection;
using FixedClient = rbt::RpcClient<FixedRequest, FixedResponse>;
using FixedServer = rbt::RpcServer<FixedRequest, FixedResponse>;
using HeaderClient = rbt::RpcClient<FixedRequest, FixedResponse, FixedRequest, FixedResponse>;
using HeaderServer = rbt::RpcServer<FixedRequest, FixedResponse, FixedRequest, FixedResponse>;
using FixedPendingCall = rbt::PendingRpcCall<FixedRequest, FixedResponse>;
using FixedRequestLoan = rbt::RpcRequestLoan<FixedRequest, FixedResponse>;
using FixedRequestGuard = rbt::RpcRequest<FixedRequest, FixedResponse>;
using FixedResponseLoan = rbt::RpcResponseLoan<FixedRequest, FixedResponse>;
constexpr std::size_t kDynamicSliceLimit = 100000U;

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

template <typename Server>
void serve_fixed(Server server,
                 std::atomic<bool>& stop,
                 std::size_t drop_requests = 0U) {
    std::size_t received = 0U;
    while (!stop.load(std::memory_order_acquire)) {
        auto result = server.receive_timeout(100ms);
        if (!result.has_value()) {
            if (result.error() == rbt::RpcError::ActivationExpired) {
                return;
            }
            continue;
        }
        if (!result.value().has_value()) {
            continue;
        }
        auto request = std::move(result).value().value();
        ++received;
        if (received <= drop_requests) {
            continue;
        }
        const auto payload = request.payload();
        auto response = request.loan_response();
        if (!response.has_value()) {
            continue;
        }
        auto response_loan = std::move(response).value();
        auto& output = response_loan.payload();
        output.value = payload.value + 1U;
        output.sequence = payload.sequence;
        (void)response_loan.send();
    }
}

void serve_replacement(FixedServer server, std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_acquire)) {
        auto result = server.receive_timeout(100ms);
        if (!result.has_value()) {
            if (result.error() == rbt::RpcError::ActivationExpired) {
                return;
            }
            continue;
        }
        if (!result.value().has_value()) {
            continue;
        }
        auto request = std::move(result).value().value();
        const auto payload = request.payload();
        auto response = request.loan_response();
        if (!response.has_value()) {
            continue;
        }
        auto response_loan = std::move(response).value();
        auto& output = response_loan.payload();
        output.value = payload.value + 1000U;
        output.sequence = payload.sequence;
        (void)response_loan.send();
    }
}

rbt::RpcDescriptor fixed_descriptor(const std::string& service,
                                    rbt::RpcResourceConfig resources = {}) {
    auto descriptor = rbt::RpcDescriptor::for_fixed<FixedRequest, FixedResponse>(service);
    descriptor.resources = resources;
    return descriptor;
}

std::string service_name(const char* suffix) {
    return std::string("/rbt/cpp-rpc-") + std::to_string(static_cast<unsigned long>(::getpid()))
        + suffix;
}

rbt::RpcClientMetrics client_metrics_for(const std::string& service) {
    for (const auto& pressure : rbt::local_rpc_pressure_snapshots()) {
        if (pressure.service_name == service && pressure.client.has_value()) {
            return pressure.client.value();
        }
    }
    std::abort();
}

rbt::RpcServerMetrics server_metrics_for(const std::string& service) {
    for (const auto& pressure : rbt::local_rpc_pressure_snapshots()) {
        if (pressure.service_name == service && pressure.server.has_value()) {
            return pressure.server.value();
        }
    }
    std::abort();
}

std::uint64_t terminal_calls(const rbt::RpcClientMetrics& metrics) {
    return metrics.calls_completed + metrics.calls_failed + metrics.calls_timed_out
        + metrics.calls_cancelled + metrics.calls_activation_expired;
}

FixedPendingCall send_pending(FixedClient& client, std::uint64_t sequence) {
    auto loan_result = client.loan_request();
    require(loan_result.has_value());
    auto loan = std::move(loan_result).value();
    loan.payload() = FixedRequest { sequence, sequence + 100U };
    auto pending_result = std::move(loan).send();
    require(pending_result.has_value());
    return std::move(pending_result).value();
}

FixedRequestGuard receive_request(FixedServer& server) {
    auto received = server.receive_timeout(2s);
    require(received.has_value());
    require(received.value().has_value());
    auto optional_request = std::move(received).value();
    return std::move(optional_request).value();
}

void complete_call(FixedClient& client, FixedServer& server, std::uint64_t sequence) {
    auto pending = send_pending(client, sequence);
    auto request = receive_request(server);
    auto response_result = request.loan_response();
    require(response_result.has_value());
    auto response = std::move(response_result).value();
    response.payload() = FixedResponse { sequence, sequence + 101U };
    require(response.send().has_value());
    auto received = pending.wait(2s);
    require(received.has_value());
    require(received.value().payload().sequence == sequence);
    require(received.value().payload().value == sequence + 101U);
}

rbt::RpcResourceConfig move_assignment_test_resources() {
    return rbt::RpcResourceConfig::defaults()
        .with_max_in_flight(2U)
        .with_max_request_loans(2U)
        .with_max_response_loans(2U)
        .with_response_buffer_size(2U)
        .with_max_borrowed_responses(2U);
}

void roundtrip_and_timeout_reuse() {
    auto resources = rbt::RpcResourceConfig::defaults()
                         .with_max_clients(1U)
                         .with_max_in_flight(2U)
                         .with_max_request_loans(1U)
                         .with_max_response_loans(1U);
    const auto descriptor = fixed_descriptor(service_name("-roundtrip"), resources);
    auto server_result = FixedServer::open_or_create(descriptor);
    require(server_result.has_value());
    auto client_result = FixedClient::open(descriptor);
    require(client_result.has_value());
    auto server = std::move(server_result).value();
    auto client = std::move(client_result).value();
    std::atomic<bool> stop { false };
    const auto wake = server.wake_handle();
    std::thread worker(serve_fixed<FixedServer>, std::move(server), std::ref(stop), 1U);

    const FixedRequest first { 11, 2 };
    auto timed_out = client.call_copy(first, 25ms);
    require(!timed_out.has_value());
    require(timed_out.error() == rbt::RpcError::DeadlineExceeded);

    const FixedRequest second { 12, 15 };
    auto response = client.call_copy(second, 2s);
    require(response.has_value());
    require(response.value().payload().value == 16U);
    require(response.value().payload().sequence == 12U);

    stop.store(true, std::memory_order_release);
    (void)wake.wake();
    worker.join();
}

void cancellation_and_activation_wake() {
    const auto descriptor = fixed_descriptor(service_name("-cancel"));
    auto server_result = FixedServer::open_or_create(descriptor);
    require(server_result.has_value());
    auto client_result = FixedClient::open(descriptor);
    require(client_result.has_value());
    auto server = std::move(server_result).value();
    auto client = std::move(client_result).value();
    std::atomic<bool> stop { false };
    const auto server_wake = server.wake_handle();
    std::thread worker(serve_fixed<FixedServer>, std::move(server), std::ref(stop), 100U);

    auto loan = client.loan_request();
    require(loan.has_value());
    auto request_loan = std::move(loan).value();
    request_loan.payload() = FixedRequest { 21, 1 };
    auto pending_result = std::move(request_loan).send();
    require(pending_result.has_value());
    auto pending = std::move(pending_result).value();
    auto cancel = pending.cancel_handle();
    (void)cancel.cancel();
    auto cancelled = pending.wait(2s);
    require(!cancelled.has_value());
    require(cancelled.error() == rbt::RpcError::Cancelled);

    auto activation_loan = client.loan_request();
    require(activation_loan.has_value());
    auto activation_request = std::move(activation_loan).value();
    activation_request.payload() = FixedRequest { 22, 4 };
    auto activation_pending_result = std::move(activation_request).send();
    require(activation_pending_result.has_value());
    auto activation_pending = std::move(activation_pending_result).value();
    auto client_wake = client.wake_handle();
    (void)client_wake.wake();
    auto expired = activation_pending.wait(2s);
    require(!expired.has_value());
    require(expired.error() == rbt::RpcError::ActivationExpired);

    stop.store(true, std::memory_order_release);
    (void)server_wake.wake();
    worker.join();
}

void ownership_and_capacity() {
    auto resources = rbt::RpcResourceConfig::defaults().with_max_request_loans(1U);
    const auto descriptor = fixed_descriptor(service_name("-capacity"), resources);
    auto server_result = FixedServer::open_or_create(descriptor);
    require(server_result.has_value());
    auto client_result = FixedClient::open(descriptor);
    require(client_result.has_value());
    auto server = std::move(server_result).value();
    auto client = std::move(client_result).value();
    static_assert(!std::is_copy_constructible<FixedClient>::value);
    static_assert(!std::is_copy_assignable<FixedClient>::value);
    static_assert(std::is_move_constructible<FixedClient>::value);
    static_assert(std::is_move_assignable<FixedClient>::value);
    static_assert(!std::is_copy_constructible<rbt::RpcRequestLoan<FixedRequest, FixedResponse>>::value);
    static_assert(!std::is_copy_constructible<rbt::PendingRpcCall<FixedRequest, FixedResponse>>::value);
    static_assert(!std::is_copy_constructible<rbt::RpcResponseGuard<FixedRequest, FixedResponse>>::value);
    static_assert(!std::is_copy_constructible<rbt::RpcRequest<FixedRequest, FixedResponse>>::value);
    static_assert(!std::is_copy_constructible<rbt::RpcResponseLoan<FixedRequest, FixedResponse>>::value);

    FixedClient moved_client;
    {
        FixedClient moved_from = std::move(client);
        moved_client = std::move(moved_from);
    }

    // Destroying the moved-from handle must not expire the live endpoint.
    auto first = moved_client.loan_request();
    require(first.has_value());
    auto second = moved_client.loan_request();
    require(!second.has_value());
    require(second.error() == rbt::RpcError::LoanPoolExhausted);
    first = rbt::RpcResult<rbt::RpcRequestLoan<FixedRequest, FixedResponse>>(
        rbt::RpcError::TransportFailure);
    auto third = moved_client.loan_request();
    require(third.has_value());
}

void pending_move_assignment_releases_both_calls() {
    const auto service = service_name("-move-pending");
    const auto descriptor = fixed_descriptor(service, move_assignment_test_resources());
    auto server_result = FixedServer::open_or_create(descriptor);
    require(server_result.has_value());
    auto server = std::move(server_result).value();
    auto client_result = FixedClient::open(descriptor);
    require(client_result.has_value());
    auto client = std::move(client_result).value();

    {
        auto destination = send_pending(client, 1U);
        auto source = send_pending(client, 2U);
        const auto before = client_metrics_for(service);
        require(before.calls_started == 2U);
        require(before.in_flight_current == 2U);
        destination = std::move(source);
    }

    const auto after = client_metrics_for(service);
    require(after.calls_started == 2U);
    require(terminal_calls(after) == 2U);
    require(after.in_flight_current == 0U);
}

void pending_move_assignment_preserves_in_flight_high_water() {
    const auto service = service_name("-move-pending-high-water");
    const auto descriptor = fixed_descriptor(service, move_assignment_test_resources());
    auto server_result = FixedServer::open_or_create(descriptor);
    require(server_result.has_value());
    auto server = std::move(server_result).value();
    auto client_result = FixedClient::open(descriptor);
    require(client_result.has_value());
    auto client = std::move(client_result).value();

    {
        auto destination = send_pending(client, 31U);
        auto source = send_pending(client, 32U);
        auto first_request = receive_request(server);
        auto second_request = receive_request(server);
        const auto before = client_metrics_for(service);
        require(before.calls_started == 2U);
        require(before.in_flight_current == 2U);
        destination = std::move(source);
    }

    const auto after_replacement = client_metrics_for(service);
    require(after_replacement.in_flight_current == 0U);
    require(terminal_calls(after_replacement) == 2U);
    {
        auto fresh_first = send_pending(client, 33U);
        auto fresh_second = send_pending(client, 34U);
        const auto after_fresh_calls = client_metrics_for(service);
        require(after_fresh_calls.calls_started == 4U);
        require(after_fresh_calls.in_flight_current == 2U);
        require(after_fresh_calls.in_flight_high_water == 2U);
    }

    const auto after = client_metrics_for(service);
    require(after.in_flight_current == 0U);
    require(terminal_calls(after) == 4U);
}

void cancelled_pending_move_assignment_records_cancelled_destination() {
    const auto service = service_name("-move-pending-cancelled");
    const auto descriptor = fixed_descriptor(service, move_assignment_test_resources());
    auto server_result = FixedServer::open_or_create(descriptor);
    require(server_result.has_value());
    auto server = std::move(server_result).value();
    auto client_result = FixedClient::open(descriptor);
    require(client_result.has_value());
    auto client = std::move(client_result).value();

    {
        auto destination = send_pending(client, 41U);
        auto source = send_pending(client, 42U);
        auto cancel = destination.cancel_handle();
        require(cancel.cancel().has_value());
        require(cancel.is_cancelled());
        destination = std::move(source);
    }

    const auto after = client_metrics_for(service);
    require(after.calls_started == 2U);
    require(after.calls_cancelled == 1U);
    require(terminal_calls(after) == 2U);
    require(after.in_flight_current == 0U);
}

void request_move_assignment_releases_both_requests() {
    const auto service = service_name("-move-request");
    const auto descriptor = fixed_descriptor(service, move_assignment_test_resources());
    auto server_result = FixedServer::open_or_create(descriptor);
    require(server_result.has_value());
    auto server = std::move(server_result).value();
    auto client_result = FixedClient::open(descriptor);
    require(client_result.has_value());
    auto client = std::move(client_result).value();

    {
        auto first_pending = send_pending(client, 11U);
        auto destination = receive_request(server);
        auto second_pending = send_pending(client, 12U);
        auto source = receive_request(server);
        const auto before = server_metrics_for(service);
        require(before.requests_received == 2U);
        require(before.requests_current == 2U);
        destination = std::move(source);
        (void)first_pending;
        (void)second_pending;
    }

    const auto after = server_metrics_for(service);
    require(after.requests_received == 2U);
    require(after.requests_current == 0U);
    require(after.responses_abandoned == 2U);
}

void response_loan_move_assignment_releases_both_requests() {
    const auto service = service_name("-move-response-loan");
    const auto descriptor = fixed_descriptor(service, move_assignment_test_resources());
    auto server_result = FixedServer::open_or_create(descriptor);
    require(server_result.has_value());
    auto server = std::move(server_result).value();
    auto client_result = FixedClient::open(descriptor);
    require(client_result.has_value());
    auto client = std::move(client_result).value();

    {
        auto first_pending = send_pending(client, 21U);
        auto first_request = receive_request(server);
        auto destination_result = first_request.loan_response();
        require(destination_result.has_value());
        auto destination = std::move(destination_result).value();

        auto second_pending = send_pending(client, 22U);
        auto second_request = receive_request(server);
        auto source_result = second_request.loan_response();
        require(source_result.has_value());
        auto source = std::move(source_result).value();
        const auto before = server_metrics_for(service);
        require(before.requests_received == 2U);
        require(before.requests_current == 2U);
        destination = std::move(source);
        (void)first_pending;
        (void)second_pending;
    }

    const auto after = server_metrics_for(service);
    require(after.requests_received == 2U);
    require(after.requests_current == 0U);
    require(after.responses_abandoned == 2U);
}

void request_loan_move_assignment_releases_capacity_and_handles_self_move() {
    const auto resources = rbt::RpcResourceConfig::defaults().with_max_request_loans(1U);
    const auto first_descriptor = fixed_descriptor(service_name("-move-request-loan-a"), resources);
    const auto second_descriptor = fixed_descriptor(service_name("-move-request-loan-b"), resources);
    auto first_server_result = FixedServer::open_or_create(first_descriptor);
    require(first_server_result.has_value());
    auto first_server = std::move(first_server_result).value();
    auto second_server_result = FixedServer::open_or_create(second_descriptor);
    require(second_server_result.has_value());
    auto second_server = std::move(second_server_result).value();
    auto first_client_result = FixedClient::open(first_descriptor);
    require(first_client_result.has_value());
    auto first_client = std::move(first_client_result).value();
    auto second_client_result = FixedClient::open(second_descriptor);
    require(second_client_result.has_value());
    auto second_client = std::move(second_client_result).value();

    {
        auto destination_result = first_client.loan_request();
        require(destination_result.has_value());
        auto destination = std::move(destination_result).value();
        destination.payload().sequence = 1U;
        auto source_result = second_client.loan_request();
        require(source_result.has_value());
        auto source = std::move(source_result).value();
        source.payload().sequence = 2U;
        destination = std::move(source);
        auto* self = &destination;
        destination = std::move(*self);
        require(destination.payload().sequence == 2U);

        auto recovered_first = first_client.loan_request();
        require(recovered_first.has_value());
    }

    auto recovered_second = second_client.loan_request();
    require(recovered_second.has_value());
}

void client_move_assignment_expires_old_activation_and_keeps_source_working() {
    const auto old_descriptor = fixed_descriptor(service_name("-client-replace-old"));
    const auto new_descriptor = fixed_descriptor(service_name("-client-replace-new"));
    auto old_server_result = FixedServer::open_or_create(old_descriptor);
    require(old_server_result.has_value());
    auto old_server = std::move(old_server_result).value();
    auto new_server_result = FixedServer::open_or_create(new_descriptor);
    require(new_server_result.has_value());
    auto new_server = std::move(new_server_result).value();
    auto old_client_result = FixedClient::open(old_descriptor);
    require(old_client_result.has_value());
    auto destination = std::move(old_client_result).value();
    auto new_client_result = FixedClient::open(new_descriptor);
    require(new_client_result.has_value());
    auto source = std::move(new_client_result).value();

    auto retained_pending = send_pending(destination, 51U);
    destination = std::move(source);
    auto expired = retained_pending.wait(2s);
    require(!expired.has_value());
    require(expired.error() == rbt::RpcError::ActivationExpired);
    complete_call(destination, new_server, 52U);
}

void server_move_assignment_expires_old_activation_and_keeps_source_working() {
    const auto old_descriptor = fixed_descriptor(service_name("-server-replace-old"));
    const auto new_descriptor = fixed_descriptor(service_name("-server-replace-new"));
    auto old_server_result = FixedServer::open_or_create(old_descriptor);
    require(old_server_result.has_value());
    auto destination = std::move(old_server_result).value();
    auto new_server_result = FixedServer::open_or_create(new_descriptor);
    require(new_server_result.has_value());
    auto source = std::move(new_server_result).value();
    auto old_client_result = FixedClient::open(old_descriptor);
    require(old_client_result.has_value());
    auto old_client = std::move(old_client_result).value();
    auto new_client_result = FixedClient::open(new_descriptor);
    require(new_client_result.has_value());
    auto new_client = std::move(new_client_result).value();

    auto retained_pending = send_pending(old_client, 61U);
    auto retained_request = receive_request(destination);
    destination = std::move(source);
    auto expired_response = retained_request.loan_response();
    require(!expired_response.has_value());
    require(expired_response.error() == rbt::RpcError::ActivationExpired);
    complete_call(new_client, destination, 62U);
    (void)retained_pending;
}

void in_flight_limit_is_typed_client_capacity() {
    auto resources = rbt::RpcResourceConfig::defaults()
                         .with_max_clients(1U)
                         .with_max_in_flight(1U)
                         .with_max_request_loans(2U);
    const auto service = service_name("-in-flight");
    const auto descriptor = fixed_descriptor(service, resources);
    auto server_result = FixedServer::open_or_create(descriptor);
    require(server_result.has_value());
    auto client_result = FixedClient::open(descriptor);
    require(client_result.has_value());
    auto server = std::move(server_result).value();
    auto client = std::move(client_result).value();

    auto first_loan = client.loan_request();
    require(first_loan.has_value());
    auto first_request = std::move(first_loan).value();
    first_request.payload() = FixedRequest { 1, 10 };
    auto first = std::move(first_request).send();
    require(first.has_value());
    // The server holds the first request so only the client in-flight bound applies.
    auto held = server.receive_timeout(2s);
    require(held.has_value() && held.value().has_value());

    auto rejected_loan = client.loan_request();
    require(rejected_loan.has_value());
    auto rejected = std::move(rejected_loan).value().send();
    require(!rejected.has_value());
    require(rejected.error() == rbt::RpcError::ClientCapacityReached);
    const auto saturated = client_metrics_for(service);
    require(saturated.calls_attempted == 2U);
    require(saturated.calls_started == 1U);
    require(saturated.capacity_rejections == 1U);
    require(saturated.server_capacity_rejections == 0U);
    require(saturated.in_flight_current == 1U);

    first = rbt::RpcResult<rbt::PendingRpcCall<FixedRequest, FixedResponse>>(
        rbt::RpcError::TransportFailure);
    auto recovered_loan = client.loan_request();
    require(recovered_loan.has_value());
    auto recovered = std::move(recovered_loan).value().send();
    require(recovered.has_value());
}

void server_disappearance_replacement_recovery() {
    auto resources = rbt::RpcResourceConfig::defaults()
                         .with_max_clients(1U)
                         .with_max_in_flight(2U);
    const auto descriptor = fixed_descriptor(service_name("-replacement"), resources);
    auto old_server_result = FixedServer::open_or_create_for_capability(
        descriptor, "cpp-rpc-old-capability");
    require(old_server_result.has_value());
    auto client_result = FixedClient::open(descriptor);
    require(client_result.has_value());
    auto old_server = std::move(old_server_result).value();
    auto client = std::move(client_result).value();

    auto old_request_result = client.loan_request();
    require(old_request_result.has_value());
    auto old_request_loan = std::move(old_request_result).value();
    old_request_loan.payload() = FixedRequest { 1U, 7U };
    auto old_pending_result = std::move(old_request_loan).send();
    require(old_pending_result.has_value());
    auto old_pending = std::move(old_pending_result).value();

    auto received_result = old_server.receive_timeout(2s);
    require(received_result.has_value());
    require(received_result.value().has_value());
    auto old_request = std::move(received_result).value().value();
    auto stale_response_result = old_request.loan_response();
    require(stale_response_result.has_value());
    auto stale_response = std::move(stale_response_result).value();
    stale_response.payload() = FixedResponse { 1U, 0xdeadU };

    // Dropping the server activation invalidates its outstanding response loan.
    // The client endpoint remains alive, so this is a real disappearance on the
    // native service rather than a client-side cancellation.
    require(old_server.expire().has_value());
    old_server = FixedServer {};

    const auto disappearance_started = std::chrono::steady_clock::now();
    auto old_result = old_pending.wait(500ms);
    const auto disappearance_elapsed = std::chrono::steady_clock::now() - disappearance_started;
    require(!old_result.has_value());
    require(disappearance_elapsed <= 1s);
    require(old_result.error() == rbt::RpcError::ServerUnavailable
            || old_result.error() == rbt::RpcError::DeadlineExceeded
            || old_result.error() == rbt::RpcError::TransportFailure);

    // The native sidecar services are part of the physical endpoint.  Once
    // the old server disappears, its same-name sidecars are marked for
    // destruction, so the old client cannot be transparently rebound.  Keep
    // that client alive long enough to prove a new call terminates with a
    // bounded, typed failure, then rebind in this same process to a fresh
    // physical service/capability identity.
    auto stale_client_result = client.call_copy(FixedRequest { 2U, 41U }, 500ms);
    require(!stale_client_result.has_value());
    require(stale_client_result.error() == rbt::RpcError::ServerUnavailable
            || stale_client_result.error() == rbt::RpcError::DeadlineExceeded
            || stale_client_result.error() == rbt::RpcError::TransportFailure);
    client = FixedClient {};
    const auto fresh_descriptor =
        fixed_descriptor(service_name("-replacement-fresh-physical"), resources);
    auto fresh_server_result = FixedServer::open_or_create_for_capability(
        fresh_descriptor, "cpp-rpc-fresh-capability");
    require(fresh_server_result.has_value());
    auto fresh_server = std::move(fresh_server_result).value();
    const auto fresh_server_wake = fresh_server.wake_handle();
    std::atomic<bool> fresh_server_stop { false };
    std::thread fresh_server_worker(
        serve_replacement, std::move(fresh_server), std::ref(fresh_server_stop));
    auto fresh_client_result = FixedClient::open(fresh_descriptor);
    require(fresh_client_result.has_value());
    auto fresh_client = std::move(fresh_client_result).value();
    auto fresh_response = fresh_client.call_copy(FixedRequest { 3U, 43U }, 1s);
    require(fresh_response.has_value());
    require(fresh_response.value().payload().sequence == 3U);
    require(fresh_response.value().payload().value == 1043U);
    auto stale_send = stale_response.send();
    require(!stale_send.has_value());
    require(stale_send.error() == rbt::RpcError::ActivationExpired);
    fresh_server_stop.store(true, std::memory_order_release);
    (void)fresh_server_wake.wake();
    fresh_server_worker.join();
}

void user_header_compile_contract() {
    const auto descriptor = fixed_descriptor(service_name("-headers"));
    auto server = HeaderServer::open_or_create(descriptor);
    require(server.has_value());
    auto client = HeaderClient::open(descriptor);
    require(client.has_value());
    auto loan = std::move(client).value().loan_request();
    require(loan.has_value());
    auto request = std::move(loan).value();
    request.user_header().sequence = 3U;
    request.payload().sequence = 3U;
}

using Slice = iox2::bb::Slice<std::uint8_t>;
using SliceClient = rbt::RpcClient<Slice, Slice, rbt::sensor::ImageHeader, rbt::sensor::ImageHeader>;
using SliceServer = rbt::RpcServer<Slice, Slice, rbt::sensor::ImageHeader, rbt::sensor::ImageHeader>;

void serve_slice(SliceServer server, std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_acquire)) {
        auto result = server.receive_timeout(100ms);
        if (!result.has_value()) {
            if (result.error() == rbt::RpcError::ActivationExpired) {
                return;
            }
            continue;
        }
        if (!result.value().has_value()) {
            continue;
        }
        auto request = std::move(result).value().value();
        const auto input = request.payload();
        auto oversized_response = request.loan_response_slice(kDynamicSliceLimit + 1U);
        require(!oversized_response.has_value());
        require(oversized_response.error() == rbt::RpcError::InvalidPayloadLength);
        auto response = request.loan_response_slice(
            static_cast<std::size_t>(input.number_of_elements()));
        if (!response.has_value()) {
            continue;
        }
        auto response_loan = std::move(response).value();
        auto output = response_loan.payload();
        const auto count = input.number_of_elements();
        for (std::uint64_t index = 0U; index < count && index < output.number_of_elements(); ++index) {
            output[index] = static_cast<std::uint8_t>(input[index] + 1U);
        }
        (void)response_loan.send();
    }
}

void dynamic_slice_roundtrip() {
    auto resources = rbt::RpcResourceConfig::defaults()
                         .with_max_request_slice_len(kDynamicSliceLimit)
                         .with_max_response_slice_len(kDynamicSliceLimit);
    const auto service = service_name("-slice");
    auto descriptor = rbt::RpcDescriptor::for_slices<rbt::sensor::ImageHeader,
                                                     std::uint8_t,
                                                     rbt::sensor::ImageHeader,
                                                     std::uint8_t>(service);
    descriptor.resources = resources;
    auto server_result = SliceServer::open_or_create(descriptor);
    require(server_result.has_value());
    auto client_result = SliceClient::open(descriptor);
    require(client_result.has_value());
    auto server = std::move(server_result).value();
    auto client = std::move(client_result).value();
    std::atomic<bool> stop { false };
    const auto wake = server.wake_handle();
    std::thread worker(serve_slice, std::move(server), std::ref(stop));
    require(client_metrics_for(service).response_guards_current == 0U);

    auto oversized_request = client.loan_request_slice(kDynamicSliceLimit + 1U);
    require(!oversized_request.has_value());
    require(oversized_request.error() == rbt::RpcError::InvalidPayloadLength);

    const std::size_t lengths[] = { 0U, 1U, 100U, kDynamicSliceLimit };
    for (const auto length : lengths) {
        auto loan = client.loan_request_slice(length);
        require(loan.has_value());
        auto request = std::move(loan).value();
        (void)request.user_header();
        auto input = request.payload();
        for (std::uint64_t index = 0U; index < input.number_of_elements(); ++index) {
            input[index] = static_cast<std::uint8_t>(index);
        }
        auto pending_result = std::move(request).send();
        require(pending_result.has_value());
        {
            auto response_result = std::move(pending_result).value().wait(2s);
            require(response_result.has_value());
            auto response = std::move(response_result).value();
            const auto output = response.payload();
            require(output.number_of_elements() == length);
            if (length > 0U) {
                const auto expected_last = static_cast<std::uint8_t>(
                    static_cast<std::uint8_t>((length - 1U) % 256U) + 1U);
                require(output[0] == 1U);
                require(output[length - 1U] == expected_last);
            }
            const auto metrics = client_metrics_for(service);
            require(metrics.response_guards_current == 1U);
            require(metrics.response_guards_high_water == 1U);
            auto moved_response = std::move(response);
            (void)moved_response;
            require(client_metrics_for(service).response_guards_current == 1U);
        }
        require(client_metrics_for(service).response_guards_current == 0U);
    }

    {
        auto first_loan_result = client.loan_request_slice(1U);
        require(first_loan_result.has_value());
        auto first_loan = std::move(first_loan_result).value();
        first_loan.payload()[0] = 1U;
        auto first_pending_result = std::move(first_loan).send();
        require(first_pending_result.has_value());
        auto first_response_result =
            std::move(first_pending_result).value().wait(2s);
        require(first_response_result.has_value());
        auto first_response = std::move(first_response_result).value();
        require(client_metrics_for(service).response_guards_current == 1U);

        auto second_loan_result = client.loan_request_slice(1U);
        require(second_loan_result.has_value());
        auto second_loan = std::move(second_loan_result).value();
        second_loan.payload()[0] = 2U;
        auto second_pending_result = std::move(second_loan).send();
        require(second_pending_result.has_value());
        auto second_response_result =
            std::move(second_pending_result).value().wait(2s);
        require(second_response_result.has_value());
        auto second_response = std::move(second_response_result).value();
        auto metrics = client_metrics_for(service);
        require(metrics.response_guards_current == 2U);
        require(metrics.response_guards_high_water == 2U);

        first_response = std::move(second_response);
        require(client_metrics_for(service).response_guards_current == 1U);
    }
    require(client_metrics_for(service).response_guards_current == 0U);

    stop.store(true, std::memory_order_release);
    (void)wake.wake();
    worker.join();
}

} // namespace

int main() {
    roundtrip_and_timeout_reuse();
    cancellation_and_activation_wake();
    ownership_and_capacity();
    pending_move_assignment_releases_both_calls();
    pending_move_assignment_preserves_in_flight_high_water();
    cancelled_pending_move_assignment_records_cancelled_destination();
    request_move_assignment_releases_both_requests();
    response_loan_move_assignment_releases_both_requests();
    request_loan_move_assignment_releases_capacity_and_handles_self_move();
    client_move_assignment_expires_old_activation_and_keeps_source_working();
    server_move_assignment_expires_old_activation_and_keeps_source_working();
    in_flight_limit_is_typed_client_capacity();
    server_disappearance_replacement_recovery();
    user_header_compile_contract();
    dynamic_slice_roundtrip();
    return 0;
}
