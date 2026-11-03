#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
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

#include <cstddef>
using namespace hg;
using namespace hg::literals;
using namespace hg::util::literals;

namespace {

template <template <typename, auto...> typename Field> struct IntegerSnapshot {
    Field<i64> count;
};

template <template <typename, auto...> typename Field> struct TextSnapshot {
    Field<string> label;
};

template <template <typename, auto...> typename Field> struct SnapshotResults {
    Field<i64> count;
    Field<string> label;
};

template <template <typename, auto...> typename Field> struct AdjustedResults {
    Field<i64> first;
    Field<i64> second;
    Field<float> third;
    Field<float> fourth;
};

template <template <typename, auto...> typename Field> struct SharedResults {
    Field<i64> first;
    Field<i64> second;
    Field<float> fraction;
};

} // namespace

TEST_CASE("a multiservice routes one generic reference by output schema",
          "[feature:service][feature:multiservice][declaration:generic][series:tsb][schema:named]") {
    using RESULT = TimeSeriesVariable<"SNAPSHOT_RESULT">;
    const auto snapshot = reference_service<RESULT, auto()->RESULT>{"snapshot"};
    const auto integer_source = node([](Out<TSB<IntegerSnapshot>> output) { output->count.set(7); });

    const auto text_source = node([](Out<TSB<TextSnapshot>> output) { output->label.set("alpha"); });

    const auto implementation = graph("name"_n = "typed_snapshots", [&] {
        snapshot[type<TSB<IntegerSnapshot>>].implementation_replies(integer_source());
        snapshot[type<TSB<TextSnapshot>>].implementation_replies(text_source());
    });
    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(snapshot[type<TSB<IntegerSnapshot>>]);
    registration.register_impl(snapshot[type<TSB<TextSnapshot>>]);

    const auto collect = node([](In<TSB<IntegerSnapshot>> integer, In<TSB<TextSnapshot>> text,
                                 Out<TSB<SnapshotResults>> output) {
        output->count.copy_tick(integer->count);
        output->label.copy_tick(text->label);
    });

    const auto tested = [&] {
        return collect(snapshot[type<TSB<IntegerSnapshot>>](), snapshot[type<TSB<TextSnapshot>>]());
    };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue})
              .equals({{{"count", 7}, {"label", "alpha"}}}));
}

TEST_CASE("a generic multiservice filters scalar claims and receives installation values",
          "[feature:service][feature:multiservice][declaration:generic][scalar:integer]") {
    using VALUE = ScalarVariable<"MULTISERVICE_VALUE">;
    const auto adjust = service<VALUE, auto(TS<VALUE>, i64)->TS<VALUE>>{"adjust", {"value", "route"}};
    const auto add_integer = node([](i64 value, i64 bias) { return value + bias; });

    const auto add_fraction = node([](float value, i64 bias) { return value + static_cast<float>(bias); });

    const auto implementation = graph("name"_n = "typed_adjust", [&](i64 bias) {
        const auto integer = adjust[type<VALUE> = type<i64>];
        const auto integer_requests = integer.implementation_requests(1);
        integer.implementation_replies(
            map(add_integer, operators::getattr(integer_requests, "value"_sym), bias), 1);

        const auto fraction = adjust[type<VALUE> = type<float>];
        const auto fraction_requests = fraction.implementation_requests(2);
        fraction.implementation_replies(
            map(add_fraction, operators::getattr(fraction_requests, "value"_sym), bias), 2);
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation, i64{10});
    registration.register_impl(adjust[type<VALUE> = type<i64>], "route"_n = 1);
    registration.register_impl(adjust[type<VALUE> = type<float>], "route"_n = 2);

    const auto collect = node([](In<TS<i64>> first, In<TS<i64>> second, In<TS<float>> third,
                                 In<TS<float>> fourth, Out<TSB<AdjustedResults>> output) {
        output->first.copy_tick(first);
        output->second.copy_tick(second);
        output->third.copy_tick(third);
        output->fourth.copy_tick(fourth);
    });

    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<float>> third,
                            wp<TS<float>> fourth) {
        return collect(adjust[type<VALUE> = type<i64>](first, 1), adjust[type<VALUE> = type<i64>](second, 1),
                       adjust[type<VALUE> = type<float>](third, 2), adjust[type<VALUE> = type<float>](fourth, 2));
    };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1}, {2}, {0.5F}, {1.5F})
              .equals({{{"first", 11}, {"second", 12}, {"third", 10.5F}, {"fourth", 11.5F}}}));

    const auto wrong_route = [&](wp<TS<i64>> input) { return adjust[type<VALUE> = type<i64>](input, 2); };
    CHECK_THROWS(run_graph(wrong_route, GraphRunOptions{.services = &catalogue}, {1}));
}

TEST_CASE("a generic multiservice shares clients and rebuilds for independent graphs",
          "[feature:service][feature:multiservice][declaration:generic][shape:multiple]") {
    using RESULT = TimeSeriesVariable<"SHARED_RESULT">;
    const auto value = reference_service<RESULT, auto()->RESULT>{"shared_value"};
    auto implementation_wirings = std::size_t{};
    const auto integer_source = node([] { return i64{10}; });

    const auto fraction_source = node([] { return 0.5F; });

    const auto implementation = graph("name"_n = "shared_typed_values", [&] {
        ++implementation_wirings;
        value[type<TS<i64>>].implementation_replies(integer_source());
        value[type<TS<float>>].implementation_replies(fraction_source());
    });
    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(value[type<TS<i64>>]);
    registration.register_impl(value[type<TS<float>>]);

    const auto collect = node([](In<TS<i64>> first, In<TS<i64>> second, In<TS<float>> fraction,
                                 Out<TSB<SharedResults>> output) {
        output->first.copy_tick(first);
        output->second.copy_tick(second);
        output->fraction.copy_tick(fraction);
    });

    const auto tested = [&] {
        return collect(value[type<TS<i64>>](), value[type<TS<i64>>](), value[type<TS<float>>]());
    };

    const auto first = run_graph(tested, GraphRunOptions{.services = &catalogue});
    CHECK(first.equals({{{"first", 10}, {"second", 10}, {"fraction", 0.5F}}}));

    const auto second = run_graph(tested, GraphRunOptions{.services = &catalogue});
    CHECK(second.equals({{{"first", 10}, {"second", 10}, {"fraction", 0.5F}}}));
    CHECK(implementation_wirings == 2);
}

TEST_CASE("a generic request reply client runs inside a mapped switch branch",
          "[feature:service][feature:map][feature:switch][feature:nested-graph]"
          "[declaration:generic][series:tsd][transition:removal]") {
    using VALUE = ScalarVariable<"MAPPED_SERVICE_VALUE">;
    const auto adjust = service<VALUE, auto(TS<VALUE>)->TS<VALUE>>{"mapped_adjust", {"value"}};
    const auto add_one = node([](i64 value) { return value + 1; });

    const auto integer_adjust = adjust[type<VALUE> = type<i64>];
    const auto implementation = graph(
        "name"_n = "mapped_adjust_impl",
        [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests) { return map(add_one, requests); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(integer_adjust,
                            feedback_service_implementation(adjust, implementation));

    const auto service_branch = graph("name"_n = "service_branch", [&](wp<TS<i64>> value) {
        return adjust[type<VALUE> = type<i64>](value);
    });

    const auto double_value = node([](i64 value) { return value * 2; });

    const auto per_key = graph("name"_n = "mapped_adjust_client",
                               [&](wp<TS<i64>> value, wp<TS<bool>> selected) {
                                   return switch_(selected,
                                                  cases{case_(true, service_branch),
                                                        case_(false, [&](auto input) {
                                                            return double_value(input);
                                                        })},
                                                  value);
                               });

    const auto copy = node([](In<TSD<i64, TS<i64>>> values, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(values);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<bool>> selected) {
        return copy(map(per_key, values, selected, "__valid_items__"_n = true));
    };

    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue},
                                  {{{1, 3}, {2, 4}}, literal::SKIP, {{1, 5}, {2, literal::REMOVE}},
                                   literal::SKIP, literal::SKIP, literal::SKIP},
                                  {true, literal::SKIP, false, literal::SKIP, true, literal::SKIP});

    CHECK(actual.equals({literal::SKIP,
                         {{1, 4}, {2, 5}},
                         {{1, 10}, {2, literal::REMOVE}},
                         literal::SKIP,
                         {{1, literal::REMOVE}},
                         {{1, 6}}}));
}

TEST_CASE("generic subscription specializations use their own mapped key types",
          "[feature:service][feature:map][feature:nested-graph]"
          "[declaration:generic][series:tss][series:tsd]") {
    using KEY = ScalarVariable<"SUBSCRIPTION_KEY", Hashable>;
    const auto lookup = subscription_service<KEY, auto(TS<KEY, TimeSeriesGuarantee::final>)->TS<i64>>{
        "typed_lookup", {"key"}};
    const auto integer_answer = node([](i64 key) { return key * 10; });

    const auto text_answer = node([](In<TS<string>>) { return i64{5}; });

    const auto integer_implementation = graph("name"_n = "integer_lookup_impl", [&](wp<TSS<i64>> requests) {
        return map(integer_answer, "__keys__"_n = requests, "__key_arg__"_n = 0);
    });

    const auto text_implementation = graph("name"_n = "text_lookup_impl", [&](wp<TSS<string>> requests) {
        return map(text_answer, "__keys__"_n = requests, "__key_arg__"_n = 0);
    });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(lookup[type<KEY> = type<i64>],
                            feedback_service_implementation(lookup, integer_implementation));
    catalogue.register_impl(lookup[type<KEY> = type<string>],
                            feedback_service_implementation(lookup, text_implementation));

    const auto integer_keys = node([](Out<TSS<i64, TimeSeriesGuarantee::final>> keys) {
        keys.apply({7, 8});
    });

    const auto text_keys = node([](Out<TSS<string, TimeSeriesGuarantee::final>> keys) {
        keys.apply({"a", "bb"});
    });

    const auto integer_client =
        graph("name"_n = "integer_lookup_client", [&](wp<TS<i64, TimeSeriesGuarantee::final>> key) {
            return lookup[type<KEY> = type<i64>](key);
        });

    const auto text_client =
        graph("name"_n = "text_lookup_client", [&](wp<TS<string, TimeSeriesGuarantee::final>> key) {
            return lookup[type<KEY> = type<string>](key);
        });

    const auto copy_integers = node([](In<TSD<i64, TS<i64>>> values, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(values);
    });

    const auto copy_text = node([](In<TSD<string, TS<i64>>> values, Out<TSD<string, TS<i64>>> output) {
        output.copy_tick(values);
    });

    const auto tested_integers = [&] {
        return copy_integers(map(integer_client, integer_keys(), "__key_arg__"_n = 0));
    };

    const auto tested_text = [&] {
        return copy_text(map(text_client, text_keys(), "__key_arg__"_n = 0));
    };

    CHECK(run_graph(tested_integers, GraphRunOptions{.services = &catalogue})
              .equals({{{7, literal::INVALID}, {8, literal::INVALID}}, {{7, 70}, {8, 80}}}));
    CHECK(run_graph(tested_text, GraphRunOptions{.services = &catalogue})
              .equals({{{"a", literal::INVALID}, {"bb", literal::INVALID}}, {{"a", 5}, {"bb", 5}}}));
}

TEST_CASE("a mapped switch selects generic subscription scalar claims",
          "[feature:service][feature:map][feature:switch][feature:nested-graph]"
          "[declaration:generic][series:tss][series:tsd][scalar:integer]") {
    using KEY = ScalarVariable<"ROUTED_SUBSCRIPTION_KEY", Hashable>;
    const auto lookup =
        subscription_service<KEY, auto(TS<KEY, TimeSeriesGuarantee::final>, i64)->TS<i64>>{
            "routed_lookup", {"key", "route"}};
    const auto add_route = node([](i64 key, i64 route) { return key * 10 + route; });

    const auto implementation = graph("name"_n = "routed_lookup_impl", [&](wp<TSS<i64>> requests, i64 route) {
        return map(add_route, "__keys__"_n = requests, route, "__key_arg__"_n = 0);
    });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(lookup[type<KEY> = type<i64>],
                            feedback_service_implementation(lookup, implementation), "route"_n = 1);
    catalogue.register_impl(lookup[type<KEY> = type<i64>],
                            feedback_service_implementation(lookup, implementation), "route"_n = 2);

    const auto keys = node([](Out<TSS<i64, TimeSeriesGuarantee::final>> output) {
        output.apply({1, 2});
    });

    const auto first_route = graph("name"_n = "first_route", [&](wp<TS<i64, TimeSeriesGuarantee::final>> key) {
        return lookup[type<KEY> = type<i64>](key, 1);
    });

    const auto second_route = graph("name"_n = "second_route", [&](wp<TS<i64, TimeSeriesGuarantee::final>> key) {
        return lookup[type<KEY> = type<i64>](key, 2);
    });

    const auto client = graph("name"_n = "routed_lookup_client",
                              [&](wp<TS<i64, TimeSeriesGuarantee::final>> key, wp<TS<bool>> selected) {
                                  return switch_(selected,
                                                 cases{case_(true, first_route), case_(false, second_route)}, key);
                              });

    const auto copy = node([](In<TSD<i64, TS<i64>>> values, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(values);
    });

    const auto tested = [&](wp<TS<bool>> selected) {
        return copy(map(client, keys(), pass_through(selected), "__key_arg__"_n = 0));
    };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue},
                    {true, literal::SKIP, false, literal::SKIP, true, literal::SKIP})
              .equals({{{1, literal::INVALID}, {2, literal::INVALID}},
                       {{1, 11}, {2, 21}},
                       {{1, literal::INVALID}, {2, literal::INVALID}},
                       {{1, 12}, {2, 22}},
                       {{1, literal::INVALID}, {2, literal::INVALID}},
                       {{1, 11}, {2, 21}}}));
}
