#include <bg/frame/arrow.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>

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

template <template <typename, auto...> typename Field> struct EmptySchema {};

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
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
    const auto view = scalar.value();
    require(view.size() == 4, "imported Arrow frame lost its row count");
    require(view.columns.size.cell(0) == 5 && !view.columns.size.cell(1) && view.columns.size.cell(3) == 9,
            "imported sliced Arrow column has incorrect values");
    require(view.columns.bid.cell(3) == 4.5 && !view.columns.ask.cell(1),
            "imported independently chunked columns have incorrect values");

    auto size_values = std::array<const i64 *, 2>{};
    auto size_batch = std::size_t{};
    view.columns.size.batches([&](const auto &batch) { size_values[size_batch++] = batch.values; });
    auto borrowed_result = borrow_arrow(view);
    require(borrowed_result.ok(), "borrowed Arrow frame export failed");
    auto borrowed = std::move(borrowed_result).ValueUnsafe();
    require(borrowed->column(0)->num_chunks() == 2, "borrowed frame export changed source chunk count");
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
        output.set(input.value().columns.size.cell(0).value_or(-1) + *trigger);
    });
    auto graph_owner = ScalarValue<>{scalar};
    const auto graph = [&](wp<TS<i64>> trigger) { return consume_frame(trigger, std::move(graph_owner)); };
    const auto consumed = bg::testing::test_graph(graph, std::array<i64, 1>{0});
    require(consumed.size() == 1, "BG node did not publish its Arrow-backed frame result");
    const auto consumed_value = consumed[0];
    require(consumed_value.has_value(), "BG node produced an invalid Arrow-backed frame result");
    require(**consumed_value == 5, "BG node read an incorrect Arrow-backed frame value");
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
    table.reset();
    source = ScalarValue<>{i64{0}};
    const auto typed_view = typed_owner.view();
    require(typed_view.value().columns.size.cell(0) == 17,
            "typed native snapshot did not outlive its Arrow source owner");
    const auto erased_view = erased_owner.view();
    require(Scalar<Frame<Quotes>>::from(erased_view).value().columns.ask.cell(0) == 2.0,
            "erased Arrow scalar owner did not outlive its source owner");
}

void test_zero_column_row_count() {
    auto table = arrow::Table::Make(arrow::schema({}),
                                    std::vector<std::shared_ptr<arrow::ChunkedArray>>{}, 3);
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
}

} // namespace

int main() {
    test_exports();
    test_fragmented_dense_copy_and_source_mutation();
    test_null_only_and_empty_exports();
    test_imports_and_runtime_lifetime();
    test_zero_column_row_count();
    test_schema_validation();
    test_arrow_scalar_owners();
}
