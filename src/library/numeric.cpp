#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>

#include <cmath>
#include <cstdint>
#include <optional>

namespace bg::operators::details {
namespace {

// Unsigned arithmetic followed by C++20 signed conversion defines modulo-2^64 wrapping.
i64 wrapping_add(i64 lhs, i64 rhs) noexcept {
    return static_cast<i64>(static_cast<std::uint64_t>(lhs) + static_cast<std::uint64_t>(rhs));
}

void register_addition() {
    add.overload(node([](double lhs, double rhs) { return lhs + rhs; }));
    add.overload(node([](i64 lhs, i64 rhs) { return wrapping_add(lhs, rhs); }));
    add.overload(node([](timedelta_t lhs, timedelta_t rhs) {
        return timedelta_t{wrapping_add(lhs.count(), rhs.count())};
    }));

    add.overload(node([](std::optional<double> lhs, std::optional<double> rhs) {
        return lhs.value_or(0.0) + rhs.value_or(0.0);
    }));
    add.overload(node([](std::optional<i64> lhs, std::optional<i64> rhs) {
        return wrapping_add(lhs.value_or(0), rhs.value_or(0));
    }));
    add.overload(node([](std::optional<timedelta_t> lhs, std::optional<timedelta_t> rhs) {
        return timedelta_t{wrapping_add(lhs.value_or(timedelta_t{}).count(), rhs.value_or(timedelta_t{}).count())};
    }));

    add.overload(node([](date_time_t timestamp, timedelta_t duration) {
        return date_time_t{timedelta_t{wrapping_add(timestamp.time_since_epoch().count(), duration.count())}};
    }));
    add.overload(node([](timedelta_t duration, date_time_t timestamp) {
        return date_time_t{timedelta_t{wrapping_add(timestamp.time_since_epoch().count(), duration.count())}};
    }));
}

i64 wrapping_subtract(i64 lhs, i64 rhs) noexcept {
    return static_cast<i64>(static_cast<std::uint64_t>(lhs) - static_cast<std::uint64_t>(rhs));
}

void register_subtraction() {
    sub.overload(node([](i64 lhs, i64 rhs) { return wrapping_subtract(lhs, rhs); }));
    sub.overload(node([](double lhs, double rhs) { return lhs - rhs; }));
    sub.overload(node([](timedelta_t lhs, timedelta_t rhs) {
        return timedelta_t{wrapping_subtract(lhs.count(), rhs.count())};
    }));
    sub.overload(node([](date_time_t timestamp, timedelta_t duration) {
        return date_time_t{timedelta_t{wrapping_subtract(timestamp.time_since_epoch().count(), duration.count())}};
    }));
    sub.overload(node([](date_time_t lhs, date_time_t rhs) {
        return timedelta_t{wrapping_subtract(lhs.time_since_epoch().count(), rhs.time_since_epoch().count())};
    }));
}

i64 wrapping_multiply(i64 lhs, i64 rhs) noexcept {
    return static_cast<i64>(static_cast<std::uint64_t>(lhs) * static_cast<std::uint64_t>(rhs));
}

void register_multiplication() {
    mul.overload(node([](i64 lhs, i64 rhs) { return wrapping_multiply(lhs, rhs); }));
    mul.overload(node([](double lhs, double rhs) { return lhs * rhs; }));
    mul.overload(node([](std::optional<i64> lhs, std::optional<i64> rhs) {
        return wrapping_multiply(lhs.value_or(1), rhs.value_or(1));
    }));
    mul.overload(node([](std::optional<double> lhs, std::optional<double> rhs) {
        return lhs.value_or(1.0) * rhs.value_or(1.0);
    }));
}

void register_positive() {
    pos.overload(node([](i64 value) { return value; }));
    pos.overload(node([](double value) { return value; }));
    pos.overload(node([](timedelta_t value) { return value; }));
}

i64 wrapping_negate(i64 value) noexcept {
    return static_cast<i64>(std::uint64_t{} - static_cast<std::uint64_t>(value));
}

void register_negative() {
    neg.overload(node([](i64 value) { return wrapping_negate(value); }));
    neg.overload(node([](double value) { return -value; }));
    neg.overload(node([](timedelta_t value) { return timedelta_t{wrapping_negate(value.count())}; }));
}

void register_absolute() {
    abs.overload(node([](i64 value) { return value < 0 ? wrapping_negate(value) : value; }));
    abs.overload(node([](double value) { return std::abs(value); }));
    abs.overload(node([](timedelta_t value) {
        const auto count = value.count();
        return timedelta_t{count < 0 ? wrapping_negate(count) : count};
    }));
}

void register_equality() {
    eq.overload(node([](i64 lhs, i64 rhs) { return lhs == rhs; }));
    eq.overload(node([](double lhs, double rhs) { return lhs == rhs; }));
    eq.overload(node([](date_time_t lhs, date_time_t rhs) { return lhs == rhs; }));
    eq.overload(node([](timedelta_t lhs, timedelta_t rhs) { return lhs == rhs; }));
}

void register_inequality() {
    ne.overload(node([](i64 lhs, i64 rhs) { return lhs != rhs; }));
    ne.overload(node([](double lhs, double rhs) { return lhs != rhs; }));
    ne.overload(node([](date_time_t lhs, date_time_t rhs) { return lhs != rhs; }));
    ne.overload(node([](timedelta_t lhs, timedelta_t rhs) { return lhs != rhs; }));
}

void register_less_than() {
    lt.overload(node([](i64 lhs, i64 rhs) { return lhs < rhs; }));
    lt.overload(node([](double lhs, double rhs) { return lhs < rhs; }));
    lt.overload(node([](date_time_t lhs, date_time_t rhs) { return lhs < rhs; }));
    lt.overload(node([](timedelta_t lhs, timedelta_t rhs) { return lhs < rhs; }));
}

void register_less_equal() {
    le.overload(node([](i64 lhs, i64 rhs) { return lhs <= rhs; }));
    le.overload(node([](double lhs, double rhs) { return lhs <= rhs; }));
    le.overload(node([](date_time_t lhs, date_time_t rhs) { return lhs <= rhs; }));
    le.overload(node([](timedelta_t lhs, timedelta_t rhs) { return lhs <= rhs; }));
}

void register_greater_than() {
    gt.overload(node([](i64 lhs, i64 rhs) { return lhs > rhs; }));
    gt.overload(node([](double lhs, double rhs) { return lhs > rhs; }));
    gt.overload(node([](date_time_t lhs, date_time_t rhs) { return lhs > rhs; }));
    gt.overload(node([](timedelta_t lhs, timedelta_t rhs) { return lhs > rhs; }));
}

void register_greater_equal() {
    ge.overload(node([](i64 lhs, i64 rhs) { return lhs >= rhs; }));
    ge.overload(node([](double lhs, double rhs) { return lhs >= rhs; }));
    ge.overload(node([](date_time_t lhs, date_time_t rhs) { return lhs >= rhs; }));
    ge.overload(node([](timedelta_t lhs, timedelta_t rhs) { return lhs >= rhs; }));
}

} // namespace

void register_numeric_operators() {
    register_addition();
    register_subtraction();
    register_multiplication();
    register_positive();
    register_negative();
    register_absolute();
    register_equality();
    register_inequality();
    register_less_than();
    register_less_equal();
    register_greater_than();
    register_greater_equal();
}

} // namespace bg::operators::details
