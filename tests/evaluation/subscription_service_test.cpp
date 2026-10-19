#include "service_test_helpers.hpp"

#include <bg/service_feedback.hpp>
#include <bg/testing/test_wiring.hpp>

TEST_CASE("subscription services deduplicate requests and return values by key",
          "[feature:service][series:tss][series:tsd]") {
    const auto service = subscription_service<auto(TS<bg::i64, TimeSeriesGuarantee::final>)->TS<float>>{
        "prices", {"key"}};

    struct RequestLog {
        std::vector<std::size_t> sizes;
        std::vector<std::size_t> added;
        std::vector<std::size_t> removed;
    };
    auto log = RequestLog{};
    const auto respond = node([&](In<TSS<bg::i64>> requests, Out<TSD<bg::i64, TS<float>>> responses) {
        log.sizes.push_back(requests.size());

        auto added_count = std::size_t{};
        requests.for_each_added([&](const auto &key) {
            ++added_count;
            responses[key].set(static_cast<float>(*key));
        });
        log.added.push_back(added_count);

        auto removed_count = std::size_t{};
        requests.for_each_removed([&](const auto &key) {
            ++removed_count;
            responses.erase(key);
        });
        log.removed.push_back(removed_count);
    });
    const auto implementation =
        graph("name"_n = "price_lookup", [&](wp<TSS<bg::i64>> requests) { return respond(requests); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto key_source = node<FinalKeySource<bg::i64>>();
    const auto tested = [&](wp<TS<bg::i64>> tick) {
        (void)tick;
        const auto key = key_source(7);
        (void)service(key);
        return service(key).as<TS<float>>();
    };
    const auto actual = testing::test_graph(tested, bg::GraphRunOptions{.services = &catalogue}, std::array<bg::i64, 1>{0});

    CHECK(actual.equals<float>({7.0F}));
    CHECK(log.sizes == std::vector<std::size_t>{1});
    CHECK(log.added == std::vector<std::size_t>{1});
    CHECK(log.removed == std::vector<std::size_t>{0});
}

TEST_CASE("feedback subscription wrapper delays requests into its nested implementation",
          "[feature:service][feature:feedback][feature:nested-graph][series:tss][series:tsd][shape:nested]") {
    const auto service = subscription_service<auto(TS<bg::i64, TimeSeriesGuarantee::final>)->TS<bg::i64>>{
        "delayed_lookup", {"key"}};
    const auto respond = node([](In<TSS<bg::i64>> requests, Out<TSD<bg::i64, TS<bg::i64>>> output) {
        requests.for_each_removed([&](const auto &key) { output.erase(key); });
        requests.for_each_added([&](const auto &key) { output[key].set(*key * 10); });
    });
    const auto implementation = graph("name"_n = "delayed_lookup_impl",
                                      [&](wp<TSS<bg::i64>> requests) { return respond(requests); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, feedback_service_implementation(service, implementation));

    const auto key_source = node<FinalKeySource<bg::i64>>();
    const auto tested = [&](wp<TS<bg::i64>> tick) {
        (void)tick;
        return service(key_source(3)).as<TS<bg::i64>>();
    };
    const auto actual = testing::test_graph(
        tested, bg::GraphRunOptions{.services = &catalogue}, std::array<bg::i64, 1>{0});
    CHECK(actual.equals<bg::i64>({std::nullopt, 30}));
}

TEST_CASE("subscription services reject a replaceable reference to a final key",
          "[feature:service][feature:wiring][series:tss][series:tsd][guarantee:final]"
          "[binding:reference-to-value]") {
    const auto service = subscription_service<auto(TS<bg::i64, TimeSeriesGuarantee::final>)->TS<float>>{
        "prices", {"key"}};
    const auto keys = node([](Out<TSD<bg::i64, TS<bg::i64, TimeSeriesGuarantee::final>>> output) {
        output[7].set(7);
    });
    const auto selector = node<FinalKeySource<bg::i64>>();

    testing::test_wiring([&] {
        const auto key = keys()[selector(7)];
        CHECK_THROWS_AS(service(key), TypeResolutionError);
    });
}

TEST_CASE("subscription service clients inside map branches update one root request dictionary",
          "[feature:service][feature:map][feature:nested-graph][series:tss][series:tsd]"
          "[shape:nested]") {
    const auto service = subscription_service<auto(TS<bg::i64, TimeSeriesGuarantee::final>)->TS<float>>{
        "lookup", {"key"}};
    struct RequestLog {
        std::vector<std::size_t> sizes;
        std::vector<std::size_t> added;
        std::vector<std::size_t> removed;
    };
    auto log = RequestLog{};
    const auto collect = node([&](In<TSS<bg::i64>> requests) {
        log.sizes.push_back(requests.size());

        auto added_count = std::size_t{};
        requests.for_each_added([&](const auto &) { ++added_count; });
        log.added.push_back(added_count);

        auto removed_count = std::size_t{};
        requests.for_each_removed([&](const auto &) { ++removed_count; });
        log.removed.push_back(removed_count);
    });
    const auto responses = node([](Out<TSD<bg::i64, TS<float>>> output) {
        output[4].set(40.0F);
        output[9].set(90.0F);
    });
    const auto implementation = graph("name"_n = "lookup_impl", [&](wp<TSS<bg::i64>> requests) {
        collect(requests);
        return responses();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    const auto tested = [&](wp<TSD<bg::i64, TS<bg::i64, TimeSeriesGuarantee::final>>> values) {
        const auto mapped = map(
            [&](auto key, auto item) {
                (void)service(key);
                return item;
            },
            values, "__key_arg__"_n = 0);
        return mapped[bg::i64{1}].as<TS<bg::i64>>();
    };

    const auto actual = testing::test_graph(tested, bg::GraphRunOptions{.services = &catalogue}, {{{1, 4}, {2, 9}}});

    CHECK(actual.equals<bg::i64>({4}));
    CHECK(log.sizes == std::vector<std::size_t>{2});
    CHECK(log.added == std::vector<std::size_t>{2});
    CHECK(log.removed == std::vector<std::size_t>{0});
}

TEST_CASE("subscription service request bindings cross nested graph boundaries",
          "[feature:service][feature:nested-graph][feature:graph-compilation]"
          "[series:tss][series:tsd][shape:nested]") {
    const auto service = subscription_service<auto(TS<bg::i64, TimeSeriesGuarantee::final>)->TS<float>>{
        "lookup", {"key"}};
    struct RequestLog {
        std::vector<std::size_t> sizes;
        std::vector<std::size_t> added;
    };
    auto log = RequestLog{};
    const auto respond = node([](In<TSS<bg::i64>> requests, Out<TSD<bg::i64, TS<float>>> responses) {
        requests.for_each_added([&](const auto &key) { responses[key].set(static_cast<float>(*key)); });
        requests.for_each_removed([&](const auto &key) { responses.erase(key); });
    });
    const auto collect = node([&](In<TSS<bg::i64>> requests) {
        log.sizes.push_back(requests.size());
        auto count = std::size_t{};
        requests.for_each_added([&](const auto &) { ++count; });
        log.added.push_back(count);
    });
    const auto implementation_with_log = graph("name"_n = "logged_lookup_impl", [&](wp<TSS<bg::i64>> requests) {
        collect(requests);
        return respond(requests);
    });
    auto logged_catalogue = ServiceCatalogue{};
    logged_catalogue.register_impl(service, implementation_with_log);

    const auto inner = graph("name"_n = "nested_lookup", [&](wp<TS<bg::i64, TimeSeriesGuarantee::final>> key) {
        (void)service(key);
        return key;
    });
    const auto key_source = node<FinalKeySource<bg::i64>>();
    const auto tested = [&](wp<TS<bg::i64>> tick) {
        (void)tick;
        return inner(key_source(3));
    };

    CHECK(testing::test_graph(tested, bg::GraphRunOptions{.services = &logged_catalogue}, std::array<bg::i64, 1>{0})
              .equals<bg::i64>({3}));
    CHECK(log.sizes == std::vector<std::size_t>{1});
    CHECK(log.added == std::vector<std::size_t>{1});
}

TEST_CASE("subscription service clients inside mesh branches update one root request dictionary",
          "[feature:service][feature:mesh][feature:nested-graph][series:tss][series:tsd]"
          "[shape:nested]") {
    const auto service = subscription_service<auto(TS<bg::i64, TimeSeriesGuarantee::final>)->TS<float>>{
        "lookup", {"key"}};
    struct RequestLog {
        std::vector<std::size_t> sizes;
        std::vector<std::size_t> added;
    };
    auto log = RequestLog{};
    const auto collect = node([&](In<TSS<bg::i64>> requests) {
        log.sizes.push_back(requests.size());
        auto count = std::size_t{};
        requests.for_each_added([&](const auto &) { ++count; });
        log.added.push_back(count);
    });
    const auto responses = node([](Out<TSD<bg::i64, TS<float>>>) {});
    const auto implementation = graph("name"_n = "lookup_impl", [&](wp<TSS<bg::i64>> requests) {
        collect(requests);
        return responses();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    const auto tested = [&](wp<TSD<bg::i64, TS<bg::i64, TimeSeriesGuarantee::final>>> values) {
        const auto mapped = mesh(
            [&](auto key, auto item) {
                (void)service(key);
                return item;
            },
            values, "__key_arg__"_n = 0);
        return mapped[bg::i64{1}].as<TS<bg::i64>>();
    };

    const auto actual = testing::test_graph(tested, bg::GraphRunOptions{.services = &catalogue}, {{{1, 4}, {2, 9}}});

    CHECK(actual.equals<bg::i64>({4}));
    CHECK(log.sizes == std::vector<std::size_t>{2});
    CHECK(log.added == std::vector<std::size_t>{2});
}

TEST_CASE("subscription service multiplicity includes scalar parameters",
          "[feature:service][series:tss][series:tsd][shape:multiple]") {
    const auto service =
        subscription_service<auto(TS<bg::i64, TimeSeriesGuarantee::final>, bg::i64)->TS<float>>{
            "lookup", {"key", "path"}};
    const auto respond =
        node([](In<TSS<bg::i64>> requests, bg::i64 path, Out<TSD<bg::i64, TS<float>>> responses) {
            requests.for_each_added([&](const auto &key) { responses[key].set(static_cast<float>(*key + path)); });
            requests.for_each_removed([&](const auto &key) { responses.erase(key); });
        });
    const auto implementation = graph("name"_n = "lookup_impl", [&](wp<TSS<bg::i64>> requests, bg::i64 path) {
        return respond(requests, path);
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation, "path"_n = 1);
    catalogue.register_impl(service, implementation, "path"_n = 2);

    const auto sum =
        node([](In<TS<float>> left, In<TS<float>> right, Out<TS<float>> output) { output.set(*left + *right); });
    const auto key_source = node<FinalKeySource<bg::i64>>();
    const auto tested = [&](wp<TS<bg::i64>> tick) {
        (void)tick;
        const auto key = key_source(3);
        const auto first = service(key, 1).as<TS<float>>();
        const auto second = service(key, 2).as<TS<float>>();
        return sum(first, second);
    };

    CHECK(testing::test_graph(tested, bg::GraphRunOptions{.services = &catalogue}, std::array<bg::i64, 1>{0}).equals<float>({9.0F}));
}

TEST_CASE("removing a map branch removes its subscription request",
          "[feature:service][feature:map][feature:nested-graph][series:tss][series:tsd]"
          "[transition:removal]") {
    const auto service = subscription_service<auto(TS<bg::i64, TimeSeriesGuarantee::final>)->TS<float>>{
        "lookup", {"key"}};
    struct RequestLog {
        std::vector<std::size_t> sizes;
        std::vector<std::size_t> added;
        std::vector<std::size_t> removed;
    };
    auto log = RequestLog{};
    const auto collect = node([&](In<TSS<bg::i64>> requests) {
        log.sizes.push_back(requests.size());
        auto added_count = std::size_t{};
        requests.for_each_added([&](const auto &) { ++added_count; });
        log.added.push_back(added_count);
        auto removed_count = std::size_t{};
        requests.for_each_removed([&](const auto &) { ++removed_count; });
        log.removed.push_back(removed_count);
    });
    const auto responses = node([](Out<TSD<bg::i64, TS<float>>>) {});
    const auto implementation = graph("name"_n = "lookup_impl", [&](wp<TSS<bg::i64>> requests) {
        collect(requests);
        return responses();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto tested = [&](wp<TSD<bg::i64, TS<bg::i64, TimeSeriesGuarantee::final>>> values) {
        const auto mapped = map(
            [&](auto key, auto item) {
                (void)service(key);

                return item;
            },
            values, "__key_arg__"_n = 0);
        return mapped[bg::i64{5}].as<TS<bg::i64>>();
    };

    const auto actual = testing::test_graph(tested, bg::GraphRunOptions{.services = &catalogue},
                                            {{{5, 7}}, {{5, literal::REMOVE}}});

    CHECK(actual.equals<bg::i64>({7, std::nullopt}));
    CHECK(log.sizes == std::vector<std::size_t>{1, 0});
    CHECK(log.added == std::vector<std::size_t>{1, 0});
    CHECK(log.removed == std::vector<std::size_t>{0, 1});
}

TEST_CASE("response-producing subscription clients inside a map reject dependency cycles",
          "[feature:service][feature:map][feature:nested-graph][feature:graph-compilation]") {
    const auto service = subscription_service<auto(TS<bg::i64, TimeSeriesGuarantee::final>)->TS<bg::i64>>{
        "lookup", {"key"}};
    const auto respond = node([](In<TSS<bg::i64>> requests, Out<TSD<bg::i64, TS<bg::i64>>> responses) {
        requests.for_each_added([&](const auto &key) { responses[key].set(*key + 1); });
        requests.for_each_removed([&](const auto &key) { responses.erase(key); });
    });
    const auto implementation =
        graph("name"_n = "lookup_impl", [&](wp<TSS<bg::i64>> requests) { return respond(requests); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    const auto tested = [&](wp<TSD<bg::i64, TS<bg::i64, TimeSeriesGuarantee::final>>> values) {
        const auto mapped = map(
            [&](auto key, auto) { return service(key).template as<TS<bg::i64>>(); }, values,
            "__key_arg__"_n = 0);
        return mapped[bg::i64{1}].as<TS<bg::i64>>();
    };

    auto diagnostic = std::string{};
    try {
        (void)testing::test_graph(tested, bg::GraphRunOptions{.services = &catalogue}, {{{1, 4}, {2, 9}}});
    } catch (const std::exception &error) {
        diagnostic = error.what();
    }

    CHECK(diagnostic.find("graph wiring contains a dependency cycle") != std::string::npos);
    INFO(diagnostic);
    CHECK(diagnostic.find("map[") != std::string::npos);
    CHECK(diagnostic.find("lookup_impl[") != std::string::npos);
}

TEST_CASE("subscription request keys compare complete hierarchical scalar values",
          "[feature:service][scalar:hierarchical][series:tss][shape:multiple]") {
    auto memory = allocator{};
    const auto first_key = bundle<ServiceKey>(memory, "tenant"_n = 3, "name"_n = symbol{"desk"});
    const auto equal_key = bundle<ServiceKey>(memory, "tenant"_n = 3, "name"_n = symbol{"desk"});
    const auto service =
        subscription_service<auto(TS<Bundle<ServiceKey>, TimeSeriesGuarantee::final>)->TS<bg::i64>>{
            "lookup", {"key"}};

    struct RequestLog {
        std::vector<std::size_t> sizes;
        std::vector<std::size_t> added;
    };
    auto log = RequestLog{};
    const auto collect = node([&](In<TSS<Bundle<ServiceKey>>> requests) {
        log.sizes.push_back(requests.size());
        auto added_count = std::size_t{};
        requests.for_each_added([&](const auto &) { ++added_count; });
        log.added.push_back(added_count);
    });
    const auto responses = node([](Out<TSD<Bundle<ServiceKey>, TS<bg::i64>>>) {});
    const auto implementation = graph("name"_n = "lookup_impl", [&](wp<TSS<Bundle<ServiceKey>>> requests) {
        collect(requests);
        return responses();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    const auto first_source = node(
        [&](In<TS<bg::i64>>, Out<TS<Bundle<ServiceKey>, TimeSeriesGuarantee::final>> output) {
            output.set_value(first_key.view());
        });
    const auto equal_source = node(
        [&](In<TS<bg::i64>>, Out<TS<Bundle<ServiceKey>, TimeSeriesGuarantee::final>> output) {
            output.set_value(equal_key.view());
        });
    const auto tested = [&](wp<TS<bg::i64>> input) {
        (void)service(first_source(input));
        (void)service(equal_source(input));
        return input;
    };

    const auto actual = testing::test_graph(tested, bg::GraphRunOptions{.services = &catalogue}, std::array<bg::i64, 1>{1});

    CHECK(actual.equals<bg::i64>({1}));
    CHECK(log.sizes.back() == 1);
    CHECK(std::ranges::find(log.added, 1) != log.added.end());
}
