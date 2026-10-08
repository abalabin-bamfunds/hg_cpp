#include <bg/frame/frame.hpp>
#include <bg/bg.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/type/type_registry.hpp>

#include <array>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;

template <template <typename, auto...> typename Field> struct Quotes {
    Field<bg::i64> size;
    Field<double> bid;
    Field<double> ask;
};

template <typename T> concept complete_type = requires { sizeof(T); };

static_assert(complete_type<Out<TSF<Quotes>>>);
static_assert(complete_type<Out<TSF<Quotes>, bg::frame::view>>);
static_assert(complete_type<Out<TSC<bg::i64>>>);
static_assert(complete_type<Out<TSC<bg::i64>, bg::frame::view>>);
static_assert(!complete_type<Out<TS<bg::i64>, bg::frame::view>>);

template <template <typename, auto...> typename Field> struct EmptySchema {};
template <template <typename, auto...> typename Field> struct BidOnly { Field<double> price; };

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

void test_shared_rows_and_column_changes() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto frame = NativeFrame<Quotes>{};
    auto release = util::scope_exit{[&]() noexcept { frame.release(memory); }};
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto id = frame.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                           .ask = std::nullopt},
                              first_time, memory);

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
    auto scalar = Scalar<Frame<Quotes>>::from(immutable.scalar_storage());
    const auto view = scalar.value();
    require(view.size() == 2 && view.columns.size.cell(1) == 6 && view.columns.bid.cell(0) == 10.5 &&
                !view.columns.ask.cell(0),
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
    auto empty_scalar = Scalar<Frame<EmptySchema>>::from(empty.scalar_storage());
    require(empty_scalar.value().size() == 4, "zero-column scalar frame lost its row count");

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto native = NativeFrame<Quotes>{};
    auto release = util::scope_exit{[&]() noexcept { native.release(memory); }};
    const auto time = MIN_DT + EvaluationTime::duration{1};
    const auto id = native.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                            .ask = Cell<double>{std::nullopt}},
                              time, memory);
    auto native_scalar = Scalar<Frame<Quotes>>::from(native.scalar_storage());
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
        require(stable.view().value().columns.bid.cell(0) == 10.5,
                "owning frame scalar changed after native row mutation");
    }
    require(failing_memory.allocations == failing_memory.deallocations, "owning frame snapshot leaked allocations");
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
        output.set_value(input.value_view());
    });
    const auto graph = [&](wp<TS<FrameValue>> input) { return copy(input); };
    const auto result = testing::test_graph(graph, std::vector{first, second});
    require(result.size() == 2 && result[0] && result[1], "TS<Frame> graph lost scalar frame values");
    require(result[0]->value().columns.bid.cell(0) == 10.5 && !result[0]->value().columns.ask.cell(0) &&
                result[1]->value().columns.size.cell(0) == 6,
            "TS<Frame> graph changed the scalar frame data");
}

struct frame_writer_state {
    row_id_t row{no_row};
};

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

struct tsf_switchable_projection {
    In<TSF<Quotes>> input;
    In<TS<i64>> selection;
    Out<TSF<BidOnly>, view> output;

    void evaluate() const {
        if (*selection == 0)
            output.bind_projection(input, columns<"bid">());
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
        captured_bid = frame.value_view().value().columns.bid.cell(0).value_or(-1.0);
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
        captured_bid = value.columns.bid.cell(0).value_or(-1.0);
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
    require(snapshots[0]->value().size() == 0 && snapshots[1]->value().columns.bid.cell(0) == 10.5 &&
                snapshots[2]->value().columns.bid.cell(0) == 11.5 &&
                !snapshots[2]->value().columns.ask.cell(0) && snapshots[3]->value().columns.ask.cell(0) == 12.5 &&
                snapshots[4]->value().columns.bid.cell(0) == 11.5 && snapshots[5]->value().size() == 0 &&
                snapshots[7]->value().size() == 0 && snapshots[8]->value().size() == 0,
            "TSF scalar snapshots changed after later row mutations");
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
            source.value_view().value().columns.bid.batches(
                [&](const auto &batch) { source_values.push_back(batch.values); });
            auto projected_values = std::vector<const double *>{};
            input.value_view().value().columns.price.batches(
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

void test_tsf_projection_rejects_empty_row_domain_mismatch() {
    using namespace bg;
    using namespace bg::frame;

    auto memory = allocator{};
    const auto time = MIN_DT + EvaluationTime::duration{1};
    auto first = NativeFrame<Quotes>{};
    auto second = NativeFrame<Quotes>{};
    first.clear(time, memory);
    second.clear(time, memory);

    auto mismatched = second.scalar_storage();
    const auto source_columns = util::aggregate_members(first.scalar_storage().columns);
    auto mismatched_columns = util::aggregate_members(mismatched.columns);
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        ((std::get<Index>(mismatched_columns) = std::get<Index>(source_columns)), ...);
    }(std::make_index_sequence<util::aggregate_member_count<Quotes<Column>>>{});

    auto output = FrameViewOutputStorage<BidOnly>{};
    auto rejected = false;
    try {
        static_cast<void>(output.bind_projection(mismatched, false, MIN_DT, time, memory, columns<"bid">()));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    output.release(memory);
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
    auto projection = FrameViewOutputStorage<BidOnly>{};
    auto projection_release = util::scope_exit{[&]() noexcept { projection.release(memory); }};
    auto filter = FrameViewOutputStorage<Quotes>{};
    auto filter_release = util::scope_exit{[&]() noexcept { filter.release(memory); }};

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto id = source.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                            .ask = Cell<double>{11.0}},
                               first_time, memory);
    const auto row_counter = RowTraversalCounter{std::addressof(source.rows())};
    const auto make_input = [&] {
        auto input = source.scalar_storage();
        const auto rows = row_counter.row_set();
        input.rows = rows;
        std::apply(
            [&](auto &...column) { ((column.visible_rows = rows, column.coverage = rows), ...); },
            util::aggregate_members(input.columns));
        return input;
    };

    auto input = make_input();
    static_cast<void>(projection.bind_projection(input, false, MIN_DT, first_time, memory, columns<"bid">()));
    static_cast<void>(filter.bind_filter<"size">(input, false, MIN_DT, first_time, memory,
                                                  [&](row_id_t row) { return source.cell<0>(row).value_or(0) > 4; }));

    auto size_patch = Quotes<PatchCell>{};
    size_patch.size = 6;
    const auto second_time = first_time + EvaluationTime::duration{1};
    source.set(id, size_patch, second_time, memory);
    input = make_input();
    row_counter.traversals = 0;
    static_cast<void>(filter.bind_filter<"size">(
        input, true, input.change_time, second_time, memory,
        [&](row_id_t row) { return source.cell<0>(row).value_or(0) > 4; }));
    require(row_counter.traversals == 0,
            "steady-state predicate-column update traversed all source rows");

    auto bid_patch = Quotes<PatchCell>{};
    bid_patch.bid = 12.5;
    const auto third_time = second_time + EvaluationTime::duration{1};
    source.set(id, bid_patch, third_time, memory);
    input = make_input();
    row_counter.traversals = 0;
    static_cast<void>(projection.bind_projection(input, true, input.change_time, third_time, memory,
                                                 columns<"bid">()));
    static_cast<void>(filter.bind_filter<"size">(
        input, true, input.change_time, third_time, memory,
        [&](row_id_t row) { return source.cell<0>(row).value_or(0) > 4; }));
    require(row_counter.traversals == 0,
            "steady-state projection and payload updates traversed all source rows");
}

void test_filtered_column_coverage_rejects_a_different_row_set() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto source = NativeFrame<Quotes>{};
    auto source_release = util::scope_exit{[&]() noexcept { source.release(memory); }};
    auto filter = FrameViewOutputStorage<Quotes>{};
    auto filter_release = util::scope_exit{[&]() noexcept { filter.release(memory); }};
    auto projection = FrameViewOutputStorage<BidOnly>{};
    auto projection_release = util::scope_exit{[&]() noexcept { projection.release(memory); }};

    const auto time = MIN_DT + EvaluationTime::duration{1};
    static_cast<void>(source.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                              .ask = Cell<double>{11.0}},
                                time, memory));
    static_cast<void>(source.add(Quotes<Cell>{.size = Cell<i64>{8}, .bid = Cell<double>{20.5},
                                              .ask = Cell<double>{21.0}},
                                time, memory));
    const auto input = source.scalar_storage();
    static_cast<void>(filter.bind_filter<"size">(input, false, MIN_DT, time, memory,
                                                 [&](row_id_t id) {
                                                     return source.cell<0>(id).value_or(0) > 6;
                                                 }));

    auto mismatched = input;
    std::get<1>(util::aggregate_members(mismatched.columns)) =
        std::get<1>(util::aggregate_members(filter.scalar.columns));
    auto rejected = false;
    try {
        static_cast<void>(projection.bind_projection(mismatched, false, MIN_DT, time, memory,
                                                     columns<"bid">()));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "a filtered column accepted a frame row set outside its visible coverage");

    auto operation_switch_rejected = false;
    try {
        static_cast<void>(filter.bind_projection(input, false, MIN_DT, time + EvaluationTime::duration{1}, memory,
                                                 columns<"size", "bid", "ask">()));
    } catch (const std::logic_error &) {
        operation_switch_rejected = true;
    }
    require(operation_switch_rejected, "a TSF view output silently switched from filtering to projection");

    static_cast<void>(projection.bind_projection(input, false, MIN_DT, time, memory, columns<"bid">()));
    operation_switch_rejected = false;
    try {
        static_cast<void>(projection.bind_filter<"price">(
            projection.scalar, false, MIN_DT, time + EvaluationTime::duration{1}, memory,
            [](row_id_t) { return true; }));
    } catch (const std::logic_error &) {
        operation_switch_rejected = true;
    }
    require(operation_switch_rejected, "a TSF view output silently switched from projection to filtering");
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
                snapshots[2]->value().columns.bid.cell(0) == 10.5 &&
                snapshots[3]->value().columns.bid.cell(0) == 11.5,
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
        test_shared_rows_and_column_changes();
        test_failed_multicolumn_insert_cleanup();
        test_recycled_rows_reuse_physical_slots();
        test_failed_value_insert_rolls_back_reused_slot();
        test_frame_scalar_views_and_snapshots();
        test_scalar_frame_time_series_graph();
        test_compiled_graph_runtime_storage_is_independent();
        test_tsf_graph_behavior();
        test_tsf_projection_view();
        test_tsf_projection_selection_change();
        test_tsf_projection_rejects_empty_row_domain_mismatch();
        test_view_updates_do_not_traverse_source_rows();
        test_filtered_column_coverage_rejects_a_different_row_set();
        test_incremental_numeric_filter();
        test_filter_greater_than_composes_with_column_projection();
    } catch (const std::exception &error) {
        std::cerr << "bg_frame_frame_test: " << error.what() << '\n';
        return 1;
    }
}
