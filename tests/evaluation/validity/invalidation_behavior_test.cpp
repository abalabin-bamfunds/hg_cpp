#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../../runtime_graph_access.hpp"

#include <chrono>

namespace {

struct switchable_definition {
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        if (publish) {
            output.set(4.0F);
        } else {
            output.invalidate();
        }
    }

    inline static bool publish{true};
};

struct float_sink_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const {
        ++evaluation_count;
        last_value = input.value();
    }

    inline static int evaluation_count{};
    inline static float last_value{};
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

struct copy_definition {
    bg::In<bg::TS<float>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(input.value()); }
};

TEST_CASE("required-input invalidation propagates through a graph", "[evaluation][invalidation]") {
    const auto switchable = bg::node<switchable_definition>();
    const auto copy = bg::node<copy_definition>();
    const auto float_sink = bg::node<float_sink_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        float_sink(copy(switchable()));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    switchable_definition::publish = true;
    float_sink_definition::evaluation_count = 0;

    runtime.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(1));
    REQUIRE(float_sink_definition::evaluation_count == 1);

    switchable_definition::publish = false;
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(2));

    CHECK(float_sink_definition::evaluation_count == 1);
}

namespace {

struct fallback_definition {
    bg::In<bg::TS<float>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(input.value());
    }

    void on_invalidation() const {
        ++invalidation_count;
        output.set(-1.0F);
    }

    inline static int evaluation_count{};
    inline static int invalidation_count{};
};

} // namespace

TEST_CASE("required-input invalidation is applied once while the input remains invalid", "[evaluation][invalidation]") {
    const auto switchable = bg::node<switchable_definition>();
    const auto fallback = bg::node<fallback_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        (void)fallback(switchable());
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    switchable_definition::publish = true;
    fallback_definition::evaluation_count = 0;
    fallback_definition::invalidation_count = 0;

    runtime.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(1));
    REQUIRE(fallback_definition::evaluation_count == 1);

    switchable_definition::publish = false;
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(2));
    REQUIRE(fallback_definition::invalidation_count == 1);

    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(3));

    CHECK(fallback_definition::evaluation_count == 1);
    CHECK(fallback_definition::invalidation_count == 1);
}

struct hold_definition {
    static constexpr auto on_invalidation = bg::preserve_output;

    bg::In<bg::TS<float>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(input.value());
    }

    inline static int evaluation_count{};
};

struct validity_observer_definition {
    bg::In<bg::TS<float>, bg::optional> input;

    void evaluate() const {
        ++evaluation_count;
        last_valid = input.valid();
    }

    inline static int evaluation_count{};
    inline static bool last_valid{};
};

TEST_CASE("a node can preserve its output when a required input becomes invalid", "[evaluation][invalidation]") {
    const auto switchable = bg::node<switchable_definition>();
    const auto hold = bg::node<hold_definition>();
    const auto validity_observer = bg::node<validity_observer_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        validity_observer(hold(switchable()));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    switchable_definition::publish = true;
    hold_definition::evaluation_count = 0;
    validity_observer_definition::evaluation_count = 0;

    runtime.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(1));

    REQUIRE(hold_definition::evaluation_count == 1);
    REQUIRE(validity_observer_definition::evaluation_count == 1);
    REQUIRE(validity_observer_definition::last_valid);

    switchable_definition::publish = false;
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(2));

    CHECK(hold_definition::evaluation_count == 1);
    CHECK(validity_observer_definition::evaluation_count == 1);
}

TEST_CASE("a node can produce an output when a required input becomes invalid", "[evaluation][invalidation]") {
    const auto switchable = bg::node<switchable_definition>();
    const auto fallback = bg::node<fallback_definition>();
    const auto float_sink = bg::node<float_sink_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        float_sink(fallback(switchable()));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    switchable_definition::publish = true;
    fallback_definition::evaluation_count = 0;
    fallback_definition::invalidation_count = 0;
    float_sink_definition::evaluation_count = 0;

    runtime.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(1));

    REQUIRE(fallback_definition::evaluation_count == 1);
    REQUIRE(fallback_definition::invalidation_count == 0);
    REQUIRE(float_sink_definition::last_value == 4.0F);

    switchable_definition::publish = false;
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(2));

    CHECK(fallback_definition::evaluation_count == 1);
    CHECK(fallback_definition::invalidation_count == 1);
    CHECK(float_sink_definition::evaluation_count == 2);
    CHECK(float_sink_definition::last_value == -1.0F);
}

namespace {

struct throwing_invalidation_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const {}
    void on_invalidation() const { throw std::runtime_error{"invalidation failure"}; }
    void stop() const noexcept { ++stop_count; }

    inline static int stop_count{};
};

} // namespace

TEST_CASE("an invalidation failure stops the runtime graph", "[evaluation][invalidation][lifecycle]") {
    const auto switchable = bg::node<switchable_definition>();
    const auto throwing_invalidation = bg::node<throwing_invalidation_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        throwing_invalidation(switchable());
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    switchable_definition::publish = true;
    throwing_invalidation_definition::stop_count = 0;

    runtime.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    runtime.evaluate(time(1));

    switchable_definition::publish = false;
    bg::testing::RuntimeGraphAccess::schedule(runtime, 0);
    CHECK_THROWS_AS(runtime.evaluate(time(2)), std::runtime_error);
    CHECK(throwing_invalidation_definition::stop_count == 1);
    CHECK_THROWS_AS(runtime.evaluate(time(3)), std::logic_error);
}

} // namespace
