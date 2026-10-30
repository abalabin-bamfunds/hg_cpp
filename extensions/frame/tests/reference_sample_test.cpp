#include <catch2/catch_test_macros.hpp>

#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/nested.hpp>
#include <bg/run_graph.hpp>
#include <bg/util/scope_exit.hpp>

#include <array>
#include <optional>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct SampleFrameBase {
    Field<i64> id;
    Field<double> price;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <template <typename, auto...> typename Field>
struct SampleFrameDerived : schema_extends<SampleFrameBase, Field> {
    Field<double> bid;
};

template <template <typename, auto...> typename Field> struct SampleSharedFrame {
    Field<i64> left;
    Field<i64> right;
};

template <template <typename, auto...> typename Field> struct SampleEmptyFrame {};

TEST_CASE("TSF reference switch samples actual derived fields and shared column masks",
          "[feature:frame][series:reference][feature:sampling]") {
    struct Observation {
        bool valid{};
        bool is_modified{};
        EvaluationTime last_updated{MIN_DT};
        std::size_t field_count{};
        bool derived_column_type_resolved{};
        bool columns_share_row_masks{};
        std::array<bool, 3> column_modified{};
        std::size_t nested_field_count{};
        bool nested_derived_column_type_resolved{};
        std::optional<double> nested_bid;
        std::optional<i64> id;
        std::optional<double> price;
        std::optional<double> bid;
    };

    auto observations = std::vector<Observation>{};
    const auto choose = node([](In<TS<bool>> select, In<REF<TSF<SampleFrameBase>>> first,
                                In<REF<TSF<SampleFrameBase>>> second, Out<REF<TSF<SampleFrameBase>>> output) {
        output.copy(*select ? second : first);
    });
    const auto observe = node([&](In<TSF<SampleFrameBase>> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = Observation{.valid = input.valid(), .is_modified = input.modified(),
                                  .last_updated = input.last_updated()};
        if (result.valid) {
            result.field_count = input.schema().field_count();
            result.derived_column_type_resolved = input.runtime_type().item_runtime_type(2).type().is_resolved();
            result.column_modified = {input.column(0).modified(), input.column(1).modified(),
                                      input.column(2).modified()};
            const auto id = input.column(0).input<i64>().extension_descriptor().storage;
            const auto price = input.column(1).input<double>().extension_descriptor().storage;
            const auto bid = input.column(2).input<double>().extension_descriptor().storage;
            result.columns_share_row_masks = id.added == price.added && id.added == bid.added &&
                                             id.modified == price.modified && id.modified == bid.modified &&
                                             id.removed == price.removed && id.removed == bid.removed;
            if (input.contains(0)) {
                result.id = input.cell<0>(0);
                result.price = input.cell<1>(0);
                result.bid = input.column("bid").input<double>().cell(0);
            }
            if (*cycle == 3) {
                const auto source = bg::details::ResolvedTimeSeries{input.input_data().last_updated_ref(),
                                                                    input.input_data().data(),
                                                                    std::addressof(input.runtime_type())};
                const auto factory = input.runtime_type().reference_sample_factory();
                auto resource = std::pmr::monotonic_buffer_resource{};
                auto memory = allocator{resource};
                auto *nested_sample = factory(input.runtime_type().type(), source, source,
                                              input.last_updated(), memory);
                if (nested_sample != nullptr) {
                    const auto release = util::scope_exit{[&]() noexcept { nested_sample->release(); }};
                    const auto nested_item = nested_sample->input();
                    const auto &nested_runtime = static_cast<const FrameSeriesRuntimeType &>(nested_item.runtime_type);
                    const auto nested_storage = nested_runtime.input_storage(nested_item.value);
                    result.nested_field_count = nested_storage.schema->field_count();
                    result.nested_derived_column_type_resolved =
                        nested_item.runtime_type.item_runtime_type(2).type().is_resolved();
                    const auto bid = nested_item.runtime_type.item(nested_item.value, 2);
                    const auto bid_input = In<TSC<double>>{
                        TimeSeriesInputData{bid.runtime_type, bid.last_updated, bid.value, true}};
                    if (bid_input.valid() && bid_input.contains(0))
                        result.nested_bid = bid_input.cell(0);
                }
            }
        }
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto inner = graph(
        "name"_n = "sample_frame_reference_switch",
        [&](wp<TS<bool>> select, wp<TSF<SCHEMA>> first, wp<TSF<SCHEMA>> second,
            wp<TS<i64>> cycle) { return observe(choose(select, first, second), cycle); });
    const auto graph = [&](wp<TS<bool>> run, wp<TS<bool>> select, wp<TSF<SCHEMA>> first,
                           wp<TSF<SCHEMA>> second, wp<TS<i64>> cycle) {
        return nested(inner, run, select, first, second, cycle);
    };

    const auto &runtime = frame_time_series_runtime_type<SampleFrameDerived>(TSF<SampleFrameDerived>::type());
    static_cast<void>(run_graph(
        graph, std::array{true, true, true, true, true}, std::array{false, false, true, false, false},
        bg::input(runtime, {literal::TimeSeriesLiteral{}, {{{"id", i64{7}}, {"price", 10.0}, {"bid", 20.0}}}}),
        bg::input(runtime, {literal::TimeSeriesLiteral{}, {{{"id", i64{8}}, {"price", 11.0}, {"bid", 21.0}}}}),
        std::array<i64, 5>{0, 1, 2, 3, 4}));

    REQUIRE(observations.size() == 5);
    CHECK(observations[2].valid);
    CHECK(observations[2].field_count == 3);
    CHECK(observations[2].derived_column_type_resolved);
    CHECK(observations[2].columns_share_row_masks);
    CHECK(observations[2].column_modified == std::array{true, true, true});
    CHECK(observations[2].id == 8);
    CHECK(observations[2].price == 11.0);
    CHECK(observations[2].bid == 21.0);
    CHECK(observations[3].valid);
    CHECK(observations[3].is_modified);
    CHECK(observations[3].column_modified == std::array{true, true, true});
    CHECK(observations[3].id == 7);
    CHECK(observations[3].nested_field_count == 3);
    CHECK(observations[3].nested_derived_column_type_resolved);
    CHECK(observations[3].nested_bid == 20.0);
    CHECK(observations[4].valid);
    CHECK_FALSE(observations[4].is_modified);
    CHECK(observations[4].column_modified == std::array{false, false, false});
    CHECK(observations[4].last_updated == observations[3].last_updated);
    CHECK(observations[4].id == 7);
}

TEST_CASE("TSC reference switch separates colliding row IDs across domains",
          "[feature:frame][series:reference][feature:sampling]") {
    struct Observation {
        bool valid{};
        bool is_modified{};
        EvaluationTime last_updated{MIN_DT};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> added;
        std::vector<row_id_t> modified;
        std::vector<row_id_t> removed;
        bool sampled_guarantee_is_ordinary{};
        std::optional<i64> value;
        bool copied_valid{};
        std::optional<i64> copied_value;
    };

    auto observations = std::vector<Observation>{};
    const auto choose = node([](In<TS<bool>> select, In<REF<TSC<i64>>> first, In<REF<TSC<i64>>> second,
                                Out<REF<TSC<i64>>> output) { output.copy(*select ? second : first); });
    const auto copy = node([](In<TSC<i64>> input, Out<TSC<i64>> output) { output.copy_tick(input); });
    const auto observe = node([&](In<TSC<i64>> input, In<TSC<i64>, optional> copied, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        auto result = Observation{.valid = input.valid(), .is_modified = input.modified(),
                                  .last_updated = input.last_updated()};
        if (result.valid) {
            input.for_each_row([&](row_id_t id, std::size_t) { result.rows.push_back(id); });
            const auto storage = input.extension_descriptor().storage;
            if (storage.added != nullptr)
                for (const auto id : *storage.added)
                    result.added.push_back(id);
            if (storage.modified != nullptr)
                for (const auto id : *storage.modified)
                    result.modified.push_back(id);
            if (storage.removed != nullptr)
                for (const auto id : *storage.removed)
                    result.removed.push_back(id);
            result.sampled_guarantee_is_ordinary =
                input.runtime_type().type().guarantee().value() == TimeSeriesGuarantee::ordinary;
            if (input.contains(0))
                result.value = input.cell(0);
        }
        result.copied_valid = copied.valid();
        if (result.copied_valid)
            copied.for_each_row([&](row_id_t id, std::size_t) { result.copied_value = copied.cell(id); });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<bool>> select, wp<TSC<i64>> first, wp<TSC<i64>> second, wp<TS<i64>> cycle) {
        const auto selected = choose(select, first, second);
        return observe(selected, copy(selected), cycle);
    };

    const auto first = input<TSC<i64>>(
        {literal::TimeSeriesLiteral{}, {10}, literal::SKIP, literal::SKIP, literal::SKIP});
    const auto second = input<TSC<i64>>(
        {literal::TimeSeriesLiteral{}, {20}, literal::SKIP, literal::SKIP, literal::SKIP});
    static_cast<void>(run_graph(graph, std::array{false, false, true, false, false}, first, second,
                                std::array<i64, 5>{0, 1, 2, 3, 4}));

    REQUIRE(observations.size() == 5);
    CHECK(observations[2].valid);
    CHECK(observations[2].rows == std::vector<row_id_t>{0});
    CHECK(observations[2].added == std::vector<row_id_t>{0});
    CHECK(observations[2].modified.empty());
    CHECK(observations[2].removed == std::vector<row_id_t>{0});
    CHECK(observations[2].sampled_guarantee_is_ordinary);
    CHECK(observations[2].value == 20);
    CHECK(observations[2].copied_valid);
    CHECK(observations[2].copied_value == 20);
    CHECK(observations[3].valid);
    CHECK(observations[3].is_modified);
    CHECK(observations[3].value == 10);
    CHECK(observations[4].valid);
    CHECK_FALSE(observations[4].is_modified);
    CHECK(observations[4].last_updated == observations[3].last_updated);
    CHECK(observations[4].value == 10);
    CHECK(observations[4].copied_valid);
    CHECK(observations[4].copied_value == 10);
}

TEST_CASE("TSC reference switch marks common rows modified in a shared domain",
          "[feature:frame][series:reference][feature:sampling]") {
    struct Observation {
        bool valid{};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> added;
        std::vector<row_id_t> modified;
        std::vector<row_id_t> removed;
        std::optional<i64> value;
    };

    auto observations = std::vector<Observation>{};
    const auto left_column = node([](In<TSF<SampleSharedFrame>> input, Out<TSC<i64>, frame::view> output) {
        output.bind(input.column("left").input<i64>());
    });
    const auto right_column = node([](In<TSF<SampleSharedFrame>> input, Out<TSC<i64>, frame::view> output) {
        output.bind(input.column("right").input<i64>());
    });
    const auto choose = node([](In<TS<bool>> select, In<REF<TSC<i64>>> left, In<REF<TSC<i64>>> right,
                                Out<REF<TSC<i64>>> output) { output.copy(*select ? right : left); });
    const auto observe = node([&](In<TSC<i64>> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = Observation{.valid = input.valid()};
        if (result.valid) {
            input.for_each_row([&](row_id_t id, std::size_t) { result.rows.push_back(id); });
            const auto storage = input.extension_descriptor().storage;
            if (storage.added != nullptr)
                for (const auto id : *storage.added)
                    result.added.push_back(id);
            if (storage.modified != nullptr)
                for (const auto id : *storage.modified)
                    result.modified.push_back(id);
            if (storage.removed != nullptr)
                for (const auto id : *storage.removed)
                    result.removed.push_back(id);
            if (input.contains(0))
                result.value = input.cell(0);
        }
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<bool>> select, wp<TSF<SampleSharedFrame>> source, wp<TS<i64>> cycle) {
        const auto left = left_column(source);
        const auto right = right_column(source);
        return observe(choose(select, left, right), cycle);
    };

    const auto frame = input<TSF<SampleSharedFrame>>(
        {{{{"left", i64{10}}, {"right", i64{10}}}}, literal::SKIP, literal::SKIP});
    static_cast<void>(run_graph(graph, std::array{false, false, true}, frame, std::array<i64, 3>{0, 1, 2}));

    REQUIRE(observations.size() == 3);
    CHECK(observations[0].valid);
    CHECK(observations[0].rows == std::vector<row_id_t>{0});
    CHECK(observations[2].valid);
    CHECK(observations[2].rows == std::vector<row_id_t>{0});
    CHECK(observations[2].added.empty());
    CHECK(observations[2].modified == std::vector<row_id_t>{0});
    CHECK(observations[2].removed.empty());
    CHECK(observations[2].value == 10);
}

TEST_CASE("anonymous TSF reference samples retain schema through copy",
          "[feature:frame][series:reference][feature:sampling]") {
    const auto fields = std::array{SchemaField{util::symbol{"value"}, &cpp_scalar_type<double>()},
                                  SchemaField{util::symbol{"count"}, &cpp_scalar_type<i64>()}};
    const auto &schema = anonymous_schema(fields);
    const auto &type = frame_series_type(schema, TimeSeriesGuarantee::ordinary);
    const auto &runtime = frame_time_series_runtime_type<SCHEMA>(type);

    struct Observation {
        bool valid{};
        bool modified{};
        EvaluationTime last_updated{MIN_DT};
        std::size_t field_count{};
        std::optional<double> value;
        std::optional<i64> count;
        bool copied_valid{};
        std::optional<double> copied_value;
        std::optional<i64> copied_count;
    };
    auto observations = std::vector<Observation>{};
    const auto choose = node([](In<TS<bool>> select, In<REF<TSF<SCHEMA>>> first,
                                In<REF<TSF<SCHEMA>>> second, Out<REF<TSF<SCHEMA>>> output) {
        output.copy(*select ? second : first);
    });
    const auto copy = node([](In<TSF<SCHEMA>> input, Out<TSF<SCHEMA>> output) { output.copy(input); });
    const auto observe = node([&](In<TSF<SCHEMA>> input, In<TSF<SCHEMA>, optional> copied,
                                  In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = Observation{.valid = input.valid(), .modified = input.modified(),
                                  .last_updated = input.last_updated()};
        if (result.valid) {
            result.field_count = input.schema().field_count();
            if (input.contains(0)) {
                result.value = input.column("value").view<double>().cell(0);
                result.count = input.column("count").view<i64>().cell(0);
            }
        }
        result.copied_valid = copied.valid();
        if (result.copied_valid)
            copied.for_each_row([&](row_id_t id, std::size_t) {
                result.copied_value = copied.column("value").input<double>().cell(id);
                result.copied_count = copied.column("count").input<i64>().cell(id);
            });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<bool>> select, wp<TSF<SCHEMA>> first, wp<TSF<SCHEMA>> second,
                           wp<TS<i64>> cycle) {
        const auto selected = choose(select, first, second);
        return observe(selected, copy(selected), cycle);
    };

    const auto first = bg::input(runtime, {{{{"value", 10.5}, {"count", i64{7}}}}, literal::SKIP});
    const auto second = bg::input(runtime, {{{{"value", 20.5}, {"count", i64{8}}}}, literal::SKIP});
    static_cast<void>(run_graph(graph, std::array{false, true}, first, second, std::array<i64, 2>{0, 1}));

    REQUIRE(observations.size() == 2);
    CHECK(observations[0].valid);
    CHECK(observations[0].field_count == 2);
    CHECK(observations[1].valid);
    CHECK(observations[1].modified);
    CHECK(observations[1].field_count == 2);
    CHECK(observations[1].value == 20.5);
    CHECK(observations[1].count == 8);
    CHECK(observations[1].copied_valid);
    CHECK(observations[1].copied_value == 20.5);
    CHECK(observations[1].copied_count == 8);
}

TEST_CASE("reference sampling keeps valid empty TSF and child update times",
          "[feature:frame][series:reference][feature:sampling]") {
    struct Observation {
        bool valid{};
        bool modified{};
        EvaluationTime last_updated{MIN_DT};
        std::size_t size{};
        bool child_valid{};
        bool child_modified{};
        EvaluationTime child_last_updated{MIN_DT};
    };
    auto observations = std::vector<Observation>{};
    const auto first_source = node([](In<TS<i64>> cycle, Out<TSF<SampleFrameBase>> output) {
        if (*cycle == 0)
            output.clear();
    });
    const auto second_source = node([](In<TS<i64>> cycle, Out<TSF<SampleFrameBase>> output) {
        if (*cycle == 0)
            output.clear();
    });
    const auto choose = node([](In<TS<bool>> select, In<REF<TSF<SampleFrameBase>>> first,
                                In<REF<TSF<SampleFrameBase>>> second,
                                Out<REF<TSF<SampleFrameBase>>> output) {
        output.copy(*select ? second : first);
    });
    const auto observe = node([&](In<TSF<SampleFrameBase>> input, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        const auto child = input.column("price");
        observations.push_back({.valid = input.valid(), .modified = input.modified(),
                                .last_updated = input.last_updated(), .size = input.size(),
                                .child_valid = child.valid(), .child_modified = child.modified(),
                                .child_last_updated = child.last_updated()});
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<bool>> select, wp<TS<i64>> cycle) {
        return observe(choose(select, first_source(cycle), second_source(cycle)), cycle);
    };

    static_cast<void>(run_graph(graph, std::array{false, true}, std::array<i64, 2>{0, 1}));

    REQUIRE(observations.size() == 2);
    CHECK(observations[0].valid);
    CHECK(observations[0].size == 0);
    CHECK(observations[1].valid);
    CHECK(observations[1].modified);
    CHECK(observations[1].size == 0);
    CHECK(observations[1].child_valid);
    CHECK(observations[1].child_modified);
    CHECK(observations[1].child_last_updated == observations[1].last_updated);
}

TEST_CASE("reference sampling keeps valid empty TSC target switches observable",
          "[feature:frame][series:reference][feature:sampling]") {
    struct Observation {
        bool valid{};
        bool modified{};
        EvaluationTime last_updated{MIN_DT};
        std::size_t size{};
    };
    auto observations = std::vector<Observation>{};
    const auto choose = node([](In<TS<bool>> select, In<REF<TSC<i64>>> first, In<REF<TSC<i64>>> second,
                                Out<REF<TSC<i64>>> output) { output.copy(*select ? second : first); });
    const auto observe = node([&](In<TSC<i64>> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        observations.push_back({.valid = input.valid(), .modified = input.modified(),
                                .last_updated = input.last_updated(), .size = input.size()});
        output.set(*cycle);
    });
    const auto first_source = node([](In<TS<i64>> cycle, Out<TSC<i64>> output) {
        if (*cycle == 0)
            output.clear();
    });
    const auto second_source = node([](In<TS<i64>> cycle, Out<TSC<i64>> output) {
        if (*cycle == 0)
            output.clear();
    });
    const auto graph = [&](wp<TS<bool>> select, wp<TS<i64>> cycle) {
        const auto selected = choose(select, first_source(cycle), second_source(cycle));
        return observe(selected, cycle);
    };

    static_cast<void>(run_graph(graph, std::array{false, true}, std::array<i64, 2>{0, 1}));

    REQUIRE(observations.size() == 2);
    CHECK(observations[0].valid);
    CHECK(observations[0].size == 0);
    CHECK(observations[1].valid);
    CHECK(observations[1].modified);
    CHECK(observations[1].size == 0);
    CHECK(observations[1].last_updated > observations[0].last_updated);
}

TEST_CASE("reference switches retain origin rows across same-cycle target changes",
          "[feature:frame][series:reference][feature:sampling]") {
    struct Observation {
        bool valid{};
        std::vector<row_id_t> added;
        std::vector<row_id_t> removed;
    };
    auto observations = std::vector<Observation>{};
    const auto first_source = node([](In<TS<i64>> cycle, Out<TSC<i64>> output) {
        if (*cycle == 0)
            static_cast<void>(output.add(10));
    });
    const auto second_source = node([](In<TS<i64>> cycle, Out<TSC<i64>> output) {
        if (*cycle == 0) {
            static_cast<void>(output.add(20));
            static_cast<void>(output.add(21));
        }
    });
    const auto third_source = node([](In<TS<i64>> cycle, Out<TSC<i64>> output) {
        if (*cycle == 0) {
            static_cast<void>(output.add(30));
            static_cast<void>(output.add(31));
            static_cast<void>(output.add(32));
        }
    });
    const auto switch_twice = node([](In<TS<i64>> cycle, In<REF<TSC<i64>>> first,
                                      In<REF<TSC<i64>>> second, In<REF<TSC<i64>>> third,
                                      Out<REF<TSC<i64>>> output) {
        if (*cycle == 0) {
            output.copy(first);
            return;
        }
        output.copy(first);
        output.copy(second);
        output.copy(third);
    });
    const auto observe = node([&](In<TSC<i64>> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = Observation{.valid = input.valid()};
        const auto storage = input.extension_descriptor().storage;
        if (storage.added != nullptr)
            for (const auto id : *storage.added)
                result.added.push_back(id);
        if (storage.removed != nullptr)
            for (const auto id : *storage.removed)
                result.removed.push_back(id);
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        const auto first = first_source(cycle);
        const auto second = second_source(cycle);
        const auto third = third_source(cycle);
        return observe(switch_twice(cycle, first, second, third), cycle);
    };

    static_cast<void>(run_graph(graph, std::array<i64, 2>{0, 1}));

    REQUIRE(observations.size() == 2);
    CHECK(observations[1].valid);
    CHECK(observations[1].added == std::vector<row_id_t>{0, 1, 2});
    CHECK(observations[1].removed == std::vector<row_id_t>{0});
}

TEST_CASE("switching away from an invalidated TSC does not leak its prior removals",
          "[feature:frame][series:reference][feature:sampling]") {
    struct Observation {
        bool valid{};
        bool modified{};
        std::vector<row_id_t> added;
        std::vector<row_id_t> removed;
    };
    auto observations = std::vector<Observation>{};
    auto removed_row = row_id_t{};
    const auto first_source = node([](In<TS<i64>> cycle, Out<TSC<i64>> output) {
        if (*cycle == 0)
            static_cast<void>(output.add(10));
    });
    const auto second_source = node([&](In<TS<i64>> cycle, Out<TSC<i64>> output) {
        if (*cycle == 0) {
            static_cast<void>(output.add(20));
            static_cast<void>(output.add(21));
            removed_row = output.add(22);
        } else if (*cycle == 1) {
            static_cast<void>(output.erase(removed_row));
        } else if (*cycle == 2) {
            output.invalidate();
        }
    });
    const auto third_source = node([](In<TS<i64>> cycle, Out<TSC<i64>> output) {
        if (*cycle == 0) {
            static_cast<void>(output.add(30));
            static_cast<void>(output.add(31));
            static_cast<void>(output.add(32));
        }
    });
    const auto choose = node([](In<TS<i64>> cycle, In<REF<TSC<i64>>> first,
                                In<REF<TSC<i64>>> second, In<REF<TSC<i64>>> third,
                                Out<REF<TSC<i64>>> output) {
        if (*cycle == 0)
            output.copy(first);
        else if (*cycle == 1)
            output.copy(second);
        else
            output.copy(third);
    });
    const auto observe = node([&](In<TSC<i64>> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = Observation{.valid = input.valid(), .modified = input.modified()};
        const auto storage = input.extension_descriptor().storage;
        if (storage.added != nullptr)
            for (const auto id : *storage.added)
                result.added.push_back(id);
        if (storage.removed != nullptr)
            for (const auto id : *storage.removed)
                result.removed.push_back(id);
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        return observe(choose(cycle, first_source(cycle), second_source(cycle), third_source(cycle)), cycle);
    };

    static_cast<void>(run_graph(graph, std::array<i64, 3>{0, 1, 2}));

    REQUIRE(observations.size() == 3);
    CHECK(observations[1].valid);
    CHECK(observations[1].added == std::vector<row_id_t>{0, 1});
    CHECK(observations[1].removed == std::vector<row_id_t>{0});
    CHECK(observations[2].valid);
    CHECK(observations[2].modified);
    CHECK(observations[2].added == std::vector<row_id_t>{0, 1, 2});
    CHECK(observations[2].removed == std::vector<row_id_t>{0, 1});
}

TEST_CASE("invalidated TSF view sampling uses cycle-start rows for the frame and child column",
          "[feature:frame][series:reference][feature:sampling]") {
    struct Observation {
        bool frame_valid{};
        bool column_valid{};
        std::vector<row_id_t> frame_added;
        std::vector<row_id_t> frame_removed;
        std::vector<row_id_t> column_added;
        std::vector<row_id_t> column_removed;
    };
    auto observations = std::vector<Observation>{};
    const auto first_source = node([](In<TS<i64>> cycle, Out<TSF<SampleFrameBase>> output) {
        if (*cycle == 0) {
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 10, .price = 1.0}));
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 11, .price = 1.1}));
        }
    });
    const auto second_source = node([](In<TS<i64>> cycle, Out<TSF<SampleFrameBase>> output) {
        if (*cycle == 0) {
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 20, .price = 2.0}));
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 21, .price = 2.1}));
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 22, .price = 2.2}));
        }
    });
    const auto third_source = node([](In<TS<i64>> cycle, Out<TSF<SampleFrameBase>> output) {
        if (*cycle == 0) {
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 30, .price = 3.0}));
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 31, .price = 3.1}));
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 32, .price = 3.2}));
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 33, .price = 3.3}));
        }
    });
    const auto bind_invalidated_view = node([](In<TSF<SampleFrameBase>> first, In<TSF<SampleFrameBase>> second,
                                               In<TS<i64>> cycle, Out<TSF<SampleFrameBase>, view> output) {
        if (*cycle == 0) {
            static_cast<void>(output.bind_projection(first, columns<"id", "price">()));
            return;
        }
        static_cast<void>(output.bind_projection(second, columns<"id", "price">()));
        output.invalidate();
    });
    const auto choose_frame = node([](In<TS<i64>> cycle, In<REF<TSF<SampleFrameBase>>> previous,
                                      In<REF<TSF<SampleFrameBase>>> current,
                                      Out<REF<TSF<SampleFrameBase>>> output) {
        output.copy(*cycle == 0 ? previous : current);
    });
    const auto choose_column = node([](In<TS<i64>> cycle, In<REF<TSC<i64>>> previous,
                                       In<REF<TSC<i64>>> current, Out<REF<TSC<i64>>> output) {
        output.copy(*cycle == 0 ? previous : current);
    });
    const auto observe = node([&](In<TSF<SampleFrameBase>> frame, In<TSC<i64>> column,
                                  In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = Observation{.frame_valid = frame.valid(), .column_valid = column.valid()};
        const auto frame_storage = frame.extension_descriptor();
        if (frame_storage.added != nullptr)
            for (const auto id : *frame_storage.added)
                result.frame_added.push_back(id);
        if (frame_storage.removed != nullptr)
            for (const auto id : *frame_storage.removed)
                result.frame_removed.push_back(id);
        column.for_each_changed([&](row_id_t id) { result.column_added.push_back(id); });
        column.for_each_removed([&](row_id_t id) { result.column_removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        const auto first = first_source(cycle);
        const auto second = second_source(cycle);
        const auto current = third_source(cycle);
        const auto view = bind_invalidated_view(first, second, cycle);
        const auto previous_column = select_column<"id">(view);
        const auto current_column = select_column<"id">(current);
        return observe(choose_frame(cycle, view, current),
                       choose_column(cycle, previous_column, current_column), cycle);
    };

    static_cast<void>(run_graph(graph, std::array<i64, 2>{0, 1}));

    REQUIRE(observations.size() == 2);
    CHECK(observations[1].frame_valid);
    CHECK(observations[1].column_valid);
    CHECK(observations[1].frame_added == std::vector<row_id_t>{0, 1, 2, 3});
    CHECK(observations[1].frame_removed == std::vector<row_id_t>{0, 1});
    CHECK(observations[1].column_added == std::vector<row_id_t>{0, 1, 2, 3});
    CHECK(observations[1].column_removed == std::vector<row_id_t>{0, 1});
}

TEST_CASE("invalidated TSF view child sampling retains source removals",
          "[feature:frame][series:reference][feature:sampling]") {
    struct Observation {
        bool frame_valid{};
        bool column_valid{};
        std::vector<row_id_t> frame_removed;
        std::vector<row_id_t> column_removed;
    };
    auto observations = std::vector<Observation>{};
    auto removed_row = row_id_t{};
    const auto previous_source = node([&](In<TS<i64>> cycle, Out<TSF<SampleFrameBase>> output) {
        if (*cycle == 0) {
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 10, .price = 1.0}));
            removed_row = output.add(Row<SampleFrameBase>{.id = 11, .price = 1.1});
        } else if (*cycle == 1) {
            static_cast<void>(output.erase(removed_row));
        }
    });
    const auto current_source = node([](In<TS<i64>> cycle, Out<TSF<SampleFrameBase>> output) {
        if (*cycle == 0) {
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 30, .price = 3.0}));
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 31, .price = 3.1}));
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 32, .price = 3.2}));
            static_cast<void>(output.add(Row<SampleFrameBase>{.id = 33, .price = 3.3}));
        }
    });
    const auto invalidate_view = node([](In<TSF<SampleFrameBase>> source, In<TS<i64>> cycle,
                                         Out<TSF<SampleFrameBase>, view> output) {
        if (*cycle == 0)
            static_cast<void>(output.bind_projection(source, columns<"id", "price">()));
        else
            output.invalidate();
    });
    const auto choose_frame = node([](In<TS<i64>> cycle, In<REF<TSF<SampleFrameBase>>> previous,
                                      In<REF<TSF<SampleFrameBase>>> current,
                                      Out<REF<TSF<SampleFrameBase>>> output) {
        output.copy(*cycle == 0 ? previous : current);
    });
    const auto choose_column = node([](In<TS<i64>> cycle, In<REF<TSC<i64>>> previous,
                                       In<REF<TSC<i64>>> current, Out<REF<TSC<i64>>> output) {
        output.copy(*cycle == 0 ? previous : current);
    });
    const auto observe = node([&](In<TSF<SampleFrameBase>> frame, In<TSC<i64>> column,
                                  In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = Observation{.frame_valid = frame.valid(), .column_valid = column.valid()};
        frame.for_each_removed([&](row_id_t id) { result.frame_removed.push_back(id); });
        column.for_each_removed([&](row_id_t id) { result.column_removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        const auto previous = invalidate_view(previous_source(cycle), cycle);
        const auto current = current_source(cycle);
        return observe(choose_frame(cycle, previous, current),
                       choose_column(cycle, select_column<"id">(previous), select_column<"id">(current)), cycle);
    };

    static_cast<void>(run_graph(graph, std::array<i64, 2>{0, 1}));

    REQUIRE(observations.size() == 2);
    CHECK(observations[1].frame_valid);
    CHECK(observations[1].column_valid);
    CHECK(observations[1].frame_removed == std::vector<row_id_t>{0, 1});
    CHECK(observations[1].column_removed == observations[1].frame_removed);
}

TEST_CASE("zero-column TSF reference samples preserve valid empty rows",
          "[feature:frame][series:reference][feature:sampling]") {
    struct Observation {
        bool valid{};
        std::size_t field_count{};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> added;
        std::vector<row_id_t> removed;
    };

    auto observations = std::vector<Observation>{};
    const auto first_source = node([](In<TS<i64>> cycle, Out<TSF<SampleEmptyFrame>> output) {
        if (*cycle == 0)
            static_cast<void>(output.add(SampleEmptyFrame<Cell>{}));
    });
    const auto second_source = node([](In<TS<i64>> cycle, Out<TSF<SampleEmptyFrame>> output) {
        if (*cycle == 0)
            static_cast<void>(output.add(SampleEmptyFrame<Cell>{}));
    });
    const auto choose = node([](In<TS<bool>> select, In<REF<TSF<SampleEmptyFrame>>> first,
                                In<REF<TSF<SampleEmptyFrame>>> second,
                                Out<REF<TSF<SampleEmptyFrame>>> output) {
        output.copy(*select ? second : first);
    });
    const auto observe = node([&](In<TSF<SampleEmptyFrame>> input, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        auto result = Observation{.valid = input.valid()};
        const auto storage = input.extension_descriptor();
        result.field_count = storage.schema == nullptr ? 0 : storage.schema->field_count();
        storage.rows.each([&](row_id_t id) { result.rows.push_back(id); });
        if (storage.added != nullptr)
            for (const auto id : *storage.added)
                result.added.push_back(id);
        if (storage.removed != nullptr)
            for (const auto id : *storage.removed)
                result.removed.push_back(id);
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<bool>> select, wp<TS<i64>> cycle) {
        const auto first = first_source(cycle);
        const auto second = second_source(cycle);
        return observe(choose(select, first, second), cycle);
    };

    const auto values = std::array<i64, 2>{0, 1};
    static_cast<void>(run_graph(graph, std::array{false, true}, values));

    REQUIRE(observations.size() == 2);
    CHECK(observations[0].valid);
    CHECK(observations[0].rows == std::vector<row_id_t>{0});
    CHECK(observations[1].valid);
    CHECK(observations[1].field_count == 0);
    CHECK(observations[1].rows == std::vector<row_id_t>{0});
    CHECK(observations[1].added == std::vector<row_id_t>{0});
    CHECK(observations[1].removed == std::vector<row_id_t>{0});
}

} // namespace
