#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <algorithm>
#include <array>
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

i64 wrapping_subtract(i64 lhs, i64 rhs) noexcept {
    return static_cast<i64>(static_cast<std::uint64_t>(lhs) - static_cast<std::uint64_t>(rhs));
}

i64 wrapping_multiply(i64 lhs, i64 rhs) noexcept {
    return static_cast<i64>(static_cast<std::uint64_t>(lhs) * static_cast<std::uint64_t>(rhs));
}

i64 wrapping_negate(i64 value) noexcept {
    return static_cast<i64>(std::uint64_t{} - static_cast<std::uint64_t>(value));
}

struct arithmetic_overloads {
    // Addition.
    overload<add> add_double{[](double lhs, double rhs) { return lhs + rhs; }};
    overload<add> add_i64{[](i64 lhs, i64 rhs) { return wrapping_add(lhs, rhs); }};
    overload<add> add_timedelta{[](timedelta_t lhs, timedelta_t rhs) {
        return timedelta_t{wrapping_add(lhs.count(), rhs.count())};
    }};
    overload<add> add_optional_double{[](std::optional<double> lhs, std::optional<double> rhs) {
        return lhs.value_or(0.0) + rhs.value_or(0.0);
    }};
    overload<add> add_optional_i64{[](std::optional<i64> lhs, std::optional<i64> rhs) {
        return wrapping_add(lhs.value_or(0), rhs.value_or(0));
    }};
    overload<add> add_optional_timedelta{[](std::optional<timedelta_t> lhs, std::optional<timedelta_t> rhs) {
        return timedelta_t{wrapping_add(lhs.value_or(timedelta_t{}).count(), rhs.value_or(timedelta_t{}).count())};
    }};
    overload<add> add_date_time_timedelta{[](date_time_t timestamp, timedelta_t duration) {
        return date_time_t{timedelta_t{wrapping_add(timestamp.time_since_epoch().count(), duration.count())}};
    }};
    overload<add> add_timedelta_date_time{[](timedelta_t duration, date_time_t timestamp) {
        return date_time_t{timedelta_t{wrapping_add(timestamp.time_since_epoch().count(), duration.count())}};
    }};

    // Subtraction.
    overload<sub> sub_i64{[](i64 lhs, i64 rhs) { return wrapping_subtract(lhs, rhs); }};
    overload<sub> sub_double{[](double lhs, double rhs) { return lhs - rhs; }};
    overload<sub> sub_timedelta{[](timedelta_t lhs, timedelta_t rhs) {
        return timedelta_t{wrapping_subtract(lhs.count(), rhs.count())};
    }};
    overload<sub> sub_date_time_timedelta{[](date_time_t timestamp, timedelta_t duration) {
        return date_time_t{timedelta_t{wrapping_subtract(timestamp.time_since_epoch().count(), duration.count())}};
    }};
    overload<sub> sub_date_time{[](date_time_t lhs, date_time_t rhs) {
        return timedelta_t{wrapping_subtract(lhs.time_since_epoch().count(), rhs.time_since_epoch().count())};
    }};

    // Multiplication.
    overload<mul> mul_i64{[](i64 lhs, i64 rhs) { return wrapping_multiply(lhs, rhs); }};
    overload<mul> mul_double{[](double lhs, double rhs) { return lhs * rhs; }};
    overload<mul> mul_optional_i64{[](std::optional<i64> lhs, std::optional<i64> rhs) {
        return wrapping_multiply(lhs.value_or(1), rhs.value_or(1));
    }};
    overload<mul> mul_optional_double{[](std::optional<double> lhs, std::optional<double> rhs) {
        return lhs.value_or(1.0) * rhs.value_or(1.0);
    }};

    // Division.
    overload<div> div_i64{[](i64 lhs, i64 rhs) {
        if (rhs == 0) {
            throw std::domain_error{"division by zero"};
        }
        return static_cast<double>(lhs) / static_cast<double>(rhs);
    }};
    overload<div> div_double{[](double lhs, double rhs) {
        if (rhs == 0.0) {
            throw std::domain_error{"division by zero"};
        }
        return lhs / rhs;
    }};

    // Floor division.
    static i64 integer_floor_division(i64 lhs, i64 rhs) {
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

    static double float_floor_division(double lhs, double rhs) {
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

    overload<floordiv> floordiv_i64{[](i64 lhs, i64 rhs) { return integer_floor_division(lhs, rhs); }};
    overload<floordiv> floordiv_double{[](double lhs, double rhs) { return float_floor_division(lhs, rhs); }};

    // Modulo.
    static i64 integer_modulo(i64 lhs, i64 rhs) {
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

    static double float_modulo(double lhs, double rhs) {
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

    overload<mod> mod_i64{[](i64 lhs, i64 rhs) { return integer_modulo(lhs, rhs); }};
    overload<mod> mod_double{[](double lhs, double rhs) { return float_modulo(lhs, rhs); }};

    // Power.
    static i64 integer_power(i64 lhs, i64 rhs) {
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

    static double floating_power(double lhs, double rhs) {
        if (lhs == 0.0 && rhs < 0.0) {
            throw std::domain_error{"zero cannot be raised to a negative power"};
        }
        if (lhs < 0.0 && std::isfinite(rhs) && std::trunc(rhs) != rhs) {
            throw std::domain_error{"a negative base with a fractional exponent has a complex result"};
        }
        return std::pow(lhs, rhs);
    }

    overload<pow> pow_i64{[](i64 lhs, i64 rhs) { return integer_power(lhs, rhs); }};
    overload<pow> pow_double{[](double lhs, double rhs) { return floating_power(lhs, rhs); }};

};

struct transform_overloads {
    // Rounding.
    static constexpr auto max_round_decimal_places = 323;
    static constexpr auto round_positive_buffer_size = static_cast<std::size_t>(
        std::numeric_limits<double>::max_exponent10 + max_round_decimal_places + 3);
    static constexpr auto round_integer_buffer_size =
        static_cast<std::size_t>(std::numeric_limits<double>::max_exponent10 + 1);

    template <std::size_t SIZE>
    static std::string_view format_fixed(double value, int precision, std::array<char, SIZE> &buffer) {
        const auto [end, error] =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, precision);
        if (error != std::errc{}) {
            throw std::runtime_error{"failed to format a rounded floating-point value"};
        }
        return {buffer.data(), static_cast<std::size_t>(end - buffer.data())};
    }

    static double parse_rounded(std::string_view text) {
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

    static double round_decimal(double value, i64 ndigits) {
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
                               (has_nonzero_tail || (retained_digits != 0 &&
                                                     (digits[retained_digits - 1] - '0') % 2 != 0)));

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

    overload<round> round_double{[](double value) { return round_decimal(value, 0); }};
    overload<round> round_double_precision{[](double value, i64 ndigits) { return round_decimal(value, ndigits); }};

    // Conversion.
    static RequirementResult result_matches(const WiringArguments &arguments) {
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
    }
    overload<convert> convert_i64{eval = [](i64 value) { return value; }, require = result_matches};
    overload<convert> convert_i64_to_double{eval = [](i64 value) { return static_cast<double>(value); },
                                            require = result_matches};
    overload<convert> convert_double_to_i64{
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
        require = result_matches};
    overload<convert> convert_double{eval = [](double value) { return value; }, require = result_matches};

    // Positive.
    overload<pos> pos_i64{[](i64 value) { return value; }};
    overload<pos> pos_double{[](double value) { return value; }};
    overload<pos> pos_timedelta{[](timedelta_t value) { return value; }};

    // Negative.
    overload<neg> neg_i64{[](i64 value) { return wrapping_negate(value); }};
    overload<neg> neg_double{[](double value) { return -value; }};
    overload<neg> neg_timedelta{[](timedelta_t value) { return timedelta_t{wrapping_negate(value.count())}; }};

    // Absolute value.
    overload<abs> abs_i64{[](i64 value) { return value < 0 ? wrapping_negate(value) : value; }};
    overload<abs> abs_double{[](double value) { return std::abs(value); }};
    overload<abs> abs_timedelta{[](timedelta_t value) {
        const auto count = value.count();
        return timedelta_t{count < 0 ? wrapping_negate(count) : count};
    }};

};

struct comparison_overloads {
    // Equality.
    overload<eq> eq_i64{[](i64 lhs, i64 rhs) { return lhs == rhs; }};
    overload<eq> eq_double{[](double lhs, double rhs) { return lhs == rhs; }};
    overload<eq> eq_date_time{[](date_time_t lhs, date_time_t rhs) { return lhs == rhs; }};
    overload<eq> eq_timedelta{[](timedelta_t lhs, timedelta_t rhs) { return lhs == rhs; }};

    // Inequality.
    overload<ne> ne_i64{[](i64 lhs, i64 rhs) { return lhs != rhs; }};
    overload<ne> ne_double{[](double lhs, double rhs) { return lhs != rhs; }};
    overload<ne> ne_date_time{[](date_time_t lhs, date_time_t rhs) { return lhs != rhs; }};
    overload<ne> ne_timedelta{[](timedelta_t lhs, timedelta_t rhs) { return lhs != rhs; }};

    // Ordering.
    overload<lt> lt_i64{[](i64 lhs, i64 rhs) { return lhs < rhs; }};
    overload<lt> lt_double{[](double lhs, double rhs) { return lhs < rhs; }};
    overload<lt> lt_date_time{[](date_time_t lhs, date_time_t rhs) { return lhs < rhs; }};
    overload<lt> lt_timedelta{[](timedelta_t lhs, timedelta_t rhs) { return lhs < rhs; }};

    overload<le> le_i64{[](i64 lhs, i64 rhs) { return lhs <= rhs; }};
    overload<le> le_double{[](double lhs, double rhs) { return lhs <= rhs; }};
    overload<le> le_date_time{[](date_time_t lhs, date_time_t rhs) { return lhs <= rhs; }};
    overload<le> le_timedelta{[](timedelta_t lhs, timedelta_t rhs) { return lhs <= rhs; }};

    overload<gt> gt_i64{[](i64 lhs, i64 rhs) { return lhs > rhs; }};
    overload<gt> gt_double{[](double lhs, double rhs) { return lhs > rhs; }};
    overload<gt> gt_date_time{[](date_time_t lhs, date_time_t rhs) { return lhs > rhs; }};
    overload<gt> gt_timedelta{[](timedelta_t lhs, timedelta_t rhs) { return lhs > rhs; }};

    overload<ge> ge_i64{[](i64 lhs, i64 rhs) { return lhs >= rhs; }};
    overload<ge> ge_double{[](double lhs, double rhs) { return lhs >= rhs; }};
    overload<ge> ge_date_time{[](date_time_t lhs, date_time_t rhs) { return lhs >= rhs; }};
    overload<ge> ge_timedelta{[](timedelta_t lhs, timedelta_t rhs) { return lhs >= rhs; }};

};

} // namespace

void register_numeric_operators() {
    register_overloads<arithmetic_overloads>();
    register_overloads<transform_overloads>();
    register_overloads<comparison_overloads>();
}

} // namespace bg::operators::details
