#include "service_test_helpers.hpp"

using namespace bg;

TEST_CASE("one multiservice graph wires several reference service replies once",
          "[feature:service][feature:multiservice][series:value][shape:multiple]") {
    const auto left = reference_service<auto(i64)->TS<i64>>{"left", {"path"}};
    const auto right = reference_service<auto()->TS<i64>>{"right"};
    const auto source = node<ServiceOutputSource>();
    auto graph_wirings = std::size_t{};
    const auto implementation = graph("name"_n = "shared_services", [&] {
        ++graph_wirings;
        left.implementation_replies(source(), 1);
        right.implementation_replies(source());
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(left);
    registration.register_impl(right);

    const auto add = node([](i64 left, i64 right, i64 tick) { return left + right + tick; });
    const auto tested = [&](wp<TS<i64>> tick) { return add(left(1).as<TS<i64>>(), right().as<TS<i64>>(), tick); };

    const auto actual = testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2});

    CHECK(actual.equals<i64>({85, 86}));
    CHECK(graph_wirings == 1);
}

TEST_CASE("a multiservice graph receives subscription requests beside a reference reply",
          "[feature:service][feature:multiservice][series:tss][series:tsd][shape:multiple]") {
    const auto clock = reference_service<auto()->TS<i64, TimeSeriesGuarantee::final>>{"clock"};
    const auto lookup = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{"lookup", {"key"}};
    const auto source = node<FinalKeySource<i64>>();

    const auto respond = node([](In<TSS<i64>> requests, Out<TSD<i64, TS<i64>>> responses) {
        requests.for_each_removed([&](const auto &key) { responses.erase(key); });
        requests.for_each_added([&](const auto &key) { responses[key].set(*key + 1); });
    });

    const auto implementation = graph("name"_n = "shared_lookup", [&] {
        lookup.implementation_replies(respond(lookup.implementation_requests()));
        clock.implementation_replies(source(42));
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(clock);
    registration.register_impl(lookup);

    const auto add_tick = node([](i64 value, i64 tick) { return value + tick; });
    const auto tested = [&](wp<TS<i64>> tick) {
        return add_tick(lookup(clock().as<TS<i64, TimeSeriesGuarantee::final>>()).as<TS<i64>>(), tick);
    };

    const auto actual = testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {2, 3});

    CHECK(actual.equals<i64>({45, 46}));
}

TEST_CASE("multiservice service requests are addressable by declared parameter name",
          "[feature:service][feature:multiservice][series:tsd][shape:multiple]") {
    const auto combine = service<auto(TS<i64>, TS<float>)->TS<float>>{"combine", {"integer", "fraction"}};
    const auto respond =
        node([](In<TSD<service_client_id_t, TS<i64>>> integers, In<TSD<service_client_id_t, TS<float>>> fractions,
                Out<TSD<service_client_id_t, TS<float>>> responses) {
            integers.for_each_removed([&](const auto &client_id) { responses.erase(client_id); });
            integers.for_each_key_value([&](const auto &client_id, const auto &integer) {
                const auto fraction = fractions[client_id];
                if (integer.valid() && fraction.valid()) {
                    responses[client_id].set(static_cast<float>(*integer) + *fraction);
                }
            });
        });
    auto graph_wirings = std::size_t{};
    const auto implementation = graph("name"_n = "shared_service", [&] {
        ++graph_wirings;
        const auto requests = combine.implementation_requests();
        const auto integers = operators::getattr(requests, "integer"_sym);
        const auto fractions = operators::getattr(requests, "fraction"_sym);
        CHECK_THROWS_AS(operators::getattr(requests, "missing"_sym), WiringError);
        combine.implementation_replies(respond(integers, fractions));
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(combine);

    const auto tested = [&](wp<TS<i64>> integer, wp<TS<float>> fraction) {
        return combine(integer, fraction).as<TS<float>>();
    };

    const auto actual = testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {2, 5}, {1.5F, 3.5F});

    CHECK(actual.equals<float>({3.5F, 8.5F}));
    CHECK(graph_wirings == 1);
}

TEST_CASE("one multiservice graph handles subscription and service requests",
          "[feature:service][feature:multiservice][series:tss][series:tsd][shape:multiple]") {
    const auto lookup = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{"lookup", {"key"}};
    const auto transform = service<auto(TS<i64>)->TS<i64>>{"transform", {"value"}};

    const auto answer_lookup = node([](In<TSS<i64>> requests, Out<TSD<i64, TS<i64>>> replies) {
        requests.for_each_added([&](const auto &key) { replies[key].set(*key * 10); });
        requests.for_each_removed([&](const auto &key) { replies.erase(key); });
    });

    const auto answer_transform = node([](In<TSD<service_client_id_t, TS<i64>>> requests,
                                          Out<TSD<service_client_id_t, TS<i64>>> replies) {
        requests.for_each_removed([&](const auto &client_id) { replies.erase(client_id); });
        requests.for_each_key_value(
            [&](const auto &client_id, const auto &value) { replies[client_id].set(*value + 100); });
    });

    const auto implementation = graph("name"_n = "combined_services", [&] {
        lookup.implementation_replies(answer_lookup(lookup.implementation_requests()));
        const auto requests = transform.implementation_requests();
        transform.implementation_replies(answer_transform(operators::getattr(requests, "value"_sym)));
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(lookup);
    registration.register_impl(transform);

    const auto add = node([](i64 lookup_result, i64 transform_result) {
        return lookup_result + transform_result;
    });

    const auto key_source = node<FinalKeySource<i64>>();
    const auto tested = [&](wp<TS<i64>> value) {
        return add(lookup(key_source(1)).as<TS<i64>>(), transform(value).as<TS<i64>>());
    };

    CHECK(testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2, 3}).equals<i64>({111, 112, 113}));
}

TEST_CASE("multiservice outputless service publishes requests",
          "[feature:service][feature:multiservice][series:tsd][shape:sink]") {
    const auto audit = service<auto(TS<i64>)->void>{"audit", {"value"}};
    auto received = std::vector<i64>{};
    const auto observe = node([&](In<TSD<service_client_id_t, TS<i64>>> requests) {
        requests.for_each_key_value([&](const auto &, const auto &value) { received.push_back(*value); });
    });

    const auto implementation = graph("name"_n = "audit_service", [&] {
        const auto requests = audit.implementation_requests();
        observe(operators::getattr(requests, "value"_sym));
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(audit);

    const auto tested = [&](wp<TS<i64>> value) {
        audit(value);
        return value;
    };

    const auto actual = testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {4, 6, 8});

    CHECK(actual.equals<i64>({4, 6, 8}));
    CHECK(received == std::vector<i64>{4, 6, 8});
}

TEST_CASE("an unused multiservice service request port can be wired",
          "[feature:service][feature:multiservice][series:tsd][shape:sink]") {
    const auto clock = reference_service<auto()->TS<i64>>{"clock"};
    const auto audit = service<auto(TS<i64>)->void>{"audit", {"value"}};
    auto implementation_wirings = std::size_t{};
    const auto source = node<ServiceOutputSource>();

    const auto observe = node([](In<TSD<service_client_id_t, TS<i64>>>) {});

    const auto implementation = graph("name"_n = "clock_and_audit", [&] {
        ++implementation_wirings;
        const auto requests = audit.implementation_requests();
        observe(operators::getattr(requests, "value"_sym));
        clock.implementation_replies(source());
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(clock);
    registration.register_impl(audit);

    const auto add = node([](i64 value, i64 tick) { return value + tick; });
    const auto tested = [&](wp<TS<i64>> tick) { return add(clock().as<TS<i64>>(), tick); };

    CHECK(testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {0}).equals<i64>({42}));
    CHECK(implementation_wirings == 1);
}

TEST_CASE("multiservice request bindings cross map graph boundaries",
          "[feature:service][feature:multiservice][feature:map][feature:nested-graph]"
          "[series:tsd][shape:nested][shape:sink]") {
    const auto audit = service<auto(TS<i64>)->void>{"audit", {"value"}};
    auto request_sizes = std::vector<std::size_t>{};
    const auto collect = node([&](In<TSD<service_client_id_t, TS<i64>>> requests) {
        request_sizes.push_back(requests.size());
        requests.for_each_key_value([&](const auto &, const auto &value) { CHECK(value.valid()); });
    });

    const auto implementation = graph("name"_n = "map_audit", [&] {
        const auto requests = audit.implementation_requests();
        collect(operators::getattr(requests, "value"_sym));
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(audit);

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        const auto mapped = map(
            [&](auto item) {
                audit(item);
                return item;
            },
            values);
        return mapped[i64{1}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {{{1, 3}}, {{1, 8}}});

    CHECK(actual.equals<i64>({3, 8}));
    CHECK(request_sizes == std::vector<std::size_t>{1, 1});
}

TEST_CASE("multiservice claims select generic service signatures",
          "[feature:service][feature:multiservice][declaration:generic][shape:multiple]") {
    using RESULT = TimeSeriesVariable<"RESULT">;
    const auto identity = reference_service<RESULT, auto()->RESULT>{"identity"};
    struct FloatingServiceOutputSource {
        Out<TS<float>> output;

        void start() const { output.set(0.5F); }
        void evaluate() const noexcept {}
    };
    const auto integer_source = node<ServiceOutputSource>();

    const auto floating_source = node<FloatingServiceOutputSource>();

    const auto implementation = graph("name"_n = "typed_identities", [&] {
        identity[type<TS<i64>>].implementation_replies(integer_source());
        identity[type<TS<float>>].implementation_replies(floating_source());
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(identity[type<TS<i64>>]);
    registration.register_impl(identity[type<TS<float>>]);

    const auto add = node([](In<TS<i64>> integer, In<TS<float>> fraction, In<TS<i64>> tick, Out<TS<float>> output) {
        output.set(static_cast<float>(*integer) + *fraction + static_cast<float>(*tick));
    });
    const auto tested = [&](wp<TS<i64>> tick) {
        return add(identity[type<TS<i64>>]().as<TS<i64>>(), identity[type<TS<float>>]().as<TS<float>>(), tick);
    };

    CHECK(testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {0}).equals<float>({42.5F}));
}

TEST_CASE("multiservice claims separate service scalar values",
          "[feature:service][feature:multiservice][shape:multiple]") {
    const auto selected = reference_service<auto(i64)->TS<i64>>{"selected", {"path"}};
    struct FirstServiceOutputSource {
        Out<TS<i64>> output;

        void start() const { output.set(10); }
        void evaluate() const noexcept {}
    };
    struct SecondServiceOutputSource {
        Out<TS<i64>> output;

        void start() const { output.set(20); }
        void evaluate() const noexcept {}
    };
    const auto first_source = node<FirstServiceOutputSource>();

    const auto second_source = node<SecondServiceOutputSource>();

    const auto implementation = graph("name"_n = "selected_paths", [&] {
        selected.implementation_replies(first_source(), 1);
        selected.implementation_replies(second_source(), 2);
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(selected, "path"_n = 1);
    registration.register_impl(selected, "path"_n = 2);

    const auto add = node([](i64 first, i64 second, i64 tick) { return first + second + tick; });
    const auto tested = [&](wp<TS<i64>> tick) {
        return add(selected(1).as<TS<i64>>(), selected(2).as<TS<i64>>(), tick);
    };

    CHECK(testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {1}).equals<i64>({31}));
}

TEST_CASE("multiservice catch-all forwards requests and replies while exact claims take precedence",
          "[feature:service][feature:multiservice][series:tsd][shape:multiple][scalar:integer]") {
    const auto routed = service<auto(TS<i64>, i64)->TS<i64>>{"routed", {"value", "instance"}};
    auto catch_all_sizes = std::vector<std::size_t>{};
    const auto answer_catch_all = node([&](In<TSD<service_client_id_t, TS<i64>>> values,
                                           In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> instances,
                                           Out<TSD<service_client_id_t, TS<i64>>> replies) {
        catch_all_sizes.push_back(values.size());
        values.for_each_removed([&](const auto &client_id) { replies.erase(client_id); });
        values.for_each_key_value([&](const auto &client_id, const auto &value) {
            const auto instance = instances[client_id];
            if (value.valid() && instance.valid()) {
                replies[client_id].set(*value + *instance);
            }
        });
    });

    const auto answer_exact = node([](In<TSD<service_client_id_t, TS<i64>>> values,
                                      Out<TSD<service_client_id_t, TS<i64>>> replies) {
        values.for_each_removed([&](const auto &client_id) { replies.erase(client_id); });
        values.for_each_key_value([&](const auto &client_id, const auto &value) {
            if (value.valid()) {
                replies[client_id].set(*value + 100);
            }
        });
    });

    const auto implementation = graph("name"_n = "routed_multiservice", [&] {
        const auto requests = routed.implementation_requests_for_catch_all("instance"_sym);
        routed.implementation_replies_for_catch_all(
            answer_catch_all(operators::getattr(requests, "value"_sym),
                             operators::getattr(requests, "instance"_sym)),
            "instance"_sym);

        const auto exact_requests = routed.implementation_requests(2);
        routed.implementation_replies(answer_exact(operators::getattr(exact_requests, "value"_sym)), 2);
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(routed, "instance"_n = 2);
    registration.register_catch_all(routed, "instance"_sym);

    const auto sum = node([](In<TS<i64>> first, In<TS<i64>> second, In<TS<i64>> third, Out<TS<i64>> output) {
        output.set(*first + *second + *third);
    });
    const auto tested = [&](wp<TS<i64>> value) {
        return sum(routed(value, 1).as<TS<i64>>(), routed(value, 2).as<TS<i64>>(), routed(value, 3).as<TS<i64>>());
    };

    const auto actual = testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {4, 6});
    CHECK(actual.equals<i64>({116, 122}));
    CHECK(catch_all_sizes == std::vector<std::size_t>{2, 2});
}

TEST_CASE("multiservice catch-all claims filter unlifted scalar arguments",
          "[feature:service][feature:multiservice][series:tsd][shape:multiple][scalar:integer]") {
    const auto routed = service<auto(TS<i64>, i64, i64)->TS<i64>>{"filtered", {"value", "group", "instance"}};
    const auto answer_catch_all =
        node([](In<TSD<service_client_id_t, TS<i64>>> values,
                In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> instances,
                Out<TSD<service_client_id_t, TS<i64>>> replies) {
            values.for_each_removed([&](const auto &client_id) { replies.erase(client_id); });
            values.for_each_key_value([&](const auto &client_id, const auto &value) {
                const auto instance = instances[client_id];
                if (value.valid() && instance.valid()) {
                    replies[client_id].set(*value + *instance + 1);
                }
            });
        });
    const auto answer_exact = node([](In<TSD<service_client_id_t, TS<i64>>> values,
                                      Out<TSD<service_client_id_t, TS<i64>>> replies) {
        values.for_each_removed([&](const auto &client_id) { replies.erase(client_id); });
        values.for_each_key_value([&](const auto &client_id, const auto &value) {
            if (value.valid()) {
                replies[client_id].set(*value + 100);
            }
        });
    });

    const auto implementation = graph("name"_n = "filtered_multiservice", [&] {
        const auto requests = routed.implementation_requests_for_catch_all("instance"_sym, "group"_n = 1);
        routed.implementation_replies_for_catch_all(
            answer_catch_all(operators::getattr(requests, "value"_sym),
                             operators::getattr(requests, "instance"_sym)),
            "instance"_sym, "group"_n = 1);

        const auto exact_requests = routed.implementation_requests(2, 8);
        routed.implementation_replies(answer_exact(operators::getattr(exact_requests, "value"_sym)), 2, 8);
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(routed, "group"_n = 2);
    registration.register_catch_all(routed, "instance"_sym, "group"_n = 1);

    const auto sum = node([](In<TS<i64>> first, In<TS<i64>> second, In<TS<i64>> third, Out<TS<i64>> output) {
        output.set(*first + *second + *third);
    });
    const auto tested = [&](wp<TS<i64>> value) {
        return sum(routed(value, 1, 3).as<TS<i64>>(), routed(value, 1, 4).as<TS<i64>>(),
                   routed(value, 2, 8).as<TS<i64>>());
    };

    CHECK(testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {5}).equals<i64>({124}));
}

TEST_CASE("overlapping multiservice catch-all claims are ambiguous",
          "[feature:service][feature:multiservice][shape:multiple][scalar:integer]") {
    const auto routed = service<auto(TS<i64>, i64, i64)->TS<i64>>{"overlap", {"value", "group", "instance"}};
    const auto implementation = graph("name"_n = "overlapping_catch_all", [&] {
        const auto requests = routed.implementation_requests_for_catch_all("instance"_sym, "group"_n = 1);
        routed.implementation_replies_for_catch_all(operators::getattr(requests, "value"_sym), "instance"_sym,
                                                    "group"_n = 1);
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_catch_all(routed, "instance"_sym);
    registration.register_catch_all(routed, "instance"_sym, "group"_n = 1);

    const auto tested = [&](wp<TS<i64>> value) { return routed(value, 1, 7).as<TS<i64>>(); };
    CHECK_THROWS_WITH(testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {5}),
                      Catch::Matchers::ContainsSubstring("ambiguous implementations"));
}

TEST_CASE("an exact multiservice claim ignores overlapping unused catch-all claims",
          "[feature:service][feature:multiservice][series:tsd][shape:multiple][scalar:integer]") {
    const auto routed = service<auto(TS<i64>, i64, i64)->TS<i64>>{"exact_first", {"value", "group", "instance"}};
    const auto answer_exact = node([](In<TSD<service_client_id_t, TS<i64>>> values,
                                      Out<TSD<service_client_id_t, TS<i64>>> replies) {
        values.for_each_key_value([&](const auto &client_id, const auto &value) {
            if (value.valid()) {
                replies[client_id].set(*value + 100);
            }
        });
    });

    const auto implementation = graph("name"_n = "exact_before_catch_all", [&] {
        const auto catch_all_requests =
            routed.implementation_requests_for_catch_all("instance"_sym, "group"_n = 1);
        routed.implementation_replies_for_catch_all(
            answer_exact(operators::getattr(catch_all_requests, "value"_sym)), "instance"_sym,
            "group"_n = 1);

        const auto exact_requests = routed.implementation_requests(2, 8);
        routed.implementation_replies(answer_exact(operators::getattr(exact_requests, "value"_sym)), 2, 8);
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(routed, "group"_n = 2);
    registration.register_catch_all(routed, "instance"_sym);
    registration.register_catch_all(routed, "instance"_sym, "group"_n = 1);

    const auto tested = [&](wp<TS<i64>> value) { return routed(value, 2, 8).as<TS<i64>>(); };
    CHECK(testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {5}).equals<i64>({105}));
}

TEST_CASE("regular exact implementations take precedence over multiservice catch-all claims",
          "[feature:service][feature:multiservice][series:tsd][shape:multiple][scalar:integer]") {
    const auto routed = service<auto(TS<i64>, i64)->TS<i64>>{"mixed_claims", {"value", "instance"}};
    const auto answer_exact = node([](In<TSD<service_client_id_t, TS<i64>>> values, i64 instance,
                                      Out<TSD<service_client_id_t, TS<i64>>> replies) {
        values.for_each_key_value([&](const auto &client_id, const auto &value) {
            if (value.valid()) {
                replies[client_id].set(*value + instance + 100);
            }
        });
    });

    const auto exact_implementation =
        graph("name"_n = "mixed_claims_exact", [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values,
                                                   i64 instance) { return answer_exact(values, instance); });

    const auto answer_catch_all = node([](In<TSD<service_client_id_t, TS<i64>>> values,
                                          In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> instances,
                                          Out<TSD<service_client_id_t, TS<i64>>> replies) {
        values.for_each_key_value([&](const auto &client_id, const auto &value) {
            const auto instance = instances[client_id];
            if (value.valid() && instance.valid()) {
                replies[client_id].set(*value + *instance);
            }
        });
    });

    const auto catch_all_implementation = graph("name"_n = "mixed_claims_catch_all", [&] {
        const auto requests = routed.implementation_requests_for_catch_all("instance"_sym);
        routed.implementation_replies_for_catch_all(
            answer_catch_all(operators::getattr(requests, "value"_sym),
                             operators::getattr(requests, "instance"_sym)),
            "instance"_sym);
    });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(routed, exact_implementation, "instance"_n = 1);
    auto registration = catalogue.register_multiservice(catch_all_implementation);
    registration.register_catch_all(routed, "instance"_sym);

    const auto sum = node([](In<TS<i64>> exact, In<TS<i64>> second, In<TS<i64>> third, Out<TS<i64>> output) {
        output.set(*exact + *second + *third);
    });
    const auto tested = [&](wp<TS<i64>> value) {
        return sum(routed(value, 1).as<TS<i64>>(), routed(value, 2).as<TS<i64>>(), routed(value, 3).as<TS<i64>>());
    };

    const auto actual = testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {4, 6});
    CHECK(actual.equals<i64>({118, 124}));
}

TEST_CASE("multiservice catch-all ports select a resolved generic service signature",
          "[feature:service][feature:multiservice][series:tsd][declaration:generic][shape:multiple]") {
    using VALUE = ScalarVariable<"VALUE">;
    const auto routed = service<VALUE, auto(TS<VALUE>, i64)->TS<VALUE>>{"generic_route", {"value", "instance"}};
    const auto answer_integers = node([](In<TSD<service_client_id_t, TS<i64>>> values,
                                         In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> instances,
                                         Out<TSD<service_client_id_t, TS<i64>>> replies) {
        values.for_each_key_value([&](const auto &client_id, const auto &value) {
            const auto instance = instances[client_id];
            if (value.valid() && instance.valid()) {
                replies[client_id].set(*value + *instance);
            }
        });
    });

    const auto answer_fractions = node([](In<TSD<service_client_id_t, TS<float>>> values,
                                          In<TSD<service_client_id_t, TS<i64, TimeSeriesGuarantee::final>>> instances,
                                          Out<TSD<service_client_id_t, TS<float>>> replies) {
        values.for_each_key_value([&](const auto &client_id, const auto &value) {
            const auto instance = instances[client_id];
            if (value.valid() && instance.valid()) {
                replies[client_id].set(*value + static_cast<float>(*instance));
            }
        });
    });

    const auto implementation = graph("name"_n = "generic_catch_all", [&] {
        const auto integer_requests =
            routed[type<VALUE> = type<i64>].implementation_requests_for_catch_all("instance"_sym);
        routed[type<VALUE> = type<i64>].implementation_replies_for_catch_all(
            answer_integers(operators::getattr(integer_requests, "value"_sym),
                            operators::getattr(integer_requests, "instance"_sym)),
            "instance"_sym);

        const auto fraction_requests =
            routed[type<VALUE> = type<float>].implementation_requests_for_catch_all("instance"_sym);
        routed[type<VALUE> = type<float>].implementation_replies_for_catch_all(
            answer_fractions(operators::getattr(fraction_requests, "value"_sym),
                             operators::getattr(fraction_requests, "instance"_sym)),
            "instance"_sym);
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_catch_all(routed[type<VALUE> = type<i64>], "instance"_sym);
    registration.register_catch_all(routed[type<VALUE> = type<float>], "instance"_sym);

    const auto add = node([](In<TS<i64>> integer, In<TS<float>> fraction, Out<TS<float>> output) {
        output.set(static_cast<float>(*integer) + *fraction);
    });
    const auto tested = [&](wp<TS<i64>> integer, wp<TS<float>> fraction) {
        return add(routed[type<VALUE> = type<i64>](integer, 1).as<TS<i64>>(),
                   routed[type<VALUE> = type<float>](fraction, 2).as<TS<float>>());
    };

    const auto actual = testing::test_graph(tested, GraphRunOptions{.services = &catalogue}, {3, 7}, {0.5F, 1.5F});
    CHECK(actual.equals<float>({6.5F, 11.5F}));
}
