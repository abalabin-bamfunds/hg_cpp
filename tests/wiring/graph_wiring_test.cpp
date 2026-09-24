#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <thread>
#include <type_traits>
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

    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    CHECK(&nodes[0].node_template() == &source);
    CHECK(&nodes[1].node_template() == &copy);
    REQUIRE(nodes[1].input_output_indices().size() == 1);
    CHECK(nodes[1].input_output_indices()[0] == 0);
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

TEST_CASE("wiring reuses a node called with the same arguments", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto scale = bg::node([](bg::In<bg::TS<float>>, float, bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    (void)scale(input, 2.0F);
    (void)scale(input, 2.0F);

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

    CHECK(wiring.nodes().size() == 3);
}

TEST_CASE("wiring keeps calls with different input sources separate", "[graph_wiring][feature:wiring]") {
    const auto first_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto second_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto copy = bg::node([](bg::In<bg::TS<float>>, bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    (void)copy(first_source());
    (void)copy(second_source());

    CHECK(wiring.nodes().size() == 4);
}

TEST_CASE("wiring reuses a node called with the same absent scalar", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node([](std::optional<int>, bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    (void)source(std::optional<int>{});
    (void)source(std::optional<int>{});

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

    CHECK(wiring.nodes().size() == 3);
}

TEST_CASE("a node can disable wiring deduplication", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node(bg::eval = [](bg::Out<bg::TS<float>>) {}, bg::deduplicate_wiring = false);
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    (void)source();
    (void)source();

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
    const auto &expected = bg::REF<bg::TS<std::int64_t>>::type();
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

} // namespace

TEST_CASE("wired nodes retain scalar arguments", "[graph_wiring][feature:wiring]") {
    const auto source = bg::node<source_definition>();
    const auto scale = scale_template{};
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        (void)scale(input, 2.0F);
    }

    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[1].input_output_indices().size() == 1);
    CHECK(nodes[1].input_output_indices()[0] == 0);
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
    bg::WiringContext context{wiring};

    CHECK_THROWS_AS(required_scalar(std::optional<int>{}), bg::WiringError);
}

TEST_CASE("explicit scalar values preserve their declared graph type",
          "[graph_wiring][feature:wiring][scalar:integer]") {
    const auto consume = bg::node([](bg::Scalar<int>) {});
    const auto value = bg::ScalarValue<int>{1};
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    consume(value);

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
