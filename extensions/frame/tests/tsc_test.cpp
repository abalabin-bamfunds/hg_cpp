#include <bg/frame/tsc.hpp>

#include <bg/bg.hpp>
#include <bg/testing/test_graph.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error{message};
}

struct writer_state {
    bg::frame::row_id_t first{bg::frame::no_row};
    bg::frame::row_id_t second{bg::frame::no_row};
    bg::frame::row_id_t third{bg::frame::no_row};
};

class CountingResource final : public std::pmr::memory_resource {
  public:
    std::size_t allocations{};
    std::size_t deallocations{};

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

inline constexpr auto test_guarantee = bg::guarantee_variable<"TSC_G">;

struct tsc_writer {
    bg::In<bg::TS<bg::i64>> command;
    bg::State<writer_state> state;
    bg::Out<bg::frame::TSC<bg::i64>> output;

    void evaluate() const {
        switch (*command) {
        case 0:
            return;
        case 1:
            state->first = output.add(10);
            return;
        case 2:
            state->second = output.add(std::nullopt);
            return;
        case 3:
            output.set(state->first, 10);
            return;
        case 4:
            output.set(state->second, 20);
            return;
        case 5:
            output.set(state->second, std::nullopt);
            return;
        case 6:
            require(output.erase(state->first), "first row was not removed");
            state->third = output.add(30);
            require(state->third != state->first, "removed ID was reused in the same cycle");
            return;
        case 7: {
            const auto reused = output.add(40);
            require(reused == state->first, "removed ID was not reused in a later cycle");
            return;
        }
        case 8: {
            const auto temporary = output.add(50);
            require(output.erase(temporary), "temporary row was not removed");
            return;
        }
        case 9:
            output.clear();
            return;
        case 10:
            output.invalidate();
            return;
        case 11:
            state->first = output.add(70);
            return;
        default:
            throw std::runtime_error{"unknown TSC test command"};
        }
    }
};

struct tsc_view {
    bg::In<bg::frame::TSC<bg::i64>, bg::optional> input;
    bg::Out<bg::frame::TSC<bg::i64>, bg::frame::view> output;

    void evaluate() const { output.bind(input); }
};

struct selectable_tsc_view {
    bg::In<bg::frame::TSC<bg::i64>, bg::optional> first;
    bg::In<bg::frame::TSC<bg::i64>, bg::optional> second;
    bg::In<bg::TS<bg::i64>> select;
    bg::Out<bg::frame::TSC<bg::i64>, bg::frame::view> output;

    void evaluate() const {
        if (*select == 0)
            output.bind(first);
        else
            output.bind(second);
    }
};

struct observation {
    bool valid{};
    bool became_valid{};
    bool became_invalid{};
    bool modified{};
    bool zero_copy{};
    std::vector<bg::frame::row_id_t> rows;
    std::vector<std::optional<bg::i64>> cells;
    std::vector<bg::frame::row_id_t> changed;
    std::vector<bg::frame::row_id_t> removed;
};

void require_snapshot(const std::optional<bg::Scalar<bg::frame::Column<bg::i64>>> &snapshot,
                      std::initializer_list<std::optional<bg::i64>> expected, const char *message) {
    require(snapshot.has_value(), message);
    const auto view = snapshot->value();
    require(view.size() == expected.size(), message);
    auto position = std::size_t{};
    for (const auto cell : expected) {
        require(view.cell(position) == cell, message);
        ++position;
    }
}

void test_tsc_guarantee_matching() {
    using Parameter = bg::frame::TSC<bg::i64, test_guarantee>;

    const auto &parameter = Parameter::type();
    const auto &ordinary = bg::frame::TSC<bg::i64>::type();
    auto type_map = bg::TypeMap{};
    require(!parameter.is_resolved() && parameter.match(ordinary, type_map),
            "TSC guarantee variable did not match ordinary");
    require(&parameter.resolve(type_map) == &ordinary, "TSC guarantee variable did not resolve to ordinary");
    require(&bg::resolve_time_series_runtime_type(ordinary).type() == &ordinary,
            "TSC semantic factory did not register the default runtime");

    auto variables = std::size_t{};
    bg::for_each_declaration_variable<Parameter>([&]<typename>(const auto &) { ++variables; });
    require(variables == 1, "TSC declaration did not expose its guarantee variable");

    auto rejected_declaration = false;
    try {
        static_cast<void>(bg::frame::TSC<bg::i64, bg::TimeSeriesGuarantee::deduplicated>::type());
    } catch (const std::invalid_argument &) {
        rejected_declaration = true;
    }
    require(rejected_declaration, "TSC accepted an unsupported resolved guarantee");

    const auto deduplicated = bg::frame::ColumnSeriesType{
        bg::cpp_scalar_type<bg::i64>(), bg::TimeSeriesGuarantee::deduplicated};
    auto rejected_runtime = false;
    try {
        static_cast<void>(bg::frame::column_time_series_runtime_type<bg::i64>(deduplicated));
    } catch (const std::invalid_argument &) {
        rejected_runtime = true;
    }
    require(rejected_runtime, "TSC runtime accepted an unsupported semantic type");

    const auto noncanonical = bg::frame::ColumnSeriesType{
        bg::cpp_scalar_type<bg::i64>(), bg::TimeSeriesGuarantee::ordinary};
    auto rejected_noncanonical_runtime = false;
    try {
        static_cast<void>(bg::frame::column_time_series_runtime_type<bg::i64>(noncanonical));
    } catch (const std::invalid_argument &) {
        rejected_noncanonical_runtime = true;
    }
    require(rejected_noncanonical_runtime, "TSC runtime accepted a noncanonical semantic type");

    const auto &unresolved = parameter;
    auto rejected_unresolved_runtime = false;
    try {
        static_cast<void>(bg::frame::column_time_series_runtime_type<bg::i64>(unresolved));
    } catch (const std::invalid_argument &) {
        rejected_unresolved_runtime = true;
    }
    require(rejected_unresolved_runtime, "TSC runtime accepted an unresolved semantic type");

    auto rejected_other_runtime = false;
    try {
        static_cast<void>(bg::frame::column_time_series_runtime_type<bg::i64>(bg::TS<bg::i64>::type()));
    } catch (const std::invalid_argument &) {
        rejected_other_runtime = true;
    }
    require(rejected_other_runtime, "TSC runtime accepted another time series family");
}

void test_column_descriptor_resolution() {
    using CELL = bg::ScalarVariable<"FRAME_CELL_TYPE">;
    const auto &generic_column = bg::frame::column_type(bg::cpp_scalar_type<CELL>());
    const auto &integer_column = bg::frame::column_type(bg::cpp_scalar_type<bg::i64>());
    require(!generic_column.is_resolved(), "generic column descriptor resolved before its cell type");
    require(&integer_column == &bg::frame::column_type(bg::cpp_scalar_type<bg::i64>()),
            "column descriptor factory did not canonicalize concrete cell types");
    require(&bg::cpp_scalar_type<bg::frame::Column<bg::i64>>() == &integer_column,
            "Column<T> did not use the canonical column descriptor factory");

    auto column_map = bg::TypeMap{};
    require(generic_column.match(integer_column, column_map), "column descriptor did not match a scalar variable");
    require(&generic_column.resolve(column_map) == &integer_column, "column descriptor resolved to a noncanonical type");
    require(bg::TypeRegistry::instance().contains(integer_column), "column descriptor was not registered");
    require(!generic_column.match(bg::frame::column_type(bg::cpp_scalar_type<double>()), column_map),
            "column descriptor accepted a conflicting scalar mapping");

    const auto &generic_series = bg::frame::column_series_type(
        bg::cpp_scalar_type<CELL>(), bg::TimeSeriesGuarantee::ordinary);
    const auto &integer_series = bg::frame::TSC<bg::i64>::type();
    auto series_map = bg::TypeMap{};
    require(!generic_series.is_resolved(), "generic column series descriptor resolved before its cell type");
    require(generic_series.match(integer_series, series_map), "column series descriptor did not match a scalar variable");
    require(&generic_series.resolve(series_map) == &integer_series,
            "column series descriptor resolved to a noncanonical type");

    const auto &cell_and_guarantee = bg::frame::column_series_type(
        bg::cpp_scalar_type<CELL>(), bg::TimeSeriesGuarantee::final);
    auto failed_match_map = bg::TypeMap{};
    require(!cell_and_guarantee.match(integer_series, failed_match_map),
            "column series descriptor accepted an incompatible guarantee");
    require(failed_match_map.find(bg::details::scalar_variable_instance<CELL>()) == nullptr,
            "failed column series matching retained its partial cell mapping");

    constexpr auto FIRST_GUARANTEE = bg::guarantee_variable<"FRAME_FIRST_GUARANTEE">;
    constexpr auto SECOND_GUARANTEE = bg::guarantee_variable<"FRAME_SECOND_GUARANTEE">;
    const auto &first_guarantee = bg::frame::column_series_type(
        bg::cpp_scalar_type<bg::i64>(), bg::details::guarantee_parameter<FIRST_GUARANTEE>());
    const auto &same_guarantee = bg::frame::column_series_type(
        bg::cpp_scalar_type<bg::i64>(), bg::details::guarantee_parameter<FIRST_GUARANTEE>());
    const auto &second_guarantee = bg::frame::column_series_type(
        bg::cpp_scalar_type<bg::i64>(), bg::details::guarantee_parameter<SECOND_GUARANTEE>());
    require(&first_guarantee == &same_guarantee, "column series factory did not canonicalize a guarantee variable");
    require(&first_guarantee != &second_guarantee,
            "column series factory conflated distinct guarantee variables");
}

void test_runtime_initialization() {
    const auto produce = bg::node([](bg::Out<bg::frame::TSC<bg::i64>> output) { output.add(42); });
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        static_cast<void>(produce());
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto memory = CountingResource{};
    const auto allocator = bg::allocator{memory};
    const auto time = bg::MIN_DT + bg::EvaluationTime::duration{1};
    auto first_global_state = bg::GlobalState{};
    auto second_global_state = bg::GlobalState{};

    {
        auto first = bg::RuntimeGraph{compiled, allocator, first_global_state};
        auto second = bg::RuntimeGraph{compiled, allocator, second_global_state};
        first.start(time);
        second.start(time);

        const auto *first_storage =
            static_cast<const bg::frame::NativeColumn<bg::i64> *>(first.node(0).output_value());
        const auto *second_storage =
            static_cast<const bg::frame::NativeColumn<bg::i64> *>(second.node(0).output_value());
        require(first_storage->size() == 0 && second_storage->size() == 0,
                "runtime instances did not start with empty TSC storage");

        first.schedule(0);
        first.evaluate(time);
        require(first_storage->size() == 1 && first_storage->cell_at(0) == 42 && second_storage->size() == 0,
                "TSC storage leaked between runtime instances");

        second.schedule(0);
        second.evaluate(time);
        require(second_storage->size() == 1 && second_storage->cell_at(0) == 42,
                "second runtime instance did not evaluate its TSC producer");
    }
    require(memory.allocations == memory.deallocations, "runtime TSC storage was not released");
}

void test_tsc_graph_behavior() {
    auto observations = std::vector<observation>{};
    const auto write = bg::node<tsc_writer>();
    const auto observe = bg::node([&](bg::In<bg::frame::TSC<bg::i64>, bg::optional> input,
                                      bg::In<bg::TS<bg::i64>> tick) {
        static_cast<void>(tick);
        auto result = observation{.valid = input.valid(),
                                  .became_valid = input.became_valid(),
                                  .became_invalid = input.became_invalid(),
                                  .modified = input.modified()};
        input.for_each_row([&](bg::frame::row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.cells.push_back(input.cell(id));
        });
        input.for_each_changed([&](bg::frame::row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](bg::frame::row_id_t id) { result.removed.push_back(id); });

        auto native_values = std::vector<const bg::i64 *>{};
        input.for_each_batch([&](const auto &batch) { native_values.push_back(batch.cells.values); });
        auto scalar_values = std::vector<const bg::i64 *>{};
        if (input.valid()) {
            input.value_view().value().batches(
                [&](const auto &batch) { scalar_values.push_back(batch.values); });
        }
        result.zero_copy = native_values == scalar_values;
        observations.push_back(std::move(result));
    });
    const auto snapshot = bg::node([](bg::In<bg::frame::TSC<bg::i64>> input,
                                      bg::Out<bg::TS<bg::frame::Column<bg::i64>>> output) {
        output.set_value(input.value_view());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> command, bg::wp<bg::TS<bg::i64>> tick) {
        const auto rows = write(command);
        observe(rows, tick);
        return snapshot(rows);
    };
    const auto commands = std::array<bg::i64, 16>{9, 1, 2, 3, 0, 4, 5, 6, 7, 8, 9, 11, 10, 0, 9, 11};
    const auto ticks = std::array<bg::i64, 16>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const auto snapshots = bg::testing::test_graph(graph, commands, ticks);

    require(observations.size() == commands.size(), "tick observer did not run for every cycle");
    for (const auto &entry : observations)
        require(entry.zero_copy, "native and scalar batches did not share cell storage");

    require(observations[0].valid && observations[0].became_valid && observations[0].rows.empty() &&
                observations[0].cells.empty() && observations[0].changed.empty() && observations[0].removed.empty(),
            "initial clear did not publish a valid empty column");
    require(observations[1].valid && observations[1].rows.size() == 1 && observations[1].changed.size() == 1,
            "first non-null row was not reported");
    require(observations[1].cells == std::vector<std::optional<bg::i64>>{10}, "first row value was lost");
    require(observations[2].cells == std::vector<std::optional<bg::i64>>{10, std::nullopt},
            "null row was not retained");
    require(observations[2].rows.size() == 2 && observations[2].changed.size() == 1,
            "null row addition was not reported");
    require(observations[3].changed == std::vector{observations[1].rows[0]},
            "equal set did not report exactly one changed row");
    require(observations[4].valid && !observations[4].modified && observations[4].changed.empty() &&
                observations[4].removed.empty(),
            "unchanged source reported row deltas through the tick observer");
    require(observations[5].cells == std::vector<std::optional<bg::i64>>{10, 20},
            "null-to-value transition was lost");
    require(observations[6].cells == std::vector<std::optional<bg::i64>>{10, std::nullopt},
            "value-to-null transition was lost");
    const auto cell_for = [](const observation &entry, bg::frame::row_id_t id) {
        const auto row = std::ranges::find(entry.rows, id);
        require(row != entry.rows.end(), "row ID was missing from observation");
        return entry.cells[static_cast<std::size_t>(row - entry.rows.begin())];
    };
    require(observations[7].changed.size() == 1 && observations[7].rows.size() == 2 &&
                cell_for(observations[7], observations[2].rows[1]) == std::nullopt &&
                cell_for(observations[7], observations[7].changed[0]) == 30 &&
                observations[7].removed == std::vector{observations[1].rows[0]},
            "same-cycle removal and addition did not preserve row values and deltas");
    require(observations[8].rows.size() == 3 &&
                cell_for(observations[8], observations[2].rows[1]) == std::nullopt &&
                cell_for(observations[8], observations[7].changed[0]) == 30 &&
                cell_for(observations[8], observations[1].rows[0]) == 40,
            "later ID reuse did not preserve row/value associations");
    require(observations[9].modified && observations[9].changed.empty() && observations[9].removed.empty(),
            "add-then-remove did not cancel its row deltas");
    require(observations[10].valid && observations[10].rows.empty() && observations[10].cells.empty() &&
                observations[10].removed.size() == observations[8].rows.size() &&
                std::is_permutation(observations[10].removed.begin(), observations[10].removed.end(),
                                    observations[8].rows.begin()),
            "clear did not report every removed row");
    require(observations[11].valid && observations[11].cells == std::vector<std::optional<bg::i64>>{70},
            "row before invalidation was not published");
    require(!observations[12].valid && observations[12].became_invalid && !observations[12].modified &&
                observations[12].changed.empty() && observations[12].removed.empty(),
            "invalidation of a live column was not observed");
    require(!observations[13].valid && !observations[13].modified && observations[13].changed.empty() &&
                observations[13].removed.empty(),
            "quiet invalid cycle reported stale row deltas");
    require(observations[14].valid && observations[14].became_valid && observations[14].rows.empty() &&
                observations[14].cells.empty() && observations[14].changed.empty() && observations[14].removed.empty(),
            "clear did not revalidate as an empty column");
    require(observations[15].valid && observations[15].cells == std::vector<std::optional<bg::i64>>{70},
            "row after empty revalidation was not published");

    require_snapshot(snapshots[0], {}, "initial empty snapshot was lost");
    require_snapshot(snapshots[1], {10}, "first snapshot was lost");
    require_snapshot(snapshots[2], {10, std::nullopt}, "nullable snapshot was lost");
    require_snapshot(snapshots[3], {10, std::nullopt}, "equal-set snapshot was not published");
    require(!snapshots[4].has_value(), "unchanged source produced a snapshot");
    require_snapshot(snapshots[5], {10, 20}, "value snapshot was lost");
    require_snapshot(snapshots[6], {10, std::nullopt}, "null transition snapshot was lost");
    require_snapshot(snapshots[7], {30, std::nullopt}, "removal snapshot was lost");
    require_snapshot(snapshots[8], {30, std::nullopt, 40}, "reused-ID snapshot was lost");
    require_snapshot(snapshots[9], {30, std::nullopt, 40}, "canceled-delta snapshot was not published");
    require_snapshot(snapshots[10], {}, "clear snapshot was not empty");
    require_snapshot(snapshots[11], {70}, "snapshot before invalidation was lost");
    require(!snapshots[12].has_value(), "invalid column produced a snapshot");
    require(!snapshots[13].has_value(), "quiet invalid cycle produced a snapshot");
    require_snapshot(snapshots[14], {}, "empty revalidation snapshot was lost");
    require_snapshot(snapshots[15], {70}, "post-revalidation snapshot was lost");
}

void test_double_tsc_graph() {
    const auto write = bg::node([](bg::In<bg::TS<double>> input, bg::Out<bg::frame::TSC<double>> output) {
        output.clear();
        output.add(*input);
    });
    const auto snapshot = bg::node([](bg::In<bg::frame::TSC<double>> input,
                                      bg::Out<bg::TS<bg::frame::Column<double>>> output) {
        output.set_value(input.value_view());
    });
    const auto graph = [&](bg::wp<bg::TS<double>> input) { return snapshot(write(input)); };
    const auto values = std::array{1.25, 2.5, 8.75};
    const auto result = bg::testing::test_graph(graph, values);
    require(result.size() == values.size(), "double TSC graph lost cycles");
    for (auto index = std::size_t{}; index != values.size(); ++index) {
        require(result[index] && result[index]->value().size() == 1 &&
                    result[index]->value().cell(0) == values[index],
                "double TSC graph changed a value");
    }
}

void test_tsc_view_binding() {
    auto observations = std::vector<observation>{};
    const auto write = bg::node<tsc_writer>();
    const auto bind = bg::node<tsc_view>();
    const auto observe = bg::node([&](bg::In<bg::frame::TSC<bg::i64>, bg::optional> input,
                                      bg::In<bg::TS<bg::i64>> tick, bg::Out<bg::TS<bg::i64>> output) {
        auto result = observation{.valid = input.valid(),
                                  .became_valid = input.became_valid(),
                                  .became_invalid = input.became_invalid(),
                                  .modified = input.modified()};
        input.for_each_row([&](bg::frame::row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.cells.push_back(input.cell(id));
        });
        input.for_each_changed([&](bg::frame::row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](bg::frame::row_id_t id) { result.removed.push_back(id); });
        auto native_values = std::vector<const bg::i64 *>{};
        input.for_each_batch([&](const auto &batch) { native_values.push_back(batch.cells.values); });
        auto scalar_values = std::vector<const bg::i64 *>{};
        if (input.valid())
            input.value_view().value().batches([&](const auto &batch) { scalar_values.push_back(batch.values); });
        result.zero_copy = native_values == scalar_values;
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> command, bg::wp<bg::TS<bg::i64>> tick) {
        return observe(bind(write(command)), tick);
    };
    const auto commands = std::array<bg::i64, 16>{9, 1, 2, 3, 0, 4, 5, 6, 7, 8, 9, 11, 10, 0, 9, 11};
    const auto ticks = std::array<bg::i64, 16>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static_cast<void>(bg::testing::test_graph(graph, commands, ticks));

    require(observations.size() == commands.size(), "TSC view observer did not run for every cycle");
    for (const auto &entry : observations)
        require(entry.zero_copy, "TSC view and scalar batches did not share native values");
    require(observations[0].valid && observations[0].became_valid && observations[0].modified &&
                observations[0].rows.empty(),
            "TSC view did not publish its first empty binding");
    require(observations[1].valid && observations[1].rows.size() == 1 && observations[1].changed.size() == 1 &&
                observations[1].cells == std::vector<std::optional<bg::i64>>{10},
            "TSC view did not report the first added row zero-copy");
    require(observations[3].modified && observations[3].changed == observations[1].rows,
            "TSC view did not preserve a source cell modification");
    require(observations[7].modified && observations[7].removed == observations[1].rows &&
                observations[7].changed.size() == 1,
            "TSC view did not preserve simultaneous row removal and addition");
    require(!observations[9].modified && observations[9].changed.empty() && observations[9].removed.empty(),
            "TSC view published a source add-then-remove patch");
    require(!observations[12].valid && observations[12].became_invalid &&
                observations[12].changed.empty() && observations[12].removed.empty(),
            "TSC view did not propagate source invalidation");
    require(observations[14].valid && observations[14].became_valid && observations[14].modified &&
                observations[14].rows.empty(),
            "TSC view did not bind a valid empty source after invalidation");
}

void test_tsc_view_rebinding() {
    auto observations = std::vector<observation>{};
    const auto write_first = bg::node<tsc_writer>();
    const auto write_second = bg::node<tsc_writer>();
    const auto bind = bg::node<selectable_tsc_view>();
    const auto observe = bg::node([&](bg::In<bg::frame::TSC<bg::i64>, bg::optional> input,
                                      bg::In<bg::TS<bg::i64>> tick, bg::Out<bg::TS<bg::i64>> output) {
        auto result = observation{.valid = input.valid(), .modified = input.modified()};
        input.for_each_row([&](bg::frame::row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.cells.push_back(input.cell(id));
        });
        input.for_each_changed([&](bg::frame::row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](bg::frame::row_id_t id) { result.removed.push_back(id); });
        auto native_values = std::vector<const bg::i64 *>{};
        input.for_each_batch([&](const auto &batch) { native_values.push_back(batch.cells.values); });
        auto scalar_values = std::vector<const bg::i64 *>{};
        if (input.valid())
            input.value_view().value().batches([&](const auto &batch) { scalar_values.push_back(batch.values); });
        result.zero_copy = native_values == scalar_values;
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> first_command, bg::wp<bg::TS<bg::i64>> second_command,
                           bg::wp<bg::TS<bg::i64>> selection, bg::wp<bg::TS<bg::i64>> tick) {
        const auto first = write_first(first_command);
        const auto second = write_second(second_command);
        return observe(bind(first, second, selection), tick);
    };
    const auto first_commands = std::array<bg::i64, 3>{1, 0, 0};
    const auto second_commands = std::array<bg::i64, 3>{1, 2, 0};
    const auto selection = std::array<bg::i64, 3>{0, 0, 1};
    const auto ticks = std::array<bg::i64, 3>{0, 1, 2};
    static_cast<void>(bg::testing::test_graph(graph, first_commands, second_commands, selection, ticks));

    require(observations.size() == 3, "TSC view rebind observer missed a cycle");
    require(observations[0].valid && observations[0].cells == std::vector<std::optional<bg::i64>>{10} &&
                observations[0].changed.size() == 1 && observations[0].zero_copy,
            "TSC view did not bind the first source");
    require(!observations[1].modified,
            "TSC view changed when its bound source had no current-cycle changes");
    require(observations[2].modified && observations[2].cells ==
                std::vector<std::optional<bg::i64>>{10, std::nullopt} &&
                observations[2].changed == std::vector<bg::frame::row_id_t>{0, 1} &&
                observations[2].removed == std::vector<bg::frame::row_id_t>{0} && observations[2].zero_copy,
            "TSC view did not preserve additions and removals when rebinding to another row domain");
}

} // namespace

int main() {
    try {
        test_tsc_guarantee_matching();
        test_column_descriptor_resolution();
        test_runtime_initialization();
        test_tsc_graph_behavior();
        test_double_tsc_graph();
        test_tsc_view_binding();
        test_tsc_view_rebinding();
        std::cout << "bg_frame_tsc_test passed\n";
    } catch (const std::exception &error) {
        std::cerr << "bg_frame_tsc_test: " << error.what() << '\n';
        return 1;
    }
}
