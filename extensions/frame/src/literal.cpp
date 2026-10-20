#include <bg/frame/literal.hpp>

#include <bg/frame/column.hpp>
#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/details/value_access.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include <bg/util/static_storage.hpp>
#include "literal_helpers.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <variant>
#include <vector>

namespace bg::frame {

const literal::Sentinel NULL_VALUE{symbol{"frame.null"}};

namespace {

std::mutex &literal_registration_mutex() {
    static bg::util::static_storage<std::mutex> mutex;
    return mutex.get();
}

template <cell_type T>
void construct_column_literal(const literal::ScalarLiteral &literal, const ScalarRuntimeType &runtime,
                              void *destination, allocator &memory) {
    if (std::addressof(runtime) != std::addressof(cpp_scalar_runtime_type<Column<T>>()))
        throw std::invalid_argument{"frame column runtime does not support native literal construction"};

    literal.visit(
        [](const literal::ScalarLiteralItem &) {
            throw std::invalid_argument{"column literal must be a sequence of cells"};
        },
        [&](std::span<const literal::ScalarLiteral> cells) {
            auto *snapshot = ColumnSnapshot<T>::create(memory, cells.size());
            auto rollback = util::scope_exit{[&]() noexcept { snapshot->release(); }};
            for (auto position = std::size_t{}; position != cells.size(); ++position) {
                if (auto cell = details::read_literal_cell<T>(cells[position])) {
                    snapshot->value_at(position) = *cell;
                    snapshot->set_valid(static_cast<row_id_t>(position));
                }
            }
            std::construct_at(static_cast<ColumnStorage<T> *>(destination), snapshot);
            rollback.release();
        });
}

void construct_column_literal(const literal::ScalarLiteral &literal, const ScalarRuntimeType &runtime,
                              void *destination, allocator &memory) {
    const auto &column = static_cast<const ColumnType &>(runtime.type());
    if (std::addressof(column.cell_type()) == std::addressof(cpp_scalar_type<i64>()))
        return construct_column_literal<i64>(literal, runtime, destination, memory);
    if (std::addressof(column.cell_type()) == std::addressof(cpp_scalar_type<double>()))
        return construct_column_literal<double>(literal, runtime, destination, memory);
    throw std::invalid_argument{"frame column literal supports only i64 and double cells"};
}

using FrameLiteralCell = std::variant<std::optional<i64>, std::optional<double>>;

void initialize_frame_literal_cells(const bg::Schema &schema, std::vector<FrameLiteralCell> &cells) {
    cells.resize(schema.field_count());
    for (auto index = std::size_t{}; index != schema.field_count(); ++index) {
        const auto &type = *schema.field(index).type;
        if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>()))
            cells[index].emplace<std::optional<i64>>();
        else if (std::addressof(type) == std::addressof(cpp_scalar_type<double>()))
            cells[index].emplace<std::optional<double>>();
        else
            throw std::invalid_argument{"frame literal supports only i64 and double cells"};
    }
}

void read_frame_literal_cell(const literal::ScalarLiteral &literal, const Type &type,
                             FrameLiteralCell &destination) {
    if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>()))
        destination.emplace<std::optional<i64>>(details::read_literal_cell<i64>(literal));
    else if (std::addressof(type) == std::addressof(cpp_scalar_type<double>()))
        destination.emplace<std::optional<double>>(details::read_literal_cell<double>(literal));
    else
        throw std::invalid_argument{"frame literal supports only i64 and double cells"};
}

const void *frame_literal_cell_pointer(const FrameLiteralCell &cell) {
    return std::visit([](const auto &value) -> const void * { return std::addressof(value); }, cell);
}

KeyValue frame_literal_key_value(const FrameLiteralCell &cell) {
    const auto &value = std::get<std::optional<i64>>(cell);
    return value ? KeyValue{*value} : KeyValue{std::monostate{}};
}

std::vector<KeyValue> read_frame_literal_key(std::span<const FrameLiteralCell> cells,
                                            const details::NativeFrameLayout &layout) {
    auto key = std::vector<KeyValue>{};
    key.reserve(layout.key_fields.size());
    for (const auto index : layout.key_fields) {
        key.push_back(frame_literal_key_value(cells[index]));
    }
    return key;
}

void construct_frame_literal(const literal::ScalarLiteral &literal, const ScalarRuntimeType &runtime,
                             void *destination, allocator &memory) {
    const auto &frame_type = static_cast<const FrameType &>(runtime.type());
    const auto &schema_type = frame_type.schema_type();
    if (!schema_type.is_resolved())
        throw std::invalid_argument{"frame literal requires a resolved schema"};
    const auto &schema = static_cast<const Schema &>(schema_type);

    const auto &native_runtime = frame_snapshot_runtime_type(frame_type);
    if (std::addressof(runtime) != std::addressof(native_runtime))
        throw std::invalid_argument{"frame runtime does not support native literal construction"};

    literal.visit(
        [](const literal::ScalarLiteralItem &) {
            throw std::invalid_argument{"frame literal must contain named columns"};
        },
        [&](std::span<const literal::ScalarLiteral> entries) {
            auto column_literals = std::vector<const literal::ScalarLiteral *>{schema.field_count(), nullptr};
            for (const auto &entry : entries) {
                const literal::ScalarLiteral *name{};
                const literal::ScalarLiteral *column{};
                entry.visit(
                    [](const literal::ScalarLiteralItem &) {
                        throw std::invalid_argument{"frame literal entries must contain a name and column"};
                    },
                    [&](std::span<const literal::ScalarLiteral> fields) {
                        if (fields.size() != 2)
                            throw std::invalid_argument{"frame literal entries must contain exactly two values"};
                        name = std::addressof(fields[0]);
                        column = std::addressof(fields[1]);
                    });

                const auto field_name = details::read_literal_field_name(*name);
                const auto *field = schema.find(symbol{field_name});
                if (field == nullptr)
                    throw std::invalid_argument{"frame literal contains an unknown column"};
                const auto field_index = static_cast<std::size_t>(field - schema.fields().data());
                if (column_literals[field_index] != nullptr)
                    throw std::invalid_argument{"frame literal contains a duplicate column"};
                column_literals[field_index] = column;
            }

            if (std::ranges::find(column_literals, nullptr) != column_literals.end())
                throw std::invalid_argument{"frame literal omits a required column"};

            auto values = std::vector<ScalarValue<>>{};
            values.reserve(column_literals.size());
            for (auto index = std::size_t{}; index != column_literals.size(); ++index) {
                const auto &field = schema.field(index);
                const ScalarRuntimeType *column_runtime{};
                if (field.type == std::addressof(cpp_scalar_type<i64>()))
                    column_runtime = std::addressof(cpp_scalar_runtime_type<Column<i64>>());
                else if (field.type == std::addressof(cpp_scalar_type<double>()))
                    column_runtime = std::addressof(cpp_scalar_runtime_type<Column<double>>());
                else
                    throw std::invalid_argument{"frame literal supports only i64 and double cells"};
                values.push_back(ScalarValue<>::from_literal(*column_runtime, *column_literals[index], memory));
            }

            auto columns = std::vector<ColumnView<>>{};
            columns.reserve(values.size());
            for (const auto &value : values)
                columns.push_back(scalar_storage_traits<Column<SCALAR>>::read_value(
                    bg::details::scalar_data(value)));

            const auto row_count = columns.empty() ? 0 : columns.front().size();
            auto value = make_frame_value(memory, schema, row_count, columns);
            runtime.construct_from_input(destination, bg::details::scalar_data(value), memory);
        });
}

template <cell_type T>
void apply_native_column_series_literal(const literal::TimeSeriesLiteral &literal, OutputState &output) {
    auto typed_output = Out<TSC<T>>{TimeSeriesOutputBinding{output}};
    if (const auto *sentinel = literal::Sentinel::from(literal)) {
        if (*sentinel == literal::SKIP)
            return;
        if (*sentinel == literal::INVALID) {
            typed_output.invalidate();
            return;
        }
        throw std::invalid_argument{"TSC literal does not accept this sentinel"};
    }

    const auto values = bg::details::scalar_literal::sequence(literal);
    if (values.empty()) {
        if (!typed_output.valid())
            typed_output.clear();
        return;
    }

    for (const auto &value : values)
        typed_output.add(details::read_literal_cell<T>(value));
}

void apply_native_column_series_literal(const literal::TimeSeriesLiteral &literal, OutputState &output) {
    const auto &type = static_cast<const ColumnSeriesType &>(output.runtime_type().type());
    if (std::addressof(type.cell_type()) == std::addressof(cpp_scalar_type<i64>()))
        return apply_native_column_series_literal<i64>(literal, output);
    if (std::addressof(type.cell_type()) == std::addressof(cpp_scalar_type<double>()))
        return apply_native_column_series_literal<double>(literal, output);
    throw std::invalid_argument{"TSC literal supports only i64 and double cell types"};
}

void apply_native_frame_series_literal(const literal::TimeSeriesLiteral &literal, OutputState &output) {
    if (const auto *sentinel = literal::Sentinel::from(literal)) {
        if (*sentinel == literal::SKIP)
            return;
        if (*sentinel == literal::INVALID) {
            output.invalidate();
            return;
        }
        throw std::invalid_argument{"TSF literal does not accept this sentinel"};
    }

    auto &storage = *static_cast<NativeFrameStorage *>(output.mutable_data());
    const auto &layout = storage.layout();
    const auto &schema = *layout.schema;
    const auto time = output.context().evaluation_time();
    auto &memory = output.allocator();

    const auto entries = literal.visit(
        [](const literal::ScalarLiteralItem &) -> std::span<const literal::ScalarLiteral> {
            throw std::invalid_argument{"TSF literal must contain a sequence of row changes"};
        },
        [](std::span<const literal::ScalarLiteral> values) { return values; }).value();

    if (entries.empty()) {
        if (!output.valid()) {
            storage.clear(time, memory);
            output.publish();
        }
        return;
    }

    for (const auto &entry : entries) {
        if (const auto tagged = literal::TaggedLiteral::from(entry)) {
            if (tagged->sentinel != literal::REMOVE)
                throw std::invalid_argument{"TSF row change does not accept this tagged literal"};
            if (layout.key_fields.empty())
                throw std::invalid_argument{"cannot remove a row by key from an unkeyed frame"};
            const auto fields = details::read_named_frame_fields(tagged->argument, schema, layout, true);
            const auto key = details::read_literal_key(fields, layout);
            if (const auto id = storage.find_key(key); id && storage.erase(*id, time, memory))
                output.publish();
            continue;
        }

        const auto fields = details::read_named_frame_fields(entry, schema, layout, false);
        auto cells = std::vector<FrameLiteralCell>{};
        initialize_frame_literal_cells(schema, cells);
        for (auto index = std::size_t{}; index != fields.size(); ++index) {
            if (fields[index] != nullptr)
                read_frame_literal_cell(*fields[index], *layout.fields[index].cell_type, cells[index]);
        }

        if (!layout.key_fields.empty()) {
            const auto key = read_frame_literal_key(cells, layout);
            if (const auto id = storage.find_key(key)) {
                auto patches = std::vector<details::NativeFrameCellPatch>(schema.field_count());
                for (auto index = std::size_t{}; index != fields.size(); ++index) {
                    if (fields[index] != nullptr &&
                        std::ranges::find(layout.key_fields, index) == layout.key_fields.end())
                        patches[index] = {true, frame_literal_cell_pointer(cells[index])};
                }
                if (storage.set(*id, patches, time, memory))
                    output.publish();
                continue;
            }
        }

        auto values = std::vector<const void *>{};
        values.reserve(cells.size());
        for (const auto &cell : cells)
            values.push_back(frame_literal_cell_pointer(cell));
        static_cast<void>(storage.add(values, time, memory));
        output.publish();
    }
}

} // namespace

namespace details {

void register_column_literal_type(const ScalarType &type) {
    const auto lock = std::lock_guard{literal_registration_mutex()};
    static_cast<void>(ScalarDispatch<ScalarLiteralOperations>::instance().add(
        type, ScalarLiteralOperations{&construct_column_literal}));
}

void register_frame_literal_type(const ScalarType &type) {
    const auto lock = std::lock_guard{literal_registration_mutex()};
    static_cast<void>(ScalarDispatch<ScalarLiteralOperations>::instance().add(
        type, ScalarLiteralOperations{&construct_frame_literal}));
}

void register_literal_runtime(const ScalarRuntimeType &runtime, ScalarLiteralOperations operations) {
    const auto lock = std::lock_guard{literal_registration_mutex()};
    static_cast<void>(ScalarDispatch<ScalarLiteralOperations>::instance().add(runtime, operations));
}

void apply_series_literal(const literal::TimeSeriesLiteral &literal, OutputState &output) {
    const auto *operations = TimeSeriesDispatch<TimeSeriesLiteralOperations>::instance().find_exact(output.runtime_type());
    if (operations == nullptr)
        throw std::invalid_argument{"output runtime does not support literal updates"};
    operations->apply(literal, output);
}

void register_frame_series_literal_runtime(const TimeSeriesRuntimeType &runtime) {
    const auto lock = std::lock_guard{literal_registration_mutex()};
    static_cast<void>(TimeSeriesDispatch<TimeSeriesLiteralOperations>::instance().add(
        runtime, TimeSeriesLiteralOperations{&apply_native_frame_series_literal}));
}

void register_column_series_literal_runtime(const TimeSeriesRuntimeType &runtime) {
    const auto lock = std::lock_guard{literal_registration_mutex()};
    static_cast<void>(TimeSeriesDispatch<TimeSeriesLiteralOperations>::instance().add(
        runtime, TimeSeriesLiteralOperations{&apply_native_column_series_literal}));
}

} // namespace details
} // namespace bg::frame
