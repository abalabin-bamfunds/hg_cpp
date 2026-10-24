#include "service_test_helpers.hpp"

#include <bg/service_feedback.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>

#include <utility>

using namespace bg;

template <template <typename, auto...> typename Field>
using SERVICE_KWARGS = BundleSchemaVariableDeclaration<"SERVICE_KWARGS", Field>;

TEST_CASE("service request and reply wiring can share a separately wired client ID",
          "[feature:service][series:tsd][shape:multiple][declaration:generic]") {
    using VALUE = ScalarVariable<"CLIENT_REPLY_VALUE">;
    const auto service = bg::service<VALUE, auto(TS<VALUE>, i64)->TS<VALUE>>{"echo_route", {"value", "route"}};
    auto submitted = std::vector<i64>{};
    const auto observe = node([&](In<TSD<service_client_id_t, TS<i64>>> requests) {
        requests.for_each_key_value([&](const auto &, const auto &value) {
            if (value.valid()) {
                submitted.push_back(*value);
            }
        });
    });
    const auto response_source = node([](Out<TSD<service_client_id_t, TS<i64>>> responses) {
        responses[service_client_id_t{123}].set(5);
    });
    const auto implementation = graph<VALUE>(
        "name"_n = "echo_route_impl",
        [&](wp<TSD<service_client_id_t, TS<VALUE>>, "value"_n> requests, i64 route) {
            observe(requests);
            return response_source();
        });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service[type<VALUE> = type<i64>], implementation, "route"_n = 10);

    const auto client_id_source = node([](Out<TS<service_client_id_t, TimeSeriesGuarantee::const_>> output) {
        output.set(123);
    });
    const auto add_request = node([](In<TS<i64>> response, In<TS<i64>> value) { return *response + *value; });
    const auto tested = [&](wp<TS<i64>> input) {
        const auto client_id = client_id_source();
        const auto typed_service = service[type<VALUE> = type<i64>];
        const auto reply = typed_service.client_replies(client_id, 10);
        const auto request = add_request(reply, input);
        typed_service.client_requests(client_id, request, 10);
        return request;
    };

    CHECK(bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 4}).equals<i64>({6, 9}));
    CHECK(submitted == std::vector<i64>{6, 9});
}

TEST_CASE("service reply wiring requires a const client ID", "[feature:service][wiring]") {
    const auto service = bg::service<auto(TS<i64>)->TS<i64>>{"reply_id", {"value"}};
    const auto client_id_source = node([](Out<TS<service_client_id_t>> output) { output.set(123); });

    CHECK_THROWS_WITH(testing::test_wiring([&] { service.client_replies(client_id_source()); }),
                      Catch::Matchers::ContainsSubstring(
                          "a service client ID must be TS<service_client_id_t, const_>"));
}

TEST_CASE("service publishes each valid client input and selects its response",
          "[feature:service][series:tsd][shape:single-node]") {
    const auto service = bg::service<auto(TS<i64>)->TS<i64>>{"double", {"value"}};
    auto client_ids = std::vector<service_client_id_t>{};

    const auto implement = node([&](In<TSD<service_client_id_t, TS<i64>>> requests,
                                    Out<TSD<service_client_id_t, TS<i64>>> responses) {
        requests.for_each_removed([&](const auto &client_id) { responses.erase(client_id); });
        requests.for_each_key_value([&](const auto &client_id, const auto &value) {
            client_ids.push_back(*client_id);
            responses[client_id].set(*value * 2);
        });
    });
    const auto implementation =
        graph("name"_n = "double_impl",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) { return implement(values); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto invalidate_zero = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input == 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto tested = [&](wp<TS<i64>> input) { return service(invalidate_zero(input)); };

    const auto actual = bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {2, 3, 0, 4, 0});

    CHECK(actual.equals<i64>({4, 6, std::nullopt, 8, std::nullopt}));
    REQUIRE(client_ids.size() == 3);
    CHECK(client_ids[0] == client_ids[1]);
    CHECK(client_ids[1] == client_ids[2]);
}

TEST_CASE("service keeps a client active while any input is valid",
          "[feature:service][series:tsd][shape:multiple]") {
    const auto service = bg::service<auto(TS<i64>, TS<float>)->TS<float>>{"sum", {"integer", "fraction"}};

    auto integer_request_sizes = std::vector<std::size_t>{};
    auto fraction_request_sizes = std::vector<std::size_t>{};
    const auto implement = node([&](In<TSD<service_client_id_t, TS<i64>>> integers,
                                    In<TSD<service_client_id_t, TS<float>>> fractions,
                                    Out<TSD<service_client_id_t, TS<float>>> responses) {
        integer_request_sizes.push_back(integers.size());
        fraction_request_sizes.push_back(fractions.size());
        integers.for_each_removed([&](const auto &client_id) {
            if (!fractions.contains(client_id)) {
                responses.erase(client_id);
            }
        });
        fractions.for_each_removed([&](const auto &client_id) {
            if (!integers.contains(client_id)) {
                responses.erase(client_id);
            }
        });

        integers.for_each_key_value([&](const auto &client_id, const auto &integer) {
            const auto fraction = fractions[client_id];
            if (!integer.valid() && !fraction.valid()) {
                responses.erase(client_id);
                return;
            }

            const auto integer_value = integer.valid() ? static_cast<float>(*integer) : 0.0F;
            const auto fraction_value = fraction.valid() ? *fraction : 0.0F;
            responses[client_id].set(integer_value + fraction_value);
        });
    });
    const auto implementation =
        graph("name"_n = "sum_impl", [&](wp<TSD<service_client_id_t, TS<float>>, "fraction"_n> fractions,
                                         wp<TSD<service_client_id_t, TS<i64>>, "integer"_n> integers) {
            return implement(integers, fractions);
        });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto invalidate_zero = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input == 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });

    const auto invalidate_negative = node([](In<TS<float>> input, Out<TS<float>> output) {
        if (*input < 0.0F) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto tested = [&](wp<TS<i64>> integer, wp<TS<float>> fraction) {
        return service(invalidate_zero(integer), invalidate_negative(fraction));
    };

    const auto actual = bg::run_graph(
        tested,
        GraphRunOptions{.services = &catalogue},
        {2, 0, 6, 0},
        {-1.0F, 3.0F, 4.0F, -1.0F}
    );

    CHECK(actual.equals<float>({2.0F, 3.0F, 10.0F, std::nullopt}));
    CHECK(integer_request_sizes == std::vector<std::size_t>{1, 1, 1, 0});
    CHECK(fraction_request_sizes == std::vector<std::size_t>{1, 1, 1, 0});
}

TEST_CASE("service arguments bind by implementation signature before wiring",
          "[feature:service][feature:variadic-wiring][series:tsd][shape:multiple]") {
    const auto service = bg::service<auto(TS<i64>, TS<i64>)->TS<i64>>{"sum_with_offset", {"first", "second"}};
    const auto add = node([](In<TSD<service_client_id_t, TS<i64>>> second, i64 offset,
                             In<TSD<service_client_id_t, TS<i64>>> first,
                             Out<TSD<service_client_id_t, TS<i64>>> output) {
        first.for_each_key_value([&](const auto &client_id, const auto &value) {
            output[client_id].set(*value + *second[client_id] + offset);
        });
    });

    const auto implementation =
        graph("name"_n = "sum_with_offset_impl",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "second"_n> second, i64 offset,
                  wp<TSD<service_client_id_t, TS<i64>>, "first"_n> first) { return add(second, offset, first); });

    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second) { return service(first, second); };

    auto direct_catalogue = ServiceCatalogue{};
    direct_catalogue.register_impl(service, implementation, 10);
    CHECK(bg::run_graph(tested, GraphRunOptions{.services = &direct_catalogue}, {2, 5}, {3, 7})
              .equals<i64>({15, 22}));

    const auto wrapper =
        graph("name"_n = "sum_with_offset_wrapper",
              [&](wp<TSB<SCHEMA>, var_args> positional,
                  wp<TSB<SERVICE_KWARGS>, var_kwargs> named) -> wp<TSD<service_client_id_t, TS<i64>>> {
                  auto supplied = std::vector<WiringArgument>{};
                  bg::details::append_wiring_arguments(supplied, bg::args(positional));
                  bg::details::append_wiring_arguments(supplied, bg::kwargs(named));
                  const auto bound = bg::details::bind_wiring_arguments(implementation.signature(), supplied);
                  const auto output =
                      static_cast<const WiringTemplate &>(implementation)(std::span<const WiringArgument>{bound});
                  return output->as<TSD<service_client_id_t, TS<i64>>>();
              });
    auto wrapped_catalogue = ServiceCatalogue{};
    wrapped_catalogue.register_impl(service, wrapper, 10);
    CHECK(bg::run_graph(tested, GraphRunOptions{.services = &wrapped_catalogue}, {2, 5}, {3, 7})
              .equals<i64>({15, 22}));
}

TEST_CASE("feedback service wrapper delays requests into its nested implementation",
          "[feature:service][feature:feedback][feature:nested-graph][series:tsd][shape:nested]") {
    const auto service = bg::service<auto(TS<i64>)->TS<i64>>{"echo", {"value"}};
    const auto respond = node([](In<TSD<service_client_id_t, TS<i64>>> requests,
                                 Out<TSD<service_client_id_t, TS<i64>>> responses) {
        requests.for_each_removed([&](const auto &client_id) { responses.erase(client_id); });
        requests.for_each_key_value([&](const auto &client_id, const auto &value) {
            responses[client_id].set(*value);
        });
    });

    const auto implementation =
        graph("name"_n = "echo_impl",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests) { return respond(requests); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, feedback_service_implementation(service, implementation));
    const auto tested = [&](wp<TS<i64>> value) { return service(value); };

    const auto actual = bg::run_graph(
        tested, GraphRunOptions{.services = &catalogue}, {2, 4, 6});
    CHECK(actual.equals<i64>({std::nullopt, 2, 4, 6}));
}

TEST_CASE("feedback service wrapper maps generic types and forwards scalar arguments",
          "[feature:service][feature:feedback][feature:nested-graph]"
          "[declaration:generic][scalar:generic][series:tsd][shape:nested]") {
    using VALUE = ScalarVariable<"FEEDBACK_SERVICE_VALUE">;
    const auto service = bg::service<VALUE, auto(TS<VALUE>, i64)->TS<VALUE>>{"generic_echo", {"value", "route"}};
    auto route_11_installation_values = std::vector<i64>{};
    auto route_22_installation_values = std::vector<i64>{};

    const auto respond = node([&](In<TSD<service_client_id_t, TS<VALUE>>> requests, i64 route, i64 installation_value,
                                  Out<TSD<service_client_id_t, TS<VALUE>>> responses) {
        CHECK(requests.size() == 1);
        requests.for_each_removed([&](const auto &client_id) { responses.erase(client_id); });
        requests.for_each_key_value([&](const auto &client_id, const auto &value) {
            if (route == 11) {
                route_11_installation_values.push_back(installation_value);
            } else {
                CHECK(route == 22);
                route_22_installation_values.push_back(installation_value);
            }
            responses[client_id].set_value(value.value_view());
        });
    });

    const auto implementation =
        graph<VALUE>("name"_n = "generic_echo_impl",
                     [&](wp<TSD<service_client_id_t, TS<VALUE>>, "value"_n> requests, i64 route,
                         i64 installation_value) { return respond(requests, route, installation_value); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service[type<VALUE> = type<i64>], feedback_service_implementation(service, implementation),
                            "route"_n = 11, 7);
    catalogue.register_impl(service[type<VALUE> = type<i64>], feedback_service_implementation(service, implementation),
                            "route"_n = 22, 9);
    const auto add = node([](i64 first, i64 second) { return first + second; });
    const auto tested = [&](wp<TS<i64>> value) {
        const auto first = service[type<VALUE> = type<i64>](value, 11);
        const auto second = service[type<VALUE> = type<i64>](value, 22);
        return add(first, second);
    };

    const auto actual = bg::run_graph(
        tested, GraphRunOptions{.services = &catalogue}, {2, 4, 6});

    CHECK(actual.equals<i64>({std::nullopt, 4, 8, 12}));
    REQUIRE_FALSE(route_11_installation_values.empty());
    REQUIRE_FALSE(route_22_installation_values.empty());
    CHECK(std::ranges::all_of(route_11_installation_values, [](const auto value) { return value == 7; }));
    CHECK(std::ranges::all_of(route_22_installation_values, [](const auto value) { return value == 9; }));
}

TEST_CASE("feedback service wrapper caps a final client request to ordinary",
          "[feature:service][feature:feedback][feature:nested-graph][series:tsd][guarantee:final]") {
    const auto service = bg::service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{"final_echo", {"value"}};
    const auto respond = node([](In<TSD<service_client_id_t, TS<i64>>> requests,
                                 Out<TSD<service_client_id_t, TS<i64>>> responses) {
        requests.for_each_key_value([&](const auto &client_id, const auto &value) {
            responses[client_id].set(*value);
        });
    });

    const auto implementation =
        graph("name"_n = "final_echo_impl",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests) { return respond(requests); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, feedback_service_implementation(service, implementation));
    const auto tested = [&] { return service(bg::operators::const_(i64{7})); };

    const auto actual = bg::run_graph(tested, GraphRunOptions{.services = &catalogue});
    CHECK(actual.equals<i64>({std::nullopt, 7}));
}

TEST_CASE("service keeps an unbound reference request as an invalid item",
          "[feature:service][series:reference][shape:multiple]") {
    const auto service = bg::service<auto(TS<i64>, TS<i64>)->TS<i64>>{"reference_status", {"stable", "selected"}};
    auto implementation_evaluations = std::size_t{};
    auto request_states = std::vector<std::array<bool, 5>>{};

    const auto implement = node([&](In<TSD<service_client_id_t, TS<i64>>> stable,
                                    In<TSD<service_client_id_t, TS<i64>>> selected,
                                    Out<TSD<service_client_id_t, TS<i64>>> responses) {
        ++implementation_evaluations;
        stable.for_each_key([&](const auto &client_id) {
            const auto stable_value = stable[client_id];
            const auto selected_value = selected[client_id];
            request_states.push_back({stable.contains(client_id), selected.contains(client_id), stable_value.valid(),
                                      selected_value.valid(), selected.modified()});
            responses[client_id].set(selected_value.valid() ? 1 : 0);
        });
    });
    const auto implementation =
        graph("name"_n = "reference_status_impl", [&](wp<TSD<service_client_id_t, TS<i64>>, "stable"_n> stable,
                                                      wp<TSD<service_client_id_t, TS<i64>>, "selected"_n> selected) {
            return implement(stable, selected);
        });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto select_reference = node([](In<TS<bool>> select, In<REF<TS<i64>>> target, Out<REF<TS<i64>>> output) {
        if (*select) {
            output.copy(target);
        } else {
            output.invalidate();
        }
    });
    const auto tested = [&](wp<TS<i64>> stable, wp<TS<i64>> target, wp<TS<bool>> select) {
        const auto selected = select_reference(select, target);
        return service(stable, selected);
    };

    const auto actual = bg::run_graph(
        tested,
        GraphRunOptions{.services = &catalogue},
        {10, 20, 30, 40, 50, 60},
        {101, 102, 103, 104, 105, 106},
        {false, false, true, false, false, true}
    );

    CHECK(actual.equals<i64>({0, 0, 1, 0, 0, 1}));
    CHECK(implementation_evaluations == 6);
    REQUIRE(request_states.size() == 6);
    CHECK((request_states[0] == std::array<bool, 5>{true, true, true, false, true}));
    CHECK((request_states[1] == std::array<bool, 5>{true, true, true, false, false}));
    CHECK((request_states[2] == std::array<bool, 5>{true, true, true, true, true}));
    CHECK((request_states[3] == std::array<bool, 5>{true, true, true, false, true}));
    CHECK((request_states[4] == std::array<bool, 5>{true, true, true, false, false}));
    CHECK((request_states[5] == std::array<bool, 5>{true, true, true, true, true}));
}

TEST_CASE("sink service receives and withdraws valid client inputs",
          "[feature:service][series:tsd][shape:sink]") {
    const auto service = bg::service<auto(TS<i64>)->void>{"audit", {"value"}};
    auto received = std::vector<i64>{};
    auto removed = std::size_t{};
    auto sizes = std::vector<std::size_t>{};

    const auto observe = node([&](In<TSD<service_client_id_t, TS<i64>>> requests) {
        sizes.push_back(requests.size());
        requests.for_each_removed([&](const auto &) { ++removed; });
        requests.for_each_key_value([&](const auto &, const auto &value) { received.push_back(*value); });
    });
    const auto implementation = graph(
        "name"_n = "audit_impl", [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) { observe(values); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto invalidate_zero = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input == 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto tested = [&](wp<TS<i64>> input) {
        const auto valid = invalidate_zero(input);
        service(valid);
        return valid;
    };

    const auto actual = bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {5, 7, 0, 9});

    CHECK(actual.equals<i64>({5, 7, std::nullopt, 9}));
    CHECK(received == std::vector<i64>{5, 7, 9});
    CHECK(sizes == std::vector<std::size_t>{1, 1, 0, 1});
    CHECK(removed == 1);
}

TEST_CASE("service scalar arguments select separate implementations", "[feature:service][shape:multiple]") {
    const auto service = bg::service<auto(TS<i64>, i64)->TS<i64>>{"add_offset", {"value", "offset"}};

    const auto implement = node([](In<TSD<service_client_id_t, TS<i64>>> requests, i64 offset,
                                   Out<TSD<service_client_id_t, TS<i64>>> responses) {
        requests.for_each_removed([&](const auto &client_id) { responses.erase(client_id); });
        requests.for_each_key_value([&](const auto &client_id, const auto &value) {
            if (value.valid()) {
                responses[client_id].set(*value + offset);
            }
        });
    });
    const auto implementation =
        graph("name"_n = "add_offset_impl", [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values, i64 offset) {
            return implement(values, offset);
        });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation, "offset"_n = 10);
    catalogue.register_impl(service, implementation, "offset"_n = 20);
    const auto sum = node([](i64 first, i64 second) { return first + second; });

    const auto tested = [&](wp<TS<i64>> input) {
        const auto first = service(input, 10);
        const auto second = service(input, 20);
        return sum(first, second);
    };

    CHECK(bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 4}).equals<i64>({32, 38}));
}

TEST_CASE("catch-all service combines unmatched scalar instances and yields to an exact implementation",
          "[feature:service][series:tsd][shape:multiple][scalar:integer]") {
    const auto service = bg::service<auto(TS<i64>, i64)->TS<i64>>{"routed_add", {"value", "instance"}};
    auto catch_all_sizes = std::vector<std::size_t>{};
    const auto respond = node([&](In<TSD<service_client_id_t, TS<i64>>> values,
                                  In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> instances,
                                  Out<TSD<service_client_id_t, TS<i64>>> output) {
        catch_all_sizes.push_back(instances.size());
        values.for_each_removed([&](const auto &id) { output.erase(id); });
        values.for_each_key_value([&](const auto &id, const auto &value) {
            output[id].set(*value + *instances[id]);
        });
    });

    const auto catch_all =
        graph("name"_n = "routed_add_catch_all",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values,
                  wp<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>, "instance"_n> instances) {
                  return respond(values, instances);
              });

    const auto exact_respond = node([](In<TSD<service_client_id_t, TS<i64>>> values, i64 instance,
                                       Out<TSD<service_client_id_t, TS<i64>>> output) {
        values.for_each_key_value([&](const auto &id, const auto &value) {
            output[id].set(*value + instance + 100);
        });
    });

    const auto exact =
        graph("name"_n = "routed_add_exact", [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values,
                                                 i64 instance) { return exact_respond(values, instance); });

    const auto sum =
        node([](In<TS<i64>> first, In<TS<i64>> second, In<TS<i64>> third) { return *first + *second + *third; });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, catch_all);
    catalogue.register_impl(service, exact, "instance"_n = 2);
    const auto tested = [&](wp<TS<i64>> value) {
        return sum(service(value, 1), service(value, 2), service(value, 3));
    };

    const auto actual = bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {4, 6});
    CHECK(actual.equals<i64>({118, 124}));
    CHECK(catch_all_sizes == std::vector<std::size_t>{2, 2});
}

TEST_CASE("catch-all services lift multiple scalar parameters",
          "[feature:service][series:tsd][shape:multiple][scalar:integer]") {
    const auto service = bg::service<auto(TS<i64>, i64, i64)->TS<i64>>{"multi_route", {"value", "region", "instance"}};
    auto group_sizes = std::vector<std::size_t>{};
    const auto respond = node([&](In<TSD<service_client_id_t, TS<i64>>> values,
                                  In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> regions,
                                  In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> instances,
                                  Out<TSD<service_client_id_t, TS<i64>>> output) {
        group_sizes.push_back(values.size());
        values.for_each_removed([&](const auto &id) { output.erase(id); });
        values.for_each_key_value([&](const auto &id, const auto &value) {
            if (value.valid() && regions[id].valid() && instances[id].valid()) {
                output[id].set(*value + *regions[id] + *instances[id]);
            }
        });
    });

    const auto implementation =
        graph("name"_n = "multi_route_catch_all",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values,
                  wp<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>, "region"_n> regions,
                  wp<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>, "instance"_n> instances) {
                  return respond(values, regions, instances);
              });

    const auto sum =
        node([](In<TS<i64>> first, In<TS<i64>> second, Out<TS<i64>> output) { output.set(*first + *second); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    const auto tested = [&](wp<TS<i64>> value) {
        return sum(service(value, 10, 100), service(value, 20, 200));
    };

    CHECK(bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2}).equals<i64>({332, 334}));
    CHECK(group_sizes == std::vector<std::size_t>{2, 2});
}

TEST_CASE("outputless catch-all service withdraws and restores final scalar entries",
          "[feature:service][series:tsd][shape:sink][transition:revalidation]") {
    const auto service = bg::service<auto(TS<i64>, i64)->void>{"routed_audit", {"value", "instance"}};
    auto request_sizes = std::vector<std::size_t>{};
    auto client_ids = std::vector<service_client_id_t>{};
    const auto observe = node([&](In<TSD<service_client_id_t, TS<i64>>> values,
                                  In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> instances) {
        request_sizes.push_back(values.size());
        values.for_each_key_value([&](const auto &id, const auto &) {
            client_ids.push_back(*id);
            CHECK(*instances[id] == 7);
        });
    });

    const auto implementation =
        graph("name"_n = "routed_audit_impl",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values,
                  wp<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>, "instance"_n> instances) {
                  observe(values, instances);
              });

    const auto invalidate_zero = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input == 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    const auto tested = [&](wp<TS<i64>> input) {
        const auto valid = invalidate_zero(input);
        service(valid, 7);
        return valid;
    };

    const auto actual = bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {5, 6, 0, 8});
    CHECK(actual.equals<i64>({5, 6, std::nullopt, 8}));
    CHECK(request_sizes == std::vector<std::size_t>{1, 1, 0, 1});
    REQUIRE(client_ids.size() == 3);
    CHECK(client_ids[0] == client_ids[1]);
    CHECK(client_ids[1] == client_ids[2]);
}

TEST_CASE("catch-all service clients inside map and switch branches forward their scalar values",
          "[feature:service][feature:map][feature:switch][feature:nested-graph][series:tsd][shape:nested]") {
    const auto service = bg::service<auto(TS<i64>, i64)->void>{"nested_route", {"value", "instance"}};
    auto requests = std::vector<std::pair<i64, i64>>{};
    const auto observe = node([&](In<TSD<service_client_id_t, TS<i64>>> values,
                                  In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> instances) {
        values.for_each_key_value([&](const auto &id, const auto &value) {
            if (value.valid() && instances[id].valid()) {
                requests.emplace_back(*value, *instances[id]);
            }
        });
    });

    const auto implementation =
        graph("name"_n = "nested_route_catch_all",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values,
                  wp<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>, "instance"_n> instances) {
                  observe(values, instances);
              });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TSD<i64, TS<i64>>> selectors) {
        const auto mapped = map(
            [&](auto value, auto selector) {
                return switch_(selector,
                               cases{case_(1,
                                           [&](auto selected) {
                                               service(selected, 10);
                                               return selected;
                                           }),
                                     case_(2,
                                           [&](auto selected) {
                                               service(selected, 20);
                                               return selected;
                                           })},
                               value)
                    .template as<TS<i64>>();
            },
            values, selectors);
        return mapped[i64{5}];
    };

    const auto actual =
        bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {{{5, 1}}, {{5, 2}}, {{5, 3}}, {{5, 4}}},
                            {{{5, 1}}, {{5, 2}}, {{5, 1}}, {{5, 2}}});
    CHECK(actual.equals<i64>({1, 2, 3, 4}));
    CHECK(requests == std::vector<std::pair<i64, i64>>{{1, 10}, {2, 20}, {3, 10}, {4, 20}});
}

TEST_CASE("feedback delays catch-all service replies while forwarding scalar values",
          "[feature:service][feature:feedback][feature:nested-graph][series:tsd][shape:nested]") {
    const auto service = bg::service<auto(TS<i64>, i64)->TS<i64>>{"delayed_route", {"value", "instance"}};
    const auto respond = node([](In<TSD<service_client_id_t, TS<i64>>> values,
                                 In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> instances,
                                 Out<TSD<service_client_id_t, TS<i64>>> output) {
        values.for_each_removed([&](const auto &id) { output.erase(id); });
        values.for_each_key_value([&](const auto &id, const auto &value) {
            output[id].set(*value + *instances[id]);
        });
    });

    const auto implementation =
        graph("name"_n = "delayed_route_catch_all",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values,
                  wp<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>, "instance"_n> instances) {
                  return respond(values, instances);
              });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, feedback_service_implementation(service, implementation));
    const auto sum =
        node([](In<TS<i64>> first, In<TS<i64>> second, Out<TS<i64>> output) { output.set(*first + *second); });
    const auto tested = [&](wp<TS<i64>> value) {
        return sum(service(value, 10), service(value, 20));
    };

    const auto actual = bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2, 3});
    CHECK(actual.equals<i64>({std::nullopt, 32, 34, 36}));
}

TEST_CASE("subscription implementation receives named installation inputs after service scalars",
          "[feature:service][shape:multiple]") {
    const auto service =
        subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>, i64)->TS<i64>>{"lookup", {"key", "route"}};

    const auto respond =
        node([](In<TSS<i64>> requests, i64 route, In<TS<i64>> factor, i64 offset, Out<TSD<i64, TS<i64>>> output) {
            requests.for_each_removed([&](const auto &key) { output.erase(key); });
            requests.for_each([&](const auto &key) { output[key].set(*key + route + *factor + offset); });
        });

    const auto implementation =
        graph("name"_n = "lookup_impl", [&](wp<TSS<i64>> requests, i64 route, wp<TS<i64>> factor, i64 offset) {
            return respond(requests, route, factor, offset);
        });

    auto catalogue = ServiceCatalogue{};
    const auto key_source = node<FinalKeySource<i64>>();
    const auto tested = [&](wp<TS<i64>> factor) {
        catalogue.register_impl(service, implementation, "route"_n = 10, factor, 100);
        return service(key_source(1), 10);
    };

    const auto actual = bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {2, 3, 4});

    CHECK(actual.equals<i64>({113, 114, 115}));
}

TEST_CASE("service clients in map branches withdraw requests when branches stop",
          "[feature:service][feature:map][series:tsd][shape:nested]") {
    const auto service = bg::service<auto(TS<i64>)->void>{"audit", {"value"}};
    auto request_sizes = std::vector<std::size_t>{};
    auto added_counts = std::vector<std::size_t>{};
    auto removed_counts = std::vector<std::size_t>{};

    const auto implement = node([&](In<TSD<service_client_id_t, TS<i64>>> requests) {
        request_sizes.push_back(requests.size());
        auto added = std::size_t{};
        requests.for_each_added([&](const auto &) { ++added; });
        added_counts.push_back(added);
        auto removed = std::size_t{};
        requests.for_each_removed([&](const auto &) { ++removed; });
        removed_counts.push_back(removed);
    });

    const auto implementation = graph(
        "name"_n = "audit_impl", [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) { implement(values); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        const auto mapped = map(
            [&](auto value) {
                service(value);
                return value;
            },
            values);
        return mapped[i64{5}];
    };

    const auto actual = bg::run_graph(tested, GraphRunOptions{.services = &catalogue},
                                            {{{5, 7}}, {{5, literal::REMOVE}}, {{5, 7}}});

    CHECK(actual.equals<i64>({7, std::nullopt, 7}));
    CHECK(request_sizes == std::vector<std::size_t>{1, 0, 1});
    CHECK(added_counts == std::vector<std::size_t>{1, 0, 1});
    CHECK(removed_counts == std::vector<std::size_t>{0, 1, 0});
}

TEST_CASE("service clients follow repeated switch changes after a map branch returns",
          "[feature:service][feature:map][feature:switch][feature:nested-graph]"
          "[series:tsd][shape:nested][transition:removal]") {
    const auto service = bg::service<auto(TS<i64>)->void>{"audit", {"value"}};
    auto request_sizes = std::vector<std::size_t>{};
    auto added_counts = std::vector<std::size_t>{};
    auto removed_counts = std::vector<std::size_t>{};

    const auto collect = node([&](In<TSD<service_client_id_t, TS<i64>>> requests) {
        request_sizes.push_back(requests.size());

        auto added = std::size_t{};
        requests.for_each_added([&](const auto &) { ++added; });
        added_counts.push_back(added);

        auto removed = std::size_t{};
        requests.for_each_removed([&](const auto &) { ++removed; });
        removed_counts.push_back(removed);
    });

    const auto implementation = graph(
        "name"_n = "audit_impl", [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) { collect(values); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto add_offset = node([](i64 value, i64 offset) { return value + offset; });
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<i64>> selected_case) {
        const auto mapped = map(
            [&](auto value, auto selector) {
                return switch_(selector,
                               cases{
                                   case_(1,
                                         [&](auto selected) {
                                             service(selected);
                                             return add_offset(selected, 100);
                                         }),
                                   case_(2,
                                         [&](auto selected) {
                                             service(selected);
                                             return add_offset(selected, 200);
                                         }),
                               },
                               value)
                    .template as<TS<i64>>();
            },
            values, selected_case);
        return mapped[i64{5}];
    };

    const auto actual = bg::run_graph(
        tested,
        GraphRunOptions{.services = &catalogue},
        {{{5, 7}}, {{5, 7}}, {{5, 7}}, {{5, literal::REMOVE}}, {{5, 7}}, {{5, 7}}},
        {1, 2, 1, 1, 2, 1}
    );

    CHECK(actual.equals<i64>({107, 207, 107, std::nullopt, 207, 107}));
    CHECK(request_sizes == std::vector<std::size_t>{1, 1, 1, 0, 1, 1});
    CHECK(added_counts == std::vector<std::size_t>{1, 1, 1, 0, 1, 1});
    CHECK(removed_counts == std::vector<std::size_t>{0, 1, 1, 1, 0, 1});
}

TEST_CASE("service rejects a response cycle through a client input",
          "[feature:service][feature:graph-compilation]") {
    const auto service = bg::service<auto(TS<i64>)->TS<i64>>{"identity", {"value"}};
    const auto implement = node([](In<TSD<service_client_id_t, TS<i64>>> requests,
                                   Out<TSD<service_client_id_t, TS<i64>>> responses) {
        requests.for_each_removed([&](const auto &client_id) { responses.erase(client_id); });
        requests.for_each_key_value(
            [&](const auto &client_id, const auto &value) { responses[client_id].set(*value); });
    });
    const auto implementation =
        graph("name"_n = "identity_impl",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) { return implement(values); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto tested = [&](wp<TS<i64>> input) {
        const auto first = service(input);
        return service(first);
    };

    auto diagnostic = std::string{};
    try {
        (void)bg::run_graph(tested, GraphRunOptions{.services = &catalogue}, {1});
    } catch (const std::exception &error) {
        diagnostic = error.what();
    }

    CHECK(diagnostic.find("graph wiring contains a dependency cycle") != std::string::npos);
    INFO(diagnostic);
    CHECK(diagnostic.find("identity_impl[") != std::string::npos);
}
