#include "format.hpp"
#include "tick_codec.hpp"

#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/time_series/tick_operations.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace bg::frame::details {
namespace {

template <typename... Arguments>
void write(fmt::format_context &context, fmt::format_string<Arguments...> pattern, Arguments &&...arguments) {
    context.advance_to(fmt::format_to(context.out(), pattern, std::forward<Arguments>(arguments)...));
}

template <typename Value> void format_scalar_value(const Value &value, fmt::format_context &context) {
    if constexpr (std::same_as<Value, std::string_view>) {
        const auto owned = ScalarValue<string>{value};
        const auto scalar = Scalar<>::from(owned);
        format_scalar(scalar, context);
    } else {
        const auto typed = Scalar<Value>::from(value);
        const auto scalar = Scalar<>::from(typed);
        format_scalar(scalar, context);
    }
}

template <typename Cell> void format_cell(const std::optional<Cell> &cell, fmt::format_context &context) {
    if (!cell) {
        write(context, "NULL_VALUE");
        return;
    }
    format_scalar_value(*cell, context);
}

template <cell_type Cell> void format_column_scalar(const ColumnView<Cell> &column, fmt::format_context &context) {
    write(context, "[");
    auto first = true;
    column.batches([&](const ColumnView<Cell>::Batch &batch) {
        for (auto index = std::size_t{}; index != batch.size; ++index) {
            if (!first)
                write(context, ", ");
            first = false;
            if (!batch.is_valid(index))
                write(context, "NULL_VALUE");
            else
                format_scalar_value(batch.value_at(index), context);
        }
    });
    write(context, "]");
}

void format_column(Scalar<> value, fmt::format_context &context) {
    const auto &type = static_cast<const ColumnType &>(value.type());
    visit_cell_type(type.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
        const auto column = Scalar<Column<Cell>>::from(value);
        format_column_scalar(column.value(), context);
    });
}

using FrameColumn = std::variant<ColumnSeriesView<bool>, ColumnSeriesView<i64>, ColumnSeriesView<double>,
                                 ColumnSeriesView<string>, ColumnSeriesView<symbol>>;

FrameColumn bind_column_series(const FrameReadStorage &storage, std::size_t index) {
    const auto binding = storage.column(index);
    return visit_cell_type(*binding.cell_type, [&]<typename Cell>(std::type_identity<Cell>) {
        auto view = std::optional<ColumnSeriesView<Cell>>{};
        binding.bind_column_series_view(binding.payload, std::addressof(view));
        return FrameColumn{*view};
    });
}

void format_frame(const FrameReadStorage &storage, fmt::format_context &context) {
    write(context, "Frame(rows={}, columns={{", storage.size);
    for (auto index = std::size_t{}; index != storage.schema->field_count(); ++index) {
        if (index != 0)
            write(context, ", ");
        write(context, "{:?}: ", storage.schema->field(index).name.text());
        const auto binding = storage.column(index);
        const auto column = storage.generic_column(index);
        visit_cell_type(*binding.cell_type, [&]<typename Cell>(std::type_identity<Cell>) {
            format_column_scalar(column.view<Cell>(), context);
        });
    }
    write(context, "}})");
}

void format_frame_scalar(Scalar<> value, fmt::format_context &context) {
    const auto &runtime = static_cast<const FrameScalarRuntimeType &>(value.runtime_type());
    format_frame(runtime.read_storage(bg::details::scalar_data(value)), context);
}

template <cell_type Cell> void format_live_column_values(const In<TSC<Cell>> &input, fmt::format_context &context) {
    const auto view = ColumnSeriesView<Cell>{input.extension_descriptor().storage};
    write(context, "{{");
    auto first = true;
    for (const auto id : view.removed()) {
        if (!first)
            write(context, ", ");
        write(context, "{}: REMOVE", id);
        first = false;
    }
    for (const auto id : view.added()) {
        if (!first)
            write(context, ", ");
        write(context, "{}: ", id);
        format_cell(view.cell(id), context);
        first = false;
    }
    for (const auto id : view.modified()) {
        if (view.added().contains(id))
            continue;
        if (!first)
            write(context, ", ");
        write(context, "{}: ", id);
        format_cell(view.cell(id), context);
        first = false;
    }
    write(context, "}}");
}

void format_live_column_tick(const In<TS_T> &input, EvaluationTime, fmt::format_context &context) {
    const auto &type = static_cast<const ColumnSeriesType &>(input.runtime_type().type());
    visit_cell_type(type.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
        const auto typed_input = In<TSC<Cell>>{input.input_data()};
        const auto storage = typed_input.extension_descriptor();
        if (ColumnSeriesView<Cell>{storage.storage}.change_time() != input.last_updated()) {
            write(context, "{{}}");
            return;
        }
        format_live_column_values(typed_input, context);
    });
}

std::vector<FrameColumn> bind_frame_columns(const FrameReadStorage &storage) {
    auto columns = std::vector<FrameColumn>{};
    columns.reserve(storage.schema->field_count());
    for (auto index = std::size_t{}; index != storage.schema->field_count(); ++index)
        columns.push_back(bind_column_series(storage, index));
    return columns;
}

void format_live_frame_cell(const std::vector<FrameColumn> &columns, std::size_t index, row_id_t id,
                            fmt::format_context &context) {
    std::visit([&](const auto &column) { format_cell(column.cell(id), context); }, columns[index]);
}

void format_live_frame_tick(const In<TS_T> &input, EvaluationTime, fmt::format_context &context) {
    const auto &runtime = static_cast<const FrameSeriesRuntimeType &>(input.runtime_type());
    auto storage = runtime.input_storage(input.input_data().data());
    storage.last_updated = input.last_updated();
    storage.modified = input.modified();
    if (storage.change_time != input.last_updated()) {
        write(context, "{{}}");
        return;
    }
    const auto &layout = native_frame_layout(*storage.schema);
    const auto columns = bind_frame_columns(storage);

    write(context, "{{");
    auto first = true;
    const auto append_separator = [&] {
        if (!first)
            write(context, ", ");
        first = false;
    };
    if (storage.removed != nullptr) {
        for (const auto id : *storage.removed) {
            append_separator();
            write(context, "{}: REMOVE", id);
        }
    }
    if (storage.added != nullptr) {
        for (const auto id : *storage.added) {
            append_separator();
            write(context, "{}: {{", id);
            auto first_key = true;
            for (const auto index : layout.key_fields) {
                if (!first_key)
                    write(context, ", ");
                first_key = false;
                write(context, "{:?}: ", storage.schema->field(index).name.text());
                format_live_frame_cell(columns, index, id, context);
            }
            for (auto index = std::size_t{}; index != storage.schema->field_count(); ++index) {
                if (std::ranges::find(layout.key_fields, index) != layout.key_fields.end())
                    continue;
                if (!first_key)
                    write(context, ", ");
                first_key = false;
                write(context, "{:?}: ", storage.schema->field(index).name.text());
                format_live_frame_cell(columns, index, id, context);
            }
            write(context, "}}");
        }
    }
    if (storage.changed != nullptr) {
        for (const auto id : *storage.changed) {
            if ((storage.added != nullptr && storage.added->contains(id)) ||
                (storage.removed != nullptr && storage.removed->contains(id)))
                continue;
            append_separator();
            write(context, "{}: {{", id);
            auto first_field = true;
            for (const auto index : layout.key_fields) {
                if (!first_field)
                    write(context, ", ");
                first_field = false;
                write(context, "{:?}: ", storage.schema->field(index).name.text());
                format_live_frame_cell(columns, index, id, context);
            }
            for (auto index = std::size_t{}; index != storage.schema->field_count(); ++index) {
                if (std::ranges::find(layout.key_fields, index) != layout.key_fields.end())
                    continue;
                const auto *modified = storage.modified_at(index);
                if (modified == nullptr || !modified->contains(id))
                    continue;
                if (!first_field)
                    write(context, ", ");
                write(context, "{:?}: ", storage.schema->field(index).name.text());
                format_live_frame_cell(columns, index, id, context);
                first_field = false;
            }
            write(context, "}}");
        }
    }
    write(context, "}}");
}

void format_serialized_cell(util::buffer_reader &reader, const ScalarType &type, fmt::format_context &context) {
    if (reader.read_raw<std::uint8_t>() == 0) {
        write(context, "NULL_VALUE");
        return;
    }
    if (std::addressof(type) == std::addressof(cpp_scalar_type<bool>())) {
        format_scalar_value(reader.read_raw<bool>(), context);
    } else if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>())) {
        format_scalar_value(reader.read_raw<i64>(), context);
    } else if (std::addressof(type) == std::addressof(cpp_scalar_type<double>())) {
        format_scalar_value(reader.read_raw<double>(), context);
    } else if (std::addressof(type) == std::addressof(cpp_scalar_type<string>())) {
        const auto value = reader.peek<ScalarValue<string>>().view();
        format_scalar(Scalar<>::from(value), context);
        reader.consume<ScalarValue<string>>();
    } else if (std::addressof(type) == std::addressof(cpp_scalar_type<symbol>())) {
        format_scalar_value(reader.read_raw<symbol>(), context);
    } else {
        throw fmt::format_error{"retained frame tick contains an unsupported cell type"};
    }
}

void format_key_cell(const std::monostate &, fmt::format_context &context) { write(context, "NULL_VALUE"); }

void format_key_cell(i64 value, fmt::format_context &context) {
    format_scalar_value(value, context);
}

void format_key_cell(symbol value, fmt::format_context &context) {
    format_scalar_value(value, context);
}

void format_key_cell(const ScalarValue<string> &value, fmt::format_context &context) {
    const auto view = value.view();
    format_scalar(Scalar<>::from(view), context);
}

void format_decoded_cell(const std::monostate &, fmt::format_context &context) { write(context, "NULL_VALUE"); }

template <typename Cell> void format_decoded_cell(const std::optional<Cell> &value,
                                                  fmt::format_context &context) {
    format_cell(value, context);
}

void format_serialized_column(const TimeSeriesType &type, util::buffer_reader &reader,
                              fmt::format_context &context) {
    static_cast<void>(reader.read_raw<std::uintptr_t>());
    const auto &cell_type = static_cast<const ColumnSeriesType &>(type).cell_type();
    write(context, "{{");
    auto first = true;
    for (;;) {
        const auto header = bg::details::TickHeader::read(reader);
        if (header.operation == bg::details::TickOperation::end)
            break;
        const auto id = reader.read_raw<row_id_t>();
        if (!first)
            write(context, ", ");
        first = false;
        write(context, "{}: ", id);
        if (header.operation == bg::details::TickOperation::remove)
            write(context, "REMOVE");
        else if (header.operation == bg::details::TickOperation::add ||
                 header.operation == bg::details::TickOperation::tick)
            format_serialized_cell(reader, cell_type, context);
        else
            throw fmt::format_error{"invalid TSC tick operation"};
    }
    write(context, "}}");
}

void format_serialized_frame(const TimeSeriesType &, util::buffer_reader &reader,
                             fmt::format_context &context) {
    const auto *schema = reader.read_raw<const bg::Schema *>();
    static_cast<void>(reader.read_raw<std::uintptr_t>());
    tick_codec::skip_changed_columns(reader);
    const auto &layout = native_frame_layout(*schema);
    write(context, "{{");
    auto first = true;
    for (;;) {
        const auto header = bg::details::TickHeader::read(reader);
        if (header.operation == bg::details::TickOperation::end)
            break;
        auto change = tick_codec::read_change(reader, *schema, layout, header);
        if (!first)
            write(context, ", ");
        first = false;
        write(context, "{}: ", change.id);
        if (change.operation == bg::details::TickOperation::remove) {
            write(context, "REMOVE");
            continue;
        }
        if (change.operation != bg::details::TickOperation::add &&
            change.operation != bg::details::TickOperation::tick)
            throw fmt::format_error{"invalid TSF tick operation"};

        write(context, "{{");
        auto first_cell = true;
        const auto format_key_at = [&](std::size_t key_position) {
            if (!first_cell)
                write(context, ", ");
            first_cell = false;
            const auto index = layout.key_fields[key_position];
            write(context, "{:?}: ", schema->field(index).name.text());
            std::visit([&](const auto &value) { format_key_cell(value, context); }, change.key[key_position]);
        };
        const auto format_cell_at = [&](std::size_t index) {
            if (!first_cell)
                write(context, ", ");
            first_cell = false;
            write(context, "{:?}: ", schema->field(index).name.text());
            std::visit([&](const auto &value) { format_decoded_cell(value, context); }, change.cells[index]);
        };
        for (auto key_position = std::size_t{}; key_position != layout.key_fields.size(); ++key_position)
            format_key_at(key_position);
        if (change.operation == bg::details::TickOperation::add) {
            for (auto index = std::size_t{}; index != schema->field_count(); ++index) {
                if (std::ranges::find(layout.key_fields, index) == layout.key_fields.end())
                    format_cell_at(index);
            }
        } else {
            for (auto index = std::size_t{}; index != schema->field_count(); ++index) {
                if (std::ranges::find(layout.key_fields, index) != layout.key_fields.end() ||
                    std::holds_alternative<std::monostate>(change.cells[index]))
                    continue;
                format_cell_at(index);
            }
        }
        write(context, "}}");
    }
    write(context, "}}");
}

} // namespace

void register_column_format_type(const ScalarType &type) {
    static_cast<void>(ScalarDispatch<ScalarFormatOperations>::instance().add(
        type, ScalarFormatOperations{&format_column}));
}

void register_frame_format_type(const ScalarType &type) {
    static_cast<void>(ScalarDispatch<ScalarFormatOperations>::instance().add(
        type, ScalarFormatOperations{&format_frame_scalar}));
}

void register_column_tick_format_type(const TimeSeriesType &type) {
    static_cast<void>(TimeSeriesDispatch<TimeSeriesTickFormatOperations>::instance().add(
        type, TimeSeriesTickFormatOperations{.live = &format_live_column_tick,
                                             .serialized = &format_serialized_column}));
}

void register_frame_tick_format_type(const TimeSeriesType &type) {
    static_cast<void>(TimeSeriesDispatch<TimeSeriesTickFormatOperations>::instance().add(
        type, TimeSeriesTickFormatOperations{.live = &format_live_frame_tick,
                                             .serialized = &format_serialized_frame}));
}

} // namespace bg::frame::details
