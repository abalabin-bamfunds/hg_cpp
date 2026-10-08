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
    require(result[0]->value().cell(0) == 1 && !result[0]->value().cell(1), "ordinary graph changed a column");
    require(result[1]->value().cell(1) == 3, "ordinary graph changed a column");
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
    auto visible = selected.rows();

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
        std::cout << "bg_frame_column_test passed\n";
    } catch (const std::exception &error) {
        std::cerr << "bg_frame_column_test: " << error.what() << '\n';
        return 1;
    }
}
