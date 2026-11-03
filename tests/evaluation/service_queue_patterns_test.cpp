#include "service_test_helpers.hpp"

#include <hg/queue_node.hpp>
#include <hg/time_series/tsl/tsl.hpp>

#include <cstddef>
#include <stdexcept>

using namespace hg;

TEST_CASE("a reference service publishes queue values in separate simulation cycles",
          "[feature:service][feature:simulation][series:value][shape:single-node]"
          "[transition:first-tick][transition:update]") {
    const auto feed = reference_service<auto()->TS<i64>>{"queued_feed"};
    auto handle = Queue{};
    const auto source = queue<TS<i64>>(
        start = [&](Queue queue) {
            handle = queue;
            if (!queue.write(::literal::TimeSeriesLiteral(i64{10})) ||
                !queue.write(::literal::TimeSeriesLiteral(i64{20}))) {
                throw std::logic_error{"queue write was rejected during start"};
            }
        });

    const auto implementation = graph("name"_n = "queued_feed_impl", [&] { return source(); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(feed, implementation);

    const auto tested = [&] { return feed(); };
    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue});

    CHECK(actual.equals<i64>({10, 20}));
    CHECK_FALSE(handle.is_open());
}

TEST_CASE("reference service clients share queue updates from one implementation",
          "[feature:service][feature:simulation][series:value][shape:fan-out]"
          "[transition:first-tick][transition:update]") {
    const auto feed = reference_service<auto()->TS<i64>>{"shared_queued_feed"};
    auto start_count = std::size_t{};
    const auto source = queue<TS<i64>>(
        start = [&](Queue queue) {
            ++start_count;
            if (!queue.write(::literal::TimeSeriesLiteral(i64{10})) ||
                !queue.write(::literal::TimeSeriesLiteral(i64{20}))) {
                throw std::logic_error{"queue write was rejected during start"};
            }
        });

    const auto implementation = graph("name"_n = "shared_queued_feed_impl", [&] { return source(); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(feed, implementation);

    const auto copy = node([](In<TSL<TS<i64>, 2>> values, Out<TSL<TS<i64>, 2>> output) {
        output.copy_tick(values);
    });

    const auto tested = [&] { return copy(tsl(feed(), feed())); };
    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue});

    CHECK(actual.equals({{{0, 10}, {1, 10}}, {{0, 20}, {1, 20}}}));
    CHECK(start_count == 1);
}

TEST_CASE("separate reference implementations contribute queue sources to one graph",
          "[feature:service][feature:simulation][series:value][shape:multiple][transition:first-tick]") {
    const auto bid = reference_service<auto()->TS<i64>>{"queued_bid"};
    const auto ask = reference_service<auto()->TS<i64>>{"queued_ask"};
    const auto bid_source = queue<TS<i64>>(start = [](Queue queue) {
        if (!queue.write(::literal::TimeSeriesLiteral(i64{40}))) {
            throw std::logic_error{"bid queue write was rejected during start"};
        }
    });
    const auto ask_source = queue<TS<i64>>(start = [](Queue queue) {
        if (!queue.write(::literal::TimeSeriesLiteral(i64{2}))) {
            throw std::logic_error{"ask queue write was rejected during start"};
        }
    });

    const auto bid_impl = graph("name"_n = "queued_bid_impl", [&] { return bid_source(); });
    const auto ask_impl = graph("name"_n = "queued_ask_impl", [&] { return ask_source(); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(bid, bid_impl);
    catalogue.register_impl(ask, ask_impl);

    const auto spread = node([](i64 bid, i64 ask) { return bid + ask; });
    const auto tested = [&] { return spread(bid(), ask()); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}).equals<i64>({42}));
}

TEST_CASE("subscription clients select their values from a queue-backed service",
          "[feature:service][feature:simulation][series:tss][series:tsd][shape:multiple]"
          "[transition:first-tick][transition:update][transition:removal][transition:resurrection]") {
    const auto quotes = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{
        "queued_quotes", {"key"}};
    const auto source = queue<TSD<i64, TS<i64>>>(start = [](Queue queue) {
        if (!queue.write(::literal::TimeSeriesLiteral{{7, 10}, {8, 20}}) ||
            !queue.write(::literal::TimeSeriesLiteral{{7, 11}}) ||
            !queue.write(::literal::TimeSeriesLiteral{{7, literal::REMOVE}}) ||
            !queue.write(::literal::TimeSeriesLiteral{{7, 12}})) {
            throw std::logic_error{"queue write was rejected during start"};
        }
    });

    const auto implementation = graph("name"_n = "queued_quotes_impl", [&](wp<TSS<i64>>) { return source(); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(quotes, implementation);

    const auto key = node<FinalKeySource<i64>>();
    const auto copy = node([](In<TSL<TS<i64>, 2>> values, Out<TSL<TS<i64>, 2>> output) {
        output.copy_tick(values);
    });
    const auto tested = [&] { return copy(tsl(quotes(key(7)), quotes(key(8)))); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue})
              .equals({{{0, 10}, {1, 20}},
                       {{0, 11}},
                       {{0, literal::INVALID}},
                       {{0, 12}}}));
}

TEST_CASE("switching between queue-backed subscriptions follows the selected keys",
          "[feature:service][feature:switch][feature:nested-graph][feature:simulation]"
          "[series:tss][series:tsd][series:value][shape:nested]"
          "[transition:removal][transition:resurrection]") {
    const auto quotes = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{
        "switched_queued_quotes", {"key"}};
    const auto source = queue<TSD<i64, TS<i64>>>(start = [](Queue queue) {
        if (!queue.write(::literal::TimeSeriesLiteral{{7, 10}, {8, 20}}) ||
            !queue.write(::literal::TimeSeriesLiteral{{7, 11}}) ||
            !queue.write(::literal::TimeSeriesLiteral{{7, literal::REMOVE}}) ||
            !queue.write(::literal::TimeSeriesLiteral{{7, 12}})) {
            throw std::logic_error{"queue write was rejected during start"};
        }
    });

    const auto implementation = graph("name"_n = "switched_queued_quotes_impl", [&](wp<TSS<i64>>) {
        return source();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(quotes, implementation);

    const auto key = node<FinalKeySource<i64>>();
    const auto key_seven = graph("name"_n = "subscribe_to_seven", [&](wp<TS<bool>>) { return quotes(key(7)); });
    const auto key_eight = graph("name"_n = "subscribe_to_eight", [&](wp<TS<bool>>) { return quotes(key(8)); });
    const auto tested = [&](wp<TS<bool>> select_seven) {
        return switch_(select_seven,
                       cases{case_(true, key_seven), case_(false, key_eight)},
                       select_seven);
    };

    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue}, {true, false, true, false, true});
    CHECK(actual.equals({10, 20, 10, 20, 10, 11, literal::INVALID, 12}));
}

TEST_CASE("mapped service clients observe queue updates through one shared implementation",
          "[feature:service][feature:map][feature:nested-graph][feature:simulation]"
          "[series:tsd][series:value][shape:nested][transition:first-tick][transition:update]") {
    const auto feed = reference_service<auto()->TS<i64>>{"mapped_queued_feed"};
    const auto source = queue<TS<i64>>(start = [](Queue queue) {
        if (!queue.write(::literal::TimeSeriesLiteral(i64{10})) ||
            !queue.write(::literal::TimeSeriesLiteral(i64{20}))) {
            throw std::logic_error{"queue write was rejected during start"};
        }
    });

    const auto implementation = graph("name"_n = "mapped_queued_feed_impl", [&] { return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(feed, implementation);

    const auto add = node([](i64 value, i64 tick) { return value + tick; });
    const auto add_feed = graph("name"_n = "add_feed_to_value", [&](wp<TS<i64>> value) {
        return add(value, feed());
    });
    const auto copy = node([](In<TSD<i64, TS<i64>>> values, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(values);
    });
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) { return copy(map(add_feed, values)); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {{{1, 1}, {2, 2}}})
              .equals({{{1, 11}, {2, 12}}, {{1, 21}, {2, 22}}}));
}

TEST_CASE("request clients receive queued replies from their service implementation",
          "[feature:service][feature:simulation][series:tsd][shape:multiple]"
          "[transition:first-tick]") {
    const auto double_value = service<auto(TS<i64>)->TS<i64>>{"queued_double", {"value"}};
    auto replies = Queue{};
    const auto response_source = queue<TSD<service_client_id_t, TS<i64>>>(
        start = [&](Queue queue) { replies = queue; });

    const auto dispatch = node([&](In<TSD<service_client_id_t, TS<i64>>> requests) {
        requests.for_each_modified_key_value([&](const auto &client_id, const auto &value) {
            if (!replies.write(::literal::TimeSeriesLiteral{{*client_id, *value * 2}})) {
                throw std::logic_error{"queue write was rejected while replying"};
            }
        });
    });

    const auto implementation = graph(
        "name"_n = "queued_double_impl",
        [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests) {
            dispatch(requests);
            return response_source();
        });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(double_value, implementation);

    const auto copy = node([](In<TSL<TS<i64>, 2>> values, Out<TSL<TS<i64>, 2>> output) {
        output.copy_tick(values);
    });

    const auto tested = [&](wp<TS<i64>> left, wp<TS<i64>> right) {
        return copy(tsl(double_value(left), double_value(right)));
    };

    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue}, {2, 4}, {3, 5});
    CHECK(actual.equals({literal::SKIP,
                         literal::SKIP,
                         {{0, 4}, {1, 6}},
                         {{0, 8}, {1, 10}}}));
}

TEST_CASE("mapped request clients receive their queue replies under the original keys",
          "[feature:service][feature:map][feature:nested-graph][feature:simulation]"
          "[series:tsd][shape:nested][transition:first-tick]") {
    const auto double_value = service<auto(TS<i64>)->TS<i64>>{"mapped_queued_double", {"value"}};
    auto replies = Queue{};
    const auto response_source = queue<TSD<service_client_id_t, TS<i64>>>(
        start = [&](Queue queue) { replies = queue; });

    const auto dispatch = node([&](In<TSD<service_client_id_t, TS<i64>>> requests) {
        requests.for_each_added_key_value([&](const auto &client_id, const auto &value) {
            if (!replies.write(::literal::TimeSeriesLiteral{{*client_id, *value * 2}})) {
                throw std::logic_error{"queue write was rejected while replying"};
            }
        });
    });

    const auto implementation = graph("name"_n = "mapped_queued_double_impl",
                                      [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests) {
                                          dispatch(requests);
                                          return response_source();
                                      });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(double_value, implementation);

    const auto client = graph("name"_n = "mapped_double_client", [&](wp<TS<i64>> value) {
        return double_value(value);
    });
    const auto copy = node([](In<TSD<i64, TS<i64>>> values, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(values);
    });
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) { return copy(map(client, values)); };

    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue}, {{{1, 2}, {2, 3}}});
    CHECK(actual.equals({{{1, literal::INVALID}, {2, literal::INVALID}}, {{1, 4}, {2, 6}}}));
}

TEST_CASE("a reused service catalogue creates a fresh queue for each graph run",
          "[feature:service][feature:simulation][series:value][shape:single-node]"
          "[transition:first-tick][transition:lifecycle]") {
    const auto feed = reference_service<auto()->TS<i64>>{"reused_queued_feed"};
    auto run_count = i64{};
    const auto source = queue<TS<i64>>(start = [&](Queue queue) {
        ++run_count;
        if (!queue.write(::literal::TimeSeriesLiteral(run_count))) {
            throw std::logic_error{"queue write was rejected during start"};
        }
    });

    const auto implementation = graph("name"_n = "reused_queued_feed_impl", [&] { return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(feed, implementation);

    const auto tested = [&] { return feed(); };
    const auto first_run = run_graph(tested, GraphRunOptions{.services = &catalogue});
    const auto second_run = run_graph(tested, GraphRunOptions{.services = &catalogue});

    CHECK(first_run.equals<i64>({1}));
    CHECK(second_run.equals<i64>({2}));
}
