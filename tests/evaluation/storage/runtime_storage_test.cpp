#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/scalar/string.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include "../../runtime_graph_access.hpp"
#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <type_traits>

namespace {

struct double_source_definition {
    bg::Out<bg::TS<double>> output;

    void evaluate() const { output.set(1.5); }
};

struct double_sink_definition {
    bg::In<bg::TS<double>> input;

    void evaluate() const {
        ++evaluation_count;
        last_value = *input;
    }

    inline static int evaluation_count{};
    inline static double last_value{};
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("runtime storage uses the output scalar runtime_type", "[evaluation][storage][feature:graph-compilation]") {
    const auto double_source = bg::node<double_source_definition>();
    const auto double_sink = bg::node<double_sink_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        double_sink(double_source());
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};

    double_sink_definition::evaluation_count = 0;
    runtime.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(1));

    CHECK(double_sink_definition::evaluation_count == 1);
    CHECK(double_sink_definition::last_value == 1.5);
}

namespace {

struct constant_definition {
    float value;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(value); }
};

struct float_sink_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const {
        ++evaluation_count;
        last_value = *input;
    }

    inline static int evaluation_count{};
    inline static float last_value{};
};

} // namespace

TEST_CASE("a node without output uses no output storage", "[evaluation][storage][feature:graph-compilation]") {
    const auto constant = bg::node<constant_definition>();
    const auto float_sink = bg::node<float_sink_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        float_sink(constant(1.0F));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    float_sink_definition::evaluation_count = 0;

    CHECK_FALSE(compiled.node(1).has_output());
    CHECK(compiled.runtime_storage_size() == 36);

    runtime.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(1));

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 1.0F);
}

namespace {

struct add_definition {
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>> rhs;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(*lhs + *rhs);
    }

    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("runtime graphs keep separate runtime storage", "[evaluation][storage][feature:graph-compilation]") {
    const auto constant = bg::node<constant_definition>();
    const auto add = bg::node<add_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        (void)add(constant(1.0F), constant(2.0F));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph first{compiled};

    first.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(first, 0);
    bg::testing::RuntimeGraphAccess::schedule(first, 1);
    first.evaluate(time(1));

    bg::RuntimeGraph second{compiled};
    add_definition::evaluation_count = 0;
    second.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(second, 2);
    second.evaluate(time(1));

    CHECK(add_definition::evaluation_count == 0);
}

namespace {

struct string_dictionary_source_definition {
    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::string>>> output;

    void start() const {
        for (auto key = std::int64_t{}; key != 16; ++key) {
            output[key].set("a string long enough to require allocated storage");
        }
    }
    void evaluate() const {}
};

} // namespace

TEST_CASE("a runtime graph releases dictionary time-series storage",
          "[evaluation][storage][tsd][series:tsd][feature:graph-compilation]") {
    const auto source = bg::node<string_dictionary_source_definition>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        static_cast<void>(source());
    }

    const auto compiled = bg::CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    {
        auto runtime = bg::RuntimeGraph{compiled, bg::allocator{memory}};
        runtime.start(time(1));
        CHECK(memory.allocations() > 1);
    }

    CHECK(memory.allocations() == 0);
}

namespace {

struct counter_state {
    static constexpr bool supports_byte_copy_initialization = true;

    counter_state() = default;
    counter_state(const counter_state &other) noexcept : value{other.value} {}
    counter_state &operator=(const counter_state &) = delete;

    int value{};
};

static_assert(!std::is_trivially_copyable_v<counter_state>);
static_assert(bg::util::byte_copy_initializable<counter_state>);

struct alignas(16) accumulated_state {
    int value{};
};

struct stateful_scalar_definition {
    int factor;
    std::optional<int> increment;
    bg::State<counter_state> state;
    bg::State<accumulated_state> accumulated;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        state->value += increment.value_or(1);
        accumulated->value += state->value;
        output.set(accumulated->value * factor);
    }
};

struct int_sink_definition {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate() const { last_value = *input; }

    inline static int last_value{};
};

} // namespace

TEST_CASE("scalar fields and state bind to their storage", "[evaluation][storage][feature:graph-compilation]") {
    const auto stateful_scalar = bg::node<stateful_scalar_definition>();
    const auto int_sink = bg::node<int_sink_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        int_sink(stateful_scalar(2, std::optional<int>{}));
    }

    const bg::CompiledGraph compiled{wiring};
    const auto &node = compiled.node(0);

    REQUIRE(node.parameters().size() == 2);
    CHECK(*static_cast<const int *>(node.parameters()[0].scalar_data()) == 2);
    CHECK(node.parameters()[1].scalar_data() == nullptr);
    CHECK(node.has_state());

    bg::RuntimeGraph first{compiled};
    first.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(first, 0);
    first.evaluate(time(1));
    CHECK(int_sink_definition::last_value == 2);

    bg::testing::RuntimeGraphAccess::schedule(first, 0);
    first.evaluate(time(2));
    CHECK(int_sink_definition::last_value == 6);

    bg::RuntimeGraph second{compiled};
    second.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(second, 0);
    second.evaluate(time(1));
    CHECK(int_sink_definition::last_value == 2);
}
