#include <bg/frame/column.hpp>

#include <bg/bg.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/type/type_registry.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error{message}; }

struct RowTraversalCounter {
    const bg::frame::NativeRows *rows{};
    mutable std::size_t traversals{};

    bg::frame::RowSet row_set() const noexcept {
        return {this,
                nullptr,
                rows->identity(),
                [](const void *context) noexcept {
                    return static_cast<const RowTraversalCounter *>(context)->rows->size();
                },
                [](const void *context, bg::frame::row_id_t id) noexcept {
                    return static_cast<const RowTraversalCounter *>(context)->rows->contains(id);
                },
                [](const void *context, std::size_t position) {
                    return static_cast<const RowTraversalCounter *>(context)->rows->id_at(position);
                },
                [](const void *context, void *target, bg::frame::RowSet::visit_function visit) {
                    const auto &counter = *static_cast<const RowTraversalCounter *>(context);
                    ++counter.traversals;
                    for (auto slot : counter.rows->live_slots())
                        visit(target, counter.rows->id_at_slot(slot));
                }};
    }
};

bg::frame::RowSet selected_row_set(const bg::frame::SelectedRows &selected) noexcept {
    return {std::addressof(selected), nullptr, selected.source().identity,
            [](const void *context) noexcept {
                return static_cast<const bg::frame::SelectedRows *>(context)->size();
            },
            [](const void *context, bg::frame::row_id_t id) noexcept {
                const auto &rows = *static_cast<const bg::frame::SelectedRows *>(context);
                return rows.contains(id) && rows.source().has(id);
            },
            [](const void *context, std::size_t position) {
                const auto &rows = *static_cast<const bg::frame::SelectedRows *>(context);
                if (position >= rows.size())
                    throw std::out_of_range{"selected row position"};
                auto remaining = position;
                auto result = bg::frame::no_row;
                rows.source().each([&](bg::frame::row_id_t id) {
                    if (result != bg::frame::no_row || !rows.contains(id))
                        return;
                    if (remaining-- == 0)
                        result = id;
                });
                if (result == bg::frame::no_row)
                    throw std::out_of_range{"selected row position"};
                return result;
            },
            [](const void *context, void *target, bg::frame::RowSet::visit_function visit) {
                const auto &rows = *static_cast<const bg::frame::SelectedRows *>(context);
                rows.source().each([&](bg::frame::row_id_t id) {
                    if (rows.contains(id))
                        visit(target, id);
                });
            }};
}

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

class FailingResource final : public std::pmr::memory_resource {
  public:
    std::size_t attempts{};
    std::size_t allocations{};
    std::size_t deallocations{};
    std::size_t fail_at{};
    void fail_next() noexcept { fail_at = attempts + 1; }
  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (++attempts == fail_at) throw std::bad_alloc{};
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

void test_empty_and_immutable() {
    using Column = bg::frame::Column<bg::i64>;
    const auto &scalar_type = bg::cpp_scalar_type<Column>();
    const auto &series_type = bg::frame::column_time_series_type<bg::i64>(bg::TimeSeriesGuarantee::ordinary);
    require(bg::TypeRegistry::instance().contains(scalar_type) &&
                bg::TypeRegistry::instance().contains(series_type),
            "column semantic types were not registered");
    auto empty = Column{};
    auto moved = std::move(empty);
    require(empty.view().size() == 0 && moved.view().size() == 0, "empty column view failed");
    require(bg::frame::ColumnView<bg::i64>{}.size() == 0, "default view has rows");
    auto column = Column{{bg::i64{1}, std::nullopt, bg::i64{3}}};
    const auto storage = column.storage();
    const auto owned = bg::ScalarValue<Column>{bg::Scalar<Column>::from(storage)};
    column = Column{{bg::i64{9}}};
    const auto view = owned.view().value();
    require(view.size() == 3 && view.cell(0) == 1 && !view.cell(1) && view.cell(2) == 3, "immutable snapshot failed");

    CountingResource destination_memory;
    auto destination_allocator = bg::allocator{destination_memory};
    const auto copied = bg::ScalarValue<Column>{owned.view(), destination_allocator};
    const auto erased = bg::ScalarValue<>{owned.view(), destination_allocator};
    require(copied.view()->cell(0) == 1 && copied.view()->cell(2) == 3, "typed snapshot copy lost values");
    const auto erased_view = erased.view();
    require(bg::Scalar<Column>::from(erased_view)->cell(0) == 1, "erased snapshot copy lost values");
}

void test_double_column_snapshot() {
    using Column = bg::frame::Column<double>;
    auto column = Column{{1.25, std::nullopt, 3.5}};
    const auto storage = column.storage();
    auto owned = bg::ScalarValue<Column>{bg::Scalar<Column>::from(storage)};
    column = Column{{9.75}};
    const auto view = owned.view().value();
    require(view.size() == 3 && view.cell(0) == 1.25 && !view.cell(1) && view.cell(2) == 3.5,
            "double snapshot lost values or nulls");
}

void test_ordinary_time_series_graph() {
    using Column = bg::frame::Column<bg::i64>;
    const auto copy = bg::node([](bg::In<bg::TS<Column>> input, bg::Out<bg::TS<Column>> output) {
        output.set_value(input.value_view());
    });
    const auto graph = [&](bg::wp<bg::TS<Column>> input) { return copy(input); };
    const auto source = std::vector{Column{{bg::i64{1}, std::nullopt}}, Column{{bg::i64{2}, bg::i64{3}}}};
    const auto result = bg::testing::test_graph(graph, source);
    require(result.size() == 2 && result[0] && result[1], "ordinary graph lost column values");
    require(result[0]->view().value().cell(0) == 1 && !result[0]->view().value().cell(1),
            "ordinary graph changed a column");
    require(result[1]->view().value().cell(1) == 3, "ordinary graph changed a column");
}

void test_native_rows_changes_and_batches() {
    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = bg::allocator{resource};
    auto native = bg::frame::NativeColumn<bg::i64>{};
    auto release = bg::util::scope_exit{[&]() noexcept { native.release(memory); }};
    const auto first_time = bg::MIN_DT + bg::EvaluationTime::duration{1};
    const auto first = native.add(bg::i64{1}, first_time, memory);
    const auto second = native.add(std::nullopt, first_time, memory);
    require(native.id_at(0) == first && native.id_at(1) == second && !native.cell(second),
            "null differs from row missing");
    require(native.row_domain() != nullptr, "row domain identity missing");
    native.set(first, bg::i64{1}, first_time, memory);
    native.set(first, bg::i64{1}, first_time, memory);
    require(native.modified().empty(), "added row was marked modified");
    auto batches = std::size_t{};
    native.for_each_batch([&](const auto &batch) {
        ++batches;
        require(batch.ids.size() == 2 && batch.ids[0] == first && batch.ids[1] == second, "batch lost live slots");
        require(batch.cells.values != nullptr && batch.cells.is_valid(0) && !batch.cells.is_valid(1),
                "batch validity failed");
    });
    require(batches == 1, "unexpected batch split");
    require(native.erase(first, first_time, memory), "erase failed");
    require(native.added().count() == 1 && native.added().contains(second) && native.removed().empty(),
            "add erase did not cancel");
    const auto same_time = native.add(bg::i64{2}, first_time, memory);
    require(same_time != first, "row ID reused within one evaluation time");
    const auto next_time = first_time + bg::EvaluationTime::duration{1};
    const auto later = native.add(bg::i64{3}, next_time, memory);
    require(later == first, "row ID not recycled on later mutation");
    require(!native.erase(999, next_time, memory), "missing row erased");
    try { native.set(999, bg::i64{1}, next_time, memory); throw std::runtime_error{"missing row set succeeded"}; }
    catch (const std::out_of_range &) {}
}

void test_selected_batches_coalesce_without_copying() {
    using namespace bg;
    using namespace bg::frame;

    struct source_batch {
        std::uint64_t first_slot{};
        std::size_t size{};
        const row_id_t *ids{};
        const i64 *values{};
        ColumnBatch<i64>::Validity valid;
    };
    struct selected_batch {
        std::uint64_t first_slot{};
        std::vector<row_id_t> ids;
        std::vector<std::optional<i64>> cells;
        const row_id_t *ids_data{};
        const i64 *values{};
        ColumnBatch<i64>::Validity valid;
    };
    struct scalar_batch {
        std::uint64_t first_slot{};
        std::size_t size{};
        const i64 *values{};
        ColumnBatch<i64>::Validity valid;
    };

    CountingResource resource;
    const auto memory = allocator{resource};
    auto native = NativeColumn<i64>{};
    auto release_native = util::scope_exit{[&]() noexcept { native.release(memory); }};
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    auto original_ids = std::array<row_id_t, 32>{};
    for (auto slot = std::size_t{}; slot != original_ids.size(); ++slot) {
        const auto value = slot >= 8 && slot < 24 ? std::optional<i64>{}
                                                  : std::optional<i64>{static_cast<i64>(slot)};
        original_ids[slot] = native.add(value, first_time, memory);
    }

    const auto second_time = first_time + EvaluationTime::duration{1};
    require(native.erase(original_ids[0], second_time, memory), "selected batch test could not erase slot zero");
    const auto replacement_zero = native.add(1000, second_time, memory);
    const auto third_time = second_time + EvaluationTime::duration{1};
    require(native.erase(original_ids[1], third_time, memory), "selected batch test could not erase slot one");
    const auto replacement_one = native.add(1001, third_time, memory);
    require(replacement_zero > replacement_one && replacement_one == original_ids[0],
            "selected batch test did not create nonmonotonic live row IDs");

    auto selected = SelectedRows{};
    auto release_selected = util::scope_exit{[&]() noexcept { selected.release(memory); }};
    const auto source_rows = RowSet::native(*native.scalar_storage().rows);
    selected.bind(source_rows);
    const auto selected_slots = std::array<std::size_t, 12>{0, 1, 2, 4, 5, 7, 8, 9, 10, 23, 24, 25};
    for (auto offset = selected_slots.size(); offset != 0; --offset)
        selected.include(native.id_at(selected_slots[offset - 1]), memory);
    auto visible = selected_row_set(selected);

    auto storage = native.scalar_storage();
    storage.visible_rows = visible;
    storage.coverage = visible;
    auto series = ColumnSeriesView<i64>{storage};
    auto scalar = Scalar<Column<i64>>::from(storage);
    const auto allocation_count = resource.allocations;

    auto source_batches = std::vector<source_batch>{};
    native.for_each_batch([&](const auto &batch) {
        source_batches.push_back(source_batch{batch.cells.first_slot, batch.cells.size, batch.ids.data(),
                                              batch.cells.values, batch.cells.valid});
    });
    auto series_batches = std::vector<selected_batch>{};
    series.for_each_batch([&](const auto &batch) {
        auto observed = selected_batch{.first_slot = batch.cells.first_slot,
                                       .ids_data = batch.ids.data(),
                                       .values = batch.cells.values,
                                       .valid = batch.cells.valid};
        for (auto position = std::size_t{}; position != batch.cells.size; ++position) {
            observed.ids.push_back(batch.ids[position]);
            observed.cells.push_back(batch.cells.is_valid(position)
                                         ? std::optional<i64>{batch.cells.values[position]}
                                         : std::nullopt);
        }
        series_batches.push_back(std::move(observed));
    });
    auto scalar_batches = std::vector<scalar_batch>{};
    scalar.value().batches([&](const auto &batch) {
        scalar_batches.push_back(scalar_batch{batch.first_slot, batch.size, batch.values, batch.valid});
    });

    require(series_batches.size() == 6, "selected batches were not coalesced at physical holes and block bounds");
    const auto expected_first_slots = std::array<row_id_t, 6>{0, 4, 7, 8, 23, 24};
    const auto expected_sizes = std::array<std::size_t, 6>{3, 2, 1, 3, 1, 2};
    auto flattened = std::size_t{};
    for (auto batch_index = std::size_t{}; batch_index != series_batches.size(); ++batch_index) {
        const auto &batch = series_batches[batch_index];
        require(batch.first_slot == expected_first_slots[batch_index] &&
                    batch.ids.size() == expected_sizes[batch_index] && batch.cells.size() == batch.ids.size(),
                "selected batch crossed a hole or a storage block boundary");
        for (auto position = std::size_t{}; position != batch.ids.size(); ++position, ++flattened) {
            const auto slot = selected_slots[flattened];
            const auto expected_id = slot == 0 ? replacement_zero
                                    : slot == 1 ? replacement_one
                                                 : original_ids[slot];
            const auto expected_value = slot == 0 ? std::optional<i64>{1000}
                                        : slot == 1 ? std::optional<i64>{1001}
                                        : slot >= 8 && slot < 24 ? std::optional<i64>{}
                                                                : std::optional<i64>{static_cast<i64>(slot)};
            require(batch.first_slot + position == slot && batch.ids[position] == expected_id &&
                        batch.cells[position] == expected_value && batch.cells[position] == native.cell(expected_id),
                    "selected batch changed source order or row/value alignment");
        }

        const auto source = std::find_if(source_batches.begin(), source_batches.end(), [&](const auto &candidate) {
            return batch.first_slot >= candidate.first_slot &&
                   batch.first_slot < candidate.first_slot + candidate.size;
        });
        require(source != source_batches.end(), "selected batch has no source storage block");
        const auto offset = batch.first_slot - source->first_slot;
        require(batch.ids_data == source->ids + offset && batch.valid == source->valid &&
                    batch.values == (source->values == nullptr ? nullptr : source->values + offset),
                "selected batch copied IDs, values, or validity storage");
    }

    require(scalar_batches.size() == series_batches.size(), "TSC and scalar views split selected batches differently");
    for (auto index = std::size_t{}; index != series_batches.size(); ++index) {
        const auto &series_batch = series_batches[index];
        const auto &column_batch = scalar_batches[index];
        require(column_batch.first_slot == series_batch.first_slot &&
                    column_batch.size == series_batch.ids.size() && column_batch.values == series_batch.values &&
                    column_batch.valid == series_batch.valid,
                "selected scalar batches did not share the TSC buffers");
    }
    require(resource.allocations == allocation_count,
            "selected batch iteration allocated through the frame memory resource");
}

void test_native_scalar_snapshots_and_allocator() {
    using Column = bg::frame::Column<bg::i64>;
    CountingResource source_memory;
    const auto source_allocator = bg::allocator{source_memory};
    auto native = bg::frame::NativeColumn<bg::i64>{};
    auto release = bg::util::scope_exit{[&]() noexcept { native.release(source_allocator); }};
    const auto time = bg::MIN_DT + bg::EvaluationTime::duration{1};
    native.add(bg::i64{4}, time, source_allocator);
    const auto scalar = bg::Scalar<Column>::from(native.scalar_storage());
    CountingResource snapshot_memory;
    auto snapshot_allocator = bg::allocator{snapshot_memory};
    auto typed = bg::ScalarValue<Column>{scalar, snapshot_allocator};
    auto erased = bg::ScalarValue<>{scalar, snapshot_allocator};
    native.set(native.id_at(0), bg::i64{8}, time, source_allocator);
    require(typed.view()->cell(0) == 4, "typed native scalar did not snapshot");
    const auto erased_view = erased.view();
    require(bg::Scalar<Column>::from(erased_view)->cell(0) == 4, "erased native scalar did not snapshot");
    auto replacement = bg::ScalarValue<Column>{scalar, snapshot_allocator};
    typed = std::move(replacement);
    require(typed.view()->cell(0) == 8, "scalar replacement failed");
}

void test_native_explicit_allocator() {
    CountingResource operation_memory;
    const auto operation_allocator = bg::allocator{operation_memory};
    {
        auto native = bg::frame::NativeColumn<bg::i64>{};
        auto release = bg::util::scope_exit{[&]() noexcept { native.release(operation_allocator); }};
        require(native.scalar_storage().native == nullptr, "empty native scalar was bound");
        const auto time = bg::MIN_DT + bg::EvaluationTime::duration{1};
        const auto id = native.add(std::nullopt, time, operation_allocator);
        require(native.scalar_storage().native != nullptr && native.scalar_storage().rows != nullptr,
                "successful add did not bind native scalar");
        native.set(id, bg::i64{4}, time, operation_allocator);
        native.clear(time + bg::EvaluationTime::duration{1}, operation_allocator);
    }
    require(operation_memory.allocations == operation_memory.deallocations, "explicit mutations leaked storage");
}

void test_null_only_blocks_and_cleanup() {
    CountingResource memory;
    {
        const auto allocator = bg::allocator{memory};
        auto native = bg::frame::NativeColumn<bg::i64>{};
        auto release = bg::util::scope_exit{[&]() noexcept { native.release(allocator); }};
        const auto time = bg::MIN_DT + bg::EvaluationTime::duration{1};
        native.add(std::nullopt, time, allocator);
        native.for_each_batch([&](const auto &batch) {
            require(batch.cells.values == nullptr && !batch.cells.is_valid(0), "null only block allocated a value");
        });
    }
    require(memory.allocations == memory.deallocations, "native allocations leaked");
}

void test_failed_mutations_leave_rows_unchanged() {
    const auto first_time = bg::MIN_DT + bg::EvaluationTime::duration{1};
    const auto second_time = first_time + bg::EvaluationTime::duration{1};

    for (auto failure = std::size_t{1};; ++failure) {
        FailingResource memory;
        auto succeeded = false;
        {
            const auto allocator = bg::allocator{memory};
            auto native = bg::frame::NativeColumn<bg::i64>{};
            auto release = bg::util::scope_exit{[&]() noexcept { native.release(allocator); }};
            for (auto value = 0; value != 70; ++value) native.add(std::nullopt, first_time, allocator);
            const auto id = native.id_at(64);
            memory.fail_at = memory.attempts + failure;
            try {
                native.set(id, bg::i64{2}, second_time, allocator);
                succeeded = true;
            } catch (const std::bad_alloc &) {
                require(native.contains(id) && !native.cell(id), "failed null-to-value set changed a row");
                require(native.added().empty() && native.modified().empty() && native.removed().empty(),
                        "failed null-to-value set changed deltas");
            }
        }
        require(memory.allocations == memory.deallocations, "failed null-to-value set leaked storage");
        if (succeeded) break;
    }

    for (auto failure = std::size_t{1};; ++failure) {
        FailingResource memory;
        auto succeeded = false;
        {
            const auto allocator = bg::allocator{memory};
            auto native = bg::frame::NativeColumn<bg::i64>{};
            auto release = bg::util::scope_exit{[&]() noexcept { native.release(allocator); }};
            for (auto value = 0; value != 70; ++value) native.add(bg::i64{value}, first_time, allocator);
            const auto id = native.id_at(64);
            memory.fail_at = memory.attempts + failure;
            try {
                native.erase(id, second_time, allocator);
                succeeded = true;
            } catch (const std::bad_alloc &) {
                require(native.contains(id) && native.cell(id) == bg::i64{64}, "failed erase changed a row");
                require(native.added().empty() && native.modified().empty() && native.removed().empty(),
                        "failed erase changed deltas");
            }
        }
        require(memory.allocations == memory.deallocations, "failed erase leaked storage");
        if (succeeded) break;
    }

    auto failures = std::size_t{};
    for (auto failure = std::size_t{1};; ++failure) {
        FailingResource memory;
        auto succeeded = false;
        {
            const auto allocator = bg::allocator{memory};
            auto native = bg::frame::NativeColumn<bg::i64>{};
            auto release = bg::util::scope_exit{[&]() noexcept { native.release(allocator); }};
            for (auto value = 0; value != 56; ++value) native.add(bg::i64{value}, first_time, allocator);
            memory.fail_at = memory.attempts + failure;
            try {
                native.add(bg::i64{56}, second_time, allocator);
                succeeded = true;
            } catch (const std::bad_alloc &) {
                ++failures;
                require(native.size() == 56 && native.cell(native.id_at(55)) == bg::i64{55},
                        "failed add changed live rows");
                require(native.added().empty() && native.modified().empty() && native.removed().empty(),
                        "failed add changed deltas");
            }
        }
        require(memory.allocations == memory.deallocations, "failed add leaked storage");
        if (succeeded) {
            require(failures != 0, "add failure injection did not fire");
            break;
        }
    }
}

void test_snapshot_lifetime_and_sparse_batches() {
    using Column = bg::frame::Column<bg::i64>;
    CountingResource destination_memory;
    auto destination_allocator = bg::allocator{destination_memory};
    auto typed = std::optional<bg::ScalarValue<Column>>{};
    auto erased = std::optional<bg::ScalarValue<>>{};
    {
        auto source_memory = std::pmr::monotonic_buffer_resource{};
        const auto source_allocator = bg::allocator{source_memory};
        auto native = bg::frame::NativeColumn<bg::i64>{};
        auto release = bg::util::scope_exit{[&]() noexcept { native.release(source_allocator); }};
        const auto time = bg::MIN_DT + bg::EvaluationTime::duration{1};
        for (auto value = 0; value != 24; ++value) native.add(bg::i64{value}, time, source_allocator);
        native.erase(native.id_at(3), time, source_allocator);
        native.erase(native.id_at(12), time, source_allocator);
        auto batches = std::size_t{};
        native.for_each_batch([&](const auto &batch) {
            ++batches;
            require(batch.ids.size() == batch.cells.size, "batch alignment failed");
        });
        require(batches > 1, "native batches did not cross bunch blocks");
        const auto scalar = bg::Scalar<Column>::from(native.scalar_storage());
        typed.emplace(scalar, destination_allocator);
        erased.emplace(scalar, destination_allocator);
    }
    require(typed->view()->size() == 22, "typed snapshot did not outlive native resource");
    for (auto position = std::size_t{}; position != 22; ++position) {
        const auto expected = position < 3 ? position : position < 12 ? position + 1 : position + 2;
        require(typed->view()->cell(position) == static_cast<bg::i64>(expected), "typed snapshot lost a sparse row");
    }
    const auto erased_view = erased->view();
    require(bg::Scalar<Column>::from(erased_view)->size() == 22, "erased snapshot did not outlive native resource");
}

void test_native_row_slots_map_external_ids_and_reuse_clean_cells() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto physical_rows = NativeRowSlots{};
    auto cells = NativeColumnData<i64>{};
    auto release = util::scope_exit{[&]() noexcept {
        cells.release(memory);
        physical_rows.release(memory);
    }};

    constexpr auto first_id = row_id_t{47};
    constexpr auto second_id = row_id_t{9};
    const auto first_slot = physical_rows.insert(first_id, memory);
    cells.add(physical_rows, first_id, i64{47}, memory);
    physical_rows.erase(no_row);
    physical_rows.erase(second_id);
    require(physical_rows.contains(first_id) && physical_rows.slot_of(first_id) == first_slot &&
                cells.cell(physical_rows, first_id) == i64{47},
            "erasing an unmapped external row ID changed another physical mapping");

    cells.erase_slot(first_slot, first_id, memory);
    physical_rows.erase(first_id);
    const auto reused_slot = physical_rows.insert(second_id, memory);
    cells.add(physical_rows, second_id, i64{9}, memory);
    require(reused_slot == first_slot && !physical_rows.contains(first_id) &&
                cells.cell(physical_rows, second_id) == i64{9},
            "physical slot reuse retained a stale cell or external row mapping");
}

void test_tsc_view_uses_logical_order_and_validates_added_local_rows() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto logical_rows = NativeRows{};
    auto physical_rows = NativeRowSlots{};
    auto cells = NativeColumnData<i64>{};
    auto release = util::scope_exit{[&]() noexcept {
        cells.release(memory);
        physical_rows.release(memory);
        logical_rows.release(memory);
    }};

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto first_id = logical_rows.add(first_time, memory);
    const auto second_id = logical_rows.add(first_time, memory);
    physical_rows.insert(second_id, memory);
    cells.add(physical_rows, second_id, std::nullopt, memory);
    physical_rows.insert(first_id, memory);
    cells.add(physical_rows, first_id, i64{10}, memory);

    auto traversal = RowTraversalCounter{std::addressof(logical_rows)};
    auto visible_rows = traversal.row_set();
    auto input = ColumnStorage<i64>{cells, physical_rows, visible_rows, logical_rows.added(), cells.modified(),
                                    logical_rows.removed(), first_time};
    const auto minimum_time_input = ColumnStorage<i64>{cells, physical_rows, visible_rows, logical_rows.added(),
                                                       cells.modified(), logical_rows.removed(), MIN_DT};
    require(ColumnSeriesView<i64>{minimum_time_input}.change_time() == MIN_DT,
            "local TSC binding did not preserve an explicit minimum change time");
    auto output = ColumnViewOutputStorage<i64>{};
    require(output.bind(input, true, first_time, first_time, memory), "TSC view did not bind its local row mapping");
    auto view = ColumnSeriesView<i64>{output.scalar};
    require(view.id_at(0) == first_id && view.id_at(1) == second_id && view.cell_at(0) == i64{10} &&
                !view.cell_at(1),
            "TSC view exposed physical slot order instead of logical row order");
    auto batch_ids = std::vector<row_id_t>{};
    auto batch_values = std::vector<std::optional<i64>>{};
    view.for_each_batch([&](const auto &batch) {
        for (auto position = std::size_t{}; position != batch.cells.size; ++position) {
            batch_ids.push_back(batch.ids[position]);
            batch_values.push_back(batch.cells.is_valid(position)
                                       ? std::optional<i64>{batch.cells.values[position]}
                                       : std::nullopt);
        }
    });
    require(batch_ids == std::vector<row_id_t>{first_id, second_id} &&
                batch_values == std::vector<std::optional<i64>>{i64{10}, std::nullopt},
            "local physical batches lost source ID order or nullable cell alignment");

    const auto second_time = first_time + EvaluationTime::duration{1};
    const auto third_id = logical_rows.add(second_time, memory);
    physical_rows.insert(third_id, memory);
    cells.add(physical_rows, third_id, i64{30}, memory);
    visible_rows = traversal.row_set();
    input = ColumnStorage<i64>{cells, physical_rows, visible_rows, logical_rows.added(), cells.modified(),
                               logical_rows.removed(), second_time};
    traversal.traversals = 0;
    require(output.bind(input, true, second_time, second_time, memory) && traversal.traversals == 0,
            "TSC view traversed unchanged local rows instead of validating the added row delta");
    view = ColumnSeriesView<i64>{output.scalar};
    require(view.cell_at(2) == i64{30}, "TSC view did not read a newly added local physical slot");

    const auto third_time = second_time + EvaluationTime::duration{1};
    static_cast<void>(logical_rows.add(third_time, memory));
    visible_rows = traversal.row_set();
    input = ColumnStorage<i64>{cells, physical_rows, visible_rows, logical_rows.added(), cells.modified(),
                               logical_rows.removed(), third_time};
    traversal.traversals = 0;
    auto rejected = false;
    try {
        static_cast<void>(output.bind(input, true, third_time, third_time, memory));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected && traversal.traversals == 0,
            "TSC view accepted a new logical row without a local physical mapping");
}

void test_tsc_view_rejects_native_rows_from_another_logical_domain() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto value_rows = NativeRows{};
    auto visible_rows = NativeRows{};
    auto cells = NativeColumnData<i64>{};
    auto release = util::scope_exit{[&]() noexcept {
        cells.release(memory);
        value_rows.release(memory);
        visible_rows.release(memory);
    }};

    const auto time = MIN_DT + EvaluationTime::duration{1};
    const auto value_id = value_rows.add(time, memory);
    const auto visible_id = visible_rows.add(time, memory);
    cells.add(value_rows, value_id, i64{10}, memory);
    require(value_id == visible_id && value_rows.identity() != visible_rows.identity(),
            "test setup did not create overlapping row IDs in distinct logical domains");

    const auto input = ColumnStorage<i64>{cells, value_rows, RowSet::native(visible_rows), &value_rows.added(),
                                          &cells.modified(), &value_rows.removed(), time};
    auto output = ColumnViewOutputStorage<i64>{};
    auto rejected = false;
    try {
        static_cast<void>(output.bind(input, true, time, time, memory));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "TSC view accepted native column deltas for an overlapping foreign row ID");
}

void test_tsc_view_revalidates_coverage_when_mapping_changes() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto first = NativeColumn<i64>{};
    auto second = NativeColumn<i64>{};
    auto release = util::scope_exit{[&]() noexcept {
        second.release(memory);
        first.release(memory);
    }};

    const auto time = MIN_DT + EvaluationTime::duration{1};
    const auto first_id = first.add(i64{10}, time, memory);
    static_cast<void>(first.add(i64{20}, time, memory));
    static_cast<void>(second.add(i64{30}, time, memory));
    const auto uncovered_id = second.add(i64{40}, time, memory);
    require(first_id != uncovered_id, "test setup did not create overlapping row IDs");

    auto selected = SelectedRows{};
    auto release_selected = util::scope_exit{[&]() noexcept { selected.release(memory); }};
    selected.bind(RowSet::native(*first.scalar_storage().rows));
    selected.include(first_id, memory);
    auto first_input = first.scalar_storage();
    first_input.visible_rows = selected_row_set(selected);
    first_input.coverage = selected_row_set(selected);

    auto output = ColumnViewOutputStorage<i64>{};
    require(output.bind(first_input, true, time, time, memory), "TSC view did not accept initial covered rows");

    auto second_input = second.scalar_storage();
    second_input.coverage = selected_row_set(selected);
    auto rejected = false;
    try {
        static_cast<void>(output.bind(second_input, true, time, time, memory));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "TSC view accepted missing retained coverage after its source mapping changed");
}

void test_tsc_view_clears_modified_rows_removed_during_the_same_cycle() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto source = NativeColumn<i64>{};
    auto release = util::scope_exit{[&]() noexcept { source.release(memory); }};
    auto output = ColumnViewOutputStorage<i64>{};

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto id = source.add(i64{10}, first_time, memory);
    require(output.bind(source.scalar_storage(), true, first_time, first_time, memory),
            "TSC view did not bind its initial row");

    const auto second_time = first_time + EvaluationTime::duration{1};
    source.set(id, i64{20}, second_time, memory);
    require(output.bind(source.scalar_storage(), true, second_time, second_time, memory) &&
                output.scalar.modified->contains(id),
            "TSC view did not report a row modified during the cycle");

    source.erase(id, second_time, memory);
    require(output.bind(source.scalar_storage(), true, second_time, second_time, memory) &&
                !output.scalar.modified->contains(id) && output.scalar.removed->contains(id),
            "TSC view retained a modification after the row was removed in the same cycle");
}

void test_aligned_batches_borrow_shared_mapping_and_accept_requested_subsets() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto rows = NativeRows{};
    auto left_cells = NativeColumnData<i64>{};
    auto right_cells = NativeColumnData<double>{};
    auto release = util::scope_exit{[&]() noexcept {
        right_cells.release(memory);
        left_cells.release(memory);
        rows.release(memory);
    }};

    const auto time = MIN_DT + EvaluationTime::duration{1};
    auto ids = std::array<row_id_t, 12>{};
    for (auto position = std::size_t{}; position != ids.size(); ++position) {
        ids[position] = rows.add(time, memory);
        left_cells.add(rows, ids[position], position == 2 ? std::nullopt : std::optional<i64>{position}, memory);
        right_cells.add(rows, ids[position], position == 9 ? std::nullopt : std::optional<double>{position + 0.5},
                        memory);
    }

    auto left_storage = ColumnStorage<i64>{left_cells, rows};
    auto right_storage = ColumnStorage<double>{right_cells, rows};
    auto left = ColumnSeriesView<i64>{left_storage};
    auto right = ColumnSeriesView<double>{right_storage};
    const auto first_left_block = left_cells.block_containing(rows, ids[0]);
    const auto first_right_block = right_cells.block_containing(rows, ids[0]);
    auto visited = std::size_t{};
    auto batches = std::size_t{};
    left.for_each_aligned_batch(right, [&](const auto &batch) {
        ++batches;
        require(batch.ids.size() == batch.left.size && batch.ids.size() == batch.right.size,
                "aligned native batches have different lengths");
        if (batches == 1) {
            require(batch.left.values == first_left_block.data && batch.right.values == first_right_block.data,
                    "aligned native batches copied values instead of borrowing source blocks");
        }
        for (auto position = std::size_t{}; position != batch.ids.size(); ++position) {
            const auto expected = static_cast<std::size_t>(batch.ids[position]);
            require(batch.ids[position] == ids[expected], "aligned native batches changed row order");
            if (expected == 2) {
                require(!batch.left.is_valid(position), "aligned native batch lost a left null");
            } else {
                require(batch.left.is_valid(position) && batch.left.values[position] == static_cast<i64>(expected),
                        "aligned native batch changed a left value");
            }
            if (expected == 9) {
                require(!batch.right.is_valid(position), "aligned native batch lost a right null");
            } else {
                require(batch.right.is_valid(position) &&
                            batch.right.values[position] == static_cast<double>(expected) + 0.5,
                        "aligned native batch changed a right value");
            }
            ++visited;
        }
    });
    require(visited == ids.size() && batches >= 2, "aligned native traversal did not split at storage blocks");

    auto right_visible = SelectedRows{};
    auto request = SelectedRows{};
    auto release_selected = util::scope_exit{[&]() noexcept {
        request.release(memory);
        right_visible.release(memory);
    }};
    right_visible.bind(RowSet::native(rows));
    request.bind(RowSet::native(rows));
    for (auto position = std::size_t{}; position != 8; ++position)
        right_visible.include(ids[position], memory);
    request.include(ids[1], memory);
    request.include(ids[3], memory);
    auto restricted_storage = ColumnStorage<double>{right_cells, rows.slots(), selected_row_set(right_visible), rows.added(),
                                                    right_cells.modified(), rows.removed(), time};
    auto restricted = ColumnSeriesView<double>{restricted_storage};
    visited = 0;
    left.for_each_aligned_batch(restricted, selected_row_set(request), [&](const auto &batch) {
        for (auto position = std::size_t{}; position != batch.ids.size(); ++position) {
            const auto expected_id = visited == 0 ? ids[1] : ids[3];
            require(batch.ids[position] == expected_id, "aligned batches changed requested row order");
            require(batch.left.is_valid(position) && batch.right.is_valid(position),
                    "aligned batches lost requested valid cells");
            ++visited;
        }
    });
    require(visited == 2, "aligned batches rejected a requested subset present in both views");

    auto callback_invoked = false;
    auto rejected = false;
    try {
        left.for_each_aligned_batch(restricted, [&](const auto &) { callback_invoked = true; });
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected && !callback_invoked,
            "aligned traversal invoked its callback before validating every requested row");
}

void test_aligned_batches_stop_at_shared_id_and_cell_block_boundaries() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto rows = NativeRows{};
    auto first_cells = NativeColumnData<double>{};
    auto second_cells = NativeColumnData<double>{};
    auto third_cells = NativeColumnData<double>{};
    auto release = util::scope_exit{[&]() noexcept {
        third_cells.release(memory);
        second_cells.release(memory);
        first_cells.release(memory);
        rows.release(memory);
    }};

    const auto time = MIN_DT + EvaluationTime::duration{1};
    auto ids = std::array<row_id_t, 12>{};
    for (auto position = std::size_t{}; position != ids.size(); ++position) {
        ids[position] = rows.add(time, memory);
        first_cells.add(rows, ids[position], static_cast<double>(position + 1), memory);
        second_cells.add(rows, ids[position], static_cast<double>(position + 11), memory);
        third_cells.add(rows, ids[position], static_cast<double>(position + 21), memory);
    }

    const auto visible = RowSet::native(rows);
    auto selected = SelectedRows{};
    auto release_selected = util::scope_exit{[&]() noexcept { selected.release(memory); }};
    selected.bind(visible);
    for (auto position = std::size_t{6}; position != 10; ++position)
        selected.include(ids[position], memory);

    const auto first_storage = ColumnStorage<double>{first_cells, rows};
    const auto second_storage = ColumnStorage<double>{second_cells, rows};
    const auto third_storage = ColumnStorage<double>{third_cells, rows};
    const auto columns = std::array{ColumnSeriesView<double>{first_storage},
                                    ColumnSeriesView<double>{second_storage},
                                    ColumnSeriesView<double>{third_storage}};
    auto scratch = std::pmr::monotonic_buffer_resource{};
    auto observed_batches = std::size_t{};
    auto observed_rows = std::size_t{};
    ColumnSeriesView<double>::for_each_aligned_batch(columns, selected_row_set(selected), scratch,
                                                      [&](std::span<const row_id_t> batch_ids,
                                                          std::span<const ColumnBatch<double>> batches) {
        require(batch_ids.size() == 2 && batches.size() == columns.size(),
                "aligned n-ary batches crossed a shared storage-block boundary");
        for (auto position = std::size_t{}; position != batch_ids.size(); ++position) {
            const auto expected = 6 + observed_rows + position;
            require(batch_ids[position] == ids[expected], "aligned n-ary batches changed requested row order");
            require(batches[0].is_valid(position) && batches[1].is_valid(position) &&
                        batches[2].is_valid(position),
                    "aligned n-ary batches lost a valid cell");
            require(batches[0].values[position] == static_cast<double>(expected + 1) &&
                        batches[1].values[position] == static_cast<double>(expected + 11) &&
                        batches[2].values[position] == static_cast<double>(expected + 21),
                    "aligned n-ary batches read through a different storage block");
        }
        observed_rows += batch_ids.size();
        ++observed_batches;
    });
    require(observed_batches == 2 && observed_rows == 4,
            "aligned n-ary batches did not split at the ID and cell storage-block boundaries");
}

void test_column_batch_subspan_advances_value_and_validity_offsets() {
    using namespace bg;
    using namespace bg::frame;

    const auto bitmap_values = std::array<double, 5>{1, 2, 3, 4, 5};
    const auto bitmap = std::array<std::uint8_t, 1>{0b00101010};
    const auto bitmap_batch = ColumnBatch<double>{20, bitmap_values.size(), bitmap_values.data(),
                                                   ColumnBatch<double>::Validity::from_bitmap(bitmap.data(), 1)};
    const auto bitmap_slice = bitmap_batch.subspan(1, 3);
    require(bitmap_slice.first_slot == 21 && bitmap_slice.size == 3 &&
                bitmap_slice.values == bitmap_values.data() + 1 && !bitmap_slice.is_valid(0) &&
                bitmap_slice.is_valid(1) && !bitmap_slice.is_valid(2),
            "column batch subspan did not advance its value and bitmap offsets");

    auto resource = std::pmr::monotonic_buffer_resource{};
    auto valid_slots = util::index<>{};
    auto release = util::scope_exit{[&]() noexcept { valid_slots.release(resource); }};
    valid_slots.set(9, resource);
    valid_slots.set(11, resource);
    const auto indexed_batch = ColumnBatch<double>{7, bitmap_values.size(), bitmap_values.data(),
                                                    ColumnBatch<double>::Validity::from_index(valid_slots)};
    const auto indexed_slice = indexed_batch.subspan(2, 2);
    require(indexed_slice.first_slot == 9 && indexed_slice.is_valid(0) && !indexed_slice.is_valid(1),
            "column batch subspan did not advance its index validity offset");

    const auto null_batch = ColumnBatch<double>{5, 2, nullptr, ColumnBatch<double>::Validity::all_invalid()};
    const auto null_slice = null_batch.subspan(1, 1);
    require(null_slice.first_slot == 6 && null_slice.values == nullptr && !null_slice.is_valid(0),
            "column batch subspan changed an all-null batch");
}

void test_aligned_batches_gather_distinct_mappings_and_validate_before_callback() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto rows = NativeRows{};
    auto left_rows = NativeRowSlots{};
    auto right_rows = NativeRowSlots{};
    auto left_cells = NativeColumnData<i64>{};
    auto aligned_cells = NativeColumnData<i64>{};
    auto right_cells = NativeColumnData<double>{};
    auto release = util::scope_exit{[&]() noexcept {
        right_cells.release(memory);
        aligned_cells.release(memory);
        left_cells.release(memory);
        right_rows.release(memory);
        left_rows.release(memory);
        rows.release(memory);
    }};

    const auto time = MIN_DT + EvaluationTime::duration{1};
    auto ids = std::array<row_id_t, 4>{};
    for (auto position = std::size_t{}; position != ids.size(); ++position)
        ids[position] = rows.add(time, memory);
    for (auto id : ids) {
        left_rows.insert(id, memory);
        left_cells.add(left_rows, id, id == ids[1] ? std::nullopt : std::optional<i64>{id + 10}, memory);
    }
    for (auto position = ids.size(); position != 0; --position) {
        const auto id = ids[position - 1];
        right_rows.insert(id, memory);
        right_cells.add(right_rows, id, id == ids[3] ? std::nullopt : std::optional<double>{id + 20.5}, memory);
        aligned_cells.add(right_rows, id, id == ids[3] ? std::nullopt : std::optional<i64>{id + 30}, memory);
    }

    const auto visible = RowSet::native(rows);
    auto left_storage = ColumnStorage<i64>{left_cells, left_rows, visible, rows.added(), left_cells.modified(),
                                           rows.removed(), time};
    auto aligned_storage = ColumnStorage<i64>{aligned_cells, right_rows, visible, rows.added(), aligned_cells.modified(),
                                              rows.removed(), time};
    auto right_storage = ColumnStorage<double>{right_cells, right_rows, visible, rows.added(), right_cells.modified(),
                                               rows.removed(), time};
    auto left = ColumnSeriesView<i64>{left_storage};
    auto aligned = ColumnSeriesView<i64>{aligned_storage};
    auto right = ColumnSeriesView<double>{right_storage};
    auto request = SelectedRows{};
    auto release_selected = util::scope_exit{[&]() noexcept { request.release(memory); }};
    request.bind(visible);
    request.include(ids[1], memory);
    request.include(ids[3], memory);

    auto visited = std::size_t{};
    left.for_each_aligned_batch(right, selected_row_set(request), [&](const auto &batch) {
        require(batch.ids.size() == 2 && batch.ids[0] == ids[1] && batch.ids[1] == ids[3],
                "gathered batches changed requested row order");
        require(!batch.left.is_valid(0) && batch.left.is_valid(1), "gathered left validity is incorrect");
        require(batch.right.is_valid(0) && !batch.right.is_valid(1), "gathered right validity is incorrect");
        require(batch.right.values[0] == 21.5 && batch.left.values[1] == 13,
                "gathered batches read cells using physical positions instead of row IDs");
        require(batch.left.values != left_cells.block_containing(left_rows, ids[1]).data &&
                    batch.right.values != right_cells.block_containing(right_rows, ids[1]).data,
                "aligned batches did not gather values for distinct physical mappings");
        visited += batch.ids.size();
    });
    require(visited == 2, "gathered batches omitted requested rows");

    const auto aligned_columns = std::array{left, aligned};
    auto scratch = std::pmr::monotonic_buffer_resource{};
    auto aligned_rows = std::size_t{};
    ColumnSeriesView<i64>::for_each_aligned_batch(
        std::span<const ColumnSeriesView<i64>>{aligned_columns}, selected_row_set(request), scratch,
        [&](std::span<const row_id_t> batch_ids, std::span<const ColumnBatch<i64>> batches) {
            require(batch_ids.size() == 2 && batches.size() == aligned_columns.size() &&
                        batch_ids[0] == ids[1] && batch_ids[1] == ids[3],
                    "n-ary gathered batches changed requested row order");
            require(!batches[0].is_valid(0) && batches[0].is_valid(1) && batches[1].is_valid(0) &&
                        !batches[1].is_valid(1),
                    "n-ary gathered batches produced an incorrect null bitmap");
            require(batches[0].values[1] == ids[3] + 10 && batches[1].values[0] == ids[1] + 30,
                    "n-ary gathered batches read a value from the wrong physical slot");
            require(batches[0].values != left_cells.block_containing(left_rows, ids[1]).data &&
                        batches[1].values != aligned_cells.block_containing(right_rows, ids[1]).data,
                    "n-ary gathered batches did not gather distinct physical mappings");
            aligned_rows += batch_ids.size();
        });
    require(aligned_rows == 2, "n-ary gathered batches omitted requested rows");

    const auto missing_id = ids[1];
    const auto missing_slot = right_rows.slot_of(missing_id);
    right_cells.erase_slot(missing_slot, missing_id, memory);
    aligned_cells.erase_slot(missing_slot, missing_id, memory);
    right_rows.erase(missing_id);
    auto callback_invoked = false;
    auto rejected = false;
    try {
        left.for_each_aligned_batch(right, selected_row_set(request), [&](const auto &) { callback_invoked = true; });
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected && !callback_invoked,
            "aligned gathered batches invoked their callback before validating requested row coverage");

    rejected = false;
    try {
        ColumnSeriesView<i64>::for_each_aligned_batch(
            std::span<const ColumnSeriesView<i64>>{aligned_columns}, selected_row_set(request), scratch,
            [&](std::span<const row_id_t>, std::span<const ColumnBatch<i64>>) { callback_invoked = true; });
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected && !callback_invoked,
            "n-ary gathered batches invoked their callback before validating requested row coverage");

    auto foreign_rows = NativeRows{};
    auto release_foreign = util::scope_exit{[&]() noexcept { foreign_rows.release(memory); }};
    static_cast<void>(foreign_rows.add(time, memory));
    auto wrong_domain = false;
    try {
        left.for_each_aligned_batch(right, RowSet::native(foreign_rows), [&](const auto &) {
            callback_invoked = true;
        });
    } catch (const std::invalid_argument &) {
        wrong_domain = true;
    }
    require(wrong_domain && !callback_invoked, "aligned batches accepted a different logical row domain");
}

void test_aligned_batches_gather_more_than_two_scratch_batches() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto rows = NativeRows{};
    auto left_rows = NativeRowSlots{};
    auto right_rows = NativeRowSlots{};
    auto left_cells = NativeColumnData<i64>{};
    auto right_cells = NativeColumnData<i64>{};
    auto release = util::scope_exit{[&]() noexcept {
        right_cells.release(memory);
        left_cells.release(memory);
        right_rows.release(memory);
        left_rows.release(memory);
        rows.release(memory);
    }};

    constexpr auto row_count = std::size_t{300};
    const auto time = MIN_DT + EvaluationTime::duration{1};
    auto ids = std::array<row_id_t, row_count>{};
    for (auto position = std::size_t{}; position != row_count; ++position)
        ids[position] = rows.add(time, memory);
    for (const auto id : ids)
        left_rows.insert(id, memory);
    for (auto position = row_count; position != 0; --position)
        right_rows.insert(ids[position - 1], memory);

    for (auto position = std::size_t{}; position != row_count; ++position) {
        const auto id = ids[position];
        const auto left_value = position == 127 || position == 255 ? std::nullopt : std::optional<i64>{id + 10};
        const auto right_value = position == 128 || position == 256 ? std::nullopt : std::optional<i64>{id + 20};
        left_cells.add(left_rows, id, left_value, memory);
        right_cells.add(right_rows, id, right_value, memory);
    }

    const auto visible = RowSet::native(rows);
    auto left_storage = ColumnStorage<i64>{left_cells, left_rows, visible, rows.added(), left_cells.modified(),
                                           rows.removed(), time};
    auto right_storage = ColumnStorage<i64>{right_cells, right_rows, visible, rows.added(), right_cells.modified(),
                                            rows.removed(), time};
    auto left = ColumnSeriesView<i64>{left_storage};
    auto right = ColumnSeriesView<i64>{right_storage};

    auto visited = std::size_t{};
    auto batches = std::size_t{};
    left.for_each_aligned_batch(right, [&](const auto &batch) {
        ++batches;
        for (auto offset = std::size_t{}; offset != batch.ids.size(); ++offset) {
            const auto position = visited + offset;
            require(batch.ids[offset] == ids[position], "multi-batch gathering changed logical row order");
            const auto left_valid = position != 127 && position != 255;
            const auto right_valid = position != 128 && position != 256;
            require(batch.left.is_valid(offset) == left_valid && batch.right.is_valid(offset) == right_valid,
                    "multi-batch gathering retained stale validity bits across scratch batches");
            if (left_valid)
                require(batch.left.values[offset] == ids[position] + 10,
                        "multi-batch gathering read a left value from the wrong physical slot");
            if (right_valid)
                require(batch.right.values[offset] == ids[position] + 20,
                        "multi-batch gathering read a right value from the wrong physical slot");
        }
        visited += batch.ids.size();
    });

    require(visited == row_count && batches == 3,
            "gathering did not flush and reset fixed-size scratch batches across more than 256 rows");
}

void test_aligned_batches_keep_null_only_blocks_unreadable() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto rows = NativeRows{};
    auto left_cells = NativeColumnData<i64>{};
    auto right_cells = NativeColumnData<double>{};
    auto third_cells = NativeColumnData<double>{};
    auto release = util::scope_exit{[&]() noexcept {
        third_cells.release(memory);
        right_cells.release(memory);
        left_cells.release(memory);
        rows.release(memory);
    }};

    constexpr auto row_count = std::size_t{300};
    const auto time = MIN_DT + EvaluationTime::duration{1};
    for (auto position = std::size_t{}; position != row_count; ++position) {
        const auto id = rows.add(time, memory);
        left_cells.add(rows, id, std::nullopt, memory);
        right_cells.add(rows, id, std::nullopt, memory);
        third_cells.add(rows, id, std::nullopt, memory);
    }

    auto left_storage = ColumnStorage<i64>{left_cells, rows};
    auto right_storage = ColumnStorage<double>{right_cells, rows};
    auto third_storage = ColumnStorage<double>{third_cells, rows};
    auto left = ColumnSeriesView<i64>{left_storage};
    auto right = ColumnSeriesView<double>{right_storage};
    auto visited = std::size_t{};
    auto batches = std::size_t{};
    left.for_each_aligned_batch(right, [&](const auto &batch) {
        ++batches;
        require(batch.left.values == nullptr && batch.right.values == nullptr,
                "an all-null native block exposed a value buffer");
        for (auto offset = std::size_t{}; offset != batch.ids.size(); ++offset) {
            require(!batch.left.is_valid(offset) && !batch.right.is_valid(offset),
                    "an all-null native block marked a value valid");
            ++visited;
        }
    });

    require(visited == row_count && batches > 1, "all-null native blocks were omitted from aligned traversal");

    const auto columns = std::array{right, ColumnSeriesView<double>{third_storage}};
    auto scratch = std::pmr::monotonic_buffer_resource{};
    visited = 0;
    batches = 0;
    ColumnSeriesView<double>::for_each_aligned_batch(
        std::span<const ColumnSeriesView<double>>{columns}, RowSet::native(rows), scratch,
        [&](std::span<const row_id_t> batch_ids, std::span<const ColumnBatch<double>> column_batches) {
            ++batches;
            require(column_batches.size() == columns.size(), "all-null n-ary batch omitted a column");
            for (auto column = std::size_t{}; column != column_batches.size(); ++column) {
                require(column_batches[column].values == nullptr,
                        "an all-null n-ary native block exposed a value buffer");
                for (auto offset = std::size_t{}; offset != batch_ids.size(); ++offset) {
                    require(!column_batches[column].is_valid(offset),
                            "an all-null n-ary native block marked a value valid");
                    ++visited;
                }
            }
        });
    require(visited == row_count * columns.size() && batches > 1,
            "all-null n-ary native blocks were omitted from aligned traversal");
}
} // namespace

int main() {
    try {
        test_empty_and_immutable();
        test_double_column_snapshot();
        test_ordinary_time_series_graph();
        test_native_rows_changes_and_batches();
        test_selected_batches_coalesce_without_copying();
        test_native_scalar_snapshots_and_allocator();
        test_native_explicit_allocator();
        test_null_only_blocks_and_cleanup();
        test_failed_mutations_leave_rows_unchanged();
        test_snapshot_lifetime_and_sparse_batches();
        test_native_row_slots_map_external_ids_and_reuse_clean_cells();
        test_tsc_view_uses_logical_order_and_validates_added_local_rows();
        test_tsc_view_rejects_native_rows_from_another_logical_domain();
        test_tsc_view_revalidates_coverage_when_mapping_changes();
        test_tsc_view_clears_modified_rows_removed_during_the_same_cycle();
        test_aligned_batches_borrow_shared_mapping_and_accept_requested_subsets();
        test_aligned_batches_stop_at_shared_id_and_cell_block_boundaries();
        test_column_batch_subspan_advances_value_and_validity_offsets();
        test_aligned_batches_gather_distinct_mappings_and_validate_before_callback();
        test_aligned_batches_gather_more_than_two_scratch_batches();
        test_aligned_batches_keep_null_only_blocks_unreadable();
        std::cout << "bg_frame_column_test passed\n";
    } catch (const std::exception &error) {
        std::cerr << "bg_frame_column_test: " << error.what() << '\n';
        return 1;
    }
}
