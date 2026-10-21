#include "service_test_helpers.hpp"

#include <bg/run_graph.hpp>
#include <bg/nested.hpp>

#include <chrono>

using namespace bg;

TEST_CASE("reference service output is available at the root graph",
          "[feature:service][feature:reference][series:value][transition:start]") {
    const auto service = reference_service<auto()->TS<i64>>{"counter"};
    const auto source = node<ServiceOutputSource>();

    const auto implementation = graph("name"_n = "counter_impl", [&] { return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto actual = testing::test_graph(service, GraphRunOptions{.services = &catalogue});

    CHECK(actual.equals<i64>({42}));
}

TEST_CASE("reference service output crosses switch boundaries",
          "[feature:service][feature:switch][feature:nested-graph]"
          "[series:value][shape:nested]") {
    const auto service = reference_service<auto()->TS<i64>>{"counter"};
    const auto source = node<ServiceOutputSource>();

    const auto implementation = graph("name"_n = "counter_impl", [&] { return source(); });

    const auto add = node([](i64 service_value, i64 value) { return service_value + value; });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto tested = [&](wp<TS<i64>> selector, wp<TS<i64>> value) {
        return switch_(selector,
                       cases{case_(1, [&](auto input) { return add(service().as<TS<i64>>(), input); }),
                             case_(2, [&](auto input) { return add(service().as<TS<i64>>(), input); })},
                       value)
            .as<TS<i64>>();
    };
    const auto actual = testing::test_graph(tested, GraphRunOptions{.services = &catalogue},
                                            {1, 2, 1}, {4, 8, 3});

    CHECK(actual.equals<i64>({46, 50, 45}));
}

TEST_CASE("reference service implementation can return a changing reference",
          "[feature:service][feature:reference][feature:switch][series:reference]") {
    const auto service = reference_service<auto()->TS<i64>>{"selected"};
    const auto source = node<ServiceOutputSource>();

    const auto implementation = graph("name"_n = "selected_impl", [&]() -> wp<TS<i64>> {
        return switch_(operators::const_(i64{1}),
                       cases{case_(i64{1}, [&] { return source(); }), case_(i64{2}, [&] { return source(); })})
            .as<TS<i64>>();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    CHECK(testing::test_graph(implementation).equals<i64>({42}));
    const auto actual = testing::test_graph(service, GraphRunOptions{.services = &catalogue});

    CHECK(actual.equals<i64>({42}));
}

struct TimedServiceSelector {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(2); }
};

struct AlternateServiceOutputSource {
    Out<TS<i64>> output;

    void start() const { output.set(84); }
    void evaluate() const noexcept {}
};

TEST_CASE("a changing service reference reaches clients through two nested boundaries in the same cycle",
          "[feature:service][feature:reference][feature:nested-graph][feature:switch]"
          "[series:reference][shape:doubly-nested][feature:timed-scheduling]") {
    const auto service = reference_service<auto()->TS<i64>>{"selected"};
    const auto selector = node<TimedServiceSelector>();

    const auto first = node<ServiceOutputSource>();

    const auto second = node<AlternateServiceOutputSource>();

    const auto implementation = graph("name"_n = "timed_selected_impl", [&]() -> wp<TS<i64>> {
        return switch_(selector(),
                       cases{case_(i64{1}, [&] { return first(); }), case_(i64{2}, [&] { return second(); })})
            .as<TS<i64>>();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto leaf = graph("name"_n = "service_leaf", [&] { return service().as<TS<i64>>(); });

    const auto middle =
        graph("name"_n = "service_middle", [&] { return nested(leaf, operators::const_(true)).as<TS<i64>>(); });
    const auto outer = [&] { return nested(middle, operators::const_(true)).as<TS<i64>>(); };
    const auto actual = testing::test_graph(outer, GraphRunOptions{.services = &catalogue});

    CHECK(actual.equals<i64>({42, 84}));
}

TEST_CASE("run_graph builds registered services before compilation",
          "[feature:service][feature:simulation][series:value][shape:source]") {
    const auto service = reference_service<auto()->TS<i64>>{"counter"};
    const auto source = node<ServiceOutputSource>();

    const auto implementation = graph("name"_n = "counter_impl", [&] { return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    auto values = std::vector<i64>{};
    const auto observe = node([&](i64 value) { values.push_back(value); });
    const auto wire = [&] { observe(service().as<TS<i64>>()); };

    run_graph(wire, GraphRunOptions{.services = &catalogue});

    auto global_state = GlobalState{};
    run_graph(wire, GraphRunOptions{.global_state = &global_state, .services = &catalogue});

    CHECK(values == std::vector<i64>{42, 42});
}
