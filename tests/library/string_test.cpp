#include <bg/library.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

using namespace bg;

TEST_CASE("string multiplication repeats text for scalar and time-series operands",
          "[feature:operator][series:value][scalar:string][scalar:i64][shape:single-node]") {
    const auto scalar = [] { return operators::mul(std::string_view{"ab"}, i64{3}); };
    const auto text_by_scalar = [](wp<TS<string>> text) { return operators::mul(text, i64{2}); };
    const auto scalar_by_text = [](wp<TS<string>> text) { return operators::mul(i64{2}, text); };
    const auto scalar_text_by_series = [](wp<TS<i64>> count) {
        return operators::mul(std::string_view{"xy"}, count);
    };
    const auto text_and_count = [](wp<TS<string>> text, wp<TS<i64>> count) {
        return operators::mul(text, count);
    };

    CHECK(bg::run_graph(scalar).equals<string>({"ababab"}));
    CHECK(bg::run_graph(text_by_scalar, {"a", "bc"}).equals<string>({"aa", "bcbc"}));
    CHECK(bg::run_graph(scalar_by_text, {"a", "bc"}).equals<string>({"aa", "bcbc"}));
    CHECK(bg::run_graph(scalar_text_by_series, {2, 3}).equals<string>({"xyxy", "xyxyxy"}));
    CHECK(bg::run_graph(text_and_count, {"a", "bc"}, {2, 3}).equals<string>({"aa", "bcbcbc"}));
}

TEST_CASE("string multiplication handles nonpositive counts, empty strings, and embedded nulls",
          "[feature:operator][series:value][scalar:string][scalar:i64]") {
    const auto repeat = [](wp<TS<string>> text, wp<TS<i64>> count) { return text * count; };
    CHECK(bg::run_graph(repeat, {"ab", "ab", "ab", "", std::string_view{"a\0b", 3}},
                              {0, -2, 2, std::numeric_limits<i64>::max(), 2})
              .equals<string>({"", "", "abab", "", std::string_view{"a\0ba\0b", 6}}));
}

TEST_CASE("string multiplication rejects overflow and oversized output before producing text",
          "[feature:operator][series:value][scalar:string][scalar:i64]") {
    const auto repeat = [](wp<TS<string>> text) {
        return operators::mul(text, std::numeric_limits<i64>::max());
    };

    CHECK_THROWS_AS(bg::run_graph(repeat, {"abc"}), std::length_error);
    CHECK_THROWS_AS(bg::run_graph(repeat, {"ab"}), std::length_error);
}

TEST_CASE("substr selects code-point slices and defaults the end to the string length",
          "[feature:operator][series:value][scalar:string][scalar:i64][shape:single-node]") {
    const auto to_end = [](wp<TS<string>> text, wp<TS<i64>> start) { return operators::substr(text, start); };
    const auto with_end = [](wp<TS<string>> text, wp<TS<i64>> start, wp<TS<i64>> end) {
        return operators::substr(text, start, end);
    };
    CHECK(bg::run_graph(to_end, {"abcde", "abcde", "abcde", "abcde", "", "abcde"},
                              {-2, -99, 5, std::numeric_limits<i64>::min(), 0, std::numeric_limits<i64>::max()})
              .equals<string>({"de", "abcde", "", "abcde", "", ""}));
    CHECK(bg::run_graph(with_end, {"abcde", "abcde", "abcde", "abcde", std::string_view{"a\0bc", 4}},
                              {-4, 1, 3, std::numeric_limits<i64>::min(), 1},
                              {-1, std::numeric_limits<i64>::min(), 1, std::numeric_limits<i64>::max(), 3})
              .equals<string>({"bcd", "", "", "abcde", std::string_view{"\0b", 2}}));
    CHECK(bg::run_graph(
              [] { return operators::substr(std::string_view{"abcde"}, i64{1}, std::numeric_limits<i64>::max()); })
              .equals<string>({"bcde"}));
}

TEST_CASE("string repetition and substr propagate input guarantees",
          "[feature:operator][feature:type-system][series:value][scalar:string][scalar:i64]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    const auto constant = [] {
        const auto result = operators::mul(std::string_view{"x"}, i64{2});
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::const_>::type());
        return result;
    };
    const auto deduplicated = [](wp<TS<string>> text) {
        const auto value = operators::dedup(text);
        const auto repeated = operators::mul(value, i64{2});
        const auto sliced = operators::substr(value, i64{0}, i64{1});
        CHECK(&repeated.type() == &TS<string, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&sliced.type() == &TS<string, TimeSeriesGuarantee::deduplicated>::type());
        return repeated;
    };
    const auto final_text = node([](Out<TS<string, TimeSeriesGuarantee::final>> output) { output.set("once"); });
    const auto final = [&] {
        const auto text = final_text();
        const auto result = operators::substr(text, i64{0});
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::final>::type());
        return result;
    };
    CHECK(bg::run_graph(constant).equals<string>({"xx"}));
    CHECK(bg::run_graph(deduplicated, {"same", "same", "next"})
              .equals<string>({"samesame", std::nullopt, "nextnext"}));
    CHECK(bg::run_graph(final).equals<string>({"once"}));
}

TEST_CASE("required string operation inputs gate evaluation and output validity",
          "[feature:operator][feature:validity][feature:invalidation][series:value][scalar:string]"
          "[validity:required][transition:invalidation][transition:revalidation]") {
    const auto graph = [](wp<TS<string>> text, wp<TS<i64>> count) {
        return operators::valid(operators::mul(text, count));
    };
    CHECK(bg::run_graph(graph, {literal::SKIP, literal::INVALID, "ab", literal::SKIP, literal::INVALID, "cd"},
                              {2, 2, 2, 2, 2, 2})
              .equals<bool>({false, std::nullopt, true, std::nullopt, false, true}));
}

TEST_CASE("ordinary string repetition publishes equal results on separate input ticks",
          "[feature:operator][series:value][scalar:string][scalar:i64][guarantee:ordinary][transition:update]") {
    const auto repeat = [](wp<TS<string>> text, wp<TS<i64>> count) { return text * count; };
    CHECK(bg::run_graph(repeat, {"same", "same", "same"}, {2, 2, 2})
              .equals<string>({"samesame", "samesame", "samesame"}));
}

TEST_CASE("string getitem indexes code points through wiring-point indexing",
          "[feature:getitem][series:value][scalar:string][scalar:i64][shape:single-node]") {
    const auto index = [](wp<TS<string>> text, wp<TS<i64>> position) { return text[position]; };
    CHECK(bg::run_graph(index, {"abc", "abc", "abc", std::string_view{"a\0b", 3}, "x"},
                              {0, -1, -3, 1, 0})
              .equals<string>({"a", "c", "a", std::string_view{"\0", 1}, "x"}));
}

TEST_CASE("string getitem rejects positive and negative out-of-range indexes",
          "[feature:getitem][series:value][scalar:string][scalar:i64]") {
    const auto index = [](wp<TS<string>> text, wp<TS<i64>> position) { return text[position]; };
    CHECK_THROWS_AS(bg::run_graph(index, {"abc"}, {3}), std::out_of_range);
    CHECK_THROWS_AS(bg::run_graph(index, {"abc"}, {std::numeric_limits<i64>::min()}), std::out_of_range);
    CHECK_THROWS_AS(bg::run_graph(index, {""}, {0}), std::out_of_range);
}

TEST_CASE("string repetition observes reference retargeting",
          "[feature:operator][feature:reference][series:value][scalar:string][scalar:i64]"
          "[binding:reference-to-value][transition:update]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TS<string>> lhs, wp<TS<string>> rhs) {
        return operators::mul(operators::if_then_else(condition, lhs, rhs), i64{2});
    };
    CHECK(bg::run_graph(graph, {true, false, false, true}, {"a", "b", "c", "d"}, {"w", "x", "y", "z"})
              .equals<string>({"aa", "xx", "yy", "dd"}));
}

TEST_CASE("string length, getitem, and substr count Unicode code points",
          "[feature:operator][feature:getitem][series:value][scalar:string][scalar:symbol][scalar:i64]") {
    const auto length = [](wp<TS<string>> text) { return operators::len_(text); };
    const auto symbol_length = [] { return operators::len_(symbol{"é🍕é"}); };
    const auto index = [](wp<TS<string>> text, wp<TS<i64>> position) { return text[position]; };
    const auto slice = [](wp<TS<string>> text, wp<TS<i64>> start, wp<TS<i64>> end) {
        return operators::substr(text, start, end);
    };
    const auto text = std::string_view{"é🍕é"};
    CHECK(bg::run_graph(length, {text, std::string_view{"a\0b", 3}}).equals<i64>({4, 3}));
    CHECK(bg::run_graph(symbol_length).equals<i64>({4}));
    CHECK(bg::run_graph(index, {text, text, text, text}, {0, 1, -1, -2})
              .equals<string>({"é", "🍕", "́", "e"}));
    CHECK(bg::run_graph(slice, {text, text, text, text}, {0, 2, -3, 1},
                              {2, 4, std::numeric_limits<i64>::max(), 3})
              .equals<string>({"é🍕", "é", "🍕é", "🍕e"}));
}

TEST_CASE("Unicode casing uses root-locale mappings and returns string values",
          "[feature:operator][series:value][scalar:string][scalar:symbol]") {
    const auto lower = [](wp<TS<string>> text) { return operators::lower(text); };
    const auto upper = [](wp<TS<string>> text) { return operators::upper(text); };
    const auto fold = [](wp<TS<string>> text) { return operators::casefold(text); };
    CHECK(bg::run_graph(lower, {"ΟΣ", "𐐀", "é"}).equals<string>({"ος", "𐐨", "é"}));
    CHECK(bg::run_graph(upper, {"ß", "é"}).equals<string>({"SS", "É"}));
    CHECK(bg::run_graph(fold, {"Straße"}).equals<string>({"strasse"}));
    CHECK(bg::run_graph([] { return operators::upper(symbol{"ß"}); }).equals<string>({"SS"}));
    CHECK(bg::run_graph([] { return operators::len_(symbol{}); }).equals<i64>({0}));
    CHECK(bg::run_graph([] { return operators::lower(symbol{}); }).equals<string>({""}));
    CHECK_THROWS_AS(bg::run_graph([] { return operators::getitem(symbol{}, i64{0}); }), std::out_of_range);
}

TEST_CASE("Unicode length, indexing, slicing, and casing reject malformed UTF-8 suffixes",
          "[feature:operator][feature:getitem][feature:validity][series:value][scalar:string]") {
    const auto malformed = std::array<std::string_view, 5>{std::string_view{"ok\xF0\x9F", 4},
                                                            std::string_view{"\x80", 1},
                                                            std::string_view{"\xC0\xAF", 2},
                                                            std::string_view{"\xED\xA0\x80", 3},
                                                            std::string_view{"\xF4\x90\x80\x80", 4}};
    const auto length = [](wp<TS<string>> text) { return operators::len_(text); };
    const auto index = [](wp<TS<string>> text) { return text[i64{0}]; };
    const auto slice = [](wp<TS<string>> text) { return operators::substr(text, i64{0}, i64{1}); };
    const auto lower = [](wp<TS<string>> text) { return operators::lower(text); };
    for (const auto text : malformed) {
        CHECK_THROWS_AS(bg::run_graph(length, {text}), std::invalid_argument);
    }
    CHECK_THROWS_AS(bg::run_graph(index, {malformed.front()}), std::invalid_argument);
    CHECK_THROWS_AS(bg::run_graph(slice, {malformed.front()}), std::invalid_argument);
    CHECK_THROWS_AS(bg::run_graph(lower, {malformed.front()}), std::invalid_argument);
    CHECK_THROWS_AS(bg::run_graph([&] { return operators::len_(symbol{malformed.front()}); }),
                     std::invalid_argument);
}

TEST_CASE("Unicode casing preserves ordinary ticks, reference retargeting, and input guarantees",
          "[feature:operator][feature:reference][feature:type-system][series:value][scalar:string]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][transition:update]") {
    const auto repeated = [](wp<TS<string>> text) { return operators::casefold(text); };
    const auto deduplicated = [](wp<TS<string>> text) {
        const auto result = operators::lower(operators::dedup(text));
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto final_text = node([](Out<TS<string, TimeSeriesGuarantee::final>> output) { output.set("é"); });
    const auto final = [&] {
        const auto text = final_text();
        const auto result = operators::upper(text);
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::final>::type());
        return result;
    };
    const auto retarget = [](wp<TS<bool>> condition, wp<TS<string>> left, wp<TS<string>> right) {
        return operators::lower(operators::if_then_else(condition, left, right));
    };

    CHECK(bg::run_graph(repeated, {"Straße", "Straße", "Straße"})
              .equals<string>({"strasse", "strasse", "strasse"}));
    CHECK(bg::run_graph(deduplicated, {"ABC"}).equals<string>({"abc"}));
    CHECK(bg::run_graph(final).equals<string>({"É"}));
    CHECK(bg::run_graph(retarget, std::array{true, false}, std::array<std::string_view, 2>{"É", "Ω"},
                              std::array<std::string_view, 2>{"A", "B"})
              .equals<string>({"é", "b"}));
}

TEST_CASE("Unicode casing uses the observed guarantee of a string reference",
          "[feature:operator][feature:reference][feature:type-system][series:value][scalar:string]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]"
          "[binding:reference-to-value][transition:update]") {
    struct final_value_source {
        Scalar<string> value;
        Out<TS<string, TimeSeriesGuarantee::final>> output;

        void start() const { output.set(*value); }
    };
    struct changing_reference {
        In<TS<bool>> select_right;
        In<REF<TS<string, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>> lhs;
        In<REF<TS<string, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>> rhs;
        Out<REF<TS<string, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>> output;

        void evaluate() const { output.copy(*select_right ? rhs : lhs); }
    };

    const auto final_source = node<final_value_source>();
    const auto select = node<changing_reference>();
    const auto graph = [&](wp<TS<bool>> select_right) {
        const auto lhs = final_source("É");
        const auto rhs = final_source("Ω");
        const auto reference = select(select_right, lhs, rhs);
        const auto result = operators::lower(reference);
        CHECK(&result.type() == &TS<string, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };

    CHECK(bg::run_graph(graph, {false, true, false}).equals<string>({"é", "ω", "é"}));
}
