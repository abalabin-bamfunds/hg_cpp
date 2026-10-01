#include "service_test_helpers.hpp"

TEST_CASE("one multiservice graph wires several reference service replies once",
          "[feature:service][feature:multiservice][series:value][shape:multiple]") {
    const auto left = reference_service<auto(std::int64_t)->TS<std::int64_t>>{"left", {"path"}};
    const auto right = reference_service<auto()->TS<std::int64_t>>{"right"};
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

    const auto add = node([](In<TS<std::int64_t>> left, In<TS<std::int64_t>> right, In<TS<std::int64_t>> tick,
                             Out<TS<std::int64_t>> output) { output.set(*left + *right + *tick); });
    const auto tested = [&](wp<TS<std::int64_t>> tick) {
        return add(left(1).as<TS<std::int64_t>>(), right().as<TS<std::int64_t>>(), tick);
    };

    const auto actual = testing::test_graph(tested, catalogue, std::array<std::int64_t, 2>{1, 2});

    CHECK(actual.equals<std::int64_t>({85, 86}));
    CHECK(graph_wirings == 1);
}

TEST_CASE("a multiservice graph receives subscription requests beside a reference reply",
          "[feature:service][feature:multiservice][series:tss][series:tsd][shape:multiple]") {
    const auto clock = reference_service<auto()->TS<std::int64_t, TimeSeriesGuarantee::final>>{"clock"};
    const auto lookup = subscription_service<auto(TS<std::int64_t, TimeSeriesGuarantee::final>)->TS<std::int64_t>>{
        "lookup", {"key"}};
    const auto source = node<FinalKeySource<std::int64_t>>();
    const auto respond = node([](In<TSS<std::int64_t>> requests, Out<TSD<std::int64_t, TS<std::int64_t>>> responses) {
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

    const auto add_tick = node([](In<TS<std::int64_t>> value, In<TS<std::int64_t>> tick, Out<TS<std::int64_t>> output) {
        output.set(*value + *tick);
    });
    const auto tested = [&](wp<TS<std::int64_t>> tick) {
        return add_tick(lookup(clock().as<TS<std::int64_t, TimeSeriesGuarantee::final>>()).as<TS<std::int64_t>>(),
                        tick);
    };

    const auto actual = testing::test_graph(tested, catalogue, std::array<std::int64_t, 2>{2, 3});

    CHECK(actual.equals<std::int64_t>({45, 46}));
}

TEST_CASE("multiservice service requests are addressable by declared parameter name",
          "[feature:service][feature:multiservice][series:tsd][shape:multiple]") {
    const auto combine =
        service<auto(TS<std::int64_t>, TS<float>)->TS<float>>{"combine", {"integer", "fraction"}};
    const auto respond =
        node([](In<TSD<service_client_id_t, TS<std::int64_t>>> integers,
                In<TSD<service_client_id_t, TS<float>>> fractions, Out<TSD<service_client_id_t, TS<float>>> responses) {
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

    const auto tested = [&](wp<TS<std::int64_t>> integer, wp<TS<float>> fraction) {
        return combine(integer, fraction).as<TS<float>>();
    };

    const auto actual =
        testing::test_graph(tested, catalogue, std::array<std::int64_t, 2>{2, 5}, std::array<float, 2>{1.5F, 3.5F});

    CHECK(actual.equals<float>({3.5F, 8.5F}));
    CHECK(graph_wirings == 1);
}

TEST_CASE("one multiservice graph handles subscription and service requests",
          "[feature:service][feature:multiservice][series:tss][series:tsd][shape:multiple]") {
    const auto lookup = subscription_service<auto(TS<std::int64_t, TimeSeriesGuarantee::final>)->TS<std::int64_t>>{
        "lookup", {"key"}};
    const auto transform = service<auto(TS<std::int64_t>)->TS<std::int64_t>>{"transform", {"value"}};

    const auto answer_lookup =
        node([](In<TSS<std::int64_t>> requests, Out<TSD<std::int64_t, TS<std::int64_t>>> replies) {
            requests.for_each_added([&](const auto &key) { replies[key].set(*key * 10); });
            requests.for_each_removed([&](const auto &key) { replies.erase(key); });
        });
    const auto answer_transform = node([](In<TSD<service_client_id_t, TS<std::int64_t>>> requests,
                                          Out<TSD<service_client_id_t, TS<std::int64_t>>> replies) {
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

    const auto add = node([](In<TS<std::int64_t>> lookup_result, In<TS<std::int64_t>> transform_result,
                             Out<TS<std::int64_t>> output) { output.set(*lookup_result + *transform_result); });
    const auto key_source = node<FinalKeySource<std::int64_t>>();
    const auto tested = [&](wp<TS<std::int64_t>> value) {
        return add(lookup(key_source(1)).as<TS<std::int64_t>>(), transform(value).as<TS<std::int64_t>>());
    };

    CHECK(testing::test_graph(tested, catalogue, std::array<std::int64_t, 3>{1, 2, 3})
              .equals<std::int64_t>({111, 112, 113}));
}

TEST_CASE("multiservice outputless service publishes requests",
          "[feature:service][feature:multiservice][series:tsd][shape:sink]") {
    const auto audit = service<auto(TS<std::int64_t>)->void>{"audit", {"value"}};
    auto received = std::vector<std::int64_t>{};
    const auto observe = node([&](In<TSD<service_client_id_t, TS<std::int64_t>>> requests) {
        requests.for_each_key_value([&](const auto &, const auto &value) { received.push_back(*value); });
    });
    const auto implementation = graph("name"_n = "audit_service", [&] {
        const auto requests = audit.implementation_requests();
        observe(operators::getattr(requests, "value"_sym));
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(audit);

    const auto tested = [&](wp<TS<std::int64_t>> value) {
        audit(value);
        return value;
    };

    const auto actual = testing::test_graph(tested, catalogue, std::array<std::int64_t, 3>{4, 6, 8});

    CHECK(actual.equals<std::int64_t>({4, 6, 8}));
    CHECK(received == std::vector<std::int64_t>{4, 6, 8});
}

TEST_CASE("an unused multiservice service request port can be wired",
          "[feature:service][feature:multiservice][series:tsd][shape:sink]") {
    const auto clock = reference_service<auto()->TS<std::int64_t>>{"clock"};
    const auto audit = service<auto(TS<std::int64_t>)->void>{"audit", {"value"}};
    auto implementation_wirings = std::size_t{};
    const auto source = node<ServiceOutputSource>();
    const auto observe = node([](In<TSD<service_client_id_t, TS<std::int64_t>>>) {});
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

    const auto add = node([](In<TS<std::int64_t>> value, In<TS<std::int64_t>> tick, Out<TS<std::int64_t>> output) {
        output.set(*value + *tick);
    });
    const auto tested = [&](wp<TS<std::int64_t>> tick) { return add(clock().as<TS<std::int64_t>>(), tick); };

    CHECK(testing::test_graph(tested, catalogue, std::array<std::int64_t, 1>{0}).equals<std::int64_t>({42}));
    CHECK(implementation_wirings == 1);
}

TEST_CASE("multiservice request bindings cross map graph boundaries",
          "[feature:service][feature:multiservice][feature:map][feature:nested-graph]"
          "[series:tsd][shape:nested][shape:sink]") {
    const auto audit = service<auto(TS<std::int64_t>)->void>{"audit", {"value"}};
    auto request_sizes = std::vector<std::size_t>{};
    const auto collect = node([&](In<TSD<service_client_id_t, TS<std::int64_t>>> requests) {
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

    const auto values = node(
        [](In<TS<std::int64_t>> value, Out<TSD<std::int64_t, TS<std::int64_t>>> output) { output[1].set(*value); });
    const auto tested = [&](wp<TS<std::int64_t>> value) {
        const auto mapped = map(
            [&](auto item) {
                audit(item);
                return item;
            },
            values(value));
        return mapped[std::int64_t{1}].as<TS<std::int64_t>>();
    };

    const auto actual = testing::test_graph(tested, catalogue, std::array<std::int64_t, 2>{3, 8});

    CHECK(actual.equals<std::int64_t>({3, 8}));
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
        identity[type<TS<std::int64_t>>].implementation_replies(integer_source());
        identity[type<TS<float>>].implementation_replies(floating_source());
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(identity[type<TS<std::int64_t>>]);
    registration.register_impl(identity[type<TS<float>>]);

    const auto add = node(
        [](In<TS<std::int64_t>> integer, In<TS<float>> fraction, In<TS<std::int64_t>> tick, Out<TS<float>> output) {
            output.set(static_cast<float>(*integer) + *fraction + static_cast<float>(*tick));
        });
    const auto tested = [&](wp<TS<std::int64_t>> tick) {
        return add(identity[type<TS<std::int64_t>>]().as<TS<std::int64_t>>(),
                   identity[type<TS<float>>]().as<TS<float>>(), tick);
    };

    CHECK(testing::test_graph(tested, catalogue, std::array<std::int64_t, 1>{0}).equals<float>({42.5F}));
}

TEST_CASE("multiservice claims separate service scalar values",
          "[feature:service][feature:multiservice][shape:multiple]") {
    const auto selected = reference_service<auto(std::int64_t)->TS<std::int64_t>>{"selected", {"path"}};
    struct FirstServiceOutputSource {
        Out<TS<std::int64_t>> output;

        void start() const { output.set(10); }
        void evaluate() const noexcept {}
    };
    struct SecondServiceOutputSource {
        Out<TS<std::int64_t>> output;

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

    const auto add = node([](In<TS<std::int64_t>> first, In<TS<std::int64_t>> second, In<TS<std::int64_t>> tick,
                             Out<TS<std::int64_t>> output) { output.set(*first + *second + *tick); });
    const auto tested = [&](wp<TS<std::int64_t>> tick) {
        return add(selected(1).as<TS<std::int64_t>>(), selected(2).as<TS<std::int64_t>>(), tick);
    };

    CHECK(testing::test_graph(tested, catalogue, std::array<std::int64_t, 1>{1}).equals<std::int64_t>({31}));
}
