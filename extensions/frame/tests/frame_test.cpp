#include <bg/frame/frame.hpp>
#include <bg/bg.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/graph/borrowed_input.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/type/type_registry.hpp>

#include <array>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;

template <template <typename, auto...> typename Field> struct Quotes {
    Field<bg::i64> size;
    Field<double> bid;
    Field<double> ask;
};

template <template <typename, auto...> typename Field> struct ReorderedQuotes {
    Field<double> ask;
    Field<i64> size;
    Field<double> bid;
};

template <template <typename, auto...> typename Field> struct GenericQuote {
    Field<bg::SCALAR> bid;
};
template <template <typename, auto...> typename Field> struct MixedGenericQuote {
    Field<i64> size;
    Field<SCALAR> bid;
    Field<double> ask;
};
template <template <typename, auto...> typename Field> struct WrongMixedGenericQuote {
    Field<double> size;
    Field<SCALAR> bid;
    Field<double> ask;
};
template <template <typename, auto...> typename Field> struct RepeatedGenericQuote {
    Field<SCALAR> size;
    Field<SCALAR> bid;
    Field<SCALAR> ask;
};
using I64Cell = ScalarVariable<"I64_CELL", i64>;
template <template <typename, auto...> typename Field> struct RepeatedConstrainedGenericQuote {
    Field<SCALAR> size;
    Field<SCALAR> bid;
    Field<I64Cell> ask;
};
template <template <typename, auto...> typename Field> struct ConstrainedGenericQuote {
    Field<i64> size;
    Field<I64Cell> bid;
    Field<double> ask;
};
template <template <typename, auto...> typename Field> struct InheritedGenericQuoteBase {
    Field<i64> size;
    Field<double> bid;
};
template <template <typename, auto...> typename Field>
struct InheritedGenericQuote : schema_extends<InheritedGenericQuoteBase, Field> {
    Field<SCALAR> bid;
    Field<double> ask;
};
template <template <typename, auto...> typename Field> struct InheritedFrameBase {
    Field<bg::i64> size;
    Field<double> bid;
};

template <template <typename, auto...> typename Field>
struct InheritedFrame : bg::schema_extends<InheritedFrameBase, Field> {
    Field<double> bid;
    Field<double> ask;
};

template <typename T> concept complete_type = requires { sizeof(T); };
template <typename Output, typename Argument>
concept accepts_frame_add = requires(const Output &output, const Argument &argument) { output.add(argument); };
template <typename Output, typename Argument>
concept accepts_frame_set = requires(const Output &output, const Argument &argument) { output.set(row_id_t{}, argument); };

static_assert(complete_type<Out<TSF<Quotes>>>);
static_assert(complete_type<Out<TSF<Quotes>, bg::frame::view>>);
static_assert(complete_type<Out<TSC<bg::i64>>>);
static_assert(complete_type<Out<TSC<bg::i64>, bg::frame::view>>);
static_assert(!complete_type<Out<TS<bg::i64>, bg::frame::view>>);
static_assert(accepts_frame_add<Out<TSF<Quotes>>, RowPatch<Quotes>>);
static_assert(!accepts_frame_add<Out<TSF<SCHEMA>>, RowPatch<SCHEMA>>);
static_assert(accepts_frame_add<Out<TSF<GenericQuote>>, Row<GenericQuote>>);
static_assert(accepts_frame_add<Out<TSF<GenericQuote>>, RowPatch<GenericQuote>>);
static_assert(accepts_frame_set<Out<TSF<GenericQuote>>, RowPatch<GenericQuote>>);
static_assert(accepts_frame_add<Out<TSF<SCHEMA>>, Row<MixedGenericQuote>>);
static_assert(accepts_frame_add<Out<TSF<SCHEMA>>, RowPatch<MixedGenericQuote>>);
static_assert(accepts_frame_set<Out<TSF<SCHEMA>>, RowPatch<MixedGenericQuote>>);

template <template <typename, auto...> typename Field> struct EmptySchema {};
template <template <typename, auto...> typename Field> struct BidOnly { Field<double> price; };
template <template <typename, auto...> typename Field> struct UnsupportedFrameCell { Field<bool> value; };

static_assert(!scalar_declaration_traits<Column<bool>>::supports_time_series);
static_assert(!time_series_type_traits<TSC<bool>>::supported);
static_assert(!time_series_type_traits<TSF<UnsupportedFrameCell>>::supported);

class FailingResource final : public std::pmr::memory_resource {
  public:
    std::size_t attempts{};
    std::size_t allocations{};
    std::size_t deallocations{};
    std::size_t fail_at{};

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (++attempts == fail_at)
            throw std::bad_alloc{};
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}

template <typename Function>
void with_native_storage(const SchemaType &schema, allocator memory, Function &&function) {
    const auto &layout = bg::frame::details::native_frame_layout(schema);
    auto *allocation = ::operator new(layout.size, std::align_val_t{layout.alignment});
    auto *storage = std::construct_at(static_cast<NativeFrameStorage *>(allocation));
    storage->initialize(layout);
    auto release = util::scope_exit{[&]() noexcept {
        storage->release(memory);
        std::destroy_at(storage);
        ::operator delete(allocation, std::align_val_t{layout.alignment});
    }};
    std::invoke(std::forward<Function>(function), *storage);
}

template <template <template <typename, auto...> typename> typename Schema, typename Function>
void with_runtime_view_storage(allocator memory, Function &&function) {
    const auto &type = frame_time_series_type<Schema>(TimeSeriesGuarantee::ordinary);
    const auto &runtime = static_cast<const FrameSeriesRuntimeType &>(
        frame_view_time_series_runtime_type<Schema>(type));
    auto *allocation = ::operator new(runtime.value_size(), std::align_val_t{runtime.value_alignment()});
    runtime.construct_initial_value(allocation);
    auto release = util::scope_exit{[&]() noexcept {
        runtime.destroy_value(allocation, memory);
        ::operator delete(allocation, std::align_val_t{runtime.value_alignment()});
    }};
    std::invoke(std::forward<Function>(function), *static_cast<RuntimeFrameViewOutputStorage *>(allocation),
                runtime);
}

template <typename Predicate>
auto runtime_predicate(Predicate &predicate) {
    return std::pair{
        static_cast<void *>(std::addressof(predicate)),
        static_cast<RuntimeFrameViewOutputStorage::predicate_function>([](void *context, row_id_t id) {
            return std::invoke(*static_cast<Predicate *>(context), id);
        })};
}

struct FrameReadBindings {
    FrameReadStorage source;
    RowSet visible_rows{};
    bool override_visible_rows{};
    std::optional<std::size_t> overridden_index;
    FrameColumnReadBinding overridden_binding;

    static FrameColumnReadBinding column(const void *context, std::size_t index) {
        const auto &bindings = *static_cast<const FrameReadBindings *>(context);
        if (bindings.overridden_index == index)
            return bindings.overridden_binding;
        auto result = bindings.source.column(index);
        if (bindings.override_visible_rows) {
            result.visible_rows = bindings.visible_rows;
            result.coverage = bindings.visible_rows;
        }
        return result;
    }

    void apply(FrameReadStorage &input) {
        source = input;
        input.columns_context = this;
        input.column_binding = &column;
    }
};

template <template <template <typename, auto...> typename> typename Schema>
FrameReadStorage native_frame_input(const NativeFrame<Schema> &source) {
    auto input = frame::details::frame_read_storage(source.core(), std::addressof(source.core()), nullptr);
    input.valid = true;
    input.modified = source.change_time() != MIN_DT;
    input.last_updated = source.change_time();
    return input;
}

struct RowTraversalCounter {
    const NativeRows *rows{};
    mutable std::size_t traversals{};

    RowSet row_set() const noexcept {
        return RowSet{this,
                      rows,
                      rows->identity(),
                      [](const void *context) noexcept {
                          return static_cast<const RowTraversalCounter *>(context)->rows->size();
                      },
                      [](const void *context, row_id_t id) noexcept {
                          return static_cast<const RowTraversalCounter *>(context)->rows->contains(id);
                      },
                      [](const void *context, std::size_t position) {
                          return static_cast<const RowTraversalCounter *>(context)->rows->id_at(position);
                      },
                      [](const void *context, void *target, RowSet::visit_function visit) {
                          const auto &counter = *static_cast<const RowTraversalCounter *>(context);
                          ++counter.traversals;
                          for (auto slot : counter.rows->live_slots())
                              visit(target, counter.rows->id_at_slot(slot));
                      }};
    }
};

void test_runtime_native_storage_mixed_and_zero_column_schemas() {
    using namespace bg;
    using namespace bg::frame;

    const auto mixed_fields = std::array{
        SchemaField{util::symbol{"number"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()},
    };
    const auto &mixed_schema = anonymous_schema(mixed_fields);
    auto resource = FailingResource{};
    const auto memory = allocator{resource};
    with_native_storage(mixed_schema, memory, [&](NativeFrameStorage &storage) {
        const auto number = std::optional<i64>{42};
        const auto price = std::optional<double>{};
        const auto values = std::array<const void *, 2>{std::addressof(number), std::addressof(price)};
        const auto time = MIN_DT + EvaluationTime::duration{1};
        const auto id = storage.add(values, time, memory);
        const auto number_storage = static_cast<const ColumnStorage<i64> *>(storage.column_storage(0));
        const auto price_storage = static_cast<const ColumnStorage<double> *>(storage.column_storage(1));
        require(number_storage->native->cell(storage.rows(), id) == number &&
                    price_storage->native->cell(storage.rows(), id) == price,
                "runtime mixed schema lost its typed values or nulls");
        require(storage.changed_columns().contains(0) && storage.changed_columns().contains(1),
                "runtime mixed schema did not publish both columns");
    });

    const auto empty_fields = std::array<SchemaField, 0>{};
    const auto &empty_schema = anonymous_schema(empty_fields);
    with_native_storage(empty_schema, memory, [&](NativeFrameStorage &storage) {
        const auto id = storage.add({}, MIN_DT + EvaluationTime::duration{1}, memory);
        require(storage.size() == 1 && storage.contains(id) && storage.changed().contains(id),
                "zero-column runtime frame lost row membership");
        require(storage.changed_columns().empty(), "zero-column runtime frame published a column");
        require(storage.erase(id, MIN_DT + EvaluationTime::duration{2}, memory) && storage.size() == 0 &&
                    storage.removed().contains(id),
                "zero-column runtime frame could not remove row membership");
    });
}

void test_native_storage_validates_row_and_patch_shape_before_mutation() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = FailingResource{};
    const auto memory = allocator{resource};
    with_native_storage(frame_schema_type<BidOnly>(), memory, [&](NativeFrameStorage &storage) {
        const auto first_time = MIN_DT + EvaluationTime::duration{1};
        auto rejected_row = false;
        try {
            storage.add({}, first_time, memory);
        } catch (const std::invalid_argument &) {
            rejected_row = true;
        }
        require(rejected_row && storage.size() == 0 && storage.change_time() == MIN_DT &&
                    storage.changed_columns().empty(),
                "invalid raw row shape mutated native frame state");

        const auto value = std::optional<double>{9.5};
        const auto values = std::array<const void *, 1>{std::addressof(value)};
        const auto id = storage.add(values, first_time, memory);
        auto rejected_patch = false;
        try {
            storage.set(id, {}, first_time + EvaluationTime::duration{1}, memory);
        } catch (const std::invalid_argument &) {
            rejected_patch = true;
        }
        const auto column = static_cast<const ColumnStorage<double> *>(storage.column_storage(0));
        require(rejected_patch && storage.change_time() == first_time && storage.changed().contains(id) &&
                    column->native->cell(storage.rows(), id) == value,
                "invalid raw patch shape changed frame deltas or values");

        const auto replacement = std::optional<double>{12.5};
        const auto patch = std::array<bg::frame::details::NativeFrameCellPatch, 1>{
            bg::frame::details::NativeFrameCellPatch{.changed = true, .value = std::addressof(replacement)}};
        const auto update_time = first_time + EvaluationTime::duration{1};
        require(storage.set(id, patch, update_time, memory) && storage.modified(0)->contains(id),
                "valid raw patch did not publish its row change");
        auto rejected_missing_row = false;
        try {
            static_cast<void>(storage.set(no_row, patch, update_time + EvaluationTime::duration{1}, memory));
        } catch (const std::out_of_range &) {
            rejected_missing_row = true;
        }
        require(rejected_missing_row && storage.change_time() == update_time && storage.changed().contains(id) &&
                    storage.modified(0)->contains(id) && storage.changed_columns().contains(0) &&
                    column->native->cell(storage.rows(), id) == replacement,
                "missing raw patch row changed the previous cycle's deltas or values");
    });
}

void test_native_storage_changed_columns_beyond_inline_capacity() {
    using namespace bg;
    using namespace bg::frame;

    auto names = std::vector<std::string>{};
    auto fields = std::vector<SchemaField>{};
    names.reserve(57);
    fields.reserve(57);
    for (auto index = std::size_t{}; index != 57; ++index) {
        names.push_back("column_" + std::to_string(index));
        const auto *type = index % 2 == 0 ? &cpp_scalar_type<i64>() : &cpp_scalar_type<double>();
        fields.push_back({util::symbol{names.back()}, type});
    }
    const auto &schema = anonymous_schema(fields);
    auto resource = FailingResource{};
    const auto memory = allocator{resource};
    with_native_storage(schema, memory, [&](NativeFrameStorage &storage) {
        auto integers = std::vector<std::optional<i64>>(29);
        auto doubles = std::vector<std::optional<double>>(28);
        auto values = std::vector<const void *>{};
        values.reserve(57);
        for (auto index = std::size_t{}; index != 57; ++index) {
            if (index % 2 == 0) {
                integers[index / 2] = static_cast<i64>(index);
                values.push_back(std::addressof(integers[index / 2]));
            } else {
                doubles[index / 2] = static_cast<double>(index) + 0.5;
                values.push_back(std::addressof(doubles[index / 2]));
            }
        }

        resource.fail_at = resource.attempts + 1;
        auto allocation_failed = false;
        try {
            storage.add(values, MIN_DT + EvaluationTime::duration{1}, memory);
        } catch (const std::bad_alloc &) {
            allocation_failed = true;
        }
        require(allocation_failed && storage.size() == 0 && storage.change_time() == MIN_DT &&
                    storage.changed_columns().empty(),
                "changed-column allocation failure published a partial frame row");

        resource.fail_at = 0;
        const auto time = MIN_DT + EvaluationTime::duration{1};
        const auto id = storage.add(values, time, memory);
        require(storage.size() == 1 && storage.changed_columns().contains(0) &&
                    storage.changed_columns().contains(55) && storage.changed_columns().contains(56),
                "changed-column index did not publish columns beyond inline capacity");

        const auto updated = std::optional<i64>{5600};
        auto patches = std::vector<bg::frame::details::NativeFrameCellPatch>(57);
        patches[56] = {.changed = true, .value = std::addressof(updated)};
        const auto update_time = time + EvaluationTime::duration{1};
        require(storage.set(id, patches, update_time, memory) && storage.modified(56)->contains(id) &&
                    storage.changed_columns().contains(56),
                "changed-column index did not retain a high-column modification");

        const auto &series_type = frame_series_type(schema, TimeSeriesGuarantee::ordinary);
        const auto &runtime = frame_time_series_runtime_type<SCHEMA>(series_type);
        const auto &field_layout = storage.layout().fields[56];
        const auto &item_layout = runtime.item_layout(56);
        const auto *frame_bytes = reinterpret_cast<const std::byte *>(std::addressof(storage));
        const auto child = runtime.item(std::addressof(storage), 56);
        require(runtime.item_count(std::addressof(storage)) == 57 &&
                    item_layout.last_updated_offset == field_layout.child_state_offset &&
                    item_layout.value_offset == field_layout.column_storage_offset &&
                    std::addressof(child.last_updated) ==
                        reinterpret_cast<const TimeSeriesState *>(frame_bytes + field_layout.child_state_offset) &&
                    child.value == storage.column_storage(56) && child.last_updated.time() == update_time,
                "high-column runtime item does not address its native child state and value");

        const auto high_column = static_cast<const ColumnStorage<i64> *>(child.value);
        require(high_column->native->cell(storage.rows(), id) == updated &&
                    runtime.item_updated(TimeSeriesState{update_time}, std::addressof(storage), 56) &&
                    !runtime.item_updated(TimeSeriesState{update_time}, std::addressof(storage), 55),
                "high-column runtime item update does not match the stored cell");

        auto updated_items = std::vector<item_id_t>{};
        runtime.for_each_updated_item(
            TimeSeriesState{update_time}, std::addressof(storage), std::addressof(updated_items),
            [](void *context, item_id_t item) { static_cast<std::vector<item_id_t> *>(context)->push_back(item); });
        require(updated_items == std::vector<item_id_t>{56},
                "high-column updated-item iteration disagrees with the changed-column index");

        auto modified_items = std::vector<TimeSeriesRuntimeType::ModifiedItem>{};
        runtime.for_each_modified_item(
            TimeSeriesState{time}, std::addressof(storage), update_time, std::addressof(modified_items),
            [](void *context, TimeSeriesRuntimeType::ModifiedItem item) {
                static_cast<std::vector<TimeSeriesRuntimeType::ModifiedItem> *>(context)->push_back(item);
            });
        require(modified_items.size() == 1 && modified_items[0].id == 56 &&
                    modified_items[0].runtime_type == std::addressof(child.runtime_type) &&
                    modified_items[0].last_updated == std::addressof(child.last_updated) &&
                    modified_items[0].value == child.value,
                "high-column modified-item iteration disagrees with the runtime child view");
    });
    require(resource.allocations == resource.deallocations,
            "runtime schema native frame leaked allocations after wide-column tests");
}

void test_shared_rows_and_column_changes() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto frame = NativeFrame<Quotes>{};
    auto release = util::scope_exit{[&]() noexcept { frame.release(memory); }};
    const auto &layout = bg::frame::details::native_frame_layout<Quotes>();
    const auto &runtime = frame_time_series_runtime_type<Quotes>(
        frame_time_series_type<Quotes>(TimeSeriesGuarantee::ordinary));
    const auto &field_layout = layout.fields[1];
    const auto &item_layout = runtime.item_layout(1);
    require(item_layout.last_updated_offset == field_layout.child_state_offset &&
                item_layout.value_offset == field_layout.column_storage_offset,
            "TSF child offsets differ from the native field layout");
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto id = frame.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                           .ask = std::nullopt},
                              first_time, memory);

    const auto child = runtime.item(&frame.core(), 1);
    const auto *frame_bytes = reinterpret_cast<const std::byte *>(std::addressof(frame.core()));
    require(std::addressof(child.last_updated) ==
                reinterpret_cast<const TimeSeriesState *>(frame_bytes + field_layout.child_state_offset) &&
                child.value == frame.core().column_storage(1),
            "TSF runtime child does not address the native field storage");

    require(frame.size() == 1 && frame.contains(id), "frame did not add one row");
    require(frame.cell<0>(id) == 5 && frame.cell<1>(id) == 10.5 && !frame.cell<2>(id),
            "frame columns do not share the inserted row");
    auto changed = std::vector<row_id_t>{};
    frame.for_each_changed([&](row_id_t value) { changed.push_back(value); });
    require(changed == std::vector<row_id_t>{id}, "frame-wide changed iteration duplicated membership changes");

    auto bid_patch = Quotes<PatchCell>{};
    bid_patch.bid = 11.25;
    const auto second_time = first_time + EvaluationTime::duration{1};
    frame.set(id, bid_patch, second_time, memory);
    require(frame.modified<1>().contains(id) && !frame.modified<2>().contains(id),
            "frame did not record the changed column");

    const auto third_time = second_time + EvaluationTime::duration{1};
    auto ask_patch = Quotes<PatchCell>{};
    ask_patch.ask = std::nullopt;
    frame.set(id, ask_patch, third_time, memory);
    require(frame.modified<1>().empty() && frame.modified<2>().contains(id),
            "column modification state survived another column's later epoch");
    require(!frame.cell<2>(id), "explicit null update did not clear the cell");

    const auto second = frame.add(Quotes<Cell>{.size = Cell<i64>{6}, .bid = Cell<double>{std::nullopt},
                                                .ask = Cell<double>{12.0}},
                                  third_time, memory);
    changed.clear();
    frame.for_each_changed([&](row_id_t value) { changed.push_back(value); });
    require(changed == std::vector<row_id_t>{id, second}, "changed iteration did not emit the union once");
    require(frame.erase(id, third_time, memory), "frame row erase failed");
    require(!frame.contains(id), "erased frame row is still live");
    auto removed = std::vector<row_id_t>{};
    frame.for_each_removed([&](row_id_t value) { removed.push_back(value); });
    require(removed == std::vector<row_id_t>{id}, "frame removal delta is incorrect");
}

void test_failed_multicolumn_insert_cleanup() {
    using namespace bg;
    using namespace bg::frame;

    const auto time = MIN_DT + EvaluationTime::duration{1};
    auto failures = std::size_t{};
    for (auto failure = std::size_t{1};; ++failure) {
        FailingResource resource;
        auto succeeded = false;
        {
            const auto memory = allocator{resource};
            auto frame = NativeFrame<Quotes>{};
            auto release = util::scope_exit{[&]() noexcept { frame.release(memory); }};
            resource.fail_at = resource.attempts + failure;
            try {
                frame.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                       .ask = Cell<double>{12.5}},
                          time, memory);
                succeeded = true;
            } catch (const std::bad_alloc &) {
                ++failures;
                require(frame.size() == 0 && frame.rows().added().empty() && frame.changed_columns().empty(),
                        "failed multi-column insert left a partial row or publication");
            }
        }
        require(resource.allocations == resource.deallocations, "failed multi-column insert leaked storage");
        if (succeeded)
            break;
    }
    require(failures > 1, "multi-column insert did not exercise failures after the first allocation");
}

void test_recycled_rows_reuse_physical_slots() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto column = NativeColumn<i64>{};
    auto release = util::scope_exit{[&]() noexcept { column.release(memory); }};
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto first = column.add(10, first_time, memory);
    const auto second = column.add(20, first_time, memory);
    const auto slot_for = [&](row_id_t id) {
        auto slot = no_row;
        column.for_each_batch([&](const auto &batch) {
            for (auto position = std::size_t{}; position != batch.ids.size(); ++position)
                if (batch.ids[position] == id)
                    slot = batch.cells.first_slot + static_cast<row_id_t>(position);
        });
        require(slot != no_row, "live row did not appear in a native batch");
        return slot;
    };
    const auto first_slot = slot_for(first);

    const auto second_time = first_time + EvaluationTime::duration{1};
    require(column.erase(first, second_time, memory), "slot reuse test could not erase the first row");
    const auto third = column.add(30, second_time, memory);
    require(third != first && slot_for(third) == first_slot,
            "same-cycle row addition did not reuse the physical slot with a fresh row ID");

    auto rows = std::vector<std::pair<row_id_t, std::optional<i64>>>{};
    column.for_each_batch([&](const auto &batch) {
        require(batch.ids.size() == batch.cells.size, "column batch IDs and cells have different lengths");
        for (auto position = std::size_t{}; position != batch.cells.size; ++position) {
            const auto value = batch.cells.is_valid(position)
                                   ? std::optional<i64>{batch.cells.values[position]}
                                   : std::nullopt;
            rows.emplace_back(batch.ids[position], value);
        }
    });
    require(rows == std::vector<std::pair<row_id_t, std::optional<i64>>>{{third, 30}, {second, 20}},
            "reused physical slot batch did not align row IDs and values");

    const auto third_time = second_time + EvaluationTime::duration{1};
    require(column.erase(third, third_time, memory), "slot reuse test could not erase the replacement row");
    const auto fourth = column.add(40, third_time, memory);
    require(fourth == first && slot_for(fourth) == first_slot,
            "a row ID was not recycled after its removal cycle while reusing its slot");

    rows.clear();
    column.for_each_batch([&](const auto &batch) {
        require(batch.ids.size() == batch.cells.size, "recycled ID batch has mismatched lengths");
        for (auto position = std::size_t{}; position != batch.cells.size; ++position) {
            const auto value = batch.cells.is_valid(position)
                                   ? std::optional<i64>{batch.cells.values[position]}
                                   : std::nullopt;
            rows.emplace_back(batch.ids[position], value);
        }
    });
    require(rows == std::vector<std::pair<row_id_t, std::optional<i64>>>{{fourth, 40}, {second, 20}},
            "recycled row ID batch did not align with its current value");

    auto churn = NativeColumn<i64>{};
    auto release_churn = util::scope_exit{[&]() noexcept { churn.release(memory); }};
    auto current = churn.add(0, first_time, memory);
    for (auto cycle = std::uint32_t{1}; cycle <= 256; ++cycle) {
        const auto time = first_time + EvaluationTime::duration{cycle};
        require(churn.erase(current, time, memory), "bounded churn could not erase its live row");
        current = churn.add(static_cast<i64>(cycle), time, memory);
        require(churn.size() == 1 && churn.live_slots().extent() == 1,
                "bounded row churn expanded physical slot storage");
        require(churn.id_at(0) == current && churn.cell(current) == static_cast<i64>(cycle),
                "bounded row churn lost row and cell alignment");
        auto batch_rows = std::size_t{};
        churn.for_each_batch([&](const auto &batch) {
            require(batch.ids.size() == batch.cells.size && batch.cells.size == 1 &&
                        batch.ids[0] == current && batch.cells.is_valid(0) &&
                        batch.cells.values[0] == static_cast<i64>(cycle),
                    "bounded row churn batch lost ID and value alignment");
            ++batch_rows;
        });
        require(batch_rows == 1, "bounded row churn did not export exactly one batch");
    }
}

void test_failed_value_insert_rolls_back_reused_slot() {
    using namespace bg;
    using namespace bg::frame;

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    auto failures = std::size_t{};
    for (auto failure = std::size_t{1};; ++failure) {
        FailingResource resource;
        auto succeeded = false;
        {
            const auto memory = allocator{resource};
            auto frame = NativeFrame<BidOnly>{};
            auto release = util::scope_exit{[&]() noexcept { frame.release(memory); }};
            const auto first = frame.add(BidOnly<Cell>{.price = Cell<double>{std::nullopt}}, first_time, memory);
            const auto second = frame.add(BidOnly<Cell>{.price = Cell<double>{std::nullopt}}, first_time, memory);
            const auto second_time = first_time + EvaluationTime::duration{1};
            require(frame.erase(first, second_time, memory), "reused-slot failure test could not erase a row");

            resource.fail_at = resource.attempts + failure;
            try {
                frame.add(BidOnly<Cell>{.price = Cell<double>{42.0}}, second_time, memory);
                succeeded = true;
            } catch (const std::bad_alloc &) {
                ++failures;
                require(frame.size() == 1 && !frame.contains(first) && frame.contains(second) &&
                            !frame.cell<0>(second) && frame.rows().added().empty() &&
                            frame.rows().removed().contains(first) && frame.changed_columns().contains(0),
                        "failed value insertion corrupted rows or deltas after slot reuse");
            }
        }
        require(resource.allocations == resource.deallocations,
                "reused-slot insertion failure leaked frame storage");
        if (succeeded)
            break;
    }
    require(failures > 0, "reused-slot value insertion did not exercise an allocation failure");
}

void test_frame_scalar_views_and_snapshots() {
    using namespace bg;
    using namespace bg::frame;

    auto columns = Quotes<Column>{.size = Column<i64>{{i64{5}, i64{6}}},
                                  .bid = Column<double>{{10.5, 11.5}},
                                  .ask = Column<double>{{std::nullopt, 12.5}}};
    auto immutable = Frame<Quotes>{std::move(columns)};
    require(immutable.size() == 2, "immutable frame has wrong row count");
    auto immutable_view = FrameView<Quotes>{immutable};
    auto scalar = Scalar<Frame<Quotes>>::from(frame::details::frame_view_scalar_runtime_type<Quotes>(),
                                               std::addressof(immutable_view));
    const auto view = scalar.value();
    require(view.size() == 2 && view.columns().size.cell(1) == 6 && view.columns().bid.cell(0) == 10.5 &&
                !view.columns().ask.cell(0),
            "immutable frame scalar view lost columns, values, or nulls");
    require(&cpp_scalar_type<Frame<Quotes>>() == &cpp_scalar_type<Frame<Quotes>>(),
            "frame schema scalar identity is not canonical");
    require(TypeRegistry::instance().contains(cpp_scalar_type<Frame<Quotes>>()),
            "frame scalar semantic type was not registered");
    const auto &series_type = frame_time_series_type<Quotes>(TimeSeriesGuarantee::ordinary);
    require(&series_type == &frame_time_series_type<Quotes>(TimeSeriesGuarantee::ordinary),
            "frame schema time series identity is not canonical");
    require(TypeRegistry::instance().contains(series_type), "frame time series semantic type was not registered");
    auto scalar_type_name = std::string{};
    cpp_scalar_type<Frame<Quotes>>().format_name(scalar_type_name);
    require(scalar_type_name == "Frame<Quotes>", "frame scalar type formatted with the wrong name");
    auto series_type_name = std::string{};
    series_type.format_name(series_type_name);
    require(series_type_name == "TSF<Quotes>", "frame time series type formatted with the wrong name");
    require(&frame_time_series_runtime_type<Quotes>(series_type) ==
                &frame_time_series_runtime_type<Quotes>(series_type),
            "frame schema runtime identity is not canonical");

    auto empty = Frame<EmptySchema>{EmptySchema<Column>{}, 4};
    auto empty_view = FrameView<EmptySchema>{empty};
    auto empty_scalar = Scalar<Frame<EmptySchema>>::from(
        frame::details::frame_view_scalar_runtime_type<EmptySchema>(), std::addressof(empty_view));
    require(empty_scalar.value().size() == 4, "zero-column scalar frame lost its row count");

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto native = NativeFrame<Quotes>{};
    auto release = util::scope_exit{[&]() noexcept { native.release(memory); }};
    const auto time = MIN_DT + EvaluationTime::duration{1};
    const auto id = native.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                            .ask = Cell<double>{std::nullopt}},
                              time, memory);
    const auto &native_runtime = static_cast<const FrameSeriesRuntimeType &>(
        frame_time_series_runtime_type<Quotes>(series_type));
    auto native_scalar = Scalar<Frame<Quotes>>::from(native_runtime.scalar_runtime_type(),
                                                      std::addressof(native.core()));
    const auto snapshot_time = time + EvaluationTime::duration{1};
    FailingResource failing_memory;
    auto failing_allocator = allocator{failing_memory};
    auto succeeded = false;
    for (auto failure = std::size_t{1};; ++failure) {
        FailingResource attempt_memory;
        auto attempt_allocator = allocator{attempt_memory};
        attempt_memory.fail_at = attempt_memory.attempts + failure;
        try {
            auto owned = ScalarValue<Frame<Quotes>>{native_scalar, attempt_allocator};
            succeeded = true;
        } catch (const std::bad_alloc &) {
            require(attempt_memory.allocations == attempt_memory.deallocations,
                    "failed frame snapshot leaked an allocation");
        }
        require(attempt_memory.allocations == attempt_memory.deallocations,
                "frame snapshot cleanup leaked an allocation");
        if (succeeded)
            break;
    }

    {
        auto stable = ScalarValue<Frame<Quotes>>{native_scalar, failing_allocator};
        auto bid_patch = Quotes<PatchCell>{};
        bid_patch.bid = 99.0;
        native.set(id, bid_patch, snapshot_time, memory);
        require(stable.view().value().columns().bid.cell(0) == 10.5,
                "owning frame scalar changed after native row mutation");
    }
    require(failing_memory.allocations == failing_memory.deallocations, "owning frame snapshot leaked allocations");

    auto snapshot_allocator = allocator{};
    auto retained_snapshot = ScalarValue<Frame<Quotes>>{native_scalar, snapshot_allocator};
    const auto retained_view = retained_snapshot.view().value();
    const auto retained_source = scalar_storage_traits<Frame<Quotes>>::borrow_value(retained_view);
    const auto *retained_owner = frame::details::frame_snapshot_owner(retained_source);
    auto retained_copy = FrameSnapshotStorage{};
    const auto &snapshot_runtime = frame_snapshot_runtime_type(frame_type(frame_schema_type<Quotes>()));
    snapshot_runtime.construct_from_input(std::addressof(retained_copy), retained_source, snapshot_allocator);
    require(retained_owner != nullptr && retained_copy.owner == retained_owner,
            "frame snapshot construction did not retain its existing owner");
    auto *bundle_owner = bg::details::BundleAllocationOwner::create(snapshot_allocator);
    auto release_bundle_owner = util::scope_exit{[&]() noexcept { bundle_owner->release(); }};
    {
        const auto allocation_context = bg::details::ScalarAllocationContext{
            snapshot_allocator, *bundle_owner, std::addressof(retained_copy), 0};
        snapshot_runtime.assign_from_input(std::addressof(retained_copy), retained_source);
    }
    require(retained_copy.owner == retained_owner,
            "frame snapshot self-assignment did not retain its owner before releasing it");
    snapshot_runtime.destroy(std::addressof(retained_copy), snapshot_allocator);

    require(cpp_scalar_type<Frame<Quotes>>().accepts(cpp_scalar_type<Frame<EmptySchema>>()) == false,
            "frame semantic types with distinct schemas matched");

    const auto &generic_frame_type = frame_type(frame_schema_type<bg::SCHEMA>());
    const auto &concrete_frame_type = frame_type(frame_schema_type<Quotes>());
    auto generic_frame_name = std::string{};
    generic_frame_type.format_name(generic_frame_name);
    require(generic_frame_name == "Frame<SCHEMA>", "generic frame type formatted with the wrong name");
    auto frame_type_map = TypeMap{};
    require(generic_frame_type.match(concrete_frame_type, frame_type_map),
            "generic frame schema did not capture a concrete schema");
    require(!generic_frame_type.match(frame_type(frame_schema_type<EmptySchema>()), frame_type_map),
            "a repeated frame schema variable matched a different schema");
    require(&generic_frame_type.resolve(frame_type_map) == &concrete_frame_type,
            "generic frame schema did not resolve to the captured schema");

    const auto anonymous_fields = std::array{SchemaField{util::symbol{"value"}, &cpp_scalar_type<i64>()}};
    const auto &anonymous_schema = bg::anonymous_schema(anonymous_fields);
    require(&anonymous_schema == &bg::anonymous_schema(anonymous_fields),
            "equivalent anonymous schema descriptors were not canonical");
    require(&frame_type(anonymous_schema) == &frame_type(bg::anonymous_schema(anonymous_fields)),
            "frame types over canonical anonymous schemas were not canonical");
    require(&frame_series_type(anonymous_schema, TimeSeriesGuarantee::ordinary) ==
                &frame_series_type(bg::anonymous_schema(anonymous_fields), TimeSeriesGuarantee::ordinary),
            "frame series types over canonical anonymous schemas were not canonical");

    const auto optional_fields = std::array{SchemaField{util::symbol{"optional"}, &cpp_scalar_type<i64>(), true}};
    const auto &optional_schema = bg::anonymous_schema(optional_fields);
    auto optional_frame_rejected = false;
    auto optional_series_rejected = false;
    try {
        static_cast<void>(frame_type(optional_schema));
    } catch (const std::invalid_argument &) {
        optional_frame_rejected = true;
    }
    try {
        static_cast<void>(frame_series_type(optional_schema, TimeSeriesGuarantee::ordinary));
    } catch (const std::invalid_argument &) {
        optional_series_rejected = true;
    }
    require(optional_frame_rejected && optional_series_rejected,
            "frame scalar or time series accepted an optional column field");

    using OPTIONAL_CELL = ScalarVariable<"FRAME_OPTIONAL_CELL">;
    const auto optional_unresolved_fields = std::array{SchemaField{
        util::symbol{"optional_unresolved"}, &cpp_scalar_type<OPTIONAL_CELL>(), true}};
    const auto &optional_unresolved_schema = bg::anonymous_schema(optional_unresolved_fields);
    auto optional_unresolved_frame_rejected = false;
    auto optional_unresolved_series_rejected = false;
    try {
        static_cast<void>(frame_type(optional_unresolved_schema));
    } catch (const std::invalid_argument &) {
        optional_unresolved_frame_rejected = true;
    }
    try {
        static_cast<void>(frame_series_type(optional_unresolved_schema, TimeSeriesGuarantee::ordinary));
    } catch (const std::invalid_argument &) {
        optional_unresolved_series_rejected = true;
    }
    require(optional_unresolved_frame_rejected && optional_unresolved_series_rejected,
            "frame scalar or time series accepted an optional field with an unresolved scalar type");

    const auto non_scalar_fields = std::array{SchemaField{
        util::symbol{"nested"}, &column_time_series_type<i64>(TimeSeriesGuarantee::ordinary), false}};
    const auto &non_scalar_schema = bg::anonymous_schema(non_scalar_fields);
    auto non_scalar_frame_rejected = false;
    auto non_scalar_series_rejected = false;
    try {
        static_cast<void>(frame_type(non_scalar_schema));
    } catch (const std::invalid_argument &) {
        non_scalar_frame_rejected = true;
    }
    try {
        static_cast<void>(frame_series_type(non_scalar_schema, TimeSeriesGuarantee::ordinary));
    } catch (const std::invalid_argument &) {
        non_scalar_series_rejected = true;
    }
    require(non_scalar_frame_rejected && non_scalar_series_rejected,
            "frame scalar or time series accepted a non-scalar field");

    const auto &generic_series_type = frame_series_type(frame_schema_type<bg::SCHEMA>(),
                                                        TimeSeriesGuarantee::ordinary);
    auto generic_series_name = std::string{};
    generic_series_type.format_name(generic_series_name);
    require(generic_series_name == "TSF<SCHEMA>", "generic frame time series type formatted with the wrong name");
    auto series_type_map = TypeMap{};
    require(generic_series_type.match(series_type, series_type_map),
            "generic frame series schema did not capture a concrete schema");
    require(&generic_series_type.resolve(series_type_map) == &series_type,
            "generic frame series schema did not resolve to the captured schema");

    const auto &inherited_base_series =
        frame_series_type(frame_schema_type<InheritedFrameBase>(), TimeSeriesGuarantee::ordinary);
    const auto &inherited_derived_series =
        frame_series_type(frame_schema_type<InheritedFrame>(), TimeSeriesGuarantee::ordinary);
    auto inherited_match_map = TypeMap{};
    require(inherited_base_series.match(inherited_derived_series, inherited_match_map),
            "inherited TSF did not match its named base schema");
    auto reverse_inherited_match_map = TypeMap{};
    require(!inherited_derived_series.match(inherited_base_series, reverse_inherited_match_map),
            "inherited TSF accepted its base schema as a derived schema");
    auto unrelated_schema_match_map = TypeMap{};
    require(!inherited_base_series.match(series_type, unrelated_schema_match_map),
            "inherited TSF accepted an unrelated named schema with matching fields");

    const auto integer_fields =
        std::array{SchemaField{util::symbol{"value"}, &cpp_scalar_type<i64>(), false}};
    const auto double_fields =
        std::array{SchemaField{util::symbol{"value"}, &cpp_scalar_type<double>(), false}};
    const auto &integer_schema = bg::anonymous_schema(integer_fields);
    const auto &double_schema = bg::anonymous_schema(double_fields);
    const auto &integer_series = frame_series_type(integer_schema, TimeSeriesGuarantee::ordinary);
    const auto &double_series = frame_series_type(double_schema, TimeSeriesGuarantee::ordinary);
    auto mismatched_cell_type_map = TypeMap{};
    require(!integer_series.match(double_series, mismatched_cell_type_map),
            "TSF matched anonymous schemas with incompatible column cell types");

    const auto &ordinary_series = frame_series_type(frame_schema_type<Quotes>(), TimeSeriesGuarantee::ordinary);
    for (const auto limit : {TimeSeriesGuarantee::final, TimeSeriesGuarantee::const_}) {
        auto rejected_guarantee = false;
        try {
            static_cast<void>(frame_series_type(frame_schema_type<Quotes>(), limit));
        } catch (const std::invalid_argument &) {
            rejected_guarantee = true;
        }
        require(rejected_guarantee, "TSF accepted a nonordinary guarantee");

        const auto &limited = dynamic_cast<const FrameSeriesType &>(
            ordinary_series.with_guarantee_limit(limit));
        require(std::addressof(limited) == std::addressof(ordinary_series),
                "TSF guarantee limit strengthened its canonical ordinary type");
        require(limited.guarantee().value() == TimeSeriesGuarantee::ordinary,
                "TSF guarantee limit changed its ordinary frame guarantee");
        for (auto index = std::size_t{}; index != limited.item_count(); ++index) {
            const auto &column = dynamic_cast<const ColumnSeriesType &>(limited.item_type(index));
            require(column.guarantee().value() == TimeSeriesGuarantee::ordinary,
                    "TSF guarantee limit changed its ordinary child TSC guarantee");
        }
    }

    auto shared_schema_type_map = TypeMap{};
    require(generic_frame_type.match(concrete_frame_type, shared_schema_type_map),
            "generic frame scalar did not capture a shared schema");
    require(generic_series_type.match(series_type, shared_schema_type_map),
            "generic frame time series did not reuse the scalar schema capture");
    require(&generic_series_type.resolve(shared_schema_type_map) == &series_type,
            "generic frame time series resolved to a different schema than the scalar");
    const auto &shared_schema_variable = dynamic_cast<const Variable &>(generic_frame_type.schema_type());
    const auto *captured_schema = shared_schema_type_map.find(shared_schema_variable);
    const auto &mismatched_series_type = frame_series_type(frame_schema_type<EmptySchema>(),
                                                           TimeSeriesGuarantee::ordinary);
    require(!generic_series_type.match(mismatched_series_type, shared_schema_type_map),
            "generic frame time series accepted a different schema in a shared type map");
    require(shared_schema_type_map.find(shared_schema_variable) == captured_schema,
            "a failed time series match changed the shared scalar schema capture");

    const auto &anonymous_series_type = frame_series_type(anonymous_schema, TimeSeriesGuarantee::ordinary);
    auto anonymous_type_map = TypeMap{};
    require(generic_frame_type.match(frame_type(anonymous_schema), anonymous_type_map) &&
                generic_series_type.match(anonymous_series_type, anonymous_type_map),
            "Frame and TSF did not share an anonymous runtime schema capture");
    require(&generic_frame_type.resolve(anonymous_type_map) == &frame_type(anonymous_schema) &&
                &generic_series_type.resolve(anonymous_type_map) == &anonymous_series_type,
            "Frame and TSF did not resolve to the shared anonymous runtime schema");

    const auto &empty_series_type = frame_series_type(frame_schema_type<EmptySchema>(),
                                                      TimeSeriesGuarantee::ordinary);
    require(generic_series_type.item_count() == std::dynamic_extent && empty_series_type.item_count() == 0,
            "unknown frame schema arity was confused with an empty schema");

    constexpr auto FRAME_GUARANTEE = guarantee_variable<"FRAME_SCHEMA_GUARANTEE">;
    const auto guarantee_parameter = bg::details::guarantee_parameter<FRAME_GUARANTEE>();
    const auto &schema_and_guarantee_type = frame_series_type(frame_schema_type<bg::SCHEMA>(), guarantee_parameter);
    auto guarantee_type_map = TypeMap{};
    require(schema_and_guarantee_type.match(series_type, guarantee_type_map),
            "frame series schema and guarantee variables did not match");
    require(&schema_and_guarantee_type.resolve(guarantee_type_map) == &series_type,
            "frame series schema and guarantee variables did not resolve");

    static const auto minimum_deduplicated =
        GuaranteeVariable{util::symbol{"FRAME_SCHEMA_MIN_DEDUPLICATED"}, TimeSeriesGuarantee::deduplicated};
    const auto &constrained_series_type = frame_series_type(
        frame_schema_type<bg::SCHEMA>(), GuaranteeParameter{minimum_deduplicated});
    const auto &schema_variable = dynamic_cast<const Variable &>(constrained_series_type.schema_type());
    auto rollback_type_map = TypeMap{};
    require(!constrained_series_type.match(series_type, rollback_type_map),
            "a frame guarantee variable matched below its minimum guarantee");
    require(rollback_type_map.find(schema_variable) == nullptr,
            "failed frame series matching retained a schema variable mapping");
    require(rollback_type_map.find(minimum_deduplicated) == nullptr,
            "failed frame series matching retained a guarantee variable mapping");
}

void test_runtime_frame_value_factory() {
    using GenericFrame = Frame<bg::SCHEMA>;
    using GenericValue = ScalarValue<GenericFrame>;

    const auto fields = std::array{SchemaField{util::symbol{"value"}, &cpp_scalar_type<i64>()}};
    const auto &schema = anonymous_schema(fields);
    auto source = std::optional<GenericValue>{};
    auto memory_resource = FailingResource{};
    auto memory = allocator{memory_resource};
    {
        auto column = Column<i64>{{11, 22, 33}};
        const auto columns = std::array{ColumnView<>{column.view()}};
        static_assert(std::same_as<decltype(make_frame_value(memory, schema, 3, columns)), GenericValue>);
        source.emplace(make_frame_value(memory, schema, 3, columns));
    }

    const auto view = source->view().value();
    require(view.size() == 3 && view.schema().field_count() == 1 &&
                view.schema().field(0).name == util::symbol{"value"} &&
                view.column("value").view<i64>().cell(0) == 11 &&
                view.column("value").view<i64>().cell(2) == 33,
            "runtime-schema frame value lost its schema, values, or source independence");

    const auto empty_fields = std::array<SchemaField, 0>{};
    const auto &empty_schema = anonymous_schema(empty_fields);
    auto empty = std::optional<GenericValue>{};
    empty.emplace(make_frame_value(memory, empty_schema, 5, std::span<const ColumnView<>>{}));
    require(empty->view().value().size() == 5 && empty->view().value().schema().field_count() == 0,
            "zero-column frame value lost its nonzero row count or schema");

    const auto mixed_fields = std::array{
        SchemaField{util::symbol{"number"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()},
    };
    const auto &mixed_schema = anonymous_schema(mixed_fields);
    auto numbers = Column<i64>{{11, std::nullopt}};
    auto prices = Column<double>{{std::nullopt, 2.5}};
    const auto mixed_columns = std::array{ColumnView<>{numbers.view()}, ColumnView<>{prices.view()}};
    auto mixed = std::optional<GenericValue>{make_frame_value(memory, mixed_schema, 2, mixed_columns)};
    const auto mixed_view = mixed->view().value();
    require(mixed_view.column("number").cell<i64>(0) == 11 && !mixed_view.column("number").cell<i64>(1) &&
                !mixed_view.column("price").cell<double>(0) && mixed_view.column("price").cell<double>(1) == 2.5,
            "runtime-schema frame factory changed mixed numeric values or nulls");

    const auto rejects_columns = [&](const bg::Schema &target_schema, std::size_t row_count,
                                     std::span<const ColumnView<>> values) {
        try {
            static_cast<void>(make_frame_value(memory, target_schema, row_count, values));
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    const auto wrong_count = std::array{ColumnView<>{numbers.view()}};
    require(rejects_columns(mixed_schema, 2, wrong_count),
            "runtime-schema frame factory accepted a mismatched column count");
    const auto wrong_type = std::array{ColumnView<>{prices.view()}, ColumnView<>{prices.view()}};
    require(rejects_columns(mixed_schema, 2, wrong_type),
            "runtime-schema frame factory accepted a mismatched column type");
    auto short_prices = Column<double>{{2.5}};
    const auto wrong_length = std::array{ColumnView<>{numbers.view()}, ColumnView<>{short_prices.view()}};
    require(rejects_columns(mixed_schema, 2, wrong_length),
            "runtime-schema frame factory accepted columns with different lengths");

    source.reset();
    mixed.reset();
    empty.reset();
    require(memory_resource.allocations == memory_resource.deallocations,
            "runtime-schema frame values did not release successful allocations");

    auto failures = std::size_t{};
    auto succeeded = false;
    for (auto failure = std::size_t{1};; ++failure) {
        auto attempt_resource = FailingResource{};
        auto attempt_memory = allocator{attempt_resource};
        attempt_resource.fail_at = failure;
        try {
            auto column = Column<i64>{{7, 9}};
            const auto columns = std::array{ColumnView<>{column.view()}};
            auto value = make_frame_value(attempt_memory, schema, 2, columns);
            require(value.view().value().column("value").view<i64>().cell(1) == 9,
                    "runtime-schema frame value changed during allocation retry");
            succeeded = true;
        } catch (const std::bad_alloc &) {
            ++failures;
        }
        require(attempt_resource.allocations == attempt_resource.deallocations,
                "runtime-schema frame value factory leaked during allocation failure or success");
        if (succeeded)
            break;
    }
    require(failures > 0, "runtime-schema frame value factory did not exercise allocation rollback");
}

void test_scalar_variable_frame_input() {
    using namespace bg;
    using namespace bg::frame;

    const auto &generic_schema = frame_schema_type<GenericQuote>();
    auto type_map = TypeMap{};
    type_map.set(bg::details::scalar_variable_instance<SCALAR>(), &cpp_scalar_type<double>());
    const auto &schema = dynamic_cast<const bg::Schema &>(generic_schema.resolve(type_map));
    auto column = Column<double>{{12.5}};
    const auto columns = std::array{ColumnView<>{column.view()}};
    auto memory = allocator{};
    auto source = make_frame_value(memory, schema, 1, columns);
    auto erased_source = ScalarValue<>{source};
    auto ticks = std::vector<std::optional<ScalarValue<>>>{};
    ticks.emplace_back(std::in_place, erased_source.view());
    const auto inputs = std::array{testing::TestGraphInput{erased_source.runtime_type(), ticks}};

    const auto read = node([](In<TS<Frame<GenericQuote>>> input, Out<TS<double>> output) {
        const auto value = input.value_view().value();
        const auto bid = value.columns().bid.view<double>();
        require(bid.cell(0) == value.column("bid").view<double>().cell(0),
                "generic frame view field did not expose its resolved cell type");
        try {
            static_cast<void>(value.columns().bid.view<i64>());
            throw std::runtime_error{"generic frame view accepted the wrong cell type"};
        } catch (const std::invalid_argument &) {
        }
        output.set(bid.cell(0).value());
    });
    using namespace bg::literals;
    const auto graph = bg::graph("name"_n = "generic_quote_input", [&](wp<TS<Frame<SCHEMA>>> input) {
        return read(input);
    });
    const auto result = testing::test_graph(graph, std::span<const testing::TestGraphInput>{inputs});

    require(result.equals<double>({12.5}),
            "generic frame input did not read a scalar-variable field by name");
}

void test_inherited_frame_fields_and_views() {
    using namespace bg;
    using namespace bg::frame;
    using Reflection = bg::details::hierarchical_scalar_schema_reflection<InheritedFrame>;

    static_assert(Reflection::field_count == 3);
    static_assert(Reflection::field_name<0>() == "size");
    static_assert(Reflection::field_name<1>() == "bid");
    static_assert(Reflection::field_name<2>() == "ask");
    require(Reflection::field_index("bid") == 1,
            "inherited frame reflection did not preserve the shadowed field's logical position");

    auto immutable = Frame<InheritedFrame>{InheritedFrame<Column>{
        InheritedFrameBase<Column>{Column<i64>{{5, 6}}, Column<double>{{99.0, 100.0}}},
        Column<double>{{12.5, 23.0}}, Column<double>{{std::nullopt, 21.5}}}};
    auto immutable_copy = Frame<InheritedFrame>{immutable};
    auto immutable_view = FrameView<InheritedFrame>{immutable_copy};
    const auto immutable_fields = Reflection::fields(immutable_view.columns());
    require(immutable_view.size() == 2 && std::get<0>(immutable_fields).cell(1) == 6 &&
                std::get<1>(immutable_fields).cell(0) == 12.5 && !std::get<2>(immutable_fields).cell(0),
            "inherited scalar frame construction or copy exposed the hidden base field");

    auto memory = allocator{};
    auto source = NativeFrame<InheritedFrame>{};
    auto source_release = util::scope_exit{[&]() noexcept { source.release(memory); }};

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto first = source.add(
        InheritedFrame<Cell>{InheritedFrameBase<Cell>{Cell<i64>{5}, Cell<double>{99.0}},
                             Cell<double>{12.5}, Cell<double>{std::nullopt}},
        first_time, memory);
    const auto second = source.add(
        InheritedFrame<Cell>{InheritedFrameBase<Cell>{Cell<i64>{6}, Cell<double>{100.0}},
                             Cell<double>{23.0}, Cell<double>{21.5}},
        first_time, memory);
    require(source.cell<0>(first) == 5 && source.cell<1>(first) == 12.5 && !source.cell<2>(first) &&
                source.cell<0>(second) == 6 && source.cell<1>(second) == 23.0 && source.cell<2>(second) == 21.5,
            "inherited native frame rows lost base, direct, or null cells");

    const auto &series_type = frame_time_series_type<InheritedFrame>(TimeSeriesGuarantee::ordinary);
    const auto &native_runtime = static_cast<const FrameSeriesRuntimeType &>(
        frame_time_series_runtime_type<InheritedFrame>(series_type));
    auto scalar = Scalar<Frame<InheritedFrame>>::from(native_runtime.scalar_runtime_type(),
                                                       std::addressof(source.core()));
    auto stable = ScalarValue<Frame<InheritedFrame>>{scalar, memory};
    auto base_scalar = Scalar<Frame<InheritedFrameBase>>::from(native_runtime.scalar_runtime_type(),
                                                                std::addressof(source.core()));
    const auto base_view = base_scalar.value();
    require(base_view.columns().size.cell(0) == 5 && base_view.columns().bid.cell(0) == 12.5 &&
                base_view.schema().field_count() == 3 && !base_view.column("ask").view<double>().cell(0),
            "base scalar view did not expose its declared and retained derived fields");
    auto stable_base = ScalarValue<Frame<InheritedFrameBase>>{base_scalar, memory};

    auto derived_snapshot = ScalarValue<Frame<InheritedFrame>>{scalar, memory};
    const auto derived_snapshot_view = derived_snapshot.view();
    const auto base_annotation = Scalar<Frame<InheritedFrameBase>>::from(derived_snapshot_view);
    auto retained_base_snapshot = ScalarValue<Frame<InheritedFrameBase>>{base_annotation, memory};
    const auto *derived_snapshot_owner =
        frame::details::frame_snapshot_owner(bg::details::scalar_data(derived_snapshot));
    const auto *base_snapshot_owner =
        frame::details::frame_snapshot_owner(bg::details::scalar_data(retained_base_snapshot));
    require(derived_snapshot_owner != nullptr && base_snapshot_owner == derived_snapshot_owner &&
                retained_base_snapshot.view().value().schema().field_count() == 3,
            "base annotation did not retain the derived snapshot owner and schema");

    auto patch = InheritedFrame<PatchCell>{InheritedFrameBase<PatchCell>{PatchCell<i64>{}, PatchCell<double>{99.5}},
                                           PatchCell<double>{std::nullopt}, PatchCell<double>{15.5}};
    const auto second_time = first_time + EvaluationTime::duration{1};
    require(source.set(first, patch, second_time, memory), "inherited native frame rejected a row patch");
    require(source.cell<0>(first) == 5 && !source.cell<1>(first) && source.cell<2>(first) == 15.5 &&
                stable.view().value().columns().bid.cell(0) == 12.5,
            "inherited native frame patch or scalar snapshot used the shadowed base field");
    const auto stable_base_view = stable_base.view().value();
    require(stable_base_view.schema().field_count() == 3 &&
                stable_base_view.columns().bid.cell(0) == 12.5 &&
                !stable_base_view.column("ask").view<double>().cell(0),
            "base scalar copy lost its derived-only column after source mutation");
    require(source.changed_columns().contains(1) && source.changed_columns().contains(2),
            "inherited native frame patch did not publish logical column ordinals");

    auto input = native_frame_input(source);
    auto predicate = [&](row_id_t id) { return source.cell<1>(id).value_or(0.0) > 10.0; };
    const auto [predicate_context, predicate_function] = runtime_predicate(predicate);
    const auto predicate_index = input.field_index(util::symbol{"bid"});
    const auto all_fields = std::array<std::size_t, 3>{0, 1, 2};
    const auto bid_field = std::array{predicate_index};
    with_runtime_view_storage<InheritedFrame>(memory, [&](auto &filter, const auto &) {
        require(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function, second_time,
                            memory),
                "inherited frame filter did not bind its logical predicate field");
        require(!filter.selected_rows.contains(first) && filter.selected_rows.contains(second),
                "inherited frame filter selected rows using the shadowed base field");

        with_runtime_view_storage<BidOnly>(memory, [&](auto &projection, const auto &runtime) {
            require(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, second_time, memory),
                    "inherited frame projection did not bind a direct field");
            auto projected = runtime.input_storage(std::addressof(projection));
            require(projected.generic_column(0).template view<double>().cell(1) == 23.0,
                    "inherited frame projection read the wrong direct field");

            const auto third_time = second_time + EvaluationTime::duration{1};
            const auto ask_field = std::array{input.field_index(util::symbol{"ask"})};
            require(projection.bind(input, ask_field, std::nullopt, nullptr, nullptr, third_time, memory),
                    "inherited frame projection did not rebind to another logical field");
            projected = runtime.input_storage(std::addressof(projection));
            require(projected.generic_column(0).template view<double>().cell(1) == 21.5,
                    "inherited frame projection rebind exposed a hidden base field");
        });

        with_runtime_view_storage<InheritedFrameBase>(memory, [&](auto &base_filter, const auto &runtime) {
            require(base_filter.bind(input, {}, 1, predicate_context, predicate_function, second_time, memory),
                    "base-schema filter did not map its declared fields to the derived input schema");
            const auto output = runtime.input_storage(std::addressof(base_filter));
            require(output.schema->field_count() == 2 && output.size == 1 &&
                        base_filter.selected_rows.contains(second) && !base_filter.selected_rows.contains(first) &&
                        output.generic_column(1).template view<double>().cell(0) == 23.0,
                    "base-schema filter did not retain its output field count or select the derived bid column");
        });

        const auto third_time = second_time + EvaluationTime::duration{1};
        require(source.erase(second, third_time, memory), "inherited native frame could not remove a row");
        input = native_frame_input(source);
        require(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function, third_time,
                            memory) && filter.removed_rows.contains(second),
                "inherited frame filter did not forward row removal");
    });
}

void test_scalar_frame_time_series_graph() {
    using namespace bg;
    using namespace bg::frame;

    using FrameValue = Frame<Quotes>;
    using Columns = Quotes<Column>;
    const auto first = FrameValue{Columns{.size = Column<i64>{{i64{5}}}, .bid = Column<double>{{10.5}},
                                          .ask = Column<double>{{std::nullopt}}}};
    const auto second = FrameValue{Columns{.size = Column<i64>{{i64{6}}}, .bid = Column<double>{{11.5}},
                                           .ask = Column<double>{{12.5}}}};
    const auto copy = bg::node([](In<TS<FrameValue>> input, Out<TS<FrameValue>> output) {
        const auto value = input.value();
        const auto scalar = Scalar<FrameValue>::from(frame::details::frame_view_scalar_runtime_type<Quotes>(),
                                                      std::addressof(value));
        output.set_value(scalar);
    });
    const auto graph = [&](wp<TS<FrameValue>> input) { return copy(input); };
    const auto result = testing::test_graph(graph, std::vector{first, second});
    require(result.size() == 2 && result[0] && result[1], "TS<Frame> graph lost scalar frame values");
    require(result[0]->value().columns().bid.cell(0) == 10.5 && !result[0]->value().columns().ask.cell(0) &&
                result[1]->value().columns().size.cell(0) == 6,
            "TS<Frame> graph changed the scalar frame data");
}

struct frame_writer_state {
    row_id_t row{no_row};
};

struct borrowed_frame_observation {
    const TimeSeriesRuntimeType *runtime_type{};
    bool zero_copy_source{};
    bool zero_copy_column{};
    bool modified{};
    EvaluationTime last_updated{MIN_DT};
    std::vector<row_id_t> rows;
    std::vector<row_id_t> changed;
    std::vector<row_id_t> size_modified;
    std::vector<row_id_t> bid_modified;
    std::vector<row_id_t> ask_modified;
    std::vector<std::optional<i64>> sizes;
    std::vector<std::optional<double>> bids;
    std::vector<std::optional<double>> asks;
};

struct borrowed_frame_sink {
    In<TSF<Quotes>, optional> input;

    void evaluate() const {
        const auto scalar = input.value_view();
        auto observation = borrowed_frame_observation{
            .runtime_type = std::addressof(input.runtime_type()),
            .zero_copy_source = input.input_data().data() == expected_data &&
                                scalar.data() == expected_data,
            .zero_copy_column = false,
            .modified = input.modified(),
            .last_updated = input.last_updated(),
        };
        auto frame = scalar.value();
        frame.columns().bid.batches([&](const auto &batch) {
            if (batch.values != nullptr && expected_bid_values != nullptr)
                observation.zero_copy_column = batch.values == expected_bid_values;
        });
        input.for_each_row([&](row_id_t id, std::size_t) {
            observation.rows.push_back(id);
            observation.sizes.push_back(input.cell<0>(id));
            observation.bids.push_back(input.cell<1>(id));
            observation.asks.push_back(input.cell<2>(id));
        });
        input.for_each_changed([&](row_id_t id) { observation.changed.push_back(id); });
        input.for_each_modified<0>([&](row_id_t id) { observation.size_modified.push_back(id); });
        input.for_each_modified<1>([&](row_id_t id) { observation.bid_modified.push_back(id); });
        input.for_each_modified<2>([&](row_id_t id) { observation.ask_modified.push_back(id); });
        observations.push_back(std::move(observation));
    }

    inline static const double *expected_bid_values{};
    inline static const void *expected_data{};
    inline static std::vector<borrowed_frame_observation> observations;
};

struct borrowed_frame_runner {
    In<TSF<Quotes>, optional> input;
    In<TSF<Quotes>, optional> source;

    void start(EvaluationContext *context) const {
        auto &parent = context->runtime_graph();
        evaluator = parent.allocation().new_object<RuntimeGraphEvaluator>();
        evaluator->bind_owner(parent, context->node_index());
        child = parent.allocation().new_object<RuntimeGraph>(*compiled_graph, parent.allocation(), *evaluator,
                                                             context->node_index());

        const auto data = input.input_data();
        const auto inputs = std::array{BorrowedInput{std::addressof(data.last_updated_ref()), data.data(),
                                                     std::addressof(data.runtime_type()),
                                                     BorrowedInputChange::unchanged}};
        auto section = bg::details::BorrowedInputSection{*child, inputs};
        auto child_context = context->child(section);
        child->start(child_context);
    }

    void evaluate(EvaluationContext *context) const {
        const auto input_data = input.input_data();
        borrowed_frame_sink::expected_data = input_data.data();
        const auto source_value = source.value_view().value();
        source_value.columns().bid.batches([&](const auto &batch) {
            if (batch.values != nullptr)
                borrowed_frame_sink::expected_bid_values = batch.values;
        });

        auto change = BorrowedInputChange::unchanged;
        if (input.became_invalid())
            change = BorrowedInputChange::invalidation;
        else if (input.became_valid())
            change = BorrowedInputChange::revalidation;
        else if (input.modified())
            change = BorrowedInputChange::update;

        const auto inputs = std::array{BorrowedInput{std::addressof(input_data.last_updated_ref()), input_data.data(),
                                                     std::addressof(input_data.runtime_type()), change}};
        auto section = bg::details::BorrowedInputSection{*child, inputs};
        auto child_context = context->child(section);
        child->evaluate(child_context);
    }

    void stop(EvaluationContext *context) const noexcept {
        if (child == nullptr)
            return;

        const auto data = input.input_data();
        const auto inputs = std::array{BorrowedInput{std::addressof(data.last_updated_ref()), data.data(),
                                                     std::addressof(data.runtime_type()),
                                                     BorrowedInputChange::unchanged}};
        auto section = bg::details::BorrowedInputSection{*child, inputs};
        auto child_context = context->child(section);
        child->stop(child_context);
        child->finalize(child_context);
        context->runtime_graph().allocation().delete_object(child);
        child = nullptr;
        context->runtime_graph().allocation().delete_object(evaluator);
        evaluator = nullptr;
    }

    inline static const CompiledGraph *compiled_graph{};
    inline static RuntimeGraphEvaluator *evaluator{};
    inline static RuntimeGraph *child{};
};

CompiledGraph make_borrowed_frame_graph() {
    auto wiring = GraphWiring{};
    const auto input = wiring.borrowed_input(TSF<Quotes>::type());
    const auto sink = node<borrowed_frame_sink>();
    {
        auto context = WiringContext{wiring};
        sink(input);
    }
    wiring.finalize();
    return CompiledGraph{wiring};
}

struct frame_observation {
    bool valid{};
    bool became_valid{};
    bool became_invalid{};
    bool modified{};
    std::size_t size{};
    std::vector<row_id_t> rows;
    std::vector<row_id_t> changed;
    std::vector<row_id_t> removed;
    std::vector<row_id_t> bid_modified;
};

struct column_observation {
    bool valid{};
    bool became_valid{};
    bool became_invalid{};
    bool modified{};
    bool zero_copy{};
    std::size_t size{};
    std::vector<row_id_t> rows;
    std::vector<row_id_t> changed;
    std::vector<row_id_t> removed;
};

struct tsf_writer {
    In<TS<i64>> command;
    State<frame_writer_state> state;
    Out<TSF<Quotes>> output;

    void evaluate() const {
        switch (*command) {
        case 0:
            output.clear();
            return;
        case 1:
            state->row = output.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                                 .ask = Cell<double>{std::nullopt}});
            return;
        case 2: {
            output.set(state->row, RowPatch<Quotes>{.bid = 11.5});
            return;
        }
        case 3: {
            auto patch = Quotes<PatchCell>{};
            patch.ask = 12.5;
            output.set(state->row, patch);
            return;
        }
        case 4:
            output.set(state->row, Quotes<PatchCell>{});
            return;
        case 5:
            require(output.erase(state->row), "TSF writer could not remove its row");
            return;
        case 6:
            output.invalidate();
            return;
        case 7:
            output.clear();
            return;
        case 8: {
            const auto temporary = output.add(Quotes<Cell>{.size = Cell<i64>{8}, .bid = Cell<double>{8.5},
                                                            .ask = Cell<double>{std::nullopt}});
            require(output.erase(temporary), "TSF writer could not erase a new row");
            return;
        }
        default:
            throw std::runtime_error{"unknown TSF test command"};
        }
    }
};

struct inherited_tsf_writer {
    In<TS<i64>> command;
    State<frame_writer_state> state;
    Out<TSF<InheritedFrame>> output;

    void evaluate() const {
        switch (*command) {
        case 0:
            output.clear();
            return;
        case 1:
            state->row = output.add(InheritedFrame<Cell>{
                InheritedFrameBase<Cell>{Cell<i64>{5}, Cell<double>{99.0}}, Cell<double>{12.5},
                Cell<double>{std::nullopt}});
            return;
        case 2: {
            auto patch = InheritedFrame<PatchCell>{InheritedFrameBase<PatchCell>{PatchCell<i64>{},
                                                                                 PatchCell<double>{99.5}},
                                                        PatchCell<double>{std::nullopt}, PatchCell<double>{15.5}};
            output.set(state->row, patch);
            return;
        }
        case 3:
            require(output.erase(state->row), "inherited TSF writer could not remove its row");
            return;
        default:
            throw std::runtime_error{"unknown inherited TSF test command"};
        }
    }
};

struct runtime_schema_tsf_writer_state {
    row_id_t first{no_row};
    row_id_t second{no_row};
    row_id_t generic{no_row};
    row_id_t generic_partial{no_row};
    row_id_t inherited{no_row};
};

struct runtime_schema_tsf_writer {
    In<TS<Frame<SCHEMA>>> schema_seed;
    In<TS<i64>> command;
    State<runtime_schema_tsf_writer_state> state;
    Out<TSF<SCHEMA>> output;

    void evaluate() const {
        require(output.schema().field_count() == 3 && output.schema().field(0).name == util::symbol{"size"} &&
                    output.schema().field(1).name == util::symbol{"bid"} &&
                    output.schema().field(2).name == util::symbol{"ask"},
                "runtime-schema TSF output did not expose its resolved schema");
        static_cast<void>(schema_seed);

        switch (*command) {
        case 0:
            return;
        case 1: {
            const auto size = i64{5};
            const auto bid = 10.5;
            const auto size_scalar = Scalar<i64>::from(size);
            const auto bid_scalar = Scalar<double>::from(bid);
            const auto row = std::array<std::optional<Scalar<>>, 3>{
                Scalar<>::from(size_scalar), Scalar<>::from(bid_scalar), std::nullopt};
            state->first = output.add(row);
            return;
        }
        case 2: {
            const auto bid = 11.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto patch = std::array{
                std::pair{util::symbol{"bid"},
                          std::optional<Scalar<>>{Scalar<>::from(typed_bid)}}};
            output.set(state->first, patch);
            return;
        }
        case 3:
            state->second = output.add(ReorderedQuotes<Cell>{.ask = Cell<double>{21.0}, .size = Cell<i64>{8},
                                                             .bid = Cell<double>{20.5}});
            return;
        case 4:
            output.set(state->second, RowPatch<ReorderedQuotes>{.bid = std::nullopt});
            return;
        case 5: {
            const auto wrong_count = std::array<std::optional<Scalar<>>, 1>{};
            auto rejected = false;
            try {
                static_cast<void>(output.add(wrong_count));
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "runtime-schema TSF output accepted a row with the wrong field count");
            return;
        }
        case 6:
            output.set(state->first, std::span<const std::pair<util::symbol, std::optional<Scalar<>>>>{});
            return;
        case 7: {
            const auto patch = std::array{
                std::pair{util::symbol{"bid"}, std::optional<Scalar<>>{}}};
            output.set(state->first, patch);
            return;
        }
        case 8: {
            const auto wrong_size_value = 5.0;
            const auto bid_value = 12.5;
            const auto wrong_size = Scalar<double>::from(wrong_size_value);
            const auto bid = Scalar<double>::from(bid_value);
            const auto wrong_type = std::array<std::optional<Scalar<>>, 3>{
                Scalar<>::from(wrong_size), Scalar<>::from(bid), std::nullopt};
            auto type_rejected = false;
            try {
                static_cast<void>(output.add(wrong_type));
            } catch (const std::invalid_argument &) {
                type_rejected = true;
            }
            auto name_rejected = false;
            try {
                const auto unknown_name = std::array{
                    std::pair{util::symbol{"unknown"}, std::optional<Scalar<>>{}}};
                output.set(state->first, unknown_name);
            } catch (const std::out_of_range &) {
                name_rejected = true;
            }
            auto duplicate_rejected = false;
            try {
                const auto generic_bid = Scalar<>::from(bid);
                const auto duplicate = std::array{
                    std::pair{util::symbol{"bid"}, std::optional<Scalar<>>{generic_bid}},
                    std::pair{util::symbol{"bid"}, std::optional<Scalar<>>{}},
                };
                output.set(state->first, duplicate);
            } catch (const std::invalid_argument &) {
                duplicate_rejected = true;
            }
            require(type_rejected && name_rejected && duplicate_rejected,
                    "runtime-schema TSF output accepted a wrong type, unknown name, or duplicate patch field");
            return;
        }
        case 9:
            static_cast<void>(output.add(RowPatch<Quotes>{.bid = 30.5}));
            return;
        case 10:
            static_cast<void>(output.add(RowPatch<Quotes>{}));
            return;
        case 11: {
            const auto bid = 40.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            state->generic = output.add(MixedGenericQuote<Cell>{
                .size = Cell<i64>{9}, .bid = Cell<SCALAR>{generic_bid}, .ask = Cell<double>{41.5}});
            return;
        }
        case 12: {
            const auto bid = 42.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            output.set(state->generic, RowPatch<MixedGenericQuote>{.bid = PatchCell<SCALAR>{generic_bid}});
            return;
        }
        case 13:
            output.set(state->generic, RowPatch<MixedGenericQuote>{.bid = std::nullopt});
            return;
        case 14: {
            const auto bid = 50.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            state->generic_partial = output.add(RowPatch<MixedGenericQuote>{
                .bid = PatchCell<SCALAR>{generic_bid}});
            return;
        }
        case 15:
            static_cast<void>(output.add(RowPatch<MixedGenericQuote>{}));
            return;
        case 16: {
            const auto wrong_bid = i64{60};
            const auto typed_bid = Scalar<i64>::from(wrong_bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            auto rejected = false;
            try {
                static_cast<void>(output.add(MixedGenericQuote<Cell>{
                    .size = Cell<i64>{10}, .bid = Cell<SCALAR>{generic_bid}, .ask = Cell<double>{61.5}}));
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "runtime-schema TSF writer accepted a generic cell with the wrong runtime type");
            return;
        }
        case 17: {
            const auto bid = 62.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            auto rejected = false;
            try {
                output.set(state->generic, RowPatch<WrongMixedGenericQuote>{
                    .bid = PatchCell<SCALAR>{generic_bid}});
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "generic patch skipped validation of an omitted concrete source field");
            return;
        }
        case 18:
            output.set(state->generic_partial, RowPatch<MixedGenericQuote>{});
            return;
        case 19: {
            const auto bid = 65.5;
            const auto ask = 66.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto typed_ask = Scalar<double>::from(ask);
            const auto generic_bid = Scalar<>::from(typed_bid);
            const auto generic_ask = Scalar<>::from(typed_ask);
            auto rejected = false;
            try {
                static_cast<void>(output.add(RepeatedGenericQuote<Cell>{
                    .size = Cell<SCALAR>{std::nullopt}, .bid = Cell<SCALAR>{generic_bid},
                    .ask = Cell<SCALAR>{generic_ask}}));
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "repeated generic field variable matched different output field types");
            return;
        }
        case 20: {
            auto rejected = false;
            try {
                output.set(state->generic, RowPatch<ConstrainedGenericQuote>{.ask = PatchCell<double>{70.5}});
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "constrained generic field matched an incompatible output field type");
            return;
        }
        case 21: {
            const auto wrong_bid = i64{75};
            const auto typed_bid = Scalar<i64>::from(wrong_bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            auto rejected = false;
            try {
                output.set(state->generic, RowPatch<MixedGenericQuote>{
                    .size = PatchCell<i64>{100}, .bid = PatchCell<SCALAR>{generic_bid}});
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "generic patch accepted a wrong later cell after decoding an earlier valid cell");
            return;
        }
        case 22: {
            const auto bid = 71.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            state->inherited = output.add(InheritedGenericQuote<Cell>{
                InheritedGenericQuoteBase<Cell>{Cell<i64>{70}, Cell<double>{99.0}},
                Cell<SCALAR>{generic_bid}, Cell<double>{72.5}});
            return;
        }
        case 23: {
            const auto bid = 73.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            auto patch = InheritedGenericQuote<PatchCell>{
                InheritedGenericQuoteBase<PatchCell>{PatchCell<i64>{}, PatchCell<double>{98.5}},
                PatchCell<SCALAR>{generic_bid}, PatchCell<double>{}};
            output.set(state->inherited, patch);
            return;
        }
        default:
            throw std::runtime_error{"unknown runtime-schema TSF test command"};
        }
    }
};

struct tsf_switchable_projection {
    In<TSF<Quotes>> input;
    In<TS<i64>> selection;
    Out<TSF<BidOnly>, view> output;

    void evaluate() const {
        if (*selection == 0)
            output.bind_projection(input, std::array<std::size_t, 1>{1});
        else
            output.bind_projection(input, columns<"ask">());
    }
};

struct filter_writer_state {
    row_id_t first{no_row};
    row_id_t first_identity{no_row};
    row_id_t second{no_row};
};

struct filter_writer {
    In<TS<i64>> command;
    State<filter_writer_state> state;
    Out<TSF<Quotes>> output;

    void evaluate() const {
        switch (*command) {
        case 0:
            output.clear();
            return;
        case 1:
            state->first = output.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                                   .ask = Cell<double>{10.0}});
            state->first_identity = state->first;
            return;
        case 2:
            state->second = output.add(Quotes<Cell>{.size = Cell<i64>{8}, .bid = Cell<double>{20.5},
                                                    .ask = Cell<double>{20.0}});
            return;
        case 3:
            output.set(state->first, Quotes<PatchCell>{.bid = 11.5});
            return;
        case 4:
            output.set(state->first, Quotes<PatchCell>{.size = 3});
            return;
        case 5:
            output.set(state->first, Quotes<PatchCell>{.size = 8});
            return;
        case 6:
            require(output.erase(state->first), "filter writer could not remove its first row");
            return;
        case 7:
            state->first = output.add(Quotes<Cell>{.size = Cell<i64>{9}, .bid = Cell<double>{12.5},
                                                   .ask = Cell<double>{12.0}});
            require(state->first == state->first_identity, "filter writer did not reuse a retired row ID");
            return;
        case 8:
            output.invalidate();
            return;
        case 9:
            output.clear();
            return;
        case 10:
            output.set(state->second, Quotes<PatchCell>{.bid = 14.0});
            return;
        case 11:
            output.set(state->second, Quotes<PatchCell>{.bid = 22.0});
            return;
        default:
            throw std::runtime_error{"unknown filter writer command"};
        }
    }
};

struct frame_command_source {
    int slot;
    Out<TS<i64>> output;

    void evaluate() const { output.set(commands[slot]); }

    inline static std::array<i64, 1> commands{1};
};

struct frame_trigger_source {
    Out<TS<i64>> output;

    void evaluate() const { output.set(1); }
};

struct frame_runtime_capture {
    In<TSF<Quotes>> frame;
    In<TS<i64>> trigger;

    void evaluate() const {
        static_cast<void>(trigger);
        captured_bid = frame.value_view().value().columns().bid.cell(0).value_or(-1.0);
    }

    inline static double captured_bid{};
};

struct frame_view_runtime_capture {
    In<TSF<Quotes>> frame;
    In<TS<i64>> trigger;

    void evaluate() const {
        static_cast<void>(trigger);
        const auto value = frame.value_view().value();
        captured_size = value.size();
        captured_bid = value.columns().bid.cell(0).value_or(-1.0);
    }

    inline static std::size_t captured_size{};
    inline static double captured_bid{};
};

void test_compiled_graph_runtime_storage_is_independent() {
    using namespace bg;
    using namespace bg::frame;

    const auto command = node<frame_command_source>();
    const auto trigger = node<frame_trigger_source>();
    const auto write = node<tsf_writer>();
    const auto capture = node<frame_runtime_capture>();
    const auto capture_view = node<frame_view_runtime_capture>();
    GraphWiring wiring;
    auto command_identity = std::optional<NodeCallIdentity>{};
    auto trigger_identity = std::optional<NodeCallIdentity>{};
    {
        WiringContext context{wiring};
        const auto command_input = command(0);
        const auto trigger_input = trigger();
        command_identity = wiring.node_call_identity(static_cast<WiringPoint>(command_input));
        trigger_identity = wiring.node_call_identity(static_cast<WiringPoint>(trigger_input));
        const auto frame = write(command_input);
        capture(frame, trigger_input);
        capture_view(filter_greater_than<"size">(frame, 4), trigger_input);
    }

    wiring.finalize();
    const auto command_index = wiring.node_index(*command_identity);
    const auto trigger_index = wiring.node_index(*trigger_identity);
    const auto compiled = CompiledGraph{wiring};
    auto first_global_state = GlobalState{};
    auto second_global_state = GlobalState{};
    auto first = RuntimeGraph{compiled, first_global_state};
    auto second = RuntimeGraph{compiled, second_global_state};
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    first.start(first_time);
    second.start(first_time);
    first.schedule(command_index);
    first.schedule(trigger_index);
    second.schedule(command_index);
    second.schedule(trigger_index);
    first.evaluate(first_time);
    second.evaluate(first_time);
    require(frame_runtime_capture::captured_bid == 10.5, "runtime frame did not publish its initial row");
    require(frame_view_runtime_capture::captured_size == 1 && frame_view_runtime_capture::captured_bid == 10.5,
            "runtime filtered frame did not publish its initial row");

    frame_command_source::commands[0] = 2;
    const auto second_time = first_time + EvaluationTime::duration{1};
    first.schedule(command_index);
    first.evaluate(second_time);
    require(frame_runtime_capture::captured_bid == 11.5, "first runtime frame did not apply its row patch");
    require(frame_view_runtime_capture::captured_size == 1 && frame_view_runtime_capture::captured_bid == 11.5,
            "first runtime filtered view did not apply its row patch");

    const auto third_time = second_time + EvaluationTime::duration{1};
    second.schedule(trigger_index);
    second.evaluate(third_time);
    require(frame_runtime_capture::captured_bid == 10.5,
            "one runtime graph's frame mutation changed another runtime graph's frame");
    require(frame_view_runtime_capture::captured_size == 1 && frame_view_runtime_capture::captured_bid == 10.5,
            "one runtime graph's filtered view mutation changed another runtime graph's view");
}

void test_tsf_graph_behavior() {
    using namespace bg;
    using namespace bg::frame;

    auto observations = std::vector<frame_observation>{};
    auto column_observations = std::vector<column_observation>{};
    auto bid_consumer_observations = std::vector<column_observation>{};
    auto ask_consumer_observations = std::vector<column_observation>{};
    auto filtered_bid_consumer_observations = std::vector<column_observation>{};
    auto filtered_ask_consumer_observations = std::vector<column_observation>{};
    auto frame_consumer_observations = std::vector<frame_observation>{};
    const auto write = node<tsf_writer>();
    const auto observe = node([&](In<TSF<Quotes>, optional> input, In<TS<i64>> tick,
                                  Out<TS<Frame<Quotes>>> snapshot) {
        static_cast<void>(tick);
        auto result = frame_observation{.valid = input.valid(),
                                        .became_valid = input.became_valid(),
                                        .became_invalid = input.became_invalid(),
                                        .modified = input.modified(),
                                        .size = input.size()};
        input.for_each_row([&](row_id_t id, std::size_t) { result.rows.push_back(id); });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        input.for_each_modified<1>([&](row_id_t id) { result.bid_modified.push_back(id); });
        if (input.valid())
            snapshot.set_value(input.value_view());
        observations.push_back(std::move(result));
    });
    const auto observe_column = node([&](In<TSC<double>, optional> input, In<TS<i64>> tick) {
        static_cast<void>(tick);
        auto result = column_observation{.valid = input.valid(),
                                         .became_valid = input.became_valid(),
                                         .became_invalid = input.became_invalid(),
                                         .modified = input.modified(),
                                         .size = input.size()};
        input.for_each_row([&](row_id_t id, std::size_t) { result.rows.push_back(id); });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        auto native_values = std::vector<const double *>{};
        input.for_each_batch([&](const auto &batch) { native_values.push_back(batch.cells.values); });
        auto scalar_values = std::vector<const double *>{};
        if (input.valid())
            input.value_view().value().batches([&](const auto &batch) { scalar_values.push_back(batch.values); });
        result.zero_copy = native_values == scalar_values;
        column_observations.push_back(std::move(result));
    });
    const auto observe_child = [](auto &consumer_observations) {
        return node([&consumer_observations](In<TSC<double>, optional> input) {
            auto result = column_observation{.valid = input.valid(),
                                             .became_valid = input.became_valid(),
                                             .became_invalid = input.became_invalid(),
                                             .modified = input.modified(),
                                             .size = input.size()};
            input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
            input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
            consumer_observations.push_back(std::move(result));
        });
    };
    const auto observe_bid = observe_child(bid_consumer_observations);
    const auto observe_ask = observe_child(ask_consumer_observations);
    const auto observe_filtered_bid = observe_child(filtered_bid_consumer_observations);
    const auto observe_filtered_ask = observe_child(filtered_ask_consumer_observations);
    const auto observe_frame = node([&](In<TSF<Quotes>, optional> input) {
        const auto read_storage = input.extension_descriptor();
        require(read_storage.source_runtime == std::addressof(input.runtime_type()),
                "TSF shared read storage lost its runtime binding");
        if (input.valid()) {
            auto bid = ColumnView<double>{};
            read_storage.bind_column_view(util::symbol{"bid"}, bid);
            require(bid.size() == input.size(), "TSF shared read storage returned the wrong column size");
            if (input.size() != 0)
                require(bid.cell(0) == input.cell<1>(read_storage.rows.at(0)),
                        "TSF shared read storage bound the wrong native column");
        }
        auto result = frame_observation{.valid = input.valid(),
                                        .became_valid = input.became_valid(),
                                        .became_invalid = input.became_invalid(),
                                        .modified = input.modified(),
                                        .size = input.size()};
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        frame_consumer_observations.push_back(std::move(result));
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        observe_column(select_column<"bid">(frame), tick);
        observe_bid(select_column<"bid">(frame));
        observe_ask(select_column<"ask">(frame));
        const auto filtered = filter_greater_than<"size">(frame, 4);
        observe_filtered_bid(select_column<"bid">(filtered));
        observe_filtered_ask(select_column<"ask">(filtered));
        observe_frame(frame);
        return observe(frame, tick);
    };
    const auto commands = std::array<i64, 9>{0, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto ticks = std::array<i64, 9>{0, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto snapshots = testing::test_graph(graph, commands, ticks);

    require(observations.size() == commands.size(), "TSF observer did not run for every cycle");
    if (column_observations.size() != commands.size())
        throw std::runtime_error{"TSF column observer did not run for every cycle: " +
                                 std::to_string(column_observations.size()) + " of " +
                                 std::to_string(commands.size())};
    require(bid_consumer_observations.size() == 7 && ask_consumer_observations.size() == 7,
            "column consumers ran for an empty patch or missed a column publication");
    require(filtered_bid_consumer_observations.size() == 6 && filtered_ask_consumer_observations.size() == 6,
            "no-tick consumers missed filtered column membership or payload publications");
    require(filtered_bid_consumer_observations[1].changed == observations[1].rows &&
                filtered_bid_consumer_observations[2].changed == observations[1].rows &&
                filtered_ask_consumer_observations[1].changed == observations[1].rows &&
                filtered_ask_consumer_observations[2].changed == observations[1].rows &&
                filtered_bid_consumer_observations[3].removed == observations[1].rows &&
                filtered_ask_consumer_observations[3].removed == observations[1].rows &&
                filtered_bid_consumer_observations[4].became_invalid &&
                filtered_ask_consumer_observations[4].became_invalid,
            "filtered column consumers did not follow membership, payload, and validity changes");
    require(frame_consumer_observations.size() == 8,
            "frame-only consumer ran for an empty patch or missed a frame publication");
    for (const auto &entry : column_observations)
        require(entry.zero_copy, "TSF child and scalar column views did not share cell storage");
    require(observations[0].valid && observations[0].became_valid && observations[0].modified &&
                observations[0].size == 0 && observations[0].changed.empty(),
            "TSF clear did not publish a valid empty frame");
    require(observations[1].valid && observations[1].size == 1 && observations[1].rows.size() == 1 &&
                observations[1].changed == observations[1].rows && observations[1].bid_modified.empty(),
            "TSF row insertion did not publish row membership");
    require(observations[2].valid && observations[2].changed == observations[1].rows &&
                observations[2].bid_modified == observations[1].rows,
            "TSF column modification was not visible in the frame delta");
    require(observations[3].valid && observations[3].changed == observations[1].rows &&
                observations[3].bid_modified.empty(),
            "TSF modification tracking leaked across columns");
    require(observations[4].valid && !observations[4].modified && observations[4].size == 1 &&
                observations[4].changed.empty() && observations[4].removed.empty(),
            "TSF omitted patch published an unchanged frame");
    require(observations[5].valid && observations[5].size == 0 && observations[5].removed == observations[1].rows &&
                observations[5].changed.empty(),
            "TSF row removal did not publish its removal delta");
    require(!observations[6].valid && observations[6].became_invalid && observations[6].size == 0 &&
                observations[6].changed.empty() && observations[6].removed.empty(),
            "TSF invalidation exposed stale row deltas");
    require(observations[7].valid && observations[7].became_valid && observations[7].size == 0 &&
                observations[7].changed.empty(),
            "TSF empty revalidation did not publish a valid frame");
    require(observations[8].valid && observations[8].modified && observations[8].size == 0 &&
                observations[8].changed.empty() && observations[8].removed.empty(),
            "TSF add-then-erase did not publish an empty frame with canceled row deltas");

    require(column_observations[0].valid && column_observations[0].became_valid &&
                column_observations[0].modified && column_observations[0].size == 0,
            "TSF column did not become a valid empty series with its frame");
    require(column_observations[1].valid && column_observations[1].size == 1 &&
                column_observations[1].changed == observations[1].rows,
            "TSF child did not observe added row membership");
    require(column_observations[2].valid && column_observations[2].changed == observations[1].rows,
            "TSF child did not observe its changed cell");
    require(column_observations[3].valid && !column_observations[3].modified &&
                column_observations[3].changed.empty(),
            "TSF child notified after a different column changed");
    require(column_observations[4].valid && !column_observations[4].modified &&
                column_observations[4].changed.empty(),
            "TSF child notified for an omitted row patch");
    require(column_observations[5].valid && column_observations[5].removed == observations[1].rows,
            "TSF child did not observe removed row membership");
    require(!column_observations[6].valid && column_observations[6].became_invalid &&
                column_observations[6].changed.empty() && column_observations[6].removed.empty(),
            "TSF invalidation did not reach its wired child");
    require(column_observations[7].valid && column_observations[7].became_valid &&
                column_observations[7].size == 0,
            "TSF revalidation did not reach its wired child");
    require(column_observations[8].valid && column_observations[8].modified &&
                column_observations[8].changed.empty() && column_observations[8].removed.empty(),
            "TSF child did not receive add-then-erase publication");

    require(bid_consumer_observations[2].modified &&
                bid_consumer_observations[2].changed == observations[1].rows &&
                ask_consumer_observations[2].modified &&
                ask_consumer_observations[2].changed == observations[1].rows,
            "column consumers missed their respective bid and ask patches");
    require(bid_consumer_observations[3].removed == observations[1].rows &&
                ask_consumer_observations[3].removed == observations[1].rows,
            "column consumers did not resume on row removal after selective column updates");
    require(frame_consumer_observations[2].modified && frame_consumer_observations[3].modified,
            "frame-only consumer missed a column patch");

    require(snapshots.size() == commands.size() && snapshots[0] && snapshots[1] && snapshots[2] && snapshots[3] &&
                snapshots[4] && snapshots[5] && snapshots[7] && snapshots[8],
            "TSF did not export valid whole-frame scalar snapshots");
    require(snapshots[0]->value().size() == 0 && snapshots[1]->value().columns().bid.cell(0) == 10.5 &&
                snapshots[2]->value().columns().bid.cell(0) == 11.5 &&
                !snapshots[2]->value().columns().ask.cell(0) && snapshots[3]->value().columns().ask.cell(0) == 12.5 &&
                snapshots[4]->value().columns().bid.cell(0) == 11.5 && snapshots[5]->value().size() == 0 &&
                snapshots[7]->value().size() == 0 && snapshots[8]->value().size() == 0,
            "TSF scalar snapshots changed after later row mutations");
}

void test_borrowed_tsf_input_with_native_and_view_runtimes() {
    using namespace bg;
    using namespace bg::frame;

    const auto child_graph = make_borrowed_frame_graph();
    borrowed_frame_runner::compiled_graph = std::addressof(child_graph);
    const auto write = node<tsf_writer>();
    const auto run_borrowed = node<borrowed_frame_runner>();
    const auto commands = std::array<i64, 2>{1, 2};

    const auto run = [&](auto graph, const TimeSeriesRuntimeType &expected_runtime) {
        borrowed_frame_sink::observations.clear();
        borrowed_frame_sink::expected_bid_values = nullptr;
        borrowed_frame_sink::expected_data = nullptr;
        borrowed_frame_runner::child = nullptr;
        borrowed_frame_runner::evaluator = nullptr;

        const auto result = testing::test_graph(graph, commands);
        require(result.size() == commands.size() && borrowed_frame_sink::observations.size() == commands.size(),
                "borrowed TSF sink did not run for every graph cycle");
        const auto &first = borrowed_frame_sink::observations[0];
        const auto &second = borrowed_frame_sink::observations[1];
        require(first.runtime_type == std::addressof(expected_runtime) &&
                    second.runtime_type == std::addressof(expected_runtime),
                "borrowed TSF input changed its runtime descriptor");
        require(first.zero_copy_source && second.zero_copy_source,
                "borrowed TSF input changed its source storage pointer");
        require(first.zero_copy_column && second.zero_copy_column,
                "borrowed TSF input changed its column storage pointer");
        require(first.rows == std::vector<row_id_t>{0} && second.rows == std::vector<row_id_t>{0} &&
                    first.sizes == std::vector<std::optional<i64>>{5} &&
                    second.sizes == std::vector<std::optional<i64>>{5} &&
                    first.bids == std::vector<std::optional<double>>{10.5} &&
                    second.bids == std::vector<std::optional<double>>{11.5} &&
                    first.asks == std::vector<std::optional<double>>{std::nullopt} &&
                    second.asks == std::vector<std::optional<double>>{std::nullopt},
                "borrowed TSF input changed row or cell values");
        require(first.modified && second.modified && second.last_updated > first.last_updated,
                "borrowed TSF input lost its modification state or time");
        require(second.changed == std::vector<row_id_t>{0} &&
                    second.size_modified.empty() && second.bid_modified == std::vector<row_id_t>{0} &&
                    second.ask_modified.empty(),
                "borrowed TSF input reported incorrect row or column deltas");
    };

    const auto native_graph = [&](wp<TS<i64>> command) {
        const auto frame = write(command);
        run_borrowed(frame, frame);
        return command;
    };
    run(native_graph, frame_time_series_runtime_type<Quotes>(TSF<Quotes>::type()));

    const auto view_graph = [&](wp<TS<i64>> command) {
        const auto frame = write(command);
        const auto view = project<Quotes, "size", "bid", "ask">(frame);
        run_borrowed(view, frame);
        return command;
    };
    run(view_graph, frame_view_time_series_runtime_type<Quotes>(TSF<Quotes>::type()));
}

void test_generic_tsf_input_schema_and_columns() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        std::size_t field_count{};
        std::array<util::symbol, 3> field_names{};
        std::optional<i64> size;
        std::optional<double> bid;
    };

    auto observations = std::vector<observation>{};
    auto selected_bids = std::vector<double>{};
    const auto write = node<tsf_writer>();
    const auto observe_selected = node([&](In<TSC<SCALAR>> input) {
        const auto column = input.input<double>();
        column.for_each_row(
            [&](row_id_t id, std::size_t) { selected_bids.push_back(column.cell(id).value()); });
    });
    const auto observe = node([&](In<TSF<SCHEMA>, optional> input, Out<TS<i64>> output) {
        require(&input.input_data().runtime_type() == &input.runtime_type(),
                "TSF input did not expose its input data");
        auto result = observation{};
        const auto &schema = input.schema();
        result.field_count = schema.field_count();
        for (auto index = std::size_t{}; index < result.field_count; ++index)
            result.field_names[index] = schema.field(index).name;

        if (input.valid()) {
            require(input.input_data().valid() == input.valid(), "TSF input data did not preserve validity");
            const auto rejects_column_index = [&](std::size_t index) {
                try {
                    static_cast<void>(input.column(index));
                } catch (const std::out_of_range &) {
                    return true;
                }
                return false;
            };
            require(rejects_column_index(schema.field_count()),
                    "generic TSF input accepted an ordinal at the field count");
            const auto wrapped_index = static_cast<std::size_t>(std::numeric_limits<item_id_t>::max()) + 1;
            require(rejects_column_index(wrapped_index),
                    "generic TSF input narrowed an invalid ordinal to a valid child index");
            auto row = row_id_t{};
            input.for_each_row([&](row_id_t id, std::size_t) { row = id; });
            result.size = input.column(0).view<i64>().cell(row);
            result.bid = input.column("bid").view<double>().cell(row);
        }

        observations.push_back(std::move(result));
        output.set(1);
    });
    const auto graph = [&](wp<TS<i64>> command) {
        const auto source = wp<TSF<SCHEMA>>{write(command)};
        static_cast<void>(observe_selected(select_column<"bid">(source)));
        return observe(source);
    };
    const auto commands = std::array<i64, 1>{1};
    static_cast<void>(testing::test_graph(graph, commands));

    require(observations.size() == 1 && observations[0].field_count == 3 &&
                observations[0].field_names ==
                    std::array{util::symbol{"size"}, util::symbol{"bid"}, util::symbol{"ask"}} &&
                observations[0].size == 5 && observations[0].bid == 10.5 && selected_bids == std::vector<double>{10.5},
            "generic TSF input did not expose its resolved schema and scalar-variable columns");
}

void test_typed_patch_rows_insert_nullable_rows() {
    using namespace bg;
    using namespace bg::frame;

    using row_values = std::tuple<std::optional<i64>, std::optional<double>, std::optional<double>>;
    struct observation {
        std::vector<row_id_t> rows;
        std::vector<row_values> values;
    };
    auto observations = std::vector<observation>{};

    const auto write = node([](In<TS<i64>> command, Out<TSF<Quotes>> output) {
        if (*command == 0)
            static_cast<void>(output.add(RowPatch<Quotes>{.bid = 10.5}));
        else
            static_cast<void>(output.add(RowPatch<Quotes>{}));
    });
    const auto observe = node([&](In<TSF<Quotes>> input, In<TS<i64>> command, Out<TS<i64>> output) {
        auto result = observation{};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.values.emplace_back(input.cell<0>(id), input.cell<1>(id), input.cell<2>(id));
        });
        observations.push_back(std::move(result));
        output.set(*command);
    });
    const auto graph = [&](wp<TS<i64>> command) { return observe(write(command), command); };
    const auto commands = std::array<i64, 2>{0, 1};
    const auto result = testing::test_graph(graph, commands);

    require(result.size() == 2 && observations.size() == 2 && observations[0].rows == std::vector<row_id_t>{0} &&
                observations[0].values == std::vector<row_values>{{std::nullopt, 10.5, std::nullopt}} &&
                observations[1].rows == std::vector<row_id_t>{0, 1} &&
                observations[1].values ==
                    std::vector<row_values>{{std::nullopt, 10.5, std::nullopt},
                                            {std::nullopt, std::nullopt, std::nullopt}},
            "typed RowPatch insertion did not initialize omitted fields to null");
}

void test_runtime_schema_tsf_writes_and_named_projection() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        std::vector<row_id_t> rows;
        std::vector<std::optional<double>> prices;
    };
    auto observations = std::vector<observation>{};
    struct row_observation {
        std::vector<row_id_t> rows;
        std::vector<std::optional<i64>> sizes;
        std::vector<std::optional<double>> bids;
        std::vector<std::optional<double>> asks;
    };
    auto source_observations = std::vector<row_observation>{};

    const auto schema_fields = std::array{
        SchemaField{util::symbol{"size"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"bid"}, &cpp_scalar_type<double>()},
        SchemaField{util::symbol{"ask"}, &cpp_scalar_type<double>()},
    };
    const auto &schema = anonymous_schema(schema_fields);
    auto empty_size = Column<i64>{std::vector<std::optional<i64>>{}};
    auto empty_bid = Column<double>{std::vector<std::optional<double>>{}};
    auto empty_ask = Column<double>{std::vector<std::optional<double>>{}};
    const auto seed_columns = std::array{ColumnView<>{empty_size.view()}, ColumnView<>{empty_bid.view()},
                                         ColumnView<>{empty_ask.view()}};
    auto memory = allocator{};
    auto seed = make_frame_value(memory, schema, 0, seed_columns);
    auto erased_seed = ScalarValue<>{seed, memory};

    auto seed_ticks = std::vector<std::optional<ScalarValue<>>>{};
    auto command_ticks = std::vector<std::optional<ScalarValue<>>>{};
    const auto commands = std::array<i64, 24>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                                               12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
    for (const auto command : commands) {
        seed_ticks.emplace_back(erased_seed.view());
        command_ticks.emplace_back(ScalarValue<i64>{command});
    }
    const auto inputs = std::array{
        testing::TestGraphInput{erased_seed.runtime_type(), seed_ticks},
        testing::TestGraphInput{cpp_scalar_runtime_type<i64>(), command_ticks},
    };

    const auto write = node<runtime_schema_tsf_writer>();
    const auto pass_through = node([&](In<TSF<SCHEMA>, optional> input, In<TS<i64>> command,
                                       Out<TSF<SCHEMA>, view> output) {
        static_cast<void>(command);
        auto result = row_observation{};
        if (input.valid()) {
            const auto sizes = input.column("size").view<i64>();
            const auto bids = input.column("bid").view<double>();
            const auto asks = input.column("ask").view<double>();
            input.for_each_row([&](row_id_t id, std::size_t) {
                result.rows.push_back(id);
                result.sizes.push_back(sizes.cell(id));
                result.bids.push_back(bids.cell(id));
                result.asks.push_back(asks.cell(id));
            });
        }
        source_observations.push_back(std::move(result));
        const auto source_names = std::array{util::symbol{"size"}, util::symbol{"bid"}, util::symbol{"ask"}};
        static_cast<void>(output.bind_projection(input, std::span<const util::symbol>{source_names}));
    });
    const auto project = node([](In<TSF<SCHEMA>, optional> input, Out<TSF<BidOnly>, view> output) {
        const auto source_names = std::array{util::symbol{"bid"}};
        static_cast<void>(output.bind_projection(input, std::span<const util::symbol>{source_names}));
    });
    const auto observe = node([&](In<TSF<BidOnly>, optional> input, In<TS<i64>> command,
                                  Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid()};
        if (input.valid()) {
            input.for_each_row([&](row_id_t id, std::size_t) {
                result.rows.push_back(id);
                result.prices.push_back(input.cell<0>(id));
            });
        }
        observations.push_back(std::move(result));
        output.set(*command);
    });
    using namespace bg::literals;
    const auto graph = bg::graph("name"_n = "runtime_schema_tsf_writer", [&](wp<TS<Frame<SCHEMA>>> seed_input,
                                                                                wp<TS<i64>> command) {
        const auto frame = write(seed_input, command);
        return observe(project(pass_through(frame, command)), command);
    });
    const auto result = testing::test_graph(graph, std::span<const testing::TestGraphInput>{inputs});

    require(result.size() == commands.size() && observations.size() == commands.size(),
            "runtime-schema TSF writer graph missed a command cycle");
    require(!observations[0].valid && observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].prices == std::vector<std::optional<double>>{10.5} &&
                observations[2].rows == std::vector<row_id_t>{0} &&
                observations[2].prices == std::vector<std::optional<double>>{11.5},
            "runtime-schema TSF writer did not add or patch scalar row arguments");
    require(observations[3].rows == std::vector<row_id_t>{0, 1} &&
                observations[3].prices == std::vector<std::optional<double>>{11.5, 20.5} &&
                observations[4].prices == std::vector<std::optional<double>>{11.5, std::nullopt},
            "runtime-schema TSF writer did not adapt concrete rows or preserve unselected columns");
    require(observations[5].rows == std::vector<row_id_t>{0, 1} &&
                observations[5].prices == std::vector<std::optional<double>>{11.5, std::nullopt} &&
                observations[6].prices == std::vector<std::optional<double>>{11.5, std::nullopt} &&
                observations[7].prices == std::vector<std::optional<double>>{std::nullopt, std::nullopt} &&
                observations[8].rows == std::vector<row_id_t>{0, 1} &&
                observations[8].prices == std::vector<std::optional<double>>{std::nullopt, std::nullopt} &&
                observations[9].rows == std::vector<row_id_t>{0, 1, 2} &&
                observations[9].prices == std::vector<std::optional<double>>{std::nullopt, std::nullopt, 30.5} &&
                observations[10].rows == std::vector<row_id_t>{0, 1, 2, 3} &&
                observations[10].prices ==
                    std::vector<std::optional<double>>{std::nullopt, std::nullopt, 30.5, std::nullopt},
            "runtime-schema TSF validation or empty patch changed projected rows");
    require(source_observations.size() == commands.size() &&
                source_observations[11].rows == std::vector<row_id_t>{0, 1, 2, 3, 4} &&
                source_observations[11].sizes[4] == 9 && source_observations[11].bids[4] == 40.5 &&
                source_observations[11].asks[4] == 41.5 && source_observations[12].bids[4] == 42.5 &&
                !source_observations[13].bids[4] && source_observations[14].bids[5] == 50.5 &&
                !source_observations[14].sizes[5] && !source_observations[14].asks[5] &&
                source_observations[15].rows.size() == 7 && !source_observations[15].bids[6] &&
                source_observations[16].rows.size() == 7 && source_observations[17].sizes[4] == 9 &&
                source_observations[17].bids[4] == std::nullopt && source_observations[18].rows.size() == 7 &&
                source_observations[19].rows.size() == 7 && source_observations[20].rows.size() == 7 &&
                source_observations[21].sizes[4] == 9 && source_observations[21].bids[4] == std::nullopt &&
                source_observations[22].rows.size() == 8 && source_observations[22].sizes[7] == 70 &&
                source_observations[22].bids[7] == 71.5 && source_observations[22].asks[7] == 72.5 &&
                source_observations[23].bids[7] == 73.5,
            "generic typed row/patch adaptation changed cells, accepted invalid mappings, or exposed a shadowed field");
}

void test_generic_declared_tsf_rows_follow_resolved_cell_type() {
    using namespace bg;
    using namespace bg::frame;
    using observed_cell = std::variant<std::optional<i64>, std::optional<double>>;

    auto observations = std::vector<std::vector<observed_cell>>{};
    const auto write = node([](In<TS<Frame<GenericQuote>>> seed, In<TS<i64>> command,
                               Out<TSF<GenericQuote>> output) {
        static_cast<void>(seed);
        const auto &cell_type = *output.schema().field(0).type;
        const auto with_bid = [&](auto value, auto &&operation) {
            using Value = std::remove_cvref_t<decltype(value)>;
            const auto typed = Scalar<Value>::from(value);
            const auto borrowed = Scalar<>::from(typed);
            std::invoke(std::forward<decltype(operation)>(operation), borrowed);
            value = Value{};
        };

        switch (*command) {
        case 0:
            if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<i64>())) {
                with_bid(i64{12}, [&](Scalar<> value) {
                    static_cast<void>(output.add(Row<GenericQuote>{.bid = Cell<SCALAR>{value}}));
                });
            } else {
                with_bid(12.5, [&](Scalar<> value) {
                    static_cast<void>(output.add(Row<GenericQuote>{.bid = Cell<SCALAR>{value}}));
                });
            }
            return;
        case 1:
            if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<i64>())) {
                with_bid(i64{14}, [&](Scalar<> value) {
                    output.set(0, RowPatch<GenericQuote>{.bid = PatchCell<SCALAR>{value}});
                });
            } else {
                with_bid(14.5, [&](Scalar<> value) {
                    output.set(0, RowPatch<GenericQuote>{.bid = PatchCell<SCALAR>{value}});
                });
            }
            return;
        case 2:
            output.set(0, RowPatch<GenericQuote>{.bid = std::nullopt});
            return;
        case 3:
            static_cast<void>(output.add(RowPatch<GenericQuote>{}));
            return;
        default:
            output.set(0, RowPatch<GenericQuote>{});
            return;
        }
    });
    const auto observe = node([&](In<TSF<GenericQuote>, optional> input, In<TS<i64>> command,
                                  Out<TS<i64>> output) {
        static_cast<void>(command);
        auto result = std::vector<observed_cell>{};
        if (input.valid()) {
            const auto column = input.column("bid");
            input.for_each_row([&](row_id_t id, std::size_t) {
                if (std::addressof(column.column_view().cell_type()) == std::addressof(cpp_scalar_type<i64>()))
                    result.emplace_back(column.view<i64>().cell(id));
                else
                    result.emplace_back(column.view<double>().cell(id));
            });
        }
        observations.push_back(std::move(result));
        output.set(1);
    });

    using namespace bg::literals;
    const auto graph = bg::graph("name"_n = "generic_declared_tsf_rows", [&](wp<TS<Frame<SCHEMA>>> seed,
                                                                                   wp<TS<i64>> command) {
        return observe(write(seed, command), command);
    });
    const auto commands = std::array<i64, 5>{0, 1, 2, 3, 4};

    for (const auto *cell_type : {&cpp_scalar_type<i64>(), &cpp_scalar_type<double>()}) {
        auto type_map = TypeMap{};
        type_map.set(bg::details::scalar_variable_instance<SCALAR>(), cell_type);
        const auto &resolved_schema = dynamic_cast<const bg::Schema &>(frame_schema_type<GenericQuote>().resolve(type_map));
        auto memory = allocator{};
        auto seed_value = std::optional<ScalarValue<Frame<SCHEMA>>>{};
        if (cell_type == std::addressof(cpp_scalar_type<i64>())) {
            auto empty = Column<i64>{std::vector<std::optional<i64>>{}};
            const auto columns = std::array{ColumnView<>{empty.view()}};
            seed_value.emplace(make_frame_value(memory, resolved_schema, 0, columns));
        } else {
            auto empty = Column<double>{std::vector<std::optional<double>>{}};
            const auto columns = std::array{ColumnView<>{empty.view()}};
            seed_value.emplace(make_frame_value(memory, resolved_schema, 0, columns));
        }
        auto erased_seed = ScalarValue<>{*seed_value, memory};
        auto seed_ticks = std::vector<std::optional<ScalarValue<>>>{};
        auto command_ticks = std::vector<std::optional<ScalarValue<>>>{};
        for (const auto command : commands) {
            seed_ticks.emplace_back(erased_seed.view());
            command_ticks.emplace_back(ScalarValue<i64>{command});
        }
        const auto inputs = std::array{
            testing::TestGraphInput{erased_seed.runtime_type(), seed_ticks},
            testing::TestGraphInput{cpp_scalar_runtime_type<i64>(), command_ticks},
        };
        const auto result = testing::test_graph(graph, std::span<const testing::TestGraphInput>{inputs});
        require(result.size() == commands.size(), "generic declared TSF graph missed a command cycle");
    }

    require(observations.size() == 2 * commands.size(), "generic declared TSF did not run for both cell types");
    for (auto offset = std::size_t{}; offset != observations.size(); offset += commands.size()) {
        require(observations[offset].size() == 1 && observations[offset + 1].size() == 1 &&
                    observations[offset + 2].size() == 1 && observations[offset + 3].size() == 2 &&
                    observations[offset + 4].size() == 2,
                "generic declared TSF changed its first row shape across updates");
        const auto integer = std::holds_alternative<std::optional<i64>>(observations[offset][0]);
        if (integer) {
            require(std::get<std::optional<i64>>(observations[offset][0]) == 12 &&
                        std::get<std::optional<i64>>(observations[offset + 1][0]) == 14 &&
                        !std::get<std::optional<i64>>(observations[offset + 2][0]) &&
                        observations[offset + 3].size() == 2 && !std::get<std::optional<i64>>(observations[offset + 3][1]) &&
                        observations[offset + 4] == observations[offset + 3],
                    "generic declared TSF did not store borrowed i64 cells independently");
        } else {
            require(std::get<std::optional<double>>(observations[offset][0]) == 12.5 &&
                        std::get<std::optional<double>>(observations[offset + 1][0]) == 14.5 &&
                        !std::get<std::optional<double>>(observations[offset + 2][0]) &&
                        observations[offset + 3].size() == 2 &&
                        !std::get<std::optional<double>>(observations[offset + 3][1]) &&
                        observations[offset + 4] == observations[offset + 3],
                    "generic declared TSF did not store borrowed double cells independently");
        }
    }
}

void test_repeated_and_constrained_generic_tsf_fields_match_runtime_schema() {
    using namespace bg;
    using namespace bg::frame;

    const auto schema_fields = std::array{
        SchemaField{util::symbol{"size"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"bid"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"ask"}, &cpp_scalar_type<i64>()},
    };
    const auto &schema = anonymous_schema(schema_fields);
    auto empty_size = Column<i64>{std::vector<std::optional<i64>>{}};
    auto empty_bid = Column<i64>{std::vector<std::optional<i64>>{}};
    auto empty_ask = Column<i64>{std::vector<std::optional<i64>>{}};
    const auto seed_columns = std::array{ColumnView<>{empty_size.view()}, ColumnView<>{empty_bid.view()},
                                         ColumnView<>{empty_ask.view()}};
    auto memory = allocator{};
    auto seed = make_frame_value(memory, schema, 0, seed_columns);
    auto erased_seed = ScalarValue<>{seed, memory};

    const auto write = node([](In<TS<Frame<SCHEMA>>> seed_input, Out<TSF<SCHEMA>> output) {
        static_cast<void>(seed_input);
        const auto size = i64{3};
        const auto bid = i64{5};
        const auto ask = i64{7};
        const auto typed_size = Scalar<i64>::from(size);
        const auto typed_bid = Scalar<i64>::from(bid);
        const auto typed_ask = Scalar<i64>::from(ask);
        const auto generic_size = Scalar<>::from(typed_size);
        const auto generic_bid = Scalar<>::from(typed_bid);
        const auto generic_ask = Scalar<>::from(typed_ask);
        static_cast<void>(output.add(Row<RepeatedConstrainedGenericQuote>{
            .size = Cell<SCALAR>{generic_size}, .bid = Cell<SCALAR>{generic_bid},
            .ask = Cell<I64Cell>{generic_ask}}));
    });
    const auto observe = node([](In<TSF<SCHEMA>> input, Out<TS<i64>> output) {
        const auto size = input.column("size").view<i64>().cell(0);
        const auto bid = input.column("bid").view<i64>().cell(0);
        const auto ask = input.column("ask").view<i64>().cell(0);
        require(size == 3 && bid == 5 && ask == 7,
                "repeated and constrained generic fields did not match compatible runtime columns");
        output.set(1);
    });

    using namespace bg::literals;
    const auto graph = bg::graph("name"_n = "repeated_constrained_generic_tsf_fields", [&](wp<TS<Frame<SCHEMA>>> seed_input) {
        return observe(write(seed_input));
    });
    const auto seed_ticks = std::array{std::optional<ScalarValue<>>{erased_seed.view()}};
    const auto inputs = std::array{testing::TestGraphInput{erased_seed.runtime_type(), seed_ticks}};
    const auto result = testing::test_graph(graph, std::span<const testing::TestGraphInput>{inputs});
    require(result.size() == 1, "repeated and constrained generic TSF writer missed its cycle");
}

void test_inherited_tsf_graph_behavior() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        bool modified{};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
        std::vector<std::optional<i64>> sizes;
        std::vector<std::optional<double>> bids;
        std::vector<std::optional<double>> asks;
        std::vector<std::optional<double>> base_bids;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<inherited_tsf_writer>();
    const auto observe = node([&](In<TSF<InheritedFrame>, optional> input,
                                  In<TSF<InheritedFrameBase>, optional> base,
                                  In<TS<i64>> tick, Out<TS<Frame<InheritedFrame>>> snapshot) {
        static_cast<void>(tick);
        auto result = observation{.valid = input.valid(), .modified = input.modified()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.sizes.push_back(input.cell<0>(id));
            result.bids.push_back(input.cell<1>(id));
            result.asks.push_back(input.cell<2>(id));
            result.base_bids.push_back(base.cell<1>(id));
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        if (input.valid())
            snapshot.set_value(input.value_view());
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        return observe(frame, frame, tick);
    };
    const auto commands = std::array<i64, 4>{0, 1, 2, 3};
    const auto ticks = std::array<i64, 4>{0, 1, 2, 3};
    const auto snapshots = testing::test_graph(graph, commands, ticks);

    require(observations.size() == commands.size() && observations[0].valid && observations[0].rows.empty() &&
                observations[1].rows == std::vector<row_id_t>{0} && observations[1].sizes[0] == 5 &&
                observations[1].bids[0] == 12.5 && !observations[1].asks[0] &&
                observations[1].base_bids[0] == observations[1].bids[0],
            "inherited TSF input did not read base and direct logical fields");
    require(observations[2].changed == std::vector<row_id_t>{0} && !observations[2].bids[0] &&
                observations[2].asks[0] == 15.5 && observations[2].base_bids[0] == observations[2].bids[0],
            "inherited TSF patch did not publish logical null and direct field updates");
    require(observations[3].rows.empty() && observations[3].removed == std::vector<row_id_t>{0},
            "inherited TSF removal did not reach typed derived and base inputs");
    require(snapshots.size() == commands.size() && snapshots[1] && snapshots[2] &&
                snapshots[1]->value().columns().bid.cell(0) == 12.5 &&
                !snapshots[2]->value().columns().bid.cell(0) &&
                snapshots[2]->value().columns().ask.cell(0) == 15.5,
            "inherited TSF scalar copies did not preserve logical field values");
}

void test_base_frame_output_retains_derived_schema() {
    using namespace bg;
    using namespace bg::frame;

    const auto write = node<inherited_tsf_writer>();
    const auto borrow_derived = [](In<TSF<InheritedFrame>, optional> input) { return input.value_view(); };
    const auto capture = node([&](In<TSF<InheritedFrame>, optional> input, In<TS<i64>> tick,
                                  Out<TS<Frame<InheritedFrameBase>>> output) {
        static_cast<void>(tick);
        if (input.valid()) {
            const auto borrowed = borrow_derived(input);
            output.set_value(Scalar<Frame<InheritedFrameBase>>::from(borrowed));
        }
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) { return capture(write(command), tick); };
    const auto commands = std::array<i64, 4>{0, 1, 2, 3};
    const auto ticks = std::array<i64, 4>{0, 1, 2, 3};
    const auto snapshots = testing::test_graph(graph, commands, ticks);

    require(snapshots.size() == commands.size() && snapshots[1] && snapshots[2],
            "base frame output did not publish owning snapshots");
    const auto first = snapshots[1]->value();
    const auto patched = snapshots[2]->value();
    require(first.schema().field_count() == 3 && first.columns().size.cell(0) == 5 &&
                first.columns().bid.cell(0) == 12.5 && !first.column("ask").view<double>().cell(0),
            "base frame output lost the actual derived schema or initial values");
    require(patched.schema().field_count() == 3 && patched.columns().size.cell(0) == 5 &&
                !patched.columns().bid.cell(0) && patched.column("ask").view<double>().cell(0) == 15.5,
            "base frame output lost derived fields during a patch");
    require(!first.column("ask").view<double>().cell(0),
            "base frame snapshot changed after later source mutation and graph teardown");
}

void test_tsf_projection_view() {
    using namespace bg;
    using namespace bg::frame;

    struct projection_observation {
        bool valid{};
        bool became_valid{};
        bool became_invalid{};
        bool modified{};
        bool zero_copy{};
        std::size_t size{};
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
        std::optional<double> price;
    };

    auto observations = std::vector<projection_observation>{};
    const auto write = node<tsf_writer>();
    const auto observe = node([&](In<TSF<Quotes>, optional> source, In<TSF<BidOnly>, optional> input, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto result = projection_observation{.valid = input.valid(),
                                             .became_valid = input.became_valid(),
                                             .became_invalid = input.became_invalid(),
                                             .modified = input.modified(),
                                             .size = input.size()};
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        if (input.valid() && input.size() != 0) {
            input.for_each_row([&](row_id_t id, std::size_t) { result.price = input.cell<0>(id); });
            auto source_values = std::vector<const double *>{};
            source.value_view().value().columns().bid.batches(
                [&](const auto &batch) { source_values.push_back(batch.values); });
            auto projected_values = std::vector<const double *>{};
            input.value_view().value().columns().price.batches(
                [&](const auto &batch) { projected_values.push_back(batch.values); });
            result.zero_copy = !source_values.empty() && source_values == projected_values;
        }
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command) {
        const auto source = write(command);
        const auto projected = project<BidOnly, "bid">(source);
        return observe(source, projected, command);
    };
    const auto commands = std::array<i64, 8>{0, 1, 2, 3, 4, 5, 6, 7};
    static_cast<void>(testing::test_graph(graph, commands));

    require(observations.size() == commands.size(),
            ("TSF projection observer missed a cycle: " + std::to_string(observations.size()) + " of " +
             std::to_string(commands.size())).c_str());
    require(observations[0].valid && observations[0].became_valid && observations[0].modified &&
                observations[0].size == 0,
            "TSF projection did not publish its first empty binding");
    require(observations[1].valid && observations[1].size == 1 && observations[1].changed.size() == 1 &&
                observations[1].zero_copy && observations[1].price == 10.5,
            "TSF projection did not expose the added source row zero-copy");
    require(observations[2].modified && observations[2].changed == observations[1].changed &&
                observations[2].zero_copy && observations[2].price == 11.5,
            "TSF projection did not propagate a mapped column modification");
    require(!observations[3].modified && observations[3].changed.empty() && observations[3].price == 11.5,
            "TSF projection published a source column that was not selected");
    require(!observations[4].modified && observations[4].changed.empty(),
            "TSF projection published an empty source patch");
    require(observations[5].modified && observations[5].size == 0 &&
                observations[5].removed == observations[1].changed,
            "TSF projection did not normalize row removal");
    require(!observations[6].valid && observations[6].became_invalid,
            "TSF projection did not propagate invalidation");
    require(observations[7].valid && observations[7].became_valid && observations[7].modified &&
                observations[7].size == 0,
            "TSF projection did not rebuild after revalidation");
}

void test_tsf_projection_empty_columns_match_target_schema() {
    using namespace bg;
    using namespace bg::frame;

    const auto reject_projection = node([](In<TSF<Quotes>> input, Out<TSF<BidOnly>, view> output) {
        output.bind_projection(input, columns<>());
    });
    const auto observe_bid_only = node([](In<TSF<BidOnly>, optional> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto write_quotes = node<tsf_writer>();
    const auto rejected_graph = [&](wp<TS<i64>> command) {
        return observe_bid_only(reject_projection(write_quotes(command)));
    };

    auto rejected = false;
    try {
        static_cast<void>(testing::test_graph(rejected_graph, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "empty TSF projection accepted a target schema with fields");

    const auto write_empty = node([](In<TS<i64>> command, Out<TSF<EmptySchema>> output) {
        if (*command == 1)
            static_cast<void>(output.add(EmptySchema<Cell>{}));
    });
    const auto project_empty = node([](In<TSF<EmptySchema>> input, Out<TSF<EmptySchema>, view> output) {
        output.bind_projection(input, columns<>());
    });
    const auto observe_empty = node([](In<TSF<EmptySchema>, optional> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto empty_graph = [&](wp<TS<i64>> command) {
        return observe_empty(project_empty(write_empty(command)));
    };
    const auto result = testing::test_graph(empty_graph, std::array<i64, 1>{1});
    require(result.equals<i64>({1}),
            "empty TSF projection did not preserve rows for a zero-field target schema");
}

void test_tsf_projection_selection_change() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        bool modified{};
        std::optional<double> price;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<tsf_writer>();
    const auto project = node<tsf_switchable_projection>();
    const auto observe = node([&](In<TSF<BidOnly>, optional> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(), .modified = input.modified()};
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        if (input.valid() && input.size() != 0)
            input.for_each_row([&](row_id_t id, std::size_t) { result.price = input.cell<0>(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> selection, wp<TS<i64>> tick) {
        const auto source = write(command);
        return observe(project(source, selection), tick);
    };
    const auto commands = std::array<i64, 8>{7, 1, 4, 3, 4, 2, 4, 6};
    const auto selections = std::array<i64, 8>{0, 0, 0, 0, 1, 1, 0, 0};
    const auto ticks = std::array<i64, 8>{0, 1, 2, 3, 4, 5, 6, 7};
    static_cast<void>(testing::test_graph(graph, commands, selections, ticks));

    require(observations.size() == commands.size(), "switchable TSF projection missed a cycle");
    require(observations[0].valid && observations[0].modified && observations[0].changed.empty(),
            "switchable TSF projection did not bind an empty source");
    require(observations[1].valid && observations[1].changed.size() == 1 && observations[1].price == 10.5,
            "switchable TSF projection did not bind its first row");
    require(!observations[2].modified,
            "switchable TSF projection published an unchanged source");
    require(!observations[3].modified,
            "switchable TSF projection published a change to the unselected column");
    require(observations[4].modified && observations[4].changed == observations[1].changed &&
                observations[4].removed.empty() && observations[4].price == 12.5,
            "switchable TSF projection did not publish a same-row selection change as a modification");
    require(!observations[5].modified,
            "switchable TSF projection published a change to the previously selected column");
    require(observations[6].modified && observations[6].changed == observations[1].changed &&
                observations[6].price == 11.5,
            "switchable TSF projection did not publish a later selection change");
    require(!observations[7].valid, "switchable TSF projection did not propagate invalidation");
}

void test_tsf_projection_rejects_invalid_mappings_before_invalidation() {
    using namespace bg;
    using namespace bg::frame;

    auto projection_errors = std::array<bool, 2>{};
    auto observations = std::vector<bool>{};
    const auto write = node<tsf_writer>();
    const auto project = node([&](In<TSF<Quotes>, optional> input, In<TS<i64>> selection,
                                  Out<TSF<BidOnly>, view> output) {
        if (*selection == 0) {
            output.bind_projection(input, std::array<std::size_t, 1>{1});
            return;
        }

        const auto source_index = *selection == 1 ? std::size_t{99} : std::size_t{0};
        auto rejected = false;
        try {
            output.bind_projection(input, std::array{source_index});
        } catch (const std::out_of_range &) {
            rejected = *selection == 1;
        } catch (const std::invalid_argument &) {
            rejected = *selection == 2;
        }
        require(rejected, "invalid TSF projection mapping was not rejected");
        projection_errors[static_cast<std::size_t>(*selection - 1)] = rejected;
    });
    const auto observe = node([&](In<TSF<BidOnly>, optional> input, In<TS<i64>> selection, Out<TS<i64>> output) {
        observations.push_back(input.valid());
        output.set(static_cast<i64>(input.valid()));
        static_cast<void>(selection);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> selection) {
        return observe(project(write(command), selection), selection);
    };
    const auto commands = std::array<i64, 3>{1, 6, 6};
    const auto selections = std::array<i64, 3>{0, 1, 2};
    const auto result = testing::test_graph(graph, commands, selections);

    require(projection_errors[0] && projection_errors[1] && observations == std::vector<bool>{true, true, true} &&
                result.equals<i64>({1, 1, 1}),
            "invalid ordinal or cell type mapping invalidated a previously valid TSF projection");
}

void test_tsf_projection_rejects_empty_row_domain_mismatch() {
    using namespace bg;
    using namespace bg::frame;

    auto memory = allocator{};
    const auto time = MIN_DT + EvaluationTime::duration{1};
    auto first = NativeFrame<Quotes>{};
    auto second = NativeFrame<Quotes>{};
    first.clear(time, memory);
    second.clear(time, memory);

    auto mismatched = native_frame_input(second);
    FrameReadBindings bindings{};
    bindings.apply(mismatched);
    bindings.source = native_frame_input(first);
    const auto bid_field = std::array{mismatched.field_index(util::symbol{"bid"})};
    auto rejected = false;
    with_runtime_view_storage<BidOnly>(memory, [&](auto &output, const auto &) {
        try {
            static_cast<void>(output.bind(mismatched, bid_field, std::nullopt, nullptr, nullptr, time, memory));
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
    });
    first.release(memory);
    second.release(memory);
    require(rejected, "empty TSF projection accepted a mismatched physical row domain");
}

void test_view_updates_do_not_traverse_source_rows() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto source = NativeFrame<Quotes>{};
    auto source_release = util::scope_exit{[&]() noexcept { source.release(memory); }};

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto id = source.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                            .ask = Cell<double>{11.0}},
                               first_time, memory);
    const auto row_counter = RowTraversalCounter{std::addressof(source.rows())};
    auto bindings = FrameReadBindings{};
    const auto make_input = [&] {
        auto input = native_frame_input(source);
        const auto rows = row_counter.row_set();
        input.rows = rows;
        bindings.visible_rows = rows;
        bindings.override_visible_rows = true;
        bindings.apply(input);
        return input;
    };

    auto input = make_input();
    const auto bid_field = std::array{input.field_index(util::symbol{"bid"})};
    const auto all_fields = std::array<std::size_t, 3>{0, 1, 2};
    const auto predicate_index = input.field_index(util::symbol{"size"});
    auto predicate = [&](row_id_t row) { return source.cell<0>(row).value_or(0) > 4; };
    const auto [predicate_context, predicate_function] = runtime_predicate(predicate);

    with_runtime_view_storage<BidOnly>(memory, [&](auto &projection, const auto &) {
        with_runtime_view_storage<Quotes>(memory, [&](auto &filter, const auto &) {
            static_cast<void>(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, first_time, memory));
            static_cast<void>(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function,
                                          first_time, memory));

            auto size_patch = Quotes<PatchCell>{};
            size_patch.size = 6;
            const auto second_time = first_time + EvaluationTime::duration{1};
            source.set(id, size_patch, second_time, memory);
            input = make_input();
            row_counter.traversals = 0;
            static_cast<void>(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function,
                                          second_time, memory));
            require(row_counter.traversals == 0,
                    "steady-state predicate-column update traversed all source rows");

            auto bid_patch = Quotes<PatchCell>{};
            bid_patch.bid = 12.5;
            const auto third_time = second_time + EvaluationTime::duration{1};
            source.set(id, bid_patch, third_time, memory);
            input = make_input();
            row_counter.traversals = 0;
            static_cast<void>(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, third_time, memory));
            static_cast<void>(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function,
                                          third_time, memory));
            require(row_counter.traversals == 0,
                    "steady-state projection and payload updates traversed all source rows");
        });
    });
}

void test_filtered_column_coverage_rejects_a_different_row_set() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto source = NativeFrame<Quotes>{};
    auto source_release = util::scope_exit{[&]() noexcept { source.release(memory); }};

    const auto time = MIN_DT + EvaluationTime::duration{1};
    static_cast<void>(source.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                              .ask = Cell<double>{11.0}},
                                time, memory));
    static_cast<void>(source.add(Quotes<Cell>{.size = Cell<i64>{8}, .bid = Cell<double>{20.5},
                                              .ask = Cell<double>{21.0}},
                                time, memory));
    const auto input = native_frame_input(source);
    const auto all_fields = std::array<std::size_t, 3>{0, 1, 2};
    const auto bid_index = input.field_index(util::symbol{"bid"});
    const auto bid_field = std::array{bid_index};
    const auto predicate_index = input.field_index(util::symbol{"size"});
    auto predicate = [&](row_id_t id) { return source.cell<0>(id).value_or(0) > 6; };
    const auto [predicate_context, predicate_function] = runtime_predicate(predicate);
    with_runtime_view_storage<Quotes>(memory, [&](auto &filter, const auto &filter_runtime) {
        with_runtime_view_storage<BidOnly>(memory, [&](auto &projection, const auto &projection_runtime) {
            static_cast<void>(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function,
                                          time, memory));

            auto mismatched = input;
            auto bindings = FrameReadBindings{};
            bindings.source = input;
            bindings.overridden_index = bid_index;
            bindings.overridden_binding = filter_runtime.input_storage(std::addressof(filter)).column(bid_index);
            bindings.apply(mismatched);
            auto rejected = false;
            try {
                static_cast<void>(projection.bind(mismatched, bid_field, std::nullopt, nullptr, nullptr, time,
                                                  memory));
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "a filtered column accepted a frame row set outside its visible coverage");

            auto operation_switch_rejected = false;
            try {
                static_cast<void>(filter.bind(input, all_fields, std::nullopt, nullptr, nullptr,
                                              time + EvaluationTime::duration{1}, memory));
            } catch (const std::logic_error &) {
                operation_switch_rejected = true;
            }
            require(operation_switch_rejected, "a TSF view output silently switched from filtering to projection");

            static_cast<void>(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, time, memory));
            auto projected = projection_runtime.input_storage(std::addressof(projection));
            auto always_true = [](row_id_t) { return true; };
            const auto [always_true_context, always_true_function] = runtime_predicate(always_true);
            operation_switch_rejected = false;
            try {
                static_cast<void>(projection.bind(projected, std::array<std::size_t, 1>{0}, 0,
                                                  always_true_context, always_true_function,
                                                  time + EvaluationTime::duration{1}, memory));
            } catch (const std::logic_error &) {
                operation_switch_rejected = true;
            }
            require(operation_switch_rejected, "a TSF view output silently switched from projection to filtering");
        });
    });
}

void test_incremental_numeric_filter() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        bool became_valid{};
        bool became_invalid{};
        bool modified{};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
        std::vector<double> bids;
    };
    auto observations = std::vector<observation>{};
    auto predicate_visits = std::vector<std::size_t>{};
    const auto write = node<filter_writer>();
    const auto filter = node([&](In<TSF<Quotes>, optional> input, Out<TSF<Quotes>, view> output) {
        auto visits = std::size_t{};
        output.template bind_filter<"size">(input, [&](row_id_t id) {
            ++visits;
            return input.cell<0>(id).value_or(0) > 4;
        });
        predicate_visits.push_back(visits);
    });
    const auto observe = node([&](In<TSF<Quotes>, optional> input, In<TS<i64>> tick,
                                  Out<TS<Frame<Quotes>>> snapshot) {
        auto result = observation{.valid = input.valid(),
                                  .became_valid = input.became_valid(),
                                  .became_invalid = input.became_invalid(),
                                  .modified = input.modified()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.bids.push_back(input.cell<1>(id).value_or(-1.0));
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        if (input.valid())
            snapshot.set_value(input.value_view());
        static_cast<void>(tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        return observe(filter(write(command)), tick);
    };
    const auto commands = std::array<i64, 10>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto ticks = std::array<i64, 10>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto snapshots = testing::test_graph(graph, commands, ticks);

    require(observations.size() == commands.size(), "numeric filter observer missed a cycle");
    require(predicate_visits == std::vector<std::size_t>{0, 1, 1, 0, 1, 1, 0, 1, 0, 0},
            "numeric filter evaluated rows outside additions and predicate-column modifications");
    require(observations[0].valid && observations[0].became_valid && observations[0].modified &&
                observations[0].rows.empty(),
            "numeric filter did not bind its first empty source");
    require(observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].changed == std::vector<row_id_t>{0} && observations[1].bids ==
                    std::vector<double>{10.5},
            "numeric filter did not include a qualifying added row");
    require(observations[2].rows == std::vector<row_id_t>{0, 1} &&
                observations[2].changed == std::vector<row_id_t>{1},
            "numeric filter did not include a second qualifying row");
    require(observations[3].modified && observations[3].changed == std::vector<row_id_t>{0} &&
                observations[3].bids == std::vector<double>{11.5, 20.5},
            "numeric filter did not forward selected payload changes");
    require(observations[4].modified && observations[4].rows == std::vector<row_id_t>{1} &&
                observations[4].removed == std::vector<row_id_t>{0},
            "numeric filter did not remove a row when its predicate became false");
    require(observations[5].modified && observations[5].rows == std::vector<row_id_t>{0, 1} &&
                observations[5].changed == std::vector<row_id_t>{0},
            "numeric filter did not reinclude a row when its predicate became true");
    require(observations[6].modified && observations[6].rows == std::vector<row_id_t>{1} &&
                observations[6].removed == std::vector<row_id_t>{0},
            "numeric filter did not remove a deleted selected row");
    const auto bid_for = [](const observation &entry, row_id_t id) {
        const auto row = std::ranges::find(entry.rows, id);
        require(row != entry.rows.end(), "numeric filter observation lost a row ID");
        return entry.bids[static_cast<std::size_t>(row - entry.rows.begin())];
    };
    require(observations[7].rows.size() == 2 && observations[7].changed == std::vector<row_id_t>{0} &&
                bid_for(observations[7], 0) == 12.5 && bid_for(observations[7], 1) == 20.5,
            "numeric filter lost row/value alignment or reused row identity incorrectly");
    require(!observations[8].valid && observations[8].became_invalid &&
                observations[8].changed.empty() && observations[8].removed.empty(),
            "numeric filter did not propagate source invalidation");
    require(observations[9].valid && observations[9].became_valid && observations[9].rows.empty(),
            "numeric filter did not rebuild after an empty source revalidation");
    require(snapshots.size() == commands.size() && snapshots[2] && snapshots[2]->value().size() == 2 &&
                snapshots[2]->value().columns().bid.cell(0) == 10.5 &&
                snapshots[3]->value().columns().bid.cell(0) == 11.5,
            "numeric filter snapshots did not retain stable row data");
}

void test_filter_greater_than_composes_with_column_projection() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        std::vector<row_id_t> rows;
        std::vector<double> bids;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<filter_writer>();
    const auto observe = node([&](In<TSC<double>, optional> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.bids.push_back(input.cell(id).value_or(-1.0));
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto filtered = filter_greater_than<"size">(write(command), 6);
        const auto projected = project<BidOnly, "bid">(filtered);
        const auto price_filtered = filter_greater_than<"price">(projected, 15.0);
        return observe(select_column<"price">(price_filtered), tick);
    };
    const auto commands = std::array<i64, 10>{0, 1, 2, 3, 4, 5, 6, 7, 10, 11};
    const auto ticks = std::array<i64, 10>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static_cast<void>(testing::test_graph(graph, commands, ticks));

    require(observations.size() == commands.size() && observations[1].rows.empty() &&
                observations[2].rows == std::vector<row_id_t>{1} &&
                observations[2].bids == std::vector<double>{20.5} &&
                observations[5].rows == std::vector<row_id_t>{1} &&
                observations[8].rows.empty() && observations[8].removed == std::vector<row_id_t>{1} &&
                observations[9].rows == std::vector<row_id_t>{1} &&
                observations[9].changed == std::vector<row_id_t>{1},
            "nested filter and projection views did not propagate selection membership changes");
}

} // namespace

int main() {
    try {
        test_runtime_native_storage_mixed_and_zero_column_schemas();
        test_native_storage_validates_row_and_patch_shape_before_mutation();
        test_native_storage_changed_columns_beyond_inline_capacity();
        test_shared_rows_and_column_changes();
        test_failed_multicolumn_insert_cleanup();
        test_recycled_rows_reuse_physical_slots();
        test_failed_value_insert_rolls_back_reused_slot();
        test_frame_scalar_views_and_snapshots();
        test_runtime_frame_value_factory();
        test_inherited_frame_fields_and_views();
        test_scalar_frame_time_series_graph();
        test_compiled_graph_runtime_storage_is_independent();
        test_tsf_graph_behavior();
        test_borrowed_tsf_input_with_native_and_view_runtimes();
        test_generic_tsf_input_schema_and_columns();
        test_generic_declared_tsf_rows_follow_resolved_cell_type();
        test_repeated_and_constrained_generic_tsf_fields_match_runtime_schema();
        test_typed_patch_rows_insert_nullable_rows();
        test_runtime_schema_tsf_writes_and_named_projection();
        test_inherited_tsf_graph_behavior();
        test_base_frame_output_retains_derived_schema();
        test_tsf_projection_view();
        test_tsf_projection_empty_columns_match_target_schema();
        test_tsf_projection_selection_change();
        test_tsf_projection_rejects_invalid_mappings_before_invalidation();
        test_tsf_projection_rejects_empty_row_domain_mismatch();
        test_view_updates_do_not_traverse_source_rows();
        test_filtered_column_coverage_rejects_a_different_row_set();
        test_incremental_numeric_filter();
        test_filter_greater_than_composes_with_column_projection();
        test_scalar_variable_frame_input();
    } catch (const std::exception &error) {
        std::cerr << "bg_frame_frame_test: " << error.what() << '\n';
        return 1;
    }
}
