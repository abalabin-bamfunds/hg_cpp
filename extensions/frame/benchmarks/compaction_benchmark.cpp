#include <bg/frame/arrow.hpp>
#include <bg/frame/frame.hpp>

#include <arrow/compute/api.h>
#include <arrow/compute/initialize.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;

constexpr auto row_count = std::size_t{32'768};
constexpr auto computed_row_count = std::size_t{8'192};
constexpr auto sample_count = std::size_t{5};
constexpr auto timing_repeats = std::size_t{3};
constexpr auto consumer_counts = std::array{std::size_t{1}, std::size_t{2}, std::size_t{5}, std::size_t{10}};

template <template <typename, auto...> typename Field> struct NarrowRows {
    Field<i64> value;
};

template <template <typename, auto...> typename Field> struct WideRows {
    Field<i64> value0;
    Field<i64> value1;
    Field<i64> value2;
    Field<i64> value3;
    Field<i64> value4;
    Field<i64> value5;
    Field<i64> value6;
    Field<i64> value7;
};

template <template <typename, auto...> typename Field> struct ComputedPairRows {
    Field<i64> left;
    Field<i64> right;
};

class CountingResource final : public std::pmr::memory_resource {
  public:
    std::size_t allocations{};
    std::size_t bytes_allocated{};

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        bytes_allocated += bytes;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

enum class Deletion { dense, one_slot, one_slot_adversarial_left, leading_ten_percent, trailing_ten_percent,
                      clustered_ten_percent, delete_then_add_more, scattered_half_percent, scattered_five_percent,
                      scattered_twenty_percent, random_five_percent };

constexpr auto deletions = std::array{
    std::pair{Deletion::dense, "dense"},
    std::pair{Deletion::one_slot, "one_slot_shift"},
    std::pair{Deletion::one_slot_adversarial_left, "one_slot_adversarial_left"},
    std::pair{Deletion::leading_ten_percent, "leading_10_percent"},
    std::pair{Deletion::trailing_ten_percent, "trailing_10_percent"},
    std::pair{Deletion::clustered_ten_percent, "clustered_10_percent"},
    std::pair{Deletion::delete_then_add_more, "delete_10_add_20_percent"},
    std::pair{Deletion::scattered_half_percent, "scattered_0_5_percent"},
    std::pair{Deletion::scattered_five_percent, "scattered_5_percent"},
    std::pair{Deletion::scattered_twenty_percent, "scattered_20_percent"},
    std::pair{Deletion::random_five_percent, "random_5_percent_seed_271828"},
};

const auto random_five_percent = [] {
    auto result = std::array<bool, row_count>{};
    auto random = std::mt19937{271'828};
    for (auto &remove : result)
        remove = random() % 20 == 0;
    return result;
}();

struct PhaseSample {
    std::size_t live_rows{};
    std::int64_t sum{};
    std::size_t native_batches{};
    std::size_t borrowed_arrow_chunks{};
    std::size_t dense_gather_chunks{};
    std::array<double, consumer_counts.size()> native_ms{};
    std::array<double, consumer_counts.size()> borrowed_arrow_sum_ms{};
    std::array<double, consumer_counts.size()> dense_gather_sum_ms{};
    std::array<double, consumer_counts.size()> id_read_ms{};
};

struct CompactionSample {
    double plan_ms{};
    std::size_t plan_allocations{};
    std::size_t plan_bytes_allocated{};
    std::size_t planned_moves{};
    std::uint64_t plan_checksum{};
    double elapsed_ms{};
    std::size_t allocations{};
    std::size_t bytes_allocated{};
    std::size_t moved_rows{};
    std::size_t value_bytes_moved{};
};

struct CaseSample {
    PhaseSample before;
    PhaseSample after;
    CompactionSample compaction;
};

struct ComputedPhaseSample {
    std::size_t live_rows{};
    std::size_t physical_runs{};
    std::size_t visible_batches{};
    std::size_t aligned_consumer_batches{};
    std::size_t borrowed_arrow_chunks{};
    std::int64_t sum{};
    double visible_ms{};
    double aligned_consumer_ms{};
    double borrowed_arrow_sum_ms{};
};

struct ComputedCaseSample {
    ComputedPhaseSample before;
    ComputedPhaseSample after;
    double compact_ms{};
    std::size_t moved_rows{};
};

struct BenchmarkSink {
    std::int64_t value{};
};

BenchmarkSink sink;

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
Row<Schema> make_row(std::size_t row, std::index_sequence<Index...>) {
    return Row<Schema>{Cell<i64>{static_cast<i64>((row + 1) * (Index + 1))}...};
}

template <template <template <typename, auto...> typename> typename Schema>
void seed(NativeFrame<Schema> &frame, allocator memory, std::vector<row_id_t> &ids) {
    ids.reserve(row_count);
    const auto time = MIN_DT + EvaluationTime::duration{1};
    for (auto row = std::size_t{}; row != row_count; ++row) {
        const auto value = make_row<Schema>(row, std::make_index_sequence<NativeFrame<Schema>::field_count>{});
        ids.push_back(frame.add(value, time, memory));
    }
}

bool should_delete(Deletion deletion, std::size_t row) {
    switch (deletion) {
    case Deletion::dense:
        return false;
    case Deletion::one_slot:
        return row == row_count * 7 / 8;
    case Deletion::one_slot_adversarial_left:
        return row == row_count * 3 / 4;
    case Deletion::leading_ten_percent:
        return row < row_count / 10;
    case Deletion::trailing_ten_percent:
        return row >= row_count * 9 / 10;
    case Deletion::clustered_ten_percent:
    case Deletion::delete_then_add_more:
        return row >= row_count * 45 / 100 && row < row_count * 55 / 100;
    case Deletion::scattered_half_percent:
        return row % 200 == 0;
    case Deletion::scattered_five_percent:
        return row % 20 == 0;
    case Deletion::scattered_twenty_percent:
        return row % 5 == 0;
    case Deletion::random_five_percent:
        return random_five_percent[row];
    }
    std::terminate();
}

std::size_t deleted_row_count(Deletion deletion) {
    auto result = std::size_t{};
    for (auto row = std::size_t{}; row != row_count; ++row)
        result += should_delete(deletion, row);
    return result;
}

template <template <template <typename, auto...> typename> typename Schema>
void erase_rows(NativeFrame<Schema> &frame, std::span<const row_id_t> ids, Deletion deletion, allocator memory) {
    const auto time = MIN_DT + EvaluationTime::duration{2};
    for (auto row = std::size_t{}; row != ids.size(); ++row)
        if (should_delete(deletion, row))
            frame.erase(ids[row], time, memory);
}

template <template <template <typename, auto...> typename> typename Schema>
void refill_rows(NativeFrame<Schema> &frame, allocator memory, std::vector<row_id_t> &ids, std::size_t count) {
    const auto time = MIN_DT + EvaluationTime::duration{2};
    for (auto row = std::size_t{}; row != count; ++row) {
        const auto value = make_row<Schema>(row_count + row,
                                            std::make_index_sequence<NativeFrame<Schema>::field_count>{});
        ids.push_back(frame.add(value, time, memory));
    }
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
void plan_compaction(NativeFrame<Schema> &frame, NativeRows &rows, allocator memory, CompactionSample &sample,
                     std::index_sequence<Index...>) {
    const auto block_end = [&](row_id_t slot) {
        auto end = std::numeric_limits<std::uint64_t>::max();
        ((end = std::min(end, frame.template column<Index>().block_containing_slot(slot).end)), ...);
        return end;
    };
    const auto start = std::chrono::steady_clock::now();
    rows.slots().compact(
        memory, block_end,
        [&](row_id_t source, row_id_t destination, std::uint32_t count, auto &&) {
            const auto account_move = [&](std::uint32_t offset) {
                const auto moved_source = static_cast<row_id_t>(source + offset);
                const auto moved_destination = static_cast<row_id_t>(destination + offset);
                const auto id = rows.id_at_slot(moved_source);
                ++sample.planned_moves;
                sample.plan_checksum = sample.plan_checksum * 1'099'511'628'211ULL + id +
                                       moved_source * 31ULL + moved_destination;
            };
            if (destination < source) {
                for (auto offset = std::uint32_t{}; offset != count; ++offset)
                    account_move(offset);
            } else {
                for (auto offset = count; offset != 0; --offset)
                    account_move(offset - 1);
            }
        });
    sample.plan_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void seed_planning_rows(NativeRows &rows, std::span<const row_id_t> ids, Deletion deletion, allocator memory) {
    const auto add_time = MIN_DT + EvaluationTime::duration{1};
    const auto erase_time = MIN_DT + EvaluationTime::duration{2};
    const auto initial_rows = row_count;
    for (auto row = std::size_t{}; row != initial_rows; ++row) {
        const auto id = rows.add(add_time, memory);
        if (id != ids[row])
            throw std::runtime_error{"planning frame produced different row IDs"};
    }
    for (auto row = std::size_t{}; row != initial_rows; ++row)
        if (should_delete(deletion, row))
            rows.erase(ids[row], erase_time, memory);
    for (auto row = initial_rows; row != ids.size(); ++row)
        if (rows.add(erase_time, memory) != ids[row])
            throw std::runtime_error{"planning frame refill produced different row IDs"};
}

template <template <template <typename, auto...> typename> typename Schema>
FrameView<Schema> frame_view(const NativeFrame<Schema> &frame) {
    using view_columns = typename FrameView<Schema>::columns_type;
    auto columns = view_columns{};
    const auto storage = frame.scalar_storage();
    auto source = bg::frame::details::frame_schema_reflection<Schema>::fields(storage.columns);
    auto destination = bg::frame::details::frame_schema_reflection<Schema>::fields(columns);
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        ((std::get<Index>(destination) = bg::scalar_storage_traits<
              typename bg::frame::details::frame_column_storage<std::remove_cvref_t<decltype(std::get<Index>(source))>>::
                  scalar_type>::read(std::get<Index>(source))),
         ...);
    }(std::make_index_sequence<NativeFrame<Schema>::field_count>{});
    return FrameView<Schema>{std::move(columns), frame.size()};
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
std::int64_t native_sum(const NativeFrame<Schema> &frame, std::size_t &batches,
                        std::index_sequence<Index...>) {
    auto sum = std::int64_t{};
    const auto visit = [&](const auto &batch) {
        ++batches;
        for (auto position = std::size_t{}; position != batch.cells.size; ++position)
            if (batch.cells.is_valid(position))
                sum += batch.cells.value_at(position);
    };
    (frame.template column<Index>().for_each_batch(frame.rows(), visit), ...);
    return sum;
}

std::int64_t arrow_sum(const std::shared_ptr<arrow::Table> &table, std::size_t &chunks) {
    auto sum = std::int64_t{};
    for (const auto &column : table->columns()) {
        chunks += static_cast<std::size_t>(column->num_chunks());
        auto result = arrow::compute::CallFunction("sum", {arrow::Datum{column}});
        if (!result.ok())
            throw std::runtime_error{result.status().ToString()};
        const auto &scalar = result->scalar_as<arrow::Int64Scalar>();
        if (!scalar.is_valid)
            throw std::runtime_error{"Arrow sum returned an invalid scalar"};
        sum += scalar.value;
    }
    return sum;
}

template <template <template <typename, auto...> typename> typename Schema>
std::int64_t borrowed_arrow_sum(const NativeFrame<Schema> &frame, std::size_t &chunks) {
    auto result = borrow_arrow(frame_view(frame));
    if (!result.ok())
        throw std::runtime_error{result.status().ToString()};
    return arrow_sum(std::move(result).ValueUnsafe(), chunks);
}

template <template <template <typename, auto...> typename> typename Schema>
std::int64_t dense_gather_sum(const NativeFrame<Schema> &frame, std::size_t &chunks) {
    auto result = copy_to_arrow(frame_view(frame));
    if (!result.ok())
        throw std::runtime_error{result.status().ToString()};
    return arrow_sum(std::move(result).ValueUnsafe(), chunks);
}

template <template <template <typename, auto...> typename> typename Schema>
std::int64_t id_sum(const NativeFrame<Schema> &frame, std::span<const row_id_t> ids) {
    auto sum = std::int64_t{};
    for (const auto id : ids)
        if (const auto value = frame.template cell<0>(id))
            sum += *value;
    return sum;
}

template <typename Function> double average_ms(Function &&function) {
    auto result = std::int64_t{};
    auto elapsed = std::chrono::steady_clock::duration{};
    for (auto repeat = std::size_t{}; repeat != timing_repeats; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
        result += std::invoke(function);
        elapsed += std::chrono::steady_clock::now() - start;
    }
    sink.value += result;
    return std::chrono::duration<double, std::milli>{elapsed}.count() / timing_repeats;
}

template <template <template <typename, auto...> typename> typename Schema>
PhaseSample measure_phase(const NativeFrame<Schema> &frame, std::span<const row_id_t> id_reads) {
    constexpr auto indices = std::make_index_sequence<NativeFrame<Schema>::field_count>{};
    auto sample = PhaseSample{.live_rows = frame.size()};
    auto native_batches = std::size_t{};
    const auto native_result = native_sum(frame, native_batches, indices);
    sample.native_batches = native_batches;

    auto borrowed_chunks = std::size_t{};
    const auto borrowed_result = borrowed_arrow_sum(frame, borrowed_chunks);
    auto copied_chunks = std::size_t{};
    const auto copied_result = dense_gather_sum(frame, copied_chunks);
    if (native_result != borrowed_result || native_result != copied_result)
        throw std::runtime_error{"native, borrowed Arrow, and dense gather sums disagree"};
    sample.sum = native_result;
    sample.borrowed_arrow_chunks = borrowed_chunks;
    sample.dense_gather_chunks = copied_chunks;

    for (auto index = std::size_t{}; index != consumer_counts.size(); ++index) {
        const auto consumers = consumer_counts[index];
        sample.native_ms[index] = average_ms([&] {
            auto result = std::int64_t{};
            for (auto consumer = std::size_t{}; consumer != consumers; ++consumer) {
                auto batches = std::size_t{};
                result += native_sum(frame, batches, indices);
            }
            return result;
        });
        sample.borrowed_arrow_sum_ms[index] = average_ms([&] {
            auto chunks = std::size_t{};
            auto result = std::int64_t{};
            for (auto consumer = std::size_t{}; consumer != consumers; ++consumer)
                result += borrowed_arrow_sum(frame, chunks);
            return result;
        });
        sample.dense_gather_sum_ms[index] = average_ms([&] {
            auto chunks = std::size_t{};
            auto result = std::int64_t{};
            for (auto consumer = std::size_t{}; consumer != consumers; ++consumer)
                result += dense_gather_sum(frame, chunks);
            return result;
        });
        sample.id_read_ms[index] = average_ms([&] {
            auto result = std::int64_t{};
            for (auto consumer = std::size_t{}; consumer != consumers; ++consumer)
                result += id_sum(frame, id_reads);
            return result;
        });
    }
    return sample;
}

template <typename Mapping> std::size_t physical_run_count(const Mapping &mapping) {
    auto result = std::size_t{};
    mapping.live_slots().for_each_run([&](row_id_t, row_id_t) { ++result; });
    return result;
}

ComputedPhaseSample measure_computed_phase(const NativeRows &source_rows, const NativeRowSlots &computed_rows,
                                           const NativeColumnData<i64> &left,
                                           const NativeColumnData<i64> &right,
                                           const ColumnStorage<i64> &left_storage,
                                           const ColumnStorage<i64> &right_storage,
                                           const FrameView<ComputedPairRows> &view) {
    const auto visible = RowSet::native(source_rows);
    const auto left_series = ColumnSeriesView<i64>{left_storage};
    const auto right_series = ColumnSeriesView<i64>{right_storage};
    auto sample = ComputedPhaseSample{.live_rows = visible.count(),
                                      .physical_runs = physical_run_count(computed_rows)};

    const auto measure_visible = [&](bool count_batches) {
        auto sum = std::int64_t{};
        left_series.for_each_batch([&](const auto &batch) {
            sample.visible_batches += count_batches;
            for (auto position = std::size_t{}; position != batch.cells.size; ++position)
                if (batch.cells.is_valid(position))
                    sum += batch.cells.value_at(position);
        });
        right_series.for_each_batch([&](const auto &batch) {
            sample.visible_batches += count_batches;
            for (auto position = std::size_t{}; position != batch.cells.size; ++position)
                if (batch.cells.is_valid(position))
                    sum += batch.cells.value_at(position);
        });
        return sum;
    };
    const auto measure_aligned = [&](bool count_batches) {
        auto sum = std::int64_t{};
        left_series.for_each_aligned_batch(right_series, visible, [&](const auto &batch) {
            sample.aligned_consumer_batches += count_batches;
            for (auto position = std::size_t{}; position != batch.ids.size(); ++position) {
                if (batch.left.is_valid(position))
                    sum += batch.left.value_at(position);
                if (batch.right.is_valid(position))
                    sum += batch.right.value_at(position);
            }
        });
        return sum;
    };

    auto expected_sum = std::int64_t{};
    source_rows.live_slots().for_each_run([&](row_id_t first, row_id_t end) {
        for (auto slot = first; slot != end; ++slot) {
            const auto id = source_rows.id_at_slot(slot);
            expected_sum += static_cast<std::int64_t>(id + 1) + static_cast<std::int64_t>(id * 3 + 2);
        }
    });

    const auto visible_sum = measure_visible(true);
    const auto aligned_sum = measure_aligned(true);
    auto borrowed = borrow_arrow(view);
    if (!borrowed.ok())
        throw std::runtime_error{borrowed.status().ToString()};
    const auto borrowed_sum = arrow_sum(borrowed.ValueUnsafe(), sample.borrowed_arrow_chunks);
    if (visible_sum != expected_sum || aligned_sum != expected_sum || borrowed_sum != expected_sum ||
        sample.visible_batches != sample.borrowed_arrow_chunks)
        throw std::runtime_error{"computed visible batches, aligned consumer, and Arrow export disagree"};
    sample.sum = visible_sum;

    sample.visible_ms = average_ms([&] { return measure_visible(false); });
    sample.aligned_consumer_ms = average_ms([&] { return measure_aligned(false); });
    sample.borrowed_arrow_sum_ms = average_ms([&] {
        auto result = borrow_arrow(view);
        if (!result.ok())
            throw std::runtime_error{result.status().ToString()};
        auto chunks = std::size_t{};
        return arrow_sum(result.ValueUnsafe(), chunks);
    });
    return sample;
}

ComputedCaseSample run_computed_sample(bool reverse_mapping) {
    auto resource = CountingResource{};
    const auto memory = allocator{resource};
    auto source = NativeFrame<NarrowRows>{};
    auto release_source = util::scope_exit{[&]() noexcept { source.core().release(memory); }};
    auto ids = std::vector<row_id_t>{};
    ids.reserve(computed_row_count);
    const auto add_time = MIN_DT + EvaluationTime::duration{1};
    const auto remove_time = MIN_DT + EvaluationTime::duration{2};
    for (auto row = std::size_t{}; row != computed_row_count; ++row)
        ids.push_back(source.add(NarrowRows<Cell>{.value = Cell<i64>{static_cast<i64>(row + 1)}}, add_time,
                                 memory));

    auto computed_rows = NativeRowSlots{};
    auto release_computed_rows = util::scope_exit{[&]() noexcept { computed_rows.release(memory); }};
    auto computed_left = NativeColumnData<i64>{};
    auto release_computed_left = util::scope_exit{[&]() noexcept { computed_left.release(memory); }};
    auto computed_right = NativeColumnData<i64>{};
    auto release_computed_right = util::scope_exit{[&]() noexcept { computed_right.release(memory); }};
    for (auto position = std::size_t{}; position != ids.size(); ++position) {
        const auto row = reverse_mapping ? ids.size() - position - 1 : position;
        const auto id = ids[row];
        computed_rows.insert(id, memory);
        computed_left.add(computed_rows, id, static_cast<i64>(row + 1), memory);
        computed_right.add(computed_rows, id, static_cast<i64>(row * 3 + 2), memory);
    }

    for (auto row = std::size_t{}; row != ids.size(); ++row) {
        if (row % 20 != 0)
            continue;
        const auto id = ids[row];
        const auto slot = computed_rows.slot_of(id);
        computed_left.erase_slot(slot, id, memory);
        computed_right.erase_slot(slot, id, memory);
        computed_rows.erase(id);
        if (!source.erase(id, remove_time, memory))
            throw std::runtime_error{"computed benchmark source row could not be removed"};
    }

    const auto visible = RowSet::native(source.rows());
    auto left_storage = ColumnStorage<i64>{computed_left, computed_rows, visible, source.rows().added(),
                                           computed_left.modified(), source.rows().removed(), remove_time};
    auto right_storage = ColumnStorage<i64>{computed_right, computed_rows, visible, source.rows().added(),
                                            computed_right.modified(), source.rows().removed(), remove_time};
    const auto view = FrameView<ComputedPairRows>{
        typename FrameView<ComputedPairRows>::columns_type{
            bg::scalar_storage_traits<Column<i64>>::read(left_storage),
            bg::scalar_storage_traits<Column<i64>>::read(right_storage)},
        source.size()};
    const auto source_size_before = source.size();

    auto result = ComputedCaseSample{};
    result.before = measure_computed_phase(source.rows(), computed_rows, computed_left, computed_right,
                                           left_storage, right_storage, view);

    auto slots_before = std::vector<row_id_t>(ids.size(), no_row);
    for (const auto id : ids)
        if (computed_rows.contains(id))
            slots_before[id] = computed_rows.slot_of(id);
    auto source_slots_before = std::vector<row_id_t>(ids.size(), no_row);
    for (const auto id : ids)
        if (source.rows().contains(id))
            source_slots_before[id] = source.rows().slot_of(id);
    const auto compact_start = std::chrono::steady_clock::now();
    computed_rows.compact(
        memory,
        [&](row_id_t slot) {
            return std::min(computed_left.block_containing_slot(slot).end,
                            computed_right.block_containing_slot(slot).end);
        },
        [&](row_id_t source_slot, row_id_t destination, std::uint32_t count, auto &&commit_mapping) {
            computed_left.prepare_relocation(source_slot, destination, count, memory);
            computed_right.prepare_relocation(source_slot, destination, count, memory);
            computed_left.relocate(source_slot, destination, count);
            computed_right.relocate(source_slot, destination, count);
            std::invoke(commit_mapping);
        });
    result.compact_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - compact_start)
                            .count();
    for (const auto id : ids) {
        if (!computed_rows.contains(id))
            continue;
        result.moved_rows += slots_before[id] != computed_rows.slot_of(id);
        if (source.rows().slot_of(id) != source_slots_before[id])
            throw std::runtime_error{"computed-column compaction changed the shared source mapping"};
    }

    result.after = measure_computed_phase(source.rows(), computed_rows, computed_left, computed_right,
                                          left_storage, right_storage, view);
    if (result.before.sum != result.after.sum || source.size() != source_size_before)
        throw std::runtime_error{"computed-column compaction changed the output values or source rows"};
    return result;
}

template <template <template <typename, auto...> typename> typename Schema>
CaseSample run_sample(Deletion deletion) {
    auto resource = CountingResource{};
    const auto memory = allocator{resource};
    auto frame = NativeFrame<Schema>{};
    auto release_frame = util::scope_exit{[&]() noexcept { frame.core().release(memory); }};
    auto ids = std::vector<row_id_t>{};
    seed(frame, memory, ids);
    erase_rows(frame, ids, deletion, memory);
    if (deletion == Deletion::delete_then_add_more)
        refill_rows(frame, memory, ids, deleted_row_count(deletion) * 2);

    auto live_ids = std::vector<row_id_t>{};
    auto id_reads = std::vector<row_id_t>{};
    for (const auto slot : frame.rows().live_slots()) {
        const auto id = frame.rows().id_at_slot(slot);
        live_ids.push_back(id);
        if (live_ids.size() % 32 == 0)
            id_reads.push_back(id);
    }

    auto result = CaseSample{};
    result.before = measure_phase(frame, id_reads);
    auto slots_before = std::vector<row_id_t>{};
    slots_before.reserve(live_ids.size());
    for (const auto id : live_ids)
        slots_before.push_back(frame.rows().slots().slot_of(id));

    auto planning_rows = NativeRows{};
    auto release_planning_rows = util::scope_exit{[&]() noexcept { planning_rows.release(memory); }};
    seed_planning_rows(planning_rows, ids, deletion, memory);
    for (const auto id : live_ids)
        if (planning_rows.slot_of(id) != frame.rows().slots().slot_of(id))
            throw std::runtime_error{"planning frame row mapping differs from the measured frame"};
    const auto planning_allocations = resource.allocations;
    const auto planning_bytes = resource.bytes_allocated;
    plan_compaction(frame, planning_rows, memory, result.compaction,
                    std::make_index_sequence<NativeFrame<Schema>::field_count>{});
    result.compaction.plan_allocations = resource.allocations - planning_allocations;
    result.compaction.plan_bytes_allocated = resource.bytes_allocated - planning_bytes;

    const auto allocation_count = resource.allocations;
    const auto allocated_bytes = resource.bytes_allocated;
    const auto compact_start = std::chrono::steady_clock::now();
    frame.core().compact(memory);
    result.compaction.elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compact_start).count();
    result.compaction.allocations = resource.allocations - allocation_count;
    result.compaction.bytes_allocated = resource.bytes_allocated - allocated_bytes;
    for (auto index = std::size_t{}; index != live_ids.size(); ++index)
        result.compaction.moved_rows += slots_before[index] != frame.rows().slots().slot_of(live_ids[index]);
    if (result.compaction.planned_moves != result.compaction.moved_rows)
        throw std::runtime_error{"planning-only and full compaction move counts disagree"};
    result.compaction.value_bytes_moved = result.compaction.moved_rows * NativeFrame<Schema>::field_count * sizeof(i64);
    result.after = measure_phase(frame, id_reads);
    if (result.before.sum != result.after.sum)
        throw std::runtime_error{"compaction changed the frame values"};
    auto position = std::size_t{};
    for (const auto slot : frame.rows().live_slots()) {
        if (position == live_ids.size() || frame.rows().id_at_slot(slot) != live_ids[position])
            throw std::runtime_error{"compaction changed row order"};
        ++position;
    }
    if (position != live_ids.size())
        throw std::runtime_error{"compaction changed the live row count"};
    return result;
}

void print_phase_row(std::string_view schema, std::string_view deletion, std::string_view phase,
                     const std::array<CaseSample, sample_count> &samples, std::size_t consumer_index) {
    const auto &first = phase == "before" ? samples.front().before : samples.front().after;
    auto native = std::array<double, sample_count>{};
    auto borrowed = std::array<double, sample_count>{};
    auto gather = std::array<double, sample_count>{};
    auto id_read = std::array<double, sample_count>{};
    for (auto sample = std::size_t{}; sample != sample_count; ++sample) {
        const auto &value = phase == "before" ? samples[sample].before : samples[sample].after;
        native[sample] = value.native_ms[consumer_index];
        borrowed[sample] = value.borrowed_arrow_sum_ms[consumer_index];
        gather[sample] = value.dense_gather_sum_ms[consumer_index];
        id_read[sample] = value.id_read_ms[consumer_index];
    }
    const auto median_time = [](auto values) {
        std::ranges::sort(values);
        return values[sample_count / 2];
    };
    const auto median_count = [&](auto select) {
        auto values = std::array<std::size_t, sample_count>{};
        for (auto sample = std::size_t{}; sample != sample_count; ++sample)
            values[sample] = select(samples[sample].compaction);
        std::ranges::sort(values);
        return values[sample_count / 2];
    };
    std::cout << "frame,native," << schema << ',' << deletion << ',' << phase << ','
              << consumer_counts[consumer_index] << ','
              << first.live_rows << ',' << first.native_batches << ',' << first.borrowed_arrow_chunks << ','
              << first.dense_gather_chunks << ',' << median_time(native) << ',' << median_time(borrowed) << ','
              << median_time(gather) << ','
              << median_time(id_read) << ',';
    const auto compact = std::array<double, sample_count>{samples[0].compaction.elapsed_ms,
                                                          samples[1].compaction.elapsed_ms,
                                                          samples[2].compaction.elapsed_ms,
                                                          samples[3].compaction.elapsed_ms,
                                                          samples[4].compaction.elapsed_ms};
    auto plan = std::array<double, sample_count>{samples[0].compaction.plan_ms, samples[1].compaction.plan_ms,
                                                 samples[2].compaction.plan_ms, samples[3].compaction.plan_ms,
                                                 samples[4].compaction.plan_ms};
    std::cout << median_time(compact) << ',' << median_time(plan) << ','
              << median_count([](const auto &value) { return value.plan_allocations; }) << ','
              << median_count([](const auto &value) { return value.plan_bytes_allocated; }) << ','
              << median_count([](const auto &value) { return value.planned_moves; }) << ','
              << median_count([](const auto &value) { return value.plan_checksum; }) << ','
              << median_count([](const auto &value) { return value.allocations; }) << ','
              << median_count([](const auto &value) { return value.bytes_allocated; }) << ','
              << median_count([](const auto &value) { return value.moved_rows; }) << ','
              << median_count([](const auto &value) { return value.value_bytes_moved; }) << ",,,\n";
}

void print_computed_phase_row(std::string_view layout, std::string_view phase,
                              const std::array<ComputedCaseSample, sample_count> &samples) {
    const auto before = phase == "before";
    const auto &first = before ? samples.front().before : samples.front().after;
    auto visible_batches = std::array<std::size_t, sample_count>{};
    auto aligned_batches = std::array<std::size_t, sample_count>{};
    auto arrow_chunks = std::array<std::size_t, sample_count>{};
    auto physical_runs = std::array<std::size_t, sample_count>{};
    auto visible_ms = std::array<double, sample_count>{};
    auto aligned_ms = std::array<double, sample_count>{};
    auto arrow_ms = std::array<double, sample_count>{};
    auto compact_ms = std::array<double, sample_count>{};
    auto moved_rows = std::array<std::size_t, sample_count>{};
    for (auto sample = std::size_t{}; sample != sample_count; ++sample) {
        const auto &value = before ? samples[sample].before : samples[sample].after;
        physical_runs[sample] = value.physical_runs;
        visible_batches[sample] = value.visible_batches;
        aligned_batches[sample] = value.aligned_consumer_batches;
        arrow_chunks[sample] = value.borrowed_arrow_chunks;
        visible_ms[sample] = value.visible_ms;
        aligned_ms[sample] = value.aligned_consumer_ms;
        arrow_ms[sample] = value.borrowed_arrow_sum_ms;
        compact_ms[sample] = samples[sample].compact_ms;
        moved_rows[sample] = samples[sample].moved_rows;
    }
    const auto median_time = [](auto values) {
        std::ranges::sort(values);
        return values[sample_count / 2];
    };
    const auto median_count = [](auto values) {
        std::ranges::sort(values);
        return values[sample_count / 2];
    };
    std::cout << "computed," << layout << ",computed_pair,scattered_5_percent," << phase << ",1,"
              << first.live_rows << ',' << median_count(visible_batches) << ',' << median_count(arrow_chunks) << ",,"
              << median_time(visible_ms) << ',' << median_time(arrow_ms) << ",,,";
    if (!before)
        std::cout << median_time(compact_ms);
    std::cout << ",,,,,,,,";
    if (before) {
        std::cout << ",,";
    } else {
        std::cout << median_count(moved_rows) << ',' << median_count(moved_rows) * 2 * sizeof(i64) << ',';
    }
    std::cout << median_count(physical_runs) << ',' << median_count(aligned_batches) << ','
              << median_time(aligned_ms) << '\n';
}

template <template <template <typename, auto...> typename> typename Schema>
void run_schema(std::string_view schema) {
    for (const auto &[deletion, deletion_name] : deletions) {
        auto samples = std::array<CaseSample, sample_count>{};
        for (auto index = std::size_t{}; index != sample_count; ++index)
            samples[index] = run_sample<Schema>(deletion);
        for (auto consumer_index = std::size_t{}; consumer_index != consumer_counts.size(); ++consumer_index) {
            print_phase_row(schema, deletion_name, "before", samples, consumer_index);
            print_phase_row(schema, deletion_name, "after", samples, consumer_index);
        }
    }
}

void run_computed_mapping(std::string_view layout, bool reverse_mapping) {
    auto samples = std::array<ComputedCaseSample, sample_count>{};
    for (auto index = std::size_t{}; index != sample_count; ++index)
        samples[index] = run_computed_sample(reverse_mapping);
    print_computed_phase_row(layout, "before", samples);
    print_computed_phase_row(layout, "after", samples);
}

} // namespace

int main() {
    try {
        const auto status = arrow::compute::Initialize();
        if (!status.ok())
            throw std::runtime_error{status.ToString()};
        std::cout << "record,layout,schema,deletion,phase,consumers,live_rows,native_batches,borrowed_arrow_chunks,"
                     "dense_gather_chunks,native_ms,borrowed_arrow_sum_destroy_ms,dense_gather_sum_destroy_ms,"
                     "id_read_ms,compact_ms,plan_only_ms,"
                     "plan_allocations,plan_allocated_bytes,planned_moves,plan_checksum,compact_allocations,"
                     "compact_allocated_bytes,moved_rows,value_bytes_moved,physical_runs,"
                     "aligned_consumer_batches,aligned_consumer_ms\n";
        run_schema<NarrowRows>("narrow_1x_i64");
        run_schema<WideRows>("wide_8x_i64");
        run_computed_mapping("aligned", false);
        run_computed_mapping("permuted", true);
        std::cerr << "benchmark checksum: " << sink.value << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "bg_frame_compaction_benchmark: " << error.what() << '\n';
        return 1;
    }
}
