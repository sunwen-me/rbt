#ifndef RBT_CPP_EFFECT_SCOPE_HPP
#define RBT_CPP_EFFECT_SCOPE_HPP

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace rbt {

/// The result reported when a managed task leaves its entry point.
enum class TaskOutcomeKind : std::uint8_t {
    Completed,
    Error,
    Exception,
    // Panicked is retained as a terminology-compatible alias for Exception.
    Panicked = Exception,
};

/// A task result carried by a TaskEvent.
struct TaskOutcome {
    using Kind = TaskOutcomeKind;
    using Status = TaskOutcomeKind;

    TaskOutcomeKind kind { TaskOutcomeKind::Completed };
    std::string message;

    [[nodiscard]] static TaskOutcome completed() {
        return {};
    }

    [[nodiscard]] static TaskOutcome error(std::string detail) {
        return { TaskOutcomeKind::Error, std::move(detail) };
    }

    [[nodiscard]] static TaskOutcome exception(std::string detail) {
        return { TaskOutcomeKind::Exception, std::move(detail) };
    }

    [[nodiscard]] static TaskOutcome panicked(std::string detail) {
        return exception(std::move(detail));
    }

    [[nodiscard]] bool is_completed() const noexcept {
        return kind == TaskOutcomeKind::Completed;
    }

    [[nodiscard]] bool is_error() const noexcept {
        return kind == TaskOutcomeKind::Error;
    }

    [[nodiscard]] bool is_exception() const noexcept {
        return kind == TaskOutcomeKind::Exception;
    }

    [[nodiscard]] bool is_panicked() const noexcept {
        return is_exception();
    }

    friend bool operator==(const TaskOutcome&, const TaskOutcome&) = default;
};

/// The supervision class assigned to a managed task.
enum class TaskKind : std::uint8_t {
    Critical,
    Background,
};

using TaskClassification = TaskKind;

/// A task-exit event emitted by an EffectScope.
struct TaskEvent {
    std::string process_instance;
    std::uint64_t activation_id { 0U };
    std::string task_name;
    TaskKind kind { TaskKind::Background };
    TaskOutcome outcome;

    [[nodiscard]] bool is_critical() const noexcept {
        return kind == TaskKind::Critical;
    }

    [[nodiscard]] bool is_background() const noexcept {
        return kind == TaskKind::Background;
    }

    [[nodiscard]] bool critical() const noexcept {
        return is_critical();
    }

    [[nodiscard]] bool background() const noexcept {
        return is_background();
    }

    friend bool operator==(const TaskEvent&, const TaskEvent&) = default;
};

/// A non-owning control handle for one task owned by an EffectScope.
class TaskHandle {
  public:
    TaskHandle() = default;

    /// Requests cooperative cancellation of this task.
    void request_stop() const noexcept {
        if (control_ != nullptr) {
            control_->request_stop();
        }
    }

    /// Returns whether this task has been asked to stop.
    [[nodiscard]] bool stop_requested() const noexcept {
        return control_ != nullptr && control_->stop_requested();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return control_ != nullptr;
    }

  private:
    struct Control {
        std::stop_source stop_source;

        void request_stop() noexcept {
            stop_source.request_stop();
        }

        [[nodiscard]] bool stop_requested() const noexcept {
            return stop_source.stop_requested();
        }
    };

    explicit TaskHandle(std::shared_ptr<Control> control)
        : control_(std::move(control)) {
    }

    std::shared_ptr<Control> control_;

    friend class EffectScope;
};

/// Owns cleanup callbacks and cancellable critical/background tasks for one
/// activation.  Calling shutdown() is safe more than once; the destructor is
/// a synchronous safety net that requests stop and joins every jthread.
class EffectScope {
  public:
    using TaskEventSink = std::function<void(TaskEvent)>;

    EffectScope()
        : EffectScope({}, 0U) {
    }

    EffectScope(std::string process_instance,
                std::uint64_t activation_id,
                TaskEventSink event_sink = {})
        : process_instance_(std::move(process_instance))
        , activation_id_(activation_id)
        , event_state_(std::make_shared<EventState>(std::move(event_sink))) {
    }

    EffectScope(const EffectScope&) = delete;
    EffectScope& operator=(const EffectScope&) = delete;
    EffectScope(EffectScope&&) = delete;
    EffectScope& operator=(EffectScope&&) = delete;

    ~EffectScope() noexcept {
        shutdown();
    }

    /// Returns the process-lifetime identity copied into task events.
    [[nodiscard]] const std::string& process_instance() const noexcept {
        return process_instance_;
    }

    /// Returns the activation identity copied into task events.
    [[nodiscard]] std::uint64_t activation_id() const noexcept {
        return activation_id_;
    }

    /// Returns whether explicit shutdown (or the destructor) has started.
    [[nodiscard]] bool is_shutdown() const noexcept {
        std::lock_guard lock(mutex_);
        return shutdown_started_;
    }

    /// Adds a synchronous cleanup callback to the LIFO stack.
    template <typename F>
    void defer(F&& cleanup) {
        Cleanup action(std::forward<F>(cleanup));
        bool run_now = false;
        {
            std::lock_guard lock(mutex_);
            if (shutdown_started_) {
                run_now = true;
            } else {
                actions_.emplace_back(std::move(action));
            }
        }
        if (run_now) {
            action.invoke();
        }
    }

    /// Alias for defer() emphasizing that the callback is synchronous.
    template <typename F>
    void defer_sync(F&& cleanup) {
        defer(std::forward<F>(cleanup));
    }

    /// Returns a snapshot of all events emitted so far.
    [[nodiscard]] std::vector<TaskEvent> events() const {
        std::lock_guard lock(event_state_->mutex);
        return event_state_->events;
    }

    /// Adds a critical task to this activation.
    template <typename F>
    [[nodiscard]] TaskHandle spawn_critical(std::string task_name, F&& task) {
        return spawn(TaskKind::Critical, std::move(task_name), std::forward<F>(task));
    }

    /// Adds a background task to this activation.
    template <typename F>
    [[nodiscard]] TaskHandle spawn_background(std::string task_name, F&& task) {
        return spawn(TaskKind::Background, std::move(task_name), std::forward<F>(task));
    }

    /// Requests stop, joins all managed tasks, and runs cleanup in strict LIFO
    /// order.  A second call does nothing.
    void shutdown() noexcept {
        std::vector<Action> actions;
        {
            std::lock_guard lock(mutex_);
            if (shutdown_started_) {
                return;
            }
            shutdown_started_ = true;
            actions.swap(actions_);
        }

        for (auto& action : actions) {
            request_stop(action);
        }

        while (!actions.empty()) {
            Action action = std::move(actions.back());
            actions.pop_back();
            std::visit(
                [](auto& value) noexcept {
                    using Value = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, Cleanup>) {
                        value.invoke();
                    } else {
                        value->stop_and_join();
                    }
                },
                action);
        }
    }

  private:
    class Cleanup {
      private:
        struct Concept {
            virtual ~Concept() = default;
            virtual void invoke() = 0;
        };

        template <typename F>
        struct Model final : Concept {
            explicit Model(F&& function)
                : function(std::forward<F>(function)) {
            }

            void invoke() override {
                std::invoke(function);
            }

            F function;
        };

      public:
        template <typename F>
        explicit Cleanup(F&& function)
            : implementation_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(function))) {
        }

        Cleanup(Cleanup&&) noexcept = default;
        Cleanup& operator=(Cleanup&&) noexcept = default;
        Cleanup(const Cleanup&) = delete;
        Cleanup& operator=(const Cleanup&) = delete;

        void invoke() noexcept {
            try {
                if (implementation_ != nullptr) {
                    implementation_->invoke();
                }
            } catch (...) {
                // A throwing cleanup must not prevent the remaining LIFO
                // callbacks from running, nor escape the destructor.
            }
        }

      private:
        std::unique_ptr<Concept> implementation_;
    };

    struct EventState {
        explicit EventState(TaskEventSink sink)
            : sink(std::move(sink)) {
        }

        mutable std::mutex mutex;
        std::vector<TaskEvent> events;
        TaskEventSink sink;
    };

    struct TaskMetadata {
        std::string process_instance;
        std::uint64_t activation_id { 0U };
        std::string task_name;
        TaskKind kind { TaskKind::Background };
    };

    struct TaskAction {
        TaskAction(std::jthread&& task_thread, std::shared_ptr<TaskHandle::Control> task_control)
            : thread(std::move(task_thread))
            , control(std::move(task_control)) {
        }

        void request_stop() noexcept {
            control->request_stop();
            thread.request_stop();
        }

        void stop_and_join() noexcept {
            request_stop();
            if (thread.joinable()) {
                try {
                    thread.join();
                } catch (...) {
                    // std::jthread's destructor remains the final safety net.
                }
            }
        }

        std::jthread thread;
        std::shared_ptr<TaskHandle::Control> control;
    };

    using Action = std::variant<Cleanup, std::unique_ptr<TaskAction>>;

    static void request_stop(Action& action) noexcept {
        std::visit(
            [](auto& value) noexcept {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, std::unique_ptr<TaskAction>>) {
                    value->request_stop();
                }
            },
            action);
    }

    template <typename T>
    [[nodiscard]] static TaskOutcome normalize_result(T&& result) {
        using Result = std::remove_cvref_t<T>;
        if constexpr (std::is_same_v<Result, TaskOutcome>) {
            return std::forward<T>(result);
        } else if constexpr (std::is_same_v<Result, TaskOutcomeKind>) {
            return { result, {} };
        } else if constexpr (std::is_same_v<Result, bool>) {
            return result ? TaskOutcome::completed() : TaskOutcome::error("task returned false");
        } else if constexpr (std::is_same_v<Result, std::string>) {
            return result.empty() ? TaskOutcome::completed() : TaskOutcome::error(std::forward<T>(result));
        } else {
            static_assert(always_false<Result>,
                          "a managed task must return void, TaskOutcome, TaskOutcomeKind, bool, or std::string");
        }
    }

    template <typename>
    static constexpr bool always_false = false;

    template <typename F>
    static void run_task(F task,
                         std::stop_token token,
                         const std::shared_ptr<TaskHandle::Control>& control,
                         const std::shared_ptr<EventState>& event_state,
                         const std::shared_ptr<const TaskMetadata>& metadata) noexcept {
        TaskOutcome outcome = TaskOutcome::completed();
        try {
            if constexpr (std::is_invocable_v<F&, std::stop_token>) {
                if constexpr (std::is_void_v<std::invoke_result_t<F&, std::stop_token>>) {
                    std::invoke(task, token);
                } else {
                    outcome = normalize_result(std::invoke(task, token));
                }
            } else if constexpr (std::is_invocable_v<F&>) {
                if constexpr (std::is_void_v<std::invoke_result_t<F&>>) {
                    std::invoke(task);
                } else {
                    outcome = normalize_result(std::invoke(task));
                }
            } else {
                static_assert(always_false<F>, "a managed task must be invocable with std::stop_token or no arguments");
            }
        } catch (const std::exception& error) {
            try {
                outcome = TaskOutcome::exception(error.what());
            } catch (...) {
                outcome = TaskOutcome::exception("std::exception");
            }
        } catch (...) {
            outcome = TaskOutcome::exception("unknown exception");
        }

        // A stop request means the owner is intentionally winding this task
        // down.  It is not an unexpected exit and must not produce an event.
        if (token.stop_requested() || control->stop_requested()) {
            return;
        }

        try {
            TaskEvent event {
                metadata->process_instance,
                metadata->activation_id,
                metadata->task_name,
                metadata->kind,
                std::move(outcome),
            };
            std::function<void(TaskEvent)> sink;
            {
                std::lock_guard lock(event_state->mutex);
                event_state->events.push_back(event);
                sink = event_state->sink;
            }
            if (sink) {
                sink(std::move(event));
            }
        } catch (...) {
            // Event delivery is part of the thread boundary as well.  A
            // broken observer must never terminate a managed std::jthread.
        }
    }

    template <typename F>
    [[nodiscard]] TaskHandle spawn(TaskKind kind, std::string task_name, F&& task) {
        auto control = std::make_shared<TaskHandle::Control>();
        auto metadata = std::make_shared<TaskMetadata>(TaskMetadata {
            process_instance_,
            activation_id_,
            std::move(task_name),
            kind,
        });
        auto event_state = event_state_;
        auto task_thread = std::jthread(
            [task = std::forward<F>(task), control, event_state, metadata](std::stop_token jthread_token) mutable noexcept {
                std::stop_callback stop_bridge(jthread_token, [control] { control->request_stop(); });
                run_task(std::move(task), control->stop_source.get_token(), control, event_state, metadata);
            });
        auto action = std::make_unique<TaskAction>(std::move(task_thread), control);
        TaskHandle handle(control);

        bool discard = false;
        {
            std::lock_guard lock(mutex_);
            if (shutdown_started_) {
                discard = true;
            } else {
                actions_.emplace_back(std::move(action));
            }
        }
        if (discard) {
            action->stop_and_join();
        }
        return handle;
    }

    std::string process_instance_;
    std::uint64_t activation_id_ { 0U };
    std::shared_ptr<EventState> event_state_;
    mutable std::mutex mutex_;
    std::vector<Action> actions_;
    bool shutdown_started_ { false };
};

} // namespace rbt

#endif // RBT_CPP_EFFECT_SCOPE_HPP
