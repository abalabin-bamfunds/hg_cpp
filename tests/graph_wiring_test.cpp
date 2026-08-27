#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/scalar_type.hpp>
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

    void evaluate() const { output.set(input.value()); }
};

inline const auto source = bg::node<source_definition>();
inline const auto copy = bg::node<copy_definition>();

class scale_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        static const bg::GenericTimeSeriesValueType input_type{bg::float_type(), bg::TimeSeriesGuarantee::ordinary};
        static const bg::Signature value{
            bg::util::symbol{"scale"},
            {{bg::util::symbol{"input"}, &input_type}, {bg::util::symbol{"factor"}, &bg::float_type()}},
            bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary)};

        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

    [[nodiscard]] const bg::ScalarStorageRepresentation *
    output_storage_representation(const bg::ResolvedSignature &,
                                  std::span<const bg::ScalarStorageRepresentation *const>) const noexcept override {
        return &bg::cpp_scalar_storage_representation<float>();
    }

  private:
    static void evaluate(const bg::CompiledNode &, void *, bg::EvaluationContext *) {}
};

inline const scale_template scale;

struct optional_scalar_definition {
    std::optional<int> value;

    void evaluate() const {}
};

struct required_scalar_definition {
    int value;

    void evaluate() const {}
};

inline const auto optional_scalar = bg::node<optional_scalar_definition>();
inline const auto required_scalar = bg::node<required_scalar_definition>();

struct scheduled_times_definition {
    bg::List<bg::EvaluationTime> times;

    void evaluate() const {}
};

inline const auto scheduled_times = bg::node<scheduled_times_definition>();

static_assert(sizeof(bg::WiringPoint) == 2 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<bg::WiringPoint>);

} // namespace

TEST_CASE("node calls add wired nodes to the active graph", "[graph_wiring]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        const bg::WiringPoint output = copy(input);

        CHECK(&input.graph() == &wiring);
        CHECK(input.node_index() == 0);
        CHECK(output.node_index() == 1);
    }

    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    CHECK(&nodes[0].node_template() == &source);
    CHECK(&nodes[1].node_template() == &copy);
    REQUIRE(nodes[1].input_sources().size() == 1);
    CHECK(nodes[1].input_sources()[0] == 0);
}

TEST_CASE("a node call requires a wiring context", "[graph_wiring]") {
    CHECK_THROWS_AS(source(), bg::WiringError);
}

TEST_CASE("wired nodes retain scalar arguments", "[graph_wiring]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        (void)scale(input, 2.0F);
    }

    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[1].input_sources().size() == 1);
    CHECK(nodes[1].input_sources()[0] == 0);
    REQUIRE(nodes[1].scalar_values().size() == 1);
    const bg::WiredScalar &factor = nodes[1].scalar_values()[0];
    CHECK(*static_cast<const float *>(factor.data()) == 2.0F);
    CHECK(factor.storage_representation().size() == sizeof(float));
    CHECK(nodes[1].signature().parameters[1].type == &bg::float_type());
}

TEST_CASE("optional scalar parameters can record absence", "[graph_wiring]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        optional_scalar(std::optional<int>{});
    }

    REQUIRE(wiring.nodes().size() == 1);
    REQUIRE(wiring.nodes()[0].scalar_values().size() == 1);
    CHECK_FALSE(wiring.nodes()[0].scalar_values()[0].has_value());
}

TEST_CASE("required scalar parameters reject absence", "[graph_wiring]") {
    bg::GraphWiring wiring;
    bg::WiringContext context{wiring};

    CHECK_THROWS_AS(required_scalar(std::optional<int>{}), bg::WiringError);
}

TEST_CASE("evaluation time lists pass through normal wiring", "[graph_wiring]") {
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
    CHECK(&wiring.nodes()[0].scalar_values()[0].storage_representation().type() ==
          &bg::cpp_scalar_type<bg::List<bg::EvaluationTime>>());
}

TEST_CASE("nested wiring contexts keep graph connections explicit", "[graph_wiring]") {
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

TEST_CASE("wiring contexts are local to one thread", "[graph_wiring]") {
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
