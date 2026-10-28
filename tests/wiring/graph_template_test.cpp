#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/run_graph.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/with_signature.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <variant>
#include <array>
#include <concepts>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace bg;

namespace {

using namespace bg::literals;

struct source_definition {
    Out<TS<float>> output;

    void evaluate() const { output.set(1.0F); }
};

struct scale_definition {
    In<TS<float>> input;
    float factor;
    Out<TS<float>> output;

    void evaluate() const { output.set(*input * factor); }
};

const auto source = node<source_definition>();

const auto scale = node<scale_definition>(
    [](WiringPoint, float factor) { return std::string{"node-factor="} + std::to_string(factor); });

const auto scale_graph = graph(
    "name"_n = "scale_graph",
    "label"_n = [](wp<TS<float>>, float factor) { return std::string{"factor="} + std::to_string(factor); },
    [](wp<TS<float>> input, float factor) -> wp<TS<float>> { return scale(input, factor); });

static_assert(std::same_as<decltype(scale_graph(std::declval<WiringPoint>(), 2.0F)), wp<TS<float>>>);

using NESTED_OUTPUT = TimeSeriesVariable<"NESTED_OUTPUT">;
using GRAPH_RESOLVED = ScalarVariable<"GRAPH_RESOLVED">;
using OUTPUT_ONLY = TimeSeriesVariable<"GRAPH_OUTPUT_ONLY">;
using SHARED_VALUE = ScalarVariable<"GRAPH_SHARED_VALUE">;
using OUTPUT_KEY = ScalarVariable<"GRAPH_OUTPUT_KEY">;
using OUTPUT_VALUE = ScalarVariable<"GRAPH_OUTPUT_VALUE">;

} // namespace

TEST_CASE("a lambda graph function supplies its signature", "[graph_template][feature:wiring][definition:function]") {
    const auto &time_series = TS<float>::type();
    const ResolvedSignature resolved =
        scale_graph.signature().resolve(std::array<const Type *, 2>{&time_series, &float_type()});

    CHECK(resolved.name.text() == "scale_graph");
    REQUIRE(resolved.parameters.size() == 2);
    CHECK(resolved.parameters[0].name.text() == "0");
    CHECK(resolved.parameters[1].name.text() == "1");
    CHECK(resolved.output == &time_series);
}

TEST_CASE("a graph function can receive wiring arguments without adding a graph parameter",
          "[graph_template][feature:wiring]") {
    auto observed_argument_count = std::size_t{};
    auto observed_scalar = i64{};
    auto observed_output_type = static_cast<const TimeSeriesType *>(nullptr);

    const auto inspect = graph(
        "name"_n = "inspect_wiring_arguments",
        [&](const WiringArguments &arguments, i64 value,
            wp<TS<float>> input) -> wp<TS<float>> {
            observed_argument_count = arguments.size();
            observed_scalar = scalar_cast<i64>(*arguments.scalar(0)).value();
            observed_output_type = arguments.resolved_output_type();
            CHECK(&arguments.resolved_type(0) == &cpp_scalar_type<i64>());
            return input;
        });

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)inspect(42, source());
    }

    CHECK(inspect.signature().parameter_count() == 2);
    CHECK(observed_argument_count == 2);
    CHECK(observed_scalar == 42);
    CHECK(observed_output_type == &TS<float>::type());
}

TEST_CASE("a graph can map its output type into a nested implementation",
          "[graph_template][feature:wiring][feature:type-system]") {
    const auto integer_source = node([](Out<TS<i64>>) {});

    const auto generic_output = graph<NESTED_OUTPUT>(
        "name"_n = "generic_output",
        [=]() -> wp<NESTED_OUTPUT> { return wp<NESTED_OUTPUT>{integer_source()}; });

    const auto run = node([](Out<TS<bool>>) {});

    const auto wrapper = graph<NESTED_OUTPUT>(
        "name"_n = "generic_wrapper",
        [=](const WiringArguments &arguments,
            wp<TS<bool>> run_input) -> wp<REF<NESTED_OUTPUT>> {
            const auto mapped_output = generic_output[arguments.type_map()];
            return nested(mapped_output, run_input);
        });

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto output = wrapper[type<NESTED_OUTPUT> = type<TS<i64>>](run());

    auto output_name = std::string{};
    output.type().format_name(output_name);
    CHECK(output_name == "REF<TS<i64>, deduplicated>");
}

TEST_CASE("a graph template expands into the active graph", "[graph_template][feature:wiring]") {
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        const WiringPoint input = source();
        (void)scale_graph(input, 2.0F);
    }

    (void)wiring.complete_pending_wiring();
    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    CHECK(nodes[1].signature().name.text() == "scale_definition");
    CHECK(nodes[0].path().label.text() == "0");

    const WiringPath &node_path = nodes[1].path();
    REQUIRE(node_path.parent != nullptr);
    CHECK(node_path.name.text() == "scale_definition");
    CHECK(node_path.label.text() == "node-factor=2.000000");
    CHECK(node_path.parent->name.text() == "scale_graph");
    CHECK(node_path.parent->label.text() == "factor=2.000000");
}

TEST_CASE("a graph template can omit its output", "[graph_template][feature:wiring]") {
    const auto scale_sink_graph = graph(
        "name"_n = "scale_sink_graph", [](wp<TS<float>> input, float factor) { (void)scale(input, factor); });
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        scale_sink_graph(source(), 2.0F);
    }

    (void)wiring.complete_pending_wiring();
    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[1].path().parent != nullptr);
    CHECK(nodes[1].path().parent->name.text() == "scale_sink_graph");
}

TEST_CASE("callable graph requirements reject before wiring the body",
          "[graph_template][feature:wiring][feature:operator]") {
    auto requirement_calls = 0;
    auto body_calls = 0;

    const auto positive_only = graph(
        "name"_n = "positive_only",
        require = [&](const WiringArguments &arguments) {
            ++requirement_calls;
            return arguments.resolved_output_type() == &TS<float>::type() &&
                   scalar_cast<i64>(*arguments.scalar(0)).value() > 0;
        },
        [&](i64) {
            ++body_calls;
            return source();
        });

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)positive_only(1);
        CHECK_THROWS_AS(positive_only(-1), WiringError);
    }
    CHECK(requirement_calls == 2);
    CHECK(body_calls == 1);

    const auto sink = graph(
        "name"_n = "requirement_sink",
        require = [](const WiringArguments &arguments) { return arguments.resolved_output_type() == nullptr; },
        [](i64) {});
    {
        const auto context = WiringContext{wiring};
        (void)sink(1);
    }

    auto operation = operator_<auto(SCALAR) -> TS<float>>{"requirement_fallback", {"value"}};
    auto selected = std::string_view{};
    operation.overload(graph(
        "name"_n = "positive",
        require = [](const WiringArguments &arguments) {
            return scalar_cast<i64>(*arguments.scalar(0)).value() > 0;
        },
        [&](i64) {
            selected = "positive";
            return source();
        }));
    operation.overload(graph(
        "name"_n = "fallback",
        require = [](const WiringArguments &arguments) {
            return scalar_cast<i64>(*arguments.scalar(0)).value() <= 0;
        },
        [&](i64) {
            selected = "fallback";
            return source();
        }));

    {
        const auto context = WiringContext{wiring};
        (void)operation(2);
        CHECK(selected == "positive");
        (void)operation(-2);
        CHECK(selected == "fallback");
    }
}

TEST_CASE("callable graph requirements return rejection diagnostics with graph labels",
          "[graph_template][feature:wiring]") {
    const auto guarded = graph(
        "name"_n = "guarded",
        "label"_n = [](i64 value) { return std::string{"value="} + std::to_string(value); },
        require = [](const WiringArguments &) { return reject("value is not accepted"); },
        [](i64) { return source(); });

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    try {
        (void)guarded(3);
        FAIL("expected graph requirement rejection");
    } catch (const WiringError &error) {
        CHECK(std::string{error.what()}.contains("value is not accepted"));
    }

    const auto accepted = graph(
        "name"_n = "accepted_label",
        "label"_n = [](i64 value) { return std::string{"value="} + std::to_string(value); },
        require = [](const WiringArguments &) { return accept; },
        [](i64) { return source(); });
    auto accepted_wiring = GraphWiring{};
    {
        const auto accepted_context = WiringContext{accepted_wiring};
        (void)accepted(4);
    }
    (void)accepted_wiring.complete_pending_wiring();
    REQUIRE(accepted_wiring.nodes().size() == 1);
    REQUIRE(accepted_wiring.nodes()[0].path().parent != nullptr);
    CHECK(accepted_wiring.nodes()[0].path().parent->label.text() == "value=4");
}

TEST_CASE("graph outputs infer output-only variables from the selected body result",
          "[graph_template][feature:wiring][feature:type-system]") {
    const auto double_source = node([](Out<TS<double>>) {});
    const auto inferred = graph<OUTPUT_ONLY>(
        "name"_n = "output_only_graph",
        [=]() -> wp<OUTPUT_ONLY> { return wp<OUTPUT_ONLY>{double_source()}; });
    const auto nested = graph(
        "name"_n = "nested_output_only_graph",
        [=]() -> wp<OUTPUT_ONLY> { return wp<OUTPUT_ONLY>{inferred()}; });

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        const auto direct = inferred();
        CHECK(&direct.type() == &TS<double>::type());

        const auto nested_result = nested();
        CHECK(&nested_result.type() == &TS<double>::type());
    }
}

TEST_CASE("graph output inference preserves input mappings and checks missing results",
          "[graph_template][feature:wiring][feature:type-system]") {
    auto body_calls = std::size_t{};
    const auto double_source = node([](Out<TS<double>>) {});
    const auto mismatched_source = node([](Out<TSD<i64, TS<i64>>>) {});
    const auto mismatched = graph(
        "name"_n = "shared_output_mismatch",
        [&, mismatched_source](wp<TS<SHARED_VALUE>>) -> wp<TSD<OUTPUT_KEY, TS<SHARED_VALUE>>> {
            ++body_calls;
            return wp<TSD<OUTPUT_KEY, TS<SHARED_VALUE>>>{mismatched_source()};
        });
    const auto missing = graph<OUTPUT_ONLY>(
        "name"_n = "missing_inferred_output",
        [] { return wp<>{}; });

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        CHECK_THROWS_WITH(mismatched(double_source()),
                          Catch::Matchers::ContainsSubstring("the graph result does not satisfy its output declaration"));
        CHECK_THROWS_WITH(missing(),
                          Catch::Matchers::ContainsSubstring("a wiring-template result does not match its output declaration"));
    }
    CHECK(body_calls == 1);
}

TEST_CASE("output inference invokes later declared resolvers before mapping the result",
          "[graph_template][feature:wiring][feature:type-system]") {
    auto body_calls = std::size_t{};
    auto resolver_calls = std::size_t{};
    const auto concrete = node([](Out<TSD<i64, TS<double>>>) {});
    const auto invalid = graph(
        "name"_n = "inferred_output_resolver_failure",
        resolve<OUTPUT_VALUE> = [&](const TypeMap &) -> const ScalarType & {
            ++resolver_calls;
            throw std::runtime_error{"output resolver rejected inference"};
        },
        [&]() -> wp<TSD<OUTPUT_KEY, TS<OUTPUT_VALUE>>> {
            ++body_calls;
            return wp<TSD<OUTPUT_KEY, TS<OUTPUT_VALUE>>>{concrete()};
        });

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        CHECK_THROWS_WITH(invalid(), Catch::Matchers::ContainsSubstring("output resolver rejected inference"));
    }

    CHECK(body_calls == 1);
    CHECK(resolver_calls == 1);
}

TEST_CASE("output resolvers can read mappings inferred earlier from the graph result",
          "[graph_template][feature:wiring][feature:type-system]") {
    auto observed_key_type = static_cast<const Type *>(nullptr);
    const auto concrete = node([](Out<TSD<i64, TS<double>>>) {});
    const auto inferred = graph(
        "name"_n = "output_resolver_reads_inferred_key",
        resolve<OUTPUT_VALUE> = [&](const TypeMap &type_map) -> const ScalarType & {
            const auto *mapped_key = std::get_if<const Type *>(type_map.find(OUTPUT_KEY::type()));
            if (mapped_key != nullptr) {
                observed_key_type = *mapped_key;
            }
            return double_type();
        },
        [=]() -> wp<TSD<OUTPUT_KEY, TS<OUTPUT_VALUE>>> {
            return wp<TSD<OUTPUT_KEY, TS<OUTPUT_VALUE>>>{concrete()};
        });

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        const auto output = inferred();
        CHECK(&output.type() == &TSD<i64, TS<double>>::type());
    }

    CHECK(observed_key_type == &int64_type());
}

TEST_CASE("with_signature forwards graph output inference to its target",
          "[graph_template][feature:wiring][feature:type-system]") {
    const auto double_source = node([](Out<TS<double>>) {});
    const auto inferred = graph<OUTPUT_ONLY>(
        "name"_n = "wrapped_output_only_graph",
        [=]() -> wp<OUTPUT_ONLY> { return wp<OUTPUT_ONLY>{double_source()}; });
    const auto wrapped = with_signature(inferred, inferred.signature());

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        const auto output = wrapped();
        CHECK(&output.type() == &TS<double>::type());
    }
}

TEST_CASE("callable output guarantee inference is local to its node signature",
          "[graph_template][feature:wiring][feature:node-api][definition:function][series:value]") {
    const auto increment = node([](double value) { return value + 1.0; });

    const auto inferred_output_graph = graph("name"_n = "inferred_callable_output",
                                                 [=](wp<TS<double>> input) { return increment(input); });

    const auto forwarding_graph = graph("name"_n = "forward_inferred_callable_output",
                                            [=](wp<TS<double>> input) { return inferred_output_graph(input); });

    const auto inputs = std::array{1.0, 2.0, 3.0};
    CHECK(bg::run_graph(inferred_output_graph, inputs).equals<double>({2.0, 3.0, 4.0}));
    CHECK(bg::run_graph(forwarding_graph, inputs).equals<double>({2.0, 3.0, 4.0}));
}

TEST_CASE("a graph reference result uses its declaration guarantee",
          "[graph_template][reference][feature:wiring][definition:function][guarantee:ordinary]") {
    const auto pass =
        graph("name"_n = "reference_pass",
                  [](wp<REF<TS<i64>>> input) -> wp<REF<TS<i64>>> { return input; });

    CHECK(pass.signature().output()->guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK_FALSE(static_cast<const TimeSeriesType &>(*pass.signature().parameter(0).type).guarantee().is_resolved());
}

TEST_CASE("a graph result preserves a stronger returned reference for downstream wiring",
          "[graph_template][reference][feature:wiring][guarantee:deduplicated]") {
    const auto source = node([](Out<REF<TS<i64, TimeSeriesGuarantee::deduplicated>,
                                         TimeSeriesGuarantee::deduplicated>>) {});
    const auto pass = graph("name"_n = "preserve_reference_guarantee",
                            [](wp<REF<TS<i64>>> input) -> wp<REF<TS<i64>>> { return input; });
    const auto sink = node([](In<REF<TS<i64, TimeSeriesGuarantee::deduplicated>,
                                    TimeSeriesGuarantee::deduplicated>>) {});

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto output = pass(source());

    CHECK(&output.type() ==
          &REF<TS<i64, TimeSeriesGuarantee::deduplicated>, TimeSeriesGuarantee::deduplicated>::type());
    (void)sink(output);
}

TEST_CASE("an explicit graph result guarantee rejects a weaker returned reference",
          "[graph_template][reference][feature:wiring][guarantee:deduplicated]") {
    using OrdinaryReference = REF<TS<i64, TimeSeriesGuarantee::ordinary>, TimeSeriesGuarantee::ordinary>;
    using DeduplicatedReference =
        REF<TS<i64, TimeSeriesGuarantee::deduplicated>, TimeSeriesGuarantee::deduplicated>;

    const auto source = node([](Out<OrdinaryReference>) {});
    const auto identity = graph("name"_n = "ordinary_reference_identity",
                                [](wp<OrdinaryReference> input) -> wp<OrdinaryReference> { return input; });
    auto parameters = std::vector<Parameter>{identity.signature().parameter(0)};
    const auto explicit_signature =
        Signature{util::symbol{"deduplicated_reference_result"}, std::move(parameters),
                  DeduplicatedReference::type()};
    const auto require_deduplicated = with_signature(identity, explicit_signature);

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    CHECK_THROWS_AS(require_deduplicated(source()), WiringError);
}

TEST_CASE("graph output re-annotation is checked at the cast", "[graph_template][feature:wiring]") {
    const auto invalid =
        graph("name"_n = "invalid_output",
                  [](wp<TS<float>> input) -> wp<TS<double>> { return wp<TS<double>>{input}; });
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    CHECK_THROWS_AS(invalid(source()), WiringError);
}

TEST_CASE("a graph signature resolves a scalar variable declaration", "[graph_template][feature:wiring]") {
    using GRAPH_SCALAR = ScalarVariable<"GRAPH_SCALAR">;

    const auto generic_identity_graph =
        graph("name"_n = "generic_identity_graph",
                  [](wp<TS<GRAPH_SCALAR>> input) -> wp<TS<GRAPH_SCALAR>> { return input; });
    const auto &time_series = TS<double>::type();
    const std::array<const Type *, 1> arguments{&time_series};

    const auto signature = generic_identity_graph.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &time_series);
    CHECK(signature.output == &time_series);
}

TEST_CASE("callable graph requirements can demand scalar-dependent resolvers",
          "[graph_template][feature:wiring][feature:type-system]") {
    auto resolver_calls = std::make_shared<std::size_t>();
    const std::weak_ptr<std::size_t> resolver_lifetime = resolver_calls;
    auto requirement_calls = std::size_t{};
    auto body_calls = std::size_t{};

    const auto resolved_sink = graph(
        "name"_n = "resolved_sink",
        resolve<GRAPH_RESOLVED> = [resolver_calls](const WiringArguments &arguments) -> const ScalarType & {
            ++*resolver_calls;
            const auto value = scalar_cast<i64>(*arguments.scalar(0)).value();
            return value % 2 == 0 ? int64_type() : double_type();
        },
        require = [&](const WiringArguments &arguments) {
            ++requirement_calls;
            const auto *mapping = arguments.type_map().find(GRAPH_RESOLVED::type());
            const auto *resolved = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
            return resolved != nullptr && *resolved != nullptr && scalar_cast<i64>(*arguments.scalar(0)).value() != 3;
        },
        [&](const WiringArguments &arguments, i64) {
            ++body_calls;
            static const auto output = node([](Out<TS<GRAPH_RESOLVED>>) {});
            (void)output(arguments.type_map());
        });
    resolver_calls.reset();

    CHECK_FALSE(resolver_lifetime.expired());
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        resolved_sink(1);
        resolved_sink(2);
        CHECK_THROWS_AS(resolved_sink(3), WiringError);
    }

    (void)wiring.complete_pending_wiring();
    const auto nodes = wiring.nodes();
    const auto retained_resolver_calls = resolver_lifetime.lock();
    REQUIRE(nodes.size() == 2);
    REQUIRE(retained_resolver_calls != nullptr);
    CHECK(nodes[0].signature().output == &TS<double>::type());
    CHECK(nodes[1].signature().output == &TS<i64>::type());
    CHECK(*retained_resolver_calls == 3);
    CHECK(requirement_calls == 3);
    CHECK(body_calls == 2);
}

TEST_CASE("a graph signature resolves a time series variable declaration",
          "[graph_template][type_system][feature:wiring]") {
    using T = TimeSeriesVariable<"T">;
    constexpr auto G = guarantee_variable<"G">;

    const auto identity = graph("name"_n = "generic_time_series_identity",
                                    [](wp<T::with<G>> input) -> wp<T::with<G>> { return input; });
    const auto &list =
        TSL<TS<double, TimeSeriesGuarantee::ordinary>, 2, TimeSeriesGuarantee::final>::type();
    const auto arguments = std::array<const Type *, 1>{&list};

    const auto signature = identity.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &list);
    CHECK(signature.output == &list);
}

namespace {

struct struct_scale_graph {
    wp<TS<float>> input;
    float factor;

    [[nodiscard]] wp<TS<float>> wire() const { return scale(input, factor); }
};

} // namespace

TEST_CASE("a struct graph definition supplies its signature", "[graph_template][feature:wiring]") {
    const auto reflected_scale_graph = graph<struct_scale_graph>();
    const auto &signature = reflected_scale_graph.signature();
    const auto &time_series = TS<float>::type();
    const ResolvedSignature resolved =
        signature.resolve(std::array<const Type *, 2>{&time_series, &float_type()});

    CHECK(resolved.name.text() == "struct_scale_graph");
    REQUIRE(resolved.parameters.size() == 2);
    CHECK(resolved.parameters[0].name.text() == "input");
    CHECK(resolved.parameters[1].name.text() == "factor");

    GraphWiring wiring;
    {
        WiringContext context{wiring};
        (void)reflected_scale_graph(source(), 2.0F);
    }

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 2);
    REQUIRE(wiring.nodes()[1].path().parent != nullptr);
    CHECK(wiring.nodes()[1].path().parent->name.text() == "struct_scale_graph");
}

TEST_CASE("node and graph templates share the wiring interface", "[graph_template][feature:wiring]") {
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        const WiringPoint input = source();
        const WiringTemplate &node_template = scale;
        const WiringTemplate &graph_template = scale_graph;

        REQUIRE(node_template(input, 2.0F));
        REQUIRE(graph_template(input, 3.0F));
    }

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 3);
}

TEST_CASE("deduplicated graph calls retain their first wiring path", "[graph_template][feature:wiring]") {
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        const WiringPoint input = source();
        (void)scale_graph(input, 2.0F);
        (void)scale_graph(input, 2.0F);
    }

    (void)wiring.complete_pending_wiring();
    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[1].path().parent != nullptr);
    CHECK(nodes[1].path().parent->label.text() == "factor=2.000000");
}

TEST_CASE("a compiled graph owns its wiring paths", "[graph_template][feature:wiring]") {
    std::unique_ptr<CompiledGraph> compiled;

    {
        GraphWiring wiring;
        WiringContext context{wiring};
        const WiringPoint input = source();
        (void)scale_graph(input, 2.0F);

        wiring.finalize();
        compiled = std::make_unique<CompiledGraph>(wiring);
    }

    const WiringPath &path = compiled->wiring_path(1);
    REQUIRE(path.parent != nullptr);
    CHECK(path.name.text() == "scale_definition");
    CHECK(path.parent->name.text() == "scale_graph");
    CHECK(path.parent->label.text() == "factor=2.000000");
}
