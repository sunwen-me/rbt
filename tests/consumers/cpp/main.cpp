#include <rbt/demo.hpp>
#include <rbt/rpc.hpp>
#include <rbt/runtime.hpp>
#include <rbt/wait.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

constexpr auto INSTALL_WAIT_TOKEN = rbt::WaitToken::new_token(7U);
constexpr auto INSTALL_WAIT_PROGRESS = rbt::WaitProgress::samples(1U);
constexpr auto INSTALL_WAIT_EVENT = rbt::ReadyEvent::synthetic_source(INSTALL_WAIT_TOKEN);

static_assert(INSTALL_WAIT_TOKEN.value() == 7U);
static_assert(INSTALL_WAIT_PROGRESS.samples_processed() == 1U);
static_assert(!INSTALL_WAIT_PROGRESS.should_stop());
static_assert(INSTALL_WAIT_EVENT.source_token() == INSTALL_WAIT_TOKEN);
static_assert(INSTALL_WAIT_EVENT.event_kind() == rbt::ReadyEventKind::Source);
static_assert(INSTALL_WAIT_EVENT.is_synthetic());

using InstalledWaitContext = rbt::WaitContext;
using InstalledWaitError = rbt::WaitError;
using InstalledWaitExit = rbt::WaitExit;
using InstalledWaitResult = rbt::WaitResult<rbt::WaitProgress>;
using InstalledWaitStats = rbt::WaitStats;
using InstalledWaitStatsSnapshot = rbt::WaitStatsSnapshot;
using InstalledWaitWakeHandle = rbt::WaitWakeHandle;

static_assert(std::is_same_v<InstalledWaitContext, rbt::WaitContext>);
static_assert(std::is_same_v<InstalledWaitError, rbt::WaitError>);
static_assert(std::is_same_v<InstalledWaitExit, rbt::WaitExit>);
static_assert(std::is_same_v<InstalledWaitResult, rbt::WaitResult<rbt::WaitProgress>>);
static_assert(std::is_same_v<InstalledWaitStats, rbt::WaitStats>);
static_assert(std::is_same_v<InstalledWaitStatsSnapshot, rbt::WaitStatsSnapshot>);
static_assert(std::is_same_v<InstalledWaitWakeHandle, rbt::WaitWakeHandle>);

class InstalledComponent final : public rbt::Component {
  public:
    rbt::Result<void> prepare() override {
        return {};
    }

    rbt::ComponentId id() const override {
        return "installed-consumer";
    }

    std::vector<rbt::RequirementSpec> requirements() const override {
        return {};
    }

    rbt::Result<void> activate(rbt::ActivationContext&) override {
        return {};
    }
};

void assert_rpc_surface() {
    using Request = rbt::demo::Frame;
    using Response = rbt::demo::Detection;
    using Client = rbt::RpcClient<Request, Response>;
    using Server = rbt::RpcServer<Request, Response>;

    static_assert(rbt::demo::FRAME_DESCRIPTOR.header_size == sizeof(Request));
    static_assert(rbt::demo::DETECTION_DESCRIPTOR.header_size == sizeof(Response));

    const auto service_name = std::string("rbt/install-smoke/rpc/cpp/")
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto descriptor = rbt::RpcDescriptor::for_fixed<Request, Response>(service_name);
    auto server_result = Server::open_or_create(descriptor);
    require(server_result.has_value());
    auto server = std::move(server_result).value();
    auto client_result = Client::open(descriptor);
    require(client_result.has_value());
    auto client = std::move(client_result).value();

    std::atomic<bool> server_ok { false };
    std::thread server_thread([server = std::move(server), &server_ok]() mutable {
        auto received = server.receive_timeout(std::chrono::seconds(2));
        if (!received.has_value() || !received.value().has_value()) {
            return;
        }
        auto request = std::move(received).value().value();
        const auto input = request.payload();
        auto response_result = request.loan_response();
        if (!response_result.has_value()) {
            return;
        }
        auto response = std::move(response_result).value();
        auto& output = response.payload();
        output.sequence = input.sequence;
        output.value = input.value + 7;
        if (response.send().has_value()) {
            server_ok.store(true, std::memory_order_release);
        }
    });

    const Request request { 17, 25 };
    auto call_result = client.call(request, std::chrono::seconds(2));
    require(call_result.has_value());
    auto response = std::move(call_result).value();
    require(response.payload().sequence == 17);
    require(response.payload().value == 32);
    server_thread.join();
    require(server_ok.load(std::memory_order_acquire));
    std::printf("RBT_RPC_INSTALL_SMOKE_OK cpp result=%llu\n",
                static_cast<unsigned long long>(response.payload().value));
}

} // namespace

int main() {
    static_assert(rbt::SDK_VERSION == std::string_view("0.8.0"));
    static_assert(rbt::demo::FRAME_DESCRIPTOR.header_size == sizeof(rbt::demo::Frame));
    assert_rpc_surface();
    rbt::ComponentRuntime runtime(std::make_unique<InstalledComponent>());
    return runtime.state().is(rbt::ComponentStateKind::Preparing) ? 0 : 1;
}
