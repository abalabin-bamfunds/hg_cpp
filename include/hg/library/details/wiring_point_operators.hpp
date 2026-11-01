#pragma once

#include <bg/library/operators.hpp>

#include <bg/graph/wiring_argument.hpp>
#include <utility>

namespace bg {
namespace details {

template <typename T>
inline constexpr bool wiring_point_operand = is_typed_wiring_point<std::remove_cvref_t<T>>;

template <typename T>
inline constexpr bool scalar_wiring_operand = supported_scalar_argument<T> || supported_scalar_value_argument<T>;

template <typename LHS, typename RHS>
concept library_binary_wiring_operands =
    (wiring_point_operand<LHS> || wiring_point_operand<RHS>) &&
    (wiring_point_operand<LHS> || scalar_wiring_operand<LHS>) &&
    (wiring_point_operand<RHS> || scalar_wiring_operand<RHS>);

} // namespace details

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator+(LHS &&lhs, RHS &&rhs) {
    return operators::add(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator-(LHS &&lhs, RHS &&rhs) {
    return operators::sub(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator|(LHS &&lhs, RHS &&rhs) {
    return operators::bit_or(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator&(LHS &&lhs, RHS &&rhs) {
    return operators::bit_and(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator^(LHS &&lhs, RHS &&rhs) {
    return operators::bit_xor(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator*(LHS &&lhs, RHS &&rhs) {
    return operators::mul(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator/(LHS &&lhs, RHS &&rhs) {
    return operators::div(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator%(LHS &&lhs, RHS &&rhs) {
    return operators::mod(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator==(LHS &&lhs, RHS &&rhs) {
    return operators::eq(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator!=(LHS &&lhs, RHS &&rhs) {
    return operators::ne(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator<(LHS &&lhs, RHS &&rhs) {
    return operators::lt(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator<=(LHS &&lhs, RHS &&rhs) {
    return operators::le(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator>(LHS &&lhs, RHS &&rhs) {
    return operators::gt(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator>=(LHS &&lhs, RHS &&rhs) {
    return operators::ge(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename POINT>
    requires details::wiring_point_operand<POINT>
auto operator+(POINT &&value) {
    return operators::pos(std::forward<POINT>(value));
}

template <typename POINT>
    requires details::wiring_point_operand<POINT>
auto operator-(POINT &&value) {
    return operators::neg(std::forward<POINT>(value));
}

template <typename POINT>
    requires details::wiring_point_operand<POINT>
auto operator~(POINT &&value) {
    return operators::invert(std::forward<POINT>(value));
}

template <typename POINT>
    requires details::wiring_point_operand<POINT>
auto operator!(POINT &&value) {
    return operators::not_(std::forward<POINT>(value));
}

// Logical wiring operators wire both operands. They do not short-circuit.
template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator&&(LHS &&lhs, RHS &&rhs) {
    return operators::and_(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename LHS, typename RHS>
    requires details::library_binary_wiring_operands<LHS, RHS>
auto operator||(LHS &&lhs, RHS &&rhs) {
    return operators::or_(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
}

template <typename POINT>
    requires details::wiring_point_operand<POINT>
auto abs(POINT &&value) {
    return operators::abs(std::forward<POINT>(value));
}

} // namespace bg
