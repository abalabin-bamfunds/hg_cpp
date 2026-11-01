#pragma once

#include <hg/scalar/format_operations.hpp>

namespace fmt {

template <typename T> struct formatter<hg::Scalar<T>> {
    constexpr auto parse(format_parse_context &context) {
        const auto it = context.begin();
        if (it != context.end() && *it != '}') {
            throw format_error{"scalar formatting does not accept format specifiers"};
        }
        return it;
    }

    auto format(hg::Scalar<T> value, format_context &context) const {
        hg::format_scalar(hg::Scalar<>::from(value), context);
        return context.out();
    }
};

} // namespace fmt
