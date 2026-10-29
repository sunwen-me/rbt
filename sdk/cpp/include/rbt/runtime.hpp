#ifndef RBT_CPP_RUNTIME_HPP
#define RBT_CPP_RUNTIME_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <rbt/effect_scope.hpp>
#include <rbt/rpc.hpp>
#include <rbt/stream.hpp>
#include <rbt/types.hpp>
#include <rbt/version.hpp>

namespace rbt {

// Result<void> is intentionally supplied by the runtime header.  The data
// SDK's primary Result<T> predates the component API and cannot instantiate a
// std::variant<void, ...>.  Keeping the specialization here preserves the
// existing data-plane API while allowing a component to return `{}` from
// prepare/activate and to attach a useful diagnostic to a failure.
template <>
class Result<void> {
  public:
    Result() noexcept = default;

    Result(StreamError error)
        : error_(error)
        , message_(to_string(error)) {
    }

    explicit Result(std::string message)
        : error_(StreamError::Transport)
        , message_(std::move(message)) {
    }

    explicit Result(const char* message)
        : Result(std::string(message == nullptr ? "component operation failed" : message)) {
    }

    [[nodiscard]] static Result failure(std::string message) {
        return Result(std::move(message));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    void value() const noexcept {
    }

    [[nodiscard]] StreamError error() const noexcept {
        return error_.value_or(StreamError::Transport);
    }

    [[nodiscard]] const std::string& message() const noexcept {
        return message_;
    }

  private:
    std::optional<StreamError> error_;
    std::string message_;
};

using ComponentId = std::string;
using ProcessInstanceId = std::string;
using CapabilityInstanceId = std::string;
using ComponentInstanceId = ProcessInstanceId;
using ActivationId = std::uint64_t;

/// Returns a fresh RFC 4122 version-4 UUID in canonical lower-case form.
[[nodiscard]] std::string generate_uuid();

/// A wire-owned message descriptor.  Unlike MessageTypeDescriptor, every
/// string is owned and the Rust wire format intentionally has no `fields`
/// member.  The local C++ field signature is retained separately when a
/// generated descriptor is converted to this DTO.
struct OwnedMessageTypeDescriptor {
    std::string namespace_name;
    std::string name;
    std::uint16_t abi_major { 0U };
    MessageShape shape { MessageShape::Fixed };
    std::uint32_t header_size { 0U };
    std::uint32_t header_align { 0U };
    std::uint32_t element_size { 0U };
    std::uint32_t element_align { 0U };
    std::string layout_hash;
    std::string local_fields;

    [[nodiscard]] std::string qualified_name() const;
    [[nodiscard]] bool wire_equal(const OwnedMessageTypeDescriptor& other) const noexcept;

    friend bool operator==(const OwnedMessageTypeDescriptor&, const OwnedMessageTypeDescriptor&) = default;
};

using WireMessageTypeDescriptor = OwnedMessageTypeDescriptor;
using MessageTypeDescriptorOwned = OwnedMessageTypeDescriptor;

/// An owning stream descriptor used by the control plane.
struct OwnedStreamDescriptor {
    std::string service_name;
    OwnedMessageTypeDescriptor message_type;
    StreamProfile profile { StreamProfile::Sensor };
    TransportFeatures transport;
    StreamResourceConfig resources;

    [[nodiscard]] StreamDescriptor borrowed() const;

    template <typename T>
    [[nodiscard]] static OwnedStreamDescriptor from(const StreamDescriptor& descriptor) {
        OwnedStreamDescriptor result;
        result.service_name = descriptor.service_name;
        const auto& type = descriptor.message_type;
        result.message_type.namespace_name = std::string(type.package);
        result.message_type.name = std::string(type.name);
        result.message_type.abi_major = type.abi_major;
        result.message_type.shape = type.shape;
        result.message_type.header_size = type.header_size;
        result.message_type.header_align = type.header_align;
        result.message_type.element_size = type.element_size;
        result.message_type.element_align = type.element_align;
        result.message_type.layout_hash = lowercase_hex(type.layout_hash);
        result.message_type.local_fields = std::string(type.fields);
        result.profile = descriptor.profile;
        result.transport = descriptor.transport;
        result.resources = descriptor.resources;
        (void)sizeof(T);
        return result;
    }

    [[nodiscard]] static OwnedStreamDescriptor from(const StreamDescriptor& descriptor);

    friend bool operator==(const OwnedStreamDescriptor&, const OwnedStreamDescriptor&) = default;
};

/// An owning RPC descriptor used by the control plane.
struct OwnedRpcDescriptor {
    std::string service_name;
    OwnedMessageTypeDescriptor request_type;
    OwnedMessageTypeDescriptor response_type;
    RpcTransportFeatures transport { RpcTransportFeatures::rpc_sidecar_v1() };
    RpcResourceConfig resources;

    /// Returns a borrowed descriptor whose message string views refer to this object.
    [[nodiscard]] RpcDescriptor borrowed() const;

    /// Copies all request/response metadata into an owning DTO.
    [[nodiscard]] static OwnedRpcDescriptor from(const RpcDescriptor& descriptor);

    friend bool operator==(const OwnedRpcDescriptor&, const OwnedRpcDescriptor&) = default;
};

struct RequirementSpec {
    std::string requirement_name;
    std::string capability_type;

    friend bool operator==(const RequirementSpec&, const RequirementSpec&) = default;
};

struct CapabilityDescriptor {
    CapabilityInstanceId capability_instance;
    ComponentId provider_component;
    ProcessInstanceId provider_process;
    ActivationId activation { 0U };
    OwnedStreamDescriptor stream;
    std::optional<OwnedRpcDescriptor> rpc {};

    [[nodiscard]] const OwnedStreamDescriptor* try_stream() const noexcept {
        return rpc ? nullptr : &stream;
    }

    [[nodiscard]] const OwnedRpcDescriptor* try_rpc() const noexcept {
        return rpc ? &*rpc : nullptr;
    }

    friend bool operator==(const CapabilityDescriptor&, const CapabilityDescriptor&) = default;
};

struct ProvidedCapability {
    std::string capability_type;
    CapabilityDescriptor descriptor;

    friend bool operator==(const ProvidedCapability&, const ProvidedCapability&) = default;
};

struct ResolvedBinding {
    std::string requirement_name;
    std::string capability_type;
    ComponentId provider;
    CapabilityDescriptor descriptor;

    friend bool operator==(const ResolvedBinding&, const ResolvedBinding&) = default;
};

struct RequirementSnapshot {
    RequirementSpec requirement;
    std::optional<ResolvedBinding> binding;

    friend bool operator==(const RequirementSnapshot&, const RequirementSnapshot&) = default;
};

struct ActivationSnapshot {
    ActivationId activation_id { 0U };
    std::vector<ResolvedBinding> bindings;

    friend bool operator==(const ActivationSnapshot&, const ActivationSnapshot&) = default;
};

struct CapabilitySnapshot {
    std::string capability_type;
    CapabilityInstanceId capability_instance;
    CapabilityDescriptor descriptor;

    friend bool operator==(const CapabilitySnapshot&, const CapabilitySnapshot&) = default;
};

/// Rust's ComponentState is externally tagged.  The string fields are used
/// for Blocked/Failed reasons and are empty for the unit-like states.
enum class ComponentStateKind : std::uint8_t {
    Declared,
    Preparing,
    Blocked,
    Activating,
    Active,
    Quiescing,
    Failed,
};

struct ComponentState {
    ComponentStateKind kind { ComponentStateKind::Preparing };
    std::string reason;

    [[nodiscard]] static ComponentState declared();
    [[nodiscard]] static ComponentState preparing();
    [[nodiscard]] static ComponentState blocked(std::string reason);
    [[nodiscard]] static ComponentState activating();
    [[nodiscard]] static ComponentState active();
    [[nodiscard]] static ComponentState quiescing();
    [[nodiscard]] static ComponentState failed(std::string reason);
    [[nodiscard]] bool is(ComponentStateKind expected) const noexcept;
    [[nodiscard]] std::string name() const;

    friend bool operator==(const ComponentState&, const ComponentState&) = default;
};

struct ComponentSnapshot {
    std::uint16_t snapshot_version { COMPOSER_SNAPSHOT_VERSION };
    ComponentId component_id;
    ProcessInstanceId process_instance;
    ComponentState state;
    std::optional<ActivationSnapshot> activation;
    std::vector<RequirementSnapshot> requirements;
    std::vector<CapabilitySnapshot> provided_capabilities;
    std::vector<StreamPressure> stream_pressures;
    std::vector<RpcPressure> rpc_pressures;

    friend bool operator==(const ComponentSnapshot&, const ComponentSnapshot&) = default;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct Revision {
    std::string epoch;
    std::uint64_t sequence { 0U };

    friend bool operator==(const Revision&, const Revision&) = default;
};

/// Owning client-side control DTOs.  `kind` is the Rust serde tag.
struct ClientMessage {
    enum class Kind : std::uint8_t {
        Hello,
        Snapshot,
        Inspect,
    };

    Kind kind { Kind::Hello };
    ComponentId component_id;
    ProcessInstanceId process_instance;
    std::uint16_t control_protocol_version { CONTROL_PROTOCOL_VERSION };
    std::uint16_t snapshot_version { COMPOSER_SNAPSHOT_VERSION };
    ComponentSnapshot snapshot;

    [[nodiscard]] static ClientMessage hello(ComponentId component, ProcessInstanceId process);
    [[nodiscard]] static ClientMessage snapshot_message(ComponentSnapshot value);
    [[nodiscard]] static ClientMessage inspect(
        std::uint16_t control_version = CONTROL_PROTOCOL_VERSION,
        std::uint16_t snapshot_version = COMPOSER_SNAPSHOT_VERSION);
};

/// Owning server-side control DTOs.  Only the fields relevant to a variant
/// are populated, which keeps decoding independent of temporary JSON text.
struct ServerMessage {
    enum class Kind : std::uint8_t {
        Activate,
        Deactivate,
        ProcessConflict,
        ProtocolMismatch,
        InspectResponse,
    };

    Kind kind { Kind::ProtocolMismatch };
    Revision revision;
    std::vector<ResolvedBinding> bindings;
    ProcessInstanceId expected_process_instance;
    ActivationId expected_activation { 0U };
    std::string reason;
    ComponentId component_id;
    ProcessInstanceId incumbent;
    ProcessInstanceId contender;
    std::uint16_t expected_control_protocol_version { CONTROL_PROTOCOL_VERSION };
    std::uint16_t expected_snapshot_version { COMPOSER_SNAPSHOT_VERSION };
    std::optional<std::uint16_t> received_control_protocol_version;
    std::optional<std::uint16_t> received_snapshot_version;
    std::string raw_view_json;

    [[nodiscard]] bool carries_revision() const noexcept;
};

/// Small protocol codec used both by the runtime and by parity tests.  All
/// decode methods return false and fill `error` rather than throwing across a
/// socket/thread boundary.
class ControlCodec {
  public:
    [[nodiscard]] static std::string encode(const ClientMessage& message);
    [[nodiscard]] static std::string encode_snapshot(const ComponentSnapshot& snapshot);
    [[nodiscard]] static bool decode_server(std::string_view line,
                                             ServerMessage& message,
                                             std::string* error = nullptr);
    [[nodiscard]] static bool decode_snapshot(std::string_view json,
                                              ComponentSnapshot& snapshot,
                                              std::string* error = nullptr);
};

struct ProtocolMismatch {
    std::uint16_t expected_control_protocol_version { CONTROL_PROTOCOL_VERSION };
    std::uint16_t expected_snapshot_version { COMPOSER_SNAPSHOT_VERSION };
    std::optional<std::uint16_t> received_control_protocol_version;
    std::optional<std::uint16_t> received_snapshot_version;
    std::string reason;
};

struct RuntimeOptions {
    std::string composer_address { "127.0.0.1:7447" };
    std::chrono::milliseconds connect_timeout { 1000 };
    std::chrono::milliseconds poll_interval { 50 };
    std::vector<std::chrono::milliseconds> reconnect_backoff {
        std::chrono::milliseconds(100),
        std::chrono::milliseconds(200),
        std::chrono::milliseconds(500),
        std::chrono::milliseconds(1000),
    };
};

enum class TaskResult : std::uint8_t {
    Ok,
    Error,
};

template <typename T>
struct CapabilityTag {
    static constexpr std::string_view name = "";
};

namespace detail {

template <typename F>
[[nodiscard]] TaskOutcome invoke_task(F&& task, std::stop_token token);

template <typename T, typename = void>
struct capability_name_impl {
    static std::string get() {
        return std::string(CapabilityTag<T>::name);
    }
};

template <typename T>
struct capability_name_impl<T, std::void_t<decltype(T::TYPE_NAME)>> {
    static std::string get() {
        return std::string(T::TYPE_NAME);
    }
};

template <typename T>
[[nodiscard]] std::string capability_name() {
    return capability_name_impl<T>::get();
}

} // namespace detail

template <typename T>
class CapabilityHandle {
  public:
    CapabilityHandle() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }

    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

    [[nodiscard]] const ResolvedBinding& binding() const noexcept {
        return binding_;
    }

    [[nodiscard]] const OwnedStreamDescriptor& stream() const {
        const auto* value = try_stream();
        if (value == nullptr) {
            throw std::logic_error("CapabilityHandle::stream called for an RPC capability");
        }
        return *value;
    }

    [[nodiscard]] const OwnedStreamDescriptor* try_stream() const noexcept {
        return valid_ ? binding_.descriptor.try_stream() : nullptr;
    }

    [[nodiscard]] const OwnedRpcDescriptor* try_rpc() const noexcept {
        return valid_ ? binding_.descriptor.try_rpc() : nullptr;
    }

    [[nodiscard]] const OwnedRpcDescriptor& rpc() const {
        const auto* value = try_rpc();
        if (value == nullptr) {
            throw std::logic_error("CapabilityHandle::rpc called for a stream capability");
        }
        return *value;
    }

    [[nodiscard]] const ComponentId& provider() const noexcept {
        return binding_.provider;
    }

    [[nodiscard]] const CapabilityInstanceId& capability_instance() const noexcept {
        return binding_.descriptor.capability_instance;
    }

    /// Creates a generated fixed-message subscriber while restoring the local
    /// C++ field signature omitted by the Rust wire descriptor.
    template <typename Message>
    [[nodiscard]] Result<FixedSubscriber<Message>> subscriber() const {
        const auto* owned_stream = try_stream();
        if (owned_stream == nullptr) {
            return Result<FixedSubscriber<Message>>(StreamError::AbiMismatch);
        }
        auto descriptor = owned_stream->borrowed();
        const auto& expected = FixedMessageBinding<Message>::descriptor();
        descriptor.message_type.fields = expected.fields;
        return FixedSubscriber<Message>::create(descriptor, expected);
    }

    /// Creates a generated slice-message subscriber while restoring the
    /// local C++ field signature omitted by the Rust wire descriptor.
    template <typename Header, typename Element>
    [[nodiscard]] Result<SliceSubscriber<Header, Element>> slice_subscriber() const {
        const auto* owned_stream = try_stream();
        if (owned_stream == nullptr) {
            return Result<SliceSubscriber<Header, Element>>(StreamError::AbiMismatch);
        }
        auto descriptor = owned_stream->borrowed();
        const auto& expected = SliceMessageBinding<Header, Element>::descriptor();
        descriptor.message_type.fields = expected.fields;
        return SliceSubscriber<Header, Element>::create(descriptor, expected);
    }

  private:
    ResolvedBinding binding_;
    bool valid_ { false };
    std::string error_;

    explicit CapabilityHandle(ResolvedBinding binding)
        : binding_(std::move(binding))
        , valid_(true) {
    }

    static CapabilityHandle invalid(std::string error) {
        CapabilityHandle result;
        result.error_ = std::move(error);
        return result;
    }

    friend class ActivationContext;
};

class ActivationContext {
  public:
    ActivationContext(ComponentId component,
                      ProcessInstanceId process,
                      ActivationId activation,
                      std::vector<ResolvedBinding> bindings,
                      EffectScope& scope);

    ActivationContext(const ActivationContext&) = delete;
    ActivationContext& operator=(const ActivationContext&) = delete;
    ActivationContext(ActivationContext&&) = delete;
    ActivationContext& operator=(ActivationContext&&) = delete;
    ~ActivationContext() = default;

    [[nodiscard]] const ComponentId& component() const noexcept;
    [[nodiscard]] const ProcessInstanceId& process_instance() const noexcept;
    [[nodiscard]] ActivationId activation_id() const noexcept;
    [[nodiscard]] const std::vector<ResolvedBinding>& bindings() const noexcept;
    [[nodiscard]] EffectScope& scope() noexcept;

    template <typename T>
    [[nodiscard]] CapabilityHandle<T> require(std::string_view requirement_name) const {
        const std::string expected_type = detail::capability_name<T>();
        const ResolvedBinding* found = nullptr;
        for (const auto& binding : bindings_) {
            if (binding.requirement_name != requirement_name) {
                continue;
            }
            if (found != nullptr) {
                return CapabilityHandle<T>::invalid("duplicate binding for requirement "
                                                   + std::string(requirement_name));
            }
            found = &binding;
        }
        if (found == nullptr) {
            return CapabilityHandle<T>::invalid("missing binding for requirement "
                                                + std::string(requirement_name));
        }
        if (!expected_type.empty() && found->capability_type != expected_type) {
            return CapabilityHandle<T>::invalid("binding for requirement " + std::string(requirement_name)
                                                + " has capability type " + found->capability_type
                                                + ", expected " + expected_type);
        }
        return CapabilityHandle<T>(*found);
    }

    template <typename T>
    [[nodiscard]] CapabilityHandle<T> provide(CapabilityInstanceId capability_instance,
                                                const OwnedStreamDescriptor& stream) {
        const std::string type = detail::capability_name<T>();
        CapabilityDescriptor descriptor {
            std::move(capability_instance),
            component_,
            process_,
            activation_,
            stream,
        };
        ResolvedBinding binding {
            {},
            type,
            component_,
            descriptor,
        };
        provided_.push_back(ProvidedCapability { type, descriptor });
        return CapabilityHandle<T>(std::move(binding));
    }

    template <typename T>
    [[nodiscard]] CapabilityHandle<T> provide(CapabilityInstanceId capability_instance,
                                                const StreamDescriptor& stream) {
        return provide<T>(std::move(capability_instance), OwnedStreamDescriptor::from(stream));
    }

    template <typename T>
    [[nodiscard]] CapabilityHandle<T> provide_rpc(CapabilityInstanceId capability_instance,
                                                    const OwnedRpcDescriptor& rpc) {
        const std::string type = detail::capability_name<T>();
        CapabilityDescriptor descriptor {
            std::move(capability_instance),
            component_,
            process_,
            activation_,
            {},
            rpc,
        };
        ResolvedBinding binding {
            {},
            type,
            component_,
            descriptor,
        };
        provided_.push_back(ProvidedCapability { type, descriptor });
        return CapabilityHandle<T>(std::move(binding));
    }

    template <typename T>
    [[nodiscard]] CapabilityHandle<T> provide_rpc(CapabilityInstanceId capability_instance,
                                                    const RpcDescriptor& rpc) {
        return provide_rpc<T>(std::move(capability_instance), OwnedRpcDescriptor::from(rpc));
    }

    template <typename T>
    [[nodiscard]] CapabilityHandle<T> provide_rpc(const OwnedRpcDescriptor& rpc) {
        return provide_rpc<T>(generate_uuid(), rpc);
    }

    template <typename T>
    [[nodiscard]] CapabilityHandle<T> provide_rpc(const RpcDescriptor& rpc) {
        return provide_rpc<T>(generate_uuid(), OwnedRpcDescriptor::from(rpc));
    }

    template <typename T>
    [[nodiscard]] CapabilityHandle<T> provide(const OwnedStreamDescriptor& stream) {
        return provide<T>(generate_uuid(), stream);
    }

    template <typename T>
    [[nodiscard]] CapabilityHandle<T> provide() {
        return provide<T>(generate_uuid(), OwnedStreamDescriptor {});
    }

    [[nodiscard]] const std::vector<ProvidedCapability>& capabilities() const noexcept;

    template <typename F>
    [[nodiscard]] TaskHandle spawn_critical(std::string task_name, F&& task) {
        return scope_.spawn_critical(std::move(task_name),
                                     [task = std::forward<F>(task)](std::stop_token token) mutable {
                                         return detail::invoke_task(std::move(task), token);
                                     });
    }

    template <typename F>
    [[nodiscard]] TaskHandle spawn_background(std::string task_name, F&& task) {
        return scope_.spawn_background(std::move(task_name),
                                       [task = std::forward<F>(task)](std::stop_token token) mutable {
                                           return detail::invoke_task(std::move(task), token);
                                       });
    }

  private:
    ComponentId component_;
    ProcessInstanceId process_;
    ActivationId activation_ { 0U };
    std::vector<ResolvedBinding> bindings_;
    std::vector<ProvidedCapability> provided_;
    EffectScope& scope_;
};

class Component {
  public:
    virtual ~Component() = default;

    /// Preparation is process-lifetime work.  A component instance is
    /// prepared once even when the composer sends multiple activations.
    virtual Result<void> prepare() = 0;
    virtual ComponentId id() const = 0;
    virtual std::vector<RequirementSpec> requirements() const = 0;
    virtual Result<void> activate(ActivationContext& context) = 0;
};

class ComponentRuntime {
  public:
    ComponentRuntime(std::unique_ptr<Component> component, RuntimeOptions options = {});
    ComponentRuntime(const ComponentRuntime&) = delete;
    ComponentRuntime& operator=(const ComponentRuntime&) = delete;
    ComponentRuntime(ComponentRuntime&&) = delete;
    ComponentRuntime& operator=(ComponentRuntime&&) = delete;
    ~ComponentRuntime();

    /// Runs until stop() is requested or the component process receives an
    /// external stop request.  Returns zero for a clean runtime shutdown.
    int run();
    void stop() noexcept;

    [[nodiscard]] ComponentSnapshot snapshot() const;
    [[nodiscard]] ComponentState state() const;
    [[nodiscard]] bool control_plane_connected() const noexcept;
    [[nodiscard]] std::optional<ProtocolMismatch> last_protocol_mismatch() const;

    /// These hooks are intentionally small and synchronous.  They are useful
    /// for deterministic unit tests and are also the exact handlers used by
    /// the control thread's decoded commands/events.
    bool apply_server_message(const ServerMessage& message);
    void apply_task_event(const TaskEvent& event);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using Runtime = ComponentRuntime;
using ComponentRunner = ComponentRuntime;

} // namespace rbt

// Helpers used by ActivationContext's task wrappers are defined after the
// public class declarations so they remain implementation details without a
// separate template source file.
namespace rbt::detail {

template <typename T>
[[nodiscard]] inline TaskOutcome normalize_task_result(T&& result) {
    using ResultType = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<ResultType, TaskOutcome>) {
        return std::forward<T>(result);
    } else if constexpr (std::is_same_v<ResultType, TaskResult>) {
        return result == TaskResult::Ok ? TaskOutcome::completed() : TaskOutcome::error("task returned error");
    } else if constexpr (std::is_same_v<ResultType, TaskOutcomeKind>) {
        return TaskOutcome { result, {} };
    } else if constexpr (std::is_same_v<ResultType, bool>) {
        return result ? TaskOutcome::completed() : TaskOutcome::error("task returned false");
    } else if constexpr (std::is_same_v<ResultType, std::string>) {
        return result.empty() ? TaskOutcome::completed() : TaskOutcome::error(std::forward<T>(result));
    } else if constexpr (std::is_same_v<ResultType, Result<void>>) {
        return result ? TaskOutcome::completed() : TaskOutcome::error(result.message());
    } else {
        static_assert(std::is_same_v<ResultType, void>,
                      "a managed task must return void, TaskOutcome, TaskResult, bool, or string");
        return TaskOutcome::completed();
    }
}

template <typename F>
[[nodiscard]] inline TaskOutcome invoke_task(F&& task, std::stop_token token) {
    try {
        if constexpr (std::is_invocable_v<F&, std::stop_token>) {
            if constexpr (std::is_void_v<std::invoke_result_t<F&, std::stop_token>>) {
                std::invoke(std::forward<F>(task), token);
                return TaskOutcome::completed();
            } else {
                return normalize_task_result(std::invoke(std::forward<F>(task), token));
            }
        } else if constexpr (std::is_invocable_v<F&>) {
            if constexpr (std::is_void_v<std::invoke_result_t<F&>>) {
                std::invoke(std::forward<F>(task));
                return TaskOutcome::completed();
            } else {
                return normalize_task_result(std::invoke(std::forward<F>(task)));
            }
        } else {
            static_assert(std::is_invocable_v<F&, std::stop_token>,
                          "a managed task must be invocable with std::stop_token or no arguments");
            return TaskOutcome::exception("invalid task callable");
        }
    } catch (const std::exception& error) {
        return TaskOutcome::exception(error.what());
    } catch (...) {
        return TaskOutcome::exception("unknown exception");
    }
}

} // namespace rbt::detail

#endif // RBT_CPP_RUNTIME_HPP
