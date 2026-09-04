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

struct base_bundle_sink_definition {
    bg::In<bg::TSB<BaseBundle>> input;

    void evaluate() const { value = *input->count; }

    inline static int value{};
};

struct bundle_sum_definition {
    bg::In<bg::TSB<DerivedBundle>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(static_cast<float>(*input->count) + *input->value); }
};

struct bundle_item_sink_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const { value = *input; }

    inline static int value{};
};

struct changing_bundle_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TSB<DerivedBundle>> output;

    void start(bg::EvaluationContext *context) const {
        output->count.set(2);
        output->value.set(3.5F);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output->value.set(4.5F); }
};

struct changing_bundle_sink_definition {
    bg::In<bg::TSB<DerivedBundle>> input;

    void evaluate() const { updates.push_back({input.item_updated(0), input.item_updated(1)}); }

    inline static std::vector<std::array<bool, 2>> updates;
};

struct count_updates_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const { ++count; }

    inline static int count{};
};

using BundleList = bg::TSL<bg::TSB<DerivedBundle>, 2>;

struct bundle_list_source_definition {
    bg::Out<BundleList> output;

    void start() const { output[1]->value.set(4.5F); }
};

struct bundle_list_sink_definition {
    bg::In<BundleList> input;

    void evaluate() const { value = *input[1]->value; }

    inline static float value{};
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
    const auto sink = bg::node<bundle_list_sink_definition>();
    bundle_list_sink_definition::value = 0.0F;

    bg::run_graph([&] { sink(source()); });

    CHECK(bundle_list_sink_definition::value == 4.5F);
}

TEST_CASE("graph code selects a named bundle field",
          "[evaluation][tsb][series:tsb][wiring][schema:named][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][feature:wiring]") {
    using namespace bg::literals;
    const auto source = bg::node<bundle_source_definition>();
    const auto sink = bg::node<bundle_item_sink_definition>();

    bg::run_graph([&] { sink(source()["count"_m]); });

    CHECK(bundle_item_sink_definition::value == 2);
}

TEST_CASE("a base bundle input accepts a derived bundle",
          "[evaluation][tsb][series:tsb][inheritance][feature:collection-change][scalar:integer]"
          "[scalar:floating-point][representation:native][declaration:concrete][binding:covariant]"
          "[schema:named]") {
    const auto source = bg::node<bundle_source_definition>();
    const auto sink = bg::node<base_bundle_sink_definition>();

    bg::run_graph([&] { sink(source()); });

    CHECK(base_bundle_sink_definition::value == 2);
}

TEST_CASE("a bundle schedules only subscribers of changed fields",
          "[evaluation][tsb][series:tsb][scheduling][feature:collection-change][transition:update]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct][schema:named]") {
    using namespace bg::literals;
    const auto source = bg::node<changing_bundle_source_definition>();
    const auto bundle_sink = bg::node<changing_bundle_sink_definition>();
    const auto count_sink = bg::node<count_updates_definition>();
    changing_bundle_sink_definition::updates.clear();
    count_updates_definition::count = 0;

    bg::run_graph([&] {
        const auto value = source();
        bundle_sink(value);
        count_sink(value["count"_m]);
    });

    CHECK(changing_bundle_sink_definition::updates == std::vector<std::array<bool, 2>>{{true, true}, {false, true}});
    CHECK(count_updates_definition::count == 1);
}

TEST_CASE("a free bundle follows the receiving named schema",
          "[evaluation][tsb][series:tsb][free][representation:free][feature:collection-change]"
          "[scalar:integer][scalar:floating-point][declaration:concrete][binding:direct]") {
    using namespace bg::literals;
    const auto sum = bg::node<bundle_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<int>> count, bg::wp<bg::TS<float>> value) {
        return sum(bg::bundle("value"_m = value, "count"_m = count));
    };

    const auto result = bg::testing::test_graph(graph, std::vector{1, 2}, std::vector{0.5F, 1.5F});

    CHECK(result.equals({1.5F, 3.5F}));
}

TEST_CASE("graph code selects a field directly from a free bundle",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    using namespace bg::literals;
    const auto graph = [](bg::wp<bg::TS<int>> count, bg::wp<bg::TS<float>> value) {
        return bg::bundle("value"_m = value, "count"_m = count)["count"_m];
    };

    const auto result = bg::testing::test_graph(graph, std::vector{1, 2}, std::vector{0.5F, 1.5F});

    CHECK(result.equals({1, 2}));
}

TEST_CASE("a free bundle requires every field of the receiving schema",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    using namespace bg::literals;
    const auto source = bg::node<bundle_source_definition>();
    const auto sum = bg::node<bundle_sum_definition>();

    CHECK_THROWS(bg::run_graph([&] { return sum(bg::bundle("count"_m = source()["count"_m])); }));
}

TEST_CASE("a free bundle requires compatible field types",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    using namespace bg::literals;
    const auto source = bg::node<bundle_source_definition>();
    const auto sum = bg::node<bundle_sum_definition>();

    CHECK_THROWS(bg::run_graph([&] {
        const auto value = source();
        return sum(bg::bundle("count"_m = value["value"_m], "value"_m = value["count"_m]));
    }));
}

TEST_CASE("a free bundle rejects duplicate field names",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    using namespace bg::literals;
    const auto source = bg::node<bundle_source_definition>();
    const auto sum = bg::node<bundle_sum_definition>();

    CHECK_THROWS(bg::run_graph([&] {
        const auto value = source();
        return sum(
            bg::bundle("count"_m = value["count"_m], "count"_m = value["count"_m], "value"_m = value["value"_m]));
    }));
}

TEST_CASE("a named bundle input accepts extra free bundle fields",
          "[evaluation][tsb][series:tsb][wiring][schema:named][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][feature:wiring]") {
    using namespace bg::literals;
    const auto sum = bg::node<bundle_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<int>> count, bg::wp<bg::TS<float>> value) {
        return sum(bg::bundle("extra"_m = count, "value"_m = value, "count"_m = count));
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
    const auto sink = bg::node<base_bundle_sink_definition>();

    bg::run_graph([&] { sink(source()); });

    CHECK(base_bundle_sink_definition::value == 4);
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

struct nested_bundle_sink_definition {
    bg::In<bg::TSB<NestedBundle>> input;

    void evaluate() const { value = static_cast<float>(*input->nested->count) + *input->value; }

    inline static float value{};
};

} // namespace

TEST_CASE("a bundle field can contain another bundle",
          "[evaluation][tsb][series:tsb][nested][feature:collection-change][schema:named]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<nested_bundle_source_definition>();
    const auto sink = bg::node<nested_bundle_sink_definition>();

    bg::run_graph([&] { sink(source()); });

    CHECK(nested_bundle_sink_definition::value == 4.5F);
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
