#include <bg/library.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <stdexcept>
#include <string_view>

using namespace bg;
using namespace bg::literals;

TEST_CASE("split defaults to unlimited splitting for scalar and time-series text",
          "[feature:operator][series:value][scalar:string]") {
    const auto scalar = [] {
        return operators::join(operators::split(std::string_view{"a,,b"}, ","), "|");
    };
    const auto series = [](wp<TS<string>> text) { return operators::join(operators::split(text, ","), "|"); };

    const auto scalar_result = testing::test_graph(scalar);
    CHECK(scalar_result.equals<string>({"a||b"}));
    CHECK(scalar_result.size() == 1);
    CHECK(testing::test_graph(series, {"a,b", "c,,d"}).equals<string>({"a|b", "c||d"}));
}

TEST_CASE("split maxsplit preserves the remaining text",
          "[feature:operator][series:value][scalar:string]") {
    const auto split = [](wp<TS<string>> text) {
        return operators::join(operators::split(text, ",", i64{1}), "|");
    };

    CHECK(testing::test_graph(split, {"a,b,c", "x,,y"}).equals<string>({"a|b,c", "x|,y"}));
}

TEST_CASE("split waits for required maxsplit and gates output validity",
          "[feature:operator][feature:validity][series:value][scalar:string][scalar:i64]"
          "[validity:required][transition:first-tick][transition:invalidation][transition:revalidation]") {
    const auto split = [](wp<TS<string>> text, wp<TS<i64>> maxsplit) {
        return operators::join(operators::split(text, ",", maxsplit), "|");
    };
    const auto valid = [](wp<TS<string>> text, wp<TS<i64>> maxsplit) {
        return operators::valid(operators::join(operators::split(text, ",", maxsplit), "|"));
    };

    CHECK(testing::test_graph(split, {"a,b,c", literal::SKIP, "d,e,f", "g,h,i", "j,k,l", "m,n"},
                              {literal::SKIP, 1, literal::SKIP, 2, literal::INVALID, 1})
              .equals<string>({std::nullopt, "a|b,c", "d|e,f", "g|h|i", std::nullopt, "m|n"}));
    CHECK(testing::test_graph(valid, {"a,b", literal::INVALID, "c,d"}, {1, 1, 1})
              .equals<bool>({true, false, true}));
}

TEST_CASE("split preserves empty pieces and accepts scalar maxsplit",
          "[feature:operator][series:value][scalar:string][scalar:i64]") {
    const auto empty_size = [] { return operators::len_(operators::split(std::string_view{""}, ",")); };
    const auto limited_size = [] {
        return operators::len_(operators::split(std::string_view{",a,"}, ",", i64{1}));
    };

    CHECK(testing::test_graph(empty_size).equals<i64>({1}));
    CHECK(testing::test_graph(limited_size).equals<i64>({2}));
    CHECK_THROWS_AS(testing::test_graph([] { return operators::split(std::string_view{"a"}, ""); }),
                     std::invalid_argument);
}

TEST_CASE("split supports an explicit fixed TSL result",
          "[feature:operator][series:value][scalar:string]") {
    const auto split = [](wp<TS<string>> text) {
        const auto pieces = operators::split[type<TSL<TS<string>, 3>>](text, ",");
        return operators::join(pieces, "|");
    };

    CHECK(testing::test_graph(split, {"a,b,c", "x,y,z"}).equals<string>({"a|b|c", "x|y|z"}));
    CHECK_THROWS_AS(testing::test_graph(split, {"a,b"}), std::invalid_argument);
}

TEST_CASE("fixed split maxsplit preserves the final remainder",
          "[feature:operator][series:fixed-tsl][scalar:string][scalar:i64]") {
    const auto split = [] {
        const auto pieces = operators::split[type<TSL<TS<string>, 4>>]("a,b,c,d,e", ",", i64{3});
        return operators::join(pieces, "|");
    };

    CHECK(testing::test_graph(split).equals<string>({"a|b|c|d,e"}));
}

TEST_CASE("split and join preserve symbol, mixed, Unicode, and embedded NUL text",
          "[feature:operator][series:value][scalar:string]") {
    const auto symbol_text = [] {
        return operators::join(operators::split(symbol{"left::right"}, "::"), symbol{"💠"});
    };
    const auto symbol_separator = [] {
        return operators::join(operators::split(std::string_view{"left💠right"}, symbol{"💠"}), "::");
    };
    const auto nul_separator = [] {
        const auto text = std::string_view{"a\0b\0c", 5};
        const auto separator = std::string_view{"\0", 1};
        return operators::join(operators::split(text, separator), separator);
    };
    const auto unicode_separator = [] {
        return operators::join(operators::split("left💠right💠end", "💠"), "|");
    };

    CHECK(testing::test_graph(symbol_text).equals<string>({"left💠right"}));
    CHECK(testing::test_graph(symbol_separator).equals<string>({"left::right"}));
    CHECK(testing::test_graph(nul_separator).equals<string>({std::string_view{"a\0b\0c", 5}}));
    CHECK(testing::test_graph(unicode_separator).equals<string>({"left|right|end"}));
}

TEST_CASE("split accepts explicit list outputs that accept the resolved output type",
          "[feature:operator][feature:wiring][series:value][scalar:string]") {
    const auto explicit_default = [] {
        return operators::join(operators::split[type<TS<List<string>>>](std::string_view{"a,b"}, ","), "|");
    };
    const auto wrong_item = [] {
        return operators::split[type<TS<List<i64>>>](std::string_view{"a,b"}, ",");
    };
    const auto stronger_output = [](wp<TS<string>> text) {
        return operators::split[type<TS<List<string>, TimeSeriesGuarantee::final>>](text, ",");
    };

    CHECK(testing::test_graph(explicit_default).equals<string>({"a|b"}));
    CHECK_THROWS_AS(testing::test_graph(wrong_item), WiringError);
    CHECK_THROWS_AS(testing::test_graph(stronger_output, {"a,b"}), WiringError);
}

TEST_CASE("fixed split rejects invalid output shapes and incompatible guarantees",
          "[feature:operator][feature:wiring][series:fixed-tsl][scalar:string]") {
    const auto zero_size = [] { return operators::split[type<TSL<TS<string>, 0>>](std::string_view{"a"}, ","); };
    const auto wrong_item = [] {
        return operators::split[type<TSL<TS<i64>, 2>>](std::string_view{"a,b"}, ",");
    };
    const auto stronger_output = [](wp<TS<string>> text) {
        return operators::split[type<TSL<TS<string, TimeSeriesGuarantee::final>, 2,
                                            TimeSeriesGuarantee::final>>](text, ",");
    };
    const auto constant_output = [] {
        return operators::split[type<TSL<TS<string, TimeSeriesGuarantee::final>, 2,
                                            TimeSeriesGuarantee::final>>](std::string_view{"a,b"}, ",");
    };

    CHECK_THROWS_AS(testing::test_graph(zero_size), WiringError);
    CHECK_THROWS_AS(testing::test_graph(wrong_item), WiringError);
    CHECK_THROWS_AS(testing::test_graph(stronger_output, {"a,b"}), WiringError);
    CHECK(testing::test_graph(constant_output).size() == 1);
}

TEST_CASE("join supports scalar lists and all-scalar split with maxsplit",
          "[feature:operator][series:value][scalar:string][scalar:i64]") {
    auto memory = allocator{};
    const auto list = ScalarValue<List<string>>{std::array<std::string_view, 3>{"a", "b", "c"}, memory};
    const auto scalar_list = [&] { return operators::join(list.view(), "|"); };
    const auto split = [] {
        return operators::join(operators::split(std::string_view{"a,b,c"}, ",", i64{1}), "|");
    };

    CHECK(testing::test_graph(scalar_list).equals<string>({"a|b|c"}));
    CHECK(testing::test_graph(split).equals<string>({"a|b,c"}));
}

TEST_CASE("scalar-list join supports symbols and mixed separators",
          "[feature:operator][series:value][scalar:string]") {
    auto memory = allocator{};
    const auto list = ScalarValue<List<symbol>>{
        std::array<symbol, 3>{symbol{"a"}, symbol{"b"}, symbol{"c"}}, memory};
    const auto string_separator = [&] { return operators::join(list.view(), "|"); };
    const auto symbol_separator = [&] { return operators::join(list.view(), symbol{"💠"}); };

    CHECK(testing::test_graph(string_separator).equals<string>({"a|b|c"}));
    CHECK(testing::test_graph(symbol_separator).equals<string>({"a💠b💠c"}));
}

TEST_CASE("join accepts free and reference-backed fixed TSL values",
          "[feature:operator][series:fixed-tsl][series:reference][scalar:string]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto join_free = [](wp<TS<string>> first, wp<TS<string>> second) {
        return operators::join(tsl(first, second), "|");
    };
    const auto join_references = [&](wp<TS<string>> first, wp<TS<string>> second) {
        const auto values = tsl(first, second);
        const auto result = operators::join(values.as<REF<TSL<TS<string>, 2>>>(), "|");
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };

    CHECK(testing::test_graph(join_free, testing::input<TS<string>>({"a", "b", "c"}),
                              testing::input<TS<string>>({"x", literal::INVALID, "z"}))
              .equals<string>({"a|x", std::nullopt, "c|z"}));
    CHECK(testing::test_graph(join_references, {"a", "b"}, {"x", "y"}).equals<string>({"a|x", "b|y"}));
}

TEST_CASE("join dereferences references stored in a fixed TSL",
          "[feature:operator][feature:reference][series:fixed-tsl][series:reference][scalar:string]"
          "[binding:reference-to-value][transition:update]") {
    const auto make_references = node([](In<REF<TS<string>>> first, In<REF<TS<string>>> second,
                                         Out<TSL<REF<TS<string>>, 2>> output) {
        output[0].copy(first);
        output[1].copy(second);
    });
    const auto join_references = [&](wp<TS<string>> first, wp<TS<string>> second) {
        return operators::join(make_references(first, second), "|");
    };

    CHECK(testing::test_graph(join_references, {"a", "b"}, {"x", "y"}).equals<string>({"a|x", "b|y"}));
}

TEST_CASE("split and join propagate the weakest input guarantee",
          "[feature:operator][feature:wiring][series:value][scalar:string]") {
    const auto split = [](wp<TS<string, TimeSeriesGuarantee::final>> text,
                          wp<TS<string, TimeSeriesGuarantee::ordinary>> separator)
        -> wp<TS<string, TimeSeriesGuarantee::ordinary>> {
        const auto parts = operators::split(text, separator);
        CHECK(&parts.type() == &TS<List<string>, TimeSeriesGuarantee::ordinary>::type());
        const auto result = operators::join(parts, "|");
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::ordinary>::type());
        return result.as<TS<string, TimeSeriesGuarantee::ordinary>>();
    };
    const auto join = [](wp<TSL<TS<string, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>> values)
        -> wp<TS<string, TimeSeriesGuarantee::final>> {
        const auto result = operators::join(values, "|");
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::final>::type());
        return result.as<TS<string, TimeSeriesGuarantee::final>>();
    };
    const auto optional_join = [](wp<TSL<TS<string, TimeSeriesGuarantee::final>, 2,
                                           TimeSeriesGuarantee::final>> values)
        -> wp<TS<string, TimeSeriesGuarantee::ordinary>> {
        const auto result = operators::join("values"_n = opt(values), "|");
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::ordinary>::type());
        return result.as<TS<string, TimeSeriesGuarantee::ordinary>>();
    };
    const auto scalar_split = [] {
        const auto result = operators::split(std::string_view{"a,b"}, ",");
        CHECK(&result.type() == &TS<List<string>, TimeSeriesGuarantee::const_>::type());
        return operators::join(result, "|");
    };

    CHECK(testing::test_graph(split, testing::input<TS<string, TimeSeriesGuarantee::final>>({"a,b"}), {","})
              .equals<string>({"a|b"}));
    CHECK(testing::test_graph(
              join, testing::input<TSL<TS<string, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>>(
                        {{{0, "a"}, {1, "b"}}}))
              .equals<string>({"a|b"}));
    CHECK(testing::test_graph(optional_join,
                              testing::input<TSL<TS<string, TimeSeriesGuarantee::final>, 2,
                                                 TimeSeriesGuarantee::final>>({{{0, "a"}, {1, "b"}}}))
              .equals<string>({"a|b"}));
    CHECK(testing::test_graph(scalar_split).equals<string>({"a|b"}));
}

TEST_CASE("join requires all TSL items unless the collection is optional",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:string][validity:all-valid]"
          "[validity:optional][transition:invalidation][transition:revalidation]") {
    const auto required = [](wp<TSL<TS<string>, 2>> values) { return operators::join(values, "|"); };
    const auto optional = [](wp<TSL<TS<string>, 2>> values) {
        return operators::join("values"_n = opt(values), "|");
    };

    CHECK(testing::test_graph(required, testing::input<TSL<TS<string>, 2>>(
                                             {literal::SKIP, {{0, "a"}}, {{1, "b"}},
                                              {{0, literal::INVALID}}, {{0, "c"}}}))
              .equals<string>({std::nullopt, std::nullopt, "a|b", std::nullopt, "c|b"}));
    CHECK(testing::test_graph(optional, testing::input<TSL<TS<string>, 2>>(
                                              {literal::SKIP, {{0, "a"}}, {{1, "b"}},
                                               {{0, literal::INVALID}}, {{0, "c"}}}))
              .equals<string>({std::nullopt, "a", "a|b", "b", "c|b"}));

    const auto required_valid = [](wp<TSL<TS<string>, 2>> values) {
        return operators::valid(operators::join(values, "|"));
    };
    const auto optional_valid = [](wp<TSL<TS<string>, 2>> values) {
        return operators::valid(operators::join("values"_n = opt(values), "|"));
    };
    CHECK(testing::test_graph(required_valid, testing::input<TSL<TS<string>, 2>>(
                                                  {{{0, "a"}, {1, "b"}}, {{0, literal::INVALID}}, {{0, "c"}}}))
              .equals<bool>({true, false, true}));
    CHECK(testing::test_graph(optional_valid, testing::input<TSL<TS<string>, 2>>(
                                                  {{{0, "a"}, {1, "b"}}, {{0, literal::INVALID}}, {{0, "c"}}}))
              .equals<bool>({true, std::nullopt, std::nullopt}));
}

TEST_CASE("optional join treats an invalid collection root as empty without startup scheduling",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:string][validity:optional]"
          "[transition:first-tick][transition:invalidation][transition:revalidation]") {
    const auto optional = [](wp<TSL<TS<string>, 2>> values) {
        return operators::join("values"_n = opt(values), "|");
    };
    CHECK(testing::test_graph(optional, testing::input<TSL<TS<string>, 2>>(
                                            {{{0, "a"}, {1, "b"}}, literal::INVALID,
                                             {{0, "c"}, {1, "d"}}}))
              .equals<string>({"a|b", "", "c|d"}));
}

TEST_CASE("required and optional joins require a valid time-series separator",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:string]"
          "[validity:required][transition:invalidation][transition:revalidation]") {
    const auto required = [](wp<TSL<TS<string>, 2>> values, wp<TS<string>> separator) {
        return operators::join(values, separator);
    };
    const auto optional = [](wp<TSL<TS<string>, 2>> values, wp<TS<string>> separator) {
        return operators::join("values"_n = opt(values), separator);
    };
    CHECK(testing::test_graph(required, testing::input<TSL<TS<string>, 2>>(
                                            {{{0, "a"}, {1, "b"}}, {{0, "c"}, {1, "d"}},
                                             {{0, "e"}, {1, "f"}}}),
                              {"|", literal::INVALID, "-"})
              .equals<string>({"a|b", std::nullopt, "e-f"}));
    CHECK(testing::test_graph(optional, testing::input<TSL<TS<string>, 2>>(
                                             {{{0, "a"}, {1, "b"}}, {{0, "c"}, {1, "d"}},
                                              {{0, "e"}, {1, "f"}}}),
                              {"|", literal::INVALID, "-"})
              .equals<string>({"a|b", std::nullopt, "e-f"}));

    const auto required_valid = [](wp<TSL<TS<string>, 2>> input, wp<TS<string>> join_separator) {
        return operators::valid(operators::join(input, join_separator));
    };
    const auto optional_valid = [](wp<TSL<TS<string>, 2>> input, wp<TS<string>> join_separator) {
        return operators::valid(operators::join("values"_n = opt(input), join_separator));
    };
    CHECK(testing::test_graph(required_valid, testing::input<TSL<TS<string>, 2>>(
                                                  {{{0, "a"}, {1, "b"}}, {{0, "c"}, {1, "d"}},
                                                   {{0, "e"}, {1, "f"}}}),
                                      {"|", literal::INVALID, "-"})
              .equals<bool>({true, false, true}));
    CHECK(testing::test_graph(optional_valid, testing::input<TSL<TS<string>, 2>>(
                                                  {{{0, "a"}, {1, "b"}}, {{0, "c"}, {1, "d"}},
                                                   {{0, "e"}, {1, "f"}}}),
                                      {"|", literal::INVALID, "-"})
              .equals<bool>({true, false, true}));
}

TEST_CASE("join handles an empty fixed TSL",
          "[feature:operator][series:fixed-tsl][scalar:string]") {
    const auto join = [] { return operators::join(tsl<TS<string>>(), "|"); };
    CHECK(testing::test_graph(join).equals<string>({""}));
}
