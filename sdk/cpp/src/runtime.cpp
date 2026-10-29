#include <rbt/runtime.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <limits>
#include <sstream>
#include <stdexcept>

namespace rbt {
namespace {

constexpr std::size_t kMaxJsonBytes = 1024U * 1024U;
constexpr std::size_t kMaxJsonDepth = 32U;

volatile sig_atomic_t runtime_stop_signal = 0;

void runtime_signal_handler(int) noexcept {
    runtime_stop_signal = 1;
}

class RuntimeSignalHandlers final {
  public:
    RuntimeSignalHandlers() noexcept {
        runtime_stop_signal = 0;

        struct sigaction action {};
        action.sa_handler = runtime_signal_handler;
        action.sa_flags = 0;
        if (::sigemptyset(&action.sa_mask) != 0) {
            return;
        }
        if (::sigaction(SIGINT, &action, &previous_sigint_) != 0) {
            return;
        }
        sigint_installed_ = true;
        if (::sigaction(SIGTERM, &action, &previous_sigterm_) != 0) {
            (void)::sigaction(SIGINT, &previous_sigint_, nullptr);
            sigint_installed_ = false;
            return;
        }
        sigterm_installed_ = true;
    }

    RuntimeSignalHandlers(const RuntimeSignalHandlers&) = delete;
    RuntimeSignalHandlers& operator=(const RuntimeSignalHandlers&) = delete;

    ~RuntimeSignalHandlers() noexcept {
        if (sigterm_installed_) {
            (void)::sigaction(SIGTERM, &previous_sigterm_, nullptr);
        }
        if (sigint_installed_) {
            (void)::sigaction(SIGINT, &previous_sigint_, nullptr);
        }
        runtime_stop_signal = 0;
    }

    [[nodiscard]] bool installed() const noexcept {
        return sigint_installed_ && sigterm_installed_;
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return runtime_stop_signal != 0;
    }

  private:
    struct sigaction previous_sigint_ {};
    struct sigaction previous_sigterm_ {};
    bool sigint_installed_ { false };
    bool sigterm_installed_ { false };
};

class JsonValue {
  public:
    enum class Kind : std::uint8_t {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    JsonValue()
        : value_(nullptr) {
    }

    explicit JsonValue(bool value)
        : value_(value) {
    }

    explicit JsonValue(std::string value, bool number)
        : value_(NumberOrString { std::move(value), number }) {
    }

    explicit JsonValue(const char* value)
        : value_(NumberOrString { std::string(value), false }) {
    }

    explicit JsonValue(std::string value)
        : value_(NumberOrString { std::move(value), false }) {
    }

    explicit JsonValue(Array value)
        : value_(std::move(value)) {
    }

    explicit JsonValue(Object value)
        : value_(std::move(value)) {
    }

    [[nodiscard]] Kind kind() const noexcept {
        if (std::holds_alternative<std::nullptr_t>(value_)) {
            return Kind::Null;
        }
        if (std::holds_alternative<bool>(value_)) {
            return Kind::Boolean;
        }
        if (std::holds_alternative<NumberOrString>(value_)) {
            return std::get<NumberOrString>(value_).number ? Kind::Number : Kind::String;
        }
        if (std::holds_alternative<Array>(value_)) {
            return Kind::Array;
        }
        return Kind::Object;
    }

    [[nodiscard]] bool is_null() const noexcept {
        return kind() == Kind::Null;
    }

    [[nodiscard]] bool is_string() const noexcept {
        return kind() == Kind::String;
    }

    [[nodiscard]] bool is_number() const noexcept {
        return kind() == Kind::Number;
    }

    [[nodiscard]] bool is_boolean() const noexcept {
        return kind() == Kind::Boolean;
    }

    [[nodiscard]] bool is_array() const noexcept {
        return kind() == Kind::Array;
    }

    [[nodiscard]] bool is_object() const noexcept {
        return kind() == Kind::Object;
    }

    [[nodiscard]] const std::string* string() const noexcept {
        if (!is_string()) {
            return nullptr;
        }
        return &std::get<NumberOrString>(value_).value;
    }

    [[nodiscard]] const std::string* number() const noexcept {
        if (!is_number()) {
            return nullptr;
        }
        return &std::get<NumberOrString>(value_).value;
    }

    [[nodiscard]] std::optional<bool> boolean() const noexcept {
        if (!is_boolean()) {
            return std::nullopt;
        }
        return std::get<bool>(value_);
    }

    [[nodiscard]] const Array* array() const noexcept {
        return std::get_if<Array>(&value_);
    }

    [[nodiscard]] const Object* object() const noexcept {
        return std::get_if<Object>(&value_);
    }

    [[nodiscard]] const JsonValue* get(std::string_view key) const noexcept {
        const auto* values = object();
        if (values == nullptr) {
            return nullptr;
        }
        auto it = values->find(std::string(key));
        return it == values->end() ? nullptr : &it->second;
    }

  private:
    struct NumberOrString {
        std::string value;
        bool number { false };
    };

    std::variant<std::nullptr_t, bool, NumberOrString, Array, Object> value_;
};

class JsonParser {
  public:
    explicit JsonParser(std::string_view input)
        : input_(input) {
    }

    [[nodiscard]] JsonValue parse() {
        if (input_.size() > kMaxJsonBytes) {
            fail("JSON input exceeds bounded codec limit");
        }
        skip_space();
        JsonValue result = parse_value(0U);
        skip_space();
        if (position_ != input_.size()) {
            fail("trailing bytes after JSON value");
        }
        return result;
    }

  private:
    [[noreturn]] void fail(std::string message) const {
        throw std::runtime_error(std::move(message) + " at byte " + std::to_string(position_));
    }

    void skip_space() {
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] JsonValue parse_value(std::size_t depth) {
        if (depth > kMaxJsonDepth) {
            fail("JSON nesting exceeds bounded codec limit");
        }
        skip_space();
        if (position_ == input_.size()) {
            fail("unexpected end of JSON input");
        }
        switch (input_[position_]) {
        case 'n':
            parse_literal("null");
            return JsonValue {};
        case 't':
            parse_literal("true");
            return JsonValue(true);
        case 'f':
            parse_literal("false");
            return JsonValue(false);
        case '"':
            return JsonValue(parse_string());
        case '[':
            return parse_array(depth);
        case '{':
            return parse_object(depth);
        default:
            if (input_[position_] == '-' || (input_[position_] >= '0' && input_[position_] <= '9')) {
                return JsonValue(parse_number(), true);
            }
            fail("unexpected JSON token");
        }
    }

    void parse_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            fail("invalid JSON literal");
        }
        position_ += literal.size();
    }

    [[nodiscard]] std::string parse_string() {
        if (input_[position_] != '"') {
            fail("expected JSON string");
        }
        ++position_;
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') {
                return result;
            }
            if (character < 0x20U) {
                fail("control character in JSON string");
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ == input_.size()) {
                fail("unterminated JSON escape");
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u':
                append_unicode_escape(result);
                break;
            default:
                fail("invalid JSON escape");
            }
        }
        fail("unterminated JSON string");
    }

    void append_unicode_escape(std::string& result) {
        auto parse_hex = [this]() -> std::uint32_t {
            if (position_ + 4U > input_.size()) {
                fail("short JSON unicode escape");
            }
            std::uint32_t value = 0U;
            for (std::size_t index = 0U; index < 4U; ++index) {
                const char character = input_[position_++];
                value <<= 4U;
                if (character >= '0' && character <= '9') {
                    value |= static_cast<std::uint32_t>(character - '0');
                } else if (character >= 'a' && character <= 'f') {
                    value |= static_cast<std::uint32_t>(character - 'a' + 10);
                } else if (character >= 'A' && character <= 'F') {
                    value |= static_cast<std::uint32_t>(character - 'A' + 10);
                } else {
                    fail("invalid JSON unicode escape");
                }
            }
            return value;
        };
        std::uint32_t codepoint = parse_hex();
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
            if (position_ + 6U > input_.size() || input_[position_] != '\\' || input_[position_ + 1U] != 'u') {
                fail("unpaired JSON high surrogate");
            }
            position_ += 2U;
            const std::uint32_t low = parse_hex();
            if (low < 0xDC00U || low > 0xDFFFU) {
                fail("invalid JSON low surrogate");
            }
            codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (low - 0xDC00U);
        } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
            fail("unpaired JSON low surrogate");
        }
        if (codepoint <= 0x7FU) {
            result.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            result.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            result.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0x10FFFFU) {
            result.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            result.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            fail("JSON unicode codepoint is out of range");
        }
    }

    [[nodiscard]] std::string parse_number() {
        const std::size_t start = position_;
        if (input_[position_] == '-') {
            ++position_;
        }
        if (position_ == input_.size()) {
            fail("incomplete JSON number");
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                fail("leading zero in JSON number");
            }
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') {
                fail("invalid JSON number");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fraction_start = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (fraction_start == position_) {
                fail("fraction has no digits");
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_start = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (exponent_start == position_) {
                fail("exponent has no digits");
            }
        }
        return std::string(input_.substr(start, position_ - start));
    }

    [[nodiscard]] JsonValue parse_array(std::size_t depth) {
        ++position_;
        JsonValue::Array values;
        skip_space();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return JsonValue(std::move(values));
        }
        while (true) {
            values.push_back(parse_value(depth + 1U));
            skip_space();
            if (position_ == input_.size()) {
                fail("unterminated JSON array");
            }
            if (input_[position_] == ']') {
                ++position_;
                return JsonValue(std::move(values));
            }
            if (input_[position_] != ',') {
                fail("expected comma in JSON array");
            }
            ++position_;
            skip_space();
            if (position_ < input_.size() && input_[position_] == ']') {
                fail("trailing comma in JSON array");
            }
        }
    }

    [[nodiscard]] JsonValue parse_object(std::size_t depth) {
        ++position_;
        JsonValue::Object values;
        skip_space();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return JsonValue(std::move(values));
        }
        while (true) {
            skip_space();
            if (position_ == input_.size() || input_[position_] != '"') {
                fail("expected JSON object key");
            }
            std::string key = parse_string();
            skip_space();
            if (position_ == input_.size() || input_[position_] != ':') {
                fail("expected colon after JSON object key");
            }
            ++position_;
            JsonValue value = parse_value(depth + 1U);
            if (!values.emplace(std::move(key), std::move(value)).second) {
                fail("duplicate JSON object key");
            }
            skip_space();
            if (position_ == input_.size()) {
                fail("unterminated JSON object");
            }
            if (input_[position_] == '}') {
                ++position_;
                return JsonValue(std::move(values));
            }
            if (input_[position_] != ',') {
                fail("expected comma in JSON object");
            }
            ++position_;
            skip_space();
            if (position_ < input_.size() && input_[position_] == '}') {
                fail("trailing comma in JSON object");
            }
        }
    }

    std::string_view input_;
    std::size_t position_ { 0U };
};

[[nodiscard]] std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (character < 0x20U) {
                constexpr char digits[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(digits[(character >> 4U) & 0x0FU]);
                result.push_back(digits[character & 0x0FU]);
            } else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] const JsonValue& require_value(const JsonValue& object,
                                              std::string_view key,
                                              std::string* error) {
    (void)error;
    const auto* value = object.get(key);
    if (value == nullptr) {
        throw std::runtime_error("missing JSON field " + std::string(key));
    }
    return *value;
}

[[nodiscard]] std::string require_string(const JsonValue& object,
                                         std::string_view key,
                                         std::string* /*error*/ = nullptr) {
    const auto& value = require_value(object, key, nullptr);
    const auto* string = value.string();
    if (string == nullptr) {
        throw std::runtime_error("JSON field " + std::string(key) + " must be a string");
    }
    return *string;
}

[[nodiscard]] std::uint64_t require_u64(const JsonValue& object, std::string_view key) {
    const auto& value = require_value(object, key, nullptr);
    const auto* number = value.number();
    if (number == nullptr || number->empty() || number->front() == '-') {
        throw std::runtime_error("JSON field " + std::string(key) + " must be an unsigned integer");
    }
    std::uint64_t result = 0U;
    const auto conversion = std::from_chars(number->data(), number->data() + number->size(), result);
    if (conversion.ec != std::errc {} || conversion.ptr != number->data() + number->size()) {
        throw std::runtime_error("JSON field " + std::string(key) + " is outside u64 range");
    }
    return result;
}

[[nodiscard]] std::uint16_t require_u16(const JsonValue& object, std::string_view key) {
    const auto value = require_u64(object, key);
    if (value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("JSON field " + std::string(key) + " is outside u16 range");
    }
    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] bool require_bool(const JsonValue& object, std::string_view key) {
    const auto value = require_value(object, key, nullptr).boolean();
    if (!value.has_value()) {
        throw std::runtime_error("JSON field " + std::string(key) + " must be a boolean");
    }
    return *value;
}

[[nodiscard]] std::optional<std::uint64_t> optional_u64(const JsonValue& object,
                                                        std::string_view key) {
    const auto* value = object.get(key);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    return require_u64(object, key);
}

[[nodiscard]] std::optional<std::uint16_t> optional_u16(const JsonValue& object, std::string_view key) {
    const auto* value = object.get(key);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    const auto number = require_u64(object, key);
    if (number > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("JSON field " + std::string(key) + " is outside u16 range");
    }
    return static_cast<std::uint16_t>(number);
}

[[nodiscard]] std::string state_json(const ComponentState& state) {
    switch (state.kind) {
    case ComponentStateKind::Declared:
        return "\"Declared\"";
    case ComponentStateKind::Preparing:
        return "\"Preparing\"";
    case ComponentStateKind::Blocked:
        return "{\"Blocked\":" + json_escape(state.reason) + "}";
    case ComponentStateKind::Activating:
        return "\"Activating\"";
    case ComponentStateKind::Active:
        return "\"Active\"";
    case ComponentStateKind::Quiescing:
        return "\"Quiescing\"";
    case ComponentStateKind::Failed:
        return "{\"Failed\":" + json_escape(state.reason) + "}";
    }
    return "\"Preparing\"";
}

[[nodiscard]] ComponentState parse_state(const JsonValue& value) {
    if (const auto* name = value.string(); name != nullptr) {
        if (*name == "Declared") {
            return ComponentState::declared();
        }
        if (*name == "Preparing") {
            return ComponentState::preparing();
        }
        if (*name == "Activating") {
            return ComponentState::activating();
        }
        if (*name == "Active") {
            return ComponentState::active();
        }
        if (*name == "Quiescing") {
            return ComponentState::quiescing();
        }
        throw std::runtime_error("unknown ComponentState " + *name);
    }
    const auto* object = value.object();
    if (object == nullptr || object->size() != 1U) {
        throw std::runtime_error("invalid externally tagged ComponentState");
    }
    const auto& entry = *object->begin();
    const auto* reason = entry.second.string();
    if (reason == nullptr) {
        throw std::runtime_error("ComponentState reason must be a string");
    }
    if (entry.first == "Blocked") {
        return ComponentState::blocked(*reason);
    }
    if (entry.first == "Failed") {
        return ComponentState::failed(*reason);
    }
    throw std::runtime_error("unknown externally tagged ComponentState " + entry.first);
}

[[nodiscard]] std::string descriptor_json(const OwnedMessageTypeDescriptor& descriptor) {
    std::string result = "{\"namespace\":" + json_escape(descriptor.namespace_name)
        + ",\"name\":" + json_escape(descriptor.name) + ",\"abi_major\":";
    result += std::to_string(descriptor.abi_major);
    result += ",\"shape\":";
    result += descriptor.shape == MessageShape::Fixed ? "\"fixed\"" : "\"slice\"";
    result += ",\"header_size\":" + std::to_string(descriptor.header_size);
    result += ",\"header_align\":" + std::to_string(descriptor.header_align);
    result += ",\"element_size\":" + std::to_string(descriptor.element_size);
    result += ",\"element_align\":" + std::to_string(descriptor.element_align);
    result += ",\"layout_hash\":" + json_escape(descriptor.layout_hash) + "}";
    return result;
}

[[nodiscard]] std::string stream_json(const OwnedStreamDescriptor& stream) {
    return "{\"service_name\":" + json_escape(stream.service_name) + ",\"message_type\":"
        + descriptor_json(stream.message_type) + ",\"profile\":"
        + (stream.profile == StreamProfile::Sensor ? "\"sensor\"" : "\"control\"")
        + ",\"transport\":{\"readiness\":" + json_escape(to_string(stream.transport.readiness))
        + "},\"resources\":{\"max_loans\":" + std::to_string(stream.resources.max_loans)
        + ",\"max_borrowed\":" + std::to_string(stream.resources.max_borrowed) + "}}";
}

[[nodiscard]] std::string rpc_resources_json(const RpcResourceConfig& resources);

[[nodiscard]] std::string rpc_descriptor_json(const OwnedRpcDescriptor& rpc) {
    return "{\"service_name\":" + json_escape(rpc.service_name) + ",\"request_type\":"
        + descriptor_json(rpc.request_type) + ",\"response_type\":"
        + descriptor_json(rpc.response_type) + ",\"transport\":{\"readiness\":"
        + json_escape(to_string(rpc.transport.readiness)) + "},\"resources\":"
        + rpc_resources_json(rpc.resources) + "}";
}

[[nodiscard]] std::string capability_descriptor_json(const CapabilityDescriptor& descriptor) {
    std::string result = "{\"capability_instance\":" + json_escape(descriptor.capability_instance)
        + ",\"provider_component\":" + json_escape(descriptor.provider_component)
        + ",\"provider_process\":" + json_escape(descriptor.provider_process)
        + ",\"activation\":" + std::to_string(descriptor.activation);
    if (descriptor.rpc.has_value()) {
        result += ",\"rpc\":" + rpc_descriptor_json(*descriptor.rpc);
    } else {
        result += ",\"stream\":" + stream_json(descriptor.stream);
    }
    result.push_back('}');
    return result;
}

[[nodiscard]] std::string resolved_binding_json(const ResolvedBinding& binding) {
    return "{\"requirement_name\":" + json_escape(binding.requirement_name)
        + ",\"capability_type\":" + json_escape(binding.capability_type)
        + ",\"provider\":" + json_escape(binding.provider)
        + ",\"descriptor\":" + capability_descriptor_json(binding.descriptor) + "}";
}

[[nodiscard]] std::string requirement_json(const RequirementSnapshot& requirement) {
    std::string result = "{\"requirement\":{\"requirement_name\":"
        + json_escape(requirement.requirement.requirement_name) + ",\"capability_type\":"
        + json_escape(requirement.requirement.capability_type) + "},\"binding\":";
    result += requirement.binding.has_value() ? resolved_binding_json(*requirement.binding) : "null";
    result += "}";
    return result;
}

[[nodiscard]] std::string activation_json(const ActivationSnapshot& activation) {
    std::string result = "{\"activation_id\":" + std::to_string(activation.activation_id) + ",\"bindings\":[";
    for (std::size_t index = 0U; index < activation.bindings.size(); ++index) {
        if (index != 0U) {
            result.push_back(',');
        }
        result += resolved_binding_json(activation.bindings[index]);
    }
    result += "]}";
    return result;
}

[[nodiscard]] std::string optional_u64_json(const std::optional<std::uint64_t>& value) {
    return value.has_value() ? std::to_string(*value) : "null";
}

[[nodiscard]] std::string pressure_event_json(
    PressureEvent event,
    const std::optional<std::string>& subscriber) {
    switch (event) {
    case PressureEvent::QueueFull:
        return "{\"kind\":\"queue_full\",\"subscriber\":"
            + (subscriber.has_value() ? json_escape(*subscriber) : "null") + "}";
    case PressureEvent::UnableToDeliver:
        return "{\"kind\":\"unable_to_deliver\"}";
    case PressureEvent::LoanPoolExhausted:
        return "{\"kind\":\"loan_pool_exhausted\"}";
    case PressureEvent::BorrowLimitReached:
        return "{\"kind\":\"borrow_limit_reached\"}";
    }
    return "{\"kind\":\"unable_to_deliver\"}";
}

[[nodiscard]] std::string stream_pressure_json(const StreamPressure& pressure) {
    std::string result = "{\"service_name\":" + json_escape(pressure.service_name)
        + ",\"profile\":"
        + (pressure.profile == StreamProfile::Sensor ? "\"sensor\"" : "\"control\"")
        + ",\"semantic\":{\"queue_depth\":" + std::to_string(pressure.semantic.queue_depth)
        + ",\"history_size\":" + std::to_string(pressure.semantic.history_size)
        + ",\"safe_overflow\":" + (pressure.semantic.safe_overflow ? "true" : "false")
        + "},\"resources\":{\"max_loans\":" + std::to_string(pressure.resources.max_loans)
        + ",\"max_borrowed\":" + std::to_string(pressure.resources.max_borrowed)
        + "},\"publisher\":";
    if (pressure.publisher.has_value()) {
        const auto& publisher = *pressure.publisher;
        result += "{\"loan_attempts\":" + std::to_string(publisher.loan_attempts)
            + ",\"loan_successes\":" + std::to_string(publisher.loan_successes)
            + ",\"loans_in_use\":" + std::to_string(publisher.loans_in_use)
            + ",\"loans_high_water\":" + std::to_string(publisher.loans_high_water)
            + ",\"loan_failures\":" + std::to_string(publisher.loan_failures)
            + ",\"payload_bytes_last\":" + std::to_string(publisher.payload_bytes_last)
            + ",\"payload_bytes_max_seen\":" + std::to_string(publisher.payload_bytes_max_seen)
            + ",\"total_loaned\":" + std::to_string(publisher.total_loaned)
            + ",\"send_attempts\":" + std::to_string(publisher.send_attempts)
            + ",\"send_successes\":" + std::to_string(publisher.send_successes)
            + ",\"send_failures\":" + std::to_string(publisher.send_failures)
            + ",\"queue_full\":" + std::to_string(publisher.queue_full)
            + ",\"unable_to_deliver\":" + std::to_string(publisher.unable_to_deliver)
            + ",\"readiness_notify_failures\":" + std::to_string(publisher.readiness_notify_failures)
            + ",\"subscriber_count\":" + std::to_string(publisher.subscriber_count)
            + ",\"estimated_dropped_samples\":"
            + optional_u64_json(publisher.estimated_dropped_samples) + "}";
    } else {
        result += "null";
    }
    result += ",\"subscriber\":";
    if (pressure.subscriber.has_value()) {
        const auto& subscriber = *pressure.subscriber;
        result += "{\"receive_attempts\":" + std::to_string(subscriber.receive_attempts)
            + ",\"received_samples\":" + std::to_string(subscriber.received_samples)
            + ",\"borrowed_samples\":" + std::to_string(subscriber.borrowed_samples)
            + ",\"borrowed_high_water\":" + std::to_string(subscriber.borrowed_high_water)
            + ",\"borrow_failures\":" + std::to_string(subscriber.borrow_failures)
            + ",\"borrow_duration_count\":" + std::to_string(subscriber.borrow_duration_count)
            + ",\"borrow_duration_total_ns\":" + std::to_string(subscriber.borrow_duration_total_ns)
            + ",\"borrow_duration_max_ns\":" + std::to_string(subscriber.borrow_duration_max_ns)
            + ",\"oldest_borrow_age_ns\":" + optional_u64_json(subscriber.oldest_borrow_age_ns)
            + ",\"estimated_dropped_samples\":"
            + optional_u64_json(subscriber.estimated_dropped_samples) + "}";
    } else {
        result += "null";
    }
    result += ",\"last_pressure_event\":";
    result += pressure.last_pressure_event.has_value()
        ? pressure_event_json(*pressure.last_pressure_event, pressure.last_pressure_subscriber)
        : "null";
    result += ",\"snapshot_timestamp_unix_ms\":" + std::to_string(pressure.snapshot_timestamp_unix_ms);
    result += "}";
    return result;
}

[[nodiscard]] std::string rpc_resources_json(const RpcResourceConfig& resources) {
    return "{\"max_in_flight\":" + std::to_string(resources.max_in_flight)
        + ",\"max_clients\":" + std::to_string(resources.max_clients)
        + ",\"max_request_loans\":" + std::to_string(resources.max_request_loans)
        + ",\"response_buffer_size\":" + std::to_string(resources.response_buffer_size)
        + ",\"max_borrowed_responses\":" + std::to_string(resources.max_borrowed_responses)
        + ",\"max_response_loans\":" + std::to_string(resources.max_response_loans)
        + ",\"max_request_slice_len\":" + std::to_string(resources.max_request_slice_len)
        + ",\"max_response_slice_len\":" + std::to_string(resources.max_response_slice_len)
        + "}";
}

[[nodiscard]] std::string rpc_client_metrics_json(const RpcClientMetrics& metrics) {
    return "{\"calls_attempted\":" + std::to_string(metrics.calls_attempted)
        + ",\"calls_started\":" + std::to_string(metrics.calls_started)
        + ",\"calls_completed\":" + std::to_string(metrics.calls_completed)
        + ",\"calls_failed\":" + std::to_string(metrics.calls_failed)
        + ",\"calls_timed_out\":" + std::to_string(metrics.calls_timed_out)
        + ",\"calls_cancelled\":" + std::to_string(metrics.calls_cancelled)
        + ",\"calls_activation_expired\":"
        + std::to_string(metrics.calls_activation_expired)
        + ",\"in_flight_current\":" + std::to_string(metrics.in_flight_current)
        + ",\"in_flight_high_water\":" + std::to_string(metrics.in_flight_high_water)
        + ",\"response_guards_current\":"
        + std::to_string(metrics.response_guards_current)
        + ",\"response_guards_high_water\":"
        + std::to_string(metrics.response_guards_high_water)
        + ",\"capacity_rejections\":" + std::to_string(metrics.capacity_rejections)
        + ",\"server_capacity_rejections\":"
        + std::to_string(metrics.server_capacity_rejections)
        + ",\"server_unavailable\":" + std::to_string(metrics.server_unavailable)
        + ",\"request_loan_failures\":" + std::to_string(metrics.request_loan_failures)
        + ",\"response_receive_failures\":"
        + std::to_string(metrics.response_receive_failures)
        + ",\"call_duration_count\":" + std::to_string(metrics.call_duration_count)
        + ",\"call_duration_total_ns\":"
        + std::to_string(metrics.call_duration_total_ns)
        + ",\"call_duration_max_ns\":" + std::to_string(metrics.call_duration_max_ns)
        + "}";
}

[[nodiscard]] std::string rpc_server_metrics_json(const RpcServerMetrics& metrics) {
    return "{\"requests_received\":" + std::to_string(metrics.requests_received)
        + ",\"requests_current\":" + std::to_string(metrics.requests_current)
        + ",\"requests_high_water\":" + std::to_string(metrics.requests_high_water)
        + ",\"responses_attempted\":" + std::to_string(metrics.responses_attempted)
        + ",\"responses_sent\":" + std::to_string(metrics.responses_sent)
        + ",\"responses_failed\":" + std::to_string(metrics.responses_failed)
        + ",\"responses_abandoned\":" + std::to_string(metrics.responses_abandoned)
        + ",\"request_borrow_failures\":"
        + std::to_string(metrics.request_borrow_failures)
        + ",\"response_loan_failures\":"
        + std::to_string(metrics.response_loan_failures) + "}";
}

[[nodiscard]] std::string rpc_pressure_json(const RpcPressure& pressure) {
    std::string result = "{\"service_name\":" + json_escape(pressure.service_name)
        + ",\"capability_instance\":"
        + (pressure.capability_instance.has_value()
               ? json_escape(*pressure.capability_instance)
               : "null")
        + ",\"resources\":" + rpc_resources_json(pressure.resources)
        + ",\"client_activation\":";
    result += optional_u64_json(pressure.client_activation);
    result += ",\"client\":";
    result += pressure.client.has_value() ? rpc_client_metrics_json(*pressure.client) : "null";
    result += ",\"server\":";
    result += pressure.server.has_value() ? rpc_server_metrics_json(*pressure.server) : "null";
    result += ",\"snapshot_timestamp_unix_ms\":"
        + std::to_string(pressure.snapshot_timestamp_unix_ms) + "}";
    return result;
}

[[nodiscard]] std::string snapshot_json_impl(const ComponentSnapshot& snapshot) {
    std::string result = "{\"snapshot_version\":" + std::to_string(snapshot.snapshot_version)
        + ",\"component_id\":" + json_escape(snapshot.component_id)
        + ",\"process_instance\":" + json_escape(snapshot.process_instance)
        + ",\"state\":" + state_json(snapshot.state) + ",\"activation\":";
    if (snapshot.activation.has_value()) {
        result += activation_json(*snapshot.activation);
    } else {
        result += "null";
    }
    result += ",\"requirements\":[";
    for (std::size_t index = 0U; index < snapshot.requirements.size(); ++index) {
        if (index != 0U) {
            result.push_back(',');
        }
        result += requirement_json(snapshot.requirements[index]);
    }
    result += "],\"provided_capabilities\":[";
    for (std::size_t index = 0U; index < snapshot.provided_capabilities.size(); ++index) {
        if (index != 0U) {
            result.push_back(',');
        }
        const auto& capability = snapshot.provided_capabilities[index];
        result += "{\"capability_type\":" + json_escape(capability.capability_type)
            + ",\"capability_instance\":" + json_escape(capability.capability_instance)
            + ",\"descriptor\":" + capability_descriptor_json(capability.descriptor) + "}";
    }
    result += "],\"stream_pressures\":[";
    for (std::size_t index = 0U; index < snapshot.stream_pressures.size(); ++index) {
        if (index != 0U) {
            result.push_back(',');
        }
        result += stream_pressure_json(snapshot.stream_pressures[index]);
    }
    result += "],\"rpc_pressures\":[";
    for (std::size_t index = 0U; index < snapshot.rpc_pressures.size(); ++index) {
        if (index != 0U) {
            result.push_back(',');
        }
        result += rpc_pressure_json(snapshot.rpc_pressures[index]);
    }
    result += "]}";
    return result;
}

[[nodiscard]] OwnedMessageTypeDescriptor parse_message_descriptor(const JsonValue& value) {
    OwnedMessageTypeDescriptor result;
    result.namespace_name = require_string(value, "namespace");
    result.name = require_string(value, "name");
    result.abi_major = require_u16(value, "abi_major");
    const auto shape = require_string(value, "shape");
    if (shape == "fixed") {
        result.shape = MessageShape::Fixed;
    } else if (shape == "slice") {
        result.shape = MessageShape::Slice;
    } else {
        throw std::runtime_error("unknown message shape " + shape);
    }
    const auto header_size = require_u64(value, "header_size");
    const auto header_align = require_u64(value, "header_align");
    const auto element_size = require_u64(value, "element_size");
    const auto element_align = require_u64(value, "element_align");
    if (header_size > std::numeric_limits<std::uint32_t>::max()
        || header_align > std::numeric_limits<std::uint32_t>::max()
        || element_size > std::numeric_limits<std::uint32_t>::max()
        || element_align > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("message descriptor size does not fit u32");
    }
    result.header_size = static_cast<std::uint32_t>(header_size);
    result.header_align = static_cast<std::uint32_t>(header_align);
    result.element_size = static_cast<std::uint32_t>(element_size);
    result.element_align = static_cast<std::uint32_t>(element_align);
    result.layout_hash = require_string(value, "layout_hash");
    if (result.layout_hash.size() != 32U) {
        throw std::runtime_error("layout_hash must contain 32 hexadecimal characters");
    }
    return result;
}

[[nodiscard]] OwnedRpcDescriptor parse_rpc_descriptor(const JsonValue& value);

[[nodiscard]] OwnedStreamDescriptor parse_stream_descriptor(const JsonValue& value) {
    OwnedStreamDescriptor result;
    result.service_name = require_string(value, "service_name");
    result.message_type = parse_message_descriptor(require_value(value, "message_type", nullptr));
    const auto profile = require_string(value, "profile");
    if (profile == "sensor") {
        result.profile = StreamProfile::Sensor;
    } else if (profile == "control") {
        result.profile = StreamProfile::Control;
    } else {
        throw std::runtime_error("unknown stream profile " + profile);
    }
    // An absent transport object is a descriptor produced before v0.2. It is
    // deliberately preserved as `None` so event-driven endpoint preflight can
    // reject it before opening the payload transport.
    if (const auto* transport = value.get("transport"); transport != nullptr) {
        const auto readiness = require_string(*transport, "readiness");
        if (readiness == "none") {
            result.transport.readiness = ReadinessProtocol::None;
        } else if (readiness == "sidecar-v1") {
            result.transport.readiness = ReadinessProtocol::SidecarV1;
        } else {
            throw std::runtime_error("unknown readiness protocol " + readiness);
        }
    }
    if (const auto* resources = value.get("resources"); resources != nullptr) {
        result.resources.max_loans = require_u64(*resources, "max_loans");
        result.resources.max_borrowed = require_u64(*resources, "max_borrowed");
    }
    return result;
}

[[nodiscard]] PressureEvent parse_pressure_event(const JsonValue& value) {
    const auto kind = require_string(value, "kind");
    if (kind == "queue_full") {
        return PressureEvent::QueueFull;
    }
    if (kind == "unable_to_deliver") {
        return PressureEvent::UnableToDeliver;
    }
    if (kind == "loan_pool_exhausted") {
        return PressureEvent::LoanPoolExhausted;
    }
    if (kind == "borrow_limit_reached") {
        return PressureEvent::BorrowLimitReached;
    }
    throw std::runtime_error("unknown pressure event " + kind);
}

[[nodiscard]] StreamPressure parse_stream_pressure(const JsonValue& value) {
    StreamPressure result;
    result.service_name = require_string(value, "service_name");
    const auto profile = require_string(value, "profile");
    if (profile == "sensor") {
        result.profile = StreamProfile::Sensor;
    } else if (profile == "control") {
        result.profile = StreamProfile::Control;
    } else {
        throw std::runtime_error("unknown pressure stream profile " + profile);
    }
    const auto* semantic = value.get("semantic");
    if (semantic == nullptr) {
        // Read pre-v0.4 pressure snapshots as a compatibility input. The
        // encoder never emits this shape.
        semantic = value.get("config");
    }
    if (semantic == nullptr) {
        throw std::runtime_error("stream pressure semantic is missing");
    }
    if (value.get("semantic") != nullptr) {
        result.semantic.queue_depth = require_u64(*semantic, "queue_depth");
        result.semantic.history_size = require_u64(*semantic, "history_size");
        result.semantic.safe_overflow = require_bool(*semantic, "safe_overflow");
    } else {
        result.semantic.queue_depth = require_u64(*semantic, "subscriber_buffer_size");
        result.semantic.history_size = require_u64(*semantic, "history_size");
        result.semantic.safe_overflow = require_bool(*semantic, "safe_overflow");
    }
    if (const auto* resources = value.get("resources"); resources != nullptr) {
        result.resources.max_loans = require_u64(*resources, "max_loans");
        result.resources.max_borrowed = require_u64(*resources, "max_borrowed");
    } else if (value.get("config") != nullptr) {
        const auto& config = *value.get("config");
        result.resources.max_borrowed = require_u64(config, "max_borrowed_samples");
        result.resources.max_loans = require_u64(config, "max_loaned_samples");
    }
    const auto& publisher = require_value(value, "publisher", nullptr);
    if (!publisher.is_null()) {
        PublisherPressure parsed;
        parsed.loan_attempts = require_u64(publisher, "loan_attempts");
        parsed.loan_successes = require_u64(publisher, "loan_successes");
        parsed.loans_in_use = require_u64(publisher, "loans_in_use");
        parsed.loans_high_water = require_u64(publisher, "loans_high_water");
        parsed.loan_failures = require_u64(publisher, "loan_failures");
        parsed.payload_bytes_last = require_u64(publisher, "payload_bytes_last");
        parsed.payload_bytes_max_seen = require_u64(publisher, "payload_bytes_max_seen");
        parsed.total_loaned = require_u64(publisher, "total_loaned");
        parsed.send_attempts = require_u64(publisher, "send_attempts");
        parsed.send_successes = require_u64(publisher, "send_successes");
        parsed.send_failures = require_u64(publisher, "send_failures");
        parsed.queue_full = require_u64(publisher, "queue_full");
        parsed.unable_to_deliver = require_u64(publisher, "unable_to_deliver");
        parsed.readiness_notify_failures = require_u64(publisher, "readiness_notify_failures");
        parsed.subscriber_count = require_u64(publisher, "subscriber_count");
        parsed.estimated_dropped_samples = optional_u64(publisher, "estimated_dropped_samples");
        result.publisher = parsed;
    }

    const auto& subscriber = require_value(value, "subscriber", nullptr);
    if (!subscriber.is_null()) {
        SubscriberPressure parsed;
        parsed.receive_attempts = require_u64(subscriber, "receive_attempts");
        parsed.received_samples = require_u64(subscriber, "received_samples");
        parsed.borrowed_samples = require_u64(subscriber, "borrowed_samples");
        parsed.borrowed_high_water = require_u64(subscriber, "borrowed_high_water");
        parsed.borrow_failures = require_u64(subscriber, "borrow_failures");
        parsed.borrow_duration_count = require_u64(subscriber, "borrow_duration_count");
        parsed.borrow_duration_total_ns = require_u64(subscriber, "borrow_duration_total_ns");
        parsed.borrow_duration_max_ns = require_u64(subscriber, "borrow_duration_max_ns");
        parsed.oldest_borrow_age_ns = optional_u64(subscriber, "oldest_borrow_age_ns");
        parsed.estimated_dropped_samples = optional_u64(subscriber, "estimated_dropped_samples");
        result.subscriber = parsed;
    }

    const auto& event = require_value(value, "last_pressure_event", nullptr);
    if (!event.is_null()) {
        result.last_pressure_event = parse_pressure_event(event);
        if (result.last_pressure_event == PressureEvent::QueueFull) {
            const auto* subscriber = event.get("subscriber");
            if (subscriber != nullptr && !subscriber->is_null()) {
                result.last_pressure_subscriber = require_string(event, "subscriber");
            }
        }
    }
    if (const auto* timestamp = value.get("snapshot_timestamp_unix_ms"); timestamp != nullptr) {
        result.snapshot_timestamp_unix_ms = require_u64(value, "snapshot_timestamp_unix_ms");
    }
    return result;
}

[[nodiscard]] CapabilityDescriptor parse_capability_descriptor(const JsonValue& value) {
    CapabilityDescriptor result;
    result.capability_instance = require_string(value, "capability_instance");
    result.provider_component = require_string(value, "provider_component");
    result.provider_process = require_string(value, "provider_process");
    result.activation = require_u64(value, "activation");
    const auto* stream = value.get("stream");
    const auto* rpc = value.get("rpc");
    const bool has_stream = stream != nullptr && !stream->is_null();
    const bool has_rpc = rpc != nullptr && !rpc->is_null();
    if (has_stream == has_rpc) {
        throw std::runtime_error("capability descriptor must contain exactly one of stream or rpc");
    }
    if (has_stream) {
        result.stream = parse_stream_descriptor(*stream);
    } else {
        result.rpc = parse_rpc_descriptor(*rpc);
    }
    return result;
}

[[nodiscard]] ResolvedBinding parse_binding(const JsonValue& value) {
    ResolvedBinding result;
    result.requirement_name = require_string(value, "requirement_name");
    result.capability_type = require_string(value, "capability_type");
    result.provider = require_string(value, "provider");
    result.descriptor = parse_capability_descriptor(require_value(value, "descriptor", nullptr));
    return result;
}

[[nodiscard]] RpcPressure parse_rpc_pressure(const JsonValue& value);

[[nodiscard]] ComponentSnapshot parse_snapshot_value(const JsonValue& value) {
    ComponentSnapshot result;
    result.snapshot_version = require_u16(value, "snapshot_version");
    result.component_id = require_string(value, "component_id");
    result.process_instance = require_string(value, "process_instance");
    result.state = parse_state(require_value(value, "state", nullptr));

    const auto& activation_value = require_value(value, "activation", nullptr);
    if (!activation_value.is_null()) {
        ActivationSnapshot activation;
        activation.activation_id = require_u64(activation_value, "activation_id");
        const auto* bindings = require_value(activation_value, "bindings", nullptr).array();
        if (bindings == nullptr) {
            throw std::runtime_error("activation.bindings must be an array");
        }
        activation.bindings.reserve(bindings->size());
        for (const auto& binding : *bindings) {
            activation.bindings.push_back(parse_binding(binding));
        }
        result.activation = std::move(activation);
    }

    const auto* requirements = require_value(value, "requirements", nullptr).array();
    if (requirements == nullptr) {
        throw std::runtime_error("requirements must be an array");
    }
    result.requirements.reserve(requirements->size());
    for (const auto& requirement_value : *requirements) {
        RequirementSnapshot requirement;
        const auto& specification = require_value(requirement_value, "requirement", nullptr);
        requirement.requirement.requirement_name = require_string(specification, "requirement_name");
        requirement.requirement.capability_type = require_string(specification, "capability_type");
        const auto& binding = require_value(requirement_value, "binding", nullptr);
        if (!binding.is_null()) {
            requirement.binding = parse_binding(binding);
        }
        result.requirements.push_back(std::move(requirement));
    }

    const auto* capabilities = require_value(value, "provided_capabilities", nullptr).array();
    if (capabilities == nullptr) {
        throw std::runtime_error("provided_capabilities must be an array");
    }
    result.provided_capabilities.reserve(capabilities->size());
    for (const auto& capability_value : *capabilities) {
        CapabilitySnapshot capability;
        capability.capability_type = require_string(capability_value, "capability_type");
        capability.capability_instance = require_string(capability_value, "capability_instance");
        capability.descriptor = parse_capability_descriptor(require_value(capability_value, "descriptor", nullptr));
        result.provided_capabilities.push_back(std::move(capability));
    }
    const auto* pressures = require_value(value, "stream_pressures", nullptr).array();
    if (pressures == nullptr) {
        throw std::runtime_error("stream_pressures must be an array");
    }
    result.stream_pressures.reserve(pressures->size());
    for (const auto& pressure : *pressures) {
        result.stream_pressures.push_back(parse_stream_pressure(pressure));
    }
    if (const auto* rpc_pressures = value.get("rpc_pressures"); rpc_pressures != nullptr) {
        const auto* values = rpc_pressures->array();
        if (values == nullptr) {
            throw std::runtime_error("rpc_pressures must be an array");
        }
        result.rpc_pressures.reserve(values->size());
        for (const auto& pressure : *values) {
            result.rpc_pressures.push_back(parse_rpc_pressure(pressure));
        }
    }
    return result;
}

[[nodiscard]] std::optional<std::string> optional_string(const JsonValue& object,
                                                          std::string_view key) {
    const auto* value = object.get(key);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    return require_string(object, key);
}

[[nodiscard]] std::size_t require_size(const JsonValue& object, std::string_view key) {
    const auto value = require_u64(object, key);
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(std::string(key) + " does not fit size_t");
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] RpcResourceConfig parse_rpc_resources(const JsonValue& value) {
    RpcResourceConfig result;
    if (const auto* field = value.get("max_in_flight"); field != nullptr) {
        result.max_in_flight = require_size(value, "max_in_flight");
    }
    if (const auto* field = value.get("max_clients"); field != nullptr) {
        result.max_clients = require_size(value, "max_clients");
    }
    if (const auto* field = value.get("max_request_loans"); field != nullptr) {
        result.max_request_loans = require_size(value, "max_request_loans");
    }
    if (const auto* field = value.get("response_buffer_size"); field != nullptr) {
        result.response_buffer_size = require_size(value, "response_buffer_size");
    }
    if (const auto* field = value.get("max_borrowed_responses"); field != nullptr) {
        result.max_borrowed_responses = require_size(value, "max_borrowed_responses");
    }
    if (const auto* field = value.get("max_response_loans"); field != nullptr) {
        result.max_response_loans = require_size(value, "max_response_loans");
    }
    if (const auto* field = value.get("max_request_slice_len"); field != nullptr) {
        result.max_request_slice_len = require_size(value, "max_request_slice_len");
    }
    if (const auto* field = value.get("max_response_slice_len"); field != nullptr) {
        result.max_response_slice_len = require_size(value, "max_response_slice_len");
    }
    return result;
}

[[nodiscard]] OwnedRpcDescriptor parse_rpc_descriptor(const JsonValue& value) {
    OwnedRpcDescriptor result;
    result.service_name = require_string(value, "service_name");

    const auto* request_type = value.get("request_type");
    const auto* request_alias = value.get("request");
    if (request_type != nullptr && request_alias != nullptr) {
        throw std::runtime_error("RPC descriptor contains both request_type and request");
    }
    const auto* request = request_type != nullptr ? request_type : request_alias;
    if (request == nullptr || request->is_null()) {
        throw std::runtime_error("RPC descriptor request_type is missing");
    }
    result.request_type = parse_message_descriptor(*request);

    const auto* response_type = value.get("response_type");
    const auto* response_alias = value.get("response");
    if (response_type != nullptr && response_alias != nullptr) {
        throw std::runtime_error("RPC descriptor contains both response_type and response");
    }
    const auto* response = response_type != nullptr ? response_type : response_alias;
    if (response == nullptr || response->is_null()) {
        throw std::runtime_error("RPC descriptor response_type is missing");
    }
    result.response_type = parse_message_descriptor(*response);

    // Rust's deserializer defaults absent transport features to readiness None.
    result.transport.readiness = RpcReadinessProtocol::None;
    if (const auto* transport = value.get("transport"); transport != nullptr) {
        if (!transport->is_object()) {
            throw std::runtime_error("RPC descriptor transport must be an object");
        }
        const auto* readiness_field = transport->get("readiness");
        if (readiness_field == nullptr) {
            result.transport.readiness = RpcReadinessProtocol::None;
        } else {
            const auto readiness = require_string(*transport, "readiness");
            if (readiness == "none") {
                result.transport.readiness = RpcReadinessProtocol::None;
            } else if (readiness == "rpc-sidecar-v1") {
                result.transport.readiness = RpcReadinessProtocol::RpcSidecarV1;
            } else {
                throw std::runtime_error("unknown RPC readiness protocol " + readiness);
            }
        }
    }
    if (const auto* resources = value.get("resources"); resources != nullptr) {
        if (!resources->is_object()) {
            throw std::runtime_error("RPC descriptor resources must be an object");
        }
        result.resources = parse_rpc_resources(*resources);
    }
    return result;
}

[[nodiscard]] RpcClientMetrics parse_rpc_client_metrics(const JsonValue& value) {
    RpcClientMetrics result;
    result.calls_attempted = require_u64(value, "calls_attempted");
    result.calls_started = require_u64(value, "calls_started");
    result.calls_completed = require_u64(value, "calls_completed");
    result.calls_failed = require_u64(value, "calls_failed");
    result.calls_timed_out = require_u64(value, "calls_timed_out");
    result.calls_cancelled = require_u64(value, "calls_cancelled");
    result.calls_activation_expired = require_u64(value, "calls_activation_expired");
    result.in_flight_current = require_u64(value, "in_flight_current");
    result.in_flight_high_water = require_u64(value, "in_flight_high_water");
    if (value.get("response_guards_current") != nullptr) {
        result.response_guards_current = require_u64(value, "response_guards_current");
    }
    if (value.get("response_guards_high_water") != nullptr) {
        result.response_guards_high_water = require_u64(value, "response_guards_high_water");
    }
    result.capacity_rejections = require_u64(value, "capacity_rejections");
    result.server_capacity_rejections = require_u64(value, "server_capacity_rejections");
    result.server_unavailable = require_u64(value, "server_unavailable");
    result.request_loan_failures = require_u64(value, "request_loan_failures");
    result.response_receive_failures = require_u64(value, "response_receive_failures");
    result.call_duration_count = require_u64(value, "call_duration_count");
    result.call_duration_total_ns = require_u64(value, "call_duration_total_ns");
    result.call_duration_max_ns = require_u64(value, "call_duration_max_ns");
    return result;
}

[[nodiscard]] RpcServerMetrics parse_rpc_server_metrics(const JsonValue& value) {
    RpcServerMetrics result;
    result.requests_received = require_u64(value, "requests_received");
    result.requests_current = require_u64(value, "requests_current");
    result.requests_high_water = require_u64(value, "requests_high_water");
    result.responses_attempted = require_u64(value, "responses_attempted");
    result.responses_sent = require_u64(value, "responses_sent");
    result.responses_failed = require_u64(value, "responses_failed");
    result.responses_abandoned = require_u64(value, "responses_abandoned");
    result.request_borrow_failures = require_u64(value, "request_borrow_failures");
    result.response_loan_failures = require_u64(value, "response_loan_failures");
    return result;
}

[[nodiscard]] RpcPressure parse_rpc_pressure(const JsonValue& value) {
    RpcPressure result;
    result.service_name = require_string(value, "service_name");
    result.capability_instance = optional_string(value, "capability_instance");
    if (const auto* resources = value.get("resources"); resources != nullptr) {
        result.resources = parse_rpc_resources(*resources);
    }
    result.client_activation = optional_u64(value, "client_activation");
    const auto& client = require_value(value, "client", nullptr);
    if (!client.is_null()) {
        result.client = parse_rpc_client_metrics(client);
    }
    const auto& server = require_value(value, "server", nullptr);
    if (!server.is_null()) {
        result.server = parse_rpc_server_metrics(server);
    }
    result.snapshot_timestamp_unix_ms = optional_u64(value, "snapshot_timestamp_unix_ms")
                                           .value_or(0U);
    return result;
}

[[nodiscard]] bool decode_snapshot_value(const JsonValue& value,
                                         ComponentSnapshot& snapshot,
                                         std::string* error) {
    try {
        snapshot = parse_snapshot_value(value);
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = exception.what();
        }
        return false;
    }
}

[[nodiscard]] bool is_hex(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')
            || (character >= 'A' && character <= 'F');
    });
}

class Socket final {
  public:
    Socket() = default;
    explicit Socket(int descriptor)
        : descriptor_(descriptor) {
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    ~Socket() {
        close();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return descriptor_ >= 0;
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    void close() noexcept {
        if (descriptor_ >= 0) {
            ::shutdown(descriptor_, SHUT_RDWR);
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }

  private:
    int descriptor_ { -1 };
};

[[nodiscard]] bool wait_for_fd(int descriptor, short events, int timeout_ms) {
    pollfd poll_descriptor { descriptor, events, 0 };
    while (true) {
        const int result = ::poll(&poll_descriptor, 1U, timeout_ms);
        if (result > 0) {
            return (poll_descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
        }
        if (result == 0) {
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

[[nodiscard]] std::pair<std::string, std::string> split_address(std::string_view address) {
    const std::size_t colon = address.rfind(':');
    if (colon == std::string_view::npos || colon == 0U || colon + 1U >= address.size()) {
        throw std::runtime_error("composer address must be host:port");
    }
    return { std::string(address.substr(0U, colon)), std::string(address.substr(colon + 1U)) };
}

[[nodiscard]] Socket connect_socket(std::string_view address, std::chrono::milliseconds timeout) {
    const auto [host, port] = split_address(address);
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int resolution = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (resolution != 0) {
        throw std::runtime_error(std::string("failed to resolve composer address: ")
                                 + ::gai_strerror(resolution));
    }

    Socket connected;
    for (addrinfo* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
        Socket socket(::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol));
        if (!socket) {
            continue;
        }
        const int flags = ::fcntl(socket.get(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
            continue;
        }
        const int connect_result = ::connect(socket.get(), candidate->ai_addr, candidate->ai_addrlen);
        if (connect_result < 0 && errno != EINPROGRESS) {
            continue;
        }
        if (connect_result < 0) {
            const auto milliseconds = std::max<std::int64_t>(0, timeout.count());
            if (!wait_for_fd(socket.get(), POLLOUT, static_cast<int>(milliseconds))) {
                continue;
            }
            int socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) < 0
                || socket_error != 0) {
                continue;
            }
        }
        connected = std::move(socket);
        break;
    }
    ::freeaddrinfo(result);
    if (!connected) {
        throw std::runtime_error("failed to connect to composer");
    }
    return connected;
}

[[nodiscard]] bool send_all(const Socket& socket, std::string_view data, int timeout_ms) {
    std::size_t sent = 0U;
    while (sent < data.size()) {
        if (!wait_for_fd(socket.get(), POLLOUT, timeout_ms)) {
            return false;
        }
        const ssize_t written = ::send(socket.get(), data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] std::string line_for(const ClientMessage& message) {
    std::string encoded = ControlCodec::encode(message);
    encoded.push_back('\n');
    return encoded;
}

} // namespace

std::string generate_uuid() {
    std::array<std::uint8_t, 16> bytes {};
    std::random_device random;
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(random());
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(36U);
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            result.push_back('-');
        }
        result.push_back(digits[(bytes[index] >> 4U) & 0x0FU]);
        result.push_back(digits[bytes[index] & 0x0FU]);
    }
    return result;
}

std::string OwnedMessageTypeDescriptor::qualified_name() const {
    return namespace_name + "/" + name + "@" + std::to_string(abi_major);
}

bool OwnedMessageTypeDescriptor::wire_equal(const OwnedMessageTypeDescriptor& other) const noexcept {
    return namespace_name == other.namespace_name && name == other.name && abi_major == other.abi_major
        && shape == other.shape && header_size == other.header_size && header_align == other.header_align
        && element_size == other.element_size && element_align == other.element_align
        && layout_hash == other.layout_hash;
}

StreamDescriptor OwnedStreamDescriptor::borrowed() const {
    MessageTypeDescriptor descriptor {
        message_type.namespace_name.empty() ? std::string_view {} : std::string_view(message_type.namespace_name),
        std::string_view(message_type.name),
        message_type.abi_major,
        message_type.shape,
        message_type.header_size,
        message_type.header_align,
        message_type.element_size,
        message_type.element_align,
        {},
        std::string_view(message_type.local_fields),
    };
    if (!message_type.layout_hash.empty() && message_type.layout_hash.size() == 32U && is_hex(message_type.layout_hash)) {
        for (std::size_t index = 0U; index < 16U; ++index) {
            auto hex = [](char character) -> std::uint8_t {
                if (character >= '0' && character <= '9') {
                    return static_cast<std::uint8_t>(character - '0');
                }
                if (character >= 'a' && character <= 'f') {
                    return static_cast<std::uint8_t>(character - 'a' + 10);
                }
                return static_cast<std::uint8_t>(character - 'A' + 10);
            };
            descriptor.layout_hash[index] = static_cast<std::uint8_t>(
                (hex(message_type.layout_hash[index * 2U]) << 4U) | hex(message_type.layout_hash[index * 2U + 1U]));
        }
    }
    return StreamDescriptor(service_name, descriptor, profile, transport, resources);
}

OwnedStreamDescriptor OwnedStreamDescriptor::from(const StreamDescriptor& descriptor) {
    OwnedStreamDescriptor result;
    result.service_name = descriptor.service_name;
    result.message_type.namespace_name = std::string(descriptor.message_type.package);
    result.message_type.name = std::string(descriptor.message_type.name);
    result.message_type.abi_major = descriptor.message_type.abi_major;
    result.message_type.shape = descriptor.message_type.shape;
    result.message_type.header_size = descriptor.message_type.header_size;
    result.message_type.header_align = descriptor.message_type.header_align;
    result.message_type.element_size = descriptor.message_type.element_size;
    result.message_type.element_align = descriptor.message_type.element_align;
    result.message_type.layout_hash = lowercase_hex(descriptor.message_type.layout_hash);
    result.message_type.local_fields = std::string(descriptor.message_type.fields);
    result.profile = descriptor.profile;
    result.transport = descriptor.transport;
    result.resources = descriptor.resources;
    return result;
}

RpcDescriptor OwnedRpcDescriptor::borrowed() const {
    const auto borrow_message = [](const OwnedMessageTypeDescriptor& type) {
        MessageTypeDescriptor descriptor {
            std::string_view(type.namespace_name),
            std::string_view(type.name),
            type.abi_major,
            type.shape,
            type.header_size,
            type.header_align,
            type.element_size,
            type.element_align,
            {},
            std::string_view(type.local_fields),
        };
        if (type.layout_hash.size() == 32U && is_hex(type.layout_hash)) {
            for (std::size_t index = 0U; index < descriptor.layout_hash.size(); ++index) {
                const auto nibble = [](char character) -> std::uint8_t {
                    if (character >= '0' && character <= '9') {
                        return static_cast<std::uint8_t>(character - '0');
                    }
                    if (character >= 'a' && character <= 'f') {
                        return static_cast<std::uint8_t>(character - 'a' + 10);
                    }
                    return static_cast<std::uint8_t>(character - 'A' + 10);
                };
                descriptor.layout_hash[index] = static_cast<std::uint8_t>(
                    (nibble(type.layout_hash[index * 2U]) << 4U)
                    | nibble(type.layout_hash[index * 2U + 1U]));
            }
        }
        return descriptor;
    };
    return RpcDescriptor(service_name,
                         borrow_message(request_type),
                         borrow_message(response_type),
                         transport,
                         resources);
}

OwnedRpcDescriptor OwnedRpcDescriptor::from(const RpcDescriptor& descriptor) {
    const auto own_message = [](const MessageTypeDescriptor& type) {
        OwnedMessageTypeDescriptor result;
        result.namespace_name = std::string(type.package);
        result.name = std::string(type.name);
        result.abi_major = type.abi_major;
        result.shape = type.shape;
        result.header_size = type.header_size;
        result.header_align = type.header_align;
        result.element_size = type.element_size;
        result.element_align = type.element_align;
        result.layout_hash = lowercase_hex(type.layout_hash);
        result.local_fields = std::string(type.fields);
        return result;
    };

    OwnedRpcDescriptor result;
    result.service_name = descriptor.service_name;
    result.request_type = own_message(descriptor.request_type);
    result.response_type = own_message(descriptor.response_type);
    result.transport = descriptor.transport;
    result.resources = descriptor.resources;
    return result;
}

ComponentState ComponentState::declared() {
    return { ComponentStateKind::Declared, {} };
}

ComponentState ComponentState::preparing() {
    return { ComponentStateKind::Preparing, {} };
}

ComponentState ComponentState::blocked(std::string reason_value) {
    return { ComponentStateKind::Blocked, std::move(reason_value) };
}

ComponentState ComponentState::activating() {
    return { ComponentStateKind::Activating, {} };
}

ComponentState ComponentState::active() {
    return { ComponentStateKind::Active, {} };
}

ComponentState ComponentState::quiescing() {
    return { ComponentStateKind::Quiescing, {} };
}

ComponentState ComponentState::failed(std::string reason_value) {
    return { ComponentStateKind::Failed, std::move(reason_value) };
}

bool ComponentState::is(ComponentStateKind expected) const noexcept {
    return kind == expected;
}

std::string ComponentState::name() const {
    switch (kind) {
    case ComponentStateKind::Declared:
        return "Declared";
    case ComponentStateKind::Preparing:
        return "Preparing";
    case ComponentStateKind::Blocked:
        return "Blocked";
    case ComponentStateKind::Activating:
        return "Activating";
    case ComponentStateKind::Active:
        return "Active";
    case ComponentStateKind::Quiescing:
        return "Quiescing";
    case ComponentStateKind::Failed:
        return "Failed";
    }
    return "Preparing";
}

bool ComponentSnapshot::validate(std::string* error) const {
    const auto fail_validation = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    if (snapshot_version != COMPOSER_SNAPSHOT_VERSION) {
        return fail_validation("snapshot version mismatch");
    }
    if (state.is(ComponentStateKind::Active) && !activation.has_value()) {
        return fail_validation("Active snapshot has no activation");
    }
    std::vector<std::string> requirement_names;
    for (const auto& requirement : requirements) {
        if (std::find(requirement_names.begin(), requirement_names.end(), requirement.requirement.requirement_name)
            != requirement_names.end()) {
            return fail_validation("duplicate requirement " + requirement.requirement.requirement_name);
        }
        requirement_names.push_back(requirement.requirement.requirement_name);
        if (requirement.binding.has_value()
            && (requirement.binding->requirement_name != requirement.requirement.requirement_name
                || requirement.binding->capability_type != requirement.requirement.capability_type)) {
            return fail_validation("requirement binding does not match declaration");
        }
    }
    if (activation.has_value()) {
        std::vector<std::string> binding_names;
        for (const auto& binding : activation->bindings) {
            if (std::find(binding_names.begin(), binding_names.end(), binding.requirement_name) != binding_names.end()) {
                return fail_validation("duplicate activation binding " + binding.requirement_name);
            }
            binding_names.push_back(binding.requirement_name);
            const auto requirement = std::find_if(requirements.begin(), requirements.end(), [&](const auto& item) {
                return item.requirement.requirement_name == binding.requirement_name;
            });
            if (requirement == requirements.end() || !requirement->binding.has_value()
                || *requirement->binding != binding) {
                return fail_validation("activation binding does not match requirement snapshot");
            }
        }
    } else {
        for (const auto& requirement : requirements) {
            if (requirement.binding.has_value()) {
                return fail_validation("bound requirement exists without activation");
            }
        }
    }
    for (const auto& capability : provided_capabilities) {
        if (capability.capability_instance != capability.descriptor.capability_instance) {
            return fail_validation("capability instance differs from descriptor");
        }
        if (capability.capability_type.empty()) {
            return fail_validation("capability type is empty");
        }
        if (capability.descriptor.provider_component != component_id
            || capability.descriptor.provider_process != process_instance
            || !activation.has_value() || capability.descriptor.activation != activation->activation_id) {
            return fail_validation("capability descriptor owner does not match snapshot");
        }
    }
    return true;
}

ClientMessage ClientMessage::hello(ComponentId component, ProcessInstanceId process) {
    ClientMessage result;
    result.kind = Kind::Hello;
    result.component_id = std::move(component);
    result.process_instance = std::move(process);
    return result;
}

ClientMessage ClientMessage::snapshot_message(ComponentSnapshot value) {
    ClientMessage result;
    result.kind = Kind::Snapshot;
    result.snapshot = std::move(value);
    return result;
}

ClientMessage ClientMessage::inspect(std::uint16_t control_version, std::uint16_t snapshot_version_value) {
    ClientMessage result;
    result.kind = Kind::Inspect;
    result.control_protocol_version = control_version;
    result.snapshot_version = snapshot_version_value;
    return result;
}

bool ServerMessage::carries_revision() const noexcept {
    return kind == Kind::Activate || kind == Kind::Deactivate || kind == Kind::ProcessConflict;
}

ActivationContext::ActivationContext(ComponentId component,
                                     ProcessInstanceId process,
                                     ActivationId activation,
                                     std::vector<ResolvedBinding> bindings,
                                     EffectScope& scope)
    : component_(std::move(component))
    , process_(std::move(process))
    , activation_(activation)
    , bindings_(std::move(bindings))
    , scope_(scope) {
}

const ComponentId& ActivationContext::component() const noexcept {
    return component_;
}

const ProcessInstanceId& ActivationContext::process_instance() const noexcept {
    return process_;
}

ActivationId ActivationContext::activation_id() const noexcept {
    return activation_;
}

const std::vector<ResolvedBinding>& ActivationContext::bindings() const noexcept {
    return bindings_;
}

EffectScope& ActivationContext::scope() noexcept {
    return scope_;
}

const std::vector<ProvidedCapability>& ActivationContext::capabilities() const noexcept {
    return provided_;
}

std::string ControlCodec::encode_snapshot(const ComponentSnapshot& snapshot) {
    return snapshot_json_impl(snapshot);
}

std::string ControlCodec::encode(const ClientMessage& message) {
    switch (message.kind) {
    case ClientMessage::Kind::Hello:
        return "{\"type\":\"hello\",\"component_id\":" + json_escape(message.component_id)
            + ",\"process_instance\":" + json_escape(message.process_instance)
            + ",\"control_protocol_version\":" + std::to_string(message.control_protocol_version)
            + ",\"snapshot_version\":" + std::to_string(message.snapshot_version) + "}";
    case ClientMessage::Kind::Snapshot:
        return "{\"type\":\"snapshot\",\"snapshot\":" + encode_snapshot(message.snapshot) + "}";
    case ClientMessage::Kind::Inspect:
        return "{\"type\":\"inspect\",\"control_protocol_version\":"
            + std::to_string(message.control_protocol_version) + ",\"snapshot_version\":"
            + std::to_string(message.snapshot_version) + "}";
    }
    return "{}";
}

bool ControlCodec::decode_snapshot(std::string_view json,
                                   ComponentSnapshot& snapshot,
                                   std::string* error) {
    try {
        JsonParser parser(json);
        return decode_snapshot_value(parser.parse(), snapshot, error);
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = exception.what();
        }
        return false;
    }
}

bool ControlCodec::decode_server(std::string_view line, ServerMessage& message, std::string* error) {
    try {
        JsonParser parser(line);
        const JsonValue root = parser.parse();
        const auto type = require_string(root, "type");
        if (type == "activate") {
            message = ServerMessage {};
            message.kind = ServerMessage::Kind::Activate;
            const auto& revision = require_value(root, "revision", nullptr);
            message.revision.epoch = require_string(revision, "epoch");
            message.revision.sequence = require_u64(revision, "sequence");
            const auto* bindings = require_value(root, "bindings", nullptr).array();
            if (bindings == nullptr) {
                throw std::runtime_error("activate.bindings must be an array");
            }
            message.bindings.reserve(bindings->size());
            for (const auto& binding : *bindings) {
                message.bindings.push_back(parse_binding(binding));
            }
        } else if (type == "deactivate") {
            message = ServerMessage {};
            message.kind = ServerMessage::Kind::Deactivate;
            const auto& revision = require_value(root, "revision", nullptr);
            message.revision.epoch = require_string(revision, "epoch");
            message.revision.sequence = require_u64(revision, "sequence");
            message.expected_process_instance = require_string(root, "expected_process_instance");
            message.expected_activation = require_u64(root, "expected_activation");
            message.reason = require_string(root, "reason");
        } else if (type == "process_conflict") {
            message = ServerMessage {};
            message.kind = ServerMessage::Kind::ProcessConflict;
            const auto& revision = require_value(root, "revision", nullptr);
            message.revision.epoch = require_string(revision, "epoch");
            message.revision.sequence = require_u64(revision, "sequence");
            message.component_id = require_string(root, "component_id");
            message.incumbent = require_string(root, "incumbent");
            message.contender = require_string(root, "contender");
        } else if (type == "protocol_mismatch") {
            message = ServerMessage {};
            message.kind = ServerMessage::Kind::ProtocolMismatch;
            message.expected_control_protocol_version = require_u16(root, "expected_control_protocol_version");
            message.expected_snapshot_version = require_u16(root, "expected_snapshot_version");
            message.received_control_protocol_version = optional_u16(root, "received_control_protocol_version");
            message.received_snapshot_version = optional_u16(root, "received_snapshot_version");
            message.reason = require_string(root, "reason");
        } else if (type == "inspect_response") {
            message = ServerMessage {};
            message.kind = ServerMessage::Kind::InspectResponse;
            const auto* view = root.get("view");
            if (view == nullptr) {
                throw std::runtime_error("inspect_response.view is missing");
            }
            // Inspection is intentionally opaque to the component runtime;
            // retain a bounded, owned canonical subtree for diagnostics.
            message.raw_view_json = "<inspect_response>";
            (void)view;
        } else {
            throw std::runtime_error("unknown server message type " + type);
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = exception.what();
        }
        return false;
    }
}

struct ComponentRuntime::Impl {
    struct QueueEvent {
        enum class Kind : std::uint8_t {
            Command,
            Task,
            Connected,
            Disconnected,
        };

        Kind kind { Kind::Disconnected };
        ServerMessage command;
        TaskEvent task;
    };

    std::unique_ptr<Component> component;
    RuntimeOptions options;
    ComponentId component_id;
    ProcessInstanceId process_instance;
    std::vector<RequirementSpec> requirements;
    ComponentState lifecycle { ComponentState::preparing() };
    std::optional<ActivationSnapshot> activation;
    std::vector<ProvidedCapability> capabilities;
    std::unique_ptr<EffectScope> scope;
    ActivationId next_activation { 1U };
    std::optional<Revision> latest_revision;
    std::optional<ProtocolMismatch> mismatch;
    mutable std::mutex state_mutex;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<QueueEvent> queue;
    std::atomic<bool> stop_requested { false };
    std::atomic<bool> connected { false };
    std::thread control_thread;
    std::condition_variable snapshot_cv;
    std::mutex snapshot_mutex;
    std::uint64_t snapshot_generation { 0U };
    bool prepared { false };
    bool running { false };

    explicit Impl(std::unique_ptr<Component> value, RuntimeOptions runtime_options)
        : component(std::move(value))
        , options(std::move(runtime_options)) {
    }

    ~Impl() {
        request_stop();
        if (control_thread.joinable()) {
            control_thread.join();
        }
        shutdown_activation();
    }

    void request_stop() noexcept {
        stop_requested.store(true, std::memory_order_release);
        snapshot_cv.notify_all();
        queue_cv.notify_all();
    }

    void enqueue(QueueEvent event) {
        {
            std::lock_guard lock(queue_mutex);
            queue.push_back(std::move(event));
        }
        queue_cv.notify_one();
    }

    [[nodiscard]] ComponentSnapshot make_snapshot() const {
        std::lock_guard lock(state_mutex);
        ComponentSnapshot result;
        result.component_id = component_id;
        result.process_instance = process_instance;
        result.state = lifecycle;
        result.activation = activation;
        result.requirements.reserve(requirements.size());
        for (const auto& requirement : requirements) {
            RequirementSnapshot snapshot;
            snapshot.requirement = requirement;
            if (activation.has_value()) {
                const auto binding = std::find_if(activation->bindings.begin(), activation->bindings.end(),
                                                  [&](const auto& value) {
                                                      return value.requirement_name == requirement.requirement_name;
                                                  });
                if (binding != activation->bindings.end()) {
                    snapshot.binding = *binding;
                }
            }
            result.requirements.push_back(std::move(snapshot));
        }
        for (const auto& capability : capabilities) {
            result.provided_capabilities.push_back(CapabilitySnapshot {
                capability.capability_type,
                capability.descriptor.capability_instance,
                capability.descriptor,
            });
        }
        result.stream_pressures = local_pressure_snapshots();
        result.rpc_pressures = local_rpc_pressure_snapshots();
        return result;
    }

    void publish_snapshot() {
        {
            std::lock_guard lock(snapshot_mutex);
            ++snapshot_generation;
        }
        snapshot_cv.notify_all();
    }

    void set_connected(bool value) {
        connected.store(value, std::memory_order_release);
        enqueue(QueueEvent { value ? QueueEvent::Kind::Connected : QueueEvent::Kind::Disconnected, {}, {} });
    }

    void shutdown_activation() noexcept {
        std::unique_ptr<EffectScope> scope_to_shutdown;
        {
            std::lock_guard lock(state_mutex);
            scope_to_shutdown = std::move(scope);
            activation.reset();
            capabilities.clear();
        }
        if (scope_to_shutdown != nullptr) {
            scope_to_shutdown->shutdown();
        }
    }

    void control_loop() {
        std::size_t backoff_index = 0U;
        while (!stop_requested.load(std::memory_order_acquire)) {
            Socket socket;
            try {
                socket = connect_socket(options.composer_address, options.connect_timeout);
                backoff_index = 0U;
                const auto hello = ClientMessage::hello(component_id, process_instance);
                const auto snapshot = ClientMessage::snapshot_message(make_snapshot());
                const int timeout = static_cast<int>(std::max<std::int64_t>(1, options.connect_timeout.count()));
                if (!send_all(socket, line_for(hello), timeout) || !send_all(socket, line_for(snapshot), timeout)) {
                    throw std::runtime_error("failed to send component hello/snapshot");
                }
                set_connected(true);

                std::string input;
                input.reserve(4096U);
                std::uint64_t sent_generation = 0U;
                {
                    std::lock_guard lock(snapshot_mutex);
                    sent_generation = snapshot_generation;
                }
                ComponentSnapshot last_sent_snapshot = snapshot.snapshot;
                auto next_pressure_snapshot = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (!stop_requested.load(std::memory_order_acquire)) {
                    std::uint64_t generation = 0U;
                    {
                        std::lock_guard lock(snapshot_mutex);
                        generation = snapshot_generation;
                    }
                    if (generation != sent_generation) {
                        auto current = make_snapshot();
                        const auto update = line_for(ClientMessage::snapshot_message(current));
                        if (!send_all(socket, update, timeout)) {
                            break;
                        }
                        last_sent_snapshot = std::move(current);
                        sent_generation = generation;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    if (now >= next_pressure_snapshot) {
                        auto current = make_snapshot();
                        if (current != last_sent_snapshot) {
                            const auto update = line_for(ClientMessage::snapshot_message(current));
                            if (!send_all(socket, update, timeout)) {
                                break;
                            }
                            last_sent_snapshot = std::move(current);
                        }
                        next_pressure_snapshot = now + std::chrono::seconds(1);
                    }

                    const int poll_timeout = static_cast<int>(std::max<std::int64_t>(1, options.poll_interval.count()));
                    if (!wait_for_fd(socket.get(), POLLIN, poll_timeout)) {
                        continue;
                    }
                    char buffer[4096];
                    const ssize_t received = ::recv(socket.get(), buffer, sizeof(buffer), 0);
                    if (received <= 0) {
                        break;
                    }
                    input.append(buffer, static_cast<std::size_t>(received));
                    if (input.size() > kMaxJsonBytes) {
                        break;
                    }
                    while (true) {
                        const auto newline = input.find('\n');
                        if (newline == std::string::npos) {
                            break;
                        }
                        std::string line = input.substr(0U, newline);
                        input.erase(0U, newline + 1U);
                        if (!line.empty() && line.back() == '\r') {
                            line.pop_back();
                        }
                        if (line.empty()) {
                            continue;
                        }
                        ServerMessage message;
                        std::string error;
                        if (!ControlCodec::decode_server(line, message, &error)) {
                            break;
                        }
                        enqueue(QueueEvent { QueueEvent::Kind::Command, std::move(message), {} });
                    }
                }
            } catch (const std::exception&) {
                // A control outage is deliberately orthogonal to local
                // activation/capability state.  The next iteration reconnects.
            }
            if (connected.exchange(false, std::memory_order_acq_rel)) {
                enqueue(QueueEvent { QueueEvent::Kind::Disconnected, {}, {} });
            }
            if (stop_requested.load(std::memory_order_acquire)) {
                break;
            }
            const auto delay = options.reconnect_backoff.empty()
                ? std::chrono::milliseconds(100)
                : options.reconnect_backoff[std::min(backoff_index, options.reconnect_backoff.size() - 1U)];
            {
                std::unique_lock lock(snapshot_mutex);
                snapshot_cv.wait_for(lock, delay, [this] {
                    return stop_requested.load(std::memory_order_acquire);
                });
            }
            if (!options.reconnect_backoff.empty()) {
                backoff_index = std::min(backoff_index + 1U, options.reconnect_backoff.size() - 1U);
            }
        }
    }

    bool accepts_revision(const Revision& revision) {
        if (!latest_revision.has_value() || latest_revision->epoch != revision.epoch
            || revision.sequence > latest_revision->sequence) {
            latest_revision = revision;
            return true;
        }
        return false;
    }

    bool handle_server(const ServerMessage& message) {
        std::lock_guard lock(state_mutex);
        if (message.carries_revision() && !accepts_revision(message.revision)) {
            return false;
        }
        switch (message.kind) {
        case ServerMessage::Kind::Activate:
            return handle_activate_locked(message);
        case ServerMessage::Kind::Deactivate:
            return handle_deactivate_locked(message);
        case ServerMessage::Kind::ProcessConflict:
            publish_snapshot();
            return true;
        case ServerMessage::Kind::ProtocolMismatch:
            mismatch = ProtocolMismatch {
                message.expected_control_protocol_version,
                message.expected_snapshot_version,
                message.received_control_protocol_version,
                message.received_snapshot_version,
                message.reason,
            };
            return true;
        case ServerMessage::Kind::InspectResponse:
            return true;
        }
        return false;
    }

    bool handle_activate_locked(const ServerMessage& message) {
        if (scope != nullptr || !(lifecycle.is(ComponentStateKind::Preparing) || lifecycle.is(ComponentStateKind::Blocked))) {
            return false;
        }
        const ActivationId activation_id = next_activation++;
        lifecycle = ComponentState::activating();
        publish_snapshot();

        auto new_scope = std::make_unique<EffectScope>(process_instance, activation_id, [this](TaskEvent event) {
            enqueue(QueueEvent { QueueEvent::Kind::Task, {}, std::move(event) });
        });
        ActivationContext context(component_id, process_instance, activation_id, message.bindings, *new_scope);
        Result<void> activation_result;
        try {
            activation_result = component->activate(context);
        } catch (const std::exception& error) {
            activation_result = Result<void>::failure(error.what());
        } catch (...) {
            activation_result = Result<void>::failure("unknown exception during activation");
        }
        if (!activation_result) {
            new_scope->shutdown();
            lifecycle = ComponentState::failed(activation_result.message());
            publish_snapshot();
            return false;
        }
        activation = ActivationSnapshot { activation_id, message.bindings };
        capabilities = context.capabilities();
        scope = std::move(new_scope);
        lifecycle = ComponentState::active();
        publish_snapshot();
        return true;
    }

    bool handle_deactivate_locked(const ServerMessage& message) {
        if (scope == nullptr || expected_process_instance_mismatch(message.expected_process_instance)
            || !activation.has_value() || activation->activation_id != message.expected_activation) {
            return false;
        }
        lifecycle = ComponentState::quiescing();
        publish_snapshot();
        auto old_scope = std::move(scope);
        activation.reset();
        capabilities.clear();
        // Explicit shutdown is the synchronization point for all activation
        // endpoints and LIFO cleanup callbacks.
        old_scope->shutdown();
        lifecycle = ComponentState::blocked(message.reason);
        publish_snapshot();
        return true;
    }

    [[nodiscard]] bool expected_process_instance_mismatch(const std::string& expected) const noexcept {
        return expected != process_instance;
    }

    void handle_task(const TaskEvent& event) {
        std::lock_guard lock(state_mutex);
        if (event.process_instance != process_instance || scope == nullptr || !activation.has_value()
            || event.activation_id != activation->activation_id) {
            return;
        }
        if (event.kind == TaskKind::Background) {
            return;
        }
        lifecycle = ComponentState::quiescing();
        publish_snapshot();
        auto old_scope = std::move(scope);
        activation.reset();
        capabilities.clear();
        old_scope->shutdown();
        std::string reason = "critical task " + event.task_name + " exited";
        if (!event.outcome.message.empty()) {
            reason += ": " + event.outcome.message;
        }
        lifecycle = ComponentState::failed(std::move(reason));
        publish_snapshot();
    }

    void process_queue() {
        std::deque<QueueEvent> events;
        {
            std::lock_guard lock(queue_mutex);
            events.swap(queue);
        }
        for (const auto& event : events) {
            switch (event.kind) {
            case QueueEvent::Kind::Command:
                (void)handle_server(event.command);
                break;
            case QueueEvent::Kind::Task:
                handle_task(event.task);
                break;
            case QueueEvent::Kind::Connected:
                {
                    std::lock_guard lock(state_mutex);
                    latest_revision.reset();
                    // Preparation has completed before the control thread is
                    // started. Until Composer supplies a resolved binding
                    // set, expose the same Blocked lifecycle used after a
                    // provider disappears instead of lingering in Preparing.
                    if (lifecycle.is(ComponentStateKind::Preparing)) {
                        lifecycle = ComponentState::blocked(
                            "awaiting resolved activation bindings");
                        publish_snapshot();
                    }
                }
                break;
            case QueueEvent::Kind::Disconnected:
                break;
            }
        }
    }
};

ComponentRuntime::ComponentRuntime(std::unique_ptr<Component> component, RuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(component), std::move(options))) {
}

ComponentRuntime::~ComponentRuntime() = default;

int ComponentRuntime::run() {
    if (impl_->component == nullptr) {
        return 1;
    }
    RuntimeSignalHandlers signal_handlers;
    if (!signal_handlers.installed()) {
        return 1;
    }
    {
        std::lock_guard lock(impl_->state_mutex);
        if (!impl_->prepared) {
            impl_->component_id = impl_->component->id();
            impl_->process_instance = generate_uuid();
            auto prepared = Result<void>::failure("component preparation failed");
            try {
                prepared = impl_->component->prepare();
            } catch (const std::exception& error) {
                prepared = Result<void>::failure(error.what());
            } catch (...) {
                prepared = Result<void>::failure("unknown exception during preparation");
            }
            if (!prepared) {
                impl_->lifecycle = ComponentState::failed(prepared.message());
                impl_->prepared = true;
                impl_->publish_snapshot();
                return 1;
            }
            impl_->requirements = impl_->component->requirements();
            impl_->lifecycle = ComponentState::preparing();
            impl_->prepared = true;
            impl_->publish_snapshot();
        }
    }
    if (signal_handlers.stop_requested()) {
        impl_->request_stop();
    }
    impl_->running = true;
    impl_->control_thread = std::thread([this] { impl_->control_loop(); });
    while (!impl_->stop_requested.load(std::memory_order_acquire)) {
        std::unique_lock lock(impl_->queue_mutex);
        impl_->queue_cv.wait_for(lock, std::chrono::milliseconds(25), [this] {
            return impl_->stop_requested.load(std::memory_order_acquire) || runtime_stop_signal != 0
                || !impl_->queue.empty();
        });
        lock.unlock();
        if (signal_handlers.stop_requested()) {
            impl_->request_stop();
        }
        impl_->process_queue();
    }
    if (signal_handlers.stop_requested()) {
        impl_->request_stop();
    }
    impl_->process_queue();
    if (impl_->control_thread.joinable()) {
        impl_->control_thread.join();
    }
    impl_->shutdown_activation();
    impl_->running = false;
    return 0;
}

void ComponentRuntime::stop() noexcept {
    if (impl_ != nullptr) {
        impl_->request_stop();
    }
}

ComponentSnapshot ComponentRuntime::snapshot() const {
    return impl_->make_snapshot();
}

ComponentState ComponentRuntime::state() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->lifecycle;
}

bool ComponentRuntime::control_plane_connected() const noexcept {
    return impl_->connected.load(std::memory_order_acquire);
}

std::optional<ProtocolMismatch> ComponentRuntime::last_protocol_mismatch() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->mismatch;
}

bool ComponentRuntime::apply_server_message(const ServerMessage& message) {
    return impl_->handle_server(message);
}

void ComponentRuntime::apply_task_event(const TaskEvent& event) {
    impl_->handle_task(event);
}

} // namespace rbt
