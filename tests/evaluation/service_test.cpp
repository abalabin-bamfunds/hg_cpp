#include "service_test_helpers.hpp"

TEST_CASE("service publishes each valid client input and selects its response",
          "[feature:service][series:tsd][shape:single-node]") {
    const auto service = bg::service<auto(TS<std::int64_t>)->TS<std::int64_t>>{"double", {"value"}};
    auto client_ids = std::vector<service_client_id_t>{};

    const auto implement = node([&](In<TSD<service_client_id_t, TS<std::int64_t>>> requests,
                                    Out<TSD<service_client_id_t, TS<std::int64_t>>> responses) {
        requests.for_each_removed([&](const auto &client_id) { responses.erase(client_id); });
        requests.for_each_key_value([&](const auto &client_id, const auto &value) {
            client_ids.push_back(*client_id);
            responses[client_id].set(*value * 2);
        });
    });
    const auto implementation =
        graph("name"_n = "double_impl", [&](wp<TSD<service_client_id_t, TS<std::int64_t>>, "value"_n> values) {
            return implement(values);
        });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto invalidate_zero = node([](In<TS<std::int64_t>> input, Out<TS<std::int64_t>> output) {
        if (*input == 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto tested = [&](wp<TS<std::int64_t>> input) {
        return service(invalidate_zero(input)).as<TS<std::int64_t>>();
    };

    const auto actual = testing::test_graph(tested, catalogue, {2, 3, 0, 4, 0});

    CHECK(actual.equals<std::int64_t>({4, 6, std::nullopt, 8, std::nullopt}));
    REQUIRE(client_ids.size() == 3);
    CHECK(client_ids[0] == client_ids[1]);
    CHECK(client_ids[1] == client_ids[2]);
}

TEST_CASE("service keeps a client active while any input is valid",
          "[feature:service][series:tsd][shape:multiple]") {
    const auto service = bg::service<auto(TS<std::int64_t>, TS<float>)->TS<float>>{"sum", {"integer", "fraction"}};

    auto integer_request_sizes = std::vector<std::size_t>{};
    auto fraction_request_sizes = std::vector<std::size_t>{};
    const auto implement = node([&](In<TSD<service_client_id_t, TS<std::int64_t>>> integers,
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
        graph("name"_n = "sum_impl",
              [&](wp<TSD<service_client_id_t, TS<float>>, "fraction"_n> fractions,
                  wp<TSD<service_client_id_t, TS<std::int64_t>>, "integer"_n> integers) {
                  return implement(integers, fractions);
              });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto invalidate_zero = node([](In<TS<std::int64_t>> input, Out<TS<std::int64_t>> output) {
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
    const auto tested = [&](wp<TS<std::int64_t>> integer, wp<TS<float>> fraction) {
        return service(invalidate_zero(integer), invalidate_negative(fraction)).as<TS<float>>();
    };

    const auto actual = testing::test_graph(tested, catalogue, std::array<std::int64_t, 4>{2, 0, 6, 0},
                                            std::array<float, 4>{-1.0F, 3.0F, 4.0F, -1.0F});

    CHECK(actual.equals<float>({2.0F, 3.0F, 10.0F, std::nullopt}));
    CHECK((actual[0] && actual[0]->value() == 2.0F));
    CHECK((actual[1] && actual[1]->value() == 3.0F));
    CHECK((actual[2] && actual[2]->value() == 10.0F));
    CHECK_FALSE(actual[3]);
    CHECK(integer_request_sizes == std::vector<std::size_t>{1, 1, 1, 0});
    CHECK(fraction_request_sizes == std::vector<std::size_t>{1, 1, 1, 0});
}

TEST_CASE("service keeps an unbound reference request as an invalid item",
          "[feature:service][series:reference][shape:multiple]") {
    const auto service = bg::service<auto(TS<std::int64_t>, TS<std::int64_t>)->TS<std::int64_t>>{
        "reference_status", {"stable", "selected"}};
    auto implementation_evaluations = std::size_t{};
    auto request_states = std::vector<std::array<bool, 5>>{};

    const auto implement = node([&](In<TSD<service_client_id_t, TS<std::int64_t>>> stable,
                                    In<TSD<service_client_id_t, TS<std::int64_t>>> selected,
                                    Out<TSD<service_client_id_t, TS<std::int64_t>>> responses) {
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
        graph("name"_n = "reference_status_impl",
              [&](wp<TSD<service_client_id_t, TS<std::int64_t>>, "stable"_n> stable,
                  wp<TSD<service_client_id_t, TS<std::int64_t>>, "selected"_n> selected) {
                  return implement(stable, selected);
              });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto select_reference = node([](In<TS<bool>> select, In<REF<TS<std::int64_t>>> target,
                                          Out<REF<TS<std::int64_t>>> output) {
        if (*select) {
            output.copy(target);
        } else {
            output.invalidate();
        }
    });
    const auto tested = [&](wp<TS<std::int64_t>> stable, wp<TS<std::int64_t>> target, wp<TS<bool>> select) {
        const auto selected = select_reference(select, target.as<REF<TS<std::int64_t>>>());
        return service(stable, selected).as<TS<std::int64_t>>();
    };

    const auto actual = testing::test_graph(tested, catalogue, std::array<std::int64_t, 6>{10, 20, 30, 40, 50, 60},
                                            std::array<std::int64_t, 6>{101, 102, 103, 104, 105, 106},
                                            std::array<bool, 6>{false, false, true, false, false, true});

    CHECK(actual.equals<std::int64_t>({0, 0, 1, 0, 0, 1}));
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
    const auto service = bg::service<auto(TS<std::int64_t>)->void>{"audit", {"value"}};
    auto received = std::vector<std::int64_t>{};
    auto removed = std::size_t{};
    auto sizes = std::vector<std::size_t>{};

    const auto observe = node([&](In<TSD<service_client_id_t, TS<std::int64_t>>> requests) {
        sizes.push_back(requests.size());
        requests.for_each_removed([&](const auto &) { ++removed; });
        requests.for_each_key_value([&](const auto &, const auto &value) { received.push_back(*value); });
    });
    const auto implementation =
        graph("name"_n = "audit_impl", [&](wp<TSD<service_client_id_t, TS<std::int64_t>>, "value"_n> values) {
            observe(values);
        });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto invalidate_zero = node([](In<TS<std::int64_t>> input, Out<TS<std::int64_t>> output) {
        if (*input == 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto tested = [&](wp<TS<std::int64_t>> input) {
        const auto valid = invalidate_zero(input);
        service(valid);
        return valid;
    };

    const auto actual = testing::test_graph(tested, catalogue, std::array<std::int64_t, 4>{5, 7, 0, 9});

    CHECK(actual.equals<std::int64_t>({5, 7, std::nullopt, 9}));
    CHECK(received == std::vector<std::int64_t>{5, 7, 9});
    CHECK(sizes == std::vector<std::size_t>{1, 1, 0, 1});
    CHECK(removed == 1);
}

TEST_CASE("service scalar arguments select separate implementations", "[feature:service][shape:multiple]") {
    const auto service =
        bg::service<auto(TS<std::int64_t>, std::int64_t)->TS<std::int64_t>>{"add_offset", {"value", "offset"}};

    const auto implement = node([](In<TSD<service_client_id_t, TS<std::int64_t>>> requests, std::int64_t offset,
                                   Out<TSD<service_client_id_t, TS<std::int64_t>>> responses) {
        requests.for_each_removed([&](const auto &client_id) { responses.erase(client_id); });
        requests.for_each_key_value([&](const auto &client_id, const auto &value) {
            if (value.valid()) {
                responses[client_id].set(*value + offset);
            }
        });
    });
    const auto implementation =
        graph("name"_n = "add_offset_impl",
              [&](wp<TSD<service_client_id_t, TS<std::int64_t>>, "value"_n> values, std::int64_t offset) {
                  return implement(values, offset);
              });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation, "offset"_n = 10);
    catalogue.register_impl(service, implementation, "offset"_n = 20);
    const auto sum = node([](In<TS<std::int64_t>> first, In<TS<std::int64_t>> second, Out<TS<std::int64_t>> output) {
        output.set(*first + *second);
    });

    const auto tested = [&](wp<TS<std::int64_t>> input) {
        const auto first = service(input, 10).as<TS<std::int64_t>>();
        const auto second = service(input, 20).as<TS<std::int64_t>>();
        return sum(first, second);
    };

    CHECK(testing::test_graph(tested, catalogue, std::array<std::int64_t, 2>{1, 4}).equals<std::int64_t>({32, 38}));
}

TEST_CASE("subscription implementation receives named installation inputs after service scalars",
          "[feature:service][shape:multiple]") {
    const auto service =
        subscription_service<auto(TS<std::int64_t, TimeSeriesGuarantee::final>, std::int64_t)->TS<std::int64_t>>{
            "lookup", {"key", "route"}};

    const auto respond = node([](In<TSS<std::int64_t>> requests, std::int64_t route, In<TS<std::int64_t>> factor,
                                 std::int64_t offset, Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
        requests.for_each_removed([&](const auto &key) { output.erase(key); });
        requests.for_each([&](const auto &key) { output[key].set(*key + route + *factor + offset); });
    });
    const auto implementation = graph(
        "name"_n = "lookup_impl", [&](wp<TSS<std::int64_t>> requests, std::int64_t route, wp<TS<std::int64_t>> factor,
                                      std::int64_t offset) { return respond(requests, route, factor, offset); });

    auto catalogue = ServiceCatalogue{};
    const auto key_source = node<FinalKeySource<std::int64_t>>();
    const auto tested = [&](wp<TS<std::int64_t>> factor) {
        catalogue.register_impl(service, implementation, "route"_n = 10, factor, 100);
        return service(key_source(1), 10).as<TS<std::int64_t>>();
    };

    const auto actual = testing::test_graph(tested, catalogue, std::array<std::int64_t, 3>{2, 3, 4});

    CHECK(actual.equals<std::int64_t>({113, 114, 115}));
}

TEST_CASE("service clients in map branches withdraw requests when branches stop",
          "[feature:service][feature:map][series:tsd][shape:nested]") {
    const auto service = bg::service<auto(TS<std::int64_t>)->void>{"audit", {"value"}};
    auto request_sizes = std::vector<std::size_t>{};
    auto added_counts = std::vector<std::size_t>{};
    auto removed_counts = std::vector<std::size_t>{};

    const auto implement = node([&](In<TSD<service_client_id_t, TS<std::int64_t>>> requests) {
        request_sizes.push_back(requests.size());
        auto added = std::size_t{};
        requests.for_each_added([&](const auto &) { ++added; });
        added_counts.push_back(added);
        auto removed = std::size_t{};
        requests.for_each_removed([&](const auto &) { ++removed; });
        removed_counts.push_back(removed);
    });
    const auto implementation = graph(
        "name"_n = "audit_impl", [&](wp<TSD<service_client_id_t, TS<std::int64_t>>, "value"_n> values) {
            implement(values);
        });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    const auto values = node([](In<TS<bool>> present, Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
        if (*present) {
            output[5].set(7);
        } else {
            output.erase(5);
        }
    });
    const auto tested = [&](wp<TS<bool>> present) {
        const auto mapped = map(
            [&](auto value) {
                service(value);
                return value;
            },
            values(present));
        return mapped[std::int64_t{5}].as<TS<std::int64_t>>();
    };

    const auto actual = testing::test_graph(tested, catalogue, std::array{true, false, true});

    CHECK(actual.equals<std::int64_t>({7, std::nullopt, 7}));
    CHECK(request_sizes == std::vector<std::size_t>{1, 0, 1});
    CHECK(added_counts == std::vector<std::size_t>{1, 0, 1});
    CHECK(removed_counts == std::vector<std::size_t>{0, 1, 0});
}

TEST_CASE("service clients follow repeated switch changes after a map branch returns",
          "[feature:service][feature:map][feature:switch][feature:nested-graph]"
          "[series:tsd][shape:nested][transition:removal]") {
    const auto service = bg::service<auto(TS<std::int64_t>)->void>{"audit", {"value"}};
    auto request_sizes = std::vector<std::size_t>{};
    auto added_counts = std::vector<std::size_t>{};
    auto removed_counts = std::vector<std::size_t>{};

    const auto collect = node([&](In<TSD<service_client_id_t, TS<std::int64_t>>> requests) {
        request_sizes.push_back(requests.size());

        auto added = std::size_t{};
        requests.for_each_added([&](const auto &) { ++added; });
        added_counts.push_back(added);

        auto removed = std::size_t{};
        requests.for_each_removed([&](const auto &) { ++removed; });
        removed_counts.push_back(removed);
    });
    const auto implementation = graph(
        "name"_n = "audit_impl", [&](wp<TSD<service_client_id_t, TS<std::int64_t>>, "value"_n> values) {
            collect(values);
        });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto values = node([](In<TS<bool>> present, Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
        if (present.value()) {
            output[5].set(7);
        } else {
            output.erase(5);
        }
    });
    const auto add_offset = node([](In<TS<std::int64_t>> value, std::int64_t offset,
                                    Out<TS<std::int64_t>> output) { output.set(value.value() + offset); });
    const auto tested = [&](wp<TS<bool>> present, wp<TS<std::int64_t>> selected_case) {
        const auto mapped = map(
            [&](auto value, auto selector) {
                return switch_(
                           selector,
                           cases{
                               case_(1, [&](auto selected) {
                                   service(selected);
                                   return add_offset(selected, 100);
                               }),
                               case_(2, [&](auto selected) {
                                   service(selected);
                                   return add_offset(selected, 200);
                               }),
                           },
                           value)
                    .template as<TS<std::int64_t>>();
            },
            values(present), selected_case);
        return mapped[std::int64_t{5}].as<TS<std::int64_t>>();
    };

    const auto actual = testing::test_graph(tested, catalogue,
                                            std::array{true, true, true, false, true, true},
                                            std::array<std::int64_t, 6>{1, 2, 1, 1, 2, 1});

    CHECK(actual.equals<std::int64_t>({107, 207, 107, std::nullopt, 207, 107}));
    CHECK(request_sizes == std::vector<std::size_t>{1, 1, 1, 0, 1, 1});
    CHECK(added_counts == std::vector<std::size_t>{1, 1, 1, 0, 1, 1});
    CHECK(removed_counts == std::vector<std::size_t>{0, 1, 1, 1, 0, 1});
}

TEST_CASE("service rejects a response cycle through a client input",
          "[feature:service][feature:graph-compilation]") {
    const auto service = bg::service<auto(TS<std::int64_t>)->TS<std::int64_t>>{"identity", {"value"}};
    const auto implement = node([](In<TSD<service_client_id_t, TS<std::int64_t>>> requests,
                                   Out<TSD<service_client_id_t, TS<std::int64_t>>> responses) {
        requests.for_each_removed([&](const auto &client_id) { responses.erase(client_id); });
        requests.for_each_key_value(
            [&](const auto &client_id, const auto &value) { responses[client_id].set(*value); });
    });
    const auto implementation =
        graph("name"_n = "identity_impl", [&](wp<TSD<service_client_id_t, TS<std::int64_t>>, "value"_n> values) {
            return implement(values);
        });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto tested = [&](wp<TS<std::int64_t>> input) {
        const auto first = service(input).as<TS<std::int64_t>>();
        return service(first).as<TS<std::int64_t>>();
    };

    auto diagnostic = std::string{};
    try {
        (void)testing::test_graph(tested, catalogue, std::array<std::int64_t, 1>{1});
    } catch (const std::exception &error) {
        diagnostic = error.what();
    }

    CHECK(diagnostic.find("graph wiring contains a dependency cycle") != std::string::npos);
    INFO(diagnostic);
    CHECK(diagnostic.find("identity_impl[") != std::string::npos);
}
