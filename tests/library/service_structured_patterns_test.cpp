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

#include <catch2/catch_test_macros.hpp>

using namespace hg;
using namespace hg::literals;
using namespace hg::util::literals;

namespace {

template <template <typename, auto...> typename Field> struct PairReply {
    Field<i64> left;
    Field<i64> right;
};

} // namespace

TEST_CASE("a request reply service returns a multi-field bundle",
          "[feature:service][series:tsb][schema:named][shape:nested]") {
    const auto combine = service<auto(TS<i64>, TS<i64>)->TSB<PairReply>>{"combine", {"left", "right"}};
    const auto make_reply = graph("name"_n = "make_pair_reply", [](wp<TS<i64>> left, wp<TS<i64>> right) {
        return bundle<PairReply>("left"_n = left, "right"_n = right);
    });

    const auto implementation =
        graph("name"_n = "combine_impl",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "left"_n> left,
                  wp<TSD<service_client_id_t, TS<i64>>, "right"_n> right) { return map(make_reply, left, right); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(combine, implementation);

    const auto tested = [&](wp<TS<i64>> left, wp<TS<i64>> right) { return combine(left, right); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2, literal::SKIP}, {10, literal::SKIP, 20})
              .equals({{{"left", 1}, {"right", 10}}, {{"left", 2}}, {{"right", 20}}}));
}

TEST_CASE("a reference service preserves partial native bundle field updates",
          "[feature:service][feature:reference][series:tsb][schema:named][representation:native]"
          "[binding:reference-to-value][transition:update]") {
    const auto snapshot = reference_service<auto()->TSB<PairReply>>{"pair_snapshot"};
    const auto copy_fields = node([](In<TS<i64>> left, In<TS<i64>> right, Out<TSB<PairReply>> output) {
        output->left.copy_tick(left);
        output->right.copy_tick(right);
    });

    const auto implementation = graph("name"_n = "pair_snapshot_impl",
                                      [&](wp<TS<i64>> left, wp<TS<i64>> right) { return copy_fields(left, right); });
    auto catalogue = ServiceCatalogue{};

    const auto tested = [&](wp<TS<i64>> left, wp<TS<i64>> right) {
        catalogue.register_impl(snapshot, implementation, left, right);
        return snapshot();
    };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2, literal::SKIP}, {10, literal::SKIP, 20})
              .equals({{{"left", 1}, {"right", 10}}, {{"left", 2}}, {{"right", 20}}}));
}

TEST_CASE("a reference service preserves reference leaves in a free bundle",
          "[feature:service][feature:reference][series:tsb][schema:named][representation:free]"
          "[binding:reference-to-reference][transition:update]") {
    const auto snapshot = reference_service<auto()->TSB<PairReply>>{"free_pair_snapshot"};
    const auto implementation = graph("name"_n = "free_pair_snapshot_impl", [](wp<TS<i64>> left, wp<TS<i64>> right) {
        return bundle<PairReply>("left"_n = left.as<REF<TS<i64>>>(), "right"_n = right.as<REF<TS<i64>>>());
    });
    auto catalogue = ServiceCatalogue{};

    const auto tested = [&](wp<TS<i64>> left, wp<TS<i64>> right) {
        catalogue.register_impl(snapshot, implementation, left, right);
        return snapshot();
    };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2, literal::SKIP}, {10, literal::SKIP, 20})
              .equals({{{"left", 1}, {"right", 10}}, {{"left", 2}}, {{"right", 20}}}));
}

const auto copy_pair_dictionary =
    node([](In<TSD<i64, TSB<PairReply>>> input, Out<TSD<i64, TSB<PairReply>>> output) { output.copy_tick(input); });

TEST_CASE("mapped request reply clients keep results independent through removal and recreation",
          "[feature:service][feature:map][feature:nested-graph]"
          "[series:tsd][series:tsb][schema:named][transition:removal]") {
    const auto combine = service<auto(TS<i64>)->TSB<PairReply>>{"mapped_combine", {"value"}};
    const auto make_reply = graph("name"_n = "make_mapped_pair_reply", [](wp<TS<i64>> value) {
        return bundle<PairReply>("left"_n = value, "right"_n = value);
    });

    const auto implementation =
        graph("name"_n = "mapped_combine_impl",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) { return map(make_reply, values); });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(combine, feedback_service_implementation(combine, implementation));

    const auto client = graph("name"_n = "mapped_combine_client", [&](wp<TS<i64>> value) { return combine(value); });
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) { return copy_pair_dictionary(map(client, values)); };

    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue},
                                  {{{1, 3}}, {{2, 4}}, {{1, literal::REMOVE}}, {{1, 5}}});

    CHECK(actual.equals({{{1, literal::INVALID}},
                         {{2, literal::INVALID}, {1, {{"left", 3}, {"right", 3}}}},
                         {{1, literal::REMOVE}, {2, {{"left", 4}, {"right", 4}}}},
                         {{1, literal::INVALID}},
                         {{1, {{"left", 5}, {"right", 5}}}}}));
}

TEST_CASE("a switched mapped service client cancels and recreates a structured reply",
          "[feature:service][feature:map][feature:switch][feature:nested-graph]"
          "[series:tsd][series:tsb][schema:named][transition:removal][transition:addition]") {
    const auto echo = service<auto(TS<i64>)->TSB<PairReply>>{"switched_echo", {"value"}};
    const auto make_reply = graph("name"_n = "make_switched_pair", [](wp<TS<i64>> value) {
        return bundle<PairReply>("left"_n = value, "right"_n = value);
    });

    const auto implementation =
        graph("name"_n = "switched_echo_impl",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) { return map(make_reply, values); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(echo, feedback_service_implementation(echo, implementation));

    const auto service_branch =
        graph("name"_n = "switched_service_branch", [&](wp<TS<i64>> value) { return echo(value); });
    const auto multiply_by_ten = node([](i64 value) { return value * 10; });

    const auto local_branch = graph("name"_n = "switched_local_branch", [&](wp<TS<i64>> value) {
        return bundle<PairReply>("left"_n = value, "right"_n = multiply_by_ten(value));
    });

    const auto per_key = graph("name"_n = "switched_pair_client", [&](wp<TS<i64>> value, wp<TS<bool>> selected) {
        return switch_(selected, cases{case_(true, service_branch), case_(false, local_branch)}, value);
    });
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TSD<i64, TS<bool>>> selected) {
        return copy_pair_dictionary(map(per_key, values, selected, "__valid_items__"_n = true));
    };

    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue},
                                  {{{1, 3}, {2, 4}}, literal::SKIP, literal::SKIP, literal::SKIP},
                                  {{{1, false}, {2, true}}, {{1, true}}, {{1, false}}, {{1, true}}});

    CHECK(actual.equals({{{1, {{"left", 3}, {"right", 30}}}},
                         {{1, literal::REMOVE}, {2, {{"left", 4}, {"right", 4}}}},
                         {{1, {{"left", 3}, {"right", 30}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{"left", 3}, {"right", 3}}}}}));
}

TEST_CASE("independent request reply clients update their own structured fields",
          "[feature:service][series:tsb][schema:named][shape:multiple][transition:update]") {
    const auto echo = service<auto(TS<i64>)->TSB<PairReply>>{"echo_pair", {"value"}};
    const auto make_reply = graph("name"_n = "make_echo_pair", [](wp<TS<i64>> value) {
        return bundle<PairReply>("left"_n = value, "right"_n = value);
    });

    const auto implementation =
        graph("name"_n = "echo_pair_impl",
              [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) { return map(make_reply, values); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(echo, implementation);

    const auto sum = node([](i64 left, i64 right) { return left + right; });
    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(operators::getattr(echo(first), "left"_sym), operators::getattr(echo(second), "right"_sym));
    };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, literal::SKIP, 2}, {10, 20, literal::SKIP})
              .equals<i64>({11, 21, 22}));
}

TEST_CASE("a late subscription client samples an existing structured reply",
          "[feature:service][feature:map][feature:nested-graph]"
          "[series:tss][series:tsd][series:tsb][schema:named][transition:addition]") {
    const auto lookup =
        subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TSB<PairReply>>{"pair_lookup", {"key"}};
    const auto multiply_by_ten = node([](i64 value) { return value * 10; });

    const auto make_reply = graph("name"_n = "make_lookup_pair", [&](wp<TS<i64>> key) {
        return bundle<PairReply>("left"_n = key, "right"_n = multiply_by_ten(key));
    });

    const auto implementation = graph("name"_n = "pair_lookup_impl", [&](wp<TSS<i64>> keys) {
        return map(make_reply, "__keys__"_n = keys, "__key_arg__"_n = 0);
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(lookup, feedback_service_implementation(lookup, implementation));

    const auto final_key = node([](i64 key, Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(key); });

    const auto client = graph("name"_n = "pair_lookup_client", [&](wp<TS<i64>> key) { return lookup(final_key(key)); });
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) { return copy_pair_dictionary(map(client, values)); };

    const auto actual = run_graph(tested, GraphRunOptions{.services = &catalogue},
                                  {{{1, 7}}, literal::SKIP, {{2, 7}}, {{1, literal::REMOVE}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals({{{1, literal::INVALID}},
                         {{1, {{"left", 7}, {"right", 70}}}},
                         {{2, {{"left", 7}, {"right", 70}}}},
                         {{1, literal::REMOVE}},
                         {{2, literal::REMOVE}}}));
}
