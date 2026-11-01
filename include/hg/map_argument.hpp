#pragma once

#include <bg/graph/wiring_argument.hpp>

#include <type_traits>
#include <utility>

namespace bg {

template <typename T> struct pass_through_argument {
    T value;
};

template <typename T> auto pass_through(T &&value) {
    return pass_through_argument<std::decay_t<T>>{std::forward<T>(value)};
}

struct MapArgument {
    WiringArgument value;
    bool pass_through{};
};

} // namespace bg
