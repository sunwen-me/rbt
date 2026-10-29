#include <rbt/effect_scope.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void test_cleanup_is_lifo_and_shutdown_is_idempotent() {
    rbt::EffectScope scope("process", 7U);
    std::vector<int> order;
    scope.defer([&] { order.push_back(1); });
    scope.defer([&] { order.push_back(2); });
    scope.defer([&] { order.push_back(3); });

    scope.shutdown();
    scope.shutdown();

    require(order == std::vector<int> { 3, 2, 1 });
}

void test_shutdown_requests_stop_and_joins_task() {
    rbt::EffectScope scope("process", 8U);
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool stopped = false;
    auto handle = scope.spawn_background("loop", [&](std::stop_token token) {
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        condition.notify_one();
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        {
            std::lock_guard lock(mutex);
            stopped = true;
        }
        condition.notify_one();
    });

    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 1s, [&] { return started; }));
    }
    scope.shutdown();

    require(handle.stop_requested());
    require(stopped);
    require(scope.events().empty());
}

void test_critical_error_and_exception_become_events() {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<rbt::TaskEvent> observed;
    rbt::EffectScope scope("proc-error", 9U, [&](rbt::TaskEvent event) {
        {
            std::lock_guard lock(mutex);
            observed.push_back(std::move(event));
        }
        condition.notify_one();
    });
    const auto returned_error = scope.spawn_critical("returned-error", [](std::stop_token) {
        return rbt::TaskOutcome::error("model failed");
    });
    const auto threw = scope.spawn_critical("threw", [](std::stop_token) -> rbt::TaskOutcome {
        throw std::runtime_error("boom");
    });
    (void)returned_error;
    (void)threw;

    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 1s, [&] { return observed.size() == 2U; }));
    }

    scope.shutdown();
    require(observed.size() == 2U);
    for (const auto& event : observed) {
        require(event.kind == rbt::TaskKind::Critical);
    }
    const auto returned = std::find_if(observed.begin(), observed.end(), [](const auto& event) {
        return event.task_name == "returned-error";
    });
    const auto threw_event = std::find_if(observed.begin(), observed.end(), [](const auto& event) {
        return event.task_name == "threw";
    });
    require(returned != observed.end());
    require(threw_event != observed.end());
    require(returned->outcome.kind == rbt::TaskOutcomeKind::Error);
    require(returned->outcome.message == "model failed");
    require(threw_event->outcome.kind == rbt::TaskOutcomeKind::Exception);
    require(threw_event->outcome.message == "boom");
}

void test_background_classification_and_identity() {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<rbt::TaskEvent> observed;
    rbt::EffectScope scope("process-instance-42", 123U, [&](rbt::TaskEvent event) {
        {
            std::lock_guard lock(mutex);
            observed.push_back(std::move(event));
        }
        condition.notify_one();
    });
    const auto handle = scope.spawn_background("metrics", [](std::stop_token) {
        return rbt::TaskOutcome::completed();
    });
    (void)handle;

    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 1s, [&] { return observed.size() == 1U; }));
    }

    scope.shutdown();
    require(observed.size() == 1U);
    require(observed.front().process_instance == "process-instance-42");
    require(observed.front().activation_id == 123U);
    require(observed.front().task_name == "metrics");
    require(observed.front().is_background());
    require(!observed.front().is_critical());
    require(observed.front().outcome.is_completed());
}

void test_scope_cancellation_does_not_emit_unexpected_exit() {
    rbt::EffectScope scope("process-instance-43", 124U);
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    const auto handle = scope.spawn_critical("cancellable", [&](std::stop_token token) {
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        condition.notify_one();
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        return rbt::TaskOutcome::error("cancelled");
    });
    (void)handle;

    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 1s, [&] { return started; }));
    }
    scope.shutdown();
    require(scope.events().empty());
}

} // namespace

int main() {
    test_cleanup_is_lifo_and_shutdown_is_idempotent();
    test_shutdown_requests_stop_and_joins_task();
    test_critical_error_and_exception_become_events();
    test_background_classification_and_identity();
    test_scope_cancellation_does_not_emit_unexpected_exit();
    return 0;
}
