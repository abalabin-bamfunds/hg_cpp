#include "service_test_helpers.hpp"

#include <bg/run_graph.hpp>

TEST_CASE("reference service output crosses switch boundaries",
          "[feature:service][feature:switch][feature:nested-graph]"
          "[series:value][shape:nested]") {
    const auto service = reference_service<auto()->TS<std::int64_t>>{"counter"};
    const auto source = node<ServiceOutputSource>();
    const auto implementation = graph("name"_n = "counter_impl", [&] { return source(); });
    const auto add = node([](In<TS<std::int64_t>> service_value, In<TS<std::int64_t>> value,
                             Out<TS<std::int64_t>> output) { output.set(*service_value + *value); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto tested = [&](wp<TS<std::int64_t>> selector, wp<TS<std::int64_t>> value) {
        auto result = switch_(selector,
                              cases{case_(1, [&](auto input) { return add(service().as<TS<std::int64_t>>(), input); }),
                                    case_(2, [&](auto input) { return add(service().as<TS<std::int64_t>>(), input); })},
                              value)
                          .as<TS<std::int64_t>>();
        return result;
    };

    const auto actual = testing::test_graph(tested, catalogue, std::array<std::int64_t, 3>{1, 2, 1},
                                            std::array<std::int64_t, 3>{4, 8, 3});

    CHECK(actual.equals<std::int64_t>({46, 50, 45}));
}

TEST_CASE("reference service output is available at the root graph",
          "[feature:service][feature:reference][series:value][transition:start]") {
    const auto service = reference_service<auto()->TS<std::int64_t>>{"counter"};
    const auto source = node<ServiceOutputSource>();
    const auto implementation = graph("name"_n = "counter_impl", [&] { return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto actual = testing::test_graph(service, catalogue);

    CHECK(actual.equals<std::int64_t>({42}));
}

TEST_CASE("run_graph builds registered services before compilation",
          "[feature:service][feature:simulation][series:value][shape:source]") {
    const auto service = reference_service<auto()->TS<std::int64_t>>{"counter"};
    const auto source = node<ServiceOutputSource>();
    const auto implementation = graph("name"_n = "counter_impl", [&] { return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    auto values = std::vector<std::int64_t>{};
    const auto observe = node([&](In<TS<std::int64_t>> input) { values.push_back(*input); });
    const auto wire = [&] { observe(service().as<TS<std::int64_t>>()); };

    run_graph(wire, catalogue);

    auto global_state = GlobalState{};
    run_graph(wire, global_state, catalogue);

    CHECK(values == std::vector<std::int64_t>{42, 42});
}
