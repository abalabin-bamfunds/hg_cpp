#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/nested.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>
#include <hg/service.hpp>
#include <hg/service_catalogue.hpp>
#include <hg/service_feedback.hpp>
#include <hg/switch.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <stdexcept>

using namespace hg;
using namespace hg::literals;

TEST_CASE("a service returns its implementation result",
          "[feature:service][series:tsd][series:value][shape:nested]") {
    const auto increment_service = service<auto(TS<i64>)->TS<i64>>{"increment", {"value"}};
    const auto increment = node([](i64 value) { return value + 1; });

    const auto implementation =
        graph("name"_n = "increment_impl", [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) {
            return map(increment, values);
        });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(increment_service, implementation);

    const auto tested = [&](wp<TS<i64>> value) { return increment_service(value); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2}).equals<i64>({2, 3}));
}

TEST_CASE("a request reply implementation can depend on a reference service",
          "[feature:service][feature:reference][series:tsd][series:value][shape:nested]") {
    const auto offset = reference_service<auto()->TS<i64>>{"offset"};
    const auto add = service<auto(TS<i64>)->TS<i64>>{"add_offset", {"value"}};

    const auto add_offset = node([](i64 value, i64 offset) { return value + offset; });

    const auto offset_impl = graph("name"_n = "offset_impl", [] { return operators::const_(i64{10}); });

    const auto add_impl = graph(
        "name"_n = "add_offset_impl",
        [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) { return map(add_offset, values, offset()); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(add, add_impl);
    catalogue.register_impl(offset, offset_impl);

    const auto tested = [&](wp<TS<i64>> value) { return add(value); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2}).equals<i64>({11, 12}));
}

TEST_CASE("feedback service wrapper resolves a declared implementation output",
          "[feature:feedback][feature:service][series:tsd][series:value][shape:nested]") {
    const auto adjust = service<auto(TS<i64>)->TS<i64>>{"adjust_with_feedback", {"value"}};
    const auto add_one = node([](i64 value) { return value + 1; });

    const auto implementation = graph("name"_n = "adjust_with_feedback_impl",
                                      [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests)
                                          -> wp<TSD<service_client_id_t, TS<i64>>> {
                                          return map(add_one, requests).as<TSD<service_client_id_t, TS<i64>>>();
                                      });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(adjust, feedback_service_implementation(adjust, implementation));

    const auto tested = [&](wp<TS<i64>> value) { return adjust(value); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1})
              .equals<i64>({std::nullopt, 2}));
}

TEST_CASE("request reply calls inside a mapped switch handle late keys",
          "[feature:service][feature:map][feature:switch][feature:nested-graph]"
          "[series:tsd][series:value][shape:nested][transition:addition]") {
    const auto adjust = service<auto(TS<i64>)->TS<i64>>{"adjust", {"value"}};
    const auto add_two = node([](i64 value) { return value + 2; });

    const auto implementation = graph("name"_n = "adjust_impl",
                                      [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests)
                                          -> wp<TSD<service_client_id_t, TS<i64>>> {
                                          return map(add_two, requests).as<TSD<service_client_id_t, TS<i64>>>();
                                      });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(adjust, feedback_service_implementation(adjust, implementation));

    const auto direct = graph("name"_n = "service_branch", [&](wp<TS<i64>> value) { return adjust(value); });
    const auto local = node([](i64 value) { return value * 2 - 2; });

    const auto per_key = graph("name"_n = "select_adjustment", [&](wp<TS<i64>> value, wp<TS<bool>> use_service) {
        return switch_(use_service, cases{case_(true, direct), case_(false, [&](auto input) { return local(input); })},
                       value);
    });

    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<bool>> use_service) {
        return copy(map(per_key, values, use_service, "__valid_items__"_n = true));
    };

    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue},
                                  {{{1, 3}}, literal::SKIP, {{2, 4}}},
                                  {true, literal::SKIP, literal::SKIP});
    CHECK(actual.equals({literal::SKIP, {{1, 5}}, literal::SKIP, {{2, 6}}}));
}

TEST_CASE("a mapped switch removes an invalid request reply before it arrives",
          "[feature:service][feature:map][feature:switch][feature:nested-graph]"
          "[series:tsd][series:value][shape:nested][transition:invalidation]") {
    const auto adjust = service<auto(TS<i64>)->TS<i64>>{"adjust_after_switch", {"value"}};
    const auto add_two = node([](i64 value) { return value + 2; });

    const auto implementation = graph("name"_n = "adjust_after_switch_impl",
                                      [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests)
                                          -> wp<TSD<service_client_id_t, TS<i64>>> {
                                          return map(add_two, requests).as<TSD<service_client_id_t, TS<i64>>>();
                                      });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(adjust, feedback_service_implementation(adjust, implementation));

    const auto service_branch = graph("name"_n = "request_reply_branch", [&](wp<TS<i64>> value) {
        return adjust(value);
    });
    const auto local = node([](i64 value) { return value * 2 - 2; });

    const auto per_key = graph("name"_n = "switch_request_reply", [&](wp<TS<i64>> value, wp<TS<bool>> use_service) {
        return switch_(use_service,
                       cases{case_(true, service_branch), case_(false, [&](auto input) { return local(input); })},
                       value);
    });

    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<bool>> use_service) {
        return copy(map(per_key, values, use_service, "__valid_items__"_n = true));
    };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue},
                    {{{1, 3}}, literal::SKIP, literal::SKIP},
                    {false, true, literal::SKIP})
              .equals({{{1, 4}}, {{1, literal::REMOVE}}, {{1, 5}}}));
}

TEST_CASE("a subscription implementation can depend on a reference service",
          "[feature:service][feature:reference][series:tss][series:tsd][shape:nested]") {
    const auto offset = reference_service<auto()->TS<i64>>{"subscription_offset"};
    const auto lookup = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{
        "dependent_lookup", {"key"}};

    const auto add_offset = node([](i64 key, i64 offset) { return key + offset; });

    const auto offset_impl = graph("name"_n = "subscription_offset_impl", [] {
        return operators::const_(i64{10});
    });

    const auto lookup_impl = graph("name"_n = "dependent_lookup_impl", [&](wp<TSS<i64>> keys)
                                       -> wp<TSD<i64, TS<i64>>> {
        return map(add_offset, "__keys__"_n = keys, offset(), "__key_arg__"_n = 0).as<TSD<i64, TS<i64>>>();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(lookup, lookup_impl);
    catalogue.register_impl(offset, offset_impl);

    const auto key = node([](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(7); });
    const auto tested = [&] { return lookup(key()); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}).equals<i64>({17}));
}

namespace {

template <template <typename, auto...> typename Field> struct DelayedLookupValue {
    Field<TS<i64>> value;
};

} // namespace

TEST_CASE("subscription clients can read a delayed free-bundle field",
          "[feature:service][feature:timed-scheduling][feature:time-series-storage]"
          "[series:tss][series:tsd][series:tsb][series:value][schema:named][representation:free]") {
    const auto lookup = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TSB<DelayedLookupValue>>{
        "delayed_lookup", {"key"}};
    const auto delayed_value = graph("name"_n = "delayed_lookup_value", [](wp<TS<i64>> key) {
        return bundle<TSB<DelayedLookupValue>>("value"_n = operators::delay(key, timedelta_t{2}));
    });

    const auto implementation = graph("name"_n = "delayed_lookup_impl", [&](wp<TSS<i64>> keys)
                                          -> wp<TSD<i64, TSB<DelayedLookupValue>>> {
        return map(delayed_value, "__keys__"_n = keys, "__key_arg__"_n = 0).as<TSD<i64, TSB<DelayedLookupValue>>>();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(lookup, implementation);

    const auto key = node([](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(7); });
    const auto tested = [&] { return lookup(key()).as<TSB<DelayedLookupValue>>()->value; };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue})
              .equals<i64>({std::nullopt, std::nullopt, 7}));
}

TEST_CASE("subscription requests stay active while another map branch uses the same key",
          "[feature:service][feature:map][feature:nested-graph][series:tss][series:tsd]"
          "[transition:removal]") {
    const auto lookup = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{
        "shared_lookup", {"key"}};
    const auto multiply_by_ten = node([](i64 key) { return key * 10; });

    const auto implementation = graph("name"_n = "shared_lookup_impl", [&](wp<TSS<i64>> keys)
                                           -> wp<TSD<i64, TS<i64>>> {
        return map(multiply_by_ten, "__keys__"_n = keys, "__key_arg__"_n = 0).as<TSD<i64, TS<i64>>>();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(lookup, feedback_service_implementation(lookup, implementation));

    const auto key = node([](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(7); });

    const auto client = graph("name"_n = "shared_lookup_client", [&](wp<TS<i64>>) { return lookup(key()); });

    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) { return copy(map(client, values)); };

    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue},
                                  {{{1, 1}}, {{2, 1}}, {{1, literal::REMOVE}}, literal::SKIP,
                                   {{2, literal::REMOVE}}, literal::SKIP, {{1, 1}}});
    CHECK(actual.equals({{{1, literal::INVALID}},
                         {{1, 70}, {2, 70}},
                         {{1, literal::REMOVE}},
                         literal::SKIP,
                         {{2, literal::REMOVE}},
                         literal::SKIP,
                         {{1, literal::INVALID}},
                         {{1, 70}}}));
}

TEST_CASE("feedback service wrapper infers a mapped implementation output",
          "[feature:feedback][feature:service][series:tsd][series:value]") {
    const auto adjust = service<auto(TS<i64>)->TS<i64>>{"inferred_adjust", {"value"}};
    const auto add_one = node([](i64 value) { return value + 1; });

    const auto implementation = graph("name"_n = "inferred_adjust_impl",
                                      [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests) {
                                          return map(add_one, requests);
                                      });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(adjust, feedback_service_implementation(adjust, implementation));

    const auto tested = [&](wp<TS<i64>> value) { return adjust(value); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1})
              .equals<i64>({std::nullopt, 2}));
}

TEST_CASE("nested graphs infer an operator output type",
          "[feature:graph-compilation][feature:nested-graph][series:value][scalar:i64]") {
    auto body_calls = std::size_t{};
    const auto inferred = graph("name"_n = "inferred_increment", [&](wp<TS<i64>> value) {
        ++body_calls;
        return operators::add(value, i64{1});
    });

    const auto declared = graph("name"_n = "declared_increment",
                                [](wp<TS<i64>> value) -> wp<TS<i64>> {
                                    return operators::add(value, i64{1}).as<TS<i64>>();
                                });

    const auto inferred_call = [&](wp<TS<i64>> value) {
        return nested(inferred, operators::const_(true), value);
    };

    const auto declared_call = [&](wp<TS<i64>> value) {
        return nested(declared, operators::const_(true), value);
    };

    CHECK(run_graph(declared_call, {1, 2}).equals<i64>({2, 3}));
    CHECK(run_graph(inferred_call, {1, 2}).equals<i64>({2, 3}));
    CHECK(body_calls == 1);
}

TEST_CASE("nested graphs preserve output resolver failures before wiring the body",
          "[feature:graph-compilation][feature:nested-graph][feature:type-system]") {
    using OUTPUT_VALUE = ScalarVariable<"NESTED_OUTPUT_VALUE">;
    auto resolver_calls = std::size_t{};
    auto body_calls = std::size_t{};
    const auto unresolved = graph(
        "name"_n = "unresolved_nested_output",
        resolve<OUTPUT_VALUE> = [&](const TypeMap &) -> const ScalarType & {
            ++resolver_calls;
            throw std::runtime_error{"nested output resolver rejected the graph"};
        },
        [&](wp<TS<i64>> value) -> wp<TS<OUTPUT_VALUE>> {
            ++body_calls;
            return wp<TS<OUTPUT_VALUE>>{value};
        });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) { return nested(unresolved, run, value); };

    CHECK_THROWS_WITH(run_graph(tested, {1}, {true}),
                      Catch::Matchers::ContainsSubstring("nested output resolver rejected the graph"));
    CHECK(resolver_calls == 1);
    CHECK(body_calls == 0);
}
