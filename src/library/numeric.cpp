#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

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

double integer_division(i64 lhs, i64 rhs) {
    if (rhs == 0) {
        throw std::domain_error{"division by zero"};
    }

    const auto negative = (lhs < 0) != (rhs < 0);
    if (lhs == 0) {
        return std::copysign(0.0, negative ? -1.0 : 1.0);
    }

    const auto lhs_magnitude = lhs < 0 ? std::uint64_t{} - static_cast<std::uint64_t>(lhs)
                                       : static_cast<std::uint64_t>(lhs);
    const auto rhs_magnitude = rhs < 0 ? std::uint64_t{} - static_cast<std::uint64_t>(rhs)
                                       : static_cast<std::uint64_t>(rhs);
    constexpr auto exact_integer_limit = std::uint64_t{1} << 53;
    // Both casts are exact in this range, so hardware division already rounds the exact quotient.
    if (lhs_magnitude <= exact_integer_limit && rhs_magnitude <= exact_integer_limit) {
        return static_cast<double>(lhs) / static_cast<double>(rhs);
    }

    // Scale the positive ratio into [1, 2), retaining its power-of-two exponent.
    auto exponent = static_cast<int>(std::bit_width(lhs_magnitude)) -
                    static_cast<int>(std::bit_width(rhs_magnitude));
    if (exponent >= 0) {
        if (lhs_magnitude < (rhs_magnitude << exponent)) {
            --exponent;
        }
    } else if ((lhs_magnitude << -exponent) < rhs_magnitude) {
        --exponent;
    }

    auto scaled_numerator = lhs_magnitude;
    auto scaled_denominator = rhs_magnitude;
    if (exponent >= 0) {
        scaled_denominator <<= exponent;
    } else {
        scaled_numerator <<= -exponent;
    }

    auto remainder = scaled_numerator - scaled_denominator;
    auto significand = std::uint64_t{1};
    // Generate the 52 fraction bits without overflowing the 64-bit remainder.
    for (auto bit = 0; bit < 52; ++bit) {
        significand <<= 1;
        if (remainder >= scaled_denominator - remainder) {
            remainder -= scaled_denominator - remainder;
            significand |= 1;
        } else {
            remainder += remainder;
        }
    }

    // The exact remainder decides nearest rounding and ties-to-even.
    if (remainder > scaled_denominator - remainder ||
        (remainder == scaled_denominator - remainder && (significand & 1U) != 0)) {
        ++significand;
    }

    const auto result = std::ldexp(static_cast<double>(significand), exponent - 52);
    return std::copysign(result, negative ? -1.0 : 1.0);
}

double float_division(double lhs, double rhs) {
    if (rhs == 0.0) {
        throw std::domain_error{"division by zero"};
    }
    return lhs / rhs;
}

void register_division() {
    div.overload(node([](i64 lhs, i64 rhs) { return integer_division(lhs, rhs); }));
    div.overload(node([](double lhs, double rhs) { return float_division(lhs, rhs); }));
}

i64 integer_floor_division(i64 lhs, i64 rhs) {
    if (rhs == 0) {
        throw std::domain_error{"division by zero"};
    }
    if (lhs == std::numeric_limits<i64>::min() && rhs == -1) {
        return std::numeric_limits<i64>::min();
    }

    auto quotient = lhs / rhs;
    const auto remainder = lhs % rhs;
    if (remainder != 0 && (remainder < 0) != (rhs < 0)) {
        --quotient;
    }
    return quotient;
}

double float_floor_division(double lhs, double rhs) {
    if (rhs == 0.0) {
        throw std::domain_error{"division by zero"};
    }

    auto remainder = std::fmod(lhs, rhs);
    auto quotient = (lhs - remainder) / rhs;
    if (remainder != 0.0) {
        if (std::signbit(remainder) != std::signbit(rhs)) {
            quotient -= 1.0;
        }
    }

    if (quotient == 0.0) {
        return std::copysign(0.0, lhs / rhs);
    }

    auto result = std::floor(quotient);
    if (quotient - result > 0.5) {
        result += 1.0;
    }
    return result;
}

void register_floor_division() {
    floordiv.overload(node([](i64 lhs, i64 rhs) { return integer_floor_division(lhs, rhs); }));
    floordiv.overload(node([](double lhs, double rhs) { return float_floor_division(lhs, rhs); }));
}

i64 integer_modulo(i64 lhs, i64 rhs) {
    if (rhs == 0) {
        throw std::domain_error{"modulo by zero"};
    }
    if (lhs == std::numeric_limits<i64>::min() && rhs == -1) {
        return 0;
    }

    auto remainder = lhs % rhs;
    if (remainder != 0 && (remainder < 0) != (rhs < 0)) {
        remainder += rhs;
    }
    return remainder;
}

double float_modulo(double lhs, double rhs) {
    if (rhs == 0.0) {
        throw std::domain_error{"modulo by zero"};
    }

    auto remainder = std::fmod(lhs, rhs);
    if (remainder != 0.0) {
        if (std::signbit(remainder) != std::signbit(rhs)) {
            remainder += rhs;
        }
    } else {
        remainder = std::copysign(0.0, rhs);
    }
    return remainder;
}

void register_modulo() {
    mod.overload(node([](i64 lhs, i64 rhs) { return integer_modulo(lhs, rhs); }));
    mod.overload(node([](double lhs, double rhs) { return float_modulo(lhs, rhs); }));
}

constexpr auto max_round_decimal_places = 323;
constexpr auto round_positive_buffer_size = static_cast<std::size_t>(
    std::numeric_limits<double>::max_exponent10 + max_round_decimal_places + 3);
constexpr auto round_integer_buffer_size =
    static_cast<std::size_t>(std::numeric_limits<double>::max_exponent10 + 1);

template <std::size_t SIZE>
std::string_view format_fixed(double value, int precision, std::array<char, SIZE> &buffer) {
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, precision);
    if (error != std::errc{}) {
        throw std::runtime_error{"failed to format a rounded floating-point value"};
    }
    return {buffer.data(), static_cast<std::size_t>(end - buffer.data())};
}

double parse_rounded(std::string_view text) {
    auto rounded = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), rounded);
    if (error == std::errc::result_out_of_range) {
        throw std::overflow_error{"rounded result is outside the double range"};
    }
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error{"failed to parse a rounded floating-point value"};
    }
    return rounded;
}

double round_decimal(double value, i64 ndigits) {
    if (!std::isfinite(value) || value == 0.0) {
        return value;
    }
    if (ndigits > max_round_decimal_places) {
        return value;
    }

    if (ndigits >= 0) {
        auto buffer = std::array<char, round_positive_buffer_size>{};
        return parse_rounded(format_fixed(value, static_cast<int>(ndigits), buffer));
    }

    const auto magnitude = std::abs(value);
    const auto integer = std::trunc(magnitude);
    if (integer == 0.0) {
        return std::copysign(0.0, value);
    }

    auto buffer = std::array<char, round_integer_buffer_size>{};
    auto digits = std::string{format_fixed(integer, 0, buffer)};
    const auto significant_integer_size = digits.size();
    if (ndigits < -static_cast<i64>(significant_integer_size)) {
        return std::copysign(0.0, value);
    }

    const auto discarded_digits = static_cast<std::size_t>(-ndigits);
    const auto retained_digits = significant_integer_size - discarded_digits;
    const auto rounding_digit = digits[retained_digits];
    const auto has_nonzero_tail = std::ranges::any_of(
        std::string_view{digits}.substr(retained_digits + 1), [](char digit) { return digit != '0'; }) ||
                                 magnitude != integer;
    const auto round_up = rounding_digit > '5' ||
                          (rounding_digit == '5' &&
                           (has_nonzero_tail || (retained_digits != 0 && (digits[retained_digits - 1] - '0') % 2 != 0)));

    auto result = retained_digits == 0 ? std::string{"0"} : digits.substr(0, retained_digits);
    if (round_up && retained_digits != 0) {
        auto position = retained_digits;
        while (position != 0 && result[position - 1] == '9') {
            result[position - 1] = '0';
            --position;
        }
        if (position == 0) {
            result.insert(result.begin(), '1');
        } else {
            ++result[position - 1];
        }
    } else if (round_up) {
        result = "1";
    }
    result.append(discarded_digits, '0');
    if (result.find_first_not_of('0') == std::string::npos) {
        return std::copysign(0.0, value);
    }
    if (std::signbit(value)) {
        result.insert(result.begin(), '-');
    }
    return parse_rounded(result);
}

void register_rounding() {
    round.overload(node([](double value) { return round_decimal(value, 0); }));
    round.overload(node([](double value, i64 ndigits) { return round_decimal(value, ndigits); }));
}

i64 integer_power(i64 lhs, i64 rhs) {
    if (rhs < 0) {
        throw std::domain_error{"an integer exponent must be nonnegative"};
    }

    auto result = std::uint64_t{1};
    auto factor = static_cast<std::uint64_t>(lhs);
    auto exponent = static_cast<std::uint64_t>(rhs);
    while (exponent != 0) {
        if ((exponent & 1U) != 0) {
            result *= factor;
        }
        exponent >>= 1U;
        if (exponent != 0) {
            factor *= factor;
        }
    }
    return static_cast<i64>(result);
}

double floating_power(double lhs, double rhs) {
    if (lhs == 0.0 && rhs < 0.0) {
        throw std::domain_error{"zero cannot be raised to a negative power"};
    }
    if (lhs < 0.0 && std::isfinite(rhs) && std::trunc(rhs) != rhs) {
        throw std::domain_error{"a negative base with a fractional exponent has a complex result"};
    }
    return std::pow(lhs, rhs);
}

void register_power() {
    pow.overload(node([](i64 lhs, i64 rhs) { return integer_power(lhs, rhs); }));
    pow.overload(node([](double lhs, double rhs) { return floating_power(lhs, rhs); }));
}

void register_conversion() {
    const auto result_matches = [](const WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(::bg::details::type_parameter<CONVERT_RESULT>());
        const auto *target_mapping = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
        const auto *target = target_mapping == nullptr || *target_mapping == nullptr
                                 ? nullptr
                                 : type_cast<TimeSeriesType>(*target_mapping);
        if (target == nullptr) {
            return reject("convert requires an explicit resolved time-series result type");
        }

        if (!target->accepts(*arguments.resolved_output_type())) {
            return reject("the requested conversion result type does not match this overload");
        }
        return accept;
    };

    convert.overload(node(
        eval = [](i64 value) { return value; },
        require = result_matches));
    convert.overload(node(
        eval = [](i64 value) { return static_cast<double>(value); },
        require = result_matches));
    convert.overload(node(
        eval = [](double value) {
            const auto limit = std::ldexp(1.0, 63);
            if (std::isnan(value)) {
                throw std::domain_error{"cannot convert NaN to i64"};
            }
            if (value < -limit || value >= limit) {
                throw std::overflow_error{"value is outside the i64 range"};
            }
            return static_cast<i64>(value);
        },
        require = result_matches));
    convert.overload(node(
        eval = [](double value) { return value; },
        require = result_matches));
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
    register_division();
    register_floor_division();
    register_modulo();
    register_power();
    register_rounding();
    register_conversion();
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
