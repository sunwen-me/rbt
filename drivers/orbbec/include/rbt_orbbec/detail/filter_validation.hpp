#pragma once

#include <rbt_orbbec/driver.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rbt_orbbec::detail {

enum class FilterValueType { Integer, Floating, Boolean, Unsupported };

struct FilterParameterSchema {
    std::string name;
    FilterValueType type { FilterValueType::Unsupported };
    double minimum { 0.0 };
    double maximum { 0.0 };
    double step { 0.0 };
};

inline long double float32_ulp(long double value) noexcept {
    const auto narrowed = static_cast<float>(value);
    if (!std::isfinite(narrowed)) {
        return 0.0L;
    }
    const auto upward = std::nextafter(narrowed, std::numeric_limits<float>::infinity());
    const auto downward = std::nextafter(narrowed, -std::numeric_limits<float>::infinity());
    long double result = 0.0L;
    if (std::isfinite(upward)) {
        result = std::max(result, std::abs(static_cast<long double>(upward) - narrowed));
    }
    if (std::isfinite(downward)) {
        result = std::max(result, std::abs(static_cast<long double>(narrowed) - downward));
    }
    return result;
}

inline void validate_filter_parameter_value(std::string_view name,
                                            double value,
                                            const FilterParameterSchema& schema) {
    const auto parameter = std::string(name);
    if (!std::isfinite(value)) {
        throw std::invalid_argument("filter parameter '" + parameter + "' must be finite");
    }
    if (!std::isfinite(schema.minimum) || !std::isfinite(schema.maximum) || !std::isfinite(schema.step)
        || schema.minimum > schema.maximum || schema.step < 0.0) {
        throw std::runtime_error("SDK returned an invalid schema for filter parameter '" + parameter + "'");
    }
    if (schema.type == FilterValueType::Unsupported) {
        throw std::invalid_argument("filter parameter '" + parameter + "' has an unsupported SDK value type");
    }
    if ((schema.type == FilterValueType::Integer
         && (std::trunc(schema.minimum) != schema.minimum || std::trunc(schema.maximum) != schema.maximum
             || std::trunc(schema.step) != schema.step))
        || (schema.type == FilterValueType::Boolean
            && (std::trunc(schema.minimum) != schema.minimum || std::trunc(schema.maximum) != schema.maximum
                || std::trunc(schema.step) != schema.step || schema.minimum < 0.0 || schema.maximum > 1.0))) {
        throw std::runtime_error("SDK returned a non-integral range for filter parameter '" + parameter + "'");
    }
    auto effective_value = static_cast<long double>(value);
    if (schema.type == FilterValueType::Floating) {
        const auto typed_value = static_cast<float>(value);
        const auto typed_minimum = static_cast<float>(schema.minimum);
        const auto typed_maximum = static_cast<float>(schema.maximum);
        const auto typed_step = static_cast<float>(schema.step);
        if (!std::isfinite(typed_minimum) || !std::isfinite(typed_maximum) || !std::isfinite(typed_step)) {
            throw std::runtime_error("SDK returned a non-finite float range for filter parameter '" + parameter + "'");
        }
        if (!std::isfinite(typed_value) || typed_value < typed_minimum || typed_value > typed_maximum) {
            throw std::out_of_range("filter parameter '" + parameter + "' is outside its SDK range");
        }
        effective_value = static_cast<long double>(typed_value);
    } else if (value < schema.minimum || value > schema.maximum) {
        throw std::out_of_range("filter parameter '" + parameter + "' is outside its SDK range");
    }
    if (schema.type == FilterValueType::Boolean && value != 0.0 && value != 1.0) {
        throw std::out_of_range("boolean filter parameter '" + parameter + "' accepts only 0 or 1");
    }
    if (schema.type == FilterValueType::Integer && std::trunc(value) != value) {
        throw std::out_of_range("integer filter parameter '" + parameter + "' must be a whole number");
    }
    if (schema.step == 0.0) {
        return;
    }

    const auto units = (effective_value - static_cast<long double>(schema.minimum))
                       / static_cast<long double>(schema.step);
    const auto nearest = std::round(units);
    const auto aligned = static_cast<long double>(schema.minimum)
                         + nearest * static_cast<long double>(schema.step);
    const auto double_roundoff = 32.0L * std::numeric_limits<double>::epsilon()
                                 * std::max({ 1.0L, std::abs(effective_value),
                                              std::abs(static_cast<long double>(schema.minimum)),
                                              std::abs(aligned) });
    const auto float_roundoff = std::abs(nearest) * float32_ulp(schema.step) + float32_ulp(aligned);
    const auto tolerance = schema.type == FilterValueType::Floating
                               ? std::min(std::max(double_roundoff, float_roundoff),
                                          static_cast<long double>(schema.step) * 1.0e-3L)
                               : 0.0L;
    if (std::abs(effective_value - aligned) > tolerance) {
        throw std::out_of_range("filter parameter '" + parameter + "' does not match its SDK step");
    }
}

inline void validate_filter_parameters(const std::vector<FilterParameter>& parameters,
                                       const std::vector<FilterParameterSchema>& schemas) {
    std::vector<std::string_view> supplied;
    supplied.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        if (std::find(supplied.begin(), supplied.end(), parameter.name) != supplied.end()) {
            throw std::invalid_argument("filter parameter '" + parameter.name + "' was specified more than once");
        }
        supplied.push_back(parameter.name);
        const auto schema = std::find_if(schemas.begin(), schemas.end(), [&](const auto& item) {
            return item.name == parameter.name;
        });
        if (schema == schemas.end()) {
            throw std::invalid_argument("unknown filter parameter '" + parameter.name + "'");
        }
        validate_filter_parameter_value(parameter.name, parameter.value, *schema);
    }
}

} // namespace rbt_orbbec::detail
