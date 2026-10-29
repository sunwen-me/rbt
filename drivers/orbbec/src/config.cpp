#include <rbt_orbbec/driver.hpp>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rbt_orbbec {
namespace {

constexpr std::size_t kMaximumFilterNameBytes = 63U;
constexpr std::size_t kMaximumDepthFilters = 16U;
constexpr std::size_t kMaximumFilterParameters = 64U;

[[noreturn]] void invalid(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

[[nodiscard]] bool ascii_alpha(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

[[nodiscard]] bool ascii_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaximumFilterNameBytes || !ascii_alpha(name.front())) {
        return false;
    }
    for (const auto character : name) {
        if (!ascii_alpha(character) && !ascii_digit(character) && character != '_') {
            return false;
        }
    }
    return true;
}

void require_name(std::string_view name, std::string_view description) {
    if (!valid_name(name)) {
        invalid(std::string(description) + " must start with an ASCII letter and contain only ASCII letters, "
                "digits, or underscores (up to 63 bytes)");
    }
}

[[nodiscard]] double parse_finite_double(std::string_view text) {
    if (text.empty()) {
        invalid("depth filter parameter value must be a finite decimal number");
    }

    // from_chars is locale-independent. It intentionally does not accept a leading
    // plus sign, so consume that one optional sign before parsing the magnitude.
    if (text.front() == '+') {
        text.remove_prefix(1U);
        if (text.empty() || text.front() == '+' || text.front() == '-') {
            invalid("depth filter parameter value must be a finite decimal number");
        }
    }

    double value = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
        invalid("depth filter parameter value must be a finite decimal number");
    }
    return value;
}

} // namespace

DepthFilterConfig parse_depth_filter(std::string_view text) {
    const auto colon = text.find(':');
    const auto name = text.substr(0U, colon);
    require_name(name, "depth filter name");

    DepthFilterConfig result;
    result.name = name;
    if (colon == std::string_view::npos) {
        return result;
    }

    const auto parameter_text = text.substr(colon + 1U);
    if (parameter_text.empty()) {
        invalid("depth filter parameter list cannot be empty");
    }

    std::size_t position = 0U;
    while (position <= parameter_text.size()) {
        const auto comma = parameter_text.find(',', position);
        const auto end = comma == std::string_view::npos ? parameter_text.size() : comma;
        const auto item = parameter_text.substr(position, end - position);
        if (item.empty()) {
            invalid("depth filter parameter cannot be empty");
        }
        const auto equals = item.find('=');
        if (equals == std::string_view::npos || item.find('=', equals + 1U) != std::string_view::npos) {
            invalid("depth filter parameters must use name=value syntax");
        }

        const auto parameter_name = item.substr(0U, equals);
        require_name(parameter_name, "depth filter parameter name");
        for (const auto& previous : result.parameters) {
            if (previous.name == parameter_name) {
                invalid("depth filter parameter names must be unique");
            }
        }
        if (result.parameters.size() >= kMaximumFilterParameters) {
            invalid("depth filter configuration exceeds 64 parameters");
        }

        result.parameters.push_back(FilterParameter {
            std::string(parameter_name),
            parse_finite_double(item.substr(equals + 1U)),
        });

        if (comma == std::string_view::npos) {
            break;
        }
        position = comma + 1U;
    }
    return result;
}

void validate_processing_options(const Options& options) {
    if (options.align_depth_to_color && (!options.color || !options.depth)) {
        invalid("depth-to-color alignment requires both color and depth streams");
    }
    if (!options.depth_filters.empty() && !options.depth) {
        invalid("depth filters require depth to be enabled");
    }
    if (options.reconnect_delay_ms < 100U || options.reconnect_delay_ms > 30000U) {
        invalid("reconnect delay must be between 100 and 30000 milliseconds");
    }
    if (options.depth_filters.size() > kMaximumDepthFilters) {
        invalid("at most 16 depth filters may be configured");
    }

    std::size_t total_parameters = 0U;
    for (std::size_t index = 0U; index < options.depth_filters.size(); ++index) {
        const auto& filter = options.depth_filters[index];
        require_name(filter.name, "depth filter name");
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (options.depth_filters[previous].name == filter.name) {
                invalid("depth filter names must be unique");
            }
        }
        if (filter.parameters.size() > kMaximumFilterParameters - total_parameters) {
            invalid("at most 64 depth filter parameters may be configured in total");
        }
        total_parameters += filter.parameters.size();

        for (std::size_t parameter_index = 0U; parameter_index < filter.parameters.size(); ++parameter_index) {
            const auto& parameter = filter.parameters[parameter_index];
            require_name(parameter.name, "depth filter parameter name");
            if (!std::isfinite(parameter.value)) {
                invalid("depth filter parameter values must be finite");
            }
            for (std::size_t previous = 0U; previous < parameter_index; ++previous) {
                if (filter.parameters[previous].name == parameter.name) {
                    invalid("depth filter parameter names must be unique");
                }
            }
        }
    }
}

std::string output_frame_id(const Options& options, Kind kind) {
    if (options.align_depth_to_color && (kind == Kind::Depth || kind == Kind::Points)) {
        kind = Kind::Color;
    }
    return frame_id(options.frame_prefix, kind);
}

} // namespace rbt_orbbec
