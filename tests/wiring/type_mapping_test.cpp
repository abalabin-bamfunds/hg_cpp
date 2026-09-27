#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/type.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <utility>

namespace {

using namespace bg;
using namespace bg::literals;

using RESULT = TimeSeriesVariable<"RESULT">;
using KEY = ScalarVariable<"KEY", Hashable>;
using VALUE = ScalarVariable<"VALUE">;

struct generic_source_definition {
    Out<RESULT> output;

    void evaluate() const {}
};

struct generic_dictionary_source_definition {
    Out<TSD<KEY, TS<VALUE>>> output;

    void evaluate() const {}
};

struct generic_source_graph_definition {
    wp<RESULT> wire() const {
        const auto source = node([](Out<TS<i64>>) {});
        return wp<RESULT>{source()};
    }
};

template <template <typename, auto...> typename Field> struct concrete_bundle_schema {
    Field<TS<i64>> value;
};

struct generic_bundle_source_definition {
    Out<TSB<SCHEMA>> output;

    void evaluate() const {}
};

struct cached_generic_sink_definition {
    In<RESULT> input;

    inline static auto requirement_calls = 0;

    static RequirementResult require(const WiringArguments &) {
        ++requirement_calls;
        return accept;
    }

    void evaluate() const {}
};

struct recording_operator_invocation {
    inline static bool called{};

    template <typename SelectOverload, typename... Arguments>
    static decltype(auto) invoke(const TypeMap &, SelectOverload &&select_overload, Arguments &&...arguments) {
        called = true;
        return std::invoke(std::forward<SelectOverload>(select_overload),
                           std::forward<Arguments>(arguments)...);
    }
};

struct callable_operator_invocation {
    inline static bool received_type_map{};

    template <typename SelectOverload, typename Function>
    static decltype(auto) invoke(const TypeMap &type_map, SelectOverload &&select_overload, Function &&) {
        received_type_map = type_map.find(details::TimeSeriesVariableInstance<RESULT>::get()) != nullptr;
        return std::invoke(std::forward<SelectOverload>(select_overload));
    }
};

TEST_CASE("a positional type argument resolves a callable node output",
          "[feature:wiring][feature:type-system][definition:function][declaration:generic]") {
    const auto source = node<RESULT>([](Out<RESULT>) {});

    testing::test_wiring([&] {
        const auto output = source[type<TS<i64>>]();
        CHECK(&output.type() == &TS<i64>::type());
    });
}

TEST_CASE("ordered type arguments resolve an aggregate node output",
          "[feature:wiring][feature:type-system][definition:aggregate][declaration:generic][series:tsd]") {
    const auto source = node<KEY, VALUE, generic_dictionary_source_definition>();

    testing::test_wiring([&] {
        const auto output = source[type<i64>, type<float>]();
        CHECK(&output.type() == &TSD<i64, TS<float>>::type());
    });
}

TEST_CASE("a named type mapping does not require positional type parameters",
          "[feature:wiring][feature:type-system][definition:function][declaration:generic]") {
    const auto source = node([](Out<RESULT>) {});

    testing::test_wiring([&] {
        const auto output = source[type<RESULT> = type<TS<i64>>]();
        CHECK(&output.type() == &TS<i64>::type());
    });
}

TEST_CASE("an explicit type argument must agree with a supplied argument",
          "[feature:wiring][feature:type-system][definition:function][declaration:generic]") {
    const auto identity = node<RESULT>([](In<RESULT>, Out<RESULT>) {});
    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        CHECK_NOTHROW(identity[type<TS<i64>>](source()));
        CHECK_THROWS_AS(identity[type<TS<float>>](source()), TypeResolutionError);
    });
}

TEST_CASE("conflicting explicit type arguments are rejected",
          "[feature:wiring][feature:type-system][definition:function][declaration:generic]") {
    const auto source = node<RESULT>([](Out<RESULT>) {});

    CHECK_THROWS_AS((source[type<TS<i64>>, type<RESULT> = type<TS<float>>]), TypeResolutionError);
}

TEST_CASE("an explicit schema argument resolves a bundle schema variable",
          "[feature:wiring][feature:type-system][definition:aggregate][declaration:generic]"
          "[series:tsb][schema:generic]") {
    const auto source = node<schema_type<SCHEMA>, generic_bundle_source_definition>();

    testing::test_wiring([&] {
        const auto output = source[type<schema_type<concrete_bundle_schema>>]();
        CHECK(&output.type() == &TSB<concrete_bundle_schema>::type());
    });
}

TEST_CASE("a positional type argument resolves a callable graph output",
          "[feature:wiring][feature:type-system][definition:function][declaration:generic]") {
    const auto source = node([](Out<TS<i64>>) {});
    const auto function = graph<RESULT>(
        "name"_n = "generic_source", [&]() -> wp<RESULT> { return wp<RESULT>{source()}; });

    testing::test_wiring([&] {
        const auto output = function[type<TS<i64>>]();
        CHECK(&output.type() == &TS<i64>::type());
    });
}

TEST_CASE("a positional type argument resolves an aggregate graph output",
          "[feature:wiring][feature:type-system][definition:aggregate][declaration:generic]") {
    const auto function = graph<RESULT, generic_source_graph_definition>();

    testing::test_wiring([&] {
        const auto output = function[type<TS<i64>>]();
        CHECK(&output.type() == &TS<i64>::type());
    });
}

TEST_CASE("a positional type argument participates in operator overload resolution",
          "[feature:wiring][feature:type-system][feature:operator][definition:function]"
          "[declaration:generic]") {
    auto operation = operator_<RESULT, auto(TS<i64>) -> RESULT>{"convert", {"value"}};
    operation.overload(graph<RESULT>(
        "name"_n = "convert_int", [](wp<TS<i64>> value) -> wp<RESULT> { return wp<RESULT>{value}; }));
    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        const auto output = operation[type<TS<i64>>](source());
        CHECK(&output.type() == &TS<i64>::type());
    });
}

TEST_CASE("a mapped operator call retains its call preparation",
          "[feature:wiring][feature:type-system][feature:operator][definition:function]"
          "[declaration:generic]") {
    auto operation = operator_<RESULT, auto(TS<i64>) -> RESULT, recording_operator_invocation>{
        "convert", {"value"}};
    operation.overload(graph<RESULT>(
        "name"_n = "convert_int", [](wp<TS<i64>> value) -> wp<RESULT> { return wp<RESULT>{value}; }));
    const auto source = node([](Out<TS<i64>>) {});
    recording_operator_invocation::called = false;

    testing::test_wiring([&] { (void)operation[type<TS<i64>>](source()); });

    CHECK(recording_operator_invocation::called);
}

TEST_CASE("operator preparation receives explicit type mappings before validating prepared arguments",
          "[feature:wiring][feature:type-system][feature:operator][definition:function]"
          "[declaration:generic]") {
    auto operation = operator_<RESULT, auto() -> RESULT, callable_operator_invocation>{"make"};
    operation.overload(node<RESULT>([](Out<RESULT>) {}));
    callable_operator_invocation::received_type_map = false;

    testing::test_wiring([&] {
        const auto output = operation[type<TS<i64>>]([] {});
        CHECK(&output.type() == &TS<i64>::type());
    });

    CHECK(callable_operator_invocation::received_type_map);
}

TEST_CASE("a named type mapping resolves an outputless operator overload",
          "[feature:wiring][feature:type-system][feature:operator][definition:function]"
          "[declaration:generic][shape:sink]") {
    auto operation = operator_<auto(RESULT) -> void>{"consume", {"value"}};
    operation.overload(node([](In<TS<i64>>) {}));
    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        operation[type<RESULT> = type<TS<i64>>](source());
    });
}

TEST_CASE("operator resolution caches explicit type mappings",
          "[feature:wiring][feature:type-system][feature:operator][definition:aggregate]"
          "[declaration:generic][shape:sink]") {
    cached_generic_sink_definition::requirement_calls = 0;
    auto operation = operator_<RESULT, auto(RESULT) -> void>{"consume", {"value"}};
    operation.overload(node<cached_generic_sink_definition>());
    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        operation[type<TS<i64>>](source());
        operation[type<TS<i64>>](source());
    });

    CHECK(cached_generic_sink_definition::requirement_calls == 1);
}

TEST_CASE("a mapped wiring proxy can be reused",
          "[feature:wiring][feature:type-system][definition:function][declaration:generic]"
          "[shape:multiple]") {
    const auto source = node<RESULT>([](Out<RESULT>) {});
    const auto integer_source = source[type<TS<i64>>];
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto first = integer_source();
    const auto second = integer_source();

    CHECK(&first.type() == &TS<i64>::type());
    CHECK(&second.type() == &TS<i64>::type());
}

TEST_CASE("a mapped temporary callable node transfers ownership to graph wiring",
          "[feature:wiring][feature:type-system][definition:function][declaration:generic][lifetime]") {
    testing::test_wiring([] {
        const auto output =
            node<RESULT>([](Out<RESULT>) {})[type<TS<i64>>][type<RESULT> = type<TS<i64>>]();
        REQUIRE(output);
        CHECK(&output->type() == &TS<i64>::type());
    });
}

TEST_CASE("a mapped temporary callable graph is valid for its wiring call",
          "[feature:wiring][feature:type-system][definition:function][declaration:generic][lifetime]") {
    testing::test_wiring([] {
        const auto output = graph<RESULT>("name"_n = "temporary", []() -> wp<RESULT> {
                                return wp<RESULT>{*node([](Out<TS<i64>>) {})()};
                            })[type<TS<i64>>]();
        CHECK(&output.type() == &TS<i64>::type());
    });
}

TEST_CASE("an incompatible explicit type argument is rejected",
          "[feature:wiring][feature:type-system][definition:function][declaration:generic]") {
    const auto source = node<RESULT>([](Out<RESULT>) {});

    CHECK_THROWS_AS((source[type<int>]), TypeResolutionError);
}

} // namespace
