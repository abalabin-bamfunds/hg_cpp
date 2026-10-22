#include <bg/library.hpp>
#include <bg/literal/sentinel.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/map.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace bg;

namespace {

struct RegexTextSource {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<string>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        const auto found = context->evaluation_time().time_since_epoch().count() % 2 == 0;
        output.set(found ? "prefix-needle-suffix" : "prefix-missing-suffix");
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }
};

struct CompiledRegexGraph {
    std::unique_ptr<CompiledGraph> graph;
    std::size_t output_node;
};

CompiledRegexGraph compile_regex_graph() {
    auto pattern = std::string{"(needle)"};
    auto wiring = GraphWiring{};
    static const auto source = node<RegexTextSource>();
    auto output_identity = std::optional<NodeCallIdentity>{};
    {
        auto context = WiringContext{wiring};
        const auto output = operators::matches(pattern, source());
        output_identity = wiring.node_call_identity(static_cast<WiringPoint>(output));
    }

    wiring.finalize();
    const auto output_node = wiring.node_index(*output_identity);
    return {std::make_unique<CompiledGraph>(wiring), output_node};
}

} // namespace

TEST_CASE("matches performs regex search and publishes only a boolean result",
          "[feature:operator][series:value][scalar:string][scalar:boolean][shape:single-node]") {
    const auto matches = [](wp<TS<string>> text) {
        return operators::matches(std::string_view{"(needle)"}, text);
    };

    CHECK(testing::test_graph(matches, {"a needle here", "needle", "needles", "no match"})
              .equals<bool>({true, true, true, false}));
}

TEST_CASE("matches accepts symbol patterns and subjects and propagates the subject guarantee",
          "[feature:operator][feature:type-system][series:value][scalar:symbol][scalar:boolean]") {
    const auto symbol_match = [] {
        return operators::matches(symbol{"(?<word>café)"}, symbol{"un café ici"});
    };
    const auto constant = [] {
        const auto result = operators::matches(std::string_view{"x"}, std::string_view{"none"});
        CHECK(&result.type() == &TS<bool, TimeSeriesGuarantee::const_>::type());
        return result;
    };
    const auto deduplicated = [](wp<TS<string, TimeSeriesGuarantee::deduplicated>> text) {
        const auto result = operators::matches(std::string_view{"x"}, text);
        CHECK(&result.type() == &TS<bool, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };

    CHECK(testing::test_graph(symbol_match).equals<bool>({true}));
    CHECK(testing::test_graph(constant).equals<bool>({false}));
    CHECK(testing::test_graph(deduplicated,
                              testing::input<TS<string, TimeSeriesGuarantee::deduplicated>>({"x", "x", "y"}))
              .equals<bool>({true, std::nullopt, false}));
}

TEST_CASE("matches accepts independent string and symbol pattern and subject types",
          "[feature:operator][series:value][scalar:string][scalar:symbol]") {
    const auto symbol_pattern = [](wp<TS<string>> text) {
        return operators::matches(symbol{"(?<word>café)"}, text);
    };
    const auto symbol_text = [](wp<TS<symbol>> text) {
        return operators::matches(std::string_view{"(?<word>café)"}, text);
    };

    CHECK(testing::test_graph(symbol_pattern, {"café", "none"}).equals<bool>({true, false}));
    CHECK(testing::test_graph(symbol_text, {symbol{"café"}, symbol{"none"}}).equals<bool>({true, false}));
}

TEST_CASE("matches observes reference-backed subjects when the selected target changes",
          "[feature:operator][feature:reference][series:value][scalar:string][scalar:boolean]"
          "[binding:reference-to-value][transition:update]") {
    const auto matches = [](wp<TS<bool>> condition, wp<TS<string>> left, wp<TS<string>> right) {
        return operators::matches("needle", operators::if_then_else(condition, left, right));
    };

    CHECK(testing::test_graph(matches, std::array{true, false, false, true},
                              std::array<std::string_view, 4>{"needle", "plain", "needle", "plain"},
                              std::array<std::string_view, 4>{"plain", "needle", "plain", "needle"})
              .equals<bool>({true, true, false, false}));
}

TEST_CASE("prepared regex matchers have independent state in mapped graph instances",
          "[feature:operator][feature:map][feature:nested-graph][series:tsd][series:value]"
          "[shape:nested]") {
    const auto copy = node([](In<TSD<i64, TS<bool>>> input, Out<TSD<i64, TS<bool>>> output) {
        output.copy_tick(input);
    });
    const auto tested = [&](wp<TSD<i64, TS<string>>> values) {
        const auto mapped = map([](wp<TS<string>> text) -> wp<TS<bool>> {
            return wp<TS<bool>>{operators::matches("x", text)};
        }, values);
        return copy(mapped);
    };

    const auto actual = testing::test_graph(tested, {{{1, "x"}, {2, "none"}}, {{1, "none"}}, {{2, "x"}}});
    CHECK(actual.equals({{{1, true}, {2, false}}, {{1, false}}, {{2, true}}}));
}

TEST_CASE("equal prepared regex patterns deduplicate matcher nodes",
          "[feature:operator][feature:wiring][feature:graph-compilation][series:value]") {
    const auto source = node([](Out<TS<string>> output) { output.set("x"); });
    const auto tested = [](wp<TS<string>> text) {
        const auto first = operators::matches(std::string_view{"x"}, text);
        const auto second = operators::matches(std::string_view{"x"}, text);
        return operators::and_(first, second);
    };

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)tested(source());
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};

    CHECK(compiled.node_count() == 3);
    CHECK(testing::test_graph([&] { return tested(source()); }).equals<bool>({true}));
}

TEST_CASE("prepared patterns outlive wiring and matchers remain runtime-instance local",
          "[feature:operator][feature:wiring][feature:state][series:value]") {
    auto compiled = compile_regex_graph();
    auto first_global_state = GlobalState{};
    auto second_global_state = GlobalState{};
    auto first_runtime = RuntimeGraph{*compiled.graph, first_global_state};
    auto second_runtime = RuntimeGraph{*compiled.graph, second_global_state};
    const auto first_start = MIN_DT + std::chrono::microseconds{1};
    const auto second_start = MIN_DT + std::chrono::microseconds{2};

    first_runtime.start(first_start);
    second_runtime.start(second_start);
    first_runtime.evaluate(first_start);
    second_runtime.evaluate(second_start);

    const auto first_output = first_runtime.node(compiled.output_node);
    const auto second_output = second_runtime.node(compiled.output_node);
    REQUIRE(first_output.output_last_updated().valid());
    REQUIRE(second_output.output_last_updated().valid());
    CHECK_FALSE(*static_cast<const bool *>(first_output.output_value()));
    CHECK(*static_cast<const bool *>(second_output.output_value()));

    second_runtime.evaluate(second_start + std::chrono::microseconds{1});
    first_runtime.evaluate(first_start + std::chrono::microseconds{1});
    CHECK(*static_cast<const bool *>(first_output.output_value()));
    CHECK_FALSE(*static_cast<const bool *>(second_output.output_value()));

    first_runtime.stop();
    second_runtime.stop();
}

TEST_CASE("match_ accepts symbol patterns and symbol subjects",
          "[feature:operator][series:fixed-tsl][scalar:string][scalar:symbol]") {
    const auto captures = [](wp<TS<symbol>> text) { return operators::match_(symbol{"(x)"}, text); };

    CHECK(testing::test_graph(captures, {symbol{"x"}, symbol{"none"}})
              .equals({{{0, "x"}}, literal::INVALID}));
}

TEST_CASE("replace supports Python replacement references and preserves UTF-8 output",
          "[feature:operator][series:value][scalar:string][shape:single-node]") {
    const auto replace = [](wp<TS<string>> text) {
        return operators::replace(R"((?<word>é)(?<optional>x)?)", R"(\g<word>2-$-\\)", text);
    };

    CHECK(testing::test_graph(replace, {"éx and é", "none"})
              .equals<string>({"é2-$-\\ and é2-$-\\", "none"}));
}

TEST_CASE("replace supports numbered, named, and whole-match references",
          "[feature:operator][series:value][scalar:string]") {
    const auto replace = [](wp<TS<string>> text) {
        return operators::replace(R"((?<word>foo))", R"(\1|\g<0>|\g<word>)", text);
    };
    const auto backreference = [](wp<TS<string>> text) { return operators::matches(R"((.)\1)", text); };

    CHECK(testing::test_graph(replace, {"foo and foo", "none"})
              .equals<string>({"foo|foo|foo and foo|foo|foo", "none"}));
    CHECK(testing::test_graph(backreference, {"book", "abc"}).equals<bool>({true, false}));
}

TEST_CASE("regex patterns and captures preserve embedded NUL",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:string][unicode]") {
    const auto subject = std::string_view{"xa\0by", 5};
    const auto literal_pattern = std::string_view{"a\0b", 3};
    const auto capture_pattern = std::string_view{"(a\0b)", 5};
    const auto matches = [=](wp<TS<string>> text) { return operators::matches(literal_pattern, text); };
    const auto replace = [=](wp<TS<string>> text) { return operators::replace(capture_pattern, "X", text); };
    const auto captures = [=](wp<TS<string>> text) { return operators::match_(capture_pattern, text); };

    CHECK(testing::test_graph(matches, {subject}).equals<bool>({true}));
    CHECK(testing::test_graph(replace, {subject}).equals<string>({"xXy"}));
    CHECK(testing::test_graph(captures, {subject}).equals({{{0, std::string_view{"a\0b", 3}}}}));
}

TEST_CASE("replace emits unmatched groups as empty and handles octal escapes as Unicode",
          "[feature:operator][series:value][scalar:string]") {
    const auto unmatched = [](wp<TS<string>> text) { return operators::replace("(a)?b", R"(\1x)", text); };
    const auto octal = [](wp<TS<string>> text) { return operators::replace("x", R"(\377)", text); };

    CHECK(testing::test_graph(unmatched, {"b", "ab"}).equals<string>({"x", "ax"}));
    CHECK(testing::test_graph(octal, {"x", "no x"}).equals<string>({"ÿ", "no ÿ"}));
}

TEST_CASE("replace follows ICU zero-length match progression",
          "[feature:operator][series:value][scalar:string][transition:update]") {
    const auto replace = [](wp<TS<string>> text) { return operators::replace("x*", "_", text); };

    CHECK(testing::test_graph(replace, {"abxd", ""}).equals<string>({"_a_b__d_", "_"}));
}

TEST_CASE("replace decodes Python control escapes and preserves escaped punctuation",
          "[feature:operator][series:value][scalar:string]") {
    const auto controls = [](wp<TS<string>> text) {
        return operators::replace("x", R"(\n\t\r\a\b\f\v)", text);
    };
    const auto punctuation = [](wp<TS<string>> text) { return operators::replace("x", R"(\&\.)", text); };

    CHECK(testing::test_graph(controls, {"x"}).equals<string>({"\n\t\r\a\b\f\v"}));
    CHECK(testing::test_graph(punctuation, {"x"}).equals<string>({R"(\&\.)"}));
}

TEST_CASE("replace accepts independent string and symbol pattern, replacement, and subject types",
          "[feature:operator][series:value][scalar:string][scalar:symbol]") {
    const auto string_symbol_string = [](wp<TS<string>> text) {
        return operators::replace(std::string_view{"x"}, symbol{"💠"}, text);
    };
    const auto symbol_string_symbol = [](wp<TS<symbol>> text) {
        return operators::replace(symbol{"x"}, std::string_view{"é"}, text);
    };

    CHECK(testing::test_graph(string_symbol_string, {"x🍕x", "none"}).equals<string>({"💠🍕💠", "none"}));
    CHECK(testing::test_graph(symbol_string_symbol, {symbol{"xx"}, symbol{"none"}})
              .equals<string>({"éé", "none"}));
}

TEST_CASE("replace preserves embedded NUL and astral UTF-8 in subjects and replacement text",
          "[feature:operator][series:value][scalar:string][unicode][transition:update]") {
    const auto replace = [](wp<TS<string>> text) {
        return operators::replace("(🍕)", std::string_view{"a\0b", 3}, text);
    };

    CHECK(testing::test_graph(replace, {"left🍕right", std::string_view{"🍕\0🍕", 9}})
              .equals<string>({std::string_view{"lefta\0bright", 12}, std::string_view{"a\0b\0a\0b", 7}}));
}

TEST_CASE("match_ returns capture groups in order and invalidates unmatched groups",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:string][transition:update]") {
    const auto captures = [](wp<TS<string>> text) {
        return operators::match_(R"((?<word>é)(?<optional>x)?)", text);
    };

    CHECK(testing::test_graph(captures, {"éx", "é", "none"})
              .equals({{{0, "é"}, {1, "x"}}, {{0, "é"}, {1, literal::INVALID}}, literal::INVALID}));
}

TEST_CASE("match_ uses a fixed capture size, rejects patterns without captures, and caps const to final",
          "[feature:operator][feature:wiring][feature:type-system][series:fixed-tsl]") {
    const auto captures = [] {
        const auto result = operators::match_("(x)", "x");
        CHECK(&result.type() == &TSL<TS<string, TimeSeriesGuarantee::final>, 1,
                                     TimeSeriesGuarantee::final>::type());
        return result;
    };
    const auto no_captures = [](wp<TS<string>> text) { return operators::match_("x", text); };

    CHECK(testing::test_graph(captures).equals({{{0, "x"}}}));
    auto error_message = std::string{};
    try {
        static_cast<void>(testing::test_graph(no_captures, {"x"}));
    } catch (const WiringError &error) {
        error_message = error.what();
    }
    CHECK(error_message.find("use matches()") != std::string::npos);
}

TEST_CASE("match_ distinguishes an all-unmatched successful match from search failure",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:string]") {
    const auto captures_valid = [](wp<TS<string>> text) {
        return operators::valid(operators::match_("(?:(a)?(b)?)", text));
    };
    const auto matched = [](wp<TS<string>> text) { return operators::matches("(?:(a)?(b)?)", text); };

    CHECK(testing::test_graph(captures_valid, {""}).equals<bool>({false}));
    CHECK(testing::test_graph(matched, {""}).equals<bool>({true}));
}

TEST_CASE("match_ preserves empty captures, omits absent captures, and revalidates after match loss",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:string]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto empty_and_absent = [](wp<TS<string>> text) { return operators::match_(R"((x)?())", text); };
    const auto captures = [](wp<TS<string>> text) { return operators::match_("(x)", text); };

    CHECK(testing::test_graph(empty_and_absent, {"", "x"})
              .equals({{{1, ""}}, {{0, "x"}, {1, ""}}}));
    CHECK(testing::test_graph(captures, {"x", "none", "x"})
              .equals({{{0, "x"}}, literal::INVALID, {{0, "x"}}}));
}

TEST_CASE("regex operators invalidate and revalidate with a required subject",
          "[feature:operator][feature:validity][series:value][scalar:string][scalar:boolean]"
          "[transition:first-tick][transition:invalidation][transition:revalidation]") {
    const auto matches = [](wp<TS<string>> text) { return operators::matches("x", text); };
    const auto replace = [](wp<TS<string>> text) { return operators::replace("x", "y", text); };
    const auto matches_valid = [](wp<TS<string>> text) { return operators::valid(operators::matches("x", text)); };
    const auto replace_valid = [](wp<TS<string>> text) { return operators::valid(operators::replace("x", "y", text)); };

    CHECK(testing::test_graph(matches, {"x", literal::INVALID, "z"}).equals<bool>({true, std::nullopt, false}));
    CHECK(testing::test_graph(replace, {"x", literal::INVALID, "z"}).equals<string>({"y", std::nullopt, "z"}));
    CHECK(testing::test_graph(matches_valid,
                              {literal::SKIP, literal::INVALID, "x", literal::SKIP, literal::INVALID, "x"})
              .equals<bool>({false, std::nullopt, true, std::nullopt, false, true}));
    CHECK(testing::test_graph(replace_valid,
                              {literal::SKIP, literal::INVALID, "x", literal::SKIP, literal::INVALID, "x"})
              .equals<bool>({false, std::nullopt, true, std::nullopt, false, true}));
}

TEST_CASE("regex operations reject malformed UTF-8 patterns, replacements, and subjects",
          "[feature:operator][feature:validity][series:value][scalar:string][unicode]") {
    const auto malformed = std::array<std::string_view, 3>{std::string_view{"\x80", 1},
                                                            std::string_view{"\xC0\xAF", 2},
                                                            std::string_view{"\xF0\x9F", 2}};
    const auto replace_pattern = [](std::string_view pattern, wp<TS<string>> text) {
        return operators::replace(pattern, "x", text);
    };
    const auto replace_text = [](std::string_view replacement, wp<TS<string>> text) {
        return operators::replace("x", replacement, text);
    };
    const auto match = [](std::string_view pattern, wp<TS<string>> text) {
        return operators::match_(pattern, text);
    };
    const auto malformed_subject = [](wp<TS<string>> text) { return operators::matches("x", text); };
    const auto malformed_replace_subject = [](wp<TS<string>> text) { return operators::replace("x", "y", text); };
    const auto malformed_capture_subject = [](wp<TS<string>> text) { return operators::match_("(x)", text); };

    for (const auto value : malformed) {
        CHECK_THROWS_AS(
            testing::test_graph([&](wp<TS<string>> text) { return operators::matches(value, text); }, {"x"}),
            WiringError);
        CHECK_THROWS_AS(
            testing::test_graph([&](wp<TS<string>> text) { return replace_pattern(value, text); }, {"x"}),
            WiringError);
        CHECK_THROWS_AS(
            testing::test_graph([&](wp<TS<string>> text) { return replace_text(value, text); }, {"x"}),
            WiringError);
        CHECK_THROWS_AS(testing::test_graph([&](wp<TS<string>> text) { return match(value, text); }, {"x"}),
                        WiringError);
        CHECK_THROWS_AS(testing::test_graph(malformed_subject, {value}), std::invalid_argument);
        CHECK_THROWS_AS(testing::test_graph(malformed_replace_subject, {value}), std::invalid_argument);
        CHECK_THROWS_AS(testing::test_graph(malformed_capture_subject, {value}), std::invalid_argument);
    }
}

TEST_CASE("regex pattern and replacement errors occur before matching",
          "[feature:operator][series:value][scalar:string][transition:start]") {
    const auto invalid_pattern = [](wp<TS<string>> text) { return operators::matches("(", text); };
    const auto invalid_group = [](wp<TS<string>> text) { return operators::replace("x", R"(\g<2>)", text); };
    const auto invalid_octal = [](wp<TS<string>> text) { return operators::replace("x", R"(\400)", text); };

    CHECK_THROWS_AS(testing::test_graph(invalid_pattern, {"no match"}), WiringError);
    CHECK_THROWS_AS(testing::test_graph(invalid_group, {"no match"}), WiringError);
    CHECK_THROWS_AS(testing::test_graph(invalid_octal, {"no match"}), WiringError);
}
