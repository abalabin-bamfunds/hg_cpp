#include <bg/frame/arrow.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/util/scope_exit.hpp>

#include <arrow/api.h>
#include <arrow/builder.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;

template <template <typename, auto...> typename Field> struct Quotes {
    Field<i64> size;
    Field<double> bid;
    Field<double> ask;
};

template <template <typename, auto...> typename Field> struct BooleanArrowRows { Field<bool> enabled; };

template <template <typename, auto...> typename Field> struct TextArrowRows {
    Field<string> name;
    Field<symbol> venue;
};

template <template <typename, auto...> typename Field> struct InheritedArrowBase {
    Field<i64> size;
    Field<double> bid;
};

template <template <typename, auto...> typename Field>
struct InheritedArrowFrame : bg::schema_extends<InheritedArrowBase, Field> {
    Field<double> bid;
    Field<double> ask;
};

template <template <typename, auto...> typename Field> struct EmptySchema {};

template <template <typename, auto...> typename Field> struct KeyedArrowRows {
    Field<i64> id;
    Field<i64> sequence;
    Field<double> price;

    static bg::SchemaMetadata metadata() { return key_metadata<"id", "sequence">(); }
};

struct RowTraversalOrder {
    const NativeRows *rows{};
    std::array<row_id_t, 2> ids{};

    RowSet row_set() const noexcept {
        return {this,
                nullptr,
                rows->identity(),
                [](const void *context) noexcept {
                    return static_cast<const RowTraversalOrder *>(context)->ids.size();
                },
                [](const void *context, row_id_t id) noexcept {
                    const auto &order = *static_cast<const RowTraversalOrder *>(context);
                    return order.rows->contains(id);
                },
                [](const void *context, std::size_t position) {
                    return static_cast<const RowTraversalOrder *>(context)->ids.at(position);
                },
                [](const void *context, void *target, RowSet::visit_function visit) {
                    for (const auto id : static_cast<const RowTraversalOrder *>(context)->ids)
                        visit(target, id);
                }};
    }
};

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}

void test_arrow_default_frame_runtime() {
    const auto &snapshot_runtime = frame_snapshot_runtime_type(frame_type(frame_schema_type<Quotes>()));
    const auto &resolved_runtime = resolve_scalar_runtime_type(cpp_scalar_type<Frame<Quotes>>(), {});
    require(&resolved_runtime == &snapshot_runtime && &cpp_scalar_runtime_type<Frame<Quotes>>() == &snapshot_runtime,
            "Arrow import replaced the default native Frame scalar runtime");

    const auto publish = node([](In<TS<i64>> command, Out<TS<Frame<Quotes>>> output) {
        static_cast<void>(command);
        auto frame = Frame<Quotes>{Quotes<Column>{.size = Column<i64>{{5}}, .bid = Column<double>{{12.5}},
                                                   .ask = Column<double>{{2.0}}}};
        auto view = FrameView<Quotes>{frame};
        output.set_value(Scalar<Frame<Quotes>>::from(bg::frame::details::frame_view_scalar_runtime_type<Quotes>(),
                                                     std::addressof(view)));
    });
    const auto graph = [&](wp<TS<i64>> command) { return publish(command); };
    const auto commands = std::array<i64, 1>{0};
    const auto snapshots = testing::test_graph(graph, commands);
    require(snapshots.size() == 1 && snapshots[0] &&
                snapshots[0]->view().value().columns().bid.cell(0) == 12.5,
            "generic Frame scalar output failed after Arrow runtime initialization");
}

void test_arrow_rejects_text_cells() {
    auto text = Column<string>{std::vector<std::optional<std::string_view>>{std::string_view{"alpha"}}};
    auto symbol_values = Column<symbol>{std::vector<std::optional<symbol>>{symbol{"XNAS"}}};
    const auto text_borrowed = borrow_arrow(ColumnView<>{text.view()});
    const auto text_copied = copy_to_arrow(ColumnView<>{text.view()});
    const auto symbol_borrowed = borrow_arrow(ColumnView<>{symbol_values.view()});
    const auto symbol_copied = copy_to_arrow(ColumnView<>{symbol_values.view()});

    const auto frame = Frame<TextArrowRows>{TextArrowRows<Column>{.name = std::move(text),
                                                                  .venue = std::move(symbol_values)}};
    const auto frame_view = FrameView<TextArrowRows>{frame};
    const auto frame_borrowed = borrow_arrow(frame_view);
    const auto frame_copied = copy_to_arrow(frame_view);
    require(!text_borrowed.ok() && !text_copied.ok() && !symbol_borrowed.ok() && !symbol_copied.ok() &&
                !frame_borrowed.ok() && !frame_copied.ok(),
            "Arrow export accepted a generic string or symbol column/frame view");
}

std::shared_ptr<arrow::Array> int_array(std::initializer_list<std::optional<std::int64_t>> values) {
    auto builder = arrow::Int64Builder{};
    for (const auto &value : values) {
        const auto status = value ? builder.Append(*value) : builder.AppendNull();
        if (!status.ok())
            throw std::runtime_error{status.ToString()};
    }
    auto result = std::shared_ptr<arrow::Array>{};
    const auto status = builder.Finish(&result);
    if (!status.ok())
        throw std::runtime_error{status.ToString()};
    return result;
}

std::shared_ptr<arrow::Array> double_array(std::initializer_list<std::optional<double>> values) {
    auto builder = arrow::DoubleBuilder{};
    for (const auto &value : values) {
        const auto status = value ? builder.Append(*value) : builder.AppendNull();
        if (!status.ok())
            throw std::runtime_error{status.ToString()};
    }
    auto result = std::shared_ptr<arrow::Array>{};
    const auto status = builder.Finish(&result);
    if (!status.ok())
        throw std::runtime_error{status.ToString()};
    return result;
}

std::shared_ptr<arrow::Array> boolean_array(std::initializer_list<std::optional<bool>> values) {
    auto builder = arrow::BooleanBuilder{};
    for (const auto &value : values) {
        const auto status = value ? builder.Append(*value) : builder.AppendNull();
        if (!status.ok())
            throw std::runtime_error{status.ToString()};
    }
    auto result = std::shared_ptr<arrow::Array>{};
    const auto status = builder.Finish(&result);
    if (!status.ok())
        throw std::runtime_error{status.ToString()};
    return result;
}

void test_arrow_batch_validity_fast_paths() {
    const auto values = std::array<i64, 4>{10, 20, 30, 40};
    const auto all_valid = ColumnBatch<i64>{0, values.size(), values.data(), ColumnBatchValidity::all_valid()};
    auto all_valid_result = arrow_details::make_array(all_valid);
    require(all_valid_result.ok(), "all-valid Arrow batch export failed");
    const auto all_valid_array = std::move(all_valid_result).ValueUnsafe();
    require(all_valid_array->null_count() == 0 && all_valid_array->data()->buffers[0] == nullptr,
            "all-valid Arrow batch allocated a validity bitmap");

    const auto mixed_bitmap = std::array<std::uint8_t, 1>{0b00001101};
    const auto mixed = ColumnBatch<i64>{0, values.size(), values.data(),
                                        ColumnBatchValidity::from_bitmap(mixed_bitmap.data(), 0)};
    auto mixed_result = arrow_details::make_array(mixed);
    require(mixed_result.ok(), "mixed-validity Arrow batch export failed");
    const auto mixed_array = std::move(mixed_result).ValueUnsafe();
    require(mixed_array->null_count() == 1 && mixed_array->IsValid(0) && mixed_array->IsNull(1) &&
                mixed_array->IsValid(2) && mixed_array->IsValid(3),
            "mixed-validity Arrow batch produced an incorrect validity bitmap");

    const auto all_null = ColumnBatch<i64>{0, 3, nullptr, ColumnBatchValidity::all_invalid()};
    auto all_null_result = arrow_details::make_array(all_null);
    require(all_null_result.ok(), "all-null Arrow batch export failed");
    const auto all_null_array = std::move(all_null_result).ValueUnsafe();
    require(all_null_array->null_count() == 3 && all_null_array->IsNull(0) && all_null_array->IsNull(1) &&
                all_null_array->IsNull(2),
            "all-null Arrow batch produced an incorrect null count or bitmap");

    const auto boolean_values = std::array<std::uint8_t, 1>{0b00101000};
    const auto boolean_validity = std::array<std::uint8_t, 1>{0b01101000};
    const auto offset_boolean = ColumnBatch<bool>{0, 4, boolean_values.data(), 3,
                                                  ColumnBatchValidity::from_bitmap(boolean_validity.data(), 3)};
    auto boolean_result = arrow_details::make_array(offset_boolean);
    require(boolean_result.ok(), "offset Boolean Arrow batch export failed");
    const auto boolean = std::static_pointer_cast<arrow::BooleanArray>(std::move(boolean_result).ValueUnsafe());
    require(boolean->offset() == 3 && boolean->null_count() == 1 && boolean->Value(0) && boolean->IsNull(1) &&
                boolean->Value(2) && !boolean->Value(3),
            "offset Boolean Arrow batch misaligned values or validity bits");
}

void test_boolean_arrow_import_and_export() {
    auto source = boolean_array({false, true, std::nullopt, false, true, std::nullopt, true, false,
                                 true, false, false, true, std::nullopt, false, true, false});
    auto sliced = source->Slice(9, 6);
    auto chunks_result = arrow::ChunkedArray::Make({sliced, boolean_array({false, true, std::nullopt})},
                                                   arrow::boolean());
    require(chunks_result.ok(), "could not construct sliced Arrow boolean chunks");
    auto chunks = std::move(chunks_result).ValueUnsafe();

    auto imported_result = from_arrow<bool>(chunks);
    require(imported_result.ok(), "Arrow boolean column import failed");
    auto imported = std::move(imported_result).ValueUnsafe();
    const auto column = Scalar<Column<bool>>::from(imported).value();
    const auto expected = std::array<std::optional<bool>, 9>{false, false, true, std::nullopt, false,
                                                              true, false, true, std::nullopt};
    require(column.size() == expected.size(), "Arrow boolean column import changed its length");
    for (auto index = std::size_t{}; index != expected.size(); ++index)
        require(column.cell(index) == expected[index], "Arrow boolean column import lost a sliced bit or null");

    auto borrowed_result = borrow_arrow(column);
    auto copied_result = copy_to_arrow(column);
    require(borrowed_result.ok() && copied_result.ok(), "Arrow boolean column export failed");
    const auto borrowed = std::move(borrowed_result).ValueUnsafe();
    const auto copied = std::move(copied_result).ValueUnsafe();
    require(borrowed->length() == static_cast<std::int64_t>(expected.size()) &&
                copied->num_chunks() == 1 && copied->length() == static_cast<std::int64_t>(expected.size()),
            "Arrow boolean exports changed the column shape");
    require(borrowed->chunk(0)->data()->buffers[1]->data() ==
                source->data()->buffers[1]->data() + 1 && borrowed->chunk(0)->offset() == 1,
            "borrowed Arrow boolean export copied or misaligned a sliced value buffer");
    for (auto index = std::size_t{}; index != expected.size(); ++index) {
        const auto borrowed_chunk = std::static_pointer_cast<arrow::BooleanArray>(
            borrowed->chunk(static_cast<int>(index < 6 ? 0 : 1)));
        const auto borrowed_index = static_cast<std::int64_t>(index < 6 ? index : index - 6);
        const auto copied_chunk = std::static_pointer_cast<arrow::BooleanArray>(copied->chunk(0));
        require(borrowed_chunk->IsNull(borrowed_index) == !expected[index].has_value() &&
                    (borrowed_chunk->IsNull(borrowed_index) || borrowed_chunk->Value(borrowed_index) == *expected[index]) &&
                    copied_chunk->IsNull(static_cast<std::int64_t>(index)) == !expected[index].has_value() &&
                    (copied_chunk->IsNull(static_cast<std::int64_t>(index)) ||
                     copied_chunk->Value(static_cast<std::int64_t>(index)) == *expected[index]),
            "Arrow boolean export changed a bit or validity");
    }

    auto all_null = Column<bool>{{std::nullopt, std::nullopt, std::nullopt}};
    auto all_null_copy = copy_to_arrow(all_null.view());
    require(all_null_copy.ok() && all_null_copy.ValueUnsafe()->chunk(0)->null_count() == 3 &&
                all_null_copy.ValueUnsafe()->chunk(0)->data()->buffers[1]->data()[0] == 0,
            "Arrow boolean gather-copy did not preserve all-null values as zero bits");

    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("enabled", arrow::boolean(), true)}),
        std::vector<std::shared_ptr<arrow::ChunkedArray>>{chunks});
    auto frame_result = from_arrow<BooleanArrowRows>(table);
    require(frame_result.ok(), "Arrow boolean frame import failed");
    auto frame_owner = std::move(frame_result).ValueUnsafe();
    const auto frame = Scalar<Frame<BooleanArrowRows>>::from(frame_owner).value();
    const auto flags = frame.columns().enabled;
    require(flags.size() == expected.size(), "Arrow boolean frame import changed its row count");
    for (auto index = std::size_t{}; index != expected.size(); ++index)
        require(flags.cell(index) == expected[index], "Arrow boolean frame import changed a column bit");
    auto frame_borrowed = borrow_arrow(frame);
    auto frame_copied = copy_to_arrow(frame);
    require(frame_borrowed.ok() && frame_copied.ok(), "Arrow boolean frame export failed");
    const auto copied_flags = std::static_pointer_cast<arrow::BooleanArray>(frame_copied.ValueUnsafe()->column(0)->chunk(0));
    require(frame_borrowed.ValueUnsafe()->num_rows() == static_cast<std::int64_t>(expected.size()) &&
                copied_flags->length() == static_cast<std::int64_t>(expected.size()) &&
                !copied_flags->Value(0) && !copied_flags->Value(1) && copied_flags->Value(2) &&
                copied_flags->IsNull(3),
            "Arrow boolean frame export changed its row values");
}

void test_arrow_first_column_runtime_registration() {
    auto column = arrow::ChunkedArray::Make({int_array({4, std::nullopt, 6})}, arrow::int64());
    require(column.ok(), "could not construct an Arrow column for cold runtime registration");
    auto imported_result = from_arrow<i64>(std::move(column).ValueUnsafe());
    require(imported_result.ok(), "cold Arrow column import failed");

    auto imported = std::move(imported_result).ValueUnsafe();
    const auto &default_runtime = cpp_scalar_runtime_type<Column<i64>>();
    const auto &resolved_runtime = resolve_scalar_runtime_type(cpp_scalar_type<Column<i64>>(), {});
    require(&default_runtime == &resolved_runtime && &imported.runtime_type() != &default_runtime,
            "cold Arrow import replaced the default Column scalar runtime");
}

void test_inferred_arrow_frame_import() {
    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("count", arrow::int64(), true), arrow::field("price", arrow::float64(), true)}),
        std::vector<std::shared_ptr<arrow::Array>>{int_array({7, std::nullopt}),
                                                   double_array({12.5, 13.0})});
    auto imported_result = from_arrow(table);
    require(imported_result.ok(), "schema-inferred Arrow frame import failed");
    auto imported = std::move(imported_result).ValueUnsafe();
    const auto &frame_type = static_cast<const FrameType &>(imported.type());
    const auto &resolved_runtime = resolve_scalar_runtime_type(frame_type, {});
    require(&resolved_runtime == &frame_snapshot_runtime_type(frame_type) &&
                &imported.runtime_type() != &resolved_runtime,
            "schema-inferred Arrow import changed the native default frame runtime");

    auto erased_copy = ScalarValue<>{imported.view()};
    table.reset();
    const auto erased_view = erased_copy.view();
    const auto scalar = Scalar<Frame<bg::SCHEMA>>::from(erased_view);
    const auto frame = scalar.value();
    const auto generic_count = frame.column("count");
    const auto generic_price = frame.column("price");
    const auto count = generic_count.view<i64>();
    const auto price = generic_price.view<double>();
    require(frame.schema().field_count() == 2 && frame.schema().field(0).name.text() == "count" &&
                frame.schema().field(1).name.text() == "price" && frame.size() == 2 &&
                !count.cell(1) && price.cell(0) == 12.5,
            "schema-inferred Arrow frame lost its runtime schema, nulls, or columns");
    auto borrowed_count_result = borrow_arrow(generic_count);
    auto copied_price_result = copy_to_arrow(generic_price);
    require(borrowed_count_result.ok() && copied_price_result.ok(),
            "generic Arrow column export failed");
    const auto borrowed_count = std::move(borrowed_count_result).ValueUnsafe();
    const auto copied_price = std::move(copied_price_result).ValueUnsafe();
    require(borrowed_count->length() == 2 && borrowed_count->chunk(0)->IsNull(1) &&
                std::static_pointer_cast<arrow::DoubleArray>(copied_price->chunk(0))->Value(1) == 13.0,
            "generic Arrow column export changed values or nulls");
    auto borrowed_result = borrow_arrow(frame);
    auto copied_result = copy_to_arrow(frame);
    require(borrowed_result.ok() && copied_result.ok(), "schema-inferred Arrow frame export failed");
    const auto borrowed = std::move(borrowed_result).ValueUnsafe();
    const auto copied = std::move(copied_result).ValueUnsafe();
    require(borrowed->num_rows() == 2 && borrowed->schema()->field(1)->name() == "price" &&
                copied->num_rows() == 2 &&
                std::static_pointer_cast<arrow::Int64Array>(copied->column(0)->chunk(0))->IsNull(1),
            "schema-inferred Arrow export changed its shape, names, or validity");
}

void test_exports() {
    auto source = Column<i64>{{10, std::nullopt, 30}};
    auto source_batch = ColumnBatch<i64>{};
    source.view().batches([&](const auto &batch) { source_batch = batch; });

    auto borrowed_result = borrow_arrow(source.view());
    require(borrowed_result.ok(), "borrowed Arrow export failed");
    auto borrowed = std::move(borrowed_result).ValueUnsafe();
    require(borrowed->num_chunks() == 1, "borrowed Arrow export changed chunk count");
    require(borrowed->chunk(0)->data()->buffers[1]->data() ==
                reinterpret_cast<const std::uint8_t *>(source_batch.values),
            "borrowed Arrow export copied the value buffer");
    require(borrowed->chunk(0)->IsNull(1), "borrowed Arrow export lost null validity");

    auto copied_result = copy_to_arrow(source.view());
    require(copied_result.ok(), "owning Arrow export failed");
    auto copied = std::move(copied_result).ValueUnsafe();
    require(copied->chunk(0)->data()->buffers[1]->data() !=
                reinterpret_cast<const std::uint8_t *>(source_batch.values),
            "owning Arrow export borrowed the value buffer");
    require(copied->chunk(0)->data()->buffers[1]->data_as<i64>()[1] == 0,
            "owning Arrow export did not initialize a null value");

    auto durable = [] {
        auto temporary = Column<i64>{{41, std::nullopt, 43}};
        auto result = copy_to_arrow(temporary.view());
        if (!result.ok())
            throw std::runtime_error{result.status().ToString()};
        return std::move(result).ValueUnsafe();
    }();
    const auto durable_values = std::static_pointer_cast<arrow::Int64Array>(durable->chunk(0));
    require(durable_values->Value(0) == 41 && durable_values->IsNull(1) && durable_values->Value(2) == 43,
            "owning Arrow export did not survive source destruction");
}

void test_fragmented_dense_copy_and_source_mutation() {
    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto native = NativeColumn<i64>{};
    const auto time = MIN_DT + EvaluationTime::duration{1};
    const auto first = native.add(10, time, memory);
    const auto removed = native.add(20, time, memory);
    const auto last = native.add(30, time, memory);
    require(native.erase(removed, time, memory), "native Arrow source did not remove the middle row");

    const auto view = scalar_storage_traits<Column<i64>>::read(native.scalar_storage());
    auto borrowed_result = borrow_arrow(view);
    auto copied_result = copy_to_arrow(view);
    require(borrowed_result.ok() && copied_result.ok(), "fragmented Arrow export failed");
    auto borrowed = std::move(borrowed_result).ValueUnsafe();
    auto copied = std::move(copied_result).ValueUnsafe();
    require(borrowed->num_chunks() == 2, "borrowed export did not preserve fragmented native batches");
    require(copied->num_chunks() == 1, "gather-copy export did not produce one dense chunk");

    const auto dense = std::static_pointer_cast<arrow::Int64Array>(copied->chunk(0));
    require(dense->Value(0) == 10 && dense->Value(1) == 30,
            "gather-copy export changed visible-row order");
    native.set(first, 11, time + EvaluationTime::duration{1}, memory);
    native.set(last, 31, time + EvaluationTime::duration{1}, memory);
    require(dense->Value(0) == 10, "gather-copy export changed after source mutation");
    native.release(memory);
    require(dense->Value(1) == 30, "gather-copy export did not survive source release");
}

void test_computed_tsc_arrow_export_preserves_traversal_and_copy_ownership() {
    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto source = NativeColumn<double>{};
    auto output = ColumnComputedOutputStorage<i64>{};
    auto output_released = false;
    auto release = util::scope_exit{[&]() noexcept {
        if (!output_released)
            output.release(memory);
        source.release(memory);
    }};

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto first_id = source.add(10.0, first_time, memory);
    const auto second_id = source.add(20.0, first_time, memory);
    auto input = source.scalar_storage();
    auto first_order = RowTraversalOrder{input.rows, {first_id, second_id}};
    input.visible_rows = first_order.row_set();
    input.coverage = input.visible_rows;
    require(output.bind(input, true, first_time, first_time, memory),
            "computed Arrow column did not bind source rows");
    output.set(first_id, i64{100}, first_time, memory);
    output.set(second_id, std::nullopt, first_time, memory);

    const auto second_time = first_time + EvaluationTime::duration{1};
    auto reverse_order = RowTraversalOrder{input.rows, {second_id, first_id}};
    input.visible_rows = reverse_order.row_set();
    input.coverage = input.visible_rows;
    static_cast<void>(output.bind(input, false, second_time, second_time, memory));

    const auto column = scalar_storage_traits<Column<i64>>::read(output.series_view().scalar_storage());
    auto source_value_buffers = std::vector<const std::uint8_t *>{};
    column.batches([&](const auto &batch) {
        source_value_buffers.push_back(reinterpret_cast<const std::uint8_t *>(batch.values));
    });
    auto borrowed_result = borrow_arrow(column);
    auto copied_result = copy_to_arrow(column);
    require(borrowed_result.ok() && copied_result.ok(), "computed TSC Arrow export failed");
    const auto borrowed = std::move(borrowed_result).ValueUnsafe();
    const auto copied = std::move(copied_result).ValueUnsafe();
    require(borrowed->num_chunks() == 2 && borrowed->chunk(0)->IsNull(0) &&
                borrowed->chunk(1)->data()->buffers[1]->data() == source_value_buffers[1],
            "computed TSC borrowed Arrow view copied values or lost traversal order and nulls");
    require(copied->num_chunks() == 1 && copied->chunk(0)->IsNull(0) &&
                std::static_pointer_cast<arrow::Int64Array>(copied->chunk(0))->Value(1) == 100 &&
                copied->chunk(0)->data()->buffers[1]->data() != source_value_buffers[1],
            "computed TSC Arrow copy did not gather independent values in traversal order");

    const auto copied_values = std::static_pointer_cast<arrow::Int64Array>(copied->chunk(0));
    output.release(memory);
    output_released = true;
    require(copied_values->IsNull(0) && copied_values->Value(1) == 100,
            "computed TSC Arrow copy did not survive output storage release");
}

void test_null_only_and_empty_exports() {
    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto nulls = NativeColumn<i64>{};
    const auto time = MIN_DT + EvaluationTime::duration{1};
    nulls.add(std::nullopt, time, memory);
    nulls.add(std::nullopt, time, memory);
    const auto null_view = scalar_storage_traits<Column<i64>>::read(nulls.scalar_storage());

    auto borrowed_result = borrow_arrow(null_view);
    auto copied_result = copy_to_arrow(null_view);
    require(borrowed_result.ok() && copied_result.ok(), "null-only Arrow export failed");
    const auto borrowed = std::move(borrowed_result).ValueUnsafe();
    const auto copied = std::move(copied_result).ValueUnsafe();
    const auto borrowed_values = borrowed->chunk(0)->data()->buffers[1];
    const auto copied_values = copied->chunk(0)->data()->buffers[1];
    require(borrowed->chunk(0)->null_count() == 2 && borrowed_values && borrowed_values->data_as<i64>()[0] == 0,
            "borrowed null-only export did not supply initialized values");
    require(copied->num_chunks() == 1 && copied->chunk(0)->null_count() == 2 &&
                copied_values->data_as<i64>()[1] == 0,
            "dense null-only export did not initialize null bytes");
    nulls.release(memory);

    auto empty = Column<i64>{};
    auto empty_result = copy_to_arrow(empty.view());
    require(empty_result.ok() && empty_result.ValueUnsafe()->num_chunks() == 1 &&
                empty_result.ValueUnsafe()->length() == 0,
            "empty numeric gather-copy export is not one empty chunk");
}

void test_imports_and_runtime_lifetime() {
    auto sliced_sizes = int_array({-1, 5, std::nullopt, 7, -1})->Slice(1, 3);
    auto other_sizes = int_array({9});
    auto sizes_result = arrow::ChunkedArray::Make({int_array({}), sliced_sizes, other_sizes}, arrow::int64());
    require(sizes_result.ok(), "could not construct sliced chunked array");
    auto sizes = std::move(sizes_result).ValueUnsafe();

    auto imported_column_result = from_arrow<i64>(sizes);
    require(imported_column_result.ok(), "Arrow column import failed");
    auto imported_column = std::move(imported_column_result).ValueUnsafe();
    const auto erased_column = imported_column.view();
    const auto typed_column = Scalar<Column<i64>>::from(erased_column);
    require(typed_column.value().size() == 4 && typed_column.value().cell(0) == 5 &&
                !typed_column.value().cell(1) && typed_column.value().cell(3) == 9,
            "Arrow column runtime returned incorrect sliced values");

    auto bids = arrow::ChunkedArray::Make({double_array({1.5, 2.5}), double_array({3.5, 4.5})},
                                          arrow::float64());
    auto asks = arrow::ChunkedArray::Make({double_array({2.0, std::nullopt, 4.0, 5.0})}, arrow::float64());
    require(bids.ok() && asks.ok(), "could not construct Arrow frame columns");
    auto schema = arrow::schema({arrow::field("size", arrow::int64(), true),
                                 arrow::field("bid", arrow::float64(), true),
                                 arrow::field("ask", arrow::float64(), true)});
    auto table = arrow::Table::Make(schema,
                                    {std::move(sizes), std::move(bids).ValueUnsafe(), std::move(asks).ValueUnsafe()},
                                    4);

    auto imported_result = from_arrow<Quotes>(table);
    require(imported_result.ok(), "Arrow frame import failed");
    auto imported = std::move(imported_result).ValueUnsafe();
    table.reset();

    const auto imported_view = imported.view();
    const auto scalar = Scalar<Frame<Quotes>>::from(imported_view);
    require(std::addressof(scalar.runtime_type()) ==
                std::addressof(arrow_details::runtime_schema_frame_scalar_runtime(
                    static_cast<const FrameType &>(frame_type(frame_schema_type<Quotes>())))),
            "Arrow frame view changed its scalar runtime");
    const auto view = scalar.value();
    require(view.size() == 4, "imported Arrow frame lost its row count");
    require(view.columns().size.cell(0) == 5 && !view.columns().size.cell(1) && view.columns().size.cell(3) == 9,
            "imported sliced Arrow column has incorrect values");
    require(view.columns().bid.cell(3) == 4.5 && !view.columns().ask.cell(1),
            "imported independently chunked columns have incorrect values");

    auto size_values = std::array<const i64 *, 2>{};
    auto size_batch = std::size_t{};
    view.columns().size.batches([&](const auto &batch) { size_values[size_batch++] = batch.values; });
    auto borrowed_result = borrow_arrow(view);
    require(borrowed_result.ok(), "borrowed Arrow frame export failed");
    auto borrowed = std::move(borrowed_result).ValueUnsafe();
    require(borrowed->column(0)->num_chunks() == 2,
            (std::string{"borrowed frame export changed source chunk count to "} +
             std::to_string(borrowed->column(0)->num_chunks()))
                .c_str());
    for (auto index = std::size_t{}; index != size_values.size(); ++index)
        require(borrowed->column(0)->chunk(static_cast<int>(index))->data()->buffers[1]->data() ==
                    reinterpret_cast<const std::uint8_t *>(size_values[index]),
                "borrowed frame export copied a source chunk buffer");

    auto copied_result = copy_to_arrow(view);
    require(copied_result.ok(), "copy export of an imported Arrow frame failed");
    auto copied = std::move(copied_result).ValueUnsafe();
    require(copied->num_rows() == 4 && copied->num_columns() == 3,
            "Arrow frame export changed the imported shape");
    const auto copied_sizes = std::static_pointer_cast<arrow::Int64Array>(copied->column(0)->chunk(0));
    require(copied_sizes->Value(0) == 5 && copied_sizes->IsNull(1),
            "Arrow frame export lost imported values or validity");

    const auto consume_frame = bg::node([](In<TS<i64>> trigger, Scalar<Frame<Quotes>> input, Out<TS<i64>> output) {
        output.set(input.value().columns().size.cell(0).value_or(-1) + *trigger);
    });
    auto graph_owner = ScalarValue<>{scalar};
    const auto graph = [&](wp<TS<i64>> trigger) { return consume_frame(trigger, std::move(graph_owner)); };
    const auto consumed = bg::testing::test_graph(graph, std::array<i64, 1>{0});
    require(consumed.size() == 1, "BG node did not publish its Arrow-backed frame result");
    const auto consumed_value = consumed[0];
    require(consumed_value.has_value(), "BG node produced an invalid Arrow-backed frame result");
    require(consumed_value->view().value() == 5, "BG node read an incorrect Arrow-backed frame value");
}

void test_column_equality_across_arrow_chunks() {
    auto first_chunks = arrow::ChunkedArray::Make(
        {int_array({1, std::nullopt}), int_array({3, 4})}, arrow::int64());
    auto second_chunks = arrow::ChunkedArray::Make(
        {int_array({1}), int_array({std::nullopt, 3, 4})}, arrow::int64());
    require(first_chunks.ok() && second_chunks.ok(), "could not create fragmented Arrow columns");

    auto first_result = from_arrow<i64>(std::move(first_chunks).ValueUnsafe());
    auto second_result = from_arrow<i64>(std::move(second_chunks).ValueUnsafe());
    require(first_result.ok() && second_result.ok(), "could not import fragmented Arrow columns");
    auto first = std::move(first_result).ValueUnsafe();
    auto second = std::move(second_result).ValueUnsafe();
    auto native = ScalarValue<Column<i64>>::from_literal({1, NULL_VALUE, 3, 4});
    auto arrow_literal = ScalarValue<>::from_literal(first.runtime_type(), {i64{1}, NULL_VALUE, i64{3}, i64{4}});
    auto native_different = ScalarValue<Column<i64>>::from_literal({i64{1}, NULL_VALUE, i64{3}, i64{5}});

    require(&arrow_literal.runtime_type() == &first.runtime_type() && first.equals(second.view()) &&
                first.equals(native.view()) && native.equals(first.view()) &&
                arrow_literal.equals(first.view()) && first.equals(arrow_literal.view()),
            "column equality depended on Arrow chunk boundaries or storage representation");
    require(!first.equals(native_different.view()) && !native_different.equals(first.view()),
            "column equality ignored a different cell value");
}

void test_exact_arrow_frame_literal_and_native_equality() {
    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("size", arrow::int64(), true), arrow::field("bid", arrow::float64(), true),
                       arrow::field("ask", arrow::float64(), true)}),
        std::vector<std::shared_ptr<arrow::Array>>{int_array({7, std::nullopt}), double_array({12.5, 14.5}),
                                                   double_array({13.5, 15.5})});
    auto imported_result = from_arrow<Quotes>(table);
    require(imported_result.ok(), "could not import an Arrow frame for literal runtime testing");
    auto imported = std::move(imported_result).ValueUnsafe();

    auto arrow_literal = ScalarValue<>::from_literal(
        imported.runtime_type(),
        {{"ask", {13.5, 15.5}}, {symbol{"size"}, {i64{7}, NULL_VALUE}}, {"bid", {12.5, 14.5}}});
    auto native = ScalarValue<Frame<Quotes>>::from_literal(
        {{"size", {i64{7}, NULL_VALUE}}, {"bid", {12.5, 14.5}}, {"ask", {13.5, 15.5}}});
    auto different_native = ScalarValue<Frame<Quotes>>::from_literal(
        {{"size", {i64{7}, NULL_VALUE}}, {"bid", {12.5, 14.5}}, {"ask", {13.5, 99.5}}});

    const auto arrow_scalar = arrow_literal.view();
    const auto arrow_view = Scalar<Frame<Quotes>>::from(arrow_scalar).value();
    require(&arrow_literal.runtime_type() == &imported.runtime_type() && arrow_view.size() == 2 &&
                arrow_view.columns().size.cell(1) == std::nullopt && arrow_view.columns().ask.cell(1) == 15.5,
            "Arrow frame literal did not use its exact runtime or preserve values and nulls");
    require(arrow_literal.equals(native.view()) && native.equals(arrow_literal.view()),
            "native and Arrow frame equality differed for equal values");
    require(!arrow_literal.equals(different_native.view()) && !different_native.equals(arrow_literal.view()),
            "native and Arrow frame equality ignored a different cell value");
}

void test_arrow_scalar_owners() {
    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("size", arrow::int64(), true), arrow::field("bid", arrow::float64(), true),
                       arrow::field("ask", arrow::float64(), true)}),
        std::vector<std::shared_ptr<arrow::Array>>{int_array({17}), double_array({1.0}), double_array({2.0})});
    auto source_result = from_arrow<Quotes>(table);
    require(source_result.ok(), "could not create Arrow scalar owner test input");
    auto source = std::move(source_result).ValueUnsafe();
    const auto borrowed = source.view();
    const auto scalar = Scalar<Frame<Quotes>>::from(borrowed);
    auto erased_owner = ScalarValue<>{scalar};
    auto typed_owner = ScalarValue<Frame<Quotes>>{scalar};

    require(&erased_owner.runtime_type().type() == &typed_owner.runtime_type().type() &&
                &erased_owner.runtime_type() != &typed_owner.runtime_type(),
            "typed and erased Arrow owners did not keep distinct scalar runtimes");
    auto erased_copy = ScalarValue<>{i64{0}};
    erased_copy = erased_owner;
    require(&erased_copy.runtime_type() == &erased_owner.runtime_type(),
            "erased Arrow scalar copy did not preserve its runtime");
    table.reset();
    source = ScalarValue<>{i64{0}};
    const auto typed_view = typed_owner.view();
    require(typed_view.value().columns().size.cell(0) == 17,
            "typed native snapshot did not outlive its Arrow source owner");
    const auto erased_view = erased_owner.view();
    require(Scalar<Frame<Quotes>>::from(erased_view).value().columns().ask.cell(0) == 2.0,
            "erased Arrow scalar owner did not outlive its source owner");
    const auto erased_copy_view = erased_copy.view();
    require(Scalar<Frame<Quotes>>::from(erased_copy_view).value().columns().ask.cell(0) == 2.0,
            "copied erased Arrow scalar did not outlive its source owner");
}

void test_inherited_arrow_frame_fields() {
    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("size", arrow::int64(), true), arrow::field("bid", arrow::float64(), true),
                       arrow::field("ask", arrow::float64(), true)}),
        std::vector<std::shared_ptr<arrow::Array>>{int_array({5, 6}), double_array({12.5, 23.0}),
                                                   double_array({std::nullopt, 21.5})});
    const auto &derived_schema = static_cast<const bg::Schema &>(frame_schema_type<InheritedArrowFrame>());
    auto imported_result = from_arrow(table, derived_schema);
    require(imported_result.ok(), "inherited Arrow frame import failed");
    auto imported = std::move(imported_result).ValueUnsafe();
    table.reset();

    const auto imported_view = imported.view();
    const auto scalar = Scalar<Frame<InheritedArrowFrame>>::from(imported_view);
    const auto view = scalar.value();
    const auto fields = bg::details::hierarchical_scalar_schema_reflection<InheritedArrowFrame>::fields(
        view.columns());
    require(view.size() == 2 && std::get<0>(fields).cell(1) == 6 && std::get<1>(fields).cell(0) == 12.5 &&
                !std::get<2>(fields).cell(0) && std::get<2>(fields).cell(1) == 21.5,
            "inherited Arrow frame read used physical rather than logical fields");

    const auto base_view = Scalar<Frame<InheritedArrowBase>>::from(scalar).value();
    require(base_view.columns().size.cell(0) == 5 && base_view.columns().bid.cell(0) == 12.5 &&
                base_view.schema().field_count() == 3 && base_view.column("ask").view<double>().cell(1) == 21.5,
            "base Arrow frame view did not inspect its retained derived column");
    auto base_borrowed_result = borrow_arrow(base_view);
    auto base_copied_result = copy_to_arrow(base_view);
    require(base_borrowed_result.ok() && base_copied_result.ok(),
            "base typed Arrow export failed to retain the derived schema");
    auto base_borrowed = std::move(base_borrowed_result).ValueUnsafe();
    auto base_copied = std::move(base_copied_result).ValueUnsafe();
    require(base_borrowed->num_columns() == 3 && base_borrowed->schema()->field(2)->name() == "ask" &&
                base_copied->num_columns() == 3 && base_copied->schema()->field(2)->name() == "ask" &&
                std::static_pointer_cast<arrow::DoubleArray>(base_copied->column(2)->chunk(0))->Value(1) == 21.5,
            "base typed Arrow export narrowed or changed the actual derived columns");

    auto borrowed_result = borrow_arrow(view);
    require(borrowed_result.ok(), "inherited Arrow frame borrowed export failed");
    auto borrowed = std::move(borrowed_result).ValueUnsafe();
    auto source_bid_values = std::array<const double *, 1>{};
    auto source_bid_batches = std::size_t{};
    std::get<1>(fields).batches([&](const auto &batch) { source_bid_values[source_bid_batches++] = batch.values; });
    require(borrowed->num_columns() == 3 && borrowed->schema()->field(1)->name() == "bid" &&
                borrowed->column(1)->chunk(0)->data()->buffers[1]->data() ==
                    reinterpret_cast<const std::uint8_t *>(source_bid_values[0]),
            "inherited Arrow borrowed export copied or reordered a logical field");

    auto copied_result = copy_to_arrow(view);
    require(copied_result.ok(), "inherited Arrow frame gather failed");
    auto copied = std::move(copied_result).ValueUnsafe();
    const auto bids = std::static_pointer_cast<arrow::DoubleArray>(copied->column(1)->chunk(0));
    const auto asks = std::static_pointer_cast<arrow::DoubleArray>(copied->column(2)->chunk(0));
    require(copied->schema()->field(0)->name() == "size" && copied->schema()->field(1)->name() == "bid" &&
                copied->schema()->field(2)->name() == "ask" &&
                bids->Value(0) == 12.5 && bids->Value(1) == 23.0 && asks->IsNull(0) && asks->Value(1) == 21.5,
            "inherited Arrow gather changed logical order, values, or nulls");
}

void test_zero_column_row_count() {
    auto table = arrow::Table::Make(arrow::schema({}),
                                    std::vector<std::shared_ptr<arrow::ChunkedArray>>{}, 3);
    auto inferred = from_arrow(table);
    require(inferred.ok(), "schema-inferred zero-column Arrow frame import failed");
    auto inferred_value = std::move(inferred).ValueUnsafe();
    const auto inferred_view = inferred_value.view();
    const auto inferred_frame = Scalar<Frame<bg::SCHEMA>>::from(inferred_view).value();
    require(inferred_frame.schema().field_count() == 0 && inferred_frame.size() == 3,
            "schema-inferred zero-column Arrow frame lost its row count");
    auto imported = from_arrow<EmptySchema>(table);
    require(imported.ok(), "zero-column Arrow frame import failed");
    auto imported_value = std::move(imported).ValueUnsafe();
    const auto imported_view = imported_value.view();
    const auto frame = Scalar<Frame<EmptySchema>>::from(imported_view).value();
    require(frame.size() == 3, "zero-column Arrow frame lost its explicit row count");
    auto exported = borrow_arrow(frame);
    require(exported.ok() && exported.ValueUnsafe()->num_rows() == 3,
            "zero-column Arrow export lost its row count");
}

void test_schema_validation() {
    const auto &schema = static_cast<const bg::Schema &>(frame_schema_type<Quotes>());
    auto wrong_name = arrow::Table::Make(
        arrow::schema({arrow::field("wrong", arrow::int64(), true), arrow::field("bid", arrow::float64(), true),
                       arrow::field("ask", arrow::float64(), true)}),
        std::vector<std::shared_ptr<arrow::Array>>{int_array({1}), double_array({2.0}), double_array({3.0})});
    require(!from_arrow<Quotes>(wrong_name).ok(), "Arrow import accepted a mismatched column name");

    auto wrong_type = arrow::Table::Make(
        arrow::schema({arrow::field("size", arrow::int64(), true), arrow::field("bid", arrow::float64(), true),
                       arrow::field("ask", arrow::float64(), true)}),
        std::vector<std::shared_ptr<arrow::Array>>{int_array({1}), double_array({2.0}), int_array({3})});
    require(!from_arrow<Quotes>(wrong_type).ok(), "Arrow import accepted a mismatched column type");
    require(!from_arrow(wrong_name, schema).ok(), "explicit-schema Arrow import accepted a mismatched name");
    require(!from_arrow(wrong_type, schema).ok(), "explicit-schema Arrow import accepted a mismatched type");

    auto wrong_order = arrow::Table::Make(
        arrow::schema({arrow::field("bid", arrow::float64(), true), arrow::field("size", arrow::int64(), true),
                       arrow::field("ask", arrow::float64(), true)}),
        std::vector<std::shared_ptr<arrow::Array>>{double_array({2.0}), int_array({1}), double_array({3.0})});
    require(!from_arrow(wrong_order, schema).ok(), "explicit-schema Arrow import accepted a field order mismatch");

    auto duplicate_names = arrow::Table::Make(
        arrow::schema({arrow::field("dup", arrow::int64(), true), arrow::field("dup", arrow::float64(), true)}),
        std::vector<std::shared_ptr<arrow::Array>>{int_array({1}), double_array({2.0})});
    require(!from_arrow(duplicate_names).ok(), "schema-inferred Arrow import accepted duplicate field names");

    auto unsupported = arrow::Table::Make(
        arrow::schema({arrow::field("label", arrow::utf8(), true)}),
        std::vector<std::shared_ptr<arrow::Array>>{arrow::MakeArrayOfNull(arrow::utf8(), 1).ValueOrDie()});
    require(!from_arrow(unsupported).ok(), "schema-inferred Arrow import accepted an unsupported cell type");
}

void test_arrow_frame_key_validation() {
    auto sliced_ids = int_array({-1, 10, std::nullopt, 30, -1})->Slice(1, 2);
    auto unique_ids = arrow::ChunkedArray::Make({sliced_ids, int_array({30})}, arrow::int64());
    auto unique_sequences = arrow::ChunkedArray::Make({int_array({2}), int_array({3, 1})}, arrow::int64());
    auto unique_prices = arrow::ChunkedArray::Make({double_array({10.0, 11.0, 12.0})}, arrow::float64());
    require(unique_ids.ok() && unique_sequences.ok() && unique_prices.ok(),
            "could not create independently chunked composite key columns");
    auto unique_table = arrow::Table::Make(
        arrow::schema({arrow::field("id", arrow::int64(), true), arrow::field("sequence", arrow::int64(), true),
                       arrow::field("price", arrow::float64(), true)}),
        std::vector<std::shared_ptr<arrow::ChunkedArray>>{std::move(unique_ids).ValueUnsafe(),
                                                          std::move(unique_sequences).ValueUnsafe(),
                                                          std::move(unique_prices).ValueUnsafe()});
    auto unique_import = from_arrow<KeyedArrowRows>(unique_table);
    require(unique_import.ok(), "Arrow import rejected unique composite keys with sliced, unequal chunks");
    const auto unique_scalar = unique_import.ValueUnsafe().view();
    const auto unique_frame = Scalar<Frame<KeyedArrowRows>>::from(unique_scalar).value();
    require(unique_frame.size() == 3 && unique_frame.columns().id.cell(0) == 10 &&
                !unique_frame.columns().id.cell(1) && unique_frame.columns().sequence.cell(2) == 1,
            "Arrow import changed composite key values or nulls across unequal chunks");

    auto ids = arrow::ChunkedArray::Make({int_array({std::nullopt}), int_array({std::nullopt})}, arrow::int64());
    auto sequences = arrow::ChunkedArray::Make({int_array({std::nullopt}), int_array({std::nullopt})}, arrow::int64());
    auto prices = arrow::ChunkedArray::Make({double_array({10.0}), double_array({11.0})}, arrow::float64());
    require(ids.ok() && sequences.ok() && prices.ok(), "could not create chunked key columns");

    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("id", arrow::int64(), true), arrow::field("sequence", arrow::int64(), true),
                       arrow::field("price", arrow::float64(), true)}),
        std::vector<std::shared_ptr<arrow::ChunkedArray>>{std::move(ids).ValueUnsafe(),
                                                          std::move(sequences).ValueUnsafe(),
                                                          std::move(prices).ValueUnsafe()});
    auto static_import = from_arrow<KeyedArrowRows>(table);
    require(!static_import.ok(), "Arrow import accepted duplicate all-null composite keys across chunks");

    const auto fields = std::array{SchemaField{util::symbol{"id"}, &cpp_scalar_type<i64>()},
                                  SchemaField{util::symbol{"sequence"}, &cpp_scalar_type<i64>()},
                                  SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()}};
    const auto &schema = anonymous_schema(fields, key_metadata<"id", "sequence">());
    auto runtime_import = from_arrow(table, schema);
    require(!runtime_import.ok(), "runtime-schema Arrow import accepted duplicate all-null composite keys");

    const auto colliding_key = static_cast<i64>(bg::frame::details::null_key_hash);
    const auto make_collision_table = [](std::initializer_list<std::optional<i64>> id_values) {
        const auto row_count = id_values.size();
        auto ids = arrow::ChunkedArray::Make({int_array(id_values)}, arrow::int64()).ValueOrDie();
        auto sequences = arrow::ChunkedArray::Make(
            {row_count == 2 ? int_array({7, 7}) : int_array({7, 7, 7})}, arrow::int64()).ValueOrDie();
        auto prices = arrow::ChunkedArray::Make(
            {row_count == 2 ? double_array({1.0, 1.0}) : double_array({1.0, 1.0, 1.0})},
            arrow::float64()).ValueOrDie();
        return arrow::Table::Make(
            arrow::schema({arrow::field("id", arrow::int64(), true),
                           arrow::field("sequence", arrow::int64(), true),
                           arrow::field("price", arrow::float64(), true)}),
            std::vector<std::shared_ptr<arrow::ChunkedArray>>{std::move(ids), std::move(sequences),
                                                              std::move(prices)});
    };
    auto distinct_collision = from_arrow<KeyedArrowRows>(
        make_collision_table({std::optional<i64>{}, std::optional<i64>{colliding_key}}));
    require(distinct_collision.ok(), "Arrow import rejected distinct keys with colliding hashes");
    require(!from_arrow<KeyedArrowRows>(make_collision_table(
                {std::optional<i64>{}, std::optional<i64>{colliding_key}, std::optional<i64>{}})).ok(),
            "Arrow import missed a duplicate behind a distinct colliding composite key");
}

} // namespace

int main() {
    test_arrow_batch_validity_fast_paths();
    test_arrow_first_column_runtime_registration();
    test_inferred_arrow_frame_import();
    test_arrow_default_frame_runtime();
    test_arrow_rejects_text_cells();
    test_exports();
    test_boolean_arrow_import_and_export();
    test_fragmented_dense_copy_and_source_mutation();
    test_computed_tsc_arrow_export_preserves_traversal_and_copy_ownership();
    test_null_only_and_empty_exports();
    test_imports_and_runtime_lifetime();
    test_column_equality_across_arrow_chunks();
    test_exact_arrow_frame_literal_and_native_equality();
    test_zero_column_row_count();
    test_schema_validation();
    test_arrow_scalar_owners();
    test_inherited_arrow_frame_fields();
    test_arrow_frame_key_validation();
}
