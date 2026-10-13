#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <limits>
#include <optional>
#include <stdexcept>

using namespace bg;

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
