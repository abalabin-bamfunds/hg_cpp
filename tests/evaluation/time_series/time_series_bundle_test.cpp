#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tsl/tsl_output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <vector>

namespace {

template <template <typename> typename Field> struct BaseBundle {
    Field<bg::TS<int>> count;
};

template <template <typename> typename Field> struct DerivedBundle : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<float>> value;
};

struct bundle_source_definition {
    bg::Out<bg::TSB<DerivedBundle>> output;

    void start() const {
        output->count.set(2);
        output->value.set(3.5F);
    }
};

struct bundle_sink_definition {
    bg::In<bg::TSB<DerivedBundle>> input;

    void evaluate() const {
        values = {static_cast<float>(*input->count), *input->value};
        updated = {input.item_updated(0), input.item_updated(1)};
    }

    inline static std::array<float, 2> values{};
    inline static std::array<bool, 2> updated{};
};

struct base_bundle_value_definition {
    bg::In<bg::TSB<BaseBundle>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input->count); }
};

struct bundle_sum_definition {
    bg::In<bg::TSB<DerivedBundle>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(static_cast<float>(*input->count) + *input->value); }
};

struct bundle_list_source_definition {
    bg::Out<bg::TSL<bg::TSB<DerivedBundle>, 2>> output;

    void start() const { output[1]->value.set(4.5F); }
};

} // namespace

TEST_CASE("a typed bundle publishes inherited and direct fields",
          "[evaluation][tsb][series:tsb][feature:collection-change][transition:update][scalar:integer]"
          "[scalar:floating-point][representation:native][declaration:concrete][binding:direct]"
          "[schema:named]") {
    const auto source = bg::node<bundle_source_definition>();
    const auto sink = bg::node<bundle_sink_definition>();

    bg::run_graph([&] { sink(source()); });

    CHECK(bundle_sink_definition::values == std::array{2.0F, 3.5F});
    CHECK(bundle_sink_definition::updated == std::array{true, true});
}

TEST_CASE("fixed time series lists can contain runtime-sized time series",
          "[evaluation][tsl][series:fixed-tsl][tsb][series:tsb][feature:collection-change]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct][schema:named]") {
    const auto source = bg::node<bundle_list_source_definition>();
    const auto value = bg::node([](bg::In<bg::TSL<bg::TSB<DerivedBundle>, 2>> input, bg::Out<bg::TS<float>> output) {
        output.set(*input[1]->value);
    });
    const auto graph = [&] { return value(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({4.5F}));
}

TEST_CASE("node code selects a named bundle field",
          "[evaluation][tsb][feature:getitem][series:tsb][schema:named][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<bundle_source_definition>();
    const auto count =
        bg::node([](bg::In<bg::TSB<DerivedBundle>> input, bg::Out<bg::TS<int>> output) { output.set(*input->count); });
    const auto graph = [&] { return count(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({2}));
}

TEST_CASE("a base bundle input accepts a derived bundle",
          "[evaluation][tsb][series:tsb][inheritance][feature:collection-change][scalar:integer]"
          "[scalar:floating-point][representation:native][declaration:concrete][binding:covariant]"
          "[schema:named]") {
    const auto source = bg::node<bundle_source_definition>();
    const auto value = bg::node<base_bundle_value_definition>();
    const auto graph = [&] { return value(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({2}));
}

TEST_CASE("a typed TSB wiring point selects an inherited field",
          "[evaluation][series:tsb][feature:getattr][schema:named][representation:native]"
          "[transition:update]") {
    const auto source = bg::node<bundle_source_definition>();
    const auto graph = [&] { return source()->count; };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({2}));
}

TEST_CASE("a typed free TSB wiring point selects its original source",
          "[evaluation][series:tsb][feature:getattr][schema:named][representation:free]"
          "[transition:update]") {
    using namespace bg::literals;

    const auto graph = [](bg::wp<bg::TS<int>> count, bg::wp<bg::TS<float>> value) {
        return bg::bundle("value"_n = value, "count"_n = count).as<bg::TSB<DerivedBundle>>()->count;
    };

    const auto actual = bg::testing::test_graph(graph, std::vector{1, 2, 3}, std::vector{0.5F, 1.5F, 2.5F});

    CHECK(actual.equals({1, 2, 3}));
}

TEST_CASE("a free bundle follows the receiving named schema",
          "[evaluation][tsb][series:tsb][free][representation:free][feature:collection-change]"
          "[scalar:integer][scalar:floating-point][declaration:concrete][binding:direct]") {
    using namespace bg::literals;
    const auto sum = bg::node<bundle_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<int>> count, bg::wp<bg::TS<float>> value) {
        return sum(bg::bundle("value"_n = value, "count"_n = count));
    };

    const auto result = bg::testing::test_graph(graph, std::vector{1, 2}, std::vector{0.5F, 1.5F});

    CHECK(result.equals({1.5F, 3.5F}));
}

TEST_CASE("a free bundle reuses materialization for the same input type",
          "[wiring][tsb][series:tsb][representation:free][feature:wiring][shape:fan-out]"
          "[schema:named][binding:representation]") {
    using namespace bg::literals;
    const auto count = bg::node([](bg::Out<bg::TS<int>>) {});
    const auto value = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto first_sum = bg::node<bundle_sum_definition>();
    const auto second_sum = bg::node<bundle_sum_definition>();
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto bundle = bg::bundle("value"_n = value(), "count"_n = count());
    (void)first_sum(bundle);
    (void)second_sum(bundle);

    REQUIRE(wiring.nodes().size() == 5);
    CHECK(wiring.nodes()[3].input_output_indices()[0] == wiring.nodes()[4].input_output_indices()[0]);
}

TEST_CASE("a free bundle materializes independently for different input schemas",
          "[wiring][tsb][series:tsb][representation:free][feature:wiring][schema:named]"
          "[binding:representation]") {
    using namespace bg::literals;
    const auto count_source = bg::node([](bg::Out<bg::TS<int>>) {});
    const auto value_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sum = bg::node<bundle_sum_definition>();
    const auto count = bg::node<base_bundle_value_definition>();
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto bundle = bg::bundle("value"_n = value_source(), "count"_n = count_source());
    (void)sum(bundle);
    (void)count(bundle);

    REQUIRE(wiring.nodes().size() == 6);
    CHECK(wiring.nodes()[3].input_output_indices()[0] != wiring.nodes()[5].input_output_indices()[0]);
}

TEST_CASE("a free bundle requires every field of the receiving schema",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    using namespace bg::literals;
    const auto count = bg::node([](bg::Out<bg::TS<int>>) {});
    const auto sum = bg::node<bundle_sum_definition>();

    CHECK_THROWS(bg::run_graph([&] { return sum(bg::bundle("count"_n = count())); }));
}

TEST_CASE("a free bundle requires compatible field types",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    using namespace bg::literals;
    const auto count = bg::node([](bg::Out<bg::TS<int>>) {});
    const auto value = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sum = bg::node<bundle_sum_definition>();

    CHECK_THROWS(bg::run_graph([&] { return sum(bg::bundle("count"_n = value(), "value"_n = count())); }));
}

TEST_CASE("a free bundle rejects duplicate field names",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    using namespace bg::literals;
    const auto count = bg::node([](bg::Out<bg::TS<int>>) {});
    const auto value = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sum = bg::node<bundle_sum_definition>();

    CHECK_THROWS(
        bg::run_graph([&] { return sum(bg::bundle("count"_n = count(), "count"_n = count(), "value"_n = value())); }));
}

TEST_CASE("a named bundle input accepts extra free bundle fields",
          "[evaluation][tsb][series:tsb][wiring][schema:named][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][feature:wiring]") {
    using namespace bg::literals;
    const auto sum = bg::node<bundle_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<int>> count, bg::wp<bg::TS<float>> value) {
        return sum(bg::bundle("extra"_n = count, "value"_n = value, "count"_n = count));
    };

    const auto result = bg::testing::test_graph(graph, std::vector{1, 2}, std::vector{0.5F, 1.5F});

    CHECK(result.equals({1.5F, 3.5F}));
}

namespace {

template <template <typename> typename Field> struct RedefinedBundle : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<int>> count;
    Field<bg::TS<float>> value;
};

struct redefined_bundle_source_definition {
    bg::Out<bg::TSB<RedefinedBundle>> output;

    void start() const {
        output->count.set(4);
        output->value.set(2.5F);
    }
};

} // namespace

TEST_CASE("a redefined bundle field shares its inherited storage",
          "[evaluation][tsb][series:tsb][inheritance][feature:collection-change][schema:named]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<redefined_bundle_source_definition>();
    const auto value = bg::node<base_bundle_value_definition>();
    const auto graph = [&] { return value(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({4}));
}

namespace {

template <template <typename> typename Field> struct NestedBundle {
    Field<bg::TSB<BaseBundle>> nested;
    Field<bg::TS<float>> value;
};

struct nested_bundle_source_definition {
    bg::Out<bg::TSB<NestedBundle>> output;

    void start() const {
        output->nested->count.set(3);
        output->value.set(1.5F);
    }
};

struct nested_bundle_sum_definition {
    bg::In<bg::TSB<NestedBundle>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(static_cast<float>(*input->nested->count) + *input->value); }
};

} // namespace

TEST_CASE("a bundle field can contain another bundle",
          "[evaluation][tsb][series:tsb][nested][feature:collection-change][schema:named]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<nested_bundle_source_definition>();
    const auto sum = bg::node<nested_bundle_sum_definition>();
    const auto graph = [&] { return sum(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({4.5F}));
}

namespace {

struct invalidating_bundle_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TSB<DerivedBundle>> output;

    void start(bg::EvaluationContext *context) const {
        output->count.set(2);
        output->value.set(3.5F);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct optional_bundle_sink_definition {
    bg::In<bg::TSB<DerivedBundle>, bg::optional> input;

    void evaluate() const { validity.push_back({input.valid(), input->count.valid(), input->value.valid()}); }

    inline static std::vector<std::array<bool, 3>> validity;
};

} // namespace

TEST_CASE("invalidating a bundle invalidates every field",
          "[evaluation][tsb][series:tsb][validity][feature:collection-change][transition:invalidation]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct][schema:named]") {
    const auto source = bg::node<invalidating_bundle_source_definition>();
    const auto sink = bg::node<optional_bundle_sink_definition>();
    optional_bundle_sink_definition::validity.clear();

    bg::run_graph([&] { sink(source()); });

    CHECK(optional_bundle_sink_definition::validity ==
          std::vector<std::array<bool, 3>>{{true, true, true}, {false, false, false}});
}
