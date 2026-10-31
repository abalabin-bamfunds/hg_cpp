#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/nested.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace bg;
using namespace bg::literals;

TEST_CASE("scalar addition runs once at startup",
          "[feature:operator][shape:source][scalar:floating-point][guarantee:const]") {
    const auto scalar_addition = [](wp<TS<bool>>) { return operators::add(1.5, 2.5); };
    CHECK(bg::run_graph(scalar_addition, {true, false, true})
              .equals<double>({4.0, std::nullopt, std::nullopt}));

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto result = operators::add("lhs"_n = 1.5, "rhs"_n = 2.5);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::const_>::type());
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 1);
    CHECK(compiled.node(0).has_output());
    CHECK(compiled.node(0).evaluation_function() != nullptr);
    REQUIRE(compiled.node(0).parameters().size() == 2);
    CHECK(compiled.node(0).parameters()[0].mode() == ParameterBindingMode::scalar);
    CHECK(compiled.node(0).parameters()[1].mode() == ParameterBindingMode::scalar);
}

TEST_CASE("addition accepts scalar and time-series operands in either order",
          "[feature:operator][series:value][scalar:floating-point][shape:single-node]") {
    const auto scalar_left = [](wp<TS<double>> value) { return 2.0 + value; };
    const auto scalar_right = [](wp<TS<double>> value) { return value + 2.0; };

    CHECK(bg::run_graph(scalar_left, {1.0, 2.0}).equals<double>({3.0, 4.0}));
    CHECK(bg::run_graph(scalar_right, {1.0, 2.0}).equals<double>({3.0, 4.0}));
}

TEST_CASE("addition accepts typed and erased wiring points",
          "[feature:operator][series:value][scalar:floating-point][shape:chain]") {
    const auto graph = [](wp<TS<double>> lhs, wp<TS<double>> rhs) {
        const auto erased = lhs + rhs;
        return erased + lhs + 1.0;
    };

    CHECK(bg::run_graph(graph, {1.0, 2.0}, {10.0, 20.0})
              .equals<double>({13.0, 25.0}));
}

TEST_CASE("addition accepts named time-series arguments",
          "[feature:operator][series:value][shape:single-node]") {
    const auto graph = [](wp<TS<double>> lhs, wp<TS<double>> rhs) {
        return operators::add("rhs"_n = rhs, "lhs"_n = lhs);
    };

    CHECK(bg::run_graph(graph, {1.0, 2.0}, {10.0, 20.0})
              .equals<double>({11.0, 22.0}));
}

TEST_CASE("string addition concatenates scalar and time-series values",
          "[feature:operator][series:value][scalar:string][shape:source][shape:single-node]") {
    const auto constant = [] {
        const auto result = operators::add(std::string_view{"left"}, std::string_view{"right"});
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::const_>::type());
        return result;
    };
    const auto prefix = [](wp<TS<string>> value) { return operators::add(std::string_view{"prefix:"}, value); };
    const auto suffix = [](wp<TS<string>> value) { return operators::add(value, std::string_view{" :suffix"}); };
    const auto left = std::array<std::string_view, 2>{"one", "two"};

    CHECK(bg::run_graph(constant).equals<string>({"leftright"}));
    CHECK(bg::run_graph(prefix, left).equals<string>({"prefix:one", "prefix:two"}));
    CHECK(bg::run_graph(suffix, left).equals<string>({"one :suffix", "two :suffix"}));
}

TEST_CASE("string addition preserves embedded nulls and supports long values",
          "[feature:operator][series:value][scalar:string]") {
    const auto input = std::array{std::string_view{"a\0b", 3}};
    const auto embedded_null = [](wp<TS<string>> value) { return operators::add(value, std::string_view{"\0c", 2}); };

    auto lhs = std::string(128, 'a');
    auto rhs = std::string(160, 'b');
    const auto long_lhs = std::array{std::string_view{lhs}};
    const auto long_rhs = std::array{std::string_view{rhs}};
    const auto long_add = [](wp<TS<string>> left, wp<TS<string>> right) { return operators::add(left, right); };
    auto expected_long = lhs + rhs;
    const auto long_scalar_add = [&] {
        return operators::add(std::string_view{lhs}, std::string_view{rhs});
    };

    CHECK(bg::run_graph(embedded_null, input).equals<string>({std::string_view{"a\0b\0c", 5}}));
    CHECK(bg::run_graph(long_add, long_lhs, long_rhs).equals<string>({expected_long}));
    CHECK(bg::run_graph(long_scalar_add).equals<string>({expected_long}));
}

TEST_CASE("string addition propagates time-series guarantees",
          "[feature:operator][feature:type-system][series:value][scalar:string]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    struct deduplicated_source {
        In<TS<string>> input;
        Out<TS<string, TimeSeriesGuarantee::deduplicated>> output;

        void evaluate() const { output.set(*input); }
    };
    struct final_source {
        Scalar<string> value;
        Out<TS<string, TimeSeriesGuarantee::final>> output;

        void start() const { output.set(*value); }
    };
    const auto pass_reference = node([](In<REF<TS<string, TimeSeriesGuarantee::final>>> input,
                                        Out<REF<TS<string, TimeSeriesGuarantee::final>>> output) {
        output.copy(input);
    });

    const auto ordinary = [](wp<TS<string, TimeSeriesGuarantee::ordinary>> lhs,
                             wp<TS<string, TimeSeriesGuarantee::ordinary>> rhs) {
        const auto result = operators::add(lhs, rhs);
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };
    const auto make_deduplicated = node<deduplicated_source>();
    const auto deduplicated = [&](wp<TS<string>> lhs) {
        const auto result = operators::add(make_deduplicated(lhs), std::string_view{"!"});
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto make_final = node<final_source>();
    const auto final = [&](wp<TS<string>> ordinary_value) {
        const auto result = operators::add(make_final(std::string_view{"final"}), ordinary_value);
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };
    const auto final_pair = [&] {
        const auto result = operators::add(make_final(std::string_view{"left"}),
                                           make_final(std::string_view{"right"}));
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::final>::type());
        return result;
    };
    const auto final_plus_deduplicated = [&](wp<TS<string>> ordinary_value) {
        const auto result = operators::add(make_final(std::string_view{"final"}),
                                           make_deduplicated(ordinary_value));
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto concatenated_deduplication = [&](wp<TS<string>> lhs, wp<TS<string>> rhs) {
        const auto result = operators::add(make_deduplicated(lhs), make_deduplicated(rhs));
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto reference = [&](wp<TS<string>> ordinary_value) {
        const auto final_value = make_final(std::string_view{"reference"});
        const auto result = operators::add(pass_reference(final_value), std::string_view{"!"});
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::ordinary>::type());
        return operators::add(result, ordinary_value);
    };
    const auto constant = [] {
        const auto result = operators::add(operators::const_(std::string_view{"a"}),
                                           operators::const_(std::string_view{"b"}));
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::const_>::type());
        return result;
    };
    const auto input = std::array<std::string_view, 2>{"x", "y"};

    CHECK(bg::run_graph(ordinary, input, input).equals<string>({"xx", "yy"}));
    CHECK(bg::run_graph(deduplicated, input).equals<string>({"x!", "y!"}));
    CHECK(bg::run_graph(final, input).equals<string>({"finalx", "finaly"}));
    CHECK(bg::run_graph(final_pair).equals<string>({"leftright"}));
    CHECK(bg::run_graph(final_plus_deduplicated, input).equals<string>({"finalx", "finaly"}));
    CHECK(bg::run_graph(concatenated_deduplication,
                              {"ab", "a"},
                              {"c", "bc"})
              .equals<string>({"abc", std::nullopt}));
    CHECK(bg::run_graph(reference, input).equals<string>({"reference!x", "reference!y"}));
    CHECK(bg::run_graph(constant).equals<string>({"ab"}));
}

TEST_CASE("string optional addition substitutes the empty string for invalid operands",
          "[feature:operator][feature:validity][validity:optional][series:value][scalar:string]") {
    struct invalidating_source {
        In<TS<string>> value;
        In<TS<bool>> invalidate;
        Out<TS<string>> output;

        void evaluate() const {
            if (*invalidate) {
                output.invalidate();
                return;
            }

            output.copy(value);
        }
    };

    const auto source = node<invalidating_source>();
    const auto strict = [&](wp<TS<string>> lhs, wp<TS<bool>> invalidate_lhs,
                            wp<TS<string>> rhs, wp<TS<bool>> invalidate_rhs) {
        return source(lhs, invalidate_lhs) + source(rhs, invalidate_rhs);
    };
    const auto optional_lhs = [&](wp<TS<string>> lhs, wp<TS<bool>> invalidate_lhs,
                                  wp<TS<string>> rhs, wp<TS<bool>> invalidate_rhs) {
        return opt(source(lhs, invalidate_lhs)) + source(rhs, invalidate_rhs);
    };
    const auto optional_rhs = [&](wp<TS<string>> lhs, wp<TS<bool>> invalidate_lhs,
                                  wp<TS<string>> rhs, wp<TS<bool>> invalidate_rhs) {
        return source(lhs, invalidate_lhs) + opt(source(rhs, invalidate_rhs));
    };
    const auto optional_plus_scalar = [&](wp<TS<string>> value, wp<TS<bool>> invalidate) {
        return opt(source(value, invalidate)) + std::string_view{"!"};
    };
    const auto scalar_plus_optional = [&](wp<TS<string>> value, wp<TS<bool>> invalidate) {
        return std::string_view{"!"} + opt(source(value, invalidate));
    };
    const auto lhs = std::array<std::string_view, 4>{"a", "b", "c", "d"};
    const auto rhs = std::array<std::string_view, 4>{"x", "y", "z", "q"};
    const auto invalidate_lhs = std::array{false, true, false, true};
    const auto invalidate_rhs = std::array{false, false, true, true};
    CHECK(bg::run_graph(strict, lhs, invalidate_lhs, rhs, invalidate_rhs)
              .equals<string>({"ax", std::nullopt, std::nullopt, std::nullopt}));
    CHECK(bg::run_graph(optional_lhs, lhs, invalidate_lhs, rhs, invalidate_rhs)
              .equals<string>({"ax", "y", "c", ""}));
    CHECK(bg::run_graph(optional_rhs, lhs, invalidate_lhs, rhs, invalidate_rhs)
              .equals<string>({"ax", "y", "c", ""}));
    CHECK(bg::run_graph(optional_plus_scalar, lhs, invalidate_lhs)
              .equals<string>({"a!", "!", "c!", "!"}));
    CHECK(bg::run_graph(scalar_plus_optional, lhs, invalidate_lhs)
              .equals<string>({"!a", "!", "!c", "!"}));
}

TEST_CASE("string optional addition waits for ticks and retains the latest input values",
          "[feature:operator][feature:validity][validity:optional][series:value][scalar:string]") {
    const auto optional_add = [](wp<TS<string>> lhs, wp<TS<string>> rhs) {
        return opt(lhs) + opt(rhs);
    };
    const auto lhs = std::vector<std::optional<std::string_view>>{
        std::nullopt, std::string_view{"a"}, std::nullopt, std::string_view{"b"}};
    const auto rhs = std::vector<std::optional<std::string_view>>{
        std::nullopt, std::nullopt, std::string_view{"x"}, std::nullopt};

    CHECK(bg::run_graph(optional_add, lhs, rhs).equals<string>({std::nullopt, "a", "ax", "bx"}));
}

TEST_CASE("addition does not convert an integer scalar to double",
          "[feature:operator][series:value][scalar:integer][scalar:floating-point]") {
    const auto graph = [](wp<TS<double>> value) { return value + 1; };
    const auto direct = [](wp<TS<double>> value) { return operators::add(value, 1); };

    CHECK_THROWS_AS(bg::run_graph(graph, {2.0}), WiringError);
    CHECK_THROWS_AS(bg::run_graph(direct, {2.0}), WiringError);
}

TEST_CASE("opt selects addition that substitutes zero for either invalid operand",
          "[feature:operator][feature:validity][validity:optional][series:value][scalar:floating-point]") {
    struct invalidating_source {
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

    const auto source = node<invalidating_source>();
    const auto strict = [](wp<TS<double>> lhs, wp<TS<double>> rhs) { return lhs + rhs; };
    const auto optional_lhs = [&](wp<TS<double>> lhs, wp<TS<bool>> invalidate_lhs,
                                  wp<TS<double>> rhs, wp<TS<bool>> invalidate_rhs) {
        return opt(source(lhs, invalidate_lhs)) + source(rhs, invalidate_rhs);
    };
    const auto optional_rhs = [&](wp<TS<double>> lhs, wp<TS<bool>> invalidate_lhs,
                                  wp<TS<double>> rhs, wp<TS<bool>> invalidate_rhs) {
        return source(lhs, invalidate_lhs) + opt(source(rhs, invalidate_rhs));
    };
    const auto strict_graph = [&](wp<TS<double>> lhs, wp<TS<bool>> invalidate_lhs,
                                  wp<TS<double>> rhs, wp<TS<bool>> invalidate_rhs) {
        return strict(source(lhs, invalidate_lhs), source(rhs, invalidate_rhs));
    };
    const auto optional_series_plus_scalar = [&](wp<TS<double>> value, wp<TS<bool>> invalidate) {
        return opt(source(value, invalidate)) + 2.0;
    };
    const auto scalar_plus_optional_series = [&](wp<TS<double>> value, wp<TS<bool>> invalidate) {
        return 2.0 + opt(source(value, invalidate));
    };
    const auto lhs = std::array{1.0, 2.0, 3.0, 4.0};
    const auto rhs = std::array{10.0, 20.0, 30.0, 40.0};
    const auto invalidate_lhs = std::array{false, true, false, true};
    const auto invalidate_rhs = std::array{false, false, true, true};

    CHECK(bg::run_graph(strict_graph, lhs, invalidate_lhs, rhs, invalidate_rhs)
              .equals<double>({11.0, std::nullopt, std::nullopt, std::nullopt}));
    CHECK(bg::run_graph(optional_lhs, lhs, invalidate_lhs, rhs, invalidate_rhs)
              .equals<double>({11.0, 20.0, 3.0, 0.0}));
    CHECK(bg::run_graph(optional_rhs, lhs, invalidate_lhs, rhs, invalidate_rhs)
              .equals<double>({11.0, 20.0, 3.0, 0.0}));
    CHECK(bg::run_graph(optional_series_plus_scalar, lhs, invalidate_lhs).equals<double>({3.0, 2.0, 5.0, 2.0}));
    CHECK(bg::run_graph(scalar_plus_optional_series, lhs, invalidate_lhs).equals<double>({3.0, 2.0, 5.0, 2.0}));
}

TEST_CASE("optional addition does not evaluate before either input ticks",
          "[feature:operator][feature:validity][validity:optional][series:value][scalar:floating-point]") {
    const auto optional_add = [](wp<TS<double>> lhs, wp<TS<double>> rhs) {
        return opt(lhs) + opt(rhs);
    };
    const auto no_updates = std::array<std::optional<double>, 2>{std::nullopt, std::nullopt};

    CHECK(bg::run_graph(optional_add, no_updates, no_updates).equals<double>({std::nullopt, std::nullopt}));
}

TEST_CASE("integer and duration optional pairs substitute zero for invalid inputs",
          "[feature:operator][feature:validity][validity:optional][series:value]"
          "[scalar:i64][scalar:timedelta]") {
    const auto invalidate_integer = node([](In<TS<i64>> value, In<TS<bool>> invalidate, Out<TS<i64>> output) {
        if (*invalidate) {
            output.invalidate();
            return;
        }

        output.copy(value);
    });
    const auto invalidate_duration = node([](In<TS<timedelta_t>> value, In<TS<bool>> invalidate,
                                             Out<TS<timedelta_t>> output) {
        if (*invalidate) {
            output.invalidate();
            return;
        }

        output.copy(value);
    });

    const auto optional_integer_add = [&](wp<TS<i64>> lhs, wp<TS<bool>> invalidate_lhs,
                                          wp<TS<i64>> rhs, wp<TS<bool>> invalidate_rhs) {
        return opt(invalidate_integer(lhs, invalidate_lhs)) + opt(invalidate_integer(rhs, invalidate_rhs));
    };
    const auto optional_duration_add = [&](wp<TS<timedelta_t>> lhs, wp<TS<bool>> invalidate_lhs,
                                           wp<TS<timedelta_t>> rhs, wp<TS<bool>> invalidate_rhs) {
        return opt(invalidate_duration(lhs, invalidate_lhs)) + opt(invalidate_duration(rhs, invalidate_rhs));
    };
    const auto integer_lhs = std::array<i64, 4>{1, 2, 3, 4};
    const auto integer_rhs = std::array<i64, 4>{10, 20, 30, 40};
    const auto duration_lhs = std::array{timedelta_t{1}, timedelta_t{2}, timedelta_t{3}, timedelta_t{4}};
    const auto duration_rhs = std::array{timedelta_t{10}, timedelta_t{20}, timedelta_t{30}, timedelta_t{40}};
    const auto invalidate_lhs = std::array{false, true, false, true};
    const auto invalidate_rhs = std::array{false, false, true, true};

    CHECK(bg::run_graph(optional_integer_add, integer_lhs, invalidate_lhs, integer_rhs, invalidate_rhs)
              .equals<i64>({11, 20, 3, 0}));
    CHECK(bg::run_graph(optional_duration_add, duration_lhs, invalidate_lhs, duration_rhs, invalidate_rhs)
              .equals<timedelta_t>({timedelta_t{11}, timedelta_t{20}, timedelta_t{3}, timedelta_t{0}}));
}

TEST_CASE("optional final inputs can produce multiple ordinary results",
          "[feature:operator][feature:validity][validity:optional][series:value]"
          "[scalar:i64][guarantee:final][guarantee:ordinary]") {
    struct final_on_tick {
        In<TS<i64>> input;
        Out<TS<i64, TimeSeriesGuarantee::final>> output;

        void evaluate() const { output.set(*input); }
    };

    const auto final_source = node<final_on_tick>();
    const auto graph = [&](wp<TS<i64>> lhs, wp<TS<i64>> rhs) {
        const auto result = operators::add(opt(final_source(lhs)), opt(final_source(rhs)));
        CHECK(&result.type() == &TS<i64>::type());
        return result;
    };
    const auto lhs = std::array<std::optional<i64>, 3>{i64{2}, std::nullopt, std::nullopt};
    const auto rhs = std::array<std::optional<i64>, 3>{std::nullopt, i64{3}, std::nullopt};

    CHECK(bg::run_graph(graph, lhs, rhs).equals<i64>({2, 5, std::nullopt}));
}

TEST_CASE("addition preserves the shared guarantee for direct time-series inputs",
          "[feature:operator][feature:type-system][series:value][guarantee:ordinary]"
          "[guarantee:deduplicated][guarantee:final][guarantee:const]") {
    struct deduplicated_source {
        In<TS<double>> input;
        Out<TS<double, TimeSeriesGuarantee::deduplicated>> output;

        void evaluate() const { output.set(*input); }
    };
    struct final_scalar_source {
        Scalar<double> value;
        Out<TS<double, TimeSeriesGuarantee::final>> output;

        void start() const { output.set(*value); }
    };

    const auto ordinary = [](wp<TS<double, TimeSeriesGuarantee::ordinary>> lhs,
                             wp<TS<double, TimeSeriesGuarantee::ordinary>> rhs) {
        const auto result = operators::add(lhs, rhs);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };
    const auto deduplicate = node<deduplicated_source>();
    const auto deduplicated = [&](wp<TS<double>> lhs, wp<TS<double>> rhs) {
        const auto result = operators::add(deduplicate(lhs), deduplicate(rhs));
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto scalar_left_deduplicated = [&](wp<TS<double>> value) {
        const auto result = operators::add(2.0, deduplicate(value));
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto scalar_right_deduplicated = [&](wp<TS<double>> value) {
        const auto result = operators::add(deduplicate(value), 2.0);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto final_source = node<final_scalar_source>();
    const auto final = [&] {
        const auto result = operators::add(final_source(2.0), final_source(3.0));
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        return result;
    };
    const auto final_plus_ordinary = [&](wp<TS<double>> ordinary) {
        const auto result = operators::add(final_source(2.0), ordinary);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };
    const auto final_plus_deduplicated = [&](wp<TS<double>> value) {
        const auto result = operators::add(final_source(2.0), deduplicate(value));
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto constant = [] {
        const auto result = operators::add(operators::const_(2.0), operators::const_(3.0));
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::const_>::type());
        return result;
    };

    CHECK(bg::run_graph(ordinary, {1.0, 1.0}, {2.0, 2.0})
              .equals<double>({3.0, 3.0}));
    CHECK(bg::run_graph(deduplicated, {1.0, 2.0, 3.0}, {2.0, 1.0, 1.0})
              .equals<double>({3.0, std::nullopt, 4.0}));
    CHECK(bg::run_graph(scalar_left_deduplicated, {1.0, 2.0})
              .equals<double>({3.0, 4.0}));
    CHECK(bg::run_graph(scalar_right_deduplicated, {1.0, 2.0})
              .equals<double>({3.0, 4.0}));
    CHECK(bg::run_graph(final).equals<double>({5.0}));
    CHECK(bg::run_graph(final_plus_ordinary, {1.0, 2.0}).equals<double>({3.0, 4.0}));
    CHECK(bg::run_graph(final_plus_deduplicated, {1.0, 2.0, 2.0, 3.0})
              .equals<double>({3.0, 4.0, std::nullopt, 5.0}));
    CHECK(bg::run_graph(constant).equals<double>({5.0}));
}

TEST_CASE("timestamp addition requires strict inputs",
          "[feature:operator][feature:validity][validity:required][series:value]"
          "[scalar:evaluation-time][scalar:timedelta]") {
    const auto optional_inputs = [](wp<TS<date_time_t>> timestamp, wp<TS<timedelta_t>> duration) {
        return operators::add(opt(timestamp), opt(duration));
    };

    CHECK_THROWS_AS(bg::run_graph(optional_inputs, {MIN_DT}, {timedelta_t{1}}),
                    WiringError);
}

TEST_CASE("addition handles signed integer, duration, and timestamp values",
          "[feature:operator][series:value][scalar:i64][scalar:evaluation-time][scalar:timedelta]") {
    const auto integers = std::array<i64, 2>{10, -4};
    const auto integer_series_plus_scalar = [](wp<TS<i64>> value) { return operators::add(value, i64{3}); };
    const auto scalar_plus_integer_series = [](wp<TS<i64>> value) { return operators::add(i64{3}, value); };
    CHECK(bg::run_graph(integer_series_plus_scalar, integers).equals<i64>({13, -1}));
    CHECK(bg::run_graph(scalar_plus_integer_series, integers).equals<i64>({13, -1}));

    const auto durations = std::array{timedelta_t{10}, timedelta_t{-4}};
    const auto duration_series_plus_scalar = [](wp<TS<timedelta_t>> value) {
        return operators::add(value, timedelta_t{3});
    };
    const auto scalar_plus_duration_series = [](wp<TS<timedelta_t>> value) {
        return operators::add(timedelta_t{3}, value);
    };
    CHECK(bg::run_graph(duration_series_plus_scalar, durations)
              .equals<timedelta_t>({timedelta_t{13}, timedelta_t{-1}}));
    CHECK(bg::run_graph(scalar_plus_duration_series, durations)
              .equals<timedelta_t>({timedelta_t{13}, timedelta_t{-1}}));

    const auto timestamp = MIN_DT + timedelta_t{10};
    const auto timestamps = std::array{timestamp, MIN_DT + timedelta_t{-4}};
    const auto timestamp_series_plus_scalar = [](wp<TS<date_time_t>> value) {
        return operators::add(value, timedelta_t{3});
    };
    const auto scalar_plus_timestamp_series = [](wp<TS<date_time_t>> value) {
        return operators::add(timedelta_t{3}, value);
    };
    CHECK(bg::run_graph(timestamp_series_plus_scalar, timestamps)
              .equals<date_time_t>({MIN_DT + timedelta_t{13}, MIN_DT + timedelta_t{-1}}));
    CHECK(bg::run_graph(scalar_plus_timestamp_series, timestamps)
              .equals<date_time_t>({MIN_DT + timedelta_t{13}, MIN_DT + timedelta_t{-1}}));

    const auto duration_series_plus_timestamp_scalar = [timestamp](wp<TS<timedelta_t>> value) {
        return operators::add(value, timestamp);
    };
    const auto timestamp_scalar_plus_duration_series = [timestamp](wp<TS<timedelta_t>> value) {
        return operators::add(timestamp, value);
    };
    const auto expected = std::array{timestamp + timedelta_t{10}, timestamp + timedelta_t{-4}};
    CHECK(bg::run_graph(duration_series_plus_timestamp_scalar, durations).equals<date_time_t>(
        {expected[0], expected[1]}));
    CHECK(bg::run_graph(timestamp_scalar_plus_duration_series, durations).equals<date_time_t>(
        {expected[0], expected[1]}));

    const auto strict_series_pair = [](wp<TS<date_time_t>> value, wp<TS<timedelta_t>> duration) {
        return operators::add(value, duration);
    };
    const auto reversed_series_pair = [](wp<TS<timedelta_t>> duration, wp<TS<date_time_t>> value) {
        return operators::add(duration, value);
    };
    CHECK(bg::run_graph(strict_series_pair, timestamps, durations).equals<date_time_t>(
        {timestamp + timedelta_t{10}, MIN_DT + timedelta_t{-8}}));
    CHECK(bg::run_graph(reversed_series_pair, durations, timestamps).equals<date_time_t>(
        {timestamp + timedelta_t{10}, MIN_DT + timedelta_t{-8}}));

    const auto scalar_pair = [timestamp] { return operators::add(timestamp, timedelta_t{3}); };
    const auto reversed_scalar_pair = [timestamp] { return operators::add(timedelta_t{3}, timestamp); };
    const auto duration_scalar_pair = [] { return operators::add(timedelta_t{2}, timedelta_t{3}); };
    CHECK(bg::run_graph(scalar_pair).equals<date_time_t>({timestamp + timedelta_t{3}}));
    CHECK(bg::run_graph(reversed_scalar_pair).equals<date_time_t>({timestamp + timedelta_t{3}}));
    CHECK(bg::run_graph(duration_scalar_pair).equals<timedelta_t>({timedelta_t{5}}));
}

TEST_CASE("integer, duration, and timestamp arithmetic wraps at signed boundaries",
          "[feature:operator][series:value][scalar:i64][scalar:evaluation-time][scalar:timedelta]") {
    constexpr auto I64_MIN = std::numeric_limits<i64>::min();
    constexpr auto I64_MAX = std::numeric_limits<i64>::max();
    const auto add_integer = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return operators::add(lhs, rhs); };
    const auto subtract_integer = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) { return operators::sub(lhs, rhs); };
    const auto optional_add_integer = [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) {
        return opt(lhs) + opt(rhs);
    };
    CHECK(bg::run_graph(add_integer, {I64_MAX, I64_MIN}, {1, -1})
              .equals<i64>({I64_MIN, I64_MAX}));
    CHECK(bg::run_graph(subtract_integer, {I64_MIN, I64_MAX}, {1, -1})
              .equals<i64>({I64_MAX, I64_MIN}));
    CHECK(bg::run_graph(optional_add_integer, {I64_MAX}, {1})
              .equals<i64>({I64_MIN}));

    const auto add_duration = [](wp<TS<timedelta_t>> lhs, wp<TS<timedelta_t>> rhs) {
        return operators::add(lhs, rhs);
    };
    const auto subtract_duration = [](wp<TS<timedelta_t>> lhs, wp<TS<timedelta_t>> rhs) {
        return operators::sub(lhs, rhs);
    };
    CHECK(bg::run_graph(add_duration,
                              {timedelta_t::max(), timedelta_t::min()},
                              {timedelta_t{1}, timedelta_t{-1}})
              .equals<timedelta_t>({timedelta_t::min(), timedelta_t::max()}));
    CHECK(bg::run_graph(subtract_duration,
                              {timedelta_t::min(), timedelta_t::max()},
                              {timedelta_t{1}, timedelta_t{-1}})
              .equals<timedelta_t>({timedelta_t::max(), timedelta_t::min()}));

    const auto add_timestamp = [](wp<TS<date_time_t>> timestamp, wp<TS<timedelta_t>> duration) {
        return operators::add(timestamp, duration);
    };
    const auto subtract_duration_from_timestamp = [](wp<TS<date_time_t>> timestamp,
                                                      wp<TS<timedelta_t>> duration) {
        return operators::sub(timestamp, duration);
    };
    const auto subtract_timestamp = [](wp<TS<date_time_t>> lhs, wp<TS<date_time_t>> rhs) {
        return operators::sub(lhs, rhs);
    };
    const auto timestamps = std::array{date_time_t{timedelta_t::max()}, date_time_t{timedelta_t::min()}};
    const auto durations = std::array{timedelta_t{1}, timedelta_t{-1}};
    const auto reversed_timestamps = std::array{timestamps[1], timestamps[0]};
    CHECK(bg::run_graph(add_timestamp, timestamps, durations)
              .equals<date_time_t>({date_time_t{timedelta_t::min()}, date_time_t{timedelta_t::max()}}));
    CHECK(bg::run_graph(subtract_duration_from_timestamp, reversed_timestamps, durations)
              .equals<date_time_t>({date_time_t{timedelta_t::max()}, date_time_t{timedelta_t::min()}}));
    CHECK(bg::run_graph(subtract_timestamp, timestamps, std::array{timestamps[1], timestamps[0]})
              .equals<timedelta_t>({timedelta_t{-1}, timedelta_t{1}}));
}

TEST_CASE("required addition invalidates and revalidates with its inputs",
          "[feature:operator][feature:validity][series:value][validity:required]"
          "[transition:invalidation][transition:revalidation]") {
    struct invalidating_source {
        In<TS<double>> input;
        Out<TS<double>> output;

        void evaluate() const {
            if (*input < 0.0) {
                output.invalidate();
                return;
            }

            output.set(*input);
        }
    };
    struct validity_observer {
        In<TS<double>, optional> input;
        Out<TS<bool>> output;

        void evaluate() const { output.set(input.valid()); }
    };

    const auto source = node<invalidating_source>();
    const auto observe = node<validity_observer>();
    const auto graph = [&](wp<TS<double>> lhs, wp<TS<double>> rhs) {
        return observe(operators::add(source(lhs), source(rhs)));
    };

    const auto actual = bg::run_graph(graph, {1.0, -1.0, 3.0}, {10.0, 10.0, 10.0});

    CHECK(actual.equals<bool>({true, false, true}));
}

TEST_CASE("required addition follows reference target invalidation and revalidation",
          "[feature:operator][feature:reference][feature:validity][series:value][validity:required]"
          "[transition:invalidation][transition:revalidation][binding:reference-to-value]") {
    struct invalidating_source {
        In<TS<double>> input;
        Out<TS<double>> output;

        void evaluate() const {
            if (*input < 0.0) {
                output.invalidate();
                return;
            }

            output.set(*input);
        }
    };
    struct validity_observer {
        In<TS<double>, optional> input;
        Out<TS<bool>> output;

        void evaluate() const { output.set(input.valid()); }
    };

    const auto source = node<invalidating_source>();
    const auto observe = node<validity_observer>();
    const auto pass_reference = node([](In<REF<TS<double>>> input, Out<REF<TS<double>>> output) {
        output.copy(input);
    });
    const auto graph = [&](wp<TS<double>> lhs, wp<TS<double>> rhs) {
        return observe(operators::add(pass_reference(source(lhs)), pass_reference(source(rhs))));
    };

    const auto actual = bg::run_graph(graph, {1.0, -1.0, 3.0}, {10.0, 10.0, 10.0});

    CHECK(actual.equals<bool>({true, false, true}));
}

TEST_CASE("addition retargets an ordinary reference between ordinary targets",
          "[feature:operator][feature:reference][series:value][guarantee:ordinary]"
          "[binding:reference-to-value][transition:update]") {
    struct changing_reference {
        In<TS<bool>> select_right;
        In<REF<TS<double>>> lhs;
        In<REF<TS<double>>> rhs;
        Out<REF<TS<double>, TimeSeriesGuarantee::ordinary>> output;

        void evaluate() const { output.copy(*select_right ? rhs : lhs); }
    };

    const auto select = node<changing_reference>();
    const auto graph = [&](wp<TS<double>> lhs, wp<TS<double>> rhs, wp<TS<bool>> select_right) {
        const auto reference = select(select_right, lhs, rhs);
        CHECK(&reference.type() == &REF<TS<double>, TimeSeriesGuarantee::ordinary>::type());
        return operators::add(reference, 1.0);
    };

    const auto actual = bg::run_graph(
        graph, {10.0, 11.0, 12.0}, {20.0, 21.0, 22.0}, {false, true, false});

    CHECK(actual.equals<double>({11.0, 22.0, 13.0}));
}

TEST_CASE("addition retargets an ordinary reference between final and const targets",
          "[feature:operator][feature:reference][feature:type-system][series:value]"
          "[guarantee:ordinary][guarantee:final][guarantee:const][binding:reference-to-value]"
          "[transition:update]") {
    struct final_value_source {
        Scalar<double> value;
        Out<TS<double, TimeSeriesGuarantee::final>> output;

        void start() const { output.set(*value); }
    };

    struct changing_reference {
        In<TS<bool>> select_right;
        In<REF<TS<double, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>> lhs;
        In<REF<TS<double, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>> rhs;
        Out<REF<TS<double, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>> output;

        void evaluate() const { output.copy(*select_right ? rhs : lhs); }
    };

    const auto select = node<changing_reference>();
    const auto final_source = node<final_value_source>();
    const auto graph = [&](wp<TS<bool>> select_right) {
        const auto lhs = final_source(1.0);
        const auto rhs = operators::const_(2.0);
        const auto reference = select(select_right, lhs, rhs);
        const auto result = operators::add(reference, 1.0);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };

    const auto actual = bg::run_graph(graph, {false, true, false, true});

    CHECK(actual.equals<double>({2.0, 3.0, 2.0, 3.0}));
}

TEST_CASE("addition observes equal values from distinct deduplicated reference targets",
          "[feature:operator][feature:reference][feature:type-system][series:value]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]"
          "[binding:reference-to-value][transition:update]") {
    struct final_value_source {
        Out<TS<double, TimeSeriesGuarantee::final>> output;

        void start() const { output.set(4.0); }
    };
    struct changing_reference {
        In<TS<bool>> select_const;
        In<REF<TS<double, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::deduplicated>> final_value;
        In<REF<TS<double, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::deduplicated>> const_value;
        Out<REF<TS<double, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::deduplicated>> output;

        void evaluate() const { output.copy(*select_const ? const_value : final_value); }
    };

    const auto final_source = node<final_value_source>();
    const auto select = node<changing_reference>();
    const auto graph = [&](wp<TS<bool>> select_const) {
        const auto final_value = final_source();
        const auto const_value = operators::const_(4.0);
        const auto reference = select(select_const, final_value, const_value);
        const auto result = operators::add(reference, 1.0);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };

    const auto actual = bg::run_graph(graph, {false, true, false, true});

    CHECK(actual.equals<double>({5.0, 5.0, 5.0, 5.0}));
}

TEST_CASE("addition runs inside a nested graph",
          "[feature:operator][feature:nested-graph][shape:nested][series:value][scalar:floating-point]") {
    const auto inner = graph(
        "name"_n = "inner_add", [](wp<TS<double>> lhs, wp<TS<double>> rhs) {
            return wp<TS<double>>{lhs + rhs};
        });
    const auto outer = [&](wp<TS<double>> lhs, wp<TS<double>> rhs, wp<TS<bool>> run) {
        return nested(inner, run, lhs, rhs);
    };

    CHECK(bg::run_graph(outer, {1.0, 2.0}, {10.0, 20.0}, {true, true})
              .equals<double>({11.0, 22.0}));
}

TEST_CASE("addition consumes references across a nested graph boundary",
          "[feature:operator][feature:reference][feature:nested-graph][shape:nested][series:value]"
          "[scalar:floating-point][binding:reference-to-value]") {
    const auto inner = graph(
        "name"_n = "inner_add_references",
        [](wp<REF<TS<double>, TimeSeriesGuarantee::const_>> lhs,
           wp<REF<TS<double>, TimeSeriesGuarantee::const_>> rhs) -> wp<TS<double>> {
            const auto result = lhs + rhs;
            CHECK(&result.type() == &TS<double>::type());
            return wp<TS<double>>{result};
        });
    const auto outer = [&](wp<TS<double>> lhs, wp<TS<double>> rhs, wp<TS<bool>> run) {
        return nested(inner, run, lhs, rhs);
    };

    CHECK(bg::run_graph(outer, {1.0, 2.0}, {10.0, 20.0}, {true, true})
              .equals<double>({11.0, 22.0}));
}

TEST_CASE("an invalid input does not make addition publish at startup",
          "[feature:operator][feature:validity][shape:single-node][validity:required]"
          "[transition:first-tick]") {
    const auto graph = [](wp<TS<double>> lhs, wp<TS<double>> rhs) {
        return operators::add(lhs, rhs);
    };
    const auto lhs = std::vector<std::optional<double>>{std::nullopt, 1.0};
    const auto rhs = std::vector<std::optional<double>>{2.0, 2.0};

    CHECK(bg::run_graph(graph, lhs, rhs).equals<double>({std::nullopt, 3.0}));
}
