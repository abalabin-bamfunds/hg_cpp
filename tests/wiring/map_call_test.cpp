#include <bg/map.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace {

using namespace bg;
using namespace bg::literals;

TEST_CASE("map call preprocessing keeps positional and named argument types",
          "[feature:map][feature:wiring][feature:variadic-wiring][series:tsd]") {
    const auto dictionary = node([](Out<TSD<std::int64_t, TS<float>>>) {});

    testing::test_wiring([&] {
        auto call = details::preprocess_map_call(dictionary(), 3, "scale"_n = 2.0, "__key_arg__"_n = 1,
                                                 "__key_set_arg__"_n = std::string{"values"});

        STATIC_CHECK(std::same_as<decltype(call.positional), std::tuple<wp<TSD<std::int64_t, TS<float>>>, int>>);
        STATIC_CHECK(std::same_as<decltype(call.named), std::tuple<details::named_argument<"scale", double>>>);
        STATIC_CHECK(std::same_as<decltype(call.options),
                                  std::tuple<details::named_argument<"__key_arg__", int>,
                                             details::named_argument<"__key_set_arg__", std::string>>>);
        STATIC_CHECK(std::same_as<typename decltype(call)::key_type, std::int64_t>);
        STATIC_CHECK(std::same_as<typename decltype(call)::inserted_key_type,
                                  wp<TS<std::int64_t, TimeSeriesGuarantee::const_>>>);

        REQUIRE(call.key_arg);
        CHECK(call.key_arg->index() == 1);
        REQUIRE(call.key_set_arg);
        REQUIRE(call.key_set_arg->name());
        CHECK(call.key_set_arg->name()->text() == "values");
    });
}

TEST_CASE("map derives graph-function argument types",
          "[feature:map][feature:wiring][feature:variadic-wiring][series:tsd][feature:reference]") {
    using ValueDictionary = details::map_argument_traits<wp<TSD<std::int64_t, TS<float>>>>;
    using ReferenceDictionary =
        details::map_argument_traits<wp<TSD<std::int64_t, REF<TS<float>, TimeSeriesGuarantee::final>>>>;
    using NamedDictionary =
        details::map_argument_traits<details::named_argument<"values", wp<TSD<std::int64_t, TS<float>>>>>;
    using GraphFunctionNamedDictionary = details::map_graph_function_parameter_t<
        details::named_argument<"values", wp<TSD<std::int64_t, TS<float>>>>>;
    using KeySet = details::map_argument_traits<wp<TSS<std::int64_t>>>;
    using PassedDictionary = details::map_argument_traits<pass_through_argument<wp<TSD<std::int64_t, TS<float>>>>>;

    STATIC_CHECK(std::same_as<typename ValueDictionary::per_key_type, wp<TS<float>>>);
    STATIC_CHECK(std::same_as<typename ReferenceDictionary::per_key_type, wp<TS<float>>>);
    STATIC_CHECK(
        std::same_as<typename NamedDictionary::per_key_type, details::named_argument<"values", wp<TS<float>>>>);
    STATIC_CHECK(std::same_as<GraphFunctionNamedDictionary, wp<TS<float>>>);

    STATIC_CHECK(KeySet::key_set_candidate);
    STATIC_CHECK(std::same_as<typename KeySet::per_key_type, wp<TSS<std::int64_t>>>);

    STATIC_CHECK(std::same_as<typename PassedDictionary::per_key_type, wp<TSD<std::int64_t, TS<float>>>>);
    STATIC_CHECK(std::same_as<typename details::map_argument_traits<int>::per_key_type, int>);
}

TEST_CASE("pass-through arguments retain their values and do not supply a map key",
          "[feature:map][feature:wiring][feature:variadic-wiring][series:tsd]") {
    auto argument = pass_through(std::string{"value"});

    CHECK(argument.value == "value");
    STATIC_CHECK(std::same_as<typename details::map_argument_traits<decltype(argument)>::key_type, void>);
}

TEST_CASE("map calls require one concrete key type",
          "[feature:map][feature:wiring][feature:variadic-wiring][series:tsd][series:tss]") {
    using NoKey = details::map_call_traits<int, details::named_argument<"scale", double>>;
    using GenericKey = details::map_call_traits<wp<TSD<SCALAR, TS<float>>>>;
    using DifferentKeys = details::map_call_traits<wp<TSD<std::int64_t, TS<float>>>, wp<TSS<bool>>>;
    using SameKey = details::map_call_traits<wp<TSD<std::int64_t, TS<float>>>, wp<TSS<std::int64_t>>>;

    STATIC_CHECK(NoKey::error == details::MapCallError::no_key_type);
    STATIC_CHECK(GenericKey::error == details::MapCallError::generic_key_type);
    STATIC_CHECK(DifferentKeys::error == details::MapCallError::inconsistent_key_types);
    STATIC_CHECK(SameKey::valid);

    CHECK_THROWS_WITH(details::preprocess_map_call(1), "a map call requires a TSD or TSS argument");

    const auto dictionary = node([](Out<TSD<std::int64_t, TS<float>>>) {});
    const auto keys = node([](Out<TSS<bool>>) {});
    testing::test_wiring([&] {
        CHECK_THROWS_WITH(details::preprocess_map_call(dictionary(), keys()),
                          "all map TSD and TSS arguments must have the same key type");
    });
}

TEST_CASE("map infers a sole TSS as its key set",
          "[feature:map][feature:wiring][feature:variadic-wiring][series:tss]") {
    const auto keys = node([](Out<TSS<std::int64_t>>) {});

    testing::test_wiring([&] {
        auto positional = details::preprocess_map_call(4, keys());
        REQUIRE(positional.key_set_arg);
        CHECK(positional.key_set_arg->index() == 1);

        auto named = details::preprocess_map_call("keys"_n = keys());
        REQUIRE(named.key_set_arg);
        REQUIRE(named.key_set_arg->name());
        CHECK(named.key_set_arg->name()->text() == "keys");

        auto flattened = details::preprocess_map_call("named"_n = 1, 4, keys());
        REQUIRE(flattened.key_set_arg);
        CHECK(flattened.key_set_arg->index() == 1);
    });
}

TEST_CASE("map forwards an unselected TSS to its graph function",
          "[feature:map][feature:wiring][feature:variadic-wiring][series:tss]") {
    using KeySet = details::map_argument_traits<wp<TSS<std::int64_t>>>;

    STATIC_CHECK(KeySet::key_set_candidate);
    STATIC_CHECK(std::same_as<typename KeySet::per_key_type, wp<TSS<std::int64_t>>>);
}

TEST_CASE("multiple TSS candidates require an explicit key-set selector",
          "[feature:map][feature:wiring][feature:variadic-wiring][series:tss]") {
    using Ambiguous = details::map_call_traits<wp<TSS<std::int64_t>>, wp<TSS<std::int64_t>>>;
    using Selected = details::map_call_traits<wp<TSS<std::int64_t>>, wp<TSS<std::int64_t>>,
                                              details::named_argument<"__key_set_arg__", int>>;

    STATIC_CHECK(Ambiguous::error == details::MapCallError::ambiguous_key_set);
    STATIC_CHECK(Selected::valid);

    const auto keys = node([](Out<TSS<std::int64_t>>) {});
    testing::test_wiring([&] {
        CHECK_THROWS_WITH(details::preprocess_map_call(keys(), keys()),
                          "a map call with multiple TSS arguments requires __key_set_arg__");

        auto selected = details::preprocess_map_call(keys(), keys(), "__key_set_arg__"_n = 1);
        STATIC_CHECK(std::tuple_size_v<decltype(selected.positional)> == 2);
        REQUIRE(selected.key_set_arg);
        CHECK(selected.key_set_arg->index() == 1);
    });
}

TEST_CASE("a selected TSD remains an argument of the mapped graph",
          "[feature:map][feature:wiring][feature:variadic-wiring][series:tsd]") {
    using Dictionary = wp<TSD<std::int64_t, TS<float>>>;
    using Selected = details::map_call_traits<Dictionary, details::named_argument<"__key_set_arg__", std::string>>;

    STATIC_CHECK(Selected::valid);
    STATIC_CHECK(std::same_as<typename details::map_argument_traits<Dictionary>::per_key_type, wp<TS<float>>>);
}

TEST_CASE("pass-through collections are excluded from map key candidates",
          "[feature:map][feature:wiring][feature:variadic-wiring][series:tsd][series:tss]") {
    using PassedDictionary = pass_through_argument<wp<TSD<std::int64_t, TS<float>>>>;
    using PassedSet = pass_through_argument<wp<TSS<std::int64_t>>>;

    STATIC_CHECK(details::map_call_traits<PassedDictionary>::error == details::MapCallError::no_key_type);
    STATIC_CHECK(details::map_call_traits<PassedSet>::error == details::MapCallError::no_key_type);
}

TEST_CASE("map call preprocessing rejects duplicate options",
          "[feature:map][feature:wiring][feature:variadic-wiring][series:tsd]") {
    const auto dictionary = node([](Out<TSD<std::int64_t, TS<float>>>) {});

    testing::test_wiring([&] {
        CHECK_THROWS_WITH(details::preprocess_map_call(dictionary(), "__key_arg__"_n = 0, "__key_arg__"_n = 1),
                          "a map call accepts one __key_arg__ option");
        CHECK_THROWS_WITH(details::preprocess_map_call(dictionary(), "__key_set_arg__"_n = 0, "__key_set_arg__"_n = 1),
                          "a map call accepts one __key_set_arg__ option");
    });
}

TEST_CASE("runtime map call preprocessing rejects invalid options",
          "[feature:map][feature:wiring][feature:variadic-wiring][definition:runtime]") {
    const auto duplicate_options = std::array{
        MapArgument{WiringArgument{util::symbol{"__key_arg__"}, std::int64_t{0}}},
        MapArgument{WiringArgument{util::symbol{"__key_arg__"}, std::int64_t{1}}},
    };
    CHECK_THROWS_WITH(details::preprocess_runtime_map_call(duplicate_options),
                      "a map call accepts one __key_arg__ option");

    const auto pass_through_option = std::array{
        MapArgument{WiringArgument{util::symbol{"__key_set_arg__"}, std::int64_t{0}}, true},
    };
    CHECK_THROWS_WITH(details::preprocess_runtime_map_call(pass_through_option),
                      "a map option cannot be pass-through");

    const auto invalid_selector = std::array{
        MapArgument{WiringArgument{util::symbol{"__key_arg__"}, true}},
    };
    CHECK_THROWS_WITH(details::preprocess_runtime_map_call(invalid_selector),
                      "a map argument selector must be an integer or a name");
}

TEST_CASE("map selectors preserve supported index and name forms",
          "[feature:map][feature:wiring][feature:variadic-wiring]") {
    const auto from_view = details::MapArgumentSelector{std::string_view{"value"}};
    const auto from_symbol = details::MapArgumentSelector{util::symbol{"other"}};

    REQUIRE(from_view.name());
    CHECK(from_view.name()->text() == "value");
    REQUIRE(from_symbol.name());
    CHECK(from_symbol.name()->text() == "other");
    CHECK_THROWS_WITH(details::MapArgumentSelector{-1}, "a map argument selector index must be non-negative");
}

} // namespace
