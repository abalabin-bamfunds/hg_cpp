#include <hg/graph.hpp>
#include <hg/graph/context.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>
#include <hg/service.hpp>
#include <hg/service_catalogue.hpp>
#include <hg/service_feedback.hpp>
#include <hg/switch.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/time_series/tsl/tsl.hpp>
#include <hg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

using namespace hg;
using namespace hg::literals;

TEST_CASE("a request reply service can call another service",
          "[feature:service][series:tsd][series:value][shape:nested]") {
    const auto increment = service<auto(TS<i64>)->TS<i64>>{"increment", {"value"}};
    const auto add_ten = service<auto(TS<i64>)->TS<i64>>{"add_ten", {"value"}};

    const auto increment_value = node([](i64 value) { return value + 1; });

    const auto increment_impl = graph("name"_n = "increment_impl",
                                      [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) {
                                          return map(increment_value, values);
                                      });

    const auto add_ten_value = node([](i64 value) { return value + 10; });

    const auto call_increment = graph("name"_n = "call_increment", [&](wp<TS<i64>> value) {
        return add_ten_value(increment(value));
    });

    const auto add_ten_impl = graph("name"_n = "add_ten_impl",
                                    [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) {
                                        return map(call_increment, values);
                                    });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(add_ten, add_ten_impl);
    catalogue.register_impl(increment, feedback_service_implementation(increment, increment_impl));

    const auto tested = [&](wp<TS<i64>> value) { return add_ten(value); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, literal::SKIP, 2})
              .equals<i64>({std::nullopt, 12, std::nullopt, 13}));
}

TEST_CASE("two clients can use one request reply implementation in the same cycle",
          "[feature:service][series:fixed-tsl][series:value][shape:multiple]") {
    const auto increment = service<auto(TS<i64>)->TS<i64>>{"increment", {"value"}};
    const auto add_one = node([](i64 value) { return value + 1; });

    const auto implementation = graph("name"_n = "shared_increment_impl",
                                       [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests) {
                                           return map(add_one, requests);
                                       });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(increment, implementation);

    const auto copy = node([](In<TSL<TS<i64>, 2>> values, Out<TSL<TS<i64>, 2>> output) {
        output.copy_tick(values);
    });

    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return copy(tsl(increment(first), increment(second)));
    };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2, literal::SKIP},
                    {10, literal::SKIP, 20})
              .equals({{{0, 2}, {1, 11}}, {{0, 3}}, {{1, 21}}}));
}

TEST_CASE("service wiring preserves client order across mixed implementations",
          "[feature:service][feature:multiservice][feature:reference][series:fixed-tsl][shape:multiple]") {
    enum class Service {
        reference,
        other_reference,
        subscription,
        other_subscription,
        request_reply,
        other_request_reply,
    };

    const auto reference = reference_service<auto()->TS<i64>>{"reference"};
    const auto other_reference = reference_service<auto()->TS<i64>>{"other_reference"};
    const auto subscription = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{
        "subscription", {"key"}};
    const auto other_subscription = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{
        "other_subscription", {"key"}};
    const auto request_reply = service<auto(TS<i64>)->TS<i64>>{"request_reply", {"value"}};
    const auto other_request_reply = service<auto(TS<i64>)->TS<i64>>{"other_request_reply", {"value"}};

    const auto interfaces = std::array<const ServiceDefinition *, 6>{
        &reference,
        &other_reference,
        &subscription,
        &other_subscription,
        &request_reply,
        &other_request_reply,
    };
    const auto expected_values = std::array<i64, 6>{10, 11, 20, 21, 8, 9};

    const auto key = node([](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(2); });

    const auto add_one = node([](i64 value) { return value + 1; });

    const auto add_two = node([](i64 value) { return value + 2; });

    const auto lookup = [&](i64 offset) {
        return node([offset](In<TSS<i64>> keys, Out<TSD<i64, TS<i64>>> responses) {
            keys.for_each_removed([&](const auto &item) { responses.erase(item); });
            keys.for_each_added([&](const auto &item) { responses[item].set(*item * 10 + offset); });
        });
    };

    const auto answer_subscription = lookup(0);

    const auto answer_other_subscription = lookup(1);

    const auto orders = std::array{
        std::vector{Service::reference, Service::other_reference},
        std::vector{Service::other_reference, Service::reference},
        std::vector{Service::subscription, Service::other_subscription},
        std::vector{Service::other_subscription, Service::subscription},
        std::vector{Service::request_reply, Service::other_request_reply},
        std::vector{Service::other_request_reply, Service::request_reply},
        std::vector{Service::reference, Service::subscription},
        std::vector{Service::subscription, Service::reference},
        std::vector{Service::reference, Service::request_reply},
        std::vector{Service::request_reply, Service::reference},
        std::vector{Service::subscription, Service::request_reply},
        std::vector{Service::request_reply, Service::subscription},
        std::vector{Service::reference, Service::subscription, Service::request_reply},
        std::vector{Service::reference, Service::request_reply, Service::subscription},
        std::vector{Service::subscription, Service::reference, Service::request_reply},
        std::vector{Service::subscription, Service::request_reply, Service::reference},
        std::vector{Service::request_reply, Service::reference, Service::subscription},
        std::vector{Service::request_reply, Service::subscription, Service::reference},
    };

    const auto add_implementation = [&](Service selected) {
        switch (selected) {
        case Service::reference:
            reference.implementation_replies(operators::const_(i64{10}));
            break;
        case Service::other_reference:
            other_reference.implementation_replies(operators::const_(i64{11}));
            break;
        case Service::subscription:
            subscription.implementation_replies(answer_subscription(subscription.implementation_requests()));
            break;
        case Service::other_subscription:
            other_subscription.implementation_replies(
                answer_other_subscription(other_subscription.implementation_requests()));
            break;
        case Service::request_reply: {
            const auto requests = request_reply.implementation_requests();
            request_reply.implementation_replies(map(add_one, operators::getattr(requests, "value"_sym)));
            break;
        }
        case Service::other_request_reply: {
            const auto requests = other_request_reply.implementation_requests();
            other_request_reply.implementation_replies(map(add_two, operators::getattr(requests, "value"_sym)));
            break;
        }
        }
    };

    const auto call = [&](Service selected, wp<TS<i64>> value) -> WiringPoint {
        switch (selected) {
        case Service::reference:
            return static_cast<WiringPoint>(reference());
        case Service::other_reference:
            return static_cast<WiringPoint>(other_reference());
        case Service::subscription:
            return static_cast<WiringPoint>(subscription(key()));
        case Service::other_subscription:
            return static_cast<WiringPoint>(other_subscription(key()));
        case Service::request_reply:
            return static_cast<WiringPoint>(request_reply(value));
        case Service::other_request_reply:
            return static_cast<WiringPoint>(other_request_reply(value));
        }
        std::unreachable();
    };

    const auto value_for = [&](Service selected) {
        return expected_values[static_cast<std::size_t>(selected)];
    };

    const auto copy = node([](In<TSL<TS<i64>>> values, Out<TSL<TS<i64>>> output) {
        output.copy_tick(values);
    });

    auto order_index = std::size_t{};
    for (const auto &order : orders) {
        INFO("service order case " << order_index++);
        auto implementation_wirings = std::size_t{};
        const auto implementation = graph("name"_n = "ordered_services", [&] {
            ++implementation_wirings;
            for (const auto selected : order) {
                add_implementation(selected);
            }
        });

        auto catalogue = ServiceCatalogue{};
        auto registration = catalogue.register_multiservice(implementation);
        for (const auto selected : order) {
            registration.register_impl(*interfaces[static_cast<std::size_t>(selected)]);
        }

        const auto tested = [&](wp<TS<i64>> value) {
            auto clients = std::vector<WiringPoint>{};
            clients.reserve(order.size());
            for (const auto selected : order) {
                clients.push_back(call(selected, value));
            }
            return copy(tsl(std::span<const WiringPoint>{clients}));
        };

        const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue}, {7});
        CHECK(implementation_wirings == 1);
        if (order.size() == 2) {
            CHECK(actual.equals({{{0, value_for(order[0])}, {1, value_for(order[1])}}}));
        } else {
            CHECK(actual.equals({{{0, value_for(order[0])},
                                  {1, value_for(order[1])},
                                  {2, value_for(order[2])}}}));
        }
    }
}

TEST_CASE("a request reply service can call itself until its request reaches zero",
          "[feature:service][feature:feedback][feature:nested-graph][series:tsd][series:value]") {
    const auto add_until_zero = service<auto(TS<i64>)->TS<i64>>{"add_until_zero", {"value"}};
    const auto add_one = node([](i64 value) { return value + 1; });

    const auto base_case = graph("name"_n = "service_base_case", [](wp<TS<i64>>) {
        return operators::const_(i64{1});
    });

    const auto recursive_case = graph("name"_n = "recursive_service_case", [&](wp<TS<i64>> value) {
        return add_one(add_until_zero(value - i64{1}));
    });

    const auto evaluate_request = graph("name"_n = "evaluate_recursive_request", [&](wp<TS<i64>> value) {
        return switch_(operators::eq(value, i64{0}),
                       cases{case_(true, base_case), case_(false, recursive_case)}, value);
    });

    const auto implementation = graph(
        "name"_n = "recursive_service_impl",
        [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests) { return map(evaluate_request, requests); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(add_until_zero, feedback_service_implementation(add_until_zero, implementation));

    const auto tested = [&](wp<TS<i64>> value) { return add_until_zero(value); };
    const auto actual = run_graph(tested,
                                  GraphRunOptions{.services = &catalogue, .run_time = std::chrono::microseconds{10}},
                                  {3});

    REQUIRE(actual.size() > 0);
    REQUIRE(actual[actual.size() - 1].has_value());
    CHECK(actual[actual.size() - 1]->value() == i64{4});
}

TEST_CASE("a service installation input stays bound to the root across map wiring",
          "[feature:service][feature:reference][feature:map][feature:nested-graph]"
          "[series:tsd][series:value][shape:nested]") {
    const auto contextual_value = reference_service<auto()->TS<i64>>{"contextual_value"};
    const auto add = node([](i64 request, i64 child_setting, i64 installed_setting) {
        return request + child_setting + installed_setting;
    });

    const auto read_setting = node([](In<TS<i64>, "setting"_n, context> setting) { return *setting; });

    const auto implementation = graph("name"_n = "contextual_value_impl",
                                      [](wp<TS<i64>> setting) { return setting; });

    const auto per_key = graph("name"_n = "read_contextual_value", [&](wp<TS<i64>> value, wp<TS<i64>> setting) {
        return with_context("setting"_n = setting, [&] {
            return add(value, read_setting(), contextual_value());
        });
    });

    auto catalogue = ServiceCatalogue{};
    const auto tested = [&](wp<TSD<i64, TS<i64>>> requests, wp<TS<i64>> setting,
                            wp<TS<i64>> child_setting) {
        catalogue.register_impl(contextual_value, implementation, setting);
        return map(per_key, requests, child_setting);
    };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {{{1, 5}}, {{1, 6}}}, {100, 200}, {7, 8})
              .equals({{{1, 112}}, {{1, 214}}}));
}

TEST_CASE("mapped subscription clients handle late keys and recreation",
          "[feature:service][feature:map][feature:nested-graph]"
          "[series:tss][series:tsd][series:value][transition:addition][transition:removal]") {
    const auto lookup = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{
        "mapped_lookup", {"key"}};
    const auto answer = node([](In<TSS<i64>> keys, Out<TSD<i64, TS<i64>>> replies) {
        keys.for_each_removed([&](const auto &key) { replies.erase(key); });
        keys.for_each_added([&](const auto &key) { replies[key].set(*key * 10); });
    });

    const auto implementation = graph("name"_n = "mapped_lookup_impl",
                                      [&](wp<TSS<i64>> keys) { return answer(keys); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(lookup, feedback_service_implementation(lookup, implementation));

    const auto per_key = graph("name"_n = "mapped_lookup_client", [&](wp<TS<i64, TimeSeriesGuarantee::final>> key) {
        return lookup(key);
    });

    const auto copy = node([](In<TSD<i64, TS<i64>>> values, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(values);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64, TimeSeriesGuarantee::final>>> keys) {
        return copy(map(per_key, keys));
    };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue},
                    {{{1, 7}}, {{2, 8}}, {{1, literal::REMOVE}}, {{1, 9}}})
              .equals({{{1, literal::INVALID}},
                       {{1, 70}, {2, literal::INVALID}},
                       {{1, literal::REMOVE}, {2, 80}},
                       {{1, literal::INVALID}},
                       {{1, 90}}}));
}
