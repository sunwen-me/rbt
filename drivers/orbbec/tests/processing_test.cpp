#ifdef NDEBUG
#undef NDEBUG
#endif

#include <rbt_orbbec/detail/filter_validation.hpp>

#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <typename Error, typename Fn> void require_throws(Fn&& fn) {
    bool threw = false;
    try {
        fn();
    } catch (const Error&) {
        threw = true;
    }
    assert(threw);
}

} // namespace

int main() {
    using rbt_orbbec::FilterParameter;
    using rbt_orbbec::detail::FilterParameterSchema;
    using rbt_orbbec::detail::FilterValueType;
    using rbt_orbbec::detail::validate_filter_parameters;

    const std::vector<FilterParameterSchema> schema {
        { "integer", FilterValueType::Integer, 2.0, 10.0, 2.0 },
        { "ratio", FilterValueType::Floating, 0.0, 1.0, 0.1 },
        { "enabled", FilterValueType::Boolean, 0.0, 1.0, 1.0 },
    };
    const std::vector<FilterParameterSchema> float32_schema {
        { "ratio", FilterValueType::Floating, 0.0, 1.0, static_cast<double>(0.01F) },
        { "quarter", FilterValueType::Floating, 0.0, 1.0, 0.25 },
        { "bounded", FilterValueType::Floating, static_cast<double>(0.1F), static_cast<double>(0.9F),
          static_cast<double>(0.1F) },
    };

    validate_filter_parameters({ FilterParameter { "integer", 6.0 },
                                 FilterParameter { "ratio", 0.3 },
                                 FilterParameter { "enabled", 1.0 } },
                               schema);
    validate_filter_parameters({ FilterParameter { "ratio", 0.1 } }, float32_schema);
    validate_filter_parameters({ FilterParameter { "bounded", 0.1 } }, float32_schema);
    validate_filter_parameters({ FilterParameter { "bounded", 0.9 } }, float32_schema);
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "ratio", 0.105 } }, float32_schema);
    });
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "bounded", 0.0999999 } }, float32_schema);
    });
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "bounded", 0.900001 } }, float32_schema);
    });
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "quarter", 0.6 } }, float32_schema);
    });
    require_throws<std::invalid_argument>([&] {
        validate_filter_parameters({ FilterParameter { "missing", 1.0 } }, schema);
    });
    require_throws<std::invalid_argument>([&] {
        validate_filter_parameters({ FilterParameter { "ratio", 0.5 },
                                     FilterParameter { "ratio", 1.0 } }, schema);
    });
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "integer", 5.0 } }, schema);
    });
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "ratio", 0.61 } }, schema);
    });
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "integer", 0.0 } }, schema);
    });
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "integer", 12.0 } }, schema);
    });
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "ratio", -0.1 } }, schema);
    });
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "ratio", 1.1 } }, schema);
    });
    require_throws<std::out_of_range>([&] {
        validate_filter_parameters({ FilterParameter { "enabled", 0.5 } }, schema);
    });
    require_throws<std::invalid_argument>([&] {
        validate_filter_parameters({ FilterParameter { "ratio", 0.0 / 0.0 } }, schema);
    });
}
