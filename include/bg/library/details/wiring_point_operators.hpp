#pragma once

#include <bg/library/operators.hpp>

#include <bg/graph/wiring_argument.hpp>

#include <utility>

namespace bg {

template <typename LHS, auto... LHS_PROPERTIES, typename RHS, auto... RHS_PROPERTIES>
auto operator+(wp<LHS, LHS_PROPERTIES...> lhs, wp<RHS, RHS_PROPERTIES...> rhs) {
    return operators::add(lhs, rhs);
}

template <typename TIME_SERIES, auto... PROPERTIES, typename SCALAR>
    requires(details::supported_scalar_argument<SCALAR> || details::supported_scalar_value_argument<SCALAR>)
auto operator+(wp<TIME_SERIES, PROPERTIES...> lhs, SCALAR &&rhs) {
    return operators::add(lhs, std::forward<SCALAR>(rhs));
}

template <typename SCALAR, typename TIME_SERIES, auto... PROPERTIES>
    requires(details::supported_scalar_argument<SCALAR> || details::supported_scalar_value_argument<SCALAR>)
auto operator+(SCALAR &&lhs, wp<TIME_SERIES, PROPERTIES...> rhs) {
    return operators::add(std::forward<SCALAR>(lhs), rhs);
}

} // namespace bg
