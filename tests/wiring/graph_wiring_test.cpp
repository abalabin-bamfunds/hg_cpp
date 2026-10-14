#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/context.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_resolution_error.hpp>

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

namespace {

struct source_definition {
    bg::Out<bg::TS<float>> output;

    void evaluate() const {}
};

struct copy_definition {
    bg::In<bg::TS<float>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(*input); }
};

class OutputRuntimeSelectorNode final : public bg::NodeTemplate {
  public:
    OutputRuntimeSelectorNode(const bg::TimeSeriesRuntimeType &selected_runtime_type, int &selector_calls)
        : signature_{bg::util::symbol{"output_runtime_selector"}, {}, bg::TS<float>::type()},
          selected_runtime_type_{&selected_runtime_type}, selector_calls_{&selector_calls} {}

    const bg::Signature &signature() const final { return signature_; }
    bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept final { return nullptr; }
    bool uses_output_runtime_type() const noexcept final { return true; }

    const bg::TimeSeriesRuntimeType *output_runtime_type(const bg::WiringArguments &) const final {
        ++*selector_calls_;
        return selected_runtime_type_;
    }

  private:
    bg::Signature signature_;
    const bg::TimeSeriesRuntimeType *selected_runtime_type_;
    int *selector_calls_;
};

class ThrowingOutputRuntimeSelectorNode final : public bg::NodeTemplate {
  public:
    ThrowingOutputRuntimeSelectorNode()
        : signature_{bg::util::symbol{"throwing_output_runtime_selector"}, {}, bg::TS<float>::type()} {}

    const bg::Signature &signature() const final { return signature_; }
    bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept final { return nullptr; }
    bool uses_output_runtime_type() const noexcept final { return true; }

    const bg::TimeSeriesRuntimeType *output_runtime_type(const bg::WiringArguments &) const final {
        throw std::runtime_error{"output descriptor selection failed"};
    }

  private:
    bg::Signature signature_;
};

static_assert(sizeof(bg::WiringPoint) == sizeof(void *));
static_assert(std::is_trivially_copyable_v<bg::WiringPoint>);

} // namespace

TEST_CASE("node calls add wired nodes to the active graph", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node<source_definition>();
    const auto copy = bg::node<copy_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        const bg::WiringPoint output = copy(input);

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
    const auto source = bg::node<source_definition>();
    const auto consume = bg::node([](bg::In<bg::TS<float>>) {});
    bg::GraphWiring wiring;
    auto source_point = bg::WiringPoint{};
    auto consume_call = bg::GraphWiring::NodeCallResult{};

    {
        bg::WiringContext context{wiring};
        source_point = source();
        const auto arguments = std::array{bg::WiringArgument{source_point}};
        consume_call = wiring.invoke_node(consume, arguments);
    }

    CHECK(wiring.nodes().empty());
    CHECK_FALSE(consume_call.output);
    const auto source_identity = wiring.node_call_identity(source_point);
    REQUIRE(source_identity);
    CHECK(source_identity->value == 0);
    CHECK(consume_call.identity.value == 1);
    CHECK_THROWS_AS(wiring.output_node_index(source_point), bg::WiringError);

    wiring.finalize();

    CHECK(wiring.node_index(*source_identity) == 0);
    CHECK(wiring.node_index(consume_call.identity) == 1);
    CHECK(wiring.output_node_index(source_point) == 0);
}

TEST_CASE("wiring point re-annotation checks type compatibility", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto typed = source();
    const auto point = static_cast<bg::WiringPoint>(typed);

    STATIC_CHECK(std::same_as<decltype(point.as<bg::TS<float>>()), bg::wp<bg::TS<float>>>);
    STATIC_CHECK(std::same_as<decltype(typed.as<bg::TS<float>>()), bg::wp<bg::TS<float>>>);
    CHECK_NOTHROW(point.as<bg::TS<float>>());
    CHECK_NOTHROW(typed.as<bg::TS<float>>());
    CHECK_NOTHROW(point.as<bg::REF<bg::TS<float>>>());
    CHECK_THROWS_AS(point.as<bg::TS<double>>(), bg::WiringError);
    CHECK_THROWS_AS(bg::wp<bg::TS<double>>{typed}, bg::WiringError);
}

TEST_CASE("wiring point reference annotations check guarantee compatibility",
          "[graph_wiring][feature:wiring][feature:reference][guarantee:ordinary]"
          "[guarantee:deduplicated][guarantee:final]") {
    using DEDUPLICATED_REFERENCE = bg::REF<bg::TS<float>, bg::TimeSeriesGuarantee::deduplicated>;
    using FINAL_REFERENCE = bg::REF<bg::TS<float>, bg::TimeSeriesGuarantee::final>;
    using VARIABLE_REFERENCE =
        bg::REF<bg::TS<float, bg::guarantee_variable<"G1">>, bg::guarantee_variable<"G2">>;

    const auto ordinary_source = bg::node(
        [](bg::Out<bg::REF<bg::TS<float>, bg::TimeSeriesGuarantee::ordinary>>) {});
    const auto deduplicated_source = bg::node(
        [](bg::Out<bg::REF<bg::TS<float>, bg::TimeSeriesGuarantee::deduplicated>>) {});
    const auto final_source = bg::node([](bg::Out<bg::REF<bg::TS<float>, bg::TimeSeriesGuarantee::final>>) {});

    bg::testing::test_wiring([&] {
        const auto ordinary = static_cast<bg::WiringPoint>(ordinary_source());
        CHECK_NOTHROW(bg::wp<bg::REF<bg::TS<float>>>{ordinary});
        CHECK_THROWS_AS((bg::wp<DEDUPLICATED_REFERENCE>{ordinary}), bg::WiringError);

        const auto annotation = ordinary.as<bg::REF<bg::TS<float>>>();
        CHECK(&annotation.type() == &ordinary.type());

        const auto deduplicated = static_cast<bg::WiringPoint>(deduplicated_source());
        CHECK_THROWS_AS((bg::wp<FINAL_REFERENCE>{deduplicated}), bg::WiringError);

        const auto final = static_cast<bg::WiringPoint>(final_source());
        CHECK_NOTHROW((bg::wp<DEDUPLICATED_REFERENCE>{final}));
        CHECK_NOTHROW((bg::wp<FINAL_REFERENCE>{final}));
        CHECK_NOTHROW((bg::wp<VARIABLE_REFERENCE>{final}));

        auto optional = bg::wp<bg::REF<bg::TS<float>>, bg::optional>{ordinary}.as<bg::TS<float>>();
        STATIC_CHECK(std::same_as<decltype(optional), bg::wp<bg::TS<float>, bg::optional>>);
        CHECK(&optional.type() == &ordinary.type());
    });
}

TEST_CASE("wiring reuses a node called with the same arguments", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto scale = bg::node([](bg::In<bg::TS<float>>, float, bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    (void)scale(input, 2.0F);
    (void)scale(input, 2.0F);

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 2);
}

TEST_CASE("wiring keeps calls with different scalar arguments separate", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto scale = bg::node([](bg::In<bg::TS<float>>, float, bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    (void)scale(input, 2.0F);
    (void)scale(input, 3.0F);

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 3);
}

TEST_CASE("wiring resolves a runtime-selected scalar argument", "[graph_wiring][feature:wiring][scalar:generic]") {
    const auto sink = bg::node([](bg::Scalar<bg::SCALAR>) {});
    auto value = bg::ScalarValue<>{std::string{"runtime scalar"}};
    const auto arguments = std::array{bg::WiringArgument{std::move(value)}};
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    (void)static_cast<const bg::WiringTemplate &>(sink)(std::span<const bg::WiringArgument>{arguments});

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    CHECK(wiring.nodes()[0].signature().parameters[0].type == &bg::string_type());
    REQUIRE(wiring.nodes()[0].scalar_values().size() == 1);
    CHECK(&wiring.nodes()[0].scalar_values()[0].runtime_type().type() == &bg::string_type());
}

TEST_CASE("const operator accepts a runtime-selected scalar argument",
          "[graph_wiring][feature:constant][scalar:generic]") {
    auto value = bg::ScalarValue<>{bg::i64{41}};
    const auto arguments = std::array{bg::WiringArgument{std::move(value)}};
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    const auto point =
        static_cast<const bg::WiringTemplate &>(bg::operators::const_)(std::span<const bg::WiringArgument>{arguments});

    REQUIRE(point);
    CHECK(point->type().accepts(bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type()));
}

TEST_CASE("constant and scalar arguments wire through the erased template interface",
          "[graph_wiring][feature:constant][scalar:generic]") {
    auto value = bg::ScalarValue<>{bg::i64{41}};
    const auto constant_arguments = std::array{bg::WiringArgument{std::move(value)}};
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto point = static_cast<const bg::WiringTemplate &>(bg::operators::const_)(
        std::span<const bg::WiringArgument>{constant_arguments});
    const auto sink = bg::node([](bg::In<bg::TS<bg::i64>>, bg::Scalar<bg::i64>, bg::Out<bg::TS<bg::i64>>) {});

    REQUIRE(point);
    const auto inputs = std::array{bg::WiringArgument{*point}, bg::WiringArgument{bg::i64{1}}};
    CHECK_NOTHROW(static_cast<const bg::WiringTemplate &>(sink)(std::span<const bg::WiringArgument>{inputs}));
}

TEST_CASE("wiring keeps calls with different input sources separate", "[graph_wiring][feature:wiring]") {
    const auto first_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto second_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto copy = bg::node([](bg::In<bg::TS<float>>, bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    (void)copy(first_source());
    (void)copy(second_source());

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 4);
}

TEST_CASE("wiring reuses a node called with the same absent scalar", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node([](std::optional<int>, bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    (void)source(std::optional<int>{});
    (void)source(std::optional<int>{});

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 1);
}

namespace {

struct unhashable_wiring_value {
    int value;
};

} // namespace

TEST_CASE("wiring does not reuse calls with an unhashable scalar", "[graph_wiring][feature:wiring][scalar:opaque]") {
    using Argument = bg::Opaque<unhashable_wiring_value>;

    const auto source = bg::node([](bg::Scalar<Argument>, bg::Out<bg::TS<float>>) {});
    const auto value = bg::ScalarValue<Argument>{unhashable_wiring_value{1}};
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    (void)source(value);
    (void)source(value);

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 2);
}

TEST_CASE("wiring does not reuse outputless node calls", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    sink(input);
    sink(input);

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 3);
}

TEST_CASE("a node can disable wiring deduplication", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node(bg::eval = [](bg::Out<bg::TS<float>>) {}, bg::deduplicate_wiring = false);
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    (void)source();
    (void)source();

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 2);
}

namespace {

struct non_deduplicated_source {
    static constexpr bool deduplicate_wiring = false;

    bg::Out<bg::TS<float>> output;

    void evaluate() const {}
};

} // namespace

TEST_CASE("an aggregate node can disable wiring deduplication", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node<non_deduplicated_source>();
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    (void)source();
    (void)source();

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 2);
}

TEST_CASE("a node call requires a wiring context", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node<source_definition>();
    CHECK_THROWS_AS(source(), bg::WiringError);
}

namespace {

using BOUND_TIME_SERIES = bg::TimeSeriesVariable<"BOUND_TIME_SERIES">;

struct bound_reference_source {
    bg::Out<bg::REF<BOUND_TIME_SERIES>> output;

    void evaluate() const noexcept {}
};

} // namespace

TEST_CASE("a node call can seed type resolution", "[graph_wiring][type_system][feature:wiring]") {
    const auto source = bg::node<bound_reference_source>();
    const auto &target = bg::TS<std::int64_t>::type();
    const auto &expected = bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::deduplicated>::type();
    auto type_map = bg::TypeMap{};
    type_map.set(bg::details::TimeSeriesVariableInstance<BOUND_TIME_SERIES>::get(), &target);
    auto wiring = bg::GraphWiring{};

    auto context = bg::WiringContext{wiring};
    const auto output = static_cast<bg::WiringPoint>(source(type_map));

    CHECK(&output.type() == &expected);
}

namespace {

class scale_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        static const bg::GenericTimeSeriesValueType input_type{bg::float_type(), bg::TimeSeriesGuarantee::ordinary};
        [[maybe_unused]] static const auto &input_type_registration = bg::TypeRegistry::register_static(input_type);
        static const bg::Signature value{
            bg::util::symbol{"scale"},
            {{bg::util::symbol{"input"}, &input_type}, {bg::util::symbol{"factor"}, &bg::float_type()}},
            bg::TS<float>::type()};

        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

  private:
    static bool evaluate(const bg::CompiledNode &, void *, bg::EvaluationContext *) { return true; }
};

class union_input_node_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        using FORM = bg::Union<"NODE_MULTIPLE_SERIES_FORMS", std::int64_t,
                               bg::TS<std::int64_t>, bg::TS<float>>;
        static const auto value = bg::Signature{
            bg::util::symbol{"union_input_node"}, {{bg::util::symbol{"input"}, &FORM::type()}}};
        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return nullptr;
    }
};

class series_only_union_input_node_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        using FORM = bg::Union<"NODE_SERIES_ONLY_FORM", bg::TS<float>>;
        static const auto value = bg::Signature{
            bg::util::symbol{"series_only_union_input_node"}, {{bg::util::symbol{"input"}, &FORM::type()}}};
        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return nullptr;
    }
};

class scalar_or_time_series_node_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        using FORM = bg::Union<"NODE_SCALAR_OR_SERIES", float, bg::TS<float>>;
        static const auto value = bg::Signature{
            bg::util::symbol{"scalar_or_time_series_node"}, {{bg::util::symbol{"input"}, &FORM::type()}}};
        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

  private:
    static bool evaluate(const bg::CompiledNode &, void *, bg::EvaluationContext *) { return true; }
};

} // namespace

TEST_CASE("runtime node templates reject a Union with multiple time-series forms",
          "[feature:wiring][feature:type-system]") {
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sink = union_input_node_template{};
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();

    CHECK_THROWS_AS(sink(input), bg::WiringError);
}

TEST_CASE("runtime node templates reject a time-series-only Union",
          "[feature:wiring][feature:type-system]") {
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sink = series_only_union_input_node_template{};
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();

    CHECK_THROWS_AS(sink(input), bg::WiringError);
}

TEST_CASE("runtime node templates resolve scalar and selected time-series Union forms",
          "[feature:wiring][feature:type-system][feature:wiring-materialisation]") {
    SECTION("the scalar alternative resolves to a scalar node input") {
        const auto sink = scalar_or_time_series_node_template{};
        auto wiring = bg::GraphWiring{};
        const auto context = bg::WiringContext{wiring};
        (void)sink(4.0F);

        wiring.finalize();

        const auto node = std::ranges::find_if(wiring.nodes(), [&](const auto &candidate) {
            return &candidate.node_template() == &sink;
        });
        REQUIRE(node != wiring.nodes().end());
        CHECK(node->signature().parameters[0].type == &bg::float_type());
        CHECK(node->inputs().empty());
        REQUIRE(node->scalar_values().size() == 1);
    }

    SECTION("a deferred source binds its selected time-series form") {
        const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
        const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});
        const auto sink = scalar_or_time_series_node_template{};
        auto wiring = bg::GraphWiring{};
        const auto context = bg::WiringContext{wiring};
        const auto semantic_source = integer_source();
        const auto selected_source = float_source();
        auto materialisation_calls = 0;
        auto deferred = wiring.create_wp<bg::TS<bg::i64>>(semantic_source);
        deferred.materialisation_option<bg::TS<float>>(
            [&](bg::wp<bg::TS<bg::i64>>) {
                ++materialisation_calls;
                return selected_source;
            },
            0.0);

        sink(deferred);
        CHECK(wiring.materialisation_required(deferred, bg::TS<float>::type()));
        wiring.finalize();

        const auto node = std::ranges::find_if(wiring.nodes(), [&](const auto &candidate) {
            return &candidate.node_template() == &sink;
        });
        REQUIRE(node != wiring.nodes().end());
        CHECK(node->signature().parameters[0].type == &bg::TS<float>::type());
        REQUIRE(node->inputs().size() == 1);
        CHECK(wiring.input_consumes(node->inputs()[0], selected_source));
        CHECK(materialisation_calls == 1);
    }
}

TEST_CASE("invoke_node captures the intended template while importing context",
          "[graph_wiring][feature:wiring]") {
    using namespace bg::literals;

    const auto target = bg::node([](bg::In<bg::TS<float>, "value"_n, bg::context>, bg::Out<bg::TS<bool>>) {});
    const auto provider = bg::node([](bg::Out<bg::TS<float>>) {});
    auto outer = bg::GraphWiring{};
    auto inner = bg::GraphWiring{outer};
    auto call = bg::GraphWiring::NodeCallResult{};
    auto outer_provider = bg::WiringPoint{};
    {
        const auto outer_context = bg::WiringContext{outer};
        outer_provider = provider();
        const auto binding = bg::with_context("value"_n = outer_provider);
        {
            const auto inner_context = bg::WiringContext{inner};
            call = inner.invoke_node(target, std::span<const bg::WiringArgument>{});
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
    const auto source = bg::node<source_definition>();
    const auto scale = scale_template{};
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        (void)scale(input, 2.0F);
    }

    (void)wiring.complete_pending_wiring();
    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[1].inputs().size() == 1);
    CHECK(nodes[1].inputs()[0].index() == 0);
    REQUIRE(nodes[1].scalar_values().size() == 1);
    const bg::WiredScalar &factor = nodes[1].scalar_values()[0];
    CHECK(*static_cast<const float *>(factor.data()) == 2.0F);
    CHECK(factor.runtime_type().size() == sizeof(float));
    CHECK(nodes[1].signature().parameters[1].type == &bg::float_type());
}

namespace {

struct optional_scalar_definition {
    std::optional<int> value;

    void evaluate() const {}
};

} // namespace

TEST_CASE("optional scalar parameters can record absence", "[graph_wiring][feature:wiring]") {
    const auto optional_scalar = bg::node<optional_scalar_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        optional_scalar(std::optional<int>{});
    }

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    REQUIRE(wiring.nodes()[0].scalar_values().size() == 1);
    CHECK_FALSE(wiring.nodes()[0].scalar_values()[0].has_value());
}

namespace {

struct required_scalar_definition {
    int value;

    void evaluate() const {}
};

} // namespace

TEST_CASE("required scalar parameters reject absence", "[graph_wiring][feature:wiring]") {
    const auto required_scalar = bg::node<required_scalar_definition>();
    bg::GraphWiring wiring;
    const bg::WiringContext context{wiring};

    try {
        (void)required_scalar(std::optional<int>{});
        FAIL("expected a required scalar to reject absence");
    } catch (const bg::TypeResolutionError &error) {
        CHECK(error.code() == bg::TypeResolutionErrorCode::mismatch);
        CHECK(error.subject().text() == "value");
        CHECK(std::string{error.what()}.contains("required scalar parameter has no value"));
    }
    CHECK_NOTHROW(wiring.finalize());
    CHECK(wiring.nodes().empty());
}

TEST_CASE("node output runtime selector runs at the call and its result is reused when binding",
          "[graph_wiring][feature:wiring][feature:node-api]") {
    const auto &matching_runtime_type = bg::cpp_time_series_runtime_type<bg::TS<float>>();
    const auto &mismatched_runtime_type = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    auto mismatch_selector_calls = 0;
    auto matching_selector_calls = 0;
    const auto mismatch = OutputRuntimeSelectorNode{mismatched_runtime_type, mismatch_selector_calls};
    const auto matching = OutputRuntimeSelectorNode{matching_runtime_type, matching_selector_calls};
    auto wiring = bg::GraphWiring{};
    {
        const bg::WiringContext context{wiring};
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
    auto wiring = bg::GraphWiring{};
    const bg::WiringContext context{wiring};

    try {
        (void)node();
        FAIL("expected output runtime selection to fail");
    } catch (const bg::WiringError &error) {
        CHECK(std::string{error.what()}.contains("output descriptor selection failed"));
        REQUIRE(error.contexts().size() == 1);
        CHECK(error.contexts().front().stage == bg::WiringFailureStage::callback);
        CHECK(error.contexts().front().operation.text() == "throwing_output_runtime_selector");
        CHECK_FALSE(error.contexts().front().path.empty());
    }

    CHECK(wiring.nodes().empty());
    CHECK(wiring.outputs().empty());
}

TEST_CASE("explicit scalar values preserve their declared graph type",
          "[graph_wiring][feature:wiring][scalar:integer]") {
    const auto consume = bg::node([](bg::Scalar<int>) {});
    const auto value = bg::ScalarValue<int>{1};
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    consume(value);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    CHECK(wiring.nodes()[0].signature().parameters[0].type == &bg::int_type());
}

namespace {

struct scheduled_times_definition {
    bg::Scalar<bg::List<bg::EvaluationTime>> times;

    void evaluate() const {}
};

} // namespace

TEST_CASE("evaluation time lists pass through normal wiring", "[graph_wiring][feature:wiring]") {
    const auto scheduled_times = bg::node<scheduled_times_definition>();
    const auto times =
        std::vector{bg::MIN_DT + std::chrono::microseconds{2}, bg::MIN_DT + std::chrono::microseconds{5}};
    const auto value = bg::ScalarValue<bg::List<bg::EvaluationTime>>{times};
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        scheduled_times(value);
    }

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 1);
    REQUIRE(wiring.nodes()[0].scalar_values().size() == 1);
    CHECK(&wiring.nodes()[0].scalar_values()[0].runtime_type().type() ==
          &bg::cpp_scalar_type<bg::List<bg::EvaluationTime>>());
}

TEST_CASE("nested wiring contexts keep graph connections explicit", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node<source_definition>();
    const auto copy = bg::node<copy_definition>();
    bg::GraphWiring outer;
    bg::GraphWiring inner;

    {
        bg::WiringContext outer_context{outer};
        const bg::WiringPoint outer_input = source();

        {
            bg::WiringContext inner_context{inner};

            CHECK_THROWS_AS(copy(outer_input), bg::WiringError);

            const bg::WiringPoint inner_input = source();
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
    bg::GraphWiring wiring;
    bg::WiringContext context{wiring};
    bool child_has_context = true;

    std::thread child{[&child_has_context] {
        try {
            (void)bg::WiringContext::current();
        } catch (const bg::WiringError &) {
            child_has_context = false;
        }
    }};
    child.join();

    CHECK_FALSE(child_has_context);
    CHECK(&bg::WiringContext::current().wiring() == &wiring);
}
