#include "service_test_helpers.hpp"

#include <bg/run_graph.hpp>
#include <bg/nested.hpp>

#include <chrono>

TEST_CASE("reference service output crosses switch boundaries",
          "[feature:service][feature:switch][feature:nested-graph]"
          "[series:value][shape:nested]") {
    const auto service = reference_service<auto()->TS<bg::i64>>{"counter"};
    const auto source = node<ServiceOutputSource>();
    const auto implementation = graph("name"_n = "counter_impl", [&] { return source(); });
    const auto add = node([](In<TS<bg::i64>> service_value, In<TS<bg::i64>> value,
                             Out<TS<bg::i64>> output) { output.set(*service_value + *value); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto tested = [&](wp<TS<bg::i64>> selector, wp<TS<bg::i64>> value) {
        auto result = switch_(selector,
                              cases{case_(1, [&](auto input) { return add(service().as<TS<bg::i64>>(), input); }),
                                    case_(2, [&](auto input) { return add(service().as<TS<bg::i64>>(), input); })},
                              value)
                          .as<TS<bg::i64>>();
        return result;
    };

    const auto actual = testing::test_graph(
        tested,
        bg::GraphRunOptions{.services = &catalogue},
        std::array<bg::i64,
        3>{1, 2, 1},
        std::array<bg::i64,
        3>{4, 8, 3}
    );

    CHECK(actual.equals<bg::i64>({46, 50, 45}));
}

TEST_CASE("reference service output is available at the root graph",
          "[feature:service][feature:reference][series:value][transition:start]") {
    const auto service = reference_service<auto()->TS<bg::i64>>{"counter"};
    const auto source = node<ServiceOutputSource>();
    const auto implementation = graph("name"_n = "counter_impl", [&] { return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto actual = testing::test_graph(service, bg::GraphRunOptions{.services = &catalogue});

    CHECK(actual.equals<bg::i64>({42}));
}

TEST_CASE("reference service implementation can return a changing reference",
          "[feature:service][feature:reference][feature:switch][series:reference]") {
    const auto service = reference_service<auto()->TS<bg::i64>>{"selected"};
    const auto source = node<ServiceOutputSource>();
    const auto implementation = graph("name"_n = "selected_impl", [&]() -> wp<TS<bg::i64>> {
        return switch_(operators::const_(bg::i64{1}),
                       cases{case_(bg::i64{1}, [&] { return source(); }),
                             case_(bg::i64{2}, [&] { return source(); })})
            .as<TS<bg::i64>>();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    CHECK(testing::test_graph(implementation).equals<bg::i64>({42}));
    const auto actual = testing::test_graph(service, bg::GraphRunOptions{.services = &catalogue});

    CHECK(actual.equals<bg::i64>({42}));
}

struct TimedServiceSelector {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<bg::i64>> output;

    void start(EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(2); }
};

struct AlternateServiceOutputSource {
    Out<TS<bg::i64>> output;

    void start() const { output.set(84); }
    void evaluate() const noexcept {}
};

TEST_CASE("a changing service reference reaches clients through two nested boundaries in the same cycle",
          "[feature:service][feature:reference][feature:nested-graph][feature:switch]"
          "[series:reference][shape:doubly-nested][feature:timed-scheduling]") {
    const auto service = reference_service<auto()->TS<bg::i64>>{"selected"};
    const auto selector = node<TimedServiceSelector>();
    const auto first = node<ServiceOutputSource>();
    const auto second = node<AlternateServiceOutputSource>();
    const auto implementation = graph("name"_n = "timed_selected_impl", [&]() -> wp<TS<bg::i64>> {
        return switch_(selector(),
                       cases{case_(bg::i64{1}, [&] { return first(); }),
                             case_(bg::i64{2}, [&] { return second(); })})
            .as<TS<bg::i64>>();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto leaf = graph("name"_n = "service_leaf", [&] { return service().as<TS<bg::i64>>(); });
    const auto middle = graph("name"_n = "service_middle", [&] {
        return nested(leaf, operators::const_(true)).as<TS<bg::i64>>();
    });
    auto values = std::vector<bg::i64>{};
    const auto observe = node([&](In<TS<bg::i64>> value) { values.push_back(*value); });

    run_graph([&] { observe(nested(middle, operators::const_(true)).as<TS<bg::i64>>()); },
              bg::GraphRunOptions{.services = &catalogue});

    CHECK(values == std::vector<bg::i64>{42, 84});
}

TEST_CASE("run_graph builds registered services before compilation",
          "[feature:service][feature:simulation][series:value][shape:source]") {
    const auto service = reference_service<auto()->TS<bg::i64>>{"counter"};
    const auto source = node<ServiceOutputSource>();
    const auto implementation = graph("name"_n = "counter_impl", [&] { return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    auto values = std::vector<bg::i64>{};
    const auto observe = node([&](In<TS<bg::i64>> input) { values.push_back(*input); });
    const auto wire = [&] { observe(service().as<TS<bg::i64>>()); };

    run_graph(wire, bg::GraphRunOptions{.services = &catalogue});

    auto global_state = GlobalState{};
    run_graph(wire, bg::GraphRunOptions{.global_state = &global_state, .services = &catalogue});

    CHECK(values == std::vector<bg::i64>{42, 42});
}
