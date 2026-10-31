#pragma once

#include <bg/scalar/format_operations.hpp>

namespace fmt {

template <typename T> struct formatter<bg::Scalar<T>> {
    constexpr auto parse(format_parse_context &context) {
        const auto it = context.begin();
        if (it != context.end() && *it != '}') {
            throw format_error{"scalar formatting does not accept format specifiers"};
        }
        return it;
    }

    auto format(bg::Scalar<T> value, format_context &context) const {
        bg::format_scalar(bg::Scalar<>::from(value), context);
        return context.out();
    }
};

} // namespace fmt
