#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/util/scope_exit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include <array>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>

using namespace bg;
using namespace bg::literals;

TEST_CASE("str_ renders built-in scalar and nested collection values",
          "[feature:operator][scalar:string][scalar:list]") {
    auto memory = allocator{};
    const auto values = ScalarValue<List<string>>{
        std::array<std::string_view, 2>{"left", "right"}, memory};
    const auto render = [&] { return operators::str_(values.view()); };
    const auto boolean = [] { return operators::str_(true); };
    const auto number = [] { return operators::str_(1.0); };

    CHECK(bg::run_graph(render).equals<string>({"['left', 'right']"}));
    CHECK(bg::run_graph(boolean).equals<string>({"True"}));
    CHECK(bg::run_graph(number).equals<string>({"1.0"}));
}

TEST_CASE("str_ renders non-finite floats and normalized date-time values",
          "[feature:operator][scalar:floating-point][scalar:evaluation-time][scalar:timedelta]") {
    const auto positive_infinity = [] { return operators::str_(std::numeric_limits<double>::infinity()); };
    const auto negative_infinity = [] { return operators::str_(-std::numeric_limits<double>::infinity()); };
    const auto not_a_number = [] { return operators::str_(std::numeric_limits<double>::quiet_NaN()); };
    const auto negative_microsecond = [] { return operators::str_(timedelta_t{-1}); };
    const auto negative_day_and_microsecond = [] {
        return operators::str_(timedelta_t{-86'400'000'001LL});
    };
    const auto positive_microsecond = [] { return operators::str_(timedelta_t{1}); };
    const auto epoch = [] { return operators::str_(date_time_t{}); };
    const auto positive_date_microsecond = [] { return operators::str_(date_time_t{timedelta_t{1}}); };
    const auto negative_date_microsecond = [] { return operators::str_(date_time_t{timedelta_t{-1}}); };

    CHECK(bg::run_graph(positive_infinity).equals<string>({"inf"}));
    CHECK(bg::run_graph(negative_infinity).equals<string>({"-inf"}));
    CHECK(bg::run_graph(not_a_number).equals<string>({"nan"}));
    CHECK(bg::run_graph(negative_microsecond).equals<string>({"-1 day, 23:59:59.999999"}));
    CHECK(bg::run_graph(negative_day_and_microsecond).equals<string>({"-2 days, 23:59:59.999999"}));
    CHECK(bg::run_graph(positive_microsecond).equals<string>({"0:00:00.000001"}));
    CHECK(bg::run_graph(epoch).equals<string>({"1970-01-01 00:00:00"}));
    CHECK(bg::run_graph(positive_date_microsecond).equals<string>({"1970-01-01 00:00:00.000001"}));
    CHECK(bg::run_graph(negative_date_microsecond).equals<string>({"1969-12-31 23:59:59.999999"}));
}

TEST_CASE("str_ accepts scalar time-series values",
          "[feature:operator][series:value][scalar:string][scalar:i64]") {
    const auto render = [](wp<TS<i64>> value) { return operators::str_(value); };
    CHECK(bg::run_graph(render, std::array<i64, 2>{7, -2}).equals<string>({"7", "-2"}));
}

TEST_CASE("str_ propagates scalar and time-series guarantees",
          "[feature:operator][series:value][scalar:string][scalar:i64][guarantee:deduplicated]") {
    const auto source = node([](Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output) { output.set(7); });
    testing::test_wiring([&] {
        const auto series_result = operators::str_(source());
        const auto series_format = operators::format_("{:d}", source());
        const auto scalar_format = operators::format_("static");
        CHECK(&series_result.type() == &TS<string, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&series_format.type() == &TS<string, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&scalar_format.type() == &TS<string, TimeSeriesGuarantee::const_>::type());
    });

    testing::test_wiring([] {
        const auto scalar_result = operators::str_(true);
        CHECK(&scalar_result.type() == &TS<string, TimeSeriesGuarantee::const_>::type());
    });
}

TEST_CASE("format_ retains numeric types for positional and named format fields",
          "[feature:operator][scalar:string][scalar:i64][scalar:floating-point]") {
    const auto positional = [] { return operators::format_("{:04d} {:.2f}", i64{7}, 1.25); };
    const auto named = [] {
        return operators::format_("{word}:{number:.2f}", "word"_n = std::string_view{"value"},
                                  "number"_n = 1.25);
    };
    const auto no_arguments = [] { return operators::format_(std::string_view{"static\0text", 11}); };

    CHECK(bg::run_graph(positional).equals<string>({"0007 1.25"}));
    CHECK(bg::run_graph(named).equals<string>({"value:1.25"}));
    CHECK(bg::run_graph(no_arguments).equals<string>({std::string_view{"static\0text", 11}}));
}

TEST_CASE("format_ waits for every positional and named argument to be valid",
          "[feature:operator][series:value][scalar:string][scalar:i64][validity:all-valid]"
          "[transition:invalidation]"
          "[transition:revalidation]") {
    const auto positional = [](wp<TS<i64>> value) {
        return operators::format_("{:d} {:d}", value, i64{10});
    };
    const auto named = [](wp<TS<i64>> value) {
        return operators::format_("{value:d} {constant:d}", "value"_n = value, "constant"_n = i64{10});
    };

    CHECK(bg::run_graph(positional, {1, literal::INVALID, 2}).equals({"1 10", literal::INVALID, "2 10"}));
    CHECK(bg::run_graph(named, {1, literal::INVALID, 2}).equals({"1 10", literal::INVALID, "2 10"}));
}

TEST_CASE("print_ supports scalar and time-series patterns and selects standard output or error",
          "[feature:operator][series:value][scalar:string][scalar:i64][transition:update]") {
    const auto previous_output = std::cout.rdbuf();
    const auto previous_error = std::cerr.rdbuf();
    auto output = std::ostringstream{};
    auto error = std::ostringstream{};
    std::cout.rdbuf(output.rdbuf());
    std::cerr.rdbuf(error.rdbuf());
    const auto restore_stream_buffers = util::scope_exit{[&]() noexcept {
        std::cout.rdbuf(previous_output);
        std::cerr.rdbuf(previous_error);
    }};

    const auto graph = [](wp<TS<string>> pattern, wp<TS<i64>> value) {
        operators::print_("value={:d}", i64{7});
        operators::print_(pattern, value);
        operators::print_(std::string_view{"error\0text", 10}, "__std_err__"_n = true);
        return operators::const_(i64{0});
    };
    (void)bg::run_graph(graph, std::array<std::string_view, 2>{"first={:d}", "second={:d}"},
                              std::array<i64, 2>{1, 2});

    CHECK(output.str() == "value=7\nfirst=1\nsecond=2\n");
    CHECK(error.str() == std::string{"error\0text\n", 11});
}

TEST_CASE("print_ formats time-series ticks and collection changes",
          "[feature:operator][series:value][scalar:string][scalar:i64][transition:invalidation]"
          "[transition:removal]") {
    const auto previous_output = std::cout.rdbuf();
    const auto previous_error = std::cerr.rdbuf();
    auto output = std::ostringstream{};
    auto error = std::ostringstream{};
    std::cout.rdbuf(output.rdbuf());
    std::cerr.rdbuf(error.rdbuf());
    const auto restore_stream_buffers = util::scope_exit{[&]() noexcept {
        std::cout.rdbuf(previous_output);
        std::cerr.rdbuf(previous_error);
    }};

    const auto direct = [](wp<TSD<i64, TSD<i64, TS<i64>>>> book) {
        operators::print_(book);
        operators::print_(book);
        operators::print_(book, "__std_err__"_n = true);
        return operators::const_(i64{0});
    };
    (void)bg::run_graph(direct, {{{1, {{2, 10}}}}, {{1, {{2, literal::REMOVE}}}}});

    CHECK(output.str() == "{1: {2: 10}}\n{1: {2: 10}}\n"
                          "{1: {2: REMOVE}}\n{1: {2: REMOVE}}\n");
    CHECK(error.str() == "{1: {2: 10}}\n{1: {2: REMOVE}}\n");
}

TEST_CASE("print_ formats nested tick arguments and scalar format arguments",
          "[feature:operator][series:value][scalar:string][scalar:i64][transition:invalidation]"
          "[transition:removal]") {
    const auto previous_output = std::cout.rdbuf();
    auto output = std::ostringstream{};
    std::cout.rdbuf(output.rdbuf());
    const auto restore_stream_buffer = util::scope_exit{[&]() noexcept { std::cout.rdbuf(previous_output); }};

    const auto graph = [](wp<TSD<i64, TSD<i64, TS<i64>>>> book, wp<TS<i64>> value) {
        operators::print_("book={} value={}", book, value);
        return operators::const_(i64{0});
    };
    (void)bg::run_graph(graph, {{{1, {{2, 10}}}}, {{1, {{2, literal::REMOVE}}}}, literal::SKIP},
                         {7, literal::INVALID, 8});
    const auto number_graph = [] {
        operators::print_("number={:04d}", i64{7});
        return operators::const_(i64{0});
    };
    (void)bg::run_graph(number_graph);
    auto memory = allocator{};
    const auto values = ScalarValue<List<i64>>{std::array<i64, 2>{1, 2}, memory};
    const auto list_graph = [&] {
        operators::print_("values={values}", "values"_n = values.view());
        return operators::const_(i64{0});
    };
    (void)bg::run_graph(list_graph);

    CHECK(output.str() == "book={1: {2: 10}} value=7\n"
                          "book={1: {2: REMOVE}} value=INVALID\n"
                          "book=SKIP value=8\nnumber=0007\nvalues=[1, 2]\n");
}

TEST_CASE("print_ treats a bare string time series as tick data and prints invalidation",
          "[feature:operator][series:value][scalar:string][scalar:i64][transition:invalidation]") {
    const auto previous_output = std::cout.rdbuf();
    auto output = std::ostringstream{};
    std::cout.rdbuf(output.rdbuf());
    const auto restore_stream_buffer = util::scope_exit{[&]() noexcept { std::cout.rdbuf(previous_output); }};

    const auto string_graph = [](wp<TS<string>> value) {
        operators::print_(value);
        return operators::const_(i64{0});
    };
    (void)bg::run_graph(string_graph, {"book={}"});

    const auto value_graph = [](wp<TS<i64>> value) {
        operators::print_(value);
        return operators::const_(i64{0});
    };
    (void)bg::run_graph(value_graph, {literal::SKIP, 7, literal::INVALID, 8});

    CHECK(output.str() == "\"book={}\"\n7\nINVALID\n8\n");
}

TEST_CASE("print_ tick arguments reject scalar format specs",
          "[feature:operator][series:value][scalar:i64][transition:invalidation]") {
    const auto previous_output = std::cout.rdbuf();
    auto output = std::ostringstream{};
    std::cout.rdbuf(output.rdbuf());
    const auto restore_stream_buffer = util::scope_exit{[&]() noexcept { std::cout.rdbuf(previous_output); }};

    const auto graph = [](wp<TS<i64>> value) {
        operators::print_("value={:04d}", value);
        return operators::const_(i64{0});
    };

    CHECK_THROWS_AS(bg::run_graph(graph, {literal::INVALID}), fmt::format_error);
}

TEST_CASE("print_ binds a reference to its value",
          "[feature:operator][series:reference][binding:reference-to-value][shape:chain]") {
    const auto previous_output = std::cout.rdbuf();
    auto output = std::ostringstream{};
    std::cout.rdbuf(output.rdbuf());
    const auto restore_stream_buffer = util::scope_exit{[&]() noexcept { std::cout.rdbuf(previous_output); }};

    const auto make_reference = node([](In<REF<TS<i64>>> value, Out<REF<TS<i64>>> reference) {
        reference.copy(value);
    });
    const auto graph = [&](wp<TS<i64>> value) {
        operators::print_(make_reference(value.as<REF<TS<i64>>>()));
        return operators::const_(i64{0});
    };

    (void)bg::run_graph(graph, {7});

    CHECK(output.str() == "7\n");
}
