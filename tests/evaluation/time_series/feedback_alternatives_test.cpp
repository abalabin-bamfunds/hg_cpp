#include <hg/feedback.hpp>
#include <hg/graph.hpp>
#include <hg/nested.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <hg/scalar/bundle/value.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace hg;
using namespace hg::literals;

template <template <typename, auto...> typename Field> struct FeedbackEvent {
    Field<symbol> kind;
};

template <typename Expected>
void check_event(const std::optional<ScalarValue<Bundle<FeedbackEvent>>> &actual,
                 const Expected &expected) {
    REQUIRE(actual.has_value());
    CHECK(&actual->type() == &expected.type());
    CHECK(actual->view().equals(expected));
}

}  // namespace

TEST_CASE("feedback delays a base Bundle by one cycle",
          "[feature:feedback][series:value][scalar:hierarchical][schema:named]"
          "[transition:first-tick][transition:update]") {
    const auto base = bundle<FeedbackEvent>(symbol{"base"});
    const auto graph = [](wp<TS<Bundle<FeedbackEvent>>> input) {
        auto delayed = feedback<TS<Bundle<FeedbackEvent>>>();
        delayed = input;
        return wp<TS<Bundle<FeedbackEvent>>>{delayed};
    };

    const auto actual = run_graph(graph, {base});

    REQUIRE(actual.size() == 2);
    CHECK_FALSE(actual[0].has_value());
    check_event(actual[1], base);
}

namespace {

template <template <typename, auto...> typename Field>
struct SmallFeedbackEvent : schema_extends<FeedbackEvent, Field> {
    Field<i64> sequence;
};

template <template <typename, auto...> typename Field>
struct LargeFeedbackEvent : schema_extends<SmallFeedbackEvent, Field> {
    Field<double> amount;
    Field<symbol> topic;
    Field<i64> detail_count;
};

const auto publish_event = node([](i64 selection,
                                   Scalar<Bundle<FeedbackEvent>> base,
                                   Scalar<Bundle<SmallFeedbackEvent>> small,
                                   Scalar<Bundle<LargeFeedbackEvent>> large,
                                   Out<TS<Bundle<FeedbackEvent>>> output) {
    switch (selection) {
        case 0: output.set(base); break;
        case 1: output.set(small); break;
        case 2: output.set(large); break;
    }
});

}  // namespace

TEST_CASE("feedback preserves a small derived Bundle",
          "[feature:feedback][series:value][scalar:hierarchical][schema:named]"
          "[binding:covariant][transition:update]") {
    const auto base = bundle<FeedbackEvent>(symbol{"base"});
    const auto small = bundle<SmallFeedbackEvent>(symbol{"small"}, i64{17});
    const auto large = bundle<LargeFeedbackEvent>(symbol{"large"}, i64{99}, 3.5, symbol{"unused"}, i64{4});

    const auto graph = [&](wp<TS<i64>> selection) {
        auto delayed = feedback<TS<Bundle<FeedbackEvent>>>();
        delayed = publish_event(selection, base, small, large);
        return wp<TS<Bundle<FeedbackEvent>>>{delayed};
    };

    const auto actual = run_graph(graph, {1});

    REQUIRE(actual.size() == 2);
    CHECK_FALSE(actual[0].has_value());
    check_event(actual[1], small);
}

TEST_CASE("feedback preserves a large derived Bundle",
          "[feature:feedback][series:value][scalar:hierarchical][schema:named]"
          "[binding:covariant][transition:update]") {
    const auto base = bundle<FeedbackEvent>(symbol{"base"});
    const auto small = bundle<SmallFeedbackEvent>(symbol{"small"}, i64{17});
    const auto large = bundle<LargeFeedbackEvent>(symbol{"large"}, i64{99}, 3.5, symbol{"orders"}, i64{4});

    const auto graph = [&](wp<TS<i64>> selection) {
        auto delayed = feedback<TS<Bundle<FeedbackEvent>>>();
        delayed = publish_event(selection, base, small, large);
        return wp<TS<Bundle<FeedbackEvent>>>{delayed};
    };

    const auto actual = run_graph(graph, {2});

    REQUIRE(actual.size() == 2);
    CHECK_FALSE(actual[0].has_value());
    check_event(actual[1], large);
}

TEST_CASE("feedback switches concrete Bundle alternatives across sparse updates",
          "[feature:feedback][series:value][scalar:hierarchical][schema:named]"
          "[binding:covariant][transition:first-tick][transition:update]") {
    const auto base = bundle<FeedbackEvent>(symbol{"base"});
    const auto small = bundle<SmallFeedbackEvent>(symbol{"small"}, i64{17});
    const auto large = bundle<LargeFeedbackEvent>(symbol{"large"}, i64{99}, 3.5, symbol{"orders"}, i64{4});

    const auto graph = [&](wp<TS<i64>> selection) {
        auto delayed = feedback<TS<Bundle<FeedbackEvent>>>();
        delayed = publish_event(selection, base, small, large);
        return wp<TS<Bundle<FeedbackEvent>>>{delayed};
    };

    const auto actual = run_graph(graph, {0, literal::SKIP, 2, literal::SKIP, 1});

    REQUIRE(actual.size() == 6);
    CHECK_FALSE(actual[0].has_value());
    check_event(actual[1], base);
    CHECK_FALSE(actual[2].has_value());
    check_event(actual[3], large);
    CHECK_FALSE(actual[4].has_value());
    check_event(actual[5], small);
}

TEST_CASE("feedback preserves a derived Bundle sampled through a reference",
          "[feature:feedback][feature:reference][series:value][scalar:hierarchical]"
          "[schema:named][binding:reference-to-value][transition:update]") {
    const auto base = bundle<FeedbackEvent>(symbol{"base"});
    const auto small = bundle<SmallFeedbackEvent>(symbol{"small"}, i64{17});
    const auto large = bundle<LargeFeedbackEvent>(symbol{"large"}, i64{99}, 3.5, symbol{"orders"}, i64{4});
    const auto copy_value = node([](In<TS<Bundle<FeedbackEvent>>> input,
                                    Out<TS<Bundle<FeedbackEvent>>> output) {
        output.set(input.value());
    });

    const auto graph = [&](wp<TS<i64>> selection) {
        auto delayed = feedback<TS<Bundle<FeedbackEvent>>>();
        const auto reference = publish_event(selection, base, small, large)
                                   .as<REF<TS<Bundle<FeedbackEvent>>>>();
        delayed = copy_value(reference);
        return wp<TS<Bundle<FeedbackEvent>>>{delayed};
    };

    const auto actual = run_graph(graph, {2, 1});

    REQUIRE(actual.size() == 3);
    CHECK_FALSE(actual[0].has_value());
    check_event(actual[1], large);
    check_event(actual[2], small);
}

TEST_CASE("a nested graph delays and preserves derived Bundle alternatives",
          "[feature:feedback][feature:nested-graph][series:value][scalar:hierarchical]"
          "[schema:named][binding:covariant][transition:update]") {
    const auto base = bundle<FeedbackEvent>(symbol{"base"});
    const auto small = bundle<SmallFeedbackEvent>(symbol{"small"}, i64{17});
    const auto large = bundle<LargeFeedbackEvent>(symbol{"large"}, i64{99}, 3.5, symbol{"orders"}, i64{4});
    const auto delayed_graph = graph(
        "name"_n = "feedback_bundle_nested",
        [](wp<TS<Bundle<FeedbackEvent>>> input) {
            auto delayed = feedback<TS<Bundle<FeedbackEvent>>>();
            delayed = input;
            return wp<TS<Bundle<FeedbackEvent>>>{delayed};
        });
    const auto tested = [&](wp<TS<i64>> selection) {
        return nested(delayed_graph, operators::const_(true), publish_event(selection, base, small, large));
    };

    const auto actual = run_graph(tested, {1, 2});

    REQUIRE(actual.size() == 3);
    CHECK_FALSE(actual[0].has_value());
    check_event(actual[1], small);
    check_event(actual[2], large);
}
