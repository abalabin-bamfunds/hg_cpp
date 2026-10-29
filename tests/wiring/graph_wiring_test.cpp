#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/context.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/declaration.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/type/type_resolution_error.hpp>

#include "../value/scalar/test_scalar_provider.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace bg;

namespace {

struct source_definition {
    Out<TS<float>> output;

    void evaluate() const {}
};

struct copy_definition {
    In<TS<float>> input;
    Out<TS<float>> output;

    void evaluate() const { output.set(*input); }
};

const auto aggregate_scalar_sink = node([](Scalar<SCALAR>) {});

struct generic_scalar_forward_graph {
    Scalar<SCALAR> value;

    void wire() const { aggregate_scalar_sink(value); }
};

const auto aggregate_scalar_forward = graph<generic_scalar_forward_graph>();

class OutputRuntimeSelectorNode final : public NodeTemplate {
  public:
    OutputRuntimeSelectorNode(const TimeSeriesRuntimeType &selected_runtime_type, int &selector_calls)
        : signature_{util::symbol{"output_runtime_selector"}, {}, TS<float>::type()},
          selected_runtime_type_{&selected_runtime_type}, selector_calls_{&selector_calls} {}

    const Signature &default_signature() const final { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept final { return nullptr; }
    bool uses_output_runtime_type() const noexcept final { return true; }

    const TimeSeriesRuntimeType *output_runtime_type(const WiringArguments &) const final {
        ++*selector_calls_;
        return selected_runtime_type_;
    }

  private:
    Signature signature_;
    const TimeSeriesRuntimeType *selected_runtime_type_;
    int *selector_calls_;
};

class ThrowingOutputRuntimeSelectorNode final : public NodeTemplate {
  public:
    ThrowingOutputRuntimeSelectorNode()
        : signature_{util::symbol{"throwing_output_runtime_selector"}, {}, TS<float>::type()} {}

    const Signature &default_signature() const final { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept final { return nullptr; }
    bool uses_output_runtime_type() const noexcept final { return true; }

    const TimeSeriesRuntimeType *output_runtime_type(const WiringArguments &) const final {
        throw std::runtime_error{"output descriptor selection failed"};
    }

  private:
    Signature signature_;
};

static_assert(sizeof(WiringPoint) == sizeof(void *));
static_assert(std::is_trivially_copyable_v<WiringPoint>);

} // namespace

TEST_CASE("node calls add wired nodes to the active graph", "[graph_wiring][feature:wiring]") {
    const auto source = node<source_definition>();

    const auto copy = node<copy_definition>();
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        const WiringPoint input = source();
        const WiringPoint output = copy(input);

        CHECK(&input.graph() == &wiring);
        CHECK(&output.graph() == &wiring);
    }

    (void)wiring.complete_pending_wiring();
    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    CHECK(&nodes[0].node_template() == &source);
    CHECK(&nodes[1].node_template() == &copy);
    REQUIRE(nodes[1].inputs().size() == 1);
    CHECK(nodes[1].inputs()[0].index() == 0);
}

TEST_CASE("collected node calls have identities separate from output indices",
          "[graph_wiring][feature:wiring]") {
    const auto source = node<source_definition>();

    const auto consume = node([](In<TS<float>>) {});
    GraphWiring wiring;
    auto source_point = WiringPoint{};
    auto consume_call = GraphWiring::NodeCallResult{};

    {
        WiringContext context{wiring};
        source_point = source();
        const auto arguments = std::array{WiringArgument{source_point}};
        consume_call = wiring.invoke_node(consume, arguments);
    }

    CHECK(wiring.nodes().empty());
    CHECK_FALSE(consume_call.output);
    const auto source_identity = wiring.node_call_identity(source_point);
    REQUIRE(source_identity);
    CHECK(source_identity->value == 0);
    CHECK(consume_call.identity.value == 1);
    CHECK_THROWS_AS(wiring.output_node_index(source_point), WiringError);

    wiring.finalize();

    CHECK(wiring.node_index(*source_identity) == 0);
    CHECK(wiring.node_index(consume_call.identity) == 1);
    CHECK(wiring.output_node_index(source_point) == 0);
}

TEST_CASE("wiring point re-annotation checks type compatibility", "[graph_wiring][feature:wiring]") {
    const auto source = node([](Out<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto typed = source();
    const auto point = static_cast<WiringPoint>(typed);

    STATIC_CHECK(std::same_as<decltype(point.as<TS<float>>()), wp<TS<float>>>);
    STATIC_CHECK(std::same_as<decltype(typed.as<TS<float>>()), wp<TS<float>>>);
    CHECK_NOTHROW(point.as<TS<float>>());
    CHECK_NOTHROW(typed.as<TS<float>>());
    CHECK_NOTHROW(point.as<REF<TS<float>>>());
    CHECK_THROWS_AS(point.as<TS<double>>(), WiringError);
    CHECK_THROWS_AS(wp<TS<double>>{typed}, WiringError);
}

TEST_CASE("wiring point reference annotations check guarantee compatibility",
          "[graph_wiring][feature:wiring][feature:reference][guarantee:ordinary]"
          "[guarantee:deduplicated][guarantee:final]") {
    using DEDUPLICATED_REFERENCE = REF<TS<float>, TimeSeriesGuarantee::deduplicated>;
    using FINAL_REFERENCE = REF<TS<float>, TimeSeriesGuarantee::final>;
    using VARIABLE_REFERENCE =
        REF<TS<float, guarantee_variable<"G1">>, guarantee_variable<"G2">>;

    const auto ordinary_source = node(
        [](Out<REF<TS<float>, TimeSeriesGuarantee::ordinary>>) {});

    const auto deduplicated_source = node(
        [](Out<REF<TS<float>, TimeSeriesGuarantee::deduplicated>>) {});

    const auto final_source = node([](Out<REF<TS<float>, TimeSeriesGuarantee::final>>) {});

    testing::test_wiring([&] {
        const auto ordinary = static_cast<WiringPoint>(ordinary_source());
        CHECK_NOTHROW(wp<REF<TS<float>>>{ordinary});
        CHECK_THROWS_AS((wp<DEDUPLICATED_REFERENCE>{ordinary}), WiringError);

        const auto annotation = ordinary.as<REF<TS<float>>>();
        CHECK(&annotation.type() == &ordinary.type());

        const auto deduplicated = static_cast<WiringPoint>(deduplicated_source());
        CHECK_THROWS_AS((wp<FINAL_REFERENCE>{deduplicated}), WiringError);

        const auto final = static_cast<WiringPoint>(final_source());
        CHECK_NOTHROW((wp<DEDUPLICATED_REFERENCE>{final}));
        CHECK_NOTHROW((wp<FINAL_REFERENCE>{final}));
        CHECK_NOTHROW((wp<VARIABLE_REFERENCE>{final}));

        auto optional = wp<REF<TS<float>>, bg::optional>{ordinary}.as<TS<float>>();
        STATIC_CHECK(std::same_as<decltype(optional), wp<TS<float>, bg::optional>>);
        CHECK(&optional.type() == &ordinary.type());
    });
}

TEST_CASE("wiring point conversion materializes values as const references",
          "[graph_wiring][feature:wiring][feature:reference][guarantee:const]") {
    const auto source = node([](Out<TS<float>>) {});

    const auto sink = node([](In<REF<TS<float>, TimeSeriesGuarantee::const_>>) {});
    auto wiring = GraphWiring{};
    auto value = WiringPoint{};
    {
        const auto context = WiringContext{wiring};
        value = source();
    }

    const auto point = value.as<REF<TS<float>>>();
    const auto repeated = value.as<REF<TS<float>>>();

    CHECK(&point.type() == &REF<TS<float>, TimeSeriesGuarantee::const_>::type());
    CHECK(static_cast<WiringPoint>(point).has_default_materialisation());
    CHECK(point.materialisation_options().empty());
    CHECK(static_cast<WiringPoint>(point).record() == static_cast<WiringPoint>(repeated).record());
    CHECK(wiring.nodes().empty());

    {
        const auto context = WiringContext{wiring};
        (void)sink(point);
    }
    CHECK(wiring.nodes().empty());

    wiring.finalize();
    CHECK(wiring.nodes().size() == 3);
}

TEST_CASE("wiring point conversion preserves an existing reference",
          "[graph_wiring][feature:wiring][feature:reference][binding:reference-to-reference]") {
    const auto source = node([](Out<REF<TS<float>>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto reference = static_cast<WiringPoint>(source());
    const auto converted = reference.as<REF<TS<float>>>();

    CHECK(static_cast<WiringPoint>(converted).record() == reference.record());
    CHECK(&converted.type() == &reference.type());
}

TEST_CASE("wiring point conversion rejects incompatible and borrowed values",
          "[graph_wiring][feature:wiring][feature:reference][feature:borrowed-input]") {
    const auto source = node([](Out<TS<float>>) {});

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    CHECK_THROWS_AS(source().as<REF<TS<double>>>(), WiringError);
    CHECK_THROWS_AS(wiring.borrowed_input(TS<float>::type()).as<REF<TS<float>>>(), WiringError);
}

TEST_CASE("wiring reuses a node called with the same arguments", "[graph_wiring][feature:wiring]") {
    const auto source = node([](Out<TS<float>>) {});

    const auto scale = node([](In<TS<float>>, float, Out<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto input = source();
    (void)scale(input, 2.0F);
    (void)scale(input, 2.0F);

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 2);
}

TEST_CASE("wiring keeps calls with different scalar arguments separate", "[graph_wiring][feature:wiring]") {
    const auto source = node([](Out<TS<float>>) {});

    const auto scale = node([](In<TS<float>>, float, Out<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto input = source();
    (void)scale(input, 2.0F);
    (void)scale(input, 3.0F);

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 3);
}

TEST_CASE("wiring resolves a runtime-selected scalar argument", "[graph_wiring][feature:wiring][scalar:generic]") {
    const auto sink = node([](Scalar<SCALAR>) {});
    auto value = ScalarValue<>{std::string{"runtime scalar"}};
    const auto arguments = std::array{WiringArgument{std::move(value)}};
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    (void)static_cast<const WiringTemplate &>(sink)(std::span<const WiringArgument>{arguments});

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    CHECK(wiring.nodes()[0].signature().parameters[0].type == &string_type());
    REQUIRE(wiring.nodes()[0].scalar_values().size() == 1);
    CHECK(&wiring.nodes()[0].scalar_values()[0].runtime_type().type() == &string_type());
}

TEST_CASE("a graph forwards an extension scalar through a generic parameter",
          "[graph_wiring][feature:wiring][scalar:generic]") {
    const auto sink = node([](Scalar<SCALAR>, Out<TS<float>>) {});

    const auto forward = graph([&](Scalar<SCALAR> value) { return sink(value); });
    auto value = ScalarValue<test_scalar_provider::value>{test_scalar_provider::value{42}};
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    (void)forward(value);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    CHECK(wiring.nodes()[0].signature().parameters[0].type ==
          &cpp_scalar_type<test_scalar_provider::value>());
    REQUIRE(wiring.nodes()[0].scalar_values().size() == 1);
    CHECK(&wiring.nodes()[0].scalar_values()[0].runtime_type().type() ==
          &cpp_scalar_type<test_scalar_provider::value>());
    const auto &wired_scalar = wiring.nodes()[0].scalar_values()[0];
    CHECK(Scalar<>::from(wired_scalar.runtime_type(), wired_scalar.data()).equals(value));
}

TEST_CASE("a graph resolves a scalar variable nested in a list view",
          "[feature:wiring][scalar:list][declaration:partially-generic]") {
    const auto sink = node([](Scalar<List<SCALAR>>) {});

    const auto forward = graph([&](Scalar<List<SCALAR>> value) { sink(value); });
    const auto value = ScalarValue<List<i64>>{std::array<i64, 2>{4, 2}};
    const auto list = Scalar<List<SCALAR>>::from(value);
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    forward(list);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    const auto &wired_scalar = wiring.nodes()[0].scalar_values()[0];
    CHECK(&wired_scalar.runtime_type().type() == &value.runtime_type().type());
    CHECK(Scalar<>::from(wired_scalar.runtime_type(), wired_scalar.data()).equals(value));
}

TEST_CASE("an aggregate graph forwards an extension scalar through a generic field",
          "[graph_wiring][feature:wiring][scalar:generic][definition:aggregate]") {
    auto value = ScalarValue<test_scalar_provider::value>{test_scalar_provider::value{42}};
    const auto scalar = Scalar<SCALAR>::from(value);
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    aggregate_scalar_forward(scalar);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    CHECK(wiring.nodes()[0].signature().parameters[0].type ==
          &cpp_scalar_type<test_scalar_provider::value>());
}

TEST_CASE("const operator accepts a runtime-selected scalar argument",
          "[graph_wiring][feature:constant][scalar:generic]") {
    auto value = ScalarValue<>{i64{41}};
    const auto arguments = std::array{WiringArgument{std::move(value)}};
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto point =
        static_cast<const WiringTemplate &>(operators::const_)(std::span<const WiringArgument>{arguments});

    REQUIRE(point);
    CHECK(point->type().accepts(TS<i64, TimeSeriesGuarantee::const_>::type()));
}

TEST_CASE("constant and scalar arguments wire through the erased template interface",
          "[graph_wiring][feature:constant][scalar:generic]") {
    auto value = ScalarValue<>{i64{41}};
    const auto constant_arguments = std::array{WiringArgument{std::move(value)}};
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto point = static_cast<const WiringTemplate &>(operators::const_)(
        std::span<const WiringArgument>{constant_arguments});

    const auto sink = node([](In<TS<i64>>, Scalar<i64>, Out<TS<i64>>) {});

    REQUIRE(point);
    const auto inputs = std::array{WiringArgument{*point}, WiringArgument{i64{1}}};
    CHECK_NOTHROW(static_cast<const WiringTemplate &>(sink)(std::span<const WiringArgument>{inputs}));
}

TEST_CASE("wiring keeps calls with different input sources separate", "[graph_wiring][feature:wiring]") {
    const auto first_source = node([](Out<TS<float>>) {});

    const auto second_source = node([](Out<TS<float>>) {});

    const auto copy = node([](In<TS<float>>, Out<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    (void)copy(first_source());
    (void)copy(second_source());

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 4);
}

TEST_CASE("wiring reuses a node called with the same absent scalar", "[graph_wiring][feature:wiring]") {
    const auto source = node([](std::optional<int>, Out<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    (void)source(std::optional<int>{});
    (void)source(std::optional<int>{});

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 1);
}

namespace {

struct unhashable_wiring_value {
    int value;
};

struct unregistered_opaque_wiring_value {
    int value;
};

struct unregistered_opaque_wiring_node {
    Scalar<Opaque<unregistered_opaque_wiring_value>> value;
    Out<TS<float>> output;

    void evaluate() const {}
};

struct unregistered_opaque_pointer_wiring_node {
    Scalar<Opaque<unregistered_opaque_wiring_value *>> value;
    Out<TS<float>> output;

    void evaluate() const {}
};

} // namespace

TEST_CASE("wiring does not reuse calls with an unhashable scalar", "[graph_wiring][feature:wiring][scalar:opaque]") {
    using Argument = Opaque<unhashable_wiring_value>;

    const auto source = node([](Scalar<Argument>, Out<TS<float>>) {});
    const auto value = ScalarValue<Argument>{unhashable_wiring_value{1}};
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    (void)source(value);
    (void)source(value);

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 2);
}

TEST_CASE("unregistered C++ values bind to exact opaque scalar parameters",
          "[graph_wiring][feature:wiring][scalar:opaque][lifetime:retained]") {
    using namespace bg::literals;

    const auto source = node<unregistered_opaque_wiring_node>();
    const auto pointer_source = node<unregistered_opaque_pointer_wiring_node>();
    auto wiring = GraphWiring{};
    auto pointed_to = unregistered_opaque_wiring_value{31};

    {
        const auto context = WiringContext{wiring};
        auto lvalue = unregistered_opaque_wiring_value{11};
        (void)source(lvalue);
        lvalue.value = 12;
        (void)source("value"_n = unregistered_opaque_wiring_value{21});
        (void)pointer_source("value"_n = &pointed_to);
    }

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 3);
    REQUIRE(wiring.nodes()[0].scalar_values().size() == 1);
    REQUIRE(wiring.nodes()[1].scalar_values().size() == 1);
    REQUIRE(wiring.nodes()[2].scalar_values().size() == 1);
    CHECK(&wiring.nodes()[0].scalar_values()[0].runtime_type().type() ==
          &cpp_scalar_type<Opaque<unregistered_opaque_wiring_value>>());
    CHECK(static_cast<const unregistered_opaque_wiring_value *>(wiring.nodes()[0].scalar_values()[0].data())->value ==
          11);
    CHECK(static_cast<const unregistered_opaque_wiring_value *>(wiring.nodes()[1].scalar_values()[0].data())->value ==
          21);
    CHECK(*static_cast<unregistered_opaque_wiring_value *const *>(wiring.nodes()[2].scalar_values()[0].data()) ==
          &pointed_to);
}

TEST_CASE("unregistered C++ values require an exact fixed opaque parameter",
          "[graph_wiring][feature:wiring][scalar:opaque]") {
    const auto mismatch = node([](Scalar<Opaque<unhashable_wiring_value>>, Out<TS<float>>) {});
    const auto generic = node([](Scalar<SCALAR>, Out<TS<float>>) {});
    const auto value = unregistered_opaque_wiring_value{1};

    CHECK_THROWS_WITH(testing::test_wiring([&] { (void)mismatch(value); }),
                      Catch::Matchers::ContainsSubstring("exact matching fixed parameter"));
    CHECK_THROWS_WITH(testing::test_wiring([&] { (void)generic(value); }),
                      Catch::Matchers::ContainsSubstring("exact matching fixed parameter"));
}

TEST_CASE("opaque source candidates leave ordinary scalar inference unchanged",
          "[graph_wiring][feature:wiring][scalar:native]") {
    const auto sink = node([](Scalar<int>, Out<TS<float>>) {});
    auto wiring = GraphWiring{};

    {
        const auto context = WiringContext{wiring};
        (void)sink(42);
    }

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    CHECK(wiring.nodes()[0].signature().parameters[0].type == &cpp_scalar_type<int>());
}

TEST_CASE("wiring does not reuse outputless node calls", "[graph_wiring][feature:wiring]") {
    const auto source = node([](Out<TS<float>>) {});

    const auto sink = node([](In<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto input = source();
    sink(input);
    sink(input);

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 3);
}

TEST_CASE("a node can disable wiring deduplication", "[graph_wiring][feature:wiring]") {
    const auto source = node(eval = [](Out<TS<float>>) {}, deduplicate_wiring = false);
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    (void)source();
    (void)source();

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 2);
}

namespace {

struct non_deduplicated_source {
    static constexpr bool deduplicate_wiring = false;

    Out<TS<float>> output;

    void evaluate() const {}
};

} // namespace

TEST_CASE("an aggregate node can disable wiring deduplication", "[graph_wiring][feature:wiring]") {
    const auto source = node<non_deduplicated_source>();
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    (void)source();
    (void)source();

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 2);
}

TEST_CASE("a node call requires a wiring context", "[graph_wiring][feature:wiring]") {
    const auto source = node<source_definition>();
    CHECK_THROWS_AS(source(), WiringError);
}

namespace {

using BOUND_TIME_SERIES = TimeSeriesVariable<"BOUND_TIME_SERIES">;

struct bound_reference_source {
    Out<REF<BOUND_TIME_SERIES>> output;

    void evaluate() const noexcept {}
};

} // namespace

TEST_CASE("a node call can seed type resolution", "[graph_wiring][type_system][feature:wiring]") {
    const auto source = node<bound_reference_source>();
    const auto &target = TS<i64>::type();
    const auto &expected = REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type();
    auto type_map = TypeMap{};
    type_map.set(details::TimeSeriesVariableInstance<BOUND_TIME_SERIES>::get(), &target);
    auto wiring = GraphWiring{};

    auto context = WiringContext{wiring};
    const auto output = static_cast<WiringPoint>(source(type_map));

    CHECK(&output.type() == &expected);
}

namespace {

class scale_template final : public NodeTemplate {
  public:
    [[nodiscard]] const Signature &default_signature() const override {
        static const auto input_type = util::static_storage<GenericTimeSeriesValueType>{
            float_type(), TimeSeriesGuarantee::ordinary};
        [[maybe_unused]] static const auto &input_type_registration = TypeRegistry::register_static(input_type.get());
        static const Signature value{
            util::symbol{"scale"},
            {{util::symbol{"input"}, &input_type.get()}, {util::symbol{"factor"}, &float_type()}},
            TS<float>::type()};

        return value;
    }

    [[nodiscard]] EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

  private:
    static bool evaluate(const CompiledNode &, void *, EvaluationContext *) { return true; }
};

class union_input_node_template final : public NodeTemplate {
  public:
    [[nodiscard]] const Signature &default_signature() const override {
        using FORM = Union<"NODE_MULTIPLE_SERIES_FORMS", i64,
                               TS<i64>, TS<float>>;
        static const auto value = Signature{
            util::symbol{"union_input_node"}, {{util::symbol{"input"}, &FORM::type()}}};
        return value;
    }

    [[nodiscard]] EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        return nullptr;
    }
};

class series_only_union_input_node_template final : public NodeTemplate {
  public:
    [[nodiscard]] const Signature &default_signature() const override {
        using FORM = Union<"NODE_SERIES_ONLY_FORM", TS<float>>;
        static const auto value = Signature{
            util::symbol{"series_only_union_input_node"}, {{util::symbol{"input"}, &FORM::type()}}};
        return value;
    }

    [[nodiscard]] EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        return nullptr;
    }
};

class scalar_or_time_series_node_template final : public NodeTemplate {
  public:
    [[nodiscard]] const Signature &default_signature() const override {
        using FORM = Union<"NODE_SCALAR_OR_SERIES", float, TS<float>>;
        static const auto value = Signature{
            util::symbol{"scalar_or_time_series_node"}, {{util::symbol{"input"}, &FORM::type()}}};
        return value;
    }

    [[nodiscard]] EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

  private:
    static bool evaluate(const CompiledNode &, void *, EvaluationContext *) { return true; }
};

} // namespace

TEST_CASE("runtime node templates reject a Union with multiple time-series forms",
          "[feature:wiring][feature:type-system]") {
    const auto source = node([](Out<TS<float>>) {});
    const auto sink = union_input_node_template{};
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto input = source();

    CHECK_THROWS_AS(sink(input), WiringError);
}

TEST_CASE("runtime node templates reject a time-series-only Union",
          "[feature:wiring][feature:type-system]") {
    const auto source = node([](Out<TS<float>>) {});
    const auto sink = series_only_union_input_node_template{};
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto input = source();

    CHECK_THROWS_AS(sink(input), WiringError);
}

TEST_CASE("runtime node templates resolve scalar and selected time-series Union forms",
          "[feature:wiring][feature:type-system][feature:wiring-materialisation]") {
    SECTION("the scalar alternative resolves to a scalar node input") {
        const auto sink = scalar_or_time_series_node_template{};
        auto wiring = GraphWiring{};
        const auto context = WiringContext{wiring};
        (void)sink(4.0F);

        wiring.finalize();

        const auto node = std::ranges::find_if(wiring.nodes(), [&](const auto &candidate) {
            return &candidate.node_template() == &sink;
        });
        REQUIRE(node != wiring.nodes().end());
        CHECK(node->signature().parameters[0].type == &float_type());
        CHECK(node->inputs().empty());
        REQUIRE(node->scalar_values().size() == 1);
    }

    SECTION("a deferred source binds its selected time-series form") {
        const auto integer_source = node([](Out<TS<i64>>) {});

        const auto float_source = node([](Out<TS<float>>) {});
        const auto sink = scalar_or_time_series_node_template{};
        auto wiring = GraphWiring{};
        const auto context = WiringContext{wiring};
        const auto semantic_source = integer_source();
        const auto selected_source = float_source();
        auto materialisation_calls = 0;
        auto deferred = wiring.create_wp<TS<i64>>(semantic_source);
        deferred.materialisation_option<TS<float>>(
            [&](wp<TS<i64>>) {
                ++materialisation_calls;
                return selected_source;
            },
            0.0);

        sink(deferred);
        CHECK(wiring.materialisation_required(deferred, TS<float>::type()));
        wiring.finalize();

        const auto node = std::ranges::find_if(wiring.nodes(), [&](const auto &candidate) {
            return &candidate.node_template() == &sink;
        });
        REQUIRE(node != wiring.nodes().end());
        CHECK(node->signature().parameters[0].type == &TS<float>::type());
        REQUIRE(node->inputs().size() == 1);
        CHECK(wiring.input_consumes(node->inputs()[0], selected_source));
        CHECK(materialisation_calls == 1);
    }
}

TEST_CASE("invoke_node captures the intended template while importing context",
          "[graph_wiring][feature:wiring]") {
    using namespace bg::literals;

    const auto target = node([](In<TS<float>, "value"_n, context>, Out<TS<bool>>) {});

    const auto provider = node([](Out<TS<float>>) {});
    auto outer = GraphWiring{};
    auto inner = GraphWiring{outer};
    auto call = GraphWiring::NodeCallResult{};
    auto outer_provider = WiringPoint{};
    {
        const auto outer_context = WiringContext{outer};
        outer_provider = provider();
        const auto binding = with_context("value"_n = outer_provider);
        {
            const auto inner_context = WiringContext{inner};
            call = inner.invoke_node(target, std::span<const WiringArgument>{});
        }
    }

    CHECK(call.identity.value == 0);
    REQUIRE(call.output);
    CHECK(inner.node_call_identity(*call.output)->value == 0);
    inner.finalize();
    REQUIRE(inner.nodes().size() == 1);
    REQUIRE(inner.nodes()[0].inputs().size() == 1);
    CHECK(inner.nodes()[0].inputs()[0].is_inherited());
    outer.finalize();
    REQUIRE(outer.nodes().size() == 2);
    REQUIRE(outer.nodes()[1].inputs().size() == 1);
    CHECK(outer.nodes()[1].inputs()[0].index() == outer.output_index(outer_provider));
}

TEST_CASE("wired nodes retain scalar arguments", "[graph_wiring][feature:wiring]") {
    const auto source = node<source_definition>();
    const auto scale = scale_template{};
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        const WiringPoint input = source();
        (void)scale(input, 2.0F);
    }

    (void)wiring.complete_pending_wiring();
    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[1].inputs().size() == 1);
    CHECK(nodes[1].inputs()[0].index() == 0);
    REQUIRE(nodes[1].scalar_values().size() == 1);
    const WiredScalar &factor = nodes[1].scalar_values()[0];
    CHECK(*static_cast<const float *>(factor.data()) == 2.0F);
    CHECK(factor.runtime_type().size() == sizeof(float));
    CHECK(nodes[1].signature().parameters[1].type == &float_type());
}

namespace {

struct optional_scalar_definition {
    std::optional<int> value;

    void evaluate() const {}
};

class optional_scalar_type_node_template final : public NodeTemplate {
  public:
    explicit optional_scalar_type_node_template(const Type &type)
        : signature_{util::symbol{"optional_scalar_type"},
                     {{util::symbol{"value"}, &type, active, required, true}},
                     TS<float>::type()} {}

    const Signature &default_signature() const final { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept final { return nullptr; }

  private:
    Signature signature_;
};

} // namespace

TEST_CASE("optional scalar parameters can record absence", "[graph_wiring][feature:wiring]") {
    const auto optional_scalar = node<optional_scalar_definition>();
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        optional_scalar(std::optional<int>{});
        optional_scalar();
    }

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 2);
    for (const auto &node : wiring.nodes()) {
        REQUIRE(node.scalar_values().size() == 1);
        CHECK_FALSE(node.scalar_values()[0].has_value());
    }
}

TEST_CASE("omitted optional scalar parameters require one concrete scalar type",
          "[graph_wiring][feature:wiring][scalar:generic]") {
    using AMBIGUOUS = ScalarVariable<"AMBIGUOUS", float, double>;

    const auto generic = optional_scalar_type_node_template{SCALAR::type()};
    const auto ambiguous = optional_scalar_type_node_template{AMBIGUOUS::type()};
    testing::test_wiring([&] {
        CHECK_THROWS_WITH(generic(), Catch::Matchers::ContainsSubstring("unique concrete scalar type"));
        CHECK_THROWS_WITH(ambiguous(), Catch::Matchers::ContainsSubstring("unique concrete scalar type"));
    });
}

namespace {

struct required_scalar_definition {
    int value;

    void evaluate() const {}
};

} // namespace

TEST_CASE("required scalar parameters reject absence", "[graph_wiring][feature:wiring]") {
    const auto required_scalar = node<required_scalar_definition>();
    GraphWiring wiring;
    const WiringContext context{wiring};

    try {
        (void)required_scalar(std::optional<int>{});
        FAIL("expected a required scalar to reject absence");
    } catch (const TypeResolutionError &error) {
        CHECK(error.code() == TypeResolutionErrorCode::mismatch);
        CHECK(error.subject().text() == "value");
        CHECK(std::string{error.what()}.contains("required scalar parameter has no value"));
    }
    CHECK_THROWS_AS(required_scalar(), WiringError);
    CHECK_NOTHROW(wiring.finalize());
    CHECK(wiring.nodes().empty());
}

TEST_CASE("node output runtime selector runs at the call and its result is reused when binding",
          "[graph_wiring][feature:wiring][feature:node-api]") {
    const auto &matching_runtime_type = cpp_time_series_runtime_type<TS<float>>();
    const auto &mismatched_runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    auto mismatch_selector_calls = 0;
    auto matching_selector_calls = 0;
    const auto mismatch = OutputRuntimeSelectorNode{mismatched_runtime_type, mismatch_selector_calls};
    const auto matching = OutputRuntimeSelectorNode{matching_runtime_type, matching_selector_calls};
    auto wiring = GraphWiring{};
    {
        const WiringContext context{wiring};
        CHECK_THROWS_WITH(mismatch(), Catch::Matchers::ContainsSubstring(
                                          "a selected output runtime type has a different semantic type"));
        CHECK(mismatch_selector_calls == 1);

        const auto output = matching();
        REQUIRE(output);
        CHECK(matching_selector_calls == 1);
    }

    CHECK_NOTHROW(wiring.finalize());
    CHECK(mismatch_selector_calls == 1);
    CHECK(matching_selector_calls == 1);
    REQUIRE(wiring.nodes().size() == 1);
    REQUIRE(wiring.outputs().size() == 1);
    CHECK(wiring.outputs()[0].runtime_type == &matching_runtime_type);
}

TEST_CASE("an output runtime callback failure retains the node's wiring context",
          "[graph_wiring][feature:wiring][feature:node-api]") {
    const auto node = ThrowingOutputRuntimeSelectorNode{};
    auto wiring = GraphWiring{};
    const WiringContext context{wiring};

    try {
        (void)node();
        FAIL("expected output runtime selection to fail");
    } catch (const WiringError &error) {
        CHECK(std::string{error.what()}.contains("output descriptor selection failed"));
        REQUIRE(error.contexts().size() == 1);
        CHECK(error.contexts().front().stage == WiringFailureStage::callback);
        CHECK(error.contexts().front().operation.text() == "throwing_output_runtime_selector");
        CHECK_FALSE(error.contexts().front().path.empty());
    }

    CHECK(wiring.nodes().empty());
    CHECK(wiring.outputs().empty());
}

TEST_CASE("explicit scalar values preserve their declared graph type",
          "[graph_wiring][feature:wiring][scalar:integer]") {
    const auto consume = node([](Scalar<int>) {});
    const auto value = ScalarValue<int>{1};
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    consume(value);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    CHECK(wiring.nodes()[0].signature().parameters[0].type == &int_type());
}

namespace {

struct scheduled_times_definition {
    Scalar<List<EvaluationTime>> times;

    void evaluate() const {}
};

} // namespace

TEST_CASE("evaluation time lists pass through normal wiring", "[graph_wiring][feature:wiring]") {
    const auto scheduled_times = node<scheduled_times_definition>();
    const auto times =
        std::vector{MIN_DT + std::chrono::microseconds{2}, MIN_DT + std::chrono::microseconds{5}};
    const auto value = ScalarValue<List<EvaluationTime>>{times};
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        scheduled_times(value);
    }

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    REQUIRE(wiring.nodes()[0].scalar_values().size() == 1);
    CHECK(&wiring.nodes()[0].scalar_values()[0].runtime_type().type() ==
          &cpp_scalar_type<List<EvaluationTime>>());
}

TEST_CASE("nested wiring contexts keep graph connections explicit", "[graph_wiring][feature:wiring]") {
    const auto source = node<source_definition>();

    const auto copy = node<copy_definition>();
    GraphWiring outer;
    GraphWiring inner;

    {
        WiringContext outer_context{outer};
        const WiringPoint outer_input = source();

        {
            WiringContext inner_context{inner};

            CHECK_THROWS_AS(copy(outer_input), WiringError);

            const WiringPoint inner_input = source();
            (void)copy(inner_input);
        }

        (void)copy(outer_input);
    }

    (void)inner.complete_pending_wiring();
    (void)outer.complete_pending_wiring();
    CHECK(outer.nodes().size() == 2);
    CHECK(inner.nodes().size() == 2);
}

TEST_CASE("wiring contexts are local to one thread", "[graph_wiring][feature:wiring]") {
    GraphWiring wiring;
    WiringContext context{wiring};
    bool child_has_context = true;

    std::thread child{[&child_has_context] {
        try {
            (void)WiringContext::current();
        } catch (const WiringError &) {
            child_has_context = false;
        }
    }};
    child.join();

    CHECK_FALSE(child_has_context);
    CHECK(&WiringContext::current().wiring() == &wiring);
}
