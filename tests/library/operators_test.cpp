#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/library.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

using namespace bg;
using namespace bg::literals;

namespace {

struct Quote {
    i64 quantity;
    double price;

    friend bool operator==(const Quote &, const Quote &) = default;
};

template <template <typename, auto...> typename Field> struct BooleanBundle {
    Field<bool> value;
};

template <typename T>
concept ContextuallyConvertibleToBool = requires(const T &value) { value ? 1 : 0; };

template <typename T>
concept ExplicitlyCastableToBool = requires(const T &value) { static_cast<bool>(value); };

template <typename T>
concept DirectListInitializableAsBool = requires(const T &value) { bool{value}; };

} // namespace

TEST_CASE("numeric division, floor division, and modulo follow signed arithmetic",
          "[feature:operator][series:value][scalar:i64][scalar:floating-point]") {
    const auto integer_division = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return operators::div(lhs, rhs); };
    const auto cxx_integer_division = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return lhs / rhs; };
    const auto cxx_scalar_left_division = [](wp<TS<i64>> rhs) { return i64{-7} / rhs; };
    const auto floor_division = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return operators::floordiv(lhs, rhs); };
    const auto modulo = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return operators::mod(lhs, rhs); };
    const auto cxx_modulo = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return lhs % rhs; };
    const auto cxx_scalar_left_modulo = [](wp<TS<i64>> rhs) { return i64{-7} % rhs; };
    const auto lhs = std::array<i64, 4>{-7, 7, -7, 7};
    const auto rhs = std::array<i64, 4>{3, -3, -3, 3};

    CHECK(testing::test_graph(integer_division, lhs, rhs)
              .equals<double>({-7.0 / 3.0, -7.0 / 3.0, 7.0 / 3.0, 7.0 / 3.0}));
    CHECK(testing::test_graph(cxx_integer_division, lhs, rhs)
              .equals<double>({-7.0 / 3.0, -7.0 / 3.0, 7.0 / 3.0, 7.0 / 3.0}));
    CHECK(testing::test_graph(cxx_scalar_left_division, std::array<i64, 2>{3, -3})
              .equals<double>({-7.0 / 3.0, -7.0 / -3.0}));
    CHECK(testing::test_graph(floor_division, lhs, rhs).equals<i64>({-3, -3, 2, 2}));
    CHECK(testing::test_graph(modulo, lhs, rhs).equals<i64>({2, -2, -1, 1}));
    CHECK(testing::test_graph(cxx_modulo, lhs, rhs).equals<i64>({2, -2, -1, 1}));
    CHECK(testing::test_graph(cxx_scalar_left_modulo, std::array<i64, 2>{3, -3}).equals<i64>({2, -1}));

    const auto mixed_division = [](wp<TS<i64>> value) { return operators::div(value, 2.0); };
    const auto optional_division = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return operators::div(opt(lhs), rhs); };
    const auto optional_modulo = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return operators::mod(opt(lhs), rhs); };
    CHECK_THROWS_AS(testing::test_graph(mixed_division, std::array<i64, 1>{7}), WiringError);
    CHECK_THROWS_AS(testing::test_graph(optional_division, lhs, rhs), WiringError);
    CHECK_THROWS_AS(testing::test_graph(optional_modulo, lhs, rhs), WiringError);

    const auto min_over_negative_one = [] { return operators::div(std::numeric_limits<i64>::min(), i64{-1}); };
    const auto floor_min_over_negative_one = [] {
        return operators::floordiv(std::numeric_limits<i64>::min(), i64{-1});
    };
    const auto modulo_min_over_negative_one = [] {
        return operators::mod(std::numeric_limits<i64>::min(), i64{-1});
    };
    CHECK(testing::test_graph(min_over_negative_one)
              .equals<double>({static_cast<double>(std::uint64_t{1} << 63)}));
    CHECK(testing::test_graph(floor_min_over_negative_one).equals<i64>({std::numeric_limits<i64>::min()}));
    CHECK(testing::test_graph(modulo_min_over_negative_one).equals<i64>({0}));

    const auto divide_by_zero = [] { return operators::div(i64{1}, i64{0}); };
    const auto floor_divide_by_zero = [] { return operators::floordiv(i64{1}, i64{0}); };
    const auto modulo_by_zero = [] { return operators::mod(i64{1}, i64{0}); };
    const auto divide_by_negative_zero = [] { return operators::div(1.0, -0.0); };
    const auto floor_divide_by_negative_zero = [] { return operators::floordiv(1.0, -0.0); };
    const auto modulo_by_negative_zero = [] { return operators::mod(1.0, -0.0); };
    CHECK_THROWS_AS(testing::test_graph(divide_by_zero), std::domain_error);
    CHECK_THROWS_AS(testing::test_graph(floor_divide_by_zero), std::domain_error);
    CHECK_THROWS_AS(testing::test_graph(modulo_by_zero), std::domain_error);
    CHECK_THROWS_AS(testing::test_graph(divide_by_negative_zero), std::domain_error);
    CHECK_THROWS_AS(testing::test_graph(floor_divide_by_negative_zero), std::domain_error);
    CHECK_THROWS_AS(testing::test_graph(modulo_by_negative_zero), std::domain_error);
}

TEST_CASE("floating division preserves signed zero",
          "[feature:operator][series:value][scalar:floating-point]") {
    const auto divide = [](wp<TS<double>> lhs, wp<TS<double>> rhs) { return operators::div(lhs, rhs); };
    const auto result = testing::test_graph(divide, std::array{-7.0, -0.0, 0.0}, std::array{2.0, 2.0, -2.0});

    CHECK(result.equals<double>({-3.5, -0.0, -0.0}));
    CHECK(std::signbit(scalar_cast<double>(*result[1]).value()));
    CHECK(std::signbit(scalar_cast<double>(*result[2]).value()));
}

TEST_CASE("integer division converts its operands to binary64",
          "[feature:operator][series:value][scalar:i64][scalar:floating-point]") {
    const auto divide = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return operators::div(lhs, rhs); };
    constexpr auto two_to_53 = i64{1} << 53;
    constexpr auto two_to_54 = i64{1} << 54;
    const auto lhs = std::array<i64, 9>{
        two_to_53 + 1,
        two_to_53 + 3,
        9007199254740993,
        two_to_54 - 1,
        std::numeric_limits<i64>::max(),
        std::numeric_limits<i64>::min(),
        1,
        0,
        27021597764222979,
    };
    const auto rhs = std::array<i64, 9>{
        two_to_53,
        two_to_53,
        3,
        1,
        1,
        -1,
        std::numeric_limits<i64>::max(),
        std::numeric_limits<i64>::min(),
        864691128455135232,
    };

    const auto result = testing::test_graph(divide, lhs, rhs);
    CHECK(result.equals<double>({1.0, 1.0 + std::ldexp(1.0, -51), 3002399751580330.5,
                                 std::ldexp(1.0, 54), std::ldexp(1.0, 63), std::ldexp(1.0, 63),
                                 std::ldexp(1.0, -63), -0.0, 0.03125000000000001}));
    CHECK(std::signbit(scalar_cast<double>(*result[7]).value()));
}

TEST_CASE("floating floor division preserves Python quotient and modulo rules",
          "[feature:operator][series:value][scalar:floating-point]") {
    const auto floor_division = [](wp<TS<double>> lhs, wp<TS<double>> rhs) { return operators::floordiv(lhs, rhs); };
    const auto modulo = [](wp<TS<double>> lhs, wp<TS<double>> rhs) { return operators::mod(lhs, rhs); };
    const auto lhs = std::array{1.0, -1.0, 1.0, -1.0};
    const auto rhs = std::array{0.1, 0.1, -0.1, -0.1};

    CHECK(testing::test_graph(floor_division, lhs, rhs).equals<double>({9.0, -10.0, -10.0, 9.0}));
    const auto result = testing::test_graph(modulo, lhs, rhs);
    CHECK(result.equals<double>({std::fmod(1.0, 0.1), 0.1 - std::fmod(1.0, 0.1),
                                 -0.1 + std::fmod(1.0, 0.1), -std::fmod(1.0, 0.1)}));
}

TEST_CASE("power and round preserve numeric result types and decimal rules",
          "[feature:operator][series:value][scalar:i64][scalar:floating-point]") {
    const auto integer_power = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return operators::pow(lhs, rhs); };
    const auto floating_power = [](wp<TS<double>> lhs, wp<TS<double>> rhs) { return operators::pow(lhs, rhs); };
    const auto round_two = [](wp<TS<double>> value) { return operators::round(value, 2); };
    const auto round_negative = [](wp<TS<double>> value) { return operators::round(value, -23); };
    const auto round_negative_24 = [](wp<TS<double>> value) { return operators::round(value, -24); };
    const auto round_negative_25 = [](wp<TS<double>> value) { return operators::round(value, -25); };
    const auto round_negative_308 = [](wp<TS<double>> value) { return operators::round(value, -308); };
    const auto round_tens = [](wp<TS<double>> value) { return operators::round(value, -1); };
    const auto round_hundreds = [](wp<TS<double>> value) { return operators::round(value, -2); };
    const auto round_scalar = [] { return operators::round(2.675, 2); };

    CHECK(testing::test_graph(integer_power, std::array<i64, 3>{2, -2, 3}, std::array<i64, 3>{10, 3, 0})
              .equals<i64>({1024, -8, 1}));
    const auto wrapping_power = [] { return operators::pow(i64{2}, i64{63}); };
    CHECK(testing::test_graph(wrapping_power).equals<i64>({std::numeric_limits<i64>::min()}));
    CHECK(testing::test_graph(floating_power, std::array{2.0, -2.0}, std::array{3.0, 4.0})
              .equals<double>({8.0, 16.0}));
    CHECK(testing::test_graph(round_scalar).equals<double>({2.67}));
    CHECK(testing::test_graph(round_two, std::array{2.675, 1.125, 1.375}).equals<double>({2.67, 1.12, 1.38}));
    CHECK(testing::test_graph(round_negative, std::array{5.0000000000000004e22}).equals<double>({1e23}));
    CHECK(testing::test_graph(round_negative_24, std::array{1.5e24}).equals<double>({1e24}));
    CHECK(testing::test_graph(round_negative_25, std::array{2.5e25}).equals<double>({3e25}));
    CHECK(testing::test_graph(round_negative, std::array{1.5e23}).equals<double>({2e23}));
    CHECK_THROWS_AS(testing::test_graph(round_negative_308, std::array{1.79e308}), std::overflow_error);
    CHECK_THROWS_AS(testing::test_graph(round_negative_308, std::array{1.5e308}), std::overflow_error);
    const auto infinity = std::numeric_limits<double>::infinity();
    CHECK(testing::test_graph(round_tens,
                              std::array{std::nextafter(5.0, 0.0), 5.0, std::nextafter(5.0, infinity),
                                         std::nextafter(25.0, 0.0), 25.0, std::nextafter(25.0, infinity),
                                         std::nextafter(35.0, 0.0), 35.0, std::nextafter(35.0, infinity)})
              .equals<double>({0.0, 0.0, 10.0, 20.0, 20.0, 30.0, 30.0, 40.0, 40.0}));
    CHECK(testing::test_graph(round_hundreds,
                              std::array{std::nextafter(50.0, 0.0), 50.0, std::nextafter(50.0, infinity)})
              .equals<double>({0.0, 0.0, 100.0}));
    const auto round_signed_zero = [](wp<TS<double>> value) { return operators::round(value, 0); };
    const auto rounded_zero = testing::test_graph(round_signed_zero, std::array{-0.0, -0.1, 0.1});
    REQUIRE(rounded_zero.size() == 3);
    CHECK(std::signbit(scalar_cast<double>(*rounded_zero[0]).value()));
    CHECK(std::signbit(scalar_cast<double>(*rounded_zero[1]).value()));
    CHECK_FALSE(std::signbit(scalar_cast<double>(*rounded_zero[2]).value()));

    const auto negative_integer_exponent = [] { return operators::pow(i64{2}, i64{-1}); };
    const auto zero_to_negative_power = [] { return operators::pow(0.0, -1.0); };
    const auto complex_power = [] { return operators::pow(-2.0, 0.5); };
    CHECK_THROWS_AS(testing::test_graph(negative_integer_exponent), std::domain_error);
    CHECK_THROWS_AS(testing::test_graph(zero_to_negative_power), std::domain_error);
    CHECK_THROWS_AS(testing::test_graph(complex_power), std::domain_error);
}

TEST_CASE("convert maps a full target type and permits collection overloads",
          "[feature:operator][feature:type-system][series:value][series:fixed-tsl]") {
    const auto to_double = [](wp<TS<i64>> value) { return operators::convert[type<TS<double>>](value); };
    const auto to_integer_from_integer = [](wp<TS<i64>> value) {
        return operators::convert[type<TS<i64>>](value);
    };
    const auto to_integer = [](wp<TS<double>> value) { return operators::convert[type<TS<i64>>](value); };
    const auto convert_without_target = [](wp<TS<i64>> value) { return operators::convert(value); };
    CHECK(testing::test_graph(to_double, std::array<i64, 2>{2, -3}).equals<double>({2.0, -3.0}));
    CHECK(testing::test_graph(to_integer_from_integer, std::array<i64, 2>{2, -3}).equals<i64>({2, -3}));
    CHECK(testing::test_graph(to_integer, std::array{2.9, -2.9}).equals<i64>({2, -2}));
    CHECK_THROWS_AS(testing::test_graph(convert_without_target, std::array<i64, 1>{7}), WiringError);

    const auto target_matches = [](const WiringArguments &arguments, const Type &target) {
        const auto *mapping = arguments.type_map().find(details::type_parameter<operators::CONVERT_RESULT>());
        const auto *requested = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
        return requested != nullptr && *requested == &target;
    };
    const auto convert_to_integer_list = operators::convert[type<TSL<TS<i64>, 2>>];
    const auto convert_to_floating_list = operators::convert[type<TSL<TS<double>, 2>>];
    static auto integer_requirement_calls = 0;
    static auto integer_body_calls = 0;
    static auto floating_requirement_calls = 0;
    static auto floating_body_calls = 0;
    operators::convert.overload(graph<operators::CONVERT_RESULT>(
        "name"_n = "fixed_list_integer",
        require = [target_matches](const WiringArguments &arguments) {
            ++integer_requirement_calls;
            return target_matches(arguments, TSL<TS<i64>, 2>::type());
        },
        [](wp<TSL<TS<i64>, 2>> value) -> wp<operators::CONVERT_RESULT> {
            ++integer_body_calls;
            return wp<operators::CONVERT_RESULT>{value};
        }));
    operators::convert.overload(graph<operators::CONVERT_RESULT>(
        "name"_n = "fixed_list_floating",
        require = [target_matches](const WiringArguments &arguments) {
            ++floating_requirement_calls;
            return target_matches(arguments, TSL<TS<double>, 2>::type());
        },
        [](wp<TSL<TS<i64>, 2>> value) -> wp<operators::CONVERT_RESULT> {
            ++floating_body_calls;
            return wp<operators::CONVERT_RESULT>{tsl(
                operators::convert[type<TS<double>>](value[0].as<TS<i64>>()),
                operators::convert[type<TS<double>>](value[1].as<TS<i64>>()))};
        }));
    testing::test_wiring([&] {
        const auto source = node([](Out<TS<i64>>) {});
        const auto input = tsl(source(), source());
        const auto integer_result = convert_to_integer_list(input);
        const auto integer_calls_after_first_wiring = integer_requirement_calls;
        const auto repeated_integer_result = convert_to_integer_list(input);
        CHECK(integer_requirement_calls == integer_calls_after_first_wiring);
        const auto floating_result = convert_to_floating_list(input);

        CHECK(&integer_result.type() == &TSL<TS<i64>, 2>::type());
        CHECK(&repeated_integer_result.type() == &TSL<TS<i64>, 2>::type());
        CHECK(&floating_result.type() == &TSL<TS<double>, 2>::type());
        CHECK(integer_body_calls > 0);
        CHECK(floating_body_calls > 0);

        const auto body_calls_before_rejection = integer_body_calls + floating_body_calls;
        CHECK_THROWS_AS((operators::convert[type<TSL<TS<bool>, 2>>](input)), WiringError);
        CHECK(integer_body_calls + floating_body_calls == body_calls_before_rejection);
    });

    testing::test_wiring([&] {
        const auto const_input = operators::convert[type<TS<i64>>](i64{7});
        CHECK(&const_input.type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
    });

    testing::test_wiring([&] {
        const auto source = node([](Out<TS<i64>>) {});
        const auto input = source();
        CHECK_THROWS_AS((operators::convert[type<TS<bool>>](input)), WiringError);
        CHECK_THROWS_AS((operators::convert[type<TS<bool>>](opt(input))), WiringError);
    });

    const auto stronger_target = [](wp<TS<i64>> value) {
        return operators::convert[type<TS<i64, TimeSeriesGuarantee::const_>>](value);
    };
    CHECK_THROWS_AS(testing::test_graph(stronger_target, std::array<i64, 1>{7}), WiringError);
}

TEST_CASE("conversion and division preserve resolved time-series guarantees",
          "[feature:operator][feature:type-system][feature:wiring][series:value]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    const auto ordinary_source = node([](Out<TS<i64>>) {});
    const auto deduplicated_source = node([](Out<TS<i64, TimeSeriesGuarantee::deduplicated>>) {});
    const auto final_source = node([](Out<TS<i64, TimeSeriesGuarantee::final>>) {});
    const auto constant_source = node([](Out<TS<i64, TimeSeriesGuarantee::const_>>) {});

    testing::test_wiring([&] {
        const auto ordinary_input = ordinary_source();
        const auto ordinary_conversion = operators::convert[type<TS<double>>](ordinary_input);
        const auto ordinary_quotient = operators::div(ordinary_input, ordinary_input);
        CHECK(&ordinary_conversion.type() == &TS<double>::type());
        CHECK(&ordinary_quotient.type() == &TS<double>::type());

        const auto deduplicated_input = deduplicated_source();
        const auto deduplicated_conversion =
            operators::convert[type<TS<double, TimeSeriesGuarantee::deduplicated>>](deduplicated_input);
        const auto weakened_deduplicated_conversion = operators::convert[type<TS<double>>](deduplicated_input);
        const auto deduplicated_quotient = operators::div(deduplicated_input, deduplicated_input);
        CHECK(&deduplicated_conversion.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&weakened_deduplicated_conversion.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&deduplicated_quotient.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());

        const auto final_input = final_source();
        const auto final_conversion =
            operators::convert[type<TS<double, TimeSeriesGuarantee::final>>](final_input);
        const auto weakened_final_conversion = operators::convert[type<TS<double>>](final_input);
        const auto final_quotient = operators::div(final_input, final_input);
        CHECK(&final_conversion.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        CHECK(&weakened_final_conversion.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        CHECK(&final_quotient.type() == &TS<double, TimeSeriesGuarantee::final>::type());

        const auto constant_input = constant_source();
        const auto constant_conversion =
            operators::convert[type<TS<double, TimeSeriesGuarantee::const_>>](constant_input);
        const auto constant_quotient = operators::div(constant_input, constant_input);
        CHECK(&constant_conversion.type() == &TS<double, TimeSeriesGuarantee::const_>::type());
        CHECK(&constant_quotient.type() == &TS<double, TimeSeriesGuarantee::const_>::type());
    });
}

TEST_CASE("conversion observes the guarantee of a referenced value",
          "[feature:operator][feature:reference][feature:type-system][series:value]"
          "[guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    const auto source = node([](Out<TS<i64, TimeSeriesGuarantee::final>>) {});
    const auto reference = node([](In<REF<TS<i64, TimeSeriesGuarantee::final>>> input,
                                   Out<REF<TS<i64, TimeSeriesGuarantee::final>>> output) { output.copy(input); });

    testing::test_wiring([&] {
        const auto input = reference(source());
        const auto ordinary_conversion = operators::convert[type<TS<double>>](input);
        CHECK(&ordinary_conversion.type() == &TS<double>::type());
        CHECK_THROWS_AS((operators::convert[type<TS<double, TimeSeriesGuarantee::final>>](input)), WiringError);
    });
}

TEST_CASE("conversion revalidates after an invalid input and emits equal values",
          "[feature:operator][feature:validity][series:value][validity:required]"
          "[transition:invalidation][transition:revalidation]") {
    struct invalidating_source {
        In<TS<i64>> value;
        In<TS<bool>> invalidate;
        Out<TS<i64>> output;

        void evaluate() const {
            if (*invalidate) {
                output.invalidate();
                return;
            }

            output.set(*value);
        }
    };

    const auto source = node<invalidating_source>();
    const auto convert = [&](wp<TS<i64>> value, wp<TS<bool>> invalidate) {
        return operators::convert[type<TS<double>>](source(value, invalidate));
    };

    CHECK(testing::test_graph(convert, std::array<i64, 4>{1, 1, 1, 1}, std::array{false, true, false, false})
              .equals<double>({1.0, std::nullopt, 1.0, 1.0}));
}

TEST_CASE("numeric conversion checks i64 boundaries and non-finite values",
          "[feature:operator][series:value][scalar:i64][scalar:floating-point]") {
    const auto to_integer = [](wp<TS<double>> value) { return operators::convert[type<TS<i64>>](value); };
    const auto minimum = -std::ldexp(1.0, 63);
    CHECK(testing::test_graph(to_integer, std::array{minimum}).equals<i64>({std::numeric_limits<i64>::min()}));
    CHECK_THROWS_AS(testing::test_graph(to_integer, std::array{std::ldexp(1.0, 63)}), std::overflow_error);
    CHECK_THROWS_AS(testing::test_graph(to_integer, std::array{std::numeric_limits<double>::infinity()}),
                    std::overflow_error);
    CHECK_THROWS_AS(testing::test_graph(to_integer, std::array{-std::numeric_limits<double>::infinity()}),
                    std::overflow_error);
    CHECK_THROWS_AS(testing::test_graph(to_integer, std::array{std::numeric_limits<double>::quiet_NaN()}),
                    std::domain_error);
}

TEST_CASE("subtraction handles values and timestamp differences",
          "[feature:operator][series:value][scalar:i64][scalar:evaluation-time][scalar:timedelta]") {
    const auto integer_difference = [](wp<TS<i64>> value) { return value - i64{3}; };
    const auto timestamp_difference = [](wp<TS<date_time_t>> lhs, wp<TS<date_time_t>> rhs) {
        return operators::sub(lhs, rhs);
    };
    const auto timestamps = std::array{MIN_DT + timedelta_t{10}, MIN_DT + timedelta_t{4}};
    const auto rhs = std::array{MIN_DT + timedelta_t{3}, MIN_DT + timedelta_t{9}};

    CHECK(testing::test_graph(integer_difference, std::array<i64, 3>{5, 3, -2}).equals<i64>({2, 0, -5}));
    CHECK(testing::test_graph(timestamp_difference, timestamps, rhs)
              .equals<timedelta_t>({timedelta_t{7}, timedelta_t{-5}}));
}

TEST_CASE("multiplication accepts scalar and time-series values",
          "[feature:operator][series:value][scalar:i64][shape:source][shape:single-node]") {
    const auto multiply = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return lhs * rhs; };
    const auto scalar_left = [](wp<TS<i64>> value) { return i64{3} * value; };
    const auto constant = [] { return operators::mul(i64{6}, i64{7}); };

    CHECK(testing::test_graph(multiply, std::array<i64, 3>{2, -3, 0}, std::array<i64, 3>{4, 2, 10})
              .equals<i64>({8, -6, 0}));
    CHECK(testing::test_graph(scalar_left, std::array<i64, 2>{-2, 5}).equals<i64>({-6, 15}));
    CHECK(testing::test_graph(constant).equals<i64>({42}));

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)operators::mul(i64{6}, i64{7});
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 1);
    CHECK(compiled.node(0).evaluation_function() != nullptr);
}

TEST_CASE("Boolean operators calculate scalar-only calls",
          "[feature:operator][feature:wiring][series:value][scalar:boolean][shape:source]") {
    const auto constant = [] { return operators::and_(true, false); };

    CHECK(testing::test_graph(constant).equals<bool>({false}));

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)operators::or_(true, false);
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 1);
    CHECK(compiled.node(0).evaluation_function() != nullptr);
}

TEST_CASE("scalar-only unary and comparison operators publish once at startup",
          "[feature:operator][series:value][scalar:i64][scalar:boolean][guarantee:const]") {
    const auto ticks = std::array{true, false, true};
    const auto negate = [](wp<TS<bool>>) { return operators::neg(i64{3}); };
    const auto compare = [](wp<TS<bool>>) { return operators::lt(i64{2}, i64{3}); };

    CHECK(testing::test_graph(negate, ticks).equals<i64>({-3, std::nullopt, std::nullopt}));
    CHECK(testing::test_graph(compare, ticks).equals<bool>({true, std::nullopt, std::nullopt}));
}

TEST_CASE("optional scalar arithmetic uses presence matching",
          "[feature:operator][feature:wiring][series:value][scalar:i64][validity:optional]") {
    const auto ticks = std::array{true, false, true};
    const auto add_absent = [](wp<TS<bool>>) { return operators::add(std::optional<i64>{}, i64{3}); };
    const auto multiply_absent = [](wp<TS<bool>>) { return operators::mul(std::optional<i64>{}, i64{3}); };
    const auto add_present = [](wp<TS<bool>>) { return operators::add(std::optional<i64>{2}, i64{3}); };

    CHECK(testing::test_graph(add_absent, ticks).equals<i64>({3, std::nullopt, std::nullopt}));
    CHECK(testing::test_graph(multiply_absent, ticks).equals<i64>({3, std::nullopt, std::nullopt}));
    CHECK(testing::test_graph(add_present, ticks).equals<i64>({5, std::nullopt, std::nullopt}));

    testing::test_wiring([&] {
        const auto absent = operators::add(std::optional<i64>{}, i64{3});
        const auto present = operators::add(std::optional<i64>{2}, i64{3});
        CHECK(&absent.type() == &TS<i64>::type());
        CHECK(&present.type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
    });
}

TEST_CASE("integer multiplication and unary operations wrap at signed boundaries",
          "[feature:operator][series:value][scalar:i64][scalar:timedelta]") {
    constexpr auto I64_MIN = std::numeric_limits<i64>::min();
    constexpr auto I64_MAX = std::numeric_limits<i64>::max();
    const auto multiply = [](wp<TS<i64>> value) { return operators::mul(value, i64{-1}); };
    const auto multiply_optional = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return opt(lhs) * opt(rhs); };
    const auto negate = [](wp<TS<i64>> value) { return operators::neg(value); };
    const auto absolute = [](wp<TS<i64>> value) { return operators::abs(value); };
    const auto integer_values = std::array{I64_MIN, I64_MAX, i64{-2}, i64{2}};

    CHECK(testing::test_graph(multiply, std::array{I64_MIN}).equals<i64>({I64_MIN}));
    CHECK(testing::test_graph(multiply_optional, std::array{I64_MAX}, std::array<i64, 1>{2})
              .equals<i64>({-2}));
    CHECK(testing::test_graph(negate, integer_values).equals<i64>({I64_MIN, -I64_MAX, 2, -2}));
    CHECK(testing::test_graph(absolute, integer_values).equals<i64>({I64_MIN, I64_MAX, 2, 2}));

    const auto negate_duration = [](wp<TS<timedelta_t>> value) { return operators::neg(value); };
    const auto absolute_duration = [](wp<TS<timedelta_t>> value) { return operators::abs(value); };
    const auto duration_values = std::array{timedelta_t::min(), timedelta_t::max()};
    CHECK(testing::test_graph(negate_duration, duration_values)
              .equals<timedelta_t>({timedelta_t::min(), timedelta_t{-timedelta_t::max().count()}}));
    CHECK(testing::test_graph(absolute_duration, duration_values)
              .equals<timedelta_t>({timedelta_t::min(), timedelta_t::max()}));

    const auto scalar_add = [] { return operators::add(std::numeric_limits<i64>::max(), i64{1}); };
    const auto scalar_subtract = [] { return operators::sub(std::numeric_limits<i64>::min(), i64{1}); };
    const auto scalar_multiply = [] { return operators::mul(std::numeric_limits<i64>::min(), i64{-1}); };
    const auto scalar_negate = [] { return operators::neg(std::numeric_limits<i64>::min()); };
    const auto scalar_absolute = [] { return operators::abs(std::numeric_limits<i64>::min()); };
    CHECK(testing::test_graph(scalar_add).equals<i64>({I64_MIN}));
    CHECK(testing::test_graph(scalar_subtract).equals<i64>({I64_MAX}));
    CHECK(testing::test_graph(scalar_multiply).equals<i64>({I64_MIN}));
    CHECK(testing::test_graph(scalar_negate).equals<i64>({I64_MIN}));
    CHECK(testing::test_graph(scalar_absolute).equals<i64>({I64_MIN}));
}

TEST_CASE("optional multiplication substitutes one for invalid operands",
          "[feature:operator][feature:validity][validity:optional][series:value][scalar:floating-point]") {
    struct possibly_invalid_value {
        In<TS<double>> value;
        In<TS<bool>> invalidate;
        Out<TS<double>> output;

        void evaluate() const {
            if (*invalidate) {
                output.invalidate();
                return;
            }

            output.copy(value);
        }
    };

    const auto source = node<possibly_invalid_value>();
    const auto multiply = [&](wp<TS<double>> lhs, wp<TS<bool>> invalidate_lhs,
                              wp<TS<double>> rhs, wp<TS<bool>> invalidate_rhs) {
        return opt(source(lhs, invalidate_lhs)) * source(rhs, invalidate_rhs);
    };
    const auto lhs = std::array{2.0, 2.0, 3.0, 3.0};
    const auto invalidate_lhs = std::array{false, true, true, false};
    const auto rhs = std::array{4.0, 4.0, 4.0, 4.0};
    const auto invalidate_rhs = std::array{false, false, false, false};

    CHECK(testing::test_graph(multiply, lhs, invalidate_lhs, rhs, invalidate_rhs).equals<double>({8.0, 4.0, 4.0, 12.0}));
}

TEST_CASE("comparisons and named Boolean operators return Boolean time series",
          "[feature:operator][series:value][scalar:boolean][shape:single-node]") {
    const auto less_than = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return operators::lt(lhs, rhs); };
    const auto equal = [](wp<TS<i64>> value) { return operators::eq(value, i64{2}); };
    const auto both = [](wp<TS<bool>> lhs, wp<TS<bool>> rhs) { return lhs && rhs; };
    const auto either = [](wp<TS<bool>> lhs, wp<TS<bool>> rhs) { return lhs || rhs; };
    const auto negate = [](wp<TS<bool>> value) { return !value; };
    const auto and_scalar_right = [](wp<TS<bool>> value) { return value && true; };
    const auto and_scalar_left = [](wp<TS<bool>> value) { return false && value; };
    const auto or_scalar_right = [](wp<TS<bool>> value) { return value || false; };
    const auto or_scalar_left = [](wp<TS<bool>> value) { return true || value; };
    const auto named_and = [](wp<TS<bool>> lhs, wp<TS<bool>> rhs) { return operators::and_(lhs, rhs); };
    const auto erased_not = [](wp<TS<bool>> value) {
        const auto erased = wp<>{value};
        return !erased;
    };
    const auto erased_and = [](wp<TS<bool>> lhs, wp<TS<bool>> rhs) {
        const auto erased_lhs = wp<>{lhs};
        const auto erased_rhs = wp<>{rhs};
        return erased_lhs && erased_rhs;
    };
    const auto erased_or = [](wp<TS<bool>> lhs, wp<TS<bool>> rhs) {
        const auto erased_lhs = wp<>{lhs};
        const auto erased_rhs = wp<>{rhs};
        return erased_lhs || erased_rhs;
    };
    const auto erased_and_scalar_right = [](wp<TS<bool>> value) { return wp<>{value} && true; };
    const auto erased_and_scalar_left = [](wp<TS<bool>> value) { return false && wp<>{value}; };
    const auto erased_or_scalar_right = [](wp<TS<bool>> value) { return wp<>{value} || false; };
    const auto erased_or_scalar_left = [](wp<TS<bool>> value) { return true || wp<>{value}; };

    CHECK(testing::test_graph(less_than, std::array<i64, 3>{1, 3, 2}, std::array<i64, 3>{2, 2, 2})
              .equals<bool>({true, false, false}));
    CHECK(testing::test_graph(equal, std::array<i64, 3>{2, 3, 2}).equals<bool>({true, false, true}));
    CHECK(testing::test_graph(both, std::array{true, true, false}, std::array{true, false, false})
              .equals<bool>({true, false, false}));
    CHECK(testing::test_graph(either, std::array{true, false}, std::array{false, false}).equals<bool>({true, false}));
    CHECK(testing::test_graph(negate, std::array{true, false}).equals<bool>({false, true}));
    CHECK(testing::test_graph(and_scalar_right, std::array{true, false}).equals<bool>({true, false}));
    CHECK(testing::test_graph(and_scalar_left, std::array{true, false}).equals<bool>({false, false}));
    CHECK(testing::test_graph(or_scalar_right, std::array{true, false}).equals<bool>({true, false}));
    CHECK(testing::test_graph(or_scalar_left, std::array{true, false}).equals<bool>({true, true}));
    CHECK(testing::test_graph(erased_not, std::array{true, false}).equals<bool>({false, true}));
    CHECK(testing::test_graph(erased_and, std::array{true, false}, std::array{true, false})
              .equals<bool>({true, false}));
    CHECK(testing::test_graph(erased_or, std::array{true, false}, std::array{false, false})
              .equals<bool>({true, false}));
    CHECK(testing::test_graph(erased_and_scalar_right, std::array{true, false}).equals<bool>({true, false}));
    CHECK(testing::test_graph(erased_and_scalar_left, std::array{true, false}).equals<bool>({false, false}));
    CHECK(testing::test_graph(erased_or_scalar_right, std::array{true, false}).equals<bool>({true, false}));
    CHECK(testing::test_graph(erased_or_scalar_left, std::array{true, false}).equals<bool>({true, true}));
    CHECK(testing::test_graph(named_and, std::array{true, false}, std::array{true, true})
              .equals<bool>({true, false}));
}

TEST_CASE("symbol equality handles scalar, mixed, and time-series inputs",
          "[feature:operator][series:value][scalar:symbol]") {
    const auto scalar_equal = [] { return operators::eq(symbol{}, symbol{""}); };
    const auto scalar_not_equal = [] { return operators::ne(symbol{}, symbol{""}); };
    const auto mixed_equal = [](wp<TS<symbol>> value) { return operators::eq(value, symbol{""}); };
    const auto mixed_not_equal = [](wp<TS<symbol>> value) { return operators::ne(value, symbol{""}); };
    const auto series_equal = [](wp<TS<symbol>> lhs, wp<TS<symbol>> rhs) { return operators::eq(lhs, rhs); };
    const auto series_not_equal = [](wp<TS<symbol>> lhs, wp<TS<symbol>> rhs) {
        return operators::ne(lhs, rhs);
    };
    const auto values = std::array{symbol{}, symbol{""}, symbol{"bid"}};
    const auto same_or_different = std::array{symbol{""}, symbol{""}, symbol{"ask"}};

    CHECK(testing::test_graph(scalar_equal).equals<bool>({false}));
    CHECK(testing::test_graph(scalar_not_equal).equals<bool>({true}));
    CHECK(testing::test_graph(mixed_equal, values).equals<bool>({false, true, false}));
    CHECK(testing::test_graph(mixed_not_equal, values).equals<bool>({true, false, true}));
    CHECK(testing::test_graph(series_equal, values, same_or_different).equals<bool>({false, true, false}));
    CHECK(testing::test_graph(series_not_equal, values, same_or_different).equals<bool>({true, false, true}));
}

TEST_CASE("required Boolean operators wait for every valid input",
          "[feature:operator][feature:validity][series:value][scalar:boolean][validity:required]") {
    struct invalidating_boolean_source {
        In<TS<bool>> value;
        In<TS<bool>> invalidate;
        Out<TS<bool>> output;

        void evaluate() const {
            if (*invalidate) {
                output.invalidate();
                return;
            }

            output.copy(value);
        }
    };
    struct validity_observer {
        In<TS<bool>, optional> value;
        Out<TS<bool>> output;

        void evaluate() const { output.set(value.valid()); }
    };

    const auto source = node<invalidating_boolean_source>();
    const auto observe_validity = node<validity_observer>();
    const auto conjunction = [&](wp<TS<bool>> lhs, wp<TS<bool>> invalidate_lhs,
                                 wp<TS<bool>> rhs, wp<TS<bool>> invalidate_rhs) {
        return observe_validity(source(lhs, invalidate_lhs) && source(rhs, invalidate_rhs));
    };
    const auto disjunction = [&](wp<TS<bool>> lhs, wp<TS<bool>> invalidate_lhs,
                                 wp<TS<bool>> rhs, wp<TS<bool>> invalidate_rhs) {
        return observe_validity(source(lhs, invalidate_lhs) || source(rhs, invalidate_rhs));
    };
    const auto valid = std::array{false, false, false};
    const auto invalid_then_valid = std::array{false, true, false};

    CHECK(testing::test_graph(conjunction, std::array{false, false, false}, valid,
                              std::array{true, true, true}, invalid_then_valid)
              .equals<bool>({true, false, true}));
    CHECK(testing::test_graph(disjunction, std::array{true, true, true}, valid,
                              std::array{false, false, false}, invalid_then_valid)
              .equals<bool>({true, false, true}));
}

TEST_CASE("generic equality compares compound time-series values",
          "[feature:operator][feature:scalar-api][scalar:compound][shape:single-node]") {
    const auto series_equal = [](wp<TS<Quote>> left, wp<TS<Quote>> right) {
        return operators::eq(left, right);
    };
    const auto lhs = std::array{Quote{.quantity = 2, .price = 3.5}, Quote{.quantity = 2, .price = 3.5}};
    const auto rhs = std::array{Quote{.quantity = 2, .price = 3.5}, Quote{.quantity = 3, .price = 3.5}};

    CHECK(testing::test_graph(series_equal, lhs, rhs).equals<bool>({true, false}));
}

TEST_CASE("C++ wiring points report output presence explicitly",
          "[feature:operator][feature:wiring][series:value]") {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    auto output = wp<>{};
    const auto present = operators::const_(true);
    output = present;

    CHECK(output.has_output());
    CHECK_FALSE(wp<>{}.has_output());
}

TEST_CASE("wiring points are not Boolean-testable",
          "[feature:operator][feature:wiring][series:value]") {
    STATIC_CHECK_FALSE(ContextuallyConvertibleToBool<wp<TS<bool>>>);
    STATIC_CHECK_FALSE(ContextuallyConvertibleToBool<wp<>>);
    STATIC_CHECK_FALSE(ContextuallyConvertibleToBool<wp<TSL<TS<bool>, SIZE>, var_args>>);
    STATIC_CHECK_FALSE(ContextuallyConvertibleToBool<wp<TSB<BooleanBundle>, var_kwargs>>);
    STATIC_CHECK_FALSE(ExplicitlyCastableToBool<wp<TS<bool>>>);
    STATIC_CHECK_FALSE(ExplicitlyCastableToBool<wp<>>);
    STATIC_CHECK_FALSE(DirectListInitializableAsBool<wp<TS<bool>>>);
    STATIC_CHECK_FALSE(DirectListInitializableAsBool<wp<>>);
}

TEST_CASE("logical wiring operators wire both operands",
          "[feature:operator][feature:wiring][series:value][scalar:boolean]") {
    const auto source = node([](Out<TS<bool>>) {});
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    auto wired_operands = 0;
    const auto make_operand = [&] {
        ++wired_operands;
        return source();
    };

    const auto both = make_operand() && make_operand();
    CHECK(wired_operands == 2);
    CHECK(&both.type() == &TS<bool>::type());

    auto right_operand_wired = 0;
    const auto make_right_operand = [&] {
        ++right_operand_wired;
        return source();
    };

    const auto false_and = false && make_right_operand();
    CHECK(right_operand_wired == 1);
    CHECK(&false_and.type() == &TS<bool>::type());

    right_operand_wired = 0;
    const auto true_or = true || make_right_operand();
    CHECK(right_operand_wired == 1);
    CHECK(&true_or.type() == &TS<bool>::type());
}

TEST_CASE("logical wiring operators preserve the resolved output guarantee",
          "[feature:operator][feature:type-system][feature:wiring][series:value][scalar:boolean]"
          "[guarantee:deduplicated]") {
    const auto source = node([](Out<TS<bool, TimeSeriesGuarantee::deduplicated>>) {});

    testing::test_wiring([&] {
        const auto typed = source();
        const auto erased = wp<>{typed};
        const auto typed_result = !typed;
        const auto erased_result = erased && true;

        CHECK(&typed_result.type() == &TS<bool, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&erased_result.type() == &TS<bool, TimeSeriesGuarantee::deduplicated>::type());
    });
}
